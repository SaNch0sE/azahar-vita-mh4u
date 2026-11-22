// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <atomic>

#include "common/common_types.h"
#include "common/thread.h"
#include "core/frontend/emu_window.h"
#include "video_core/renderer_base.h"

namespace Core {
class System;
}

namespace VitaFrontend {

class Ui;

/**
 * The Vita's window, such as it is: a fixed 960x544 display with no window system above it.
 *
 * While a title runs the window is the sgxd render thread's (SgxdRenderer::RenderThread): each
 * VBlank's present record polls the input (PollEvents) and presents the guest's screens
 * (PresentGuestScreens), and the thread polls on its own between them when it has nothing to
 * run. Before and after a title the emulation thread does both.
 *
 * The interface is the same thread's. The stats overlay is built and drawn in each present. The
 * PS button opens the pause menu (SetPauseMenu): the polling thread runs it to the end, while
 * the run loop waits at WaitWhilePaused, and leaves what only the emulation thread can do
 * (savestates, quitting) for TakeUiAction.
 */
class EmuWindowVita final : public Frontend::EmuWindow {
public:
    explicit EmuWindowVita(Core::System& system);
    ~EmuWindowVita() override;

    /// Reads the pad and the touchscreen, on the thread that presents. A PS button press with
    /// a pause menu set runs the menu here, until it closes.
    void PollEvents() override;

    void SwapBuffers() override;

    /// The renderer's present at each VBlank: keeps the guest's screens
    /// and presents them under the interface.
    void PresentGuestScreens(const GuestScreen (&screens)[2]) override;
    /// The renderer is going, and guest memory with it: presents show no guest screens again
    /// until the next PresentGuestScreens.
    void ForgetGuestScreens() override;
    /// The last present's picture still rendering on the GPU (SgxdPresent::Busy).
    bool PresentBusy() override;
    /// SgxdPresent::FlipPending, FlipDone: the last present on screen once its 3D is done.
    bool FlipPending(u32* fence) override;
    s64 FlipDueIn() override;
    void FlipDone() override;

    /// The stats overlay's numbers changed since the last call (GPU's vblank: a present for the
    /// overlay when the guest's screens are unchanged).
    bool TakeOverlayChanged() override {
        return overlay_changed.exchange(false, std::memory_order_relaxed);
    }
    void MarkOverlayChanged() {
        overlay_changed.store(true, std::memory_order_relaxed);
    }

    /// Draws the guest's screens, if the renderer has sent any, and whatever the UI wants, and
    /// presents. `ui` may be null, in which case only the screens are drawn. The interface
    /// SetUi attached is built first unless `update` is false (the caller built it).
    void Present(Ui* ui, bool update = true);

    /// Hands the interface to SwapBuffers, which builds and draws it from then on. Null
    /// detaches it again.
    void SetUi(Ui* ui) {
        render_ui.store(ui, std::memory_order_release);
    }

    /// While a title runs: the interface the PS button opens the pause menu of, on the thread
    /// that polls. Null before the title shuts down.
    void SetPauseMenu(Ui* ui) {
        pause_ui.store(ui, std::memory_order_release);
    }

    void WaitWhilePaused();

    /// What the pause menu left for the run loop (a Ui::Action: savestates, quitting), taken
    /// and cleared.
    [[nodiscard]] int TakeUiAction();

    /// True once the user has asked to leave.
    [[nodiscard]] bool IsQuitRequested() const {
        return quit_requested;
    }

    void RequestQuit() {
        quit_requested = true;
    }

    /// Pad state as of the last PollEvents, as a mask of SCE_CTRL_* bits.
    [[nodiscard]] u32 ButtonsHeld() const {
        return buttons_held.load(std::memory_order_relaxed);
    }

    /// Left stick, each axis in [-1, 1] with y positive upwards.
    [[nodiscard]] std::pair<float, float> LeftStick() const;

    /// Stops feeding the guest its pad state, for while a menu owns the input.
    void SetInputToGuest(bool enabled) {
        input_to_guest.store(enabled, std::memory_order_relaxed);
    }

    void UpdateCurrentFramebufferLayout();

private:
    void ReadPad();
    void UpdateTouch();
    void DrawGuestScreens();
    /// The pause menu, run on the polling thread until it closes; the run loop waits meanwhile.
    void RunPauseMenu(Ui& ui);

    Core::System& system;

    std::atomic<Ui*> render_ui{nullptr};
    std::atomic<int> pending_ui_action{0};
    std::atomic<Ui*> pause_ui{nullptr};
    /// The PS button as of the last poll, for its press's edge (the polling thread's).
    bool ps_held{};
    /// The pause menu is open (under `pause_gate`): the run loop waits on it.
    Common::CoreCondition pause_gate;
    bool paused{};

    std::atomic<u32> buttons_held{};
    std::atomic<s32> stick_x{}, stick_y{};
    std::atomic<bool> input_to_guest{true};
    std::atomic<bool> quit_requested{};
    std::atomic<bool> overlay_changed{};
    bool touch_down{};

    GuestScreen screens[2]{};
    bool have_screens{};
    u32 screens_skipped{};
    /// Each screen's framebuffers as last logged (the last two addresses, format, stride,
    /// fill): DrawGuestScreens logs a screen's when it changes, a double buffer's flips aside.
    std::array<std::array<u32, 5>, 2> screens_seen{};
};

} // namespace VitaFrontend
