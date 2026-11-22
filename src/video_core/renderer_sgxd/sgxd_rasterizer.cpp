// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <utility>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <fmt/ranges.h>
#include <nihstro/shader_bytecode.h>
#include "common/alignment.h"
#include "common/assert.h"
#include "common/hash.h"
#include "common/logging/log.h"
#include "common/swap.h"
#include "core/memory.h"
#include "video_core/pica/pica_core.h"
#include "video_core/pica/regs_external.h"
#include "video_core/pica/shader_setup.h"
#include "video_core/renderer_gxm/gxm_flags.h"
#include "video_core/renderer_gxm/gxm_rasterizer.h"
#include "video_core/texture/texture_decode.h"
#include "video_core/renderer_gxm/usse/fs_usse_gen.h"
#include "video_core/renderer_gxm/usse/gxp_writer.h"
#include "video_core/renderer_gxm/usse/vs_usse_gen.h"
#include "video_core/shader/generator/shader_gen.h"

#include "sgxd_program.h"
#ifdef __vita__
#include "sgxd_ptla.h"
#endif

namespace GxmRenderer {

namespace {

using Pica::f24;
using FramebufferRegs = Pica::FramebufferRegs;

const char* TopologyName(Pica::PipelineRegs::TriangleTopology topology) {
    switch (topology) {
    case Pica::PipelineRegs::TriangleTopology::List:
        return "list";
    case Pica::PipelineRegs::TriangleTopology::Strip:
        return "strip";
    case Pica::PipelineRegs::TriangleTopology::Fan:
        return "fan";
    case Pica::PipelineRegs::TriangleTopology::Shader:
        return "shader";
    }
    return "unknown";
}

// What libgxm's shader patcher adds to a fragment program (sgxd_recipe.c's imgui_fs and
// imgui_fs_blend, Sony's words): the emitter leaves the colour as F16 in pa0, packed to the
// output with END, or packed in place and blended over the output buffer's o0.
constexpr u64 PackToOutput = 0x40a40a3da01d8000ull; // pack.u8.f16.scl o0, pa0 (end)
constexpr u64 PackInPlace = 0x40a00a3ea01d8000ull;  // pack.u8.f16.scl pa0, pa0
// F16F16F16F16 (0x1b), the proctex colour rows.
constexpr unsigned LutFormatF16x4 = 0x1b;

// sop2 o0, pa0, o0 (end): SRC_ALPHA, ONE_MINUS_SRC_ALPHA on colour and alpha. BlendWord sets its
// factor and operation fields for other blends.
constexpr u64 BlendSourceAlpha = 0x809482dd90000000ull;
// The fields BlendWord sets: USE1 CMOD1 24, ASEL1 21:20, CMOD2 15, AMOD1 11, ASEL2 10:9, CSEL1
// 8:6, CSEL2 5:3, AMOD2 2; USE0 COP 19:18, AOP 17:16.
constexpr u64 Sop2Fields = 0x01308ffcull << 32 | 0xf0000ull;

struct Sop2Factor {
    u32 sel;
    u32 complement;
};

/// A PICA colour factor as a SOP2 CSEL, src1 the fragment and src2 the output buffer (0 zero,
/// 1 src1, 2 src2, 3 src1 alpha, 4 src2 alpha, 5 min(src1 alpha, 1 - src2 alpha); ONE is zero
/// complemented). Sel 7: none (the constant colour).
constexpr Sop2Factor ColourFactor(FramebufferRegs::BlendFactor f) {
    using F = FramebufferRegs::BlendFactor;
    switch (f) {
    case F::Zero:
        return {0, 0};
    case F::One:
        return {0, 1};
    case F::SourceColor:
        return {1, 0};
    case F::OneMinusSourceColor:
        return {1, 1};
    case F::DestColor:
        return {2, 0};
    case F::OneMinusDestColor:
        return {2, 1};
    case F::SourceAlpha:
        return {3, 0};
    case F::OneMinusSourceAlpha:
        return {3, 1};
    case F::DestAlpha:
        return {4, 0};
    case F::OneMinusDestAlpha:
        return {4, 1};
    case F::SourceAlphaSaturate:
        return {5, 0};
    default:
        return {7, 0};
    }
}

/// A PICA alpha factor as a SOP2 ASEL (0 zero, 1 src1 alpha, 2 src2 alpha).
constexpr Sop2Factor AlphaFactor(FramebufferRegs::BlendFactor f) {
    using F = FramebufferRegs::BlendFactor;
    switch (f) {
    case F::Zero:
        return {0, 0};
    case F::One:
    case F::SourceAlphaSaturate:
        return {0, 1};
    case F::SourceColor:
    case F::SourceAlpha:
        return {1, 0};
    case F::OneMinusSourceColor:
    case F::OneMinusSourceAlpha:
        return {1, 1};
    case F::DestColor:
    case F::DestAlpha:
        return {2, 0};
    case F::OneMinusDestColor:
    case F::OneMinusDestAlpha:
        return {2, 1};
    default:
        return {3, 0};
    }
}

/// A PICA blend equation as a SOP2 COP/AOP (0 add, 1 sub, 2 min, 3 max); 4 for ReverseSubtract.
constexpr u32 Sop2Op(FramebufferRegs::BlendEquation eq) {
    using Eq = FramebufferRegs::BlendEquation;
    switch (eq) {
    case Eq::Add:
        return 0;
    case Eq::Subtract:
        return 1;
    case Eq::Min:
        return 2;
    case Eq::Max:
        return 3;
    default:
        return 4;
    }
}

/// The blend SOP2 for the PICA's registers, or 0 for a blend no SOP2 does.
constexpr u64 BlendWord(FramebufferRegs::BlendEquation eq_rgb, FramebufferRegs::BlendEquation eq_a,
                        FramebufferRegs::BlendFactor src_rgb, FramebufferRegs::BlendFactor dst_rgb,
                        FramebufferRegs::BlendFactor src_a, FramebufferRegs::BlendFactor dst_a) {
    const Sop2Factor c1 = ColourFactor(src_rgb), c2 = ColourFactor(dst_rgb);
    const Sop2Factor a1 = AlphaFactor(src_a), a2 = AlphaFactor(dst_a);
    const u32 cop = Sop2Op(eq_rgb), aop = Sop2Op(eq_a);
    if (c1.sel == 7 || c2.sel == 7 || a1.sel == 3 || a2.sel == 3 || cop == 4 || aop == 4) {
        return 0;
    }
    const u64 use1 = c1.complement << 24 | a1.sel << 20 | c2.complement << 15 |
                     a1.complement << 11 | a2.sel << 9 | c1.sel << 6 | c2.sel << 3 |
                     a2.complement << 2;
    const u64 use0 = cop << 18 | aop << 16;
    return (BlendSourceAlpha & ~Sop2Fields) | use1 << 32 | use0;
}
static_assert(BlendWord(FramebufferRegs::BlendEquation::Add, FramebufferRegs::BlendEquation::Add,
                        FramebufferRegs::BlendFactor::SourceAlpha,
                        FramebufferRegs::BlendFactor::OneMinusSourceAlpha,
                        FramebufferRegs::BlendFactor::SourceAlpha,
                        FramebufferRegs::BlendFactor::OneMinusSourceAlpha) == BlendSourceAlpha);

// The dummy programs at the code bases: a secondary with no program of its own runs them.
const u32 DummyVertexSecondary = sgxd_program_doutu0(0, 0, 0);
const u32 DummyPixelSecondary = sgxd_program_doutu0(1, 0, 0);

/// The PICA's depth compare as the ISP's DCMP (same depth values, so the functions map by name).
u32 DepthCompare(FramebufferRegs::CompareFunc func) {
    using F = FramebufferRegs::CompareFunc;
    switch (func) {
    case F::Never:
        return 0;
    case F::LessThan:
        return 1;
    case F::Equal:
        return 2;
    case F::LessThanOrEqual:
        return 3;
    case F::GreaterThan:
        return 4;
    case F::NotEqual:
        return 5;
    case F::GreaterThanOrEqual:
        return 6;
    case F::Always:
        return 7;
    }
    UNREACHABLE();
}

/// The cull mode for the MTE (state word 21's CULLMODE), with the viewport's y scale positive.
u32 CullMode(Pica::RasterizerRegs::CullMode mode) {
    switch (mode) {
    case Pica::RasterizerRegs::CullMode::KeepClockWise:
        return 1;
    case Pica::RasterizerRegs::CullMode::KeepCounterClockWise:
        return 2;
    default:
        return 0;
    }
}

/// A program's words, primary and secondary, hashed: two builds' programs compared in the logs.
[[maybe_unused]] u64 ProgramHash(const Usse::Program& p) {
    return Common::HashCombine(
        Common::ComputeHash64(p.primary.data(), p.primary.size() * sizeof(p.primary[0])),
        Common::ComputeHash64(p.secondary.data(), p.secondary.size() * sizeof(p.secondary[0])));
}

/// The bool uniforms the program's flow control reads, a bit each (over the program's whole
/// extent, so over-approximate).
u16 UsedBoolUniforms(const Pica::ShaderSetup& setup) {
    using nihstro::Instruction;
    using nihstro::OpCode;
    const auto& code = setup.GetProgramCode();
    const u32 extent = std::min<u32>(setup.GetBiggestProgramSize(), Pica::MAX_PROGRAM_CODE_LENGTH);
    u16 used = 0;
    for (u32 offset = 0; offset < extent; offset++) {
        const Instruction instr = {code[offset]};
        switch (instr.opcode.Value()) {
        case OpCode::Id::IFU:
        case OpCode::Id::CALLU:
        case OpCode::Id::JMPU:
            used |= static_cast<u16>(1u << (instr.flow_control.bool_uniform_id & 15));
            break;
        default:
            break;
        }
    }
    return used;
}

/// Host builds: SGXD_PROGRAM_DUMP=<dir> writes every placed program's words there as they go to
/// the code heap. The words are the
/// console's, so the host shows what the GPU runs.
void DumpProgram([[maybe_unused]] const char* kind, [[maybe_unused]] u64 hash,
                 [[maybe_unused]] std::span<const u64> words) {
#ifndef __vita__
    static const char* const dir = std::getenv("SGXD_PROGRAM_DUMP");
    if (dir == nullptr || words.empty()) {
        return;
    }
    const std::string path = fmt::format("{}/{}_{:016x}.bin", dir, kind, hash);
    if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(words.data(), sizeof(u64), words.size(), f);
        std::fclose(f);
    }
#endif
}

void PutVec(float* out, const float* values, u32 count) {
    std::memcpy(out, values, count * sizeof(float));
}

/// A fragment uniform's floats, by the names fs_usse_gen's PlanUniforms gives them. Integers
/// are passed as floats, as the program reads them.
void FragmentUniformValues(const Pica::Shader::Generator::FSUniformData& d,
                           const Usse::GxpUniform& u, float* out) {
    const std::string& n = u.name;
    const auto ints = [&](const Common::Vec4i* rows, u32 count) {
        for (u32 i = 0; i < count; i++) {
            out[i * 4 + 0] = static_cast<float>(rows[i].x);
            out[i * 4 + 1] = static_cast<float>(rows[i].y);
            out[i * 4 + 2] = static_cast<float>(rows[i].z);
            out[i * 4 + 3] = static_cast<float>(rows[i].w);
        }
    };
    if (n == "alphatest_ref") {
        out[0] = static_cast<float>(d.alphatest_ref);
    } else if (n == "const_color") {
        ASSERT(u.array_size <= 6);
        for (u32 i = 0; i < u.array_size; i++) {
            PutVec(out + i * 4, d.const_color[i].AsArray(), 4);
        }
    } else if (n == "tev_combiner_buffer_color") {
        PutVec(out, d.tev_combiner_buffer_color.AsArray(), 4);
    } else if (n == "scissor_x1") {
        out[0] = static_cast<float>(d.scissor_x1);
    } else if (n == "scissor_y1") {
        out[0] = static_cast<float>(d.scissor_y1);
    } else if (n == "scissor_x2") {
        out[0] = static_cast<float>(d.scissor_x2);
    } else if (n == "scissor_y2") {
        out[0] = static_cast<float>(d.scissor_y2);
    } else if (n == "fog_lut_offset") {
        out[0] = static_cast<float>(d.fog_lut_offset);
    } else if (n == "fog_color") {
        PutVec(out, d.fog_color.AsArray(), 3);
    } else if (n == "tex_border_color") {
        ASSERT(u.array_size <= 3);
        for (u32 i = 0; i < u.array_size; i++) {
            PutVec(out + i * 4, d.tex_border_color[i].AsArray(), 4);
        }
    } else if (n == "proctex_lut_offset") {
        out[0] = static_cast<float>(d.proctex_lut_offset);
    } else if (n == "proctex_diff_lut_offset") {
        out[0] = static_cast<float>(d.proctex_diff_lut_offset);
    } else if (n == "proctex_color_map_offset") {
        out[0] = static_cast<float>(d.proctex_color_map_offset);
    } else if (n == "proctex_alpha_map_offset") {
        out[0] = static_cast<float>(d.proctex_alpha_map_offset);
    } else if (n == "proctex_bias") {
        out[0] = d.proctex_bias;
    } else if (n == "lighting_lut_offset") {
        ASSERT(u.array_size <= 6);
        ints(d.lighting_lut_offset, u.array_size);
    } else if (n == "lighting_global_ambient") {
        PutVec(out, d.lighting_global_ambient.AsArray(), 3);
    } else {
        UNREACHABLE_MSG("GXM: the fragment uniform {} has no value", n);
    }
}

} // Anonymous namespace

