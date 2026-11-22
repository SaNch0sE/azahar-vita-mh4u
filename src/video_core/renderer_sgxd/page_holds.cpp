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
#include "video_core/renderer_sgxd/sgxd_flags.h"
#include "video_core/renderer_sgxd/page_holds.h"

namespace SgxdRenderer {

namespace {

constexpr u32 PageBits = 12;
constexpr u32 PageSize = 1u << PageBits;
/// The versions' pool: CDRAM, uncached for the CPU, as the moved pages it versions are.
constexpr u32 VersionPoolSize = 4u << 20;

static_assert(sizeof(AzaharHoldRun) == sizeof(std::array<u32, 3>));
static_assert(PageHolds::Unheld == AZAHAR_HOLD_NONE && PageHolds::ReadOnly == AZAHAR_HOLD_WRITES &&
              PageHolds::NoAccess == AZAHAR_HOLD_ALL);

} // namespace

PageHolds::PageHolds(Memory::MemorySystem& memory)
    : fcram_host{memory.GetFCRAMPointer(0)},
      vram_host{memory.GetPhysicalPointer(Memory::VRAM_PADDR)},
      fcram_pages{Memory::FCRAM_SIZE >> PageBits},
      pages{fcram_pages + (Memory::VRAM_SIZE >> PageBits)}, read_ticket(fcram_pages),
      write_ticket(pages), applied(pages), watched(fcram_pages) {
#ifdef VITA_DIAGNOSTICS
    kinds.resize(pages);
    ranges.resize(pages);
    write_kinds.resize(pages);
    write_ranges.resize(pages);
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
    versions.resize(fcram_pages);
    const u32 count = VersionPoolSize >> PageBits;
    free_slots.reserve(count);
    for (u32 s = count; s-- > 0;) {
        free_slots.push_back(static_cast<u16>(s));
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

void PageHolds::Change(u32 page, Access access) {
#ifdef VITA_DIAGNOSTICS
    if (access > applied[page]) {
        stats.held_pages++;
    } else {
        stats.released_pages++;
    }
#endif
    applied[page] = access;
    changed.push_back(page);
}

const char* PageHolds::KindName(Kind kind) {
    static constexpr const char* names[Kinds] = {
        "texture",     "depth",        "index",  "vertex",  "colour", "fill",
        "transfer in", "transfer out", "dma in", "dma out", "present"};
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
        // VRAM is held for reads as a whole (the class comment).
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
    for (u32 p = first; p < first + count; p++) {
        read_ticket[p] = ticket;
        geometry_read[p] = kind == Vertex || kind == Index ? 1 : 0;
#ifdef VITA_DIAGNOSTICS
        kinds[p] = kind;
        ranges[p] = {addr, bytes};
#endif
        if (versions[p].on) {
            // A changed page that later work in the frame reads: the GPU's MMU onto the guest's
            // slot before it. Protected by the frame (StartFrame), not by this work.
            if (Changed(p)) {
                RemapToGuest(p, ticket);
#ifdef VITA_DIAGNOSTICS
                stats.early_remaps++;
#endif
            }
            versions[p].direct |= kind != Texture;
            versions[p].use_frame = frame;
            continue;
        }
        if (applied[p] < ReadOnly) {
            Change(p, ReadOnly);
        }
    }
    open_runs.push_back({first, count});
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
        // The GPU writes the guest's slot.
        if (p < fcram_pages && versions[p].on) {
            if (Changed(p)) {
                RemapToGuest(p, ticket);
#ifdef VITA_DIAGNOSTICS
                stats.early_remaps++;
#endif
            }
            versions[p].direct = true;
            versions[p].use_frame = frame;
        }
        write_ticket[p] = ticket;
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
    open_runs.push_back({first, count});
}

void PageHolds::Close(u32 ticket) {
    if (open_ticket == 0) {
        return;
    }
    DEBUG_ASSERT_MSG(open_ticket == ticket, "page holds: ticket {} closes {}'s holds", ticket,
                     open_ticket);
    if (!open_runs.empty()) {
        groups.push_back({ticket, std::move(open_runs)});
        open_runs.clear();
    }
    open_ticket = 0;
}

PageHolds::Access PageHolds::Wanted(u32 page, u32 reached) const {
    const Access holds = HoldsWant(page, reached);
    return Watched(page) && holds < ReadOnly ? ReadOnly : holds;
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
            watch_ticket[p]++;
            RefreshPermit(p);
            if (applied[p] < ReadOnly) {
                Change(p, ReadOnly);
            }
        } else if (!(watch_lo[p][0] <= lo && hi <= watch_hi[p][0]) &&
                   !(watch_hi[p][1] != 0 && watch_lo[p][1] <= lo && hi <= watch_hi[p][1])) {
            AddRange(watch_lo[p], watch_hi[p], lo, hi);
            RefreshPermit(p);
        }
    }
    seen_slot = {addr, bytes, unwatches};
}

void PageHolds::Watch(PAddr addr, u32 bytes) {
    u32 first, count;
    if (!PagesOf(addr, bytes, first, count) || first >= fcram_pages) {
        return;
    }
    // The texture's bytes on each page (StoreMisses lets the guest's stores clear of them
    // through): only the first and last pages can hold part of it. Every draw watches its
    // textures again, so a page already watched over these bytes costs a compare (merging every
    // page's range each time was ~5% of a game's emulation thread).
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
            watch_ticket[p]++;
            if (applied[p] < ReadOnly) {
                Change(p, ReadOnly);
            }
        } else if (!(watch_lo[p][0] <= lo && hi <= watch_hi[p][0]) &&
                   !(watch_hi[p][1] != 0 && watch_lo[p][1] <= lo && hi <= watch_hi[p][1])) {
            AddRange(watch_lo[p], watch_hi[p], lo, hi);
        }
    }
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
        // A permit's bytes miss what the queued work reads, not every list scanned on the page.
        VoidPermit(p);
        if (applied[p] < ReadOnly) {
            Change(p, ReadOnly);
        }
    }
}

