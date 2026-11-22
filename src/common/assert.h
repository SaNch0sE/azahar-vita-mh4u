// Copyright 2013 Dolphin Emulator Project / 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <cstdlib>
#include <iostream>
#ifdef CITRA_LINUX_GCC_BACKTRACE
#define BOOST_STACKTRACE_USE_BACKTRACE
#include <boost/stacktrace.hpp>
#undef BOOST_STACKTRACE_USE_BACKTRACE
#endif
#include "common/common_funcs.h"
#include "common/logging/backend.h"
#include "common/logging/log.h"

// For asserts we'd like to keep all the junk executed when an assert happens away from the
// important code in the function. One way of doing this is to put all the relevant code inside a
// lambda and force the compiler to not inline it.

#define ASSERT(_a_)                                                                                \
    do                                                                                             \
        if (!(_a_)) [[unlikely]] {                                                                 \
            []() CITRA_NO_INLINE CITRA_NO_RETURN {                                                 \
                LOG_CRITICAL(Debug, "Assertion Failed!");                                          \
                Common::Log::Stop();                                                               \
                Crash();                                                                           \
                exit(1);                                                                           \
            }();                                                                                   \
        }                                                                                          \
    while (0)

#define ASSERT_MSG(_a_, ...)                                                                       \
    do                                                                                             \
        if (!(_a_)) [[unlikely]] {                                                                 \
            [&]() CITRA_NO_INLINE CITRA_NO_RETURN {                                                \
                LOG_CRITICAL(Debug, "Assertion Failed!\n" __VA_ARGS__);                            \
                Common::Log::Stop();                                                               \
                Crash();                                                                           \
                exit(1);                                                                           \
            }();                                                                                   \
        }                                                                                          \
    while (0)

#define UNREACHABLE()                                                                              \
    ([]() CITRA_NO_INLINE CITRA_NO_RETURN {                                                        \
        LOG_CRITICAL(Debug, "Unreachable code!");                                                  \
        Common::Log::Stop();                                                                       \
        Crash();                                                                                   \
        exit(1);                                                                                   \
    }())

#define UNREACHABLE_MSG(...)                                                                       \
    ([&]() CITRA_NO_INLINE CITRA_NO_RETURN {                                                       \
        LOG_CRITICAL(Debug, "Unreachable code!\n" __VA_ARGS__);                                    \
        Common::Log::Stop();                                                                       \
        Crash();                                                                                   \
        exit(1);                                                                                   \
    }())

#ifdef _DEBUG
#define DEBUG_ASSERT(_a_) ASSERT(_a_)
#define DEBUG_ASSERT_MSG(_a_, ...) ASSERT_MSG(_a_, __VA_ARGS__)
#else // not debug
#define DEBUG_ASSERT(_a_)
#define DEBUG_ASSERT_MSG(_a_, _desc_, ...)
#endif

#ifdef CITRA_LINUX_GCC_BACKTRACE
#define UNIMPLEMENTED()                                                                            \
    ([]() CITRA_NO_INLINE CITRA_NO_RETURN {                                                        \
        LOG_CRITICAL(Debug, "Unimplemented code!");                                                \
        Common::Log::Stop();                                                                       \
        std::cerr << boost::stacktrace::stacktrace();                                              \
        Crash();                                                                                   \
        exit(1);                                                                                   \
    }())
#else
#define UNIMPLEMENTED() LOG_CRITICAL(Debug, "Unimplemented code!")
#endif
#define UNIMPLEMENTED_MSG(_a_, ...) LOG_CRITICAL(Debug, _a_, __VA_ARGS__)
