// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cstring>
#include <type_traits>
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
    case UniformKind::PositionScale:
        return "position_scale";
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
    case UniformKind::ShadowBiasConstant:
        return "shadow_bias_constant";
    case UniformKind::ShadowBiasLinear:
        return "shadow_bias_linear";
    case UniformKind::ShadowTextureBias:
        return "shadow_texture_bias";
    case UniformKind::ShadowBorderDepth:
        return "shadow_border_depth";
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
    case UniformKind::LightingLutCoordHalf:
        return "lighting_lut_coord_half";
    case UniformKind::LightingLutCoordWrap:
        return "lighting_lut_coord_wrap";
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
    case SamplerKind::LutLfHalf:
        return "texture_buffer_lut_lf_half";
    case SamplerKind::LutLfHalfWrap:
        return "texture_buffer_lut_lf_half_wrap";
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
        fmt::format_to(it,
                       R"({{"name":"{}","sa":{},"components":{},"array_size":{},"u8":{},"f16":{}}})",
                       UniformName(u), u.sa_offset, u.components, u.array_size, u.u8, u.f16);
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

namespace {

/// SerializeProgram's bytes: scalars as they lie in memory, vectors as a count then elements.
class Writer {
public:
    template <typename T>
        requires std::is_arithmetic_v<T> || std::is_enum_v<T>
    void Put(T value) {
        const auto* p = reinterpret_cast<const uint8_t*>(&value);
        out.insert(out.end(), p, p + sizeof(T));
    }
    template <typename T, typename F>
    void PutVector(const std::vector<T>& values, F&& put) {
        Put(static_cast<uint32_t>(values.size()));
        for (const T& value : values) {
            put(value);
        }
    }
    std::vector<uint8_t> out;
};

class Reader {
public:
    Reader(const uint8_t* data_, size_t size_) : data{data_}, size{size_} {}
    template <typename T>
        requires std::is_arithmetic_v<T> || std::is_enum_v<T>
    void Get(T& value) {
        if (at + sizeof(T) > size) {
            ok = false;
            value = T{};
            return;
        }
        std::memcpy(&value, data + at, sizeof(T));
        at += sizeof(T);
    }
    template <typename T, typename F>
    void GetVector(std::vector<T>& values, F&& get) {
        uint32_t count = 0;
        Get(count);
        // No more elements than bytes left: a damaged count reads no further.
        if (!ok || count > size - at) {
            ok = false;
            return;
        }
        values.resize(count);
        for (T& value : values) {
            get(value);
        }
    }
    [[nodiscard]] bool Done() const {
        return ok && at == size;
    }

private:
    const uint8_t* data;
    size_t size;
    size_t at = 0;
    bool ok = true;
};

} // namespace

std::vector<uint8_t> SerializeProgram(const Program& p) {
    Writer w;
    w.PutVector(p.inputs, [&](const Varying& v) {
        w.Put(v.input);
        w.Put(v.components);
        w.Put(v.half);
        w.Put(v.fixed);
    });
    w.PutVector(p.issues, [&](const TextureIssue& t) {
        w.Put(t.unit);
        w.Put(t.coord);
        w.Put(t.pa);
        w.Put(t.after_input);
        w.Put(t.f16);
        w.Put(t.fog);
        w.Put(t.alpha);
        w.Put(t.raw);
        w.Put(t.proctex);
    });
    w.PutVector(p.uniforms, [&](const Uniform& u) {
        w.Put(u.kind);
        w.Put(u.index);
        w.Put(u.sa_offset);
        w.Put(u.components);
        w.Put(u.array_size);
        w.Put(u.u8);
        w.Put(u.f16);
        w.Put(u.lane);
    });
    w.PutVector(p.samplers, [&](const Sampler& s) {
        w.Put(s.kind);
        w.Put(s.index);
        w.Put(s.unit);
    });
    w.Put(p.pa_count);
    w.Put(p.temp_count);
    w.Put(p.peak_live);
    w.Put(p.uniform_floats);
    w.PutVector(p.literals, [&](uint32_t l) { w.Put(l); });
    w.Put(p.uses_discard);
    w.Put(p.output_u8);
    w.PutVector(p.phase_starts, [&](uint32_t start) { w.Put(start); });
    w.PutVector(p.primary, [&](uint64_t word) { w.Put(word); });
    w.PutVector(p.secondary, [&](uint64_t word) { w.Put(word); });
    w.Put(p.vertex);
    w.Put(p.gs_temps_read_first);
    w.Put(p.per_instance);
    w.PutVector(p.attributes, [&](const Attribute& a) {
        w.Put(a.kind);
        w.Put(a.index);
        w.Put(a.resource);
        w.Put(a.words);
    });
    w.Put(p.outselects);
    w.Put(p.texsize);
    for (const Program::PositionRow& row : p.position_rows) {
        w.Put(row.uniform);
        for (int c = 0; c < 4; c++) {
            w.Put(row.uniform_lane[c]);
            w.Put(row.source_lane[c]);
        }
        w.Put(row.negate);
    }
    w.Put(p.buffer_pointer);
    w.Put(p.ldst_base_value);
    w.Put(p.buffer_floats);
    w.Put(p.secondary_reg_count);
    w.PutVector(p.prefetched_uniforms, [&](const std::pair<uint32_t, uint32_t>& u) {
        w.Put(u.first);
        w.Put(u.second);
    });
    return std::move(w.out);
}

