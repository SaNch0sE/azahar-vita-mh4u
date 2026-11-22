// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <string>
#include <vector>
#include "common/common_types.h"

namespace Core {

struct SaveStateInfo {
    u32 slot;
    u64 time;
    enum class ValidationStatus {
        OK,
        RevisionMismatch,
        BuildMismatch,
    } status;
    std::string build_name;
    std::string build_version;
};

constexpr u32 SaveStateSlotCount = 11; // Maximum count of savestate slots

/// The serialized state's layout: a state another build saved loads where this matches
/// (RevisionMismatch), else it is refused (BuildMismatch). Raised whenever what the state holds
/// changes; tools/savestate_check.py stops the build at a change to the code that serializes it
/// until it is raised or recorded as compatible. 0: a state saved before the format was written.
constexpr u32 SaveStateFormat = 2;

std::vector<SaveStateInfo> ListSaveStates(u64 program_id, u64 movie_id);

SaveStateInfo GetSaveStateInfo(u64 program_id, u64 movie_id, u32 slot);

} // namespace Core
