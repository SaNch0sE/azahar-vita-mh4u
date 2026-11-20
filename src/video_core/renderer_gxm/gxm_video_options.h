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
    Upscale upscale = Upscale::Linear;
    bool fxaa = false;
    /// Internal render scale in quarters: 3 = 0.75x, 4 = 1x .. 8 = 2x (next launch).
    u32 scale_q = 4;
    /// 4x MSAA on render targets (next launch).
    bool msaa = false;
    /// The top screen across the whole display (16:9) instead of the 3DS's 5:3 with side
    /// bars: for the Heavenfall mod built with --cheat-aspect-ratio 16:9, which widens the
    /// game's views and interface for a stretched picture.
    bool wide = false;
    /// The speed / frame rate counter in the top right corner (Ui::DrawStatsHud).
    bool show_fps = true;
    /// "Smooth gradients": the game's dither pattern off (render word bits, Mh4uHooks) and the
    /// top screen kept in 8-bit colour from the render target to the present
    /// (RasterizerGxm::SmoothDisplayTransfer) instead of the 3DS's 5:6:5 display buffers.
    bool smooth = true; // default on since 2026-11-20 (the user: sky bands gone, no cost)
    /// "HUD resolution": the game's 2D layer (HUD, text, menus) drawn at the screen's
    /// resolution over the stretched 3D picture (RasterizerGxm, HudBegin). 0 off (400x240),
    /// 1 the screen's resolution, 2 lighter (1.5x). Uses the 8-bit display copies.
    u32 hudres = 0;
};

/// The live options (loaded from the file on first use).
VideoOptions& Video();
/// Writes the live options to the file.
void SaveVideoOptions();

} // namespace GxmRenderer
