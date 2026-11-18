// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// heapprof (ux0:data/mh4u/heapprof, release too): a sampling profile of the C++ heap, to find
// what keeps growing in a long session. MH4U in normal play near monsters grew the heap by
// ~60 KiB a second until std::bad_alloc ended the game (2026-11-15), while a save state of the
// same spot stayed flat - so the leak had to be caught in a real session, at release speed.
//
// The global operator new/delete are replaced (everything the C++ code allocates passes here).
// About once every SamplePeriod bytes allocated, the allocation is recorded with its call site
// (the return address into the code that called operator new, plus two more return addresses
// found on the stack); a delete of a recorded pointer takes it out again. So the per-site sums
// estimate the live bytes each call site holds, and a leak is the site whose sum keeps rising.
// Every 10 s HeapProfReport prints the top sites; on an allocation failure the same report is
// printed before std::bad_alloc is thrown. Off (no file): one predictable branch per call.
//
// Fragmentation (2026-11-18): a 1 h 45 min session died in std::bad_alloc on a 258 KiB request
// with ~212 MiB in use of the 240 MiB block: the heap had grown to the whole block and the ~33
// MiB free were in pieces. So each report also walks malloc's free lists (free bytes, chunk
// count, the largest free chunk and the top chunk, free bytes by chunk size), and allocations of
// BigMin bytes or more are all recorded (not sampled) with their call site: how many, how many
// bytes and the largest size since the last report, and how many are still live. Reports are
// also appended to ux0:data/mh4u/heapprof.txt (the TTY drops lines under load).
//
// Sites are addresses in the executable (loaded at 0x81000000, not relocated): name them with
//   arm-vita-eabi-addr2line -Cfpe build-vita-rel/src/citra_vita/citra_vita 0x...

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <mutex>
#include <new>
#include <malloc.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>

#include "citra_vita/heap_prof.h"

