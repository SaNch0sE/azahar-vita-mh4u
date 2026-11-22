// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include "common/common_types.h"
#include "video_core/pica/regs_external.h"
#include "video_core/pica/regs_lcd.h"
#include "video_core/renderer_sgxd/guest_gpu_memory.h"
#include "video_core/renderer_sgxd/sgxd_rasterizer.h"

namespace Core {
class System;
}

namespace Frontend {
class EmuWindow;
}

namespace Pica {
class PicaCore;
}

namespace SgxdRenderer {

/// The renderer the Vita frontend presents through. Its rasterizer and presents run on the
/// render thread (RenderThread), which owns the GPU (sgxd) and the window while a title runs;
/// each VBlank's present hands the guest's screens to the frontend through the window's
/// PresentGuestScreens, on that thread, and the frontend draws them from guest memory
/// (EmuWindowVita::DrawGuestScreens).
class RendererSgxd {
public:
    explicit RendererSgxd(Core::System& system, Pica::PicaCore& pica, Frontend::EmuWindow& window);
    ~RendererSgxd();

    RendererSgxd(const RendererSgxd&) = delete;
    RendererSgxd& operator=(const RendererSgxd&) = delete;

    [[nodiscard]] RasterizerSgxd& Rasterizer() {
        return rasterizer;
    }

    /// Shows the latest frame at a VBlank, with the guest's screens as the VBlank found them:
    /// both framebuffer configurations and LCD fills (top, bottom).
    void PresentFrame(const std::array<Pica::FramebufferConfig, 2>& fb,
                      const std::array<Pica::ColorFill, 2>& fill);
    /// The frame's end on the emulation thread: the frame count, the performance counters and
    /// the pacing (the window's PaceFrame, else the frame limiter). The window's events are the
    /// render thread's (PollWindow).
    void EndFrame();
    /// The window's framebuffer layout made again from its size.
    void UpdateCurrentFramebufferLayout(bool is_portrait_mode = {});

    [[nodiscard]] Frontend::EmuWindow& GetRenderWindow() {
        return render_window;
    }

    /// The render thread polls the window with each VBlank's present and between presents.
    void PollWindow();
    /// The window's last present still being drawn: a present now would be dropped.
    bool PresentBusy();
    /// The window's last present on screen once its GPU work is done (EmuWindow::FlipDone), and
    /// whether one waits, with its fence (FlipPending). The backend's GPU waits flip it too
    /// (SgxdBackend::SetFlipHook).
    void FlipDone();
    [[nodiscard]] bool FlipPending(u32* fence);
    [[nodiscard]] s64 FlipDueIn();

    /// From the emulation thread: whether [addr, addr + size) would rather be somewhere the CPU
    /// reaches uncached, because the GPU is about to write it (a display transfer's output) or
    /// show it (a framebuffer the LCD names). The GPU then calls MakeUncached, the guest
    /// stopped.
    bool WouldMakeUncached(PAddr addr, u32 size) const;
    /// See WouldMakeUncached. `overwritten`: the GPU is about to write the whole range.
    void MakeUncached(PAddr addr, u32 size, bool overwritten);
    /// From the emulation thread at vblank, with the guest stopped: a frame has ended. Whether
    /// memory MakeUncached moved should go back (unused long enough, or the pool short of free
    /// pages), which ReturnUncached then does with the render thread held.
    bool UncachedFrameEnded() {
        return guest_memory.EndFrame();
    }
    void ReturnUncached() {
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
    Core::System& system;
    Frontend::EmuWindow& render_window;
    Memory::MemorySystem& memory;
    GuestGpuMemory guest_memory;
    RasterizerSgxd rasterizer;
};

} // namespace SgxdRenderer
