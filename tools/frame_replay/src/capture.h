// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// frame_replay: a capture file (FORMAT.md) in memory.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace frame_replay {

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

struct Event {
    u32 time;
    u32 tid;
    u32 tag;
    u32 arg;
};

enum class MemKind : u32 { CmdList, Vertices, Indices, Texture, Colour, Depth, DmaSource };
const char* MemKindName(u32 kind);

struct MemLoad {
    u32 seq;
    u32 kind;
    u32 paddr;
    u32 size;
    u32 blob;
};

constexpr u32 NumRegs = 0x732;
constexpr u32 InternalBase = 0x400; // PicaCore::Regs: the internal (PICA) registers from word 0x400

struct Uniforms {
    std::array<std::array<float, 4>, 96> f{};
    u32 b{};
    std::array<u32, 4> i{};
};

struct Draw {
    u32 seq, pass, draw, vertices, time;
    std::vector<u32> regs;      // PicaCore::Regs as the draw saw them
    std::vector<u32> changed;   // the indices that changed since the draw before
    Uniforms vs, gs;
    int vs_program = -1;        // index into Frame::programs
    int gs_program = -1;
};

struct Program {
    u32 which; // 0 VS, 1 GS
    std::vector<u32> code;
    std::vector<u32> swizzle;
};

struct Pass {
    u32 seq, pass, target, width, height, draws, sync, time;
    /// PTIM: a synchronous pass's TA and 3D microseconds; timed is false without one.
    u32 ta_us = 0, d3_us = 0;
    bool timed = false;
};

/// PRES: a pass's surface or a display transfer's output as the console left it (kind 0: the
/// surface, A8B8G8R8; kind 1: the transfer's linear output in its PICA format).
struct Result {
    u32 seq, index, kind, addr, width, height, format, blob;
};

struct Op {
    u32 seq;
    u32 kind;
    u32 queued = 0, begin = 0, end = 0;
    std::vector<size_t> loads;  // into Frame::loads
    std::vector<size_t> draws;  // into Frame::draws
    std::vector<size_t> passes; // into Frame::passes
};

struct GspCommand {
    std::array<u32, 8> words;
    u32 time;
};

struct RegWrite {
    u32 index, value, lcd, time;
};

struct Dma {
    u32 time;
    std::vector<std::array<u32, 3>> runs;
};

/// The emulation thread's stream, in order: what the replay walks.
struct StreamItem {
    enum Kind { Gsp, Reg, DmaOp, OpQueued } kind;
    size_t index;
};

struct PicaState {
    std::vector<u32> regs;
    std::vector<u32> lcd;
    Program vs, gs;
    Uniforms vs_u, gs_u;
    u32 vs_entry = 0, gs_entry = 0;
    std::array<std::array<float, 4>, 16> default_attributes{};
    std::vector<u32> proctex; // noise, colour map, alpha map, colour, colour difference
    std::vector<u32> lighting; // 24 x 256
    std::vector<u32> fog;      // 128
};

struct Frame {
    bool present = false;
    u32 number = 0, start = 0, end = 0;
    bool has_state = false;
    PicaState state;
    std::vector<GspCommand> gsp;
    std::vector<RegWrite> regs;
    std::vector<Dma> dmas;
    std::vector<StreamItem> stream;
    std::map<u32, Op> ops;
    std::vector<MemLoad> loads;
    std::vector<Draw> draws;
    std::vector<Pass> passes;
    std::vector<Result> results;
    /// RSTA: the renderer's state at the frame's start (surfaces with pixels, depth stores).
    bool has_rasterizer_state = false;
    u32 state_surfaces = 0, state_depths = 0, state_clears = 0;
    std::vector<Program> programs;
    std::map<u32, std::pair<size_t, size_t>> blobs; // id -> (offset in Capture::data, size)
};

class Capture {
public:
    bool Load(const std::string& path, std::string* error);

    std::string path;
    u32 version = 0;
    std::map<std::string, std::string> info;
    std::map<u32, std::string> threads; // THRD: the event threads (the app's own IDs)
    std::vector<std::string> tags;
    std::vector<Event> events;
    Frame frame;
    std::vector<u8> data; // the unpacked stream (blobs point into it)
    u32 t0 = 0, t1 = 0;   // the events' time range

    int TagId(const std::string& name) const;
    const u8* Blob(u32 id, size_t* size) const;
};

} // namespace frame_replay
