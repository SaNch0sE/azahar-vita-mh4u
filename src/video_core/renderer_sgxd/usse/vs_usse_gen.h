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
#include "video_core/renderer_sgxd/usse/usse_program.h"

namespace SgxdRenderer::Usse {

/**
 * The PICA vertex emitter: a USSE vertex program for a PICA vertex program. Its fragment
 * interface is by semantic (POSITION, COLOR0, TEXCOORD0-6), so every generated fragment
 * program links against it. A program it cannot express comes back empty, with `refusal` set.
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
 *   v<r> (point mode) or float uniform c<gs_first_uniform + r> (the uniform modes), then in
 *   the fixed-primitive mode one row for each register of gs_input_rows. Each
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

/**
 * Per-vertex effect lighting (Usse::PvEffectEligible), Draw mode only: the epilogue writes
 * TEXCOORD4 as (primary.rgb, N.V) in place of the quaternion, for the fragment program's
 * pv_effect form. N is (0, 0, 1) turned by the normalised quaternion and V the view normalised,
 * as upstream's generator makes them per pixel; primary.rgb = PvAmbient + sum_i PvLightDiffuse_i *
 * f(N . PvLightVector_i), unclamped, f max(x, 0) or |x| for a two-sided light (UniformKind's Pv
 * kinds, which the rasterizer fills).
 */
struct VsPvEffect {
    uint8_t lights = 0;          ///< 1 or 2; 0: off
    std::array<uint8_t, 2> num{}; ///< each light's PICA number (light_src[num])
    uint8_t two_sided = 0;       ///< bit i: light i's diffuse is two-sided
};

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
    /// Input registers whose loader is an integer format (UNSIGNED_BYTE, BYTE or SHORT): the
    /// fetch brings the raw words, and the prologue unpacks them to integer-valued floats, as
    /// the PICA's loader converts them. s8_regs and s16_regs mark the BYTE and SHORT ones.
    uint16_t u8_regs;
    uint16_t s8_regs = 0;
    uint16_t s16_regs = 0;
    VsMode mode = VsMode::Draw;
    uint32_t gs_rows = 0;
    bool gs_rows_uniform = false;
    bool gs_rows_table = false;
    uint32_t gs_first_uniform = 0;
    /// The fixed-primitive mode's input registers that hold an earlier draw's rows: one row each
    /// after the uniform rows, in register order, loaded by the prologue. The others read zero.
    uint16_t gs_input_rows = 0;
    /// The fixed-primitive mode, for a program that reads a float uniform with an address
    /// register (GsIndexesUniforms): the entry ends in one more word, the address less 4 of the
    /// invocation's table of 128 + 3 words as gs_rows_table's, its rows at c<gs_first_uniform> on.
    /// The indexed reads go through it, the others as without it.
    bool gs_uniform_table = false;
    uint32_t gs_max_triangles = 0;
    /// Geometry, point and fixed-primitive modes: the invocations run in order in one
    /// instance, the temporaries and the emit buffer's vertices kept from each to the next
    /// and, through a state block (GsStateQuads), from the draw before: for a program that reads
    /// a temporary before it writes it
    /// (Program::gs_temps_read_first). Its attribute is four words: the invocation count (a
    /// float), the first invocation's table entry less 4, the state block less 4 (its loads)
    /// and the state block (its stores); each entry is the words the parallel form's attribute
    /// holds. The count is at least one.
    bool gs_serial = false;
    /// With gs_serial, a trail program's (GsTrailHead): one instance a trail, side by side. Each
    /// instance's attribute is the in-order form's four words, its count negative where it starts
    /// past the draw's first invocation: it runs only where that invocation is a trail's head (the
    /// program's opening CMP and JMPC say so), and stops before the next head. Zero: none to run.
    /// Only the instance that runs the draw's last invocation stores the state block.
    bool gs_trails = false;
    /// Geometry: each invocation first zeroes its gs_max_triangles triangles' room, so the ones
    /// it leaves unset are degenerate (a padded draw's pass B draws every slot), rather than the
    /// CPU zeroing them all in uncached memory.
    bool gs_zero_slots = false;
    /// Per-vertex effect lighting's lights (VsPvEffect), Draw mode only.
    VsPvEffect pv{};
};

/// The in-order state block (VsRequest::gs_serial), in float4s: the 16 temporaries, then the emit
/// buffer's three vertices, 16 output attributes each (vertex v's attribute k at 16 + 16 v + k).
inline constexpr uint32_t GsStateQuads = 16 + 3 * 16;

/// The triangles one invocation of the geometry program at `main_offset` stores at most: the
/// EMITs after a SETEMIT that asks for a primitive, along the longest path (an IF's longer
/// branch, a forward jump's longer way, a call's body at each call). 0 when an EMIT can run more
/// than once (one in a loop, or any backward jump): no bound.
[[nodiscard]] uint32_t GsTriangleBound(std::span<const uint32_t> code, uint32_t code_size,
                                       uint32_t main_offset);

/// Whether the geometry program at `main_offset` draws trails: it opens with a CMP and a JMPC to
/// a head block that emits nothing, calls nothing, jumps nowhere and ENDs (it keeps what the
/// trail's invocations after it read: Monster Hunter 4 Ultimate's and XX's ribbons, which keep a
/// matrix in r11-r14, a counter in r15 and the emit buffer from one invocation to the next). The
/// invocations from one head to the next then run in order, and the trails side by side
/// (VsRequest::gs_trails).
[[nodiscard]] bool GsTrailHead(std::span<const uint32_t> code, uint32_t code_size,
                               uint32_t main_offset);

/// Whether any instruction of the code, reached or not, reads a float uniform with an address
/// register (VsRequest::gs_uniform_table).
[[nodiscard]] bool GsIndexesUniforms(std::span<const uint32_t> code, uint32_t code_size);

/// `level` 2 runs the IR's optimising passes (the second tier). `scratch`: the calling thread.
[[nodiscard]] std::optional<Program> EmitVertexProgram(const VsRequest& request,
                                                      uint32_t& input_regs, Scratch scratch,
                                                      std::string* refusal = nullptr,
                                                      int level = 1);


} // namespace SgxdRenderer::Usse
