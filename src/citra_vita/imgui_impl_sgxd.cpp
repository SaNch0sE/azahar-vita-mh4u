// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cstring>

#include <imgui_vita2d/imgui.h>

#include "citra_vita/imgui_impl_sgxd.h"
#include "citra_vita/sgxd_present.h"
#include "common/logging/log.h"
#include "sgxd_recipe.h"

namespace {

using VitaFrontend::SgxdPresent::DisplayHeight;
using VitaFrontend::SgxdPresent::DisplayWidth;
using VitaFrontend::SgxdPresent::VertexWords;

static_assert(sizeof(ImDrawVert) == VertexWords * 4, "ImDrawVert layout changed");
static_assert(IM_OFFSETOF(ImDrawVert, pos) == 0 && IM_OFFSETOF(ImDrawVert, uv) == 8 &&
                  IM_OFFSETOF(ImDrawVert, col) == 16,
              "ImDrawVert is not the recipe's x, y, u, v, colour");

// ImDrawCmd's TextureId for the font atlas: every draw samples the one texture, so the value
// only has to be non-null.
int font_texture_tag;
bool overflow_logged;

// A draw command's clip rectangle as the GPU and the CPU apply it. The region clip keeps whole
// 32-pixel tiles, so a window's content would show up to 31 pixels past a side that falls inside
// a tile (a scrolled list over the tabs above it); triangles across such a side are cut on the
// CPU to the rectangle itself.
struct Clip {
    u32 tiles;   ///< the region clip of the tiles the rectangle touches; 0, the whole target
    ImVec4 rect; ///< the rectangle on the target
    bool exact;  ///< the tiles are the rectangle: nothing to cut
};

// The command's clip; false when nothing of its rectangle is on the target.
bool ClipOf(const ImVec4& rect, Clip* clip) {
    const float width = static_cast<float>(DisplayWidth);
    const float height = static_cast<float>(DisplayHeight);
    const ImVec4 on(std::max(rect.x, 0.0f), std::max(rect.y, 0.0f), std::min(rect.z, width),
                    std::min(rect.w, height));
    const u32 x0 = static_cast<u32>(on.x);
    const u32 y0 = static_cast<u32>(on.y);
    const u32 x1 = static_cast<u32>(on.z);
    const u32 y1 = static_cast<u32>(on.w);
    if (x1 <= x0 || y1 <= y0) {
        return false;
    }
    const u32 tx0 = x0 / 32, ty0 = y0 / 32, tx1 = (x1 - 1) / 32, ty1 = (y1 - 1) / 32;
    const bool whole = tx0 == 0 && ty0 == 0 && tx1 == (DisplayWidth - 1) / 32 &&
                       ty1 == (DisplayHeight - 1) / 32;
    clip->tiles = whole ? 0 : SGXD_RECIPE_CLIP(tx0, ty0, tx1, ty1);
    clip->rect = on;
    clip->exact = on.x == static_cast<float>(tx0 * 32) && on.y == static_cast<float>(ty0 * 32) &&
                  on.z == std::min(static_cast<float>((tx1 + 1) * 32), width) &&
                  on.w == std::min(static_cast<float>((ty1 + 1) * 32), height);
    return true;
}

// The vertex at `t` of the way from a to b: position and texture coordinates, and each byte of
// the colour, as the rasteriser interpolates them across the triangle.
ImDrawVert Lerp(const ImDrawVert& a, const ImDrawVert& b, float t) {
    ImDrawVert v;
    v.pos = ImVec2(a.pos.x + (b.pos.x - a.pos.x) * t, a.pos.y + (b.pos.y - a.pos.y) * t);
    v.uv = ImVec2(a.uv.x + (b.uv.x - a.uv.x) * t, a.uv.y + (b.uv.y - a.uv.y) * t);
    v.col = 0;
    for (u32 shift = 0; shift < 32; shift += 8) {
        const float ca = static_cast<float>(a.col >> shift & 0xff);
        const float cb = static_cast<float>(b.col >> shift & 0xff);
        v.col |= static_cast<u32>(ca + (cb - ca) * t + 0.5f) << shift;
    }
    return v;
}

// The part of the convex polygon in[0, n) on the inner side of one of the rectangle's sides
// (the x or y coordinate `bound`, inside towards `sign`), into `out`; its count. One side adds at
// most one vertex.
int CutPolygon(const ImDrawVert* in, int n, ImDrawVert* out, bool y, float bound, float sign) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        const ImDrawVert& a = in[i];
        const ImDrawVert& b = in[(i + 1) % n];
        const float da = sign * ((y ? a.pos.y : a.pos.x) - bound);
        const float db = sign * ((y ? b.pos.y : b.pos.x) - bound);
        if (da >= 0.0f) {
            out[m++] = a;
        }
        if ((da >= 0.0f) != (db >= 0.0f)) {
            ImDrawVert v = Lerp(a, b, da / (da - db));
            (y ? v.pos.y : v.pos.x) = bound;
            out[m++] = v;
        }
    }
    return m;
}

