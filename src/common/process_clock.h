// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <chrono>
#include "common/common_types.h"

#ifdef __vita__
#include <psp2/kernel/processmgr.h>
#endif

namespace Common {

/**
 * A steady clock for the paths that read the time often. On the Vita, libstdc++'s steady_clock
 * and high_resolution_clock are gettimeofday (it is built without CLOCK_MONOTONIC): a syscall
 * whose kernel side also converts the time to a calendar date. This is the process time,
 * sceKernelGetProcessTimeWide, in microseconds: a syscall still, without the date. Elsewhere it is
 * steady_clock.
 */
struct ProcessClock {
    using rep = s64;
    using period = std::micro;
    using duration = std::chrono::microseconds;
    using time_point = std::chrono::time_point<ProcessClock>;
    static constexpr bool is_steady = true;

    static time_point now() noexcept {
#ifdef __vita__
        return time_point(duration(static_cast<rep>(sceKernelGetProcessTimeWide())));
#else
        return time_point(std::chrono::duration_cast<duration>(
            std::chrono::steady_clock::now().time_since_epoch()));
#endif
    }
};

} // namespace Common
