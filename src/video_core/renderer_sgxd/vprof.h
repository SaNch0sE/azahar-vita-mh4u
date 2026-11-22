// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <vector>
#include "common/common_types.h"

namespace Pica {
class PicaCore;
}
namespace Memory {
class MemorySystem;
}

/**
 * vprof: sgxd's frame capture (diagnostics build, sgxd_flags `vprof`). One capture is a file on
 * the host PC, host0:vprof-N.vprof, that tools/frame_replay reads. A capture holds:
 *
 * - the SgxdTrace events of every thread (the GPU's ops, GPU stamps, presents,
 *   vblanks, the emulation thread's idles) and each sgxd pass's kick;
 * - one frame, vblank to vblank: the PICA's state at its start, every GPU register the guest
 *   wrote (GSP's commands as their register writes), the DMAs, and for each GPU op the guest
 *   memory it read, plus each draw's registers. Replaying the register writes over that memory
 *   is replaying the frame, on the console or on the host through upstream's software renderer.
 *
 * The capture starts `vprof_at` seconds (default 10) after the renderer, or when host0 holds
 * vprof-trigger (polled every second with `vprof_trigger`), and lasts `vprof_len` seconds
 * (default 2); the frame is the first whole one inside it. The layout is
 * tools/frame_replay/FORMAT.md.
 */
// Without VITA_DIAGNOSTICS every function below is an inline no-op, and the release build
// compiles none of vprof's sources (video_core/CMakeLists.txt).
namespace SgxdRenderer::Vprof {

#ifdef VITA_DIAGNOSTICS

/// The capture thread, from the renderer's start (idempotent; nothing without the flag).
void Start(Memory::MemorySystem& memory);
void Event(const char* tag, u32 arg);

/// Whether the frame is being recorded: the hooks below are cheap no-ops otherwise.
bool Recording();
/// Whether a capture runs: from its start to its file, the frame's drain included.
bool Capturing();
/// A line for the capture's INFO record (the ROM, the guest's code segments), any thread.
void Info(const char* key, const char* value);
/// Emulation thread: a vblank (a frame's end and the next one's start). True when the frame
/// starts here: the caller runs State as the frame's first GPU op.
bool VBlank();
void State(const Pica::PicaCore& pica);
/// Emulation thread: a GPU register write by the guest (index into PicaCore::Regs; `lcd` for
/// the LCD's registers), after it is stored.
void RegWrite(u32 index, u32 value, bool lcd);
/// Emulation thread: a GSP command (its eight words, the addresses physical).
void Gsp(const u32* words);
/// Emulation thread: a GSP DMA, as the PTLA runs it: `runs` physical (source, destination,
/// size) triples.
void Dma(const u32* triples, u32 runs);
/// Emulation thread: a GPU op (VideoCore::GPU::RunGpuOp) is about to run; returns its number.
u32 OpQueued(u32 kind);
/// Whether the running op is one of the recorded frame's.
bool InOp();
void OpBegin(u32 seq);
void OpEnd(u32 seq);
/// Guest memory the running op read.
enum class Mem : u32 { CmdList, Vertices, Indices, Texture, Colour, Depth, DmaSource };
void Memory(Mem kind, u32 paddr, u32 size);
/// Render thread: a draw, with the PICA's state at it (registers and uniforms, as deltas);
/// the rasterizer's pass and draw numbers and the vertex count.
void Draw(const Pica::PicaCore& pica, u32 pass, u32 draw, u32 vertices);
/// Render thread: an sgxd pass was kicked (every pass while a capture runs).
void Pass(u32 pass, u32 target, u32 width, u32 height, u32 draws, bool sync);
/// Render thread: a recorded frame's pass ran synchronously in ta_us and d3_us (FrameSync).
void PassTimes(u32 pass, u32 ta_us, u32 d3_us);
/// A capture of the events now, whatever the flags say (the replay's `profile`), until
/// ProfileStop, which returns once its file is written. False when it did not start within 3 s.
bool ProfileStart(Memory::MemorySystem& memory);
void ProfileStop();
/// Whether the recorded frame is running: its passes run synchronously (sgxd), so that each
/// pass's result can be read as it ends.
bool FrameSync();
/// A record of the recorded frame, as FORMAT.md lays it out (the rasterizer's RSTA), and a
/// blob of bytes for it (its id).
void FrameRecord(u32 tag, const std::vector<u32>& words);
u32 FrameBlob(const u8* data, u32 size);
/// Whether a pass's or a transfer's result is wanted: the recorded frame's (PRES), or a
/// replay's `results` request (RPRS).
bool WantResults();
/// A result: kind 0 a pass's surface (A8B8G8R8, width x height, `format` 0), kind 1 the guest
/// memory a display transfer wrote (`format` its PICA pixel format).
void FrameResult(u32 kind, u32 addr, u32 width, u32 height, u32 format, const u8* data,
                 u32 size);

#else

inline void Start(Memory::MemorySystem&) {}
inline void Event(const char*, u32) {}
inline bool Recording() {
    return false;
}
inline bool Capturing() {
    return false;
}
inline void Info(const char*, const char*) {}
inline bool VBlank() {
    return false;
}
inline void State(const Pica::PicaCore&) {}
inline void RegWrite(u32, u32, bool) {}
inline void Gsp(const u32*) {}
inline void Dma(const u32*, u32) {}
inline u32 OpQueued(u32) {
    return 0;
}
inline bool InOp() {
    return false;
}
inline void OpBegin(u32) {}
inline void OpEnd(u32) {}
enum class Mem : u32 { CmdList, Vertices, Indices, Texture, Colour, Depth, DmaSource };
inline void Memory(Mem, u32, u32) {}
inline void Draw(const Pica::PicaCore&, u32, u32, u32) {}
inline void Pass(u32, u32, u32, u32, u32, bool) {}
inline void PassTimes(u32, u32, u32) {}
inline bool FrameSync() {
    return false;
}
inline bool WantResults() {
    return false;
}

#endif

} // namespace SgxdRenderer::Vprof