RasterizerGxm::RasterizerGxm(Pica::PicaCore& pica_, Memory::MemorySystem& memory_)
    : VideoCore::RasterizerAccelerated{memory_, pica_} {}

RasterizerGxm::~RasterizerGxm() = default;

void RasterizerSgxd::AddTriangle(const Pica::OutputVertex&, const Pica::OutputVertex&,
                                const Pica::OutputVertex&) {
    // The CPU vertex path. The default attribute index is 0xF while immediate-mode vertices
    // are submitted.
    const auto& pipeline = regs.pipeline;
    UNREACHABLE_MSG("sgxd: AddTriangle, a draw on the CPU vertex path: geometry shader {}, "
                    "topology {}, {} vertices, immediate mode {}",
                    pipeline.use_gs == Pica::PipelineRegs::UseGS::Yes ? "on" : "off",
                    TopologyName(pipeline.triangle_topology), pipeline.num_vertices,
                    pipeline.vs_default_attributes_setup.index == 0xF ? "yes" : "no");
}

void RasterizerGxm::DrawTriangles() {
    // Reached at the end of every CPU-path draw, even one with no triangle.
    const auto& pipeline = regs.pipeline;
    UNREACHABLE_MSG("sgxd: DrawTriangles, a draw on the CPU vertex path: geometry shader {}, "
                    "topology {}, {} vertices",
                    pipeline.use_gs == Pica::PipelineRegs::UseGS::Yes ? "on" : "off",
                    TopologyName(pipeline.triangle_topology), pipeline.num_vertices);
}

// The host surfaces are the only copy of what the passes drew; the display transfer writes
// them into guest memory, and nothing else reads them back.

void RasterizerSgxd::FlushAll() {}

void RasterizerSgxd::FlushRegion(PAddr, u32) {}

void RasterizerSgxd::InvalidateRegion(PAddr, u32) {}

void RasterizerSgxd::FlushAndInvalidateRegion(PAddr, u32) {}

void RasterizerSgxd::ClearAll(bool) {}

