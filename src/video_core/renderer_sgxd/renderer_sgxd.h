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

/// The renderer the Vita frontend presents through. Its rasterizer and presents run on the
/// render thread (RenderThread), which owns the GPU (sgxd) and the window while a title runs;
/// each VBlank's present hands the guest's screens to the frontend through the window's
/// PresentGuestScreens, on that thread, and the frontend draws them from guest memory
/// (EmuWindowVita::DrawGuestScreens).
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
    /// The render thread polls the window (PollWindow) with each VBlank's present and between
    /// presents; the frame's end on the emulation thread leaves it alone.
    void PollWindowEvents() override {}
    void PollWindow();
    /// The window's last present on screen once its GPU work is done (EmuWindow::FlipDone), and
    /// whether one waits, with its fence (FlipPending). The backend's GPU waits flip it too
    /// (SgxdBackend::SetFlipHook).
    void FlipDone();
    [[nodiscard]] bool FlipPending(u32* fence);
    [[nodiscard]] s64 FlipDueIn();
    bool WouldMakeUncached(PAddr addr, u32 size) const override;
    void MakeUncached(PAddr addr, u32 size, bool overwritten) override;
    bool UncachedFrameEnded() override {
        return guest_memory.EndFrame();
    }
    void ReturnUncached() override {
        guest_memory.ReturnUnused();
    }
    /// The render side's part in the page moves (GuestGpuMemory::MoveHooks; RenderThread).
    void SetMoveHooks(GuestGpuMemory::MoveHooks hooks) {
        guest_memory.SetMoveHooks(std::move(hooks));
    }

#ifdef VITA_DIAGNOSTICS
    /// The moves of guest FCRAM to CDRAM since the last call (`perflog`).
    GuestGpuMemory::MoveStats TakeMoveStats() {
        return guest_memory.TakeMoveStats();
    }
    /// GuestGpuMemory::DescribePages, for a fault's report.
    [[nodiscard]] std::string DescribePages(PAddr addr, u32 size) const {
        return guest_memory.DescribePages(addr, size);
    }
#endif

private:
    Memory::MemorySystem& memory;
    GuestGpuMemory guest_memory;
    RasterizerGxm rasterizer;
};

} // namespace GxmRenderer
