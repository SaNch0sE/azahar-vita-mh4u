// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <bit>
#include <array>
#include <cstdio>
#include <optional>
#include <cstring>
#include <initializer_list>
#include <stdexcept>
#include <utility>
#include "video_core/renderer_sgxd/usse/usse_encoder.h"

namespace SgxdRenderer::Usse {

namespace {

/// A pattern's fields, worked out at compile time: its fixed ones and each run of a letter.
struct Layout {
    struct Run {
        char letter;
        uint8_t shift;
        uint8_t width;
    };
    uint64_t ones = 0;
    std::array<Run, 64> runs{};
    uint8_t count = 0;
};

constexpr Layout MakeLayout(const char* pattern) {
    Layout layout{};
    int i = 0;
    while (i < 64) {
        const char c = pattern[i];
        int j = i;
        while (j < 64 && pattern[j] == c) {
            j++;
        }
        const int width = j - i;
        const int shift = 64 - j;
        if (c == '1') {
            layout.ones |= ((1ull << width) - 1) << shift;
        } else if (c != '0' && c != '-') {
            layout.runs[layout.count++] = {c, static_cast<uint8_t>(shift),
                                           static_cast<uint8_t>(width)};
        }
        i = j;
    }
    return layout;
}

// Vita3K's decoder patterns, verbatim: 64 characters, MSB first; a run of one letter is
// one field, '0'/'1' are fixed bits, '-' is don't care (written as 0).
constexpr Layout PatV32Nmad =
    MakeLayout("00001pppsrrydcbawwwwneeeemmoiittkkllffffffzzzzzzzggghhhhhhjjjjjj");
constexpr Layout PatV16Nmad =
    MakeLayout("00010pppsrrydcbawwwwneeeemmoiittkkllffffffzzzzzzzggghhhhhhjjjjjj");
constexpr Layout PatVmad2 =
    MakeLayout("00000dpps-ry-cbawwwineeeemmookttffgghhhhhhzzjjllllllqqqqqquuuuuu");
// VECMAD has no 3-bit predicate: USE1 bit 26 is its F16 flag and bits 25..24 the 2-bit short
// predicate (SGXVEC_USE1_VECMAD_F16, SGXVEC_USE1_VECMAD_SVPRED_SHIFT).
constexpr Layout PatVmad =
    MakeLayout("000110ppsg1oderaaittnwwwwcbfhzkkjjllmmmmmmqqqquuuuvvxyAAAABBBBBB");
constexpr Layout PatVdp =
    MakeLayout("00011pppsc0oderaagttnwwwwbflllkkhhiijjjjjjzzzzmmmqqqyyyxxxuuuuuu");
constexpr Layout PatVcomp =
    MakeLayout("00110pppsddyenr-aaaaobbccmmff-ttkk--ggggggg-------hhhhhhh---wwww");
constexpr Layout PatVmov =
    MakeLayout("00111pppstrydecbmmaanoooiwwwwkllffgghhhhjjjjjjqqqqqquuuuuuvvvvvv");
constexpr Layout PatVpck =
    MakeLayout("01000pppsnuydercaaaaffftttmmmmbbkkllgggggggoohiijjqqqqqqvwwwwwwx");
// SOP2 and SOP3 (Vita3K's layouts).
constexpr Layout PatSop2 =
    MakeLayout("10000ppcsnaaderbmooofllggghhhittkkjjqqqqqqquvvwwxyzzzzzzzAAAAAAA");
constexpr Layout PatSop3 =
    MakeLayout("10001ppcsnooderbmallfgghhhiiikttjjqquuuuuuuvvvvvvvwwwwwwwxxxxxxx");
// Bit 54 ('-' in Vita3K's pattern) is the compare's nosched bit, where VPCK and SMP have
// theirs; exposed as 'K'. (It looked like "set on every compare except the one feeding a
// kill" until the liveness rule explained it.)
constexpr Layout PatVtst =
    MakeLayout("01001pppsKoydrceavttiizzmhhhnnbbkkffgggggggwlluuuujjjjjjjqqqqqqq");
constexpr Layout PatSmp =
    MakeLayout("11100pppsn-ymrceffaaddlltbbggkhhiijjoooooooqqqqqqquuuuuuuvvvvvvv");
constexpr Layout PatVldst =
    MakeLayout("111oopppsnmycrbakkkkddeetgffihjlqquuvvvvvvvwwwwwwwxxxxxxxzzzzzzz");
constexpr Layout PatVbw =
    MakeLayout("01ooopppsnrydecxaaaaittttthhbwkkffggjjjjjjjlllllllmmmmmmmqqqqqqq");
constexpr Layout PatI16Mad =
    MakeLayout("10100ppasnredbck-tttmmffoolhhgiijjqquuuuuuuvvvvvvvwwwwwwwxxxxxxx");
constexpr Layout PatI32Mad =
    MakeLayout("10101pps-nrcdeba0tttif00yy000kgghhjjlllllllmmmmmmmoooooooqqqqqqq");
constexpr Layout PatDepthf =
    MakeLayout("11111011s-11recb----npp---tffa--kkddggggggghhhhhhhiiiiiiijjjjjjj");
constexpr Layout PatBr =
    MakeLayout("11111ppps000e-----wynba00r----------------iloooooooooooooooooooo");

struct Field {
    char letter;
    uint64_t value;
};

[[noreturn]] void Fail(const char* what) {
    throw std::logic_error(what);
}

uint64_t Pack(const Layout& layout, std::initializer_list<Field> fields) {
    // Each letter's field, the first supplied: the patterns' letters are 'A' to 'z'.
    std::array<uint8_t, 'z' - 'A' + 1> at;
    at.fill(0xff);
    uint8_t k = 0;
    for (const Field& f : fields) {
        if (f.letter < 'A' || f.letter > 'z') {
            Fail("usse: pattern field letter out of range");
        }
        uint8_t& slot = at[f.letter - 'A'];
        if (slot == 0xff) {
            slot = k;
        }
        k++;
    }
    uint64_t word = layout.ones;
    for (uint8_t r = 0; r < layout.count; r++) {
        const Layout::Run& run = layout.runs[r];
        const uint8_t index = at[run.letter - 'A'];
        if (index == 0xff) {
            Fail("usse: pattern field not supplied");
        }
        const uint64_t value = fields.begin()[index].value;
        if (run.width < 64 && value >> run.width) {
            const std::string what = std::string("usse: field value does not fit: '") + run.letter +
                                     "' = " + std::to_string(value) + " in " +
                                     std::to_string(run.width) + " bits";
            Fail(what.c_str());
        }
        word |= value << run.shift;
    }
    return word;
}

// --- operand encodings ---

// Destination bank: (bank, ext).
std::pair<uint64_t, uint64_t> DestBank(Bank bank) {
    switch (bank) {
    case Bank::Temp:
    case Bank::Internal:
        return {0, 0};
    case Bank::Output:
        return {1, 0};
    case Bank::PrimAttr:
        return {2, 0};
    case Bank::SecAttr:
        return {0, 1};
    case Bank::Const:
        return {1, 1};
    }
    Fail("usse: bad dest bank");
}

// Source 1/2 bank: (bank, ext).
std::pair<uint64_t, uint64_t> Src12Bank(Bank bank) {
    switch (bank) {
    case Bank::Temp:
    case Bank::Internal:
        return {0, 0};
    case Bank::Output:
        return {1, 0};
    case Bank::PrimAttr:
        return {2, 0};
    case Bank::SecAttr:
        return {3, 0};
    case Bank::Const:
        return {1, 1};
    }
    Fail("usse: bad source bank");
}

// Source 0 bank (the three-source forms): (bank, ext).
std::pair<uint64_t, uint64_t> Src0Bank(Bank bank) {
    switch (bank) {
    case Bank::Temp:
    case Bank::Internal:
        return {0, 0};
    case Bank::PrimAttr:
        return {1, 0};
    case Bank::Output:
        return {0, 1};
    case Bank::SecAttr:
        return {1, 1};
    default:
        break;
    }
    Fail("usse: bad source-0 bank");
}

// A register number in a field that addresses 64-bit pairs (`is_double_regs` in the
// decoder): the index halved, and an internal register as the top of the temp bank.
// `bits` is the decoder's reg_bits: 7 for a 6-bit field, 8 for a 7-bit one.
uint64_t DoubledNum(Reg reg, int bits) {
    if (reg.bank == Bank::Internal) {
        const int limit = (1 << bits) - 8;
        return static_cast<uint64_t>((limit + reg.n * 2) >> 1);
    }
    if (reg.bank == Bank::Const) {
        return reg.n; // constant indices are not doubled
    }
    if (reg.n & 1) {
        static char message[64];
        std::snprintf(message, sizeof(message), "usse: odd register %d of bank %d cannot address a pair",
                      reg.n, static_cast<int>(reg.bank));
        Fail(message);
    }
    return reg.n >> 1;
}

// A register number in a field that addresses single 32-bit registers.
uint64_t SingleNum(Reg reg, int bits) {
    if (reg.bank == Bank::Internal) {
        return static_cast<uint64_t>((1 << bits) - 4 + reg.n);
    }
    return reg.n;
}

uint64_t ExtVecPred(Pred p) {
    switch (p) {
    case Pred::None:
        return 0;
    case Pred::P0:
        return 1;
    case Pred::P1:
        return 2;
    case Pred::P2:
        return 3;
    case Pred::NotP0:
        return 4;
    case Pred::NotP1:
        return 5;
    }
    return 0;
}

// VECMAD's short form: none, p0, !p0 (SGXVEC_USE_VEC_SVPRED_*; 3 is per-channel, unused).
uint64_t MadPred(Pred p) {
    switch (p) {
    case Pred::None:
        return 0;
    case Pred::P0:
        return 1;
    case Pred::NotP0:
        return 2;
    default:
        Fail("usse: mad takes only p0 or !p0 as its predicate");
    }
}

uint64_t ExtPred(Pred p) {
    switch (p) {
    case Pred::None:
        return 0;
    case Pred::P0:
        return 1;
    case Pred::P1:
        return 2;
    case Pred::P2:
        return 3;
    case Pred::NotP0:
        return 5;
    case Pred::NotP1:
        return 6;
    }
    return 0;
}

// The 12-bit swizzle of V32NMAD's first source: three bits per channel.
uint64_t Swizzle12(const Swizzle& s) {
    return static_cast<uint64_t>(s[0]) | static_cast<uint64_t>(s[1]) << 3 |
           static_cast<uint64_t>(s[2]) << 6 | static_cast<uint64_t>(s[3]) << 9;
}

#define SW(a, b, c, d) Swizzle{Ch::a, Ch::b, Ch::c, Ch::d}
constexpr Swizzle Vec4Std[16] = {
    SW(X, X, X, X), SW(Y, Y, Y, Y), SW(Z, Z, Z, Z), SW(W, W, W, W),
    SW(X, Y, Z, W), SW(Y, Z, W, W), SW(X, Y, Z, Z), SW(X, X, Y, Z),
    SW(X, Y, X, Y), SW(X, Y, W, Z), SW(Z, X, Y, W), SW(Z, W, Z, W),
    SW(Y, Z, X, Z), SW(X, X, Y, Y), SW(X, Z, W, W), SW(X, Y, Z, One),
};
constexpr Swizzle Vec4Ext[16] = {
    SW(Y, Z, X, W), SW(Z, W, X, Y), SW(X, Z, W, Y), SW(Y, Y, W, W),
    SW(W, Y, Z, W), SW(W, Z, W, Z), SW(X, Y, Z, X), SW(Z, Z, W, W),
    SW(X, W, Z, X), SW(Y, Y, Y, X), SW(Y, Y, Y, Z), SW(X, Z, Y, W),
    SW(X, X, X, Y), SW(Z, Y, X, W), SW(Y, Y, Z, Z), SW(Z, Z, Z, Y),
};
// Three-wide tables: the fourth channel is unused.
constexpr Swizzle Vec3Std[16] = {
    SW(X, X, X, X), SW(Y, Y, Y, X), SW(Z, Z, Z, X), SW(W, W, W, X),
    SW(X, Y, Z, X), SW(Y, Z, W, X), SW(X, X, Y, X), SW(X, Y, X, X),
    SW(Y, Y, X, X), SW(Y, Y, Z, X), SW(Z, X, Y, X), SW(X, Z, Y, X),
    SW(Y, Z, X, X), SW(Z, Y, X, X), SW(Z, Z, Y, X), SW(X, Y, One, X),
};
constexpr Swizzle Vec3Ext[11] = {
    SW(X, Y, Y, X),          SW(Y, X, Y, X),          SW(X, X, Z, X),
    SW(Y, X, X, X),          SW(X, Y, Zero, X),       SW(X, One, Zero, X),
    SW(Zero, Zero, Zero, X), SW(One, One, One, X),    SW(Half, Half, Half, X),
    SW(Two, Two, Two, X),    SW(X, Zero, Zero, X),
};
#undef SW

bool SameChannels(const Swizzle& a, const Swizzle& b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

// A swizzle as (table index, extended) in the four-wide tables.
std::pair<uint64_t, uint64_t> Vec4Swizzle(const Swizzle& s) {
    for (int i = 0; i < 16; i++) {
        if (SameChannels(Vec4Std[i], s, 4)) {
            return {static_cast<uint64_t>(i), 0};
        }
    }
    for (int i = 0; i < 16; i++) {
        if (SameChannels(Vec4Ext[i], s, 4)) {
            return {static_cast<uint64_t>(i), 1};
        }
    }
    static const char* names = "xyzw012h";
    static char message[64];
    std::snprintf(message, sizeof(message), "usse: swizzle %c%c%c%c not in the four-wide tables",
                  names[static_cast<int>(s[0])], names[static_cast<int>(s[1])],
                  names[static_cast<int>(s[2])], names[static_cast<int>(s[3])]);
    Fail(message);
}

// The same in the three-wide tables (the fourth channel ignored).
std::pair<uint64_t, uint64_t> Vec3Swizzle(const Swizzle& s) {
    for (int i = 0; i < 16; i++) {
        if (SameChannels(Vec3Std[i], s, 3)) {
            return {static_cast<uint64_t>(i), 0};
        }
    }
    for (int i = 0; i < 11; i++) {
        if (SameChannels(Vec3Ext[i], s, 3)) {
            return {static_cast<uint64_t>(i), 1};
        }
    }
    Fail("usse: swizzle not in the three-wide tables");
}

} // Anonymous namespace

std::optional<Swizzle> Vec4SwizzleForPair(Ch c0, Ch c1) {
    for (const Swizzle* table : {Vec4Std, Vec4Ext}) {
        for (int i = 0; i < 16; i++) {
            if (table[i][0] == c0 && table[i][1] == c1) {
                return table[i];
            }
        }
    }
    return std::nullopt;
}

bool InVec4StdTable(const Swizzle& s) {
    for (const Swizzle& t : Vec4Std) {
        if (SameChannels(t, s, 4)) {
            return true;
        }
    }
    return false;
}

bool InVec4Tables(const Swizzle& s) {
    if (InVec4StdTable(s)) {
        return true;
    }
    for (const Swizzle& t : Vec4Ext) {
        if (SameChannels(t, s, 4)) {
            return true;
        }
    }
    return false;
}

bool InVec3Tables(const Swizzle& s) {
    for (int i = 0; i < 16; i++) {
        if (SameChannels(Vec3Std[i], s, 3)) {
            return true;
        }
    }
    for (int i = 0; i < 11; i++) {
        if (SameChannels(Vec3Ext[i], s, 3)) {
            return true;
        }
    }
    return false;
}

namespace {

// V32NMAD's second source only has the standard four-wide table.
uint64_t Vec4StdSwizzle(const Swizzle& s) {
    const auto [index, ext] = Vec4Swizzle(s);
    if (ext) {
        Fail("usse: swizzle needs the extended table where only the standard one exists");
    }
    return index;
}

} // Anonymous namespace

bool StdSwizzleForPair(Ch c0, Ch c1, Swizzle& out) {
    for (const Swizzle& s : Vec4Std) {
        if (s[0] == c0 && s[1] == c1) {
            out = s;
            return true;
        }
    }
    return false;
}

Reg Encoder::Map(Reg r) const {
    if (secondary && r.bank == Bank::SecAttr) {
        return {Bank::PrimAttr, r.n};
    }
    return r;
}

void Encoder::Push(uint64_t word, uint8_t nosched_bit, uint16_t reads, uint16_t writes) {
    words.push_back(word);
    meta.push_back({reads, writes, nosched_bit});
}

uint16_t Encoder::Reads(const Src& src, uint8_t lanes) {
    // Lane j of the result reads src.swz[j]; only the lanes written count, since a read
    // of a component nothing ever wrote would keep it live back to the program's start.
    if (src.reg.bank != Bank::Internal) {
        return 0;
    }
    uint16_t mask = 0;
    for (unsigned j = 0; j < 4; j++) {
        const Ch ch = src.swz[j];
        if ((lanes & (1u << j)) && ch <= Ch::W) {
            mask |= static_cast<uint16_t>(1u << (src.reg.n * 4 + static_cast<unsigned>(ch)));
        }
    }
    return mask;
}

uint16_t Encoder::ReadsOne(Reg reg, uint8_t comp) {
    return reg.bank == Bank::Internal ? static_cast<uint16_t>(1u << (reg.n * 4 + (comp & 3))) : 0;
}

uint16_t Encoder::ReadsAll(Reg reg) {
    return reg.bank == Bank::Internal ? static_cast<uint16_t>(0xfu << (reg.n * 4)) : 0;
}

uint16_t Encoder::Writes(const Dst& dst, Pred pred) {
    // A predicated write may not happen, so it ends no live range.
    if (dst.reg.bank != Bank::Internal || pred != Pred::None) {
        return 0;
    }
    return static_cast<uint16_t>((dst.mask & 0xf) << (dst.reg.n * 4));
}

void Encoder::FinishNosched() {
    const std::size_t n = words.size();
    // after[i]: the components live once word i has executed.
    std::vector<uint16_t> after(n, 0);
    uint16_t live = 0;
    for (std::size_t i = n; i-- > 0;) {
        after[i] = live;
        live = static_cast<uint16_t>((live & ~meta[i].writes) | meta[i].reads);
    }
    for (std::size_t i = 0; i < n; i++) {
        if (meta[i].bit == 0) {
            continue;
        }
        const bool set = i + 1 < n && after[i + 1] != 0;
        const uint64_t bit = 1ull << meta[i].bit;
        words[i] = set ? (words[i] | bit) : (words[i] & ~bit);
    }
}

void Encoder::EndVertex() {
    Push(0xfb275000a0200000ull, 0, 0, 0);
}

void Encoder::EndSecondary() {
    Push(0xf804014000000000ull, 0, 0, 0);
}

void Encoder::WdfVertex(uint8_t drc) {
    // Bit 43 is the wait's nosched bit (psp2cgc's `f9200800...` form before an internal
    // register write): without it the boundary after the instruction that follows the wait is
    // unguarded, and every dynamic uniform read is corrupted.
    Push(drc ? 0xf920000100000000ull : 0xf920000000000000ull, 43, 0, 0);
}

std::size_t Encoder::Br(Pred pred) {
    const std::size_t index = words.size();
    // Letters as Vita3K's pattern names them: s syncend, e exception, w pwait, y sync_ext,
    // n nosched, b br_monitor, a save_link, r br_type, i any_inst, l all_inst.
    Push(Pack(PatBr, {
        {'p', ExtPred(pred)}, {'s', 0}, {'e', 0}, {'w', 0}, {'y', 0}, {'n', 0}, {'b', 0},
        {'a', 0}, {'r', 1}, {'i', 0}, {'l', 0}, {'o', 0},
    }), 43, 0, 0);
    return index;
}

void Encoder::PatchBr(std::size_t index, std::size_t target) {
    const int64_t off = static_cast<int64_t>(target) - static_cast<int64_t>(index);
    if (off < -(1 << 19) || off >= (1 << 19)) {
        Fail("usse: branch offset out of range");
    }
    words[index] = (words[index] & ~0xfffffull) | (static_cast<uint64_t>(off) & 0xfffff);
}

namespace {
// Integer operand banks: single 32-bit registers, 7-bit fields; an immediate is bank 2
// with the extension bit.
std::pair<uint64_t, uint64_t> IntSrc12Bank(Bank bank) {
    switch (bank) {
    case Bank::Temp:
    case Bank::Internal:
        return {0, 0};
    case Bank::Output:
        return {1, 0};
    case Bank::PrimAttr:
        return {2, 0};
    case Bank::SecAttr:
        return {3, 0};
    default:
        break;
    }
    Fail("usse: bad integer source bank");
}
} // Anonymous namespace

void Encoder::Lda32(Reg dst, Reg base_sa, const Reg* offset_reg, uint8_t offset_words,
                    uint8_t count, uint8_t drc) {
    dst = Map(dst);
    base_sa = Map(base_sa);
    if (count != 4 && count != 16) {
        Fail("usse: a load fetches 4 or 16 words");
    }
    const auto [bb, bext] = Src0Bank(base_sa.bank);
    uint64_t dest_pa = 0;
    switch (dst.bank) {
    case Bank::Temp:
        break;
    case Bank::PrimAttr:
        dest_pa = 1;
        break;
    default:
        Fail("usse: a load lands in a temporary, a primary attribute or (secondary program) a secondary attribute");
    }
    uint64_t s1b = 2, s1ext = 1, s1n = offset_words;
    if (offset_reg != nullptr) {
        const Reg r = Map(*offset_reg);
        const auto [b, e] = IntSrc12Bank(r.bank);
        s1b = b;
        s1ext = e;
        s1n = SingleNum(r, 7);
    }
    Push(Pack(PatVldst, {
        {'o', 1}, {'p', 0}, {'s', skip_invalid ? 1u : 0u}, {'n', 0}, {'m', 0}, {'y', 0}, {'c', 0},
        {'r', bext}, {'b', s1ext}, {'a', 1}, {'k', static_cast<uint64_t>(count - 1)}, {'d', 0},
        {'e', 0}, {'t', dest_pa}, {'g', 0}, {'f', 0}, {'i', 0}, {'h', bb}, {'j', 0}, {'l', drc},
        {'q', s1b}, {'u', 2}, {'v', SingleNum(dst, 7)}, {'w', SingleNum(base_sa, 7)},
        {'x', s1n}, {'z', 0},
    }), 54, 0, 0);
}

void Encoder::Sta32(Reg base, uint8_t offset_words, Reg value, uint8_t drc) {
    base = Map(base);
    value = Map(value);
    if (offset_words > 127) {
        Fail("usse: a store's immediate offset is 7 bits");
    }
    const auto [bb, bext] = Src0Bank(base.bank);
    const auto [vb, vext] = IntSrc12Bank(value.bank);
    // op 2 (store), one word; moe_expand and the L1/L2 bypass set as in the firmware's word
    // f022000280004282.
    Push(Pack(PatVldst, {
        {'o', 2}, {'p', 0}, {'s', skip_invalid ? 1u : 0u}, {'n', 0}, {'m', 1}, {'y', 0}, {'c', 0},
        {'r', bext}, {'b', 1}, {'a', vext}, {'k', 0}, {'d', 0}, {'e', 0}, {'t', 0}, {'g', 0},
        {'f', 0}, {'i', 0}, {'h', bb}, {'j', 1}, {'l', drc}, {'q', 2}, {'u', vb}, {'v', 0},
        {'w', SingleNum(base, 7)}, {'x', offset_words}, {'z', SingleNum(value, 7)},
    }), 54, 0, 0);
}

void Encoder::Idf(uint8_t drc) {
    Push(drc ? 0xf820000100000000ull : 0xf820000000000000ull, 0, 0, 0);
}

void Encoder::Bitwise(BwOp op, Reg dst, IOp src1, IOp src2, Pred pred) {
    dst = Map(dst);
    const auto [db, dext] = DestBank(dst.bank);
    const auto operand = [this](const IOp& o) -> std::array<uint64_t, 3> {
        if (o.imm) {
            if (o.value > 127) {
                Fail("usse: bitwise immediate is 7 bits");
            }
            return {2, 1, o.value};
        }
        const Reg r = Map(o.reg);
        const auto [b, e] = IntSrc12Bank(r.bank);
        return {b, e, SingleNum(r, 7)};
    };
    const auto s1 = operand(src1);
    const auto s2 = operand(src2);
    uint64_t op1 = 2, op2 = 0;
    switch (op) {
    case BwOp::And: op1 = 2; op2 = 0; break;
    case BwOp::Or: op1 = 2; op2 = 1; break;
    case BwOp::Xor: op1 = 3; op2 = 0; break;
    }
    Push(Pack(PatVbw, {
        {'o', op1}, {'p', ExtPred(pred)}, {'s', skip_invalid ? 1u : 0u}, {'n', 0}, {'r', 0}, {'y', 0},
        {'d', dext}, {'e', 0}, {'c', s1[1]}, {'x', s2[1]}, {'a', 0}, {'i', 0}, {'t', 0}, {'h', 0},
        {'b', op2}, {'w', 0}, {'k', db}, {'f', s1[0]}, {'g', s2[0]}, {'j', SingleNum(dst, 7)},
        {'l', 0}, {'m', s1[2]}, {'q', s2[2]},
    }), 54, 0, 0);
}

void Encoder::MadI16(Reg dst, Reg src0, Reg src1, IOp src2) {
    dst = Map(dst);
    src0 = Map(src0);
    src1 = Map(src1);
    const auto [db, dext] = DestBank(dst.bank);
    uint64_t s0b = 0;
    switch (src0.bank) {
    case Bank::Temp: s0b = 0; break;
    case Bank::PrimAttr: s0b = 1; break;
    default: Fail("usse: mad.i16's first source is a temporary or a primary attribute");
    }
    const auto [s1b, s1ext] = IntSrc12Bank(src1.bank);
    uint64_t s2b = 2, s2ext = 1, s2n = src2.value, s2fmt = 1;
    if (!src2.imm) {
        const Reg r = Map(src2.reg);
        const auto [b, e] = IntSrc12Bank(r.bank);
        s2b = b;
        s2ext = e;
        s2n = SingleNum(r, 7);
        s2fmt = 0;
    }
    Push(Pack(PatI16Mad, {
        {'p', 0}, {'a', 0}, {'s', skip_invalid ? 1u : 0u}, {'n', 0}, {'r', 0}, {'e', 0}, {'d', dext},
        {'b', 0}, {'c', s1ext}, {'k', s2ext}, {'t', 0}, {'m', 2}, {'f', s2fmt}, {'o', 0}, {'l', 0},
        {'h', 0}, {'g', s0b}, {'i', db}, {'j', s1b}, {'q', s2b}, {'u', SingleNum(dst, 7)},
        {'v', SingleNum(src0, 7)}, {'w', SingleNum(src1, 7)}, {'x', s2n},
    }), 54, 0, 0);
}

void Encoder::MadI32(Reg dst, Reg src0, uint8_t imm, Reg src2) {
    dst = Map(dst);
    src0 = Map(src0);
    src2 = Map(src2);
    const auto [db, dext] = DestBank(dst.bank);
    uint64_t s0b = 0;
    switch (src0.bank) {
    case Bank::Temp:
    case Bank::Internal: s0b = 0; break;
    case Bank::PrimAttr: s0b = 1; break;
    default: Fail("usse: mad.i32's first source is a temporary or a primary attribute");
    }
    // The immediate is a signed 7 bits: 0x44 multiplies as -60.
    if (imm > 63) {
        Fail("usse: mad.i32's immediate is a signed 7 bits");
    }
    const auto [s2b, s2ext] = IntSrc12Bank(src2.bank);
    // The compiler's words have bit 55 set and bit 56 clear, where Vita3K's pattern puts
    // skipinv at 56 and a don't-care at 55; written as the compiler writes them.
    Push(Pack(PatI32Mad, {
        {'p', 0}, {'s', 0}, {'n', 0}, {'r', 0}, {'c', 0}, {'d', 0}, {'e', dext},
        {'b', 1}, {'a', s2ext}, {'t', 0}, {'i', 1}, {'f', 0}, {'y', 2}, {'k', s0b}, {'g', db},
        {'h', 2}, {'j', s2b}, {'l', SingleNum(dst, 7)}, {'m', SingleNum(src0, 7)}, {'o', imm},
        {'q', SingleNum(src2, 7)},
    }) | (1ull << 55), 54, 0, 0);
}

void Encoder::PackS16(Reg dst, Reg src, uint8_t comp, Pred pred) {
    dst = Map(dst);
    src = Map(src);
    const auto [db, dext] = DestBank(dst.bank);
    const auto [sb, sext] = Src12Bank(src.bank);
    Push(Pack(PatVpck, {
        {'p', ExtPred(pred)}, {'s', skip_invalid ? 1u : 0u}, {'n', 0}, {'u', 0}, {'y', 0}, {'d', dext},
        {'e', 0}, {'r', sext}, {'c', 1}, {'a', 0}, {'f', 6}, {'t', 4}, {'m', 1}, {'b', db}, {'k', sb},
        {'l', 2}, {'g', SingleNum(dst, 7)}, {'o', 0}, {'h', 0}, {'i', 0}, {'j', 0},
        {'q', DoubledNum(src, 7)}, {'v', static_cast<uint64_t>((comp >> 1) & 1)}, {'w', 0},
        {'x', static_cast<uint64_t>(comp & 1)},
    }), 54, ReadsOne(src, comp), 0);
}

void Encoder::UnpackInt(Reg dst, Reg src, uint8_t select, uint8_t format) {
    dst = Map(dst);
    src = Map(src);
    if (select > 3 || (format != 0 && format != 1 && format != 4) || (src.n & 1) ||
        src.bank == Bank::Internal || src.bank == Bank::Const) {
        Fail("usse: an integer unpack reads lanes 0-3 of U8, S8 or S16 from an even "
             "unified-store register");
    }
    const auto [db, dext] = DestBank(dst.bank);
    const auto [sb, sext] = Src12Bank(src.bank);
    // The hardware's form (docs/sgxd/usse.md): lane x's select is lo bits 1..0, the low bit of the
    // source 2 number, then x. Without the scale flag (h) a byte converts to its integer value, and
    // only lane x is written: so one byte, mask x, one register. S8 and S16 (the PICA's BYTE and
    // SHORT loaders) take the same form with their source format. A 16-bit lane is a half: lo bit 0
    // picks it within a register and lo bit 7 the pair's register, as PackS16 picks an F32 lane
    // (halves 2 and 3 through lo bit 1 read garbage).
    const bool half = format == 4;
    Push(Pack(PatVpck, {
        {'p', 0}, {'s', skip_invalid ? 1u : 0u}, {'n', 0}, {'u', 0}, {'y', 0}, {'d', dext},
        {'e', 0}, {'r', sext}, {'c', 0}, {'a', 0}, {'f', format}, {'t', 6}, {'m', 1}, {'b', db},
        {'k', sb}, {'l', 0}, {'g', SingleNum(dst, 7)}, {'o', 3}, {'h', 0}, {'i', 1}, {'j', 2},
        {'q', DoubledNum(src, 7)}, {'v', half ? static_cast<uint64_t>(select >> 1) : 0u},
        {'w', half ? 0u : static_cast<uint64_t>(select >> 1)},
        {'x', static_cast<uint64_t>(select & 1)},
    }), 54, 0, 0);
}

void Encoder::Phas(bool wait_all) {
    Push(wait_all ? 0xfa44070000000000ull : 0xfa44010000000000ull, 0, 0, 0);
}

void Encoder::Nop() {
    Push(0xf800094000000000ull, 0, 0, 0);
}

void Encoder::NopAfterKill() {
    Push(0xf804014000000000ull, 0, 0, 0);
}

void Encoder::Wdf(uint8_t drc) {
    // The fragment form always carried bit 43 (nosched) since it was copied from a
    // compiler sequence that needed it; now placed by FinishNosched like every other.
    Push(drc ? 0xf920000100000000ull : 0xf920000000000000ull, 43, 0, 0);
}

void Encoder::KillUnlessP1() {
    kill_indices.push_back(words.size());
    Push(0xf9300406f0000000ull, 0, 0, 0); // source registers filled in by PatchKills
}

void Encoder::DepthF(Reg src, uint8_t control_sa) {
    src = Map(src);
    if (src.bank != Bank::Temp) {
        Fail("usse: depthf reads a temporary");
    }
    // psp2cgc's word for `depthf r0` with the control word at sa2: fb300000f0000102.
    Push(Pack(PatDepthf, {
        {'s', 0}, {'r', 0}, {'e', 0}, {'c', 0}, {'b', 0}, {'n', 0}, {'p', 0}, {'t', 0}, {'f', 0},
        {'a', 0}, {'k', 3}, {'d', 3}, {'g', 0}, {'h', SingleNum(src, 7)}, {'i', control_sa},
        {'j', control_sa},
    }), 43, 0, 0);
}

void Encoder::PatchKills(uint32_t control_sa) {
    // Bits 13-7 and 6-0 are the op's src1 and src2 register numbers (the DEPTHF layout,
    // which shares the visibility-test category); the bank bits above them stay as the
    // compiler writes them.
    const uint64_t regs = static_cast<uint64_t>(control_sa & 0x7f);
    for (const std::size_t index : kill_indices) {
        words[index] = (words[index] & ~0x3fffull) | (regs << 7) | regs;
    }
}

namespace {

bool UnifiedStore(Bank bank) {
    return bank != Bank::Internal && bank != Bank::Const;
}

// The unified-store words a source reads for the lanes in mask (bit k: word reg.n + k of its
// bank, Z and W counted as words 2 and 3), or 0 for another bank.
uint32_t SourceWords(const Src& src, uint8_t mask) {
    uint32_t words = 0;
    if (!UnifiedStore(src.reg.bank)) {
        return 0;
    }
    for (unsigned j = 0; j < 4; j++) {
        if ((mask & (1u << j)) && src.swz[j] <= Ch::W) {
            words |= 1u << static_cast<unsigned>(src.swz[j]);
        }
    }
    return words;
}

// Whether a write of dst's lanes changes a word src reads for its lanes in mask.
bool Overwrites(const Dst& dst, const Src& src, uint8_t mask) {
    if (dst.reg.bank != src.reg.bank || !UnifiedStore(src.reg.bank)) {
        return false;
    }
    const uint32_t read = SourceWords(src, mask);
    for (unsigned k = 0; k < 4; k++) {
        if ((read & (1u << k)) && src.reg.n + k >= dst.reg.n && src.reg.n + k < dst.reg.n + 2u &&
            (dst.mask & (1u << (src.reg.n + k - dst.reg.n)))) {
            return true;
        }
    }
    return false;
}

} // Anonymous namespace

void Encoder::Vec(VecOp op, Dst dst, Src src1, Src src2, Pred pred, bool nosched) {
    // An F32 vector operation on the unified store works on a register pair (docs/sgxd/usse.md): a
    // unified-store destination takes mask bits 0 and 1 alone (bits 2 and 3 write nothing), and a
    // unified-store second source does not read the words a z or w lane names (the first source
    // does). Lanes z and w of a unified-store vector are the next pair's x and y, so such a write
    // is made there, its sources' z and w lanes moved to x and y.
    if (!UnifiedStore(Map(dst.reg).bank) || (dst.mask & 0xc) == 0) {
        VecPair(op, dst, src1, src2, pred, nosched);
        return;
    }
    const Dst low{dst.reg, static_cast<uint8_t>(dst.mask & 0x3)};
    const Dst high{Reg{dst.reg.bank, static_cast<uint8_t>(dst.reg.n + 2)},
                   static_cast<uint8_t>(dst.mask >> 2)};
    Src high1 = src1, high2 = src2;
    high1.swz = {src1.swz[2], src1.swz[3], src1.swz[2], src1.swz[3]};
    high2.swz = {src2.swz[2], src2.swz[3], src2.swz[2], src2.swz[3]};
    if (low.mask == 0) {
        VecPair(op, high, high1, high2, pred, nosched);
        return;
    }
    // Two instructions: the second must not read what the first wrote.
    if (!Overwrites(low, high1, high.mask) && !Overwrites(low, high2, high.mask)) {
        VecPair(op, low, src1, src2, pred, nosched);
        VecPair(op, high, high1, high2, pred, nosched);
    } else if (!Overwrites(high, src1, low.mask) && !Overwrites(high, src2, low.mask)) {
        VecPair(op, high, high1, high2, pred, nosched);
        VecPair(op, low, src1, src2, pred, nosched);
    } else {
        Fail("usse: a unified-store vector write whose two pairs read each other");
    }
}

void Encoder::VecPair(VecOp op, Dst dst, Src src1, Src src2, Pred pred, bool nosched) {
    // A unified-store second source's z and w read nothing (usse_ir's SecondSourceInPair keeps
    // them out; an internal destination's lanes z and w count too).
    const bool pair_dst = UnifiedStore(Map(dst.reg).bank);
    const uint8_t lanes = pair_dst ? dst.mask & 0x3 : dst.mask;
    if (SourceWords(Src{Map(src2.reg), src2.swz, src2.mod}, lanes) & 0xc) {
        Fail("usse: a unified-store second source's z or w");
    }
    if (pair_dst && std::find(std::begin(Vec4Std), std::end(Vec4Std), src2.swz) ==
                        std::end(Vec4Std)) {
        // Only lanes x and y count: a standard swizzle with the same two.
        Swizzle pair;
        if (!StdSwizzleForPair(src2.swz[0], src2.swz[1], pair)) {
            Fail("usse: a vector operation's second source pair no standard swizzle has");
        }
        src2.swz = pair;
    }
    VecWord(op, dst, src1, src2, pred, nosched);
}

void Encoder::VecWord(VecOp op, Dst dst, Src src1, Src src2, Pred pred, bool nosched) {
    dst.reg = Map(dst.reg);
    src1.reg = Map(src1.reg);
    src2.reg = Map(src2.reg);

    const auto [db, dext] = DestBank(dst.reg.bank);
    const auto [s1b, s1ext] = Src12Bank(src1.reg.bank);
    const auto [s2b, s2ext] = Src12Bank(src2.reg.bank);
    if (src2.mod == Mod::Neg || src2.mod == Mod::NegAbs) {
        Fail("usse: the second source of a vector operation cannot be negated");
    }
    const uint64_t swz1 = Swizzle12(src1.swz);
    Push(Pack(PatV32Nmad, {
        {'p', ExtVecPred(pred)}, {'s', skip_invalid ? 1u : 0u}, {'r', (swz1 >> 10) & 3}, {'y', 0}, {'d', dext},
        {'c', (swz1 >> 9) & 1}, {'b', s1ext}, {'a', s2ext}, {'w', Vec4StdSwizzle(src2.swz)},
        {'n', 0}, {'e', dst.mask}, {'m', static_cast<uint64_t>(src1.mod)},
        {'o', src2.mod == Mod::Abs ? 1u : 0u}, {'i', (swz1 >> 7) & 3}, {'t', db}, {'k', s1b},
        {'l', s2b}, {'f', DoubledNum(dst.reg, 7)}, {'z', swz1 & 0x7f},
        {'g', static_cast<uint64_t>(op)}, {'h', DoubledNum(src1.reg, 7)},
        {'j', DoubledNum(src2.reg, 7)},
    }), 43, Reads(src1, dst.mask) | Reads(src2, dst.mask), Writes(dst, pred));
}

void Encoder::Vec16(VecOp op, Dst dst, Src src1, Src src2, Pred pred, bool nosched) {
    // An F16 pair holds all four lanes: no move to the second pair. The register mask as F16
    // lanes: a register is two.
    dst.mask = static_cast<uint8_t>(((dst.mask & 1) ? 0x3 : 0) | ((dst.mask & 2) ? 0xc : 0));
    VecWord(op, dst, src1, src2, pred, nosched);
    // Same fields, the opcode selects the f16 unit.
    uint64_t& word = words.back();
    word = (word & ~(0x1full << 59)) | (0b00010ull << 59);
    (void)PatV16Nmad;
}

void Encoder::Mad16(Dst dst, Src src0, Src src1, Src src2) {
    dst.reg = Map(dst.reg);
    src0.reg = Map(src0.reg);
    src1.reg = Map(src1.reg);
    src2.reg = Map(src2.reg);
    uint64_t s0b = 0;
    switch (src0.reg.bank) {
    case Bank::Temp:
        s0b = 0;
        break;
    case Bank::PrimAttr:
        s0b = 1;
        break;
    default:
        Fail("usse: vmad2's first source is a temporary or a primary attribute");
    }
    if (src0.mod != Mod::None || src1.mod != Mod::None || src2.mod != Mod::None) {
        Fail("usse: an F16 vmad2 with a modifier");
    }
    // The tables' common entries (Vita3K's vmad2): 0-3 a lane broadcast, 4 xyzw; source 0's
    // 7 is zwxy.
    const auto code = [](const Swizzle& s, bool first, uint64_t& out) {
        for (uint64_t k = 0; k < 4; k++) {
            const Ch c = static_cast<Ch>(k);
            if (s == Swizzle{c, c, c, c}) {
                out = k;
                return true;
            }
        }
        if (s == XYZW) {
            out = 4;
            return true;
        }
        if (first && s == Swizzle{Ch::Z, Ch::W, Ch::X, Ch::Y}) {
            out = 7;
            return true;
        }
        return false;
    };
    uint64_t sw0, sw1, sw2;
    if (!code(src0.swz, true, sw0) || !code(src1.swz, false, sw1) || !code(src2.swz, false, sw2)) {
        Fail("usse: an F16 vmad2 swizzle not in its tables");
    }
    const auto [db, dext] = DestBank(dst.reg.bank);
    if (dext) {
        Fail("usse: vmad2 destination bank");
    }
    const auto [s1b, s1ext] = Src12Bank(src1.reg.bank);
    const auto [s2b, s2ext] = Src12Bank(src2.reg.bank);
    const uint64_t mask = ((dst.mask & 1) ? 0x3 : 0) | ((dst.mask & 2) ? 0xc : 0);
    Push(Pack(PatVmad2, {
        {'d', 1}, {'p', 0}, {'s', skip_invalid ? 1u : 0u}, {'r', sw0 >> 2}, {'y', 0},
        {'c', 0}, {'b', s1ext}, {'a', s2ext}, {'w', sw2},
        {'i', sw1 >> 2}, {'n', 0}, {'e', mask}, {'m', 0}, {'o', 0}, {'k', s0b}, {'t', db},
        {'f', s1b}, {'g', s2b}, {'h', DoubledNum(dst.reg, 6)}, {'z', sw1 & 3}, {'j', sw0 & 3},
        {'l', DoubledNum(src0.reg, 6)}, {'q', DoubledNum(src1.reg, 6)}, {'u', DoubledNum(src2.reg, 6)},
    }), 43, 0, 0);
}

void Encoder::Comp16(CompOp op, Dst dst, Src src, uint8_t src_comp, bool f16_src, bool f16_dst) {
    dst.reg = Map(dst.reg);
    src.reg = Map(src.reg);
    if (dst.reg.bank == Bank::Internal || src.reg.bank == Bank::Internal) {
        Fail("usse: an F16 VECCOMPLEX on an internal register");
    }
    const auto [db, dext] = DestBank(dst.reg.bank);
    const auto [sb, sext] = Src12Bank(src.reg.bank);
    // An F16 result: the low half (mask x); an F32 one the
    // register the mask names.
    Push(Pack(PatVcomp, {
        {'p', 0}, {'s', skip_invalid ? 1u : 0u}, {'d', f16_dst ? 1u : 0u}, {'y', 0}, {'e', dext},
        {'n', 0}, {'r', sext}, {'a', 0}, {'o', 0}, {'b', static_cast<uint64_t>(op)},
        {'c', f16_src ? 1u : 0u}, {'m', static_cast<uint64_t>(src.mod)}, {'f', src_comp & 3u},
        {'t', db}, {'k', sb}, {'g', DoubledNum(dst.reg, 8)}, {'h', DoubledNum(src.reg, 8)},
        {'w', 1},
    }), 43, 0, 0);
}

void Encoder::Mad(Dst dst, Src src1, Src gpi0, Src gpi1, bool vec4, Pred pred, bool nosched) {
    dst.reg = Map(dst.reg);
    src1.reg = Map(src1.reg);

    if (gpi0.reg.bank != Bank::Internal || gpi1.reg.bank != Bank::Internal) {
        Fail("usse: mad's second and third sources are internal registers");
    }
    const auto [db, dext] = DestBank(dst.reg.bank);
    const auto [s1b, s1ext] = Src12Bank(src1.reg.bank);
    const auto pick = [vec4](const Swizzle& s) { return vec4 ? Vec4Swizzle(s) : Vec3Swizzle(s); };
    const auto [sw1, sw1ext] = pick(src1.swz);
    const auto [sw0, sw0ext] = pick(gpi0.swz);
    const auto [sw2, sw2ext] = pick(gpi1.swz);
    const auto neg = [](Mod m) { return (m == Mod::Neg || m == Mod::NegAbs) ? 1u : 0u; };
    const auto abs = [](Mod m) { return (m == Mod::Abs || m == Mod::NegAbs) ? 1u : 0u; };
    Push(Pack(PatVmad, {
        {'p', MadPred(pred)}, {'s', skip_invalid ? 1u : 0u}, {'g', sw2ext}, {'o', vec4 ? 1u : 0u}, {'d', dext},
        {'e', 0}, {'r', s1ext}, {'a', 3}, {'i', abs(gpi0.mod)}, {'t', 0},
        {'n', 0}, {'w', dst.mask}, {'c', neg(src1.mod)}, {'b', abs(src1.mod)},
        {'f', neg(gpi1.mod)}, {'h', abs(gpi1.mod)}, {'z', sw0ext}, {'k', db}, {'j', s1b},
        {'l', gpi0.reg.n}, {'m', DoubledNum(dst.reg, 7)}, {'q', sw0}, {'u', sw2},
        {'v', gpi1.reg.n}, {'x', neg(gpi0.mod)}, {'y', sw1ext}, {'A', sw1},
        {'B', DoubledNum(src1.reg, 7)},
    }), 43, Reads(src1, dst.mask) | Reads(gpi0, dst.mask) | Reads(gpi1, dst.mask),
         Writes(dst, pred));
}

bool Encoder::Mad2Swizzle(const Swizzle& s, uint64_t& code) {
    if (s[0] == Ch::X && s[1] == Ch::X) {
        code = 0;
    } else if (s[0] == Ch::Y && s[1] == Ch::Y) {
        code = 1;
    } else if (s[0] == Ch::X && s[1] == Ch::Y) {
        code = 4;
    } else {
        return false;
    }
    return true;
}

void Encoder::Mad2(Dst dst, Src src0, Src src1, Src src2, Pred pred, bool nosched) {
    dst.reg = Map(dst.reg);
    src0.reg = Map(src0.reg);
    src1.reg = Map(src1.reg);
    src2.reg = Map(src2.reg);
    if (dst.mask & ~3u) {
        Fail("usse: vmad2 writes a pair");
    }
    if (pred != Pred::None) {
        Fail("usse: vmad2 predication is unverified");
    }
    uint64_t s0b = 0;
    switch (src0.reg.bank) {
    case Bank::Temp:
        s0b = 0;
        break;
    case Bank::PrimAttr:
        s0b = 1;
        break;
    default:
        Fail("usse: vmad2's first source is a temporary or a primary attribute");
    }
    if (src0.mod == Mod::Neg || src0.mod == Mod::NegAbs || src1.mod != Mod::None ||
        src2.mod != Mod::None) {
        Fail("usse: vmad2 modifier not encodable");
    }
    uint64_t sw0, sw1, sw2;
    if (!Mad2Swizzle(src0.swz, sw0) || !Mad2Swizzle(src1.swz, sw1) || !Mad2Swizzle(src2.swz, sw2)) {
        Fail("usse: vmad2 swizzle not in its tables");
    }
    const auto [db, dext] = DestBank(dst.reg.bank);
    if (dext) {
        Fail("usse: vmad2 destination bank");
    }
    const auto [s1b, s1ext] = Src12Bank(src1.reg.bank);
    const auto [s2b, s2ext] = Src12Bank(src2.reg.bank);
    Push(Pack(PatVmad2, {
        {'d', 0}, {'p', 0}, {'s', skip_invalid ? 1u : 0u}, {'r', sw0 >> 2}, {'y', 0},
        {'c', src0.mod == Mod::Abs ? 1u : 0u}, {'b', s1ext}, {'a', s2ext}, {'w', sw2},
        {'i', sw1 >> 2}, {'n', 0}, {'e', dst.mask}, {'m', 0}, {'o', 0}, {'k', s0b}, {'t', db},
        {'f', s1b}, {'g', s2b}, {'h', DoubledNum(dst.reg, 6)}, {'z', sw1 & 3}, {'j', sw0 & 3},
        {'l', DoubledNum(src0.reg, 6)}, {'q', DoubledNum(src1.reg, 6)}, {'u', DoubledNum(src2.reg, 6)},
    }), 43, Reads(src0, dst.mask) | Reads(src1, dst.mask) | Reads(src2, dst.mask),
         Writes(dst, pred));
}

void Encoder::Dot(Dst dst, Src src1, Src gpi0, bool vec4, Pred pred, bool nosched, uint8_t clip) {
    dst.reg = Map(dst.reg);
    src1.reg = Map(src1.reg);

    if (gpi0.reg.bank != Bank::Internal) {
        Fail("usse: dot's second source is an internal register");
    }
    const auto [db, dext] = DestBank(dst.reg.bank);
    const auto [s1b, s1ext] = Src12Bank(src1.reg.bank);
    const auto [sw0, sw0ext] = vec4 ? Vec4Swizzle(gpi0.swz) : Vec3Swizzle(gpi0.swz);
    if (sw0ext) {
        Fail("usse: dot's internal source only has the standard swizzle table");
    }
    const auto neg = [](Mod m) { return (m == Mod::Neg || m == Mod::NegAbs) ? 1u : 0u; };
    const auto abs = [](Mod m) { return (m == Mod::Abs || m == Mod::NegAbs) ? 1u : 0u; };
    Push(Pack(PatVdp, {
        {'p', ExtVecPred(pred)}, {'s', skip_invalid ? 1u : 0u}, {'c', clip != 0 ? 1u : 0u},
        {'o', vec4 ? 1u : 0u}, {'d', dext},
        {'e', 0}, {'r', s1ext}, {'a', 3}, {'g', abs(gpi0.mod)}, {'t', 0},
        {'n', 0}, {'w', dst.mask}, {'b', neg(src1.mod)}, {'f', abs(src1.mod)},
        {'l', clip != 0 ? clip - 1u : 0u}, {'k', db}, {'h', s1b}, {'i', gpi0.reg.n},
        {'j', DoubledNum(dst.reg, 7)},
        {'z', sw0}, {'m', static_cast<uint64_t>(src1.swz[3])},
        {'q', static_cast<uint64_t>(src1.swz[2])}, {'y', static_cast<uint64_t>(src1.swz[1])},
        {'x', static_cast<uint64_t>(src1.swz[0])}, {'u', DoubledNum(src1.reg, 7)},
    }), 43, Reads(src1, vec4 ? 0xf : 0x7) | Reads(gpi0, vec4 ? 0xf : 0x7), Writes(dst, pred));
}

void Encoder::Comp(CompOp op, Dst dst, Src src, uint8_t src_comp, Pred pred, bool nosched) {
    dst.reg = Map(dst.reg);
    src.reg = Map(src.reg);

    const auto [db, dext] = DestBank(dst.reg.bank);
    const auto [sb, sext] = Src12Bank(src.reg.bank);
    Push(Pack(PatVcomp, {
        {'p', ExtPred(pred)}, {'s', skip_invalid ? 1u : 0u}, {'d', 0}, {'y', 0}, {'e', dext}, {'n', 0}, {'r', sext},
        {'a', 0}, {'o', 0}, {'b', static_cast<uint64_t>(op)}, {'c', 0},
        {'m', static_cast<uint64_t>(src.mod)}, {'f', src_comp}, {'t', db}, {'k', sb},
        {'g', DoubledNum(dst.reg, 8)}, {'h', DoubledNum(src.reg, 8)}, {'w', dst.mask},
    }), 43, ReadsOne(src.reg, src_comp), Writes(dst, pred));
}

namespace {
// VPCK from F32 (source format 6), or from F16 (5) with the four halves in the one pair at `lo`,
// or from U8 (0) with the four bytes in the one register at `lo`; `scale` normalises the U8 side.
// `sel` is each destination lane's source lane, two bits a lane from x up: C1SSEL, C2SSEL and
// C3SSEL in their own fields, C0SSEL split over bits 7 and 0 for an F32 source and in bits 1..0
// (the absent second source's number) for the others (sgxvec34usedefs.h).
uint64_t PackMove(uint64_t dest_fmt, Dst dst, Reg lo, Reg hi, bool skip_invalid,
                  uint64_t src_fmt = 6, uint64_t scale = 0, uint8_t sel = PackSelIdentity) {
    const auto [db, dext] = DestBank(dst.reg.bank);
    const auto [lb, lext] = Src12Bank(lo.bank);
    uint64_t hb = 0, hext = 0, hn = 0;
    if (lo.bank == Bank::Internal || src_fmt == 5 || src_fmt == 0) {
        // An internal register is a whole 128 bits; the second source is marked absent.
        hb = 2;
        hext = 1;
        hn = src_fmt == 6 ? 0 : (sel >> 1) & 1;
    } else {
        const auto [b, e] = Src12Bank(hi.bank);
        hb = b;
        hext = e;
        hn = DoubledNum(hi, 7);
    }
    return Pack(PatVpck, {
                             {'p', 0},
                             {'s', skip_invalid ? 1u : 0u},
                             {'n', 0},
                             {'u', 0},
                             {'y', 0},
                             {'d', dext},
                             {'e', 0},
                             {'r', lext},
                             {'c', hext},
                             {'a', 0},
                             {'f', src_fmt},
                             {'t', dest_fmt},
                             {'m', dst.mask},
                             {'b', db},
                             {'k', lb},
                             {'l', hb},
                             {'g', SingleNum(dst.reg, 7)},
                             {'o', static_cast<uint64_t>(sel >> 6 & 3)},
                             {'h', scale},
                             {'i', static_cast<uint64_t>(sel >> 2 & 3)},
                             {'j', static_cast<uint64_t>(sel >> 4 & 3)},
                             {'q', DoubledNum(lo, 7)},
                             {'v', src_fmt == 6 ? static_cast<uint64_t>(sel >> 1 & 1) : 0u},
                             {'w', hn},
                             {'x', static_cast<uint64_t>(sel & 1)},
                         });
}

// A U8 operand of SOP2/SOP3: (bank, ext, number). Internal and constant registers have none.
// A number's top bit selects the C10 reading under colour format control (psp2cgc's fx10 SOPs
// read sa64 for sa0): the U8 operands stay below it.
std::array<uint64_t, 3> SopOperand(Reg r) {
    if (r.bank == Bank::Internal || r.bank == Bank::Const) {
        Fail("usse: a SOP operand is a temporary, attribute or output register");
    }
    if (r.bank != Bank::Virtual && r.n >= 64) {
        Fail("usse: a SOP operand's register number has the C10 select bit");
    }
    const auto [b, e] = Src12Bank(r.bank);
    return {b, e, SingleNum(r, 7)};
}
} // Anonymous namespace

uint32_t SopCtl::Pack() const {
    return uint32_t{csel1} | uint32_t{csel2} << 3 | uint32_t{asel1} << 6 | uint32_t{asel2} << 8 |
           uint32_t{cmod1} << 10 | uint32_t{cmod2} << 11 | uint32_t{amod1} << 12 |
           uint32_t{amod2} << 13 | uint32_t{cop} << 14 | uint32_t{aop} << 16 |
           uint32_t{src1_mod} << 18 | uint32_t{asrc1_mod} << 19;
}

SopCtl SopCtl::Unpack(uint32_t w) {
    SopCtl c;
    c.csel1 = w & 7;
    c.csel2 = w >> 3 & 7;
    c.asel1 = w >> 6 & 3;
    c.asel2 = w >> 8 & 3;
    c.cmod1 = w >> 10 & 1;
    c.cmod2 = w >> 11 & 1;
    c.amod1 = w >> 12 & 1;
    c.amod2 = w >> 13 & 1;
    c.cop = w >> 14 & 3;
    c.aop = w >> 16 & 3;
    c.src1_mod = w >> 18 & 1;
    c.asrc1_mod = w >> 19 & 1;
    return c;
}

void Encoder::Sop2(Reg dst, Reg src1, Reg src2, const SopCtl& c, bool end) {
    dst = Map(dst);
    const auto [db, dext] = DestBank(dst.bank);
    if (dst.bank == Bank::Internal || dst.bank == Bank::Const) {
        Fail("usse: a SOP destination is a temporary, attribute or output register");
    }
    const auto s1 = SopOperand(Map(src1));
    const auto s2 = SopOperand(Map(src2));
    Push(Pack(PatSop2,
              {
                  {'p', 0},
                  {'c', c.cmod1},
                  {'s', skip_invalid ? 1u : 0u},
                  {'n', 0},
                  {'a', c.asel1},
                  {'d', dext},
                  {'e', end ? 1u : 0u},
                  {'r', s1[1]},
                  {'b', s2[1]},
                  {'m', c.cmod2},
                  {'o', 0},
                  {'f', c.amod1},
                  {'l', c.asel2},
                  {'g', c.csel1},
                  {'h', c.csel2},
                  {'i', c.amod2},
                  {'t', db},
                  {'k', s1[0]},
                  {'j', s2[0]},
                  {'q', SingleNum(dst, 7)},
                  {'u', c.src1_mod},
                  {'v', c.cop},
                  {'w', c.aop},
                  {'x', c.asrc1_mod},
                  {'y', 0},
                  {'z', s1[2]},
                  {'A', s2[2]},
              }),
         54, 0, 0);
}

void Encoder::Sop3(Reg dst, Reg src0, Reg src1, Reg src2, const SopCtl& c, bool end) {
    dst = Map(dst);
    src0 = Map(src0);
    const auto [db, dext] = DestBank(dst.bank);
    if (dst.bank == Bank::Internal || dst.bank == Bank::Const) {
        Fail("usse: a SOP destination is a temporary, attribute or output register");
    }
    if (src0.bank != Bank::Temp && src0.bank != Bank::PrimAttr) {
        Fail("usse: SOP3's source 0 is a temporary or a primary attribute");
    }
    const auto s1 = SopOperand(Map(src1));
    const auto s2 = SopOperand(Map(src2));
    Push(Pack(PatSop3,
              {
                  {'p', 0},
                  {'c', c.cmod1},
                  {'s', skip_invalid ? 1u : 0u},
                  {'n', 0},
                  {'o', c.cop},
                  {'d', dext},
                  {'e', end ? 1u : 0u},
                  {'r', s1[1]},
                  {'b', s2[1]},
                  {'m', c.cmod2},
                  {'a', c.amod1},
                  {'l', c.asel1},
                  {'f', 0},
                  {'g', c.aop},
                  {'h', c.csel1},
                  {'i', c.csel2},
                  {'k', src0.bank == Bank::PrimAttr ? 1u : 0u},
                  {'t', db},
                  {'j', s1[0]},
                  {'q', s2[0]},
                  {'u', SingleNum(dst, 7)},
                  {'v', SingleNum(src0, 7)},
                  {'w', s1[2]},
                  {'x', s2[2]},
              }),
         54, 0, 0);
}

void Encoder::PackU8(Reg dst, Reg lo, Reg hi, uint8_t sel) {
    dst = Map(dst);
    lo = Map(lo);
    hi = Map(hi);
    Push(PackMove(0, {dst, 0xf}, lo, hi, skip_invalid, 6, 1, sel), 54, ReadsAll(lo) | ReadsAll(hi),
         0);
}

void Encoder::PackU8F16(Reg dst, Reg src, uint8_t sel) {
    dst = Map(dst);
    src = Map(src);
    if (src.n & 1) {
        Fail("usse: four F16 lanes are read from an even register");
    }
    Push(PackMove(0, {dst, 0xf}, src, src, skip_invalid, 5, 1, sel), 54, ReadsAll(src), 0);
}

void Encoder::UnpackU8(Dst dst, Reg src, uint8_t sel) {
    dst.reg = Map(dst.reg);
    src = Map(src);
    if (src.n & 1) {
        Fail("usse: four U8 lanes are read from an even register");
    }
    if (dst.reg.bank != Bank::Internal && (dst.mask & 0xc) != 0) {
        Fail("usse: a U8 unpack into unified-store registers writes two lanes");
    }
    Push(PackMove(6, dst, src, src, skip_invalid, 0, 1, sel), 54, 0, Writes(dst, Pred::None));
}

void Encoder::UnpackF16x4(Dst dst, Reg src) {
    dst.reg = Map(dst.reg);
    src = Map(src);
    // psp2cgc's pack.f32.f16 i0.xyzw, pa0.xyzw is 40c10bbcaf998000: source format F16, the
    // second source absent as for an internal one.
    if (src.n & 1) {
        Fail("usse: four F16 lanes are read from an even register");
    }
    Push(PackMove(6, dst, src, src, skip_invalid, 5), 54, ReadsAll(src), Writes(dst, Pred::None));
}

void Encoder::MoveF32x4(Dst dst, Reg lo, Reg hi, bool nosched) {
    dst.reg = Map(dst.reg);
    lo = Map(lo);
    hi = Map(hi);

    Push(PackMove(6, dst, lo, hi, skip_invalid), 54, ReadsAll(lo) | ReadsAll(hi),
         Writes(dst, Pred::None));
}

void Encoder::PackF16(Dst dst, Reg lo, Reg hi, bool nosched, uint8_t sel) {
    dst.reg = Map(dst.reg);
    lo = Map(lo);
    hi = Map(hi);

    Push(PackMove(5, dst, lo, hi, skip_invalid, 6, 0, sel), 54, ReadsAll(lo) | ReadsAll(hi),
         Writes(dst, Pred::None));
}

void Encoder::MoveInternal(Dst dst, Src src, Pred pred, bool nosched) {
    dst.reg = Map(dst.reg);
    src.reg = Map(src.reg);

    const auto [db, dext] = DestBank(dst.reg.bank);
    const auto [sb, sext] = Src12Bank(src.reg.bank);
    Push(Pack(PatVmov, {
        {'p', ExtPred(pred)}, {'s', skip_invalid ? 1u : 0u}, {'t', 0}, {'r', 0}, {'y', 0}, {'d', dext}, {'e', 0},
        {'c', sext}, {'b', 0}, {'m', 0}, {'a', 0}, {'n', 0}, {'o', 5},
        {'i', 0}, {'w', Vec4StdSwizzle(src.swz)}, {'k', 0}, {'l', db}, {'f', sb}, {'g', 0},
        {'h', dst.mask}, {'j', DoubledNum(dst.reg, 7)}, {'q', 0},
        {'u', DoubledNum(src.reg, 7)}, {'v', 0},
    }), 43, Reads(src, dst.mask), Writes(dst, pred));
}

void Encoder::CondMove(Dst dst, Src src0, Src src1, Src src2, bool nosched) {
    dst.reg = Map(dst.reg);
    src0.reg = Map(src0.reg);
    src1.reg = Map(src1.reg);
    src2.reg = Map(src2.reg);

    // The one swizzle field selects the lanes of the moved sources 1 and 2; the tested source 0
    // keeps the identity.
    if (src0.swz != XYZW) {
        Fail("usse: a conditional move tests source 0 with the identity swizzle only");
    }
    if (src1.swz != src2.swz) {
        Fail("usse: a conditional move's two moved sources share one swizzle");
    }
    const auto [db, dext] = DestBank(dst.reg.bank);
    const auto [s0b, s0ext] = Src0Bank(src0.reg.bank);
    const auto [s1b, s1ext] = Src12Bank(src1.reg.bank);
    const auto [s2b, s2ext] = Src12Bank(src2.reg.bank);
    Push(Pack(PatVmov, {
        {'p', 0}, {'s', skip_invalid ? 1u : 0u}, {'t', 0}, {'r', 0}, {'y', 0}, {'d', dext}, {'e', s0ext},
        {'c', s1ext}, {'b', s2ext}, {'m', 1}, {'a', 0}, {'n', 0}, {'o', 5},
        {'i', 1}, {'w', Vec4StdSwizzle(src1.swz)}, {'k', s0b}, {'l', db}, {'f', s1b},
        {'g', s2b}, {'h', dst.mask}, {'j', DoubledNum(dst.reg, 7)},
        {'q', DoubledNum(src0.reg, 7)}, {'u', DoubledNum(src1.reg, 7)},
        {'v', DoubledNum(src2.reg, 7)},
    }), 43, Reads(src0, dst.mask) | Reads(src1, dst.mask) | Reads(src2, dst.mask),
         Writes(dst, Pred::None));
}

void Encoder::Test(Cond cond, uint8_t pred_n, Src src1, uint8_t chan, Src src2,
                   bool src2_scalar, Pred pred, TestAlu alu) {
    src1.reg = Map(src1.reg);
    src2.reg = Map(src2.reg);

    const auto [s1b, s1ext] = Src12Bank(src1.reg.bank);
    const auto [s2b, s2ext] = Src12Bank(src2.reg.bank);
    // The test unit subtracts src2 from src1 and tests the sign and the zero flag of the
    // difference: sign 2 = positive, 1 = negative; zero 1 = include equal, 2 = exclude;
    // and whether the two are combined with AND (1) or OR (0).
    uint64_t sign = 0, zero = 0, andc = 0;
    switch (cond) {
    case Cond::Gt: sign = 2; zero = 2; andc = 1; break;
    case Cond::Ge: sign = 2; zero = 1; andc = 0; break;
    case Cond::Lt: sign = 1; zero = 2; andc = 1; break;
    case Cond::Le: sign = 1; zero = 1; andc = 0; break;
    case Cond::Eq: sign = 0; zero = 1; andc = 1; break; // psp2cgc's cmp.eq.f32
    case Cond::Ne: sign = 0; zero = 2; andc = 1; break;
    }
    Push(Pack(PatVtst, {
        {'p', ExtPred(pred)}, {'s', skip_invalid ? 1u : 0u}, {'K', 0}, {'o', 0}, {'y', 0}, {'d', 1},
        {'r', (src1.mod == Mod::Neg) ? 1u : 0u}, {'c', s1ext}, {'e', s2ext}, {'a', 1},
        {'v', src2_scalar ? 1u : 0u}, {'t', 0}, {'i', sign}, {'z', zero}, {'m', andc},
        {'h', chan}, {'n', pred_n}, {'b', 1}, {'k', s1b}, {'f', s2b}, {'g', 0}, {'w', 0},
        {'l', 0}, {'u', alu == TestAlu::Mul ? 13u : 14u}, {'j', DoubledNum(src1.reg, 8)},
        {'q', DoubledNum(src2.reg, 8)},
    }), 54, ReadsOne(src1.reg, chan) | ReadsOne(src2.reg, src2_scalar ? 0 : chan), 0);
}

namespace {

// The dual-issue words' swizzle tables, which are not the single-issue ones: sixteen standard
// entries ending in the constants 0, 0.5, 1 and 2, and an extended set. Three-wide entries'
// fourth channel is unused.
#define SW(a, b, c, d) Swizzle{Ch::a, Ch::b, Ch::c, Ch::d}
constexpr Swizzle DualVec3Std[16] = {
    SW(X, X, X, X),          SW(Y, Y, Y, X),          SW(Z, Z, Z, X),       SW(W, W, W, X),
    SW(X, Y, Z, X),          SW(Y, Z, W, X),          SW(X, X, Y, X),       SW(X, Y, X, X),
    SW(Y, Y, X, X),          SW(Y, Y, Z, X),          SW(Z, X, Y, X),       SW(X, Z, Y, X),
    SW(Zero, Zero, Zero, X), SW(Half, Half, Half, X), SW(One, One, One, X), SW(Two, Two, Two, X),
};
constexpr Swizzle DualVec3Ext[11] = {
    SW(X, Y, Y, X),    SW(Y, X, Y, X),      SW(X, X, Z, X), SW(Y, X, X, X),
    SW(X, Y, Zero, X), SW(X, One, Zero, X), SW(X, Z, Y, X), SW(Y, Z, X, X),
    SW(Z, Y, X, X),    SW(Z, Z, Y, X),      SW(X, Y, One, X),
};
constexpr Swizzle DualVec4Std[16] = {
    SW(X, X, X, X),             SW(Y, Y, Y, Y),             SW(Z, Z, Z, Z),
    SW(W, W, W, W),             SW(X, Y, Z, W),             SW(Y, Z, W, W),
    SW(X, Y, Z, Z),             SW(X, X, Y, Z),             SW(X, Y, X, Y),
    SW(X, Y, W, Z),             SW(Z, X, Y, W),             SW(Z, W, Z, W),
    SW(Zero, Zero, Zero, Zero), SW(Half, Half, Half, Half), SW(One, One, One, One),
    SW(Two, Two, Two, Two),
};
constexpr Swizzle DualVec4Ext[16] = {
    SW(Y, Z, X, W), SW(Z, W, X, Y), SW(X, Z, W, Y), SW(Y, Y, W, W),
    SW(W, Y, Z, W), SW(W, Z, W, Z), SW(X, Y, Z, X), SW(Z, Z, W, W),
    SW(X, W, Z, X), SW(Y, Y, Y, X), SW(Y, Y, Y, Z), SW(Z, W, Z, W),
    SW(Y, Z, X, Z), SW(X, X, Y, Y), SW(X, Z, W, W), SW(X, Y, Z, One),
};
#undef SW

// A source slot of a dual-issue word: the unified store's operand, or an internal register.
enum class DualSlot : uint8_t { U, G0, G1, G2, None };
using DualSlots = std::array<DualSlot, 3>;
constexpr DualSlots DualInvalid{DualSlot::None, DualSlot::None, DualSlot::None};
#define SL(...) DualSlots{__VA_ARGS__}
#define U DualSlot::U
#define G0 DualSlot::G0
#define G1 DualSlot::G1
#define G2 DualSlot::G2
// OP1's slots by its source count and the source configuration, then OP2's by both counts.
constexpr DualSlots DualPrimary[3][4] = {
    {SL(U), SL(G0), SL(G1), SL(G2)},
    {SL(U, G1), SL(G0, U), SL(G0, G1), DualInvalid},
    {SL(U, G1, G2), SL(G0, U, G2), SL(G0, G1, U), SL(G0, G1, G2)},
};
constexpr DualSlots DualSecondary[3][3][4] = {
    {
        {SL(G0), SL(U), SL(U), SL(U)},
        {SL(G0, G1), SL(U, G1), SL(G0, U), SL(G0, U)},
        {SL(G0, G1, G2), SL(U, G1, G2), SL(G0, U, G2), SL(G0, U, G1)},
    },
    {
        {SL(G0), SL(G1), SL(U), DualInvalid},
        {SL(G0, G2), SL(G1, G2), SL(G2, U), DualInvalid},
        {DualInvalid, DualInvalid, DualInvalid, DualInvalid},
    },
    {
        {SL(G0), SL(G1), SL(G2), SL(U)},
        {DualInvalid, DualInvalid, DualInvalid, DualInvalid},
        {DualInvalid, DualInvalid, DualInvalid, DualInvalid},
    },
};
#undef SL
#undef U
#undef G0
#undef G1
#undef G2

// How an operation sits in one half of the word.
struct DualForm {
    uint8_t code = 0;     // the operation's code, standard or extended
    bool ext = false;
    bool vector = false;  // sources read lane by lane (`lanes`), else one lane of each (`lane`)
    uint8_t lanes = 0;
    uint8_t lane = 0;
    // The destination: an internal register and its mask, or the unified store's fields.
    uint8_t gpi = 0, mask = 0;
    uint8_t usd = 0, usd_bank = 0;
};

bool OneLane(uint8_t mask, uint8_t& lane) {
    if (mask == 0 || (mask & (mask - 1)) != 0) {
        return false;
    }
    lane = static_cast<uint8_t>(std::countr_zero(mask));
    return true;
}

// The form of `op` writing the internal register (`to_us` false) or the unified store.
bool DualFormOf(const DualOp& op, bool vec4, bool to_us, DualForm& f) {
    using K = DualOp::Kind;
    const uint8_t full = vec4 ? 0xf : 0x7;
    const bool lanewise = op.kind == K::Mul || op.kind == K::Add || op.kind == K::Mov ||
                          op.kind == K::Mad;
    const uint8_t mask = op.dst.mask & 0xf;
    uint8_t lane = 0;
    const bool one = OneLane(mask, lane);
    if (op.kind == K::Dot && op.vec4 != vec4) {
        return false;
    }
    // A lane-by-lane operation as its vector form, or (one unified-store register) its scalar.
    const auto vector_code = [&]() -> uint8_t {
        return op.kind == K::Mad ? 0 : op.kind == K::Mul ? 3 : op.kind == K::Add ? 4 : 5;
    };
    const auto scalar = [&]() {
        f.vector = false;
        f.lane = lane;
        switch (op.kind) {
        case K::Dot:
            f.code = 1;
            f.vector = true;
            f.lanes = full;
            return true;
        case K::Rsq:
            f.code = 6;
            return true;
        case K::Rcp:
            f.code = 7;
            return true;
        case K::Add:
            f.code = 1;
            f.ext = true;
            return true;
        case K::Mul:
            f.code = 2;
            f.ext = true;
            return true;
        case K::Frc:
            f.code = 3;
            f.ext = true;
            return true;
        case K::Exp:
            f.code = 4;
            f.ext = true;
            return true;
        case K::Log:
            f.code = 5;
            f.ext = true;
            return true;
        default:
            return false; // no scalar move
        }
    };
    if (!to_us) {
        if (op.dst.reg.bank != Bank::Internal || mask == 0 || (mask & ~full) != 0) {
            return false;
        }
        f.gpi = op.dst.reg.n;
        f.mask = mask;
        if (lanewise) {
            f.code = vector_code();
            f.vector = true;
            f.lanes = mask;
            return true;
        }
        return one && scalar();
    }
    if (op.dst.reg.bank == Bank::Internal) {
        // The unified store's destination can name an internal register (all of it). Not
        // emitted: a pair forms only where nothing internal is live past it (Lowering::Encode).
        return false;
    }
    switch (op.dst.reg.bank) {
    case Bank::Temp:
        f.usd_bank = 0;
        break;
    case Bank::Output:
        f.usd_bank = 1;
        break;
    case Bank::PrimAttr:
        f.usd_bank = 2;
        break;
    default:
        return false;
    }
    if (lanewise && mask == 0x3) {
        // A vector result: the pair's x and y, implied.
        if ((op.dst.reg.n & 1) != 0 || op.dst.reg.n / 2 >= 124) {
            return false;
        }
        f.code = vector_code();
        f.vector = true;
        f.lanes = 0x3;
        f.usd = static_cast<uint8_t>(op.dst.reg.n / 2);
        return true;
    }
    // A scalar result: the one register, its number not doubled.
    if (!one || op.dst.reg.n + lane >= 124) {
        return false;
    }
    f.usd = static_cast<uint8_t>(op.dst.reg.n + lane);
    return scalar();
}

// A source's fields in its slot.
struct DualSource {
    uint8_t num = 0, bank = 0, swizzle = 4;
    bool ext = false, neg = false, abs = false;
};

// The table entry reading `swz` in `lanes`, standard first. The extended tables are unused: on
// the console g1's extended entry 7 (yzx) read garbage, and the unified store's are unmeasured.
bool DualSwizzle(const Swizzle& swz, uint8_t lanes, bool vec4, bool allow_ext, DualSource& out) {
    const auto match = [&](const Swizzle* table, std::size_t count, bool ext) {
        for (std::size_t i = 0; i < count; i++) {
            bool same = true;
            for (unsigned l = 0; l < 4 && same; l++) {
                same = !(lanes >> l & 1) || table[i][l] == swz[l];
            }
            if (same) {
                out.swizzle = static_cast<uint8_t>(i);
                out.ext = ext;
                return true;
            }
        }
        return false;
    };
    if (vec4) {
        return match(DualVec4Std, 16, false) || (allow_ext && match(DualVec4Ext, 16, true));
    }
    return match(DualVec3Std, 16, false) || (allow_ext && match(DualVec3Ext, 11, true));
}

// Whether `src`, read as `f` reads it, fits `slot`. `few`: OP1 has under two sources (the
// unified store then has its extended swizzles and its absolute flag, and g2 is i2).
bool DualFit(const Src& src, const DualForm& f, DualSlot slot, bool vec4, bool few,
             DualSource& out) {
    out = {};
    const bool neg = src.mod == Mod::Neg || src.mod == Mod::NegAbs;
    const bool abs = src.mod == Mod::Abs || src.mod == Mod::NegAbs;
    const Ch scalar_ch = src.swz[f.lane & 3];
    if (src.reg.bank == Bank::Const) {
        // One of the tables' constants in every lane read: the constant bank's 0, 1, 2 and
        // 0.5 (word x of entries 0, 2, 4 and 12), in any slot but g2, whose swizzle is fixed.
        const uint8_t lanes = f.vector ? f.lanes : static_cast<uint8_t>(1u << (f.lane & 3));
        for (unsigned l = 0; l < 4; l++) {
            if ((lanes >> l & 1) && src.swz[l] != Ch::X) {
                return false;
            }
        }
        switch (src.reg.n) {
        case 0:
            out.swizzle = 12;
            break;
        case 12:
            out.swizzle = 13;
            break;
        case 2:
            out.swizzle = 14;
            break;
        case 4:
            out.swizzle = 15;
            break;
        default:
            return false;
        }
        out.neg = neg;
        return slot == DualSlot::G1 || slot == DualSlot::U || (slot == DualSlot::G0 && !neg);
    }
    if (slot == DualSlot::U) {
        switch (src.reg.bank) {
        case Bank::Temp:
            out.bank = 0;
            break;
        case Bank::Output:
            out.bank = 1;
            break;
        case Bank::PrimAttr:
            out.bank = 2;
            break;
        case Bank::SecAttr:
            out.bank = 3;
            break;
        default:
            return false;
        }
        if (abs && !few) {
            return false;
        }
        out.neg = neg;
        out.abs = abs;
        if (!f.vector) {
            // One register, its number not doubled: the lane's own.
            if (scalar_ch > Ch::W) {
                return false;
            }
            const unsigned n = src.reg.n + static_cast<unsigned>(scalar_ch);
            if (n > 127 || (src.reg.bank == Bank::Temp && n >= 124)) {
                return false;
            }
            out.num = static_cast<uint8_t>(n);
            out.swizzle = 0;
            return true;
        }
        if ((src.reg.n & 1) != 0 || (src.reg.bank == Bank::Temp && src.reg.n / 2 >= 124)) {
            return false;
        }
        out.num = static_cast<uint8_t>(src.reg.n / 2);
        return DualSwizzle(src.swz, f.lanes, vec4, false, out);
    }
    if (src.reg.bank != Bank::Internal || abs) {
        return false;
    }
    out.num = src.reg.n;
    out.neg = neg;
    if (slot != DualSlot::G1 && neg) {
        return false;
    }
    if (slot == DualSlot::G2) {
        if (!f.vector) {
            return scalar_ch == Ch::X;
        }
        for (unsigned l = 0; l < 4; l++) {
            if ((f.lanes >> l & 1) && src.swz[l] != static_cast<Ch>(l)) {
                return false;
            }
        }
        return true;
    }
    if (!f.vector) {
        if (scalar_ch > Ch::W) {
            return false;
        }
        out.swizzle = static_cast<uint8_t>(scalar_ch);
        return true;
    }
    return DualSwizzle(src.swz, f.lanes, vec4, false, out);
}

} // Anonymous namespace

bool Encoder::Dual(const DualOp& first, const DualOp& second) {
    using K = DualOp::Kind;
    DualOp ops[2] = {first, second};
    for (DualOp& op : ops) {
        op.dst.reg = Map(op.dst.reg);
        for (unsigned k = 0; k < op.count; k++) {
            op.src[k].reg = Map(op.src[k].reg);
        }
        if (op.count == 0 || op.count > 3) {
            return false;
        }
    }
    // The orders an operation's sources may take: a product's two swapped.
    const auto orders = [](const DualOp& op) {
        return (op.kind == K::Mul || op.kind == K::Add || op.kind == K::Dot || op.kind == K::Mad)
                   ? 2
                   : 1;
    };
    const auto source = [](const DualOp& op, int order, unsigned k) -> const Src& {
        return op.src[order == 1 && k < 2 ? 1 - k : k];
    };
    for (int primary = 0; primary < 2; primary++) {
        const DualOp& p = ops[primary];
        const DualOp& s = ops[1 - primary];
        const bool few = p.count < 2;
        for (int vec4 = 0; vec4 < 2; vec4++) {
            for (int pri_us = 0; pri_us < 2; pri_us++) {
                DualForm pf, sf;
                if (!DualFormOf(p, vec4 != 0, pri_us != 0, pf) ||
                    !DualFormOf(s, vec4 != 0, pri_us == 0, sf)) {
                    continue;
                }
                // OP1's extended table is DVEC3's alone (the bit is DVEC4's w mask); OP2 has
                // no standard code 0.
                if ((pf.ext && vec4 != 0) || (!sf.ext && sf.code == 0)) {
                    continue;
                }
                for (unsigned cfg = 0; cfg < 4; cfg++) {
                    const DualSlots& ps = DualPrimary[p.count - 1][cfg];
                    const DualSlots& ss = DualSecondary[p.count - 1][s.count - 1][cfg];
                    if (ps[0] == DualSlot::None || ss[0] == DualSlot::None) {
                        continue;
                    }
                    for (int po = 0; po < orders(p); po++) {
                        for (int so = 0; so < orders(s); so++) {
                            std::array<DualSource, 4> slot{}; // by DualSlot
                            bool ok = true;
                            uint16_t reads = 0;
                            const auto place = [&](const DualOp& op, const DualForm& f,
                                                   const DualSlots& slots, int order) {
                                for (unsigned k = 0; k < op.count && ok; k++) {
                                    const Src& src = source(op, order, k);
                                    DualSource fit;
                                    ok = DualFit(src, f, slots[k], vec4 != 0, few, fit);
                                    if (!ok) {
                                        break;
                                    }
                                    if (slots[k] == DualSlot::G2 && few && fit.num != 2 &&
                                        src.reg.bank == Bank::Internal) {
                                        ok = false; // g2 is i2 when OP1 has one source
                                        break;
                                    }
                                    slot[static_cast<std::size_t>(slots[k])] = fit;
                                    reads |= f.vector
                                                 ? Reads(src, f.lanes)
                                                 : ReadsOne(src.reg, static_cast<uint8_t>(
                                                                         src.swz[f.lane & 3]));
                                }
                            };
                            place(p, pf, ps, po);
                            place(s, sf, ss, so);
                            if (!ok) {
                                continue;
                            }
                            const DualForm& gf = pri_us != 0 ? sf : pf; // the internal write
                            const DualForm& uf = pri_us != 0 ? pf : sf;
                            const DualSource& u = slot[0];
                            const DualSource& g0 = slot[1];
                            const DualSource& g1 = slot[2];
                            const DualSource& g2 = slot[3];
                            const uint64_t g2_field =
                                few ? (u.ext ? 2u : 0u) | (u.abs ? 1u : 0u) : g2.num;
                            if (!few && (u.ext || u.abs)) {
                                continue;
                            }
                            const uint64_t hi =
                                (vec4 != 0 ? 5ull : 4ull) << 27 | (g1.neg ? 1ull : 0ull) << 26 |
                                (skip_invalid ? 1ull : 0ull) << 23 |
                                (vec4 != 0 ? (gf.mask >> 3 & 1ull) : (pf.ext ? 1ull : 0ull)) << 22 |
                                (g1.ext ? 1ull : 0ull) << 20 | uint64_t{u.swizzle} << 16 |
                                (u.neg ? 1ull : 0ull) << 15 | uint64_t{pf.code} << 12 |
                                (sf.ext ? 1ull : 0ull) << 11 | (pri_us != 0 ? 1ull : 0ull) << 10 |
                                uint64_t{g0.swizzle} << 6 | uint64_t{g1.swizzle} << 2 |
                                uint64_t{uf.usd_bank};
                            const uint64_t lo =
                                uint64_t{u.bank} << 30 | uint64_t{gf.gpi} << 28 |
                                uint64_t{uf.usd} << 21 | uint64_t{sf.code} << 18 |
                                uint64_t{cfg} << 16 | g2_field << 14 | uint64_t{g1.num} << 12 |
                                uint64_t{g0.num} << 10 | (gf.mask & 7ull) << 7 | uint64_t{u.num};
                            uint16_t writes = static_cast<uint16_t>(gf.mask << (gf.gpi * 4));
                            if (uf.usd_bank == 0 && uf.usd >= 124 && uf.vector) {
                                writes |= static_cast<uint16_t>(uf.lanes << ((uf.usd - 124) * 4));
                            }
                            Push(hi << 32 | lo, 0, reads, writes);
                            return true;
                        }
                    }
                }
            }
        }
    }
    return false;
}

void Encoder::Sample2D(Reg dst, Reg coord, uint8_t sampler_sa, const Reg* lod, uint8_t drc,
                       bool nosched, uint8_t fconv, bool point) {
    Sample(dst, coord, 2, sampler_sa, lod, drc, fconv, point);
}

void Encoder::SampleCube(Reg dst, Reg coord, uint8_t sampler_sa, uint8_t drc, bool nosched) {
    Sample(dst, coord, 3, sampler_sa, nullptr, drc);
}

void Encoder::Sample(Reg dst, Reg coord, uint8_t dims, uint8_t sampler_sa, const Reg* lod,
                     uint8_t drc, uint8_t fconv, bool point) {
    const auto [cb, cext] = Src0Bank(coord.bank);
    if (dst.bank != Bank::Temp && dst.bank != Bank::PrimAttr) {
        Fail("usse: a texture read lands in a temporary or a primary attribute");
    }
    // The four F32 result words start at a 64-bit boundary (an F16 or F32 destination is aligned
    // to 64 bits on the SGX543); the field itself is not doubled.
    if (dst.n & 1) {
        Fail("usse: a texture read's destination is pair-aligned");
    }
    // The state source is an 8-bit doubled field, so the control words start at an even SA.
    if (sampler_sa & 1) {
        Fail("usse: a texture's control words start at an even secondary attribute");
    }
    Push(Pack(PatSmp,
              {
                  {'p', 0},
                  {'s', lod || (point && skip_invalid) ? 1u : 0u},
                  {'n', 0},
                  {'y', 0},
                  {'m', 0},
                  {'r', cext},
                  {'c', 0},
                  {'e', lod ? 0u : 1u},
                  {'f', fconv},
                  {'a', 0},
                  {'d', static_cast<uint64_t>(dims - 1)},
                  {'l', lod ? 2u : 0u},
                  {'t', dst.bank == Bank::PrimAttr ? 1u : 0u},
                  {'b', 0},
                  {'g', 0},
                  {'k', cb},
                  {'h', drc},
                  {'i', 3},
                  {'j', lod ? 0u : 2u},
                  {'o', dst.n},
                  {'q', DoubledNum(coord, 8)},
                  {'u', static_cast<uint64_t>(sampler_sa >> 1)},
                  {'v', lod ? DoubledNum(*lod, 8) : 0u},
              }),
         54, static_cast<uint16_t>(ReadsAll(coord) | (lod ? ReadsAll(*lod) : 0)), 0);
}

} // namespace SgxdRenderer::Usse
