// Copyright Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include "common/common_types.h"

#ifdef __vita__
#include <psp2/kernel/processmgr.h>
#endif

namespace Common::PipelineStats {

/// repeatdiag (render thread only): fragment-block registers whose change bumped the mirror's
/// fragment version, by register index, and why software draws missed the repeat path.
inline std::array<u32, 0x300> frag_reg_changes{};
inline std::array<u32, 8> repeat_miss{};
/// Pre-shading: draws whose worker triangles were used, shaded by the render thread instead
/// (the worker had not started), waited for, refused (assembler not empty).
inline std::atomic<u32> preshade_used{0}, preshade_stolen{0}, preshade_waited{0},
    preshade_refused{0};
inline std::atomic<u64> preshade_feed_us{0}, preshade_draw_us{0}, preshade_local_us{0};
inline std::atomic<u32> preshade_tris{0};
/// Render thread: shading a draw the worker was already running (preshadesteal), waiting for
/// the worker instead, and shading draws the worker never took or whose result was refused.
inline std::atomic<u64> preshade_steal_running_us{0}, preshade_wait_us{0}, preshade_other_us{0};
/// BindPrograms split (render thread): FSConfig + hash + lookup, pipeline bind, calls.
inline u64 bp_config_us = 0, bp_pipeline_us = 0, bp_calls = 0, bp_tier2_us = 0;


/**
 * Where the emulation thread's wall time goes when it is not running the guest.
 *
 * The software renderer is a pipeline of three threads and the emulation thread waits on it in
 * two places: the drains behind RunSync, and - one level down, inside the ops those drains are
 * waiting for - the rasterizer's stripe barrier. Both were invisible in any profile, because a
 * thread asleep on a condition variable spends no cycles and shows up nowhere. These counters
 * are the only instrument that sees them, so they are built on every platform rather than only
 * on the console: the Linux tier exists to rehearse this design and cannot rehearse what it
 * cannot measure.
 *
 * Every counter accumulates until something drains it with exchange(0); a frontend reading them
 * once a second turns them into a per-second breakdown.
 */

/// Microseconds on a clock that does not jump. std::chrono::steady_clock is not usable for this
/// on the console - it read 1178 ms inside a 1000 ms window - so the kernel's process timer,
/// which is microseconds by definition, stands in there.
[[nodiscard]] inline u64 NowUs() {
#ifdef __vita__
    return sceKernelGetProcessTimeWide();
#else
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
#endif
}

/// A clock read that only feeds statistics: NowUs in the diagnostic build, 0 in release, so
/// the per-draw counters cost no system call there. The render thread made ~280k of these
/// per 3 s in the hub at ~0.7 us each.
/// Release: StatUs reads the clock only while this is set (ux0:data/mh4u/timedraw).
inline bool stat_timing = false;
[[nodiscard]] inline u64 StatUs() {
#ifdef VITA_DIAGNOSTICS
    return NowUs();
#else
    return stat_timing ? NowUs() : 0;
#endif
}
/// Render thread (with stat_timing): ApplyShipment on the mirror, and whole shipment ops.
inline std::atomic<u64> rt_apply_us{0}, rt_op_us{0};
/// Pre-shade worker (with stat_timing): GS draws shaded, memo hits, VS invocations; time in
/// the memo key, the shader setup, the VS, the GS, all of RunShippedDraw, and ApplyShipment.
/// Shader JIT (a32): the last program's code size and the code arena's use (bytes), and
/// shader runs that fell back to the interpreter (uncompiled program).
/// timedraw: VS float-uniform rows whose value changed, per row, on the render mirror.
inline std::array<std::atomic<u32>, 96> vs_frow_changes{};
inline std::atomic<u32> jit_last_bytes{0}, jit_arena_bytes{0}, jit_interp_runs{0};
inline std::atomic<u32> psw_draws{0}, psw_hits{0}, psw_vertices{0};
inline std::atomic<u64> psw_key_us{0}, psw_setup_us{0}, psw_vs_us{0}, psw_gs_us{0},
    psw_total_us{0}, psw_apply_us{0}, psw_load_us{0}, psw_loop_us{0};
inline std::atomic<u32> rt_ops{0};

/// Ops the render thread retired, and the wall time spent inside their bodies.
/// Draws whose vertices the render thread shaded in software on the mirror PICA, the time it
/// took, and how many of them were never offered to the hardware path (hw = false at ship).
/// RunShippedDraw's geometry-shader memo, both generations (KiB, last value).
inline std::atomic<u32> gs_memo_kib{0};
/// GS memo hits and misses (all PicaCores: the pre-shade worker and the render mirror).
inline std::atomic<u32> gs_memo_hit_n{0}, gs_memo_miss_n{0};
/// Time the shipping (GX) thread spent blocked, by cause: the pre-shade ring full, the render
/// queue's brake, the vertex ring out of space, the frame arenas over budget (us).
inline std::atomic<u64> gxw_shade_us{0}, gxw_brake_us{0}, gxw_ring_us{0}, gxw_arena_us{0};
inline std::atomic<u32> sw_shaded_draws{0};
inline std::atomic<u64> sw_shaded_us{0};
inline std::atomic<u32> sw_shaded_not_offered{0};
/// Why a backend declined a draw offered for hardware vertex shading, by reason index.
inline std::array<std::atomic<u32>, 12> hw_declines{};
/// Why the emulation thread did not offer a draw for hardware shading: 0 geometry shader,
/// 1 hardware shaders off, 2 primitive assembler not empty, 3 list not a multiple of three,
/// 4 the hardware shipment itself failed.
inline std::array<std::atomic<u32>, 5> hw_not_offered{};
inline std::atomic<u32> gpu_ops{0};
inline std::atomic<u64> gpu_busy_us{0};
/// The subset of that time spent in swap-marker ops (a whole frame's present).
inline std::atomic<u64> gpu_swap_us{0};
/// Surface readbacks the guest CPU forced: the count and the kilobytes handed back. A read of
/// a render target drains the GPU before it can answer, so a title that does this every frame
/// pays for it whether or not the pixels are new.
inline std::atomic<u32> cache_downloads{0};
inline std::atomic<u64> cache_download_kb{0};
/// Readbacks answered from the previous one instead of draining the GPU for fresh pixels.
inline std::atomic<u32> cache_stale_reads{0};
/// Guest display transfers and texture copies, split by whether the rasterizer took them or
/// the software blitter did. A software one reads the source through the CPU, which flushes
/// whatever surface owns it: that is where a readback storm comes from.
inline std::atomic<u32> xfer_accel{0};
inline std::atomic<u32> xfer_soft{0};
inline std::atomic<u32> copy_accel{0};
inline std::atomic<u32> copy_soft{0};

/// Draws that asked the cache for a framebuffer, and the subset of those that changed it. A
/// change of render target is a register write on the PICA and free there, but it is what the
/// Vita's renderer has to turn into a new GXM scene, and a scene costs a tile store and a
/// reload however few draws it holds. The guest decides this, so the count is the same number
/// on both tiers: what the pi5 reports here is what the console will be asked to do.
inline std::atomic<u32> fb_binds{0};
inline std::atomic<u32> fb_switches{0};

/// Times the queue hit its depth ceiling and made the emulation thread wait for it to drain,
/// and the wall time those waits cost it. This is the emulator throttling itself to the render
/// thread's rate: while it is braking the guest does not run, so the core looks idle and the
/// speed falls by exactly this much.
inline std::atomic<u32> gpu_brakes{0};

/**
 * Where the render thread is right now, for the case where it stops being anywhere.
 *
 * A hang on the console produces no output at all from the thread that hung, but the
 * emulation thread keeps printing its per-second line for several seconds afterwards -
 * until the queue reaches GpuThread::MaxDepth and the brake parks it too. That window is
 * the only chance to learn where the render thread went, so it leaves a breadcrumb: a
 * pointer to a string literal and the time it was stored. The frontend prints both, and a
 * phase that stops advancing while its age climbs names the call that never returned.
 *
 * The store is one relaxed word, and the call sites are compiled under VITA_RENDER_TRACE,
 * which the Vita build has on by default.
 */
inline std::atomic<const char*> render_phase{"none"};
inline std::atomic<u64> render_phase_us{0};
/// Three numbers the phase may carry, printed beside it. For a wait: what it is waiting for,
/// what has been submitted, and what the GPU has finished - which is the whole question when a
/// wait never returns.
inline std::atomic<u64> render_phase_a{0};
inline std::atomic<u64> render_phase_b{0};
inline std::atomic<u64> render_phase_c{0};

inline void SetRenderPhase(const char* name) {
    render_phase.store(name, std::memory_order_relaxed);
    render_phase_us.store(NowUs(), std::memory_order_relaxed);
}

inline void SetRenderPhase(const char* name, u64 a, u64 b = 0, u64 c = 0) {
    render_phase_a.store(a, std::memory_order_relaxed);
    render_phase_b.store(b, std::memory_order_relaxed);
    render_phase_c.store(c, std::memory_order_relaxed);
    SetRenderPhase(name);
}

/// The name alone, for a site hot enough that the clock read would count (a draw): the age
/// printed beside it is then that of the last timed phase.
inline void SetRenderPhaseName(const char* name) {
    render_phase.store(name, std::memory_order_relaxed);
}

/// What the frontend adds to the stuck-queue log (GpuThread::Enqueue): a line of its own
/// with what only it can read, such as the kernel module's cross-core counters. Called from
/// the emulation thread, once a second while the render thread does not drain.
inline std::atomic<void (*)()> stuck_report{nullptr};
/// The renderer's own addition to the same log: the last scenes it submitted, against the
/// GPU's completed epoch. Read from the emulation thread while the render thread is parked.
inline std::atomic<void (*)()> scene_report{nullptr};

#ifdef VITA_RENDER_TRACE
#define GXM_PHASE(...) ::Common::PipelineStats::SetRenderPhase(__VA_ARGS__)
#define GXM_PHASE_NAME(name) ::Common::PipelineStats::SetRenderPhaseName(name)
#else
#define GXM_PHASE(...) ((void)0)
#define GXM_PHASE_NAME(name) ((void)0)
#endif

/// Frames actually put on the screen. The frontend's "game fps" counts frames the guest
/// finished, which is what the emulated console produced, not what the display showed - the
/// two diverge whenever presents are being dropped, and only this one tracks the latter.
inline std::atomic<u32> presents{0};
/// Vblanks whose present was not queued because no new top-screen frame had arrived.
inline std::atomic<u32> dup_presents{0};
/// Frameskip signals, in vblanks per second (release counters): presents pending, queue
/// backlog estimate, arena backpressure, and vblanks that armed a skip.
inline std::atomic<u32> skipsig_presents{0};
inline std::atomic<u32> skipsig_backlog{0};
inline std::atomic<u32> skipsig_arena{0};
inline std::atomic<u32> skipsig_armed{0};
/// Running totals (never reset) for the stall recorder (GxmPresent::EndFrame): GX
/// command-list time, guest file-read time and count, guest top-screen frames finished.
inline std::atomic<u64> gx_list_total_us{0};
inline std::atomic<u64> fs_read_total_us{0};
inline std::atomic<u64> fs_reads_total{0};
/// Guest file reads done on a worker thread (File::Read's async path): per second, total.
inline std::atomic<u32> fs_async_us{0};
inline std::atomic<u64> fs_async_total_us{0};
inline std::atomic<u64> guest_frames_total{0};
/// GX thread work, per second (release line `gxc`): command-list KiB, draws, draws shipped
/// to the render thread, vertex KiB copied, shader code KiB collected.
inline std::atomic<u32> gxc_cmd_bytes{0};
inline std::atomic<u32> gxc_draws{0};
inline std::atomic<u32> gxc_ships{0};
inline std::atomic<u32> gxc_vtx_bytes{0};
inline std::atomic<u32> gxc_code_bytes{0};
/// Guest file reads (FS File::Read): time spent, count and KiB, per second.
inline std::atomic<u32> fs_read_us{0};
inline std::atomic<u32> fs_reads{0};
inline std::atomic<u32> fs_read_kib{0};
/// Presents the render thread dropped because a newer one was queued behind them.
inline std::atomic<u32> presents_superseded{0};
/// Draws dropped because they were for the hidden bottom screen (single-screen layout).
inline std::atomic<u32> bottom_draws_dropped{0};
/// Bottom-screen display transfers dropped in the single-screen layout (GPU::QueueMemoryTransfer).
inline std::atomic<u32> bottom_xfers_dropped{0};
/// RightEyeDisabler refusals: second-eye command lists and display transfers (per second).
inline std::atomic<u32> re_lists_blocked{0};
inline std::atomic<u32> re_xfers_blocked{0};
/// Top-screen-sized display transfers (frame boundaries), per second: ~30 at 30 fps unless the
/// game also copies a second eye.
inline std::atomic<u32> top_xfers{0};
/// Post-process draws and blits dropped by the bloom / depth-of-field chain rule (`postfx`).
inline std::atomic<u32> fx_dropped{0};
/// The 2D layer (hudres Stage 0): draws after the grade into its target, those of them using
/// depth or stencil, scenes begun there, grades seen, and EndFrame arriving inside the layer.
inline std::atomic<u32> hud_post_draws{0};
inline std::atomic<u32> hud_post_depth{0};
inline std::atomic<u32> hud_post_scenes{0};
inline std::atomic<u32> hud_grades{0};
inline std::atomic<u32> hud_cut{0};
/// HUD resolution: draws redirected to the screen-sized target, frames it started, and
/// resolves (the guest picture needed it: a read, a depth draw, an unmatched transfer).
inline std::atomic<u32> hudres_draws{0};
inline std::atomic<u32> hudres_frames{0};
inline std::atomic<u32> hudres_resolves{0};
/// Guest pixels the 2D layer's draws cover (their draw rectangles), in thousands per second:
/// the fill the HUD resolution multiplies.
inline std::atomic<u32> hud_post_area{0};
/// The part of it drawn by triangles whose three vertices have alpha 0 (invisible under
/// SrcAlpha blending), thousands of guest pixels per second.
inline std::atomic<u32> hud_post_area_zero{0};
/// Time the frame limiter slept on the emulation thread (the guest ahead of real time).
inline std::atomic<u32> limiter_sleep_us{0};
/// Time the GX thread spent parsing command lists, and how many.
inline std::atomic<u32> gx_list_us{0};
inline std::atomic<u32> gx_lists{0};
/// VBlank snapshots taken of CPU-written framebuffers (GPU::DisplaySnapshot).
inline std::atomic<u32> display_snapshots{0};

/// Wall time spent inside the frontend's present call (GL swap, or the software path's
/// texture upload + copy + present). When the display connection itself is the bottleneck -
/// vsync against a slow display, or an X server on the far side of an ssh tunnel - this is
/// where the render thread blocks, and this number approaches a full second per second.
inline std::atomic<u64> present_us{0};

/// The emulation thread waiting for the render thread's queue to empty (every RunSync).
inline std::atomic<u64> drain_us{0};
inline std::atomic<u32> drain_count{0};

/// How long a completion interrupt spends queued: from the emulation thread enqueueing it
/// behind its fill or transfer, to the render thread actually posting it. This is what the
/// guest waits out when the write it is waiting for rides the render-thread queue.
inline std::atomic<u64> interrupt_lag_us{0};
inline std::atomic<u32> interrupt_count{0};

/// Pixels the software rasterizer actually walked, after the triangle edges have clipped each
/// scanline - not the bounding span, which counts however diagonal the geometry happens to be. Divided into
/// the render thread's busy time this gives nanoseconds per fragment, which is what the
/// rasterizer's speed actually is and does not move with whatever the game happens to be
/// drawing. Wall-clock frame rate on a demo that wanders between light and heavy scenes varies
/// by a third run to run, which is wider than most changes worth making.
inline std::atomic<u64> fragments{0};

/// The same two totals, never drained, so a whole run can be reduced to one figure. A
/// per-second sample covers whatever the game was drawing that second; summing the run removes
/// that, and a fixed run from boot then repeats to well under a percent.
inline std::atomic<u64> fragments_total{0};
inline std::atomic<u64> gpu_busy_total_us{0};

/// The render thread waiting for its stripe worker at the end of a batch (HelpAndWait).
inline std::atomic<u64> barrier_us{0};
inline std::atomic<u32> barrier_count{0};

} // namespace Common::PipelineStats
