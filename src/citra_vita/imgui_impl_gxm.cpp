// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cstring>

#include <imgui_vita2d/imgui.h>
#include <psp2/gxm.h>

#include "citra_vita/gxm_present.h"
#include "citra_vita/gxm_shaders.gen.h"
#include "citra_vita/imgui_impl_gxm.h"
#include "common/logging/log.h"

namespace {

using VitaFrontend::GxmPresent::DisplayHeight;
using VitaFrontend::GxmPresent::DisplayWidth;

struct BackendState {
    SceGxmShaderPatcherId vertex_id{}, fragment_id{};
    SceGxmVertexProgram* vertex_program{};
    SceGxmFragmentProgram* fragment_program{};
    const SceGxmProgramParameter* scale_param{};
    const SceGxmProgramParameter* offset_param{};
    SceGxmTexture font_texture{};
};

BackendState s;

} // Anonymous namespace

bool ImGui_ImplGxm_Init() {
    auto* patcher = VitaFrontend::GxmPresent::Patcher();
    const SceGxmProgram* vertex = GxmShader_imgui_v();
    const SceGxmProgram* fragment = GxmShader_imgui_f();
    if (sceGxmProgramCheck(vertex) != 0 || sceGxmProgramCheck(fragment) != 0) {
        LOG_CRITICAL(Frontend, "the vendored imgui programs are not valid GXP");
        return false;
    }
    sceGxmShaderPatcherRegisterProgram(patcher, vertex, &s.vertex_id);
    sceGxmShaderPatcherRegisterProgram(patcher, fragment, &s.fragment_id);

    const auto attribute = [&](const char* name, u16 offset, SceGxmAttributeFormat format,
                               u8 components) {
        const SceGxmProgramParameter* param = sceGxmProgramFindParameterByName(vertex, name);
        SceGxmVertexAttribute attr{};
        attr.streamIndex = 0;
        attr.offset = offset;
        attr.format = static_cast<u8>(format);
        attr.componentCount = components;
        attr.regIndex = sceGxmProgramParameterGetResourceIndex(param);
        return attr;
    };
    static_assert(sizeof(ImDrawVert) == 20, "ImDrawVert layout changed");
    SceGxmVertexAttribute attrs[3] = {
        attribute("aPosition", IM_OFFSETOF(ImDrawVert, pos), SCE_GXM_ATTRIBUTE_FORMAT_F32, 2),
        attribute("aTexcoord", IM_OFFSETOF(ImDrawVert, uv), SCE_GXM_ATTRIBUTE_FORMAT_F32, 2),
        attribute("aColor", IM_OFFSETOF(ImDrawVert, col), SCE_GXM_ATTRIBUTE_FORMAT_U8N, 4),
    };
    SceGxmVertexStream stream{};
    stream.stride = sizeof(ImDrawVert);
    stream.indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
    if (sceGxmShaderPatcherCreateVertexProgram(patcher, s.vertex_id, attrs, 3, &stream, 1,
                                               &s.vertex_program) < 0) {
        return false;
    }

    SceGxmBlendInfo blend{};
    blend.colorMask = SCE_GXM_COLOR_MASK_ALL;
    blend.colorFunc = SCE_GXM_BLEND_FUNC_ADD;
    blend.alphaFunc = SCE_GXM_BLEND_FUNC_ADD;
    blend.colorSrc = SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
    blend.colorDst = SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaSrc = SCE_GXM_BLEND_FACTOR_ONE;
    blend.alphaDst = SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    if (sceGxmShaderPatcherCreateFragmentProgram(
            patcher, s.fragment_id, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, SCE_GXM_MULTISAMPLE_NONE,
            &blend, vertex, &s.fragment_program) < 0) {
        return false;
    }
    s.scale_param = sceGxmProgramFindParameterByName(vertex, "uScale");
    s.offset_param = sceGxmProgramFindParameterByName(vertex, "uOffset");

    // The font atlas into a GPU-visible linear texture.
    ImGuiIO& io = ImGui::GetIO();
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    const u32 bytes = static_cast<u32>(width * height * 4);
    void* font_pixels = VitaFrontend::GxmPresent::AllocMapped(bytes);
    if (font_pixels == nullptr) {
        return false;
    }
    std::memcpy(font_pixels, pixels, bytes);
    // ImGui hands the atlas over as RGBA byte order, R first: the ABGR swizzle, like the display
    // surface.
    sceGxmTextureInitLinear(&s.font_texture, font_pixels, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR,
                            static_cast<u32>(width), static_cast<u32>(height), 1);
    sceGxmTextureSetMinFilter(&s.font_texture, SCE_GXM_TEXTURE_FILTER_LINEAR);
    sceGxmTextureSetMagFilter(&s.font_texture, SCE_GXM_TEXTURE_FILTER_LINEAR);
    io.Fonts->TexID = &s.font_texture;
    io.BackendRendererName = "imgui_impl_gxm";
    return true;
}