bool RasterizerGxm::AccelerateDisplayTransfer(const Pica::DisplayTransferConfig& config) {
    using Pica::PixelFormat;
    const PAddr input = config.GetPhysicalInputAddress();
    const PAddr output = config.GetPhysicalOutputAddress();
    const u32 width = config.output_width, height = config.output_height;
    const auto describe = [&] {
        return fmt::format("{}, format {} to {}, flags {:#x}", config.DebugName(),
                           static_cast<u32>(config.input_format.Value()),
                           static_cast<u32>(config.output_format.Value()), config.flags);
    };
    // simple_tri's transfer: a tiled RGBA8 buffer into a linear RGB8
    // one of the same size, no flip, no scaling.
    ASSERT_MSG(!config.input_linear && !config.dont_swizzle && !config.flip_vertically &&
                   !config.crop_input_lines && config.scaling == config.NoScale &&
                   config.input_width == width && config.input_height == height,
               "GXM: AccelerateDisplayTransfer {}: only a tiled-to-linear copy of one size is "
               "built",
               describe());
    ASSERT_MSG(config.input_format == PixelFormat::RGBA8 && config.output_format == PixelFormat::RGB8,
               "GXM: AccelerateDisplayTransfer {}: only RGBA8 to RGB8 is built", describe());
    const u32 bytes = width * height * 3;
    const bool fcram = output >= Memory::FCRAM_PADDR && output + bytes <= Memory::FCRAM_PADDR_END;
    ASSERT_MSG(fcram || (output >= Memory::VRAM_PADDR && output + bytes <= Memory::VRAM_PADDR_END),
               "GXM: AccelerateDisplayTransfer {}: the output is outside the mapped guest memory",
               describe());
    if (backend.InPass()) {
        EndPass("the display transfer");
    }
    const auto source = std::find_if(surfaces.begin(), surfaces.end(),
                                     [&](const Surface& s) { return s.addr == input; });
    ASSERT_MSG(source != surfaces.end() && source->width == width && source->height == height,
               "GXM: AccelerateDisplayTransfer {}: no {}x{} surface at {:#x}; a picture the CPU "
               "drew needs GPU untiling, which is not built",
               describe(), width, height, input);

    // The surface's row y is the PICA's row y. The PICA's RGB8 is B, G, R in memory, gxm's
    // U8U8U8_RGB store. Guest memory is mapped for the GPU at its PICA address; VRAM is
    // uncached CDRAM and needs no cache work.
    u8* const out = memory.GetPhysicalPointer(output);
    const SgxdBackend::Store store{output, fcram ? out : nullptr, fcram ? bytes : 0,
                                   SGXD_RECIPE_PACK_U888_RGB};
    u32 ta_us = 0, d3_us = 0;
    backend.Blit(source->memory, width, height, store, &ta_us, &d3_us);
    // Progress: a line every 30 transfers.
    if (transfers > 0 && transfers % 30 == 0) {
        LOG_INFO(Render, "transfer {}: TA {} us, 3D {} us", transfers + 1, ta_us, d3_us);
    }
    // gxm_flags dumpat=N: transfers N and N + 1 checked and written, to compare runs frame for
    // frame. Only then: the check waits for the render and the write holds the emulation thread.
    static const u32 dump_at = GxmFlagValue("dumpat");
    const bool dump = dump_at != 0 && transfers + 1 >= dump_at && transfers + 1 < dump_at + 2;
    transfers++;
    if (dump) {
        const auto pixel = [&](u32 x, u32 y) {
            const u8* p = out + (y * width + x) * 3;
            return static_cast<u32>(p[2]) << 16 | static_cast<u32>(p[1]) << 8 | p[0];
        };
        // Every byte against the PICA's RGB8 of the surface (B, G, R from R, G, B, A).
        u32 wrong = 0;
        for (u32 n = 0; n < width * height; n++) {
            const u8* in = source->memory.cpu + n * 4;
            const u8* got = out + n * 3;
            wrong += (got[0] != in[2]) + (got[1] != in[1]) + (got[2] != in[0]);
        }
        LOG_INFO(Render,
                 "transfer {}: {:#x} to {:#x} ({}), {}x{} RGB8, TA {} us, 3D {} us; {} of {} bytes "
                 "differ from the surface's RGB8; as R G B: centre {:06x}, corners {:06x} {:06x} "
                 "{:06x} {:06x}",
                 transfers, input, output, fcram ? "FCRAM" : "VRAM", width, height, ta_us, d3_us,
                 wrong, bytes, pixel(width / 2, height / 2), pixel(0, 0), pixel(width - 1, 0),
                 pixel(0, height - 1), pixel(width - 1, height - 1));
        // The frame as a PPM (R, G, B, rows as in memory) where the Vita build has a host0;
        // nothing without one.
#ifdef __vita__
        const std::string path = fmt::format("host0:azahar-transfer-{}.ppm", transfers);
        if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
            std::fprintf(f, "P6\n%u %u\n255\n", width, height);
            std::vector<u8> rgb(bytes);
            for (u32 n = 0; n < width * height; n++) {
                rgb[n * 3] = out[n * 3 + 2];
                rgb[n * 3 + 1] = out[n * 3 + 1];
                rgb[n * 3 + 2] = out[n * 3];
            }
            std::fwrite(rgb.data(), 1, rgb.size(), f);
            std::fclose(f);
            LOG_INFO(Render, "transfer {}: written to {}", transfers, path);
        }
#endif
        // The picture as a map, a character per 16x16 block from its centre pixel: '.' the
        // first corner's value, '#' white, '+' anything else; a row of blocks a line.
        const u32 background = pixel(0, 0);
        for (u32 ty = 0; ty < height / 16; ty++) {
            std::string row;
            for (u32 tx = 0; tx < width / 16; tx++) {
                const u32 v = pixel(tx * 16 + 8, ty * 16 + 8);
                row += v == background ? '.' : v == 0xffffffu ? '#' : '+';
            }
            LOG_INFO(Render, "transfer {} rows {:2}: |{}|", transfers, ty, row);
        }
    }
    return true;
}

// P3D tells the guest it may overwrite the vertex and index buffers, which the open pass reads
// at its TA: the pass renders now. TA append (H5) will later
// run only the TA here.
// P3D goes to the guest right after, as upstream's does (GPU::Signal): the pass's TA may still
// be reading the vertices and indices the guest is now free to overwrite.
void RasterizerSgxd::NotifyP3D() {
    const bool in_pass = backend.InPass();
    if (in_pass) {
        EndPass("P3D");
    }
    if (p3ds++ < 4 || p3ds % 60 == 0) {
        LOG_INFO(Render, "P3D {}: {}; {} passes so far", p3ds,
                 in_pass ? "ended the open pass" : "no pass open", passes);
    }
}

bool RasterizerGxm::AccelerateTextureCopy(const Pica::DisplayTransferConfig& config) {
    UNREACHABLE_MSG("GXM: AccelerateTextureCopy from {:#x} to {:#x}, {} bytes, input "
                    "width {} gap {}, output width {} gap {}",
                    config.GetPhysicalInputAddress(), config.GetPhysicalOutputAddress(),
                    config.texture_copy.size, config.texture_copy.input_width.Value(),
                    config.texture_copy.input_gap.Value(),
                    config.texture_copy.output_width.Value(),
                    config.texture_copy.output_gap.Value());
}

