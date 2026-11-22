// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <vector>

#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/touch.h>

#include "azahar_native.h"
#include "citra_vita/emu_window_vita.h"
#include "citra_vita/hud_stats.h"
#include "citra_vita/sgxd_present.h"
#include "citra_vita/vita_ui.h"
#include "common/logging/log.h"
#include "common/settings.h"
#include "core/3ds.h"
#include "core/core.h"
#include "core/frontend/framebuffer_layout.h"
#include "video_core/renderer_gxm/gxm_rasterizer.h"

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

namespace {
std::atomic<EmuWindowVita::Wide> widescreen{EmuWindowVita::Wide::Off};
std::atomic<u32> overlay_quarters{0};
std::atomic<u32> overlay_percent{60};
} // namespace

void EmuWindowVita::SetWidescreen(Wide mode) {
    widescreen.store(mode, std::memory_order_relaxed);
}

EmuWindowVita::Wide EmuWindowVita::Widescreen() {
    return widescreen.load(std::memory_order_relaxed);
}

void EmuWindowVita::SetOverlay(u32 quarters) {
    overlay_quarters.store(quarters, std::memory_order_relaxed);
}

bool EmuWindowVita::Overlay() {
    return overlay_quarters.load(std::memory_order_relaxed) != 0;
}

void EmuWindowVita::SetOverlayOpacity(u32 percent) {
    overlay_percent.store(std::clamp(percent, 10u, 100u), std::memory_order_relaxed);
}

u32 EmuWindowVita::OverlayOpacity() {
    return overlay_percent.load(std::memory_order_relaxed);
}

void EmuWindowVita::UpdateCurrentFramebufferLayout() {
    EmuWindow::UpdateCurrentFramebufferLayout(DisplayWidth, DisplayHeight, false);
    Layout::FramebufferLayout layout = GetFramebufferLayout();
    const bool single =
        Settings::values.layout_option.GetValue() == Settings::LayoutOption::SingleScreen;
    const bool swapped = Settings::values.swap_screen.GetValue();
    // The widescreen top screen fills the display where it is the one screen shown; the
    // renderer draws its margins only then.
    const bool wide = Widescreen() && single && !swapped;
    SgxdRenderer::RasterizerSgxd::SetWidescreen(wide ? WidescreenMargin : 0);
    if (wide) {
        layout.top_screen = {0, 0, DisplayWidth, DisplayHeight};
    }
    if (single && Overlay()) {
        // The other screen at 3/4 of its size (5/4 for the large overlay) in the display's
        // bottom right corner, translucent. Touch follows the bottom screen's rectangle
        // (EmuWindow::IsWithinTouchscreen).
        const float opacity = static_cast<float>(OverlayOpacity()) / 100.0f;
        const u32 quarters = overlay_quarters.load(std::memory_order_relaxed);
        const u32 w = static_cast<u32>(swapped ? Core::kScreenTopWidth : Core::kScreenBottomWidth) *
                      quarters / 4;
        const u32 h =
            static_cast<u32>(swapped ? Core::kScreenTopHeight : Core::kScreenBottomHeight) *
            quarters / 4;
        const Common::Rectangle<u32> rect{DisplayWidth - w, DisplayHeight - h, DisplayWidth,
                                          DisplayHeight};
        if (!swapped) {
            layout.bottom_screen_enabled = true;
            layout.bottom_screen = rect;
            layout.bottom_opacity = opacity;
        } else {
            layout.top_screen_enabled = true;
            layout.top_screen = rect;
            layout.top_opacity = opacity;
        }
    }
    NotifyFramebufferLayoutChanged(layout);
}