bool DeserializeProgram(const uint8_t* data, size_t size, Program& p) {
    Reader r{data, size};
    p = Program{};
    r.GetVector(p.inputs, [&](Varying& v) {
        r.Get(v.input);
        r.Get(v.components);
        r.Get(v.half);
        r.Get(v.fixed);
    });
    r.GetVector(p.issues, [&](TextureIssue& t) {
        r.Get(t.unit);
        r.Get(t.coord);
        r.Get(t.pa);
        r.Get(t.after_input);
        r.Get(t.f16);
        r.Get(t.fog);
        r.Get(t.alpha);
        r.Get(t.raw);
        r.Get(t.proctex);
    });
    r.GetVector(p.uniforms, [&](Uniform& u) {
        r.Get(u.kind);
        r.Get(u.index);
        r.Get(u.sa_offset);
        r.Get(u.components);
        r.Get(u.array_size);
        r.Get(u.u8);
        r.Get(u.f16);
        r.Get(u.lane);
    });
    r.GetVector(p.samplers, [&](Sampler& s) {
        r.Get(s.kind);
        r.Get(s.index);
        r.Get(s.unit);
    });
    r.Get(p.pa_count);
    r.Get(p.temp_count);
    r.Get(p.peak_live);
    r.Get(p.uniform_floats);
    r.GetVector(p.literals, [&](uint32_t& l) { r.Get(l); });
    r.Get(p.uses_discard);
    r.Get(p.output_u8);
    r.GetVector(p.phase_starts, [&](uint32_t& start) { r.Get(start); });
    r.GetVector(p.primary, [&](uint64_t& word) { r.Get(word); });
    r.GetVector(p.secondary, [&](uint64_t& word) { r.Get(word); });
    r.Get(p.vertex);
    r.Get(p.gs_temps_read_first);
    r.Get(p.per_instance);
    r.GetVector(p.attributes, [&](Attribute& a) {
        r.Get(a.kind);
        r.Get(a.index);
        r.Get(a.resource);
        r.Get(a.words);
    });
    r.Get(p.outselects);
    r.Get(p.texsize);
    for (Program::PositionRow& row : p.position_rows) {
        r.Get(row.uniform);
        for (int c = 0; c < 4; c++) {
            r.Get(row.uniform_lane[c]);
            r.Get(row.source_lane[c]);
        }
        r.Get(row.negate);
    }
    r.Get(p.buffer_pointer);
    r.Get(p.ldst_base_value);
    r.Get(p.buffer_floats);
    r.Get(p.secondary_reg_count);
    r.GetVector(p.prefetched_uniforms, [&](std::pair<uint32_t, uint32_t>& u) {
        r.Get(u.first);
        r.Get(u.second);
    });
    return r.Done();
}

} // namespace SgxdRenderer::Usse
