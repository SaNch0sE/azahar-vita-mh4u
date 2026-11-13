// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

/// The port's own data folder on the Vita: the ROM, the update and saves (sdmc), mods, cheats,
/// diagnostics. Separate from Azahar's ux0:data/azahar, which a first boot copies from.
#define VITA_DATA_DIR "ux0:data/mh4u"
/// Azahar's folder, read once to migrate from.
#define VITA_AZAHAR_DIR "ux0:data/azahar"
