// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <cstdint>
#include <vector>

namespace GxmRenderer::Usse {

/**
 * The rasterizer's fixed programs, written as USSE directly (they used to be Cg compiled
 * by SceShaccCg on the console at first boot; nothing compiles on the console any more):
 *
 *  - the pass-through vertex program for software-shaded draws: the PICA output vertex's
 *    attributes to the fragment interface, with the depth mapping on clip z
 *    (vert_position, vert_color, vert_texcoord0/1/2, vert_texcoord0_w, vert_normquat,
 *    vert_view; uniforms flip_viewport, depth_scale, depth_offset);
 *  - the blit quad's vertex program (vert_position float2, vert_texcoord float2 to
 *    POSITION and TEXCOORD0);
 *  - the blit's fragment program (one fetch of blit_source at TEXUNIT0);
 *  - the clear quad's fragment program (uniform float4 fill_colour).
 *
 * Each is the compiler's own program for the same Cg, instruction for instruction where
 * a compiled twin existed (the pass-through and blit vertex programs), so the words are
 * known to run.
 */
[[nodiscard]] std::vector<uint8_t> EmitPassThroughVertexProgram();
[[nodiscard]] std::vector<uint8_t> EmitBlitVertexProgram();
[[nodiscard]] std::vector<uint8_t> EmitBlitFragmentProgram();
[[nodiscard]] std::vector<uint8_t> EmitFillFragmentProgram();
/**
 * The present's sharp-bilinear upscale (TEXCOORD0 float2, sampler `sb_source` at TEXUNIT0):
 * nearest-looking texels with a linear blend only across the last output pixel of each
 * texel. Uniforms, in texels: sb_size (texture width, height), sb_inv (1 / size), sb_scale
 * (output pixels per texel, per axis), sb_lo and sb_hi (0.5 -/+ (0.5 - 0.5 / scale)). The
 * texture must be linear-filtered.
 */
[[nodiscard]] std::vector<uint8_t> EmitSharpBilinearFragmentProgram();
/**
 * FXAA (the classic low-tap form, green as luma) over a texture (TEXCOORD0 float2, sampler
 * `fx_source` at TEXUNIT0, linear-filtered). Uniforms: fx_half (half a texel, u and v),
 * fx_reduce (0.25 * reduce_mul, reduce_min), fx_span (span in texels, 1/6), fx_texel (one
 * texel, u and v).
 */
[[nodiscard]] std::vector<uint8_t> EmitFxaaFragmentProgram();

} // namespace GxmRenderer::Usse
