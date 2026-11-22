// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <atomic>
#include "common/common_types.h"

namespace Core {

/**
 * The emulation threads ("hosts"). Host 0 runs emulated core 0 and everything the cores share:
 * the services, the GPU, the HLE kernel's objects. Host 1, when there is one, runs the other
 * emulated cores and owns their schedulers and timers. Any other thread (the DSP's, an async
 * section's) works as host 0's, under the HLE lock as before.
 *
 * Each host keeps its own "current" core, process and timer. Which host a thread is comes from
 * its thread pointer register: thread_local is emulated TLS on the Vita (a call and a key lookup
 * on every access).
 */
constexpr u32 NumHosts = 2;

/// The second emulation thread's TPIDRURO while it runs (0: none).
extern u32 host1_tp;
/// Whether the second emulation thread runs the emulated cores other than core 0.
extern std::atomic<bool> multi_host;
/// The second emulation thread waits while host 0 runs one of its svcs (System::SecondHostSvc):
/// its cores' state is host 0's meanwhile.
extern std::atomic<bool> host1_parked;

[[nodiscard]] inline u32 ThisHost() {
#if defined(__vita__)
    u32 tp;
    asm("mrc p15, 0, %0, c13, c0, 3" : "=r"(tp));
    return tp == host1_tp ? 1u : 0u; // a thread's pointer is never 0
#else
    return 0;
#endif
}

/// The host that runs emulated core `core` and owns its scheduler and timer.
[[nodiscard]] inline u32 HostOfCore(u32 core) {
    return core != 0 && multi_host.load(std::memory_order_relaxed) ? 1u : 0u;
}

/// Whether the calling thread may change core `core`'s scheduler now: its own host's, or host
/// 0's while the second thread waits for it.
[[nodiscard]] inline bool OwnsCore(u32 core) {
    const u32 host = ThisHost();
    return HostOfCore(core) == host ||
           (host == 0 && host1_parked.load(std::memory_order_acquire));
}

} // namespace Core
