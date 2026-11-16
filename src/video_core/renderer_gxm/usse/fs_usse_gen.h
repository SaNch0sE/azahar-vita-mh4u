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

/**
 * PvHalf: N.H per light computed by the vertex program (TEXCOORD6.x / .y in place of the view
 * vector, under the vertex uniform pv_light_dir[0].w) for directional lights with the per-vertex
 * normal; the fragment program reads it instead of building the half vector per pixel. Bit 11
 * of lut_rr (set after SetReflMode, which rewrites the word).
 */
inline void SetPvHalf(Pica::Shader::FSConfig& config, bool on) {
    auto& raw = config.lighting.lut_rr.raw;
    raw = on ? (raw | (1u << 11)) : (raw & ~(1u << 11));
}
[[nodiscard]] inline bool PvHalfOf(const Pica::Shader::FSConfig& config) {
    return (config.lighting.lut_rr.raw >> 11) & 1u;
}
/// The vertex program is the pass-through one (software-shaded draws): TEXCOORD5 holds the
/// quaternion, not the per-vertex normal, so the fragment program builds its normal per pixel.
/// Bit 12 of lut_rr, set after SetReflMode.
inline void SetNoPvNormal(Pica::Shader::FSConfig& config, bool on) {
    auto& raw = config.lighting.lut_rr.raw;
    raw = on ? (raw | (1u << 12)) : (raw & ~(1u << 12));
}
[[nodiscard]] inline bool NoPvNormalOf(const Pica::Shader::FSConfig& config) {
    return (config.lighting.lut_rr.raw >> 12) & 1u;
}

/**
 * Black lighting terms, from the light colour registers (spare bits of lut_d0, bits 0-4 being
 * the PICA's): bit 8 + i the i-th enabled light's specular_0, bit 16 + i its ambient, bit 24
 * the global ambient. A black term is not loaded or added.
 */
inline void SetBlackTerms(Pica::Shader::FSConfig& config, uint32_t spec0, uint32_t ambient,
                          bool global_ambient) {
    auto& raw = config.lighting.lut_d0.raw;
    raw = (raw & 0x1Fu) | ((spec0 & 0xFFu) << 8) | ((ambient & 0xFFu) << 16) |
          (global_ambient ? 1u << 24 : 0u);
}
[[nodiscard]] inline bool Specular0Black(const Pica::Shader::FSConfig& config, uint32_t i) {
    return (config.lighting.lut_d0.raw >> (8 + i)) & 1u;
}
[[nodiscard]] inline bool AmbientBlack(const Pica::Shader::FSConfig& config, uint32_t i) {
    return (config.lighting.lut_d0.raw >> (16 + i)) & 1u;
}
[[nodiscard]] inline bool GlobalAmbientBlack(const Pica::Shader::FSConfig& config) {
    return (config.lighting.lut_d0.raw >> 24) & 1u;
}

/**
 * The Fresnel LUT's evaluation, the same way (spare bits of lut_fr): the lookup, or
 *   Schlick  c0 + c1 * (1 - |N.V|)^m, m in bits 16-23, (c0, c1) in the uniform
 *            lighting_fr_coef - MH4U's tables are all this form with m = 5
 *   Const    c0 (a flat table)
 */
enum class FrMode : uint32_t { Lookup = 0, Schlick = 1, Const = 2 };
inline void SetFrMode(Pica::Shader::FSConfig& config, FrMode mode, uint32_t m = 0) {
    auto& raw = config.lighting.lut_fr.raw;
    raw = (raw & 0x1Fu) | (static_cast<uint32_t>(mode) << 8) | ((m & 0xFFu) << 16);
}
[[nodiscard]] inline FrMode FrModeOf(const Pica::Shader::FSConfig& config) {
    return static_cast<FrMode>((config.lighting.lut_fr.raw >> 8) & 0x3u);
}
[[nodiscard]] inline uint32_t FrExponentOf(const Pica::Shader::FSConfig& config) {
    return (config.lighting.lut_fr.raw >> 16) & 0xFFu;
}

} // namespace GxmRenderer::Usse
