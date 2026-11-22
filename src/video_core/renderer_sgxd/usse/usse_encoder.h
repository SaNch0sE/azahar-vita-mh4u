// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace GxmRenderer::Usse {

/**
 * The USSE instruction encoder: one builder per instruction form the fragment emitter
 * uses, each producing the 64-bit word the SDK compiler would have produced for the same
 * operation. The bit layouts are Vita3K's decoder
 * patterns (usse_translator_entry.cpp), one field per letter run; the field values come
 * from psp2cgc's output for the same operation, kept as the tests in usse_encoder_test.
 *
 * Register model, as the SDK's disassembly names things:
 *   pa<n>  primary attributes: interpolants and scratch (64 of them)
 *   sa<n>  secondary attributes: uniforms, literals, sampler words (128)
 *   r<n>   temporaries (64)
 *   i0-i2  the three 128-bit internal registers: the only operands a 4-wide f32 operation
 *          can read and write in one instruction; not preserved across a thread switch
 *   c<n>   the hardware constant bank (0, 1, 0.5, 2, ...)
 * A "register" of the unified store is 32 bits, so a float2 is two of them and an f32
 * vector operation on the unified store touches two components at most; the i registers
 * are what four-wide f32 arithmetic goes through, which is why the emitter's register
 * plan keeps vectors in them for as short a span as possible.
 */

/// Virtual: a register of the IR's virtual unified store (usse_ir.h), never encoded.
enum class Bank : uint8_t { Temp, PrimAttr, SecAttr, Internal, Const, Output, Virtual };

struct Reg {
    Bank bank;
    uint8_t n; ///< the index the disassembly prints: pa4 is {PrimAttr, 4}, i1 is {Internal, 1}

    static constexpr Reg Pa(uint8_t n) { return {Bank::PrimAttr, n}; }
    static constexpr Reg Sa(uint8_t n) { return {Bank::SecAttr, n}; }
    static constexpr Reg R(uint8_t n) { return {Bank::Temp, n}; }
    static constexpr Reg I(uint8_t n) { return {Bank::Internal, n}; }
    static constexpr Reg C(uint8_t n) { return {Bank::Const, n}; }
    static constexpr Reg O(uint8_t n) { return {Bank::Output, n}; }
    static constexpr Reg V(uint8_t n) { return {Bank::Virtual, n}; }
};

/// Swizzle channels as the hardware numbers them: x y z w, then the constants.
enum class Ch : uint8_t { X = 0, Y = 1, Z = 2, W = 3, Zero = 4, One = 5, Two = 6, Half = 7 };
using Swizzle = std::array<Ch, 4>;
constexpr Swizzle XYZW{Ch::X, Ch::Y, Ch::Z, Ch::W};
constexpr Swizzle XXXX{Ch::X, Ch::X, Ch::X, Ch::X};
constexpr Swizzle YYYY{Ch::Y, Ch::Y, Ch::Y, Ch::Y};
constexpr Swizzle ZZZZ{Ch::Z, Ch::Z, Ch::Z, Ch::Z};
constexpr Swizzle WWWW{Ch::W, Ch::W, Ch::W, Ch::W};
constexpr Swizzle ZWZW{Ch::Z, Ch::W, Ch::Z, Ch::W};
/// A VPCK's lane selects, two bits a destination lane from x up: each lane its own.
constexpr uint8_t PackSelIdentity = 0xE4;
/// Every destination lane from source lane `lane`.
constexpr uint8_t PackSelBroadcast(uint8_t lane) {
    return static_cast<uint8_t>(lane * 0x55);
}

/// Source modifiers.
enum class Mod : uint8_t { None = 0, Neg = 1, Abs = 2, NegAbs = 3 };

/// A standard-table (second source) swizzle whose first two lanes are (c0, c1), if the
/// table has one; the other lanes are whatever that entry carries.
bool StdSwizzleForPair(Ch c0, Ch c1, Swizzle& out);
/// Whether the first three channels are a three-wide table entry (mad/dot vec3 forms).
bool InVec3Tables(const Swizzle& s);
/// Whether a four-wide source swizzle is in the standard table (a vector op's second
/// source has only that one) or in either table (its first source).
bool InVec4StdTable(const Swizzle& s);
/// A four-wide table entry whose first two channels are (c0, c1): what a pair-masked
/// four-wide op reads (the compiler's `mad.f32 sa86.xy, sa50.xx, i1.zw, i0.zw` is the
/// vec4 form with mask xy and the zwzw entry).
std::optional<Swizzle> Vec4SwizzleForPair(Ch c0, Ch c1);
bool InVec4Tables(const Swizzle& s);

