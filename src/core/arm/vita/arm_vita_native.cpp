// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "core/arm/vita/arm_vita_native.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <iterator>
#include <optional>
#include <string_view>
#include <utility>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>
#include "common/assert.h"
#include "common/host_shared_memory.h"
#include "common/logging/log.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/thread.h"
#include "core/hle/kernel/vm_manager.h"
#include "core/memory.h"
#include "video_core/gpu.h"
#include "video_core/renderer_sgxd/sgxd_flags.h"
#include "video_core/renderer_sgxd/vprof.h"

namespace Core {

namespace {

#ifdef VITA_DIAGNOSTICS
VitaNativeCensus census;
std::unordered_map<u32, std::pair<u32, u32>> hold_page_accesses;
#endif

/// Never hand the guest a slice longer than this, so a runaway guest cannot stall the frontend.
constexpr u64 MaxQuantumNs = 16'000'000;
/// Nor one so short that the timer costs more than the work it bounds.
constexpr u64 MinQuantumNs = 250'000;

u64 TicksToNanoseconds(s64 ticks) {
    if (ticks <= 0) {
        return 0;
    }
    return static_cast<u64>(ticks) * 1'000'000'000ull / BASE_CLOCK_RATE_ARM11;
}

/// Every guest page is mapped read, write and execute. The emulated process's own permissions
/// are not reported through the mapping observer; W^X per section is a later refinement.
constexpr u32 GuestPerm = AZAHAR_PERM_R | AZAHAR_PERM_W | AZAHAR_PERM_X;

/// The core the module takes. The emulation thread must not be pinned there, or the detach
/// leaves it with nowhere to run: it moves to core 0, where the rasterizer's first worker lives
/// and where it mostly waits inside azaharRun anyway.
constexpr int TakenCore = 2;

const char* ReasonName(u32 reason) {
    switch (reason) {
    case AZAHAR_EXIT_SVC:
        return "svc";
    case AZAHAR_EXIT_UND:
        return "undefined instruction";
    case AZAHAR_EXIT_PABT:
        return "prefetch abort";
    case AZAHAR_EXIT_DABT:
        return "data abort";
    case AZAHAR_EXIT_TIMER:
        return "preemption";
    default:
        return "unknown exit";
    }
}

// ---------------------------------------------------------------------------- VFP short vectors
// The A9 has no VFPv2 short vectors and traps every VFP data-processing instruction while FPSCR's
// LEN or STRIDE is set: those run here, element by element, as scalars under the guest's FPSCR.

constexpr u32 FpscrVectorBits = 0x00370000; // STRIDE (21:20) and LEN (18:16)
/// The most VFP instructions one undefined-instruction exit runs.
constexpr u32 MaxVfpRun = 64;

enum class VfpOp { Mla, Mls, Nmls, Nmla, Mul, Nmul, Add, Sub, Div, Cpy, Abs, Neg, Sqrt };

u32 ReadFpscr() {
    u32 v;
    asm volatile("vmrs %0, fpscr" : "=r"(v));
    return v;
}

void WriteFpscr(u32 v) {
    asm volatile("vmsr fpscr, %0" ::"r"(v) : "memory");
}

float VfpSingle(VfpOp op, float d, float n, float m) {
    switch (op) {
    case VfpOp::Mla:
        asm volatile("vmla.f32 %0, %1, %2" : "+t"(d) : "t"(n), "t"(m));
        break;
    case VfpOp::Mls:
        asm volatile("vmls.f32 %0, %1, %2" : "+t"(d) : "t"(n), "t"(m));
        break;
    case VfpOp::Nmls:
        asm volatile("vnmls.f32 %0, %1, %2" : "+t"(d) : "t"(n), "t"(m));
        break;
    case VfpOp::Nmla:
        asm volatile("vnmla.f32 %0, %1, %2" : "+t"(d) : "t"(n), "t"(m));
        break;
    case VfpOp::Mul:
        asm volatile("vmul.f32 %0, %1, %2" : "=t"(d) : "t"(n), "t"(m));
        break;
    case VfpOp::Nmul:
        asm volatile("vnmul.f32 %0, %1, %2" : "=t"(d) : "t"(n), "t"(m));
        break;
    case VfpOp::Add:
        asm volatile("vadd.f32 %0, %1, %2" : "=t"(d) : "t"(n), "t"(m));
        break;
    case VfpOp::Sub:
        asm volatile("vsub.f32 %0, %1, %2" : "=t"(d) : "t"(n), "t"(m));
        break;
    case VfpOp::Div:
        asm volatile("vdiv.f32 %0, %1, %2" : "=t"(d) : "t"(n), "t"(m));
        break;
    case VfpOp::Cpy:
        asm volatile("vmov.f32 %0, %1" : "=t"(d) : "t"(m));
        break;
    case VfpOp::Abs:
        asm volatile("vabs.f32 %0, %1" : "=t"(d) : "t"(m));
        break;
    case VfpOp::Neg:
        asm volatile("vneg.f32 %0, %1" : "=t"(d) : "t"(m));
        break;
    case VfpOp::Sqrt:
        asm volatile("vsqrt.f32 %0, %1" : "=t"(d) : "t"(m));
        break;
    }
    return d;
}

double VfpDouble(VfpOp op, double d, double n, double m) {
    switch (op) {
    case VfpOp::Mla:
        asm volatile("vmla.f64 %P0, %P1, %P2" : "+w"(d) : "w"(n), "w"(m));
        break;
    case VfpOp::Mls:
        asm volatile("vmls.f64 %P0, %P1, %P2" : "+w"(d) : "w"(n), "w"(m));
        break;
    case VfpOp::Nmls:
        asm volatile("vnmls.f64 %P0, %P1, %P2" : "+w"(d) : "w"(n), "w"(m));
        break;
    case VfpOp::Nmla:
        asm volatile("vnmla.f64 %P0, %P1, %P2" : "+w"(d) : "w"(n), "w"(m));
        break;
    case VfpOp::Mul:
        asm volatile("vmul.f64 %P0, %P1, %P2" : "=w"(d) : "w"(n), "w"(m));
        break;
    case VfpOp::Nmul:
        asm volatile("vnmul.f64 %P0, %P1, %P2" : "=w"(d) : "w"(n), "w"(m));
        break;
    case VfpOp::Add:
        asm volatile("vadd.f64 %P0, %P1, %P2" : "=w"(d) : "w"(n), "w"(m));
        break;
    case VfpOp::Sub:
        asm volatile("vsub.f64 %P0, %P1, %P2" : "=w"(d) : "w"(n), "w"(m));
        break;
    case VfpOp::Div:
        asm volatile("vdiv.f64 %P0, %P1, %P2" : "=w"(d) : "w"(n), "w"(m));
        break;
    case VfpOp::Cpy:
        asm volatile("vmov.f64 %P0, %P1" : "=w"(d) : "w"(m));
        break;
    case VfpOp::Abs:
        asm volatile("vabs.f64 %P0, %P1" : "=w"(d) : "w"(m));
        break;
    case VfpOp::Neg:
        asm volatile("vneg.f64 %P0, %P1" : "=w"(d) : "w"(m));
        break;
    case VfpOp::Sqrt:
        asm volatile("vsqrt.f64 %P0, %P1" : "=w"(d) : "w"(m));
        break;
    }
    return d;
}

/// Whether the condition `cond` (an instruction's top four bits) holds for the flags in `cpsr`.
bool ConditionPasses(u32 cond, u32 cpsr) {
    const bool n = cpsr >> 31 & 1, z = cpsr >> 30 & 1, c = cpsr >> 29 & 1, v = cpsr >> 28 & 1;
    bool pass = true;
    switch (cond >> 1) {
    case 0:
        pass = z;
        break;
    case 1:
        pass = c;
        break;
    case 2:
        pass = n;
        break;
    case 3:
        pass = v;
        break;
    case 4:
        pass = c && !z;
        break;
    case 5:
        pass = n == v;
        break;
    case 6:
        pass = !z && n == v;
        break;
    default:
        return true; // AL
    }
    return (cond & 1) ? !pass : pass;
}

/// An ARM VFP data-processing instruction (cond 1110 .... 101x ...0, cond not 0xF).
bool IsVfpDataProcessing(u32 insn) {
    return (insn >> 28) != 0xF && (insn & 0x0F000E10) == 0x0E000A00;
}

struct VfpDecoded {
    VfpOp op;
    bool dp, two;
    u32 d, n, m;
};

/// The VFPv2 data-processing forms; false for the ones not run here (fused, immediate, compare,
/// convert, registers past d15).
bool DecodeVfp(u32 insn, VfpDecoded& out) {
    if ((insn & 0x0F000E10) != 0x0E000A00) {
        return false;
    }
    const bool dp = insn >> 8 & 1;
    const u32 pqr = (insn >> 21 & 4) | (insn >> 20 & 3);
    const bool op6 = insn >> 6 & 1;
    const u32 vd = insn >> 12 & 15, vn = insn >> 16 & 15, vm = insn & 15;
    const u32 bd = insn >> 22 & 1, bn = insn >> 7 & 1, bm = insn >> 5 & 1;
    VfpOp op;
    bool two = false;
    switch (pqr) {
    case 0:
        op = op6 ? VfpOp::Mls : VfpOp::Mla;
        break;
    case 1:
        op = op6 ? VfpOp::Nmla : VfpOp::Nmls;
        break;
    case 2:
        op = op6 ? VfpOp::Nmul : VfpOp::Mul;
        break;
    case 3:
        op = op6 ? VfpOp::Sub : VfpOp::Add;
        break;
    case 4:
        if (op6) {
            return false;
        }
        op = VfpOp::Div;
        break;
    case 7: {
        const u32 opc3 = insn >> 6 & 3;
        two = true;
        if (vn == 0 && opc3 == 1) {
            op = VfpOp::Cpy;
        } else if (vn == 0 && opc3 == 3) {
            op = VfpOp::Abs;
        } else if (vn == 1 && opc3 == 1) {
            op = VfpOp::Neg;
        } else if (vn == 1 && opc3 == 3) {
            op = VfpOp::Sqrt;
        } else {
            return false;
        }
        break;
    }
    default:
        return false;
    }
    // Singles s0-s31 in banks of eight, doubles d0-d15 in banks of four (VFPv2's sixteen).
    if (dp && (bd || bm || (!two && bn))) {
        return false;
    }
    out = {op, dp, two, dp ? vd : vd << 1 | bd, dp ? vn : vn << 1 | bn, dp ? vm : vm << 1 | bm};
    return true;
}

/// The decoded instruction as azaharVfpPatch takes it (VfpOp's order is AZAHAR_VFP_OP_*'s).
u32 VfpForm(const VfpDecoded& v) {
    return static_cast<u32>(v.op) | (v.dp ? 1u << 4 : 0u) | (v.two ? 1u << 5 : 0u) | v.d << 8 |
           v.n << 16 | v.m << 24;
}
static_assert(static_cast<u32>(VfpOp::Sqrt) == AZAHAR_VFP_OP_SQRT &&
              static_cast<u32>(VfpOp::Mul) == AZAHAR_VFP_OP_MUL);

/// Runs a VFP instruction under the guest's LEN and STRIDE; false for one DecodeVfp refuses.
bool RunVfpVector(AzaharContext& ctx, u32 insn) {
    VfpDecoded v;
    if (!DecodeVfp(insn, v)) {
        return false;
    }
    const VfpOp op = v.op;
    const bool dp = v.dp, two = v.two;
    const u32 d = v.d, n = v.n, m = v.m;
    const u32 bank = dp ? 4 : 8;
    const u32 stride_field = ctx.fpscr >> 20 & 3;
    if (stride_field != 0 && stride_field != 3) {
        return false; // UNPREDICTABLE strides
    }
    const u32 stride = stride_field == 3 ? 2 : 1;
    // A destination in the first bank is a scalar whatever LEN says; a second operand there
    // is the same scalar for every element (VFPv2's mixed vectors).
    const u32 len = d < bank ? 1 : (ctx.fpscr >> 16 & 7) + 1;
    // More elements than the bank holds at the stride is UNPREDICTABLE.
    ASSERT_MSG(len * stride <= bank, "VFP vector {:08X}: LEN {} at stride {} over a bank of {}",
               insn, len, stride, bank);
    const bool m_scalar = m < bank;
    const auto next = [&](u32 r) { return r / bank * bank + (r % bank + stride) % bank; };
    const u32 host = ReadFpscr();
    WriteFpscr(ctx.fpscr & ~FpscrVectorBits);
    u32 di = d, ni = n, mi = m;
    for (u32 e = 0; e < len; e++) {
        if (dp) {
            const auto get = [&](u32 r) {
                return std::bit_cast<double>(u64{ctx.fpregs[r * 2]} | u64{ctx.fpregs[r * 2 + 1]}
                                                                          << 32);
            };
            const u64 r = std::bit_cast<u64>(VfpDouble(op, get(di), two ? 0.0 : get(ni), get(mi)));
            ctx.fpregs[di * 2] = static_cast<u32>(r);
            ctx.fpregs[di * 2 + 1] = static_cast<u32>(r >> 32);
        } else {
            const auto get = [&](u32 r) { return std::bit_cast<float>(ctx.fpregs[r]); };
            ctx.fpregs[di] =
                std::bit_cast<u32>(VfpSingle(op, get(di), two ? 0.0f : get(ni), get(mi)));
        }
        di = next(di);
        ni = next(ni);
        if (!m_scalar) {
            mi = next(mi);
        }
    }
    const u32 flags = ReadFpscr();
    WriteFpscr(host);
    ctx.fpscr = (ctx.fpscr & FpscrVectorBits) | (flags & ~FpscrVectorBits);
    return true;
}

// ---------------------------------------------------------------------------- VFP thunks
// A trapping VFP site becomes a branch (ARM B<cond>, Thumb B.W) to a guest-code thunk:
//   push {r0-r2}; mrs r1, apsr; vmrs r2, fpscr; and r2, r2, #LEN|STRIDE; cmp r2, #vector; bne miss
//   LEN and STRIDE cleared; the elements as scalars; FPSCR and APSR back; pop {r0-r2}; b site+4
// miss: msr apsr_nzcvq, r1; pop {r0-r2}; the original instruction; b site+4

/// The thunk area, below PROCESS_IMAGE_VADDR: ARM sites below ~0x020C0000 reach it, Thumb sites
/// below ~0x010C0000; farther ones take the svc.
constexpr VAddr VfpAreaBase = 0x000C0000;
constexpr u32 VfpAreaSize = 0x40000;
/// The largest thunk: ARM 19 words and eight elements, Thumb about as many bytes.
constexpr u32 VfpThunkMax = 128;

bool InVfpArea(u32 addr) {
    return addr - VfpAreaBase < VfpAreaSize;
}

/// A patched site: the original instruction (Thumb's halfwords high first, as fetched).
struct VfpSite {
    u32 insn;
    bool thumb;
};

/// A thunk's instructions by address, for a thread stopped inside it (FinishVfpThunk).
struct VfpThunk {
    u32 site;
    bool thumb;
    u32 mrs;      ///< mrs r1: past it r1 holds the guest's flags
    u32 elements; ///< the first element: from here LEN and STRIDE are cleared
    u32 restore;  ///< the vmrs after the last element
    u32 back;     ///< the hit's branch back
    u32 miss;     ///< the miss's msr
    u32 miss_original;
    u32 miss_back;
    u32 end;
};

/// The guest's patched sites and thunks (one guest, whichever core runs it), the area, and the
/// map it is mapped into.
std::unordered_map<u32, VfpSite> vfp_patched;
std::map<u32, VfpThunk> vfp_thunks;
std::unique_ptr<Common::HostSharedMemory> vfp_area;
const VitaGuestMap* vfp_area_map{};
u32 vfp_area_used{};

/// The guest that map held is gone, and its patches with it.
void ForgetVfpPatches(const VitaGuestMap* map) {
    if (vfp_area_map == map) {
        vfp_patched.clear();
        vfp_thunks.clear();
        vfp_area_used = 0;
        vfp_area_map = nullptr;
    }
}

/// ARM B<cond> at `from` to `to`, if in reach.
std::optional<u32> ArmBranch(u32 cond, u32 from, u32 to) {
    const s32 off = static_cast<s32>(to - (from + 8));
    if ((off & 3) != 0 || off < -(1 << 25) || off >= (1 << 25)) {
        return std::nullopt;
    }
    return cond << 28 | 0x0A000000u | ((static_cast<u32>(off) >> 2) & 0x00FFFFFFu);
}

/// Thumb-2 B.W (T4) at `from` to `to`, its halfwords, if in reach.
std::optional<std::array<u16, 2>> ThumbBranchW(u32 from, u32 to) {
    const s32 off = static_cast<s32>(to - (from + 4));
    if ((off & 1) != 0 || off < -(1 << 24) || off >= (1 << 24)) {
        return std::nullopt;
    }
    const u32 u = static_cast<u32>(off);
    const u32 sign = u >> 31 & 1, i1 = u >> 23 & 1, i2 = u >> 22 & 1;
    const u32 j1 = ~(i1 ^ sign) & 1, j2 = ~(i2 ^ sign) & 1;
    return std::array<u16, 2>{static_cast<u16>(0xF000u | sign << 10 | (u >> 12 & 0x3FFu)),
                              static_cast<u16>(0x9000u | j1 << 13 | j2 << 11 | (u >> 1 & 0x7FFu))};
}

/// Thumb-2's modified immediate (i:imm3:imm8) for v, if it has one.
std::optional<u32> ThumbModifiedImm(u32 v) {
    if (v < 256) {
        return v;
    }
    for (u32 rot = 8; rot < 32; rot++) {
        const u32 x = v << rot | v >> (32 - rot); // v is x rotated right by rot
        if (x < 256 && (x & 0x80) != 0) {
            return rot << 7 | (x & 0x7F);
        }
    }
    return std::nullopt;
}

/// Thumb-2 data processing with a modified immediate: hw1 holds the op and Rn, hw2 Rd.
std::array<u16, 2> ThumbImmOp(u16 hw1, u32 rd, u32 imm) {
    const auto enc = ThumbModifiedImm(imm);
    ASSERT_MSG(enc.has_value(), "Thumb-2 immediate {:#x}", imm);
    return {static_cast<u16>(hw1 | (*enc >> 11 & 1) << 10),
            static_cast<u16>((*enc >> 8 & 7) << 12 | rd << 8 | (*enc & 0xFF))};
}

/// A thread stopped inside a thunk leaves it as the thunk would have or as if it had not begun:
/// before the elements it stands at the site again, past the first it runs the rest here.
void FinishVfpThunk(Memory::MemorySystem& memory, AzaharContext& c) {
    const u32 pc = c.r[15];
    auto it = vfp_thunks.upper_bound(pc);
    ASSERT_MSG(it != vfp_thunks.begin(), "a thread at {:08X} in no VFP thunk", pc);
    const VfpThunk& t = (--it)->second;
    ASSERT_MSG(pc < t.end, "a thread at {:08X} past its VFP thunk", pc);
    const auto pop = [&] {
        c.r[0] = memory.Read32(c.r[13]);
        c.r[1] = memory.Read32(c.r[13] + 4);
        c.r[2] = memory.Read32(c.r[13] + 8);
        c.r[13] += 12;
    };
    const auto flags = [&] { c.cpsr = (c.cpsr & ~0xF8000000u) | (c.r[1] & 0xF8000000u); };
    const u32 base = it->first;
    if (pc == base) {
        c.r[15] = t.site;
        return;
    }
    if (pc < t.elements) {
        if (pc > t.mrs) {
            flags();
        }
        pop();
        c.r[15] = t.site;
        return;
    }
    if (pc < t.back) {
        for (u32 a = std::max(pc, t.elements); a < t.restore; a += 4) {
            const u8* const p = vfp_area->Data() + (a - VfpAreaBase);
            u32 e;
            if (t.thumb) {
                u16 hw[2];
                std::memcpy(hw, p, 4);
                e = u32{hw[0]} << 16 | hw[1];
            } else {
                std::memcpy(&e, p, 4);
            }
            [[maybe_unused]] const bool ran = RunVfpVector(c, e);
            DEBUG_ASSERT_MSG(ran, "a VFP thunk's element {:08X} at {:08X}", e, a);
        }
        c.fpscr |= c.r[2] & FpscrVectorBits;
        flags();
        pop();
        c.r[15] = t.site + 4;
        return;
    }
    if (pc == t.back || pc == t.miss_back) {
        c.r[15] = t.site + 4;
        return;
    }
    if (pc < t.miss_original) {
        flags();
        pop();
    }
    c.r[15] = t.site; // the miss path before its original ran
}

} // Anonymous namespace

// ---------------------------------------------------------------------------- VitaGuestMap

VitaGuestMap::VitaGuestMap(Memory::MemorySystem& memory_) : memory{memory_} {}

VitaGuestMap::~VitaGuestMap() {
    if (installed) {
        azaharRelease();
    }
    ForgetVfpPatches(this);
}

bool VitaGuestMap::Edit(u32 op, u32 user_va, u32 guest_va, u32 size, u32 perm) const {
    AzaharEditRequest req{};
    req.op = op;
    req.range = AzaharRange{user_va, guest_va, size, perm};
    const int ret = azaharEdit(&req);
    if (ret < 0) {
        LOG_ERROR(Core_ARM11, "azaharEdit op {} guest {:#010x}+{:#x} failed: {}", op, guest_va, size,
                  ret);
        return false;
    }
    return true;
}

u32 VitaGuestMap::MapPerm(const u8* host) {
    const u8* vram = memory.GetPhysicalPointer(Memory::VRAM_PADDR);
    const bool in_vram = vram != nullptr && host >= vram && host < vram + Memory::VRAM_SIZE;
    return in_vram ? GuestPerm | AZAHAR_PERM_APP_ATTR : GuestPerm;
}

void VitaGuestMap::OnMapped(VAddr base, u32 size, MemoryRef target) {
    if (base + size > GuestEnd || base + size < base) {
        return;
    }
    DEBUG_ASSERT_MSG(extra.size == 0 || base + size <= extra_base ||
                         base >= extra_base + extra.size,
                     "a guest mapping {:#010x}+{:#x} over the VFP thunk area", base, size);
    u8* host = target.GetPtr();
    if (host == nullptr) {
        OnUnmapped(base, size);
        return;
    }
    if ((reinterpret_cast<uintptr_t>(host) & 0xFFFu) != 0) {
        // The module maps whole pages; backing that does not start on one cannot be described.
        LOG_WARNING(Core_ARM11, "Guest {:#010x}+{:#x}: backing {} is not page aligned", base, size,
                    fmt::ptr(host));
        OnUnmapped(base, size);
        return;
    }

    const Mapping wanted{size, host};
    const auto existing = live_mappings.find(base);
    if (existing != live_mappings.end() && existing->second == wanted) {
        return;
    }

    // Whatever was there is replaced, on the guest side too when the table is live.
    ForgetRange(base, size, installed);
    live_mappings.emplace(base, wanted);
    if (installed) {
        Edit(AZAHAR_EDIT_MAP, reinterpret_cast<u32>(host), base, size, MapPerm(host));
        // The window is mapped open: the GPU's page holds cover it too.
        memory.HostRemapped(host, size);
    }
}

const u8* VitaGuestMap::HostAt(VAddr addr) const {
    if (extra.size != 0 && addr - extra_base < extra.size) {
        return extra.host + (addr - extra_base);
    }
    auto it = live_mappings.upper_bound(addr);
    if (it == live_mappings.begin()) {
        return nullptr;
    }
    --it;
    if (addr - it->first >= it->second.size) {
        return nullptr;
    }
    return it->second.host + (addr - it->first);
}

std::string VitaGuestMap::Describe(VAddr addr) const {
    auto it = live_mappings.upper_bound(addr);
    if (it == live_mappings.begin()) {
        return "no window";
    }
    --it;
    if (addr - it->first >= it->second.size) {
        return "no window";
    }
    return fmt::format("window {:08X}+{:X} host {}", it->first, it->second.size,
                       fmt::ptr(it->second.host + (addr - it->first)));
}

void VitaGuestMap::OnUnmapped(VAddr base, u32 size) {
    if (base + size > GuestEnd || base + size < base) {
        return;
    }
    ForgetRange(base, size, installed);
}

void VitaGuestMap::ForgetRange(VAddr base, u32 size, bool unmap) {
    if (size == 0) {
        return;
    }
    const VAddr end = base + size;

    auto it = live_mappings.lower_bound(base);
    if (it != live_mappings.begin()) {
        const auto prev = std::prev(it);
        if (prev->first + prev->second.size > base) {
            it = prev;
        }
    }

    while (it != live_mappings.end() && it->first < end) {
        const VAddr entry_base = it->first;
        const Mapping entry = it->second;
        const VAddr entry_end = entry_base + entry.size;
        const VAddr overlap_base = std::max(entry_base, base);
        const VAddr overlap_end = std::min(entry_end, end);

        it = live_mappings.erase(it);

        if (unmap) {
            Edit(AZAHAR_EDIT_UNMAP, 0, overlap_base, overlap_end - overlap_base, 0);
        }

        if (entry_base < overlap_base) {
            live_mappings.emplace(entry_base, Mapping{overlap_base - entry_base, entry.host});
        }
        if (overlap_end < entry_end) {
            live_mappings.emplace(overlap_end, Mapping{entry_end - overlap_end,
                                                       entry.host + (overlap_end - entry_base)});
        }
    }
}

void VitaGuestMap::OnPageTableChanged() {
    mirrored = nullptr;
    if (installed) {
        for (const auto& [base, mapping] : live_mappings) {
            Edit(AZAHAR_EDIT_UNMAP, 0, base, mapping.size, 0);
        }
    }
    live_mappings.clear();
}

bool VitaGuestMap::Install() {
    if (installed) {
        return true;
    }
    AzaharMapRequest req{};
    req.count = 0;
    const int ret = azaharMap(&req);
    if (ret < 0) {
        LOG_ERROR(Core_ARM11, "azaharMap failed: {}", ret);
        return false;
    }
    installed = true;
    unsigned added = 0;
    for (const auto& [base, mapping] : live_mappings) {
        if (Edit(AZAHAR_EDIT_MAP, reinterpret_cast<u32>(mapping.host), base, mapping.size,
                 MapPerm(mapping.host))) {
            added++;
        }
    }
    if (extra.size != 0) {
        const bool mapped = Edit(AZAHAR_EDIT_MAP, reinterpret_cast<u32>(extra.host), extra_base,
                                 extra.size, AZAHAR_PERM_R | AZAHAR_PERM_X);
        ASSERT_MSG(mapped, "the VFP thunk area {:#010x}+{:#x} was not mapped", extra_base,
                   extra.size);
    }
    LOG_INFO(Core_ARM11, "Guest table installed on the taken core with {} range(s)", added);
    return true;
}

void VitaGuestMap::SyncCode(VAddr base, u32 size) {
    if (!installed || size == 0) {
        return;
    }
    const VAddr start = base & ~0xFFFu;
    const VAddr end = (base + size + 0xFFFu) & ~0xFFFu;
    auto it = live_mappings.lower_bound(start);
    if (it != live_mappings.begin()) {
        const auto prev = std::prev(it);
        if (prev->first + prev->second.size > start) {
            it = prev;
        }
    }
    for (; it != live_mappings.end() && it->first < end; ++it) {
        const VAddr lo = std::max(it->first, start);
        const VAddr hi = std::min(it->first + it->second.size, end);
        if (lo < hi) {
            Edit(AZAHAR_EDIT_SYNC_CODE, reinterpret_cast<u32>(it->second.host + (lo - it->first)),
                 lo, hi - lo, 0);
        }
    }
    if (extra.size != 0) {
        const VAddr lo = std::max(extra_base, start);
        const VAddr hi = std::min(extra_base + extra.size, end);
        if (lo < hi) {
            Edit(AZAHAR_EDIT_SYNC_CODE, reinterpret_cast<u32>(extra.host + (lo - extra_base)), lo,
                 hi - lo, 0);
        }
    }
}

void VitaGuestMap::MapExtra(VAddr base, u32 size, u8* host) {
    ASSERT_MSG(extra.size == 0 && size != 0 && ((base | size) & 0xFFFu) == 0 &&
                   (reinterpret_cast<uintptr_t>(host) & 0xFFFu) == 0 &&
                   base + size <= Memory::PROCESS_IMAGE_VADDR,
               "an extra guest mapping {:#010x}+{:#x} at {}", base, size, fmt::ptr(host));
    const auto next = live_mappings.lower_bound(base);
    ASSERT_MSG(next == live_mappings.end() || next->first >= base + size,
               "an extra guest mapping {:#010x}+{:#x} over a guest one", base, size);
    extra_base = base;
    extra = Mapping{size, host};
    if (installed) {
        const bool mapped = Edit(AZAHAR_EDIT_MAP, reinterpret_cast<u32>(host), base, size,
                                 AZAHAR_PERM_R | AZAHAR_PERM_X);
        ASSERT_MSG(mapped, "the extra guest mapping {:#010x}+{:#x} was not mapped", base, size);
    }
}

void VitaGuestMap::SyncAllCode() {
    if (!installed) {
        return;
    }
    for (const auto& [base, mapping] : live_mappings) {
        Edit(AZAHAR_EDIT_SYNC_CODE, reinterpret_cast<u32>(mapping.host), base, mapping.size, 0);
    }
}

// ---------------------------------------------------------------------------- ARM_VitaNative

std::shared_ptr<VitaGuestMap> ARM_VitaNative::CreateMap(Memory::MemorySystem& memory) {
    // The module holds a core only in its resident design, and a thread pinned to a held core
    // never runs again - so this thread steps aside before the take, and steps back if the
    // module answers that it folded the guest into its caller instead. Folded, the partition's
    // choice is the one that matters: the slice runs on this thread's core, so the emulation
    // thread belongs on the core the frontend gave it and the guest goes there with it.
    const SceUID self = sceKernelGetThreadId();
    const int affinity = sceKernelGetThreadCpuAffinityMask(self);
    if (affinity == (SCE_KERNEL_CPU_MASK_USER_0 << TakenCore) || affinity == 0) {
        sceKernelChangeThreadCpuAffinityMask(self, SCE_KERNEL_CPU_MASK_USER_0);
    }

    const int ret = azaharTakeCore();
    if (ret < 0) {
        LOG_WARNING(Core_ARM11, "azaharTakeCore failed ({}): native execution unavailable", ret);
        return nullptr;
    }
    if (ret == AZAHAR_TAKE_FOLDED && affinity != 0) {
        const int back = sceKernelChangeThreadCpuAffinityMask(self, affinity);
        LOG_INFO(Core_ARM11, "folded native execution: emulation thread back on affinity {:#x} "
                             "({:#010x})", affinity, back);
    }
    auto map = std::make_shared<VitaGuestMap>(memory);
    memory.SetMappingObserver(map.get());
    return map;
}

ARM_VitaNative::ARM_VitaNative(Core::System& system_, Memory::MemorySystem& memory_, u32 id,
                               std::shared_ptr<Core::Timing::Timer> timer_,
                               std::shared_ptr<VitaGuestMap> map_)
    : ARM_Interface(id, timer_), system{system_}, memory{memory_}, svc_context{system_},
      map{std::move(map_)} {
    usable = map != nullptr;
    context.cpsr = 0x10;
}

ARM_VitaNative::~ARM_VitaNative() {
    if (map != nullptr && map.use_count() == 1) {
        memory.SetMappingObserver(nullptr);
    }
}

void ARM_VitaNative::Run() {
    if (break_flag) [[unlikely]] {
        return;
    }
    // The scheduler asks for a stop immediately before handing out a new slice; that request is
    // about the slice that just ended.
    stop_requested = false;

    const u64 quantum = std::clamp(TicksToNanoseconds(timer->GetDowncount()), MinQuantumNs,
                                   MaxQuantumNs);
    while (RunSlice(quantum)) {
        if (timer->GetDowncount() <= 0 || break_flag || stop_requested) {
            break;
        }
    }
}

void ARM_VitaNative::Step() {
    if (break_flag) [[unlikely]] {
        return;
    }
    stop_requested = false;
    RunSlice(MinQuantumNs);
}

void ARM_VitaNative::PatchVfp(u32 pc, u32 insn, bool thumb) {
    // A trapping instruction becomes a branch to a thunk; out of reach or room, svc AZAHAR_VFP_SVC,
    // which the module runs without an exit. The caller syncs the site's code.
    VfpDecoded v;
    if (InVfpArea(pc) || !DecodeVfp(insn, v)) {
        return;
    }
    if (PatchVfpThunk(pc, insn, thumb)) {
#ifdef VITA_DIAGNOSTICS
        census.vfp_thunks++;
#endif
        return;
    }
    if (azaharVfpPatch(pc, VfpForm(v)) != 0) {
        return;
    }
    if (thumb) {
        memory.Write16(pc, 0xDF00 | AZAHAR_VFP_SVC);
        memory.Write16(pc + 2, 0xBF00);
    } else {
        memory.Write32(pc, (insn & 0xF0000000u) | 0x0F000000u | AZAHAR_VFP_SVC);
    }
    vfp_patched[pc] = VfpSite{insn, thumb};
#ifdef VITA_DIAGNOSTICS
    census.vfp_svcs++;
#endif
}

bool ARM_VitaNative::PatchVfpThunk(u32 pc, u32 insn, bool thumb) {
    VfpDecoded v;
    if (!DecodeVfp(insn, v)) {
        return false;
    }
    const u32 vector = context.fpscr & FpscrVectorBits;
    const u32 stride_field = vector >> 20 & 3;
    if (vector == 0 || (stride_field != 0 && stride_field != 3)) {
        return false;
    }
    const u32 bank = v.dp ? 4 : 8;
    const bool scalar = v.d < bank; // one element whatever LEN says
    const u32 len = scalar ? 1 : (vector >> 16 & 7) + 1;
    const u32 stride = stride_field == 3 ? 2 : 1;
    const auto next = [&](u32 r) { return r / bank * bank + (r % bank + stride) % bank; };
    // An element: the instruction, unconditional, on its registers.
    const u32 al = (insn & 0x0FFFFFFFu) | 0xE0000000u;
    const u32 fields = 0x0040F02Fu | (v.two ? 0u : 0x000F0080u);
    const auto element = [&](u32 d, u32 n, u32 m) {
        u32 e = al & ~fields;
        if (v.dp) {
            e |= d << 12 | m;
            if (!v.two) {
                e |= n << 16;
            }
        } else {
            e |= (d >> 1) << 12 | (d & 1) << 22 | (m >> 1) | (m & 1) << 5;
            if (!v.two) {
                e |= (n >> 1) << 16 | (n & 1) << 7;
            }
        }
        return e;
    };
    DEBUG_ASSERT_MSG(element(v.d, v.n, v.m) == al, "VFP thunk: {:08X} re-encoded as {:08X}", al,
                     element(v.d, v.n, v.m));
    std::array<u32, 8> elements{};
    for (u32 k = 0, d = v.d, n = v.n, m = v.m; k < len; k++) {
        elements[k] = element(d, n, m);
        d = next(d);
        n = next(n);
        if (v.m >= bank) {
            m = next(m);
        }
    }

    // The area, mapped into this guest's table.
    if (!vfp_area) {
        vfp_area = std::make_unique<Common::HostSharedMemory>(VfpAreaSize, "azahar-vfp-thunks");
        ASSERT_MSG(vfp_area->Data() != nullptr &&
                       (reinterpret_cast<uintptr_t>(vfp_area->Data()) & 0xFFFu) == 0,
                   "the VFP thunk area's {} KiB", VfpAreaSize >> 10);
    }
    if (vfp_area_map != map.get()) {
        ForgetVfpPatches(vfp_area_map); // another guest's
        map->MapExtra(VfpAreaBase, VfpAreaSize, vfp_area->Data());
        vfp_area_map = map.get();
    }
    if (vfp_area_used + VfpThunkMax > VfpAreaSize) {
        return false;
    }
    const u32 base = VfpAreaBase + vfp_area_used;

    // The thunk goes to a buffer first: nothing changes unless every branch reaches.
    std::array<u8, VfpThunkMax> buf{};
    u32 size = 0;
    const auto at = [&] { return base + size; };
    const auto put16 = [&](u16 h) {
        ASSERT(size + 2 <= VfpThunkMax);
        std::memcpy(buf.data() + size, &h, 2);
        size += 2;
    };
    const auto put32 = [&](u32 w) {
        if (thumb) {
            put16(static_cast<u16>(w >> 16));
            put16(static_cast<u16>(w));
        } else {
            ASSERT(size + 4 <= VfpThunkMax);
            std::memcpy(buf.data() + size, &w, 4);
            size += 4;
        }
    };
    const auto put_pair = [&](const std::array<u16, 2>& hw) {
        put16(hw[0]);
        put16(hw[1]);
    };
    const u32 back_to = pc + 4;
    VfpThunk t{};
    t.site = pc;
    t.thumb = thumb;
    const auto put_back = [&] {
        if (thumb) {
            const auto b = ThumbBranchW(at(), back_to);
            if (!b) {
                return false;
            }
            put_pair(*b);
        } else {
            const auto b = ArmBranch(0xE, at(), back_to);
            if (!b) {
                return false;
            }
            put32(*b);
        }
        return true;
    };

    // push {r0-r2}; mrs r1, apsr; vmrs r2, fpscr; and r2, r2, #LEN|STRIDE; the guard
    if (thumb) {
        put16(0xB407);
        t.mrs = at();
        put_pair({0xF3EF, 0x8100});
        put32(0xEEF12A10);
        put_pair(ThumbImmOp(0xF002, 2, FpscrVectorBits)); // and r2, r2, #imm
        if (scalar) {
            put16(0x2A00); // cmp r2, #0
        } else {
            put_pair(ThumbImmOp(0xF1B2, 0xF, vector)); // cmp r2, #vector
        }
    } else {
        put32(0xE92D0007);
        t.mrs = at();
        put32(0xE10F1000);
        put32(0xEEF12A10);
        put32(0xE2022837);                                        // and r2, r2, #0x370000
        put32(scalar ? 0xE3520000u : 0xE3520800u | vector >> 16); // cmp r2, #...
    }
    const u32 guard_at = at(); // its branch to the miss, filled in below
    thumb ? put16(0) : put32(0);
    // vmrs r0, fpscr; bic r0, r0, #LEN|STRIDE; vmsr fpscr, r0
    put32(0xEEF10A10);
    if (thumb) {
        put_pair(ThumbImmOp(0xF020, 0, FpscrVectorBits)); // bic r0, r0, #imm
    } else {
        put32(0xE3C00837);
    }
    put32(0xEEE10A10);
    t.elements = at();
    for (u32 k = 0; k < len; k++) {
        put32(elements[k]);
    }
    t.restore = at();
    // vmrs r0, fpscr; orr r0, r0, r2; vmsr fpscr, r0; msr apsr_nzcvq, r1; pop {r0-r2}; b back
    put32(0xEEF10A10);
    if (thumb) {
        put16(0x4310); // orrs r0, r2 (the flags come back below)
    } else {
        put32(0xE1800002);
    }
    put32(0xEEE10A10);
    if (thumb) {
        put_pair({0xF381, 0x8800});
        put16(0xBC07);
    } else {
        put32(0xE128F001);
        put32(0xE8BD0007);
    }
    t.back = at();
    if (!put_back()) {
        return false;
    }
    // miss: msr apsr_nzcvq, r1; pop {r0-r2}; the original; b back
    t.miss = at();
    if (thumb) {
        put_pair({0xF381, 0x8800});
        put16(0xBC07);
        t.miss_original = at();
        put32(insn);
    } else {
        put32(0xE128F001);
        put32(0xE8BD0007);
        t.miss_original = at();
        put32(al);
    }
    t.miss_back = at();
    if (!put_back()) {
        return false;
    }
    t.end = at();
    // The guard's branch: bne (beq for a scalar destination) to the miss.
    const u32 cond = scalar ? 0x0u : 0x1u;
    if (thumb) {
        const s32 off = static_cast<s32>(t.miss - (guard_at + 4));
        ASSERT(off >= -256 && off <= 254 && (off & 1) == 0);
        const u16 b =
            static_cast<u16>(0xD000u | cond << 8 | ((static_cast<u32>(off) >> 1) & 0xFFu));
        std::memcpy(buf.data() + (guard_at - base), &b, 2);
    } else {
        const auto b = ArmBranch(cond, guard_at, t.miss);
        ASSERT(b.has_value());
        std::memcpy(buf.data() + (guard_at - base), &*b, 4);
    }

    std::array<u16, 2> site_hw{};
    u32 site_word = 0;
    if (thumb) {
        const auto b = ThumbBranchW(pc, base);
        if (!b) {
            return false;
        }
        site_hw = *b;
    } else {
        const auto b = ArmBranch(insn >> 28, pc, base);
        if (!b) {
            return false;
        }
        site_word = *b;
    }

    std::memcpy(vfp_area->Data() + vfp_area_used, buf.data(), size);
    map->SyncCode(base, size);
    vfp_area_used += (size + 3) & ~3u;
    vfp_thunks[base] = t;
    if (thumb) {
        memory.Write16(pc, site_hw[0]);
        memory.Write16(pc + 2, site_hw[1]);
    } else {
        memory.Write32(pc, site_word);
    }
    vfp_patched[pc] = VfpSite{insn, thumb};
    return true;
}

u32 ARM_VitaNative::FetchCode(u32 pc, bool thumb) const {
    if (InVfpArea(pc) && vfp_area) {
        const u8* const p = vfp_area->Data() + (pc - VfpAreaBase);
        if (thumb) {
            u16 hw[2];
            std::memcpy(hw, p, 4);
            return u32{hw[0]} << 16 | hw[1];
        }
        u32 w;
        std::memcpy(&w, p, 4);
        return w;
    }
    return thumb ? u32{memory.Read16(pc)} << 16 | memory.Read16(pc + 2) : memory.Read32(pc);
}

void ARM_VitaNative::PrepareSaveState() {
    if (vfp_patched.empty()) {
        return;
    }
    // Every thread that stands in a thunk leaves it (FinishVfpThunk): each core's running one,
    // through the core, and every other one through its saved context.
    for (u32 i = 0; i < system.GetNumCores(); i++) {
        ARM_Interface& core = system.GetCore(i);
        if (!InVfpArea(core.GetPC())) {
            continue;
        }
        ThreadContext t;
        core.SaveContext(t);
        AzaharContext c{};
        ToAzahar(t, c);
        FinishVfpThunk(memory, c);
        FromAzahar(c, t);
        core.LoadContext(t);
    }
    auto& kernel = system.Kernel();
    for (u32 i = 0; i < system.GetNumCores(); i++) {
        auto& manager = kernel.GetThreadManager(i);
        const Kernel::Thread* const running = manager.GetCurrentThread();
        for (const auto& thread : manager.GetThreadList()) {
            if (thread.get() == running || !InVfpArea(thread->context.cpu_registers[15] & ~1u)) {
                continue;
            }
            AzaharContext c{};
            ToAzahar(thread->context, c);
            FinishVfpThunk(memory, c);
            FromAzahar(c, thread->context);
        }
    }
    // The guest's own instructions back; each traps and is patched again at its next run.
    for (const auto& [pc, site] : vfp_patched) {
        if (site.thumb) {
            memory.Write16(pc, static_cast<u16>(site.insn >> 16));
            memory.Write16(pc + 2, static_cast<u16>(site.insn));
        } else {
            memory.Write32(pc, site.insn);
        }
        map->SyncCode(pc, 4);
    }
    LOG_INFO(Core_ARM11, "VFP: {} patched sites restored for the savestate ({} thunks)",
             vfp_patched.size(), vfp_thunks.size());
    vfp_patched.clear();
    vfp_thunks.clear();
    vfp_area_used = 0;
}

bool ARM_VitaNative::RunSlice(u64 quantum_ns) {
    if (!usable) [[unlikely]] {
        system.SetStatus(System::ResultStatus::ErrorCoreExceptionRaised,
                         "Vita native backend is not usable");
        return false;
    }
    if (!map->IsInstalled()) {
        if (!map->Install()) {
            system.SetStatus(System::ResultStatus::ErrorCoreExceptionRaised,
                             "Vita native backend could not install the guest table");
            usable = false;
            return false;
        }
        // From now on there is a guest view for the GPU's work to hold pages of.
        system.GPU().EnablePageHolds();
    }

    AzaharRunRequest run{};
    run.ctx = context;
    run.ctx.tpidruro = cp15[CP15_THREAD_URO];
    run.ctx.tpidrurw = cp15[CP15_THREAD_UPRW];
    run.quantum_us = static_cast<u32>(quantum_ns / 1000);
    // The svcs the module answers without an exit (none of their answers change before one).
    run.fast = AZAHAR_FAST_ALL;
    auto& kernel = system.Kernel();
    if (const auto* thread = kernel.GetCurrentThreadManager().GetCurrentThread()) {
        run.thread_id = thread->GetThreadId();
    } else {
        run.fast &= ~(1u << AZAHAR_FAST_THREAD_ID);
    }
    if (const auto process = kernel.GetCurrentProcess()) {
        run.process_id = process->process_id;
    } else {
        run.fast &= ~(1u << AZAHAR_FAST_PROCESS_ID);
    }
    run.tick_hz = static_cast<u64>(BASE_CLOCK_RATE_ARM11 * timer->GetClockScale() + 0.5);
    run.tick_base = timer->GetTicks();

    const int ret = azaharRun(&run);
    if (ret < 0) {
        system.SetStatus(System::ResultStatus::ErrorCoreExceptionRaised,
                         fmt::format("azaharRun failed: {}", ret).c_str());
        usable = false;
        return false;
    }

    context = run.ctx;
    cp15[CP15_THREAD_UPRW] = run.ctx.tpidrurw;
    // The guest ticks the module counted, converted as its GetSystemTick answers were: the clock ends
    // at or past every value the guest read in the slice.
    timer->AddScaledTicks(run.guest_ticks);

#ifdef VITA_DIAGNOSTICS
    census.slices++;
    census.reasons[run.reason < census.reasons.size() ? run.reason : 0]++;
    for (u32 i = 0; i < AZAHAR_FAST_KINDS; i++) {
        census.fast[i] += run.fast_count[i];
    }
    census.guest_ticks += run.guest_ticks;
    census.kernel_us += run.kernel_us;
    if (run.reason == AZAHAR_EXIT_SVC) {
        census.svcs[run.svc_number < census.svcs.size() ? run.svc_number : 0]++;
        static const bool perflog = SgxdRenderer::SgxdFlag("perflog");
        if (perflog && run.svc_number == 0x22) {
            CountArbitration();
        }
        if (perflog && run.svc_number == 0x0A) {
            const auto* thread = system.Kernel().GetCurrentThreadManager().GetCurrentThread();
            const u32 id = thread ? thread->GetThreadId() : 0;
            const s64 ns = static_cast<s64>(u64{context.r[1]} << 32 | context.r[0]);
            auto it = std::find_if(census.sleeps.begin(), census.sleeps.end(), [&](const auto& s) {
                return s.count == 0 || (s.lr == context.r[14] && s.thread == id);
            });
            if (it == census.sleeps.end()) {
                census.sleep_other++;
            } else {
                *it = {context.r[14], id, it->count + 1, ns};
            }
        }
    }
#endif

    switch (run.reason) {
    case AZAHAR_EXIT_SVC:
        // A patched VFP instruction the module did not run: run here. One with no record is from a
        // savestate made before the restore at each save, and is lost.
        if (run.svc_number == AZAHAR_VFP_SVC) {
            const bool thumb = context.cpsr >> 5 & 1;
            const u32 site = context.r[15] - (thumb ? 2u : 4u);
            const auto it = vfp_patched.find(site);
            ASSERT_MSG(it != vfp_patched.end(),
                       "svc {:#x} at {:08X} with no patched VFP instruction behind it (a savestate "
                       "from a build that saved the module's svcs)",
                       AZAHAR_VFP_SVC, site);
            [[maybe_unused]] const bool ran = RunVfpVector(context, it->second.insn);
            DEBUG_ASSERT_MSG(ran, "a patched VFP instruction at {:08X} did not run", it->first);
            return !break_flag && !stop_requested;
        }
        svc_context.CallSVC(run.svc_number);
        // An svc can reschedule, which replaces the register file wholesale.
        return !break_flag && !stop_requested;

    case AZAHAR_EXIT_TIMER:
        return false;

    case AZAHAR_EXIT_DABT: {
        // A small-page permission fault (DFSR FS 0b01111) may be a GPU page hold: the GPU waits for the
        // work, opens the page, and the access runs again.
        const u32 status = (run.fsr & 0xFu) | ((run.fsr >> 6) & 0x10u);
        const bool write = (run.fsr & (1u << 11)) != 0;
        if (status == 0xFu) {
            const u8* const host = map->HostAt(run.far);
            if (host != nullptr && system.GPU().GuestAbort(host, write, context.r[15], run.far)) {
                return !break_flag && !stop_requested;
            }
        }
        ReportFault(run);
        return false;
    }
    case AZAHAR_EXIT_UND: {
        // VFP instructions under the guest's short vectors (RunVfpVector), ARM or Thumb-2: the trapped
        // one and the data-processing ones after it run here.
        if ((context.fpscr & FpscrVectorBits) != 0) {
            u32 pc = context.r[15];
            const bool thumb = context.cpsr >> 5 & 1;
            DEBUG_ASSERT_MSG(!thumb || (context.cpsr & 0x0600FC00) == 0,
                             "VFP short vectors in a Thumb IT block at {:08X}", pc);
            u32 ran = 0;
            for (; ran < MaxVfpRun; ran++) {
                const u32 insn = FetchCode(pc, thumb);
                const u32 cond = thumb ? 0xEu : insn >> 28;
                // After the trapped one, only data-processing forms (Thumb-2's start 0xEE).
                if (ran > 0 && (thumb ? (insn >> 24) != 0xEE : !IsVfpDataProcessing(insn))) {
                    break;
                }
                if (cond == 0xF ||
                    !(!ConditionPasses(cond, context.cpsr) || RunVfpVector(context, insn))) {
                    break;
                }
                PatchVfp(pc, insn, thumb);
                pc += 4;
            }
            if (ran > 0) {
                // The patched instructions past the guest's instruction cache.
                map->SyncCode(context.r[15], pc - context.r[15]);
                context.r[15] = pc;
                return !break_flag && !stop_requested;
            }
        }
        ReportFault(run);
        return false;
    }
    case AZAHAR_EXIT_PABT:
    default:
        ReportFault(run);
        return false;
    }
}

#ifdef VITA_DIAGNOSTICS
void ARM_VitaNative::CountArbitration() {
    // ArbitrateAddress(handle r0, address r1, type r2, value r3, ns r4:r5): would it wake a thread or
    // block this one?
    const VAddr address = context.r[1];
    const u32 type = context.r[2];
    const s32 value = static_cast<s32>(context.r[3]);
    auto& kernel = system.Kernel();
    switch (type) {
    case 0: { // Signal
        bool waiter = false;
        for (u32 core = 0; core < system.GetNumCores() && !waiter; core++) {
            for (const auto& thread : kernel.GetThreadManager(core).GetThreadList()) {
                if (thread->status == Kernel::ThreadStatus::WaitArb &&
                    thread->wait_address == address) {
                    waiter = true;
                    break;
                }
            }
        }
        (waiter ? census.arb_signal_wake : census.arb_signal_idle)++;
        break;
    }
    case 1: // WaitIfLessThan
    case 2: // DecrementAndWaitIfLessThan
    case 3: // WaitIfLessThanWithTimeout
    case 4: // DecrementAndWaitIfLessThanWithTimeout
        (static_cast<s32>(memory.Read32(address)) < value ? census.arb_wait_block
                                                          : census.arb_wait_pass)++;
        break;
    default:
        census.arb_other++;
        break;
    }
}

VitaNativeCensus TakeVitaNativeCensus() {
    return std::exchange(census, VitaNativeCensus{});
}
#endif

void ARM_VitaNative::ReportFault(const AzaharRunRequest& run) {
    std::string registers;
    for (int i = 0; i < 16; i++) {
        registers += fmt::format("r{:02d} = {:08X}\n", i, context.r[i]);
    }
    const std::string message =
        fmt::format("{}Guest {} at PC {:08X} (cpsr {:08X}, fpscr {:08X}, fpexc {:08X}), fsr {:08X} "
                    "far {:08X}",
                    registers, ReasonName(run.reason), context.r[15], context.cpsr, context.fpscr,
                    fpexc, run.fsr, run.far);
    LOG_CRITICAL(Core_ARM11, "{}", message);
    std::string code;
    for (u32 a = context.r[15] - 16; a <= context.r[15] + 12; a += 4) {
        code += fmt::format("{}{:08X}", a == context.r[15] ? " *" : " ", memory.Read32(a));
    }
    LOG_CRITICAL(Core_ARM11, "code around PC:{}", code);
    if (InThunkArea(run.ctx.r[15])) {
        const ThunkClaim* const c = Host1ClaimAt(run.ctx.r[15]);
        std::string host0;
        for (u32 a = run.ctx.r[15] - 16; a <= run.ctx.r[15] + 12; a += 4) {
            u32 w = 0;
            if (thunk_area && InThunkArea(a)) {
                std::memcpy(&w, thunk_area->Data() + (a - ThunkAreaBase), 4);
            }
            host0 += fmt::format("{}{:08X}", a == run.ctx.r[15] ? " *" : " ", w);
        }
        LOG_CRITICAL(Core_ARM11, "host 0's thunk area around PC:{}; upcall body {:08X}, tail {:08X}",
                     host0, upcall_body, upcall_after_svc);
        LOG_CRITICAL(Core_ARM11,
                     "thunk area: host 1's stale {}, used {}; {} claims; the claim at PC: {}",
                     host1_area_stale, host1_area_used, thunk_claims.load(std::memory_order_relaxed),
                     c ? fmt::format("entry {:08X} for site {:08X} ({}, kind {})", c->entry,
                                     c->site, c->thumb ? "Thumb" : "ARM",
                                     static_cast<int>(c->kind))
                       : std::string("none"));
    }
    // Where the guest came from: the code before LR and the stack from SP.
    const u32 lr = context.r[14] & ~1u;
    std::string call;
    for (u32 a = lr - 16; a < lr + 8; a += 4) {
        call += fmt::format("{}{:08X}", a == (lr & ~3u) ? " *" : " ", memory.Read32(a));
    }
    LOG_CRITICAL(Core_ARM11, "code around LR {:08X}:{}", context.r[14], call);
    for (u32 row = 0; row < 8; row++) {
        std::string words;
        for (u32 w = 0; w < 8; w++) {
            words += fmt::format(" {:08X}", memory.Read32(context.r[13] + (row * 8 + w) * 4));
        }
        LOG_CRITICAL(Core_ARM11, "stack {:08X}:{}", context.r[13] + row * 32, words);
    }
    system.SetStatus(System::ResultStatus::ErrorCoreExceptionRaised, message.c_str());
}

void ARM_VitaNative::ClearInstructionCache() {
    if (map) {
        map->SyncAllCode();
    }
}

void ARM_VitaNative::InvalidateCacheRange(u32 start_address, std::size_t length) {
    if (!map) {
        return;
    }
    // svcInvalidateInstructionCacheRange, which some titles issue every frame over a page
    // of their data. A sync is a kernel round trip that cleans the page and drops core 2's
    // icache, and it is only owed where something can execute: the process's own view of
    // the range says whether it can.
    if (const auto process = system.Kernel().GetCurrentProcess()) {
        const auto& vm = process->vm_manager;
        bool executable = false;
        const VAddr end = start_address + static_cast<VAddr>(length);
        for (VAddr at = start_address; at < end;) {
            const auto vma = vm.FindVMA(at);
            if (vma == vm.vma_map.end()) {
                break;
            }
            if ((static_cast<u8>(vma->second.permissions) &
                 static_cast<u8>(Kernel::VMAPermission::Execute)) != 0) {
                executable = true;
                break;
            }
            at = vma->second.base + vma->second.size;
        }
        if (!executable) {
            static u32 shown = 0;
            if (shown++ < 4) {
                LOG_INFO(Core_ARM11, "icache invalidate of {:08X}+{:X} skipped: nothing executes there",
                         start_address, length);
            }
            return;
        }
    }
    map->SyncCode(start_address, static_cast<u32>(length));
}

void ARM_VitaNative::ClearExclusiveState() {
    // Nothing to do here. Whether an exception return clears the local monitor is
    // IMPLEMENTATION DEFINED on ARMv7-A, so entry.S executes
    // CLREX explicitly before every guest entry - which covers this and every other path back
    // into the guest, including the ones that never call through here.
}

void ARM_VitaNative::SetPageTable(const std::shared_ptr<Memory::PageTable>& page_table) {
    current_page_table = page_table;
    // The one call that reaches every core: the kernel updates the memory system's current
    // table only for the core it considers running, so a table given to any other core would
    // never reach the mapping observer at all - and the guest table would install empty.
    if (map && page_table && map->MirroredTable() != page_table.get()) {
        memory.SyncMappingObserver(*page_table);
        map->SetMirroredTable(page_table.get());
    }
}

std::shared_ptr<Memory::PageTable> ARM_VitaNative::GetPageTable() const {
    return current_page_table;
}

void ARM_VitaNative::SetPC(u32 pc) {
    context.r[15] = pc & ~1u;
}

u32 ARM_VitaNative::GetPC() const {
    return context.r[15] & ~1u;
}

u32 ARM_VitaNative::GetReg(int index) const {
    ASSERT(index >= 0 && index < 16);
    return index == 15 ? GetPC() : context.r[index];
}

void ARM_VitaNative::SetReg(int index, u32 value) {
    ASSERT(index >= 0 && index < 16);
    if (index == 15) {
        SetPC(value);
    } else {
        context.r[index] = value;
    }
}

u32 ARM_VitaNative::GetVFPReg(int index) const {
    ASSERT(index >= 0 && index < 64);
    return context.fpregs[index];
}

void ARM_VitaNative::SetVFPReg(int index, u32 value) {
    ASSERT(index >= 0 && index < 64);
    context.fpregs[index] = value;
}

u32 ARM_VitaNative::GetVFPSystemReg(VFPSystemRegister reg) const {
    switch (reg) {
    case VFP_FPSCR:
        return context.fpscr;
    case VFP_FPEXC:
        return fpexc;
    default:
        UNREACHABLE_MSG("Unknown VFP system register: {}", reg);
    }
}

void ARM_VitaNative::SetVFPSystemReg(VFPSystemRegister reg, u32 value) {
    switch (reg) {
    case VFP_FPSCR:
        context.fpscr = value;
        return;
    case VFP_FPEXC:
        fpexc = value;
        return;
    default:
        UNREACHABLE_MSG("Unknown VFP system register: {}", reg);
    }
}

u32 ARM_VitaNative::GetCPSR() const {
    return context.cpsr;
}

void ARM_VitaNative::SetCPSR(u32 cpsr) {
    context.cpsr = cpsr;
}

u32 ARM_VitaNative::GetCP15Register(CP15Register reg) const {
    return cp15[reg];
}

void ARM_VitaNative::SetCP15Register(CP15Register reg, u32 value) {
    cp15[reg] = value;
}

void ARM_VitaNative::SaveContext(ThreadContext& ctx) {
    std::memcpy(ctx.cpu_registers.data(), context.r, sizeof(context.r));
    ctx.cpu_registers[15] = GetPC();
    ctx.cpsr = context.cpsr;
    std::memcpy(ctx.fpu_registers.data(), context.fpregs, sizeof(context.fpregs));
    ctx.fpscr = context.fpscr;
    ctx.fpexc = fpexc;
}

void ARM_VitaNative::LoadContext(const ThreadContext& ctx) {
    std::memcpy(context.r, ctx.cpu_registers.data(), sizeof(context.r));
    context.r[15] = ctx.cpu_registers[15] & ~1u;
    context.cpsr = ctx.cpsr;
    std::memcpy(context.fpregs, ctx.fpu_registers.data(), sizeof(context.fpregs));
    context.fpscr = ctx.fpscr;
    fpexc = ctx.fpexc;
}

void ARM_VitaNative::PrepareReschedule() {
    // Called from the HLE side while the guest is stopped at an exit, or from another host thread
    // while it runs. The former ends the slice loop; the latter is bounded by the quantum.
    stop_requested = true;
}

} // namespace Core
