// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <chrono>
#include <optional>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <psp2/ctrl.h>
#include <psp2/touch.h>

#include "citra_vita/emu_window_vita.h"
#include "citra_vita/sgxd_present.h"
#include "citra_vita/vita_ui.h"
#include "common/logging/log.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/frontend/framebuffer_layout.h"

namespace VitaFrontend {

namespace {
constexpr u32 DisplayWidth = 960;
constexpr u32 DisplayHeight = 544;

/// The front panel reports in its own resolution, which is twice the display in each axis.
constexpr int TouchPanelWidth = 1920;
constexpr int TouchPanelHeight = 1088;
} // Anonymous namespace

EmuWindowVita::EmuWindowVita(Core::System& system_) : system{system_} {
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);

    // The display never resizes and never rotates, so the layout is computed once. Touch input is
    // resolved against it, so it has to exist before the first frame.
    UpdateCurrentFramebufferLayout();
}

EmuWindowVita::~EmuWindowVita() {
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_STOP);
}

void EmuWindowVita::UpdateCurrentFramebufferLayout() {
    EmuWindow::UpdateCurrentFramebufferLayout(DisplayWidth, DisplayHeight, false);
}

std::pair<float, float> EmuWindowVita::LeftStick() const {
    // The pad reports each axis as a byte around 128. The dead zone is wide because the sticks on
    // this console rest off-centre often enough that a narrow one walks the character.
    constexpr float dead_zone = 0.2f;
    const auto axis = [](s32 raw) {
        const float value = (static_cast<float>(raw) - 128.0f) / 128.0f;
        if (std::abs(value) < dead_zone) {
            return 0.0f;
        }
        const float scaled = (std::abs(value) - dead_zone) / (1.0f - dead_zone);
        return value < 0.0f ? -scaled : scaled;
    };
    // y is reported downwards; the guest's circle pad is upwards.
    return {axis(stick_x.load(std::memory_order_relaxed)),
            -axis(stick_y.load(std::memory_order_relaxed))};
}

namespace {

/**
 * Scripted pad input, for driving a title where nobody holds the console (Vita3K under
 * Xvfb, an unattended console): ux0:data/azahar/autoinput.txt, one press per line,
 * `<seconds since the title started> <button> [hold seconds]` with the Vita's button
 * names (cross circle square triangle start select up down left right l r), or
 * `<seconds> stick <x> <y> [hold]` with x and y in -1..1, or `<seconds> touch <x> <y>
 * [hold]` with x and y in bottom-screen pixels (0..320, 0..240). Pressed buttons are added
 * to the pad's; a scripted stick or touch replaces it. A missing file is the normal case.
 */
struct ScriptedPress {
    double at;
    double until;
    u32 buttons;
    bool stick;
    bool touch;
    u8 x, y;
    float touch_x, touch_y; ///< bottom-screen pixels
};

class InputScript {
public:
    void Load() {
#ifdef VITA_DIAGNOSTICS
        loaded = true;
        FILE* f = std::fopen("ux0:data/azahar/autoinput.txt", "rb");
        if (f == nullptr) {
            return;
        }
        char line[128];
        while (std::fgets(line, sizeof(line), f) != nullptr) {
            char* p = line;
            const double at = std::strtod(p, &p);
            while (*p == ' ') {
                p++;
            }
            char* name = p;
            while (*p != '\0' && *p != ' ' && *p != '\n' && *p != '\r') {
                p++;
            }
            if (name == p) {
                continue;
            }
            const std::string button(name, p);
            ScriptedPress press{at, 0, 0, false, false, 128, 128, 0.0f, 0.0f};
            if (button == "touch") {
                press.touch = true;
                press.touch_x = static_cast<float>(std::strtod(p, &p));
                press.touch_y = static_cast<float>(std::strtod(p, &p));
            } else if (button == "stick") {
                const double x = std::strtod(p, &p);
                const double y = std::strtod(p, &p);
                press.stick = true;
                press.x = static_cast<u8>(std::clamp(128.0 + x * 127.0, 0.0, 255.0));
                press.y = static_cast<u8>(std::clamp(128.0 - y * 127.0, 0.0, 255.0));
            } else {
                press.buttons = ButtonMask(button);
                if (press.buttons == 0) {
                    LOG_WARNING(Frontend, "autoinput.txt: unknown button '{}'", button);
                    continue;
                }
            }
            char* end = p;
            const double hold = std::strtod(p, &end);
            press.until = at + (end != p && hold > 0.0 ? hold : 0.5);
            presses.push_back(press);
        }
        std::fclose(f);
        LOG_INFO(Frontend, "autoinput.txt: {} presses", presses.size());
#endif
    }

