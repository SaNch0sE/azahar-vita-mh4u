// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// pica_cg's logging: the generators' log lines go to stderr. Shadows common/logging/log.h, so
// the tool links none of the logging backend.

#pragma once

#include <cstdio>
#include <fmt/format.h>
#include "common/logging/formatter.h"

#define PICA_CG_LOG(level, cls, ...)                                                               \
    fmt::print(stderr, "[{}] {}: {}\n", level, #cls, fmt::format(__VA_ARGS__))
#define LOG_TRACE(cls, ...) ((void)0)
#define LOG_DEBUG(cls, ...) ((void)0)
#define LOG_INFO(cls, ...) PICA_CG_LOG("info", cls, __VA_ARGS__)
#define LOG_WARNING(cls, ...) PICA_CG_LOG("warning", cls, __VA_ARGS__)
#define LOG_ERROR(cls, ...) PICA_CG_LOG("error", cls, __VA_ARGS__)
#define LOG_CRITICAL(cls, ...) PICA_CG_LOG("critical", cls, __VA_ARGS__)
