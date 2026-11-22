// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// pica_cg has no logging backend (see log.h next to this); common/assert.h only stops it.

#pragma once

namespace Common::Log {
inline void Stop() {}
} // namespace Common::Log