bool RasterizerGxm::AccelerateFill(const Pica::MemoryFillConfig& config) {
    const PAddr start = config.GetStartAddress();
    const PAddr end = config.GetEndAddress();
    const u32 width_bits = config.fill_32bit ? 32 : (config.fill_24bit ? 24 : 16);
    const auto describe = [&] {
        return fmt::format("{:#x}-{:#x} ({}) with {:#x}, {}-bit", start, end,
                           config.IsVRAM() ? "VRAM" : "FCRAM", config.value_32bit, width_bits);
    };
    ASSERT_MSG(config.fill_32bit, "GXM: AccelerateFill {}: only 32-bit fills are built",
               describe());
    ASSERT_MSG(end > start && (end - start) % 4 == 0,
               "GXM: AccelerateFill {}: not a whole number of words", describe());
    // An FCRAM destination would need the CPU cache cleaned and invalidated around the PTLA: not built.
    ASSERT_MSG(start >= Memory::VRAM_PADDR && end <= Memory::VRAM_PADDR_END,
               "GXM: AccelerateFill {}: only guest VRAM is filled on the GPU", describe());

    // A fill over the open pass's buffers comes after its draws: the pass renders first.
    if (backend.InPass()) {
        const Surface& s = surfaces[pass_surface];
        const PAddr colour_end = s.addr + s.width * s.height * 4;
        const PAddr depth_end = pass_depth + s.width * s.height * 4;
        if ((start < colour_end && s.addr < end) ||
            (pass_depth_cleared && start < depth_end && pass_depth < end)) {
            EndPass("a fill over its buffers");
        }
    }

    // The range as rows of 4096 pixels (16 KiB, under the PTLA's 64 KiB stride) and a last
    // partial row, filled in place at its PICA address, which is its GPU address.
    constexpr u32 RowPixels = 4096;
    const u32 pixels = (end - start) / 4;
#ifdef __vita__
    const SgxdPtlaFill fill{start,           SGXD_PTLA_8888,     RowPixels * 4, RowPixels,
                            pixels / RowPixels, pixels % RowPixels, config.value_32bit};
    char failed[160];
    u32 elapsed_us = 0;
    ASSERT_MSG(sgxd_ptla_fill(&fill, &elapsed_us, failed, sizeof(failed)) == 0,
               "GXM: AccelerateFill {}: {}", describe(), failed);
#else
    // The host build has no GPU: the fill runs on the CPU so guest memory stays meaningful.
    u8* const dest = memory.GetPhysicalPointer(start);
    for (u32 i = 0; i < pixels; i++) {
        std::memcpy(dest + i * 4, &config.value_32bit, 4);
    }
    const u32 elapsed_us = 0;
#endif
    RecordClear({start, end, config.value_32bit, 4, 0});

    // The first fills are logged with what the guest's view of VRAM holds at both ends.
    if (fills++ < 4) {
        const u8* view = memory.GetPhysicalPointer(start);
        u32 first = 0, last = 0;
        std::memcpy(&first, view, 4);
        std::memcpy(&last, view + (end - start) - 4, 4);
        LOG_INFO(Render, "fill {}: {} in {} us; the guest's view holds {:#010x} ... {:#010x}",
                 fills, describe(), elapsed_us, first, last);
    }
    return true;
}

void RasterizerSgxd::RecordClear(const ClearedRange& range) {
    ForgetClears(range.start, range.end);
    u32 slot = clear_count;
    if (slot == MaxClears) {
        slot = 0;
        for (u32 i = 1; i < MaxClears; i++) {
            if (cleared[i].order < cleared[slot].order) {
                slot = i;
            }
        }
    } else {
        clear_count++;
    }
    cleared[slot] = range;
    cleared[slot].order = clear_order++;
}

void RasterizerSgxd::ForgetClears(PAddr start, PAddr end) {
    for (u32 i = 0; i < clear_count;) {
        if (cleared[i].start < end && start < cleared[i].end) {
            cleared[i] = cleared[--clear_count];
        } else {
            i++;
        }
    }
}

bool RasterizerSgxd::TakeClear(PAddr start, PAddr end, ClearedRange& out) {
    for (u32 i = 0; i < clear_count; i++) {
        if (cleared[i].start <= start && end <= cleared[i].end) {
            out = cleared[i];
            cleared[i] = cleared[--clear_count];
            return true;
        }
    }
    return false;
}

RasterizerGxm::Surface& RasterizerGxm::SurfaceFor(PAddr addr, u32 width, u32 height) {
    for (Surface& s : surfaces) {
        if (s.addr == addr) {
            ASSERT_MSG(s.width == width && s.height == height,
                       "GXM: the colour buffer at {:#x} was {}x{} and is now {}x{}: a surface "
                       "that changes size is not built",
                       addr, s.width, s.height, width, height);
            return s;
        }
    }
    surfaces.push_back({addr, width, height, backend.AllocSurface(width, height), 0});
    LOG_INFO(Render, "surface {:#x}: {}x{} RGBA8 at GPU {:#010x}", addr, width, height,
             surfaces.back().memory.gpu);
    return surfaces.back();
}

void RasterizerSgxd::EndPass(const char* why) {
    Surface& s = surfaces[pass_surface];
    const u32 pass_draws = backend.PassDraws();
    u32 ta_us = 0, d3_us = 0;
    backend.EndPass(&ta_us, &d3_us);
    s.passes++;
    if (passes++ < 8) {
        LOG_INFO(Render, "pass {} on {:#x}: {} draws, TA {} us, 3D {} us; ended by {}", passes,
                 s.addr, pass_draws, ta_us, d3_us, why);
    }
}

#ifdef VITA_DIAGNOSTICS
void RasterizerSgxd::LogTextureStats() {
    static const bool perflog = SgxdFlag("perflog");
    if (!perflog) {
        return;
    }
    TextureStats& t = texture_stats;
    const u64 now = sceKernelGetProcessTimeWide();
    if (t.start_us == 0) {
        t.start_us = now;
        return;
    }
    if (now - t.start_us < 1000000) {
        return;
    }
    LOG_INFO(Render,
             "perf: textures in {:.2f} s: {} change checks ({} found new bytes) over {} KiB in "
             "{:.1f} ms; {} untiled ({} KiB); {} cached",
             (now - t.start_us) / 1e6, t.checks, t.changed, t.hashed_bytes / 1024,
             t.hash_us / 1e3, t.untiles, t.untiled_bytes / 1024, textures.size());
    t = TextureStats{};
    t.tiny_logged = tiny_logged;
    t.start_us = now;
    t.passes_mark = passes;
    t.evictions_mark = evictions;
}

void RasterizerGxm::NoteBankBuild(const Pica::ShaderSetup& setup, u32 rows) {
    static const bool bankstats = GxmFlag("bankstats");
    if (!bankstats) {
        return;
    }
    BankLutStats& b = bank_lut_stats;
    b.bank_builds++;
    b.bank_rows += rows;
    constexpr std::array<u32, 3> Back{1, 8, 32};
    for (u32 k = 0; k < Back.size(); k++) {
        // Before that many builds: the version that bank held (0, everything, at the start).
        const u32 before = b.build_count >= Back[k]
                               ? b.build_versions[(b.build_count - Back[k]) % b.build_versions.size()]
                               : 0;
        u32 changed = 0;
        for (const u32 stamp : setup.f_stamp) {
            changed += stamp > before ? 1 : 0;
        }
        b.changed_since[k] += changed;
    }
    b.build_versions[b.build_count % b.build_versions.size()] = setup.f_version;
    b.build_count++;
}

void RasterizerGxm::NoteLutTable(u32 index, bool ring_move) {
    static const bool bankstats = GxmFlag("bankstats");
    if (!bankstats) {
        return;
    }
    BankLutStats& b = bank_lut_stats;
    if (ring_move) {
        // Contents unchanged, rewritten into the half moved to.
        b.lut_ring_moves++;
        return;
    }
    b.lut_tables++;
    const auto& lut = pica.lighting.luts[index];
    std::array<u32, 256> words;
    static_assert(sizeof(words) == sizeof(lut));
    std::memcpy(words.data(), lut.data(), sizeof(words));
    if (b.lut_shadow.empty()) {
        b.lut_shadow.resize(pica.lighting.luts.size());
    }
    u32 changed = 0;
    for (u32 i = 0; i < words.size(); i++) {
        changed += words[i] != b.lut_shadow[index][i] ? 1 : 0;
    }
    b.lut_entries_changed += changed;
    b.lut_shadow[index] = words;
    const u64 hash = Common::ComputeHash64(words.data(), sizeof(words));
    const u32 known = std::min<u32>(b.lut_hash_count, static_cast<u32>(b.lut_hashes.size()));
    for (u32 back = 1; back <= known; back++) {
        if (b.lut_hashes[(b.lut_hash_count - back) % b.lut_hashes.size()] == hash) {
            b.lut_seen_64 += back <= 64 ? 1 : 0;
            b.lut_seen_256++;
            break;
        }
    }
    b.lut_hashes[b.lut_hash_count % b.lut_hashes.size()] = hash;
    b.lut_hash_count++;
}
#endif

