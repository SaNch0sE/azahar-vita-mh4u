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

} // namespace SgxdRenderer::Usse
