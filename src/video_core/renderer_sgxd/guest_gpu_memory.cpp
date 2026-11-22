// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <array>
#include <chrono>
#include <tuple>
#include <vector>
#include "common/assert.h"
#include "common/host_shared_memory.h"
#include "common/logging/log.h"
#include "core/memory.h"
#include "video_core/renderer_sgxd/guest_gpu_memory.h"
#include "video_core/renderer_sgxd/sgxd_backend.h"

#include "azahar_native.h"
#include "common/host_clock.h"
#include "sgxd.h"
#include "sgxd_ptla.h"
#ifdef VITA_DIAGNOSTICS
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#endif

namespace SgxdRenderer {

namespace {
constexpr u32 PageSize = 0x1000;
// The CDRAM pool for moved FCRAM pages; a frame can use about 8 MiB. It grows by GrowSize
// blocks, CDRAM or uncached main memory, up to what azaharReback takes. Full with nothing to put
// back stops the emulator: FCRAM the GPU writes, reads and shows is never left cached.
constexpr u32 PoolSize = 8 * 1024 * 1024;
constexpr u32 GrowSize = 1024 * 1024;
constexpr u32 MaxPoolSize = 32 * 1024 * 1024;
// A pool short of free pages puts back the pages no GPU use stamped for a second (at vblank).
constexpr u32 IdleFrames = 60;
constexpr u32 MaxPagesPerMove = 256;
} // namespace

GuestGpuMemory::GuestGpuMemory([[maybe_unused]] Memory::MemorySystem& memory) {
    const u8* host = memory.GetPhysicalPointer(Memory::VRAM_PADDR);
    // CDRAM is uncached for the CPU and the GPU does not snoop it: no CACHECONSISTENT.
    const int ret = sgxdMap(Memory::VRAM_PADDR, host, Memory::VRAM_SIZE, 0);
    ASSERT_MSG(ret >= 0, "sgxd: guest VRAM ({}) was not mapped at GPU {:#010x}: {:#010x}",
               fmt::ptr(host), static_cast<u32>(Memory::VRAM_PADDR), static_cast<u32>(ret));
    vram = host;
    LOG_INFO(Render, "sgxd: guest VRAM {} mapped at GPU {:#010x}, {} KiB", fmt::ptr(host),
             static_cast<u32>(Memory::VRAM_PADDR), static_cast<u32>(Memory::VRAM_SIZE) >> 10);
    // The filler past VRAM's end (PastVramBytes): the untile reads on into it.
    alignas(0x1000) static u8 past_vram_block[PastVramBlock];
    for (u32 at = 0; at < PastVramBytes; at += PastVramBlock) {
        const u32 gpu = Memory::VRAM_PADDR + Memory::VRAM_SIZE + at;
        const int past_ret = sgxdMap(gpu, past_vram_block, PastVramBlock, 0);
        ASSERT_MSG(past_ret >= 0, "sgxd: the filler past guest VRAM was not mapped at GPU "
                                  "{:#010x}: {:#010x}",
                   gpu, static_cast<u32>(past_ret));
    }
    past_vram = true;

    // FCRAM is cached and the GPU does not snoop: cleaned before a kick reads it
    // (SgxdBackend::ReadsCached), except the pages moved to CDRAM.
    u8* const fcram_host = memory.GetFCRAMPointer(0);
    const int fcram_ret = sgxdMap(Memory::FCRAM_PADDR, fcram_host, Memory::FCRAM_SIZE, 0);
    ASSERT_MSG(fcram_ret >= 0, "sgxd: guest FCRAM ({}) was not mapped at GPU {:#010x}: {:#010x}",
               fmt::ptr(fcram_host), static_cast<u32>(Memory::FCRAM_PADDR),
               static_cast<u32>(fcram_ret));
    fcram = fcram_host;
    LOG_INFO(Render, "sgxd: guest FCRAM {} mapped at GPU {:#010x}, {} KiB", fmt::ptr(fcram_host),
             static_cast<u32>(Memory::FCRAM_PADDR), static_cast<u32>(Memory::FCRAM_SIZE) >> 10);

    static_assert(FcramPages == Memory::FCRAM_SIZE / PageSize);
    for (auto& word : moved_bits) {
        word.store(0, std::memory_order_relaxed);
    }
    last_use = std::make_unique<std::atomic<u32>[]>(Memory::FCRAM_SIZE / PageSize);
    // Its whole reach at once: GrowPool's resizes on the render thread never move it under
    // EndFrame's walk on the emulation thread.
    pool_owner.reserve(MaxPoolSize / PageSize);
    pool_owner.assign(PoolSize / PageSize, NoPage);
    pool_pages.store(PoolSize / PageSize, std::memory_order_release);
    pool_fence.assign(PoolSize / PageSize, 0);
    pool.push_back(std::make_unique<Common::HostSharedMemory>(
        PoolSize, "azahar-fcram-cdram", Common::HostSharedMemory::Placement::GpuLocal));
    pool_starts.push_back(0);
    // FCRAM's own block under the module's tables, so that its pages can move.
    const int adopt =
        azaharAdopt(static_cast<u32>(reinterpret_cast<uintptr_t>(fcram_host)), Memory::FCRAM_SIZE);
    if (adopt < 0) {
        SgxdDrainKernelLog();
    }
    ASSERT_MSG(adopt >= 0, "sgxd: guest FCRAM not adopted ({:#010x})", static_cast<u32>(adopt));
    adopted = true;
    LOG_INFO(Render, "sgxd: FCRAM the GPU writes and shows moves to CDRAM: {} KiB pool at {}",
             PoolSize >> 10, fmt::ptr(pool[0]->Data()));
}

u8* GuestGpuMemory::PoolPage(u32 slot) const {
    const auto at = std::upper_bound(pool_starts.begin(), pool_starts.end(), slot) - 1;
    return pool[at - pool_starts.begin()]->Data() + (slot - *at) * PageSize;
}

bool GuestGpuMemory::GrowPool() {
    const u32 pages = static_cast<u32>(pool_owner.size());
    if (pages * PageSize + GrowSize > MaxPoolSize) {
        return false;
    }
    pool.push_back(std::make_unique<Common::HostSharedMemory>(
        GrowSize, "azahar-fcram-cdram", Common::HostSharedMemory::Placement::GpuLocalOrUncached));
    pool_starts.push_back(pages);
    pool_owner.resize(pages + GrowSize / PageSize, NoPage);
    pool_pages.store(pages + GrowSize / PageSize, std::memory_order_release);
    pool_fence.resize(pages + GrowSize / PageSize, 0);
    LOG_INFO(Render, "sgxd: the CDRAM pool grows to {} KiB", (pages * PageSize + GrowSize) >> 10);
    return true;
}

GuestGpuMemory::~GuestGpuMemory() {
    // Sony's descriptors go back before the pool's block and FCRAM's do.
    if (adopted) {
        const int ret = azaharRebackRestore();
        if (ret < 0) {
            LOG_ERROR(Render, "sgxd: moved FCRAM pages not restored: {:#010x}",
                      static_cast<u32>(ret));
        }
    }
    if (vram != nullptr) {
        const int ret = sgxdUnmap(Memory::VRAM_PADDR, Memory::VRAM_SIZE);
        if (ret < 0) {
            LOG_ERROR(Render, "sgxd: guest VRAM unmap failed: {:#010x}", static_cast<u32>(ret));
        }
    }
    if (past_vram) {
        const int ret = sgxdUnmap(Memory::VRAM_PADDR + Memory::VRAM_SIZE, PastVramBytes);
        if (ret < 0) {
            LOG_ERROR(Render, "sgxd: the filler past guest VRAM: unmap failed: {:#010x}",
                      static_cast<u32>(ret));
        }
    }
    if (fcram != nullptr) {
        const int ret = sgxdUnmap(Memory::FCRAM_PADDR, Memory::FCRAM_SIZE);
        if (ret < 0) {
            LOG_ERROR(Render, "sgxd: guest FCRAM unmap failed: {:#010x}", static_cast<u32>(ret));
        }
    }
}

bool GuestGpuMemory::IsUncached(PAddr addr, u32 size) const {
    if (size == 0) {
        return true;
    }
    if (addr >= Memory::VRAM_PADDR && addr - Memory::VRAM_PADDR + u64{size} <= Memory::VRAM_SIZE) {
        return true;
    }
    if (addr < Memory::FCRAM_PADDR || addr - Memory::FCRAM_PADDR + u64{size} > Memory::FCRAM_SIZE ||
        !last_use) {
        return false;
    }
    const u32 first = (addr - Memory::FCRAM_PADDR) / PageSize;
    const u32 last = (addr - Memory::FCRAM_PADDR + size - 1) / PageSize;
    const u32 now = frame.load(std::memory_order_relaxed);
    for (u32 page = first; page <= last; page++) {
        if (!Moved(page)) {
            return false;
        }
        last_use[page].store(now, std::memory_order_relaxed);
    }
    return true;
}

bool GuestGpuMemory::IsStale(u32 page) const {
    return frame.load(std::memory_order_relaxed) - last_use[page].load(std::memory_order_relaxed) >=
           StaleFrames;
}

std::pair<u32, u32> GuestGpuMemory::FreePoolRun(u32 want) const {
    const u32 pages = static_cast<u32>(pool_owner.size());
    const auto free = [&](u32 slot) {
        return pool_owner[slot] == NoPage && hooks.reached(pool_fence[slot]);
    };
    for (u32 first = 0; first < pages; first++) {
        if (!free(first)) {
            continue;
        }
        // Within one block of the pool: azaharReback takes a run at consecutive addresses.
        u32 length = 1;
        while (length < want && first + length < pages && free(first + length) &&
               !std::binary_search(pool_starts.begin(), pool_starts.end(), first + length)) {
            length++;
        }
        return {first, length};
    }
    return {0, 0};
}

void GuestGpuMemory::WaitPoolFence() {
    bool any = false;
    u32 oldest = 0;
    for (u32 slot = 0; slot < pool_owner.size(); slot++) {
        const u32 fence = pool_fence[slot];
        if (pool_owner[slot] != NoPage || hooks.reached(fence)) {
            continue;
        }
        if (!any || static_cast<s32>(fence - oldest) < 0) {
            oldest = fence;
            any = true;
        }
    }
    if (any) {
        hooks.wait(oldest);
    }
}

u32 GuestGpuMemory::RemapGpu(u32 page, u32 count) {
    std::array<u32, SGXD_REMAP_MAX> pte_va, pte;
    u32 fence = 0;
    for (u32 k = 0; k < count;) {
        u32 n = 0;
        for (; n < SGXD_REMAP_MAX && k < count; n++, k++) {
            const PAddr addr = Memory::FCRAM_PADDR + (page + k) * PageSize;
            u32 out[2];
            const int r = sgxdPte(addr, fcram + (page + k) * PageSize, out);
            ASSERT_MSG(r == 0, "sgxd: no GPU remap of FCRAM {:#010x}: {:#010x}", addr,
                       static_cast<u32>(r));
            pte_va[n] = out[0];
            pte[n] = out[1];
        }
        fence = hooks.remap(pte_va.data(), pte.data(), n);
    }
    return fence;
}

bool GuestGpuMemory::PoolLow() const {
    const u32 pages = pool_pages.load(std::memory_order_relaxed);
    return pool_used.load(std::memory_order_relaxed) > pages - pages / 8;
}

bool GuestGpuMemory::EndFrame() {
    const u32 now = frame.fetch_add(1, std::memory_order_relaxed) + 1;
    if (pool_used.load(std::memory_order_relaxed) == 0) {
        return false;
    }
    if (PoolLow()) {
        return true;
    }
    if (now % 64 != 0) {
        return false;
    }
    // By index up to the published size: a move may grow the pool meanwhile (pool_pages).
    const u32 pages = pool_pages.load(std::memory_order_acquire);
    for (u32 i = 0; i < pages; i++) {
        const u32 page = pool_owner[i];
        if (page != NoPage && IsStale(page) && !hooks.versioned(page)) {
            return true;
        }
    }
    return false;
}

u32 GuestGpuMemory::ReturnPages(const std::vector<std::pair<u32, u32>>& pages) {
    u32 returned = 0;
    for (std::size_t i = 0; i < pages.size();) {
        const u32 page = pages[i].first;
        u32 run = 1;
        while (i + run < pages.size() && pages[i + run].first == page + run &&
               run < MaxPagesPerMove) {
            run++;
        }
        // The GPU's writes queued to the pages land in their pool pages: done first, so that
        // the copy back takes them. Its reads go on from the pool pages until its remap.
        hooks.wait_writes(Memory::FCRAM_PADDR + page * PageSize, run * PageSize);
        const int ret = azaharRebackReturn(
            static_cast<u32>(reinterpret_cast<uintptr_t>(fcram + page * PageSize)), run * PageSize);
        if (ret != static_cast<int>(run)) {
            SgxdDrainKernelLog();
        }
        ASSERT_MSG(ret == static_cast<int>(run),
                   "sgxd: returning FCRAM {:#010x}+{:#x} from CDRAM failed ({:#010x})",
                   Memory::FCRAM_PADDR + page * PageSize, run * PageSize, static_cast<u32>(ret));
        // The GPU onto the FCRAM pages after the work queued before, which may read the pool
        // pages: they take another move once that work is done.
        const u32 fence = RemapGpu(page, run);
        for (u32 k = 0; k < run; k++) {
            SetMoved(page + k, false);
            pool_owner[pages[i + k].second] = NoPage;
            pool_fence[pages[i + k].second] = fence;
        }
        pool_used -= run;
        returned += run;
        i += run;
    }
    return returned;
}

u32 GuestGpuMemory::EvictPool(u32 want, u32 idle_frames) {
    const u32 now = frame.load(std::memory_order_relaxed);
    // (last use, FCRAM page, pool page) of the pages the last `idle_frames` frames left alone (two
    // at least: an open pass may still read a page this frame used, its cache not cleaned for it).
    // A versioned page stays: the guest's view of it may be on a version slot, which
    // azaharRebackReturn refuses.
    std::vector<std::tuple<u32, u32, u32>> idle;
    for (u32 slot = 0; slot < pool_owner.size(); slot++) {
        const u32 page = pool_owner[slot];
        if (page == NoPage || hooks.versioned(page)) {
            continue;
        }
        const u32 used = last_use[page].load(std::memory_order_relaxed);
        if (now - used >= idle_frames) {
            idle.emplace_back(used, page, slot);
        }
    }
    const u32 count = std::min<u32>(static_cast<u32>(idle.size()),
                                    std::max<u32>(want, static_cast<u32>(pool_owner.size()) / 8));
    if (count == 0) {
        return 0;
    }
    std::partial_sort(idle.begin(), idle.begin() + count, idle.end());
    std::vector<std::pair<u32, u32>> pages;
    for (u32 i = 0; i < count; i++) {
        pages.emplace_back(std::get<1>(idle[i]), std::get<2>(idle[i]));
    }
    std::sort(pages.begin(), pages.end());
#ifdef VITA_DIAGNOSTICS
    NoteReturn(pages, idle_frames == IdleFrames ? "the pool full, 60 frames idle"
                                                : "the pool full, 2 frames idle");
#endif
    const u32 start = azaharCyclesStart();
    const u32 returned = ReturnPages(pages);
    // This thread's cycles: the work's time, a wait in it left out, and no clock syscall.
    const u64 us = Common::CyclesToUs(azaharCycles() - start);
#ifdef VITA_DIAGNOSTICS
    stats.us += us;
    stats.returns++;
    stats.returned_pages += returned;
#endif
    LOG_INFO(Render,
             "sgxd: the CDRAM pool full: {} FCRAM page(s) unused longest (from frame {}) back in "
             "{} us",
             returned, std::get<0>(idle[count - 1]), us);
    return returned;
}

void GuestGpuMemory::ReturnUnused() {
    ReturnStale();
    // The pages a second has left alone go back; with none, the pool grows at the moves; at its
    // largest, the pages the last two frames left alone go back. A page still queued work reads
    // is moved again at its use.
    if (PoolLow()) {
        const u32 want = static_cast<u32>(pool_owner.size()) / 8;
        if (EvictPool(want, IdleFrames) == 0 &&
            pool_owner.size() * PageSize + GrowSize > MaxPoolSize) {
            EvictPool(want, 2);
        }
    }
}

void GuestGpuMemory::ReturnStale() {
    // The stale pages by FCRAM page, with their pool pages, for runs of neighbours.
    std::vector<std::pair<u32, u32>> stale;
    for (u32 slot = 0; slot < pool_owner.size(); slot++) {
        const u32 page = pool_owner[slot];
        if (page != NoPage && IsStale(page) && !hooks.versioned(page)) {
            stale.emplace_back(page, slot);
        }
    }
    if (stale.empty()) {
        return;
    }
    std::sort(stale.begin(), stale.end());
#ifdef VITA_DIAGNOSTICS
    NoteReturn(stale, "stale");
#endif
    const u32 start = azaharCyclesStart();
    const u32 returned = ReturnPages(stale);
    // This thread's cycles: the work's time, a wait in it left out, and no clock syscall.
    const u64 us = Common::CyclesToUs(azaharCycles() - start);
#ifdef VITA_DIAGNOSTICS
    stats.us += us;
    stats.returns++;
    stats.returned_pages += returned;
#endif
    LOG_INFO(Render,
             "sgxd: {} FCRAM page(s) unused for {} frames back from CDRAM in {} us; pool {} of "
             "{} pages",
             returned, StaleFrames, us, pool_used.load(), pool_owner.size());
}

#ifdef VITA_DIAGNOSTICS
void GuestGpuMemory::NoteReturn(const std::vector<std::pair<u32, u32>>& pages, const char* kind) {
    last_returned.clear();
    for (const auto& [page, slot] : pages) {
        last_returned.push_back(page);
    }
    last_return_frame = frame.load(std::memory_order_relaxed);
    last_return_kind = kind;
    const auto [start, end] = WatchRange();
    u32 in_range = 0, first = 0, last = 0;
    for (const auto& [page, slot] : pages) {
        const PAddr addr = Memory::FCRAM_PADDR + page * PageSize;
        if (end != 0 && addr >= start && addr < end) {
            first = in_range++ == 0 ? addr : first;
            last = addr;
        }
    }
    if (in_range != 0) {
        sceClibPrintf("[azahar] pagewatch: sys %u frame %u: return %u pages %08x-%08x (%s)\n",
                      static_cast<unsigned>(sceKernelGetSystemTimeWide()),
                      static_cast<unsigned>(last_return_frame), static_cast<unsigned>(in_range),
                      static_cast<unsigned>(first), static_cast<unsigned>(last + PageSize - 1),
                      kind);
    }
}

std::string GuestGpuMemory::DescribePages(PAddr addr, u32 size) const {
    if (size == 0 || addr < Memory::FCRAM_PADDR ||
        addr - Memory::FCRAM_PADDR + u64{size} > Memory::FCRAM_SIZE || !last_use) {
        return "not FCRAM";
    }
    const u32 now = frame.load(std::memory_order_relaxed);
    std::string out = fmt::format("frame {}:", now);
    const u32 first = (addr - Memory::FCRAM_PADDR) / PageSize;
    const u32 last = (addr - Memory::FCRAM_PADDR + size - 1) / PageSize;
    for (u32 page = first; page <= last; page++) {
        out += fmt::format(" {:#010x} ", Memory::FCRAM_PADDR + page * PageSize);
        out += Moved(page) ? fmt::format("moved, used {} frames ago",
                                         now - last_use[page].load(std::memory_order_relaxed))
                           : std::string("cached");
        if (std::find(last_returned.begin(), last_returned.end(), page) != last_returned.end()) {
            out += fmt::format(" (returned at frame {}, {})", last_return_frame, last_return_kind);
        }
        out += ";";
    }
    return out;
}
#endif

bool GuestGpuMemory::WouldMove(PAddr addr, u32 size) const {
    if (size == 0 || addr < Memory::FCRAM_PADDR || addr - Memory::FCRAM_PADDR + u64{size} > Memory::FCRAM_SIZE) {
        return false;
    }
    return !IsUncached(addr, size);
}

void GuestGpuMemory::MakeUncached([[maybe_unused]] PAddr addr, [[maybe_unused]] u32 size,
                                  [[maybe_unused]] bool overwritten) {
    if (!WouldMove(addr, size)) {
        return;
    }
    // No idle GPU (the class's comment): the GPU's writes queued to the range land first, so
    // that the copy takes them; its reads go on from the old pages until its remap.
    hooks.wait_writes(addr, size);
    const u32 start = azaharCyclesStart();
    const u32 offset = addr - Memory::FCRAM_PADDR;
    const u32 first = offset / PageSize;
    const u32 end = (offset + size + PageSize - 1) / PageSize;
    // A page keeps its contents unless the GPU is about to write all of it.
    const auto copies = [&](u32 page) {
        return !overwritten || page * PageSize < offset || (page + 1) * PageSize > offset + size;
    };
    u32 moved_pages = 0, copied_pages = 0;
    u32 page = first;
    while (page < end) {
        if (Moved(page)) {
            page++;
            continue;
        }
        // A run of pages not moved yet, alike in whether they are copied, within one call.
        const bool copy = copies(page);
        u32 run = 1;
        while (page + run < end && !Moved(page + run) && copies(page + run) == copy &&
               run < MaxPagesPerMove) {
            run++;
        }
        auto [slot, free_pages] = FreePoolRun(run);
        if (free_pages == 0) {
            // Pool pages given back whose queued readers are still running: the oldest return's.
            WaitPoolFence();
            std::tie(slot, free_pages) = FreePoolRun(run);
        }
        if (free_pages == 0 && GrowPool()) {
            // No pages go back here (the class's comment): the vblank puts back what a second
            // has left alone (ReturnUnused), and meanwhile the pool grows.
            std::tie(slot, free_pages) = FreePoolRun(run);
        }
        ASSERT_MSG(free_pages != 0,
                   "sgxd: the {} KiB CDRAM pool at its largest is full; FCRAM from {:#010x} "
                   "cannot move",
                   pool_owner.size() * PageSize >> 10, Memory::FCRAM_PADDR + page * PageSize);
        run = free_pages;
        const AzaharRebackRequest req{
            static_cast<u32>(reinterpret_cast<uintptr_t>(fcram + page * PageSize)),
            static_cast<u32>(reinterpret_cast<uintptr_t>(PoolPage(slot))),
            run * PageSize, copy ? AZAHAR_REBACK_COPY : 0u};
        const int ret = azaharReback(&req);
        // The pool pages are the move's from here on, whatever it answered: a failed call may
        // have moved some of them, and a page under two FCRAM pages would join them.
        pool_used += run;
        const u32 now = frame.load(std::memory_order_relaxed);
        for (u32 k = 0; k < run; k++) {
            pool_owner[slot + k] = page + k;
            last_use[page + k].store(now, std::memory_order_relaxed);
        }
        if (ret != static_cast<int>(run)) {
            SgxdDrainKernelLog();
        }
        ASSERT_MSG(ret == static_cast<int>(run),
                   "sgxd: moving FCRAM {:#010x}+{:#x} to CDRAM failed ({:#010x})",
                   Memory::FCRAM_PADDR + page * PageSize, run * PageSize, static_cast<u32>(ret));
        RemapGpu(page, run);
        for (u32 k = 0; k < run; k++) {
            SetMoved(page + k, true);
        }
        moved_pages += run;
        copied_pages += copy ? run : 0;
        page += run;
    }
    // This thread's cycles: the work's time, a wait in it left out, and no clock syscall.
    const u64 us = Common::CyclesToUs(azaharCycles() - start);
#ifdef VITA_DIAGNOSTICS
    stats.us += us;
    stats.calls++;
    stats.pages += moved_pages;
    stats.copied_pages += copied_pages;
#endif
    LOG_INFO(Render,
             "sgxd: FCRAM {:#010x}+{:#x} to CDRAM{}: {} page(s) moved, {} copied, in {} us; pool "
             "{} of {} pages",
             addr, size, overwritten ? " (the GPU overwrites it)" : "", moved_pages, copied_pages,
             us, pool_used.load(), pool_owner.size());
}

#ifdef VITA_DIAGNOSTICS
GuestGpuMemory::MoveStats GuestGpuMemory::TakeMoveStats() {
    MoveStats out = stats;
    out.pool_pages_used = pool_used.load();
    out.pool_pages = static_cast<u32>(pool_owner.size());
    stats = {};
    return out;
}
#endif

} // namespace SgxdRenderer
