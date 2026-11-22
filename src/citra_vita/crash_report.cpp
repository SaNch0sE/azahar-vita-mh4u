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
// and to ux0:data/azahar/crash.txt, which the next start prints (PrintLast). malloc, calloc,
// realloc and free keep the heap's large blocks with where they were allocated (PrintHeapSites).
// A crash the process finds itself then ends in Recover rather than the system's error dialog:
// its own core dump, and the app started again on its game list. A hardware fault (a data abort,
// an undefined instruction in the code) stays the system's.

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <string_view>

#include <psp2/appmgr.h>
#include <psp2/display.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/rtc.h>
#include <unwind.h>
#include <zlib.h>

#include <cryptopp/sha.h>
#include <fmt/format.h>

#include "audio_core/hle/hle.h"
#include "azahar_native.h"
#include "citra_vita/config.h"
#include "citra_vita/crash_report.h"
#include "common/assert.h"
#include "common/logging/backend.h"
#include "common/logging/log.h"
#include "common/named_thread.h"
#include "common/scm_rev.h"
#include "core/cheats/cheat_base.h"
#include "core/cheats/cheats.h"
#include "video_core/renderer_sgxd/guest_gpu_memory.h"
#include "video_core/renderer_sgxd/render_thread.h"
#include "video_core/renderer_sgxd/sgxd_backend.h"

extern "C" {
// The text segment's ends (ldscript.ld).
extern const char __executable_start[];
extern const char etext[];
// The link's build ID note (ldscript.ld).
extern const char __build_id_start[];
extern const char __build_id_end[];

void __real___assert_func(const char* file, int line, const char* func, const char* expr);
void* __real__malloc_r(struct _reent* r, size_t size);
void __real__free_r(struct _reent* r, void* p);
void __malloc_lock(struct _reent*);
void __malloc_unlock(struct _reent*);
// newlib's malloc (mallocr.c): where its arena starts, and its bins (the third is the top chunk).
extern char* __malloc_sbrk_base;
extern void* __malloc_av_[];
extern unsigned int _newlib_heap_size_user;
}

namespace {

constexpr const char* ReportPath = Common::Log::CrashReportPath;

char assert_text[256];
/// The size of the last allocation malloc refused (operator new's, before its std::bad_alloc).
size_t failed_malloc_size;
/// A report written: abort's raise after the terminate handler's adds none.
bool reported;

/// Recover's restart (EnableRecovery): its arguments, packed in restart_text, with room after
/// them for seq=N and crashed=PATH; none before EnableRecovery.
char restart_text[2048];
const char* restart_argv[40];
int restart_argc;
const unsigned long* restart_seq;
/// The stall watch held: a core dump's length, a recovery's.
std::atomic<bool> stall_held;
char system_text[12288];
std::size_t system_length;

void SystemLine(const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int n = std::vsnprintf(system_text + system_length,
                                 sizeof(system_text) - system_length, format, args);
    va_end(args);
    if (n > 0) {
        system_length = std::min(system_length + static_cast<std::size_t>(n),
                                 sizeof(system_text) - 1);
    }
}

/// The SHA-256 of a file, in hex; nothing a report needs is read for it later: these are the
/// plugins as they are when the app starts, hashed then. Its error when it cannot be read.
int HashFile(const char* path, char (&hex)[CryptoPP::SHA256::DIGESTSIZE * 2 + 1]) {
    const SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) {
        return fd;
    }
    static u8 buffer[64 * 1024];
    CryptoPP::SHA256 sha;
    int got;
    while ((got = sceIoRead(fd, buffer, sizeof(buffer))) > 0) {
        sha.Update(buffer, static_cast<std::size_t>(got));
    }
    sceIoClose(fd);
    if (got < 0) {
        return got;
    }
    u8 digest[CryptoPP::SHA256::DIGESTSIZE];
    sha.Final(digest);
    for (std::size_t i = 0; i < sizeof(digest); i++) {
        std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
    }
    return 0;
}

void PluginLine(const char* label, const char* path) {
    char hex[CryptoPP::SHA256::DIGESTSIZE * 2 + 1];
    if (const int r = HashFile(path, hex); r < 0) {
        SystemLine("[azahar] system:   %s %s (not read: %08x)\n", label, path,
                   static_cast<unsigned>(r));
    } else {
        SystemLine("[azahar] system:   %s %s sha256 %s\n", label, path, hex);
    }
}

/// The user plugins taiHEN's config.txt names for this app (*ALL and its title's sections) and the
/// shell (*main), as written there: sceKernelGetModuleList has only those in this process. taiHEN
/// reads ux0's file if there is one, else ur0's. Plugins for other titles are counted: they do not
/// run beside this app, and the *KERNEL section's are listed as loaded.
void ListUserPlugins() {
    const char* path = "ux0:tai/config.txt";
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) {
        path = "ur0:tai/config.txt";
        fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    }
    if (fd < 0) {
        SystemLine("[azahar] system: no taiHEN config.txt read (%08x)\n",
                   static_cast<unsigned>(fd));
        return;
    }
    static char config[32 * 1024];
    const int got = sceIoRead(fd, config, sizeof(config) - 1);
    sceIoClose(fd);
    if (got < 0) {
        SystemLine("[azahar] system: %s not read (%08x)\n", path, static_cast<unsigned>(got));
        return;
    }
    config[got] = '\0';
    SystemLine("[azahar] system: user plugins in %s for this app and the shell:\n", path);
    char section[32] = "";
    bool runs_here = false;
    unsigned other_titles = 0;
    for (char* line = config; line != nullptr && *line != '\0';) {
        char* const next = std::strchr(line, '\n');
        if (next != nullptr) {
            *next = '\0';
        }
        std::size_t length = std::strlen(line);
        while (length != 0 && (line[length - 1] == '\r' || line[length - 1] == ' ')) {
            line[--length] = '\0';
        }
        while (*line == ' ' || *line == '\t') {
            line++;
        }
        if (*line == '*') {
            std::snprintf(section, sizeof(section), "%s", line + 1);
            runs_here = std::strcmp(section, "ALL") == 0 || std::strcmp(section, "main") == 0 ||
                        std::strcmp(section, AZAHAR_VITA_TITLEID) == 0;
        } else if (*line != '\0' && *line != '#') {
            if (runs_here) {
                char label[40];
                std::snprintf(label, sizeof(label), "*%s", section);
                PluginLine(label, line);
            } else if (std::strcmp(section, "KERNEL") != 0) {
                other_titles++;
            }
        }
        line = next != nullptr ? next + 1 : nullptr;
    }
    if (other_titles != 0) {
        SystemLine("[azahar] system:   and %u for other titles\n", other_titles);
    }
}

bool FirmwarePath(const char* path) {
    return std::strncmp(path, "os0:", 4) == 0 || std::strncmp(path, "vs0:", 4) == 0;
}

char cheats_text[1024];

/// The header's facts, noted as they become known: how core 3 was unlocked and the module's
/// version (NoteSystem), the firmware by its GPU driver (NoteGpuDriver).
int cores_unlocked;
int native_version;
const char* firmware = "not known yet";
char game_path[256];
char game_details[384];
char game_plugin[384];
u64 game_program_id;
u64 game_started_us;
u32 titles_started;
u32 title_jumps;
std::atomic<u32> resumes;
std::atomic<u64> last_resume_us;
s32 loaded_slot = -1;
u64 loaded_slot_us;

/// The console's last text (every sceClibPrintf of the app's: its own lines and the kernel log it
/// drains), oldest overwritten, for the report: the app's TTY reaches nobody on a retail Vita.
/// Appended from any thread without a lock (a line written at the same moment as another can
/// interleave with it); the head is a running byte count.
constexpr std::size_t ConsoleRingSize = 16 * 1024;
static_assert((ConsoleRingSize & (ConsoleRingSize - 1)) == 0, "a power of two");
char console_ring[ConsoleRingSize];
std::atomic<std::size_t> console_head;
/// PrintLast's reprint of the last run's report stays out of it.
std::atomic<bool> console_quiet;

/// Whether a console line is left out of the ring: sgxd's line for each GPU mapping, hundreds as a
/// title starts, would push out everything else.
bool ConsoleNoise(std::string_view line) {
    return line.find("] map gpu ") != std::string_view::npos;
}

void ConsoleRecord(std::string_view text) {
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        const std::string_view line =
            text.substr(0, end == std::string_view::npos ? text.size() : end + 1);
        text.remove_prefix(line.size());
        if (ConsoleNoise(line)) {
            continue;
        }
        const std::size_t at = console_head.fetch_add(line.size(), std::memory_order_relaxed);
        for (std::size_t i = 0; i < line.size(); i++) {
            console_ring[(at + i) & (ConsoleRingSize - 1)] = line[i];
        }
    }
}

/// The link's build ID as hex, which names the one ELF that resolves this build's addresses: a
/// commit does not, a `-dirty` build's least of all. Its note: the sizes of its name and ID, its
/// type, the name to four bytes, the ID.
const char* BuildId() {
    static char text[41];
    const auto* const note = reinterpret_cast<const u32*>(__build_id_start);
    const auto* const end = reinterpret_cast<const u8*>(__build_id_end);
    if (__build_id_end - __build_id_start < 16) {
        return "none";
    }
    const u32 id_size = note[1];
    const u8* const id = reinterpret_cast<const u8*>(note + 3) + ((note[0] + 3) & ~3u);
    if (id + id_size > end || id_size * 2 >= sizeof(text)) {
        return "none";
    }
    for (u32 i = 0; i < id_size; i++) {
        std::snprintf(text + i * 2, 3, "%02x", id[i]);
    }
    return text;
}

