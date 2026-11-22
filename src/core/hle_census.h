// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include "common/common_types.h"

namespace Core {

/**
 * gxm_flags `perflog`: what each emulated core costs the emulation thread, and how long the work
 * a second emulation thread would wait for at the HLE lock takes. The emulation threads write it, the sites under the HLE lock; the Vita frontend sets
 * `on` and takes it once a second under the lock too. With parallel cores a core's run and idle
 * times are added outside the lock, so a take can lose part of one slice's. Off, every site costs
 * one test of `on`.
 */
struct HleCensus {
    static constexpr std::size_t MaxCores = 4;
    /// Hold lengths by power of two: [0] under 1 us, [i] under 2^i us, the last 2^16 us and over.
    static constexpr std::size_t HoldBuckets = 18;

    using Clock = std::chrono::steady_clock;

    struct CoreTime {
        u64 run_us{};      ///< Run or Step: azaharRun and the svcs its slices exit for
        u64 svc_us{};      ///< of run_us, CallSVC
        u64 azahar_us{};   ///< of run_us, azaharRun (the guest and the world switches)
        u64 guest_ticks{}; ///< the guest's own time, on the guest clock
        u64 event_us{};    ///< the core's timer events and reschedules at slice boundaries
        u64 idle_us{};     ///< no thread to run: the GPU wait or the skip to the next event
        u32 runs{};        ///< Run or Step calls
        u64 lock_wait_us{};    ///< waiting for the HLE lock another thread held
        u32 lock_waits{};      ///< such waits
        u64 horizon_wait_us{}; ///< parallel cores: waiting for another core's progress
        u32 horizon_waits{};
        u32 quiet_boundaries{}; ///< parallel cores: boundaries that took no lock
    };

    /// One kind of hold: an svc, an HLE service's IPC, a timer event, the GPU's interrupts.
    struct Site {
        std::string name;
        u32 count{};
        u64 total_us{};
        u32 max_us{};
    };

#if defined(VITA_MULTICORE) && defined(VITA_DIAGNOSTICS)
    bool on{};
#else
    /// Multicore diagnostics builds only: the census measures what parallel cores would contend
    /// for, and any other build compiles every site of it out.
    static constexpr bool on = false;
#endif
    std::array<CoreTime, MaxCores> cores{};
    /// The holds a second thread would wait out: an svc, a core's slice boundary (its events
    /// and reschedule), the GPU's interrupt delivery. Timer events inside a boundary go to
    /// `sites` but not here.
    std::array<u32, HoldBuckets> holds{};
    u64 hold_us{};
    /// By a key that lives as long as the site's owner: the svc's table entry, the HLE
    /// service, the event type.
    std::unordered_map<const void*, Site> sites;
    /// Lock waits by the site that held the lock when the wait began (its key in `sites`).
    std::unordered_map<const void*, Site> waits;

    /// Under the lock, just taken: core `core_id` waited `us` for it, `held_by` holding it.
    void Wait(u32 core_id, const void* held_by, u64 us) {
        auto& core = Of(core_id);
        core.lock_wait_us += us;
        core.lock_waits++;
        Site& site = waits[held_by];
        site.count++;
        site.total_us += us;
        site.max_us = std::max<u32>(site.max_us, static_cast<u32>(std::min<u64>(us, ~0u)));
    }

    static u64 Since(Clock::time_point start) {
        return static_cast<u64>(
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
    }

    /// Counts a hold of `us` at `key`, naming the site `name()` the first time. `top`: one the
    /// lock histogram counts.
    template <typename Name>
    void Hold(const void* key, Name&& name, u64 us, bool top) {
        auto [it, added] = sites.try_emplace(key);
        Site& site = it->second;
        if (added) {
            site.name = name();
        }
        site.count++;
        site.total_us += us;
        site.max_us = std::max<u32>(site.max_us, static_cast<u32>(std::min<u64>(us, ~0u)));
        if (top) {
            std::size_t bucket = 0;
            while (bucket + 1 < HoldBuckets && us >= (1ull << bucket)) {
                bucket++;
            }
            holds[bucket]++;
            hold_us += us;
        }
    }

    CoreTime& Of(u32 core_id) {
        return cores[core_id < MaxCores ? core_id : MaxCores - 1];
    }

    /// The counts since the last call.
    HleCensus Take();
};

extern HleCensus hle_census;
/// The site holding the HLE lock now (a key of HleCensus::sites), set as a hold starts: what a
/// thread that has to wait for the lock waits for. Written with the census on only.
extern std::atomic<const void*> hle_holder;

/// The HLE lock for emulated core `core_id`'s thread, with the census on timing a wait for it
/// and charging the wait to the holder.
template <typename Lock>
inline std::unique_lock<Lock> LockHle(Lock& lock, u32 core_id) {
    if (!hle_census.on) [[likely]] {
        return std::unique_lock{lock};
    }
    std::unique_lock held{lock, std::try_to_lock};
    if (held.owns_lock()) {
        return held;
    }
    const void* const held_by = hle_holder.load(std::memory_order_relaxed);
    const auto start = HleCensus::Clock::now();
    held.lock();
    hle_census.Wait(core_id, held_by, HleCensus::Since(start));
    return held;
}

} // namespace Core
