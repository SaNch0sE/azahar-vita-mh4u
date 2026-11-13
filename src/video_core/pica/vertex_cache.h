// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include "common/common_types.h"
#include "video_core/pica/draw_payload.h"

namespace Pica {

/**
 * Hardware draws whose vertex and index data have not changed since they were last drawn reuse
 * the GPU copy made then, instead of copying out of guest memory again (the copy runs at the
 * CPU's ~300 MB/s ceiling for uncached memory, ~80 ms a second on the ship's deck, where most
 * of the data is static meshes).
 *
 * A draw is keyed by everything that decides what it reads (vertex array configuration, base
 * address, index buffer, count, offset). Its copy lives in a GPU-visible pool, in the same
 * layout the vertex ring would hold it, and the entry stays valid until any byte it was copied
 * from may have changed:
 * - guest CPU stores: the source pages are armed in the guest write tracker, whose dirty runs
 *   arrive at the guest's next data-cache flush (InvalidatePhysical);
 * - the emulator's own writes into guest memory (file reads, GX DMA, IPC buffers):
 *   Memory::WriteBlock & co. call InvalidateHost, the rasterizer's invalidations call
 *   InvalidatePhysical.
 * A key invalidated twice is volatile (dynamic data rewritten every frame) and is not cached
 * again: it would only churn the pool. The pool is a bump allocator; when it is full the GPU is
 * waited for once and the whole cache starts over. Blocks of dropped entries are never reused
 * before that, so queued draws can still read them.
 */
class VertexCache {
    /// Every user but the once-a-second stats reader is the emulation thread: a spinlock
    /// costs an atomic, where std::mutex on the Vita was ~5 us a draw. It sleeps after a while
    /// so a preempted holder on the same core gets to run.
    struct SpinLock {
        std::atomic_flag flag = ATOMIC_FLAG_INIT;
        void lock() {
            for (u32 n = 0; flag.test_and_set(std::memory_order_acquire); n++) {
                if (n > 2000) {
                    std::this_thread::sleep_for(std::chrono::microseconds(50));
                    n = 0;
                }
            }
        }
        void unlock() {
            flag.clear(std::memory_order_release);
        }
    };

public:
    struct Entry {
        u8* block = nullptr;
        DrawArenaLayout layout{};
        u32 vertex_min = 0;
        u32 vertex_max = 0;
        /// Host ranges the copy was taken from.
        std::vector<std::pair<const u8*, u32>> sources;
    };
    static constexpr u32 MaxEntryBytes = 512 * 1024;

    bool enabled = false;

    /// Emulation thread. Copies out the entry for `key` (everything but its sources).
    bool Find(u64 key, Entry& out) {
        std::scoped_lock lock{mutex};
        stat_lookups++;
        const auto it = entries.find(key);
        if (it == entries.end()) {
            return false;
        }
        stat_hits++;
        out.block = it->second.block;
        out.layout = it->second.layout;
        out.vertex_min = it->second.vertex_min;
        out.vertex_max = it->second.vertex_max;
        return true;
    }

    /// Whether a miss on `key` should be cached.
    bool Wanted(u64 key, u32 bytes) {
        std::scoped_lock lock{mutex};
        if (pool == nullptr || bytes > MaxEntryBytes) {
            return false;
        }
        const auto it = volatility.find(key);
        return it == volatility.end() || it->second < 2;
    }

    /// Room for `bytes` in the pool, or nullptr when it is full (the caller waits for the GPU
    /// and calls Reset).
    u8* Allocate(u32 bytes) {
        std::scoped_lock lock{mutex};
        const u32 aligned = (bytes + 63) & ~63u;
        if (pool == nullptr || pool_used + aligned > pool_size) {
            return nullptr;
        }
        u8* const block = pool + pool_used;
        pool_used += aligned;
        return block;
    }

