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
#include <utility>
#include <vector>

#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/touch.h>

#include "azahar_native.h"
#include "citra_vita/config.h"
#include "citra_vita/crash_report.h"
#include "citra_vita/emu_window_vita.h"
#include "citra_vita/hud_stats.h"
#include "citra_vita/sgxd_present.h"
#include "citra_vita/uvc_stream.h"
#include "citra_vita/vita_ui.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "common/mutex.h"
#include "common/process_clock.h"
#include "common/settings.h"
#include "core/3ds.h"
#include "core/core.h"
#include "core/frontend/framebuffer_layout.h"
#include "video_core/renderer_sgxd/sgxd_backend.h"
#include "video_core/renderer_sgxd/sgxd_flags.h"
#include "video_core/renderer_sgxd/sgxd_rasterizer.h"

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
    sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);
    SceTouchPanelInfo rear{};
    const int rear_info = sceTouchGetPanelInfo(SCE_TOUCH_PORT_BACK, &rear);
    ASSERT_MSG(rear_info >= 0, "the rear panel's info: {:#010x}", static_cast<u32>(rear_info));
    rear_middle = (rear.minAaX + rear.maxAaX) / 2;

    // The display never resizes and never rotates, so the layout is computed once. Touch input is
    // resolved against it, so it has to exist before the first frame.
    UpdateCurrentFramebufferLayout();
}

EmuWindowVita::~EmuWindowVita() {
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_STOP);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_STOP);
}

namespace {
std::atomic<EmuWindowVita::Wide> widescreen{EmuWindowVita::Wide::Off};
std::atomic<u32> overlay_percent{60};
Common::Mutex on_screen_mutex;
std::optional<ControlLayouts::ControlLayout> on_screen_chosen;
std::atomic<u32> on_screen_generation_set{};
/// SetSwapShortcut's: PS + cross until the settings are applied.
std::atomic<u32> swap_shortcut_buttons{SCE_CTRL_CROSS};
std::atomic<bool> swap_shortcut_ps{true};
Common::Mutex chosen_mutex;
ScreenLayouts::Builtin chosen_builtin{ScreenLayouts::Builtin::Default};
std::optional<ScreenLayouts::ScreenLayout> chosen_custom;
} // namespace

void EmuWindowVita::SetWidescreen(Wide mode) {
    widescreen.store(mode, std::memory_order_relaxed);
}

EmuWindowVita::Wide EmuWindowVita::Widescreen() {
    return widescreen.load(std::memory_order_relaxed);
}

void EmuWindowVita::SetOverlayOpacity(u32 percent) {
    overlay_percent.store(std::clamp(percent, 10u, 100u), std::memory_order_relaxed);
}

u32 EmuWindowVita::OverlayOpacity() {
    return overlay_percent.load(std::memory_order_relaxed);
}

void EmuWindowVita::SetLayout(ScreenLayouts::Builtin builtin) {
    std::scoped_lock lk{chosen_mutex};
    chosen_builtin = builtin;
    chosen_custom.reset();
}

void EmuWindowVita::SetLayout(const ScreenLayouts::ScreenLayout& custom) {
    std::scoped_lock lk{chosen_mutex};
    chosen_custom = custom;
}

void EmuWindowVita::PreviewLayout(std::optional<ScreenLayouts::ScreenLayout> layout) {
    preview_layout = std::move(layout);
    UpdateCurrentFramebufferLayout();
}

