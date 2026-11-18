// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <mutex>

#include "common/big_arena.h"

namespace Common::BigArena {

namespace {

constexpr std::size_t Granule = 4096;
constexpr std::uint16_t UsedBit = 0x8000;
constexpr std::size_t MaxGranules = 0x7fff; // run lengths are 15 bits

std::mutex g_lock;
unsigned char* g_base = nullptr;
std::size_t g_granules = 0;
// For the first granule of each run: its length, with UsedBit when allocated. For the last
// granule of each run: its length (so a free can find the run before it). Other entries are
// stale and never read.
std::uint16_t* g_head = nullptr;
std::uint16_t* g_tail = nullptr;
std::size_t g_used = 0, g_peak = 0, g_allocs = 0, g_fallbacks = 0;

void SetRun(std::size_t start, std::size_t len, bool used) {
    g_head[start] = static_cast<std::uint16_t>(len | (used ? UsedBit : 0));
    g_tail[start + len - 1] = static_cast<std::uint16_t>(len);
}

bool InArena(const void* p) {
    const auto* b = static_cast<const unsigned char*>(p);
    return g_base != nullptr && b >= g_base && b < g_base + g_granules * Granule;
}

} // namespace

bool Init(std::size_t bytes) {
    const std::size_t granules = std::min(bytes / Granule, MaxGranules);
    if (g_base != nullptr || granules == 0) {
        return false;
    }
    auto* base = static_cast<unsigned char*>(std::malloc(granules * Granule));
    auto* head = static_cast<std::uint16_t*>(std::calloc(granules, sizeof(std::uint16_t)));
    auto* tail = static_cast<std::uint16_t*>(std::calloc(granules, sizeof(std::uint16_t)));
    if (base == nullptr || head == nullptr || tail == nullptr) {
        std::free(base);
        std::free(head);
        std::free(tail);
        return false;
    }
    std::scoped_lock lock{g_lock};
    g_head = head;
    g_tail = tail;
    g_granules = granules;
    SetRun(0, granules, false);
    g_base = base; // last: InArena is false until the run table is set
    return true;
}

void* Alloc(std::size_t bytes) noexcept {
    if (bytes >= MinBytes && g_base != nullptr) {
        const std::size_t need = (bytes + Granule - 1) / Granule;
        std::scoped_lock lock{g_lock};
        // Best fit, lowest address on ties.
        std::size_t best = g_granules, best_len = 0;
        for (std::size_t g = 0; g < g_granules;) {
            const std::size_t len = g_head[g] & ~UsedBit;
            if ((g_head[g] & UsedBit) == 0 && len >= need && (best_len == 0 || len < best_len)) {
                best = g;
                best_len = len;
                if (len == need) {
                    break;
                }
            }
            g += len;
        }
        if (best_len != 0) {
            SetRun(best, need, true);
            if (best_len > need) {
                SetRun(best + need, best_len - need, false);
            }
            g_used += need * Granule;
            g_peak = std::max(g_peak, g_used);
            g_allocs++;
            return g_base + best * Granule;
        }
        g_fallbacks++;
    }
    return std::malloc(bytes != 0 ? bytes : 1);
}

void Free(void* p) noexcept {
    if (p == nullptr) {
        return;
    }
    if (!InArena(p)) {
        std::free(p);
        return;
    }
    std::scoped_lock lock{g_lock};
    std::size_t start = static_cast<std::size_t>(static_cast<unsigned char*>(p) - g_base) / Granule;
    std::size_t len = g_head[start] & ~UsedBit;
    g_used -= len * Granule;
    // Merge with the free run after, then with the free run before.
    const std::size_t next = start + len;
    if (next < g_granules && (g_head[next] & UsedBit) == 0) {
        len += g_head[next];
    }
    if (start > 0) {
        const std::size_t prev_len = g_tail[start - 1];
        const std::size_t prev = start - prev_len;
        if ((g_head[prev] & UsedBit) == 0) {
            start = prev;
            len += prev_len;
        }
    }
    SetRun(start, len, false);
}

Stats GetStats() {
    Stats s;
    std::scoped_lock lock{g_lock};
    if (g_base == nullptr) {
        return s;
    }
    s.capacity = g_granules * Granule;
    s.used = g_used;
    s.peak = g_peak;
    s.allocs = g_allocs;
    s.fallbacks = g_fallbacks;
    for (std::size_t g = 0; g < g_granules;) {
        const std::size_t len = g_head[g] & ~UsedBit;
        if ((g_head[g] & UsedBit) == 0) {
            s.largest = std::max(s.largest, len * Granule);
        }
        g += len;
    }
    return s;
}

} // namespace Common::BigArena
