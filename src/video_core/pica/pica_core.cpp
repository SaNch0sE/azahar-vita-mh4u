// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cstring>

#include "common/arch.h"
#include "common/archives.h"
#include "common/microprofile.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/memory.h"
#include "video_core/pica/pica_core.h"
#include "video_core/pica/vertex_loader.h"
#include "video_core/pica/words_differ.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_sgxd/vprof.h"
#include "video_core/shader/shader.h"

namespace Pica {

MICROPROFILE_DEFINE(GPU_Drawing, "GPU", "Drawing", MP_RGB(50, 50, 240));

// Class representing implementation details of each internal register
// The set/get pattern is used instead of a bitfield union to allow
// constexpr evaluation.
class RegImplInfo {
private:
    using NeedsSpecialHandlingBF = BitField<0, 1, u16>;
    using SupportsBatchBF = BitField<1, 1, u16>;
    using RegsUntilSpecialBF = BitField<2, 14, u16>;

    u16 raw{};

public:
    constexpr bool NeedsSpecialHandling() const {
        return NeedsSpecialHandlingBF::ExtractValue(raw) != 0;
    }

    constexpr void SetNeedsSpecialHandling() {
        raw = (raw & ~NeedsSpecialHandlingBF::mask) | NeedsSpecialHandlingBF::FormatValue(1);
    }

    constexpr bool SupportsBatch() const {
        return SupportsBatchBF::ExtractValue(raw) != 0;
    }

    constexpr void SetSupportsBatch() {
        raw = (raw & ~SupportsBatchBF::mask) | SupportsBatchBF::FormatValue(1);
    }

    constexpr u16 RegsUntilSpecial() const {
        return RegsUntilSpecialBF::ExtractValue(raw);
    }

    constexpr void SetRegsUntilSpecial(u16 value) {
        raw = (raw & ~RegsUntilSpecialBF::mask) | RegsUntilSpecialBF::FormatValue(value);
    }
};

union CommandHeader {
    u32 hex;
    BitField<0, 16, u32> cmd_id;
    BitField<16, 4, u32> parameter_mask;
    BitField<20, 8, u32> extra_data_length;
    BitField<31, 1, u32> group_commands;
};
static_assert(sizeof(CommandHeader) == sizeof(u32), "CommandHeader has incorrect size!");

PicaCore::PicaCore(Memory::MemorySystem& memory_)
    : memory{memory_}, geometry_pipeline{regs.internal, gs_unit, gs_setup},
      shader_engine{CreateEngine(Settings::values.use_shader_jit.GetValue())} {
    InitializeRegs();
    dirty_regs.SetAllDirty();

    const auto submit_vertex = [this](const AttributeBuffer& buffer) {
        const auto add_triangle = [this](const OutputVertex& v0, const OutputVertex& v1,
                                         const OutputVertex& v2) {
            rasterizer->AddTriangle(v0, v1, v2);
        };
        const auto vertex = OutputVertex(regs.internal.rasterizer, buffer);
        primitive_assembler.SubmitVertex(vertex, add_triangle);
    };

    gs_unit.SetVertexHandlers(submit_vertex, [this]() { primitive_assembler.SetWinding(); });
    geometry_pipeline.SetVertexHandler(submit_vertex);

    primitive_assembler.Reconfigure(PipelineRegs::TriangleTopology::List);
}

PicaCore::~PicaCore() = default;

void PicaCore::InitializeRegs() {
    // Values initialized by GSP
    regs.internal.irq_autostop = 1;
    regs.internal.irq_mask = 0xFFFFFFF0;
    // Older versions of libctru didn't initialize this, initialize it here to avoid endless black
    // screen. Not needed on actual hardware due to previous software already having set it up
    regs.internal.irq_compare = 0x12345678;

    auto& framebuffer_top = regs.framebuffer_config[0];
    auto& framebuffer_sub = regs.framebuffer_config[1];

    // Set framebuffer defaults from nn::gx::Initialize
    framebuffer_top.address_left1 = 0x181E6000;
    framebuffer_top.address_left2 = 0x1822C800;
    framebuffer_top.address_right1 = 0x18273000;
    framebuffer_top.address_right2 = 0x182B9800;
    framebuffer_sub.address_left1 = 0x1848F000;
    framebuffer_sub.address_left2 = 0x184C7800;

    framebuffer_top.width.Assign(240);
    framebuffer_top.height.Assign(400);
    framebuffer_top.stride = 3 * 240;
    framebuffer_top.color_format.Assign(PixelFormat::RGB8);
    framebuffer_top.active_fb = 0;

    framebuffer_sub.width.Assign(240);
    framebuffer_sub.height.Assign(320);
    framebuffer_sub.stride = 3 * 240;
    framebuffer_sub.color_format.Assign(PixelFormat::RGB8);
    framebuffer_sub.active_fb = 0;

    // Tales of Abyss expects this register to have the following default values.
    auto& gs = regs.internal.gs;
    gs.max_input_attribute_index.Assign(1);
    gs.shader_mode.Assign(ShaderRegs::ShaderMode::VS);
}

void PicaCore::BindRasterizer(VideoCore::RasterizerInterface* rasterizer) {
    this->rasterizer = rasterizer;
}

void PicaCore::SetInterruptHandler(Service::GSP::InterruptHandler& signal_interrupt) {
    this->signal_interrupt = signal_interrupt;
}

static bool any_byte_match(u32 a, u32 b) {
    return ((a & 0xFF) == (b & 0xFF)) || (((a >> 8) & 0xFF) == ((b >> 8) & 0xFF)) ||
           (((a >> 16) & 0xFF) == ((b >> 16) & 0xFF)) || (((a >> 24) & 0xFF) == ((b >> 24) & 0xFF));
}

// Special-register dispatch: one handler per register id, called through a table indexed by
// the id. A switch over the special ids (0x010..0x2DD, about 80 cases) is too sparse for a
// jump table, so clang emits a comparison tree that every special write walks (several
// branches for a uniform upload, the commonest special write in a command list). The table is
// one load and one indirect call; a null entry means the register is plain.
struct PicaCore::Dispatch {
    static bool GsMirrorsVs(const PicaCore& p) {
        return p.GsMirrorsVs();
    }

    // Before a write of the vertex unit's: whether the geometry unit mirrors it (the write is
    // then logged, FlushGsMirror). If it no longer does, the log so far goes in first: the
    // mirroring stopped before this write.
    static bool MirrorsVsWrite(PicaCore& p) {
        if (p.GsMirrorsVs()) {
            return true;
        }
        p.FlushGsMirror();
        return false;
    }

    template <std::size_t N>
    static void LogWords(std::array<u32, N>& bits, u32 lo, u32 count) {
        for (u32 at = lo, end = lo + count; at < end;) {
            const u32 bit = at % 32;
            const u32 n = std::min(32 - bit, end - at);
            bits[at / 32] |= (n == 32 ? ~0u : (1u << n) - 1u) << bit;
            at += n;
        }
    }

    static void IrqRequest(PicaCore& p, u32 id, u32, bool& stop_requested) {
        // TODO(PabloMK7): This logic is not fully accurate, but close enough:
        // https://problemkaputt.de/gbatek-3ds-gpu-internal-registers-finalize-interrupt-registers.htm
        if (any_byte_match(p.regs.internal.reg_array[id], p.regs.internal.irq_compare))
            [[likely]] {
            p.rasterizer->NotifyP3D();
            p.signal_interrupt(Service::GSP::InterruptId::P3D,
                               p.delay_generator.CalculateAndResetDelay());
            if (p.regs.internal.irq_autostop) [[likely]] {
                stop_requested = true;
            }
        }
    }