/// Predicates on an instruction.
enum class Pred : uint8_t { None, P0, P1, P2, NotP0, NotP1 };

/// The two-source vector operations (V32NMAD / V16NMAD op2).
enum class VecOp : uint8_t { Mul = 0, Add = 1, Frc = 2, Dsx = 3, Dsy = 4, Min = 5, Max = 6, Dp = 7 };

/// The complex operations (VCOMP op2).
enum class CompOp : uint8_t { Rcp = 0, Rsq = 1, Log = 2, Exp = 3 };

/// Compare conditions a VTST writes to a predicate.
enum class Cond : uint8_t { Gt, Ge, Lt, Le, Eq, Ne };
/// What the vector test's unit makes of its sources before the sign and zero tests (VTST's
/// alu_op with alu_sel 0: Vita3K's vf32_opcode, 14 VSUB, 13 VMUL).
enum class TestAlu : uint8_t { Sub, Mul };

/// The bitwise operations (VBW).
enum class BwOp : uint8_t { And, Or, Xor };

/// The integer colour unit's factors and operations (SOP2 and SOP3) on U8 registers: four bytes, x (red) the lowest, alpha the
/// top. Each product is x * y / 255 to nearest, a sum of two rounds once, and the result
/// saturates (docs/sgxd/usse.md).
struct SopCtl {
    /// Colour factors. SOP2: 0 zero, 1 src1, 2 src2, 3 src1 alpha, 4 src2 alpha, 5 min(src1
    /// alpha, 1 - src2 alpha); csel1 6 is 2 x src2. SOP3: 0 zero, 2 src1, 3 src1 alpha, 4 src0,
    /// 5 src0 alpha, 6 src2, 7 src2 alpha.
    uint8_t csel1 = 0, csel2 = 0;
    /// Alpha factors. SOP2: asel1 0 zero, 1 src1 alpha, 2 src2 alpha, 3 2 x src2 alpha; asel2
    /// 0 zero, 1 src1 alpha, 2 src2 alpha. SOP3: asel1 0 zero, 1 src0 alpha, 2 src1 alpha,
    /// 3 src2 alpha; its second alpha factor is csel2's alpha (asel2 unused).
    uint8_t asel1 = 0, asel2 = 0;
    bool cmod1 = false, cmod2 = false, amod1 = false, amod2 = false; ///< factors complemented
    uint8_t cop = 0, aop = 0; ///< 0 add, 1 sub (clamped at 0); SOP2 also 2 min, 3 max
    bool src1_mod = false, asrc1_mod = false; ///< SOP2: source 1's colour, alpha complemented

    /// The fields in one word (Ir::Inst keeps them so).
    uint32_t Pack() const;
    static SopCtl Unpack(uint32_t word);
};

/// An integer operand of the bitwise and integer multiply-add forms: a single 32-bit
/// register, or a 7-bit (VBW) / 8-bit (I16MAD) immediate.
struct IOp {
    bool imm;
    Reg reg;
    uint8_t value;
    static constexpr IOp R(Reg r) { return {false, r, 0}; }
    static constexpr IOp Imm(uint8_t v) { return {true, {Bank::Temp, 0}, v}; }
};

struct Src {
    Reg reg;
    Swizzle swz = XYZW;
    Mod mod = Mod::None;
};

struct Dst {
    Reg reg;
    uint8_t mask = 0xf; ///< components written, bit 0 = x
};

class Encoder {
public:
    /// The "skip invalid instances" bit every arithmetic instruction carries. The compiler
    /// sets it on some instructions and not others (clear on most of a long program, and
    /// on every instruction of the ubershader); the emitter chooses per program, and it is
    /// left settable so a compiler program can be reproduced exactly.
    bool skip_invalid = true;
    /// Encoding a secondary program: the secondary attributes are addressed with the primary
    /// attribute bank codes there (the compiler's secondary programs, all forms), since a
    /// secondary program has no primary attributes of its own.
    bool secondary = false;
    /// Places the nosched bits from the liveness of the internal registers: the bit on instruction
    /// i keeps the instance scheduled across the boundary after instruction i + 1, and psp2cgc sets
    /// it exactly when a component of i0-i2 is live there (a wait or a nop counts as an
    /// instruction).
    /// Call once, after the last instruction. The builders' `nosched` arguments are ignored.
    void FinishNosched();

