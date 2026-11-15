// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

/// heapprof: see heap_prof.cpp. Switched on once at start-up (before other threads).
void HeapProfInit(bool on);
bool HeapProfEnabled();
/// Prints the call sites holding the most sampled live heap, with their change since the last
/// report. `failure`: called from a failed allocation.
void HeapProfReport(bool failure = false);
