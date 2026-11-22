// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <nihstro/shader_bytecode.h>
#include "video_core/renderer_sgxd/usse/usse_encoder.h"
#include "video_core/renderer_sgxd/usse/usse_ir.h"
#include "video_core/renderer_sgxd/usse/usse_program.h"
#include "video_core/renderer_sgxd/usse/vs_usse_gen.h"

/**
 * How a PICA vertex program becomes a USSE one (the forms are psp2cgc's for the same
 * operations, see vs_encoder_test):
 *
 * Registers. A PICA float4 lives in a quad of four consecutive registers of the unified
 * store: inputs a loader feeds in primary attributes (a quad per attribute), inputs with no
 * loader and the register-resident uniforms in secondary
 * attributes, everything else (temporaries, output attributes, the load quad, the
 * address registers as 16-bit integers, the loop state) in virtual quads that the IR
 * (usse_ir.h) places by liveness: temporaries first, then primary attributes past the
 * inputs, then the output registers the epilogue writes last. An operand with an
 * arbitrary swizzle is read four wide into an internal register (a four-wide op needs an
 * internal destination); the result lands in i2 and is moved to its quad two lanes at a
 * time.
 *
 * Uniforms. The 128 float4 uniforms cannot be register-resident (512 of 128 SAs), so
 * they sit in memory behind the resident uniforms: the ones a program reads with a
 * constant index ride in the SA image the secondary PDS program DMAs (per draw), the others
 * are loaded at their use with lda32 through the buffer pointer at the data block's head:
 * base + address register, wrapped to 128 entries and scaled to bytes with the compiler's
 * integer sequence.
 *
 * Control flow. The condition codes are the predicates p0 and p1 (CMP writes them, the
 * conditional forms branch on them), p2 is the scratch predicate, always used positive
 * since the vector ops have no !p2. Subroutines are inlined at each call, IF/ELSE and LOOP
 * are branches, END branches to the epilogue. A branch on per-vertex data puts the
 * program in per-instance mode (Program::per_instance), as the compiler does.
 */
namespace SgxdRenderer::Usse {


using nihstro::DestRegister;
using nihstro::Instruction;
using nihstro::OpCode;
using nihstro::RegisterType;
using nihstro::SourceRegister;
using nihstro::SwizzlePattern;

namespace {

/// The words an integer attribute of register i is fetched as: one for bytes, and for shorts
/// its halves, two to a word (the DMA reads from any byte address).
uint32_t RawWords(const VsRequest& req, uint32_t i) {
    if ((req.s16_regs & (1u << i)) == 0) {
        return 1;
    }
    uint32_t comps = req.reg_components[i];
    if (comps == 0 || comps > 4) {
        comps = 4;
    }
    return (comps + 1) >> 1;
}


[[noreturn]] void Refuse(const std::string& what) {
    throw std::runtime_error(what);
}

/// The static emit state's refusal, which EmitVertexProgram retries with the dynamic one.
constexpr const char* UnknownEmitState = "an EMIT whose SETEMIT is not on its straight-line path";

// Scratch, in the virtual store. One load quad: a PICA instruction reads at most one
// uniform (the inverted forms exist so that either operand can be it). The floats sit at
// even registers, where a compare reads them.
constexpr uint8_t TmpLoad = 0;    ///< uniform loaded at use (4)
constexpr uint8_t TmpA0X = 4;     ///< a0.x as int16
constexpr uint8_t TmpA0Y = 5;     ///< a0.y as int16
constexpr uint8_t TmpAlS16 = 6;   ///< aL as int16
constexpr uint8_t TmpAddr = 7;    ///< the byte offset of a dynamically indexed uniform
constexpr uint8_t TmpAlF = 8;     ///< aL as float
constexpr uint8_t TmpCount0 = 10; ///< loop counters by depth, float
constexpr uint8_t TmpCount1 = 12;
constexpr uint8_t TmpQuads = 16; ///< the first PICA quad
constexpr uint32_t VirtualLimit = 256;
constexpr uint32_t PaLimit = 64;
constexpr uint32_t SaLimit = 128;
/// Output registers free as scratch until the epilogue writes them.
constexpr uint8_t OutScratchEnd = 24;

// The resident secondary attributes: the depth mapping, then the input defaults and the
// integer uniforms.
constexpr uint32_t SaPositionScale = 0; // two: the clip x and y factors
constexpr uint32_t SaDepthScale = 2;
constexpr uint32_t SaDepthOffset = 3;
constexpr uint32_t SaClipZSign = 4; // its pair's second SA unused
constexpr uint32_t SaResidentHead = 6;

// The data block: the buffer pointer, then the literals. The ones a compare reads
// as a scalar sit at even offsets (a test reads the .x of a pair); the odd slots between
// them are unused.
enum DataSlot : uint32_t {
    DataPtr = 0,
    DataPad1 = 1,    ///< zero (the compiler keeps integer operands on even SAs too)
    DataNeg128 = 2,  ///< -128.0f
    Data0x10000 = 3, ///< the base of a register load offset
    Data127 = 4,     ///< 127.0f
    DataScratch0 = 5,
    DataEpsilon = 6, ///< 1e-6f
    DataScratch1 = 7,
    DataNeg1Eps = 8, ///< -1.00001f
    DataScratch2 = 9,
    DataNeg1 = 10, ///< -1.0f
    DataScratch3 = 11,
    DataOnes16 = 12, ///< 0x00010001: the mad.i16 multiplier for an add (even, like psp2cgc's)
    DataPad13 = 13,
    DataSize = 14,
    DataGsCap = DataSize, ///< Geometry: gs_max_triangles as a float, past the shared literals
};
constexpr uint32_t DataLiteralCount = DataSize - 1;

// Output registers of the fragment interface, in the order the output selects lay them out
// (OutSelects, TexSize): POSITION o0-o3, COLOR0 o4-o7, TEXCOORD0-2 two each from o8,
// TEXCOORD3 o14-17 (the fog table's coordinate, Usse::TextureIssue::fog: the PICA depth times
// w, and w, in its first and fourth lanes; texcoord0's w in its second), TEXCOORD4 o18-21 (the
// quaternion), TEXCOORD5 o22-24 (the view). Upstream's flat copy of the quaternion is not written:
// the fragment programs do not read it (FragmentEmitter::Lighting).
constexpr uint8_t OutPosition = 0;
constexpr uint8_t OutColor = 4;
constexpr uint8_t OutTexcoord0 = 8;
constexpr uint8_t OutTexcoord1 = 10;
constexpr uint8_t OutTexcoord2 = 12;
constexpr uint8_t OutTexcoord3 = 14;   ///< TEXCOORD3's u (depth times w) and v (texcoord0's w)
constexpr uint8_t OutTexcoord3St = 16; ///< TEXCOORD3's s (zero) and t (clip w)
constexpr uint8_t OutNormquat = 18;
constexpr uint8_t OutView = 22;
constexpr uint32_t OutSelects = 0x19001800; ///< 25 registers, position, colour
/// The texcoord widths, three bits a set: TC0-2 UV, TC3 UVST, TC4 UVST,
/// TC5 UVS. The sets present are contiguous: with TC5 left out and TC6 kept the fragment programs
/// read another set's data for TC6.
constexpr uint32_t TexSize = 0x1fe49;

// VSOutputAttributes::Semantic, as regs_rasterizer.h numbers them.
enum Semantic : uint8_t {
    SemPositionX = 0,
    SemQuaternionX = 4,
    SemColorR = 8,
    SemTexcoord0U = 12,
    SemTexcoord0V = 13,
    SemTexcoord1U = 14,
    SemTexcoord1V = 15,
    SemTexcoord0W = 16,
    SemViewX = 18,
    SemTexcoord2U = 22,
    SemTexcoord2V = 23,
};

const Src ConstZero{Reg::C(0), XXXX};
const Src ConstOne{Reg::C(2), XXXX};
const Src ConstZeroOne{Reg::C(1), XYZW}; ///< (0, 1) as a pair
const Src ConstHalf{Reg::C(12), XXXX};
const Reg I0 = Reg::I(0);
const Reg I1 = Reg::I(1);
const Reg I2 = Reg::I(2);

struct Quad {
    Bank bank;
    uint8_t base;
    Reg Base() const {
        return {bank, base};
    }
    Reg Pair(int which) const {
        return {bank, static_cast<uint8_t>(base + which * 2)};
    }
};

struct Operand {
    Quad quad;
    Swizzle swz;
    Mod mod;
};

/// An inlining of a code range: main, or a subroutine at one call site. Jumps resolve
/// within it.
struct Context {
    uint32_t begin;
    uint32_t end;
    std::map<uint32_t, int> labels; ///< PICA offset -> IR label
};

Ch ToCh(SwizzlePattern::Selector s) {
    switch (s) {
    case SwizzlePattern::Selector::x:
        return Ch::X;
    case SwizzlePattern::Selector::y:
        return Ch::Y;
    case SwizzlePattern::Selector::z:
        return Ch::Z;
    default:
        return Ch::W;
    }
}

Swizzle SelectorSwizzle(const SwizzlePattern& p, int source) {
    switch (source) {
    case 1:
        return {ToCh(p.src1_selector_0), ToCh(p.src1_selector_1), ToCh(p.src1_selector_2),
                ToCh(p.src1_selector_3)};
    case 2:
        return {ToCh(p.src2_selector_0), ToCh(p.src2_selector_1), ToCh(p.src2_selector_2),
                ToCh(p.src2_selector_3)};
    default:
        return {ToCh(p.src3_selector_0), ToCh(p.src3_selector_1), ToCh(p.src3_selector_2),
                ToCh(p.src3_selector_3)};
    }
}

uint8_t DestMask(const SwizzlePattern& p) {
    uint8_t mask = 0;
    for (int i = 0; i < 4; i++) {
        if (p.DestComponentEnabled(i)) {
            mask |= static_cast<uint8_t>(1u << i);
        }
    }
    return mask;
}

bool InVec4Table(const Swizzle& s) {
    static constexpr Swizzle table[] = {
        {Ch::X, Ch::X, Ch::X, Ch::X},   {Ch::Y, Ch::Y, Ch::Y, Ch::Y}, {Ch::Z, Ch::Z, Ch::Z, Ch::Z},
        {Ch::W, Ch::W, Ch::W, Ch::W},   {Ch::X, Ch::Y, Ch::Z, Ch::W}, {Ch::Y, Ch::Z, Ch::W, Ch::W},
        {Ch::X, Ch::Y, Ch::Z, Ch::Z},   {Ch::X, Ch::X, Ch::Y, Ch::Z}, {Ch::X, Ch::Y, Ch::X, Ch::Y},
        {Ch::X, Ch::Y, Ch::W, Ch::Z},   {Ch::Z, Ch::X, Ch::Y, Ch::W}, {Ch::Z, Ch::W, Ch::Z, Ch::W},
        {Ch::Y, Ch::Z, Ch::X, Ch::Z},   {Ch::X, Ch::X, Ch::Y, Ch::Y}, {Ch::X, Ch::Z, Ch::W, Ch::W},
        {Ch::X, Ch::Y, Ch::Z, Ch::One}, {Ch::Y, Ch::Z, Ch::X, Ch::W}, {Ch::Z, Ch::W, Ch::X, Ch::Y},
        {Ch::X, Ch::Z, Ch::W, Ch::Y},   {Ch::Y, Ch::Y, Ch::W, Ch::W}, {Ch::W, Ch::Y, Ch::Z, Ch::W},
        {Ch::W, Ch::Z, Ch::W, Ch::Z},   {Ch::X, Ch::Y, Ch::Z, Ch::X}, {Ch::Z, Ch::Z, Ch::W, Ch::W},
        {Ch::X, Ch::W, Ch::Z, Ch::X},   {Ch::Y, Ch::Y, Ch::Y, Ch::X}, {Ch::Y, Ch::Y, Ch::Y, Ch::Z},
        {Ch::X, Ch::Z, Ch::Y, Ch::W},   {Ch::X, Ch::X, Ch::X, Ch::Y}, {Ch::Z, Ch::Y, Ch::X, Ch::W},
        {Ch::Y, Ch::Y, Ch::Z, Ch::Z},   {Ch::Z, Ch::Z, Ch::Z, Ch::Y},
    };
    for (const auto& t : table) {
        if (t == s) {
            return true;
        }
    }
    return false;
}

class VertexEmitter {
public:
    VertexEmitter(const VsRequest& request_, int level_, bool dynamic_emit_, Scratch scratch)
        : req{request_}, level{level_}, dynamic_emit{dynamic_emit_} {
        e.scratch = scratch;
    }

    std::optional<Program> Emit(uint32_t& input_regs);
    int level = 1;

private:
    struct Scan {
        uint16_t inputs = 0;                 ///< input registers read
        uint16_t temps = 0;                  ///< temporaries touched
        std::array<uint8_t, 16> temp_init{}; ///< lanes read before (linearly) written
        std::array<uint8_t, 16> temp_written{};
        uint16_t attrs_written = 0;
        std::array<uint16_t, 128> uniform_uses{};
        bool any_uniform = false;
        bool a0x = false, a0y = false, al = false;
        uint8_t ints = 0; ///< integer uniforms LOOP reads
        bool cc = false;  ///< a conditional form reads the condition codes
    };
    void ScanProgram();
    void FindPositionRows();
    void MarkReachable(uint32_t begin, uint32_t end);
    /// OpenContext's walk over [begin, end) from `entry`: the instructions a context's emission
    /// may pass; `backward` takes each taken jump's target at or before the jump. A subroutine's
    /// walk goes on past every jump (its range is emitted whole); main's (`precise`) not past a
    /// JMPU its baked bool always takes, nor an END: main emits what it passes (main_walk).
    std::vector<bool> Walk(uint32_t begin, uint32_t end, uint32_t entry, bool precise,
                           const std::function<void(uint32_t)>& backward) const;
    void NoteSource(const SourceRegister& reg, uint32_t addr, const Swizzle& swz);
    void NoteDest(const DestRegister& reg, uint8_t mask);

