// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <string>

namespace VitaFrontend {

/// printf to the console, also kept (the last two minutes of lines) for the bug report: the
/// release build has no log file, and the per-second speed lines are what a report needs most.
void ReportPrintf(const char* format, ...) __attribute__((format(printf, 1, 2)));

/// Writes ux0:data/mh4u/reports/mh4u_report_<date>_<time>.zip: report.txt (version, game
/// program, mod, settings, files, the recent speed lines), the in-game save, the game's extra
/// data and the newest crash dump; with_state adds the newest save state. Runs on the emulation
/// thread while the pause menu is up. Returns the message to show.
std::string WriteBugReport(bool with_state);

} // namespace VitaFrontend