    static void TriangleTopology(PicaCore& p, u32, u32, bool&) {
        p.primitive_assembler.Reconfigure(p.regs.internal.pipeline.triangle_topology);
    }

    static void RestartPrimitive(PicaCore& p, u32, u32, bool&) {
        p.primitive_assembler.Reset();
    }

    static void DefaultAttributesIndex(PicaCore& p, u32, u32, bool&) {
        p.immediate.Reset();
    }

    // Load default vertex input attributes
    static void DefaultAttributesValue(PicaCore& p, u32, u32 value, bool&) {
        p.SubmitImmediate(value);
    }

    static void DefaultAttributesValueBatch(PicaCore& p, const u32* values, u32 count) {
        for (u32 i = 0; i < count; i++) {
            p.SubmitImmediate(values[i]);
        }
    }

    static void CommandBufferTrigger(PicaCore& p, u32 id, u32, bool&) {
        const u32 index = static_cast<u32>(id - PICA_REG_INDEX(pipeline.command_buffer.trigger[0]));
        const PAddr addr = p.regs.internal.pipeline.command_buffer.GetPhysicalAddress(index);
        const u32 size = p.regs.internal.pipeline.command_buffer.GetSize(index);
        p.memory.GuardPhysicalRead(addr, size);
        const u8* head = p.memory.GetPhysicalPointer(addr);
        GxmRenderer::Vprof::Memory(GxmRenderer::Vprof::Mem::CmdList, addr, size);
        p.cmd_list.Reset(addr, head, size);
    }

    // It seems like these trigger vertex rendering
    static void TriggerDraw(PicaCore& p, u32 id, u32, bool&) {
        p.DrawArrays(id == PICA_REG_INDEX(pipeline.trigger_draw_indexed));
    }

    static void GsBoolUniforms(PicaCore& p, u32, u32, bool&) {
        p.FlushGsMirror();
        p.gs_setup.WriteUniformBoolReg(p.regs.internal.gs.bool_uniforms.Value());
    }

    static void GsIntUniform(PicaCore& p, u32 id, u32, bool&) {
        p.FlushGsMirror();
        const u32 index = id - PICA_REG_INDEX(gs.int_uniforms[0]);
        p.gs_setup.WriteUniformIntReg(index, p.regs.internal.gs.GetIntUniform(index));
    }

    static void GsUniformValue(PicaCore& p, u32, u32 value, bool&) {
        p.FlushGsMirror();
        p.gs_setup.WriteUniformFloatReg(p.regs.internal.gs, value);
        p.dirty_regs.Set(PICA_REG_INDEX(gs.uniform_setup));
    }

    static void GsUniformValueBatch(PicaCore& p, const u32* values, u32 count) {
        p.FlushGsMirror();
        p.gs_setup.WriteUniformFloatRegRange(p.regs.internal.gs, values, count);
        p.dirty_regs.Set(PICA_REG_INDEX(gs.uniform_setup));
    }

    static void GsProgramWord(PicaCore& p, u32, u32 value, bool&) {
        p.FlushGsMirror();
        u32& offset = p.regs.internal.gs.program.offset;
        if (offset >= 4096) {
            LOG_ERROR(HW_GPU, "Invalid GS program offset {}", offset);
        } else {
            p.gs_setup.UpdateProgramCode(offset, value);
            offset++;
            p.dirty_regs.Set(PICA_REG_INDEX(gs.program.offset));
        }
    }

    static void GsProgramWordBatch(PicaCore& p, const u32* values, u32 count) {
        p.FlushGsMirror();
        u32& offset = p.regs.internal.gs.program.offset;
        if (offset + count > 4096) {
            LOG_ERROR(HW_GPU, "Invalid GS program offset {} count {}", offset, count);
        } else {
            p.gs_setup.UpdateProgramCodeRange(offset, values, count);
            offset += count;
            p.dirty_regs.Set(PICA_REG_INDEX(gs.program.offset));
        }
    }

    static void GsSwizzleWord(PicaCore& p, u32, u32 value, bool&) {
        p.FlushGsMirror();
        u32& offset = p.regs.internal.gs.swizzle_patterns.offset;
        if (offset >= p.gs_setup.GetSwizzleData().size()) {
            LOG_ERROR(HW_GPU, "Invalid GS swizzle pattern offset {}", offset);
        } else {
            p.gs_setup.UpdateSwizzleData(offset, value);
            offset++;
            p.dirty_regs.Set(PICA_REG_INDEX(gs.swizzle_patterns.offset));
        }
    }

    static void GsSwizzleWordBatch(PicaCore& p, const u32* values, u32 count) {
        p.FlushGsMirror();
        u32& offset = p.regs.internal.gs.swizzle_patterns.offset;
        if (offset + count > p.gs_setup.GetSwizzleData().size()) {
            LOG_ERROR(HW_GPU, "Invalid GS swizzle pattern offset {} count {}", offset, count);
        } else {
            p.gs_setup.UpdateSwizzleDataRange(offset, values, count);
            offset += count;
            p.dirty_regs.Set(PICA_REG_INDEX(gs.swizzle_patterns.offset));
        }
    }

    static void VsOutputMask(PicaCore& p, u32, u32 value, bool&) {
        if (GsMirrorsVs(p)) {
            p.regs.internal.gs.output_mask.Assign(value);
            // The rasterizer reads it, through the sgxd render thread's copy.
            p.dirty_regs.Set(PICA_REG_INDEX(gs.output_mask));
        }
    }

    static void VsBoolUniforms(PicaCore& p, u32, u32, bool&) {
        const bool mirror = MirrorsVsWrite(p);
        p.vs_setup.WriteUniformBoolReg(p.regs.internal.vs.bool_uniforms.Value());
        if (mirror) {
            p.gs_mirror.b = p.gs_mirror.pending = true;
        }
    }

    static void VsIntUniform(PicaCore& p, u32 id, u32, bool&) {
        const u32 index = id - PICA_REG_INDEX(vs.int_uniforms[0]);
        const bool mirror = MirrorsVsWrite(p);
        p.vs_setup.WriteUniformIntReg(index, p.regs.internal.vs.GetIntUniform(index));
        if (mirror) {
            p.gs_mirror.i |= 1u << index;
            p.gs_mirror.pending = true;
        }
    }

    static void VsUniformValue(PicaCore& p, u32, u32 value, bool&) {
        const bool mirror = MirrorsVsWrite(p);
        const auto index = p.vs_setup.WriteUniformFloatReg(p.regs.internal.vs, value);
        p.dirty_regs.Set(PICA_REG_INDEX(vs.uniform_setup));
        if (mirror && index) {
            LogWords(p.gs_mirror.f, *index, 1);
            p.gs_mirror.pending = true;
        }
    }

    static void VsUniformValueBatch(PicaCore& p, const u32* values, u32 count) {
        const bool mirror = MirrorsVsWrite(p);
        const auto range = p.vs_setup.WriteUniformFloatRegRange(p.regs.internal.vs, values, count);
        p.dirty_regs.Set(PICA_REG_INDEX(vs.uniform_setup));
        if (range && mirror) {
            LogWords(p.gs_mirror.f, range->first_index, range->count);
            p.gs_mirror.pending = true;
        }
    }

