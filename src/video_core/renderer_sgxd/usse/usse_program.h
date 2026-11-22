// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace SgxdRenderer::Usse {

/**
 * An emitted USSE program, as sgxd runs it: the code words, the register counts the PDS
 * programs and the pass state take, and the program's secondary attribute (SA) layout, which
 * the rasterizer fills per draw (the SA image the secondary PDS program DMAs).
 *
 * The SAs, in order:
 *  - the resident uniforms: the first `uniform_floats` SAs, each uniform at its `sa_offset`
 *    (a float4 array's elements four floats apart);
 *  - the data block: the uniform buffer pointer when the program reads uniforms from memory
 *    (`buffer_pointer`, DataHead), then four SAs of state words per sampler, then the literals
 *    (SamplerSaOffset, LiteralSaOffset);
 *  - past them, up to `secondary_reg_count`: the prefetched uniforms (vertex programs) and
 *    what the secondary program writes.
 *
 * The uniforms past the resident ones live in a buffer in memory that the program loads
 * through the pointer: `buffer_floats` floats, the pointer's value the buffer's address plus
 * `ldst_base_value` (the byte offset of the memory part minus four, which the load offsets
 * count from; see usse_encoder Lda32).
 */

/// Where sampler `index`'s four state words and literal `index` sit, from `data_at`: the SA past
/// the resident uniforms and the buffer pointer (uniform_floats + DataHead). The samplers come
/// first: their count is known before a program's code is, its literals only after (a fragment
/// program keeps the ones its code reads).
inline uint32_t SamplerSaOffset(uint32_t data_at, uint32_t index) {
    return data_at + index * 4;
}
inline uint32_t LiteralSaOffset(uint32_t data_at, uint32_t sampler_count, uint32_t index) {
    return data_at + sampler_count * 4 + index;
}

/// An interpolated input as the hardware names it: the id in the iterator word's bits 12-15.
enum class FragmentInput : uint32_t {
    TexCoord0 = 0x0,
    TexCoord1 = 0x1,
    TexCoord2 = 0x2,
    TexCoord3 = 0x3,
    TexCoord4 = 0x4,
    TexCoord5 = 0x5,
    TexCoord6 = 0x6,
    TexCoord7 = 0x7,
    TexCoord8 = 0x8,
    TexCoord9 = 0x9,
    Color0 = 0xA,
    Color1 = 0xB,
    Fog = 0xC,
    Position = 0xD, ///< WPOS
};

struct Varying {
    FragmentInput input;
    uint8_t components; ///< 1-4, each one PA register (f32)
    /// Iterated as F16 (DOUTI USEFORMAT F16, psp2cgc's `half` varyings): two components a
    /// register, read with pack.f32.f16.
    bool half = false;
    /// A colour iterated in fixed point (DOUTI without the colour's float bit): its four
    /// bytes in one register.
    bool fixed = false;
};
inline uint8_t VaryingWords(const Varying& v) {
    return v.fixed ? 1 : v.half ? static_cast<uint8_t>((v.components + 1) / 2) : v.components;
}

/// A texture read the pixel PDS program issues before the program starts (a non-dependent
/// read): its own DOUTI (TEXISSUE the coordinate set, no USE issue) and DOUTT of the unit's
/// state words, after the DOUTI of inputs[after_input]. The texel lands as four F32 words at
/// `pa` (FCONV F32): issues fill the PAs in PDS order (docs/sgxd/pds.md).
struct TextureIssue {
    uint8_t unit;        ///< the PICA texture unit whose state words the DOUTT carries
    uint8_t coord;       ///< TEXISSUE: the coordinate set, TCn
    uint8_t pa;          ///< the texel's first PA register
    uint8_t after_input; ///< issued after this input's DOUTI
    bool f16 = false;    ///< four F16 in two words (FCONV F16), for the U8 combiners
    /// The fog factor table (RasterizerSgxd::FogTableWords), not a unit: its packed U8888 word
    /// (FCONV raw, one word) read at TEXCOORD3 projected (TEXPROJ T), whose u and t the vertex
    /// program writes as clip z and w: u / t is the PICA depth, linear in screen space.
    bool fog = false;
    /// The unit's split alpha texture (RasterizerSgxd::AlphaTextureWords, A8 with its byte in
    /// .w), read at the unit's coordinate in the texel's form, f16 or F32.
    bool alpha = false;
};
inline constexpr uint8_t IssueTexelWords = 4;
/// The issues a pixel PDS program takes (sgxd_program_pica_pixel_pds).
inline constexpr uint32_t MaxTextureIssues = 7;
inline constexpr uint8_t IssueTexelWordsF16 = 2;
inline constexpr uint8_t IssueTexelWordsRaw = 1;

