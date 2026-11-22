// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <psp2/display.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>

#include "citra_vita/crash_report.h"
#include "citra_vita/i18n.h"
#include "citra_vita/sgxd_present.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "common/named_thread.h"
#include "common/settings.h"
#include "common/thread.h"
#include "sgxd.h"
#include "sgxd_mkq.h"
#include "sgxd_program.h"
#include "sgxd_ptla.h"
#include "sgxd_recipe.h"
#include "sgxd_session.h"
#include "video_core/renderer_sgxd/sgxd_flags.h"
#include "video_core/renderer_sgxd/sgxd_backend.h"

namespace VitaFrontend::SgxdPresent {

namespace {

// GPU addresses (docs/sgxd/memory.md; sgxd_test's): the recipe's block at 16 MiB,
// the display buffers at 1 GiB, the texture at 60 MiB.
constexpr u32 RecipeVa = 0x01000000;
constexpr u32 TextureVa = 0x03c00000;
// The parameter buffer every recipe shares (sgxd_recipe_pb), Sony's model: its page table at 32
// MiB, the pages 256 KiB on. Page numbers count from the DPM requestor base, this block's address,
// so every recipe block lies within 256 MiB of it. A TA that runs out of pages while scenes wait
// for their 3D stops the GPU: those 3Ds wait for it (docs/sgxd/ta.md). So the buffer is large
// enough that no pass comes near: 7104 pages, the TA context stalls at 6814 (ZLS_PAGE_THRESHOLD), a
// TA started beside binned scenes is sure of 3000 (SGXD_RECIPE_PASS_PAGES), and the scenes waiting
// for their 3D may hold 3814 when the next TA starts. The first 1536 pages are CDRAM, the rest main
// memory mapped after them (PbExtVa), up to the texture. A 3D gives its pages back at the free
// list's tail, so once the list has turned over the TA takes every page in turn.
constexpr u32 PbVa = 0x02000000;
constexpr u32 PbCdramPages = 1536;
constexpr u32 PbExtPages = 5568;
constexpr u32 PbPages = PbCdramPages + PbExtPages;
constexpr u32 PbSize = 0x40000 + PbCdramPages * 0x1000;
constexpr u32 PbExtVa = PbVa + PbSize;
constexpr u32 PbExtSize = PbExtPages * 0x1000;
constexpr u32 DisplayVa = 0x40000000;
constexpr u32 DisplayBuffers = 3;
constexpr u32 DisplayBufferSize = 0x200000; // 960 * 544 * 4 rounded up to CDRAM's 256 KiB
// Parameter pages: 1.6 MiB (2000 draws took 84 a pass).
constexpr u32 RecipePages = 418;

// The ImGui pass's capacities. A frame of the menus is a few thousand vertices and a few
// hundred draws; the indices are 16-bit, so no frame can address more than 64 Ki vertices.
constexpr u32 MaxVertices = 0x8000;
constexpr u32 MaxIndices = SGXD_RECIPE_MAX_INDICES;
constexpr u32 MaxDraws = 1024;

// In the page after the layout (sgxd_test's kicker page): the kicker word, the background's
// colour and secondary, the vertex program's SA.
constexpr u32 KickerValue = 0x5a;
constexpr u32 BackgroundSa = 0x400;
constexpr u32 VertexSa = 0xa00;

// The guest screens' area, the pages after the kicker page: the programs (placed once, within
// DOUTU's reach of CV and CF), their index list, each screen's six vertices and vertex fetch,
// the dummy secondaries, and the pixel primaries, one per texture seen (a guest alternates two
// or three framebuffers a screen), reused in turn when the table is full.
constexpr u32 ScreenAreaSize = 0x4000;
constexpr u32 ScreenVertexCode = 0x000;
constexpr u32 ScreenFragmentCode = 0x040;
constexpr u32 ScreenIndices = 0x080;
constexpr u32 ScreenBlendCode = 0x0c0; // a translucent screen's fragment program, six words
constexpr u32 ScreenVertices = 0x100; // + 0x100 a screen
constexpr u32 ScreenVertexPds = 0x400; // + 0x80 a screen
constexpr u32 ScreenVsecPds = 0x500;
constexpr u32 ScreenFsecPds = 0x580;
constexpr u32 ScreenPixelPds = 0x800; // + 0x80 an entry
constexpr u32 ScreenPixelEntries = 32;
constexpr u32 ScreenPdsSlot = 0x80;
constexpr u32 ScreenSlots = 2;

struct State {
    bool initialized = false;
    SceUID block = -1, cdram = -1, texture = -1, black = -1, pb = -1, pb_ext = -1;
    u8* memory = nullptr;
    u8* pb_memory = nullptr;
    u8* pb_ext_memory = nullptr;
    u8* buffers = nullptr;
    u32 size = 0, texture_size = 0;
    u8* texture_base = nullptr;
    u32 texture_width = 0, texture_height = 0;
    u32 kicker_offset = 0;
    SgxdRecipe recipe{};
    SgxdRecipeDraw draws[MaxDraws]{};
    Batch batch{};
    u32 frame_index = 0;
    u32 back = 0;
    u32 clear = 0xff000000;
    bool in_frame = false;
    bool failed = false;
    // The last present's buffer, shown once its 3D is done (an async one's at the next
    // BeginFrame), and the chain count by which it is.
    bool shown_pending = false;
    u8* shown_buffer = nullptr;
    u32 shown_fence = 0;
    // Flip pacing: a drawn async present goes on screen no sooner than 7/8 of the presents'
    // running mean interval after the last flip (FlipDueIn). The last flip's and the last
    // issue's process times, us.
    SceUInt64 last_flip_us = 0;
    SceUInt64 last_issue_us = 0;
    s64 issue_interval_us = 0;
    // Each display buffer's last sceDisplaySetFrameBuf: its order (0: never set) and the vblank
    // count from which the display shows it, between from_lo and from_hi (the counts before and
    // after the call, plus one: SceDisplay's NEXTFRAME set takes effect at its vblank count + 1,
    // display.c ksceDisplaySetFrameBufInternal). The count is sceDisplayGetVcount's 16 bits.
    struct BufferSet {
        u32 order;
        u16 from_lo, from_hi;
    } sets[DisplayBuffers]{};
    u32 set_order = 0;
    u32 display_waits = 0;
    // Vsync, as libgxm's display queue: a picture whose 3D is done joins a queue (ShowLast), and
    // the display thread sets each for the next vblank and waits until the display has taken it
    // (sceDisplayWaitSetFrameBuf), one a vblank; the buffer shown before is free then. BeginFrame
    // waits for a free buffer, so with one shown and two queued the render thread waits as
    // sceGxmDisplayQueueAddEntry does: nothing is dropped and nothing polls. Each buffer's part
    // and the queue, under display_gate; `queue_mode`: the parts are the queue's (vsync on), not
    // the sets' (vsync off, the render thread's own flips).
    enum class Part : u8 { Free, Drawn, Queued, Shown };
    Part parts[DisplayBuffers]{};
    u32 queue[DisplayBuffers]{};
    u32 queued = 0;
    bool queue_mode = false;
    bool display_stop = false;
    u32 touches[4]{}; // the guest screens this frame samples, [lo, hi) GPU address pairs
    char failed_text[160]{};