    void Layout();
    Quad AllocateQuad();

    void Prologue();
    void Epilogue();
    /// In main's context, the instructions main's walk does not pass (main_walk) are left out.
    uint32_t EmitRange(uint32_t begin, uint32_t end);
    uint32_t EmitInstruction(uint32_t offset);
    void EmitArithmetic(const Instruction& instr, const SwizzlePattern& swz);
    void EmitMad(const Instruction& instr, const SwizzlePattern& swz);

    Operand Source(const SourceRegister& reg, uint32_t addr, const Swizzle& swz, bool negate);
    std::optional<Quad> Dest(const DestRegister& reg);
    Src Materialize(Reg internal, const Operand& op);
    /// The operand as a four-wide source straight from the unified store (V32NMAD's and
    /// VDP's first source take any swizzle).
    static Src Direct(const Operand& op) {
        return {op.quad.Base(), op.swz, op.mod};
    }
    /// A source for one lane of a two-wide move: the pair holding the lane's channel.
    static Src LaneSrc(const Operand& op, int lane) {
        const Ch ch = op.swz[lane];
        return {op.quad.Pair(static_cast<int>(ch) / 2), (static_cast<int>(ch) & 1) ? YYYY : XXXX,
                op.mod};
    }
    void Store(const Quad& dst, uint8_t mask, Reg internal);
    Src Data(DataSlot slot) const {
        return {Reg::Sa(static_cast<uint8_t>(data_base + slot)), XXXX};
    }
    Reg DataReg(DataSlot slot) const {
        return Reg::Sa(static_cast<uint8_t>(data_base + slot));
    }

    int NewLabel();
    int LabelAt(uint32_t offset);
    void OpenContext(uint32_t begin, uint32_t end, uint32_t entry, bool precise);
    void Bind(int label);
    bool Bound(int label) const {
        return label_bound[static_cast<std::size_t>(label)];
    }
    void Branch(Pred pred, int target);
    void CondBranch(Instruction::FlowControlType flow, int target, bool when_true);
    std::optional<bool> KnownBool(uint32_t id) const {
        if (id < 16 && (req.bool_mask & (1u << id)) != 0) {
            return (req.bools & (1u << id)) != 0;
        }
        return std::nullopt;
    }

    /// The pair and lane a fragment-interface semantic comes from, or nothing for 1.0.
    std::optional<Src> SemanticSrc(Semantic s) const;
    void GatherFour(Reg internal, Semantic first);
    void OutputPair(uint8_t out, Semantic s0, Semantic s1);
    void OutputTexcoord(uint8_t out, Semantic su, Semantic sv);
    void OutputLane(uint8_t out, int lane, Semantic s);

    const VsRequest& req;
    Scan scan;
    std::vector<bool> reachable;
    /// Main's walk over the whole program from its entry (ScanProgram): what main emits.
    std::vector<bool> main_walk;
    Ir::Builder e;   ///< the primary program
    Program program;

    std::array<int, 16> input_pa{};   ///< input register -> PA base, or -1
    std::array<int, 16> input_raw_pa{}; ///< a U8 input register -> its fetched word's PA, or -1
    std::array<int, 16> default_sa{}; ///< input register -> SA base, or -1
    std::array<std::optional<Quad>, 16> temp_quad{};
    std::array<std::optional<Quad>, 16> attr_quad{};
    std::array<int, 128> uniform_sa{}; ///< uniform -> SA base when prefetched, or -1
    uint32_t resident = 0;             ///< the resident uniforms' size (floats)
    uint32_t data_base = 0;            ///< the data block's first SA
    uint32_t sa_top = 0;               ///< SAs in use
    int int_sa = -1;                   ///< uniforms_i's SA base
    uint8_t pa_inputs = 0;             ///< PA registers the attributes take
    uint32_t next_virtual = TmpQuads;  ///< the next free virtual quad's first register
    std::vector<uint8_t> prefetch;     ///< uniforms the SA image carries, by index

    std::vector<bool> label_bound;
    int end_label = -1;
    std::vector<Context> contexts;
    int loop_depth = 0;
    std::vector<int> loop_breaks; ///< each open LOOP's label for a BREAKC (-1: none yet)
    bool per_instance = false;