/// The report's first lines (Common::Log's preamble): the build and the console, what the console
/// ran, the game and the settings it ran with, then the console's last text. Allocates nothing.
void WritePreamble() {
    // The kernel log since the app's last drain (sgxd's and the module's lines) into the ring,
    // read here rather than through SgxdDrainKernelLog, whose sceClibPrintf reaches the debug
    // channel: the stall watch's report keeps off it (a debug link that stopped holds a print
    // for good), and the watch's first line brings this preamble.
    if (native_version > 0) {
        static char klog[0x1000];
        for (int round = 0; round < 32; round++) {
            const int n = azaharKlogRead(klog, sizeof(klog) - 1);
            if (n <= 0) {
                break;
            }
            klog[n] = '\0';
            ConsoleRecord(klog);
        }
    }
    static char text[ConsoleRingSize + 8192];
    std::size_t at = 0;
    const auto add = [&](const char* format, auto... args) {
        if (at + 1 < sizeof(text)) {
            const int n = std::snprintf(text + at, sizeof(text) - at, format, args...);
            at = n > 0 ? std::min(at + static_cast<std::size_t>(n), sizeof(text) - 1) : at;
        }
    };
    const u64 now_us = sceKernelGetProcessTimeWide();
    add("[azahar] report: Azahar %s (%.10s), build %s, azaharnative %d, %s, firmware %s, core 3 "
        "%s; at %u.%01u s\n",
        Common::g_scm_desc, Common::g_scm_rev, BuildId(), native_version,
        sceKernelIsPSVitaTV() ? "PS TV" : "PS Vita", firmware,
        cores_unlocked == AZAHAR_CORES_CAPUNLOCKER ? "CapUnlocker's"
        : cores_unlocked == AZAHAR_CORES_OWN       ? "azaharnative's"
                                                   : "not unlocked",
        static_cast<unsigned>(now_us / 1000000), static_cast<unsigned>(now_us / 100000 % 10));
    Common::Log::AppendCrashReport(std::string_view{text, at});
    if (system_length != 0) {
        Common::Log::AppendCrashReport(std::string_view{system_text, system_length});
    }
    at = 0;
    if (game_path[0] != '\0') {
        const u64 ran_us = now_us - game_started_us;
        add("[azahar] game: %s, program %016llx, running %u.%01u s\n", game_path,
            static_cast<unsigned long long>(game_program_id),
            static_cast<unsigned>(ran_us / 1000000), static_cast<unsigned>(ran_us / 100000 % 10));
        add("[azahar] game: %s\n", game_details);
        if (game_plugin[0] != '\0') {
            add("[azahar] game: plugin %s\n", game_plugin);
        }
    }
    add("[azahar] session: titles started: %u, by a jump: %u; ", titles_started, title_jumps);
    const auto ago = [&](u64 at_us, unsigned& seconds, unsigned& tenths) {
        seconds = static_cast<unsigned>((now_us - at_us) / 1000000);
        tenths = static_cast<unsigned>((now_us - at_us) / 100000 % 10);
    };
    unsigned seconds, tenths;
    const u32 resumed = resumes.load(std::memory_order_relaxed);
    if (resumed != 0) {
        ago(last_resume_us.load(std::memory_order_relaxed), seconds, tenths);
        add("resumes from standby: %u, the last %u.%01u s ago; ", resumed, seconds, tenths);
    } else {
        add("%s", "resumes from standby: 0; ");
    }
    if (loaded_slot >= 0) {
        ago(loaded_slot_us, seconds, tenths);
        add("slot %d loaded %u.%01u s ago\n", static_cast<int>(loaded_slot), seconds, tenths);
    } else {
        add("%s", "no savestate loaded\n");
    }
    at += VitaFrontend::Config::WriteApplied(text + at, sizeof(text) - at);
    Common::Log::AppendCrashReport(std::string_view{text, at});
    Common::Log::AppendCrashReport(cheats_text);
    at = 0;
    const std::size_t head = console_head.load(std::memory_order_relaxed);
    const std::size_t from = head > ConsoleRingSize ? head - ConsoleRingSize : 0;
    add("[azahar] console: its last %u bytes before this report:\n",
        static_cast<unsigned>(head - from));
    // The newest whole lines that fit with their prefixes, found back from the head: lines
    // shorter than the prefix take more room than the ring holds, and the last ones before the
    // crash are the ones to keep. A ring that has wrapped starts inside a line: never that one.
    constexpr std::size_t Prefix = sizeof("[azahar] console: ") - 1;
    const std::size_t room = sizeof(text) - at - 32;
    std::size_t first = head;
    std::size_t used = 0;
    std::size_t line_end = head;
    std::size_t i = head;
    for (; i > from; i--) {
        if (console_ring[(i - 1) & (ConsoleRingSize - 1)] == '\n' && i != head) {
            if (used + (line_end - i) + Prefix > room) {
                break;
            }
            used += line_end - i + Prefix;
            first = i;
            line_end = i;
        }
    }
    if (i == 0 && used + line_end + Prefix <= room) {
        first = 0; // the ring has not wrapped: its first line is whole
    }
    bool line_start = true;
    for (std::size_t k = first; k < head && at + 32 < sizeof(text); k++) {
        const char c = console_ring[k & (ConsoleRingSize - 1)];
        if (line_start) {
            add("%s", "[azahar] console: ");
        }
        text[at++] = c;
        line_start = c == '\n';
    }
    if (at != 0 && text[at - 1] != '\n') {
        text[at++] = '\n';
    }
    Common::Log::AppendCrashReport(std::string_view{text, at});
}

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

/// Where a scan for return addresses from `sp` stops: the thread's stack's top, at most `limit`
/// bytes on; an upcall's SP (its own stack, not the thread's) gets 16 KiB.
uintptr_t StackScanEnd(const SceKernelThreadInfo& info, uintptr_t sp, uintptr_t limit) {
    const uintptr_t begin = reinterpret_cast<uintptr_t>(info.stack);
    const uintptr_t end = begin + info.stackSize;
    if (sp < begin || sp >= end) {
        return sp + 0x4000;
    }
    return std::min(end, sp + limit);
}

/// The heap's large blocks, live: their size and the return addresses on the allocating thread's
/// stack (heapsites, and a crash's report). Kept under malloc's own lock.
constexpr size_t SiteThreshold = 512 * 1024;
struct Site {
    const void* p;
    uint32_t size;
    uint32_t ra[10];
};
Site sites[96];
uint32_t sites_dropped;

/// A chunk's size from newlib's header before it (mallocr.c).
size_t ChunkSize(const void* p) {
    return *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(p) - 4) & ~3u;
}

void TrackSite(const void* p, size_t size) {
    // The scan starts past these locals: the site it fills and the thread's entry point (a code
    // address) would be found on the stack too.
    struct {
        SceKernelThreadInfo info;
        Site site;
    } l{};
    l.site = {p, static_cast<uint32_t>(size), {}};
    l.info.size = sizeof(l.info);
    sceKernelGetThreadInfo(sceKernelGetThreadId(), &l.info);
    const uintptr_t from = reinterpret_cast<uintptr_t>(&l + 1);
    const uintptr_t end = StackScanEnd(l.info, from, 0x8000);
    uint32_t n = 0;
    for (uintptr_t a = (from + 3) & ~uintptr_t{3}; a + 4 <= end && n < 10; a += 4) {
        uintptr_t value;
        std::memcpy(&value, reinterpret_cast<const void*>(a), sizeof(value));
        if (IsReturnAddress(value)) {
            l.site.ra[n++] = static_cast<uint32_t>(value);
        }
    }
    __malloc_lock(_REENT);
    Site* const slot = std::find_if(std::begin(sites), std::end(sites),
                                    [](const Site& s) { return s.p == nullptr; });
    if (slot != std::end(sites)) {
        *slot = l.site;
    } else {
        sites_dropped++;
    }
    __malloc_unlock(_REENT);
}

void UntrackSite(const void* p) {
    __malloc_lock(_REENT);
    for (Site& s : sites) {
        if (s.p == p) {
            s.p = nullptr;
            break;
        }
    }
    __malloc_unlock(_REENT);
}

#ifdef VITA_DIAGNOSTICS
/// `leaktrack`: every heap block's allocation site from then on, to name what a quit leaves behind
/// (`leaks`). The sites are call chains (unwound, so slow: a diagnostic), each kept once; every 8
/// bytes of the heap hold the site of the block that starts there. The tables are a memory block of
/// their own, outside the heap they watch. Under malloc's own lock.
constexpr uint32_t LeakDepth = 10;
constexpr uint32_t LeakSites = 16384;
constexpr uint32_t LeakBuckets = 32768;
struct LeakSite {
    uint32_t ra[LeakDepth];
};
struct LeakTables {
    uint16_t buckets[LeakBuckets];
    LeakSite sites[LeakSites];
    uint32_t counts[LeakSites];
    uint32_t bytes[LeakSites];
    uint16_t ids[1]; ///< one per 8 bytes of the heap
};
LeakTables* leak;
uintptr_t leak_base, leak_end;
uint32_t leak_sites_used = 1;
uint32_t leak_full;

struct StackBounds {
    SceUID thread;
    uintptr_t lo, hi;
};
StackBounds stack_bounds[64];
uint32_t stack_bounds_next;

