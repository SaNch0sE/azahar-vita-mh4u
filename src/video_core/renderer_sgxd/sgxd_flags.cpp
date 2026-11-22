// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>
#include "common/logging/log.h"
#include "video_core/renderer_sgxd/sgxd_flags.h"
#include "video_core/renderer_sgxd/vprof.h"

namespace GxmRenderer {

namespace {

#ifdef VITA_DIAGNOSTICS
const std::vector<std::string>& Flags() {
    static const std::vector<std::string> flags = [] {
        std::vector<std::string> out;
        std::string text;
        // The development build reads them from the host PC as well, so a run changes them without an
        // install.
        for (const char* path : {"ux0:data/azahar/sgxd_flags.txt", "host0:sgxd_flags.txt"}) {
            if (FILE* f = std::fopen(path, "rb")) {
                char buf[256];
                while (std::size_t n = std::fread(buf, 1, sizeof(buf), f)) {
                    text.append(buf, n);
                }
                text += ' ';
                std::fclose(f);
            }
        }
        {
            std::istringstream in(text);
            std::string word;
            while (in >> word) {
                out.push_back(word);
            }
            std::string joined;
            for (const auto& w : out) {
                joined += w + ' ';
            }
            LOG_WARNING(Render, "gxm_flags.txt: {}", joined);
        }
        return out;
    }();
    return flags;
}
#endif

} // Anonymous namespace

bool GxmFlag(const char* name) {
#ifdef VITA_DIAGNOSTICS
    for (const auto& flag : Flags()) {
        if (flag == name) {
            return true;
        }
    }
#else
    // The release build has no bring-up switches. Nothing reads the file, nothing reads the
    // environment, and every GxmFlag call site folds away to the default behaviour.
    (void)name;
#endif
    return false;
}

unsigned SgxdFlagValue(const char* name) {
#ifdef VITA_DIAGNOSTICS
    const std::string prefix = std::string(name) + '=';
    for (const auto& flag : Flags()) {
        if (flag.rfind(prefix, 0) == 0) {
            return static_cast<unsigned>(std::strtoul(flag.c_str() + prefix.size(), nullptr, 0));
        }
    }
#else
    (void)name;
#endif
    return 0;
}

std::vector<unsigned> SgxdFlagValues(const char* name) {
    std::vector<unsigned> values;
#ifdef VITA_DIAGNOSTICS
    const std::string prefix = std::string(name) + '=';
    for (const auto& flag : Flags()) {
        if (flag.rfind(prefix, 0) == 0) {
            values.push_back(
                static_cast<unsigned>(std::strtoul(flag.c_str() + prefix.size(), nullptr, 0)));
        }
    }
#else
    (void)name;
#endif
    return values;
}

#ifdef VITA_DIAGNOSTICS
namespace {
struct TraceEvent {
    unsigned long long us;
    const char* tag;
    unsigned arg;
    unsigned sys; // the kernel's system time, low 32 bits: sgxd's STAMP clock
};
constexpr std::size_t TraceCapacity = 65536;
TraceEvent trace_events[TraceCapacity];
std::atomic<std::size_t> trace_count{0};
std::atomic<bool> trace_written{false};
} // Anonymous namespace

void SgxdTrace(const char* tag, unsigned arg) {
    Vprof::Event(tag, arg);
    static const bool on = SgxdFlag("tracelog");
    if (!on || trace_written.load(std::memory_order_relaxed)) {
        return;
    }
    // `tracelog_frames`: the frame pacing's events only (the core's idle and timing events fill
    // the buffer in 5 s otherwise).
    static const bool frames_only = SgxdFlag("tracelog_frames");
    if (frames_only) {
        static constexpr std::string_view frame_tags[] = {
            "vblank",         "late",        "lead_wait",       "lead_wait_end",
            "present",        "presented",   "present_behind",  "present_pending",
            "flip",           "sgx_present", "sgx_present_end", "emu_ticket",
            "emu_ticket_end", "emu_space",   "emu_space_end",   "emu_sync",
            "emu_sync_end",   "hold",        "hold_parked",     "release"};
        if (std::find(std::begin(frame_tags), std::end(frame_tags), std::string_view{tag}) ==
            std::end(frame_tags)) {
            return;
        }
    }
    // The window: `tracelog_at` seconds after the first event (8 when not given), for
    // `tracelog_len` seconds (1).
    static const long long trace_from =
        (SgxdFlagValue("tracelog_at") != 0 ? SgxdFlagValue("tracelog_at") : 8) * 1'000'000ll;
    static const long long trace_to =
        trace_from +
        (SgxdFlagValue("tracelog_len") != 0 ? SgxdFlagValue("tracelog_len") : 1) * 1'000'000ll;
    using Clock = std::chrono::steady_clock;
    static const Clock::time_point start = Clock::now();
    const long long us =
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
    if (us < trace_from) {
        return;
    }
    if (us < trace_to) {
        const std::size_t i = trace_count.fetch_add(1, std::memory_order_relaxed);
        if (i < TraceCapacity) {
            trace_events[i] = {static_cast<unsigned long long>(us), tag, arg,
                               static_cast<unsigned>(sceKernelGetSystemTimeWide())};
        }
        return;
    }
    if (trace_written.exchange(true)) {
        return;
    }
    // The window has closed: events still being stored by another thread are at most a few.
    const std::size_t n = std::min(trace_count.load(), TraceCapacity);
    // Formatted here and written at once: a write over host0 is a round trip to the host PC.
    std::string text;
    text.reserve(n * 48);
    for (std::size_t i = 0; i < n; i++) {
        const TraceEvent& e = trace_events[i];
        char line[96];
        const int len = std::snprintf(line, sizeof(line), "%llu %s %u %u\n", e.us,
                                      e.tag ? e.tag : "?", e.arg, e.sys);
        text.append(line, static_cast<std::size_t>(std::max(len, 0)));
    }
    if (std::FILE* f = std::fopen("host0:azahar-trace.txt", "wb")) {
        std::setvbuf(f, nullptr, _IONBF, 0);
        std::fwrite(text.data(), 1, text.size(), f);
        std::fclose(f);
    }
    LOG_INFO(Render, "tracelog: {} events written to host0:azahar-trace.txt", n);
}
#endif

} // namespace GxmRenderer
