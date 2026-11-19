// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

// MH4U game hooks by data only: once a guest frame, from the emulation thread while the guest is
// stopped, read (and for opt-in switches, write) the game's own objects. No code is patched and
// no code page is synced, so none of the hazards of rewriting code after a state load apply.
// Engine addresses from MH4UltimateDC (https://github.com/CompaLarios18/MH4UltimateDC), checked
// against our v1.1 and v1.0 executables; see docs/plans/vita-mh4u-decomp-perf-hud.md.

namespace Core {
class System;
}

namespace Mh4uHooks {

/// Call every guest slice (ARM_VitaNative::RunSlice); does its work once per guest frame.
void Tick(Core::System& system);

/// Puts back every game value the hooks changed, so a save state carries vanilla data (it loads
/// on other builds). The next Tick applies them again.
void BeforeSave(Core::System& system);

/// True when ux0:data/mh4u/<name> exists, or (diagnostics builds) gxm flag <name> is set.
bool Switch(const char* name);

/// The bottom screen is not shown (Single screen layout, not swapped) and its skip is allowed
/// (`nobottomskip` absent).
bool BottomHidden();

} // namespace Mh4uHooks
