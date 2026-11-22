// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <atomic>
#include <arm_neon.h>
#include <fmt/ranges.h>
#include <nihstro/shader_bytecode.h>
#include "common/alignment.h"
#include "common/assert.h"
#include "common/color.h"
#include "common/hash.h"
#include "common/logging/log.h"
#include "common/math_util.h"
#include "common/settings.h"
#include "common/swap.h"
#include "core/memory.h"
#include "video_core/pica/pica_core.h"
#include "video_core/pica/regs_external.h"
#include "video_core/pica/shader_setup.h"
#include "video_core/pica/shader_unit.h"
#include "video_core/pica/vertex_loader.h"
#include "video_core/renderer_sgxd/sgxd_dump.h"
#include "video_core/renderer_sgxd/sgxd_flags.h"
#include "video_core/renderer_sgxd/sgxd_rasterizer.h"
#include "video_core/renderer_sgxd/untile_geometry.h"
#include "video_core/renderer_sgxd/usse/fs_usse_gen.h"
#include "video_core/renderer_sgxd/usse/usse_encoder.h"
#include "video_core/renderer_sgxd/usse/usse_ir.h"
#include "video_core/renderer_sgxd/usse/usse_program.h"
#include "video_core/renderer_sgxd/usse/vs_usse_gen.h"
#include "video_core/renderer_sgxd/vprof.h"
#include "video_core/shader/generator/shader_gen.h"
#include "video_core/shader/shader.h"
#include "video_core/texture/texture_decode.h"

#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>
#include "azahar_native.h"
#include "common/host_clock.h"
#include "sgxd.h"
#include "sgxd_program.h"
#include "sgxd_ptla.h"
#include "sgxd_session.h"

