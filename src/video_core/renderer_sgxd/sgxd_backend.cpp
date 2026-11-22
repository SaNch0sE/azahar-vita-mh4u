// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include "common/assert.h"
#include "common/logging/log.h"
#include "video_core/renderer_gxm/gxm_flags.h"
#include "video_core/renderer_gxm/sgxd_backend.h"
#include "video_core/renderer_gxm/usse/usse_encoder.h"
#include "video_core/shader/generator/profile.h"

#include <psp2/kernel/sysmem.h>
#include "sgxd.h"
#include "sgxd_mkq.h"
#include "sgxd_ptla.h"
#include "sgxd_session.h"

namespace SgxdRenderer {

namespace {

u64 WaitMark() {
#ifdef VITA_DIAGNOSTICS
    return sgxd_wait_us_total();
#else
    return 0;
#endif
}

// GPU address map: docs/sgxd/memory.md.
constexpr u32 BlockVa = 0x04000000;
constexpr u32 RecipesEnd = 0x18000000;
constexpr u32 SurfacesVa = 0x48000000;
constexpr u32 SurfacesSize = 8u << 20;
// The guest textures' host copies, after the surfaces. They are U8888, 8 times an ETC1
// texture's bytes: a game's open pass read 42 textures in 14 MiB.
constexpr u32 TexturesVa = SurfacesVa + SurfacesSize;
constexpr u32 TexturesSize = 40u << 20;
constexpr u32 Pages = 418;
constexpr u32 MaxIndices = SGXD_RECIPE_MAX_INDICES;
// The code heap follows each layout. It must stay within DOUTU's reach of CV and CF (EXE, 20
// bits of 8 bytes: 8 MiB). The PDS arena and the pass data make the shared block.
constexpr u32 CodeSize = 1u << 20;
constexpr u32 PdsSize = 1u << 20;
constexpr u32 DataSize = 1u << 20;
// LUT_LF_ROWS rows of 256 F32F32 texels (1 MiB), then the proctex rows (F32F32 and F16x4).
constexpr u32 LutLfSize = Pica::Shader::LUT_LF_ROWS * 256 * 8;
constexpr u32 LutRgSize = Pica::Shader::LUT_RG_ROWS * 256 * 8;
constexpr u32 LutRgbaSize = Pica::Shader::LUT_RGBA_ROWS * 256 * 8;
constexpr u32 LutSize = LutLfSize + LutRgSize + LutRgbaSize;

constexpr u32 AlignUp(u32 value, u32 align) {
    return (value + align - 1) & ~(align - 1);
}

// A block mapped at va for the GPU, in CDRAM: uncached for the CPU, so what it writes needs no
// cache work. Without CDRAM it stops. `size` is a multiple of 256 KiB.
u8* MapBlock(const char* name, u32 va, u32 size, int& uid) {
    uid = sceKernelAllocMemBlock(name, SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, size, nullptr);
    void* base = nullptr;
    const int got = uid >= 0 ? sceKernelGetMemBlockBase(uid, &base) : uid;
    ASSERT_MSG(got >= 0, "sgxd: no {} KiB of CDRAM for {} ({:#010x})", size >> 10, name,
               static_cast<u32>(got));
    [[maybe_unused]] const int idle = sgxd_gpu_idle();
    DEBUG_ASSERT_MSG(idle == 0, "sgxd: a PTLA op never completed before mapping {}", name);
    const int mapped = sgxdMap(va, base, size, 0);
    DEBUG_ASSERT_MSG(mapped == 0, "sgxd: {} not mapped at GPU {:#010x}: {:#010x}", name, va,
                     static_cast<u32>(mapped));
    return static_cast<u8*>(base);
}

void FreeBlock(u8* cpu, u32 va, u32 size, int uid) {
    if (cpu == nullptr) {
        return;
    }
    sgxd_gpu_idle();
    sgxdUnmap(va, size);
    sceKernelFreeMemBlock(uid);
}

} // Anonymous namespace

SgxdBackend::SgxdBackend() {
    surfaces = MapBlock("azahar-sgxd-surfaces", SurfacesVa, SurfacesSize, surfaces_block);
    surfaces_size = SurfacesSize;
    textures = MapBlock("azahar-sgxd-textures", TexturesVa, TexturesSize, textures_block);
    pds_begin = pds_used = 0;
    pds_end = pds_begin + PdsSize;
    data_begin = data_used = pds_end;
    data_end = data_begin + DataSize;
    lut_begin = data_end;
    shared_size = AlignUp(lut_begin + LutSize, 1u << 20);
    shared = MapBlock("azahar-sgxd-shared", BlockVa, shared_size, shared_block);
    next_va = BlockVa + shared_size;
    draws.resize(MaxDraws);
    programs.resize(MaxDraws);
    identity.resize(MaxIndices);
    for (u32 n = 0; n < MaxIndices; n++) {
        identity[n] = static_cast<u16>(n);
    }
    // A layout only has to hold MaxDraws valid draws; each pass writes its own.
    init_draws.assign(MaxDraws, SgxdRecipeDraw{0, 3, SGXD_RECIPE_ISP_NEVER, SGXD_R_FPRIM, 0, 0,
                                               nullptr});
    if (!GxmFlag("ptlapoll")) {
        const int r = sgxd_ptla_events(&events);
        ASSERT_MSG(r == 0, "sgxd: no PTLA completion interrupt ({:#010x})", static_cast<u32>(r));
        ptla_async = true;
    }
    // A render's TA done, 3D kick and 3D done run from IRQ 52 as well; gxm_flags renderpoll
    // keeps them in the render thread's syscalls.
    render_async = ptla_async && !GxmFlag("renderpoll");
    LOG_INFO(Render, "sgxd: renders {}", render_async ? "as async lists" : "synchronous");
    // Four macrotiles a side at every size: the cores' loads then follow the scene's quadrants.
    sgxd_recipe_grid = 4;
#ifdef VITA_DIAGNOSTICS
    if (GxmFlag("grid2")) {
        sgxd_recipe_grid = 0;
    }
#endif
}

u32 SgxdBackend::RenderFence() const {
    return sgxd_chain_fence();
}

bool SgxdBackend::RenderReached(u32 fence) const {
    return sgxd_chain_reached(fence) != 0;
}

bool SgxdBackend::RenderTaReached(u32 fence) const {
    return sgxd_chain_ta_reached(fence) != 0;
}

void SgxdBackend::TraceStamps() {
#ifdef VITA_DIAGNOSTICS
    // tracelog: the lists' GPU stamps since the last call, from the kernel's log (the oldest
    // lost when more than the log holds ran in between).
    static constexpr const char* tags[4] = {"gpu_ta", "gpu_ta_end", "gpu_3d", "gpu_3d_end"};
    static u32 traced;
    const volatile SgxdEvents* ev = sgxd_ptla_page();
    if (!ev) {
        return;
    }
    const u32 stamps = ev->stamps;
    std::atomic_thread_fence(std::memory_order_acquire);
    if (stamps - traced > SGXD_STAMP_LOG) {
        traced = stamps - SGXD_STAMP_LOG;
    }
    for (; traced != stamps; traced++) {
        const volatile u32* entry = ev->stamp_log[traced % SGXD_STAMP_LOG];
        GxmTrace(tags[entry[1] & 3], entry[0]);
    }
#endif
}

void SgxdBackend::RenderIdle(const char* why) {
    if (!render_async || sgxd_chain_idle() == 0) {
        RetireReached();
        return;
    }
    char failed[200];
    sgxd_session_render_async_failed(failed, sizeof(failed));
    LOG_CRITICAL(Render, "sgxd: the async render before {} stopped", why);
    Fault(*current, failed);
}

u32 SgxdBackend::FenceAll() const {
    return sgxd_chain_fence() + (sgxd_session_pending() != nullptr ? 1u : 0u);
}

void SgxdBackend::RenderWait(u32 fence, const char* why) {
    if (sgxd_chain_reached(fence)) {
        return;
    }
    // A fence past the lists sent is the pending 3D's: its finish goes now.
    if (static_cast<s32>(fence - sgxd_chain_fence()) > 0) {
        DEBUG_ASSERT_MSG(fence - sgxd_chain_fence() == 1 && sgxd_session_pending(),
                         "sgxd: {} waits for fence {}, past the {} lists sent", why, fence,
                         sgxd_chain_fence());
        FinishPending(FinishWhy::Wait);
    }
    [[maybe_unused]] const u64 before = WaitMark();
    if (sgxd_chain_wait(fence) == 0) {
        TraceStamps();
        WaitSite(why, before);
        RetireReached();
        return;
    }
    char failed[200];
    sgxd_session_render_async_failed(failed, sizeof(failed));
    LOG_CRITICAL(Render, "sgxd: the async render before {} stopped", why);
    Fault(*current, failed);
}

void SgxdBackend::WaitPassMark(u32 mark, const char* why) {
    if (mark == 0) {
        return;
    }
    // The open pass's own mark would underflow below and wait on an old fence instead.
    ASSERT_MSG(mark <= passes, "sgxd: {} waits for the open pass", why);
    // Past the fences kept, the oldest kept ended later.
    const u32 at = passes - mark < PassFences ? mark : passes - PassFences + 1;
    RenderWait(pass_fences[at % PassFences], why);
}

void SgxdBackend::AfterGpu(std::function<void()> action) {
    retire.emplace_back(FenceAll(), std::move(action));
}

void SgxdBackend::RetireReached() {
    std::size_t n = 0;
    while (n < retire.size() && sgxd_chain_reached(retire[n].first)) {
        retire[n].second();
        n++;
    }
    retire.erase(retire.begin(), retire.begin() + n);
}

void SgxdBackend::RetireAll(const char* why) {
    if (!retire.empty()) {
        RenderWait(retire.back().first, why);
        RetireReached();
    }
}

u32 SgxdBackend::PtlaFence() const {
    return sgxd_ptla_fence();
}

bool SgxdBackend::PtlaReached(u32 fence) const {
    return sgxd_ptla_reached(fence) != 0;
}

SgxdBackend::~SgxdBackend() {
    if (ptla_async) {
        sgxd_ptla_events(nullptr);
    }
    for (const auto& r : recipes) {
        FreeBlock(r->memory, r->va, r->size, r->block);
    }
    FreeBlock(shared, BlockVa, shared_size, shared_block);
    FreeBlock(surfaces, SurfacesVa, surfaces_size, surfaces_block);
    FreeBlock(textures, TexturesVa, TexturesSize, textures_block);
}

SgxdBackend::Recipe& SgxdBackend::RecipeFor(u32 width, u32 height, bool side) {
    Recipe* oldest = nullptr;
    for (const auto& r : recipes) {
        if (r->width == width && r->height == height && r->side == side) {
            return *r;
        }
    }
    auto made = std::make_unique<Recipe>();
    Recipe& r = *made;
    r.width = width;
    r.height = height;
    r.side = side;
    r.va = next_va;
    [[maybe_unused]] const int init = sgxd_recipe_init_programs(
        &r.recipe, width, height, r.va, Pages, identity.data(), MaxIndices, init_draws.data(),
        MaxDraws);
    DEBUG_ASSERT_MSG(init == 0, "sgxd: the recipe does not take a {}x{} target", width, height);
    r.bg_offset = AlignUp(r.recipe.offset[SGXD_R_END], 0x1000);
    r.code_begin = r.bg_offset + 0x1000;
    r.size = AlignUp(r.code_begin + CodeSize, 1u << 20);
    DEBUG_ASSERT_MSG(r.code_begin + CodeSize - r.recipe.offset[SGXD_R_CV] <= (0xfffffu << 3),
                     "sgxd: the code heap is past DOUTU's reach of CV");
    // A program's DOUTU holds its distance from CV or CF, the same under every recipe only if
    // the code heap is: the layout after CV does not depend on the size.
    if (!recipes.empty()) {
        const Recipe& first = *recipes.front();
        [[maybe_unused]] const u32 cv = r.recipe.offset[SGXD_R_CV];
        [[maybe_unused]] const u32 first_cv = first.recipe.offset[SGXD_R_CV];
        DEBUG_ASSERT_MSG(r.code_begin - cv == first.code_begin - first_cv &&
                             r.recipe.offset[SGXD_R_CF] - cv ==
                                 first.recipe.offset[SGXD_R_CF] - first_cv,
                         "sgxd: the {}x{} recipe's code heap is not where the {}x{} one's is",
                         width, height, first.width, first.height);
    }
    DEBUG_ASSERT_MSG(r.va + r.size <= RecipesEnd, "sgxd: no GPU space for a {}x{} recipe",
                     width, height);
    r.memory = MapBlock("azahar-sgxd-recipe", r.va, r.size, r.block);
    next_va = r.va + r.size;

    sgxd_recipe_write(&r.recipe, r.memory);
    // The arrays go with the recipe: only the indices are read again.
    r.recipe.draws = nullptr;
    sgxd_recipe_sa_background(&r.recipe, r.memory, r.bg_offset, 0xff000000u);
    if (!recipes.empty()) {
        const Recipe& first = *recipes.front();
        std::memcpy(r.memory + r.code_begin, first.memory + first.code_begin, code_used);
    }
    LOG_INFO(Render,
             "sgxd: the rasterizer's {}x{} recipe, {} KiB at GPU {:#010x}: layout {} KiB, code "
             "heap copy at {:#x} ({} bytes); shared block at {:#010x}",
             width, height, r.size >> 10, r.va, r.recipe.offset[SGXD_R_END] >> 10, r.code_begin,
             code_used, BlockVa);
    recipes.push_back(std::move(made));
    return r;
}

u32 SgxdBackend::PlaceCode(unsigned cb, std::span<const u64> words) {
    DEBUG_ASSERT_MSG(!recipes.empty(), "sgxd: code placed before the first recipe");
    const u32 bytes = static_cast<u32>(words.size_bytes());
    const u32 at = AlignUp(code_used, 16);
    DEBUG_ASSERT_MSG(at + bytes <= CodeSize, "sgxd: the code heap is full ({} KiB)",
                     CodeSize >> 10);
    for (const auto& r : recipes) {
        std::memcpy(r->memory + r->code_begin + at, words.data(), bytes);
    }
    code_used = at + bytes;
    const Recipe& first = *recipes.front();
    const u32 offset = first.code_begin + at - first.recipe.offset[cb ? SGXD_R_CF : SGXD_R_CV];
#ifdef VITA_DIAGNOSTICS
    placed_code.push_back({cb, offset, bytes, at});
#endif
    return offset;
}

u32 SgxdBackend::PlacePds(const SgxdPdsProgram& program) {
    const std::string key(reinterpret_cast<const char*>(program.words), program.count * 4);
    if (const auto it = pds_by_words.find(key); it != pds_by_words.end()) {
        return it->second;
    }
    const u32 at = AlignUp(pds_used, 32);
    DEBUG_ASSERT_MSG(at + program.count * 4 <= pds_end,
                     "sgxd: the PDS arena is full ({} KiB, {} programs): the generation flip that "
                     "would reuse it is not built",
                     PdsSize >> 10, pds_by_words.size());
    std::memcpy(shared + at, program.words, program.count * 4);
    pds_used = at + program.count * 4;
    const u32 va = BlockVa + at;
    pds_by_words.emplace(key, va);
    return va;
}

SgxdBackend::Data SgxdBackend::AllocData(u32 bytes, u32 align) {
    DEBUG_ASSERT(in_pass);
    const u32 at = AlignUp(data_used, align);
    DEBUG_ASSERT_MSG(at + bytes <= data_end, "sgxd: a pass's data passed {} KiB", DataSize >> 10);
    data_used = at + bytes;
    return {shared + at, BlockVa + at};
}

SgxdBackend::Data SgxdBackend::GeometryVertices(u64 key, u32 bytes, bool& fresh) {
    const auto it = geometry_vertices.find(key);
    fresh = it == geometry_vertices.end();
    if (!fresh) {
        return it->second;
    }
    const Data vertices = AllocTexture(bytes);
    geometry_vertices.emplace(key, vertices);
    return vertices;
}

SgxdBackend::Data SgxdBackend::LutRows() const {
    return {shared + lut_begin, BlockVa + lut_begin};
}

SgxdBackend::Data SgxdBackend::LutRgRows() const {
    const u32 at = lut_begin + LutLfSize;
    return {shared + at, BlockVa + at};
}

SgxdBackend::Data SgxdBackend::LutRgbaRows() const {
    const u32 at = lut_begin + LutLfSize + LutRgSize;
    return {shared + at, BlockVa + at};
}

SgxdBackend::Data SgxdBackend::AllocSurface(u32 width, u32 height) {
    const u32 bytes = AlignUp(width * height * 4, 0x1000);
    DEBUG_ASSERT_MSG(surfaces_used + bytes <= surfaces_size,
                     "sgxd: no room for a {}x{} host surface ({} KiB used)", width, height,
                     surfaces_used >> 10);
    const Data d{surfaces + surfaces_used, SurfacesVa + surfaces_used};
    surfaces_used += bytes;
    return d;
}

SgxdBackend::Data SgxdBackend::AllocTexture(u32 bytes, u32* ready) {
    bytes = AlignUp(bytes, 0x1000);
    for (auto it = textures_free.begin(); it != textures_free.end(); ++it) {
        if (it->bytes >= bytes) {
            const u32 at = it->offset;
            it->offset += bytes;
            it->bytes -= bytes;
            if (it->bytes == 0) {
                textures_free.erase(it);
            }
            return {textures + at, TexturesVa + at};
        }
    }
    DEBUG_ASSERT_MSG(textures_used + bytes <= TexturesSize,
                     "sgxd: no room for a {} KiB texture ({} KiB used, {} ranges free)",
                     bytes >> 10, textures_used >> 10, textures_free.size());
    const Data d{textures + textures_used, TexturesVa + textures_used};
    textures_used += bytes;
    return d;
}

void SgxdBackend::FreeTexture(const Data& texture, u32 bytes) {
    // Kept in address order, merged with its neighbours.
    FreeRange range{texture.gpu - TexturesVa, AlignUp(bytes, 0x1000)};
    auto it = std::lower_bound(textures_free.begin(), textures_free.end(), range.offset,
                               [](const FreeRange& f, u32 offset) { return f.offset < offset; });
    if (it != textures_free.begin() && std::prev(it)->offset + std::prev(it)->bytes == range.offset) {
        --it;
        it->bytes += range.bytes;
    } else {
        it = textures_free.insert(it, range);
    }
    const auto next = std::next(it);
    if (next != textures_free.end() && it->offset + it->bytes == next->offset) {
        it->bytes += next->bytes;
        textures_free.erase(next);
    }
}

void SgxdBackend::OpenPass(u32 target_gpu, u32 width, u32 height, u32 pack, u32 clear,
                           float depth) {
    DEBUG_ASSERT(!in_pass);
    // The recipe blocks and the shared block's pass data are the running render's until it
    // completes.
    RenderIdle("a pass opened");
    Recipe& r = RecipeFor(width, height);
    current = &r;
    sgxd_recipe_set_target(&r.recipe, r.memory, target_gpu, width);
    sgxd_recipe_set_target_pack(&r.recipe, r.memory, pack);
    *reinterpret_cast<volatile u32*>(r.memory + r.bg_offset) = clear;
    sgxd_recipe_set_background_depth(&r.recipe, r.memory, depth);
    data_used = data_begin;
    draw_count = 0;
    cached.clear();
    stores.clear();
    in_pass = true;
}

void SgxdBackend::BeginPass(const Data& target, u32 width, u32 height, u32 clear, float depth) {
    // A tile no draw touches stays EMPTY and is skipped whole, background included, keeping the
    // target's bytes: the target starts
    // as the clear through the PTLA, and the background repeats it in the tiles that render.
    const SgxdPtlaFill fill{target.gpu, SGXD_PTLA_8888, width * 4, width, height, 0, clear};
    char failed[160];
    [[maybe_unused]] const int filled = sgxd_ptla_fill(&fill, nullptr, failed, sizeof(failed));
    DEBUG_ASSERT_MSG(filled == 0, "sgxd: a {}x{} target's clear stopped: {}", width, height,
                     failed);
    OpenPass(target.gpu, width, height, SGXD_RECIPE_PACK_U8888, clear, depth);
}

void SgxdBackend::Draw(const SgxdRecipeDraw& draw, const SgxdRecipeDrawPrograms& program) {
    // Release too: past MaxDraws the stores below overrun the arrays.
    ASSERT(in_pass);
    ASSERT_MSG(draw_count < MaxDraws, "sgxd: a pass of more than {} draws", MaxDraws);
    programs[draw_count] = program;
    draws[draw_count] = draw;
    draws[draw_count].programs = &programs[draw_count];
    if (GxmFlag("sgxd_pass_recipe_draw")) {
        draws[draw_count] = {0, 3, draw.isp, SGXD_R_FPRIM, 0, 0, nullptr};
    }
    draw_count++;
}

void SgxdBackend::DrawHang() {
    DEBUG_ASSERT(in_pass);
    const Recipe& r = *current;
    SgxdRecipeDrawPrograms p =
        BlitPrograms(Data{nullptr, pass_target.lo}, r.width, r.height, r.width);
    Usse::Encoder e;
    e.Phas(true);
    const std::size_t loop = e.Br(Usse::Pred::None);
    e.PatchBr(loop, loop);
    const u32 hang = sgxd_program_doutu0(HeapCodeBase(1), PlaceCode(1, e.Words()), 0);
    const u32 colour = SGXD_PROGRAM_BLIT_DOUTI_COLOUR;
    const u32 words = 4;
    const auto fprim = PixelPds(hang, &colour, &words, 1);
    ASSERT(fprim.va != 0);
    p.fprim = fprim.va;
    p.fprim_units = fprim.units;
    Draw(SgxdRecipeDraw{0, 6, SGXD_RECIPE_ISP_TRANS | SGXD_RECIPE_ISP_CPRES, SGXD_R_FPRIM, 0, 0,
                        nullptr, SGXD_RECIPE_ISP_C},
         p);
    LOG_CRITICAL(Render, "sgxd: hangtest: pass {} ({}x{}) gets a pixel program that never ends",
                 passes, r.width, r.height);
}

void SgxdBackend::ReadsCached(const u8* cpu, u32 bytes) {
    cached.push_back({cpu, bytes});
}

void SgxdBackend::EndPass(u32* ta_us, u32* d3_us) {
    DEBUG_ASSERT(in_pass);
    FlushStreamOut(false);
    if (draw_count != 0 && draw_count < MaxDraws && hang_test.exchange(false)) {
        DrawHang();
    }
    in_pass = false;
    passes++;
    if (draw_count == 0) {
        // Its data may hold a side render's vertices, sent already.
        data_fence[data_slot] = pass_fences[passes % PassFences] = FenceAll();
        return;
    }
    Recipe& r = *current;
    const int taken = sgxd_recipe_set_draws(&r.recipe, r.memory, draws.data(), draw_count);
    ASSERT_MSG(taken == 0, "sgxd: the pass's {} draws were not taken", draw_count);
    sgxd_recipe_restage(&r.recipe, r.memory);
    for (const CachedRange& range : cached) {
        SgxdCacheRange(range.cpu, range.bytes, SGXD_CACHE_CLEAN, CacheSite::Vertices);
    }
    // A store's lines leave the cache before the GPU writes under them, and again after, so
    // the CPU reads what the GPU wrote (the second is an invalidate of clean lines).
    for (const Store& store : stores) {
        SgxdCacheRange(store.cpu, store.bytes, SGXD_CACHE_CLEAN_INVALIDATE,
                       CacheSite::TransferBefore);
    }
    SgxdSessionRender render{};
    // A store into cached memory needs the CPU's lines dropped after the GPU wrote: those
    // passes stay synchronous.
    if (stores.empty()) {
#ifdef VITA_DIAGNOSTICS
        if (passes % 60 < 2 && GxmFlag("gpucycles")) {
            static constexpr u32 offsets[] = {0x144, 0xe80, 0xe84, 0xe88, 0xe8c, 0xe90, 0xe94,
                                              0xe98, 0xeb4, 0xeb8, 0xebc, 0xec0, 0xec4, 0xec8};
            // The master's bank (0x4000; bank 0 reads the same) and the four cores' (0x8000 up).
            std::array<SgxdOp, 5 * std::size(offsets)> ops{};
            for (u32 b = 0; b < 5; b++) {
                for (u32 k = 0; k < std::size(offsets); k++) {
                    ops[b * std::size(offsets) + k] = {SGXD_OP_READ, (b + 1) * 0x4000 + offsets[k],
                                                       0, 0, 0};
                }
            }
            ASSERT_MSG(sgxd_chain_idle() == 0 && sgxd_gpu_idle() == 0 &&
                           sgxdSubmit(ops.data(), static_cast<uint32_t>(ops.size())) ==
                               static_cast<int>(ops.size()),
                       "sgxd: gpucycles reads failed");
            std::string line;
            for (const SgxdOp& op : ops) {
                line += fmt::format(" {:05x}={:08x}", op.reg, op.value);
            }
            LOG_INFO(Render, "gpucycles pass {}:{}", passes, line);
        }
#endif
        sgxd_session_render_async_result(&render);
        if (sgxd_session_render_async(&r.recipe, 0, failed, sizeof(failed)) != 0) {
            Fault(r, failed);
        }
        if (ta_us) {
            *ta_us = render.ta_us;
        }
        if (d3_us) {
            *d3_us = render.d3_us;
        }
        return;
    }
    const bool ok =
        sgxd_session_phase(&r.recipe, sgxd_session_slc_inval, "slc", failed, sizeof(failed)) ==
            0 &&
        sgxd_session_render(&r.recipe, &render, failed, sizeof(failed)) == 0;
    if (!ok) {
        static u32 dump[SGXD_DUMP_WORDS];
        const int dumped = sgxdDump(dump, SGXD_DUMP_WORDS);
        if (std::FILE* f = std::fopen("host0:azahar-sgxd-dump.bin", "wb")) {
            std::fwrite(dump, 4, dumped > 0 ? SGXD_DUMP_WORDS : 0, f);
            std::fclose(f);
        }
        if (std::FILE* f = std::fopen("host0:azahar-sgxd-block.bin", "wb")) {
            std::fwrite(r.memory, 1, r.size, f);
            std::fclose(f);
        }
        if (std::FILE* f = std::fopen("host0:azahar-sgxd-shared.bin", "wb")) {
            std::fwrite(shared, 1, shared_size, f);
            std::fclose(f);
        }
        LOG_CRITICAL(Render,
                     "sgxd: dump {:#x}; the {}x{} recipe's block {} KiB at GPU {:#010x} and the "
                     "shared block {} KiB at GPU {:#010x} written to host0",
                     static_cast<u32>(dumped), r.width, r.height, r.size >> 10, r.va,
                     shared_size >> 10, BlockVa);
        // The kernel's own snapshot from the moment the poll gave up (sgxdFaultDump), and what
        // the GPU's MMU makes of the addresses a hang is usually about: each BIF bank's fault
        // address, and each draw's programs and index list.
        const int faulted = sgxdFaultDump(dump, SGXD_DUMP_WORDS);
        if (std::FILE* f = std::fopen("host0:azahar-sgxd-fault.bin", "wb")) {
            std::fwrite(dump, 4, faulted > 0 ? SGXD_DUMP_WORDS : 0, f);
            std::fclose(f);
        }
        std::FILE* f = std::fopen("host0:azahar-sgxd-fault.txt", "w");
        const auto translate = [f](const char* what, u32 va) {
            u32 out[2]{};
            const int ret = sgxdTranslate(va, out);
            LOG_CRITICAL(Render, "sgxd: {} {:#010x}: pde {:08x} pte {:08x} ({:#x})", what, va,
                         out[0], out[1], static_cast<u32>(ret));
            if (f) {
                std::fprintf(f, "%s %08x pde %08x pte %08x ret %08x\n", what,
                             static_cast<unsigned>(va), static_cast<unsigned>(out[0]),
                             static_cast<unsigned>(out[1]), static_cast<unsigned>(ret));
            }
        };
        static constexpr u32 FaultBanks[] = {0x0000, 0x4000, 0x8000, 0xc000, 0x10000, 0x14000};
        for (const u32 bank : FaultBanks) {
            u32 bif[2]{};   // BIF_INT_STAT, BIF_FAULT
            sgxdReadRegs(bank + 0x0c04, 2, bif);
            LOG_CRITICAL(Render, "sgxd: bank {:#07x} bif int {:08x} fault {:08x}", bank, bif[0],
                         bif[1]);
            if (f) {
                std::fprintf(f, "bank %05x bif_int %08x bif_fault %08x\n",
                             static_cast<unsigned>(bank), static_cast<unsigned>(bif[0]),
                             static_cast<unsigned>(bif[1]));
            }
            if (bif[1] & ~0xfffu) {
                translate("fault", bif[1] & ~0xfffu);
            }
        }
        for (u32 n = 0; n < draw_count; n++) {
            const SgxdRecipeDrawPrograms& p = programs[n];
            translate("vspds", p.vspds);
            translate("vsec", p.vsec);
            translate("fprim", p.fprim);
            if (p.indices) {
                translate("indices", p.indices);
            }
        }
        if (f) {
            std::fprintf(f, "fault dump %d\n", faulted);
            std::fclose(f);
        }
    }
    for (const Store& store : stores) {
        SgxdCacheRange(store.cpu, store.bytes, SGXD_CACHE_CLEAN_INVALIDATE,
                       CacheSite::TransferAfter);
    }
}

void SgxdBackend::RenderAsync(Recipe& r, const u32 touches[6]) {
    char failed[200] = "";
    if (sgxd_session_render_overlap(&r.recipe, touches, failed, sizeof(failed)) != 0) {
        Fault(r, failed);
    }
    // Its list ends with its 3D, nothing left pending: that list's fence is the render's.
    r.fence = sgxd_chain_fence();
    SgxdTrace("kick", r.width);
    sent_renders[sent_renders_next++ % sent_renders.size()] = {
        sgxd_mkq_jobs_sent(), r.width, r.height, static_cast<u32>(r.kind), 0, "render"};
}

void SgxdBackend::RenderSync(Recipe& r, u32* ta_us, u32* d3_us) {
    char failed[200] = "";
    SgxdSessionRender render{};
#ifdef VITA_DIAGNOSTICS
    // gpucycles probe: each core's 3D (0xe90), TA (0xe8c) and memory stall (0xec4) cycles around
    // synchronous renders 60 to 180.
    static constexpr u32 cycle_regs[] = {0xe90, 0xe8c, 0xec4};
    static const bool gpu_cycles = SgxdFlag("gpucycles");
    static u32 cycle_passes = 0;
    const bool cycles = gpu_cycles && cycle_passes++ >= 60 && cycle_passes <= 180;
    std::array<SgxdOp, 4 * std::size(cycle_regs)> before{}, after{};
    const auto read_cycles = [](auto& ops) {
        for (u32 c = 0; c < 4; c++) {
            for (u32 k = 0; k < std::size(cycle_regs); k++) {
                ops[c * std::size(cycle_regs) + k] =
                    SgxdOp{SGXD_OP_READ, (c + 2) * 0x4000 + cycle_regs[k]};
            }
        }
        ASSERT_MSG(sgxdSubmit(ops.data(), static_cast<uint32_t>(ops.size())) ==
                       static_cast<int>(ops.size()),
                   "sgxd: gpucycles reads failed");
    };
    if (cycles) {
        read_cycles(before);
    }
    // gpuperf=G probe: the SGX's counters around the same renders, set up as Sony's driver does for
    // Razor's groups. G: 1 USSE processing and texture reads, 2 ISP pixels before and after hidden
    // surface removal, 3 ISP parameter fetch.
    static const u32 perf_group = SgxdFlagValue("gpuperf");
    static constexpr u32 PerfGroups[4][8] = {{3, 3, 0, 0, 0, 0, 0, 0},
                                             {0, 0, 0, 0, 0x64, 0x44, 1, 0},
                                             {0, 0, 0, 0x2c, 0x2c, 0x2c, 0, 0},
                                             {0, 0, 0, 0, 0, 0x36, 0x36, 0}};
    static constexpr u32 PerfBits[4][8] = {{0, 8, 0x1c, 0x1c, 0x1c, 8, 0x12, 0},
                                           {0x1c, 0x1c, 0x1c, 0x1c, 0, 0, 0, 0},
                                           {0x1c, 0x1c, 0x1c, 0xf, 0xa, 1, 0x18, 0},
                                           {0x1c, 0x1c, 0x1c, 0x1c, 0x1c, 0x12, 6, 0}};
    static u32 perf_passes = 0;
    const bool perf = perf_group != 0 && perf_group < 4 && perf_passes++ >= 60 &&
                      perf_passes <= 180;
    std::array<SgxdOp, 32> perf_before{}, perf_after{};
    const auto read_perf = [](auto& ops) {
        for (u32 c = 0; c < 4; c++) {
            for (u32 k = 0; k < 8; k++) {
                ops[c * 8 + k] = SgxdOp{SGXD_OP_READ, (c + 2) * 0x4000 + 0x44 + k * 4};
            }
        }
        ASSERT_MSG(sgxdSubmit(ops.data(), static_cast<uint32_t>(ops.size())) ==
                       static_cast<int>(ops.size()),
                   "sgxd: gpuperf reads failed");
    };
    if (perf) {
        if (static bool set = false; !std::exchange(set, true)) {
            std::vector<SgxdOp> ops{{SGXD_OP_WRITE, 0x40, 0xff},
                                    {SGXD_OP_WRITE, 0xe80, 1},
                                    {SGXD_OP_WRITE, 0xe80, 0},
                                    {SGXD_OP_WRITE, 0x78, 1}};
            const u32* g = PerfGroups[perf_group];
            const u32* b = PerfBits[perf_group];
            for (u32 c = 0; c < 4; c++) {
                for (u32 n = 0; n < 4; n++) {
                    ops.push_back({SGXD_OP_WRITE, (c + 2) * 0x4000 + 0x90 + n * 4,
                                   g[2 * n] | b[2 * n] << 8 | g[2 * n + 1] << 16 |
                                       b[2 * n + 1] << 24});
                }
            }
            // gpuperfsum=MASK: SUM_MUX bits (EUR_CR_PERF 15..8): a counter adds its signal's value
            // instead of counting events.
            ops.push_back({SGXD_OP_WRITE, 0x40, (SgxdFlagValue("gpuperfsum") & 0xffu) << 8});
            ASSERT_MSG(sgxdSubmit(ops.data(), static_cast<uint32_t>(ops.size())) ==
                           static_cast<int>(ops.size()),
                       "sgxd: gpuperf's set-up failed");
            LOG_INFO(Render, "sgxd: gpuperf: the performance counters on, group {}", perf_group);
        }
        read_perf(perf_before);
    }
#endif
    const bool ok =
        sgxd_session_phase(&r.recipe, sgxd_session_slc_inval, "slc", failed, sizeof(failed)) ==
            0 &&
        sgxd_session_render(&r.recipe, &render, failed, sizeof(failed)) == 0;
    if (!ok) {
        Fault(r, failed);
    }
#ifdef VITA_DIAGNOSTICS
    if (cycles) {
        read_cycles(after);
        std::string line;
        for (u32 k = 0; k < std::size(cycle_regs); k++) {
            line += k == 0 ? " 3D" : k == 1 ? "; TA" : "; stall";
            for (u32 c = 0; c < 4; c++) {
                const std::size_t i = c * std::size(cycle_regs) + k;
                line += fmt::format(" {}", after[i].value - before[i].value);
            }
        }
        LOG_INFO(Render, "sgxd: gpucycles pass {} ({}x{}, {} draws, TA {} us, 3D {} us):{}",
                 passes, r.width, r.height, draw_count, render.ta_us, render.d3_us, line);
    }    if (perf) {
        read_perf(perf_after);
        std::string line;
        for (u32 c = 0; c < 4; c++) {
            line += fmt::format("{} core {}:", c ? ";" : "", c);
            for (u32 k = 0; k < 8; k++) {
                line += fmt::format(" {}", perf_after[c * 8 + k].value - perf_before[c * 8 + k].value);
            }
        }
        LOG_INFO(Render, "sgxd: gpuperf pass {} ({}x{}, {} draws, TA {} us, 3D {} us): {}", passes,
                 r.width, r.height, draw_count, render.ta_us, render.d3_us, line);
    }
#endif
    SgxdTrace("sync_end");
    if (ta_us) {
        *ta_us = render.ta_us;
    }
    if (d3_us) {
        *d3_us = render.d3_us;
    }
}

#ifdef VITA_DIAGNOSTICS
// The file's layout: "USSE", 1, the programs, the heap's bytes, then each program's
// bank, offset (DOUTU's, from the bank's base), bytes and place in the heap.
void SgxdBackend::WriteCode() {
    if (std::FILE* f = std::fopen("host0:azahar-sgxd-code.bin", "wb"); f && !recipes.empty()) {
        const Recipe& first = *recipes.front();
        const u32 head[4] = {0x45535355, 1, static_cast<u32>(placed_code.size()), code_used};
        std::fwrite(head, 4, 4, f);
        std::fwrite(placed_code.data(), sizeof(PlacedCode), placed_code.size(), f);
        std::fwrite(first.memory + first.code_begin, 1, code_used, f);
        std::fclose(f);
        LOG_CRITICAL(Render, "sgxd: {} USSE programs ({} KiB of code) written to host0",
                     placed_code.size(), code_used >> 10);
    }
}
#endif

void SgxdBackend::Fault(const Recipe& r, const char* failed) {
    for (u32 k = sent_renders_next > sent_renders.size() ? sent_renders_next - 64 : 0;
         k != sent_renders_next; k++) {
        const SentRender& sr = sent_renders[k % sent_renders.size()];
        LOG_CRITICAL(Render, "sgxd: sent render: job {}, {} {}x{} kind {}, {} draws", sr.job,
                     sr.what, sr.width, sr.height, sr.kind, sr.draws);
    }
    SgxdDrainKernelLog();
    static u32 dump[SGXD_DUMP_WORDS];
    const int dumped = sgxdDump(dump, SGXD_DUMP_WORDS);
    if (std::FILE* f = std::fopen("host0:azahar-sgxd-dump.bin", "wb")) {
        std::fwrite(dump, 4, dumped > 0 ? SGXD_DUMP_WORDS : 0, f);
        std::fclose(f);
    }
    if (std::FILE* f = std::fopen("host0:azahar-sgxd-block.bin", "wb")) {
        std::fwrite(r.memory, 1, r.size, f);
        std::fclose(f);
    }
    // The recipe whose VDM stream the TA reads (EUR_CR_MASTER_VDM_CTRL_STREAM_BASE), where that
    // is not r: a pass stopped in the TA while the render thread waits on a later one.
    u32 stream = 0;
    sgxdReadRegs(0x4238, 1, &stream);
    for (const auto& t : recipes) {
        if (t && t.get() != &r && sgxd_recipe_va(&t->recipe, SGXD_R_STREAM) == stream) {
            if (std::FILE* f = std::fopen("host0:azahar-sgxd-ta-block.bin", "wb")) {
                std::fwrite(t->memory, 1, t->size, f);
                std::fclose(f);
            }
            LOG_CRITICAL(Render,
                         "sgxd: the TA's stream {:#010x}: the {}x{} recipe's block {} KiB at GPU "
                         "{:#010x} written to host0",
                         stream, t->width, t->height, t->size >> 10, t->va);
        }
    }
    if (std::FILE* f = std::fopen("host0:azahar-sgxd-shared.bin", "wb")) {
        std::fwrite(shared, 1, shared_size, f);
        std::fclose(f);
    }
    LOG_CRITICAL(Render,
                 "sgxd: dump {:#x}; the {}x{} recipe's block {} KiB at GPU {:#010x} and the "
                 "shared block {} KiB at GPU {:#010x} written to host0",
                 static_cast<u32>(dumped), r.width, r.height, r.size >> 10, r.va,
                 shared_size >> 10, BlockVa);
#ifdef VITA_DIAGNOSTICS
    WriteCode();
#endif
    if (stream_hook) {
        stream_hook();
    }
    if (fault_hook) {
        fault_hook();
    }
    const int faulted = sgxdFaultDump(dump, SGXD_DUMP_WORDS);
    if (std::FILE* f = std::fopen("host0:azahar-sgxd-fault.bin", "wb")) {
        std::fwrite(dump, 4, faulted > 0 ? SGXD_DUMP_WORDS : 0, f);
        std::fclose(f);
    }
    std::FILE* f = std::fopen("host0:azahar-sgxd-fault.txt", "w");
    const auto translate = [f](const char* what, u32 va) {
        u32 out[2]{};
        const int ret = sgxdTranslate(va, out);
        LOG_CRITICAL(Render, "sgxd: {} {:#010x}: pde {:08x} pte {:08x} ({:#x})", what, va,
                     out[0], out[1], static_cast<u32>(ret));
        if (f) {
            std::fprintf(f, "%s %08x pde %08x pte %08x ret %08x\n", what,
                         static_cast<unsigned>(va), static_cast<unsigned>(out[0]),
                         static_cast<unsigned>(out[1]), static_cast<unsigned>(ret));
        }
    };
    static constexpr u32 FaultBanks[] = {0x0000, 0x4000, 0x8000, 0xc000, 0x10000, 0x14000};
    for (const u32 bank : FaultBanks) {
        u32 bif[2]{};   // BIF_INT_STAT, BIF_FAULT
        sgxdReadRegs(bank + 0x0c04, 2, bif);
        LOG_CRITICAL(Render, "sgxd: bank {:#07x} bif int {:08x} fault {:08x}", bank, bif[0],
                     bif[1]);
        if (f) {
            std::fprintf(f, "bank %05x bif_int %08x bif_fault %08x\n",
                         static_cast<unsigned>(bank), static_cast<unsigned>(bif[0]),
                         static_cast<unsigned>(bif[1]));
        }
        if (bif[1] & ~0xfffu) {
            translate("fault", bif[1] & ~0xfffu);
        }
    }
    for (u32 n = 0; n < draw_count; n++) {
        const SgxdRecipeDrawPrograms& p = programs[n];
        translate("vspds", p.vspds);
        translate("vsec", p.vsec);
        translate("fprim", p.fprim);
        if (p.indices) {
            translate("indices", p.indices);
        }
    }
    if (f) {
        std::fprintf(f, "fault dump %d\n", faulted);
        std::fclose(f);
    }
    UNREACHABLE_MSG("sgxd: pass {} of {} draws stopped: {}", passes, draw_count, failed);
}

void SgxdBackend::PlaceFixedPrograms() {
    if (blit_ready) {
        return;
    }
    u64 words[4];
    blit_vertex = sgxd_program_doutu0(0, PlaceCode(0, {words, sgxd_program_blit_vertex(words)}), 0);
    blit_fragment =
        sgxd_program_doutu0(1, PlaceCode(1, {words, sgxd_program_blit_fragment(words)}), 0);
    untile_fragment =
        sgxd_program_doutu0(1, PlaceCode(1, {words, sgxd_program_screen_fragment(words)}), 0);
    merge_vertex =
        sgxd_program_doutu0(0, PlaceCode(0, {words, sgxd_program_merge_vertex(words)}), 0);
    // The merge fragment: the blit fragment with an `and o0, pa4, pa5` before its end.
    sgxd_program_blit_fragment(words);
    Usse::Encoder e;
    e.Bitwise(Usse::BwOp::And, Usse::Reg::O(0), Usse::IOp::R(Usse::Reg::Pa(4)),
              Usse::IOp::R(Usse::Reg::Pa(5)));
    const u64 merge[3] = {words[0], e.Words()[0], words[2]};
    merge_fragment = sgxd_program_doutu0(1, PlaceCode(1, merge), 0);
    blit_ready = true;
}

SgxdRecipeDrawPrograms SgxdBackend::BlitPrograms(const Data& source, u32 width, u32 height) {
    PlaceFixedPrograms();
    // Two triangles over the target: x, y, z, w, a white colour (unused), u, v. Pixel row y
    // reads texel row y: both follow y the same way.
    constexpr u32 N = SGXD_PROGRAM_BLIT_VERTEX_WORDS;
    const Data vertices = AllocData(6 * N * 4);
    static constexpr float corner[6][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, -1}, {1, 1}, {-1, 1}};
    float* v = reinterpret_cast<float*>(vertices.cpu);
    for (u32 n = 0; n < 6; n++) {
        const float x = corner[n][0], y = corner[n][1];
        const float words[N] = {x, y, 0.5f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, (x + 1) / 2, (y + 1) / 2};
        std::memcpy(v + n * N, words, sizeof(words));
    }