/// Where a scan from `from` (on the calling thread's stack) stops: 1 KiB on, within the stack; an
/// upcall's SP (its own stack) gets 512 bytes.
uintptr_t LeakScanEnd(uintptr_t from) {
    const SceUID self = sceKernelGetThreadId();
    for (const StackBounds& b : stack_bounds) {
        if (b.thread == self && from >= b.lo && from < b.hi) {
            return std::min(b.hi, from + 1024);
        }
    }
    SceKernelThreadInfo info{};
    info.size = sizeof(info);
    if (sceKernelGetThreadInfo(self, &info) >= 0) {
        const uintptr_t lo = reinterpret_cast<uintptr_t>(info.stack);
        const uintptr_t hi = lo + info.stackSize;
        stack_bounds[stack_bounds_next++ % std::size(stack_bounds)] = {self, lo, hi};
        if (from >= lo && from < hi) {
            return std::min(hi, from + 1024);
        }
    }
    return from + 512;
}

void LeakRecord(const void* p) {
    const uintptr_t at = reinterpret_cast<uintptr_t>(p);
    if (at < leak_base || at >= leak_end) {
        return;
    }
    // The chain as the unwinder walks it (the EHABI tables: exact, through every C++ frame);
    // where it stops early (a C frame without a table: newlib, pthread-embedded), the rest from
    // the stack's scan, after a 1 (the scan starts past this local, which it fills).
    struct {
        LeakSite site;
        uint32_t n;
    } l{};
    __malloc_lock(_REENT);
    _Unwind_Backtrace(
        [](_Unwind_Context* context, void* arg) {
            auto* const out = static_cast<decltype(l)*>(arg);
            if (out->n == LeakDepth) {
                return _URC_END_OF_STACK;
            }
            out->site.ra[out->n++] = static_cast<uint32_t>(_Unwind_GetIP(context));
            return _URC_NO_REASON;
        },
        &l);
    uint32_t n = l.n;
    if (n < 4) {
        l.site.ra[n++] = 1;
        const uintptr_t from = reinterpret_cast<uintptr_t>(&l + 1);
        const uintptr_t end = LeakScanEnd(from);
        for (uintptr_t a = (from + 3) & ~uintptr_t{3}; a + 4 <= end && n < LeakDepth; a += 4) {
            uintptr_t value;
            std::memcpy(&value, reinterpret_cast<const void*>(a), sizeof(value));
            if (IsReturnAddress(value)) {
                l.site.ra[n++] = static_cast<uint32_t>(value);
            }
        }
    }
    uint32_t hash = 2166136261u;
    for (const uint32_t ra : l.site.ra) {
        hash = (hash ^ ra) * 16777619u;
    }
    uint16_t id = 0;
    for (uint32_t probe = 0; probe < 64; probe++) {
        uint16_t& bucket = leak->buckets[(hash + probe) % LeakBuckets];
        if (bucket == 0) {
            if (leak_sites_used < LeakSites) {
                id = static_cast<uint16_t>(leak_sites_used++);
                leak->sites[id] = l.site;
                bucket = id;
            }
            break;
        }
        if (std::memcmp(&leak->sites[bucket], &l.site, sizeof(l.site)) == 0) {
            id = bucket;
            break;
        }
    }
    leak_full += id == 0 ? 1 : 0;
    leak->ids[(at - leak_base) >> 3] = id;
    __malloc_unlock(_REENT);
}

/// A block freed: its site goes, so that one the C library allocates there itself (its own calls
/// pass the wrap by) shows as unrecorded, not as the block before it.
void LeakForget(const void* p) {
    const uintptr_t at = reinterpret_cast<uintptr_t>(p);
    if (at >= leak_base && at < leak_end) {
        leak->ids[(at - leak_base) >> 3] = 0;
    }
}
#endif

} // Anonymous namespace

