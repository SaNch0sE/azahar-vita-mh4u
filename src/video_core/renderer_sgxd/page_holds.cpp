// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <utility>
#include "azahar_native.h"
#include "common/assert.h"
#include "core/memory.h"
#include "video_core/renderer_gxm/page_holds.h"

namespace SgxdRenderer {

namespace {

constexpr u32 PageBits = 12;
constexpr u32 PageSize = 1u << PageBits;

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
#endif
    ASSERT_MSG(fcram_host != nullptr && vram_host != nullptr,
               "page holds: guest FCRAM or VRAM has no host address");
}

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

void PageHolds::Read(PAddr addr, u32 bytes, u32 ticket, [[maybe_unused]] Kind kind) {
    u32 first, count;
    if (!PagesOf(addr, bytes, first, count)) {
        return;
    }
    DEBUG_ASSERT_MSG(open_ticket == 0 || open_ticket == ticket,
                     "page holds: ticket {} while {}'s are open", ticket, open_ticket);
    open_ticket = ticket;
    if (Seen(addr, bytes, 0)) {
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
#endif
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
        write_ticket[p] = ticket;
        if (p < fcram_pages) {
            index_stamp[p]++;
            index_watch[p] = 0;
        }
#ifdef VITA_DIAGNOSTICS
        kinds[p] = kind;
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
    return Live(read_ticket[page], reached) ? ReadOnly : Unheld;
}

u32 PageHolds::WaitFor(u32 page, bool write, u32 reached) const {
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

void PageHolds::Flush() {
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

#ifdef VITA_DIAGNOSTICS
PageHolds::Stats PageHolds::TakeStats() {
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

PAddr PageHolds::AddressOf(u32 page) const {
    return page < fcram_pages ? Memory::FCRAM_PADDR + (page << PageBits)
                              : Memory::VRAM_PADDR + ((page - fcram_pages) << PageBits);
}
#endif

} // namespace SgxdRenderer
