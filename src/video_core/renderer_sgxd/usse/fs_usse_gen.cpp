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
#include "video_core/renderer_sgxd/usse/fs_usse_gen.h"
#include "video_core/renderer_sgxd/usse/usse_encoder.h"
#include "video_core/renderer_sgxd/usse/usse_ir.h"
#include "video_core/renderer_sgxd/usse/usse_program.h"

namespace SgxdRenderer::Usse {

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
constexpr uint8_t TmpLutDelta = 30;   ///< fog/proctex: the interpolation weight (1)
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
constexpr uint8_t TmpLightAtten = 0;  ///< lighting: a light's geometric factor (.y), in
                                      ///< combiner_output's home (the attenuation is TmpMisc.z)
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
// Shadows, F32 a lane each: the shadow write's tile word as bytes (the intensity, the depth's
// low, middle and high byte) and its values (d, s, ref_d); a Shadow2D read's -z and its taps'
// texels (TmpShadowVal holds the taps' results there).
constexpr uint8_t TmpShadowOld = 208;
constexpr uint8_t TmpShadowVal = 212;
constexpr uint8_t TmpShadowZ = 220;    ///< -z in x, the four taps' weights (fract) in z and w
constexpr uint8_t TmpShadowTap1 = 216; ///< a Shadow2D read's other three taps' texels
constexpr uint8_t TmpShadowTap2 = 224;
constexpr uint8_t TmpShadowTexel = 232;
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
/// lighting: a LUT read at N.V, the same for every light (.x D0, .y D1), read once before them;
/// N.V itself in .z.
constexpr uint8_t TmpLutShared = 244;
/// lighting: a memory light's base past the load offset's reach (LightParams): the uniform
/// buffer's pointer moved on to the light's quad, in TmpLutShared's unused .w.
constexpr uint8_t TmpLightBase = TmpLutShared + 3;
/// lighting: a light's D0 (.x) and D1 (.y), and its spot factor while the distance attenuation
/// is read (.z), in the last proctex batch read's home (the procedural texture is done before
/// the lights).
constexpr uint8_t TmpLutValue = 156;
/// lighting: the LUT inputs a light's lookups read off its light and half vectors (N.H, V.H,
/// L.N, the spot's), a lane each in that order, in the batch read before's home.
constexpr uint8_t TmpLutInput = 152;
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
    Lit65536, ///< shadows: a depth's high byte's weight and its inverse
    LitInv65536,
    LitDepthMax,    ///< 0xffffff, the 24-bit depth's scale
    LitQuarter,     ///< a byte's quarter, which the U8 pack keeps whether it rounds or truncates
    LitShadowWidth, ///< a Shadow2D unit 0's size (FSConfig shadow_width_log2): its border test
    LitShadowHeight,
    LitHalf,        ///< 0.5: a signed filtered LUT read's x per input
    LitLutWrapLast, ///< 127.5 / 256: its last positive x, entry 127's centre
    LitMinNormal,   ///< 2^-126, the smallest normal float: the bump renormalisation's floor
    LitMinNormalPad,
    LitShadowInvWidth, ///< 1 / LitShadowWidth and 1 / LitShadowHeight: a texel's step, a pair
    LitShadowInvHeight,
    LitShadowNegInvWidth, ///< -1 / LitShadowWidth and -1 / LitShadowHeight, a pair
    LitShadowNegInvHeight,
    LitNegHalf, ///< -0.5: a Shadow2D read's texel-space offset
    LitNegHalfPad,
    LiteralCount, // even, so the sampler words stay pair-aligned
};
static_assert(LiteralCount % 2 == 0);

/// A Shadow2D unit 0's side from FSConfig's log2 less 3.
constexpr u32 ShadowSize(u32 log2_less_3) {
    return 8u << log2_less_3;
}

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
    /// `literal_reserve`: the SA words the light budget keeps back for the literals (the whole
    /// table until an emission has counted the ones the program reads).
    FragmentEmitter(const FSConfig& config_, int level_, uint32_t literal_reserve_ = LiteralCount)
        : level{level_}, literal_reserve{literal_reserve_}, config{config_} {}

    std::optional<Program> Emit(std::string* refusal);
    uint32_t SampledUnits() {
        PlanInputs();
        return (unit_used[0] ? 1u : 0u) | (unit_used[1] ? 2u : 0u) | (unit_used[2] ? 4u : 0u);
    }
    int level = 1;
    /// After Emit: whether a light's parameters went to the uniform buffer's memory part, the
    /// literal words the program reads, and its SA words in all.
    bool LightsInMemory() const {
        return std::any_of(light_memory.begin(), light_memory.end(),
                           [](int32_t at) { return at >= 0; });
    }
    uint32_t LiteralWords() const {
        return lit_pairs * 2;
    }
    uint32_t SaWords() const {
        return uniform_floats + data_head + lit_pairs * 2 +
               static_cast<uint32_t>(program.samplers.size()) * 4;
    }
    /// The SA words' parts so far (also after a failed Emit), for a refusal.
    std::string Budget() const {
        const auto in_memory = std::count_if(light_memory.begin(), light_memory.end(),
                                             [](int32_t at) { return at >= 0; });
        return fmt::format("{} uniform floats, pointer {}, {} literal words ({} reserved), {} "
                           "samplers: {} SA words; {} lights, {} in memory",
                           uniform_floats, data_head, lit_pairs * 2, literal_reserve,
                           program.samplers.size(), SaWords(), config.lighting.src_num.Value(),
                           in_memory);
    }

private:
    uint32_t literal_reserve = LiteralCount;
    /// Null when the configuration can be emitted, else what stops it.
    const char* Unsupported() const;
    void PlanInputs();
    void PlanUniforms();
    /// `units`: the texture units read here (bit n unit n).
    void SampleTextures(uint32_t units = 7);
    void RoundPrimary();
    void Lighting();
    /// The LUT's value at `input` into `into` (one temp lane): a filtered read of the LUT's F16
    /// row, times `scale` if given. `pair_free`: `into` is an even lane with a free neighbour, so
    /// an unscaled read lands there itself (LightingLutRowFetch).
    void LightingLutLookup(Sampler sampler, Src input, bool abs_input, bool two_sided, Dst into,
                           const Src* scale = nullptr, bool pair_free = false);
    /// LightingLutLookup's second half, from TmpLutCoord's x: the row, the read and the value,
    /// into `into`. A LUT at the input the last lookup read goes on here from the same x.
    /// `row_set`: TmpLutCoord.y already holds the row (the pair's fold).
    void LightingLutRowFetch(Sampler sampler, bool abs_input, Dst into, const Src* scale = nullptr,
                             bool pair_free = false, bool row_set = false);
    void LutLfFetch();
    /// A LUT texel fetch at index i1.y (-i1.y when `negated`) from texture `sampler_index` (256
    /// texels a row) through the row scale and bias literal pairs, into TmpLutEntry, and waits.
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
    /// `parts`: bit 0 the inputs the alpha test's first phase reads (the constants, the vertex
    /// colour, early_units' texels), bit 1 the others. Both when the test comes last.
    void U8Inputs(uint32_t parts = 3);
    /// Whether the alpha test runs in a first phase of its own, before the lighting: the
    /// stages (early_stages) and units (early_units) the tested alpha comes from.
    bool PlanEarlyAlpha();
    /// A stage's alpha alone, as U8TevStage makes it (the colour bytes are not the stage's).
    void U8AlphaStage(u32 index);
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
    bool PrimaryLaneMoved() const;
    /// A texture issue's place in PDS order: after the last input so far, or before them all.
    uint8_t IssueAfter() const {
        return program.inputs.empty() ? IssueBeforeInputs
                                      : static_cast<uint8_t>(program.inputs.size() - 1);
    }
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
    /// The light about to be lit: whether its parameters live in memory (light_mem), and the
    /// loads' state for one that does. Its quads are read through LightQuad.
    void LightParams(uint32_t num);
    /// Quad q of the light LightParams named (0 specular_0, 1 specular_1, 2 diffuse with the
    /// distance scale in .w, 3 ambient, 4 position with the distance bias in .w, 5 the spot
    /// direction): a resident light's SAs, a memory light's quad of TmpLightMem, which
    /// LightQuads fills.
    Vec4Loc LightQuad(uint32_t q) const;
    /// Whether light i of the config has a specular_0 or a specular_1 term: the secondary
    /// colour is read and the light's colour is not zero (nor every light's, for specular_0).
    bool LightSpecular0(uint32_t light_index) const;
    bool LightSpecular1(uint32_t light_index) const;
    /// D0 or D1 read at N.V: the same for every light, read once before them (Lighting).
    bool LutSharedAtNv(const Pica::Shader::LutConfig& lut, Sampler sampler) const;
    /// The LUT inputs light i's own reads take (a bit per LutInput): the reads of a term that
    /// is left out are not made.
    uint32_t LightInputs(uint32_t light_index) const;
    /// The quads light i's program reads (bit q): a zero colour's, an unread spot direction's
    /// and an unlit sum's take no registers.
    uint32_t LightQuadsRead(uint32_t light_index) const;
    /// A memory light's parameter quads in `mask` loaded into TmpLightMem unless they are
    /// already; with `wait` waited for too, else a LUT fetch's WDF that follows covers them
    /// (no internal register may hold a value across either). Nothing for a resident light.
    void LightQuads(uint32_t mask, bool wait);
    /// Unit 0's Shadow2D read into its texel (every lane): four taps around the coordinate
    /// compared with its depth and filtered.
    void ShadowTexture(Reg coord, uint32_t sampler_index);
    /// Shadow rendering: the tile's word in o0 updated by the fragment's depth and the
    /// combiner output's green, into pa0 as U8. `u8` the U8
    /// combiners' result, or null for the F32 one in TmpOutput.
    void ShadowWrite(const Reg* u8);
    /// A uniform's place: `components` floats (or array_size float4s) at a pair-aligned
    /// offset.
    uint32_t AllocateUniform(UniformKind kind, uint8_t components, uint32_t array_size,
                             uint8_t index = 0);
    /// A U8 uniform's place (Uniform::u8): a word an element, anywhere.
    uint32_t AllocateUniformU8(UniformKind kind, uint8_t components, uint32_t array_size);
    /// An F16 float4 uniform's place (Uniform::f16): a pair an element, pair-aligned.
    uint32_t AllocateUniformF16(UniformKind kind, uint32_t array_size);
    /// A U8 word of element `index`'s component `lane` in every byte (Uniform::lane).
    uint32_t AllocateUniformU8Lane(UniformKind kind, uint8_t index, uint8_t lane);
    /// A SOP3's source 0, which is a temporary or a primary attribute: a secondary attribute
    /// copied into TmpU8Const first.
    Reg U8Src0(Reg r);
    /// The LUT sampler's index among the declared samplers.
    uint32_t LutSamplerIndex() const;
    bool LightingLutEnabled(const Pica::Shader::LutConfig& lut, Sampler sampler) const;

    FSConfig config;
    Ir::Builder e; ///< the program, in virtual quads; usse_ir places them
    bool u8_tev = false;             ///< the U8 combiners (U8TevSupported)
    int colour_u8_pa = -1;           ///< the vertex colour's bytes' register (PlanInputs)
    /// The U8 combiners read a unit's texel as four F16, packed to U8 from there: not a unit
    /// whose split alpha is a dependent read or that clamps to its border colour (U8Inputs
    /// would merge that alpha into the border's), or that the lighting reads as its shadow or
    /// bump map (that code moves or reads F32 lanes).
    bool TexelF16(u32 unit) const {
        const auto& tex = config.texture;
        const auto& border = tex.texture_border_color[unit];
        const auto& lighting = config.lighting;
        const bool split = tex.alpha_split >> unit & 1;
        return u8_tev && !((border.enable_s || border.enable_t) && split) &&
               !(unit == 0 && tex.texture0_type == TextureType::Shadow2D) &&
               (!(tex.alpha_split >> unit & 1) || unit_issued[unit]) &&
               !(uses_shadow && lighting.shadow_selector == unit) &&
               !(uses_lighting && lighting.bump_mode != LightingRegs::LightingBumpMode::None &&
                 lighting.bump_selector == unit);
    }
    /// An issued unit the U8 combiners read as its texture's stored bytes (FSConfig raw_u8): one
    /// register, no pack. Not a split unit (its alpha is merged from the F16 alpha) nor one
    /// clamping to its border colour (an F16 colour moves in).
    bool TexelRaw(u32 unit) const {
        const auto& tex = config.texture;
        const auto& border = tex.texture_border_color[unit];
        return TexelF16(unit) && unit_issued[unit] && (tex.raw_u8 >> unit & 1) &&
               !(tex.alpha_split >> unit & 1) && !border.enable_s && !border.enable_t;
    }
    std::array<bool, 3> texel_raw{}; ///< TexelRaw's units, issued after every input (PlanInputs)
    /// A split unit whose alpha texture the PDS reads too (TextureIssue::alpha), into alpha_loc.
    bool AlphaIssued(u32 unit) const {
        return (config.texture.alpha_split >> unit & 1) && unit_issued[unit];
    }
    std::array<Reg, 6> u8_out{};     ///< each stage's output, U8
    Reg u8_prev = Reg::V(TmpU8Zero); ///< the combiner output so far, U8
    /// White (0xffffffff) and AddSigned's half (U8Inputs): their literal words themselves, or
    /// their copies where a SOP cannot name the literal.
    Reg u8_white = Reg::V(TmpU8White);
    Reg u8_half = Reg::V(TmpU8Half);
    /// The lighting's sums packed to TmpU8Diffuse and TmpU8Specular where Lighting ends, not by
    /// U8Inputs.
    bool u8_lighting_packed = false;
    /// U8Broadcast's registers by BroadcastInput and lane; 0 for none.
    std::array<std::array<uint8_t, 3>, 7> u8_bcast{};
    uint32_t u8_bcast_count = 0;
    /// The alpha test first (PlanEarlyAlpha): a lit program whose tested alpha no light
    /// reaches tests it in a first phase, and lights only the pixels the test keeps.
    bool early_alpha = false;
    uint32_t early_stages = 0; ///< bit k: stage k's alpha reaches the test
    uint32_t early_units = 0;  ///< bit n: unit n's texel does
    Program program;
    std::array<bool, 3> unit_used{};
    /// The units whose texel is the pixel's own colour (FSConfig own_pixel): the tile's colour
    /// in o0, no sampler (unit_used is false for them).
    std::array<bool, 3> unit_own{};
    /// The units whose read the pixel PDS program issues (Program::issues): plain 2D at an
    /// untouched coordinate. Their texels are PAs, the others' the SMPs' temporaries.
    std::array<bool, 3> unit_issued{};
    bool lut_half_used = false;    ///< an unsigned lighting lookup reads the F16 rows
    uint32_t sampler_lut_half = 0; ///< their sampler (SamplerKind::LutLfHalf)
    bool lut_wrap_used = false;    ///< a signed one reads them repeating in u
    uint32_t sampler_lut_wrap = 0; ///< SamplerKind::LutLfHalfWrap
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
    /// With w_buffered, the F32 fog's TEXCOORD3 (u: the PICA's depth times w): its PA base.
    uint8_t fog_coord_pa = 0;
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
    /// The U8 combiners' constant with a moved channel, per stage and lane: its broadcast's SA
    /// (AllocateUniformU8Lane), ~0u where none reads it.
    std::array<std::array<uint32_t, 3>, 6> sa_const_lane{};
    std::array<uint32_t, 6> sa_dot3_const{}; ///< a stage's Dot3Const
    uint32_t sa_buffer_color = 0;
    uint32_t sa_scissor = 0;
    uint32_t sa_fog_offset = 0;
    uint32_t sa_fog_color = 0;
    std::array<uint32_t, 24> sa_lut_row{}; ///< per lighting LUT the program reads: its row's SA
    /// per lighting LUT: its (offset or bound, row) pair's SA, or UINT32_MAX (PlanUniforms)
    std::array<uint32_t, 24> sa_lut_coord{};
    uint32_t sa_global_ambient = 0;
    uint32_t sa_border = 0;               ///< tex_border_color float4[3]: F32 texels
    uint32_t sa_border16 = 0;             ///< tex_border_color half4[3]: F16 texels
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
    /// The light being lit when it lives in memory (LightParams): its float offset (from
    /// TmpLightBase with light_mem_far), its quads loaded into TmpLightMem so far (bit q:
    /// floats 4q..4q+3), and whether a load still waits for its WDF.
    bool light_mem = false;
    uint32_t light_num = 0; ///< the PICA number of the light being lit
    bool light_mem_far = false;
    uint32_t light_mem_offset = 0;
    uint32_t light_mem_loaded = 0;
    bool light_mem_pending = false;
    /// W-buffering (not shadow rendering): the fog's depth is the PICA's W depth, TEXCOORD3's u.
    bool w_buffered = false;
    bool uses_shadow = false;             ///< lighting reads the shadow texture
    bool shadow_write = false;            ///< shadow rendering: ShadowWrite updates o0's word
    bool shadow_wbuffer = false;          ///< its depth is z / w (W-buffering)
    bool shadow_texture = false;          ///< unit 0 is a Shadow2D read (ShadowTexture)
    /// The lighting's shadow is unit 0's Shadow2D texel, one value in every lane: kept as
    /// TmpShadow.x alone, a scalar the lights multiply by.
    bool shadow_scalar = false;
    /// shadow_scalar with nothing else reading unit 0 (no stage, no bump map): ShadowTexture
    /// writes the scalar itself, and unit 0 has no texel.
    bool shadow_direct = false;
    uint32_t sa_shadow_constant = 0;      ///< shadow_bias_constant, shadow_bias_linear
    uint32_t sa_shadow_linear = 0;
    uint32_t sa_shadow_tex_bias = 0;      ///< shadow_texture_bias
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
    /// Per PICA light number, each quad's place (LightQuad's numbers): a resident light's SA,
    /// a memory light's float offset from light_memory; -1 for a quad its program does not
    /// read (LightQuadsRead).
    std::array<std::array<int32_t, 6>, 8> light_quad{};
};

