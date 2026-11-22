// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/common_types.h"

/**
 * The settings a player chooses, kept on the memory card (ux0:data/azahar/settings.txt) and
 * applied to the emulator: one value for all games, and for the options where titles differ, a
 * title's own value over it.
 *
 * Each option's value is one integer: a toggle's 0 or 1, a choice's index, a slider's value. The
 * file names choices by word rather than index, so that reordering a menu does not change what a
 * file means. What reaches the emulator is the option's effective value: a pin (a launch argument
 * or a scripted launch's command, which say exactly what a run wants and are never written to the
 * file) over the running title's own value over the one for all games.
 *
 * The values are the emulator's settings and the frontend's own switches (the layout's overlay,
 * widescreen, the top screen's scale, the menus' theme and language, the start-up notice); this is
 * the only place that writes them, so that what a menu shows is what runs. Any thread: the pause
 * menu edits on the render thread, the run loop applies a title's values on the emulation thread.
 */
namespace VitaFrontend::Config {

enum class Option : u32 {
    ScreenLayout,
    SwapScreens,
    OverlayOpacity,
    /// The top screen alone on the display (EmuWindowVita::Wide): 0 at 5:3, 1 the widescreen
    /// hack, 2 stretched.
    Widescreen,
    TopScale,
    /// 2x MSAA on the top screen (RasterizerSgxd::SetTopMsaa): 0 off, 1 on.
    Msaa,
    Vsync,
    /// Percent of full speed, 0 for none. Not applied while vsync is on, which holds the
    /// emulation to the display's rate already: a limit kept for a title or all games waits
    /// until vsync is off. A pinned one (a scripted run's `limit`) applies as asked.
    SpeedLimit,
    /// How many frames in a row the late skip may leave a screen out while the GPU lags
    /// (RenderThread::SetMaxScreenSkips): 0 to 4, or NoFrameSkipLimit.
    FrameSkipLimit,
    /// Whether the game waits for queued GPU work before it reads what that work writes
    /// (RenderThread::SetReadbackHolds): 1, or 0 to read what is there at once.
    WaitForGpu,
    /// A stereo title's right eye left undrawn: 1, or 0 to draw both eyes.
    SkipRightEye,
    StretchAudio,
    // The controls: the Vita input each 3DS button answers to (ControlInput's order), the
    // NativeButton order without the debug pad's, Home and Power.
    MapA,
    MapB,
    MapX,
    MapY,
    MapUp,
    MapDown,
    MapLeft,
    MapRight,
    MapL,
    MapR,
    MapZL,
    MapZR,
    MapStart,
    MapSelect,
    RightStick,
    Region,
    /// Every title as on a New 3DS (Settings' is_new_3ds): 1, or 0 for the New 3DS's own titles
    /// alone. Read as a title loads.
    ForceNew3ds,
    /// Their colours: 0 dark, 1 light (the HOME Menu's). The frontend's own; the menus read it
    /// as they draw.
    Theme,
    /// The notice before sgxd takes the GPU at start (ShowGxmNotice): 1, or 0 to take it at once.
    /// The frontend's own, read as the app starts.
    StartupNotice,
    /// One of the 3DS's screens as a USB webcam (UvcStream): 0 off, 1 the top screen, 2 the
    /// bottom. The Vita keeps its own layout.
    UsbVideo,
    /// The menus' language (I18n::Language): 0 English, 1 Simplified Chinese. The frontend's own;
    /// the menus show the next frame in it. Its default is the console's language.
    Language,
    Count,
};

/// The screen layouts' choices, in the menu's order (ScreenLayout's values).
enum class Layout : int {
    Default,
    SingleScreen,
    LargeScreen,
    SideBySide,
    Hybrid,
    /// The single screen with the other one small in a corner (EmuWindowVita::SetOverlay).
    Overlay,
    /// The overlay with the other screen larger: 5/4 of its size, not 3/4.
    LargeOverlay,
    Count,
};

/// FrameSkipLimit's value for no limit.
constexpr int NoFrameSkipLimit = 5;

/// The Vita inputs a 3DS button can answer to (the Map* options' values), in the menu's order.
/// The rear panel's halves are pad bits of their own (EmuWindowVita::ReadPad).
enum class ControlInput : int {
    Circle,
    Cross,
    Triangle,
    Square,
    Up,
    Down,
    Left,
    Right,
    L,
    R,
    Start,
    Select,
    RearLeft,
    RearRight,
    None,
    Count,
};
/// What the right stick drives (the RightStick option's values): the 3DS's C-stick (the New 3DS's,
/// and the Circle Pad Pro's in a game made for it), L and R (the stick left and right past half
/// way: games that turn their camera with them), or the d-pad (its four directions past half way).
enum class RightStickMode : int {
    CStick,
    ShoulderButtons,
    DPad,
    Count,
};


/// Whether a title can have its own value. The options for the screens, the speed and the
/// controls are: what suits one title (a top screen it needs whole, a scale its GPU work allows,
/// vsync where its frame rate is uneven, ZL on a button it leaves free) does not suit the next. The
/// sound, the region, the menus' theme and language and the start-up notice are the player's or
/// the console's, the same whatever runs.
[[nodiscard]] bool PerTitle(Option option);
/// Whether a change takes effect under a running title. The region is read as a title loads.
[[nodiscard]] bool Runtime(Option option);
/// The option's range: 0 and 1 for a toggle, 0 and the last index for a choice.
[[nodiscard]] int Minimum(Option option);
[[nodiscard]] int Maximum(Option option);
[[nodiscard]] int Default(Option option);

/// The file's values, applied. Once, at start, before anything reads a setting.
void Load();
/// Writes the file if anything in it changed since it was read or written.
void Save();

/// The key a title's own values are kept under: its file's name, the same in the game list and
/// at the launch, and each file its own; for a title installed onto the emulated SD card, whose
/// contents are all named by number, its title ID.
[[nodiscard]] std::string TitleKeyOf(const std::string& path);

/// Makes `title`'s own values the effective ones (an empty key: none, the values for all games),
/// and applies them. At a title's launch and after its end.
void SetRunningTitle(const std::string& title);
[[nodiscard]] std::string RunningTitle();

[[nodiscard]] int Global(Option option);
void SetGlobal(Option option, int value);
[[nodiscard]] std::optional<int> TitleValue(const std::string& title, Option option);
/// Gives the title its own value, or with nullopt takes it away (the value for all games again).
void SetTitleValue(const std::string& title, Option option, std::optional<int> value);
[[nodiscard]] bool HasTitleValues(const std::string& title);
void ClearTitleValues(const std::string& title);

/// What runs: the pin, else the running title's own value, else the one for all games.
[[nodiscard]] int Effective(Option option);
/// The value that runs for `title` when it runs, pins left out: its own, else all games'.
[[nodiscard]] int ForTitle(const std::string& title, Option option);

/// Option::TopScale's value as the renderer's scale in quarters (1x, 1.25x, 1.5x, 1.75x, 2x: 4
/// to 8), and a scale (`scale=1.5`) as the option's value.
[[nodiscard]] constexpr u32 TopScaleQuarters(int value) {
    return static_cast<u32>(std::clamp(value, 0, 4)) + 4;
}
[[nodiscard]] constexpr int TopScaleValue(float scale) {
    return static_cast<int>(scale * 4.0f + 0.5f) - 4;
}

/// A launch argument's or a command's value, for the rest of the session. A menu's change to the
/// option takes the pin away: what the player chose last is what runs.
void Pin(Option option, int value);
[[nodiscard]] std::optional<int> PinOf(Option option);

/// The folders the game list takes titles from besides ux0:data/azahar/roms, in the order the
/// player added them: on another memory device (a USB or SD2Vita card's uma0:) or anywhere on one.
/// All games' section keeps them, one `rom_folder` line each.
[[nodiscard]] std::vector<std::string> RomFolders();
/// Adds `folder` (a path as the Vita writes it, "uma0:" or "uma0:/roms"): false where it is
/// ux0:data/azahar/roms or listed already, a slash after the device or at the end aside, and
/// letters' case too, as the memory devices' file systems ignore it.
bool AddRomFolder(const std::string& folder);
void RemoveRomFolder(std::size_t index);

/// The values applied last, each option's file key and word, marked where the running title's own
/// or a pin set it, as "[azahar] settings:" lines in `out` (cut where they do not fit): for a
/// crash report. Takes no lock and allocates nothing. Returns the length written.
std::size_t WriteApplied(char* out, std::size_t size);

/// The running title's applied values that are not their options' defaults, among the options a
/// game runs differently with (widescreen, the top screen's resolution, anti-aliasing, vsync, the
/// speed limit, the frame skip limit and Wait for GPU results), as their file keys separated by
/// commas in `out`: a crash's notice asks the player to try the defaults first. Nothing with no
/// title running. Takes no lock and allocates nothing (CrashReport::Recover hands it to the
/// restart). Returns the length written.
std::size_t WriteChangedFromDefaults(char* out, std::size_t size);

/// An option WriteChangedFromDefaults named; `own` where the value was the game's own.
struct ChangedOption {
    Option option;
    bool own;
};
/// The options WriteChangedFromDefaults's keys name, in their order; a key no option has is left
/// out.
std::vector<ChangedOption> OptionsOfKeys(std::string_view keys);

} // namespace VitaFrontend::Config