// The command's triangles cut to `rect`, as indices from `first`: a triangle inside keeps its
// vertices, one outside goes, one across a side becomes a fan of vertices added to the batch.
// Their count in `written`; false when the batch has no room for them.
bool CutTriangles(VitaFrontend::SgxdPresent::Batch& b, const ImDrawList& list,
                  const ImDrawCmd& cmd, u32 offset, const ImVec4& rect, u32 first, u32* written) {
    const ImDrawVert* vertices = list.VtxBuffer.Data + cmd.VtxOffset;
    const ImDrawIdx* indices = list.IdxBuffer.Data + cmd.IdxOffset;
    const auto outcode = [&](const ImDrawVert& v) {
        return (v.pos.x < rect.x ? 1u : 0u) | (v.pos.x > rect.z ? 2u : 0u) |
               (v.pos.y < rect.y ? 4u : 0u) | (v.pos.y > rect.w ? 8u : 0u);
    };
    const u32 room = b.max_indices - first;
    u32 n = 0;
    for (u32 i = 0; i + 2 < cmd.ElemCount; i += 3) {
        const ImDrawVert& v0 = vertices[indices[i]];
        const ImDrawVert& v1 = vertices[indices[i + 1]];
        const ImDrawVert& v2 = vertices[indices[i + 2]];
        const u32 c0 = outcode(v0), c1 = outcode(v1), c2 = outcode(v2);
        if ((c0 & c1 & c2) != 0) {
            continue;
        }
        if ((c0 | c1 | c2) == 0) {
            if (room - n < 3) {
                return false;
            }
            for (u32 k = 0; k < 3; k++) {
                b.indices[first + n++] = static_cast<u16>(indices[i + k] + offset);
            }
            continue;
        }
        // A triangle cut by the four sides has at most seven vertices.
        ImDrawVert poly[2][8] = {{v0, v1, v2}};
        int count = 3;
        count = CutPolygon(poly[0], count, poly[1], false, rect.x, 1.0f);
        count = CutPolygon(poly[1], count, poly[0], false, rect.z, -1.0f);
        count = CutPolygon(poly[0], count, poly[1], true, rect.y, 1.0f);
        count = CutPolygon(poly[1], count, poly[0], true, rect.w, -1.0f);
        if (count < 3) {
            continue;
        }
        const u32 added = static_cast<u32>(count);
        if (added > b.max_vertices - b.vertex_count || room - n < 3 * (added - 2)) {
            return false;
        }
        const u32 at = b.vertex_count;
        std::memcpy(b.vertices + at * VertexWords, poly[0], added * sizeof(ImDrawVert));
        b.vertex_count += added;
        for (u32 k = 1; k + 1 < added; k++) {
            b.indices[first + n++] = static_cast<u16>(at);
            b.indices[first + n++] = static_cast<u16>(at + k);
            b.indices[first + n++] = static_cast<u16>(at + k + 1);
        }
    }
    *written = n;
    return true;
}

} // Anonymous namespace

