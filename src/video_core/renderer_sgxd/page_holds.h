// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <deque>
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
 * The guest pages queued GPU work still reads or writes, held against the guest: a page the work
 * reads is closed to the guest's writes, a page it writes to any guest access, until the work is
 * done. That lets the GPU's interrupts reach the guest at submission while the SGX still runs it.
 *
 * Holds are by ticket, the render thread's numbers for points in its queue: a hold made for
 * ticket T lasts until the GPU has done the work queued before T's fence. Each page keeps the
 * last ticket that reads it and the last that writes it; each ticket keeps the pages it held,
 * so that Release lets go of a ticket's pages once it is reached, and only those no later
 * ticket holds. The guest's view of a page changes through azaharHold (vita_native), by the
 * app's address: every guest alias of the page at once. The holds are not write tracking:
 * nothing is kept about a page once its work is done.
 *
 * Guest VRAM is held for reads as a whole, from the first GPU read of it on, and for writes
 * page by page: the guest writes VRAM with the CPU rarely (titles fill it by DMA, which is
 * ordered on the GPU), and a vertex range in VRAM is not scanned for its indices (they are
 * uncached CDRAM). A guest write to VRAM waits for every read of VRAM queued before it, and
 * the page is closed to writes again at the next GPU read of VRAM.
 *
 * Page versions, per frame: an FCRAM page the GPU reads gets
 * more physical pages (Slots: its home and slots of a CDRAM pool), the first allocated the first
 * time the guest writes it while queued work reads it, the next when every slot it has is read.
 * A versioned page is protected by the frame, not by the work: armed (closed to writes) at each
 * frame's start (the vblank), open once the guest has written it in the frame. The first write
 * of a frame swaps the guest's mapping (the app's view and every guest window, azaharFlip) onto
 * a slot no queued work reads, its bytes copied there, when queued work reads the guest's slot;
 * the GPU's MMU keeps the slot its queued work reads, and the page is marked changed. With every
 * slot read, the write goes into the guest's slot, without a wait (queued work that reads it may
 * see the newer bytes). At the next frame's start the GPU's MMU is
 * remapped onto the changed pages' slots in one batch, in the GPU's queue order (Remaps: the
 * microkernel stores the PTEs and invalidates the MMU, sgxd_remap_submit). A changed page that
 * work later in the same frame reads or writes is remapped before that work, and stays open. A
 * page no GPU work has used for ConvergeFrames frames goes home and gives its slot back. Only
 * pages already moved to CDRAM are versioned (GuestGpuMemory): no slot then needs cache work.
 *
 * Emulation side only, under the GPU's lock: the pages change while the guest is stopped (in
 * a GSP command, a register write, a vblank or an exit), before it runs again (Apply).
 */
class PageHolds {
public:
    explicit PageHolds(Memory::MemorySystem& memory);
    ~PageHolds();

    /// The versions' pool at this GPU address, sgxd's page tables at TablesVa (sgxdMapTables):
    /// free in sgxd's map between guest FCRAM and the display buffers.
    static constexpr u32 VersionsVa = 0x30000000;
    static constexpr u32 TablesVa = 0x2c000000;

    enum Access : u8 { Unheld = 0, ReadOnly = 1, NoAccess = 2 };

    enum Kind : u8 {
        Texture,
        Depth,
        Index,
        Vertex,
        Colour,
        FillOut,
        TransferIn,
        TransferOut,
        DmaIn,
        DmaOut,
        Present,
        Kinds
    };
    static const char* KindName(Kind kind);

    /// Queued GPU work up to `ticket`, the ticket the next fence takes, reads (writes) the
    /// guest's [addr, addr + bytes). Addresses outside guest FCRAM and VRAM hold nothing.
    void Read(PAddr addr, u32 bytes, u32 ticket, Kind kind);
    void Write(PAddr addr, u32 bytes, u32 ticket, Kind kind);
    /// A fence took `ticket`: the holds made for it are complete.
    void Close(u32 ticket);
    /// The guest's view gets the holds made since the last call. Before the guest runs again.
    void Apply();
    /// Lets go of the pages whose tickets are all reached: the tickets up to `reached` let go
    /// of their pages, and the guest's view follows (azaharHold).
    void Release(u32 reached);

