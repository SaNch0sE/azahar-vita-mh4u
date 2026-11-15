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
};

/// The live options (loaded from the file on first use).
VideoOptions& Video();
/// Writes the live options to the file.
void SaveVideoOptions();

} // namespace GxmRenderer