    /// Applies the presses due now. `elapsed` is seconds since the title started.
    void Apply(double elapsed, u32& buttons, u8& lx, u8& ly) const {
        for (const ScriptedPress& press : presses) {
            if (elapsed < press.at || elapsed >= press.until) {
                continue;
            }
            buttons |= press.buttons;
            if (press.stick) {
                lx = press.x;
                ly = press.y;
            }
        }
    }

    /// The scripted touch due now, in bottom-screen pixels, if any.
    std::optional<std::pair<float, float>> Touch(double elapsed) const {
        for (const ScriptedPress& press : presses) {
            if (press.touch && elapsed >= press.at && elapsed < press.until) {
                return std::make_pair(press.touch_x, press.touch_y);
            }
        }
        return std::nullopt;
    }

    bool loaded = false;
    bool Loaded() const {
#ifdef VITA_DIAGNOSTICS
        return loaded;
#else
        return false;
#endif
    }
    bool Empty() const {
#ifdef VITA_DIAGNOSTICS
        return presses.empty();
#else
        return true;
#endif
    }

private:
    static u32 ButtonMask(const std::string& name) {
        static constexpr std::pair<const char*, u32> names[] = {
            {"cross", SCE_CTRL_CROSS},   {"circle", SCE_CTRL_CIRCLE},
            {"square", SCE_CTRL_SQUARE}, {"triangle", SCE_CTRL_TRIANGLE},
            {"start", SCE_CTRL_START},   {"select", SCE_CTRL_SELECT},
            {"up", SCE_CTRL_UP},         {"down", SCE_CTRL_DOWN},
            {"left", SCE_CTRL_LEFT},     {"right", SCE_CTRL_RIGHT},
            {"l", SCE_CTRL_LTRIGGER},    {"r", SCE_CTRL_RTRIGGER},
        };
        for (const auto& [n, mask] : names) {
            if (name == n) {
                return mask;
            }
        }
        return 0;
    }

    std::vector<ScriptedPress> presses;
};

InputScript input_script;
std::chrono::steady_clock::time_point script_start;
bool script_started = false;

double ScriptElapsed() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - script_start).count();
}

} // Anonymous namespace

void EmuWindowVita::PollEvents() {
    SceCtrlData pad{};
    if (sceCtrlPeekBufferPositive(0, &pad, 1) > 0) {
        if (!input_script.Loaded()) {
            input_script.Load();
        }
        if (!input_script.Empty() && system.IsPoweredOn()) {
            if (!script_started) {
                script_started = true;
                script_start = std::chrono::steady_clock::now();
            }
            const double elapsed = ScriptElapsed();
            u8 lx = static_cast<u8>(pad.lx);
            u8 ly = static_cast<u8>(pad.ly);
            input_script.Apply(elapsed, pad.buttons, lx, ly);
            pad.lx = lx;
            pad.ly = ly;
        }
        buttons_held.store(pad.buttons, std::memory_order_relaxed);
        stick_x.store(pad.lx, std::memory_order_relaxed);
        stick_y.store(pad.ly, std::memory_order_relaxed);
    }
    UpdateTouch();
}

void EmuWindowVita::UpdateTouch() {
    if (!input_to_guest.load(std::memory_order_relaxed)) {
        if (touch_down) {
            TouchReleased();
            touch_down = false;
        }
        return;
    }

    // A scripted touch stands in for the panel.
    if (script_started) {
        if (const auto tap = input_script.Touch(ScriptElapsed())) {
            const auto& rect = GetFramebufferLayout().bottom_screen;
            const u32 x = rect.left + static_cast<u32>(std::clamp(tap->first, 0.0f, 319.0f) *
                                                       static_cast<float>(rect.GetWidth()) / 320.0f);
            const u32 y = rect.top + static_cast<u32>(std::clamp(tap->second, 0.0f, 239.0f) *
                                                      static_cast<float>(rect.GetHeight()) / 240.0f);
            if (touch_down) {
                TouchMoved(x, y);
            } else {
                TouchPressed(x, y);
                touch_down = true;
            }
            return;
        }
    }

    SceTouchData touch{};
    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) <= 0 || touch.reportNum == 0) {
        if (touch_down) {
            TouchReleased();
            touch_down = false;
        }
        return;
    }

    // The panel's coordinates are its own; the emulator wants pixels on the display, which is
    // what the layout the touch code compares against is expressed in.
    const u32 x = static_cast<u32>(static_cast<int>(touch.report[0].x) * DisplayWidth /
                                  TouchPanelWidth);
    const u32 y = static_cast<u32>(static_cast<int>(touch.report[0].y) * DisplayHeight /
                                  TouchPanelHeight);
    if (touch_down) {
        TouchMoved(x, y);
    } else {
        TouchPressed(x, y);
        touch_down = true;
    }
}

