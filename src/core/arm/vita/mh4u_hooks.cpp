// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <fmt/format.h>
#include <psp2/kernel/clib.h>

#include "common/pipeline_stats.h"
#include "common/settings.h"
#include "common/vita_data_dir.h"
#include "core/arm/vita/mh4u_hooks.h"
#include "core/core.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"
#include "core/memory.h"
#include "video_core/renderer_gxm/gxm_flags.h"

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
                           {0x00E5D474, "fade"}}}};
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
                           {0x00E5D444, "fade"}}}};
// EUR v1.1: data (singletons, vtables) as USA v1.1; only code moved. Untested.
constexpr Version EurV11{"EUR v1.1", UsaV11.lt_render, UsaV11.scene, UsaV11.camera,
                         UsaV11.filters};

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
};
State g;

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

const Version* Detect(u64 program_id) {
    if (program_id == TitleUsa) {
        if (Read32(0x00E6CAD8) == 0x00BFA670) {
            return &UsaV11;
        }
        if (Read32(0x00E6CAA8) == 0x00BF9FF8) {
            return &UsaV10;
        }
    } else if (program_id == TitleEur && Read32(0x00E6CAD8) == 0x00BFA6C0) {
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
        g.version = Detect(process->codeset->program_id);
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
}

void BeforeSave(Core::System& system) {
    if (g.version == nullptr || !g.bottom_written) {
        return;
    }
    g_memory = &system.Memory();
    const u32 lt_render = Read32(g.version->lt_render);
    if (lt_render != 0 && Valid(lt_render + PresentBottom)) {
        g_memory->Write8(lt_render + PresentBottom, 1);
    }
    g.bottom_written = false;
}

} // namespace Mh4uHooks
