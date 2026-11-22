// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <utility>

#include <fmt/format.h>

#include "citra_vita/screen_layout.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "common/mutex.h"

namespace VitaFrontend::ScreenLayouts {

Affine Affine::Inverse() const {
    const float det = Determinant();
    ASSERT_MSG(det != 0.0f, "screen layout: a transformation with no inverse ({} {} {} {} {} {})", a,
               b, c, d, e, f);
    // The 2x2 part's inverse, then its translation: the inverse's columns against -(e, f).
    Affine inverse{d / det, -b / det, -c / det, a / det, 0.0f, 0.0f};
    const float32x2_t t = vneg_f32(Column(2));
    inverse.SetColumn(
        2, vmla_lane_f32(vmul_lane_f32(inverse.Column(0), t, 0), inverse.Column(1), t, 1));
    return inverse;
}

Affine Affine::operator*(const Affine& first) const {
    // Each of first's columns through this one: column i is this(first's column i), the
    // translation's with this one's added.
    const float32x2_t c0 = Column(0), c1 = Column(1);
    const float32x2_t f0 = first.Column(0), f1 = first.Column(1), f2 = first.Column(2);
    Affine product;
    product.SetColumn(0, vmla_lane_f32(vmul_lane_f32(c0, f0, 0), c1, f0, 1));
    product.SetColumn(1, vmla_lane_f32(vmul_lane_f32(c0, f1, 0), c1, f1, 1));
    product.SetColumn(2, vmla_lane_f32(vmla_lane_f32(Column(2), c0, f2, 0), c1, f2, 1));
    return product;
}

Affine Affine::Rotate(int quarter_turns) {
    switch (((quarter_turns % 4) + 4) % 4) {
    case 1:
        return {0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 0.0f};
    case 2:
        return {-1.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f};
    case 3:
        return {0.0f, -1.0f, 1.0f, 0.0f, 0.0f, 0.0f};
    default:
        return {};
    }
}

namespace {

Size Scaled(Size size, float k) {
    return {size.w * k, size.h * k};
}

/// An arrangement `size` across scaled as large as the display holds it, in proportion, and
/// centred on it.
Affine FitToDisplay(Size size) {
    const float k = std::min(DisplayWidth / size.w, DisplayHeight / size.h);
    return Affine::Translate((DisplayWidth - k * size.w) * 0.5f,
                             (DisplayHeight - k * size.h) * 0.5f) *
           Affine::Scale(k);
}

/// Where the small screen of a pair goes beside the large one.
enum class Beside {
    /// Under it, the two centred on each other.
    Below,
    /// At its right, level with its top, its middle or its bottom.
    RightTop,
    RightMiddle,
    RightBottom,
};

/// Two screens as the 3DS layouts pair them: `large` at the pair's top left, `small` (no taller
/// than it, at the size it is to have beside it) where `beside` says; then the pair, as one, fitted
/// to the display. The large screen's transformation and the small one's.
std::pair<Affine, Affine> Pair(Size large, Size small, Beside beside) {
    Size pair{};
    Affine large_place, small_place;
    switch (beside) {
    case Beside::Below:
        pair = {std::max(large.w, small.w), large.h + small.h};
        large_place = Affine::Place((pair.w - large.w) * 0.5f, 0.0f, large);
        small_place = Affine::Place((pair.w - small.w) * 0.5f, large.h, small);
        break;
    case Beside::RightTop:
    case Beside::RightMiddle:
    case Beside::RightBottom: {
        pair = {large.w + small.w, large.h};
        const float room = large.h - small.h;
        const float y = beside == Beside::RightTop      ? 0.0f
                        : beside == Beside::RightMiddle ? room * 0.5f
                                                        : room;
        large_place = Affine::Place(0.0f, 0.0f, large);
        small_place = Affine::Place(large.w, y, small);
        break;
    }
    }
    const Affine fit = FitToDisplay(pair);
    return {fit * large_place, fit * small_place};
}

/// A screen alone, fitted to the display.
Affine Alone(Size size) {
    return FitToDisplay(size) * Affine::Place(0.0f, 0.0f, size);
}

} // namespace

ScreenLayout MakeBuiltin(Builtin builtin, bool swapped, bool widescreen, float overlay_opacity) {
    ScreenLayout layout;
    // The pairs' large screen is the top one, or with swap the bottom one.
    Placement& large = layout[swapped ? Screen::Bottom : Screen::Top];
    Placement& small = layout[swapped ? Screen::Top : Screen::Bottom];
    const Size large_size = swapped ? BottomScreenSize : TopScreenSize;
    const Size small_size = swapped ? TopScreenSize : BottomScreenSize;
    switch (builtin) {
    case Builtin::Default:
        // The 3DS's own: the large screen over the other, each at its size.
        std::tie(large.transform, small.transform) = Pair(large_size, small_size, Beside::Below);
        break;
    case Builtin::LargeScreen:
        // The other screen at a quarter of its size, at the large one's right, bottoms level.
        std::tie(large.transform, small.transform) =
            Pair(large_size, Scaled(small_size, 1.0f / 4.0f), Beside::RightBottom);
        break;
    case Builtin::SideBySide:
        std::tie(large.transform, small.transform) =
            Pair(large_size, small_size, Beside::RightMiddle);
        break;
    case Builtin::Hybrid:
        // The other screen beside the large one, smaller (1/1.8 of its size; the top screen beside
        // the bottom one 1/2.25, at its top). The 3DS layouts' third screen, the top one again over
        // the small one, is left out: a present draws two.
        std::tie(large.transform, small.transform) =
            swapped ? Pair(large_size, Scaled(small_size, 1.0f / 2.25f), Beside::RightTop)
                    : Pair(large_size, Scaled(small_size, 1.0f / 1.8f), Beside::RightBottom);
        break;
    case Builtin::SingleScreen:
    case Builtin::Overlay:
    case Builtin::LargeOverlay:
        // The large screen alone, fitted to the display; the other where it would be alone.
        large.transform = Alone(large_size);
        small.transform = Alone(small_size);
        small.shown = false;
        if (widescreen && !swapped) {
            large.transform = Affine::Place(0.0f, 0.0f, {DisplayWidth, DisplayHeight});
        }
        if (builtin != Builtin::SingleScreen) {
            // The other screen at 3/4 of its size (5/4 the large overlay's) in the display's
            // bottom-right corner, see-through, over the large one.
            const Size corner = Scaled(small_size, builtin == Builtin::Overlay ? 0.75f : 1.25f);
            small.transform =
                Affine::Place(DisplayWidth - corner.w, DisplayHeight - corner.h, corner);
            small.shown = true;
            small.opacity = overlay_opacity;
            layout.front = swapped ? Screen::Top : Screen::Bottom;
        }
        break;
    case Builtin::Count:
        UNREACHABLE();
    }
    return layout;
}

ScreenLayout Swapped(ScreenLayout layout) {
    std::swap(layout.screens[0], layout.screens[1]);
    layout.front = layout.front == Screen::Top ? Screen::Bottom : Screen::Top;
    return layout;
}

Quad Corners(const Affine& transform) {
    // The unit square's corners, a lane each: x (0, 1, 0, 1), y (0, 0, 1, 1).
    static constexpr float UnitX[4] = {0.0f, 1.0f, 0.0f, 1.0f};
    static constexpr float UnitY[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    const float32x4_t ux = vld1q_f32(UnitX), uy = vld1q_f32(UnitY);
    Quad quad;
    vst1q_f32(quad.x, vmlaq_n_f32(vmlaq_n_f32(vdupq_n_f32(transform.e), ux, transform.a), uy,
                                  transform.c));
    vst1q_f32(quad.y, vmlaq_n_f32(vmlaq_n_f32(vdupq_n_f32(transform.f), ux, transform.b), uy,
                                  transform.d));
    return quad;
}

std::array<float, 4> Bounds(const Quad& quad) {
    const float32x4_t x = vld1q_f32(quad.x), y = vld1q_f32(quad.y);
    // Pairwise across the four lanes, twice.
    const float32x2_t low = vpmin_f32(vget_low_f32(x), vget_high_f32(x));
    const float32x2_t high = vpmax_f32(vget_low_f32(x), vget_high_f32(x));
    const float32x2_t top = vpmin_f32(vget_low_f32(y), vget_high_f32(y));
    const float32x2_t bottom = vpmax_f32(vget_low_f32(y), vget_high_f32(y));
    return {vget_lane_f32(vpmin_f32(low, low), 0), vget_lane_f32(vpmin_f32(top, top), 0),
            vget_lane_f32(vpmax_f32(high, high), 0), vget_lane_f32(vpmax_f32(bottom, bottom), 0)};
}

bool WiderThan3ds(const Affine& transform) {
    const float width = std::hypot(transform.a, transform.b);
    const float height = std::hypot(transform.c, transform.d);
    // Past rounding: the single screen layout's 906.67x544 is 5:3.
    return width > height * (TopScreenSize.w / TopScreenSize.h) * 1.005f;
}

int TurnDegrees(const Affine& transform) {
    constexpr float Degrees = 180.0f / 3.14159265f;
    const int turn = static_cast<int>(std::lround(std::atan2(transform.b, transform.a) * Degrees));
    return (turn % 360 + 360) % 360;
}

Affine AboutCentre(const Affine& transform, const Affine& step) {
    const Point c = transform.Apply({0.5f, 0.5f});
    return Affine::Translate(c.x, c.y) * step * Affine::Translate(-c.x, -c.y) * transform;
}

Affine CentredAt(const Affine& transform, Point centre) {
    const Point c = transform.Apply({0.5f, 0.5f});
    return Affine::Translate(centre.x - c.x, centre.y - c.y) * transform;
}

Affine Fitted(const Affine& transform, Size size, bool stretch) {
    const std::array<float, 4> b = Bounds(Corners(transform));
    const float sx = size.w / (b[2] - b[0]), sy = size.h / (b[3] - b[1]);
    const float k = std::min(sx, sy);
    return AboutCentre(transform, stretch ? Affine::Scale(sx, sy) : Affine::Scale(k));
}

namespace {

constexpr char LayoutsPath[] = "ux0:data/azahar/layouts.txt";
/// The screens' keys in the file, by Screen.
constexpr const char* ScreenKeys[] = {"top", "bottom"};

Common::Mutex mutex;
std::vector<ScreenLayout> layouts;

std::string Trim(const std::string& text) {
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

/// A transformation as the file writes it, six numbers; nullopt for anything else, or one that
/// leaves the screen less than a pixel.
std::optional<Affine> ParseTransform(const std::string& text) {
    float v[6];
    const char* at = text.c_str();
    for (float& value : v) {
        char* end = nullptr;
        value = std::strtof(at, &end);
        if (end == at || !std::isfinite(value)) {
            return std::nullopt;
        }
        at = end;
    }
    if (!Trim(at).empty()) {
        return std::nullopt;
    }
    const Affine transform{v[0], v[1], v[2], v[3], v[4], v[5]};
    if (std::abs(transform.Determinant()) < 1.0f) {
        return std::nullopt;
    }
    return transform;
}

void WriteLocked() {
    FILE* f = std::fopen(LayoutsPath, "w");
    if (f == nullptr) {
        LOG_ERROR(Frontend, "Could not write {}", LayoutsPath);
        return;
    }
    std::fprintf(f, "# Azahar's screen layouts, each a section named for it. A screen's line is the "
                    "affine\n# transformation from its picture's unit square (x right, y down) to "
                    "the 960x544 display's\n# pixels: a b c d e f, x' = a x + c y + e and y' = b "
                    "x + d y + f.\n");
    for (const ScreenLayout& layout : layouts) {
        std::fprintf(f, "\n[%s]\n", layout.name.c_str());
        for (std::size_t i = 0; i < layout.screens.size(); i++) {
            const Placement& p = layout.screens[i];
            const Affine& t = p.transform;
            std::fprintf(f, "%s", fmt::format("{} = {} {} {} {} {} {}\n{}_shown = {}\n{}_opacity = {}\n",
                                              ScreenKeys[i], t.a, t.b, t.c, t.d, t.e, t.f,
                                              ScreenKeys[i], p.shown ? "on" : "off", ScreenKeys[i],
                                              std::lround(p.opacity * 100.0f))
                                      .c_str());
        }
        std::fprintf(f, "front = %s\n", ScreenKeys[static_cast<std::size_t>(layout.front)]);
    }
    std::fclose(f);
    LOG_INFO(Frontend, "{} screen layouts written to {}", layouts.size(), LayoutsPath);
}

} // namespace

void Load() {
    std::scoped_lock lock{mutex};
    layouts.clear();
    FILE* f = std::fopen(LayoutsPath, "r");
    if (f == nullptr) {
        return;
    }
    // Each section's layout as it is read, and which of its screens a line placed.
    std::optional<ScreenLayout> layout;
    std::array<bool, 2> placed{};
    const auto finish = [&] {
        if (!layout) {
            return;
        }
        if (!placed[0] || !placed[1]) {
            LOG_WARNING(Frontend, "{}: layout {} left out, it does not place both screens",
                        LayoutsPath, layout->name);
        } else if (std::any_of(layouts.begin(), layouts.end(),
                               [&](const ScreenLayout& l) { return l.name == layout->name; })) {
            LOG_WARNING(Frontend, "{}: a second layout named {} left out", LayoutsPath,
                        layout->name);
        } else {
            layouts.push_back(std::move(*layout));
        }
        layout.reset();
    };
    char buffer[512];
    int line_number = 0;
    while (std::fgets(buffer, sizeof(buffer), f) != nullptr) {
        line_number++;
        const std::string line = Trim(buffer);
        if (line.empty() || line[0] == '#') {
            continue;
        }
        if (line.front() == '[' && line.back() == ']') {
            finish();
            layout.emplace();
            layout->name = Trim(line.substr(1, line.size() - 2));
            placed = {};
            continue;
        }
        const std::size_t equals = line.find('=');
        const std::string key = Trim(line.substr(0, equals));
        const std::string value = equals == std::string::npos ? "" : Trim(line.substr(equals + 1));
        bool understood = false;
        if (layout && equals != std::string::npos) {
            for (std::size_t i = 0; i < 2; i++) {
                const std::string screen = ScreenKeys[i];
                Placement& p = layout->screens[i];
                if (key == screen) {
                    if (const std::optional<Affine> t = ParseTransform(value)) {
                        p.transform = *t;
                        placed[i] = understood = true;
                    }
                } else if (key == screen + "_shown" && (value == "on" || value == "off")) {
                    p.shown = value == "on";
                    understood = true;
                } else if (key == screen + "_opacity") {
                    char* end = nullptr;
                    const long percent = std::strtol(value.c_str(), &end, 10);
                    if (end != value.c_str() && *end == '\0' && percent >= 10 && percent <= 100) {
                        p.opacity = static_cast<float>(percent) / 100.0f;
                        understood = true;
                    }
                }
            }
            if (key == "front" && (value == "top" || value == "bottom")) {
                layout->front = value == "top" ? Screen::Top : Screen::Bottom;
                understood = true;
            }
        }
        if (!understood) {
            LOG_WARNING(Frontend, "{} line {} not understood: {}", LayoutsPath, line_number, line);
        }
    }
    finish();
    std::fclose(f);
    LOG_INFO(Frontend, "{} screen layouts read from {}", layouts.size(), LayoutsPath);
}

std::size_t Count() {
    std::scoped_lock lock{mutex};
    return layouts.size();
}

std::optional<ScreenLayout> Get(std::size_t index) {
    std::scoped_lock lock{mutex};
    if (index >= layouts.size()) {
        return std::nullopt;
    }
    return layouts[index];
}

std::optional<std::size_t> Find(const std::string& name) {
    std::scoped_lock lock{mutex};
    for (std::size_t i = 0; i < layouts.size(); i++) {
        if (layouts[i].name == name) {
            return i;
        }
    }
    return std::nullopt;
}

std::vector<std::string> Names() {
    std::scoped_lock lock{mutex};
    std::vector<std::string> names;
    names.reserve(layouts.size());
    for (const ScreenLayout& layout : layouts) {
        names.push_back(layout.name);
    }
    return names;
}

std::size_t Put(std::optional<std::size_t> index, ScreenLayout layout) {
    std::scoped_lock lock{mutex};
    ASSERT_MSG(!layout.name.empty(), "screen layout: a custom layout with no name");
    for (std::size_t i = 0; i < layouts.size(); i++) {
        ASSERT_MSG(i == index || layouts[i].name != layout.name,
                   "screen layout: a second layout named {}", layout.name);
    }
    std::size_t at;
    if (index) {
        ASSERT_MSG(*index < layouts.size(), "screen layout: no custom layout {}", *index);
        layouts[*index] = std::move(layout);
        at = *index;
    } else {
        layouts.push_back(std::move(layout));
        at = layouts.size() - 1;
    }
    WriteLocked();
    return at;
}

void Remove(std::size_t index) {
    std::scoped_lock lock{mutex};
    ASSERT_MSG(index < layouts.size(), "screen layout: no custom layout {}", index);
    layouts.erase(layouts.begin() + static_cast<std::ptrdiff_t>(index));
    WriteLocked();
}

} // namespace VitaFrontend::ScreenLayouts
