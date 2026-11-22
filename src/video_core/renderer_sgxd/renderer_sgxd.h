// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include "video_core/renderer_base.h"
#include "video_core/renderer_gxm/gxm_rasterizer.h"

namespace Pica {
class PicaCore;
}

namespace GxmRenderer {

/// The renderer the Vita frontend presents through. The frontend owns the GXM context and the
/// display queue; a finished frame is handed to it through the window's SwapBuffers.
class RendererGxm : public VideoCore::RendererBase {
public:
    explicit RendererGxm(Core::System& system, Pica::PicaCore& pica, Frontend::EmuWindow& window);
    ~RendererGxm() override;

    [[nodiscard]] VideoCore::RasterizerInterface* Rasterizer() override {
        return &rasterizer;
    }

    void SwapBuffers() override;
    void TryPresent(int timeout_ms, bool is_secondary) override {}

private:
    RasterizerGxm rasterizer;
};

} // namespace GxmRenderer
