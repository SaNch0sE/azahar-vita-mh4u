// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// frame_replay: the results of the recorded frame's replay on the replay player
// (player.sh; FORMAT.md "Replay results").
#pragma once

#include <map>
#include <string>
#include <vector>
#include "capture.h"

namespace frame_replay {

struct ReplayResults {
    std::string path;
    /// Where the replay ran: "player" (the replay player), the results'
    /// CAPTURE.<where>.bin.
    std::string where;
    u32 draws = 0;
    std::vector<std::string> text;
    struct PassTime {
        u32 run, pass, ta_us, d3_us;
    };
    std::vector<PassTime> passes;
    struct OpTime {
        u32 run, seq, us;
    };
    std::vector<OpTime> ops;
    struct Cost {
        u32 ta_us, d3_us, pass;
    };
    std::map<u32, Cost> costs; // by draw
    struct Image {
        u32 width = 0, height = 0, addr = 0;
        std::vector<u8> rgba;
    };
    std::map<u32, Image> images; // by draw
    /// The `results` request's run: every pass's surface and transfer's output (RPRS), as
    /// Frame::results has them from the console.
    struct Result {
        u32 index, kind, addr, width, height, format;
        std::vector<u8> bytes;
    };
    std::vector<Result> results;
    /// The `profile` request's runs (RFRM): the time to hand the frame over, to the GPU's idle,
    /// the memory loads' part, and the present's.
    struct Run {
        u32 index, submit_us, idle_us, loads_us, present_us;
    };
    std::vector<Run> runs;
};

/// CAPTURE.<where>.bin, if there is one; false and a message otherwise.
bool LoadReplays(const Capture* cap, std::string* error, const std::string& where = "player");
const ReplayResults* Replays(const Capture* cap);

} // namespace frame_replay