    const std::vector<uint64_t>& Words() const { return words; }
    std::size_t Count() const { return words.size(); }

    /// The phase marker every program starts with. `wait_all` is the last (or only)
    /// phase's form; the first of two phases waits differently.
    void Phas(bool wait_all);
    /// The word that ends a vertex program's primary program (every compiled vertex
    /// program ends with it; the fragment programs do not have it).
    void EndVertex();
    /// The nop with the end bit that closes a secondary program (the compiler appends it
    /// when its last instruction cannot carry the bit itself).
    void EndSecondary();
    /// The wait the vertex compiler emits after its loads (the fragment form has bit 43 set).
    void WdfVertex(uint8_t drc);
    /// A branch to a word index resolved later with PatchBr; returns this word's index.
    /// The offset is relative to the branch word itself.
    std::size_t Br(Pred pred);
    void PatchBr(std::size_t index, std::size_t target);
    /// Load `count` (4 or 16) 32-bit words from the uniform buffer at base_sa (the SA holding
    /// the buffer pointer) plus an offset: `offset_words` (an immediate, in 4-byte words;
    /// the hardware adds one word) when `offset_reg` is null, else the register's value
    /// minus 0x10000 (the compiler's convention for register offsets). The destination is
    /// a temporary, primary attribute, or (in a secondary program) secondary attribute
    /// base register; the read completes at Wdf(drc).
    void Lda32(Reg dst, Reg base_sa, const Reg* offset_reg, uint8_t offset_words, uint8_t count,
               uint8_t drc);
    /// Store the 32-bit register `value` at the address in register `base` plus
    /// `offset_words` 4-byte words (an immediate): the firmware's single-word `sta32`, past the
    /// L1 and L2 caches. The store
    /// completes at Idf(drc) then Wdf(drc).
    void Sta32(Reg base, uint8_t offset_words, Reg value, uint8_t drc);
    /// Mark the outstanding stores on counter `drc`, which a following Wdf waits for.
    void Idf(uint8_t drc);
    /// dst = src1 op src2 on 32-bit words. An immediate operand is 7 bits.
    void Bitwise(BwOp op, Reg dst, IOp src1, IOp src2, Pred pred = Pred::None);
    /// dst.x = src0.x * src1 + src2 on signed 16-bit lanes (mad.i16): src1 is a register
    /// holding the 16-bit multiplier in each lane, src2 a register or an 8-bit immediate.
    void MadI16(Reg dst, Reg src0, Reg src1, IOp src2);
    /// dst = src0.lo16 * imm16 + src2 on signed 32-bit words (mad.i32, the compiler's form
    /// for scaling a uniform index to a byte offset): imm is 7 bits.
    void MadI32(Reg dst, Reg src0, uint8_t imm, Reg src2);
    /// dst (one 32-bit register) = (int16) src.<comp> from an f32, rounded to nearest (SGX543
    /// has no other rounding).
    void PackS16(Reg dst, Reg src, uint8_t comp, Pred pred = Pred::None);
    /// VPCK to F32, unscaled, mask x: lane `select` of `src` (an even register, read as a 64-bit
    /// pair) into dst, the one register the unscaled form writes. `format` is the source's
    /// (VPCK's numbering: 0 U8, 1 S8, 4 S16).
    void UnpackInt(Reg dst, Reg src, uint8_t select, uint8_t format = 0);
    void Nop();
    /// The nop the compiler places right after a kill (a different word from Nop's).
    void NopAfterKill();
    /// Wait for outstanding texture reads on dependency counter `drc`.
    void Wdf(uint8_t drc);
    /// Discard the pixel when predicate p1 is false (the form the compiler emits after a
    /// cmp into p1). The op is a visibility test that reads a control word from a secondary
    /// attribute; the register is only known once the uniforms are allocated, so the word
    /// is left blank here and PatchKills fills it in.
    void KillUnlessP1();
    /// Write the fragment's depth from `src` (one 32-bit register); ends the phase like a
    /// kill. `control_sa` names the data block's 0xE000 word, as the kill does.
    void DepthF(Reg src, uint8_t control_sa);
    /// Point every kill at `control_sa`, the SA holding its control word (psp2cgc's data
    /// block's first literal, raw 0xE000); any other word draws wrong or faults.
    void PatchKills(uint32_t control_sa);

