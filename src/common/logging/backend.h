// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <string_view>
#include "common/logging/filter.h"
#ifdef HAVE_LIBRETRO
#include "libretro.h"
#endif

namespace Common::Log {

class Filter;

/// Initializes the logging system. This should be the first thing called in main.
void Initialize(std::string_view log_file = "");
#ifdef HAVE_LIBRETRO
void LibRetroStart(retro_log_printf_t callback);
#endif

/// Flushes and closes the backends. Messages are written as they are logged, so this is only
/// needed to make sure what has been logged has reached the disk before the process ends.
void Stop();

void DisableLoggingInTests();

/**
 * The global filter will prevent any messages from even being processed if they are filtered.
 */
void SetGlobalFilter(const Filter& filter);

/**
 * Only allow messages that match the specified regex. The regex is matched against the final log
 * text.
 */
bool SetRegexFilter(const std::string& regex);

void SetColorConsoleBackendEnabled(bool enabled);

#ifdef __vita__
/// The crash report the next start prints (citra_vita's CrashReport::PrintLast).
constexpr char CrashReportPath[] = "ux0:data/azahar/crash.txt";
/// Appends `text` to the crash report, where a critical message outlives a lost debug
/// link and the next launch's console. Allocates nothing.
void AppendCrashReport(std::string_view text);
/// Called before the first text a report appends to the crash report: the frontend's lines on
/// what the run had (the console's plugins, the settings), appended through AppendCrashReport.
/// It allocates nothing. A run's first report starts at its first critical line; a crash renews
/// it (RenewCrashReport), and a report packed away ends it (EndCrashReport).
void SetCrashReportPreamble(void (*hook)());
/// At a crash's start: a preamble written more than a few seconds before (an earlier critical
/// line's, a load error's) is written again before the crash's next line, with the game, the
/// settings and the console's text as they are now. One written for this crash's own earlier
/// lines stays.
void RenewCrashReport();
/// The crash report is done with (packed into an archive): removed when `remove`, and the next
/// report starts with its own preamble.
void EndCrashReport(bool remove);
/// Called by Stop, which every failed assertion calls before its fault: the frontend's last act
/// (the Vita's blacks out its display).
void SetStopHook(void (*hook)());
#endif
} // namespace Common::Log
