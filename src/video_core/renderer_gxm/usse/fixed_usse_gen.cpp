// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <bit>
#include "video_core/renderer_gxm/usse/fixed_usse_gen.h"
#include "video_core/renderer_gxm/usse/gxp_writer.h"
#include "video_core/renderer_gxm/usse/usse_encoder.h"

namespace GxmRenderer::Usse {

namespace {

const Src ConstOne{Reg::C(2), XXXX};
const Src ConstZeroOne{Reg::C(1), XYZW};
const Src ConstZero{Reg::C(0), XXXX};
const Src ConstHalf{Reg::C(12), XXXX};
const Reg I0 = Reg::I(0);
const Reg I1 = Reg::I(1);

// The fragment interface's output registers, as vs_usse_gen lays them out.
constexpr uint8_t OutPosition = 0;
constexpr uint8_t OutColor = 4;
constexpr uint8_t OutTexcoord0 = 8;
constexpr uint8_t OutTexcoord1 = 10;
constexpr uint8_t OutTexcoord2 = 12;
constexpr uint8_t OutTexcoord0W = 14;
constexpr uint8_t OutNormquat = 16;
constexpr uint8_t OutNormquatFlat = 20;
constexpr uint8_t OutView = 24;

} // Anonymous namespace

std::vector<uint8_t> EmitPassThroughVertexProgram() {
    // Attributes at four primary attributes each: position pa0, colour pa4, texcoord0 pa8,
    // texcoord1 pa12, texcoord2 pa16, texcoord0_w pa20, normquat pa24, view pa28.
    GxpProgram gxp;
    gxp.vertex = true;
    const char* names[] = {"vert_position",   "vert_color",    "vert_texcoord0", "vert_texcoord1",
                           "vert_texcoord2",  "vert_texcoord0_w", "vert_normquat", "vert_view"};
    const uint8_t widths[] = {4, 4, 2, 2, 2, 1, 4, 3};
    for (uint32_t i = 0; i < 8; i++) {
        gxp.attributes.push_back({names[i], i * 4});
        gxp.attrib_pa_regs |= ((1ull << widths[i]) - 1) << (i * 4);
    }
    gxp.pa_count = 32;
    gxp.uniforms.push_back({"flip_viewport", 0, 1, 1});
    gxp.uniforms.push_back({"depth_scale", 1, 1, 1});
    gxp.uniforms.push_back({"depth_offset", 2, 1, 1});
    gxp.uniform_floats = 4;
    gxp.vertex_outputs1 = 0x1f001800; // with TEXCOORD7, the fog coordinate (o27-30)
    gxp.vertex_outputs2 = 0xeff249;
    gxp.literals = {std::bit_cast<uint32_t>(255.0f / 256.0f), 0u}; // sa4: the fog scale

    Encoder e;
    e.skip_invalid = true;
    e.Phas(true);
    e.Nop();
    e.MoveInternal({Reg::O(OutColor), 3}, {Reg::Pa(4), XYZW});
    e.MoveInternal({Reg::O(OutColor + 2), 3}, {Reg::Pa(6), XYZW});
    e.MoveInternal({Reg::O(OutTexcoord0), 3}, {Reg::Pa(8), XYZW});
    e.MoveInternal({Reg::O(OutTexcoord1), 3}, {Reg::Pa(12), XYZW});
    e.MoveInternal({Reg::O(OutTexcoord2), 3}, {Reg::Pa(16), XYZW});
    e.MoveInternal({Reg::O(OutTexcoord0W), 1}, {Reg::Pa(20), XYZW});
    e.MoveInternal({Reg::O(OutNormquat), 3}, {Reg::Pa(24), XYZW});
    e.MoveInternal({Reg::O(OutNormquat + 2), 3}, {Reg::Pa(26), XYZW});
    e.MoveInternal({Reg::O(OutNormquatFlat), 3}, {Reg::Pa(24), XYZW});
    e.MoveInternal({Reg::O(OutNormquatFlat + 2), 3}, {Reg::Pa(26), XYZW});
    e.MoveInternal({Reg::O(OutView), 3}, {Reg::Pa(28), XYZW});
    e.MoveInternal({Reg::O(OutView + 2), 1}, {Reg::Pa(30), XYZW});
    // Position: y flipped when flip_viewport > 0.5, z = (z / w * depth_scale +
    // depth_offset) * w.
    e.Vec(VecOp::Mul, {I0, 0xf}, {Reg::Pa(0), XYZW}, ConstOne);
    e.Test(Cond::Gt, 2, {Reg::Sa(0)}, 0, ConstHalf, false);
    e.Vec(VecOp::Mul, {I0, 0x2}, {I0, YYYY, Mod::Neg}, ConstOne, Pred::P2);
    e.Comp(CompOp::Rcp, {I1, 0x1}, {I0}, 3);
    e.Vec(VecOp::Mul, {I1, 0x2}, {I0, ZZZZ}, {I1, XXXX});
    e.Vec(VecOp::Mul, {I1, 0x2}, {I1, YYYY}, {Reg::Sa(0), YYYY});
    e.Vec(VecOp::Add, {I1, 0x2}, {I1, YYYY}, {Reg::Sa(2), XXXX});
    e.Vec(VecOp::Mul, {I0, 0x4}, {I1, YYYY}, {I0, WWWW});
    e.MoveInternal({Reg::O(OutPosition), 3}, {I0, XYZW});
    e.MoveInternal({Reg::O(OutPosition + 2), 3}, {I0, ZWZW});
    // TEXCOORD7, the fog coordinate (vs_usse_gen's epilogue explains it).
    e.Vec(VecOp::Mul, {I1, 0x1}, {I0, WWWW}, ConstHalf);
    e.Vec(VecOp::Add, {I1, 0x2}, {I1, XXXX, Mod::Neg}, {I0, ZZZZ});
    e.Vec(VecOp::Mul, {I1, 0x2}, {I1, YYYY}, {Reg::Sa(4), XXXX});
    e.Vec(VecOp::Add, {I1, 0x2}, {I1, YYYY}, {I1, XXXX});
    e.MoveInternal({Reg::O(26), 0x2}, {I1, YYYY});
    e.MoveInternal({Reg::O(28), 0x1}, {I1, XXXX});
    e.MoveInternal({Reg::O(28), 0x2}, ConstZero);
    e.MoveInternal({Reg::O(30), 0x1}, {I0, WWWW});
    e.EndVertex();
    e.FinishNosched();
    gxp.primary = e.Words();
    return WriteGxp(gxp);
}

std::vector<uint8_t> EmitBlitVertexProgram() {
    GxpProgram gxp;
    gxp.vertex = true;
    gxp.attributes.push_back({"vert_position", 0});
    gxp.attributes.push_back({"vert_texcoord", 4});
    gxp.attrib_pa_regs = 0x33;
    gxp.pa_count = 8;
    gxp.vertex_outputs1 = 0x6001000; // six output registers: position, texcoord0
    gxp.vertex_outputs2 = 0x1;

    Encoder e;
    e.skip_invalid = true;
    e.Phas(true);
    e.MoveInternal({Reg::O(OutPosition), 3}, {Reg::Pa(0), XYZW});
    e.MoveInternal({Reg::O(OutPosition + 2), 3}, ConstZeroOne);
    e.MoveInternal({Reg::O(OutColor), 3}, {Reg::Pa(4), XYZW}); // o4: TEXCOORD0 here
    e.EndVertex();
    e.FinishNosched();
    gxp.primary = e.Words();
    return WriteGxp(gxp);
}

std::vector<uint8_t> EmitBlitFragmentProgram() {
    GxpProgram gxp;
    gxp.inputs.push_back({FragmentInput::TexCoord0, 2});
    gxp.samplers.push_back({"blit_source", 0});
    gxp.pa_count = 4;
    gxp.temp_count = 4;

    Encoder e;
    e.skip_invalid = false;
    e.Phas(true);
    e.Nop();
    e.Sample2D(Reg::R(0), Reg::Pa(0), static_cast<uint8_t>(SamplerSaOffset(0, 0, 0)), nullptr, 0);
    e.Wdf(0);
    e.PackF16({Reg::Pa(0), 0xf}, Reg::R(0), Reg::R(2));
    e.FinishNosched();
    gxp.primary = e.Words();
    return WriteGxp(gxp);
}

std::vector<uint8_t> EmitFillFragmentProgram() {
    GxpProgram gxp;
    gxp.uniforms.push_back({"fill_colour", 0, 4, 1});
    gxp.uniform_floats = 4;
    gxp.pa_count = 4;

    Encoder e;
    e.skip_invalid = false;
    e.Phas(true);
    e.Nop();
    e.PackF16({Reg::Pa(0), 0xf}, Reg::Sa(0), Reg::Sa(2));
    e.FinishNosched();
    gxp.primary = e.Words();
    return WriteGxp(gxp);
}

std::vector<uint8_t> EmitSharpBilinearFragmentProgram() {
    // t = uv * size; i = floor(t); f = t - i; g = f - clamp(f, lo, hi); uv' = (i + g * scale +
    // 0.5) / size: inside a texel the coordinate snaps to its centre, and only the band of
    // width 1 / scale at its edges blends to the neighbour.
    GxpProgram gxp;
    gxp.inputs.push_back({FragmentInput::TexCoord0, 2});
    gxp.uniforms.push_back({"sb_size", 0, 2, 1});
    gxp.uniforms.push_back({"sb_inv", 2, 2, 1});
    gxp.uniforms.push_back({"sb_scale", 4, 2, 1});
    gxp.uniforms.push_back({"sb_lo", 6, 2, 1});
    gxp.uniforms.push_back({"sb_hi", 8, 2, 1});
    gxp.uniform_floats = 10;
    gxp.samplers.push_back({"sb_source", 0});
    gxp.pa_count = 4;
    gxp.temp_count = 8;

    const Reg I2 = Reg::I(2);
    Encoder e;
    e.skip_invalid = false;
    e.Phas(true);
    e.Nop();
    e.Vec(VecOp::Mul, {I0, 0x3}, {Reg::Pa(0), XYZW}, {Reg::Sa(0), XYZW});    // t
    e.Vec(VecOp::Frc, {I1, 0x3}, {I0, XYZW}, {I0, XYZW});                    // f
    e.Vec(VecOp::Add, {I0, 0x3}, {I1, XYZW, Mod::Neg}, {I0, XYZW});          // i
    e.Vec(VecOp::Max, {I2, 0x3}, {I1, XYZW}, {Reg::Sa(6), XYZW});            // clamp(f, lo,
    e.Vec(VecOp::Min, {I2, 0x3}, {I2, XYZW}, {Reg::Sa(8), XYZW});            //   hi)
    e.Vec(VecOp::Add, {I1, 0x3}, {I2, XYZW, Mod::Neg}, {I1, XYZW});          // g
    e.Vec(VecOp::Mul, {I1, 0x3}, {I1, XYZW}, {Reg::Sa(4), XYZW});            // g * scale
    e.Vec(VecOp::Add, {I1, 0x3}, {I1, XYZW}, ConstHalf);                     //   + 0.5
    e.Vec(VecOp::Add, {I0, 0x3}, {I0, XYZW}, {I1, XYZW});                    // i + ...
    e.Vec(VecOp::Mul, {I0, 0x3}, {I0, XYZW}, {Reg::Sa(2), XYZW});            // / size
    e.MoveInternal({Reg::R(0), 0x3}, {I0, XYZW});
    e.Sample2D(Reg::R(4), Reg::R(0), static_cast<uint8_t>(SamplerSaOffset(10, 0, 0)), nullptr,
               0);
    e.Wdf(0);
    e.PackF16({Reg::Pa(0), 0xf}, Reg::R(4), Reg::R(6));
    e.FinishNosched();
    gxp.primary = e.Words();
    return WriteGxp(gxp);
}

std::vector<uint8_t> EmitFxaaFragmentProgram() {
    GxpProgram gxp;
    gxp.inputs.push_back({FragmentInput::TexCoord0, 2});
    gxp.uniforms.push_back({"fx_half", 0, 2, 1});
    gxp.uniforms.push_back({"fx_reduce", 2, 2, 1});
    gxp.uniforms.push_back({"fx_span", 4, 2, 1});
    gxp.uniforms.push_back({"fx_texel", 6, 2, 1});
    gxp.uniform_floats = 8;
    gxp.samplers.push_back({"fx_source", 0});
    gxp.pa_count = 4;
    gxp.temp_count = 32;
    const uint8_t sampler = static_cast<uint8_t>(SamplerSaOffset(8, 0, 0));

    const Reg I2 = Reg::I(2);
    const Reg uv = Reg::Pa(0);
    const auto R = [](uint8_t n) { return Reg::R(n); };
    const auto G = [](uint8_t base) { return Src{Reg::R(base), YYYY}; }; // a sample's green
    Encoder e;
    e.skip_invalid = false;
    e.Phas(true);
    e.Nop();
    // Corner coordinates (half a texel diagonally: each bilinear tap averages a 2x2 block):
    // NW r0, SE r2, NE r4, SW r6. Internal registers never live across a texture read.
    e.Vec(VecOp::Add, {I0, 0x3}, {Reg::Sa(0), XYZW, Mod::Neg}, {uv, XYZW}); // uv - h
    e.Vec(VecOp::Add, {I1, 0x3}, {Reg::Sa(0), XYZW}, {uv, XYZW});           // uv + h
    e.MoveInternal({R(0), 0x3}, {I0, XYZW});
    e.MoveInternal({R(2), 0x3}, {I1, XYZW});
    e.MoveInternal({I2, 0x1}, {I1, XYZW});
    e.MoveInternal({I2, 0x2}, {I0, XYZW});
    e.MoveInternal({R(4), 0x3}, {I2, XYZW});
    e.MoveInternal({I2, 0x1}, {I0, XYZW});
    e.MoveInternal({I2, 0x2}, {I1, XYZW});
    e.MoveInternal({R(6), 0x3}, {I2, XYZW});
    // M r8, NW r12, SE r16, NE r20, SW r24.
    e.Sample2D(R(8), uv, sampler, nullptr, 0);
    e.Sample2D(R(12), R(0), sampler, nullptr, 0);
    e.Sample2D(R(16), R(2), sampler, nullptr, 0);
    e.Sample2D(R(20), R(4), sampler, nullptr, 0);
    e.Sample2D(R(24), R(6), sampler, nullptr, 0);
    e.Wdf(0);
    // Luma range of the five, kept in r28.x (min) and r30.x (max).
    e.Vec(VecOp::Min, {I0, 0x1}, G(12), G(16));
    e.Vec(VecOp::Min, {I0, 0x1}, {I0, XXXX}, G(20));
    e.Vec(VecOp::Min, {I0, 0x1}, {I0, XXXX}, G(24));
    e.Vec(VecOp::Min, {I0, 0x1}, {I0, XXXX}, G(8));
    e.Vec(VecOp::Max, {I1, 0x1}, G(12), G(16));
    e.Vec(VecOp::Max, {I1, 0x1}, {I1, XXXX}, G(20));
    e.Vec(VecOp::Max, {I1, 0x1}, {I1, XXXX}, G(24));
    e.Vec(VecOp::Max, {I1, 0x1}, {I1, XXXX}, G(8));
    e.MoveInternal({R(28), 0x1}, {I0, XYZW});
    e.MoveInternal({R(30), 0x1}, {I1, XYZW});
    // Sums: x = NW + NE, y = SW + SE, z = NW + SW, w = NE + SE.
    e.Vec(VecOp::Add, {I0, 0x1}, G(12), G(20));
    e.Vec(VecOp::Add, {I0, 0x2}, G(24), G(16));
    e.Vec(VecOp::Add, {I0, 0x4}, G(12), G(24));
    e.Vec(VecOp::Add, {I0, 0x8}, G(20), G(16));
    // dir = ((SW + SE) - (NW + NE), (NW + SW) - (NE + SE)).
    e.Vec(VecOp::Add, {I1, 0x1}, {I0, XXXX, Mod::Neg}, {I0, YYYY});
    e.Vec(VecOp::Add, {I1, 0x2}, {I0, WWWW, Mod::Neg}, {I0, ZZZZ});
    // reduce = max((sum of four) * 0.25 * reduce_mul, reduce_min).
    e.Vec(VecOp::Add, {I2, 0x1}, {I0, XXXX}, {I0, YYYY});
    e.Vec(VecOp::Mul, {I2, 0x1}, {I2, XXXX}, {Reg::Sa(2), XXXX});
    e.Vec(VecOp::Max, {I2, 0x1}, {I2, XXXX}, {Reg::Sa(2), YYYY});
    // rcp(min(|dir.x|, |dir.y|) + reduce).
    e.Vec(VecOp::Max, {I2, 0x2}, {I1, YYYY, Mod::Neg}, {I1, YYYY});
    e.Vec(VecOp::Max, {I2, 0x4}, {I1, XXXX, Mod::Neg}, {I1, XXXX});
    e.Vec(VecOp::Min, {I2, 0x2}, {I2, ZZZZ}, {I2, YYYY});
    e.Vec(VecOp::Add, {I2, 0x2}, {I2, YYYY}, {I2, XXXX});
    e.Comp(CompOp::Rcp, {I2, 0x4}, {I2}, 1);
    e.Vec(VecOp::Mul, {I1, 0x3}, {I1, XYZW}, {I2, ZZZZ});
    // Clamped to the span, in texels.
    e.Vec(VecOp::Min, {I1, 0x3}, {I1, XYZW}, {Reg::Sa(4), XXXX});
    e.Vec(VecOp::Max, {I1, 0x3}, {Reg::Sa(4), XXXX, Mod::Neg}, {I1, XYZW});
    e.Vec(VecOp::Mul, {I1, 0x3}, {I1, XYZW}, {Reg::Sa(6), XYZW});
    // Taps along it: uv -/+ dir / 6 (r0, r2), uv -/+ dir / 2 (r4, r6).
    e.Vec(VecOp::Mul, {I2, 0x3}, {I1, XYZW}, {Reg::Sa(4), YYYY});
    e.Vec(VecOp::Add, {I0, 0x3}, {I2, XYZW, Mod::Neg}, {uv, XYZW});
    e.MoveInternal({R(0), 0x3}, {I0, XYZW});
    e.Vec(VecOp::Add, {I0, 0x3}, {I2, XYZW}, {uv, XYZW});
    e.MoveInternal({R(2), 0x3}, {I0, XYZW});
    e.Vec(VecOp::Mul, {I2, 0x3}, {I1, XYZW}, ConstHalf);
    e.Vec(VecOp::Add, {I0, 0x3}, {I2, XYZW, Mod::Neg}, {uv, XYZW});
    e.MoveInternal({R(4), 0x3}, {I0, XYZW});
    e.Vec(VecOp::Add, {I0, 0x3}, {I2, XYZW}, {uv, XYZW});
    e.MoveInternal({R(6), 0x3}, {I0, XYZW});
    e.Sample2D(R(12), R(0), sampler, nullptr, 0);
    e.Sample2D(R(16), R(2), sampler, nullptr, 0);
    e.Sample2D(R(20), R(4), sampler, nullptr, 0);
    e.Sample2D(R(24), R(6), sampler, nullptr, 0);
    e.Wdf(0);
    // A = (r12 + r16) / 2; B = (A + (r20 + r24) / 2) / 2.
    e.MoveF32x4({I0, 0xf}, R(12), R(14));
    e.MoveF32x4({I1, 0xf}, R(16), R(18));
    e.Vec(VecOp::Add, {I0, 0xf}, {I0, XYZW}, {I1, XYZW});
    e.Vec(VecOp::Mul, {I0, 0xf}, {I0, XYZW}, ConstHalf);
    e.MoveF32x4({I1, 0xf}, R(20), R(22));
    e.MoveF32x4({I2, 0xf}, R(24), R(26));
    e.Vec(VecOp::Add, {I1, 0xf}, {I1, XYZW}, {I2, XYZW});
    e.Vec(VecOp::Mul, {I1, 0xf}, {I1, XYZW}, ConstHalf);
    e.Vec(VecOp::Add, {I1, 0xf}, {I1, XYZW}, {I0, XYZW});
    e.Vec(VecOp::Mul, {I1, 0xf}, {I1, XYZW}, ConstHalf);
    // B unless its luma leaves the five's range, then A.
    e.Test(Cond::Lt, 0, {I1}, 1, {R(28), XXXX}, true);
    e.Test(Cond::Gt, 0, {I1}, 1, {R(30), XXXX}, true, Pred::NotP0);
    e.MoveInternal({I1, 0xf}, {I0, XYZW}, Pred::P0);
    e.MoveInternal({R(8), 0x3}, {I1, XYZW});
    e.MoveInternal({R(10), 0x3}, {I1, ZWZW});
    e.PackF16({Reg::Pa(0), 0xf}, R(8), R(10));
    e.FinishNosched();
    gxp.primary = e.Words();
    return WriteGxp(gxp);
}

} // namespace GxmRenderer::Usse