    [[nodiscard]] u32 PageOf(const u8* host) const;
    static constexpr u32 NoPage = ~0u;
    /// The ticket a guest access to the page must wait for, 0 for none: a read waits for the
    /// page's last GPU writer, a write for its last reader too. Tickets past `reached` only.
    [[nodiscard]] u32 WaitFor(u32 page, bool write, u32 reached) const;
    [[nodiscard]] PAddr AddressOf(u32 page) const;
    [[nodiscard]] Access Applied(u32 page) const {
        return static_cast<Access>(applied[page]);
    }
    /// The page's view opened at once, its work done (WaitFor): an abort on it can resume.
    void Reopen(u32 page, u32 reached);
    /// A guest window now shows [host, host + size), mapped open: the held pages of it are
    /// sent to the module again, which holds every window of them.
    void Resend(const u8* host, u32 size);

    /// T12, texture write tracking: the FCRAM pages a texture is taken
    /// from are watched, closed to the guest's writes on top of the holds, until the guest or
    /// the HLE writes one (the caller marks the textures over it written, then Unwatch opens it
    /// as its holds allow). A texture's next use watches its pages again.
    void Watch(PAddr addr, u32 bytes);
    [[nodiscard]] bool Watched(u32 page) const {
        return page < fcram_pages && watched[page] != 0;
    }
    void Unwatch(u32 page, u32 reached);
    /// Unwatch with the page's change left for the caller's Apply (a range of pages in one
    /// azaharHold, RenderThread::InvalidateWritten).
    void UnwatchUnsent(u32 page, u32 reached);

    /// Index lists' write tracking, for the scan's memo of their ranges
    /// (RenderThread::HoldVertices): the FCRAM pages of a list scanned once are watched, closed to
    /// the guest's writes on top of the holds, their store permits void, until a write (the
    /// guest's, a store past the holds, the HLE's, a GPU write hold, a version) moves their stamp
    /// on and opens them. A
    /// page written in the last IndexQuietFrames frames, or versioned, is not watched: a buffer
    /// the game fills each frame would trap at every draw.
    [[nodiscard]] bool IndexMemoable(u32 first, u32 count) const;
    /// The sum of the pages' stamps: a write to any of them changes it.
    [[nodiscard]] u32 IndexStamp(u32 first, u32 count) const;
    void WatchIndices(u32 first, u32 count);
    [[nodiscard]] bool IndexWatched(u32 page) const {
        return page < fcram_pages && index_watch[page] != 0;
    }
    /// A write to the page (or one may have been made): its stamp moves on, the watch goes.
    void IndexWritten(u32 page, u32 reached);
    /// Whether the page is closed to writes by the index watch alone (Wanted without it: open).
    [[nodiscard]] bool ClosedByIndexWatchAlone(u32 page, u32 reached) const {
        return IndexWatched(page) && HoldsWant(page, reached) == Unheld && !Watched(page) &&
               parse_held[page] == 0;
    }
    static constexpr u32 IndexQuietFrames = 30;

