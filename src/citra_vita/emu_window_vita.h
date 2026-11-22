// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <atomic>

#include "common/common_types.h"
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
 * Every GXM call happens on the emulation thread. A finished frame is presented from
 * SwapBuffers, which the renderer calls at the end of each guest frame.
 *
 * The interface is drawn there too: ImGui's frame is built and rendered inside that same call,
 * and the menu's decision is published for the run loop to read. While a menu is up emulation
 * is paused and no frames arrive, so the run loop presents directly.
 */
class EmuWindowVita final : public Frontend::EmuWindow {
public:
    explicit EmuWindowVita(Core::System& system);
    ~EmuWindowVita() override;

    /// Reads the pad and the touchscreen. Called on the emulation thread once per iteration.
    void PollEvents() override;

    void SwapBuffers() override;

    /// Draws whatever the UI wants and presents. `ui` may be null, in which case the frame
    /// is left empty.
    void Present(Ui* ui);

    /// Hands the interface to SwapBuffers, which builds and draws it from then on. Null
    /// detaches it again.
    void SetUi(Ui* ui) {
        render_ui.store(ui, std::memory_order_release);
    }

    /// The last decision the interface reached inside SwapBuffers, taken by the run loop and
    /// cleared.
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
    void UpdateTouch();

    Core::System& system;

    std::atomic<Ui*> render_ui{nullptr};
    std::atomic<int> pending_ui_action{0};

    std::atomic<u32> buttons_held{};
    std::atomic<s32> stick_x{}, stick_y{};
    std::atomic<bool> input_to_guest{true};
    std::atomic<bool> quit_requested{};
    bool touch_down{};
};

} // namespace VitaFrontend
