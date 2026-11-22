// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// The process's own report of an abort. newlib's abort() ends in raise(), whose _kill_r stops the
// process on an undefined instruction, and the kernel's crash backtrace stops there: newlib's
// frames carry no unwind tables. The reason goes to stderr, which no console shows: an assert's
// text (newlib's own checks of a failed allocation among them, and imgui's), libstdc++'s
// "terminate called after throwing" in a build without our terminate handler. The link wraps
// raise, __assert_func and malloc with these (CMakeLists.txt): the text, the size of the last
// allocation refused and the return addresses found on the thread's stack go to the debug channel
// and to ux0:data/azahar/crash.txt, which the next start prints (PrintLast).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/threadmgr.h>

#include "citra_vita/config.h"
#include "citra_vita/crash_report.h"
#include "common/logging/backend.h"
#include "video_core/renderer_sgxd/sgxd_backend.h"

extern "C" {
// The text segment's ends (ldscript.ld).
extern const char __executable_start[];
extern const char etext[];
// The link's build ID note (ldscript.ld).
extern const char __build_id_start[];
extern const char __build_id_end[];

int __real_raise(int sig);
void __real___assert_func(const char* file, int line, const char* func, const char* expr);
void* __real_malloc(size_t size);
}

namespace {

constexpr const char* ReportPath = Common::Log::CrashReportPath;
constexpr char LastReportPath[] = "ux0:data/azahar/crash-last.txt";

char assert_text[256];
/// The size of the last allocation malloc refused (operator new's, before its std::bad_alloc).
size_t failed_malloc_size;
/// A report written: abort's raise after the terminate handler's adds none.
bool reported;

/// A word on the stack that a call left: inside the text segment, and the instruction before it
/// a BL or BLX. Other words that point into the code (function pointers, a vtable's) mostly fail
/// the second test.
bool IsReturnAddress(uintptr_t value) {
    const uintptr_t start = reinterpret_cast<uintptr_t>(__executable_start);
    const uintptr_t end = reinterpret_cast<uintptr_t>(etext);
    const uintptr_t at = value & ~uintptr_t{1};
    if (at < start + 4 || at > end) {
        return false;
    }
    if ((value & 1) != 0) {
        // Thumb: BL or BLX (immediate) is 11110... then 11x1... / 11x0...; BLX (register) is
        // 010001111.
        uint16_t first, second;
        std::memcpy(&first, reinterpret_cast<const void*>(at - 4), sizeof(first));
        std::memcpy(&second, reinterpret_cast<const void*>(at - 2), sizeof(second));
        return ((first & 0xF800) == 0xF000 && (second & 0xC000) == 0xC000) ||
               (second & 0xFF87) == 0x4780;
    }
    if ((value & 3) != 0) {
        return false;
    }
    uint32_t insn;
    std::memcpy(&insn, reinterpret_cast<const void*>(at - 4), sizeof(insn));
    return (insn & 0x0F000000) == 0x0B000000 || (insn & 0xFE000000) == 0xFA000000 ||
           (insn & 0x0FFFFFF0) == 0x012FFF30;
}

} // Anonymous namespace

