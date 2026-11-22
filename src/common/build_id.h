// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <span>
#include "common/common_types.h"

namespace Common {

/// The link's build ID (the note the linker makes of the executable's contents, citra_vita's
/// ldscript.ld): one value a linked executable, whatever its commit says. Empty in an executable
/// linked without the note.
[[nodiscard]] std::span<const u8> BuildId();

} // namespace Common
