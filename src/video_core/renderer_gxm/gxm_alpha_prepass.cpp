// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cstring>
#include "video_core/renderer_gxm/gxm_alpha_prepass.h"

namespace GxmRenderer {

namespace {
using Pica::FramebufferRegs;
using TevStageConfig = Pica::TexturingRegs::TevStageConfig;
using Source = TevStageConfig::Source;
using Operation = TevStageConfig::Operation;
using ColorModifier = TevStageConfig::ColorModifier;
using AlphaModifier = TevStageConfig::AlphaModifier;

// The generator's notion of a stage that changes nothing (fs_usse_gen.cpp has the same).
bool PassThrough(const TevStageConfig& stage) {
    return stage.color_op == Operation::Replace && stage.alpha_op == Operation::Replace &&
           stage.color_source1 == Source::Previous && stage.alpha_source1 == Source::Previous &&
           stage.color_modifier1 == ColorModifier::SourceColor &&
           stage.alpha_modifier1 == AlphaModifier::SourceAlpha &&
           stage.GetColorMultiplier() == 1 && stage.GetAlphaMultiplier() == 1;
}

u32 LastActiveStage(const Pica::Shader::FSConfig& config) {
    u32 last = 0;
    for (u32 i = 0; i < config.texture.tev_stages.size(); i++) {
        if (!PassThrough(config.texture.tev_stages[i])) {
            last = i;
        }
    }
    return last;
}
} // Anonymous namespace

int AlphaChainClass(const Pica::Shader::FSConfig& config) {
    const auto func = config.framebuffer.alpha_test_func.Value();
    if (func == FramebufferRegs::CompareFunc::Always || func == FramebufferRegs::CompareFunc::Never) {
        return 0;
    }
    if (config.framebuffer.depthmap_enable == Pica::RasterizerRegs::DepthBuffering::WBuffering) {
        return 3;
    }
    const u32 last = LastActiveStage(config);
    for (u32 i = 0; i <= last; i++) {
        const TevStageConfig stage = config.texture.tev_stages[i];
        if (PassThrough(stage)) {
            continue;
        }
        if (stage.color_op == Operation::Dot3_RGBA) {
            return 6;
        }
        u32 args = 0;
        switch (stage.alpha_op) {
        case Operation::Replace:
            args = 1;
            break;
        case Operation::Modulate:
        case Operation::Add:
        case Operation::AddSigned:
        case Operation::Subtract:
            args = 2;
            break;
        case Operation::Lerp:
        case Operation::MultiplyThenAdd:
        case Operation::AddThenMultiply:
            args = 3;
            break;
        default:
            return 6;
        }
        const Source sources[3] = {stage.alpha_source1, stage.alpha_source2,
                                   stage.alpha_source3};
        const AlphaModifier mods[3] = {stage.alpha_modifier1, stage.alpha_modifier2,
                                       stage.alpha_modifier3};
        for (u32 a = 0; a < args; a++) {
            // The generator's stage-0 quirk: Previous on the first stage reads source 3.
            const Source src = (i == 0 && sources[a] == Source::Previous)
                                   ? stage.alpha_source3.Value()
                                   : sources[a];
            const bool alpha_lane = mods[a] == AlphaModifier::SourceAlpha ||
                                    mods[a] == AlphaModifier::OneMinusSourceAlpha;
            switch (src) {
            case Source::PrimaryColor:
            case Source::Texture1:
            case Source::Texture2:
            case Source::Constant:
                break;
            case Source::Texture0:
                if (config.texture.texture0_type !=
                    Pica::TexturingRegs::TextureConfig::TextureType::Texture2D) {
                    return 11;
                }
                break;
            case Source::Previous:
                // The alpha-only form's colour lanes are not the original's.
                if (!alpha_lane) {
                    return 10;
                }
                break;
            case Source::PreviousBuffer:
                if (!alpha_lane) {
                    return 9;
                }
                break;
            case Source::PrimaryFragmentColor:
            case Source::SecondaryFragmentColor:
                return 7;
            case Source::Texture3:
                return 8;
            default:
                return 12;
            }
        }
    }
    return 1;
}

Pica::Shader::FSConfig AlphaOnlyConfig(const Pica::Shader::FSConfig& config) {
    // FSConfig has no default constructor: copy the bytes, as the emitter's dbgminfs does.
    alignas(Pica::Shader::FSConfig) unsigned char raw[sizeof(Pica::Shader::FSConfig)];
    std::memcpy(raw, &config, sizeof(raw));
    auto& c = *reinterpret_cast<Pica::Shader::FSConfig*>(raw);
    c.lighting.enable.Assign(0);
    c.texture.fog_mode.Assign(Pica::TexturingRegs::FogMode::None);
    c.proctex.enable.Assign(0);
    const u32 last = LastActiveStage(config);
    for (u32 i = 0; i <= last; i++) {
        const TevStageConfig real = config.texture.tev_stages[i];
        if (PassThrough(real)) {
            continue;
        }
        TevStageConfig stage = real;
        const auto cmod = [](AlphaModifier m) {
            static constexpr u32 map[4] = {0x2, 0x4, 0x8, 0xC}; // alpha, red, green, blue
            const u32 v = static_cast<u32>(m);
            return static_cast<ColorModifier>(map[(v >> 1) & 3] | (v & 1));
        };
        stage.color_source1.Assign(real.alpha_source1.Value());
        stage.color_source2.Assign(real.alpha_source2.Value());
        stage.color_source3.Assign(real.alpha_source3.Value());
        stage.color_modifier1.Assign(cmod(real.alpha_modifier1.Value()));
        stage.color_modifier2.Assign(cmod(real.alpha_modifier2.Value()));
        stage.color_modifier3.Assign(cmod(real.alpha_modifier3.Value()));
        stage.color_op.Assign(real.alpha_op.Value());
        stage.color_scale.Assign(real.alpha_scale.Value());
        c.texture.tev_stages[i].sources_raw = stage.sources_raw;
        c.texture.tev_stages[i].modifiers_raw = stage.modifiers_raw;
        c.texture.tev_stages[i].ops_raw = stage.ops_raw;
        c.texture.tev_stages[i].scales_raw = stage.scales_raw;
    }
    Pica::Shader::FSConfig out = c;
    return out;
}

} // namespace GxmRenderer