void EmuWindowVita::UpdateCurrentFramebufferLayout() {
    using ScreenLayouts::Affine;
    using ScreenLayouts::Placement;
    using ScreenLayouts::Screen;
    using ScreenLayouts::ScreenLayout;
    const bool swapped = Settings::values.swap_screen.GetValue();
    ScreenLayout layout;
    if (preview_layout) {
        layout = *preview_layout;
    } else {
        std::scoped_lock lk{chosen_mutex};
        layout = chosen_custom ? (swapped ? ScreenLayouts::Swapped(*chosen_custom) : *chosen_custom)
                               : ScreenLayouts::MakeBuiltin(
                                     chosen_builtin, swapped, Widescreen() != Wide::Off,
                                     static_cast<float>(OverlayOpacity()) / 100.0f);
    }
    if (menu_banner) {
        const Common::Rectangle<u32>& r = *menu_banner;
        layout[Screen::Top] = {Affine::Place(static_cast<float>(r.left), static_cast<float>(r.top),
                                             {static_cast<float>(r.GetWidth()),
                                              static_cast<float>(r.GetHeight())}),
                               true, 1.0f};
        layout[Screen::Bottom].shown = false;
    }
    // With the widescreen hack the renderer draws the picture's margins where the top screen shows
    // wider than the 3DS's 5:3 (the single screen layouts'); stretched, the 5:3 picture fills it.
    const Placement& top = layout[Screen::Top];
    const bool wide = Widescreen() == Wide::Hack && !menu_banner && top.shown &&
                      ScreenLayouts::WiderThan3ds(top.transform);
    SgxdRenderer::RasterizerSgxd::SetWidescreen(wide ? WidescreenMargin : 0);

    Layout::FramebufferLayout record{};
    record.width = DisplayWidth;
    record.height = DisplayHeight;
    for (std::size_t i = 0; i < placed.size(); i++) {
        const Placement& p = layout.screens[i];
        PlacedScreen& s = placed[i];
        s.shown = p.shown;
        s.opacity = std::clamp(p.opacity, 0.0f, 1.0f);
        s.quad = ScreenLayouts::Corners(p.transform);
        const std::array<float, 4> b = ScreenLayouts::Bounds(s.quad);
        const auto pixel = [](float v, u32 limit) {
            return static_cast<u32>(std::clamp(std::lround(v), 0l, static_cast<long>(limit)));
        };
        const Common::Rectangle<u32> bounds{pixel(b[0], DisplayWidth), pixel(b[1], DisplayHeight),
                                            pixel(b[2], DisplayWidth), pixel(b[3], DisplayHeight)};
        (i == 0 ? record.top_screen : record.bottom_screen) = bounds;
        (i == 0 ? record.top_screen_enabled : record.bottom_screen_enabled) = p.shown;
        (i == 0 ? record.top_opacity : record.bottom_opacity) = s.opacity;
    }
    draw_order = layout.front == Screen::Top ? std::array<u32, 2>{1, 0} : std::array<u32, 2>{0, 1};
    // Every touch on the display goes back through the bottom screen's transformation.
    touch_from_display = layout[Screen::Bottom].transform.Inverse();
    NotifyFramebufferLayoutChanged(record);
}

void EmuWindowVita::ShowMenuBanner(std::optional<Common::Rectangle<u32>> rect) {
    menu_banner = rect;
    if (!rect) {
        have_screens = false;
    }
    UpdateCurrentFramebufferLayout();
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
 * Scripted pad input, for driving a title where nobody holds the console:
 * ux0:data/azahar/autoinput.txt, one press per line,
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
    if (layout_update.exchange(false, std::memory_order_acq_rel)) {
        UpdateCurrentFramebufferLayout();
    }
    ReadPad();
    UpdateTouch();
    // The PS button: held, a layer of shortcuts (the Swap screens shortcut's button pressed with
    // it swaps the screens), the game seeing none of the buttons pressed with it (ReadPad's
    // menu_held keeps each from the guest until it is let go, as after the pause menu); let go
    // with no shortcut taken, it opens the pause menu over a running title. No guest binding
    // reads it, so the title never sees it.
    const u32 held = buttons_held.load(std::memory_order_relaxed);
    const u32 pressed = held & ~pad_before;
    pad_before = held;
    const bool ps = (held & SCE_CTRL_PSBUTTON) != 0;
    const u32 shortcut = swap_shortcut_buttons.load(std::memory_order_relaxed);
    const bool shortcut_with_ps = swap_shortcut_ps.load(std::memory_order_relaxed);
    if ((pressed & shortcut) != 0 && (ps || !shortcut_with_ps) &&
        pause_ui.load(std::memory_order_acquire) != nullptr) {
        ToggleSwapScreens();
        ps_shortcut_taken |= ps;
    }
    const bool released = !ps && ps_held;
    ps_held = ps;
    // A scripted open taken at every poll: one asked for as a PS release opens the menu is that
    // menu's, not a second one's after it.
    const bool scripted = script_menu_open.exchange(false, std::memory_order_acq_rel);
    if ((released && !std::exchange(ps_shortcut_taken, false)) || scripted) {
        if (Ui* const ui = pause_ui.load(std::memory_order_acquire)) {
            RunPauseMenu(*ui);
        }
    }
}