void PageHolds::IndexWritten(u32 page, u32 reached) {
    index_stamp[page]++;
    index_write_frame[page] = frame;
    index_watch[page] = 0;
    const Access wanted = Wanted(page, reached);
    if (wanted < applied[page]) {
        Change(page, wanted);
    }
    Flush();
}

void PageHolds::Unwatch(u32 page, u32 reached) {
    UnwatchUnsent(page, reached);
    Flush();
}

void PageHolds::UnwatchUnsent(u32 page, u32 reached) {
    unwatches++;
    watched[page] = 0;
    watch_lo[page] = {};
    watch_hi[page] = {};
    const Access wanted = Wanted(page, reached);
    if (wanted < applied[page]) {
        Change(page, wanted);
    }
}

PageHolds::Access PageHolds::HoldsWant(u32 page, u32 reached) const {
    if (Live(write_ticket[page], reached)) {
        return NoAccess;
    }
    if (page >= fcram_pages) {
        return vram_held ? ReadOnly : Unheld;
    }
    if (versions[page].on) {
        // Protected by the frame: closed to writes while the frame's arming lasts.
        return versions[page].armed ? ReadOnly : Unheld;
    }
    return Live(read_ticket[page], reached) ? ReadOnly : Unheld;
}

u32 PageHolds::WaitFor(u32 page, bool write, u32 reached) const {
    // A versioned page: writes go on without a wait (a GPU write queued aside).
    if (page < fcram_pages && versions[page].on) {
        return Live(write_ticket[page], reached) ? write_ticket[page] : 0;
    }
    u32 ticket = Live(write_ticket[page], reached) ? write_ticket[page] : 0;
    if (write) {
        const u32 read = page >= fcram_pages ? vram_read : read_ticket[page];
        if (Live(read, reached) && (ticket == 0 || static_cast<s32>(read - ticket) > 0)) {
            ticket = read;
        }
    }
    return ticket;
}

void PageHolds::Release(u32 reached) {
    while (!groups.empty() && !Live(groups.front().ticket, reached)) {
        for (const Run& run : groups.front().runs) {
            for (u32 p = run.first; p < run.first + run.count; p++) {
                const Access wanted = Wanted(p, reached);
                if (wanted < applied[p]) {
                    Change(p, wanted);
                }
            }
        }
        groups.pop_front();
    }
    Flush();
}

