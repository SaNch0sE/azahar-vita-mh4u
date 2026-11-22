// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "common/common_types.h"

namespace Common {
class HostSharedMemory;
}

namespace Memory {
class MemorySystem;
}

namespace SgxdRenderer {

/**
 * Guest memory in the GPU's address space at its PICA physical address, so that a PICA address
 * is a GPU address and the draw path translates nothing (docs/sgxd/memory.md).
 * Guest VRAM (CDRAM, uncached for the CPU) at 0x18000000, guest FCRAM (cached: cleaned before a
 * kick reads it) at 0x20000000. The GPU must be idle when this is made and destroyed: sgxd
 * invalidates its MMU then.
 *
 * FCRAM the GPU writes and reads back (a display transfer's output, a framebuffer, a texture)
 * is moved onto CDRAM pages of a pool of ours (azaharReback): the same host, guest and GPU
 * addresses, uncached for the CPU, so the GPU's passes need no cache maintenance. The render
 * thread moves what its work is about to use, whose page holds keep the guest's writes off the
 * pages during the copy (the module refuses the second emulation thread's). Past 8 MiB the pool
 * grows into uncached main memory. At vblank (ReturnUnused) a page unused for StaleFrames frames
 * goes back to FCRAM, and with the pool short the pages unused longest (EvictPool), but none
 * that page versions hold; a move never puts pages back itself.
 *
 * Neither waits for the GPU to go idle: the CPU's views move at once, the GPU's mapping by its
 * own remap in queue order (MoveHooks::remap), so earlier work reads the old page, with the same
 * bytes. Only GPU writes queued to the pages are waited for (MoveHooks::wait_writes). A returned
 * page's pool page takes another move once work queued before the return completed (pool_fence).
 */
class GuestGpuMemory {
public:
    explicit GuestGpuMemory(Memory::MemorySystem& memory);
    ~GuestGpuMemory();

    GuestGpuMemory(const GuestGpuMemory&) = delete;
    GuestGpuMemory& operator=(const GuestGpuMemory&) = delete;

    /// The live instance's pool in use and whole, for a crash report: its atomics only.
    static void DescribePool(char* out, std::size_t size);

    /// A moved page no GPU use has stamped for this many frames goes back to FCRAM.
    static constexpr u32 StaleFrames = 1000;

    /// What a move needs of the render side (RenderThread sets them; a move runs on the render
    /// thread, or with it held as every return does, its side on the caller's): waiting for the GPU
    /// writes queued to a range; the GPU's own remap of PTE words (sgxdPte pairs) in its queue's
    /// order, returning a fence the work queued with it is done by; that fence reached, and a wait
    /// for it; whether page versions hold a moved FCRAM page (PageHolds::Versioned), which does not
    /// go back until they let it go.
    struct MoveHooks {
        /// The GPU's queued writes to [addr, addr + size) landed: before a move's copy, or a
        /// return's (`moved`: the pages are in the pool, where the GPU's passes write too).
        std::function<void(PAddr addr, u32 size, bool moved)> wait_writes;
        std::function<u32(const u32* pte_va, const u32* pte, u32 count)> remap;
        std::function<bool(u32 fence)> reached;
        std::function<void(u32 fence)> wait;
        std::function<bool(u32 page)> versioned;
    };
    void SetMoveHooks(MoveHooks hooks_) {
        hooks = std::move(hooks_);
    }

    /// Whether the CPU reaches all of [addr, addr + size) uncached: guest VRAM, or FCRAM pages
    /// moved to CDRAM. Such memory needs no cache operation around the GPU. Every caller is a
    /// GPU use of the range: the moved pages of it are stamped with the frame.
    [[nodiscard]] bool IsUncached(PAddr addr, u32 size) const;
#ifdef VITA_DIAGNOSTICS
    /// [addr, addr + size)'s FCRAM pages for a report: each moved (and the frames since its
    /// last use) or not, and whether the last return took it.
    [[nodiscard]] std::string DescribePages(PAddr addr, u32 size) const;
#endif

    /// Whether MakeUncached would move any page of [addr, addr + size): FCRAM not moved yet.
    /// A full pool or a failed move stops the emulator inside MakeUncached.
    /// Any thread: the pages change only inside MakeUncached and ReturnUnused.
    [[nodiscard]] bool WouldMove(PAddr addr, u32 size) const;

    /**
     * Moves the FCRAM pages of [addr, addr + size) onto CDRAM. `overwritten`: the GPU is about
     * to write the whole range, so only the pages it covers in part keep their contents; the
     * others are not copied. On the render thread: from the work about to use the pages (its
     * page holds keep the guest off them), or for the emulation side (GPU::KeepUncached) after
     * the work queued, the guest stopped.
     */
    void MakeUncached(PAddr addr, u32 size, bool overwritten);

