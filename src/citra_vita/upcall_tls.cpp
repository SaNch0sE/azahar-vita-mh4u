// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// Thread-local storage and malloc's lock for the Vita native core's upcalls: the emulator's C++
// called from the guest's world runs with the guest's TPIDRURO, a 3DS address below 0x40000000
// (user mode cannot write the register). The link wraps two of newlib's calls (CMakeLists.txt):
// - sceKernelGetTLSAddr, through which newlib's per-thread state, pthread's keys and the C++
//   thread_locals (-femulated-tls) find their slots: inside an upcall, the emulation thread's.
// - __malloc_lock and __malloc_unlock: a recursive lock of our own, its owner the thread's
//   TPIDRURO, in place of SceLibKernel's lightweight mutex, which takes the thread's identity
//   from TPIDRURO itself.

#include <atomic>
#include <cstdint>
#include <psp2/kernel/threadmgr.h>
#include <reent.h>

extern "C" {

/// The emulation thread's TPIDRURO, recorded before each slice (ARM_VitaNative::RunSlice).
extern uint32_t azahar_upcall_host_tp;

void* __real_sceKernelGetTLSAddr(int key);

namespace {

uint32_t ThreadPointer() {
    uint32_t tp;
    asm volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(tp));
    return tp;
}

/// Inside an upcall: every Vita thread's TPIDRURO is above 0x40000000.
bool InUpcall(uint32_t tp) {
    return tp < 0x40000000u;
}

std::atomic<uint32_t> malloc_owner{0};
uint32_t malloc_depth = 0;

} // Anonymous namespace

void* __wrap_sceKernelGetTLSAddr(int key) {
    void* const slot = __real_sceKernelGetTLSAddr(key);
    const uint32_t tp = ThreadPointer();
    if (!InUpcall(tp)) {
        return slot;
    }
    return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(slot) - tp + azahar_upcall_host_tp);
}

void __wrap___malloc_lock(struct _reent*) {
    const uint32_t me = ThreadPointer();
    if (malloc_owner.load(std::memory_order_relaxed) == me) {
        malloc_depth++;
        return;
    }
    for (uint32_t spins = 0;; spins++) {
        uint32_t expected = 0;
        if (malloc_owner.compare_exchange_weak(expected, me, std::memory_order_acquire,
                                               std::memory_order_relaxed)) {
            break;
        }
        // A Vita thread lets the holder run, which may be waiting for this core; an upcall
        // spins, its holder on another core (this one runs the guest).
        if (!InUpcall(me) && spins >= 64) {
            sceKernelDelayThread(1);
            spins = 0;
        }
    }
    malloc_depth = 1;
}

void __wrap___malloc_unlock(struct _reent*) {
    if (--malloc_depth == 0) {
        malloc_owner.store(0, std::memory_order_release);
    }
}

} // extern "C"
