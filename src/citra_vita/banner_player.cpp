// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <optional>
#include <span>

#include <fmt/format.h>

#include "citra_vita/banner_player.h"
#include "citra_vita/emu_window_vita.h"
#include "citra_vita/sgxd_present.h"
#include "common/alignment.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "core/core.h"
#include "core/hle/service/gsp/gsp_command.h"
#include "core/loader/loader.h"
#include "core/loader/smdh.h"
#include "core/memory.h"
#include "video_core/gpu.h"
#include "video_core/pica/regs_internal.h"

namespace VitaFrontend {

namespace {

using Banner::Material;

// VRAM, as the banner lays it out.
/// The top screen's framebuffer: RGBA8, the LCD's 240-pixel columns 400 times.
constexpr PAddr ScreenAddress = Memory::VRAM_PADDR;
constexpr u32 ScreenColumnBytes = 240 * 4;
constexpr u32 BufferBytes = ScreenColumnBytes * 400;
/// The PICA's colour (RGBA8) and depth (D24S8) buffers, 240x400 and tiled.
constexpr PAddr ColorAddress = Memory::VRAM_PADDR + 0x60000;
constexpr PAddr DepthAddress = Memory::VRAM_PADDR + 0xc0000;
/// The frame's command list, its vertices and indices, the banner's textures.
constexpr PAddr CommandAddress = Memory::VRAM_PADDR + 0x120000;
constexpr u32 CommandBytes = 0x40000;
constexpr PAddr VertexAddress = Memory::VRAM_PADDR + 0x160000;
constexpr u32 VertexBytes = 0x100000;
constexpr PAddr TextureAddress = Memory::VRAM_PADDR + 0x260000;
constexpr u32 TextureBytes = Memory::VRAM_SIZE - 0x260000;
static_assert(BufferBytes <= 0x60000);
/// A white texture (8x8 RGBA8) for a mapper without one: an unbound unit's.
constexpr u32 WhiteBytes = 8 * 8 * 4;

constexpr u32 FormatRgba8 = 0;
/// The frames a selection rests before its banner loads.
constexpr u32 LoadDelayFrames = 12;

// PICA registers, by 3dbrew's names where azahar's register structures have no field.
namespace Reg {
constexpr u32 Finalize = PICA_REG_INDEX(irq_request);
constexpr u32 FaceCulling = PICA_REG_INDEX(rasterizer.cull_mode);
constexpr u32 ViewportWidth = PICA_REG_INDEX(rasterizer.viewport_size_x);
constexpr u32 ClipEnable = PICA_REG_INDEX(rasterizer.clip_enable);
constexpr u32 DepthScale = PICA_REG_INDEX(rasterizer.viewport_depth_range);
constexpr u32 DepthOffset = PICA_REG_INDEX(rasterizer.viewport_depth_near_plane);
constexpr u32 OutmapTotal = PICA_REG_INDEX(rasterizer.vs_output_total);
constexpr u32 Outmap0 = PICA_REG_INDEX(rasterizer.vs_output_attributes[0]);
constexpr u32 OutattrMode = 0x64; // GPUREG_SH_OUTATTR_MODE
constexpr u32 Scissor = PICA_REG_INDEX(rasterizer.scissor_test.mode);
constexpr u32 ViewportXy = PICA_REG_INDEX(rasterizer.viewport_corner);
constexpr u32 DepthMapEnable = PICA_REG_INDEX(rasterizer.depthmap_enable);
constexpr u32 RenderBufDim = 0x6e;  // GPUREG_RENDERBUF_DIM
constexpr u32 OutattrClock = 0x6f;  // GPUREG_SH_OUTATTR_CLOCK
constexpr u32 TexUnitConfig = PICA_REG_INDEX(texturing.main_config);
constexpr u32 TexUnit0 = PICA_REG_INDEX(texturing.texture0);
constexpr u32 TexUnit1 = PICA_REG_INDEX(texturing.texture1);
constexpr u32 TexUnit2 = PICA_REG_INDEX(texturing.texture2);
constexpr u32 CubeAddress = PICA_REG_INDEX(texturing.cube_address[0]);
constexpr u32 TexUnit0Type = PICA_REG_INDEX(texturing.texture0_format);
constexpr u32 TexUnit1Type = PICA_REG_INDEX(texturing.texture1_format);
constexpr u32 TexUnit2Type = PICA_REG_INDEX(texturing.texture2_format);
constexpr u32 LightingEnable0 = PICA_REG_INDEX(texturing.fragment_lighting_enable);
constexpr u32 TexEnv0 = PICA_REG_INDEX(texturing.tev_stage0);
constexpr u32 TexEnv4 = PICA_REG_INDEX(texturing.tev_stage4);
constexpr u32 TexEnvUpdateBuffer = PICA_REG_INDEX(texturing.tev_combiner_buffer_input);
constexpr u32 TexEnvBufferColor = PICA_REG_INDEX(texturing.tev_combiner_buffer_color);
constexpr u32 ColorOperation = PICA_REG_INDEX(framebuffer.output_merger);
constexpr u32 AlphaTest = PICA_REG_INDEX(framebuffer.output_merger.alpha_test);
constexpr u32 DepthColorMask = PICA_REG_INDEX(framebuffer.output_merger.depth_color_mask);
constexpr u32 FramebufferInvalidate = 0x110;
constexpr u32 FramebufferFlush = 0x111;
constexpr u32 ColorBufferRead = 0x112;
constexpr u32 DepthBufferFormat = PICA_REG_INDEX(framebuffer.framebuffer.depth_format);
constexpr u32 ColorBufferFormat = PICA_REG_INDEX(framebuffer.framebuffer.color_format);
constexpr u32 FramebufferBlock32 = 0x11b;
constexpr u32 DepthBufferLoc = PICA_REG_INDEX(framebuffer.framebuffer.depth_buffer_address);
constexpr u32 ColorBufferLoc = PICA_REG_INDEX(framebuffer.framebuffer.color_buffer_address);
constexpr u32 FramebufferDim = 0x11e;
constexpr u32 Light0 = PICA_REG_INDEX(lighting.light[0]);
constexpr u32 LightingAmbient = PICA_REG_INDEX(lighting.global_ambient);
constexpr u32 LightingNumLights = PICA_REG_INDEX(lighting.max_light_index);
constexpr u32 LightingConfig0 = PICA_REG_INDEX(lighting.config0);
constexpr u32 LightingConfig1 = PICA_REG_INDEX(lighting.config1);
constexpr u32 LightingLutIndex = PICA_REG_INDEX(lighting.lut_config);
constexpr u32 LightingEnable1 = PICA_REG_INDEX(lighting.disable);
constexpr u32 LightingLutData = PICA_REG_INDEX(lighting.lut_data[0]);
constexpr u32 LightingLutAbs = PICA_REG_INDEX(lighting.abs_lut_input);
constexpr u32 LightingLutSelect = PICA_REG_INDEX(lighting.lut_input);
constexpr u32 LightingLutScale = PICA_REG_INDEX(lighting.lut_scale);
constexpr u32 LightingPermutation = PICA_REG_INDEX(lighting.light_enable);
constexpr u32 AttribBuffersLoc = PICA_REG_INDEX(pipeline.vertex_attributes);
constexpr u32 AttribBuffer0Offset = PICA_REG_INDEX(pipeline.vertex_attributes.attribute_loaders[0]);
constexpr u32 IndexBufferConfig = PICA_REG_INDEX(pipeline.index_array);
constexpr u32 NumVertices = PICA_REG_INDEX(pipeline.num_vertices);
constexpr u32 GeostageConfig = 0x229;
constexpr u32 VertexOffset = PICA_REG_INDEX(pipeline.vertex_offset);
constexpr u32 DrawElements = PICA_REG_INDEX(pipeline.trigger_draw_indexed);
constexpr u32 VtxFunc = 0x231;
constexpr u32 VshNumAttr = PICA_REG_INDEX(pipeline.max_input_attrib_index);
constexpr u32 VshComMode = PICA_REG_INDEX(pipeline.gs_unit_exclusive_configuration);
constexpr u32 StartDrawFunc0 = PICA_REG_INDEX(pipeline.gpu_mode);
constexpr u32 VshOutmapTotal1 = PICA_REG_INDEX(pipeline.vs_outmap_total_minus_1_a);
constexpr u32 VshOutmapTotal2 = PICA_REG_INDEX(pipeline.vs_outmap_total_minus_1_b);
constexpr u32 GshMisc0 = PICA_REG_INDEX(pipeline.gs_config);
constexpr u32 GeostageConfig2 = 0x253;
constexpr u32 PrimitiveConfig = PICA_REG_INDEX(pipeline.triangle_topology);
constexpr u32 RestartPrimitive = PICA_REG_INDEX(pipeline.restart_primitive);
constexpr u32 VshBoolUniform = PICA_REG_INDEX(vs.bool_uniforms);
constexpr u32 VshInputBufferConfig = PICA_REG_INDEX(vs.input_buffer_config);
constexpr u32 VshEntryPoint = PICA_REG_INDEX(vs.main_offset);
constexpr u32 VshAttributesPermutationLow = PICA_REG_INDEX(vs.input_attribute_to_register_map_low);
constexpr u32 VshAttributesPermutationHigh =
    PICA_REG_INDEX(vs.input_attribute_to_register_map_high);
constexpr u32 VshOutmapMask = PICA_REG_INDEX(vs.output_mask);
constexpr u32 VshCodeTransferEnd = 0x2bf;
constexpr u32 VshCodeTransferIndex = PICA_REG_INDEX(vs.program.offset);
constexpr u32 VshCodeTransferData = PICA_REG_INDEX(vs.program.set_word[0]);
constexpr u32 VshOpdescsIndex = PICA_REG_INDEX(vs.swizzle_patterns.offset);
constexpr u32 VshOpdescsData = PICA_REG_INDEX(vs.swizzle_patterns.set_word[0]);
static_assert(OutattrMode == 0x64 && TexUnit0 == 0x81 && TexEnv4 == 0xf0 && Light0 == 0x140 &&
              LightingLutData == 0x1c8 && LightingPermutation == 0x1d9 &&
              AttribBuffer0Offset == 0x203 && VshComMode == 0x244 && GshMisc0 == 0x252 &&
              VshOpdescsData == 0x2d6);
} // namespace Reg

/// The banner's vertex shader: each of the seven attributes (Banner::GpuVertex) to the output
/// of its number, `mov oN, vN` with the identity swizzle, then `end`.
constexpr u32 ShaderOutputs = 7;
constexpr u32 PassThroughSwizzle = 0x36f; // xyzw written, src1 .xyzw, nothing negated
std::array<u32, ShaderOutputs + 1> PassThroughShader() {
    std::array<u32, ShaderOutputs + 1> code{};
    constexpr u32 Mov = 0x13, End = 0x22;
    for (u32 i = 0; i < ShaderOutputs; i++) {
        code[i] = Mov << 26 | i << 21 | i << 12; // operand descriptor 0
    }
    code[ShaderOutputs] = End << 26;
    return code;
}

/// The outputs' semantics (VSOutputAttributes): position, quaternion, colour, texture
/// coordinate 0 with its w, texture coordinates 1 and 2, the view vector.
constexpr std::array<u32, ShaderOutputs> OutputMap = {
    0x03020100, 0x07060504, 0x0b0a0908, 0x1f100d0c, 0x1f1f0f0e, 0x1f1f1716, 0x1f141312,
};
/// The attributes' formats: floats, 4, 4, 4, 3, 2, 2 and 3 of them (ATTRIBBUFFERS_FORMAT_LOW).
constexpr u32 AttributeFormats = 0x0b77bfff;
constexpr u32 VertexStride = sizeof(Banner::GpuVertex);

/// A float in the PICA's narrower formats (f24, f31, f16): `mantissa` and `exponent` bits; one
/// past the format's range its largest of that sign.
u32 PicaFloat(float v, u32 mantissa, u32 exponent) {
    u32 bits;
    std::memcpy(&bits, &v, 4);
    const u32 sign = bits >> 31 << (mantissa + exponent);
    const s32 e = static_cast<s32>(bits >> 23 & 0xff) - 127 + ((1 << (exponent - 1)) - 1);
    if ((bits & 0x7fffffff) == 0 || e <= 0) {
        return sign;
    }
    if (e >= (1 << exponent) - 1) {
        return sign | ((1u << exponent) - 2) << mantissa | ((1u << mantissa) - 1);
    }
    return sign | static_cast<u32>(e) << mantissa | (bits & 0x7fffff) >> (23 - mantissa);
}
u32 F24(float v) {
    return PicaFloat(v, 16, 7);
}
u32 F31(float v) {
    return PicaFloat(v, 23, 7);
}
u32 F16(float v) {
    return PicaFloat(v, 10, 5);
}

/// A lighting colour register: 0-255 a channel (more is brighter than white), blue lowest.
u32 LightColor(const Common::Vec3f& c) {
    const auto ch = [](float v) {
        return static_cast<u32>(std::clamp(std::lround(v * 255.0f), 0l, 1023l));
    };
    return ch(c.z) | ch(c.y) << 10 | ch(c.x) << 20;
}

u32 Rgba8(const Common::Vec4f& c) {
    const auto ch = [](float v) {
        return static_cast<u32>(std::clamp(std::lround(v * 255.0f), 0l, 255l));
    };
    return ch(c.x) | ch(c.y) << 8 | ch(c.z) << 16 | ch(c.w) << 24;
}

/// A PICA command list being built: each write a value and a header (register, byte mask,
/// more values, consecutive registers), each command padded to 8 bytes.
class Commands {
public:
    explicit Commands(std::vector<u32>& words_) : words{words_} {
        words.clear();
    }