namespace VitaFrontend::CrashReport {

namespace {

/// The calling thread's call chain as the unwinder walks it (the EHABI tables): none of the scan's
/// leftovers of older calls, though it ends at a C frame without a table (newlib, pthread-embedded)
/// and at a guest thunk, which the scan runs past. In a line of its own, after the scan's: walking
/// a damaged stack can fault, and what was written before it stays. The walk stops short of the
/// stack's ends for the same reason.
void WriteCallChain(uintptr_t stack_begin, uintptr_t stack_end) {
    constexpr uintptr_t Margin = 512; // more than a frame's unwinding pops at once
    constexpr int MaxFrames = 48;
    static struct {
        uintptr_t begin, end;
        int found, at;
        char text[640];
    } walk;
    walk.begin = stack_begin;
    walk.end = stack_end - Margin;
    walk.found = 0;
    walk.at = std::snprintf(walk.text, sizeof(walk.text), "[azahar] crash: call chain:");
    _Unwind_Backtrace(
        [](_Unwind_Context* context, void*) {
            const uintptr_t sp = _Unwind_GetGR(context, 13);
            if (walk.found == MaxFrames || sp < walk.begin || sp >= walk.end) {
                return _URC_END_OF_STACK;
            }
            walk.at += std::snprintf(walk.text + walk.at, sizeof(walk.text) - walk.at, "%s %08x",
                                     walk.found % 12 == 0 && walk.found != 0 ? "\n[azahar] crash:"
                                                                             : "",
                                     static_cast<unsigned>(_Unwind_GetIP(context)));
            walk.found++;
            return _URC_NO_REASON;
        },
        nullptr);
    std::snprintf(walk.text + walk.at, sizeof(walk.text) - walk.at, "\n");
    Write(walk.text);
}

} // Anonymous namespace

void WriteStackReport(const char* reason) {
    static char report[2048];
    reported = true;
    SceKernelThreadInfo info{};
    info.size = sizeof(info);
    const SceUID self = sceKernelGetThreadId();
    sceKernelGetThreadInfo(self, &info);
    uintptr_t sp;
    asm volatile("mov %0, sp" : "=r"(sp));
    const uintptr_t stack_end = reinterpret_cast<uintptr_t>(info.stack) + info.stackSize;
    int at = std::snprintf(report, sizeof(report),
                           "[azahar] crash: %s\n[azahar] crash: thread %08x \"%s\" (stack "
                           "%08x-%08x, sp %08x)",
                           reason, static_cast<unsigned>(self), info.name,
                           static_cast<unsigned>(reinterpret_cast<uintptr_t>(info.stack)),
                           static_cast<unsigned>(stack_end), static_cast<unsigned>(sp));
    if (failed_malloc_size != 0) {
        at += std::snprintf(report + at, sizeof(report) - at,
                            "; the last allocation refused: %u bytes",
                            static_cast<unsigned>(failed_malloc_size));
    }
    at += std::snprintf(report + at, sizeof(report) - at, "\n[azahar] crash: return addresses:");
    // From here to the stack's top, the first 96, innermost first (the live frames' among older
    // calls' leftovers): arm-vita-eabi-addr2line -f -e citra_vita.
    int found = 0;
    for (uintptr_t p = sp & ~uintptr_t{3}; p + 4 <= stack_end && found < 96; p += 4) {
        uintptr_t value;
        std::memcpy(&value, reinterpret_cast<const void*>(p), sizeof(value));
        if (IsReturnAddress(value) && at < static_cast<int>(sizeof(report)) - 32) {
            at += std::snprintf(report + at, sizeof(report) - at, "%s %08x",
                                found % 12 == 0 && found != 0 ? "\n[azahar] crash:" : "",
                                static_cast<unsigned>(value));
            found++;
        }
    }
    if (at < static_cast<int>(sizeof(report)) - 1) {
        report[at++] = '\n';
        report[at] = '\0';
    }
    Write(report);
    WriteCallChain(reinterpret_cast<uintptr_t>(info.stack),
                   reinterpret_cast<uintptr_t>(info.stack) + info.stackSize);
}

void NoteSystem(int version, int cores) {
    cores_unlocked = cores;
    native_version = version;
    system_length = 0;
    // taiHEN, HENkaku and the plugins its config.txt loads into the kernel.
    static char plugins[2048];
    const int listed =
        native_version > 0 ? azaharKernelPlugins(plugins, sizeof(plugins)) : AZAHAR_ERR_RESOLVE;
    if (listed < 0) {
        SystemLine("[azahar] system: kernel plugins not listed (%08x)\n",
                   static_cast<unsigned>(listed));
    } else {
        SystemLine("[azahar] system: %d kernel plugins:\n", listed);
        for (const char* line = plugins; *line != '\0';) {
            const char* const end = std::strchr(line, '\n');
            const int length = end != nullptr ? static_cast<int>(end - line)
                                              : static_cast<int>(std::strlen(line));
            char name[64], path[192];
            if (std::snprintf(name, sizeof(name), "%.*s", length, line) > 0 &&
                std::strchr(name, ' ') != nullptr) {
                const int name_length = static_cast<int>(std::strchr(name, ' ') - name);
                std::snprintf(path, sizeof(path), "%.*s", length - name_length - 1,
                              line + name_length + 1);
                name[name_length] = '\0';
                PluginLine(name, path);
            } else {
                SystemLine("[azahar] system:   %.*s\n", length, line);
            }
            line += length + (end != nullptr ? 1 : 0);
        }
    }
    // This process's modules from outside the firmware: the app's own and taiHEN's user plugins.
    static SceUID modules[128];
    SceSize count = std::size(modules);
    if (const int r = sceKernelGetModuleList(0xFF, modules, &count); r < 0) {
        SystemLine("[azahar] system: the app's modules not listed (%08x)\n",
                   static_cast<unsigned>(r));
    } else {
        SystemLine("[azahar] system: the app's modules:\n");
        for (SceSize i = 0; i < count && i < std::size(modules); i++) {
            static SceKernelModuleInfo info;
            std::memset(&info, 0, sizeof(info));
            info.size = sizeof(info);
            if (sceKernelGetModuleInfo(modules[i], &info) < 0) {
                continue;
            }
            info.module_name[sizeof(info.module_name) - 1] = '\0';
            info.path[sizeof(info.path) - 1] = '\0';
            if (FirmwarePath(info.path)) {
                continue;
            }
            // The app's own has its build ID, and it is the one large file here.
            if (std::string_view{info.path}.ends_with("eboot.bin")) {
                SystemLine("[azahar] system:   %s %s\n", info.module_name, info.path);
            } else {
                PluginLine(info.module_name, info.path);
            }
        }
    }
    ListUserPlugins();
    Common::Log::SetCrashReportPreamble(WritePreamble);
}

void NoteGpuDriver(u32 module_nid) {
    // sgxd's three (sgxd.c's SGXD_GPU_MODULE_NID*).
    firmware = module_nid == 0x4069F83Bu   ? "3.60"
               : module_nid == 0xC0F361A3u ? "3.65"
               : module_nid == 0xC01EF58Au ? "3.68"
                                           : "not one sgxd knows";
}

void NoteGame(const std::string& path, u64 program_id, const std::string& details) {
    std::snprintf(game_path, sizeof(game_path), "%s", path.c_str());
    std::snprintf(game_details, sizeof(game_details), "%s", details.c_str());
    game_program_id = program_id;
    game_started_us = sceKernelGetProcessTimeWide();
    if (path.empty()) {
        game_plugin[0] = '\0';
    } else {
        titles_started++;
    }
}

void NoteJump() {
    title_jumps++;
}

void NoteResume() {
    last_resume_us.store(sceKernelGetProcessTimeWide(), std::memory_order_relaxed);
    resumes.fetch_add(1, std::memory_order_relaxed);
}

void NoteStateLoaded(u32 slot) {
    loaded_slot = static_cast<s32>(slot);
    loaded_slot_us = sceKernelGetProcessTimeWide();
}

void NotePlugin(const std::string& plugin) {
    std::snprintf(game_plugin, sizeof(game_plugin), "%s", plugin.c_str());
}

void NoteCheats(const Cheats::CheatEngine* engine) {
    if (engine == nullptr) {
        cheats_text[0] = '\0';
        return;
    }
    std::string text = "[azahar] settings: cheats on:";
    std::size_t on = 0;
    for (const auto& cheat : engine->GetCheats()) {
        if (cheat->IsEnabled()) {
            text.append(on++ == 0 ? " " : ", ").append(cheat->GetName());
        }
    }
    if (on == 0) {
        text.append(" none");
    }
    std::snprintf(cheats_text, sizeof(cheats_text), "%s\n", text.c_str());
}

void Write(const char* text) {
    sceClibPrintf("%s", text);
    Common::Log::AppendCrashReport(text);
}

void PrintLast() {
    const SceUID fd = sceIoOpen(ReportPath, SCE_O_RDONLY, 0);
    if (fd < 0) {
        return;
    }
    char text[2048];
    const int n = sceIoRead(fd, text, sizeof(text) - 1);
    sceIoClose(fd);
    text[n > 0 ? n : 0] = '\0';
    // Each line marked as the last run's: a scripted launch's watch for a stop skips them. Not
    // the console's ring: this run's report would show the last one's again.
    console_quiet = true;
    sceClibPrintf("[azahar] the last run's crash report (%s):\n", LastReportPath);
    for (char* line = text; *line != '\0';) {
        char* const end = std::strchr(line, '\n');
        const int length = end != nullptr ? static_cast<int>(end - line)
                                          : static_cast<int>(std::strlen(line));
        sceClibPrintf("[azahar] last run: %.*s\n", length, line);
        line += length + (end != nullptr ? 1 : 0);
    }
    console_quiet = false;
    sceIoRemove(LastReportPath);
    sceIoRename(ReportPath, LastReportPath);
}

} // namespace VitaFrontend::CrashReport

