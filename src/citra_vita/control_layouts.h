// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string>

#include "common/common_types.h"

/**
 * The player's own control layouts: each a name, the Vita input every 3DS button answers to,
 * and for any button a round button of its own on the front panel, drawn over the game. A
 * finger that comes down on an on-screen button holds it while it stays on the panel, whichever
 * on-screen button it slides onto; any other finger is the 3DS touch screen's. Kept in
 * ux0:data/azahar/control_layouts.txt.
 */
namespace VitaFrontend::ControlLayouts {

/// The 3DS buttons a layout maps, in the menus' order (Config::Option::MapA's onwards), then the
/// frontend's own on-screen button.
enum class Button : u8 {
    A,
    B,
    X,
    Y,
    Up,
    Down,
    Left,
    Right,
    L,
    R,
    ZL,
    ZR,
    Start,
    Select,
    /// Swaps the screens (EmuWindowVita::ToggleSwapScreens): on screen only, no Vita input.
    SwapScreens,
    Count,
};
/// The 3DS buttons, the ones a layout maps to Vita inputs.
inline constexpr std::size_t ButtonCount = static_cast<std::size_t>(Button::SwapScreens);
/// The buttons a layout can put on the screen: the 3DS's and Swap screens.
inline constexpr std::size_t SpotCount = static_cast<std::size_t>(Button::Count);
/// Swap screens' bit in the on-screen buttons' word, past every NativeButton's.
inline constexpr u32 SwapScreensBit = 31;

/// The button's name in the menus ("A", "ZL", "Start", "Swap screens").
[[nodiscard]] const char* Label(Button button);
/// The text on its on-screen button ("A", "Swap"; the d-pad's are drawn as arrows).
[[nodiscard]] const char* DiscLabel(Button button);
/// Its Settings::NativeButton (SwapScreensBit for Swap screens): the bit the on-screen buttons'
/// word holds it at.
[[nodiscard]] u32 NativeButtonOf(Button button);

/// An on-screen button's limits, its diameter in display pixels.
inline constexpr float MinSize = 40.0f;
inline constexpr float MaxSize = 200.0f;

/// An on-screen button's place: its middle on the 960x544 display and its diameter, in pixels.
struct OnScreen {
    float x, y, size;
    bool operator==(const OnScreen&) const = default;
};

struct ControlLayout {
    std::string name;
    /// By Button: the Vita input it answers to, a Config::ControlInput.
    std::array<int, ButtonCount> inputs{};
    /// By Button: its on-screen button, if it has one (as well as its input).
    std::array<std::optional<OnScreen>, SpotCount> on_screen{};
    /// How solid the on-screen buttons are drawn, 0.1 to 1.
    float opacity = 0.5f;

    [[nodiscard]] bool HasOnScreen() const;
};

/// A layout's longest name in characters (the menus' keyboard types ASCII).
inline constexpr std::size_t MaxNameLength = 20;

/// A new on-screen button's place for `button`: a free spot of its own on the display's sides.
[[nodiscard]] OnScreen DefaultPlace(Button button);
/// The place kept on the display and within the size limits.
[[nodiscard]] OnScreen Limited(OnScreen place);
/// The on-screen button a display point is on, the one drawn last (in front) first.
[[nodiscard]] std::optional<Button> Hit(const ControlLayout& layout, float x, float y);

/// The layouts in the file, read. Once, before the settings are read (Config::Load).
void Load();
[[nodiscard]] std::size_t Count();
[[nodiscard]] std::optional<ControlLayout> Get(std::size_t index);
[[nodiscard]] std::optional<std::size_t> Find(const std::string& name);
/// Writes `layout` as layout `index`, or (nullopt) as a new one after the others, and the file at
/// once. Its index. Config::SaveControlLayout's, which keeps the settings that choose it.
std::size_t Put(std::optional<std::size_t> index, ControlLayout layout);
/// Takes layout `index` out, and writes the file. Config::DeleteControlLayout's.
void Remove(std::size_t index);

/// The glyphs the in-game drawing uses. Once, after ImGui_ImplSgxd_Init (with StatsHud::Init).
void InitHud();
/// The layout's on-screen buttons into the frame being built (SgxdPresent), those `held`
/// (NativeButton bits) drawn pressed. In game, under the stats overlay.
void DrawHud(const ControlLayout& layout, u32 held);

} // namespace VitaFrontend::ControlLayouts
