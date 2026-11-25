// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "video_core/renderer_sgxd/usse/usse_program.h"
#include "video_core/shader/generator/pica_fs_config.h"

namespace SgxdRenderer::Usse {

/**
 * The PICA fragment emitter: a USSE fragment program (Program) for a fragment
 * configuration. Follows upstream's fragment generator (glsl_fs_shader_gen.cpp, and its Cg form
 * in tools/pica_cg) stage by stage, with the combiners in U8 where the PICA's precision allows
 * and the lighting LUTs read as filtered F16 rows. Its uniforms
 * (UniformKind: FSUniformData's fields) and samplers (SamplerKind) say what they hold, which
 * the rasterizer fills and binds them by.
 *
 * Returns the program, or empty when the configuration is one it does not express
 * (FragmentEmitter::Unsupported), an operand form the encoder cannot express, or more
 * secondary attributes than there are.
 */
/// `refusal`, when given, receives why an empty result came back. `level` 2 runs the IR's
/// optimising passes (the second tier). `scratch`: the calling thread.
/// The program itself, for sgxd, which places the code and builds the PDS programs.
[[nodiscard]] std::optional<Program> EmitFragmentProgram(const Pica::Shader::FSConfig& config,
                                                        Scratch scratch,
                                                        std::string* refusal = nullptr,
                                                        int level = 1);

/// The texture units 0-2 (a mask) the program for `config` samples: the TEV stages' sources,
/// the bump map and the shadow texture, as the emitter plans them.
[[nodiscard]] uint32_t SampledTextureUnits(const Pica::Shader::FSConfig& config);

/**
 * Per-vertex effect lighting (RasterizerSgxd::SetPvEffectLighting): whether a configuration's
 * lighting can be made at the vertices, the rasterizer's and the emitters' one rule. It holds
 * where the lighting is on with no bump map, one or two lights, each directional with no
 * distance or spot attenuation, no geometric factor and no shadow, with specular_0 and
 * specular_1 zero (so no specular term and no table but the Fresnel one), no shadow at all, and
 * the Fresnel table off or read at N.V. Then, as upstream's generator has it:
 *   primary.rgb = global_ambient + sum_i (ambient_i + diffuse_i * f(N.L_i)), f max(x, 0), or |x|
 *       for a two-sided light, L_i the light's position normalised;
 *   secondary.rgb = 0;
 *   each alpha the Fresnel table at N.V where its enable_*_alpha asks, else 1;
 * all clamped to [0, 1]. The vertex program makes primary.rgb and N.V (VsRequest::pv), the
 * fragment program reads them as TEXCOORD4 and looks the Fresnel table up per pixel.
 */
[[nodiscard]] bool PvEffectEligible(const Pica::Shader::FSConfig& config);

} // namespace SgxdRenderer::Usse
