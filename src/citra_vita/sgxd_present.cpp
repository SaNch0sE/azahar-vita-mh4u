// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <psp2/display.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <fmt/format.h>

#include "citra_vita/sgxd_present.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "sgxd.h"
#include "sgxd_program.h"
#include "sgxd_ptla.h"
#include "sgxd_recipe.h"
#include "sgxd_session.h"
#include "video_core/renderer_sgxd/sgxd_flags.h"
#include "video_core/renderer_sgxd/sgxd_backend.h"

namespace VitaFrontend::SgxdPresent {

namespace {

// GPU addresses (docs/sgxd/memory.md; sgxd_test's): the recipe's block at 16 MiB,
// the display buffers at 1 GiB, the texture at 48 MiB.
constexpr u32 RecipeVa = 0x01000000;
constexpr u32 TextureVa = 0x03000000;
// The parameter buffer every recipe shares (sgxd_recipe_pb), Sony's model: its page table at
// 32 MiB, the pages 256 KiB on. Page numbers count from the DPM requestor base, this block's
// address, so every recipe block lies within 256 MiB of it. 1024 pages hold two scenes in
// flight at the 418 a recipe had to itself.
constexpr u32 PbVa = 0x02000000;
constexpr u32 PbPages = 1024;
constexpr u32 PbSize = 0x40000 + PbPages * 0x1000;
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
    SceUID block = -1, cdram = -1, texture = -1, black = -1, pb = -1;
    u8* memory = nullptr;
    u8* pb_memory = nullptr;
    u8* buffers = nullptr;
    u32 size = 0, texture_size = 0;
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
    u32 touches[4]{}; // the guest screens this frame samples, [lo, hi) GPU address pairs
    char failed_text[160]{};

