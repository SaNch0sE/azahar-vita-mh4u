// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <atomic>
#include <optional>

#include "citra_vita/screen_layout.h"
#include "citra_vita/sgxd_present.h"
#include "common/common_types.h"
#include "common/thread.h"
#include "core/frontend/emu_window.h"

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
    /// The rear panel's left and right halves in the pad word (ReadPad): bits the pad read never
    /// sets, L1 and R1 coming only through SceCtrl's Ext2 reads.
    static constexpr u32 RearLeftButton = 0x00000400;
    static constexpr u32 RearRightButton = 0x00000800;

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
    /// The game list's banner (BannerPlayer): the guest's top screen, which the GPU renders with
    /// no title, shown in `rect` by the menus' own presents: PresentGuestScreens keeps the
    /// screens without presenting, and Present draws them under the interface. No bottom
    /// screen. nullopt: the layout as the settings make it, and no guest screens until a title
    /// gives some.
    void ShowMenuBanner(std::optional<Common::Rectangle<u32>> rect);
    [[nodiscard]] bool MenuBannerShown() const {
        return menu_banner.has_value();
    }
    /// The last present's picture still rendering on the GPU (SgxdPresent::Busy).
    bool PresentBusy() override;
    /// SgxdPresent::FlipPending, FlipDone: the last present on screen once its 3D is done.
    bool FlipPending(u32* fence) override;
    s64 FlipDueIn() override;
    void FlipDone() override;
    /// With vsync, and no speed limit or 100%: one guest frame a display refresh (the
    /// emulation waits for the refresh after the one its frame before had).
    bool PaceFrame() override;

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
    /// The interface alone on black, without the guest's screens: a savestate's progress, while
    /// the screens' memory may be the load's to replace. The caller keeps the render thread from
    /// presenting meanwhile (GPU::RenderHold), or there is none.
    void PresentInterfaceOnly(Ui* ui);

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

    /// A quit (back to the game list) or an exit the command channel asks for, left for
    /// TakeUiAction as the pause menu leaves one: the interface detached first.
    void RequestQuit(bool exit_application);
    /// The command channel: the pause menu opens as the PS button opens it, stays a
    /// second, and closes with `action` (a Ui::Action) as if chosen in it.
    void ScriptPauseMenu(int action);
    /// A suspend's (the power callback's, before the GPU goes back): the pause menu opens over a
    /// running title as the PS button opens it, unless it is open, and this waits until the run
    /// loop is parked at WaitWhilePaused, `timeout` at most. The title then sleeps and wakes in
    /// the state the pause menu leaves it in. False with no title running, or at the timeout.
    bool PauseForSuspend(std::chrono::milliseconds timeout);

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

    /// The pad as the guest sees it: without the buttons still held from closing the pause menu.
    [[nodiscard]] u32 GuestButtons() const {
        return buttons_held.load(std::memory_order_relaxed) &
               ~menu_held.load(std::memory_order_relaxed);
    }

    /// Left stick, each axis in [-1, 1] with y positive upwards.
    [[nodiscard]] std::pair<float, float> LeftStick() const;
    /// Right stick, as LeftStick: the guest's C-stick (the Circle Pad Pro's, the New 3DS's).
    [[nodiscard]] std::pair<float, float> RightStick() const;

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

    /// Applies the screen layout (SetLayout's, with Swap screens and widescreen; the layout editor's
    /// while it previews one; the game list's banner over it): each screen's corners on the
    /// display, which every present draws it between, and the bottom screen's inverse, which takes
    /// every touch onto the touch screen. Once, when the layout changes.
    void UpdateCurrentFramebufferLayout();

    /// The layout the settings choose (Config::Option::ScreenLayout): a built-in one, or a custom
    /// one's transformations. Any thread; the next UpdateCurrentFramebufferLayout applies it.
    static void SetLayout(ScreenLayouts::Builtin builtin);
    static void SetLayout(const ScreenLayouts::ScreenLayout& custom);
    /// The screens placed again at the next poll (PollEvents), for a layout's setting changed on
    /// another thread than the presenting one.
    void RequestLayoutUpdate() {
        layout_update.store(true, std::memory_order_release);
    }
    /// The layout editor's: the screens placed as `layout` places them from now on, as it is (no
    /// swap), until nullopt gives the chosen layout back. Applied at once. The presenting thread's.
    void PreviewLayout(std::optional<ScreenLayouts::ScreenLayout> layout);

    /// Widescreen, the top screen in the single screen layouts: Off at 5:3; Hack fills the
    /// display, the scene drawn wider (RasterizerSgxd::SetWidescreen), as in any custom layout that
    /// places it wider than 5:3; Stretch fills it with the 5:3 picture stretched.
    enum class Wide : u8 { Off, Hack, Stretch };
    static void SetWidescreen(Wide mode);
    [[nodiscard]] static Wide Widescreen();
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
    /// ScriptPauseMenu's: the menu to open at the next poll, and the action it closes with.
    std::atomic<bool> script_menu_open{};
    std::atomic<int> script_menu_action{};
    /// The pause menu is open (under `pause_gate`): the run loop waits on it, `parked` while it
    /// does.
    Common::CoreCondition pause_gate;
    bool paused{};
    bool parked{};

    std::atomic<u32> buttons_held{};
    /// The buttons held when the pause menu closed, each kept from the guest until released: the
    /// press that chose Resume (cross, the 3DS's B) or cancelled (circle, its A) is the menu's.
    std::atomic<u32> menu_held{};
    std::atomic<s32> stick_x{}, stick_y{};
    std::atomic<s32> right_x{128}, right_y{128};
    std::atomic<bool> input_to_guest{true};
    std::atomic<bool> loading{};
    std::atomic<bool> quit_requested{};
    std::atomic<bool> overlay_changed{};
    std::atomic<bool> layout_update{};
    /// A finger on the front panel (the polling thread's), and whether it came down on the bottom
    /// screen, which it then holds wherever it slides: one that comes down off it touches nothing.
    bool touch_down{};
    bool touch_on_screen{};
    /// The rear panel's middle in its own coordinates (its active area's), splitting its halves.
    int rear_middle{};
    /// The volume key held (the polling thread's), and when it steps again.
    u32 volume_keys{};
    u64 volume_repeat_us{};
    u32 pad_reads{};

    GuestScreen screens[2]{};
    bool have_screens{};
    std::optional<Common::Rectangle<u32>> menu_banner;
    std::optional<ScreenLayouts::ScreenLayout> preview_layout;
    /// The layout as UpdateCurrentFramebufferLayout applied it, for the presents and the touches
    /// (the presenting thread's): each screen's corners on the display, by ScreenLayouts::Screen;
    /// the order they are drawn in, the one in front last; the bottom screen's inverse.
    struct PlacedScreen {
        bool shown{};
        float opacity{1.0f};
        SgxdPresent::ScreenQuad quad{};
    };
    std::array<PlacedScreen, 2> placed{};
    std::array<u32, 2> draw_order{0, 1};
    ScreenLayouts::Affine touch_from_display{};
    /// PaceFrame: the display's vblank count the next guest frame waits for, and whether the
    /// cadence runs (a frame paced the last time). 16 bits, as sceDisplayGetVcount counts.
    u16 pace_target{};
    bool pacing{};
    u32 screens_skipped{};
    /// Each screen's framebuffers as last logged (the last two addresses, format, stride,
    /// fill): DrawGuestScreens logs a screen's when it changes, a double buffer's flips aside.
    std::array<std::array<u32, 5>, 2> screens_seen{};
};

} // namespace VitaFrontend