extern "C" {

// Every sceClibPrintf of the app's, into the console's ring as well (the link wraps it,
// CMakeLists.txt). Sony's formatter, not newlib's, which allocates for some conversions: a crash's
// lines go through here too. Formatted twice; "%s" alone, as the kernel log's drain and the crash
// lines print, is recorded as it is.
int __wrap_sceClibPrintf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    if (!console_quiet.load(std::memory_order_relaxed)) {
        if (std::strcmp(format, "%s") == 0) {
            va_list copy;
            va_copy(copy, args);
            const char* const text = va_arg(copy, const char*);
            va_end(copy);
            if (text != nullptr) {
                ConsoleRecord(text);
            }
        } else {
            char line[512];
            va_list copy;
            va_copy(copy, args);
            const int n = sceClibVsnprintf(line, sizeof(line), format, copy);
            va_end(copy);
            if (n > 0) {
                ConsoleRecord(std::string_view{line, std::min<std::size_t>(n, sizeof(line) - 1)});
            }
        }
    }
    const int printed = sceClibVprintf(format, args);
    va_end(args);
    return printed;
}

// The reent the wrappers below give the allocator. newlib's malloc, calloc, realloc and free look
// up the calling thread's (__getreent, a costly TLS lookup through SceLibKernel) only to pass it
// on, and the allocator uses it for its lock,
// which ignores it (upcall_tls.cpp), and for errno after a failure, which lands in the global one.
#define MALLOC_REENT _global_impure_ptr

void __wrap___assert_func(const char* file, int line, const char* func, const char* expr) {
    std::snprintf(assert_text, sizeof(assert_text), "%s:%d %s: assertion \"%s\" failed",
                  file != nullptr ? file : "?", line, func != nullptr ? func : "?",
                  expr != nullptr ? expr : "?");
    __real___assert_func(file, line, func, expr);
}

void* __wrap_malloc(size_t size) {
    void* const p = __real__malloc_r(MALLOC_REENT, size);
    if (p == nullptr && size != 0) {
        failed_malloc_size = size;
    }
    return p;
}

int __wrap_raise(int sig) {
    if (!reported) {
        char reason[320];
        std::snprintf(reason, sizeof(reason), "raise(%d) from %08x%s%s", sig,
                      static_cast<unsigned>(
                          reinterpret_cast<uintptr_t>(__builtin_return_address(0))),
                      assert_text[0] != '\0' ? "; " : "", assert_text);
        VitaFrontend::CrashReport::WriteStackReport(reason);
    }
    return __real_raise(sig);
}

} // extern "C"