void EmuWindowVita::SwapBuffers() {
    Present(render_ui.load(std::memory_order_acquire));
}

void EmuWindowVita::PresentGuestScreens(const GuestScreen (&next)[2]) {
    screens[0] = next[0];
    screens[1] = next[1];
    have_screens = true;
    Present(render_ui.load(std::memory_order_acquire));
}

void EmuWindowVita::ForgetGuestScreens() {
    have_screens = false;
}

// Each screen into its rectangle of the layout, sampled from guest memory where the guest
// points the LCD. An LCD fill, or a screen no framebuffer
// was named for, shows the frame's black clear.
void EmuWindowVita::DrawGuestScreens() {
    const auto& layout = GetFramebufferLayout();
    const bool enabled[2] = {layout.top_screen_enabled, layout.bottom_screen_enabled};
    const Common::Rectangle<u32> rects[2] = {layout.top_screen, layout.bottom_screen};
    for (u32 i = 0; i < 2; i++) {
        const GuestScreen& s = screens[i];
        const Common::Rectangle<u32>& r = rects[i];
        std::array<u32, 5>& seen = screens_seen[i];
        const bool flipped = s.address == seen[0] || s.address == seen[1];
        if (!flipped || s.format != seen[2] || s.stride != seen[3] || (s.fill ? 1u : 0u) != seen[4]) {
            seen = {s.address, seen[0], s.format, s.stride, s.fill ? 1u : 0u};
            LOG_INFO(Frontend,
                     "screen {}: framebuffer {:#010x}, format {}, stride {}, {} columns{}, into "
                     "{}x{} at ({}, {})",
                     i, s.address, s.format, s.stride, s.columns,
                     s.fill ? fmt::format(", filled {:02x}{:02x}{:02x}", s.fill_r, s.fill_g,
                                          s.fill_b)
                            : std::string{},
                     r.GetWidth(), r.GetHeight(), r.left, r.top);
        }
        if (!enabled[i] || s.fill || s.address == 0 || s.pixels == nullptr) {
            continue;
        }
        if (!SgxdPresent::DrawGuestScreen(s.address, s.pixels, s.stride, s.columns, s.format,
                                          r.left, r.top, r.GetWidth(), r.GetHeight()) &&
            screens_skipped++ % 600 == 0) {
            LOG_WARNING(Frontend,
                        "screen {} not drawn: framebuffer {:#010x}, format {}, stride {} ({} "
                        "times)",
                        i, s.address, s.format, s.stride, screens_skipped);
        }
    }
}

int EmuWindowVita::TakeUiAction() {
    return pending_ui_action.exchange(0, std::memory_order_acq_rel);
}

void EmuWindowVita::Present(Ui* ui) {
    // The decision the interface reaches is published for the run loop.
    if (ui != nullptr && render_ui.load(std::memory_order_acquire) == ui) {
        const auto action = ui->Update();
        if (action != Ui::Action::None) {
            pending_ui_action.store(static_cast<int>(action), std::memory_order_release);
        }
    }
    present_us[0].fetch_add(Since(t), std::memory_order_relaxed);
    SgxdPresent::BeginFrame(0, 0, 0);
    if (have_screens) {
        DrawGuestScreens();
    }
    present_us[1].fetch_add(Since(t), std::memory_order_relaxed);
    // A frame being dumped leaves the menu out.
    if (ui != nullptr && !SgxdPresent::DumpPending()) {
        ui->Draw();
    }
    present_us[2].fetch_add(Since(t), std::memory_order_relaxed);
    SgxdPresent::EndFrame();
    present_us[3].fetch_add(Since(t), std::memory_order_relaxed);
}

} // namespace VitaFrontend
