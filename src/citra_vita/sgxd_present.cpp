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
#include "common/logging/log.h"
#include "sgxd.h"
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

struct State {
    bool initialized = false;
    SceUID block = -1, cdram = -1, texture = -1, black = -1;
    u8* memory = nullptr;
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
    char failed_text[160]{};
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
    g.texture = g.cdram = g.block = -1;
    g.memory = g.buffers = nullptr;
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

} // Anonymous namespace

bool Initialize(std::string* error) {
    if (g.initialized) {
        return true;
    }
    g = State{};

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
    g.size = (g.kicker_offset + 0x1000 + 0xffff) & ~0xffffu;

    g.block = sceKernelAllocMemBlock("sgxd_present_recipe", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW,
                                     g.size, nullptr);
    g.cdram = sceKernelAllocMemBlock("sgxd_present_display", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
                                     DisplayBuffers * DisplayBufferSize, nullptr);
    void* base = nullptr;
    void* buffers = nullptr;
    if (g.block < 0 || g.cdram < 0 || sceKernelGetMemBlockBase(g.block, &base) < 0 ||
        sceKernelGetMemBlockBase(g.cdram, &buffers) < 0) {
        *error = fmt::format("sgxd: no memory for the display ({:#010x}, {:#010x})",
                             static_cast<u32>(g.block), static_cast<u32>(g.cdram));
        FreeBlocks();
        return false;
    }
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
        sgxdRelease();
        FreeBlocks();
        return false;
    }
    Report("sgxd: {} KiB at {:#010x}, {} display buffers at {:#010x}", g.size / 1024, RecipeVa,
           DisplayBuffers, DisplayVa);
    g.initialized = true;
    return true;
}

bool IsInitialized() {
    return g.initialized;
}

void Shutdown() {
    if (!g.initialized) {
        return;
    }
    ShowBlackFrame();
    if (g.texture_size) {
        sgxdUnmap(TextureVa, g.texture_size);
    }
    const int unmapped_fb = sgxdUnmap(DisplayVa, DisplayBuffers * DisplayBufferSize);
    const int unmapped = sgxdUnmap(RecipeVa, g.size);
    const int released = sgxdRelease();
    Report("sgxd: unmap {:#x}, {:#x}; release {:#x} after {} frames", static_cast<u32>(unmapped),
           static_cast<u32>(unmapped_fb), static_cast<u32>(released), g.frame_index);
    FreeBlocks();
    const SceUID black = g.black;
    g = State{};
    g.black = black;   // still on screen; the process's exit frees it
}

bool SetTexture(const void* texels, u32 width, u32 height, bool bilinear) {
    if (!g.initialized || g.texture >= 0 || width == 0 || height == 0 || width > 4096 ||
        height > 4096 || (width & 7)) {
        return false;
    }
    const u32 bytes = width * height * 4;
    const u32 size = (bytes + 0xffff) & ~0xffffu;
    g.texture = sceKernelAllocMemBlock("sgxd_present_texture", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW,
                                       size, nullptr);
    void* base = nullptr;
    if (g.texture < 0 || sceKernelGetMemBlockBase(g.texture, &base) < 0) {
        Report("sgxd: no memory for a {}x{} texture ({:#010x})", width, height,
               static_cast<u32>(g.texture));
        return false;
    }
    std::memcpy(base, texels, bytes);
    const int mapped = sgxdMap(TextureVa, base, size, 0);
    if (mapped != 0) {
        Report("sgxd: texture map {:#010x}", static_cast<u32>(mapped));
        sceKernelFreeMemBlock(g.texture);
        g.texture = -1;
        return false;
    }
    g.texture_size = size;
    u32 words[4];
    sgxd_recipe_texture_words(TextureVa, width, height, bilinear ? 1 : 0, words);
    sgxd_recipe_set_texture(&g.recipe, g.memory, words);
    return true;
}

void BeginFrame(u8 clear_r, u8 clear_g, u8 clear_b) {
    if (!g.initialized) {
        return;
    }
    g.back = g.frame_index % DisplayBuffers;
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
    g.in_frame = true;
}

Batch& CurrentBatch() {
    return g.batch;
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
    // The last present's 3D reads the region arrays restage refills: it must be done.
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
    SgxdSessionRender render{};
    SgxdRenderer::SgxdTrace("sgx_present");
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

    static u32 dump_index = 0;
    if (dump_requested) {
        dump_requested = false;
        GxmRenderer::SgxdCacheRange(buffer, DisplayWidth * DisplayHeight * 4, SGXD_CACHE_INVALIDATE);
        char path[64];
        std::snprintf(path, sizeof(path), "ux0:data/azahar/frame_%02u.ppm", dump_index++ % 20);
        WriteDisplayPpm(path, buffer);
        LOG_INFO(Frontend, "wrote {}", path);
    }

    // The frame before must be on screen before this one is queued: with three buffers the one
    // rendered next is then neither shown nor queued. `novsync` (diagnostics) skips the wait and
    // may tear.
    static const bool novsync = GxmRenderer::GxmFlag("novsync");
    if (!novsync) {
        sceDisplayWaitSetFrameBuf();
    }
    const int shown = ShowFrame(buffer);
    if (shown < 0) {
        LOG_ERROR(Frontend, "sgxd: sceDisplaySetFrameBuf {:#010x}", static_cast<u32>(shown));
    }
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