    void Write(u32 reg, u32 value, u32 mask = 0xf) {
        words.push_back(value);
        words.push_back(reg | mask << 16);
    }
    /// `values` into consecutive registers from `reg`.
    void Run(u32 reg, std::span<const u32> values) {
        Command(reg, values, true);
    }
    /// `values` one after the other into `reg` (an upload port).
    void Burst(u32 reg, std::span<const u32> values) {
        for (std::size_t at = 0; at < values.size(); at += 256) {
            Command(reg, values.subspan(at, std::min<std::size_t>(256, values.size() - at)), false);
        }
    }
    /// The list's end: the interrupt request, the list a multiple of 16 bytes.
    void Finish() {
        Write(Reg::Finalize, 0x12345678);
        if (words.size() % 4 != 0) {
            Write(Reg::Finalize, 0x12345678);
        }
    }

private:
    void Command(u32 reg, std::span<const u32> values, bool consecutive) {
        ASSERT(!values.empty() && values.size() <= 256);
        words.push_back(values[0]);
        words.push_back(reg | 0xfu << 16 | static_cast<u32>(values.size() - 1) << 20 |
                        (consecutive ? 1u << 31 : 0));
        words.insert(words.end(), values.begin() + 1, values.end());
        if (words.size() % 2 != 0) {
            words.push_back(0);
        }
    }