namespace {

constexpr std::size_t SamplePeriod = 64 * 1024;
constexpr std::size_t PtrSlots = 1 << 15; // sampled live allocations (power of two)
constexpr std::size_t SiteSlots = 1 << 11;
constexpr std::size_t BigMin = 32 * 1024; // recorded exactly from this size up
// The executable's text (the data segment starts above it, at 0x81a60000 in this layout).
constexpr std::uintptr_t TextLo = 0x81000000u, TextHi = 0x81a60000u;

struct PtrSlot {
    std::uintptr_t ptr; // 0 empty, 1 tombstone
    std::uint32_t weight;
    std::uint16_t site;
    std::uint8_t big;
};
struct Site {
    std::uintptr_t ra[3];
    std::int64_t live; // estimated live bytes
    std::int32_t count;
    std::int64_t reported; // live at the last report
    // Allocations of BigMin bytes or more: since the last report, and still live.
    std::uint32_t big_allocs, big_frees, big_max;
    std::int64_t big_bytes;
    std::int32_t big_live;
};

bool g_on = false;
std::size_t g_failed_size = 0; // the request that failed, for the report
/// This file's own code (operator new, Sample...), skipped by the stack scan.
std::uintptr_t g_self_lo = 0, g_self_hi = 0;
// A blocking mutex, not a spinlock: every thread's delete takes it, and on the Vita a
// higher-priority thread spinning on a core starves the lower-priority holder there (the first
// spinlock version froze the game, and the console's network with it).
std::mutex g_lock;
// On the heap, and only when switched on: as .bss they took ~470 KiB from the game partition's
// small remainder outside the heap (thread stacks, GPU blocks) even with the profile off.
PtrSlot* g_ptrs = nullptr;
Site* g_sites = nullptr;
std::size_t g_ptr_used = 0;
std::atomic<std::int64_t> g_total{0};
thread_local std::int64_t t_until_sample = SamplePeriod;

struct Lock {
    std::scoped_lock<std::mutex> lock{g_lock};
};

std::size_t PtrHash(std::uintptr_t p) {
    return ((p >> 3) * 0x9E3779B1u) & (PtrSlots - 1);
}

std::uint16_t SiteIndex(const std::uintptr_t ra[3]) {
    std::size_t h = ((ra[0] * 0x9E3779B1u) ^ (ra[1] * 0x85EBCA77u) ^ ra[2]) & (SiteSlots - 1);
    for (std::size_t n = 0; n < SiteSlots; n++, h = (h + 1) & (SiteSlots - 1)) {
        Site& s = g_sites[h];
        if (s.ra[0] == ra[0] && s.ra[1] == ra[1] && s.ra[2] == ra[2]) {
            return static_cast<std::uint16_t>(h);
        }
        if (s.ra[0] == 0) {
            s.ra[0] = ra[0];
            s.ra[1] = ra[1];
            s.ra[2] = ra[2];
            return static_cast<std::uint16_t>(h);
        }
    }
    return 0; // full: lumped into slot 0
}

/// Return addresses on the stack above this frame: odd (Thumb) words inside the executable's
/// text. A heuristic (stale words show up too) - enough to see past the container code.
__attribute__((noinline)) void StackCallers(std::uintptr_t ra0, std::uintptr_t out[2]) {
    const volatile std::uint32_t* sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    std::size_t n = 0;
    for (std::size_t i = 0; i < 96 && n < 2; i++) {
        const std::uint32_t w = sp[i];
        if ((w & 1) != 0 && w > TextLo && w < TextHi && w != ra0 && (n == 0 || w != out[0]) &&
            (w < g_self_lo || w > g_self_hi)) {
            out[n++] = w;
        }
    }
}

__attribute__((noinline)) void Sample(void* p, std::size_t size, std::uintptr_t ra0) {
    // Weight: the bytes this sample stands for (whole periods crossed); a big allocation is
    // recorded exactly instead, and leaves the sampling countdown alone.
    const bool big = size >= BigMin;
    std::int64_t weight = 0;
    if (big) {
        weight = static_cast<std::int64_t>(size);
    } else {
        while (t_until_sample <= 0) {
            t_until_sample += SamplePeriod;
            weight += SamplePeriod;
        }
    }
    std::uintptr_t ra[3] = {ra0, 0, 0};
    StackCallers(ra0, ra + 1);
    Lock lock;
    if (g_ptr_used >= PtrSlots * 3 / 4) {
        return;
    }
    const std::uint16_t site = SiteIndex(ra);
    std::size_t h = PtrHash(reinterpret_cast<std::uintptr_t>(p));
    while (g_ptrs[h].ptr > 1) {
        h = (h + 1) & (PtrSlots - 1);
    }
    if (g_ptrs[h].ptr == 0) {
        g_ptr_used++;
    }
    g_ptrs[h] = {reinterpret_cast<std::uintptr_t>(p), static_cast<std::uint32_t>(weight), site,
                 static_cast<std::uint8_t>(big)};
    Site& s = g_sites[site];
    s.live += weight;
    s.count++;
    if (big) {
        s.big_allocs++;
        s.big_bytes += weight;
        s.big_max = std::max(s.big_max, static_cast<std::uint32_t>(size));
        s.big_live++;
    }
    g_total.fetch_add(weight, std::memory_order_relaxed);
}

void Forget(void* p) {
    const std::uintptr_t key = reinterpret_cast<std::uintptr_t>(p);
    Lock lock;
    std::size_t h = PtrHash(key);
    for (std::size_t n = 0; n < PtrSlots && g_ptrs[h].ptr != 0;
         n++, h = (h + 1) & (PtrSlots - 1)) {
        if (g_ptrs[h].ptr == key) {
            Site& s = g_sites[g_ptrs[h].site];
            s.live -= g_ptrs[h].weight;
            s.count--;
            if (g_ptrs[h].big) {
                s.big_frees++;
                s.big_live--;
            }
            g_total.fetch_sub(g_ptrs[h].weight, std::memory_order_relaxed);
            g_ptrs[h].ptr = 1;
            return;
        }
    }
}

[[gnu::always_inline]] inline void* Allocate(std::size_t size, std::uintptr_t ra) {
    void* p = std::malloc(size != 0 ? size : 1);
    if (p == nullptr) [[unlikely]] {
        g_failed_size = size;
        HeapProfReport(true);
        throw std::bad_alloc();
    }
    if (g_on) [[unlikely]] {
        if (size >= BigMin) {
            Sample(p, size, ra);
        } else {
            t_until_sample -= static_cast<std::int64_t>(size);
            if (t_until_sample <= 0) {
                Sample(p, size, ra);
            }
        }
    }
    return p;
}

[[gnu::always_inline]] inline void Release(void* p) {
    if (g_on && p != nullptr) [[unlikely]] {
        Forget(p);
    }
    std::free(p);
}

// newlib's malloc (mallocr.c): __malloc_av_ holds the bins; bin i's list head is a fake chunk
// whose fd/bk are av_[2i + 2] and av_[2i + 3], and av_[2] is the top chunk. A chunk is
// {prev_size, size | flags, fd, bk}.
extern "C" void* __malloc_av_[];

struct FragStats {
    std::uint32_t free_bytes = 0, chunks = 0, largest = 0, top = 0;
    std::uint32_t by_size[6] = {}; // free bytes in chunks <4K, <32K, <128K, <512K, <2M, >=2M
};

FragStats WalkFreeLists() {
    FragStats f;
    __malloc_lock(_REENT);
    struct Chunk {
        std::size_t prev_size, size;
        Chunk *fd, *bk;
    };
    const auto bin_at = [](int i) {
        return reinterpret_cast<Chunk*>(reinterpret_cast<char*>(&__malloc_av_[2 * i + 2]) -
                                        2 * sizeof(std::size_t));
    };
    const Chunk* top = bin_at(0)->fd;
    f.top = static_cast<std::uint32_t>(top->size & ~std::size_t{3});
    for (int i = 1; i < 128; i++) {
        const Chunk* bin = bin_at(i);
        std::size_t guard = 0;
        for (const Chunk* c = bin->fd; c != bin && guard < (1u << 22); c = c->fd, guard++) {
            const std::uint32_t size = static_cast<std::uint32_t>(c->size & ~std::size_t{3});
            f.free_bytes += size;
            f.chunks++;
            f.largest = std::max(f.largest, size);
            const int k = size < 4096 ? 0 : size < 32768 ? 1 : size < 131072 ? 2
                        : size < 524288 ? 3 : size < 2097152 ? 4 : 5;
            f.by_size[k] += size;
        }
    }
    __malloc_unlock(_REENT);
    return f;
}

/// Each report line goes to the TTY and into g_text, written to heapprof.txt in one call at the
/// end of the report (the file stays open: opening it each time cost ~150 ms of game time).
SceUID g_out = -1;
char g_text[8192];
std::size_t g_text_len = 0;
__attribute__((format(printf, 1, 2))) void Out(const char* format, ...) {
    char line[256];
    va_list args;
    va_start(args, format);
    const int n = sceClibVsnprintf(line, sizeof(line), format, args);
    va_end(args);
    sceClibPrintf("%s", line);
    const std::size_t len = std::min<std::size_t>(n > 0 ? n : 0, sizeof(line) - 1);
    if (g_text_len + len <= sizeof(g_text)) {
        std::memcpy(g_text + g_text_len, line, len);
        g_text_len += len;
    }
}

} // namespace