    static void VsProgramWord(PicaCore& p, u32, u32 value, bool&) {
        u32& offset = p.regs.internal.vs.program.offset;
        if (offset >= 512) {
            LOG_ERROR(HW_GPU, "Invalid VS program offset {}", offset);
        } else {
            const bool mirror = MirrorsVsWrite(p);
            p.vs_setup.UpdateProgramCode(offset, value);
            if (mirror) {
                LogWords(p.gs_mirror.code, offset, 1);
                p.gs_mirror.pending = true;
            }
            offset++;
            p.dirty_regs.Set(PICA_REG_INDEX(vs.program.offset));
        }
    }

    static void VsProgramWordBatch(PicaCore& p, const u32* values, u32 count) {
        u32& offset = p.regs.internal.vs.program.offset;
        if (offset + count > 512) {
            LOG_ERROR(HW_GPU, "Invalid VS program offset {} count {}", offset, count);
        } else {
            const bool mirror = MirrorsVsWrite(p);
            p.vs_setup.UpdateProgramCodeRange(offset, values, count);
            if (mirror) {
                LogWords(p.gs_mirror.code, offset, count);
                p.gs_mirror.pending = true;
            }
            offset += count;
            p.dirty_regs.Set(PICA_REG_INDEX(vs.program.offset));
        }
    }

    static void VsSwizzleWord(PicaCore& p, u32, u32 value, bool&) {
        u32& offset = p.regs.internal.vs.swizzle_patterns.offset;
        if (offset >= p.vs_setup.GetSwizzleData().size()) {
            LOG_ERROR(HW_GPU, "Invalid VS swizzle pattern offset {}", offset);
        } else {
            const bool mirror = MirrorsVsWrite(p);
            p.vs_setup.UpdateSwizzleData(offset, value);
            if (mirror) {
                LogWords(p.gs_mirror.swizzle, offset, 1);
                p.gs_mirror.pending = true;
            }
            offset++;
            p.dirty_regs.Set(PICA_REG_INDEX(vs.swizzle_patterns.offset));
        }
    }

    static void VsSwizzleWordBatch(PicaCore& p, const u32* values, u32 count) {
        u32& offset = p.regs.internal.vs.swizzle_patterns.offset;
        if (offset + count > p.vs_setup.GetSwizzleData().size()) {
            LOG_ERROR(HW_GPU, "Invalid VS swizzle pattern offset {} count {}", offset, count);
        } else {
            const bool mirror = MirrorsVsWrite(p);
            p.vs_setup.UpdateSwizzleDataRange(offset, values, count);
            if (mirror) {
                LogWords(p.gs_mirror.swizzle, offset, count);
                p.gs_mirror.pending = true;
            }
            offset += count;
            p.dirty_regs.Set(PICA_REG_INDEX(vs.swizzle_patterns.offset));
        }
    }

    static void LightingLutData(PicaCore& p, u32, u32 value, bool&) {
        auto& lut_config = p.regs.internal.lighting.lut_config;
        const u32 prev =
            std::exchange(p.lighting.luts[lut_config.type][lut_config.index].raw, value);
        p.lighting.lut_dirty |= (prev != value) << lut_config.type;
        lut_config.index.Assign(lut_config.index + 1);
        p.dirty_regs.Set(PICA_REG_INDEX(lighting.lut_config));
    }

    static void LightingLutDataBatch(PicaCore& p, const u32* values, u32 count) {
        // Games rewrite their lighting LUTs every frame, 256 words at a time: compare and copy the
        // runs up to the wrap instead of exchanging one word at a time. The compare is
        // WordsDiffer's, not newlib's memcmp.
        auto& lut_config = p.regs.internal.lighting.lut_config;
        if (lut_config.type >= p.lighting.luts.size()) [[unlikely]] {
            lut_config.index.Assign(lut_config.index + count);
            p.dirty_regs.Set(PICA_REG_INDEX(lighting.lut_config));
            return;
        }
        auto& lut = p.lighting.luts[lut_config.type];
        static_assert(sizeof(lut[0]) == sizeof(u32));
        u32 index = lut_config.index;
        u32 done = 0;
        while (done < count) {
            const u32 run = std::min<u32>(count - done, static_cast<u32>(lut.size()) - index);
            if (WordsDiffer(reinterpret_cast<const u32*>(&lut[index]), values + done, run)) {
                std::memcpy(&lut[index], values + done, run * sizeof(u32));
                p.lighting.lut_dirty |= 1u << lut_config.type;
            }
            index = (index + run) % static_cast<u32>(lut.size());
            done += run;
        }
        lut_config.index.Assign(lut_config.index + count);
        p.dirty_regs.Set(PICA_REG_INDEX(lighting.lut_config));
    }

    static void FogLutData(PicaCore& p, u32, u32 value, bool&) {
        FogLutDataBatch(p, &value, 1);
    }

    static void FogLutDataBatch(PicaCore& p, const u32* values, u32 count) {
        auto& offset = p.regs.internal.texturing.fog_lut_offset;
        for (u32 i = 0; i < count; i++) {
            const u32 prev = std::exchange(p.fog.lut[(offset + i) % 128].raw, values[i]);
            p.fog.lut_dirty |= prev != values[i];
        }
        offset.Assign(offset + count);
        p.dirty_regs.Set(PICA_REG_INDEX(texturing.fog_lut_offset));
    }

    static void ProcTexLutData(PicaCore& p, u32, u32 value, bool&) {
        ProcTexLutDataBatch(p, &value, 1);
    }

    static void ProcTexLutDataBatch(PicaCore& p, const u32* values, u32 count) {
        auto& index = p.regs.internal.texturing.proctex_lut_config.index;
        const auto lut_table = p.regs.internal.texturing.proctex_lut_config.ref_table.Value();
        const auto sync_lut = [&](auto& proctex_table) {
            for (u32 i = 0; i < count; i++) {
                const u32 prev =
                    std::exchange(proctex_table[(index + i) % proctex_table.size()].raw, values[i]);
                p.proctex.table_dirty |= (prev != values[i]) << u32(lut_table);
            }
        };
        switch (lut_table) {
        case TexturingRegs::ProcTexLutTable::Noise:
            sync_lut(p.proctex.noise_table);
            break;
        case TexturingRegs::ProcTexLutTable::ColorMap:
            sync_lut(p.proctex.color_map_table);
            break;
        case TexturingRegs::ProcTexLutTable::AlphaMap:
            sync_lut(p.proctex.alpha_map_table);
            break;
        case TexturingRegs::ProcTexLutTable::Color:
            sync_lut(p.proctex.color_table);
            break;
        case TexturingRegs::ProcTexLutTable::ColorDiff:
            sync_lut(p.proctex.color_diff_table);
            break;
        }
        index.Assign(index + count);
        p.dirty_regs.Set(PICA_REG_INDEX(texturing.proctex_lut_config));
    }

