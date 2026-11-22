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

// The region clip of a clip rectangle in pixels: the 32-pixel tiles it touches (inclusive), or
// 0 for the whole target. False when nothing of it is on the target.
bool TileClip(const ImVec4& rect, u32* clip) {
    const u32 x0 = static_cast<u32>(std::max(rect.x, 0.0f));
    const u32 y0 = static_cast<u32>(std::max(rect.y, 0.0f));
    const u32 x1 = static_cast<u32>(std::min(rect.z, static_cast<float>(DisplayWidth)));
    const u32 y1 = static_cast<u32>(std::min(rect.w, static_cast<float>(DisplayHeight)));
    if (x1 <= x0 || y1 <= y0) {
        return false;
    }
    const u32 tx0 = x0 / 32, ty0 = y0 / 32, tx1 = (x1 - 1) / 32, ty1 = (y1 - 1) / 32;
    const bool whole = tx0 == 0 && ty0 == 0 && tx1 == (DisplayWidth - 1) / 32 &&
                       ty1 == (DisplayHeight - 1) / 32;
    *clip = whole ? 0 : SGXD_RECIPE_CLIP(tx0, ty0, tx1, ty1);
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
            u32 clip = 0;
            if (cmd.ElemCount == 0 || !TileClip(cmd.ClipRect, &clip)) {
                continue;
            }
            // A draw's indices start on a multiple of 8 (the VDM's index list).
            const u32 first = (b.index_count + 7) & ~7u;
            if (b.draw_count >= b.max_draws || first > b.max_indices ||
                cmd.ElemCount > b.max_indices - first) {
                overflow = true;
                break;
            }
            const ImDrawIdx* indices = list->IdxBuffer.Data + cmd.IdxOffset;
            const u32 offset = base + cmd.VtxOffset;
            for (u32 i = 0; i < cmd.ElemCount; i++) {
                b.indices[first + i] = static_cast<u16>(indices[i] + offset);
            }
            b.draws[b.draw_count++] = SgxdRecipeDraw{first, cmd.ElemCount, SGXD_RECIPE_ISP_TRANS,
                                                     SGXD_R_FPRIM, 0, clip};
            b.index_count = first + cmd.ElemCount;
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