    SgxdRecipeDrawPrograms p{};
    SgxdPdsProgram pds{};
    const SgxdVertexFetch fetch{vertices.gpu, N * 4, N, 0};
    [[maybe_unused]] const int vspds = sgxd_program_vertex_pds(&fetch, 1, blit_vertex, 0, &pds);
    DEBUG_ASSERT(vspds == 0);
    p.vspds = PlacePds(pds);
    p.vspds_units = pds.units;
    // The dummy secondaries at the code bases (the recipe's CV_DUMMY_VS and CF_DUMMY_FS).
    [[maybe_unused]] const int vsec =
        sgxd_program_secondary_pds(0, 0, sgxd_program_doutu0(0, 0, 0), &pds);
    DEBUG_ASSERT(vsec == 0);
    p.vsec = PlacePds(pds);
    p.vsec_units = pds.units;
    p.vsec_sa = 0;
    [[maybe_unused]] const int fsec =
        sgxd_program_secondary_pds(0, 0, sgxd_program_doutu0(1, 0, 0), &pds);
    DEBUG_ASSERT(fsec == 0);
    p.fsec = PlacePds(pds);
    p.fsec_units = pds.units;
    // The texel comes back unconverted (FCONV 0), the surface's word as it is.
    u32 tex[4];
    sgxd_recipe_texture_words(source.gpu, width, height, 0, tex);
    [[maybe_unused]] const int fprim = sgxd_program_blit_pixel_pds(blit_fragment, tex, &pds);
    DEBUG_ASSERT(fprim == 0);
    p.fprim = PlacePds(pds);
    p.fprim_units = pds.units;
    sgxd_program_vertex_dms(N, 0, 0, N, &p.vdm_w2, &p.vdm_w4, &p.vdm_w5);
    p.pixel_size = sgxd_program_pixel_dms(SGXD_PROGRAM_BLIT_PA, 0, 0);
    p.outselects = SGXD_PROGRAM_BLIT_OUTSELECTS;
    p.texsize = SGXD_PROGRAM_BLIT_TEXSIZE;
    const float viewport[6] = {width / 2.0f, width / 2.0f, height / 2.0f, height / 2.0f, 0.0f,
                               1.0f};
    std::memcpy(p.viewport, viewport, sizeof(viewport));
    p.indices = 0;
    p.primitive = SGXD_PROGRAM_TRIANGLES;
    return p;
}

