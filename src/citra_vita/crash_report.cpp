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

#include <algorithm>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <string_view>

#include <psp2/display.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <unwind.h>

#include "audio_core/hle/hle.h"
#include "citra_vita/config.h"
#include "citra_vita/crash_report.h"
#include "common/assert.h"
#include "common/logging/backend.h"
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

int __real_raise(int sig);
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
        }
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

void StartStallWatch() {
    main_thread = sceKernelGetThreadId();
    stall_flag = sceKernelCreateEventFlag("azahar stall watch", 0, 0, nullptr);
    ASSERT_MSG(stall_flag >= 0, "stall watch: sceKernelCreateEventFlag {:#010x}",
               static_cast<u32>(stall_flag));
    // For the process's life: never joined.
    new Common::NamedThread(Common::ThreadCfg{"stall watch", 16 * 1024}, [] {
        SceUInt64 last_beat = sceKernelGetProcessTimeWide();
        bool reported = false;
        for (;;) {
            SceUInt timeout = 10 * 1000 * 1000;
            unsigned int bits = 0;
            if (sceKernelWaitEventFlag(stall_flag, 1, SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT,
                                       &bits, &timeout) >= 0) {
                last_beat = sceKernelGetProcessTimeWide();
                reported = false;
                continue;
            }
            const SceUInt64 quiet_s = (sceKernelGetProcessTimeWide() - last_beat) / 1000000;
            if (!reported && quiet_s >= StallSeconds) {
                char reason[64];
                std::snprintf(reason, sizeof(reason), "the frontend's loop stopped for %u s",
                              static_cast<unsigned>(quiet_s));
                // To the memory card only: a link that stopped (a host0 answer
                // lost) would hold a print here for good. The next start prints it. Its
                // preamble as things are now.
                Common::Log::RenewCrashReport();
                PrintThreadStates(reason, true);
                ProbeCores();
                MoveRenderThread();
                Common::Log::AppendCrashReport(
                    "[azahar] stall: the frontend's loop stopped; faulting for the core dump\n");
                BlankDisplay();
                __builtin_trap();
                reported = true;
            }
        }
    });
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
    return __real_raise(sig);
}

} // extern "C"
