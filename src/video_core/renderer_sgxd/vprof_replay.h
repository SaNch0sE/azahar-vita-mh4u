// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#ifdef VITA_DIAGNOSTICS

#include <functional>
#include <string>
#include <vector>
#include "common/common_types.h"

namespace Pica {
class PicaCore;
}
namespace Memory {
class MemorySystem;
}

namespace SgxdRenderer {
class RasterizerSgxd;
}

/**
 * vprof's replay on the console, in the replay player: a recorded frame (host0:vprof-replay.vprof,
 * tools/frame_replay/FORMAT.md) run again through azahar's own GPU path. Requests come from the
 * host PC as host0:vprof-replay.txt (one per line, the file removed once read):
 *
 *   run N              the whole frame N times; the passes synchronous, their TA and 3D times
 *   costs              every draw's 3D cost: the frame up to the draw's op with the draws after
 *                      it left out, three times each, the pass's 3D time against the draw before
 *   image D            the target of draw D (the frame's, from 0) right after it
 *   images             the targets after every draw
 *   results            one run, with every pass's surface and every display transfer's output
 *                      as it ended (RPRS), to hold against the console's own (the capture's PRES)
 *   profile N [present]  the frame N times as the game runs it, the passes pipelined, each run
 *                      to the GPU's idle (RFRM), under vprof's sampler (its own capture file);
 *                      with `present` the screens shown after each run
 *   quit               guest memory as it was, and the guest goes on
 *
 * Every run starts from the capture's rasterizer state (RSTA): the pending clears, the colour
 * surfaces and the depth stores as they were when the frame began on the console, in place of
 * this renderer's own.
 *
 * Each request's results go to host0:vprof-replay-out-N.bin (records as FORMAT.md's "Replay
 * results"), then host0:vprof-replay-done-N marks it complete.
 */
namespace SgxdRenderer::Vprof {

struct ReplayHost {
    Pica::PicaCore& pica;
    Memory::MemorySystem& memory;
    RasterizerSgxd& rasterizer;
    /// A GSP command's eight words, its addresses physical.
    std::function<void(const u32* words)> gsp;
    /// A GPU register write (PicaCore::Regs index or RegsLcd's).
    std::function<void(u32 index, u32 value, bool lcd)> reg;
    /// A DMA's physical runs (source, destination, size triples).
    std::function<void(const std::vector<u32>& triples)> dma;
    /// Runs fn as a GPU op (numbered like the frame's).
    std::function<void(std::function<void()> fn)> op;
    /// Runs fn once everything before it has run.
    std::function<void(std::function<void()> fn)> sync;
    /// Shows the screens as the frame left them, as a vblank does (`profile N present`).
    std::function<void()> present;
    /// The frame before ends, as the top screen's swap ends one: the GPU's per-frame state
    /// (RightEyeDisabler's) starts over, so each run blocks the right eye as the game's frame did.
    std::function<void()> frame_start;
    /// `pica` set whole (the frame's restored state): the render thread's PicaCore takes it
    /// (RenderThread::TakeWholeState).
    std::function<void()> state_restored;
};

/// Serves requests until `quit`, on the emulation thread: the capture at `capture`, the first
/// batch of requests `first` when it is not empty, then host0:vprof-replay.txt's.
void Serve(ReplayHost& host, const std::string& capture, const std::string& first);

/// vprof.cpp's op hooks, while a replay runs: the ops' numbers and each op's memory.
bool ReplayActive();
u32 ReplayOpQueued();
void ReplayOpBegin(u32 seq);
void ReplayOpEnd(u32 seq);
/// Whether the running replay collects results (the `results` request), and one result.
bool ReplayWantsResults();
void ReplayResult(u32 kind, u32 addr, u32 width, u32 height, u32 format, const u8* data,
                  u32 size);

} // namespace SgxdRenderer::Vprof

#endif