void SgxdBackend::Blit(const Data& source, u32 width, u32 height, const Store& dest, u32* ta_us,
                       u32* d3_us) {
    OpenPass(dest.gpu, width, height, dest.pack, 0, 1.0f);
    // Uncached memory (VRAM) comes without a CPU range and needs no cache work.
    if (dest.bytes != 0) {
        stores.push_back(dest);
    }
    const SgxdRecipeDrawPrograms p = BlitPrograms(source, width, height);
    // ISP A: opaque, DCMP ALWAYS, DWRITEDIS, BPRES; no culling.
    const u32 isp = 7u << 22 | 1u << 20 | 1u << 9;
    Draw(SgxdRecipeDraw{0, 6, isp, SGXD_R_FPRIM, 0, 0, nullptr}, p);
    EndPass(ta_us, d3_us);
}

namespace {

constexpr u32 QuadVertexWords = 6 * SGXD_PROGRAM_BLIT_VERTEX_WORDS;

/// One quad as the blit's two triangles into v: target pixels [x0, x1) x [y0, y1) of a
/// width x height viewport, and the texture coordinate at each corner (00 at x0, y0; 10 at
/// x1, y0; 01 at x0, y1; 11 at x1, y1).
void PutQuad(float* v, u32 width, u32 height, float x0, float y0, float x1, float y1,
             const float (&uv)[4][2]) {
    constexpr u32 N = SGXD_PROGRAM_BLIT_VERTEX_WORDS;
    const float corner[4][2] = {{x0, y0}, {x1, y0}, {x0, y1}, {x1, y1}};
    static constexpr u32 order[6] = {0, 1, 2, 1, 3, 2};
    for (u32 n = 0; n < 6; n++) {
        const u32 c = order[n];
        const float words[N] = {2 * corner[c][0] / width - 1,
                                2 * corner[c][1] / height - 1,
                                0.5f,
                                1.0f,
                                1.0f,
                                1.0f,
                                1.0f,
                                1.0f,
                                uv[c][0],
                                uv[c][1]};
        std::memcpy(v + n * N, words, sizeof(words));
    }
}

/// Where an untile writes a w x h image with rows `stride` texels apart: a PBE target of width x
/// height with rows target_stride apart, per_row image rows on each. A stride under 32 packs
/// 32 / w image rows side by side per 32-pixel row (the PBE takes multiples of 32). Swizzled:
/// the PBE's TWIDDLED layout, the same Morton order, y first.
struct UntileTarget {
    u32 width, height, stride, per_row;
    bool twiddled;
};

UntileTarget UntileTargetFor(u32 w, u32 h, u32 stride, bool swizzled = false) {
    if (swizzled) {
        DEBUG_ASSERT_MSG(w == h, "sgxd: a swizzled {}x{} untile: squares only", w, h);
        return {w, h, std::max(w, 32u), 1, true};
    }
    if (stride % 32 == 0) {
        DEBUG_ASSERT(stride >= w);
        return {w, h, stride, 1, false};
    }
    DEBUG_ASSERT_MSG(stride == w && w < 32, "sgxd: rows {} texels apart for a {}-wide image",
                     stride, w);
    return {32, h * w / 32, 32, 32 / w, false};
}

/// PutQuad for image rows [y0, y1), w texels wide, placed on target t: one quad when t's rows
/// are the image's, else a quad per row with the coordinates interpolated down the block.
/// Returns the quads written, each QuadVertexWords.
u32 PutRows(float* v, const UntileTarget& t, u32 w, u32 y0, u32 y1, const float (&uv)[4][2]) {
    if (t.per_row == 1) {
        PutQuad(v, t.width, t.height, 0, static_cast<float>(y0), static_cast<float>(w),
                static_cast<float>(y1), uv);
        return 1;
    }
    const auto lerp = [](const float (&a)[2], const float (&b)[2], float f, u32 i) {
        return a[i] + (b[i] - a[i]) * f;
    };
    for (u32 y = y0; y < y1; y++) {
        const float f0 = static_cast<float>(y - y0) / (y1 - y0);
        const float f1 = static_cast<float>(y + 1 - y0) / (y1 - y0);
        const float row[4][2] = {
            {lerp(uv[0], uv[2], f0, 0), lerp(uv[0], uv[2], f0, 1)},
            {lerp(uv[1], uv[3], f0, 0), lerp(uv[1], uv[3], f0, 1)},
            {lerp(uv[0], uv[2], f1, 0), lerp(uv[0], uv[2], f1, 1)},
            {lerp(uv[1], uv[3], f1, 0), lerp(uv[1], uv[3], f1, 1)},
        };
        const float x0 = static_cast<float>(y % t.per_row * w);
        const float ty = static_cast<float>(y / t.per_row);
        PutQuad(v + (y - y0) * QuadVertexWords, t.width, t.height, x0, ty, x0 + w, ty + 1, row);
    }
    return y1 - y0;
}

u32 TileRowQuads(const UntileTarget& t) {
    return t.per_row == 1 ? 1 : 8;
}

/// and the two flags its vertices depend on.
u64 GeometryKey(u32 step, u32 w, u32 h, u32 stride, bool alpha, bool swizzled) {
    return u64{step} << 56 | u64{w} << 42 | u64{h} << 28 | u64{stride} << 8 |
           (alpha ? 2u : 0u) | (swizzled ? 1u : 0u);
}

constexpr u32 MergeQuadWords = 6 * SGXD_PROGRAM_MERGE_VERTEX_WORDS;

void PutMergeQuad(float* v, u32 width, u32 height, float x0, float y0, float x1, float y1,
                  const float (&uv0)[4][2], const float (&uv1)[4][2]) {
    constexpr u32 N = SGXD_PROGRAM_MERGE_VERTEX_WORDS;
    const float corner[4][2] = {{x0, y0}, {x1, y0}, {x0, y1}, {x1, y1}};
    static constexpr u32 order[6] = {0, 1, 2, 1, 3, 2};
    for (u32 n = 0; n < 6; n++) {
        const u32 c = order[n];
        const float words[N] = {2 * corner[c][0] / width - 1,
                                2 * corner[c][1] / height - 1,
                                0.5f,
                                1.0f,
                                1.0f,
                                1.0f,
                                1.0f,
                                1.0f,
                                uv0[c][0],
                                uv0[c][1],
                                uv1[c][0],
                                uv1[c][1]};
        std::memcpy(v + n * N, words, sizeof(words));
    }
}

/// DOUTT words for a texture read point for point: DADJUST 31, MIPMAPCLAMP 15 (no mips), U
/// and V CLAMP; NORM. type 0 (SWIZZLED) takes log2 sizes, 3 (LINEAR) sizes less one.
void SideTextureWords(u32 type, u32 code, u32 swizzle, u32 width, u32 height, u32 va,
                      u32 words[4], u32 palette = 0) {
    const u32 sizes = type == 0 ? static_cast<u32>(std::countr_zero(width)) << 16 |
                                      static_cast<u32>(std::countr_zero(height))
                                : (width - 1) << 12 | (height - 1);
    words[0] = (code >> 7 & 1u) << 31 | 31u << 21 | 15u << 17 | 2u << 6 | 2u << 3;
    words[1] = type << 29 | (code & 0x1fu) << 24 | sizes;
    words[2] = va;
    words[3] = 1u << 31 | (swizzle & 7u) << 28 | (palette >> 6 & 0x03ffffffu);
}

} // namespace

