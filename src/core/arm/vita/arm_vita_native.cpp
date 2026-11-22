// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "core/arm/vita/arm_vita_native.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <optional>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>
#include "common/assert.h"
#include "common/host_shared_memory.h"
#include "common/logging/log.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/address_arbiter.h"
#include "core/hle/kernel/client_session.h"
#include "core/hle/kernel/errors.h"
#include "core/hle/kernel/event.h"
#include "core/hle/kernel/ipc_debugger/recorder.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/mutex.h"
#include "core/hle/kernel/process.h"
#include "core/hle/kernel/resource_limit.h"
#include "core/hle/kernel/svc.h"
#include "core/hle/kernel/thread.h"
#include "core/hle/kernel/vm_manager.h"
#include "core/hle/kernel/wait_object.h"
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

/// In-kernel scheduling (azaharnative 34): the module switches the guest's threads on a
/// SleepThread itself (AzaharSchedThread). On unless the frontend turned it off.
bool in_kernel_scheduling = true;
/// One slice's thread table: as ThreadManager::ExportNativeSchedule gave it, the module's copy
/// of it (the threads' contexts in place), and the states the slice left.
std::vector<Kernel::ThreadManager::NativeSchedEntry> sched_before, sched_after;
std::array<AzaharSchedThread, AZAHAR_SCHED_MAX> sched_table;
/// The arbiter the slice's table carries the waiters of, from export to import.
std::shared_ptr<Kernel::AddressArbiter> sched_arbiter;
/// The threads whose last stop was a SleepThread or an ArbitrateAddress (an exit for one, or one
/// the module answered): only their slices carry the table. A title that sleeps rarely (a game: 200
/// a second among 5,000 svc exits) paid the table's export and import on every slice, 6% of its
/// race.
std::unordered_set<u32> sched_waiters;
/// The arbiter handle of the last ArbitrateAddress that exited: the one the module answers
/// (azaharnative 36; a title has one, MH Generations its single CreateAddressArbiter).
Kernel::Handle sched_arbiter_handle{};

// A thread's context is the module's AzaharContext, fpexc after it: the module loads a thread
// from its own context in place (the upcall's switch, the in-kernel scheduler's table).
using ThreadContext = ARM_Interface::ThreadContext;
static_assert(std::is_standard_layout_v<ThreadContext>);
static_assert(offsetof(ThreadContext, cpu_registers) == offsetof(AzaharContext, r) &&
              sizeof(ThreadContext::cpu_registers) == sizeof(AzaharContext::r));
static_assert(offsetof(ThreadContext, cpsr) == offsetof(AzaharContext, cpsr));
static_assert(offsetof(ThreadContext, tpidruro) == offsetof(AzaharContext, tpidruro));
static_assert(offsetof(ThreadContext, tpidrurw) == offsetof(AzaharContext, tpidrurw));
static_assert(offsetof(ThreadContext, fpscr) == offsetof(AzaharContext, fpscr));
static_assert(offsetof(ThreadContext, fpu_registers) == offsetof(AzaharContext, fpregs) &&
              sizeof(ThreadContext::fpu_registers) == sizeof(AzaharContext::fpregs));
static_assert(offsetof(ThreadContext, fpexc) == sizeof(AzaharContext));

AzaharContext* AsAzahar(ThreadContext& t) {
    return reinterpret_cast<AzaharContext*>(&t);
}

void ToAzahar(const ThreadContext& t, AzaharContext& c) {
    std::memcpy(&c, &t, sizeof(c));
    c.r[15] &= ~1u;
}

void FromAzahar(const AzaharContext& c, ThreadContext& t) {
    std::memcpy(&t, &c, sizeof(c));
}

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
constexpr VAddr ThunkAreaBase = 0x000C0000;
constexpr u32 ThunkAreaSize = 0x40000;
/// The largest thunk: ARM 19 words and eight elements, Thumb about as many bytes.
constexpr u32 VfpThunkMax = 128;

bool InThunkArea(u32 addr) {
    return addr - ThunkAreaBase < ThunkAreaSize;
}