    std::vector<u32>& words;
};

/// The state every frame starts from: the buffers, the viewport and depth mapping, the vertex
/// shader and its outputs, the vertex layout.
void FrameState(Commands& c) {
    c.Write(Reg::StartDrawFunc0, 1);
    c.Write(Reg::FramebufferInvalidate, 1);
    // Colour and depth read and written (the material's depth mask decides the depth's write).
    c.Run(Reg::ColorBufferRead, std::array<u32, 4>{0xf, 0xf, 0x3, 0x3});
    c.Write(Reg::DepthBufferFormat, 3); // D24S8
    c.Write(Reg::ColorBufferFormat, 2); // RGBA8, 32 bits a pixel
    c.Write(Reg::FramebufferBlock32, 0);
    c.Run(Reg::DepthBufferLoc, std::array<u32, 3>{DepthAddress >> 3, ColorAddress >> 3,
                                                   240 | (400 - 1) << 12 | 1 << 24});
    c.Write(Reg::RenderBufDim, 240 | (400 - 1) << 12 | 1 << 24);
    c.Run(Reg::ViewportWidth, std::array<u32, 4>{F24(120.0f), F31(2.0f / 240.0f) << 1,
                                                  F24(200.0f), F31(2.0f / 400.0f) << 1});
    c.Write(Reg::ViewportXy, 0);
    c.Write(Reg::Scissor, 0);
    c.Write(Reg::ClipEnable, 0);
    // Depth: 0 at the near plane (the vertices' z 0), 1 at the far one (-w).
    c.Write(Reg::DepthMapEnable, 1);
    c.Write(Reg::DepthScale, F24(-1.0f));
    c.Write(Reg::DepthOffset, F24(0.0f));

    // The vertex shader, alone (no geometry shader).
    const auto code = PassThroughShader();
    c.Write(Reg::VshCodeTransferIndex, 0);
    c.Burst(Reg::VshCodeTransferData, code);
    c.Write(Reg::VshCodeTransferEnd, 1);
    c.Write(Reg::VshOpdescsIndex, 0);
    c.Burst(Reg::VshOpdescsData, std::array<u32, 1>{PassThroughSwizzle});
    c.Write(Reg::VshEntryPoint, 0x7fff0000);
    c.Write(Reg::VshBoolUniform, 0x7fff0000);
    c.Write(Reg::VshInputBufferConfig, 0xa0000000 | (ShaderOutputs - 1));
    c.Write(Reg::VshNumAttr, ShaderOutputs - 1);
    c.Write(Reg::VshAttributesPermutationLow, 0x06543210);
    c.Write(Reg::VshAttributesPermutationHigh, 0);
    c.Write(Reg::VshOutmapMask, (1 << ShaderOutputs) - 1);
    c.Write(Reg::VshComMode, 0);
    c.Write(Reg::GeostageConfig, 0);
    c.Write(Reg::GshMisc0, 0);
    c.Write(Reg::OutmapTotal, ShaderOutputs);
    c.Write(Reg::VshOutmapTotal1, ShaderOutputs - 1);
    c.Write(Reg::VshOutmapTotal2, ShaderOutputs - 1);
    c.Run(Reg::Outmap0, OutputMap);
    c.Write(Reg::OutattrMode, 1);
    // Position z, colour, texture coordinates 0-2, texture coordinate 0's w, quaternion/view.
    c.Write(Reg::OutattrClock, 1 << 0 | 1 << 1 | 7 << 8 | 1 << 16 | 1 << 24);
    // Triangle lists of the shader's outputs.
    c.Write(Reg::PrimitiveConfig, ShaderOutputs - 1);

    // One interleaved buffer of GpuVertex.
    c.Run(Reg::AttribBuffersLoc,
          std::array<u32, 3>{VertexAddress >> 3, AttributeFormats, (ShaderOutputs - 1) << 28});
    c.Run(Reg::AttribBuffer0Offset + 1,
          std::array<u32, 2>{0x06543210, VertexStride << 16 | ShaderOutputs << 28});
}

/// Whether one of the material's combiner stages takes texture unit `unit` as a source (3 to 5,
/// in the colour's or the alpha's three).
bool ReadsTexture(const Material& m, u32 unit) {
    for (const Material::Stage& stage : m.stages) {
        for (const u32 shift : {0u, 4u, 8u, 16u, 20u, 24u}) {
            if ((stage.source >> shift & 0xf) == 3 + unit) {
                return true;
            }
        }
    }
    return false;
}

/// A lighting table as the material uses it: its LUT id, its field's shift in LUTINPUT_SELECT,
/// _SCALE and _ABS (+1), its disable bit in LIGHTING_CONFIG1, and the table (none: off).
struct TableUse {
    u32 id;
    u32 shift;
    u32 disable_bit;
    std::optional<Banner::LutUse> use;
};

/// The tables a lit material uses: distribution 0 and 1, Fresnel, reflection blue, green and
/// red. Reflection's green and blue fall back to red's table.
std::array<TableUse, 6> TableUses(const Material& m) {
    const bool reflection = (m.frag_flags & 32) != 0 && m.luts[Material::ReflecR];
    const auto lut = [&m](Material::Lut l, bool on) {
        return on ? m.luts[l] : std::nullopt;
    };
    return {{
        {0, 0, 16, lut(Material::Dist0, (m.frag_flags & 2) != 0)},
        {1, 4, 17, lut(Material::Dist1, (m.frag_flags & 4) != 0)},
        {3, 12, 19, lut(Material::Fresnel, (m.fresnel & 3) != 0)},
        {4, 16, 22,
         reflection ? (m.luts[Material::ReflecB] ? m.luts[Material::ReflecB]
                                                 : m.luts[Material::ReflecR])
                    : std::nullopt},
        {5, 20, 21,
         reflection ? (m.luts[Material::ReflecG] ? m.luts[Material::ReflecG]
                                                 : m.luts[Material::ReflecR])
                    : std::nullopt},
        {6, 24, 20, lut(Material::ReflecR, reflection)},
    }};
}

/// A PICA texture format's bits a pixel; 0 for a format the PICA has none of.
u32 FormatBits(u32 format) {
    // RGBA8, RGB8, RGB5A1, RGB565, RGBA4, IA8, RG8, I8, A8, IA4, I4, A4, ETC1, ETC1A4.
    constexpr u32 bits[] = {32, 24, 16, 16, 16, 16, 16, 8, 8, 8, 4, 4, 4, 8};
    return format < std::size(bits) ? bits[format] : 0;
}

/// Whether the combiner stage passes the previous one's result on unchanged (the fragment
/// program generator leaves its other sources unread).
bool PassThrough(const Pica::TexturingRegs::TevStageConfig& stage) {
    using Stage = Pica::TexturingRegs::TevStageConfig;
    return stage.color_op == Stage::Operation::Replace &&
           stage.alpha_op == Stage::Operation::Replace &&
           stage.color_source1 == Stage::Source::Previous &&
           stage.alpha_source1 == Stage::Source::Previous &&
           stage.color_modifier1 == Stage::ColorModifier::SourceColor &&
           stage.alpha_modifier1 == Stage::AlphaModifier::SourceAlpha &&
           stage.GetColorMultiplier() == 1 && stage.GetAlphaMultiplier() == 1;
}

/// What of the scene the PICA (as azahar's GPU emulates it) cannot draw, empty when it can draw
/// all of it: a texture of a size, format or levels the PICA does not take, or with less data
/// than they need; a material whose state the fragment program generator refuses (a combiner
/// source the PICA has none of, min/max blending, a fragment mode other than the default, a
/// wrap mode it does not build, a border around a cube map); a cube map on a unit other than the
/// first; a texture or lighting table the material needs and the scene does not have.
std::string Undrawable(const Banner::Scene& scene) {
    for (const auto& [name, t] : scene.textures) {
        const u32 levels = std::max(t.levels, 1u);
        const u32 bits = FormatBits(t.format);
        if (bits == 0 || t.width < 8 || t.width > 1024 || t.width % 8 != 0 || t.height < 8 ||
            t.height > 1024 || t.height % 8 != 0 || levels > 8 ||
            (t.width >> (levels - 1)) < 8 || (t.height >> (levels - 1)) < 8) {
            return fmt::format("texture {}: {}x{}, format {}, {} levels", name, t.width, t.height,
                               t.format, levels);
        }
        u64 bytes = 0;
        for (u32 l = 0; l < levels; l++) {
            bytes += u64{t.width >> l} * (t.height >> l) * bits / 8;
        }
        if (bytes > t.face_bytes) {
            return fmt::format("texture {}: {} bytes of the {} it needs", name, t.face_bytes,
                               bytes);
        }
    }
    using Wrap = Pica::TexturingRegs::TextureConfig::WrapMode;
    for (const Banner::Model& model : scene.models) {
        for (const Material& m : model.materials) {
            if ((m.color_operation & 3) != 0) {
                return fmt::format("material {}: fragment mode {}", m.name,
                                   m.color_operation & 3);
            }
            // The blend equations (when blending): add, subtract or reverse subtract.
            if ((m.color_operation >> 8 & 1) != 0 &&
                ((m.blend_func & 7) > 2 || (m.blend_func >> 8 & 7) > 2)) {
                return fmt::format("material {}: blend {:#x}", m.name, m.blend_func);
            }
            for (u32 i = 0; i < m.stages.size(); i++) {
                Pica::TexturingRegs::TevStageConfig stage{};
                stage.sources_raw = m.stages[i].source;
                stage.modifiers_raw = m.stages[i].operand;
                stage.ops_raw = m.stages[i].combiner;
                stage.scales_raw = m.stages[i].scale;
                if (PassThrough(stage)) {
                    continue;
                }
                // Sources 7 to 12 are none.
                for (const u32 shift : {0u, 4u, 8u, 16u, 20u, 24u}) {
                    const u32 source = m.stages[i].source >> shift & 0xf;
                    if (source >= 7 && source <= 12) {
                        return fmt::format("material {}: combiner {} source {}", m.name, i,
                                           source);
                    }
                }
            }
            for (u32 i = 0; i < 3; i++) {
                if (!m.mappers[i]) {
                    continue;
                }
                const Banner::TextureMapper& tm = *m.mappers[i];
                bool cube = false;
                if (!tm.texture.empty()) {
                    const auto it = scene.textures.find(tm.texture);
                    if (it == scene.textures.end()) {
                        // The unit stays off (Render), which no combiner may read.
                        if (ReadsTexture(m, i)) {
                            return fmt::format("material {}: no texture {} for unit {}, which a "
                                               "combiner reads",
                                               m.name, tm.texture, i);
                        }
                        continue;
                    }
                    cube = it->second.cube;
                }
                // The wraps of s (bits 12-14) and t (8-10).
                for (const u32 shift : {12u, 8u}) {
                    const auto wrap = static_cast<Wrap>(tm.param >> shift & 7);
                    if (wrap == Wrap::ClampToEdge2 || wrap == Wrap::ClampToBorder2 ||
                        (cube && wrap == Wrap::ClampToBorder)) {
                        return fmt::format("material {}: unit {} wraps in mode {}{}", m.name, i,
                                           static_cast<u32>(wrap), cube ? " (a cube map)" : "");
                    }
                }
                if (cube && i != 0) {
                    return fmt::format("material {}: a cube map on texture unit {}", m.name, i);
                }
            }
            if ((m.flags & 1) == 0) {
                continue;
            }
            for (const TableUse& u : TableUses(m)) {
                if (!u.use) {
                    continue;
                }
                if (!scene.luts.contains({u.use->set, u.use->sampler})) {
                    return fmt::format("material {}: no lighting table {}/{}", m.name,
                                       u.use->set, u.use->sampler);
                }
                if (u.use->input > 5 || (u.use->scale > 3 && u.use->scale < 6)) {
                    return fmt::format("material {}: a table's input {} or scale {}", m.name,
                                       u.use->input, u.use->scale);
                }
            }
        }
    }
    return {};
}

} // Anonymous namespace

BannerPlayer::BannerPlayer(Core::System& system_, EmuWindowVita& window_)
    : system{system_}, window{window_} {}

BannerPlayer::~BannerPlayer() {
    Stop();
}

void BannerPlayer::Start(const Common::Rectangle<u32>& rect_) {
    if (running) {
        return;
    }
    const auto result = system.InitGpuOnly(window);
    ASSERT_MSG(result == Core::System::ResultStatus::Success, "banner: the GPU did not come up");
    rect = rect_;
    running = true;
    // The GPU's VRAM is new: the banner goes there again.
    uploaded = false;
    LOG_INFO(Frontend, "banner: the GPU is up");
}

void BannerPlayer::Stop() {
    if (!running) {
        return;
    }
    // A read still going finishes first: the title's load comes next.
    if (reader.joinable()) {
        reader.join();
        read = {};
    }
    jingle.StopAndWait();
    if (shown) {
        window.ShowMenuBanner(std::nullopt);
        shown = false;
    }
    system.ShutdownGpuOnly();
    SgxdPresent::SessionEnded();
    running = false;
    LOG_INFO(Frontend, "banner: the GPU is down");
}

BannerPlayer::Loaded BannerPlayer::Read(const std::string& title) {
    const auto started = std::chrono::steady_clock::now();
    Loaded banner;
    banner.title = title;
    std::vector<u8> data;
    const std::unique_ptr<Loader::AppLoader> loader = Loader::GetLoader(title);
    if (!loader || loader->ReadBanner(data) != Loader::ResultStatus::Success) {
        LOG_INFO(Frontend, "banner: {} has none", title);
        return banner;
    }
    // The language scene the HOME Menu of the console's region shows (North America's for a
    // title it may run on, else Europe's, else Japan's), in English or Japanese; without one the
    // common scene alone, as on a console of that region.
    std::vector<Banner::BannerLanguage> languages;
    std::vector<u8> smdh;
    if (loader->ReadIcon(smdh) == Loader::ResultStatus::Success && Loader::IsValidSMDH(smdh)) {
        Loader::SMDH data;
        std::memcpy(&data, smdh.data(), sizeof(data));
        using Region = Loader::SMDH::GameRegion;
        const std::vector<Region> regions = data.GetRegions();
        const auto has = [&regions](Region r) {
            return std::find(regions.begin(), regions.end(), r) != regions.end();
        };
        if (has(Region::NorthAmerica)) {
            languages.push_back(Banner::BannerLanguage::AmericanEnglish);
        } else if (has(Region::Europe) || has(Region::Australia)) {
            languages.push_back(Banner::BannerLanguage::EuropeanEnglish);
        } else if (has(Region::Japan)) {
            languages.push_back(Banner::BannerLanguage::Japanese);
        }
    }
    // A banner this cannot draw is not shown (the panel as for a title without one).
    std::string error;
    std::optional<Banner::Scene> parsed = Banner::ReadBanner(data, languages, error);
    if (!parsed) {
        LOG_WARNING(Frontend, "banner: {}'s is not shown: {}", title, error);
        return banner;
    }
    auto scene = std::make_unique<Banner::Scene>(std::move(*parsed));
    auto animator = std::make_unique<Banner::SceneAnimator>(*scene);
    error = animator->Error();
    if (error.empty()) {
        error = Undrawable(*scene);
    }
    // The textures' places in VRAM, after the white one.
    u32 used = WhiteBytes;
    for (const auto& [name, t] : scene->textures) {
        if (!error.empty()) {
            break;
        }
        used = Common::AlignUp(used, 128);
        if (used + t.data.size() > TextureBytes) {
            error = fmt::format("its textures take more than {} bytes", TextureBytes);
            break;
        }
        banner.textures.emplace(name, TextureAddress + used);
        used += static_cast<u32>(t.data.size());
    }
    if (!error.empty()) {
        LOG_WARNING(Frontend, "banner: {}'s is not shown: {}", title, error);
        banner.textures.clear();
        return banner;
    }
    banner.texture_bytes = used;
    banner.jingle_pcm = Banner::DecodeJingle(Banner::BannerJingle(data));
    LOG_INFO(Frontend, "banner: {}: {} models, {} textures, read in {} ms", title,
             scene->models.size(), scene->textures.size(),
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - started)
                 .count());
    banner.scene = std::move(scene);
    banner.animator = std::move(animator);
    return banner;
}

