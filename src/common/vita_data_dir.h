// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

/// The port's own data folder on the Vita: the ROM, the update and saves (sdmc), mods, cheats,
/// diagnostics. A release uses nothing else; diagnostic builds fall back to Azahar's folder for
/// the ROM and the game's data when the port has no copy of its own.
#define VITA_DATA_DIR "ux0:data/mh4u"
/// Azahar's folder (diagnostic builds' fallback).
#define VITA_AZAHAR_DIR "ux0:data/azahar"
