// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include "common/common_types.h"
#include "core/frontend/framebuffer_layout.h"
#include "video_core/pica/regs_external.h"
#include "video_core/pica/regs_lcd.h"
#include "video_core/rasterizer_interface.h"

namespace Frontend {
class EmuWindow;
}

namespace Core {
class System;
}

namespace VideoCore {

enum class ScreenId : u32 {
    TopLeft,
    TopRight,
    Bottom,
};

struct RendererSettings {
    // Screenshot
    std::atomic_bool screenshot_requested{false};
    void* screenshot_bits{};
    std::function<void(bool)> screenshot_complete_callback;
    Layout::FramebufferLayout screenshot_framebuffer_layout;
    // Renderer
    std::atomic_bool bg_color_update_requested{false};
    std::atomic_bool shader_update_requested{false};
};

class RendererBase : NonCopyable {
public:
    explicit RendererBase(Core::System& system, Frontend::EmuWindow& window,
                          Frontend::EmuWindow* secondary_window);
    virtual ~RendererBase();

    /// Returns the rasterizer owned by the renderer
    virtual VideoCore::RasterizerInterface* Rasterizer() = 0;

    /// Finalize rendering the guest frame and draw into the presentation texture
    virtual void SwapBuffers() = 0;

    /// Shows the latest frame at a VBlank, with the guest's screens as the VBlank found them:
    /// both framebuffer configurations and LCD fills (top, bottom).
    virtual void PresentFrame([[maybe_unused]] const std::array<Pica::FramebufferConfig, 2>& fb,
                              [[maybe_unused]] const std::array<Pica::ColorFill, 2>& fill) {}

    /// From the emulation thread: whether the renderer would rather keep [addr, addr + size)
    /// somewhere the CPU reaches uncached, because the GPU is about to write it (a display
    /// transfer's output) or show it (a framebuffer the LCD names). The GPU then calls
    /// MakeUncached, the guest stopped.
    virtual bool WouldMakeUncached([[maybe_unused]] PAddr addr, [[maybe_unused]] u32 size) const {
        return false;
    }
    /// See WouldMakeUncached. `overwritten`: the GPU is about to write the whole range.
    virtual void MakeUncached([[maybe_unused]] PAddr addr, [[maybe_unused]] u32 size,
                              [[maybe_unused]] bool overwritten) {}
    /// From the emulation thread at vblank, with the guest stopped: a frame has ended. Whether
    /// memory MakeUncached moved has gone unused long enough to go back, which the GPU then
    /// has done by ReturnUncached.
    virtual bool UncachedFrameEnded() {
        return false;
    }
    /// See UncachedFrameEnded.
    virtual void ReturnUncached() {}

    /// Draws the latest frame to the window waiting timeout_ms for a frame to arrive (Renderer
    /// specific implementation)
    virtual void TryPresent(int timeout_ms, bool is_secondary) = 0;
    virtual void TryPresent(int timeout_ms) {
        TryPresent(timeout_ms, false);
    }

    /// Prepares for video dumping (e.g. create necessary buffers, etc)
    virtual void PrepareVideoDumping() {}

    /// Cleans up after video dumping is ended
    virtual void CleanupVideoDumping() {}

    /// This is called to notify the rendering backend of a surface change
    // if second == true then it is the second screen
    virtual void NotifySurfaceChanged(bool second) {}

    /// Returns the resolution scale factor relative to the native 3DS screen resolution
    u32 GetResolutionScaleFactor();

    /// Updates the framebuffer layout of the contained render window handle.
    void UpdateCurrentFramebufferLayout(bool is_portrait_mode = {});

    /// Ends the current frame
    void EndFrame();

    /// The window's events, which EndFrame polls. The GXM renderer's render thread polls them
    /// with each present instead: the window is that thread's.
    virtual void PollWindowEvents();

    f32 GetCurrentFPS() const {
        return current_fps;
    }

    s32 GetCurrentFrame() const {
        return current_frame;
    }

    Frontend::EmuWindow& GetRenderWindow() {
        return render_window;
    }

    const Frontend::EmuWindow& GetRenderWindow() const {
        return render_window;
    }

    [[nodiscard]] RendererSettings& Settings() {
        return settings;
    }

    [[nodiscard]] const RendererSettings& Settings() const {
        return settings;
    }

    /// Returns true if a screenshot is being processed
    [[nodiscard]] bool IsScreenshotPending() const;

    /// Request a screenshot of the next frame
    void RequestScreenshot(void* data, std::function<void(bool)> callback,
                           const Layout::FramebufferLayout& layout);

protected:
    Core::System& system;
    RendererSettings settings;
    Frontend::EmuWindow& render_window;    /// Reference to the render window handle.
    Frontend::EmuWindow* secondary_window; /// Reference to the secondary render window handle.

protected:
    f32 current_fps = 0.0f; /// Current framerate, should be set by the renderer
    s32 current_frame = 0;  /// Current frame, should be set by the renderer
};

} // namespace VideoCore