namespace SgxdRenderer {

namespace {

/// The 4 KiB guest pages of VRAM and the New 3DS's FCRAM (TextureBucket's index).
constexpr u32 GuestPages = (Memory::VRAM_SIZE + Memory::FCRAM_N3DS_SIZE) >> 12;

/// The register words of each texture unit that TextureFor's memos follow.
#define TEXTURE_UNIT_WORDS(n)                                                                      \
    {PICA_REG_INDEX(texturing.texture##n.width), PICA_REG_INDEX(texturing.texture##n.type),        \
     PICA_REG_INDEX(texturing.texture##n.lod), PICA_REG_INDEX(texturing.texture##n.address),       \
     PICA_REG_INDEX(texturing.texture##n##_format)}
constexpr std::array<std::array<u16, 5>, 3> TextureUnitWords{
    {TEXTURE_UNIT_WORDS(0), TEXTURE_UNIT_WORDS(1), TEXTURE_UNIT_WORDS(2)}};
#undef TEXTURE_UNIT_WORDS

/// How the untile reads a PICA texture format: the texture unit's format code and swizzle, its
/// bits a texel, and the palette that expands a 4-bit index. Code 0xff: not built.
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
    case F::RG8: // byte 0 green, byte 1 red: U8U8 as RG, blue and alpha 0 (the texture's
                 // alpha_one reads alpha 1, as upstream's DecodeRG8)
        return {0x07, 5, 16};
    case F::I8: // U8 as 1RRR
        return {0x00, 5, 8};
    case F::A8: // U8 as R000
        return {0x00, 6, 8};
    // The 4-bit formats: a nibble a texel, the low one first; IA4 a byte, intensity in the high
    // nibble.
    case F::IA4:
        return {0x95, 0, 8, TexturePalette::IA4};
    case F::I4:
        return {0x94, 0, 4, TexturePalette::I4};
    case F::A4:
        return {0x94, 0, 4, TexturePalette::A4};
    case F::ETC1: // the texture unit's ETC1, its blocks moved (QueueUntile; a cube face: Untile)
        return {SgxdBackend::UntileEtc1, 0, 4};
    case F::ETC1A4: // its colour as ETC1's, the alpha joined by a merge render (Untile)
        return {SgxdBackend::UntileEtc1A4, 0, 8};
    default:
        return {0xff, 0, 0};
    }
}


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

// The fragment program's epilogue (as libgxm's shader patcher appends it): the colour as F16 in
// pa0, packed to the output with END, or packed in place and blended over o0.
constexpr u64 PackToOutput = 0x40a40a3da01d8000ull; // pack.u8.f16.scl o0, pa0 (end)
constexpr u64 PackInPlace = 0x40a00a3ea01d8000ull;  // pack.u8.f16.scl pa0, pa0
// VPCK's destination mask (bits 37:34, bit 34 R): an unwritten byte keeps what o0 held.
constexpr u64 PackMask = 0xfull << 34;

/// LIMM: a 32-bit immediate into register `num` of bank `bank` (0 r, 1 o, 2 pa), with the end bit.
constexpr u64 Limm(u32 bank, u32 num, u32 value, bool end) {
    return u64{0xfc} << 56 | u64{1} << 55 | u64{2} << 52 | u64{end} << 50 |
           u64{(value >> 26) & 0x3f} << 44 | u64{(value >> 21) & 0x1f} << 36 | u64{bank} << 32 |
           u64{num} << 21 | (value & 0x1fffff);
}
static_assert(Limm(1, 0, 0xffffffffu, true) == 0xfca7f1f1001fffffull);
static_assert(Limm(2, 0, 0x80402010u, false) == 0xfca2002200002010ull);
// The texture format code of F32F32, read by the float4 query.
constexpr unsigned LutFormatF32F32 = 0x1e;
// F16F16F16F16 (0x1b), the proctex colour rows.
constexpr unsigned LutFormatF16x4 = 0x1b;
// F16 (0x0b), the lighting values' filtered rows (F16 filters linearly, F32 does not).
constexpr unsigned LutFormatF16 = 0x0b;

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

/// A PICA factor for the swapped SOP2, src1 the output buffer and src2 the fragment.
/// SRC_ALPHA_SATURATE has none (7, 3).
constexpr Sop2Factor ColourFactorSwapped(FramebufferRegs::BlendFactor f) {
    using F = FramebufferRegs::BlendFactor;
    switch (f) {
    case F::Zero:
        return {0, 0};
    case F::One:
        return {0, 1};
    case F::SourceColor:
        return {2, 0};
    case F::OneMinusSourceColor:
        return {2, 1};
    case F::DestColor:
        return {1, 0};
    case F::OneMinusDestColor:
        return {1, 1};
    case F::SourceAlpha:
        return {4, 0};
    case F::OneMinusSourceAlpha:
        return {4, 1};
    case F::DestAlpha:
        return {3, 0};
    case F::OneMinusDestAlpha:
        return {3, 1};
    default:
        return {7, 0};
    }
}

constexpr Sop2Factor AlphaFactorSwapped(FramebufferRegs::BlendFactor f) {
    const Sop2Factor a = AlphaFactor(f);
    return {a.sel == 1 ? 2u : a.sel == 2 ? 1u : a.sel, a.complement};
}

/// ReverseSubtract as the SOP2's SUB with its operands swapped (`sop2 o0, o0, pa0`). Exact when
/// neither equation is a plain Subtract and no colour factor is SRC_ALPHA_SATURATE; 0 otherwise.
u64 BlendWordSwapped(FramebufferRegs::BlendEquation eq_rgb, FramebufferRegs::BlendEquation eq_a,
                     FramebufferRegs::BlendFactor src_rgb, FramebufferRegs::BlendFactor dst_rgb,
                     FramebufferRegs::BlendFactor src_a, FramebufferRegs::BlendFactor dst_a) {
    using Eq = FramebufferRegs::BlendEquation;
    const auto op = [](Eq eq) -> u32 {
        switch (eq) {
        case Eq::Add:
            return 0;
        case Eq::ReverseSubtract:
            return 1;
        case Eq::Min:
            return 2;
        case Eq::Max:
            return 3;
        default:
            return 4;
        }
    };
    const Sop2Factor c1 = ColourFactorSwapped(dst_rgb), c2 = ColourFactorSwapped(src_rgb);
    const Sop2Factor a1 = AlphaFactorSwapped(dst_a), a2 = AlphaFactorSwapped(src_a);
    const u32 cop = op(eq_rgb), aop = op(eq_a);
    if (c1.sel == 7 || c2.sel == 7 || a1.sel == 3 || a2.sel == 3 || cop == 4 || aop == 4) {
        return 0;
    }
    Usse::SopCtl s;
    s.csel1 = static_cast<u8>(c1.sel);
    s.cmod1 = c1.complement != 0;
    s.csel2 = static_cast<u8>(c2.sel);
    s.cmod2 = c2.complement != 0;
    s.asel1 = static_cast<u8>(a1.sel);
    s.amod1 = a1.complement != 0;
    s.asel2 = static_cast<u8>(a2.sel);
    s.amod2 = a2.complement != 0;
    s.cop = static_cast<u8>(cop);
    s.aop = static_cast<u8>(aop);
    Usse::Encoder enc;
    enc.Sop2(Usse::Reg::O(0), Usse::Reg::O(0), Usse::Reg::Pa(0), s, true);
#ifdef VITA_DIAGNOSTICS
    // The encoder's unswapped form of the same fields is BlendWord's.
    static const bool checked = [] {
        Usse::SopCtl f;
        f.csel1 = 3, f.csel2 = 3, f.cmod2 = true, f.asel1 = 1, f.asel2 = 1, f.amod2 = true;
        Usse::Encoder forward;
        forward.Sop2(Usse::Reg::O(0), Usse::Reg::Pa(0), Usse::Reg::O(0), f, true);
        DEBUG_ASSERT_MSG(forward.Words()[0] == BlendSourceAlpha,
                         "sgxd: the encoder's blend SOP2 {:016x} is not BlendSourceAlpha {:016x}",
                         forward.Words()[0], BlendSourceAlpha);
        return true;
    }();
#endif
    return enc.Words()[0];
}

/// A constant factor beside the fragment's own colour, which no one SOP2 does: with the constant
/// in pa2, one SOP3 for the alpha into pa1 and one for the colour into o0, then a select takes
/// pa1's alpha byte (BlendSplitSelect). MIN or MAX in alpha ignores the factors and runs as a
/// SOP2, whose MIN and MAX are verified on the hardware (SOP3's are not). False for MIN, MAX or
/// SRC_ALPHA_SATURATE in colour.
bool BlendSplitWords(FramebufferRegs::BlendEquation eq_rgb, FramebufferRegs::BlendEquation eq_a,
                     FramebufferRegs::BlendFactor src_rgb, FramebufferRegs::BlendFactor dst_rgb,
                     FramebufferRegs::BlendFactor src_a, FramebufferRegs::BlendFactor dst_a,
                     u64& alpha_word, u64& colour_word) {
    using Eq = FramebufferRegs::BlendEquation;
    using F = FramebufferRegs::BlendFactor;
    const auto op = [](Eq eq, bool& swap) -> int {
        swap = eq == Eq::ReverseSubtract;
        return eq == Eq::Add ? 0 : eq == Eq::Subtract || eq == Eq::ReverseSubtract ? 1 : -1;
    };
    // A colour factor's CSEL (fragment and buffer as src1 and src2, or swapped); -1 for none.
    const auto colour = [](F f, bool swap) -> std::pair<int, bool> {
        const int frag = swap ? 6 : 2, buf = swap ? 2 : 6;
        switch (f) {
        case F::Zero:
            return {0, false};
        case F::One:
            return {0, true};
        case F::SourceColor:
            return {frag, false};
        case F::OneMinusSourceColor:
            return {frag, true};
        case F::DestColor:
            return {buf, false};
        case F::OneMinusDestColor:
            return {buf, true};
        case F::SourceAlpha:
            return {frag + 1, false};
        case F::OneMinusSourceAlpha:
            return {frag + 1, true};
        case F::DestAlpha:
            return {buf + 1, false};
        case F::OneMinusDestAlpha:
            return {buf + 1, true};
        case F::ConstantColor:
            return {4, false};
        case F::OneMinusConstantColor:
            return {4, true};
        case F::ConstantAlpha:
            return {5, false};
        case F::OneMinusConstantAlpha:
            return {5, true};
        default:
            return {-1, false};
        }
    };
    // An alpha factor: which alpha (0 zero/one, 1 the fragment's, 2 the buffer's, 3 the constant's)
    // and whether complemented.
    const auto alpha = [](F f) -> std::pair<int, bool> {
        switch (f) {
        case F::Zero:
            return {0, false};
        case F::One:
        case F::SourceAlphaSaturate:
            return {0, true};
        case F::SourceColor:
        case F::SourceAlpha:
            return {1, false};
        case F::OneMinusSourceColor:
        case F::OneMinusSourceAlpha:
            return {1, true};
        case F::DestColor:
        case F::DestAlpha:
            return {2, false};
        case F::OneMinusDestColor:
        case F::OneMinusDestAlpha:
            return {2, true};
        case F::ConstantColor:
        case F::ConstantAlpha:
            return {3, false};
        default:
            return {3, true};
        }
    };
    bool swap_rgb = false, swap_a = false;
    const int cop = op(eq_rgb, swap_rgb), aop = op(eq_a, swap_a);
    const auto c1 = colour(swap_rgb ? dst_rgb : src_rgb, swap_rgb);
    const auto c2 = colour(swap_rgb ? src_rgb : dst_rgb, swap_rgb);
    const bool alpha_min_max = eq_a == Eq::Min || eq_a == Eq::Max;
    if (cop < 0 || (aop < 0 && !alpha_min_max) || c1.first < 0 || c2.first < 0) {
        return false;
    }
    Usse::Encoder enc;
    const Usse::Reg frag = Usse::Reg::Pa(0), buffer = Usse::Reg::O(0);
    if (alpha_min_max) {
        Usse::SopCtl sa;
        sa.amod1 = true;
        sa.amod2 = true;
        sa.aop = eq_a == Eq::Min ? 2 : 3;
        enc.Sop2(Usse::Reg::Pa(1), frag, buffer, sa);
    } else {
        // The alpha into pa1: ASEL1 (1 src0, 2 src1, 3 src2) and CSEL2's alpha (5 src0, 3 src1, 7 src2).
        const auto a1 = alpha(swap_a ? dst_a : src_a), a2 = alpha(swap_a ? src_a : dst_a);
        const auto which = [&](int k, bool first) -> u8 {
            const int src = k == 3 ? 0 : (k == 1) != swap_a ? 1 : 2;
            if (first) {
                return static_cast<u8>(src == 0 ? 1 : src == 1 ? 2 : 3);
            }
            return static_cast<u8>(src == 0 ? 5 : src == 1 ? 3 : 7);
        };
        Usse::SopCtl sa;
        sa.asel1 = a1.first == 0 ? 0 : which(a1.first, true);
        sa.amod1 = a1.second;
        sa.csel2 = a2.first == 0 ? 0 : which(a2.first, false);
        sa.cmod2 = a2.second;
        sa.aop = static_cast<u8>(aop);
        enc.Sop3(Usse::Reg::Pa(1), Usse::Reg::Pa(2), swap_a ? buffer : frag,
                 swap_a ? frag : buffer, sa);
    }
    Usse::SopCtl sc;
    sc.csel1 = static_cast<u8>(c1.first);
    sc.cmod1 = c1.second;
    sc.csel2 = static_cast<u8>(c2.first);
    sc.cmod2 = c2.second;
    sc.cop = static_cast<u8>(cop);
    enc.Sop3(buffer, Usse::Reg::Pa(2), swap_rgb ? buffer : frag, swap_rgb ? frag : buffer, sc);
    alpha_word = enc.Words()[0];
    colour_word = enc.Words()[1];
    return true;
}

/// The split blend's end: `sop3 o0, pa2, pa1, o0` with pa2 0xff in the alpha byte only.
u64 BlendSplitSelect() {
    Usse::Encoder enc;
    Usse::SopCtl s;
    s.csel1 = 4;
    s.csel2 = 4;
    s.cmod2 = true;
    s.asel1 = 1;
    enc.Sop3(Usse::Reg::O(0), Usse::Reg::Pa(2), Usse::Reg::Pa(1), Usse::Reg::O(0), s, true);
    return enc.Words()[0];
}

/// A U8 program's output into o0, ending the program: `sop2 o0, pa0, pa0`.
u64 U8OutputCopy() {
    Usse::Encoder enc;
    Usse::SopCtl s;
    s.cmod1 = s.amod1 = true;
    enc.Sop2(Usse::Reg::O(0), Usse::Reg::Pa(0), Usse::Reg::Pa(0), s, true);
    return enc.Words()[0];
}

/// The same under a write mask: `sop3 o0, pa1, pa0, o0` with pa1 0xff in each byte written and 0
/// in each kept (exact: each product is x * 255 / 255 or zero).
u64 U8OutputSelect() {
    Usse::Encoder enc;
    Usse::SopCtl s;
    s.csel1 = 4;
    s.csel2 = 4;
    s.cmod2 = true;
    s.asel1 = 1;
    enc.Sop3(Usse::Reg::O(0), Usse::Reg::Pa(1), Usse::Reg::Pa(0), Usse::Reg::O(0), s, true);
    return enc.Words()[0];
}

/// A blend SOP2 into pa0 instead of o0, not ending the program (U8OutputSelect then writes the
/// masked bytes): the destination and end fields differ.
u64 BlendIntoPa0(u64 blend) {
    static const u64 retarget = [] {
        Usse::Encoder to_o0, to_pa0;
        to_o0.Sop2(Usse::Reg::O(0), Usse::Reg::Pa(0), Usse::Reg::O(0), {}, true);
        to_pa0.Sop2(Usse::Reg::Pa(0), Usse::Reg::Pa(0), Usse::Reg::O(0), {}, false);
        return to_o0.Words()[0] ^ to_pa0.Words()[0];
    }();
    return blend ^ retarget;
}

/// The epilogue's byte mask (bit 0 R) as U8OutputSelect's pa1.
constexpr u32 U8ByteMask(u8 mask) {
    u32 word = 0;
    for (u32 i = 0; i < 4; i++) {
        word |= (mask >> i & 1) ? 0xffu << (i * 8) : 0;
    }
    return word;
}

/// The epilogue's blend: into o0, or with R, G and B masked apart into pa0 and through the mask.
void PushBlend(std::vector<u64>& code, u64 blend, u8 mask) {
    if (mask == 0xf) {
        code.push_back(blend);
        return;
    }
    code.push_back(BlendIntoPa0(blend));
    code.push_back(Limm(2, 1, U8ByteMask(mask), false));
    code.push_back(U8OutputSelect());
}

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

/// The MTE's cull mode. The draws render the target transposed (SgxdBackend::PicaTarget), which
/// keeps a triangle's winding, so the PICA's modes map as they are.
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

/// Rows [row, row + count) of the PICA's float uniforms as F32: the 96 real rows, then ones.
void CopyUniformRows(float* out, u32 row, u32 count, const Pica::Uniforms& uniforms) {
    static_assert(sizeof(Common::Vec4<f24>) == 4 * sizeof(float));
    constexpr u32 Real = static_cast<u32>(std::tuple_size_v<decltype(uniforms.f)>);
    const u32 real = row < Real ? std::min(count, Real - row) : 0;
    if (real != 0) {
        std::memcpy(out, uniforms.f.data() + row, real * sizeof(uniforms.f[0]));
    }
    std::fill(out + real * 4, out + count * 4, 1.0f);
}

/// A fragment uniform's byte offset in FSUniformData, and whether it holds ints.
struct UniformField {
    std::size_t offset;
    bool is_int;
};

UniformField FragmentUniformField(const Usse::Uniform& u) {
    using D = Pica::Shader::Generator::FSUniformData;
    using L = Pica::Shader::Generator::LightSrc;
    using K = Usse::UniformKind;
    const bool lut = u.kind == K::LightingLutRow || u.kind == K::LightingLutCoordHalf ||
                     u.kind == K::LightingLutCoordWrap;
    DEBUG_ASSERT(u.index < (lut ? 24 : 8));
    const std::size_t light = offsetof(D, light_src) + u.index * sizeof(L);
    switch (u.kind) {
    case K::AlphaTestRef:
        return {offsetof(D, alphatest_ref), true};
    case K::ConstColor:
        // The array from stage 0, or one stage's colour (a moved channel's broadcast).
        return {offsetof(D, const_color) + u.index * sizeof(D::const_color[0]), false};
    case K::BufferColor:
        return {offsetof(D, tev_combiner_buffer_color), false};
    case K::ScissorX1:
        return {offsetof(D, scissor_x1), true};
    case K::ScissorY1:
        return {offsetof(D, scissor_y1), true};
    case K::ScissorX2:
        return {offsetof(D, scissor_x2), true};
    case K::ScissorY2:
        return {offsetof(D, scissor_y2), true};
    case K::FogLutOffset:
        return {offsetof(D, fog_lut_offset), true};
    case K::FogColor:
        return {offsetof(D, fog_color), false};
    case K::TexBorderColor:
        return {offsetof(D, tex_border_color), false};
    case K::ProctexLutOffset:
        return {offsetof(D, proctex_lut_offset), true};
    case K::ProctexDiffLutOffset:
        return {offsetof(D, proctex_diff_lut_offset), true};
    case K::ProctexColorMapOffset:
        return {offsetof(D, proctex_color_map_offset), true};
    case K::ProctexAlphaMapOffset:
        return {offsetof(D, proctex_alpha_map_offset), true};
    case K::ProctexBias:
        return {offsetof(D, proctex_bias), false};
    case K::ProctexNoiseLutOffset:
        return {offsetof(D, proctex_noise_lut_offset), true};
    case K::ProctexNoiseF:
        return {offsetof(D, proctex_noise_f), false};
    case K::ProctexNoiseA:
        return {offsetof(D, proctex_noise_a), false};
    case K::ProctexNoiseP:
        return {offsetof(D, proctex_noise_p), false};
    case K::LightingLutOffset:
        return {offsetof(D, lighting_lut_offset), true};
    case K::LightingLutRow:
    case K::LightingLutCoordHalf:
    case K::LightingLutCoordWrap:
        // LUT `index`'s offset, one int of the int4[6]
        return {offsetof(D, lighting_lut_offset) + u.index * sizeof(s32), true};
    case K::LightingGlobalAmbient:
        return {offsetof(D, lighting_global_ambient), false};
    case K::LightSpecular0:
        return {light + offsetof(L, specular_0), false};
    case K::LightSpecular1:
        return {light + offsetof(L, specular_1), false};
    case K::LightDiffuse:
        return {light + offsetof(L, diffuse), false};
    case K::LightAmbient:
        return {light + offsetof(L, ambient), false};
    case K::LightPosition:
    case K::LightDirection:
        return {light + offsetof(L, position), false};
    case K::LightSpotDirection:
        return {light + offsetof(L, spot_direction), false};
    case K::LightDistAttenBias:
        return {light + offsetof(L, dist_atten_bias), false};
    case K::LightDistAttenScale:
        return {light + offsetof(L, dist_atten_scale), false};
    case K::ShadowBiasConstant:
        return {offsetof(D, shadow_bias_constant), false};
    case K::ShadowBiasLinear:
        return {offsetof(D, shadow_bias_linear), false};
    case K::ShadowTextureBias:
        return {offsetof(D, shadow_texture_bias), true};
    case K::Dot3Const:
        return {offsetof(D, const_color) + u.index * sizeof(D::const_color[0]), false};
    default:
        UNREACHABLE_MSG("sgxd: the fragment uniform {} has no value", Usse::UniformName(u));
    }
}

} // Anonymous namespace

namespace {
std::atomic<u32> top_scale{4};
std::atomic<u32> top_margin{0};
} // namespace

RasterizerSgxd::RasterizerSgxd(Pica::PicaCore& pica_, Memory::MemorySystem& memory_,
                             GuestGpuMemory& guest_memory_)
    : VideoCore::RasterizerAccelerated{memory_, pica_}, guest_memory{guest_memory_} {
    Vprof::Start(memory_);
#ifdef VITA_DIAGNOSTICS
    Usse::sched_probe.lut_reads = SgxdFlagValue("fslut");
    Usse::sched_probe.lut_kind = SgxdFlagValue("fslutkind");
    Usse::sched_probe.nosched = static_cast<int>(SgxdFlagValue("fsnosched"));
#endif
#ifdef VITA_DIAGNOSTICS
    Usse::Encoder::probe_skip_point_reads = SgxdFlag("smpskip");
#endif
#ifdef VITA_DIAGNOSTICS
    // zflip: clip z = 2z + w, the TA's near plane the PICA's z = -w and its far plane z = 0.
    Usse::z_flip = SgxdFlag("zflip");
#endif
#ifdef VITA_DIAGNOSTICS
    // sgxd_flags scaledmask: a scaled pass storing a part keeps its draws in it by the ISP's
    // mask, not by the depth scratch and its copy (the mask's hang, open).
    scaled_mask = SgxdFlag("scaledmask");
    // sgxd_flags widescreen=N: the top screen N rows wider each side whatever the frontend's
    // layout asks (frame replays of the widescreen passes).
    if (const u32 margin = SgxdFlagValue("widescreen"); margin != 0) {
        SetWidescreen(margin);
        top_margin_forced = true;
    }
#endif
    if (!SgxdFlag("notier2")) {
        tier2 = std::make_unique<ShaderTier2>();
    }
    tier1 = std::make_unique<ShaderTier1>(&disk_cache);
    if (!SgxdFlag("notier2")) {
        tier2 = std::make_unique<ShaderTier2>(&disk_cache, *tier1);
    }
    // The pool's room (SgxdBackend::MakeRoom): every copy but the open pass's may go.
    backend.SetTextureDropper(
        [this](std::vector<SgxdBackend::Droppable>& out) {
            for (const Texture& t : textures) {
                if (t.last_used >= passes) {
                    continue;
                }
                if (t.bytes != 0) {
                    out.push_back({t.memory.gpu, t.bytes, &t});
                }
                if (t.alpha_bytes != 0) {
                    out.push_back({t.alpha.gpu, t.alpha_bytes, &t});
                }
            }
        },
        [this](const void* key) {
            Texture& t = *const_cast<Texture*>(static_cast<const Texture*>(key));
            DEBUG_ASSERT(t.last_used < passes);
            DropTexture(t);
            evictions++;
        });
    backend.SetRoomTaken([this](u32 begin, u32 end) { KeepGsRows(begin, end); });
}

RasterizerSgxd::~RasterizerSgxd() {
    if (upscale_instance == this) {
        upscale_instance = nullptr;
    }
}

RasterizerSgxd* RasterizerSgxd::upscale_instance = nullptr;

void RasterizerSgxd::SetTopScale(u32 quarters) {
    top_scale.store(std::clamp(quarters, 4u, 8u), std::memory_order_relaxed);
}

u32 RasterizerSgxd::TopScale() {
    return top_scale.load(std::memory_order_relaxed);
}

void RasterizerSgxd::SetWidescreen(u32 margin) {
    // A multiple of 4: whole pixels at every scale (PassX).
    top_margin.store(std::min(margin, 32u) & ~3u, std::memory_order_relaxed);
}

u32 RasterizerSgxd::Widescreen() {
    return top_margin.load(std::memory_order_relaxed);
}

bool RasterizerSgxd::ScalesWhole(const ColourBuffer& target, u32 quarters, u32 margin) {
    // The 1x copy's sizes (DownscalePicture): the transpose in 8s by 32 or more in 16s, at most
    // ScaledSideMax a side. Widescreen, the margins wider.
    return target.height % 8 == 0 && target.width % 16 == 0 && target.width >= 32 &&
           (target.height + 2 * margin) * quarters / 4 <= ScaledSideMax &&
           target.width * quarters / 4 <= ScaledSideMax;
}

RasterizerSgxd::ColourBuffer RasterizerSgxd::TopWindowFor(const ColourBuffer& buffer) const {
    const u32 q = TopScale();
    const u32 margin = Widescreen();
    if ((q == 4 && margin == 0) || ScalesWhole(buffer, q, margin)) {
        return buffer;
    }
    // A top-screen transfer from inside it, whole strips of 8 rows in: the rows from there to the
    // buffer's end. The window's rows count up from the buffer's last memory row, so they number the
    // same in both.
    const u32 strip = buffer.width * 8 * buffer.bpp;
    ColourBuffer window = buffer;
    for (const PAddr s : top_sources) {
        if (s > buffer.addr && s < buffer.End() && (s - buffer.addr) % strip == 0 &&
            s > window.addr) {
            const ColourBuffer w{s, buffer.width, buffer.height - (s - buffer.addr) / strip * 8,
                                 buffer.format, buffer.bpp};
            if (ScalesWhole(w, q, margin)) {
                window = w;
            }
        }
    }
    return window;
}

RasterizerSgxd::Upscaled* RasterizerSgxd::UpscaledFor(const ColourBuffer& target) {
    upscale_instance = this;
    const u32 q = TopScale();
    const u32 margin = Widescreen();
    if (q == 4 && margin == 0) {
        return nullptr;
    }
    if (std::none_of(top_sources.begin(), top_sources.end(), [&](PAddr s) {
            return s >= target.addr && s < target.End();
        })) {
        return nullptr;
    }
    if (!ScalesWhole(target, q, margin)) {
        return nullptr;
    }
    const auto make = [&](Upscaled& u) {
        u = Upscaled{target.addr, target.width, target.height, target.End() - target.addr, q,
                     margin};
        u.stride = Common::AlignUp((target.height + 2 * margin) * q / 4, 8u);
        u.picture = backend.AllocSurface(u.stride, target.width * q / 4, true);
    };
    for (Upscaled& u : upscaled) {
        if (u.addr == target.addr) {
            if (u.width != target.width || u.height != target.height || u.quarters != q ||
                u.margin != margin) {
                backend.FreeSurface(u.picture, u.stride, u.width * u.quarters / 4);
                make(u);
            }
            return &u;
        }
    }
    make(upscaled.emplace_back());
    LOG_INFO(Render, "sgxd: the top screen's {}x{} colour buffer at {:#x} is drawn at {}/4x, {} "
                     "rows wider each side",
             target.width, target.height, target.addr, q, margin);
    return &upscaled.back();
}

void RasterizerSgxd::NoteTopTransfer(PAddr input, PAddr output, u32 out_width, u32 out_height,
                                    u32 bpp, bool flip) {
    if (std::find(top_sources.begin(), top_sources.end(), input) == top_sources.end()) {
        if (top_sources.size() == 4) {
            top_sources.erase(top_sources.begin());
        }
        top_sources.push_back(input);
    }
    const auto sp = std::find_if(screen_pictures.begin(), screen_pictures.end(),
                                 [output](const ScreenPicture& p) { return p.output == output; });
    const auto u = std::find_if(upscaled.begin(), upscaled.end(), [input](const Upscaled& x) {
        return input >= x.addr && input < x.addr + x.bytes;
    });
    // From inside the buffer, whole strips of 8 rows in: the transfer's first row.
    const u32 strip = u != upscaled.end() ? u->bytes / u->height * 8 : 0;
    const u32 first_row = u != upscaled.end() ? (input - u->addr) / strip * 8 : 0;
    if ((TopScale() == 4 && Widescreen() == 0) || u == upscaled.end() || !u->valid ||
        (input - u->addr) % strip != 0 || out_width > u->width ||
        first_row + out_height > u->height || out_width * u->quarters % 4 != 0 ||
        out_height * u->quarters % 4 != 0) {
        if (sp != screen_pictures.end()) {
            sp->valid = false;
        }
        return;
    }
    // The transfer takes out_width columns of out_height rows from first_row on: the picture's
    // out_height pixels from first_row's of its first out_width rows, scaled (its transpose),
    // with the widescreen margins either side. The present reads the copy's rows 8s of pixels
    // apart; the columns past those are the picture's next ones, never shown.
    const u32 q = u->quarters;
    const u32 w = Common::AlignUp((out_height + 2 * u->margin) * q / 4, 8u), h = out_width * q / 4;
    ScreenPicture* p = sp != screen_pictures.end() ? &*sp : nullptr;
    if (p && (p->width != w || p->height != h)) {
        backend.FreeSurface(p->picture, p->width, p->height);
        p->picture = backend.AllocSurface(w, h, true);
    }
    if (!p) {
        p = &screen_pictures.emplace_back(ScreenPicture{output, 0, 0, backend.AllocSurface(w, h, true)});
        LOG_INFO(Render, "sgxd: the scaled copy of the top screen's transfers to {:#x} ({} in all)",
                 output, screen_pictures.size());
    }
    p->bytes = out_width * out_height * bpp;
    p->row_bytes = out_width * bpp;
    p->width = w;
    p->height = h;
    p->quarters = q;
    p->margin = u->margin;
    p->flip = flip;
    if (backend.InPass()) {
        EndPass("the upscaled top screen's copy");
    }
    backend.CopyPicture(p->picture.gpu, u->picture.gpu, w, h, u->stride, first_row * q / 4);
    p->valid = true;
}

bool RasterizerSgxd::TopScreenPicture(PAddr lcd, u32& gpu, u32& width, u32& height, u32& row,
                                     u32& quarters, u32& margin, bool& flip) {
    const RasterizerSgxd* r = upscale_instance;
    if (!r || (TopScale() == 4 && Widescreen() == 0)) {
        return false;
    }
    for (const ScreenPicture& p : r->screen_pictures) {
        if (p.valid && lcd >= p.output && lcd < p.output + p.bytes &&
            (lcd - p.output) % p.row_bytes == 0 &&
            (lcd - p.output) / p.row_bytes + 400 <= p.bytes / p.row_bytes &&
            p.height >= 60 * p.quarters) {
            gpu = p.picture.gpu;
            width = p.width;
            height = p.height;
            row = (lcd - p.output) / p.row_bytes;
            // Flipped, the output's row r is the picture's row count - r.
            if (p.flip) {
                row = p.bytes / p.row_bytes - row;
            }
            quarters = p.quarters;
            margin = p.margin;
            flip = p.flip;
            return true;
        }
    }
    return false;
}

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

void RasterizerSgxd::DrawTriangles() {
    // Reached at the end of every CPU-path draw, even one with no triangle.
    const auto& pipeline = regs.pipeline;
    UNREACHABLE_MSG("sgxd: DrawTriangles, a draw on the CPU vertex path: geometry shader {}, "
                    "topology {}, {} vertices",
                    pipeline.use_gs == Pica::PipelineRegs::UseGS::Yes ? "on" : "off",
                    TopologyName(pipeline.triangle_topology), pipeline.num_vertices);
}

// The passes store into guest memory itself: there is nothing to flush or invalidate.

void RasterizerSgxd::FlushAll() {}

void RasterizerSgxd::FlushRegion(PAddr, u32) {}

void RasterizerSgxd::InvalidateRegion(PAddr, u32) {}

void RasterizerSgxd::FlushAndInvalidateRegion(PAddr, u32) {}

void RasterizerSgxd::ClearAll(bool) {}

namespace {
/// A transfer's pixel format as the texture format of the same layout (PixelFormat swaps RGB565
/// and RGB5A1 against TextureFormat).
Pica::TexturingRegs::TextureFormat TextureFormatOf(Pica::PixelFormat format) {
    using TF = Pica::TexturingRegs::TextureFormat;
    switch (format) {
    case Pica::PixelFormat::RGBA8:
        return TF::RGBA8;
    case Pica::PixelFormat::RGB8:
        return TF::RGB8;
    case Pica::PixelFormat::RGB565:
        return TF::RGB565;
    case Pica::PixelFormat::RGB5A1:
        return TF::RGB5A1;
    case Pica::PixelFormat::RGBA4:
        return TF::RGBA4;
    }
    UNREACHABLE_MSG("sgxd: pixel format {} is not a PICA transfer format", static_cast<u32>(format));
}
} // namespace

bool RasterizerSgxd::AccelerateDisplayTransfer(const Pica::DisplayTransferConfig& config) {
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
    // Flipped and cropped, the PICA places the output's last row as if its rows were input_width
    // apart: the output starts that much later, and games aim before their buffer for it.
    const bool skewed = config.flip_vertically && config.crop_input_lines &&
                        config.input_width > config.output_width;
    const PAddr output =
        config.GetPhysicalOutputAddress() +
        (skewed ? (config.input_width - config.output_width) * (config.output_height - 1) *
                      Pica::BytesPerPixel(config.output_format)
                : 0);
    // ScaleX halves the output's width, ScaleXY its height too (each output pixel the average of 2
    // or 2 x 2); output_width/height count the input's pixels.
    const u32 scale_x = config.scaling != config.NoScale ? 2 : 1;
    const u32 scale_y = config.scaling == config.ScaleXY ? 2 : 1;
    const u32 width = config.output_width / scale_x, height = config.output_height / scale_y;
    EndDoubleRender(width, height);
    const auto describe = [&] {
        return fmt::format("{}, format {} to {}, flags {:#x}", config.DebugName(),
                           static_cast<u32>(config.input_format.Value()),
                           static_cast<u32>(config.output_format.Value()), config.flags);
    };
#ifdef VITA_DIAGNOSTICS
    // sgxd_flags xferlog: every display transfer, texture copy and fill as it comes.
    static const bool xfer_log = SgxdFlag("xferlog");
    if (xfer_log) {
        LOG_INFO(Render, "xferlog: display transfer {}", describe());
    }
#endif
    // An address in no guest memory drops the transfer (a virtual address GSP could not convert
    // comes through as it is).
    if (!memory.IsValidPhysicalAddress(input) || !memory.IsValidPhysicalAddress(output)) {
        LOG_ERROR(Render, "sgxd: AccelerateDisplayTransfer {}: dropped, not guest memory",
                  describe());
        return true;
    }
    EndDoubleRender(input, width | height << 16);
    u32 in_width = config.input_width, in_height = config.input_height;
    // A tiled picture into a tiled one (dont_swizzle on a tiled input): a pass on the output as a PICA
    // colour buffer.
    const bool tiled_out = !config.input_linear && config.dont_swizzle;
    // The game's supersampling (Supersampled): an averaging transfer of a colour buffer to the
    // screen marks the buffer, whose passes draw at 1x from then on; once they have, the
    // transfer copies their picture, rows in_width / scale_x apart, as it is.
    u32 read_x = scale_x, read_y = scale_y;
    const u32 in_bits = UntileFormatFor(TextureFormatOf(config.input_format)).bits;
    static const bool keep_supersample = GxmFlag("keepaa");
    if ((scale_x != 1 || scale_y != 1) && !tiled_out && !keep_supersample) {
        const auto s = std::find_if(supersampled.begin(), supersampled.end(),
                                    [&](const Supersampled& e) { return e.colour == input; });
        if (s == supersampled.end()) {
            supersampled.push_back({input, in_width, in_height, scale_x, scale_y, width * scale_x,
                                    height * scale_y, in_width * in_height * in_bits / 8, 0, 0,
                                    static_cast<FramebufferRegs::ColorFormat>(
                                        static_cast<u32>(config.input_format.Value())),
                                    in_bits / 8});
            LOG_INFO(Render, "supersampling: {}x{} at {:#x} averaged {}x{}: drawn at 1x from now",
                     in_width, in_height, input, scale_x, scale_y);
        } else if (s->drawn_1x) {
            DEBUG_ASSERT_MSG(s->width == in_width && s->height == in_height &&
                                 s->scale_x == scale_x && s->scale_y == scale_y,
                             "GXM: AccelerateDisplayTransfer {}: the supersampled buffer was "
                             "{}x{} averaged {}x{}",
                             describe(), s->width, s->height, s->scale_x, s->scale_y);
            in_width /= scale_x;
            in_height /= scale_y;
            read_x = read_y = 1;
            s->live = false;
        }
    } else {
        DEBUG_ASSERT_MSG(!MeetsSupersampled1x(input, input + in_width * in_height * in_bits / 8),
                         "GXM: AccelerateDisplayTransfer {}: a transfer of a supersampled buffer "
                         "drawn at 1x before its averaging one is not built",
                         describe());
    }
    DEBUG_ASSERT_MSG(!config.input_linear && !config.flip_vertically &&
                         config.scaling <= config.ScaleXY && in_width >= width * read_x &&
                         in_height >= height * read_y,
                     "GXM: AccelerateDisplayTransfer {}: only a tiled input without flip is "
                     "built",
                     describe());
    // The output format as the PBE's pack mode (docs/sgxd/pbe.md), from an A8B8G8R8 picture. The
    // PBE truncates, as the PICA's conversions do.
    struct Out {
        u32 pack;
        bool mirror;
        u32 bytes;
    };
    Out out_format{};
    switch (config.output_format) {
    case PixelFormat::RGBA8:
        out_format = {SGXD_RECIPE_PACK_U8888_RGBA, false, 4};
        break;
    case PixelFormat::RGB8: // B, G, R in memory: gxm's U8U8U8_RGB store
        out_format = {SGXD_RECIPE_PACK_U888_RGB, false, 3};
        break;
    case PixelFormat::RGB565:
        out_format = {SGXD_RECIPE_PACK_R5G6B5, false, 2};
        break;
    case PixelFormat::RGB5A1:
        out_format = {SGXD_RECIPE_PACK_A1R5G5B5_RGBA, false, 2};
        break;
    case PixelFormat::RGBA4:
        out_format = {SGXD_RECIPE_PACK_A4R4G4B4, true, 2};
        break;
    default:
        UNREACHABLE_MSG("sgxd: AccelerateDisplayTransfer {}: that output format is not built",
                        describe());
    }
    const u32 bytes = width * height * out_format.bytes;
    const bool fcram = output >= Memory::FCRAM_PADDR && output + bytes <= Memory::FCRAM_PADDR_END;
    DEBUG_ASSERT_MSG(
        fcram || (output >= Memory::VRAM_PADDR && output + bytes <= Memory::VRAM_PADDR_END),
        "sgxd: AccelerateDisplayTransfer {}: the output is outside the mapped guest memory",
        describe());
    // From guest memory: a pass over it renders first, and the transfer's render reads what its 3D
    // stored (the GPU's renders run in order).
    const UntileFormat f = UntileFormatFor(TextureFormatOf(config.input_format));
    const u32 in_bytes = in_width * in_height * f.bits / 8;
    if (backend.InPass() && PassOverlaps(input, input + in_bytes)) {
        EndPass("a display transfer from its colour buffer");
    }
    if (backend.InPass() && PassOverlaps(output, output + bytes)) {
        EndPass("a display transfer over its buffers");
    }
    DEBUG_ASSERT_MSG(f.code != 0xff && f.code != SgxdBackend::UntileEtc1 &&
                         f.code != SgxdBackend::UntileEtc1A4 && in_width % 8 == 0 &&
                         in_width <= SgxdBackend::UntileSide && in_height % 8 == 0 &&
                         in_height <= SgxdBackend::UntileSide,
                     "sgxd: AccelerateDisplayTransfer {}: a {}x{} picture: the untile takes sides "
                     "of whole tiles",
                     describe(), in_width, in_height);

    // The output: guest memory mapped for the GPU at its PICA address (VRAM, and FCRAM moved to
    // CDRAM before the op), uncached.
    u8* const out = memory.GetPhysicalPointer(output);
    ASSERT_MSG(!fcram || guest_memory.IsUncached(output, bytes),
               "sgxd: AccelerateDisplayTransfer {}: the output {:#x}-{:#x} is FCRAM the CPU "
               "caches; moving it to CDRAM before the transfer is not built",
               describe(), output, output + bytes);
    u32 ta_us = 0, d3_us = 0;
    if (!guest_memory.IsUncached(input, in_bytes)) {
        SgxdCacheRange(memory.GetPhysicalPointer(input), in_bytes, SGXD_CACHE_CLEAN,
                       CacheSite::Textures);
    }
    if (tiled_out || linear_in) {
        if (backend.InPass()) {
            EndPass("a tiled display transfer");
        }
        const ColourBuffer target{output, width, height,
                                  static_cast<FramebufferRegs::ColorFormat>(
                                      static_cast<u32>(config.output_format.Value())),
                                  out_format.bytes};
        const SgxdBackend::UntileSource source{input,  in_width, in_height, f.bits,
                                               f.code, f.swizzle, 0};
        if (linear_in) {
            backend.TransferLinearToTiled(PicaTargetFor(target, "a linear display transfer"),
                                          source, config.flip_vertically != 0, &ta_us, &d3_us);
        } else {
            backend.TransferTiledFromGuest(PicaTargetFor(target, "a tiled display transfer"),
                                           source, scale_x, scale_y, &ta_us, &d3_us);
        }
    } else {
        backend.TransferFromGuest({input, in_width, in_height, f.bits, f.code, f.swizzle, 0},
                                  output, width, height, out_format.pack, out_format.mirror, &ta_us,
                                  &d3_us, read_x, read_y);
    }
#ifdef VITA_DIAGNOSTICS
    PerflogTransfer(input, in_width, in_height, output, width, height, read_x, read_y, ta_us,
                    d3_us);
#endif
    TexturesWritten(output, output + bytes);
    DepthStoresWritten(output, output + bytes);
    // A whole picture of the top screen's size or more (a game: 256x416, the LCD 16 rows into
    // it): its source is the upscaled top screen.
    if (!tiled_out && !linear_in && scale_x == 1 && scale_y == 1 && width >= 240 &&
        height >= 400) {
        NoteTopTransfer(input, output, width, height, out_format.bytes,
                        config.flip_vertically != 0);
    }
    // The blit's 3D stays pending, as a pass's does: the next list finishes it. Whatever waits on the
    // blit (a page hold on its output, a ticket) finishes it then (InterruptPoll, RenderWait).
#ifdef VITA_DIAGNOSTICS
    if (transfers > 0 && transfers % 30 == 0 && SgxdFlag("perflog")) {
        const SgxdEvents& ev = backend.Events();
        LOG_INFO(Render,
                 "transfer {}: TA {} us, 3D {} us; PTLA ops done {}; renders {}, async {}; IRQ 52 "
                 "{} (SW_EVENT {}, unexpected {:#x})",
                 transfers + 1, ta_us, d3_us, sgxd_ptla_done(), sgxd_session_renders(),
                 sgxd_chain_done(), ev.irqs, ev.sw_events, ev.unexpected);
    }
    transfers++;
    VprofTransferResult(output, width, height, static_cast<u32>(config.output_format.Value()),
                        out, bytes);
#endif
    return true;
}

// P3D lets the guest overwrite the vertex and index buffers the open pass reads at its TA: the
// pass renders now. The page holds keep the guest off the buffers until the work is done.
void RasterizerSgxd::NotifyP3D() {
    FlushImmediate();
    const bool in_pass = backend.InPass();
    if (in_pass) {
        EndPass("P3D");
    }
    // A clear quad filled before it was alone in what would have been its pass.
    filled_site = -1;
    if (p3ds++ < 4 || p3ds % 60 == 0) {
        LOG_INFO(Render, "P3D {}: {}; {} passes so far, {} opened by a covering draw", p3ds,
                 in_pass ? "ended the open pass" : "no pass open", passes, covered_passes);
    }
}

// The last pass's 3D gets its finishing list here when something waits on it; otherwise it goes
// with the next pass's list. An open pass stays open.
void RasterizerSgxd::GpuIdle() {
    backend.RenderFinish();
}


u64 RasterizerSgxd::InterruptFence() {
    const SgxdBackend::InterruptFence f = backend.InterruptFenceNow();
    return u64{f.render} << 32 | f.ptla;
}

bool RasterizerSgxd::InterruptPoll(u64 fence) {
    return backend.InterruptPoll({static_cast<u32>(fence >> 32), static_cast<u32>(fence)});
}

bool RasterizerSgxd::InterruptReached(u64 fence) {
    return backend.InterruptReached({static_cast<u32>(fence >> 32), static_cast<u32>(fence)});
}

void RasterizerSgxd::InterruptKick(u64 fence) {
    backend.InterruptKick({static_cast<u32>(fence >> 32), static_cast<u32>(fence)});
}

bool RasterizerSgxd::InterruptArm(u64 fence) {
    return backend.InterruptArm({static_cast<u32>(fence >> 32), static_cast<u32>(fence)});
}

void RasterizerSgxd::InterruptDisarm() {
    backend.InterruptDisarm();
}

void RasterizerSgxd::InterruptWaker(s32 flag, u32 bits) {
    backend.InterruptWaker(flag, bits);
}

void RasterizerSgxd::InterruptWait(u64 fence) {
    backend.InterruptWait({static_cast<u32>(fence >> 32), static_cast<u32>(fence)});
}

#ifdef VITA_DIAGNOSTICS
u64 RasterizerSgxd::GpuWaitUs() const {
    return sgxd_wait_us_total();
}
#endif

bool RasterizerSgxd::AccelerateTextureCopy(const Pica::DisplayTransferConfig& config) {
    // Lines of input_width bytes input_gap apart into lines of output_width output_gap apart (16-byte
    // units); alike on both sides, or with no gaps, one contiguous copy.
    const auto& tc = config.texture_copy;
    u32 in_width = tc.input_width * 16, out_width = tc.output_width * 16;
    const u32 in_gap = tc.input_gap * 16, out_gap = tc.output_gap * 16;
    const PAddr src = config.GetPhysicalInputAddress(), dst = config.GetPhysicalOutputAddress();
#ifdef VITA_DIAGNOSTICS
    static const bool xfer_log = SgxdFlag("xferlog");
    if (xfer_log) {
        LOG_INFO(Render, "xferlog: texture copy {:#x} to {:#x}, {} bytes, input {}+{}, output {}+{}",
                 src, dst, tc.size, in_width, in_gap, out_width, out_gap);
    }
#endif
    // An address in no guest memory drops the copy.
    if (!memory.IsValidPhysicalAddress(src) || !memory.IsValidPhysicalAddress(dst)) {
        LOG_ERROR(Render,
                  "sgxd: AccelerateTextureCopy from {:#x} to {:#x}, {} bytes: dropped, not "
                  "guest memory",
                  src, dst, tc.size);
        return true;
    }
    if (in_gap == 0 && out_gap == 0) {
        return AccelerateDma(src, dst, tc.size);
    }
    // A side with no gap is one stream whatever its width.
    if (in_gap == 0) {
        in_width = out_width;
    } else if (out_gap == 0) {
        out_width = in_width;
    }
    DEBUG_ASSERT_MSG(in_width == out_width && in_width != 0 && tc.size % in_width == 0,
                     "sgxd: AccelerateTextureCopy from {:#x} to {:#x}, {} bytes, input width {} "
                     "gap {}, output width {} gap {}: lines of two widths are not built",
                     src, dst, tc.size, in_width, in_gap, out_width, out_gap);
    return CopyLines(src, in_width + in_gap, dst, out_width + out_gap, in_width,
                     tc.size / in_width);
}

bool RasterizerSgxd::AccelerateFill(const Pica::MemoryFillConfig& config) {
    FlushImmediate();
    const PAddr start = config.GetStartAddress();
    const PAddr end = config.GetEndAddress();
    // An address in no guest memory drops the fill.
    if (!memory.IsValidPhysicalAddress(start)) {
        LOG_ERROR(Render, "sgxd: AccelerateFill {:#x}-{:#x}: dropped, not guest memory", start, end);
        return true;
    }
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
    if (backend.InPass() && PassOverlaps(start, end)) {
        EndPass("a fill over its buffers");
    }
    DepthStoresWritten(start, end);

    // The range as rows of 4096 pixels (under the PTLA's 64 KiB stride) and a last partial row, filled
    // in place at its PICA address, which is its GPU address.
    constexpr u32 RowPixels = 4096;
    const u32 pixels = (end - start) / bytes;
    const u32 format = bytes == 4 ? SGXD_PTLA_8888 : (bytes == 3 ? SGXD_PTLA_888 : SGXD_PTLA_U88);
    // The pending 3D completes before the PTLA writes over it.
    backend.RenderBefore(start, end, "a fill");
    const SgxdPtlaFill fill{start,           format,             RowPixels * bytes, RowPixels,
                            pixels / RowPixels, pixels % RowPixels, value};
    char failed[160];
    u32 elapsed_us = 0;
    [[maybe_unused]] const int filled = sgxd_ptla_fill(&fill, &elapsed_us, failed, sizeof(failed));
#ifdef VITA_DIAGNOSTICS
    if (filled != 0) {
        // A stopped list before the PTLA op: its post-mortem and the GPU dump.
        LOG_CRITICAL(Render, "sgxd: AccelerateFill {}: {}", describe(), failed);
        backend.RenderIdle("a failed fill");
    }
#endif
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

void RasterizerSgxd::RemapPages(const u32* pages, const u32* pte_va, const u32* pte, u32 count,
                               bool direct) {
    // Page versions: the GPU's MMU moves onto the slots the guest wrote, in the GPU's order. What reads
    // the pages before is sent first: the queued untiles, and the open pass when queued work read a
    // page other than as a texture (direct) or a texture of the pass is untiled in place again. The
    // pending 3D finishes, so no unit runs while the remap invalidates the MMU's caches.
    const auto pass_texture_in_place = [&] {
        for (const Texture& t : textures) {
            if (!t.in_place || t.last_used != passes) {
                continue;
            }
            for (u32 k = 0; k < count; k++) {
                for (u32 part = 0; part < t.parts; part++) {
                    if (pages[k] < t.part_addr[part] + t.part_bytes &&
                        t.part_addr[part] < pages[k] + 0x1000) {
                        return true;
                    }
                }
            }
        }
        return false;
    };
    if (backend.InPass() && (direct || pass_texture_in_place())) {
        EndPass("a page version's remap");
    }
    for (u32 k = 0; k < count; k++) {
        ForgetClears(pages[k], pages[k] + 0x1000);
        TexturesWritten(pages[k], pages[k] + 0x1000);
        DepthStoresWritten(pages[k], pages[k] + 0x1000);
    }
    backend.RemapPages(pte_va, pte, count);
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
    // The PTLA copies 8888 words exactly; a size that is no whole number of words has its last 1-3
    // bytes copied by the CPU. A copy from or to an address no word starts at goes as U8 pixels.
    const bool bytes = !strided && size > 0 && (src % 4 != 0 || dst % 4 != 0);
    ASSERT_MSG(bytes || (src % 4 == 0 && dst % 4 == 0 && size > 0 &&
                         (!strided || (line % 4 == 0 && src_stride % 4 == 0 &&
                                       dst_stride % 4 == 0 && line / 4 <= 4096))),
               "sgxd: AccelerateDma {}: only word-aligned lines are built", describe());
    const u32 tail = bytes ? 0 : size % 4;
    // A range past guest FCRAM or VRAM reaches whatever the GPU has mapped after it.
    ASSERT_MSG((src_fcram || inside(src, size, Memory::VRAM_PADDR, Memory::VRAM_SIZE)) &&
                   (dst_fcram || inside(dst, dst_size, Memory::VRAM_PADDR, Memory::VRAM_SIZE)),
               "sgxd: AccelerateDma {}: outside the mapped guest memory", describe());

    // A copy over the open pass's buffers, or from its colour buffer, comes after its draws.
    const PAddr end = dst + dst_size;
    if (backend.InPass() && PassOverlaps(dst, end)) {
        EndPass("a DMA over its buffers");
    }
    if (backend.InPass() && src < pass_colour.End() && pass_colour.addr < src + size) {
        EndPass("a DMA from its colour buffer");
    }
    // A supersampled buffer's live 1x picture is read in the game's layout (SupersampledSource).
    const PAddr read = SupersampledSource(src, size);
    DEBUG_ASSERT_MSG(read == src || size % 4 == 0,
                     "GXM: AccelerateDma {}: a supersampled buffer's copy with a byte tail",
                     describe());
    // The pending 3D completes before the PTLA reads what it writes, or writes what it reads.
    backend.RenderBefore(src, src + size, "a DMA's source");
    if (read != src) {
        backend.RenderBefore(read, read + size, "a DMA's magnified source");
    }
    backend.RenderBefore(dst, end, "a DMA's destination");
    ForgetClears(dst, end);
    TexturesWritten(dst, end);
    DepthStoresWritten(dst, end);

    // Guest FCRAM is cached for the CPU (but the pages moved to CDRAM): the source is cleaned before
    // the PTLA reads it, the destination's lines leave the cache before and after.
    const bool src_cached = read == src && src_fcram && !guest_memory.IsUncached(src, size);
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
        strided
            ? SgxdPtlaCopy{read, dst, SGXD_PTLA_8888, src_stride, line / 4, lines, 0, dst_stride}
            : SgxdPtlaCopy{read,
                           dst,
                           SGXD_PTLA_8888,
                           RowPixels * 4,
                           RowPixels,
                           pixels / RowPixels,
                           pixels % RowPixels,
                           0};
    char failed[160];
    u32 elapsed_us = 0;
    [[maybe_unused]] const int copied =
        pixels == 0 ? 0 : sgxd_ptla_copy(&copy, &elapsed_us, failed, sizeof(failed));
#ifdef VITA_DIAGNOSTICS
    if (copied != 0) {
        // A stopped list before the PTLA op, or a flip that found the GPU busy: the post-mortem.
        LOG_CRITICAL(Render, "sgxd: AccelerateDma {}: {}", describe(), failed);
        backend.Stopped("a failed copy");
    }
#endif
    DEBUG_ASSERT_MSG(copied == 0, "sgxd: AccelerateDma {}: {}", describe(), failed);
    // The copy is queued. The destination's cache lines go once it has completed, and the PTLA's
    // uncopied last bytes then too: before the guest's interrupt, or the rasterizer's next read.
    if (dst_cached || tail != 0) {
        const u8* const src_cpu = memory.GetPhysicalPointer(src);
        backend.AfterGpu([dst_cpu, dst_size, dst_cached, src_cpu, size, tail] {
            if (dst_cached) {
                SgxdCacheRange(dst_cpu, dst_size, SGXD_CACHE_CLEAN_INVALIDATE, CacheSite::DmaDest);
            }
            if (tail != 0) {
                std::memcpy(dst_cpu + size - tail, src_cpu + size - tail, tail);
            }
        });
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

namespace {
/// A float24 register value halved: its exponent less one (16 mantissa bits, 7 exponent bits).
u32 HalveF24(u32 raw) {
    DEBUG_ASSERT_MSG((raw >> 16 & 0x7f) > 1, "GXM: halving the float24 {:#x}", raw);
    return raw - (1u << 16);
}
} // namespace

bool RasterizerGxm::DropSupersampling() {
    auto& fb = regs.framebuffer.framebuffer;
    const PAddr colour = fb.GetColorBufferPhysicalAddress();
    const auto s = std::find_if(supersampled.begin(), supersampled.end(),
                                [&](const Supersampled& e) { return e.colour == colour; });
    // A draw of the averaged picture: its viewport the averaged region's size.
    const auto& vp = regs.rasterizer;
    const float vp_width = f24::FromRaw(vp.viewport_size_x).ToFloat32() * 2;
    const float vp_height = f24::FromRaw(vp.viewport_size_y).ToFloat32() * 2;
    const bool picture = s != supersampled.end() &&
                         vp_width == static_cast<float>(s->region_width) &&
                         vp_height == static_cast<float>(s->region_height);
    DEBUG_ASSERT_MSG(s == supersampled.end() || picture || !s->live,
                     "GXM: a draw with a {}x{} viewport to the supersampled colour buffer at "
                     "{:#x} while its 1x picture ({}x{}) waits for its transfer is not built",
                     vp_width, vp_height, colour, s->region_width, s->region_height);
    const bool drop = picture;
    if (drop != supersample_dropped) {
        // The per-draw caches keyed by these words (StateRegs::Fragment) saw the other values.
        for (const u32 r : {PICA_REG_INDEX(rasterizer.scissor_test.mode),
                            PICA_REG_INDEX(rasterizer.scissor_test.x1),
                            PICA_REG_INDEX(rasterizer.scissor_test.x2),
                            PICA_REG_INDEX(rasterizer.viewport_size_x),
                            PICA_REG_INDEX(rasterizer.viewport_size_y),
                            PICA_REG_INDEX(rasterizer.viewport_corner),
                            PICA_REG_INDEX(framebuffer.framebuffer.width)}) {
            pica.dirty_regs.Set(r);
        }
        supersample_dropped = drop;
    }
    if (!drop) {
        return false;
    }
    DEBUG_ASSERT_MSG(fb.GetWidth() == s->width && fb.GetHeight() == s->height &&
                         s->width % (8 * s->scale_x) == 0 && s->height % (8 * s->scale_y) == 0,
                     "GXM: the supersampled colour buffer at {:#x} ({}x{}, averaged {}x{}) drawn "
                     "as {}x{}",
                     colour, s->width, s->height, s->scale_x, s->scale_y, fb.GetWidth(),
                     fb.GetHeight());
    auto& rs = regs.rasterizer;
    std::memcpy(&supersample_saved_fb, &fb, sizeof(fb));
    std::memcpy(&supersample_saved_rs, &rs, sizeof(rs));
    fb.width.Assign(s->width / s->scale_x);
    fb.height.Assign(s->height / s->scale_y - 1);
    // The scissor's [first, last + 1) halved outward.
    auto& sc = rs.scissor_test;
    if (s->scale_x == 2) {
        rs.viewport_size_x.Assign(HalveF24(rs.viewport_size_x));
        rs.viewport_corner.x.Assign(rs.viewport_corner.x >> 1);
        sc.x1.Assign(sc.x1 / 2);
        sc.x2.Assign((sc.x2 + 2) / 2 - 1);
    }
    if (s->scale_y == 2) {
        rs.viewport_size_y.Assign(HalveF24(rs.viewport_size_y));
        rs.viewport_corner.y.Assign(rs.viewport_corner.y >> 1);
        sc.y1.Assign(sc.y1 / 2);
        sc.y2.Assign((sc.y2 + 2) / 2 - 1);
    }
    s->live = true;
    s->full_current = false;
    if (!s->drawn_1x) {
        s->drawn_1x = true;
        s->depth = fb.GetDepthBufferPhysicalAddress();
        s->depth_bytes =
            s->width * s->height * FramebufferRegs::BytesPerDepthPixel(fb.depth_format);
        LOG_INFO(Render, "supersampling: the colour buffer at {:#x} drawn {}x{} for {}x{}", colour,
                 fb.GetWidth(), fb.GetHeight(), s->width, s->height);
    }
    return true;
}

void RasterizerGxm::RestoreSupersampling() {
    std::memcpy(&regs.framebuffer.framebuffer, &supersample_saved_fb, sizeof(supersample_saved_fb));
    std::memcpy(&regs.rasterizer, &supersample_saved_rs, sizeof(supersample_saved_rs));
}

PAddr RasterizerGxm::SupersampledSource(PAddr src, u32 size) {
    for (Supersampled& s : supersampled) {
        if (!s.live || src + size <= s.colour || s.colour + s.bytes <= src) {
            continue;
        }
        DEBUG_ASSERT_MSG(src >= s.colour && src + size <= s.colour + s.bytes,
                         "GXM: a copy of {:#x}+{:#x} reaches past the supersampled buffer at "
                         "{:#x}+{:#x}",
                         src, size, s.colour, s.bytes);
        if (!s.full_current) {
            // a game copies its top screen whole in its menus (a 480x400 TextureCopy).
            if (s.full.gpu == 0) {
                s.full = backend.AllocSurface(s.bytes / 4, 1);
            }
            if (backend.InPass()) {
                EndPass("a supersampled buffer's picture magnified");
            }
            const UntileFormat f = UntileFormatFor(
                static_cast<Pica::TexturingRegs::TextureFormat>(static_cast<u32>(s.format)));
            backend.MagnifyTiled(
                PicaTargetFor({s.full.gpu, s.width, s.height, s.format, s.bpp},
                              "a supersampled buffer's picture magnified"),
                {s.colour, s.width / s.scale_x, s.height / s.scale_y, f.bits, f.code, f.swizzle, 0},
                s.scale_x, s.scale_y);
            s.full_current = true;
        }
        return s.full.gpu + (src - s.colour);
    }
    return src;
}

bool RasterizerGxm::MeetsSupersampled1x(PAddr lo, PAddr hi) const {
    return std::any_of(supersampled.begin(), supersampled.end(), [&](const Supersampled& s) {
        return s.live && ((lo < s.colour + s.bytes && s.colour < hi) ||
                          (s.depth != 0 && lo < s.depth + s.depth_bytes && s.depth < hi));
    });
}

bool RasterizerGxm::SkipsDoubleRender(u32 count) {
    static const bool keep = GxmFlag("keepaa");
    if (keep) {
        return false;
    }
    const auto& fb = regs.framebuffer.framebuffer;
    const auto& om = regs.framebuffer.output_merger;
    const PAddr colour = fb.GetColorBufferPhysicalAddress();
    const auto unit0 = regs.texturing.GetTextures()[0];
    const PAddr tex0 = unit0.enabled ? unit0.config.GetPhysicalAddress() : 0;
    const bool quad = count == 4 || count == 6;
    // The blend back: the copy over the scene at an alpha (MH Generations: SourceAlpha,
    // OneMinusSourceAlpha, the TEV's constant alpha 0x7f).
    using BF = FramebufferRegs::BlendFactor;
    const auto& b = om.alpha_blending;
    const bool alpha_blend =
        om.alphablend_enable &&
        (b.factor_source_rgb == BF::SourceAlpha || b.factor_source_rgb == BF::ConstantAlpha) &&
        (b.factor_dest_rgb == BF::OneMinusSourceAlpha ||
         b.factor_dest_rgb == BF::OneMinusConstantAlpha);
    const auto& rs = regs.rasterizer;
    const u32 vp_width = static_cast<u32>(f24::FromRaw(rs.viewport_size_x).ToFloat32() * 2);
    const u32 vp_height = static_cast<u32>(f24::FromRaw(rs.viewport_size_y).ToFloat32() * 2);
    // The scene's draws: the bottom screen's (MH Generations draws it into the same buffer,
    // 240x320) go on.
    if (skipping_scene != 0 && colour == skipping_scene && vp_width == skipping_width &&
        vp_height == skipping_height) {
        if (quad && tex0 == skipping_copy && alpha_blend) {
            skipping_scene = 0; // the blend: drawn
        } else {
            double_render_draws++;
            return true;
        }
    }
    if (colour != prev_draw_colour) {
        prev_other_colour = std::exchange(prev_draw_colour, colour);
    }
    // MH Generations draws four times into the copy's buffer between its scene and the copy. The
    // copy writes the texel as it is: no blend, or ONE, ZERO, ADD (MH Generations' blending is
    // on for every draw).
    const bool copies =
        !om.alphablend_enable || (b.blend_equation_rgb == FramebufferRegs::BlendEquation::Add &&
                                  b.blend_equation_a == FramebufferRegs::BlendEquation::Add &&
                                  b.factor_source_rgb == BF::One && b.factor_dest_rgb == BF::Zero &&
                                  b.factor_source_a == BF::One && b.factor_dest_a == BF::Zero);
    if (quad && tex0 != 0 && tex0 == prev_other_colour && colour != tex0 && copies) {
        // A copy of the last buffer drawn before this one: the first render's, when its pair is
        // known.
        copy_scene = tex0;
        copy_target = colour;
        drawn_after_copy = false;
        const auto d =
            std::find_if(double_renders.begin(), double_renders.end(), [&](const DoubleRender& r) {
                return r.scene == tex0 && r.copy == colour;
            });
        if (d != double_renders.end()) {
            skipping_scene = tex0;
            skipping_copy = colour;
            skipping_width = d->width;
            skipping_height = d->height;
        }
    } else if (colour == copy_scene && copy_scene != 0) {
        if (quad && tex0 == copy_target && alpha_blend && drawn_after_copy) {
            if (std::none_of(double_renders.begin(), double_renders.end(),
                             [&](const DoubleRender& d) {
                                 return d.scene == copy_scene && d.copy == copy_target;
                             })) {
                double_renders.push_back({copy_scene, copy_target, vp_width, vp_height});
                LOG_INFO(Render,
                         "double render: {:#x} drawn twice, the first copied to {:#x} and blended "
                         "back: the second skipped from now",
                         copy_scene, copy_target);
            }
            copy_scene = 0;
        } else {
            drawn_after_copy = true;
        }
    }
    return false;
}

void RasterizerGxm::LateDropped(PAddr colour, u32 width, u32 height) {
    if (skipping_scene == 0 ||
        !((colour == skipping_scene && width >= skipping_width && height >= skipping_height) ||
          colour == skipping_copy)) {
        return;
    }
    LOG_DEBUG(Render, "double render: a draw into {:#x} dropped late: the skip of {:#x} ends",
              colour, skipping_scene);
    skipping_scene = 0;
}

void RasterizerGxm::EndDoubleRender(u32 width, u32 height) {
    if (skipping_scene == 0 || width != skipping_width || height != skipping_height) {
        return;
    }
    LOG_WARNING(Render,
                "double render: no blend of {:#x} back over {:#x} before the display transfer: "
                "the pair dropped",
                skipping_copy, skipping_scene);
    std::erase_if(double_renders, [&](const DoubleRender& d) {
        return d.scene == skipping_scene && d.copy == skipping_copy;
    });
    skipping_scene = 0;
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

namespace {

bool InFcram(PAddr addr) {
    return addr >= Memory::FCRAM_PADDR && addr < Memory::FCRAM_PADDR_END;
}

} // namespace

const RasterizerSgxd::NarrowScratch& RasterizerSgxd::NarrowScratchFor(const ColourBuffer& c) {
    for (const NarrowScratch& n : narrow_scratch) {
        if (n.addr == c.addr && n.width == c.width && n.height == c.height && n.bpp == c.bpp) {
            return n;
        }
    }
    // 32-bit samples hold the widest format, and 32 rows at least the whole tile a store may write.
    const u32 wide = NarrowWidth(c);
    return narrow_scratch.emplace_back(NarrowScratch{
        c.addr, c.width, c.height, c.bpp, wide,
        backend.AllocSurface(wide, std::max(c.height, 32u))});
}

void RasterizerSgxd::CopyLines(u32 src, u32 dst, u32 line_bytes, u32 lines, u32 src_stride,
                              u32 dst_stride, [[maybe_unused]] const char* what) {
    DEBUG_ASSERT(line_bytes % 4 == 0 && src_stride % 4 == 0 && dst_stride % 4 == 0);
    const SgxdPtlaCopy copy{src,   dst, SGXD_PTLA_8888, src_stride, line_bytes / 4,
                            lines, 0,   dst_stride};
    char failed[160];
    u32 elapsed_us = 0;
    [[maybe_unused]] const int copied = sgxd_ptla_copy(&copy, &elapsed_us, failed, sizeof(failed));
    DEBUG_ASSERT_MSG(copied == 0, "sgxd: {}: {}", what, failed);
}

SgxdBackend::PicaTarget RasterizerSgxd::PicaTargetFor(const ColourBuffer& c, const char* what) {
    // The PBE's pack for each colour format: the
    // PICA's bytes from the A8B8G8R8 a pass's fragments leave on chip.
    using CF = FramebufferRegs::ColorFormat;
    u32 pack = 0;
    switch (c.format) {
    case CF::RGBA8:
        pack = SGXD_RECIPE_PACK_U8888_RGBA;
        break;
    case CF::RGB8: // B, G, R in memory: U8U8U8_RGB
        pack = SGXD_RECIPE_PACK_U888_RGB;
        break;
    case CF::RGB565:
        pack = SGXD_RECIPE_PACK_R5G6B5;
        break;
    case CF::RGB5A1:
        pack = SGXD_RECIPE_PACK_A1R5G5B5_RGBA;
        break;
    case CF::RGBA4:
        pack = SGXD_RECIPE_PACK_A4R4G4B4_RGBA;
        break;
    default:
        UNREACHABLE_MSG("sgxd: {}: the colour buffer at {:#x} has format {}, which is not built",
                        what, c.addr, static_cast<u32>(c.format));
    }
    // Route 1 draws the buffer transposed in 8x16 strips: its width in 16s, its height in 8s.
    ASSERT_MSG(c.width % 16 == 0 && c.width >= 32 && c.width <= 1024 && c.height % 8 == 0 &&
                   c.height >= 32 && c.height <= 1024,
               "sgxd: {}: a {}x{} colour buffer at {:#x}: passes take widths of 32 to 1024 in 16s "
               "and heights of 32 to 1024 in 8s",
               what, c.width, c.height, c.addr);
    // FCRAM is drawn into once it is uncached (moved under the draw's or transfer's write hold).
    if (InFcram(c.addr)) {
        KeepUncached(c.addr, c.End() - c.addr);
    }
    ASSERT_MSG(!InFcram(c.addr) || guest_memory.IsUncached(c.addr, c.End() - c.addr),
               "sgxd: {}: the colour buffer at {:#x}-{:#x} is FCRAM the CPU caches", what, c.addr,
               c.End());
    return {c.addr, c.width, c.height, pack, c.bpp};
}

bool RasterizerSgxd::PassOverlaps(PAddr start, PAddr end) const {
    const PAddr depth_end = pass_depth + pass_colour.width * pass_colour.height * pass_depth_bytes;
    return (start < pass_colour.End() && pass_colour.addr < end) ||
           ((pass_depth_cleared || pass_depth_used) && start < depth_end && pass_depth < end);
}

namespace {

/// A fill's value as the A8B8G8R8 word of the colour it leaves in a buffer of `format` (5 and 6
/// bits widened by repeating their top bits). A wider fill clears only when it repeats per pixel.
u32 ClearColour(FramebufferRegs::ColorFormat format, u32 value, u32 fill_bytes) {
    using F = FramebufferRegs::ColorFormat;
    const u32 bpp = FramebufferRegs::BytesPerColorPixel(format);
    DEBUG_ASSERT_MSG(fill_bytes == bpp || (fill_bytes == 4 && bpp == 2 &&
                                            (value & 0xffff) == value >> 16),
                     "sgxd: a {}-byte fill of {:#x} over a colour buffer of format {}", fill_bytes,
                     value, static_cast<u32>(format));
    const auto x5 = [](u32 c) { return c << 3 | c >> 2; };
    const auto x6 = [](u32 c) { return c << 2 | c >> 4; };
    u32 r = 0, g = 0, b = 0, a = 255;
    switch (format) {
    case F::RGBA8: // the word 0xRRGGBBAA
        r = value >> 24, g = value >> 16 & 0xff, b = value >> 8 & 0xff, a = value & 0xff;
        break;
    case F::RGB8: // bytes B, G, R from the low one
        b = value & 0xff, g = value >> 8 & 0xff, r = value >> 16 & 0xff;
        break;
    case F::RGB5A1:
        r = x5(value >> 11 & 31), g = x5(value >> 6 & 31), b = x5(value >> 1 & 31);
        a = (value & 1) * 255;
        break;
    case F::RGB565:
        r = x5(value >> 11 & 31), g = x6(value >> 5 & 63), b = x5(value & 31);
        break;
    case F::RGBA4:
        r = (value >> 12 & 15) * 17, g = (value >> 8 & 15) * 17, b = (value >> 4 & 15) * 17;
        a = (value & 15) * 17;
        break;
    default:
        UNREACHABLE_MSG("sgxd: colour format {}", static_cast<u32>(format));
    }
    return a << 24 | b << 16 | g << 8 | r;
}

} // namespace

namespace {

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

#ifdef VITA_DIAGNOSTICS
RasterizerSgxd::StreamOutNote RasterizerSgxd::stream_note;
RasterizerSgxd* RasterizerSgxd::stream_self;

void RasterizerSgxd::NoteStreamOut(const char* stage, u32 mode, u32 invocations, u32 rows, u32 cap,
                                  u32 count, const VertexProgram& program) {
    stream_self = this;
    SgxdBackend::stream_hook = PrintStreamOut;
    stream_note = {stage,       passes, backend.PassDraws() + 1, mode,
                   invocations, rows,   cap,                     count,
                   program.primary_doutu0, program.usse.temp_count};
}

#endif

void RasterizerSgxd::KeepGsRows(u32 begin, u32 end) {
    for (u32 r = 0; r < 16; r++) {
        const u32 row = gs_last_rows[r] + 4;
        if ((gs_regs_loaded >> r & 1) == 0 || (gs_rows_kept >> r & 1) != 0 || row < begin ||
            row >= end) {
            continue;
        }
        // Allocated with the first row (OpenPass is no place for an eviction).
        DEBUG_ASSERT(gs_kept_rows.cpu != nullptr);
        std::memcpy(gs_kept_rows.cpu + r * 16, backend.SharedAt(row), 16);
        gs_last_rows[r] = gs_kept_rows.gpu + r * 16 - 4;
        gs_rows_kept |= static_cast<u16>(1u << r);
    }
}

const RasterizerSgxd::Texture& RasterizerSgxd::TextureFor(u32 unit) {
    const auto& at = TextureUnitWords[unit];
    const u32* const r = regs.reg_array.data();
    const std::array<u32, 5> words{r[at[0]], r[at[1]], r[at[2]], r[at[3]], r[at[4]]};
    const u32 hash = words[0] ^ words[1] ^ words[2] ^ words[3] ^ words[4];
    // A cube's faces are words of their own, and its type means a cube only on unit 0.
    const bool memoised = (words[1] >> 28 & 7) != Pica::TexturingRegs::TextureConfig::TextureCube;
#ifdef VITA_DIAGNOSTICS
    texture_stats.lookups++;
#endif
    bool written = false;
    for (u32 m = 0; memoised && m < texture_memos.size(); m++) {
        // The XOR picks the candidates; the words decide.
        const TextureMemo& memo = texture_memos[m];
        if (memo.hash != hash || memo.drops != texture_drops || memo.texture == nullptr ||
            memo.words != words) {
            continue;
        }
        Texture& t = *memo.texture;
        t.last_used = passes;
        DEBUG_ASSERT_MSG(!backend.InPass() || backend.PassDraws() == pass_open_draws ||
                             !UnitReadsDrawn(unit),
                         "sgxd: texture unit {} at {:#x} reads the colour buffer at {:#x} its own "
                         "pass drew",
                         unit, t.addr, pass_colour.addr);
        // Stale only when written over (GPU work, or the guest's and HLE's writes to FCRAM textures).
        written = TextureWritten(t);
        if (!written) {
#ifdef VITA_DIAGNOSTICS
            texture_stats.memo_hits++;
#endif
            return t;
        }
        break;
    }
    Texture& t = TextureFromRegs(unit, written);
    if (memoised) {
        texture_memos[unit] = {words, hash, &t, texture_drops};
    }
    return t;
}

/// A mipmapped cube's face stride, for a w x w U8888 face: its chain down to 1x1 whatever
/// MIPMAPCLAMP says, rounded up to 2 KiB (measured). Without mipmapping the faces touch.
u32 CubeFaceStride(u32 w) {
    u32 chain = 0;
    for (u32 side = w; side >= 1; side >>= 1) {
        chain += side * side * 4;
    }
    return Common::AlignUp(chain, 2048u);
}

RasterizerSgxd::TextureExtent RasterizerSgxd::TextureExtentFor(u32 unit) const {
    using TextureConfig = Pica::TexturingRegs::TextureConfig;
    const auto t = regs.texturing.GetTextures()[unit];
    const auto& c = t.config;
    const u32 width = c.width, height = c.height;
    TextureExtent e{};
    e.cube = unit == 0 && c.type == TextureConfig::TextureCube;
    // The levels down to a side of 8, as the texture cache builds them.
    e.levels = 1;
    while (e.levels <= c.lod.max_level && width >> e.levels >= 8 && height >> e.levels >= 8) {
        e.levels++;
    }
    // The parts: a 2D texture's levels in a row, or a cube's six faces at their own addresses. The
    // host cube's slots have the PICA's +Y and -Y swapped: the program reads at (u, -v, w).
    e.part_addr = {c.GetPhysicalAddress()};
    e.parts = e.cube ? 6 : 1;
    if (e.cube) {
        using Face = Pica::TexturingRegs::CubeFace;
        static constexpr Face slots[6] = {Face::PositiveX, Face::NegativeX, Face::NegativeY,
                                          Face::PositiveY, Face::PositiveZ, Face::NegativeZ};
        for (u32 k = 0; k < 6; k++) {
            e.part_addr[k] = regs.texturing.GetCubePhysicalAddress(slots[k]);
        }
    }
    const u32 bits = UntileFormatFor(t.format).bits;
    for (u32 l = 0; l < e.levels; l++) {
        e.bytes += (width >> l) * (height >> l) * bits / 8;
    }
    return e;
}

bool RasterizerSgxd::UnitReadsDrawn(u32 unit) const {
    const UnitRead& r = unit_reads[unit];
    for (u32 k = 0; k < r.parts; k++) {
        if (r.lo[k] < pass_drawn_hi && pass_drawn_lo < r.hi[k]) {
            return true;
        }
    }
    return false;
}

std::pair<PAddr, PAddr> RasterizerSgxd::DrawnRows() const {
    const auto& fb = regs.framebuffer.framebuffer;
    const auto& rs = regs.rasterizer;
    const s32 height = static_cast<s32>(fb.GetHeight());
    // The viewport clips the primitives, an including scissor the pixels.
    s32 y0 = static_cast<s32>(rs.viewport_corner.y);
    s32 y1 = y0 + static_cast<s32>(std::ceil(f24::FromRaw(rs.viewport_size_y).ToFloat32() * 2));
    const auto& sc = rs.scissor_test;
    if (sc.mode == Pica::RasterizerRegs::ScissorMode::Include) {
        y0 = std::max(y0, static_cast<s32>(sc.y1.Value()));
        y1 = std::min(y1, static_cast<s32>(sc.y2.Value()) + 1);
    }
    y0 = std::clamp(y0, 0, height);
    y1 = std::clamp(y1, 0, height);
    if (y0 >= y1) {
        return {0, 0};
    }
    // Rows run bottom to top in memory, eight to a row of tiles.
    const u32 tile_row = fb.GetWidth() * 8 * FramebufferRegs::BytesPerColorPixel(fb.color_format);
    const u32 first = static_cast<u32>(height - y1) / 8;
    const u32 last = static_cast<u32>(height - 1 - y0) / 8;
    const PAddr addr = fb.GetColorBufferPhysicalAddress();
    return {addr + first * tile_row, addr + (last + 1) * tile_row};
}

void RasterizerSgxd::TextureReads(bool is_indexed, const float* immediate, u32 count) {
    using Topology = Pica::PipelineRegs::TriangleTopology;
    using TextureType = Pica::TexturingRegs::TextureConfig::TextureType;
    using WrapMode = Pica::TexturingRegs::TextureConfig::WrapMode;
    const auto textures = regs.texturing.GetTextures();
    std::array<Pica::OutputVertex, 6> v;
    int quad = -1; // run on the first unit that needs it: 1 when its vertices are known
    for (u32 unit = 0; unit < 3; unit++) {
        UnitRead& r = unit_reads[unit];
        r = {};
        const auto& t = textures[unit];
        if (!t.enabled || t.config.GetPhysicalAddress() == 0) {
            continue;
        }
        const TextureExtent e = TextureExtentFor(unit);
        r.parts = e.parts;
        for (u32 k = 0; k < e.parts; k++) {
            r.lo[k] = e.part_addr[k];
            r.hi[k] = e.part_addr[k] + e.bytes;
        }
        // Narrowed only for a 2D texture of one level over what the pass drew, read by a quad.
        const auto& c = t.config;
        if (!backend.InPass() || !UnitReadsDrawn(unit) || e.cube || e.levels != 1 ||
            (unit == 0 && c.type != TextureType::Texture2D) || c.height < 8) {
            continue;
        }
        if (quad < 0) {
            const auto& pipeline = regs.pipeline;
            const Topology topology = pipeline.triangle_topology;
            const bool list = topology == Topology::List || topology == Topology::Shader;
            quad = pipeline.use_gs == Pica::PipelineRegs::UseGS::No && count == (list ? 6u : 4u) &&
                           (list || topology == Topology::Strip || topology == Topology::Fan) &&
                           QuadVertices(is_indexed, immediate, count, v)
                       ? 1
                       : 0;
            for (u32 j = 0; quad == 1 && j < count; j++) {
                quad = v[j].pos.w.ToFloat32() > 0.0001f ? 1 : 0;
            }
        }
        if (quad == 0) {
            continue;
        }
        const bool unit2_coord1 = regs.texturing.main_config.texture2_use_coord1 != 0;
        float v0 = 1e9f, v1 = -1e9f;
        for (u32 j = 0; j < count; j++) {
            const auto& tc = unit == 0                     ? v[j].tc0
                             : (unit == 1 || unit2_coord1) ? v[j].tc1
                                                           : v[j].tc2;
            v0 = std::min(v0, tc.v().ToFloat32());
            v1 = std::max(v1, tc.v().ToFloat32());
        }
        // Past the edge, a clamped read stays on the edge's row; a repeated one reaches all.
        const bool clamps =
            c.wrap_t == WrapMode::ClampToEdge || c.wrap_t == WrapMode::ClampToBorder;
        if (!(v0 >= 0.0f && v1 <= 1.0f) && !clamps) {
            continue;
        }
        // A texel either side for the filter; rows bottom to top as the buffer's.
        const s32 height = static_cast<s32>(c.height);
        const s32 t0 = std::clamp(static_cast<s32>(std::floor(v0 * height)) - 1, 0, height - 1);
        const s32 t1 = std::clamp(static_cast<s32>(std::ceil(v1 * height)) + 1, 0, height - 1);
        const u32 tile_row = e.bytes / (c.height / 8);
        const u32 first = static_cast<u32>(height - 1 - t1) / 8;
        const u32 last = static_cast<u32>(height - 1 - t0) / 8;
        r.lo[0] = e.part_addr[0] + first * tile_row;
        r.hi[0] = e.part_addr[0] + (last + 1) * tile_row;
    }
}

bool RasterizerSgxd::TexturesReadPassColour() const {
    u32 overlapping = 0;
    for (u32 unit = 0; unit < 3; unit++) {
        if (UnitReadsDrawn(unit)) {
            overlapping |= 1u << unit;
        }
    }
    // Only a unit the program samples reads it (a unit left enabled that no stage reads would end
    // passes for nothing).
    return overlapping != 0 && (overlapping & Usse::SampledTextureUnits(FragmentConfig())) != 0;
}

RasterizerSgxd::Texture& RasterizerSgxd::TextureFromRegs(u32 unit, bool written) {
    using TF = Pica::TexturingRegs::TextureFormat;
    const auto t = regs.texturing.GetTextures()[unit];
    const auto& c = t.config;
    const PAddr addr = c.GetPhysicalAddress();
    const u32 width = c.width, height = c.height;
    const UntileFormat f = UntileFormatFor(t.format);
    DEBUG_ASSERT_MSG(f.code != 0xff, "sgxd: texture unit {} at {:#x}, {}x{}: format {} is not built",
                     unit, addr, width, height, static_cast<u32>(t.format));
    const TextureExtent extent = TextureExtentFor(unit);
    const bool cube = extent.cube;
    const u32 levels = extent.levels;
    DEBUG_ASSERT_MSG(!cube || width == height, "sgxd: a {}x{} cube texture is not built", width,
                     height);
    // ETC1A4 as compressed colour and A8 alpha, or level by level into RGBA.
    const bool split = SplitAlpha(unit);
    const bool etc1a4_levels = !cube && t.format == TF::ETC1A4 && !split;
    const std::array<PAddr, 6>& part_addr = extent.part_addr;
    const u32 parts = extent.parts;
    const u32 bytes = extent.bytes;
    // A colour buffer a pass drew is a texture as any other: guest memory holds what the passes stored
    // and the untile runs after their 3D. Draw ends the open pass before a draw that reads its buffer.
    DEBUG_ASSERT_MSG(!backend.InPass() || backend.PassDraws() == pass_open_draws ||
                         !UnitReadsDrawn(unit),
                     "sgxd: texture unit {} at {:#x} reads the colour buffer at {:#x} its own pass "
                     "drew",
                     unit, part_addr[0], pass_colour.addr);
    Texture* it = FindTexture(addr);
    if (it && (it->width != width || it->height != height || it->format != t.format ||
               it->levels != levels || it->parts != parts || it->part_addr != part_addr)) {
        DropTexture(*it);
        it = nullptr;
    }
    // Untiled again only when its pages were written: by GPU work, or in FCRAM by the guest and the
    // HLE (GuestWrote). FCRAM pages move to CDRAM at the untile, so no clean is needed.
    if (it) {
        it->last_used = passes;
        if (!written && !TextureWritten(*it)) {
            return *it;
        }
    } else {
        // A cube's faces and a wide ETC1A4's levels are untiled at once into the texture's own copy; the
        // others are queued (QueueUntile). The PBE writes whole 32-pixel tiles, so those copies leave room
        // past the chain's end.
        u32 host_bytes = 0;
        if (cube) {
            host_bytes = levels == 1 ? 5 * width * width * 4 +
                                           std::max(width, 32u) * std::max(width, 32u) * 4
                                     : 6 * CubeFaceStride(width) + 32 * 32 * 4;
        }
        for (u32 l = 0, offset = 0; etc1a4_levels && l < levels; l++) {
            const u32 w = width >> l, h = height >> l;
            const u32 rows = w < 32 ? h * w / 32 : h;
            host_bytes = std::max(host_bytes, offset + std::max(w, 32u) * std::max(rows, 32u) * 4);
            offset += w * h * 4;
        }
        textures.push_back({addr, width, height, t.format,
                            host_bytes != 0 ? backend.AllocTexture(host_bytes)
                                            : SgxdBackend::Data{},
                            host_bytes, levels, parts, part_addr, bytes, write_seq});
        it = &textures.back();
        it->last_used = passes;
        IndexTexture(*it);
    }
    it->checked_seq = write_seq;
    it->in_place = cube || etc1a4_levels;
    it->alpha_one = t.format == Pica::TexturingRegs::TextureFormat::RG8;
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
        // Each face a SWIZZLED square (or its chain, level after level), in slots CubeFaceStride apart; a
        // face's whole tiles run over the ones after it, written next.
        const u32 stride = levels == 1 ? width * height * 4 : CubeFaceStride(width);
        for (u32 k = 0; k < 6; k++) {
            for (u32 l = 0, guest = 0, host = 0; l < levels; l++) {
                const u32 w = width >> l;
                backend.Untile({part_addr[k] + guest, w, w, f.bits, f.code, f.swizzle, palette},
                               it->memory.gpu + k * stride + host, w, true);
                guest += w * w * f.bits / 8;
                host += w * w * 4;
            }
        }
    }
    // ETC1 stays compressed in the texture unit's SWIZZLED order, and so does ETC1A4's colour, its
    // alpha an A8 texture beside it (SplitAlpha); the others untile to a LINEAR chain.
    SgxdBackend::UntileSource source{addr, width, height, f.bits, f.code, f.swizzle, palette};
    // A side that is no power of two, which QueueUntile's batches do not take: one level, untiled on
    // its own, rows a multiple of 32 texels apart. As a power-of-two chain its render would need a
    // parameter buffer of its own.
    const bool npot = !cube && (!std::has_single_bit(width) || !std::has_single_bit(height));
    // A side under 8 is less than a tile: the PICA's tile rows are width / 8 tiles apart (none for a
    // 4-wide texture), so rows 8 on read tile 0 again. Decoded on the CPU, rows 32 texels apart.
    const bool tiny = !cube && (width < 8 || height < 8);
    // A one-channel format's queued copy is a byte a texel, which the draws read with the format's
    // own U8 swizzle: a quarter of A8B8G8R8.
    const bool one_channel =
        t.format == TF::A8 || t.format == TF::I8 || t.format == TF::A4 || t.format == TF::I4;
    const bool byte_copy = one_channel && !cube && !npot && !tiny && !etc1a4_levels;
    if (byte_copy) {
        const UntileFormat alpha = UntileFormatFor(f.bits == 8 ? TF::A8 : TF::A4);
        source.code = alpha.code;
        source.swizzle = alpha.swizzle;
        source.palette = PaletteFor(alpha.palette);
        source.byte_copy = true;
    }
    if (tiny) {
        DEBUG_ASSERT_MSG(levels == 1 && t.format != TF::ETC1 && t.format != TF::ETC1A4,
                         "sgxd: texture unit {} at {:#x}, {}x{} in format {} with {} levels: a "
                         "side under 8 is built for one level, not ETC1",
                         unit, addr, width, height, static_cast<u32>(t.format), levels);
        if (it->bytes != 0) {
            backend.FreeTextureAfterPass(it->memory, it->bytes);
        }
        // Rows 32 texels apart, or the width's 32s.
        it->stride = Common::AlignUp(width, 32u);
        it->bytes = it->stride * Common::AlignUp(height, 32u) * 4;
        // Written by the CPU at once: the work that may still sample or untile into the range goes first.
        u32 ready = 0;
        it->memory = backend.AllocTexture(it->bytes, &ready);
        backend.RenderWait(ready, "a tiny texture's range");
        Pica::Texture::TextureInfo info{addr, width, height, 0, t.format, false};
        info.SetDefaultStride();
        const u8* const guest = memory.GetPhysicalPointer(addr);
        for (u32 y = 0; y < height; y++) {
            for (u32 x = 0; x < width; x++) {
                const auto texel = Pica::Texture::LookupTexture(guest, x, y, info);
                u8* const out = it->memory.cpu + (y * it->stride + x) * 4;
                for (u32 k = 0; k < 4; k++) {
                    out[k] = texel[k];
                }
            }
        }
    } else if (npot) {
        DEBUG_ASSERT_MSG(levels == 1 && !etc1a4_levels && t.format != TF::ETC1 &&
                             t.format != TF::ETC1A4,
                         "sgxd: texture unit {} at {:#x}, {}x{} in format {} with {} levels: a "
                         "side that is no power of two is built for one level, not ETC1",
                         unit, addr, width, height, static_cast<u32>(t.format), levels);
        if (it->bytes != 0) {
            backend.FreeTextureAfterPass(it->memory, it->bytes);
        }
        it->stride = Common::AlignUp(width, 32u);
        it->bytes = it->stride * Common::AlignUp(height, 32u) * 4;
        it->memory = backend.AllocTexture(it->bytes);
        backend.Untile(source, it->memory.gpu, it->stride);
    } else if (!cube && !etc1a4_levels) {
        // The copy moves: the last one's ranges go back once the open pass is sent.
        if (it->bytes != 0) {
            backend.FreeTextureAfterPass(it->memory, it->bytes);
        }
        if (it->alpha_bytes != 0) {
            backend.FreeTextureAfterPass(it->alpha, it->alpha_bytes);
        }
        const SgxdBackend::UntileCopy copy = backend.QueueUntile(source, levels);
        it->memory = copy.colour;
        it->bytes = copy.colour_bytes;
        it->byte_swizzle = !byte_copy                                   ? 0
                           : t.format == TF::A8 || t.format == TF::A4 ? 6
                                                                        : 5;
        it->alpha = copy.alpha;
        it->alpha_bytes = copy.alpha_bytes;
    }
    // An ETC1A4 level 0 wider than a split takes: level by level, largest first.
    for (u32 l = 0, guest = 0, host = 0; etc1a4_levels && l < levels; l++) {
        const u32 w = width >> l, h = height >> l;
        backend.Untile({addr + guest, w, h, f.bits, f.code, f.swizzle, palette},
                       it->memory.gpu + host, w);
        guest += w * h * f.bits / 8;
        host += w * h * 4;
    }
#ifdef VITA_DIAGNOSTICS
    texture_stats.untiles++;
    texture_stats.untiled_bytes += u64{bytes} * parts;
#endif
#ifdef VITA_DIAGNOSTICS
    static const u64 untile_log_at = u64{SgxdFlagValue("passlog")} * 1000000;
    if (untile_log_at != 0) {
        const u64 now = sceKernelGetProcessTimeWide();
        if (now >= untile_log_at && now < untile_log_at + 1000000) {
            LOG_INFO(Render, "perf: untile {:#x} {}x{} format {}, {} levels{}{}, {} KiB{}", addr,
                     width, height, static_cast<u32>(t.format), levels, cube ? ", cube" : "",
                     tiny   ? ", tiny"
                     : npot ? ", npot"
                            : "",
                     (u64{bytes} * parts) >> 10, written ? ", the memo saw a write" : "");
        }
    }
#endif
    static std::array<u32, 16> format_untiles{};
    const u32 format_untile = format_untiles[static_cast<u32>(t.format) & 15]++;
    const bool dump_this = format_untile < 8 && SgxdFlag("dumptex");
    // Out of a pass, nothing sends the queue later.
    if (!backend.InPass()) {
        backend.FlushUntiles();
    }
    if (untiles++ < 8 || dump_this) {
        LOG_INFO(Render, "texture {:#x}: {}x{} format {}, {} levels{}, untiled to GPU {:#010x}{}",
                 addr, width, height, static_cast<u32>(t.format), levels, cube ? ", cube" : "",
                 it->memory.gpu, it->byte_swizzle != 0 ? ", a byte a texel" : "");
#ifdef VITA_DIAGNOSTICS
        if (dump_this) {
            backend.FlushUntiles();
            backend.RenderIdle("dumptex");
            DumpTexture(*it, f.bits, split);
        }
#endif
    }
    return *it;
}

u32 RasterizerSgxd::TextureBucket(PAddr addr) {
    if (addr >= Memory::VRAM_PADDR && addr - Memory::VRAM_PADDR < Memory::VRAM_SIZE) {
        return (addr - Memory::VRAM_PADDR) >> 12;
    }
    DEBUG_ASSERT_MSG(addr >= Memory::FCRAM_PADDR &&
                         addr - Memory::FCRAM_PADDR < Memory::FCRAM_N3DS_SIZE,
                     "sgxd: a texture at {:#x}, outside VRAM and FCRAM", addr);
    return (Memory::VRAM_SIZE >> 12) + ((addr - Memory::FCRAM_PADDR) >> 12);
}

RasterizerSgxd::Texture* RasterizerSgxd::FindTexture(PAddr addr) const {
    const u32 bucket = TextureBucket(addr);
    if (bucket >= texture_buckets.size()) {
        return nullptr;
    }
    for (Texture* t = texture_buckets[bucket]; t; t = t->bucket_next) {
        if (t->addr == addr) {
            return t;
        }
    }
    return nullptr;
}

u32 RasterizerSgxd::ReadableBytes(PAddr addr, u32 bytes) {
    if (addr >= Memory::VRAM_PADDR && addr - Memory::VRAM_PADDR < Memory::VRAM_SIZE) {
        const u32 inside = Memory::VRAM_PADDR + Memory::VRAM_SIZE - addr;
        ASSERT_MSG(bytes <= inside + GuestGpuMemory::PastVramBytes,
                   "sgxd: a texture at {:#x}, {} bytes: past the filler after VRAM's end", addr,
                   bytes);
        return std::min(bytes, inside);
    }
    ASSERT_MSG(addr >= Memory::FCRAM_PADDR &&
                   addr - Memory::FCRAM_PADDR + u64{bytes} <= Memory::FCRAM_N3DS_SIZE,
               "sgxd: a texture at {:#x}, {} bytes: outside VRAM and FCRAM, or past FCRAM's end",
               addr, bytes);
    return bytes;
}

std::pair<u32, u32> RasterizerSgxd::PartPages(const Texture& texture, u32 k) {
    const PAddr addr = texture.part_addr[k];
    return {TextureBucket(addr), TextureBucket(addr + ReadableBytes(addr, texture.part_bytes) - 1)};
}

void RasterizerSgxd::IndexTexture(Texture& texture) {
    if (texture_buckets.empty()) {
        texture_buckets.assign(GuestPages, nullptr);
        page_textures.assign(GuestPages, 0);
        page_writes.assign(GuestPages, 0);
    }
    Texture*& head = texture_buckets[TextureBucket(texture.addr)];
    texture.bucket_next = head;
    head = &texture;
    for (u32 k = 0; k < texture.parts; k++) {
        const auto [first, last] = PartPages(texture, k);
        for (u32 p = first; p <= last; p++) {
            DEBUG_ASSERT(page_textures[p] != 0xffff);
            page_textures[p]++;
        }
    }
}

void RasterizerSgxd::DropTexture(Texture& texture) {
    texture_drops++;
    for (u32 k = 0; k < texture.parts; k++) {
        const auto [first, last] = PartPages(texture, k);
        for (u32 p = first; p <= last; p++) {
            DEBUG_ASSERT(page_textures[p] != 0);
            page_textures[p]--;
        }
    }
    for (Texture** link = &texture_buckets[TextureBucket(texture.addr)]; *link;
         link = &(*link)->bucket_next) {
        if (*link == &texture) {
            *link = texture.bucket_next;
            break;
        }
    }
    // A copy the open pass samples goes back once the pass is sent; an evicted one's at once.
    const bool sampled = texture.last_used >= passes;
    for (const auto& [copy, size] : {std::pair{texture.memory, texture.bytes},
                                     std::pair{texture.alpha, texture.alpha_bytes}}) {
        if (size != 0) {
            sampled ? backend.FreeTextureAfterPass(copy, size) : backend.FreeTexture(copy, size);
        }
    }
    // Rare: the list has no way from an element to its place.
    textures.remove_if([&](const Texture& t) { return &t == &texture; });
}

bool RasterizerSgxd::SplitAlpha(u32 unit) const {
    return SplitAlphaIn(regs, unit);
}

bool RasterizerSgxd::SplitAlphaIn(const Pica::RegsInternal& regs, u32 unit) {
    const auto t = regs.texturing.GetTextures()[unit];
    return t.enabled && t.format == Pica::TexturingRegs::TextureFormat::ETC1A4 &&
           !(unit == 0 && t.config.type == Pica::TexturingRegs::TextureConfig::TextureCube) &&
           t.config.width <= UntileGeometry::Etc1A4MaxWidth;
}

void RasterizerSgxd::AlphaTextureWords(const Texture& tex, const u32 colour[4], u32 words[4]) {
    DEBUG_ASSERT(tex.alpha_bytes != 0);
    // LINEAR U8 (0x00, sizes less one); the LODMIN bits in words 2 and 3 kept.
    words[0] = colour[0] & ~(1u << 31);
    words[1] = 3u << 29 | (tex.width - 1) << 12 | (tex.height - 1);
    words[2] = tex.alpha.gpu | (colour[2] & 3u);
    words[3] = 1u << 31 | 6u << 28 | (colour[3] & 3u << 26);
}

u32 RasterizerSgxd::PaletteFor(TexturePalette palette) {
    if (palette == TexturePalette::None) {
        return 0;
    }
    if (!palettes.cpu) {
        // U8888 words at 64-byte multiples: A4 (16), I4 (16), IA4 (256).
        palettes = backend.AllocTextureFixed(0x1000);
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
    // A shadow type reads it as a 2D unit (FragmentConfig).
    const bool cube = unit == 0 && type == TextureConfig::TextureCube;
    DEBUG_ASSERT_MSG(unit != 0 || cube || type == TextureConfig::Texture2D ||
                         type == TextureConfig::Projection2D || type == TextureConfig::Shadow2D ||
                         type == TextureConfig::ShadowCube,
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

const RasterizerSgxd::Texture& RasterizerSgxd::TextureWords(u32 unit, u32 words[4]) {
    using TextureConfig = Pica::TexturingRegs::TextureConfig;
    const auto t = regs.texturing.GetTextures()[unit];
    // A unit that is off still reaches the TEV: it reads zeros, point sampled and clamped.
    const TextureConfig off{};
    // A texture at address 0 reads as transparent black.
    const bool real = t.enabled && t.config.GetPhysicalAddress() != 0;
    const Texture& tex = real ? TextureFor(unit) : NullTexture(unit);
#ifdef VITA_DIAGNOSTICS
    if (tex_dumping && real) {
        DumpFrameTexture(unit, tex);
    }
#endif
    const TextureConfig& c = real ? t.config : off;
    // A shadow texture's texel is one the program compares itself: point sampled from its first level
    // through the unit's wrap, NORM off.
    const bool shadow = unit == 0 && real && c.type == TextureConfig::Shadow2D;
    DEBUG_ASSERT_MSG(!shadow || tex.format == Pica::TexturingRegs::TextureFormat::RGBA8,
                     "sgxd: a shadow texture in format {}", static_cast<u32>(tex.format));
    const u32 mag = !shadow && c.mag_filter == Pica::TexturingRegs::TextureConfig::Linear ? 1 : 0;
    const u32 min = !shadow && c.min_filter == Pica::TexturingRegs::TextureConfig::Linear ? 1 : 0;
    const u32 mipf = !shadow && c.mip_filter == Pica::TexturingRegs::TextureConfig::Linear ? 1 : 0;
    const u32 addressing = AddressMode(c.wrap_s) << 6 | AddressMode(c.wrap_t) << 3;
    // The chain's levels: MIPMAPCLAMP the last level (15 for one), LODMIN the PICA's min_level,
    // DADJUST its bias in eighths, zero at 31.
    const u32 clamp = tex.levels > 1 && !shadow ? tex.levels - 1 : 15;
    const u32 lod_min = shadow ? 0u : std::min<u32>(c.lod.min_level, tex.levels - 1);
    // A bias under DADJUST's -3.875 takes -3.875: the LOD differs only where it is above 3.875 before
    // the bias.
    const s32 dadjust = 31 + ((static_cast<s32>(c.lod.bias) + 16) >> 5);
    DEBUG_ASSERT_MSG(dadjust <= 63,
                     "sgxd: texture unit {}: LOD bias {} is past DADJUST's range", unit,
                     static_cast<s32>(c.lod.bias) / 256.0f);
    // U8U8U8U8, ABGR (R the lowest byte), LINEAR or a CUBE of SWIZZLED faces (log2 sizes);
    // a 2D ETC1, and a split ETC1A4's colour, a SWIZZLED chain of its blocks
    // (SgxdBackend::QueueUntile; TEXFEXT and 0x04, log2 sizes; an ETC1 cube's faces are
    // untiled); NORM.
    if (tex.stride > tex.width) {
        // LINEAR_STRIDED: its stride (in words less one) takes DADJUST's, MIPMAPCLAMP's and the
        // minification filters' bits, so one level, no bias, the magnification filter for both. REPEAT
        // and MIRROR wrap over each side rounded up to a power of two (exact on a power-of-two side).
        const bool pot = std::has_single_bit(tex.width) && std::has_single_bit(tex.height);
        DEBUG_ASSERT_MSG(min == mag &&
                             (pot || (AddressMode(c.wrap_s) == 2 && AddressMode(c.wrap_t) == 2)),
                         "sgxd: texture unit {}: a {}x{} texture (rows {} apart) with filters {} "
                         "{} or wrap {} {}: LINEAR_STRIDED magnifies and minifies alike, and "
                         "repeats or mirrors over a power of two",
                         unit, tex.width, tex.height, tex.stride, mag, min,
                         static_cast<u32>(c.wrap_s.Value()), static_cast<u32>(c.wrap_t.Value()));
        const u32 internal = tex.stride - 1;
        words[0] = (internal >> 3 & 0x3ffu) << 17 | mag << 12 | (internal & 7u) << 9 | addressing |
                   (internal >> 13 & 3u) << 1;
        words[1] = 6u << 29 | 0x0cu << 24 | (tex.width - 1) << 12 | (tex.height - 1);
        words[2] = tex.memory.gpu;
        words[3] = 1u << 31 | (tex.alpha_one ? 4u << 28 : 0u);
        return tex;
    }
    const bool etc1 = (tex.format == Pica::TexturingRegs::TextureFormat::ETC1 ||
                       tex.alpha_bytes != 0) &&
                      tex.parts == 1;
    const u32 log2_sizes = static_cast<u32>(std::countr_zero(tex.width)) << 16 |
                           static_cast<u32>(std::countr_zero(tex.height));
    words[0] = (etc1 ? 1u << 31 : 0u) | static_cast<u32>(std::clamp(dadjust, 0, 63)) << 21 |
               clamp << 17 | mag << 12 | min << 10 | mipf << 9 | addressing;
    // A byte copy is U8 (0x00) with its format's swizzle (Texture::byte_swizzle).
    const u32 code = tex.byte_swizzle != 0 ? 0x00u : 0x0cu;
    words[1] = etc1             ? (SgxdBackend::UntileEtc1 & 0x1fu) << 24 | log2_sizes
               : tex.parts == 6 ? 2u << 29 | code << 24 | log2_sizes
                                : 3u << 29 | code << 24 | (tex.width - 1) << 12 | (tex.height - 1);
    words[2] = tex.memory.gpu | lod_min >> 2;
    // Swizzle 4 on U8U8U8U8: alpha one (bit 2).
    const u32 swizzle = tex.byte_swizzle != 0 ? tex.byte_swizzle : tex.alpha_one ? 4u : 0u;
    words[3] = (shadow ? 0u : 1u << 31) | swizzle << 28 | (lod_min & 3u) << 26;
    return tex;
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
#ifdef VITA_DIAGNOSTICS
    texture_stats.writes++;
#endif
    // A write into an upscaled buffer by anything but its own scaled pass: its picture is stale.
    for (Upscaled& u : upscaled) {
        if (&u != upscaled_writing && start < u.addr + u.bytes && u.addr < end) {
            u.valid = false;
        }
    }
    if (page_textures.empty()) {
        return; // no texture yet (IndexTexture makes the page tables)
    }
    std::array<std::pair<u32, u32>, 2> spans;
    u32 count = 0;
    const auto span = [&](PAddr base, u32 size, u32 base_page) {
        const PAddr lo = std::max(start, base);
        const u64 hi = std::min<u64>(end, u64{base} + size);
        if (lo < hi) {
            spans[count++] = {base_page + ((lo - base) >> 12),
                              base_page + static_cast<u32>((hi - 1 - base) >> 12)};
        }
    };
    span(Memory::VRAM_PADDR, Memory::VRAM_SIZE, 0);
    span(Memory::FCRAM_PADDR, Memory::FCRAM_N3DS_SIZE, Memory::VRAM_SIZE >> 12);
    // Pages no texture covers: nothing to stamp, and write_seq stays.
    u32 covered = 0;
    for (u32 i = 0; i < count; i++) {
        for (u32 p = spans[i].first; p <= spans[i].second; p++) {
            covered |= page_textures[p];
        }
    }
    if (covered == 0) {
        return;
    }
    write_seq++;
    DEBUG_ASSERT_MSG(write_seq != 0, "sgxd: the guest write count wrapped");
    for (u32 i = 0; i < count; i++) {
        std::fill(page_writes.begin() + spans[i].first, page_writes.begin() + spans[i].second + 1,
                  write_seq);
#ifdef VITA_DIAGNOSTICS
        texture_stats.write_pages += spans[i].second + 1 - spans[i].first;
#endif
    }
#ifdef VITA_DIAGNOSTICS
    texture_stats.stamped++;
    write_ring[write_seq % write_ring.size()] = {start, end};
#endif
}

bool RasterizerSgxd::TextureWritten(Texture& texture) {
    if (texture.checked_seq == write_seq) {
        return false;
    }
    const u32 since = texture.checked_seq;
    texture.checked_seq = write_seq;
    // The newest stamp over its parts' pages, without a branch a page.
    u32 newest = 0;
    [[maybe_unused]] u32 pages = 0;
    for (u32 k = 0; k < texture.parts; k++) {
        const auto [first, last] = PartPages(texture, k);
        for (u32 p = first; p <= last; p++) {
            newest = std::max(newest, page_writes[p]);
        }
        pages += last + 1 - first;
    }
    const bool written = newest > since;
#ifdef VITA_DIAGNOSTICS
    // A page written by none of the texture's bytes: the write shares only its page.
    TextureStats& s = texture_stats;
    s.checks++;
    s.check_pages += pages;
    if (written) {
        s.written++;
        if (write_seq - since > write_ring.size()) {
            s.untold++;
        } else {
            bool overlaps = false;
            for (u32 seq = since + 1; seq <= write_seq; seq++) {
                const auto [start, end] = write_ring[seq % write_ring.size()];
                for (u32 k = 0; k < texture.parts; k++) {
                    overlaps |= start < texture.part_addr[k] + texture.part_bytes &&
                                texture.part_addr[k] < end;
                }
            }
            s.shared += overlaps ? 0 : 1;
        }
    }
#endif
    return written;
}

RasterizerSgxd::DepthStore& RasterizerSgxd::DepthStoreFor(PAddr addr, u32 width, u32 height,
                                                        u32 format, u32 quarters, u32 margin) {
    // The buffer's transpose scaled, the widescreen margins beside its columns.
    const auto make = [&] {
        return NewDepthStore(width * quarters / 4, (height + 2 * margin) * quarters / 4, format,
                             quarters != 4 || margin != 0);
    };
    for (DepthStore& d : depth_stores) {
        if (d.addr == addr) {
            if (d.width == width && d.height == height && d.format == format &&
                d.quarters == quarters && d.margin == margin) {
                return d;
            }
            for (const auto* old : {&d.store, &d.scratch}) {
                if (old->memory.cpu) {
                    backend.FreeSurface(old->memory, old->width,
                                        old->height * (old->stencil ? 5 : 4) / 4);
                }
            }
            d = DepthStore{addr, width, height, format, quarters, margin};
            d.store = make();
            return d;
        }
    }
    depth_stores.push_back(DepthStore{addr, width, height, format, quarters, margin});
    depth_stores.back().store = make();
    return depth_stores.back();
}

bool RasterizerSgxd::DepthStoreValid(PAddr addr, u32 width, u32 height, u32 format, u32 quarters,
                                    u32 margin) const {
    return std::any_of(depth_stores.begin(), depth_stores.end(), [&](const DepthStore& d) {
        return d.addr == addr && d.width == width && d.height == height && d.format == format &&
               d.quarters == quarters && d.margin == margin && d.valid;
    });
}

RasterizerSgxd::DepthStore* RasterizerSgxd::DepthStoreOf(PAddr addr, u32 width, u32 height,
                                                       u32 format) {
    for (DepthStore& d : depth_stores) {
        if (d.addr == addr && d.width == width && d.height == height && d.format == format &&
            d.quarters == 4 && d.margin == 0) {
            return &d;
        }
    }
    return nullptr;
}

void RasterizerSgxd::SettleDepthClear(DepthStore& d) {
    if (!d.pending) {
        return;
    }
    const DepthStore::PendingClear p = *d.pending;
    d.pending.reset();
    const bool part = p.part.x1 != 0;
    backend.FillDepthStorePart(d.store, part ? p.part.x0 : 0, part ? p.part.x1 : d.store.width,
                               part ? p.part.y0 : 0, part ? p.part.y1 : d.store.height,
                               p.has_z ? &p.z : nullptr, p.has_stencil ? &p.stencil : nullptr);
}

SgxdBackend::DepthStore RasterizerSgxd::NewDepthStore(u32 width, u32 height, u32 format,
                                                     bool upscaled) {
    // The pass's target, the guest buffer's transpose (SgxdBackend::PicaTarget), in whole 32x32
    // tiles; the stencil (D24S8's) a quarter of the depth's bytes after it.
    const bool stencil = format == static_cast<u32>(FramebufferRegs::DepthFormat::D24S8);
    const u32 w = Common::AlignUp(height, 32u), h = Common::AlignUp(width, 32u);
    return {backend.AllocSurface(w, h * (stencil ? 5 : 4) / 4, upscaled), w, h, stencil};
}

void RasterizerSgxd::DepthStoresWritten(PAddr start, PAddr end) {
    for (DepthStore& d : depth_stores) {
        const u32 bytes = FramebufferRegs::BytesPerDepthPixel(
            static_cast<FramebufferRegs::DepthFormat>(d.format));
        if (start < d.addr + d.width * d.height * bytes && d.addr < end) {
            d.valid = false;
            d.pending.reset();
        }
    }
}

void RasterizerSgxd::EndPass(const char* why) {
    const ColourBuffer& s = pass_colour;
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
    // A pass that used depth keeps it for the next one on the buffer, stored (loaded first when it
    // was loaded, else filled with the clear). A clear no draw used is the store's fill alone; a pass
    // that neither cleared nor loaded it leaves the store stale. An unused depth in the bytes the
    // pass's colour drew keeps no store: guest memory holds the colour.
    const PAddr depth_end = pass_depth + s.width * s.height * pass_depth_bytes;
    const bool depth_under_colour = pass_drawn_lo < pass_drawn_hi && pass_depth < pass_drawn_hi &&
                                    pass_drawn_lo < depth_end;
    DEBUG_ASSERT_MSG(!depth_under_colour || !pass_depth_used,
                     "sgxd: {}: a pass on the colour buffer at {:#x} ({}x{}) used the depth "
                     "buffer at {:#x} in the bytes it drew ({:#x}..{:#x})",
                     why, s.addr, s.width, s.height, pass_depth, pass_drawn_lo, pass_drawn_hi);
    DepthStore* part_copy = nullptr;
    if ((pass_depth_cleared || pass_depth_load || pass_depth_covered) && !depth_under_colour) {
        DepthStore& d = DepthStoreFor(pass_depth, s.width, s.height, pass_depth_format,
                                      pass_quarters, pass_margin);
        // A pass that wrote its part's depth whole keeps the rest of the store as it was.
        if (!pass_depth_load && !pass_depth_part) {
            backend.FillDepthStore(d.store, pass_bg_depth, pass_bg_stencil);
        }
        if (pass_depth_used) {
            using DL = SgxdBackend::DepthLoad;
            DL load = !pass_depth_load ? DL::None : pass_depth_guest ? DL::Guest : DL::Store;
#ifdef VITA_DIAGNOSTICS
            static const bool no_zload = SgxdFlag("nozload");
            load = no_zload ? DL::None : load;
#endif
            // A pass storing part of its target (StorePartFor) also drew depth past it, where the PICA's
            // viewport clipped: its depth goes to the scratch, and only its part into the store once its 3D
            // is done. A pass storing its whole target (PassStoresWholeDepth), or masked to its part from its
            // first draw, keeps its draws off the margin and stores into the store itself.
            const bool masked_whole = pass_margin_masked && !pass_depth_part;
            if (pass_part.x1 != 0 && !PassStoresWholeDepth() && !masked_whole) {
                if (!d.scratch.memory.cpu) {
                    d.scratch = NewDepthStore(d.width * d.quarters / 4,
                                              (d.height + 2 * d.margin) * d.quarters / 4, d.format,
                                              d.quarters != 4 || d.margin != 0);
                }
                backend.SetDepthStore(d.store, load, &d.scratch);
                backend.TouchAllTiles();
                part_copy = &d;
            } else {
                backend.SetDepthStore(d.store, load);
            }
        }
        d.valid = true;
        d.ta_w = pass_ta_w;
        d.ta_map = pass_w_map;
    } else if (pass_depth_used) {
        DepthStoresWritten(pass_depth, pass_depth + s.width * s.height * pass_depth_bytes);
    }
#ifdef VITA_DIAGNOSTICS
    const u32 ended_quarters = pass_quarters;
#endif
    u32 ta_us = 0, d3_us = 0;
    backend.EndPass(&ta_us, &d3_us);
    if (part_copy) {
        // The part's edges in the store's samples: x in the 8s CopyDepthPart takes, exact at 1x, widened
        // out to them when scaled (the extra samples lie past the viewport, never shown).
        const s32 widen = pass_widened ? 1 : 0;
        const u32 x0 = static_cast<u32>(PassX(static_cast<s32>(pass_part.x0), -widen)) & ~7u;
        const u32 x1 = std::min(Common::AlignUp(static_cast<u32>(PassX(
                                                    static_cast<s32>(pass_part.x1), widen)),
                                                8u),
                                part_copy->store.width);
        backend.CopyDepthPart(part_copy->scratch, part_copy->store, x0, x1,
                              static_cast<u32>(PassEdge(static_cast<s32>(pass_part.y0))),
                              static_cast<u32>(PassEdge(static_cast<s32>(pass_part.y1))));
    }
    // A scaled pass stored its picture alone: the guest's buffer from it, at 1x.
    if (pass_upscaled) {
        backend.DownscalePicture(pass_scaled_target);
    }
    // A narrow buffer's scratch back into it, behind the pass's 3D.
    if (pass_narrow) {
        const NarrowScratch& n = *pass_narrow;
        const u32 wide_strip = n.scratch_width * 8 * n.bpp;
        backend.RenderBefore(n.surface.gpu, n.surface.gpu + wide_strip * (n.height / 8),
                             "a narrow colour buffer's copy out");
        CopyLines(n.surface.gpu, s.addr, n.width * 8 * n.bpp, n.height / 8, wide_strip,
                  n.width * 8 * n.bpp, "a narrow colour buffer's copy out");
        pass_narrow = nullptr;
    }
    // The pass stored its tiles into the guest's buffer: the textures over what it drew are
    // untiled again, behind its 3D. The tiles no draw covered store the bytes they loaded or
    // the clear guest memory holds.
    upscaled_writing = pass_upscaled;
    if (pass_drawn_lo < pass_drawn_hi) {
        TexturesWritten(pass_drawn_lo, pass_drawn_hi);
    }
    upscaled_writing = nullptr;
    if (pass_upscaled) {
        pass_upscaled->valid = true;
        pass_upscaled = nullptr;
    }
    pass_quarters = 4;
    pass_margin = 0;
    pass_widened = false;
    pass_drawn_lo = pass_drawn_hi = 0;
    // A pass a clear quad opened: whether the next one there is filled.
    if (pass_clear_site >= 0) {
        clear_quad_sites[pass_clear_site].alone = pass_guest_draws == 1;
        pass_clear_site = -1;
    }
#ifdef VITA_DIAGNOSTICS
    VprofPassResult(s);
    LogTextureStats();
    static const u64 pass_log_at = u64{SgxdFlagValue("passlog")} * 1000000;
    if (pass_log_at != 0) {
        const u64 now = sceKernelGetProcessTimeWide();
        if (now >= pass_log_at && now < pass_log_at + 1000000) {
            const char* depth_how = !pass_depth_used ? "unused"
                                    : !pass_depth_load ? "cleared"
                                    : pass_depth_guest ? "loaded from guest memory"
                                                       : "loaded";
            LOG_INFO(Render,
                     "perf: pass on {:#x} {}x{} at {}/4x: {} draws, {} primitives, opened by {}, "
                     "depth {}, TA {} us, 3D {} us; ended by {}",
                     s.addr, s.width, s.height, ended_quarters, pass_draws, pass_prims, pass_opened,
                     depth_how, ta_us, d3_us, why);
        }
    }
#endif
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
             "perf: textures in {:.2f} s: {} untiled ({} KiB); {} cached; {} lookups, {} from "
             "the memos; {} writes, {} over texture pages ({} pages stamped); {} checks over {} "
             "pages, {} found a write ({} only on a shared page, {} untold); {} samplers of a "
             "unit that is off; {} untiles with a side under 8 ({} texels, {} us)",
             (now - t.start_us) / 1e6, t.untiles, t.untiled_bytes / 1024, textures.size(),
             t.lookups, t.memo_hits, t.writes, t.stamped, t.write_pages, t.checks, t.check_pages,
             t.written, t.shared, t.untold, t.null_samples, t.tiny, t.tiny_texels, t.tiny_us);
    if (clip_plane_draws != 0) {
        LOG_INFO(Render, "perf: user clip plane on in {} of {} draws (ignored), last ({} {} {} {})",
                 clip_plane_draws, clip_plane_all, clip_plane_last[0], clip_plane_last[1],
                 clip_plane_last[2], clip_plane_last[3]);
    }
    clip_plane_draws = clip_plane_all = 0;
    const WidenStats& w = widen_stats;
    LOG_INFO(Render,
             "perf: 8-bit index lists widened: {} ({} KiB read from VRAM, {} KiB from FCRAM on "
             "CDRAM, {} KiB from cached FCRAM), {:.1f} ms",
             w.lists, w.bytes[0] / 1024, w.bytes[1] / 1024, w.bytes[2] / 1024,
             w.cycles / (1000.0 * scePowerGetArmClockFrequency()));
    widen_stats = {};
    // The pool drops textures only when full, so how full it is says nothing of a title's needs: the
    // copies cached, those a draw took this second, the evictions, and the bytes in use.
    u64 cached = 0, taken = 0;
    for (const Texture& tex : textures) {
        const u64 bytes = u64{tex.bytes} + tex.alpha_bytes;
        cached += bytes;
        taken += tex.last_used >= t.passes_mark ? bytes : 0;
    }
    LOG_INFO(Render,
             "perf: texture pool: {} KiB cached, {} KiB taken by draws in the second, {} "
             "evictions; {} KiB of the pool in use",
             cached >> 10, taken >> 10, evictions - t.evictions_mark,
             backend.TexturePoolLive() >> 10);
    std::string ends;
    for (const auto& [why, n] : t.pass_ends) {
        ends += fmt::format("{}{} {}", ends.empty() ? "" : "; ", why, n);
    }
    LOG_INFO(Render, "perf: passes ended by: {}; kept, a narrower viewport masked: {}", ends,
             std::exchange(viewport_masks, 0));
    t.pass_ends.clear();
    t = TextureStats{};
    t.tiny_logged = tiny_logged;
    t.start_us = now;
    t.passes_mark = passes;
    t.evictions_mark = evictions;
}
#endif

u64 RasterizerSgxd::VertexKeyOf(const Pica::Shader::Generator::PicaVSConfig& config,
                                const VertexInputs& in) {
    return Common::HashCombine(
        config.Hash(),
        static_cast<u64>(in.mode) << 32 | static_cast<u64>(in.u8_regs) << 16 | in.default_regs,
        static_cast<u64>(in.bools) << 16 | in.used_bools |
            (static_cast<u64>(in.s8_regs) << 32 | static_cast<u64>(in.s16_regs) << 48),
        Common::ComputeHash64(in.reg_components.data(), in.reg_components.size()));
}

Usse::VsRequest RasterizerSgxd::VertexRequestOf(const Pica::Shader::Generator::PicaVSConfig& config,
                                                const Pica::ShaderSetup& setup,
                                                const VertexInputs& in) {
    const auto& state = config.state;
    Usse::VsRequest request{};
    request.code = setup.GetProgramCode();
    request.swizzle = setup.GetSwizzleData();
    request.code_size =
        std::min<u32>(setup.GetBiggestProgramSize(), Pica::MAX_PROGRAM_CODE_LENGTH);
    request.main_offset = state.main_offset;
    request.num_outputs = state.num_outputs;
    request.output_map = state.output_map;
    const auto maps = state.gs_state.GetSemanticMaps();
    for (u32 i = 0; i < maps.size(); i++) {
        request.semantics[i] = {static_cast<u8>(std::min<u32>(maps[i].attribute_index, 255)),
                                static_cast<u8>(maps[i].component_index & 3)};
    }
    request.default_regs = in.default_regs;
    request.bools = in.bools;
    request.bool_mask = in.used_bools;
    request.reg_components = in.reg_components;
    request.u8_regs = in.u8_regs;
    request.s8_regs = in.s8_regs;
    request.s16_regs = in.s16_regs;
    request.mode = in.mode;
    return request;
}

namespace {
u16 BoolUniforms(const Pica::ShaderSetup& setup) {
    u16 bools = 0;
    for (u32 n = 0; n < 16; n++) {
        bools |= static_cast<u16>((setup.uniforms.b[n] ? 1u : 0u) << n);
    }
    return bools;
}
} // Anonymous namespace

void RasterizerSgxd::PrefetchVertex(const Pica::RegsInternal& parse_regs,
                                    Pica::ShaderSetup& parse_setup) {
    if (parse_regs.pipeline.use_gs == Pica::PipelineRegs::UseGS::Yes) {
        return;
    }
    // The words VertexStageFor reuses a layout's program by: unchanged, the same program.
    const auto words = VertexProgramKey(parse_regs, parse_setup, Usse::VsMode::Draw);
    const u64 layout_hash = StateRegs::Hash(StateRegs::Layout, parse_regs.reg_array);
    if (words == parse_vertex_words && layout_hash == parse_layout_hash) {
        return;
    }
    parse_vertex_words = words;
    parse_layout_hash = layout_hash;
    const Pica::Shader::Generator::PicaVSConfig config{parse_regs, parse_setup};
    u16* used = parse_used_bools.Find(config.state.program_hash);
    if (!used) {
        used = &parse_used_bools.Emplace(config.state.program_hash, UsedBoolUniforms(parse_setup));
    }
    const VertexLayout layout = DecodeVertexLayout(parse_regs);
    const VertexInputs in{static_cast<u16>(~layout.enabled_regs),
                          layout.u8_regs,
                          layout.s8_regs,
                          layout.s16_regs,
                          layout.reg_components,
                          Usse::VsMode::Draw,
                          static_cast<u16>(BoolUniforms(parse_setup) & *used),
                          *used};
    const u64 key = VertexKeyOf(config, in);
    if (!tier1->WantsVertex(key)) {
        return;
    }
    // One copy of a program's code for its variants' jobs.
    const u64 code_hash = parse_setup.GetProgramCodeHash();
    const u64 swizzle_hash = parse_setup.GetSwizzleDataHash();
    if (!parse_vs_code || code_hash != parse_vs_code_hash || swizzle_hash != parse_vs_swizzle_hash) {
        auto code = std::make_shared<ShaderTier1::VertexCode>();
        code->code = parse_setup.GetProgramCode();
        code->swizzle = parse_setup.GetSwizzleData();
        parse_vs_code = std::move(code);
        parse_vs_code_hash = code_hash;
        parse_vs_swizzle_hash = swizzle_hash;
    }
    Usse::VsRequest request = VertexRequestOf(config, parse_setup, in);
    request.code = std::span<const u32>{parse_vs_code->code.data(), request.code.size()};
    request.swizzle = std::span<const u32>{parse_vs_code->swizzle.data(), request.swizzle.size()};
    tier1->QueueVertex(key, request, parse_vs_code);
}

namespace {

/// With gxm_flags dumpusse, an emitted program on host0 as azahar-KIND-KEY.json;
/// `kind` is vs or fs (vs2, fs2: the second tier's), `key` the program cache's.
void DumpProgram(const char* kind, u64 key, const Usse::Program& program) {
    if (!SgxdFlag("dumpusse")) {
        return;
    }
    const std::string json = Usse::ProgramJson(program);
    const std::string path = fmt::format("host0:azahar-{}-{:016x}.json", kind, key);
    if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(json.data(), 1, json.size(), f);
        std::fclose(f);
        LOG_INFO(Render, "{} program {:016x}: written to {}", kind, key, path);
    }
}

/// With sgxd_flags dumpusse, bytes to host0:azahar-NAME.
void DumpBytes(const std::string& name, const void* data, std::size_t size) {
    const std::string path = fmt::format("host0:azahar-{}", name);
    if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(data, 1, size, f);
        std::fclose(f);
    }
}

/// With sgxd_flags dumpusse, the 3DS side of a fragment program: its PICA configuration's bytes
/// (azahar-fs-KEY.fsconfig) and upstream's GLSL for it (azahar-fs-KEY.glsl), for glsl2cg and
/// psp2cgc to compile the same program as Sony's compiler would.
void DumpPicaFragment(u64 key, const Pica::Shader::FSConfig& config) {
    if (!SgxdFlag("dumpusse")) {
        return;
    }
    DumpBytes(fmt::format("fs-{:016x}.fsconfig", key), &config, sizeof(config));
    const std::string glsl = Pica::Shader::Generator::GLSL::GenerateFragmentShader(
        config, Pica::Shader::UserConfig{}, Pica::Shader::Profile{});
    DumpBytes(fmt::format("fs-{:016x}.glsl", key), glsl.data(), glsl.size());
}

/// With sgxd_flags dumpusse, the 3DS side of a vertex program: its PicaVSConfig's bytes, then
/// the PICA program code and swizzle data (4096 words each) as azahar-vs-KEY.pica, and
/// upstream's GLSL for it (azahar-vs-KEY.glsl).
void DumpPicaVertex(u64 key, const Pica::Shader::Generator::PicaVSConfig& config,
                    const Pica::ShaderSetup& setup) {
    if (!SgxdFlag("dumpusse")) {
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
    u16 default_regs, const std::array<u8, 16>& reg_components, u16 u8_regs, u16 s8_regs,
    u16 s16_regs, Usse::VsMode mode) {
    const Pica::Shader::Generator::PicaVSConfig config{regs, pica.vs_setup};
    const auto& state = config.state;
    // The bools the program branches on are baked in; which ones is found once per program.
    u16* used = used_bools.Find(state.program_hash);
    if (!used) {
        used = &used_bools.Emplace(state.program_hash, UsedBoolUniforms(pica.vs_setup));
    }
    const VertexInputs in{default_regs,
                          u8_regs,
                          s8_regs,
                          s16_regs,
                          reg_components,
                          mode,
                          static_cast<u16>(BoolUniforms(pica.vs_setup) & *used),
                          *used};
    const u64 key = VertexKeyOf(config, in);
    if (VertexProgram* const found = vertex_programs.Find(key)) {
        VertexProgram& v = *found;
        if (!v.tier2_queued && ++v.draws >= Tier2Draws) {
            v.tier2_queued = true;
            if (tier2) {
                tier2->QueueVertex(key, VertexRequestOf(config, pica.vs_setup, in));
            }
        }
        return v;
    }

    const Usse::VsRequest request = VertexRequestOf(config, pica.vs_setup, in);
    VertexProgram v{};
    std::string refusal;
    std::optional<Usse::Program> emitted;
    u64 emit_us = 0;
    if (taken) {
        emitted = std::move(taken->program);
        v.input_regs = taken->input_regs;
        refusal = std::move(taken->refusal);
        emit_us = taken->took_us;
    } else {
        const u32 emit_start = azaharCyclesStart();
        emitted = Usse::EmitVertexProgram(request, v.input_regs, Usse::Scratch::Render, &refusal);
        emit_us = Common::CyclesToUs(azaharCycles() - emit_start);
        NoteEmit(0, emit_us);
    }
    // The PICA side before the assert: a program the emitter refuses is the one to look at.
    DumpPicaVertex(key, config, pica.vs_setup, request);
    ASSERT_MSG(emitted.has_value(),
               "sgxd: vertex program {:016x} (entry {}): the emitter has no program for it: {}",
               state.program_hash, request.main_offset, refusal);
    v.usse = std::move(*emitted);
    ResolveVertexProgram(v);
    DumpProgram("vs", key, v.usse);
    DumpPicaVertex(key, config, pica.vs_setup);
    PlaceVertexCode(v);
    LOG_INFO(Render,
             "vertex program {:016x}: {} + {} words, {} PAs, {} temps, {} SAs, {} uniforms{}, "
             "emitted in {} us{}",
             state.program_hash, v.usse.primary.size(), v.usse.secondary.size(), v.usse.pa_count,
             v.usse.temp_count, v.usse.secondary_reg_count, v.usse.uniforms.size(),
             v.usse.per_instance ? ", per instance" : "", emit_us, taken ? " (worker)" : "");
    if (tier2 && v.usse.primary.size() >= Tier2Words) {
        v.tier2_queued = true;
        tier2->QueueVertex(key, request);
    }
    return vertex_programs.Emplace(key, std::move(v));
}

void RasterizerSgxd::ResolveVertexProgram(VertexProgram& v) {
    const Usse::Program& g = v.usse;
    // The image covers the resident uniforms, the data block, the prefetched uniforms and the SAs the
    // secondary program writes; the secondary PDS program DMAs all of it.
    const u32 head = g.uniform_floats + Usse::DataHead(g);
    v.sa_words = std::max(g.secondary_reg_count, head + static_cast<u32>(g.literals.size()));
    v.sa_image.assign(v.sa_words, 0.0f);
    std::memcpy(v.sa_image.data() + head, g.literals.data(), g.literals.size() * 4);
    v.sa_scratch = v.sa_image;
    v.sa_slots.clear();
    v.has_bank = false;
    for (u32 n = 0; n < g.uniforms.size(); n++) {
        const Usse::Uniform& u = g.uniforms[n];
        if (u.kind == Usse::UniformKind::FloatBank) {
            DEBUG_ASSERT(g.buffer_pointer && u.sa_offset + u.array_size * 4 <= g.buffer_floats);
            v.has_bank = true;
            v.bank_offset = u.sa_offset;
            v.bank_rows = u.array_size;
            continue;
        }
        DEBUG_ASSERT(u.sa_offset + u.components * u.array_size <= g.uniform_floats);
        DEBUG_ASSERT(u.kind != Usse::UniformKind::DefaultInput || u.index < 16);
        v.sa_slots.push_back({u.kind, u.components, u.index, u.sa_offset, u.array_size, n});
    }
    v.attr_resource.fill(~0u);
    for (const Usse::Attribute& a : g.attributes) {
        if (a.kind == Usse::AttributeKind::Input) {
            DEBUG_ASSERT(a.index < v.attr_resource.size() && v.attr_resource[a.index] == ~0u);
            v.attr_resource[a.index] = a.resource;
        }
    }
}

void RasterizerSgxd::PlaceVertexCode(VertexProgram& v) {
    const u32 primary = backend.PlaceCode(0, v.usse.primary);
    v.primary_doutu0 =
        sgxd_program_doutu0(SgxdBackend::HeapCodeBase(0), primary, v.usse.temp_count);
    v.code = {primary, static_cast<u32>(v.usse.primary.size() * sizeof(u64)), 0, 0};
    v.secondary_doutu0 = DummyVertexSecondary;
    if (!v.usse.secondary.empty()) {
        const u32 secondary = backend.PlaceCode(0, v.usse.secondary);
        v.secondary_doutu0 = sgxd_program_doutu0(SgxdBackend::HeapCodeBase(0), secondary, 0);
        v.code[2] = secondary;
        v.code[3] = static_cast<u32>(v.usse.secondary.size() * sizeof(u64));
    }
}

void RasterizerSgxd::FreeProgramCode(const std::array<u32, 4>& code) {
    backend.FreeCode(code[0], code[1]);
    backend.FreeCode(code[2], code[3]);
}

void RasterizerSgxd::FlushPrograms() {
    LOG_INFO(Render, "sgxd: code heap {} KiB free: {} vertex-side and {} fragment programs dropped",
             backend.CodeFree() >> 10, vertex_programs.Size(), fragment_programs.Size());
    vertex_programs.ForEach([this](const VertexProgram& v) { FreeProgramCode(v.code); });
    fragment_programs.ForEach([this](const FragmentProgram& f) { FreeProgramCode(f.code); });
    vertex_programs.Clear();
    fragment_programs.Clear();
    fragment_states.Clear();
    vertex_layouts.Clear();
    last_fragment = nullptr;
    last_layout = nullptr;
    last_passthrough = nullptr;
    program_flushes++;
}

RasterizerSgxd::VertexProgram RasterizerSgxd::PlaceVertexProgram(Usse::Program usse,
                                                                 const char* what, u64 key,
                                                                 u64 emit_us) {
    VertexProgram v{};
    v.usse = std::move(usse);
    ResolveVertexProgram(v);
    DumpProgram("vs", key, v.usse);
    PlaceVertexCode(v);
    LOG_INFO(Render, "{} {:016x}: {} words, {} PAs, {} temps, {} SAs{}, emitted in {} us", what,
             key, v.usse.primary.size(), v.usse.pa_count, v.usse.temp_count,
             v.usse.secondary_reg_count, v.usse.per_instance ? ", per instance" : "", emit_us);
    return v;
}

RasterizerSgxd::GeometryKey RasterizerSgxd::GeometryKeyFor(u32 rows, bool rows_uniform,
                                                           bool rows_table, u32 first_uniform,
                                                           u16 input_rows, u32 max_triangles,
                                                           bool serial) {
    Pica::ShaderSetup& setup = pica.gs_setup;
    setup.DoProgramCodeFixup();
    const u64 program_hash = setup.GetProgramCodeHash();
    const u16* found_used = used_bools.Find(program_hash);
    const u16 used =
        found_used ? *found_used : used_bools.Emplace(program_hash, UsedBoolUniforms(setup));
    u16 bools = 0;
    for (u32 n = 0; n < 16; n++) {
        bools |= static_cast<u16>((setup.uniforms.b[n] ? 1u : 0u) << n);
    }
    bools &= used;
    const bool uniform_table = GsUniformTable(rows_uniform, rows_table);
    const u32 output_mask = regs.gs.output_mask;
    // Every field through a combine: a bit XORed onto the result's top bit would undo a bool at the
    // top of the argument before it.
    const u64 key = Common::HashCombine(
        Common::HashCombine(program_hash, setup.GetSwizzleDataHash()),
        static_cast<u64>(regs.gs.main_offset.Value()) << 16 | output_mask,
        static_cast<u64>(bools) << 48 | static_cast<u64>(used) << 32 |
            static_cast<u64>(max_triangles) << 16 | (rows_table ? 0x8000u : 0u) |
            first_uniform << 8 | (rows_uniform ? 0x80u : 0u) | rows,
        (uniform_table ? 1ull << 17 : 0ull) | static_cast<u64>(input_rows) << 1 |
            (serial ? 1u : 0u));
    return {key, bools, used, uniform_table};
}

Usse::VsRequest RasterizerSgxd::GeometryRequest(const GeometryKey& k, u32 rows, bool rows_uniform,
                                                bool rows_table, u32 first_uniform, u16 input_rows,
                                                u32 max_triangles, bool serial) const {
    const Pica::ShaderSetup& setup = pica.gs_setup;
    const u32 output_mask = regs.gs.output_mask;
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
    request.bools = k.bools;
    request.bool_mask = k.used;
    request.mode = Usse::VsMode::Geometry;
    request.gs_rows = rows;
    request.gs_rows_uniform = rows_uniform;
    request.gs_rows_table = rows_table;
    request.gs_uniform_table = k.uniform_table;
    request.gs_first_uniform = first_uniform;
    request.gs_input_rows = input_rows;
    request.gs_max_triangles = max_triangles;
    request.gs_serial = serial;
    return request;
}

const RasterizerSgxd::VertexProgram& RasterizerSgxd::GeometryProgramFor(
    u32 rows, bool rows_uniform, bool rows_table, u32 first_uniform, u16 input_rows,
    u32 max_triangles, bool serial, const Describe& describe) {
    const GeometryKey k = GeometryKeyFor(rows, rows_uniform, rows_table, first_uniform, input_rows,
                                         max_triangles, serial);
    const VertexProgram* const found = vertex_programs.Find(k.key);
    return found ? *found
                 : MakeGeometryProgram(k, rows, rows_uniform, rows_table, first_uniform, input_rows,
                                       max_triangles, serial, describe);
}

void RasterizerSgxd::QueueGeometryPair(u32 rows, bool rows_uniform, bool rows_table,
                                       u32 first_uniform, u16 input_rows, u32 max_triangles,
                                       bool serial, bool b15) {
    Pica::ShaderSetup& setup = pica.gs_setup;
    setup.uniforms.b[15] = b15;
    const GeometryKey first = GeometryKeyFor(rows, rows_uniform, rows_table, first_uniform,
                                             input_rows, max_triangles, serial);
    setup.uniforms.b[15] = true;
    const GeometryKey rest = GeometryKeyFor(rows, rows_uniform, rows_table, first_uniform,
                                            input_rows, max_triangles, serial);
    disk_cache.Open(title_id);
    if (first.key == rest.key || vertex_programs.Find(first.key) != nullptr ||
        vertex_programs.Find(rest.key) != nullptr || disk_cache.Contains(first.key, true) ||
        disk_cache.Contains(rest.key, true)) {
        return;
    }
    tier1->QueueGeometry(rest.key,
                         GeometryRequest(rest, rows, rows_uniform, rows_table, first_uniform,
                                         input_rows, max_triangles, serial));
}

const RasterizerSgxd::VertexProgram& RasterizerSgxd::MakeGeometryProgram(
    const GeometryKey& k, u32 rows, bool rows_uniform, bool rows_table, u32 first_uniform,
    u16 input_rows, u32 max_triangles, bool serial, const Describe& describe) {
    const u64 key = k.key;
    const Usse::VsRequest request = GeometryRequest(
        k, rows, rows_uniform, rows_table, first_uniform, input_rows, max_triangles, serial);
    // The second of a new draw's pair from the first tier's worker: taken first, so its slot is free.
    std::optional<ShaderTier1::Taken> taken = tier1->TakeGeometry(key);
    disk_cache.Open(title_id);
    if (!taken) {
        if (auto cached = disk_cache.Find(key, true)) {
            // A run before emitted its second tier: that one.
            VertexProgram v = PlaceVertexProgram(std::move(cached->program),
                                                 "geometry program from the disk", key);
            v.input_regs = cached->input_regs;
            v.tier2_queued = true;
            return vertex_programs.Emplace(key, std::move(v));
        }
    }
    u32 input_regs = 0;
    std::string refusal;
    std::optional<Usse::Program> emitted;
    u64 emit_us = 0;
    if (taken) {
        emitted = std::move(taken->program);
        input_regs = taken->input_regs;
        refusal = std::move(taken->refusal);
        emit_us = taken->took_us;
        NoteTier1(1, *taken);
    } else {
        const u32 emit_start = azaharCyclesStart();
        emitted = Usse::EmitVertexProgram(request, input_regs, Usse::Scratch::Render, &refusal);
        emit_us = Common::CyclesToUs(azaharCycles() - emit_start);
        NoteEmit(1, emit_us);
    }
    ASSERT_MSG(emitted.has_value(),
               "sgxd: {}: geometry program {:016x}: the emitter has no program for it: {}",
               describe(), pica.gs_setup.GetProgramCodeHash(), refusal);
    VertexProgram v =
        PlaceVertexProgram(std::move(*emitted),
                           taken ? "geometry program (worker)" : "geometry program", key, emit_us);
    v.input_regs = input_regs;
    if (tier2 && v.usse.primary.size() >= Tier2Words) {
        v.tier2_queued = true;
        tier2->QueueVertex(key, request);
    }
    return vertex_programs.Emplace(key, std::move(v));
}

const RasterizerSgxd::VertexProgram& RasterizerSgxd::PassthroughProgramFor(u32 outputs) {
    // mov o<reg>, v<k> for the k-th register of the output mask, then end.
    const u32 output_mask = regs.gs.output_mask;
    // The registers the program follows from: unchanged since the last draw, the same program.
    std::array<u32, 9> from;
    from[0] = output_mask;
    from[1] = regs.rasterizer.vs_output_total;
    for (u32 i = 0; i < 7; i++) {
        from[2 + i] = regs.rasterizer.vs_output_attributes[i].raw;
    }
    if (last_passthrough != nullptr && from == last_passthrough_regs) {
        return *last_passthrough;
    }
    last_passthrough_regs = from;
    last_passthrough = &PassthroughProgramLookup(outputs, output_mask);
    return *last_passthrough;
}

const RasterizerSgxd::VertexProgram& RasterizerSgxd::PassthroughProgramLookup(u32 outputs,
                                                                            u32 output_mask) {
    const Pica::Shader::Generator::PicaFixedGSConfig gs_config{regs};
    const auto maps = gs_config.state.GetSemanticMaps();
    u64 key = Common::HashCombine(0x6770617373ull /* "gpass" */, output_mask);
    for (const auto& m : maps) {
        key = Common::HashCombine(key, static_cast<u64>(m.attribute_index) << 8 | m.component_index);
    }
    if (const VertexProgram* const found = vertex_programs.Find(key)) {
        return *found;
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
    auto emitted = Usse::EmitVertexProgram(request, input_regs, Usse::Scratch::Render, &refusal);
    ASSERT_MSG(emitted.has_value(), "sgxd: the geometry passthrough program: {}", refusal);
    ASSERT(request.num_outputs == outputs);
    VertexProgram v = PlaceVertexProgram(std::move(*emitted), "geometry passthrough", key);
    v.input_regs = input_regs;
    return vertex_programs.Emplace(key, std::move(v));
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
    u16 mapped = 0; // point mode: the input registers the draw loads
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
            mapped |= static_cast<u16>(1u << reg);
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
        const auto& ag = a0.vp->usse;
        const auto out =
            std::find_if(ag.attributes.begin(), ag.attributes.end(), [](const auto& a) {
                return a.kind == Usse::AttributeKind::OutAddress;
            });
        DEBUG_ASSERT(out != ag.attributes.end() && a0.fetch_count < a0.fetch.size());
        a0.fetch[a0.fetch_count++] = {a0_table.gpu, 4, 1, out->resource};
        a0.fresh = true;
        a0.indices = a0_indices.gpu;
        a0.count = a0_count;
        u32 vsec_sa = 0;
        const SgxdBackend::Data vsa = VertexSa(*a0.vp, pica.vs_setup, vsec_sa, describe);
        SgxdRecipeDrawPrograms p{};
        VertexPrograms(a0, vsa, vsec_sa, p, describe);
#ifdef VITA_DIAGNOSTICS
        NoteStreamOut("A0", static_cast<u32>(pipeline.gs_config.mode.Value()), invocations, rows,
                      0, a0_count, *a0.vp);
#endif
        backend.StreamOut(p, a0_count, SgxdBackend::StreamStage::A0, wait);
    }

    // --- pass A1: the geometry program, one invocation a vertex ---
    // b15 turns true after every invocation and stays so until the game writes it again: a program
    // that reads it while it is false runs the first invocation with it false and the rest true.
    const bool b15 = pica.gs_setup.uniforms.b[15];
    const VertexProgram* first_program = nullptr;
    const VertexProgram* program = nullptr;
    u16 input_rows = 0;
    // A program that reads a temporary before it writes it runs its invocations in order, the
    // temporaries kept from each to the next and from the draw before, as the PICA's unit keeps them.
    bool serial = false;
    const auto programs = [&] {
        // A new draw's two programs at once: the second on the first tier's worker.
        QueueGeometryPair(rows, rows_uniform, rows_table, first_uniform, input_rows, cap, serial,
                          b15);
        pica.gs_setup.uniforms.b[15] = b15;
        first_program = &GeometryProgramFor(rows, rows_uniform, rows_table, first_uniform,
                                            input_rows, cap, serial, describe);
        pica.gs_setup.uniforms.b[15] = true;
        program = &GeometryProgramFor(rows, rows_uniform, rows_table, first_uniform, input_rows,
                                      cap, serial, describe);
    };
    programs();
    if (!serial && (first_program->usse.gs_temps_read_first | program->usse.gs_temps_read_first)) {
        serial = true;
        programs();
    }
    // The unit keeps its input registers between draws: one the draw does not load holds the row an
    // earlier point-mode draw's last invocation loaded, or zero. The draw reads it where that draw's
    // vertex program stored it: in the same pass (its A0s run before its A1s), in an earlier one's
    // data, or in gs_kept_rows (KeepGsRows copies it out before the room is taken).
    const u16 stale = static_cast<u16>((first_program->input_regs | program->input_regs) & ~mapped &
                                       gs_regs_loaded);
    DEBUG_ASSERT_MSG(stale == 0 || !rows_table,
                     "sgxd: {}: a variable-primitive geometry program reads input registers {:#x} "
                     "an earlier draw loaded: not built",
                     describe(), stale);
    if (rows_uniform && stale != 0) {
        input_rows = stale;
        programs();
    } else if (!rows_uniform && stale >> rows != 0) {
        rows = static_cast<u32>(std::bit_width(stale));
        programs();
    }
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
    const bool uniform_table = GsUniformTable(rows_uniform, rows_table);
    ASSERT_MSG(!uniform_table || first_uniform + rows <= 128,
               "sgxd: {}: geometry rows c{} to c{} past the float uniforms", describe(),
               first_uniform, first_uniform + rows - 1);
    const u32 words = rows_table ? 4
                                 : 2 + rows + static_cast<u32>(std::popcount(input_rows)) +
                                       (uniform_table ? 1u : 0u);
    const SgxdBackend::Data gs_table = stored_data(invocations * words * 4);
    if (!gs_zero_row.cpu) {
        gs_zero_row = backend.AllocTextureFixed(16);
        std::memset(gs_zero_row.cpu, 0, 16);
    }
    // The row tables (128 entries and 3 a fetch past the last may read) of the variable mode and of
    // a fixed-primitive program's indexed reads, and the variable mode's count rows.
    constexpr u32 TableWords = 128 + 3;
    const SgxdBackend::Data row_tables = rows_table || uniform_table
                                             ? stored_data(invocations * TableWords * 4)
                                             : SgxdBackend::Data{};
    const SgxdBackend::Data count_rows =
        rows_table ? backend.AllocData(invocations * 16) : SgxdBackend::Data{};
    // Each invocation's entry is made in `entry` and copied out whole: the pass data is uncached, and
    // reading it back would wait on the bus. The unfilled rows and the kept registers' rows are the
    // same in every entry.
    std::array<u32, 2 + 64 + 16 + 1> entry;
    std::array<u32, 64> row_vertex; // set for the rows with an attribute, the only ones read
    std::array<u32, 64> row_offset; // the output's bytes, less 4
    if (!rows_table) {
        for (u32 r = 0; r < rows; r++) {
            const int a = row_attr[r];
            if (a < 0) {
                entry[2 + r] = (stale >> r & 1) != 0 ? gs_last_rows[r] : gs_zero_row.gpu - 4;
                continue;
            }
            row_vertex[r] = static_cast<u32>(a) / vs_outputs;
            row_offset[r] = (static_cast<u32>(a) % vs_outputs) * 16 - 4;
        }
        for (u32 r = 0, k = 2 + rows; r < 16; r++) {
            if ((input_rows >> r & 1) != 0) {
                entry[k++] = gs_last_rows[r];
            }
        }
    }
    u32* const rows_out = reinterpret_cast<u32*>(gs_table.cpu);
    for (u32 i = 0; i < invocations; i++) {
        entry[0] = gs_out.gpu + i * cap * tri_bytes;
        entry[1] = gs_ends.gpu + i * 4;
        if (rows_table) {
            entry[2] = row_tables.gpu + i * TableWords * 4 - 4;
            entry[3] = 0;
        } else {
            for (u32 r = 0; r < rows; r++) {
                if (row_attr[r] >= 0) {
                    const u32 v = vertex_at(i * per_invocation + row_vertex[r]) - a0.vertex_min;
                    entry[2 + r] = vs_out.gpu + v * vsize + row_offset[r];
                }
            }
            if (uniform_table) {
                // The rows at their uniforms' entries; run_invocations fills the others.
                entry[words - 1] = row_tables.gpu + i * TableWords * 4 - 4;
                std::memcpy(reinterpret_cast<u32*>(row_tables.cpu) + i * TableWords +
                                first_uniform,
                            entry.data() + 2, rows * 4);
            }
        }
        std::memcpy(rows_out + i * words, entry.data(), words * 4);
    }
    // The rows the last invocation loaded: what those registers hold for the draws after.
    if (!rows_table) {
        if (!gs_kept_rows.cpu) {
            gs_kept_rows = backend.AllocTextureFixed(16 * 16);
        }
        for (u32 r = 0; r < rows && !rows_uniform; r++) {
            if (row_attr[r] >= 0) {
                gs_last_rows[r] = entry[2 + r];
                gs_rows_kept &= static_cast<u16>(~(1u << r));
                gs_rows_own = static_cast<u16>(gs_own_data ? gs_rows_own | 1u << r
                                                           : gs_rows_own & ~(1u << r));
            }
        }
    }
    gs_regs_loaded |= mapped;
    // Invocations [first, first + n) with program gp, padded to whole triangles with the last again.
    const auto run_invocations = [&](const VertexProgram& gp, u32 first, u32 n) {
        const auto in = std::find_if(gp.usse.attributes.begin(), gp.usse.attributes.end(),
                                     [](const auto& a) {
                                         return a.kind == Usse::AttributeKind::GsInRows;
                                     });
        DEBUG_ASSERT(in != gp.usse.attributes.end());
        VertexStage a1;
        a1.vp = &gp;
        a1.fetch_count = 1;
        a1.fresh = true; // the table or the serial entries
        if (serial) {
            // One instance runs them all (VsRequest::gs_serial): its entry the count, the first table entry
            // and the unit's temporaries; the triangle that vertex three times.
            if (!gs_unit_temps.cpu) {
                gs_unit_temps = backend.AllocTextureFixed(Usse::GsStateQuads * 16);
                std::memset(gs_unit_temps.cpu, 0, Usse::GsStateQuads * 16);
            }
            const SgxdBackend::Data entries = backend.AllocData(16);
            u32* const e = reinterpret_cast<u32*>(entries.cpu);
            e[0] = std::bit_cast<u32>(static_cast<float>(n));
            e[1] = gs_table.gpu + first * words * 4 - 4;
            e[2] = gs_unit_temps.gpu - 4;
            e[3] = gs_unit_temps.gpu;
            a1.fetch[0] = {entries.gpu, 16, 4, in->resource};
            a1.count = 3;
        } else {
            a1.fetch[0] = {gs_table.gpu, words * 4, words, in->resource};
            a1.count = Common::AlignUp(n, 3u);
        }
        const SgxdBackend::Data a1_indices = backend.AllocData(a1.count * 2, 4);
        u16* const list = reinterpret_cast<u16*>(a1_indices.cpu);
        DEBUG_ASSERT(invocations <= 0x10000);
        for (u32 i = 0; i < a1.count; i++) {
            list[i] = static_cast<u16>(serial ? 0 : first + std::min(i, n - 1));
        }
        a1.indices = a1_indices.gpu;
        a1.setup = &pica.gs_setup;
        u32 vsec_sa = 0;
        const SgxdBackend::Data vsa = VertexSa(gp, pica.gs_setup, vsec_sa, describe);
        if (rows_table) {
            // Past its rows an entry points at the shared uniform, as the SA image's bank pointer reads it.
            DEBUG_ASSERT(gp.usse.buffer_pointer);
            u32 pointer = 0;
            std::memcpy(&pointer, vsa.cpu + gp.usse.uniform_floats * 4, 4);
            u32* const tables = reinterpret_cast<u32*>(row_tables.cpu);
            float* const counts = reinterpret_cast<float*>(count_rows.cpu);
            for (u32 i = first; i < first + n; i++) {
                const u32 j = prims[i];
                const u32 v_n = vertex_at(j);
                const u32 own = 1 + main_vertices * vs_outputs + (v_n - main_vertices);
                u32* const t = tables + i * TableWords;
                std::fill_n(counts + i * 4, 4, static_cast<float>(v_n));
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
        NoteStreamOut("A1", static_cast<u32>(pipeline.gs_config.mode.Value()), n, rows, cap,
                      a1.count, gp);
#endif
        // In order, a render of its own: the A1 draws of one render run side by side, and the unit's
        // temporaries go from one in-order draw to the next.
        if (serial) {
            backend.FlushStreamOut(false);
        }
        backend.StreamOut(p, a1.count, SgxdBackend::StreamStage::A1, wait);
        if (serial && !wait) {
            backend.FlushStreamOut(false);
        }
#ifdef VITA_DIAGNOSTICS
        stream_note.stage = nullptr;
#endif
    };
    if (first_program == program) {
        run_invocations(*program, 0, invocations);
    } else {
        run_invocations(*first_program, 0, 1);
        if (invocations > 1) {
            run_invocations(*program, 1, invocations - 1);
        }
    }

    // --- pass B: the stored triangles, as the draw's vertices ---
    std::vector<u16>& list = gs_list;
    list.clear();
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
#ifdef VITA_DIAGNOSTICS
        // The programs and their state for tools/sgxd_gs_stage.py, before the assert.
        if (!(ends[i] >= first && (ends[i] - first) % tri_bytes == 0 && stored < cap)) {
            stream_note.stage = "B";
            PrintStreamOut();
        }
#endif
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
    if (gs_draws++ < 4) {
        LOG_INFO(Render, "geometry draw {}: {} invocations, {} triangles{}", gs_draws, invocations,
                 list.size() / 3, padded ? " (every slot, the room the program's bound)" : "");
    }
    VertexStage b;
    if (list.empty()) {
        return b;
    }
    const VertexProgram& pp = PassthroughProgramFor(gs_outputs);
    for (u32 k = 0; k < gs_outputs; k++) {
        if (pp.attr_resource[k] == ~0u) {
            continue; // an output no semantic reads
        }
        b.fetch[b.fetch_count++] = {gs_out.gpu + k * 16, gs_outputs * 16, 4, pp.attr_resource[k]};
    }
    const SgxdBackend::Data b_indices = backend.AllocData(static_cast<u32>(list.size()) * 2, 4);
    std::memcpy(b_indices.cpu, list.data(), list.size() * 2);
    b.vp = &pp;
    b.fresh = true; // the stored triangles
    b.indices = b_indices.gpu;
    b.count = static_cast<u32>(list.size());
    b.setup = &pica.vs_setup;
    return b;
}

Pica::Shader::FSConfig RasterizerSgxd::FragmentConfig() const {
    return FragmentConfigFor(regs, draw_own_pixel, draw_kills_viewport);
}

void RasterizerSgxd::PrefetchFragment(const Pica::RegsInternal& parse_regs) {
    tier1->QueueFragment(FragmentConfigFor(parse_regs, 0, false));
}

Pica::Shader::FSConfig RasterizerSgxd::FragmentConfigFor(const Pica::RegsInternal& regs,
                                                         u32 own_pixel, bool kills_viewport) {
    Pica::Shader::FSConfig config{regs};
    // The SGX's samplers have no border colour: a border mode clamps (AddressMode) and the fragment
    // program puts the border colour past the texture.
    config.texture.ApplyProfile(Pica::Shader::Profile{});
    // MIN and MAX blend in the epilogue's SOP2 (BlendWord): nothing for the fragment program to
    // emulate.
    for (auto* blend : {&config.framebuffer.requested_rgb_blend,
                        &config.framebuffer.requested_alpha_blend}) {
        if (blend->RequiresMinMaxEmulation()) {
            blend->SetMinMaxEmulationDisabled();
        }
    }
    // An alpha test whose failures the blend would leave as the target has them (alpha 0 takes zero
    // of itself and all of the target, with no depth or stencil write): the program gives those
    // fragments alpha 0 instead of killing them, and the draw is translucent instead of
    // punch-through, whose shading waits for the ISP's feedback.
    const auto& om = regs.framebuffer.output_merger;
    const auto func = config.framebuffer.alpha_test_func.Value();
    if (func != FramebufferRegs::CompareFunc::Always &&
        func != FramebufferRegs::CompareFunc::Never && om.alphablend_enable &&
        om.fragment_operation_mode == FramebufferRegs::FragmentOperationMode::Default) {
        using Eq = FramebufferRegs::BlendEquation;
        using F = FramebufferRegs::BlendFactor;
        const auto keeps_target = [](Eq eq, F src, F dst, bool colour) {
            const bool src_zero =
                src == F::Zero || src == F::SourceAlpha || (colour && src == F::SourceAlphaSaturate);
            const bool dst_one = dst == F::One || dst == F::OneMinusSourceAlpha;
            return (eq == Eq::Add || eq == Eq::ReverseSubtract) && src_zero && dst_one;
        };
        const auto& ab = om.alpha_blending;
        const auto& fb = regs.framebuffer.framebuffer;
        const bool dwrite = fb.allow_depth_stencil_write != 0 && om.depth_write_enable;
        if (keeps_target(ab.blend_equation_rgb, ab.factor_source_rgb, ab.factor_dest_rgb, true) &&
            keeps_target(ab.blend_equation_a, ab.factor_source_a, ab.factor_dest_a, false) &&
            !dwrite && !om.stencil_test.enable) {
            config.framebuffer.alpha_test_blends.Assign(1);
        }
    }
    // An include box over the whole viewport (within the colour buffer) keeps every fragment the pass
    // stores: the test goes, and with it the kill that made the draw punch-through.
    const auto& rs = regs.rasterizer;
    const auto& sc = rs.scissor_test;
    // A draw writing depth past its part into a store that keeps all of it keeps its fragments by the
    // scissor's kill (the viewport's box); a viewport off the tile grid is the ISP's mask instead.
    if (kills_viewport) {
        DEBUG_ASSERT_MSG(config.framebuffer.scissor_test_mode !=
                             Pica::RasterizerRegs::ScissorMode::Exclude,
                         "sgxd: an excluding scissor with a viewport kept by the kill: not built");
        config.framebuffer.scissor_test_mode.Assign(Pica::RasterizerRegs::ScissorMode::Include);
    } else if (config.framebuffer.scissor_test_mode == Pica::RasterizerRegs::ScissorMode::Include) {
        const auto& fb = regs.framebuffer.framebuffer;
        const s32 vx0 = std::max<s32>(rs.viewport_corner.x, 0);
        const s32 vy0 = std::max<s32>(rs.viewport_corner.y, 0);
        const s32 vx1 =
            std::min<s32>(rs.viewport_corner.x +
                              static_cast<s32>(f24::FromRaw(rs.viewport_size_x).ToFloat32() * 2),
                          static_cast<s32>(fb.GetWidth()));
        const s32 vy1 =
            std::min<s32>(rs.viewport_corner.y +
                              static_cast<s32>(f24::FromRaw(rs.viewport_size_y).ToFloat32() * 2),
                          static_cast<s32>(fb.GetHeight()));
        if (static_cast<s32>(sc.x1) <= vx0 && static_cast<s32>(sc.y1) <= vy0 &&
            static_cast<s32>(sc.x2) + 1 >= vx1 && static_cast<s32>(sc.y2) + 1 >= vy1) {
            config.framebuffer.scissor_test_mode.Assign(
                Pica::RasterizerRegs::ScissorMode::Disabled);
        }
    }
    u32 alpha_split = 0;
    for (u32 unit = 0; unit < 3; unit++) {
        alpha_split |= SplitAlphaIn(regs, unit) ? 1u << unit : 0u;
    }
    config.texture.alpha_split.Assign(alpha_split);
    config.texture.own_pixel.Assign(own_pixel);
    // A shadow unit 0 that is off reads zeros, as the software renderer leaves its colour: the null
    // texture, read as a plain 2D unit's.
    using TextureType = Pica::TexturingRegs::TextureConfig::TextureType;
    const auto unit0 = regs.texturing.GetTextures()[0];
    if ((config.texture.texture0_type == TextureType::Shadow2D ||
         config.texture.texture0_type == TextureType::ShadowCube) &&
        (!unit0.enabled || unit0.config.GetPhysicalAddress() == 0)) {
        config.texture.texture0_type.Assign(TextureType::Texture2D);
    }
    // A Shadow2D unit 0's size: its border test's bounds.
    if (config.texture.texture0_type == Pica::TexturingRegs::TextureConfig::Shadow2D) {
        const auto& t0 = regs.texturing.texture0;
        const u32 w = t0.width, h = t0.height;
        DEBUG_ASSERT_MSG(std::has_single_bit(w) && std::has_single_bit(h) && w >= 8 && h >= 8 &&
                             w <= 1024 && h <= 1024,
                         "sgxd: a {}x{} shadow texture: sides of 8 to 1024, powers of two, are "
                         "built",
                         w, h);
        config.texture.shadow_width_log2.Assign(static_cast<u32>(std::countr_zero(w)) - 3);
        config.texture.shadow_height_log2.Assign(static_cast<u32>(std::countr_zero(h)) - 3);
    }
    return config;
}

const RasterizerSgxd::FragmentProgram& RasterizerSgxd::FragmentProgramFor(const Epilogue& epilogue) {
    const u64 epilogue_key = epilogue.Key();
    if (fragment_regs_dirty) {
        fragment_regs_dirty = false;
        fragment_regs_hash = StateRegs::Hash(StateRegs::Fragment, regs.reg_array);
    }
    if (!last_fragment || fragment_regs_hash != last_fragment_regs ||
        epilogue_key != last_fragment_epilogue || draw_own_pixel != last_fragment_own ||
        draw_kills_viewport != last_fragment_kills ||
        draw_depth_stencil != last_fragment_depth_stencil) {
        last_fragment_regs = fragment_regs_hash;
        last_fragment_epilogue = epilogue_key;
        last_fragment_own = draw_own_pixel;
        last_fragment_kills = draw_kills_viewport;
        last_fragment_depth_stencil = draw_depth_stencil;
        // The own-pixel units come from the vertices, the viewport's kill from the pass, and the depth and
        // stencil writes from registers the fragment ones leave out.
        u64 state = Common::HashCombine(fragment_regs_hash, epilogue_key);
        if (draw_own_pixel != 0) {
            state = Common::HashCombine(state, draw_own_pixel);
        }
        if (draw_kills_viewport) {
            state = Common::HashCombine(state, 0x6b696c6cu);
        }
        if (draw_depth_stencil) {
            state = Common::HashCombine(state, 0x64737772u);
        }
        if (const FragmentState* const found = fragment_states.Find(state)) {
            last_fragment = found->program;
            last_fragment_key = found->key;
        } else {
            const Pica::Shader::FSConfig config = FragmentConfig();
            const u64 key = Common::HashCombine(config.Hash(), epilogue_key);
            FragmentProgram* program = fragment_programs.Find(key);
            std::optional<ShaderTier1::Taken> taken;
            if (!program) {
                // The first tier: registers placed, no optimising passes, and the draw waits for it (a
                // worker's if one began it, else emitted here). The second tier replaces it after
                // Tier2Draws draws.
                std::string refusal;
                std::optional<Usse::Program> emitted;
                u64 emit_us = 0;
                if (taken) {
                    emitted = std::move(taken->program);
                    refusal = std::move(taken->refusal);
                    emit_us = taken->took_us;
                } else {
                    const u32 emit_start = azaharCyclesStart();
                    emitted = Usse::EmitFragmentProgram(config, Usse::Scratch::Render, &refusal);
                    emit_us = Common::CyclesToUs(azaharCycles() - emit_start);
                    NoteEmit(2, emit_us);
                }
                ASSERT_MSG(emitted.has_value(),
                           "sgxd: the emitter has no fragment program for this state: {}",
                           refusal);
                DumpProgram("fs", key, *emitted);
                DumpPicaFragment(key, config);
                program = &fragment_programs.Emplace(
                    key, PlaceFragmentProgram(std::move(*emitted), epilogue, key, emit_us));
                if (tier2 && program->usse.primary.size() >= Tier2Words) {
                    program->tier2_queued = true;
                    QueueFragmentTier2(key, config);
                }
            }
            if (fragment_states.Size() >= MaxFragmentStates) {
                fragment_states.Clear();
            }
            fragment_states.Emplace(state, FragmentState{program, key});
            last_fragment = program;
            last_fragment_key = key;
        }
    }
    FragmentProgram& f = *last_fragment;
    if (!f.tier2_queued && ++f.draws >= Tier2Draws) {
        f.tier2_queued = true;
        if (tier2) {
            QueueFragmentTier2(last_fragment_key, FragmentConfig());
        }
    }
    return f;
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
        // One job a config (QueueFragmentTier2): every program of it that waits takes the result.
        Tier2Config* const c = tier2_configs.Find(result.key);
        ASSERT_MSG(c != nullptr, "sgxd: tier 2 of fragment config {:016x} that was never queued",
                   result.key);
        ASSERT_MSG(result.program.has_value(), "sgxd: tier 2 of fragment config {:016x}: {}",
                   result.key, result.refusal);
        c->program = std::move(*result.program);
        LOG_INFO(Render,
                 "tier 2 fragment config {:016x}: emitted in {} us ({} cycles), program {:016x}, "
                 "for {} programs",
                 result.key, result.took_us, result.took_cycles, ProgramHash(*c->program),
                 c->waiting.size());
        for (const u64 key : std::exchange(c->waiting, {})) {
            InstallFragmentTier2(key, *c->program);
        }
    }
}

void RasterizerSgxd::InstallVertexTier2(ShaderTier2::Result& result) {
    VertexProgram* const old = vertex_programs.Find(result.key);
    ASSERT_MSG(result.program.has_value(), "sgxd: tier 2 of vertex program {:016x}: {}",
               result.key, result.refusal);
    ASSERT(old != nullptr || program_flushes != 0);
    if (!old) {
        return;
    }
    // The draw binds an attribute for each input register the program reads: both tiers must read
    // the same ones.
    ASSERT_MSG(result.input_regs == old->input_regs,
               "sgxd: tier 2 of vertex program {:016x} reads input registers {:#x}, tier 1 {:#x}",
               result.key, result.input_regs, old->input_regs);
    DumpProgram("vs2", result.key, *result.program);
    const u32 temps_before = old->usse.temp_count;
    const std::size_t words_before = old->usse.primary.size();
    VertexProgram v{};
    v.usse = std::move(*result.program);
    ResolveVertexProgram(v);
    v.input_regs = result.input_regs;
    PlaceVertexCode(v);
    v.draws = old->draws;
    v.tier2_queued = true;
    LOG_INFO(Render,
             "tier 2 vertex program {:016x}: {} words to {}, {} temps to {}, emitted in {} us ({} "
             "cycles), program {:016x}",
             result.key, words_before, v.usse.primary.size(), temps_before, v.usse.temp_count,
             result.took_us, result.took_cycles, ProgramHash(v.usse));
    FreeProgramCode(old->code);
    *old = std::move(v);
}

RasterizerSgxd::FragmentProgram RasterizerSgxd::PlaceFragmentProgram(Usse::Program usse,
                                                                     const Epilogue& epilogue,
                                                                     u64 hash, u64 emit_us) {
    FragmentProgram f{};
    f.usse = std::move(usse);
    f.epilogue = epilogue;
    const Usse::Program& g = f.usse;
    // A second phase (a kill or a depth write) gets its address into the first phase's PHAS where the
    // code is placed (PlaceCodePhased).

    // SMP reads its four state words as two pairs: with memory uniforms the resident count is odd and
    // the buffer pointer makes it even.
    DEBUG_ASSERT(g.samplers.empty() ||
                 Usse::SamplerSaOffset(g.uniform_floats + Usse::DataHead(g), 0) % 2 == 0);
    for (const Usse::Uniform& u : g.uniforms) {
        // Resident, or in the memory buffer (FragmentSa) at the same float index.
        DEBUG_ASSERT(u.sa_offset + (u.u8 ? 1u : u.f16 ? 2u : u.components) * u.array_size <=
                     (g.buffer_pointer ? g.buffer_floats : g.uniform_floats));
        const UniformField field = FragmentUniformField(u);
        DEBUG_ASSERT(!u.u8 || (!field.is_int && u.sa_offset + u.array_size <= g.uniform_floats));
        DEBUG_ASSERT(!u.f16 || (!field.is_int && u.components == 4 &&
                                u.sa_offset + 2 * u.array_size <= g.uniform_floats));
        f.uniforms.push_back(
            {u.sa_offset, static_cast<u16>(field.offset), static_cast<u8>(u.components),
             static_cast<u8>(u.array_size), 16, field.is_int,
             u.kind == Usse::UniformKind::LightDirection, u.u8, u.f16, u.lane,
             static_cast<u8>(u.kind == Usse::UniformKind::LightingLutRow         ? 1
                             : u.kind == Usse::UniformKind::LightingLutCoordHalf ? 2
                             : u.kind == Usse::UniformKind::LightingLutCoordWrap ? 3
                                                                                 : 0),
             static_cast<u8>(u.kind == Usse::UniformKind::LightDistAttenScale  ? 1
                             : u.kind == Usse::UniformKind::LightDistAttenBias ? 2
                                                                               : 0)});
    }

    std::vector<u64> code = g.primary;
    // sgxd_flags fswhite=1: every fragment white, the program still run (kills included); fswhite=2:
    // the program replaced by the white write alone. fsonly=N: those probes only on the programs whose
    // keys' low 32 bits are N (drawkeys logs each draw's key).
    static const std::vector<u32> fs_only = SgxdFlagValues("fsonly");
    const u32 key = static_cast<u32>(hash);
    const bool probed =
        fs_only.empty() || std::find(fs_only.begin(), fs_only.end(), key) != fs_only.end();
    if (!fs_only.empty()) {
        LOG_INFO(Render, "fsonly: program {:016x} {}", hash, probed ? "probed" : "kept");
    }
    const u32 fs_white = probed ? SgxdFlagValue("fswhite") : 0;
    std::span<const u32> phase_starts = g.phase_starts;
    static constexpr u32 single_phase[1] = {0};
    // sgxd_flags fscut=N: a single-phase program's first N instructions, then the white write.
    const u32 fs_cut = probed ? SgxdFlagValue("fscut") : 0;
    const bool cut = fs_cut != 0 && phase_starts.size() == 1 && fs_cut < code.size();
    if (cut) {
        code.resize(fs_cut);
    }
    if (fs_white == 2 || fs_white == 4) {
        // fswhite=4: as many NOPs as the program's words before the white write.
        const std::size_t words = code.size();
        code = {0xfa44070000000000ull}; // PHAS (last)
        if (fs_white == 4) {
            code.insert(code.end(), words, 0xf800094000000000ull);
        }
        code.push_back(Limm(1, 0, 0xffffffffu, true)); // LIMM o0
        phase_starts = single_phase;
    } else if (fs_white == 1 || cut) {
        code.push_back(Limm(1, 0, 0xffffffffu, true));
    } else if (g.output_u8) {
        // pa0 holds the colour as U8 already: the blend reads it as it is; without one, a copy into o0, or
        // with bytes kept a select by the write mask.
        if (epilogue.split_alpha) {
            code.push_back(Limm(2, 2, epilogue.colour, false));
            code.push_back(epilogue.split_alpha);
            code.push_back(epilogue.blend);
            code.push_back(Limm(2, 2, 0xff000000u, false));
            code.push_back(BlendSplitSelect());
        } else if (epilogue.blend) {
            if (epilogue.constant) {
                code.push_back(Limm(2, 0, epilogue.colour, false));
            }
            PushBlend(code, epilogue.blend, epilogue.mask);
        } else if (epilogue.mask == 0xf) {
            code.push_back(U8OutputCopy());
        } else {
            code.push_back(Limm(2, 1, U8ByteMask(epilogue.mask), false));
            code.push_back(U8OutputSelect());
        }
    } else if (epilogue.split_alpha) {
        code.push_back(PackInPlace);
        code.push_back(Limm(2, 2, epilogue.colour, false));
        code.push_back(epilogue.split_alpha);
        code.push_back(epilogue.blend);
        code.push_back(Limm(2, 2, 0xff000000u, false));
        code.push_back(BlendSplitSelect());
    } else if (epilogue.blend) {
        code.push_back(PackInPlace);
        if (epilogue.constant) {
            code.push_back(Limm(2, 0, epilogue.colour, false));
        }
        PushBlend(code, epilogue.blend, epilogue.mask);
    } else {
        code.push_back((PackToOutput & ~PackMask) | u64{epilogue.mask} << 34);
    }
    const std::size_t code_words = code.size();
    const u32 primary = backend.PlaceCodePhased(1, std::move(code), phase_starts, g.temp_count);
    const u32 doutu0 = sgxd_program_doutu0(SgxdBackend::HeapCodeBase(1), primary, g.temp_count);
    f.code = {primary, static_cast<u32>(code_words * sizeof(u64)), 0, 0};
    f.secondary_doutu0 = DummyPixelSecondary;
    if (!g.secondary.empty()) {
        const u32 secondary = backend.PlaceCode(1, g.secondary);
        f.secondary_doutu0 = sgxd_program_doutu0(SgxdBackend::HeapCodeBase(1), secondary, 0);
        f.code[2] = secondary;
        f.code[3] = static_cast<u32>(g.secondary.size() * sizeof(u64));
    }
    f.primary_doutu0 = doutu0;

    ASSERT_MSG(g.inputs.size() <= f.iterators.size(),
               "sgxd: fragment program {:016x} has {} inputs (input:components):{}, {} at most",
               hash, g.inputs.size(),
               [&] {
                   std::string list;
                   for (const Usse::Varying& in : g.inputs) {
                       list += fmt::format(" {:x}:{}{}", static_cast<u32>(in.input),
                                           in.components, in.half ? "h" : "");
                   }
                   return list;
               }(),
               f.iterators.size());
    u32 iterated = 0;
    for (size_t i = 0; i < g.inputs.size(); i++) {
        f.iterators[i] = Usse::VaryingDescriptor(g, i);
        f.iterator_words[i] = Usse::VaryingWords(g.inputs[i]);
        iterated += Usse::VaryingWords(g.inputs[i]);
    }
    f.iterator_count = static_cast<u32>(g.inputs.size());
    // The pack reads pa0 and pa1 (four F16); the split blend's scratch is pa1 and pa2.
    f.pixel_pa = std::max<u32>({g.pa_count, iterated, epilogue.split_alpha ? 3u : 2u});
    LOG_INFO(Render,
             "fragment program {:016x}, blend {:016x}: {} words, {} inputs, {} PAs, {} temps, "
             "{} uniform floats, {} literals{}",
             hash, epilogue.blend, code_words, g.inputs.size(), f.pixel_pa, g.temp_count,
             g.uniform_floats, g.literals.size(),
             emit_us != 0 ? fmt::format(", emitted in {} us", emit_us) : "");
    return f;
}

namespace {

// The LUT rows as the shaders sample them: (value, difference) F32F32 texels. Multiplying by
// reciprocals (within one ulp of the quotients) avoids VFP's unpipelined vdiv.

/// The LutEntry: the value 0.12 fixed point in bits 0-11, the difference's magnitude 0.11 in
/// bits 12-22 and its sign in bit 23.
void LightingLutRow(float* out, const Pica::PicaCore::Lighting::LutEntry* lut, u32 count) {
    static_assert(sizeof(*lut) == sizeof(u32));
    DEBUG_ASSERT(count % 4 == 0);
    const float32x4_t value_scale = vdupq_n_f32(1.0f / 4095.0f);
    const float32x4_t diff_scale = vdupq_n_f32(1.0f / 2047.0f);
    for (u32 i = 0; i < count; i += 4) {
        const uint32x4_t raw = vld1q_u32(&lut[i].raw);
        const float32x4_t value =
            vmulq_f32(vcvtq_f32_u32(vandq_u32(raw, vdupq_n_u32(0xfff))), value_scale);
        const float32x4_t magnitude = vmulq_f32(
            vcvtq_f32_u32(vandq_u32(vshrq_n_u32(raw, 12), vdupq_n_u32(0x7ff))), diff_scale);
        // Bit 23 moved to the float's sign bit.
        const uint32x4_t sign = vandq_u32(vshlq_n_u32(raw, 8), vdupq_n_u32(0x80000000u));
        const float32x4_t diff =
            vreinterpretq_f32_u32(veorq_u32(vreinterpretq_u32_f32(magnitude), sign));
        const float32x4x2_t texels{{value, diff}};
        vst2q_f32(out + i * 2, texels);
    }
}

u16 ToF16(float value);

/// The lighting values alone as F16, for the filtered reads (SamplerKind::LutLfHalf).
void LightingLutRowHalf(u16* out, const Pica::PicaCore::Lighting::LutEntry* lut, u32 count) {
    for (u32 i = 0; i < count; i++) {
        out[i] = ToF16(static_cast<float>(lut[i].raw & 0xfff) * (1.0f / 4095.0f));
    }
}

/// The fog LutEntry: the difference 1.1.11 two's complement in bits 0-12, the value 0.0.11 in
/// bits 13-23.
void FogLutRow(float* out, const Pica::PicaCore::Fog::LutEntry* lut, u32 count) {
    static_assert(sizeof(*lut) == sizeof(u32));
    DEBUG_ASSERT(count % 4 == 0);
    const float32x4_t scale = vdupq_n_f32(1.0f / 2047.0f);
    for (u32 i = 0; i < count; i += 4) {
        const uint32x4_t raw = vld1q_u32(&lut[i].raw);
        const float32x4_t value = vmulq_f32(
            vcvtq_f32_u32(vandq_u32(vshrq_n_u32(raw, 13), vdupq_n_u32(0x7ff))), scale);
        const int32x4_t difference = vshrq_n_s32(vshlq_n_s32(vreinterpretq_s32_u32(raw), 19), 19);
        const float32x4_t diff = vmulq_f32(vcvtq_f32_s32(difference), scale);
        const float32x4x2_t texels{{value, diff}};
        vst2q_f32(out + i * 2, texels);
    }
}

} // namespace

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
    if (lut_row + needed > (lut_half + 1) * Half) {
        lut_half_mark[lut_half] = backend.PassMark();
        lut_half ^= 1;
        backend.WaitPassMark(lut_half_mark[lut_half], "a LUT ring's half");
        lut_row = lut_half * Half;
        lut_rows_by_contents.Clear();
        lighting.lut_dirty = lighting.LutAllDirty;
        fog.lut_dirty = true;
    }
    const SgxdBackend::Data rows = backend.LutRows();
    const SgxdBackend::Data half_rows = backend.LutLfHalfRows();
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
        const auto& lut = lighting.luts[index];
        const u64 contents = Common::ComputeHash64(lut.data(), sizeof(lut));
        u32 row;
        if (const u32* const held = lut_rows_by_contents.Find(contents)) {
            row = *held;
        } else {
            row = take_row();
            float* const out = reinterpret_cast<float*>(rows.cpu + row * RowBytes);
            LightingLutRow(out, lut.data(), static_cast<u32>(lut.size()));
            LightingLutRowHalf(reinterpret_cast<u16*>(half_rows.cpu) +
                                   row * Pica::Shader::LUT_TEX_WIDTH,
                               lut.data(), static_cast<u32>(lut.size()));
            lut_rows_by_contents.Emplace(contents, u32{row});
        }
        fs_data.lighting_lut_offset[index / 4][index % 4] =
            static_cast<int>(row * Pica::Shader::LUT_TEX_WIDTH);
    }
    if (fog.lut_dirty) {
        fog.lut_dirty = false;
        const u32 row = take_row();
        float* const out = reinterpret_cast<float*>(rows.cpu + row * RowBytes);
        FogLutRow(out, fog.lut.data(), static_cast<u32>(fog.lut.size()));
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

namespace {

// The fog factor table: FogTableTexels x 1 U8888 texels, u the PICA depth: texel j is depth
// (j + 0.5) / FogTableTexels, and a bilinear read between two texels interpolates the factor
// (exact where the LUT is linear between them). A side's size is a power of two.
constexpr u32 FogTableTexels = 1024;
constexpr u32 FogTableBytes = FogTableTexels * 4;
constexpr u32 FogTableSlots = 16;
// U8888, read raw as the stored word, filtered byte by byte.
constexpr unsigned FogTableFormatU8888 = 0x0c;

} // namespace

void RasterizerSgxd::FogTableWords(bool flip, u32 words[4]) {
    static_assert(std::tuple_size_v<decltype(fog_table_pass)> == FogTableSlots);
    if (fog_table_dirty || flip != fog_table_flip) {
        fog_table_dirty = false;
        fog_table_flip = flip;
        const auto& lut = pica.fog.lut;
        const u64 contents =
            Common::HashCombine(Common::ComputeHash64(lut.data(), sizeof(lut)), flip ? 1 : 0);
        if (!fog_tables.cpu) {
            fog_tables = backend.AllocTextureFixed(FogTableSlots * FogTableBytes);
            fog_table_pass.fill(~0u);
        }
        u32 slot;
        if (const u32* const held = fog_tables_by_contents.Find(contents)) {
            slot = *held;
        } else {
            // As SyncLutLf's ring.
            constexpr u32 Half = FogTableSlots / 2;
            if (fog_table_next == (fog_table_half + 1) * Half) {
                fog_table_half_mark[fog_table_half] = backend.PassMark();
                fog_table_half ^= 1;
                backend.WaitPassMark(fog_table_half_mark[fog_table_half],
                                     "a fog table ring's half");
                fog_table_next = fog_table_half * Half;
                fog_tables_by_contents.Clear();
            }
            slot = fog_table_next++;
            DEBUG_ASSERT_MSG(fog_table_pass[slot] != passes,
                             "sgxd: pass {} changed the fog LUT {} times: the fog table ring "
                             "would overwrite a table its own draws read",
                             passes, Half);
            fog_table_pass[slot] = passes;
            // The shader generator's factor: index = depth * 128 (1 - depth flipped), entry floor(index)
            // clamped to [0, 127], value + difference * (index - entry) clamped to [0, 1]; (f, f, f, 1) in 8
            // bits as the program's pack rounds, so the fog's SOP3 reads alpha one.
            u32* const texels = reinterpret_cast<u32*>(fog_tables.cpu + slot * FogTableBytes);
            for (u32 j = 0; j < FogTableTexels; j++) {
                const float depth = (static_cast<float>(j) + 0.5f) / FogTableTexels;
                const float index = (flip ? 1.0f - depth : depth) * 128.0f;
                const u32 entry = std::min<u32>(static_cast<u32>(index), 127);
                const float factor =
                    std::clamp(lut[entry].ToFloat() +
                                   lut[entry].DiffToFloat() * (index - static_cast<float>(entry)),
                               0.0f, 1.0f);
                const u32 byte = static_cast<u32>(std::lround(factor * 255.0f));
                texels[j] = 0xff000000u | byte * 0x010101u;
            }
            fog_tables_by_contents.Emplace(contents, u32{slot});
        }
        sgxd_recipe_texture_words_format(fog_tables.gpu + slot * FogTableBytes, FogTableTexels, 1,
                                         1, FogTableFormatU8888, 0, fog_table_words.data());
    }
    std::memcpy(words, fog_table_words.data(), sizeof(fog_table_words));
}

namespace {

// The procedural texture's table: ProcTexTableTexels x 1 U8888 texels, u the coordinate after its
// clamp: texel j is u = (j + 0.5) / ProcTexTableTexels, bilinear between texels.
constexpr u32 ProcTexTableTexels = 1024;
constexpr u32 ProcTexTableBytes = ProcTexTableTexels * 4;
constexpr u32 ProcTexTableSlots = 16;

} // namespace

void RasterizerSgxd::ProcTexTableWords(u32 words[4]) {
    using Clamp = Pica::TexturingRegs::ProcTexClamp;
    using Combiner = Pica::TexturingRegs::ProcTexCombiner;
    using Filter = Pica::TexturingRegs::ProcTexFilter;
    static_assert(std::tuple_size_v<decltype(proctex_table_pass)> == ProcTexTableSlots);
    const Pica::Shader::ProcTexConfig pt{regs.texturing};
    const std::array<u32, 3> table_regs{pt.raw, static_cast<u32>(pt.lut_width),
                                        static_cast<u32>(pt.lut_offset0)};
    if (proctex_table_dirty || table_regs != proctex_table_regs) {
        proctex_table_dirty = false;
        proctex_table_regs = table_regs;
        const auto& t = pica.proctex;
        u64 contents = Common::ComputeHash64(table_regs.data(), sizeof(table_regs));
        for (const auto [data, bytes] : {std::pair<const void*, std::size_t>{
                                             t.color_map_table.data(), sizeof(t.color_map_table)},
                                         {t.alpha_map_table.data(), sizeof(t.alpha_map_table)},
                                         {t.color_table.data(), sizeof(t.color_table)},
                                         {t.color_diff_table.data(), sizeof(t.color_diff_table)}}) {
            contents = Common::HashCombine(contents, Common::ComputeHash64(data, bytes));
        }
        if (!proctex_tables.cpu) {
            proctex_tables = backend.AllocTextureFixed(ProcTexTableSlots * ProcTexTableBytes);
            proctex_table_pass.fill(~0u);
        }
        u32 slot;
        if (const u32* const held = proctex_tables_by_contents.Find(contents)) {
            slot = *held;
        } else {
            // As SyncLutLf's ring.
            constexpr u32 Half = ProcTexTableSlots / 2;
            if (proctex_table_next == (proctex_table_half + 1) * Half) {
                proctex_table_half_mark[proctex_table_half] = backend.PassMark();
                proctex_table_half ^= 1;
                backend.WaitPassMark(proctex_table_half_mark[proctex_table_half],
                                     "a proctex table ring's half");
                proctex_table_next = proctex_table_half * Half;
                proctex_tables_by_contents.Clear();
            }
            slot = proctex_table_next++;
            DEBUG_ASSERT_MSG(proctex_table_pass[slot] != passes,
                             "sgxd: pass {} changed the proctex tables {} times: the table ring "
                             "would overwrite a table its own draws read",
                             passes, Half);
            proctex_table_pass[slot] = passes;
            // The shader generator's ProcTex() at a clamped u: the combiner, the map's entry
            // interpolated by its slope, the colour table's entry at that (level 0).
            const auto map = [](const auto& table, Combiner combiner, float u) {
                const float coord = (combiner == Combiner::U2 ? u * u : u) * 128.0f;
                const u32 entry = std::min<u32>(static_cast<u32>(coord), 127);
                return std::clamp(table[entry].ToFloat() + table[entry].DiffToFloat() *
                                                               (coord - static_cast<float>(entry)),
                                  0.0f, 1.0f);
            };
            const bool linear = pt.lut_filter == Filter::Linear;
            u32* const texels =
                reinterpret_cast<u32*>(proctex_tables.cpu + slot * ProcTexTableBytes);
            for (u32 j = 0; j < ProcTexTableTexels; j++) {
                const float u = (static_cast<float>(j) + 0.5f) / ProcTexTableTexels;
                const float coord = map(t.color_map_table, pt.color_combiner, u) *
                                    static_cast<float>(pt.lut_width - 1);
                const float at = linear ? std::floor(coord) : std::round(coord);
                const u32 entry =
                    static_cast<u32>(std::clamp(static_cast<s32>(at) + pt.lut_offset0, 0, 255));
                const auto colour = t.color_table[entry].ToVector();
                const auto slope = t.color_diff_table[entry].ToVector();
                std::array<float, 4> value;
                for (u32 c = 0; c < 4; c++) {
                    value[c] = (static_cast<float>(colour[c]) +
                                (linear ? (coord - at) * static_cast<float>(slope[c]) : 0.0f)) /
                               255.0f;
                }
                if (pt.separate_alpha) {
                    value[3] = map(t.alpha_map_table, pt.alpha_combiner, u);
                }
                u32 texel = 0;
                for (u32 c = 0; c < 4; c++) {
                    texel |=
                        static_cast<u32>(std::lround(std::clamp(value[c], 0.0f, 1.0f) * 255.0f))
                        << (c * 8);
                }
                texels[j] = texel;
            }
            proctex_tables_by_contents.Emplace(contents, u32{slot});
        }
        sgxd_recipe_texture_words_format(proctex_tables.gpu + slot * ProcTexTableBytes,
                                         ProcTexTableTexels, 1, 1, FogTableFormatU8888, 0,
                                         proctex_table_words.data());
        // u's address mode: MIRROR_CLAMP reads min(|u|, 1), MIRROR the mirrored repeat.
        proctex_table_words[0] =
            (proctex_table_words[0] & ~(7u << 6)) | (pt.u_clamp == Clamp::ToEdge ? 3u : 1u) << 6;
    }
    std::memcpy(words, proctex_table_words.data(), sizeof(proctex_table_words));
}

u32 RasterizerSgxd::NoiseTable() {
    if (!noise_table.cpu) {
        // NoiseRand1D and NoiseRand2D: NoiseRand1D's period is 144, so x, y < 256 cover the
        // point mod 144 and the next.
        const auto rand1d = [](u32 v) {
            static constexpr u32 table[16] = {0, 4, 10, 8, 4, 9, 7, 12, 5, 15, 13, 14, 11, 15, 2, 11};
            return ((v % 9 + 2) * 3 & 0xF) ^ table[(v / 9) & 0xF];
        };
        static constexpr u32 table2[16] = {10, 2, 15, 8, 0, 7, 4, 5, 5, 13, 2, 6, 13, 9, 3, 14};
        // F16x4 texels, R first: h * (x odd), h * (y odd), h, 0, for the program's bilinear read.
        noise_table = backend.AllocTextureFixed(256 * 256 * 8);
        u16* const texels = reinterpret_cast<u16*>(noise_table.cpu);
        for (u32 y = 0; y < 256; y++) {
            for (u32 x = 0; x < 256; x++) {
                const u32 u2 = rand1d(x);
                u32 v2 = rand1d(y);
                v2 += (u2 & 3) == 1 ? 4 : 0;
                v2 ^= (u2 & 1) * 6;
                v2 += 10 + u2;
                v2 &= 0xF;
                v2 ^= table2[u2];
                const float h = -1.0f + static_cast<float>(v2) * 2.0f / 15.0f;
                u16* const t = texels + (y * 256 + x) * 4;
                t[0] = ToF16((x & 1) ? h : 0.0f);
                t[1] = ToF16((y & 1) ? h : 0.0f);
                t[2] = ToF16(h);
                t[3] = 0;
            }
        }
    }
    return noise_table.gpu;
}

void RasterizerSgxd::SyncLutProcTex() {
    auto& proctex = pica.proctex;
    if (!proctex.table_dirty) {
        return;
    }
    // As SyncLutLf: both rings in halves, and a move of either moves both.
    constexpr u32 RgHalf = Pica::Shader::LUT_RG_ROWS / 2;
    constexpr u32 RgbaHalf = Pica::Shader::LUT_RGBA_ROWS / 2;
    const u32 rg_needed = (proctex.noise_lut_dirty ? 1u : 0u) +
                          (proctex.color_map_dirty ? 1u : 0u) + (proctex.alpha_map_dirty ? 1u : 0u);
    const u32 rgba_needed = (proctex.lut_dirty ? 1u : 0u) + (proctex.diff_lut_dirty ? 1u : 0u);
    if (lut_rg_row + rg_needed > (proctex_half + 1) * RgHalf ||
        lut_rgba_row + rgba_needed > (proctex_half + 1) * RgbaHalf) {
        proctex_half_mark[proctex_half] = backend.PassMark();
        proctex_half ^= 1;
        backend.WaitPassMark(proctex_half_mark[proctex_half], "a proctex ring's half");
        lut_rg_row = proctex_half * RgHalf;
        lut_rgba_row = proctex_half * RgbaHalf;
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
    if (proctex.noise_lut_dirty) {
        sync_value(proctex.noise_table, fs_data.proctex_noise_lut_offset);
    }
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
#ifdef VITA_DIAGNOSTICS
    {
        const auto& fb = regs.framebuffer.framebuffer;
    }
#endif
    const bool drawn = Draw(is_indexed, nullptr, 0);
    scanned_indices.reset();
    gs_indices_set = false;
    return drawn;
}

RasterizerSgxd::VertexLayout& RasterizerSgxd::VertexLayoutFor() {
    if (layout_regs_dirty) {
        layout_regs_dirty = false;
        layout_regs_hash = StateRegs::Hash(StateRegs::Layout, regs.reg_array);
    }
    if (!last_layout || layout_regs_hash != last_layout_regs) {
        last_layout_regs = layout_regs_hash;
        last_layout = vertex_layouts.Find(layout_regs_hash);
        if (!last_layout) {
            if (vertex_layouts.Size() >= MaxVertexLayouts) {
                vertex_layouts.Clear();
            }
            last_layout = &vertex_layouts.Emplace(layout_regs_hash, DecodeVertexLayout(regs));
        }
    }
    return *last_layout;
}

RasterizerSgxd::VertexLayout RasterizerSgxd::DecodeVertexLayout(const Pica::RegsInternal& regs) {
    const auto& va = regs.pipeline.vertex_attributes;
    VertexLayout layout{};
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
            using AttributeFormat = Pica::PipelineRegs::VertexAttributeFormat;
            const auto format = va.GetFormat(attribute_index);
            // Integer attributes are unpacked by the program: fetched as the words they start. The DMA reads
            // from any byte address at any stride.
            const bool is_s8 = format == AttributeFormat::BYTE;
            const bool is_s16 = format == AttributeFormat::SHORT;
            const bool is_u8 = format == AttributeFormat::UBYTE || is_s8 || is_s16;
            const u32 reg = regs.vs.GetRegisterForAttribute(attribute_index);
            const VertexLayout::Attribute attribute{
                static_cast<u8>(reg), static_cast<u8>(loader_index), offset,
                static_cast<u32>(loader.byte_count),
                is_s16 ? (elements + 1) / 2 : is_u8 ? 1u : elements};
            const auto mark = [reg](u16& mask, bool on) {
                mask = static_cast<u16>(on ? mask | 1u << reg : mask & ~(1u << reg));
            };
            mark(layout.u8_regs, is_u8);
            mark(layout.s8_regs, is_s8);
            mark(layout.s16_regs, is_s16);
            // Two loaders feeding one register is a guest bug; the last wins, as in GL.
            const auto same =
                std::find_if(layout.attributes.begin(), layout.attributes.begin() + layout.count,
                             [&](const VertexLayout::Attribute& a) { return a.reg == reg; });
            if (same != layout.attributes.begin() + layout.count) {
                *same = attribute;
            } else {
                layout.attributes[layout.count++] = attribute;
            }
            layout.enabled_regs |= static_cast<u16>(1u << reg);
            layout.reg_components[reg] = static_cast<u8>(elements);
            offset += va.GetStride(attribute_index);
        }
    }
    return layout;
}

std::array<u32, RasterizerSgxd::VertexLayout::ProgramKeyWords> RasterizerSgxd::VertexProgramKey(
    const Pica::RegsInternal& regs, Pica::ShaderSetup& setup, Usse::VsMode mode) {
    // What PicaVSConfig and VertexProgramFor read past the layout: the code and swizzles (hashed only
    // when they change), the entry, lighting, the output mask and attributes, the mode and the bools.
    const auto& reg_array = regs.reg_array;
    const u64 code = setup.GetProgramCodeHash();
    const u64 swizzles = setup.GetSwizzleDataHash();
    std::array<u32, VertexLayout::ProgramKeyWords> key;
    key[0] = static_cast<u32>(code);
    key[1] = static_cast<u32>(code >> 32);
    key[2] = static_cast<u32>(swizzles);
    key[3] = static_cast<u32>(swizzles >> 32);
    key[4] = reg_array[PICA_REG_INDEX(vs.main_offset)];
    key[5] = reg_array[PICA_REG_INDEX(lighting.disable)];
    key[6] = reg_array[PICA_REG_INDEX(vs.output_mask)];
    key[7] = reg_array[PICA_REG_INDEX(rasterizer.vs_output_total)];
    for (u32 n = 0; n < 7; n++) {
        key[8 + n] = reg_array[PICA_REG_INDEX(rasterizer.vs_output_attributes) + n];
    }
    u32 bools = 0;
    for (u32 n = 0; n < 16; n++) {
        bools |= (setup.uniforms.b[n] ? 1u : 0u) << n;
    }
    key[15] = static_cast<u32>(mode);
    key[16] = bools;
    return key;
}

RasterizerSgxd::StorePart RasterizerSgxd::ViewportPart(const ColourBuffer& target) const {
    const auto& rs = regs.rasterizer;
    const s32 vw = static_cast<s32>(f24::FromRaw(rs.viewport_size_x).ToFloat32() * 2);
    const s32 vh = static_cast<s32>(f24::FromRaw(rs.viewport_size_y).ToFloat32() * 2);
    // The target is the buffer transposed: rows corner.y .. + vh are columns H - corner.y - vh ..
    // H - corner.y, and the PICA's columns are its rows. A narrow buffer is drawn into its scratch.
    const s32 w = static_cast<s32>(target.width), h = static_cast<s32>(target.height);
    const s32 cx = rs.viewport_corner.x, cy = rs.viewport_corner.y;
    StorePart p{static_cast<u32>(std::clamp(h - cy - vh, 0, h)),
                static_cast<u32>(std::clamp(h - cy, 0, h)), static_cast<u32>(std::clamp(cx, 0, w)),
                static_cast<u32>(std::clamp(cx + vw, 0, w))};
    const u32 scratch = NarrowWidth(target);
    if (scratch != 0) {
        p.y0 = 0;
        p.y1 = scratch;
    }
    if (p.x0 == 0 && p.x1 == static_cast<u32>(h) && p.y0 == 0 &&
        p.y1 == (scratch != 0 ? scratch : static_cast<u32>(w))) {
        return {};
    }
    return p;
}

bool RasterizerSgxd::OnStoreGrid(const StorePart& p) {
    // An empty part (a viewport past the buffer) keeps nothing.
    return p.x1 == 0 || (p.x0 < p.x1 && p.y0 < p.y1 && p.x0 % 8 == 0 && p.x1 % 8 == 0 &&
                         p.y0 % 16 == 0 && p.y1 % 8 == 0);
}

bool RasterizerSgxd::KeepPassForViewport(const ColourBuffer& target) {
    // A pass storing a part of its buffer, or all of it, keeps its draws to their viewports by the part
    // or by the ISP's mask over the margin (MaskOutside). A draw whose viewport lies inside that region
    // clears the mask outside its own; the region only narrows, so a wider viewport ends the pass.
    // Scaled and widened passes, and a narrow buffer's scratch, keep to their parts.
    if (pass_quarters != 4 || pass_margin != 0 || pass_narrow) {
        return false;
    }
    const StorePart view = ViewportPart(target);
    if (view.x1 == 0) {
        return false;
    }
    const StorePart whole{0, target.height, 0, std::max(target.width, 32u)};
    const StorePart region = pass_mask.x1 != 0   ? pass_mask
                             : pass_part.x1 != 0 ? pass_part
                                                 : whole;
    if (view.x0 < region.x0 || view.x1 > region.x1 || view.y0 < region.y0 ||
        view.y1 > region.y1 || view.x0 >= view.x1 || view.y0 >= view.y1) {
        return false;
    }
    if (view != region) {
        // In a pass masked to its part the draws stay in the part's tiles, and the narrower mask with them.
        backend.MaskOutside(PassX(static_cast<s32>(view.x0)), PassX(static_cast<s32>(view.x1)),
                            PassEdge(static_cast<s32>(view.y0)),
                            PassEdge(static_cast<s32>(view.y1)), pass_margin_masked);
        pass_mask = view;
#ifdef VITA_DIAGNOSTICS
        viewport_masks++;
#endif
    }
    return true;
}

RasterizerSgxd::StorePart RasterizerSgxd::StorePartFor(const ColourBuffer& target) const {
    // The PICA draws inside its viewport alone; the SGX clips at a guard band, so a viewport smaller
    // than the buffer stores only its part of the target (SgxdPicaStore's part) and guest memory keeps
    // the margin's bytes. TA clip planes at the viewport's edges would lock up the MTE's clipper
    // (docs/sgxd/ta.md). A part off the strips' grid is kept by the ISP's mask (MaskOutside).
    const StorePart p = ViewportPart(target);
    return OnStoreGrid(p) ? p : StorePart{};
}

namespace {
void WidenIndices(u16* out, const u8* in, u32 count) {
    u32 n = 0;
    for (; n + 16 <= count; n += 16) {
        const uint8x16_t bytes = vld1q_u8(in + n);
        vst1q_u16(out + n, vmovl_u8(vget_low_u8(bytes)));
        vst1q_u16(out + n + 8, vmovl_u8(vget_high_u8(bytes)));
    }
    for (; n < count; n++) {
        out[n] = in[n];
    }
}
} // Anonymous namespace

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
    u16 u8_regs = 0, s8_regs = 0, s16_regs = 0;
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
    // The format decoded once (VertexLayoutFor); the addresses added mesh by mesh.
    VertexLayout* layout = nullptr;
    if (!immediate) {
        layout = &VertexLayoutFor();
        for (u32 n = 0; n < layout->count; n++) {
            const VertexLayout::Attribute& a = layout->attributes[n];
            feeds[feed_count++] = {a.reg, base + va.attribute_loaders[a.loader].data_offset + a.offset,
                                   a.stride, a.words};
        }
        enabled_regs = layout->enabled_regs;
        u8_regs = layout->u8_regs;
        s8_regs = layout->s8_regs;
        s16_regs = layout->s16_regs;
        reg_components = layout->reg_components;
    }
    // Every register no loader feeds is a uniform: the fixed default attribute where the guest set
    // one, (0, 0, 0, 1) otherwise.
    const u16 default_regs = static_cast<u16>(~enabled_regs);
    // The program the layout resolved to last, while its other inputs are the same.
    const VertexProgram* program = nullptr;
    std::array<u32, VertexLayout::ProgramKeyWords> program_key{};
    if (layout) {
        program_key = VertexProgramKey(regs, pica.vs_setup, mode);
        if (layout->program && layout->program->tier2_queued && layout->program_key == program_key) {
            program = layout->program;
        }
    }
    if (!program) {
        program = &VertexProgramFor(default_regs, reg_components, u8_regs, s8_regs, s16_regs, mode);
        if (layout) {
            layout->program_key = program_key;
            layout->program = program;
        }
    }
    const VertexProgram& vp = *program;
    const Usse::Program& vg = vp.usse;

    // --- the index list ---
    // The VDM reads 16-bit indices where the guest put them; it has no 8-bit format, so 8-bit indices
    // are widened by the render thread into the pass data. The vertex range is what the cache clean
    // needs.
    u32 indices_gpu = 0;
    u32 vertex_min = immediate ? 0 : pipeline.vertex_offset;
    u32 vertex_max = vertex_min + count - 1;
    // A draw's vertex range only decides which of its bytes in cached FCRAM to clean (and record for
    // the VBlank's move to CDRAM). Where every vertex an index can reach is uncached already (VRAM, or
    // 8-bit indices into moved FCRAM pages), no index is read: uncached reads are slow.
    bool scan = true;
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
        const bool indices_uncached = guest_memory.IsUncached(index_address, index_bytes);
        const u32 reach = index_u16 ? 0xffffu : 0xffu; // the highest index there can be
        if (mode == Usse::VsMode::Draw && indices_uncached) {
            scan = false;
            for (u32 n = 0; n < feed_count && !scan; n++) {
                const Feed& f = feeds[n];
                if ((vp.input_regs & (1u << f.reg)) == 0) {
                    continue;
                }
                const bool vram =
                    f.address >= Memory::VRAM_PADDR && f.address < Memory::VRAM_PADDR_END;
                scan = !vram && (index_u16 || !guest_memory.IsUncached(
                                                  f.address, reach * f.stride + f.words * 4));
            }
        }
        if (!scan) {
            // The range unread: every vertex the indices can reach.
            vertex_min = 0;
            vertex_max = reach;
        }
        if (index_u16) {
            DEBUG_ASSERT_MSG(gs_indices_set || (index_address & 1) == 0,
                             "sgxd: {}: 16-bit indices at {:#x}", describe(), index_address);
            if (scan && scanned_indices) {
                vertex_min = scanned_indices->first;
                vertex_max = scanned_indices->second;
            } else if (scan) {
                const auto [lo, hi] = Common::FindMinMax(
                    std::span<const u16>{reinterpret_cast<const u16*>(index_scan), count});
                vertex_min = lo;
                vertex_max = hi;
            }
            // The GPU reads them for a draw alone: a geometry draw's stages make their own lists.
            if (gs_indices_set) {
            } else if (indices_uncached) {
                indices_gpu = index_address;
                SgxdCacheSkipped(CacheSite::Vertices, index_bytes);
            } else {
                indices_gpu = index_address;
                backend.ReadsCached(index_cpu, index_bytes);
                CachedRead(index_address, index_bytes);
            }
        } else {
            if (scan) {
                const auto [lo, hi] = Common::FindMinMax(std::span<const u8>{index_scan, count});
                vertex_min = lo;
                vertex_max = hi;
            }
            // Widened here, into the pass data: a side render for it would make the pass's TA wait.
            if (mode == Usse::VsMode::Draw) {
#ifdef VITA_DIAGNOSTICS
                const u32 c0 = azaharCyclesStart();
#endif
                const SgxdBackend::Data wide = backend.AllocData(count * 2, 16);
                WidenIndices(reinterpret_cast<u16*>(wide.cpu), index_scan, count);
                indices_gpu = wide.gpu;
#ifdef VITA_DIAGNOSTICS
                widen_stats.cycles += azaharCycles() - c0;
                const bool vram =
                    index_address >= Memory::VRAM_PADDR && index_address < Memory::VRAM_PADDR_END;
                widen_stats.bytes[vram ? 0 : indices_uncached ? 1 : 2] += count;
                widen_stats.lists++;
#endif
            }
        }
    }

    VertexStage stage;
    std::array<SgxdVertexFetch, SGXD_PROGRAM_MAX_FETCH>& fetch = stage.fetch;
    u32 fetch_count = 0;
    const u32 first = mode != Usse::VsMode::Draw ? vertex_min
                     : is_indexed                ? 0
                                                 : pipeline.vertex_offset;
    for (u32 n = 0; n < feed_count; n++) {
        const Feed& f = feeds[n];
        if ((vp.input_regs & (1u << f.reg)) == 0) {
            continue;
        }
        const u32 resource = vp.attr_resource[f.reg];
        DEBUG_ASSERT_MSG(resource != ~0u, "sgxd: {}: the program reads vs_in_attr{} but has no "
                         "such attribute",
                         describe(), f.reg);
        DEBUG_ASSERT(fetch_count < fetch.size());
        // The identity index list counts from 0, the first vertex in the address; a guest list indexes
        // from the loader's address.
        const PAddr address = immediate ? f.address : f.address + first * f.stride;
        fetch[fetch_count++] = {address, f.stride, f.words, resource};
        // Unscanned, every vertex the indices can reach is uncached (above).
        if (immediate || !scan) {
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


    stage.vp = &vp;
    stage.fetch_count = fetch_count;
    stage.indices = mode == Usse::VsMode::Draw ? indices_gpu : 0;
    stage.count = count;
    stage.vertex_min = vertex_min;
    stage.vertex_max = vertex_max;
    stage.setup = &pica.vs_setup;
    return stage;
}

const std::array<s8, 16>& RasterizerSgxd::DefaultInputAttributes() {
    const auto& va = regs.pipeline.vertex_attributes;
    const std::array<u32, 3> key{va.attribute_mask.Value(),
                                 regs.vs.input_attribute_to_register_map_low,
                                 regs.vs.input_attribute_to_register_map_high};
    if (!default_input_valid || key != default_input_key) {
        default_input_key = key;
        default_input_valid = true;
        default_input_attr.fill(-1);
        // In attribute order: a later attribute sent to the same register wins.
        for (u32 attr = 0; attr < 16; attr++) {
            if (va.IsDefaultAttribute(attr)) {
                default_input_attr[regs.vs.GetRegisterForAttribute(attr)] = static_cast<s8>(attr);
            }
        }
    }
    return default_input_attr;
}

SgxdBackend::Data RasterizerSgxd::VertexSa(const VertexProgram& vp,
                                          const Pica::ShaderSetup& setup, u32& vsec_sa,
                                          const Describe& describe) {
    const Usse::Program& vg = vp.usse;
    // The image is put together in cached memory and copied once (the pass data is uncached CDRAM).
    // The program's scratch holds the fixed part; each draw writes its values over the same words.
    vsec_sa = vp.sa_words;
    DEBUG_ASSERT(vp.sa_scratch.size() == vsec_sa);
    float* const vsa_f = vp.sa_scratch.data();
    const auto& uniforms = setup.uniforms;
    if (vg.buffer_pointer) {
        const u32 pointer = BankBuffer(vp, setup) + static_cast<u32>(vg.ldst_base_value);
        std::memcpy(vsa_f + vg.uniform_floats, &pointer, 4);
    }
    const std::array<s8, 16>* default_attrs = nullptr;
    using Kind = Usse::UniformKind;
    for (const VertexSaSlot& slot : vp.sa_slots) {
        float* const out = vsa_f + slot.sa;
        switch (slot.kind) {
        case Kind::FlipViewport:
            out[0] = vs_data.flip_viewport ? 1.0f : 0.0f;
            break;
        case Kind::DepthScale:
            out[0] = fs_data.depth_scale;
            break;
        case Kind::DepthOffset:
            out[0] = fs_data.depth_offset;
            break;
        case Kind::IntUniforms:
            for (u32 n = 0; n < 4 && n < slot.array_size; n++) {
                for (u32 c = 0; c < 4; c++) {
                    out[n * 4 + c] = static_cast<float>(uniforms.i[n][c]);
                }
            }
            break;
        case Kind::DefaultInput: {
            float value[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            if (default_attrs == nullptr) {
                default_attrs = &DefaultInputAttributes();
            }
            DEBUG_ASSERT(slot.index < 16);
            if (const s8 attr = (*default_attrs)[slot.index]; attr >= 0) {
                for (u32 c = 0; c < 4; c++) {
                    value[c] = pica.input_default_attributes[attr][c].ToFloat32();
                }
            }
            PutVec(out, value, slot.components);
            break;
        }
        default:
            UNREACHABLE_MSG("sgxd: {}: the vertex uniform {} has no value", describe(),
                            Usse::UniformName(vg.uniforms[slot.uniform]));
        }
    }
    for (const auto& [sa, row] : vg.prefetched_uniforms) {
        DEBUG_ASSERT(sa + 4 <= vsec_sa && row < 128);
        CopyUniformRows(vsa_f + sa, row, 1, uniforms);
    }
    // The program's last image in this pass again: that copy, whose secondary PDS program the backend
    // still has.
    const u32 pass = backend.DataOpens();
    if (vp.sa_last_pass == pass && vp.sa_last.size() == vsec_sa &&
        std::memcmp(vp.sa_last.data(), vsa_f, vsec_sa * 4) == 0) {
        return vp.sa_last_data;
    }
    const SgxdBackend::Data vsa = backend.AllocData(vsec_sa * 4);
    std::memcpy(vsa.cpu, vsa_f, vsec_sa * 4);
    vp.sa_last.assign(vsa_f, vsa_f + vsec_sa);
    vp.sa_last_pass = pass;
    vp.sa_last_data = vsa;
    return vsa;
}

u32 RasterizerSgxd::BankBuffer(const VertexProgram& vp, const Pica::ShaderSetup& setup) {
    const Usse::Program& vg = vp.usse;
    const u32 pass = backend.DataOpens();
    const Pica::ShaderSetup* const from = vp.has_bank ? &setup : nullptr;
    const u32 version = vp.has_bank ? setup.f_version : 0;
    const u32 offset = vp.has_bank ? vp.bank_offset : 0;
    const u32 rows = vp.has_bank ? vp.bank_rows : 0;
    for (const BankUpload& u : bank_uploads) {
        if (u.gpu != 0 && u.pass == pass && u.setup == from && u.version == version &&
            u.floats == vg.buffer_floats && u.offset == offset && u.rows == rows) {
            return u.gpu;
        }
    }
    // The buffer: zeros, the float bank where uniforms_f is; rows past the 96 real ones read as ones,
    // and rows 0-3 again past 127 (the emitter's wrap mirror).
    const SgxdBackend::Data vbuf = backend.AllocData(vg.buffer_floats * 4);
    float* const buffer = reinterpret_cast<float*>(vbuf.cpu);
    const u32 bank_begin = vp.has_bank ? vp.bank_offset : vg.buffer_floats;
    const u32 bank_end = vp.has_bank ? vp.bank_offset + vp.bank_rows * 4 : vg.buffer_floats;
    std::memset(buffer, 0, bank_begin * 4);
    std::memset(buffer + bank_end, 0, (vg.buffer_floats - bank_end) * 4);
    if (vp.has_bank) {
        float* const bank = buffer + vp.bank_offset;
        const u32 low = std::min(vp.bank_rows, 128u);
        CopyUniformRows(bank, 0, low, setup.uniforms);
        if (vp.bank_rows > 128) {
            CopyUniformRows(bank + 128 * 4, 0, vp.bank_rows - 128, setup.uniforms);
        }
    }
    bank_uploads[bank_upload_next] = {pass, from, version, vg.buffer_floats, offset, rows, vbuf.gpu};
    bank_upload_next = (bank_upload_next + 1) % bank_uploads.size();
    return vbuf.gpu;
}

void RasterizerSgxd::VertexPrograms(const VertexStage& stage, const SgxdBackend::Data& vsa,
                                   u32 vsec_sa, SgxdRecipeDrawPrograms& p,
                                   const Describe& describe) {
    const VertexProgram& vp = *stage.vp;
    const Usse::Program& vg = vp.usse;
    const auto vspds = backend.VertexPds(stage.fetch.data(), stage.fetch_count,
                                         vp.primary_doutu0, vg.per_instance, stage.fresh);
    DEBUG_ASSERT_MSG(vspds.va != 0, "sgxd: {}: {} attributes the vertex PDS does not take",
                     describe(), stage.fetch_count);
    p.vspds = vspds.va;
    p.vspds_units = vspds.units;
    const auto vsec = backend.SecondaryPds(vsa.gpu, vsec_sa, vp.secondary_doutu0);
    DEBUG_ASSERT(vsec.va != 0);
    p.vsec = vsec.va;
    p.vsec_units = vsec.units;
    p.vsec_sa = vsec_sa;
    // Its VDM words follow from the program and the SA size alone: worked out once.
    if (vp.vdm_sa != vsec_sa) {
        sgxd_program_vertex_dms(vg.pa_count, vg.temp_count, vsec_sa, vg.outselects >> 24,
                                &vp.vdm_words[0], &vp.vdm_words[1], &vp.vdm_words[2]);
        vp.vdm_sa = vsec_sa;
    }
    p.vdm_w2 = vp.vdm_words[0];
    p.vdm_w4 = vp.vdm_words[1];
    p.vdm_w5 = vp.vdm_words[2];
    p.outselects = vg.outselects;
    p.texsize = vg.texsize;
    p.indices = stage.indices;
}

bool RasterizerSgxd::QuadVertices(bool is_indexed, const float* immediate, u32 count,
                                 std::array<Pica::OutputVertex, 6>& out) {
    QuadRun& r = quad_run;
    if (r.serial == draw_serial && r.indexed == is_indexed && r.immediate == immediate &&
        r.count == count) {
        out = r.out;
        return r.ok;
    }
    r = QuadRun{draw_serial, is_indexed, immediate, count, false, {}};
    r.ok = RunVertices(is_indexed, immediate, count, std::span(r.out).first(count));
    out = r.out;
    return r.ok;
}

bool RasterizerSgxd::RunVertices(bool is_indexed, const float* immediate, u32 count,
                                 std::span<Pica::OutputVertex> out) {
    const auto& pipeline = regs.pipeline;
    if (!cover_engine) {
        cover_engine = Pica::CreateEngine(false);
    }
    cover_engine->SetupBatch(pica.vs_setup, regs.vs.main_offset);
    Pica::ShaderUnit unit;
    const Pica::VertexLoader loader(memory, pipeline);
    const PAddr base = pipeline.vertex_attributes.GetPhysicalBaseAddress();
    const u8* const indices = memory.GetPhysicalPointer(base + pipeline.index_array.offset);
    const u32 attributes = regs.vs.max_input_attribute_index + 1;
    for (u32 j = 0; j < count; j++) {
        Pica::AttributeBuffer input{}, output{};
        if (immediate) {
            for (u32 a = 0; a < attributes; a++) {
                for (u32 c = 0; c < 4; c++) {
                    input[a][c] = f24::FromFloat32(immediate[(j * attributes + a) * 4 + c]);
                }
            }
        } else {
            if (is_indexed && indices == nullptr) {
                return false;
            }
            const u32 vertex = !is_indexed ? j + pipeline.vertex_offset
                               : pipeline.index_array.format != 0
                                   ? reinterpret_cast<const u16*>(indices)[j]
                                   : indices[j];
            loader.LoadVertex(base, j, vertex, input, pica.input_default_attributes);
        }
        unit.LoadInput(regs.vs, input);
        cover_engine->Run(pica.vs_setup, unit);
        unit.WriteOutput(regs.vs, output);
        out[j] = Pica::OutputVertex(regs.rasterizer, output);
    }
    return true;
}

void RasterizerSgxd::FitWDepth(bool is_indexed, const float* immediate, u32 count, bool grows) {
    [[maybe_unused]] const auto where = [&] {
        return fmt::format("draw {} of the pass onto {:#x}", backend.PassDraws() + 1,
                           pass_colour.addr);
    };
    // The vertex program's z against its w, from the two vertices furthest apart in w; a draw of one w
    // leaves it to the next. A geometry program's positions are not the vertex program's.
    if (regs.pipeline.use_gs == Pica::PipelineRegs::UseGS::Yes) {
        return;
    }
    const u32 n = std::min(count, 6u);
    std::array<Pica::OutputVertex, 6> out;
    if (n < 2 || !RunVertices(is_indexed, immediate, n, std::span(out).first(n))) {
        return;
    }
    u32 lo = 0, hi = 0;
    for (u32 j = 1; j < n; j++) {
        lo = out[j].pos.w.ToFloat32() < out[lo].pos.w.ToFloat32() ? j : lo;
        hi = out[j].pos.w.ToFloat32() > out[hi].pos.w.ToFloat32() ? j : hi;
    }
    const float w0 = out[lo].pos.w.ToFloat32(), w1 = out[hi].pos.w.ToFloat32();
    if (!(w1 - w0 > 1e-3f * std::max(std::abs(w0), std::abs(w1)))) {
        return;
    }
    const float z0 = out[lo].pos.z.ToFloat32(), z1 = out[hi].pos.z.ToFloat32();
    const float a = (z1 - z0) / (w1 - w0);
    const float b = z0 - a * w0;
    for (u32 j = 0; j < n; j++) {
        const float z = out[j].pos.z.ToFloat32(), w = out[j].pos.w.ToFloat32();
        // f24 positions: 16 mantissa bits.
        if (std::abs(z - (a * w + b)) > 1e-3f * (std::abs(z) + std::abs(w))) {
            DEBUG_ASSERT_MSG(false,
                             "sgxd: {}: a W-buffered draw's z is not a * w + b (vertex {}: z {} w "
                             "{}, fitted {} * w + {}): no perspective projection",
                             where(), j, z, w, a, b);
            return;
        }
    }
    const float alpha = fs_data.depth_scale * a + fs_data.depth_offset;
    DEBUG_ASSERT_MSG((alpha > 0.0f) == grows,
                     "sgxd: {}: the W depth {} * w + {} runs against the TA's depth ({})",
                     where(), alpha, fs_data.depth_scale * b, grows ? "1 - 1/w" : "1/w");
    pass_w_map = {true, grows, alpha, fs_data.depth_scale * b, ProjectionNearW(a, b)};
}

bool RasterizerSgxd::MapZOnW(bool is_indexed, const float* immediate, u32 count,
                             float* viewport) {
    [[maybe_unused]] const auto where = [&] {
        return fmt::format("draw {} of the pass onto {:#x}", backend.PassDraws() + 1,
                           pass_colour.addr);
    };
    // The TA's depth is not linear in the PICA's (offset + scale * z / w), so only a draw of one depth
    // has an exact TA depth: of what is drawn, each triangle clipped to the depth range and the buffer.
    // A quad (run on the CPU) of one depth is exact; one whose visible depths lie in the far eighth
    // gets their middle. A larger draw is taken for a layer behind the scene, at FarLayerDepth (a sky
    // projected towards the far plane). Diagnostics builds check the larger draws now and then.
    constexpr u32 CpuVertices = 6;
    constexpr float FarLayerDepth = 1.0f - 1.0f / 4096.0f;
    if (regs.pipeline.use_gs == Pica::PipelineRegs::UseGS::Yes) {
        DEBUG_ASSERT_MSG(false,
                         "sgxd: {}: a geometry program's Z-buffered draw tests depth against the "
                         "TA's W depths",
                         where());
        return false;
    }
    // The visible depths of the CPU-run vertices, lowest and highest (lowest above highest when
    // nothing is drawn). False when the vertices are not in memory.
    auto& out = zw_vertices;
    const auto visible_depths = [&](float& lo, float& hi) {
        out.resize(count);
        if (!RunVertices(is_indexed, immediate, count, out)) {
            return false;
        }
        // The colour buffer in clip space: x / w and y / w within [l, h].
        const auto& rr = regs.rasterizer;
        const float hx = f24::FromRaw(rr.viewport_size_x).ToFloat32();
        const float hy = f24::FromRaw(rr.viewport_size_y).ToFloat32();
        const float xl = -static_cast<float>(rr.viewport_corner.x) / hx - 1.0f;
        const float xh =
            static_cast<float>(static_cast<s32>(regs.framebuffer.framebuffer.GetWidth()) -
                               rr.viewport_corner.x) / hx - 1.0f;
        const float yl = -static_cast<float>(rr.viewport_corner.y) / hy - 1.0f;
        const float yh =
            static_cast<float>(static_cast<s32>(draw_rows) - rr.viewport_corner.y) / hy - 1.0f;
        using Clip = std::array<float, 4>; // x y z w
        const auto planes = std::to_array<std::array<float, 4>>({
            {1.0f, 0.0f, 0.0f, -xl}, {-1.0f, 0.0f, 0.0f, xh}, {0.0f, 1.0f, 0.0f, -yl},
            {0.0f, -1.0f, 0.0f, yh}, {0.0f, 0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f, 1.0f},
        });
        lo = std::numeric_limits<float>::max();
        hi = -std::numeric_limits<float>::max();
        const auto clip_triangle = [&](u32 i0, u32 i1, u32 i2) {
            // Sutherland-Hodgman: three vertices and one more per plane at most.
            std::array<Clip, 9> poly, next;
            u32 n = 0;
            for (const u32 i : {i0, i1, i2}) {
                const auto& p = out[i].pos;
                poly[n++] = {p.x.ToFloat32(), p.y.ToFloat32(), p.z.ToFloat32(), p.w.ToFloat32()};
            }
            for (const auto& pl : planes) {
                const auto dist = [&](const Clip& v) {
                    return pl[0] * v[0] + pl[1] * v[1] + pl[2] * v[2] + pl[3] * v[3];
                };
                u32 m = 0;
                for (u32 k = 0; k < n; k++) {
                    const Clip& a = poly[k];
                    const Clip& b = poly[(k + 1) % n];
                    const float da = dist(a), db = dist(b);
                    if (da >= 0.0f) {
                        next[m++] = a;
                    }
                    if ((da >= 0.0f) != (db >= 0.0f)) {
                        const float t = da / (da - db);
                        next[m++] = {a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1]),
                                     a[2] + t * (b[2] - a[2]), a[3] + t * (b[3] - a[3])};
                    }
                }
                poly = next;
                n = m;
                if (n < 3) {
                    return;
                }
            }
            for (u32 k = 0; k < n; k++) {
                if (poly[k][3] > 0.0f) {
                    const float d =
                        fs_data.depth_offset + fs_data.depth_scale * poly[k][2] / poly[k][3];
                    lo = std::min(lo, d);
                    hi = std::max(hi, d);
                }
            }
        };
        switch (regs.pipeline.triangle_topology) {
        case Pica::PipelineRegs::TriangleTopology::List:
        case Pica::PipelineRegs::TriangleTopology::Shader: // a list, as the primitive assembler
            for (u32 j = 0; j + 2 < count; j += 3) {
                clip_triangle(j, j + 1, j + 2);
            }
            break;
        case Pica::PipelineRegs::TriangleTopology::Strip:
            for (u32 j = 0; j + 2 < count; j++) {
                clip_triangle(j, j + 1, j + 2);
            }
            break;
        case Pica::PipelineRegs::TriangleTopology::Fan:
            for (u32 j = 1; j + 1 < count; j++) {
                clip_triangle(0, j, j + 1);
            }
            break;
        }
        return true;
    };
    [[maybe_unused]] const auto describe = [&](float lo, float hi) {
        std::string list;
        for (u32 j = 0; j < count; j++) {
            const auto& p = out[j].pos;
            list += fmt::format(" ({} {} {} {})", p.x.ToFloat32(), p.y.ToFloat32(),
                                p.z.ToFloat32(), p.w.ToFloat32());
        }
        return fmt::format(
            "{}: a Z-buffered draw of {} vertices, visible depths {} to {}, tests depth against "
            "the TA's W depths: only a quad of one depth, or a layer in the far eighth of the "
            "depth range, is mapped (depth {} + {} * z / w, compare {}, write {}; x y z w:{})",
            where(), count, lo, hi, fs_data.depth_offset, fs_data.depth_scale,
            static_cast<u32>(regs.framebuffer.output_merger.depth_test_func.Value()),
            regs.framebuffer.output_merger.depth_write_enable.Value(), list);
    };
    float d = FarLayerDepth;
    if (count <= CpuVertices) {
        float lo, hi;
        if (count == 0 || !visible_depths(lo, hi)) {
            DEBUG_ASSERT_MSG(false,
                             "sgxd: {}: a Z-buffered draw of {} vertices tests depth against the "
                             "TA's W depths, and its vertices are not in memory",
                             where(), count);
            return false;
        }
        if (lo > hi) {
            // Nothing of it is drawn: any depth will do.
            lo = hi = fs_data.depth_offset + fs_data.depth_scale * out[0].pos.z.ToFloat32() /
                                                 out[0].pos.w.ToFloat32();
        }
        d = (lo + hi) / 2.0f;
        if (hi - lo > 1e-6f * std::max(1.0f, std::abs(d)) && lo < 7.0f / 8.0f) {
            DEBUG_ASSERT_MSG(false, "sgxd: {}", describe(lo, hi));
            return false;
        }
    } else {
#ifdef VITA_DIAGNOSTICS
        // One in 32, so a diagnostics build keeps its speed.
        static u32 checked = 0;
        if (float lo, hi; checked++ % 32 == 0 && visible_depths(lo, hi) && lo <= hi) {
            DEBUG_ASSERT_MSG(lo >= 7.0f / 8.0f, "sgxd: {}", describe(lo, hi));
        }
#endif
    }
    const WDepthMap& m = pass_w_map;
    if (!m.fitted) {
        DEBUG_ASSERT_MSG(false,
                         "sgxd: {}: a Z-buffered draw tests depth against the TA's W depths, "
                         "but no W-buffered draw of theirs was fitted (FitWDepth)",
                         where());
        return false;
    }
    // Where the PICA's W depth equals d. Nearer than every visible w, it is before all of them.
    const float w_at = (d - m.beta) / m.alpha;
    float t;
    if (!(w_at > 0.0f)) {
        t = m.grows ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max();
    } else {
        t = m.grows ? 1.0f - w_depth_k / w_at : w_depth_k / w_at;
    }
    viewport[4] = t;
    viewport[5] = 0.0f;
    return true;
}

u32 RasterizerSgxd::OwnPixelUnits(bool is_indexed, const float* immediate, u32 count) {
    using Topology = Pica::PipelineRegs::TriangleTopology;
    using TextureType = Pica::TexturingRegs::TextureConfig::TextureType;
    using Filter = Pica::TexturingRegs::TextureConfig::TextureFilter;
    const auto& fb = regs.framebuffer.framebuffer;
    if (fb.color_format != FramebufferRegs::ColorFormat::RGBA8) {
        return 0;
    }
    const PAddr colour = fb.GetColorBufferPhysicalAddress();
    const u32 width = fb.GetWidth(), height = fb.GetHeight();
    const auto textures = regs.texturing.GetTextures();
    const auto& lighting = regs.lighting;
    u32 units = 0;
    for (u32 unit = 0; unit < 3; unit++) {
        const auto& t = textures[unit];
        const auto& c = t.config;
        // The lighting's bump and shadow reads take F32 texels from a sampler.
        const bool lit =
            !lighting.disable &&
            (lighting.config0.bump_selector == unit ||
             (lighting.config0.enable_shadow && lighting.config0.shadow_selector == unit));
        if (t.enabled && c.GetPhysicalAddress() == colour && c.width == width &&
            c.height == height && t.format == Pica::TexturingRegs::TextureFormat::RGBA8 &&
            c.mag_filter == Filter::Nearest && c.min_filter == Filter::Nearest &&
            (unit != 0 || c.type == TextureType::Texture2D) && !lit) {
            units |= 1u << unit;
        }
    }
    if (units == 0) {
        return 0;
    }
    const auto& pipeline = regs.pipeline;
    const Topology topology = pipeline.triangle_topology;
    const bool list = topology == Topology::List || topology == Topology::Shader;
    if (pipeline.use_gs != Pica::PipelineRegs::UseGS::No || count != (list ? 6u : 4u) ||
        (!list && topology != Topology::Strip && topology != Topology::Fan)) {
        return 0;
    }
    std::array<Pica::OutputVertex, 6> v;
    if (!QuadVertices(is_indexed, immediate, count, v)) {
        return 0;
    }
    // One w: the coordinates interpolate linearly in the window.
    const float w0 = v[0].pos.w.ToFloat32();
    for (u32 j = 0; j < count; j++) {
        if (!(w0 > 0.0001f) || v[j].pos.w.ToFloat32() != w0) {
            return 0;
        }
    }
    // The PICA reads texel floor(u * width) at the pixel's centre, rows bottom to
    // top in both: the pixel's own when u * width is the window x plus an offset within half a texel,
    // the same at every vertex.
    const auto& rs = regs.rasterizer;
    const float half_x = f24::FromRaw(rs.viewport_size_x).ToFloat32();
    const float half_y = f24::FromRaw(rs.viewport_size_y).ToFloat32();
    const float corner_x = static_cast<float>(rs.viewport_corner.x);
    const float corner_y = static_cast<float>(rs.viewport_corner.y);
    const bool unit2_coord1 = regs.texturing.main_config.texture2_use_coord1 != 0;
    constexpr float same = 1.0f / 64, margin = 1.0f / 32;
    u32 own = 0;
    for (u32 unit = 0; unit < 3; unit++) {
        if (!(units >> unit & 1)) {
            continue;
        }
        float dx0 = 1e9f, dx1 = -1e9f, dy0 = 1e9f, dy1 = -1e9f;
        for (u32 j = 0; j < count; j++) {
            const auto& tc = unit == 0                     ? v[j].tc0
                             : (unit == 1 || unit2_coord1) ? v[j].tc1
                                                           : v[j].tc2;
            const float x = (v[j].pos.x.ToFloat32() / w0 + 1) * half_x + corner_x;
            const float y = (v[j].pos.y.ToFloat32() / w0 + 1) * half_y + corner_y;
            const float dx = tc.u().ToFloat32() * static_cast<float>(width) - x;
            const float dy = tc.v().ToFloat32() * static_cast<float>(height) - y;
            dx0 = std::min(dx0, dx);
            dx1 = std::max(dx1, dx);
            dy0 = std::min(dy0, dy);
            dy1 = std::max(dy1, dy);
        }
        if (dx1 - dx0 < same && dy1 - dy0 < same && dx0 > -0.5f + margin && dx1 < 0.5f - margin &&
            dy0 > -0.5f + margin && dy1 < 0.5f - margin) {
            own |= 1u << unit;
        }
    }
    return own;
}

namespace {
// A stencil test that writes every pixel's stencil from its reference alone, reading none: ALWAYS,
// the depth test ALWAYS or off, and the pass op ZERO or REPLACE through a full write mask.
bool StencilOverwrites(const FramebufferRegs& f) {
    using CF = FramebufferRegs::CompareFunc;
    using SA = FramebufferRegs::StencilAction;
    const auto& om = f.output_merger;
    const auto& st = om.stencil_test;
    return st.func == CF::Always && st.write_mask == 0xff &&
           (!om.depth_test_enable || om.depth_test_func == CF::Always) &&
           (st.action_depth_pass == SA::Zero || st.action_depth_pass == SA::Replace);
}

bool StencilWrites(const FramebufferRegs& f) {
    using SA = FramebufferRegs::StencilAction;
    const auto& st = f.output_merger.stencil_test;
    return st.write_mask != 0 && (st.action_stencil_fail != SA::Keep ||
                                  st.action_depth_fail != SA::Keep ||
                                  st.action_depth_pass != SA::Keep);
}
} // Anonymous namespace

bool RasterizerSgxd::CoversTarget(bool is_indexed, const float* immediate, u32 count, u32 width,
                                 u32 height, const StorePart& part, bool depth) {
    using Topology = Pica::PipelineRegs::TriangleTopology;
    using CullMode = Pica::RasterizerRegs::CullMode;
    const auto& pipeline = regs.pipeline;
    const auto& om = regs.framebuffer.output_merger;
    const auto& fb = regs.framebuffer.framebuffer;
    const auto& rs = regs.rasterizer;
    const Topology topology = pipeline.triangle_topology;
    const bool list = topology == Topology::List || topology == Topology::Shader;
    if (pipeline.use_gs != Pica::PipelineRegs::UseGS::No || count != (list ? 6u : 4u) ||
        (!list && topology != Topology::Strip && topology != Topology::Fan)) {
        return false;
    }
    const bool writes_all = fb.allow_color_write != 0 && om.red_enable && om.green_enable &&
                            om.blue_enable && om.alpha_enable;
    // A blend keeps nothing of the target with ZERO for both destination factors and no source factor
    // of the destination's.
    using BF = FramebufferRegs::BlendFactor;
    const auto of_dest = [](BF f) {
        return f == BF::DestColor || f == BF::OneMinusDestColor || f == BF::DestAlpha ||
               f == BF::OneMinusDestAlpha || f == BF::SourceAlphaSaturate;
    };
    const auto& b = om.alpha_blending;
    const bool blend_reads =
        om.alphablend_enable &&
        (b.factor_dest_rgb != BF::Zero || b.factor_dest_a != BF::Zero ||
         of_dest(b.factor_source_rgb) || of_dest(b.factor_source_a));
    // A test kills a pixel unless it is ALWAYS.
    using CF = FramebufferRegs::CompareFunc;
    const bool depth_kills = om.depth_test_enable && om.depth_test_func != CF::Always;
    const bool stencil_kills = om.stencil_test.enable && om.stencil_test.func != CF::Always;
    const bool reads_target =
        blend_reads || (!om.alphablend_enable && om.logic_op != FramebufferRegs::LogicOp::Copy) ||
        om.alpha_test.enable || depth_kills || stencil_kills;
    // Every pixel's depth written, nothing killing a pixel first, and the stencil written whole or not
    // tested.
    const bool depth_kept = om.alpha_test.enable || depth_kills ||
                            (om.stencil_test.enable && !StencilOverwrites(regs.framebuffer));
    if (depth ? depth_kept : (!writes_all || reads_target)) {
        return false;
    }
    // What has to be written, in window coordinates: the target, or the part.
    u32 rx0 = 0, rx1 = width, ry0 = 0, ry1 = height;
    if (part.x1 != 0) {
        rx0 = part.y0;
        rx1 = part.y1;
        ry0 = height - part.x1;
        ry1 = height - part.x0;
    }
    const auto& sc = rs.scissor_test;
    if (sc.mode == Pica::RasterizerRegs::ScissorMode::Exclude ||
        (sc.mode == Pica::RasterizerRegs::ScissorMode::Include &&
         (sc.x1 > rx0 || sc.y1 > ry0 || sc.x2 + 1 < rx1 || sc.y2 + 1 < ry1))) {
        return false;
    }
    // The viewport clips x and y: it has to hold that.
    const float half_x = f24::FromRaw(rs.viewport_size_x).ToFloat32();
    const float half_y = f24::FromRaw(rs.viewport_size_y).ToFloat32();
    const float corner_x = static_cast<float>(rs.viewport_corner.x);
    const float corner_y = static_cast<float>(rs.viewport_corner.y);
    const float fx0 = static_cast<float>(rx0), fx1 = static_cast<float>(rx1);
    const float fy0 = static_cast<float>(ry0), fy1 = static_cast<float>(ry1);
    if (corner_x > fx0 || corner_y > fy0 || corner_x + 2 * half_x < fx1 ||
        corner_y + 2 * half_y < fy1) {
        return false;
    }

    std::array<Pica::OutputVertex, 6> vertices;
    if (!QuadVertices(is_indexed, immediate, count, vertices)) {
        return false;
    }
    std::array<float, 12> x{}, y{};
    for (u32 j = 0; j < count; j++) {
        const Pica::OutputVertex& v = vertices[j];
        const float w = v.pos.w.ToFloat32(), z = v.pos.z.ToFloat32();
        if (!(w > 0.0001f) || z > 0 || z < -w) {
            return false;
        }
        x[j] = (v.pos.x.ToFloat32() / w + 1) * half_x + corner_x;
        y[j] = (v.pos.y.ToFloat32() / w + 1) * half_y + corner_y;
    }
    static constexpr u32 tris[3][2][3] = {
        {{0, 1, 2}, {3, 4, 5}}, // list
        {{0, 1, 2}, {2, 1, 3}}, // strip
        {{0, 1, 2}, {0, 2, 3}}, // fan
    };
    const auto& t = tris[list ? 0 : topology == Topology::Strip ? 1 : 2];
    const float x0 = *std::min_element(x.begin(), x.begin() + count);
    const float x1 = *std::max_element(x.begin(), x.begin() + count);
    const float y0 = *std::min_element(y.begin(), y.begin() + count);
    const float y1 = *std::max_element(y.begin(), y.begin() + count);
    if (x0 > fx0 + 0.25f || y0 > fy0 + 0.25f || x1 < fx1 - 0.25f || y1 < fy1 - 0.25f) {
        return false;
    }
    // Each triangle is kept, its vertices on three of the box's corners, the two leaving out opposite
    // corners: the box whole.
    const float eps = 0.01f;
    u32 left_out[2]{};
    for (u32 k = 0; k < 2; k++) {
        const u32 a = t[k][0], b = t[k][1], c = t[k][2];
        const float area = (x[b] - x[a]) * (y[c] - y[a]) - (y[b] - y[a]) * (x[c] - x[a]);
        const CullMode cull = rs.cull_mode;
        if (area == 0 || (cull == CullMode::KeepCounterClockWise && area < 0) ||
            (cull == CullMode::KeepClockWise && area > 0)) {
            return false;
        }
        u32 corners = 0;
        for (const u32 v : t[k]) {
            const bool left = std::abs(x[v] - x0) < eps, right = std::abs(x[v] - x1) < eps;
            const bool bottom = std::abs(y[v] - y0) < eps, top = std::abs(y[v] - y1) < eps;
            if ((!left && !right) || (!bottom && !top)) {
                return false;
            }
            corners |= 1u << ((right ? 1u : 0u) | (top ? 2u : 0u));
        }
        if (std::popcount(corners) != 3) {
            return false;
        }
        left_out[k] = static_cast<u32>(std::countr_zero(~corners & 0xfu));
    }
    return (left_out[0] ^ left_out[1]) == 3;
}

bool RasterizerSgxd::FillsClearQuad(bool is_indexed, const float* immediate, u32 count,
                                   const ColourBuffer& target, const StorePart& part,
                                   PAddr depth, bool dwrite, bool stencil, bool act) {
    using Tev = Pica::TexturingRegs::TevStageConfig;
    const auto& om = regs.framebuffer.output_merger;
    const auto& fb = regs.framebuffer.framebuffer;
    const auto& rs = regs.rasterizer;
    // The colour is one the CPU knows: the first stage replaces it with the primary colour or its
    // constant, the later stages pass it on, no fog, and a blend that copies or none.
    const auto stages = regs.texturing.GetTevStages();
    const Tev& s0 = stages[0];
    const auto known = [](Tev::Source s) {
        return s == Tev::Source::PrimaryColor || s == Tev::Source::Constant;
    };
    const auto passes_on = [](const Tev& s) {
        return s.color_op == Tev::Operation::Replace && s.alpha_op == Tev::Operation::Replace &&
               s.color_source1 == Tev::Source::Previous &&
               s.alpha_source1 == Tev::Source::Previous &&
               s.color_modifier1 == Tev::ColorModifier::SourceColor &&
               s.alpha_modifier1 == Tev::AlphaModifier::SourceAlpha &&
               s.GetColorMultiplier() == 1 && s.GetAlphaMultiplier() == 1;
    };
    using BF = FramebufferRegs::BlendFactor;
    using BE = FramebufferRegs::BlendEquation;
    const auto& ab = om.alpha_blending;
    const bool copies = om.alphablend_enable
                            ? ab.blend_equation_rgb == BE::Add && ab.blend_equation_a == BE::Add &&
                                  ab.factor_source_rgb == BF::One &&
                                  ab.factor_dest_rgb == BF::Zero &&
                                  ab.factor_source_a == BF::One && ab.factor_dest_a == BF::Zero
                            : om.logic_op == FramebufferRegs::LogicOp::Copy;
    if (om.fragment_operation_mode != FramebufferRegs::FragmentOperationMode::Default ||
        regs.texturing.fog_mode != Pica::TexturingRegs::FogMode::None || !copies ||
        s0.color_op != Tev::Operation::Replace || s0.alpha_op != Tev::Operation::Replace ||
        !known(s0.color_source1) || !known(s0.alpha_source1) ||
        s0.color_modifier1 != Tev::ColorModifier::SourceColor ||
        s0.alpha_modifier1 != Tev::AlphaModifier::SourceAlpha || s0.GetColorMultiplier() != 1 ||
        s0.GetAlphaMultiplier() != 1 ||
        !std::all_of(stages.begin() + 1, stages.end(), passes_on)) {
        return false;
    }
    // Guest VRAM, a buffer drawn at 1x and not through a scratch, and a stencil test that writes
    // every pixel's or none.
    const u32 width = target.width, height = target.height, bpp = target.bpp;
    if (target.addr < Memory::VRAM_PADDR || target.End() > Memory::VRAM_PADDR_END ||
        width < 32 || UpscaledFor(target) != nullptr ||
        (stencil && !StencilOverwrites(regs.framebuffer))) {
        return false;
    }
    if (!CoversTarget(is_indexed, immediate, count, width, height, part) ||
        (dwrite && !CoversTarget(is_indexed, immediate, count, width, height, part, true))) {
        return false;
    }
    // The tile rows the part takes and the tiles of each, as one strided fill; all of the buffer as
    // one run.
    const u32 tpr = width / 8;
    const u32 r0 = part.x1 != 0 ? part.x0 / 8 : 0, r1 = part.x1 != 0 ? part.x1 / 8 : height / 8;
    const u32 c0 = part.x1 != 0 ? part.y0 / 8 : 0, c1 = part.x1 != 0 ? part.y1 / 8 : tpr;
    const u32 line = (c1 - c0) * 64, stride = tpr * 64 * bpp;
    if (part.x1 != 0 && (line > SGXD_PTLA_MAX_EXTENT || stride > 0xffffu)) {
        return false;
    }
    // The depth and stencil store, which a part, or the stencil alone, changes only in part.
    const auto df = fb.depth_format.Value();
    const bool has_stencil = df == FramebufferRegs::DepthFormat::D24S8;
    const bool stencil_write = stencil && fb.allow_depth_stencil_write != 0;
    const bool whole_store =
        part.x1 == 0 && (!dwrite || stencil_write || !has_stencil) && (dwrite || !has_stencil);
    if ((dwrite || stencil_write) && !whole_store &&
        !DepthStoreValid(depth, width, height, static_cast<u32>(df), 4, 0)) {
        return false;
    }

    // The window positions as the shader interpreter makes them: one colour and one depth.
    std::array<Pica::OutputVertex, 6> v;
    if (!QuadVertices(is_indexed, immediate, count, v)) {
        return false;
    }
    std::array<u8, 4> rgba{};
    for (u32 c = 0; c < 4; c++) {
        const Tev::Source source = c < 3 ? s0.color_source1.Value() : s0.alpha_source1.Value();
        if (source == Tev::Source::Constant) {
            rgba[c] = static_cast<u8>(s0.const_color >> (8 * c));
            continue;
        }
        // The interpolated colour times 255 is rounded: a value that far from a half rounds the same
        // however the GPU interpolates it.
        const f24 first = v[0].color[c];
        const float scaled = first.ToFloat32() * 255.0f;
        const float near = std::round(scaled);
        if (!(scaled >= 0.0f && scaled <= 255.0f) || std::abs(scaled - near) > 0.01f) {
            return false;
        }
        for (u32 j = 1; j < count; j++) {
            if (v[j].color[c].ToFloat32() != first.ToFloat32()) {
                return false;
            }
        }
        rgba[c] = static_cast<u8>(near);
    }
    // The depth: z / w scaled and offset (times w W-buffered), clamped; only 0 and 1, which the TA
    // gives exactly too.
    float z = 0.0f;
    if (dwrite) {
        const float scale = f24::FromRaw(rs.viewport_depth_range).ToFloat32();
        const float offset = f24::FromRaw(rs.viewport_depth_near_plane).ToFloat32();
        for (u32 j = 0; j < count; j++) {
            const float w = v[j].pos.w.ToFloat32();
            float d = v[j].pos.z.ToFloat32() / w * scale + offset;
            if (rs.depthmap_enable == Pica::RasterizerRegs::DepthBuffering::WBuffering) {
                d *= w;
            }
            d = std::clamp(d, 0.0f, 1.0f);
            if ((d != 0.0f && d != 1.0f) || (j != 0 && d != z)) {
                return false;
            }
            z = d;
        }
    }
    const auto& st = om.stencil_test;
    const u32 stencil_value =
        st.action_depth_pass == FramebufferRegs::StencilAction::Replace ? st.reference_value : 0;
    if (!act) {
        return true;
    }

    // The colour as the PICA writes it, its bytes low first, as a fill's value.
    const Common::Vec4<u8> colour{rgba[0], rgba[1], rgba[2], rgba[3]};
    std::array<u8, 4> bytes{};
    using CF = FramebufferRegs::ColorFormat;
    switch (target.format) {
    case CF::RGBA8:
        Common::Color::EncodeRGBA8(colour, bytes.data());
        break;
    case CF::RGB8:
        Common::Color::EncodeRGB8(colour, bytes.data());
        break;
    case CF::RGB5A1:
        Common::Color::EncodeRGB5A1(colour, bytes.data());
        break;
    case CF::RGB565:
        Common::Color::EncodeRGB565(colour, bytes.data());
        break;
    case CF::RGBA4:
        Common::Color::EncodeRGBA4(colour, bytes.data());
        break;
    default:
        return false;
    }
    const u32 value = bytes[0] | bytes[1] << 8 | bytes[2] << 16 | bytes[3] << 24;
    const u32 format = bpp == 4 ? SGXD_PTLA_8888 : (bpp == 3 ? SGXD_PTLA_888 : SGXD_PTLA_U88);
    // A part: its tile rows, a buffer row of tiles apart. All of it: rows of 4096 pixels and a last
    // partial one.
    constexpr u32 RowPixels = 4096;
    const PAddr start = target.addr + (r0 * tpr + c0) * 64 * bpp;
    const PAddr end = part.x1 != 0 ? start + (r1 - r0 - 1) * stride + line * bpp : target.End();
    const u32 pixels = width * height;
    const SgxdPtlaFill fill = part.x1 != 0 ? SgxdPtlaFill{start, format, stride, line, r1 - r0, 0,
                                                          value}
                                           : SgxdPtlaFill{start, format, RowPixels * bpp,
                                                          RowPixels, pixels / RowPixels,
                                                          pixels % RowPixels, value};
    DepthStoresWritten(start, end);
    backend.RenderBefore(start, end, "a clear quad's fill");
    char failed[160];
    [[maybe_unused]] const int filled = sgxd_ptla_fill(&fill, nullptr, failed, sizeof(failed));
    DEBUG_ASSERT_MSG(filled == 0, "sgxd: a clear quad's fill of {:#x}-{:#x}: {}", start, end,
                     failed);
    // All of the buffer cleared: a pass on it starts from the clear. A part: the clear recorded over
    // the buffer no longer holds.
    if (part.x1 == 0) {
        RecordClear({target.addr, target.End(), value, bpp, 0});
    } else {
        ForgetClears(target.addr, target.End());
    }
    TexturesWritten(start, end);
    // The depth and stencil wait in the store's pending clear until a pass uses them: a clear of the
    // same part merges into it, one of another part writes it first.
    if (dwrite || stencil_write) {
        // A fill recorded over the depth buffer no longer holds.
        ForgetClears(depth, depth + width * height * FramebufferRegs::BytesPerDepthPixel(df));
        DepthStore& d = DepthStoreFor(depth, width, height, static_cast<u32>(df));
        if (d.pending && d.pending->part != part) {
            SettleDepthClear(d);
        }
        DepthStore::PendingClear p = d.pending.value_or(DepthStore::PendingClear{part});
        if (dwrite) {
            p.z = z;
            p.has_z = true;
        }
        if (stencil_write) {
            p.stencil = stencil_value;
            p.has_stencil = true;
        }
        d.pending = p;
        d.valid = true;
        d.ta_w = false;
    }
    const auto filled_what = [&] {
        return fmt::format("{:#x} ({}x{}, part {}..{} x {}..{}): colour {:#x}{}{}", target.addr,
                           width, height, part.x0, part.x1, part.y0, part.y1, value,
                           dwrite ? fmt::format(", depth {}", z) : "",
                           stencil_write ? fmt::format(", stencil {}", stencil_value) : "");
    };
    if (quad_fills++ < 4) {
        LOG_INFO(Render, "a clear quad filled over {}", filled_what());
    }
#ifdef VITA_DIAGNOSTICS
    static const u64 pass_log_at = u64{SgxdFlagValue("passlog")} * 1000000;
    if (pass_log_at != 0) {
        const u64 now = sceKernelGetProcessTimeWide();
        if (now >= pass_log_at && now < pass_log_at + 1000000) {
            LOG_INFO(Render, "perf: clear quad filled over {}", filled_what());
        }
    }
#endif
    return true;
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
    // The programs start again with the code heap half full, before the draw holds any: their code
    // goes back once the passes begun so far are done, and the open pass's draws emit theirs again
    // from the half left (no second flush before the next pass).
    if (backend.CodeFree() < SgxdBackend::CodeBytes / 2 && backend.PassMark() != flush_mark) {
        flush_mark = backend.PassMark();
        FlushPrograms();
    }
    // A DMA into cached memory still queued: the draw reads guest memory as the guest would after its
    // interrupt.
    if (backend.RetirePending()) {
        backend.RetireAll("a draw after a DMA's cache work");
    }
    const auto& pipeline = regs.pipeline;
    const u32 count = immediate ? immediate_count : pipeline.num_vertices;
    draw_serial++;
#ifdef VITA_DIAGNOSTICS
    VprofDraw(is_indexed, immediate != nullptr, count);
    if (ReplaySkipsDraw()) {
        return true;
    }
#endif
    if (SkipsDoubleRender(count)) {
        return true;
    }
    // The game's supersampling dropped (Supersampled): what the draw reads of its target's
    // size, its viewport and its scissor are the 1x buffer's until it returns.
    struct SupersampleRestore {
        RasterizerGxm& r;
        const bool dropped;
        ~SupersampleRestore() {
            if (dropped) {
                r.RestoreSupersampling();
            }
        }
    } const supersample_restore{*this, DropSupersampling()};
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
    // The cached state's registers changed, before SyncDrawUniforms clears the marks.
    fragment_regs_dirty |= StateRegs::AnyDirty(StateRegs::FragmentMask, pica.dirty_regs.qwords);
    layout_regs_dirty |= StateRegs::AnyDirty(StateRegs::LayoutMask, pica.dirty_regs.qwords);
    SyncDrawUniforms();

    // --- the target and the output merger ---
    const auto& fb = regs.framebuffer.framebuffer;
    const auto& om = regs.framebuffer.output_merger;
    // Shadow rendering: the fragment program updates the shadow map's word in place from o0, so every
    // fragment runs, in order, with no blend and no depth or stencil test.
    const bool shadow =
        om.fragment_operation_mode == FramebufferRegs::FragmentOperationMode::Shadow;
    // Gas rendering's density pass draws as Default; its fog
    // stage's half writes (0, 0, 0, 0).
    DEBUG_ASSERT_MSG(
        om.fragment_operation_mode == FramebufferRegs::FragmentOperationMode::Default ||
            om.fragment_operation_mode == FramebufferRegs::FragmentOperationMode::Gas || shadow,
        "sgxd: {}: fragment operation mode {}", describe(),
        static_cast<u32>(om.fragment_operation_mode.Value()));
    DEBUG_ASSERT_MSG(!shadow || fb.color_format == FramebufferRegs::ColorFormat::RGBA8,
                     "sgxd: {}: a shadow map in colour format {}", describe(),
                     static_cast<u32>(fb.color_format.Value()));
    // A texture unit that reads the pixel's own colour reads the tile (OwnPixelUnits).
    draw_own_pixel = OwnPixelUnits(is_indexed, immediate, count);
    // The colour write mask: without a blend, the pack's byte mask; with one, the masked channels blend
    // as ZERO, ONE. The SOP2 has one factor pair for R, G and B, so they mask together.
    using BF = FramebufferRegs::BlendFactor;
    using BE = FramebufferRegs::BlendEquation;
    u8 mask = fb.allow_color_write == 0
                  ? 0
                  : static_cast<u8>(om.red_enable | om.green_enable << 1 | om.blue_enable << 2 |
                                    om.alpha_enable << 3);
    // ONE, ZERO, ADD in colour and alpha is no blend: the pass type stays opaque, so the ISP shades only
    // what no later opaque draw hides.
    const auto& ab = om.alpha_blending;
    const bool identity_blend = ab.blend_equation_rgb == BE::Add &&
                                ab.blend_equation_a == BE::Add && ab.factor_source_rgb == BF::One &&
                                ab.factor_dest_rgb == BF::Zero && ab.factor_source_a == BF::One &&
                                ab.factor_dest_a == BF::Zero;
    const bool blend = !shadow && om.alphablend_enable != 0 && !identity_blend;
    Epilogue epilogue{};
    if (shadow) {
        mask = 0xf;
        epilogue.mask = mask;
    } else if (blend) {
        const auto& b = om.alpha_blending;
        BE eq_rgb = b.blend_equation_rgb, eq_a = b.blend_equation_a;
        BF src_rgb = b.factor_source_rgb, dst_rgb = b.factor_dest_rgb;
        BF src_a = b.factor_source_a, dst_a = b.factor_dest_a;
        // R, G and B masked apart: the blend into pa0, then the mask's bytes of it into o0 (PushBlend).
        if ((mask & 7) != 0 && (mask & 7) != 7) {
            epilogue.mask = mask;
        }
        if ((mask & 7) == 0) {
            eq_rgb = BE::Add, src_rgb = BF::Zero, dst_rgb = BF::One;
        }
        if ((mask & 8) == 0) {
            eq_a = BE::Add, src_a = BF::Zero, dst_a = BF::One;
        }
        // ReverseSubtract has no SOP2 form (src1 - src2 only), but with a ZERO factor it is an ADD for
        // every input: dest factor ZERO gives 0, source factor ZERO the dest term alone.
        const auto reverse_to_add = [](BE& eq, BF& src, BF& dst) {
            if (eq == BE::ReverseSubtract && dst == BF::Zero) {
                eq = BE::Add, src = BF::Zero;
            } else if (eq == BE::ReverseSubtract && src == BF::Zero) {
                eq = BE::Add;
            }
        };
        reverse_to_add(eq_rgb, src_rgb, dst_rgb);
        reverse_to_add(eq_a, src_a, dst_a);
        // A constant factor: the SOP2 has none, but with no factor on the fragment's own colour the
        // constant stands in for the source: LIMM pa0 before the blend, CONSTANT_* read as SRC_*.
        const auto is_constant = [](BF f) { return f >= BF::ConstantColor && f <= BF::OneMinusConstantAlpha; };
        const auto reads_source = [](BF f) {
            return f == BF::SourceColor || f == BF::OneMinusSourceColor || f == BF::SourceAlpha ||
                   f == BF::OneMinusSourceAlpha || f == BF::SourceAlphaSaturate;
        };
        const bool any_constant = is_constant(src_rgb) || is_constant(dst_rgb) ||
                                  is_constant(src_a) || is_constant(dst_a);
        const bool reads_fragment = src_rgb != BF::Zero || src_a != BF::Zero ||
                                    reads_source(dst_rgb) || reads_source(dst_a);
        if (any_constant && reads_fragment) {
            // The constant beside the fragment's colour: the split form (BlendSplitWords).
            [[maybe_unused]] const bool split = BlendSplitWords(
                eq_rgb, eq_a, src_rgb, dst_rgb, src_a, dst_a, epilogue.split_alpha, epilogue.blend);
            DEBUG_ASSERT_MSG(epilogue.mask == 0xf,
                             "sgxd: {}: colour mask {:#x} with a split blend (a constant factor "
                             "with the fragment's colour) is not built",
                             describe(), mask);
            DEBUG_ASSERT_MSG(split,
                             "sgxd: {}: blend {}/{} {}/{} {}/{}: a constant factor with the "
                             "fragment's colour and MIN, MAX or SRC_ALPHA_SATURATE in colour is "
                             "not built",
                             describe(), static_cast<u32>(eq_rgb), static_cast<u32>(eq_a),
                             static_cast<u32>(src_rgb), static_cast<u32>(dst_rgb),
                             static_cast<u32>(src_a), static_cast<u32>(dst_a));
            epilogue.colour = om.blend_const.raw;
        } else if (any_constant) {
            // CONSTANT_COLOR 10 and 11 are SRC_COLOR 2 and 3 eight down, CONSTANT_ALPHA 12 and 13 SRC_ALPHA
            // 6 and 7 six down (eight down for all four would make CONSTANT_ALPHA DEST_COLOR).
            const auto as_source = [](BF f) {
                switch (f) {
                case BF::ConstantColor:
                    return BF::SourceColor;
                case BF::OneMinusConstantColor:
                    return BF::OneMinusSourceColor;
                case BF::ConstantAlpha:
                    return BF::SourceAlpha;
                case BF::OneMinusConstantAlpha:
                    return BF::OneMinusSourceAlpha;
                default:
                    return f;
                }
            };
            dst_rgb = as_source(dst_rgb);
            dst_a = as_source(dst_a);
            epilogue.constant = true;
            epilogue.colour = om.blend_const.raw;
        }
        if (epilogue.split_alpha == 0) {
            epilogue.blend = BlendWord(eq_rgb, eq_a, src_rgb, dst_rgb, src_a, dst_a);
        }
        if (epilogue.blend == 0 && (eq_rgb == BE::ReverseSubtract || eq_a == BE::ReverseSubtract)) {
            epilogue.blend = BlendWordSwapped(eq_rgb, eq_a, src_rgb, dst_rgb, src_a, dst_a);
        }
        DEBUG_ASSERT_MSG(epilogue.blend != 0,
                         "sgxd: {}: blend {}/{} {}/{} {}/{}: ReverseSubtract with Subtract or "
                         "SRC_ALPHA_SATURATE is not built",
                         describe(), static_cast<u32>(eq_rgb), static_cast<u32>(eq_a),
                         static_cast<u32>(src_rgb), static_cast<u32>(dst_rgb),
                         static_cast<u32>(src_a), static_cast<u32>(dst_a));
    } else if (om.alphablend_enable != 0) {
        // The identity blend: a copy (the logic op only applies with blending off).
        epilogue.mask = mask;
    } else {
        // NoOp keeps the buffer: no byte written.
        DEBUG_ASSERT_MSG(om.logic_op == FramebufferRegs::LogicOp::Copy ||
                             om.logic_op == FramebufferRegs::LogicOp::NoOp,
                         "sgxd: {}: logic op {} is not built", describe(),
                         static_cast<u32>(om.logic_op.Value()));
        if (om.logic_op == FramebufferRegs::LogicOp::NoOp) {
            mask = 0;
        }
        epilogue.mask = mask;
    }
    // A fragment that keeps some of what is under it reads o0, as one reading its own pixel
    // does: the pass type is translucent, so the ISP neither removes what it covers nor runs
    // it out of order.
    bool translucent = shadow || blend || mask != 0xf || draw_own_pixel != 0;
#ifdef VITA_DIAGNOSTICS
    // sgxd_flags keepalpha_opaque (a probe, pictures may be wrong): a draw with no blend that
    // keeps the destination's alpha (colour mask RGB) as opaque, so the ISP hides its covered
    // fragments; exact only when no opaque draw that writes alpha lies hidden under it.
    static const bool keepalpha_opaque = SgxdFlag("keepalpha_opaque");
    if (keepalpha_opaque && !shadow && !blend && mask == 0x7 && draw_own_pixel == 0) {
        translucent = false;
    }
#endif
    // The test runs only when enabled, the write whenever enabled and allowed, and neither
    // (nor the stencil) with no depth buffer (address 0).
    const bool has_depth = fb.GetDepthBufferPhysicalAddress() != 0;
    const u32 dcmp =
        !shadow && has_depth && om.depth_test_enable ? DepthCompare(om.depth_test_func) : 7;
    const bool dwrite =
        !shadow && has_depth && fb.allow_depth_stencil_write != 0 && om.depth_write_enable;
    // FragmentConfigFor's alpha test as a blend takes a draw writing neither.
    draw_depth_stencil =
        has_depth && ((fb.allow_depth_stencil_write != 0 && om.depth_write_enable) ||
                      om.stencil_test.enable);
    // The stencil: a D24S8 buffer's top byte, on chip as the depth is. The ISP's compare codes are the
    // depth's, and its ops are in the PICA's order.
    const auto& st = om.stencil_test;
    const bool stencil = !shadow && has_depth && st.enable &&
                         fb.depth_format == FramebufferRegs::DepthFormat::D24S8;
    const bool uses_depth = dcmp != 7 || dwrite || stencil;

    // With the top screen upscaled, a buffer too tall to scale whole is drawn from the top screen's
    // first row on alone (TopWindowFor). A draw onto rows before that alone is drawn onto the whole
    // buffer at 1x; a draw over both is drawn in the window alone.
    const ColourBuffer buffer{fb.GetColorBufferPhysicalAddress(), fb.GetWidth(), fb.GetHeight(),
                              fb.color_format,
                              FramebufferRegs::BytesPerColorPixel(fb.color_format)};
    ColourBuffer target = TopWindowFor(buffer);
    if (target.addr != buffer.addr) {
        const auto [lo, hi] = DrawnRows();
        if (lo < target.addr && lo < hi) {
            const bool before_only = hi <= target.addr;
            if (window_misses++ < 8) {
                [[maybe_unused]] const u32 row_bytes = buffer.width * buffer.bpp;
                LOG_INFO(Render,
                         "{}: rows {}..{} of the colour buffer at {:#x}, {} its upscaled top "
                         "screen's window, drawn {} (frame {})",
                         describe(), (lo - buffer.addr) / row_bytes,
                         (hi - buffer.addr) / row_bytes, buffer.addr,
                         before_only ? "before" : "before and in",
                         before_only ? "at 1x" : "in the window alone", frames);
            }
            if (before_only) {
                target = buffer;
            }
        }
    }
    const PAddr colour = target.addr;
    const u32 width = target.width, height = target.height;
    const PAddr depth = fb.GetDepthBufferPhysicalAddress() +
                        (buffer.height - height) * width *
                            FramebufferRegs::BytesPerDepthPixel(fb.depth_format);
    draw_rows = height;
    // A draw whose texture reads bytes the open pass drew: guest memory holds them only once the pass
    // ended, so it ends here. Bytes no
    // draw covered are guest memory's still.
    TextureReads(is_indexed, immediate, count);
    DEBUG_ASSERT_MSG(
        std::none_of(unit_reads.begin(), unit_reads.end(),
                     [&](const UnitRead& r) {
                         for (u32 k = 0; k < r.parts; k++) {
                             if (MeetsSupersampled1x(r.lo[k], r.hi[k])) {
                                 return true;
                             }
                         }
                         return false;
                     }),
        "GXM: {}: a texture read of a supersampled buffer drawn at 1x before its transfer is "
        "not built",
        describe());
    if (backend.InPass() && TexturesReadPassColour()) {
        EndPass("a draw reading its own colour buffer");
    }
    if (backend.InPass() &&
        (pass_colour.addr != colour || pass_colour.width != width ||
         pass_colour.height != height || pass_colour.format != fb.color_format ||
         (uses_depth && pass_depth != depth))) {
        EndPass("a draw to other buffers");
    }
    // A pass stores its draws' viewport's part of the buffer: a draw with another viewport starts the
    // next pass, so no draw stores past its own.
    const StorePart part = StorePartFor(target);
    if (backend.InPass() && pass_part != part && !KeepPassForViewport(target)) {
        EndPass("a draw with another viewport");
    }
    // A viewport off the store's grid keeps its fragments by the ISP's mask (MaskOutside): no kill, no
    // punch-through. A draw with another viewport after the mask starts the next pass (a mask set again
    // is not verified). A pass masked to its part keeps its draws with that viewport.
    const StorePart view = ViewportPart(target);
    const StorePart view_mask = OnStoreGrid(view) ? StorePart{} : view;
    if (backend.InPass() && pass_mask.x1 != 0 && pass_mask != view_mask &&
        !(view_mask.x1 == 0 && pass_mask == part) && !KeepPassForViewport(target)) {
        EndPass("a draw with another viewport's mask");
    }
    // A geometry draw's stages take the pass's data (GeometryStage): the stored vertex outputs, the
    // triangles, B's index list and the row tables. A pass that may not hold them ends first; the
    // bound keeps the room small where the program has one.
    if (pipeline.use_gs == Pica::PipelineRegs::UseGS::Yes) {
        const u32 vertices = pipeline.num_vertices;
        const u32 vs_outputs = pipeline.vs_outmap_total_minus_1_a + 1;
        const u32 gs_outputs = static_cast<u32>(std::popcount(regs.gs.output_mask.Value()));
        const u32 tri_bytes = 3 * gs_outputs * 16;
        const bool variable =
            pipeline.gs_config.mode == Pica::PipelineRegs::GSMode::VariablePrimitive;
        const u32 room = std::max(256u << 10, vertices * tri_bytes);
        const GsCodeFacts facts = GsFacts();
        const u32 gs_bound = facts.bound;
        const u32 triangles =
            gs_bound != 0 ? std::min(room, vertices * gs_bound * (tri_bytes + 6)) : room;
        const bool tables =
            variable || (pipeline.gs_config.mode == Pica::PipelineRegs::GSMode::FixedPrimitive &&
                         facts.indexes_uniforms);
        // A0 stores every vertex of the indices' range, with a table entry and an index each.
        u32 a0_vertices = vertices;
        if (is_indexed) {
            DEBUG_ASSERT_MSG(gs_indices_set, "sgxd: {}: a geometry draw without its indices' copy",
                             describe());
            const u8* const index_cpu = gs_indices.data();
            if (pipeline.index_array.format != 0 && scanned_indices) {
                a0_vertices = scanned_indices->second - scanned_indices->first + 1u;
            } else if (pipeline.index_array.format != 0) {
                const auto [lo, hi] = Common::FindMinMax(
                    std::span<const u16>{reinterpret_cast<const u16*>(index_cpu), vertices});
                a0_vertices = hi - lo + 1u;
            } else {
                const auto [lo, hi] = Common::FindMinMax(std::span<const u8>{index_cpu, vertices});
                a0_vertices = hi - lo + 1u;
            }
        }
        u32 bound = triangles + a0_vertices * (vs_outputs * 16 + 4 + 2) +
                    vertices * (tables ? 576 : 96) + (16u << 10);
        // Past what a pass's data holds, the stored outputs, triangles and tables take memory of their own
        // (gs_own_data), the pass's data their index lists.
        gs_own_data = bound > SgxdBackend::PassDataBytes - (64u << 10);
        if (gs_own_data) {
            bound = a0_vertices * 2 + vertices * (variable ? 22 : 6) + (128u << 10) + (16u << 10);
        }
        if (backend.InPass() && backend.DataLeft() < bound) {
            EndPass("the pass's data for a geometry draw");
        }
    } else if (backend.InPass()) {
        // Any other draw takes its immediate vertices or widened indices, and a few KiB for its programs'
        // images and buffers and masks. A program that indexes its uniforms copies its whole bank when they
        // change.
        const u32 vertex_bytes = immediate ? count * immediate_attributes * 16 : count * 2;
        if (backend.DataLeft() < vertex_bytes + (16u << 10)) {
            EndPass("the pass's data for a draw");
        }
    }
    // A pass holds SgxdBackend::MaxDraws draws (its arrays, its recipe's layout). From here a
    // draw adds at most four to the open pass: the viewport's mask, every tile touched for guest
    // memory's depth, the margin's mask (each once a pass) and its own. A pass with fewer left
    // ends, and the draw goes on in the next pass over the same target, loaded. a game
    // Chronicles 3D draws 1048-1061 into its top screen in later scenes.
    constexpr u32 DrawAddsAtMost = 4;
    if (backend.InPass() && backend.PassDraws() + DrawAddsAtMost > SgxdBackend::MaxDraws) {
        EndPass("a pass full of draws");
    }
    // A pass that would only write one colour over the part it stores is a fill: the PTLA's, beside
    // the 3D. Only where the last such quad was alone in its pass (ClearQuadSite).
    if (!backend.InPass()) {
        pass_clear_site = -1;
        if (filled_site >= 0) {
            ClearQuadSite& s = clear_quad_sites[filled_site];
            if (s.addr == colour && s.part == part) {
                s.alone = false;
            }
            filled_site = -1;
        }
        s32 site = -1;
        for (u32 i = 0; i < clear_quad_sites.size(); i++) {
            if (clear_quad_sites[i].addr == colour && clear_quad_sites[i].part == part) {
                site = static_cast<s32>(i);
            }
        }
        const bool alone = site >= 0 && clear_quad_sites[site].alone;
        if (draw_own_pixel == 0 && FillsClearQuad(is_indexed, immediate, count, target, part,
                                                  depth, dwrite, stencil, alone)) {
            if (alone) {
                filled_site = site;
                return true;
            }
            if (site < 0) {
                site = static_cast<s32>(clear_quad_next);
                clear_quad_next = (clear_quad_next + 1) % clear_quad_sites.size();
                clear_quad_sites[site] = {colour, part, false};
            }
            pass_clear_site = site;
        }
    }
    if (!backend.InPass()) {
        // A buffer of a width no pass draws: its scratch (NarrowScratch), the guest's bytes copied in.
        pass_narrow = nullptr;
        ColourBuffer drawn = target;
        if (narrow != 0) {
            DEBUG_ASSERT_MSG(target.width % 8 == 0 && target.height >= 8 &&
                                 target.height % 8 == 0 && (target.width >= 32 || !uses_depth),
                             "sgxd: {}: a {}x{} colour buffer{}: a narrow buffer is drawn only "
                             "8s wide and high, under 32 wide without depth",
                             describe(), target.width, target.height,
                             uses_depth ? " with depth" : "");
            // Its depth (32 wide or more) needs nothing of its own: the store's tiles cover the scratch's pass
            // (NewDepthStore aligns both widths to the same 32).
            ASSERT_MSG(target.width < 32 || target.width % 8 == 0,
                       "sgxd: {}: a {}x{} colour buffer: a buffer not in 16s wide is drawn only 8s "
                       "wide",
                       describe(), target.width, target.height);
            const NarrowScratch& n = NarrowScratchFor(target);
            const u32 strip = target.width * 8 * target.bpp;
            backend.RenderBefore(target.addr, target.End(), "a narrow colour buffer's copy in");
            CopyLines(target.addr, n.surface.gpu, strip, target.height / 8, strip,
                      narrow * 8 * target.bpp, "a narrow colour buffer's copy in");
            drawn.addr = n.surface.gpu;
            drawn.width = narrow;
            // A pass's target is 32 rows at least; the viewport and scissor mirror the guest's height, so its
            // rows are the scratch's first strips all the same.
            drawn.height = std::max(target.height, 32u);
            pass_narrow = &n;
        }
        SgxdBackend::PicaTarget t = PicaTargetFor(drawn, "a draw");
        t.x0 = part.x0;
        t.x1 = part.x1;
        t.y0 = part.y0;
        t.y1 = part.y1;
        pass_part = part;
        pass_mask = {};
        pass_margin_masked = false;
        ClearedRange clear{};
        const bool took = TakeClear(colour, target.End(), clear);
        ClearedRange depth_clear{};
        pass_depth = depth;
        pass_depth_bytes = FramebufferRegs::BytesPerDepthPixel(fb.depth_format);
        pass_depth_cleared = false;
        pass_depth_format = static_cast<u32>(fb.depth_format.Value());
        pass_depth_used = false;
        pass_depth_written = false;
        pass_depth_load = false;
        pass_depth_covered = false;
        pass_depth_part = false;
        pass_depth_guest = false;
        pass_ta_w = false;
        pass_w_map = {};
        float bg_depth = 1.0f;
        u32 bg_stencil = 0;
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
            if (fb.depth_format == FramebufferRegs::DepthFormat::D24S8) {
                bg_stencil = depth_clear.value >> 24;
            }
        }
        // A buffer with a picture is drawn at its scale, the top screen's widescreen wider: a pass as wide
        // as the screen draws the margins beside it too.
        pass_upscaled = !pass_narrow ? UpscaledFor(target) : nullptr;
        pass_quarters = pass_upscaled ? pass_upscaled->quarters : 4;
        pass_margin = pass_upscaled ? pass_upscaled->margin : 0;
        pass_widened = pass_margin != 0 && part.x1 != 0 && part.x1 - part.x0 >= TopScreenRows;
        if (pass_upscaled) {
            t.picture = pass_upscaled->picture.gpu;
            t.picture_stride = pass_upscaled->stride;
            t.picture_quarters = pass_quarters;
            t.picture_margin = pass_margin;
            t.picture_widen = pass_widened;
            pass_scaled_target = t;
        }
        // The pass starts from the fill's clear, which guest memory holds already (a tile no
        // draw touches keeps it), from nothing when its first draw writes every pixel it stores,
        // or else from the buffer's own bytes: its first draws read them (ColorFormat numbers
        // the first five TextureFormats).
        const bool covered = draw_own_pixel == 0 && CoversTarget(is_indexed, immediate, count,
                                                                 width, height, PartToCover());
        pass_colour = target;
#ifdef VITA_DIAGNOSTICS
        drawn_buffers[colour] = target;
#endif
#ifdef VITA_DIAGNOSTICS
        pass_opened = covered                                     ? "covered"
                      : took                                      ? "clear"
                      : pass_upscaled && !pass_upscaled->valid    ? "magnified load"
                                                                  : "load";
#endif
        if (covered && pass_margin != 0) {
            // Its first draw covers the buffer, not the widescreen margins: black where no draw reaches.
            backend.BeginPass(t, 0xff000000u, bg_depth);
            backend.TouchAllTiles();
            covered_passes++;
        } else if (covered) {
            backend.BeginPassOver(t, bg_depth);
            covered_passes++;
        } else if (took) {
            backend.BeginPass(t, ClearColour(target.format, clear.value, clear.bytes), bg_depth);
            if (pass_upscaled) {
                // Every tile stores, so the picture is the guest's buffer in all of it.
                backend.TouchAllTiles();
            }
#ifdef VITA_DIAGNOSTICS
        } else if (static const bool no_load = SgxdFlag("noload"); no_load) {
            // sgxd_flags `noload`: the pass starts from a clear, its picture's load left out.
            backend.BeginPass(t, 0, bg_depth);
#endif
        } else {
            const UntileFormat f = UntileFormatFor(
                static_cast<Pica::TexturingRegs::TextureFormat>(static_cast<u32>(target.format)));
            // A narrow buffer loads from its scratch; a scaled pass loads its picture, or the guest's bytes
            // magnified when that is stale.
            const SgxdBackend::UntileSource guest{drawn.addr, drawn.width, height, f.bits,
                                                  f.code,     f.swizzle,   0};
            if (pass_upscaled) {
                backend.BeginPassLoadedScaled(t, bg_depth, guest, !pass_upscaled->valid);
            } else {
                backend.BeginPassLoaded(t, bg_depth, guest);
            }
            if (loads++ < 4) {
                LOG_INFO(Render, "{}: the colour buffer at {:#x} loaded from guest memory",
                         describe(), colour);
            }
        }
        backend.SetBackgroundStencil(bg_stencil);
        pass_bg_depth = bg_depth;
        pass_bg_stencil = bg_stencil;
        pass_open_draws = backend.PassDraws();
        pass_drawn_lo = pass_drawn_hi = 0;
        pass_guest_draws = 0;
    }
    pass_guest_draws++;
    // After the pass's load, whose draws cover the margin too.
    if (view_mask.x1 != 0 && pass_mask != view_mask) {
        DEBUG_ASSERT(pass_mask.x1 == 0);
        const bool opening = backend.PassDraws() == pass_open_draws;
        // A viewport as wide as the screen keeps the widescreen margins beside it too.
        const s32 widen =
            pass_margin != 0 && view_mask.x1 - view_mask.x0 >= TopScreenRows ? 1 : 0;
        backend.MaskOutside(PassX(view_mask.x0, -widen), PassX(view_mask.x1, widen),
                            PassEdge(view_mask.y0), PassEdge(view_mask.y1));
        pass_mask = view_mask;
        if (opening) {
            pass_open_draws = backend.PassDraws();
        }
    }
    // Without a clear, the depth is what the last pass on the buffer stored, or what guest memory
    // holds. A draw writing every pixel's depth without reading any (DCMP ALWAYS, no stencil test)
    // needs none, and the pass then knows all of it, as after a clear. Any other draw writing depth
    // starts from the depth before it. A pass storing part of its target needs the part alone written
    // and keeps the depth past it. A clear quad's depth not written yet goes into the store first.
    if (uses_depth && !pass_depth_used) {
        if (DepthStore* d = DepthStoreOf(depth, width, height, pass_depth_format)) {
            SettleDepthClear(*d);
        }
    }
    const bool reads_depth = dcmp != 7 || (stencil && !StencilOverwrites(regs.framebuffer));
    if (dwrite && !reads_depth && !pass_depth_cleared && !pass_depth_load && !pass_depth_covered &&
        CoversTarget(is_indexed, immediate, count, width, height, PartToCover(), true)) {
        const bool part = PartToCover().x1 != 0;
        pass_depth_covered = !part || DepthStoreValid(depth, width, height, pass_depth_format,
                                                      pass_quarters, pass_margin);
        pass_depth_part = part && pass_depth_covered;
    }
    if ((reads_depth || dwrite) && !pass_depth_cleared && !pass_depth_load && !pass_depth_covered) {
        const auto it = std::find_if(depth_stores.begin(), depth_stores.end(), [&](const DepthStore& d) {
            return d.addr == depth && d.width == width && d.height == height &&
                   d.format == pass_depth_format && d.quarters == pass_quarters &&
                   d.margin == pass_margin;
        });
        pass_depth_load = true;
        pass_depth_guest = it == depth_stores.end() || !it->valid;
        pass_ta_w = !pass_depth_guest && it->ta_w;
        pass_w_map = pass_ta_w ? it->ta_map : WDepthMap{};
        if (pass_depth_guest && (pass_quarters != 4 || pass_margin != 0)) {
            // A scaled or widescreen pass with no such depth to load (the buffer just turned
            // scaled; a game never clears its depth, every pass loads the last one's): its store
            // starts at the far plane, as no such pass can load the 1x depth. One frame drawn
            // without the depth before it.
            DepthStore& d = DepthStoreFor(depth, width, height, pass_depth_format, pass_quarters,
                                          pass_margin);
            backend.FillDepthStore(d.store, 1.0f, 0);
            d.valid = true;
            pass_depth_guest = false;
            LOG_INFO(Render, "{}: the {}/4x depth store of {:#x} starts at the far plane",
                     describe(), pass_quarters, depth);
        }
        if (pass_depth_guest) {
            // D24S8's words are ZLS's I24ZI8S (depth the low 24 bits, stencil the top byte),
            // untiled raw into the store (U8U8U8U8, swizzle 0); D24's the same without stencil.
            using DF = FramebufferRegs::DepthFormat;
            const DF df = fb.depth_format;
            DEBUG_ASSERT_MSG(df == DF::D24S8 || df == DF::D24,
                             "sgxd: {}: the depth buffer at {:#x} (format {}) from guest memory: "
                             "only D24 and D24S8 are built",
                             describe(), depth, pass_depth_format);
            const u32 bits = df == DF::D24S8 ? 32 : 24;
            const u32 bytes = width * height * bits / 8;
            if (!guest_memory.IsUncached(depth, bytes)) {
                SgxdCacheRange(memory.GetPhysicalPointer(depth), bytes, SGXD_CACHE_CLEAN,
                               CacheSite::Textures);
            }
            // The store is the pass's transposed target: the untile transposes too.
            DepthStore& d = DepthStoreFor(depth, width, height, pass_depth_format);
            backend.Untile({depth, width, height, bits, bits == 32 ? 0x0cu : 0x98u, 0, 0},
                           d.store.memory.gpu, d.store.width, false, true);
            // Every tile converts to F32, drawn on or not.
            backend.TouchAllTiles();
            if (guest_depth_loads++ < 4) {
                LOG_INFO(Render, "{}: the depth buffer at {:#x} ({}x{} format {}) loaded from "
                                 "guest memory",
                         describe(), depth, width, height, pass_depth_format);
            }
        }
    }
    pass_depth_used |= uses_depth;
    pass_depth_written |= dwrite || (stencil && StencilWrites(regs.framebuffer));
    // A pass that loaded guest memory's depth, or a scaled one, stores all of it: a draw writing depth
    // past the part where the PICA's viewport clips must not. The ISP's mask over the margin keeps it
    // out from here on (the draws stay opaque, where a kill made them punch-through). Any other pass
    // storing a part of its target takes the mask from its first draw too: the SGX draws a triangle
    // out to the target's edge, and fragments past the part would be shaded for nothing.
    const bool margin_mask =
        pass_quarters == 4 && pass_margin == 0 && pass_samples == 1 && !pass_narrow;
    if (pass_part.x1 != 0 && pass_mask.x1 == 0 &&
        (margin_mask || (PassStoresWholeDepth() && (dwrite || stencil)))) {
        const bool opening = backend.PassDraws() == pass_open_draws;
        const s32 widen = pass_widened ? 1 : 0;
        pass_margin_masked = margin_mask && pass_guest_draws == 1;
        backend.MaskOutside(PassX(pass_part.x0, -widen), PassX(pass_part.x1, widen),
                            PassEdge(pass_part.y0), PassEdge(pass_part.y1), pass_margin_masked);
        pass_mask = pass_part;
        if (opening) {
            pass_open_draws = backend.PassDraws();
        }
    }
    // A mask inside the part keeps the draw inside it already.
    const bool masked_inside = pass_mask.x1 != 0 && pass_mask.x0 >= pass_part.x0 &&
                               pass_mask.x1 <= pass_part.x1 && pass_mask.y0 >= pass_part.y0 &&
                               pass_mask.y1 <= pass_part.y1;
    draw_kills_viewport = PassStoresWholeDepth() && pass_part.x1 != 0 && (dwrite || stencil) &&
                          !masked_inside;
    {
        // The scissor box in WPOS (pixel centres from the target's top-left). The target is transposed:
        // WPOS x is the PICA's y mirrored like the viewport's corner, WPOS y the PICA's x. A draw killing
        // past its viewport takes its box's overlap with the viewport.
        const auto& rs = regs.rasterizer;
        const auto& sc = rs.scissor_test;
        s32 x1 = static_cast<s32>(sc.x1.Value()), x2 = static_cast<s32>(sc.x2.Value()) + 1;
        s32 y1 = static_cast<s32>(sc.y1.Value()), y2 = static_cast<s32>(sc.y2.Value()) + 1;
        if (draw_kills_viewport) {
            if (sc.mode != Pica::RasterizerRegs::ScissorMode::Include) {
                x1 = y1 = 0;
                x2 = y2 = 0x10000;
            }
            const s32 vx = rs.viewport_corner.x, vy = rs.viewport_corner.y;
            x1 = std::max(x1, vx);
            x2 = std::min(x2,
                          vx + static_cast<s32>(f24::FromRaw(rs.viewport_size_x).ToFloat32() * 2));
            y1 = std::max(y1, vy);
            y2 = std::min(y2,
                          vy + static_cast<s32>(f24::FromRaw(rs.viewport_size_y).ToFloat32() * 2));
        }
        const s32 target_height = static_cast<s32>(height);
        // In the pass's pixels: scaled, each edge at the first pixel centre past it, past the margin; a box
        // over all of a widened pass's part takes the margins too.
        const s32 sx1 = target_height - y2, sx2 = target_height - y1;
        const s32 span0 = pass_part.x1 != 0 ? static_cast<s32>(pass_part.x0) : 0;
        const s32 span1 = pass_part.x1 != 0 ? static_cast<s32>(pass_part.x1) : target_height;
        const s32 widen =
            pass_margin != 0 && (pass_widened || pass_part.x1 == 0) && sx1 <= span0 && sx2 >= span1
                ? 1
                : 0;
        fs_data.scissor_x1 = PassX(sx1, -widen);
        fs_data.scissor_x2 = PassX(sx2, widen);
        fs_data.scissor_y1 = PassEdge(x1);
        fs_data.scissor_y2 = PassEdge(x2);
    }

    // --- the vertex stage: the loaders' attributes into the vertex program, or with a
    // geometry shader, the triangles its passes stored ---
    draw_view = DrawViewport();
    const VertexStage stage = pipeline.use_gs == Pica::PipelineRegs::UseGS::Yes
                                  ? GeometryStage(is_indexed, describe)
                                  : VertexStageFor(is_indexed, immediate, count,
                                                   Usse::VsMode::Draw, describe);
    if (stage.count == 0) {
        return true; // the geometry program stored no triangle
    }
    const VertexProgram& vp = *stage.vp;
    const Usse::Program& vg = vp.usse;

    // --- the vertex program's SA block and uniform buffer ---
    u32 vsec_sa = 0;
    const SgxdBackend::Data vsa = VertexSa(vp, *stage.setup, vsec_sa, describe);
    const float* const vsa_f = reinterpret_cast<const float*>(vsa.cpu);
    const u32 vsa_words = vsec_sa;

    // --- the fragment program and its SA block ---
    const FragmentProgram& fp = FragmentProgramFor(epilogue);
    const Usse::Program& fg = fp.usse;
#ifdef VITA_DIAGNOSTICS
    // A recorded frame's draw (vprof) and its fragment program's dumpusse key.
    if (vprof_draw_noted) {
        LOG_INFO(Render, "vprof draw {}: fs {:016x}, {} words, {} vertices, {}{}, target {:#x}",
                 vprof_draws - 1, last_fragment_key, fg.primary.size(), stage.count,
                 translucent ? "translucent" : "opaque", fg.uses_discard ? " punch-through" : "",
                 fb.GetColorBufferPhysicalAddress());
    }
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
    // The data block follows the resident uniforms: the memory buffer's pointer, the literals, the
    // samplers' words.
    const u32 head = fg.uniform_floats + Usse::DataHead(fg);
    const u32 fsa_words = head + static_cast<u32>(fg.literals.size()) +
                          static_cast<u32>(fg.samplers.size()) * 4;
    SgxdBackend::Data fsa{};
    u32 unit_words[3][4]; // each texture unit's words, for its sampler and a PDS issue
    // A unit's texture and words, for its alpha sampler. The units the pixel PDS reads have no
    // sampler: their words here.
    const Texture* unit_texture[3] = {};
    for (const Usse::TextureIssue& issue : fg.issues) {
        if (!issue.fog && !issue.alpha && !issue.proctex) {
            unit_texture[issue.unit] = &TextureWords(issue.unit, unit_words[issue.unit]);
        }
    }
    if (fsa_words != 0) {
        // Made in cached memory and copied out once (the pass data is uncached); the buffer's pointer goes
        // in once the buffer is placed.
        fsa_scratch.assign(fsa_words, 0);
        u8* const fsa_cpu = reinterpret_cast<u8*>(fsa_scratch.data());
        float* const fsa_f = reinterpret_cast<float*>(fsa_cpu);
        u8* fbuf_cpu = nullptr;
        if (fg.buffer_pointer) {
            fbuf_scratch.assign(fg.buffer_floats, 0);
            fbuf_cpu = reinterpret_cast<u8*>(fbuf_scratch.data());
        }
        const u8* const fs_bytes = reinterpret_cast<const u8*>(&fs_data);
        for (const UniformSource& u : fp.uniforms) {
            const u8* src = fs_bytes + u.offset;
            if (u.u8) {
                // A word an element, the colour's channels as bytes, rounded as the program's pack rounds.
                for (u32 e = 0; e < u.count; e++, src += u.stride) {
                    u32 word = 0;
                    for (u32 c = 0; c < u.components; c++) {
                        float value;
                        std::memcpy(&value, src + (u.lane >= 0 ? u.lane : c) * 4, 4);
                        word |=
                            static_cast<u32>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f))
                            << (8 * c);
                    }
                    std::memcpy(fsa_cpu + (u.sa + e) * 4, &word, 4);
                }
                continue;
            }
            if (u.f16) {
                // A pair an element, four F16 (Usse::Uniform::f16).
                for (u32 e = 0; e < u.count; e++, src += u.stride) {
                    u16 half[4];
                    for (u32 c = 0; c < 4; c++) {
                        float value;
                        std::memcpy(&value, src + c * 4, 4);
                        half[c] = ToF16(value);
                    }
                    std::memcpy(fsa_cpu + (u.sa + e * 2) * 4, half, sizeof(half));
                }
                continue;
            }
            float* dst = u.sa < fg.uniform_floats ? fsa_f + u.sa
                                                  : reinterpret_cast<float*>(fbuf_cpu) + u.sa;
            if (u.dot3_const) {
                float c[3];
                std::memcpy(c, src, sizeof(c));
                float sum = 0.0f;
                for (u32 k = 0; k < 3; k++) {
                    const float b = static_cast<float>(std::lround(
                                        std::clamp(c[k], 0.0f, 1.0f) * 255.0f)) / 255.0f - 0.5f;
                    dst[k] = 4.0f * b;
                    sum += b;
                }
                dst[3] = -2.0f * sum;
                continue;
            }
            if (u.lut_row != 0) {
                // The row's centre, v = (offset / 256 + 0.5) / LUT_LF_ROWS; a pair's x first.
                s32 value;
                std::memcpy(&value, src, 4);
                const float row = (static_cast<float>(value / 256) + 0.5f) /
                                  static_cast<float>(Pica::Shader::LUT_LF_ROWS);
                if (u.lut_row == 1) {
                    dst[0] = row;
                } else {
                    dst[0] = (u.lut_row == 2 ? 0.5f : 127.5f) / 256.0f;
                    dst[1] = row;
                }
                continue;
            }
            for (u32 e = 0; e < u.count; e++, src += u.stride, dst += 4) {
                for (u32 c = 0; c < u.components; c++) {
                    if (u.is_int) {
                        s32 value;
                        std::memcpy(&value, src + c * 4, 4);
                        dst[c] = static_cast<float>(value);
                    } else {
                        std::memcpy(dst + c, src + c * 4, 4);
                        if (u.dist_atten == 2) {
                            dst[c] += 0.5f / 256.0f;
                        }
                    }
                }
                if (u.normalize) {
                    // As the program would: x * rsq(dot(x, x)).
                    const float scale =
                        1.0f / std::sqrt(dst[0] * dst[0] + dst[1] * dst[1] + dst[2] * dst[2]);
                    dst[0] *= scale;
                    dst[1] *= scale;
                    dst[2] *= scale;
                }
            }
        }
        for (size_t i = 0; i < fg.literals.size(); i++) {
            std::memcpy(fsa_cpu + Usse::LiteralSaOffset(head, static_cast<u32>(fg.samplers.size()),
                                                        static_cast<u32>(i)) *
                                      4,
                        &fg.literals[i], 4);
        }
        // The LUT textures, linear and point sampled (the program computes texel centres): F32F32 (value,
        // delta) for lighting, fog and the proctex maps, F16x4 for its colours, read by the float4 query.
        for (size_t i = 0; i < fg.samplers.size(); i++) {
            const Usse::Sampler& sampler = fg.samplers[i];
            u32 words[4];
            switch (sampler.kind) {
            case Usse::SamplerKind::Texture:
                unit_texture[sampler.index] = &TextureWords(sampler.index, words);
                std::memcpy(unit_words[sampler.index], words, sizeof(words));
                break;
            case Usse::SamplerKind::TextureAlpha:
                // After the unit's own sampler (PlanInputs puts it last).
                DEBUG_ASSERT(unit_texture[sampler.index] != nullptr);
                AlphaTextureWords(*unit_texture[sampler.index], unit_words[sampler.index], words);
                break;
            case Usse::SamplerKind::LutRg:
                sgxd_recipe_texture_words_format(backend.LutRgRows().gpu,
                                                 Pica::Shader::LUT_TEX_WIDTH,
                                                 Pica::Shader::LUT_RG_ROWS, 0, LutFormatF32F32, 0,
                                                 words);
                break;
            case Usse::SamplerKind::ProctexNoise:
                // F16x4, bilinear: NoiseTable's texels.
                sgxd_recipe_texture_words_format(NoiseTable(), 256, 256, 1, LutFormatF16x4, 0,
                                                 words);
                break;
            case Usse::SamplerKind::LutRgba:
                sgxd_recipe_texture_words_format(backend.LutRgbaRows().gpu,
                                                 Pica::Shader::LUT_TEX_WIDTH,
                                                 Pica::Shader::LUT_RGBA_ROWS, 0, LutFormatF16x4, 0,
                                                 words);
                break;
            case Usse::SamplerKind::LutLf:
                sgxd_recipe_texture_words_format(backend.LutRows().gpu,
                                                 Pica::Shader::LUT_TEX_WIDTH,
                                                 Pica::Shader::LUT_LF_ROWS, 0, LutFormatF32F32, 0,
                                                 words);
                break;
            case Usse::SamplerKind::LutLfHalf:
            case Usse::SamplerKind::LutLfHalfWrap:
                sgxd_recipe_texture_words_format(
                    backend.LutLfHalfRows().gpu, Pica::Shader::LUT_TEX_WIDTH,
                    Pica::Shader::LUT_LF_ROWS, 1, LutFormatF16, 0, words);
                if (sampler.kind == Usse::SamplerKind::LutLfHalfWrap) {
                    words[0] &= ~(7u << 6); // UADDRMODE REPEAT
                }
                break;
            }
            std::memcpy(fsa_cpu + Usse::SamplerSaOffset(head, static_cast<u32>(i)) * 4, words,
                        sizeof(words));
        }
        // Placed in the pass data, or the program's last copies in this pass again when they are the same.
        const u32 pass = backend.DataOpens();
        const bool same_pass = fp.sa_last_pass == pass;
        if (fbuf_cpu != nullptr) {
            SgxdBackend::Data fbuf = fp.buf_last_data;
            if (!same_pass || fp.buf_last != fbuf_scratch) {
                fbuf = backend.AllocData(fg.buffer_floats * 4);
                std::memcpy(fbuf.cpu, fbuf_cpu, fg.buffer_floats * 4);
                fp.buf_last = fbuf_scratch;
                fp.buf_last_data = fbuf;
            }
            const u32 pointer = fbuf.gpu + static_cast<u32>(fg.ldst_base_value);
            std::memcpy(fsa_cpu + fg.uniform_floats * 4, &pointer, 4);
        }
        if (same_pass && fp.sa_last == fsa_scratch) {
            fsa = fp.sa_last_data;
        } else {
            fsa = backend.AllocData(fsa_words * 4);
            std::memcpy(fsa.cpu, fsa_cpu, fsa_words * 4);
            fp.sa_last = fsa_scratch;
            fp.sa_last_data = fsa;
        }
        fp.sa_last_pass = pass;
    }

    // --- the PDS programs ---
    SgxdRecipeDrawPrograms p{};
    VertexPrograms(stage, vsa, vsec_sa, p, describe);
    const auto fsec = backend.SecondaryPds(fsa.gpu, fsa_words, fp.secondary_doutu0);
    DEBUG_ASSERT(fsec.va != 0);
    p.fsec = fsec.va;
    p.fsec_units = fsec.units;
    if (fg.issues.empty()) {
        const auto fprim = backend.PixelPds(fp.primary_doutu0, fp.iterators.data(),
                                            fp.iterator_words.data(), fp.iterator_count);
        DEBUG_ASSERT_MSG(fprim.va != 0, "sgxd: pixel PDS with {} iterators", fp.iterator_count);
        p.fprim = fprim.va;
        p.fprim_units = fprim.units;
    } else {
        std::array<SgxdTextureIssue, Usse::MaxTextureIssues> issues{};
        DEBUG_ASSERT(fg.issues.size() <= issues.size());
        for (size_t i = 0; i < fg.issues.size(); i++) {
            const Usse::TextureIssue& issue = fg.issues[i];
            if (issue.fog) {
                FogTableWords(regs.texturing.fog_flip != 0, issues[i].tex);
                issues[i].raw = 1u;
                // TEXCOORD3's u (depth times w) over t (w): the PICA's Z depth; W-buffered, u alone.
                issues[i].proj_t = regs.rasterizer.depthmap_enable ==
                                           Pica::RasterizerRegs::DepthBuffering::WBuffering &&
                                       !regs.framebuffer.IsShadowRendering()
                                   ? 0u
                                   : 1u;
            } else if (issue.proctex) {
                ProcTexTableWords(issues[i].tex);
            } else if (issue.alpha) {
                DEBUG_ASSERT(unit_texture[issue.unit] != nullptr);
                AlphaTextureWords(*unit_texture[issue.unit], unit_words[issue.unit], issues[i].tex);
            } else {
                std::memcpy(issues[i].tex, unit_words[issue.unit], sizeof(issues[i].tex));
            }
            issues[i].coord = issue.coord;
            issues[i].after = issue.after_input;
            issues[i].f16 = issue.f16 ? 1u : 0u;
        }
        const auto fprim = backend.PicaPixelPds(fp.primary_doutu0, fp.iterators.data(),
                                                fp.iterator_words.data(), fp.iterator_count,
                                                issues.data(), static_cast<u32>(fg.issues.size()));
        DEBUG_ASSERT_MSG(fprim.va != 0, "sgxd: pixel PDS with {} texture issues", fg.issues.size());
        p.fprim = fprim.va;
        p.fprim_units = fprim.units;
    }
    if (fp.pixel_size_sa != fsa_words) {
        fp.pixel_size = sgxd_program_pixel_dms(fp.pixel_pa, fg.temp_count, fsa_words);
        fp.pixel_size_sa = fsa_words;
    }
    p.pixel_size = fp.pixel_size;
    // The target is the buffer transposed (SgxdBackend::PicaTarget): the vertex program
    // writes NDC y as clip x and NDC x as clip y, so pixel column x is memory row x and pixel
    // row y the PICA's x. The program writes z as -(2z + w), which the TA clips to the PICA's
    // planes (VertexEmitter::Epilogue); the viewport's z maps it to the PICA depth,
    // z / w * depth_scale + depth_offset.
    const float half_w = f24::FromRaw(regs.rasterizer.viewport_size_x).ToFloat32();
    const float half_h = f24::FromRaw(regs.rasterizer.viewport_size_y).ToFloat32();
    // The PICA's window y runs up from the target's last memory row: the corner is mirrored as upstream's y-down Vulkan path
    // mirrors it (FramebufferHelper, flip_rect), and NDC y +1 is the target's first memory row
    // (the top of the PICA's window), so its scale is negative. With it positive every draw was
    // mirrored along the rows.
    // In the pass's pixels: the PICA's scaled with the pass (the upscaled top screen).
    // Widescreen, past the picture's left margin.
    const float vs = static_cast<float>(pass_quarters) / 4.0f;
    p.viewport[0] = (static_cast<float>(height + pass_margin) -
                     static_cast<float>(regs.rasterizer.viewport_corner.y) - half_h) *
                    vs;
    p.viewport[1] = -half_h * vs;
    p.viewport[2] = (static_cast<float>(regs.rasterizer.viewport_corner.x) + half_w) * vs;
    p.viewport[3] = half_w * vs;
    p.viewport[4] = fs_data.depth_offset - fs_data.depth_scale / 2.0f;
    p.viewport[5] = -fs_data.depth_scale / 2.0f;
    if (Usse::z_flip) {
        // z written as 2z + w: z / w * depth_scale / 2 + depth_offset - depth_scale / 2.
        p.viewport[5] = fs_data.depth_scale / 2.0f;
    }
    // W-buffering: the TA's WBUFFEN gives the ISP the viewport's z mapping of 1/w (docs/sgxd/ta.md),
    // linear on the screen and in the PICA's depth order: 1 - 1/w where the PICA's depth grows with w,
    // else 1/w. The depths stored are these, and a pass's Z-buffered draws do not compare with them as
    // the PICA's would.
    u32 mte = 0;
    if (regs.rasterizer.depthmap_enable == Pica::RasterizerRegs::DepthBuffering::WBuffering &&
        !regs.framebuffer.IsShadowRendering()) {
        const bool grows = fs_data.depth_offset - fs_data.depth_scale > 0.0f;
        if (dwrite) {
            pass_ta_w = true;
            if (!pass_w_map.fitted) {
                FitWDepth(is_indexed, immediate, count, grows);
            }
        }
        // K at or below the draw's near plane, from its position rows or the pass's fit. Depths stored
        // before a change compare against the new ones from the next clear on.
        const float near_w = std::fmin(draw_near_w, pass_w_map.fitted
                                                        ? pass_w_map.near_w
                                                        : std::numeric_limits<float>::quiet_NaN());
        if (near_w < w_depth_k) {
            const float k = std::exp2(std::floor(std::log2(near_w)));
            LOG_INFO(Render, "sgxd: W-buffered depth 1 - K / w: K {} -> {} for a near plane at w {}",
                     w_depth_k, k, near_w);
            w_depth_k = k;
        }
        p.viewport[4] = grows ? 1.0f : 0.0f;
        p.viewport[5] = grows ? -w_depth_k : w_depth_k;
        mte = SGXD_RECIPE_MTE_WBUFFEN;
    } else if (pass_ta_w && dcmp != 7 &&
               regs.rasterizer.depthmap_enable ==
                   Pica::RasterizerRegs::DepthBuffering::ZBuffering &&
               !regs.framebuffer.IsShadowRendering()) {
        // A Z-buffered draw testing against them (the PICA takes the depth mode per draw).
        [[maybe_unused]] const bool mapped = MapZOnW(is_indexed, immediate, count, p.viewport);
    }
    p.primitive = primitive;

    // ISP A: PASSTYPE, DCMP, DWRITEDIS, BPRES. Translucent (1) with a blend or a write mask, else opaque
    // (0); a program that kills is TRANSPT (2) or FASTPT (4): its first phase waits for the ISP's
    // feedback, which the opaque and translucent types never give.
    u32 pass_type = translucent ? 1u : 0u;
    if (fg.uses_discard) {
        pass_type = translucent ? 2u : 4u;
    }
    const u32 isp = pass_type << 25 | dcmp << 22 | (dwrite ? 0u : 1u << 20) | 1u << 9 |
                    (stencil ? SGXD_RECIPE_ISP_CPRES | st.reference_value : 0u);
    const u32 ispc =
        !stencil ? 0u
                 : DepthCompare(st.func) << 25 |
                       static_cast<u32>(st.action_stencil_fail.Value()) << 22 |
                       static_cast<u32>(st.action_depth_fail.Value()) << 19 |
                       static_cast<u32>(st.action_depth_pass.Value()) << 16 |
                       st.input_mask << 8 |
                       (fb.allow_depth_stencil_write != 0 ? static_cast<u32>(st.write_mask) : 0u);
    // A pass masked to the part it stores: nothing of the draw in the tiles past the part's.
    const u32 clip = !pass_margin_masked
                         ? 0u
                         : SGXD_RECIPE_CLIP(static_cast<u32>(PassX(pass_part.x0)) / 32,
                                            static_cast<u32>(PassEdge(pass_part.y0)) / 32,
                                            (static_cast<u32>(PassX(pass_part.x1)) - 1) / 32,
                                            (static_cast<u32>(PassEdge(pass_part.y1)) - 1) / 32);
    const SgxdRecipeDraw draw{0, stage.count, isp, SGXD_R_FPRIM,
                              CullMode(regs.rasterizer.cull_mode), clip, nullptr, ispc, mte};
    backend.Draw(draw, p);
    if (const auto [lo, hi] = DrawnRows(); lo < hi) {
        const bool first = pass_drawn_lo >= pass_drawn_hi;
        pass_drawn_lo = first ? lo : std::min(pass_drawn_lo, lo);
        pass_drawn_hi = first ? hi : std::max(pass_drawn_hi, hi);
    }
#ifdef VITA_DIAGNOSTICS
    PerflogDraw(describe, isp, blend, p);
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

} // namespace SgxdRenderer