void EmuWindowVita::RunPauseMenu(Ui& ui) {
    {
        std::scoped_lock lk{pause_gate};
        paused = true;
        pause_gate.NotifyAll();
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
    // The layout's settings as the menu left them, however it closed (Resume, cancel, a
    // savestate; a scripted close with the layout editor open, whose preview goes with it): the
    // presents from here draw them.
    preview_layout.reset();
    UpdateCurrentFramebufferLayout();
    menu_held.store(buttons_held.load(std::memory_order_relaxed), std::memory_order_relaxed);
    // A PS button held as the menu closes opens nothing when it is let go, and the buttons held
    // now press no shortcut.
    pad_before = buttons_held.load(std::memory_order_relaxed);
    ps_shortcut_taken = ps_held;
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
    if (paused) {
        parked = true;
        pause_gate.NotifyAll();
    }
    while (paused) {
        pause_gate.WaitFor(std::chrono::milliseconds(100));
        // Paused is not stalled (the stall watch).
        CrashReport::StallWatchBeat();
    }
    parked = false;
}

bool EmuWindowVita::PauseForSuspend(std::chrono::milliseconds timeout) {
    if (pause_ui.load(std::memory_order_acquire) == nullptr) {
        return false;
    }
    std::scoped_lock lk{pause_gate};
    // Opened by the next poll (the render thread's present), as ScriptPauseMenu does; one open
    // already stays, and no second one opens after it closes.
    if (!paused) {
        script_menu_open.store(true, std::memory_order_release);
    }
    const auto until = Common::ProcessClock::now() + timeout;
    while (!(paused && parked)) {
        const auto now = Common::ProcessClock::now();
        if (now >= until) {
            return false;
        }
        pause_gate.WaitFor(std::chrono::duration_cast<std::chrono::milliseconds>(until - now));
    }
    return true;
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
        // The rear panel's halves as two more buttons, which the controls give ZL and ZR by
        // default (input_factory_vita): a finger anywhere on a half holds it.
        SceTouchData rear{};
        if (sceTouchPeek(SCE_TOUCH_PORT_BACK, &rear, 1) > 0) {
            for (u32 i = 0; i < rear.reportNum; i++) {
                pad.buttons |= rear.report[i].x < rear_middle ? RearLeftButton : RearRightButton;
            }
        }
        menu_held.fetch_and(pad.buttons, std::memory_order_relaxed);
        // A button held with the PS button is the shortcuts' (PollEvents): kept from the guest
        // from the first sample it shows in, before that sample is published (GuestButtons).
        if ((pad.buttons & SCE_CTRL_PSBUTTON) != 0) {
            menu_held.fetch_or(pad.buttons & ~SCE_CTRL_PSBUTTON, std::memory_order_relaxed);
        }
        buttons_held.store(pad.buttons, std::memory_order_release);
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
        sceKernelPowerTick(keep_display_on ? SCE_KERNEL_POWER_TICK_DEFAULT
                                           : SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
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
    // HDMI and USB audio have no volume of their own, and
    // SceShell's handler steps nothing there either (AZAHAR_ERR_ROUTE).
    const int level = azaharSystemVolume(step);
    if (level < 0) {
        LOG_DEBUG(Frontend, "azaharSystemVolume: {:#010x}", static_cast<u32>(level));
        return;
    }
    HudStats::volume_level.store(static_cast<u32>(level), std::memory_order_relaxed);
    HudStats::volume_until_us.store(now + 2000000u, std::memory_order_release);
    MarkOverlayChanged();
}

void EmuWindowVita::UpdateTouch() {
    if (const u32 generation = on_screen_generation_set.load(std::memory_order_acquire);
        generation != on_screen_generation) {
        std::scoped_lock lk{on_screen_mutex};
        on_screen_generation = generation;
        on_screen = on_screen_chosen;
    }
    u32 held = 0;
    const auto hold = [&](u32 buttons) {
        if (on_screen_held.exchange(buttons, std::memory_order_relaxed) != buttons) {
            MarkOverlayChanged();
        }
    };
    const auto release = [this] {
        if (touch_down) {
            TouchReleased();
            touch_down = touch_on_screen = false;
        }
        touch_finger = -1;
    };
    if (!input_to_guest.load(std::memory_order_relaxed)) {
        release();
        button_finger_count = 0;
        hold(0);
        return;
    }
    // The touch screen's coordinates, 0 to 1 across it, kept just inside: the 3DS's panel reads
    // a finger past its edge at the edge.
    static constexpr float Last = 0.999999f;
    const auto press = [this](float x, float y) {
        TouchPressedAt(std::clamp(x, 0.0f, Last), std::clamp(y, 0.0f, Last));
    };

    if (script_started) {
        if (const auto tap = input_script.Touch(ScriptElapsed())) {
            press(std::clamp(tap->first, 0.0f, 319.0f) / 320.0f,
                  std::clamp(tap->second, 0.0f, 239.0f) / 240.0f);
            touch_down = touch_on_screen = true;
            return;
        }
    }

    SceTouchData touch{};
    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1) <= 0) {
        touch.reportNum = 0;
    }
    // Each finger by its report ID: one that came down on an on-screen button holds the button
    // under it; the first other one is the touch screen's, as the 3DS has one touch point; any
    // more touch nothing.
    std::array<int, 8> fingers_now{};
    std::size_t fingers_now_count = 0;
    std::optional<ScreenLayouts::Point> touch_at;
    const u32 reports = std::min<u32>(touch.reportNum, SCE_TOUCH_MAX_REPORT);
    for (u32 i = 0; i < reports; i++) {
        const int id = touch.report[i].id;
        const ScreenLayouts::Point on_display{
            static_cast<float>(touch.report[i].x) * DisplayWidth / TouchPanelWidth,
            static_cast<float>(touch.report[i].y) * DisplayHeight / TouchPanelHeight};
        const auto button =
            on_screen ? ControlLayouts::Hit(*on_screen, on_display.x, on_display.y) : std::nullopt;
        const bool button_finger =
            std::find(button_fingers.begin(), button_fingers.begin() + button_finger_count, id) !=
            button_fingers.begin() + button_finger_count;
        if (button_finger || (id != touch_finger && button)) {
            if (button) {
                held |= 1u << ControlLayouts::NativeButtonOf(*button);
            }
            if (fingers_now_count < fingers_now.size()) {
                fingers_now[fingers_now_count++] = id;
            }
            continue;
        }
        if (id == touch_finger) {
            touch_at = on_display;
        } else if (touch_finger < 0) {
            touch_finger = id;
            touch_at = on_display;
        }
    }
    button_fingers = fingers_now;
    button_finger_count = fingers_now_count;
    const u32 held_before = on_screen_held.load(std::memory_order_relaxed);
    hold(held);
    // Swap screens as a finger comes down on it (it holds no 3DS button), in a title alone as the
    // PS shortcut: on the game list or the settings an all-games layout's spot would pin the
    // next title's screens unseen.
    if (((held & ~held_before) >> ControlLayouts::SwapScreensBit & 1) != 0 &&
        pause_ui.load(std::memory_order_acquire) != nullptr) {
        ToggleSwapScreens();
    }
    if (!touch_at) {
        release();
        return;
    }

    // The bottom screen's inverse takes the display's point onto the touch screen.
    const ScreenLayouts::Point on_screen_point = touch_from_display.Apply(*touch_at);
    if (!touch_down) {
        touch_down = true;
        touch_on_screen = on_screen_point.x >= 0.0f && on_screen_point.x < 1.0f &&
                          on_screen_point.y >= 0.0f && on_screen_point.y < 1.0f;
    }
    if (touch_on_screen) {
        press(on_screen_point.x, on_screen_point.y);
    }
}