    int out_address_pa = -1;          ///< StoreOutputs: the vertex's store address
    std::vector<Quad> gs_row_quads;   ///< Geometry, point mode: input register v<r>
    std::optional<Quad> gs_zero_inputs;        ///< Geometry, uniform modes: what v<r> reads (zero)
    std::array<std::optional<Quad>, 16> gs_input_quads; ///< Geometry: VsRequest::gs_input_rows
    std::array<std::vector<Quad>, 3> gs_slots; ///< Geometry: the emit buffer's three vertices
    Quad gs_state{};                  ///< Geometry: x the triangles stored (float), y a triangle's
                                      ///< bytes / 16, z the next triangle's address (ints)
    /// Geometry, gs_serial: the invocation's table entry (loaded words), the loop's state (x
    /// the invocations left, a float; y the next entry's address less 4; z 1) and its labels.
    std::vector<Quad> gs_entry_quads;
    Quad gs_loop{};
    int gs_loop_top = -1;
    uint32_t gs_entry_words = 0;
    Reg GsWord(uint32_t k) const {
        if (!req.gs_serial) {
            return Reg::Pa(static_cast<uint8_t>(k));
        }
        return Reg::V(static_cast<uint8_t>(gs_entry_quads[k / 4].base + k % 4));
    }
    /// The last SETEMIT on the straight-line path to here, or nothing past a join.
    struct EmitState {
        uint8_t vertex_id;
        bool prim;
        bool winding;
    };
    std::optional<EmitState> emit_state;
    /// Geometry: the emit state in registers as well (gs_emit: x the vertex id, y the
    /// primitive flag, z the winding flag, as floats), for an EMIT no straight-line SETEMIT
    /// reaches (GsEmitDynamic). Only for a program the static form refuses.
    bool dynamic_emit = false;
    Quad gs_emit{};
    /// Whether a SETEMIT ran on every path to here: upstream's emitter keeps its state from the
    /// invocation before, which no invocation here sees. The same over the jumps into each
    /// label not bound yet (-1 for none), and whether the code here is reached falling through.
    bool setemit_done = false;
    std::vector<int8_t> label_setemit;
    bool falls_through = true;
    /// Literals past the shared ones: Geometry's cap and a pad, so the prefetched uniforms
    /// after them stay on even SAs (a pair operand addresses them).
    uint32_t ExtraLiterals() const {
        return req.mode == VsMode::Geometry ? 2u : 0u;
    }
    std::optional<Operand> GsTableRead(uint32_t index, uint32_t addr, const Swizzle& swz,
                                       Mod mod, Reg table);
    std::optional<Operand> GsSource(const SourceRegister& reg, uint32_t addr, const Swizzle& swz,
                                    Mod mod);
    void GsEmit();
    void GsEmitDynamic();
    void GsCopyToSlot(uint8_t vertex_id);
    Reg GsStateSlot(uint32_t v, Reg block);
    void GsStoreTriangle(bool winding);
    void StoreQuad(Reg base, uint32_t word, const Quad& q);
    void EpilogueStreamOut();
};

void VertexEmitter::NoteSource(const SourceRegister& reg, uint32_t addr, const Swizzle& swz) {
    const uint32_t index = static_cast<uint32_t>(reg.GetIndex());
    uint8_t lanes = 0;
    for (const Ch ch : swz) {
        if (ch <= Ch::W) {
            lanes |= static_cast<uint8_t>(1u << static_cast<int>(ch));
        }
    }
    switch (reg.GetRegisterType()) {
    case RegisterType::Input:
        scan.inputs |= static_cast<uint16_t>(1u << (index & 15));
        break;
    case RegisterType::Temporary:
        scan.temps |= static_cast<uint16_t>(1u << (index & 15));
        scan.temp_init[index & 15] |= static_cast<uint8_t>(lanes & ~scan.temp_written[index & 15]);
        break;
    case RegisterType::FloatUniform:
        scan.any_uniform = true;
        if (addr == 0) {
            scan.uniform_uses[index & 127]++;
        } else if (addr == 1) {
            scan.a0x = true;
        } else if (addr == 2) {
            scan.a0y = true;
        } else {
            scan.al = true;
        }
        break;
    default:
        break;
    }
}

void VertexEmitter::NoteDest(const DestRegister& reg, uint8_t mask) {
    const uint32_t index = static_cast<uint32_t>(reg.GetIndex());
    if (reg.GetRegisterType() == RegisterType::Temporary) {
        scan.temps |= static_cast<uint16_t>(1u << (index & 15));
        scan.temp_written[index & 15] |= mask;
    } else if (reg.GetRegisterType() == RegisterType::Output) {
        const uint32_t attr = req.output_map[index & 15];
        if (attr < req.num_outputs) {
            scan.attrs_written |= static_cast<uint16_t>(1u << attr);
        }
    }
}

/// Marks the instructions main can reach: the code buffer holds every program the title
/// uploaded, and scanning them all would charge this program for their registers.
void VertexEmitter::MarkReachable(uint32_t begin, uint32_t end) {
    for (uint32_t offset = begin; offset < end && offset < reachable.size(); offset++) {
        if (reachable[offset]) {
            return; // joined a range already walked
        }
        reachable[offset] = true;
        const Instruction instr = {req.code[offset]};
        const auto& flow = instr.flow_control;
        switch (instr.opcode.Value()) {
        case OpCode::Id::END:
            return;
        case OpCode::Id::JMPC:
        case OpCode::Id::JMPU:
            MarkReachable(flow.dest_offset, end);
            break;
        case OpCode::Id::CALL:
        case OpCode::Id::CALLC:
        case OpCode::Id::CALLU:
            MarkReachable(flow.dest_offset, flow.dest_offset + flow.num_instructions);
            break;
        case OpCode::Id::IFC:
        case OpCode::Id::IFU:
            MarkReachable(offset + 1, flow.dest_offset);
            MarkReachable(flow.dest_offset, flow.dest_offset + flow.num_instructions);
            offset = flow.dest_offset + flow.num_instructions - 1;
            break;
        case OpCode::Id::LOOP:
            MarkReachable(offset + 1, flow.dest_offset + 1);
            offset = flow.dest_offset;
            break;
        default:
            break;
        }
    }
}

/// Where the position's z and w come from (Program::position_rows): each written by exactly one
/// reachable instruction, a DP4 of a float uniform read without the address register and one
/// register, the same register through the same lanes for both, nothing writing it between the
/// two in program order.
void VertexEmitter::FindPositionRows() {
    struct Found {
        uint32_t offset = 0;
        uint32_t writers = 0;
    };
    std::array<Found, 2> found{};
    std::array<uint8_t, 2> lane{};
    std::array<uint32_t, 2> attr{};
    for (int k = 0; k < 2; k++) {
        const VsSemantic& m = req.semantics[SemPositionX + 2 + k];
        attr[k] = m.attribute;
        lane[k] = m.component;
    }
    const uint32_t extent = static_cast<uint32_t>(reachable.size());
    for (uint32_t offset = 0; offset < extent; offset++) {
        if (!reachable[offset]) {
            continue;
        }
        const Instruction instr = {req.code[offset]};
        const OpCode::Info info = instr.opcode.Value().GetInfo();
        uint8_t mask = 0;
        DestRegister dest;
        if (info.type == OpCode::Type::Arithmetic) {
            dest = instr.common.dest.Value();
            mask = DestMask(SwizzlePattern{req.swizzle[instr.common.operand_desc_id]});
        } else if (info.type == OpCode::Type::MultiplyAdd) {
            dest = instr.mad.dest.Value();
            mask = DestMask(SwizzlePattern{req.swizzle[instr.mad.operand_desc_id]});
        } else {
            continue;
        }
        if (dest.GetRegisterType() != RegisterType::Output) {
            continue;
        }
        const uint32_t out_attr = req.output_map[static_cast<uint32_t>(dest.GetIndex()) & 15];
        for (int k = 0; k < 2; k++) {
            if (out_attr == attr[k] && (mask >> lane[k] & 1) != 0) {
                found[k].offset = offset;
                found[k].writers++;
            }
        }
    }
    std::array<Program::PositionRow, 2> rows{};
    SourceRegister shared{};
    Swizzle shared_lanes{};
    for (int k = 0; k < 2; k++) {
        if (found[k].writers != 1) {
            return;
        }
        const Instruction instr = {req.code[found[k].offset]};
        if (instr.opcode.Value().EffectiveOpCode() != OpCode::Id::DP4) {
            return;
        }
        const SwizzlePattern swz = {req.swizzle[instr.common.operand_desc_id]};
        const SourceRegister s1 = instr.common.GetSrc1(false);
        const SourceRegister s2 = instr.common.GetSrc2(false);
        // DP4's second source cannot be a uniform: the first is the row.
        if (s1.GetRegisterType() != RegisterType::FloatUniform ||
            instr.common.address_register_index != 0 ||
            (s2.GetRegisterType() != RegisterType::Temporary &&
             s2.GetRegisterType() != RegisterType::Input)) {
            return;
        }
        const Swizzle row_lanes = SelectorSwizzle(swz, 1);
        const Swizzle source_lanes = SelectorSwizzle(swz, 2);
        if (k == 0) {
            shared = s2;
            shared_lanes = source_lanes;
        } else if (s2.GetRegisterType() != shared.GetRegisterType() ||
                   s2.GetIndex() != shared.GetIndex() || source_lanes != shared_lanes) {
            return;
        }
        Program::PositionRow& row = rows[k];
        row.uniform = static_cast<int8_t>(s1.GetIndex());
        for (int c = 0; c < 4; c++) {
            row.uniform_lane[c] = static_cast<uint8_t>(row_lanes[c]);
            row.source_lane[c] = static_cast<uint8_t>(source_lanes[c]);
        }
        row.negate = static_cast<bool>(swz.negate_src1) != static_cast<bool>(swz.negate_src2);
    }
    if (shared.GetRegisterType() == RegisterType::Temporary) {
        const uint32_t lo = std::min(found[0].offset, found[1].offset);
        const uint32_t hi = std::max(found[0].offset, found[1].offset);
        for (uint32_t offset = lo + 1; offset < hi; offset++) {
            const Instruction instr = {req.code[offset]};
            const OpCode::Info info = instr.opcode.Value().GetInfo();
            DestRegister dest;
            if (info.type == OpCode::Type::Arithmetic) {
                dest = instr.common.dest.Value();
            } else if (info.type == OpCode::Type::MultiplyAdd) {
                dest = instr.mad.dest.Value();
            } else if (info.type == OpCode::Type::UniformFlowControl ||
                       info.type == OpCode::Type::Conditional) {
                return; // a branch between them: no single order
            } else {
                continue;
            }
            if (dest.GetRegisterType() == RegisterType::Temporary &&
                dest.GetIndex() == shared.GetIndex()) {
                return;
            }
        }
    }
    program.position_rows = rows;
}

void VertexEmitter::ScanProgram() {
    const uint32_t extent =
        std::min<uint32_t>(req.code_size, static_cast<uint32_t>(req.code.size()));
    reachable.assign(extent, false);
    MarkReachable(req.main_offset, extent);
    // Main emits what its walk passes (Emit), on either side of its entry: the scan takes all
    // of it in. MarkReachable stops where it joins a range already marked, which may have been a
    // subroutine's shorter one.
    main_walk = Walk(0, extent, req.main_offset, true, nullptr);
    for (uint32_t offset = 0; offset < extent; offset++) {
        if (main_walk[offset] && !reachable[offset]) {
            MarkReachable(offset, extent);
        }
    }
    for (uint32_t offset = 0; offset < extent; offset++) {
        if (!reachable[offset]) {
            continue;
        }
        const Instruction instr = {req.code[offset]};
        const OpCode::Info info = instr.opcode.Value().GetInfo();
        if (info.type == OpCode::Type::Arithmetic) {
            const SwizzlePattern swz = {req.swizzle[instr.common.operand_desc_id]};
            const bool inverted = (info.subtype & OpCode::Info::SrcInversed) != 0;
            NoteSource(instr.common.GetSrc1(inverted),
                       !inverted * instr.common.address_register_index, SelectorSwizzle(swz, 1));
            NoteSource(instr.common.GetSrc2(inverted),
                       inverted * instr.common.address_register_index, SelectorSwizzle(swz, 2));
            const auto op = instr.opcode.Value().EffectiveOpCode();
            if (op == OpCode::Id::MOVA) {
                if (swz.DestComponentEnabled(0)) {
                    scan.a0x = true;
                }
                if (swz.DestComponentEnabled(1)) {
                    scan.a0y = true;
                }
            } else if (op != OpCode::Id::CMP) {
                NoteDest(instr.common.dest.Value(), DestMask(swz));
            }
        } else if (info.type == OpCode::Type::MultiplyAdd) {
            const SwizzlePattern swz = {req.swizzle[instr.mad.operand_desc_id]};
            const bool inverted = instr.opcode.Value().EffectiveOpCode() == OpCode::Id::MADI;
            NoteSource(instr.mad.GetSrc1(inverted), 0, SelectorSwizzle(swz, 1));
            NoteSource(instr.mad.GetSrc2(inverted), !inverted * instr.mad.address_register_index,
                       SelectorSwizzle(swz, 2));
            NoteSource(instr.mad.GetSrc3(inverted), inverted * instr.mad.address_register_index,
                       SelectorSwizzle(swz, 3));
            NoteDest(instr.mad.dest.Value(), DestMask(swz));
        } else {
            switch (instr.opcode.Value()) {
            case OpCode::Id::LOOP:
                scan.ints |= static_cast<uint8_t>(1u << (instr.flow_control.int_uniform_id & 3));
                scan.al = true;
                break;
            case OpCode::Id::IFC:
            case OpCode::Id::JMPC:
            case OpCode::Id::CALLC:
            case OpCode::Id::BREAKC:
                scan.cc = true;
                break;
            default:
                break;
            }
        }
    }
}

Quad VertexEmitter::AllocateQuad() {
    if (next_virtual + 4 > VirtualLimit) {
        Refuse("virtual register budget");
    }
    const Quad q{Bank::Virtual, static_cast<uint8_t>(next_virtual)};
    next_virtual += 4;
    return q;
}

void VertexEmitter::Layout() {
    input_pa.fill(-1);
    default_sa.fill(-1);
    uniform_sa.fill(-1);

    // A geometry program's attribute is its row addresses (VsMode); its inputs are loaded.
    if (req.mode == VsMode::Geometry) {
        if (req.gs_uniform_table && (!req.gs_rows_uniform || req.gs_rows_table)) {
            Refuse("a uniform table outside the fixed-primitive mode");
        }
        const uint32_t words =
            req.gs_rows_table
                ? 4
                : 2 + req.gs_rows + static_cast<uint32_t>(std::popcount(req.gs_input_rows)) +
                      (req.gs_uniform_table ? 1u : 0u);
        if (words + 2 > PaLimit) {
            Refuse("more geometry input rows than primary attributes");
        }
        if (req.gs_serial && req.gs_rows_table) {
            Refuse("the variable-primitive mode's invocations in order (temporaries kept)");
        }
        gs_entry_words = words;
        const uint32_t attribute_words = req.gs_serial ? 4 : words;
        program.attributes.push_back(
            {AttributeKind::GsInRows, 0, 0, static_cast<uint8_t>(attribute_words)});
        pa_inputs = static_cast<uint8_t>((attribute_words + 1) & ~1u);
    }
    // Attributes: four PAs each, in register order; the components a loader does not
    // supply are filled by the prologue.
    for (uint32_t i = 0; i < 16 && req.mode != VsMode::Geometry; i++) {
        if ((scan.inputs & (1u << i)) == 0 || (req.default_regs & (1u << i)) != 0) {
            continue;
        }
        input_pa[i] = pa_inputs;
        if ((req.u8_regs & (1u << i)) == 0) {
            uint32_t comps = req.reg_components[i];
            if (comps == 0 || comps > 4) {
                comps = 4;
            }
            program.attributes.push_back({AttributeKind::Input, static_cast<uint8_t>(i), pa_inputs,
                                      static_cast<uint8_t>(comps)});
        }
        pa_inputs = static_cast<uint8_t>(pa_inputs + 4);
    }
    // An integer attribute's words (one for bytes, two for four shorts) land in a pair of PAs
    // of their own after the quads, even as the unpack needs (docs/sgxd/usse.md); the prologue
    // unpacks them into the quad.
    input_raw_pa.fill(-1);
    for (uint32_t i = 0; i < 16; i++) {
        if (input_pa[i] < 0 || (req.u8_regs & (1u << i)) == 0) {
            continue;
        }
        input_raw_pa[i] = pa_inputs;
        const uint32_t words = RawWords(req, i);
        program.attributes.push_back({AttributeKind::Input, static_cast<uint8_t>(i), pa_inputs,
                                  static_cast<uint8_t>(words)});
        pa_inputs = static_cast<uint8_t>(pa_inputs + (words > 2 ? 4 : 2));
    }
    if (req.mode == VsMode::StoreOutputs) {
        out_address_pa = pa_inputs;
        program.attributes.push_back({AttributeKind::OutAddress, 0, pa_inputs, 1});
        pa_inputs = static_cast<uint8_t>(pa_inputs + 2);
    }

    program.uniforms.push_back({UniformKind::PositionScale, 0, SaPositionScale, 2, 1});
    program.uniforms.push_back({UniformKind::DepthScale, 0, SaDepthScale, 1, 1});
    program.uniforms.push_back({UniformKind::DepthOffset, 0, SaDepthOffset, 1, 1});
    program.uniforms.push_back({UniformKind::ClipZSign, 0, SaClipZSign, 1, 1});
    resident = SaResidentHead;
    for (uint32_t i = 0; i < 16 && req.mode != VsMode::Geometry; i++) {
        if ((scan.inputs & (1u << i)) != 0 && (req.default_regs & (1u << i)) != 0) {
            default_sa[i] = static_cast<int>(resident);
            program.uniforms.push_back(
                {UniformKind::DefaultInput, static_cast<uint8_t>(i), resident, 4, 1});
            resident += 4;
        }
    }
    if (scan.ints != 0) {
        int_sa = static_cast<int>(resident);
        program.uniforms.push_back({UniformKind::IntUniforms, 0, resident, 4, 4});
        resident += 16;
    }
    program.uniform_floats = resident;

    data_base = resident;
    sa_top = data_base + DataSize + ExtraLiterals();
    std::vector<uint8_t> order;
    for (uint32_t n = 0; n < 128; n++) {
        const bool row = req.mode == VsMode::Geometry && req.gs_rows_uniform &&
                         n >= req.gs_first_uniform && n < req.gs_first_uniform + req.gs_rows;
        if (scan.uniform_uses[n] != 0 && !row) {
            order.push_back(static_cast<uint8_t>(n));
        }
    }
    std::stable_sort(order.begin(), order.end(), [&](uint8_t a, uint8_t b) {
        return scan.uniform_uses[a] > scan.uniform_uses[b];
    });
    for (const uint8_t n : order) {
        if (sa_top + 4 > SaLimit) {
            break;
        }
        uniform_sa[n] = static_cast<int>(sa_top);
        prefetch.push_back(n);
        sa_top += 4;
    }
    std::sort(prefetch.begin(), prefetch.end());
    // The bank, for what the SAs do not hold: an indexed read, a row read with a constant index that
    // was not prefetched (one past the SAs, a geometry program's own rows), and the variable-primitive
    // mode's tables, which point past an invocation's rows into it. A program whose reads all fit has
    // none: the pass copies no bank for it (its pointer slot stays, as a literal).
    bool bank = scan.a0x || scan.a0y || scan.al || req.gs_rows_table || req.gs_uniform_table;
    for (uint32_t n = 0; n < 128 && !bank; n++) {
        bank = scan.uniform_uses[n] != 0 && uniform_sa[n] < 0;
    }
    if (bank) {
        // 128 entries plus a mirror of the first four: an indexed read whose index wraps at 128 may
        // be issued at an immediate offset from a neighbouring row's address, and the offset runs
        // into the mirror.
        program.uniforms.push_back(
            {UniformKind::FloatBank, 0, resident, 4, 128 + Ir::UniformMirrorRows});
        program.buffer_pointer = true;
        program.ldst_base_value = static_cast<int32_t>(resident * 4) - 4;
        program.buffer_floats = resident + 512 + Ir::UniformMirrorRows * 4;
    }
    // The header's secondary register count decides what the hardware allocates: undeclared SA
    // quads read as black screens.
    program.secondary_reg_count = sa_top;

    for (uint32_t k = 0; k < req.num_outputs && k < 16; k++) {
        attr_quad[k] = AllocateQuad();
    }
    for (uint32_t t = 0; t < 16; t++) {
        if ((scan.temps & (1u << t)) != 0) {
            temp_quad[t] = AllocateQuad();
        }
    }
    if (req.mode == VsMode::Geometry) {
        if (req.num_outputs * 12 > 127) {
            Refuse("a geometry program's triangle is past a store's immediate offset");
        }
        for (uint32_t r = 0; r < req.gs_rows && !req.gs_rows_uniform; r++) {
            gs_row_quads.push_back(AllocateQuad());
        }
        if (scan.inputs >> (req.gs_rows_uniform ? 0 : std::min<uint32_t>(req.gs_rows, 16)) != 0) {
            gs_zero_inputs = AllocateQuad();
        }
        if (req.gs_input_rows != 0 && (!req.gs_rows_uniform || req.gs_rows_table)) {
            Refuse("input rows of their own outside the fixed-primitive mode");
        }
        for (uint32_t r = 0; r < 16; r++) {
            if ((req.gs_input_rows >> r & 1) != 0) {
                gs_input_quads[r] = AllocateQuad();
            }
        }
        if (req.gs_serial && req.num_outputs > 16) {
            Refuse("more than 16 output attributes in the in-order state block");
        }
        for (auto& slot : gs_slots) {
            for (uint32_t k = 0; k < req.num_outputs; k++) {
                slot.push_back(AllocateQuad());
            }
        }
        gs_state = AllocateQuad();
        if (dynamic_emit) {
            gs_emit = AllocateQuad();
        }
        if (req.gs_serial) {
            for (uint32_t q = 0; q < (gs_entry_words + 3) / 4; q++) {
                gs_entry_quads.push_back(AllocateQuad());
            }
            gs_loop = AllocateQuad();
        }
    }
}

/// Point mode's rows are the input registers, loaded by the prologue. The uniform modes' rows
/// are loaded where they are read, as the uniforms not prefetched are: particle systems read
/// dozens of rows in their loops, more than the registers hold.
std::optional<Operand> VertexEmitter::GsSource(const SourceRegister& reg, uint32_t addr,
                                               const Swizzle& swz, Mod mod) {
    if (req.mode != VsMode::Geometry) {
        return std::nullopt;
    }
    const uint32_t index = static_cast<uint32_t>(reg.GetIndex());
    switch (reg.GetRegisterType()) {
    case RegisterType::Input:
        // A register the draw's rows do not fill (the uniform modes fill none) keeps what the
        // unit had: an earlier point-mode draw's last row, or zero. Point mode's table row for it
        // points at either; the fixed-primitive mode reads a row of its own (gs_input_rows) or zero.
        if (req.gs_rows_uniform && index < 16 && gs_input_quads[index]) {
            return Operand{*gs_input_quads[index], swz, mod};
        }
        if (req.gs_rows_uniform || index >= req.gs_rows) {
            return Operand{*gs_zero_inputs, swz, mod};
        }
        return Operand{gs_row_quads[index], swz, mod};
    case RegisterType::FloatUniform: {
        if (!req.gs_rows_uniform) {
            return std::nullopt;
        }
        if (req.gs_rows_table) {
            return GsTableRead(index, addr, swz, mod, Reg::Pa(2));
        }
        if (addr != 0) {
            if (!req.gs_uniform_table) {
                Refuse("an indexed uniform read in a geometry program with its inputs in "
                       "uniforms, without its table");
            }
            return GsTableRead(index, addr, swz, mod, GsWord(gs_entry_words - 1));
        }
        const uint32_t n = index & 127;
        if (n < req.gs_first_uniform || n >= req.gs_first_uniform + req.gs_rows) {
            return std::nullopt;
        }
        const Reg dst = Reg::V(TmpLoad);
        e.Lda32(dst, GsWord(2 + n - req.gs_first_uniform), nullptr, 0, 4, 0);
        e.WdfVertex(0);
        return Operand{{Bank::Virtual, TmpLoad}, swz, mod};
    }
    default:
        return std::nullopt;
    }
}

/// A read through the invocation's table (`table` holds its address less 4, and a fetch reads
/// one word on): entry n, then the row that entry points at. The variable-primitive mode's
/// constant index past every invocation's rows reads the shared uniform as usual.
std::optional<Operand> VertexEmitter::GsTableRead(uint32_t index, uint32_t addr,
                                                  const Swizzle& swz, Mod mod, Reg table) {
    const uint32_t n = index & 127;
    if (addr == 0 && n >= req.gs_rows) {
        return std::nullopt;
    }
    const Reg dst = Reg::V(TmpLoad);
    if (addr == 0) {
        e.Lda32(dst, table, nullptr, static_cast<uint8_t>(n), 4, 0);
    } else {
        // (index + a) & 127, times 4, plus the table: as the indexed uniform read's address.
        const Reg areg = addr == 1 ? Reg::V(TmpA0X) : addr == 2 ? Reg::V(TmpA0Y) : Reg::V(TmpAlS16);
        const Reg address = Reg::V(TmpAddr);
        e.MadI16(address, areg, DataReg(DataOnes16), IOp::Imm(static_cast<uint8_t>(n)));
        e.Bitwise(BwOp::And, address, IOp::Imm(0x7f), IOp::R(address));
        e.MadI32(address, address, 4, table);
        e.Lda32(dst, address, nullptr, 0, 4, 0);
    }
    e.WdfVertex(0);
    e.Lda32(dst, dst, nullptr, 0, 4, 0);
    e.WdfVertex(0);
    return Operand{{Bank::Virtual, TmpLoad}, swz, mod};
}

Operand VertexEmitter::Source(const SourceRegister& reg, uint32_t addr, const Swizzle& swz,
                              bool negate) {
    constexpr uint8_t load_quad = TmpLoad;
    const uint32_t index = static_cast<uint32_t>(reg.GetIndex());
    const Mod mod = negate ? Mod::Neg : Mod::None;
    if (const auto row = GsSource(reg, addr, swz, mod)) {
        return *row;
    }
    switch (reg.GetRegisterType()) {
    case RegisterType::Input:
        if (input_pa[index & 15] >= 0) {
            return {{Bank::PrimAttr, static_cast<uint8_t>(input_pa[index & 15])}, swz, mod};
        }
        if (default_sa[index & 15] >= 0) {
            return {{Bank::SecAttr, static_cast<uint8_t>(default_sa[index & 15])}, swz, mod};
        }
        Refuse("input register without a place");
    case RegisterType::Temporary: {
        const uint32_t t = index & 15;
        if (!temp_quad[t]) {
            Refuse("temporary without a place");
        }
        return {*temp_quad[t], swz, mod};
    }
    case RegisterType::FloatUniform: {
        const Reg ptr = DataReg(DataPtr);
        const Reg dst = Reg::V(load_quad);
        if (addr == 0) {
            if (uniform_sa[index & 127] >= 0) {
                return {{Bank::SecAttr, static_cast<uint8_t>(uniform_sa[index & 127])}, swz, mod};
            }
            const uint32_t n = index & 127;
            if (n <= 31) {
                e.Lda32(dst, ptr, nullptr, static_cast<uint8_t>(n * 4), 4, 0);
            } else {
                const Reg address = Reg::V(TmpAddr);
                e.Bitwise(BwOp::Or, address, IOp::Imm(static_cast<uint8_t>(n)), IOp::Imm(0));
                e.MadI32(address, address, 16, ptr);
                e.Lda32(dst, address, nullptr, 0, 4, 0);
            }
            e.WdfVertex(0);
            return {{Bank::Virtual, load_quad}, swz, mod};
        }
        // Indexed by an address register: (index + a) & 127, times 16, plus the buffer
        // pointer, into a per-instance register that is the load's base with offset 0.
        // That is psp2cgc's form in every per-instance program (`mad.i32 o0.x, r0.lo16,
        // 0x10.lo16, sa16.x; lda32.fetch4 r0, [o0, 0x0]`); the `[sa16, r3]` form it uses
        // in programs without branches breaks skinned vertices.
        const Reg areg = addr == 1 ? Reg::V(TmpA0X) : addr == 2 ? Reg::V(TmpA0Y) : Reg::V(TmpAlS16);
        const Reg address = Reg::V(TmpAddr);
        e.MadI16(address, areg, DataReg(DataOnes16),
                    IOp::Imm(static_cast<uint8_t>(index & 127)));
        e.Bitwise(BwOp::And, address, IOp::Imm(0x7f), IOp::R(address));
        e.MadI32(address, address, 16, ptr);
        e.Lda32(dst, address, nullptr, 0, 4, 0);
        e.WdfVertex(0);
        return {{Bank::Virtual, load_quad}, swz, mod};
    }
    default:
        Refuse("unknown source register type");
    }
}

std::optional<Quad> VertexEmitter::Dest(const DestRegister& reg) {
    const uint32_t index = static_cast<uint32_t>(reg.GetIndex());
    switch (reg.GetRegisterType()) {
    case RegisterType::Output: {
        const uint32_t attr = req.output_map[index & 15];
        if (attr < req.num_outputs && attr_quad[attr]) {
            return attr_quad[attr];
        }
        return std::nullopt; // an output nothing reads
    }
    case RegisterType::Temporary: {
        const uint32_t t = index & 15;
        if (!temp_quad[t]) {
            Refuse("temporary without a place");
        }
        return temp_quad[t];
    }
    default:
        Refuse("unknown destination register type");
    }
}

Src VertexEmitter::Materialize(Reg internal, const Operand& op) {
    e.Vec(VecOp::Mul, {internal, 0xf}, Direct(op), ConstOne);
    return {internal, XYZW};
}

void VertexEmitter::Store(const Quad& dst, uint8_t mask, Reg internal) {
    if (mask & 0x3) {
        e.MoveInternal({dst.Pair(0), static_cast<uint8_t>(mask & 0x3)}, {internal, XYZW});
    }
    if (mask & 0xc) {
        e.MoveInternal({dst.Pair(1), static_cast<uint8_t>(mask >> 2)}, {internal, ZWZW});
    }
}

int VertexEmitter::NewLabel() {
    const int label = e.NewLabel();
    label_bound.push_back(false);
    label_setemit.push_back(-1);
    return label;
}

int VertexEmitter::LabelAt(uint32_t offset) {
    Context& ctx = contexts.back();
    if (offset < ctx.begin || offset >= ctx.end) {
        Refuse("jump out of its subroutine: to instruction " + std::to_string(offset) +
               ", the subroutine's " + std::to_string(ctx.begin) + ".." +
               std::to_string(ctx.end));
    }
    auto it = ctx.labels.find(offset);
    if (it == ctx.labels.end()) {
        it = ctx.labels.emplace(offset, NewLabel()).first;
    }
    return it->second;
}

/// Enters main or a subroutine. A backward jump's target gets its label now: EmitRange binds
/// labels as it walks past their offsets, and the jump comes after the target. Only the jumps
/// the context's own walk reaches count (main's range runs over the subroutines after its END).
/// The walk starts at `entry`, which main's range may have code before (Emit).
void VertexEmitter::OpenContext(uint32_t begin, uint32_t end, uint32_t entry, bool precise) {
    contexts.push_back({begin, end, {}});
    Walk(begin, end, entry, precise, [this](uint32_t target) { LabelAt(target); });
}

std::vector<bool> VertexEmitter::Walk(uint32_t begin, uint32_t end, uint32_t entry, bool precise,
                                      const std::function<void(uint32_t)>& backward) const {
    std::vector<bool> seen(end - begin, false);
    std::vector<uint32_t> work{entry};
    while (!work.empty()) {
        const uint32_t offset = work.back();
        work.pop_back();
        if (offset < begin || offset >= end || seen[offset - begin]) {
            continue;
        }
        seen[offset - begin] = true;
        const Instruction instr = {req.code[offset]};
        const auto& flow = instr.flow_control;
        const auto op = instr.opcode.Value();
        if (op == OpCode::Id::END) {
            continue;
        }
        bool taken = op == OpCode::Id::JMPC;
        bool always = false;
        if (op == OpCode::Id::JMPU) {
            const auto known = KnownBool(flow.bool_uniform_id);
            taken = known && *known != ((flow.num_instructions & 1) != 0);
            always = taken;
        }
        if (taken || op == OpCode::Id::IFC || op == OpCode::Id::IFU) {
            if (taken && flow.dest_offset <= offset && flow.dest_offset >= begin && backward) {
                backward(flow.dest_offset);
            }
            work.push_back(flow.dest_offset);
        }
        if (op == OpCode::Id::IFC || op == OpCode::Id::IFU) {
            // The if-branch's end goes on at the join past the else-branch, which the walk
            // along the else-branch need not reach: an END or an always-taken JMPU ends it
            // (emitted, the code after the join would be left out).
            work.push_back(flow.dest_offset + flow.num_instructions);
        }
        // Calls return here; the subroutine opens its own context.
        if (!(precise && always)) {
            work.push_back(offset + 1);
        }
    }
    return seen;
}

void VertexEmitter::Bind(int label) {
    label_bound[static_cast<std::size_t>(label)] = true;
    e.Bind(label);
    emit_state.reset(); // a join: the paths into it may have set different emit states
    // A backward jump's target (no jump into it yet) is first reached falling through.
    const int8_t jumped = label_setemit[static_cast<std::size_t>(label)];
    if (jumped >= 0) {
        setemit_done = (!falls_through || setemit_done) && jumped != 0;
    } else if (!falls_through) {
        setemit_done = false;
    }
    falls_through = true;
}

void VertexEmitter::Branch(Pred pred, int target) {
    per_instance = true;
    e.Br(pred, target);
    if (!Bound(target)) {
        int8_t& jumped = label_setemit[static_cast<std::size_t>(target)];
        jumped = static_cast<int8_t>((jumped != 0) && setemit_done);
    }
    if (pred == Pred::None) {
        falls_through = false;
    }
}

void VertexEmitter::CondBranch(Instruction::FlowControlType flow, int target, bool when_true) {
    using Op = Instruction::FlowControlType::Op;
    const Pred a = flow.refx.Value() ? Pred::P0 : Pred::NotP0;
    const Pred not_a = flow.refx.Value() ? Pred::NotP0 : Pred::P0;
    const Pred b = flow.refy.Value() ? Pred::P1 : Pred::NotP1;
    const Pred not_b = flow.refy.Value() ? Pred::NotP1 : Pred::P1;
    switch (flow.op) {
    case Op::JustX:
        Branch(when_true ? a : not_a, target);
        break;
    case Op::JustY:
        Branch(when_true ? b : not_b, target);
        break;
    case Op::Or:
        if (when_true) {
            Branch(a, target);
            Branch(b, target);
        } else {
            const int skip = NewLabel();
            Branch(a, skip);
            Branch(not_b, target);
            Bind(skip);
        }
        break;
    case Op::And:
        if (when_true) {
            const int skip = NewLabel();
            Branch(not_a, skip);
            Branch(b, target);
            Bind(skip);
        } else {
            Branch(not_a, target);
            Branch(not_b, target);
        }
        break;
    default:
        Refuse("unknown condition");
    }
}

uint32_t VertexEmitter::EmitRange(uint32_t begin, uint32_t end) {
    uint32_t pc = begin;
    while (pc < end) {
        if (contexts.size() == 1 && !main_walk[pc]) {
            pc++;
            continue;
        }
        const Context& ctx = contexts.back();
        const auto it = ctx.labels.find(pc);
        if (it != ctx.labels.end() && !Bound(it->second)) {
            Bind(it->second);
        }
        pc = EmitInstruction(pc);
    }
    return pc;
}

void VertexEmitter::EmitArithmetic(const Instruction& instr, const SwizzlePattern& swz) {
    const OpCode::Info info = instr.opcode.Value().GetInfo();
    const bool inverted = (info.subtype & OpCode::Info::SrcInversed) != 0;
    const auto op = instr.opcode.Value().EffectiveOpCode();
    const uint8_t mask = DestMask(swz);
    const auto src1 = [&] {
        return Source(instr.common.GetSrc1(inverted),
                      !inverted * instr.common.address_register_index, SelectorSwizzle(swz, 1),
                      swz.negate_src1);
    };
    const auto src2 = [&] {
        return Source(instr.common.GetSrc2(inverted),
                      inverted * instr.common.address_register_index, SelectorSwizzle(swz, 2),
                      swz.negate_src2);
    };

    switch (op) {
    case OpCode::Id::ADD:
    case OpCode::Id::MUL:
    case OpCode::Id::MAX:
    case OpCode::Id::MIN: {
        const auto dst = Dest(instr.common.dest.Value());
        if (!dst || mask == 0) {
            return;
        }
        const Operand a = src1();
        const Operand b = src2();
        const VecOp vop = op == OpCode::Id::ADD   ? VecOp::Add
                          : op == OpCode::Id::MUL ? VecOp::Mul
                          : op == OpCode::Id::MAX ? VecOp::Max
                                                  : VecOp::Min;
        // The second source goes through i1: the op's second source takes neither a free
        // swizzle nor a negation, and a four-wide read of the unified store is only proven
        // for the first.
        const Src sb = Materialize(I1, b);
        e.Vec(vop, {I2, 0xf}, Direct(a), sb);
        Store(*dst, mask, I2);
        return;
    }
    case OpCode::Id::DP3:
    case OpCode::Id::DP4:
    case OpCode::Id::DPH:
    case OpCode::Id::DPHI: {
        const auto dst = Dest(instr.common.dest.Value());
        if (!dst || mask == 0) {
            return;
        }
        Operand a = src1();
        const Operand b = src2();
        const bool vec4 = op != OpCode::Id::DP3;
        if (op == OpCode::Id::DPH || op == OpCode::Id::DPHI) {
            a.swz[3] = Ch::One;
        }
        const Src sb = Materialize(I1, b);
        if (std::popcount(mask) == 1) {
            const int lane = std::countr_zero(mask);
            e.Dot({dst->Pair(lane / 2), static_cast<uint8_t>(1u << (lane & 1))}, Direct(a), sb,
                     vec4);
        } else {
            e.Dot({I2, 0x1}, Direct(a), sb, vec4);
            if (mask & 0x3) {
                e.MoveInternal({dst->Pair(0), static_cast<uint8_t>(mask & 0x3)}, {I2, XXXX});
            }
            if (mask & 0xc) {
                e.MoveInternal({dst->Pair(1), static_cast<uint8_t>(mask >> 2)}, {I2, XXXX});
            }
        }
        return;
    }
    case OpCode::Id::MOV: {
        const auto dst = Dest(instr.common.dest.Value());
        if (!dst || mask == 0) {
            return;
        }
        const Operand a = src1();
        // Lane pairs whose channels sit in one source pair move straight across.
        bool direct = true;
        for (int h = 0; h < 2; h++) {
            const uint8_t m = static_cast<uint8_t>((mask >> (2 * h)) & 3);
            if (m == 0) {
                continue;
            }
            const Ch c0 = a.swz[2 * h];
            const Ch c1 = a.swz[2 * h + 1];
            const bool need0 = (m & 1) != 0, need1 = (m & 2) != 0;
            if ((need0 && c0 > Ch::W) || (need1 && c1 > Ch::W)) {
                direct = false;
                break;
            }
            if (need0 && need1) {
                if (static_cast<int>(c0) / 2 != static_cast<int>(c1) / 2 ||
                    (c0 != c1 && (static_cast<int>(c0) & 1) != 0)) {
                    direct = false; // different pairs, or the yx order the table lacks
                    break;
                }
            }
        }
        // In place (the destination is the source's quad), a half's write is a pair the other
        // half may read: the half reading the pair the other writes goes first, and two halves
        // reading each other's pairs go through I2 instead.
        int source_pair[2] = {-1, -1};
        for (int h = 0; h < 2 && direct; h++) {
            const uint8_t m = static_cast<uint8_t>((mask >> (2 * h)) & 3);
            if (m != 0) {
                source_pair[h] = static_cast<int>((m & 1) ? a.swz[2 * h] : a.swz[2 * h + 1]) / 2;
            }
        }
        const bool in_place = dst->bank == a.quad.bank && dst->base == a.quad.base;
        if (in_place && source_pair[0] == 1 && source_pair[1] == 0) {
            direct = false; // a swap of the halves
        }
        const bool high_first = in_place && source_pair[1] == 0;
        if (direct) {
            for (int k = 0; k < 2; k++) {
                const int h = high_first ? 1 - k : k;
                const uint8_t m = static_cast<uint8_t>((mask >> (2 * h)) & 3);
                if (m == 0) {
                    continue;
                }
                const Ch c0 = a.swz[2 * h];
                const Ch c1 = a.swz[2 * h + 1];
                const Ch lead = (m & 1) ? c0 : c1;
                const Reg pair = a.quad.Pair(static_cast<int>(lead) / 2);
                Swizzle s = XXXX;
                if (m == 3 && c0 != c1) {
                    s = XYZW;
                } else if ((static_cast<int>(lead) & 1) != 0) {
                    s = YYYY;
                }
                if (a.mod == Mod::None) {
                    e.MoveInternal({dst->Pair(h), m}, {pair, s});
                } else {
                    e.Vec(VecOp::Mul, {dst->Pair(h), m}, {pair, s, a.mod}, ConstOne);
                }
            }
            return;
        }
        Materialize(I2, a);
        Store(*dst, mask, I2);
        return;
    }
    case OpCode::Id::FLR: {
        const auto dst = Dest(instr.common.dest.Value());
        if (!dst || mask == 0) {
            return;
        }
        const Src sa = Materialize(I0, src1());
        e.Vec(VecOp::Frc, {I1, 0xf}, sa, sa);                   // a - floor(a)
        e.Vec(VecOp::Add, {I2, 0xf}, {I1, XYZW, Mod::Neg}, sa); // a - frac
        Store(*dst, mask, I2);
        return;
    }
    case OpCode::Id::RCP:
    case OpCode::Id::RSQ:
    case OpCode::Id::EX2:
    case OpCode::Id::LG2: {
        const auto dst = Dest(instr.common.dest.Value());
        if (!dst || mask == 0) {
            return;
        }
        const Operand a = src1();
        const Ch ch = a.swz[0];
        if (ch > Ch::W) {
            Refuse("complex op on a constant channel");
        }
        const Reg pair = a.quad.Pair(static_cast<int>(ch) / 2);
        const uint8_t comp = static_cast<uint8_t>(static_cast<int>(ch) & 1);
        Pred pred = Pred::None;
        if (op == OpCode::Id::RCP) {
            // The GLSL emitter's guard: 1/0 leaves the register alone.
            e.Test(Cond::Ne, 2, {pair}, comp, ConstZero, true);
            pred = Pred::P2;
        } else if (op == OpCode::Id::RSQ) {
            Cond cond = Cond::Gt;
            if (a.mod == Mod::Neg) {
                cond = Cond::Lt;
            } else if (a.mod == Mod::Abs) {
                cond = Cond::Ne;
            } else if (a.mod == Mod::NegAbs) {
                return; // never positive
            }
            e.Test(cond, 2, {pair}, comp, ConstZero, true);
            pred = Pred::P2;
        }
        const CompOp cop = op == OpCode::Id::RCP   ? CompOp::Rcp
                           : op == OpCode::Id::RSQ ? CompOp::Rsq
                           : op == OpCode::Id::EX2 ? CompOp::Exp
                                                   : CompOp::Log;
        // In place, the source's pair is written last: the other half reads it first.
        const bool in_place = dst->bank == a.quad.bank && dst->base == a.quad.base;
        const int last = in_place ? static_cast<int>(ch) / 2 : 1;
        for (int k = 0; k < 2; k++) {
            const int h = last == 0 ? 1 - k : k;
            const uint8_t m = static_cast<uint8_t>((mask >> (2 * h)) & 3);
            if (m != 0) {
                e.Comp(cop, {dst->Pair(h), m}, {pair, XYZW, a.mod}, comp, pred);
            }
        }
        return;
    }
    case OpCode::Id::SGE:
    case OpCode::Id::SGEI:
    case OpCode::Id::SLT:
    case OpCode::Id::SLTI: {
        const auto dst = Dest(instr.common.dest.Value());
        if (!dst || mask == 0) {
            return;
        }
        // Both operands first: one may be a uniform read (a load and a wait), and no
        // internal register may be live across that wait.
        const Operand a = src1();
        const Operand b = src2();
        Materialize(I0, a);
        Materialize(I1, b);
        const Cond cond = (op == OpCode::Id::SGE || op == OpCode::Id::SGEI) ? Cond::Ge : Cond::Lt;
        for (int lane = 0; lane < 4; lane++) {
            if ((mask & (1u << lane)) == 0) {
                continue;
            }
            const Dst d{dst->Pair(lane / 2), static_cast<uint8_t>(1u << (lane & 1))};
            e.Test(cond, 2, {I0}, static_cast<uint8_t>(lane), {I1}, false);
            e.MoveInternal(d, ConstZero);
            e.MoveInternal(d, ConstOne, Pred::P2);
        }
        return;
    }
    case OpCode::Id::CMP: {
        using CompareOp = Instruction::Common::CompareOpType::Op;
        const auto cond = [](CompareOp c) -> Cond {
            switch (c) {
            case CompareOp::Equal:
                return Cond::Eq;
            case CompareOp::NotEqual:
                return Cond::Ne;
            case CompareOp::LessThan:
                return Cond::Lt;
            case CompareOp::LessEqual:
                return Cond::Le;
            case CompareOp::GreaterThan:
                return Cond::Gt;
            case CompareOp::GreaterEqual:
                return Cond::Ge;
            default:
                Refuse("unknown compare");
            }
        };
        const Cond cx = cond(instr.common.compare_op.x.Value());
        const Cond cy = cond(instr.common.compare_op.y.Value());
        const Operand a = src1();
        const Operand b = src2();
        Materialize(I0, a);
        Materialize(I1, b);
        e.Test(cx, 0, {I0}, 0, {I1}, false);
        e.Test(cy, 1, {I0}, 1, {I1}, false);
        return;
    }
    case OpCode::Id::MOVA: {
        // a0.x/y = (int) src.x/y when within [-128, 127], else 0 (the GLSL helper's rule), as int16
        // words the address arithmetic reads. The conversion truncates toward zero, and SGX543's
        // pack rounds to nearest only, so the lanes are truncated first, into the load quad: with f
        // = floor(|a|), min(f, max(-f, a)) is f where a >= 0 and -f where a < 0 (-f >= a there),
        // without a predicate.
        // In psp2cgc's shape: the pack reads the unified register directly and is never predicated
        // (both gave wrong indices), and the out-of-range case zeroes the result through a
        // predicated `or.u32`.
        Materialize(I0, src1());
        e.Vec(VecOp::Frc, {I1, 0x3}, {I0, XYZW, Mod::Abs}, {I0, XYZW, Mod::Abs});
        e.Vec(VecOp::Add, {I1, 0x3}, {I1, XYZW, Mod::Neg}, {I0, XYZW, Mod::Abs}); // f
        e.Vec(VecOp::Max, {I2, 0x3}, {I1, XYZW, Mod::Neg}, {I0, XYZW});
        const Reg pair = Reg::V(TmpLoad);
        e.Vec(VecOp::Min, {pair, 0x3}, {I1, XYZW}, {I2, XYZW});
        for (int lane = 0; lane < 2; lane++) {
            if (!swz.DestComponentEnabled(lane)) {
                continue;
            }
            const Reg areg = Reg::V(lane == 0 ? TmpA0X : TmpA0Y);
            const uint8_t comp = static_cast<uint8_t>(lane);
            e.PackS16(areg, pair, comp);
            e.Test(Cond::Lt, 2, {pair}, comp, Data(DataNeg128), true);
            e.Bitwise(BwOp::Or, areg, IOp::Imm(0), IOp::Imm(0), Pred::P2);
            e.Test(Cond::Gt, 2, {pair}, comp, Data(Data127), true);
            e.Bitwise(BwOp::Or, areg, IOp::Imm(0), IOp::Imm(0), Pred::P2);
        }
        return;
    }
    default:
        Refuse(std::string("unhandled arithmetic instruction ") + info.name);
    }
}

void VertexEmitter::EmitMad(const Instruction& instr, const SwizzlePattern& swz) {
    const bool inverted = instr.opcode.Value().EffectiveOpCode() == OpCode::Id::MADI;
    const uint8_t mask = DestMask(swz);
    const auto dst = Dest(instr.mad.dest.Value());
    if (!dst || mask == 0) {
        return;
    }
    const Operand a =
        Source(instr.mad.GetSrc1(inverted), 0, SelectorSwizzle(swz, 1), swz.negate_src1);
    const Operand b =
        Source(instr.mad.GetSrc2(inverted), !inverted * instr.mad.address_register_index,
               SelectorSwizzle(swz, 2), swz.negate_src2);
    const Operand c =
        Source(instr.mad.GetSrc3(inverted), inverted * instr.mad.address_register_index,
               SelectorSwizzle(swz, 3), swz.negate_src3);
    const Src sb = Materialize(I0, b);
    const Src sc = Materialize(I1, c);
    // VMAD's first source only has the table swizzles.
    const Src sa = InVec4Table(a.swz) ? Direct(a) : Materialize(I2, a);
    e.Mad({I2, 0xf}, sa, sb, sc, true);
    Store(*dst, mask, I2);
}

uint32_t VertexEmitter::EmitInstruction(uint32_t offset) {
    const Instruction instr = {req.code[offset]};
    const OpCode::Info info = instr.opcode.Value().GetInfo();
    if (info.type == OpCode::Type::Arithmetic) {
        const SwizzlePattern swz{req.swizzle[instr.common.operand_desc_id]};
        EmitArithmetic(instr, swz);
        return offset + 1;
    }
    if (info.type == OpCode::Type::MultiplyAdd) {
        const auto op = instr.opcode.Value().EffectiveOpCode();
        if (op != OpCode::Id::MAD && op != OpCode::Id::MADI) {
            Refuse("unhandled multiply-add instruction");
        }
        const SwizzlePattern swz{req.swizzle[instr.mad.operand_desc_id]};
        EmitMad(instr, swz);
        return offset + 1;
    }

    const auto& flow = instr.flow_control;
    switch (instr.opcode.Value()) {
    case OpCode::Id::NOP:
        return offset + 1;

    case OpCode::Id::END: {
        const Context& ctx = contexts.back();
        Branch(Pred::None, end_label);
        // Main stops here unless a jump still targets something further on; a subroutine
        // range keeps going (its remaining instructions may be reached by a jump too).
        if (contexts.size() == 1) {
            for (const auto& [at, label] : ctx.labels) {
                if (at > offset && !Bound(label)) {
                    return offset + 1;
                }
            }
            return ctx.end;
        }
        return offset + 1;
    }

    case OpCode::Id::JMPC: {
        const int target = LabelAt(flow.dest_offset);
        CondBranch(flow, target, true);
        return offset + 1;
    }
    case OpCode::Id::JMPU: {
        const auto known = KnownBool(flow.bool_uniform_id);
        if (!known) {
            Refuse("bool uniform not baked");
        }
        const bool invert = (flow.num_instructions & 1) != 0;
        if (*known != invert) {
            Branch(Pred::None, LabelAt(flow.dest_offset));
        }
        return offset + 1;
    }

    case OpCode::Id::CALL:
    case OpCode::Id::CALLC:
    case OpCode::Id::CALLU: {
        int skip = -1;
        if (instr.opcode.Value() == OpCode::Id::CALLC) {
            skip = NewLabel();
            CondBranch(flow, skip, false);
        } else if (instr.opcode.Value() == OpCode::Id::CALLU) {
            const auto known = KnownBool(flow.bool_uniform_id);
            if (!known) {
                Refuse("bool uniform not baked");
            }
            if (!*known) {
                return offset + 1;
            }
        }
        if (contexts.size() >= 8) {
            Refuse("call depth");
        }
        const uint32_t begin = flow.dest_offset;
        const uint32_t end = begin + flow.num_instructions;
        OpenContext(begin, end, begin, false);
        EmitRange(begin, end);
        for (const auto& [at, label] : contexts.back().labels) {
            if (!Bound(label)) {
                Refuse("jump into code the subroutine never reaches");
            }
        }
        contexts.pop_back();
        if (skip >= 0) {
            Bind(skip);
        }
        return offset + 1;
    }

    case OpCode::Id::IFU:
    case OpCode::Id::IFC: {
        const uint32_t if_begin = offset + 1;
        const uint32_t else_begin = flow.dest_offset;
        const uint32_t endif = else_begin + flow.num_instructions;
        if (instr.opcode.Value() == OpCode::Id::IFU) {
            const auto known = KnownBool(flow.bool_uniform_id);
            if (!known) {
                Refuse("bool uniform not baked");
            }
            if (*known) {
                EmitRange(if_begin, else_begin);
            } else if (flow.num_instructions != 0) {
                EmitRange(else_begin, endif);
            }
            return endif;
        }
        const int else_label = NewLabel();
        CondBranch(flow, else_label, false);
        EmitRange(if_begin, else_begin);
        if (flow.num_instructions != 0) {
            const int end = NewLabel();
            Branch(Pred::None, end);
            Bind(else_label);
            EmitRange(else_begin, endif);
            Bind(end);
        } else {
            Bind(else_label);
        }
        return endif;
    }

    case OpCode::Id::LOOP: {
        if (loop_depth >= 2 || int_sa < 0) {
            Refuse("loop nesting");
        }
        const uint8_t iq = static_cast<uint8_t>(int_sa + 4 * (flow.int_uniform_id & 3));
        const Reg count = Reg::V(loop_depth == 0 ? TmpCount0 : TmpCount1);
        // aL = i.y; count = 0; while (count <= i.x) { body; aL += i.z; count++ }
        e.MoveInternal({Reg::V(TmpAlF), 0x1}, {Reg::Sa(iq), YYYY});
        e.PackS16(Reg::V(TmpAlS16), Reg::V(TmpAlF), 0);
        e.MoveInternal({count, 0x1}, ConstZero);
        const int top = NewLabel();
        const int exit = NewLabel();
        Bind(top);
        e.Test(Cond::Gt, 2, {count}, 0, {Reg::Sa(iq)}, true);
        Branch(Pred::P2, exit);
        loop_depth++;
        loop_breaks.push_back(-1);
        EmitRange(offset + 1, flow.dest_offset + 1);
        const int broken = loop_breaks.back();
        loop_breaks.pop_back();
        loop_depth--;
        const auto step = [&] {
            e.Vec(VecOp::Add, {Reg::V(TmpAlF), 0x1}, {Reg::V(TmpAlF), XYZW},
                  {Reg::Sa(static_cast<uint8_t>(iq + 2)), XXXX});
            e.PackS16(Reg::V(TmpAlS16), Reg::V(TmpAlF), 0);
        };
        step();
        e.Vec(VecOp::Add, {count, 0x1}, {count, XYZW}, ConstOne);
        Branch(Pred::None, top);
        if (broken >= 0) {
            // A round left by a BREAKC still steps aL (upstream's interpreter).
            Bind(broken);
            step();
        }
        Bind(exit);
        return flow.dest_offset + 1;
    }

    case OpCode::Id::BREAKC: {
        // Out of the innermost LOOP, from an IF or a subroutine inside it too (both inlined).
        if (loop_breaks.empty()) {
            Refuse("BREAKC outside a LOOP");
        }
        if (loop_breaks.back() < 0) {
            loop_breaks.back() = NewLabel();
        }
        CondBranch(flow, loop_breaks.back(), true);
        return offset + 1;
    }

    case OpCode::Id::SETEMIT:
        if (req.mode != VsMode::Geometry) {
            Refuse("SETEMIT outside a geometry program");
        }
        emit_state = EmitState{static_cast<uint8_t>(instr.setemit.vertex_id.Value()),
                               instr.setemit.prim_emit.Value() != 0,
                               instr.setemit.winding.Value() != 0};
        setemit_done = true;
        if (dynamic_emit) {
            if (emit_state->vertex_id > 2) {
                Refuse("SETEMIT of vertex 3");
            }
            e.MoveInternal({gs_emit.Pair(0), 0x1},
                           emit_state->vertex_id != 0 ? ConstOne : ConstZero);
            if (emit_state->vertex_id == 2) {
                e.Vec(VecOp::Add, {gs_emit.Base(), 0x1}, {gs_emit.Base(), XYZW}, ConstOne);
            }
            e.MoveInternal({gs_emit.Pair(0), 0x2}, emit_state->prim ? ConstOne : ConstZero);
            e.MoveInternal({gs_emit.Pair(1), 0x1}, emit_state->winding ? ConstOne : ConstZero);
        }
        return offset + 1;
    case OpCode::Id::EMIT:
        if (req.mode != VsMode::Geometry) {
            Refuse("EMIT outside a geometry program");
        }
        GsEmit();
        return offset + 1;

    default:
        Refuse(std::string("unhandled instruction ") + info.name);
    }
}

void VertexEmitter::StoreQuad(Reg base, uint32_t word, const Quad& q) {
    for (uint32_t c = 0; c < 4; c++) {
        e.Sta32(base, static_cast<uint8_t>(word + c), Reg{q.bank, static_cast<uint8_t>(q.base + c)},
                0);
    }
}

/// EMIT (upstream's GeometryEmitter::Emit): the output attributes into the emit buffer's
/// vertex; with the primitive flag, the buffer's three vertices are a triangle, in the order
/// 1, 0, 2 with the winding flag (the primitive assembler's inversion), stored at the next
/// triangle's place unless gs_max_triangles are stored already.
void VertexEmitter::GsEmit() {
    if (!emit_state && dynamic_emit) {
        GsEmitDynamic();
        return;
    }
    if (!emit_state) {
        Refuse(UnknownEmitState);
    }
    const EmitState st = *emit_state;
    if (st.vertex_id > 2) {
        Refuse("SETEMIT of vertex 3");
    }
    GsCopyToSlot(st.vertex_id);
    if (!st.prim) {
        return;
    }
    const int full = NewLabel();
    e.Test(Cond::Ge, 2, {gs_state.Base()}, 0, Data(DataGsCap), true);
    Branch(Pred::P2, full);
    GsStoreTriangle(st.winding);
    Bind(full);
    emit_state = st; // the join is this EMIT's own
}

/// The address of the state block's emit buffer vertex v (GsStateQuads, 256 bytes in) from
/// `block` (PA 2 for the loads, PA 3 for the stores): the offsets' 7 bits reach 32 float4s.
Reg VertexEmitter::GsStateSlot(uint32_t v, Reg block) {
    const Reg address = Reg::V(TmpAddr);
    e.Bitwise(BwOp::Or, address, IOp::Imm(static_cast<uint8_t>(16 * (v + 1))), IOp::Imm(0));
    e.MadI32(address, address, 16, block);
    return address;
}

void VertexEmitter::GsCopyToSlot(uint8_t vertex_id) {
    for (uint32_t k = 0; k < req.num_outputs; k++) {
        const Quad& slot = gs_slots[vertex_id][k];
        e.MoveInternal({slot.Pair(0), 0x3}, {attr_quad[k]->Pair(0), XYZW});
        e.MoveInternal({slot.Pair(1), 0x3}, {attr_quad[k]->Pair(1), XYZW});
    }
}

void VertexEmitter::GsStoreTriangle(bool winding) {
    const Reg size = Reg::V(static_cast<uint8_t>(gs_state.base + 1));
    const Reg next = Reg::V(static_cast<uint8_t>(gs_state.base + 2));
    static constexpr uint8_t straight[3] = {0, 1, 2};
    static constexpr uint8_t inverted[3] = {1, 0, 2};
    const uint8_t* order = winding ? inverted : straight;
    for (uint32_t v = 0; v < 3; v++) {
        for (uint32_t k = 0; k < req.num_outputs; k++) {
            StoreQuad(next, (v * req.num_outputs + k) * 4, gs_slots[order[v]][k]);
        }
    }
    e.MadI32(next, size, 16, next);
    e.Vec(VecOp::Add, {gs_state.Base(), 0x1}, {gs_state.Base(), XYZW}, ConstOne);
}

/// EMIT with the emit state read from gs_emit (SETEMITs in the branches of an IF, the EMIT after
/// it): the slot by the vertex id, then with the primitive flag the triangle in the winding's
/// order.
void VertexEmitter::GsEmitDynamic() {
    if (!setemit_done) {
        Refuse("an EMIT that can run before its invocation's first SETEMIT (upstream: the "
               "invocation before's emit state)");
    }
    for (uint8_t id = 0; id < 3; id++) {
        const int skip = NewLabel();
        // P2: another vertex id (x is 0, 1 or 2).
        e.Test(id == 2 ? Cond::Le : Cond::Ne, 2, {gs_emit.Pair(0)}, 0,
               id == 0 ? ConstZero : ConstOne, true);
        Branch(Pred::P2, skip);
        GsCopyToSlot(id);
        Bind(skip);
    }
    const int done = NewLabel();
    e.Test(Cond::Eq, 2, {gs_emit.Pair(0)}, 1, ConstZero, true);
    Branch(Pred::P2, done);
    e.Test(Cond::Ge, 2, {gs_state.Base()}, 0, Data(DataGsCap), true);
    Branch(Pred::P2, done);
    const int inverted = NewLabel();
    const int stored = NewLabel();
    e.Test(Cond::Ne, 2, {gs_emit.Pair(1)}, 0, ConstZero, true);
    Branch(Pred::P2, inverted);
    GsStoreTriangle(false);
    Branch(Pred::None, stored);
    Bind(inverted);
    GsStoreTriangle(true);
    Bind(stored);
    Bind(done);
}

/// The stream-out modes' end: the stores (StoreOutputs: the output attributes; Geometry: the
/// end of the triangles into the region's header), their completion, and a position every
/// vertex shares, so the draw covers nothing.
void VertexEmitter::EpilogueStreamOut() {
    if (req.mode == VsMode::StoreOutputs) {
        const Reg base = Reg::Pa(static_cast<uint8_t>(out_address_pa));
        for (uint32_t k = 0; k < req.num_outputs; k++) {
            StoreQuad(base, k * 4, *attr_quad[k]);
        }
    } else if (req.gs_serial) {
        // The invocation's end, then the next until none is left; the temporaries to the
        // state block for the draw after.
        const Reg entry = Reg::V(static_cast<uint8_t>(gs_loop.base + 1));
        e.Sta32(GsWord(1), 0, Reg::V(static_cast<uint8_t>(gs_state.base + 2)), 0);
        // The entry's words times 4: an immediate of 64 or more reads negative, so 16- and 17-word
        // entries would step back into other data.
        e.MadI32(entry, Reg::V(static_cast<uint8_t>(gs_loop.base + 2)), 4, entry);
        e.Vec(VecOp::Add, {gs_loop.Pair(0), 0x1}, {ConstOne.reg, ConstOne.swz, Mod::Neg},
              {gs_loop.Pair(0), XYZW});
        e.Test(Cond::Gt, 2, {gs_loop.Pair(0)}, 0, ConstZero, true);
        Branch(Pred::P2, gs_loop_top);
        for (uint32_t t = 0; t < 16; t++) {
            if (temp_quad[t]) {
                StoreQuad(Reg::Pa(3), t * 4, *temp_quad[t]);
            }
        }
        for (uint32_t v = 0; v < 3; v++) {
            const Reg slot = GsStateSlot(v, Reg::Pa(3));
            for (uint32_t k = 0; k < req.num_outputs; k++) {
                StoreQuad(slot, k * 4, gs_slots[v][k]);
            }
        }
    } else {
        e.Sta32(GsWord(1), 0, Reg::V(static_cast<uint8_t>(gs_state.base + 2)), 0);
    }
    e.Idf(0);
    e.WdfVertex(0);
    e.MoveInternal({Reg::O(OutPosition), 0x3}, {Reg::C(0), XYZW});
    e.MoveInternal({Reg::O(OutPosition + 2), 0x3}, ConstZeroOne);
}

void VertexEmitter::Prologue() {
    // The components a loader does not supply: (0, 0, 0, 1), as the PICA fills them.
    for (uint32_t i = 0; i < 16; i++) {
        if (input_pa[i] < 0) {
            continue;
        }
        uint32_t comps = req.reg_components[i];
        if (comps == 0 || comps > 4) {
            comps = 4;
        }
        const Quad q{Bank::PrimAttr, static_cast<uint8_t>(input_pa[i])};
        if (input_raw_pa[i] >= 0) {
            const bool s16 = (req.s16_regs & (1u << i)) != 0;
            const uint8_t format = s16 ? 4 : (req.s8_regs & (1u << i)) ? 1 : 0;
            // A SHORT attribute's half h is in the pair h / 4, lane h % 4.
            for (uint32_t c = 0; c < comps; c++) {
                const Reg raw = Reg::Pa(static_cast<uint8_t>(input_raw_pa[i] + 2 * (c >> 2)));
                e.UnpackInt(Reg::Pa(static_cast<uint8_t>(input_pa[i] + c)), raw,
                            static_cast<uint8_t>(c & 3), format);
            }
        }
        switch (comps) {
        case 1:
            e.MoveInternal({q.Pair(0), 0x2}, ConstZero);
            [[fallthrough]];
        case 2:
            e.MoveInternal({q.Pair(1), 0x3}, ConstZeroOne);
            break;
        case 3:
            e.MoveInternal({q.Pair(1), 0x2}, ConstOne);
            break;
        default:
            break;
        }
    }
    // Output attributes the program never assigns: (0, 0, 0, 1), as the wrappers write it.
    for (uint32_t k = 0; k < req.num_outputs && k < 16; k++) {
        if ((scan.attrs_written & (1u << k)) == 0 && attr_quad[k]) {
            e.MoveInternal({attr_quad[k]->Pair(0), 0x3}, {Reg::C(0), XYZW});
            e.MoveInternal({attr_quad[k]->Pair(1), 0x3}, ConstZeroOne);
        }
    }
    // Temporaries read before they are written start at (0, 0, 0, 1) too; in order
    // (gs_serial), they are the state block's, below.
    for (uint32_t t = 0; t < 16; t++) {
        if (!temp_quad[t] || scan.temp_init[t] == 0 || req.gs_serial) {
            continue;
        }
        const uint8_t m = scan.temp_init[t];
        if (m & 0x3) {
            e.MoveInternal({temp_quad[t]->Pair(0), static_cast<uint8_t>(m & 0x3)},
                           {Reg::C(0), XYZW});
        }
        if (m & 0xc) {
            e.MoveInternal({temp_quad[t]->Pair(1), static_cast<uint8_t>(m >> 2)}, ConstZeroOne);
        }
    }
    if (scan.cc) {
        // The condition codes start false.
        e.Test(Cond::Ne, 0, ConstZero, 0, ConstZero, true);
        e.Test(Cond::Ne, 1, ConstZero, 0, ConstZero, true);
    }
    if (scan.a0x) {
        e.Bitwise(BwOp::Or, Reg::V(TmpA0X), IOp::Imm(0), IOp::Imm(0));
    }
    if (scan.a0y) {
        e.Bitwise(BwOp::Or, Reg::V(TmpA0Y), IOp::Imm(0), IOp::Imm(0));
    }
    if (scan.al) {
        e.Bitwise(BwOp::Or, Reg::V(TmpAlS16), IOp::Imm(0), IOp::Imm(0));
    }
    if (req.mode == VsMode::Geometry && req.gs_serial) {
        const Reg entry = Reg::V(static_cast<uint8_t>(gs_loop.base + 1));
        gs_loop_top = NewLabel();
        e.MoveInternal({gs_loop.Pair(0), 0x1}, {Reg::Pa(0), XXXX});
        e.Bitwise(BwOp::Or, entry, IOp::R(Reg::Pa(1)), IOp::Imm(0));
        // z: the entry's words, mad.i32's multiplier for the entry's step (its first source is
        // no SA, and its immediate is signed, EpilogueStreamOut).
        e.Bitwise(BwOp::Or, Reg::V(static_cast<uint8_t>(gs_loop.base + 2)),
                  IOp::Imm(static_cast<uint8_t>(gs_entry_words)), IOp::Imm(0));
        // Every temporary the program touches: one it writes first in code order may still be read
        // first on an invocation's path.
        for (uint32_t t = 0; t < 16; t++) {
            if (temp_quad[t]) {
                e.Lda32(temp_quad[t]->Base(), Reg::Pa(2), nullptr, static_cast<uint8_t>(t * 4), 4,
                        0);
                e.WdfVertex(0);
            }
        }
        // The emit buffer's vertices the draw before left (GsStateQuads).
        for (uint32_t v = 0; v < 3; v++) {
            const Reg slot = GsStateSlot(v, Reg::Pa(2));
            for (uint32_t k = 0; k < req.num_outputs; k++) {
                e.Lda32(gs_slots[v][k].Base(), slot, nullptr, static_cast<uint8_t>(k * 4), 4, 0);
                e.WdfVertex(0);
            }
        }
        Bind(gs_loop_top);
        for (uint32_t q = 0; q < gs_entry_quads.size(); q++) {
            e.Lda32(gs_entry_quads[q].Base(), entry, nullptr, static_cast<uint8_t>(q * 4), 4, 0);
            e.WdfVertex(0);
        }
    }
    if (req.mode == VsMode::Geometry) {
        // word).
        for (uint32_t r = 0; r < gs_row_quads.size(); r++) {
            e.Lda32(gs_row_quads[r].Base(), GsWord(2 + r), nullptr, 0, 4, 0);
            e.WdfVertex(0);
        }
        for (uint32_t r = 0, k = 2 + req.gs_rows; r < 16; r++) {
            if (gs_input_quads[r]) {
                e.Lda32(gs_input_quads[r]->Base(), GsWord(k++), nullptr, 0, 4, 0);
                e.WdfVertex(0);
            }
        }
        if (gs_zero_inputs) {
            e.MoveInternal({gs_zero_inputs->Pair(0), 0x3}, {Reg::C(0), XYZW});
            e.MoveInternal({gs_zero_inputs->Pair(1), 0x3}, {Reg::C(0), XYZW});
        }
        // No triangle stored; the first goes at PA 0.
        const Reg size = Reg::V(static_cast<uint8_t>(gs_state.base + 1));
        const Reg next = Reg::V(static_cast<uint8_t>(gs_state.base + 2));
        e.MoveInternal({gs_state.Pair(0), 0x1}, ConstZero);
        e.Bitwise(BwOp::Or, size, IOp::Imm(static_cast<uint8_t>(3 * req.num_outputs)),
                  IOp::Imm(0));
        e.Bitwise(BwOp::Or, next, IOp::R(GsWord(0)), IOp::Imm(0));
        if (dynamic_emit) {
            // Every EMIT that reads it comes after a SETEMIT (setemit_done): written for the
            // register allocator, never read as zero.
            e.MoveInternal({gs_emit.Pair(0), 0x3}, {Reg::C(0), XYZW});
            e.MoveInternal({gs_emit.Pair(1), 0x3}, {Reg::C(0), XYZW});
        }
    }
}

std::optional<Src> VertexEmitter::SemanticSrc(Semantic s) const {
    const VsSemantic& m = req.semantics[s];
    if (m.attribute >= req.num_outputs || m.attribute >= 16 || !attr_quad[m.attribute]) {
        return std::nullopt;
    }
    const Quad& q = *attr_quad[m.attribute];
    return Src{q.Pair(m.component / 2), (m.component & 1) ? YYYY : XXXX};
}

void VertexEmitter::GatherFour(Reg internal, Semantic first) {
    // One four-wide read when the four come from one attribute in order.
    const VsSemantic& m0 = req.semantics[first];
    bool straight = m0.attribute < req.num_outputs && m0.component == 0;
    for (int i = 1; i < 4 && straight; i++) {
        const VsSemantic& mi = req.semantics[first + i];
        straight = mi.attribute == m0.attribute && mi.component == static_cast<uint8_t>(i);
    }
    if (straight && attr_quad[m0.attribute]) {
        e.Vec(VecOp::Mul, {internal, 0xf}, {attr_quad[m0.attribute]->Base(), XYZW}, ConstOne);
        return;
    }
    for (int i = 0; i < 4; i++) {
        const auto src = SemanticSrc(static_cast<Semantic>(first + i));
        e.MoveInternal({internal, static_cast<uint8_t>(1u << i)}, src ? *src : ConstOne);
    }
}

void VertexEmitter::OutputLane(uint8_t out, int lane, Semantic s) {
    const auto src = SemanticSrc(s);
    e.MoveInternal({Reg::O(out), static_cast<uint8_t>(1u << lane)}, src ? *src : ConstOne);
}

void VertexEmitter::OutputPair(uint8_t out, Semantic s0, Semantic s1) {
    const VsSemantic& m0 = req.semantics[s0];
    const VsSemantic& m1 = req.semantics[s1];
    if (m0.attribute < req.num_outputs && m1.attribute == m0.attribute && attr_quad[m0.attribute] &&
        (m0.component & 1) == 0 && m1.component == m0.component + 1) {
        e.MoveInternal({Reg::O(out), 0x3}, {attr_quad[m0.attribute]->Pair(m0.component / 2), XYZW});
        return;
    }
    // Two lanes no attribute feeds (the emitter's 1.0 fill) go in one pair move.
    if (!SemanticSrc(s0) && !SemanticSrc(s1)) {
        e.MoveInternal({Reg::O(out), 0x3}, ConstOne);
        return;
    }
    OutputLane(out, 0, s0);
    OutputLane(out, 1, s1);
}

void VertexEmitter::OutputTexcoord(uint8_t out, Semantic su, Semantic sv) {
    // (s, 1 - t): the host copies keep the PICA's rows in memory order, t = 1 at row 0
    // (SgxdBackend::Untile), so a fragment program reads there; written flipped, a plain read
    // takes the coordinate as it arrives and the pixel PDS program can issue it
    // (FragmentEmitter::SampleTextures).
    OutputLane(out, 0, su);
    if (const auto v = SemanticSrc(sv)) {
        e.Vec(VecOp::Add, {Reg::O(out), 0x2}, Src{v->reg, v->swz, Mod::Neg}, ConstOne);
    } else {
        e.MoveInternal({Reg::O(out), 0x2}, ConstZero); // 1 - 1.0, the fill
    }
}

void VertexEmitter::Epilogue() {
    // Position: after the wrappers' nudge of a z barely past a clip boundary, clip z for the
    // TA's clipping, which keeps -w <= z <= w (MTE_CTRL's OPENGL): the
    // PICA's own planes, -w <= z <= 0, are that range for +-(2z + w), and the viewport maps it
    // back to the PICA depth (RasterizerSgxd::Draw). No division by w: a vertex in the eye's
    // plane has w = 0.
    GatherFour(I0, SemPositionX);
    e.Comp(CompOp::Rcp, {I1, 0x1}, {I0}, 3);              // 1/w
    e.Vec(VecOp::Mul, {I1, 0x2}, {I0, ZZZZ}, {I1, XXXX}); // ndc z
    e.Test(Cond::Gt, 2, {I1}, 1, ConstZero, true);
    e.Test(Cond::Lt, 2, {I1}, 1, Data(DataEpsilon), true, Pred::P2);
    e.MoveInternal({I0, 0x4}, ConstZero, Pred::P2); // z = 0
    e.Test(Cond::Lt, 2, {I1}, 1, Data(DataNeg1), true);
    e.Test(Cond::Gt, 2, {I1}, 1, Data(DataNeg1Eps), true, Pred::P2);
    e.Vec(VecOp::Mul, {I0, 0x4}, {I0, WWWW, Mod::Neg}, ConstOne, Pred::P2); // z = -w
    e.Vec(VecOp::Mul, {I1, 0x2}, {I0, ZZZZ}, {Reg::Sa(SaDepthScale), XXXX}); // z * depth_scale
    e.Vec(VecOp::Mul, {I1, 0x1}, {I0, WWWW}, {Reg::Sa(SaDepthScale), YYYY}); // w * depth_offset
    e.Vec(VecOp::Add, {I1, 0x2}, {I1, YYYY}, {I1, XXXX});         // depth * w
    e.Vec(VecOp::Add, {I1, 0x1}, {I0, ZZZZ}, {I0, ZZZZ}); // 2z
    e.Vec(VecOp::Add, {I1, 0x1}, {I1, XXXX}, {I0, WWWW}); // 2z + w
    // z = sign * (2z + w): the TA's near plane on the side the draw's vertices behind the eye lie
    // past (RasterizerSgxd::ClipZSign).
    e.Vec(VecOp::Mul, {I0, 0x4}, {I1, XXXX}, {Reg::Sa(SaClipZSign), XXXX});
    // The draws render the guest's colour buffer transposed (SgxdBackend::PicaTarget, route 1):
    // clip x takes y and clip y takes x, and the viewport maps them (RasterizerSgxd::Draw). Each
    // is scaled down by the factor the viewport is scaled up by, the viewport flip's sign in
    // the first (RasterizerSgxd::GuardScale): the pixels are the same, and the TA's guard band,
    // a multiple of the viewport, is wider. The standard swizzles have no YX pair: a lane each.
    e.Vec(VecOp::Mul, {Reg::O(OutPosition), 0x1}, {I0, YYYY}, {Reg::Sa(SaPositionScale), XXXX});
    e.Vec(VecOp::Mul, {Reg::O(OutPosition), 0x2}, {I0, XXXX}, {Reg::Sa(SaPositionScale), YYYY});
    e.MoveInternal({Reg::O(OutPosition + 2), 0x3}, {I0, ZWZW});
    // The fog table's coordinate (Usse::TextureIssue::fog): TEXCOORD3's u and t are the PICA
    // depth times w and w, which the PDS divides (TEXPROJ T) after iterating them
    // perspective-correct: the depth, WPOS's z. The clipper's new vertices take them exactly,
    // both being linear in clip space.
    e.MoveInternal({Reg::O(OutTexcoord3), 0x1}, {I1, YYYY});
    e.MoveInternal({Reg::O(OutTexcoord3St), 0x1}, ConstZero);
    e.MoveInternal({Reg::O(OutTexcoord3St), 0x2}, {I0, WWWW});

    GatherFour(I1, SemColorR);
    e.Vec(VecOp::Min, {Reg::O(OutColor), 0x3}, {I1, XYZW, Mod::Abs}, ConstOne);
    e.Vec(VecOp::Min, {Reg::O(OutColor + 2), 0x3}, {I1, ZWZW, Mod::Abs}, ConstOne);

    GatherFour(I2, SemQuaternionX);
    e.MoveInternal({Reg::O(OutNormquat), 0x3}, {I2, XYZW});
    e.MoveInternal({Reg::O(OutNormquat + 2), 0x3}, {I2, ZWZW});

    OutputTexcoord(OutTexcoord0, SemTexcoord0U, SemTexcoord0V);
    OutputTexcoord(OutTexcoord1, SemTexcoord1U, SemTexcoord1V);
    OutputTexcoord(OutTexcoord2, SemTexcoord2U, SemTexcoord2V);
    OutputLane(OutTexcoord3, 1, SemTexcoord0W);
    OutputPair(OutView, SemViewX, static_cast<Semantic>(SemViewX + 1));
    OutputLane(OutView + 2, 0, static_cast<Semantic>(SemViewX + 2));
}

std::optional<Program> VertexEmitter::Emit(uint32_t& input_regs) {
    if (req.code.size() < req.code_size || req.main_offset >= req.code_size) {
        Refuse("program extent");
    }
    ScanProgram();
    if (req.mode == VsMode::Draw) {
        FindPositionRows();
    }
    input_regs = scan.inputs;
    for (uint32_t t = 0; t < 16; t++) {
        if (scan.temp_init[t] != 0) {
            program.gs_temps_read_first |= static_cast<uint16_t>(1u << t);
        }
    }
    Layout();

    e.skip_invalid = true;
    end_label = NewLabel();
    e.Phas(true);
    e.Nop();
    Prologue();
    // Main emits what its walk passes, from its entry on, and may jump back into code before it: a
    // binary several programs share keeps common tails there, and an entry's own code may end in a
    // jump past the others'. Then main's range is the whole program, the code before the entry
    // first, behind a branch to it.
    const bool before_entry =
        std::find(main_walk.begin(), main_walk.begin() + req.main_offset, true) !=
        main_walk.begin() + req.main_offset;
    OpenContext(before_entry ? 0 : req.main_offset, req.code_size, req.main_offset, true);
    if (before_entry) {
        Branch(Pred::None, LabelAt(req.main_offset));
        EmitRange(0, req.main_offset);
    }
    EmitRange(req.main_offset, req.code_size);
    for (const auto& [at, label] : contexts.back().labels) {
        if (!Bound(label)) {
            Refuse("jump into code main never reaches");
        }
    }
    contexts.pop_back();
    // The prefetched uniforms reach their SAs through the SA image the secondary PDS program
    // DMAs: no secondary USSE program. Loads of its own are not complete when a draw's first
    // vertices read them (docs/sgxd/pds.md).
    for (const uint8_t n : prefetch) {
        program.prefetched_uniforms.emplace_back(static_cast<uint32_t>(uniform_sa[n]), n);
    }
    const std::size_t epilogue_at = e.Count();
    Bind(end_label);
    if (req.mode == VsMode::Draw) {
        Epilogue();
    } else {
        EpilogueStreamOut();
    }
    e.EndVertex();

    // Where the virtual quads may live, in spill order: the temporaries, the primary attributes
    // past the inputs, the output registers until the epilogue writes them, then the temporaries
    // past r63.
    // psp2cgc gives a vertex program up to 112 of them; from r120 the pair and single numbers name
    // the internal registers. Geometry programs need up to 34 quads.
    e.AddClass({Bank::Temp, 0, 64});
    e.AddClass({Bank::PrimAttr, pa_inputs, PaLimit});
    // In order (gs_serial) the loop runs back past epilogue_at: no output register for them.
    if (!req.gs_serial) {
        e.AddClass({Bank::Output, 0, OutScratchEnd, epilogue_at});
    }
    e.AddClass({Bank::Temp, 64, 112});
    Encoder primary;
    const Ir::LowerResult lowered = e.Lower(primary, level);
    primary.FinishNosched();

    program.vertex = true;
    program.per_instance = per_instance;
    program.outselects = OutSelects;
    program.texsize = TexSize;
    program.pa_count = std::max<uint32_t>({static_cast<uint32_t>(pa_inputs), lowered.pa_count, 4u});
    program.temp_count = lowered.temp_count;
    program.literals = {
        0u, std::bit_cast<uint32_t>(-128.0f), 0x00010000u, std::bit_cast<uint32_t>(127.0f),
        0u, std::bit_cast<uint32_t>(1e-6f),   0u,          std::bit_cast<uint32_t>(-1.00001f),
        0u, std::bit_cast<uint32_t>(-1.0f),   0u,          0x00010001u,
        0u};
    static_assert(DataLiteralCount == 13);
    if (!program.buffer_pointer) {
        // No uniform read: the pointer slot stays, as a literal, so the layout is one.
        program.literals.insert(program.literals.begin(), 0u);
    }
    if (req.mode == VsMode::Geometry) {
        program.literals.push_back(
            std::bit_cast<uint32_t>(static_cast<float>(req.gs_max_triangles)));
        program.literals.push_back(0u);
    }
    program.primary = primary.Words();
    return program;
}

struct GsBoundWalk {
    std::span<const uint32_t> code;
    uint32_t extent;
    bool bounded = true;