/// What a uniform holds: the rasterizer fills it by its kind and index (UniformName names it
/// for messages and the program dump).
enum class UniformKind : uint8_t {
    // Vertex programs.
    FlipViewport,
    DepthScale,
    DepthOffset,
    DefaultInput, ///< input register `index`'s fixed value
    IntUniforms,
    FloatBank, ///< the float bank, read through the buffer pointer
    // Fragment programs: FSUniformData's fields.
    AlphaTestRef,
    ConstColor,
    BufferColor, ///< tev_combiner_buffer_color
    ScissorX1,
    ScissorY1,
    ScissorX2,
    ScissorY2,
    FogLutOffset,
    FogColor,
    TexBorderColor,
    ProctexLutOffset,
    ProctexDiffLutOffset,
    ProctexColorMapOffset,
    ProctexAlphaMapOffset,
    ProctexBias,
    ProctexNoiseLutOffset,
    ProctexNoiseF,
    ProctexNoiseA,
    ProctexNoiseP,
    LightingLutOffset,
    LightingLutRow, ///< each lighting LUT's row centre in the lf texture: (offset / 256 + 0.5) /
                    ///< LUT_LF_ROWS
    LightingGlobalAmbient,
    // Light `index`'s members (light_src[index]).
    LightSpecular0,
    LightSpecular1,
    LightDiffuse,
    LightAmbient,
    LightPosition,
    LightSpotDirection,
    LightDistAttenBias,
    LightDistAttenScale,
    /// A directional light's position normalised (its light vector), once a draw as the
    /// rasterizer fills the SAs instead of in every pixel.
    LightDirection,
    /// Stage `index`'s constant colour as a U8 Dot3 reads it (FragmentEmitter::U8Dot3Const):
    /// 4 (b - 0.5) in x, y and z and -2 sum(b - 0.5) in w, b its bytes over 255.
    Dot3Const,
};

struct Uniform {
    UniformKind kind;
    uint8_t index;       ///< DefaultInput's register, a light member's light; 0 otherwise
    uint32_t sa_offset;  ///< its first SA (resident) or float of the memory buffer
    uint8_t components;  ///< 1-4
    uint32_t array_size; ///< 1 for a scalar or vector
    /// One word an element, the components as unsigned bytes (round(x * 255), x the lowest,
    /// a missing component zero): what the U8 combiners' SOPs read in place.
    bool u8 = false;
};

enum class SamplerKind : uint8_t {
    Texture,      ///< PICA texture unit `index`
    TextureAlpha, ///< a split ETC1A4 unit's alpha (TEXUNIT 7 + index)
    LutLf,        ///< the lighting and fog LUTs
    LutRg,        ///< the proctex maps
    LutRgba,      ///< the proctex colours
    ProctexNoise,
};

struct Sampler {
    SamplerKind kind;
    uint8_t index; ///< Texture's and TextureAlpha's PICA unit; 0 otherwise
    uint32_t unit; ///< TEXUNITn, 0-15
};

