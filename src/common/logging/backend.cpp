// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <boost/algorithm/string/replace.hpp>
#include <boost/regex.hpp>

#include <fmt/format.h>

#ifdef _WIN32
#include <share.h>   // For _SH_DENYWR
#include <windows.h> // For OutputDebugStringW
#else
#define _SH_DENYWR 0
#endif

#ifdef __vita__
#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#endif

#ifdef CITRA_LINUX_GCC_BACKTRACE
#define BOOST_STACKTRACE_USE_BACKTRACE
#include <boost/stacktrace.hpp>
#undef BOOST_STACKTRACE_USE_BACKTRACE
#include <signal.h>
#endif

#include "common/common_paths.h"
#include "common/file_util.h"
#include "common/literals.h"
#include "common/logging/backend.h"
#include "common/logging/log.h"
#include "common/logging/log_entry.h"
#include "common/logging/text_formatter.h"
#include "common/mutex.h"
#include "common/settings.h"
#include "common/string_util.h"
#include "common/thread.h"

namespace Common::Log {

std::array<Level, static_cast<std::size_t>(Class::Count)> class_level{}; // all Trace

namespace {

/**
 * Interface for logging backends.
 */
class Backend {
public:
    virtual ~Backend() = default;

    virtual void Write(const Entry& entry) = 0;

    virtual void EnableForStacktrace() = 0;

    virtual void Flush() = 0;

    virtual void Close() = 0;
};

#ifdef HAVE_LIBRETRO
/**
 * LibRetro backend
 */
class LibRetroBackend : public Backend {
public:
    explicit LibRetroBackend() {}
    explicit LibRetroBackend(retro_log_printf_t callback) : callback(callback) {}

    ~LibRetroBackend() override = default;

    void Write(const Entry& entry) override {
        if (callback == nullptr) {
            return;
        }
        retro_log_level log_level;

        switch (entry.log_level) {
        case Common::Log::Level::Trace:
            log_level = retro_log_level::RETRO_LOG_DEBUG;
            break;
        case Common::Log::Level::Debug:
            log_level = retro_log_level::RETRO_LOG_DEBUG;
            break;
        case Common::Log::Level::Info:
            log_level = retro_log_level::RETRO_LOG_INFO;
            break;
        case Common::Log::Level::Warning:
            log_level = retro_log_level::RETRO_LOG_WARN;
            break;
        case Common::Log::Level::Error:
            log_level = retro_log_level::RETRO_LOG_ERROR;
            break;
        case Common::Log::Level::Critical:
            log_level = retro_log_level::RETRO_LOG_ERROR;
            break;
        default:
            log_level = retro_log_level::RETRO_LOG_DUMMY;
        }

        auto str = FormatLogMessage(entry).append(1, '\n');
        callback(log_level, str.c_str());
    }

    void Flush() override {}

    void Close() override {}

    void EnableForStacktrace() override {}

private:
    retro_log_printf_t callback = nullptr;
};
#endif

/**
 * Backend that writes to stderr and with color
 */
class ColorConsoleBackend final : public Backend {
public:
    explicit ColorConsoleBackend() = default;

    ~ColorConsoleBackend() override = default;

    void Write(const Entry& entry) override {
        if (enabled.load(std::memory_order_relaxed)) {
            PrintColoredMessage(entry);
        }
    }

    void Flush() override {
        std::fflush(stderr);
    }

    void Close() override {
        enabled = false;
    }

    void EnableForStacktrace() override {
        enabled = true;
    }

    void SetEnabled(bool enabled_) {
        enabled = enabled_;
    }

private:
    std::atomic_bool enabled{false};
};

/**
 * Backend that writes to a file passed into the constructor
 */
class FileBackend final : public Backend {
public:
    explicit FileBackend(const std::string& filename) {
        auto old_filename = filename;
        boost::replace_all(old_filename, ".txt", ".old.txt");

        // Existence checks are done within the functions themselves.
        // We don't particularly care if these succeed or not.
        static_cast<void>(FileUtil::Delete(old_filename));
        static_cast<void>(FileUtil::Rename(filename, old_filename));

        // _SH_DENYWR allows read only access to the file for other programs.
        // It is #defined to 0 on other platforms
        file = std::make_unique<FileUtil::IOFile>(filename, "w", _SH_DENYWR);
    }

    ~FileBackend() override = default;