namespace VitaFrontend::CrashReport {

#ifdef VITA_DIAGNOSTICS
void StartLeakTracking() {
    if (leak != nullptr) {
        sceClibPrintf("[azahar] leaktrack: on since before\n");
        return;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(__malloc_sbrk_base);
    const size_t size = offsetof(LeakTables, ids) + (_newlib_heap_size_user >> 3) * 2;
    const size_t block = (size + 0xFFFFF) & ~size_t{0xFFFFF};
    const SceUID uid = sceKernelAllocMemBlock("azahar-leaktrack", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
                                              static_cast<SceSize>(block), nullptr);
    void* tables = nullptr;
    if (uid < 0 || sceKernelGetMemBlockBase(uid, &tables) < 0) {
        sceClibPrintf("[azahar] leaktrack: no %u KiB block (%08x)\n",
                      static_cast<unsigned>(block >> 10), static_cast<unsigned>(uid));
        return;
    }
    std::memset(tables, 0, block);
    __malloc_lock(_REENT);
    leak_base = base;
    leak_end = base + _newlib_heap_size_user;
    leak = static_cast<LeakTables*>(tables);
    __malloc_unlock(_REENT);
    sceClibPrintf("[azahar] leaktrack: on, %u KiB of tables\n", static_cast<unsigned>(block >> 10));
}

void PrintLeaks(bool every_chunk) {
    if (leak == nullptr) {
        sceClibPrintf("[azahar] leaks: leaktrack is off\n");
        return;
    }
    __malloc_lock(_REENT);
    std::memset(leak->counts, 0, sizeof(leak->counts));
    std::memset(leak->bytes, 0, sizeof(leak->bytes));
    const uintptr_t base = (leak_base + 7) & ~uintptr_t{7};
    const uintptr_t top = reinterpret_cast<uintptr_t>(__malloc_av_[2]);
    uint32_t chunks = 0;
    for (uintptr_t p = base; p < top;) {
        const uint32_t size = *reinterpret_cast<const uint32_t*>(p + 4) & ~3u;
        if (size < 16 || p + size > top) {
            break;
        }
        if ((*reinterpret_cast<const uint32_t*>(p + size + 4) & 1u) != 0) {
            const uint16_t id = leak->ids[(p + 8 - leak_base) >> 3];
            leak->counts[id]++;
            leak->bytes[id] += size;
            chunks++;
            if (every_chunk && id != 0) {
                const uint32_t* const words = reinterpret_cast<const uint32_t*>(p + 8);
                const LeakSite& site = leak->sites[id];
                sceClibPrintf("[azahar] leaks: +%08x %u bytes: %08x %08x site %u: %08x %08x %08x "
                              "%08x %08x %08x %08x %08x %08x %08x\n",
                              static_cast<unsigned>(p - base), size, words[0], words[1], id,
                              site.ra[0], site.ra[1], site.ra[2], site.ra[3], site.ra[4],
                              site.ra[5], site.ra[6], site.ra[7], site.ra[8], site.ra[9]);
            }
        }
        p += size;
    }
    const uint32_t used = leak_sites_used;
    __malloc_unlock(_REENT);
    sceClibPrintf("[azahar] leaks: %u chunks in use, %u of them unrecorded (from before leaktrack, or "
                  "the C library's own: %u KiB); "
                  "%u sites, %u allocations unrecorded\n",
                  chunks, leak->counts[0], leak->bytes[0] >> 10, used - 1, leak_full);
    for (uint32_t shown = 0; shown < 48; shown++) {
        uint32_t best = 0;
        for (uint32_t id = 1; id < used; id++) {
            if (leak->counts[id] > leak->counts[best] || best == 0) {
                best = leak->counts[id] != 0 ? id : best;
            }
        }
        if (best == 0 || leak->counts[best] == 0) {
            break;
        }
        const LeakSite& site = leak->sites[best];
        sceClibPrintf("[azahar] leaks: %u chunks, %u bytes: %08x %08x %08x %08x %08x %08x %08x "
                      "%08x %08x %08x\n",
                      leak->counts[best], leak->bytes[best], site.ra[0], site.ra[1], site.ra[2],
                      site.ra[3], site.ra[4], site.ra[5], site.ra[6], site.ra[7], site.ra[8],
                      site.ra[9]);
        leak->counts[best] = 0;
    }
}
#endif

void PrintHeapSites(bool to_report) {
    static Site copy[std::size(sites)];
    __malloc_lock(_REENT);
    std::memcpy(copy, sites, sizeof(sites));
    const uint32_t dropped = sites_dropped;
    __malloc_unlock(_REENT);
    uint32_t count = 0, total = 0;
    for (const Site& s : copy) {
        if (s.p != nullptr) {
            count++;
            total += s.size;
        }
    }
    char line[200];
    std::snprintf(line, sizeof(line),
                  "[azahar] heapsites: %u blocks of 512 KiB or more, %u KiB (%u not kept)\n",
                  count, total >> 10, dropped);
    to_report ? Write(line) : static_cast<void>(sceClibPrintf("%s", line));
    const uintptr_t base = reinterpret_cast<uintptr_t>(__malloc_sbrk_base);
    for (const Site& s : copy) {
        if (s.p == nullptr) {
            continue;
        }
        int at = std::snprintf(line, sizeof(line), "[azahar] heapsites: %u KiB at +%08x:",
                               s.size >> 10,
                               static_cast<unsigned>(reinterpret_cast<uintptr_t>(s.p) - base));
        for (const uint32_t ra : s.ra) {
            if (ra != 0 && at < static_cast<int>(sizeof(line)) - 12) {
                at += std::snprintf(line + at, sizeof(line) - at, " %08x", ra);
            }
        }
        std::snprintf(line + at, sizeof(line) - at, "\n");
        to_report ? Write(line) : static_cast<void>(sceClibPrintf("%s", line));
    }
}

void BlankDisplay() {
    sceDisplaySetFrameBuf(nullptr, SCE_DISPLAY_SETBUF_NEXTFRAME);
}

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
    BlankDisplay();
    static char report[2048];
    static SceKernelThreadInfo info;
    reported = true;
    info.size = sizeof(info);
    const SceUID self = sceKernelGetThreadId();
    sceKernelGetThreadInfo(self, &info);
    uintptr_t sp;
    asm volatile("mov %0, sp" : "=r"(sp));
    const uintptr_t stack_end = StackScanEnd(info, sp, 0x40000);
    int at = std::snprintf(report, sizeof(report),
                           "[azahar] crash: %s\n[azahar] crash: thread %08x \"%s\" (stack "
                           "%08x-%08x, sp %08x)",
                           reason, static_cast<unsigned>(self), info.name,
                           static_cast<unsigned>(reinterpret_cast<uintptr_t>(info.stack)),
                           static_cast<unsigned>(reinterpret_cast<uintptr_t>(info.stack) +
                                                 info.stackSize),
                           static_cast<unsigned>(sp));
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
    if (failed_malloc_size != 0) {
        PrintHeapSites(true);
    }
}

namespace {

// SceLibKernel's, not in vitasdk's headers (the layouts are the SDK's).
struct LwMutexInfo {
    SceSize size;
    SceUID uid;
    char name[32];
    SceUInt32 attr;
    void* work;
    SceInt32 init_count;
    SceInt32 current_count;
    SceUID current_owner;
    SceUInt32 waiters;
};
struct LwCondInfo {
    SceSize size;
    SceUID uid;
    char name[32];
    SceUInt32 attr;
    void* work;
    void* lw_mutex;
    SceUInt32 waiters;
};
extern "C" int sceKernelGetLwMutexInfoById(SceUID id, LwMutexInfo* info);
extern "C" int sceKernelGetLwCondInfoById(SceUID id, LwCondInfo* info);
extern "C" int sceKernelGetCondInfo(SceUID id, SceKernelCondInfo* info);

/// Where the stall listing goes: the debug channel (`hostthreads`), or the crash report on the
/// memory card only (the stall watch: the debug link may be what stopped, and a print
/// would then wait on it for good, short of the fault after it).
bool stall_to_report;

void StallLine(const char* format, ...) {
    char line[320];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (stall_to_report) {
        Common::Log::AppendCrashReport(line);
    } else {
        sceClibPrintf("%s", line);
    }
}

void ThreadName(SceUID id, char (&name)[32]) {
    SceKernelThreadInfo info{};
    info.size = sizeof(info);
    std::strcpy(name, "?");
    if (sceKernelGetThreadInfo(id, &info) >= 0) {
        std::memcpy(name, info.name, sizeof(name));
        name[sizeof(name) - 1] = '\0';
    }
}

/// What a thread waits on (its wait type, Sony's SDK's values): the object's kind and name, and
/// for a lock its owner, for a thread's end that thread.
void DescribeWait(const SceKernelThreadInfo& t, char* out, size_t size) {
    const SceUInt32 type = t.waitType & 0x7FFFFFFFu;
    const SceUID id = t.waitId;
    const char* kind = "?";
    char name[32] = "";
    SceUID owner = 0;
    // Only this process's objects are asked about: their IDs carry 0x40000000. Asking about a
    // system object (SceAudio's event flag, 0x00010b81) is a critical usage error that stops the
    // process.
    const bool ours = (static_cast<SceUInt32>(id) & 0x40000000u) != 0;
    switch (type == 0 || type == 0x1 || ours ? type : 0xFFFFFFFFu) {
    case 0xFFFFFFFFu:
        std::snprintf(out, size, "a system object %08x (type %08x)", static_cast<unsigned>(id),
                      static_cast<unsigned>(t.waitType));
        return;
    case 0:
        std::snprintf(out, size, "no wait");
        return;
    case 0x1:
        std::snprintf(out, size, "a delay");
        return;
    case 0x2:
        kind = "the end of thread";
        ThreadName(id, name);
        break;
    case 0x10: {
        kind = "event flag";
        SceKernelEventFlagInfo info{};
        info.size = sizeof(info);
        if (sceKernelGetEventFlagInfo(id, &info) >= 0) {
            std::memcpy(name, info.name, sizeof(name));
        }
        break;
    }
    case 0x20: {
        kind = "semaphore";
        SceKernelSemaInfo info{};
        info.size = sizeof(info);
        if (sceKernelGetSemaInfo(id, &info) >= 0) {
            std::memcpy(name, info.name, sizeof(name));
        }
        break;
    }
    case 0x40: {
        kind = "mutex";
        SceKernelMutexInfo info{};
        info.size = sizeof(info);
        if (sceKernelGetMutexInfo(id, &info) >= 0) {
            std::memcpy(name, info.name, sizeof(name));
            owner = info.currentOwnerId;
        }
        break;
    }
    case 0x100:
    case 0x200: {
        kind = "condition";
        SceKernelCondInfo info{};
        info.size = sizeof(info);
        if (sceKernelGetCondInfo(id, &info) >= 0) {
            std::memcpy(name, info.name, sizeof(name));
        }
        break;
    }
    case 0x100000: {
        kind = "lightweight mutex";
        LwMutexInfo info{};
        info.size = sizeof(info);
        if (sceKernelGetLwMutexInfoById(id, &info) >= 0) {
            std::memcpy(name, info.name, sizeof(name));
            owner = info.current_owner;
        }
        break;
    }
    case 0x200000:
    case 0x400000: {
        kind = "lightweight condition";
        LwCondInfo info{};
        info.size = sizeof(info);
        if (sceKernelGetLwCondInfoById(id, &info) >= 0) {
            std::memcpy(name, info.name, sizeof(name));
        }
        break;
    }
    case 0x10000:
    case 0x20000:
        kind = "event";
        break;
    case 0x40000:
        kind = "message pipe";
        break;
    default:
        break;
    }
    name[sizeof(name) - 1] = '\0';
    int at = std::snprintf(out, size, "%s %08x \"%s\" (type %08x)", kind,
                           static_cast<unsigned>(id), name, static_cast<unsigned>(t.waitType));
    if (owner > 0 && (static_cast<SceUInt32>(owner) & 0x40000000u) != 0 && at > 0 &&
        static_cast<size_t>(at) < size) {
        char owner_name[32];
        ThreadName(owner, owner_name);
        std::snprintf(out + at, size - at, ", held by %08x \"%s\"", static_cast<unsigned>(owner),
                      owner_name);
    }
}

/// One thread's state, its wait and the return addresses on its stack from the top down (the
/// outermost frames first; past its current depth, which the report cannot see, are older calls'
/// leftovers): arm-vita-eabi-addr2line -f -e citra_vita.
void PrintThreadState(SceUID id) {
    SceKernelThreadInfo t{};
    t.size = sizeof(t);
    if (sceKernelGetThreadInfo(id, &t) < 0) {
        return;
    }
    char wait[160];
    DescribeWait(t, wait, sizeof(wait));
    StallLine("[azahar] stall: thread %08x \"%s\" status %x, waits on %s\n",
                  static_cast<unsigned>(id), t.name, static_cast<unsigned>(t.status), wait);
    const uintptr_t begin = reinterpret_cast<uintptr_t>(t.stack);
    char line[256];
    int at = 0;
    int found = 0;
    for (uintptr_t p = (begin + t.stackSize - 4) & ~uintptr_t{3}; p >= begin && found < 48; p -= 4) {
        uintptr_t value;
        std::memcpy(&value, reinterpret_cast<const void*>(p), sizeof(value));
        if (!IsReturnAddress(value)) {
            continue;
        }
        at += std::snprintf(line + at, sizeof(line) - at, " %08x", static_cast<unsigned>(value));
        if (++found % 12 == 0) {
            StallLine("[azahar] stall:   %s\n", line);
            at = 0;
        }
    }
    if (at != 0) {
        StallLine("[azahar] stall:   %s\n", line);
    }
}

SceUID stall_flag = -1;
SceUID main_thread;

} // Anonymous namespace

void PrintThreadStates(const char* reason, bool to_report) {
    stall_to_report = to_report;
    StallLine("[azahar] stall: %s; the threads (the main thread, then the emulator's):\n", reason);
    PrintThreadState(main_thread);
    std::int32_t ids[64];
    const size_t n = Common::RunningNamedThreadIds(ids, std::size(ids));
    for (size_t i = 0; i < n; i++) {
        PrintThreadState(ids[i]);
    }
    // The render thread's own view, twice: whether its loop still turns (a thread stuck in one
    // call has the same passes) and what the emulation waits for.
    static char state[1024];
    for (int k = 0; k < 2; k++) {
        if (k != 0) {
            sceKernelDelayThread(200 * 1000);
        }
        const int at = std::snprintf(state, sizeof(state), "[azahar] stall: render%s: ",
                                     k == 0 ? "" : " 200 ms on");
        SgxdRenderer::RenderThread::DescribeLive(state + at, sizeof(state) - at - 1);
        std::strcat(state, "\n");
        if (stall_to_report) {
            Common::Log::AppendCrashReport(state);
        } else {
            sceClibPrintf("%s", state);
        }
        // The DSP thread's (a frame it never ends holds the emulation thread at its interrupt).
        const int dsp_at = std::snprintf(state, sizeof(state), "[azahar] stall: dsp%s: ",
                                         k == 0 ? "" : " 200 ms on");
        AudioCore::DspHle::DescribeWorker(state + dsp_at, sizeof(state) - dsp_at - 1);
        std::strcat(state, "\n");
        if (stall_to_report) {
            Common::Log::AppendCrashReport(state);
        } else {
            sceClibPrintf("%s", state);
        }
    }
}

namespace {

/// The memory left: the heap, the GPU's pool and the system's. The heap chunk by chunk as heapmap
/// does, only ever inside its arena: a damaged chunk ends the walk, not the report. Last in the
/// report, behind malloc's lock: a thread that holds it stopped costs this line alone.
void WriteMemoryLine() {
    __malloc_lock(_REENT);
    const uintptr_t base = (reinterpret_cast<uintptr_t>(__malloc_sbrk_base) + 7) & ~uintptr_t{7};
    const uintptr_t top = reinterpret_cast<uintptr_t>(__malloc_av_[2]);
    const uintptr_t heap_end =
        reinterpret_cast<uintptr_t>(__malloc_sbrk_base) + _newlib_heap_size_user;
    u32 used = 0, largest_free = 0, free_run = 0, top_room = 0;
    bool whole = top >= base && top < heap_end;
    if (whole) {
        for (uintptr_t p = base; p < top;) {
            const u32 size = *reinterpret_cast<const u32*>(p + 4) & ~3u;
            if (size < 16 || p + size > top) {
                whole = false;
                break;
            }
            if ((*reinterpret_cast<const u32*>(p + size + 4) & 1u) != 0) {
                used += size;
                largest_free = std::max(largest_free, free_run);
                free_run = 0;
            } else {
                free_run += size;
            }
            p += size;
        }
        top_room = static_cast<u32>(heap_end - top);
    }
    __malloc_unlock(_REENT);
    largest_free = std::max({largest_free, free_run, top_room});

    char pool[64];
    SgxdRenderer::GuestGpuMemory::DescribePool(pool, sizeof(pool));
    SceKernelFreeMemorySizeInfo system_free{};
    system_free.size = sizeof(system_free);
    sceKernelGetFreeMemorySize(&system_free);
    char line[256];
    std::snprintf(line, sizeof(line),
                  "[azahar] memory: heap %u KiB in use of %u, largest free %u KiB%s; %s; system "
                  "free: %u KiB user, %u CDRAM, %u phycont\n",
                  used >> 10, _newlib_heap_size_user >> 10, largest_free >> 10,
                  whole ? "" : " (the walk stopped at a damaged chunk)", pool,
                  static_cast<unsigned>(system_free.size_user) >> 10,
                  static_cast<unsigned>(system_free.size_cdram) >> 10,
                  static_cast<unsigned>(system_free.size_phycont) >> 10);
    Common::Log::AppendCrashReport(line);
}

/// The render and DSP threads' own views and the memory left: a core dump has every thread's
/// registers and stack, but not the large heap blocks where these live.
int ProcessStateEntry(SceSize, void*) {
    static char state[1024];
    int at = std::snprintf(state, sizeof(state), "[azahar] crash: render: ");
    SgxdRenderer::RenderThread::DescribeLive(state + at, sizeof(state) - at - 1);
    std::strcat(state, "\n");
    Common::Log::AppendCrashReport(state);
    at = std::snprintf(state, sizeof(state), "[azahar] crash: dsp: ");
    AudioCore::DspHle::DescribeWorker(state + at, sizeof(state) - at - 1);
    std::strcat(state, "\n");
    Common::Log::AppendCrashReport(state);
    WriteMemoryLine();
    return 0;
}

int ProbeEntry(SceSize, void*) {
    return 0;
}

/// A core whose probe did not run (ProbeCores), -1 for none: a core dump would wait for it for
/// good, as the suspend of every thread does.
std::atomic<int> stopped_core{-1};

/// Whether each core still runs a thread of ours: a probe pinned to it in turn, given 200 ms to
/// run and end. A render thread ready to run that never runs leaves this open: its core stopped,
/// or the thread did.
void ProbeCores() {
    for (int core = 0; core < 4; core++) {
        const SceUID t = sceKernelCreateThread("stall probe", ProbeEntry,
                                               SCE_KERNEL_PROCESS_PRIORITY_USER_HIGH, 0x1000, 0,
                                               SCE_KERNEL_CPU_MASK_USER_0 << core, nullptr);
        if (t < 0) {
            StallLine("[azahar] stall: core %d: no probe thread (%08x)\n", core,
                      static_cast<unsigned>(t));
            continue;
        }
        const SceUInt64 start = sceKernelGetProcessTimeWide();
        sceKernelStartThread(t, 0, nullptr);
        SceUInt timeout = 200 * 1000;
        const int r = sceKernelWaitThreadEnd(t, nullptr, &timeout);
        StallLine("[azahar] stall: core %d: a probe thread %s (%llu us)\n", core,
                  r >= 0 ? "ran" : "did not run", static_cast<unsigned long long>(
                                                      sceKernelGetProcessTimeWide() - start));
        if (r >= 0) {
            sceKernelDeleteThread(t);
            continue;
        }
        stopped_core = core;
        // A core that runs nothing of ours: what it runs instead, three times 50 ms apart.
        for (int k = 0; k < 3; k++) {
            AzaharCoreSample cs{};
            const int got = azaharCoreSample(static_cast<uint32_t>(core), &cs);
            if (got < 0) {
                StallLine("[azahar] stall: core %d: no sample (%08x)\n", core,
                          static_cast<unsigned>(got));
                break;
            }
            cs.thread_name[sizeof(cs.thread_name) - 1] = '\0';
            cs.module_name[sizeof(cs.module_name) - 1] = '\0';
            StallLine("[azahar] stall: core %d runs pid %08x thread %08x \"%s\"%s; PC in %s+%x; "
                      "PCs %08x %08x %08x %08x %08x %08x %08x %08x\n",
                      core, static_cast<unsigned>(cs.pid), static_cast<unsigned>(cs.thid),
                      cs.thread_name, cs.idle ? " (the idle thread)" : "",
                      cs.module_name[0] ? cs.module_name : "?",
                      static_cast<unsigned>((cs.pcs[0] & ~1u) - cs.module_text),
                      static_cast<unsigned>(cs.pcs[0]),
                      static_cast<unsigned>(cs.pcs[1]), static_cast<unsigned>(cs.pcs[2]),
                      static_cast<unsigned>(cs.pcs[3]), static_cast<unsigned>(cs.pcs[4]),
                      static_cast<unsigned>(cs.pcs[5]), static_cast<unsigned>(cs.pcs[6]),
                      static_cast<unsigned>(cs.pcs[7]));
            sceKernelDelayThread(50 * 1000);
        }
    }
}

/// The module's stray exceptions (azaharStray): code of its own that faulted outside the guest's,
/// which parks a core that has no slice to end. The instruction, and an abort's address.
void PrintStrays() {
    static constexpr const char* Names[] = {"exception", "svc", "undefined instruction",
                                            "prefetch abort", "data abort"};
    for (u32 core = 0; core < 4; core++) {
        AzaharStray s{};
        const int got = azaharStray(core, &s);
        if (got < 0 || s.count == 0) {
            continue;
        }
        s.module_name[sizeof(s.module_name) - 1] = '\0';
        const bool thumb = (s.spsr & (1u << 5)) != 0;
        const u32 at = s.lr - (s.reason == 4 ? 8u : (thumb && s.reason != 3) ? 2u : 4u);
        StallLine("[azahar] stray: core %u: %u, the first a %s at %s+%x (LR %08x, SPSR %08x); "
                  "DFSR %08x DFAR %08x IFSR %08x IFAR %08x; %u retries; slice step %u; %u slices "
                  "ended so far\n",
                  static_cast<unsigned>(core), static_cast<unsigned>(s.count),
                  s.reason < std::size(Names) ? Names[s.reason] : Names[0],
                  s.module_text != 0 ? s.module_name : "?",
                  static_cast<unsigned>(at - s.module_text), static_cast<unsigned>(s.lr),
                  static_cast<unsigned>(s.spsr), static_cast<unsigned>(s.dfsr),
                  static_cast<unsigned>(s.dfar), static_cast<unsigned>(s.ifsr),
                  static_cast<unsigned>(s.ifar), static_cast<unsigned>(s.retries),
                  static_cast<unsigned>(s.progress), static_cast<unsigned>(s.ended));
    }
}

/// The render thread moved to core 3, then its view 500 ms later: whether it runs again away
/// from its core.
void MoveRenderThread() {
    const s32 tid = SgxdRenderer::RenderThread::LiveThreadId();
    if (tid < 0) {
        return;
    }
    const int r = sceKernelChangeThreadCpuAffinityMask(tid, SCE_KERNEL_CPU_MASK_USER_0 << 3);
    StallLine("[azahar] stall: the render thread moved to core 3: %08x\n",
              static_cast<unsigned>(r));
    sceKernelDelayThread(500 * 1000);
    static char state[1024];
    const int at = std::snprintf(state, sizeof(state), "[azahar] stall: render 500 ms on core 3: ");
    SgxdRenderer::RenderThread::DescribeLive(state + at, sizeof(state) - at - 1);
    std::strcat(state, "\n");
    if (stall_to_report) {
        Common::Log::AppendCrashReport(state);
    } else {
        sceClibPrintf("%s", state);
    }
}

} // namespace

void WriteProcessState() {
    // A thread of its own: the render thread's view reads that thread's state, and
    // sceKernelGetThreadInfo on another thread's ID is a critical error that debug-suspends the
    // caller, and the crash must not depend on its resume.
    constexpr SceUInt WaitUs = 5 * 1000 * 1000;
    const SceUID t = sceKernelCreateThread("crash report", ProcessStateEntry,
                                           SCE_KERNEL_PROCESS_PRIORITY_USER_HIGH, 64 * 1024, 0,
                                           SCE_KERNEL_CPU_MASK_USER_ALL, nullptr);
    if (t < 0 || sceKernelStartThread(t, 0, nullptr) < 0) {
        Common::Log::AppendCrashReport("[azahar] crash: no thread to read the state with\n");
        return;
    }
    SceUInt timeout = WaitUs;
    if (sceKernelWaitThreadEnd(t, nullptr, &timeout) < 0) {
        Common::Log::AppendCrashReport("[azahar] crash: reading the state did not end\n");
        return;
    }
    sceKernelDeleteThread(t);
}

void PrintBusErrorDump(bool to_report) {
    stall_to_report = to_report;
    // The dump (azaharBusError): a header (its own size, the two sections' sizes), the first
    // section, then the second: per interconnect a record (0x38 bytes: size, initiator and target
    // agent counts, its register block, its error state) and one per agent (0x20 bytes: size, the
    // agent's block, AGENT_STATUS, AGENT_ERROR_LOG, AGENT_ERROR_LOG_ADDR, each 64-bit; status bit
    // 24 an error). Read from a stall's dump; printed: the header, the first section's non-zero
    // rows, and the records that hold an error.
    static u32 words[0xC00];
    std::memset(words, 0, sizeof(words));
    const int got = azaharBusError(words, sizeof(words));
    StallLine("[azahar] bus error dump: %d bytes (%08x); header %08x %08x %08x %08x %08x %08x "
              "%08x\n",
              std::max(got, 0), static_cast<unsigned>(got), static_cast<unsigned>(words[0]),
              static_cast<unsigned>(words[1]), static_cast<unsigned>(words[2]),
              static_cast<unsigned>(words[3]), static_cast<unsigned>(words[4]),
              static_cast<unsigned>(words[5]), static_cast<unsigned>(words[6]));
    const u32 count = static_cast<u32>(std::max(got, 0)) / 4;
    if (count < 7 || words[0] % 4 != 0 || words[2] % 4 != 0) {
        return;
    }
    const u32 first = words[0] / 4, second = (words[0] + words[2]) / 4;
    for (u32 i = first; i + 8 <= std::min(second, count); i += 8) {
        const u32* const w = words + i;
        if (std::all_of(w, w + 8, [](u32 v) { return v == 0; })) {
            continue;
        }
        StallLine("[azahar] bus error dump +%04x: %08x %08x %08x %08x %08x %08x %08x %08x\n",
                  i * 4, static_cast<unsigned>(w[0]), static_cast<unsigned>(w[1]),
                  static_cast<unsigned>(w[2]), static_cast<unsigned>(w[3]),
                  static_cast<unsigned>(w[4]), static_cast<unsigned>(w[5]),
                  static_cast<unsigned>(w[6]), static_cast<unsigned>(w[7]));
    }
    u32 at = second + 2, agents = 0, printed = 0;
    while (at < count) {
        const u32* const w = words + at;
        const u32 size = w[0];
        if (size == 0x38 && at + 14 <= count) {
            if (std::any_of(w + 4, w + 14, [](u32 v) { return v != 0; })) {
                StallLine("[azahar] bus error: interconnect %08x (%u IA, %u TA): %08x %08x %08x "
                          "%08x %08x %08x %08x %08x %08x %08x\n",
                          static_cast<unsigned>(w[3]), static_cast<unsigned>(w[1]),
                          static_cast<unsigned>(w[2]), static_cast<unsigned>(w[4]),
                          static_cast<unsigned>(w[5]), static_cast<unsigned>(w[6]),
                          static_cast<unsigned>(w[7]), static_cast<unsigned>(w[8]),
                          static_cast<unsigned>(w[9]), static_cast<unsigned>(w[10]),
                          static_cast<unsigned>(w[11]), static_cast<unsigned>(w[12]),
                          static_cast<unsigned>(w[13]));
                printed++;
            }
        } else if (size == 0x20 && at + 8 <= count) {
            agents++;
            if ((w[2] & 0x01000000u) != 0 || std::any_of(w + 3, w + 8, [](u32 v) { return v; })) {
                StallLine("[azahar] bus error: agent %08x status %08x %08x, log %08x %08x, "
                          "address %08x %08x\n",
                          static_cast<unsigned>(w[1]), static_cast<unsigned>(w[2]),
                          static_cast<unsigned>(w[3]), static_cast<unsigned>(w[4]),
                          static_cast<unsigned>(w[5]), static_cast<unsigned>(w[6]),
                          static_cast<unsigned>(w[7]));
                printed++;
            }
        } else {
            StallLine("[azahar] bus error dump: a record of %08x bytes at +%04x, read no further\n",
                      static_cast<unsigned>(size), at * 4);
            break;
        }
        at += size / 4;
        if (printed >= 64) {
            break;
        }
    }
    StallLine("[azahar] bus error dump: %u agents read, %u records with an error\n", agents,
              printed);
    AzaharBusErrorCatch c{};
    const int r = azaharBusErrorWatch(&c);
    StallLine("[azahar] bus error caught: %08x, %u interrupt(s), the first on core %u\n",
              static_cast<unsigned>(r), static_cast<unsigned>(c.count),
              static_cast<unsigned>(c.core));
    if (c.count == 0) {
        return;
    }
    for (u32 core = 0; core < 4; core++) {
        const u32* const p = c.pcs[core];
        c.module_name[core][sizeof(c.module_name[core]) - 1] = '\0';
        StallLine("[azahar] bus error caught: core %u pid %08x thread %08x, PC in %s+%x; PCs %08x "
                  "%08x %08x %08x %08x %08x %08x %08x\n",
                  static_cast<unsigned>(core), static_cast<unsigned>(c.pid[core]),
                  static_cast<unsigned>(c.thid[core]),
                  c.module_name[core][0] ? c.module_name[core] : "?",
                  static_cast<unsigned>((p[0] & ~1u) - c.module_text[core]),
                  static_cast<unsigned>(p[0]), static_cast<unsigned>(p[1]),
                  static_cast<unsigned>(p[2]), static_cast<unsigned>(p[3]),
                  static_cast<unsigned>(p[4]), static_cast<unsigned>(p[5]),
                  static_cast<unsigned>(p[6]), static_cast<unsigned>(p[7]));
        StallLine("[azahar] bus error caught: core %u PCs %08x %08x %08x %08x %08x %08x %08x "
                  "%08x\n",
                  static_cast<unsigned>(core), static_cast<unsigned>(p[8]),
                  static_cast<unsigned>(p[9]), static_cast<unsigned>(p[10]),
                  static_cast<unsigned>(p[11]), static_cast<unsigned>(p[12]),
                  static_cast<unsigned>(p[13]), static_cast<unsigned>(p[14]),
                  static_cast<unsigned>(p[15]));
    }
}

void WatchBusErrors() {
    const int r = azaharBusErrorWatch(nullptr);
    sceClibPrintf("[azahar] bus error watch: %08x\n", static_cast<unsigned>(r));
}

void PrintTableScan(bool to_report) {
    stall_to_report = to_report;
    AzaharTableScan t{};
    const int r = azaharTableScan(&t);
    StallLine("[azahar] tables: %08x; the guest's upper copy: %u stale, %u missing; our tables: "
              "%u descriptor(s) outside memory\n",
              static_cast<unsigned>(r), static_cast<unsigned>(t.stale),
              static_cast<unsigned>(t.missing), static_cast<unsigned>(t.ours_wild));
    for (u32 k = 0; k < std::min<u32>(t.stale, AZAHAR_TABLESCAN_N); k++) {
        StallLine("[azahar] tables: stale %08x: guest %08x, process %08x; its table: %u outside "
                  "memory (%08x)\n",
                  static_cast<unsigned>(t.stale_va[k]), static_cast<unsigned>(t.stale_guest[k]),
                  static_cast<unsigned>(t.stale_process[k]),
                  static_cast<unsigned>(t.stale_wild[k]),
                  static_cast<unsigned>(t.stale_wild_desc[k]));
    }
    StallLine("[azahar] tables: the guest's copy of the kernel's low entries: %u stale\n",
              static_cast<unsigned>(t.kstale));
    for (u32 k = 0; k < std::min<u32>(t.kstale, AZAHAR_TABLESCAN_N); k++) {
        StallLine("[azahar] tables: kernel stale %08x: guest %08x, kernel %08x; its table: %u "
                  "outside memory (%08x)\n",
                  static_cast<unsigned>(t.kstale_va[k]), static_cast<unsigned>(t.kstale_guest[k]),
                  static_cast<unsigned>(t.kstale_kernel[k]),
                  static_cast<unsigned>(t.kstale_wild[k]),
                  static_cast<unsigned>(t.kstale_wild_desc[k]));
    }
    StallLine("[azahar] tables: by domain (DACR %08x in the syscall): valid level-1 entries, "
              "the first, outside memory\n",
              static_cast<unsigned>(t.dacr));
    for (u32 d = 0; d < 16; d++) {
        if (t.dom_valid[d] == 0) {
            continue;
        }
        StallLine("[azahar] tables: domain %2u (%s): %u, first %08x = %08x; %u outside memory "
                  "(%08x)\n",
                  static_cast<unsigned>(d), ((t.dacr >> (2 * d)) & 3u) ? "open" : "No-access",
                  static_cast<unsigned>(t.dom_valid[d]), static_cast<unsigned>(t.dom_first_va[d]),
                  static_cast<unsigned>(t.dom_first_desc[d]), static_cast<unsigned>(t.dom_wild[d]),
                  static_cast<unsigned>(t.dom_wild_desc[d]));
    }
    StallLine("[azahar] tables: page fffff000: guest level-1 %08x (the process's %08x), "
              "level-2 %08x\n",
              static_cast<unsigned>(t.top_l1), static_cast<unsigned>(t.top_process),
              static_cast<unsigned>(t.top_l2));
    for (u32 k = 0; k < std::min<u32>(t.ours_wild, AZAHAR_TABLESCAN_N); k++) {
        StallLine("[azahar] tables: ours at pa %08x: %08x\n",
                  static_cast<unsigned>(t.ours_wild_at[k]),
                  static_cast<unsigned>(t.ours_wild_desc[k]));
    }
}

void StartStallWatch() {
    main_thread = sceKernelGetThreadId();
    stall_flag = sceKernelCreateEventFlag("azahar stall watch", 0, 0, nullptr);
    ASSERT_MSG(stall_flag >= 0, "stall watch: sceKernelCreateEventFlag {:#010x}",
               static_cast<u32>(stall_flag));
    // For the process's life: never joined.
    new Common::NamedThread(Common::ThreadCfg{"stall watch", 16 * 1024}, [] {
        SceUInt64 last_beat = sceKernelGetProcessTimeWide();
        for (;;) {
            SceUInt timeout = 10 * 1000 * 1000;
            unsigned int bits = 0;
            if (sceKernelWaitEventFlag(stall_flag, 1, SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT,
                                       &bits, &timeout) >= 0 ||
                stall_held.load(std::memory_order_relaxed)) {
                last_beat = sceKernelGetProcessTimeWide();
                continue;
            }
            const SceUInt64 quiet_s = (sceKernelGetProcessTimeWide() - last_beat) / 1000000;
            if (quiet_s >= StallSeconds) {
                char reason[64];
                std::snprintf(reason, sizeof(reason), "the frontend's loop stopped for %u s",
                              static_cast<unsigned>(quiet_s));
                // To the memory card only: a link that stopped (a host0 answer
                // lost) would hold a print here for good. The next start prints it. Its
                // preamble as things are now.
                Common::Log::RenewCrashReport();
                PrintThreadStates(reason, true);
                ProbeCores();
                PrintStrays();
                PrintBusErrorDump(true);
                PrintTableScan(true);
                MoveRenderThread();
                // Then the core dump, every thread's registers and stack in it (parse_core.py),
                // where the listing above has no thread's PC or depth, and the game list again.
                // The listing is the report: the watch's own stack says nothing.
                Common::Log::AppendCrashReport(
                    "[azahar] stall: the frontend's loop stopped; recovering\n");
                reported = true;
                Recover(reason);
            }
        }
    });
}

void EnableRecovery(const std::vector<std::string>& arguments, const unsigned long* command_seq) {
    std::size_t at = 0;
    int n = 0;
    for (const std::string& argument : arguments) {
        // Three places kept for seq=N, crashed=PATH and changed=KEYS, and the list's end.
        if (at + argument.size() + 1 > sizeof(restart_text) ||
            n + 4 >= static_cast<int>(std::size(restart_argv))) {
            break;
        }
        std::memcpy(restart_text + at, argument.c_str(), argument.size() + 1);
        restart_argv[n++] = restart_text + at;
        at += argument.size() + 1;
    }
    restart_seq = command_seq;
    restart_argc = n;
    Common::Log::SetCrashHook([] { Recover("a failed assert"); });
}

u64 DumpBytesWritten(long since) {
    // The file's path once found: the system writes the dump under it to the end.
    static char found[160];
    static long found_since;
    if (found_since != since) {
        found[0] = '\0';
        found_since = since;
    }
    if (found[0] == '\0') {
        long newest = since - 1;
        for (const char* dir : {"host0:", "ux0:data/"}) {
            const SceUID d = sceIoDopen(dir);
            if (d < 0) {
                continue;
            }
            SceIoDirent entry{};
            while (sceIoDread(d, &entry) > 0) {
                const std::string_view name{entry.d_name};
                if (name.starts_with("psp2core-") && name.ends_with(".psp2dmp.tmp")) {
                    const long time = std::strtol(entry.d_name + 9, nullptr, 10);
                    if (time > newest) {
                        newest = time;
                        std::snprintf(found, sizeof(found), "%s%s", dir, entry.d_name);
                    }
                }
                entry = {};
            }
            sceIoDclose(d);
        }
    }
    SceIoStat stat{};
    return found[0] != '\0' && sceIoGetstat(found, &stat) >= 0 ? static_cast<u64>(stat.st_size)
                                                                : 0;
}

namespace {
constexpr char DumpSizePath[] = "ux0:data/azahar/dump-size.txt";
}

u64 LastDumpSize() {
    char text[32]{};
    const SceUID fd = sceIoOpen(DumpSizePath, SCE_O_RDONLY, 0);
    if (fd < 0) {
        return 0;
    }
    sceIoRead(fd, text, sizeof(text) - 1);
    sceIoClose(fd);
    return std::strtoull(text, nullptr, 10);
}

void NoteDumpSize(const char* path) {
    SceIoStat stat{};
    if (sceIoGetstat(path, &stat) < 0) {
        return;
    }
    char text[32];
    const int n = std::snprintf(text, sizeof(text), "%llu\n",
                                static_cast<unsigned long long>(stat.st_size));
    const SceUID fd = sceIoOpen(DumpSizePath, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd >= 0) {
        sceIoWrite(fd, text, n);
        sceIoClose(fd);
    }
}

namespace {

constexpr char CrashesDir[] = "ux0:data/azahar/crashes";

struct ZipEntry {
    std::string name;
    u32 crc{}, compressed{}, size{}, offset{};
};

void Put16(std::vector<u8>& out, u32 v) {
    out.push_back(static_cast<u8>(v));
    out.push_back(static_cast<u8>(v >> 8));
}

void Put32(std::vector<u8>& out, u32 v) {
    Put16(out, v & 0xFFFF);
    Put16(out, v >> 16);
}

bool WriteAll(SceUID fd, const void* data, std::size_t size) {
    return sceIoWrite(fd, data, static_cast<SceSize>(size)) == static_cast<int>(size);
}

/// A local header (signature, version 2.0, no flags, deflate, the time, then the entry's crc and
/// sizes, which are written again once known) or a central directory record.
std::vector<u8> ZipHeader(const ZipEntry& e, u32 dos_time, u32 dos_date, bool central) {
    std::vector<u8> h;
    Put32(h, central ? 0x02014b50 : 0x04034b50);
    if (central) {
        Put16(h, 20);
    }
    Put16(h, 20);
    Put16(h, 0);
    Put16(h, 8);
    Put16(h, dos_time);
    Put16(h, dos_date);
    Put32(h, e.crc);
    Put32(h, e.compressed);
    Put32(h, e.size);
    Put16(h, static_cast<u32>(e.name.size()));
    Put16(h, 0);
    if (central) {
        Put16(h, 0);           // comment
        Put16(h, 0);           // disk
        Put16(h, 0);           // internal attributes
        Put32(h, 0);           // external attributes
        Put32(h, e.offset);
    }
    h.insert(h.end(), e.name.begin(), e.name.end());
    return h;
}

bool ZipFile(SceUID fd, const std::string& path, ZipEntry& e, u32 dos_time, u32 dos_date,
             u64& done, u64 total, const std::function<void(u64, u64)>& progress) {
    const SceUID in = sceIoOpen(path.c_str(), SCE_O_RDONLY, 0);
    if (in < 0) {
        return false;
    }
    e.offset = static_cast<u32>(sceIoLseek(fd, 0, SCE_SEEK_CUR));
    const std::vector<u8> header = ZipHeader(e, dos_time, dos_date, false);
    bool ok = WriteAll(fd, header.data(), header.size());
    z_stream z{};
    ok = ok && deflateInit2(&z, Z_BEST_SPEED, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) == Z_OK;
    std::vector<u8> input(256 * 1024), output(256 * 1024);
    uLong crc = crc32(0, nullptr, 0);
    int read = 0;
    for (bool end = false; ok && !end;) {
        read = sceIoRead(in, input.data(), static_cast<SceSize>(input.size()));
        if (read < 0) {
            ok = false;
            break;
        }
        end = read == 0;
        crc = crc32(crc, input.data(), static_cast<uInt>(read));
        e.size += static_cast<u32>(read);
        z.next_in = input.data();
        z.avail_in = static_cast<uInt>(read);
        do {
            z.next_out = output.data();
            z.avail_out = static_cast<uInt>(output.size());
            deflate(&z, end ? Z_FINISH : Z_NO_FLUSH);
            const std::size_t produced = output.size() - z.avail_out;
            ok = WriteAll(fd, output.data(), produced);
            e.compressed += static_cast<u32>(produced);
        } while (ok && z.avail_out == 0);
        done += static_cast<u64>(read);
        progress(done, total);
    }
    deflateEnd(&z);
    sceIoClose(in);
    e.crc = static_cast<u32>(crc);
    const SceOff end = sceIoLseek(fd, 0, SCE_SEEK_CUR);
    const std::vector<u8> final_header = ZipHeader(e, dos_time, dos_date, false);
    ok = ok && sceIoLseek(fd, e.offset, SCE_SEEK_SET) == e.offset &&
         WriteAll(fd, final_header.data(), final_header.size()) &&
         sceIoLseek(fd, end, SCE_SEEK_SET) == end;
    return ok;
}

u64 FileSize(const std::string& path) {
    SceIoStat stat{};
    return sceIoGetstat(path.c_str(), &stat) >= 0 ? static_cast<u64>(stat.st_size) : 0;
}

} // Anonymous namespace

std::string ArchiveCrash(const std::string& report, const std::string& dump,
                         const std::function<void(u64 done, u64 total)>& progress) {
    SceDateTime now{};
    sceRtcGetCurrentClockLocalTime(&now);
    sceIoMkdir(CrashesDir, 0777);
    const std::string archive =
        fmt::format("{}/crash-{:04}{:02}{:02}-{:02}{:02}{:02}.zip", CrashesDir, now.year, now.month,
                    now.day, now.hour, now.minute, now.second);
    const u32 dos_time = static_cast<u32>(now.hour << 11 | now.minute << 5 | now.second / 2);
    const u32 dos_date =
        static_cast<u32>(std::max(now.year - 1980, 0) << 9 | now.month << 5 | now.day);
    const SceUID fd = sceIoOpen(archive.c_str(), SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) {
        LOG_ERROR(Frontend, "Could not create {}: {:#010x}", archive, static_cast<u32>(fd));
        return {};
    }
    std::vector<ZipEntry> entries{{"crash.txt"}};
    if (!dump.empty()) {
        const std::size_t slash = dump.find_last_of(":/");
        entries.push_back({slash == std::string::npos ? dump : dump.substr(slash + 1)});
    }
    const u64 total = FileSize(report) + (dump.empty() ? 0 : FileSize(dump));
    u64 done = 0;
    bool ok = ZipFile(fd, report, entries[0], dos_time, dos_date, done, total, progress) &&
              (dump.empty() ||
               ZipFile(fd, dump, entries[1], dos_time, dos_date, done, total, progress));
    std::vector<u8> directory;
    for (const ZipEntry& e : entries) {
        const std::vector<u8> h = ZipHeader(e, dos_time, dos_date, true);
        directory.insert(directory.end(), h.begin(), h.end());
    }
    const u32 directory_at = static_cast<u32>(sceIoLseek(fd, 0, SCE_SEEK_CUR));
    const u32 directory_size = static_cast<u32>(directory.size());
    Put32(directory, 0x06054b50);
    Put16(directory, 0);
    Put16(directory, 0);
    Put16(directory, static_cast<u32>(entries.size()));
    Put16(directory, static_cast<u32>(entries.size()));
    Put32(directory, directory_size);
    Put32(directory, directory_at);
    Put16(directory, 0);
    ok = ok && WriteAll(fd, directory.data(), directory.size());
    ok = sceIoClose(fd) >= 0 && ok;
    if (!ok) {
        LOG_ERROR(Frontend, "Could not write {}; {} and {} stay", archive, report, dump);
        sceIoRemove(archive.c_str());
        return {};
    }
    if (!dump.empty()) {
        sceIoRemove(dump.c_str());
    }
    sceClibPrintf("[azahar] crash archive %s: %u + %u bytes in %llu\n", archive.c_str(),
                  entries[0].size, dump.empty() ? 0u : entries[1].size,
                  static_cast<unsigned long long>(FileSize(archive)));
    return archive;
}

/// A core dump being written (Coredump's or Recover's). The module's dump keeps one dump's state at
/// a time, with no guard of its own, and its first part suspends the process, the rest runs beside
/// it: a crash of another thread meanwhile (an assert's Recover) would start a second one, then
/// end the process under the first, and a process that ends while its dump runs takes the kernel
/// down. So a dump waits for the one running.
std::atomic<bool> dump_running;

int DumpAlone(char* path, unsigned path_size) {
    bool idle = false;
    while (!dump_running.compare_exchange_weak(idle, true)) {
        idle = false;
        sceKernelDelayThread(100 * 1000);
    }
    const int dumped = azaharCoredump(path, path_size);
    dump_running = false;
    return dumped;
}

int Coredump(char* path, unsigned path_size) {
    stall_held = true;
    const int dumped = DumpAlone(path, path_size);
    stall_held = false;
    return dumped;
}

void Recover(const char* reason) {
    static std::atomic<bool> recovering{false};
    if (recovering.exchange(true)) {
        // Another thread's crash is being taken: this one stops here, its message in the log.
        sceKernelExitDeleteThread(0);
    }
    stall_held = true;
    Common::Log::RenewCrashReport();
    if (!reported) {
        WriteStackReport(reason);
    }
    WriteProcessState();
    BlankDisplay();
    if (restart_argc == 0) {
        __builtin_trap();
    }
    // The dump before anything else changes the process. Minutes over host0, seconds
    // to a memory card; the display stays black meanwhile. None with a core stopped: the dump's
    // suspend of every thread would wait for it for good.
    static char path[256];
    static char line[320];
    const int stopped = stopped_core.load();
    int dumped = -1;
    if (stopped >= 0) {
        std::snprintf(line, sizeof(line), "[azahar] crash: no core dump, core %d stopped\n",
                      stopped);
        Write(line);
    } else {
        dumped = DumpAlone(path, sizeof(path));
    }
    std::snprintf(line, sizeof(line), "[azahar] crash: core dump %08x, %s; restarting\n",
                  static_cast<unsigned>(dumped), dumped >= 0 ? path : "none");
    Write(line);
    // The restart: the game list, its notice naming the dump, and the running title's settings
    // that are not at their defaults (to try those first).
    static char seq[32];
    static char crashed[sizeof(path) + 16];
    static char changed[200];
    int n = restart_argc;
    if (restart_seq != nullptr) {
        std::snprintf(seq, sizeof(seq), "seq=%lu", *restart_seq);
        restart_argv[n++] = seq;
    }
    std::snprintf(crashed, sizeof(crashed), "crashed=%s", dumped >= 0 ? path : "");
    restart_argv[n++] = crashed;
    constexpr std::size_t changed_at = sizeof("changed=") - 1;
    std::memcpy(changed, "changed=", changed_at);
    if (VitaFrontend::Config::WriteChangedFromDefaults(changed + changed_at,
                                                       sizeof(changed) - changed_at) > 0) {
        restart_argv[n++] = changed;
    }
    restart_argv[n] = nullptr;
    const int exec =
        sceAppMgrLoadExec("app0:eboot.bin", const_cast<char* const*>(restart_argv), nullptr);
    sceClibPrintf("[azahar] crash: restart %08x\n", static_cast<unsigned>(exec));
    if (exec >= 0) {
        // As LoadAzaharNative's restart: the system ends this process soon after the call.
        for (int waited = 0; waited < 50; ++waited) {
            sceKernelDelayThread(100 * 1000);
        }
    }
    __builtin_trap();
}

void StallWatchBeat() {
    if (stall_flag >= 0) {
        sceKernelSetEventFlag(stall_flag, 1);
    }
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
    // sgxd's four (sgxd.c's SGXD_GPU_MODULE_NID*).
    firmware = module_nid == 0x4069F83Bu   ? "3.60"
               : module_nid == 0xC0F361A3u ? "3.65"
               : module_nid == 0xC01EF58Au ? "3.68"
               : module_nid == 0x18EECAEEu ? "3.73"
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
    // A stall's listing is a few KiB, the preamble's console up to 16.
    static char text[64 * 1024];
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
    } else if (size >= SiteThreshold) {
        TrackSite(p, size);
    }
#ifdef VITA_DIAGNOSTICS
    if (leak != nullptr && p != nullptr) {
        LeakRecord(p);
    }
#endif
    return p;
}