    /// Whether a run over `begin` up to `end` may change the primitive flag: a SETEMIT there or
    /// in a body it calls.
    bool SetsPrim(uint32_t begin, uint32_t end, int depth) const {
        if (depth > 16) {
            return true;
        }
        for (uint32_t offset = begin; offset < end && offset < extent; offset++) {
            const Instruction instr = {code[offset]};
            const auto& flow = instr.flow_control;
            switch (instr.opcode.Value()) {
            case OpCode::Id::SETEMIT:
                return true;
            case OpCode::Id::CALL:
            case OpCode::Id::CALLC:
            case OpCode::Id::CALLU:
                if (SetsPrim(flow.dest_offset, flow.dest_offset + flow.num_instructions,
                             depth + 1)) {
                    return true;
                }
                break;
            default:
                break;
            }
        }
        return false;
    }

    /// The most triangles a run from `begin` up to `end` (or an END) stores; `prim` is the last
    /// SETEMIT's primitive flag on the way in.
    uint32_t Range(uint32_t begin, uint32_t end, bool prim, int depth) {
        if (depth > 16) {
            bounded = false;
            return 0;
        }
        uint32_t count = 0;
        for (uint32_t offset = begin; offset < end && offset < extent && bounded; offset++) {
            const Instruction instr = {code[offset]};
            const auto& flow = instr.flow_control;
            switch (instr.opcode.Value()) {
            case OpCode::Id::END:
                return count;
            case OpCode::Id::SETEMIT:
                prim = instr.setemit.prim_emit.Value() != 0;
                break;
            case OpCode::Id::EMIT:
                count += prim ? 1 : 0;
                break;
            case OpCode::Id::JMPC:
            case OpCode::Id::JMPU:
                if (flow.dest_offset <= offset) {
                    bounded = false;
                    return 0;
                }
                // Taken or not: the longer of the two ways on.
                return count + std::max(Range(offset + 1, end, prim, depth + 1),
                                        Range(flow.dest_offset, end, prim, depth + 1));
            case OpCode::Id::CALL:
            case OpCode::Id::CALLC:
            case OpCode::Id::CALLU: {
                // A call that returns: its body's EMITs, then on (an END inside it counts the
                // rest too, which only raises the bound). A SETEMIT in the body leaves the flag
                // set, as after an IF: the bound can only be higher.
                const uint32_t body = flow.dest_offset, body_end = body + flow.num_instructions;
                count += Range(body, body_end, prim, depth + 1);
                prim = prim || SetsPrim(body, body_end, depth + 1);
                break;
            }
            case OpCode::Id::IFC:
            case OpCode::Id::IFU:
                return count +
                       std::max(Range(offset + 1, flow.dest_offset, prim, depth + 1),
                                Range(flow.dest_offset, flow.dest_offset + flow.num_instructions,
                                      prim, depth + 1)) +
                       Range(flow.dest_offset + flow.num_instructions, end, true, depth + 1);
            case OpCode::Id::LOOP: {
                // A body that stores a triangle on any round is unbounded; a SETEMIT in it sets
                // the flag for the rounds after the first.
                const bool sets = SetsPrim(offset + 1, flow.dest_offset + 1, depth + 1);
                if (Range(offset + 1, flow.dest_offset + 1, prim || sets, depth + 1) != 0) {
                    bounded = false;
                    return 0;
                }
                prim = prim || sets;
                offset = flow.dest_offset;
                break;
            }
            default:
                break;
            }
        }
        return count;
    }
};

} // Anonymous namespace

uint32_t GsTriangleBound(std::span<const uint32_t> code, uint32_t code_size, uint32_t main_offset) {
    GsBoundWalk walk{code, std::min<uint32_t>(code_size, static_cast<uint32_t>(code.size()))};
    const uint32_t bound = walk.Range(main_offset, walk.extent, false, 0);
    return walk.bounded ? bound : 0;
}

bool GsIndexesUniforms(std::span<const uint32_t> code, uint32_t code_size) {
    const uint32_t extent = std::min<uint32_t>(code_size, static_cast<uint32_t>(code.size()));
    for (uint32_t offset = 0; offset < extent; offset++) {
        const Instruction instr = {code[offset]};
        const OpCode::Info info = instr.opcode.Value().GetInfo();
        // The operand the address register applies to, as ScanProgram notes it.
        if (info.type == OpCode::Type::Arithmetic && instr.common.address_register_index != 0) {
            const bool inverted = (info.subtype & OpCode::Info::SrcInversed) != 0;
            const SourceRegister indexed =
                inverted ? instr.common.GetSrc2(true) : instr.common.GetSrc1(false);
            if (indexed.GetRegisterType() == RegisterType::FloatUniform) {
                return true;
            }
        } else if (info.type == OpCode::Type::MultiplyAdd &&
                   instr.mad.address_register_index != 0) {
            const bool inverted = instr.opcode.Value().EffectiveOpCode() == OpCode::Id::MADI;
            const SourceRegister indexed =
                inverted ? instr.mad.GetSrc3(true) : instr.mad.GetSrc2(false);
            if (indexed.GetRegisterType() == RegisterType::FloatUniform) {
                return true;
            }
        }
    }
    return false;
}

std::optional<Program> EmitVertexProgram(const VsRequest& request, uint32_t& input_regs,
                                        Scratch scratch, std::string* refusal, int level) {
    // A geometry program with an EMIT that different SETEMITs reach (a join between) keeps
    // its emit state in registers too (VertexEmitter::dynamic_emit).
    for (const bool dynamic_emit : {false, true}) {
        try {
            VertexEmitter emitter{request, level, dynamic_emit, scratch};
            return emitter.Emit(input_regs);
        } catch (const std::exception& ex) {
            if (!dynamic_emit && request.mode == VsMode::Geometry &&
                std::string_view{ex.what()} == UnknownEmitState) {
                continue;
            }
            if (refusal) {
                *refusal = ex.what();
            }
            return std::nullopt;
        }
    }
    return std::nullopt;
}

} // namespace SgxdRenderer::Usse