void HeapProfInit(bool on) {
    using NewFn = void* (*)(std::size_t);
    using DelFn = void (*)(void*);
    const std::uintptr_t fns[] = {
        reinterpret_cast<std::uintptr_t>(&Sample),
        reinterpret_cast<std::uintptr_t>(&Forget),
        reinterpret_cast<std::uintptr_t>(&StackCallers),
        reinterpret_cast<std::uintptr_t>(static_cast<NewFn>(&::operator new)),
        reinterpret_cast<std::uintptr_t>(static_cast<NewFn>(&::operator new[])),
        reinterpret_cast<std::uintptr_t>(static_cast<DelFn>(&::operator delete)),
        reinterpret_cast<std::uintptr_t>(static_cast<DelFn>(&::operator delete[])),
    };
    g_self_lo = ~std::uintptr_t{0};
    for (const std::uintptr_t f : fns) {
        g_self_lo = std::min(g_self_lo, f & ~std::uintptr_t{1});
        g_self_hi = std::max(g_self_hi, f + 0x200);
    }
    if (on) {
        g_ptrs = static_cast<PtrSlot*>(std::calloc(PtrSlots, sizeof(PtrSlot)));
        g_sites = static_cast<Site*>(std::calloc(SiteSlots, sizeof(Site)));
        on = g_ptrs != nullptr && g_sites != nullptr;
        g_out = sceIoOpen("ux0:data/mh4u/heapprof.txt",
                          SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0666);
    }
    g_on = on;
}

