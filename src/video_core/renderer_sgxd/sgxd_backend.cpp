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
#include "common/hash.h"
#include "common/logging/log.h"
#include "common/named_thread.h"
#include "common/process_clock.h"
#include "core/memory.h"
#include "video_core/renderer_sgxd/sgxd_flags.h"
#include "video_core/renderer_sgxd/sgxd_backend.h"
#include "video_core/renderer_sgxd/untile_geometry.h"
#include "video_core/renderer_sgxd/vprof.h"
#include "video_core/renderer_sgxd/usse/usse_encoder.h"
#include "video_core/shader/generator/profile.h"

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include "sgxd.h"
#include "sgxd_mkq.h"
#include "sgxd_ptla.h"
#include "sgxd_session.h"

namespace SgxdRenderer {

using namespace UntileGeometry;
static_assert(SgxdBackend::UntileSide == UntileGeometry::UntileSide);

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
// With the shared parameter buffer, every block within 256 MiB of the DPM requestor base.
constexpr u32 RecipesEnd = 0x11000000;
constexpr u32 SurfacesVa = 0x48000000;
constexpr u32 SurfacesSize = 8u << 20;
// The guest textures' host copies, after the surfaces. They are U8888, 8 times an ETC1
// texture's bytes: a game's open pass read 42 textures in 14 MiB.
constexpr u32 TexturesVa = SurfacesVa + SurfacesSize;
constexpr u32 TexturesSize = 40u << 20;
constexpr u32 MicrokernelVa = 0x00400000;
constexpr u32 Pages = 418;
constexpr u32 MaxIndices = SGXD_RECIPE_MAX_INDICES;
// The code heap follows each layout. It must stay within DOUTU's reach of CV and CF (EXE, 20
// bits of 8 bytes: 8 MiB). The PDS arena and the pass data make the shared block.
constexpr u32 CodeSize = SgxdBackend::CodeBytes;
// The PDS arena: two halves, each draw's programs placed in the current one. A pass start past half
// of it flips to the other once no render reads it (FlipPdsHalf). No program is kept for good: a
// pixel PDS names its code's address, which a flush or the second tier moves.
constexpr u32 PdsHalfSize = 2u << 20;
constexpr u32 PdsSize = 2 * PdsHalfSize;
// The PDS keeps running a program at an address it has run, whatever is invalidated, so a half is
// never written again at an address it ran under: each use of a half takes the next window, the
// same memory mapped again. The GPU idles and the PDS resets only when the windows run out.
constexpr u32 PdsWindowVa = 0x50000000;
constexpr u32 PdsWindows = 64;
constexpr u32 DataSize = SgxdBackend::PassDataBytes;
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

SgxdBackend* drain_owner;

} // Anonymous namespace

SgxdBackend::SgxdBackend() {
    // SceShell's CDRAM for the recipes: sgxd holds the GPU, and its release copies the shell's
    // contents back.
    u32 shell_va = 0;
    const int shell_took = azaharShellCdram(&shell_va);
    ASSERT_MSG(shell_took == 0 && shell_va != 0, "sgxd: the shell's CDRAM was not ours ({:#010x})",
               static_cast<u32>(shell_took));
    shell_cdram = reinterpret_cast<u8*>(shell_va);
    LOG_INFO(Render, "sgxd: the shell's {} MiB of CDRAM at {:#010x}: {} recipe slots",
             ShellCdramSize >> 20, shell_va, ShellSlots);
    surfaces = MapBlock("azahar-sgxd-surfaces", SurfacesVa, SurfacesSize, surfaces_block);
    surfaces_size = SurfacesSize;
    textures = MapBlock("azahar-sgxd-textures", TexturesVa, TexturesSize, textures_block);
    textures_fixed = TexturesSize;
    pds_begin = 0;
    pds_used = pds_begin;
    pds_end = pds_begin + PdsSize;
    data_begin = data_used = pds_end;
    data_end = data_begin + DataSize;
    lut_begin = data_begin + DataSlots * DataSize;
    code_begin = AlignUp(lut_begin + LutSize, 0x1000);
    shared_size = AlignUp(code_begin + CodeSize, 1u << 20);
    shared = MapBlock("azahar-sgxd-shared", BlockVa, shared_size, shared_block);
    for (u32 w = 0; w < PdsWindows; w++) {
        const int mapped = sgxdMap(PdsWindowVa + w * PdsHalfSize,
                                   shared + pds_begin + (w & 1) * PdsHalfSize, PdsHalfSize, 0);
        ASSERT_MSG(mapped == 0, "sgxd: the PDS arena's window {} not mapped at GPU {:#010x}: {:#010x}",
                   w, PdsWindowVa + w * PdsHalfSize, static_cast<u32>(mapped));
    }
    // Each slot of the shell's CDRAM at a GPU address of its own, mapped once: a recipe made in a
    // slot maps nothing. The recipes past the slots take blocks of the game's CDRAM.
    shell_slots_va = BlockVa + shared_size;
    for (u32 slot = 0; slot < ShellSlots; slot++) {
        const int mapped =
            sgxdMap(ShellSlotVa(slot), shell_cdram + slot * RecipeBlockSize, RecipeBlockSize, 0);
        ASSERT_MSG(mapped == 0,
                   "sgxd: the shell's CDRAM slot {} not mapped at GPU {:#010x}: {:#010x}", slot,
                   ShellSlotVa(slot), static_cast<u32>(mapped));
    }
    next_va = ShellSlotVa(ShellSlots);
    sgxd_recipe_code_heap = BlockVa + code_begin;
    draws.resize(MaxDraws);
    programs.resize(MaxDraws);
    identity.resize(MaxIndices);
    for (u32 n = 0; n < MaxIndices; n++) {
        identity[n] = static_cast<u16>(n);
    }
    // A layout only has to hold MaxDraws valid draws; each pass writes its own.
    init_draws.assign(MaxDraws, SgxdRecipeDraw{0, 3, SGXD_RECIPE_ISP_NEVER, SGXD_R_FPRIM, 0, 0,
                                               nullptr});
    // The previous backend ran programs at these addresses and the PDS keeps running a program at
    // an address it has run (docs/sgxd/pds.md): start from the wrap reset with every unit reset.
    {
        const int idle = sgxd_gpu_idle();
        ASSERT_MSG(idle == 0, "sgxd: the GPU did not go idle for the backend's start");
        const int reset = sgxdResetAll();
        ASSERT_MSG(reset == 0, "sgxd: the GPU's reset at the backend's start failed ({:#010x})",
                   static_cast<u32>(reset));
        // The reset cleared what the take's init script set: it runs again, as Sony's driver's
        // does.
        char again[160] = "";
        const int set_up = sgxd_session_reinit(again, sizeof(again));
        ASSERT_MSG(set_up == 0, "sgxd: the GPU not set up again after its reset: {}", again);
        sgxd_recipe_pb_rewrite();
        LOG_INFO(Render, "sgxd: every unit of the GPU reset: the backend's start");
    }
    {
        const int slc = sgxd_slc_invalidate();
        ASSERT_MSG(slc == 0, "sgxd: the SLC not emptied at the backend's start");
    }
    char failed[160] = "";
    const int started = sgxd_ptla_events(&events, MicrokernelVa, failed, sizeof(failed));
    ASSERT_MSG(started == 0, "sgxd: {}", failed);
    // sgxd_gpu_idle (a block mapped or freed) completes the pending 3D first.
    drain_owner = this;
    sgxd_set_render_drain([] {
        drain_owner->RenderIdle("sgxd_gpu_idle");
        return 0;
    });
    // Four macrotiles a side at every size: the cores' loads then follow the scene's quadrants.
    sgxd_recipe_grid = 4;
}

u32 SgxdBackend::RenderFence() const {
    // The pending 3D completes in the list after the last one sent; RenderFinish sends that list if
    // nothing else does.
    const bool pending = sgxd_session_pending() != nullptr;
    sgxd_session_pending_fence();
    return sgxd_chain_fence() + (pending ? 1 : 0);
}

SgxdBackend::Recipe* SgxdBackend::Pending() const {
    const SgxdRecipe* p = sgxd_session_pending();
    for (const auto& r : recipes) {
        if (&r->recipe == p) {
            return r.get();
        }
    }
    return nullptr;
}

u32 SgxdBackend::RenderTaFence() const {
    return sgxd_chain_fence();
}

bool SgxdBackend::RenderReached(u32 fence) const {
    return sgxd_chain_reached(fence) != 0;
}

bool SgxdBackend::RenderTaReached(u32 fence) const {
    return sgxd_chain_ta_reached(fence) != 0;
}

void SgxdBackend::RenderIdle(const char* why) {
    [[maybe_unused]] const u64 before = WaitMark();
    FinishPending(FinishWhy::Idle);
    if (sgxd_chain_idle() == 0) {
        WaitSite(why, before);
        RetireReached();
        return;
    }
    Stopped(why);
}