bool ImGui_ImplSgxd_Init() {
    ImGuiIO& io = ImGui::GetIO();
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    // ImGui hands the atlas over as RGBA byte order, R first: A8B8G8R8, the recipe's texel.
    if (!VitaFrontend::SgxdPresent::SetTexture(pixels, static_cast<u32>(width),
                                              static_cast<u32>(height), true)) {
        LOG_CRITICAL(Frontend, "sgxd: the {}x{} font atlas was not taken", width, height);
        return false;
    }
    LOG_INFO(Frontend, "sgxd: the font atlas is {}x{} ({} KiB)", width, height,
             width * height * 4 / 1024);
    io.Fonts->TexID = &font_texture_tag;
    io.BackendRendererName = "imgui_impl_sgxd";
    overflow_logged = false;
    return true;
}

void ImGui_ImplSgxd_Shutdown() {
    ImGui::GetIO().Fonts->TexID = nullptr;
}

void ImGui_ImplSgxd_RenderDrawData(ImDrawData* draw_data) {
    if (draw_data == nullptr || draw_data->CmdListsCount == 0 ||
        !VitaFrontend::SgxdPresent::IsInitialized()) {
        return;
    }
    auto& b = VitaFrontend::SgxdPresent::CurrentBatch();
    bool overflow = false;
    for (int n = 0; n < draw_data->CmdListsCount && !overflow; n++) {
        const ImDrawList* list = draw_data->CmdLists[n];
        const u32 base = b.vertex_count;
        const u32 count = static_cast<u32>(list->VtxBuffer.Size);
        if (count > b.max_vertices - base) {
            overflow = true;
            break;
        }
        std::memcpy(b.vertices + base * VertexWords, list->VtxBuffer.Data,
                    static_cast<std::size_t>(count) * sizeof(ImDrawVert));
        b.vertex_count += count;

        for (int c = 0; c < list->CmdBuffer.Size; c++) {
            const ImDrawCmd& cmd = list->CmdBuffer[c];
            if (cmd.UserCallback != nullptr) {
                // There is no render state to reset: every draw is the same program.
                if (cmd.UserCallback != ImDrawCallback_ResetRenderState) {
                    cmd.UserCallback(list, &cmd);
                }
                continue;
            }
            Clip clip;
            if (cmd.ElemCount == 0 || !ClipOf(cmd.ClipRect, &clip)) {
                continue;
            }
            // A draw's indices start on a multiple of 8 (the VDM's index list).
            const u32 first = (b.index_count + 7) & ~7u;
            if (b.draw_count >= b.max_draws || first > b.max_indices) {
                overflow = true;
                break;
            }
            const u32 offset = base + cmd.VtxOffset;
            u32 written = 0;
            if (clip.exact) {
                if (cmd.ElemCount > b.max_indices - first) {
                    overflow = true;
                    break;
                }
                const ImDrawIdx* indices = list->IdxBuffer.Data + cmd.IdxOffset;
                for (u32 i = 0; i < cmd.ElemCount; i++) {
                    b.indices[first + i] = static_cast<u16>(indices[i] + offset);
                }
                written = cmd.ElemCount;
            } else if (!CutTriangles(b, *list, cmd, offset, clip.rect, first, &written)) {
                overflow = true;
                break;
            }
            if (written == 0) {
                continue;
            }
            b.draws[b.draw_count++] = SgxdRecipeDraw{first, written, SGXD_RECIPE_ISP_TRANS,
                                                     SGXD_R_FPRIM, 0, clip.tiles};
            b.index_count = first + written;
        }
    }
    if (overflow && !overflow_logged) {
        overflow_logged = true;
        LOG_ERROR(Frontend,
                  "sgxd: a frame's ImGui geometry passed the pass's capacity ({} vertices, {} "
                  "indices, {} draws); the rest was left out",
                  b.max_vertices, b.max_indices, b.max_draws);
    }
}
