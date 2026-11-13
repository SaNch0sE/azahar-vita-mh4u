// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

// emuprof (diagnostic): where the emulation thread's HLE time goes. Every system call (IPC
// requests by service and command) and every timing event callback is timed and summed by name;
// the frontend prints the largest once a second. Emulation thread only: no locking.

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>
#include <fmt/format.h>
#include "common/common_types.h"

namespace Common::EmuProf {

struct Entry {
    u64 us = 0;
    u32 count = 0;
};

inline bool enabled = false;
inline std::unordered_map<std::string, Entry> entries;
/// Literal keys, by address: no string is built or hashed per call (the string-keyed Add costs
/// a few microseconds on the Vita, which distorted fine-grained timings).
inline std::unordered_map<const char*, Entry> literal_entries;

inline void Add(const std::string& key, u64 us) {
    auto& e = entries[key];
    e.us += us;
    e.count++;
}
inline void Add(const char* key, u64 us) {
    auto& e = literal_entries[key];
    e.us += us;
    e.count++;
}

/// The `top` largest entries since the last call, as one line; clears them.
inline std::string TakeReport(std::size_t top = 12) {
    std::vector<std::pair<std::string, Entry>> v(entries.begin(), entries.end());
    entries.clear();
    for (const auto& [key, e] : literal_entries) {
        v.emplace_back(key, e);
    }
    literal_entries.clear();
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.us > b.second.us; });
    std::string out;
    for (std::size_t i = 0; i < std::min(top, v.size()); i++) {
        out += fmt::format(" | {} {} ms /{}", v[i].first, v[i].second.us / 1000, v[i].second.count);
    }
    return out;
}

} // namespace Common::EmuProf
