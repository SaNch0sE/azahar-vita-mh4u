// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <atomic>

/**
 * A check of newlib's heap at chosen points, for finding what damages it: each free chunk in its
 * bin's list, its links and what the chunk after it says of it (and with `full`, every chunk's
 * size and each free one's neighbours). At the first damage it prints the
 * point, the chunk and what is wrong, and stops the process with the heap as it is (a fault the
 * the system keeps for a core dump). Off unless the command channel turns it on
 * (`heapcheck`); off, a check is one load. Diagnostics builds only: elsewhere a check is nothing.
 */
namespace Common::HeapCheck {

#ifdef VITA_DIAGNOSTICS

extern std::atomic<bool> enabled;
/// Every chunk walked too, not only the free ones in their bins: slow (0.7 ms a check in PSMD).
extern std::atomic<bool> full;

void CheckNow(const char* where);

inline void Check(const char* where) {
    if (enabled.load(std::memory_order_relaxed)) [[unlikely]] {
        CheckNow(where);
    }
}

/// The checks so far and their average time, for the console.
void PrintStats();
#else
inline void Check(const char*) {}
#endif

} // namespace Common::HeapCheck