namespace {

/// With gxm_flags dumpgxp, an emitted program as a GXP container on host0, for
/// psp2shaderperf -disasm; `kind` is vs or fs, `key` the program cache's.
void DumpGxp(const char* kind, u64 key, const Usse::GxpProgram& gxp) {
    if (!GxmFlag("dumpgxp")) {
        return;
    }
    const std::vector<uint8_t> bytes = Usse::WriteGxp(gxp);
    const std::string path = fmt::format("host0:azahar-{}-{:016x}.gxp", kind, key);
    if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);
        LOG_INFO(Render, "{} program {:016x}: written to {}", kind, key, path);
    }
}

} // namespace

const RasterizerSgxd::VertexProgram& RasterizerSgxd::VertexProgramFor(
    u16 default_regs, const std::array<u8, 16>& reg_components, u16 u8_regs) {
    const Pica::Shader::Generator::PicaVSConfig config{regs, pica.vs_setup};
    const auto& state = config.state;
    // The bools the program branches on are baked in; which ones is found once per program.
    auto used = used_bools.find(state.program_hash);
    if (used == used_bools.end()) {
        used = used_bools.emplace(state.program_hash, UsedBoolUniforms(pica.vs_setup)).first;
    }
    u16 bools = 0;
    for (u32 n = 0; n < 16; n++) {
        bools |= static_cast<u16>((pica.vs_setup.uniforms.b[n] ? 1u : 0u) << n);
    }
    bools &= used->second;
    const u64 key = Common::HashCombine(
        config.Hash(), static_cast<u64>(u8_regs) << 16 | default_regs,
        static_cast<u64>(bools) << 16 | used->second,
        Common::ComputeHash64(reg_components.data(), reg_components.size()));
    if (const auto it = vertex_programs.find(key); it != vertex_programs.end()) {
        return it->second;
    }

    Usse::VsRequest request{};
    request.code = pica.vs_setup.GetProgramCode();
    request.swizzle = pica.vs_setup.GetSwizzleData();
    request.code_size =
        std::min<u32>(pica.vs_setup.GetBiggestProgramSize(), Pica::MAX_PROGRAM_CODE_LENGTH);
    request.main_offset = state.main_offset;
    request.num_outputs = state.num_outputs;
    request.output_map = state.output_map;
    const auto maps = state.gs_state.GetSemanticMaps();
    for (u32 i = 0; i < maps.size(); i++) {
        request.semantics[i] = {static_cast<u8>(std::min<u32>(maps[i].attribute_index, 255)),
                                static_cast<u8>(maps[i].component_index & 3)};
    }
    request.default_regs = default_regs;
    request.bools = bools;
    request.bool_mask = used->second;
    request.reg_components = reg_components;
    request.u8_regs = u8_regs;

    VertexProgram v{};
    std::string refusal;
    auto gxp = Usse::EmitVertexGxp(request, v.input_regs, &refusal);
    ASSERT_MSG(gxp.has_value(), "GXM: vertex program {:016x}: the emitter has no program for it: {}",
               state.program_hash, refusal);
    v.gxp = std::move(*gxp);
    DumpGxp("vs", key, v.gxp);
    DumpProgram("vs", key, v.gxp.primary);
    DumpProgram("vsec", key, v.gxp.secondary);
    v.primary_doutu0 =
        sgxd_program_doutu0(0, backend.PlaceCode(0, v.gxp.primary), v.gxp.temp_count);
    v.secondary_doutu0 = v.gxp.secondary.empty()
                             ? DummyVertexSecondary
                             : sgxd_program_doutu0(0, backend.PlaceCode(0, v.gxp.secondary), 0);
    LOG_INFO(Render,
             "vertex program {:016x}: {} + {} words, {} PAs, {} temps, {} SAs, {} uniforms{}, "
             "emitted in {} us",
             state.program_hash, v.gxp.primary.size(), v.gxp.secondary.size(), v.gxp.pa_count,
             v.gxp.temp_count, v.gxp.secondary_reg_count, v.gxp.uniforms.size(),
             v.gxp.per_instance ? ", per instance" : "");
    return vertex_programs.emplace(key, std::move(v)).first->second;
}

const RasterizerGxm::FragmentProgram& RasterizerGxm::FragmentProgramFor(u64 blend) {
    const Pica::Shader::FSConfig config{regs};
    const u64 key = Common::HashCombine(config.Hash(), blend);
    if (const auto it = fragment_programs.find(key); it != fragment_programs.end()) {
        return it->second;
    }
    FragmentProgram f{};
    std::string refusal;
    auto gxp = Usse::EmitFragmentGxp(config, &refusal);
    ASSERT_MSG(gxp.has_value(), "GXM: the emitter has no fragment program for this state: {}",
               refusal);
    f.gxp = std::move(*gxp);
    DumpGxp("fs", key, f.gxp);
#ifdef VITA_DIAGNOSTICS
    // fsload: the primary program's words from host0:fsload.gxp instead of ours, with
    // our inputs, uniforms and samplers; its temps if more. For timing another compiler's code on
    // the same frames: what it reads from our registers is whatever they hold.
    if (GxmFlag("fsload")) {
        std::FILE* file = std::fopen("host0:fsload.gxp", "rb");
        ASSERT_MSG(file, "GXM: fsload: no host0:fsload.gxp");
        std::vector<u8> bytes;
        u8 chunk[4096];
        for (std::size_t n; (n = std::fread(chunk, 1, sizeof(chunk), file)) > 0;) {
            bytes.insert(bytes.end(), chunk, chunk + n);
        }
        std::fclose(file);
        // SceGxmProgram: temps at 0x34, the primary program's count at 0x3c and its offset,
        // relative to that field, at 0x40.
        u32 temps, count, offset;
        ASSERT_MSG(bytes.size() >= 0x44 && std::memcmp(bytes.data(), "GXP", 4) == 0,
                   "GXM: fsload: not a GXP");
        std::memcpy(&temps, bytes.data() + 0x34, 4);
        std::memcpy(&count, bytes.data() + 0x3c, 4);
        std::memcpy(&offset, bytes.data() + 0x40, 4);
        ASSERT_MSG(0x40 + offset + 8ull * count <= bytes.size(), "GXM: fsload: short GXP");
        f.gxp.primary.resize(count);
        std::memcpy(f.gxp.primary.data(), bytes.data() + 0x40 + offset, 8ull * count);
        f.gxp.temp_count = std::max(f.gxp.temp_count, temps);
        f.gxp.phase_starts = {0};
        LOG_INFO(Render, "fsload: fragment program {:016x}: {} words, {} temps", hash, count,
                 f.gxp.temp_count);
    }
#endif
    const Usse::GxpProgram& g = f.gxp;
    // Not built yet: a second phase (a kill or a depth write) needs its PHAS to carry the next
    // phase's address, which the patcher relocates; samplers need texture state words; a memory
    // uniform buffer needs its pointer.
    ASSERT_MSG(g.phase_starts.size() == 1,
               "GXM: a fragment program of {} phases (kill {}, depth write {}): not built",
               g.phase_starts.size(), g.uses_discard, g.writes_depth);
    if (!g.samplers.empty()) {
        std::string names;
        for (const Usse::GxpSampler& sampler : g.samplers) {
            names += fmt::format(" {} (unit {})", sampler.name, sampler.unit);
        }
        UNREACHABLE_MSG("GXM: a fragment program with samplers:{}; not built", names);
    }
    ASSERT_MSG(!g.buffer_pointer, "GXM: a fragment program with memory uniforms: not built");

    std::vector<u64> code = g.primary;
    if (blend) {
        code.push_back(PackInPlace);
        code.push_back(blend);
    } else {
        code.push_back(PackToOutput);
    }
    DumpProgram("fs", key, code);
    DumpProgram("fsec", key, g.secondary);
    const u32 doutu0 = sgxd_program_doutu0(1, backend.PlaceCode(1, code), g.temp_count);
    f.secondary_doutu0 = g.secondary.empty()
                             ? DummyPixelSecondary
                             : sgxd_program_doutu0(1, backend.PlaceCode(1, g.secondary), 0);
    f.primary_doutu0 = doutu0;

    std::array<u32, 12> iterators{};
    std::array<unsigned, 12> words{};
    ASSERT(g.inputs.size() <= iterators.size());
    u32 iterated = 0;
    for (size_t i = 0; i < g.inputs.size(); i++) {
        iterators[i] = Usse::VaryingDescriptor(g, i);
        words[i] = g.inputs[i].components;
        iterated += g.inputs[i].components;
    }
    SgxdPdsProgram pds{};
    ASSERT(sgxd_program_pixel_pds(doutu0, iterators.data(), words.data(),
                                  static_cast<unsigned>(g.inputs.size()), &pds) == 0);
    f.fprim = backend.PlacePds(pds);
    f.fprim_units = pds.units;
    // The pack reads pa0 and pa1 (four F16), the iterators write from pa0.
    f.pixel_pa = std::max<u32>({g.pa_count, iterated, 2u});
    LOG_INFO(Render,
             "fragment program {:016x}, blend {:016x}: {} words, {} inputs, {} PAs, {} temps, "
             "{} uniform floats, {} literals",
             config.Hash(), blend, code.size(), g.inputs.size(), f.pixel_pa,
             g.temp_count, g.uniform_floats, g.literals.size());
    return fragment_programs.emplace(key, std::move(f)).first->second;
}

