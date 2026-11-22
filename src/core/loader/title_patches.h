// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <string>
#include <string_view>
#include <vector>
#include "common/common_types.h"

namespace Core {
class System;
}

namespace Kernel {
class Process;
}

namespace Loader {

/// A patch built in for a title, as a menu shows it.
struct TitlePatchInfo {
    const char* name;        ///< a few words
    const char* description; ///< what it changes, for the player
};

/// The patches built in for the title, in the order they go in; empty for a title with none.
[[nodiscard]] std::vector<TitlePatchInfo> TitlePatchesOf(u64 title_id);

/**
 * Writes the code patches built in for the process's title into its code: after the title is
 * loaded, and again after a save state's load, which restores the code the state was saved with.
 * A patch goes in only where every word it replaces is the one it was made for (another version
 * of the title keeps its code).
 * @returns for each of the title's patches (TitlePatchesOf's order), whether it is in the code:
 *     written now, or there already (a state saved with it).
 */
std::vector<bool> ApplyTitlePatches(Core::System& system, const Kernel::Process& process);

/**
 * What the player can do when the patches built in for a game do not run on the copy loaded:
 * another region's copy (its product code, `product_code`, the patched one's but for the region's
 * letter: the patches are one title ID's), or the patched title with other code (another
 * revision, `remaster_version`, or an update; `in` from ApplyTitlePatches). Names the copy they
 * are made for. Empty when they run, or when the game has none.
 */
[[nodiscard]] std::string TitlePatchesWarning(u64 title_id, std::string_view product_code,
                                              u16 remaster_version, const std::vector<bool>& in);

} // namespace Loader