    void Write(const Entry& entry) override {
        if (!enabled) {
            return;
        }

        bytes_written += file->WriteString(FormatLogMessage(entry).append(1, '\n'));

        using namespace Common::Literals;
        // Prevent logs from exceeding a set maximum size in the event that log entries are spammed.
        const auto write_limit = 100_MiB;
        const bool write_limit_exceeded = bytes_written > write_limit;
        if (entry.log_level >= Level::Error || write_limit_exceeded) {
            if (write_limit_exceeded) {
                // Stop writing after the write limit is exceeded.
                // Don't close the file so we can print a stacktrace if necessary
                enabled = false;
            }
            file->Flush();
        }
    }

    void Flush() override {
        file->Flush();
    }

    void Close() override {
        file->Close();
        enabled = false;
    }

    void EnableForStacktrace() override {
        enabled = true;
        bytes_written = 0;
    }

private:
    std::unique_ptr<FileUtil::IOFile> file;
    bool enabled = true;
    std::size_t bytes_written = 0;
};

/**
 * Backend that writes to Visual Studio's output window
 */
class DebuggerBackend final : public Backend {
public:
    explicit DebuggerBackend() = default;

    ~DebuggerBackend() override = default;

    void Write(const Entry& entry) override {
#ifdef _WIN32
        ::OutputDebugStringW(UTF8ToUTF16W(FormatLogMessage(entry).append(1, '\n')).c_str());
#endif
    }

    void Flush() override {}

    void Close() override {}

    void EnableForStacktrace() override {}
};

#ifdef __vita__
/**
 * Backend that writes to the PS Vita's debug channel.
 *
 * The only backend on that target. A file backend would write to the memory card through the
 * same emulator that is being debugged, and there is no console attached to write to - the
 * kernel's own channel, which a logger plugin picks up, reaches a developer either way and needs
 * nothing from the process beyond the call itself.
 */
class VitaBackend final : public Backend {
public:
    explicit VitaBackend() = default;

    ~VitaBackend() override = default;

    void Write(const Entry& entry) override {
        const std::string text = FormatLogMessage(entry).append(1, '\n');
#ifdef VITA_DIAGNOSTICS
        // perflog's lines go to ux0:data/azahar/perflog.txt, a block at a time: the debug
        // channel drops most of a second's hundred lines, and printing them takes the threads'
        // time. Under Impl's writing lock, as every Write is.
        if (text.find("perf: ") != std::string::npos ||
            text.find("EndOnePass") != std::string::npos) {
            static std::string pending;
            static bool started = false;
            pending += text;
            if (pending.size() >= 24 * 1024) {
                if (FILE* f = std::fopen("ux0:data/azahar/perflog.txt", started ? "ab" : "wb")) {
                    std::fwrite(pending.data(), 1, pending.size(), f);
                    std::fclose(f);
                }
                started = true;
                pending.clear();
            }
            return;
        }
#endif
#if defined(VITA_DEBUG_TOOLS) && !defined(VITA_DIAGNOSTICS)
        // The slow-frame recorder's reports also go to ux0:data/azahar/slowframes.txt, so a
        // slowdown met in ordinary play (no PC on the debug channel) is kept. Appended across
        // runs, started again past 4 MB; at most a frame's eight lines a second.
        if (text.find("slow frame ") != std::string::npos) {
            static FILE* file = [] {
                const char* path = "ux0:data/azahar/slowframes.txt";
                FILE* f = std::fopen(path, "ab");
                if (f != nullptr && std::fseek(f, 0, SEEK_END) == 0 &&
                    std::ftell(f) > 4 * 1024 * 1024) {
                    std::fclose(f);
                    f = std::fopen(path, "wb");
                }
                if (f != nullptr) {
                    std::fputs("--- run started\n", f);
                }
                return f;
            }();
            if (file != nullptr) {
                std::fwrite(text.data(), 1, text.size(), file);
                std::fflush(file);
            }
        }
#endif
        sceClibPrintf("%s", text.c_str());
        if (entry.log_level == Level::Critical) {
            AppendCrashReport(text);
        }
    }

    void Flush() override {}

    void Close() override {}

    void EnableForStacktrace() override {}
};
#endif

#ifdef ANDROID
/**
 * Backend that writes to the Android logcat
 */
class LogcatBackend : public Backend {
public:
    explicit LogcatBackend() = default;

    ~LogcatBackend() override = default;

    void Write(const Entry& entry) override {
        PrintMessageToLogcat(entry);
    }

    void Flush() override {}

    void Close() override {}

