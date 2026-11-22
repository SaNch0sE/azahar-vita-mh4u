// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <vector>

namespace GxmRenderer {

/**
 * Bring-up switches read once from ux0:data/azahar/sgxd_flags.txt (and host0:sgxd_flags.txt):
 * whitespace-separated words, one per switch.
 * Absent file: nothing set. Compiled in only when VITA_DIAGNOSTICS is defined; the release build
 * answers false to every one of them and reads no file.
 *
 * Measuring:
 *   perflog   log a second's `perf:` lines: speed and frame rates, the native core's slices
 *             (guest time, how they ended, the svcs answered in the guest and the ones that
 *             left it), the presents, the command lists, fills and transfers, the cache
 *             operations by site, the FCRAM pages moved to CDRAM, the textures' untiles, sgxd's
 *             microkernel (shapes, flips, jobs, interrupts); each PDS arena flip
 *   holdlog   with perflog, the first 200 page-hold aborts that waited (holdlog=N: from N
 *             seconds of the process's time on), each with the guest's PC and instruction, the
 *             held range and the page's live read and write bytes, parse holds and index watch
 *   screenlog  the late skip's screen map, four frames in every 600: the colour buffers drawn
 *             and dropped and the screen each maps to, the transfers and fills, the LCD buffers
 *             (RenderThread::EndFrame), and each present's buffer for each screen;
 *             screenlog_at=S every frame from S seconds of the process's time on, 600 frames
 *   serial    no overlap between the guest's frames and the GPU's: the vblank waits for the
 *             GPU to finish the frame (no late skip)
 *   latelog   the late skip's late frames: each kept draw reading a colour buffer whose draws
 *             the same frame dropped, the transfers (dropped or not) and the fills (400 lines
 *             each); latelog_at=S from S seconds of the process's time on
 *
 *   threadtimes  with perflog, each placed thread's run time in the second and the cores its
 *             role allows (Common::RoleThreads; core 3's threads, which psp2prof cannot see).
 *             Not for speed: sceKernelGetThreadInfo of another thread debug-suspends the
 *             caller until a debugger resumes it (200-270 ms a second of the emulation thread).
 *
 * The shader emitter:
 *   notier2   vertex and fragment programs stay the first tier's: no worker re-emits them with
 *             the optimising passes after RasterizerSgxd::Tier2Draws draws
 *   dumpusse  write each emitted program to host0 as azahar-vs-KEY.json / azahar-fs-KEY.json
 *             (vs2 / fs2 for the second tier's): its words and interface (Usse::ProgramJson),
 *             which tools/usse2gxp.py puts in a GXP for psp2shaderperf; with the
 *             PICA side of each (.fsconfig, .pica, .glsl) for compiling it with psp2cgc
 *
 * Debugging a pass on the console:
 *   dumptex   write the first eight untiled textures to host0, each level: the guest's bytes
 *             and the host copy, azahar-texN-ADDR-WxH-fFORMAT-sSTRIDE-{guest,host}.bin
 *   texdump=N  every texture the draws sample over a frame of both screens from N s of the
 *             process's time on, once each, as the host copy its unit reads, at its GPU size:
 *             azahar-texf-ADDR-WxH-sSTRIDE-fFORMAT-lLEVELS-uUNIT-KIND.bin
 *   picdump=N  after each of the first two display transfers from N s on, the GPU idle: every
 *             scaled picture with its buffer's guest bytes, every screen picture with its
 *             output's guest bytes (azahar-picN-*.bin; tools/picdump_view.py makes PNGs)
 *   drawkeys  log the first thousand draws' fragment programs by dumpusse's key (words, temps,
 *             PAs, issues): a replay's per-draw costs tied to the dumped programs;
 *             drawkeys_at=S the first thousand from S seconds of the process's time on
 *
 *   noload    a pass that would load its colour buffer's picture (BeginPassLoaded) starts from
 *             a clear instead; nozload: a pass's depth is not loaded (still stored): what the
 *             loads cost, in syncpasses' or a replay's pass times (the pictures are wrong)
 *   syncpasses every pass runs whole, TA then 3D, before EndPass returns: exact pass times, in
 *             perflog's per-target lines (async passes take no times)
 *   syncside  with syncpasses or in a replay, the side renders (texture untiles and their
 *             gathers) run whole too, their times summed: perflog's "side renders" line, and
 *             in a replay one more pass after the frame's (a synchronous pass's times start
 *             after what was queued, so a queued side render's time is in no pass's)
 *   passlog=N  every pass ended in the second from N s of the process's time on: its target,
 *             scale, draws, how it opened (clear, covered, load), times (with syncpasses) and
 *             what ended it
 *   gpucycles a probe: each core's 3D, TA and memory stall cycles (the GPU's own counters)
 *             around the 60th to the 180th synchronous render (syncpasses, a replay)
 *   gpuperf=G a probe: the SGX's performance counters (Razor's metric group G: 1 the USSE's
 *             processing and texture read requests, 2 pixels before hidden surface removal
 *             and pixels sent on to be shaded, 3 the ISP's parameter fetch) around the same
 *             renders, each core's eight counters; gpuperfsum=MASK: those counters'
 *             SUM_MUX bits set
 *   u8colour  a probe: the U8 combiners read the vertex colour's bytes from a second,
 *             fixed-point iterator of it (what that iterator delivers; a pack fewer)
 *   smpskip   a probe: skip-invalid on the texture reads that are point sampled from one level
 *             (the lighting's tables, a shadow's taps), which run in uncovered instances too
 *   keepalpha_opaque  a probe: a draw with no blend that keeps the destination's alpha
 *             (colour mask RGB) is opaque, not translucent (pictures may be wrong: a hidden
 *             opaque draw's alpha is lost)
 *   fsonly=N  fswhite and fscut on the program whose key's low 32 bits are N alone
 *             (drawkeys logs each draw's fs key; the word repeated names more)
 *   fscut=N   a single-phase fragment program cut to its first N instructions, then the white
 *             write: bisecting which instruction a draw's time goes to
 *   fswhite=1 every fragment white, the program still run (kills included); fswhite=2 the
 *             program replaced by the white write alone: the vertex side and rasterisation;
 *             fswhite=4 as many NOPs as the program's words, then the white write (its time
 *             without its loads and reads)
 *   wbufz     W-buffering drawn as Z-buffering: the program writes no depth, so its draws are
 *             not DEPTHFEEDBACK (the depths stored differ): what the feedback costs
 *   clipcheck=N  from N s of the process's time on, the draws in the first 50 ms of each second
 *             (clipcheckall: every draw) run through the vertex program on the CPU and clipped
 *             at the TA's near and far planes: their triangles the TA clips at its guard band
 *             with vertices of different w (the clipper lockup's trigger), those crossing w 0,
 *             those left with w near 0 by the near and far planes (the TA has no w plane), and
 *             non-finite positions; a line for each draw with the last two (600 at most) and
 *             each second's totals
 *   keepaa    the game's anti-aliasing kept, for comparing pictures and times: a colour
 *             buffer a display transfer averages down drawn at the game's size
 *             (RasterizerGxm::Supersampled), a jittered second render of the scene drawn
 *             (RasterizerGxm::DoubleRender)
 *
 * Checking guest memory against the CPU:
 *   untilecheck  every RGBA8, RGB8, ETC1 and ETC1A4 untile's levels against upstream's CPU
 *             decode (ETC1's, and a split ETC1A4's colour and alpha, decoded back through the
 *             texture unit), once its batch is sent; the log counts the levels that differ
 *             (with dumptex, a "want" file too)
 *   keycheck  the fragment and vertex format registers' kept hashes, the program the hash
 *             found and the cached LUT rows against the registers at every draw: a register
 *             missing from StateRegs (or a collision) stops the run
 *   remapcheck  each page's newest remap whose list is done read back from the GPU's page
 *             table (sgxdTranslate) before the next list goes; a wrong entry is logged
 *
 * sgxd's frame capture: vprof, vprof_at, vprof_len, vprof_trigger, vprof_vblanks
 *   (docs/debugging.md, "Wrong pictures")
 *
 * Timelines:
 *   tracelog  SgxdTrace's events from 8 s to 9 s after the first one (tracelog_at=S and
 *             tracelog_len=S move and widen the window; 65536 events at most), written to host0 as
 *             azahar-trace.txt when the window closes: a line per event, microseconds from the
 *             first event, the tag, its argument and the kernel's system time (low 32 bits,
 *             microseconds). Synchronous renders (a store into cached memory) are
 *             sync/sync_end, the present sgx_present/sgx_present_end; the async lists, which
 *             sgxd's microkernel runs, carry no GPU stamps. Frame pacing: late (the next frame's
 *             decision: 0 whole, 1 whole with the GPU late, 2 the top's draws dropped, 3 the
 *             bottom's), lead_wait/lead_wait_end,
 *             present (1: the top screen's buffer differs from the last shown), presented (it
 *             is drawn), present_behind, present_pending, flip (1: its 3D was done before the flip),
 *             flip_vcount (the display's vblank count at the set), limit (the frame limiter's
 *             sleep, microseconds), display_wait (with vsync, the present waiting for a
 *             display buffer: one shown, two queued);
 *             emu_ticket, emu_space, emu_sync (the emulation thread's waits for the render
 *             thread, each with an _end), idle (the guest has no thread to run: the ticks to
 *             its next event). The GPU's side: frame_ticket (a vblank's ticket, the late
 *             skip's), reached (a ticket's work done, as the render thread polls it), kick (a
 *             render sent to the GPU: its width), finish (a pending render's 3D sent on its
 *             own), gpu (perflog's sampled stream states when they change: 3D in bits 15:8, TA
 *             in 7:0, busy bit 16), list_run/list_run_end (the render thread on a list's parsed
 *             records), parse/parse_end (the parse thread on a list)
 */
[[nodiscard]] bool GxmFlag(const char* name);
/// A flag written `name=N`: N, or 0 when the flag is not given (always 0 in the release build).
[[nodiscard]] unsigned SgxdFlagValue(const char* name);
/// Every `name=N` given, in the file's order (none in the release build).
[[nodiscard]] std::vector<unsigned> SgxdFlagValues(const char* name);

/// With sgxd_flags `tracelog`, one timeline event: `tag` a string literal, `arg` its detail (an
/// op kind, an interrupt id). Any thread; nothing in the release build.
#ifdef VITA_DIAGNOSTICS
void SgxdTrace(const char* tag, unsigned arg = 0);
#else
inline void SgxdTrace(const char*, unsigned = 0) {}
#endif

/// A span's two tracelog events: `begin` now (with `arg`), `end` when it goes.
struct TraceSpan {
    TraceSpan(const char* begin, const char* end_, unsigned arg = 0) : end{end_} {
        SgxdTrace(begin, arg);
    }
    ~TraceSpan() {
        SgxdTrace(end);
    }
    TraceSpan(const TraceSpan&) = delete;
    TraceSpan& operator=(const TraceSpan&) = delete;
    const char* end;
};

} // namespace GxmRenderer