bool RasterizerSgxd::AccelerateDrawBatch(bool is_indexed) {
    using Topology = Pica::PipelineRegs::TriangleTopology;
    const auto& pipeline = regs.pipeline;
    const auto describe = [&] {
        return fmt::format("{} draw, topology {}, {} vertices",
                           is_indexed ? "indexed" : "non-indexed",
                           TopologyName(pipeline.triangle_topology), pipeline.num_vertices);
    };
    ASSERT_MSG(!is_indexed, "GXM: {}: indexed draws are not built", describe());
    ASSERT_MSG(pipeline.use_gs == Pica::PipelineRegs::UseGS::No,
               "GXM: {}: geometry shaders are not built", describe());
    u32 primitive = SGXD_PROGRAM_TRIANGLES;
    switch (pipeline.triangle_topology) {
    case Topology::List:
        break;
    case Topology::Strip:
        primitive = SGXD_PROGRAM_STRIP;
        break;
    case Topology::Fan:
        primitive = SGXD_PROGRAM_FAN;
        break;
    default:
        UNREACHABLE_MSG("sgxd: {}: that topology is not built", describe());
    }
    const u32 count = pipeline.num_vertices;
    ASSERT_MSG(count > 0 && count <= SGXD_RECIPE_MAX_INDICES,
               "GXM: {}: more vertices than the identity index list", describe());
    SyncDrawUniforms();

    // --- the target and the output merger ---
    const auto& fb = regs.framebuffer.framebuffer;
    const auto& om = regs.framebuffer.output_merger;
    ASSERT_MSG(fb.color_format == FramebufferRegs::ColorFormat::RGBA8,
               "GXM: {}: colour format {} (RGBA8 only)", describe(),
               static_cast<u32>(fb.color_format.Value()));
    ASSERT_MSG(fb.allow_color_write != 0 && om.red_enable && om.green_enable &&
                   om.blue_enable && om.alpha_enable,
               "GXM: {}: a colour write mask is not built", describe());
    ASSERT_MSG(om.fragment_operation_mode == FramebufferRegs::FragmentOperationMode::Default,
               "GXM: {}: gas and shadow rendering are not built", describe());
    ASSERT_MSG(!om.stencil_test.enable, "GXM: {}: the stencil test is not built", describe());
    const bool blend = om.alphablend_enable != 0;
    u64 blend_word = 0;
    if (blend) {
        const auto& b = om.alpha_blending;
        using Eq = FramebufferRegs::BlendEquation;
        using Factor = FramebufferRegs::BlendFactor;
        ASSERT_MSG(b.blend_equation_rgb == Eq::Add && b.blend_equation_a == Eq::Add &&
                       b.factor_source_rgb == Factor::SourceAlpha &&
                       b.factor_dest_rgb == Factor::OneMinusSourceAlpha &&
                       b.factor_source_a == Factor::SourceAlpha &&
                       b.factor_dest_a == Factor::OneMinusSourceAlpha,
                   "GXM: {}: blend {}/{} {}/{} {}/{}: only ADD with SRC_ALPHA, "
                   "ONE_MINUS_SRC_ALPHA is built",
                   describe(), static_cast<u32>(b.blend_equation_rgb.Value()),
                   static_cast<u32>(b.blend_equation_a.Value()),
                   static_cast<u32>(b.factor_source_rgb.Value()),
                   static_cast<u32>(b.factor_dest_rgb.Value()),
                   static_cast<u32>(b.factor_source_a.Value()),
                   static_cast<u32>(b.factor_dest_a.Value()));
    } else {
        ASSERT_MSG(om.logic_op == FramebufferRegs::LogicOp::Copy,
                   "GXM: {}: logic op {} is not built", describe(),
                   static_cast<u32>(om.logic_op.Value()));
    }
    // As upstream: the test runs only when enabled, the write whenever enabled and allowed.
    const u32 dcmp = om.depth_test_enable ? DepthCompare(om.depth_test_func) : 7;
    const bool dwrite = fb.allow_depth_stencil_write != 0 && om.depth_write_enable;
    const bool uses_depth = dcmp != 7 || dwrite;

    const PAddr colour = fb.GetColorBufferPhysicalAddress();
    const PAddr depth = fb.GetDepthBufferPhysicalAddress();
    const u32 width = fb.GetWidth(), height = fb.GetHeight();
    if (backend.InPass() && (surfaces[pass_surface].addr != colour ||
                             (uses_depth && pass_depth != depth))) {
        EndPass("a draw to other buffers");
    }
    if (!backend.InPass()) {
        Surface& s = SurfaceFor(colour, width, height);
        ClearedRange clear{};
        ASSERT_MSG(TakeClear(colour, colour + width * height * 4, clear),
                   "GXM: {}: the colour buffer at {:#x} has no fill pending, and loading its "
                   "picture into a pass is not built",
                   describe(), colour);
        ClearedRange depth_clear{};
        pass_depth = depth;
        pass_depth_cleared = false;
        float bg_depth = 1.0f;
        if (fb.depth_format == FramebufferRegs::DepthFormat::D24S8 &&
            TakeClear(depth, depth + width * height * 4, depth_clear)) {
            pass_depth_cleared = true;
            bg_depth = static_cast<float>(depth_clear.value & 0xffffff) / 16777215.0f;
        }
        pass_surface = static_cast<std::size_t>(&s - surfaces.data());
        backend.BeginPass(s.memory, width, height, Common::swap32(clear.value), bg_depth);
    }
    ASSERT_MSG(!uses_depth || pass_depth_cleared,
               "GXM: {}: the depth buffer at {:#x} (format {}) has no D24S8 fill pending, and "
               "loading depth into a pass is not built",
               describe(), depth, static_cast<u32>(fb.depth_format.Value()));

    // --- the vertex fetch: every loader's attributes at their PICA addresses ---
    const auto& va = pipeline.vertex_attributes;
    const PAddr base = va.GetPhysicalBaseAddress();
    struct Feed {
        u32 reg;
        PAddr address;
        u32 stride;
        u32 words;
    };
    std::array<Feed, 16> feeds{};
    u32 feed_count = 0;
    u16 enabled_regs = 0;
    std::array<u8, 16> reg_components{};
    u16 u8_regs = 0;
    for (u32 loader_index = 0; loader_index < 12; loader_index++) {
        const auto& loader = va.attribute_loaders[loader_index];
        if (loader.component_count == 0 || loader.byte_count == 0) {
            continue;
        }
        u32 offset = 0;
        for (u32 comp = 0; comp < loader.component_count && comp < 12; ++comp) {
            const u32 attribute_index = loader.GetComponent(comp);
            if (attribute_index >= 12) {
                // Attribute ids 12, 13, 14 and 15 signify 4, 8, 12 and 16-byte paddings.
                offset = Common::AlignUp(offset, 4);
                offset += (attribute_index - 11) * 4;
                continue;
            }
            const u32 elements = va.GetNumElements(attribute_index);
            if (elements == 0) {
                continue;
            }
            offset = Common::AlignUp(offset, va.GetElementSizeInBytes(attribute_index));
            ASSERT_MSG(va.GetFormat(attribute_index) ==
                           Pica::PipelineRegs::VertexAttributeFormat::FLOAT,
                       "GXM: {}: attribute {} is format {}; only F32 attributes are built",
                       describe(), attribute_index,
                       static_cast<u32>(va.GetFormat(attribute_index)));
            const u32 reg = regs.vs.GetRegisterForAttribute(attribute_index);
            const PAddr address = base + loader.data_offset + offset;
            // A U8 attribute is fetched as the word its bytes start: the DMA moves words.
            const u32 stride = loader.byte_count;
            DEBUG_ASSERT_MSG(!is_u8 || ((address | stride) & 3) == 0,
                             "GXM: {}: U8 attribute {} at {:#x}, stride {}: not word-aligned",
                             describe(), attribute_index, address, stride);
            // Two loaders feeding one register is a guest bug; the last wins, as in GL.
            const auto same = std::find_if(feeds.begin(), feeds.begin() + feed_count,
                                           [&](const Feed& f) { return f.reg == reg; });
            const Feed feed{reg, address, stride, is_u8 ? 1u : elements};
            u8_regs = static_cast<u16>(is_u8 ? u8_regs | 1u << reg : u8_regs & ~(1u << reg));
            if (same != feeds.begin() + feed_count) {
                *same = feed;
            } else {
                feeds[feed_count++] = feed;
            }
            enabled_regs |= static_cast<u16>(1u << reg);
            reg_components[reg] = static_cast<u8>(elements);
            offset += va.GetStride(attribute_index);
        }
    }
    // Every register no loader feeds is a uniform to the program (vs_in_regN): the fixed default
    // attribute's value where the guest set one, (0, 0, 0, 1) otherwise.
    const u16 default_regs = static_cast<u16>(~enabled_regs);
    const VertexProgram& vp = VertexProgramFor(default_regs, reg_components, u8_regs);
    const Usse::GxpProgram& vg = vp.gxp;
#ifdef VITA_DIAGNOSTICS
    // drawring: the last draws, printed when a render stops (SgxdBackend::fault_hook).
    static const bool draw_ring_on = GxmFlag("drawring");
    if (draw_ring_on) {
        // Before the hook already there (sgxd_present's), which PrintDrawRing calls after.
        if (SgxdBackend::fault_hook != PrintDrawRing) {
            draw_ring_chained = SgxdBackend::fault_hook;
            SgxdBackend::fault_hook = PrintDrawRing;
        }
        // The draw's PICA programs, each kept once for the fault's azahar-sgxd-programs.bin,
        // and the integer and bool uniforms a loop's count and a branch come from.
        std::string units;
        for (Pica::ShaderSetup* setup : {&pica.vs_setup, &pica.gs_setup}) {
            if (setup == &pica.gs_setup && pipeline.use_gs != Pica::PipelineRegs::UseGS::Yes) {
                break;
            }
            const u64 hash = setup->GetProgramCodeHash();
            if (!ring_programs.contains(hash)) {
                std::vector<u32>& keep = ring_programs[hash];
                keep.push_back(setup->entry_point);
                keep.insert(keep.end(), setup->GetProgramCode().begin(),
                            setup->GetProgramCode().end());
                keep.insert(keep.end(), setup->GetSwizzleData().begin(),
                            setup->GetSwizzleData().end());
            }
            const auto& u = setup->uniforms;
            u32 b = 0;
            for (u32 k = 0; k < 16; k++) {
                b |= (u.b[k] ? 1u : 0u) << k;
            }
            units += fmt::format("; {} {:016x} entry {:#x} b {:04x} i", setup == &pica.vs_setup ? "VS" : "GS",
                                 hash, setup->entry_point, b);
            for (const auto& i : u.i) {
                units += fmt::format(" {},{},{}", i.x, i.y, i.z);
            }
        }
        draw_ring[draw_ring_next++ % draw_ring.size()] =
            fmt::format("pass {} draw {}: {}; VS {:016x}{} doutu {:#010x}, {} temps, secondary "
                        "{:#010x}{}",
                        passes, backend.PassDraws() + 1, describe(),
                        const_cast<Pica::ShaderSetup*>(stage.setup)->GetProgramCodeHash(),
                        pipeline.use_gs == Pica::PipelineRegs::UseGS::Yes ? " (GS)" : "",
                        vp.primary_doutu0, vg.temp_count, vp.secondary_doutu0, units);
    }
#endif

#ifdef VITA_DIAGNOSTICS
    // skipfrom=N / skipto=M: a pass's draws N..M (from 1) are left out, a hang's bisection.
    // Counted per pass with the draws left out (PassDraws counts only those drawn).
    static const u32 skip_from = GxmFlagValue("skipfrom");
    static const u32 skip_to = GxmFlagValue("skipto");
    static u32 skip_pass = ~0u, skip_n = 0;
    if (skip_from != 0) {
        if (skip_pass != passes) {
            skip_pass = passes;
            skip_n = 0;
        }
        skip_n++;
        if (skip_n >= skip_from && (skip_to == 0 || skip_n <= skip_to)) {
            return true;
        }
    }
#endif

    std::array<SgxdVertexFetch, SGXD_PROGRAM_MAX_FETCH> fetch{};
    u32 fetch_count = 0;
    const u32 first = pipeline.vertex_offset;
    for (u32 n = 0; n < feed_count; n++) {
        const Feed& f = feeds[n];
        if ((vp.input_regs & (1u << f.reg)) == 0) {
            continue;
        }
        const std::string name = fmt::format("vs_in_attr{}", f.reg);
        const auto attribute =
            std::find_if(vg.attributes.begin(), vg.attributes.end(),
                         [&](const Usse::GxpAttribute& a) { return a.name == name; });
        ASSERT_MSG(attribute != vg.attributes.end(), "GXM: {}: the program reads {} but has no "
                   "such attribute", describe(), name);
        ASSERT(fetch_count < fetch.size());
        // The identity index list counts from 0: the first vertex is in the address.
        const PAddr address = f.address + first * f.stride;
        fetch[fetch_count++] = {address, f.stride, f.words, attribute->resource};
        // FCRAM is cached for the CPU: what the guest wrote is cleaned to memory before the
        // kick. VRAM is uncached CDRAM.
        const u32 bytes = (count - 1) * f.stride + f.words * 4;
        ASSERT_MSG((address >= Memory::FCRAM_PADDR && address + bytes <= Memory::FCRAM_PADDR_END) ||
                       (address >= Memory::VRAM_PADDR && address + bytes <= Memory::VRAM_PADDR_END),
                   "GXM: {}: vertices at {:#x}+{:#x} are outside the mapped guest memory",
                   describe(), address, bytes);
        if (address >= Memory::FCRAM_PADDR) {
            backend.ReadsCached(memory.GetPhysicalPointer(address), bytes);
        }
    }

    // --- the vertex program's SA block and uniform buffer ---
    // The image covers the containers, the prefetched uniforms after them and the SAs the
    // secondary program writes; the secondary PDS program DMAs all of it.
    const u32 vsa_words = vg.uniform_floats + Usse::DataHead(vg) + static_cast<u32>(vg.literals.size());
    const u32 vsec_sa = std::max(vg.secondary_reg_count, vsa_words);
    const SgxdBackend::Data vsa = backend.AllocData(vsec_sa * 4);
    float* const vsa_f = reinterpret_cast<float*>(vsa.cpu);
    std::memset(vsa.cpu, 0, vsec_sa * 4);
    SgxdBackend::Data vbuf{};
    if (vg.buffer_pointer) {
        vbuf = backend.AllocData(vg.buffer_floats * 4);
        std::memset(vbuf.cpu, 0, vg.buffer_floats * 4);
        const u32 pointer = vbuf.gpu + static_cast<u32>(vg.ldst_base_value);
        std::memcpy(vsa.cpu + vg.uniform_floats * 4, &pointer, 4);
    }
    for (size_t i = 0; i < vg.literals.size(); i++) {
        std::memcpy(vsa.cpu + (vg.uniform_floats + Usse::DataHead(vg) + i) * 4, &vg.literals[i],
                    4);
    }
    const auto& uniforms = pica.vs_setup.uniforms;
    for (const Usse::GxpUniform& u : vg.uniforms) {
        if (u.name == "uniforms_f") {
            // The whole bank, f24 as float: rows past the 96 real uniforms read as ones, as the
            // PICA reads them, and rows 0-3 again past 127 (the emitter's wrap mirror).
            ASSERT(vg.buffer_pointer && u.sa_offset + u.array_size * 4 <= vg.buffer_floats);
            float* const bank = reinterpret_cast<float*>(vbuf.cpu) + u.sa_offset;
            for (u32 n = 0; n < u.array_size; n++) {
                const u32 row = n < 128 ? n : n - 128;
                for (u32 c = 0; c < 4; c++) {
                    bank[n * 4 + c] = row < 96 ? uniforms.f[row][c].ToFloat32() : 1.0f;
                }
            }
            continue;
        }
        ASSERT(u.sa_offset + u.components * u.array_size <= vg.uniform_floats);
        float* const out = vsa_f + u.sa_offset;
        if (u.name == "flip_viewport") {
            out[0] = vs_data.flip_viewport ? 1.0f : 0.0f;
        } else if (u.name == "depth_scale") {
            out[0] = fs_data.depth_scale;
        } else if (u.name == "depth_offset") {
            out[0] = fs_data.depth_offset;
        } else if (u.name == "uniforms_i") {
            for (u32 n = 0; n < 4 && n < u.array_size; n++) {
                for (u32 c = 0; c < 4; c++) {
                    out[n * 4 + c] = static_cast<float>(uniforms.i[n][c]);
                }
            }
        } else if (u.name.starts_with("vs_in_reg")) {
            const u32 reg = static_cast<u32>(std::stoul(u.name.substr(9)));
            float value[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            for (u32 attr = 0; attr < 16; attr++) {
                if (va.IsDefaultAttribute(attr) && regs.vs.GetRegisterForAttribute(attr) == reg) {
                    for (u32 c = 0; c < 4; c++) {
                        value[c] = pica.input_default_attributes[attr][c].ToFloat32();
                    }
                }
            }
            PutVec(out, value, u.components);
        } else {
            UNREACHABLE_MSG("GXM: {}: the vertex uniform {} has no value", describe(), u.name);
        }
    }
    const u32 vsec_sa = std::max(vg.secondary_reg_count, vsa_words);

    // --- the fragment program and its SA block ---
    const FragmentProgram& fp = FragmentProgramFor(blend_word);
    const Usse::GxpProgram& fg = fp.gxp;
    const u32 fsa_words = fg.uniform_floats + static_cast<u32>(fg.literals.size());
    SgxdBackend::Data fsa{};
    if (fsa_words != 0) {
        fsa = backend.AllocData(fsa_words * 4);
        std::memset(fsa.cpu, 0, fsa_words * 4);
        float* const fsa_f = reinterpret_cast<float*>(fsa.cpu);
        for (const Usse::GxpUniform& u : fg.uniforms) {
            ASSERT(u.sa_offset + u.components * u.array_size <= fg.uniform_floats);
            FragmentUniformValues(fs_data, u, fsa_f + u.sa_offset);
        }
        for (size_t i = 0; i < fg.literals.size(); i++) {
            std::memcpy(fsa.cpu + Usse::LiteralSaOffset(fg.uniform_floats, static_cast<u32>(i)) * 4,
                        &fg.literals[i], 4);
        }
    }

    // --- the PDS programs ---
    SgxdPdsProgram pds{};
    ASSERT_MSG(sgxd_program_vertex_pds(fetch.data(), fetch_count, vp.primary_doutu0,
                                       vg.per_instance ? 1 : 0,
                                       &pds) == 0,
               "GXM: {}: {} attributes the vertex PDS does not take", describe(), fetch_count);
    SgxdRecipeDrawPrograms p{};
    p.vspds = backend.PlacePds(pds);
    p.vspds_units = pds.units;
    ASSERT(sgxd_program_secondary_pds(vsa.gpu, vsa_words, vp.secondary_doutu0, &pds) == 0);
    p.vsec = backend.PlacePds(pds);
    p.vsec_units = pds.units;
    p.vsec_sa = vsec_sa;
    ASSERT(sgxd_program_secondary_pds(fsa.gpu, fsa_words, fp.secondary_doutu0, &pds) == 0);
    p.fsec = backend.PlacePds(pds);
    p.fsec_units = pds.units;
    p.fprim = fp.fprim;
    p.fprim_units = fp.fprim_units;
    sgxd_program_vertex_dms(vg.pa_count, vg.temp_count, vsec_sa, vg.vertex_outputs1 >> 24,
                            &p.vdm_w2, &p.vdm_w4, &p.vdm_w5);
    p.pixel_size = sgxd_program_pixel_dms(fp.pixel_pa, fg.temp_count, fsa_words);
    p.outselects = vg.vertex_outputs1;
    p.texsize = vg.vertex_outputs2;
    // Pixel row y is memory row y: the PICA's viewport as it stands, and z as the program
    // wrote it (depth times w).
    const float half_w = f24::FromRaw(regs.rasterizer.viewport_size_x).ToFloat32();
    const float half_h = f24::FromRaw(regs.rasterizer.viewport_size_y).ToFloat32();
    p.viewport[0] = static_cast<float>(regs.rasterizer.viewport_corner.x) + half_w;
    p.viewport[1] = half_w;
    p.viewport[2] = static_cast<float>(regs.rasterizer.viewport_corner.y) + half_h;
    p.viewport[3] = half_h;
    p.viewport[4] = 0.0f;
    p.viewport[5] = 1.0f;
    p.indices = 0;
    p.primitive = primitive;

    const u32 isp = (blend ? 1u : 0u) << 25 | dcmp << 22 | (dwrite ? 0u : 1u << 20) | 1u << 9;
    const SgxdRecipeDraw draw{0, count, isp, SGXD_R_FPRIM,
                              CullMode(regs.rasterizer.cull_mode), 0, nullptr};
    backend.Draw(draw, p);
#ifdef VITA_DIAGNOSTICS
    if (backend.PassDraws() <= 3 && GxmFlag("perflog")) {
        std::string units;
        const auto textures = regs.texturing.GetTextures();
        for (u32 k = 0; k < textures.size(); k++) {
            if (textures[k].enabled) {
                units += fmt::format(" t{} {:#x} {}x{} f{}", k,
                                     textures[k].config.GetPhysicalAddress(),
                                     textures[k].config.width.Value(),
                                     textures[k].config.height.Value(),
                                     static_cast<u32>(textures[k].format));
            }
        }
        backend.NotePass(fmt::format(" [draw {}: {}, ISP {:#x}, blend {}, viewport {} {} {} {};{}]",
                                     backend.PassDraws(), describe(), isp, blend, p.viewport[0],
                                     p.viewport[1], p.viewport[2], p.viewport[3], units));
    }
#endif
    if (draws++ < 4) {
        LOG_INFO(Render,
                 "draw {}: {}, {} attributes, ISP {:#010x}, cull {}, viewport {} {} {} {}; "
                 "VSPDS {:#010x} VSEC {:#010x} ({} SAs) FSEC {:#010x} FPRIM {:#010x}",
                 draws, describe(), fetch_count, isp, CullMode(regs.rasterizer.cull_mode),
                 p.viewport[0], p.viewport[1], p.viewport[2], p.viewport[3], p.vspds, p.vsec,
                 vsec_sa, p.fsec, p.fprim);
        LOG_INFO(Render, "draw {}: depth test {} ({}), write {}, blend {}; vertex SAs [{}]",
                 draws, om.depth_test_enable != 0, dcmp, dwrite, blend,
                 fmt::join(std::span<const float>(vsa_f, std::min(vsa_words, 16u)), " "));
    }
    return true;
}

} // namespace GxmRenderer