const char* FragmentEmitter::Unsupported() const {
    const auto& fb = config.framebuffer;
    const auto& tex = config.texture;
    const auto& lighting = config.lighting;
    if (fb.shadow_rendering && tex.fog_mode == TexturingRegs::FogMode::Gas) {
        return "shadow rendering with gas";
    }
    if (fb.shadow_rendering && tex.fog_mode == TexturingRegs::FogMode::Fog) {
        return "shadow rendering with fog";
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
    const auto t0 = tex.texture0_type.Value();
    if (t0 == TextureType::Shadow2D && (tex.alpha_split & 1)) {
        return "a split alpha shadow texture";
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
    // Shadow rendering writes no depth: its own depth is in the word it writes (ShadowWrite).
    shadow_write = config.framebuffer.shadow_rendering != 0;
    shadow_wbuffer = shadow_write && config.framebuffer.depthmap_enable ==
                                         Pica::RasterizerRegs::DepthBuffering::WBuffering;
    // W-buffering: the TA orders the draw by 1/w (WBUFFEN), and the program writes no depth.
    w_buffered = !shadow_write && config.framebuffer.depthmap_enable ==
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
    shadow_texture = unit_used[0] && tex.texture0_type == TextureType::Shadow2D;
    shadow_scalar = uses_shadow && shadow_texture && lighting.shadow_selector == 0;
    shadow_direct = shadow_scalar && !StageReads(Source::Texture0) &&
                    !(lighting.bump_mode != LightingRegs::LightingBumpMode::None &&
                      lighting.bump_selector == 0);
    for (u32 unit = 0; unit < 3; unit++) {
        const auto& border = tex.texture_border_color[unit];
        if (unit_used[unit] && (border.enable_s || border.enable_t)) {
            uses_border = true;
        }
    }
    if (uses_lighting) {
        // The lookups read the filtered F16 rows, a signed one repeating in u.
        const auto note = [&](const Pica::Shader::LutConfig& lut, Sampler sampler) {
            if (LightingLutEnabled(lut, sampler)) {
                (lut.abs_input ? lut_half_used : lut_wrap_used) = true;
            }
        };
        note(lighting.lut_d0, Sampler::Distribution0);
        note(lighting.lut_d1, Sampler::Distribution1);
        note(lighting.lut_fr, Sampler::Fresnel);
        note(lighting.lut_rr, Sampler::ReflectRed);
        note(lighting.lut_rg, Sampler::ReflectGreen);
        note(lighting.lut_rb, Sampler::ReflectBlue);
        for (u32 i = 0; i < lighting.src_num; i++) {
            const auto& light = lighting.lights[i];
            if (light.dist_atten_enable) {
                lut_half_used = true;
            }
            if (light.spot_atten_enable) {
                note(lighting.lut_sp, Sampler::SpotlightAttenuation);
            }
        }
    }
    fog_issued = uses_fog && u8_tev;
    lut_used = uses_fog && !fog_issued;

    // The U8 combiners read the vertex colour as the iterator's bytes: one register after the
    // other inputs, no pack. Not where a stage moves one of its channels (U8Inputs packs that
    // from the F32 lanes); unread, the colour is not iterated.
    const bool colour_u8 = u8_tev && StageReads(Source::PrimaryColor) && !PrimaryLaneMoved();
    // Otherwise pa0-3 is the vertex colour; the quaternions and the view arrive as half4. The
    // colour, position and coordinates stay F32: F16 is too coarse for byte rounding and depth.
    uint8_t pa = 0;
    if (!u8_tev || (StageReads(Source::PrimaryColor) && !colour_u8)) {
        program.inputs.push_back({FragmentInput::Color0, 4});
        pa = 4;
    }
    if (uses_scissor || (uses_fog && !fog_issued && !w_buffered) || shadow_write) {
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
    // A plain 2D read at an untouched coordinate is the PDS's to issue before the program starts
    // (a non-dependent read: no SMP, no wait). The vertex program writes TEXCOORD0..2 as
    // (s, 1 - t), so the coordinate needs no arithmetic, and is iterated only for the border test
    // or a split unit's alpha read. A shadow texture's taps are the program's own (ShadowTexture).
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
    // The texels follow the iterated inputs so far, in PDS order, pair-aligned; a raw one (a
    // word) after every input instead, so nothing pair-aligned follows it.
    for (u32 unit = 0; unit < 3; unit++) {
        texel_raw[unit] = TexelRaw(unit);
        if (!unit_issued[unit] || texel_raw[unit]) {
            continue;
        }
        const bool f16 = TexelF16(unit);
        texel_loc[unit] = {Bank::PrimAttr, pa, f16};
        program.issues.push_back({static_cast<uint8_t>(unit),
                                  static_cast<uint8_t>(unit_coord[unit]), pa,
                                  IssueAfter(), f16});
        pa += f16 ? IssueTexelWordsF16 : IssueTexelWords;
        if (AlphaIssued(unit)) {
            // The split alpha's A8 texture at the same coordinate, its byte in .w, in the
            // colour's form; SampleTextures (F32) or U8Inputs (F16) puts it in the texel.
            alpha_loc[unit] = {Bank::PrimAttr, pa, f16};
            program.issues.push_back(
                {static_cast<uint8_t>(unit), static_cast<uint8_t>(unit_coord[unit]), pa,
                 IssueAfter(), f16, false, true});
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
    const bool texcoord0_w = unit_used[0] && (tex.texture0_type == TextureType::Projection2D ||
                                              tex.texture0_type == TextureType::TextureCube ||
                                              tex.texture0_type == TextureType::Shadow2D);
    // The F32 fog's depth with W-buffering: the PICA's W depth, TEXCOORD3's u.
    const bool fog_coord = uses_fog && !fog_issued && w_buffered;
    if (texcoord0_w || fog_coord) {
        // texcoord0's w, TEXCOORD3's second lane (its first is the fog's depth times w).
        DEBUG_ASSERT(pa % 2 == 0);
        program.inputs.push_back({FragmentInput::TexCoord3, 2});
        texcoord0_w_pa = static_cast<uint8_t>(pa + 1);
        fog_coord_pa = pa;
        pa += 2;
    }
    if (colour_u8) {
        // The colour's four bytes (DOUTI without the colour's float bit).
        program.inputs.push_back({FragmentInput::Color0, 4, false, true});
        colour_u8_pa = pa;
        pa += 1;
    }
    for (u32 unit = 0; unit < 3; unit++) {
        if (texel_raw[unit]) {
            texel_loc[unit] = {Bank::PrimAttr, pa, false};
            TextureIssue issue{static_cast<uint8_t>(unit), static_cast<uint8_t>(unit_coord[unit]),
                               pa, IssueAfter()};
            issue.raw = true;
            program.issues.push_back(issue);
            pa += IssueTexelWordsRaw;
        }
    }
    if (fog_issued) {
        // The fog factor's word, after every input: one register, so nothing pair-aligned
        // follows it either.
        fog_pa = pa;
        program.issues.push_back(
            {0, 3, pa, IssueAfter(), false, true});
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
    if (lut_half_used) {
        sampler_lut_half = static_cast<uint32_t>(program.samplers.size());
        program.samplers.push_back({SamplerKind::LutLfHalf, 0, 10});
    }
    if (lut_wrap_used) {
        sampler_lut_wrap = static_cast<uint32_t>(program.samplers.size());
        program.samplers.push_back({SamplerKind::LutLfHalfWrap, 0, 11});
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

uint32_t FragmentEmitter::AllocateUniformU8Lane(UniformKind kind, uint8_t index, uint8_t lane) {
    const uint32_t at = uniform_floats;
    program.uniforms.push_back({kind, index, at, 4, 1, true, false, static_cast<int8_t>(lane)});
    uniform_floats++;
    return at;
}

uint32_t FragmentEmitter::AllocateUniformF16(UniformKind kind, uint32_t array_size) {
    if (uniform_floats & 1) {
        uniform_floats++;
    }
    const uint32_t at = uniform_floats;
    program.uniforms.push_back({kind, 0, at, 4, array_size, false, true});
    uniform_floats += array_size * 2;
    return at;
}

void FragmentEmitter::PlanUniforms() {
    const auto& tex = config.texture;
    const auto& lighting = config.lighting;
    if (config.framebuffer.alpha_test_func != FramebufferRegs::CompareFunc::Always &&
        config.framebuffer.alpha_test_func != FramebufferRegs::CompareFunc::Never) {
        sa_alpha_ref = AllocateUniform(UniformKind::AlphaTestRef, 1, 1);
    }
    if (shadow_write) {
        sa_shadow_constant = AllocateUniform(UniformKind::ShadowBiasConstant, 1, 1);
        sa_shadow_linear = AllocateUniform(UniformKind::ShadowBiasLinear, 1, 1);
    }
    if (shadow_texture) {
        sa_shadow_tex_bias = AllocateUniform(UniformKind::ShadowTextureBias, 1, 1);
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
    // A constant's moved channel (const.rrr, an alpha from const.g) as a word of its own with that
    // byte in every byte, made by the host: no unpack and pack a use.
    for (auto& lanes : sa_const_lane) {
        lanes.fill(~0u);
    }
    if (u8_tev && const_stages != 0) {
        ForEachU8Arg([&](u32 k, Source csrc, ColorModifier cmod, Source asrc,
                         AlphaModifier amod) {
            const auto add = [&](Source src, int lane) {
                if (src == Source::Constant && lane >= 0 && sa_const_lane[k][lane] == ~0u) {
                    sa_const_lane[k][lane] = AllocateUniformU8Lane(
                        UniformKind::ConstColor, static_cast<uint8_t>(k),
                        static_cast<uint8_t>(lane));
                }
            };
            add(csrc, MovedLane(cmod));
            add(asrc, MovedLane(amod));
        });
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
        // In the form of the texels it replaces (SampleTextures).
        bool f32 = false, f16 = false;
        for (u32 unit = 0; unit < 3; unit++) {
            const auto& border = tex.texture_border_color[unit];
            if (unit_used[unit] && (border.enable_s || border.enable_t)) {
                (TexelF16(unit) ? f16 : f32) = true;
            }
        }
        if (f32) {
            sa_border = AllocateUniform(UniformKind::TexBorderColor, 4, 3);
        }
        if (f16) {
            sa_border16 = AllocateUniformF16(UniformKind::TexBorderColor, 3);
        }
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
    sa_lut_row.fill(UINT32_MAX);
    sa_lut_coord.fill(UINT32_MAX);
    if (uses_lighting) {
        // A row scalar for each LUT read, packed: a prefix up to the highest of the 24 took the SAs
        // a resident light needs.
        std::array<bool, 24> row_used{};
        const auto note = [&](bool used, u32 sampler) {
            row_used[sampler] = row_used[sampler] || used;
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
        // Lights are register-resident (four SAs a quad they read) while the budget holds, counting
        // the literals, the sampler words and the pointer behind them; the rest live in the memory
        // part of the uniform buffer and are loaded when their turn comes.
        const uint32_t tail =
            literal_reserve + static_cast<uint32_t>(program.samplers.size()) * 4 + 1;
        // A filtered lookup takes its row as the y of a pair whose x is the read's offset
        // (unsigned input) or bound (signed), so its coordinate is one instruction. The pairs
        // cost an SA each, so only while every light stays resident: shared LUTs first.
        std::array<const Pica::Shader::LutConfig*, 24> pair_lut{};
        pair_lut[0] = &lighting.lut_d0;
        pair_lut[1] = &lighting.lut_d1;
        pair_lut[3] = &lighting.lut_fr;
        pair_lut[4] = &lighting.lut_rb;
        pair_lut[5] = &lighting.lut_rg;
        pair_lut[6] = &lighting.lut_rr;
        for (u32 num = 0; num < 8; num++) {
            pair_lut[8 + num] = &lighting.lut_sp;
        }
        const auto resident_lights = [&](u32 paired) {
            uint32_t at = uniform_floats;
            u32 singles = 0;
            for (u32 sampler = 0; sampler < row_used.size(); sampler++) {
                if (row_used[sampler] && (paired >> sampler & 1) != 0) {
                    at = ((at + 1) & ~1u) + 2;
                } else if (row_used[sampler]) {
                    singles++;
                }
            }
            at = ((at + singles + 1) & ~1u) + 3; // the global ambient
            u32 resident = 0;
            for (u32 i = 0; i < lighting.src_num; i++) {
                const u32 floats = 4 * static_cast<u32>(std::popcount(LightQuadsRead(i)));
                if (((at + 1) & ~1u) + floats + tail > 128) {
                    break;
                }
                at = ((at + 1) & ~1u) + floats;
                resident++;
            }
            return resident;
        };
        const u32 resident = resident_lights(0);
        u32 paired = 0;
        for (u32 sampler = 0; sampler < row_used.size(); sampler++) {
            if (row_used[sampler] && pair_lut[sampler] != nullptr &&
                resident_lights(paired | 1u << sampler) == resident) {
                paired |= 1u << sampler;
            }
        }
        for (u32 sampler = 0; sampler < row_used.size(); sampler++) {
            if (row_used[sampler] && (paired >> sampler & 1) != 0) {
                const UniformKind kind = static_cast<bool>(pair_lut[sampler]->abs_input)
                                             ? UniformKind::LightingLutCoordHalf
                                             : UniformKind::LightingLutCoordWrap;
                sa_lut_coord[sampler] = AllocateUniform(kind, 2, 1, static_cast<uint8_t>(sampler));
                sa_lut_row[sampler] = sa_lut_coord[sampler] + 1;
            }
        }
        for (u32 sampler = 0; sampler < row_used.size(); sampler++) {
            if (row_used[sampler] && sa_lut_row[sampler] == UINT32_MAX) {
                sa_lut_row[sampler] = AllocateUniform(UniformKind::LightingLutRow, 1, 1,
                                                      static_cast<uint8_t>(sampler));
            }
        }
        sa_global_ambient = AllocateUniform(UniformKind::LightingGlobalAmbient, 3, 1);
        for (auto& quads : light_quad) {
            quads.fill(-1);
        }
        bool in_memory = false;
        for (u32 i = 0; i < lighting.src_num; i++) {
            const auto& config_light = lighting.lights[i];
            const u32 num = config_light.num;
            // A directional light's vector is its position normalised, the same for the
            // whole draw: the rasterizer normalises it (LightDirection), unless the distance
            // attenuation wants the position's length.
            const UniformKind position = LightPreNormalised(config_light)
                                             ? UniformKind::LightDirection
                                             : UniformKind::LightPosition;
            // A quad a member of three floats, the quads the program reads alone
            // (LightQuadsRead). The distance attenuation's scale and bias ride in the .w of
            // the diffuse and position quads; a memory light loads those first (Lighting).
            const UniformKind members[6] = {UniformKind::LightSpecular0,
                                            UniformKind::LightSpecular1,
                                            UniformKind::LightDiffuse,
                                            UniformKind::LightAmbient,
                                            position,
                                            UniformKind::LightSpotDirection};
            const u32 quads = LightQuadsRead(i);
            const u32 floats = 4 * static_cast<u32>(std::popcount(quads));
            // Once a light is in memory the ones after it are too (resident_lights' count).
            const bool resident_light =
                !in_memory && ((uniform_floats + 1) & ~1u) + floats + tail <= 128;
            in_memory = !resident_light;
            const uint8_t light = static_cast<uint8_t>(num);
            for (u32 q = 0; q < 6; q++) {
                if ((quads >> q & 1) == 0) {
                    continue;
                }
                const UniformKind partner =
                    q == 2 ? UniformKind::LightDistAttenScale : UniformKind::LightDistAttenBias;
                const bool partnered = (q == 2 || q == 4) && config_light.dist_atten_enable;
                if (resident_light) {
                    light_quad[num][q] =
                        static_cast<int32_t>(AllocateUniform(members[q], 3, 1, light));
                    if (partnered) {
                        AllocateUniform(partner, 1, 1, light);
                    }
                } else {
                    const uint32_t at = AllocateMemoryUniform(members[q], light, 3);
                    if (light_memory[num] < 0) {
                        light_memory[num] = static_cast<int32_t>(at);
                    }
                    light_quad[num][q] = static_cast<int32_t>(at) - light_memory[num];
                    if (partnered) {
                        AllocateMemoryUniform(partner, light, 1);
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

void FragmentEmitter::LightParams(uint32_t num) {
    light_num = num;
    light_mem = light_memory[num] >= 0;
    if (!light_mem) {
        return;
    }
    // A load per quad: one sixteen-float load left the quads after the first unloaded. The offset
    // is seven bits of floats, which the fifth light past the budget passes: from there the quads
    // load from the pointer plus the light's quad, as psp2cgc addresses a far uniform.
    const uint32_t off = static_cast<uint32_t>(light_memory[num]);
    const int32_t last = *std::max_element(light_quad[num].begin(), light_quad[num].end());
    light_mem_far = off + static_cast<uint32_t>(last) > 127;
    if (light_mem_far) {
        const Reg base = Reg::V(TmpLightBase);
        e.Bitwise(BwOp::Or, base, IOp::Imm(static_cast<uint8_t>(off / 4)), IOp::Imm(0));
        e.MadI32(base, base, 16, Reg::Sa(static_cast<uint8_t>(uniform_floats)));
    }
    light_mem_offset = light_mem_far ? off % 4 : off;
    light_mem_loaded = 0;
    light_mem_pending = false;
}

Vec4Loc FragmentEmitter::LightQuad(uint32_t q) const {
    if (light_mem) {
        return {Bank::Virtual, static_cast<uint8_t>(TmpLightMem + 4 * q)};
    }
    DEBUG_ASSERT_MSG(light_quad[light_num][q] >= 0, "lighting: light {}'s quad {} is not planned",
                     light_num, q);
    return {Bank::SecAttr, static_cast<uint8_t>(light_quad[light_num][q])};
}

bool FragmentEmitter::LightSpecular0(uint32_t light_index) const {
    const auto& lighting = config.lighting;
    return uses_secondary_color && !lighting.specular0_zero &&
           !lighting.lights[light_index].specular0_zero;
}

bool FragmentEmitter::LightSpecular1(uint32_t light_index) const {
    return uses_secondary_color && !config.lighting.lights[light_index].specular1_zero;
}

bool FragmentEmitter::LutSharedAtNv(const Pica::Shader::LutConfig& lut, Sampler sampler) const {
    // The lights' two-sidedness picks the lookup's form, so they must agree.
    const auto& lighting = config.lighting;
    bool same_sides = lighting.src_num > 1;
    for (u32 i = 1; i < lighting.src_num; i++) {
        same_sides = same_sides &&
                     lighting.lights[i].two_sided_diffuse == lighting.lights[0].two_sided_diffuse;
    }
    return uses_secondary_color && same_sides && LightingLutEnabled(lut, sampler) &&
           lut.type == LutInput::NV;
}

uint32_t FragmentEmitter::LightInputs(uint32_t light_index) const {
    const auto& lighting = config.lighting;
    const auto& light = lighting.lights[light_index];
    u32 inputs = 0;
    const auto input_of = [&](const Pica::Shader::LutConfig& lut, Sampler sampler) {
        if (LightingLutEnabled(lut, sampler)) {
            inputs |= 1u << static_cast<u32>(lut.type.Value());
        }
    };
    if (light.spot_atten_enable) {
        input_of(lighting.lut_sp, Sampler::SpotlightAttenuation);
    }
    // D0 multiplies specular_0 alone; D1 and the reflection tables specular_1.
    if (LightSpecular0(light_index) &&
        !LutSharedAtNv(lighting.lut_d0, Sampler::Distribution0)) {
        input_of(lighting.lut_d0, Sampler::Distribution0);
    }
    if (LightSpecular1(light_index)) {
        if (!LutSharedAtNv(lighting.lut_d1, Sampler::Distribution1)) {
            input_of(lighting.lut_d1, Sampler::Distribution1);
        }
        input_of(lighting.lut_rr, Sampler::ReflectRed);
        input_of(lighting.lut_rg, Sampler::ReflectGreen);
        input_of(lighting.lut_rb, Sampler::ReflectBlue);
    }
    if (light_index + 1 == lighting.src_num &&
        ((uses_primary_color && lighting.enable_primary_alpha) ||
         (uses_secondary_color && lighting.enable_secondary_alpha))) {
        input_of(lighting.lut_fr, Sampler::Fresnel);
    }
    return inputs;
}

uint32_t FragmentEmitter::LightQuadsRead(uint32_t light_index) const {
    const auto& light = config.lighting.lights[light_index];
    u32 quads = 0x10; // the position
    if (uses_primary_color || light.dist_atten_enable) {
        quads |= 0x4; // the diffuse colour, the distance scale beside it
    }
    if (uses_primary_color && !light.ambient_zero) {
        quads |= 0x8;
    }
    if (LightSpecular0(light_index)) {
        quads |= 0x1;
    }
    if (LightSpecular1(light_index)) {
        quads |= 0x2;
    }
    if ((LightInputs(light_index) >> static_cast<u32>(LutInput::SP) & 1) != 0) {
        quads |= 0x20;
    }
    return quads;
}

void FragmentEmitter::LightQuads(uint32_t mask, bool wait) {
    // Each quad is loaded just before the code that reads it, to keep the temporaries down.
    if (!light_mem) {
        return;
    }
    // The data block's head, or the far light's base.
    const Reg ptr =
        light_mem_far ? Reg::V(TmpLightBase) : Reg::Sa(static_cast<uint8_t>(uniform_floats));
    for (uint32_t q = 0; q < 6; q++) {
        const int32_t at = light_quad[light_num][q];
        if ((mask >> q & 1) != 0 && (light_mem_loaded >> q & 1) == 0 && at >= 0) {
            e.Lda32(Reg::V(static_cast<uint8_t>(TmpLightMem + 4 * q)), ptr, nullptr,
                    static_cast<uint8_t>(light_mem_offset + static_cast<uint32_t>(at)), 4, 0);
            light_mem_loaded |= 1u << q;
            light_mem_pending = true;
        }
    }
    if (wait && light_mem_pending) {
        e.WdfVertex(0);
        light_mem_pending = false;
    }
}

void FragmentEmitter::ShadowTexture(Reg coord, uint32_t sampler_index) {
    // Four taps, each compared on its own and blended bilinearly (PCF). A texel gives its density
    // (red) where its depth (alpha, blue, green: high to low byte) is at or past
    // z = trunc(min(|w|, 1) * 0xffffff) - bias, else 0. Each tap is a point read; a border side is
    // tested here.
    const auto& tex = config.texture;
    const Reg w_pair = Reg::Pa(static_cast<uint8_t>(texcoord0_w_pa & ~1u));
    const Swizzle w_swz = (texcoord0_w_pa & 1) ? YYYY : XXXX;
    constexpr Swizzle XYXY{Ch::X, Ch::Y, Ch::X, Ch::Y};
    // The coordinate as the host copy reads it, (s, 1 - t); divided by w unless orthographic,
    // as a Projection2D unit's: back to t, divided, flipped again.
    Reg uv = coord;
    if (!tex.shadow_texture_orthographic) {
        const Reg proj = Reg::V(TmpProj);
        e.Vec(VecOp::Add, {proj, 0x1}, {coord, XYZW}, ConstZero, Pred::None, true);
        e.Vec(VecOp::Add, {proj, 0x2}, {coord, XYZW, Mod::Neg}, ConstOne, Pred::None, true);
        e.Comp(CompOp::Rcp, {I0, 0x1}, {w_pair}, static_cast<uint8_t>(texcoord0_w_pa & 1),
               Pred::None, true);
        e.Vec(VecOp::Mul, {proj, 0x3}, {proj, XYZW}, {I0, XXXX}, Pred::None, true);
        const Reg flipped = Reg::V(TmpTexFlip);
        e.Vec(VecOp::Add, {flipped, 0x3}, {proj, XYZW}, ConstZero, Pred::None, true);
        e.Vec(VecOp::Add, {flipped, 0x2}, {proj, XYZW, Mod::Neg}, ConstOne, Pred::None, true);
        uv = flipped;
    }
    const Reg weights = Reg::V(TmpShadowZ);
    // c = uv * size - 0.5 in texels; f = fract(c), the weights, in TmpShadowZ's z and w;
    // the first tap at the centre of floor(c), (floor(c) + 0.5) / size = uv - f / size, the
    // others a texel right, down and both. The size is a power of two: every product here is
    // exact, and uv - f / size is the centre itself, which a float holds.
    const Src size{Reg::Sa(static_cast<uint8_t>(LitSa(LitShadowWidth) & ~1u)), XYZW};
    const Src inv_size{Reg::Sa(static_cast<uint8_t>(LitSa(LitShadowInvWidth) & ~1u)), XYXY};
    const Src neg_inv_size{Reg::Sa(static_cast<uint8_t>(LitSa(LitShadowNegInvWidth))), XYZW};
    const Reg texels = Reg::V(TmpShadowTexel); // c, until the reads land there
    const Reg fract = Reg::V(TmpShadowZ + 2);
    e.Mad2({texels, 0x3}, {uv, XYZW}, size, Lit(LitNegHalf));
    e.Vec(VecOp::Frc, {fract, 0x3}, {texels, XYZW}, {texels, XYZW}, Pred::None, true);
    // The taps two at a time, each pair waited for and compared before the next (four in flight
    // raise the register peak). (0, 0)'s coordinate in TmpProj, kept for the others; (1, 0) and
    // then (0, 1) in TmpTexFlip, (1, 1) in TmpProj's other pair, each made after the read before
    // it in that register has been waited for.
    const Reg tap0 = Reg::V(TmpProj), tap_k = Reg::V(TmpTexFlip), tap_m = Reg::V(TmpProj + 2);
    const Reg tap_coord[4] = {tap0, tap_k, tap_k, tap_m};
    e.Mad2({tap0, 0x3}, {fract, XYZW}, neg_inv_size, {uv, XYZW});
    e.Vec(VecOp::Add, {taps_b, 0xf}, {I0, XYXY}, ConstZero, Pred::None, true);
    e.Vec(VecOp::Add, {taps_b, 0x1}, {taps_b, XYZW}, {inv_size.reg, XXXX}, Pred::None, true);
    e.Vec(VecOp::Add, {taps_b, 0x8}, {taps_b, XYZW}, {inv_size.reg, YYYY}, Pred::None, true);
    // Each tap's texel as four F16 lanes in two words (FCONV F16): with NORM off (TextureWords)
    // its bytes come back as whole numbers, which F16 holds exactly. A pair for each of the two.
    const Reg texel_of[4] = {Reg::V(TmpShadowTexel), Reg::V(TmpShadowTexel + 2),
                             Reg::V(TmpShadowTexel), Reg::V(TmpShadowTexel + 2)};
    // Each tap's result in TmpShadowVal: x the (0, 0) tap, y (0, 1), z (1, 0), w (1, 1), so
    // that one pass along x blends both rows. A result is the tap's density where its depth
    // ref = G + 256 B + 65536 A is at or past z, else 0, in bytes: to 0..1 once, after the
    // blend. The test takes -z (weights.x) less -ref (the dot product with the negated byte
    // weights; whole numbers below 2^24, exact): a test's second source is not negated.
    const Reg results = Reg::V(TmpShadowVal);
    const uint8_t result_lane[4] = {0x1, 0x4, 0x2, 0x8};
    const auto& border = tex.texture_border_color[0];
    const bool bordered = border.enable_s || border.enable_t;
    // z itself (a test's second source is not negated); the weights (R, G, B, A) = (0, 1, 256,
    // 65536); every result 0.
    const Reg z = Reg::V(TmpShadowTap3);
    e.Vec(VecOp::Add, {z, 0x1}, {weights, XXXX, Mod::Neg}, ConstZero, Pred::None, true);
    e.Vec(VecOp::Add, {I2, 0x1}, ConstZero, ConstZero, Pred::None, true);
    e.Vec(VecOp::Add, {I2, 0x2}, ConstOne, ConstZero, Pred::None, true);
    e.Vec(VecOp::Add, {I2, 0x4}, Lit(Lit256), ConstZero, Pred::None, true);
    e.Vec(VecOp::Add, {I2, 0x8}, Lit(Lit65536), ConstZero, Pred::None, true);
    e.Vec(VecOp::Add, {results, 0xf}, ConstZero, ConstZero, Pred::None, true);
    // A border tap's result depends on z alone: the border colour's, once, in border_result.x.
    const Reg border_result = Reg::V(TmpShadowTap2);
    if (bordered) {
        LoadVec4(I1, {Bank::SecAttr, static_cast<uint8_t>(sa_border)});
        // Its bytes: floor(c * 255 + 0.5), from -floor.
        e.Vec(VecOp::Mul, {I1, 0xf}, {I1, XYZW}, Lit(LitTwoFiftyFive), Pred::None, true);
        e.Vec(VecOp::Add, {I1, 0xf}, {I1, XYZW}, ConstHalf, Pred::None, true);
        e.Vec(VecOp::Frc, {I1, 0xf}, ConstZero, {I1, XYZW}, Pred::None, true);
        e.Vec(VecOp::Add, {I1, 0xf}, {I1, XYZW, Mod::Neg}, ConstZero, Pred::None, true);
        e.Dot({I0, 0x1}, {I1, XYZW}, {I2, XYZW}, true, Pred::None, true);
        e.Vec(VecOp::Add, {border_result, 0x1}, ConstZero, ConstZero, Pred::None, true);
        e.Test(Cond::Ge, 2, {I0}, 0, {z, XXXX}, true);
        e.Vec(VecOp::Add, {border_result, 0x1}, {I1, XXXX}, ConstZero, Pred::P2, true);
    }
    for (int r = 0; r < 2; r++) {
        if (r == 0) {
            e.Vec(VecOp::Add, {tap_k, 0x3}, {inv_size.reg, X0}, {tap0, XYZW}, Pred::None, true);
        } else {
            e.Vec(VecOp::Add, {tap_k, 0x3}, {inv_size.reg, Y0}, {tap0, XYZW}, Pred::None, true);
            e.Vec(VecOp::Add, {tap_m, 0x3}, inv_size, {tap0, XYZW}, Pred::None, true);
        }
        e.Sample2DPoint(texel_of[2 * r], tap_coord[2 * r], SamplerSa(sampler_index), 0, 2, 2);
        e.Sample2DPoint(texel_of[2 * r + 1], tap_coord[2 * r + 1], SamplerSa(sampler_index), 0, 2,
                        2);
        if (r == 0) {
            // While the first reads are in flight: -z = bias - trunc(min(|w|, 1) * 0xffffff), every
            // result 0, and a border tap's result (it depends on z alone) from the border colour's
            // (-depth, density), made by the rasterizer once a draw (ShadowBorderDepth).
            e.Vec(VecOp::Min, {I0, 0x1}, {w_pair, w_swz, Mod::Abs}, ConstOne, Pred::None, true);
            e.Vec(VecOp::Mul, {I0, 0x1}, {I0, XXXX}, Lit(LitDepthMax), Pred::None, true);
            e.Vec(VecOp::Frc, {I0, 0x1}, ConstZero, {I0, XXXX}, Pred::None, true); // -floor
            e.Vec(VecOp::Add, {weights, 0x1}, {I0, XXXX}, SaScalar(sa_shadow_tex_bias),
                  Pred::None, true);
            e.Vec(VecOp::Add, {results, 0xf}, ConstZero, ConstZero, Pred::None, true);
            if (bordered) {
                const Reg border_depth = Reg::Sa(static_cast<uint8_t>(sa_shadow_border));
                e.Vec(VecOp::Add, {border_result, 0x1}, ConstZero, ConstZero, Pred::None, true);
                e.Test(Cond::Ge, 2, {weights}, 0, {border_depth, XXXX}, true);
                e.Vec(VecOp::Add, {border_result, 0x1}, {border_depth, YYYY}, ConstZero,
                      Pred::P2, true);
            }
        }
        e.Wdf(0);
        for (int k = 2 * r; k < 2 * r + 2; k++) {
        // A border side (ClampToBorder) reads the border colour past the texture: a tap whose
        // point is outside [0, 1] on that axis. A tap's centre is never 0 or 1 ((i + 0.5) /
        // size), so it is inside where (c - 1) c < 0: c - 1 for both axes, the test unit's
        // product with c an axis, folded by AND into p1, and the border's result taken where
        // not.
        if (bordered) {
            e.Vec(VecOp::Add, {I0, 0x3}, {ConstOne.reg, XXXX, Mod::Neg}, {tap_coord[k], XYZW},
                  Pred::None, true);
            bool tested = false;
            for (uint8_t chan = 0; chan < 2; chan++) {
                if (chan == 0 ? border.enable_s : border.enable_t) {
                    e.Test(Cond::Lt, 1, {I0}, chan, {tap_coord[k], XYZW}, false,
                           tested ? Pred::P1 : Pred::None, TestAlu::Mul);
                    tested = true;
                }
            }
        }
        e.UnpackF16x4({I1, 0xf}, texel_of[k]);
        e.Dot({I0, 0x1}, {I1, XYZW}, {I2, XYZW}, true, Pred::None, true);
        e.Test(Cond::Ge, 2, {weights}, 0, {I0, XXXX}, true);
        e.Vec(VecOp::Add, {results, result_lane[k]}, {I1, XXXX}, ConstZero, Pred::P2, true);
        if (bordered) {
            e.Vec(VecOp::Add, {results, result_lane[k]}, {border_result, XXXX}, ConstZero,
                  Pred::NotP1, true);
        }
    }
    // A border side (ClampToBorder) reads the border colour past the texture: a tap whose point
    // is outside [0, 1] on that axis takes the border's result. The taps lie on two columns and
    // two rows, so the four of them are tested at once: m = clamp(min(c, 1 - c) * 65536, 0, 1)
    // is 1 for a centre inside and 0 outside (a centre is at least half a texel, 1 / 2048, from
    // 0 and 1), over (x0, x1, y0, y1); a tap is inside where its column and its row are, and its
    // result is the border's where it is not, exact on whole numbers.
    if (bordered) {
        // In the internal registers: a four-wide second source of the unified store reads one
        // pair.
        constexpr Swizzle XXYY{Ch::X, Ch::X, Ch::Y, Ch::Y};
        constexpr Swizzle Step{Ch::Zero, Ch::X, Ch::Zero, Ch::Y};
        e.Vec(VecOp::Add, {I1, 0xf}, {inv_size.reg, Step}, {tap0, XXYY}, Pred::None, true);
        e.Vec(VecOp::Add, {I0, 0xf}, {I1, XYZW, Mod::Neg}, ConstOne, Pred::None, true);
        e.Vec(VecOp::Min, {I0, 0xf}, {I1, XYZW}, {I0, XYZW}, Pred::None, true);
        e.Vec(VecOp::Mul, {I0, 0xf}, {I0, XYZW}, Lit(Lit65536), Pred::None, true);
        e.Vec(VecOp::Max, {I0, 0xf}, {I0, XYZW}, ConstZero, Pred::None, true);
        e.Vec(VecOp::Min, {I0, 0xf}, {I0, XYZW}, ConstOne, Pred::None, true);
        // The results' lanes: (0, 0), (0, 1), (1, 0), (1, 1), the column first.
        Src inside{I0, XXYY};
        if (border.enable_s && border.enable_t) {
            e.Vec(VecOp::Mul, {I2, 0xf}, {I0, XXYY}, {I0, ZWZW}, Pred::None, true);
            inside = {I2, XYZW};
        } else if (border.enable_t) {
            inside = {I0, ZWZW};
        }
        // border - (border - result) * inside.
        e.Vec(VecOp::Add, {I1, 0xf}, {results, XYZW, Mod::Neg}, {border_result, XXXX}, Pred::None,
              true);
        e.Vec(VecOp::Mul, {I1, 0xf}, {I1, XYZW}, inside, Pred::None, true);
        e.Vec(VecOp::Add, {results, 0xf}, {I1, XYZW, Mod::Neg}, {border_result, XXXX}, Pred::None,
              true);
    }
    // Along x: (row 0, row 1) = (x, y) + ((z, w) - (x, y)) * f.x; then along y; each lerp's
    // product and sum one two-wide multiply-add over the stored pairs (TmpShadowTap1's, the
    // taps' texels read by then). Then to 0..1, into where it is read.
    const Reg vals = Reg::V(TmpShadowVal), vals_hi = Reg::V(TmpShadowVal + 2);
    const Reg rows = Reg::V(TmpShadowTap1), lerp = Reg::V(TmpShadowTap1 + 2);
    e.Vec(VecOp::Add, {rows, 0x3}, {vals, XYZW, Mod::Neg}, {vals_hi, XYZW}, Pred::None, true);
    e.Mad2({rows, 0x3}, {rows, XYZW}, {fract, XXXX}, {vals, XYZW});
    e.Vec(VecOp::Add, {lerp, 0x1}, {rows, XXXX, Mod::Neg}, {rows, YYYY}, Pred::None, true);
    e.Mad2({lerp, 0x1}, {lerp, XXXX}, {fract, YYYY}, {rows, XXXX});
    const Src value{lerp, XXXX};
    if (shadow_direct) {
        // The lighting's scalar alone (shadow_scalar), or 1 - it: no stage reads the texel.
        const Reg shadow = Reg::V(TmpShadow);
        if (config.lighting.shadow_invert) {
            e.Vec(VecOp::Mul, {I2, 0x1}, value, Lit(LitInv255), Pred::None, true);
            e.Vec(VecOp::Add, {shadow, 0x1}, {I2, XXXX, Mod::Neg}, ConstOne, Pred::None, true);
        } else {
            e.Vec(VecOp::Mul, {shadow, 0x1}, value, Lit(LitInv255), Pred::None, true);
        }
        return;
    }
    // The texel, the value in every lane: the same product into both pairs.
    e.Vec(VecOp::Mul, {Reg::V(TmpTex0), 0x3}, value, Lit(LitInv255), Pred::None, true);
    e.Vec(VecOp::Mul, {Reg::V(TmpTex0 + 2), 0x3}, value, Lit(LitInv255), Pred::None, true);
    texel_loc[0] = {Bank::Virtual, TmpTex0, false};
}

void FragmentEmitter::ShadowWrite(const Reg* u8) {
    // UpdateShadow on the tile's word, (depth << 8) | intensity as the PBE keeps the
    // PICA's RGBA8 (R the low byte: the intensity, then the depth's low, middle and high byte):
    // a fragment nearer than the depth there moves it when its intensity (the combiner output's
    // green) is zero, else lowers the intensity to s / (constant + linear * d / ref_d). In F32
    // on whole numbers, which the 24-bit depths are exactly.
    for (uint8_t b = 0; b < 4; b++) {
        e.UnpackInt(Reg::V(static_cast<uint8_t>(TmpShadowOld + b)), Reg::O(0), b, 0);
    }
    const Reg old_lo = Reg::V(TmpShadowOld);     // (ref_s, lo)
    const Reg old_hi = Reg::V(TmpShadowOld + 2); // (mid, hi)
    const Reg val = Reg::V(TmpShadowVal);        // (d, s)
    const Reg ref = Reg::V(TmpShadowVal + 2);    // (ref_d, -)
    if (u8) {
        e.UnpackInt(Reg::V(TmpShadowVal + 1), *u8, 1, 0);
    } else {
        e.Vec(VecOp::Mul, {I0, 0x1}, {Reg::V(TmpOutput), YYYY}, Lit(LitTwoFiftyFive), Pred::None,
              true);
        e.Vec(VecOp::Add, {I0, 0x1}, {I0, XXXX}, ConstHalf, Pred::None, true);
        e.Vec(VecOp::Frc, {I0, 0x1}, ConstZero, {I0, XXXX}, Pred::None, true);
        e.Vec(VecOp::Add, {val, 0x2}, {I0, XXXX, Mod::Neg}, ConstZero, Pred::None, true);
    }
    // d = floor(clamp(depth, 0, 1) * 0xffffff): WPOS z, z / w with W-buffering.
    const Reg zw = Reg::Pa(static_cast<uint8_t>(position_pa + 2));
    if (shadow_wbuffer) {
        e.Comp(CompOp::Rcp, {I0, 0x1}, {zw}, 1, Pred::None, true);
        e.Vec(VecOp::Mul, {I0, 0x1}, {zw, XXXX}, {I0, XXXX}, Pred::None, true);
    } else {
        e.Vec(VecOp::Add, {I0, 0x1}, {zw, XXXX}, ConstZero, Pred::None, true);
    }
    Clamp01(I0, 0x1);
    e.Vec(VecOp::Mul, {I0, 0x1}, {I0, XXXX}, Lit(LitDepthMax), Pred::None, true);
    e.Vec(VecOp::Frc, {I0, 0x1}, ConstZero, {I0, XXXX}, Pred::None, true);
    e.Vec(VecOp::Add, {val, 0x1}, {I0, XXXX, Mod::Neg}, ConstZero, Pred::None, true);
    // ref_d = 65536 hi + 256 mid + lo.
    e.Vec(VecOp::Mul, {I0, 0x1}, {old_hi, YYYY}, Lit(Lit65536), Pred::None, true);
    e.Vec(VecOp::Mul, {I1, 0x1}, {old_hi, XXXX}, Lit(Lit256), Pred::None, true);
    e.Vec(VecOp::Add, {I0, 0x1}, {I0, XXXX}, {I1, XXXX}, Pred::None, true);
    e.Vec(VecOp::Add, {ref, 0x1}, {I0, XXXX}, {old_lo, YYYY}, Pred::None, true);
    // s' = min(clamp(floor(s / (constant + linear * d / ref_d)), 0, 255), ref_s).
    e.Comp(CompOp::Rcp, {I0, 0x1}, {ref}, 0, Pred::None, true);
    e.Vec(VecOp::Mul, {I0, 0x1}, {I0, XXXX}, {val, XXXX}, Pred::None, true);
    e.Vec(VecOp::Mul, {I0, 0x1}, {I0, XXXX}, SaScalar(sa_shadow_linear), Pred::None, true);
    e.Vec(VecOp::Add, {I0, 0x1}, {I0, XXXX}, SaScalar(sa_shadow_constant), Pred::None, true);
    e.Comp(CompOp::Rcp, {I1, 0x1}, {I0}, 0, Pred::None, true);
    e.Vec(VecOp::Mul, {I1, 0x1}, {I1, XXXX}, {val, YYYY}, Pred::None, true);
    e.Vec(VecOp::Frc, {I1, 0x1}, ConstZero, {I1, XXXX}, Pred::None, true);
    e.Vec(VecOp::Add, {I1, 0x1}, {I1, XXXX, Mod::Neg}, ConstZero, Pred::None, true);
    e.Vec(VecOp::Max, {I1, 0x1}, {I1, XXXX}, ConstZero, Pred::None, true);
    e.Vec(VecOp::Min, {I1, 0x1}, {I1, XXXX}, Lit(LitTwoFiftyFive), Pred::None, true);
    e.Vec(VecOp::Min, {I1, 0x1}, {I1, XXXX}, {old_lo, XXXX}, Pred::None, true);
    // P1: nearer (d < ref_d); P2: no intensity (s == 0).
    e.Test(Cond::Lt, 1, {val}, 0, {ref, XXXX}, false);
    e.Test(Cond::Eq, 2, {val}, 1, ConstZero, true);
    // The intensity: ref_s, or s' for a nearer fragment with one.
    e.Vec(VecOp::Add, {I1, 0x1}, {old_lo, XXXX}, ConstZero, Pred::P2, true);
    e.Vec(VecOp::Add, {I2, 0x1}, {old_lo, XXXX}, ConstZero, Pred::None, true);
    e.Vec(VecOp::Add, {I2, 0x1}, {I1, XXXX}, ConstZero, Pred::P1, true);
    // The depth: ref_d, or d for a nearer fragment without one.
    e.Vec(VecOp::Add, {I1, 0x2}, {ref, XXXX}, ConstZero, Pred::None, true);
    e.Vec(VecOp::Add, {I1, 0x2}, {val, XXXX}, ConstZero, Pred::P2, true);
    e.Vec(VecOp::Add, {I2, 0x2}, {ref, XXXX}, ConstZero, Pred::None, true);
    e.Vec(VecOp::Add, {I2, 0x2}, {I1, YYYY}, ConstZero, Pred::P1, true);
    // Its bytes: hi = floor(D / 65536), m = D - 65536 hi, mid = floor(m / 256), lo = m - 256 mid.
    e.Vec(VecOp::Mul, {I0, 0x1}, {I2, YYYY}, Lit(LitInv65536), Pred::None, true);
    e.Vec(VecOp::Frc, {I0, 0x1}, ConstZero, {I0, XXXX}, Pred::None, true); // -hi
    e.Vec(VecOp::Mul, {I0, 0x2}, {I0, XXXX}, Lit(Lit65536), Pred::None, true);
    e.Vec(VecOp::Add, {I0, 0x2}, {I0, YYYY}, {I2, YYYY}, Pred::None, true); // m
    e.Vec(VecOp::Mul, {I0, 0x4}, {I0, YYYY}, Lit(LitInv256), Pred::None, true);
    e.Vec(VecOp::Frc, {I0, 0x4}, ConstZero, {I0, ZZZZ}, Pred::None, true); // -mid
    e.Vec(VecOp::Mul, {I0, 0x8}, {I0, ZZZZ}, Lit(Lit256), Pred::None, true);
    e.Vec(VecOp::Add, {I0, 0x8}, {I0, WWWW}, {I0, YYYY}, Pred::None, true); // lo
    // (s', lo, mid, hi), each (b + 1/4) / 255: the U8 pack keeps b whether it rounds or truncates.
    const Src quarter = Lit(LitQuarter);
    e.Vec(VecOp::Add, {I1, 0x1}, {I2, XXXX}, quarter, Pred::None, true);
    e.Vec(VecOp::Add, {I1, 0x2}, {I0, WWWW}, quarter, Pred::None, true);
    e.Vec(VecOp::Add, {I1, 0x4}, {I0, ZZZZ, Mod::Neg}, quarter, Pred::None, true);
    e.Vec(VecOp::Add, {I1, 0x8}, {I0, XXXX, Mod::Neg}, quarter, Pred::None, true);
    e.Vec(VecOp::Mul, {I1, 0xf}, {I1, XYZW}, Lit(LitInv255), Pred::None, true);
    e.PackU8(Reg::Pa(0), I1, I1);
    program.output_u8 = true;
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
    // Gas, the fog stage's half of gas rendering: (0, 0, 0, 0), the blend going on from it (a
    // transparent pixel, not a discard).
    if (config.texture.fog_mode == TexturingRegs::FogMode::Gas) {
        e.Vec(VecOp::Add, {Reg::V(TmpOutput), 0x3}, ConstZero, ConstZero);
        e.Vec(VecOp::Add, {Reg::V(TmpOutput + 2), 0x3}, ConstZero, ConstZero);
        return;
    }
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
    // The depth is WPOS's z; with W-buffering the PICA's W depth (z / w * scale + offset, times
    // w), TEXCOORD3's u iterated perspective-correct.
    const Reg pos = w_buffered ? Reg::Pa(fog_coord_pa) : Reg::Pa(position_pa + 2);
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

void FragmentEmitter::SampleTextures(uint32_t units) {
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
        if (!(units >> unit & 1)) {
            sampler_index += unit_issued[unit] ? 0 : 1; // the other call's read
            continue;
        }
        const int coord = (unit == 2 && tex.texture2_use_coord1) ? 1 : static_cast<int>(unit);
        coords[unit] = Reg::Pa(texcoord_pa[coord]); // not iterated for an issued unit alone
        if (unit == 0 && shadow_texture) {
            // Its four reads wait for themselves; the other units' reads follow.
            ShadowTexture(coords[0], sampler_index);
            sampler_index++;
            continue;
        }
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
        if (unit_used[unit] && (units >> unit & 1) && (config.texture.alpha_split >> unit & 1)) {
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
    // A coordinate past [0, 1] on an axis that clamps to the border colour replaces the texel with
    // it. (u - 1) * u is above 0 there and only there (u - 1 is exact from 0.5 up and negative
    // below 1), so u - 1 for both axes and the test unit's product with u an axis, folded by AND
    // into p1, say "inside"; the colour moves in where not. An F16 texel takes the F16 colour's
    // pair by two bitwise moves (F16 bits are no F32 values for a float move), an F32 one the F32
    // colour's two pairs.
    for (u32 unit = 0; unit < 3; unit++) {
        const auto& border = tex.texture_border_color[unit];
        // A shadow read is lit (1) past its texture's edge instead (ShadowTexture).
        if (!unit_used[unit] || !(units >> unit & 1) || !(border.enable_s || border.enable_t) ||
            (unit == 0 && shadow_texture)) {
            continue;
        }
        e.Vec(VecOp::Add, {I0, 0x3}, {ConstOne.reg, XXXX, Mod::Neg}, {coords[unit], XYZW},
              Pred::None, true);
        bool tested = false;
        for (uint8_t chan = 0; chan < 2; chan++) {
            if (chan == 0 ? border.enable_s : border.enable_t) {
                e.Test(Cond::Le, 1, {I0}, chan, {coords[unit], XYZW}, false,
                       tested ? Pred::P1 : Pred::None, TestAlu::Mul);
                tested = true;
            }
        }
        const Vec4Loc texel = texel_loc[unit];
        if (texel.f16) {
            for (uint8_t word = 0; word < 2; word++) {
                const Reg dst{texel.bank, static_cast<uint8_t>(texel.base + word)};
                e.Bitwise(BwOp::Or, dst,
                          IOp::R(Reg::Sa(static_cast<uint8_t>(sa_border16 + unit * 2 + word))),
                          IOp::Imm(0), Pred::NotP1);
            }
        } else {
            const Vec4Loc colour{Bank::SecAttr, static_cast<uint8_t>(sa_border + unit * 4)};
            e.MoveInternal({texel.Pair(0), 0x3}, {colour.Pair(0), XYZW}, Pred::NotP1, true);
            e.MoveInternal({texel.Pair(1), 0x3}, {colour.Pair(1), XYZW}, Pred::NotP1, true);
        }
    }
    if (uses_shadow && (units >> config.lighting.shadow_selector & 1)) {
        // shadow = tex, or 1 - tex when inverted.
        const auto& lighting = config.lighting;
        const Vec4Loc texel = texel_loc[lighting.shadow_selector];
        const Vec4Loc shadow{Bank::Virtual, TmpShadow};
        if (shadow_direct) {
            // ShadowTexture wrote it
        } else if (shadow_scalar) {
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

void FragmentEmitter::LightingLutLookup(Sampler sampler, Src input, bool abs_input, bool two_sided,
                                        Dst into, const Src* scale, bool pair_free) {
    // Unsigned: 256 steps over [0, 1]; signed: 128 steps over [-1, 1] with negatives wrapped
    // to the top half of the table. The filtered F16 rows: a linear read's texel-space x is
    // u * 256 - 0.5, so u = input + 0.5 / 256 puts it at the position, input * 256, and the
    // texture unit interpolates between the entries there (with the next entry's value where
    // entry.g is the delta, and 6-bit weights: docs/sgxd/tsp.md). The clamp takes a one-sided
    // input's negatives to entry 0, and 1 and past it to entry 255. The distance's read makes
    // its own x (Lighting).
    const Reg coord = Reg::V(TmpLutCoord);
    // With the LUT's (offset or bound, row) pair (PlanUniforms) the row rides in lane y:
    // a first source's swizzle lane can be a constant (docs/sgxd/usse.md), so (input, 0) + (0.5
    // / 256, row) and min((x, 2), (bound, row)) make both coordinates at once.
    const uint32_t pair = sa_lut_coord[static_cast<uint32_t>(sampler)];
    const bool fold = pair != UINT32_MAX;
    DEBUG_ASSERT((pair & 1) == 0 || !fold);
    const Src pair_src{Reg::Sa(static_cast<uint8_t>(pair)), XYZW};
    if (abs_input) {
        const Mod mod = two_sided ? Mod::Abs : Mod::None;
        if (fold) {
            e.Vec(VecOp::Add, {coord, 0x3},
                  {input.reg, {input.swz[0], Ch::Zero, Ch::Zero, Ch::Zero}, mod}, pair_src);
        } else {
            e.Vec(VecOp::Add, {coord, 0x1}, {input.reg, input.swz, mod}, Lit(LitHalfInv256));
        }
    } else {
        // Signed: 128 entries a unit, the negatives in the table's top half. The rows
        // repeat in u (LutLfHalfWrap), so u = input / 2 + 0.5 / 256 wraps a negative input
        // there, and -1/128 interpolates into entry 0 as the PICA's delta does. A positive
        // input stops at entry 127's centre: past it the next texel is -1's entry, where
        // the PICA reads entry 127 and its delta.
        // The bound's pair: x min the bound, 2 min the row (a row centre is below 1).
        const Swizzle x2{Ch::X, Ch::Two, Ch::Two, Ch::Two};
        if (input.reg.bank != Bank::Internal) {
            e.Mad2({coord, 0x1}, input, Lit(LitHalf), Lit(LitHalfInv256));
            if (fold) {
                e.Vec(VecOp::Min, {coord, 0x3}, {coord, x2}, pair_src);
            } else {
                e.Vec(VecOp::Min, {coord, 0x1}, {coord, XYZW}, Lit(LitLutWrapLast));
            }
        } else {
            e.Vec(VecOp::Mul, {I0, 0x1}, input, Lit(LitHalf), Pred::None, true);
            e.Vec(VecOp::Add, {I0, 0x1}, {I0, XYZW}, Lit(LitHalfInv256), Pred::None, true);
            if (fold) {
                e.Vec(VecOp::Min, {coord, 0x3}, {I0, x2}, pair_src);
            } else {
                e.Vec(VecOp::Min, {coord, 0x1}, {I0, XYZW}, Lit(LitLutWrapLast));
            }
        }
    }
    LightingLutRowFetch(sampler, abs_input, into, scale, pair_free, fold);
}

void FragmentEmitter::LightingLutRowFetch(Sampler sampler, bool abs_input, Dst into,
                                          const Src* scale, bool pair_free, bool row_set) {
    // The LUT is one whole row of the lf texture (lighting_lut_offset is row * 256): the row's
    // v comes from the CPU (UniformKind::LightingLutRow).
    const uint32_t sampler_index = static_cast<uint32_t>(sampler);
    DEBUG_ASSERT_MSG(sa_lut_row[sampler_index] != UINT32_MAX,
                     "lighting: LUT {} read without its row planned (PlanUniforms)", sampler_index);
    const Reg coord = Reg::V(TmpLutCoord);
    // TmpLutCoord.x holds the position (LightingLutLookup); the row's v, the read, the value.
    if (!row_set) {
        e.Vec(VecOp::Add, {coord, 0x2}, SaScalar(sa_lut_row[sampler_index]), ConstZero, Pred::None,
              true);
    }
    const uint8_t row_sampler = SamplerSa(abs_input ? sampler_lut_half : sampler_lut_wrap);
    const auto read = [&](Reg dst) {
        e.Sample2DPoint(dst, coord, row_sampler, 0, 2);
        e.Wdf(0);
    };
    // The F16 rows have one channel: the read writes one F32 register, the first of a
    // 64-bit-aligned pair. Into an even lane with nothing in its neighbour it lands where it is
    // read, without the move.
    if (pair_free && scale == nullptr && (into.mask == 0x1 || into.mask == 0x4)) {
        const uint8_t at = static_cast<uint8_t>(into.reg.n + (into.mask == 0x4 ? 2 : 0));
        DEBUG_ASSERT(into.reg.bank == Bank::Virtual && (at & 1) == 0);
        read(Reg::V(at));
        light_mem_pending = false;
        return;
    }
    read(Reg::V(TmpLutEntry));
    light_mem_pending = false; // the wait is for every read on the counter
    if (scale != nullptr) {
        e.Vec(VecOp::Mul, into, {Reg::V(TmpLutEntry), XXXX}, *scale);
    } else {
        e.Vec(VecOp::Add, into, {Reg::V(TmpLutEntry), XXXX}, ConstZero);
    }
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
    const Vec4Loc half_vector{Bank::Virtual, TmpHalf};
    const Reg dot_product = Reg::V(TmpMisc);   // .x; .y the squared half vector length
    const Reg misc_hi = Reg::V(TmpMisc + 2);   // .x the attenuation, .y light distance
    const bool config7 = lighting.config == LightingRegs::LightingConfig::Config7;

    // For the U8 combiners the sums are packed at the end, the pack saturating as the clamp does
    // (2.0 and -1.0 pack to 0xff and 0x00), unless a moved channel of one is packed again from its
    // F32 lanes (U8Inputs' broadcasts). Packed, a sum's alpha byte is read only by an argument
    // taking its alpha.
    bool lanes_moved = false;
    bool primary_alpha_read = !u8_tev;
    bool secondary_alpha_read = !u8_tev;
    // The secondary colour's colour lanes the arguments read (bit per lane).
    uint32_t secondary_lanes = u8_tev ? 0 : 0x7;
    if (u8_tev) {
        ForEachU8Arg([&](u32, Source csrc, ColorModifier cmod, Source asrc, AlphaModifier amod) {
            const auto lit = [](Source s) {
                return s == Source::PrimaryFragmentColor || s == Source::SecondaryFragmentColor;
            };
            lanes_moved = lanes_moved || (lit(csrc) && MovedLane(cmod) >= 0) ||
                          (lit(asrc) && MovedLane(amod) >= 0);
            const bool c_alpha =
                cmod == ColorModifier::SourceAlpha || cmod == ColorModifier::OneMinusSourceAlpha;
            const bool a_alpha =
                amod == AlphaModifier::SourceAlpha || amod == AlphaModifier::OneMinusSourceAlpha;
            if (csrc == Source::SecondaryFragmentColor && !c_alpha) {
                secondary_lanes |= MovedLane(cmod) >= 0 ? 1u << MovedLane(cmod) : 0x7u;
            }
            if (asrc == Source::SecondaryFragmentColor && MovedLane(amod) >= 0) {
                secondary_lanes |= 1u << MovedLane(amod);
            }
            for (const Source lit_src :
                 {Source::PrimaryFragmentColor, Source::SecondaryFragmentColor}) {
                if ((csrc == lit_src && c_alpha) || (asrc == lit_src && a_alpha)) {
                    (lit_src == Source::PrimaryFragmentColor ? primary_alpha_read
                                                             : secondary_alpha_read) = true;
                }
            }
        });
    }
    u8_lighting_packed = u8_tev && !lanes_moved;
    primary_alpha_read = primary_alpha_read || !u8_lighting_packed;
    secondary_alpha_read = secondary_alpha_read || !u8_lighting_packed;
    // One colour lane of the secondary read (specular.rrr): the specular sum is that lane's, a
    // light's term stored or added in one instruction instead of two. The other lanes stay
    // unwritten: only U8Inputs' packs read the sum, and a lane no argument takes is a byte no SOP
    // reads.
    const int spec_lane = u8_tev && std::has_single_bit(secondary_lanes)
                              ? std::countr_zero(secondary_lanes)
                              : -1;

    const bool fresnel_read = lighting.src_num > 0 &&
                              LightingLutEnabled(lighting.lut_fr, Sampler::Fresnel);
    // One light on the U8 combiners: a sum is its light's term, packed from the internal
    // register it ends in (no quad, no moves). The diffuse's ambient add makes its alpha lane
    // 1 + 1, which packs to 0xff as 1.0 does (the pack saturates); the specular's alpha is set
    // where it is read. Not when the fresnel read or the shadow writes a sum's alpha after it.
    const bool shadow_alpha_written = uses_shadow && lighting.shadow_alpha;
    const bool direct_primary =
        u8_lighting_packed && lighting.src_num == 1 && uses_primary_color &&
        !(lighting.enable_primary_alpha && (fresnel_read || shadow_alpha_written));
    const bool direct_secondary =
        u8_lighting_packed && lighting.src_num == 1 && uses_secondary_color &&
        !(lighting.enable_secondary_alpha && (fresnel_read || shadow_alpha_written));

    // diffuse_sum = specular_sum = (0, 0, 0, 1): the alpha here, unless the last light's
    // fresnel read writes it or nothing reads it, the colour by the first light's store (or
    // zero below when there is none).
    if (uses_primary_color && primary_alpha_read && !direct_primary &&
        !(fresnel_read && lighting.enable_primary_alpha)) {
        e.Vec(VecOp::Add, {diffuse_sum.Pair(1), 0x2}, ConstZero, {Reg::C(1), XYZW});
    }
    if (uses_secondary_color && secondary_alpha_read && !direct_secondary &&
        !(fresnel_read && lighting.enable_secondary_alpha)) {
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
    // The first light's diffuse goes in with the global ambient added; a packed sum is packed
    // where it lies.
    const Reg global_ambient = Reg::Sa(static_cast<uint8_t>(sa_global_ambient));
    bool ambient_added = false;
    const auto store3_ambient = [&](Vec4Loc to, Reg internal) {
        e.Vec(VecOp::Add, {to.Pair(0), 0x3}, {global_ambient, XYZW}, {internal, XYZW}, Pred::None,
              true);
        e.Vec(VecOp::Add, {to.Pair(1), 0x1}, {global_ambient, ZZZZ}, {internal, ZZZZ}, Pred::None,
              true);
        ambient_added = true;
    };
    const auto accumulate3 = [&](Vec4Loc to, Reg internal, Pred pred = Pred::None) {
        e.Vec(VecOp::Add, {to.Pair(0), 0x3}, {to.Pair(0), XYZW}, {internal, XYZW}, pred, true);
        e.Vec(VecOp::Add, {to.Pair(1), 0x1}, {to.Pair(1), XYZW}, {internal, ZZZZ}, pred, true);
    };
    const auto store1 = [&](Vec4Loc to, Reg internal, int lane) {
        const uint8_t mask = static_cast<uint8_t>(1u << (lane & 1));
        e.MoveInternal({to.Pair(lane >> 1), mask}, {internal, lane < 2 ? XYZW : ZWZW}, Pred::None,
                       true);
    };
    const auto accumulate1 = [&](Vec4Loc to, Reg internal, int lane, Pred pred = Pred::None) {
        const Reg pair = to.Pair(lane >> 1);
        const uint8_t mask = static_cast<uint8_t>(1u << (lane & 1));
        e.Vec(VecOp::Add, {pair, mask}, {pair, XYZW}, {internal, lane < 2 ? XYZW : ZZZZ}, pred,
              true);
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
    // z = sqrt(max(1 - (x*x + y*y), 0)) as d * rsq(d) with d at least the smallest normal float:
    // rsq(0) is inf and 0 * inf nan; the floor's z (about 1e-19) is 0 to the lighting.
    const auto renorm = [&](Reg into, Reg scratch) {
        e.Vec(VecOp::Mul, {scratch, 0x3}, {into, XYZW}, {into, XYZW}, Pred::None, true);
        e.Vec(VecOp::Add, {scratch, 0x1}, {scratch, XYZW}, {scratch, YYYY}, Pred::None, true);
        e.Vec(VecOp::Add, {scratch, 0x1}, {scratch, XYZW, Mod::Neg}, ConstOne, Pred::None, true);
        e.Vec(VecOp::Max, {scratch, 0x1}, {scratch, XYZW}, Lit(LitMinNormal), Pred::None, true);
        e.Comp(CompOp::Rsq, {scratch, 0x2}, {scratch}, 0, Pred::None, true);
        e.Vec(VecOp::Mul, {into, 0x4}, {scratch, XXXX}, {scratch, YYYY}, Pred::None, true);
    };
    // A normal map without the tangent is perturbed after the quaternion, into i1, and rotated
    // there (rotate's v): no park and reload.
    const bool normal_in_place =
        lighting.bump_mode == LightingRegs::LightingBumpMode::NormalMap && !config7;
    switch (lighting.bump_mode) {
    case LightingRegs::LightingBumpMode::NormalMap:
        if (!normal_in_place) {
            perturbation(I0);
            if (lighting.bump_renorm) {
                renorm(I0, I1);
            }
            default_tangent();
        }
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
    if (!default_normal_only && !normal_in_place) {
        StoreVec4(normal, I0);
    }
    if (config7) {
        StoreVec4(tangent, I1);
    }

    // quat = normalize(normquat); the rotation is the same for -q, so the flat copy's sign is not
    // needed.
    LoadVec4(I0, normquat);
    e.Dot({I2, 0x1}, {I0, XYZW}, {I0, XYZW}, true, Pred::None, true);
    e.Comp(CompOp::Rsq, {I2, 0x1}, {I2}, 0, Pred::None, true);
    e.Vec(VecOp::Mul, {I0, 0xf}, {I0, XYZW}, {I2, XXXX}, Pred::None, true);
    // rotate(q, v) = v + 2 * cross(q.xyz, cross(q.xyz, v) + q.w * v), for the normal and
    // the tangent; i0 = q throughout.
    const auto rotate = [&](Vec4Loc v, bool loaded = false) {
        // i0 = q and i1 = v throughout (`loaded`: v is in i1 already); mad's sources (the first
        // with any swizzle and a negation, the internal ones with the extended table) keep v
        // from reloads.
        if (!loaded) {
            LoadVec4(I1, v);
        }
        constexpr Swizzle YZX{Ch::Y, Ch::Z, Ch::X, Ch::W};
        constexpr Swizzle ZXY{Ch::Z, Ch::X, Ch::Y, Ch::W};
        // i2 = t = cross(q, v) + q.w * v = q.yzx * v.zxy - q.zxy * v.yzx + v * q.w
        e.Vec(VecOp::Mul, {I2, 0x7}, {I0, YZX}, {I1, ZXY}, Pred::None, true);
        e.Mad({I2, 0x7}, {I0, ZXY, Mod::Neg}, {I1, YZX}, {I2, XYZW}, true, Pred::None, true);
        e.Mad({I2, 0x7}, {I1, XYZW}, {I0, WWWW}, {I2, XYZW}, true, Pred::None, true);
        // result = v + 2 * cross(q, t) = v + q.yzx * (2t).zxy - q.zxy * (2t).yzx, into i1
        e.Vec(VecOp::Mul, {I2, 0x7}, {I2, XYZW}, Lit(LitTwo), Pred::None, true);
        e.Mad({I1, 0x7}, {I0, YZX}, {I2, ZXY}, {I1, XYZW}, true, Pred::None, true);
        e.Mad({I1, 0x7}, {I0, ZXY, Mod::Neg}, {I2, YZX}, {I1, XYZW}, true, Pred::None, true);
        StoreVec4(v, I1);
    };
    // Where i2 starts to hold the default normal (stored, and kept in i2 from there): SIZE_MAX
    // for none.
    std::size_t normal_i2_from = SIZE_MAX;
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
        normal_i2_from = e.Count();
    } else if (normal_in_place) {
        perturbation(I1);
        if (lighting.bump_renorm) {
            renorm(I1, I2);
        }
        rotate(normal, true);
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
    bool reads_nv = false; ///< a lookup reads N.V: made once, with the view (TmpLutShared.z)
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
                reads_nv = true;
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
        if (reads_nv) {
            e.Dot({Reg::V(TmpLutShared + 2), 0x1}, {normal.Pair(0), XYZW}, {I0, XYZW}, false,
                  Pred::None, true);
        }
    }

    // The lanes of TmpLutInput holding the light's N.H, V.H, L.N and spot inputs (-1: not
    // read), set per light where its vectors are made.
    std::array<int8_t, 6> input_lane{};
    input_lane.fill(-1);
    const auto input_dst = [&](LutInput input) -> Dst {
        const int lane = input_lane[static_cast<u32>(input)];
        return {Reg::V(static_cast<uint8_t>(TmpLutInput + (lane & 2))),
                static_cast<uint8_t>(1u << (lane & 1))};
    };
    // A LUT input: the light's from its lane, N.V and C.P made into i0.x from the vectors
    // parked in temps.
    const auto lut_input = [&](LutInput input) -> Src {
        if (input_lane[static_cast<u32>(input)] >= 0) {
            const Dst at = input_dst(input);
            return {at.reg, at.mask == 0x2 ? YYYY : XXXX};
        }
        switch (input) {
        case LutInput::NV:
            // made once with the view
            return {Reg::V(TmpLutShared + 2), XXXX};
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
            UNREACHABLE_MSG("lighting: LUT input {} without its lane", static_cast<u32>(input));
        }
        return {I0, XXXX};
    };
    // scale * LUT(input) into `into`, for an enabled LUT. Within a light the vectors stay put, so a
    // LUT read at the input the previous one indexed takes its index from that lookup.
    struct LutIndexKey {
        LutInput input;
        bool abs_input;
        bool two_sided;
        bool operator==(const LutIndexKey&) const = default;
    };
    std::optional<LutIndexKey> last_lut_index;
    // `pair_free`: LightingLutLookup's, the caller's word that `into`'s neighbour is free.
    const auto lut_value = [&](const Pica::Shader::LutConfig& lut, Sampler sampler, bool two_sided,
                               Dst into, bool pair_free = false) {
        const LutIndexKey key{lut.type.Value(), static_cast<bool>(lut.abs_input), two_sided};
        // Scales are powers of two between 0.25 and 8, made of one or two factors from the
        // constant bank; a filtered read takes the first on its value's move.
        const float scale = lut.GetScale();
        std::array<Src, 2> factors{};
        u32 factor_count = 0;
        if (scale == 2.0f) {
            factors = {Lit(LitTwo)};
            factor_count = 1;
        } else if (scale == 4.0f) {
            factors = {Lit(LitFour)};
            factor_count = 1;
        } else if (scale == 8.0f) {
            factors = {Lit(LitTwo), Lit(LitFour)};
            factor_count = 2;
        } else if (scale == 0.5f) {
            factors = {ConstHalf};
            factor_count = 1;
        } else if (scale == 0.25f) {
            factors = {ConstHalf, ConstHalf};
            factor_count = 2;
        } else {
            DEBUG_ASSERT_MSG(scale == 1.0f, "lighting: LUT scale {}", scale);
        }
        const Src* fetch_scale = factor_count > 0 ? &factors[0] : nullptr;
        if (last_lut_index == key) {
            // TmpLutCoord's x holds its position
            LightingLutRowFetch(sampler, key.abs_input, into, fetch_scale, pair_free);
        } else {
            LightingLutLookup(sampler, lut_input(key.input), key.abs_input, two_sided, into,
                              fetch_scale, pair_free);
            last_lut_index = key;
        }
        for (u32 k = fetch_scale != nullptr ? 1 : 0; k < factor_count; k++) {
            e.Vec(VecOp::Mul, into, {into.reg, XYZW}, factors[k]);
        }
    };

    // D0 or D1 read at N.V gives every light the same value (the normal and the view are no
    // light's): read once here, kept in TmpLutShared. The lights' two-sidedness picks the lookup's
    // form, so they must agree.
    const bool d0_shared = LutSharedAtNv(lighting.lut_d0, Sampler::Distribution0);
    const bool d1_shared = LutSharedAtNv(lighting.lut_d1, Sampler::Distribution1);
    const bool sides = lighting.src_num > 0 && lighting.lights[0].two_sided_diffuse;
    if (d0_shared) {
        // .y (D1's) is written after it
        lut_value(lighting.lut_d0, Sampler::Distribution0, sides, {Reg::V(TmpLutShared), 0x1},
                  true);
    }
    if (d1_shared) {
        lut_value(lighting.lut_d1, Sampler::Distribution1, sides, {Reg::V(TmpLutShared), 0x2});
    }

    for (u32 light_index = 0; light_index < lighting.src_num; light_index++) {
        const auto& light = lighting.lights[light_index];
        last_lut_index.reset(); // another light's vectors
        LightParams(light.num);
        const Bank pb = light_mem ? Bank::Virtual : Bank::SecAttr;
        // The light's terms: a zero colour's is left out (RasterizerSgxd::FragmentConfigFor),
        // with the table reads that only multiply it and the colour's quad.
        const bool spec0_on = LightSpecular0(light_index);
        const bool spec1_on = LightSpecular1(light_index);
        const bool ambient_on = !light.ambient_zero;
        const u32 quads_read = LightQuadsRead(light_index);
        const auto quad = [&](u32 q) {
            return (quads_read >> q & 1) != 0 ? LightQuad(q) : Vec4Loc{pb, 0};
        };
        const Vec4Loc position = quad(4);
        const Vec4Loc spot = quad(5);
        const Vec4Loc diffuse = quad(2);
        const Vec4Loc ambient = quad(3);
        const Vec4Loc specular_0 = quad(0);
        const Vec4Loc specular_1 = quad(1);
        const bool two_sided = light.two_sided_diffuse;
        const bool shadow_primary = uses_shadow && lighting.shadow_primary && light.shadow_enable;
        const bool shadow_secondary =
            uses_shadow && lighting.shadow_secondary && light.shadow_enable;
        const Vec4Loc shadow{Bank::Virtual, TmpShadow};
        // A memory light's quads: the position (the distance bias in its .w), the spot
        // direction, and with the distance attenuation the diffuse colour's, whose .w is the
        // scale.
        LightQuads(0x30 | (light.dist_atten_enable ? 0x4u : 0u), true);

        // The inputs this light's lookups read, a lane each for those read off its vectors.
        const u32 inputs = LightInputs(light_index);
        const auto reads = [&](LutInput input) {
            return (inputs >> static_cast<u32>(input) & 1) != 0;
        };
        input_lane.fill(-1);
        int8_t lanes = 0;
        for (const LutInput input : {LutInput::NH, LutInput::VH, LutInput::LN, LutInput::SP}) {
            if (reads(input)) {
                input_lane[static_cast<u32>(input)] = lanes++;
            }
        }

        const bool geo0 = light.geometric_factor_0 && spec0_on;
        const bool geo1 = light.geometric_factor_1 && spec1_on;
        const bool geo = geo0 || geo1;
        const bool halfway = geo || reads(LutInput::NH) || reads(LutInput::VH) || reads(LutInput::CP);
        // A directional light's vector with the default normal still in i2: L.N reads the vector
        // where its uniform lies, against i2, and so does the half vector's add, as its first
        // source (whose z and w lanes a unified-store read gives): no load into i0. Not with
        // the geometric factor or C.P (they keep the half vector's length or the vector), nor
        // the spot's input (a second dot with the vector).
        const bool light_in_sa = normal_i2_from != SIZE_MAX && pb == Bank::SecAttr &&
                                 LightPreNormalised(light) && !geo && !reads(LutInput::CP) &&
                                 !reads(LutInput::SP) && e.InternalKept(normal_i2_from, 2);
        // The light vector in i0 = position (+ view unless directional); distance; normalised. A
        // directional light's comes normalised in its uniform (LightPreNormalised): normalised in a
        // secondary program it drew a 32-pixel tile pattern over lit surfaces.
        if (!light_in_sa) {
            LoadVec4(I0, position);
        }
        if (!LightPreNormalised(light)) {
            // Four lanes, though w means nothing: every lane the store reads is then the
            // op's, so the loads fold into the ops (Lowering::LoadSources) instead of moves.
            if (!light.directional) {
                LoadVec4(I1, view);
                e.Vec(VecOp::Add, {I0, 0xf}, {I0, XYZW}, {I1, XYZW}, Pred::None, true);
            }
            e.Dot({I1, 0x1}, {I0, XYZW}, {I0, XYZW}, false, Pred::None, true);
            e.Comp(CompOp::Rsq, {I1, 0x2}, {I1}, 0, Pred::None, true);
            if (light.dist_atten_enable) {
                // .y = the distance, d * rsq(d)
                e.Vec(VecOp::Mul, {misc_hi, 0x2}, {I1, XXXX}, {I1, YYYY}, Pred::None, true);
            }
            e.Vec(VecOp::Mul, {I0, 0xf}, {I0, XYZW}, {I1, YYYY}, Pred::None, true);
        }
        // i0 holds the light vector from here on: the dots that read it are made now, so it is
        // never parked (nor the half vector, but for C.P).
        // half_vector = normalize(nview + light_vector) in i1; its squared length kept for the
        // geometric factor.
        if (halfway) {
            // light_in_sa: the length in i0 (free), so i2 keeps the normal.
            const Reg scratch = light_in_sa ? I0 : I2;
            LoadVec4(I1, nview);
            if (light_in_sa) {
                e.Vec(VecOp::Add, {I1, 0xf}, {position.Pair(0), XYZW}, {I1, XYZW}, Pred::None,
                      true);
            } else {
                e.Vec(VecOp::Add, {I1, 0xf}, {I1, XYZW}, {I0, XYZW}, Pred::None, true);
            }
            e.Dot({scratch, 0x1}, {I1, XYZW}, {I1, XYZW}, false, Pred::None, true);
            if (geo) {
                e.MoveInternal({dot_product, 0x2}, {scratch, XXXX}, Pred::None, true);
            }
            e.Comp(CompOp::Rsq, {scratch, 0x1}, {scratch}, 0, Pred::None, true);
            e.Vec(VecOp::Mul, {I1, 0xf}, {I1, XYZW}, {scratch, XXXX}, Pred::None, true);
            if (reads(LutInput::NH)) {
                e.Dot(input_dst(LutInput::NH), {normal.Pair(0), XYZW}, {I1, XYZW}, false,
                      Pred::None, true);
            }
            if (reads(LutInput::VH)) {
                e.Dot(input_dst(LutInput::VH), {nview.Pair(0), XYZW}, {I1, XYZW}, false, Pred::None,
                      true);
            }
            if (reads(LutInput::CP)) {
                StoreVec4(half_vector, I1);
            }
        }
        // dot(l, n), the L.N input; the spot's input dot(l, spot_dir).
        // light_in_sa: the value into i0 (holding nothing now), so i2 keeps the normal for the
        // next light.
        const Dst ln =
            reads(LutInput::LN) ? input_dst(LutInput::LN) : Dst{light_in_sa ? I0 : I2, 0x1};
        if (light_in_sa) {
            e.Dot(ln, {position.Pair(0), XYZW}, {I2, XYZW}, false, Pred::None, true);
        } else {
            e.Dot(ln, {normal.Pair(0), XYZW}, {I0, XYZW}, false, Pred::None, true);
        }
        if (reads(LutInput::SP)) {
            e.Dot(input_dst(LutInput::SP), {spot.Pair(0), XYZW}, {I0, XYZW}, false, Pred::None,
                  true);
        }
        // dot_product = two_sided ? |dot(l, n)| : max(dot(l, n), 0)
        const Src ln_value{ln.reg, ln.mask == 0x2 ? YYYY : XXXX};
        if (two_sided) {
            e.Vec(VecOp::Add, {dot_product, 0x1}, {ln_value.reg, ln_value.swz, Mod::Abs}, ConstZero,
                  Pred::None, true);
        } else {
            e.Vec(VecOp::Max, {dot_product, 0x1}, ln_value, ConstZero, Pred::None, true);
        }

        // geo_factor = hh == 0 ? 0 : min(dot_product / hh, 1), in `geo_reg`.y. The attenuation is
        // misc_hi.x, beside the distance it is made from (a pair of its own costs a register).
        const Reg geo_reg = Reg::V(TmpLightAtten);
        const Reg atten = misc_hi;
        if ((geo0 || geo1) && uses_secondary_color) {
            e.Comp(CompOp::Rcp, {I0, 0x2}, {dot_product}, 1, Pred::None, true);
            e.Vec(VecOp::Mul, {I0, 0x2}, {I0, YYYY}, {dot_product, XXXX}, Pred::None, true);
            e.Vec(VecOp::Min, {geo_reg, 0x2}, {I0, XYZW}, ConstOne);
            e.Test(Cond::Gt, 0, {dot_product}, 1, ConstZero, false);
            e.MoveInternal({geo_reg, 0x2}, {Reg::C(0), XXXX}, Pred::NotP0, true);
        }
        // Attenuation: spot and distance, multiplied into `atten`.x (no register at all when
        // neither is on). The distance's factor lands in `atten` itself, read after the
        // distance beside it (.y) is used; a spot factor before it waits in TmpLutValue.z,
        // whose .w is free.
        const bool spot_on = light.spot_atten_enable &&
                             LightingLutEnabled(lighting.lut_sp, Sampler::SpotlightAttenuation);
        const bool atten_on = spot_on || light.dist_atten_enable;
        const Reg spot_factor = light.dist_atten_enable ? Reg::V(TmpLutValue + 2) : atten;
        if (spot_on) {
            // `atten`.y is the distance only with the distance attenuation on
            lut_value(lighting.lut_sp, LightingRegs::SpotlightAttenuationSampler(light.num),
                      two_sided, {spot_factor, 0x1}, true);
        }
        if (light.dist_atten_enable) {
            // input = clamp(scale * distance + bias, 0, 1), unsigned lookup (the bias and
            // scale are the .w of quads 4 and 2, loaded with the light's position). They come
            // in the read's units (UniformSource::dist_atten): the bias has its half texel
            // added, so the read's x is one vmad2.
            const Sampler da_sampler = LightingRegs::DistanceAttenuationSampler(light.num);
            e.Mad2({Reg::V(TmpLutCoord), 0x1}, {misc_hi, YYYY}, Scalar(pb, diffuse.base + 3u),
                   Scalar(pb, position.base + 3u));
            LightingLutRowFetch(da_sampler, true, {atten, 0x1}, nullptr, true);
            last_lut_index.reset(); // TmpLutCoord's x holds the distance's position now
            if (spot_on) {
                e.Vec(VecOp::Mul, {atten, 0x1}, {atten, XYZW}, {spot_factor, XXXX});
            }
        }

        // A light with neither term adds nothing to the specular sum; the one light whose term
        // is the sum itself (direct_secondary) packs zero.
        if (uses_secondary_color && (spec0_on || spec1_on || direct_secondary)) {
        if (!spec0_on && !spec1_on) {
            e.Vec(VecOp::Add, {I1, 0x7}, ConstZero, ConstZero, Pred::None, true);
        } else {
            // The light's LUT values first, each into a lane of its own: refl = (rr ? LUT : 1, rg ?
            // LUT : refl.r, rb ? LUT : refl.r) in TmpLightAcc (no register when all three LUTs are
            // off: refl = 1), d0 and d1 in TmpLutValue (or TmpLutShared). A memory light's specular
            // quads load before the last fetch, whose wait covers them; its diffuse ones at the
            // diffuse term, when the specular quads are dead (keeping the register peak down).
            const Vec4Loc acc{Bank::Virtual, TmpLightAcc};
            const Reg values = Reg::V(TmpLutValue);
            const bool rr =
                spec1_on && LightingLutEnabled(lighting.lut_rr, Sampler::ReflectRed);
            const bool rg =
                spec1_on && LightingLutEnabled(lighting.lut_rg, Sampler::ReflectGreen);
            const bool rb =
                spec1_on && LightingLutEnabled(lighting.lut_rb, Sampler::ReflectBlue);
            const bool d1_on =
                spec1_on && LightingLutEnabled(lighting.lut_d1, Sampler::Distribution1);
            const bool d0_on =
                spec0_on && LightingLutEnabled(lighting.lut_d0, Sampler::Distribution0);
            const bool refl_on = rr || rg || rb;
            // rr alone: refl = (rr, rr, rr), multiplied in as a broadcast.
            const bool refl_scalar = rr && !rg && !rb;
            // An even lane's read before its odd neighbour's (rr before rg, d0 before d1), so it
            // lands in place (LightingLutRowFetch's pair_free); rb's neighbour is acc.w, unused.
            struct Fetch {
                const Pica::Shader::LutConfig* lut;
                Sampler sampler;
                Dst into;
                bool pair_free;
            };
            std::array<Fetch, 5> fetches{};
            u32 fetch_count = 0;
            if (rr) {
                fetches[fetch_count++] = {
                    &lighting.lut_rr, Sampler::ReflectRed, {acc.Pair(0), 0x1}, true};
            }
            if (rg) {
                fetches[fetch_count++] = {
                    &lighting.lut_rg, Sampler::ReflectGreen, {acc.Pair(0), 0x2}, false};
            }
            if (rb) {
                fetches[fetch_count++] = {
                    &lighting.lut_rb, Sampler::ReflectBlue, {acc.Pair(1), 0x1}, true};
            }
            if (d0_on && !d0_shared) {
                fetches[fetch_count++] = {
                    &lighting.lut_d0, Sampler::Distribution0, {values, 0x1}, true};
            }
            if (d1_on && !d1_shared) {
                fetches[fetch_count++] = {
                    &lighting.lut_d1, Sampler::Distribution1, {values, 0x2}, false};
            }
            const uint32_t specular_quads = (spec0_on ? 0x1u : 0u) | (spec1_on ? 0x2u : 0u);
            for (u32 i = 0; i < fetch_count; i++) {
                if (i + 1 == fetch_count) {
                    LightQuads(specular_quads, false);
                }
                lut_value(*fetches[i].lut, fetches[i].sampler, two_sided, fetches[i].into,
                          fetches[i].pair_free);
            }
            LightQuads(specular_quads, true);
            if (refl_on && !refl_scalar) {
                if (!rr) {
                    e.MoveInternal({acc.Pair(0), 0x1}, {Reg::C(2), XXXX}, Pred::None, true);
                }
                if (!rg) {
                    e.Vec(VecOp::Add, {acc.Pair(0), 0x2}, {acc.Pair(0), XXXX}, ConstZero);
                }
                if (!rb) {
                    e.Vec(VecOp::Add, {acc.Pair(1), 0x1}, {acc.Pair(0), XXXX}, ConstZero);
                }
            }
            // specular_1 * d1 * refl (each factor only when it is not 1) in i2, then
            // specular_0 * d0 added on in i1. With specular_0 zero (the light's or every
            // light's) the first part alone, made in i1; with specular_1 zero the second.
            const Reg term = spec0_on ? I2 : I1;
            const Reg scratch = spec0_on ? I1 : I2;
            if (spec1_on) {
            LoadVec4(term, specular_1);
            if (d1_on) {
                const Src d1 = d1_shared ? Src{Reg::V(TmpLutShared), YYYY} : Src{values, YYYY};
                e.Vec(VecOp::Mul, {term, 0x7}, {term, XYZW}, d1, Pred::None, true);
            }
        if (refl_scalar) {
            e.Vec(VecOp::Mul, {term, 0x7}, {term, XYZW}, {acc.Pair(0), XXXX}, Pred::None, true);
        } else if (refl_on) {
            LoadVec4(scratch, acc);
            e.Vec(VecOp::Mul, {term, 0x7}, {term, XYZW}, {scratch, XYZW}, Pred::None, true);
        }
        if (geo1) {
            e.Vec(VecOp::Mul, {term, 0x7}, {term, XYZW}, {geo_reg, YYYY}, Pred::None, true);
        }
            } // spec1_on
        if (spec0_on) {
        LoadVec4(I1, specular_0);
        if (d0_on) {
            const Src d0 = d0_shared ? Src{Reg::V(TmpLutShared), XXXX} : Src{values, XXXX};
            e.Vec(VecOp::Mul, {I1, 0x7}, {I1, XYZW}, d0, Pred::None, true);
        }
        if (geo0) {
            e.Vec(VecOp::Mul, {I1, 0x7}, {I1, XYZW}, {geo_reg, YYYY}, Pred::None, true);
        }
        // The add's first source the side that is a plain load (only the first source folds
        // a load): specular_1's when nothing multiplies it but something multiplies specular_0.
        if (!spec1_on) {
            // specular_0's part alone
        } else if (!d1_on && !refl_on && !geo1 && (d0_on || geo0)) {
            e.Vec(VecOp::Add, {I1, 0x7}, {I2, XYZW}, {I1, XYZW}, Pred::None, true);
        } else {
            e.Vec(VecOp::Add, {I1, 0x7}, {I1, XYZW}, {I2, XYZW}, Pred::None, true);
        }
        }
        } // a term
        // * clamp_highlights * atten, into specular_sum. clamp_highlights is sign(dot_product),
        // which is never negative: the term is zeroed where dot_product is not positive, the
        // predicate made here: the first light's term zeroed, a later one's accumulation
        // predicated on it (a zeroing move fewer each).
        const bool clamp_accumulate = lighting.clamp_highlights && !specular_empty;
        if (lighting.clamp_highlights) {
            e.Test(Cond::Gt, 0, {dot_product}, 0, ConstZero, false);
            if (!clamp_accumulate) {
                e.MoveInternal({I1, 0x7}, {Reg::C(0), XXXX}, Pred::NotP0, true);
            }
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
        if (direct_secondary) {
            // An alpha no argument reads: its byte from lane z, so lane w is not read (a load
            // of the specular quad into I1 then folds into its first multiply).
            constexpr uint8_t xyzz = 0xA4;
            if (secondary_alpha_read) {
                e.Vec(VecOp::Add, {I1, 0x8}, ConstZero, ConstOne, Pred::None, true);
            }
            e.PackU8(Reg::V(TmpU8Specular), I1, I1, secondary_alpha_read ? PackSelIdentity : xyzz);
            specular_empty = false;
        } else if (specular_empty) {
            if (spec_lane >= 0) {
                store1(specular_sum, I1, spec_lane);
            } else {
                store3(specular_sum, I1);
            }
            specular_empty = false;
        } else if (spec_lane >= 0) {
            accumulate1(specular_sum, I1, spec_lane, clamp_accumulate ? Pred::P0 : Pred::None);
        } else {
            accumulate3(specular_sum, I1, clamp_accumulate ? Pred::P0 : Pred::None);
        }
        } // uses_secondary_color
        if (uses_primary_color) {
        // diffuse_sum += (diffuse * dot_product + ambient) * atten
        LightQuads(0xc, true);
        LoadVec4(I0, diffuse);
        if (ambient_on) {
            LoadVec4(I1, ambient);
        }
        e.Vec(VecOp::Mul, {I0, 0x7}, {I0, XYZW}, {dot_product, XXXX}, Pred::None, true);
        if (shadow_primary && shadow_scalar) {
            e.Vec(VecOp::Mul, {I0, 0x7}, {I0, XYZW}, {shadow.Pair(0), XXXX}, Pred::None, true);
        } else if (shadow_primary) {
            LoadVec4(I2, shadow);
            e.Vec(VecOp::Mul, {I0, 0x7}, {I0, XYZW}, {I2, XYZW}, Pred::None, true);
        }
        // The ambient first, so its load folds into the add's first source.
        if (ambient_on) {
            e.Vec(VecOp::Add, {I0, 0x7}, {I1, XYZW}, {I0, XYZW}, Pred::None, true);
        }
        if (atten_on) {
            e.Vec(VecOp::Mul, {I0, 0x7}, {I0, XYZW}, {atten, XXXX}, Pred::None, true);
        }
        if (direct_primary) {
            const Swizzle xyz1{Ch::X, Ch::Y, Ch::Z, Ch::One};
            e.Vec(VecOp::Add, {I0, 0xf}, {global_ambient, xyz1}, {I0, xyz1}, Pred::None, true);
            e.PackU8(Reg::V(TmpU8Diffuse), I0, I0);
            diffuse_empty = false;
        } else if (diffuse_empty) {
            store3_ambient(diffuse_sum, I0);
            diffuse_empty = false;
        } else {
            accumulate3(diffuse_sum, I0);
        }
        } // uses_primary_color

        // The fresnel term, on the last light, into the alphas asked for.
        const bool fresnel_primary = uses_primary_color && lighting.enable_primary_alpha;
        const bool fresnel_secondary = uses_secondary_color && lighting.enable_secondary_alpha;
        if (light_index + 1 == lighting.src_num && (fresnel_primary || fresnel_secondary) &&
            LightingLutEnabled(lighting.lut_fr, Sampler::Fresnel)) {
            const Vec4Loc first = fresnel_primary ? diffuse_sum : specular_sum;
            lut_value(lighting.lut_fr, Sampler::Fresnel, two_sided, {first.Pair(1), 0x2});
            if (fresnel_primary && fresnel_secondary) {
                e.Vec(VecOp::Add, {specular_sum.Pair(1), 0x2}, {diffuse_sum.Pair(1), XYZW},
                      ConstZero);
            }
        }
    }

    // Every light's specular term left out: the sum's colour is zero.
    if (uses_secondary_color && specular_empty && lighting.src_num != 0) {
        e.Vec(VecOp::Add, {specular_sum.Pair(0), 0x3}, ConstZero, ConstZero);
        e.Vec(VecOp::Add, {specular_sum.Pair(1), 0x1}, ConstZero, ConstZero);
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
    // diffuse_sum.rgb += global ambient (unless the first light's store added it); both
    // clamped in place, or packed for the U8 combiners (unless packed at the light). For the U8
    // combiners with a moved lane the sums stay F32 for U8Inputs' packs, which saturate as the
    // clamp does: not clamped.
    if (uses_primary_color && !direct_primary) {
        if (ambient_added && u8_lighting_packed) {
            e.PackU8(Reg::V(TmpU8Diffuse), diffuse_sum.Pair(0), diffuse_sum.Pair(1));
        } else if (!(ambient_added && u8_tev)) {
            LoadVec4(I0, diffuse_sum);
            if (!ambient_added) {
                e.Vec(VecOp::Add, {I0, 0x7}, {global_ambient, XYZW}, {I0, XYZW}, Pred::None, true);
            }
            if (u8_lighting_packed) {
                e.PackU8(Reg::V(TmpU8Diffuse), I0, I0);
            } else {
                if (!u8_tev) {
                    Clamp01(I0);
                }
                StoreVec4(diffuse_sum, I0);
            }
        }
    }
    if (uses_secondary_color && !direct_secondary) {
        if (u8_lighting_packed) {
            e.PackU8(Reg::V(TmpU8Specular), specular_sum.Pair(0), specular_sum.Pair(1));
        } else if (!u8_tev) {
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
    // An argument is built in the internal register itself: the colour source's quad in one move
    // (or one lane broadcast), the alpha source's lane moved into w when it differs, then one add
    // for the inverted lanes.
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

bool FragmentEmitter::PrimaryLaneMoved() const {
    bool moved = false;
    ForEachU8Arg([&](u32, Source csrc, ColorModifier cmod, Source asrc, AlphaModifier amod) {
        moved = moved || (csrc == Source::PrimaryColor && MovedLane(cmod) >= 0) ||
                (asrc == Source::PrimaryColor && MovedLane(amod) >= 0);
    });
    return moved;
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

void FragmentEmitter::U8Inputs(uint32_t parts) {
    // With the alpha test first, an input is its first phase's (bit 0 of `parts`) when the
    // tested alpha can come from it: the constants, the vertex colour and early_units' texels.
    const bool first = (parts & 1) != 0, second = (parts & 2) != 0;
    const auto unit_now = [&](u32 unit) {
        return early_alpha && (early_units >> unit & 1) ? first : second;
    };
    if (first) {
        // White and AddSigned's half: the literal words themselves, as the SOPs read the
        // constant colours; copied through a VBW only past register 63, whose number's top bit
        // selects the colour format control's C10 reading (SopOperand). Zero from white.
        const auto literal = [&](uint8_t lit, uint8_t tmp) {
            const uint32_t sa = LitSa(lit);
            if (sa < 64) {
                return Reg::Sa(static_cast<uint8_t>(sa));
            }
            e.Bitwise(BwOp::Or, Reg::V(tmp), IOp::R(Reg::Sa(static_cast<uint8_t>(sa))),
                      IOp::Imm(0));
            return Reg::V(tmp);
        };
        u8_white = literal(LitU8White, TmpU8White);
        e.Bitwise(BwOp::And, Reg::V(TmpU8Zero), IOp::R(u8_white), IOp::Imm(0));
        bool half = false;
        for (u32 k = 0; k < config.texture.tev_stages.size(); k++) {
            const TevStageConfig stage = config.texture.tev_stages[k];
            half = half ||
                   (!IsPassThroughTevStage(stage) && (stage.color_op == Operation::AddSigned ||
                                                      stage.alpha_op == Operation::AddSigned));
        }
        if (half) {
            u8_half = literal(LitU8Half, TmpU8Half);
        }
        // The inputs, packed to nearest as the F32 path's byte rounding has them (the vertex
        // colour); the texels and the lighting's colours are rounded here, where the F32 path
        // carried them unrounded into the first stage reading them.
        if (StageReads(Source::PrimaryColor) && colour_u8_pa < 0) {
            e.PackU8(Reg::V(TmpU8Primary), Reg::Pa(0), Reg::Pa(2));
        }
    }
    for (u32 unit = 0; unit < 3; unit++) {
        if (!unit_now(unit)) {
            continue;
        }
        if (unit_own[unit]) {
            // The tile's colour as the program finds it: the pixel's own texel.
            SopCtl s;
            s.cmod1 = s.amod1 = true; // ONE * src1 + ZERO
            e.Sop2(Reg::V(static_cast<uint8_t>(TmpU8Tex + unit)), Reg::O(0), Reg::O(0), s);
            continue;
        }
        // A unit only the lighting reads (the shadow, a bump map) has no stage to pack for; a
        // raw one's texel is its bytes already (U8Source).
        if (!unit_used[unit] || texel_raw[unit] ||
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
    if (second && uses_proctex) {
        if (proctex_tabled) {
            e.PackU8F16(Reg::V(TmpU8Tex3), proctex_loc.Pair(0));
        } else {
            e.PackU8(Reg::V(TmpU8Tex3), Reg::V(TmpTex3), Reg::V(TmpTex3 + 2));
        }
    }
    if (second && uses_lighting && !u8_lighting_packed) {
        e.PackU8(Reg::V(TmpU8Diffuse), Reg::V(TmpDiffuse), Reg::V(TmpDiffuse + 2));
        e.PackU8(Reg::V(TmpU8Specular), Reg::V(TmpSpecular), Reg::V(TmpSpecular + 2));
    }
    // A channel a modifier moves (alpha = primary.r, colour = texel.ggg): the input packed
    // again from the same lanes, that channel into every byte, so the argument reads it as the
    // colour or the alpha itself.
    if (first) {
        u8_bcast = {};
        u8_bcast_count = 0;
    }
    uint32_t& next = u8_bcast_count;
    const auto broadcast = [&](Source src, int lane) {
        if (lane < 0 || !U8InputPresent(src)) {
            return;
        }
        const int input = BroadcastInput(src);
        if (input < 0) {
            throw std::runtime_error("usse: a moved U8 channel U8TevSupported did not admit");
        }
        // The part the input itself is packed in.
        const bool now =
            src == Source::PrimaryColor
                ? first
                : src >= Source::Texture0 && src <= Source::Texture2
                      ? unit_now(static_cast<u32>(src) - static_cast<u32>(Source::Texture0))
                      : second;
        if (!now || u8_bcast[input][lane] != 0) {
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
            const u32 unit = static_cast<u32>(input - BroadcastInput(Source::Texture0));
            const Vec4Loc texel = texel_loc[unit];
            if (texel_raw[unit]) {
                U8LaneBroadcast(Reg::Pa(texel.base), lane, dst);
            } else if (texel.f16) {
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
        if (texel_raw[unit]) {
            return Reg::Pa(texel_loc[unit].base);
        }
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
    // Lanes no operation reads take the other's register as it is.
    Reg c = u8_white, a = u8_white;
    u32 cm = 0, am = 0;
    // A moved channel U8Inputs did not broadcast: made here from the source's colour.
    const auto lazy = [&](Source src) {
        return BroadcastInput(src) < 0 ||
               (src >= Source::Texture0 && src <= Source::Texture2 &&
                unit_own[static_cast<u32>(src) - static_cast<u32>(Source::Texture0)]);
    };
    // A constant's from its broadcast word (PlanUniforms), any other's made here.
    const auto lane_broadcast = [&](Source src, int lane, uint8_t tmp) {
        if (src == Source::Constant) {
            const uint32_t sa = sa_const_lane[stage_index][lane];
            DEBUG_ASSERT(sa != ~0u);
            return Reg::Sa(static_cast<uint8_t>(sa));
        }
        return U8LaneBroadcast(U8Source(src, stage_index, false), lane, Reg::V(tmp));
    };
    if (need_colour) {
        c = clane < 0  ? U8Source(cs, stage_index, ReadsAlphaLane(cmod))
            : lazy(cs) ? lane_broadcast(cs, clane, static_cast<uint8_t>(TmpU8LaneColour + k))
                       : U8Broadcast(cs, clane);
        cm = clane >= 0 ? static_cast<u32>(cmod) & 1 : static_cast<u32>(cmod);
    }
    if (need_alpha) {
        a = alane < 0  ? U8Source(as, stage_index, true)
            : lazy(as) ? lane_broadcast(as, alane, static_cast<uint8_t>(TmpU8LaneAlpha + k))
                       : U8Broadcast(as, alane);
        am = alane >= 0 ? static_cast<u32>(amod) & 1 : static_cast<u32>(amod);
    }
    if (!need_colour) {
        c = a;
    }
    if (!need_alpha) {
        a = c;
    }
    const bool one = c.bank == a.bank && c.n == a.n;
    if (cm == 0 && am == 0 && one) {
        return c;
    }
    // One word over white: colour = the colour source's colour or alpha, complemented or not, times
    // one; alpha = the alpha source's alpha, complemented or not. A SOP3's source 0 is no secondary
    // attribute (U8Src0 copies one): the alpha's source goes there, or else the colour's, its alpha
    // then from source 2; one secondary attribute for both takes a SOP2, which names it as source
    // 2.
    SopCtl s;
    s.cmod1 = (cm & 1) != 0;
    s.amod1 = (am & 1) != 0;
    const Reg dst = Reg::V(static_cast<uint8_t>(TmpU8Arg + k));
    if (one && c.bank == Bank::SecAttr) {
        s.csel1 = (cm & 2) ? 4 : 2; // src2 alpha : src2
        s.asel1 = 2;                // src2 alpha
        e.Sop2(dst, u8_white, c, s);
    } else if (a.bank != Bank::SecAttr || c.bank == Bank::SecAttr) {
        s.csel1 = (cm & 2) ? 7 : 6; // src2 alpha : src2
        s.asel1 = 1;                // src0 alpha
        e.Sop3(dst, U8Src0(a), u8_white, c, s);
    } else {
        s.csel1 = (cm & 2) ? 5 : 4; // src0 alpha : src0
        s.asel1 = 3;                // src2 alpha
        e.Sop3(dst, c, u8_white, a, s);
    }
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
        const Reg h = u8_half;
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
        // a * b + c: b in source 0 (a where b is a secondary attribute and a is not: source 0
        // names none), ONE * c.
        SopCtl s;
        s.csel1 = 4;
        s.csel2 = 0;
        s.cmod2 = true;
        s.asel1 = 1;
        const bool swap = b.bank == Bank::SecAttr && a.bank != Bank::SecAttr;
        e.Sop3(dst, U8Src0(swap ? a : b), swap ? b : a, c, s);
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
            // other. A Replace side is argument 0 itself, read by the merge.
            const Reg colour = stage.color_op == Operation::Replace ? args[0] : Reg::V(TmpU8Colour);
            const Reg alpha = stage.alpha_op == Operation::Replace ? args[0] : out;
            if (stage.color_op != Operation::Replace) {
                U8Op(stage.color_op, colour, args);
            }
            if (stage.alpha_op != Operation::Replace) {
                U8Op(stage.alpha_op, alpha, args);
            }
            SopCtl s;
            s.cmod1 = true; // ONE * colour result
            s.amod2 = true; // ONE * the alpha result's alpha
            e.Sop2(out, colour, alpha, s);
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

bool FragmentEmitter::PlanEarlyAlpha() {
    // A program that kills runs its first phase, up to the kill, for every pixel the ISP hands it;
    // with the test after the last stage that is the shadow, the lights and the combiners, for
    // pixels the test then drops too. Where the tested alpha comes from texels, the vertex colour
    // and constants alone, the first phase is those stages' alpha and the test, and the lighting
    // and the stages follow the kill.
    const auto func = config.framebuffer.alpha_test_func.Value();
    if (!u8_tev || !uses_lighting || !uses_kill || shadow_write ||
        func == FramebufferRegs::CompareFunc::Always ||
        func == FramebufferRegs::CompareFunc::Never) {
        return false;
    }
    const auto& stages = config.texture.tev_stages;
    // The stage whose output a Previous read at stage k sees, or -1 (zero).
    const auto before = [&](int k) {
        for (int j = k - 1; j >= 0; j--) {
            if (!IsPassThroughTevStage(stages[j])) {
                return j;
            }
        }
        return -1;
    };
    const int last = before(static_cast<int>(stages.size()));
    if (last < 0) {
        return false;
    }
    uint32_t need = 1u << last, units = 0;
    for (int k = last; k >= 0; k--) {
        if (!(need >> k & 1)) {
            continue;
        }
        const TevStageConfig stage = stages[k];
        if (stage.color_op == Operation::Dot3_RGBA) {
            return false; // its alpha is the colours' dot product
        }
        const Operation op = stage.alpha_op;
        const u32 args = op == Operation::Replace ? 1u
                         : (op == Operation::Lerp || op == Operation::MultiplyThenAdd ||
                            op == Operation::AddThenMultiply)
                               ? 3u
                               : 2u;
        const Source srcs[3] = {stage.alpha_source1, stage.alpha_source2, stage.alpha_source3};
        const AlphaModifier mods[3] = {stage.alpha_modifier1, stage.alpha_modifier2,
                                       stage.alpha_modifier3};
        for (u32 a = 0; a < args; a++) {
            const Source src = AlphaSource(stage, srcs[a], static_cast<u32>(k));
            const bool moved = MovedLane(mods[a]) >= 0;
            switch (src) {
            case Source::PrimaryColor:
            case Source::Constant:
                break;
            case Source::Texture0:
            case Source::Texture1:
            case Source::Texture2: {
                const u32 unit = static_cast<u32>(src) - static_cast<u32>(Source::Texture0);
                if (unit_own[unit]) {
                    return false;
                }
                units |= unit_used[unit] ? 1u << unit : 0;
                break;
            }
            case Source::Previous: {
                // A colour channel of a stage's output is the colour stages'.
                if (moved) {
                    return false;
                }
                const int from = before(k);
                need |= from >= 0 ? 1u << from : 0;
                break;
            }
            case Source::PreviousBuffer: {
                if (moved) {
                    return false;
                }
                int from = buffer_alpha_from[k];
                if (from >= 0 && IsPassThroughTevStage(stages[from])) {
                    from = before(from);
                }
                need |= from >= 0 ? 1u << from : 0;
                break;
            }
            default:
                return false; // the lighting's colours, the procedural texture
            }
        }
    }
    early_stages = need;
    early_units = units;
    return true;
}

void FragmentEmitter::U8AlphaStage(u32 index) {
    // The alpha bytes U8TevStage makes, by the instructions it makes them with: each argument's
    // alpha (U8Arg, the colour not read), the alpha operation over them (in every form of a
    // stage the alpha is U8Op's, or a SOP2 with U8Op's alpha factors), the scale.
    const TevStageConfig stage = config.texture.tev_stages[index];
    const Operation op = stage.alpha_op;
    const u32 count = op == Operation::Replace ? 1u
                      : (op == Operation::Lerp || op == Operation::MultiplyThenAdd ||
                         op == Operation::AddThenMultiply)
                            ? 3u
                            : 2u;
    const Source csrcs[3] = {stage.color_source1, stage.color_source2, stage.color_source3};
    const ColorModifier cmods[3] = {stage.color_modifier1, stage.color_modifier2,
                                    stage.color_modifier3};
    const Source asrcs[3] = {stage.alpha_source1, stage.alpha_source2, stage.alpha_source3};
    const AlphaModifier amods[3] = {stage.alpha_modifier1, stage.alpha_modifier2,
                                    stage.alpha_modifier3};
    std::array<Reg, 3> args{Reg::V(TmpU8Zero), Reg::V(TmpU8Zero), Reg::V(TmpU8Zero)};
    for (u32 k = 0; k < count; k++) {
        args[k] = U8Arg(static_cast<uint8_t>(k), index, csrcs[k], cmods[k], asrcs[k], amods[k],
                        false, true);
    }
    const unsigned scale = stage.GetAlphaMultiplier();
    const bool scratch = args[0].bank == Bank::Virtual &&
                         ((args[0].n >= TmpU8Arg && args[0].n < TmpU8Arg + 3) ||
                          args[0].n == TmpU8Const);
    if (op == Operation::Replace && scale == 1 && !scratch) {
        u8_out[index] = args[0];
        u8_prev = args[0];
        return;
    }
    const Reg out = Reg::V(static_cast<uint8_t>(TmpU8Stage + index * 2));
    U8Op(op, out, args);
    for (unsigned step = 2; step <= scale; step *= 2) {
        SopCtl s;
        s.cmod1 = s.amod1 = s.cmod2 = s.amod2 = true; // the result added to itself, saturating
        e.Sop2(out, out, out, s);
    }
    u8_out[index] = out;
    u8_prev = out;
}

void FragmentEmitter::U8Output() {
    Reg final = U8EvenFinal();
    if (!early_alpha) {
        AlphaTest(&final);
    }
    if (shadow_write) {
        ShadowWrite(&final);
        return;
    }
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
    // The texture combiners in U8 registers with SOP2 and SOP3 (each product, and each sum of
    // two, to nearest, as the F32 path's byte rounding gives) where every stage has a U8 form,
    // else the F32 path. Gas zeroes the F32 combiner output (Fog).
    u8_tev = U8TevSupported() && config.texture.fog_mode != TexturingRegs::FogMode::Gas;
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
        0x7f7f7f7fu,
        std::bit_cast<uint32_t>(65536.0f),
        std::bit_cast<uint32_t>(1.0f / 65536.0f),
        std::bit_cast<uint32_t>(16777215.0f),
        std::bit_cast<uint32_t>(0.25f),
        std::bit_cast<uint32_t>(static_cast<float>(ShadowSize(config.texture.shadow_width_log2))),
        std::bit_cast<uint32_t>(static_cast<float>(ShadowSize(config.texture.shadow_height_log2))),
        std::bit_cast<uint32_t>(0.5f),
        std::bit_cast<uint32_t>(127.5f / 256.0f),
        std::bit_cast<uint32_t>(std::numeric_limits<float>::min()),
        0u,
        std::bit_cast<uint32_t>(1.0f /
                                static_cast<float>(ShadowSize(config.texture.shadow_width_log2))),
        std::bit_cast<uint32_t>(1.0f /
                                static_cast<float>(ShadowSize(config.texture.shadow_height_log2)))});
    static_assert(LiteralCount == 58);
    static_assert(LitShadowWidth % 2 == 0 && LitShadowInvWidth % 2 == 0);
    static_assert(std::tuple_size_v<decltype(literal_table)> == LiteralCount);
    static_assert(Pica::Shader::LUT_RG_ROWS == Pica::Shader::LUT_RGBA_ROWS);
    program.uses_discard = uses_kill;
    if (uniform_floats + literal_reserve + program.samplers.size() * 4 > 128) {
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
    // Dual-issue words in the second tier alone: finding them slows the first tier's worker.
    e.dual_issue = level >= 2;

    // A kill ends its phase: psp2cgc follows every kill with its nop and a new phase marker. The
    // first marker waits with condition 1 when another phase follows, 7 otherwise, as the
    // compiler's do.
    e.Phas(!uses_kill);
    e.Nop();
    Scissor();
    PlanCombinerBuffer();
    early_alpha = PlanEarlyAlpha();
    if (early_alpha) {
        // The first phase: the texels the tested alpha reads, those stages' alpha, the test and
        // its kill. Every register it wrote stays for the second.
        SampleTextures(early_units);
        U8Inputs(1);
        for (u32 i = 0; i < config.texture.tev_stages.size(); i++) {
            if (IsPassThroughTevStage(config.texture.tev_stages[i])) {
                u8_out[i] = u8_prev;
            } else if (early_stages >> i & 1) {
                U8AlphaStage(i);
            }
        }
        Reg tested = U8EvenFinal();
        AlphaTest(&tested);
        SampleTextures(~early_units & 7);
    } else {
        SampleTextures();
    }
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
    if (u8_tev) {
        // combiner_output starts at zero, in U8.
        U8Inputs(early_alpha ? 2 : 3);
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
        if (shadow_write) {
            ShadowWrite(nullptr);
        } else {
            Fog();
            e.PackF16({Reg::Pa(0), 0xf}, Reg::V(TmpOutput), Reg::V(TmpOutput + 2));
        }
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
    FragmentEmitter emitter{config, 1, std::nullopt};
    return emitter.SampledUnits();
}

std::optional<Program> EmitFragmentProgram(const FSConfig& config, Scratch scratch,
                                          std::string* refusal, int level) {
    FragmentEmitter emitter{config, level, scratch};
    std::optional<FragmentEmitter> exact;
    try {
        std::optional<Program> emitted = emitter.Emit(refusal);
        if (!emitted || !emitter.LightsInMemory()) {
            return emitted;
        }
        // Lights in memory because the budget held the whole literal table back: again with only
        // the literals this program reads. A light in registers can read literals its memory form
        // did not: again with the reserve the emission read, until it holds them. The reserve only
        // grows, and the whole table (the first emission's) fits.
        uint32_t reserve = emitter.LiteralWords();
        std::string why;
        std::optional<Program> resident;
        while (true) {
            exact.emplace(config, level, scratch, reserve);
            try {
                resident = exact->Emit(&why);
            } catch (const std::exception&) {
                // An SA past 127 throws as soon as the code reads one: the literals so far.
                if (exact->LiteralWords() <= reserve) {
                    throw;
                }
            }
            if (exact->LiteralWords() <= reserve) {
                break;
            }
            reserve = exact->LiteralWords();
        }
        DEBUG_ASSERT_MSG(resident.has_value() && exact->SaWords() <= 128,
                         "usse: the program planned with its {} literal words: {} SA words; {}",
                         reserve, exact->SaWords(), why);
        if (refusal) {
            *refusal = why;
        }
        return resident;
    } catch (const std::exception& ex) {
        // An operand form the encoder cannot express; with the emission it stopped in and its
        // SA words so far (an SA past 127 does not fit an operand).
        if (refusal) {
            *refusal = fmt::format("{} (the {} emission: {}; the first read {} literal words)",
                                   ex.what(), exact ? "second" : "first",
                                   (exact ? *exact : emitter).Budget(), emitter.LiteralWords());
        }
        return std::nullopt;
    }
}

} // namespace SgxdRenderer::Usse
