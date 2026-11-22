// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <array>
#include <bit>
#include <stdexcept>
#include <string>
#include <fmt/format.h>
#include "sgxd_program.h"
#include "video_core/renderer_gxm/usse/fs_usse_gen.h"
#include "video_core/renderer_gxm/usse/usse_encoder.h"
#include "video_core/renderer_gxm/usse/usse_ir.h"
#include "video_core/renderer_gxm/usse/usse_program.h"

namespace GxmRenderer::Usse {

namespace {
} // namespace

namespace {

using Pica::FramebufferRegs;
using Pica::LightingRegs;
using Pica::TexturingRegs;
using Pica::Shader::FSConfig;
using TevStageConfig = TexturingRegs::TevStageConfig;
using Source = TevStageConfig::Source;
using Operation = TevStageConfig::Operation;
using ColorModifier = TevStageConfig::ColorModifier;
using AlphaModifier = TevStageConfig::AlphaModifier;
using TextureType = TexturingRegs::TextureConfig::TextureType;
using Sampler = LightingRegs::LightingSampler;
using LutInput = LightingRegs::LightingLutInput;

// A PreviousBuffer source's alpha is the last alpha update's, its colour the last colour
// update's.
bool ReadsAlphaLane(ColorModifier m) {
    return (static_cast<u32>(m) & ~1u) == 0x2; // SourceAlpha, OneMinusSourceAlpha
}
bool ReadsAlphaLane(AlphaModifier m) {
    return ((static_cast<u32>(m) >> 1) & 3) == 0; // SourceAlpha, OneMinusSourceAlpha
}

// The colour channel (0-2) a modifier moves into place, SourceRed, Green or Blue or their
// complements; -1 for the colour or the alpha itself.
int MovedLane(ColorModifier m) {
    const u32 v = static_cast<u32>(m);
    return v >= 4 ? static_cast<int>(v >> 2) - 1 : -1;
}
int MovedLane(AlphaModifier m) {
    const u32 v = static_cast<u32>(m);
    return v >= 2 ? static_cast<int>(v >> 1) - 1 : -1;
}

// The U8 combiners' inputs packed from F32 or F16 lanes, whose pack can move a channel: their
// index into FragmentEmitter::u8_bcast, or -1.
int BroadcastInput(Source s) {
    switch (s) {
    case Source::PrimaryColor:
        return 0;
    case Source::PrimaryFragmentColor:
        return 1;
    case Source::SecondaryFragmentColor:
        return 2;
    case Source::Texture0:
        return 3;
    case Source::Texture1:
        return 4;
    case Source::Texture2:
        return 5;
    case Source::Texture3:
        return 6;
    default:
        return -1;
    }
}

// The generator's notion of a stage that changes nothing.
bool IsPassThroughTevStage(const TevStageConfig& stage) {
    return (stage.color_op == Operation::Replace && stage.alpha_op == Operation::Replace &&
            stage.color_source1 == Source::Previous && stage.alpha_source1 == Source::Previous &&
            stage.color_modifier1 == ColorModifier::SourceColor &&
            stage.alpha_modifier1 == AlphaModifier::SourceAlpha &&
            stage.GetColorMultiplier() == 1 && stage.GetAlphaMultiplier() == 1);
}

// A procedural texture of u alone that an address mode reproduces (MIRROR_CLAMP of the iterated
// u is min(|u|, 1)): a table the pixel PDS reads (TextureIssue::proctex).
bool ProcTexTabled(const Pica::Shader::ProcTexConfig& pt) {
    using Clamp = TexturingRegs::ProcTexClamp;
    using Combiner = TexturingRegs::ProcTexCombiner;
    using Filter = TexturingRegs::ProcTexFilter;
    using Shift = TexturingRegs::ProcTexShift;
    const auto of_u = [](Combiner c) { return c == Combiner::U || c == Combiner::U2; };
    return !pt.noise_enable && pt.u_shift != Shift::Odd && pt.u_shift != Shift::Even &&
           (pt.lut_filter == Filter::Linear || pt.lut_filter == Filter::Nearest) &&
           (pt.u_clamp == Clamp::ToEdge || pt.u_clamp == Clamp::MirroredRepeat) &&
           of_u(pt.color_combiner) && (!pt.separate_alpha || of_u(pt.alpha_combiner));
}

// The generator's stage-0 quirk: Previous on the first stage reads that stage's source 3.
Source ColorSource(const TevStageConfig& stage, Source source, u32 index) {
    return (index == 0 && source == Source::Previous) ? stage.color_source3.Value() : source;
}

Source AlphaSource(const TevStageConfig& stage, Source source, u32 index) {
    return (index == 0 && source == Source::Previous) ? stage.alpha_source3.Value() : source;
}

/**
 * The register plan: everything long-lived in the unified store,
 * the three internal registers only inside one pass, with nosched set for as long as one
 * of them holds a value.
 *
 * Primary attributes: pa0-3 COLOR0, then the inputs a configuration needs.
 * Temporaries (32-bit each), fixed homes:
 */
constexpr uint8_t TmpOutput = 0;      ///< combiner_output, the running result (4)
constexpr uint8_t TmpTex0 = 4;        ///< the three texture units' colours (4 each)
constexpr uint8_t TmpBuffer = 16;     ///< combiner_buffer (4)
constexpr uint8_t TmpNextBuffer = 20; ///< next_combiner_buffer (4)
constexpr uint8_t TmpTexFlip = 16;    ///< the texture units' (s, 1 - t) (2 each), in the
                                      ///< combiner buffers' homes before the stages run
constexpr uint8_t TmpPrimary = 24;    ///< the vertex colour, byte-rounded (4)
constexpr uint8_t TmpArg = 28;        ///< the three stage arguments (4 each); lighting
                                      ///< scratch before the stages run
constexpr uint8_t TmpAlphaSplit = 28; ///< a split unit's alpha texel (4 each,
                                      ///< FSConfig alpha_split), in the stage arguments'
                                      ///< homes while SampleTextures runs
constexpr uint8_t TmpLutCoord = 28;   ///< lighting/fog: the LUT texel coordinate (2)
constexpr uint8_t TmpLutDelta = 30;   ///< lighting/fog: the interpolation weight (1)
constexpr uint8_t TmpLutEntry = 32;   ///< lighting/fog: the fetched LUT entry (4)
constexpr uint8_t TmpHalf = 36;       ///< lighting: the normalised half vector (3)
constexpr uint8_t TmpLight = 40;      ///< lighting: the normalised light vector (3)
constexpr uint8_t TmpMisc = 44;       ///< lighting: dot product, clamp_highlights, distance,
                                      ///< the squared half vector length (4)
constexpr uint8_t TmpParked = 44;     ///< a stage's colour while its alpha's three arguments
                                      ///< load (4), in TmpMisc's home: lighting is done by then
constexpr uint8_t TmpNormal = 48;     ///< lighting: the surface normal in view space (3)
constexpr uint8_t TmpView = 52;       ///< lighting: the normalised view vector (3)
constexpr uint8_t TmpDiffuse = 56;    ///< lighting: diffuse_sum, primary_fragment_color (4)
constexpr uint8_t TmpSpecular = 60;   ///< lighting: specular_sum, secondary_fragment_color (4)
constexpr uint8_t TmpTangent = 16;    ///< lighting: the tangent (Config7 only) (3), in the
                                      ///< combiner buffer's home, which is filled afterwards
constexpr uint8_t TmpLightAtten = 0;  ///< lighting: per-light attenuation (.x) and geometric
                                      ///< factor (.y), in combiner_output's home (2)
constexpr uint8_t TmpShadow = 64;     ///< lighting: the shadow texture's colour (4; .x
                                      ///< alone for a Shadow2D unit's, shadow_scalar)
constexpr uint8_t TmpLightMem = 68;   ///< lighting: a light's parameters loaded from the
                                      ///< uniform buffer (24: the SA layout, six quads)
constexpr uint8_t TmpProj = 92;       ///< unit 0's projected coordinate (2), its cube
                                      ///< direction (3), or the depth (1)
constexpr uint8_t TmpTex3 = 96;       ///< the procedural texture's colour (4)
constexpr uint8_t TmpProcUv = 100;    ///< proctex: u, v (2), the level (.z)
constexpr uint8_t TmpSnap = 104;      ///< a stage's output kept for a later stage's
                                      ///< PreviousBuffer read (4 per stage, 0-3)
constexpr uint8_t TmpBatch = 128;     ///< reads issued together before one WDF: read k's
                                      ///< coordinate (2) and interpolation weight (.z) at
                                      ///< 128 + 4k, k < 8
constexpr uint8_t TmpBatchTexel = 160; ///< read k's texel (4) at 160 + 4k
constexpr uint8_t TmpNoise = 120;     ///< proctex noise: the grid point mod 144 (2); the
                                      ///< fraction (2); the noise LUT at it (2)
// U8 values take one register each, four to a quad (liveness is per lane). The stage outputs take
// even lanes of TmpSnap's quads: the alpha test's and the fog's unpacks read an even register.
// The proctex batch quads are shared: only the F32 combiners and the proctex, run first, use them.
constexpr uint8_t TmpU8Stage = TmpSnap; ///< stage k's output at TmpSnap + 2k
constexpr uint8_t TmpU8Scratch2 = 184;  ///< the alpha test's integer alpha (even: VTST reads pairs)
constexpr uint8_t TmpU8Colour = 185;    ///< a stage's colour result when its alpha op differs
constexpr uint8_t TmpU8Final = 186;     ///< the output, when no stage wrote one (even)
constexpr uint8_t TmpU8FogFactor = 187;
constexpr uint8_t TmpU8Fogged = 189;
constexpr uint8_t TmpU8White = 192;     ///< 0xffffffff
constexpr uint8_t TmpU8Zero = 193;
constexpr uint8_t TmpU8Half = 194; ///< 0x7f7f7f7f, AddSigned's 0.5 as the F32 path rounds it
constexpr uint8_t TmpU8Primary = 195;
constexpr uint8_t TmpU8Tex = 196; ///< texture units 0-2, one register each
constexpr uint8_t TmpU8Tex3 = 199;
constexpr uint8_t TmpU8Diffuse = 200;
constexpr uint8_t TmpU8Specular = 201;
constexpr uint8_t TmpU8Const = 203; ///< a SOP3 source 0's copy of a secondary attribute
constexpr uint8_t TmpU8Arg = 204; ///< the three arguments, one register each
constexpr uint8_t TmpU8Scratch = 207;
// The U8 combiners' inputs with one channel in every byte (U8Broadcast), one register each.
constexpr uint8_t TmpU8Broadcast = 236;
constexpr uint32_t U8BroadcastMax = 8;
/// A moved channel of a register U8Inputs does not broadcast (a stage's output, a constant, an
/// own-pixel texel), made where an argument reads it (U8LaneBroadcast): argument k's colour at
/// 248 + k, its alpha at 252 + k.
constexpr uint8_t TmpU8LaneColour = 248;
constexpr uint8_t TmpU8LaneAlpha = 252;
/// Even copies of U8 registers a VPCK unpacks (U8Even), in the proctex batch quads the combiners
/// leave alone.
constexpr uint8_t TmpU8EvenA = 188;
constexpr uint8_t TmpU8EvenB = 190;
constexpr uint8_t TmpLightAcc = 20;   ///< lighting: the spot direction, then the reflection
                                      ///< vector, then the specular accumulator, in
                                      ///< next_combiner_buffer's home (3). Nothing lighting
                                      ///< keeps may sit under a LUT fetch's scratch
                                      ///< (TmpLutCoord..TmpLutEntry + 3) or the half vector.

// The hardware constant bank as the disassembly names it: c<n>.x is table entry n and .y
// entry n + 1 (the bank is not pair-addressed). Entry 0 is 0, entries 2-3 are 1, entry 4
// is 2, entry 12 is 0.5 (Vita3K's usse_constant_table.h; confirmed by psp2shaderperf).
const Src ConstZero{Reg::C(0), XXXX};
const Src ConstOne{Reg::C(2), XXXX};
const Src ConstHalf{Reg::C(12), XXXX};

const Reg I0 = Reg::I(0);
const Reg I1 = Reg::I(1);
const Reg I2 = Reg::I(2);

// The literals follow the uniforms. The first is the kill's operand, the raw word 0xE000 (the
// second pads it to a pair): ISP feedback word 0 with ACMPMODE ALWAYS, AREF 0 and depth writes
// on, so the kill's predicate alone decides. Other modes compare an 8-bit value against AREF,
// and DWDISABLE does not stop the depth write.
enum Lit : uint8_t {
    LitKillControl,
    LitKillControlPad,
    LitTwoFiftyFive,
    LitInv255,
    LitTwo,
    LitFour,
    Lit128,
    Lit127,
    LitInv256,
    LitInvLutRows,
    LitHalfInv256,
    Lit256,
    Lit255,
    LitNeg128,
    // Procedural textures: the LUT texture rows, the configuration's constants.
    LitInvRgRows,
    LitProcWidth,   ///< lut_width
    LitProcWidthM1, ///< lut_width - 1
    LitProcOff0,    ///< the colour LUT's offsets by mip level, 0-3 from the configuration
    LitProcOff1,
    LitProcOff2,
    LitProcOff3,
    LitProc240, ///< and 4-7 fixed
    LitProc248,
    LitProc252,
    LitProc254,
    LitProcLodMin,
    LitProcLodMax,
    LitProcPad,
    LitPlusTwo, ///< the (2, -2) pair the default normal's rotation multiplies by
    LitMinusTwo,
    // LUT texel coordinates: (column, row) * scale + bias, pairs.
    LitLutScaleX, ///< 1, 1 / LUT_LF_ROWS
    LitLutScaleY,
    LitLutBiasX, ///< 0.5 / 256, 0.5 / LUT_LF_ROWS
    LitLutBiasY,
    LitRgScaleX, ///< the same for the RG / RGBA LUT rows
    LitRgScaleY,
    LitRgBiasX,
    LitRgBiasY,
    LitNeg127, ///< the clamp bound of a negated LUT index
    LitNeg127Pad,
    LitNine, ///< proctex noise: the grid's 9, the hash's period 144 and its inverse
    Lit144,
    LitInv144,
    LitNoisePad,
    LitU8White, ///< the U8 combiners' raw words: 0xffffffff, and 127 in each byte
    LitU8Half,
    LiteralCount, // even, so the sampler words stay pair-aligned
};
static_assert(LiteralCount % 2 == 0);

/// Whether a light's vector comes normalised in its uniform (UniformKind::LightDirection): a
/// directional light's, unless its distance attenuation reads the position's length.
bool LightPreNormalised(const auto& light) {
    return light.directional && !light.dist_atten_enable;
}

// A float4 somewhere in the unified store: two 64-bit pairs at base and base + 2.
struct Vec4Loc {
    Bank bank;
    uint8_t base;
    bool f16 = false; ///< four F16 lanes in the one pair at `base` (an F16 varying)
    Reg Pair(int which) const {
        return {bank, static_cast<uint8_t>(base + which * 2)};
    }
};

class FragmentEmitter {
public:
    FragmentEmitter(const FSConfig& config_, int level_) : config{config_}, level{level_} {}

    std::optional<Program> Emit(std::string* refusal);
    uint32_t SampledUnits() {
        PlanInputs();
        return (unit_used[0] ? 1u : 0u) | (unit_used[1] ? 2u : 0u) | (unit_used[2] ? 4u : 0u);
    }
    int level = 1;

private:
    /// Null when the configuration can be emitted, else what stops it.
    const char* Unsupported() const;
    void PlanInputs();
    void PlanUniforms();
    void SampleTextures();
    void RoundPrimary();
    void Lighting();
    void LightingLutLookup(Sampler sampler, bool abs_input, bool two_sided);
    /// LightingLutLookup's second half, from TmpLutDelta (x the fraction, y the negated
    /// index): the wrap, the texel and the interpolation, into i0.x. A LUT at the input the
    /// last lookup indexed goes on here from the same TmpLutDelta.
    void LightingLutRowFetch(Sampler sampler, bool abs_input);
    void LutLfFetch();
    /// A LUT texel fetch: i1.y holds the index, `sampler_index` names the LUT texture (256
    /// texels a row, `inv_rows` its row count's inverse); the entry lands in TmpLutEntry.
    /// Fetches the LUT entry at index i1.y (or -i1.y when `negated`) of a texture with the
    /// given row scale and bias literal pairs into TmpLutEntry, and waits.
    void LutFetch(uint32_t sampler_index, uint8_t scale_lit, uint8_t bias_lit, bool negated);
    /// LutFetch's texel coordinate alone, into `coord`: the sample is the caller's, so several
    /// can share one WDF.
    void LutCoordInto(Reg coord, uint8_t scale_lit, uint8_t bias_lit, bool negated);
    void ProcTex();
    void ProcTexColour(bool mipmapped);
    /// ProcTexLookupLUT: the rg LUT at i0.x (0..1) plus `offset`, interpolated, into i0.x.
    void ProcTexLookup(Src offset);
    /// ProcTexLookup in three parts, for lookups that share one WDF: the coordinate and
    /// weight of batch slot `slot` from i0.x, its sample, and after the WDF its value into i0.x.
    void ProcTexLookupCoord(Src offset, uint8_t slot);
    void ProcTexLookupSample(uint8_t slot);
    void ProcSample(Reg dst, Reg coord, uint8_t sampler_sa);
    void ProcWdf();
    void ProcTexLookupFinish(uint8_t slot);
    /// The combiner of u and v (TmpProcUv) into i0.x.
    void ProcTexCombine(Pica::TexturingRegs::ProcTexCombiner combiner);
    /// uv (TmpProcUv) += amplitude * the noise coefficient at uv, then |uv|.
    void ProcTexNoise();
    /// The shift term for one axis from the other's coordinate, into i2.x.
    void ProcTexShift(Pica::TexturingRegs::ProcTexShift mode, Pica::TexturingRegs::ProcTexClamp clamp,
                      Src coord);
    /// The clamp of the lanes of i0 in `mask` (x u, y v), in place, through i1.
    void ProcTexClamp(Pica::TexturingRegs::ProcTexClamp mode, uint8_t mask);
    /// floor(i0.<lane>) in place, through i1.
    void FloorLane(uint8_t lane);
    void TevStage(u32 index);
    /// Loads stage argument `arg_index` into i<arg_index>, the lanes in `lanes` (0x7 colour,
    /// 0x8 alpha, 0xf both), straight from its sources.
    void LoadArg(uint8_t arg_index, const TevStageConfig& stage, u32 stage_index, Source csrc,
                 ColorModifier cmod, Source asrc, AlphaModifier amod, uint8_t lanes);
    Vec4Loc SourceLocation(Source source, u32 stage_index, bool alpha) const;
    /// Works out which stage output each PreviousBuffer read sees.
    void PlanCombinerBuffer();
    void ColorOp(Operation op, uint8_t mask);
    /// The alpha test on the F32 combiner output (TmpOutput.w), or on the U8 one's alpha byte
    /// when `u8` is a register.
    void AlphaTest(Reg* u8 = nullptr);
    /// The U8 combiners (u8_tev): whether every stage has a U8 form; the inputs packed to U8; a
    /// source's U8 register; an argument built (its register: the source itself when it needs
    /// no modifier); an operation into `dst`; a stage; and what follows the last stage.
    bool U8TevSupported() const;
    bool StageReads(Source source) const;
    void U8Inputs();
    /// The combiner output in an even register (the alpha test's and the pack's unpacks).
    Reg U8EvenFinal();
    Reg U8Source(Source source, u32 stage_index, bool alpha) const;
    /// `need_colour` / `need_alpha`: whether an operation reads the argument's colour / alpha
    /// (the other lanes are left as they come).
    Reg U8Arg(uint8_t k, u32 stage_index, Source csrc, ColorModifier cmod, Source asrc,
              AlphaModifier amod, bool need_colour = true, bool need_alpha = true);
    /// Calls f(stage index, colour source, colour modifier, alpha source, alpha modifier) for
    /// each argument a U8 stage reads, the sources as ColorSource and AlphaSource give them.
    template <typename F>
    void ForEachU8Arg(F&& f) const;
    /// Whether U8Inputs packs `source` (BroadcastInput's inputs): absent, it reads as zero.
    bool U8InputPresent(Source source) const;
    /// Channel `lane` of a packed input in every byte, for the channel-moving modifiers: the
    /// input packed again with each byte from that lane (U8Inputs, PackSelBroadcast).
    Reg U8Broadcast(Source source, int lane) const;
    void U8Op(Operation op, Reg dst, const std::array<Reg, 3>& args);
    /// r, or its copy in the even register `even` (r odd, or an odd secondary attribute): what
    /// a U8 unpack reads.
    Reg U8Even(Reg r, uint8_t even);
    /// Byte `lane` of the U8 register `src` in every byte of dst, through i0.
    Reg U8LaneBroadcast(Reg src, int lane, Reg dst);
    /// Dot3: (a - 0.5) . (b - 0.5) * 4 over the colour bytes, in every byte of dst.
    void U8Dot3(Reg dst, Reg a, Reg b);
    /// The same with b the stage's constant colour, its side of the product in the F32
    /// uniform at `sa` (UniformKind::Dot3Const).
    void U8Dot3Const(Reg dst, Reg a, uint32_t sa);
    /// Which colour argument of a Dot3 stage is its constant colour unmodified (0 or 1), or
    /// -1 for neither.
    static int Dot3ConstArg(const TevStageConfig& stage, u32 index);
    void U8TevStage(u32 index);
    void U8Output();
    /// The nop a kill needs, then the phase marker that starts the next phase.
    void NewPhaseAfterKill();
    void Scissor();
    void Fog();
    /// The fog's factor, clamped, in every lane of i0 (Fog's first half).
    void FogFactor();
    void Byteround(Reg value, Reg scratch);
    void Clamp01(Reg value, uint8_t mask = 0xf);
    void LoadVec4(Reg internal, Vec4Loc from);
    void StoreVec4(Vec4Loc to, Reg internal, uint8_t mask = 0xf);
    void CopyVec4(Vec4Loc to, Vec4Loc from, uint8_t mask = 0xf);
    /// Normalises the float3 at `from` into internal register `into` (xyz), through `scratch`.
    void Normalize3(Reg into, Vec4Loc from, Reg scratch);
    /// A scalar secondary attribute as a broadcast source.
    static Src SaScalar(uint32_t sa) {
        return {Reg::Sa(static_cast<uint8_t>(sa & ~1u)), (sa & 1) ? YYYY : XXXX};
    }
    /// A scalar anywhere in the unified store as a broadcast source.
    static Src Scalar(Bank bank, uint32_t reg) {
        return {Reg{bank, static_cast<uint8_t>(reg & ~1u)}, (reg & 1) ? YYYY : XXXX};
    }
    /// The data block starts after the resident uniforms (its first SA the uniform buffer pointer
    /// with data_head), the samplers' words, then the literals the code reads: a literal's pair
    /// gets the next free pair the first time either is read, so a draw's SA image carries a few
    /// literals, not all 44 (each tile a draw touches sets its state up again).
    Src Lit(uint8_t index) const {
        return SaScalar(LitSa(index));
    }
    uint32_t LitSa(uint8_t index) const {
        int8_t& slot = lit_pair_slot[index / 2];
        if (slot < 0) {
            slot = static_cast<int8_t>(lit_pairs++);
        }
        return LiteralSaOffset(uniform_floats + data_head,
                               static_cast<uint32_t>(program.samplers.size()),
                               static_cast<uint32_t>(slot) * 2 + index % 2);
    }
    uint8_t SamplerSa(uint32_t sampler_index) const {
        return static_cast<uint8_t>(SamplerSaOffset(uniform_floats + data_head, sampler_index));
    }
    /// Per pair of the literal table: its pair in the program's literals (Lit), or -1.
    mutable std::array<int8_t, LiteralCount / 2> lit_pair_slot = [] {
        std::array<int8_t, LiteralCount / 2> slots;
        slots.fill(-1);
        return slots;
    }();
    mutable uint32_t lit_pairs = 0;
    /// A uniform in the memory part of the default uniform buffer: `components` floats at
    /// a float offset past the resident part (the light parameters of lights past the SA
    /// budget); returns the offset.
    uint32_t AllocateMemoryUniform(UniformKind kind, uint8_t index, uint8_t components);
    /// A light's parameters: the SA quad set for a resident light, else TmpLightMem, which
    /// LightQuads fills quad by quad.
    Vec4Loc LightParams(uint32_t num);
    /// A memory light's parameter quads in `mask` loaded into TmpLightMem unless they are
    /// already; with `wait` waited for too, else a LUT fetch's WDF that follows covers them
    /// (no internal register may hold a value across either). Nothing for a resident light.
    void LightQuads(uint32_t mask, bool wait);
    void WriteDepth();
    /// A uniform's place: `components` floats (or array_size float4s) at a pair-aligned
    /// offset.
    uint32_t AllocateUniform(UniformKind kind, uint8_t components, uint32_t array_size,
                             uint8_t index = 0);
    /// A U8 uniform's place (Uniform::u8): a word an element, anywhere.
    uint32_t AllocateUniformU8(UniformKind kind, uint8_t components, uint32_t array_size);
    /// A SOP3's source 0, which is a temporary or a primary attribute: a secondary attribute
    /// copied into TmpU8Const first.
    Reg U8Src0(Reg r);
    /// The LUT sampler's index among the declared samplers.
    uint32_t LutSamplerIndex() const;
    bool LightingLutEnabled(const Pica::Shader::LutConfig& lut, Sampler sampler) const;

