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
#include <fmt/format.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>

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
    // Cover key (OnTrap): the last gated key and when the last coverlog line was printed.
    u64 cover_key = 0;
    u64 cover_log_us = 0;
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
// opaque 2D panes into one number. A later step arms the world skip only while this key equals
// the key of a frame the render thread proved covered; any change to a cover pane changes the
// key before that frame is built. Read-only: nothing here writes guest memory.
//
// Objects (docs/plans/vita-mh4u-2d-layer.md, checked against the v1.1 disassembly):
// - sUnit: [sUnit+0x614] lines (sUnit::draw's count), line i at sUnit+0x14+i*0x18, drawn only
//   while bit 2 of line+0x08 is set; first unit at line+0x0C, next at unit+0x0C. A unit draws
//   only in state 2 (unit+0x04 & 7) with bits 11 and 14 of unit+0x04 set.
// - Layout unit: draw list at +0x120, groups linked through group+0x48.
// - Group (0x50): +0x04 name, +0x08 first pane, +0x18 order, +0x24 texture array, +0x28 x/y/z,
//   +0x3C RGBA, +0x40 revision (volatile), +0x44 flags (0x400 shown, 0x200 re-recorded this
//   frame: volatile).
// - Pane (0x24): +0x08 data, +0x10 kind (0 sprite, 1 null, 2 text, 3 boundary, 4 window),
//   +0x14 next, +0x20 first child (nulls).
// - Sprite (0x68): +0x00..+0x0B texture words, +0x10 x/y/z, +0x20 size, +0x28 scale, +0x30 uv,
//   +0x40 four RGBA corners, +0x50 rotation, +0x58 bits 0-3 origin / bit 7 visible / bits 8+
//   vertex slot (volatile). Windows are read with the same layout (medium confidence).
// - Null (0x30): +0x00 x/y/z, +0x10 scale, +0x18 RGBA, +0x1C rotation, +0x24 bit 0 visible /
//   bits 1+ matrix slot (volatile).

constexpr u32 MaxUnitLines = 64;    // sUnit's line array ends at +0x614 = 0x14 + 64 * 0x18
constexpr u32 MaxUnitsPerLine = 1024;
constexpr u32 MaxGroupsPerUnit = 512;
constexpr u32 MaxPanes = 16384;     // per walk: a runaway list cannot stall the frame
constexpr u32 MaxPanesPerList = 2048;
constexpr u32 MaxNullDepth = 12;
constexpr float MinCandidateSide = 24.0f;
constexpr u32 ScreenW = 400, ScreenH = 240;

float AsFloat(u32 word) {
    float f;
    std::memcpy(&f, &word, sizeof(f));
    return f;
}

u64 Mix(u64 h, u32 v) {
    h = (h ^ v) * 0x9E3779B97F4A7C15ull;
    return h ^ (h >> 29);
}

/// Copies `size` (< one page) bytes of guest memory in one go; false, nothing read, unless both
/// ends are mapped (so every page in between is).
bool ReadGuest(u32 addr, void* out, u32 size) {
    if (addr == 0 || (addr & 3) != 0 || addr + size < addr || !Valid(addr) ||
        !Valid(addr + size - 1)) {
        return false;
    }
    g_memory->ReadBlock(*g.process, addr, out, size);
    return true;
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
};

struct CoverWalk {
    bool logging = false;
    u32 panes = 0;
    u64 key = 0xCBF29CE484222325ull;
    u32 candidates = 0;
    u64 area = 0; ///< clipped screen pixels of all candidates, overlaps counted each time
    u32 units = 0, groups = 0;
    std::array<GroupLog, 24> logs{};
    u32 log_count = 0;
    GroupLog* current = nullptr; ///< the group being walked, when logging
};

