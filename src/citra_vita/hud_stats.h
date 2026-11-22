// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <atomic>
#include "common/common_types.h"

/**
 * The numbers the on-screen stats window shows, published once a second by the run loop
 * from the core's perf stats and read when the interface is drawn.
 */
namespace VitaFrontend::HudStats {

inline std::atomic<u32> speed_percent{0};   ///< emulated time over wall time
inline std::atomic<u32> game_fps10{0};      ///< the guest's frame rate, tenths
inline std::atomic<u32> shown_fps{0};       ///< frames presented in the last second
inline std::atomic<bool> valid{false};

} // namespace VitaFrontend::HudStats