void PageHolds::Reopen(u32 page, u32 reached) {
    Release(reached);
    Access wanted = Wanted(page, reached);
    // A VRAM page the guest writes opens until the next GPU read of VRAM holds it again.
    if (page >= fcram_pages && wanted == ReadOnly && !Live(vram_read, reached)) {
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

u16 PageHolds::AllocSlot() {
    ASSERT_MSG(!free_slots.empty(), "page holds: the {} KiB versions' pool is full",
               VersionPoolSize >> 10);
    const u16 s = free_slots.back();
    free_slots.pop_back();
    return s;
}

u8* PageHolds::SlotHost(u16 slot) const {
    return version_pool->Data() + (static_cast<u32>(slot) << PageBits);
}

void PageHolds::Flip(u32 page, u16 slot) {
    const u32 va = static_cast<u32>(reinterpret_cast<uintptr_t>(fcram_host + (page << PageBits)));
    const u32 to =
        slot == NoSlot ? 0u : static_cast<u32>(reinterpret_cast<uintptr_t>(SlotHost(slot)));
    flips.push_back({va, to});
}

bool PageHolds::Version(u32 page, u32 reached) {
    DEBUG_ASSERT(page < fcram_pages);
    PageVersion& v = versions[page];
    // A GPU write queued: the guest waits for it, as the ordinary holds decide.
    if (Live(write_ticket[page], reached)) {
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
        v.pool[0] = AllocSlot();
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
                v.pool[s - 1] = AllocSlot();
                target = s;
            }
        }
    }
    if (target != Slots) {
        if (target == 0) {
            // Home: the app's view goes there first, the home's only CPU mapping.
            Flip(page, NoSlot);
            Flush();
            std::memcpy(host, SlotHost(v.pool[v.guest - 1]), PageSize);
        } else {
            std::memcpy(SlotHost(v.pool[target - 1]), host, PageSize);
            Flip(page, v.pool[target - 1]);
        }
        v.guest = target;
        // The ranges held for the open ticket are read again: a later read remaps.
        seen = {};
#ifdef VITA_DIAGNOSTICS
        stats.swaps++;
    } else if (read) {
        stats.in_place_read++;
    } else {
        stats.in_place++;
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

void PageHolds::RemapToGuest(u32 page, u32 ticket) {
    // The PTE word now, while the app's view shows the guest's slot of this ticket.
    u32 out[2];
    const int r = sgxdPte(AddressOf(page), fcram_host + (page << PageBits), out);
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
    std::size_t kept = 0;
    for (const u32 page : versioned) {
        PageVersion& v = versions[page];
        // The changed pages onto the GPU, in one batch at the frame's start.
        if (Changed(page)) {
            RemapToGuest(page, ticket);
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
                    free_slots.push_back(slot);
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
                Flip(page, NoSlot);
                Flush();
                std::memcpy(fcram_host + (page << PageBits), SlotHost(v.pool[v.guest - 1]),
                            PageSize);
                v.guest = 0;
            }
            v.armed = true;
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

PAddr PageHolds::AddressOf(u32 page) const {
    return page < fcram_pages ? Memory::FCRAM_PADDR + (page << PageBits)
                              : Memory::VRAM_PADDR + ((page - fcram_pages) << PageBits);
}

#ifdef VITA_DIAGNOSTICS
PageHolds::Stats PageHolds::TakeStats() {
    stats.slots_used = static_cast<u32>((VersionPoolSize >> PageBits) - free_slots.size());
    return std::exchange(stats, {});
}

std::string PageHolds::DisplayState(u32 page, u32 reached) const {
    if (page >= fcram_pages) {
        return "VRAM";
    }
    const PageVersion& v = versions[page];
    return fmt::format("display {} {:#x}-{:#x}, other {}, watched {}, version {}",
                       Live(display_ticket[page], reached) ? "live" : "done", display_lo[page],
                       display_hi[page], Live(other_ticket[page], reached) ? "live" : "done",
                       watched[page],
                       !v.on              ? "none"
                       : v.gpu != v.guest ? "changed"
                                          : "on",
                       Live(read_ticket[page], reached) ? "live" : "done", read_lo[page][0],
                       read_hi[page][0], read_lo[page][1], read_hi[page][1],
                       Live(write_ticket[page], reached) ? "live" : "done", write_lo[page][0],
                       write_hi[page][0], write_lo[page][1], write_hi[page][1], parse_held[page],
                       index_watch[page]);
}

PageHolds::Kind PageHolds::WaitKind(u32 page, bool write, u32 reached) const {
    if (page >= fcram_pages && write && !Live(write_ticket[page], reached)) {
        return static_cast<Kind>(vram_read_kind);
    }
    return static_cast<Kind>(kinds[page]);
}
#endif

} // namespace SgxdRenderer