void BannerPlayer::Install(Loaded&& banner) {
    loaded = std::move(banner.title);
    jingle.Stop();
    // The animator before its scene: it points into it.
    animator.reset();
    scene = std::move(banner.scene);
    animator = std::move(banner.animator);
    textures = std::move(banner.textures);
    texture_bytes = banner.texture_bytes;
    jingle_pcm = std::move(banner.jingle_pcm);
    uploaded = false;
    frame = 0;
}

void BannerPlayer::TakeRead() {
    if (!reader.joinable() || !read_done.load(std::memory_order_acquire)) {
        return;
    }
    reader.join();
    Loaded banner = std::exchange(read, {});
    // The selection moved on meanwhile: the title now wanted is read next.
    if (banner.title == wanted) {
        Install(std::move(banner));
    }
}

void BannerPlayer::Upload() {
    // In the host frame: the white texture and the scene's into VRAM, where Load placed them, as
    // the PICA reads them.
    Memory::MemorySystem& memory = system.Memory();
    u8* const base = memory.GetPhysicalPointer(TextureAddress);
    std::memset(base, 0xff, WhiteBytes);
    for (const auto& [name, t] : scene->textures) {
        std::memcpy(base + (textures.at(name) - TextureAddress), t.data.data(), t.data.size());
    }
    system.GPU().HostWrote(TextureAddress, texture_bytes);
    uploaded = true;
}

