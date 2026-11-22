// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <chrono>
#include <utility>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <fmt/ranges.h>
#include <nihstro/shader_bytecode.h>
#include "common/alignment.h"
#include "common/assert.h"
#include "common/hash.h"
#include "common/logging/log.h"
#include "common/math_util.h"
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

#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>
#include "azahar_native.h"
#include "sgxd.h"
#include "sgxd_program.h"
#include "sgxd_ptla.h"
#include "sgxd_session.h"

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
// The texture format code of F32F32, read by the float4 query.
constexpr unsigned LutFormatF32F32 = 0x1e;
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

void PutVec(float* out, const float* values, u32 count) {
    std::memcpy(out, values, count * sizeof(float));
}

/// Where the fragment uniform `name` (fs_usse_gen's PlanUniforms names) is in FSUniformData:
/// its byte offset and whether it holds ints, which the program reads as floats.
struct UniformField {
    std::size_t offset;
    bool is_int;
};

UniformField FragmentUniformField(const std::string& n) {
    using D = Pica::Shader::Generator::FSUniformData;
    using L = Pica::Shader::Generator::LightSrc;
#define FIELD(name, is_int)                                                                        \
    if (n == #name) {                                                                              \
        return {offsetof(D, name), is_int};                                                        \
    }
    FIELD(alphatest_ref, true)
    FIELD(const_color, false)
    FIELD(tev_combiner_buffer_color, false)
    FIELD(scissor_x1, true)
    FIELD(scissor_y1, true)
    FIELD(scissor_x2, true)
    FIELD(scissor_y2, true)
    FIELD(fog_lut_offset, true)
    FIELD(fog_color, false)
    FIELD(tex_border_color, false)
    FIELD(proctex_lut_offset, true)
    FIELD(proctex_diff_lut_offset, true)
    FIELD(proctex_color_map_offset, true)
    FIELD(proctex_alpha_map_offset, true)
    FIELD(proctex_bias, false)
    FIELD(lighting_lut_offset, true)
    FIELD(lighting_global_ambient, false)
#undef FIELD
    unsigned light = 0;
    char member[32] = "";
    if (std::sscanf(n.c_str(), "light_src[%u].%31s", &light, member) == 2 && light < 8) {
        const std::size_t base = offsetof(D, light_src) + light * sizeof(L);
        const std::string_view m = member;
#define MEMBER(name)                                                                               \
    if (m == #name) {                                                                              \
        return {base + offsetof(L, name), false};                                                  \
    }
        MEMBER(specular_0)
        MEMBER(specular_1)
        MEMBER(diffuse)
        MEMBER(ambient)
        MEMBER(position)
        MEMBER(spot_direction)
        MEMBER(dist_atten_bias)
        MEMBER(dist_atten_scale)
#undef MEMBER
    }
    UNREACHABLE_MSG("GXM: the fragment uniform {} has no value", n);
}

} // Anonymous namespace

RasterizerSgxd::RasterizerSgxd(Pica::PicaCore& pica_, Memory::MemorySystem& memory_,
                             GuestGpuMemory& guest_memory_)
    : VideoCore::RasterizerAccelerated{memory_, pica_}, guest_memory{guest_memory_} {
    Usse::Ir::SetTier2Skip(GxmFlagValue("tier2skip"));
#ifdef VITA_DIAGNOSTICS
    // sgxd_flags eotmixpbe=N: EUR_CR_PIXELBE for every 3D kick built (the eotmix probes).
    if (const unsigned pixelbe = SgxdFlagValue("eotmixpbe"); pixelbe != 0) {
        sgxd_recipe_pixelbe = pixelbe;
    }
    // gxm_flags eotmixreal=V+1: the 2x passes' end of tile stores the guest's 1x copy too.
    if (const unsigned real = GxmFlagValue("eotmixreal"); real != 0) {
        backend.eot_mix_real = static_cast<int>(real) - 1;
    }
#endif
    if (!SgxdFlag("notier2")) {
        tier2 = std::make_unique<ShaderTier2>();
    }
}

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
    FlushImmediate();
#ifdef VITA_DIAGNOSTICS
    // sgxd_flags `texdump=N`: every texture the draws take over a frame of both screens from N s.
    static const u64 tex_dump_at = u64{SgxdFlagValue("texdump")} * 1000000;
    if (tex_dump_at != 0 && tex_dump_transfers < 4 && sceKernelGetProcessTimeWide() >= tex_dump_at) {
        tex_dumping = ++tex_dump_transfers < 4;
        if (!tex_dumping) {
            LOG_INFO(Render, "texdump: {} textures written to host0", tex_dumped.size());
        }
    }
#endif
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
    DEBUG_ASSERT_MSG(
        !config.input_linear && !config.dont_swizzle && !config.flip_vertically &&
            !config.crop_input_lines && config.scaling == config.NoScale &&
            config.input_width == width && config.input_height == height,
        "GXM: AccelerateDisplayTransfer {}: only a tiled-to-linear copy of one size is "
        "built",
        describe());
    DEBUG_ASSERT_MSG(config.input_format == PixelFormat::RGBA8 &&
                         config.output_format == PixelFormat::RGB8,
                     "GXM: AccelerateDisplayTransfer {}: only RGBA8 to RGB8 is built", describe());
    const u32 bytes = width * height * 3;
    const bool fcram = output >= Memory::FCRAM_PADDR && output + bytes <= Memory::FCRAM_PADDR_END;
    DEBUG_ASSERT_MSG(
        fcram || (output >= Memory::VRAM_PADDR && output + bytes <= Memory::VRAM_PADDR_END),
        "sgxd: AccelerateDisplayTransfer {}: the output is outside the mapped guest memory",
        describe());
    if (backend.InPass()) {
        EndPass("the display transfer");
    }
    const auto source = std::find_if(surfaces.begin(), surfaces.end(),
                                     [&](const Surface& s) { return s.addr == input; });
    DEBUG_ASSERT_MSG(
        source != surfaces.end() && source->width == width && source->height == height,
        "GXM: AccelerateDisplayTransfer {}: no {}x{} surface at {:#x}; a picture the CPU "
        "drew needs GPU untiling, which is not built",
        describe(), width, height, input);

    // The surface's row y is the PICA's row y. The PICA's RGB8 is B, G, R in memory, gxm's
    // U8U8U8_RGB store. Guest memory is mapped for the GPU at its PICA address; VRAM, and
    // FCRAM moved to CDRAM (GuestGpuMemory), is uncached and needs no cache work.
    u8* const out = memory.GetPhysicalPointer(output);
    const bool cached = fcram && !guest_memory.IsUncached(output, bytes);
    if (fcram && !cached) {
        SgxdCacheSkipped(CacheSite::TransferBefore, bytes);
        SgxdCacheSkipped(CacheSite::TransferAfter, bytes);
    }
    const SgxdBackend::Store store{output, cached ? out : nullptr, cached ? bytes : 0,
                                   SGXD_RECIPE_PACK_U888_RGB};
    u32 ta_us = 0, d3_us = 0;
    backend.Blit(source->memory, width, height, store, &ta_us, &d3_us);
    TexturesWritten(output, output + bytes);
    // Progress: a line every 30 transfers.
    if (transfers > 0 && transfers % 30 == 0) {
        const SgxdEvents& ev = backend.Events();
        LOG_INFO(Render,
                 "transfer {}: TA {} us, 3D {} us; PTLA interrupts {} ({} completions, SLC in the "
                 "ISR {} us at most, {} us in all, {} timeouts, unexpected {:#x}); renders {}, "
                 "async {} (TA interrupts {}, 3D {}; the ISR's longest list run {} us)",
                 transfers + 1, ta_us, d3_us, ev.irqs, ev.twod, ev.slc_us_max, ev.slc_us_total,
                 ev.slc_timeouts, ev.unexpected, sgxd_session_renders(), ev.chains, ev.ta, ev.d3,
                 ev.chain_isr_us_max);
    }
    // gxm_flags dumpat=N: transfers N and N + 1 checked and written, to compare runs frame for
    // frame. Only then: the check waits for the render and the write holds the emulation thread.
    static const u32 dump_at = GxmFlagValue("dumpat");
    const bool dump = dump_at != 0 && transfers + 1 >= dump_at && transfers + 1 < dump_at + 2;
    transfers++;
    if (dump) {
        // The check reads the output back: the async render completes first.
        backend.RenderIdle("the transfer's check");
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
    FlushImmediate();
    const bool in_pass = backend.InPass();
    if (in_pass) {
        EndPass("P3D");
    }
    if (p3ds++ < 4 || p3ds % 60 == 0) {
        LOG_INFO(Render, "P3D {}: {}; {} passes so far", p3ds,
                 in_pass ? "ended the open pass" : "no pass open", passes);
    }
}

RasterizerGxm::GpuFence RasterizerGxm::GpuFenceAll() const {
    GpuFence fence{};
    fence.twod = true;
    fence.twod_count = backend.PtlaFence();
    fence.render = true;
    fence.render_count = backend.RenderFence();
    return fence;
}

// Passes sample only host memory in their 3D (surfaces, the LUT rows): after the TA the guest's
// memory is free. A pass that samples guest textures will need the whole render here.
RasterizerGxm::GpuFence RasterizerGxm::GpuFenceVertices() const {
    GpuFence fence{};
    fence.ta = true;
    fence.render_count = backend.RenderFence();
    return fence;
}

// gxm_flags earlyppf: PPF once the blit's TA has run, not its 3D, which then runs while the
// guest builds its next frame. The guest must not touch the transfer's output on the CPU before
// the 3D ends: the present, fills and DMAs over it wait for the 3D (SgxdBackend::RenderBefore),
// the guest's own CPU does not.
RasterizerGxm::GpuFence RasterizerGxm::GpuFenceTransfer() const {
    static const bool early = GxmFlag("earlyppf");
    if (!early) {
        return GpuFenceAll();
    }
    GpuFence fence{};
    fence.twod = true;
    fence.twod_count = backend.PtlaFence();
    fence.ta = backend.RenderAsync();
    fence.render_count = backend.RenderTaFence();
    return fence;
}

u64 RasterizerSgxd::GpuWaitUs() const {
    return sgxd_wait_us_total();
}
#endif

bool RasterizerGxm::AccelerateTextureCopy(const Pica::DisplayTransferConfig& config) {
    // Lines of input_width bytes input_gap apart into lines of output_width output_gap apart (16-byte
    // units); alike on both sides, or with no gaps, one contiguous copy.
    const auto& tc = config.texture_copy;
    const u32 in_width = tc.input_width * 16, in_gap = tc.input_gap * 16;
    const u32 out_width = tc.output_width * 16, out_gap = tc.output_gap * 16;
    const PAddr src = config.GetPhysicalInputAddress(), dst = config.GetPhysicalOutputAddress();
    if (in_gap == 0 && out_gap == 0) {
        return AccelerateDma(src, dst, tc.size);
    }
    DEBUG_ASSERT_MSG(in_width == out_width && in_width != 0 && tc.size % in_width == 0,
                     "sgxd: AccelerateTextureCopy from {:#x} to {:#x}, {} bytes, input width {} "
                     "gap {}, output width {} gap {}: lines of two widths are not built",
                     src, dst, tc.size, in_width, in_gap, out_width, out_gap);
    return CopyLines(src, in_width + in_gap, dst, out_width + out_gap, in_width,
                     tc.size / in_width);
}

