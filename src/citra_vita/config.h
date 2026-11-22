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

#include "citra_vita/control_layouts.h"
#include "citra_vita/screen_layout.h"
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
    /// The stats overlay over a running title (StatsHud): 1, or 0 for none (the volume still
    /// shows after a volume key). All games' alone; the overlay reads it as it draws.
    StatsOverlay,
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
    /// Upstream's simulate_3ds_gpu_timings: 1 delays GSP's interrupts by the delay generators'
    /// estimates of their work, 0 (upstream's default) raises them at once.
    SimulateGpuTimings,
    /// Upstream's cpu_clock_percentage: the emulated CPU's clock in percent, 5 to 400. Applied
    /// as a title starts: the second emulation thread reads the timers' scale unlocked.
    CpuClock,
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
    /// The control layout (ControlLayouts): 0 the Map* options' mapping, then the player's own
    /// layouts (custom layout N is CustomControlValue(N)), each its own mapping and on-screen
    /// buttons in place of the Map* options'.
    ControlLayout,
    /// The 3DS's accelerometer and gyroscope from the Vita's (VitaMotion): 1, or 0 for none
    /// (both read zero).
    Motion,
    RightStick,
    /// What swaps the screens while a title runs (Shortcut): a button pressed with PS held, a rear
    /// panel half, or nothing.
    SwapShortcut,
    Region,
    /// The 3DS's system language (Service::CFG::SystemLanguage's order: Japanese, English, French,
    /// German, Italian, Spanish, Simplified Chinese, Korean, Dutch, Portuguese, Russian,
    /// Traditional Chinese), written into the config save as a title loads. With the region on
    /// auto, a language the title's region lacks gives that region's first.
    SystemLanguage,
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

/// The built-in screen layouts, ScreenLayout's first values in the menu's order. The custom ones
/// (ScreenLayouts) follow: custom layout N is Layout::Count + N.
using Layout = ScreenLayouts::Builtin;

/// ScreenLayout's value for custom layout `index`, and the custom layout a value is, if it is one.
[[nodiscard]] constexpr int CustomLayoutValue(std::size_t index) {
    return static_cast<int>(Layout::Count) + static_cast<int>(index);
}
[[nodiscard]] constexpr std::optional<std::size_t> CustomLayoutIndex(int value) {
    return value >= static_cast<int>(Layout::Count)
               ? std::optional<std::size_t>{static_cast<std::size_t>(value) -
                                            static_cast<std::size_t>(Layout::Count)}
               : std::nullopt;
}

/// Saves a custom screen layout (ScreenLayouts::Put): as custom layout `index`'s new self, or
/// (nullopt) as a new one. Its index. The settings that choose it apply it again, and the settings
/// file, which names the custom layouts it chooses, is written again.
std::size_t SaveScreenLayout(std::optional<std::size_t> index, ScreenLayouts::ScreenLayout layout);
/// Deletes custom layout `index`: the settings that chose it choose Default, and those that chose
/// one after it follow it down.
void DeleteScreenLayout(std::size_t index);

/// ControlLayout's value for custom control layout `index`, and the one a value is, if any.
[[nodiscard]] constexpr int CustomControlValue(std::size_t index) {
    return 1 + static_cast<int>(index);
}
[[nodiscard]] constexpr std::optional<std::size_t> CustomControlIndex(int value) {
    return value >= 1 ? std::optional<std::size_t>{static_cast<std::size_t>(value) - 1}
                      : std::nullopt;
}

/// Saves a control layout (ControlLayouts::Put) as layout `index`'s new self, or (nullopt) as a
/// new one: its index. The settings that choose it apply it again, and the settings file is
/// written again.
std::size_t SaveControlLayout(std::optional<std::size_t> index,
                              ControlLayouts::ControlLayout layout);
/// Deletes control layout `index`: the settings that chose it go back to the Controls tab's
/// mapping (0), and those that chose one after it follow it down.
void DeleteControlLayout(std::size_t index);

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

/// A shortcut's input (the SwapShortcut option's values): one of the pad's buttons pressed while
/// the PS button is held (ControlInput's order, Circle to Select), a rear panel half pressed alone
/// (it presses what it is mapped to as well), or none. The values are ControlInput's.
enum class Shortcut : int {
    PsCircle,
    PsCross,
    PsTriangle,
    PsSquare,
    PsUp,
    PsDown,
    PsLeft,
    PsRight,
    PsL,
    PsR,
    PsStart,
    PsSelect,
    RearLeft,
    RearRight,
    None,
    Count,
};

/// An input's word in the files (settings.txt's map_ keys, control_layouts.txt), and the input a
/// word names.
[[nodiscard]] const char* InputWord(ControlInput input);
[[nodiscard]] std::optional<ControlInput> InputFromWord(const std::string& word);

/// Whether a title can have its own value. The options for the screens, the speed and the
/// controls are: what suits one title (a top screen it needs whole, a scale its GPU work allows,
/// vsync where its frame rate is uneven, ZL on a button it leaves free) does not suit the next. The
/// sound, the region, the system language, the menus' theme and language and the start-up notice are the player's or
/// the console's, the same whatever runs.
[[nodiscard]] bool PerTitle(Option option);
/// Whether a change takes effect under a running title. The region, the system language and the
/// CPU clock are read as a title starts.
[[nodiscard]] bool Runtime(Option option);
/// The option's range: 0 and 1 for a toggle, 0 and the last index for a choice (the screen layout's
/// last custom layout's).
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
void Unpin(Option option);

/// The 3DS's user name the games read (CFG's username: a greeting, a save's owner), which a title
/// gets as it starts; empty for the console's own (AZAHAR on a new one). At most MaxConsoleName
/// UTF-16 units, the 3DS's limit, cut between characters (a settings file's longer name too); the
/// menus' keyboard types ASCII only and shows how many are left.
constexpr std::size_t MaxConsoleName = 10;
[[nodiscard]] std::string ConsoleName();
void SetConsoleName(const std::string& name);
/// The name the console's config save held before the first ConsoleName written into it, which
/// clearing the setting brings back; empty while none was written.
[[nodiscard]] std::string ConsoleOwnName();
void SetConsoleOwnName(const std::string& name);

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
/// game runs differently with (widescreen, the internal resolution, anti-aliasing, vsync, the
/// speed limit, the frame skip limit, Wait for GPU results, skipping the right eye, the 3DS's
/// GPU timings and its CPU clock), as their file keys separated by commas in `out`, a key marked *
/// where the value is the game's own: a crash's notice asks the player to try the defaults first.
/// Nothing with no title running. Takes no lock and allocates nothing (CrashReport::Recover hands
/// it to the restart). Returns the length written.
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
