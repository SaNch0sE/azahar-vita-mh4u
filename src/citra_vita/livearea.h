// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <string>
#include <vector>

#include "common/common_types.h"

/**
 * A title pinned to the LiveArea: a bubble of its own, whose eboot starts Azahar with the title
 * (bubble/main.c). The bubble is put together from Azahar's own package and promoted as VitaShell
 * installs a vpk.
 */
namespace VitaFrontend::LiveArea {

/// Whether the title at `path` has a bubble.
bool IsPinned(const std::string& path);

/// Puts a bubble for the title at `path` on the LiveArea, named `name`, with `icon` (its 48x48
/// icon as A8B8G8R8 texels, or empty for a plain one). Empty when it worked; else what the user
/// can do about it (the details go to the log).
std::string Pin(const std::string& path, const std::string& name, const std::vector<u32>& icon);

/// Takes the title's bubble off the LiveArea. As Pin, empty when it worked.
std::string Unpin(const std::string& path);

} // namespace VitaFrontend::LiveArea