void* __wrap_calloc(size_t count, size_t size) {
    void* const p = _calloc_r(MALLOC_REENT, count, size);
    if (p == nullptr && count * size != 0) {
        failed_malloc_size = count * size;
    } else if (count * size >= SiteThreshold) {
        TrackSite(p, count * size);
    }
#ifdef VITA_DIAGNOSTICS
    if (leak != nullptr && p != nullptr) {
        LeakRecord(p);
    }
#endif
    return p;
}

void* __wrap_realloc(void* old, size_t size) {
    if (old != nullptr && ChunkSize(old) >= SiteThreshold) {
        UntrackSite(old);
    }
#ifdef VITA_DIAGNOSTICS
    if (leak != nullptr && old != nullptr) {
        LeakForget(old);
    }
#endif
    void* const p = _realloc_r(MALLOC_REENT, old, size);
    if (p == nullptr && size != 0) {
        failed_malloc_size = size;
    } else if (size >= SiteThreshold) {
        TrackSite(p, size);
    }
#ifdef VITA_DIAGNOSTICS
    if (leak != nullptr && p != nullptr) {
        LeakRecord(p);
    }
#endif
    return p;
}

void __wrap_free(void* p) {
    if (p != nullptr && ChunkSize(p) >= SiteThreshold) {
        UntrackSite(p);
    }
#ifdef VITA_DIAGNOSTICS
    if (leak != nullptr && p != nullptr) {
        LeakForget(p);
    }
#endif
    __real__free_r(MALLOC_REENT, p);
}

// The C library's own entry points, which its stdio and the like call without malloc: blocks it
// allocates for itself are recorded too while leaktrack is on.
void* __wrap__malloc_r(struct _reent* r, size_t size) {
    void* const p = __real__malloc_r(r, size);
#ifdef VITA_DIAGNOSTICS
    if (leak != nullptr && p != nullptr) {
        LeakRecord(p);
    }
#endif
    return p;
}

void __wrap__free_r(struct _reent* r, void* p) {
#ifdef VITA_DIAGNOSTICS
    if (leak != nullptr && p != nullptr) {
        LeakForget(p);
    }
#endif
    __real__free_r(r, p);
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
    // newlib raises only for abort.
    VitaFrontend::CrashReport::Recover("an abort");
}

} // extern "C"
