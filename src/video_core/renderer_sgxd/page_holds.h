// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <deque>
#include <vector>
#include "common/common_types.h"

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
 * Emulation side only, under the GPU's lock: the pages change while the guest is stopped (in
 * a GSP command, a register write, a vblank or an exit), before it runs again (Apply).
 */
class PageHolds {
public:
    explicit PageHolds(Memory::MemorySystem& memory);

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
    [[nodiscard]] Access Applied(u32 page) const {
        return static_cast<Access>(applied[page]);
    }
    /// The page's view opened at once, its work done (WaitFor): an abort on it can resume.
    void Reopen(u32 page, u32 reached);
    /// A guest window now shows [host, host + size), mapped open: the held pages of it are
    /// sent to the module again, which holds every window of them.
    void Resend(const u8* host, u32 size);
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
    /// Since the last call (`perflog`): azaharHold's calls, the runs and pages it changed
    /// (descriptors), the pages held and let go of.
    struct Stats {
        u32 calls, runs, descriptors, held_pages, released_pages;
    };
    Stats TakeStats();
    /// The kind of the hold an access to the page waits for (WaitFor's ticket): the last
    /// hold made on the page, or for a VRAM page's readers the last VRAM read's.
    [[nodiscard]] Kind WaitKind(u32 page, bool write, u32 reached) const;
    /// The page's guest physical address.
    [[nodiscard]] PAddr AddressOf(u32 page) const;
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
    /// Sends the changed pages to the module.
    void Flush();

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

#ifdef VITA_DIAGNOSTICS
    Stats stats{};
    std::vector<u8> kinds;
    u8 vram_read_kind{};
#endif
};

} // namespace SgxdRenderer