namespace {

/// A stick's two bytes as the guest's axes: radially, its direction kept. Within the dead zone
/// it is centred; from there its distance scales to the 3DS's full reach at `full`, and past it
/// stays there.
std::pair<float, float> StickAxes(s32 x, s32 y) {
    // The pad reports each axis as a byte around 128. The dead zone is wide because the sticks on
    // this console rest off-centre often enough that a narrow one walks the character. Not every
    // stick reaches the bytes' ends, and a round gate's diagonal gives each axis ~0.71: scaled to
    // the ends per axis, the guest's pad stops short.
    constexpr float dead_zone = 0.2f;
    constexpr float full = 0.9f;
    // y is reported downwards; the guest's sticks are upwards.
    const float fx = (static_cast<float>(x) - 128.0f) / 128.0f;
    const float fy = (128.0f - static_cast<float>(y)) / 128.0f;
    const float r = std::sqrt(fx * fx + fy * fy);
    if (r < dead_zone) {
        return {0.0f, 0.0f};
    }
    const float scale = std::min((r - dead_zone) / (full - dead_zone), 1.0f) / r;
    return {std::clamp(fx * scale, -1.0f, 1.0f), std::clamp(fy * scale, -1.0f, 1.0f)};
}

} // namespace

std::pair<float, float> EmuWindowVita::LeftStick() const {
    return StickAxes(stick_x.load(std::memory_order_relaxed),
                     stick_y.load(std::memory_order_relaxed));
}

std::pair<float, float> EmuWindowVita::RightStick() const {
    return StickAxes(right_x.load(std::memory_order_relaxed),
                     right_y.load(std::memory_order_relaxed));
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
    ReadPad();
    UpdateTouch();
    // The PS button, edge triggered, opens the pause menu over a running title. No guest
    // binding reads it, so the title never sees the press.
    const bool ps = (buttons_held.load(std::memory_order_relaxed) & SCE_CTRL_PSBUTTON) != 0;
    const bool pressed = ps && !ps_held;
    ps_held = ps;
    if (pressed || script_menu_open.exchange(false, std::memory_order_acq_rel)) {
        if (Ui* const ui = pause_ui.load(std::memory_order_acquire)) {
            RunPauseMenu(*ui);
        }
    }
}

void EmuWindowVita::RunPauseMenu(Ui& ui) {
    {
        std::scoped_lock lk{pause_gate};
        paused = true;
    }
    ui.OpenPauseMenu();
    LOG_INFO(Frontend, "pause menu: open");
    Ui::Action action = Ui::Action::None;
    u32 frames = 0;
    while (action == Ui::Action::None && !IsQuitRequested()) {
        ReadPad();
        UpdateTouch();
        ps_held = (buttons_held.load(std::memory_order_relaxed) & SCE_CTRL_PSBUTTON) != 0;
        // A frame a refresh: the present waits for no vblank, and is dropped while the frame
        // before still renders, so wait for both here instead (each asleep until its event).
        SgxdPresent::WaitDrawn();
        action = ui.Update();
        // A scripted choice (ScriptPauseMenu), a second after the menu opened.
        if (action == Ui::Action::None && ++frames >= 60) {
            if (const int scripted = script_menu_action.exchange(0); scripted != 0) {
                action = static_cast<Ui::Action>(scripted);
            }
        }
        Present(&ui, false);
        sceDisplayWaitVblankStart();
    }
    LOG_INFO(Frontend, "pause menu: closed, action {}", static_cast<int>(action));
    switch (action) {
    case Ui::Action::SaveState:
    case Ui::Action::LoadState:
        ui.EnterGame();
        pending_ui_action.store(static_cast<int>(action), std::memory_order_release);
        break;
    case Ui::Action::QuitGame:
    case Ui::Action::ExitApplication:
        // The run loop shuts the title down: no menu opens again, and the presents until then
        // draw no interface (Update has left the pause menu for the game list).
        pause_ui.store(nullptr, std::memory_order_release);
        render_ui.store(nullptr, std::memory_order_release);
        pending_ui_action.store(static_cast<int>(action), std::memory_order_release);
        break;
    default:
        // Resume: Update has gone back to the game.
        break;
    }
    std::scoped_lock lk{pause_gate};
    paused = false;
    pause_gate.NotifyAll();
}

void EmuWindowVita::WaitWhilePaused() {
    std::scoped_lock lk{pause_gate};
    while (paused) {
        pause_gate.WaitFor(std::chrono::milliseconds(100));
    }
}

