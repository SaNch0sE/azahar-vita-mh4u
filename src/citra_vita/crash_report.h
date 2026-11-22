// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include "common/common_types.h"

namespace Cheats {
class CheatEngine;
}

namespace VitaFrontend::CrashReport {

/// The last run's report, if it left one: printed on the debug channel and set aside
/// (ux0:data/azahar/crash.txt to crash-last.txt).
void PrintLast();

/// What the console runs besides its firmware, for every report this run writes: the kernel
/// plugins, this process's modules and the user plugins config.txt names, each with its file's
/// SHA-256; `version` is the loaded module's (0: none, which lists no plugins) and `cores`
/// azaharUnlockCores' result. They head the report, on the memory card only. Once, after the
/// module is loaded.
void NoteSystem(int version, int cores);

/// The firmware for the report's header, by its GPU driver's module NID (HENkaku has the
/// firmware's version read 3.70).
void NoteGpuDriver(u32 module_nid);

/// The running title for the report: its file, its program ID, `details` (what copy it is: its
/// name, product code and version, the file's type and size) and its start; an empty path at its
/// end.
void NoteGame(const std::string& path, u64 program_id, const std::string& details);

/// The running title's 3GX plugin for the report: its title, author and file; empty for none.
void NotePlugin(const std::string& plugin);

/// The session's line of the report: NoteGame counts the titles started; these a title's jump to
/// another one (which started through NoteGame too), a resume from standby (from any thread) and a
/// savestate loaded.
void NoteJump();
void NoteResume();
void NoteStateLoaded(u32 slot);

/// The running title's cheats that are on, by name, for the report's settings lines (which also
/// carry Config::WriteApplied's values): at a title's start and at each switch in its Cheats tab;
/// nullptr at its end.
void NoteCheats(const Cheats::CheatEngine* engine);

/// `text` on the debug channel and appended to ux0:data/azahar/crash.txt, where it outlives a
/// lost debug link. Allocates nothing: the heap may be what failed.
void Write(const char* text);

/// The report of a crash on the calling thread, written: `reason`, the thread and its stack, the
/// size of the last allocation malloc refused, the return addresses found on the stack from here
/// up (the throw's or the abort's callers, before anything unwinds) and the call chain as the
/// unwinder walks it. Once: the abort that follows adds none. Allocates nothing.
void WriteStackReport(const char* reason);

/// What a core dump cannot show, for a crash's report: the render and DSP threads' own views and
/// the memory left (the heap, its largest free run, the GPU's pool, the system's). Read on a thread
/// of its own, which this waits five seconds for at most, so that it cannot hold up the recovery.
/// Allocates nothing.
void WriteProcessState();

/// The display blacked out (WriteStackReport, the stall watch and, through Common::Log's stop
/// hook, every failed assertion do it before the fault): a faulted process stays until its core
/// dump is written, minutes over host0, and would hold its last picture on the OLED.
void BlankDisplay();

/// The heap's live blocks of 512 KiB or more, each with the return addresses on its allocating
/// thread's stack: on the debug channel, and with `to_report` in the crash report too.
void PrintHeapSites(bool to_report);

/// From now on, every heap block's allocation site is kept (`leaktrack`), in tables of their own
/// outside the heap; PrintLeaks groups the blocks in use by the site that allocated them (`leaks`):
/// what quits leave behind, and where it came from; with `every_chunk` each such block as well
/// (`leaks all`). Diagnostics builds only.
#ifdef VITA_DIAGNOSTICS
void StartLeakTracking();
void PrintLeaks(bool every_chunk);
#endif

/// The stall watch (with the command channel): a thread that, once the frontend's loop has not
/// beaten for StallSeconds (a frozen frontend: its loops read the command channel once a second
/// and the pause's wait beats too; a title's load or a savestate's save stops them for less),
/// prints every emulator thread's state, what it waits on and the return addresses on its stack
/// (PrintThreadStates), then faults (an undefined instruction) so that the system keeps the
/// process for a core dump. StallWatchBeat: the loop alive. From the main thread.
constexpr unsigned StallSeconds = 40;
void StartStallWatch();
void StallWatchBeat();
/// The listing: on the debug channel, or with `to_report` in the crash report on the memory card
/// only.
void PrintThreadStates(const char* reason, bool to_report = false);

/// SceKernelBusError's dump of the bus's error registers (azaharBusError): its handler parks the
/// core that took a bus error for good, and the threads pinned there never run again. On the debug
/// channel, or with `to_report` in the crash report.
void PrintBusErrorDump(bool to_report);

/// SceKernelBusError's handler hooked (azaharBusErrorWatch): the first bus error's moment, every
/// core's PC and thread, is kept for PrintBusErrorDump. Once, at start.
void WatchBusErrors();

/// The guest's translation tables (azaharTableScan): stale copies of the app's level-1 entries
/// and descriptors that name no memory, a candidate for that bus error's read.
void PrintTableScan(bool to_report);

} // namespace VitaFrontend::CrashReport
