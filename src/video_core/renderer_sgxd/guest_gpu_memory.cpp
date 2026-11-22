// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/assert.h"
#include "common/logging/log.h"
#include "core/memory.h"
#include "video_core/renderer_gxm/guest_gpu_memory.h"

#ifdef __vita__
#include "sgxd.h"
#endif

namespace SgxdRenderer {

GuestGpuMemory::GuestGpuMemory([[maybe_unused]] Memory::MemorySystem& memory) {
#ifdef __vita__
    const u8* host = memory.GetPhysicalPointer(Memory::VRAM_PADDR);
    // CDRAM is uncached for the CPU and the GPU does not snoop it: no CACHECONSISTENT.
    const int ret = sgxdMap(Memory::VRAM_PADDR, host, Memory::VRAM_SIZE, 0);
    ASSERT_MSG(ret >= 0, "sgxd: guest VRAM ({}) was not mapped at GPU {:#010x}: {:#010x}",
               fmt::ptr(host), static_cast<u32>(Memory::VRAM_PADDR), static_cast<u32>(ret));
    vram = host;
    LOG_INFO(Render, "sgxd: guest VRAM {} mapped at GPU {:#010x}, {} KiB", fmt::ptr(host),
             static_cast<u32>(Memory::VRAM_PADDR), static_cast<u32>(Memory::VRAM_SIZE) >> 10);

    // FCRAM is ordinary cached memory (malloc'd, page-aligned: HostSharedMemory). The GPU does
    // not snoop the CPU's cache, so what the guest wrote there is cleaned before a kick reads it
    // (SgxdBackend::ReadsCached).
    u8* const fcram_host = memory.GetFCRAMPointer(0);
    const int fcram_ret = sgxdMap(Memory::FCRAM_PADDR, fcram_host, Memory::FCRAM_SIZE, 0);
    ASSERT_MSG(fcram_ret >= 0, "sgxd: guest FCRAM ({}) was not mapped at GPU {:#010x}: {:#010x}",
               fmt::ptr(fcram_host), static_cast<u32>(Memory::FCRAM_PADDR),
               static_cast<u32>(fcram_ret));
    fcram = fcram_host;
    LOG_INFO(Render, "sgxd: guest FCRAM {} mapped at GPU {:#010x}, {} KiB", fmt::ptr(fcram_host),
             static_cast<u32>(Memory::FCRAM_PADDR), static_cast<u32>(Memory::FCRAM_SIZE) >> 10);
    LOG_INFO(Render, "sgxd: FCRAM the GPU writes and shows moves to CDRAM: {}",
             enabled ? fmt::format("{} KiB pool at {}", PoolSize >> 10,
                                   fmt::ptr(pool->Data()))
                     : std::string{"off"});
#endif
}

GuestGpuMemory::~GuestGpuMemory() {
#ifdef __vita__
    if (vram != nullptr) {
        const int ret = sgxdUnmap(Memory::VRAM_PADDR, Memory::VRAM_SIZE);
        if (ret < 0) {
            LOG_ERROR(Render, "sgxd: guest VRAM unmap failed: {:#010x}", static_cast<u32>(ret));
        }
    }
    if (fcram != nullptr) {
        const int ret = sgxdUnmap(Memory::FCRAM_PADDR, Memory::FCRAM_SIZE);
        if (ret < 0) {
            LOG_ERROR(Render, "sgxd: guest FCRAM unmap failed: {:#010x}", static_cast<u32>(ret));
        }
    }
#endif
}

} // namespace SgxdRenderer
