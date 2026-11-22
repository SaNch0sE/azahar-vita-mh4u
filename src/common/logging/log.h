// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <algorithm>
#include <array>
#include <string_view>

#include "common/logging/formatter.h"
#include "common/logging/types.h"

namespace Common::Log {

// trims up to and including the last of ../, ..\, src/, src\ in a string
constexpr const char* TrimSourcePath(std::string_view source) {
    const auto rfind = [source](const std::string_view match) {
        return source.rfind(match) == source.npos ? 0 : (source.rfind(match) + match.size());
    };
    auto idx = std::max({rfind("src/"), rfind("src\\"), rfind("../"), rfind("..\\")});
    return source.data() + idx;
}

/// Logs a message to the global logger, using fmt
void FmtLogMessageImpl(Class log_class, Level log_level, const char* filename,
                       unsigned int line_num, const char* function, fmt::string_view format,
                       const fmt::format_args& args);

/// Each class's lowest level logged, as the filter in use has it (Trace before the backend
/// starts). LOG_TRACE and LOG_DEBUG test their class's before they evaluate their arguments.
extern std::array<Level, static_cast<std::size_t>(Class::Count)> class_level;
inline bool Logs(Class log_class, Level log_level) {
    return log_level >= class_level[static_cast<std::size_t>(log_class)];
}

template <typename... Args>
void FmtLogMessage(Class log_class, Level log_level, const char* filename, unsigned int line_num,
                   const char* function, fmt::format_string<Args...> format, const Args&... args) {
    FmtLogMessageImpl(log_class, log_level, filename, line_num, function, format,
                      fmt::make_format_args(args...));
}

} // namespace Common::Log

// The trimmed source path, as a compile-time constant. Evaluated at run time it rescans
// __FILE__ before the level filter runs, so every filtered LOG_TRACE paid for it: a quarter
// of the emulation thread's samples on the Vita (a profile of a homebrew drawing a triangle).
#define LOG_SOURCE_FILE ([]() consteval { return Common::Log::TrimSourcePath(__FILE__); }())

// Define the fmt lib macros
#define LOG_GENERIC(log_class, log_level, ...)                                                     \
    Common::Log::FmtLogMessage(log_class, log_level, LOG_SOURCE_FILE, __LINE__, __func__,          \
                               __VA_ARGS__)

// LOG_TRACE and LOG_DEBUG sit in the svc, scheduling and IPC paths: with Debug filtered out for
// their class they cost one compare, not the argument packing and the call into the backend's
// filter. LOG_INFO, LOG_WARNING and LOG_ERROR test the same way. #ifdef _DEBUG
#define LOG_TRACE(log_class, ...)                                                                  \
    (Common::Log::Logs(Common::Log::Class::log_class, Common::Log::Level::Debug)                   \
         ? Common::Log::FmtLogMessage(Common::Log::Class::log_class, Common::Log::Level::Debug,    \
                                      LOG_SOURCE_FILE, __LINE__, __func__, __VA_ARGS__)            \
         : void())
// #else
// #define LOG_TRACE(log_class, fmt, ...) (void(0))
// #endif

#define LOG_DEBUG(log_class, ...)                                                                  \
    (Common::Log::Logs(Common::Log::Class::log_class, Common::Log::Level::Debug)                   \
         ? Common::Log::FmtLogMessage(Common::Log::Class::log_class, Common::Log::Level::Debug,    \
                                      LOG_SOURCE_FILE, __LINE__, __func__, __VA_ARGS__)            \
         : void())
#define LOG_INFO(log_class, ...)                                                                   \
    (Common::Log::Logs(Common::Log::Class::log_class, Common::Log::Level::Info)                    \
         ? Common::Log::FmtLogMessage(Common::Log::Class::log_class, Common::Log::Level::Info,     \
                                      LOG_SOURCE_FILE, __LINE__, __func__, __VA_ARGS__)            \
         : void())
#define LOG_WARNING(log_class, ...)                                                                \
    (Common::Log::Logs(Common::Log::Class::log_class, Common::Log::Level::Warning)                 \
         ? Common::Log::FmtLogMessage(Common::Log::Class::log_class, Common::Log::Level::Warning,  \
                                      LOG_SOURCE_FILE, __LINE__, __func__, __VA_ARGS__)            \
         : void())
#define LOG_ERROR(log_class, ...)                                                                  \
    (Common::Log::Logs(Common::Log::Class::log_class, Common::Log::Level::Error)                   \
         ? Common::Log::FmtLogMessage(Common::Log::Class::log_class, Common::Log::Level::Error,    \
                                      LOG_SOURCE_FILE, __LINE__, __func__, __VA_ARGS__)            \
         : void())
#define LOG_CRITICAL(log_class, ...)                                                               \
    Common::Log::FmtLogMessage(Common::Log::Class::log_class, Common::Log::Level::Critical,        \
                               LOG_SOURCE_FILE, __LINE__, __func__, __VA_ARGS__)