void EmuWindowVita::ReadPad() {
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
        right_x.store(pad.rx, std::memory_order_relaxed);
        right_y.store(pad.ry, std::memory_order_relaxed);
        // The volume keys are in no app's pad data while sgxd owns the GPU (out of SceShell's,
        // they are out of every app's too): azaharnative reads them from Syscon. Pad data a
        // debugger injects presses them past SceCtrl's masks, into the pad data only.
        const int volume = azaharVolumeKeys();
        VolumeKeys(pad.buttons | (volume > 0 ? static_cast<u32>(volume) : 0u));
    }
    // A standby takes the GPU from sgxd for the rest of the session (docs/sgxd/takeover.md): no
    // automatic one while the emulator runs, as a video player keeps it off. The timer counts
    // minutes.
    if (++pad_reads % 64 == 0) {
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
    }
}

void EmuWindowVita::VolumeKeys(u32 buttons) {
    constexpr u32 Keys = SCE_CTRL_VOLUP | SCE_CTRL_VOLDOWN;
    const u32 held = buttons & Keys;
    if (held == 0 || held == Keys) {
        volume_keys = 0;
        return;
    }
    // During a load or save the keys do nothing, and one held through its end does nothing
    // until it is let go.
    if (loading.load(std::memory_order_relaxed)) {
        volume_keys = held;
        volume_repeat_us = std::numeric_limits<u64>::max();
        return;
    }
    const u64 now = sceKernelGetProcessTimeWide();
    if (held == volume_keys && now < volume_repeat_us) {
        return;
    }
    volume_repeat_us = now + (held == volume_keys ? 100000u : 400000u);
    volume_keys = held;
    const int step = held == SCE_CTRL_VOLUP ? 1 : -1;
    // The system volume, as SceShell steps it (azaharnative sets it while SceShell sleeps).
    int level = azaharSystemVolume(step);
    if (level == AZAHAR_ERR_ROUTE) {
        constexpr int Steps = 20;
        const int emulator =
            std::clamp(static_cast<int>(std::lround(Settings::values.volume.GetValue() * Steps)) +
                           step,
                       0, Steps);
        Settings::values.volume = static_cast<float>(emulator) / Steps;
        level = static_cast<int>(HudStats::VolumeEmulator | (emulator * 100 / Steps));
    } else if (level < 0) {
        LOG_DEBUG(Frontend, "azaharSystemVolume: {:#010x}", static_cast<u32>(level));
        return;
    }
    HudStats::volume_level.store(static_cast<u32>(level), std::memory_order_relaxed);
    HudStats::volume_until_us.store(now + 2000000u, std::memory_order_release);
    MarkOverlayChanged();
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

bool EmuWindowVita::PresentBusy() {
    return SgxdPresent::Busy();
}

bool EmuWindowVita::FlipPending(u32* fence) {
    return SgxdPresent::FlipPending(fence);
}

s64 EmuWindowVita::FlipDueIn() {
    return SgxdPresent::FlipDueIn();
}

void EmuWindowVita::FlipDone() {
    SgxdPresent::FlipDone();
}

bool EmuWindowVita::PaceFrame() {
    const double limit = Settings::GetFrameLimit();
    if (!Settings::values.use_vsync.GetValue() || (limit != 0.0 && limit != 100.0)) {
        pacing = false;
        return false;
    }
    // A guest frame a display refresh: the frame that just ended waits for the refresh after its
    // predecessor's, so the frames go on screen one a refresh (SgxdPresent sets one a vblank) and a
    // heavy frame borrows from the light ones after it. The emulation is never held more than two
    // refreshes behind: a title under full speed stays that far behind and does not wait, and one
    // that was behind runs fast for two frames at most.
    // SceDisplay's count has 16 bits, so the differences are taken in 16 bits: right while frames
    // come less than 9 minutes apart. A target ahead of the count comes only after a longer pause,
    // and the cadence starts again from now.
    const u16 now = static_cast<u16>(sceDisplayGetVcount());
    const s16 behind = static_cast<s16>(static_cast<u16>(now - pace_target));
    if (!pacing || behind < 0) {
        pace_target = now;
        pacing = true;
    } else if (behind > 2) {
        pace_target = static_cast<u16>(now - 2);
    }
    pace_target = static_cast<u16>(pace_target + 1);
    // The vblanks still to come, counted from one reading: the count read again at once after
    // a wait may not have moved yet, and a second wait would cost a whole refresh.
    for (s16 left = static_cast<s16>(static_cast<u16>(pace_target - now)); left > 0; left--) {
        sceDisplayWaitVblankStart();
    }
    return true;
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
    const float opacity[2] = {std::min(layout.top_opacity, 1.0f),
                              std::min(layout.bottom_opacity, 1.0f)};
    // An overlaid (translucent) screen after the one under it.
    const u32 order[2] = {opacity[0] < 1.0f ? 1u : 0u, opacity[0] < 1.0f ? 0u : 1u};
    for (const u32 i : order) {
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
        // The upscaled or widescreen top screen's picture, when the buffer's transfer copied one.
        u32 picture = 0, picture_w = 0, picture_h = 0, picture_row = 0, quarters = 4, margin = 0;
        bool flip = false;
        if (i == 0 &&
            SgxdRenderer::RasterizerSgxd::TopScreenPicture(s.address, picture, picture_w, picture_h,
                                                         picture_row, quarters, margin, flip) &&
            SgxdPresent::DrawPictureScreen(picture, picture_w, picture_h, picture_row, quarters,
                                           margin, flip, r.left, r.top, r.GetWidth(),
                                           r.GetHeight(), opacity[i])) {
            continue;
        }
        if (!SgxdPresent::DrawGuestScreen(s.address, s.uncached ? nullptr : s.pixels, s.stride,
                                          s.columns, s.format, r.left, r.top, r.GetWidth(),
                                          r.GetHeight(), opacity[i]) &&
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

void EmuWindowVita::ScriptPauseMenu(int action) {
    script_menu_action.store(action, std::memory_order_relaxed);
    script_menu_open.store(true, std::memory_order_release);
}

void EmuWindowVita::RequestQuit(bool exit_application) {
    const auto action = exit_application ? Ui::Action::ExitApplication : Ui::Action::QuitGame;
    pause_ui.store(nullptr, std::memory_order_release);
    render_ui.store(nullptr, std::memory_order_release);
    pending_ui_action.store(static_cast<int>(action), std::memory_order_release);
}

namespace {
#ifdef VITA_DIAGNOSTICS
std::atomic<u64> present_us[4]{};
#endif
u32 presents_busy;

[[maybe_unused]] u64 Since(std::chrono::steady_clock::time_point& t) {
    const auto now = std::chrono::steady_clock::now();
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(now - t).count();
    t = now;
    return static_cast<u64>(us);
}
} // namespace

#ifdef VITA_DIAGNOSTICS
PresentTimes TakePresentTimes() {
    return PresentTimes{present_us[0].exchange(0), present_us[1].exchange(0),
                        present_us[2].exchange(0), present_us[3].exchange(0),
                        SgxdPresent::TakeDisplayWaits()};
}
#endif

void EmuWindowVita::Present(Ui* ui, bool update) {
    const auto mark = [&t](u32 part) {
        present_us[part].fetch_add(Since(t), std::memory_order_relaxed);
    };
#else
    const auto mark = [](u32) {};
#endif
    // The attached interface over a running title is the stats overlay, which asks for
    // nothing: the pause menu runs in RunPauseMenu.
    if (update && ui != nullptr && render_ui.load(std::memory_order_acquire) == ui) {
        [[maybe_unused]] const auto action = ui->Update();
    }
    mark(0);
    // The frame before still rendering: this one is dropped, nothing waits for the GPU.
    if (SgxdPresent::Busy()) {
        if (presents_busy++ % 600 == 0) {
            LOG_INFO(Frontend, "present dropped, the last frame still rendering ({} times)",
                     presents_busy);
        }
        return;
    }
    SgxdPresent::BeginFrame(0, 0, 0);
    if (have_screens) {
        DrawGuestScreens();
    }
    mark(1);
    // A frame being dumped leaves the menu out.
    if (ui != nullptr && !SgxdPresent::DumpPending()) {
        ui->Draw();
    }
    mark(2);
    SgxdPresent::EndFrame();
    mark(3);
}

} // namespace VitaFrontend
