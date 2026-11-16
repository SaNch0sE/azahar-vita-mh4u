// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <cstdint>
#include <string>
#include <vector>
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
[[nodiscard]] std::vector<uint8_t> EmitFragmentProgram(const Pica::Shader::FSConfig& config,
                                                       std::string* refusal = nullptr,
                                                       int level = 1);

/// dbgstageN: lit programs stop after combiner stage `stage` (0-5) and output its colour with
/// alpha 1 and no fog, to find the stage a wrong colour enters at. -1: off.
void SetDebugStopStage(int stage);
/// dbgunlit: the dbgstage and dbgout switches act on the programs without lighting instead.
void SetDebugUnlit(bool unlit);
/// dbgopaque: the targeted programs output alpha 1, everything else unchanged.
void SetDebugOpaque(bool opaque);
/// Whether the next emitted program is one the debug switches act on (the rasterizer's
/// ux0:data/mh4u/dbg_hashes.txt filter; run with notier2 so no other thread emits).
void SetDebugSelected(bool selected);

/// Whether a procedural texture reduces to a function of one clamped coordinate (no noise,
/// no shift, no separate alpha, the U or V combiner, an unmipmapped filter): the program then
/// samples a 256-texel strip the rasterizer bakes on unit 6 (ProcTexStripUnit) instead of
/// computing it. The rasterizer asks the same question of the registers. `noptstrip` is off.
[[nodiscard]] bool ProcTexStripEligible(bool noise, Pica::TexturingRegs::ProcTexShift u_shift,
                                        Pica::TexturingRegs::ProcTexShift v_shift,
                                        bool separate_alpha,
                                        Pica::TexturingRegs::ProcTexCombiner combiner,
                                        Pica::TexturingRegs::ProcTexFilter filter);
constexpr uint32_t ProcTexStripUnit = 6;
constexpr uint32_t ProcTexStripTexels = 256;
/// Fog's baked strip: the fog LUT interpolated as the PICA does, 256 texels over depth.
constexpr uint32_t FogStripUnit = 7;

/// Fog read as a non-dependent texture query of TEXCOORD7 (the vertex programs' fog
/// coordinate) instead of in the shader; off with `nofogquery` (flag or ux0:data/mh4u file).
/// The strip is then baked reversed for a flipped fog (SyncFogStrip).
bool FogQueryEnabled();

/**
 * How a program evaluates the reflection-red LUT, chosen per draw by the rasterizer from the
 * LUT's contents and kept in spare bits of the configuration's lut_rr word (bits 0-4 are the
 * PICA's), so the program key, tier-2 re-emits and dumps all carry it. Zero is the real lookup.
 *   Lookup  the 256-entry LUT, fetched and interpolated as the PICA does
 *   Power   |N.H|^n, n in bits 16-23: Monster Hunter 4 Ultimate's tables are exactly x^16
 *           and x^40 - a few multiplies instead of a dependent texture read
 *   One     taken as 1 (the old shortcut; `reflone` forces it, for A/B)
 *   Dead    the specular_1 term is zero for every enabled light: not computed at all
 */
enum class ReflMode : uint32_t { Lookup = 0, Power = 1, One = 2, Dead = 3 };
inline void SetReflMode(Pica::Shader::FSConfig& config, ReflMode mode, uint32_t n = 0) {
    auto& raw = config.lighting.lut_rr.raw;
    raw = (raw & 0x1Fu) | (static_cast<uint32_t>(mode) << 8) | ((n & 0xFFu) << 16);
}
[[nodiscard]] inline ReflMode ReflModeOf(const Pica::Shader::FSConfig& config) {
    return static_cast<ReflMode>((config.lighting.lut_rr.raw >> 8) & 0x7u);
}
[[nodiscard]] inline uint32_t ReflExponentOf(const Pica::Shader::FSConfig& config) {
    return (config.lighting.lut_rr.raw >> 16) & 0xFFu;
}

} // namespace GxmRenderer::Usse