void SgxdBackend::SideRender(const Data& vertices, std::span<const SideDraw> list, u32 dest_gpu,
                             u32 dest_stride, u32 width, u32 height, bool merge, bool twiddled) {
    // Inside a pass or not: the vertices are the caller's, in the textures pool or the pass data.
    ASSERT(!list.empty() && list.size() <= MaxDraws);
    DEBUG_ASSERT(dest_stride >= std::max(width, 32u) && dest_stride % 32 == 0 &&
                 width <= UntileSide && height <= UntileSide);
    Recipe& r = RecipeFor(UntileSide, UntileSide, true);
    PlaceFixedPrograms();
    const u32 N = merge ? SGXD_PROGRAM_MERGE_VERTEX_WORDS : SGXD_PROGRAM_BLIT_VERTEX_WORDS;

    SgxdRecipeDrawPrograms p{};
    SgxdPdsProgram pds{};
    const SgxdVertexFetch fetch{vertices.gpu, N * 4, N, 0};
    [[maybe_unused]] const int vspds =
        sgxd_program_vertex_pds(&fetch, 1, merge ? merge_vertex : blit_vertex, 0, &pds);
    DEBUG_ASSERT(vspds == 0);
    p.vspds = PlacePds(pds);
    p.vspds_units = pds.units;
    [[maybe_unused]] const int vsec =
        sgxd_program_secondary_pds(0, 0, sgxd_program_doutu0(0, 0, 0), &pds);
    DEBUG_ASSERT(vsec == 0);
    p.vsec = PlacePds(pds);
    p.vsec_units = pds.units;
    p.vsec_sa = 0;
    [[maybe_unused]] const int fsec =
        sgxd_program_secondary_pds(0, 0, sgxd_program_doutu0(1, 0, 0), &pds);
    DEBUG_ASSERT(fsec == 0);
    p.fsec = PlacePds(pds);
    p.fsec_units = pds.units;
    sgxd_program_vertex_dms(N, 0, 0, N, &p.vdm_w2, &p.vdm_w4, &p.vdm_w5);
    p.pixel_size = sgxd_program_pixel_dms(SGXD_PROGRAM_BLIT_PA, 0, 0);
    p.outselects = merge ? SGXD_PROGRAM_MERGE_OUTSELECTS : SGXD_PROGRAM_BLIT_OUTSELECTS;
    p.texsize = merge ? SGXD_PROGRAM_MERGE_TEXSIZE : SGXD_PROGRAM_BLIT_TEXSIZE;
    const float viewport[6] = {width / 2.0f, width / 2.0f, height / 2.0f, height / 2.0f,
                               0.0f,         1.0f};
    std::memcpy(p.viewport, viewport, sizeof(viewport));
    p.indices = 0;
    p.primitive = SGXD_PROGRAM_TRIANGLES;

    // The quads' right and bottom edges put the next tile column and row in the TA's lists,
    // where the background alone would be written past the target: rows stride apart, so the
    // column's tiles land on the next rows' first 32 texels (a 256x128 texture's texels 0-31 of
    // rows 1-127 would hold the background). The clip keeps the tiles
    // the target covers.
    const u32 clip =
        SGXD_RECIPE_CLIP(0, 0, std::max(width, 32u) / 32 - 1, std::max(height, 32u) / 32 - 1);
    const u32 isp = 7u << 22 | 1u << 20 | 1u << 9;
    untile_draws.resize(list.size());
    untile_programs.assign(list.size(), p);
    for (std::size_t d = 0; d < list.size(); d++) {
        const SideDraw& sd = list[d];
        // the word unconverted.
        [[maybe_unused]] const int fprim =
            merge          ? sgxd_program_merge_pixel_pds(merge_fragment, sd.tex, sd.tex1, &pds)
            : sd.converted ? sgxd_program_screen_pixel_pds(untile_fragment, sd.tex, &pds)
                           : sgxd_program_blit_pixel_pds(blit_fragment, sd.tex, &pds);
        DEBUG_ASSERT(fprim == 0);
        untile_programs[d].fprim = PlacePds(pds);
        untile_programs[d].fprim_units = pds.units;
        untile_draws[d] =
            SgxdRecipeDraw{sd.first, sd.count, isp, SGXD_R_FPRIM, 0, clip, &untile_programs[d]};
    }
    sgxd_recipe_set_target(&r.recipe, r.memory, dest_gpu, dest_stride);
    // MEMLAYOUT (26..27) 2: TWIDDLED.
    sgxd_recipe_set_target_pack(&r.recipe, r.memory,
                                SGXD_RECIPE_PACK_U8888 | (twiddled ? 2u << 26 : 0u));
    [[maybe_unused]] const int taken = sgxd_recipe_set_draws(
        &r.recipe, r.memory, untile_draws.data(), static_cast<unsigned>(list.size()));
    DEBUG_ASSERT_MSG(taken == 0, "sgxd: the side render's {} draws were not taken", list.size());
    sgxd_recipe_restage(&r.recipe, r.memory);
#ifdef VITA_DIAGNOSTICS
    static const bool sync_side = SgxdFlag("syncside");
    if (sync_side && (replay_sync || SgxdFlag("syncpasses"))) {
        u32 ta_us = 0, d3_us = 0;
        RenderSync(r, &ta_us, &d3_us);
        r.fence = FenceAll();
        side_times[0]++;
        side_times[1] += ta_us;
        side_times[2] += d3_us;
        static const u64 log_at = u64{SgxdFlagValue("passlog")} * 1000000;
        const u64 now = sceKernelGetProcessTimeWide();
        if (log_at != 0 && now >= log_at && now < log_at + 1000000) {
            LOG_INFO(Render, "perf: side render {}x{}: {} draws, TA {} us, 3D {} us", width, height,
                     list.size(), ta_us, d3_us);
        }
        NoteSent("side", static_cast<u32>(list.size()));
        return;
    }
#endif
    // Queued like a pass. Its reads and writes are not tracked: whatever a fill or copy touches
    // next waits for it (RenderBefore).
    static constexpr u32 everything[6] = {0, ~0u, 0, 0, 0, 0};
#ifdef VITA_DIAGNOSTICS
    sgxd_mkq_label_next =
        sgxd_mkq_label("side", reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
#endif
    RenderAsync(r, everything);
    NoteSent("side", static_cast<u32>(list.size()));
}

void SgxdBackend::StreamOut(const SgxdRecipeDrawPrograms& programs, u32 count, StreamStage stage,
                            bool wait) {
    DEBUG_ASSERT(in_pass && count != 0 && count % 3 == 0);
    for (const CachedRange& range : cached) {
        SgxdCacheRange(range.cpu, range.bytes, SGXD_CACHE_CLEAN, CacheSite::Vertices);
    }
    cached.clear();
    auto& queued = stream_draws[static_cast<u32>(stage)];
    if (queued.size() == StreamMaxDraws) {
        // A full render: the A0s queued go first, so this draw's A0 is sent before its A1.
        FlushStreamOut(false);
    }
    queued.push_back({programs, count});
    if (wait && stage == StreamStage::A1) {
        FlushStreamOut(true);
    }
}

void SgxdBackend::FlushStreamOut(bool wait) {
    for (auto& queued : stream_draws) {
        if (!queued.empty()) {
            StreamRender(queued, wait);
        }
    }
}

void SgxdBackend::StreamRender(std::vector<StreamDraw>& queued, bool wait) {
    DEBUG_ASSERT(in_pass && !queued.empty() && queued.size() <= StreamMaxDraws);
    Recipe& r = RecipeFor(UntileSide, UntileSide, true);
    PlaceFixedPrograms();
    if (stream_target.cpu == nullptr) {
        stream_target = AllocSurface(32, 32);
    }
    SgxdPdsProgram pds{};
    [[maybe_unused]] const int fsec =
        sgxd_program_secondary_pds(0, 0, sgxd_program_doutu0(1, 0, 0), &pds);
    DEBUG_ASSERT(fsec == 0);
    p.fsec = PlacePds(pds);
    p.fsec_units = pds.units;
    const u32 tex[4] = {};
    [[maybe_unused]] const int fprim = sgxd_program_blit_pixel_pds(blit_fragment, tex, &pds);
    DEBUG_ASSERT(fprim == 0);
    p.fprim = PlacePds(pds);
    p.fprim_units = pds.units;
    p.pixel_size = sgxd_program_pixel_dms(SGXD_PROGRAM_BLIT_PA, 0, 0);
    const float viewport[6] = {16.0f, 16.0f, 16.0f, 16.0f, 0.0f, 1.0f};
    constexpr u32 clip = SGXD_RECIPE_CLIP(0, 0, 0, 0);
    untile_programs.resize(queued.size());
    untile_draws.resize(queued.size());
    for (std::size_t d = 0; d < queued.size(); d++) {
        SgxdRecipeDrawPrograms& p = untile_programs[d];
        p = queued[d].programs;
        p.fsec = fsec.va;
        p.fsec_units = fsec.units;
        p.fprim = fprim.va;
        p.fprim_units = fprim.units;
        p.pixel_size = sgxd_program_pixel_dms(SGXD_PROGRAM_BLIT_PA, 0, 0);
        std::memcpy(p.viewport, viewport, sizeof(viewport));
        p.primitive = SGXD_PROGRAM_TRIANGLES;
        untile_draws[d] = {0, queued[d].count, SGXD_RECIPE_ISP_NEVER, SGXD_R_FPRIM, 0, clip, &p};
    }
    sgxd_recipe_set_target(&r.recipe, r.memory, stream_target.gpu, 32);
    sgxd_recipe_set_target_pack(&r.recipe, r.memory, SGXD_RECIPE_PACK_U8888);
    [[maybe_unused]] const int taken = sgxd_recipe_set_draws(
        &r.recipe, r.memory, untile_draws.data(), static_cast<unsigned>(queued.size()));
    DEBUG_ASSERT_MSG(taken == 0, "sgxd: the {} stream-out draws were not taken", queued.size());
    sgxd_recipe_restage(&r.recipe, r.memory);
    [[maybe_unused]] const u32 draws = static_cast<u32>(queued.size());
#ifdef VITA_DIAGNOSTICS
    stream_draws_sent += draws;
    stream_renders_sent++;
#endif
    queued.clear();
    if (wait) {
        RenderSync(r, nullptr, nullptr);
        return;
    }
    // As SideRender's: what it stores is pass data, read by the pass's own TA after it.
    static constexpr u32 everything[6] = {0, ~0u, 0, 0, 0, 0};
#ifdef VITA_DIAGNOSTICS
    sgxd_mkq_label_next =
        sgxd_mkq_label(&queued == &stream_draws[0] ? "stream-out A0" : "stream-out A1", 0);
#endif
    RenderAsync(r, everything);
    NoteSent(&queued == &stream_draws[0] ? "stream-out A0" : "stream-out A1", draws);
}

void SgxdBackend::Untile(const UntileSource& src, u32 dest_gpu, u32 dest_stride, bool swizzled) {
    const u32 w = src.width, h = src.height;
    DEBUG_ASSERT_MSG(w >= 8 && h >= 8 && w <= UntileSide && h <= UntileSide &&
                         (w & (w - 1)) == 0 && (h & (h - 1)) == 0,
                     "sgxd: a {}x{} texture: the untile takes powers of two from 8 to {}", w, h,
                     UntileSide);
    if (src.code == UntileEtc1 || src.code == UntileEtc1A4) {
        UntileEtc1Blocks(src, dest_gpu, dest_stride, swizzled);
        return;
    }
    // Tile rows per band: the band's SWIZZLED texture is 8 x (rows * w) texels, at most 4096.
    const u32 tile_rows = h / 8;
    const u32 band_rows = std::min(tile_rows, 4096 / w);
    const u32 bands = tile_rows / band_rows;
    const u32 band_texels = band_rows * w; // the band's SWIZZLED height
    const UntileTarget t = UntileTargetFor(w, h, dest_stride, swizzled);
    const u32 quads = t.per_row == 1 ? 1 : 8; // a tile row's
    const Data vertices = AllocData(tile_rows * quads * QuadVertexWords * 4);
    float* v = reinterpret_cast<float*>(vertices.cpu);
    // Tile row ty holds PICA rows 8ty..8ty+7, image rows 8ty..8ty+7. Along the quad, image
    // x is the band's SWIZZLED row (ty's local row) * w + x, image y the column: pixel
    // centres land on texel centres.
    for (u32 ty = 0; ty < tile_rows; ty++) {
        const float v0 = static_cast<float>((ty % band_rows) * w) / band_texels;
        const float v1 = static_cast<float>((ty % band_rows + 1) * w) / band_texels;
        const float uv[4][2] = {{0, v0}, {0, v1}, {1, v0}, {1, v1}};
        PutRows(v + ty * quads * QuadVertexWords, t, w, 8 * ty, 8 * (ty + 1), uv);
    }
    // More than one band only from 512 texels wide, where a band is 8 or 4 tile rows: its
    // first index is a multiple of 8, as a draw's must be.
    std::vector<SideDraw> list(bands);
    for (u32 b = 0; b < bands; b++) {
        SideTextureWords(0, src.code, src.swizzle, 8, band_texels,
                         src.gpu + b * band_texels * src.bits, list[b].tex, src.palette);
        list[b].converted = true;
        list[b].first = b * band_rows * quads * 6;
        list[b].count = band_rows * quads * 6;
    }
    SideRender(vertices, list, dest_gpu, t.stride, t.width, t.height, false, t.twiddled);
}

void SgxdBackend::UntileEtc1Blocks(const UntileSource& src, u32 dest_gpu, u32 dest_stride,
                                   bool swizzled) {
    const u32 w = src.width, h = src.height;
    const bool alpha = src.code == UntileEtc1A4;
    // A PICA ETC1 texture is 8x8 tiles of four 4x4 blocks, x first, each a little-endian u64 with
    // the colour word second. The SGX's ETC1 (0x84) reads the colour word first and SWIZZLED orders
    // blocks y first; ETC1A4 puts each block's alpha u64 before it.
    // Pass 1 copies the colour words raw into a 128-word-wide scratch, a column quad per word: word
    // d takes the tile's word perm[d] (each block's words swapped, blocks 1 and 2 swapped).
    // Pass 2: each tile row is then a SWIZZLED ETC1 texture of w x 8, decoded into the host copy
    // (for ETC1A4 into a colour copy UntileEtc1A4Merge joins with the alpha).
    static constexpr u32 kPerm[8] = {1, 0, 5, 4, 3, 2, 7, 6};
    static constexpr u32 kPermA4[8] = {3, 2, 11, 10, 7, 6, 15, 14};
    const u32* const perm = alpha ? kPermA4 : kPerm;
    const u32 tile_words = alpha ? 16 : 8;
    constexpr u32 ScratchWidth = 128;
    const u32 source_width = ScratchWidth / 8 * tile_words;
    const u32 words = w * h / 64 * tile_words;
    const u32 rows = std::max(1u, words / source_width);
    const u32 scratch_rows = std::max(rows, 32u);
    DEBUG_ASSERT(rows <= UntileSide);
    const u32 scratch_bytes = ScratchWidth * scratch_rows * 4;
    const Data scratch = AllocTexture(scratch_bytes);

    bool fresh = false;
    const Data swap = GeometryVertices(GeometryKey(2, w, h, 0, alpha, false),
                                       ScratchWidth * QuadVertexWords * 4, fresh);
    float* v = reinterpret_cast<float*>(swap.cpu);
    for (u32 x = 0; fresh && x < ScratchWidth; x++) {
        const float u = ((x / 8) * tile_words + perm[x % 8] + 0.5f) / source_width;
        const float uv[4][2] = {{u, 0}, {u, 0}, {u, 1}, {u, 1}};
        PutQuad(v + x * QuadVertexWords, ScratchWidth, rows, static_cast<float>(x), 0,
                static_cast<float>(x + 1), static_cast<float>(rows), uv);
    }
    SideDraw pass1{};
    SideTextureWords(3, 0x0c, 0, source_width, rows, src.gpu, pass1.tex);
    pass1.converted = false;
    pass1.first = 0;
    pass1.count = ScratchWidth * 6;
    SideRender(swap, {&pass1, 1}, scratch.gpu, ScratchWidth, ScratchWidth, rows);

    const u32 colour_stride = std::max(w, 32u);
    const u32 colour_bytes = colour_stride * std::max(h, 32u) * 4;
    const Data colour = alpha ? AllocTexture(colour_bytes) : Data{};
    // A draw's first index is a multiple of 8 (the VDM takes the index list's address in
    // 16-byte units): a tile row's vertices take 8 slots for one quad (6 used), 48 for the
    // 8 of packed rows.
    const UntileTarget t =
        UntileTargetFor(w, h, alpha ? colour_stride : dest_stride, !alpha && swizzled);
    const u32 slots = t.per_row == 1 ? 8 : 48;
    const u32 tile_rows = h / 8;
    const Data decode =
        GeometryVertices(GeometryKey(3, w, h, alpha ? colour_stride : dest_stride, alpha,
                                     !alpha && swizzled),
                         tile_rows * slots * SGXD_PROGRAM_BLIT_VERTEX_WORDS * 4, fresh);
    v = reinterpret_cast<float*>(decode.cpu);
    std::vector<SideDraw> list(tile_rows);
    for (u32 ty = 0; ty < tile_rows; ty++) {
        static constexpr float uv[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
        const u32 quads = fresh ? PutRows(v + ty * slots * SGXD_PROGRAM_BLIT_VERTEX_WORDS, t, w,
                                          8 * ty, 8 * (ty + 1), uv)
                                : TileRowQuads(t);
        SideTextureWords(0, UntileEtc1, 0, w, 8, scratch.gpu + ty * (w / 8) * 32, list[ty].tex);
        list[ty].converted = true;
        list[ty].first = ty * slots;
        list[ty].count = quads * 6;
    }
    SideRender(decode, list, alpha ? colour.gpu : dest_gpu, t.stride, t.width, t.height, false,
               t.twiddled);
    FreeTexture(scratch, scratch_bytes);
    if (alpha) {
        UntileEtc1A4Merge(src, colour.gpu, colour_stride, dest_gpu, dest_stride, swizzled);
        FreeTexture(colour, colour_bytes);
    }
}

void SgxdBackend::UntileEtc1A4Merge(const UntileSource& src, u32 colour_gpu, u32 colour_stride,
                                    u32 dest_gpu, u32 dest_stride, bool swizzled) {
    const u32 w = src.width, h = src.height;
    // The alpha source viewed LINEAR is a tile row per row, 2w words (LINEAR's rows round up to 8).
    // A block's alpha u64 holds 4 bits a texel at bit 4 * (4x + y).
    // Pass A gathers each block row's alpha words in x order: target word D = (w / 2) * (by & 1) +
    // 2 * bx + i1 holds block (bx, by)'s word i1, read raw from the tile row's word
    // 16 (bx >> 1) + 4 (bx & 1) + 8 (by & 1) + i1.
    const u32 tile_rows = h / 8;
    const u32 alpha_stride = std::max(w, 32u);
    const u32 alpha_bytes = alpha_stride * std::max(tile_rows, 32u) * 4;
    const Data alphas = AllocTexture(alpha_bytes);
    bool fresh = false;
    const Data gather =
        GeometryVertices(GeometryKey(4, w, h, 0, true, false), w * QuadVertexWords * 4, fresh);
    float* v = reinterpret_cast<float*>(gather.cpu);
    for (u32 d = 0; fresh && d < w; d++) {
        const u32 p = d / (w / 2), bx = d % (w / 2) / 2, i1 = d % 2;
        const u32 s = 16 * (bx >> 1) + 4 * (bx & 1) + 8 * p + i1;
        const float u = (s + 0.5f) / (2 * w);
        const float uv[4][2] = {{u, 0}, {u, 0}, {u, 1}, {u, 1}};
        PutQuad(v + d * QuadVertexWords, w, tile_rows, static_cast<float>(d), 0,
                static_cast<float>(d + 1), static_cast<float>(tile_rows), uv);
    }
    SideDraw pass{};
    SideTextureWords(3, 0x0c, 0, 2 * w, tile_rows, src.gpu, pass.tex);
    pass.converted = false;
    pass.first = 0;
    pass.count = w * 6;
    SideRender(gather, {&pass, 1}, alphas.gpu, alpha_stride, w, tile_rows);

    // The merge: dest = colour AND the alpha read as P4 through merge_palette ((17n) << 24 |
    // 0xffffff). Texel (x, y)'s nibble in its block row is 4x + 2 ((y >> 1) & 1) + (y & 1); a block
    // row is 4w nibbles, one P4 row, or for w < 32 the odd block row 4w nibbles into a tile row's.
    // A row quad per y.
    if (!merge_palette.cpu) {
        merge_palette = AllocTexture(0x1000);
        u32* const entries = reinterpret_cast<u32*>(merge_palette.cpu);
        for (u32 n = 0; n < 16; n++) {
            entries[n] = (n * 17) << 24 | 0xffffffu;
        }
    }
    const bool block_rows = alpha_stride == w;
    const u32 p4_width = block_rows ? 4 * w : 8 * alpha_stride;
    const u32 p4_rows = block_rows ? h / 4 : tile_rows;
    const UntileTarget t = UntileTargetFor(w, h, dest_stride, swizzled);
    const Data rows = GeometryVertices(GeometryKey(5, w, h, dest_stride, true, swizzled),
                                       h * MergeQuadWords * 4, fresh);
    v = reinterpret_cast<float*>(rows.cpu);
    for (u32 y = 0; fresh && y < h; y++) {
        const u32 row = block_rows ? y >> 2 : y >> 3;
        const u32 base = block_rows ? 0 : 4 * w * (y >> 2 & 1);
        const float k = static_cast<float>(base + 2 * (y >> 1 & 1) + (y & 1));
        const float u1a = (k - 1.5f) / p4_width, u1b = (4.0f * w + k - 1.5f) / p4_width;
        const float v1 = (row + 0.5f) / p4_rows;
        const float u0b = static_cast<float>(w) / colour_stride;
        const float v0a = static_cast<float>(y) / h, v0b = static_cast<float>(y + 1) / h;
        const float uv0[4][2] = {{0, v0a}, {u0b, v0a}, {0, v0b}, {u0b, v0b}};
        const float uv1[4][2] = {{u1a, v1}, {u1b, v1}, {u1a, v1}, {u1b, v1}};
        const float x0 = static_cast<float>(y % t.per_row * w);
        const float ty = static_cast<float>(y / t.per_row);
        PutMergeQuad(v + y * MergeQuadWords, t.width, t.height, x0, ty, x0 + w, ty + 1, uv0,
                     uv1);
    }
    SideDraw merge{};
    SideTextureWords(3, 0x0c, 0, colour_stride, h, colour_gpu, merge.tex);
    SideTextureWords(3, 0x94, 0, p4_width, p4_rows, alphas.gpu, merge.tex1, merge_palette.gpu);
    merge.first = 0;
    merge.count = h * 6;
    SideRender(rows, {&merge, 1}, dest_gpu, t.stride, t.width, t.height, true, t.twiddled);
    FreeTexture(alphas, alpha_bytes);
}

} // namespace SgxdRenderer
