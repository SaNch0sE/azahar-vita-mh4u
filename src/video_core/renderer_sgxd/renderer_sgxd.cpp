// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "core/core.h"
#include "core/frontend/emu_window.h"
#include "video_core/renderer_gxm/renderer_gxm.h"

namespace GxmRenderer {

RendererSgxd::RendererSgxd(Core::System& system, Pica::PicaCore& pica, Frontend::EmuWindow& window)
    : VideoCore::RendererBase{system, window, nullptr}, rasterizer{pica} {}

RendererGxm::~RendererGxm() = default;

void RendererGxm::SwapBuffers() {
    // Nothing calls it: the GPU presents through PresentFrame at each VBlank.
}

} // namespace GxmRenderer
