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
// Sites are addresses in the executable (loaded at 0x81000000, not relocated): name them with
//   arm-vita-eabi-addr2line -Cfpe build-vita-rel/src/citra_vita/citra_vita 0x...

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <new>
#include <psp2/kernel/clib.h>

#include "citra_vita/heap_prof.h"

namespace {

constexpr std::size_t SamplePeriod = 64 * 1024;
constexpr std::size_t PtrSlots = 1 << 15; // sampled live allocations (power of two)
constexpr std::size_t SiteSlots = 1 << 11;
// The executable's text (the data segment starts above it, at 0x81a60000 in this layout).
constexpr std::uintptr_t TextLo = 0x81000000u, TextHi = 0x81a60000u;

struct PtrSlot {
    std::uintptr_t ptr; // 0 empty, 1 tombstone
    std::uint32_t weight;
    std::uint16_t site;
};
struct Site {
    std::uintptr_t ra[3];
    std::int64_t live; // estimated live bytes
    std::int32_t count;
    std::int64_t reported; // live at the last report
};

bool g_on = false;
/// This file's own code (operator new, Sample...), skipped by the stack scan.
std::uintptr_t g_self_lo = 0, g_self_hi = 0;
// A blocking mutex, not a spinlock: every thread's delete takes it, and on the Vita a
// higher-priority thread spinning on a core starves the lower-priority holder there (the first
// spinlock version froze the game, and the console's network with it).
std::mutex g_lock;
PtrSlot g_ptrs[PtrSlots];
Site g_sites[SiteSlots];
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
    // Weight: the bytes this sample stands for (whole periods crossed).
    std::int64_t weight = 0;
    while (t_until_sample <= 0) {
        t_until_sample += SamplePeriod;
        weight += SamplePeriod;
    }
    (void)size;
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
    g_ptrs[h] = {reinterpret_cast<std::uintptr_t>(p), static_cast<std::uint32_t>(weight), site};
    g_sites[site].live += weight;
    g_sites[site].count++;
    g_total.fetch_add(weight, std::memory_order_relaxed);
}

void Forget(void* p) {
    const std::uintptr_t key = reinterpret_cast<std::uintptr_t>(p);
    Lock lock;
    std::size_t h = PtrHash(key);
    for (std::size_t n = 0; n < PtrSlots && g_ptrs[h].ptr != 0;
         n++, h = (h + 1) & (PtrSlots - 1)) {
        if (g_ptrs[h].ptr == key) {
            g_sites[g_ptrs[h].site].live -= g_ptrs[h].weight;
            g_sites[g_ptrs[h].site].count--;
            g_total.fetch_sub(g_ptrs[h].weight, std::memory_order_relaxed);
            g_ptrs[h].ptr = 1;
            return;
        }
    }
}

[[gnu::always_inline]] inline void* Allocate(std::size_t size, std::uintptr_t ra) {
    void* p = std::malloc(size != 0 ? size : 1);
    if (p == nullptr) [[unlikely]] {
        HeapProfReport(true);
        throw std::bad_alloc();
    }
    if (g_on) [[unlikely]] {
        t_until_sample -= static_cast<std::int64_t>(size);
        if (t_until_sample <= 0) {
            Sample(p, size, ra);
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
    g_on = on;
}

bool HeapProfEnabled() {
    return g_on;
}

void HeapProfReport(bool failure) {
    if (!g_on) {
        if (failure) {
            sceClibPrintf("[mh4u] heapprof: allocation failed (profile off)\n");
        }
        return;
    }
    // Copy the top sites under the lock, print outside it (printing allocates nothing here, but
    // keeps the lock short).
    constexpr std::size_t Top = 10;
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
    sceClibPrintf("[mh4u] heapprof%s: sampled live %d KiB, %u sites, %u ptrs\n",
                  failure ? " (ALLOCATION FAILED)" : "",
                  static_cast<int>(g_total.load(std::memory_order_relaxed) / 1024),
                  static_cast<unsigned>(sites_used), static_cast<unsigned>(g_ptr_used));
    for (std::size_t k = 0; k < Top && rows[k].live > 0; k++) {
        sceClibPrintf("[mh4u] heapprof  %6d KiB %+5d n%-5d %08x %08x %08x\n",
                      static_cast<int>(rows[k].live / 1024), static_cast<int>(rows[k].delta / 1024),
                      static_cast<int>(rows[k].count), static_cast<unsigned>(rows[k].ra[0]),
                      static_cast<unsigned>(rows[k].ra[1]), static_cast<unsigned>(rows[k].ra[2]));
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
