// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include "common/common_types.h"

namespace Memory {
class MemorySystem;
}

namespace SgxdRenderer {

/**
 * Guest memory in the GPU's address space at its PICA physical address, so that a PICA address
 * is a GPU address and the draw path translates nothing. Guest VRAM
 * (CDRAM, uncached for the CPU) at 0x18000000, guest FCRAM (cached: cleaned before a kick
 * reads it) at 0x20000000. The GPU must be idle when this is made and
 * destroyed: sgxd invalidates its MMU then. Nothing on host builds.
 */
class GuestGpuMemory {
public:
    explicit GuestGpuMemory(Memory::MemorySystem& memory);
    ~GuestGpuMemory();

    GuestGpuMemory(const GuestGpuMemory&) = delete;
    GuestGpuMemory& operator=(const GuestGpuMemory&) = delete;

private:
    const u8* vram{};
    const u8* fcram{};
};

} // namespace SgxdRenderer
