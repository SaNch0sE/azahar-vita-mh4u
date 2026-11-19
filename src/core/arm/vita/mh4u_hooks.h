// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include "common/common_types.h"

// MH4U game hooks: once a guest frame, from the emulation thread while the guest is stopped,
// read (and for opt-in switches, write) the game's own objects. The one exception to "data
// only" is the cover trap (one word in the main loop, patched by ARM_VitaNative like the movie
// poll; see docs/plans/vita-mh4u-menu-world-skip.md). Engine addresses from MH4UltimateDC
// (https://github.com/CompaLarios18/MH4UltimateDC), checked against our v1.1 and v1.0
// executables; see docs/plans/vita-mh4u-decomp-perf-hud.md.

namespace Core {
class System;
}
namespace Kernel {
class Process;
}
namespace Memory {
class MemorySystem;
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

/// The private system call the cover trap uses (`svc 0xF1`; 0xF0 is the movie poll's).
constexpr u32 CoverSvc = 0xF1;
constexpr u32 CoverSvcWord = 0xEF000000u | CoverSvc;

/// Where the cover trap goes in this executable: the `ldr r1,[r0]` in sMhMain::move just before
/// the call to sLtRender::move. addr 0 if the executable is not one we know.
struct CoverSite {
    u32 addr = 0;
    const char* version = nullptr;
};
CoverSite FindCoverSite(const Kernel::Process& process, Memory::MemorySystem& memory);

/// True for the trap address of any known version (the svc handler's check, so a save state
/// carrying the patch is served even when the patch itself is switched off).
bool IsCoverSite(u32 addr);

/// The trap was taken and the replaced instruction already emulated: every update of the guest
/// frame is done, its render is about to be built. Walks the layouts (read-only).
void OnTrap(Core::System& system);

/// Cover counters for the once-a-second line, reset by the call.
struct CoverStats {
    u32 traps = 0;       ///< traps taken (~ guest frames while the main loop runs)
    u32 key_changes = 0; ///< times the (gated) cover key changed
    u32 walk_avg_us = 0; ///< layout walk time, mean and worst
    u32 walk_max_us = 0;
};
CoverStats TakeCoverStats();

} // namespace Mh4uHooks