    FSConfig config;
    Ir::Builder e; ///< the program, in virtual quads; usse_ir places them
    bool u8_tev = false;             ///< the U8 combiners (U8TevSupported)
    int colour_u8_pa = -1;           ///< sgxd_flags u8colour: the colour's bytes' register
    /// The U8 combiners read a unit's texel as four F16, packed to U8 from there: not a unit
    /// that clamps to its border colour, whose split alpha is a dependent read, or that the
    /// lighting reads as its shadow or bump map (that code moves or reads F32 lanes).
    bool TexelF16(u32 unit) const {
        const auto& tex = config.texture;
        const auto& border = tex.texture_border_color[unit];
        const auto& lighting = config.lighting;
        return u8_tev && !border.enable_s && !border.enable_t &&
               (!(tex.alpha_split >> unit & 1) || unit_issued[unit]) &&
               !(uses_shadow && lighting.shadow_selector == unit) &&
               !(uses_lighting && lighting.bump_mode != LightingRegs::LightingBumpMode::None &&
                 lighting.bump_selector == unit);
    }
    /// A split unit whose alpha texture the PDS reads too (TextureIssue::alpha), into alpha_loc.
    bool AlphaIssued(u32 unit) const {
        return (config.texture.alpha_split >> unit & 1) && unit_issued[unit];
    }
    std::array<Reg, 6> u8_out{};     ///< each stage's output, U8
    Reg u8_prev = Reg::V(TmpU8Zero); ///< the combiner output so far, U8
    /// The lighting's sums packed to TmpU8Diffuse and TmpU8Specular where Lighting ends, not by
    /// U8Inputs.
    bool u8_lighting_packed = false;
    /// U8Broadcast's registers by BroadcastInput and lane; 0 for none.
    std::array<std::array<uint8_t, 3>, 7> u8_bcast{};
    Program program;
    std::array<bool, 3> unit_used{};
    /// The units whose texel is the pixel's own colour (FSConfig own_pixel): the tile's colour
    /// in o0, no sampler (unit_used is false for them).
    std::array<bool, 3> unit_own{};
    /// The units whose read the pixel PDS program issues (Program::issues): plain 2D at an
    /// untouched coordinate. Their texels are PAs, the others' the SMPs' temporaries.
    std::array<bool, 3> unit_issued{};
    std::array<Vec4Loc, 3> texel_loc{
        {{Bank::Virtual, TmpTex0}, {Bank::Virtual, TmpTex0 + 4}, {Bank::Virtual, TmpTex0 + 8}}};
    std::array<Vec4Loc, 3> alpha_loc{};   ///< an issued split alpha's texel (AlphaIssued)
    std::array<uint8_t, 3> texcoord_pa{}; ///< PA base of TEXCOORD0..2, when declared
    uint8_t position_pa = 0;              ///< PA base of WPOS (x, y, z, w), when declared
    uint8_t texcoord0_w_pa = 0;           ///< PA of TEXCOORD3's y (unit 0's projective w)
    /// The U8 fog's factor from the PDS's read of the fog table (TextureIssue::fog), in fog_pa
    /// as a packed U8888 word; else FogFactor's dependent read of the LUT.
    bool fog_issued = false;
    uint8_t fog_pa = 0;
    uint8_t scissor_pa = 0;               ///< scratch PA holding the scissor test's value
                                          ///< until the alpha test folds it into its kill
    bool fold_scissor = false;            ///< the scissor test kills with the alpha test
    uint8_t normquat_pa = 0;              ///< PA of TEXCOORD4 (the quaternion, half4)
    uint8_t view_pa = 0;                  ///< PA base of TEXCOORD5 (the view, float3)
    bool buffer_used = false;
    /// Where a stage's PreviousBuffer colour / alpha comes from: -2 zero (stage 0), -1 the
    /// tev_combiner_buffer_color uniform, else the stage whose output it is.
    std::array<int, 6> buffer_color_from{};
    std::array<int, 6> buffer_alpha_from{};
    /// Lanes of a stage's output a later stage reads through PreviousBuffer.
    std::array<uint8_t, 6> snapshot_lanes{};
    bool uses_kill = false;
    bool uses_scissor = false;
    bool uses_fog = false;
    bool uses_lighting = false;
    bool uses_primary_color = false;   ///< a stage reads PrimaryFragmentColor
    bool uses_secondary_color = false; ///< a stage reads SecondaryFragmentColor
    bool lut_used = false;
    // Where the uniforms landed (secondary attributes), assigned by PlanUniforms.
    uint32_t uniform_floats = 0;
    uint32_t sa_alpha_ref = 0;
    uint32_t sa_const_color = 0;
    std::array<uint32_t, 6> sa_dot3_const{}; ///< a stage's Dot3Const
    uint32_t sa_buffer_color = 0;
    uint32_t sa_scissor = 0;
    uint32_t sa_fog_offset = 0;
    uint32_t sa_fog_color = 0;
    uint32_t sa_lut_offset = 0;
    uint32_t sa_global_ambient = 0;
    uint32_t sa_border = 0;               ///< tex_border_color float4[3]
    uint32_t data_head = 0;               ///< 1 when the data block starts with the pointer
    uint32_t memory_floats = 0;           ///< floats in the memory part of the uniform buffer
    struct MemoryUniform {
        UniformKind kind;
        uint8_t index;
        uint32_t offset;
        uint8_t components;
    };
    std::vector<MemoryUniform> memory_uniforms;
    std::array<int32_t, 8> light_memory{}; ///< per light number: float offset, or -1 (resident)
    /// The light being lit when it lives in memory (LightParams): its float offset, its quads
    /// loaded into TmpLightMem so far (bit q: floats 4q..4q+3), and whether a load still
    /// waits for its WDF.
    bool light_mem = false;
    uint32_t light_mem_offset = 0;
    uint32_t light_mem_loaded = 0;
    bool light_mem_pending = false;
    bool uses_depth = false;              ///< W-buffering: the fragment writes its depth
    bool uses_shadow = false;             ///< lighting reads the shadow texture
    bool uses_border = false;             ///< a unit clamps to its border colour
    bool uses_proctex = false;
    bool proctex_rgb_read = false;   ///< a stage reads Texture3's red, green or blue
    bool proctex_alpha_read = false; ///< a stage reads Texture3's alpha
    /// The procedural texture as a table the pixel PDS reads (TextureIssue::proctex,
    /// ProcTexTabled), its texel in proctex_loc: no program of its own.
    bool proctex_tabled = false;
    Vec4Loc proctex_loc{};
    uint32_t sampler_alpha[3] = {};       ///< the split units' alpha samplers' indices
    uint32_t sampler_rg = 0;              ///< the rg and rgba LUT samplers' indices
    uint32_t sampler_rgba = 0;
    uint32_t sampler_noise = 0;           ///< the noise hash table's (RasterizerSgxd)
    uint32_t sa_proctex = 0;              ///< lut_offset, diff_lut_offset, color_map_offset,
                                          ///< alpha_map_offset, bias (scalars, in that order)
    uint32_t sa_noise_lut = 0;            ///< proctex_noise_lut_offset
    uint32_t sa_noise_f = 0;              ///< proctex_noise_f, _a, _p (pairs)
    uint32_t sa_noise_a = 0;
    uint32_t sa_noise_p = 0;
    std::array<uint32_t, 8> sa_light{}; ///< per PICA light number; per light: specular_0
                                        ///< +0, dist_atten_bias +3, specular_1 +4,
                                        ///< dist_atten_scale +7, diffuse +8, ambient +12,
                                        ///< position +16, spot_direction +20
};

const char* FragmentEmitter::Unsupported() const {
    const auto& fb = config.framebuffer;
    const auto& tex = config.texture;
    const auto& lighting = config.lighting;
    if (tex.fog_mode == TexturingRegs::FogMode::Gas) {
        return "gas";
    }
    if (fb.shadow_rendering) {
        return "shadow rendering";
    }
    if ((tex.alpha_split & 1) && tex.texture0_type == TextureType::TextureCube) {
        return "a split alpha texture on a cube";
    }
    if (fb.logic_op != FramebufferRegs::LogicOp::Copy &&
        fb.logic_op != FramebufferRegs::LogicOp::NoOp) {
        return "logic op";
    }
    auto rgb = fb.requested_rgb_blend;
    auto alpha = fb.requested_alpha_blend;
    if (rgb.RequiresMinMaxEmulation() || alpha.RequiresMinMaxEmulation()) {
        return "min/max blend";
    }
    // Shadow textures: the rasterizer binds a white texture for them (P5), so a program
    // sampling them would not be the fix.
    const auto t0 = tex.texture0_type.Value();
    if (t0 != TextureType::Disabled && t0 != TextureType::Texture2D &&
        t0 != TextureType::Projection2D && t0 != TextureType::TextureCube) {
        return "texture unit 0 type";
    }
    if (t0 == TextureType::TextureCube &&
        (tex.texture_border_color[0].enable_s || tex.texture_border_color[0].enable_t)) {
        return "cube texture border colour";
    }
    for (u32 i = 0; i < tex.tev_stages.size(); i++) {
        const TevStageConfig stage = tex.tev_stages[i];
        if (IsPassThroughTevStage(stage)) {
            continue;
        }
        const Source sources[] = {
            ColorSource(stage, stage.color_source1, i), ColorSource(stage, stage.color_source2, i),
            ColorSource(stage, stage.color_source3, i), AlphaSource(stage, stage.alpha_source1, i),
            AlphaSource(stage, stage.alpha_source2, i), AlphaSource(stage, stage.alpha_source3, i),
        };
        for (const Source s : sources) {
            switch (s) {
            case Source::PrimaryColor:
            case Source::Texture0:
            case Source::Texture1:
            case Source::Texture2:
            case Source::PreviousBuffer:
            case Source::Constant:
            case Source::Previous:
                break;
            case Source::PrimaryFragmentColor:
            case Source::SecondaryFragmentColor:
                break;
            case Source::Texture3: // the procedural texture, zero when it is off
                break;
            default:
                return "tev source";
            }
        }
    }
    return nullptr;
}

bool FragmentEmitter::LightingLutEnabled(const Pica::Shader::LutConfig& lut,
                                         Sampler sampler) const {
    return lut.enable &&
           LightingRegs::IsLightingSamplerSupported(config.lighting.config, sampler);
}

void FragmentEmitter::PlanInputs() {
    const auto& tex = config.texture;
    const auto& lighting = config.lighting;
    uses_lighting = lighting.enable;
    for (u32 i = 0; i < tex.tev_stages.size(); i++) {
        const TevStageConfig stage = tex.tev_stages[i];
        if (IsPassThroughTevStage(stage)) {
            continue;
        }
        const Source sources[] = {
            ColorSource(stage, stage.color_source1, i), ColorSource(stage, stage.color_source2, i),
            ColorSource(stage, stage.color_source3, i), AlphaSource(stage, stage.alpha_source1, i),
            AlphaSource(stage, stage.alpha_source2, i), AlphaSource(stage, stage.alpha_source3, i),
        };
        for (const Source s : sources) {
            if (s == Source::Texture0 && tex.texture0_type != TextureType::Disabled) {
                unit_used[0] = true;
            } else if (s == Source::Texture1) {
                unit_used[1] = true;
            } else if (s == Source::Texture2) {
                unit_used[2] = true;
            } else if (s == Source::PreviousBuffer) {
                buffer_used = true;
            } else if (s == Source::PrimaryFragmentColor) {
                uses_primary_color = true;
            } else if (s == Source::SecondaryFragmentColor) {
                uses_secondary_color = true;
            } else if (s == Source::Texture3 && config.proctex.enable) {
                uses_proctex = true;
            }
        }
        // Which of Texture3's channels the stage's operations read: argument 1 for Replace,
        // 1 and 2 for the two-argument operations, all three for the three-argument ones;
        // Dot3_RGBA's alpha comes from its colour arguments.
        const auto args = [](Operation op) -> u32 {
            return op == Operation::Replace ? 1
                   : (op == Operation::Lerp || op == Operation::MultiplyThenAdd ||
                      op == Operation::AddThenMultiply)
                       ? 3
                       : 2;
        };
        const Source colour_src[] = {stage.color_source1, stage.color_source2,
                                     stage.color_source3};
        const ColorModifier colour_mod[] = {stage.color_modifier1, stage.color_modifier2,
                                            stage.color_modifier3};
        const Source alpha_src[] = {stage.alpha_source1, stage.alpha_source2,
                                    stage.alpha_source3};
        const AlphaModifier alpha_mod[] = {stage.alpha_modifier1, stage.alpha_modifier2,
                                           stage.alpha_modifier3};
        for (u32 k = 0; k < args(stage.color_op); k++) {
            if (ColorSource(stage, colour_src[k], i) == Source::Texture3) {
                const bool alpha = (static_cast<u32>(colour_mod[k]) & ~1u) == 0x2;
                (alpha ? proctex_alpha_read : proctex_rgb_read) = true;
            }
        }
        if (stage.color_op != Operation::Dot3_RGBA) {
            for (u32 k = 0; k < args(stage.alpha_op); k++) {
                if (AlphaSource(stage, alpha_src[k], i) == Source::Texture3) {
                    const bool alpha = ((static_cast<u32>(alpha_mod[k]) >> 1) & 3) == 0;
                    (alpha ? proctex_alpha_read : proctex_rgb_read) = true;
                }
            }
        }
    }
    // Lighting reaches the picture only through the fragment colours: a program whose stages read
    // neither has nothing to light (lighting is on after reset, so a title that never touches it
    // has it on).
    uses_lighting = uses_lighting && (uses_primary_color || uses_secondary_color);
    if (uses_lighting && lighting.bump_mode != LightingRegs::LightingBumpMode::None) {
        const u32 unit = lighting.bump_selector;
        if (unit < 3 && (unit != 0 || tex.texture0_type != TextureType::Disabled)) {
            unit_used[unit] = true;
        }
    }
    uses_scissor = config.framebuffer.scissor_test_mode.Value() !=
                   Pica::RasterizerRegs::ScissorMode::Disabled;
    uses_fog = tex.fog_mode == TexturingRegs::FogMode::Fog;
    uses_depth = config.framebuffer.depthmap_enable ==
                 Pica::RasterizerRegs::DepthBuffering::WBuffering;
    if (uses_lighting && lighting.enable_shadow) {
        const u32 unit = lighting.shadow_selector;
        if (unit < 3 && (unit != 0 || tex.texture0_type != TextureType::Disabled)) {
            unit_used[unit] = true;
            uses_shadow = true;
        }
    }
    // A unit whose texel is the pixel's own colour reads the tile, not a sampler. The
    // rasterizer (OwnPixelUnits) sets it only on a plain 2D unit the lighting does not read.
    for (u32 unit = 0; unit < 3; unit++) {
        if (!(tex.own_pixel >> unit & 1) || !unit_used[unit]) {
            continue;
        }
        const bool lit =
            (uses_shadow && lighting.shadow_selector == unit) ||
            (uses_lighting && lighting.bump_mode != LightingRegs::LightingBumpMode::None &&
             lighting.bump_selector == unit);
        if (lit || (unit == 0 && tex.texture0_type != TextureType::Texture2D)) {
            throw std::runtime_error("usse: an own-pixel unit the lighting reads or not 2D");
        }
        unit_own[unit] = true;
        unit_used[unit] = false;
    }
    shadow_scalar = uses_shadow && shadow_texture && lighting.shadow_selector == 0;
    for (u32 unit = 0; unit < 3; unit++) {
        const auto& border = tex.texture_border_color[unit];
        if (unit_used[unit] && (border.enable_s || border.enable_t)) {
            uses_border = true;
        }
    }
    if (uses_lighting) {
        lut_used = LightingLutEnabled(lighting.lut_d0, Sampler::Distribution0) ||
                   LightingLutEnabled(lighting.lut_d1, Sampler::Distribution1) ||
                   LightingLutEnabled(lighting.lut_fr, Sampler::Fresnel) ||
                   LightingLutEnabled(lighting.lut_rr, Sampler::ReflectRed) ||
                   LightingLutEnabled(lighting.lut_rg, Sampler::ReflectGreen) ||
                   LightingLutEnabled(lighting.lut_rb, Sampler::ReflectBlue);
        for (u32 i = 0; i < lighting.src_num; i++) {
            const auto& light = lighting.lights[i];
            if (light.dist_atten_enable) {
                lut_used = true;
            }
            if (light.spot_atten_enable &&
                LightingLutEnabled(lighting.lut_sp, Sampler::SpotlightAttenuation)) {
                lut_used = true;
            }
        }
    }
    fog_issued = uses_fog && u8_tev;
    lut_used = uses_fog && !fog_issued;

    // pa0-3 is the vertex colour. The quaternions and the view arrive as psp2cgc's `half4`
    // varyings (two lanes a register; the GPU User's Guide's fragment input advice), read once
    // into an internal register with pack.f32.f16 (LoadVec4). The colour, the position and the
    // texture coordinates stay F32: an F16 colour moved 1% of a game's grid frame by up to 9/255
    // after the byte rounding, and depth and a repeating texture's
    // coordinates need the bits. The four-wide inputs follow, then the pairs, then the one
    // float3 (which need not be followed by anything pair-aligned).
    program.inputs.push_back({FragmentInput::Color0, 4});
    uint8_t pa = 4;
    if (uses_scissor || (uses_fog && !fog_issued) || uses_depth) {
        program.inputs.push_back({FragmentInput::Position, 4});
        position_pa = pa;
        pa += 4;
    }
    // The quaternion alone: the flat copy's sign does not change the rotation.
    if (uses_lighting) {
        program.inputs.push_back({FragmentInput::TexCoord4, 4, true});
        normquat_pa = pa;
        pa += 2;
    }
    // A plain 2D read at an untouched coordinate is the PDS's to issue before the program
    // starts (a non-dependent read): no SMP, no wait, no thread waiting on the texture unit
    // (Sony's USSE Performance tutorial, "Dependent vs. Non-dependent Texture Reads"). The
    // vertex program writes TEXCOORD0..2 as (s, 1 - t), the rows the host copies keep, so the
    // coordinate needs no arithmetic here, and the program iterates it only for the border test
    // or a split unit's alpha read.
    const u32 unit_coord[3] = {0, 1, tex.texture2_use_coord1 ? 1u : 2u};
    for (u32 unit = 0; unit < 3; unit++) {
        unit_issued[unit] =
            unit_used[unit] && (unit != 0 || tex.texture0_type == TextureType::Texture2D);
    }
    u32 unit_issues = fog_issued ? 1 : 0;
    for (u32 unit = 0; unit < 3; unit++) {
        unit_issues += unit_issued[unit] ? (AlphaIssued(unit) ? 2 : 1) : 0;
    }
    proctex_tabled =
        uses_proctex && ProcTexTabled(config.proctex) && unit_issues < MaxTextureIssues;
    const u32 proc_coord =
        uses_proctex && !proctex_tabled ? std::min<u32>(config.proctex.coord, 2) : 3;
    bool coord_needed[3] = {proc_coord == 0, proc_coord == 1, proc_coord == 2};
    for (u32 unit = 0; unit < 3; unit++) {
        const auto& border = tex.texture_border_color[unit];
        if (unit_used[unit] && (!unit_issued[unit] || border.enable_s || border.enable_t)) {
            coord_needed[unit_coord[unit]] = true;
        }
    }
    const FragmentInput coord_inputs[3] = {FragmentInput::TexCoord0, FragmentInput::TexCoord1,
                                           FragmentInput::TexCoord2};
    for (int c = 0; c < 3; c++) {
        if (coord_needed[c]) {
            program.inputs.push_back({coord_inputs[c], 2});
            texcoord_pa[c] = pa;
            pa += 2;
        }
    }
    // The texels follow the iterated inputs so far, in PDS order, pair-aligned.
    for (u32 unit = 0; unit < 3; unit++) {
        if (!unit_issued[unit]) {
            continue;
        }
        const bool f16 = TexelF16(unit);
        texel_loc[unit] = {Bank::PrimAttr, pa, f16};
        program.issues.push_back({static_cast<uint8_t>(unit),
                                  static_cast<uint8_t>(unit_coord[unit]), pa,
                                  static_cast<uint8_t>(program.inputs.size() - 1), f16});
        pa += f16 ? IssueTexelWordsF16 : IssueTexelWords;
        if (AlphaIssued(unit)) {
            // The split alpha's A8 texture at the same coordinate, its byte in .w, in the
            // colour's form; SampleTextures (F32) or U8Inputs (F16) puts it in the texel.
            alpha_loc[unit] = {Bank::PrimAttr, pa, f16};
            program.issues.push_back(
                {static_cast<uint8_t>(unit), static_cast<uint8_t>(unit_coord[unit]), pa,
                 static_cast<uint8_t>(program.inputs.size() - 1), f16, false, true});
            pa += f16 ? IssueTexelWordsF16 : IssueTexelWords;
        }
    }
    if (proctex_tabled) {
        proctex_loc = {Bank::PrimAttr, pa, u8_tev};
        TextureIssue issue{0, static_cast<uint8_t>(std::min<u32>(config.proctex.coord, 2)), pa,
                           IssueAfter(), u8_tev};
        issue.proctex = true;
        program.issues.push_back(issue);
        pa += u8_tev ? IssueTexelWordsF16 : IssueTexelWords;
    }
    if (uses_lighting) {
        // Declared four wide, as psp2cgc declares a half3 varying: the console allocates by
        // the declaration.
        program.inputs.push_back({FragmentInput::TexCoord5, 4, true});
        view_pa = pa;
        pa += 2;
    }
    if (unit_used[0] && (tex.texture0_type == TextureType::Projection2D ||
                         tex.texture0_type == TextureType::TextureCube)) {
        // texcoord0's w, TEXCOORD3's second lane (its first is the fog table's coordinate).
        program.inputs.push_back({FragmentInput::TexCoord3, 2});
        texcoord0_w_pa = static_cast<uint8_t>(pa + 1);
        pa += 2;
    }
    if (sched_probe.u8_colour && u8_tev && StageReads(Source::PrimaryColor)) {
        // sgxd_flags u8colour (a probe): the colour again, as the iterator's bytes.
        program.inputs.push_back({FragmentInput::Color0, 4, false, true});
        colour_u8_pa = pa;
        pa += 1;
    }
    if (fog_issued) {
        // The fog factor's word, after every input: one register, so nothing pair-aligned
        // follows it either.
        fog_pa = pa;
        program.issues.push_back(
            {0, 3, pa, static_cast<uint8_t>(program.inputs.size() - 1), false, true});
        pa += IssueTexelWordsRaw;
    }
    // One kill per program, as the compiler has it: with both tests on, the scissor
    // result waits in a scratch PA (the compiler's pa9) for the alpha test's kill.
    fold_scissor = uses_scissor &&
                   config.framebuffer.alpha_test_func != FramebufferRegs::CompareFunc::Always;
    if (fold_scissor) {
        pa += pa & 1;
        scissor_pa = pa;
        pa += 2;
    }
    program.pa_count = pa;
    // An issued unit's words go to the pixel PDS per draw, not to the SAs: no SMP reads them.
    for (u32 unit = 0; unit < 3; unit++) {
        if (unit_used[unit] && !unit_issued[unit]) {
            program.samplers.push_back({SamplerKind::Texture, static_cast<uint8_t>(unit), unit});
        }
    }
    if (lut_used) {
        program.samplers.push_back({SamplerKind::LutLf, 0, 3});
    }
    if (uses_proctex && !proctex_tabled) {
        sampler_rg = static_cast<uint32_t>(program.samplers.size());
        program.samplers.push_back({SamplerKind::LutRg, 0, 4});
        sampler_rgba = static_cast<uint32_t>(program.samplers.size());
        program.samplers.push_back({SamplerKind::LutRgba, 0, 5});
        if (config.proctex.noise_enable) {
            sampler_noise = static_cast<uint32_t>(program.samplers.size());
            program.samplers.push_back({SamplerKind::ProctexNoise, 0, 6});
        }
    }
    // A split unit's alpha texture (TEXUNIT 7 + unit), after everything else so the other
    // samplers keep their indices.
    for (u32 unit = 0; unit < 3; unit++) {
        if (unit_used[unit] && (config.texture.alpha_split >> unit & 1) && !AlphaIssued(unit)) {
            sampler_alpha[unit] = static_cast<uint32_t>(program.samplers.size());
            program.samplers.push_back(
                {SamplerKind::TextureAlpha, static_cast<uint8_t>(unit), 7 + unit});
        }
    }
}

uint32_t FragmentEmitter::AllocateUniform(UniformKind kind, uint8_t components,
                                          uint32_t array_size, uint8_t index) {
    // Vectors and arrays start on a pair, and an array's elements are four floats apart;
    // scalars pack, so consecutive scalars such as the scissor bounds form pairs the program
    // reads as one operand.
    if ((components > 1 || array_size > 1) && (uniform_floats & 1)) {
        uniform_floats++;
    }
    const uint32_t at = uniform_floats;
    program.uniforms.push_back({kind, index, at, components, array_size});
    uniform_floats += array_size > 1 ? array_size * 4 : components;
    return at;
}

uint32_t FragmentEmitter::AllocateUniformU8(UniformKind kind, uint8_t components,
                                            uint32_t array_size) {
    const uint32_t at = uniform_floats;
    program.uniforms.push_back({kind, 0, at, components, array_size, true});
    uniform_floats += array_size;
    return at;
}

void FragmentEmitter::PlanUniforms() {
    const auto& tex = config.texture;
    const auto& lighting = config.lighting;
    if (config.framebuffer.alpha_test_func != FramebufferRegs::CompareFunc::Always &&
        config.framebuffer.alpha_test_func != FramebufferRegs::CompareFunc::Never) {
        sa_alpha_ref = AllocateUniform(UniformKind::AlphaTestRef, 1, 1);
    }
    // The constant colours as a prefix up to the last stage that reads one (the rasterizer
    // copies the uniform's array_size of them).
    u32 const_stages = 0;
    for (u32 i = 0; i < tex.tev_stages.size(); i++) {
        const TevStageConfig stage = tex.tev_stages[i];
        if (IsPassThroughTevStage(stage)) {
            continue;
        }
        const Source sources[] = {
            ColorSource(stage, stage.color_source1, i), ColorSource(stage, stage.color_source2, i),
            ColorSource(stage, stage.color_source3, i), AlphaSource(stage, stage.alpha_source1, i),
            AlphaSource(stage, stage.alpha_source2, i), AlphaSource(stage, stage.alpha_source3, i),
        };
        for (const Source s : sources) {
            if (s == Source::Constant) {
                const_stages = i + 1;
            }
        }
    }
    // The U8 combiners read the colours as packed words, in place (Uniform::u8): no pack in
    // the program.
    if (const_stages != 0) {
        sa_const_color = u8_tev ? AllocateUniformU8(UniformKind::ConstColor, 4, const_stages)
                                : AllocateUniform(UniformKind::ConstColor, 4, const_stages);
    }
    if (u8_tev) {
        for (u32 i = 0; i < tex.tev_stages.size(); i++) {
            const TevStageConfig stage = tex.tev_stages[i];
            if (!IsPassThroughTevStage(stage) && Dot3ConstArg(stage, i) >= 0) {
                sa_dot3_const[i] =
                    AllocateUniform(UniformKind::Dot3Const, 4, 1, static_cast<uint8_t>(i));
            }
        }
    }
    if (buffer_used) {
        sa_buffer_color = u8_tev ? AllocateUniformU8(UniformKind::BufferColor, 4, 1)
                                 : AllocateUniform(UniformKind::BufferColor, 4, 1);
    }
    if (uses_scissor) {
        // Four scalars the program reads as two pairs: they start on a pair.
        uniform_floats += uniform_floats & 1;
        sa_scissor = AllocateUniform(UniformKind::ScissorX1, 1, 1);
        AllocateUniform(UniformKind::ScissorY1, 1, 1);
        AllocateUniform(UniformKind::ScissorX2, 1, 1);
        AllocateUniform(UniformKind::ScissorY2, 1, 1);
    }
    if (uses_fog) {
        if (!fog_issued) {
            sa_fog_offset = AllocateUniform(UniformKind::FogLutOffset, 1, 1);
        }
        sa_fog_color = u8_tev ? AllocateUniformU8(UniformKind::FogColor, 3, 1)
                              : AllocateUniform(UniformKind::FogColor, 3, 1);
    }
    if (uses_border) {
        sa_border = AllocateUniform(UniformKind::TexBorderColor, 4, 3);
    }
    if (uses_proctex && !proctex_tabled) {
        sa_proctex = AllocateUniform(UniformKind::ProctexLutOffset, 1, 1);
        AllocateUniform(UniformKind::ProctexDiffLutOffset, 1, 1);
        AllocateUniform(UniformKind::ProctexColorMapOffset, 1, 1);
        AllocateUniform(UniformKind::ProctexAlphaMapOffset, 1, 1);
        AllocateUniform(UniformKind::ProctexBias, 1, 1);
        if (config.proctex.noise_enable) {
            sa_noise_lut = AllocateUniform(UniformKind::ProctexNoiseLutOffset, 1, 1);
            sa_noise_f = AllocateUniform(UniformKind::ProctexNoiseF, 2, 1);
            sa_noise_a = AllocateUniform(UniformKind::ProctexNoiseA, 2, 1);
            sa_noise_p = AllocateUniform(UniformKind::ProctexNoiseP, 2, 1);
        }
    }
    light_memory.fill(-1);
    if (uses_lighting) {
        // The LUT offsets as a prefix of the float4[6] up to the highest sampler used.
        u32 highest = 0;
        const auto note = [&](bool used, u32 sampler) {
            if (used) {
                highest = std::max(highest, sampler + 1);
            }
        };
        note(LightingLutEnabled(lighting.lut_d0, Sampler::Distribution0), 0);
        note(LightingLutEnabled(lighting.lut_d1, Sampler::Distribution1), 1);
        note(LightingLutEnabled(lighting.lut_fr, Sampler::Fresnel), 3);
        note(LightingLutEnabled(lighting.lut_rb, Sampler::ReflectBlue), 4);
        note(LightingLutEnabled(lighting.lut_rg, Sampler::ReflectGreen), 5);
        note(LightingLutEnabled(lighting.lut_rr, Sampler::ReflectRed), 6);
        for (u32 i = 0; i < lighting.src_num; i++) {
            const auto& light = lighting.lights[i];
            note(light.spot_atten_enable &&
                     LightingLutEnabled(lighting.lut_sp, Sampler::SpotlightAttenuation),
                 8 + light.num);
            note(light.dist_atten_enable, 16 + light.num);
        }
        if (highest != 0) {
            sa_lut_offset = AllocateUniform(UniformKind::LightingLutRow, 4, (highest + 3) / 4);
        }
        sa_global_ambient = AllocateUniform(UniformKind::LightingGlobalAmbient, 3, 1);
        // Lights are register-resident (24 SAs each) while the budget holds, counting the
        // literals, the sampler words and the pointer behind them; the rest live in the
        // memory part of the uniform buffer and are loaded when their turn comes.
        const uint32_t tail = LiteralCount + static_cast<uint32_t>(program.samplers.size()) * 4 + 1;
        for (u32 i = 0; i < lighting.src_num; i++) {
            const u32 num = lighting.lights[i].num;
            // A directional light's vector is its position normalised, the same for the
            // whole draw: the rasterizer normalises it (LightDirection), unless the distance
            // attenuation wants the position's length.
            const UniformKind position = LightPreNormalised(lighting.lights[i])
                                             ? UniformKind::LightDirection
                                             : UniformKind::LightPosition;
            const UniformKind members[8] = {UniformKind::LightSpecular0,
                                            UniformKind::LightDistAttenBias,
                                            UniformKind::LightSpecular1,
                                            UniformKind::LightDistAttenScale,
                                            UniformKind::LightDiffuse,
                                            UniformKind::LightAmbient,
                                            position,
                                            UniformKind::LightSpotDirection};
            const uint8_t sizes[8] = {3, 1, 3, 1, 3, 3, 3, 3};
            const bool resident_light = ((uniform_floats + 1) & ~1u) + 24 + tail <= 128;
            for (int m = 0; m < 8; m++) {
                const uint8_t light = static_cast<uint8_t>(num);
                if (resident_light) {
                    const uint32_t at = AllocateUniform(members[m], sizes[m], 1, light);
                    if (m == 0) {
                        sa_light[num] = at;
                    }
                } else {
                    const uint32_t at = AllocateMemoryUniform(members[m], light, sizes[m]);
                    if (m == 0) {
                        light_memory[num] = static_cast<int32_t>(at);
                    }
                }
            }
        }
    }
    if (!memory_uniforms.empty()) {
        // The pointer takes the data block's first SA; an odd resident count keeps the
        // literals behind it on their pairs.
        uniform_floats |= 1;
        data_head = 1;
        program.buffer_pointer = true;
        program.ldst_base_value = static_cast<int32_t>(uniform_floats * 4) - 4;
        program.buffer_floats = uniform_floats + memory_floats;
        for (const MemoryUniform& m : memory_uniforms) {
            program.uniforms.push_back(
                {m.kind, m.index, uniform_floats + m.offset, m.components, 1});
        }
    } else if (uniform_floats & 1) {
        uniform_floats++;
    }
    program.uniform_floats = uniform_floats;
}

uint32_t FragmentEmitter::AllocateMemoryUniform(UniformKind kind, uint8_t index,
                                                uint8_t components) {
    // The SA layout, so the code addressing a light does not care where it lives: every
    // member starts on a pair, a float3 takes four floats with its scalar partner.
    if (components > 1 && (memory_floats & 1)) {
        memory_floats++;
    }
    const uint32_t at = memory_floats;
    memory_uniforms.push_back({kind, index, at, components});
    memory_floats += components;
    return at;
}

Vec4Loc FragmentEmitter::LightParams(uint32_t num) {
    light_mem = light_memory[num] >= 0;
    if (!light_mem) {
        return {Bank::SecAttr, static_cast<uint8_t>(sa_light[num])};
    }
    // 24 floats from the buffer, a load per quad. The allocator places each quad on its
    // own: one sixteen-float load filled sixteen registers from the first quad's home (past
    // the program's temps), while the three quads after it read registers nothing loaded
    // (a game's speckled drivers). The immediate offset is in floats and seven
    // bits wide; the fourth light past the budget still fits.
    const uint32_t off = static_cast<uint32_t>(light_memory[num]);
    if (off + 20 > 127) {
        throw std::runtime_error("light parameters past the load offset's reach");
    }
    light_mem_offset = off;
    light_mem_loaded = 0;
    light_mem_pending = false;
    return {Bank::Virtual, TmpLightMem};
}

void FragmentEmitter::LightQuads(uint32_t mask, bool wait) {
    // Each quad is loaded just before the code that reads it, not all six at the light's
    // start: the six quads (24 temporaries) were live across every LUT fetch of the light, and
    // the programs with a light in memory were the ones at 60-64 temporaries.
    if (!light_mem) {
        return;
    }
    const Reg ptr = Reg::Sa(static_cast<uint8_t>(uniform_floats)); // the data block's head
    for (uint32_t q = 0; q < 6; q++) {
        if ((mask >> q & 1) != 0 && (light_mem_loaded >> q & 1) == 0) {
            e.Lda32(Reg::V(static_cast<uint8_t>(TmpLightMem + 4 * q)), ptr, nullptr,
                    static_cast<uint8_t>(light_mem_offset + 4 * q), 4, 0);
            light_mem_loaded |= 1u << q;
            light_mem_pending = true;
        }
    }
    if (wait && light_mem_pending) {
        e.WdfVertex(0);
        light_mem_pending = false;
    }
}

uint32_t FragmentEmitter::LutSamplerIndex() const {
    uint32_t index = 0;
    for (u32 u = 0; u < 3; u++) {
        index += unit_used[u] && !unit_issued[u] ? 1 : 0;
    }
    return index;
}

void FragmentEmitter::NewPhaseAfterKill() {
    // A kill ends its phase; a program has one kill and two phases, never more: three phases, or a
    // second kill inside phase two, lock the GPU up. So the scissor and alpha tests share one kill
    // (fold_scissor).
    e.NopAfterKill();
    e.PhaseStart();
    e.Phas(true);
    e.Nop();
}

void FragmentEmitter::Scissor() {
    if (!uses_scissor) {
        return;
    }
    // inside = x >= x1 && y >= y1 && x < x2 && y < y2. Pixel centres sit at .5 and the
    // bounds are whole numbers, so x < x2 is x2 - x - 0.5 >= 0, and the four conditions
    // become one minimum against zero.
    const Reg pos = Reg::Pa(position_pa);
    const Reg lo = Reg::Sa(static_cast<uint8_t>(sa_scissor));
    const Reg hi = Reg::Sa(static_cast<uint8_t>(sa_scissor + 2));
    e.Vec(VecOp::Add, {I0, 0x3}, {lo, XYZW, Mod::Neg}, {pos, XYZW}, Pred::None, true);
    e.Vec(VecOp::Add, {I1, 0x3}, {pos, XYZW, Mod::Neg}, {hi, XYZW}, Pred::None, true);
    e.Vec(VecOp::Add, {I1, 0x3}, {Reg::C(12), XXXX, Mod::Neg}, {I1, XYZW}, Pred::None, true);
    e.Vec(VecOp::Min, {I0, 0x3}, {I0, XYZW}, {I1, XYZW}, Pred::None, true);
    e.Vec(VecOp::Min, {I0, 0x1}, {I0, XYZW}, {I0, YYYY}, Pred::None, true);
    // Exclude discards the inside (keep when the minimum is negative); Include the outside.
    const bool include = config.framebuffer.scissor_test_mode.Value() ==
                         Pica::RasterizerRegs::ScissorMode::Include;
    if (fold_scissor) {
        e.MoveInternal({Reg::Pa(scissor_pa), 0x1}, {I0, XYZW}, Pred::None, true);
        return;
    }
    e.Test(include ? Cond::Ge : Cond::Lt, 1, {I0}, 0, ConstZero, false);
    e.KillUnlessP1();
    NewPhaseAfterKill();
}

void FragmentEmitter::LutFetch(uint32_t sampler_index, uint8_t scale_lit, uint8_t bias_lit,
                               bool negated) {
    // i1.y holds a whole-number index into a LUT texture (256 texels per row), negated when
    // the caller kept it as -floor (frc's form). (column, row) = (frac, floor) of index /
    // 256, then one scale and one bias to the texel centre.
    LutCoordInto(Reg::V(TmpLutCoord), scale_lit, bias_lit, negated);
    e.Sample2DPoint(Reg::V(TmpLutEntry), Reg::V(TmpLutCoord), SamplerSa(sampler_index), 0);
    e.Wdf(0);
    light_mem_pending = false; // the wait is for every read on the counter
}

void FragmentEmitter::LutCoordInto(Reg coord, uint8_t scale_lit, uint8_t bias_lit, bool negated) {
    e.Vec(VecOp::Mul, {I0, 0x2}, {I1, YYYY, negated ? Mod::Neg : Mod::None}, Lit(LitInv256),
          Pred::None, true);
    e.Vec(VecOp::Frc, {I0, 0x1}, {I0, YYYY}, {I0, YYYY}, Pred::None, true);     // column / 256
    e.Vec(VecOp::Add, {I0, 0x2}, {I0, XXXX, Mod::Neg}, {I0, YYYY}, Pred::None, true); // row
    e.Vec(VecOp::Mul, {I0, 0x3}, {I0, XYZW}, {Lit(scale_lit).reg, XYZW}, Pred::None, true);
    e.Vec(VecOp::Add, {coord, 0x3}, {I0, XYZW}, {Lit(bias_lit).reg, XYZW}, Pred::None, true);
}

void FragmentEmitter::LutLfFetch() {
    // The lf LUT entry at i1.y interpolated with TmpLutDelta: entry.r + entry.g * delta
    // in i0.x.
    LutFetch(LutSamplerIndex(), LitLutScaleX, LitLutBiasX, true);
    e.Vec(VecOp::Mul, {I0, 0x1}, {Reg::V(TmpLutEntry), YYYY}, {Reg::V(TmpLutDelta), XXXX},
          Pred::None, true);
    e.Vec(VecOp::Add, {I0, 0x1}, {I0, XYZW}, {Reg::V(TmpLutEntry), XXXX}, Pred::None, true);
}

void FragmentEmitter::FloorLane(uint8_t lane) {
    const Swizzle swz = lane == 0 ? XXXX : lane == 1 ? YYYY : lane == 2 ? ZZZZ : WWWW;
    const uint8_t mask = static_cast<uint8_t>(1u << lane);
    e.Vec(VecOp::Frc, {I1, mask}, {I0, swz}, {I0, swz}, Pred::None, true);
    e.Vec(VecOp::Add, {I0, mask}, {I1, swz, Mod::Neg}, {I0, swz}, Pred::None, true);
}

void FragmentEmitter::ProcTexLookup(Src offset) {
    ProcTexLookupCoord(offset, 0);
    ProcTexLookupSample(0);
    ProcWdf();
    ProcTexLookupFinish(0);
}

void FragmentEmitter::ProcTexLookupCoord(Src offset, uint8_t slot) {
    // coord * 128: the whole part (clamped to the table) selects the entry, the fraction
    // interpolates with its slope, as the generator's ProcTexLookupLUT.
    const uint8_t home = static_cast<uint8_t>(TmpBatch + 4 * slot);
    e.Vec(VecOp::Mul, {I0, 0x2}, {I0, XXXX}, Lit(Lit128), Pred::None, true);
    e.Vec(VecOp::Frc, {I1, 0x2}, {I0, YYYY}, {I0, YYYY}, Pred::None, true);
    e.Vec(VecOp::Add, {I1, 0x2}, {I1, YYYY, Mod::Neg}, {I0, YYYY}, Pred::None, true); // floor
    e.Vec(VecOp::Max, {I1, 0x2}, {I1, XYZW}, ConstZero, Pred::None, true);
    e.Vec(VecOp::Min, {I1, 0x2}, {I1, XYZW}, Lit(Lit127), Pred::None, true);
    e.Vec(VecOp::Add, {I1, 0x1}, {I1, YYYY, Mod::Neg}, {I0, YYYY}, Pred::None, true); // fraction
    e.MoveInternal({Reg::V(home + 2), 0x1}, {I1, XYZW}, Pred::None, true);
    e.Vec(VecOp::Add, {I1, 0x2}, {I1, YYYY}, offset, Pred::None, true);
    LutCoordInto(Reg::V(home), LitRgScaleX, LitRgBiasX, false);
}

void FragmentEmitter::ProcTexLookupSample(uint8_t slot) {
    ProcSample(Reg::V(TmpBatchTexel + 4 * slot), Reg::V(TmpBatch + 4 * slot),
               SamplerSa(sampler_rg));
}

void FragmentEmitter::ProcSample(Reg dst, Reg coord, uint8_t sampler_sa) {
    // One level, no computed LOD: the LUTs point sampled, the noise table bilinear in both
    // filters, so no derivative decides what either returns.
    e.Sample2DPoint(dst, coord, sampler_sa, 0);
}

void FragmentEmitter::ProcWdf() {
    e.Wdf(0);
}

void FragmentEmitter::ProcTexLookupFinish(uint8_t slot) {
    const Reg texel = Reg::V(TmpBatchTexel + 4 * slot);
    e.Vec(VecOp::Mul, {I0, 0x1}, {texel, YYYY}, {Reg::V(TmpBatch + 4 * slot + 2), XXXX},
          Pred::None, true);
    e.Vec(VecOp::Add, {I0, 0x1}, {I0, XYZW}, {texel, XXXX}, Pred::None, true);
    Clamp01(I0, 0x1);
}

void FragmentEmitter::ProcTexCombine(TexturingRegs::ProcTexCombiner combiner) {
    using Combiner = TexturingRegs::ProcTexCombiner;
    const Src u{Reg::V(TmpProcUv), XXXX};
    const Src v{Reg::V(TmpProcUv), YYYY};
    // sqrt(u * u + v * v) into i0.y, zero when the sum is (rsq of zero is not finite).
    const auto length = [&] {
        e.Vec(VecOp::Mul, {I1, 0x1}, u, {Reg::V(TmpProcUv), XXXX}, Pred::None, true);
        e.Vec(VecOp::Mul, {I1, 0x2}, v, {Reg::V(TmpProcUv), YYYY}, Pred::None, true);
        e.Vec(VecOp::Add, {I1, 0x1}, {I1, XXXX}, {I1, YYYY}, Pred::None, true);
        e.Comp(CompOp::Rsq, {I1, 0x2}, {I1}, 0, Pred::None, true);
        e.Comp(CompOp::Rcp, {I0, 0x2}, {I1}, 1, Pred::None, true);
        e.Test(Cond::Gt, 2, {I1}, 0, ConstZero, true);
        e.MoveInternal({I0, 0x2}, ConstZero, Pred::None, true);
        e.Comp(CompOp::Rcp, {I0, 0x2}, {I1}, 1, Pred::P2, true);
    };
    switch (combiner) {
    case Combiner::U:
        e.MoveInternal({I0, 0x1}, u, Pred::None, true);
        break;
    case Combiner::U2:
        e.Vec(VecOp::Mul, {I0, 0x1}, u, {Reg::V(TmpProcUv), XXXX}, Pred::None, true);
        break;
    case Combiner::V:
        e.MoveInternal({I0, 0x1}, v, Pred::None, true);
        break;
    case Combiner::V2:
        e.Vec(VecOp::Mul, {I0, 0x1}, v, {Reg::V(TmpProcUv), YYYY}, Pred::None, true);
        break;
    case Combiner::Add:
        e.Vec(VecOp::Add, {I0, 0x1}, u, {Reg::V(TmpProcUv), YYYY}, Pred::None, true);
        e.Vec(VecOp::Mul, {I0, 0x1}, {I0, XYZW}, ConstHalf, Pred::None, true);
        break;
    case Combiner::Add2:
        e.Vec(VecOp::Mul, {I1, 0x1}, u, {Reg::V(TmpProcUv), XXXX}, Pred::None, true);
        e.Vec(VecOp::Mul, {I1, 0x2}, v, {Reg::V(TmpProcUv), YYYY}, Pred::None, true);
        e.Vec(VecOp::Add, {I0, 0x1}, {I1, XXXX}, {I1, YYYY}, Pred::None, true);
        e.Vec(VecOp::Mul, {I0, 0x1}, {I0, XYZW}, ConstHalf, Pred::None, true);
        break;
    case Combiner::SqrtAdd2:
        length();
        e.Vec(VecOp::Min, {I0, 0x1}, {I0, YYYY}, ConstOne, Pred::None, true);
        break;
    case Combiner::Min:
        e.Vec(VecOp::Min, {I0, 0x1}, u, {Reg::V(TmpProcUv), YYYY}, Pred::None, true);
        break;
    case Combiner::Max:
        e.Vec(VecOp::Max, {I0, 0x1}, u, {Reg::V(TmpProcUv), YYYY}, Pred::None, true);
        break;
    case Combiner::RMax:
        length();
        e.Vec(VecOp::Add, {I0, 0x1}, u, {Reg::V(TmpProcUv), YYYY}, Pred::None, true);
        e.Vec(VecOp::Mul, {I0, 0x1}, {I0, XYZW}, ConstHalf, Pred::None, true);
        e.Vec(VecOp::Add, {I0, 0x1}, {I0, XXXX}, {I0, YYYY}, Pred::None, true);
        e.Vec(VecOp::Mul, {I0, 0x1}, {I0, XYZW}, ConstHalf, Pred::None, true);
        e.Vec(VecOp::Min, {I0, 0x1}, {I0, XYZW}, ConstOne, Pred::None, true);
        break;
    default:
        e.MoveInternal({I0, 0x1}, ConstZero, Pred::None, true);
        break;
    }
}

void FragmentEmitter::ProcTexShift(TexturingRegs::ProcTexShift mode,
                                   TexturingRegs::ProcTexClamp clamp, Src coord) {
    using Shift = TexturingRegs::ProcTexShift;
    if (mode != Shift::Odd && mode != Shift::Even) {
        e.MoveInternal({I2, 0x1}, ConstZero, Pred::None, true);
        return;
    }
    // offset * ((int(c) [+ 1] >> 1) & 1), the integer arithmetic in floats: the coordinate
    // is never negative here, so int(c) is floor(c); x >> 1 is floor(x / 2); x & 1 is
    // 2 * frac(x / 2).
    e.Vec(VecOp::Frc, {I1, 0x1}, coord, coord, Pred::None, true);
    e.Vec(VecOp::Add, {I1, 0x1}, {I1, XXXX, Mod::Neg}, coord, Pred::None, true); // floor
    if (mode == Shift::Even) {
        e.Vec(VecOp::Add, {I1, 0x1}, {I1, XYZW}, ConstOne, Pred::None, true);
    }
    e.Vec(VecOp::Mul, {I1, 0x2}, {I1, XXXX}, ConstHalf, Pred::None, true);
    e.Vec(VecOp::Frc, {I2, 0x2}, {I1, YYYY}, {I1, YYYY}, Pred::None, true);
    e.Vec(VecOp::Add, {I1, 0x2}, {I2, YYYY, Mod::Neg}, {I1, YYYY}, Pred::None, true); // >> 1
    e.Vec(VecOp::Mul, {I1, 0x2}, {I1, YYYY}, ConstHalf, Pred::None, true);
    e.Vec(VecOp::Frc, {I1, 0x2}, {I1, YYYY}, {I1, YYYY}, Pred::None, true);
    e.Vec(VecOp::Mul, {I2, 0x1}, {I1, YYYY}, Lit(LitTwo), Pred::None, true);   // & 1
    if (clamp != TexturingRegs::ProcTexClamp::MirroredRepeat) {
        e.Vec(VecOp::Mul, {I2, 0x1}, {I2, XYZW}, ConstHalf, Pred::None, true); // offset 0.5
    }
}

void FragmentEmitter::ProcTexClamp(TexturingRegs::ProcTexClamp mode, uint8_t mask) {
    using Clamp = TexturingRegs::ProcTexClamp;
    switch (mode) {
    case Clamp::ToZero:
        for (uint8_t lane = 0; lane < 2; lane++) {
            if (mask & (1u << lane)) {
                e.Test(Cond::Gt, 2, {I0}, lane, ConstOne, true);
                e.MoveInternal({I0, static_cast<uint8_t>(1u << lane)}, ConstZero, Pred::P2, true);
            }
        }
        break;
    case Clamp::SymmetricalRepeat:
        e.Vec(VecOp::Frc, {I0, mask}, {I0, XYZW}, {I0, XYZW}, Pred::None, true);
        break;
    case Clamp::MirroredRepeat:
        // frac(c) for an even floor(c), 1 - frac(c) for an odd one: the triangle wave
        // 1 - 2 |frac(c / 2) - 1/2| (c is never negative here).
        e.Vec(VecOp::Mul, {I1, mask}, {I0, XYZW}, ConstHalf, Pred::None, true);
        e.Vec(VecOp::Frc, {I1, mask}, {I1, XYZW}, {I1, XYZW}, Pred::None, true);
        e.Vec(VecOp::Add, {I1, mask}, {ConstHalf.reg, ConstHalf.swz, Mod::Neg}, {I1, XYZW},
              Pred::None, true);
        e.Vec(VecOp::Mul, {I1, mask}, {I1, XYZW, Mod::Abs}, Lit(LitTwo), Pred::None, true);
        e.Vec(VecOp::Add, {I0, mask}, {I1, XYZW, Mod::Neg}, ConstOne, Pred::None, true);
        break;
    case Clamp::Pulse:
        for (uint8_t lane = 0; lane < 2; lane++) {
            if (mask & (1u << lane)) {
                const uint8_t m = static_cast<uint8_t>(1u << lane);
                e.Test(Cond::Gt, 2, {I0}, lane, ConstHalf, true);
                e.MoveInternal({I0, m}, ConstZero, Pred::None, true);
                e.MoveInternal({I0, m}, ConstOne, Pred::P2, true);
            }
        }
        break;
    case Clamp::ToEdge:
    default:
        e.Vec(VecOp::Min, {I0, mask}, {I0, XYZW}, ConstOne, Pred::None, true);
        break;
    }
}

void FragmentEmitter::ProcTexNoise() {
    // NoiseCoef at uv: grid = 9 f |uv + p|, point = floor(grid), frac = grid - point; corner i's
    // hash h_i (period 144) times s - k_i, s = frac.x + frac.y, k = 0, 1, 1, 2, mixed
    // bilinearly at (xn, yn) = the noise LUT at frac.x and frac.y.
    // The texture unit mixes. RasterizerSgxd's F16x4 table holds h * (x odd), h * (y odd) and h
    // at (x, y) (NoiseTable), read bilinear at (point + (xn, yn) + 0.5) / 256. Mixing is
    // linear, so with B = mix(h * (x odd)), C = mix(h * (y odd)) and A = mix(h):
    //   coef = s A - Sx - Sy,  Sx = mix(h * dx) = B for an even point.x, A - B for an odd one
    // (the corners with dx = 1 are the odd columns, or the even ones; 144 is even), Sy alike
    // with C. The filter's weights are 6 bits, truncated (docs/sgxd/tsp.md): the mix is at xn and
    // yn truncated to 1/64.
    const Reg uv = Reg::V(TmpProcUv);
    const Reg point = Reg::V(TmpNoise);
    const Reg frac = Reg::V(TmpNoise + 2);
    const Reg fade = Reg::V(TmpNoise + 4); ///< xn, yn
    e.Vec(VecOp::Add, {I0, 0x3}, {uv, XYZW}, {Reg::Sa(static_cast<uint8_t>(sa_noise_p)), XYZW},
          Pred::None, true);
    e.Vec(VecOp::Mul, {I0, 0x3}, {I0, XYZW, Mod::Abs},
          {Reg::Sa(static_cast<uint8_t>(sa_noise_f)), XYZW}, Pred::None, true);
    e.Vec(VecOp::Mul, {I0, 0x3}, {I0, XYZW}, Lit(LitNine), Pred::None, true);
    e.Vec(VecOp::Frc, {I1, 0x3}, {I0, XYZW}, {I0, XYZW}, Pred::None, true);
    e.MoveInternal({frac, 0x3}, {I1, XYZW}, Pred::None, true);
    e.Vec(VecOp::Add, {I0, 0x3}, {I1, XYZW, Mod::Neg}, {I0, XYZW}, Pred::None, true); // floor
    e.Vec(VecOp::Mul, {I0, 0x3}, {I0, XYZW}, Lit(LitInv144), Pred::None, true);
    e.Vec(VecOp::Frc, {I0, 0x3}, {I0, XYZW}, {I0, XYZW}, Pred::None, true);
    e.Vec(VecOp::Mul, {I0, 0x3}, {I0, XYZW}, Lit(Lit144), Pred::None, true);
    e.MoveInternal({point, 0x3}, {I0, XYZW}, Pred::None, true);
    // The noise LUT at frac.x and frac.y: both reads, one WDF.
    e.MoveInternal({I0, 0x1}, {frac, XXXX}, Pred::None, true);
    ProcTexLookupCoord(SaScalar(sa_noise_lut), 0);
    e.MoveInternal({I0, 0x1}, {frac, YYYY}, Pred::None, true);
    ProcTexLookupCoord(SaScalar(sa_noise_lut), 1);
    ProcTexLookupSample(0);
    ProcTexLookupSample(1);
    ProcWdf();
    ProcTexLookupFinish(0);
    e.MoveInternal({fade, 0x1}, {I0, XXXX}, Pred::None, true);
    ProcTexLookupFinish(1);
    e.MoveInternal({fade, 0x2}, {I0, XXXX}, Pred::None, true);
    // The table at (point + fade + 0.5) / 256.
    const Reg coord = Reg::V(TmpBatch);
    const Reg texel = Reg::V(TmpBatchTexel); ///< B, C; then A at the next pair
    e.Vec(VecOp::Add, {I0, 0x3}, {point, XYZW}, {fade, XYZW}, Pred::None, true);
    e.Vec(VecOp::Mul, {I0, 0x3}, {I0, XYZW}, Lit(LitInv256), Pred::None, true);
    e.Vec(VecOp::Add, {coord, 0x3}, {I0, XYZW}, Lit(LitHalfInv256), Pred::None, true);
    ProcSample(texel, coord, SamplerSa(sampler_noise));
    ProcWdf();
    const Src a{Reg::V(TmpBatchTexel + 2), XXXX};
    // The parities p = 2 frac(point / 2), then S = B + p (A - 2 B), for x and y at once.
    e.Vec(VecOp::Mul, {I0, 0x3}, {point, XYZW}, ConstHalf, Pred::None, true);
    e.Vec(VecOp::Frc, {I0, 0x3}, {I0, XYZW}, {I0, XYZW}, Pred::None, true);
    e.Vec(VecOp::Mul, {I0, 0x3}, {I0, XYZW}, Lit(LitTwo), Pred::None, true);
    e.Vec(VecOp::Mul, {I1, 0x3}, {texel, XYZW}, Lit(LitTwo), Pred::None, true);
    e.Vec(VecOp::Add, {I1, 0x3}, {I1, XYZW, Mod::Neg}, a, Pred::None, true);
    e.Vec(VecOp::Mul, {I1, 0x3}, {I1, XYZW}, {I0, XYZW}, Pred::None, true);
    e.Vec(VecOp::Add, {I1, 0x3}, {I1, XYZW}, {texel, XYZW}, Pred::None, true);
    // coef = (frac.x + frac.y) A - (Sx + Sy)
    e.Vec(VecOp::Add, {I1, 0x1}, {I1, XXXX}, {I1, YYYY}, Pred::None, true);
    e.Vec(VecOp::Add, {I2, 0x1}, {frac, XXXX}, {frac, YYYY}, Pred::None, true);
    e.Vec(VecOp::Mul, {I2, 0x1}, {I2, XXXX}, a, Pred::None, true);
    e.Vec(VecOp::Add, {I0, 0x1}, {I1, XXXX, Mod::Neg}, {I2, XXXX}, Pred::None, true);
    // uv = |uv + a * coef|
    e.Vec(VecOp::Mul, {I1, 0x3}, {Reg::Sa(static_cast<uint8_t>(sa_noise_a)), XYZW}, {I0, XXXX},
          Pred::None, true);
    e.Vec(VecOp::Add, {I1, 0x3}, {uv, XYZW}, {I1, XYZW}, Pred::None, true);
    e.Vec(VecOp::Add, {uv, 0x3}, {I1, XYZW, Mod::Abs}, ConstZero, Pred::None, true);
}

void FragmentEmitter::ProcTex() {
    using Filter = TexturingRegs::ProcTexFilter;
    const auto& pt = config.proctex;
    const Vec4Loc uv_home{Bank::Virtual, TmpProcUv};
    const Reg uv = uv_home.Pair(0);
    const Reg tc = Reg::Pa(texcoord_pa[std::min<u32>(pt.coord, 2)]);
    // uv = |texcoord|, the coordinate as the PICA has it: the vertex program wrote (s, 1 - t).
    e.Vec(VecOp::Add, {uv, 0x1}, {tc, XYZW, Mod::Abs}, ConstZero, Pred::None, true);
    e.Vec(VecOp::Add, {uv, 0x2}, {tc, XYZW, Mod::Neg}, ConstOne, Pred::None, true);
    e.Vec(VecOp::Add, {uv, 0x2}, {uv, XYZW, Mod::Abs}, ConstZero, Pred::None, true);

    // The level, for the mipmapped filters: log2(|lut_width * bias| * (|duv.x| + |duv.y|))
    // clamped to the configuration's range, zero at a zero bias. The nearest level serves
    // both mipmap filter families.
    const auto filter = pt.lut_filter.Value();
    const bool mipmapped = filter != Filter::Linear && filter != Filter::Nearest;
    if (mipmapped) {
        // The derivatives land in the unified store, as the compiler's do (Sony's
        // disassembler rejects them aimed at an internal register).
        const Vec4Loc scratch{Bank::Virtual, TmpTex3};
        e.Vec(VecOp::Dsx, {scratch.Pair(0), 0x3}, {tc, XYZW, Mod::Abs}, {tc, XYZW, Mod::Abs});
        e.Vec(VecOp::Dsy, {scratch.Pair(1), 0x3}, {tc, XYZW, Mod::Abs}, {tc, XYZW, Mod::Abs});
        e.Vec(VecOp::Max, {I0, 0x3}, {scratch.Pair(0), XYZW, Mod::Abs},
              {scratch.Pair(1), XYZW, Mod::Abs}, Pred::None, true);
        e.Vec(VecOp::Add, {I0, 0x1}, {I0, XXXX}, {I0, YYYY}, Pred::None, true);
        e.Vec(VecOp::Mul, {I1, 0x1}, Lit(LitProcWidth), SaScalar(sa_proctex + 4), Pred::None, true);
        e.Vec(VecOp::Mul, {I0, 0x1}, {I0, XYZW}, {I1, XXXX, Mod::Abs}, Pred::None, true);
        e.Comp(CompOp::Log, {I0, 0x1}, {I0}, 0, Pred::None, true);
        e.Test(Cond::Eq, 2, {Reg::Sa(static_cast<uint8_t>((sa_proctex + 4) & ~1u))},
               static_cast<uint8_t>((sa_proctex + 4) & 1), ConstZero, true);
        e.MoveInternal({I0, 0x1}, ConstZero, Pred::P2, true);
        e.Vec(VecOp::Max, {I0, 0x1}, {I0, XYZW}, Lit(LitProcLodMin), Pred::None, true);
        e.Vec(VecOp::Min, {I0, 0x1}, {I0, XYZW}, Lit(LitProcLodMax), Pred::None, true);
        e.Vec(VecOp::Add, {I0, 0x1}, {I0, XYZW}, ConstHalf, Pred::None, true);
        FloorLane(0);
        e.MoveInternal({uv_home.Pair(1), 0x1}, {I0, XXXX}, Pred::None, true); // .z = level
    }

    // u = uv.x + u_shift(uv.y), v = uv.y + v_shift(uv.x), each clamped its way; the shifts from the
    // coordinates before the noise, parked in TmpTex3 (free until the colour lands there). A None
    // shift is left out. u goes home before v is computed: v reads uv.y and its parked shift only.
    const Reg parked = Reg::V(TmpTex3);
    const auto shifts = [](TexturingRegs::ProcTexShift mode) {
        return mode == TexturingRegs::ProcTexShift::Odd ||
               mode == TexturingRegs::ProcTexShift::Even;
    };
    const bool u_shifts = shifts(pt.u_shift);
    const bool v_shifts = shifts(pt.v_shift);
    if (u_shifts) {
        ProcTexShift(pt.u_shift, pt.u_clamp, {uv, YYYY});
        e.MoveInternal({parked, 0x1}, {I2, XXXX}, Pred::None, true);
    }
    if (v_shifts) {
        ProcTexShift(pt.v_shift, pt.v_clamp, {uv, XXXX});
        e.MoveInternal({parked, 0x2}, {I2, XXXX}, Pred::None, true);
    }
    const bool noise = pt.noise_enable;
    const bool colour = !pt.separate_alpha || proctex_rgb_read;
    if (noise) {
        ProcTexNoise();
    }
    // u and v into i0.xy with their shifts, clamped as a vec2 where both clamp alike.
    if (u_shifts && v_shifts) {
        e.Vec(VecOp::Add, {I0, 0x3}, {uv, XYZW}, {parked, XYZW}, Pred::None, true);
    } else {
        e.MoveInternal({I0, 0x3}, {uv, XYZW}, Pred::None, true);
        if (u_shifts || v_shifts) {
            e.Vec(VecOp::Add, {I0, static_cast<uint8_t>(u_shifts ? 0x1 : 0x2)}, {I0, XYZW},
                  {parked, XYZW}, Pred::None, true);
        }
    }
    if (pt.u_clamp == pt.v_clamp) {
        ProcTexClamp(pt.u_clamp, 0x3);
    } else {
        ProcTexClamp(pt.u_clamp, 0x1);
        ProcTexClamp(pt.v_clamp, 0x2);
    }
    e.MoveInternal({uv, 0x3}, {I0, XYZW}, Pred::None, true);

    // The colour LUT coordinate, then the colour; with a separate alpha that no stage's
    // colour reads, none: its alpha is replaced and its rgb never read.
    if (colour) {
        ProcTexColour(mipmapped);
    }
    if (pt.separate_alpha && proctex_alpha_read) {
        ProcTexCombine(pt.alpha_combiner);
        ProcTexLookup(SaScalar(sa_proctex + 3));
        e.MoveInternal({Reg::V(TmpTex3 + 2), 0x2}, {I0, XXXX}, Pred::None, true);
    }
}

void FragmentEmitter::ProcTexColour(bool mipmapped) {
    using Filter = TexturingRegs::ProcTexFilter;
    const auto& pt = config.proctex;
    const Vec4Loc uv_home{Bank::Virtual, TmpProcUv};
    const auto filter = pt.lut_filter.Value();
    ProcTexCombine(pt.color_combiner);
    ProcTexLookup(SaScalar(sa_proctex + 2));
    // lut_coord * (width - 1) [+ 0.5 for nearest], the level's offset; the level's width is
    // lut_width >> level (the table is halved per level).
    if (mipmapped) {
        // width_level - 1 = floor(lut_width * 2^-level) - 1, also for a width not a power of two;
        // offset from the per-level table.
        e.Vec(VecOp::Mul, {I1, 0x1}, {ConstOne.reg, XXXX, Mod::Neg}, {uv_home.Pair(1), XXXX},
              Pred::None, true);
        e.Comp(CompOp::Exp, {I1, 0x1}, {I1}, 0, Pred::None, true);
        e.Vec(VecOp::Mul, {I1, 0x1}, {I1, XYZW}, Lit(LitProcWidth), Pred::None, true);
        e.Vec(VecOp::Frc, {I1, 0x2}, {I1, XXXX}, {I1, XXXX}, Pred::None, true);
        e.Vec(VecOp::Add, {I1, 0x1}, {I1, YYYY, Mod::Neg}, {I1, XXXX}, Pred::None, true);
        e.Vec(VecOp::Add, {I1, 0x1}, {ConstOne.reg, XXXX, Mod::Neg}, {I1, XYZW}, Pred::None, true);
        e.Vec(VecOp::Mul, {I0, 0x1}, {I0, XXXX}, {I1, XXXX}, Pred::None, true);
        // offset = offsets[level]: walk the table.
        e.MoveInternal({I1, 0x2}, Lit(LitProcOff0), Pred::None, true);
        e.MoveInternal({I1, 0x1}, {uv_home.Pair(1), XXXX}, Pred::None, true); // the level
        const uint8_t table[7] = {LitProcOff1, LitProcOff2, LitProcOff3, LitProc240,
                                  LitProc248, LitProc252, LitProc254};
        for (int level = 1; level <= 7; level++) {
            e.Vec(VecOp::Add, {I1, 0x1}, {ConstOne.reg, XXXX, Mod::Neg}, {I1, XXXX}, Pred::None,
                  true);
            e.Test(Cond::Ge, 2, {I1}, 0, ConstZero, true);
            e.MoveInternal({I1, 0x2}, Lit(table[level - 1]), Pred::P2, true);
        }
    } else {
        e.Vec(VecOp::Mul, {I0, 0x1}, {I0, XXXX}, Lit(LitProcWidthM1), Pred::None, true);
        e.MoveInternal({I1, 0x2}, Lit(LitProcOff0), Pred::None, true);
    }
    const bool linear = filter == Filter::Linear || filter == Filter::LinearMipmapNearest ||
                        filter == Filter::LinearMipmapLinear;
    const Vec4Loc tex3{Bank::Virtual, TmpTex3};
    const Vec4Loc entry{Bank::Virtual, TmpLutEntry};
    if (linear) {
        // index_i = floor(coord) + offset; fraction f; colour = lut(index_i + lut_offset) +
        // f * lut(index_i + diff_offset).
        e.MoveInternal({I0, 0x4}, {I0, XXXX}, Pred::None, true);
        FloorLane(2);
        e.Vec(VecOp::Add, {I0, 0x1}, {I0, ZZZZ, Mod::Neg}, {I0, XXXX}, Pred::None, true); // f
        e.MoveInternal({Reg::V(TmpLutDelta), 0x1}, {I0, XXXX}, Pred::None, true);
        e.Vec(VecOp::Add, {I0, 0x4}, {I0, ZZZZ}, {I1, YYYY}, Pred::None, true);       // + offset
        e.MoveInternal({uv_home.Pair(1), 0x2}, {I0, ZZZZ}, Pred::None, true);       // .w = index_i
        // The entry and its slope: both reads, one WDF.
        e.Vec(VecOp::Add, {I1, 0x2}, {I0, ZZZZ}, SaScalar(sa_proctex + 0), Pred::None, true);
        LutCoordInto(Reg::V(TmpBatch), LitRgScaleX, LitRgBiasX, false);
        e.Vec(VecOp::Add, {I1, 0x2}, {uv_home.Pair(1), YYYY}, SaScalar(sa_proctex + 1),
              Pred::None, true);
        LutCoordInto(Reg::V(TmpBatch + 4), LitRgScaleX, LitRgBiasX, false);
        const uint8_t rgba = SamplerSa(sampler_rgba);
        ProcSample(Reg::V(TmpBatchTexel), Reg::V(TmpBatch), rgba);
        ProcSample(Reg::V(TmpBatchTexel + 4), Reg::V(TmpBatch + 4), rgba);
        ProcWdf();
        LoadVec4(I0, {Bank::Virtual, static_cast<uint8_t>(TmpBatchTexel + 4)});
        e.Vec(VecOp::Mul, {I0, 0xf}, {I0, XYZW}, {Reg::V(TmpLutDelta), XXXX}, Pred::None, true);
        LoadVec4(I1, {Bank::Virtual, TmpBatchTexel});
        e.Vec(VecOp::Add, {I0, 0xf}, {I0, XYZW}, {I1, XYZW}, Pred::None, true);
        StoreVec4(tex3, I0);
    } else {
        // lut(floor(coord + offset + 0.5) + lut_offset)
        e.Vec(VecOp::Add, {I0, 0x1}, {I0, XXXX}, {I1, YYYY}, Pred::None, true);
        e.Vec(VecOp::Add, {I0, 0x1}, {I0, XYZW}, ConstHalf, Pred::None, true);
        FloorLane(0);
        e.Vec(VecOp::Add, {I1, 0x2}, {I0, XXXX}, SaScalar(sa_proctex + 0), Pred::None, true);
        LutFetch(sampler_rgba, LitRgScaleX, LitRgBiasX, false);
        CopyVec4(tex3, entry);
    }
}

void FragmentEmitter::Fog() {
    if (!uses_fog) {
        return;
    }
    FogFactor();
    LoadVec4(I1, {Bank::Virtual, TmpOutput});
    LoadVec4(I2, {Bank::SecAttr, static_cast<uint8_t>(sa_fog_color)});
    e.Vec(VecOp::Add, {I1, 0x7}, {I2, XYZW, Mod::Neg}, {I1, XYZW}, Pred::None, true);
    e.Mad({I1, 0x7}, {I1, XYZW}, {I0, XYZW}, {I2, XYZW}, true, Pred::None, true);
    StoreVec4({Bank::Virtual, TmpOutput}, I1);
}

void FragmentEmitter::FogFactor() {
    // fog_index = depth * 128 (or (1 - depth) * 128 flipped); i = clamp(floor(index), 0,
    // 127); f = index - i; entry = lut_lf(i + fog_lut_offset); factor = clamp(r + g * f).
    const Reg pos = Reg::Pa(position_pa + 2); // z, w
    if (config.texture.fog_flip) {
        e.Vec(VecOp::Add, {I0, 0x1}, {pos, XYZW, Mod::Neg}, ConstOne, Pred::None, true);
        e.Vec(VecOp::Mul, {I0, 0x1}, {I0, XYZW}, Lit(Lit128), Pred::None, true);
    } else {
        e.Vec(VecOp::Mul, {I0, 0x1}, {pos, XYZW}, Lit(Lit128), Pred::None, true);
    }
    // i1.y = -i (frc's floor form, see LightingLutLookup), clamped to [-127, 0].
    e.Vec(VecOp::Frc, {I1, 0x2}, ConstZero, {I0, XXXX}, Pred::None, true);
    e.Vec(VecOp::Max, {I1, 0x2}, {I1, XYZW}, Lit(LitNeg127), Pred::None, true);
    e.Vec(VecOp::Min, {I1, 0x2}, {I1, XYZW}, ConstZero, Pred::None, true);
    e.Vec(VecOp::Add, {I1, 0x1}, {I1, YYYY}, {I0, XXXX}, Pred::None, true); // f = index - i
    e.MoveInternal({Reg::V(TmpLutDelta), 0x3}, {I1, XYZW}, Pred::None, true);
    const Src fog_offset = SaScalar(sa_fog_offset);
    e.Vec(VecOp::Add, {I1, 0x2}, {fog_offset.reg, fog_offset.swz, Mod::Neg}, {I1, YYYY},
          Pred::None, true);
    LutLfFetch();
    Clamp01(I0, 0x1);
    e.MoveInternal({I0, 0xf}, {I0, XXXX}, Pred::None, true);
}

void FragmentEmitter::SampleTextures() {
    // Every SMP on counter 0, one wait for all before the first use. The vertex program wrote
    // TEXCOORD0..2 as (s, 1 - t): the host copy keeps the PICA's rows in memory order, t = 1 at row
    // 0 (an ETC1 block cannot be flipped by moving it). REPEAT, MIRROR, CLAMP and the border test
    // are symmetric under it. The issued units' texels are already in their PAs.
    const auto& tex = config.texture;
    bool any = false;
    uint32_t sampler_index = 0;
    Reg coords[3] = {Reg::Pa(0), Reg::Pa(0), Reg::Pa(0)};
    for (u32 unit = 0; unit < 3; unit++) {
        if (!unit_used[unit]) {
            continue;
        }
        const int coord = (unit == 2 && tex.texture2_use_coord1) ? 1 : static_cast<int>(unit);
        coords[unit] = Reg::Pa(texcoord_pa[coord]); // not iterated for an issued unit alone
        if (unit == 0 && tex.texture0_type == TextureType::TextureCube) {
            // The direction (u, v, w) of texcoord0 and texcoord0_w. The SGX's cube takes a face's
            // (s, t) as the PICA does, and the faces keep t = 1 at row 0: reading at (u, -v, w)
            // flips t on every face, with +Y and -Y swapped in the host cube
            // (RasterizerSgxd::TextureFor). The PA holds 1 - v: -v is that less one.
            const Reg dir = Reg::V(TmpProj);
            e.Vec(VecOp::Add, {dir, 0x3}, {coords[0], XYZW}, ConstZero, Pred::None, true);
            e.Vec(VecOp::Add, {dir, 0x2}, {ConstOne.reg, XXXX, Mod::Neg}, {coords[0], XYZW},
                  Pred::None, true);
            e.Vec(VecOp::Add, {Reg::V(TmpProj + 2), 0x1},
                  {Reg::Pa(static_cast<uint8_t>(texcoord0_w_pa & ~1u)),
                   (texcoord0_w_pa & 1) ? YYYY : XXXX},
                  ConstZero, Pred::None, true);
            coords[0] = dir;
            e.SampleCube(Reg::V(TmpTex0), dir, SamplerSa(sampler_index), 0);
            sampler_index++;
            any = true;
            continue;
        }
        if (unit == 0 && tex.texture0_type == TextureType::Projection2D) {
            // texcoord0 / texcoord0_w, at (s, 1 - t) of the projected coordinate: back to t
            // first, then projected, then flipped again.
            const Reg proj = Reg::V(TmpProj);
            e.Vec(VecOp::Add, {proj, 0x1}, {coords[0], XYZW}, ConstZero, Pred::None, true);
            e.Vec(VecOp::Add, {proj, 0x2}, {coords[0], XYZW, Mod::Neg}, ConstOne, Pred::None, true);
            e.Comp(CompOp::Rcp, {I0, 0x1}, {Reg::Pa(static_cast<uint8_t>(texcoord0_w_pa & ~1u))},
                   static_cast<uint8_t>(texcoord0_w_pa & 1), Pred::None, true);
            e.Vec(VecOp::Mul, {proj, 0x3}, {proj, XYZW}, {I0, XXXX}, Pred::None, true);
            const Reg flipped = Reg::V(TmpTexFlip);
            e.Vec(VecOp::Add, {flipped, 0x3}, {proj, XYZW}, ConstZero, Pred::None, true);
            e.Vec(VecOp::Add, {flipped, 0x2}, {proj, XYZW, Mod::Neg}, ConstOne, Pred::None, true);
            coords[0] = flipped;
        }
        if (!unit_issued[unit]) {
            const bool f16 = TexelF16(unit);
            e.Sample2D(Reg::V(static_cast<uint8_t>(TmpTex0 + unit * 4)), coords[unit],
                       SamplerSa(sampler_index), nullptr, 0, false, f16 ? 2 : 3);
            texel_loc[unit].f16 = f16;
            sampler_index++;
            any = true;
        }
        if ((config.texture.alpha_split >> unit & 1) && !AlphaIssued(unit)) {
            // The alpha from the unit's A8 texture, its byte in .w (swizzle R000): the colour
            // texture is ETC1 kept compressed (SgxdBackend::QueueUntile).
            e.Sample2D(Reg::V(static_cast<uint8_t>(TmpAlphaSplit + unit * 4)), coords[unit],
                       SamplerSa(sampler_alpha[unit]), nullptr, 0);
            any = true;
        }
    }
    if (any) {
        e.Wdf(0);
    }
    for (u32 unit = 0; unit < 3; unit++) {
        if (unit_used[unit] && (config.texture.alpha_split >> unit & 1)) {
            const Vec4Loc texel = texel_loc[unit];
            if (texel.f16) {
                continue; // U8Inputs merges the issued F16 alpha in U8
            }
            const Vec4Loc alpha =
                AlphaIssued(unit)
                    ? alpha_loc[unit]
                    : Vec4Loc{Bank::Virtual, static_cast<uint8_t>(TmpAlphaSplit + unit * 4)};
            e.MoveInternal({texel.Pair(1), 0x2}, {alpha.Pair(1), XYZW}, Pred::None, true);
        }
    }
    // A coordinate past [0, 1] on an axis that clamps to the border colour replaces the
    // texel with it (the generator's sampleTexUnit).
    for (u32 unit = 0; unit < 3; unit++) {
        const auto& border = tex.texture_border_color[unit];
        if (!unit_used[unit] || !(border.enable_s || border.enable_t)) {
            continue;
        }
        const Vec4Loc colour{Bank::SecAttr, static_cast<uint8_t>(sa_border + unit * 4)};
        const Vec4Loc texel = texel_loc[unit];
        const auto replace_when = [&](Cond cond, uint8_t chan, const Src& bound) {
            e.Test(cond, 2, {coords[unit]}, chan, bound, true);
            e.MoveInternal({texel.Pair(0), 0x3}, {colour.Pair(0), XYZW}, Pred::P2, true);
            e.MoveInternal({texel.Pair(1), 0x3}, {colour.Pair(1), XYZW}, Pred::P2, true);
        };
        for (uint8_t chan = 0; chan < 2; chan++) {
            if (chan == 0 ? border.enable_s : border.enable_t) {
                replace_when(Cond::Lt, chan, ConstZero);
                replace_when(Cond::Gt, chan, ConstOne);
            }
        }
    }
    if (uses_shadow) {
        // shadow = tex, or 1 - tex when inverted.
        const auto& lighting = config.lighting;
        const Vec4Loc texel = texel_loc[lighting.shadow_selector];
        const Vec4Loc shadow{Bank::Virtual, TmpShadow};
        if (shadow_scalar) {
            if (lighting.shadow_invert) {
                e.Vec(VecOp::Add, {shadow.Pair(0), 0x1}, {texel.Pair(0), XXXX, Mod::Neg},
                      ConstOne);
            } else {
                e.Vec(VecOp::Add, {shadow.Pair(0), 0x1}, {texel.Pair(0), XXXX}, ConstZero);
            }
        } else if (lighting.shadow_invert) {
            for (int pair = 0; pair < 2; pair++) {
                e.Vec(VecOp::Add, {shadow.Pair(pair), 0x3}, {texel.Pair(pair), XYZW, Mod::Neg},
                      ConstOne);
            }
        } else {
            CopyVec4(shadow, texel);
        }
    }
}

void FragmentEmitter::LoadVec4(Reg internal, Vec4Loc from) {
    if (from.bank == Bank::Const) {
        e.MoveInternal({internal, 0xf}, {Reg::C(0), XXXX}, Pred::None, true);
        return;
    }
    if (from.f16) {
        e.UnpackF16x4({internal, 0xf}, from.Pair(0));
        return;
    }
    e.MoveF32x4({internal, 0xf}, from.Pair(0), from.Pair(1), true);
}

void FragmentEmitter::StoreVec4(Vec4Loc to, Reg internal, uint8_t mask) {
    // Only the lanes written: an internal register's others are another instance's.
    if (mask & 0x3) {
        e.MoveInternal({to.Pair(0), static_cast<uint8_t>(mask & 0x3)}, {internal, XYZW},
                       Pred::None, true);
    }
    if (mask & 0xc) {
        e.MoveInternal({to.Pair(1), static_cast<uint8_t>(mask >> 2)}, {internal, ZWZW},
                       Pred::None, true);
    }
}

void FragmentEmitter::CopyVec4(Vec4Loc to, Vec4Loc from, uint8_t mask) {
    // Two-wide adds of zero: a move between unified-store registers.
    if (mask & 0x3) {
        e.Vec(VecOp::Add, {to.Pair(0), static_cast<uint8_t>(mask & 0x3)}, {from.Pair(0), XYZW},
              ConstZero);
    }
    if (mask & 0xc) {
        e.Vec(VecOp::Add, {to.Pair(1), static_cast<uint8_t>(mask >> 2)}, {from.Pair(1), XYZW},
              ConstZero);
    }
}

void FragmentEmitter::Byteround(Reg value, Reg) {
    // floor(x * 255 + 0.5) / 255. frc computes src1 - floor(src2), so with a zero first
    // source it is -floor(x) in one instruction (psp2cgc's form), undone by the negation
    // on the last multiply.
    e.Vec(VecOp::Mul, {value, 0xf}, {value, XYZW}, Lit(LitTwoFiftyFive), Pred::None, true);
    e.Vec(VecOp::Add, {value, 0xf}, {value, XYZW}, ConstHalf, Pred::None, true);
    e.Vec(VecOp::Frc, {value, 0xf}, ConstZero, {value, XYZW}, Pred::None, true);
    e.Vec(VecOp::Mul, {value, 0xf}, {value, XYZW, Mod::Neg}, Lit(LitInv255), Pred::None, true);
}

void FragmentEmitter::Clamp01(Reg value, uint8_t mask) {
    e.Vec(VecOp::Max, {value, mask}, {value, XYZW}, ConstZero, Pred::None, true);
    e.Vec(VecOp::Min, {value, mask}, {value, XYZW}, ConstOne, Pred::None, true);
}

void FragmentEmitter::Normalize3(Reg into, Vec4Loc from, Reg scratch) {
    LoadVec4(into, from);
    e.Dot({scratch, 0x1}, {into, XYZW}, {into, XYZW}, false, Pred::None, true);
    e.Comp(CompOp::Rsq, {scratch, 0x1}, {scratch}, 0, Pred::None, true);
    e.Vec(VecOp::Mul, {into, 0x7}, {into, XYZW}, {scratch, XXXX}, Pred::None, true);
}

void FragmentEmitter::RoundPrimary() {
    LoadVec4(I0, {Bank::PrimAttr, 0});
    Byteround(I0, I1);
    StoreVec4({Bank::Virtual, TmpPrimary}, I0);
}

void FragmentEmitter::LightingLutLookup(Sampler sampler, bool abs_input, bool two_sided) {
    // The LUT input sits in i0.x. Unsigned: 256 steps over [0, 1]; signed: 128 steps over
    // [-1, 1] with negatives wrapped to the top half of the table. Leaves the value in i0.x.
    // The index is kept negated: frc with a zero first source is -floor in one
    // instruction, and every later step takes the sign in its stride.
    if (abs_input) {
        if (two_sided) {
            e.Vec(VecOp::Add, {I0, 0x1}, {I0, XYZW, Mod::Abs}, ConstZero, Pred::None, true);
        } else {
            e.Vec(VecOp::Max, {I0, 0x1}, {I0, XYZW}, ConstZero, Pred::None, true);
        }
        e.Vec(VecOp::Mul, {I0, 0x1}, {I0, XYZW}, Lit(Lit256), Pred::None, true);
        e.Vec(VecOp::Frc, {I1, 0x2}, ConstZero, {I0, XXXX}, Pred::None, true); // -index
        e.Vec(VecOp::Max, {I1, 0x2}, {I1, XYZW}, Lit(LitNeg255), Pred::None, true);
        e.Vec(VecOp::Min, {I1, 0x2}, {I1, XYZW}, ConstZero, Pred::None, true);
    } else {
        e.Vec(VecOp::Mul, {I0, 0x1}, {I0, XYZW}, Lit(Lit128), Pred::None, true);
        e.Vec(VecOp::Frc, {I1, 0x2}, ConstZero, {I0, XXXX}, Pred::None, true); // -index
        e.Vec(VecOp::Max, {I1, 0x2}, {I1, XYZW}, Lit(LitNeg127), Pred::None, true);
        e.Vec(VecOp::Min, {I1, 0x2}, {I1, XYZW}, Lit(Lit128), Pred::None, true);
    }
    // delta = position - index (kept with the negated index for a LUT at the same input),
    // then the index wrapped and offset to its row.
    e.Vec(VecOp::Add, {I1, 0x1}, {I1, YYYY}, {I0, XXXX}, Pred::None, true);
    e.MoveInternal({Reg::V(TmpLutDelta), 0x3}, {I1, XYZW}, Pred::None, true);
    LightingLutRowFetch(sampler, abs_input);
}

void FragmentEmitter::LightingLutRowFetch(Sampler sampler, bool abs_input) {
    if (!abs_input) {
        e.Test(Cond::Gt, 0, {delta}, 1, ConstZero, false);
        e.Vec(VecOp::Add, {delta, 0x2}, {Lit(Lit256).reg, Lit(Lit256).swz, Mod::Neg}, {delta, YYYY},
              Pred::P0, true);
    }
    // The LUT is one whole row of the lf texture (lighting_lut_offset is row * 256): the row's
    // v comes from the CPU (UniformKind::LightingLutRow).
    const uint32_t sampler_index = static_cast<uint32_t>(sampler);
    const Reg coord = Reg::V(TmpLutCoord);
    e.Mad2({coord, 0x1}, {delta, YYYY, Mod::Abs}, Lit(LitInv256), Lit(LitHalfInv256));
    e.Vec(VecOp::Add, {coord, 0x2}, SaScalar(sa_lut_offset + sampler_index), ConstZero, Pred::None,
          true);
    e.Sample2DPoint(Reg::V(TmpLutEntry), coord, SamplerSa(LutSamplerIndex()), 0);
    e.Wdf(0);
    light_mem_pending = false; // the wait is for every read on the counter
    // entry.r + entry.g * delta in i0.x (vmad2 does not write the internal registers).
    e.Vec(VecOp::Mul, {I0, 0x1}, {Reg::V(TmpLutEntry), YYYY}, {delta, XXXX}, Pred::None, true);
    e.Vec(VecOp::Add, {I0, 0x1}, {I0, XYZW}, {Reg::V(TmpLutEntry), XXXX}, Pred::None, true);
}

void FragmentEmitter::Lighting() {
    const auto& lighting = config.lighting;
    const Vec4Loc normquat{Bank::PrimAttr, normquat_pa, true};
    const Vec4Loc view{Bank::PrimAttr, view_pa, true};
    const Vec4Loc diffuse_sum{Bank::Virtual, TmpDiffuse};
    const Vec4Loc specular_sum{Bank::Virtual, TmpSpecular};
    const Vec4Loc normal{Bank::Virtual, TmpNormal};
    const Vec4Loc tangent{Bank::Virtual, TmpTangent};
    const Vec4Loc nview{Bank::Virtual, TmpView};
    // The light's vector: TmpLight, or a pre-normalised light's own uniform (set per light).
    Vec4Loc light_vector{Bank::Virtual, TmpLight};
    const Vec4Loc half_vector{Bank::Virtual, TmpHalf};
    const Reg dot_product = Reg::V(TmpMisc);   // .x; .y the squared half vector length
    const Reg misc_hi = Reg::V(TmpMisc + 2);   // .x clamp_highlights, .y light distance
    const bool config7 = lighting.config == LightingRegs::LightingConfig::Config7;

    // diffuse_sum = specular_sum = (0, 0, 0, 1): the alpha here, the colour by the first
    // light's store (or zero below when there is none).
    if (uses_primary_color) {
        e.Vec(VecOp::Add, {diffuse_sum.Pair(1), 0x2}, ConstZero, {Reg::C(1), XYZW});
    }
    if (uses_secondary_color) {
        e.Vec(VecOp::Add, {specular_sum.Pair(1), 0x2}, ConstZero, {Reg::C(1), XYZW});
    }
    if (lighting.src_num == 0) {
        e.Vec(VecOp::Add, {diffuse_sum.Pair(0), 0x3}, ConstZero, ConstZero);
        e.Vec(VecOp::Add, {diffuse_sum.Pair(1), 0x1}, ConstZero, ConstZero);
        e.Vec(VecOp::Add, {specular_sum.Pair(0), 0x3}, ConstZero, ConstZero);
        e.Vec(VecOp::Add, {specular_sum.Pair(1), 0x1}, ConstZero, ConstZero);
    }
    bool diffuse_empty = true;
    bool specular_empty = true;
    // The three colour lanes of an internal register into a quad.
    const auto store3 = [&](Vec4Loc to, Reg internal) {
        e.MoveInternal({to.Pair(0), 0x3}, {internal, XYZW}, Pred::None, true);
        e.MoveInternal({to.Pair(1), 0x1}, {internal, ZWZW}, Pred::None, true);
    };

    // The surface normal and tangent in the quaternion's frame, from the bump map or the
    // defaults.
    const auto perturbation = [&](Reg into) {
        // 2 * tex.rgb - 1
        const Vec4Loc tex = texel_loc[lighting.bump_selector];
        LoadVec4(into, tex);
        e.Vec(VecOp::Mul, {into, 0x7}, {into, XYZW}, Lit(LitTwo), Pred::None, true);
        e.Vec(VecOp::Add, {into, 0x7}, {ConstOne.reg, XXXX, Mod::Neg}, {into, XYZW}, Pred::None,
              true);
    };
    // i0 = surface normal, i1 = surface tangent (only when Config7 wants a tangent). All four
    // lanes: a parked vector is stored and reloaded whole, and an internal register's unwritten
    // lanes are another instance's.
    const auto default_normal = [&] { // (0, 0, 1, 0)
        e.MoveInternal({I0, 0xf}, {Reg::C(0), XXXX}, Pred::None, true);
        e.Vec(VecOp::Add, {I0, 0x4}, ConstOne, ConstZero, Pred::None, true);
    };
    const auto default_tangent = [&] { // (1, 0, 0, 0)
        e.MoveInternal({I1, 0xf}, {Reg::C(0), XXXX}, Pred::None, true);
        e.Vec(VecOp::Add, {I1, 0x1}, ConstOne, ConstZero, Pred::None, true);
    };
    switch (lighting.bump_mode) {
    case LightingRegs::LightingBumpMode::NormalMap:
        perturbation(I0);
        if (lighting.bump_renorm) {
            // z = sqrt(max(1 - (x*x + y*y), 0))
            e.Vec(VecOp::Mul, {I1, 0x3}, {I0, XYZW}, {I0, XYZW}, Pred::None, true);
            e.Vec(VecOp::Add, {I1, 0x1}, {I1, XYZW}, {I1, YYYY}, Pred::None, true);
            e.Vec(VecOp::Add, {I1, 0x1}, {I1, XYZW, Mod::Neg}, ConstOne, Pred::None, true);
            e.Vec(VecOp::Max, {I1, 0x1}, {I1, XYZW}, ConstZero, Pred::None, true);
            e.Comp(CompOp::Rsq, {I1, 0x2}, {I1}, 0, Pred::None, true);
            e.Vec(VecOp::Mul, {I0, 0x4}, {I1, XXXX}, {I1, YYYY}, Pred::None, true); // x * rsq(x)
            // rsq(0) is inf and 0 * inf is nan: a zero input gives a zero z.
            e.Test(Cond::Gt, 0, {I1}, 0, ConstZero, false);
            e.MoveInternal({I0, 0x4}, {Reg::C(0), XXXX}, Pred::NotP0, true);
        }
        default_tangent();
        break;
    case LightingRegs::LightingBumpMode::TangentMap:
        perturbation(I1);
        default_normal();
        break;
    default:
        default_normal();
        default_tangent();
        break;
    }
    // Park them: the quaternion work below needs all three internal registers. The
    // default normal is not parked: its rotation has a closed form (below).
    const bool default_normal_only = lighting.bump_mode != LightingRegs::LightingBumpMode::NormalMap &&
                                     lighting.bump_mode != LightingRegs::LightingBumpMode::TangentMap &&
                                     !config7;
    if (!default_normal_only) {
        StoreVec4(normal, I0);
    }
    if (config7) {
        StoreVec4(tangent, I1);
    }

    // quat = normalize(dot(normquat, normquat_flat) < 0 ? -normquat : normquat)
    LoadVec4(I0, normquat);
    e.Dot({I2, 0x1}, {I0, XYZW}, {I0, XYZW}, true, Pred::None, true);
    e.Comp(CompOp::Rsq, {I2, 0x1}, {I2}, 0, Pred::None, true);
    e.Vec(VecOp::Mul, {I0, 0xf}, {I0, XYZW}, {I2, XXXX}, Pred::None, true);
    // rotate(q, v) = v + 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v), for the normal and
    // the tangent; i0 = q throughout.
    const auto rotate = [&](Vec4Loc v) {
        LoadVec4(I1, v);
        // i2 = cross(q, v) + q.w * v = q.yzx * v.zxy - q.zxy * v.yzx + q.w * v. The
        // second source only has the standard swizzle table, which holds zxy but not yzx,
        // so yzx always goes on the first source.
        constexpr Swizzle YZX{Ch::Y, Ch::Z, Ch::X, Ch::W};
        constexpr Swizzle ZXY{Ch::Z, Ch::X, Ch::Y, Ch::W};
        e.Vec(VecOp::Mul, {I2, 0x7}, {I0, YZX}, {I1, ZXY}, Pred::None, true);
        e.Vec(VecOp::Mul, {I1, 0x7}, {I1, YZX}, {I0, ZXY}, Pred::None, true);
        e.Vec(VecOp::Add, {I2, 0x7}, {I1, XYZW, Mod::Neg}, {I2, XYZW}, Pred::None, true);
        LoadVec4(I1, v);
        e.Mad({I2, 0x7}, {I1, XYZW}, {I0, WWWW}, {I2, XYZW}, true, Pred::None, true);
        // i1 = cross(q, i2) = q.yzx * i2.zxy - q.zxy * i2.yzx; result = v + 2 * i1
        e.Vec(VecOp::Mul, {I1, 0x7}, {I0, YZX}, {I2, ZXY}, Pred::None, true);
        e.Vec(VecOp::Mul, {I2, 0x7}, {I2, YZX}, {I0, ZXY}, Pred::None, true);
        e.Vec(VecOp::Add, {I1, 0x7}, {I2, XYZW, Mod::Neg}, {I1, XYZW}, Pred::None, true);
        e.Vec(VecOp::Mul, {I1, 0x7}, {I1, XYZW}, Lit(LitTwo), Pred::None, true);
        LoadVec4(I2, v);
        e.Vec(VecOp::Add, {I1, 0x7}, {I1, XYZW}, {I2, XYZW}, Pred::None, true);
        StoreVec4(v, I1);
    };
    if (default_normal_only) {
        // rotate(q, (0, 0, 1)) = (2(xz + yw), 2(yz - xw), 1 - 2(x^2 + y^2)), with i0 = q:
        // P = (x^2, y^2, xz, yz) doubled, Q = (yw, xw, yw, xw), then n.xy = (2, -2) * Q.zw
        // + 2P.zw and n.z = 1 - (2P.x + 2P.y). Six instructions for the general rotation's
        // twenty-two.
        constexpr Swizzle XYZZ{Ch::X, Ch::Y, Ch::Z, Ch::Z};
        constexpr Swizzle XYXY{Ch::X, Ch::Y, Ch::X, Ch::Y};
        constexpr Swizzle YXYX{Ch::Y, Ch::X, Ch::Y, Ch::X};
        e.Vec(VecOp::Mul, {I1, 0xf}, {I0, XYZZ}, {I0, XYXY}, Pred::None, true);
        e.Vec(VecOp::Mul, {I2, 0xf}, {I0, YXYX}, {I0, WWWW}, Pred::None, true);
        e.Vec(VecOp::Mul, {I1, 0xf}, {I1, XYZW}, Lit(LitTwo), Pred::None, true);
        e.Mad({I2, 0x3}, {Lit(LitPlusTwo).reg, XYZW}, {I2, ZWZW}, {I1, ZWZW}, true, Pred::None,
              true);
        e.Vec(VecOp::Add, {I2, 0x4}, {I1, XXXX}, {I1, YYYY}, Pred::None, true);
        e.Vec(VecOp::Add, {I2, 0x4}, {I2, ZZZZ, Mod::Neg}, ConstOne, Pred::None, true);
        StoreVec4(normal, I2);
    } else {
        rotate(normal);
        if (config7) {
            rotate(tangent);
        }
    }
    // What the enabled LUTs read decides whether the view and half vectors are needed at
    // all: a light with the distribution LUT on the (light, normal) angle alone needs
    // neither.
    bool needs_half = false;
    bool needs_view = false;
    for (u32 li = 0; li < lighting.src_num; li++) {
        const auto& light = lighting.lights[li];
        if (uses_secondary_color && (light.geometric_factor_0 || light.geometric_factor_1)) {
            needs_half = true;
        }
        const auto input_of = [&](const Pica::Shader::LutConfig& lut, Sampler sampler) {
            if (!LightingLutEnabled(lut, sampler)) {
                return;
            }
            switch (lut.type.Value()) {
            case LutInput::NH:
            case LutInput::VH:
            case LutInput::CP:
                needs_half = true;
                if (lut.type.Value() == LutInput::VH) {
                    needs_view = true;
                }
                break;
            case LutInput::NV:
                needs_view = true;
                break;
            default:
                break;
            }
        };
        if (uses_secondary_color) {
            input_of(lighting.lut_d0, Sampler::Distribution0);
            input_of(lighting.lut_d1, Sampler::Distribution1);
            input_of(lighting.lut_rr, Sampler::ReflectRed);
            input_of(lighting.lut_rg, Sampler::ReflectGreen);
            input_of(lighting.lut_rb, Sampler::ReflectBlue);
        }
        if (li + 1 == lighting.src_num &&
            ((uses_primary_color && lighting.enable_primary_alpha) ||
             (uses_secondary_color && lighting.enable_secondary_alpha))) {
            input_of(lighting.lut_fr, Sampler::Fresnel);
        }
        if (light.spot_atten_enable) {
            input_of(lighting.lut_sp, Sampler::SpotlightAttenuation);
        }
    }
    if (needs_half || needs_view) {
        Normalize3(I0, view, I1);
        StoreVec4(nview, I0);
    }

    // The LUT inputs computed per light, into i0.x, from the vectors parked in temps.
    const auto lut_input = [&](LutInput input) {
        switch (input) {
        case LutInput::NH:
            LoadVec4(I1, half_vector);
            e.Dot({I0, 0x1}, {normal.Pair(0), XYZW}, {I1, XYZW}, false, Pred::None, true);
            break;
        case LutInput::VH:
            LoadVec4(I1, half_vector);
            e.Dot({I0, 0x1}, {nview.Pair(0), XYZW}, {I1, XYZW}, false, Pred::None, true);
            break;
        case LutInput::NV:
            LoadVec4(I1, nview);
            e.Dot({I0, 0x1}, {normal.Pair(0), XYZW}, {I1, XYZW}, false, Pred::None, true);
            break;
        case LutInput::LN:
            LoadVec4(I1, normal);
            e.Dot({I0, 0x1}, {light_vector.Pair(0), XYZW}, {I1, XYZW}, false, Pred::None, true);
            break;
        case LutInput::SP:
            // dot(light_vector, spot_dir), spot_dir loaded per light into TmpLightAcc.
            LoadVec4(I1, {Bank::Virtual, TmpLightAcc});
            e.Dot({I0, 0x1}, {light_vector.Pair(0), XYZW}, {I1, XYZW}, false, Pred::None, true);
            break;
        case LutInput::CP:
            if (config7) {
                // dot(half - normal * dot(normal, half), tangent)
                LoadVec4(I1, half_vector);
                e.Dot({I0, 0x1}, {normal.Pair(0), XYZW}, {I1, XYZW}, false, Pred::None, true);
                LoadVec4(I2, normal);
                e.Vec(VecOp::Mul, {I2, 0x7}, {I2, XYZW}, {I0, XXXX}, Pred::None, true);
                e.Vec(VecOp::Add, {I1, 0x7}, {I2, XYZW, Mod::Neg}, {I1, XYZW}, Pred::None, true);
                e.Dot({I0, 0x1}, {tangent.Pair(0), XYZW}, {I1, XYZW}, false, Pred::None, true);
            } else {
                e.MoveInternal({I0, 0x1}, {Reg::C(0), XXXX}, Pred::None, true);
            }
            break;
        default:
            e.MoveInternal({I0, 0x1}, {Reg::C(0), XXXX}, Pred::None, true);
            break;
        }
    };
    // scale * LUT(input) into i0.x, for an enabled LUT. Within a light the vectors stay put, so
    // a LUT read at the input the previous one indexed (a game's D0 and D1 both at N.V) takes its
    // index from that lookup: no dot product and no index arithmetic again.
    struct LutIndexKey {
        LutInput input;
        bool abs_input;
        bool two_sided;
        bool operator==(const LutIndexKey&) const = default;
    };
    std::optional<LutIndexKey> last_lut_index;
    const auto lut_value = [&](const Pica::Shader::LutConfig& lut, Sampler sampler,
                               bool two_sided) {
        const LutIndexKey key{lut.type.Value(), static_cast<bool>(lut.abs_input), two_sided};
        if (last_lut_index == key) {
            LightingLutRowFetch(sampler, key.abs_input); // TmpLutDelta holds its index
        } else {
            lut_input(key.input);
            LightingLutLookup(sampler, key.abs_input, two_sided);
            last_lut_index = key;
        }
        const float scale = lut.GetScale();
        if (scale != 1.0f) {
            // Scales are powers of two between 0.25 and 8: made from the constant bank.
            if (scale == 2.0f) {
                e.Vec(VecOp::Mul, {I0, 0x1}, {I0, XYZW}, Lit(LitTwo), Pred::None, true);
            } else if (scale == 4.0f) {
                e.Vec(VecOp::Mul, {I0, 0x1}, {I0, XYZW}, Lit(LitFour), Pred::None, true);
            } else if (scale == 8.0f) {
                e.Vec(VecOp::Mul, {I0, 0x1}, {I0, XYZW}, Lit(LitTwo), Pred::None, true);
                e.Vec(VecOp::Mul, {I0, 0x1}, {I0, XYZW}, Lit(LitFour), Pred::None, true);
            } else if (scale == 0.5f) {
                e.Vec(VecOp::Mul, {I0, 0x1}, {I0, XYZW}, ConstHalf, Pred::None, true);
            } else if (scale == 0.25f) {
                e.Vec(VecOp::Mul, {I0, 0x1}, {I0, XYZW}, ConstHalf, Pred::None, true);
                e.Vec(VecOp::Mul, {I0, 0x1}, {I0, XYZW}, ConstHalf, Pred::None, true);
            }
        }
    };

    for (u32 light_index = 0; light_index < lighting.src_num; light_index++) {
        const auto& light = lighting.lights[light_index];
        last_lut_index.reset(); // another light's vectors
        const Vec4Loc params = LightParams(light.num);
        const Bank pb = params.bank;
        const uint32_t sa = params.base;
        const Vec4Loc position{pb, static_cast<uint8_t>(sa + 16)};
        const Vec4Loc spot{pb, static_cast<uint8_t>(sa + 20)};
        const Vec4Loc diffuse{pb, static_cast<uint8_t>(sa + 8)};
        const Vec4Loc ambient{pb, static_cast<uint8_t>(sa + 12)};
        const Vec4Loc specular_0{pb, static_cast<uint8_t>(sa + 0)};
        const Vec4Loc specular_1{pb, static_cast<uint8_t>(sa + 4)};
        const bool two_sided = light.two_sided_diffuse;
        const bool shadow_primary = uses_shadow && lighting.shadow_primary && light.shadow_enable;
        const bool shadow_secondary =
            uses_shadow && lighting.shadow_secondary && light.shadow_enable;
        const Vec4Loc shadow{Bank::Virtual, TmpShadow};
        // A memory light's quads: 0 specular_0 (and the distance bias in .w), 1 specular_1
        // (the distance scale), 2 diffuse, 3 ambient, 4 position, 5 the spot direction.
        LightQuads(0x30, true);

        // light_vector = position (+ view unless directional); distance; normalised.
        // Normalising a directional light's direction once per draw in a secondary program
        // (psp2cgc does that) drew a 32-pixel tile pattern over every lit surface on the
        // console, 2026-09-06: the tiles of one fragment job do not all see the same
        // secondary attributes once the secondary program has written them.
        LoadVec4(I0, position);
        if (LightPreNormalised(light)) {
            light_vector = position;
        } else {
            if (!light.directional) {
                LoadVec4(I1, view);
                e.Vec(VecOp::Add, {I0, 0x7}, {I0, XYZW}, {I1, XYZW}, Pred::None, true);
            }
            e.Dot({I1, 0x1}, {I0, XYZW}, {I0, XYZW}, false, Pred::None, true);
            e.Comp(CompOp::Rsq, {I1, 0x2}, {I1}, 0, Pred::None, true);
            if (light.dist_atten_enable) {
                e.Vec(VecOp::Mul, {I1, 0x1}, {I1, XYZW}, {I1, YYYY}, Pred::None, true); // length
                e.MoveInternal({misc_hi, 0x2}, {I1, XXXX}, Pred::None, true); // .y = distance
            }
            e.Vec(VecOp::Mul, {I0, 0x7}, {I0, XYZW}, {I1, YYYY}, Pred::None, true);
            light_vector = {Bank::Virtual, TmpLight};
            StoreVec4(light_vector, I0);
        }
        CopyVec4({Bank::Virtual, TmpLightAcc}, spot, 0x7);
        // half_vector = normalize(nview + light_vector); its squared length kept for the
        // geometric factor.
        const bool geo0 = light.geometric_factor_0;
        const bool geo1 = light.geometric_factor_1;
        if (needs_half) {
            LoadVec4(I1, nview);
            e.Vec(VecOp::Add, {I1, 0x7}, {I1, XYZW}, {I0, XYZW}, Pred::None, true);
            e.Dot({I2, 0x1}, {I1, XYZW}, {I1, XYZW}, false, Pred::None, true);
            if (geo0 || geo1) {
                e.MoveInternal({dot_product, 0x2}, {I2, XXXX}, Pred::None, true);
            }
            e.Comp(CompOp::Rsq, {I2, 0x1}, {I2}, 0, Pred::None, true);
            e.Vec(VecOp::Mul, {I1, 0x7}, {I1, XYZW}, {I2, XXXX}, Pred::None, true);
            StoreVec4(half_vector, I1);
        }
        // dot_product = two_sided ? |dot(l, n)| : max(dot(l, n), 0)
        LoadVec4(I1, normal);
        e.Dot({I0, 0x1}, {light_vector.Pair(0), XYZW}, {I1, XYZW}, false, Pred::None, true);
        if (two_sided) {
            e.Vec(VecOp::Add, {I0, 0x1}, {I0, XYZW, Mod::Abs}, ConstZero, Pred::None, true);
        } else {
            e.Vec(VecOp::Max, {I0, 0x1}, {I0, XYZW}, ConstZero, Pred::None, true);
        }
        e.MoveInternal({dot_product, 0x1}, {I0, XYZW}, Pred::None, true);
        // clamp_highlights = sign(dot_product): 1 when positive, else 0 (never negative).
        if (lighting.clamp_highlights) {
            e.Test(Cond::Gt, 0, {I0}, 0, ConstZero, false);
            e.MoveInternal({misc_hi, 0x1}, {Reg::C(0), XXXX}, Pred::None, true);
            e.MoveInternal({misc_hi, 0x1}, {Reg::C(2), XXXX}, Pred::P0, true);
        }

        // geo_factor = hh == 0 ? 0 : min(dot_product / hh, 1), in `atten`.y.
        const Reg atten = Reg::V(TmpLightAtten);
        if ((geo0 || geo1) && uses_secondary_color) {
            e.Comp(CompOp::Rcp, {I0, 0x2}, {dot_product}, 1, Pred::None, true);
            e.Vec(VecOp::Mul, {I0, 0x2}, {I0, YYYY}, {dot_product, XXXX}, Pred::None, true);
            e.Vec(VecOp::Min, {I0, 0x2}, {I0, XYZW}, ConstOne, Pred::None, true);
            e.Test(Cond::Gt, 0, {dot_product}, 1, ConstZero, false);
            e.MoveInternal({I0, 0x2}, {Reg::C(0), XXXX}, Pred::NotP0, true);
            e.MoveInternal({atten, 0x2}, {I0, XYZW}, Pred::None, true);
        }
        // Attenuation: spot and distance, multiplied into `atten`.x (starts at 1; no
        // register at all when neither is on).
        const bool spot_on = light.spot_atten_enable &&
                             LightingLutEnabled(lighting.lut_sp, Sampler::SpotlightAttenuation);
        const bool atten_on = spot_on || light.dist_atten_enable;
        if (atten_on) {
            e.MoveInternal({atten, 0x1}, {Reg::C(2), XXXX}, Pred::None, true);
        }
        if (spot_on) {
            lut_value(lighting.lut_sp, LightingRegs::SpotlightAttenuationSampler(light.num),
                      two_sided);
            e.Vec(VecOp::Mul, {atten, 0x1}, {atten, XYZW}, {I0, XXXX}, Pred::None, true);
        }
        if (light.dist_atten_enable) {
            // index = clamp(scale * distance + bias, 0, 1), unsigned lookup (the bias and
            // scale are the .w of quads 0 and 1; a load takes four words).
            LightQuads(0x3, true);
            e.Vec(VecOp::Mul, {I0, 0x1}, {misc_hi, YYYY}, Scalar(pb, sa + 7), Pred::None, true);
            e.Vec(VecOp::Add, {I0, 0x1}, {I0, XYZW}, Scalar(pb, sa + 3), Pred::None, true);
            Clamp01(I0, 0x1);
            LightingLutLookup(LightingRegs::DistanceAttenuationSampler(light.num), true, false);
            last_lut_index.reset(); // TmpLutDelta holds the distance's index now
            e.Vec(VecOp::Mul, {atten, 0x1}, {atten, XYZW}, {I0, XXXX}, Pred::None, true);
        }

        if (uses_secondary_color) {
        // refl = (rr ? LUT : 1, rg ? LUT : refl.r, rb ? LUT : refl.r), kept in TmpLightAcc
        // (the spot direction is no longer needed by then); no register when all three
        // LUTs are off (refl = 1).
        const Vec4Loc acc{Bank::Virtual, TmpLightAcc};
        const bool rr = LightingLutEnabled(lighting.lut_rr, Sampler::ReflectRed);
        const bool rg = LightingLutEnabled(lighting.lut_rg, Sampler::ReflectGreen);
        const bool rb = LightingLutEnabled(lighting.lut_rb, Sampler::ReflectBlue);
        const bool d1_on = LightingLutEnabled(lighting.lut_d1, Sampler::Distribution1);
        const bool d0_on = LightingLutEnabled(lighting.lut_d0, Sampler::Distribution0);
        const bool refl_on = rr || rg || rb;
        if (refl_on) {
            if (rg) {
                lut_value(lighting.lut_rg, Sampler::ReflectGreen, two_sided);
                e.MoveInternal({acc.Pair(0), 0x2}, {I0, XXXX}, Pred::None, true);
            }
            if (rb) {
                lut_value(lighting.lut_rb, Sampler::ReflectBlue, two_sided);
                e.MoveInternal({acc.Pair(1), 0x1}, {I0, XXXX}, Pred::None, true);
            }
            if (rr) {
                lut_value(lighting.lut_rr, Sampler::ReflectRed, two_sided);
            } else {
                e.MoveInternal({I0, 0x1}, {Reg::C(2), XXXX}, Pred::None, true);
            }
            e.MoveInternal({acc.Pair(0), 0x1}, {I0, XXXX}, Pred::None, true);
            if (!rg) {
                e.MoveInternal({acc.Pair(0), 0x2}, {I0, XXXX}, Pred::None, true);
            }
            if (!rb) {
                e.MoveInternal({acc.Pair(1), 0x1}, {I0, XXXX}, Pred::None, true);
            }
        }
        // specular_1 = d1 * refl * light.specular_1 (each factor only when it is not 1),
        // into i2; parked in the accumulator's home when the d0 lookup runs next (it
        // needs the internal registers). A memory light's quads load with the LUT fetch
        // before their use: specular_1's now, with specular_0's and the diffuse ones unless
        // the d0 fetch comes (i2 holds a value until then, so no wait of their own after it).
        const uint32_t diffuse_quads = uses_primary_color ? 0xcu : 0u;
        LightQuads(1u << 1 | (d0_on ? 0u : 1u | diffuse_quads), !d1_on);
        if (d1_on) {
            lut_value(lighting.lut_d1, Sampler::Distribution1, two_sided);
            LoadVec4(I2, specular_1);
            e.Vec(VecOp::Mul, {I2, 0x7}, {I2, XYZW}, {I0, XXXX}, Pred::None, true);
        } else {
            LoadVec4(I2, specular_1);
        }
        if (refl_on) {
            LoadVec4(I1, acc);
            e.Vec(VecOp::Mul, {I2, 0x7}, {I2, XYZW}, {I1, XYZW}, Pred::None, true);
        }
        if (geo1) {
            e.Vec(VecOp::Mul, {I2, 0x7}, {I2, XYZW}, {atten, YYYY}, Pred::None, true);
        }
        if (d0_on) {
            StoreVec4(acc, I2);
        }
        // specular_0 = d0 * light.specular_0 (d0 from its LUT or 1), added on.
        if (d0_on) {
            LightQuads(1u | diffuse_quads, false);
            lut_value(lighting.lut_d0, Sampler::Distribution0, two_sided);
            LoadVec4(I1, specular_0);
            e.Vec(VecOp::Mul, {I1, 0x7}, {I1, XYZW}, {I0, XXXX}, Pred::None, true);
        } else {
            LoadVec4(I1, specular_0);
        }
        if (geo0) {
            e.Vec(VecOp::Mul, {I1, 0x7}, {I1, XYZW}, {atten, YYYY}, Pred::None, true);
        }
        if (d0_on) {
            LoadVec4(I2, acc);
        }
        e.Vec(VecOp::Add, {I1, 0x7}, {I1, XYZW}, {I2, XYZW}, Pred::None, true);
        // * clamp_highlights * atten, into specular_sum.
        if (lighting.clamp_highlights) {
            e.Vec(VecOp::Mul, {I1, 0x7}, {I1, XYZW}, {misc_hi, XXXX}, Pred::None, true);
        }
        if (atten_on) {
            e.Vec(VecOp::Mul, {I1, 0x7}, {I1, XYZW}, {atten, XXXX}, Pred::None, true);
        }
        if (shadow_secondary && shadow_scalar) {
            e.Vec(VecOp::Mul, {I1, 0x7}, {I1, XYZW}, {shadow.Pair(0), XXXX}, Pred::None, true);
        } else if (shadow_secondary) {
            LoadVec4(I2, shadow);
            e.Vec(VecOp::Mul, {I1, 0x7}, {I1, XYZW}, {I2, XYZW}, Pred::None, true);
        }
        if (specular_empty) {
            store3(specular_sum, I1);
            specular_empty = false;
        } else {
            LoadVec4(I2, specular_sum);
            e.Vec(VecOp::Add, {I2, 0x7}, {I2, XYZW}, {I1, XYZW}, Pred::None, true);
            store3(specular_sum, I2);
        }
        } // uses_secondary_color
        if (uses_primary_color) {
        // diffuse_sum += (diffuse * dot_product + ambient) * atten
        LightQuads(0xc, true);
        LoadVec4(I0, diffuse);
        LoadVec4(I1, ambient);
        e.Vec(VecOp::Mul, {I0, 0x7}, {I0, XYZW}, {dot_product, XXXX}, Pred::None, true);
        if (shadow_primary && shadow_scalar) {
            e.Vec(VecOp::Mul, {I0, 0x7}, {I0, XYZW}, {shadow.Pair(0), XXXX}, Pred::None, true);
        } else if (shadow_primary) {
            LoadVec4(I2, shadow);
            e.Vec(VecOp::Mul, {I0, 0x7}, {I0, XYZW}, {I2, XYZW}, Pred::None, true);
        }
        e.Vec(VecOp::Add, {I0, 0x7}, {I0, XYZW}, {I1, XYZW}, Pred::None, true);
        if (atten_on) {
            e.Vec(VecOp::Mul, {I0, 0x7}, {I0, XYZW}, {atten, XXXX}, Pred::None, true);
        }
        if (diffuse_empty) {
            store3(diffuse_sum, I0);
            diffuse_empty = false;
        } else {
            LoadVec4(I2, diffuse_sum);
            e.Vec(VecOp::Add, {I2, 0x7}, {I2, XYZW}, {I0, XYZW}, Pred::None, true);
            store3(diffuse_sum, I2);
        }
        } // uses_primary_color

        // The fresnel term, on the last light, into the alphas asked for.
        const bool fresnel_primary = uses_primary_color && lighting.enable_primary_alpha;
        const bool fresnel_secondary = uses_secondary_color && lighting.enable_secondary_alpha;
        if (light_index + 1 == lighting.src_num && (fresnel_primary || fresnel_secondary) &&
            LightingLutEnabled(lighting.lut_fr, Sampler::Fresnel)) {
            lut_value(lighting.lut_fr, Sampler::Fresnel, two_sided);
            if (fresnel_primary) {
                e.MoveInternal({diffuse_sum.Pair(1), 0x2}, {I0, XXXX}, Pred::None, true);
            }
            if (fresnel_secondary) {
                e.MoveInternal({specular_sum.Pair(1), 0x2}, {I0, XXXX}, Pred::None, true);
            }
        }
    }

    if (uses_shadow && lighting.shadow_alpha) {
        const Vec4Loc shadow{Bank::Virtual, TmpShadow};
        const Src shadow_alpha =
            shadow_scalar ? Src{shadow.Pair(0), XXXX} : Src{shadow.Pair(1), YYYY};
        if (lighting.enable_primary_alpha && uses_primary_color) {
            e.Vec(VecOp::Mul, {diffuse_sum.Pair(1), 0x2}, {diffuse_sum.Pair(1), XYZW},
                  shadow_alpha);
        }
        if (lighting.enable_secondary_alpha && uses_secondary_color) {
            e.Vec(VecOp::Mul, {specular_sum.Pair(1), 0x2}, {specular_sum.Pair(1), XYZW},
                  shadow_alpha);
        }
    }
    // diffuse_sum.rgb += global ambient; both clamped in place. For the U8 combiners they are
    // packed here instead, the pack saturating as the clamp does, unless a moved channel of
    // one is packed again from its F32 lanes (U8Inputs' broadcasts).
    bool lanes_moved = false;
    if (u8_tev) {
        ForEachU8Arg([&](u32, Source csrc, ColorModifier cmod, Source asrc, AlphaModifier amod) {
            const auto lit = [](Source s) {
                return s == Source::PrimaryFragmentColor || s == Source::SecondaryFragmentColor;
            };
            lanes_moved = lanes_moved || (lit(csrc) && MovedLane(cmod) >= 0) ||
                          (lit(asrc) && MovedLane(amod) >= 0);
        });
    }
    u8_lighting_packed = u8_tev && !lanes_moved;
    if (uses_primary_color) {
        LoadVec4(I0, diffuse_sum);
        LoadVec4(I1, {Bank::SecAttr, static_cast<uint8_t>(sa_global_ambient)});
        e.Vec(VecOp::Add, {I0, 0x7}, {I0, XYZW}, {I1, XYZW}, Pred::None, true);
        if (u8_lighting_packed) {
            e.PackU8(Reg::V(TmpU8Diffuse), I0, I0);
        } else {
            Clamp01(I0);
            StoreVec4(diffuse_sum, I0);
        }
    }
    if (uses_secondary_color) {
        if (u8_lighting_packed) {
            e.PackU8(Reg::V(TmpU8Specular), specular_sum.Pair(0), specular_sum.Pair(1));
        } else {
            LoadVec4(I0, specular_sum);
            Clamp01(I0);
            StoreVec4(specular_sum, I0);
        }
    }
}

void FragmentEmitter::PlanCombinerBuffer() {
    // The generator's dataflow: combiner_buffer starts at zero, next_combiner_buffer at the
    // uniform; after every stage, buffer = next, then next takes the stage's colour and/or
    // alpha when the stage updates the buffer. So stage k reads the last updating stage at
    // most k - 2 back, else the uniform, and stage 0 reads zero.
    int color_from = -1;
    int alpha_from = -1;
    for (u32 k = 0; k < 6; k++) {
        buffer_color_from[k] = k == 0 ? -2 : color_from;
        buffer_alpha_from[k] = k == 0 ? -2 : alpha_from;
        // What stage k's shuffle hands the stage after next.
        const u32 j = k >= 1 ? k - 1 : 0;
        if (k >= 1) {
            if (config.TevStageUpdatesCombinerBufferColor(j)) {
                color_from = static_cast<int>(j);
            }
            if (config.TevStageUpdatesCombinerBufferAlpha(j)) {
                alpha_from = static_cast<int>(j);
            }
        }
    }
    snapshot_lanes.fill(0);
    if (!buffer_used) {
        return;
    }
    for (u32 k = 0; k < 6; k++) {
        const TevStageConfig stage = config.texture.tev_stages[k];
        if (IsPassThroughTevStage(stage)) {
            continue;
        }
        // The lane the modifier reads, not the argument's kind: a colour argument can read a
        // buffered stage's alpha (SourceAlpha).
        const auto reads = [&](Source src, bool alpha) {
            if (src != Source::PreviousBuffer) {
                return;
            }
            const int from = alpha ? buffer_alpha_from[k] : buffer_color_from[k];
            if (from >= 0) {
                snapshot_lanes[from] |= alpha ? 0x8 : 0x7;
            }
        };
        reads(ColorSource(stage, stage.color_source1, k), ReadsAlphaLane(stage.color_modifier1));
        reads(ColorSource(stage, stage.color_source2, k), ReadsAlphaLane(stage.color_modifier2));
        reads(ColorSource(stage, stage.color_source3, k), ReadsAlphaLane(stage.color_modifier3));
        reads(AlphaSource(stage, stage.alpha_source1, k), ReadsAlphaLane(stage.alpha_modifier1));
        reads(AlphaSource(stage, stage.alpha_source2, k), ReadsAlphaLane(stage.alpha_modifier2));
        reads(AlphaSource(stage, stage.alpha_source3, k), ReadsAlphaLane(stage.alpha_modifier3));
    }
}

Vec4Loc FragmentEmitter::SourceLocation(Source source, u32 stage_index, bool alpha) const {
    switch (source) {
    case Source::PrimaryColor:
        return {Bank::Virtual, TmpPrimary};
    case Source::PrimaryFragmentColor:
        // Zero without lighting, as the generator has it.
        return uses_lighting ? Vec4Loc{Bank::Virtual, TmpDiffuse} : Vec4Loc{Bank::Const, 0};
    case Source::SecondaryFragmentColor:
        return uses_lighting ? Vec4Loc{Bank::Virtual, TmpSpecular} : Vec4Loc{Bank::Const, 0};
    case Source::Texture0:
        return texel_loc[0];
    case Source::Texture1:
        return texel_loc[1];
    case Source::Texture2:
        return texel_loc[2];
    case Source::PreviousBuffer: {
        const int from = alpha ? buffer_alpha_from[stage_index] : buffer_color_from[stage_index];
        if (from == -2) {
            return {Bank::Const, 0};
        }
        if (from == -1) {
            return {Bank::SecAttr, static_cast<uint8_t>(sa_buffer_color)};
        }
        return {Bank::Virtual, static_cast<uint8_t>(TmpSnap + from * 4)};
    }
    case Source::Texture3:
        return proctex_tabled ? proctex_loc
               : uses_proctex ? Vec4Loc{Bank::Virtual, TmpTex3}
                              : Vec4Loc{Bank::Const, 0};
    case Source::Constant:
        return {Bank::SecAttr, static_cast<uint8_t>(sa_const_color + stage_index * 4)};
    case Source::Previous:
    default:
        return {Bank::Virtual, TmpOutput};
    }
}

void FragmentEmitter::LoadArg(uint8_t arg_index, const TevStageConfig& stage, u32 stage_index,
                              Source csrc, ColorModifier cmod, Source asrc, AlphaModifier amod,
                              uint8_t lanes) {
    // An argument is built in the internal register itself: the colour source's quad in one
    // move (or one lane of it broadcast), the alpha source's lane moved into w when it
    // differs, then one add for the inverted lanes. Assembling it in temporaries first cost
    // three copies and a load per argument.
    const Reg dst = Reg::I(arg_index);
    const Vec4Loc c =
        SourceLocation(ColorSource(stage, csrc, stage_index), stage_index, ReadsAlphaLane(cmod));
    const Vec4Loc a =
        SourceLocation(AlphaSource(stage, asrc, stage_index), stage_index, ReadsAlphaLane(amod));
    const bool want_c = lanes & 0x7;
    const bool want_a = lanes & 0x8;

    // A component of a float4 in the unified store: which pair, and which half.
    const auto channel = [](int comp) {
        return std::pair<int, Ch>{comp >> 1, (comp & 1) ? Ch::Y : Ch::X};
    };
    const auto same = [](const Vec4Loc& x, const Vec4Loc& y) {
        return x.bank == y.bank && x.base == y.base;
    };
    // dst.<mask> = one component of `from` broadcast.
    const auto lane = [&](uint8_t mask, const Vec4Loc& from, int comp) {
        if (from.bank == Bank::Const) {
            e.MoveInternal({dst, mask}, {Reg::C(0), XXXX}, Pred::None, true);
            return;
        }
        const auto [pair, ch] = channel(comp);
        e.MoveInternal({dst, mask}, {from.Pair(pair), Swizzle{ch, ch, ch, ch}}, Pred::None,
                       true);
    };

    const u32 cm = static_cast<u32>(cmod);
    const bool cinv = cm & 1;
    int broadcast = -1; // -1: rgb; otherwise the source component broadcast to rgb
    switch (cm & ~1u) {
    case 0x0: broadcast = -1; break; // SourceColor
    case 0x2: broadcast = 3; break;  // SourceAlpha
    case 0x4: broadcast = 0; break;  // SourceRed
    case 0x8: broadcast = 1; break;  // SourceGreen
    case 0xc: broadcast = 2; break;  // SourceBlue
    default: broadcast = -1; break;
    }
    const u32 am = static_cast<u32>(amod);
    const bool ainv = am & 1;
    const int acomp[4] = {3, 0, 1, 2}; // SourceAlpha, Red, Green, Blue
    const int alpha_comp = acomp[(am >> 1) & 3];

    bool alpha_done = false;
    if (want_c) {
        if (c.bank == Bank::Const) {
            e.MoveInternal({dst, 0x7}, {Reg::C(0), XXXX}, Pred::None, true);
        } else if (broadcast < 0) {
            // The whole quad; w is right already when the alpha comes from the same
            // source's own alpha.
            LoadVec4(dst, c);
            alpha_done = want_a && same(a, c) && alpha_comp == 3;
        } else {
            lane(0x7, c, broadcast);
        }
    }
    if (want_a && !alpha_done) {
        lane(0x8, a, alpha_comp);
    }
    const uint8_t inv = static_cast<uint8_t>((want_c && cinv ? 0x7 : 0) | (want_a && ainv ? 0x8 : 0));
    if (inv != 0) {
        e.Vec(VecOp::Add, {dst, inv}, {dst, XYZW, Mod::Neg}, ConstOne, Pred::None, true);
    }
}

void FragmentEmitter::ColorOp(Operation op, uint8_t mask) {
    // Arguments 1 and 2 are in i0 and i1, argument 3 (when the op has one) in i2 before
    // this is called; the result lands in i2 under `mask`.
    switch (op) {
    case Operation::Replace:
        e.MoveInternal({I2, mask}, {I0, XYZW}, Pred::None, true);
        break;
    case Operation::Modulate:
        e.Vec(VecOp::Mul, {I2, mask}, {I0, XYZW}, {I1, XYZW}, Pred::None, true);
        break;
    case Operation::Add:
        e.Vec(VecOp::Add, {I2, mask}, {I0, XYZW}, {I1, XYZW}, Pred::None, true);
        break;
    case Operation::AddSigned:
        e.Vec(VecOp::Add, {I2, mask}, {I0, XYZW}, {I1, XYZW}, Pred::None, true);
        e.Vec(VecOp::Add, {I2, mask}, {Reg::C(12), XXXX, Mod::Neg}, {I2, XYZW}, Pred::None,
              true);
        break;
    case Operation::Subtract:
        e.Vec(VecOp::Add, {I2, mask}, {I1, XYZW, Mod::Neg}, {I0, XYZW}, Pred::None, true);
        break;
    case Operation::Lerp:
        // lerp(arg2, arg1, arg3) = arg2 + arg3 * (arg1 - arg2), arg3 waiting in i2.
        e.Vec(VecOp::Add, {I0, mask}, {I1, XYZW, Mod::Neg}, {I0, XYZW}, Pred::None, true);
        e.Mad({I2, mask}, {I0, XYZW}, {I2, XYZW}, {I1, XYZW}, true, Pred::None, true);
        break;
    case Operation::MultiplyThenAdd:
        e.Mad({I2, mask}, {I0, XYZW}, {I1, XYZW}, {I2, XYZW}, true, Pred::None, true);
        break;
    case Operation::AddThenMultiply:
        e.Vec(VecOp::Add, {I0, mask}, {I0, XYZW}, {I1, XYZW}, Pred::None, true);
        e.Vec(VecOp::Min, {I0, mask}, {I0, XYZW}, ConstOne, Pred::None, true);
        e.Vec(VecOp::Mul, {I2, mask}, {I0, XYZW}, {I2, XYZW}, Pred::None, true);
        break;
    case Operation::Dot3_RGB:
    case Operation::Dot3_RGBA:
        e.Vec(VecOp::Add, {I0, 0x7}, {Reg::C(12), XXXX, Mod::Neg}, {I0, XYZW}, Pred::None,
              true);
        e.Vec(VecOp::Add, {I1, 0x7}, {Reg::C(12), XXXX, Mod::Neg}, {I1, XYZW}, Pred::None,
              true);
        e.Dot({I2, 0x1}, {I0, XYZW}, {I1, XYZW}, false, Pred::None, true);
        e.Vec(VecOp::Mul, {I2, mask}, {I2, XXXX}, Lit(LitFour), Pred::None, true);
        break;
    default:
        e.MoveInternal({I2, mask}, {Reg::C(0), XXXX}, Pred::None, true);
        break;
    }
}

void FragmentEmitter::TevStage(u32 index) {
    const TevStageConfig stage = config.texture.tev_stages[index];
    if (!IsPassThroughTevStage(stage)) {
        const bool dot3_alpha = stage.color_op == Operation::Dot3_RGBA;
        const bool same_op = stage.color_op == stage.alpha_op && !dot3_alpha &&
                             stage.color_op != Operation::Dot3_RGB;
        const auto needs_three = [](Operation op) {
            return op == Operation::Lerp || op == Operation::MultiplyThenAdd ||
                   op == Operation::AddThenMultiply;
        };
        // Whether the colour operation leaves i0 and i1 as loaded (only i2 written), so
        // the alpha operation can use their w lanes without loading them again.
        const auto keeps_args = [](Operation op) {
            return op == Operation::Replace || op == Operation::Modulate ||
                   op == Operation::Add || op == Operation::AddSigned ||
                   op == Operation::Subtract || op == Operation::MultiplyThenAdd;
        };
        const bool separate_alpha = !same_op && !dot3_alpha;
        const bool alpha_three = separate_alpha && needs_three(stage.alpha_op);
        // Arguments 1 and 2 carry their alpha from the start when the colour operation
        // keeps them; argument 3's alpha only when the alpha operation is the same.
        const bool alpha_early = !separate_alpha || (keeps_args(stage.color_op) && !alpha_three);
        const uint8_t lanes01 = alpha_early ? 0xf : 0x7;
        const auto load = [&](uint8_t arg, uint8_t lanes) {
            switch (arg) {
            case 0:
                LoadArg(0, stage, index, stage.color_source1, stage.color_modifier1,
                        stage.alpha_source1, stage.alpha_modifier1, lanes);
                break;
            case 1:
                LoadArg(1, stage, index, stage.color_source2, stage.color_modifier2,
                        stage.alpha_source2, stage.alpha_modifier2, lanes);
                break;
            default:
                LoadArg(2, stage, index, stage.color_source3, stage.color_modifier3,
                        stage.alpha_source3, stage.alpha_modifier3, lanes);
                break;
            }
        };
        load(0, lanes01);
        load(1, lanes01);
        if (needs_three(stage.color_op)) {
            load(2, same_op ? 0xf : 0x7);
        }
        ColorOp(stage.color_op, same_op || dot3_alpha ? 0xf : 0x7);
        if (separate_alpha) {
            if (alpha_three) {
                // i2's colour result must survive: park it, load the alphas, restore. Not in
                // TmpOutput, which an alpha argument's Previous with a colour channel
                // (SourceRed..Blue) still reads. Its w is written, as every lane read back is: the
                // alpha replaces it below.
                e.Vec(VecOp::Add, {Reg::V(TmpParked + 2), 0x2}, ConstZero, ConstZero);
                StoreVec4({Bank::Virtual, TmpParked}, I2, 0x7);
                load(0, 0x8);
                load(1, 0x8);
                load(2, 0x8);
                ColorOp(stage.alpha_op, 0x8);
                e.MoveInternal({I0, 0xf}, {I2, XYZW}, Pred::None, true);
                LoadVec4(I2, {Bank::Virtual, TmpParked});
                e.MoveInternal({I2, 0x8}, {I0, XYZW}, Pred::None, true);
            } else {
                if (!alpha_early) {
                    load(0, 0x8);
                    load(1, 0x8);
                }
                ColorOp(stage.alpha_op, 0x8);
            }
        }
        Clamp01(I2);
        Byteround(I2, I0);
        const unsigned cscale = stage.GetColorMultiplier();
        const unsigned ascale = stage.GetAlphaMultiplier();
        if (cscale != 1) {
            e.Vec(VecOp::Mul, {I2, 0x7}, {I2, XYZW}, Lit(cscale == 2 ? LitTwo : LitFour),
                  Pred::None, true);
        }
        if (ascale != 1) {
            e.Vec(VecOp::Mul, {I2, 0x8}, {I2, XYZW}, Lit(ascale == 2 ? LitTwo : LitFour),
                  Pred::None, true);
        }
        if (cscale != 1 || ascale != 1) {
            Clamp01(I2);
        }
        StoreVec4({Bank::Virtual, TmpOutput}, I2);
    }
    if (snapshot_lanes[index] != 0) {
        // A later stage reads this output through PreviousBuffer.
        CopyVec4({Bank::Virtual, static_cast<uint8_t>(TmpSnap + index * 4)},
                 {Bank::Virtual, TmpOutput}, snapshot_lanes[index]);
    }
}

template <typename F>
void FragmentEmitter::ForEachU8Arg(F&& f) const {
    const auto three = [](Operation op) {
        return op == Operation::Lerp || op == Operation::MultiplyThenAdd ||
               op == Operation::AddThenMultiply;
    };
    for (u32 k = 0; k < config.texture.tev_stages.size(); k++) {
        const TevStageConfig stage = config.texture.tev_stages[k];
        if (IsPassThroughTevStage(stage)) {
            continue;
        }
        const bool two =
            stage.color_op != Operation::Replace || stage.alpha_op != Operation::Replace;
        const bool need3 = three(stage.color_op) || three(stage.alpha_op);
        const Source csrcs[3] = {stage.color_source1, stage.color_source2, stage.color_source3};
        const Source asrcs[3] = {stage.alpha_source1, stage.alpha_source2, stage.alpha_source3};
        const ColorModifier cmods[3] = {stage.color_modifier1, stage.color_modifier2,
                                        stage.color_modifier3};
        const AlphaModifier amods[3] = {stage.alpha_modifier1, stage.alpha_modifier2,
                                        stage.alpha_modifier3};
        for (int a = 0; a < 3; a++) {
            if ((a == 1 && !two) || (a == 2 && !need3)) {
                continue;
            }
            f(k, ColorSource(stage, csrcs[a], k), cmods[a], AlphaSource(stage, asrcs[a], k),
              amods[a]);
        }
    }
}

bool FragmentEmitter::U8TevSupported() const {
    // Every operation but Dot3. The modifiers: the colour itself, its alpha, or either
    // complemented; the alpha itself or complemented; and a colour channel (SourceRed, Green,
    // Blue) of an input U8Inputs packs, which the pack moves into every byte (U8Broadcast).
    const auto op_ok = [](Operation op) {
        switch (op) {
        case Operation::Replace:
        case Operation::Modulate:
        case Operation::Add:
        case Operation::AddSigned:
        case Operation::Lerp:
        case Operation::Subtract:
        case Operation::MultiplyThenAdd:
        case Operation::AddThenMultiply:
            return true;
        default:
            return false;
        }
    };
    // Dot3 too, in F32 between unpacks (U8Dot3); a Dot3_RGBA stage's alpha operation is not
    // read.
    for (u32 k = 0; k < config.texture.tev_stages.size(); k++) {
        const TevStageConfig stage = config.texture.tev_stages[k];
        const bool dot3 =
            stage.color_op == Operation::Dot3_RGB || stage.color_op == Operation::Dot3_RGBA;
        if (!IsPassThroughTevStage(stage) &&
            ((!op_ok(stage.color_op) && !dot3) ||
             (!op_ok(stage.alpha_op) && stage.color_op != Operation::Dot3_RGBA))) {
            return false;
        }
    }
    bool ok = true;
    uint32_t moved = 0; // bit 3 * input + lane
    ForEachU8Arg([&](u32, Source csrc, ColorModifier cmod, Source asrc, AlphaModifier amod) {
        const u32 cm = static_cast<u32>(cmod);
        const bool cm_known = cm <= 5 || cm == 8 || cm == 9 || cm == 12 || cm == 13;
        if (!cm_known || static_cast<u32>(amod) > 7) {
            ok = false;
            return;
        }
        const auto note = [&](Source src, int lane) {
            if (lane < 0) {
                return;
            }
            const int input = BroadcastInput(src);
            // A stage's output, a constant or an own-pixel texel (which has no lanes to pack
            // from) is broadcast where it is read (U8LaneBroadcast).
            const bool own = src >= Source::Texture0 && src <= Source::Texture2 &&
                             (config.texture.own_pixel >>
                                  (static_cast<u32>(src) - static_cast<u32>(Source::Texture0)) &
                              1);
            if (input < 0 || own) {
                return;
            }
            moved |= 1u << (3 * input + lane);
        };
        note(csrc, MovedLane(cmod));
        note(asrc, MovedLane(amod));
    });
    return ok && static_cast<uint32_t>(std::popcount(moved)) <= U8BroadcastMax;
}

bool FragmentEmitter::StageReads(Source source) const {
    for (u32 k = 0; k < config.texture.tev_stages.size(); k++) {
        const TevStageConfig stage = config.texture.tev_stages[k];
        if (IsPassThroughTevStage(stage)) {
            continue;
        }
        for (const Source s : {stage.color_source1.Value(), stage.color_source2.Value(),
                               stage.color_source3.Value()}) {
            if (ColorSource(stage, s, k) == source) {
                return true;
            }
        }
        for (const Source s : {stage.alpha_source1.Value(), stage.alpha_source2.Value(),
                               stage.alpha_source3.Value()}) {
            if (AlphaSource(stage, s, k) == source) {
                return true;
            }
        }
    }
    return false;
}

void FragmentEmitter::U8Inputs() {
    // White and AddSigned's half from their literal words through a VBW, so no SOP reads a
    // secondary attribute (the colour format control's C10 reading of a high register number
    // is not measured); zero from white.
    e.Bitwise(BwOp::Or, Reg::V(TmpU8White),
              IOp::R(Reg::Sa(static_cast<uint8_t>(LitSa(LitU8White)))), IOp::Imm(0));
    e.Bitwise(BwOp::And, Reg::V(TmpU8Zero), IOp::R(Reg::V(TmpU8White)), IOp::Imm(0));
    bool half = false;
    for (u32 k = 0; k < config.texture.tev_stages.size(); k++) {
        const TevStageConfig stage = config.texture.tev_stages[k];
        half = half || (!IsPassThroughTevStage(stage) && (stage.color_op == Operation::AddSigned ||
                                                          stage.alpha_op == Operation::AddSigned));
    }
    if (half) {
        e.Bitwise(BwOp::Or, Reg::V(TmpU8Half),
                  IOp::R(Reg::Sa(static_cast<uint8_t>(LitSa(LitU8Half)))), IOp::Imm(0));
    }
    // The inputs, packed to nearest as the F32 path's byte rounding has them (the vertex
    // colour); the texels and the lighting's colours are rounded here, where the F32 path
    // carried them unrounded into the first stage reading them.
    if (StageReads(Source::PrimaryColor)) {
        e.PackU8(Reg::V(TmpU8Primary), Reg::Pa(0), Reg::Pa(2));
    }
    for (u32 unit = 0; unit < 3; unit++) {
        if (unit_own[unit]) {
            // The tile's colour as the program finds it: the pixel's own texel.
            SopCtl s;
            s.cmod1 = s.amod1 = true; // ONE * src1 + ZERO
            e.Sop2(Reg::V(static_cast<uint8_t>(TmpU8Tex + unit)), Reg::O(0), Reg::O(0), s);
            continue;
        }
        // A unit only the lighting reads (the shadow, a bump map) has no stage to pack for.
        if (!unit_used[unit] ||
            !StageReads(static_cast<Source>(static_cast<u32>(Source::Texture0) + unit))) {
            continue;
        }
        const Vec4Loc texel = texel_loc[unit];
        const Reg dst = Reg::V(static_cast<uint8_t>(TmpU8Tex + unit));
        if (texel.f16) {
            e.PackU8F16(dst, texel.Pair(0));
        } else {
            e.PackU8(dst, texel.Pair(0), texel.Pair(1));
        }
        if (texel.f16 && AlphaIssued(unit)) {
            // The split alpha, packed too, then ONE * the colour + ONE * the alpha's alpha.
            e.PackU8F16(Reg::V(TmpU8Scratch), alpha_loc[unit].Pair(0));
            SopCtl s;
            s.cmod1 = true;
            s.amod2 = true;
            e.Sop2(dst, dst, Reg::V(TmpU8Scratch), s);
        }
    }
    if (uses_proctex) {
        if (proctex_tabled) {
            e.PackU8F16(Reg::V(TmpU8Tex3), proctex_loc.Pair(0));
        } else {
            e.PackU8(Reg::V(TmpU8Tex3), Reg::V(TmpTex3), Reg::V(TmpTex3 + 2));
        }
    }
    if (uses_lighting && !u8_lighting_packed) {
        e.PackU8(Reg::V(TmpU8Diffuse), Reg::V(TmpDiffuse), Reg::V(TmpDiffuse + 2));
        e.PackU8(Reg::V(TmpU8Specular), Reg::V(TmpSpecular), Reg::V(TmpSpecular + 2));
    }
    // A channel a modifier moves (alpha = primary.r, colour = texel.ggg): the input packed
    // again from the same lanes, that channel into every byte, so the argument reads it as the
    // colour or the alpha itself.
    u8_bcast = {};
    uint32_t next = 0;
    const auto broadcast = [&](Source src, int lane) {
        if (lane < 0 || !U8InputPresent(src)) {
            return;
        }
        const int input = BroadcastInput(src);
        if (input < 0) {
            throw std::runtime_error("usse: a moved U8 channel U8TevSupported did not admit");
        }
        if (u8_bcast[input][lane] != 0) {
            return;
        }
        if (next >= U8BroadcastMax) {
            throw std::runtime_error("usse: more moved U8 channels than U8TevSupported admits");
        }
        const Reg dst = Reg::V(static_cast<uint8_t>(TmpU8Broadcast + next++));
        u8_bcast[input][lane] = dst.n;
        const uint8_t sel = PackSelBroadcast(static_cast<uint8_t>(lane));
        switch (src) {
        case Source::PrimaryColor:
            e.PackU8(dst, Reg::Pa(0), Reg::Pa(2), sel);
            break;
        case Source::PrimaryFragmentColor:
            e.PackU8(dst, Reg::V(TmpDiffuse), Reg::V(TmpDiffuse + 2), sel);
            break;
        case Source::SecondaryFragmentColor:
            e.PackU8(dst, Reg::V(TmpSpecular), Reg::V(TmpSpecular + 2), sel);
            break;
        case Source::Texture3:
            if (proctex_tabled) {
                e.PackU8F16(dst, proctex_loc.Pair(0), sel);
            } else {
                e.PackU8(dst, Reg::V(TmpTex3), Reg::V(TmpTex3 + 2), sel);
            }
            break;
        default: {
            // The colour texel's lanes: a split alpha is not a colour channel.
            const Vec4Loc texel = texel_loc[input - BroadcastInput(Source::Texture0)];
            if (texel.f16) {
                e.PackU8F16(dst, texel.Pair(0), sel);
            } else {
                e.PackU8(dst, texel.Pair(0), texel.Pair(1), sel);
            }
            break;
        }
        }
    };
    ForEachU8Arg([&](u32, Source csrc, ColorModifier cmod, Source asrc, AlphaModifier amod) {
        broadcast(csrc, MovedLane(cmod));
        broadcast(asrc, MovedLane(amod));
    });
}

Reg FragmentEmitter::U8EvenFinal() {
    if (!(u8_prev.n & 1)) {
        return u8_prev;
    }
    // No stage wrote it (zero): an even register for the unpacks.
    SopCtl s;
    s.cmod1 = s.amod1 = true;
    e.Sop2(Reg::V(TmpU8Final), u8_prev, u8_prev, s);
    return Reg::V(TmpU8Final);
}

bool FragmentEmitter::U8InputPresent(Source source) const {
    switch (source) {
    case Source::PrimaryColor:
        return true;
    case Source::PrimaryFragmentColor:
    case Source::SecondaryFragmentColor:
        return uses_lighting;
    case Source::Texture0:
    case Source::Texture1:
    case Source::Texture2:
        return unit_used[static_cast<u32>(source) - static_cast<u32>(Source::Texture0)];
    case Source::Texture3:
        return uses_proctex;
    default:
        return false;
    }
}

Reg FragmentEmitter::U8Broadcast(Source source, int lane) const {
    if (!U8InputPresent(source)) {
        return Reg::V(TmpU8Zero);
    }
    const uint8_t n = u8_bcast[BroadcastInput(source)][lane];
    if (n == 0) {
        throw std::runtime_error("usse: a moved U8 channel U8Inputs did not pack");
    }
    return Reg::V(n);
}

Reg FragmentEmitter::U8Source(Source source, u32 stage_index, bool alpha) const {
    switch (source) {
    case Source::PrimaryColor:
        return colour_u8_pa >= 0 ? Reg::Pa(static_cast<uint8_t>(colour_u8_pa))
                                 : Reg::V(TmpU8Primary);
    case Source::PrimaryFragmentColor:
        return Reg::V(uses_lighting ? TmpU8Diffuse : TmpU8Zero);
    case Source::SecondaryFragmentColor:
        return Reg::V(uses_lighting ? TmpU8Specular : TmpU8Zero);
    case Source::Texture0:
    case Source::Texture1:
    case Source::Texture2: {
        const u32 unit = static_cast<u32>(source) - static_cast<u32>(Source::Texture0);
        return unit_used[unit] || unit_own[unit] ? Reg::V(static_cast<uint8_t>(TmpU8Tex + unit))
                                                 : Reg::V(TmpU8Zero);
    }
    case Source::Texture3:
        return Reg::V(uses_proctex ? TmpU8Tex3 : TmpU8Zero);
    case Source::PreviousBuffer: {
        const int from = alpha ? buffer_alpha_from[stage_index] : buffer_color_from[stage_index];
        if (from == -2) {
            return Reg::V(TmpU8Zero);
        }
        if (from == -1) {
            return Reg::Sa(static_cast<uint8_t>(sa_buffer_color));
        }
        return u8_out[from];
    }
    case Source::Constant:
        return Reg::Sa(static_cast<uint8_t>(sa_const_color + stage_index));
    case Source::Previous:
    default:
        return u8_prev;
    }
}

Reg FragmentEmitter::U8Arg(uint8_t k, u32 stage_index, Source csrc, ColorModifier cmod, Source asrc,
                           AlphaModifier amod, bool need_colour, bool need_alpha) {
    const TevStageConfig stage = config.texture.tev_stages[stage_index];
    const Source cs = ColorSource(stage, csrc, stage_index);
    const Source as = AlphaSource(stage, asrc, stage_index);
    // A moved channel reads the input's broadcast as its colour or alpha: the modifier keeps
    // only its complement.
    const int clane = MovedLane(cmod);
    const int alane = MovedLane(amod);
    // Lanes no operation reads take the other's register as it is (an unread SA alpha takes
    // white's, which a SOP3's source 0 can name without a copy).
    Reg c = Reg::V(TmpU8White), a = Reg::V(TmpU8White);
    u32 cm = 0, am = 0;
    // A moved channel U8Inputs did not broadcast: made here from the source's colour.
    const auto lazy = [&](Source src) {
        return BroadcastInput(src) < 0 ||
               (src >= Source::Texture0 && src <= Source::Texture2 &&
                unit_own[static_cast<u32>(src) - static_cast<u32>(Source::Texture0)]);
    };
    if (need_colour) {
        c = clane < 0  ? U8Source(cs, stage_index, ReadsAlphaLane(cmod))
            : lazy(cs) ? U8LaneBroadcast(U8Source(cs, stage_index, false), clane,
                                         Reg::V(static_cast<uint8_t>(TmpU8LaneColour + k)))
                       : U8Broadcast(cs, clane);
        cm = clane >= 0 ? static_cast<u32>(cmod) & 1 : static_cast<u32>(cmod);
    }
    if (need_alpha) {
        a = alane < 0  ? U8Source(as, stage_index, true)
            : lazy(as) ? U8LaneBroadcast(U8Source(as, stage_index, false), alane,
                                         Reg::V(static_cast<uint8_t>(TmpU8LaneAlpha + k)))
                       : U8Broadcast(as, alane);
        am = alane >= 0 ? static_cast<u32>(amod) & 1 : static_cast<u32>(amod);
    }
    if (!need_colour) {
        c = a;
    }
    if (!need_alpha) {
        a = c.bank == Bank::SecAttr ? Reg::V(TmpU8White) : c;
    }
    if (cm == 0 && am == 0 && (!need_alpha || (c.bank == a.bank && c.n == a.n))) {
        return c;
    }
    // One SOP3 over white: colour = the colour source's colour or alpha, complemented or not,
    // times one; alpha = the alpha source's alpha (source 0), complemented or not.
    SopCtl s;
    s.csel1 = (cm & 2) ? 7 : 6; // src2 alpha : src2
    s.cmod1 = (cm & 1) != 0;
    s.asel1 = 1; // src0 alpha
    s.amod1 = (am & 1) != 0;
    const Reg dst = Reg::V(static_cast<uint8_t>(TmpU8Arg + k));
    e.Sop3(dst, U8Src0(a), Reg::V(TmpU8White), c, s);
    return dst;
}

Reg FragmentEmitter::U8Even(Reg r, uint8_t even) {
    if (!(r.n & 1) && (r.bank == Bank::Virtual || r.bank == Bank::SecAttr)) {
        return r;
    }
    if (r.bank == Bank::SecAttr) {
        e.Bitwise(BwOp::Or, Reg::V(even), IOp::R(r), IOp::Imm(0));
    } else {
        SopCtl s;
        s.cmod1 = s.amod1 = true; // ONE * src1 + ZERO
        e.Sop2(Reg::V(even), r, r, s);
    }
    return Reg::V(even);
}

Reg FragmentEmitter::U8LaneBroadcast(Reg src, int lane, Reg dst) {
    e.UnpackU8({I0, 0xf}, U8Even(src, TmpU8EvenA));
    e.PackU8(dst, I0, I0, PackSelBroadcast(static_cast<uint8_t>(lane)));
    return dst;
}

void FragmentEmitter::U8Dot3(Reg dst, Reg a, Reg b) {
    // As the F32 path's ColorOp, from the bytes as F32 (k / 255), packed back saturated and to
    // nearest: the F32 path's clamp and floor(x * 255 + 0.5), x * 255 being a sum of products
    // over 255 (odd), never a half.
    e.UnpackU8({I0, 0x7}, U8Even(a, TmpU8EvenA));
    e.UnpackU8({I1, 0x7}, U8Even(b, TmpU8EvenB));
    e.Vec(VecOp::Add, {I0, 0x7}, {Reg::C(12), XXXX, Mod::Neg}, {I0, XYZW}, Pred::None, true);
    e.Vec(VecOp::Add, {I1, 0x7}, {Reg::C(12), XXXX, Mod::Neg}, {I1, XYZW}, Pred::None, true);
    e.Dot({I2, 0x1}, {I0, XYZW}, {I1, XYZW}, false, Pred::None, true);
    // Every lane (the pack reads all four).
    e.Vec(VecOp::Mul, {I2, 0xf}, {I2, XXXX}, Lit(LitFour), Pred::None, true);
    e.PackU8(dst, I2, I2);
}

int FragmentEmitter::Dot3ConstArg(const TevStageConfig& stage, u32 index) {
    if (stage.color_op != Operation::Dot3_RGB && stage.color_op != Operation::Dot3_RGBA) {
        return -1;
    }
    const auto plain = [&](Source source, ColorModifier modifier) {
        return ColorSource(stage, source, index) == Source::Constant &&
               modifier == ColorModifier::SourceColor;
    };
    return plain(stage.color_source2, stage.color_modifier2)   ? 1
           : plain(stage.color_source1, stage.color_modifier1) ? 0
                                                               : -1;
}

void FragmentEmitter::U8Dot3Const(Reg dst, Reg a, uint32_t sa) {
    // (a - 0.5) . (b - 0.5) * 4 = a . 4 (b - 0.5) - 2 sum(b - 0.5), the constant's two terms
    // made once a draw. The sums differ from U8Dot3's by a few ulps and the pack's steps are
    // 1 / 510 from every value (U8Dot3): the same bytes.
    const Reg k = Reg::Sa(static_cast<uint8_t>(sa));
    e.UnpackU8({I0, 0x7}, U8Even(a, TmpU8EvenA));
    e.Dot({I2, 0x1}, {k, XYZW}, {I0, XYZW}, false, Pred::None, true);
    e.Vec(VecOp::Add, {I2, 0xf}, {I2, XXXX}, SaScalar(sa + 3), Pred::None, true);
    e.PackU8(dst, I2, I2);
}

Reg FragmentEmitter::U8Src0(Reg r) {
    if (r.bank != Bank::SecAttr) {
        return r;
    }
    e.Bitwise(BwOp::Or, Reg::V(TmpU8Const), IOp::R(r), IOp::Imm(0));
    return Reg::V(TmpU8Const);
}

void FragmentEmitter::U8Op(Operation op, Reg dst, const std::array<Reg, 3>& args) {
    const Reg a = args[0], b = args[1], c = args[2];
    const auto one_plus_one = [](uint8_t cop) {
        SopCtl s;
        s.cmod1 = s.cmod2 = s.amod1 = s.amod2 = true; // ONE * src1 op ONE * src2
        s.cop = s.aop = cop;
        return s;
    };
    const Reg scratch = Reg::V(TmpU8Scratch);
    switch (op) {
    case Operation::Replace: {
        SopCtl s;
        s.cmod1 = s.amod1 = true; // ONE * src1 + ZERO
        e.Sop2(dst, a, a, s);
        break;
    }
    case Operation::Modulate: {
        SopCtl s;
        s.csel1 = 2; // src2 * src1
        s.asel1 = 2;
        e.Sop2(dst, a, b, s);
        break;
    }
    case Operation::Add:
        e.Sop2(dst, a, b, one_plus_one(0));
        break;
    case Operation::Subtract:
        e.Sop2(dst, a, b, one_plus_one(1));
        break;
    case Operation::AddSigned: {
        // a + b - 127 (the F32 path's 0.5, rounded), clamped: p = max(a - h, 0), q = max(h - a,
        // 0), one of them zero; the result is max(min(p + b, 255) - q, 0).
        const Reg h = Reg::V(TmpU8Half);
        e.Sop2(scratch, a, h, one_plus_one(1));
        e.Sop2(dst, h, a, one_plus_one(1));
        e.Sop2(scratch, scratch, b, one_plus_one(0));
        e.Sop2(dst, scratch, dst, one_plus_one(1));
        break;
    }
    case Operation::Lerp: {
        // a * c + b * (1 - c): SOP3 with c in source 0.
        SopCtl s;
        s.csel1 = 4;
        s.csel2 = 4;
        s.cmod2 = true;
        s.asel1 = 1;
        e.Sop3(dst, U8Src0(c), a, b, s);
        break;
    }
    case Operation::MultiplyThenAdd: {
        // a * b + c: b in source 0, ONE * c.
        SopCtl s;
        s.csel1 = 4;
        s.csel2 = 0;
        s.cmod2 = true;
        s.asel1 = 1;
        e.Sop3(dst, U8Src0(b), a, c, s);
        break;
    }
    case Operation::AddThenMultiply: {
        e.Sop2(scratch, a, b, one_plus_one(0));
        SopCtl s;
        s.csel1 = 2;
        s.asel1 = 2;
        e.Sop2(dst, scratch, c, s);
        break;
    }
    default:
        // U8TevSupported admits none other.
        throw std::runtime_error("usse: a U8 combiner operation without a U8 form");
    }
}

void FragmentEmitter::U8TevStage(u32 index) {
    const TevStageConfig stage = config.texture.tev_stages[index];
    if (IsPassThroughTevStage(stage)) {
        u8_out[index] = u8_prev;
        return;
    }
    const auto three = [](Operation op) {
        return op == Operation::Lerp || op == Operation::MultiplyThenAdd ||
               op == Operation::AddThenMultiply;
    };
    const Operation color_op = stage.color_op;
    const bool two = color_op != Operation::Replace || stage.alpha_op != Operation::Replace;
    const bool need3 = three(color_op) || three(stage.alpha_op);
    const auto arity = [&](Operation op) {
        return op == Operation::Replace ? 1u : three(op) ? 3u : 2u;
    };
    const bool dot3 =
        stage.color_op == Operation::Dot3_RGB || stage.color_op == Operation::Dot3_RGBA;
    const u32 colour_args = arity(color_op);
    // Dot3_RGBA writes its alpha too: the alpha operation reads nothing.
    const u32 alpha_args = stage.color_op == Operation::Dot3_RGBA ? 0u : arity(stage.alpha_op);
    const Reg out = Reg::V(static_cast<uint8_t>(TmpU8Stage + index * 2));
    const unsigned cscale = stage.GetColorMultiplier();
    const unsigned ascale = stage.GetAlphaMultiplier();
    // An operation SOP2 makes in one over (args[0], args[1]): its colour and alpha factors.
    const auto single = [](Operation op) {
        return op == Operation::Replace || op == Operation::Modulate || op == Operation::Add ||
               op == Operation::Subtract;
    };
    const auto colour_factors = [&](SopCtl& c) {
        // ONE * src1 (+ ZERO), src2 * src1, ONE * src1 + ONE * src2 or - ONE * src2, as U8Op
        // makes each.
        switch (color_op) {
        case Operation::Replace:
            c.cmod1 = true;
            break;
        case Operation::Modulate:
            c.csel1 = 2;
            break;
        default:
            c.cmod1 = c.cmod2 = true;
            c.cop = color_op == Operation::Subtract ? 1 : 0;
            break;
        }
    };
    // Both operations one SOP2 straight over plain registers (no modifier, no moved channel),
    // no argument built: the colour's arguments are src1 (and src2), the alpha's found among
    // them, or put in src2 when the colour reads one argument.
    const auto direct = [&]() {
        const Source csrc[2] = {stage.color_source1, stage.color_source2};
        const ColorModifier cmods[2] = {stage.color_modifier1, stage.color_modifier2};
        const Source asrc[2] = {stage.alpha_source1, stage.alpha_source2};
        const AlphaModifier amods[2] = {stage.alpha_modifier1, stage.alpha_modifier2};
        std::array<Reg, 2> c{Reg::V(TmpU8Zero), Reg::V(TmpU8Zero)};
        std::array<Reg, 2> a{Reg::V(TmpU8Zero), Reg::V(TmpU8Zero)};
        for (u32 k = 0; k < colour_args; k++) {
            if (cmods[k] != ColorModifier::SourceColor) {
                return false;
            }
            c[k] = U8Source(ColorSource(stage, csrc[k], index), index, false);
        }
        for (u32 k = 0; k < alpha_args; k++) {
            if (amods[k] != AlphaModifier::SourceAlpha) {
                return false;
            }
            a[k] = U8Source(AlphaSource(stage, asrc[k], index), index, true);
        }
        const auto same = [](Reg x, Reg y) { return x.bank == y.bank && x.n == y.n; };
        // A Replace of one register for both: the copy-free form below.
        if (color_op == Operation::Replace && stage.alpha_op == Operation::Replace &&
            same(c[0], a[0])) {
            return false;
        }
        const bool free2 = colour_args < 2;
        const Reg s1 = c[0];
        Reg s2 = c[1];
        SopCtl f;
        colour_factors(f);
        switch (stage.alpha_op) {
        case Operation::Replace:
            if (same(a[0], s1)) {
                f.amod1 = true; // ONE * src1 alpha
            } else if (free2 || same(a[0], s2)) {
                s2 = a[0];
                f.amod2 = true; // ZERO * src1 alpha + ONE * src2 alpha
            } else {
                return false;
            }
            break;
        case Operation::Modulate:
            if (same(a[0], s1) && (free2 || same(a[1], s2))) {
                s2 = a[1];
            } else if (same(a[1], s1) && (free2 || same(a[0], s2))) {
                s2 = a[0];
            } else {
                return false;
            }
            f.asel1 = 2; // src2 alpha * src1 alpha
            break;
        default:
            if (same(a[0], s1) && (free2 || same(a[1], s2))) {
                s2 = a[1];
            } else if (stage.alpha_op == Operation::Add && same(a[1], s1) &&
                       (free2 || same(a[0], s2))) {
                s2 = a[0];
            } else {
                return false;
            }
            f.amod1 = f.amod2 = true;
            f.aop = stage.alpha_op == Operation::Subtract ? 1 : 0;
            break;
        }
        e.Sop2(out, s1, s2, f);
        return true;
    };
    if (dot3 || !single(color_op) || !single(stage.alpha_op) || !direct()) {
        std::array<Reg, 3> args{Reg::V(TmpU8Zero), Reg::V(TmpU8Zero), Reg::V(TmpU8Zero)};
        args[0] = U8Arg(0, index, stage.color_source1, stage.color_modifier1, stage.alpha_source1,
                        stage.alpha_modifier1, true, alpha_args > 0);
        if (two) {
            args[1] =
                U8Arg(1, index, stage.color_source2, stage.color_modifier2, stage.alpha_source2,
                      stage.alpha_modifier2, colour_args > 1, alpha_args > 1);
        }
        if (need3) {
            args[2] =
                U8Arg(2, index, stage.color_source3, stage.color_modifier3, stage.alpha_source3,
                      stage.alpha_modifier3, colour_args > 2, alpha_args > 2);
        }
        // A Replace of a register no later stage writes (not an argument's or a source 0 copy's,
        // the constant's uniform included) is that register: no copy.
        const auto stable = [](const Reg& r) {
            return r.bank != Bank::Virtual ||
                   (!(r.n >= TmpU8Arg && r.n < TmpU8Arg + 3) && r.n != TmpU8Const);
        };
        if (color_op == Operation::Replace && stage.alpha_op == Operation::Replace && cscale == 1 &&
            ascale == 1 && stable(args[0])) {
            u8_out[index] = args[0];
            u8_prev = args[0];
            return;
        }
        if (dot3) {
            const int constant = Dot3ConstArg(stage, index);
            const auto dot = [&](Reg dst) {
                if (constant >= 0) {
                    U8Dot3Const(dst, args[1 - constant], sa_dot3_const[index]);
                } else {
                    U8Dot3(dst, args[0], args[1]);
                }
            };
            if (stage.color_op == Operation::Dot3_RGBA) {
                dot(out);
            } else {
                dot(Reg::V(TmpU8Colour));
                U8Op(stage.alpha_op, out, args);
                SopCtl s;
                s.cmod1 = true; // ONE * the dot
                s.amod2 = true; // ONE * the alpha result's alpha
                e.Sop2(out, Reg::V(TmpU8Colour), out, s);
            }
        } else if (stage.color_op == stage.alpha_op) {
            U8Op(stage.alpha_op, out, args);
        } else if (single(stage.color_op) && single(stage.alpha_op)) {
            // Both in one SOP2, the colour's factors and the alpha's apart: ONE * src1 (+ ZERO),
            // src2 * src1, ONE * src1 + ONE * src2 or - ONE * src2, as U8Op makes each.
            SopCtl s;
            colour_factors(s);
            switch (stage.alpha_op) {
            case Operation::Replace:
                s.amod1 = true;
                break;
            case Operation::Modulate:
                s.asel1 = 2;
                break;
            default:
                s.amod1 = s.amod2 = true;
                s.aop = stage.alpha_op == Operation::Subtract ? 1 : 0;
                break;
            }
            e.Sop2(out, args[0], args[1], s);
        } else {
            // Each operation over all four bytes, then the colour of one and the alpha of the
            // other.
            U8Op(stage.color_op, Reg::V(TmpU8Colour), args);
            U8Op(stage.alpha_op, out, args);
            SopCtl s;
            s.cmod1 = true; // ONE * colour result
            s.amod2 = true; // ONE * the alpha result's alpha
            e.Sop2(out, Reg::V(TmpU8Colour), out, s);
        }
    }
    // The scale (x2, x4) as saturating adds of the result to itself, as the F32 path clamps.
    for (unsigned step = 2; step <= 4; step *= 2) {
        const bool c2 = cscale >= step, a2 = ascale >= step;
        if (!c2 && !a2) {
            continue;
        }
        SopCtl s;
        s.cmod1 = s.amod1 = true;
        s.cmod2 = c2;
        s.amod2 = a2;
        e.Sop2(out, out, out, s);
    }
    u8_out[index] = out;
    u8_prev = out;
}

void FragmentEmitter::U8Output() {
    Reg final = U8EvenFinal();
    AlphaTest(&final);
    if (uses_fog) {
        // colour * f + fog colour * (1 - f) on the colour, the alpha kept: the factor with
        // alpha one, so the fog colour's alpha term is zero, and one SOP3. The factor is in 8
        // bits, where the F32 path lerped with the F32 factor: the PDS's read of the fog table
        // (f, f, f, 1), or FogFactor's packed.
        Reg factor = Reg::Pa(fog_pa);
        if (!fog_issued) {
            FogFactor();
            e.MoveInternal({I0, 0x8}, ConstOne, Pred::None, true);
            e.PackU8(Reg::V(TmpU8FogFactor), I0, I0);
            factor = Reg::V(TmpU8FogFactor);
        }
        SopCtl f;
        f.csel1 = 4; // src0 (the factor)
        f.csel2 = 4;
        f.cmod2 = true;
        f.amod1 = true; // ONE * the colour's alpha
        e.Sop3(Reg::V(TmpU8Fogged), factor, final, Reg::Sa(static_cast<uint8_t>(sa_fog_color)), f);
        final = Reg::V(TmpU8Fogged);
    }
    SopCtl s;
    s.cmod1 = s.amod1 = true; // ONE * the colour + ZERO
    e.Sop2(Reg::Pa(0), final, final, s);
    program.output_u8 = true;
}

void FragmentEmitter::AlphaTest(Reg* u8) {
    const auto func = config.framebuffer.alpha_test_func.Value();
    if (func == FramebufferRegs::CompareFunc::Always) {
        return;
    }
    if (func == FramebufferRegs::CompareFunc::Never) {
        e.Test(Cond::Gt, 1, ConstZero, 0, ConstZero, false);
        if (uses_depth) {
            WriteDepth();
        }
        e.KillUnlessP1();
        NewPhaseAfterKill();
        return;
    }
    // floor(alpha * 255) against the reference, kept when the PICA function holds: from the
    // F32 output, or the U8 output's alpha byte as an integer.
    Src alpha{I0};
    if (u8) {
        e.UnpackInt(Reg::V(TmpU8Scratch2), *u8, 3, 0);
        alpha = {Reg::V(TmpU8Scratch2)};
    } else {
        e.Vec(VecOp::Mul, {I0, 0x1}, {Reg::V(TmpOutput + 2), YYYY}, Lit(LitTwoFiftyFive),
              Pred::None, true);
        e.Vec(VecOp::Frc, {I1, 0x1}, {I0, XYZW}, {I0, XYZW}, Pred::None, true);
        e.Vec(VecOp::Add, {I0, 0x1}, {I1, XYZW, Mod::Neg}, {I0, XYZW}, Pred::None, true);
    }
    Cond cond = Cond::Ge;
    switch (func) {
    case FramebufferRegs::CompareFunc::Equal: cond = Cond::Eq; break;
    case FramebufferRegs::CompareFunc::NotEqual: cond = Cond::Ne; break;
    case FramebufferRegs::CompareFunc::LessThan: cond = Cond::Lt; break;
    case FramebufferRegs::CompareFunc::LessThanOrEqual: cond = Cond::Le; break;
    case FramebufferRegs::CompareFunc::GreaterThan: cond = Cond::Gt; break;
    case FramebufferRegs::CompareFunc::GreaterThanOrEqual: cond = Cond::Ge; break;
    default: break;
    }
    e.Test(cond, 1, alpha, 0, SaScalar(sa_alpha_ref), false);
    if (fold_scissor) {
        // p1 = p1 && the scissor test kept the pixel (Scissor left its minimum in the
        // scratch PA).
        const bool include = config.framebuffer.scissor_test_mode.Value() ==
                             Pica::RasterizerRegs::ScissorMode::Include;
        e.Test(include ? Cond::Ge : Cond::Lt, 1, {Reg::Pa(scissor_pa)}, 0, ConstZero, false,
               Pred::P1);
    }
    if (config.framebuffer.alpha_test_blends) {
        // The blend leaves the target as it is under alpha 0 (RasterizerSgxd::FragmentConfig):
        // the fragments the tests fail get it instead of a kill. The U8 word goes whole (its
        // colour is multiplied by zero too).
        if (u8) {
            // A secondary attribute (a copy-free Replace of the constant or the buffer colour)
            // is the draw's, not the pixel's: zeroed it would stay zero for the pixels after.
            // The output goes to an even temporary of its own first.
            if (u8->bank == Bank::SecAttr) {
                e.Bitwise(BwOp::Or, Reg::V(TmpU8Final), IOp::R(*u8), IOp::Imm(0));
                *u8 = Reg::V(TmpU8Final);
            }
            e.Bitwise(BwOp::And, *u8, IOp::R(*u8), IOp::Imm(0), Pred::NotP1);
        } else {
            e.Vec(VecOp::Add, {Reg::V(TmpOutput + 2), 0x2}, ConstZero, ConstZero, Pred::NotP1,
                  true);
        }
        return;
    }
    e.KillUnlessP1();
    NewPhaseAfterKill();
}

std::optional<Program> FragmentEmitter::Emit(std::string* refusal) {
    if (const char* why = Unsupported()) {
        if (refusal) {
            *refusal = why;
        }
        return {};
    }
    u8_tev = u8_combiners && U8TevSupported();
    PlanInputs();
    // ShadowCube is not built, nor a type the PICA does not define. Only a program that reads unit
    // 0 has a use for its type: a game can leave all-ones (type 7) in its texture registers under a
    // combiner of constants.
    if (const auto t0 = config.texture.texture0_type.Value();
        unit_used[0] && t0 != TextureType::Texture2D && t0 != TextureType::Projection2D &&
        t0 != TextureType::TextureCube && t0 != TextureType::Shadow2D) {
        if (refusal) {
            *refusal = "texture unit 0 type";
        }
        return {};
    }
    PlanUniforms();
    uses_kill = (config.framebuffer.alpha_test_func != FramebufferRegs::CompareFunc::Always ||
                 uses_scissor) &&
                !config.framebuffer.alpha_test_blends;
    if (uses_kill) {
        LitSa(LitKillControl); // the kill's operand pair, patched in after the code (PatchKills)
    }

    // The table; program.literals gets the pairs the code reads (Lit).
    const auto literal_table = std::to_array<uint32_t>({
        0xE000u, // the kill's control word, as every compiler program has it
        0u,
        std::bit_cast<uint32_t>(255.0f),
        std::bit_cast<uint32_t>(1.0f / 255.0f),
        std::bit_cast<uint32_t>(2.0f),
        std::bit_cast<uint32_t>(4.0f),
        std::bit_cast<uint32_t>(128.0f),
        std::bit_cast<uint32_t>(127.0f),
        std::bit_cast<uint32_t>(1.0f / 256.0f),
        std::bit_cast<uint32_t>(1.0f / static_cast<float>(Pica::Shader::LUT_LF_ROWS)),
        std::bit_cast<uint32_t>(0.5f / 256.0f),
        std::bit_cast<uint32_t>(256.0f),
        std::bit_cast<uint32_t>(255.0f),
        std::bit_cast<uint32_t>(-128.0f),
        std::bit_cast<uint32_t>(1.0f / static_cast<float>(Pica::Shader::LUT_RG_ROWS)),
        std::bit_cast<uint32_t>(static_cast<float>(config.proctex.lut_width)),
        std::bit_cast<uint32_t>(static_cast<float>(config.proctex.lut_width - 1)),
        std::bit_cast<uint32_t>(static_cast<float>(config.proctex.lut_offset0)),
        std::bit_cast<uint32_t>(static_cast<float>(config.proctex.lut_offset1)),
        std::bit_cast<uint32_t>(static_cast<float>(config.proctex.lut_offset2)),
        std::bit_cast<uint32_t>(static_cast<float>(config.proctex.lut_offset3)),
        std::bit_cast<uint32_t>(240.0f),
        std::bit_cast<uint32_t>(248.0f),
        std::bit_cast<uint32_t>(252.0f),
        std::bit_cast<uint32_t>(254.0f),
        std::bit_cast<uint32_t>(std::max(0.0f, static_cast<float>(config.proctex.lod_min))),
        std::bit_cast<uint32_t>(std::min(7.0f, static_cast<float>(config.proctex.lod_max))),
        0u,
        std::bit_cast<uint32_t>(2.0f),
        std::bit_cast<uint32_t>(-2.0f),
        std::bit_cast<uint32_t>(1.0f),
        std::bit_cast<uint32_t>(1.0f / static_cast<float>(Pica::Shader::LUT_LF_ROWS)),
        std::bit_cast<uint32_t>(0.5f / 256.0f),
        std::bit_cast<uint32_t>(0.5f / static_cast<float>(Pica::Shader::LUT_LF_ROWS)),
        std::bit_cast<uint32_t>(1.0f),
        std::bit_cast<uint32_t>(1.0f / static_cast<float>(Pica::Shader::LUT_RG_ROWS)),
        std::bit_cast<uint32_t>(0.5f / 256.0f),
        std::bit_cast<uint32_t>(0.5f / static_cast<float>(Pica::Shader::LUT_RG_ROWS)),
        std::bit_cast<uint32_t>(-127.0f),
        0u,
        std::bit_cast<uint32_t>(9.0f),
        std::bit_cast<uint32_t>(144.0f),
        std::bit_cast<uint32_t>(1.0f / 144.0f),
        0u,
        0xffffffffu,
        0x7f7f7f7fu};
    static_assert(LiteralCount == 46);
    static_assert(std::tuple_size_v<decltype(literal_table)> == LiteralCount);
    static_assert(Pica::Shader::LUT_RG_ROWS == Pica::Shader::LUT_RGBA_ROWS);
    program.uses_discard = uses_kill;
    if (uniform_floats + LiteralCount + program.samplers.size() * 4 > 128) {
        if (refusal) {
            *refusal = "secondary attribute budget";
        }
        return {};
    }

    // The phase marker must be followed by a NOP, and the nosched bit guards the boundary after the
    // next instruction: FinishNosched places it as the compiler does.
    // skipinv as psp2cgc places it (Ir::Builder::skip_invalid_auto): without it the pipes run a
    // task's invalid instances too. The LUT and noise reads are point sampled from one level
    // (Sample2DPoint), so only the texture units' coordinates keep it clear.
    e.skip_invalid_auto = true;
    e.skip_invalid = true;

    // A kill ends its phase: psp2cgc follows every kill with its nop and then a new phase
    // marker (all 148 of SM3DL's programs, whose scissor kill comes first: phase one is the
    // scissor test alone, phase two everything else). The first marker waits with
    // condition 1 when another phase follows, 7 otherwise, as the compiler's do. Before
    // this the emitter kept sampling and shading in the kill's phase and opened its second
    // phase only before the output pack; Vita3K did not mind, the console drew noise.
    e.Phas(!(uses_kill || uses_depth));
    e.Nop();
    Scissor();
    SampleTextures();
    // An own-pixel unit's texel is the tile's colour, as F32 (the U8 combiners copy it in
    // U8Inputs).
    if (!u8_tev) {
        for (u32 unit = 0; unit < 3; unit++) {
            if (unit_own[unit]) {
                // Two pairs: an F32 unpack into unified-store registers writes two lanes.
                const uint8_t at = static_cast<uint8_t>(TmpTex0 + 4 * unit);
                e.UnpackU8({Reg::V(at), 0x3}, Reg::O(0));
                e.UnpackU8({Reg::V(static_cast<uint8_t>(at + 2)), 0x3}, Reg::O(0), 0xEE);
                texel_loc[unit] = {Bank::Virtual, at};
            }
        }
    }
    if (uses_proctex && !proctex_tabled) {
        ProcTex();
    }
    if (!u8_tev) {
        RoundPrimary();
    }
    // The texture unit 0 colour is zero when the unit is disabled and a stage reads it.
    if (!unit_used[0] && config.texture.texture0_type == TextureType::Disabled) {
        CopyVec4({Bank::Virtual, TmpTex0}, {Bank::Const, 0});
    }
    if (uses_lighting) {
        Lighting();
    }
    PlanCombinerBuffer();
    if (u8_tev) {
        // combiner_output starts at zero, in U8.
        U8Inputs();
        u8_prev = Reg::V(TmpU8Zero);
        for (u32 i = 0; i < config.texture.tev_stages.size(); i++) {
            U8TevStage(i);
        }
        U8Output();
    } else {
        // combiner_output starts at zero; the buffers at zero and the register value.
        e.Vec(VecOp::Add, {Reg::V(TmpOutput), 0x3}, ConstZero, ConstZero);
        e.Vec(VecOp::Add, {Reg::V(TmpOutput + 2), 0x3}, ConstZero, ConstZero);
        for (u32 i = 0; i < config.texture.tev_stages.size(); i++) {
            TevStage(i);
        }
        AlphaTest();
        if (uses_depth && !uses_kill) {
            // No kill to share the phase end with: the depth write ends phase one by itself.
            WriteDepth();
            NewPhaseAfterKill();
        }
        Fog();
        e.PackF16({Reg::Pa(0), 0xf}, Reg::V(TmpOutput), Reg::V(TmpOutput + 2));
    }

    // The quads go to the inputs' pairs past their last reference (free: a task's size counts
    // its primary attributes and its temporaries together, sgxd_program_pixel_dms), then to the
    // temporaries, then to primary attributes past the interpolants.
    e.AddClass({Bank::PrimAttr, 0, static_cast<uint8_t>(program.pa_count & ~1u), SIZE_MAX, true});
    e.AddClass({Bank::Temp, 0, 64});
    e.AddClass({Bank::PrimAttr, static_cast<uint8_t>((program.pa_count + 3) & ~3u), 64});
    // A scalar or a pair in a quad of its own takes two registers, not four: the temporaries
    // decide how many pixel tasks run at once (the occupancy below).
    e.split_pairs = true;
    // The second tier keeps the hoisting whose registers let the most pixel tasks run at once
    // (the PDS's count in the draw's sgxd_program_pixel_dms word, 16 written as 0), then the
    // most USE instances: the LUT reads wait one by one, and only other tasks hide that.
    const uint32_t sa_words = uniform_floats + data_head + lit_pairs * 2 +
                              static_cast<uint32_t>(program.samplers.size()) * 4;
    e.occupancy = [&](uint32_t temps, uint32_t pa) {
        const uint32_t dms =
            sgxd_program_pixel_dms(std::max<uint32_t>({program.pa_count, pa, 2u}), temps, sa_words);
        const uint32_t tasks = (dms >> 22 & 15) != 0 ? (dms >> 22 & 15) : 16;
        return tasks << 4 | ((dms >> 15 & 3) + 1);
    };
    Encoder encoder;
    const Ir::LowerResult lowered = e.Lower(encoder, level);
    if (uses_kill) {
        encoder.PatchKills(LitSa(LitKillControl));
    }
    // Every fragment program has had SAs to DMA: one pair at least, rather than a pixel task
    // without a secondary DMA, which the console has not run.
    if (uniform_floats + data_head + lit_pairs * 2 + program.samplers.size() * 4 == 0) {
        LitSa(LitKillControl);
    }
    encoder.FinishNosched();
    program.literals.assign(lit_pairs * 2, 0u);
    for (uint32_t pair = 0; pair < LiteralCount / 2; pair++) {
        if (lit_pair_slot[pair] >= 0) {
            program.literals[lit_pair_slot[pair] * 2] = literal_table[pair * 2];
            program.literals[lit_pair_slot[pair] * 2 + 1] = literal_table[pair * 2 + 1];
        }
    }
    program.temp_count = lowered.temp_count;
    program.peak_live = lowered.peak_live;
    program.peak_at = lowered.peak_at;
    program.peak_regs = lowered.peak_regs;
    program.pa_count = std::max<uint32_t>(program.pa_count, lowered.pa_count);
    program.phase_starts = lowered.phase_starts;
    program.primary = encoder.Words();
    return program;
}

} // Anonymous namespace

uint32_t SampledTextureUnits(const FSConfig& config) {
    FragmentEmitter emitter{config, 1};
    return emitter.SampledUnits();
}

std::optional<Program> EmitFragmentProgram(const FSConfig& config, std::string* refusal,
                                          int level) {
    FragmentEmitter emitter{config, level};
    std::optional<FragmentEmitter> exact;
    try {
        FragmentEmitter emitter{config, level};
        return emitter.Emit(refusal);
    } catch (const std::exception& ex) {
        // An operand form the encoder cannot express; with the emission it stopped in and its
        // SA words so far (an SA past 127 does not fit an operand).
        if (refusal) {
            *refusal = ex.what();
        }
        return std::nullopt;
    }
}

} // namespace GxmRenderer::Usse