    static consteval std::array<SpecialFn, RegsInternal::NUM_REGS> BuildSpecialLUT() {
        std::array<SpecialFn, RegsInternal::NUM_REGS> table{};
        const auto set = [&table](u32 base, u32 n, SpecialFn fn) {
            for (u32 i = 0; i < n; ++i) {
                table[base + i] = fn;
            }
        };
        set(PICA_REG_INDEX(irq_request), 1, &IrqRequest);
        set(PICA_REG_INDEX(pipeline.triangle_topology), 1, &TriangleTopology);
        set(PICA_REG_INDEX(pipeline.restart_primitive), 1, &RestartPrimitive);
        set(PICA_REG_INDEX(pipeline.vs_default_attributes_setup.index), 1,
            &DefaultAttributesIndex);
        set(PICA_REG_INDEX(pipeline.vs_default_attributes_setup.set_value[0]), 3,
            &DefaultAttributesValue);
        set(PICA_REG_INDEX(pipeline.command_buffer.trigger[0]), 2, &CommandBufferTrigger);
        set(PICA_REG_INDEX(pipeline.trigger_draw), 1, &TriggerDraw);
        set(PICA_REG_INDEX(pipeline.trigger_draw_indexed), 1, &TriggerDraw);
        set(PICA_REG_INDEX(gs.bool_uniforms), 1, &GsBoolUniforms);
        set(PICA_REG_INDEX(gs.int_uniforms[0]), 4, &GsIntUniform);
        set(PICA_REG_INDEX(gs.uniform_setup.set_value[0]), 8, &GsUniformValue);
        set(PICA_REG_INDEX(gs.program.set_word[0]), 8, &GsProgramWord);
        set(PICA_REG_INDEX(gs.swizzle_patterns.set_word[0]), 8, &GsSwizzleWord);
        set(PICA_REG_INDEX(vs.output_mask), 1, &VsOutputMask);
        set(PICA_REG_INDEX(vs.bool_uniforms), 1, &VsBoolUniforms);
        set(PICA_REG_INDEX(vs.int_uniforms[0]), 4, &VsIntUniform);
        set(PICA_REG_INDEX(vs.uniform_setup.set_value[0]), 8, &VsUniformValue);
        set(PICA_REG_INDEX(vs.program.set_word[0]), 8, &VsProgramWord);
        set(PICA_REG_INDEX(vs.swizzle_patterns.set_word[0]), 8, &VsSwizzleWord);
        set(PICA_REG_INDEX(lighting.lut_data[0]), 8, &LightingLutData);
        set(PICA_REG_INDEX(texturing.fog_lut_data[0]), 8, &FogLutData);
        set(PICA_REG_INDEX(texturing.proctex_lut_data[0]), 8, &ProcTexLutData);
        return table;
    }