void EmuWindowVita::SetOnScreenButtons(std::optional<ControlLayouts::ControlLayout> layout) {
    std::scoped_lock lk{on_screen_mutex};
    on_screen_chosen = layout && layout->HasOnScreen() ? std::move(layout) : std::nullopt;
    on_screen_generation_set.fetch_add(1, std::memory_order_release);
}

void EmuWindowVita::SetSwapShortcut(u32 buttons, bool with_ps) {
    swap_shortcut_buttons.store(buttons, std::memory_order_relaxed);
    swap_shortcut_ps.store(with_ps, std::memory_order_relaxed);
}

void EmuWindowVita::ToggleSwapScreens() {
    using Config::Option;
    const int swapped = Config::Effective(Option::SwapScreens) != 0 ? 0 : 1;
    Config::Pin(Option::SwapScreens, swapped);
    swap_toggled.store(true, std::memory_order_relaxed);
    UpdateCurrentFramebufferLayout();
    LOG_INFO(Frontend, "swap screens: {}", swapped != 0 ? "on" : "off");
}

void EmuWindowVita::ForgetSwapToggle() {
    if (swap_toggled.exchange(false, std::memory_order_relaxed)) {
        Config::Unpin(Config::Option::SwapScreens);
    }
}

void EmuWindowVita::DrawOnScreenButtons() {
    if (on_screen) {
        ControlLayouts::DrawHud(*on_screen, on_screen_held.load(std::memory_order_relaxed));
    }
}

