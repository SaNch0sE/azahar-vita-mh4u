// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "video_core/renderer_gxm/usse/gxp_writer.h"
#include "video_core/shader/generator/pica_fs_config.h"

namespace GxmRenderer::Usse {

/**
 * The PICA fragment emitter: a GXP program for a fragment configuration, written directly
 * as USSE instructions. Mirrors cg_fs_shader_gen.cpp
 * pass for pass and names its uniforms the same, so the rasterizer's uniform upload and
 * texture binding are unchanged; what the compiler took seconds for takes microseconds.
 *
 * Returns the container, or empty when the configuration is outside what is implemented
 * so far (the caller compiles the Cg instead): lighting, fog, procedural textures,
 * scissor, projection/cube/shadow textures, border colours, shadow rendering, W-buffer
 * depth, min/max blend emulation, bitwise logic ops.
 */
/// `refusal`, when given, receives why an empty result came back. `level` 2 runs the IR's
/// optimising passes (the second tier).
/// The program itself, for sgxd, which places the code and builds the PDS programs.
[[nodiscard]] std::optional<GxpProgram> EmitFragmentGxp(const Pica::Shader::FSConfig& config,
                                                        std::string* refusal = nullptr,
                                                        int level = 1);
[[nodiscard]] std::vector<uint8_t> EmitFragmentProgram(const Pica::Shader::FSConfig& config,
                                                       std::string* refusal = nullptr,
                                                       int level = 1);

/// The lighting's diagnostics switch (sgxd_flags light16why), set before any program is
/// emitted.
struct LightingSwitches {
    /// A probe: the U8 combiners read the vertex colour from a second, fixed-point iterator
    /// of it (one register after the other inputs) instead of packing the four floats.
    bool u8_colour = false;
};
inline LightingSwitches lighting_switches;

/// The texture units 0-2 (a mask) the program for `config` samples: the TEV stages' sources,
/// the bump map and the shadow texture, as the emitter plans them.
[[nodiscard]] uint32_t SampledTextureUnits(const Pica::Shader::FSConfig& config);

} // namespace GxmRenderer::Usse
