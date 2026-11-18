// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <cstddef>

/// heapprof: see heap_prof.cpp. Switched on once at start-up (before other threads).
void HeapProfInit(bool on);
bool HeapProfEnabled();
/// Keeps `bytes` of heap aside; the first failed operator new frees it and tries again.
void HeapReserveInit(std::size_t bytes);
/// True once the reserve was used (or never set).
bool HeapReserveReleased();
/// Prints the call sites holding the most sampled live heap, with their change since the last
/// report. `failure`: called from a failed allocation.
void HeapProfReport(bool failure = false);