void EmuWindowVita::SwapBuffers() {
    Present(render_ui.load(std::memory_order_acquire));
}

void EmuWindowVita::PresentGuestScreens(const GuestScreen (&next)[2]) {
    screens[0] = next[0];
    screens[1] = next[1];
    have_screens = true;
    if (menu_banner) {
        return;
    }
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

// Each screen onto its quad of the layout, sampled from guest memory where the guest points the
// LCD. An LCD fill, or a screen no framebuffer was named for, shows the frame's black clear.
void EmuWindowVita::DrawGuestScreens() {
    // The screen in front after the one under it.
    for (const u32 i : draw_order) {
        const GuestScreen& s = screens[i];
        const PlacedScreen& place = placed[i];
#ifdef VITA_DIAGNOSTICS
        std::array<u32, 5>& seen = screens_seen[i];
        const bool flipped = s.address == seen[0] || s.address == seen[1];
        if (!flipped || s.format != seen[2] || s.stride != seen[3] || (s.fill ? 1u : 0u) != seen[4]) {
            seen = {s.address, seen[0], s.format, s.stride, s.fill ? 1u : 0u};
            LOG_INFO(Frontend,
                     "screen {}: framebuffer {:#010x}, format {}, stride {}, {} columns{}, onto "
                     "({:.1f}, {:.1f}) ({:.1f}, {:.1f}) ({:.1f}, {:.1f}) ({:.1f}, {:.1f})",
                     i, s.address, s.format, s.stride, s.columns,
                     s.fill ? fmt::format(", filled {:02x}{:02x}{:02x}", s.fill_r, s.fill_g,
                                          s.fill_b)
                            : std::string{},
                     place.quad.x[0], place.quad.y[0], place.quad.x[1], place.quad.y[1],
                     place.quad.x[2], place.quad.y[2], place.quad.x[3], place.quad.y[3]);
        }
#endif
        if (!s.fill && s.address != 0) {
            // The stream reads the LCD buffer with the CPU: a screen picture's bytes go there
            // first.
            if (UvcStream::Streams(i)) {
                SgxdRenderer::RasterizerSgxd::ScreenToGuest(s.address, s.stride * s.columns);
            }
            UvcStream::Offer(i, s.pixels, s.stride, s.columns, s.format);
        }
        if (!place.shown || s.fill || s.address == 0 || s.pixels == nullptr) {
            continue;
        }
        // The screen's picture, which the last display transfer into the buffer made from its
        // source's picture, while it holds the buffer's bytes; else guest memory's.
        u32 picture = 0, picture_w = 0, picture_h = 0, picture_row = 0, quarters = 4, margin = 0;
        u32 picture_format = 0;
        bool flip = false;
        if (SgxdRenderer::RasterizerSgxd::ScreenPictureOf(s.address, s.columns, picture, picture_w,
                                                         picture_h, picture_row, quarters, margin,
                                                         flip, picture_format) &&
            SgxdPresent::DrawPictureScreen(picture, picture_w, picture_h, picture_format,
                                           picture_row, quarters, margin, flip, s.columns,
                                           place.quad, place.opacity)) {
            continue;
        }
        if (!SgxdPresent::DrawGuestScreen(s.address, s.uncached ? nullptr : s.pixels, s.stride,
                                          s.columns, s.format, place.quad, place.opacity) &&
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

void EmuWindowVita::OpenPauseMenuSoon() {
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

void EmuWindowVita::PresentInterfaceOnly(Ui* ui) {
    SgxdPresent::WaitDrawn();
    if (SgxdPresent::Busy()) {
        return;
    }
    SgxdPresent::BeginFrame(0, 0, 0);
    ui->Draw();
    SgxdPresent::EndFrame();
}

void EmuWindowVita::Present(Ui* ui, bool update) {
    SgxdRenderer::SgxdDrainKernelLog();
#ifdef VITA_DIAGNOSTICS
    auto t = std::chrono::steady_clock::now();
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
