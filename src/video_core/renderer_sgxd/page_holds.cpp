// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
#include <utility>
#include "azahar_native.h"
#include "common/assert.h"
#include "common/host_shared_memory.h"
#include "common/logging/log.h"
#include "core/memory.h"
#include "sgxd.h"
#include "video_core/renderer_sgxd/page_holds.h"
#include "video_core/renderer_sgxd/sgxd_backend.h"

namespace SgxdRenderer {

namespace {

constexpr u32 PageBits = 12;
constexpr u32 PageSize = 1u << PageBits;
/// The versions' pool: CDRAM, uncached for the CPU, as the moved pages it versions are.
constexpr u32 VersionPoolSize = 4u << 20;
/// The cached pool, for the vertex and index pages that stay in cached FCRAM: main memory.
constexpr u32 CachedPoolSize = 4u << 20;
constexpr u16 CachedSlot = VersionPoolSize >> PageBits;

static_assert(sizeof(AzaharHoldRun) == sizeof(std::array<u32, 3>));
static_assert(PageHolds::Unheld == AZAHAR_HOLD_NONE && PageHolds::ReadOnly == AZAHAR_HOLD_WRITES &&
              PageHolds::NoAccess == AZAHAR_HOLD_ALL);

} // namespace

PageHolds::PageHolds(Memory::MemorySystem& memory)
    : fcram_host{memory.GetFCRAMPointer(0)},
      vram_host{memory.GetPhysicalPointer(Memory::VRAM_PADDR)},
      fcram_pages{Memory::FCRAM_SIZE >> PageBits},
      pages{fcram_pages + (Memory::VRAM_SIZE >> PageBits)}, list_kept(pages),
      read_ticket(fcram_pages),
      copy_ticket(fcram_pages), copy_lo(fcram_pages), copy_hi(fcram_pages), copy_from(fcram_pages),
      read_lo(fcram_pages), read_hi(fcram_pages), other_read(fcram_pages),
      written(fcram_pages, 1),
      move_ticket(fcram_pages), write_ticket(pages), write_run(pages),
      write_lo(pages), write_hi(pages), display_ticket(fcram_pages), other_ticket(fcram_pages),
      display_lo(fcram_pages), display_hi(fcram_pages), applied(pages), picture_watch(pages),
      picture_watch_lo(pages), picture_watch_hi(pages),
      watched(fcram_pages),
      watch_lo(fcram_pages), watch_hi(fcram_pages), parse_held(pages),
      index_watch(fcram_pages), index_stamp(fcram_pages),
      index_write_frame(fcram_pages) {
#ifdef VITA_DIAGNOSTICS
    kinds.resize(pages);
    ranges.resize(pages);
    write_kinds.resize(pages);
    write_ranges.resize(pages);
    sent.resize(pages);
    changed_by.resize(pages);
    written_by.assign(fcram_pages, ByAll);
    past_lines.resize(fcram_pages);
#endif
    ASSERT_MSG(fcram_host != nullptr && vram_host != nullptr,
               "page holds: guest FCRAM or VRAM has no host address");
    version_pool = std::make_unique<Common::HostSharedMemory>(
        VersionPoolSize, "azahar-page-versions", Common::HostSharedMemory::Placement::GpuLocal);
    // sgxd's page tables in the GPU's own view, for the remaps the microkernel stores (GPU idle).
    const int tables = sgxdMapTables(TablesVa);
    ASSERT_MSG(tables >= 0,
               "page holds: sgxd's page tables were not mapped at GPU {:#010x}: {:#010x}", TablesVa,
               static_cast<u32>(tables));
    cached_pool = std::make_unique<Common::HostSharedMemory>(
        CachedPoolSize, "azahar-cached-versions", Common::HostSharedMemory::Placement::OwnBlock);
    versions.resize(fcram_pages);
    const u32 count = VersionPoolSize >> PageBits;
    free_slots.reserve(count);
    for (u32 s = count; s-- > 0;) {
        free_slots.push_back(static_cast<u16>(s));
    }
    const u32 cached_count = CachedPoolSize >> PageBits;
    free_cached_slots.reserve(cached_count);
    for (u32 s = cached_count; s-- > 0;) {
        free_cached_slots.push_back(static_cast<u16>(CachedSlot + s));
    }
}

PageHolds::~PageHolds() = default;

bool PageHolds::PagesOf(PAddr addr, u32 bytes, u32& first, u32& count) const {
    if (bytes == 0) {
        return false;
    }
    PAddr region;
    u32 base, region_pages;
    if (addr >= Memory::FCRAM_PADDR && addr - Memory::FCRAM_PADDR < Memory::FCRAM_SIZE) {
        region = Memory::FCRAM_PADDR;
        base = 0;
        region_pages = fcram_pages;
    } else if (addr >= Memory::VRAM_PADDR && addr - Memory::VRAM_PADDR < Memory::VRAM_SIZE) {
        region = Memory::VRAM_PADDR;
        base = fcram_pages;
        region_pages = pages - fcram_pages;
    } else {
        return false;
    }
    const u32 lo = (addr - region) >> PageBits;
    const u64 end = static_cast<u64>(addr - region) + bytes + PageSize - 1;
    const u32 hi = static_cast<u32>(std::min<u64>(end >> PageBits, region_pages));
    first = base + lo;
    count = hi - lo;
    return count != 0;
}

u32 PageHolds::PageOf(const u8* host) const {
    const uintptr_t h = reinterpret_cast<uintptr_t>(host);
    const uintptr_t f = h - reinterpret_cast<uintptr_t>(fcram_host);
    if (f < static_cast<uintptr_t>(fcram_pages) << PageBits) {
        return static_cast<u32>(f >> PageBits);
    }
    const uintptr_t v = h - reinterpret_cast<uintptr_t>(vram_host);
    if (v < static_cast<uintptr_t>(pages - fcram_pages) << PageBits) {
        return fcram_pages + static_cast<u32>(v >> PageBits);
    }
    return NoPage;
}

bool PageHolds::Seen(PAddr addr, u32 bytes, u32 kind) {
    const std::array<u32, 4> key{addr, bytes, kind, open_ticket};
    const u32 hash = (addr >> 4) ^ (addr >> 12) ^ (bytes * 0x9E3779B1u) ^ kind;
    auto& slot = seen[hash % seen.size()];
    // Word by word: std::array's == is a memcmp call into newlib.
    if (((slot[0] ^ key[0]) | (slot[1] ^ key[1]) | (slot[2] ^ key[2]) | (slot[3] ^ key[3])) == 0) {
        return true;
    }
    slot = key;
    return false;
}

#ifdef VITA_DIAGNOSTICS
[[gnu::noinline]]
#endif
void PageHolds::Change(u32 page, Access access) {
#ifdef VITA_DIAGNOSTICS
    changed_by[page] = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
    if (access > applied[page]) {
        stats.held_pages++;
    } else {
        stats.released_pages++;
    }
#endif
    applied[page] = access;
    changed.push_back(page);
    if (access == Unheld && page < fcram_pages) {
        written[page] = 1;
#ifdef VITA_DIAGNOSTICS
        written_by[page] |= ByOpen;
#endif
    }
}

const char* PageHolds::KindName(Kind kind) {
    static constexpr const char* names[Kinds] = {
        "texture",     "depth",        "index",       "vertex", "colour",  "fill",
        "transfer in", "transfer out", "display out", "dma in", "dma out", "present"};
    return kind < Kinds ? names[kind] : "?";
}

void PageHolds::Read(PAddr addr, u32 bytes, u32 ticket, Kind kind) {
    u32 first, count;
    if (!PagesOf(addr, bytes, first, count)) {
        return;
    }
    DEBUG_ASSERT_MSG(open_ticket == 0 || open_ticket == ticket,
                     "page holds: ticket {} while {}'s are open", ticket, open_ticket);
    open_ticket = ticket;
    // A texture's range apart from the same range read otherwise (PageVersion::direct).
    if (Seen(addr, bytes, kind == Texture ? 0 : 2)) {
        return;
    }
    if (first >= fcram_pages) {
        // VRAM is held for reads as a whole (the class comment), the bytes each read covers
        // kept while it is live (StoreMisses).
        NoteVramRead(addr, bytes, ticket);
        vram_read = ticket;
#ifdef VITA_DIAGNOSTICS
        vram_read_kind = kind;
#endif
        if (!vram_held) {
            vram_held = true;
            for (u32 p = fcram_pages; p < pages; p++) {
                if (applied[p] < ReadOnly) {
                    Change(p, ReadOnly);
                }
            }
        } else {
            for (const u32 p : vram_opened) {
                if (applied[p] < ReadOnly) {
                    Change(p, ReadOnly);
                }
            }
        }
        vram_opened.clear();
        return;
    }
    const bool geometry = kind == Vertex || kind == Index;
    for (u32 p = first; p < first + count; p++) {
        // The cached pool's versions are a vertex or index page's alone.
        if (!geometry && versions[p].on && versions[p].cached) {
            LeaveVersions(p, ticket);
        }
        // A draw's texture moves to CDRAM at its untile (Moves).
        if (kind == Texture) {
            NoteMove(p, ticket);
        }
        NoteDisplay(p, addr, bytes, ticket, kind == Present);
        // The bytes it reads on the page, with the page's other live reads' (StoreMisses).
        const PAddr page_addr = AddressOf(p);
        const u16 lo = static_cast<u16>(std::max(addr, page_addr) - page_addr);
        const u16 hi = static_cast<u16>(
            std::min<u64>(u64{addr} + bytes, u64{page_addr} + PageSize) - page_addr);
        if (Live(read_ticket[p], last_reached)) {
            AddRange(read_lo[p], read_hi[p], lo, hi);
        } else {
            read_lo[p] = {lo, 0};
            read_hi[p] = {hi, 0};
        }
        read_ticket[p] = ticket;
        if (!geometry) {
            other_read[p] = ticket;
        }
#ifdef VITA_DIAGNOSTICS
        kinds[p] = kind;
        ranges[p] = {addr, bytes};
#endif
        if (versions[p].on) {
            // A changed page that later work in the frame reads: the GPU's MMU onto the guest's
            // slot before it. Protected by the frame (StartFrame), not by this work.
            if (Changed(p)) {
                RemapToGuest(p, ticket, !versions[p].cached);
#ifdef VITA_DIAGNOSTICS
                stats.early_remaps++;
#endif
            }
            versions[p].direct |= kind != Texture;
            versions[p].use_frame = frame;
            if (!versions[p].cached) {
                continue;
            }
            // Protected by the work: closed from this read until the guest's next write.
            versions[p].armed = true;
        }
        if (applied[p] < ReadOnly) {
            Change(p, ReadOnly);
        }
        if (geometry && written[p] != 0) {
            to_clean.push_back(p);
        }
    }
}

void PageHolds::CopyRead(PAddr addr, u32 bytes, u32 ticket, Kind kind) {
    u32 first, count;
    if (!PagesOf(addr, bytes, first, count)) {
        return;
    }
    if (first + count > fcram_pages) {
        Read(addr, bytes, ticket, kind);
        return;
    }
    DEBUG_ASSERT_MSG(open_ticket == 0 || open_ticket == ticket,
                     "page holds: ticket {} while {}'s are open", ticket, open_ticket);
    open_ticket = ticket;
    if (Seen(addr, bytes, 3)) {
        return;
    }
    const u32 done = Processed();
    for (u32 p = first; p < first + count; p++) {
        // The bytes it reads on the page, with the page's other live copy reads' (StoreMisses).
        const PAddr page_addr = AddressOf(p);
        const u16 lo = static_cast<u16>(std::max(addr, page_addr) - page_addr);
        const u16 hi = static_cast<u16>(
            std::min<u64>(u64{addr} + bytes, u64{page_addr} + PageSize) - page_addr);
        if (Live(copy_ticket[p], done)) {
            AddRange(copy_lo[p], copy_hi[p], lo, hi);
        } else {
            copy_lo[p] = {lo, 0};
            copy_hi[p] = {hi, 0};
        }
        copy_ticket[p] = ticket;
#ifdef VITA_DIAGNOSTICS
        kinds[p] = kind;
        ranges[p] = {addr, bytes};
#endif
        if (applied[p] < ReadOnly) {
            Change(p, ReadOnly);
        }
    }
}

PageHolds::CopyTaken PageHolds::TakeCopy(u32 page, u32 reached) {
    // Live at CopyHeld; the render thread may have run its ticket since (Processed moves on).
    DEBUG_ASSERT(page < fcram_pages && copy_ticket[page] != 0);
    const CopyTaken t{copy_from[page], copy_ticket[page], copy_lo[page], copy_hi[page]};
    copy_from[page] = copy_ticket[page];
    copy_ticket[page] = 0;
    copy_lo[page] = {};
    copy_hi[page] = {};
    const Access wanted = Wanted(page, reached);
    if (wanted < applied[page]) {
        Change(page, wanted);
    }
    return t;
}

void PageHolds::Write(PAddr addr, u32 bytes, u32 ticket, [[maybe_unused]] Kind kind) {
    u32 first, count;
    if (!PagesOf(addr, bytes, first, count)) {
        return;
    }
    DEBUG_ASSERT_MSG(open_ticket == 0 || open_ticket == ticket,
                     "page holds: ticket {} while {}'s are open", ticket, open_ticket);
    open_ticket = ticket;
    if (Seen(addr, bytes, 1)) {
        return;
    }
    for (u32 p = first; p < first + count; p++) {
        // A draw's colour buffer moves to CDRAM at its pass (Moves).
        if (kind == Colour && p < fcram_pages) {
            NoteMove(p, ticket);
        }
        if (p < fcram_pages && versions[p].on && versions[p].cached) {
            LeaveVersions(p, ticket);
        }
        // The GPU writes the guest's slot.
        if (p < fcram_pages && versions[p].on) {
            if (Changed(p)) {
                RemapToGuest(p, ticket, true);
#ifdef VITA_DIAGNOSTICS
                stats.early_remaps++;
#endif
            }
            versions[p].direct = true;
            versions[p].use_frame = frame;
        }
        if (p < fcram_pages) {
            NoteDisplay(p, addr, bytes, ticket, kind == DisplayOut);
        }
        // The bytes it writes on the page, with the page's other live writes' (TouchesWrite), as
        // two ranges: a game can read a header between two display-transfer outputs on one page.
        const PAddr page_addr = AddressOf(p);
        const u16 lo = static_cast<u16>(std::max(addr, page_addr) - page_addr);
        const u16 hi = static_cast<u16>(
            std::min<u64>(u64{addr} + bytes, u64{page_addr} + PageSize) - page_addr);
        if (Live(write_ticket[p], last_reached)) {
            AddRange(write_lo[p], write_hi[p], lo, hi);
        } else {
            write_lo[p] = {lo, 0};
            write_hi[p] = {hi, 0};
        }
        write_ticket[p] = ticket;
        write_run[p] = first << 16 | count;
        if (list_kept[p] != 0) {
            list_writes.push_back({page_addr + lo, page_addr + hi});
        }
        if (p < fcram_pages) {
            index_stamp[p]++;
            index_watch[p] = 0;
        }
#ifdef VITA_DIAGNOSTICS
        kinds[p] = kind;
        ranges[p] = {addr, bytes};
        write_kinds[p] = kind;
        write_ranges[p] = {addr, bytes};
#endif
        if (applied[p] < NoAccess) {
            Change(p, NoAccess);
        }
    }
}

void PageHolds::Moves(PAddr addr, u32 bytes, u32 ticket) {
    u32 first, count;
    if (!PagesOf(addr, bytes, first, count) || first >= fcram_pages) {
        return;
    }
    // A range this ticket noted already: the textures a list's draws share, each draw's.
    const std::array<u32, 3> key{addr, bytes, ticket};
    auto& slot = moves_seen[((addr >> 12) ^ bytes) % moves_seen.size()];
    if (slot == key) {
        return;
    }
    slot = key;
    for (u32 p = first; p < first + count; p++) {
        if (versions[p].on && versions[p].cached) {
            LeaveVersions(p, ticket);
        }
        NoteMove(p, ticket);
    }
}

void PageHolds::Close(u32 ticket) {
    if (open_ticket == 0) {
        return;
    }
    DEBUG_ASSERT_MSG(open_ticket == ticket, "page holds: ticket {} closes {}'s holds", ticket,
                     open_ticket);
    open_ticket = 0;
}

PageHolds::Access PageHolds::Wanted(u32 page, u32 reached) const {
    // Rows a picture owns: closed whole until copied (RenderThread::AskPictureCopy).
    if (picture_owned && picture_owned(AddressOf(page), PageSize)) {
        return NoAccess;
    }
    const Access holds = HoldsWant(page, reached);
    return (Watched(page) || parse_held[page] != 0 || IndexWatched(page) ||
            picture_watch[page] != 0 || list_kept[page] != 0) &&
                   holds < ReadOnly
               ? ReadOnly
               : holds;
}

void PageHolds::HoldParse(PAddr addr, u32 bytes) {
    u32 first, count;
    if (!PagesOf(addr, bytes, first, count)) {
        return;
    }
    for (u32 p = first; p < first + count; p++) {
        DEBUG_ASSERT_MSG(parse_held[p] != 0xffff, "page holds: page {:#010x} in 65535 lists",
                         AddressOf(p));
        if (parse_held[p]++ == 0 && applied[p] < ReadOnly) {
            Change(p, ReadOnly);
        }
    }
}

void PageHolds::ReleaseParse(PAddr addr, u32 bytes, const std::vector<u32>& released) {
    u32 first, count;
    if (!PagesOf(addr, bytes, first, count)) {
        return;
    }
    for (u32 p = first; p < first + count; p++) {
        if (std::find(released.begin(), released.end(), p) == released.end()) {
            ReleaseParsePage(p, 1);
        }
    }
}

void PageHolds::ReleaseParsePage(u32 page, u32 count) {
    DEBUG_ASSERT_MSG(parse_held[page] >= count, "page holds: page {:#010x}: {} parse holds let go of {}",
                     AddressOf(page), count, parse_held[page]);
    parse_held[page] = static_cast<u16>(parse_held[page] - count);
    if (parse_held[page] == 0) {
        const Access wanted = Wanted(page, last_reached);
        if (wanted < applied[page]) {
            Change(page, wanted);
        }
    }
}

const u8* PageHolds::HostOf(u32 page) const {
    return page < fcram_pages ? fcram_host + (static_cast<uintptr_t>(page) << PageBits)
                              : vram_host + (static_cast<uintptr_t>(page - fcram_pages) << PageBits);
}

void PageHolds::KeepList(PAddr addr, u32 bytes) {
    u32 first, count;
    if (!PagesOf(addr, bytes, first, count)) {
        return;
    }
    for (u32 p = first; p < first + count; p++) {
        if (list_kept[p] != 0) {
            continue;
        }
        list_kept[p] = 1;
        if (applied[p] < ReadOnly) {
            Change(p, ReadOnly);
        }
    }
}

void PageHolds::UnkeepList(u32 page, u32 reached) {
    list_kept[page] = 0;
    const Access wanted = Wanted(page, reached);
    if (wanted < applied[page]) {
        Change(page, wanted);
    }
    Flush();
}

void PageHolds::Watch(PAddr addr, u32 bytes) {
    auto& seen_slot =
        watch_seen[((addr >> 4) ^ (addr >> 12) ^ (bytes * 0x9E3779B1u)) % watch_seen.size()];
    if (((seen_slot[0] ^ addr) | (seen_slot[1] ^ bytes) | (seen_slot[2] ^ unwatches)) == 0) {
        return;
    }
    u32 first, count;
    if (!PagesOf(addr, bytes, first, count) || first >= fcram_pages) {
        return;
    }
    // The texture's bytes on each page (StoreMisses): only the first and last pages can hold
    // part of it. A page already watched over these bytes costs a compare.
    const u32 last = first + count - 1;
    const u16 first_lo = static_cast<u16>(addr & (PageSize - 1));
    const u16 last_hi = static_cast<u16>(((addr + bytes - 1) & (PageSize - 1)) + 1);
    for (u32 p = first; p <= last; p++) {
        const u16 lo = p == first ? first_lo : 0;
        const u16 hi = p == last ? last_hi : static_cast<u16>(PageSize);
        if (watched[p] == 0) {
            watched[p] = 1;
            watch_lo[p] = {lo, 0};
            watch_hi[p] = {hi, 0};
            if (applied[p] < ReadOnly) {
                Change(p, ReadOnly);
            }
        } else if (!(watch_lo[p][0] <= lo && hi <= watch_hi[p][0]) &&
                   !(watch_hi[p][1] != 0 && watch_lo[p][1] <= lo && hi <= watch_hi[p][1])) {
            AddRange(watch_lo[p], watch_hi[p], lo, hi);
        }
    }
    seen_slot = {addr, bytes, unwatches};
}

bool PageHolds::IndexMemoable(u32 first, u32 count) const {
    for (u32 p = first; p < first + count; p++) {
        if (p >= fcram_pages || versions[p].on || frame - index_write_frame[p] < IndexQuietFrames) {
            return false;
        }
    }
    return true;
}

u32 PageHolds::IndexStamp(u32 first, u32 count) const {
    u32 stamp = 0;
    for (u32 p = first; p < first + count; p++) {
        stamp += index_stamp[p];
    }
    return stamp;
}

void PageHolds::WatchIndices(u32 first, u32 count) {
    for (u32 p = first; p < first + count; p++) {
        if (index_watch[p] != 0) {
            continue;
        }
        index_watch[p] = 1;
        if (applied[p] < ReadOnly) {
            Change(p, ReadOnly);
        }
    }
}

void PageHolds::IndexWritten(u32 page, u32 reached) {
    index_stamp[page]++;
    index_write_frame[page] = frame;
    index_watch[page] = 0;
    // An open page has nothing to open.
    if (applied[page] == Unheld) {
        return;
    }
    const Access wanted = Wanted(page, reached);
    if (wanted < applied[page]) {
        Change(page, wanted);
    }
    Flush();
}

namespace {
std::atomic<bool> readback_holds{true};
} // namespace

void PageHolds::SetReadbackHolds(bool on) {
    readback_holds.store(on, std::memory_order_relaxed);
}


bool PageHolds::TouchesWatch(const u8* host, u32 bytes) const {
    const u32 page = PageOf(host);
    const u32 at = static_cast<u32>(host - fcram_host) & (PageSize - 1);
    DEBUG_ASSERT(page < fcram_pages && at + bytes <= PageSize);
    const auto& lo = watch_lo[page];
    const auto& hi = watch_hi[page];
    return (hi[0] != 0 && at < hi[0] && at + bytes > lo[0]) ||
           (hi[1] != 0 && at < hi[1] && at + bytes > lo[1]);
}

void PageHolds::Unwatch(u32 page, u32 reached) {
    unwatches++;
    watched[page] = 0;
    watch_lo[page] = {};
    watch_hi[page] = {};
    const Access wanted = Wanted(page, reached);
    if (wanted < applied[page]) {
        Change(page, wanted);
    }
    Flush();
}

PageHolds::Access PageHolds::HoldsWant(u32 page, u32 reached) const {
    if (Live(write_ticket[page], reached) && readback_holds.load(std::memory_order_relaxed)) {
        return NoAccess;
    }
    if (page >= fcram_pages) {
        return vram_held ? ReadOnly : Unheld;
    }
    // The render thread's CPU reads (CopyRead), a versioned page's too: a version moves the
    // guest's view, which they read.
    if (CopyLive(page)) {
        return ReadOnly;
    }
    if (versions[page].on) {
        // Protected by the frame: closed to writes while the frame's arming lasts.
        return versions[page].armed ? ReadOnly : Unheld;
    }
    return Live(read_ticket[page], reached) ? ReadOnly : Unheld;
}

u32 PageHolds::WaitFor(u32 page, bool write, u32 reached) const {
    const bool writer = Live(write_ticket[page], reached) &&
                        readback_holds.load(std::memory_order_relaxed);
    // A versioned page: writes go on without a wait, but for a queued GPU write to it, which a
    // write waits for even with Wait for GPU off: Version takes no slot past a queued GPU write.
    if (page < fcram_pages && versions[page].on) {
        return writer || (write && Live(write_ticket[page], reached)) ? write_ticket[page] : 0;
    }
    u32 ticket = writer ? write_ticket[page] : 0;
    if (write) {
        const u32 read = page >= fcram_pages ? vram_read : read_ticket[page];
        if (Live(read, reached) && (ticket == 0 || static_cast<s32>(read - ticket) > 0)) {
            ticket = read;
        }
    }
    return ticket;
}

u32 PageHolds::GpuReadWaitFor(const u8* host, u32 bytes, u32 reached) const {
    const u32 page = PageOf(host);
    if (page == NoPage || !Live(write_ticket[page], reached) || !TouchesWrite(host, bytes)) {
        return 0;
    }
    return write_ticket[page];
}

void PageHolds::AddRange(std::array<u16, 2>& lo, std::array<u16, 2>& hi, u16 l, u16 h) {
    // The range it overlaps or touches, else a free one, else the nearer one (hi 0: none).
    u32 k = 2;
    for (u32 i = 0; i < 2 && k == 2; i++) {
        if (hi[i] != 0 && l <= hi[i] && h >= lo[i]) {
            k = i;
        }
    }
    if (k == 2) {
        const auto gap = [&](u32 i) { return l > hi[i] ? l - hi[i] : lo[i] - h; };
        k = hi[1] == 0 ? 1 : gap(0) <= gap(1) ? 0 : 1;
    }
    lo[k] = hi[k] == 0 ? l : std::min(lo[k], l);
    hi[k] = std::max(hi[k], h);
    // Grown into each other: one.
    if (hi[1] != 0 && lo[0] <= hi[1] && lo[1] <= hi[0]) {
        lo = {std::min(lo[0], lo[1]), 0};
        hi = {std::max(hi[0], hi[1]), 0};
    }
}

void PageHolds::NoteDisplay(u32 p, PAddr addr, u32 bytes, u32 ticket, bool display) {
    if (!display) {
        other_ticket[p] = ticket;
        return;
    }
    const PAddr page_addr = AddressOf(p);
    const u16 l = static_cast<u16>(std::max(addr, page_addr) - page_addr);
    const u16 h = static_cast<u16>(std::min(addr + bytes, page_addr + PageSize) - page_addr);
    if (!Live(display_ticket[p], last_reached)) {
        display_lo[p] = {l, 0};
        display_hi[p] = {h, 0};
    } else {
        AddRange(display_lo[p], display_hi[p], l, h);
    }
    display_ticket[p] = ticket;
}

bool PageHolds::DisplayOnly(u32 page, u32 reached) const {
    if (page == NoPage || page >= fcram_pages || !Live(display_ticket[page], reached) ||
        Live(other_ticket[page], reached) || CopyLive(page) || watched[page] != 0 ||
        parse_held[page] != 0 || index_watch[page] != 0) {
        return false;
    }
    const PageVersion& v = versions[page];
    return !(v.on && v.gpu != v.guest);
}

bool PageHolds::OpenPastDisplay(const u8* host, u32 reached, u32 reach) {
    const u32 page = PageOf(host);
    if (!DisplayOnly(page, reached)) {
        return false;
    }
    const u32 offset =
        static_cast<u32>(host - (fcram_host + (static_cast<uintptr_t>(page) << PageBits)));
    // Clear of the display bytes: nothing of what the aborted instruction may touch from there on,
    // [offset, offset + reach), in a range (a flat margin shut reads just before a framebuffer).
    const auto& lo = display_lo[page];
    const auto& hi = display_hi[page];
    for (u32 i = 0; i < 2; i++) {
        if (hi[i] != 0 && offset < hi[i] && offset + reach > lo[i]) {
            return false;
        }
    }
    Change(page, Unheld);
    Flush();
    return true;
}

void PageHolds::Release(u32 reached) {
    last_reached = reached;
    Flush();
}

void PageHolds::LetGo(u32 first, u32 count, u32 reached) {
    for (u32 p = first; p < first + count; p++) {
        // Nothing to change: an FCRAM page, not versioned, closed to writes or open (a live
        // write would have closed it whole), or a VRAM page the GPU did not write.
        if (p < fcram_pages ? applied[p] <= ReadOnly && !versions[p].on
                            : applied[p] != NoAccess) {
            continue;
        }
        Access wanted = Wanted(p, reached);
        // Letting go and holding again costs two descriptor changes a page: an FCRAM page whose
        // work is all done stays closed to writes, reads going on, and a VRAM page the GPU wrote
        // likewise (the next frame's passes write the same render targets). A write opens either as
        // any abort on a page with no live hold (Reopen).
        if (wanted == Unheld && p < fcram_pages && !versions[p].on) {
            wanted = ReadOnly;
        }
        if (wanted < applied[p]) {
            Change(p, wanted);
        }
    }
}

void PageHolds::Reopen(u32 page, u32 reached) {
    last_reached = reached;
    // The page's last write hold done: its other pages go with it, as its ticket's release
    // would have let go of them, so the guest's next accesses there do not fault one by one.
    const u32 run = write_run[page];
    if (run != 0 && !Live(write_ticket[page], reached)) {
        LetGo(run >> 16, run & 0xffff, reached);
        write_run[page] = 0;
    }
    Access wanted = Wanted(page, reached);
    // A VRAM page the guest writes opens until the next GPU read of VRAM holds it again.
    if (page >= fcram_pages && wanted == ReadOnly && !Live(vram_read, reached) &&
        parse_held[page] == 0 && picture_watch[page] == 0 && list_kept[page] == 0) {
        wanted = Unheld;
        vram_opened.push_back(page);
    }
    if (wanted < applied[page]) {
        Change(page, wanted);
    }
    Flush();
}

void PageHolds::Resend(const u8* host, u32 size) {
    const u8* const end = host + size;
    for (const u8* p = host; p < end; p += PageSize) {
        const u32 page = PageOf(p);
        if (page != NoPage && applied[page] != Unheld) {
            changed.push_back(page);
        }
    }
    Flush();
}

void PageHolds::Apply() {
    Flush();
}

void PageHolds::BeginBatch() {
    DEBUG_ASSERT(!batch);
    batch = true;
}

void PageHolds::EndBatch() {
    DEBUG_ASSERT(batch);
    batch = false;
    Flush();
}

u16 PageHolds::AllocSlot(bool cached) {
    std::vector<u16>& free = cached ? free_cached_slots : free_slots;
    ASSERT_MSG(!free.empty(), "page holds: the {} KiB {} versions' pool is full",
               (cached ? CachedPoolSize : VersionPoolSize) >> 10, cached ? "cached" : "CDRAM");
    const u16 s = free.back();
    free.pop_back();
    return s;
}

void PageHolds::FreeSlot(u16 slot) {
    (slot >= CachedSlot ? free_cached_slots : free_slots).push_back(slot);
}

u8* PageHolds::SlotHost(u16 slot) const {
    return slot >= CachedSlot
               ? cached_pool->Data() + (static_cast<u32>(slot - CachedSlot) << PageBits)
               : version_pool->Data() + (static_cast<u32>(slot) << PageBits);
}

void PageHolds::Flip(u32 page, u16 slot, bool copy) {
    const u32 va = static_cast<u32>(reinterpret_cast<uintptr_t>(fcram_host + (page << PageBits)));
    const u32 to =
        slot == NoSlot ? 0u : static_cast<u32>(reinterpret_cast<uintptr_t>(SlotHost(slot)));
    flips.push_back({va | (copy ? AZAHAR_FLIP_COPY : 0u), to});
    // The bytes reach the page the guest goes to through the cache.
    written[page] = 1;
#ifdef VITA_DIAGNOSTICS
    written_by[page] |= ByFlip;
#endif
}

void PageHolds::LeaveVersions(u32 page, u32 ticket) {
    PageVersion& v = versions[page];
    DEBUG_ASSERT(v.on && v.cached);
    ASSERT_MSG(!batch, "page holds: page {:#010x} leaves its versions inside a batch",
               AddressOf(page));
    if (v.guest != 0) {
        // Home with the guest's bytes, once the queued work that reads the home's is done.
        const u32 home = v.gpu == 0 ? read_ticket[page] : v.busy[0];
        if (Live(home, last_reached)) {
            last_reached = wait_ticket(home);
        }
        Flip(page, NoSlot, true);
        Flush();
        v.guest = 0;
        // The ranges held for the open ticket are read again.
        seen = {};
    }
    if (v.gpu != 0) {
        RemapToGuest(page, ticket, true);
    }
    for (u8 s = 1; s < Slots; s++) {
        if (v.pool[s - 1] != NoSlot) {
            retired_slots.push_back({v.pool[s - 1], v.busy[s]});
        }
    }
    v = PageVersion{};
    std::erase(versioned, page);
#ifdef VITA_DIAGNOSTICS
    stats.left_versions++;
#endif
    const Access wanted = Wanted(page, last_reached);
    if (wanted != applied[page]) {
        Change(page, wanted);
    }
}

void PageHolds::LeaveVersions(PAddr addr, u32 bytes, u32 ticket) {
    u32 first, count;
    if (!PagesOf(addr, bytes, first, count) || first >= fcram_pages) {
        return;
    }
    for (u32 p = first; p < first + count; p++) {
        if (versions[p].on && versions[p].cached) {
            LeaveVersions(p, ticket);
        }
    }
    Flush();
}

bool PageHolds::HasCachedVersions(PAddr addr, u32 bytes) const {
    u32 first, count;
    if (!PagesOf(addr, bytes, first, count) || first >= fcram_pages) {
        return false;
    }
    for (u32 p = first; p < first + count; p++) {
        if (versions[p].on && versions[p].cached) {
            return true;
        }
    }
    return false;
}

void PageHolds::AllWritten() {
    std::fill(written.begin(), written.end(), u8{1});
#ifdef VITA_DIAGNOSTICS
    std::fill(written_by.begin(), written_by.end(), u8{ByAll});
#endif
    seen = {};
}

void PageHolds::CleanGeometry(const std::function<bool(PAddr)>& cached) {
    if (to_clean.empty()) {
        return;
    }
    std::sort(to_clean.begin(), to_clean.end());
    to_clean.erase(std::unique(to_clean.begin(), to_clean.end()), to_clean.end());
    // But the pages moved to CDRAM, and those open again (a list left out after its holds).
    std::erase_if(to_clean,
                  [&](u32 p) { return applied[p] == Unheld || !cached(AddressOf(p)); });
    if (to_clean.empty()) {
        return;
    }
    ASSERT_MSG(!batch, "page holds: a vertex or index read inside a batch");
    Flush();
    for (std::size_t at = 0; at < to_clean.size();) {
        std::size_t end = at + 1;
        while (end < to_clean.size() && to_clean[end] == to_clean[end - 1] + 1) {
            end++;
        }
        const u32 bytes = static_cast<u32>(end - at) << PageBits;
        const int r = SgxdCacheRange(fcram_host + (to_clean[at] << PageBits), bytes,
                                     SGXD_CACHE_CLEAN, CacheSite::Vertices);
        ASSERT_MSG(r >= 0, "page holds: no clean of {:#010x}+{:#x}: {:#010x}",
                   AddressOf(to_clean[at]), bytes, static_cast<u32>(r));
        at = end;
    }
    for (const u32 p : to_clean) {
        written[p] = 0;
    }
#ifdef VITA_DIAGNOSTICS
    stats.cleaned_pages += static_cast<u32>(to_clean.size());
    for (const u32 p : to_clean) {
        const u8 by = std::exchange(written_by[p], u8{0});
        if (by == ByPast) {
            stats.cleaned_past++;
            for (const u32 w : past_lines[p]) {
                stats.past_lines += static_cast<u32>(std::popcount(w));
            }
        } else if ((by & ByPast) != 0) {
            stats.cleaned_past_and++;
        } else if ((by & ByFlip) != 0) {
            stats.cleaned_flip++;
        } else {
            stats.cleaned_other++;
        }
        past_lines[p] = {};
    }
#endif
    to_clean.clear();
}

#ifdef VITA_DIAGNOSTICS
void PageHolds::NotePast(u32 page, const u8* host, u32 bytes) {
    written_by[page] |= ByPast;
    const u32 at = static_cast<u32>(host - fcram_host) - (page << PageBits);
    DEBUG_ASSERT(bytes != 0 && at + bytes <= (1u << PageBits));
    for (u32 line = at >> 5; line <= (at + bytes - 1) >> 5; line++) {
        past_lines[page][line >> 5] |= 1u << (line & 31);
    }
}
#endif

bool PageHolds::Version(u32 page, u32 reached, bool cached) {
    DEBUG_ASSERT(page < fcram_pages);
    PageVersion& v = versions[page];
    ASSERT_MSG(!v.on || v.cached == cached,
               "page holds: page {:#010x} has {} versions and is {}", AddressOf(page),
               v.cached ? "cached" : "CDRAM", cached ? "cached" : "moved");
    // A GPU write queued: the guest's write waits for it (WaitFor, whatever Wait for GPU says).
    // A live CopyRead: the render thread reads the guest's view, which a version moves, and a
    // version opens the page for the frame, the held bytes too (the caller takes their copy
    // first: RenderThread::CopyFault).
    if (Live(write_ticket[page], reached) || CopyLive(page)) {
        return false;
    }
    u8* const host = fcram_host + (page << PageBits);
    const bool read = Live(read_ticket[page], reached);
    if (!v.on) {
        // Nothing queued reads it: the ordinary holds open it.
        if (!read) {
            return false;
        }
        v = PageVersion{};
        v.cached = cached;
        v.pool[0] = AllocSlot(cached);
        v.on = true;
        index_stamp[page]++;
        index_watch[page] = 0;
        versioned.push_back(page);
#ifdef VITA_DIAGNOSTICS
        stats.versions++;
#endif
    }
    // Onto a slot no queued work reads when queued work reads the guest's: the GPU's MMU keeps
    // the slot its queued work reads. Otherwise the write goes where the guest is, without a wait.
    u8 target = Slots;
    if (read && !Changed(page)) {
        for (u8 i = 1; i < Slots && target == Slots; i++) {
            const u8 s = static_cast<u8>((v.guest + i) % Slots);
            if ((s == 0 || v.pool[s - 1] != NoSlot) && !Live(v.busy[s], reached)) {
                target = s;
            }
        }
        for (u8 s = 1; s < Slots && target == Slots; s++) {
            if (v.pool[s - 1] == NoSlot) {
                v.pool[s - 1] = AllocSlot(v.cached);
                target = s;
            }
        }
    }
    if (target != Slots) {
        if (target == 0) {
            // Home, the bytes with it: copied through the app's view before the guest's
            // windows follow, so the second emulation thread never reads the home's old bytes.
            Flip(page, NoSlot, true);
        } else {
            // The page is closed to the guest's writes (the abort that got here): its loads read
            // the same bytes on either page.
            std::memcpy(SlotHost(v.pool[target - 1]), host, PageSize);
            Flip(page, v.pool[target - 1]);
        }
        v.guest = target;
        // The ranges held for the open ticket are read again: a later read remaps.
        seen = {};
#ifdef VITA_DIAGNOSTICS
        stats.swaps++;
#endif
    } else {
        // A cached-pool page's next read closes it again.
        if (v.cached) {
            seen = {};
        }
#ifdef VITA_DIAGNOSTICS
        (read ? stats.in_place_read : stats.in_place)++;
#endif
    }
    // Open for the rest of the frame.
    v.armed = false;
    v.use_frame = frame;
    if (applied[page] != Unheld) {
        Change(page, Unheld);
    }
    Flush();
    return true;
}

void PageHolds::RemapToGuest(u32 page, u32 ticket, bool cache_clean) {
    ASSERT_MSG(cache_clean || (versions[page].cached && written[page] != 0),
               "page holds: page {:#010x} remapped with no cache clean to come", AddressOf(page));
    // The PTE word now, while the app's view shows the guest's slot of this ticket.
    u32 out[2];
    const int r = sgxdPte(AddressOf(page), fcram_host + (page << PageBits),
                          cache_clean ? 0u : SGXD_PTE_NO_CACHE_CLEAN, out);
    ASSERT_MSG(r == 0, "page holds: no remap of page {:#010x}: {:#010x}", AddressOf(page),
               static_cast<u32>(r));
    PageVersion& v = versions[page];
    remaps.push_back({AddressOf(page), out[0], out[1], v.direct ? 1u : 0u});
    // The slot the GPU leaves is read by queued work up to this ticket.
    v.busy[v.gpu] = ticket;
    v.gpu = v.guest;
    v.direct = false;
    v.use_frame = frame;
}

void PageHolds::StartFrame(u32 reached, u32 ticket) {
    frame++;
    std::erase_if(retired_slots, [&](const std::pair<u16, u32>& retired) {
        if (Live(retired.second, reached)) {
            return false;
        }
        FreeSlot(retired.first);
        return true;
    });
    std::size_t kept = 0;
    for (const u32 page : versioned) {
        PageVersion& v = versions[page];
        // The changed pages onto the GPU, in one batch at the frame's start.
        if (Changed(page)) {
            RemapToGuest(page, ticket, !v.cached);
#ifdef VITA_DIAGNOSTICS
            stats.frame_remaps++;
#endif
        }
        const bool idle = frame - v.use_frame >= ConvergeFrames &&
                          !Live(read_ticket[page], reached) && !Live(write_ticket[page], reached);
        const bool pool_read = std::any_of(v.busy.begin() + 1, v.busy.end(),
                                           [&](u32 busy) { return Live(busy, reached); });
        if (idle && v.guest == 0 && v.gpu == 0 && !pool_read) {
            // Home on both sides and nothing reads its pool slots: they go back, one page again.
            for (const u16 slot : v.pool) {
                if (slot != NoSlot) {
                    FreeSlot(slot);
                }
            }
            v = PageVersion{};
#ifdef VITA_DIAGNOSTICS
            stats.homes++;
#endif
        } else {
            if (idle && v.guest != 0 && v.guest == v.gpu && !Live(v.busy[0], reached)) {
                // Unused for a while on a pool slot: the guest goes home with its bytes (no
                // queued work reads the home), the GPU at the next frame's start.
                Flip(page, NoSlot, true);
                Flush();
                v.guest = 0;
            }
            // A cached-pool page is armed by its reads (Read).
            if (!v.cached) {
                v.armed = true;
            }
            versioned[kept++] = page;
        }
        const Access wanted = Wanted(page, reached);
        if (wanted != applied[page]) {
            Change(page, wanted);
        }
    }
    versioned.resize(kept);
    Flush();
}

void PageHolds::Flush() {
    if (batch) {
        return;
    }
    for (std::size_t at = 0; at < flips.size(); at += AZAHAR_FLIP_MAX_RUNS) {
        const u32 n =
            static_cast<u32>(std::min<std::size_t>(AZAHAR_FLIP_MAX_RUNS, flips.size() - at));
        const int r = azaharFlip(reinterpret_cast<const AzaharFlipRun*>(flips.data() + at), n);
        ASSERT_MSG(r >= 0, "page holds: azaharFlip of {} pages failed: {:#010x}", n,
                   static_cast<u32>(r));
#ifdef VITA_DIAGNOSTICS
        stats.flip_calls++;
#endif
    }
    flips.clear();
    if (changed.empty()) {
        return;
    }
    // In page order, so that neighbours share a run; a page changed twice is sent once, as it
    // is now.
    std::sort(changed.begin(), changed.end());
    changed.erase(std::unique(changed.begin(), changed.end()), changed.end());
    calls_runs.clear();
    for (const u32 p : changed) {
#ifdef VITA_DIAGNOSTICS
        sent[p] = applied[p];
#endif
        const u8* const host = p < fcram_pages ? fcram_host + (p << PageBits)
                                               : vram_host + ((p - fcram_pages) << PageBits);
        const u32 va = static_cast<u32>(reinterpret_cast<uintptr_t>(host));
        if (!calls_runs.empty()) {
            auto& last = calls_runs.back();
            if (last[0] + last[1] == va && last[2] == applied[p]) {
                last[1] += PageSize;
                continue;
            }
        }
        calls_runs.push_back({va, PageSize, applied[p]});
    }
    changed.clear();
    for (std::size_t at = 0; at < calls_runs.size(); at += AZAHAR_HOLD_MAX_RUNS) {
        const u32 n =
            static_cast<u32>(std::min<std::size_t>(AZAHAR_HOLD_MAX_RUNS, calls_runs.size() - at));
        const int r = azaharHold(reinterpret_cast<const AzaharHoldRun*>(calls_runs.data() + at), n);
        ASSERT_MSG(r >= 0, "page holds: azaharHold of {} runs failed: {:#010x}", n,
                   static_cast<u32>(r));
#ifdef VITA_DIAGNOSTICS
        stats.calls++;
        stats.runs += n;
        stats.descriptors += static_cast<u32>(r);
#endif
    }
}

bool PageHolds::TouchesWrite(const u8* host, u32 bytes) const {
    const u32 page = PageOf(host);
    const u8* const base = page < fcram_pages ? fcram_host : vram_host;
    const u32 at = static_cast<u32>(host - base) & (PageSize - 1);
    DEBUG_ASSERT(page != NoPage && at + bytes <= PageSize);
    const auto& lo = write_lo[page];
    const auto& hi = write_hi[page];
    return (at < hi[0] && at + bytes > lo[0]) || (hi[1] != 0 && at < hi[1] && at + bytes > lo[1]);
}

bool PageHolds::StoreMisses(const u8* host, u32 bytes, u32 reached, bool closed) const {
    const u32 page = PageOf(host);
    // A handed-over list's page has its own path (ParseFault copies first), and so does a
    // versioned one (Version) unless a GPU write is queued to it: then guest and GPU share the
    // guest's slot, so a store beside a render target waits only on its bytes.
    // A closed page's guest slot is the one its live reads read, or none of them is (Changed, until
    // a read remaps it): its bytes decide.
    if (page == NoPage || parse_held[page] != 0) {
        return false;
    }
    const bool fcram = page < fcram_pages;
    if (fcram && !closed && versions[page].on &&
        (Changed(page) || !Live(write_ticket[page], reached))) {
        return false;
    }
    const u32 at = static_cast<u32>(host - (fcram ? fcram_host : vram_host)) & (PageSize - 1);
    DEBUG_ASSERT(at + bytes <= PageSize);
    const auto overlaps = [](const std::array<u16, 2>& lo, const std::array<u16, 2>& hi, u32 l,
                             u32 h) {
        return (hi[0] != 0 && l < hi[0] && h > lo[0]) || (hi[1] != 0 && l < hi[1] && h > lo[1]);
    };
    // The render thread's CPU reads (CopyRead): their bytes take a copy for it first
    // (RenderThread::CopyFault).
    const bool copying = CopyLive(page);
    if (copying && overlaps(copy_lo[page], copy_hi[page], at, at + bytes)) {
        return false;
    }
    // A store into a watched texture's bytes marks it written (the caller unwatches the page
    // first, RenderThread::TakeHeldWrite); one beside them leaves it as it was (TouchesWatch).
    const bool watching = fcram && watched[page] != 0;
    if (watching && overlaps(watch_lo[page], watch_hi[page], at, at + bytes)) {
        return false;
    }
    // The GPU's reads: an FCRAM page's bytes; VRAM's, held as a whole, by the ranges its live
    // reads cover.
    bool reading;
    if (fcram) {
        reading = Live(read_ticket[page], reached);
        if (reading && overlaps(read_lo[page], read_hi[page], at, at + bytes)) {
            return false;
        }
    } else {
        reading = Live(vram_read, reached);
        const PAddr address = AddressOf(page) + at;
        if (reading && VramReadMeets(address, address + bytes, reached)) {
            return false;
        }
    }
    // Clear of the GPU's writes by whole cache lines: a line the store dirties that holds bytes
    // the GPU writes would be written back over them.
    constexpr u32 Line = 64;
    const bool writing = Live(write_ticket[page], reached);
    if (writing && overlaps(write_lo[page], write_hi[page], at & ~(Line - 1),
                            (at + bytes + Line - 1) & ~(Line - 1))) {
        return false;
    }
    return reading || writing || watching || closed || copying;
}

[[gnu::noinline]] void PageHolds::NoteVramRead(PAddr addr, u32 bytes, u32 ticket) {
    // The ranges done dropped from the front.
    while (vram_reads_count != 0 && !Live(vram_reads[vram_reads_head][2], last_reached)) {
        vram_reads_head = (vram_reads_head + 1) % VramReads;
        vram_reads_count--;
    }
    if (vram_reads_count != 0) {
        auto& last = vram_reads[(vram_reads_head + vram_reads_count - 1) % VramReads];
        if ((last[2] == ticket && addr <= last[1] && last[0] <= addr + bytes) ||
            vram_reads_count == VramReads) {
            last[0] = std::min<u32>(last[0], addr);
            last[1] = std::max<u32>(last[1], addr + bytes);
            last[2] = ticket;
            return;
        }
    }
    vram_reads[(vram_reads_head + vram_reads_count) % VramReads] = {addr, addr + bytes, ticket};
    vram_reads_count++;
}

bool PageHolds::VramReadMeets(PAddr lo, PAddr hi, u32 reached) const {
    for (u32 i = 0; i < vram_reads_count; i++) {
        const auto& r = vram_reads[(vram_reads_head + i) % VramReads];
        if (Live(r[2], reached) && lo < r[1] && r[0] < hi) {
            return true;
        }
    }
    return false;
}

PAddr PageHolds::AddressOf(u32 page) const {
    return page < fcram_pages ? Memory::FCRAM_PADDR + (page << PageBits)
                              : Memory::VRAM_PADDR + ((page - fcram_pages) << PageBits);
}

#ifdef VITA_DIAGNOSTICS
PageHolds::Stats PageHolds::TakeStats() {
    stats.slots_used = static_cast<u32>((VersionPoolSize >> PageBits) - free_slots.size());
    stats.cached_slots_used =
        static_cast<u32>((CachedPoolSize >> PageBits) - free_cached_slots.size());
    return std::exchange(stats, {});
}

std::string PageHolds::DisplayState(u32 page, u32 reached) const {
    if (page >= fcram_pages) {
        return "VRAM";
    }
    const PageVersion& v = versions[page];
    return fmt::format("display {} {:#x}-{:#x} {:#x}-{:#x}, other {}, watched {}, version {}; "
                       "reads {} {:#x}-{:#x} {:#x}-{:#x}, writes {} {:#x}-{:#x} {:#x}-{:#x}, "
                       "parse {}, index watch {}",
                       Live(display_ticket[page], reached) ? "live" : "done", display_lo[page][0],
                       display_hi[page][0], display_lo[page][1], display_hi[page][1],
                       Live(other_ticket[page], reached) ? "live" : "done", watched[page],
                       !v.on              ? "none"
                       : v.gpu != v.guest ? "changed"
                                          : "on",
                       Live(read_ticket[page], reached) ? "live" : "done", read_lo[page][0],
                       read_hi[page][0], read_lo[page][1], read_hi[page][1],
                       Live(write_ticket[page], reached) ? "live" : "done", write_lo[page][0],
                       write_hi[page][0], write_lo[page][1], write_hi[page][1], parse_held[page],
                       index_watch[page]);
}

u32 PageHolds::LoweredUnsent() const {
    for (const u32 p : changed) {
        if (applied[p] < sent[p]) {
            return p;
        }
    }
    return NoPage;
}

std::string PageHolds::HoldState(u32 page, u32 reached) const {
    const bool queued = std::find(changed.begin(), changed.end(), page) != changed.end();
    std::string s = fmt::format("applied {} sent {} {} by {:#010x}, parse {}, write {} {}",
                                applied[page], sent[page], queued ? "queued" : "not queued",
                                changed_by[page], parse_held[page], write_ticket[page],
                                Live(write_ticket[page], reached) ? "live" : "done");
    if (page < fcram_pages) {
        const PageVersion& v = versions[page];
        s += fmt::format(", read {} {}, copy {} {} {:#x}-{:#x} {:#x}-{:#x} (render thread at {}), "
                         "watch {}, version on {} armed {} guest {} gpu {}, {} flips queued, {} "
                         "changed queued",
                         read_ticket[page], Live(read_ticket[page], reached) ? "live" : "done",
                         copy_ticket[page], CopyLive(page) ? "live" : "done", copy_lo[page][0],
                         copy_hi[page][0], copy_lo[page][1], copy_hi[page][1], Processed(),
                         index_watch[page], v.on, v.armed, v.guest, v.gpu, flips.size(),
                         changed.size());
    }
    return s;
}

std::string PageHolds::VersionList() const {
    std::string s = fmt::format("{} versioned pages:", versioned.size());
    for (const u32 page : versioned) {
        const PageVersion& v = versions[page];
        s += fmt::format(" {:#010x} (guest {} gpu {}{}{})", AddressOf(page), v.guest, v.gpu,
                         v.armed ? " armed" : "", v.cached ? " cached" : "");
    }
    return s;
}

PageHolds::Kind PageHolds::WaitKind(u32 page, bool write, u32 reached) const {
    if (page >= fcram_pages && write && !Live(write_ticket[page], reached)) {
        return static_cast<Kind>(vram_read_kind);
    }
    return static_cast<Kind>(kinds[page]);
}
#endif

} // namespace SgxdRenderer