bool RasterizerGxm::AccelerateFill(const Pica::MemoryFillConfig& config) {
    FlushImmediate();
    const PAddr start = config.GetStartAddress();
    const PAddr end = config.GetEndAddress();
    // The fill's pixel: 32, 24 or 16 bits, its value as the bytes the PICA writes, low first.
    const u32 bytes = config.fill_32bit ? 4 : (config.fill_24bit ? 3 : 2);
    const u32 value = config.fill_32bit  ? config.value_32bit
                      : config.fill_24bit ? (config.value_24bit_r | config.value_24bit_g << 8 |
                                             config.value_24bit_b << 16)
                                          : config.value_16bit.Value();
    const auto describe = [&] {
        return fmt::format("{:#x}-{:#x} ({}) with {:#x}, {}-bit", start, end,
                           config.IsVRAM() ? "VRAM" : "FCRAM", value, bytes * 8);
    };
#ifdef VITA_DIAGNOSTICS
    static const bool xfer_log = SgxdFlag("xferlog");
    if (xfer_log) {
        LOG_INFO(Render, "xferlog: fill {}", describe());
    }
#endif
    DEBUG_ASSERT_MSG(end > start && (end - start) % bytes == 0,
                     "sgxd: AccelerateFill {}: not a whole number of pixels", describe());
    // An FCRAM destination would need the CPU cache cleaned and invalidated around the PTLA: not built.
    DEBUG_ASSERT_MSG(start >= Memory::VRAM_PADDR && end <= Memory::VRAM_PADDR_END,
                     "sgxd: AccelerateFill {}: only guest VRAM is filled on the GPU", describe());

    // A fill over the open pass's buffers comes after its draws: the pass renders first.
    if (backend.InPass()) {
        const Surface& s = surfaces[pass_surface];
        const PAddr colour_end = s.addr + s.width * s.height * 4;
        const PAddr depth_end = pass_depth + s.width * s.height * pass_depth_bytes;
        if ((start < colour_end && s.addr < end) ||
            (pass_depth_cleared && start < depth_end && pass_depth < end)) {
            EndPass("a fill over its buffers");
        }
    }

    // The range as rows of 4096 pixels (under the PTLA's 64 KiB stride) and a last partial row, filled
    // in place at its PICA address, which is its GPU address.
    constexpr u32 RowPixels = 4096;
    const u32 pixels = (end - start) / bytes;
    const u32 format = bytes == 4 ? SGXD_PTLA_8888 : (bytes == 3 ? SGXD_PTLA_888 : SGXD_PTLA_U88);
    const SgxdPtlaFill fill{start,           format,             RowPixels * bytes, RowPixels,
                            pixels / RowPixels, pixels % RowPixels, value};
    char failed[160];
    u32 elapsed_us = 0;
    [[maybe_unused]] const int filled = sgxd_ptla_fill(&fill, &elapsed_us, failed, sizeof(failed));
    DEBUG_ASSERT_MSG(filled == 0, "sgxd: AccelerateFill {}: {}", describe(), failed);
    RecordClear({start, end, value, bytes, 0});
    TexturesWritten(start, end);

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

bool RasterizerSgxd::AccelerateDma(PAddr src, PAddr dst, u32 size) {
#ifdef VITA_DIAGNOSTICS
    static const bool xfer_log = SgxdFlag("xferlog");
    if (xfer_log) {
        LOG_INFO(Render, "xferlog: dma {:#x} to {:#x}, {} bytes", src, dst, size);
    }
#endif
    return CopyLines(src, size, dst, size, size, 1);
}

void RasterizerSgxd::MoveWaits() {
    if (backend.RetirePending()) {
        backend.RetireAll("a page move after a DMA's cache work");
    }
}

void RasterizerSgxd::KeepUncached(PAddr addr, u32 size) {
    if (guest_memory.WouldMove(addr, size)) {
        guest_memory.MakeUncached(addr, size, false);
    }
}

u32 RasterizerSgxd::RemapMoved(const u32* pte_va, const u32* pte, u32 count) {
    // A page move's or return's: the GPU onto the pages the CPU's views have now, after the work
    // queued before. Nothing else changes.
    backend.RemapPages(pte_va, pte, count);
    return backend.FenceAll();
}

bool RasterizerSgxd::CopyLines(PAddr src, u32 src_stride, PAddr dst, u32 dst_stride, u32 line,
                              u32 lines) {
    FlushImmediate();
    // The spans the lines lie in; one line (a DMA) is a plain copy.
    const bool strided = lines > 1 && (src_stride != line || dst_stride != line);
    const u32 size = strided ? src_stride * (lines - 1) + line : line * lines;
    const u32 dst_size = strided ? dst_stride * (lines - 1) + line : size;
    const auto describe = [&] {
        return strided ? fmt::format("{:#x} to {:#x}, {} lines of {} bytes, {} and {} apart", src,
                                     dst, lines, line, src_stride, dst_stride)
                       : fmt::format("{:#x} to {:#x}, {} bytes", src, dst, size);
    };
    const auto inside = [](PAddr addr, u32 bytes, PAddr base, u32 region) {
        return addr >= base && addr - base + u64{bytes} <= region;
    };
    const bool src_fcram = inside(src, size, Memory::FCRAM_PADDR, Memory::FCRAM_SIZE);
    const bool dst_fcram = inside(dst, dst_size, Memory::FCRAM_PADDR, Memory::FCRAM_SIZE);
    DEBUG_ASSERT_MSG(src % 4 == 0 && dst % 4 == 0 && size % 4 == 0 && size > 0,
                     "GXM: AccelerateDma {}: only whole words are copied on the GPU", describe());
    // A range past guest FCRAM or VRAM reaches whatever the GPU has mapped after it.
    ASSERT_MSG((src_fcram || inside(src, size, Memory::VRAM_PADDR, Memory::VRAM_SIZE)) &&
                   (dst_fcram || inside(dst, dst_size, Memory::VRAM_PADDR, Memory::VRAM_SIZE)),
               "sgxd: AccelerateDma {}: outside the mapped guest memory", describe());

    // A copy over the open pass's buffers comes after its draws: the pass renders first.
    const PAddr end = dst + dst_size;
    if (backend.InPass()) {
        const Surface& s = surfaces[pass_surface];
        const PAddr colour_end = s.addr + s.width * s.height * 4;
        const PAddr depth_end = pass_depth + s.width * s.height * pass_depth_bytes;
        if ((dst < colour_end && s.addr < end) ||
            (pass_depth_cleared && dst < depth_end && pass_depth < end)) {
            EndPass("a DMA over its buffers");
        }
    }
    // A source a pass drew is in its surface: guest memory takes it first (SyncRange), the
    // open pass ended first if it draws there.
    const bool drawn_source = std::any_of(surfaces.begin(), surfaces.end(), [&](const Surface& o) {
        return o.passes > 0 && !o.guest_newer && o.addr < src + size && src < o.End();
    });
    if (drawn_source) {
        if (backend.InPass() && surfaces[pass_surface].addr < src + size &&
            src < surfaces[pass_surface].End()) {
            EndPass("a DMA from its colour buffer");
        }
        sync_reason = "dma source";
        SyncRange(src, src + size);
    }
    ForgetClears(dst, end);
    TexturesWritten(dst, end);

    // Guest FCRAM is cached for the CPU (but the pages moved to CDRAM): the source is cleaned before
    // the PTLA reads it, the destination's lines leave the cache before and after.
    const bool src_cached = src_fcram && !guest_memory.IsUncached(src, size);
    const bool dst_cached = dst_fcram && !guest_memory.IsUncached(dst, dst_size);
    u8* const dst_cpu = memory.GetPhysicalPointer(dst);
    if (src_cached) {
        SgxdCacheRange(memory.GetPhysicalPointer(src), size, SGXD_CACHE_CLEAN,
                       CacheSite::DmaSource);
    } else if (src_fcram) {
        SgxdCacheSkipped(CacheSite::DmaSource, size);
    }
    if (dst_cached) {
        SgxdCacheRange(dst_cpu, dst_size, SGXD_CACHE_CLEAN_INVALIDATE, CacheSite::DmaDest);
    } else if (dst_fcram) {
        SgxdCacheSkipped(CacheSite::DmaDest, dst_size);
    }

    // As rows of 4096 words (under the PTLA's 64 KiB stride) and a last partial row; strided lines a
    // row each.
    constexpr u32 RowPixels = 4096;
    const u32 pixels = strided ? line / 4 * lines : size / 4;
    const SgxdPtlaCopy copy =
        strided ? SgxdPtlaCopy{src, dst, SGXD_PTLA_8888, src_stride, line / 4, lines, 0, dst_stride}
                : SgxdPtlaCopy{src, dst, SGXD_PTLA_8888, RowPixels * 4, RowPixels,
                               pixels / RowPixels, pixels % RowPixels, 0};
    char failed[160];
    u32 elapsed_us = 0;
    [[maybe_unused]] const int copied = sgxd_ptla_copy(&copy, &elapsed_us, failed, sizeof(failed));
    DEBUG_ASSERT_MSG(copied == 0, "sgxd: AccelerateDma {}: {}", describe(), failed);
    if (dst_cached) {
        // The CPU's lines over the destination go after the copy: an asynchronous one
        // completes here first.
        [[maybe_unused]] const int idle = sgxd_gpu_idle();
        DEBUG_ASSERT_MSG(idle == 0, "GXM: AccelerateDma {}: the PTLA never completed", describe());
        SgxdCacheRange(dst_cpu, dst_size, SGXD_CACHE_CLEAN_INVALIDATE, CacheSite::DmaDest);
    }
    if (dmas++ < 4) {
        LOG_INFO(Render, "DMA {}: {} ({} to {}) in {} us", dmas, describe(),
                 src_fcram ? "FCRAM" : "VRAM", dst_fcram ? "FCRAM" : "VRAM", elapsed_us);
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
            DEBUG_ASSERT_MSG(
                s.width == width && s.height == height,
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

namespace {

/// How the untile reads a PICA texture format: the texture unit's format code and swizzle
/// that give its channels as the PICA does (docs/sgxd/tsp.md), its bits a
/// texel, and for the 4-bit formats and IA4 the palette that expands the index (P4, P8).
/// Code 0xff: not built.
struct UntileFormat {
    u32 code;
    u32 swizzle;
    u32 bits;
    TexturePalette palette = TexturePalette::None;
};

UntileFormat UntileFormatFor(Pica::TexturingRegs::TextureFormat format) {
    using F = Pica::TexturingRegs::TextureFormat;
    switch (format) {
    case F::RGBA8: // bytes A, B, G, R: U8U8U8U8 mirrored (RGBA)
        return {0x0c, 2, 32};
    case F::RGB8: // bytes B, G, R: U8U8U8 with red and blue swapped
        return {0x98, 1, 24};
    case F::RGB5A1: // R 15:11, G 10:6, B 5:1, A 0: U1U5U5U5 mirrored
        return {0x04, 2, 16};
    case F::RGB565: // R 15:11, G 10:5, B 4:0: U5U6U5 with red and blue swapped
        return {0x05, 1, 16};
    case F::RGBA4: // R 15:12 .. A 3:0: U4U4U4U4 mirrored
        return {0x02, 2, 16};
    case F::IA8: // byte 0 alpha, byte 1 intensity: U8U8 as RGGG
        return {0x07, 3, 16};
    case F::I8: // U8 as 1RRR
        return {0x00, 5, 8};
    case F::A8: // U8 as R000
        return {0x00, 6, 8};
    // The 4-bit formats: a nibble a texel, the low one first (upstream's texture_decode.cpp);
    // IA4 a byte, intensity in the high nibble.
    case F::IA4:
        return {0x95, 0, 8, TexturePalette::IA4};
    case F::I4:
        return {0x94, 0, 4, TexturePalette::I4};
    case F::A4:
        return {0x94, 0, 4, TexturePalette::A4};
    case F::ETC1: // the texture unit's ETC1 once its blocks are reordered (Untile)
        return {SgxdBackend::UntileEtc1, 0, 4};
    case F::ETC1A4: // its colour as ETC1's, the alpha joined by a merge render (Untile)
        return {SgxdBackend::UntileEtc1A4, 0, 8};
    default:
        return {0xff, 0, 0};
    }
}

/// A PICA wrap mode as DOUTT0's address mode (0 REPEAT, 1 MIRROR, 2 CLAMP). The border modes clamp
/// and the fragment program puts the border colour past [0, 1].
u32 AddressMode(Pica::TexturingRegs::TextureConfig::WrapMode mode) {
    using W = Pica::TexturingRegs::TextureConfig::WrapMode;
    switch (mode) {
    case W::ClampToEdge:
    case W::ClampToBorder:
        return 2;
    case W::Repeat:
    case W::Repeat2:
    case W::Repeat3:
        return 0;
    case W::MirroredRepeat:
        return 1;
    default:
        UNREACHABLE_MSG("sgxd: texture wrap mode {} is not built", static_cast<u32>(mode));
    }
}

} // namespace

const RasterizerSgxd::Texture& RasterizerSgxd::TextureFor(u32 unit) {
    using TextureConfig = Pica::TexturingRegs::TextureConfig;
    const auto t = regs.texturing.GetTextures()[unit];
    const auto& c = t.config;
    const PAddr addr = c.GetPhysicalAddress();
    const u32 width = c.width, height = c.height;
    const UntileFormat f = UntileFormatFor(t.format);
    DEBUG_ASSERT_MSG(f.code != 0xff, "sgxd: texture unit {} at {:#x}, {}x{}: format {} is not built",
                     unit, addr, width, height, static_cast<u32>(t.format));
    const bool cube = unit == 0 && c.type == TextureConfig::TextureCube;
    // The levels upstream's cache builds (MipLevels): down to a side of 8, a whole 8x8 tile.
    u32 levels = 1;
    while (levels <= c.lod.max_level && width >> levels >= 8 && height >> levels >= 8) {
        levels++;
    }
    DEBUG_ASSERT_MSG(!cube || (levels == 1 && width == height),
                     "GXM: a {}x{} cube texture of {} levels is not built", width, height, levels);
    // The guest's parts: a 2D texture's levels, which follow each other, or a cube's six
    // faces, each at its own address. The host cube's slots are the SGX's +X, -X, +Y, -Y, +Z,
    // -Z with the PICA's +Y and -Y swapped: the fragment program reads at (u, -v, w), which
    // flips t on every face (FragmentEmitter::SampleTextures).
    std::array<PAddr, 6> part_addr{addr};
    const u32 parts = cube ? 6 : 1;
    if (cube) {
        using Face = Pica::TexturingRegs::CubeFace;
        static constexpr Face slots[6] = {Face::PositiveX, Face::NegativeX, Face::NegativeY,
                                          Face::PositiveY, Face::PositiveZ, Face::NegativeZ};
        for (u32 k = 0; k < 6; k++) {
            part_addr[k] = regs.texturing.GetCubePhysicalAddress(slots[k]);
        }
    }
    u32 bytes = 0;
    for (u32 l = 0; l < levels; l++) {
        bytes += (width >> l) * (height >> l) * f.bits / 8;
    }
    for (const Surface& s : surfaces) {
        for (u32 k = 0; k < parts; k++) {
            DEBUG_ASSERT_MSG(part_addr[k] >= s.addr + s.width * s.height * 4 ||
                                 s.addr >= part_addr[k] + bytes,
                             "GXM: texture unit {} at {:#x} reads the colour buffer at {:#x}: "
                             "rendering to a texture is not built",
                             unit, part_addr[k], s.addr);
        }
    }
    auto it = std::find_if(textures.begin(), textures.end(),
                           [addr](const Texture& e) { return e.addr == addr; });
    if (it != textures.end() &&
        (it->width != width || it->height != height || it->format != t.format ||
         it->levels != levels || it->parts != parts || it->part_addr != part_addr)) {
        backend.FreeTexture(it->memory, it->bytes);
        textures.erase(it);
        it = textures.end();
    }
    // A texture in FCRAM is untiled again in each pass that samples it: the guest's CPU writes
    // there leave no mark, and nothing compares the bytes. Its pages are in CDRAM by now
    // (RenderThread::KeepTargetUncached), so the GPU reads what the guest wrote with no clean.
    // Guest VRAM changes only through the GPU operations that mark its textures dirty.
    const bool fcram = addr >= Memory::FCRAM_PADDR;
    if (it != textures.end()) {
        if (!it->dirty && (!fcram || it->untiled_pass == passes)) {
            return *it;
        }
    } else {
        // The PBE writes whole 32-pixel tiles: a level's untile writes up to 32 rows of at
        // least 32 texels (SgxdBackend::Untile), past the chain's end for the last levels; a
        // cube face max(w, 32)^2 texels.
        u32 host_bytes = 0;
        if (cube) {
            host_bytes = 5 * width * width * 4 + std::max(width, 32u) * std::max(width, 32u) * 4;
        }
        for (u32 l = 0, offset = 0; !cube && l < levels; l++) {
            const u32 w = width >> l, h = height >> l;
            const u32 rows = w < 32 ? h * w / 32 : h;
            host_bytes = std::max(host_bytes, offset + std::max(w, 32u) * std::max(rows, 32u) * 4);
            offset += w * h * 4;
        }
        textures.push_back({addr, width, height, t.format, backend.AllocTexture(host_bytes),
                            host_bytes, levels, parts, part_addr, bytes, passes, false});
        it = std::prev(textures.end());
    }
    it->untiled_pass = passes;
    it->dirty = false;
    it->byte_swizzle = 0;
    // Each FCRAM part moves to CDRAM here, under the draw's read hold. A texture past VRAM's end reads
    // on into the filler past it (GuestGpuMemory::PastVramBytes); the tiny decode on the CPU has none.
    const bool tiny_cut = (width < 8 || height < 8) && ReadableBytes(part_addr[0], bytes) < bytes;
    ASSERT_MSG(!tiny_cut,
               "sgxd: texture unit {} at {:#x}, {}x{} format {}: a side under 8 past VRAM's end",
               unit, addr, width, height, static_cast<u32>(t.format));
    for (u32 k = 0; k < parts; k++) {
        const u32 part_bytes = ReadableBytes(part_addr[k], bytes);
        if (part_addr[k] >= Memory::FCRAM_PADDR) {
            KeepUncached(part_addr[k], part_bytes);
        }
#ifdef VITA_DIAGNOSTICS
        ASSERT_MSG(guest_memory.IsUncached(part_addr[k], part_bytes),
                   "sgxd: texture unit {} at {:#x}, {} bytes ({}x{}, format {}, {} levels): still "
                   "in cached FCRAM after its move; {}",
                   unit, part_addr[k], part_bytes, width, height, static_cast<u32>(t.format),
                   levels, guest_memory.DescribePages(part_addr[k], part_bytes));
#else
        ASSERT_MSG(guest_memory.IsUncached(part_addr[k], part_bytes),
                   "sgxd: texture unit {} at {:#x}, {} bytes: still in cached FCRAM after its "
                   "move",
                   unit, part_addr[k], part_bytes);
#endif
        if (part_addr[k] >= Memory::FCRAM_PADDR) {
            SgxdCacheSkipped(CacheSite::Textures, part_bytes);
        }
    }
    const u32 palette = PaletteFor(f.palette);
    if (cube) {
        // Each face a SWIZZLED square, back to back in slot order; a face's whole tiles run
        // over the ones after it, written next.
        for (u32 k = 0; k < 6; k++) {
            backend.Untile({part_addr[k], width, height, f.bits, f.code, f.swizzle, palette},
                           it->memory.gpu + k * width * height * 4, width, true);
        }
    }
    // Largest first: a level's whole tiles run over the ones after it, written next.
    for (u32 l = 0, guest = 0, host = 0; !cube && l < levels; l++) {
        const u32 w = width >> l, h = height >> l;
        backend.Untile({addr + guest, w, h, f.bits, f.code, f.swizzle, palette},
                       it->memory.gpu + host, w);
        guest += w * h * f.bits / 8;
        host += w * h * 4;
    }
#ifdef VITA_DIAGNOSTICS
    texture_stats.untiles++;
    texture_stats.untiled_bytes += u64{bytes} * parts;
    // untilecheck: every level against upstream's decoder, for the formats whose host copy is
    // R, G, B, A bytes; either row order (the rows it matches, the first it does not). The
    // levels follow each other in both copies: the guest's packed, the host's rows of their
    // width (a LINEAR chain, down to a side of 8).
    using TF = Pica::TexturingRegs::TextureFormat;
    // Upstream's level 0 (RGBA bytes) when the check failed, for dumptex's "want" file.
    std::vector<u8> want_image;
    if (!cube && GxmFlag("untilecheck") &&
        (t.format == TF::RGBA8 || t.format == TF::RGB8 || t.format == TF::ETC1 ||
         t.format == TF::ETC1A4)) {
        const bool alpha = t.format == TF::RGBA8 || t.format == TF::ETC1A4;
        backend.RenderIdle("untilecheck");
        static u32 untile_checks, untile_bad;
        untile_checks++;
        bool level0_bad = false;
        for (u32 l = 0, guest_at = 0, host_at = 0; l < levels; l++) {
            const u32 w = width >> l, h = height >> l;
            Pica::Texture::TextureInfo info{addr + guest_at, w, h, 0, t.format, false};
            info.SetDefaultStride();
            const u8* const guest = memory.GetPhysicalPointer(addr + guest_at);
            const u8* const host = it->memory.cpu + host_at;
            guest_at += w * h * f.bits / 8;
            host_at += w * h * 4;
            u32 wrong[2] = {}, first_bad[2] = {h, h};
            // The first differing texel of each order: x, y, then want and got as RGBA words.
            std::array<u32, 4> first_texel[2]{};
            if (l == 0) {
                want_image.resize(w * h * 4);
            }
            for (u32 y = 0; y < h; y++) {
                for (u32 x = 0; x < w; x++) {
                    const auto want = Pica::Texture::LookupTexture(guest, x, y, info);
                    if (l == 0) {
                        for (u32 c = 0; c < 4; c++) {
                            want_image[(y * w + x) * 4 + c] = want[c];
                        }
                    }
                    for (u32 o = 0; o < 2; o++) {
                        const u8* got = host + ((o ? h - 1 - y : y) * w + x) * 4;
                        bool bad = false;
                        for (u32 c = 0; c < (alpha ? 4u : 3u); c++) {
                            bad = bad || std::abs(static_cast<int>(got[c]) - want[c]) > 3;
                        }
                        if (bad) {
                            if (wrong[o]++ == 0) {
                                first_texel[o] = {
                                    x, y,
                                    u32{want[0]} | u32{want[1]} << 8 | u32{want[2]} << 16 |
                                        u32{want[3]} << 24,
                                    u32{got[0]} | u32{got[1]} << 8 | u32{got[2]} << 16 |
                                        u32{got[3]} << 24};
                            }
                            first_bad[o] = std::min(first_bad[o], o ? h - 1 - y : y);
                        }
                    }
                }
            }
            const u32 o = wrong[1] < wrong[0] ? 1 : 0;
            if (wrong[o] == 0) {
                continue;
            }
            level0_bad = level0_bad || l == 0;
            if (untile_bad++ < 64) {
                LOG_ERROR(Render,
                          "untilecheck {}: texture {:#x} {}x{} format {}, level {} of {} ({}x{}) "
                          "at GPU {:#x}: {} of {} texels differ ({} row order), the first in "
                          "host row {}; texel ({}, {}) want {:#010x} got {:#010x} (RGBA from "
                          "the low byte)",
                          untiles, addr, width, height, static_cast<u32>(t.format), l, levels, w,
                          h, it->memory.gpu + (host_at - w * h * 4), wrong[o], w * h,
                          o ? "flipped" : "same", first_bad[o], first_texel[o][0],
                          first_texel[o][1], first_texel[o][2], first_texel[o][3]);
            }
        }
        if (!level0_bad) {
            want_image.clear();
        }
        if (untile_checks % 64 == 0) {
            LOG_INFO(Render, "untilecheck: {} checked, {} levels differ", untile_checks,
                     untile_bad);
        }
    }
#endif
    // Each format's first 8 untiles (dumptex), or 48 from untile N on (dumptexfrom=N); the
    // log's first 8 of any.
    static std::array<u32, 16> format_untiles{};
    static const u32 dump_from = GxmFlagValue("dumptexfrom");
    static u32 dumped_from;
    const u32 format_untile = format_untiles[static_cast<u32>(t.format) & 15]++;
    const bool dump_this = (format_untile < 8 && GxmFlag("dumptex")) ||
                           (dump_from != 0 && untiles >= dump_from && dumped_from++ < 48);
    if (untiles++ < 8 || dump_this) {
        LOG_INFO(Render, "texture {:#x}: {}x{} format {}, {} levels{}, untiled to GPU {:#010x}",
                 addr, width, height, static_cast<u32>(t.format), levels, cube ? ", cube" : "",
                 it->memory.gpu);
#ifdef VITA_DIAGNOSTICS
        // dumptex: each level's (a cube: each face's) guest bytes and host copy to host0,
        // named by its guest address. A 2D level's host copy has rows of its width; a cube
        // face is SWIZZLED, written as -s0.
        if (dump_this) {
            const auto write = [&](PAddr at, u32 w, u32 h, u32 stride, const char* what,
                                   const void* data, u32 size) {
                const std::string path =
                    fmt::format("host0:azahar-tex{}-{:x}-{}x{}-f{}-s{}-{}.bin", untiles, at, w, h,
                                static_cast<u32>(t.format), stride, what);
                if (std::FILE* file = std::fopen(path.c_str(), "wb")) {
                    std::fwrite(data, 1, size, file);
                    std::fclose(file);
                }
            };
            for (u32 k = 0; cube && k < 6; k++) {
                const u32 size = width * height * 4;
                write(part_addr[k], width, height, 0, "guest",
                      memory.GetPhysicalPointer(part_addr[k]), bytes);
                write(part_addr[k], width, height, 0, "host", it->memory.cpu + k * size, size);
            }
            for (u32 l = 0, guest = 0, host = 0; !cube && l < levels; l++) {
                const u32 w = width >> l, h = height >> l;
                write(addr + guest, w, h, w, "guest", memory.GetPhysicalPointer(addr) + guest,
                      w * h * f.bits / 8);
                write(addr + guest, w, h, w, "host", it->memory.cpu + host, w * h * 4);
                if (l == 0 && !want_image.empty()) {
                    write(addr, w, h, w, "want", want_image.data(), w * h * 4);
                }
                guest += w * h * f.bits / 8;
                host += w * h * 4;
            }
        }
#endif
    }
    return *it;
}

u32 RasterizerSgxd::PaletteFor(TexturePalette palette) {
    if (palette == TexturePalette::None) {
        return 0;
    }
    if (!palettes.cpu) {
        // U8888 words at 64-byte multiples: A4 (16), I4 (16), IA4 (256).
        palettes = backend.AllocTexture(0x1000);
        u32* const p = reinterpret_cast<u32*>(palettes.cpu);
        for (u32 i = 0; i < 16; i++) {
            p[i] = i * 17 << 24;
            p[16 + i] = 0xff000000u | i * 0x111111u;
        }
        for (u32 b = 0; b < 256; b++) {
            p[32 + b] = (b & 15) * 17 << 24 | (b >> 4) * 0x111111u;
        }
    }
    static constexpr u32 offset[] = {0, 0, 64, 128};
    return palettes.gpu + offset[static_cast<u32>(palette)];
}

const RasterizerSgxd::Texture& RasterizerSgxd::NullTexture(u32 unit) {
    // Unit 0's type picks the sampler, a 2D or a cube; units 1 and 2 are 2D.
    using TextureConfig = Pica::TexturingRegs::TextureConfig;
    const TextureConfig::TextureType type = regs.texturing.texture0.type;
    const bool cube = unit == 0 && type == TextureConfig::TextureCube;
    DEBUG_ASSERT_MSG(unit != 0 || cube || type == TextureConfig::Texture2D ||
                         type == TextureConfig::Projection2D,
                     "sgxd: texture unit 0 is off, of type {}: no null texture for it is built",
                     static_cast<u32>(type));
#ifdef VITA_DIAGNOSTICS
    texture_stats.null_samples++;
#endif
    Texture& tex = null_textures[cube ? 1 : 0];
    if (tex.bytes == 0) {
        // Whole 32-texel tiles of zeros: a LINEAR 2D, or six SWIZZLED cube faces.
        constexpr u32 side = 32;
        tex.width = side;
        tex.height = side;
        tex.format = Pica::TexturingRegs::TextureFormat::RGBA8;
        tex.levels = 1;
        tex.parts = cube ? 6 : 1;
        tex.bytes = tex.parts * side * side * 4;
        u32 ready = 0;
        tex.memory = backend.AllocTexture(tex.bytes, &ready);
        backend.RenderWait(ready, "the null texture's range");
        std::memset(tex.memory.cpu, 0, tex.bytes);
    }
    return tex;
}

void RasterizerGxm::TextureWords(u32 unit, u32 words[4]) {
    using TextureConfig = Pica::TexturingRegs::TextureConfig;
    const auto t = regs.texturing.GetTextures()[unit];
    // A unit that is off still reaches the TEV: it reads zeros, point sampled and clamped.
    const TextureConfig off{};
    const Texture& tex = t.enabled ? TextureFor(unit) : NullTexture(unit);
    const TextureConfig& c = t.enabled ? t.config : off;
    const u32 mag = c.mag_filter == Pica::TexturingRegs::TextureConfig::Linear ? 1 : 0;
    const u32 min = c.min_filter == Pica::TexturingRegs::TextureConfig::Linear ? 1 : 0;
    const u32 mipf = c.mip_filter == Pica::TexturingRegs::TextureConfig::Linear ? 1 : 0;
    const u32 addressing = AddressMode(c.wrap_s) << 6 | AddressMode(c.wrap_t) << 3;
    // The chain's levels: MIPMAPCLAMP the last level (15 for one), LODMIN the PICA's min_level,
    // DADJUST its bias in eighths, zero at 31.
    const u32 clamp = tex.levels > 1 ? tex.levels - 1 : 15;
    const u32 lod_min = std::min<u32>(c.lod.min_level, tex.levels - 1);
    const s32 dadjust = 31 + ((static_cast<s32>(c.lod.bias) + 16) >> 5);
    DEBUG_ASSERT_MSG(dadjust >= 0 && dadjust <= 63,
                     "sgxd: texture unit {}: LOD bias {} is past DADJUST's range", unit,
                     static_cast<s32>(c.lod.bias) / 256.0f);
    // U8U8U8U8, ABGR (R the lowest byte), LINEAR or a CUBE of SWIZZLED faces (log2 sizes);
    // NORM.
    words[0] = static_cast<u32>(std::clamp(dadjust, 0, 63)) << 21 | clamp << 17 | mag << 12 |
               min << 10 | mipf << 9 | addressing;
    words[1] = tex.parts == 6 ? 2u << 29 | 0x0cu << 24 |
                                    static_cast<u32>(std::countr_zero(tex.width)) << 16 |
                                    static_cast<u32>(std::countr_zero(tex.height))
                              : 3u << 29 | 0x0cu << 24 | (tex.width - 1) << 12 | (tex.height - 1);
    words[2] = tex.memory.gpu | lod_min >> 2;
    words[3] = 1u << 31 | (lod_min & 3u) << 26;
}

void RasterizerSgxd::CachedRead(PAddr addr, u32 size) {
    std::scoped_lock lock{cached_reads_mutex};
    // A draw's attributes are usually one buffer: extend the last range where they touch.
    if (!cached_reads.empty()) {
        auto& [last, last_size] = cached_reads.back();
        if (addr <= last + last_size && last <= addr + size) {
            const PAddr end = std::max(last + last_size, addr + size);
            last = std::min(last, addr);
            last_size = end - last;
            return;
        }
    }
    cached_reads.emplace_back(addr, size);
}

std::vector<std::pair<PAddr, u32>> RasterizerSgxd::TakeCachedReads() {
    std::scoped_lock lock{cached_reads_mutex};
    return std::exchange(cached_reads, {});
}

void RasterizerSgxd::TexturesWritten(PAddr start, PAddr end) {
    for (Texture& t : textures) {
        for (u32 k = 0; k < t.parts; k++) {
            if (start < t.part_addr[k] + t.part_bytes && t.part_addr[k] < end) {
                t.dirty = true;
            }
        }
    }
}

void RasterizerSgxd::EndPass(const char* why) {
    Surface& s = surfaces[pass_surface];
    const u32 pass_draws = backend.PassDraws();
#ifdef VITA_DIAGNOSTICS
    const u32 pass_prims = backend.PassPrimitives();
#endif
#ifdef VITA_DIAGNOSTICS
    {
        auto& ends = texture_stats.pass_ends;
        const auto it =
            std::find_if(ends.begin(), ends.end(), [why](const auto& e) { return e.first == why; });
        if (it != ends.end()) {
            it->second++;
        } else {
            ends.emplace_back(why, 1u);
        }
    }
#endif
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
    LOG_INFO(Render, "perf: textures in {:.2f} s: {} untiled ({} KiB); {} cached",
             (now - t.start_us) / 1e6, t.untiles, t.untiled_bytes / 1024, textures.size());
    std::string ends;
    for (const auto& [why, n] : t.pass_ends) {
        ends += fmt::format("{}{} {}", ends.empty() ? "" : "; ", why, n);
    }
    LOG_INFO(Render, "perf: passes ended by: {}", ends);
    t.pass_ends.clear();
    static const bool bankstats = GxmFlag("bankstats");
    BankLutStats& b = bank_lut_stats;
    const auto per_build = [&](u64 rows) {
        return b.bank_builds ? static_cast<double>(rows) / b.bank_builds : 0.0;
    };
    if (bankstats) {
    LOG_INFO(Render,
             "perf: banks: {} draws, {} with a bank, {} built ({:.1f} rows each; changed since "
             "1 build {:.1f}, 8 builds {:.1f}, 32 builds {:.1f}), {} zero buffers",
             b.draws, b.bank_lookups, b.bank_builds, per_build(b.bank_rows),
             per_build(b.changed_since[0]), per_build(b.changed_since[1]),
             per_build(b.changed_since[2]), b.zero_builds);
    LOG_INFO(Render,
             "perf: LUT rows: {} lighting tables changed ({:.1f} of 256 entries each; contents "
             "converted within the last 64: {}, 256: {}), {} more rewritten for a ring move; "
             "{} fog",
             b.lut_tables,
             b.lut_tables ? static_cast<double>(b.lut_entries_changed) / b.lut_tables : 0.0,
             b.lut_seen_64, b.lut_seen_256, b.lut_ring_moves, b.fog_tables);
    }
    // The histories carry over: they describe the stream, not the second.
    b.draws = b.bank_lookups = b.bank_builds = b.zero_builds = 0;
    b.bank_rows = 0;
    b.changed_since = {};
    b.lut_tables = b.lut_ring_moves = b.lut_seen_64 = b.lut_seen_256 = b.fog_tables = 0;
    b.lut_entries_changed = 0;
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

/// With gxm_flags dumpgxp, bytes to host0:azahar-NAME.
void DumpBytes(const std::string& name, const void* data, std::size_t size) {
    const std::string path = fmt::format("host0:azahar-{}", name);
    if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(data, 1, size, f);
        std::fclose(f);
    }
}

/// With gxm_flags dumpgxp, the 3DS side of a fragment program: its PICA configuration's bytes
/// (azahar-fs-KEY.fsconfig) and upstream's GLSL for it (azahar-fs-KEY.glsl), for glsl2cg and
/// psp2cgc to compile the same program as Sony's compiler would.
void DumpPicaFragment(u64 key, const Pica::Shader::FSConfig& config) {
    if (!GxmFlag("dumpgxp")) {
        return;
    }
    DumpBytes(fmt::format("fs-{:016x}.fsconfig", key), &config, sizeof(config));
    const std::string glsl = Pica::Shader::Generator::GLSL::GenerateFragmentShader(
        config, Pica::Shader::UserConfig{}, Pica::Shader::Profile{});
    DumpBytes(fmt::format("fs-{:016x}.glsl", key), glsl.data(), glsl.size());
}

/// With gxm_flags dumpgxp, the 3DS side of a vertex program: its PicaVSConfig's bytes, then
/// the PICA program code and swizzle data (4096 words each) as azahar-vs-KEY.pica, and
/// upstream's GLSL for it (azahar-vs-KEY.glsl).
void DumpPicaVertex(u64 key, const Pica::Shader::Generator::PicaVSConfig& config,
                    const Pica::ShaderSetup& setup) {
    if (!GxmFlag("dumpgxp")) {
        return;
    }
    std::vector<u8> bytes(sizeof(config));
    std::memcpy(bytes.data(), &config, sizeof(config));
    const auto code = setup.GetProgramCode();
    const auto swizzle = setup.GetSwizzleData();
    const auto append = [&](const auto& words) {
        const auto* p = reinterpret_cast<const u8*>(words.data());
        bytes.insert(bytes.end(), p, p + words.size() * sizeof(words[0]));
    };
    append(code);
    append(swizzle);
    DumpBytes(fmt::format("vs-{:016x}.pica", key), bytes.data(), bytes.size());
    const std::string glsl = Pica::Shader::Generator::GLSL::GenerateVertexShader(
        setup, config, Pica::Shader::Generator::ExtraVSConfig{});
    DumpBytes(fmt::format("vs-{:016x}.glsl", key), glsl.data(), glsl.size());
}

} // namespace

const RasterizerSgxd::VertexProgram& RasterizerSgxd::VertexProgramFor(
    u16 default_regs, const std::array<u8, 16>& reg_components, u16 u8_regs, Usse::VsMode mode) {
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
        config.Hash(),
        static_cast<u64>(mode) << 32 | static_cast<u64>(u8_regs) << 16 | default_regs,
        static_cast<u64>(bools) << 16 | used->second,
        Common::ComputeHash64(reg_components.data(), reg_components.size()));
    const auto make_request = [&] {
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
        request.mode = mode;
        return request;
    };
    if (const auto it = vertex_programs.find(key); it != vertex_programs.end()) {
        VertexProgram& v = it->second;
        if (!v.tier2_queued && ++v.draws >= Tier2Draws) {
            v.tier2_queued = true;
            if (tier2) {
                tier2->QueueVertex(key, make_request());
            }
        }
        return v;
    }

    const Usse::VsRequest request = make_request();
    VertexProgram v{};
    std::string refusal;
    auto gxp = Usse::EmitVertexGxp(request, v.input_regs, &refusal);
    DEBUG_ASSERT_MSG(gxp.has_value(),
                     "sgxd: vertex program {:016x}: the emitter has no program for it: {}",
                     state.program_hash, refusal);
    v.gxp = std::move(*gxp);
    DumpGxp("vs", key, v.gxp);
    DumpPicaVertex(key, config, pica.vs_setup);
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

RasterizerGxm::VertexProgram RasterizerGxm::PlaceVertexProgram(Usse::GxpProgram gxp,
                                                                 const char* what, u64 key,
                                                                 u64 emit_us) {
    VertexProgram v{};
    v.gxp = std::move(gxp);
    DumpGxp("vs", key, v.gxp);
    v.primary_doutu0 = sgxd_program_doutu0(0, backend.PlaceCode(0, v.gxp.primary),
                                           v.gxp.temp_count);
    v.secondary_doutu0 = v.gxp.secondary.empty()
                             ? DummyVertexSecondary
                             : sgxd_program_doutu0(0, backend.PlaceCode(0, v.gxp.secondary), 0);
    LOG_INFO(Render, "{} {:016x}: {} words, {} PAs, {} temps, {} SAs{}", what, key,
             v.gxp.primary.size(), v.gxp.pa_count, v.gxp.temp_count, v.gxp.secondary_reg_count,
             v.gxp.per_instance ? ", per instance" : "");
    return v;
}

const RasterizerGxm::VertexProgram& RasterizerGxm::GeometryProgramFor(u32 rows, bool rows_uniform,
                                                                      bool rows_table,
                                                                      u32 first_uniform,
                                                                      u32 max_triangles,
                                                                      const Describe& describe) {
    Pica::ShaderSetup& setup = pica.gs_setup;
    setup.DoProgramCodeFixup();
    const u64 program_hash = setup.GetProgramCodeHash();
    auto used = used_bools.find(program_hash);
    if (used == used_bools.end()) {
        used = used_bools.emplace(program_hash, UsedBoolUniforms(setup)).first;
    }
    u16 bools = 0;
    for (u32 n = 0; n < 16; n++) {
        bools |= static_cast<u16>((setup.uniforms.b[n] ? 1u : 0u) << n);
    }
    bools &= used->second;
    const bool uniform_table = GsUniformTable(rows_uniform, rows_table);
    // b15 turns true after every invocation (upstream GeometryPipeline::SubmitVertex): baked
    // in, it holds for all of them only when the draw's first sees it true already.
    DEBUG_ASSERT_MSG((used->second & 0x8000) == 0 || (bools & 0x8000) != 0,
                     "GXM: {}: the geometry program reads b15 while it is false (the first "
                     "invocation's own value) is not built",
                     describe());
    const u32 output_mask = regs.gs.output_mask;
    const u64 key = Common::HashCombine(
        Common::HashCombine(program_hash, setup.GetSwizzleDataHash()),
        Common::HashCombine(static_cast<u64>(regs.gs.main_offset.Value()) << 16 | output_mask,
                            static_cast<u64>(bools) << 48 | static_cast<u64>(used->second) << 32 |
                                static_cast<u64>(max_triangles) << 16 |
                                (rows_table ? 0x8000u : 0u) | first_uniform << 8 |
                                (rows_uniform ? 0x80u : 0u) | rows));
    if (const auto it = vertex_programs.find(key); it != vertex_programs.end()) {
        return it->second;
    }
    Usse::VsRequest request{};
    request.code = setup.GetProgramCode();
    request.swizzle = setup.GetSwizzleData();
    request.code_size = std::min<u32>(setup.GetBiggestProgramSize(), Pica::MAX_PROGRAM_CODE_LENGTH);
    request.main_offset = regs.gs.main_offset;
    request.output_map.fill(16);
    for (u32 reg = 0; reg < 16; reg++) {
        if ((output_mask >> reg) & 1) {
            request.output_map[reg] = request.num_outputs++;
        }
    }
    request.bools = bools;
    request.bool_mask = used->second;
    request.mode = Usse::VsMode::Geometry;
    request.gs_rows = rows;
    request.gs_rows_uniform = rows_uniform;
    request.gs_rows_table = rows_table;
    request.gs_uniform_table = k.uniform_table;
    request.gs_first_uniform = first_uniform;
    request.gs_max_triangles = max_triangles;
    u32 input_regs = 0;
    std::string refusal;
    auto gxp = Usse::EmitVertexGxp(request, input_regs, &refusal);
    DEBUG_ASSERT_MSG(gxp.has_value(),
                     "sgxd: {}: geometry program {:016x}: the emitter has no program for it: {}",
                     describe(), program_hash, refusal);
    return vertex_programs.emplace(key, PlaceVertexProgram(std::move(*gxp), "geometry program", key))
        .first->second;
}

const RasterizerSgxd::VertexProgram& RasterizerSgxd::PassthroughProgramFor(u32 outputs) {
    // mov o<reg>, v<k> for the k-th register of the output mask, then end.
    const u32 output_mask = regs.gs.output_mask;
    const Pica::Shader::Generator::PicaFixedGSConfig gs_config{regs};
    const auto maps = gs_config.state.GetSemanticMaps();
    u64 key = Common::HashCombine(0x6770617373ull /* "gpass" */, output_mask);
    for (const auto& m : maps) {
        key = Common::HashCombine(key, static_cast<u64>(m.attribute_index) << 8 | m.component_index);
    }
    if (const auto it = vertex_programs.find(key); it != vertex_programs.end()) {
        return it->second;
    }
    std::vector<u32> code;
    Usse::VsRequest request{};
    request.output_map.fill(16);
    for (u32 reg = 0; reg < 16; reg++) {
        if ((output_mask >> reg) & 1) {
            // MOV (0x13): dest o<reg> at bit 21, src1 v<k> at bit 12, operand descriptor 0.
            code.push_back(0x13u << 26 | reg << 21 | request.num_outputs << 12);
            request.output_map[reg] = request.num_outputs++;
        }
    }
    code.push_back(0x22u << 26); // END
    // Descriptor 0: dest xyzw, src1 xyzw (0x36f).
    static const std::array<u32, 1> swizzle{0x36f};
    request.code = code;
    request.swizzle = swizzle;
    request.code_size = static_cast<u32>(code.size());
    request.main_offset = 0;
    for (u32 i = 0; i < maps.size(); i++) {
        request.semantics[i] = {static_cast<u8>(std::min<u32>(maps[i].attribute_index, 255)),
                                static_cast<u8>(maps[i].component_index & 3)};
    }
    for (u32 k = 0; k < request.num_outputs; k++) {
        request.reg_components[k] = 4;
    }
    request.default_regs = static_cast<u16>(~((1u << request.num_outputs) - 1));
    u32 input_regs = 0;
    std::string refusal;
    auto gxp = Usse::EmitVertexGxp(request, input_regs, &refusal);
    ASSERT_MSG(gxp.has_value(), "GXM: the geometry passthrough program: {}", refusal);
    ASSERT(request.num_outputs == outputs);
    VertexProgram v = PlaceVertexProgram(std::move(*gxp), "geometry passthrough", key);
    v.input_regs = input_regs;
    return vertex_programs.emplace(key, std::move(v)).first->second;
}

RasterizerSgxd::GsCodeFacts RasterizerSgxd::GsFacts() {
    Pica::ShaderSetup& setup = pica.gs_setup;
    setup.DoProgramCodeFixup();
    const u64 key = Common::HashCombine(setup.GetProgramCodeHash(), regs.gs.main_offset.Value());
    if (const GsCodeFacts* const found = gs_code_facts.Find(key)) {
        return *found;
    }
    const u32 size =
        std::min<u32>(setup.GetBiggestProgramSize(), Pica::MAX_PROGRAM_CODE_LENGTH);
    return gs_code_facts.Emplace(
        key, {Usse::GsTriangleBound(setup.GetProgramCode(), size, regs.gs.main_offset),
              Usse::GsIndexesUniforms(setup.GetProgramCode(), size)});
}

RasterizerSgxd::VertexStage RasterizerSgxd::GeometryStage(bool is_indexed,
                                                        const Describe& describe) {
    using GSMode = Pica::PipelineRegs::GSMode;
    const auto& pipeline = regs.pipeline;
    const u32 count = pipeline.num_vertices;
    const u32 vs_outputs = pipeline.vs_outmap_total_minus_1_a + 1;

    // --- the invocations: the vertices each one takes, and where its inputs land ---
    // Point mode: attribute a of the invocation's buffer is input register GetRegisterForAttribute(a).
    // The fixed-primitive mode: the vertices' outputs are float uniforms from start_index on.
    const u8* index_cpu = nullptr;
    const bool index_u16 = pipeline.index_array.format != 0;
    if (is_indexed) {
        // As the guest had them at the draw (RenderThread::PutGsIndices).
        DEBUG_ASSERT_MSG(gs_indices_set, "sgxd: {}: a geometry draw without its indices' copy",
                         describe());
        index_cpu = gs_indices.data();
    }
    const auto vertex_at = [&](u32 j) -> u32 {
        if (!is_indexed) {
            return pipeline.vertex_offset + j;
        }
        return index_u16 ? reinterpret_cast<const u16*>(index_cpu)[j] : index_cpu[j];
    };
    u32 per_invocation = 0;
    u32 rows = 0;
    bool rows_uniform = false;
    bool rows_table = false;
    // The variable mode: each primitive's count is in the index stream; its first main_vertices
    // vertices bring every output, the others their first.
    std::vector<u32> prims;
    const u32 main_vertices = pipeline.variable_vertex_main_num_minus_1 + 1;
    u32 first_uniform = 0;
    std::array<int, 64> row_attr{}; // row -> the invocation's attribute, or -1 for none
    row_attr.fill(-1);
    switch (pipeline.gs_config.mode.Value()) {
    case GSMode::Point: {
        const u32 inputs = regs.gs.max_input_attribute_index + 1;
        DEBUG_ASSERT_MSG(inputs % vs_outputs == 0 && regs.gs.input_to_uniform == 0,
                         "sgxd: {}: geometry point mode with {} inputs of {} outputs a vertex",
                         describe(), inputs, vs_outputs);
        per_invocation = inputs / vs_outputs;
        for (u32 a = 0; a < inputs; a++) {
            const u32 reg = regs.gs.GetRegisterForAttribute(a);
            row_attr[reg] = static_cast<int>(a);
            rows = std::max(rows, reg + 1);
        }
        break;
    }
    case GSMode::FixedPrimitive:
        DEBUG_ASSERT(vs_outputs == pipeline.gs_config.stride_minus_1 + 1u);
        per_invocation = pipeline.gs_config.fixed_vertex_num_minus_1 + 1;
        rows = vs_outputs * per_invocation;
        rows_uniform = true;
        first_uniform = pipeline.gs_config.start_index;
        for (u32 r = 0; r < rows; r++) {
            row_attr[r] = static_cast<int>(r);
        }
        break;
    case GSMode::VariablePrimitive:
        // c0 = the count, the main vertices' outputs from c1 on, then the other vertices' first outputs.
        // The rows differ in number between invocations, so they go through a table.
        DEBUG_ASSERT_MSG(is_indexed && regs.gs.input_to_uniform == 1,
                         "sgxd: {}: a variable-primitive geometry draw without indices", describe());
        rows_uniform = true;
        rows_table = true;
        for (u32 j = 0; j < count;) {
            const u32 n = vertex_at(j);
            DEBUG_ASSERT_MSG(n >= main_vertices && j + 1 + n <= count,
                             "sgxd: {}: a variable primitive of {} vertices at {} of {} ({} main): a "
                             "primitive carried to the next draw is not built",
                             describe(), n, j, count, main_vertices);
            prims.push_back(j);
            rows = std::max(rows, 1 + main_vertices * vs_outputs + (n - main_vertices));
            j += 1 + n;
        }
        DEBUG_ASSERT_MSG(rows < 128, "sgxd: {}: {} geometry input rows", describe(), rows);
        break;
    default:
        UNREACHABLE_MSG("sgxd: {}: geometry shader mode {} is not built", describe(),
                        static_cast<u32>(pipeline.gs_config.mode.Value()));
    }
    DEBUG_ASSERT_MSG(rows_table || (count % per_invocation == 0 && count != 0),
                     "sgxd: {}: {} vertices for invocations of {}: a partial invocation carried "
                     "to the next draw is not built",
                     describe(), count, per_invocation);
    const u32 invocations = rows_table ? static_cast<u32>(prims.size()) : count / per_invocation;

    // The room an invocation gets, and whether the CPU must read back what each stored. With the
    // program's own bound (Usse::GsTriangleBound) as the room, the unused triangles zeroed (three
    // equal vertices, which the TA drops) and pass B's list over every slot, nothing is read back: A0
    // and A1 join the pass's stream-out queue (SgxdBackend::StreamOut). Without a bound, or past the
    // pass data, they run to completion and the stored counts are read back.
    const u32 gs_outputs = static_cast<u32>(std::popcount(regs.gs.output_mask.Value()));
    const u32 tri_bytes = 3 * gs_outputs * 16;
    // Triangles an invocation may store: what a quarter of the pass data holds, to 1024.
    const u32 room = std::clamp<u32>((256u << 10) / (invocations * tri_bytes), 1u, 1024u);
    const u32 bound = GsFacts().bound;
    const bool padded = bound != 0 && bound <= room;
    const u32 cap = padded ? bound : room;
#ifdef VITA_DIAGNOSTICS
    // dumpgs reads what the first four draws stored: those wait.
    static const bool dump_gs = GxmFlag("dumpgs");
    const bool wait = !padded || (dump_gs && gs_draws < 4);
#else
    const bool wait = !padded;

    // --- pass A0: the vertex program, each vertex's outputs stored densely ---
    VertexStage a0 = VertexStageFor(is_indexed, nullptr, count, Usse::VsMode::StoreOutputs,
                                    describe);
    const u32 vertices = a0.vertex_max - a0.vertex_min + 1;
    const u32 vsize = vs_outputs * 16;
    if (rows_table) {
        // The vertices alone must reach A0's range's ends, or it runs the program on vertices the draw
        // has not got.
        u32 lo = ~0u, hi = 0;
        for (const u32 j : prims) {
            for (u32 k = 1; k <= vertex_at(j); k++) {
                lo = std::min(lo, vertex_at(j + k));
                hi = std::max(hi, vertex_at(j + k));
            }
        }
        DEBUG_ASSERT_MSG(lo == a0.vertex_min && hi == a0.vertex_max,
                         "sgxd: {}: the variable primitives' vertices are {}..{}, the index range "
                         "{}..{} (a count outside the vertices)",
                         describe(), lo, hi, a0.vertex_min, a0.vertex_max);
    }
    DEBUG_ASSERT_MSG(vertices <= 0x10000 &&
                         static_cast<u32>(std::popcount(regs.vs.output_mask.Value())) == vs_outputs,
                     "sgxd: {}: {} vertices, {} vertex outputs for {} geometry inputs a vertex",
                     describe(), vertices, std::popcount(regs.vs.output_mask.Value()), vs_outputs);
    // The stored vertex outputs, triangles and tables: the pass's data, or memory of their own
    // (gs_own_data), which the CPU writes at once after the work that last used it.
    const auto stored_data = [&](u32 bytes) {
        if (!gs_own_data) {
            return backend.AllocData(bytes);
        }
        // Given back after the open pass, whose stream-out and B draw read it.
        DEBUG_ASSERT(backend.InPass());
        u32 ready = 0;
        const SgxdBackend::Data d = backend.AllocTexture(bytes, &ready);
        backend.RenderWait(ready, "a geometry draw's own data");
        backend.FreeTextureAfterPass(d, bytes);
        return d;
    };
    const SgxdBackend::Data vs_out = stored_data(vertices * vsize);
    const u32 a0_count = Common::AlignUp(vertices, 3u);
    const SgxdBackend::Data a0_table = stored_data(vertices * 4);
    const SgxdBackend::Data a0_indices = backend.AllocData(a0_count * 2, 4);
    u32* const table = reinterpret_cast<u32*>(a0_table.cpu);
    u16* const a0_list = reinterpret_cast<u16*>(a0_indices.cpu);
    for (u32 j = 0; j < vertices; j++) {
        table[j] = vs_out.gpu + j * vsize;
        a0_list[j] = static_cast<u16>(j);
    }
    for (u32 j = vertices; j < a0_count; j++) {
        a0_list[j] = static_cast<u16>(vertices - 1); // the last again: the same stores
    }
    {
        const auto& ag = a0.vp->gxp;
        const auto out = std::find_if(ag.attributes.begin(), ag.attributes.end(),
                                      [](const auto& a) { return a.name == "vs_out_address"; });
        DEBUG_ASSERT(out != ag.attributes.end() && a0.fetch_count < a0.fetch.size());
        a0.fetch[a0.fetch_count++] = {a0_table.gpu, 4, 1, out->resource};
        a0.indices = a0_indices.gpu;
        a0.count = a0_count;
        u32 vsec_sa = 0;
        const SgxdBackend::Data vsa = VertexSa(ag, pica.vs_setup, vsec_sa, describe);
        SgxdRecipeDrawPrograms p{};
        VertexPrograms(a0, vsa, vsec_sa, p, describe);
#ifdef VITA_DIAGNOSTICS
        NoteStreamOut("A0", static_cast<u32>(pipeline.gs_config.mode.Value()), invocations, rows,
                      0, a0_count, *a0.vp);
#endif
        backend.StreamOut(p, a0_count, SgxdBackend::StreamStage::A0, wait);
    }

    // --- pass A1: the geometry program, one invocation a vertex ---
    // A row an earlier draw left in its own memory is gone with its pass.
    ASSERT_MSG((stale & gs_rows_own) == 0,
               "sgxd: {}: a geometry program reads input registers {:#x} an earlier draw left in "
               "memory of its own (past a pass's data): not built",
               describe(), stale & gs_rows_own);
    const SgxdBackend::Data gs_out = stored_data(invocations * cap * tri_bytes);
    if (padded) {
        std::memset(gs_out.cpu, 0, invocations * cap * tri_bytes);
    }
    const SgxdBackend::Data gs_ends = backend.AllocData(invocations * 4);
    const u32 words = rows_table ? 4 : 2 + rows;
    const SgxdBackend::Data gs_table = stored_data(invocations * words * 4);
    const SgxdBackend::Data zero_row = backend.AllocData(16);
    std::memset(zero_row.cpu, 0, 16);
    // The row tables (128 entries and 3 a fetch past the last may read) of the variable mode and of
    // a fixed-primitive program's indexed reads, and the variable mode's count rows.
    constexpr u32 TableWords = 128 + 3;
    const SgxdBackend::Data row_tables = rows_table || uniform_table
                                             ? stored_data(invocations * TableWords * 4)
                                             : SgxdBackend::Data{};
    const SgxdBackend::Data count_rows =
        rows_table ? backend.AllocData(invocations * 16) : SgxdBackend::Data{};
    u32* const rows_out = reinterpret_cast<u32*>(gs_table.cpu);
    for (u32 i = 0; i < invocations; i++) {
        u32* const t = rows_out + i * words;
        t[0] = gs_out.gpu + i * cap * tri_bytes;
        t[1] = gs_ends.gpu + i * 4;
        if (rows_table) {
            t[2] = row_tables.gpu + i * TableWords * 4 - 4;
            t[3] = 0;
            continue;
        }
        for (u32 r = 0; r < rows; r++) {
            const int a = row_attr[r];
            if (a < 0) {
                t[2 + r] = zero_row.gpu - 4;
                continue;
            }
            const u32 j = i * per_invocation + static_cast<u32>(a) / vs_outputs;
            const u32 v = vertex_at(j) - a0.vertex_min;
            t[2 + r] = vs_out.gpu + v * vsize + (static_cast<u32>(a) % vs_outputs) * 16 - 4;
        }
    }
    const VertexProgram& gp =
        GeometryProgramFor(rows, rows_uniform, rows_table, first_uniform, cap, describe);
    {
        const auto in = std::find_if(gp.gxp.attributes.begin(), gp.gxp.attributes.end(),
                                     [](const auto& a) { return a.name == "gs_in_rows"; });
        DEBUG_ASSERT(in != gp.gxp.attributes.end());
        VertexStage a1{};
        a1.vp = &gp;
        a1.fetch[0] = {gs_table.gpu, words * 4, words, in->resource};
        a1.fetch_count = 1;
        a1.count = Common::AlignUp(invocations, 3u);
        const SgxdBackend::Data a1_indices = backend.AllocData(a1.count * 2, 4);
        u16* const list = reinterpret_cast<u16*>(a1_indices.cpu);
        DEBUG_ASSERT(invocations <= 0x10000);
        for (u32 i = 0; i < a1.count; i++) {
            list[i] = static_cast<u16>(std::min(i, invocations - 1));
        }
        a1.indices = a1_indices.gpu;
        a1.setup = &pica.gs_setup;
        u32 vsec_sa = 0;
        const SgxdBackend::Data vsa = VertexSa(gp.gxp, pica.gs_setup, vsec_sa, describe);
        if (rows_table) {
            // Past its rows an entry points at the shared uniform, as the SA image's bank pointer reads it.
            DEBUG_ASSERT(gp.gxp.buffer_pointer);
            u32 pointer = 0;
            std::memcpy(&pointer, vsa.cpu + gp.gxp.uniform_floats * 4, 4);
            u32* const tables = reinterpret_cast<u32*>(row_tables.cpu);
            float* const counts = reinterpret_cast<float*>(count_rows.cpu);
            for (u32 i = 0; i < invocations; i++) {
                const u32 j = prims[i];
                const u32 n = vertex_at(j);
                const u32 own = 1 + main_vertices * vs_outputs + (n - main_vertices);
                u32* const t = tables + i * TableWords;
                std::fill_n(counts + i * 4, 4, static_cast<float>(n));
                t[0] = count_rows.gpu + i * 16 - 4;
                const u32 main_rows = main_vertices * vs_outputs;
                for (u32 r = 1; r < own; r++) {
                    const bool full = r - 1 < main_rows;
                    const u32 k = full ? (r - 1) / vs_outputs : main_vertices + (r - 1 - main_rows);
                    const u32 output = full ? (r - 1) % vs_outputs : 0;
                    const u32 v = vertex_at(j + 1 + k) - a0.vertex_min;
                    t[r] = vs_out.gpu + v * vsize + output * 16 - 4;
                }
                for (u32 r = own; r < TableWords; r++) {
                    t[r] = pointer + 16 * (r & 127);
                }
            }
        } else if (uniform_table) {
            // Around the rows the entry loop placed, the shared uniforms: the same in every table.
            DEBUG_ASSERT(gp.usse.buffer_pointer);
            u32 pointer = 0;
            std::memcpy(&pointer, vsa.cpu + gp.usse.uniform_floats * 4, 4);
            std::array<u32, TableWords> shared;
            for (u32 r = 0; r < TableWords; r++) {
                shared[r] = pointer + 16 * (r & 127);
            }
            const u32 past = first_uniform + rows;
            u32* const tables = reinterpret_cast<u32*>(row_tables.cpu);
            for (u32 i = first; i < first + n; i++) {
                u32* const t = tables + i * TableWords;
                std::memcpy(t, shared.data(), first_uniform * 4);
                std::memcpy(t + past, shared.data() + past, (TableWords - past) * 4);
            }
        }
        SgxdRecipeDrawPrograms p{};
        VertexPrograms(a1, vsa, vsec_sa, p, describe);
#ifdef VITA_DIAGNOSTICS
        NoteStreamOut("A1", static_cast<u32>(pipeline.gs_config.mode.Value()), invocations, rows,
                      cap, a1.count, gp);
#endif
        backend.StreamOut(p, a1.count, SgxdBackend::StreamStage::A1, wait);
#ifdef VITA_DIAGNOSTICS
        stream_note.stage = nullptr;
#endif
    }

    // --- pass B: the stored triangles, as the draw's vertices ---
    std::vector<u16> list;
    const u32* const ends = reinterpret_cast<const u32*>(gs_ends.cpu);
    if (padded) {
        // Every slot: the ones no EMIT filled are zeros.
        DEBUG_ASSERT(invocations * cap * 3 <= 0x10000);
        list.resize(invocations * cap * 3);
        for (u32 n = 0; n < list.size(); n++) {
            list[n] = static_cast<u16>(n);
        }
    }
    for (u32 i = 0; i < invocations && !padded; i++) {
        const u32 first = gs_out.gpu + i * cap * tri_bytes;
        const u32 stored = (ends[i] - first) / tri_bytes;
        DEBUG_ASSERT_MSG(ends[i] >= first && (ends[i] - first) % tri_bytes == 0 && stored < cap,
                         "sgxd: {}: geometry invocation {} of {} ended at {:#x} (first triangle "
                         "{:#x}, {} bytes each, room for {})",
                         describe(), i, invocations, ends[i], first, tri_bytes, cap);
        for (u32 t = 0; t < stored; t++) {
            const u32 base = (i * cap + t) * 3;
            DEBUG_ASSERT(base + 2 < 0x10000);
            list.push_back(static_cast<u16>(base));
            list.push_back(static_cast<u16>(base + 1));
            list.push_back(static_cast<u16>(base + 2));
        }
    }
#ifdef VITA_DIAGNOSTICS
    // dumpgs: the first four geometry draws' vertex stage outputs (vs_outputs vec4s a vertex
    // from a0's vertex_min), stored triangles (room for `cap` an invocation) and end addresses.
    if (gs_draws < 4 && GxmFlag("dumpgs")) {
        const auto write = [&](const char* what, const void* data, u32 size) {
            const std::string path =
                fmt::format("host0:azahar-gs{}-{}-v{}-o{}-c{}-{}.bin", gs_draws, invocations,
                            a0.vertex_min, gs_outputs, cap, what);
            if (std::FILE* file = std::fopen(path.c_str(), "wb")) {
                std::fwrite(data, 1, size, file);
                std::fclose(file);
            }
        };
        write("vs", vs_out.cpu, vertices * vsize);
        write("out", gs_out.cpu, invocations * cap * tri_bytes);
        std::vector<u32> firsts(invocations);
        for (u32 i = 0; i < invocations; i++) {
            firsts[i] = (ends[i] - (gs_out.gpu + i * cap * tri_bytes)) / tri_bytes;
        }
        write("stored", firsts.data(), invocations * 4);
    }
#endif
    if (gs_draws++ < 4) {
        LOG_INFO(Render, "geometry draw {}: {} invocations, {} triangles{}", gs_draws, invocations,
                 list.size() / 3, padded ? " (every slot, the room the program's bound)" : "");
    }
    VertexStage b{};
    if (list.empty()) {
        return b;
    }
    const VertexProgram& pp = PassthroughProgramFor(gs_outputs);
    for (u32 k = 0; k < gs_outputs; k++) {
        const std::string name = fmt::format("vs_in_attr{}", k);
        const auto attribute =
            std::find_if(pp.gxp.attributes.begin(), pp.gxp.attributes.end(),
                         [&](const Usse::GxpAttribute& a) { return a.name == name; });
        if (attribute == pp.gxp.attributes.end()) {
            continue; // an output no semantic reads
        }
        b.fetch[b.fetch_count++] = {gs_out.gpu + k * 16, gs_outputs * 16, 4, attribute->resource};
    }
    const SgxdBackend::Data b_indices = backend.AllocData(static_cast<u32>(list.size()) * 2, 4);
    std::memcpy(b_indices.cpu, list.data(), list.size() * 2);
    b.vp = &pp;
    b.indices = b_indices.gpu;
    b.count = static_cast<u32>(list.size());
    b.setup = &pica.vs_setup;
    return b;
}

const RasterizerGxm::FragmentProgram& RasterizerGxm::FragmentProgramFor(u64 blend) {
    const Pica::Shader::FSConfig config{regs};
    const u64 key = Common::HashCombine(config.Hash(), blend);
    if (const auto it = fragment_programs.find(key); it != fragment_programs.end()) {
        FragmentProgram& f = it->second;
        if (!f.tier2_queued && ++f.draws >= Tier2Draws) {
            f.tier2_queued = true;
            if (tier2) {
                tier2->QueueFragment(key, config);
            }
        }
        return f;
    }
    // The first tier: registers placed, no optimising passes; microseconds, and the draw
    // waits for it. The second tier replaces it once it has drawn Tier2Draws times.
    std::string refusal;
    auto gxp = Usse::EmitFragmentGxp(config, &refusal);
    DEBUG_ASSERT_MSG(gxp.has_value(), "GXM: the emitter has no fragment program for this state: {}",
                     refusal);
    DumpGxp("fs", key, *gxp);
    DumpPicaFragment(key, config);
    FragmentProgram f = PlaceFragmentProgram(std::move(*gxp), blend, config.Hash());
    return fragment_programs.emplace(key, std::move(f)).first->second;
}

void RasterizerSgxd::QueueFragmentTier2(u64 key, const Pica::Shader::FSConfig& config) {
    const u64 config_key = ShaderTier1::ConfigKey(config);
    Tier2Config* c = tier2_configs.Find(config_key);
    if (c && c->program) {
        InstallFragmentTier2(key, *c->program);
        return;
    }
    if (!c) {
        c = &tier2_configs.Emplace(config_key, Tier2Config{});
        tier2->QueueFragment(config_key, config);
    }
    c->waiting.push_back(key);
}

void RasterizerSgxd::InstallFragmentTier2(u64 key, const Usse::Program& program) {
    FragmentProgram* const old = fragment_programs.Find(key);
    // A program FlushPrograms dropped after its second tier was queued.
    ASSERT(old != nullptr || program_flushes != 0);
    if (!old) {
        return;
    }
    DumpProgram("fs2", key, program);
    const u32 temps_before = old->usse.temp_count;
    const std::size_t words_before = old->usse.primary.size();
    FragmentProgram f = PlaceFragmentProgram(Usse::Program{program}, old->epilogue, key);
    f.draws = old->draws;
    f.tier2_queued = true;
    LOG_INFO(Render, "tier 2 fragment program {:016x}: {} words to {}, {} temps to {}", key,
             words_before, f.usse.primary.size(), temps_before, f.usse.temp_count);
    // In place: last_fragment and every draw's reference see the new program. The first tier's code
    // goes back to the heap once the passes that may run it are done.
    FreeProgramCode(old->code);
    *old = std::move(f);
}

void RasterizerSgxd::InstallTier2() {
    for (ShaderTier2::Result& result : tier2->Take()) {
        if (result.vertex) {
            InstallVertexTier2(result);
            continue;
        }
        const auto it = fragment_programs.find(result.key);
        ASSERT_MSG(result.program.has_value(), "sgxd: tier 2 of fragment program {:016x}: {}",
                   result.key, result.refusal);
        ASSERT(it != fragment_programs.end());
        DumpGxp("fs2", result.key, *result.program);
        const u32 temps_before = it->second.gxp.temp_count;
        const std::size_t words_before = it->second.gxp.primary.size();
        FragmentProgram f =
            PlaceFragmentProgram(std::move(*result.program), it->second.blend, result.key);
        f.draws = it->second.draws;
        f.tier2_queued = true;
        LOG_INFO(Render,
                 "tier 2 fragment program {:016x}: {} words to {}, {} temps to {}, emitted in {} "
                 "us ({} cycles), program {:016x}",
                 result.key, words_before, f.gxp.primary.size(), temps_before, f.gxp.temp_count,
                 result.took_us, result.took_cycles, ProgramHash(f.usse));
        it->second = std::move(f);
    }
}

void RasterizerSgxd::InstallVertexTier2(ShaderTier2::Result& result) {
    const auto it = vertex_programs.find(result.key);
    ASSERT_MSG(result.program.has_value(), "sgxd: tier 2 of vertex program {:016x}: {}",
               result.key, result.refusal);
    ASSERT(it != vertex_programs.end());
    // The draw binds an attribute for each input register the program reads: both tiers must read
    // the same ones.
    ASSERT_MSG(result.input_regs == it->second.input_regs,
               "sgxd: tier 2 of vertex program {:016x} reads input registers {:#x}, tier 1 {:#x}",
               result.key, result.input_regs, it->second.input_regs);
    DumpGxp("vs2", result.key, *result.program);
    const u32 temps_before = it->second.gxp.temp_count;
    const std::size_t words_before = it->second.gxp.primary.size();
    VertexProgram v{};
    v.gxp = std::move(*result.program);
    v.input_regs = result.input_regs;
    v.primary_doutu0 = sgxd_program_doutu0(0, backend.PlaceCode(0, v.gxp.primary),
                                           v.gxp.temp_count);
    v.secondary_doutu0 = v.gxp.secondary.empty()
                             ? DummyVertexSecondary
                             : sgxd_program_doutu0(0, backend.PlaceCode(0, v.gxp.secondary), 0);
    v.draws = it->second.draws;
    v.tier2_queued = true;
    LOG_INFO(Render,
             "tier 2 vertex program {:016x}: {} words to {}, {} temps to {}, emitted in {} us ({} "
             "cycles), program {:016x}",
             result.key, words_before, v.gxp.primary.size(), temps_before, v.gxp.temp_count,
             result.took_us, result.took_cycles, ProgramHash(v.usse));
    it->second = std::move(v);
}

RasterizerGxm::FragmentProgram RasterizerGxm::PlaceFragmentProgram(Usse::GxpProgram gxp, u64 blend,
                                                                   u64 hash) {
    FragmentProgram f{};
    f.gxp = std::move(gxp);
    f.blend = blend;
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
    DEBUG_ASSERT_MSG(g.phase_starts.size() == 1,
                     "GXM: a fragment program of {} phases (kill {}, depth write {}): not built",
                     g.phase_starts.size(), g.uses_discard, g.writes_depth);
    // The texture units' samplers, the lighting and fog LUT's (SyncLutLf) and the proctex
    // LUTs' are built.
    for (const Usse::GxpSampler& sampler : g.samplers) {
        if (sampler.name != "texture_buffer_lut_lf" && sampler.name != "texture_buffer_lut_rg" &&
            sampler.name != "texture_buffer_lut_rgba" && sampler.name != "tex0" &&
            sampler.name != "tex1" && sampler.name != "tex2") {
            std::string names;
            for (const Usse::GxpSampler& s : g.samplers) {
                names += fmt::format(" {} (unit {})", s.name, s.unit);
            }
            UNREACHABLE_MSG("GXM: a fragment program with samplers:{}; only the textures and "
                            "the LUTs are built",
                            names);
        }
    }
    // SMP reads its four state words as two pairs.
    DEBUG_ASSERT(g.samplers.empty() ||
                 Usse::SamplerSaOffset(g.uniform_floats, static_cast<u32>(g.literals.size()), 0) %
                         2 ==
                     0);
    DEBUG_ASSERT_MSG(!g.buffer_pointer, "GXM: a fragment program with memory uniforms: not built");
    for (const Usse::GxpUniform& u : g.uniforms) {
        DEBUG_ASSERT(u.sa_offset + u.components * u.array_size <= g.uniform_floats);
        const UniformField field = FragmentUniformField(u.name);
        f.uniforms.push_back({u.sa_offset, static_cast<u16>(field.offset),
                              static_cast<u8>(u.components), static_cast<u8>(u.array_size), 16,
                              field.is_int});
    }

    std::vector<u64> code = g.primary;
    if (blend) {
        code.push_back(PackInPlace);
        code.push_back(blend);
    } else {
        code.push_back(PackToOutput);
    }
    const u32 doutu0 = sgxd_program_doutu0(1, backend.PlaceCode(1, code), g.temp_count);
    f.secondary_doutu0 = g.secondary.empty()
                             ? DummyPixelSecondary
                             : sgxd_program_doutu0(1, backend.PlaceCode(1, g.secondary), 0);
    f.primary_doutu0 = doutu0;

    std::array<u32, 12> iterators{};
    std::array<unsigned, 12> words{};
    DEBUG_ASSERT(g.inputs.size() <= iterators.size());
    u32 iterated = 0;
    for (size_t i = 0; i < g.inputs.size(); i++) {
        iterators[i] = Usse::VaryingDescriptor(g, i);
        words[i] = g.inputs[i].components;
        iterated += g.inputs[i].components;
    }
    SgxdPdsProgram pds{};
    [[maybe_unused]] const int fprim = sgxd_program_pixel_pds(
        doutu0, iterators.data(), words.data(), static_cast<unsigned>(g.inputs.size()), &pds);
    DEBUG_ASSERT(fprim == 0);
    f.fprim = backend.PlacePds(pds);
    f.fprim_units = pds.units;
    // The pack reads pa0 and pa1 (four F16), the iterators write from pa0.
    f.pixel_pa = std::max<u32>({g.pa_count, iterated, 2u});
    LOG_INFO(Render,
             "fragment program {:016x}, blend {:016x}: {} words, {} inputs, {} PAs, {} temps, "
             "{} uniform floats, {} literals",
             hash, blend, code.size(), g.inputs.size(), f.pixel_pa,
             g.temp_count, g.uniform_floats, g.literals.size());
    return f;
}

void RasterizerSgxd::SyncLutLf() {
    auto& lighting = pica.lighting;
    auto& fog = pica.fog;
    if (!lut_rows_ready) {
        lut_row_pass.fill(~0u);
        lut_rg_row_pass.fill(~0u);
        lut_rgba_row_pass.fill(~0u);
        lut_rows_ready = true;
    }
    if (!lighting.lut_dirty && !fog.lut_dirty) {
        return;
    }
    // The ring is two halves. Moving to the other half starts it again with every LUT, so no offset
    // points at the half left behind; the passes reading the half moved to were sent long ago.
    constexpr u32 Half = Pica::Shader::LUT_LF_ROWS / 2;
    const u32 needed = static_cast<u32>(std::popcount(lighting.lut_dirty)) + (fog.lut_dirty ? 1 : 0);
    if (lut_row + needed > Pica::Shader::LUT_LF_ROWS) {
        lut_row = 0;
        lighting.lut_dirty = lighting.LutAllDirty;
        fog.lut_dirty = true;
    }
    const SgxdBackend::Data rows = backend.LutRows();
    const auto take_row = [&] {
        ASSERT_MSG(lut_row_pass[lut_row] != passes,
                   "sgxd: pass {} changed the LUTs more than {} rows' worth: the LUT ring would "
                   "overwrite rows its own draws sample",
                   passes, Half);
        lut_row_pass[lut_row] = passes;
        return lut_row++;
    };
    // The rows are uncached (the shared block is NC).
    constexpr u32 RowBytes = Pica::Shader::LUT_TEX_WIDTH * 8;
    while (lighting.lut_dirty) {
        const u32 index = static_cast<u32>(std::countr_zero(lighting.lut_dirty));
        lighting.lut_dirty &= ~(1u << index);
        const u32 row = take_row();
        float* const out = reinterpret_cast<float*>(rows.cpu + row * RowBytes);
        const auto& lut = lighting.luts[index];
        for (u32 i = 0; i < lut.size(); i++) {
            out[i * 2] = lut[i].ToFloat();
            out[i * 2 + 1] = lut[i].DiffToFloat();
        }
        fs_data.lighting_lut_offset[index / 4][index % 4] =
            static_cast<int>(row * Pica::Shader::LUT_TEX_WIDTH);
    }
    if (fog.lut_dirty) {
        fog.lut_dirty = false;
        const u32 row = take_row();
        float* const out = reinterpret_cast<float*>(rows.cpu + row * RowBytes);
        for (u32 i = 0; i < fog.lut.size(); i++) {
            out[i * 2] = fog.lut[i].ToFloat();
            out[i * 2 + 1] = fog.lut[i].DiffToFloat();
        }
        fs_data.fog_lut_offset = static_cast<int>(row * Pica::Shader::LUT_TEX_WIDTH);
    }
}

namespace {

/// F32 to F16, rounded to nearest even; the proctex tables are within [-1, 1].
u16 ToF16(float value) {
    u32 bits = std::bit_cast<u32>(value);
    const u16 sign = static_cast<u16>((bits >> 16) & 0x8000);
    bits &= 0x7fffffff;
    DEBUG_ASSERT(bits < 0x477ff000); // 65504, the largest half
    if (bits < 0x38800000) {         // under 2^-14: a subnormal half, m * 2^-24
        return static_cast<u16>(sign | static_cast<u16>(std::nearbyint(
                                            std::bit_cast<float>(bits) * 16777216.0f)));
    }
    u32 half = (bits - 0x38000000) >> 13;
    const u32 rest = bits & 0x1fff;
    if (rest > 0x1000 || (rest == 0x1000 && (half & 1) != 0)) {
        half++;
    }
    return static_cast<u16>(sign | half);
}

} // namespace

void RasterizerSgxd::SyncLutProcTex() {
    auto& proctex = pica.proctex;
    if (!proctex.table_dirty) {
        return;
    }
    // As SyncLutLf, and upstream's SyncAndUploadLUTs: a wrap of either ring starts both again
    // with every table. The noise table is not sampled (the emitter has no noise yet).
    const u32 rg_needed = (proctex.color_map_dirty ? 1u : 0u) + (proctex.alpha_map_dirty ? 1u : 0u);
    const u32 rgba_needed = (proctex.lut_dirty ? 1u : 0u) + (proctex.diff_lut_dirty ? 1u : 0u);
    if (lut_rg_row + rg_needed > Pica::Shader::LUT_RG_ROWS ||
        lut_rgba_row + rgba_needed > Pica::Shader::LUT_RGBA_ROWS) {
        lut_rg_row = 0;
        lut_rgba_row = 0;
        proctex.table_dirty = proctex.TableAllDirty;
    }
    const auto take_row = [&](u32& next, auto& row_pass) {
        DEBUG_ASSERT_MSG(row_pass[next] != passes,
                         "sgxd: pass {} changed the proctex tables more than {} rows' worth", passes,
                         row_pass.size() / 2);
        row_pass[next] = passes;
        return next++;
    };
    constexpr u32 RowBytes = Pica::Shader::LUT_TEX_WIDTH * 8;
    const SgxdBackend::Data rg = backend.LutRgRows();
    const auto sync_value = [&](const auto& table, int& offset) {
        const u32 row = take_row(lut_rg_row, lut_rg_row_pass);
        float* const out = reinterpret_cast<float*>(rg.cpu + row * RowBytes);
        for (u32 i = 0; i < table.size(); i++) {
            out[i * 2] = table[i].ToFloat();
            out[i * 2 + 1] = table[i].DiffToFloat();
        }
        offset = static_cast<int>(row * Pica::Shader::LUT_TEX_WIDTH);
    };
    if (proctex.color_map_dirty) {
        sync_value(proctex.color_map_table, fs_data.proctex_color_map_offset);
    }
    if (proctex.alpha_map_dirty) {
        sync_value(proctex.alpha_map_table, fs_data.proctex_alpha_map_offset);
    }
    const SgxdBackend::Data rgba = backend.LutRgbaRows();
    const auto sync_colour = [&](const auto& table, int& offset) {
        const u32 row = take_row(lut_rgba_row, lut_rgba_row_pass);
        u16* const out = reinterpret_cast<u16*>(rgba.cpu + row * RowBytes);
        for (u32 i = 0; i < table.size(); i++) {
            const auto v = table[i].ToVector();
            for (u32 c = 0; c < 4; c++) {
                out[i * 4 + c] = ToF16(static_cast<float>(v[c]) / 255.0f);
            }
        }
        offset = static_cast<int>(row * Pica::Shader::LUT_TEX_WIDTH);
    };
    if (proctex.lut_dirty) {
        sync_colour(proctex.color_table, fs_data.proctex_lut_offset);
    }
    if (proctex.diff_lut_dirty) {
        sync_colour(proctex.color_diff_table, fs_data.proctex_diff_lut_offset);
    }
    proctex.table_dirty = 0;
}

bool RasterizerSgxd::AccelerateImmediateVertex(const Pica::AttributeBuffer& input) {
    // The vertex program reads attributes 0 to max_input_attribute_index; a change of that count
    // between vertices ends the batch.
    const u32 attributes = regs.vs.max_input_attribute_index + 1;
    if (attributes != immediate_attributes) {
        FlushImmediate();
        immediate_attributes = attributes;
    }
    for (u32 attr = 0; attr < attributes; attr++) {
        for (u32 c = 0; c < 4; c++) {
            immediate_vertices.push_back(input[attr][c].ToFloat32());
        }
    }
    return true;
}

void RasterizerSgxd::FlushImmediate() {
    if (immediate_vertices.empty()) {
        return;
    }
    using Topology = Pica::PipelineRegs::TriangleTopology;
    const u32 floats = immediate_attributes * 4;
    const u32 count = static_cast<u32>(immediate_vertices.size()) / floats;
    // A list keeps a partial triangle for the next batch, as the PICA's assembler does; a strip or fan
    // split by a register write between its vertices is drawn as two.
    const u32 drawn = regs.pipeline.triangle_topology == Topology::List ? count - count % 3 : count;
    if (drawn >= 3) {
        Draw(false, immediate_vertices.data(), drawn);
    }
    immediate_vertices.erase(immediate_vertices.begin(),
                             immediate_vertices.begin() + static_cast<std::ptrdiff_t>(
                                 regs.pipeline.triangle_topology == Topology::List
                                     ? drawn * floats
                                     : immediate_vertices.size()));
}

bool RasterizerSgxd::AccelerateDrawBatch(bool is_indexed) {
    FlushImmediate();
    return Draw(is_indexed, nullptr, 0);
}

RasterizerSgxd::VertexStage RasterizerSgxd::VertexStageFor(bool is_indexed, const float* immediate,
                                                         u32 count, Usse::VsMode mode,
                                                         const Describe& describe) {
    const auto& pipeline = regs.pipeline;
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
    // An immediate batch sits in the pass data, each vertex its attributes' four floats.
    SgxdBackend::Data immediate_data{};
    if (immediate) {
        const u32 stride = immediate_attributes * 16;
        immediate_data = backend.AllocData(count * stride);
        std::memcpy(immediate_data.cpu, immediate, count * stride);
        for (u32 attr = 0; attr < immediate_attributes; attr++) {
            const u32 reg = regs.vs.GetRegisterForAttribute(attr);
            feeds[feed_count++] = {reg, immediate_data.gpu + attr * 16, stride, 4};
            enabled_regs |= static_cast<u16>(1u << reg);
            reg_components[reg] = 4;
        }
    }
    for (u32 loader_index = 0; loader_index < 12 && !immediate; loader_index++) {
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
            const auto format = va.GetFormat(attribute_index);
            const bool is_u8 = format == Pica::PipelineRegs::VertexAttributeFormat::UBYTE;
            DEBUG_ASSERT_MSG(is_u8 || format == Pica::PipelineRegs::VertexAttributeFormat::FLOAT,
                             "GXM: {}: attribute {} is format {}; F32 and U8 attributes are built",
                             describe(), attribute_index, static_cast<u32>(format));
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
    // Every register no loader feeds is a uniform: the fixed default attribute where the guest set
    // one, (0, 0, 0, 1) otherwise.
    const u16 default_regs = static_cast<u16>(~enabled_regs);
    const VertexProgram& vp = VertexProgramFor(default_regs, reg_components, u8_regs, mode);
    const Usse::GxpProgram& vg = vp.gxp;

    // --- the index list ---
    // The VDM reads 16-bit indices where the guest put them (the GPU maps guest memory at the
    // PICA's addresses); it has no 8-bit format (IDXSIZE is 16 or 32 bits), so 8-bit indices
    // are widened into the pass data. The vertices an indexed draw reads are
    // [vertex_min, vertex_max], which the cache clean below needs.
    u32 indices_gpu = 0;
    u32 vertex_min = immediate ? 0 : pipeline.vertex_offset;
    u32 vertex_max = vertex_min + count - 1;
    if (is_indexed) {
        const auto& index_info = pipeline.index_array;
        const PAddr index_address = va.GetPhysicalBaseAddress() + index_info.offset;
        const bool index_u16 = index_info.format != 0;
        const u32 index_bytes = count * (index_u16 ? 2 : 1);
        // A geometry draw's copy stands for guest memory.
        const bool indices_mapped = gs_indices_set ||
                                    (index_address >= Memory::FCRAM_PADDR &&
                                     index_address + index_bytes <= Memory::FCRAM_PADDR_END) ||
                                    (index_address >= Memory::VRAM_PADDR &&
                                     index_address + index_bytes <= Memory::VRAM_PADDR_END);
        if (!indices_mapped) {
            // Indices past the guest's memory (a New 3DS's FCRAM range on an Old 3DS): all 0, every triangle
            // degenerate. The TA would stop on the GPU's page fault reading them.
            ASSERT_MSG(mode == Usse::VsMode::Draw,
                       "sgxd: {}: a stream-out's indices at {:#x}+{:#x} are outside the mapped "
                       "guest memory",
                       describe(), index_address, index_bytes);
            static bool logged = false;
            if (!logged) {
                logged = true;
                LOG_WARNING(Render,
                            "sgxd: {}: indices at {:#x}+{:#x} are outside the guest's memory: "
                            "the draw draws nothing (logged once)",
                            describe(), index_address, index_bytes);
            }
            return VertexStage{};
        }
        const u8* const index_cpu = memory.GetPhysicalPointer(index_address);
        // What the CPU reads of them: a geometry draw's copy (GsIndices), else guest memory.
        const u8* const index_scan = gs_indices_set ? gs_indices.data() : index_cpu;
        if (index_u16) {
            DEBUG_ASSERT_MSG(gs_indices_set || (index_address & 1) == 0,
                             "sgxd: {}: 16-bit indices at {:#x}", describe(), index_address);
            const auto [lo, hi] = Common::FindMinMax(
                std::span<const u16>{reinterpret_cast<const u16*>(index_cpu), count});
            vertex_min = lo;
            vertex_max = hi;
            indices_gpu = index_address;
            if (guest_memory.IsUncached(index_address, index_bytes)) {
                SgxdCacheSkipped(CacheSite::Vertices, index_bytes);
            } else {
                indices_gpu = index_address;
                backend.ReadsCached(index_cpu, index_bytes);
                CachedRead(index_address, index_bytes);
            }
        } else {
            const auto [lo, hi] = Common::FindMinMax(std::span<const u8>{index_cpu, count});
            vertex_min = lo;
            vertex_max = hi;
            if (mode == Usse::VsMode::Draw) {
                const SgxdBackend::Data wide = backend.AllocData(count * 2, 4);
                u16* const out = reinterpret_cast<u16*>(wide.cpu);
                for (u32 n = 0; n < count; n++) {
                    out[n] = index_cpu[n];
                }
                indices_gpu = wide.gpu;
            }
        }
    }

    std::array<SgxdVertexFetch, SGXD_PROGRAM_MAX_FETCH> fetch{};
    u32 fetch_count = 0;
    const u32 first = mode != Usse::VsMode::Draw ? vertex_min
                     : is_indexed                ? 0
                                                 : pipeline.vertex_offset;
    for (u32 n = 0; n < feed_count; n++) {
        const Feed& f = feeds[n];
        if ((vp.input_regs & (1u << f.reg)) == 0) {
            continue;
        }
        const std::string name = fmt::format("vs_in_attr{}", f.reg);
        const auto attribute =
            std::find_if(vg.attributes.begin(), vg.attributes.end(),
                         [&](const Usse::GxpAttribute& a) { return a.name == name; });
        DEBUG_ASSERT_MSG(attribute != vg.attributes.end(),
                         "GXM: {}: the program reads {} but has no "
                         "such attribute",
                         describe(), name);
        DEBUG_ASSERT(fetch_count < fetch.size());
        // The identity index list counts from 0, the first vertex in the address; a guest list indexes
        // from the loader's address.
        const PAddr address = immediate ? f.address : f.address + first * f.stride;
        fetch[fetch_count++] = {address, f.stride, f.words, attribute->resource};
        if (immediate) {
            continue;
        }
        // FCRAM is cached for the CPU: what the guest wrote is cleaned to memory before the kick.
        const PAddr read = address + (vertex_min - first) * f.stride;
        const u32 bytes = (vertex_max - vertex_min) * f.stride + f.words * 4;
        // Not seen yet: the TA would stop on the GPU's page fault.
        ASSERT_MSG(
            (read >= Memory::FCRAM_PADDR && read + bytes <= Memory::FCRAM_PADDR_END) ||
                (read >= Memory::VRAM_PADDR && read + bytes <= Memory::VRAM_PADDR_END),
            "sgxd: {}: vertices at {:#x}+{:#x} are outside the mapped guest memory", describe(),
            read, bytes);
        if (guest_memory.IsUncached(read, bytes)) {
            SgxdCacheSkipped(CacheSite::Vertices, bytes);
        } else {
            backend.ReadsCached(memory.GetPhysicalPointer(read), bytes);
            CachedRead(read, bytes);
        }
    }


    VertexStage stage{};
    stage.vp = &vp;
    stage.fetch = fetch;
    stage.fetch_count = fetch_count;
    stage.indices = mode == Usse::VsMode::Draw ? indices_gpu : 0;
    stage.count = count;
    stage.vertex_min = vertex_min;
    stage.vertex_max = vertex_max;
    stage.setup = &pica.vs_setup;
    return stage;
}

SgxdBackend::Data RasterizerGxm::VertexSa(const Usse::GxpProgram& vg,
                                          const Pica::ShaderSetup& setup, u32& vsec_sa,
                                          const Describe& describe) {
    // The image covers the containers, the prefetched uniforms after them and the SAs the
    // secondary program writes; the secondary PDS program DMAs all of it.
    const u32 vsa_words = vg.uniform_floats + Usse::DataHead(vg) + static_cast<u32>(vg.literals.size());
    vsec_sa = std::max(vg.secondary_reg_count, vsa_words);
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
    const auto& uniforms = setup.uniforms;
    const auto& va = regs.pipeline.vertex_attributes;
    for (const Usse::GxpUniform& u : vg.uniforms) {
        if (u.name == "uniforms_f") {
            // The whole bank, f24 as float: rows past the 96 real uniforms read as ones, as the
            // PICA reads them, and rows 0-3 again past 127 (the emitter's wrap mirror).
            DEBUG_ASSERT(vg.buffer_pointer && u.sa_offset + u.array_size * 4 <= vg.buffer_floats);
            float* const bank = reinterpret_cast<float*>(vbuf.cpu) + u.sa_offset;
            for (u32 n = 0; n < u.array_size; n++) {
                const u32 row = n < 128 ? n : n - 128;
                for (u32 c = 0; c < 4; c++) {
                    bank[n * 4 + c] = row < 96 ? uniforms.f[row][c].ToFloat32() : 1.0f;
                }
            }
            continue;
        }
        DEBUG_ASSERT(u.sa_offset + u.components * u.array_size <= vg.uniform_floats);
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
    // The uniforms read with a constant index, as the bank has them: rows past the 96 real
    // uniforms read as ones.
    for (const auto& [sa, row] : vg.prefetched_uniforms) {
        DEBUG_ASSERT(sa + 4 <= vsec_sa && row < 128);
        for (u32 c = 0; c < 4; c++) {
            vsa_f[sa + c] = row < 96 ? uniforms.f[row][c].ToFloat32() : 1.0f;
        }
    }

    return vsa;
}

void RasterizerSgxd::VertexPrograms(const VertexStage& stage, const SgxdBackend::Data& vsa,
                                   u32 vsec_sa, SgxdRecipeDrawPrograms& p,
                                   const Describe& describe) {
    const VertexProgram& vp = *stage.vp;
    const Usse::GxpProgram& vg = vp.gxp;
    SgxdPdsProgram pds{};
    [[maybe_unused]] const int vspds =
        sgxd_program_vertex_pds(stage.fetch.data(), stage.fetch_count, vp.primary_doutu0,
                                vg.per_instance ? 1 : 0, &pds);
    DEBUG_ASSERT_MSG(vspds == 0, "GXM: {}: {} attributes the vertex PDS does not take", describe(),
                     stage.fetch_count);
    p.vspds = backend.PlacePds(pds);
    p.vspds_units = pds.units;
    [[maybe_unused]] const int vsec =
        sgxd_program_secondary_pds(vsa.gpu, vsec_sa, vp.secondary_doutu0, &pds);
    DEBUG_ASSERT(vsec == 0);
    p.vsec = backend.PlacePds(pds);
    p.vsec_units = pds.units;
    p.vsec_sa = vsec_sa;
    sgxd_program_vertex_dms(vg.pa_count, vg.temp_count, vsec_sa, vg.vertex_outputs1 >> 24,
                            &p.vdm_w2, &p.vdm_w4, &p.vdm_w5);
    p.outselects = vg.vertex_outputs1;
    p.texsize = vg.vertex_outputs2;
    p.indices = stage.indices;
}

bool RasterizerSgxd::Draw(bool is_indexed, const float* immediate, u32 immediate_count) {
    using Topology = Pica::PipelineRegs::TriangleTopology;
#ifdef VITA_DIAGNOSTICS
    clip_plane_all++;
    if (regs.rasterizer.clip_enable) {
        clip_plane_draws++;
        const auto c = regs.rasterizer.GetClipCoef();
        clip_plane_last = {c.x.ToFloat32(), c.y.ToFloat32(), c.z.ToFloat32(), c.w.ToFloat32()};
    }
#endif
    // Second-tier programs go in before the draw looks any program up, so no draw swaps tiers halfway.
    if (tier2 && tier2->Ready()) {
        InstallTier2();
    }
    // A DMA into cached memory still queued: the draw reads guest memory as the guest would after its
    // interrupt.
    if (backend.RetirePending()) {
        backend.RetireAll("a draw after a DMA's cache work");
    }
    const auto& pipeline = regs.pipeline;
    const u32 count = immediate ? immediate_count : pipeline.num_vertices;
    const auto describe = [&] {
        return fmt::format("{} draw, topology {}, {} vertices",
                           immediate ? "immediate" : (is_indexed ? "indexed" : "non-indexed"),
                           TopologyName(pipeline.triangle_topology), count);
    };
    DEBUG_ASSERT_MSG(pipeline.use_gs == Pica::PipelineRegs::UseGS::No ||
                         (!immediate && (pipeline.triangle_topology == Topology::Shader ||
                                         pipeline.triangle_topology == Topology::List)),
                     "sgxd: {}: a geometry shader with this topology or in immediate mode is "
                     "not built",
                     describe());
    u32 primitive = SGXD_PROGRAM_TRIANGLES;
    switch (pipeline.triangle_topology) {
    case Topology::List:
    case Topology::Shader: // without a geometry shader, a list (upstream's primitive assembler)
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
    DEBUG_ASSERT_MSG(count > 0 && (is_indexed || count <= SGXD_RECIPE_MAX_INDICES),
                     "sgxd: {}: more vertices than the identity index list", describe());
    SyncDrawUniforms();

    // --- the target and the output merger ---
    const auto& fb = regs.framebuffer.framebuffer;
    const auto& om = regs.framebuffer.output_merger;
    DEBUG_ASSERT_MSG(fb.color_format == FramebufferRegs::ColorFormat::RGBA8,
                     "GXM: {}: colour format {} (RGBA8 only)", describe(),
                     static_cast<u32>(fb.color_format.Value()));
    DEBUG_ASSERT_MSG(fb.allow_color_write != 0 && om.red_enable && om.green_enable &&
                         om.blue_enable && om.alpha_enable,
                     "GXM: {}: a colour write mask is not built", describe());
    DEBUG_ASSERT_MSG(om.fragment_operation_mode == FramebufferRegs::FragmentOperationMode::Default,
                     "GXM: {}: gas and shadow rendering are not built", describe());
    DEBUG_ASSERT_MSG(!om.stencil_test.enable, "GXM: {}: the stencil test is not built", describe());
    const bool blend = om.alphablend_enable != 0;
    u64 blend_word = 0;
    if (blend) {
        const auto& b = om.alpha_blending;
        blend_word = BlendWord(b.blend_equation_rgb, b.blend_equation_a, b.factor_source_rgb,
                               b.factor_dest_rgb, b.factor_source_a, b.factor_dest_a);
        DEBUG_ASSERT_MSG(blend_word != 0,
                         "GXM: {}: blend {}/{} {}/{} {}/{}: a constant factor or ReverseSubtract "
                         "is not built",
                         describe(), static_cast<u32>(b.blend_equation_rgb.Value()),
                         static_cast<u32>(b.blend_equation_a.Value()),
                         static_cast<u32>(b.factor_source_rgb.Value()),
                         static_cast<u32>(b.factor_dest_rgb.Value()),
                         static_cast<u32>(b.factor_source_a.Value()),
                         static_cast<u32>(b.factor_dest_a.Value()));
    } else {
        DEBUG_ASSERT_MSG(om.logic_op == FramebufferRegs::LogicOp::Copy,
                         "sgxd: {}: logic op {} is not built", describe(),
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
        [[maybe_unused]] const bool took = TakeClear(colour, colour + width * height * 4, clear);
        DEBUG_ASSERT_MSG(took,
                         "GXM: {}: the colour buffer at {:#x} has no fill pending, and loading its "
                         "picture into a pass is not built",
                         describe(), colour);
        ClearedRange depth_clear{};
        pass_depth = depth;
        pass_depth_bytes = FramebufferRegs::BytesPerDepthPixel(fb.depth_format);
        pass_depth_cleared = false;
        float bg_depth = 1.0f;
        if (TakeClear(depth, depth + width * height * pass_depth_bytes, depth_clear)) {
            // The depth is the low 16 or 24 bits of each pixel; a fill of another width clears it only when its
            // pattern repeats per pixel.
            const u32 depth_bits = fb.depth_format == FramebufferRegs::DepthFormat::D16 ? 16 : 24;
            const u32 mask = (1u << depth_bits) - 1;
            DEBUG_ASSERT_MSG(depth_clear.bytes == pass_depth_bytes ||
                                 (depth_clear.bytes == 4 && pass_depth_bytes == 2 &&
                                  (depth_clear.value & 0xffff) == depth_clear.value >> 16),
                             "sgxd: {}: the depth buffer at {:#x} ({} bytes a pixel) was cleared "
                             "by a {}-byte fill of {:#x}",
                             describe(), depth, pass_depth_bytes, depth_clear.bytes,
                             depth_clear.value);
            pass_depth_cleared = true;
            bg_depth = static_cast<float>(depth_clear.value & mask) / static_cast<float>(mask);
        }
        pass_surface = static_cast<std::size_t>(&s - surfaces.data());
        backend.BeginPass(s.memory, width, height, Common::swap32(clear.value), bg_depth);
    }
    DEBUG_ASSERT_MSG(
        !uses_depth || pass_depth_cleared,
        "GXM: {}: the depth buffer at {:#x} (format {}) has no fill pending, and "
        "loading depth into a pass is not built",
        describe(), depth, static_cast<u32>(fb.depth_format.Value()));

    // --- the vertex stage: the loaders' attributes into the vertex program, or with a
    // geometry shader, the triangles its passes stored ---
    const VertexStage stage = pipeline.use_gs == Pica::PipelineRegs::UseGS::Yes
                                  ? GeometryStage(is_indexed, describe)
                                  : VertexStageFor(is_indexed, immediate, count,
                                                   Usse::VsMode::Draw, describe);
    if (stage.count == 0) {
        return true; // the geometry program stored no triangle
    }
    const VertexProgram& vp = *stage.vp;
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
            DEBUG_ASSERT(vg.buffer_pointer && u.sa_offset + u.array_size * 4 <= vg.buffer_floats);
            float* const bank = reinterpret_cast<float*>(vbuf.cpu) + u.sa_offset;
            for (u32 n = 0; n < u.array_size; n++) {
                const u32 row = n < 128 ? n : n - 128;
                for (u32 c = 0; c < 4; c++) {
                    bank[n * 4 + c] = row < 96 ? uniforms.f[row][c].ToFloat32() : 1.0f;
                }
            }
            continue;
        }
        DEBUG_ASSERT(u.sa_offset + u.components * u.array_size <= vg.uniform_floats);
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
#ifdef VITA_DIAGNOSTICS
    // sgxd_flags drawkeys: the first thousand draws' fragment program keys (drawkeys_at=S from S s), to
    // tie a replay's per-draw costs to the dumped programs.
    static const bool draw_keys = SgxdFlag("drawkeys") || SgxdFlagValue("drawkeys_at") != 0;
    static const u64 draw_keys_from = u64{SgxdFlagValue("drawkeys_at")} * 1000000;
    static u32 draw_keys_logged = 0;
    if (draw_keys && draw_keys_logged < 1000 && sceKernelGetProcessTimeWide() >= draw_keys_from) {
        LOG_INFO(Render,
                 "drawkey {}: {} vertices, fs {:016x}: {} words, {} temps, {} PAs, {} issues, "
                 "{} samplers, {} literals; {}{} (blend {} {}/{} {}/{} {}/{}, mask {:#x}, own "
                 "pixel {:#x}), depth compare {} write {}; target {:#x} {}x{}",
                 draw_keys_logged++, stage.count, last_fragment_key, fg.primary.size(),
                 fg.temp_count, fp.pixel_pa, fg.issues.size(), fg.samplers.size(),
                 fg.literals.size(), translucent ? "translucent" : "opaque",
                 fg.uses_discard ? fmt::format(" punch-through, phase two at {}",
                                               fg.phase_starts.back())
                                 : std::string{},
                 blend,
                 static_cast<u32>(om.alpha_blending.blend_equation_rgb.Value()),
                 static_cast<u32>(om.alpha_blending.blend_equation_a.Value()),
                 static_cast<u32>(om.alpha_blending.factor_source_rgb.Value()),
                 static_cast<u32>(om.alpha_blending.factor_dest_rgb.Value()),
                 static_cast<u32>(om.alpha_blending.factor_source_a.Value()),
                 static_cast<u32>(om.alpha_blending.factor_dest_a.Value()), mask, draw_own_pixel,
                 dcmp, dwrite, fb.GetColorBufferPhysicalAddress(), fb.GetWidth(),
                 fb.GetHeight());
    }
#endif
    if (!fg.samplers.empty()) {
        SyncLutLf();
        SyncLutProcTex();
    }
    const u32 fsa_words = fg.uniform_floats + static_cast<u32>(fg.literals.size()) +
                          static_cast<u32>(fg.samplers.size()) * 4;
    SgxdBackend::Data fsa{};
    if (fsa_words != 0) {
        fsa = backend.AllocData(fsa_words * 4);
        std::memset(fsa.cpu, 0, fsa_words * 4);
        float* const fsa_f = reinterpret_cast<float*>(fsa.cpu);
        const u8* const fs_bytes = reinterpret_cast<const u8*>(&fs_data);
        for (const UniformSource& u : fp.uniforms) {
            const u8* src = fs_bytes + u.offset;
            float* dst = fsa_f + u.sa;
            for (u32 e = 0; e < u.count; e++, src += u.stride, dst += 4) {
                for (u32 c = 0; c < u.components; c++) {
                    if (u.is_int) {
                        s32 value;
                        std::memcpy(&value, src + c * 4, 4);
                        dst[c] = static_cast<float>(value);
                    } else {
                        std::memcpy(dst + c, src + c * 4, 4);
                    }
                }
            }
        }
        for (size_t i = 0; i < fg.literals.size(); i++) {
            std::memcpy(fsa.cpu + Usse::LiteralSaOffset(fg.uniform_floats, static_cast<u32>(i)) * 4,
                        &fg.literals[i], 4);
        }
        // The LUT textures, linear and point sampled (the program computes texel centres): F32F32 (value,
        // delta) for lighting, fog and the proctex maps, F16x4 for its colours, read by the float4 query.
        for (size_t i = 0; i < fg.samplers.size(); i++) {
            const std::string& name = fg.samplers[i].name;
            u32 words[4];
            if (name == "tex0" || name == "tex1" || name == "tex2") {
                TextureWords(fg.samplers[i].unit, words);
            } else if (name == "texture_buffer_lut_rg") {
                sgxd_recipe_texture_words_format(backend.LutRgRows().gpu,
                                                 Pica::Shader::LUT_TEX_WIDTH,
                                                 Pica::Shader::LUT_RG_ROWS, 0, LutFormatF32F32, 0,
                                                 words);
            } else if (name == "texture_buffer_lut_rgba") {
                sgxd_recipe_texture_words_format(backend.LutRgbaRows().gpu,
                                                 Pica::Shader::LUT_TEX_WIDTH,
                                                 Pica::Shader::LUT_RGBA_ROWS, 0, LutFormatF16x4, 0,
                                                 words);
            } else {
                sgxd_recipe_texture_words_format(backend.LutRows().gpu,
                                                 Pica::Shader::LUT_TEX_WIDTH,
                                                 Pica::Shader::LUT_LF_ROWS, 0, LutFormatF32F32, 0,
                                                 words);
            }
            std::memcpy(fsa.cpu + Usse::SamplerSaOffset(fg.uniform_floats,
                                                        static_cast<u32>(fg.literals.size()),
                                                        static_cast<u32>(i)) *
                                      4,
                        words, sizeof(words));
        }
    }

    // --- the PDS programs ---
    SgxdRecipeDrawPrograms p{};
    p.vspds = backend.PlacePds(pds);
    p.vspds_units = pds.units;
    ASSERT(sgxd_program_secondary_pds(vsa.gpu, vsa_words, vp.secondary_doutu0, &pds) == 0);
    p.vsec = backend.PlacePds(pds);
    p.vsec_units = pds.units;
    p.vsec_sa = vsec_sa;
    [[maybe_unused]] const int fsec =
        sgxd_program_secondary_pds(fsa.gpu, fsa_words, fp.secondary_doutu0, &pds);
    DEBUG_ASSERT(fsec == 0);
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
    p.primitive = primitive;

    const u32 isp = (blend ? 1u : 0u) << 25 | dcmp << 22 | (dwrite ? 0u : 1u << 20) | 1u << 9;
    const SgxdRecipeDraw draw{0, stage.count, isp, SGXD_R_FPRIM,
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
                 draws, describe(), stage.fetch_count, isp, CullMode(regs.rasterizer.cull_mode),
                 p.viewport[0], p.viewport[1], p.viewport[2], p.viewport[3], p.vspds, p.vsec,
                 vsec_sa, p.fsec, p.fprim);
        LOG_INFO(Render, "draw {}: depth test {} ({}), write {}, blend {}; vertex SAs [{}]",
                 draws, om.depth_test_enable != 0, dcmp, dwrite, blend,
                 fmt::join(std::span<const float>(vsa_f, std::min(vsa_words, 16u)), " "));
    }
    return true;
}

} // namespace GxmRenderer