    /// A guest write aborted on FCRAM page `page`, moved to CDRAM (the caller checks): whether
    /// the page is open to the write now, swapped onto a free slot or written in place. Not
    /// taken, the ordinary holds decide (a GPU write queued, or a page no queued work reads).
    bool Version(u32 page, u32 reached);
    /// A remap of the GPU's MMU the holds asked for: the GPU address of the page's PTE in
    /// sgxd's mapped tables and the PTE word naming the guest's page (sgxdPte), for the GPU to
    /// store before the work that reads or writes the page; and whether work queued since the
    /// GPU moved onto the slot it leaves read or wrote the page other than as a texture (direct,
    /// 1: the open pass may, and ends first; textures are read by their untiles, sent first).
    struct Remap {
        PAddr page;
        u32 pte_va;
        u32 pte;
        u32 direct;
    };
    [[nodiscard]] std::vector<Remap>& Remaps() {
        return remaps;
    }
    /// A frame starts (the vblank): the changed pages' remaps for `ticket` (the next fence's),
    /// the versioned pages armed again, and pages no GPU work has used for ConvergeFrames frames
    /// back home.
    void StartFrame(u32 reached, u32 ticket);
    static constexpr u32 ConvergeFrames = 60;
    /// Whether the FCRAM page has slots besides its home: the guest's view or the GPU's may be
    /// on one (azaharFlip), and the page stays moved (GuestGpuMemory) until it is home again.
    [[nodiscard]] bool Versioned(u32 page) const {
        return versions[page].on;
    }
    /// A byte a page, FCRAM's then VRAM's: nonzero where the guest's view is held (the HLE's
    /// block copies check it, Memory::MemorySystem::SetHostGuard).
    [[nodiscard]] const u8* HeldMap() const {
        return applied.data();
    }

#ifdef VITA_DIAGNOSTICS
    struct Stats {
        u32 calls, runs, descriptors, held_pages, released_pages;
        u32 flip_calls, versions, swaps, in_place_read, in_place, frame_remaps, early_remaps, homes,
            slots_used;
    };
    Stats TakeStats();
    /// The kind of the hold an access to the page waits for (WaitFor's ticket): the last
    /// hold made on the page, or for a VRAM page's readers the last VRAM read's.
    [[nodiscard]] Kind WaitKind(u32 page, bool write, u32 reached) const;
    [[nodiscard]] std::pair<PAddr, u32> RangeOf(u32 page) const {
        return ranges[page];
    }
    /// Why OpenPastDisplay would leave the page shut (holdlog): the display bytes and the
    /// other accesses, the watch, the version.
    [[nodiscard]] std::string DisplayState(u32 page, u32 reached) const;
    [[nodiscard]] std::pair<Kind, std::pair<PAddr, u32>> WriterOf(u32 page) const {
        return {static_cast<Kind>(write_kinds[page]), write_ranges[page]};
    }
#endif

private:
    /// A run of pages, [first, first + count).
    struct Run {
        u32 first;
        u32 count;
    };
    /// A ticket's pages, for Release.
    struct Group {
        u32 ticket;
        std::vector<Run> runs;
    };

    [[nodiscard]] bool Live(u32 ticket, u32 reached) const {
        return ticket != 0 && static_cast<s32>(ticket - reached) > 0;
    }
    [[nodiscard]] Access Wanted(u32 page, u32 reached) const;
    [[nodiscard]] Access HoldsWant(u32 page, u32 reached) const;
    [[nodiscard]] bool PagesOf(PAddr addr, u32 bytes, u32& first, u32& count) const;
    /// Whether this ticket held the range already (a small cache of the ranges held for it).
    bool Seen(PAddr addr, u32 bytes, u32 kind);
    void Change(u32 page, Access access);
    /// Sends the changed pages to the module, the flips before the holds.
    void Flush();
    /// The page's app view and guest windows on slot `slot`'s page, or home (NoSlot), at the
    /// next Flush.
    void Flip(u32 page, u16 slot);
    [[nodiscard]] u16 AllocSlot();
    [[nodiscard]] u8* SlotHost(u16 slot) const;
    /// The GPU's MMU onto the guest's slot of `page` before the work of `ticket` (a Remap); the
    /// slot it leaves is read by queued work up to that ticket.
    void RemapToGuest(u32 page, u32 ticket);
    [[nodiscard]] bool Changed(u32 page) const {
        return versions[page].on && versions[page].guest != versions[page].gpu;
    }

