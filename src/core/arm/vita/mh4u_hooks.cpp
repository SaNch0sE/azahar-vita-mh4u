// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <fmt/format.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>

#include "common/mh4u_cover.h"
#include "common/pipeline_stats.h"
#include "common/settings.h"
#include "common/vita_data_dir.h"
#include "core/arm/vita/mh4u_hooks.h"
#include "core/core.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"
#include "core/memory.h"
#include "video_core/renderer_gxm/gxm_flags.h"
#include "video_core/renderer_gxm/gxm_video_options.h"

namespace Mh4uHooks {

namespace {

struct Filter {
    u32 vtable;
    const char* name;
};

/// Per executable: where the singletons live and the screen-filter vtables. Offsets inside the
/// objects are the same in all of them.
struct Version {
    const char* name;
    u32 lt_render; ///< sLtRender* (render words, present flags)
    u32 scene;     ///< sScene* (filter array)
    u32 camera;    ///< sMhCamera*
    std::array<Filter, 11> filters;
    u32 unit = 0;   ///< sUnit* (the unit lines)
    u32 layout = 0; ///< sMhLayout*
    // Cover trap (docs/plans/vita-mh4u-menu-world-skip.md): code addresses differ per version.
    u32 trap = 0;        ///< `ldr r1,[r0]` before the call to sLtRender::move in sMhMain::move
    u32 layout_draw = 0; ///< uLayout::draw, vtable slot +0x30 of the eight layout classes
    u32 monnyan = 0;     ///< uLayoutMonnyan's vtable (its draw is its own)
};

// USA v1.1 (the update; MH4UltimateDC's "USA v1.3.0", title version 1072).
constexpr Version UsaV11{"USA v1.1", 0x010572CC, 0x010572F0, 0x0105729C,
                         {{{0x00E5E7F4, "bloom"},
                           {0x00E5BAFC, "dof"},
                           {0x00E5E844, "bokeh"},
                           {0x00E62520, "radial"},
                           {0x00E6313C, "mhradial"},
                           {0x00E61E08, "multiblur"},
                           {0x00E5D56C, "haze"},
                           {0x00E5FF74, "mhhaze"},
                           {0x00E62F28, "colcorr"},
                           {0x00E5E970, "color"},
                           {0x00E5D474, "fade"}}},
                         0x010572F8, 0x01057534, 0x00AF5510, 0x00B04774, 0x00E60E14};
// USA v1.0 (the base game's program, as the web installer's mod brings): singletons 8 lower,
// filter vtables 0x30 lower (bloom, dof, bokeh, radial, multi-blur and haze checked in our dump;
// the last three by the same rule).
constexpr Version UsaV10{"USA v1.0", 0x010572C4, 0x010572E8, 0x01057294,
                         {{{0x00E5E7C4, "bloom"},
                           {0x00E5BACC, "dof"},
                           {0x00E5E814, "bokeh"},
                           {0x00E624F0, "radial"},
                           {0x00E6310C, "mhradial"},
                           {0x00E61DD8, "multiblur"},
                           {0x00E5D53C, "haze"},
                           {0x00E5FF44, "mhhaze"},
                           {0x00E62EF8, "colcorr"},
                           {0x00E5E940, "color"},
                           {0x00E5D444, "fade"}}},
                         0x010572F0, 0x0105752C, 0x00AF4E98, 0x00B040FC, 0x00E60DE4};
// EUR v1.1: data (singletons, vtables) as USA v1.1; only code moved. Untested.
constexpr Version EurV11{"EUR v1.1",     UsaV11.lt_render, UsaV11.scene,  UsaV11.camera,
                         UsaV11.filters, UsaV11.unit,      UsaV11.layout, 0x00AF5560,
                         0x00B047C4,     UsaV11.monnyan};

constexpr u64 TitleUsa = 0x0004000000126300;
constexpr u64 TitleEur = 0x0004000000126100;

// sLtRender offsets.
constexpr u32 RenderWordLive = 0x6BA4;
constexpr u32 RenderWordEdit = 0x6BA8;
constexpr u32 DisplaySkipBottom = 0x8AAC; ///< mDisplaySkip[1]: while 0, +0x8AB2 stays as written
constexpr u32 PresentBottom = 0x8AB2;     ///< the game builds the bottom pass while non-zero
// sScene offsets.
constexpr u32 FilterArray = 0x20;
constexpr u32 FilterCount = 0x24;
// sMhCamera offset of the byte buildFrame tests with the second-eye bit.
constexpr u32 CameraEyeByte = 0xE08;

struct State {
    const Kernel::Process* process = nullptr;
    const Version* version = nullptr;
    bool unknown_logged = false;
    u32 frames_seen = 0;      ///< guest frames since the process (or state) changed
    u32 last_frame_total = 0; ///< PipelineStats::guest_frames_total at the last tick
    u32 slices_idle = 0;
    // Logs, printed on change.
    u32 live_word = 0, edit_word = 0, eye_byte = 0xFFFFFFFF;
    u32 filter_hash = 0;
    bool scene_logged = false;
    std::array<u32, 8> unknown_vtables{};
    // P1: the bottom present flag this module wrote (restored before a save).
    bool bottom_written = false;
    // G2: dither bits this module cleared in the edit word (put back before a save), and how
    // often it had to clear them in the last second (another writer: give up).
    u32 dither_cleared = 0;
    u32 dither_clears = 0, dither_window = 0;
    bool dither_gave_up = false;
};
State g;

// Cover counters, taken by the once-a-second line on the frontend's thread.
std::atomic<u32> cov_traps{0};
std::atomic<u32> cov_key_changes{0};
std::atomic<u32> cov_walks{0};
std::atomic<u32> cov_walk_us{0};
std::atomic<u32> cov_walk_max_us{0};

Memory::MemorySystem* g_memory = nullptr;

bool Valid(u32 addr) {
    return g.process != nullptr && g_memory->IsValidVirtualAddress(*g.process, addr);
}
u32 Read32(u32 addr) {
    return Valid(addr) && Valid(addr + 3) ? g_memory->Read32(addr) : 0;
}
u8 Read8(u32 addr) {
    return Valid(addr) ? g_memory->Read8(addr) : 0;
}

/// The version by title id and the sLtRender vtable's move slot (it points into code, which
/// moved between versions). Works for any process, before Tick has adopted it.
const Version* DetectIn(const Kernel::Process& process, Memory::MemorySystem& memory) {
    if (process.codeset == nullptr) {
        return nullptr;
    }
    const auto read = [&](u32 addr) -> u32 {
        if (!memory.IsValidVirtualAddress(process, addr) ||
            !memory.IsValidVirtualAddress(process, addr + 3)) {
            return 0;
        }
        u32 value = 0;
        memory.ReadBlock(process, addr, &value, sizeof(value));
        return value;
    };
    const u64 program_id = process.codeset->program_id;
    if (program_id == TitleUsa) {
        if (read(0x00E6CAD8) == 0x00BFA670) {
            return &UsaV11;
        }
        if (read(0x00E6CAA8) == 0x00BF9FF8) {
            return &UsaV10;
        }
    } else if (program_id == TitleEur && read(0x00E6CAD8) == 0x00BFA6C0) {
        return &EurV11;
    }
    return nullptr;
}

/// The class name of an unknown filter: vtable slot 5 is `ldr r0,[pc]; bx lr; .word DTI`, and
/// the DTI object is expected to hold the name pointer at +4.
std::string DtiName(u32 vtable) {
    const u32 get_dti = Read32(vtable + 5 * 4) & ~1u;
    const u32 dti = Read32(get_dti + 8);
    const u32 name = Read32(dti + 4);
    std::string out;
    for (u32 i = 0; i < 40 && Valid(name + i); i++) {
        const char c = static_cast<char>(Read8(name + i));
        if (c == 0) {
            break;
        }
        out += (c >= 32 && c < 127) ? c : '?';
    }
    return out.empty() ? fmt::format("dti {:08X}", dti) : out;
}

void LogRenderWords(u32 lt_render) {
    const u32 live = Read32(lt_render + RenderWordLive);
    const u32 edit = Read32(lt_render + RenderWordEdit);
    const u32 camera = Read32(g.version->camera);
    const u32 eye = camera != 0 ? Read8(camera + CameraEyeByte) : 0xFF;
    // Bit 24 of the edit word flaps: the cheat / mod sets it, the game rewrites it from a query
    // every frame. Not a change worth a line.
    constexpr u32 Flapping = 0x01000000;
    if (live != g.live_word || ((edit ^ g.edit_word) & ~Flapping) != 0 || eye != g.eye_byte) {
        g.live_word = live;
        g.edit_word = edit;
        g.eye_byte = eye;
        sceClibPrintf("[mh4u] render word live %08X edit %08X eye2 %u (bit 0x40 %s)\n", live,
                      edit, eye, (live & 0x40) != 0 ? "set" : "clear");
    }
}

void LogFilters() {
    const u32 scene = Read32(g.version->scene);
    if (scene == 0) {
        return;
    }
    const u32 array = Read32(scene + FilterArray);
    const u32 raw_count = Read32(scene + FilterCount);
    const u32 count = std::min<u32>(raw_count, 64);
    if (!g.scene_logged) {
        g.scene_logged = true;
        sceClibPrintf("[mh4u] filters: scene %08X array %08X count %u (+0x18..+0x30: %08X %08X "
                      "%08X %08X %08X %08X %08X)\n",
                      scene, array, raw_count, Read32(scene + 0x18), Read32(scene + 0x1C),
                      Read32(scene + 0x20), Read32(scene + 0x24), Read32(scene + 0x28),
                      Read32(scene + 0x2C), Read32(scene + 0x30));
    }
    u32 hash = count * 0x9E3779B1u + 1;
    std::array<u32, 64> vtables{};
    for (u32 i = 0; i < count; i++) {
        const u32 object = Read32(array + 4 * i);
        vtables[i] = object != 0 ? Read32(object) : 0;
        hash = (hash ^ vtables[i]) * 0x01000193u;
    }
    if (hash == g.filter_hash) {
        return;
    }
    g.filter_hash = hash;
    std::string line;
    for (u32 i = 0; i < count; i++) {
        const char* name = nullptr;
        for (const Filter& f : g.version->filters) {
            if (f.vtable == vtables[i]) {
                name = f.name;
            }
        }
        if (name != nullptr) {
            line += fmt::format(" {}", name);
            continue;
        }
        line += fmt::format(" ?{:08X}", vtables[i]);
        // Each unknown class once: its name from the type info.
        for (u32& v : g.unknown_vtables) {
            if (v == vtables[i]) {
                break;
            }
            if (v == 0) {
                v = vtables[i];
                sceClibPrintf("[mh4u] filters: vtable %08X is %s\n", vtables[i],
                              DtiName(vtables[i]).c_str());
                break;
            }
        }
    }
    sceClibPrintf("[mh4u] filters (%u):%s\n", count, line.c_str());
}

/// P1: the game stops building its bottom-screen pass while that screen is not shown (Single
/// screen layout): ~7,000-9,000 draws a second it built for us to drop, -11..15% emulation
/// thread CPU (2026-11-19). Re-asserted each frame (a byte compare); only while mDisplaySkip[1]
/// is 0, when the game leaves the flag alone. On by default; kill switch `nosrcbottom` (and
/// `nobottomskip`, which keeps the whole bottom screen).
void BottomPass(u32 lt_render) {
    static const bool enabled = !Switch("nosrcbottom");
    if (!enabled && !g.bottom_written) {
        return;
    }
    if (Read32(lt_render + DisplaySkipBottom) != 0) {
        return;
    }
    const u8 want = enabled && BottomHidden() ? 0 : 1;
    if (Read8(lt_render + PresentBottom) != want) {
        g_memory->Write8(lt_render + PresentBottom, want);
    }
    g.bottom_written = want == 0;
}

/// G2 (Smooth gradients): the game's dither pattern off - render word bits 0x100000 (top) and
/// 0x200000 (bottom) cleared in the edit word, which the game applies itself each frame. The
/// 8-bit display copies (RasterizerGxm::SmoothDisplayTransfer) keep the picture from banding.
/// If something else (a cheat, the mod) keeps setting the bits, stop after a second of it.
void Dither(u32 lt_render) {
    constexpr u32 DitherBits = 0x00100000 | 0x00200000;
    const bool want_off = GxmRenderer::Video().smooth && !g.dither_gave_up;
    const u32 edit = Read32(lt_render + RenderWordEdit);
    if (want_off && (edit & DitherBits) != 0) {
        g.dither_cleared |= edit & DitherBits;
        g_memory->Write32(lt_render + RenderWordEdit, edit & ~DitherBits);
        if (++g.dither_clears > 6) {
            g.dither_gave_up = true;
            sceClibPrintf("[mh4u] smooth: another writer keeps the dither on (mod or cheat); "
                          "choose No Dithering in the mod's settings\n");
        }
    } else if (!want_off && g.dither_cleared != 0) {
        g_memory->Write32(lt_render + RenderWordEdit, edit | g.dither_cleared);
        g.dither_cleared = 0;
    }
    if (++g.dither_window >= 60) {
        g.dither_window = 0;
        g.dither_clears = 0;
    }
}

// ---------------------------------------------------------------------------------------------
// Cover key (docs/plans/vita-mh4u-menu-world-skip.md, step 2). At the cover trap - every update
// of the frame done, its render not built yet - walk the game's layouts and fold the large
// opaque 2D panes into one number. The skip is armed only while this key equals the key of a
// frame the render thread proved covered; any change to a cover pane changes the key before
// that frame is built. The walk itself writes nothing.
//
// Objects (docs/plans/vita-mh4u-2d-layer.md, checked against the v1.1 disassembly):
// - sUnit: [sUnit+0x614] lines (sUnit::draw's count), line i at sUnit+0x14+i*0x18, drawn only
//   while bit 2 of line+0x08 is set; first unit at line+0x0C, next at unit+0x0C. A unit draws
//   only in state 2 (unit+0x04 & 7) with bits 11 and 14 of unit+0x04 set, and in view 0 only
//   with bit 16 of unit+0x04 set.
// - Layout unit: draw list at +0x120, groups linked through group+0x48.
// - Group (0x50): +0x04 name, +0x08 first pane, +0x0C redraw queue, +0x18 order, +0x24 texture
//   array, +0x28 x/y/z, +0x3C RGBA, +0x40 revision (low 16 bits sprites at the last build,
//   byte +0x42 buffers to rebuild, bits 24-30 a change counter), +0x44 flags (0x100 cached,
//   0x200 re-recorded this frame, 0x400 shown).
// - Pane (0x24): +0x08 data, +0x10 kind (0 sprite, 1 null, 2 text, 3 boundary, 4 window),
//   +0x14 next, +0x20 first child (nulls).
// - Sprite (0x68): +0x00..+0x0B texture words, +0x10 x/y/z, +0x20 size, +0x28 scale, +0x30 uv,
//   +0x40 four RGBA corners, +0x50 rotation, +0x58 bits 0-3 origin / bit 7 visible / bits 8+
//   vertex slot (volatile). Windows are read with the same layout (medium confidence).
// - Null (0x30): +0x00 x/y/z, +0x10 scale, +0x18 RGBA, +0x1C rotation, +0x24 bit 0 visible /
//   bits 1+ matrix slot (volatile).
//
// Speed (the first walk cost 1.5 ms a frame at the quest counter, ~1.4 us a pane: the cost is
// guest memory the walk touches, not the reads' bookkeeping). Objects are read through the page
// table's host pointers, a sprite's flag and corner words first, the rest only for a candidate.
// And most groups are not walked at all: a group whose header (the words the key uses, its
// list and links) is unchanged re-reads only the candidates its last full walk found, with
// the nulls above them - the same hash the full walk gives, for a tenth of the reads. That can
// miss a pane that turned into a candidate since (a hidden panel shown inside an unchanged
// group): more cover, never less, so it can only delay an arm. Every frame one group (in
// turn) is walked in full anyway, and a group whose header changed always is.
// In the field (no candidate of a quarter screen anywhere) a group with an unchanged header is
// not read at all: its key is not plausible anyway, so a stale group can only delay an arm - a
// group that turns big makes that walk start over without reuse.

constexpr u32 MaxUnitLines = 64;    // sUnit's line array ends at +0x614 = 0x14 + 64 * 0x18
constexpr u32 MaxUnitsPerLine = 1024;
constexpr u32 MaxGroupsPerUnit = 512;
constexpr u32 MaxPanes = 16384;     // per walk: a runaway list cannot stall the frame
constexpr u32 MaxPanesPerList = 2048;
constexpr u32 MaxNullDepth = 12;
constexpr float MinCandidateSide = 24.0f;
constexpr u32 ScreenW = 400, ScreenH = 240;
constexpr u64 ScreenArea = u64{ScreenW} * ScreenH;
constexpr u32 ViewZeroBit = 1u << 16; // unit+0x04: drawn in view 0 (the top screen's world)

float AsFloat(u32 word) {
    float f;
    std::memcpy(&f, &word, sizeof(f));
    return f;
}

u64 Mix(u64 h, u32 v) {
    h = (h ^ v) * 0x9E3779B97F4A7C15ull;
    return h ^ (h >> 29);
}

/// The guest's page table, host pointer per 4 KiB page (null for pages that are not plain
/// memory), taken at each trap.
u8** g_pages = nullptr;
u32 g_reads_direct = 0, g_reads_checked = 0; ///< coverlog: which way the walk read

/// Copies `size` (< one page) bytes of guest memory: straight through the page's host pointer
/// when the object lies in one plain page, else the checked read. False, nothing read, when
/// the address is null, misaligned or unmapped.
bool ReadGuest(u32 addr, void* out, u32 size) {
    if (addr == 0 || (addr & 3) != 0 || addr + size < addr) {
        return false;
    }
    const u32 page = addr >> Memory::CITRA_PAGE_BITS;
    if (g_pages != nullptr && ((addr + size - 1) >> Memory::CITRA_PAGE_BITS) == page) {
        if (const u8* host = g_pages[page]) {
            std::memcpy(out, host + (addr & Memory::CITRA_PAGE_MASK), size);
            g_reads_direct++;
            return true;
        }
    }
    if (!Valid(addr) || !Valid(addr + size - 1)) {
        return false;
    }
    g_reads_checked++;
    g_memory->ReadBlock(*g.process, addr, out, size);
    return true;
}

u32 Fetch32(u32 addr) {
    u32 v = 0;
    ReadGuest(addr, &v, sizeof(v));
    return v;
}

std::string GuestString(u32 addr, u32 max) {
    std::string out;
    for (u32 i = 0; i < max && Valid(addr + i); i++) {
        const char c = static_cast<char>(g_memory->Read8(addr + i));
        if (c == 0) {
            break;
        }
        out += (c >= 32 && c < 127) ? c : '?';
    }
    return out;
}

/// The position and scale a pane's own values are relative to (group, then nulls). Rotation is
/// never carried: rotated nulls and sprites are not candidates.
struct Xform {
    float x, y, sx, sy;
};

/// One candidate group for the coverlog line, gathered during the walk, printed after it.
struct GroupLog {
    u32 group, name, order, flags, alpha;
    u32 candidates, windows;
    u32 area;
    float big_w, big_h;
    u32 big_area, big_tex0, big_tex1;
    u32 revision, queue; ///< +0x40, +0x0C as read now
    u32 header_diff;     ///< header words (bit = word index) that changed since the last log
    u32 nodes;           ///< tracked candidates and nulls
};

/// A tracked object of a group: a candidate (sprite or window) its last full walk found, or a
/// null above one. Parents come before children, in the full walk's order.
struct Node {
    u32 data;
    u8 kind;
    s16 parent; ///< index of the null above it, -1 for the group
};

/// What one group gave the key last time, and how to give it again.
struct GroupEntry {
    u32 group = 0;
    u64 sig = 0;
    std::array<u32, 20> header{}; ///< last seen (coverlog's header diff)
    u32 header_diff = 0;
    u64 hash = 0;
    u64 area = 0;
    u32 candidates = 0;
    u32 big = 0; ///< its largest candidate's clipped area
    std::vector<Node> nodes;
};

struct CoverWalk {
    bool logging = false;
    bool full = false;   ///< walk every group in full (nocovercache, coverlog)
    bool field = false;  ///< leave groups with an unchanged header as they were (see above)
    u32 panes = 0;
    u64 key = 0xCBF29CE484222325ull;
    u32 candidates = 0;
    u64 area = 0; ///< clipped screen pixels of all candidates, overlaps counted each time
    u32 big = 0;  ///< the largest candidate's clipped area
    u32 units = 0, groups = 0;
    bool turns = false; ///< this walk reads one group in full, in turn
    u32 tracked = 0, loose = 0, full_changed = 0, full_turn = 0, verify_diffs = 0;
    u32 rev_skipped = 0;
    // The group being walked.
    u64 group_hash = 0;
    u64 group_area = 0;
    u32 group_candidates = 0;
    u32 group_big = 0;
    std::vector<Node>* record = nullptr; ///< where a full walk notes what it found
    std::array<GroupLog, 24> logs{};
    u32 log_count = 0;
    GroupLog* current = nullptr; ///< the group being walked, when logging
};

/// Cover state of the current process (reset with it, dropping the hidden list unwritten).
struct Cover {
    u64 key = 0;          ///< the gated key of the last walk
    u32 key_start = 0;    ///< the trap at which `key` took its value
    u64 log_us = 0;       ///< when the last coverlog line was printed
    u32 walks = 0;
    bool field = false;   ///< the last full walk found no candidate of a quarter screen
    u32 verify_pick = 0;  ///< the group (by walk order) walked in full this time
    bool rev_off = false; ///< the revision rule was caught out: off for the session
    u32 last_rev_skipped = 0;
    /// The units the last walk saw (HideWorld at the same trap uses them, not a second walk).
    struct UnitSeen {
        u32 unit, vtable, flags;
        bool layout, drawn;
    };
    std::vector<UnitSeen> units;
    u32 units_seq = 0;
    std::array<std::pair<u32, bool>, 32> layout_vtables{}; ///< vtable -> layout family
    u32 last_groups = 0;  ///< shown groups on the last walk
    /// The last walk without logging (coverlog): groups re-read by their tracked candidates,
    /// left as they were (field), walked in full (header changed / in turn), verify diffs.
    u32 last_tracked = 0, last_loose = 0, last_full_changed = 0, last_full_turn = 0;
    u32 verify_diffs = 0; ///< since the last coverlog line
    std::array<GroupEntry, 256> groups{};
    // Decision.
    u32 proof_read = 0;   ///< proof ring position read up to
    Common::Mh4uCover::Proof proof{}; ///< the newest proof (by tag)
    bool have_proof = false;
    u32 covered_run = 0;  ///< covered proofs in a row, newest last
    bool armed = false;
    u64 armed_key = 0;
    const char* why = "start"; ///< why not armed (the last refusal)
    std::array<u32, 64> armed_at{};  ///< armed_at[s % 64] == s: trap s was armed
    std::array<u64, 64> key_at{};    ///< the key armed at that trap
    std::array<u64, 16> blacklist{};
    u32 blacklist_next = 0;
    u32 last_trap_frames = 0; ///< PipelineStats::guest_frames_total at the last trap
    u32 lines_logged = 0;
    struct Hidden {
        u32 unit, vtable;
        bool operator<(const Hidden& o) const {
            return unit < o.unit;
        }
    };
    std::vector<Hidden> hidden; ///< units whose view-0 bit this module cleared, by address
};
Cover cv;

/// Traps taken, all processes: the frame tag (Common::Mh4uCover::trap_seq). Never reset, so a
/// proof from before a state load can never pass for a newer frame's.
u32 g_seq = 0;

// Counters for the once-a-second line (frontend thread takes them).
std::atomic<u32> cov_armed{0};
std::atomic<u32> cov_mispredicts{0};
std::atomic<u32> cov_hidden{0};
std::atomic<u32> cov_proofs{0};

/// A visible sprite (or window) with four opaque corners, no rotation and a scaled size of at
/// least 24x24: hash it and add its clipped area. The area model is simple - positions add,
/// scales multiply, origin 0-8 as a 3x3 grid (row 0 top), screen point (200 - x, 120 - y) - and
/// serves only as a plausibility gate. True when it is one.
bool Candidate(u32 data, u32 kind, const Xform& xf, u64 chain, CoverWalk& w) {
    std::array<u32, 0x68 / 4> s;
    // The flags first (a hidden sprite stops here), then corners and rotation.
    if (!ReadGuest(data + 0x58, &s[0x58 / 4], 4)) {
        return false;
    }
    const u32 flags = s[0x58 / 4];
    if ((flags & 0x80) == 0) {
        return false;
    }
    if (!ReadGuest(data + 0x40, &s[0x40 / 4], 0x18)) {
        return false;
    }
    for (u32 c = 0x40; c < 0x50; c += 4) {
        if ((s[c / 4] >> 24) != 0xFF) {
            return false;
        }
    }
    if (AsFloat(s[0x50 / 4]) != 0.0f) {
        return false; // rotated (or NaN)
    }
    if (!ReadGuest(data, s.data(), 0x40)) {
        return false;
    }
    const float width = std::fabs(AsFloat(s[0x20 / 4]) * AsFloat(s[0x28 / 4]) * xf.sx);
    const float height = std::fabs(AsFloat(s[0x24 / 4]) * AsFloat(s[0x2C / 4]) * xf.sy);
    if (!(width >= MinCandidateSide && height >= MinCandidateSide && width < 1e5f &&
          height < 1e5f)) {
        return false; // too small, or not a number
    }
    const float px = xf.x + xf.sx * AsFloat(s[0x10 / 4]);
    const float py = xf.y + xf.sy * AsFloat(s[0x14 / 4]);
    if (!(std::fabs(px) < 1e5f && std::fabs(py) < 1e5f)) {
        return false;
    }

    // Key: the sprite's words up to its flags, the flags without the vertex slot, its kind and
    // the words of the group and nulls above it.
    u64 h = w.group_hash;
    for (u32 i = 0; i < 0x58 / 4; i++) {
        h = Mix(h, s[i]);
    }
    h = Mix(h, flags & 0xFF);
    h = Mix(h, kind);
    w.group_hash = Mix(h, static_cast<u32>(chain) ^ static_cast<u32>(chain >> 32));
    w.group_candidates++;

    const u32 origin = flags & 0xF;
    const u32 col = origin < 9 ? origin % 3 : 1;
    const u32 row = origin < 9 ? origin / 3 : 1;
    const float x0 = px - width * 0.5f * static_cast<float>(col);
    const float y0 = py - height * 0.5f * static_cast<float>(2 - row);
    // Game (x0..x0+w, y0..y0+h) -> screen (200 - x, 120 - y): both axes flip, extents keep.
    const float l = std::max(0.0f, 200.0f - (x0 + width));
    const float r = std::min(static_cast<float>(ScreenW), 200.0f - x0);
    const float t = std::max(0.0f, 120.0f - (y0 + height));
    const float b = std::min(static_cast<float>(ScreenH), 120.0f - y0);
    const u32 area = (r > l && b > t) ? static_cast<u32>((r - l) * (b - t)) : 0;
    w.group_area += area;
    w.group_big = std::max(w.group_big, area);

    if (GroupLog* gl = w.current) {
        gl->candidates++;
        gl->windows += kind == 4 ? 1 : 0;
        gl->area += area;
        if (gl->candidates == 1 || area > gl->big_area) {
            gl->big_area = area;
            gl->big_w = width;
            gl->big_h = height;
            gl->big_tex0 = s[0];
            gl->big_tex1 = s[1];
        }
    }
    return true;
}

/// A null's subtree counts only while the null is visible, opaque and unrotated: its frame
/// and chain for the panes below, or false.
bool EnterNull(u32 data, const Xform& xf, u64 chain, Xform& inner, u64& inner_chain) {
    std::array<u32, 0x28 / 4> nl;
    if (!ReadGuest(data, nl.data(), 0x28) || (nl[0x24 / 4] & 1) == 0 ||
        (nl[0x18 / 4] >> 24) != 0xFF || AsFloat(nl[0x1C / 4]) != 0.0f) {
        return false;
    }
    inner = Xform{xf.x + xf.sx * AsFloat(nl[0]), xf.y + xf.sy * AsFloat(nl[1]),
                  xf.sx * AsFloat(nl[0x10 / 4]), xf.sy * AsFloat(nl[0x14 / 4])};
    u64 c = chain;
    for (u32 i = 0; i < 0x24 / 4; i++) {
        c = Mix(c, nl[i]);
    }
    inner_chain = Mix(c, nl[0x24 / 4] & 1); // the matrix slot above bit 0 is volatile
    return true;
}

void WalkPanes(u32 pane, const Xform& xf, u64 chain, u32 depth, s16 parent, CoverWalk& w) {
    for (u32 n = 0; pane != 0 && n < MaxPanesPerList; n++) {
        if (++w.panes > MaxPanes) {
            return;
        }
        std::array<u32, 0x24 / 4> p;
        // +0x08..+0x23 (data, kind, next, child): the first two words are not used.
        if (!ReadGuest(pane + 0x08, &p[0x08 / 4], 0x1C)) {
            return;
        }
        const u32 kind = p[0x10 / 4] & 0xFF;
        const u32 data = p[0x08 / 4];
        const u32 child = p[0x20 / 4];
        if (kind == 0 || kind == 4) {
            if (Candidate(data, kind, xf, chain, w) && w.record != nullptr) {
                w.record->push_back(Node{data, static_cast<u8>(kind), parent});
            }
        } else if (kind == 1 && child != 0 && depth < MaxNullDepth) {
            Xform inner;
            u64 inner_chain;
            if (EnterNull(data, xf, chain, inner, inner_chain)) {
                // Noted before its subtree; dropped again if nothing below it was a candidate.
                const std::size_t at = w.record != nullptr ? w.record->size() : 0;
                if (w.record != nullptr && at < 0x7FFF) {
                    w.record->push_back(Node{data, 1, parent});
                }
                WalkPanes(child, inner, inner_chain, depth + 1, static_cast<s16>(at), w);
                if (w.record != nullptr && w.record->size() == at + 1) {
                    w.record->pop_back();
                }
            }
        }
        // Text (2) and boundaries (3) are never candidates: text is many small glyphs and
        // changes for reasons that do not move a cover.
        pane = p[0x14 / 4];
    }
}

/// The group again from its tracked objects only: the full walk's hash for them, in its order.
void WalkTracked(const std::vector<Node>& nodes, const Xform& base, u64 base_chain,
                 CoverWalk& w) {
    // Frames of the tracked nulls (by node index); a null now hidden hides its subtree.
    static std::vector<Xform> frames;
    static std::vector<u64> chains;
    static std::vector<u8> open;
    frames.resize(nodes.size());
    chains.resize(nodes.size());
    open.assign(nodes.size(), 0);
    for (std::size_t i = 0; i < nodes.size(); i++) {
        const Node& node = nodes[i];
        const bool top = node.parent < 0;
        if (!top && (static_cast<std::size_t>(node.parent) >= i || !open[node.parent])) {
            continue;
        }
        const Xform xf = top ? base : frames[node.parent];
        const u64 chain = top ? base_chain : chains[node.parent];
        w.panes++;
        if (node.kind == 1) {
            open[i] = EnterNull(node.data, xf, chain, frames[i], chains[i]) ? 1 : 0;
        } else {
            Candidate(node.data, node.kind, xf, chain, w);
        }
    }
}

GroupEntry& GroupSlot(u32 group) {
    // Open addressing on the group's address; a full neighbourhood evicts its first slot.
    const u32 home = (group >> 4) * 0x9E3779B1u >> 24;
    for (u32 i = 0; i < 8; i++) {
        GroupEntry& e = cv.groups[(home + i) & 0xFF];
        if (e.group == group || e.group == 0) {
            return e;
        }
    }
    return cv.groups[home & 0xFF];
}

void WalkLayout(u32 unit, u32 unit_flags, CoverWalk& w) {
    u32 group = Fetch32(unit + 0x120);
    for (u32 n = 0; group != 0 && n < MaxGroupsPerUnit && w.panes <= MaxPanes; n++) {
        std::array<u32, 0x50 / 4> gw;
        if (!ReadGuest(group, gw.data(), 0x50)) {
            return;
        }
        const u32 flags = gw[0x44 / 4];
        const u32 next = gw[0x48 / 4];
        // Shown groups of any view (the mod picks the view at draw time), at full alpha.
        if ((flags & 0x400) == 0 || (gw[0x3C / 4] >> 24) != 0xFF) {
            group = next;
            continue;
        }
        const u32 ordinal = w.groups++;
        // The header words that matter: hash, name, first pane, order, texture array,
        // position, colour, flags and link. Not the redraw queue (+0x0C), the cached command
        // lists (+0x10..+0x20), the revision (+0x40) or flag 0x200: the game changes those
        // while it re-records an unchanged group.
        u64 sig = Mix(0x2545F491u, unit_flags);
        for (const u32 i : {0x00u, 0x04u, 0x08u, 0x18u, 0x24u, 0x28u, 0x2Cu, 0x30u, 0x34u, 0x38u,
                            0x3Cu, 0x48u}) {
            sig = Mix(sig, gw[i / 4]);
        }
        sig = Mix(sig, flags & ~0x200u);
        GroupEntry& entry = GroupSlot(group);
        const bool known = entry.group == group && entry.sig == sig;
        if (entry.group == group) {
            for (u32 i = 0; i < gw.size(); i++) {
                entry.header_diff |= gw[i] != entry.header[i] ? 1u << i : 0u;
            }
        }
        const auto add = [&](u64 hash, u64 area, u32 candidates, u32 big) {
            w.candidates += candidates;
            w.area += area;
            w.big = std::max(w.big, big);
            w.key = Mix(Mix(w.key, static_cast<u32>(hash)), static_cast<u32>(hash >> 32));
        };
        const Xform base{AsFloat(gw[0x28 / 4]), AsFloat(gw[0x2C / 4]), 1, 1};
        u64 chain = Mix(0x84222325u, unit_flags);
        chain = Mix(chain, gw[0x18 / 4]);  // order
        chain = Mix(chain, gw[0x24 / 4]);  // texture array
        chain = Mix(chain, gw[0x28 / 4]);  // x, y, z
        chain = Mix(chain, gw[0x2C / 4]);
        chain = Mix(chain, gw[0x30 / 4]);
        chain = Mix(chain, gw[0x3C / 4]);  // colour
        chain = Mix(chain, flags & ~0x200u); // 0x200: re-recorded this frame
        const auto begin_group = [&] {
            w.group_hash = Mix(0xCBF29CE4u, group);
            w.group_area = 0;
            w.group_candidates = 0;
            w.group_big = 0;
        };
        // In turn (every 8th walk, one group at a time) a group is read in full, whatever
        // it looks like: what catches a pane that turned into a candidate, and checks the
        // revision rule below.
        const bool turn = w.turns && ordinal == cv.verify_pick;
        // The game's own word that nothing changed: a cached group (0x100) not re-recorded
        // last frame (0x200), nothing to rebuild (+0x42), an empty redraw queue (+0x0C) and
        // the same revision as last time. A pane change needs a re-record or a queued redraw,
        // or the game would show stale pixels itself.
        const u32 revision = gw[0x40 / 4];
        const bool game_still = known && (flags & 0x100) != 0 && (flags & 0x200) == 0 &&
                                ((revision >> 16) & 0xFF) == 0 && gw[0x0C / 4] == 0 &&
                                revision == entry.header[0x40 / 4];
        static const bool no_rev = Switch("nocoverrev");
        if (game_still && !w.full && !turn && !no_rev && !cv.rev_off) {
            w.rev_skipped++;
            add(entry.hash, entry.area, entry.candidates, entry.big);
            entry.header = gw;
            group = next;
            continue;
        }
        if (known && w.field && !w.full) {
            w.loose++;
            add(entry.hash, entry.area, entry.candidates, entry.big);
            entry.header = gw;
            group = next;
            continue;
        }
        u64 tracked_hash = 0;
        if (known && !w.full) {
            begin_group();
            WalkTracked(entry.nodes, base, chain, w);
            tracked_hash = w.group_candidates != 0 ? w.group_hash : 0;
            if (turn && game_still && tracked_hash != entry.hash && !cv.rev_off) {
                // A tracked candidate changed while the game's words said nothing did.
                cv.rev_off = true;
                sceClibPrintf("[mh4u] cover: group %08X changed under an unchanged revision "
                              "(%08X): revision rule off for this session\n",
                              group, revision);
            }
            if (!turn) {
                w.tracked++;
                entry.hash = tracked_hash;
                entry.area = w.group_area;
                entry.candidates = w.group_candidates;
                entry.big = w.group_big;
                entry.header = gw;
                add(entry.hash, entry.area, entry.candidates, entry.big);
                group = next;
                continue;
            }
        }
        // In full: the header changed, it is this group's turn, or reuse is off.
        if (turn) {
            w.full_turn++;
        } else {
            w.full_changed++;
        }
        GroupLog* gl = nullptr;
        if (w.logging && w.log_count < w.logs.size()) {
            gl = &w.logs[w.log_count];
            *gl = GroupLog{group, gw[0x04 / 4], gw[0x18 / 4], flags, gw[0x3C / 4] >> 24};
            gl->revision = gw[0x40 / 4];
            gl->queue = gw[0x0C / 4];
            gl->header_diff = entry.group == group ? entry.header_diff : 0xFFFFF;
        }
        w.current = gl;
        begin_group();
        std::vector<Node> nodes;
        nodes.reserve(entry.group == group ? entry.nodes.size() : 16);
        w.record = &nodes;
        WalkPanes(gw[0x08 / 4], base, chain, 0, -1, w);
        w.record = nullptr;
        w.current = nullptr;
        if (gl != nullptr) {
            gl->nodes = static_cast<u32>(nodes.size());
            if (w.group_candidates != 0) {
                w.log_count++;
            }
        }
        // A group with no candidate adds nothing to the key, as before the reuse.
        const u64 hash = w.group_candidates != 0 ? w.group_hash : 0;
        if (known && !w.full && hash != tracked_hash) {
            w.verify_diffs++; // a pane turned into a candidate (or the structure moved)
        }
        const u32 diff = entry.group == group ? entry.header_diff : 0;
        entry = GroupEntry{group, sig, gw, diff, hash, w.group_area, w.group_candidates,
                           w.group_big, std::move(nodes)};
        add(hash, entry.area, entry.candidates, entry.big);
        group = next;
    }
}

/// Calls `fn(unit, vtable, flags, layout, drawn)` for every unit on sUnit's lines.
template <class Fn>
void ForEachUnit(Fn&& fn) {
    const u32 units = Fetch32(g.version->unit);
    const u32 lines = std::min<u32>(units != 0 ? Fetch32(units + 0x614) : 0, MaxUnitLines);
    for (u32 i = 0; i < lines; i++) {
        const u32 line = units + 0x14 + i * 0x18;
        u32 unit = Fetch32(line + 0x0C);
        const bool drawn = (Fetch32(line + 0x08) & 4) != 0;
        for (u32 n = 0; unit != 0 && n < MaxUnitsPerLine; n++) {
            u32 head[4];
            if (!ReadGuest(unit, head, sizeof(head))) {
                break;
            }
            const u32 vtable = head[0];
            // The class decides the family: looked up once per vtable, not once per unit.
            bool layout = false;
            if (vtable != 0) {
                auto& cache = cv.layout_vtables;
                const auto hit = std::find_if(cache.begin(), cache.end(),
                                              [&](const auto& e) { return e.first == vtable; });
                if (hit != cache.end()) {
                    layout = hit->second;
                } else {
                    layout = vtable == g.version->monnyan ||
                             Fetch32(vtable + 0x30) == g.version->layout_draw;
                    const auto free = std::find_if(cache.begin(), cache.end(),
                                                   [](const auto& e) { return e.first == 0; });
                    (free != cache.end() ? *free : cache[vtable % cache.size()]) = {vtable, layout};
                }
            }
            fn(unit, vtable, head[1], layout, drawn);
            unit = head[3]; // +0x0C: next on the line
        }
    }
}

void RunWalk(CoverWalk& w) {
    cv.units.clear();
    cv.units_seq = g_seq;
    ForEachUnit([&](u32 unit, u32 vtable, u32 flags, bool layout, bool drawn) {
        cv.units.push_back(Cover::UnitSeen{unit, vtable, flags, layout, drawn});
        if (drawn && layout && (flags & 7) == 2 && (flags & 0x4800) == 0x4800 &&
            w.panes <= MaxPanes) {
            w.units++;
            WalkLayout(unit, flags, w);
        }
    });
}

void CoverKey() {
    static const bool log_enabled = Switch("coverlog");
    static const bool no_cache = Switch("nocovercache");
    const SceUInt64 start = sceKernelGetProcessTimeWide();
    CoverWalk w;
    w.logging = log_enabled && start - cv.log_us >= 1'000'000;
    w.full = no_cache || w.logging;
    w.field = cv.field;
    // One group in full every 8th walk: a full read of the biggest group is most of a turn
    // walk's 0.8 ms, and every 2nd walk made it most of the mean.
    w.turns = cv.walks++ % 8 == 0;
    RunWalk(w);
    if (w.field && !w.full && w.big * 4 >= ScreenArea) {
        // Something turned big while groups were left as they were: read it all again.
        const bool logging = w.logging;
        w = CoverWalk{};
        w.logging = logging;
        RunWalk(w);
    }
    cv.field = w.big * 4 < ScreenArea;
    cv.last_groups = w.groups;
    if (w.turns) {
        cv.verify_pick = w.groups != 0 ? (cv.verify_pick + 1) % w.groups : 0;
    }
    cv.verify_diffs += w.verify_diffs;

    const u64 raw = w.candidates != 0 ? (w.key != 0 ? w.key : 1) : 0;
    // Plausibility: the candidates must at least add up to one screen.
    const bool plausible = w.area >= ScreenArea;
    const u64 key = plausible ? raw : 0;
    if (key != cv.key) {
        cv.key = key;
        cv.key_start = g_seq;
        cov_key_changes.fetch_add(1, std::memory_order_relaxed);
    }
    Common::Mh4uCover::key_present.store(key != 0, std::memory_order_relaxed);

    const u32 us = static_cast<u32>(sceKernelGetProcessTimeWide() - start);
    if (!w.logging) {
        // The once-a-second logging walk reads everything: kept out of the walk times.
        cov_walks.fetch_add(1, std::memory_order_relaxed);
        cov_walk_us.fetch_add(us, std::memory_order_relaxed);
        u32 max = cov_walk_max_us.load(std::memory_order_relaxed);
        while (us > max && !cov_walk_max_us.compare_exchange_weak(max, us)) {
        }
        cv.last_tracked = w.tracked;
        cv.last_rev_skipped = w.rev_skipped;
        cv.last_loose = w.loose;
        cv.last_full_changed = w.full_changed;
        cv.last_full_turn = w.full_turn;
        return;
    }
    cv.log_us = start;
    sceClibPrintf("[mh4u] cover: key %016llX%s cand %u area %llu (%llu%% of a screen, largest "
                  "%u%%) units %u groups %u panes %u%s full walk %u us%s\n",
                  static_cast<unsigned long long>(raw), plausible ? "" : " (gated: too small)",
                  w.candidates, static_cast<unsigned long long>(w.area),
                  static_cast<unsigned long long>(w.area * 100 / ScreenArea),
                  static_cast<u32>(u64{w.big} * 100 / ScreenArea), w.units, w.groups, w.panes,
                  w.panes > MaxPanes ? " (budget hit)" : "", us, cv.field ? " field" : "");
    sceClibPrintf("[mh4u] cover: last walk: %u groups unchanged by their revision%s, %u by "
                  "tracked candidates, %u left (field), %u in full for a changed header, %u in "
                  "turn; %u turn diffs this second; reads %u direct %u checked, %u units\n",
                  cv.last_rev_skipped, cv.rev_off ? " (rule off)" : "", cv.last_tracked,
                  cv.last_loose, cv.last_full_changed, cv.last_full_turn, cv.verify_diffs,
                  g_reads_direct, g_reads_checked, static_cast<u32>(cv.units.size()));
    cv.verify_diffs = 0;
    g_reads_direct = 0;
    g_reads_checked = 0;
    sceClibPrintf("[mh4u] cover: trap %u key since %u, %s, proof %s tag %u covered %u late %u, "
                  "hidden %u\n",
                  g_seq, cv.key_start, cv.armed ? "armed" : cv.why,
                  cv.have_proof ? "newest" : "none", cv.proof.tag, cv.proof.covered ? 1 : 0,
                  cv.proof.late3d, static_cast<u32>(cv.hidden.size()));
    for (u32 i = 0; i < w.log_count; i++) {
        const GroupLog& gl = w.logs[i];
        sceClibPrintf("[mh4u] cover:   %-24s order %08X flags %08X a%u: %u cand (%u win) %u px, "
                      "largest %dx%d %u px tex %08X %08X; rev %08X queue %08X header words "
                      "changed %05X, %u tracked\n",
                      GuestString(gl.name, 32).c_str(), gl.order, gl.flags, gl.alpha,
                      gl.candidates, gl.windows, gl.area, static_cast<int>(gl.big_w),
                      static_cast<int>(gl.big_h), gl.big_area, gl.big_tex0, gl.big_tex1,
                      gl.revision, gl.queue, gl.header_diff, gl.nodes);
    }
    // The header diff is per second: start over.
    for (GroupEntry& e : cv.groups) {
        e.header_diff = 0;
    }
}

// ---------------------------------------------------------------------------------------------
// The decision and the skip (steps 4-5). The render thread's proofs arrive tagged with the
// trap count at the moment the guest queued the frame's display transfer. The game queues a
// frame's transfer after that frame's trap, before or after the next one, so a tag is the
// trap its frame was built at or up to TagLag later; every rule below allows for that in the
// safe direction.

constexpr u32 TagLag = 2;
constexpr u32 MinCoveredRun = 4; ///< covered proofs in a row before an arm
constexpr u32 ProofMaxAge = 30;
constexpr u32 MaxTransitionLines = 64;

bool Blacklisted(u64 key) {
    return std::find(cv.blacklist.begin(), cv.blacklist.end(), key) != cv.blacklist.end();
}

bool WasArmed(u32 seq) {
    return seq != 0 && cv.armed_at[seq % 64] == seq;
}

/// One line per arm/disarm (and mispredict), at most MaxTransitionLines per process.
void LogTransition(const char* what, const std::string& detail) {
    if (cv.lines_logged < MaxTransitionLines) {
        sceClibPrintf("[mh4u] cover: %s at trap %u (key %016llX since %u, proof tag %u)%s\n",
                      what, g_seq, static_cast<unsigned long long>(cv.key), cv.key_start,
                      cv.proof.tag, detail.c_str());
    } else if (cv.lines_logged == MaxTransitionLines) {
        sceClibPrintf("[mh4u] cover: further arm/disarm lines not logged\n");
    }
    cv.lines_logged++;
}

/// Sets the view-0 bit again on every listed unit that is still alive with the same class,
/// then forgets the list. A unit freed meanwhile is not found and not written.
void RestoreWorld() {
    if (cv.hidden.empty()) {
        return;
    }
    ForEachUnit([&](u32 unit, u32 vtable, u32 flags, bool, bool) {
        const auto it = std::lower_bound(cv.hidden.begin(), cv.hidden.end(),
                                         Cover::Hidden{unit, 0});
        if (it != cv.hidden.end() && it->unit == unit && it->vtable == vtable &&
            (flags & ViewZeroBit) == 0) {
            g_memory->Write32(unit + 0x04, flags | ViewZeroBit);
        }
    });
    cv.hidden.clear();
}

/// Clears the view-0 bit of every non-layout unit that has it, noting each: sUnit::draw then
/// skips their draw calls (only those - updates run apart). New units each trap too.
void HideWorld() {
    const auto hide = [&](u32 unit, u32 vtable, u32 flags, bool layout, bool drawn) {
        if (!drawn || layout || vtable == 0 || (flags & ViewZeroBit) == 0) {
            return;
        }
        g_memory->Write32(unit + 0x04, flags & ~ViewZeroBit);
        const Cover::Hidden h{unit, vtable};
        const auto it = std::lower_bound(cv.hidden.begin(), cv.hidden.end(), h);
        if (it == cv.hidden.end() || it->unit != unit) {
            cv.hidden.insert(it, h);
        } else {
            it->vtable = vtable; // the address now holds another unit: this one is ours
        }
    };
    if (cv.units_seq == g_seq) {
        // This trap's walk saw every unit already (the guest has not run since).
        for (const Cover::UnitSeen& u : cv.units) {
            hide(u.unit, u.vtable, u.flags, u.layout, u.drawn);
        }
    } else {
        ForEachUnit(hide);
    }
    const u32 n = static_cast<u32>(cv.hidden.size());
    u32 max = cov_hidden.load(std::memory_order_relaxed);
    while (n > max && !cov_hidden.compare_exchange_weak(max, n)) {
    }
}

void Disarm(const char* why) {
    static const bool shadow = Switch("covershadow");
    if (!shadow) {
        RestoreWorld();
    }
    cv.hidden.clear();
    if (cv.armed) {
        LogTransition(shadow ? "would disarm" : "disarm", fmt::format(": {}", why));
    }
    cv.armed = false;
    Common::Mh4uCover::armed.store(false, std::memory_order_relaxed);
}

/// Takes the proofs the render thread published since the last trap: the newest by tag, and
/// the mispredict net - a frame built while armed (every trap its tag can name) proven not
/// covered blacklists the key it was armed under.
void TakeProofs() {
    namespace MC = Common::Mh4uCover;
    const u32 head = MC::proof_head.load(std::memory_order_acquire);
    if (head - cv.proof_read > MC::ProofRingSize) {
        cv.proof_read = head - MC::ProofRingSize;
    }
    for (; cv.proof_read != head; cv.proof_read++) {
        const MC::Proof p = MC::ReadProof(cv.proof_read);
        // The proof uncovers what a world-dependent draw touched; late3d (how many came after
        // the first cover) is for the logs.
        const bool good = p.covered;
        if (good) {
            cov_proofs.fetch_add(1, std::memory_order_relaxed);
        }
        // Covered proofs in a row: a cover that holds only every other frame never arms.
        cv.covered_run = good ? cv.covered_run + 1 : 0;
        if (!cv.have_proof || static_cast<s32>(p.tag - cv.proof.tag) >= 0) {
            cv.proof = p;
            cv.have_proof = true;
        }
        if (good || p.tag <= TagLag) {
            continue;
        }
        bool all_armed = true;
        for (u32 s = p.tag - TagLag; s <= p.tag && all_armed; s++) {
            all_armed = WasArmed(s);
        }
        if (!all_armed) {
            continue;
        }
        const u64 key = cv.key_at[p.tag % 64];
        cov_mispredicts.fetch_add(1, std::memory_order_relaxed);
        cv.blacklist[cv.blacklist_next++ % cv.blacklist.size()] = key;
        LogTransition("proof", fmt::format(": MISPREDICT, frame tag {} {} (late {}); key {:016X} "
                                           "blacklisted",
                                           p.tag, p.covered ? "covered" : "not covered",
                                           p.late3d, key));
        if (cv.armed && cv.armed_key == key) {
            Disarm("mispredict");
        }
    }
}

void Decide() {
    static const bool no_skip = Switch("nocoverskip");
    static const bool shadow = Switch("covershadow");
    if (no_skip) {
        return; // the trap, the key and the proofs only
    }
    const char* why = nullptr;
    if (cv.key == 0) {
        why = "no cover key";
    } else if (Blacklisted(cv.key)) {
        why = "key blacklisted";
    } else if (!cv.have_proof) {
        why = "no proof";
    } else if (!cv.proof.covered) {
        why = "not covered";
    } else if (!cv.armed && cv.covered_run < MinCoveredRun) {
        why = "too few covered proofs in a row";
    } else if (static_cast<s32>(cv.proof.tag - (cv.key_start + 1 + TagLag)) < 0) {
        why = "no proof since the key changed";
    } else if (g_seq - cv.proof.tag >= ProofMaxAge) {
        why = "proof too old";
    }
    if (why != nullptr) {
        cv.why = why;
        if (cv.armed) {
            Disarm(why);
        }
        return;
    }
    if (!cv.armed) {
        cv.armed = true;
        cv.armed_key = cv.key;
        Common::Mh4uCover::armed.store(true, std::memory_order_relaxed);
        LogTransition(shadow ? "would arm" : "arm", "");
    }
    cv.armed_at[g_seq % 64] = g_seq;
    cv.key_at[g_seq % 64] = cv.key;
    cov_armed.fetch_add(1, std::memory_order_relaxed);
    if (!shadow) {
        HideWorld();
    }
}

} // namespace

bool Switch(const char* name) {
    if (std::FILE* f = std::fopen((std::string(VITA_DATA_DIR "/") + name).c_str(), "rb")) {
        std::fclose(f);
        return true;
    }
    return GxmRenderer::GxmFlag(name);
}

bool BottomHidden() {
    static const bool keep_bottom = Switch("nobottomskip");
    return !keep_bottom &&
           Settings::values.layout_option.GetValue() == Settings::LayoutOption::SingleScreen &&
           !Settings::values.swap_screen.GetValue();
}

void Tick(Core::System& system) {
    static const bool off = Switch("nohooks");
    if (off) {
        return;
    }
    // Once a guest frame (a top-screen transfer); every 64 slices while frames stand still.
    const u32 frames = Common::PipelineStats::guest_frames_total.load(std::memory_order_relaxed);
    if (frames == g.last_frame_total && ++g.slices_idle < 64) {
        return;
    }
    g.slices_idle = 0;
    const bool new_frame = frames != g.last_frame_total;
    g.last_frame_total = frames;

    const auto process = system.Kernel().GetCurrentProcess();
    if (!process || process->codeset == nullptr) {
        return;
    }
    if (process.get() != g.process) {
        // Boot or a state load: look again, and wait a second for the game to settle.
        const bool bottom_written = g.bottom_written;
        g = State{};
        g.bottom_written = bottom_written;
        g.process = process.get();
        g.last_frame_total = frames;
        g_memory = &system.Memory();
        // The cover state goes with the old process: its hidden list names units of a game
        // that is gone, so nothing is written back; proofs queued so far are dropped.
        cv = Cover{};
        cv.proof_read = Common::Mh4uCover::proof_head.load(std::memory_order_acquire);
        cv.last_trap_frames = frames;
        Common::Mh4uCover::armed.store(false, std::memory_order_relaxed);
        Common::Mh4uCover::key_present.store(false, std::memory_order_relaxed);
        return;
    }
    // Cover watchdog: the world stays hidden only while the main loop keeps trapping. Frames
    // shown without a trap (a loop of its own: loading, an error screen) put it back. Three,
    // not one: the frame count moves on the GX thread and can bunch up behind a slow list.
    if (cv.armed && frames - cv.last_trap_frames >= 3 && g.version != nullptr) {
        g_memory = &system.Memory();
        Disarm("watchdog: frames without a trap");
    }
    if (new_frame && g.frames_seen < 60) {
        g.frames_seen++;
        return;
    }
    if (g.frames_seen < 60) {
        return;
    }
    if (g.version == nullptr) {
        g.version = DetectIn(*process, system.Memory());
        if (g.version == nullptr) {
            if (!g.unknown_logged) {
                g.unknown_logged = true;
                sceClibPrintf("[mh4u] hooks: unknown executable (program %016llX, %08X %08X)\n",
                              static_cast<unsigned long long>(process->codeset->program_id),
                              Read32(0x00E6CAD8), Read32(0x00E6CAA8));
            }
            return;
        }
        sceClibPrintf("[mh4u] hooks: %s\n", g.version->name);
    }
    const u32 lt_render = Read32(g.version->lt_render);
    if (lt_render == 0 || !Valid(lt_render + PresentBottom)) {
        return;
    }
    LogRenderWords(lt_render);
    LogFilters();
    BottomPass(lt_render);
    Dither(lt_render);
}

CoverSite FindCoverSite(const Kernel::Process& process, Memory::MemorySystem& memory) {
    const Version* version = DetectIn(process, memory);
    return version != nullptr ? CoverSite{version->trap, version->name} : CoverSite{};
}

bool IsCoverSite(u32 addr) {
    return addr == UsaV11.trap || addr == UsaV10.trap || addr == EurV11.trap;
}

void OnTrap(Core::System& system) {
    cov_traps.fetch_add(1, std::memory_order_relaxed);
    // The frame tag: counted for every trap, so the render thread's proofs stay in step.
    g_seq++;
    Common::Mh4uCover::trap_seq.store(g_seq, std::memory_order_relaxed);
    static const bool off = Switch("nohooks");
    if (off || g.version == nullptr || g.process == nullptr || g_memory == nullptr) {
        return;
    }
    // Tick adopts a new process (boot, state load) and detects its version a second later;
    // until then the walk has no addresses to trust.
    if (system.Kernel().GetCurrentProcess().get() != g.process) {
        return;
    }
    cv.last_trap_frames =
        Common::PipelineStats::guest_frames_total.load(std::memory_order_relaxed);
    // The guest's own page table (it is the current one at the trap): host pointers per page.
    const std::shared_ptr<Memory::PageTable> table = g_memory->GetCurrentPageTable();
    g_pages = table ? table->GetPointerArray().data() : nullptr;
    TakeProofs();
    CoverKey();
    // Before this frame's render is built: an arm or a disarm here takes effect in it.
    Decide();
    g_pages = nullptr;
}

CoverStats TakeCoverStats() {
    CoverStats out;
    out.traps = cov_traps.exchange(0, std::memory_order_relaxed);
    out.key_changes = cov_key_changes.exchange(0, std::memory_order_relaxed);
    const u32 walks = cov_walks.exchange(0, std::memory_order_relaxed);
    const u32 us = cov_walk_us.exchange(0, std::memory_order_relaxed);
    out.walk_avg_us = walks != 0 ? us / walks : 0;
    out.walk_max_us = cov_walk_max_us.exchange(0, std::memory_order_relaxed);
    out.armed = cov_armed.exchange(0, std::memory_order_relaxed);
    out.mispredicts = cov_mispredicts.exchange(0, std::memory_order_relaxed);
    out.hidden = cov_hidden.exchange(0, std::memory_order_relaxed);
    out.proofs = cov_proofs.exchange(0, std::memory_order_relaxed);
    return out;
}

void BeforeSave(Core::System& system) {
    if (g.version == nullptr) {
        return;
    }
    g_memory = &system.Memory();
    if (cv.armed || !cv.hidden.empty()) {
        // The world back on: a state carries every unit's view bit as the game left it. The
        // next trap arms again if the cover still holds.
        Disarm("save state");
    }
    if (!g.bottom_written && g.dither_cleared == 0) {
        return;
    }
    const u32 lt_render = Read32(g.version->lt_render);
    if (lt_render != 0 && Valid(lt_render + PresentBottom)) {
        if (g.bottom_written) {
            g_memory->Write8(lt_render + PresentBottom, 1);
        }
        if (g.dither_cleared != 0) {
            g_memory->Write32(lt_render + RenderWordEdit,
                              Read32(lt_render + RenderWordEdit) | g.dither_cleared);
        }
    }
    g.bottom_written = false;
    g.dither_cleared = 0;
}

} // namespace Mh4uHooks
