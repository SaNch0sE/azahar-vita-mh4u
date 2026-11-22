// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <atomic>
#include "common/common_types.h"

namespace Common {

/**
 * The host CPU's clock in MHz, set once a guest frame (RendererSgxd::EndFrame: a plugin such as
 * PSVshell may change it). It turns a thread's cycle counter into time: azaharCycles, the PMU's
 * cycle counter, which ThreadMgr keeps per thread and user code reads without a syscall. A
 * thread's cycles stop while it sleeps, so they time work on one thread, not waits.
 */
inline std::atomic<u32> host_clock_mhz{444};

inline void SetHostClockMhz(u32 mhz) {
    host_clock_mhz.store(mhz != 0 ? mhz : 1, std::memory_order_relaxed);
}

[[nodiscard]] inline u32 HostClockMhz() {
    return host_clock_mhz.load(std::memory_order_relaxed);
}

/// Microseconds of a difference of one thread's cycle counter.
[[nodiscard]] inline u64 CyclesToUs(u32 cycles) {
    return cycles / HostClockMhz();
}

} // namespace Common