void BannerPlayer::Frame(const std::string& title, u32 backdrop) {
    if (!running) {
        return;
    }
    if (title != wanted) {
        wanted = title;
        wanted_frames = 0;
    }
    TakeRead();
    // Scrolling past titles reads none of them: only one the selection rests on, one at a time.
    if (wanted != loaded && wanted_frames < LoadDelayFrames) {
        wanted_frames++;
    }
    if (wanted != loaded && wanted_frames >= LoadDelayFrames && !reader.joinable()) {
        if (wanted.empty()) {
            Install({});
        } else {
            read_done.store(false, std::memory_order_relaxed);
            // On the cores the game list leaves idle.
            reader = Common::NamedThread{
                Common::ThreadCfg{"banner read", 1024 * 1024, 1 << 0 | 1 << 1 | 1 << 3},
                [this, title = wanted] {
                    read = Read(title);
                    read_done.store(true, std::memory_order_release);
                }};
        }
    }
    // The panel shows a banner only while the selection rests on the title it belongs to.
    if (!scene || wanted != loaded) {
        jingle.Stop();
        if (shown) {
            window.ShowMenuBanner(std::nullopt);
            shown = false;
        }
        return;
    }
    // The panel first: the host frame's present then only stores the new picture for the
    // menus' present to draw.
    if (!shown) {
        window.ShowMenuBanner(rect);
        shown = true;
    }
    VideoCore::GPU& gpu = system.GPU();
    bool drawn = false;
    gpu.HostFrame([&] {
        if (!uploaded) {
            Upload();
        }
        drawn = Render(backdrop);
    });
    if (!drawn) {
        // A frame past the banner's room in VRAM: the title shows without it from here on.
        jingle.Stop();
        jingle_pcm.clear();
        animator.reset();
        scene.reset();
        textures.clear();
        window.ShowMenuBanner(std::nullopt);
        shown = false;
        return;
    }
    if (!jingle_pcm.empty()) {
        jingle.Play(std::move(jingle_pcm));
        jingle_pcm.clear();
    }
}

