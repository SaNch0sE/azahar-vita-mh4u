// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

namespace GxmRenderer {

/**
 * Bring-up switches read once from ux0:data/azahar/sgxd_flags.txt (and host0:sgxd_flags.txt):
 * whitespace-separated words, one per switch.
 * Absent file: nothing set. Compiled in only when VITA_DIAGNOSTICS is defined; the release build
 * answers false to every one of them and reads no file.
 */
[[nodiscard]] bool GxmFlag(const char* name);

} // namespace GxmRenderer