    void Insert(u64 key, Entry&& entry) {
        std::scoped_lock lock{mutex};
        for (const auto& [host, size] : entry.sources) {
            for (uintptr_t page = reinterpret_cast<uintptr_t>(host) >> 12;
                 page <= (reinterpret_cast<uintptr_t>(host) + size - 1) >> 12; page++) {
                watched_pages[page]++;
            }
        }
        watched_count.store(static_cast<u32>(watched_pages.size()), std::memory_order_relaxed);
        entries[key] = std::move(entry);
        stat_inserts++;
    }

    /// Drops every entry that read from [host, host + size). Any thread.
    void InvalidateHost(const u8* host, std::size_t size) {
        // The write hook fires on every IPC buffer write: nothing watched, nothing to do.
        if (size == 0 || watched_count.load(std::memory_order_relaxed) == 0) {
            return;
        }
        std::scoped_lock lock{mutex};
        if (watched_pages.empty()) {
            return;
        }
        const uintptr_t lo = reinterpret_cast<uintptr_t>(host);
        const uintptr_t hi = lo + size;
        bool watched = false;
        for (uintptr_t page = lo >> 12; page <= (hi - 1) >> 12; page++) {
            if (watched_pages.contains(page)) {
                watched = true;
                break;
            }
        }
        if (!watched) {
            return;
        }
        for (auto it = entries.begin(); it != entries.end();) {
            bool overlaps = false;
            for (const auto& [src, n] : it->second.sources) {
                const uintptr_t s = reinterpret_cast<uintptr_t>(src);
                if (s < hi && lo < s + n) {
                    overlaps = true;
                    break;
                }
            }
            if (!overlaps) {
                ++it;
                continue;
            }
            for (const auto& [src, n] : it->second.sources) {
                const uintptr_t s = reinterpret_cast<uintptr_t>(src);
                for (uintptr_t page = s >> 12; page <= (s + n - 1) >> 12; page++) {
                    const auto w = watched_pages.find(page);
                    if (w != watched_pages.end() && --w->second == 0) {
                        watched_pages.erase(w);
                    }
                }
            }
            watched_count.store(static_cast<u32>(watched_pages.size()), std::memory_order_relaxed);
            u8& v = volatility[it->first];
            v = v < 255 ? v + 1 : v;
            stat_invalidations++;
            it = entries.erase(it);
        }
    }

    /// Every entry dropped and the pool emptied: only once the GPU has finished with it.
    void Reset() {
        std::scoped_lock lock{mutex};
        entries.clear();
        watched_pages.clear();
        watched_count.store(0, std::memory_order_relaxed);
        pool_used = 0;
        stat_resets++;
    }

    void SetPool(u8* base, u32 size) {
        std::scoped_lock lock{mutex};
        pool = base;
        pool_size = size;
        pool_used = 0;
    }
    [[nodiscard]] bool HasPool() const {
        return pool != nullptr;
    }

    struct Stats {
        u32 lookups, hits, inserts, invalidations, resets, entries, pool_kib;
    };
    Stats TakeStats() {
        std::scoped_lock lock{mutex};
        const Stats s{stat_lookups, stat_hits, stat_inserts, stat_invalidations, stat_resets,
                      static_cast<u32>(entries.size()), pool_used / 1024};
        stat_lookups = stat_hits = stat_inserts = stat_invalidations = stat_resets = 0;
        return s;
    }

private:
    SpinLock mutex;
    std::atomic<u32> watched_count{0}; ///< watched_pages.size(), readable without the lock
    std::unordered_map<u64, Entry> entries;
    std::unordered_map<u64, u8> volatility;
    std::unordered_map<uintptr_t, u32> watched_pages;
    u8* pool = nullptr;
    u32 pool_size = 0;
    u32 pool_used = 0;
    u32 stat_lookups = 0, stat_hits = 0, stat_inserts = 0, stat_invalidations = 0,
        stat_resets = 0;
};

/// The one cache, owned by the emulation-side PICA (pica_core.cpp).
VertexCache& GetVertexCache();

} // namespace Pica
