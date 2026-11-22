// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <fmt/format.h>
#include "common/assert.h"
#include "video_core/renderer_sgxd/usse/usse_program.h"

namespace SgxdRenderer::Usse {

uint32_t VaryingDescriptor(const Program& program, size_t index) {
    const Varying& input = program.inputs[index];
    // 0x0c00000f: constant bits and "no texture query"; the input id in bits 12-15; the
    // component count minus one in bits 22-23; COLOR0 marked float (the default type of a
    // colour iterator is otherwise fixed-point); bit 25 on the last descriptor.
    uint32_t info = 0x0c00000f | (static_cast<uint32_t>(input.input) << 12) |
                    (static_cast<uint32_t>(input.components - 1) << 22);
    if (input.half) {
        // USEFORMAT F16 (bits 29..28 = 2), a colour too: psp2cgc's `half4 TEXCOORD4` is
        // 2ec0400f and its `half4 COLOR0` 2cc0a00f.
        info |= 0x20000000;
    } else if (!input.fixed &&
               (input.input == FragmentInput::Color0 || input.input == FragmentInput::Color1)) {
        info |= 0x00100000;
    }
    if (index + 1 == program.inputs.size()) {
        info |= 0x02000000;
    }
    return info;
}

std::string UniformName(const Uniform& uniform) {
    const auto light = [&](const char* member) {
        return "light_src[" + std::to_string(uniform.index) + "]." + member;
    };
    switch (uniform.kind) {
    case UniformKind::FlipViewport:
        return "flip_viewport";
    case UniformKind::DepthScale:
        return "depth_scale";
    case UniformKind::DepthOffset:
        return "depth_offset";
    case UniformKind::ClipZSign:
        return "clip_z_sign";
    case UniformKind::DefaultInput:
        return "vs_in_reg" + std::to_string(uniform.index);
    case UniformKind::IntUniforms:
        return "uniforms_i";
    case UniformKind::FloatBank:
        return "uniforms_f";
    case UniformKind::AlphaTestRef:
        return "alphatest_ref";
    case UniformKind::Dot3Const:
        return "dot3_const[" + std::to_string(uniform.index) + "]";
    case UniformKind::ConstColor:
        return "const_color";
    case UniformKind::BufferColor:
        return "tev_combiner_buffer_color";
    case UniformKind::ScissorX1:
        return "scissor_x1";
    case UniformKind::ScissorY1:
        return "scissor_y1";
    case UniformKind::ScissorX2:
        return "scissor_x2";
    case UniformKind::ScissorY2:
        return "scissor_y2";
    case UniformKind::FogLutOffset:
        return "fog_lut_offset";
    case UniformKind::FogColor:
        return "fog_color";
    case UniformKind::TexBorderColor:
        return "tex_border_color";
    case UniformKind::ProctexLutOffset:
        return "proctex_lut_offset";
    case UniformKind::ProctexDiffLutOffset:
        return "proctex_diff_lut_offset";
    case UniformKind::ProctexColorMapOffset:
        return "proctex_color_map_offset";
    case UniformKind::ProctexAlphaMapOffset:
        return "proctex_alpha_map_offset";
    case UniformKind::ProctexBias:
        return "proctex_bias";
    case UniformKind::ProctexNoiseLutOffset:
        return "proctex_noise_lut_offset";
    case UniformKind::ProctexNoiseF:
        return "proctex_noise_f";
    case UniformKind::ProctexNoiseA:
        return "proctex_noise_a";
    case UniformKind::ProctexNoiseP:
        return "proctex_noise_p";
    case UniformKind::LightingLutOffset:
        return "lighting_lut_offset";
    case UniformKind::LightingLutRow:
        return "lighting_lut_row";
    case UniformKind::LightingGlobalAmbient:
        return "lighting_global_ambient";
    case UniformKind::LightSpecular0:
        return light("specular_0");
    case UniformKind::LightSpecular1:
        return light("specular_1");
    case UniformKind::LightDiffuse:
        return light("diffuse");
    case UniformKind::LightAmbient:
        return light("ambient");
    case UniformKind::LightPosition:
        return light("position");
    case UniformKind::LightDirection:
        return "normalize(" + light("position") + ")";
    case UniformKind::LightSpotDirection:
        return light("spot_direction");
    case UniformKind::LightDistAttenBias:
        return light("dist_atten_bias");
    case UniformKind::LightDistAttenScale:
        return light("dist_atten_scale");
    }
    UNREACHABLE();
}

std::string SamplerName(const Sampler& sampler) {
    switch (sampler.kind) {
    case SamplerKind::Texture:
        return "tex" + std::to_string(sampler.index);
    case SamplerKind::TextureAlpha:
        return "tex" + std::to_string(sampler.index) + "a";
    case SamplerKind::LutLf:
        return "texture_buffer_lut_lf";
    case SamplerKind::LutRg:
        return "texture_buffer_lut_rg";
    case SamplerKind::LutRgba:
        return "texture_buffer_lut_rgba";
    case SamplerKind::ProctexNoise:
        return "proctex_noise";
    }
    UNREACHABLE();
}

std::string AttributeName(const Attribute& attribute) {
    switch (attribute.kind) {
    case AttributeKind::Input:
        return "vs_in_attr" + std::to_string(attribute.index);
    case AttributeKind::OutAddress:
        return "vs_out_address";
    case AttributeKind::GsInRows:
        return "gs_in_rows";
    }
    UNREACHABLE();
}

std::string ProgramJson(const Program& p) {
    std::string out;
    auto it = std::back_inserter(out);
    const auto list = [&](const char* name, std::size_t count, const auto& item) {
        fmt::format_to(it, R"(,"{}":[)", name);
        for (std::size_t i = 0; i < count; i++) {
            fmt::format_to(it, "{}", i ? "," : "");
            item(i);
        }
        fmt::format_to(it, "]");
    };
    fmt::format_to(it,
                   R"({{"vertex":{},"per_instance":{},"uses_discard":{},)"
                   R"("pa_count":{},"temp_count":{},"uniform_floats":{},"buffer_pointer":{},)"
                   R"("ldst_base_value":{},"buffer_floats":{},"secondary_reg_count":{},)"
                   R"("outselects":{},"texsize":{},"peak_live":{})",
                   p.vertex, p.per_instance, p.uses_discard, p.pa_count,
                   p.temp_count, p.uniform_floats, p.buffer_pointer, p.ldst_base_value,
                   p.buffer_floats, p.secondary_reg_count, p.outselects, p.texsize, p.peak_live);
    fmt::format_to(it, R"(,"peak_at":{})", p.peak_at);
    list("peak_regs", p.peak_regs.size(),
         [&](std::size_t i) { fmt::format_to(it, "{}", p.peak_regs[i]); });
    list("literals", p.literals.size(),
         [&](std::size_t i) { fmt::format_to(it, "{}", p.literals[i]); });
    list("phase_starts", p.phase_starts.size(),
         [&](std::size_t i) { fmt::format_to(it, "{}", p.phase_starts[i]); });
    list("primary", p.primary.size(),
         [&](std::size_t i) { fmt::format_to(it, R"("{:016x}")", p.primary[i]); });
    list("secondary", p.secondary.size(),
         [&](std::size_t i) { fmt::format_to(it, R"("{:016x}")", p.secondary[i]); });
    list("inputs", p.inputs.size(), [&](std::size_t i) {
        fmt::format_to(it, R"({{"input":{},"components":{},"half":{}}})",
                       static_cast<uint32_t>(p.inputs[i].input), p.inputs[i].components,
                       p.inputs[i].half ? "true" : "false");
    });
    list("uniforms", p.uniforms.size(), [&](std::size_t i) {
        const Uniform& u = p.uniforms[i];
        fmt::format_to(it, R"({{"name":"{}","sa":{},"components":{},"array_size":{},"u8":{}}})",
                       UniformName(u), u.sa_offset, u.components, u.array_size, u.u8);
    });
    list("samplers", p.samplers.size(), [&](std::size_t i) {
        fmt::format_to(it, R"({{"name":"{}","unit":{}}})", SamplerName(p.samplers[i]),
                       p.samplers[i].unit);
    });
    list("attributes", p.attributes.size(), [&](std::size_t i) {
        const Attribute& a = p.attributes[i];
        fmt::format_to(it, R"({{"name":"{}","resource":{},"words":{}}})", AttributeName(a),
                       a.resource, a.words);
    });
    list("prefetched_uniforms", p.prefetched_uniforms.size(), [&](std::size_t i) {
        fmt::format_to(it, "[{},{}]", p.prefetched_uniforms[i].first,
                       p.prefetched_uniforms[i].second);
    });
    fmt::format_to(it, "}}\n");
    return out;
}

} // namespace SgxdRenderer::Usse