void SgxdBackend::Stopped(const char* why) {
    char failed[8192];
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
    DEBUG_ASSERT_MSG(waker_flag >= 0, "sgxd: {} waits with no waker", why);
    const TraceSpan span{"render_wait", "render_wait_end", sgxd_chain_fence() - fence};
    int r;
    for (;;) {
        u32 flip_fence;
        if (flip_pending && flip_pending(&flip_fence) && static_cast<s32>(fence - flip_fence) > 0) {
            r = sgxd_chain_sleep(flip_fence, waker_flag, waker_bits);
            if (r != 0) {
                break;
            }
            flip_done();
            continue;
        }
        const s64 due_in = flip_due_in ? flip_due_in() : 0;
        if (due_in > 0) {
            r = sgxd_chain_sleep_until(fence, waker_flag, waker_bits,
                                       sceKernelGetProcessTimeWide() + static_cast<u64>(due_in));
            if (r == 1) {
                flip_done();
                continue;
            }
            break;
        }
        r = sgxd_chain_sleep(fence, waker_flag, waker_bits);
        break;
    }
    // The flag takes one waiter: a refused wait names the others.
    ASSERT_MSG(r >= -1, "sgxd: {}: the wait's event flag failed ({:#010x}); {}", why,
               static_cast<u32>(r), Common::DescribeWaiters(waker_flag));
    if (r == 0) {
        WaitSite(why, before);
        RetireReached();
        return;
    }
    char failed[8192];
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

void SgxdBackend::RemapPages(const u32* pte_va, const u32* pte, u32 count) {
    // The queued untiles and stream-out draws read before it; the pending 3D finishes, so nothing
    // runs beside the remaps' MMU invalidate.
    FlushUntiles();
    FlushStreamOut(false);
    if (sgxd_session_pending()) {
        FinishPending(FinishWhy::Remap);
    }
    char failed[160];
    [[maybe_unused]] const int sent = sgxd_remap_submit(pte_va, pte, count, failed, sizeof(failed));
    ASSERT_MSG(sent == 0, "sgxd: {}", failed);
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

void SgxdBackend::FinishPending([[maybe_unused]] FinishWhy why) {
    if (!sgxd_session_pending()) {
        return;
    }
#ifdef VITA_DIAGNOSTICS
    finishes[static_cast<std::size_t>(why)]++;
#endif
    Recipe* const r = Pending();
    SgxdTrace("finish");
    char failed[200] = "";
    if (sgxd_session_finish_pending(failed, sizeof(failed)) != 0) {
        LOG_CRITICAL(Render, "sgxd: the pending 3D's finish did not start");
        Fault(r ? *r : *current, failed);
    }
}

void SgxdBackend::RenderFinish() {
    if (sgxd_session_pending_fenced()) {
        FinishPending(FinishWhy::Fenced);
    }
}

void SgxdBackend::RenderBefore(u32 lo, u32 hi, [[maybe_unused]] const char* why) {
    // The queued untiles and the stream-out draws' programs read guest memory as it is before the
    // op.
    FlushUntiles();
    FlushStreamOut(false);
    // The op is queued behind the finish, which the kernel runs first.
    if (sgxd_session_pending_touches(lo, hi)) {
        FinishPending(FinishWhy::Touch);
    }
}

SgxdBackend::InterruptFence SgxdBackend::InterruptFenceNow() const {
    return {sgxd_chain_fence() + (sgxd_session_pending() != nullptr ? 1u : 0u),
            sgxd_ptla_fence()};
}

bool SgxdBackend::InterruptPoll(const InterruptFence& fence) {
    // The pending 3D finishes in the list after the last one sent: with the chain free, that list
    // goes.
    if (static_cast<s32>(fence.render - sgxd_chain_fence()) > 0 && sgxd_session_pending() &&
        sgxd_chain_reached(sgxd_chain_fence())) {
        FinishPending(FinishWhy::Poll);
    }
    if (!sgxd_chain_reached(fence.render) || !sgxd_ptla_reached(fence.ptla)) {
        return false;
    }
    // The CPU's part of the work (a DMA's cache lines) before the guest hears of it.
    RetireReached();
    return true;
}

void SgxdBackend::InterruptKick(const InterruptFence& fence) {
    if (static_cast<s32>(fence.render - sgxd_chain_fence()) > 0 && sgxd_session_pending()) {
        FinishPending(FinishWhy::Kick);
    }
}

bool SgxdBackend::InterruptArm(const InterruptFence& fence) {
    u32 render = fence.render;
    if (static_cast<s32>(fence.render - sgxd_chain_fence()) > 0 && sgxd_session_pending()) {
        render = sgxd_chain_fence();
    }
    return sgxd_wake_arm(render, fence.ptla) != 0;
}

void SgxdBackend::InterruptDisarm() {
    sgxd_wake_disarm();
}

bool SgxdBackend::FlipArm(u32 fence) {
    return sgxd_wake_arm(fence, sgxd_ptla_done()) != 0;
}

void SgxdBackend::InterruptWaker(s32 flag, u32 bits) {
    const int r = sgxd_ptla_waker(flag, bits);
    ASSERT_MSG(flag < 0 || r == 0, "sgxd: sgxdWaker {:#010x}", static_cast<u32>(r));
    waker_flag = flag;
    waker_bits = bits;
}

void SgxdBackend::InterruptWait(const InterruptFence& fence) {
    RenderWait(fence.render, "a guest interrupt");
    if (!sgxd_ptla_reached(fence.ptla)) {
        [[maybe_unused]] const u64 before = WaitMark();
        ASSERT_MSG(sgxd_ptla_idle() == 0, "sgxd: the PTLA op before a guest interrupt stopped");
        WaitSite("a guest interrupt's PTLA op", before);
    }
}

u32 SgxdBackend::PtlaFence() const {
    return sgxd_ptla_fence();
}

bool SgxdBackend::PtlaReached(u32 fence) const {
    return sgxd_ptla_reached(fence) != 0;
}

SgxdBackend::~SgxdBackend() {
    RenderIdle("the backend's end");
    // The lists kept for this backend's recipes: the next backend makes its own at their addresses.
    sgxd_session_forget(nullptr);
    sgxd_set_render_drain(nullptr);
    char failed[16];
    sgxd_ptla_events(nullptr, 0, failed, sizeof(failed));
    for (const auto& r : recipes) {
        FreeRecipeBlock(*r);
    }
    for (u32 slot = 0; slot < ShellSlots; slot++) {
        sgxdUnmap(ShellSlotVa(slot), RecipeBlockSize);
    }
    sgxdUnmap(PdsWindowVa, PdsWindows * PdsHalfSize);
    FreeBlock(shared, BlockVa, shared_size, shared_block);
    FreeBlock(surfaces, SurfacesVa, surfaces_size, surfaces_block);
    FreeBlock(textures, TexturesVa, TexturesSize, textures_block);
}

void SgxdBackend::FreeRecipeBlock(const Recipe& r) {
    if (r.shell_slot < 0) {
        FreeBlock(r.memory, r.va, r.size, r.block);
        return;
    }
    // Its slot stays mapped at its address for the next recipe there (its render is done).
    shell_slots_used &= ~(u64{1} << r.shell_slot);
}

SgxdBackend::Recipe& SgxdBackend::RecipeFor(u32 width, u32 height, Kind kind) {
    // A recipe block is its render's until that 3D completes (its fence): an instance the GPU
    // is done with, else a new one, else the one done first. The pending 3D's is never free.
    const Recipe* const pending = Pending();
    u32 instances = 0;
    Recipe* oldest = nullptr;
    for (const auto& r : recipes) {
        if (r->width == width && r->height == height && r->kind == kind) {
            instances++;
            if (r.get() == pending) {
                continue;
            }
            if (sgxd_chain_reached(r->fence)) {
                return *r;
            }
            if (!oldest || static_cast<s32>(r->fence - oldest->fence) < 0) {
                oldest = r.get();
            }
        }
    }
    if (instances >= MaxInstances && oldest) {
        RenderWait(oldest->fence, "a recipe instance");
        return *oldest;
    }
    // CDRAM bounds the instances in all, not only each size's MaxInstances: a game had 14 blocks
    // by 41 s, and a 15th found no CDRAM once its FCRAM textures were untiled every frame.
    // With less free than a block and a margin, and an
    // instance of the size in flight, that one's render is waited for, as at MaxInstances. The
    // first instance of a size is made, or MapBlock stops the emulator.
    if (oldest) {
        SceKernelFreeMemorySizeInfo free{};
        free.size = sizeof(free);
        sceKernelGetFreeMemorySize(&free);
        if (static_cast<u32>(free.size_cdram) < RecipeBlockSize + RecipeCdramMargin) {
            RenderWait(oldest->fence, "a recipe instance (CDRAM)");
            return *oldest;
        }
    }
    int slot = -1;
    for (u32 k = 0; k < ShellSlots && slot < 0; k++) {
        if ((shell_slots_used >> k & 1) == 0) {
            slot = static_cast<int>(k);
        }
    }
    if (slot >= 0) {
        va = ShellSlotVa(static_cast<u32>(slot));
        va_end = va + RecipeBlockSize;
    }
    auto made = std::make_unique<Recipe>();
    Recipe& r = *made;
    r.width = width;
    r.height = height;
    r.kind = kind;
    r.instance = instances;
    r.va = next_va;
    // Every pass renders to a surface or guest memory: the recipe's own FB and DEPTH, and its
    // pages, take no CDRAM.
    sgxd_recipe_external_fb = 1;
    [[maybe_unused]] const int init = sgxd_recipe_init_programs(
        &r.recipe, width, height, r.va, Pages, identity.data(), MaxIndices, init_draws.data(),
        MaxDraws);
    sgxd_recipe_external_fb = 0;
    DEBUG_ASSERT_MSG(init == 0, "sgxd: the recipe does not take a {}x{} target", width, height);
    r.bg_offset = AlignUp(r.recipe.offset[SGXD_R_END], 0x1000);
    r.size = AlignUp(r.bg_offset + 0x1000, 256u << 10);
    DEBUG_ASSERT_MSG(r.size <= RecipeBlockSize, "sgxd: a {}x{} recipe takes {} KiB, past {}", width,
                     height, r.size >> 10, RecipeBlockSize >> 10);
    DEBUG_ASSERT_MSG(r.va + r.size <= RecipesEnd, "sgxd: no GPU space for a {}x{} recipe",
                     width, height);
    r.memory = MapRecipeBlock(r);
    next_va = AlignUp(r.va + r.size, 1u << 20);

    sgxd_recipe_write(&r.recipe, r.memory);
    // The arrays go with the recipe: only the indices are read again.
    r.recipe.draws = nullptr;
    if (kind == Kind::Pica) {
        // The pixel event program DOUTAs IR0 to the end of tile, which OpenPass writes after the
        // background word: in the layout's PIXEV, before its first run.
        const u32 eot_index = (r.bg_offset + PicaEotOffset - r.recipe.offset[SGXD_R_CF]) >> 3;
        u32 pixev[SGXD_RECIPE_PICA_PIXEV_WORDS];
        sgxd_recipe_pica_pixev(&r.recipe, eot_index, pixev);
        std::memcpy(r.memory + r.recipe.offset[SGXD_R_PIXEV], pixev, sizeof(pixev));
        sgxd_recipe_set_pixel_event(&r.recipe, sgxd_recipe_va(&r.recipe, SGXD_R_PIXEV), 4, 4);
    }
    sgxd_recipe_sa_background(&r.recipe, r.memory, r.bg_offset, 0xff000000u);
    static constexpr const char* kinds[] = {"linear", "PICA", "side"};
    LOG_INFO(Render,
             "sgxd: the rasterizer's {}x{} {} recipe {}, {} KiB at GPU {:#010x} ({}): layout {} "
             "KiB; shared block at {:#010x}",
             width, height, kinds[static_cast<u32>(kind)], r.instance, r.size >> 10, r.va,
             r.shell_slot >= 0 ? "the shell's CDRAM" : "the game's",
             r.recipe.offset[SGXD_R_END] >> 10, BlockVa);
    recipes.push_back(std::move(made));
    return r;
}

void SgxdBackend::ReleaseCode() {
    std::erase_if(code_pending, [this](const CodeRange& r) {
        if (r.mark > passes) {
            return false; // the open pass may run it
        }
        const u32 at = passes - r.mark < PassFences ? r.mark : passes - PassFences + 1;
        if (r.mark != 0 && !RenderReached(pass_fences[at % PassFences])) {
            return false;
        }
        const auto it =
            std::lower_bound(code_free.begin(), code_free.end(), r.offset,
                             [](const CodeRange& f, u32 offset) { return f.offset < offset; });
        const auto placed = code_free.insert(it, {r.offset, r.bytes, 0});
        if (placed + 1 != code_free.end() && placed->offset + placed->bytes == placed[1].offset) {
            placed->bytes += placed[1].bytes;
            code_free.erase(placed + 1);
        }
        if (placed != code_free.begin() && placed[-1].offset + placed[-1].bytes == placed->offset) {
            placed[-1].bytes += placed->bytes;
            code_free.erase(placed);
        }
        return true;
    });
}

u32 SgxdBackend::AllocateCode(u32 bytes) {
    const u32 need = AlignUp(bytes, 16);
    for (;;) {
        ReleaseCode();
        for (auto it = code_free.begin(); it != code_free.end(); ++it) {
            if (it->bytes >= need) {
                const u32 at = it->offset;
                it->offset += need;
                it->bytes -= need;
                if (it->bytes == 0) {
                    code_free.erase(it);
                }
                return at;
            }
        }
        const u32 at = AlignUp(code_used, 16);
        if (at + need <= CodeSize) {
            code_used = at + need;
            return at;
        }
        ASSERT_MSG(!code_pending.empty() && code_pending.front().mark <= passes,
                   "sgxd: the code heap is full ({} KiB, {} KiB free in pieces)", CodeSize >> 10,
                   [this] {
                       u32 sum = 0;
                       for (const CodeRange& f : code_free) {
                           sum += f.bytes;
                       }
                       return sum >> 10;
                   }());
        WaitPassMark(code_pending.front().mark, "the code heap's reuse");
    }
}

u32 SgxdBackend::CodeFree() {
    ReleaseCode();
    u32 free = CodeSize - AlignUp(code_used, 16);
    for (const CodeRange& f : code_free) {
        free += f.bytes;
    }
    return free;
}

void SgxdBackend::FreeCode(u32 offset, u32 bytes) {
    if (bytes != 0) {
        code_pending.push_back({offset, AlignUp(bytes, 16), PassMark()});
    }
}

u32 SgxdBackend::PlaceCode(unsigned cb, std::span<const u64> words) {
    const u32 bytes = static_cast<u32>(words.size_bytes());
    const u32 at = AllocateCode(bytes);
    std::memcpy(shared + code_begin + at, words.data(), bytes);
#ifdef VITA_DIAGNOSTICS
    placed_code.push_back({cb, at, bytes, at});
#endif
    return at;
}

u32 SgxdBackend::PlaceCodePhased(unsigned cb, std::vector<u64> words,
                                 std::span<const u32> phase_starts, u32 temps) {
    // Its place first: each PHAS names the next phase's address, from the heap's start.
    const u32 offset = AllocateCode(static_cast<u32>(words.size() * sizeof(u64)));
    for (std::size_t k = 0; k + 1 < phase_starts.size(); k++) {
        u64& phas = words[phase_starts[k]];
        // 11111 010 s 100: OTHER2 PHAS; bit 50 PHAS_IMM.
        DEBUG_ASSERT_MSG(((phas >> 52) & 0xff7) == 0xfa4 && (phas >> 50 & 1) != 0,
                         "sgxd: phase {} opens with {:016x}, no immediate PHAS", k, phas);
        const u64 exe = (offset >> 3) + phase_starts[k + 1];
        DEBUG_ASSERT(exe < (1u << 20));
        phas = (phas & ~(0xffull << 32 | 0xfffffull)) | u64{(temps + 3) / 4} << 32 | exe;
    }
    std::memcpy(shared + code_begin + offset, words.data(), words.size() * sizeof(u64));
#ifdef VITA_DIAGNOSTICS
    placed_code.push_back({cb, offset, static_cast<u32>(words.size() * sizeof(u64)), offset});
#endif
    return offset;
}

u32 SgxdBackend::AppendPds(const SgxdPdsProgram& program) {
    const u32 bytes = program.count * 4;
    const u32 end = pds_begin + (pds_half + 1) * PdsHalfSize;
    const u32 at = AlignUp(pds_used, 32);
    ASSERT_MSG(at + bytes <= end, "sgxd: the PDS arena's flipping half is full ({} KiB, {} programs)",
               PdsHalfSize >> 10, pds_index_programs);
    std::memcpy(shared + at, program.words, bytes);
    pds_used = at + bytes;
    pds_index_programs++;
    return PdsWindowVa + pds_window * PdsHalfSize + (at - (pds_begin + pds_half * PdsHalfSize));
}

u32 SgxdBackend::PdsIndex::Hash(const u32* key, u32 words) {
    u32 h = HashStart(words);
    for (u32 i = 0; i < words; i++) {
        h = HashStep(h, key[i]);
    }
    return HashEnd(h);
}

u32 SgxdBackend::PdsIndex::HashEnd(u32 h) {
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    return h ^ (h >> 16);
}

const SgxdBackend::PdsRef* SgxdBackend::PdsIndex::Find(const u32* key, u32 words,
                                                       u32 hash) const {
    const u32 mask = static_cast<u32>(slots.size()) - 1;
    for (u32 i = hash & mask;; i = (i + 1) & mask) {
        const Slot& slot = slots[i];
        if (slot.key_words == 0) {
            return nullptr;
        }
        if (slot.hash == hash && slot.key_words == words) {
            const u32* const kept = keys.data() + slot.key_at;
            u32 n = 0;
            while (n < words && kept[n] == key[n]) {
                n++;
            }
            if (n == words) {
                return &slot.ref;
            }
        }
    }
}

void SgxdBackend::PdsIndex::Insert(const u32* key, u32 words, u32 hash, PdsRef ref) {
    if ((used + 1) * 2 > slots.size()) {
        Grow();
    }
    const u32 mask = static_cast<u32>(slots.size()) - 1;
    u32 i = hash & mask;
    while (slots[i].key_words != 0) {
        i = (i + 1) & mask;
    }
    slots[i] = Slot{hash, static_cast<u32>(keys.size()), words, ref};
    keys.insert(keys.end(), key, key + words);
    used++;
}

void SgxdBackend::PdsIndex::Grow() {
    std::vector<Slot> old(slots.size() * 2);
    old.swap(slots);
    const u32 mask = static_cast<u32>(slots.size()) - 1;
    for (const Slot& slot : old) {
        if (slot.key_words == 0) {
            continue;
        }
        u32 i = slot.hash & mask;
        while (slots[i].key_words != 0) {
            i = (i + 1) & mask;
        }
        slots[i] = slot;
    }
}

void SgxdBackend::PdsIndex::Clear() {
    std::fill(slots.begin(), slots.end(), Slot{});
    keys.clear();
    used = 0;
}

template <typename Build>
SgxdBackend::PdsRef SgxdBackend::KeyedPds(const u32* key, u32 words, Build&& build) {
    return KeyedPds(key, words, PdsIndex::Hash(key, words), build);
}

template <typename Build>
SgxdBackend::PdsRef SgxdBackend::KeyedPds(const u32* key, u32 words, u32 hash, Build&& build) {
    if (const PdsRef* hit = pds_index.Find(key, words, hash)) {
        return *hit;
    }
    // The generators write the words they count (finish, pds_words); no clearing of the rest.
    SgxdPdsProgram pds;
    if (build(pds) != 0) {
        return PdsRef{0, 0}; // the caller asserts, naming what it asked for
    }
    const PdsRef ref{AppendPds(pds), pds.units};
    pds_index.Insert(key, words, hash, ref);
    return ref;
}

template <typename Build>
SgxdBackend::PdsTemplate* SgxdBackend::TemplatePds(const u32* key, u32 words, Build&& build) {
    const u32 hash = PdsIndex::Hash(key, words);
    if (const PdsRef* hit = pds_shapes.Find(key, words, hash)) {
        return &pds_templates[hit->va];
    }
    PdsTemplate& t = pds_templates.emplace_back();
    if (build(t) != 0) {
        pds_templates.pop_back();
        return nullptr;
    }
    pds_shapes.Insert(key, words, hash, PdsRef{static_cast<u32>(pds_templates.size() - 1), 0});
    return &t;
}

namespace {
enum PdsTag : u32 {
    TagVertex = 1,
    TagSecondary,
    TagBlitPixel,
    TagScreenPixel,
    TagMergePixel,
    TagRaw64Pixel,
    TagTerm,
    TagPicaPixel,
    TagPixel,
    TagVertexShape,
    TagSecondaryShape,
};
} // namespace

SgxdBackend::PdsRef SgxdBackend::VertexPds(const SgxdVertexFetch* fetch, u32 count, u32 doutu0,
                                           bool per_instance, bool fresh) {
    static_assert(sizeof(SgxdVertexFetch) == 4 * sizeof(u32));
    std::array<u32, 4 + 4 * 32> key;
    DEBUG_ASSERT_MSG(count <= 32, "sgxd: {} vertex fetches", count);
    const u32 words = 4 + 4 * count;
    key[1] = doutu0;
    key[2] = per_instance ? 1 : 0;
    key[3] = count;
    u32 hash = 0;
    if (!fresh) {
        key[0] = TagVertex;
        std::memcpy(key.data() + 4, fetch, count * sizeof(SgxdVertexFetch));
        hash = PdsIndex::Hash(key.data(), words);
        if (const PdsRef* hit = pds_index.Find(key.data(), words, hash)) {
            return *hit;
        }
    }
    // A miss, or a fetch of pass data allocated for this draw: the program built for the same
    // fetches at other addresses, these written in.
    key[0] = TagVertexShape;
    for (u32 n = 0; n < count; n++) {
        key[4 + 4 * n] = 0;
        key[5 + 4 * n] = fetch[n].stride;
        key[6 + 4 * n] = fetch[n].words;
        key[7 + 4 * n] = fetch[n].pa;
    }
    PdsTemplate* const t = TemplatePds(key.data(), words, [&](PdsTemplate& built) {
        return sgxd_program_vertex_pds_at(fetch, count, doutu0, per_instance ? 1 : 0,
                                          &built.program, built.at.data());
    });
    if (!t) {
        return PdsRef{0, 0}; // the caller asserts, naming what it asked for
    }
    for (u32 n = 0; n < count; n++) {
        t->program.words[t->at[n]] = fetch[n].address;
    }
    const PdsRef ref{AppendPds(t->program), t->program.units};
    if (!fresh) {
        key[0] = TagVertex;
        for (u32 n = 0; n < count; n++) {
            key[4 + 4 * n] = fetch[n].address;
        }
        pds_index.Insert(key.data(), words, hash, ref);
    }
    return ref;
}

SgxdBackend::PdsRef SgxdBackend::SecondaryPds(u32 sa_address, u32 sa_words, u32 doutu0) {
    if (sa_words == 0) {
        // No DMA: the address is not in the program.
        const u32 key[4] = {TagSecondary, 0, 0, doutu0};
        return KeyedPds(key, 4, [&](SgxdPdsProgram& pds) {
            return sgxd_program_secondary_pds(0, 0, doutu0, &pds);
        });
    }
    // Every SA image is pass data allocated for its draw: no lookup by its address, the program for
    // the same SA words at another address with this one written in. A draw that takes the last
    // image of its program again finds the program in secondary_kept.
    SecondaryKept& kept = secondary_kept[(sa_address >> 4) & (secondary_kept.size() - 1)];
    if (kept.address == sa_address && kept.words == sa_words && kept.doutu0 == doutu0) {
        return kept.ref;
    }
    const u32 shape[4] = {TagSecondaryShape, 0, sa_words, doutu0};
    PdsTemplate* const t = TemplatePds(shape, 4, [&](PdsTemplate& built) {
        return sgxd_program_secondary_pds_at(sa_address, sa_words, doutu0, &built.program,
                                             built.at.data());
    });
    if (!t) {
        return PdsRef{0, 0};
    }
    t->program.words[t->at[0]] = sa_address;
    const PdsRef ref{AppendPds(t->program), t->program.units};
    kept = {sa_address, sa_words, doutu0, ref};
    return ref;
}

SgxdBackend::PdsRef SgxdBackend::BlitPixelPds(u32 doutu0, const u32 tex[4]) {
    const u32 key[6] = {TagBlitPixel, doutu0, tex[0], tex[1], tex[2], tex[3]};
    return KeyedPds(key, 6, [&](SgxdPdsProgram& pds) {
        return sgxd_program_blit_pixel_pds(doutu0, tex, &pds);
    });
}

SgxdBackend::PdsRef SgxdBackend::ScreenPixelPds(u32 doutu0, const u32 tex[4]) {
    const u32 key[6] = {TagScreenPixel, doutu0, tex[0], tex[1], tex[2], tex[3]};
    return KeyedPds(key, 6, [&](SgxdPdsProgram& pds) {
        return sgxd_program_screen_pixel_pds(doutu0, tex, &pds);
    });
}

SgxdBackend::PdsRef SgxdBackend::Raw64PixelPds(u32 doutu0, const u32 tex[4]) {
    const u32 key[6] = {TagRaw64Pixel, doutu0, tex[0], tex[1], tex[2], tex[3]};
    return KeyedPds(key, 6, [&](SgxdPdsProgram& pds) {
        return sgxd_program_raw64_pixel_pds(doutu0, tex, &pds);
    });
}

SgxdBackend::PdsRef SgxdBackend::TermPds(const SgxdRecipe& recipe) {
    // Its words follow from the tiling and the layout's code offsets, which every instance shares.
    const u32 key[5] = {TagTerm, recipe.tx, recipe.ty, recipe.layout_width, recipe.layout_height};
    return KeyedPds(key, 5, [&](SgxdPdsProgram& pds) {
        uint32_t units = 0;
        pds.count = sgxd_recipe_term_pds(&recipe, pds.words, &units);
        pds.units = units;
        return pds.count != 0 ? 0 : -1;
    });
}

SgxdBackend::PdsRef SgxdBackend::MergePixelPds(u32 doutu0, const u32 tex0[4], const u32 tex1[4]) {
    const u32 key[10] = {TagMergePixel, doutu0,  tex0[0], tex0[1], tex0[2],
                         tex0[3],       tex1[0], tex1[1], tex1[2], tex1[3]};
    return KeyedPds(key, 10, [&](SgxdPdsProgram& pds) {
        return sgxd_program_merge_pixel_pds(doutu0, tex0, tex1, &pds);
    });
}

SgxdBackend::PdsRef SgxdBackend::PixelPds(u32 doutu0, const u32* iterators, const u32* words,
                                          u32 count) {
    DEBUG_ASSERT(count <= 12);
    std::array<u32, 3 + 2 * 12> key;
    u32 n = 0;
    key[n++] = TagPixel;
    key[n++] = doutu0;
    key[n++] = count;
    for (u32 i = 0; i < count; i++) {
        key[n++] = iterators[i];
        key[n++] = words[i];
    }
    return KeyedPds(key.data(), n, [&](SgxdPdsProgram& pds) {
        static_assert(sizeof(unsigned) == sizeof(u32));
        return sgxd_program_pixel_pds(doutu0, iterators, reinterpret_cast<const unsigned*>(words),
                                      count, &pds);
    });
}

SgxdBackend::PdsRef SgxdBackend::PicaPixelPds(u32 doutu0, const u32* iterators, const u32* words,
                                              u32 count, const SgxdTextureIssue* issues,
                                              u32 issue_count) {
    DEBUG_ASSERT(count <= 12 && issue_count <= 7);
    std::array<u32, 4 + 2 * 12 + 5 * 7> key;
    u32 n = 0;
    key[n++] = TagPicaPixel;
    key[n++] = doutu0;
    key[n++] = count;
    key[n++] = issue_count;
    for (u32 i = 0; i < count; i++) {
        key[n++] = iterators[i];
        key[n++] = words[i];
    }
    for (u32 i = 0; i < issue_count; i++) {
        key[n++] = issues[i].coord | issues[i].after << 8 | issues[i].f16 << 16 |
                   issues[i].raw << 17 | issues[i].proj_t << 18;
        key[n++] = issues[i].tex[0];
        key[n++] = issues[i].tex[1];
        key[n++] = issues[i].tex[2];
        key[n++] = issues[i].tex[3];
    }
    return KeyedPds(key.data(), n, [&](SgxdPdsProgram& pds) {
        static_assert(sizeof(unsigned) == sizeof(u32));
        return sgxd_program_pica_pixel_pds(doutu0, iterators,
                                           reinterpret_cast<const unsigned*>(words), count, issues,
                                           issue_count, &pds);
    });
}

void SgxdBackend::FlipPdsHalf() {
    const u32 other = pds_half ^ 1;
    const u32 window = (pds_window + 1) % PdsWindows;
    // No list after the last flip away from the other half reads it, so its fence is long reached
    // as a rule. This half's are dropped now.
    RenderWait(pds_fence[other], "the PDS arena's flip");
    pds_fence[pds_half] = FenceAll();
    pds_index.Clear();
    secondary_kept = {};
    pds_index_programs = 0;
    pds_window = window;
    pds_half = other;
    pds_used = pds_begin + other * PdsHalfSize;
    pds_flips++;
    if (window != 0) {
        // A window the PDS has not run programs from: no idle, no reset.
#ifdef VITA_DIAGNOSTICS
        if (SgxdFlag("perflog")) {
            LOG_INFO(Render, "sgxd: PDS arena flip {} to window {} ({:#010x})", pds_flips,
                     window, PdsWindowVa + window * PdsHalfSize);
        }
#endif
        return;
    }
    // The first window again: its addresses are written again, and the PDS keeps running a program
    // at an address it has run (docs/sgxd/pds.md): the GPU idles and the cores are reset
    // (ResetCores).
    FinishPending(FinishWhy::PdsFlip);
    RenderIdle("the PDS arena's flip");
    ResetCores("the PDS arena's flip");
#ifdef VITA_DIAGNOSTICS
    if (SgxdFlag("perflog")) {
        LOG_INFO(Render, "sgxd: PDS arena flip {} to window 0, the GPU idled and reset ({} lists sent)",
                 pds_flips, sgxd_chain_fence());
    }
#endif
}

void SgxdBackend::ResetCores(const char* why) {
    // Every core's PDS (bank 0's SOFT_RESET reaches all four) with the USE and the caching units,
    // SOFT_RESET 0x000c8d00, which keeps the code bases and the PDS, DMS and BIF settings. The PDS
    // alone leaves the USE's service counters behind, and the next TA's first vertex task is
    // never serviced. A list of its own: from core 0's USSE the reset would not reach cores 2 and 3.
    const int idle = sgxd_gpu_idle();
    ASSERT_MSG(idle == 0, "sgxd: the GPU did not go idle for the cores' reset ({})", why);
    std::array<SgxdOp, 2> reset{
        {{SGXD_OP_WRITE, 0x0080, 0x000c8d00, 0, 0}, {SGXD_OP_WRITE, 0x0080, 0, 0, 0}}};
    const int pulsed = sgxdSubmit(reset.data(), reset.size());
    ASSERT_MSG(pulsed == static_cast<int>(reset.size()),
               "sgxd: the cores' reset ({}) stopped ({:#x})", why, static_cast<u32>(pulsed));
    LOG_INFO(Render, "sgxd: the cores reset: {}", why);
}

const u8* SgxdBackend::SharedAt(u32 gpu) const {
    return shared + (gpu - BlockVa);
}

__attribute__((noinline)) SgxdBackend::Data SgxdBackend::AllocData(u32 bytes, u32 align) {
    DEBUG_ASSERT(in_pass);
    const u32 at = AlignUp(data_used, align);
    const uintptr_t caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    const u32 hashed = static_cast<u32>(caller >> 1 ^ caller >> 5 ^ caller >> 9);
    DataSite& slot = data_sites[hashed & (data_sites.size() - 1)];
    slot.caller = slot.caller == 0 || slot.caller == caller ? caller : 1;
    slot.bytes += bytes;
    slot.count++;
    if (at + bytes > data_end) {
        for (const DataSite& site : data_sites) {
            if (site.caller != 0) {
                LOG_CRITICAL(Render, "sgxd: pass data from {}: {} bytes in {} allocations",
                             site.caller == 1 ? std::string("several callers")
                                              : fmt::format("{:#010x}", site.caller),
                             site.bytes, site.count);
            }
        }
    }
    // In every build: past its room a pass's data overwrites what the GPU still reads.
    ASSERT_MSG(at + bytes <= data_end,
               "sgxd: a pass's data passed {} KiB ({} bytes asked, {} draws)", DataSize >> 10,
               bytes, draw_count);
    data_used = at + bytes;
    return {shared + at, BlockVa + at};
}

SgxdBackend::Data SgxdBackend::GeometryVertices(u64 key, u32 bytes, bool& fresh) {
    const auto it = geometry_vertices.find(key);
    fresh = it == geometry_vertices.end();
    if (!fresh) {
        return it->second;
    }
    const Data vertices = AllocTextureFixed(bytes);
    geometry_vertices.emplace(key, vertices);
    return vertices;
}

template <typename Make>
const SgxdBackend::Geometry& SgxdBackend::GeometryFor(u64 key, u32 vertex_bytes, Make&& make) {
    const auto [it, fresh] = geometries.try_emplace(key);
    Geometry& g = it->second;
    if (fresh) {
        const u32 count = make(nullptr, g);
        g.vertices = AllocTextureFixed(count * vertex_bytes);
        make(reinterpret_cast<float*>(g.vertices.cpu), g);
    }
    return g;
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

namespace {

u32 NewerFence(u32 a, u32 b) {
    return a == 0 ? b : b == 0 ? a : static_cast<s32>(a - b) > 0 ? a : b;
}

template <typename Range>
bool TakeFree(std::vector<Range>& free, u32 bytes, u32& at, u32* fence = nullptr) {
    for (auto it = free.begin(); it != free.end(); ++it) {
        if (it->bytes >= bytes) {
            at = it->offset;
            if (fence) {
                *fence = it->fence;
            }
            it->offset += bytes;
            it->bytes -= bytes;
            if (it->bytes == 0) {
                free.erase(it);
            }
            return true;
        }
    }
    return false;
}

template <typename Range>
void GiveFree(std::vector<Range>& free, Range range) {
    auto it = std::lower_bound(free.begin(), free.end(), range.offset,
                               [](const Range& f, u32 offset) { return f.offset < offset; });
    if (it != free.begin() && std::prev(it)->offset + std::prev(it)->bytes == range.offset) {
        --it;
        it->bytes += range.bytes;
        it->fence = NewerFence(it->fence, range.fence);
    } else {
        it = free.insert(it, range);
    }
    const auto next = std::next(it);
    if (next != free.end() && it->offset + it->bytes == next->offset) {
        it->bytes += next->bytes;
        it->fence = NewerFence(it->fence, next->fence);
        free.erase(next);
    }
}

} // namespace

SgxdBackend::Data SgxdBackend::AllocSurface(u32 width, u32 height) {
    const u32 bytes = AlignUp(width * height * 4, 0x1000);
    u32 at = 0;
    if (TakeFree(surfaces_free, bytes, at)) {
        return {surfaces + at, SurfacesVa + at};
    }
    // Past the block lie the textures, and past its memory the CPU's pointer: a surface the block
    // has no room for comes from the textures pool. The block holds the depth the passes leave on
    // the GPU, none dropped while it is the newer copy.
    if (surfaces_used + bytes > surfaces_size) {
        u32 ready = 0;
        const Data d = AllocTexture(bytes, &ready);
        RenderWait(ready, "a surface's range in the textures pool");
        if (surfaces_spilled++ < 4) {
            LOG_INFO(Render,
                     "sgxd: a {}x{} surface past the surfaces block ({} KiB used) at GPU {:#010x}",
                     width, height, surfaces_used >> 10, d.gpu);
        }
        return d;
    }
    const Data d{surfaces + surfaces_used, SurfacesVa + surfaces_used};
    surfaces_used += bytes;
    return d;
}

void SgxdBackend::FreeSurface(const Data& surface, u32 width, u32 height) {
    // The next surface there is cleared or loaded by the GPU behind the lists reading this one; the
    // pending 3D's finish goes ahead of them.
    FinishPending(FinishWhy::FreeSurface);
    GiveFree(surfaces_free,
             FreeRange{surface.gpu - SurfacesVa, AlignUp(width * height * 4, 0x1000)});
}

bool SgxdBackend::TextureFits(u32 bytes) const {
    bytes = AlignUp(bytes, 0x1000);
    return textures_used + bytes <= textures_fixed ||
           std::any_of(textures_free.begin(), textures_free.end(),
                       [bytes](const FreeRange& f) { return f.bytes >= bytes; });
}

u32 SgxdBackend::TexturePoolLive() const {
    u32 free_bytes = 0;
    for (const FreeRange& f : textures_free) {
        free_bytes += f.bytes;
    }
    return textures_used - free_bytes + (TexturesSize - textures_fixed);
}

SgxdBackend::Data SgxdBackend::AllocTexture(u32 bytes, u32* ready) {
    bytes = AlignUp(bytes, 0x1000);
    // Textures, untiles' temporaries and vertices alike make room by evicting textures.
    while (!TextureFits(bytes) && texture_evictor && texture_evictor()) {
    }
    u32 at = 0;
    if (TakeFree(textures_free, bytes, at)) {
        return {textures + at, TexturesVa + at};
    }
    DEBUG_ASSERT_MSG(textures_used + bytes <= textures_fixed,
                     "sgxd: no room for a {} KiB texture ({} KiB used, {} KiB fixed; free{})",
                     bytes >> 10, textures_used >> 10, (TexturesSize - textures_fixed) >> 10, [&] {
                         std::string f;
                         for (const FreeRange& r : textures_free) {
                             f += fmt::format(" {:#x}+{}K", r.offset, r.bytes >> 10);
                         }
                         return f;
                     }());
    const Data d{textures + textures_used, TexturesVa + textures_used};
    textures_used += bytes;
    return d;
}

SgxdBackend::Data SgxdBackend::AllocTextureFixed(u32 bytes) {
    bytes = AlignUp(bytes, 0x1000);
    while (textures_used + bytes > textures_fixed && texture_evictor && texture_evictor()) {
    }
    DEBUG_ASSERT_MSG(textures_used + bytes <= textures_fixed,
                     "sgxd: no room for {} KiB of fixed texture memory ({} KiB used)", bytes >> 10,
                     textures_used >> 10);
    textures_fixed -= bytes;
    return {textures + textures_fixed, TexturesVa + textures_fixed};
}

void SgxdBackend::FreeTexture(const Data& texture, u32 bytes) {
    // A copy in a queued batch's block: its untile goes first, or it would write over the next
    // owner.
    for (u32 k = 0; k < UntileBatchKinds; k++) {
        const UntileBatch& b = untile_batches[k];
        if (b.block.cpu && texture.gpu - b.block.gpu < b.capacity * UnitBytes(k)) {
            FlushUntiles();
            break;
        }
    }
    // The range goes to the next texture, written behind the lists that sample this one; the
    // pending 3D's finish goes ahead of them.
    FinishPending(FinishWhy::FreeTexture);
    GiveTexture(texture, bytes);
}

void SgxdBackend::FreeTextureAfterPass(const Data& texture, u32 bytes) {
    if (!in_pass) {
        FreeTexture(texture, bytes);
        return;
    }
    pass_frees.emplace_back(texture, bytes);
}

void SgxdBackend::GiveTexture(const Data& texture, u32 bytes) {
    GiveFree(textures_free,
             FreeRange{texture.gpu - TexturesVa, AlignUp(bytes, 0x1000), FenceAll()});
    if (!textures_free.empty() &&
        textures_free.back().offset + textures_free.back().bytes == textures_used) {
        textures_used = textures_free.back().offset;
        textures_top_fence = NewerFence(textures_top_fence, textures_free.back().fence);
        textures_free.pop_back();
    }
}

void SgxdBackend::OpenPass(u32 target_gpu, u32 width, u32 height, u32 pack, u32 clear,
                           float depth, const PicaTarget* pica) {
    DEBUG_ASSERT(!in_pass);
    // The recipe instance and the pass data slot are the GPU's until their last render's 3D
    // completes: RecipeFor and the slot's fence wait only when the GPU is that far behind.
    // Past half of the current PDS half: the next pass starts in the other one.
    if (pds_used - (pds_begin + pds_half * PdsHalfSize) > PdsHalfSize / 2) {
        FlipPdsHalf();
    }
    Recipe& r = RecipeFor(width, height, pica ? Kind::Pica : Kind::Linear);
    current = &r;
    pass_target = pica ? Footprint{pica->gpu, pica->gpu + pica->width * pica->height * pica->bpp}
                       : Footprint{target_gpu, target_gpu + width * height * 4};
    pass_source = {};
    data_slot = (data_slot + 1) % DataSlots;
    RenderWait(data_fence[data_slot], "a pass's data slot");
    data_begin = pds_end + data_slot * DataSize;
    data_end = data_begin + DataSize;
    if (pica) {
        const SgxdPicaStore store{pica->gpu, pica->pack, pica->bpp, pica->x0,
                                  pica->x1,  pica->y0,   pica->y1};
        u64 eot[SGXD_RECIPE_PICA_EOT_MAX];
        const unsigned words = sgxd_recipe_pica_eot(&r.recipe, &store, eot);
        // No end of tile: the instance would keep the last target's.
        ASSERT_MSG(words != 0,
                   "sgxd: a {}x{} colour buffer at {:#010x} (its part {}..{} x {}..{}): route 1 "
                   "takes widths of 32 to 1024 in 16s and heights of 8 to 1024 in 8s, and parts "
                   "in 8s (y0 in 16s)",
                   pica->width, pica->height, pica->gpu, pica->x0, pica->x1, pica->y0, pica->y1);
        std::memcpy(r.memory + r.bg_offset + PicaEotOffset, eot, words * 8);
    } else {
        sgxd_recipe_set_target(&r.recipe, r.memory, target_gpu, width);
        sgxd_recipe_set_target_pack(&r.recipe, r.memory, pack);
    }
    *reinterpret_cast<volatile u32*>(r.memory + r.bg_offset) = clear;
    sgxd_recipe_set_background_depth(&r.recipe, r.memory, depth);
    sgxd_recipe_set_background_stencil(&r.recipe, 0);
    sgxd_recipe_set_zls(&r.recipe, 0, 0, 0, 0);
    pass_zls = {};
    data_used = data_begin;
    data_opens++;
    data_sites = {};
    draw_count = 0;
    cached.clear();
    in_pass = true;
}

void SgxdBackend::BeginPass(const PicaTarget& target, u32 clear, float depth,
                            const std::function<void()>& before) {
    // A tile no draw touches stays EMPTY and is skipped whole, keeping the target's bytes (the
    // guest's, which the fill cleared).
    OpenPass(target.gpu, target.height, target.width, target.pack, clear, depth, &target);
    if (before) {
        before();
    }
}

void SgxdBackend::BeginPassOver(const PicaTarget& target, float depth,
                                const std::function<void()>& before) {
    OpenPass(target.gpu, target.height, target.width, target.pack, 0, depth, &target);
    if (before) {
        before();
    }
}

void SgxdBackend::BeginPassLoaded(const PicaTarget& target, float depth, const UntileSource& guest,
                                  const std::function<void()>& before) {
    OpenPass(target.gpu, target.height, target.width, target.pack, 0, depth, &target);
    if (before) {
        before();
    }
    // Each tile reads its own old texels before its end of tile stores the new ones over them
    // (H2: a pass reading its own target reloads it exactly).
    pass_source = {guest.gpu, guest.gpu + guest.width * guest.height * guest.bits / 8};
    DrawTiled(guest, true);
}

void SgxdBackend::SetDepthStore(const DepthStore& store, DepthLoad load,
                                const DepthStore* store_to) {
    DEBUG_ASSERT(in_pass);
    const DepthStore& out = store_to ? *store_to : store;
    DEBUG_ASSERT(out.width == store.width && out.height == store.height &&
                 out.stencil == store.stencil);
    const u32 samples = store.width * store.height;
    sgxd_recipe_set_zls_to(
        &current->recipe, store.memory.gpu, store.stencil ? store.memory.gpu + samples * 4 : 0,
        static_cast<int>(load), out.memory.gpu, out.stencil ? out.memory.gpu + samples * 4 : 0);
    pass_zls = {store.memory.gpu, store.memory.gpu + samples * (store.stencil ? 5 : 4)};
}

void SgxdBackend::CopyDepthPart(const DepthStore& from, const DepthStore& to, u32 x0, u32 x1,
                                u32 y0, u32 y1) {
    DEBUG_ASSERT(from.width == to.width && from.height == to.height && from.stencil == to.stencil &&
                 x0 % 8 == 0 && x1 % 8 == 0 && x0 < x1 && y0 < y1 && x1 <= to.width &&
                 y1 <= to.height);
    const u32 samples = to.width * to.height;
    RenderBefore(to.memory.gpu, to.memory.gpu + samples * (to.stencil ? 5 : 4),
                 "a depth store's part");
    char failed[160];
    const u32 at = (y0 * to.width + x0) * 4;
    const SgxdPtlaCopy depth{from.memory.gpu + at,
                             to.memory.gpu + at,
                             SGXD_PTLA_8888,
                             to.width * 4,
                             x1 - x0,
                             y1 - y0,
                             0,
                             0};
    [[maybe_unused]] int copied = sgxd_ptla_copy(&depth, nullptr, failed, sizeof(failed));
    DEBUG_ASSERT_MSG(copied == 0, "sgxd: a {}x{} depth store's part copy stopped: {}", to.width,
                     to.height, failed);
    if (to.stencil) {
        // One byte a sample: four samples a PTLA pixel.
        const u32 bytes = samples * 4 + y0 * to.width + x0;
        const SgxdPtlaCopy stencil{from.memory.gpu + bytes,
                                   to.memory.gpu + bytes,
                                   SGXD_PTLA_8888,
                                   to.width,
                                   (x1 - x0) / 4,
                                   y1 - y0,
                                   0,
                                   0};
        copied = sgxd_ptla_copy(&stencil, nullptr, failed, sizeof(failed));
        DEBUG_ASSERT_MSG(copied == 0, "sgxd: a {}x{} stencil store's part copy stopped: {}",
                         to.width, to.height, failed);
    }
}

void SgxdBackend::TouchAllTiles() {
    DEBUG_ASSERT(in_pass);
    const Recipe& r = *current;
    const SgxdRecipeDrawPrograms p =
        BlitPrograms(Data{nullptr, pass_target.lo}, r.width, r.height, false, r.width);
    Draw(SgxdRecipeDraw{0, 6, SGXD_RECIPE_ISP_NEVER, SGXD_R_FPRIM, 0, 0, nullptr}, p);
}

void SgxdBackend::MaskOutside(u32 x0, u32 x1, u32 y0, u32 y1, bool part_tiles) {
    DEBUG_ASSERT(in_pass);
    const Recipe& r = *current;
    DEBUG_ASSERT(x0 <= x1 && x1 <= r.width && y0 <= y1 && y1 <= r.height);
    // The margin as rectangles: the rows above and below the part, the columns beside it.
    // part_tiles: only the margin inside the tiles the part touches (the region clip keeps its
    // draws there).
    const u32 bx0 = part_tiles ? x0 & ~31u : 0, by0 = part_tiles ? y0 & ~31u : 0;
    const u32 bx1 = part_tiles ? std::min((x1 + 31) & ~31u, r.width) : r.width;
    const u32 by1 = part_tiles ? std::min((y1 + 31) & ~31u, r.height) : r.height;
    std::array<std::array<u32, 4>, 4> rects;
    u32 n = 0;
    const auto add = [&](u32 ax0, u32 ax1, u32 ay0, u32 ay1) {
        if (ax0 < ax1 && ay0 < ay1) {
            rects[n++] = {ax0, ax1, ay0, ay1};
        }
    };
    add(bx0, bx1, by0, y0);
    add(bx0, bx1, y1, by1);
    add(bx0, x0, y0, y1);
    add(x1, bx1, y0, y1);
    if (n == 0) {
        return;
    }
    // The blit's programs; corners on pixel edges, so a pixel is masked when its centre is in the
    // margin.
    SgxdRecipeDrawPrograms p =
        BlitPrograms(Data{nullptr, pass_target.lo}, r.width, r.height, false, r.width);
    constexpr u32 N = SGXD_PROGRAM_BLIT_VERTEX_WORDS;
    const Data vertices = AllocData(n * 6 * N * 4);
    float* v = reinterpret_cast<float*>(vertices.cpu);
    for (u32 i = 0; i < n; i++) {
        const auto [rx0, rx1, ry0, ry1] = rects[i];
        const float l = 2.0f * static_cast<float>(rx0) / static_cast<float>(r.width) - 1.0f;
        const float rr = 2.0f * static_cast<float>(rx1) / static_cast<float>(r.width) - 1.0f;
        const float t = 2.0f * static_cast<float>(ry0) / static_cast<float>(r.height) - 1.0f;
        const float b = 2.0f * static_cast<float>(ry1) / static_cast<float>(r.height) - 1.0f;
        const float corner[6][2] = {{l, t}, {rr, t}, {l, b}, {rr, t}, {rr, b}, {l, b}};
        for (u32 c = 0; c < 6; c++) {
            const float words[N] = {corner[c][0], corner[c][1], 0.5f, 1.0f, 1.0f,
                                    1.0f,         1.0f,         1.0f, 0.0f, 0.0f};
            std::memcpy(v + (i * 6 + c) * N, words, sizeof(words));
        }
    }
    const SgxdVertexFetch fetch{vertices.gpu, N * 4, N, 0};
    const auto vspds = VertexPds(&fetch, 1, blit_vertex, false);
    DEBUG_ASSERT(vspds.va != 0);
    p.vspds = vspds.va;
    p.vspds_units = vspds.units;
    Draw(SgxdRecipeDraw{0, n * 6, SGXD_RECIPE_ISP_MASK_UPDATE, SGXD_R_FPRIM, 0, 0, nullptr,
                        SGXD_RECIPE_ISP_C_MASK_CLEAR},
         p);
}

void SgxdBackend::FillDepthStore(const DepthStore& store, float z, u32 stencil) {
    const u32 samples = store.width * store.height;
    RenderBefore(store.memory.gpu, store.memory.gpu + samples * (store.stencil ? 5 : 4),
                 "a depth store's fill");
    char failed[160];
    const SgxdPtlaFill depth{store.memory.gpu, SGXD_PTLA_8888, store.width * 4, store.width,
                             store.height, 0, std::bit_cast<u32>(z)};
    [[maybe_unused]] int filled = sgxd_ptla_fill(&depth, nullptr, failed, sizeof(failed));
    DEBUG_ASSERT_MSG(filled == 0, "sgxd: a {}x{} depth store's fill stopped: {}", store.width,
                     store.height, failed);
    if (store.stencil) {
        const SgxdPtlaFill bytes{store.memory.gpu + samples * 4, SGXD_PTLA_8888, store.width,
                                 store.width / 4, store.height, 0, (stencil & 0xff) * 0x01010101u};
        filled = sgxd_ptla_fill(&bytes, nullptr, failed, sizeof(failed));
        DEBUG_ASSERT_MSG(filled == 0, "sgxd: a {}x{} stencil store's fill stopped: {}",
                         store.width, store.height, failed);
    }
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
    FlushUntiles();
    FlushStreamOut(false);
    if (draw_count != 0 && draw_count < MaxDraws && hang_test.exchange(false)) {
        DrawHang();
    }
    in_pass = false;
    passes++;
    // The copies the pass may sample, freed while it was open (FreeTextureAfterPass), go back
    // once its render is sent: whatever takes them is written behind its 3D.
    const auto give_back = [this] {
        for (const auto& [texture, bytes] : pass_frees) {
            GiveTexture(texture, bytes);
        }
        pass_frees.clear();
    };
    if (draw_count == 0) {
        // Its data may hold a side render's vertices, sent already.
        data_fence[data_slot] = pass_fences[passes % PassFences] = FenceAll();
        give_back();
        return;
    }
    Recipe& r = *current;
    Vprof::Pass(passes, pass_target.lo, r.width, r.height, draw_count, false);
    const int taken = sgxd_recipe_set_draws(&r.recipe, r.memory, draws.data(), draw_count);
    ASSERT_MSG(taken == 0, "sgxd: the pass's {} draws were not taken", draw_count);
    sgxd_recipe_restage(&r.recipe, r.memory);
    for (const CachedRange& range : cached) {
        SgxdCacheRange(range.cpu, range.bytes, SGXD_CACHE_CLEAN, CacheSite::Vertices);
    }
    SgxdSessionRender render{};
#ifdef VITA_DIAGNOSTICS
    static const bool sync_passes = SgxdFlag("syncpasses");
    if (sync_passes || replay_sync || Vprof::FrameSync()) {
        RenderSync(r, &render.ta_us, &render.d3_us);
        r.fence = data_fence[data_slot] = pass_fences[passes % PassFences] = FenceAll();
        if (replay_sync) {
            replay_times.push_back({passes, render.ta_us, render.d3_us});
        } else if (Vprof::FrameSync()) {
            Vprof::PassTimes(passes, render.ta_us, render.d3_us);
        }
#ifdef VITA_DIAGNOSTICS
        if (sync_passes && SgxdFlag("perflog")) {
            TargetTimes(r.width, r.height, render.ta_us, render.d3_us);
        }
#endif
        if (ta_us) {
            *ta_us = render.ta_us;
        }
        if (d3_us) {
            *d3_us = render.d3_us;
        }
        give_back();
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

#ifdef VITA_DIAGNOSTICS
void SgxdBackend::TargetTimes(u32 width, u32 height, u32 ta_us, u32 d3_us) {
    const u64 key = u64{pass_target.lo} << 32 | width << 16 | height;
    TargetTime& t = target_times[key];
    t.passes++;
    t.draws += draw_count;
    t.ta_us += ta_us;
    t.d3_us += d3_us;
    if (d3_us > 8000 && slow_passes++ < 48) {
        LOG_INFO(Render, "perf: slow pass: target {:#010x} {}x{}, 3D {:.1f} ms:{}", pass_target.lo,
                 width, height, d3_us / 1000.0, pass_note);
    }
    pass_note.clear();
    const u64 now = sceKernelGetProcessTimeWide();
    if (target_times_start == 0) {
        target_times_start = now;
    }
    if (now - target_times_start < 1000000) {
        return;
    }
    std::vector<std::pair<u64, TargetTime>> sorted(target_times.begin(), target_times.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.second.d3_us > b.second.d3_us; });
    for (const auto& [k, v] : sorted) {
        LOG_INFO(Render, "perf: target {:#010x} {}x{}: {} passes, {} draws, TA {:.1f} ms, 3D {:.1f} ms",
                 static_cast<u32>(k >> 32), static_cast<u32>(k >> 16) & 0xffff,
                 static_cast<u32>(k) & 0xffff, v.passes, v.draws, v.ta_us / 1000.0,
                 v.d3_us / 1000.0);
    }
    target_times.clear();
    target_times_start = now;
    const u64 total = sgxd_wait_us_total();
    u64 sited = 0;
    std::string line;
    for (const auto& [site, v] : wait_sites) {
        sited += v.second;
        line += fmt::format("; {} {:.1f} ms in {}", site, v.second / 1000.0, v.first);
    }
    if (wait_sites_mark != 0) {
        LOG_INFO(Render, "perf: GPU waits by site: elsewhere {:.1f} ms{}",
                 (total - wait_sites_mark - sited) / 1000.0, line);
    }
    static u32 kept_mark, built_mark, sync_mark, kick_mark;
    if (side_times[0] != 0) {
        LOG_INFO(Render, "perf: side renders (syncside) {}, TA {:.1f} ms, 3D {:.1f} ms",
                 side_times[0], side_times[1] / 1000.0, side_times[2] / 1000.0);
        side_times = {};
    }
    LOG_INFO(Render, "perf: render lists {} kept, {} built; sgxd calls {} sync, {} kicks",
             sgxd_session_kept_sent - kept_mark, sgxd_session_kept_built - built_mark,
             sgxd_sync_submits - sync_mark, sgxd_kicks - kick_mark);
    kept_mark = sgxd_session_kept_sent;
    built_mark = sgxd_session_kept_built;
    sync_mark = sgxd_sync_submits;
    kick_mark = sgxd_kicks;
    static u32 stream_draws_mark, stream_renders_mark;
    LOG_INFO(Render, "perf: stream-out {} draws in {} renders",
             stream_draws_sent - stream_draws_mark, stream_renders_sent - stream_renders_mark);
    stream_draws_mark = stream_draws_sent;
    stream_renders_mark = stream_renders_sent;
    static constexpr const char* finish_names[] = {
        "idle", "wait", "remap",    "fenced",       "touch",
        "poll", "kick", "PDS flip", "free surface", "free texture"};
    static_assert(std::size(finish_names) == static_cast<std::size_t>(FinishWhy::Count));
    static u32 finishes_mark;
    std::string why_line;
    u32 counted = 0;
    for (std::size_t k = 0; k < finishes.size(); k++) {
        counted += finishes[k];
        if (finishes[k] != 0) {
            why_line += fmt::format(", {} {}", finish_names[k], finishes[k]);
        }
    }
    const u32 all = sgxd_session_finishes - finishes_mark;
    LOG_INFO(Render, "perf: 3D finished alone {} (present {}{})", all, all - counted, why_line);
    finishes_mark = sgxd_session_finishes;
    finishes.fill(0);
    SceKernelFreeMemorySizeInfo free{};
    free.size = sizeof(free);
    sceKernelGetFreeMemorySize(&free);
    LOG_INFO(Render, "perf: CDRAM free {} KiB; {} recipe blocks, {} pass data slots of {} KiB",
             free.size_cdram >> 10, recipes.size(), DataSlots, DataSize >> 10);
    static u32 map_lines;
    if (map_lines++ % 30 == 0) {
        std::string map;
        u32 total = 0;
        for (uintptr_t a = 0x60000000; a < 0x68000000;) {
            SceKernelMemBlockInfo info{};
            info.size = sizeof(info);
            if (sceKernelGetMemBlockInfoByAddr(reinterpret_cast<void*>(a), &info) < 0 ||
                info.mappedSize == 0) {
                a += 256u << 10;
                continue;
            }
            const u8* const base = static_cast<const u8*>(info.mappedBase);
            const char* name = base == surfaces   ? "surfaces"
                               : base == textures ? "textures"
                               : base == shared   ? "shared"
                               : std::any_of(recipes.begin(), recipes.end(),
                                             [base](const auto& r) { return r->memory == base; })
                                   ? "recipe"
                                   : "?";
            map += fmt::format(" {:#010x}+{}K {}", reinterpret_cast<uintptr_t>(base),
                               info.mappedSize >> 10, name);
            total += info.mappedSize;
            a = std::max(a + (256u << 10), reinterpret_cast<uintptr_t>(base) + info.mappedSize);
        }
        LOG_INFO(Render, "perf: CDRAM blocks {} KiB in all:{}", total >> 10, map);
    }
    if (map_lines == 1) {
        std::string line;
        u32 blocks = 0;
        for (uint64_t a = 0x00010000; a < 0x100000000ull;) {
            if (a == 0x60000000) {
                a = 0x68000000;
                continue;
            }
            SceKernelMemBlockInfo info{};
            info.size = sizeof(info);
            if (sceKernelGetMemBlockInfoByAddr(reinterpret_cast<void*>(static_cast<uintptr_t>(a)),
                                               &info) < 0 ||
                info.mappedSize == 0) {
                a += 256u << 10;
                continue;
            }
            const uint64_t base = reinterpret_cast<uintptr_t>(info.mappedBase);
            line += fmt::format(" {:#010x}+{}K t{:08x} m{:x}", base, info.mappedSize >> 10,
                                static_cast<u32>(info.type), info.memoryType);
            if (++blocks % 16 == 0) {
                LOG_INFO(Render, "perf: blocks outside the CDRAM window:{}", line);
                line.clear();
            }
            a = std::max(a + (256u << 10), base + info.mappedSize);
        }
        LOG_INFO(Render, "perf: blocks outside the CDRAM window:{} ({} in all)", line, blocks);
    }
    LOG_INFO(Render,
             "perf: microkernel {} shapes ({} instructions), {} flips, {} jobs, {} SW_EVENT "
             "interrupts, {} IRQ 52 in all",
             sgxd_mkq_compiles, sgxd_mkq_instructions, sgxd_mkq_flips, sgxd_mkq_jobs_sent(),
             events.sw_events, events.irqs);
    wait_sites.clear();
    wait_sites_mark = total;
}
#endif

void SgxdBackend::WaitSite([[maybe_unused]] const char* site, [[maybe_unused]] u64 before) {
#ifdef VITA_DIAGNOSTICS
    const u64 us = sgxd_wait_us_total() - before;
    if (us != 0) {
        auto& v = wait_sites[site];
        v.first++;
        v.second += us;
    }
#endif
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
    RenderIdle("a synchronous render");
    SgxdTrace("sync");
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
    if (std::FILE* f = std::fopen("host0:azahar-sgxd-code.bin", "wb")) {
        const u32 head[4] = {0x45535355, 1, static_cast<u32>(placed_code.size()), code_used};
        std::fwrite(head, 4, 4, f);
        std::fwrite(placed_code.data(), sizeof(PlacedCode), placed_code.size(), f);
        std::fwrite(shared + code_begin, 1, code_used, f);
        std::fclose(f);
        LOG_CRITICAL(Render, "sgxd: {} USSE programs ({} KiB of code) written to host0",
                     placed_code.size(), code_used >> 10);
    }
    if (sgxd_mkq_state()) {
        u32 instructions = 0;
        const volatile u64* code = sgxd_mkq_code(&instructions);
        u32 words = 0;
        const volatile u32* memory = sgxd_mkq_memory(&words);
        if (std::FILE* f = std::fopen("host0:azahar-sgxd-mk-state.bin", "wb")) {
            for (u32 n = 0; n < words; n++) {
                const u32 word = memory[n];
                std::fwrite(&word, 4, 1, f);
            }
            std::fclose(f);
        }
        if (std::FILE* f = std::fopen("host0:azahar-sgxd-mk.bin", "wb")) {
            for (u32 n = 0; n < SGXD_MK_STATE_WORDS; n++) {
                const u32 word = sgxd_mkq_state()[n];
                std::fwrite(&word, 4, 1, f);
            }
            for (u32 n = 0; n < instructions; n++) {
                const u64 word = code[n];
                std::fwrite(&word, 8, 1, f);
            }
            std::fclose(f);
        }
        LOG_CRITICAL(Render, "sgxd: the microkernel's state and {} instructions written to host0",
                     instructions);
    }
}
#endif

void SgxdBackend::Fault(const Recipe& r, const char* failed) {
    // The reason before the dumps: their writes to host0 can outlast the run.
    LOG_CRITICAL(Render, "sgxd: pass {} stopped: {}", passes, failed);
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
    blit_vertex = sgxd_program_doutu0(HeapCodeBase(0),
                                      PlaceCode(0, {words, sgxd_program_blit_vertex(words)}), 0);
    blit_fragment = sgxd_program_doutu0(
        HeapCodeBase(1), PlaceCode(1, {words, sgxd_program_blit_fragment(words)}), 0);
    untile_fragment = sgxd_program_doutu0(
        HeapCodeBase(1), PlaceCode(1, {words, sgxd_program_screen_fragment(words)}), 0);
    merge_vertex = sgxd_program_doutu0(HeapCodeBase(0),
                                       PlaceCode(0, {words, sgxd_program_merge_vertex(words)}), 0);
    // The merge fragment: the blit fragment with an `and o0, pa4, pa5` before its end.
    sgxd_program_blit_fragment(words);
    Usse::Encoder e;
    e.Bitwise(Usse::BwOp::And, Usse::Reg::O(0), Usse::IOp::R(Usse::Reg::Pa(4)),
              Usse::IOp::R(Usse::Reg::Pa(5)));
    const u64 merge[3] = {words[0], e.Words()[0], words[2]};
    merge_fragment = sgxd_program_doutu0(HeapCodeBase(1), PlaceCode(1, merge), 0);
    raw64_fragment = sgxd_program_doutu0(
        HeapCodeBase(1), PlaceCode(1, {words, sgxd_program_raw64_fragment(words)}), 0);
    blit_ready = true;
}

SgxdRecipeDrawPrograms SgxdBackend::BlitPrograms(const Data& source, u32 width, u32 height,
                                                 bool mirror, u32 source_stride, u32 scale_x,
                                                 u32 scale_y) {
    DEBUG_ASSERT((scale_x == 1 || scale_x == 2) && (scale_y == 1 || scale_y == 2));
    const u32 stride = source_stride != 0 ? source_stride : width;
    PlaceFixedPrograms();
    // Two triangles over the target: x, y, z, w, a white colour (unused), u, v. Pixel row y
    // reads texel row y: both follow y the same way.
    constexpr u32 N = SGXD_PROGRAM_BLIT_VERTEX_WORDS;
    const Data vertices = AllocData(6 * N * 4);
    static constexpr float corner[6][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, -1}, {1, 1}, {-1, 1}};
    float* v = reinterpret_cast<float*>(vertices.cpu);
    for (u32 n = 0; n < 6; n++) {
        const float x = corner[n][0], y = corner[n][1];
        // Scaled, output pixel x's centre is source column 2x + 1, between texels 2x and
        // 2x + 1: the bilinear read weighs both 32/64 (exact; the filter's weights are
        // 6 bits) and rounds halves up, where the
        // PICA's box filter truncates (upstream's sw_blitter): one LSB on odd sums.
        const float u = (x + 1) / 2 * width * scale_x / stride;
        const float words[N] = {x, y, 0.5f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, u, (y + 1) / 2};
        std::memcpy(v + n * N, words, sizeof(words));
    }

    SgxdRecipeDrawPrograms p{};
    const SgxdVertexFetch fetch{vertices.gpu, N * 4, N, 0};
    const auto vspds = VertexPds(&fetch, 1, blit_vertex, false);
    DEBUG_ASSERT(vspds.va != 0);
    p.vspds = vspds.va;
    p.vspds_units = vspds.units;
    // The dummy secondaries at the code bases (the recipe's CV_DUMMY_VS and CF_DUMMY_FS).
    const auto vsec = SecondaryPds(0, 0, sgxd_program_doutu0(0, 0, 0));
    DEBUG_ASSERT(vsec.va != 0);
    p.vsec = vsec.va;
    p.vsec_units = vsec.units;
    p.vsec_sa = 0;
    const auto fsec = SecondaryPds(0, 0, sgxd_program_doutu0(1, 0, 0));
    DEBUG_ASSERT(fsec.va != 0);
    p.fsec = fsec.va;
    p.fsec_units = fsec.units;
    // The texel comes back unconverted (FCONV 0), the surface's word as it is; mirrored, the
    // screen program converts it (U8U8U8U8 under swizzle 2) and packs U8888 again.
    u32 tex[4];
    sgxd_recipe_texture_words_format(source.gpu, stride, height * scale_y,
                                     scale_x != 1 || scale_y != 1, 0x0c, mirror ? 2 : 0, tex);
    const PdsRef fprim =
        mirror ? ScreenPixelPds(untile_fragment, tex) : BlitPixelPds(blit_fragment, tex);
    DEBUG_ASSERT(fprim.va != 0);
    p.fprim = fprim.va;
    p.fprim_units = fprim.units;
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

void SgxdBackend::TransferTiledFromGuest(const PicaTarget& target, const UntileSource& source,
                                         u32 scale_x, u32 scale_y, u32* ta_us, u32* d3_us) {
    OpenPass(target.gpu, target.height, target.width, target.pack, 0, 1.0f, &target);
    pass_source = {source.gpu, source.gpu + source.width * source.height * source.bits / 8};
    DrawTiled(source, true, scale_x, scale_y);
    EndPass(ta_us, d3_us);
}

void SgxdBackend::TransferLinearToTiled(const PicaTarget& target, const UntileSource& source,
                                        bool flip, u32* ta_us, u32* d3_us) {
    OpenPass(target.gpu, target.height, target.width, target.pack, 0, 1.0f, &target);
    pass_source = {source.gpu, source.gpu + source.width * source.height * source.bits / 8};
    PlaceFixedPrograms();
    const Recipe& r = *current;
    // The target is transposed: its x is the picture's row, its y the column. Flipped, input row y
    // is output row rows - 1 - y.
    const u32 rows = r.width, columns = r.height;
    constexpr u32 N = SGXD_PROGRAM_BLIT_VERTEX_WORDS;
    const Data vertices = AllocData(QuadVertexWords * 4);
    const float u1 = static_cast<float>(columns) / static_cast<float>(source.width);
    const float v_end = static_cast<float>(rows) / static_cast<float>(source.height);
    const float v0 = flip ? v_end : 0, v1 = flip ? 0 : v_end;
    const float uv[4][2] = {{0, v0}, {0, v1}, {u1, v0}, {u1, v1}};
    PutQuad(reinterpret_cast<float*>(vertices.cpu), r.width, r.height, 0, 0,
            static_cast<float>(rows), static_cast<float>(columns), uv);

    SgxdRecipeDrawPrograms p{};
    const SgxdVertexFetch fetch{vertices.gpu, N * 4, N, 0};
    const auto vspds = VertexPds(&fetch, 1, blit_vertex, false);
    DEBUG_ASSERT(vspds.va != 0);
    p.vspds = vspds.va;
    p.vspds_units = vspds.units;
    const auto vsec = SecondaryPds(0, 0, sgxd_program_doutu0(0, 0, 0));
    DEBUG_ASSERT(vsec.va != 0);
    p.vsec = vsec.va;
    p.vsec_units = vsec.units;
    p.vsec_sa = 0;
    const auto fsec = SecondaryPds(0, 0, sgxd_program_doutu0(1, 0, 0));
    DEBUG_ASSERT(fsec.va != 0);
    p.fsec = fsec.va;
    p.fsec_units = fsec.units;
    sgxd_program_vertex_dms(N, 0, 0, N, &p.vdm_w2, &p.vdm_w4, &p.vdm_w5);
    p.pixel_size = sgxd_program_pixel_dms(SGXD_PROGRAM_BLIT_PA, 0, 0);
    p.outselects = SGXD_PROGRAM_BLIT_OUTSELECTS;
    p.texsize = SGXD_PROGRAM_BLIT_TEXSIZE;
    const float viewport[6] = {r.width / 2.0f, r.width / 2.0f, r.height / 2.0f, r.height / 2.0f,
                               0.0f, 1.0f};
    std::memcpy(p.viewport, viewport, sizeof(viewport));
    p.indices = 0;
    p.primitive = SGXD_PROGRAM_TRIANGLES;
    // The texel as the untile reads the format (its code and swizzle), point sampled; the
    // screen program converts it (half4) and packs U8888 for the target's pack.
    u32 tex[4];
    sgxd_recipe_texture_words_format(source.gpu, source.width, source.height, 0, source.code,
                                     source.swizzle, tex);
    const PdsRef fprim = ScreenPixelPds(untile_fragment, tex);
    DEBUG_ASSERT(fprim.va != 0);
    p.fprim = fprim.va;
    p.fprim_units = fprim.units;
    // ISP A: opaque, DCMP ALWAYS, DWRITEDIS, BPRES; no culling (DrawTiled's).
    const u32 isp = 7u << 22 | 1u << 20 | 1u << 9;
    const u32 clip = SGXD_RECIPE_CLIP(0, 0, (r.width + 31) / 32 - 1, (r.height + 31) / 32 - 1);
    Draw(SgxdRecipeDraw{0, 6, isp, SGXD_R_FPRIM, 0, clip, nullptr}, p);
    EndPass(ta_us, d3_us);
}

void SgxdBackend::TransferFromGuest(const UntileSource& source, u32 dest_gpu, u32 width,
                                    u32 height, u32 pack, bool mirror, u32* ta_us, u32* d3_us,
                                    u32 scale_x, u32 scale_y, bool flip) {
    OpenPass(dest_gpu, width, height, pack, 0, 1.0f);
    if (!mirror) {
        pass_source = {source.gpu, source.gpu + source.width * source.height * source.bits / 8};
        DrawTiled(source, false, scale_x, scale_y, 4, 0, 0, flip);
        EndPass(ta_us, d3_us);
        return;
    }
    DEBUG_ASSERT_MSG(!flip, "sgxd: a flipped display transfer into a mirrored pack is not built");
    // The untile writes whole 32x32 tiles (UntileTargetFor), rows a multiple of 32 apart; the
    // picture is the pass's data until its render completes.
    const u32 stride = AlignUp(source.width, 32);
    const u32 bytes = stride * AlignUp(source.height, 32) * 4;
    DEBUG_ASSERT_MSG(bytes <= DataSize / 2,
                     "sgxd: a {}x{} display transfer from guest memory: more than the pass's data",
                     source.width, source.height);
    const Data picture = AllocData(bytes, 0x1000);
    Untile(source, picture.gpu, stride);
    pass_source = {picture.gpu, picture.gpu + stride * height * scale_y * 4};
    const SgxdRecipeDrawPrograms p =
        BlitPrograms(picture, width, height, mirror, stride, scale_x, scale_y);
    const u32 isp = 7u << 22 | 1u << 20 | 1u << 9;
    Draw(SgxdRecipeDraw{0, 6, isp, SGXD_R_FPRIM, 0, 0, nullptr}, p);
    EndPass(ta_us, d3_us);
}

namespace {

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

u32 PutRows(float* v, const UntileTarget& t, u32 w, u32 y0, u32 y1, const float (&uv)[4][2]) {
    return PutRowsAt(v, t.width, t.height, w * t.per_row, w, 0, y0, y1, uv);
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
                             u32 dest_stride, u32 width, u32 height, bool merge, bool twiddled,
                             u32 pack, bool double_pixels) {
    // Inside a pass or not: the vertices are the caller's, in the textures pool or the pass data.
    ASSERT(!list.empty() && list.size() <= MaxDraws);
    DEBUG_ASSERT(dest_stride >= std::max(width, 32u) && dest_stride % 32 == 0 &&
                 width <= UntileSide && height <= UntileSide);
    Recipe& r = RecipeFor(UntileSide, UntileSide, Kind::Side);
    PlaceFixedPrograms();
    // At the target's size inside the 1024x1024 layout: the TE and 3D walk only its macrotiles, the
    // PBE takes its size. The PDS keeps a program by address, so another size's comes from the
    // flipping half.
    [[maybe_unused]] const int sized = sgxd_recipe_set_size(&r.recipe, r.memory, width, height);
    DEBUG_ASSERT_MSG(sized == 0, "sgxd: a {}x{} side render does not fit the side recipe", width,
                     height);
    if (width != r.recipe.layout_width || height != r.recipe.layout_height) {
        const PdsRef term = TermPds(r.recipe);
        DEBUG_ASSERT(term.va != 0);
        sgxd_recipe_set_term(&r.recipe, term.va, term.units);
    }
    sgxd_recipe_set_double_pixels(&r.recipe, double_pixels ? 1 : 0);
    const u32 N = merge ? SGXD_PROGRAM_MERGE_VERTEX_WORDS : SGXD_PROGRAM_BLIT_VERTEX_WORDS;

    SgxdRecipeDrawPrograms p{};
    const auto vsec = SecondaryPds(0, 0, sgxd_program_doutu0(0, 0, 0));
    DEBUG_ASSERT(vsec.va != 0);
    p.vsec = vsec.va;
    p.vsec_units = vsec.units;
    p.vsec_sa = 0;
    const auto fsec = SecondaryPds(0, 0, sgxd_program_doutu0(1, 0, 0));
    DEBUG_ASSERT(fsec.va != 0);
    p.fsec = fsec.va;
    p.fsec_units = fsec.units;
    sgxd_program_vertex_dms(N, 0, 0, N, &p.vdm_w2, &p.vdm_w4, &p.vdm_w5);
    p.pixel_size = sgxd_program_pixel_dms(SGXD_PROGRAM_BLIT_PA, 0, 0);
    p.outselects = merge ? SGXD_PROGRAM_MERGE_OUTSELECTS : SGXD_PROGRAM_BLIT_OUTSELECTS;
    p.texsize = merge ? SGXD_PROGRAM_MERGE_TEXSIZE : SGXD_PROGRAM_BLIT_TEXSIZE;
    const float viewport[6] = {width / 2.0f, width / 2.0f, height / 2.0f, height / 2.0f,
                               0.0f,         1.0f};
    std::memcpy(p.viewport, viewport, sizeof(viewport));
    p.indices = 0;
    p.primitive = SGXD_PROGRAM_TRIANGLES;

    // The quads' right and bottom edges put the next tile column and row in the TA's lists, whose
    // background would land past the target: the clip keeps the tiles the target covers (LastTile).
    const auto [last_x, last_y] = LastTile(width, height);
    const u32 clip = SGXD_RECIPE_CLIP(0, 0, last_x, last_y);
    const u32 isp = 7u << 22 | 1u << 20 | 1u << 9;
    untile_draws.resize(list.size());
    untile_programs.assign(list.size(), p);
    // moved to its slot by the viewport.
    u32 fetched = 0;
    PdsRef vspds{};
    for (std::size_t d = 0; d < list.size(); d++) {
        const SideDraw& sd = list[d];
        const u32 from = sd.vertices != 0 ? sd.vertices : vertices.gpu;
        if (from != fetched || vspds.va == 0) {
            const SgxdVertexFetch fetch{from, N * 4, N, 0};
            vspds = VertexPds(&fetch, 1, merge ? merge_vertex : blit_vertex, false);
            DEBUG_ASSERT(vspds.va != 0);
            fetched = from;
        }
        untile_programs[d].vspds = vspds.va;
        untile_programs[d].vspds_units = vspds.units;
        if (sd.view_width != 0) {
            const float w = sd.view_width / 2.0f, h = sd.view_height / 2.0f;
            const float view[6] = {w + sd.view_x, w, h + sd.view_y, h, 0.0f, 1.0f};
            std::memcpy(untile_programs[d].viewport, view, sizeof(view));
        }
        // the word unconverted.
        const PdsRef fprim = merge          ? MergePixelPds(merge_fragment, sd.tex, sd.tex1)
                             : sd.raw64     ? Raw64PixelPds(raw64_fragment, sd.tex)
                             : sd.converted ? ScreenPixelPds(untile_fragment, sd.tex)
                                            : BlitPixelPds(blit_fragment, sd.tex);
        DEBUG_ASSERT(fprim.va != 0);
        untile_programs[d].fprim = fprim.va;
        untile_programs[d].fprim_units = fprim.units;
        untile_draws[d] =
            SgxdRecipeDraw{sd.first, sd.count, isp, SGXD_R_FPRIM, 0, clip, &untile_programs[d]};
    }
    sgxd_recipe_set_target(&r.recipe, r.memory, dest_gpu, dest_stride);
    // MEMLAYOUT (26..27) 2: TWIDDLED.
    sgxd_recipe_set_target_pack(&r.recipe, r.memory, pack | (twiddled ? 2u << 26 : 0u));
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
    Recipe& r = RecipeFor(UntileSide, UntileSide, Kind::Side);
    PlaceFixedPrograms();
    // At its 32x32 target's size, as SideRender sizes the recipe (whatever it drew last).
    [[maybe_unused]] const int sized = sgxd_recipe_set_size(&r.recipe, r.memory, 32, 32);
    DEBUG_ASSERT(sized == 0);
    const PdsRef term = TermPds(r.recipe);
    DEBUG_ASSERT(term.va != 0);
    sgxd_recipe_set_term(&r.recipe, term.va, term.units);
    sgxd_recipe_set_double_pixels(&r.recipe, 0);
    if (stream_target.cpu == nullptr) {
        stream_target = AllocSurface(32, 32);
    }
    const auto fsec = SecondaryPds(0, 0, sgxd_program_doutu0(1, 0, 0));
    DEBUG_ASSERT(fsec.va != 0);
    const u32 tex[4] = {};
    const PdsRef fprim = BlitPixelPds(blit_fragment, tex);
    DEBUG_ASSERT(fprim.va != 0);
    const float viewport[6] = {16.0f, 16.0f, 16.0f, 16.0f, 0.0f, 1.0f};
    constexpr u32 clip = SGXD_RECIPE_CLIP(0, 0, 0, 0);
    untile_programs.resize(queued.size());
    untile_draws.resize(queued.size());
    const u32 pixel_size = sgxd_program_pixel_dms(SGXD_PROGRAM_BLIT_PA, 0, 0);
    for (std::size_t d = 0; d < queued.size(); d++) {
        SgxdRecipeDrawPrograms& p = untile_programs[d];
        p = queued[d].programs;
        p.fsec = fsec.va;
        p.fsec_units = fsec.units;
        p.fprim = fprim.va;
        p.fprim_units = fprim.units;
        p.pixel_size = pixel_size;
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

void SgxdBackend::Untile(const UntileSource& src, u32 dest_gpu, u32 dest_stride, bool swizzled,
                         bool transposed) {
    const u32 w = src.width, h = src.height;
    // A texture's sides are powers of two; a display transfer's picture has any height of whole
    // tiles (416 for a 3D-mode screen).
    const bool pot = (w & (w - 1)) == 0 && (h & (h - 1)) == 0;
    const bool etc1 = src.code == UntileEtc1 || src.code == UntileEtc1A4;
    DEBUG_ASSERT_MSG(w >= 8 && h >= 8 && w <= UntileSide && h <= UntileSide && w % 8 == 0 &&
                         h % 8 == 0 && (swizzled || etc1 ? pot : true) &&
                         !(transposed && (swizzled || etc1)),
                     "sgxd: a {}x{} picture: the untile takes sides of whole tiles up to {} "
                     "(powers of two for ETC1 and cube faces; transposed, neither)",
                     w, h, UntileSide);
    if (etc1) {
        UntileEtc1Blocks(src, dest_gpu, dest_stride, swizzled);
        return;
    }
    const u32 tile_rows = h / 8;
    // Transposed, the target is h wide and w tall, and the texture's own orientation: tile row
    // ty is target columns 8ty..8ty+7, its texel (sx, sy) at (8ty + sx, sy % w).
    const UntileTarget t = transposed ? UntileTarget{h, w, dest_stride, 1, false}
                                      : UntileTargetFor(w, h, dest_stride, swizzled);
    const u32 quads = t.per_row == 1 ? 1 : 8; // a tile row's
    // Tile row ty holds PICA rows 8ty..8ty+7, image rows 8ty..8ty+7. Along the quad, image
    // x is the band's SWIZZLED row (ty's local row) * w + x, image y the column: pixel
    // centres land on texel centres.
    const Geometry& g = GeometryFor(
        GeometryKey(transposed ? 6 : 1, w, h, dest_stride, false, swizzled),
        SGXD_PROGRAM_BLIT_VERTEX_WORDS * 4, [&](float* v, Geometry& geometry) {
            u32 vertex_count = 0;
            geometry.bands = TileBands(w, tile_rows, quads, vertex_count);
            for (const Band& band : geometry.bands) {
                const u32 band_texels = std::bit_ceil(band.rows * w);
                float* const band_v =
                    v ? v + band.first_vertex * SGXD_PROGRAM_BLIT_VERTEX_WORDS : nullptr;
                for (u32 row = 0; row < band.rows && v; row++) {
                    const u32 ty = band.first_row + row;
                    const float v0 = static_cast<float>(row * w) / band_texels;
                    const float v1 = static_cast<float>((row + 1) * w) / band_texels;
                    if (transposed) {
                        const float uv[4][2] = {{0, v0}, {1, v0}, {0, v1}, {1, v1}};
                        PutQuad(band_v + row * QuadVertexWords, t.width, t.height,
                                static_cast<float>(8 * ty), 0, static_cast<float>(8 * ty + 8),
                                static_cast<float>(w), uv);
                    } else {
                        const float uv[4][2] = {{0, v0}, {0, v1}, {1, v0}, {1, v1}};
                        PutRows(band_v + row * quads * QuadVertexWords, t, w, 8 * ty, 8 * (ty + 1),
                                uv);
                    }
                }
            }
            return vertex_count;
        });
    const std::vector<Band>& bands = g.bands;
    std::vector<SideDraw> list(bands.size());
    for (std::size_t b = 0; b < bands.size(); b++) {
        const Band& band = bands[b];
        const u32 band_texels = std::bit_ceil(band.rows * w); // the band's SWIZZLED height
        SideTextureWords(0, src.code, src.swizzle, 8, band_texels,
                         src.gpu + band.first_row * 8 * w * src.bits / 8, list[b].tex,
                         src.palette);
        list[b].converted = true;
        list[b].first = band.first_vertex;
        list[b].count = band.rows * quads * 6;
    }
    SideRender(g.vertices, list, dest_gpu, t.stride, t.width, t.height, false, t.twiddled);
}

u32 SgxdBackend::ChainBytes(u32 width, u32 height, u32 levels, u32 bytes) {
    u32 end = 0;
    for (const ChainRender& r : ChainRenders(width, height, levels)) {
        end = std::max(end, (r.offset + r.stride * AlignUp(r.rows, 32)) * bytes);
    }
    return end;
}

void SgxdBackend::UntileChain(const UntileSource& src, u32 levels, u32 dest_gpu) {
    const u32 w0 = src.width, h0 = src.height;
    DEBUG_ASSERT_MSG(levels >= 1 && std::has_single_bit(w0) && std::has_single_bit(h0) &&
                         w0 <= UntileSide && h0 <= UntileSide && (w0 >> (levels - 1)) >= 8 &&
                         (h0 >> (levels - 1)) >= 8 && src.code != UntileEtc1 &&
                         src.code != UntileEtc1A4,
                     "sgxd: a {}x{} chain of {} levels (format {:#x}): the chain untile takes "
                     "power-of-two sides from 8 to {}, not ETC1",
                     w0, h0, levels, src.code, UntileSide);
    std::vector<GeometryDraw> draws;
    for (const ChainRender& cr : ChainRenders(w0, h0, levels)) {
        const u32 vertex_count = ChainVertices(cr, w0, h0, src.bits, nullptr, draws);
        bool fresh = false;
        const Data vertices =
            GeometryVertices(GeometryKey(7, w0, h0, cr.first | cr.end << 4, false, false),
                             vertex_count * SGXD_PROGRAM_BLIT_VERTEX_WORDS * 4, fresh);
        if (fresh) {
            ChainVertices(cr, w0, h0, src.bits, reinterpret_cast<float*>(vertices.cpu), draws);
        }
        std::vector<SideDraw> list(draws.size());
        for (std::size_t n = 0; n < draws.size(); n++) {
            SideTextureWords(0, src.code, src.swizzle, 8, draws[n].view_rows,
                             src.gpu + draws[n].source, list[n].tex, src.palette);
            list[n].converted = true;
            list[n].first = draws[n].first;
            list[n].count = draws[n].count;
        }
        SideRender(vertices, list, dest_gpu + cr.offset * (src.byte_copy ? 1 : 4), cr.stride,
                   cr.stride, cr.rows, false, false,
                   src.byte_copy ? SGXD_RECIPE_PACK_U8_A : SGXD_RECIPE_PACK_U8888);
    }
}

u32 SgxdBackend::UnitBytes(u32 kind) {
    switch (kind) {
    case BatchChain256:
        return 256 * 4;
    case BatchChain512:
        return 512 * 4;
    case BatchChainWide:
        return UntileSide * 4;
    case BatchBytes256:
        return 256;
    case BatchBytes512:
        return 512;
    case BatchBytesWide:
        return UntileSide;
    case BatchAlpha:
        return AlphaBatchStride;
    default: {
        const u32 side = 32u << (kind - BatchEtc1);
        return side * side * 8;
    }
    }
}

u32 SgxdBackend::MaxUnits(u32 kind) {
    return kind >= BatchEtc1 && kind < BatchAlpha ? UntileSide >> (5 + kind - BatchEtc1)
                                                   : UntileSide;
}

u32 SgxdBackend::SlotUnits(u32 kind) {
    return std::max(1u, 0x1000 / UnitBytes(kind));
}

namespace {
/// A batch's block at its first member: room for this many bytes of members, or the member's
/// own slot where that is more.
constexpr u32 UntileBlockBytes = 1u << 20;
} // namespace

SgxdBackend::UntileCopy SgxdBackend::QueueUntile(const UntileSource& src, u32 levels) {
    const u32 w0 = src.width, h0 = src.height;
    const bool split = src.code == UntileEtc1A4;
    const bool etc1 = split || src.code == UntileEtc1;
    DEBUG_ASSERT_MSG(levels >= 1 && std::has_single_bit(w0) && std::has_single_bit(h0) &&
                         w0 <= UntileSide && h0 <= UntileSide && (w0 >> (levels - 1)) >= 8 &&
                         (h0 >> (levels - 1)) >= 8 && (!split || w0 <= Etc1A4MaxWidth),
                     "sgxd: a {}x{} chain of {} levels (format {:#x}): the untile takes "
                     "power-of-two sides from 8 to {}, ETC1A4 up to {} wide",
                     w0, h0, levels, src.code, UntileSide, Etc1A4MaxWidth);
    DEBUG_ASSERT(!flushing_untiles);
    constexpr u32 VertexBytes = SGXD_PROGRAM_BLIT_VERTEX_WORDS * 4;
    const Geometry* geometry = nullptr;
    u32 batch = 0, units = 0;
    if (etc1) {
        const u32 side = Etc1TargetFor(w0, h0, levels).height;
        batch = BatchEtc1 + static_cast<u32>(std::countr_zero(side / 32));
        units = Etc1Squares(w0, h0, levels);
        geometry = &GeometryFor(
            GeometryKey(9, w0, h0, levels, false, false), VertexBytes,
            [&](float* v, Geometry& g) { return Etc1Vertices(w0, h0, levels, v, g.draws); });
    } else {
        const u32 stride = ChainBatchStride(w0);
        const ChainRender cr = ChainAt(w0, h0, levels, stride);
        if (cr.rows > UntileSide) {
            // More rows than a render's (a 1024x1024 chain): a copy of its own, untiled now.
            const u32 bytes = ChainBytes(w0, h0, levels, src.byte_copy ? 1 : 4);
            const Data own = AllocTexture(bytes);
            UntileChain(src, levels, own.gpu);
            return {own, bytes, {}, 0};
        }
        batch = (stride == UntileSide ? BatchChainWide
                 : stride == 512      ? BatchChain512
                                      : BatchChain256) +
                (src.byte_copy ? BatchBytes256 - BatchChain256 : 0);
        units = cr.rows;
        geometry = &GeometryFor(
            GeometryKey(15, w0, h0, levels | src.bits << 4, false, false), VertexBytes,
            [&](float* v, Geometry& g) { return ChainVertices(cr, w0, h0, src.bits, v, g.draws); });
    }
    units = AlignUp(units, SlotUnits(batch));
    DEBUG_ASSERT(units <= MaxUnits(batch));
    const u32 batch_draws = static_cast<u32>(geometry->draws.size());
    // A split ETC1A4's A8 slot, a draw a level, and its gather's rows, a draw for the colour
    // words and one a level.
    const u32 alpha_units =
        split ? AlignUp(ChainAt(w0, h0, levels, AlphaBatchStride).rows, SlotUnits(BatchAlpha))
              : 0;
    const u32 member_gather_rows = split ? Etc1A4GatherFor(w0, h0, levels).rows : 0;
    const auto fits = [this](u32 kind, u32 n, u32 d) {
        const UntileBatch& b = untile_batches[kind];
        return b.draws + d <= MaxDraws && (!b.block.cpu || b.used + n <= b.capacity);
    };
    if (!fits(batch, units, batch_draws) ||
        (split && (!fits(BatchAlpha, alpha_units, levels) ||
                   gather_rows + member_gather_rows > UntileSide ||
                   gather_draws + 1 + levels > MaxDraws))) {
        FlushUntiles();
    }
    const auto take = [this](u32 kind, u32 n, u32 d) {
        UntileBatch& b = untile_batches[kind];
        const u32 unit = UnitBytes(kind);
        if (!b.block.cpu) {
            // A LINEAR target in whole tile rows: the tiles its render stores stay in the block.
            const u32 band = kind >= BatchEtc1 && kind < BatchAlpha ? 1 : 32;
            b.capacity =
                std::min(MaxUnits(kind), std::max(AlignUp(n, band), UntileBlockBytes / unit));
            b.block = AllocTexture(b.capacity * unit);
            b.used = 0;
            b.draws = 0;
        }
        const u32 at = b.used;
        b.used += n;
        b.draws += d;
        return Data{b.block.cpu + at * unit, b.block.gpu + at * unit};
    };
    UntileCopy copy{take(batch, units, batch_draws), units * UnitBytes(batch), {}, 0};
    UntileMember m{src, levels, batch, 0, 0, 0};
    m.geometry = geometry;
    m.at = (copy.colour.gpu - untile_batches[batch].block.gpu) / UnitBytes(batch);
    if (split) {
        m.gather = &GeometryFor(GeometryKey(13, w0, h0, levels, true, false), VertexBytes,
                                [&](float* v, Geometry& g) {
                                    return Etc1A4GatherVertices(w0, h0, levels, v, g.draws);
                                });
        const ChainRender alpha_cr = ChainAt(w0, h0, levels, AlphaBatchStride);
        m.alpha = &GeometryFor(GeometryKey(14, w0, h0, levels, true, false), VertexBytes,
                               [&](float* v, Geometry& g) {
                                   return Etc1A4AlphaVertices(alpha_cr, w0, h0, levels, v, g.draws);
                               });
        copy.alpha = take(BatchAlpha, alpha_units, levels);
        copy.alpha_bytes = alpha_units * AlphaBatchStride;
        m.alpha_at = (copy.alpha.gpu - untile_batches[BatchAlpha].block.gpu) / AlphaBatchStride;
        m.gather_row = gather_rows;
        gather_rows += member_gather_rows;
        gather_draws += 1 + levels;
    }
    untile_members.push_back(m);
    return copy;
}

void SgxdBackend::FlushUntiles() {
    if (untile_members.empty()) {
        return;
    }
    DEBUG_ASSERT(!flushing_untiles);
    flushing_untiles = true;
    constexpr u32 G = Etc1A4GatherStride;
    std::vector<SideDraw> list;
    const auto add = [&](const Geometry& geometry, u32 view_x, u32 view_y, u32 view_width,
                         u32 view_height, const auto& texture) {
        for (const GeometryDraw& d : geometry.draws) {
            SideDraw& sd = list.emplace_back();
            texture(d, sd);
            sd.first = d.first;
            sd.count = d.count;
            sd.vertices = geometry.vertices.gpu;
            sd.view_x = view_x;
            sd.view_y = view_y;
            sd.view_width = view_width;
            sd.view_height = view_height;
        }
    };

    // The gather first: the ETC1 and alpha renders read the split members' words from it.
    Data scratch{};
    u32 scratch_bytes = 0;
    if (gather_rows != 0) {
        scratch_bytes = G * 4 * AlignUp(gather_rows, 32u);
        scratch = AllocTexture(scratch_bytes);
        list.clear();
        for (const UntileMember& m : untile_members) {
            if (m.source.code != UntileEtc1A4) {
                continue;
            }
            const u32 w = m.source.width, h = m.source.height;
            add(*m.gather, 0, m.gather_row, G, Etc1A4GatherFor(w, h, m.levels).rows,
                [&](const GeometryDraw& d, SideDraw& sd) {
                    SideTextureWords(3, 0x0c, 0, d.view_width, d.view_rows,
                                     m.source.gpu + d.source, sd.tex);
                });
        }
        SideRender({}, list, scratch.gpu, G, G, gather_rows);
    }

    for (u32 k = 0; k < BatchAlpha; k++) {
        const UntileBatch& b = untile_batches[k];
        if (!b.block.cpu) {
            continue;
        }
        const bool etc1 = k >= BatchEtc1;
        const bool bytes = k >= BatchBytes256 && k < BatchEtc1;
        const u32 pack = bytes ? SGXD_RECIPE_PACK_U8_A : SGXD_RECIPE_PACK_U8888;
        // A LINEAR batch whose members share one width under its stride is drawn in rows of that
        // width: a level as wide as the rows is a quad a tile row, not one a texel row.
        const u32 batch_stride = etc1 ? 0 : UnitBytes(k) / (bytes ? 1 : 4);
        u32 narrow = 0;
        for (const UntileMember& m : untile_members) {
            if (!etc1 && m.batch == k) {
                narrow = narrow == 0 || narrow == m.source.width ? m.source.width : ~0u;
            }
        }
        if (narrow == ~0u || narrow < 32 || narrow >= batch_stride ||
            b.used * (batch_stride / narrow) > UntileSide) {
            narrow = 0;
        }
        list.clear();
        for (const UntileMember& m : untile_members) {
            if (m.batch != k) {
                continue;
            }
            const UntileSource& src = m.source;
            const u32 w = src.width, h = src.height;
            if (etc1) {
                // U32U32 (0x1f), read raw: a block's two words as the guest stores them, a split
                // member's from its gather's colour rows.
                const u32 base =
                    src.code == UntileEtc1A4 ? scratch.gpu + m.gather_row * G * 4 : src.gpu;
                const Etc1Target t = Etc1TargetFor(w, h, m.levels);
                const auto size = list.size();
                add(*m.geometry, m.at * t.height, 0, t.width, t.height,
                    [&](const GeometryDraw& d, SideDraw& sd) {
                        SideTextureWords(0, 0x1f, 0, 2, d.view_rows, base + d.source, sd.tex);
                    });
                for (auto n = size; n < list.size(); n++) {
                    list[n].raw64 = true;
                }
            } else if (narrow != 0) {
                const ChainRender cr = ChainAt(w, h, m.levels, narrow);
                constexpr u32 VertexBytes = SGXD_PROGRAM_BLIT_VERTEX_WORDS * 4;
                const Geometry& g = GeometryFor(
                    GeometryKey(16, w, h, m.levels | src.bits << 4, false, false), VertexBytes,
                    [&](float* v, Geometry& geometry) {
                        return ChainVertices(cr, w, h, src.bits, v, geometry.draws);
                    });
                const auto size = list.size();
                add(g, 0, m.at * (batch_stride / narrow), cr.stride, cr.rows,
                    [&](const GeometryDraw& d, SideDraw& sd) {
                        SideTextureWords(0, src.code, src.swizzle, 8, d.view_rows,
                                         src.gpu + d.source, sd.tex, src.palette);
                    });
                for (auto n = size; n < list.size(); n++) {
                    list[n].converted = true;
                }
            } else {
                const ChainRender cr = ChainAt(w, h, m.levels, ChainBatchStride(w));
                const auto size = list.size();
                add(*m.geometry, 0, m.at, cr.stride, cr.rows,
                    [&](const GeometryDraw& d, SideDraw& sd) {
                        SideTextureWords(0, src.code, src.swizzle, 8, d.view_rows,
                                         src.gpu + d.source, sd.tex, src.palette);
                    });
                for (auto n = size; n < list.size(); n++) {
                    list[n].converted = true;
                }
            }
        }
        if (etc1) {
            const u32 side = 32u << (k - BatchEtc1);
            const u32 width = std::bit_ceil(b.used) * side;
            SideRender({}, list, b.block.gpu, std::max(width, 32u), width, side, false, true,
                       SGXD_RECIPE_PACK_F32F32_SWAPPED, true);
        } else if (narrow != 0) {
            SideRender({}, list, b.block.gpu, narrow, narrow, b.used * (batch_stride / narrow),
                       false, false, pack);
        } else {
            SideRender({}, list, b.block.gpu, batch_stride, batch_stride, b.used, false, false,
                       pack);
        }
    }

    // The alpha: the gathered nibbles as P4 through (17n) << 24 | 0xffffff, its A a byte.
    if (const UntileBatch& b = untile_batches[BatchAlpha]; b.block.cpu) {
        if (!merge_palette.cpu) {
            merge_palette = AllocTextureFixed(0x1000);
            u32* const entries = reinterpret_cast<u32*>(merge_palette.cpu);
            for (u32 n = 0; n < 16; n++) {
                entries[n] = (n * 17) << 24 | 0xffffffu;
            }
        }
        list.clear();
        for (const UntileMember& m : untile_members) {
            if (m.source.code != UntileEtc1A4) {
                continue;
            }
            const u32 w = m.source.width, h = m.source.height;
            const ChainRender cr = ChainAt(w, h, m.levels, AlphaBatchStride);
            const u32 gather = scratch.gpu + m.gather_row * G * 4;
            add(*m.alpha, 0, m.alpha_at, cr.stride, cr.rows,
                [&](const GeometryDraw& d, SideDraw& sd) {
                    SideTextureWords(3, 0x94, 0, d.view_width, d.view_rows, gather + d.source,
                                     sd.tex, merge_palette.gpu);
                });
        }
        SideRender({}, list, b.block.gpu, AlphaBatchStride, AlphaBatchStride, b.used, false,
                   false, SGXD_RECIPE_PACK_U8_A);
    }

    // What no member took goes back, and the gather's scratch. Only side renders write these next,
    // behind the renders above; the tiles a render stores past its last slot the next owner writes
    // over.
    for (u32 k = 0; k < UntileBatchKinds; k++) {
        UntileBatch& b = untile_batches[k];
        if (!b.block.cpu) {
            continue;
        }
        const u32 unit = UnitBytes(k);
        if (b.used < b.capacity) {
            GiveTexture({b.block.cpu + b.used * unit, b.block.gpu + b.used * unit},
                        (b.capacity - b.used) * unit);
        }
        b = UntileBatch{};
    }
    if (scratch.cpu) {
        GiveTexture(scratch, scratch_bytes);
    }
    untile_members.clear();
    gather_rows = 0;
    gather_draws = 0;
    flushing_untiles = false;
}

void SgxdBackend::DrawTiled(const UntileSource& src, bool transposed, u32 scale_x, u32 scale_y) {
    DEBUG_ASSERT(in_pass);
    const u32 w = src.width, h = src.height;
    DEBUG_ASSERT_MSG(w >= 8 && h >= 8 && w <= 4096 && w % 8 == 0 && h % 8 == 0 &&
                         src.code != UntileEtc1 && src.code != UntileEtc1A4 &&
                         (scale_x == 1 || scale_x == 2) && (scale_y == 1 || scale_y == 2),
                     "sgxd: a {}x{} tiled picture (format {:#x}, scale {}x{}) read into a pass is "
                     "not built",
                     w, h, src.code, scale_x, scale_y);
    DEBUG_ASSERT_MSG(!flip || !transposed, "sgxd: a flipped transposed tiled picture is not built");
    PlaceFixedPrograms();
    const Recipe& r = *current;
    // The tile rows the target shows: a transposed target's width is the picture's rows, a
    // linear one's height; scaled, scale_y picture rows to each.
    const u32 shown = (transposed ? r.width : r.height) * scale_y;
    const u32 tile_rows = std::min(h, AlignUp(shown, 8)) / 8;
    const u64 key = u64{0x80} << 56 | u64{transposed} << 55 | u64{scale_x - 1} << 54 |
                    u64{scale_y - 1} << 53 | u64{w} << 40 | u64{h} << 27 | u64{r.width} << 14 |
                    r.height;
    constexpr u32 N = SGXD_PROGRAM_BLIT_VERTEX_WORDS;
    const Geometry& g = GeometryFor(key, N * 4, [&](float* v, Geometry& geometry) {
        u32 vertex_count = 0;
        geometry.bands = TileBands(w, tile_rows, 1, vertex_count);
        for (const Band& band : geometry.bands) {
            const u32 band_texels = std::bit_ceil(band.rows * w);
            float* const band_v = v ? v + band.first_vertex * N : nullptr;
            for (u32 row = 0; row < band.rows && v; row++) {
                const u32 ty = band.first_row + row;
                const float v0 = static_cast<float>(row * w) / band_texels;
                const float v1 = static_cast<float>((row + 1) * w) / band_texels;
                if (transposed) {
                    // below.
                    const float uv[4][2] = {{0, v0}, {1, v0}, {0, v1}, {1, v1}};
                    PutQuad(band_v + row * QuadVertexWords, r.width, r.height,
                            static_cast<float>(8 * ty) / scale_y, 0,
                            static_cast<float>(8 * ty + 8) / scale_y,
                            static_cast<float>(w) / scale_x, uv);
                } else if (!flip) {
                    // Target x is the picture's column, scaled: a pixel's centre between texels.
                    const float uv[4][2] = {{0, v0}, {0, v1}, {1, v0}, {1, v1}};
                    PutQuad(band_v + row * QuadVertexWords, r.width, r.height, 0,
                            static_cast<float>(8 * ty) / scale_y, static_cast<float>(w) / scale_x,
                            static_cast<float>(8 * ty + 8) / scale_y, uv);
                } else {
                    const float height_px = static_cast<float>(r.height);
                    const float uv[4][2] = {{1, v0}, {1, v1}, {0, v0}, {0, v1}};
                    PutQuad(band_v + row * QuadVertexWords, r.width, r.height, 0,
                            height_px - static_cast<float>(8 * ty + 8) * fy,
                            static_cast<float>(w) * fx,
                            height_px - static_cast<float>(8 * ty) * fy, uv);
                }
            }
        }
        return vertex_count;
    });
    const Data& vertices = g.vertices;

    SgxdRecipeDrawPrograms p{};
    const SgxdVertexFetch fetch{vertices.gpu, N * 4, N, 0};
    const auto vspds = VertexPds(&fetch, 1, blit_vertex, false);
    DEBUG_ASSERT(vspds.va != 0);
    p.vspds = vspds.va;
    p.vspds_units = vspds.units;
    const auto vsec = SecondaryPds(0, 0, sgxd_program_doutu0(0, 0, 0));
    DEBUG_ASSERT(vsec.va != 0);
    p.vsec = vsec.va;
    p.vsec_units = vsec.units;
    p.vsec_sa = 0;
    const auto fsec = SecondaryPds(0, 0, sgxd_program_doutu0(1, 0, 0));
    DEBUG_ASSERT(fsec.va != 0);
    p.fsec = fsec.va;
    p.fsec_units = fsec.units;
    sgxd_program_vertex_dms(N, 0, 0, N, &p.vdm_w2, &p.vdm_w4, &p.vdm_w5);
    p.pixel_size = sgxd_program_pixel_dms(SGXD_PROGRAM_BLIT_PA, 0, 0);
    p.outselects = SGXD_PROGRAM_BLIT_OUTSELECTS;
    p.texsize = SGXD_PROGRAM_BLIT_TEXSIZE;
    const float viewport[6] = {r.width / 2.0f, r.width / 2.0f, r.height / 2.0f, r.height / 2.0f,
                               0.0f, 1.0f};
    std::memcpy(p.viewport, viewport, sizeof(viewport));
    p.indices = 0;
    p.primitive = SGXD_PROGRAM_TRIANGLES;
    // ISP A: opaque, DCMP ALWAYS, DWRITEDIS, BPRES; no culling. The region clip keeps the
    // target's tiles (quads wider than a transfer's output reach past them).
    const u32 isp = 7u << 22 | 1u << 20 | 1u << 9;
    const u32 clip = SGXD_RECIPE_CLIP(0, 0, (r.width + 31) / 32 - 1, (r.height + 31) / 32 - 1);
    for (const Band& band : g.bands) {
        const u32 band_texels = std::bit_ceil(band.rows * w);
        u32 tex[4];
        SideTextureWords(0, src.code, src.swizzle, 8, band_texels,
                         src.gpu + band.first_row * 8 * w * src.bits / 8, tex, src.palette);
        if (scale_x != 1 || scale_y != 1) {
            tex[0] |= 1u << 12 | 1u << 10; // MAGFILTER, MINFILTER bilinear
        }
        const PdsRef fprim = ScreenPixelPds(untile_fragment, tex);
        DEBUG_ASSERT(fprim.va != 0);
        p.fprim = fprim.va;
        p.fprim_units = fprim.units;
        Draw(SgxdRecipeDraw{band.first_vertex, band.rows * 6, isp, SGXD_R_FPRIM, 0, clip, nullptr},
             p);
    }
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
        merge_palette = AllocTextureFixed(0x1000);
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