    void EnableForStacktrace() override {}
};
#endif

bool initialization_in_progress_suppress_logging = true;
bool logging_initialized = false;

#ifdef CITRA_LINUX_GCC_BACKTRACE
[[noreturn]] void SleepForever() {
    while (true) {
        pause();
    }
}
#endif

/**
 * Static state as a singleton.
 */
class Impl {
public:
    static Impl& Instance() {
        if (!instance) {
            throw std::runtime_error("Using Logging instance before its initialization");
        }
        return *instance;
    }
#ifdef HAVE_LIBRETRO
    static void Initialize(retro_log_printf_t callback) {
        if (instance) {
            LOG_WARNING(Log, "Reinitializing logging backend");
            return;
        }
        initialization_in_progress_suppress_logging = true;
        Filter filter;
        filter.ParseFilterString(Settings::values.log_filter.GetValue());
        instance = std::unique_ptr<Impl, decltype(&Deleter)>(new Impl(callback, filter), Deleter);
        class_level = filter.ClassLevels();
        initialization_in_progress_suppress_logging = false;
    }
#endif
    static void Initialize(std::string_view log_file) {
        if (instance) {
            LOG_WARNING(Log, "Reinitializing logging backend");
            return;
        }
        initialization_in_progress_suppress_logging = true;
        const auto& log_dir = FileUtil::GetUserPath(FileUtil::UserPath::LogDir);
#ifndef __vita__ // no log file there
        void(FileUtil::CreateFullPath(log_dir));
#endif
        Filter filter;
        filter.ParseFilterString(Settings::values.log_filter.GetValue());
        instance = std::unique_ptr<Impl, decltype(&Deleter)>(
            new Impl(fmt::format("{}{}", log_dir, log_file), filter), Deleter);
        class_level = filter.ClassLevels();
        initialization_in_progress_suppress_logging = false;
        logging_initialized = true;
    }

