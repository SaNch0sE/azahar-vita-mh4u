// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <vector>

#include <fmt/format.h>
#include <imgui_vita2d/imgui.h>

#include "citra_vita/config.h"
#include "citra_vita/control_layouts.h"
#include "citra_vita/sgxd_present.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "common/settings.h"
#include "common/thread.h"
#include "sgxd_recipe.h"

namespace VitaFrontend::ControlLayouts {

namespace {

constexpr float DisplayWidth = 960.0f;
constexpr float DisplayHeight = 544.0f;

/// By Button: its label, the text on its on-screen button, its key in the file, its bit in the
/// on-screen buttons' word (a NativeButton's).
constexpr struct {
    const char* label;
    const char* disc;
    const char* key;
    u32 bit;
} Buttons[SpotCount] = {
    {"A", "A", "a", Settings::NativeButton::A},
    {"B", "B", "b", Settings::NativeButton::B},
    {"X", "X", "x", Settings::NativeButton::X},
    {"Y", "Y", "y", Settings::NativeButton::Y},
    {"Up", "Up", "up", Settings::NativeButton::Up},
    {"Down", "Down", "down", Settings::NativeButton::Down},
    {"Left", "Left", "left", Settings::NativeButton::Left},
    {"Right", "Right", "right", Settings::NativeButton::Right},
    {"L", "L", "l", Settings::NativeButton::L},
    {"R", "R", "r", Settings::NativeButton::R},
    {"ZL", "ZL", "zl", Settings::NativeButton::ZL},
    {"ZR", "ZR", "zr", Settings::NativeButton::ZR},
    {"Start", "Start", "start", Settings::NativeButton::Start},
    {"Select", "Select", "select", Settings::NativeButton::Select},
    {"Swap screens", "Swap", "swap_screens", SwapScreensBit},
};

/// By Button: a new on-screen button's place. The shoulders high on the sides (ZR clear of the
/// stats overlay's box in the top right corner), the d-pad's and the face buttons' crosses low on
/// the left and the right, Select and Start under the middle, Swap screens in the top left corner.
constexpr OnScreen Defaults[SpotCount] = {
    {910.0f, 420.0f, 64.0f}, // A
    {850.0f, 480.0f, 64.0f}, // B
    {850.0f, 360.0f, 64.0f}, // X
    {790.0f, 420.0f, 64.0f}, // Y
    {110.0f, 360.0f, 64.0f}, // Up
    {110.0f, 480.0f, 64.0f}, // Down
    {50.0f, 420.0f, 64.0f},  // Left
    {170.0f, 420.0f, 64.0f}, // Right
    {60.0f, 260.0f, 88.0f},  // L
    {900.0f, 260.0f, 88.0f}, // R
    {60.0f, 150.0f, 88.0f},  // ZL
    {900.0f, 150.0f, 88.0f}, // ZR
    {540.0f, 514.0f, 56.0f}, // Start
    {420.0f, 514.0f, 56.0f}, // Select
    {40.0f, 40.0f, 64.0f},   // Swap screens
};

constexpr char LayoutsPath[] = "ux0:data/azahar/control_layouts.txt";

Common::Mutex mutex;
std::vector<ControlLayout> layouts;

std::string Trim(const std::string& text) {
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

/// Three numbers, the file's on-screen place; nullopt for anything else or a place off the
/// display or out of the size limits.
std::optional<OnScreen> ParsePlace(const std::string& text) {
    float v[3];
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
    const OnScreen place{v[0], v[1], v[2]};
    const OnScreen kept = Limited(place);
    if (std::abs(kept.x - place.x) > 0.5f || std::abs(kept.y - place.y) > 0.5f ||
        std::abs(kept.size - place.size) > 0.5f) {
        return std::nullopt;
    }
    return kept;
}

void WriteLocked() {
    FILE* f = std::fopen(LayoutsPath, "w");
    if (f == nullptr) {
        LOG_ERROR(Frontend, "Could not write {}", LayoutsPath);
        return;
    }
    std::fprintf(f, "# Azahar's control layouts, each a section named for it. A button's line is "
                    "the Vita input it\n# answers to; its _on_screen line, if it has one, its "
                    "on-screen button: its middle's x and y on\n# the 960x544 display and its "
                    "diameter, in pixels. opacity: the on-screen buttons', in percent.\n");
    for (const ControlLayout& layout : layouts) {
        std::fprintf(f, "\n[%s]\nopacity = %ld\n", layout.name.c_str(),
                     std::lround(layout.opacity * 100.0f));
        for (std::size_t i = 0; i < SpotCount; i++) {
            if (i < ButtonCount) {
                std::fprintf(
                    f, "%s = %s\n", Buttons[i].key,
                    Config::InputWord(static_cast<Config::ControlInput>(layout.inputs[i])));
            }
            if (const auto& p = layout.on_screen[i]) {
                std::fprintf(f, "%s",
                             fmt::format("{}_on_screen = {} {} {}\n", Buttons[i].key,
                                         std::lround(p->x), std::lround(p->y),
                                         std::lround(p->size))
                                 .c_str());
            }
        }
    }
    std::fclose(f);
    LOG_INFO(Frontend, "{} control layouts written to {}", layouts.size(), LayoutsPath);
}

/// The in-game drawing's glyphs, from the atlas's first font as the stats overlay takes them.
struct Glyph {
    float x0, y0, x1, y1;
    float u0, v0, u1, v1;
    float advance;
    bool visible;
};
std::array<Glyph, 128> glyphs{};
float white_u, white_v;
float glyph_height;

/// The last frame's vertices and indices (relative to its first vertex), made again when the
/// layout or the held buttons change.
std::vector<u32> hud_vertices;
std::vector<u16> hud_indices;
std::string hud_for_name;
std::array<std::optional<OnScreen>, SpotCount> hud_for_places{};
float hud_for_opacity = -1.0f;
u32 hud_for_held = ~0u;

u16 PutVertex(float x, float y, float u, float v, u32 colour) {
    const u16 index = static_cast<u16>(hud_vertices.size() / SgxdPresent::VertexWords);
    u32 words[SgxdPresent::VertexWords];
    const float xyuv[4] = {x, y, u, v};
    std::memcpy(words, xyuv, sizeof(xyuv));
    words[4] = colour;
    hud_vertices.insert(hud_vertices.end(), std::begin(words), std::end(words));
    return index;
}

void PutTriangle(u16 a, u16 b, u16 c) {
    hud_indices.insert(hud_indices.end(), {a, b, c});
}

/// A disc of `colour` and a ring of `ring` around it, `segments` steps round.
void PutDisc(float cx, float cy, float radius, u32 colour, u32 ring) {
    constexpr int Segments = 32;
    constexpr float Ring = 3.0f;
    const u16 centre = PutVertex(cx, cy, white_u, white_v, colour);
    u16 first_rim = 0, first_inner = 0, first_outer = 0;
    for (int i = 0; i < Segments; i++) {
        const float a = 6.2831853f * static_cast<float>(i) / Segments;
        const float c = std::cos(a), s = std::sin(a);
        const u16 rim = PutVertex(cx + c * (radius - Ring), cy + s * (radius - Ring), white_u,
                                  white_v, colour);
        const u16 inner = PutVertex(cx + c * (radius - Ring), cy + s * (radius - Ring), white_u,
                                    white_v, ring);
        const u16 outer = PutVertex(cx + c * radius, cy + s * radius, white_u, white_v, ring);
        if (i == 0) {
            first_rim = rim, first_inner = inner, first_outer = outer;
        } else {
            PutTriangle(centre, static_cast<u16>(rim - 3), rim);
            PutTriangle(static_cast<u16>(inner - 3), static_cast<u16>(outer - 3), outer);
            PutTriangle(static_cast<u16>(inner - 3), outer, inner);
        }
        if (i == Segments - 1) {
            PutTriangle(centre, rim, first_rim);
            PutTriangle(inner, outer, first_outer);
            PutTriangle(inner, first_outer, first_inner);
        }
    }
}

void PutText(float cx, float cy, float scale, const char* text, u32 colour) {
    float width = 0.0f;
    for (const char* c = text; *c != '\0'; c++) {
        width += glyphs[static_cast<u8>(*c) & 0x7f].advance * scale;
    }
    float x = cx - width * 0.5f;
    const float y = cy - glyph_height * scale * 0.5f;
    for (const char* c = text; *c != '\0'; c++) {
        const Glyph& g = glyphs[static_cast<u8>(*c) & 0x7f];
        DEBUG_ASSERT_MSG(g.advance != 0.0f, "on-screen buttons: no glyph for {:?}", *c);
        if (g.visible) {
            const u16 v0 = PutVertex(x + g.x0 * scale, y + g.y0 * scale, g.u0, g.v0, colour);
            const u16 v1 = PutVertex(x + g.x1 * scale, y + g.y0 * scale, g.u1, g.v0, colour);
            const u16 v2 = PutVertex(x + g.x1 * scale, y + g.y1 * scale, g.u1, g.v1, colour);
            const u16 v3 = PutVertex(x + g.x0 * scale, y + g.y1 * scale, g.u0, g.v1, colour);
            PutTriangle(v0, v1, v2);
            PutTriangle(v0, v2, v3);
        }
        x += g.advance * scale;
    }
}

/// The d-pad's arrow, pointing (dx, dy).
void PutArrow(float cx, float cy, float radius, float dx, float dy, u32 colour) {
    const float r = radius * 0.42f;
    // The tip ahead, the base's two corners behind it on either side.
    const u16 tip = PutVertex(cx + dx * r, cy + dy * r, white_u, white_v, colour);
    const u16 a = PutVertex(cx - dx * r * 0.6f - dy * r, cy - dy * r * 0.6f + dx * r, white_u,
                            white_v, colour);
    const u16 b = PutVertex(cx - dx * r * 0.6f + dy * r, cy - dy * r * 0.6f - dx * r, white_u,
                            white_v, colour);
    PutTriangle(tip, a, b);
}

u32 Colour(u8 r, u8 g, u8 b, float alpha) {
    return IM_COL32(r, g, b, static_cast<u8>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f));
}

void MakeHud(const ControlLayout& layout, u32 held) {
    hud_vertices.clear();
    hud_indices.clear();
    const float alpha = layout.opacity;
    for (std::size_t i = 0; i < SpotCount; i++) {
        const auto& p = layout.on_screen[i];
        if (!p) {
            continue;
        }
        const bool down = (held >> NativeButtonOf(static_cast<Button>(i)) & 1) != 0;
        const float radius = p->size * 0.5f;
        PutDisc(p->x, p->y, radius,
                down ? Colour(255, 255, 255, alpha * 0.7f) : Colour(20, 20, 20, alpha * 0.55f),
                Colour(255, 255, 255, alpha * (down ? 1.0f : 0.8f)));
        const u32 ink = down ? Colour(20, 20, 20, std::max(alpha, 0.6f))
                             : Colour(255, 255, 255, std::max(alpha, 0.6f));
        const auto button = static_cast<Button>(i);
        switch (button) {
        case Button::Up:
            PutArrow(p->x, p->y, radius, 0.0f, -1.0f, ink);
            break;
        case Button::Down:
            PutArrow(p->x, p->y, radius, 0.0f, 1.0f, ink);
            break;
        case Button::Left:
            PutArrow(p->x, p->y, radius, -1.0f, 0.0f, ink);
            break;
        case Button::Right:
            PutArrow(p->x, p->y, radius, 1.0f, 0.0f, ink);
            break;
        default: {
            // The label as large as fits across the button, at most twice the font's size.
            const char* const label = Buttons[i].disc;
            float width = 0.0f;
            for (const char* c = label; *c != '\0'; c++) {
                width += glyphs[static_cast<u8>(*c) & 0x7f].advance;
            }
            const float scale = std::min({2.0f, p->size * 0.7f / width,
                                          p->size * 0.45f / glyph_height});
            PutText(p->x, p->y, scale, label, ink);
            break;
        }
        }
    }
}

} // namespace

const char* Label(Button button) {
    return Buttons[static_cast<std::size_t>(button)].label;
}

const char* DiscLabel(Button button) {
    return Buttons[static_cast<std::size_t>(button)].disc;
}

u32 NativeButtonOf(Button button) {
    return Buttons[static_cast<std::size_t>(button)].bit;
}

bool ControlLayout::HasOnScreen() const {
    return std::any_of(on_screen.begin(), on_screen.end(),
                       [](const std::optional<OnScreen>& p) { return p.has_value(); });
}

OnScreen DefaultPlace(Button button) {
    return Defaults[static_cast<std::size_t>(button)];
}

OnScreen Limited(OnScreen place) {
    place.size = std::clamp(place.size, MinSize, MaxSize);
    const float r = place.size * 0.5f;
    place.x = std::clamp(place.x, r, DisplayWidth - r);
    place.y = std::clamp(place.y, r, DisplayHeight - r);
    return place;
}

std::optional<Button> Hit(const ControlLayout& layout, float x, float y) {
    for (std::size_t i = SpotCount; i-- > 0;) {
        if (const auto& p = layout.on_screen[i]) {
            const float r = p->size * 0.5f;
            if ((x - p->x) * (x - p->x) + (y - p->y) * (y - p->y) <= r * r) {
                return static_cast<Button>(i);
            }
        }
    }
    return std::nullopt;
}

void Load() {
    std::scoped_lock lock{mutex};
    layouts.clear();
    FILE* f = std::fopen(LayoutsPath, "r");
    if (f == nullptr) {
        return;
    }
    // Each section's layout as it is read, and which buttons a line mapped.
    std::optional<ControlLayout> layout;
    std::array<bool, ButtonCount> mapped{};
    const auto finish = [&] {
        if (!layout) {
            return;
        }
        if (!std::all_of(mapped.begin(), mapped.end(), [](bool m) { return m; })) {
            LOG_WARNING(Frontend, "{}: layout {} left out, it does not map every button",
                        LayoutsPath, layout->name);
        } else if (std::any_of(layouts.begin(), layouts.end(),
                               [&](const ControlLayout& l) { return l.name == layout->name; })) {
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
            mapped = {};
            continue;
        }
        const std::size_t equals = line.find('=');
        const std::string key = Trim(line.substr(0, equals));
        const std::string value = equals == std::string::npos ? "" : Trim(line.substr(equals + 1));
        bool understood = false;
        if (layout && equals != std::string::npos) {
            for (std::size_t i = 0; i < SpotCount; i++) {
                const std::string button = Buttons[i].key;
                if (key == button && i < ButtonCount) {
                    if (const auto input = Config::InputFromWord(value)) {
                        layout->inputs[i] = static_cast<int>(*input);
                        mapped[i] = understood = true;
                    }
                } else if (key == button + "_on_screen") {
                    if (const auto place = ParsePlace(value)) {
                        layout->on_screen[i] = *place;
                        understood = true;
                    }
                }
            }
            if (key == "opacity") {
                char* end = nullptr;
                const long percent = std::strtol(value.c_str(), &end, 10);
                if (end != value.c_str() && *end == '\0' && percent >= 10 && percent <= 100) {
                    layout->opacity = static_cast<float>(percent) / 100.0f;
                    understood = true;
                }
            }
        }
        if (!understood) {
            LOG_WARNING(Frontend, "{} line {} not understood: {}", LayoutsPath, line_number, line);
        }
    }
    finish();
    std::fclose(f);
    LOG_INFO(Frontend, "{} control layouts read from {}", layouts.size(), LayoutsPath);
}

std::size_t Count() {
    std::scoped_lock lock{mutex};
    return layouts.size();
}

std::optional<ControlLayout> Get(std::size_t index) {
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

std::size_t Put(std::optional<std::size_t> index, ControlLayout layout) {
    std::scoped_lock lock{mutex};
    ASSERT_MSG(!layout.name.empty(), "control layout: a layout with no name");
    for (std::size_t i = 0; i < layouts.size(); i++) {
        ASSERT_MSG(i == index || layouts[i].name != layout.name,
                   "control layout: a second layout named {}", layout.name);
    }
    std::size_t at;
    if (index) {
        ASSERT_MSG(*index < layouts.size(), "control layout: no layout {}", *index);
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
    ASSERT_MSG(index < layouts.size(), "control layout: no layout {}", index);
    layouts.erase(layouts.begin() + static_cast<std::ptrdiff_t>(index));
    WriteLocked();
}

void InitHud() {
    const ImGuiIO& io = ImGui::GetIO();
    const ImFont* font = io.Fonts->Fonts[0];
    for (std::size_t i = 0; i < SpotCount; i++) {
        for (const char* c = Buttons[i].disc; *c != '\0'; c++) {
            const ImFontGlyph* g = font->FindGlyph(static_cast<ImWchar>(*c));
            ASSERT_MSG(g != nullptr && g->Codepoint == static_cast<u32>(*c),
                       "on-screen buttons: the font has no glyph for {:?}", *c);
            glyphs[static_cast<u8>(*c)] = {g->X0, g->Y0, g->X1,       g->Y1,         g->U0,
                                           g->V0, g->U1, g->V1, g->AdvanceX, g->Visible != 0};
        }
    }
    white_u = io.Fonts->TexUvWhitePixel.x;
    white_v = io.Fonts->TexUvWhitePixel.y;
    glyph_height = font->FontSize;
}

void DrawHud(const ControlLayout& layout, u32 held) {
    if (layout.name != hud_for_name || layout.on_screen != hud_for_places ||
        layout.opacity != hud_for_opacity || held != hud_for_held) {
        hud_for_name = layout.name;
        hud_for_places = layout.on_screen;
        hud_for_opacity = layout.opacity;
        hud_for_held = held;
        MakeHud(layout, held);
    }
    if (hud_indices.empty()) {
        return;
    }
    auto& b = SgxdPresent::CurrentBatch();
    const u32 count = static_cast<u32>(hud_vertices.size() / SgxdPresent::VertexWords);
    const u32 index_count = static_cast<u32>(hud_indices.size());
    // A draw's indices start on a multiple of 8 (the VDM's index list).
    const u32 first = (b.index_count + 7) & ~7u;
    ASSERT_MSG(count <= b.max_vertices - b.vertex_count && b.draw_count < b.max_draws &&
                   first <= b.max_indices && index_count <= b.max_indices - first,
               "on-screen buttons: the frame's batch is full");
    std::memcpy(b.vertices + b.vertex_count * SgxdPresent::VertexWords, hud_vertices.data(),
                hud_vertices.size() * sizeof(u32));
    for (u32 i = 0; i < index_count; i++) {
        b.indices[first + i] = static_cast<u16>(b.vertex_count + hud_indices[i]);
    }
    b.draws[b.draw_count++] = SgxdRecipeDraw{
        first, index_count, SGXD_RECIPE_ISP_TRANS, SGXD_R_FPRIM, 0,
        SGXD_RECIPE_CLIP(0, 0, static_cast<u32>(DisplayWidth) / 32 - 1,
                         (static_cast<u32>(DisplayHeight) + 31) / 32 - 1)};
    b.vertex_count += count;
    b.index_count = first + index_count;
}

} // namespace VitaFrontend::ControlLayouts