bool HeapProfEnabled() {
    return g_on;
}

void HeapProfReport(bool failure) {
    if (!g_on) {
        if (failure) {
            const struct mallinfo info = mallinfo();
            const FragStats frag = WalkFreeLists();
            sceClibPrintf("[mh4u] heapprof: allocation of %u KiB failed (profile off): arena %u "
                          "KiB, used %u, free %u in %u chunks, largest %u, top %u\n",
                          static_cast<unsigned>(g_failed_size / 1024),
                          static_cast<unsigned>(info.arena / 1024),
                          static_cast<unsigned>(info.uordblks / 1024), frag.free_bytes / 1024,
                          frag.chunks, frag.largest / 1024, frag.top / 1024);
        }
        return;
    }
    // Copy the top sites under the lock, print outside it (printing allocates nothing here, but
    // keeps the lock short).
    const SceUInt64 start = sceKernelGetProcessTimeWide();
    g_text_len = 0;
    // mallinfo and the walk take malloc's lock themselves: before ours, and nothing here
    // allocates while either is held.
    const struct mallinfo info = mallinfo();
    const FragStats frag = WalkFreeLists();
    constexpr std::size_t Top = 10;
    struct BigRow {
        std::uintptr_t ra[3];
        std::uint32_t allocs, frees, max;
        std::int64_t bytes;
        std::int32_t live;
    } big[Top] = {};
    struct Row {
        std::uintptr_t ra[3];
        std::int64_t live, delta;
        std::int32_t count;
    } rows[Top] = {};
    std::size_t sites_used = 0;
    {
        Lock lock;
        for (std::size_t i = 0; i < SiteSlots; i++) {
            Site& s = g_sites[i];
            if (s.ra[0] == 0) {
                continue;
            }
            sites_used++;
            const Row r{{s.ra[0], s.ra[1], s.ra[2]}, s.live, s.live - s.reported, s.count};
            s.reported = s.live;
            // Big allocations, ranked by bytes allocated since the last report (the churn
            // that leaves holes), then the window restarts.
            const BigRow b{{s.ra[0], s.ra[1], s.ra[2]}, s.big_allocs, s.big_frees, s.big_max,
                           s.big_bytes, s.big_live};
            s.big_allocs = s.big_frees = s.big_max = 0;
            s.big_bytes = 0;
            for (std::size_t k = 0; k < Top && (b.bytes > 0 || b.live > 0); k++) {
                if (b.bytes > big[k].bytes ||
                    (b.bytes == big[k].bytes && b.live > big[k].live)) {
                    for (std::size_t m = Top - 1; m > k; m--) {
                        big[m] = big[m - 1];
                    }
                    big[k] = b;
                    break;
                }
            }
            // Ranked by live bytes: a leak climbs to the top and stays there.
            for (std::size_t k = 0; k < Top; k++) {
                if (r.live > rows[k].live) {
                    for (std::size_t m = Top - 1; m > k; m--) {
                        rows[m] = rows[m - 1];
                    }
                    rows[k] = r;
                    break;
                }
            }
        }
    }
    if (failure) {
        Out("[mh4u] heapprof: ALLOCATION OF %u KiB FAILED\n",
            static_cast<unsigned>(g_failed_size / 1024));
    }
    Out("[mh4u] heapprof t%u%s: sampled live %d KiB, %u sites, %u ptrs\n",
                  static_cast<unsigned>(sceKernelGetProcessTimeWide() / 1000000),
                  failure ? " (ALLOCATION FAILED)" : "",
                  static_cast<int>(g_total.load(std::memory_order_relaxed) / 1024),
                  static_cast<unsigned>(sites_used), static_cast<unsigned>(g_ptr_used));
    for (std::size_t k = 0; k < Top && rows[k].live > 0; k++) {
        Out("[mh4u] heapprof  %6d KiB %+5d n%-5d %08x %08x %08x\n",
                      static_cast<int>(rows[k].live / 1024), static_cast<int>(rows[k].delta / 1024),
                      static_cast<int>(rows[k].count), static_cast<unsigned>(rows[k].ra[0]),
                      static_cast<unsigned>(rows[k].ra[1]), static_cast<unsigned>(rows[k].ra[2]));
    }
    Out("[mh4u] heapfrag arena %u KiB, used %u, free %u in %u chunks, largest %u, top %u; "
        "free by chunk <4K %u <32K %u <128K %u <512K %u <2M %u >=2M %u\n",
        static_cast<unsigned>(info.arena / 1024), static_cast<unsigned>(info.uordblks / 1024),
        frag.free_bytes / 1024, frag.chunks, frag.largest / 1024, frag.top / 1024,
        frag.by_size[0] / 1024, frag.by_size[1] / 1024, frag.by_size[2] / 1024,
        frag.by_size[3] / 1024, frag.by_size[4] / 1024, frag.by_size[5] / 1024);
    for (std::size_t k = 0; k < Top && (big[k].bytes > 0 || big[k].live > 0); k++) {
        Out("[mh4u] heapbig %6d KiB n%-4u f%-4u max %5u KiB live %-4d %08x %08x %08x\n",
            static_cast<int>(big[k].bytes / 1024), big[k].allocs, big[k].frees,
            big[k].max / 1024, static_cast<int>(big[k].live),
            static_cast<unsigned>(big[k].ra[0]), static_cast<unsigned>(big[k].ra[1]),
            static_cast<unsigned>(big[k].ra[2]));
    }
    Out("[mh4u] heapprof report took %u us\n",
        static_cast<unsigned>(sceKernelGetProcessTimeWide() - start));
    if (g_out >= 0 && g_text_len > 0) {
        sceIoWrite(g_out, g_text, g_text_len);
    }
}

void* operator new(std::size_t size) {
    return Allocate(size, reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)));
}
void* operator new[](std::size_t size) {
    return Allocate(size, reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)));
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    void* p = std::malloc(size != 0 ? size : 1);
    return p;
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    void* p = std::malloc(size != 0 ? size : 1);
    return p;
}
void operator delete(void* p) noexcept {
    Release(p);
}
void operator delete[](void* p) noexcept {
    Release(p);
}
void operator delete(void* p, std::size_t) noexcept {
    Release(p);
}
void operator delete[](void* p, std::size_t) noexcept {
    Release(p);
}
void operator delete(void* p, const std::nothrow_t&) noexcept {
    Release(p);
}
void operator delete[](void* p, const std::nothrow_t&) noexcept {
    Release(p);
}