/// A patched site: the original instruction (Thumb's halfwords high first, as fetched).
struct PatchedSite {
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

// The svcs the module answers in the vector (AZAHAR_FAST_*) are answered by thunks as well, from
// two read-only pages it maps into the guest's table (AZAHAR_USER_TIMER_VA): GetSystemTick from
// the private timer and the entry's clock, GetThreadId and GetProcessId on the current thread and
// process, and the process data cache svcs on the current process. The module reports an ARM site
// it answered (AzaharRunRequest.fast_site) and PatchSvc branches the site to a thunk. An svc keeps
// every register but its outputs, and the flags: the thunk works in the caller-saved r2, r3 and
// r12 with LR holding the flags, all four on the guest's stack meanwhile, and writes its answer
// to r0 and r1. A handle it does not answer, or an answer the page does not hold this entry, runs
// the original svc in the thunk's miss path (an exit, or the module's own answer). Thumb's 16-bit
// svc has no room for a branch and stays.

/// An svc thunk's instructions by address, for a thread stopped inside it (FinishSvcThunk).
struct SvcThunk {
    u32 site;
    u32 mrs;      ///< mrs lr: past it LR holds the guest's flags
    u32 answered; ///< from here the answer is in r0 and r1 (and the svc's inputs may be gone)
    u32 done;     ///< the hit's msr
    u32 pop;      ///< pop {r2, r3, r12, lr}
    u32 back;
    u32 miss; ///< the miss's msr
    u32 miss_pop;
    u32 miss_svc;
    u32 miss_back;
    u32 end;
};
/// The largest svc thunk: GetSystemTick's 31 words.
constexpr u32 SvcThunkMax = 136;

/// The guest's patched sites and thunks; the svc sites PatchSvc could not patch.
std::unordered_map<u32, PatchedSite> patched_sites;
std::map<u32, VfpThunk> vfp_thunks;
std::map<u32, SvcThunk> svc_thunks;
/// An upcall thunk (the experiment below): its site, end, and the svcs its tail may run (the
/// reschedule's and the original's), each followed by the branch back.
struct UpcallThunk {
    u32 site;
    u32 end;
    u32 svc_reschedule;
    u32 svc_original;
};
std::map<u32, UpcallThunk> upcall_thunks;
std::unordered_set<u32> svc_refused;
std::unique_ptr<Common::HostSharedMemory> thunk_area;
const VitaGuestMap* thunk_area_map{};
u32 thunk_area_used{};

/// The guest that map held is gone, and its patches with it.
void ForgetThunks(const VitaGuestMap* map) {
    if (thunk_area_map == map) {
        patched_sites.clear();
        vfp_thunks.clear();
        svc_thunks.clear();
        svc_refused.clear();
        upcall_thunks.clear();
        thunk_area_used = 0;
        thunk_area_map = nullptr;
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
            const u8* const p = thunk_area->Data() + (a - ThunkAreaBase);
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

/// FinishVfpThunk's for an svc thunk: before its answer the thread stands at the site, after it
/// past it.
void FinishSvcThunk(Memory::MemorySystem& memory, AzaharContext& c) {
    const u32 pc = c.r[15];
    auto it = svc_thunks.upper_bound(pc);
    ASSERT_MSG(it != svc_thunks.begin(), "a thread at {:08X} in no svc thunk", pc);
    const SvcThunk& t = (--it)->second;
    ASSERT_MSG(pc < t.end && (c.cpsr & 0x20) == 0, "a thread at {:08X} past its svc thunk", pc);
    const auto pop = [&] {
        c.r[2] = memory.Read32(c.r[13]);
        c.r[3] = memory.Read32(c.r[13] + 4);
        c.r[12] = memory.Read32(c.r[13] + 8);
        c.r[14] = memory.Read32(c.r[13] + 12);
        c.r[13] += 16;
    };
    const auto flags = [&] { c.cpsr = (c.cpsr & ~0xF8000000u) | (c.r[14] & 0xF8000000u); };
    if (pc == it->first || pc == t.miss_svc) {
        c.r[15] = t.site;
    } else if (pc == t.back || pc == t.miss_back) {
        c.r[15] = t.site + 4;
    } else if (pc == t.pop || pc == t.miss_pop) {
        pop();
        c.r[15] = pc == t.pop ? t.site + 4 : t.site;
    } else {
        // Between the mrs and an msr: the flags are in LR.
        DEBUG_ASSERT_MSG(pc <= t.done || pc == t.miss, "a thread at {:08X} in svc thunk {:08X}", pc,
                         it->first);
        if (pc > t.mrs) {
            flags();
        }
        pop();
        c.r[15] = pc >= t.answered && pc <= t.done ? t.site + 4 : t.site;
    }
}

// Upcalls (the module built with AZAHAR_SEAL=0; on in a VITA_UPCALLS build, else sgxd_flags
// `upcalls`): an svc that leaves the guest is answered by the HLE kernel's own
// handler, called from the guest's world. The unsealed table has the app's pages (the process's
// TTBR1 entries, with the domains granted for the slice), so a thunk in place of the svc switches
// to a stack of the app's and BLXes into the app's code at user level, with no exception and no
// world switch. The C++ for svc N (UpcallSvcN, ARM_VitaNative::UpcallSvc) loads the running
// core's register file from the svc's r0-r7, runs SVC::CallSVCForUpcall under the HLE lock, and
// answers in r0 (the svc's result) and r1 (UpcallStatus), r1-r3's outputs in the thunk's frame.
//
// An svc keeps every register but its outputs, and the flags; the C++ keeps r4-r11, d8-d15 and
// SP by the calling convention, so the thunk keeps the caller-saved rest: r0-r3, r12, LR (the
// BLX's) and the flags on the guest's stack, the guest's SP on the app's stack, r4-r7 copied as
// the C++'s stack arguments. FPSCR stays the guest's (LEN and STRIDE are 0 at a call). The C++
// runs with the guest's TPIDRURO (user mode cannot write it): the frontend's wraps find the
// emulation thread's thread-local storage for it (citra_vita/upcall_tls.cpp), and a Sony syscall
// it makes is forwarded to Sony's world (ForwardSonySyscall).
//
// A svc that stopped the thread or asked for a reschedule answers UpcallReschedule, and the
// thunk's tail runs svc UpcallRescheduleSvc for an exit that only reschedules. (The tail's other
// svc, the original for UpcallBusy, is no longer answered.) A quantum that ends inside a thunk or
// the C++ runs the same thread on at once, and the in-kernel scheduler is off meanwhile: another
// thread's upcall would use the same stack.

enum UpcallStatus : u32 { UpcallDone = 0, UpcallReschedule = 1, UpcallBusy = 2 };
constexpr u32 UpcallRescheduleSvc = 0xFD;

alignas(16) std::array<u8, 64 * 1024> upcall_stack;
Core::System* upcall_system{};
/// Every svc an upcall (a VITA_UPCALLS build); the svcs the
/// module does not answer go to the redirect too. Idle loops are skipped (CheckTickPolls) with
/// upcalls only: the skip is one.
bool upcall_all{};

u32 upcall_quantums{};
/// The core whose slice an upcall runs in: its register file is the svc's for the HLE handlers.
ARM_VitaNative* upcall_core{};

bool UpcallsOn() {
    return upcall_all;
}

/// Whether svc `number`'s sites become upcalls: with upcalls on, every svc PatchUpcall has a C++
/// answer for.
bool UpcallFor(u32 number) {
    if (!upcall_all) {
        return false;
    }
    // Every svc but the module's own.
    return number != 0 && number != AZAHAR_VFP_SVC && number != UpcallRescheduleSvc;
}

/// The upcall for svc N: its eight input registers to ARM_VitaNative::UpcallSvc.
template <u32 N>
u64 UpcallSvcN(u32 r0, u32 r1, u32 r2, u32 r3, u32 r4, u32 r5, u32 r6, u32 r7) {
    return ARM_VitaNative::UpcallSvc(N, {r0, r1, r2, r3, r4, r5, r6, r7});
}
using UpcallFn = u64 (*)(u32, u32, u32, u32, u32, u32, u32, u32);
template <std::size_t... I>
constexpr std::array<UpcallFn, sizeof...(I)> MakeUpcallTable(std::index_sequence<I...>) {
    return {&UpcallSvcN<static_cast<u32>(I)>...};
}
constexpr auto upcall_table = MakeUpcallTable(std::make_index_sequence<0x100>{});
/// Whether a reschedule was asked while an upcall ran (ARM_VitaNative::PrepareReschedule).
std::atomic<bool> upcall_reschedule_asked{};

bool InUpcall(u32 pc) {
    if (pc >= 0x40000000u) {
        return true; // the app's own code
    }
    const auto it = upcall_thunks.upper_bound(pc);
    return it != upcall_thunks.begin() && pc < std::prev(it)->second.end;
}

/// The svc at `pc` is one an upcall thunk's tail runs: the guest's registers and stack are its
/// own again there, and the exit is an ordinary one.
bool IsUpcallTailSvc(u32 pc) {
    const auto it = upcall_thunks.upper_bound(pc);
    if (it == upcall_thunks.begin()) {
        return false;
    }
    const UpcallThunk& t = std::prev(it)->second;
    return pc == t.svc_reschedule || pc == t.svc_original;
}

u64 UpcallAnswer(u32 result, UpcallStatus status) {
    return u64{result} | u64{status} << 32;
}

/// The guest's r0-r3, r12 and LR as the thunk saved them on the guest's stack (the guest's SP is
/// at the top of the app's stack): an output beyond r0 goes into its slot, which the thunk's
/// tail pops. A guest address, reachable as it is because the upcall runs in the guest's world.
u32* UpcallGuestRegs() {
    u32 guest_sp;
    std::memcpy(&guest_sp, upcall_stack.data() + upcall_stack.size() - 8, sizeof(guest_sp));
    return reinterpret_cast<u32*>(static_cast<uintptr_t>(guest_sp)) + 1; // past the flags word
}

/// A Sony syscall (svc 0, its number in r12) an upcall's C++ made in the guest's world, made again
/// in Sony's with the same r0-r3 and r12 and the eight words past the upcall's SP copied onto this
/// thread's own stack (the kernel never answers a syscall made with SP on the upcall stack).
void ForwardSonySyscall(AzaharContext& c) {
    alignas(8) std::array<u32, 8> args;
    std::memcpy(args.data(), reinterpret_cast<const void*>(static_cast<uintptr_t>(c.r[13])),
                sizeof(args));
    register u32 r0 asm("r0") = c.r[0];
    register u32 r1 asm("r1") = c.r[1];
    register u32 r2 asm("r2") = c.r[2];
    register u32 r3 asm("r3") = c.r[3];
    register u32 r12 asm("r12") = c.r[12];
    asm volatile("mov r4, sp\n"
                 "mov sp, %[sp]\n"
                 "svc 0\n"
                 "mov sp, r4\n"
                 : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3), "+r"(r12)
                 : [sp] "r"(args.data())
                 : "r4", "memory", "cc");
    c.r[0] = r0;
    c.r[1] = r1;
    c.r[2] = r2;
    c.r[3] = r3;
    c.r[12] = r12;
}

/// A thread stopped in an upcall thunk stands after one of its tail's svcs (any other stop runs
/// on at once): it goes on after the site.
void FinishUpcallThunk(AzaharContext& c) {
    const u32 pc = c.r[15];
    const auto it = upcall_thunks.upper_bound(pc);
    ASSERT_MSG(it != upcall_thunks.begin(), "a thread at {:08X} in no upcall thunk", pc);
    const UpcallThunk& t = std::prev(it)->second;
    ASSERT_MSG(pc == t.svc_reschedule + 4 || pc == t.svc_original + 4,
               "a thread at {:08X} inside upcall thunk {:08X}", pc, std::prev(it)->first);
    c.r[15] = t.site + 4;
}

/// The Finish* for the thunk the thread stands in.
void FinishThunk(Memory::MemorySystem& memory, AzaharContext& c) {
    const u32 pc = c.r[15];
    const auto below = [pc](const auto& thunks) -> s64 {
        const auto it = thunks.upper_bound(pc);
        return it == thunks.begin() ? -1 : s64{std::prev(it)->first};
    };
    const s64 v = below(vfp_thunks), s = below(svc_thunks), u = below(upcall_thunks);
    if (u > v && u > s) {
        FinishUpcallThunk(c);
    } else if (s > v) {
        FinishSvcThunk(memory, c);
    } else {
        FinishVfpThunk(memory, c);
    }
}

} // Anonymous namespace

// ---------------------------------------------------------------------------- VitaGuestMap

VitaGuestMap::VitaGuestMap(Memory::MemorySystem& memory_) : memory{memory_} {}

VitaGuestMap::~VitaGuestMap() {
    // The module takes a release from a core taken but never mapped (a state loaded before the
    // guest's first slice).
    azaharRelease();
    ForgetThunks(this);
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
    run.ctx.cpsr = 0x10;
    upcall_system = &system;
#ifdef VITA_UPCALLS
    // Every svc an upcall (the module built with AZAHAR_SEAL=0); gxm_flags noupcalls turns it off.
    upcall_all = !GxmRenderer::GxmFlag("noupcalls");
#endif
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
    if (InThunkArea(pc) || !DecodeVfp(insn, v)) {
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
    patched_sites[pc] = PatchedSite{insn, thumb};
#ifdef VITA_DIAGNOSTICS
    census.vfp_svcs++;
#endif
}

bool ARM_VitaNative::PatchVfpThunk(u32 pc, u32 insn, bool thumb) {
    VfpDecoded v;
    if (!DecodeVfp(insn, v)) {
        return false;
    }
    const u32 vector = run.ctx.fpscr & FpscrVectorBits;
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

    const auto room = ThunkRoom(VfpThunkMax);
    if (!room) {
        return false;
    }
    const u32 base = *room;

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

    std::memcpy(thunk_area->Data() + thunk_area_used, buf.data(), size);
    map->SyncCode(base, size);
    thunk_area_used += (size + 3) & ~3u;
    vfp_thunks[base] = t;
    if (thumb) {
        memory.Write16(pc, site_hw[0]);
        memory.Write16(pc + 2, site_hw[1]);
    } else {
        memory.Write32(pc, site_word);
    }
    patched_sites[pc] = PatchedSite{insn, thumb};
    return true;
}

std::optional<u32> ARM_VitaNative::ThunkRoom(u32 size) {
    if (!thunk_area) {
        thunk_area = std::make_unique<Common::HostSharedMemory>(ThunkAreaSize, "azahar-thunks");
        ASSERT_MSG(thunk_area->Data() != nullptr &&
                       (reinterpret_cast<uintptr_t>(thunk_area->Data()) & 0xFFFu) == 0,
                   "the thunk area's {} KiB", ThunkAreaSize >> 10);
    }
    if (thunk_area_map != map.get()) {
        ForgetThunks(thunk_area_map); // another guest's
        map->MapExtra(ThunkAreaBase, ThunkAreaSize, thunk_area->Data());
        thunk_area_map = map.get();
    }
    if (thunk_area_used + size > ThunkAreaSize) {
        return std::nullopt;
    }
    return ThunkAreaBase + thunk_area_used;
}

void ARM_VitaNative::PatchSvc(u32 pc) {
    if (InThunkArea(pc) || svc_refused.contains(pc) || patched_sites.contains(pc)) {
        return;
    }
    const u32 insn = memory.Read32(pc);
    const u32 cond = insn >> 28;
    const u32 number = insn & 0x00FFFFFFu;
    // The svc and what answers it: the handle it must name (register, value; none for the
    // clock) and the AzaharUserPage bit that says the page holds the answer.
    u32 kind;
    switch (number) {
    case 0x28:
        kind = AZAHAR_FAST_TICK;
        break;
    case 0x37:
        kind = AZAHAR_FAST_THREAD_ID;
        break;
    case 0x35:
        kind = AZAHAR_FAST_PROCESS_ID;
        break;
    case 0x52:
    case 0x53:
    case 0x54:
        kind = AZAHAR_FAST_CACHE;
        break;
    default:
        kind = AZAHAR_FAST_KINDS;
        break;
    }
    if ((insn & 0x0F000000u) != 0x0F000000u || cond == 0xF || kind == AZAHAR_FAST_KINDS) {
        svc_refused.insert(pc);
        return;
    }
    const auto room = ThunkRoom(SvcThunkMax);
    if (!room) {
        svc_refused.insert(pc);
        return;
    }
    const u32 base = *room;

    std::array<u32, SvcThunkMax / 4> buf{};
    u32 n = 0;
    const auto at = [&] { return base + n * 4; };
    const auto put = [&](u32 w) {
        ASSERT(n < buf.size());
        buf[n++] = w;
    };
    // ARM encodings, unconditional.
    const auto movw = [](u32 rd, u32 v) {
        return 0xE3000000u | (v >> 12 & 0xF) << 16 | rd << 12 | (v & 0xFFF);
    };
    const auto movt = [](u32 rd, u32 v) {
        return 0xE3400000u | (v >> 28 & 0xF) << 16 | rd << 12 | (v >> 16 & 0xFFF);
    };
    const auto ldr = [](u32 rt, u32 rn, s32 off) {
        return (off < 0 ? 0xE5100000u : 0xE5900000u) | rn << 16 | rt << 12 |
               static_cast<u32>(off < 0 ? -off : off);
    };
    const auto cmp = [](u32 rn, u32 rm) { return 0xE1500000u | rn << 16 | rm; };
    const auto tst = [](u32 rn, u32 imm8) { return 0xE3100000u | rn << 16 | imm8; };
    const auto umull = [](u32 lo, u32 hi, u32 rn, u32 rm) {
        return 0xE0800090u | hi << 16 | lo << 12 | rm << 8 | rn;
    };
    const auto adds = [](u32 rd, u32 rn, u32 rm) { return 0xE0900000u | rn << 16 | rd << 12 | rm; };
    std::vector<std::pair<u32, u32>> to_miss; // (word, condition)
    const auto branch_miss = [&](u32 c) {
        to_miss.emplace_back(n, c);
        put(0);
    };
    constexpr u32 EQ = 0x0, NE = 0x1;
    const auto page = [&] {
        put(movw(12, AZAHAR_USER_PAGE_VA & 0xFFFF));
        put(movt(12, AZAHAR_USER_PAGE_VA));
    };
    // r3 = the page's answers; the miss unless this one's bit is set.
    const auto answers = [&] {
        put(ldr(3, 12, offsetof(AzaharUserPage, on)));
        put(tst(3, 1u << kind));
        branch_miss(EQ);
    };
    // The miss unless register `reg` names `value` (r12 holds it).
    const auto handle = [&](u32 reg, u32 value) {
        put(movw(12, value & 0xFFFF));
        put(movt(12, value));
        put(cmp(reg, 12));
        branch_miss(NE);
    };

    SvcThunk t{};
    t.site = pc;
    put(0xE92D500Cu); // push {r2, r3, r12, lr}
    t.mrs = at();
    put(0xE10FE000u); // mrs lr, apsr
    switch (kind) {
    case AZAHAR_FAST_TICK: {
        // r1:r0 = base + (quantum - counter) * mult, the counter read between two reads of the
        // same gen (r2): azahar_svc_fast's computation.
        page();
        const u32 loop = at();
        put(ldr(2, 12, offsetof(AzaharUserPage, gen)));
        answers();
        put(ldr(3, 12,
                static_cast<s32>(AZAHAR_USER_TIMER_VA + 0x604) -
                    static_cast<s32>(AZAHAR_USER_PAGE_VA)));
        put(ldr(0, 12, offsetof(AzaharUserPage, quantum)));
        put(0xE0403003u); // sub r3, r0, r3: the timer ticks spent
        put(ldr(0, 12, offsetof(AzaharUserPage, mult_frac)));
        put(umull(0, 1, 3, 0)); // r1 = spent * frac >> 32
        put(ldr(0, 12, offsetof(AzaharUserPage, mult_int)));
        put(umull(0, 3, 3, 0)); // r3:r0 = spent * int
        put(adds(0, 0, 1));
        put(0xE2A33000u); // adc r3, r3, #0
        put(ldr(1, 12, offsetof(AzaharUserPage, base_lo)));
        put(adds(0, 0, 1));
        put(ldr(1, 12, offsetof(AzaharUserPage, base_hi)));
        put(0xE0A31001u); // adc r1, r3, r1
        put(ldr(3, 12, offsetof(AzaharUserPage, gen)));
        put(cmp(3, 2));
        const auto again = ArmBranch(NE, at(), loop);
        ASSERT(again.has_value());
        put(*again);
        t.answered = at();
        break;
    }
    case AZAHAR_FAST_THREAD_ID:
    case AZAHAR_FAST_PROCESS_ID:
        // r0 = 0, r1 = the ID, for the current thread (0xFFFF8000) or process (0xFFFF8001).
        handle(1, kind == AZAHAR_FAST_THREAD_ID ? 0xFFFF8000u : 0xFFFF8001u);
        page();
        answers();
        put(0xE3A00000u); // mov r0, #0
        put(ldr(1, 12,
                kind == AZAHAR_FAST_THREAD_ID ? offsetof(AzaharUserPage, thread_id)
                                              : offsetof(AzaharUserPage, process_id)));
        t.answered = at();
        break;
    default:
        // The process data cache svcs on the current process (r0 0xFFFF8001): success.
        handle(0, 0xFFFF8001u);
        page();
        answers();
        put(0xE3A00000u); // mov r0, #0
        t.answered = at();
        break;
    }
    // msr apsr_nzcvq, lr; pop {r2, r3, r12, lr}; b back
    t.done = at();
    put(0xE128F00Eu);
    t.pop = at();
    put(0xE8BD500Cu);
    const auto put_back = [&] {
        const auto b = ArmBranch(0xE, at(), pc + 4);
        if (b) {
            put(*b);
        }
        return b.has_value();
    };
    t.back = at();
    if (!put_back()) {
        svc_refused.insert(pc);
        return;
    }
    // miss: msr apsr_nzcvq, lr; pop {r2, r3, r12, lr}; the svc, its inputs untouched; b back
    t.miss = at();
    put(0xE128F00Eu);
    t.miss_pop = at();
    put(0xE8BD500Cu);
    t.miss_svc = at();
    put(0xEF000000u | number);
    t.miss_back = at();
    put_back();
    t.end = at();
    for (const auto& [word, c] : to_miss) {
        const auto b = ArmBranch(c, base + word * 4, t.miss);
        ASSERT(b.has_value());
        buf[word] = *b;
    }
    const auto site = ArmBranch(cond, pc, base);
    if (!site) {
        svc_refused.insert(pc);
        return;
    }

    std::memcpy(thunk_area->Data() + thunk_area_used, buf.data(), n * 4);
    map->SyncCode(base, n * 4);
    thunk_area_used += n * 4;
    svc_thunks[base] = t;
    memory.Write32(pc, *site);
    map->SyncCode(pc, 4);
    patched_sites[pc] = PatchedSite{insn, false};
#ifdef VITA_DIAGNOSTICS
    census.svc_thunks++;
#endif
}

void ARM_VitaNative::PatchUpcall(u32 pc) {
    if (InThunkArea(pc) || patched_sites.contains(pc) || svc_refused.contains(pc)) {
        return;
    }
    const u32 insn = memory.Read32(pc);
    const u32 cond = insn >> 28;
    if ((insn & 0x0F000000u) != 0x0F000000u || cond == 0xF) {
        return;
    }
    const u32 number = insn & 0x00FFFFFFu;
    if (number >= upcall_table.size()) {
        svc_refused.insert(pc);
        return;
    }
    const UpcallFn fn = upcall_table[number];
    constexpr u32 UpcallThunkMax = 160;
    const auto room = ThunkRoom(UpcallThunkMax);
    if (!room) {
        svc_refused.insert(pc);
        return;
    }
    const u32 base = *room;
    std::array<u32, UpcallThunkMax / 4> buf{};
    u32 n = 0;
    const auto at = [&] { return base + n * 4; };
    const auto put = [&](u32 w) {
        ASSERT(n < buf.size());
        buf[n++] = w;
    };
    const auto movw = [](u32 rd, u32 v) {
        return 0xE3000000u | (v >> 12 & 0xF) << 16 | rd << 12 | (v & 0xFFF);
    };
    const auto movt = [](u32 rd, u32 v) {
        return 0xE3400000u | (v >> 28 & 0xF) << 16 | rd << 12 | (v >> 16 & 0xFFF);
    };
    const u32 top =
        static_cast<u32>(reinterpret_cast<uintptr_t>(upcall_stack.data() + upcall_stack.size()));
    const u32 target = static_cast<u32>(reinterpret_cast<uintptr_t>(fn));
    put(0xE92D500Fu); // push {r0-r3, r12, lr}: the guest's stack
    put(0xE10FC000u); // mrs r12, apsr
    put(0xE52DC004u); // push {r12}
    put(movw(14, top));
    put(movt(14, top));
    put(0xE52ED008u); // str sp, [lr, #-8]!: the guest's SP at the app stack's top
    put(0xE1A0D00Eu); // mov sp, lr
    put(0xE92D00F0u); // push {r4-r7}: an svc's fifth to eighth inputs, the C++'s stack arguments
    put(movw(12, target));
    put(movt(12, target));
    put(0xE12FFF3Cu); // blx r12: the arguments in r0-r7; r0 the result, r1 UpcallStatus
    put(0xE59DD010u); // ldr sp, [sp, #16]
    put(0xE3510001u); // cmp r1, #1
    put(0xE49DC004u); // pop {r12}: the guest's flags
    const u32 to_busy = n;
    put(0); // bhi busy
    const u32 to_reschedule = n;
    put(0); // beq reschedule
    UpcallThunk t{};
    t.site = pc;
    std::array<u32, 2> backs{};
    // done: the result over the guest's r0; msr apsr_nzcvq, r12; pop {r0-r3, r12, lr}; b back
    put(0xE58D0000u);
    put(0xE128F00Cu);
    put(0xE8BD500Fu);
    backs[0] = n;
    put(0);
    // reschedule: the same, then svc UpcallRescheduleSvc
    const u32 reschedule = at();
    put(0xE58D0000u);
    put(0xE128F00Cu);
    put(0xE8BD500Fu);
    t.svc_reschedule = at();
    put(0xEF000000u | UpcallRescheduleSvc);
    backs[1] = n;
    put(0);
    // busy: msr apsr_nzcvq, r12; pop {r0-r3, r12, lr}; the original svc; b back
    const u32 busy = at();
    put(0xE128F00Cu);
    put(0xE8BD500Fu);
    t.svc_original = at();
    put(0xEF000000u | number);
    const u32 last_back = n;
    put(0);
    t.end = at();
    for (const u32 word : {backs[0], backs[1], last_back}) {
        const auto b = ArmBranch(0xE, base + word * 4, pc + 4);
        if (!b) {
            svc_refused.insert(pc);
            return;
        }
        buf[word] = *b;
    }
    const auto b_busy = ArmBranch(0x8, base + to_busy * 4, busy);                // HI
    const auto b_resched = ArmBranch(0x0, base + to_reschedule * 4, reschedule); // EQ
    const auto site = ArmBranch(cond, pc, base);
    if (!b_busy || !b_resched || !site) {
        svc_refused.insert(pc);
        return;
    }
    buf[to_busy] = *b_busy;
    buf[to_reschedule] = *b_resched;
    std::memcpy(thunk_area->Data() + thunk_area_used, buf.data(), n * 4);
    map->SyncCode(base, n * 4);
    thunk_area_used += n * 4;
    upcall_thunks[base] = t;
    memory.Write32(pc, *site);
    map->SyncCode(pc, 4);
    patched_sites[pc] = PatchedSite{insn, false};
    LOG_INFO(Core_ARM11, "upcall: svc {:#x} at {:08X} -> thunk {:08X}, C++ {:08X}, stack {:08X}",
             insn & 0x00FFFFFFu, pc, base, target, top);
#ifdef VITA_DIAGNOSTICS
    census.upcall_sites++;
#endif
}

u64 ARM_VitaNative::UpcallSvc(u32 svc, const std::array<u32, 8>& in) {
    ARM_VitaNative& core = *upcall_core;
#ifdef VITA_DIAGNOSTICS
    census.upcall_runs++;
    census.upcall_svcs[svc & 0xff]++;
#endif
    // The svc's register file for the HLE kernel's handler (SVC::GetReg reads the running
    // core's): r0-r7 as they came, r12 and LR from the thunk's frame, SP the guest's at the svc.
    u32* const frame = UpcallGuestRegs();
    for (u32 i = 0; i < in.size(); i++) {
        core.context.r[i] = in[i];
    }
    core.context.r[12] = frame[4];
    core.context.r[13] = static_cast<u32>(reinterpret_cast<uintptr_t>(frame + 6));
    core.context.r[14] = frame[5];
    auto& kernel = upcall_system->Kernel();
    const Kernel::Thread* const thread = kernel.GetCurrentThreadManager().GetCurrentThread();
    upcall_reschedule_asked.store(false, std::memory_order_relaxed);
    core.svc_context.CallSVCForUpcall(svc);
    // The outputs: r0 as the result, r1-r3 into the frame the thunk's tail pops. The thunk keeps
    // r4-r7 (callee-saved), so an svc must not write them.
    for (u32 i = 1; i < 4; i++) {
        frame[i] = core.context.r[i];
    }
    for (u32 i = 4; i < in.size(); i++) {
        DEBUG_ASSERT_MSG(core.context.r[i] == in[i], "upcall: svc {:#x} wrote r{}", svc, i);
    }
    // The thread stopped running, or one more urgent may run: the scheduler's, after an exit.
    const bool reschedule = upcall_reschedule_asked.load(std::memory_order_relaxed) ||
                            thread == nullptr || thread->status != Kernel::ThreadStatus::Running ||
                            kernel.GetCurrentThreadManager().GetCurrentThread() != thread;
#ifdef VITA_DIAGNOSTICS
    census.upcall_reschedules += reschedule;
#endif
    return UpcallAnswer(core.context.r[0], reschedule ? UpcallReschedule : UpcallDone);
}

u32 ARM_VitaNative::FetchCode(u32 pc, bool thumb) const {
    if (InThunkArea(pc) && thunk_area) {
        const u8* const p = thunk_area->Data() + (pc - ThunkAreaBase);
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
    if (patched_sites.empty()) {
        return;
    }
    // Every thread that stands in a thunk leaves it (FinishVfpThunk): each core's running one,
    // through the core, and every other one through its saved context.
    for (u32 i = 0; i < system.GetNumCores(); i++) {
        ARM_Interface& core = system.GetCore(i);
        if (!InThunkArea(core.GetPC())) {
            continue;
        }
        ThreadContext t;
        core.SaveContext(t);
        AzaharContext c{};
        ToAzahar(t, c);
        FinishThunk(memory, c);
        FromAzahar(c, t);
        core.LoadContext(t);
    }
    auto& kernel = system.Kernel();
    for (u32 i = 0; i < system.GetNumCores(); i++) {
        auto& manager = kernel.GetThreadManager(i);
        const Kernel::Thread* const running = manager.GetCurrentThread();
        for (const auto& thread : manager.GetThreadList()) {
            if (thread.get() == running || !InThunkArea(thread->context.cpu_registers[15] & ~1u)) {
                continue;
            }
            AzaharContext c{};
            ToAzahar(thread->context, c);
            FinishThunk(memory, c);
            FromAzahar(c, thread->context);
        }
    }
    // The guest's own instructions back; each traps and is patched again at its next run.
    for (const auto& [pc, site] : patched_sites) {
        if (site.thumb) {
            memory.Write16(pc, static_cast<u16>(site.insn >> 16));
            memory.Write16(pc + 2, static_cast<u16>(site.insn));
        } else {
            memory.Write32(pc, site.insn);
        }
        map->SyncCode(pc, 4);
    }
    LOG_INFO(Core_ARM11,
             "thunks: {} patched sites restored for the savestate ({} VFP thunks, {} svc thunks)",
             patched_sites.size(), vfp_thunks.size(), svc_thunks.size());
    patched_sites.clear();
    vfp_thunks.clear();
    svc_thunks.clear();
    svc_refused.clear();
    upcall_thunks.clear();
    thunk_area_used = 0;
}

bool ARM_VitaNative::RunSlice(u64 quantum_ns) {
    // A slice that ended inside an upcall runs the same thread on at once, a minimal quantum at a
    // time, until it is back in its own code (RunSliceOnce sets upcall_again).
    for (;;) {
        upcall_again = false;
        const bool go_on = RunSliceOnce(quantum_ns);
        if (!upcall_again) {
            return go_on;
        }
        quantum_ns = MinQuantumNs;
    }
}

bool ARM_VitaNative::RunSliceOnce(u64 quantum_ns) {
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

    // The last slice's request: every input is set again here.
    run.ctx.tpidruro = cp15[CP15_THREAD_URO];
    run.ctx.tpidrurw = cp15[CP15_THREAD_UPRW];
    run.quantum_us = static_cast<u32>(quantum_ns / 1000);
    // The svcs the module answers without an exit (none of their answers change before one).
    run.fast = AZAHAR_FAST_ALL;
    auto& kernel = system.Kernel();
    if (const auto* thread = kernel.GetCurrentThreadManager().GetCurrentThread()) {
        run.thread_id = thread->GetThreadId();
    } else {
        run.thread_id = 0;
        run.fast &= ~(1u << AZAHAR_FAST_THREAD_ID);
    }
    if (const auto process = kernel.GetCurrentProcess()) {
        run.process_id = process->process_id;
    } else {
        run.process_id = 0;
        run.fast &= ~(1u << AZAHAR_FAST_PROCESS_ID);
    }
    run.tick_hz = static_cast<u64>(BASE_CLOCK_RATE_ARM11 * timer->GetClockScale() + 0.5);
    run.tick_base = timer->GetTicks();

    // In-kernel scheduling: the running thread's process's ready and sleeping threads, up to the
    // next event that is not one of their wakeups (at most a slice's length on). The clock must
    // be unscaled: the module converts a sleep's nanoseconds as nsToCycles does.
    run.sched = nullptr;
    run.sched_count = 0;
    run.sched_current = 0;
    run.sched_deadline = 0;
    run.sched_arbiter = 0;
    bool scheduled = false;
    int current_before = -1;
    const auto* const running = kernel.GetCurrentThreadManager().GetCurrentThread();
    if (!UpcallsOn() && timer->GetClockScale() == 1.0 &&
        running != nullptr && sched_waiters.contains(running->thread_id)) {
        auto& threads = kernel.GetCurrentThreadManager();
        // The DSP's interrupt wakes the guest's event waiters from its own thread, under the HLE
        // lock: the ready queue and the threads' states are read here, and rewritten at the
        // import, under it too.
        std::scoped_lock hle_lock{kernel.GetHLELock()};
        sched_arbiter = sched_arbiter_handle != 0
                            ? kernel.GetCurrentProcess()->handle_table.Get<Kernel::AddressArbiter>(
                                  sched_arbiter_handle)
                            : nullptr;
        const int current =
            threads.ExportNativeSchedule(sched_before, AZAHAR_SCHED_MAX, sched_arbiter.get());
        if (current >= 0) {
            const s64 base = static_cast<s64>(run.tick_base);
            const auto* const wakeup = threads.WakeupEventType();
            const s64 deadline = timer->NextEventTime(
                base + Timing::MAX_SLICE_LENGTH, [&](const TimingEventType* type, uintptr_t id) {
                    // The sleepers' and the timed arbitration waits' wakes are the module's.
                    return type == wakeup &&
                           std::any_of(
                               sched_before.begin(), sched_before.end(), [&](const auto& e) {
                                   return (e.state == AZAHAR_SCHED_SLEEP ||
                                           (e.state == AZAHAR_SCHED_ARB && e.wake != ~0ull)) &&
                                          e.thread->thread_id == id;
                               });
                });
            if (deadline > base) {
                for (std::size_t i = 0; i < sched_before.size(); i++) {
                    const auto& e = sched_before[i];
                    sched_table[i] = AzaharSchedThread{e.thread->thread_id,
                                                       static_cast<u32>(e.thread->current_priority),
                                                       e.state,
                                                       e.order,
                                                       e.wake,
                                                       e.thread->GetTLSAddress(),
                                                       0,
                                                       AsAzahar(e.thread->context),
                                                       e.address};
                    // The module loads it as it is (a thread that never ran has its entry's
                    // Thumb bit in the PC).
                    e.thread->context.cpu_registers[15] &= ~1u;
                }
                run.sched = sched_table.data();
                run.sched_count = static_cast<u32>(sched_before.size());
                run.sched_current = current;
                run.sched_deadline = static_cast<u64>(deadline);
                run.sched_arbiter = sched_arbiter ? sched_arbiter_handle : 0;
                current_before = current;
                scheduled = true;
            }
        }
    }

    upcall_core = this;
    const int ret = azaharRun(&run);
    if (ret < 0) {
        system.SetStatus(System::ResultStatus::ErrorCoreExceptionRaised,
                         fmt::format("azaharRun failed: {}", ret).c_str());
        usable = false;
        return false;
    }

    cp15[CP15_THREAD_UPRW] = run.ctx.tpidrurw;
    if (run.fast_site != 0) {
        PatchSvc(run.fast_site - 4);
    }
    if (UpcallsOn() && InUpcall(context.r[15]) &&
        !(run.reason == AZAHAR_EXIT_SVC && IsUpcallTailSvc(context.r[15] - 4))) {
        // The words on the upcall stack that point into the app's code (0x81000000 up), deepest
        // first: the C++ path's return addresses, for addr2line on the run's ELF.
        const auto returns = [&] {
            std::string list;
            const u32 stack_lo = static_cast<u32>(reinterpret_cast<uintptr_t>(upcall_stack.data()));
            const u32 stack_hi = stack_lo + static_cast<u32>(upcall_stack.size());
            if (context.r[13] >= stack_lo && context.r[13] < stack_hi) {
                for (u32 a = context.r[13] & ~3u; a < stack_hi && list.size() < 400; a += 4) {
                    u32 w;
                    std::memcpy(&w, upcall_stack.data() + (a - stack_lo), 4);
                    if (w >= 0x81000000u && w < 0x82000000u) {
                        list += fmt::format(" {:08X}", w);
                    }
                }
            }
            return list;
        };
        // A Sony syscall from the app's code in the guest's world: made in Sony's, then the same
        // thread on. Each is logged before and after: one that never answers is the last line.
        if (run.reason == AZAHAR_EXIT_SVC && run.svc_number == 0 && context.r[15] >= 0x40000000u) {
            const u32 number = context.r[12];
            LOG_INFO(Core_ARM11, "upcall: Sony syscall {:#x} from {:08X} (lr {:08X}); path:{}",
                     number, context.r[15], context.r[14], returns());
            ForwardSonySyscall(run.ctx);
            LOG_INFO(Core_ARM11, "upcall: Sony syscall {:#x} answered {:08X}", number,
                     context.r[0]);
            timer->AddScaledTicks(run.guest_ticks);
#ifdef VITA_DIAGNOSTICS
            census.upcall_syscalls++;
#endif
            upcall_again = true;
            return true;
        }
        // The experiment's observation: anything but a quantum's end inside an upcall.
        if (run.reason != AZAHAR_EXIT_TIMER) {
            ASSERT_MSG(false,
                       "upcall: {} at {:08X} (svc {:#x}, lr {:08X}, sp {:08X}, fsr {:08X}, far "
                       "{:08X}); app code on the upcall stack:{}",
                       ReasonName(run.reason), context.r[15], run.svc_number, context.r[14],
                       context.r[13], run.fsr, run.far, returns());
        }
        // An upcall that does not come back (a spin on a lock another thread holds) runs one
        // quantum after another: logged at a quarter second, stopped at five.
        upcall_quantums++;
        if (upcall_quantums == 1000) {
            LOG_WARNING(Core_ARM11, "upcall: 1000 quantums in one, pc {:08X} lr {:08X}; path:{}",
                        context.r[15], context.r[14], returns());
        }
        ASSERT_MSG(upcall_quantums < 20000,
                   "upcall: 20000 quantums in one, pc {:08X} lr {:08X}; app code on the upcall "
                   "stack:{}",
                   context.r[15], context.r[14], returns());
        timer->AddScaledTicks(run.guest_ticks);
#ifdef VITA_DIAGNOSTICS
        census.upcall_preempted++;
#endif
        upcall_again = true; // the same thread on, until it is back in its own code
        return true;
    }
    upcall_quantums = 0;
    // The guest ticks the module counted, converted as its GetSystemTick answers were: the clock ends
    // at or past every value the guest read in the slice.
    timer->AddScaledTicks(run.guest_ticks);

    // The module's scheduling taken back (the threads that stopped running saved their contexts
    // in place), the one running now's TLS; with none running the slice loop ends and RunLoop
    // idles to its next event.
    if (scheduled) {
        sched_after = sched_before;
        for (std::size_t i = 0; i < sched_after.size(); i++) {
            const AzaharSchedThread& t = sched_table[i];
            sched_after[i].state = t.state;
            sched_after[i].order = t.order;
            sched_after[i].wake = t.wake;
            sched_after[i].address = t.address;
            if (t.saved) {
                // A thread the module put to sleep, or to wait at the arbiter, stopped there.
                if (t.state == AZAHAR_SCHED_SLEEP || t.state == AZAHAR_SCHED_ARB) {
                    sched_waiters.insert(t.id);
                }
            }
        }
        cp15[CP15_THREAD_URO] = run.ctx.tpidruro;
#ifdef VITA_DIAGNOSTICS
        static const bool trace_arb = SgxdRenderer::SgxdFlag("perflog");
        if (trace_arb && run.sched_arbitrations != 0 && census.arb_traces.size() < 16) {
            std::string trace = fmt::format(
                "arbiter {:#x}, current {} -> {}, exit {} svc {:#x}:", run.sched_arbiter,
                current_before, run.sched_current, ReasonName(run.reason), run.svc_number);
            for (std::size_t i = 0; i < sched_before.size(); i++) {
                const auto& b = sched_before[i];
                const auto& a = sched_after[i];
                trace += fmt::format(" [{} {}->{} {:08x}->{:08x} o{}->{} w{:x}->{:x} r0 {:08x}]",
                                     b.thread->thread_id, b.state, a.state, b.address, a.address,
                                     b.order, a.order, b.wake, a.wake,
                                     b.thread->context.cpu_registers[0]);
            }
            census.arb_traces.push_back(std::move(trace));
        }
#endif
        {
            std::scoped_lock hle_lock{kernel.GetHLELock()};
            kernel.GetCurrentThreadManager().ImportNativeSchedule(
                sched_before, sched_after, run.sched_current, timer->GetTicks(),
                sched_arbiter.get());
        }
#ifdef VITA_DIAGNOSTICS
        census.sched_slices++;
        census.sched_switches += run.sched_switches;
        census.sched_sleeps += run.sched_sleeps;
        census.sched_idle += run.sched_idle;
        census.sched_arbitrations += run.sched_arbitrations;
#endif
        if (run.sched_current < 0) {
            return false;
        }
        if (run.reason == AZAHAR_EXIT_SVC && run.sched_handled) {
            return !break_flag && !stop_requested;
        }
    }

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
    }
#endif

    // Whether this thread's slices carry the table from now on: a SleepThread or ArbitrateAddress
    // exit says yes (the latter's arbiter the one the module answers), any other svc exit no.
    if (run.reason == AZAHAR_EXIT_SVC) {
        if (const auto* const t = kernel.GetCurrentThreadManager().GetCurrentThread()) {
            if (run.svc_number == 0x0A || run.svc_number == 0x22) {
                sched_waiters.insert(t->thread_id);
            } else {
                sched_waiters.erase(t->thread_id);
            }
            if (run.svc_number == 0x22) {
                sched_arbiter_handle = run.ctx.r[0];
            }
        }
    }

    switch (run.reason) {
    case AZAHAR_EXIT_SVC:
        // An upcall asked for a reschedule: this exit is for it alone.
        if (run.svc_number == UpcallRescheduleSvc && UpcallsOn() &&
            IsUpcallTailSvc(context.r[15] - 4)) {
            return false;
        }
        // A patched VFP instruction the module did not run: run here. One with no record is from a
        // savestate made before the restore at each save, and is lost.
        if (run.svc_number == AZAHAR_VFP_SVC) {
            const bool thumb = run.ctx.cpsr >> 5 & 1;
            const u32 site = run.ctx.r[15] - (thumb ? 2u : 4u);
            const auto it = patched_sites.find(site);
            ASSERT_MSG(it != patched_sites.end(),
                       "svc {:#x} at {:08X} with no patched VFP instruction behind it (a savestate "
                       "from a build that saved the module's svcs)",
                       AZAHAR_VFP_SVC, site);
            [[maybe_unused]] const bool ran = RunVfpVector(run.ctx, it->second.insn);
            DEBUG_ASSERT_MSG(ran, "a patched VFP instruction at {:08X} did not run", it->first);
            return !break_flag && !stop_requested;
        }
        if (UpcallFor(run.svc_number) && (context.cpsr & 0x20) == 0) {
            PatchUpcall(context.r[15] - 4);
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
        ReportFault();
        return false;
    }
    case AZAHAR_EXIT_UND: {
        // VFP instructions under the guest's short vectors (RunVfpVector), ARM or Thumb-2: the trapped
        // one and the data-processing ones after it run here.
        if ((run.ctx.fpscr & FpscrVectorBits) != 0) {
            u32 pc = run.ctx.r[15];
            const bool thumb = run.ctx.cpsr >> 5 & 1;
            // Run and patched as if unconditional: an IT block's condition would be lost.
            ASSERT_MSG(!thumb || (run.ctx.cpsr & 0x0600FC00) == 0,
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
                    !(!ConditionPasses(cond, run.ctx.cpsr) || RunVfpVector(run.ctx, insn))) {
                    break;
                }
                PatchVfp(pc, insn, thumb);
                pc += 4;
            }
            if (ran > 0) {
                // The patched instructions past the guest's instruction cache.
                map->SyncCode(run.ctx.r[15], pc - run.ctx.r[15]);
                run.ctx.r[15] = pc;
                return !break_flag && !stop_requested;
            }
        }
        ReportFault();
        return false;
    }
    case AZAHAR_EXIT_PABT:
    default:
        ReportFault();
        return false;
    }
}

#ifdef VITA_DIAGNOSTICS
VitaNativeCensus TakeVitaNativeCensus() {
    return std::exchange(census, VitaNativeCensus{});
}
#endif

void SetVitaInKernelScheduling(bool on) {
    in_kernel_scheduling = on;
}

void ARM_VitaNative::ReportFault() {
    std::string registers;
    for (int i = 0; i < 16; i++) {
        registers += fmt::format("r{:02d} = {:08X}\n", i, run.ctx.r[i]);
    }
    const std::string message =
        fmt::format("{}Guest {} at PC {:08X} (cpsr {:08X}, fpscr {:08X}, fpexc {:08X}), fsr {:08X} "
                    "far {:08X}",
                    registers, ReasonName(run.reason), run.ctx.r[15], run.ctx.cpsr, run.ctx.fpscr,
                    fpexc, run.fsr, run.far);
    LOG_CRITICAL(Core_ARM11, "{}", message);
    std::string code;
    for (u32 a = run.ctx.r[15] - 16; a <= run.ctx.r[15] + 12; a += 4) {
        code += fmt::format("{}{:08X}", a == run.ctx.r[15] ? " *" : " ",
                            InThunkArea(a) ? FetchCode(a, false) : memory.Read32(a));
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
    // How the module's tables map the PC and the fault address on each core.
    if (azaharVersion() >= 87) {
        const auto tables = [](u32 va) {
            AzaharVaReport t{};
            azaharVaReport(va, &t);
            std::string cores;
            for (u32 n = 0; n < 4; n++) {
                cores += fmt::format("; core {}: attached {}, view {:08X}/{:08X}, {} overrides", n,
                                     t.attached[n], t.view_l1[n], t.view_l2[n],
                                     t.view_overrides[n]);
            }
            LOG_CRITICAL(Core_ARM11,
                         "tables at {:08X} from core {} (azaharMap's {}): shared {:08X}/{:08X}{}; "
                         "failed view builds {} (the last {:08X} on core {})",
                         va, t.core, t.map_core, t.shared_l1, t.shared_l2, cores, t.view_failures,
                         static_cast<u32>(t.view_last_error), t.view_last_core);
            std::string overrides;
            for (u32 i = 0; i < t.view_overrides[t.core & 3] && i < AZAHAR_ATTACH_OVERRIDES; i++) {
                const AzaharRange& r = t.overrides[i];
                overrides += fmt::format(" {:08X}+{:X} <- {:08X} ({:X})", r.guest_va, r.size,
                                         r.user_va, r.perm);
            }
            LOG_CRITICAL(Core_ARM11, "core {}'s view overrides:{}", t.core,
                         overrides.empty() ? std::string{" none"} : overrides);
        };
        tables(run.ctx.r[15]);
        if (run.far != run.ctx.r[15]) {
            tables(run.far);
        }
    }
    // Where the guest came from: the code before LR and the stack from SP.
    const u32 lr = run.ctx.r[14] & ~1u;
    std::string call;
    for (u32 a = lr - 16; a < lr + 8; a += 4) {
        call += fmt::format("{}{:08X}", a == (lr & ~3u) ? " *" : " ", memory.Read32(a));
    }
    LOG_CRITICAL(Core_ARM11, "code around LR {:08X}:{}", run.ctx.r[14], call);
    for (u32 row = 0; row < 8; row++) {
        std::string words;
        for (u32 w = 0; w < 8; w++) {
            words += fmt::format(" {:08X}", memory.Read32(run.ctx.r[13] + (row * 8 + w) * 4));
        }
        LOG_CRITICAL(Core_ARM11, "stack {:08X}:{}", run.ctx.r[13] + row * 32, words);
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
    run.ctx.r[15] = pc & ~1u;
}

u32 ARM_VitaNative::GetPC() const {
    return run.ctx.r[15] & ~1u;
}

u32 ARM_VitaNative::GetReg(int index) const {
    ASSERT(index >= 0 && index < 16);
    return index == 15 ? GetPC() : run.ctx.r[index];
}

void ARM_VitaNative::SetReg(int index, u32 value) {
    ASSERT(index >= 0 && index < 16);
    if (index == 15) {
        SetPC(value);
    } else {
        run.ctx.r[index] = value;
    }
}

u32 ARM_VitaNative::GetVFPReg(int index) const {
    ASSERT(index >= 0 && index < 64);
    return run.ctx.fpregs[index];
}

void ARM_VitaNative::SetVFPReg(int index, u32 value) {
    ASSERT(index >= 0 && index < 64);
    run.ctx.fpregs[index] = value;
}

u32 ARM_VitaNative::GetVFPSystemReg(VFPSystemRegister reg) const {
    switch (reg) {
    case VFP_FPSCR:
        return run.ctx.fpscr;
    case VFP_FPEXC:
        return fpexc;
    default:
        UNREACHABLE_MSG("Unknown VFP system register: {}", reg);
    }
}

void ARM_VitaNative::SetVFPSystemReg(VFPSystemRegister reg, u32 value) {
    switch (reg) {
    case VFP_FPSCR:
        run.ctx.fpscr = value;
        return;
    case VFP_FPEXC:
        fpexc = value;
        return;
    default:
        UNREACHABLE_MSG("Unknown VFP system register: {}", reg);
    }
}

u32 ARM_VitaNative::GetCPSR() const {
    return run.ctx.cpsr;
}

void ARM_VitaNative::SetCPSR(u32 cpsr) {
    run.ctx.cpsr = cpsr;
}

u32 ARM_VitaNative::GetCP15Register(CP15Register reg) const {
    return cp15[reg];
}

void ARM_VitaNative::SetCP15Register(CP15Register reg, u32 value) {
    cp15[reg] = value;
}

void ARM_VitaNative::SaveContext(ThreadContext& ctx) {
    FromAzahar(run.ctx, ctx);
    ctx.cpu_registers[15] = GetPC();
    ctx.fpexc = fpexc;
}

void ARM_VitaNative::LoadContext(const ThreadContext& ctx) {
    ToAzahar(ctx, run.ctx);
    fpexc = ctx.fpexc;
}

void ARM_VitaNative::PrepareReschedule() {
    // From the HLE while the guest is stopped at an exit, or from another thread while it runs
    // (bounded by the quantum). An upcall takes an exit for it.
    stop_requested = true;
    upcall_reschedule_asked.store(true, std::memory_order_relaxed);
}

} // namespace Core
