// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include "common/common_types.h"

namespace GxmRenderer {

/// Image options the pause menu sets, saved to ux0:data/mh4u/video.txt (key=value lines) and
/// read once at start. The present reads the live values every frame; options marked
/// "next launch" are read by the renderer when it is created.
struct VideoOptions {
    enum class Upscale : u32 { Linear = 0, SharpBilinear = 1, EdgeAware = 2 };
    // Defaults since 2026-11-20 (the user's choice): Sharp bilinear, no FPS counter, smooth
    // gradients, HUD resolution 1.33x (below). A saved options file keeps its own values, once
    // it carries the current version (gxm_video_options.cpp, OptionsVersion).
    Upscale upscale = Upscale::SharpBilinear;
    bool fxaa = false;
    /// Internal render scale in quarters: 3 = 0.75x, 4 = 1x .. 8 = 2x (next launch).
    u32 scale_q = 4;
    /// 4x MSAA on render targets (next launch).
    bool msaa = false;
    /// The top screen across the whole display (16:9) instead of the 3DS's 5:3 with side
    /// bars: for the Heavenfall mod built with --cheat-aspect-ratio 16:9, which widens the
    /// game's views and interface for a stretched picture.
    bool wide = true; // default 16:9 since 2026-11-20 (the user plays the Heavenfall 16:9 build)
    /// The speed / frame rate counter in the top right corner (Ui::DrawStatsHud).
    bool show_fps = false;
    /// "Smooth gradients": the game's dither pattern off (render word bits, Mh4uHooks) and the
    /// top screen kept in 8-bit colour from the render target to the present
    /// (RasterizerGxm::SmoothDisplayTransfer) instead of the 3DS's 5:6:5 display buffers.
    bool smooth = true; // default on since 2026-11-20 (the user: sky bands gone, no cost)
    /// "HUD resolution": the game's 2D layer (HUD, text, menus) drawn at the screen's
    /// resolution over the stretched 3D picture (RasterizerGxm, HudBegin). 0 off (400x240),
    /// 1 the screen's resolution, 2 lighter (1.5x), 3 1.25x, 4 1.33x, 5 1.4x, 6 1.45x. Uses the 8-bit display copies.
    u32 hudres = 4; // 1.33x by default since 2026-11-20 (headroom for Sharp / FXAA)
};

/// The live options (loaded from the file on first use).
VideoOptions& Video();
/// The options as the app started (the first Video() call), for settings that apply on the next
/// launch.
const VideoOptions& VideoAtLaunch();
/// With the hi-res HUD on at launch, the upscale filter and FXAA keep their launch values until
/// a restart: changed live, the HUD copy and the present's own frames disagreed for a while and
/// the interface jiggled (2026-11-20). The menu asks for a restart instead.
bool FilterLocked();
VideoOptions::Upscale EffectiveUpscale();
bool EffectiveFxaa();
/// Writes the live options to the file.
void SaveVideoOptions();

} // namespace GxmRenderer