    /**
     * A frame has ended: from the emulation thread at vblank, with the guest stopped. Whether
     * pages should go back (ReturnUnused): the pool short of free pages, or (looked at every
     * 64th frame) a moved page StaleFrames frames without a use.
     */
    [[nodiscard]] bool EndFrame();

    /// Puts back on FCRAM the moved pages StaleFrames frames without a use, and with the pool
    /// short of free pages the ones unused longest. At vblank, the guest stopped and the render
    /// thread held.
    void ReturnUnused();

#ifdef VITA_DIAGNOSTICS
    struct MoveStats {
        u64 us;
        u32 calls;
        u32 pages;
        u32 copied_pages;
        u32 returns;
        u32 returned_pages;
        u32 pool_pages_used;
        u32 pool_pages;
    };
    /// The moves since the last call, and the pool's use (`perflog`; diagnostics builds only).
    MoveStats TakeMoveStats();
#endif

    /// The filler mapped on the GPU past VRAM's end, for textures that run past it: one block of
    /// main memory again and again, its contents undefined, so the GPU's reads there are mapped and
    /// its writes reach no guest memory.
    static constexpr u32 PastVramBytes = Memory::VRAM_SIZE;
    static constexpr u32 PastVramBlock = 1u << 20;

private:
    const u8* vram{};
    u8* fcram{};
    bool past_vram{};
    std::vector<std::unique_ptr<Common::HostSharedMemory>> pool;
    std::vector<u32> pool_starts;
    std::atomic<u32> pool_used{}; ///< pool pages under an FCRAM page (EndFrame reads it)
    /// pool_owner's size, for EndFrame on the emulation thread while a move may grow the pool.
    std::atomic<u32> pool_pages{};
    std::vector<u32> pool_owner;  ///< per pool page: the FCRAM page on it, or NoPage
    /// Per pool page: the fence of the GPU's remap away from it when its page went back (0:
    /// none); queued work may read it until then.
    std::vector<u32> pool_fence;
    MoveHooks hooks;
    bool adopted{};               ///< FCRAM's block is under azaharnative's tables
    /// The Old 3DS's FCRAM in 4 KiB pages (checked against core/memory.h in the constructor).
    static constexpr u32 FcramPages = 0x08000000 / 0x1000;
    /// Per FCRAM page, a bit: moved. Set by the move's thread, read by both.
    std::array<std::atomic<u32>, FcramPages / 32> moved_bits{};
    [[nodiscard]] bool Moved(u32 page) const {
        return (moved_bits[page / 32].load(std::memory_order_acquire) >> (page % 32) & 1) != 0;
    }
    void SetMoved(u32 page, bool on) {
        const u32 bit = 1u << (page % 32);
        if (on) {
            moved_bits[page / 32].fetch_or(bit, std::memory_order_release);
        } else {
            moved_bits[page / 32].fetch_and(~bit, std::memory_order_release);
        }
    }
    /// Per FCRAM page: the frame of its last GPU use while moved. Stamped from the render and
    /// emulation threads.
    std::unique_ptr<std::atomic<u32>[]> last_use;
    std::atomic<u32> frame{};
#ifdef VITA_DIAGNOSTICS
    MoveStats stats{};
    /// The last return's FCRAM pages (EvictPool, ReturnStale), its frame and its kind.
    std::vector<u32> last_returned;
    u32 last_return_frame{};
    const char* last_return_kind = "";
    void NoteReturn(const std::vector<std::pair<u32, u32>>& pages, const char* kind);
#endif

    static constexpr u32 NoPage = ~0u;

    [[nodiscard]] u8* PoolPage(u32 slot) const;
    /// One more block for the pool, CDRAM or uncached main memory; false at the pool's largest.
    bool GrowPool();
    /// The first run of free pool pages (no FCRAM page on them, no queued work reading them)
    /// within one of its blocks, at most `want` long, as (first, length); length 0 when there
    /// is none.
    [[nodiscard]] std::pair<u32, u32> FreePoolRun(u32 want) const;
    /// The GPU's PTEs of FCRAM pages [page, page + count) onto the pages the CPU's views have
    /// now, in the GPU's order (MoveHooks::remap). Returns the last remap's fence.
    u32 RemapGpu(u32 page, u32 count);
    /// Waits for the oldest return whose pool pages queued work may still read, if any.
    void WaitPoolFence();
    [[nodiscard]] bool IsStale(u32 page) const;
    [[nodiscard]] bool PoolLow() const;
    void ReturnStale();
    /// Puts moved pages back on FCRAM, as (FCRAM page, pool page) pairs sorted by FCRAM page.
    /// Returns how many went back.
    u32 ReturnPages(const std::vector<std::pair<u32, u32>>& pages);
    /// A full pool: the moved pages unused longest (none of the last `idle_frames` frames used)
    /// go back to FCRAM, an eighth of the pool at least `want` pages, so that the move goes on.
    /// Returns how many went back.
    u32 EvictPool(u32 want, u32 idle_frames);
};

} // namespace SgxdRenderer