    u8* fcram_host;
    u8* vram_host;
    u32 fcram_pages;
    u32 pages;

    std::vector<u32> read_ticket;  ///< FCRAM pages only (VRAM's reads: vram_read)
    /// FCRAM pages: whether the last read hold was a vertex or index read (GeometryReadOnly).
    std::vector<u8> geometry_read;
    std::vector<u32> write_ticket; ///< every page
    std::vector<u8> applied;       ///< Access, as the guest's view has it (after Apply)
    std::vector<u8> watched;       ///< T12: FCRAM pages a texture is taken from (Watch)
    /// T12: a watched page's textures' bytes, as up to two ranges [lo, hi) (hi 0: none), and
    /// a count of the times it was watched anew (the stores beside them: StoreMisses).
    std::vector<std::array<u16, 2>> watch_lo, watch_hi;
    std::vector<u32> watch_ticket;
    /// Watch's ranges watched whole, direct-mapped: a texture's range comes back with every draw
    /// that reads it, and its pages stay watched until UnwatchUnsent, which counts `unwatches`;
    /// a slot with the count still current needs no walk of the range's pages.
    std::array<std::array<u32, 3>, 256> watch_seen{};
    u32 unwatches{};
    std::vector<u8> index_watch;
    std::vector<u32> index_stamp, index_write_frame;

    /// The holds made for the next ticket, and the tickets' groups not yet released.
    u32 open_ticket{};
    std::vector<Run> open_runs;
    std::deque<Group> groups;
    /// Seen's ranges, direct-mapped: 256 slots, as a draw-heavy list's textures and vertex ranges
    /// evicted each other from 64.
    std::array<std::array<u32, 4>, 256> seen{};

    /// VRAM's reads: the last ticket that read VRAM, whether its pages are held, and the pages
    /// a guest write opened since (held again at the next read).
    u32 vram_read{};
    bool vram_held{};
    std::vector<u32> vram_opened;

    std::vector<u32> changed;
    std::vector<std::array<u32, 3>> calls_runs;

    /// A page's versions: its slots (0 the home, the rest pages of the pool, NoSlot until used),
    /// the ticket up to which queued work reads each slot the GPU has left, which slot the guest
    /// (and the app) is on and which the GPU's MMU names as of the work queued, whether it is armed
    /// (closed to writes) this frame, and the frame of its last GPU use. Five slots: the GPU runs
    /// up to four frames behind the guest (RenderThread::LeadFramesBack), each frame's work reading
    /// the slot of its own frame, so a page the guest rewrites each frame needs a fifth to write.
    static constexpr u16 NoSlot = 0xffff;
    static constexpr u8 Slots = 5;
    static constexpr std::array<u16, Slots - 1> NoPoolSlots() {
        std::array<u16, Slots - 1> pool{};
        pool.fill(NoSlot);
        return pool;
    }
    struct PageVersion {
        std::array<u16, Slots - 1> pool{NoPoolSlots()};
        std::array<u32, Slots> busy{};
        u8 guest{};
        u8 gpu{};
        bool armed{};
        bool on{};
        bool direct{}; ///< queued work read or wrote the GPU's slot other than as a texture
        u32 use_frame{};
    };
    std::unique_ptr<Common::HostSharedMemory> version_pool;
    std::vector<PageVersion> versions;
    std::vector<u32> versioned;
    std::vector<u16> free_slots;
    std::vector<Remap> remaps;
    /// azaharFlip's runs for the next Flush: (app address, version's app address or 0).
    std::vector<std::array<u32, 2>> flips;
    u32 frame{};

#ifdef VITA_DIAGNOSTICS
    Stats stats{};
    std::vector<u8> kinds;
    std::vector<std::pair<PAddr, u32>> ranges;
    std::vector<u8> write_kinds;
    std::vector<std::pair<PAddr, u32>> write_ranges;
    u8 vram_read_kind{};
#endif
};

} // namespace SgxdRenderer