void ImGui_ImplGxm_Shutdown() {
    // The atlas's memory is GxmPresent's, which frees it at its own Shutdown.
    auto* patcher = VitaFrontend::GxmPresent::Patcher();
    if (s.vertex_program != nullptr) {
        sceGxmShaderPatcherReleaseVertexProgram(patcher, s.vertex_program);
    }
    if (s.fragment_program != nullptr) {
        sceGxmShaderPatcherReleaseFragmentProgram(patcher, s.fragment_program);
    }
    if (s.vertex_id != nullptr) {
        sceGxmShaderPatcherUnregisterProgram(patcher, s.vertex_id);
    }
    if (s.fragment_id != nullptr) {
        sceGxmShaderPatcherUnregisterProgram(patcher, s.fragment_id);
    }
    s = BackendState{};
}

void ImGui_ImplGxm_RenderDrawData(ImDrawData* draw_data) {
    if (draw_data == nullptr || draw_data->CmdListsCount == 0) {
        return;
    }
    SceGxmContext* const context = VitaFrontend::GxmPresent::Context();
    sceGxmSetVertexProgram(context, s.vertex_program);
    sceGxmSetFragmentProgram(context, s.fragment_program);

    void* uniform_buffer = nullptr;
    sceGxmReserveVertexDefaultUniformBuffer(context, &uniform_buffer);
    const float scale[2] = {2.0f / DisplayWidth, -2.0f / DisplayHeight};
    const float offset[2] = {-1.0f, 1.0f};
    sceGxmSetUniformDataF(uniform_buffer, s.scale_param, 0, 2, scale);
    sceGxmSetUniformDataF(uniform_buffer, s.offset_param, 0, 2, offset);

    for (int n = 0; n < draw_data->CmdListsCount; n++) {
        const ImDrawList* list = draw_data->CmdLists[n];
        const u32 vertex_bytes = static_cast<u32>(list->VtxBuffer.Size) * sizeof(ImDrawVert);
        const u32 index_bytes = static_cast<u32>(list->IdxBuffer.Size) * sizeof(ImDrawIdx);
        auto* verts = static_cast<ImDrawVert*>(VitaFrontend::GxmPresent::FrameAlloc(vertex_bytes));
        auto* indices =
            static_cast<ImDrawIdx*>(VitaFrontend::GxmPresent::FrameAlloc(index_bytes, 2));
        std::memcpy(verts, list->VtxBuffer.Data, vertex_bytes);
        std::memcpy(indices, list->IdxBuffer.Data, index_bytes);
        sceGxmSetVertexStream(context, 0, verts);

        for (int c = 0; c < list->CmdBuffer.Size; c++) {
            const ImDrawCmd& cmd = list->CmdBuffer[c];
            if (cmd.UserCallback != nullptr) {
                cmd.UserCallback(list, &cmd);
                continue;
            }
            sceGxmSetFragmentTexture(context, 0, static_cast<SceGxmTexture*>(cmd.TextureId));
            // The region clip is tile-granular (32 pixels); ImGui's clip mostly bounds scrolling
            // regions, where a slightly loose one does not show.
            const u32 cx0 = static_cast<u32>(std::max(cmd.ClipRect.x, 0.0f));
            const u32 cy0 = static_cast<u32>(std::max(cmd.ClipRect.y, 0.0f));
            const u32 cx1 = static_cast<u32>(std::min(cmd.ClipRect.z, float{DisplayWidth}));
            const u32 cy1 = static_cast<u32>(std::min(cmd.ClipRect.w, float{DisplayHeight}));
            if (cx1 <= cx0 || cy1 <= cy0) {
                continue;
            }
            sceGxmSetRegionClip(context, SCE_GXM_REGION_CLIP_OUTSIDE, cx0, cy0, cx1 - 1, cy1 - 1);
            sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLES, SCE_GXM_INDEX_FORMAT_U16,
                       indices + cmd.IdxOffset, cmd.ElemCount);
        }
    }
}