    /// dst = op(src1, src2), f32, two components on the unified store or four on an
    /// internal register. src2 has no negate: swap the sources or negate src1.
    void Vec(VecOp op, Dst dst, Src src1, Src src2, Pred pred = Pred::None, bool nosched = false);
    /// The f16 form of the same.
    void Vec16(VecOp op, Dst dst, Src src1, Src src2, Pred pred = Pred::None, bool nosched = false);
    /// One F32 vector instruction on a unified-store destination's pair (or an internal one).
    void VecPair(VecOp op, Dst dst, Src src1, Src src2, Pred pred, bool nosched);
    /// Vec's and Vec16's instruction word, as given.
    void VecWord(VecOp op, Dst dst, Src src1, Src src2, Pred pred, bool nosched);
    /// dst = src1 * i<gpi0> + i<gpi1>, four-wide (or three-wide) f32; dst is any bank. The
    /// predicate is the short form: none, p0 or !p0 only.
    void Mad(Dst dst, Src src1, Src gpi0, Src gpi1, bool vec4, Pred pred = Pred::None,
             bool nosched = false);
    /// The two-wide mad with three unified-store sources (psp2cgc's `mad.f32 o0.xy,
    /// pa0.xy, sa44.xx, sa48.xy`): dst = src0 * src1 + src2 on a pair. src0 is a
    /// temporary or primary attribute (absolute value allowed, no negation); every source
    /// swizzle is xx, yy or xy; src1 and src2 carry no modifier (the compiler was never
    /// seen to use one, so their encoding is unverified).
    void Mad2(Dst dst, Src src0, Src src1, Src src2, Pred pred = Pred::None, bool nosched = false);
    /// Whether a two-wide source's first two channels are a vmad2 swizzle (xx, yy, xy).
    static bool Mad2Swizzle(const Swizzle& s, uint64_t& code);
    /// dst.<one component> = dot(src1, i<gpi0>), three- or four-wide.
    /// `clip`, plane + 1: the dot is user clip plane `clip - 1`'s test (psp2cgc's
    /// 18d188811140003c for CLP0 into o10.x: VDP with bit 54 set and the plane in bits 36..34).
    void Dot(Dst dst, Src src1, Src gpi0, bool vec4, Pred pred = Pred::None, bool nosched = false,
             uint8_t clip = 0);
    /// dst.<mask> = op(src.<comp>): rcp, rsq, log2, exp2 on one component.
    void Comp(CompOp op, Dst dst, Src src, uint8_t src_comp, Pred pred = Pred::None,
              bool nosched = false);
    /// Four-wide f32 move src -> dst through the pack unit: reads two consecutive
    /// 64-bit halves (lo = src, hi = the register after it).
    void MoveF32x4(Dst dst, Reg lo, Reg hi, bool nosched = false);
    /// pack.f32.f16 dst.xyzw, src: the four F16 lanes of the pair at `src` as F32.
    void UnpackF16x4(Dst dst, Reg src);
    /// Four f32 in (lo, hi) packed to half4 at dst.
    void PackF16(Dst dst, Reg lo, Reg hi, bool nosched = false);
    /// Four-wide f32 move between internal registers with a swizzle.
    void MoveInternal(Dst dst, Src src, Pred pred = Pred::None, bool nosched = false);
    /// dst = (src0 != 0) ? src1 : src2, per component, f32. src0 is tested with the identity
    /// swizzle; src1 and src2 share the instruction's one swizzle and must name the same one.
    void CondMove(Dst dst, Src src0, Src src1, Src src2, bool nosched = false);
    /// p<pred_n> = src1.<chan> <cond> src2.x (src2 broadcast when `src2_scalar`).
    /// `pred` guards the write: with p1 as the guard and the destination, the result is
    /// the previous p1 AND the test, the compiler's way of folding two kills into one.
    /// TestAlu::Mul tests the product src1 * src2 against 0 instead of the difference.
    void Test(Cond cond, uint8_t pred_n, Src src1, uint8_t chan, Src src2, bool src2_scalar,
              Pred pred = Pred::None, TestAlu alu = TestAlu::Sub);
    /// A 2D texture read: four f32 at dst (temp or pa, pair-aligned) from sampler words at
    /// sa<sampler> (even), coordinates at coord.xy (pair-aligned); `lod` is an internal
    /// register holding the level in x, or
    /// null for the hardware's own level. Counter `drc` is what Wdf waits on.
    /// `point`: point sampled from a texture of one level (the result needs no neighbour).
    void Sample2D(Reg dst, Reg coord, uint8_t sampler_sa, const Reg* lod, uint8_t drc,
                  bool nosched = false, uint8_t fconv = 3, bool point = false);
    /// A probe (sgxd_flags smpskip): skip-invalid on the reads that are point sampled from one
    /// level, which run in a task's uncovered instances as every read with the hardware's own
    /// level does. Set before any program is emitted.
    static inline bool probe_skip_point_reads = false;
    /// The same for a cube texture: the direction at coord.xyz, three 32-bit registers from a
    /// pair boundary (CDIM 2, as psp2cgc encodes a dependent texCUBE), the hardware's level.
    void SampleCube(Reg dst, Reg coord, uint8_t sampler_sa, uint8_t drc, bool nosched = false);
    /// dst = c.csel1 * src1 c.cop c.csel2 * src2 on U8 registers (SOP2); `end` ends the program.
    /// Operands are temporaries, primary or secondary attributes or outputs, not internal.
    void Sop2(Reg dst, Reg src1, Reg src2, const SopCtl& c, bool end = false);
    /// The three-source form (SOP3): source 0 is a temporary or a primary attribute.
    void Sop3(Reg dst, Reg src0, Reg src1, Reg src2, const SopCtl& c, bool end = false);
    /// VPCK F32 to normalised U8 (x 255 to nearest, saturating): the four lanes at (lo, hi),
    /// as MoveF32x4 reads them, into the one register `dst`. `sel` picks each byte's source
    /// lane, two bits a byte from x up (PackSelIdentity: x, y, z, w; C0SSEL-C3SSEL).
    void PackU8(Reg dst, Reg lo, Reg hi, uint8_t sel = PackSelIdentity);
    /// The same from four F16 lanes in the one pair at `src` (pack.u8.f16.scl).
    void PackU8F16(Reg dst, Reg src, uint8_t sel = PackSelIdentity);
    /// VPCK normalised U8 to F32: bytes of the register `src` (even) as F32 lanes, `sel`
    /// each lane's byte as PackU8's. A unified-store destination takes two lanes (mask bits 2
    /// and 3 write nothing), so four bytes go to two pairs, the
    /// second's selects bytes 2 and 3.
    void UnpackU8(Dst dst, Reg src, uint8_t sel = PackSelIdentity);

private:
    /// Sample2D and SampleCube: `dims` coordinates (CDIM dims - 1); `fconv` the result's
    /// conversion (3 four F32 words, 2 four F16 in two words: Vita3K's fconv_type).
    void Sample(Reg dst, Reg coord, uint8_t dims, uint8_t sampler_sa, const Reg* lod, uint8_t drc,
                uint8_t fconv = 3, bool point = false);
    /// What FinishNosched needs of each word: the internal register components it reads
    /// and writes (bit = register * 4 + component) and where its nosched bit is (0: none).
    struct Meta {
        uint16_t reads;
        uint16_t writes;
        uint8_t bit;
    };
    /// A register as the current program addresses it (see `secondary`).
    Reg Map(Reg r) const;
    void Push(uint64_t word, uint8_t nosched_bit, uint16_t reads, uint16_t writes);
    static uint16_t Reads(const Src& src, uint8_t lanes);
    static uint16_t ReadsOne(Reg reg, uint8_t comp);
    static uint16_t ReadsAll(Reg reg);
    static uint16_t Writes(const Dst& dst, Pred pred);
    std::vector<uint64_t> words;
    std::vector<Meta> meta;
    std::vector<std::size_t> kill_indices;
};

} // namespace GxmRenderer::Usse
