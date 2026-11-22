// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include "video_core/renderer_gxm/usse/gxp_writer.h"

namespace GxmRenderer::Usse {

/**
 * The PICA vertex emitter: a GXP vertex program for a PICA vertex program, written directly
 * as USSE instructions. The counterpart of cg_vs_shader_gen.cpp with the same interface:
 * the same attribute names (vs_in_attrN), the same uniforms (uniforms_f, uniforms_i,
 * flip_viewport, depth_scale, depth_offset, vs_in_regN), and the same fragment interface
 * by semantic (POSITION, COLOR0, TEXCOORD0-6), so the rasterizer's binding and uniform
 * upload are unchanged and every generated fragment program links against it. Where
 * SceShaccCg took seconds to tens of seconds per program on the console, this takes
 * microseconds.
 *
 * Returns the container, or empty (with `refusal` set) for a program it cannot express:
 * an instruction outside the set below, a bool uniform not baked in, a jump out of its
 * subroutine, deeper loop nesting than two, more live registers than the unified store
 * holds. The caller then falls back to the Cg path.
 */

/// Where a fragment-interface semantic comes from: an output attribute's component, or the
/// constant 1.0 when `attribute` is past the output count.
struct VsSemantic {
    uint8_t attribute;
    uint8_t component;
};

/**
 * What the program does with its results. The geometry shader's stream-out renders
 * (RasterizerSgxd::GeometryStage) store theirs to memory through USSE stores:
 * - Draw: the fragment interface, as for every draw.
 * - StoreOutputs: the vertex program of a draw with a geometry shader. The output attributes
 *   (the output mask's registers in order, float4 each) are stored at the address in the
 *   attribute `vs_out_address` (one word, fetched per vertex), and the position is a point
 *   outside the viewport, so nothing is drawn.
 * - Geometry: a PICA geometry program, one invocation per vertex. The attribute
 *   `gs_in_rows` brings 2 + gs_rows words: where the invocation's first triangle goes, where
 *   the address past its last one goes, then each input row's address less 4 (a fetch reads
 *   from one word past its base, docs/sgxd/usse.md). Row r is input register
 *   v<r> (point mode) or float uniform c<gs_first_uniform + r> (the uniform modes). Each
 *   triangle an EMIT completes is stored as its three vertices' output attributes, one after
 *   the other; at most gs_max_triangles are stored, the rest dropped. The position is as
 *   StoreOutputs'.
 *   With gs_rows_table (the variable-primitive mode, whose rows differ in number between
 *   invocations and are read with an address register), the attribute is 4 words: the
 *   first two as above, then the address less 4 of the invocation's table of 128 + 3 words,
 *   entry n the address less 4 of what c<n> reads (the invocation's row, or the shared
 *   uniform past its rows). Every read of c0..c<gs_rows - 1>, and every indexed read, goes
 *   through the table.
 */
enum class VsMode : uint8_t { Draw, StoreOutputs, Geometry };

struct VsRequest {
    std::span<const uint32_t> code;    ///< the PICA program code
    std::span<const uint32_t> swizzle; ///< the operand descriptors
    uint32_t code_size;                ///< the written extent of `code`
    uint32_t main_offset;
    uint32_t num_outputs;              ///< output attributes (gs_output_attributes_count)
    std::array<uint32_t, 16> output_map; ///< PICA output register -> output attribute
    std::array<VsSemantic, 24> semantics; ///< by VSOutputAttributes::Semantic
    /// The draw's shape, as cg_vs_shader_gen's VSExtra: input registers no loader feeds
    /// (uniforms vs_in_regN), the bool uniforms baked in, components each loader supplies.
    uint16_t default_regs;
    uint16_t bools;
    uint16_t bool_mask;
    std::array<uint8_t, 16> reg_components;
    /// Input registers whose loader is UNSIGNED_BYTE: the fetch brings the raw word, and the
    /// prologue unpacks its bytes to integer-valued floats, as the PICA's loader converts them.
    uint16_t u8_regs;
    VsMode mode = VsMode::Draw;
    uint32_t gs_rows = 0;
    bool gs_rows_uniform = false;
    bool gs_rows_table = false;
    uint32_t gs_first_uniform = 0;
    /// The fixed-primitive mode, for a program that reads a float uniform with an address
    /// register (GsIndexesUniforms): the entry ends in one more word, the address less 4 of the
    /// invocation's table of 128 + 3 words as gs_rows_table's, its rows at c<gs_first_uniform> on.
    /// The indexed reads go through it, the others as without it.
    bool gs_uniform_table = false;
    uint32_t gs_max_triangles = 0;
};

/// The triangles one invocation of the geometry program at `main_offset` stores at most: the
/// EMITs after a SETEMIT that asks for a primitive, along the longest path (an IF's longer
/// branch, a forward jump's longer way, a call's body at each call). 0 when an EMIT can run more
/// than once (one in a loop, or any backward jump): no bound.
[[nodiscard]] uint32_t GsTriangleBound(std::span<const uint32_t> code, uint32_t code_size,
                                       uint32_t main_offset);

/// Whether any instruction of the code, reached or not, reads a float uniform with an address
/// register (VsRequest::gs_uniform_table).
[[nodiscard]] bool GsIndexesUniforms(std::span<const uint32_t> code, uint32_t code_size);

/// `level` 2 runs the IR's optimising passes (the second tier).
[[nodiscard]] std::optional<GxpProgram> EmitVertexGxp(const VsRequest& request,
                                                      uint32_t& input_regs,
                                                      std::string* refusal = nullptr,
                                                      int level = 1);
[[nodiscard]] std::vector<uint8_t> EmitVertexProgram(const VsRequest& request, uint32_t& input_regs,
                                                     std::string* refusal = nullptr, int level = 1);

} // namespace GxmRenderer::Usse
