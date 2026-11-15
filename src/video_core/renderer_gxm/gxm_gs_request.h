// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <bit>
#include "video_core/pica/regs_internal.h"
#include "video_core/renderer_gxm/usse/vs_usse_gen.h"

namespace GxmRenderer {

/**
 * The GS half of a fused VS + GS request (Usse::GsStage) for a Point-mode geometry-shader
 * draw with one VS vertex per GS invocation, and the GS's output map and semantics into the
 * request. Returns false, with `why` set, for a draw that shape does not cover. Shared by the
 * rasterizer and the host tool (tools/usse_host/gs_emit.cpp).
 */
inline bool BuildGsStage(const Pica::RegsInternal& regs, Usse::GsStage& gs,
                         Usse::VsRequest& request, const char** why) {
    using Pica::PipelineRegs;
    const auto& pipeline = regs.pipeline;
    const auto fail = [why](const char* what) {
        *why = what;
        return false;
    };
    if (pipeline.use_gs != PipelineRegs::UseGS::Yes ||
        pipeline.gs_config.mode != PipelineRegs::GSMode::Point) {
        return fail("not a Point-mode geometry shader");
    }
    if (pipeline.variable_primitive != 0 || regs.gs.input_to_uniform != 0) {
        return fail("variable primitive or GS inputs to uniforms");
    }
    if (pipeline.triangle_topology != PipelineRegs::TriangleTopology::Shader &&
        pipeline.triangle_topology != PipelineRegs::TriangleTopology::List) {
        return fail("topology");
    }
    const u32 vs_outputs = static_cast<u32>(std::popcount(regs.vs.output_mask.Value()));
    if (vs_outputs == 0 || vs_outputs > 16 ||
        pipeline.vs_outmap_total_minus_1_a + 1 != vs_outputs ||
        regs.gs.max_input_attribute_index + 1 != vs_outputs) {
        return fail("more than one VS vertex per GS invocation");
    }
    gs.vs_num_outputs = vs_outputs;
    gs.vs_output_map.fill(16);
    u32 n = 0;
    for (u32 reg = 0; reg < 16; reg++) {
        if (regs.vs.output_mask & (1u << reg)) {
            gs.vs_output_map[reg] = n++;
        }
    }
    gs.input_from.fill(0xff);
    for (u32 i = 0; i < vs_outputs; i++) {
        gs.input_from[regs.gs.GetRegisterForAttribute(i) & 15] = static_cast<u8>(i);
    }
    gs.main_offset = regs.gs.main_offset;
    // The final outputs are the GS's, in output_mask order; the rasterizer's semantic map
    // names them.
    request.num_outputs = 0;
    request.output_map.fill(16);
    for (u32 reg = 0; reg < 16; reg++) {
        if (regs.gs.output_mask & (1u << reg)) {
            request.output_map[reg] = request.num_outputs++;
        }
    }
    for (auto& s : request.semantics) {
        s = {16, 0};
    }
    const u32 total = std::min<u32>(regs.rasterizer.vs_output_total, 7);
    for (u32 attrib = 0; attrib < total; attrib++) {
        const auto& a = regs.rasterizer.vs_output_attributes[attrib];
        const u32 sem[4] = {a.map_x.Value(), a.map_y.Value(), a.map_z.Value(), a.map_w.Value()};
        for (u32 comp = 0; comp < 4; comp++) {
            if (sem[comp] < 24) {
                request.semantics[sem[comp]] = {static_cast<u8>(attrib), static_cast<u8>(comp)};
            }
        }
    }
    request.gs = &gs;
    return true;
}

} // namespace GxmRenderer