    static void Stop() {
        instance->CloseBackends();
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    void SetGlobalFilter(const Filter& f) {
        filter = f;
        class_level = f.ClassLevels();
    }

    bool SetRegexFilter(const std::string& regex) {
        if (regex.empty()) {
            regex_filter = boost::regex();
            return true;
        }
        regex_filter = boost::regex(regex, boost::regex_constants::no_except);
        if (regex_filter.status() != 0) {
            regex_filter = boost::regex();
            return false;
        }
        return true;
    }

    const Filter& GetFilter() const {
        return filter;
    }

    void SetColorConsoleBackendEnabled([[maybe_unused]] bool enabled) {
#ifndef __vita__
        color_console_backend.SetEnabled(enabled);
#endif
    }

    void PushEntry(Class log_class, Level log_level, const char* filename, unsigned int line_num,
                   const char* function, std::string message) {
        Entry new_entry = CreateEntry(log_class, log_level, filename, line_num, function,
                                      std::move(message), time_origin);
        if (!regex_filter.empty() &&
            !boost::regex_search(FormatLogMessage(new_entry), regex_filter)) {
            return;
        }
        // Written here, on the thread that logged it. The backends are not safe to enter from
        // two threads at once - the file backend counts the bytes it has written and the console
        // one puts a line out in several calls - so one lock covers the whole set of them.
        const bool flush = Settings::values.instant_debug_log.GetValue();
        std::scoped_lock lock{writing_mutex};
        ForEachBackend([&new_entry, flush](Backend& backend) {
            backend.Write(new_entry);
            if (flush) {
                backend.Flush();
            }
        });
    }

    static Entry CreateEntry(Class log_class, Level log_level, const char* filename,
                             unsigned int line_nr, const char* function, std::string&& message,
                             const std::chrono::steady_clock::time_point& time_origin) {
        using std::chrono::duration_cast;
        using std::chrono::microseconds;
        using std::chrono::steady_clock;

        return {
            .timestamp = duration_cast<microseconds>(steady_clock::now() - time_origin),
            .log_class = log_class,
            .log_level = log_level,
            .filename = filename,
            .line_num = line_nr,
            .function = function,
            .message = std::move(message),
        };
    }

private:
#ifdef HAVE_LIBRETRO
    Impl(retro_log_printf_t callback, const Filter& filter_)
        : filter{filter_}, file_backend{""}, libretro_backend{callback} {}
#endif
#ifdef __vita__
    Impl([[maybe_unused]] const std::string& file_backend_filename, const Filter& filter_)
        : filter{filter_} {}
#else
    Impl(const std::string& file_backend_filename, const Filter& filter_)
        : filter{filter_}, file_backend{file_backend_filename} {
#ifdef CITRA_LINUX_GCC_BACKTRACE
        int waker_pipefd[2];
        int done_printing_pipefd[2];
        if (pipe2(waker_pipefd, O_CLOEXEC) || pipe2(done_printing_pipefd, O_CLOEXEC)) {
            abort();
        }
        backtrace_thread_waker_fd = waker_pipefd[1];
        backtrace_done_printing_fd = done_printing_pipefd[0];
        std::thread([this, wait_fd = waker_pipefd[0], done_fd = done_printing_pipefd[1]] {
            Common::SetCurrentThreadName("citra:Crash");
            Common::SetCurrentThreadRole(Common::ThreadRole::Other);
            for (u8 ignore = 0; read(wait_fd, &ignore, 1) != 1;)
                ;
            const int sig = received_signal;
            if (sig <= 0) {
                abort();
            }
            // Deliberately without writing_mutex: the thread that faulted may hold it, and a
            // crash report that deadlocks is worth less than one whose lines interleave.
            const auto signal_entry = CreateEntry(
                Class::Log, Level::Critical, "?", 0, "?",
                fmt::vformat("Received signal {}", fmt::make_format_args(sig)), time_origin);
            ForEachBackend([&signal_entry](Backend& backend) {
                backend.EnableForStacktrace();
                backend.Write(signal_entry);
            });
            const auto backtrace =
                boost::stacktrace::stacktrace::from_dump(backtrace_storage.data(), 4096);
            for (const auto& frame : backtrace.as_vector()) {
                auto line = boost::stacktrace::detail::to_string(&frame, 1);
                if (line.empty()) {
                    abort();
                }
                line.pop_back(); // Remove newline
                const auto frame_entry = CreateEntry(Class::Log, Level::Critical, "?", 0, "?",
                                                     std::move(line), time_origin);
                ForEachBackend([&frame_entry](Backend& backend) { backend.Write(frame_entry); });
            }
            using namespace std::literals;
            const auto rip_entry =
                CreateEntry(Class::Log, Level::Critical, "?", 0, "?", "RIP"s, time_origin);
            ForEachBackend([&rip_entry](Backend& backend) {
                backend.Write(rip_entry);
                backend.Flush();
            });
            for (const u8 anything = 0; write(done_fd, &anything, 1) != 1;)
                ;
            // Abort on original thread to help debugging
            SleepForever();
        }).detach();
        signal(SIGSEGV, &HandleSignal);
        signal(SIGABRT, &HandleSignal);
#endif
    }
#endif // __vita__

    ~Impl() {
#ifdef CITRA_LINUX_GCC_BACKTRACE
        if (int zero_or_ignore = 0;
            !received_signal.compare_exchange_strong(zero_or_ignore, SIGKILL)) {
            SleepForever();
        }
#endif
    }

    void CloseBackends() {
        std::scoped_lock lock{writing_mutex};
        ForEachBackend([](Backend& backend) {
            backend.Flush();
            backend.Close();
        });
    }

    void ForEachBackend(auto lambda) {
#if defined(__vita__)
        lambda(static_cast<Backend&>(vita_backend));
#elif defined(HAVE_LIBRETRO)
        lambda(static_cast<Backend&>(libretro_backend));
#else
        lambda(static_cast<Backend&>(debugger_backend));
        lambda(static_cast<Backend&>(color_console_backend));
        lambda(static_cast<Backend&>(file_backend));
#ifdef ANDROID
        lambda(static_cast<Backend&>(lc_backend));
#endif // ANDROID
#endif
    }

    static void Deleter(Impl* ptr) {
        delete ptr;
    }

#ifdef CITRA_LINUX_GCC_BACKTRACE
    [[noreturn]] static void HandleSignal(int sig) {
        signal(SIGABRT, SIG_DFL);
        signal(SIGSEGV, SIG_DFL);
        if (sig <= 0) {
            abort();
        }
        instance->InstanceHandleSignal(sig);
    }

    [[noreturn]] void InstanceHandleSignal(int sig) {
        if (int zero_or_ignore = 0; !received_signal.compare_exchange_strong(zero_or_ignore, sig)) {
            if (received_signal == SIGKILL) {
                abort();
            }
            SleepForever();
        }
        // Don't restart like boost suggests. We want to append to the log file and not lose dynamic
        // symbols. This may segfault if it unwinds outside C/C++ code but we'll just have to fall
        // back to core dumps.
        boost::stacktrace::safe_dump_to(backtrace_storage.data(), 4096);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        for (const int anything = 0; write(backtrace_thread_waker_fd, &anything, 1) != 1;)
            ;
        for (u8 ignore = 0; read(backtrace_done_printing_fd, &ignore, 1) != 1;)
            ;
        abort();
    }
#endif

    static inline std::unique_ptr<Impl, decltype(&Deleter)> instance{nullptr, Deleter};

    Filter filter;
    boost::regex regex_filter;
#ifdef __vita__
    VitaBackend vita_backend{};
#else
    DebuggerBackend debugger_backend{};
    ColorConsoleBackend color_console_backend{};
    FileBackend file_backend;
#ifdef ANDROID
    LogcatBackend lc_backend{};
#endif
#ifdef HAVE_LIBRETRO
    LibRetroBackend libretro_backend;
#endif
#endif // __vita__

    Common::Mutex writing_mutex;
    std::chrono::steady_clock::time_point time_origin{std::chrono::steady_clock::now()};

#ifdef CITRA_LINUX_GCC_BACKTRACE
    std::atomic_int received_signal{0};
    std::array<u8, 4096> backtrace_storage{};
    int backtrace_thread_waker_fd;
    int backtrace_done_printing_fd;
#endif
};
} // namespace

#ifdef HAVE_LIBRETRO
void LibRetroStart(retro_log_printf_t callback) {
    Impl::Initialize(callback);
}
#endif

void Initialize(std::string_view log_file) {
    Impl::Initialize(log_file.empty() ? LOG_FILE : log_file);
}

#ifdef __vita__
namespace {
void (*stop_hook)() = nullptr;
void (*crash_hook)() = nullptr;
void (*report_preamble)() = nullptr;
std::atomic_flag report_begun = ATOMIC_FLAG_INIT;
/// When the preamble was last written (process time, us), 0 for none.
std::atomic<u64> report_preamble_us{0};
/// A preamble older than this is another event's (RenewCrashReport): a crash's own lines come
/// within seconds of each other.
constexpr u64 RenewAfterUs = 10'000'000;
}

void SetCrashReportPreamble(void (*hook)()) {
    report_preamble = hook;
}

void SetStopHook(void (*hook)()) {
    stop_hook = hook;
}

void SetCrashHook(void (*hook)()) {
    crash_hook = hook;
}

void Crashed() {
    if (crash_hook != nullptr) {
        crash_hook();
    }
    __builtin_trap();
}
#endif

void Stop() {
#ifdef __vita__
    if (stop_hook != nullptr) {
        stop_hook();
    }
#endif
    if (logging_initialized) {
        Impl::Stop();
    }
}

void DisableLoggingInTests() {
    initialization_in_progress_suppress_logging = true;
}

void SetGlobalFilter(const Filter& filter) {
    Impl::Instance().SetGlobalFilter(filter);
}

bool SetRegexFilter(const std::string& regex) {
    return Impl::Instance().SetRegexFilter(regex);
}

void SetColorConsoleBackendEnabled(bool enabled) {
    Impl::Instance().SetColorConsoleBackendEnabled(enabled);
}

void FmtLogMessageImpl(Class log_class, Level log_level, const char* filename,
                       unsigned int line_num, const char* function, fmt::string_view format,
                       const fmt::format_args& args) {
    if (initialization_in_progress_suppress_logging && log_level < Level::Critical) [[unlikely]] {
        return;
    }

    if (logging_initialized) [[likely]] {
        if (!Impl::Instance().GetFilter().CheckMessage(log_class, log_level)) {
            return;
        }
        Impl::Instance().PushEntry(log_class, log_level, filename, line_num, function,
                                   fmt::vformat(format, args));
    } else {
        // In the rare case that logging occurs before initialization, write the
        // message to stderr to preserve useful debug information.
        Entry new_entry = Impl::CreateEntry(log_class, log_level, filename, line_num, function,
                                            fmt::vformat(format, args), {});
        PrintMessage(new_entry);
    }
}
#ifdef __vita__
void AppendCrashReport(std::string_view text) {
    // The preamble's own lines come through here too, after the flag is set.
    if (report_preamble != nullptr && !report_begun.test_and_set()) {
        report_preamble_us.store(sceKernelGetProcessTimeWide(), std::memory_order_relaxed);
        report_preamble();
    }
    const SceUID fd = sceIoOpen(CrashReportPath, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    if (fd >= 0) {
        sceIoWrite(fd, text.data(), text.size());
        sceIoClose(fd);
    }
}

void RenewCrashReport() {
    const u64 at = report_preamble_us.load(std::memory_order_relaxed);
    if (at != 0 && sceKernelGetProcessTimeWide() - at > RenewAfterUs) {
        report_begun.clear();
    }
}

void EndCrashReport(bool remove) {
    if (remove) {
        sceIoRemove(CrashReportPath);
    }
    report_preamble_us.store(0, std::memory_order_relaxed);
    report_begun.clear();
}
#endif
} // namespace Common::Log