    u32 screen_offset = 0;
    u32 screen_vertex = 0, screen_fragment = 0, screen_blend = 0; // DOUTU word 0s
    SgxdRecipeDrawPrograms screen_programs[ScreenSlots]{};
    u32 screens_in_frame = 0;
    struct PixelEntry {
        u32 tex[4];
        bool blend; ///< the translucent screen's fragment program
        u32 va, units;
    } pixel_entries[ScreenPixelEntries]{};
    u32 pixel_used = 0, pixel_next = 0;
};

State g;
bool dump_requested = false;
// The display thread and its gate (outside State, which Shutdown resets by assignment); the gate
// made at its first use, not before main.
Common::CoreCondition& DisplayGate() {
    static Common::CoreCondition gate;
    return gate;
}
Common::NamedThread display_thread;

template <typename... Args>
void Report(fmt::format_string<Args...> format, Args&&... args) {
    LOG_INFO(Frontend, "{}", fmt::format(format, std::forward<Args>(args)...));
}

void FreeBlocks() {
    if (g.texture >= 0) {
        sceKernelFreeMemBlock(g.texture);
    }
    if (g.cdram >= 0) {
        sceKernelFreeMemBlock(g.cdram);
    }
    if (g.block >= 0) {
        sceKernelFreeMemBlock(g.block);
    }
    if (g.pb >= 0) {
        sceKernelFreeMemBlock(g.pb);
    }
    if (g.pb_ext >= 0) {
        sceKernelFreeMemBlock(g.pb_ext);
    }
    g.texture = g.cdram = g.block = g.pb = g.pb_ext = -1;
    g.memory = g.buffers = g.pb_memory = nullptr;
    g.texture_base = nullptr;
    g.texture_width = g.texture_height = 0;
}

// The clear: a quad over the whole target that covers no pixel (DCMP never), so every tile is
// in the scene and the background object fills it (a tile nothing touches keeps its bytes).
void PutClear(Batch& b) {
    const float xs[2] = {0.0f, static_cast<float>(DisplayWidth)};
    const float ys[2] = {0.0f, static_cast<float>(DisplayHeight)};
    for (u32 v = 0; v < 4; v++) {
        const float words[4] = {xs[v & 1], ys[v >> 1], 0.0f, 0.0f};
        std::memcpy(b.vertices + v * VertexWords, words, sizeof(words));
        b.vertices[v * VertexWords + 4] = 0;
    }
    static const u16 quad[8] = {0, 1, 2, 1, 3, 2, 0, 0};
    std::memcpy(b.indices, quad, sizeof(quad));
    b.draws[0] = SgxdRecipeDraw{0, 6, SGXD_RECIPE_ISP_NEVER, SGXD_R_FPRIM, 0, 0};
    b.vertex_count = 4;
    b.index_count = 8;
    b.draw_count = 1;
}

void WriteDisplayPpm(const char* path, const u8* px) {
    FILE* f = std::fopen(path, "wb");
    if (f == nullptr) {
        return;
    }
    std::fprintf(f, "P6\n%u %u\n255\n", DisplayWidth, DisplayHeight);
    std::vector<u8> row(DisplayWidth * 3);
    for (u32 y = 0; y < DisplayHeight; y++) {
        const u8* src = px + static_cast<std::size_t>(y) * DisplayWidth * 4;
        for (u32 x = 0; x < DisplayWidth; x++) {
            row[x * 3 + 0] = src[x * 4 + 0];
            row[x * 3 + 1] = src[x * 4 + 1];
            row[x * 3 + 2] = src[x * 4 + 2];
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
}

int ShowFrame(void* base, SceDisplaySetBufSync sync = SCE_DISPLAY_SETBUF_NEXTFRAME) {
    SceDisplayFrameBuf fb{};
    fb.size = sizeof(fb);
    fb.base = base;
    fb.pitch = DisplayWidth;
    fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    fb.width = DisplayWidth;
    fb.height = DisplayHeight;
    return sceDisplaySetFrameBuf(&fb, sync);
}

// A black frame of its own, so the display leaves the buffers before they are freed.
void ShowBlackFrame() {
    if (g.black < 0) {
        g.black = sceKernelAllocMemBlock("sgxd_present_black", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
                                         DisplayBufferSize, nullptr);
    }
    void* base = nullptr;
    if (g.black < 0 || sceKernelGetMemBlockBase(g.black, &base) < 0) {
        return;
    }
    std::memset(base, 0, DisplayBufferSize);
    ShowFrame(base);
    sceDisplayWaitSetFrameBuf();
}

bool AtOrAfter(u16 a, u16 b) {
    return static_cast<s16>(static_cast<u16>(a - b)) >= 0;
}

// A display buffer the display neither shows nor will: one never set, or one a newer set has
// taken over from (that set's from_hi reached) or takes over from before this one would show
// (its from_hi at or before this one's from_lo). The oldest such, or -1.
int FreeDisplayBuffer() {
    const u16 now = static_cast<u16>(sceDisplayGetVcount());
    int found = -1;
    for (u32 i = 0; i < DisplayBuffers; i++) {
        const State::BufferSet& s = g.sets[i];
        bool free = s.order == 0;
        for (u32 j = 0; j < DisplayBuffers && !free; j++) {
            const State::BufferSet& n = g.sets[j];
            free =
                n.order > s.order && (AtOrAfter(now, n.from_hi) || AtOrAfter(s.from_lo, n.from_hi));
        }
        if (free && (found < 0 || s.order < g.sets[found].order)) {
            found = static_cast<int>(i);
        }
    }
    return found;
}

u32 ScreenVa(u32 offset) {
    return RecipeVa + g.screen_offset + offset;
}

void PutPds(u32 offset, const SgxdPdsProgram& p) {
    std::memcpy(g.memory + g.screen_offset + offset, p.words, p.count * 4);
}

// The screens' programs, their index list, and each screen's vertex fetch and secondaries:
// everything but the pixel primaries, which follow the textures.
void WriteScreenPrograms() {
    const SgxdRecipe& r = g.recipe;
    u8* area = g.memory + g.screen_offset;
    u64 words[6];
    std::memcpy(area + ScreenVertexCode, words, sgxd_program_blit_vertex(words) * 8);
    std::memcpy(area + ScreenFragmentCode, words, sgxd_program_screen_fragment(words) * 8);
    std::memcpy(area + ScreenBlendCode, words, sgxd_program_screen_blend_fragment(words) * 8);
    g.screen_vertex = sgxd_program_doutu0(0, g.screen_offset + ScreenVertexCode -
                                                 r.offset[SGXD_R_CV], 0);
    g.screen_fragment = sgxd_program_doutu0(1, g.screen_offset + ScreenFragmentCode -
                                                   r.offset[SGXD_R_CF], 0);
    g.screen_blend = sgxd_program_doutu0(1, g.screen_offset + ScreenBlendCode -
                                                r.offset[SGXD_R_CF], 0);
    static const u16 indices[8] = {0, 1, 2, 3, 4, 5, 0, 0};
    std::memcpy(area + ScreenIndices, indices, sizeof(indices));

    SgxdPdsProgram pds{};
    sgxd_program_secondary_pds(0, 0, sgxd_program_doutu0(0, 0, 0), &pds);
    PutPds(ScreenVsecPds, pds);
    const u32 vsec_units = pds.units;
    sgxd_program_secondary_pds(0, 0, sgxd_program_doutu0(1, 0, 0), &pds);
    PutPds(ScreenFsecPds, pds);
    const u32 fsec_units = pds.units;
    constexpr u32 N = SGXD_PROGRAM_BLIT_VERTEX_WORDS;
    for (u32 slot = 0; slot < ScreenSlots; slot++) {
        SgxdRecipeDrawPrograms& p = g.screen_programs[slot];
        const SgxdVertexFetch fetch{ScreenVa(ScreenVertices + slot * 0x100), N * 4, N, 0};
        sgxd_program_vertex_pds(&fetch, 1, g.screen_vertex, 0, &pds);
        PutPds(ScreenVertexPds + slot * ScreenPdsSlot, pds);
        p.vspds = ScreenVa(ScreenVertexPds + slot * ScreenPdsSlot);
        p.vspds_units = pds.units;
        p.vsec = ScreenVa(ScreenVsecPds);
        p.vsec_units = vsec_units;
        p.vsec_sa = 0;
        p.fsec = ScreenVa(ScreenFsecPds);
        p.fsec_units = fsec_units;
        sgxd_program_vertex_dms(N, 0, 0, N, &p.vdm_w2, &p.vdm_w4, &p.vdm_w5);
        p.pixel_size = sgxd_program_pixel_dms(SGXD_PROGRAM_BLIT_PA, 0, 0);
        p.outselects = SGXD_PROGRAM_BLIT_OUTSELECTS;
        p.texsize = SGXD_PROGRAM_BLIT_TEXSIZE;
        const float viewport[6] = {DisplayWidth / 2.0f, DisplayWidth / 2.0f,
                                   DisplayHeight / 2.0f, DisplayHeight / 2.0f, 0.0f, 1.0f};
        std::memcpy(p.viewport, viewport, sizeof(viewport));
        p.indices = ScreenVa(ScreenIndices);
        p.primitive = SGXD_PROGRAM_TRIANGLES;
    }
}

// The pixel primary sampling tex (into the blending fragment program for a translucent screen),
// written the first time it is seen. A full table starts over at its oldest entry.
const State::PixelEntry& PixelEntryFor(const u32 tex[4], bool blend) {
    for (u32 i = 0; i < g.pixel_used; i++) {
        if (std::memcmp(g.pixel_entries[i].tex, tex, 16) == 0 && g.pixel_entries[i].blend == blend) {
            return g.pixel_entries[i];
        }
    }
    const u32 i = g.pixel_used < ScreenPixelEntries ? g.pixel_used++ : g.pixel_next;
    g.pixel_next = (i + 1) % ScreenPixelEntries;
    State::PixelEntry& e = g.pixel_entries[i];
    SgxdPdsProgram pds{};
    sgxd_program_screen_pixel_pds(blend ? g.screen_blend : g.screen_fragment, tex, &pds);
    PutPds(ScreenPixelPds + i * ScreenPdsSlot, pds);
    std::memcpy(e.tex, tex, 16);
    e.blend = blend;
    e.va = ScreenVa(ScreenPixelPds + i * ScreenPdsSlot);
    e.units = pds.units;
    return e;
}

// The texture unit's format for a PICA framebuffer format: the code and swizzle that read its
// bytes as the PICA stores them (SgxdBackend::Untile), and its bytes a pixel.
bool ScreenFormat(u32 pica_format, u32& code, u32& swizzle, u32& bytes) {
    switch (pica_format) {
    case 0: // RGBA8, bytes A, B, G, R: U8U8U8U8 mirrored over the texel
        code = 0x0c, swizzle = 2, bytes = 4;
        return true;
    case 1: // RGB8, bytes B, G, R: U8U8U8 mirrored
        code = 0x98, swizzle = 1, bytes = 3;
        return true;
    case 2: // RGB565, red at 15:11
        code = 0x05, swizzle = 1, bytes = 2;
        return true;
    case 3: // RGB5A1, alpha at bit 0
        code = 0x04, swizzle = 2, bytes = 2;
        return true;
    case 4: // RGBA4, red in the top nibble
        code = 0x02, swizzle = 2, bytes = 2;
        return true;
    default:
        return false;
    }
}

} // Anonymous namespace

void DisplayLoop();
void SetQueueMode(bool on);

bool Initialize(std::string* error) {
    if (g.initialized) {
        return true;
    }
    g = State{};

    g.pb = sceKernelAllocMemBlock("sgxd_parameter_buffer", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
                                  PbSize, nullptr);
    void* pb = nullptr;
    ASSERT_MSG(g.pb >= 0 && sceKernelGetMemBlockBase(g.pb, &pb) >= 0,
               "sgxd: no CDRAM for the parameter buffer ({:#010x})", static_cast<u32>(g.pb));
    g.pb_memory = static_cast<u8*>(pb);
    // The pages past CDRAM's: main memory, uncached (only the GPU touches them).
    g.pb_ext = sceKernelAllocMemBlock("sgxd_parameter_buffer_ext",
                                      SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, PbExtSize, nullptr);
    void* pb_ext = nullptr;
    ASSERT_MSG(g.pb_ext >= 0 && sceKernelGetMemBlockBase(g.pb_ext, &pb_ext) >= 0,
               "sgxd: no memory for the parameter buffer's main-memory pages ({:#010x})",
               static_cast<u32>(g.pb_ext));
    g.pb_ext_memory = static_cast<u8*>(pb_ext);
    static_assert(PbExtVa + PbExtSize <= TextureVa, "the parameter buffer runs into the texture");
    const u32 first = (PbVa + 0x40000 - RecipeVa) / 0x1000;
    sgxd_recipe_pb = SgxdRecipePb{RecipeVa, PbVa, first, first + PbPages - 1};
    sgxd_recipe_pb_write(g.pb_memory);
    sgxd_recipe_pb_memory = g.pb_memory;

    // The layout, sized for the capacities. The arrays only have to hold valid draws for
    // sgxd_recipe_write; every frame writes its own in place (Batch).
    std::vector<u32> init_vertices(MaxVertices * VertexWords, 0);
    std::vector<u16> init_indices(MaxIndices, 0);
    std::vector<SgxdRecipeDraw> init_draws(MaxDraws,
                                           SgxdRecipeDraw{0, 6, SGXD_RECIPE_ISP_NEVER, SGXD_R_FPRIM,
                                                          0, 0});
    SgxdRecipe& r = g.recipe;
    const auto init = [&](u32 sa_offset) {
        return sgxd_recipe_init_imgui(&r, DisplayWidth, DisplayHeight, RecipeVa, RecipePages,
                                      init_vertices.data(), MaxVertices, init_indices.data(),
                                      MaxIndices, init_draws.data(), MaxDraws, sa_offset,
                                      SGXD_IMGUI_MODULATE);
    };
    // The vertex SA follows the layout's end: size the layout first.
    if (init(0) < 0) {
        *error = "sgxd: the recipe does not take the display's size";
        return false;
    }
    g.kicker_offset = (r.offset[SGXD_R_END] + 0xfff) & ~0xfffu;
    if (init(g.kicker_offset + VertexSa) < 0) {
        *error = "sgxd: the recipe does not take the display's size";
        return false;
    }
    g.screen_offset = g.kicker_offset + 0x1000;
    g.size = (g.screen_offset + ScreenAreaSize + 0x3ffff) & ~0x3ffffu; // CDRAM's 256 KiB unit

    g.block = sceKernelAllocMemBlock("sgxd_present_recipe", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
                                     g.size, nullptr);
    g.cdram = sceKernelAllocMemBlock("sgxd_present_display", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
                                     DisplayBuffers * DisplayBufferSize, nullptr);
    void* base = nullptr;
    void* buffers = nullptr;
    const bool got = g.block >= 0 && g.cdram >= 0 &&
                     sceKernelGetMemBlockBase(g.block, &base) >= 0 &&
                     sceKernelGetMemBlockBase(g.cdram, &buffers) >= 0;
    ASSERT_MSG(got, "sgxd: no CDRAM for the display ({:#010x}, {:#010x})",
               static_cast<u32>(g.block), static_cast<u32>(g.cdram));
    g.memory = static_cast<u8*>(base);
    g.buffers = static_cast<u8*>(buffers);

    sgxd_recipe_write(&r, g.memory);
    // The arrays go with this function; nothing reads them again (no sgxd_recipe_write_scene).
    r.imgui_verts = nullptr;
    r.indices = nullptr;
    r.draws = nullptr;
    *reinterpret_cast<u32*>(g.memory + g.kicker_offset) = KickerValue;
    sgxd_recipe_sa_background(&r, g.memory, g.kicker_offset + BackgroundSa, g.clear);
    // clip = pixel * 2 / size - 1, the recipe's viewport undoing it (ImGui's projection).
    const float sa[4] = {2.0f / DisplayWidth, 2.0f / DisplayHeight, -1.0f, -1.0f};
    sgxd_recipe_write_vertex_sa(&r, g.memory, sa);
    std::memset(g.buffers, 0, DisplayBuffers * DisplayBufferSize);
    WriteScreenPrograms();

    SgxdInfo info{};
    info.size = sizeof(info);
    const int taken = sgxdTake(&info);
    Report("sgxd take {:#x}: step {} status {:#010x} core id {:#010x}, {} us",
           static_cast<u32>(taken), info.step, static_cast<u32>(info.step_status), info.core_id,
           info.take_us);
    CrashReport::NoteGpuDriver(info.module_nid);
    if (taken == SGXD_ERR_FIRMWARE) {
        // sgxd knows the GPU drivers of firmware 3.60, 3.65, 3.68 and 3.73. The firmware's version
        // is no check: HENkaku spoofs it (3.70) for every caller, the kernel's included.
        *error = I18n::Tr("Azahar needs firmware 3.60, 3.65, 3.68 or 3.73.");
        FreeBlocks();
        return false;
    }
    if (taken < 0) {
        *error = fmt::format("sgxd: the GPU could not be taken ({:#010x} at step {})",
                             static_cast<u32>(taken), info.step);
        FreeBlocks();
        return false;
    }
    const int mapped = sgxdMap(RecipeVa, g.memory, g.size, 0);
    const int mapped_fb = sgxdMap(DisplayVa, g.buffers, DisplayBuffers * DisplayBufferSize, 0);
    const int mapped_pb = g.pb >= 0 ? sgxdMap(PbVa, g.pb_memory, PbSize, 0) : 0;
    ASSERT_MSG(mapped_pb == 0, "sgxd: the parameter buffer not mapped at GPU {:#010x}: {:#010x}",
               PbVa, static_cast<u32>(mapped_pb));
    const int mapped_pb_ext = g.pb_ext >= 0 ? sgxdMap(PbExtVa, g.pb_ext_memory, PbExtSize, 0) : 0;
    ASSERT_MSG(mapped_pb_ext == 0,
               "sgxd: the parameter buffer's main-memory pages not mapped at GPU {:#010x}: "
               "{:#010x}",
               PbExtVa, static_cast<u32>(mapped_pb_ext));
#ifdef VITA_DIAGNOSTICS
    // A fault's post-mortem: the present's block (its region arrays, DPMSLOT) and the parameter
    // buffer (the page table, the pages).
    SgxdRenderer::SgxdBackend::fault_hook = [] {
        if (std::FILE* f = std::fopen("host0:azahar-sgxd-present.bin", "wb")) {
            std::fwrite(g.memory, 1, g.size, f);
            std::fclose(f);
        }
        if (g.pb_memory) {
            if (std::FILE* f = std::fopen("host0:azahar-sgxd-pb.bin", "wb")) {
                std::fwrite(g.pb_memory, 1, PbSize, f);
                std::fclose(f);
            }
        }
    };
#endif
    char failed[160] = "";
    const int init_ok = mapped == 0 && mapped_fb == 0
                            ? sgxd_session_init(&r, RecipeVa + g.kicker_offset, failed,
                                                sizeof(failed))
                            : -1;
    if (init_ok == 0) {
        sgxd_session_set_reinit(&g.recipe, RecipeVa + g.kicker_offset);
    }
    if (init_ok < 0) {
        *error = fmt::format("sgxd: map {:#010x}, {:#010x}; init {}", static_cast<u32>(mapped),
                             static_cast<u32>(mapped_fb), failed);
        if (mapped_fb == 0) {
            sgxdUnmap(DisplayVa, DisplayBuffers * DisplayBufferSize);
        }
        if (mapped == 0) {
            sgxdUnmap(RecipeVa, g.size);
        }
        if (g.pb >= 0) {
            sgxdUnmap(PbVa, PbSize);
        }
        if (g.pb_ext >= 0) {
            sgxdUnmap(PbExtVa, PbExtSize);
        }
        sgxdRelease();
        FreeBlocks();
        return false;
    }
#ifdef VITA_DIAGNOSTICS
    Report("sgxd: {} KiB at {:#010x}, {} display buffers at {:#010x}", g.size / 1024, RecipeVa,
           DisplayBuffers, DisplayVa);
    if (g.pb >= 0) {
        Report("sgxd: one parameter buffer, pages {:#x}-{:#x} at {:#010x} ({} in CDRAM, {} in main "
               "memory at {:#010x}), requestor base {:#010x}",
               sgxd_recipe_pb.first, sgxd_recipe_pb.last, PbVa + 0x40000, PbCdramPages, PbExtPages,
               PbExtVa, RecipeVa);
    }
#endif
    g.initialized = true;
    display_thread = Common::NamedThread{Common::ThreadCfg{"SgxdDisplay"}, [] { DisplayLoop(); }};
    return true;
}

bool IsInitialized() {
    return g.initialized;
}

void Suspend() {
    if (!g.initialized) {
        return;
    }
    // A standby waits 3 s for this (WatchStandby); past that the module's own suspend (sgxd.c's
    // sysevent) takes the GPU down, and this runs after it, after the resume even. Nothing to
    // hand back then; the drain would never come, and the take after the resume is all.
    if (sgxdOwned() <= 0) {
        Report("sgxd: suspend: the system's suspend took the GPU already");
        return;
    }
    sgxd_mkq_park(1);
    // A job of the queue's finishes in milliseconds: three seconds at most here.
    const SceUInt64 t0 = sceKernelGetProcessTimeWide();
    while (!sgxd_mkq_drained() && sceKernelGetProcessTimeWide() - t0 < 3000000) {
        sceKernelDelayThread(1000);
    }
    const bool drained = sgxd_mkq_drained();
    Report("sgxd: suspend: the queue {} after {} us", drained ? "drained" : "not drained",
           sceKernelGetProcessTimeWide() - t0);
    if (!drained && sgxdOwned() <= 0) {
        Report("sgxd: suspend: the system's suspend took the GPU during the drain");
        return;
    }
    ASSERT_MSG(drained, "sgxd: the GPU's queue did not drain for the suspend");
    // The GPU the system's until the take after the resume: a retail console turns its display
    // off after a while without input with no standby after it, and its lock screen draws with
    // the GPU.
    const int given = sgxdSuspend();
    if (given == SGXD_ERR_LOST) {
        Report("sgxd: suspend: the system's suspend took the GPU before the hand-back");
        return;
    }
    ASSERT_MSG(given == 0, "sgxd: the GPU was not given back for the suspend ({:#010x})",
               static_cast<u32>(given));
}

bool Owned() {
    return g.initialized && sgxdOwned() > 0;
}

void WatchStandby(int flag, u32 bits) {
    const int watched = sgxdStandby(flag, bits);
    ASSERT_MSG(watched == 0, "sgxd: the standby's flag was not registered ({:#010x})",
               static_cast<u32>(watched));
}

bool Resume(std::string* error) {
    SgxdInfo info{};
    info.size = sizeof(info);
    const int taken = sgxdTake(&info);
    Report("sgxd take again {:#x}: step {} status {:#010x}, {} us", static_cast<u32>(taken),
           info.step, static_cast<u32>(info.step_status), info.take_us);
    if (taken < 0) {
        *error = fmt::format("the GPU was not taken again ({:#010x} at step {})",
                             static_cast<u32>(taken), info.step);
        return false;
    }
    // The power-off emptied the DPM, and the page list it keeps in memory is as it left it: the
    // list written afresh, every page free (the GPU was idle at the suspend), and the next TA
    // and 3D load it again (their lists' keys change with it, so no kept list skips the load).
    sgxd_recipe_pb_write(g.pb_memory);
    char failed[160] = "";
    if (sgxd_session_resume(&g.recipe, RecipeVa + g.kicker_offset, failed, sizeof(failed)) < 0 ||
        sgxd_ptla_resume(failed, sizeof(failed)) < 0) {
        *error = failed;
        return false;
    }
    const int resumed = sgxdResumed();
    sgxd_mkq_park(0);
    if (resumed < 0) {
        *error = fmt::format("sgxdResumed {:#010x}", static_cast<u32>(resumed));
        return false;
    }
    Report("sgxd: resumed");
    return true;
}

void ShowLast();

void Shutdown() {
    if (!g.initialized) {
        return;
    }
    SetQueueMode(false);
    {
        std::scoped_lock lk{DisplayGate()};
        g.display_stop = true;
        DisplayGate().NotifyAll();
    }
    display_thread.join();
    ShowLast();
    ShowBlackFrame();
    if (g.texture_size) {
        sgxdUnmap(TextureVa, g.texture_size);
    }
    const int unmapped_fb = sgxdUnmap(DisplayVa, DisplayBuffers * DisplayBufferSize);
    const int unmapped = sgxdUnmap(RecipeVa, g.size);
    if (g.pb >= 0) {
        sgxdUnmap(PbVa, PbSize);
    }
    if (g.pb_ext >= 0) {
        sgxdUnmap(PbExtVa, PbExtSize);
    }
    const int released = sgxdRelease();
    Report("sgxd: unmap {:#x}, {:#x}; release {:#x} after {} frames", static_cast<u32>(unmapped),
           static_cast<u32>(unmapped_fb), static_cast<u32>(released), g.frame_index);
    FreeBlocks();
    const SceUID black = g.black;
    g = State{};
    g.black = black;   // still on screen; the process's exit frees it
}

// Presents run as async lists (sgxd_session_render_overlap) while the rasterizer's renders do:
// once sgxd's interrupt page is set up (the backend's).
bool Async() {
    return sgxd_ptla_page() != nullptr;
}

bool Busy() {
    if (!g.initialized || !Async() || !g.shown_pending || sgxd_chain_reached(g.shown_fence)) {
        return false;
    }
    // Its 3D finishes in the list after it: with the queue run down to it, that list goes now.
    char failed[200] = "";
    if (sgxd_session_pending() == &g.recipe && sgxd_chain_reached(sgxd_chain_fence()) &&
        sgxd_session_finish_pending(failed, sizeof(failed)) < 0) {
        ASSERT_MSG(false, "sgxd: the present's 3D did not finish: {}", failed);
    }
    if (sgxd_chain_faulted()) {
        sgxd_session_render_async_failed(failed, sizeof(failed));
        ASSERT_MSG(false, "sgxd: the async present stopped: {}", failed);
    }
    return !sgxd_chain_reached(g.shown_fence);
}

bool FlipPending(u32* fence) {
    if (!g.shown_pending || !Async() || sgxd_chain_reached(g.shown_fence)) {
        return false;
    }
    *fence = g.shown_fence;
    return true;
}

s64 FlipDueIn() {
    if (!g.shown_pending || !Async() || !sgxd_chain_reached(g.shown_fence)) {
        return 0;
    }
    if (g.queue_mode) {
        // Queued as soon as its 3D is done: the display thread keeps the vblanks.
        return 0;
    }
    const s64 due = static_cast<s64>(g.last_flip_us) + g.issue_interval_us * 7 / 8;
    const s64 now = static_cast<s64>(sceKernelGetProcessTimeWide());
    return due > now ? due - now : 0;
}

// On screen once its 3D is done, not at the next present's BeginFrame, which in a GPU-bound scene
// comes tens of ms later. Paced (FlipDueIn): presents drawn 30 and 10 ms apart go on screen about
// 20 ms apart.
void FlipDone() {
    if (g.shown_pending && Async() && sgxd_chain_reached(g.shown_fence) && FlipDueIn() == 0) {
        ShowLast();
    }
}

// The last async present's 3D, waited for: before the recipe block or the texture is written
// again (its 3D reads them). Asleep until the GPU's interrupt (the backend's wait), not spinning.
void FinishLast() {
    if (sgxd_chain_reached(g.shown_fence)) {
        return;
    }
    char failed[200] = "";
    // No list after it has finished its 3D: one does now.
    if (sgxd_session_pending() == &g.recipe &&
        sgxd_session_finish_pending(failed, sizeof(failed)) < 0) {
        ASSERT_MSG(false, "sgxd: the present's 3D did not finish: {}", failed);
    }
    SgxdRenderer::SgxdWaitChain(g.shown_fence);
}

void WaitDrawn() {
    if (g.initialized && g.shown_pending && Async()) {
        FinishLast();
    }
}

void SessionEnded() {
    // The session's lists are done (its backend's end waited for the GPU) and its events page
    // is gone, so every fence reads as reached: the flip goes at once.
    ShowLast();
}

// The last async present's 3D completes, then its buffer goes on screen.
void ShowLast() {
    if (!g.shown_pending) {
        return;
    }
    g.shown_pending = false;
    SgxdRenderer::SgxdTrace("flip", sgxd_chain_reached(g.shown_fence) ? 1u : 0u);
    FinishLast();
    u8* const buffer = g.shown_buffer;
    static u32 dump_index = 0;
    if (dump_requested) {
        dump_requested = false;
        SgxdRenderer::SgxdCacheRange(buffer, DisplayWidth * DisplayHeight * 4, SGXD_CACHE_INVALIDATE,
                                    SgxdRenderer::CacheSite::Other);
        char path[64];
        std::snprintf(path, sizeof(path), "ux0:data/azahar/frame_%02u.ppm", dump_index++ % 20);
        WriteDisplayPpm(path, buffer);
        LOG_INFO(Frontend, "wrote {}", path);
    }
    if (g.queue_mode) {
        // Vsync: to the display thread, in order. Three buffers: at most the one before waits.
        std::scoped_lock lk{DisplayGate()};
        const u32 index = static_cast<u32>((buffer - g.buffers) / DisplayBufferSize);
        ASSERT_MSG(g.queued < DisplayBuffers - 1, "sgxd: the display queue is full");
        g.parts[index] = State::Part::Queued;
        g.queue[g.queued++] = index;
        DisplayGate().NotifyAll();
        return;
    }
    // Vsync off: the display switches to the buffer at once, within the picture it scans out (it
    // tears), and every older buffer is free. SceDisplay takes an immediate set only once a set
    // has given the head its pitch, format and size (0x80290006 otherwise,
    // ksceDisplaySetFrameBufInternal): the first is NEXTFRAME. Sets before the queue runs
    // (presents before sgxd's interrupt page) take effect at the vblank after.
    const bool next = g.set_order == 0 || Settings::values.use_vsync.GetValue();
    const u16 before = static_cast<u16>(sceDisplayGetVcount());
    const int shown =
        ShowFrame(buffer, next ? SCE_DISPLAY_SETBUF_NEXTFRAME : SCE_DISPLAY_SETBUF_IMMEDIATE);
    const u16 after = static_cast<u16>(sceDisplayGetVcount());
    SgxdRenderer::SgxdTrace("flip_vcount", before);
    ASSERT_MSG(shown >= 0, "sgxd: sceDisplaySetFrameBuf {:#010x}", static_cast<u32>(shown));
    g.last_flip_us = sceKernelGetProcessTimeWide();
    State::BufferSet& set = g.sets[(buffer - g.buffers) / DisplayBufferSize];
    set.order = ++g.set_order;
    set.from_lo = next ? static_cast<u16>(before + 1) : before;
    set.from_hi = next ? static_cast<u16>(after + 1) : before;
}

bool SetTexture(const void* texels, u32 width, u32 height, bool bilinear) {
    if (!g.initialized || g.texture >= 0 || width == 0 || height == 0 || width > 4096 ||
        height > 4096 || (width & 7)) {
        return false;
    }
    const u32 bytes = width * height * 4;
    // Main memory, uncached: the menus' atlas with the Chinese glyphs (MenuGlyphs) is 1-2 MiB,
    // which CDRAM, a game's to fill, does not spare; a frame reads only the glyphs it draws.
    const u32 size = (bytes + 0xfff) & ~0xfffu;
    // Its GPU addresses end where the renderer's blocks start (64 MiB).
    ASSERT_MSG(size <= 0x400000, "sgxd: a {} KiB texture runs past its 4 MiB of GPU addresses",
               size >> 10);
    g.texture = sceKernelAllocMemBlock("sgxd_present_texture",
                                       SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, size, nullptr);
    void* base = nullptr;
    const bool got = g.texture >= 0 && sceKernelGetMemBlockBase(g.texture, &base) >= 0;
    ASSERT_MSG(got, "sgxd: no {} KiB of main memory for a {}x{} texture ({:#010x})", size >> 10,
               width, height, static_cast<u32>(g.texture));
    std::memcpy(base, texels, bytes);
    const int mapped = sgxdMap(TextureVa, base, size, 0);
    if (mapped != 0) {
        Report("sgxd: texture map {:#010x}", static_cast<u32>(mapped));
        sceKernelFreeMemBlock(g.texture);
        g.texture = -1;
        return false;
    }
    g.texture_size = size;
    g.texture_base = static_cast<u8*>(base);
    g.texture_width = width;
    g.texture_height = height;
    ShowLast();
    u32 words[4];
    sgxd_recipe_texture_words(TextureVa, width, height, bilinear ? 1 : 0, words);
    sgxd_recipe_set_texture(&g.recipe, g.memory, words);
    return true;
}

bool UpdateTexture(u32 x, u32 y, u32 width, u32 height, const u32* texels) {
    if (!g.initialized || g.texture_base == nullptr || x + width > g.texture_width ||
        y + height > g.texture_height) {
        return false;
    }
    // Memory the CPU writes uncached and the texture unit reads through its cache, which every
    // kick invalidates.
    for (u32 row = 0; row < height; ++row) {
        std::memcpy(g.texture_base + ((y + row) * g.texture_width + x) * 4, texels + row * width,
                    width * 4);
    }
    return true;
}

// Vsync off, or before the queue runs: a buffer no set shows or will (FreeDisplayBuffer).
void BeginFrameSets() {
    int back = FreeDisplayBuffer();
    if (back < 0) {
        // A vblank fell within a set and leaves which buffer shows open: the newest set shows
        // once this wait returns, every older one is free.
        g.display_waits++;
        u32 newest = 0;
        for (u32 i = 1; i < DisplayBuffers; i++) {
            newest = g.sets[i].order > g.sets[newest].order ? i : newest;
        }
        sceDisplayWaitSetFrameBuf();
        g.sets[newest].from_hi = static_cast<u16>(sceDisplayGetVcount());
        back = FreeDisplayBuffer();
        ASSERT_MSG(back >= 0, "sgxd: no display buffer free after the display took the newest");
    }
    g.back = static_cast<u32>(back);
}

// Into or out of the display queue (vsync turned on or off, or the first async present).
// Into: the last set shows once the display has taken it, the others are free. Out of: the queue
// runs dry first, and the buffer shown then is the newest set.
void SetQueueMode(bool on) {
    if (on == g.queue_mode) {
        return;
    }
    ShowLast();
    if (on) {
        if (g.set_order != 0) {
            sceDisplayWaitSetFrameBuf();
        }
        std::scoped_lock lk{DisplayGate()};
        u32 newest = 0;
        for (u32 i = 1; i < DisplayBuffers; i++) {
            newest = g.sets[i].order > g.sets[newest].order ? i : newest;
        }
        for (u32 i = 0; i < DisplayBuffers; i++) {
            g.parts[i] = g.set_order != 0 && i == newest ? State::Part::Shown : State::Part::Free;
        }
        g.queued = 0;
        g.queue_mode = true;
        return;
    }
    std::scoped_lock lk{DisplayGate()};
    while (g.queued != 0) {
        DisplayGate().Wait();
    }
    const u16 now = static_cast<u16>(sceDisplayGetVcount());
    for (u32 i = 0; i < DisplayBuffers; i++) {
        g.sets[i] = {};
        if (g.parts[i] == State::Part::Shown) {
            g.sets[i] = {++g.set_order, now, now};
        }
    }
    g.queue_mode = false;
}

// The display thread (vsync): each queued picture set for the next vblank, then waited for until
// the display has taken it; the picture shown before is free from then on.
void DisplayLoop() {
    for (;;) {
        u32 index;
        {
            std::scoped_lock lk{DisplayGate()};
            while (g.queued == 0 && !g.display_stop) {
                DisplayGate().Wait();
            }
            if (g.display_stop) {
                return;
            }
            index = g.queue[0];
        }
        u8* const buffer = g.buffers + static_cast<std::size_t>(index) * DisplayBufferSize;
        SgxdRenderer::SgxdTrace("flip_vcount", static_cast<u16>(sceDisplayGetVcount()));
        const int shown = ShowFrame(buffer, SCE_DISPLAY_SETBUF_NEXTFRAME);
        ASSERT_MSG(shown >= 0, "sgxd: sceDisplaySetFrameBuf {:#010x}", static_cast<u32>(shown));
        sceDisplayWaitSetFrameBuf();
        std::scoped_lock lk{DisplayGate()};
        for (State::Part& part : g.parts) {
            part = part == State::Part::Shown ? State::Part::Free : part;
        }
        g.parts[index] = State::Part::Shown;
        g.queued--;
        for (u32 i = 0; i < g.queued; i++) {
            g.queue[i] = g.queue[i + 1];
        }
        g.set_order++;
        DisplayGate().NotifyAll();
    }
}

void BeginFrame(u8 clear_r, u8 clear_g, u8 clear_b) {
    if (!g.initialized) {
        return;
    }
    SetQueueMode(Async() && Settings::values.use_vsync.GetValue());
    ShowLast();
    if (g.queue_mode) {
        std::scoped_lock lk{DisplayGate()};
        int free = -1;
        for (;;) {
            for (u32 i = 0; i < DisplayBuffers && free < 0; i++) {
                free = g.parts[i] == State::Part::Free ? static_cast<int>(i) : -1;
            }
            if (free >= 0) {
                break;
            }
            // One shown, two queued: the display thread frees one at the next vblank.
            g.display_waits++;
            SgxdRenderer::SgxdTrace("display_wait");
            DisplayGate().Wait();
        }
        g.parts[free] = State::Part::Drawn;
        g.back = static_cast<u32>(free);
    } else {
        BeginFrameSets();
    }
    std::memset(g.touches, 0, sizeof(g.touches));
    g.clear = 0xff000000u | static_cast<u32>(clear_b) << 16 | static_cast<u32>(clear_g) << 8 |
              clear_r;
    Batch& b = g.batch;
    b.vertices = reinterpret_cast<u32*>(g.memory + g.recipe.offset[SGXD_R_VERTS]);
    b.indices = reinterpret_cast<u16*>(g.memory + g.recipe.offset[SGXD_R_INDICES]);
    b.draws = g.draws;
    b.max_vertices = MaxVertices;
    b.max_indices = MaxIndices;
    b.max_draws = MaxDraws;
    PutClear(b);
    g.screens_in_frame = 0;
    g.in_frame = true;
}

Batch& CurrentBatch() {
    return g.batch;
}

namespace {

/// The quad's two triangles (top-left, top-right, bottom-left; top-right, bottom-right,
/// bottom-left) into the slot's vertices: clip space x and y, z, w, a white colour (unused) with
/// the opacity as its alpha, then each corner's u and v (by corner: top-left, top-right,
/// bottom-left, bottom-right).
void PutScreenVertices(u32 slot, const ScreenQuad& quad, float opacity, const float (&uv)[4][2]) {
    constexpr u32 N = SGXD_PROGRAM_BLIT_VERTEX_WORDS;
    constexpr u32 Triangles[6] = {0, 1, 2, 1, 3, 2};
    // The corners into clip space, the four at once: 2 x / width - 1, 2 y / height - 1.
    float clip_x[4], clip_y[4];
    vst1q_f32(clip_x, vmlaq_n_f32(vdupq_n_f32(-1.0f), vld1q_f32(quad.x), 2.0f / DisplayWidth));
    vst1q_f32(clip_y, vmlaq_n_f32(vdupq_n_f32(-1.0f), vld1q_f32(quad.y), 2.0f / DisplayHeight));
    float* v = reinterpret_cast<float*>(g.memory + g.screen_offset + ScreenVertices + slot * 0x100);
    for (u32 n = 0; n < 6; n++) {
        const u32 c = Triangles[n];
        const float words[N] = {clip_x[c],
                                clip_y[c],
                                0.5f,
                                1.0f,
                                1.0f,
                                1.0f,
                                1.0f,
                                opacity,
                                uv[c][0],
                                uv[c][1]};
        std::memcpy(v + n * N, words, sizeof(words));
    }
}

std::pair<float, float> QuadSize(const ScreenQuad& quad) {
    return {std::hypot(quad.x[1] - quad.x[0], quad.y[1] - quad.y[0]),
            std::hypot(quad.x[2] - quad.x[0], quad.y[2] - quad.y[0])};
}

} // namespace

bool DrawGuestScreen(u32 va, const u8* cpu, u32 stride, u32 columns, u32 pica_format,
                     const ScreenQuad& quad, float opacity) {
    u32 code, swizzle, bytes;
    Batch& b = g.batch;
    const auto [w, h] = QuadSize(quad);
    // A linear texture's rows are its width rounded up to 8 texels: the framebuffer's stride is the
    // texture's width, 240 texels or more, and u covers the first 240.
    if (!g.in_frame || g.screens_in_frame >= ScreenSlots || b.draw_count >= b.max_draws ||
        !ScreenFormat(pica_format, code, swizzle, bytes) || stride % bytes != 0 ||
        stride / bytes < 240 || (stride / bytes) % 8 != 0 || w < 1.0f || h < 1.0f ||
        columns == 0) {
        return false;
    }
    const u32 texels = stride / bytes;
    const float u_end = 240.0f / static_cast<float>(texels);
    const u32 slot = g.screens_in_frame++;
    g.touches[slot * 2] = va;
    g.touches[slot * 2 + 1] = va + stride * columns;
    // A picture the CPU drew is in memory before the GPU reads it.
    if (cpu != nullptr) {
        SgxdRenderer::SgxdCacheRange(cpu, stride * columns, SGXD_CACHE_CLEAN,
                                    SgxdRenderer::CacheSite::Screens);
    } else {
        SgxdRenderer::SgxdCacheSkipped(SgxdRenderer::CacheSite::Screens, stride * columns);
    }

    // Texel (u, v): u along a column, 240 pixels from the bottom of the upright picture to its
    // top; v across the columns, left to right. So the picture's x follows v and its y follows u
    // reversed.
    const float uv[4][2] = {{u_end, 0.0f}, {u_end, 1.0f}, {0.0f, 0.0f}, {0.0f, 1.0f}};
    PutScreenVertices(slot, quad, opacity, uv);

    // Point sampled: each display pixel shows the framebuffer pixel under its centre. A
    // translucent screen (the overlay, drawn smaller than the 3DS's) is filtered, and reads its
    // alpha as one (RGBA as RGB1: the LCD shows no alpha).
    const bool blend = opacity < 1.0f;
    u32 tex[4];
    sgxd_recipe_texture_words_format(va, texels, columns, blend ? 1 : 0, code,
                                     blend && swizzle == 2 ? 6 : swizzle, tex);
    const State::PixelEntry& e = PixelEntryFor(tex, blend);
    SgxdRecipeDrawPrograms& p = g.screen_programs[slot];
    p.fprim = e.va;
    p.fprim_units = e.units;
    // Opaque, DCMP ALWAYS, no depth write (the blit's ISP word); translucent to blend.
    const u32 isp = (blend ? 1u << 25 : 0u) | 7u << 22 | 1u << 20 | 1u << 9;
    b.draws[b.draw_count++] = SgxdRecipeDraw{0, 6, isp, SGXD_R_FPRIM, 0, 0, &p};
    return true;
}

bool DrawPictureScreen(u32 va, u32 width, u32 height, u32 pica_format, u32 row, u32 quarters,
                       u32 margin, bool flip, u32 rows, const ScreenQuad& quad, float opacity) {
    Batch& b = g.batch;
    const auto [w, h] = QuadSize(quad);
    // An 8-bit output's picture: U8888 as the pass's tile held it (SGXD_RECIPE_PACK_U8888), so
    // unmirrored. A 16-bit one's: the LCD's format, read as DrawGuestScreen reads it.
    u32 code = 0x0c, swizzle = 0, bytes = 4;
    const bool own_format = pica_format != 0 && pica_format != 1;
    if (!g.in_frame || g.screens_in_frame >= ScreenSlots || b.draw_count >= b.max_draws ||
        width % 8 != 0 || width == 0 || height == 0 || w < 1.0f || h < 1.0f ||
        (own_format && !ScreenFormat(pica_format, code, swizzle, bytes))) {
        return false;
    }
    const u32 slot = g.screens_in_frame++;
    g.touches[slot * 2] = va;
    g.touches[slot * 2 + 1] = va + width * height * bytes;
    SgxdRenderer::SgxdCacheSkipped(SgxdRenderer::CacheSite::Screens, width * height * bytes);
    // Texel (u, v): u along a picture row, the LCD buffer's rows, left to right on the upright
    // picture; v across them, the buffer's columns, from the bottom of the upright picture to its
    // top.
    // The widescreen picture's margins (margin of the screen's rows each side, the picture's
    // first column the screen's row -margin): as much of them as the quad is wider than the
    // screen, none in one of its shape.
    const float scale = static_cast<float>(quarters) / 4.0f;
    const float screen_rows = static_cast<float>(rows);
    const float shown =
        std::clamp((w * 240.0f / h - screen_rows) / 2.0f, 0.0f, static_cast<float>(margin));
    // Flipped, the display's left edge is the picture's column row + margin plus the margin
    // shown, and the columns go down to its right edge.
    const float first = static_cast<float>(row + margin) + (flip ? shown : -shown);
    const float last = flip ? first - screen_rows - 2 * shown : first + screen_rows + 2 * shown;
    const float u0 = scale * first / width, u1 = scale * last / width;
    const float v1 = scale * 240 / height;
    const float uv[4][2] = {{u0, v1}, {u1, v1}, {u0, 0.0f}, {u1, 0.0f}};
    PutScreenVertices(slot, quad, opacity, uv);
    // Bilinear: the scaled picture scaled again to the layout's rectangle. Translucent, its
    // alpha read as one: U8888's 1BGR, RGBA's RGB1 (RGB565 has none).
    const bool blend = opacity < 1.0f;
    u32 tex[4];
    sgxd_recipe_texture_words_format(va, width, height, 1, code,
                                     !blend ? swizzle
                                     : own_format ? (swizzle == 2 ? 6 : swizzle)
                                                  : 4,
                                     tex);
    const State::PixelEntry& e = PixelEntryFor(tex, blend);
    SgxdRecipeDrawPrograms& p = g.screen_programs[slot];
    p.fprim = e.va;
    p.fprim_units = e.units;
    const u32 isp = (blend ? 1u << 25 : 0u) | 7u << 22 | 1u << 20 | 1u << 9;
    b.draws[b.draw_count++] = SgxdRecipeDraw{0, 6, isp, SGXD_R_FPRIM, 0, 0, &p};
    return true;
}

void EndFrame() {
    if (!g.initialized || !g.in_frame) {
        return;
    }
    g.in_frame = false;
    if (g.failed) {
        return;
    }
    SgxdRecipe& r = g.recipe;
    u8* buffer = g.buffers + g.back * DisplayBufferSize;
    char failed[160] = "";
    *reinterpret_cast<volatile u32*>(g.memory + g.kicker_offset + BackgroundSa) = g.clear;
    if (sgxd_recipe_set_draws(&r, g.memory, g.batch.draws, g.batch.draw_count) < 0) {
        Report("sgxd: frame {}: {} draws not taken", g.frame_index, g.batch.draw_count);
        return;
    }
    sgxd_recipe_set_target(&r, g.memory, DisplayVa + g.back * DisplayBufferSize, DisplayWidth);
#ifdef VITA_DIAGNOSTICS
    // The last present's 3D reads this recipe's memory, which the restage and this list set
    // up again: it must be done.
    if (Async() && g.frame_index > 0 &&
        (sgxd_session_pending() == &r || !sgxd_chain_reached(g.shown_fence))) {
        static u32 reported;
        if (reported++ < 8) {
            Report("sgxd: frame {}: restaged with the last present's 3D maybe running "
                   "(pending {}, fence {} not reached)",
                   g.frame_index, sgxd_session_pending() == &r ? 1 : 0, g.shown_fence);
        }
    }
#endif
    sgxd_recipe_restage(&r, g.memory);
    SgxdRenderer::SgxdTrace("sgx_present");
    if (Async()) {
        // After the display transfer's blit: this list's TA runs over its 3D, then waits for it
        // (the guest screens are its output). The buffer goes on screen at the next BeginFrame,
        // once this 3D is done; three buffers keep it apart from the shown and the set one.
        if (sgxd_session_render_overlap(&r, g.touches, failed, sizeof(failed)) < 0) {
            std::snprintf(g.failed_text, sizeof(g.failed_text), "%s", failed);
            g.failed = true;
            Report("sgxd: frame {} stopped: {}", g.frame_index, failed);
            return;
        }
        g.shown_pending = true;
        g.shown_buffer = buffer;
        g.shown_fence = sgxd_chain_fence();
        // The presents' interval, for the flip's pacing: each one's from 4 to 100 ms.
        const SceUInt64 now_us = sceKernelGetProcessTimeWide();
        if (g.last_issue_us != 0) {
            const s64 interval =
                std::clamp<s64>(static_cast<s64>(now_us - g.last_issue_us), 4000, 100000);
            g.issue_interval_us += (interval - g.issue_interval_us) / 8;
        }
        g.last_issue_us = now_us;
        SgxdRenderer::SgxdTrace("sgx_present_end");
#ifdef VITA_DIAGNOSTICS
        if (g.frame_index % 600 == 0) {
            Report("sgxd: frame {}: {} draws, {} vertices, {} indices (async)", g.frame_index,
                   g.batch.draw_count, g.batch.vertex_count, g.batch.index_count);
        }
#endif
        g.frame_index++;
        return;
    }
    SgxdSessionRender render{};
    if ((g.frame_index > 0 && sgxd_session_phase(&r, sgxd_session_slc_inval, "slc", failed,
                                                 sizeof(failed)) < 0) ||
        sgxd_session_render(&r, &render, failed, sizeof(failed)) < 0) {
        // A stopped phase leaves the GPU in a state the next kick cannot build on; the frames
        // stop here, and the display keeps the last good one.
        std::snprintf(g.failed_text, sizeof(g.failed_text), "%s", failed);
        g.failed = true;
        Report("sgxd: frame {} stopped: {}", g.frame_index, failed);
        return;
    }
    SgxdRenderer::SgxdTrace("sgx_present_end");
    g.shown_pending = true;
    g.shown_buffer = buffer;
    g.shown_fence = sgxd_chain_fence();
    ShowLast();
#ifdef VITA_DIAGNOSTICS
    if (g.frame_index % 600 == 0) {
        Report("sgxd: frame {}: {} draws, {} vertices, {} indices; TA {} us, 3D {} us",
               g.frame_index, g.batch.draw_count, g.batch.vertex_count, g.batch.index_count,
               render.ta_us, render.d3_us);
    }
#endif
    g.frame_index++;
}

u32 TakeDisplayWaits() {
    return std::exchange(g.display_waits, 0);
}

void DumpNextFrame() {
    dump_requested = true;
}

bool DumpPending() {
    return dump_requested;
}

} // namespace VitaFrontend::SgxdPresent
