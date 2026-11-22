// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string_view>

#include <imgui_vita2d/imgui.h>
#include <psp2/kernel/processmgr.h>

#include "citra_vita/hud_stats.h"
#include "citra_vita/sgxd_present.h"
#include "citra_vita/stats_hud.h"
#include "common/assert.h"
#include "sgxd_recipe.h"

namespace VitaFrontend::StatsHud {

namespace {

using SgxdPresent::VertexWords;

/// A character's quad at the pen (its line's top-left), in pixels at the overlay's scale, and
/// its texels in the atlas.
struct Glyph {
    float x0, y0, x1, y1;
    float u0, v0, u1, v1;
    float advance;
    bool visible;
};
std::array<Glyph, 128> glyphs{};
float white_u, white_v; ///< the atlas's white texel, for the box
float line_height;

/// The box's top-right corner, its padding and the gap between the lines (the ImGui window's).
constexpr float Right = static_cast<float>(SgxdPresent::DisplayWidth) - 8.0f;
constexpr float Top = 8.0f;
constexpr float Padding = 16.0f;
constexpr float LineGap = 10.0f;
/// A8B8G8R8: the box translucent near-black, the text white.
constexpr u32 BoxColour = IM_COL32(15, 15, 15, 140);
constexpr u32 TextColour = IM_COL32(255, 255, 255, 255);

/// The overlay's quads (four vertices each), made when the numbers change.
constexpr u32 MaxQuads = 48;
std::array<u32, MaxQuads * 4 * VertexWords> vertices{};
u32 quads = 0;
u32 clip = 0;
/// The numbers the quads show: speed, game fps, shown fps (all three ~0u: the stats hidden), and
/// the volume (~0u: none).
std::array<u32, 4> made_for{~0u, ~0u, ~0u, ~0u};
std::atomic<bool> stats_shown{true};

void PutQuad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
             u32 colour) {
    DEBUG_ASSERT(quads < MaxQuads);
    u32* v = vertices.data() + quads * 4 * VertexWords;
    const float corners[4][4] = {
        {x0, y0, u0, v0}, {x1, y0, u1, v0}, {x1, y1, u1, v1}, {x0, y1, u0, v1}};
    for (u32 k = 0; k < 4; k++) {
        std::memcpy(v + k * VertexWords, corners[k], sizeof(corners[k]));
        v[k * VertexWords + 4] = colour;
    }
    quads++;
}

float Width(const char* text) {
    float w = 0.0f;
    for (const char* c = text; *c != '\0'; c++) {
        w += glyphs[static_cast<u8>(*c) & 0x7f].advance;
    }
    return w;
}

void PutText(float x, float y, const char* text) {
    for (const char* c = text; *c != '\0'; c++) {
        const Glyph& g = glyphs[static_cast<u8>(*c) & 0x7f];
        DEBUG_ASSERT_MSG(g.advance != 0.0f, "stats overlay: no glyph for {:?}", *c);
        if (g.visible) {
            PutQuad(x + g.x0, y + g.y0, x + g.x1, y + g.y1, g.u0, g.v0, g.u1, g.v1, TextColour);
        }
        x += g.advance;
    }
}

void Make(bool stats, u32 speed, u32 fps10, u32 shown, u32 volume) {
    char speed_line[32], frame_line[32], volume_line[32];
    std::snprintf(speed_line, sizeof(speed_line), "%u%%/%u", speed, (fps10 + 5) / 10);
    std::snprintf(frame_line, sizeof(frame_line), "%.1fms/%ufps",
                  shown != 0 ? 1000.0f / static_cast<float>(shown) : 0.0f, shown);
    std::snprintf(volume_line, sizeof(volume_line), "Volume %u/30", volume);
    // The speed and frame lines, then the volume's.
    std::array<const char*, 3> text{};
    u32 lines = 0;
    if (stats) {
        text[lines++] = speed_line;
        text[lines++] = frame_line;
    }
    if (volume != ~0u) {
        text[lines++] = volume_line;
    }
    float width = 0.0f;
    for (u32 n = 0; n < lines; n++) {
        width = std::max(width, Width(text[n]));
    }
    width += 2.0f * Padding;
    const float height = lines * line_height + (lines - 1) * LineGap + 2.0f * Padding;
    const float left = std::floor(Right - width);
    quads = 0;
    PutQuad(left, Top, Right, Top + height, white_u, white_v, white_u, white_v, BoxColour);
    for (u32 n = 0; n < lines; n++) {
        PutText(left + Padding, Top + Padding + n * (line_height + LineGap), text[n]);
    }
    // The 32-pixel tiles the box covers: the TA's region clip.
    clip = SGXD_RECIPE_CLIP(static_cast<u32>(left) / 32, static_cast<u32>(Top) / 32,
                            (static_cast<u32>(Right) - 1) / 32,
                            (static_cast<u32>(Top + height) - 1) / 32);
}

} // Anonymous namespace

