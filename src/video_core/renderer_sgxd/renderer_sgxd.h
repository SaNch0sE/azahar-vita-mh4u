// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include "video_core/renderer_base.h"
#include "video_core/renderer_sgxd/guest_gpu_memory.h"
#include "video_core/renderer_sgxd/sgxd_rasterizer.h"

namespace Pica {
class PicaCore;
}

namespace GxmRenderer {

/// The renderer the Vita frontend presents through. Its operations run on the GPU's render
/// thread, which owns the GPU (sgxd) while a title runs; each VBlank's present hands the guest's
/// screens to the frontend through the window's PresentGuestScreens, on that thread, and the
/// frontend draws them from guest memory.
class RendererGxm : public VideoCore::RendererBase {
public:
    explicit RendererGxm(Core::System& system, Pica::PicaCore& pica, Frontend::EmuWindow& window);
    ~RendererGxm() override;

    [[nodiscard]] VideoCore::RasterizerInterface* Rasterizer() override {
        return &rasterizer;
    }

    void SwapBuffers() override;
    void TryPresent(int timeout_ms, bool is_secondary) override {}

    void PresentFrame(const std::array<Pica::FramebufferConfig, 2>& fb,
                      const std::array<Pica::ColorFill, 2>& fill) override;

private:
    Memory::MemorySystem& memory;
    GuestGpuMemory guest_memory;
    RasterizerGxm rasterizer;
};

} // namespace GxmRenderer