    static consteval std::array<BatchFn, RegsInternal::NUM_REGS> BuildBatchLUT() {
        std::array<BatchFn, RegsInternal::NUM_REGS> table{};
        const auto set = [&table](u32 base, u32 n, BatchFn fn) {
            for (u32 i = 0; i < n; ++i) {
                table[base + i] = fn;
            }
        };
        set(PICA_REG_INDEX(pipeline.vs_default_attributes_setup.set_value[0]), 3,
            &DefaultAttributesValueBatch);
        set(PICA_REG_INDEX(gs.uniform_setup.set_value[0]), 8, &GsUniformValueBatch);
        set(PICA_REG_INDEX(gs.program.set_word[0]), 8, &GsProgramWordBatch);
        set(PICA_REG_INDEX(gs.swizzle_patterns.set_word[0]), 8, &GsSwizzleWordBatch);
        set(PICA_REG_INDEX(vs.uniform_setup.set_value[0]), 8, &VsUniformValueBatch);
        set(PICA_REG_INDEX(vs.program.set_word[0]), 8, &VsProgramWordBatch);
        set(PICA_REG_INDEX(vs.swizzle_patterns.set_word[0]), 8, &VsSwizzleWordBatch);
        set(PICA_REG_INDEX(lighting.lut_data[0]), 8, &LightingLutDataBatch);
        set(PICA_REG_INDEX(texturing.fog_lut_data[0]), 8, &FogLutDataBatch);
        set(PICA_REG_INDEX(texturing.proctex_lut_data[0]), 8, &ProcTexLutDataBatch);
        return table;
    }
};

static constexpr std::array<PicaCore::SpecialFn, RegsInternal::NUM_REGS> special_lut =
    PicaCore::Dispatch::BuildSpecialLUT();
static constexpr std::array<PicaCore::BatchFn, RegsInternal::NUM_REGS> batch_lut =
    PicaCore::Dispatch::BuildBatchLUT();

// With assembler_idle, the primitive topology and restart registers are plain: their handlers only
// reset the primitive assembler (and take the topology), which changes nothing while it holds no
// vertex, no strip and no winding flip, and PicaCore::SyncAssemblerTopology takes the topology
// before anything uses the assembler. With hardware draws it is always idle, and games write those
// two registers hundreds of times a frame.
static consteval std::array<RegImplInfo, RegsInternal::NUM_REGS> BuildRegImplFlagsLUT(
    bool assembler_idle) {
    std::array<RegImplInfo, RegsInternal::NUM_REGS> table{};

    // Special and batch registers are the ones with a dispatch handler.
    for (u32 i = 0; i < RegsInternal::NUM_REGS; ++i) {
        const bool plain_when_idle = i == PICA_REG_INDEX(pipeline.triangle_topology) ||
                                     i == PICA_REG_INDEX(pipeline.restart_primitive);
        if (special_lut[i] != nullptr && !(assembler_idle && plain_when_idle)) {
            table[i].SetNeedsSpecialHandling();
        }
        if (batch_lut[i] != nullptr) {
            table[i].SetSupportsBatch();
        }
    }

    // Build distances to next special register.
    u16 regs_since_special = std::numeric_limits<u16>::max();
    for (size_t i = RegsInternal::NUM_REGS; i-- > 0;) {
        if (table[i].NeedsSpecialHandling()) {
            regs_since_special = 0;
        }
        table[i].SetRegsUntilSpecial(regs_since_special);
        if (regs_since_special != std::numeric_limits<u16>::max()) {
            regs_since_special++;
        }
    }

    return table;
}

static constexpr std::array<RegImplInfo, RegsInternal::NUM_REGS> reg_impl_flags_lut =
    BuildRegImplFlagsLUT(false);
static constexpr std::array<RegImplInfo, RegsInternal::NUM_REGS> reg_impl_flags_lut_idle =
    BuildRegImplFlagsLUT(true);

// Expand a 4-bit mask to 4-byte mask, e.g. 0b0101 -> 0x00FF00FF
static constexpr std::array<u32, 16> ExpandBitsToBytes = {
    0x00000000, 0x000000ff, 0x0000ff00, 0x0000ffff, 0x00ff0000, 0x00ff00ff, 0x00ffff00, 0x00ffffff,
    0xff000000, 0xff0000ff, 0xff00ff00, 0xff00ffff, 0xffff0000, 0xffff00ff, 0xffffff00, 0xffffffff,
};

#if CITRA_ARCH(arm32)
// Thumb-2 command-list fast path, see pica_cmdlist_a32.S. Returns the new command index and
// stores the number of applied commands in *out_count.
extern "C" u32 PicaCmdListFastLoop(const u32* head, u32 index, u32 length, const u16* flags_lut,
                                   const u32* expand_lut, u32* reg_array, u8* dirty_bytes,
                                   u32* out_count);
static_assert(sizeof(RegImplInfo) == sizeof(u16), "the loop reads the flags as halfwords");
#endif

/**
 * This is the main loop for processing GPU command lists. On Azahar, it's the most
 * CPU expensive function (excluding the inner Draw calls) due to applications submitting
 * 10-50 command lists per frame, each with hundreds of commands in them. For this reason,
 * it is important that this function is well optimized to reduce the load on the CPU.
 *
 * Each command in the list has the following properties:
 *  - Commands come in [value (32 bit), header (32 bit)] pairs, most of the time.
 *  - The register ID that the 32 bit value should be written to is stored in the header.
 *  - The mask of bits that should be written comes in the header
 *    (to be able to write individual bytes of the 4-byte register)
 *  - Commands can have an extra length N, which means that N extra words follow
 *    after the header word.
 *    - If group_command is set in the header, N sequential registers are
 *      written to starting from the ID + 1 specified in the header.
 *    - If group_command is not set, the same register is written
 *      to with the N extra words. This is used for things like shader uploads
 *      which has a single register ID.
 *
 * Regarding implementation details, we store all register values in an array,
 * as well as a dirty array to indicate which registers have changed since
 * the last draw. Some registers need special handling, as they are "trigger"
 * registers that start the draw, or store the data in the shader units.
 *
 * To be able to determine if a register is special, we use a lookup table
 * generated by BuildRegImplFlagsLUT(). This allows determining if a register
 * is special or not in O(1). This LUT also determines if a register has
 * support for batch handling (extra_data_length != 0 && group_command == 0)
 * and the amount of commands away from the next special register, useful for
 * sequential writes (extra_data_length != 0 && group_command == 1).
 *
 * As much as possible, we want to target the following optimizations:
 *  - We should prevent branches and jumps to functions if they are not needed.
 *  - We should clearly separate special command handling from normal commands
 *    that are much cheaper to handle.
 *  - Commands with extra length should be processed in batch if possible.
 *  - Vectorization should be used as much as possible.
 */
void PicaCore::ProcessCmdList(PAddr list, u32 size, bool ignore_list) [[hot]] {
    if (ignore_list) {
        rasterizer->NotifyP3D();
        signal_interrupt(Service::GSP::InterruptId::P3D, delay_generator.CalculateAndResetDelay());
        return;
    }

    // A list, or a buffer it jumps to, a queued DMA still writes waits for it (a game Triple
    // Deluxe's lists jump into VRAM its DMAs fill each frame: a stale buffer there ended the
    // list before its P3D, and the game waited for that P3D for good).
    memory.GuardPhysicalRead(list, size);
    const u8* head = memory.GetPhysicalPointer(list);
    GxmRenderer::Vprof::Memory(GxmRenderer::Vprof::Mem::CmdList, list, size);
    cmd_list.Reset(list, head, size);

    bool stop_requested = false;
    bool skip_fast_path = false;
    while (cmd_list.current_index < cmd_list.length) {
        if (stop_requested) [[unlikely]] {
            break;
        }
        if (cmd_list.current_index % 2 != 0) {
            cmd_list.current_index++;
        }

        // Early path that processes commands in batches of 4. If any of the commands
        // needs special handling or has extra length it stops and falls back to the
        // slower path. This pattern allows the compiler to auto-vectorize the function
        // if the current ISA allows it (that's why we process in batches of 4
        // as most SIMD operations work with 128 bit registers). MSVC is not able to
        // auto-vectorize this part with SSE4.2, due to the LUT read, instead it just
        // unrolls the loop. Other ISAs and/or compilers may be able to do it,
        // that's why it was decided to keep the structure like this.
        if (!debug_context) [[likely]] {
            if (!skip_fast_path && !immediate_open) {
                // Chosen at each entry: only a slow-path command (a draw) can fill the
                // assembler.
                const auto& flags_lut =
                    primitive_assembler.IsIdle() ? reg_impl_flags_lut_idle : reg_impl_flags_lut;
#if CITRA_ARCH(arm32)
                // Hand-written Thumb-2 loop (pica_cmdlist_a32.S): applies plain register
                // writes, group writes of plain registers among them, until list end or a
                // special command, which falls through to the slow path below with
                // current_index already advanced past the batch. `run` counts the writes.
                // Not called for a command it would hand straight back (a special register,
                // an id past the file, a same-register batch): 497 of the 1311 slow-path
                // commands of a game's race-start frame come right after another.
                u32 run = 0;
                u32 index = cmd_list.current_index;
                const CommandHeader next{index + 1 < cmd_list.length ? cmd_list.head[index + 1]
                                                                     : 0u};
                const bool slow_next =
                    index + 1 < cmd_list.length &&
                    (next.cmd_id >= RegsInternal::NUM_REGS ||
                     (next.extra_data_length != 0 ? !next.group_commands
                                                  : flags_lut[next.cmd_id].NeedsSpecialHandling()));
                if (!slow_next) {
                    index = PicaCmdListFastLoop(
                        cmd_list.head, cmd_list.current_index, cmd_list.length,
                        reinterpret_cast<const u16*>(flags_lut.data()), ExpandBitsToBytes.data(),
                        regs.internal.reg_array.data(),
                        reinterpret_cast<u8*>(dirty_regs.qwords.data()), &run);
                }
                if (run > 0) {
                    delay_generator.AddCommands(run);
                    cmd_list.current_index = index;
                    if (index + 1 >= cmd_list.length) {
                        continue;
                    }
                }
#else
                constexpr u32 batch_size = 4;
                u32 index = cmd_list.current_index;
                u32 ids[batch_size], values[batch_size], masks[batch_size];
                u32 run = 0;

                while (run < batch_size && index + 1 < cmd_list.length) {
                    const u32 value = cmd_list.head[index];
                    const CommandHeader header{cmd_list.head[index + 1]};

                    // If extra handling is needed stop and fallback to slower path.
                    if (header.extra_data_length != 0 || header.cmd_id >= RegsInternal::NUM_REGS ||
                        flags_lut[header.cmd_id].NeedsSpecialHandling()) {
                        skip_fast_path = true;
                        break;
                    }

                    ids[run] = header.cmd_id;
                    values[run] = value;
                    masks[run] = header.parameter_mask;
                    ++run;
                    index += 2;
                }

                // Process the commands that we have read so far (up to 4).
                if (run > 0) {
                    delay_generator.AddCommands(run);
                    for (u32 i = 0; i < run; ++i) {
                        const u32 id = ids[i];
                        const u32 write_mask = ExpandBitsToBytes[masks[i]];
                        const u32 old_value = regs.internal.reg_array[id];
                        const u32 new_value = (old_value & ~write_mask) | (values[i] & write_mask);
                        regs.internal.reg_array[id] = new_value;
                        // Marked only when it changes (as pica_cmdlist_a32.S).
                        if (new_value != old_value) {
                            dirty_regs.Set(id);
                        }
                    }
                    cmd_list.current_index = index;

                    // Continue from the while loop in case we reached the end of the list.
                    continue;
                }
            }
#else
            constexpr u32 batch_size = 4;
            u32 index = cmd_list.current_index;
            u32 ids[batch_size], values[batch_size], masks[batch_size];
            u32 run = 0;

            while (run < batch_size && index + 1 < cmd_list.length) {
                const u32 value = cmd_list.head[index];
                const CommandHeader header{cmd_list.head[index + 1]};

                // If extra handling is needed stop and fallback to slower path.
                if (header.extra_data_length != 0 || header.cmd_id >= RegsInternal::NUM_REGS ||
                    flags_lut[header.cmd_id].NeedsSpecialHandling()) {
                    skip_fast_path = true;
                    break;
                }

                ids[run] = header.cmd_id;
                values[run] = value;
                masks[run] = header.parameter_mask;
                ++run;
                index += 2;
            }

            // Process the commands that we have read so far (up to 4).
            if (run > 0) {
                delay_generator.AddCommands(run);
                for (u32 i = 0; i < run; ++i) {
                    const u32 id = ids[i];
                    const u32 write_mask = ExpandBitsToBytes[masks[i]];
                    const u32 old_value = regs.internal.reg_array[id];
                    const u32 new_value = (old_value & ~write_mask) | (values[i] & write_mask);
                    regs.internal.reg_array[id] = new_value;
                    // Marked only when it changes (as pica_cmdlist_a32.S).
                    if (new_value != old_value) {
                        dirty_regs.Set(id);
                    }
                }
                cmd_list.current_index = index;

                // Continue from the while loop in case we reached the end of the list.
                continue;
            }
#endif
        }
        // Slow path, command needs special handling.

        skip_fast_path = false;

        // Read the header and the value to write.
        const u32 value = cmd_list.head[cmd_list.current_index++];
        const CommandHeader header{cmd_list.head[cmd_list.current_index++]};

        // Write to the requested PICA register.
        WriteInternalReg(header.cmd_id, value, header.parameter_mask, stop_requested);

        // Write any extra paramters as well.
        const u32 count = header.extra_data_length;
        if (count == 0)
            continue;

        // Handle commands with extra length.
        const u32* extra = &cmd_list.head[cmd_list.current_index];
        cmd_list.current_index += count;

        if (!header.group_commands) {
            // Same register written count times in a row (program/swizzle upload, LUT, etc.).
            WriteInternalRegBatch(header.cmd_id, extra, count, header.parameter_mask,
                                  stop_requested);
        } else {
            // Sequential registers written from header.cmd_id+1 to header.cmd_id+count.
            WriteInternalRegSequential(header.cmd_id + 1, extra, count, header.parameter_mask,
                                       stop_requested);
        }
    }
}

// Handle batch register writes
void PicaCore::WriteInternalRegBatch(u32 id, const u32* values, u32 count, u32 mask,
                                     bool& stop_requested) {
    if (id >= RegsInternal::NUM_REGS) [[unlikely]] {
        // Writes to OOB registers are no-op.
        LOG_DEBUG(HW_GPU,
                  "Commandlist tried to write to invalid register 0x{:03X} repeated 0x{:04X} times"
                  "(mask: {:X})",
                  id, count, mask);
        return;
    }

    delay_generator.AddCommands(count);
    if (immediate_open && !IsImmediateData(id)) {
        FlushImmediate();
    }
    const u32 write_mask = ExpandBitsToBytes[mask];
    // Only write the last value to the register array.
    // Batch handlers should take this in mind.
    regs.internal.reg_array[id] =
        (regs.internal.reg_array[id] & ~write_mask) | (values[count - 1] & write_mask);
    dirty_regs.Set(id);

    if (reg_impl_flags_lut[id].SupportsBatch()) {
        // If the register supports batch then call the handler.
        batch_lut[id](*this, values, count);
    } else if (reg_impl_flags_lut[id].NeedsSpecialHandling()) [[unlikely]] {
        // Unlikely as all special regs that make sense to use batch mode already
        // support batch handling.
        for (u32 i = 0; i < count && !stop_requested; ++i) {
            special_lut[id](*this, id, values[i], stop_requested);
        }
    }
}

// Handle sequential register writes.
void PicaCore::WriteInternalRegSequential(u32 id, const u32* __restrict values, u32 count, u32 mask,
                                          bool& stop_requested) {
    if (id + count > RegsInternal::NUM_REGS) [[unlikely]] {
        // Writes to OOB registers are no-op.
        LOG_DEBUG(
            HW_GPU,
            "Commandlist tried to write to invalid register range 0x{:03X}-0x{:03X} (mask: {:X})",
            id, id + count, mask);
        if (id >= RegsInternal::NUM_REGS) {
            return;
        } else {
            count = RegsInternal::NUM_REGS - id;
        }
    }
    if (immediate_open && !(IsImmediateData(id) && IsImmediateData(id + count - 1))) {
        FlushImmediate();
    }
    const u32 write_mask = ExpandBitsToBytes[mask];
    u32* __restrict dst = &regs.internal.reg_array[id];
    u32 offset = 0;

    // This code is structured so that it uses the LUT to get the distance from the
    // register ID to the next special register. Then copies the range of normal registers
    // until it reaches the special register which is handled individually, and so on.
    while (offset < count) {
        if (stop_requested) [[unlikely]] {
            break;
        }
        const u32 reg = id + offset;
        // Distance to the next special register, capped by how many writes
        // are actually left in this write. If this register is special this is 0.
        const u32 batch_count =
            std::min<u32>(reg_impl_flags_lut[reg].RegsUntilSpecial(), count - offset);
        if (batch_count > 0) {
            delay_generator.AddCommands(batch_count);

            // Allows the compiler to auto-vectorize thanks to the __restrict keywords.
            // Verified in MSVC that vectorization is happening.
            u32* __restrict batch_dst = dst + offset;
            const u32* __restrict batch_src = values + offset;
            for (u32 i = 0; i < batch_count; ++i) {
                batch_dst[i] = (batch_dst[i] & ~write_mask) | (batch_src[i] & write_mask);
            }

            dirty_regs.SetRange(reg, batch_count);
            offset += batch_count;
        }
        // Whatever register stopped the batch (if we didn't reach the end)
        // needs individual handling, then resume batching after it.
        if (offset >= count) {
            break;
        }
        const u32 special = id + offset;
        const BatchFn batch = batch_lut[special];
        if (!batch) {
            WriteInternalReg(special, values[offset], mask, stop_requested);
            ++offset;
            continue;
        }
        // A port's aliases in a row (set_value[0..7], set_word[0..7], lut_data[0..7], the default
        // attribute's three) are one stream into the port, as WriteInternalReg would feed it a word
        // at a time: the port's batch handler takes them at once.
        u32 run = 1;
        while (offset + run < count && batch_lut[special + run] == batch) {
            ++run;
        }
        delay_generator.AddCommands(run);
        if (immediate_open && !IsImmediateData(special)) {
            FlushImmediate();
        }
        for (u32 i = 0; i < run; ++i) {
            dst[offset + i] = (dst[offset + i] & ~write_mask) | (values[offset + i] & write_mask);
        }
        dirty_regs.SetRange(special, run);
        batch(*this, values + offset, run);
        offset += run;
    }
}

// Handle individual command write.
void PicaCore::WriteInternalReg(u32 id, u32 value, u32 mask, bool& stop_requested) {
    if (id >= RegsInternal::NUM_REGS) [[unlikely]] {
        // Writes to OOB registers are no-op.
        LOG_DEBUG(
            HW_GPU,
            "Commandlist tried to write to invalid register 0x{:03X} (value: {:08X}, mask: {:X})",
            id, value, mask);
        return;
    }

    delay_generator.AddCommands(1);

    if (immediate_open && !IsImmediateData(id)) {
        FlushImmediate();
    }

    // TODO: Figure out how register masking acts on e.g. vs.uniform_setup.set_value
    const u32 old_value = regs.internal.reg_array[id];
    const u32 write_mask = ExpandBitsToBytes[mask];
    regs.internal.reg_array[id] = (old_value & ~write_mask) | (value & write_mask);

    // A plain register is marked dirty only when the write changes it (the command lists
    // rewrite most of them with the values they hold); a special one at every write, before its
    // handler, which may draw (the render thread's copy takes it with that draw). The registers
    // a handler advances (the upload indexes and offsets) the handler marks.
    if (const SpecialFn handler = special_lut[id]) {
        dirty_regs.Set(id);
        handler(*this, id, value, stop_requested);
    } else if (regs.internal.reg_array[id] != old_value) {
        dirty_regs.Set(id);
    }
}

void PicaCore::SubmitImmediate(u32 value) {
    // Push to word to the queue. This returns true when a full attribute is formed.
    if (!immediate.queue.Push(value)) {
        return;
    }

    constexpr std::size_t IMMEDIATE_MODE_INDEX = 0xF;

    auto& setup = regs.internal.pipeline.vs_default_attributes_setup;
    if (setup.index > IMMEDIATE_MODE_INDEX) {
        LOG_ERROR(HW_GPU, "Invalid VS default attribute index {}", setup.index);
        return;
    }

    // Retrieve the attribute and place it in the default attribute buffer.
    const auto attribute = immediate.queue.Get();
    if (setup.index < IMMEDIATE_MODE_INDEX) {
        // Most rewrites hold the value already, and a change sends the whole array to the render
        // thread with the next draw.
        if (input_default_attributes[setup.index] != attribute) {
            input_default_attributes[setup.index] = attribute;
            default_attributes_changed = true;
        }
        setup.index++;
        dirty_regs.Set(PICA_REG_INDEX(pipeline.vs_default_attributes_setup.index));
        return;
    }

    // When index is 0xF the attribute is used for immediate mode drawing.
    immediate.input_vertex[immediate.current_attribute] = attribute;
    if (immediate.current_attribute < regs.internal.pipeline.max_input_attrib_index) {
        immediate.current_attribute++;
        return;
    }

    // We formed a vertex, flush.
    DrawImmediate();
}

void PicaCore::FlushGsMirror() {
    if (!gs_mirror.pending) {
        return;
    }
    // In the logged order's place, the vertex unit's values now: each logged word or uniform
    // was last written by the vertex unit (a later write of either unit's flushes first).
    for (u32 w = 0; w < gs_mirror.f.size(); w++) {
        for (u32 bits = gs_mirror.f[w]; bits != 0; bits &= bits - 1) {
            const u32 index = w * 32 + static_cast<u32>(std::countr_zero(bits));
            gs_setup.MirrorFloatUniform(index, vs_setup.uniforms.f[index]);
        }
    }
    for (u32 bits = gs_mirror.i; bits != 0; bits &= bits - 1) {
        const u32 index = static_cast<u32>(std::countr_zero(bits));
        gs_setup.WriteUniformIntReg(index, vs_setup.uniforms.i[index]);
    }
    if (gs_mirror.b) {
        u32 value = 0;
        for (u32 k = 0; k < vs_setup.uniforms.b.size(); k++) {
            value |= static_cast<u32>(vs_setup.uniforms.b[k]) << k;
        }
        gs_setup.WriteUniformBoolReg(value);
    }
    // Code and swizzles a run of logged words at a time.
    const auto runs = [](const auto& bits, const auto& apply) {
        u32 start = 0;
        bool in_run = false;
        for (u32 w = 0; w < bits.size(); w++) {
            if (bits[w] == (in_run ? ~0u : 0u)) {
                continue;
            }
            for (u32 b = 0; b < 32; b++) {
                const bool set = (bits[w] >> b) & 1;
                if (set != in_run) {
                    if (set) {
                        start = w * 32 + b;
                    } else {
                        apply(start, w * 32 + b - start);
                    }
                    in_run = set;
                }
            }
        }
        if (in_run) {
            apply(start, static_cast<u32>(bits.size()) * 32 - start);
        }
    };
    runs(gs_mirror.code, [this](u32 lo, u32 count) {
        gs_setup.UpdateProgramCodeRange(lo, vs_setup.GetProgramCode().data() + lo, count);
    });
    runs(gs_mirror.swizzle, [this](u32 lo, u32 count) {
        gs_setup.UpdateSwizzleDataRange(lo, vs_setup.GetSwizzleData().data() + lo, count);
    });
    gs_mirror = {};
}

void PicaCore::SyncAssemblerTopology() {
    // While the assembler was idle the fast loop took topology writes as plain registers
    // (BuildRegImplFlagsLUT): its topology follows the register before it is used. A busy
    // assembler took every topology write through its handler.
    const auto topology = regs.internal.pipeline.triangle_topology.Value();
    if (primitive_assembler.GetTopology() != topology) {
        DEBUG_ASSERT_MSG(primitive_assembler.IsIdle(),
                         "PICA: the primitive assembler holds vertices under topology {}, the "
                         "register says {}",
                         static_cast<u32>(primitive_assembler.GetTopology()),
                         static_cast<u32>(topology));
        primitive_assembler.Reconfigure(topology);
    }
}

void PicaCore::DrawImmediate() {
    SyncAssemblerTopology();
    if (!GsMirrorsVs()) {
        FlushGsMirror();
    }
    // The rasterizer runs the vertex program on the GPU: it keeps the raw attributes and draws
    // them as one batch at the next register write that is not immediate data.
    if (regs.internal.pipeline.use_gs == PipelineRegs::UseGS::No &&
        rasterizer->AccelerateImmediateVertex(immediate.input_vertex)) {
        immediate_open = true;
        immediate.current_attribute = 0;
        return;
    }

    // Compile the vertex shader.
    shader_engine->SetupBatch(vs_setup, regs.internal.vs.main_offset);

    ShaderUnit shader_unit;
    AttributeBuffer output{};

    // Invoke the vertex shader for the vertex.
    shader_unit.LoadInput(regs.internal.vs, immediate.input_vertex);
    shader_engine->Run(vs_setup, shader_unit);
    shader_unit.WriteOutput(regs.internal.vs, output);

    // Reconfigure geometry pipeline if needed.
    if (immediate.reset_geometry_pipeline) {
        geometry_pipeline.Reconfigure();
        immediate.reset_geometry_pipeline = false;
    }

    // Send to geometry pipeline.
    ASSERT(!geometry_pipeline.NeedIndexInput());
    geometry_pipeline.Setup(shader_engine.get());
    geometry_pipeline.SubmitVertex(output);

    // Flush the immediate triangle.
    rasterizer->DrawTriangles();
    immediate.current_attribute = 0;
}

void PicaCore::FlushImmediate() {
    immediate_open = false;
    rasterizer->FlushImmediate();
}

void PicaCore::DrawArrays(bool is_indexed) {
    MICROPROFILE_SCOPE(GPU_Drawing);
    SyncAssemblerTopology();
    if (!GsMirrorsVs()) {
        FlushGsMirror();
    }

    const bool accelerate_draw = [this] {
        // Geometry shaders cannot be accelerated due to register preservation, except by a
        // rasterizer that runs the whole geometry pipeline itself.
        if (regs.internal.pipeline.use_gs == PipelineRegs::UseGS::Yes) {
            return rasterizer->AcceleratesGeometryShaders() && primitive_assembler.IsEmpty();
        }

        // TODO (wwylele): for Strip/Fan topology, if the primitive assember is not restarted
        // after this draw call, the buffered vertex from this draw should "leak" to the next
        // draw, in which case we should buffer the vertex into the software primitive assember,
        // or disable accelerate draw completely. However, there is not game found yet that does
        // this, so this is left unimplemented for now. Revisit this when an issue is found in
        // games.

        bool accelerate_draw = Settings::values.use_hw_shader && primitive_assembler.IsEmpty();
        const auto topology = primitive_assembler.GetTopology();
        if (topology == PipelineRegs::TriangleTopology::Shader ||
            topology == PipelineRegs::TriangleTopology::List) {
            accelerate_draw = accelerate_draw && (regs.internal.pipeline.num_vertices % 3) == 0;
        }
        return accelerate_draw;
    }();

    // Add vertices to the delay generator.
    delay_generator.AddVertices(regs.internal.pipeline.num_vertices,
                                regs.internal.pipeline.triangle_topology);

    // Attempt to use hardware vertex shaders if possible.
    if (accelerate_draw && rasterizer->AccelerateDrawBatch(is_indexed)) {
        return;
    }

    // We cannot accelerate the draw, so load and execute the vertex shader for each vertex.
    LoadVertices(is_indexed);

    // Draw emitted triangles.
    rasterizer->DrawTriangles();
}

void PicaCore::LoadVertices(bool is_indexed) {
    // The CPU reads the vertices: GPU work queued before that writes guest memory (a DMA,
    // a transfer) completes first.
    rasterizer->WaitForGpuWrites();
    // Read and validate vertex information from the loaders
    const auto& pipeline = regs.internal.pipeline;
    const PAddr base_address = pipeline.vertex_attributes.GetPhysicalBaseAddress();
    const auto loader = VertexLoader(memory, pipeline);
    regs.internal.rasterizer.ValidateSemantics();

    // Locate index buffer.
    const auto& index_info = pipeline.index_array;
    const u8* index_address_8 = memory.GetPhysicalPointer(base_address + index_info.offset);
    if (index_address_8 == nullptr) {
        // Mario & Luigi: Superstar Saga sets an invalid base address
        // for the vertex attributes. Return early if that is the case.
        return;
    }
    const u16* index_address_16 = reinterpret_cast<const u16*>(index_address_8);
    const bool index_u16 = index_info.format != 0;

    // Simple circular-replacement vertex cache
    const std::size_t VERTEX_CACHE_SIZE = 64;
    std::array<bool, VERTEX_CACHE_SIZE> vertex_cache_valid{};
    std::array<u16, VERTEX_CACHE_SIZE> vertex_cache_ids;
    std::array<AttributeBuffer, VERTEX_CACHE_SIZE> vertex_cache;
    u32 vertex_cache_pos = 0;

    // Compile the vertex shader for this batch.
    ShaderUnit shader_unit;
    AttributeBuffer vs_output;
    shader_engine->SetupBatch(vs_setup, regs.internal.vs.main_offset);

    // Setup geometry pipeline in case we are using a geometry shader.
    geometry_pipeline.Reconfigure();
    geometry_pipeline.Setup(shader_engine.get());
    ASSERT(!geometry_pipeline.NeedIndexInput() || is_indexed);

    for (u32 index = 0; index < pipeline.num_vertices; ++index) {
        // Indexed rendering doesn't use the start offset
        const u32 vertex = is_indexed
                               ? (index_u16 ? index_address_16[index] : index_address_8[index])
                               : (index + pipeline.vertex_offset);

        bool vertex_cache_hit = false;
        if (is_indexed) {
            if (geometry_pipeline.NeedIndexInput()) {
                geometry_pipeline.SubmitIndex(vertex);
                continue;
            }

            for (u32 i = 0; i < VERTEX_CACHE_SIZE; ++i) {
                if (vertex_cache_valid[i] && vertex == vertex_cache_ids[i]) {
                    vs_output = vertex_cache[i];
                    vertex_cache_hit = true;
                    break;
                }
            }
        }

        if (!vertex_cache_hit) {
            // Initialize data for the current vertex
            AttributeBuffer input;
            loader.LoadVertex(base_address, index, vertex, input, input_default_attributes);

            // Invoke the vertex shader for this vertex.
            shader_unit.LoadInput(regs.internal.vs, input);
            shader_engine->Run(vs_setup, shader_unit);
            shader_unit.WriteOutput(regs.internal.vs, vs_output);

            // Cache the vertex when doing indexed rendering.
            if (is_indexed) {
                vertex_cache[vertex_cache_pos] = vs_output;
                vertex_cache_valid[vertex_cache_pos] = true;
                vertex_cache_ids[vertex_cache_pos] = vertex;
                vertex_cache_pos = (vertex_cache_pos + 1) % VERTEX_CACHE_SIZE;
            }
        }

        // Send to geometry pipeline
        geometry_pipeline.SubmitVertex(vs_output);
    }
}

PicaCore::RenderPropertiesGuess PicaCore::GuessCmdRenderProperties(PAddr list, u32 size) {
    // Initialize command list tracking.
    memory.GuardPhysicalRead(list, size);
    const u8* head = memory.GetPhysicalPointer(list);
    cmd_list.Reset(list, head, size);

    constexpr size_t max_iterations = 0x100;

    RenderPropertiesGuess find_info{};

    find_info.vp_height = regs.internal.rasterizer.viewport_size_y.Value();
    find_info.paddr = regs.internal.framebuffer.framebuffer.color_buffer_address.Value() * 8;

    auto process_write = [this, &find_info](u32 cmd_id, u32 value) {
        switch (cmd_id) {
        case PICA_REG_INDEX(rasterizer.viewport_size_y):
            find_info.vp_height = value;
            find_info.vp_heigh_found = true;
            break;
        case PICA_REG_INDEX(framebuffer.framebuffer.color_buffer_address):
            find_info.paddr = value * 8;
            find_info.paddr_found = true;
            break;
        [[unlikely]] case PICA_REG_INDEX(pipeline.command_buffer.trigger[0]):
        [[unlikely]] case PICA_REG_INDEX(pipeline.command_buffer.trigger[1]): {
            const u32 index =
                static_cast<u32>(cmd_id - PICA_REG_INDEX(pipeline.command_buffer.trigger[0]));
            const PAddr addr = regs.internal.pipeline.command_buffer.GetPhysicalAddress(index);
            const u32 size = regs.internal.pipeline.command_buffer.GetSize(index);
            memory.GuardPhysicalRead(addr, size);
            const u8* head = memory.GetPhysicalPointer(addr);
            cmd_list.Reset(addr, head, size);
            break;
        }
        default:
            break;
        }
        return find_info.vp_heigh_found && find_info.paddr_found;
    };

    size_t iterations = 0;
    while (cmd_list.current_index < cmd_list.length && iterations < max_iterations) {
        // Align read pointer to 8 bytes
        if (cmd_list.current_index % 2 != 0) {
            cmd_list.current_index++;
        }

        // Read the header and the value to write.
        const u32 value = cmd_list.head[cmd_list.current_index++];
        const CommandHeader header{cmd_list.head[cmd_list.current_index++]};

        // Write to the requested PICA register.
        if (process_write(header.cmd_id, value))
            break;

        // Write any extra paramters as well. A run that reaches none of the registers the guess
        // looks at is skipped whole: a 256-word LUT upload took 256 calls (3.2% of a game's
        // emulation thread went to the guess, its lists 66 commands deep on average).
        const u32 extra = header.extra_data_length;
        const u32 first = header.group_commands ? header.cmd_id + 1 : header.cmd_id;
        const u32 last = header.group_commands ? header.cmd_id + extra : header.cmd_id;
        const auto reaches = [first, last](u32 reg) { return first <= reg && reg <= last; };
        if (extra != 0 && !reaches(PICA_REG_INDEX(rasterizer.viewport_size_y)) &&
            !reaches(PICA_REG_INDEX(framebuffer.framebuffer.color_buffer_address)) &&
            !reaches(PICA_REG_INDEX(pipeline.command_buffer.trigger[0])) &&
            !reaches(PICA_REG_INDEX(pipeline.command_buffer.trigger[1]))) {
            cmd_list.current_index += extra;
            iterations++;
            continue;
        }
        for (u32 i = 0; i < header.extra_data_length; ++i) {
            const u32 cmd = header.cmd_id + (header.group_commands ? i + 1 : 0);
            const u32 extra_value = cmd_list.head[cmd_list.current_index++];
            if (process_write(cmd, extra_value))
                break;
        }

        iterations++;
    }

    return find_info;
}

template <class Archive>
void PicaCore::CommandList::serialize(Archive& ar, const u32 file_version) {
    ar & addr;
    ar & length;
    ar & current_index;
    if (Archive::is_loading::value) {
        const u8* ptr = Core::System::GetInstance().Memory().GetPhysicalPointer(addr);
        head = reinterpret_cast<const u32*>(ptr);
    }
}

SERIALIZE_IMPL(PicaCore::CommandList)

} // namespace Pica
