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
#ifdef VITA_DIAGNOSTICS
/// The present's time since the last call, in µs, by part (diagnostics: `perflog`).
struct PresentTimes {
    u64 ui_update, screens, ui_draw, end_frame;
    u32 display_waits;
};
PresentTimes TakePresentTimes();
#endif

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

    /// The stats overlay's numbers or the volume changed since the last call (GPU's vblank:
    /// a present for the overlay when the guest's screens are unchanged).
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
    /// A title's or a savestate's load or save is running: the volume keys do nothing
    /// (VolumeKeys). The render thread polls the pad from the renderer's start, partway through
    /// a title's load.
    void SetLoading(bool on) {
        loading.store(on, std::memory_order_relaxed);
    }

    /// The shared layout for the display, with the Vita's own options over it: the widescreen
    /// top screen filling the display in the single screen layout, and the overlay layout's
    /// other screen at the bottom, translucent (SetOverlay).
    void UpdateCurrentFramebufferLayout();

    /// Widescreen: the top screen drawn wider than the 3DS's (RasterizerSgxd::SetWidescreen),
    /// filling the display in the single screen layouts; elsewhere it shows at 5:3 as before.
    static void SetWidescreen(bool on);
    [[nodiscard]] static bool Widescreen();
    /// The overlay layouts: the single screen layout with the other screen at quarters/4 of its
    /// size in the display's bottom right corner (3, or 5 for the large overlay; 0 for none),
    /// opacity percent opaque, and touchable there.
    static void SetOverlay(u32 quarters);
    [[nodiscard]] static bool Overlay();
    static void SetOverlayOpacity(u32 percent);
    [[nodiscard]] static u32 OverlayOpacity();
    /// The widescreen picture's extra rows on each side of the top screen's 400: the display's
    /// 960x544 at the top screen's 240 rows is 423.5 wide.
    static constexpr u32 WidescreenMargin = 12;

private:
    void ReadPad();
    /// The volume keys, this app's while sgxd owns the GPU (azaharnative keeps them from
    /// SceShell): each press, and each 0.1 s held past 0.4 s, steps the system volume
    /// (azaharSystemVolume). HDMI and USB audio have none: there they step nothing.
    void VolumeKeys(u32 buttons);
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
    std::atomic<bool> loading{};
    std::atomic<bool> quit_requested{};
    std::atomic<bool> overlay_changed{};
    bool touch_down{};
    /// The volume key held (the polling thread's), and when it steps again.
    u32 volume_keys{};
    u64 volume_repeat_us{};
    u32 pad_reads{};

    GuestScreen screens[2]{};
    bool have_screens{};
    u32 screens_skipped{};
    /// Each screen's framebuffers as last logged (the last two addresses, format, stride,
    /// fill): DrawGuestScreens logs a screen's when it changes, a double buffer's flips aside.
    std::array<std::array<u32, 5>, 2> screens_seen{};
};

} // namespace VitaFrontend
