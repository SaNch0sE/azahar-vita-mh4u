// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include "common/common_types.h"
#include "video_core/pica/dirty_regs.h"
#include "video_core/pica/regs_internal.h"

/**
 * The register sets the rasterizer's per-draw caches are keyed by. A draw hashes a set again
 * only after one of its registers changed (the dirty marks: PicaCore marks a plain register
 * only when a write changes it, as the command lists rewrite most of them before every draw
 * with the values they hold), and the hash finds the cached result.
 */
namespace SgxdRenderer::StateRegs {

#define STATE_TEV_REGS(n)                                                                          \
    PICA_REG_INDEX(texturing.tev_stage##n.sources_raw),                                            \
        PICA_REG_INDEX(texturing.tev_stage##n.modifiers_raw),                                      \
        PICA_REG_INDEX(texturing.tev_stage##n.ops_raw),                                            \
        PICA_REG_INDEX(texturing.tev_stage##n.scales_raw)
#define STATE_TEXTURE_REGS(n)                                                                      \
    PICA_REG_INDEX(texturing.texture##n.width), PICA_REG_INDEX(texturing.texture##n.type),         \
        PICA_REG_INDEX(texturing.texture##n##_format)
#define STATE_LIGHT_REGS(n)                                                                        \
    PICA_REG_INDEX(lighting.light[n].config), PICA_REG_INDEX(lighting.light[n].specular_0),        \
        PICA_REG_INDEX(lighting.light[n].specular_1), PICA_REG_INDEX(lighting.light[n].ambient)

/// The registers FSConfig's constructor and RasterizerSgxd::SplitAlpha read, whole words (a
/// field's neighbours in its word come along). Not the TEV constant colours or the lights'
/// colours and positions: uniforms, not the program, but for each light's specular_0,
/// specular_1 and ambient: one that is zero leaves its term out of the program
/// (RasterizerSgxd::FragmentConfigFor). The scissor box, the
/// viewport and the colour buffer's size are, as far as RasterizerSgxd::FragmentConfig drops a box
/// that covers the viewport.
inline constexpr std::array<u16, 91> Fragment{
    // FramebufferConfig
    PICA_REG_INDEX(rasterizer.scissor_test.mode),
    PICA_REG_INDEX(rasterizer.scissor_test.x1),
    PICA_REG_INDEX(rasterizer.scissor_test.x2),
    PICA_REG_INDEX(rasterizer.viewport_size_x),
    PICA_REG_INDEX(rasterizer.viewport_size_y),
    PICA_REG_INDEX(rasterizer.viewport_corner),
    PICA_REG_INDEX(framebuffer.framebuffer.width),
    PICA_REG_INDEX(rasterizer.depthmap_enable),
    PICA_REG_INDEX(framebuffer.output_merger.fragment_operation_mode),
    PICA_REG_INDEX(framebuffer.output_merger.alpha_blending),
    PICA_REG_INDEX(framebuffer.output_merger.logic_op),
    PICA_REG_INDEX(framebuffer.output_merger.alpha_test),
    // TextureConfig, ProcTexConfig, SplitAlpha (the sizes: an ETC1A4 unit's width)
    PICA_REG_INDEX(texturing.main_config),
    STATE_TEXTURE_REGS(0),
    STATE_TEXTURE_REGS(1),
    STATE_TEXTURE_REGS(2),
    PICA_REG_INDEX(texturing.shadow),
    PICA_REG_INDEX(texturing.tev_combiner_buffer_input),
    STATE_TEV_REGS(0),
    STATE_TEV_REGS(1),
    STATE_TEV_REGS(2),
    STATE_TEV_REGS(3),
    STATE_TEV_REGS(4),
    STATE_TEV_REGS(5),
    PICA_REG_INDEX(texturing.proctex),
    PICA_REG_INDEX(texturing.proctex_lut),
    PICA_REG_INDEX(texturing.proctex_lut_offset),
    // LightConfig
    PICA_REG_INDEX(lighting.disable),
    PICA_REG_INDEX(lighting.max_light_index),
    PICA_REG_INDEX(lighting.config0),
    PICA_REG_INDEX(lighting.config1),
    PICA_REG_INDEX(lighting.light_enable),
    PICA_REG_INDEX(lighting.abs_lut_input),
    PICA_REG_INDEX(lighting.lut_input),
    PICA_REG_INDEX(lighting.lut_scale),
    STATE_LIGHT_REGS(0),
    STATE_LIGHT_REGS(1),
    STATE_LIGHT_REGS(2),
    STATE_LIGHT_REGS(3),
    STATE_LIGHT_REGS(4),
    STATE_LIGHT_REGS(5),
    STATE_LIGHT_REGS(6),
    STATE_LIGHT_REGS(7),
};

#undef STATE_TEV_REGS
#undef STATE_TEXTURE_REGS
#undef STATE_LIGHT_REGS

/// RasterizerSgxd::VertexLayoutFor's words: the attribute formats after the base, each loader's
/// words 1 and 2 (its word 0 is the offset), the VS input count and input map.
inline constexpr std::array<u16, 29> Layout = [] {
    constexpr u32 Va = PICA_REG_INDEX(pipeline.vertex_attributes);
    std::array<u16, 29> r{};
    r[0] = Va + 1;
    r[1] = Va + 2;
    for (u32 l = 0; l < 12; l++) {
        r[2 + 2 * l] = static_cast<u16>(Va + 3 + 3 * l + 1);
        r[3 + 2 * l] = static_cast<u16>(Va + 3 + 3 * l + 2);
    }
    r[26] = PICA_REG_INDEX(vs.input_buffer_config);
    r[27] = PICA_REG_INDEX(vs.input_attribute_to_register_map_low);
    r[28] = PICA_REG_INDEX(vs.input_attribute_to_register_map_high);
    return r;
}();

/// A set in Pica::DirtyRegs' form.
template <std::size_t N>
constexpr std::array<u64, 12> Mask(const std::array<u16, N>& list) {
    static_assert(sizeof(Pica::DirtyRegs::qwords) == sizeof(std::array<u64, 12>));
    std::array<u64, 12> mask{};
    for (const u16 r : list) {
        mask[r >> 6] |= 1ull << (r & 63);
    }
    return mask;
}
inline constexpr std::array<u64, 12> FragmentMask = Mask(Fragment);
inline constexpr std::array<u64, 12> LayoutMask = Mask(Layout);

/// Whether one of the set's registers is marked in `dirty` (Pica::DirtyRegs::qwords).
inline bool AnyDirty(const std::array<u64, 12>& mask, const std::array<u64, 12>& dirty) {
    u64 any = 0;
    for (u32 w = 0; w < mask.size(); w++) {
        any |= dirty[w] & mask[w];
    }
    return any != 0;
}

/// A set's words hashed: two lanes of rotate, xor and multiply (FxHash, 32-bit multiplies,
/// what the CPU has). For a given word each step is a bijection of the lane, so two states
/// that differ in one word never hash alike; 64 bits for the rest.
template <std::size_t N, typename Words>
u64 Hash(const std::array<u16, N>& list, const Words& regs) {
    u32 a = 0, b = 0;
    for (const u16 r : list) {
        const u32 w = regs[r];
        a = (std::rotl(a, 5) ^ w) * 0x9e3779b1u;
        b = (std::rotl(b, 7) ^ w) * 0x85ebca77u;
    }
    return u64{a} << 32 | b;
}

} // namespace SgxdRenderer::StateRegs