    u32 screen_offset = 0;
    u32 screen_vertex = 0, screen_fragment = 0;   // DOUTU word 0s
    SgxdRecipeDrawPrograms screen_programs[ScreenSlots]{};
    u32 screens_in_frame = 0;
    struct PixelEntry {
        u32 tex[4];
        u32 va, units;
    } pixel_entries[ScreenPixelEntries]{};
    u32 pixel_used = 0, pixel_next = 0;
};

State g;
bool dump_requested = false;

template <typename... Args>
void Report(fmt::format_string<Args...> format, Args&&... args) {
    const std::string text = fmt::format(format, std::forward<Args>(args)...);
    LOG_INFO(Frontend, "{}", text);
    sceClibPrintf("[azahar] %s\n", text.c_str());
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
    g.texture = g.cdram = g.block = g.pb = -1;
    g.memory = g.buffers = g.pb_memory = nullptr;
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

// The frame just rendered as a PPM, as GxmPresent's WriteDisplayPpm.
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

int ShowFrame(void* base) {
    SceDisplayFrameBuf fb{};
    fb.size = sizeof(fb);
    fb.base = base;
    fb.pitch = DisplayWidth;
    fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    fb.width = DisplayWidth;
    fb.height = DisplayHeight;
    return sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
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
    u64 words[4];
    std::memcpy(area + ScreenVertexCode, words, sgxd_program_blit_vertex(words) * 8);
    std::memcpy(area + ScreenFragmentCode, words, sgxd_program_screen_fragment(words) * 8);
    g.screen_vertex = sgxd_program_doutu0(0, g.screen_offset + ScreenVertexCode -
                                                 r.offset[SGXD_R_CV], 0);
    g.screen_fragment = sgxd_program_doutu0(1, g.screen_offset + ScreenFragmentCode -
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

// The pixel primary sampling tex, written the first time tex is seen. A full table starts
// over at its oldest entry.
const State::PixelEntry& PixelEntryFor(const u32 tex[4]) {
    for (u32 i = 0; i < g.pixel_used; i++) {
        if (std::memcmp(g.pixel_entries[i].tex, tex, 16) == 0) {
            return g.pixel_entries[i];
        }
    }
    const u32 i = g.pixel_used < ScreenPixelEntries ? g.pixel_used++ : g.pixel_next;
    g.pixel_next = (i + 1) % ScreenPixelEntries;
    State::PixelEntry& e = g.pixel_entries[i];
    SgxdPdsProgram pds{};
    sgxd_program_screen_pixel_pds(g.screen_fragment, tex, &pds);
    PutPds(ScreenPixelPds + i * ScreenPdsSlot, pds);
    std::memcpy(e.tex, tex, 16);
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
        sgxdRelease();
        FreeBlocks();
        return false;
    }
    Report("sgxd: {} KiB at {:#010x}, {} display buffers at {:#010x}", g.size / 1024, RecipeVa,
           DisplayBuffers, DisplayVa);
    if (g.pb >= 0) {
        Report("sgxd: one parameter buffer, pages {:#x}-{:#x} at {:#010x}, requestor base {:#010x}",
               sgxd_recipe_pb.first, sgxd_recipe_pb.last, PbVa + 0x40000, RecipeVa);
    }
    g.initialized = true;
    return true;
}

bool IsInitialized() {
    return g.initialized;
}

void ShowLast();

void Shutdown() {
    if (!g.initialized) {
        return;
    }
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
    if (sgxd_ptla_page()->chain_fault) {
        sgxd_session_render_async_failed(failed, sizeof(failed));
        ASSERT_MSG(false, "sgxd: the async present stopped: {}", failed);
    }
    return !sgxd_chain_reached(g.shown_fence);
}

// The last async present's 3D completes, then its buffer goes on screen. Before the recipe block
// or the texture is written again: its 3D reads them.
void ShowLast() {
    if (!g.shown_pending) {
        return;
    }
    g.shown_pending = false;
    char failed[200] = "";
    SgxdRenderer::SgxdTrace("flip", sgxd_chain_reached(g.shown_fence) ? 1u : 0u);
    if (!sgxd_chain_reached(g.shown_fence)) {
        // No list after it has finished its 3D: one does now.
        if (sgxd_session_pending() == &g.recipe &&
            sgxd_session_finish_pending(failed, sizeof(failed)) < 0) {
            ASSERT_MSG(false, "sgxd: the present's 3D did not finish: {}", failed);
        }
        if (sgxd_chain_idle() < 0) {
            sgxd_session_render_async_failed(failed, sizeof(failed));
            ASSERT_MSG(false, "sgxd: the async present stopped: {}", failed);
        }
    }
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
    static const bool novsync = GxmRenderer::GxmFlag("novsync");
    if (!novsync) {
        sceDisplayWaitSetFrameBuf();
    }
    const int shown = ShowFrame(buffer);
    if (shown < 0) {
        LOG_ERROR(Frontend, "sgxd: sceDisplaySetFrameBuf {:#010x}", static_cast<u32>(shown));
    }
}

bool SetTexture(const void* texels, u32 width, u32 height, bool bilinear) {
    if (!g.initialized || g.texture >= 0 || width == 0 || height == 0 || width > 4096 ||
        height > 4096 || (width & 7)) {
        return false;
    }
    const u32 bytes = width * height * 4;
    const u32 size = (bytes + 0x3ffff) & ~0x3ffffu; // CDRAM's 256 KiB unit
    g.texture = sceKernelAllocMemBlock("sgxd_present_texture", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
                                       size, nullptr);
    void* base = nullptr;
    const bool got = g.texture >= 0 && sceKernelGetMemBlockBase(g.texture, &base) >= 0;
    ASSERT_MSG(got, "sgxd: no {} KiB of CDRAM for a {}x{} texture ({:#010x})", size >> 10,
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
    ShowLast();
    u32 words[4];
    sgxd_recipe_texture_words(TextureVa, width, height, bilinear ? 1 : 0, words);
    sgxd_recipe_set_texture(&g.recipe, g.memory, words);
    return true;
}

void BeginFrame(u8 clear_r, u8 clear_g, u8 clear_b) {
    if (!g.initialized) {
        return;
    }
    ShowLast();
    g.back = g.frame_index % DisplayBuffers;
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

bool DrawGuestScreen(u32 va, const u8* cpu, u32 stride, u32 columns, u32 pica_format, u32 x,
                     u32 y, u32 w, u32 h) {
    u32 code, swizzle, bytes;
    Batch& b = g.batch;
    // A linear texture's rows are its width rounded up to 8 texels: the framebuffer's stride is the
    // texture's width, 240 texels or more, and u covers the first 240.
    if (!g.in_frame || g.screens_in_frame >= ScreenSlots || b.draw_count >= b.max_draws ||
        !ScreenFormat(pica_format, code, swizzle, bytes) || stride % bytes != 0 ||
        stride / bytes < 240 || (stride / bytes) % 8 != 0 || w == 0 || h == 0 || columns == 0) {
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
    // top; v across the columns, left to right. So display x follows v and display y follows u
    // reversed. Two triangles; x, y in clip space, z, w, a white colour (unused), u, v.
    constexpr u32 N = SGXD_PROGRAM_BLIT_VERTEX_WORDS;
    const float x0 = 2.0f * x / DisplayWidth - 1, x1 = 2.0f * (x + w) / DisplayWidth - 1;
    const float y0 = 2.0f * y / DisplayHeight - 1, y1 = 2.0f * (y + h) / DisplayHeight - 1;
    const float corner[6][4] = {
        {x0, y0, 1, 0}, {x1, y0, 1, 1}, {x0, y1, 0, 0},
        {x1, y0, 1, 1}, {x1, y1, 0, 1}, {x0, y1, 0, 0},
    };
    float* v = reinterpret_cast<float*>(g.memory + g.screen_offset + ScreenVertices + slot * 0x100);
    for (u32 n = 0; n < 6; n++) {
        const float words[N] = {corner[n][0], corner[n][1], 0.5f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
                                corner[n][2] * u_end, corner[n][3]};
        std::memcpy(v + n * N, words, sizeof(words));
    }

    // Point sampled: each display pixel shows the framebuffer pixel under its centre.
    u32 tex[4];
    sgxd_recipe_texture_words_format(va, texels, columns, 0, code, swizzle, tex);
    const State::PixelEntry& e = PixelEntryFor(tex);
    SgxdRecipeDrawPrograms& p = g.screen_programs[slot];
    p.fprim = e.va;
    p.fprim_units = e.units;
    // Opaque, DCMP ALWAYS, no depth write (the blit's ISP word).
    const u32 isp = 7u << 22 | 1u << 20 | 1u << 9;
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
        // once this 3D is done; three buffers keep it apart from the shown and the queued one.
        if (sgxd_session_render_overlap(&r, g.touches, failed, sizeof(failed)) < 0) {
            std::snprintf(g.failed_text, sizeof(g.failed_text), "%s", failed);
            g.failed = true;
            Report("sgxd: frame {} stopped: {}", g.frame_index, failed);
            return;
        }
        g.shown_pending = true;
        g.shown_buffer = buffer;
        g.shown_fence = sgxd_chain_fence() + 1;
        SgxdRenderer::SgxdTrace("sgx_present_end");
        if (g.frame_index % 600 == 0) {
            Report("sgxd: frame {}: {} draws, {} vertices, {} indices (async)", g.frame_index,
                   g.batch.draw_count, g.batch.vertex_count, g.batch.index_count);
        }
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
    if (g.frame_index % 600 == 0) {
        Report("sgxd: frame {}: {} draws, {} vertices, {} indices; TA {} us, 3D {} us",
               g.frame_index, g.batch.draw_count, g.batch.vertex_count, g.batch.index_count,
               render.ta_us, render.d3_us);
    }
    g.frame_index++;
}

void DumpNextFrame() {
    dump_requested = true;
}

bool DumpPending() {
    return dump_requested;
}

} // namespace VitaFrontend::SgxdPresent