enum class AttributeKind : uint8_t {
    Input,      ///< input register `index`
    OutAddress, ///< where a store-outputs program writes its vertex
    GsInRows,   ///< the geometry program's row
};

struct Attribute {
    AttributeKind kind;
    uint8_t index;     ///< Input's register; 0 otherwise
    uint32_t resource; ///< its first primary attribute register
    uint8_t words;     ///< the primary attribute registers its fetch fills
};

/// Names, for messages and the program dump (sgxd_flags dumpusse): the parameter names the
/// emitters' Cg sources had.
[[nodiscard]] std::string UniformName(const Uniform& uniform);
[[nodiscard]] std::string SamplerName(const Sampler& sampler);
[[nodiscard]] std::string AttributeName(const Attribute& attribute);

struct Program {
    /// In PA order: the first input starts at pa0, each takes `components` registers, and
    /// the issued textures' texels (`issues`) after the input each follows.
    std::vector<Varying> inputs;
    std::vector<TextureIssue> issues;
    std::vector<Uniform> uniforms;
    std::vector<Sampler> samplers;
    uint32_t pa_count = 0;   ///< primary attribute registers the program touches
    uint32_t temp_count = 0; ///< temporaries (r0..) the program touches
    uint32_t peak_live = 0;  ///< Ir::LowerResult::peak_live (dumpusse)
    uint32_t peak_at = 0;
    std::vector<uint16_t> peak_regs;
    /// The resident uniforms' SAs: the highest SA a resident uniform occupies + 1.
    uint32_t uniform_floats = 0;
    std::vector<uint32_t> literals;
    bool uses_discard = false;
    /// The fragment's colour leaves in pa0 as one U8 register (the U8 combiners without fog),
    /// not as four F16 lanes in pa0 and pa1: the epilogue packs nothing.
    bool output_u8 = false;
    /// Instruction index at which each phase begins (the first is 0); one entry per PHAS.
    std::vector<uint32_t> phase_starts{0};
    std::vector<uint64_t> primary;
    std::vector<uint64_t> secondary;

    bool vertex = false;
    /// The PICA temporaries the program reads before it writes them (a geometry program
    /// that needs VsRequest::gs_serial).
    uint16_t gs_temps_read_first = 0;
    /// One vertex per USSE instance (the vertex PDS's per-instance mode): set on a program
    /// that branches on per-vertex data; the others run several vertices per instance.
    bool per_instance = false;
    std::vector<Attribute> attributes;
    /// The vertex output state words: OUTSELECTS ((output register count << 24) | 0x1000 |
    /// (0x800 with COLOR0)) and TEXSIZE (three bits per TEXCOORDn: 1 one or two floats, 3
    /// three, 7 four).
    uint32_t outselects = 0;
    uint32_t texsize = 0;
    bool buffer_pointer = false;
    int32_t ldst_base_value = 0;
    uint32_t buffer_floats = 0;
    /// The SAs in all: past the data block, the prefetched uniforms, then what the secondary
    /// program writes.
    uint32_t secondary_reg_count = 0;
    /// The uniforms read with a constant index, each as (SA register, PICA uniform row). The
    /// SA image carries them past the data block, so the secondary PDS program's DMA puts
    /// them in place before any vertex runs.
    std::vector<std::pair<uint32_t, uint32_t>> prefetched_uniforms;
};

/// The data block's first SA holds the buffer pointer in a program that has one; the literals
/// follow.
inline uint32_t DataHead(const Program& p) {
    return p.buffer_pointer ? 1u : 0u;
}

/// A fragment program's iterator word for input `index`: the pixel PDS's DOUTI word 1.
[[nodiscard]] uint32_t VaryingDescriptor(const Program& program, size_t index);

/// The program as JSON for sgxd_flags dumpusse: its words and everything above, with the
/// names, which tools/usse2gxp.py puts into a GXP for psp2shaderperf.
[[nodiscard]] std::string ProgramJson(const Program& program);

} // namespace SgxdRenderer::Usse
