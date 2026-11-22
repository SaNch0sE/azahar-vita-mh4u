// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <arm_neon.h>

#include "common/common_types.h"

/**
 * Where the 3DS's two screens go on the Vita's display, as one affine transformation a screen:
 * from the screen's unit square (x right, y down over the picture as the 3DS shows it) onto the
 * 960x544 display, in its pixels. The player's own layouts are kept as the transformations
 * themselves (ux0:data/azahar/layouts.txt).
 *
 * The window applies a layout once, when it changes (EmuWindowVita::UpdateCurrentFramebufferLayout):
 * each screen's four corners on the display, which every present draws its quad between, and the
 * bottom screen's inverse, which takes every touch on the display back into the touch screen's own
 * coordinates. The products run on NEON: a transformation's columns (a, b), (c, d) and (e, f)
 * are each a pair of lanes.
 */
namespace VitaFrontend::ScreenLayouts {

inline constexpr float DisplayWidth = 960.0f;
inline constexpr float DisplayHeight = 544.0f;

struct Point {
    float x, y;
};

struct Size {
    float w, h;
};

inline constexpr Size TopScreenSize{400.0f, 240.0f};
inline constexpr Size BottomScreenSize{320.0f, 240.0f};

/// A screen's four corners on the display, by the unit square's corner: (0, 0), (1, 0), (0, 1)
/// and (1, 1), the upright picture's top-left, top-right, bottom-left and bottom-right.
struct Quad {
    float x[4];
    float y[4];
};

/// x' = a x + c y + e, y' = b x + d y + f (SVG's matrix(a b c d e f)).
struct Affine {
    float a = 1.0f, b = 0.0f, c = 0.0f, d = 1.0f, e = 0.0f, f = 0.0f;

    /// The columns as lane pairs: 0 (a, b), 1 (c, d), 2 (e, f).
    [[nodiscard]] float32x2_t Column(std::size_t i) const {
        return i == 0 ? float32x2_t{a, b} : i == 1 ? float32x2_t{c, d} : float32x2_t{e, f};
    }
    void SetColumn(std::size_t i, float32x2_t column) {
        (i == 0 ? a : i == 1 ? c : e) = vget_lane_f32(column, 0);
        (i == 0 ? b : i == 1 ? d : f) = vget_lane_f32(column, 1);
    }
    [[nodiscard]] Point Apply(Point p) const {
        const float32x2_t xy{p.x, p.y};
        const float32x2_t r = vmla_lane_f32(vmla_lane_f32(Column(2), Column(0), xy, 0), Column(1),
                                            xy, 1);
        return {vget_lane_f32(r, 0), vget_lane_f32(r, 1)};
    }
    [[nodiscard]] float Determinant() const {
        return a * d - b * c;
    }
    /// The transformation back; it must have one (a determinant other than 0).
    [[nodiscard]] Affine Inverse() const;
    /// This one after `first`: (this * first)(p) is this(first(p)).
    [[nodiscard]] Affine operator*(const Affine& first) const;

    [[nodiscard]] static Affine Translate(float x, float y) {
        return {1.0f, 0.0f, 0.0f, 1.0f, x, y};
    }
    [[nodiscard]] static Affine Scale(float sx, float sy) {
        return {sx, 0.0f, 0.0f, sy, 0.0f, 0.0f};
    }
    [[nodiscard]] static Affine Scale(float s) {
        return Scale(s, s);
    }
    /// Quarter turns clockwise on the display (its y runs down), exactly.
    [[nodiscard]] static Affine Rotate(int quarter_turns);
    /// The unit square onto the rectangle at (x, y) of `size`: scaled to its size, then moved.
    [[nodiscard]] static Affine Place(float x, float y, Size size) {
        return Translate(x, y) * Scale(size.w, size.h);
    }
};

enum class Screen : u8 {
    Top,
    Bottom,
};

struct Placement {
    Affine transform;
    /// Drawn. A hidden bottom screen still takes touches where it would be, as the single screen
    /// layout's always has: a game's "touch the screen" goes on without it.
    bool shown = true;
    /// 1 opaque; below, the screen blends over what is under it.
    float opacity = 1.0f;
};

struct ScreenLayout {
    /// A custom layout's name; empty for a built-in one.
    std::string name;
    std::array<Placement, 2> screens{};
    /// The screen drawn over the other where they overlap.
    Screen front = Screen::Bottom;

