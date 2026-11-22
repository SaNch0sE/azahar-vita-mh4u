// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#ifdef VITA_DIAGNOSTICS

#include <cstdint>
#include <reent.h>

#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include "common/heap_check.h"

// newlib's malloc (mallocr.c): its lock, where the heap starts and its bins, av_[2] being the top
// chunk (the heap's free end).
extern "C" {
void __malloc_lock(struct _reent*);
void __malloc_unlock(struct _reent*);
extern char* __malloc_sbrk_base;
extern void* __malloc_av_[];
}

namespace Common::HeapCheck {

std::atomic<bool> enabled{false};
std::atomic<bool> full{false};

namespace {

std::atomic<std::uint32_t> checks{0};
std::atomic<std::uint64_t> check_us{0};

std::uint32_t Word(std::uintptr_t at) {
    return *reinterpret_cast<const volatile std::uint32_t*>(at);
}

[[noreturn]] void Damaged(const char* where, const char* what, std::uintptr_t chunk,
                          std::uintptr_t value) {
    sceClibPrintf("[azahar] heapcheck: damaged at %s: %s, chunk %08x (%08x), thread %08x, check "
                  "%u\n",
                  where, what, static_cast<unsigned>(chunk), static_cast<unsigned>(value),
                  static_cast<unsigned>(sceKernelGetThreadId()),
                  static_cast<unsigned>(checks.load(std::memory_order_relaxed)));
    for (std::uintptr_t row = (chunk & ~std::uintptr_t{15}) - 32; row < (chunk & ~std::uintptr_t{15}) + 48;
         row += 16) {
        sceClibPrintf("[azahar] heapcheck: %08x: %08x %08x %08x %08x\n",
                      static_cast<unsigned>(row), Word(row), Word(row + 4), Word(row + 8),
                      Word(row + 12));
    }
    // The console's lines out, then a fault with the heap and every thread as they are: the
    // the system stops the process for a core dump (pdump). The malloc lock stays held, so no
    // other thread changes the heap meanwhile.
    sceKernelDelayThread(200000);
    *reinterpret_cast<volatile std::uint32_t*>(4) = 0;
    __builtin_unreachable();
}

} // namespace

void CheckNow(const char* where) {
    const std::uint64_t start = sceKernelGetProcessTimeWide();
    __malloc_lock(_REENT);
    const std::uintptr_t base = (reinterpret_cast<std::uintptr_t>(__malloc_sbrk_base) + 7) & ~std::uintptr_t{7};
    const std::uintptr_t top = reinterpret_cast<std::uintptr_t>(__malloc_av_[2]);
    // A bin's header is a fake chunk 8 bytes before its pair of links (bin_at).
    const std::uintptr_t bins_lo = reinterpret_cast<std::uintptr_t>(&__malloc_av_[0]);
    const std::uintptr_t bins_hi = reinterpret_cast<std::uintptr_t>(&__malloc_av_[256]);
    const auto link_ok = [&](std::uintptr_t link) {
        return (link >= base && link < top && (link & 7) == 0) ||
               (link >= bins_lo && link < bins_hi && ((link - bins_lo) & 7) == 0);
    };
    if (top < base || (top & 7) != 0) {
        Damaged(where, "the top chunk", top, base);
    }
    // Each bin's list of free chunks (bin 1 the last remainder, 2 on by size): each chunk in the
    // heap and aligned, its back link the chunk before it, the next chunk's record of it free and
    // its size.
    for (std::uintptr_t bin = bins_lo + 8; bin < bins_hi; bin += 8) {
        std::uintptr_t previous = bin;
        std::uint32_t steps = 0;
        for (std::uintptr_t p = Word(bin + 8); p != bin; p = Word(p + 8)) {
            if (p < base || p >= top || (p & 7) != 0) {
                Damaged(where, "a bin's list leaves the heap", previous, p);
            }
            if (Word(p + 12) != previous) {
                Damaged(where, "a free chunk's back link in its bin", p, Word(p + 12));
            }
            const std::uint32_t size = Word(p + 4) & ~3u;
            if (size < 16 || (size & 7) != 0 || p + size > top) {
                Damaged(where, "a free chunk's size in its bin", p, Word(p + 4));
            }
            if ((Word(p + size + 4) & 1u) != 0 || Word(p + size) != size) {
                Damaged(where, "a free chunk in a bin that the next chunk calls in use", p,
                        Word(p + size + 4));
            }
            if (++steps > 1000000) {
                Damaged(where, "a bin's list that does not end", bin, p);
            }
            previous = p;
        }
        if (Word(bin + 12) != previous) {
            Damaged(where, "a bin's last link", bin, Word(bin + 12));
        }
    }
    if (!full.load(std::memory_order_relaxed)) {
        __malloc_unlock(_REENT);
        checks.fetch_add(1, std::memory_order_relaxed);
        check_us.fetch_add(sceKernelGetProcessTimeWide() - start, std::memory_order_relaxed);
        return;
    }
    bool previous_free = false;
    for (std::uintptr_t p = base; p < top;) {
        const std::uint32_t size = Word(p + 4) & ~3u;
        if (size < 16 || (size & 7) != 0 || p + size > top) {
            Damaged(where, "a chunk's size", p, Word(p + 4));
        }
        const std::uintptr_t next = p + size;
        const bool free = (Word(next + 4) & 1u) == 0;
        if (free) {
            if (previous_free) {
                Damaged(where, "two free chunks in a row", p, size);
            }
            if (Word(next) != size) {
                Damaged(where, "a free chunk's size in the next chunk", p, Word(next));
            }
            const std::uintptr_t fd = Word(p + 8);
            const std::uintptr_t bk = Word(p + 12);
            if (!link_ok(fd)) {
                Damaged(where, "a free chunk's forward link", p, fd);
            }
            if (!link_ok(bk)) {
                Damaged(where, "a free chunk's back link", p, bk);
            }
            if (Word(fd + 12) != p) {
                Damaged(where, "the forward neighbour's back link", p, fd);
            }
            if (Word(bk + 8) != p) {
                Damaged(where, "the back neighbour's forward link", p, bk);
            }
        }
        previous_free = free;
        p = next;
    }
    __malloc_unlock(_REENT);
    checks.fetch_add(1, std::memory_order_relaxed);
    check_us.fetch_add(sceKernelGetProcessTimeWide() - start, std::memory_order_relaxed);
}

void PrintStats() {
    const std::uint32_t n = checks.load(std::memory_order_relaxed);
    const std::uint64_t us = check_us.load(std::memory_order_relaxed);
    sceClibPrintf("[azahar] heapcheck: %u checks, %u us each on average\n", static_cast<unsigned>(n),
                  static_cast<unsigned>(n != 0 ? us / n : 0));
}

} // namespace Common::HeapCheck

#endif
