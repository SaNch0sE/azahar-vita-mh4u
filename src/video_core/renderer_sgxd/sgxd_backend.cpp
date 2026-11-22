// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "common/assert.h"
#include "common/logging/log.h"
#include "video_core/renderer_gxm/gxm_flags.h"
#include "video_core/renderer_gxm/sgxd_backend.h"
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
}

SgxdBackend::Recipe& SgxdBackend::RecipeFor(u32 width, u32 height) {
    Recipe* oldest = nullptr;
    for (const auto& r : recipes) {
        if (r->width == width && r->height == height) {
            return *r;
        }
    }
    auto made = std::make_unique<Recipe>();
    Recipe& r = *made;
    r.width = width;
    r.height = height;
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

SgxdRecipeDrawPrograms SgxdBackend::BlitPrograms(const Data& source, u32 width, u32 height) {
    if (!blit_ready) {
        u64 words[4];
        blit_vertex = sgxd_program_doutu0(0, PlaceCode(0, {words, sgxd_program_blit_vertex(words)}), 0);
        blit_fragment =
            sgxd_program_doutu0(1, PlaceCode(1, {words, sgxd_program_blit_fragment(words)}), 0);
        blit_ready = true;
    }
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

} // namespace SgxdRenderer