/// A visible sprite (or window) with four opaque corners, no rotation and a scaled size of at
/// least 24x24: hash it and add its clipped area. The area model is simple - positions add,
/// scales multiply, origin 0-8 as a 3x3 grid (row 0 top), screen point (200 - x, 120 - y) - and
/// serves only as a plausibility gate.
void Candidate(u32 data, u32 kind, const Xform& xf, u64 chain, CoverWalk& w) {
    std::array<u32, 0x68 / 4> s;
    if (!ReadGuest(data, s.data(), 0x68)) {
        return;
    }
    const u32 flags = s[0x58 / 4];
    if ((flags & 0x80) == 0) {
        return;
    }
    for (u32 c = 0x40; c < 0x50; c += 4) {
        if ((s[c / 4] >> 24) != 0xFF) {
            return;
        }
    }
    if (AsFloat(s[0x50 / 4]) != 0.0f) {
        return; // rotated (or NaN)
    }
    const float width = std::fabs(AsFloat(s[0x20 / 4]) * AsFloat(s[0x28 / 4]) * xf.sx);
    const float height = std::fabs(AsFloat(s[0x24 / 4]) * AsFloat(s[0x2C / 4]) * xf.sy);
    if (!(width >= MinCandidateSide && height >= MinCandidateSide && width < 1e5f &&
          height < 1e5f)) {
        return; // too small, or not a number
    }
    const float px = xf.x + xf.sx * AsFloat(s[0x10 / 4]);
    const float py = xf.y + xf.sy * AsFloat(s[0x14 / 4]);
    if (!(std::fabs(px) < 1e5f && std::fabs(py) < 1e5f)) {
        return;
    }

    // Key: the sprite's words up to its flags, the flags without the vertex slot, its kind and
    // the words of the group and nulls above it.
    u64 h = w.key;
    for (u32 i = 0; i < 0x58 / 4; i++) {
        h = Mix(h, s[i]);
    }
    h = Mix(h, flags & 0xFF);
    h = Mix(h, kind);
    w.key = Mix(h, static_cast<u32>(chain) ^ static_cast<u32>(chain >> 32));
    w.candidates++;

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
    w.area += area;

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
}

void WalkPanes(u32 pane, const Xform& xf, u64 chain, u32 depth, CoverWalk& w) {
    for (u32 n = 0; pane != 0 && n < MaxPanesPerList; n++) {
        if (++w.panes > MaxPanes) {
            return;
        }
        std::array<u32, 0x24 / 4> p;
        if (!ReadGuest(pane, p.data(), 0x24)) {
            return;
        }
        const u32 kind = p[0x10 / 4] & 0xFF;
        const u32 data = p[0x08 / 4];
        const u32 child = p[0x20 / 4];
        if (kind == 0 || kind == 4) {
            Candidate(data, kind, xf, chain, w);
        } else if (kind == 1 && child != 0 && depth < MaxNullDepth) {
            // A null: its subtree counts only while the null is visible, opaque and unrotated.
            std::array<u32, 0x28 / 4> nl;
            if (ReadGuest(data, nl.data(), 0x28) && (nl[0x24 / 4] & 1) != 0 &&
                (nl[0x18 / 4] >> 24) == 0xFF && AsFloat(nl[0x1C / 4]) == 0.0f) {
                const Xform inner{xf.x + xf.sx * AsFloat(nl[0]), xf.y + xf.sy * AsFloat(nl[1]),
                                  xf.sx * AsFloat(nl[0x10 / 4]), xf.sy * AsFloat(nl[0x14 / 4])};
                u64 c = chain;
                for (u32 i = 0; i < 0x24 / 4; i++) {
                    c = Mix(c, nl[i]);
                }
                c = Mix(c, nl[0x24 / 4] & 1); // the matrix slot above bit 0 is volatile
                WalkPanes(child, inner, c, depth + 1, w);
            }
        }
        // Text (2) and boundaries (3) are never candidates: text is many small glyphs and
        // changes for reasons that do not move a cover.
        pane = p[0x14 / 4];
    }
}

void WalkLayout(u32 unit, u32 unit_flags, CoverWalk& w) {
    u32 group = Read32(unit + 0x120);
    for (u32 n = 0; group != 0 && n < MaxGroupsPerUnit && w.panes <= MaxPanes; n++) {
        std::array<u32, 0x50 / 4> gw;
        if (!ReadGuest(group, gw.data(), 0x50)) {
            return;
        }
        const u32 flags = gw[0x44 / 4];
        const u32 next = gw[0x48 / 4];
        // Shown groups of any view (the mod picks the view at draw time), at full alpha.
        if ((flags & 0x400) != 0 && (gw[0x3C / 4] >> 24) == 0xFF) {
            w.groups++;
            u64 chain = Mix(0x84222325u, unit_flags);
            chain = Mix(chain, gw[0x18 / 4]);  // order
            chain = Mix(chain, gw[0x24 / 4]);  // texture array
            chain = Mix(chain, gw[0x28 / 4]);  // x, y, z
            chain = Mix(chain, gw[0x2C / 4]);
            chain = Mix(chain, gw[0x30 / 4]);
            chain = Mix(chain, gw[0x3C / 4]);  // colour
            chain = Mix(chain, flags & ~0x200u); // 0x200: re-recorded this frame
            GroupLog* gl = nullptr;
            if (w.logging && w.log_count < w.logs.size()) {
                gl = &w.logs[w.log_count];
                *gl = GroupLog{group, gw[0x04 / 4], gw[0x18 / 4], flags, gw[0x3C / 4] >> 24};
            }
            w.current = gl;
            const u32 before = w.candidates;
            WalkPanes(gw[0x08 / 4], Xform{AsFloat(gw[0x28 / 4]), AsFloat(gw[0x2C / 4]), 1, 1},
                      chain, 0, w);
            w.current = nullptr;
            if (gl != nullptr && w.candidates != before) {
                w.log_count++;
            }
        }
        group = next;
    }
}