bool BannerPlayer::Render(u32 backdrop) {
    animator->Evaluate(frame++, scratch);
    VideoCore::GPU& gpu = system.GPU();
    Memory::MemorySystem& memory = system.Memory();

    const u64 vertex_bytes = u64{scratch.vertices.size()} * VertexStride;
    const u64 index_bytes = u64{scratch.indices.size()} * sizeof(u16);
    if (Common::AlignUp(vertex_bytes, 16) + index_bytes > VertexBytes) {
        LOG_WARNING(Frontend, "banner: {}'s frame has {} vertices and {} indices, past {} bytes",
                    loaded, scratch.vertices.size(), scratch.indices.size(), VertexBytes);
        return false;
    }
    const u32 index_offset = Common::AlignUp(static_cast<u32>(vertex_bytes), 16);
    u8* const vertices = memory.GetPhysicalPointer(VertexAddress);
    std::memcpy(vertices, scratch.vertices.data(), vertex_bytes);
    std::memcpy(vertices + index_offset, scratch.indices.data(), index_bytes);

    Commands c{commands};
    FrameState(c);
    // The lighting tables each slot holds in this list.
    std::array<const Banner::LutTable*, 7> lut_slots{};
    for (const Banner::Draw& draw : scratch.draws) {
        const Material& m = *draw.material;
        c.Write(Reg::FaceCulling, m.cull);

        // Fragment operations, as the material sets them; colour always written, the depth
        // tested and written as its flags say.
        c.Run(Reg::ColorOperation,
              std::array<u32, 7>{m.color_operation, m.blend_func, m.logic_op, m.blend_color,
                                 m.alpha_test, m.stencil_test, m.stencil_op});
        c.Write(Reg::DepthColorMask, (m.depth_flags & 1) | (m.depth & 0x70) | 0xf00 |
                                         ((m.depth_flags & 2) != 0 ? 1u << 12 : 0));

        // Texture units: each mapper's texture, or the white one.
        u32 units = 0;
        for (u32 i = 0; i < 3; i++) {
            if (!m.mappers[i]) {
                continue;
            }
            const Banner::TextureMapper& tm = *m.mappers[i];
            PAddr address = TextureAddress;
            u32 width = 8, height = 8, levels = 1, format = FormatRgba8;
            bool cube = false;
            u32 face_bytes = 0;
            if (!tm.texture.empty()) {
                const auto it = scene->textures.find(tm.texture);
                if (it == scene->textures.end()) {
                    // A name no texture of the scene has: the unit stays off, which no
                    // combiner stage reads (Undrawable).
                    continue;
                }
                address = textures.at(tm.texture);
                width = it->second.width, height = it->second.height;
                levels = std::max(it->second.levels, 1u), format = it->second.format;
                cube = it->second.cube;
                face_bytes = it->second.face_bytes;
            }
            units |= 1 << i;
            const u32 base = i == 0 ? Reg::TexUnit0 : i == 1 ? Reg::TexUnit1 : Reg::TexUnit2;
            const u32 param = (tm.param & ~(7u << 28)) | (cube ? 1u << 28 : 0);
            const u32 lod = (tm.lod & 0x1fff) | (levels - 1) << 16;
            c.Run(base, std::array<u32, 5>{tm.border, height | width << 16, param, lod,
                                            address >> 3});
            if (cube) {
                std::array<u32, 5> faces;
                for (u32 f = 0; f < 5; f++) {
                    faces[f] = (address + face_bytes * (f + 1)) >> 3 & 0x3fffff;
                }
                c.Run(Reg::CubeAddress, faces);
            }
            c.Write(i == 0 ? Reg::TexUnit0Type : i == 1 ? Reg::TexUnit1Type : Reg::TexUnit2Type,
                    format);
        }
        c.Write(Reg::TexUnitConfig, units | 1 << 12 | 1 << 16);

        // The texture combiners, their constants from the material's colours at this frame.
        std::array<std::array<u32, 5>, 6> stages;
        for (u32 i = 0; i < 6; i++) {
            const Material::Stage& s = m.stages[i];
            const u32 constant = s.constant < 6 ? 5 + s.constant : s.constant - 6;
            stages[i] = {s.source, s.operand, s.combiner, Rgba8(draw.colors[constant]), s.scale};
        }
        for (u32 i = 0; i < 6; i++) {
            c.Run(i < 4 ? Reg::TexEnv0 + 8 * i : Reg::TexEnv4 + 8 * (i - 4), stages[i]);
        }
        // No fog.
        c.Write(Reg::TexEnvUpdateBuffer, m.update_buffer & ~7u);
        c.Write(Reg::TexEnvBufferColor, m.buffer_color);

        // Fragment lighting.
        const bool lit = (m.flags & 1) != 0;
        c.Write(Reg::LightingEnable0, lit ? 1 : 0);
        c.Write(Reg::LightingEnable1, lit ? 0 : 1);
        if (lit) {
            // Each light's config: directional (bit 0), one-sided, and the material's geometric
            // factors 0 and 1 (bits 2 and 3, from its flags 8 and 16).
            const u32 geometric = ((m.frag_flags & 8) != 0 ? 4u : 0) |
                                  ((m.frag_flags & 16) != 0 ? 8u : 0);
            for (u32 i = 0; i < draw.lights.size(); i++) {
                const Banner::DrawLight& l = draw.lights[i];
                const u32 at = Reg::Light0 + 0x10 * i;
                // A direction towards the light, or a point light's position.
                c.Run(at, std::array<u32, 6>{LightColor(l.specular0), LightColor(l.specular1),
                                              LightColor(l.diffuse), LightColor(l.ambient),
                                              F16(l.direction.x) | F16(l.direction.y) << 16,
                                              F16(l.direction.z)});
                c.Write(at + 9, (l.point ? 0u : 1u) | geometric);
            }
            c.Write(Reg::LightingAmbient, LightColor(draw.global_ambient));
            c.Write(Reg::LightingNumLights, static_cast<u32>(draw.lights.size()) - 1);
            // The tables the material uses (Undrawable: the scene has each, its input and
            // scale the PICA's).
            const std::array<TableUse, 6> uses = TableUses(m);
            // Shadow, spotlight and distance attenuation off for every light.
            u32 config1 = 0xff | 0xff << 8 | 0xffu << 24;
            u32 abs = 0, select = 0, scale = 0;
            for (const TableUse& u : uses) {
                if (!u.use) {
                    config1 |= 1u << u.disable_bit;
                    continue;
                }
                const auto it = scene->luts.find({u.use->set, u.use->sampler});
                if (!it->second.absolute) {
                    abs |= 2u << u.shift;
                }
                select |= u.use->input << u.shift;
                scale |= u.use->scale << u.shift;
                if (lut_slots[u.id] != &it->second) {
                    lut_slots[u.id] = &it->second;
                    c.Write(Reg::LightingLutIndex, u.id << 8);
                    c.Burst(Reg::LightingLutData, it->second.entries);
                }
            }
            const bool fresnel = uses[2].use.has_value();
            // The first configuration with every table the material uses, the cheapest first
            // (as the CTR SDK picks one); the Fresnel table into the alphas it selects.
            using LightingRegs = Pica::LightingRegs;
            constexpr LightingRegs::LightingConfig configs[] = {
                LightingRegs::LightingConfig::Config0, LightingRegs::LightingConfig::Config1,
                LightingRegs::LightingConfig::Config2, LightingRegs::LightingConfig::Config3,
                LightingRegs::LightingConfig::Config4, LightingRegs::LightingConfig::Config5,
                LightingRegs::LightingConfig::Config6, LightingRegs::LightingConfig::Config7};
            const auto config = *std::find_if(std::begin(configs), std::end(configs), [&](auto c) {
                return std::all_of(uses.begin(), uses.end(), [c](const TableUse& u) {
                    return !u.use || LightingRegs::IsLightingSamplerSupported(
                                         c, static_cast<LightingRegs::LightingSampler>(u.id));
                });
            });
            const u32 config0 = static_cast<u32>(config) << 4 |
                                (fresnel ? (m.fresnel & 3) << 2 : 0) |
                                ((m.frag_flags & 1) != 0 ? 1u << 27 : 0);
            c.Write(Reg::LightingConfig0, config0);
            c.Write(Reg::LightingConfig1, config1);
            c.Run(Reg::LightingLutAbs, std::array<u32, 3>{abs, select, scale});
            c.Write(Reg::LightingPermutation, 0x76543210);
        }

        // The draw: its vertices from their place in the frame's buffer, its 16-bit indices.
        c.Write(Reg::AttribBuffer0Offset, draw.first_vertex * VertexStride);
        c.Write(Reg::RestartPrimitive, 1);
        c.Write(Reg::IndexBufferConfig, (index_offset + draw.first_index * 2) | 1u << 31);
        c.Write(Reg::NumVertices, draw.index_count);
        c.Write(Reg::VertexOffset, 0);
        c.Write(Reg::GeostageConfig, 0x100, 0x2);
        c.Write(Reg::GeostageConfig2, 0x100, 0x2);
        c.Write(Reg::StartDrawFunc0, 0);
        c.Write(Reg::DrawElements, 1);
        c.Write(Reg::StartDrawFunc0, 1);
        c.Write(Reg::VtxFunc, 1);
        c.Write(Reg::GeostageConfig, 0, 0x2);
        c.Write(Reg::GeostageConfig2, 0, 0x2);
    }
    c.Write(Reg::FramebufferFlush, 1);
    c.Write(Reg::FramebufferInvalidate, 1);
    c.Finish();
    const u32 list_bytes = static_cast<u32>(commands.size() * sizeof(u32));
    if (list_bytes > CommandBytes) {
        LOG_WARNING(Frontend, "banner: {}'s command list takes {} bytes, past {}", loaded,
                    list_bytes, CommandBytes);
        return false;
    }
    std::memcpy(memory.GetPhysicalPointer(CommandAddress), commands.data(), list_bytes);

    // The buffers cleared: the colour to the panel's backdrop, the depth to the far plane.
    Service::GSP::Command fill{};
    fill.id.Assign(Service::GSP::CommandId::MemoryFill);
    fill.memory_fill.start1 = ColorAddress;
    fill.memory_fill.end1 = ColorAddress + BufferBytes;
    fill.memory_fill.value1 = backdrop << 8 | 0xff; // RGBA8 in memory: A, B, G, R
    fill.memory_fill.control1 = 0x201;              // 32-bit values, started
    fill.memory_fill.start2 = DepthAddress;
    fill.memory_fill.end2 = DepthAddress + BufferBytes;
    fill.memory_fill.value2 = 0x00ffffff; // depth 1, stencil 0
    fill.memory_fill.control2 = 0x201;
    gpu.ExecutePhysical(fill);

    Service::GSP::Command list{};
    list.id.Assign(Service::GSP::CommandId::SubmitCmdList);
    list.submit_gpu_cmdlist.address = CommandAddress;
    list.submit_gpu_cmdlist.size = list_bytes;
    gpu.ExecutePhysical(list);

    // The colour buffer (tiled) to the top screen's framebuffer (linear), both RGBA8.
    Service::GSP::Command transfer{};
    transfer.id.Assign(Service::GSP::CommandId::DisplayTransfer);
    transfer.display_transfer.in_buffer_address = ColorAddress;
    transfer.display_transfer.out_buffer_address = ScreenAddress;
    transfer.display_transfer.in_buffer_size = 400 << 16 | 240;
    transfer.display_transfer.out_buffer_size = 400 << 16 | 240;
    transfer.display_transfer.flags = 0;
    gpu.ExecutePhysical(transfer);
    gpu.SetHostTopScreen(ScreenAddress, FormatRgba8, ScreenColumnBytes);
    return true;
}

} // namespace VitaFrontend