void SetShown(bool shown) {
    stats_shown.store(shown, std::memory_order_relaxed);
}

void Init() {
    const ImGuiIO& io = ImGui::GetIO();
    // The atlas's first font, the 13-pixel one, at 1.2 rather than the menus' scale
    // (FontGlobalScale).
    const ImFont* font = io.Fonts->Fonts[0];
    constexpr float scale = 1.2f;
    for (const char c : std::string_view{" 0123456789%/.fmpsVolue"}) {
        const ImFontGlyph* g = font->FindGlyph(static_cast<ImWchar>(c));
        ASSERT_MSG(g != nullptr && g->Codepoint == static_cast<u32>(c),
                   "stats overlay: the font has no glyph for {:?}", c);
        glyphs[static_cast<u8>(c)] = {
            g->X0 * scale, g->Y0 * scale, g->X1 * scale, g->Y1 * scale,       g->U0,
            g->V0,         g->U1,         g->V1,         g->AdvanceX * scale, g->Visible != 0};
    }
    white_u = io.Fonts->TexUvWhitePixel.x;
    white_v = io.Fonts->TexUvWhitePixel.y;
    line_height = font->FontSize * scale;
}

void Draw() {
    if (!HudStats::valid.load(std::memory_order_acquire)) {
        return;
    }
    // The volume while the volume keys' last step is recent (EmuWindowVita::VolumeKeys).
    const bool volume =
        sceKernelGetProcessTimeWide() < HudStats::volume_until_us.load(std::memory_order_acquire);
    const bool stats = stats_shown.load(std::memory_order_relaxed);
    if (!stats && !volume) {
        return;
    }
    const std::array<u32, 4> now{
        stats ? HudStats::speed_percent.load(std::memory_order_relaxed) : ~0u,
        stats ? HudStats::game_fps10.load(std::memory_order_relaxed) : ~0u,
        stats ? HudStats::shown_fps.load(std::memory_order_relaxed) : ~0u,
        volume ? HudStats::volume_level.load(std::memory_order_relaxed) : ~0u};
    if (now != made_for) {
        made_for = now;
        Make(stats, now[0], now[1], now[2], now[3]);
    }
    auto& b = SgxdPresent::CurrentBatch();
    const u32 count = quads * 4;
    const u32 index_count = quads * 6;
    // A draw's indices start on a multiple of 8 (the VDM's index list).
    const u32 first = (b.index_count + 7) & ~7u;
    DEBUG_ASSERT_MSG(count <= b.max_vertices - b.vertex_count && b.draw_count < b.max_draws &&
                         first <= b.max_indices && index_count <= b.max_indices - first,
                     "stats overlay: the frame's batch is full");
    std::memcpy(b.vertices + b.vertex_count * VertexWords, vertices.data(),
                count * VertexWords * sizeof(u32));
    for (u32 q = 0; q < quads; q++) {
        const u16 v = static_cast<u16>(b.vertex_count + q * 4);
        u16* const out = b.indices + first + q * 6;
        out[0] = v;
        out[1] = static_cast<u16>(v + 1);
        out[2] = static_cast<u16>(v + 2);
        out[3] = v;
        out[4] = static_cast<u16>(v + 2);
        out[5] = static_cast<u16>(v + 3);
    }
    b.draws[b.draw_count++] =
        SgxdRecipeDraw{first, index_count, SGXD_RECIPE_ISP_TRANS, SGXD_R_FPRIM, 0, clip};
    b.vertex_count += count;
    b.index_count = first + index_count;
}

} // namespace VitaFrontend::StatsHud
