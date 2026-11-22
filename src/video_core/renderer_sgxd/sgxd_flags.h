// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

namespace GxmRenderer {

/**
 * Bring-up switches read once from ux0:data/azahar/sgxd_flags.txt (and host0:sgxd_flags.txt):
 * whitespace-separated words, one per switch.
 * Absent file: nothing set. Compiled in only when VITA_DIAGNOSTICS is defined; the release build
 * answers false to every one of them and reads no file.
 *
 * Debugging a pass on the console:
 *
 *   noload    a pass that would load its colour buffer's picture (BeginPassLoaded) starts from
 *             a clear instead; nozload: a pass's depth is not loaded (still stored): what the
 *             loads cost, in syncpasses' or a replay's pass times (the pictures are wrong)
 *   noviewkill  a viewport off the end of tile's store grid (StorePartFor) keeps no kill: its
 *             margin is drawn over and stored (wrong), for what the punch-through kill costs
 *   syncpasses every pass runs whole, TA then 3D, before EndPass returns: exact pass times
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
 *   vsprobe=N draws N to N + 3 also run their vertex stage as a stream-out and log each
 *             loader and vertex; attrprobe the same for the first four draws with BYTE or
 *             SHORT attributes (UnpackInt's lanes)
 *
 * Checking guest memory against the CPU:
 *   untilecheck  every RGBA8, RGB8, ETC1 and ETC1A4 untile's level 0 against upstream's CPU
 *             decode; the log counts the levels that differ (with dumptex, a "want" file too)
 *   poolcheck the textures pool's GPU mapping, checked at the first untile that differs
 *   tilecheck a write-back against the CPU's own encoding of the surface
 *
 * The profiler: vprof, vprof_at, vprof_len, vprof_us, vprof_cpus, vprof_trigger, vprof_replay,
 *   vprof_replay_at
 *
 * Timelines:
 *   tracelog  SgxdTrace's events from 8 s to 9 s after the first one (tracelog_at=S and
 *             tracelog_len=S move and widen the window; 65536 events at most), written to host0 as
 *             azahar-trace.txt when the window closes: a line per event, microseconds from the
 *             first event, the tag, its argument and the kernel's system time (low 32 bits,
 *             microseconds). sgxd's async lists add their GPU stamps as events gpu_ta, gpu_ta_end,
 *             gpu_3d and gpu_3d_end whose argument is the stamp in that clock; synchronous
 *             renders (a store into cached memory) are sync/sync_end, the present
 *             sgx_present/sgx_present_end
 */
[[nodiscard]] bool GxmFlag(const char* name);

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