void CoverKey() {
    static const bool log_enabled = Switch("coverlog");
    const SceUInt64 start = sceKernelGetProcessTimeWide();
    CoverWalk w;
    w.logging = log_enabled && start - g.cover_log_us >= 1'000'000;

    const u32 units = Read32(g.version->unit);
    const u32 lines = std::min<u32>(units != 0 ? Read32(units + 0x614) : 0, MaxUnitLines);
    for (u32 i = 0; i < lines && w.panes <= MaxPanes; i++) {
        const u32 line = units + 0x14 + i * 0x18;
        if ((Read32(line + 0x08) & 4) == 0) {
            continue; // sUnit::draw skips the line
        }
        u32 unit = Read32(line + 0x0C);
        for (u32 n = 0; unit != 0 && n < MaxUnitsPerLine; n++) {
            const u32 vtable = Read32(unit);
            const u32 flags = Read32(unit + 0x04);
            const bool layout =
                vtable != 0 && (vtable == g.version->monnyan ||
                                Read32(vtable + 0x30) == g.version->layout_draw);
            if (layout && (flags & 7) == 2 && (flags & 0x4800) == 0x4800) {
                w.units++;
                WalkLayout(unit, flags, w);
            }
            unit = Read32(unit + 0x0C);
        }
    }

    const u64 raw = w.candidates != 0 ? (w.key != 0 ? w.key : 1) : 0;
    // Plausibility: the candidates must at least add up to one screen.
    const bool plausible = w.area >= u64{ScreenW} * ScreenH;
    const u64 key = plausible ? raw : 0;
    if (key != g.cover_key) {
        g.cover_key = key;
        cov_key_changes.fetch_add(1, std::memory_order_relaxed);
    }

    const u32 us = static_cast<u32>(sceKernelGetProcessTimeWide() - start);
    cov_walks.fetch_add(1, std::memory_order_relaxed);
    cov_walk_us.fetch_add(us, std::memory_order_relaxed);
    u32 max = cov_walk_max_us.load(std::memory_order_relaxed);
    while (us > max && !cov_walk_max_us.compare_exchange_weak(max, us)) {
    }

    if (!w.logging) {
        return;
    }
    g.cover_log_us = start;
    sceClibPrintf("[mh4u] cover: key %016llX%s cand %u area %llu (%llu%% of a screen) units %u "
                  "groups %u panes %u%s walk %u us\n",
                  static_cast<unsigned long long>(raw), plausible ? "" : " (gated: too small)",
                  w.candidates, static_cast<unsigned long long>(w.area),
                  static_cast<unsigned long long>(w.area * 100 / (ScreenW * ScreenH)), w.units,
                  w.groups, w.panes, w.panes > MaxPanes ? " (budget hit)" : "", us);
    for (u32 i = 0; i < w.log_count; i++) {
        const GroupLog& gl = w.logs[i];
        sceClibPrintf("[mh4u] cover:   %-24s order %08X flags %08X a%u: %u cand (%u win) %u px, "
                      "largest %dx%d %u px tex %08X %08X\n",
                      GuestString(gl.name, 32).c_str(), gl.order, gl.flags, gl.alpha,
                      gl.candidates, gl.windows, gl.area, static_cast<int>(gl.big_w),
                      static_cast<int>(gl.big_h), gl.big_area, gl.big_tex0, gl.big_tex1);
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
        return;
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
    static const bool off = Switch("nohooks");
    if (off || g.version == nullptr || g.process == nullptr || g_memory == nullptr) {
        return;
    }
    // Tick adopts a new process (boot, state load) and detects its version a second later;
    // until then the walk has no addresses to trust.
    if (system.Kernel().GetCurrentProcess().get() != g.process) {
        return;
    }
    CoverKey();
}

CoverStats TakeCoverStats() {
    CoverStats out;
    out.traps = cov_traps.exchange(0, std::memory_order_relaxed);
    out.key_changes = cov_key_changes.exchange(0, std::memory_order_relaxed);
    const u32 walks = cov_walks.exchange(0, std::memory_order_relaxed);
    const u32 us = cov_walk_us.exchange(0, std::memory_order_relaxed);
    out.walk_avg_us = walks != 0 ? us / walks : 0;
    out.walk_max_us = cov_walk_max_us.exchange(0, std::memory_order_relaxed);
    return out;
}

void BeforeSave(Core::System& system) {
    if (g.version == nullptr || (!g.bottom_written && g.dither_cleared == 0)) {
        return;
    }
    g_memory = &system.Memory();
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