    [[nodiscard]] Placement& operator[](Screen screen) {
        return screens[static_cast<std::size_t>(screen)];
    }
    [[nodiscard]] const Placement& operator[](Screen screen) const {
        return screens[static_cast<std::size_t>(screen)];
    }
};

/// The built-in layouts, in the menu's order (Config::Layout's values, Config::Option::ScreenLayout's
/// first ones).
enum class Builtin : int {
    Default,
    SingleScreen,
    LargeScreen,
    SideBySide,
    Hybrid,
    /// The single screen with the other one small in a corner, see-through.
    Overlay,
    /// The overlay with the other screen larger: 5/4 of its size, not 3/4.
    LargeOverlay,
    Count,
};

/// A built-in layout, made from its steps. swapped: the bottom screen takes the top screen's part.
/// widescreen: the top screen, where it is the one screen shown, placed over the whole display
/// (the widescreen hack's picture drawn wider to fill it: WiderThan3ds). overlay_opacity: the
/// overlay layouts' small screen's.
[[nodiscard]] ScreenLayout MakeBuiltin(Builtin layout, bool swapped, bool widescreen,
                                       float overlay_opacity);

/// The layout with its screens' places traded (Swap screens over a custom layout).
[[nodiscard]] ScreenLayout Swapped(ScreenLayout layout);

/// The unit square's corners on the display, the four at once.
[[nodiscard]] Quad Corners(const Affine& transform);
/// The smallest display rectangle holding the quad: its left, top, right, bottom.
[[nodiscard]] std::array<float, 4> Bounds(const Quad& quad);
/// The screen's picture's width over its height as placed, against the 3DS's 5:3: wider, so that the
/// widescreen picture's extra columns fill it.
[[nodiscard]] bool WiderThan3ds(const Affine& transform);
/// Its turn on the display, in degrees clockwise (0 to 359).
[[nodiscard]] int TurnDegrees(const Affine& transform);

/// An edit's step about the screen's middle (a scale, a turn): the screen stays where it is.
[[nodiscard]] Affine AboutCentre(const Affine& transform, const Affine& step);
/// The screen moved so that its middle is at `centre`.
[[nodiscard]] Affine CentredAt(const Affine& transform, Point centre);
/// The screen scaled about its middle until its bounds are `size`: stretched to it, or in
/// proportion as large as fits.
[[nodiscard]] Affine Fitted(const Affine& transform, Size size, bool stretch);

/// A custom layout's longest name, in characters (the menus' keyboard types ASCII).
inline constexpr std::size_t MaxNameLength = 20;

/// The custom layouts in the file, read. Once, before the settings are read (Config::Load).
void Load();
[[nodiscard]] std::size_t Count();
/// A copy of custom layout `index`, nullopt past the end.
[[nodiscard]] std::optional<ScreenLayout> Get(std::size_t index);
/// The custom layout named `name`, if one is.
[[nodiscard]] std::optional<std::size_t> Find(const std::string& name);
[[nodiscard]] std::vector<std::string> Names();
/// Writes `layout` as custom layout `index`, or (nullopt) as a new one after the others, and the
/// file at once. Its index. Config::SaveScreenLayout's, which keeps the settings that choose it.
std::size_t Put(std::optional<std::size_t> index, ScreenLayout layout);
/// Takes custom layout `index` out, and writes the file. Config::DeleteScreenLayout's.
void Remove(std::size_t index);

} // namespace VitaFrontend::ScreenLayouts
