// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>

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
 * widescreen, the top screen's scale); this is the only place that writes them, so that what a
 * menu shows is what runs. Any thread: the pause menu edits on the render thread, the run loop
 * applies a title's values on the emulation thread.
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
    Vsync,
    /// Percent of full speed, 0 for none. Not applied while vsync is on, which holds the
    /// emulation to the display's rate already: a limit kept for a title or all games waits
    /// until vsync is off. A pinned one (a scripted run's `limit`) applies as asked.
    SpeedLimit,
    /// How many frames in a row the late skip may leave a screen out while the GPU lags
    /// (RenderThread::SetMaxScreenSkips): 0 to 4, or NoFrameSkipLimit.
    FrameSkipLimit,
    /// A stereo title's right eye left undrawn: 1, or 0 to draw both eyes.
    SkipRightEye,
    StretchAudio,
    RightStick,
    Region,
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

/// Whether a title can have its own value. The options for the screens and the speed are: what
/// suits one title (a top screen it needs whole, a scale its GPU work allows, vsync where its
/// frame rate is uneven) does not suit the next. The sound and the region are the player's or
/// the console's, the same whatever runs.
[[nodiscard]] bool PerTitle(Option option);
/// Whether a change takes effect under a running title. The region is read as a title loads.
[[nodiscard]] bool Runtime(Option option);
/// The option's range: 0 and 1 for a toggle, 0 and the last index for a choice.
[[nodiscard]] int Minimum(Option option);
[[nodiscard]] int Maximum(Option option);

/// The file's values, applied. Once, at start, before anything reads a setting.
void Load();
/// Writes the file if anything in it changed since it was read or written.
void Save();

/// The key a title's own values are kept under: its file's name, the same in the game list and
/// at the launch, and each file its own.
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

/// The values applied last, each option's file key and word, marked where the running title's own
/// or a pin set it, as "[azahar] settings:" lines in `out` (cut where they do not fit): for a
/// crash report. Takes no lock and allocates nothing. Returns the length written.
std::size_t WriteApplied(char* out, std::size_t size);

} // namespace VitaFrontend::Config
