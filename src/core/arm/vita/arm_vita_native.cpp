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

// The module loads a thread from its AzaharContext in place; fpexc follows it.
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

/// Every guest page is mapped RWX; the process's own permissions are not tracked.
constexpr u32 GuestPerm = AZAHAR_PERM_R | AZAHAR_PERM_W | AZAHAR_PERM_X;

/// The core the module takes; the emulation thread must not be pinned there.
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

/// Luma's alias of FCRAM: a physical address (0x2xxxxxxx) with bit 31 set.
constexpr bool IsLumaAlias(u32 address) {
    return address >> 28 == 0xAu;
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
// GetThreadId's thunk answers for another handle too, from the IDs the svc gave before (ThreadIds).

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
    /// GetSystemTick's only: the idle skip's exit and resume points, and the callers' counters.
    u32 idle, idle_pop, idle_b;
    u32 answer, answer_mrs, answer_b;
    u32 data;
    u32 skip; ///< the idle skip's upcall stub, 0 until a caller spins on the clock
    u32 end;
};
/// The largest svc thunk: GetSystemTick's 71 words.
constexpr u32 SvcThunkMax = 296;

/// The guest's patched sites and thunks; the svc sites PatchSvc could not patch.
std::unordered_map<u32, PatchedSite> patched_sites;
std::map<u32, VfpThunk> vfp_thunks;
std::map<u32, SvcThunk> svc_thunks;
/// An upcall thunk: its entry stub, its end, and where the guest's moved code in it starts.
struct UpcallThunk {
    u32 site;
    u32 end;
    u32 moved;
};
std::map<u32, UpcallThunk> upcall_thunks;
/// The upcall thunks' shared body (EnsureUpcallBody) and the PC after its AZAHAR_SWITCH_SVC; 0
/// for none.
u32 upcall_body{}, upcall_after_svc{};
/// The svc redirect's record: the module returns an svc it does not answer into `entry`.
AzaharRedirect upcall_redirect{};
std::unordered_set<u32> svc_refused;
/// A GetSystemTick thunk's counters for the idle loops (CheckTickPolls): eight caller slots with
/// their calls, then the caller sent to the idle skip.
constexpr u32 TickSlots = 8;
constexpr u32 TickSkipWord = 2 * TickSlots;
constexpr u32 TickDataWords = TickSkipWord + 1;
struct TickWindow {
    u32 caller{};
    u32 calls{};
    u64 since{};
};
struct TickThunk {
    u32 base;
    u32 site;
    u32 data;
    std::array<TickWindow, TickSlots> window{};
};
std::vector<TickThunk> tick_thunks;
/// A caller reading the clock this often in a guest vblank is spinning on it.
constexpr u64 IdleReadsPerVblank = 1000;
std::unique_ptr<Common::HostSharedMemory> thunk_area;
const VitaGuestMap* thunk_area_map{};
u32 thunk_area_used{};

/// The mutex leases at the thunk area's end: the owning process's ID, then by handle slot
/// (handle >> 15) the handle and its lease word's address, then the words (MutexFastPath).
constexpr u32 LeaseSlots = 4096;
constexpr u32 LeaseTableOffset = ThunkAreaSize - LeaseSlots * 12 - 16;
constexpr u32 LeaseEntriesOffset = LeaseTableOffset + 16;
constexpr u32 LeaseWordsOffset = ThunkAreaSize - LeaseSlots * 4;
constexpr u32 NoLeaseProcess = 0xFFFFFFFF;
struct LeaseEntry {
    u32 handle;
    u32 word;
};
/// Below the leases, the same process's thread IDs by handle slot, for GetThreadId's thunk: the ID
/// << 15 | the handle's generation, 0 for none (no handle has generation 0).
constexpr u32 ThreadIdsOffset = LeaseTableOffset - LeaseSlots * 4;
constexpr u32 ThreadIdShift = 15;
/// Each word's mutex, the entry slot + 1 that names it (0: none), and the free words.
std::array<Kernel::Mutex*, LeaseSlots> lease_owner{};
std::array<u16, LeaseSlots> lease_entry_of{};
std::vector<u16> lease_free;
u32& LeaseProcess() {
    return *reinterpret_cast<u32*>(thunk_area->Data() + LeaseTableOffset);
}
LeaseEntry* LeaseEntries() {
    return reinterpret_cast<LeaseEntry*>(thunk_area->Data() + LeaseEntriesOffset);
}
u32* LeaseWords() {
    return reinterpret_cast<u32*>(thunk_area->Data() + LeaseWordsOffset);
}
u32* ThreadIds() {
    return reinterpret_cast<u32*>(thunk_area->Data() + ThreadIdsOffset);
}
u32 LeaseIndex(const u32* word) {
    const auto i = static_cast<u32>(word - LeaseWords());
    ASSERT(i < LeaseSlots);
    return i;
}

/// Every lease dropped (the guest is gone, or the area new): the mutexes are the kernel's alone.
void ResetLeases() {
    for (Kernel::Mutex*& mutex : lease_owner) {
        if (mutex != nullptr) {
            mutex->lease = nullptr;
            mutex = nullptr;
        }
    }
    lease_entry_of.fill(0);
    lease_free.clear();
    for (u32 i = LeaseSlots; i-- > 0;) {
        lease_free.push_back(static_cast<u16>(i));
    }
    std::memset(thunk_area->Data() + ThreadIdsOffset, 0, ThunkAreaSize - ThreadIdsOffset);
    LeaseProcess() = NoLeaseProcess;
}

/// Host 1's own thunk area: host 1 builds each thunk host 0 published (ClaimThunk) the first time
/// it runs into it; until then the place traps.
constexpr u32 Host1ThunkSize = ThreadIdsOffset & ~0xFFFu;
constexpr u32 ThunkTrapSvc = 0xF9;
constexpr u32 ThunkTrapArm = 0xEF000000u | ThunkTrapSvc;
constexpr u16 ThunkTrapThumb = 0xDF00u | ThunkTrapSvc;
std::unique_ptr<Common::HostSharedMemory> host1_area;
/// Host 1's area holds thunks of a guest or area that is gone: reset before the next thunk.
bool host1_area_stale = true;
/// Host 1 ran in this area since its reset (PrepareSaveState).
bool host1_area_used = false;
/// A thunk's site: its entry, the site, the instruction(s) the branch replaced (Thumb's halfwords
/// high first, PatchedSite's), whether Thumb, and what host 1 builds: the svc or VFP instruction
/// and a branch back, or a mutex svc's lock or unlock on the shared leases first (MutexFastPath,
/// its entry 20 bytes into the room).
enum class ClaimKind : u8 { Svc, Vfp, Mutex };
struct ThunkClaim {
    u32 entry;
    u32 site;
    u32 insn;
    bool thumb;
    ClaimKind kind;
};
constexpr u32 MaxThunkClaims = 8192;
/// By entry, ascending (ThunkRoom's addresses rise); `thunk_claims` of them published.
std::array<ThunkClaim, MaxThunkClaims> thunk_claim;
std::atomic<u32> thunk_claims{};

/// Host 1's area: traps throughout, out to memory for its instruction fetches.
void ResetHost1Area(const VitaGuestMap& map) {
    thunk_claims.store(0, std::memory_order_relaxed);
    if (!host1_area) {
        host1_area = std::make_unique<Common::HostSharedMemory>(Host1ThunkSize, "azahar-thunks-1");
        ASSERT_MSG(host1_area->Data() != nullptr &&
                       (reinterpret_cast<uintptr_t>(host1_area->Data()) & 0xFFFu) == 0,
                   "host 1's thunk area's {} KiB", Host1ThunkSize >> 10);
    }
    u32* const words = reinterpret_cast<u32*>(host1_area->Data());
    std::fill(words, words + Host1ThunkSize / 4, ThunkTrapArm);
    map.SyncViewCode(host1_area->Data(), ThunkAreaBase, Host1ThunkSize);
    host1_area_used = false;
}

/// Host 0 publishes a thunk before branching its site to it; a Thumb thunk's trap goes in now.
void ClaimThunk(const VitaGuestMap& map, u32 entry, u32 site, u32 insn, bool thumb,
                ClaimKind kind) {
    const u32 n = thunk_claims.load(std::memory_order_relaxed);
    ASSERT_MSG(n < MaxThunkClaims, "{} thunk claims", n);
    DEBUG_ASSERT(n == 0 || thunk_claim[n - 1].entry < entry);
    thunk_claim[n] = ThunkClaim{entry, site, insn, thumb, kind};
    if (thumb) {
        const u32 off = entry - ThunkAreaBase;
        std::memcpy(host1_area->Data() + off, &ThunkTrapThumb, 2);
        map.SyncViewCode(host1_area->Data() + (off & ~0xFFFu), ThunkAreaBase + (off & ~0xFFFu),
                         0x1000);
    }
    thunk_claims.store(n + 1, std::memory_order_release);
}

std::optional<std::vector<u32>> MutexFastPath(u32 svc, u32 base, u32 resume);
std::optional<u32> ArmBranch(u32 cond, u32 from, u32 to);

/// Host 1's version of a mutex claim: the lock or unlock on the lease, then its svc (at
/// `svc_at`) and the branch back, from the room's start (`base`).
struct Host1Mutex {
    u32 base;
    u32 svc_at;
    std::vector<u32> words;
};
Host1Mutex Host1MutexThunk(const ThunkClaim& c) {
    const u32 base = c.entry - 20;
    const u32 number = c.insn & 0x00FFFFFFu;
    auto words = MutexFastPath(number, base, c.site + 4);
    ASSERT(words.has_value());
    const u32 svc_at = base + static_cast<u32>(words->size()) * 4;
    words->push_back(0xEF000000u | number);
    words->push_back(*ArmBranch(0xE, svc_at + 4, c.site + 4));
    return Host1Mutex{base, svc_at, std::move(*words)};
}

/// The claim whose thunk holds `pc` in host 1's version, or nullptr.
const ThunkClaim* Host1ClaimAt(u32 pc) {
    const u32 n = thunk_claims.load(std::memory_order_acquire);
    const auto it = std::upper_bound(thunk_claim.begin(), thunk_claim.begin() + n, pc,
                                     [](u32 v, const ThunkClaim& c) { return v < c.entry; });
    if (it == thunk_claim.begin()) {
        return nullptr;
    }
    const ThunkClaim& c = *std::prev(it);
    const u32 end = c.kind == ClaimKind::Mutex ? Host1MutexThunk(c).svc_at + 8 : c.entry + 8;
    return pc < end ? &c : nullptr;
}

/// Host 1's version of a claim's thunk, in host 1's area at the claim's entry.
void WriteHost1Thunk(const ThunkClaim& claim, const VitaGuestMap& map);

/// Whether host 1's thread at `pc` is in a mutex thunk before its svc, its registers in the
/// thunk's shared slots.
bool InHost1MutexSlots(u32 pc) {
    if (!InThunkArea(pc)) {
        return false;
    }
    const ThunkClaim* const c = Host1ClaimAt(pc);
    return c != nullptr && c->kind == ClaimKind::Mutex && pc < Host1MutexThunk(*c).svc_at;
}

/// Where host 1's thread at `pc` in its own thunk stands in the guest's code.
u32 Host1ThunkReturn(u32 pc) {
    const ThunkClaim* const c = Host1ClaimAt(pc);
    ASSERT_MSG(c != nullptr, "a thread at {:08X} in host 1's thunk area outside a thunk", pc);
    if (c->kind == ClaimKind::Mutex) {
        const u32 svc_at = Host1MutexThunk(*c).svc_at;
        ASSERT_MSG(pc == svc_at || pc == svc_at + 4,
                   "a thread at {:08X} inside host 1's mutex thunk for {:08X}", pc, c->site);
        return pc == svc_at ? c->site : c->site + 4;
    }
    return c->site + (pc - c->entry);
}

void LeaseClosed(Kernel::Mutex& mutex, Kernel::Handle handle) {
    LeaseEntry& entry = LeaseEntries()[handle >> 15];
    if (entry.handle == handle) {
        lease_entry_of[LeaseIndex(mutex.lease)] = 0;
        entry = {};
    }
}

void LeaseFreed(Kernel::Mutex& mutex) {
    const u32 i = LeaseIndex(mutex.lease);
    DEBUG_ASSERT(lease_owner[i] == &mutex);
    if (lease_entry_of[i] != 0) {
        LeaseEntries()[lease_entry_of[i] - 1] = {};
        lease_entry_of[i] = 0;
    }
    lease_owner[i] = nullptr;
    lease_free.push_back(static_cast<u16>(i));
}

void LeaseReclaimHeld(u32 thread_id) {
    const u32* const words = LeaseWords();
    for (u32 i = 0; i < LeaseSlots; i++) {
        const u32 state = words[i];
        if (lease_owner[i] != nullptr && state != Kernel::Mutex::LeaseFree &&
            state != Kernel::Mutex::LeaseKernel &&
            (state & ~Kernel::Mutex::LeaseWaiters) >> 8 == thread_id) {
            lease_owner[i]->Reclaim();
        }
    }
}

/// Lends the mutex behind `handle` to the guest's thunks, which lock and unlock it from the next
/// call on while no thread waits on it.
void LeaseMutex(Kernel::KernelSystem& kernel, u32 handle) {
    const u32 slot = handle >> 15;
    if (slot >= LeaseSlots || LeaseEntries()[slot].handle == handle) {
        return;
    }
    const auto& process = *kernel.GetCurrentProcess();
    const auto mutex = process.handle_table.Get<Kernel::Mutex>(handle);
    if (mutex == nullptr) {
        return;
    }
    // One process's: the thunks find a handle by its value alone.
    if (LeaseProcess() == NoLeaseProcess) {
        LeaseProcess() = process.process_id;
    } else if (LeaseProcess() != process.process_id) {
        return;
    }
    u32 i;
    if (mutex->lease == nullptr) {
        if (lease_free.empty()) {
            return;
        }
        i = lease_free.back();
        lease_free.pop_back();
        lease_owner[i] = mutex.get();
        mutex->lease = &LeaseWords()[i];
        *mutex->lease = Kernel::Mutex::LeaseKernel;
        mutex->Relet();
    } else {
        i = LeaseIndex(mutex->lease);
        if (lease_entry_of[i] != 0) {
            return; // another handle names it
        }
    }
    LeaseEntry& entry = LeaseEntries()[slot];
    if (entry.handle != 0) {
        lease_entry_of[(entry.word - ThunkAreaBase - LeaseWordsOffset) / 4] = 0;
    }
    // The word before the handle: host 1's thunks read the handle first.
    std::atomic_ref<u32>(entry.word).store(ThunkAreaBase + LeaseWordsOffset + i * 4);
    std::atomic_ref<u32>(entry.handle).store(handle, std::memory_order_release);
    lease_entry_of[i] = static_cast<u16>(slot + 1);
}

/// GetThreadId's answer for a handle, the thunk's from now on. One word a slot: a thunk reading
/// while another host's thread closes the handle sees the ID or none.
void ThreadIdAnswered(const Kernel::Process& process, Kernel::Handle handle, u32 thread_id) {
    const u32 slot = handle >> 15;
    if (slot >= LeaseSlots || thread_id >> (32 - ThreadIdShift) != 0) {
        return; // the current thread's pseudo handle; an ID past the word
    }
    if (LeaseProcess() == NoLeaseProcess) {
        LeaseProcess() = process.process_id;
    } else if (LeaseProcess() != process.process_id) {
        return;
    }
    std::atomic_ref<u32>(ThreadIds()[slot]).store(thread_id << ThreadIdShift | (handle & 0x7FFFu));
}

void ThreadIdClosed(Kernel::Handle handle) {
    std::atomic_ref<u32>(ThreadIds()[handle >> 15]).store(0);
}

void ThreadIdsCleared() {
    for (u32 slot = 0; slot < LeaseSlots; slot++) {
        std::atomic_ref<u32>(ThreadIds()[slot]).store(0);
    }
}

/// The guest that map held is gone, and its patches with it.
void ForgetThunks(const VitaGuestMap* map) {
    if (thunk_area_map == map) {
        ResetLeases();
        patched_sites.clear();
        vfp_thunks.clear();
        svc_thunks.clear();
        svc_refused.clear();
        tick_thunks.clear();
        upcall_thunks.clear();
        upcall_body = upcall_after_svc = 0;
        upcall_redirect.entry = 0;
        thunk_area_used = 0;
        thunk_area_map = nullptr;
        host1_area_stale = true;
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
    if (pc == it->first || pc == t.miss_svc || pc == t.idle_b || pc == t.answer) {
        c.r[15] = t.site;
    } else if (pc == t.back || pc == t.miss_back) {
        c.r[15] = t.site + 4;
    } else if (pc == t.pop || pc == t.miss_pop || pc == t.idle_pop || pc == t.answer_mrs) {
        pop();
        c.r[15] = pc == t.pop ? t.site + 4 : t.site;
    } else {
        // Between the mrs and an msr (or the answer's mrs and its branch): the flags are in LR.
        DEBUG_ASSERT_MSG(pc <= t.done || pc == t.miss || pc == t.idle || pc == t.answer_b,
                         "a thread at {:08X} in svc thunk {:08X}", pc, it->first);
        if (pc > t.mrs) {
            flags();
        }
        pop();
        c.r[15] = pc >= t.answered && pc <= t.done ? t.site + 4 : t.site;
    }
}

// Upcalls: an svc that leaves the guest is answered by the HLE kernel's handler, called from the
// guest's world. A thunk in place of the svc switches to a stack of the app's and BLXes into the
// app's code at user level (UpcallSvcN, ARM_VitaNative::UpcallSvc), which answers in r0 (the
// result) and r1 (UpcallStatus), the other outputs in the thunk's frame.
//
// The thunk keeps the caller-saved registers, the flags and LR; r4-r7 go as the C++'s stack
// arguments. The C++ runs with the emulation thread's TPIDRURO and FPSCR (Sony's libraries find
// the thread through TPIDRURO); a Sony syscall it makes is forwarded (ForwardSonySyscall).
//
// Each site's stub pushes the frame with its resume PC and goes to the shared body. A Thumb
// site's 16-bit svc has no room for a branch: the next instruction moves into the stub. A site
// out of reach stays an svc, which the module returns into the redirect (AzaharRedirect).
//
// An svc that stopped the thread or asked for a reschedule answers UpcallReschedule and the tail
// runs svc UpcallRescheduleSvc. A quantum ending inside a thunk or its C++ runs the same thread
// on: another thread's upcall would use the same stack.

enum UpcallStatus : u32 { UpcallDone = 0, UpcallReschedule = 1 };
constexpr u32 UpcallRescheduleSvc = AZAHAR_SWITCH_SVC;

alignas(16) std::array<u8, 64 * 1024> upcall_stack;
#ifdef VITA_DIAGNOSTICS
constexpr u8 UpcallStackFill = 0xA5;
bool upcall_stack_filled{};
#endif
} // Anonymous namespace
} // namespace Core

/// The emulation thread's TPIDRURO, set at each slice, for the upcall body and upcall_tls.cpp.
extern "C" u32 azahar_upcall_host_tp = 0;

namespace Core {
namespace {
Core::System* upcall_system{};
u32 upcall_quantums{};
/// The quantums one upcall may take before the stop: five seconds of minimal ones.
constexpr u32 MaxUpcallQuantums = 20000;
u32 upcall_forwards{};
/// The last Sony syscall an upcall forwarded, and its caller: for the asserts.
u32 upcall_last_syscall{}, upcall_last_syscall_lr{};
/// The core whose slice an upcall runs in: its register file is the svc's for the HLE handlers.
ARM_VitaNative* upcall_core{};

bool UpcallFor(u32 number) {
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

/// The emulation thread's FPSCR, set at each slice: the upcall body's for the C++.
u32 upcall_host_fpscr{};

/// Whether `pc` is the app's code or an upcall thunk's own (not a Thumb site's moved instruction).
bool InUpcall(u32 pc) {
    if (pc >= 0x40000000u) {
        return true; // the app's own code
    }
    const auto it = upcall_thunks.upper_bound(pc);
    return it != upcall_thunks.begin() && pc < std::prev(it)->second.moved;
}

/// An svc exit at `pc` came from the upcall body's tail: an ordinary exit.
bool IsUpcallTailExit(u32 pc) {
    return upcall_after_svc != 0 && pc == upcall_after_svc;
}

u64 UpcallAnswer(u32 result, UpcallStatus status) {
    return u64{result} | u64{status} << 32;
}

/// The thunk's frame at the upcall stack's top, never on the guest's stack (a game may keep words
/// below its SP live): [0] the flags, [1 + n] rn for r0-r12, [14] SP, [15] LR, [16] the resume PC
/// (bit 0 for Thumb). The tail loads r0-r12, SP, LR and PC with one ldm. Below the frame, the
/// body's: the guest's TPIDRURO, d8-d15, FPSCR, the C++'s address and r4-r7.
constexpr u32 UpcallFrameWords = 17;
constexpr u32 FrameFlags = 0, FrameR0 = 1, FrameSp = 14, FrameLr = 15, FrameResume = 16;
u32* UpcallFrame() {
    return reinterpret_cast<u32*>(upcall_stack.data() + upcall_stack.size() - UpcallFrameWords * 4);
}
/// The guest's d8-d15 at the svc, as the body saved them.
const u32* UpcallVfpSave() {
    return reinterpret_cast<const u32*>(reinterpret_cast<const u8*>(UpcallFrame()) - 4 - 64);
}
/// The guest's FPSCR at the svc (the C++ runs with the emulation thread's).
u32 UpcallGuestFpscr() {
    u32 v;
    std::memcpy(&v, reinterpret_cast<const u8*>(UpcallFrame()) - 4 - 64 - 4, sizeof(v));
    return v;
}

/// The guest clock's rate this slice (AzaharRunRequest.tick_hz), for an idle's quantum.
u64 upcall_tick_hz{};

/// AZAHAR_CLOCK_SVC, from an upcall only: the guest ticks since the slice's clock last started;
/// it starts again over `quantum` private-timer ticks, from `base` when `set`.
u64 UpcallClock(bool set, u32 quantum, u64 base) {
    static_assert(AZAHAR_CLOCK_SVC == 0xFB);
    register u32 r0 asm("r0") = set ? 1u : 0u;
    register u32 r1 asm("r1") = quantum;
    register u32 r2 asm("r2") = static_cast<u32>(base);
    register u32 r3 asm("r3") = static_cast<u32>(base >> 32);
    asm volatile("svc #0xfb" : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3) : : "r12", "memory");
    return u64{r1} << 32 | r0;
}

/// `ns` in private-timer ticks at this slice's guest clock rate, at least one, in 32.32 fixed
/// point (no doubles: a library call each on ARM32). For up to MaxQuantumNs.
u32 UpcallTimerTicks(u64 ns) {
    const auto* const page = reinterpret_cast<const volatile AzaharUserPage*>(
        static_cast<uintptr_t>(AZAHAR_USER_PAGE_VA));
    static u64 tick_hz{}, ticks_per_ns_q32{};
    static u32 mult_int{}, mult_frac{};
    if (upcall_tick_hz != tick_hz || page->mult_int != mult_int || page->mult_frac != mult_frac) {
        tick_hz = upcall_tick_hz;
        mult_int = page->mult_int;
        mult_frac = page->mult_frac;
        const double per_tick = mult_int + mult_frac / 4294967296.0;
        ticks_per_ns_q32 =
            static_cast<u64>(static_cast<double>(tick_hz) / 1e9 / per_tick * 4294967296.0);
    }
    DEBUG_ASSERT(ns <= MaxQuantumNs);
    const u64 ticks = (ns * ticks_per_ns_q32) >> 32;
    return static_cast<u32>(std::clamp<u64>(ticks, 1, 0xFFFFFFFF));
}

/// Whether the module's clock stands still for an upcall's C++: an exit inside adds no guest time.
bool upcall_clock_paused{};

/// The guest's time on again after an upcall's C++, over `quantum_ns`, the C++'s time left out.
void ResumeUpcallClock(Core::Timing::Timer& timer, u64 quantum_ns) {
    upcall_clock_paused = false;
    UpcallClock(true, UpcallTimerTicks(quantum_ns), timer.GetTicks());
}

/// Pauses the guest's clock for an upcall's C++. The timer runs on over the re-arm quantum: it is
/// the core's only interrupt in the guest's world, and its re-arm is where a pending SGI sends the
/// core back to Sony's.
u64 PauseUpcallClock() {
    upcall_clock_paused = true;
    return UpcallClock(false, UpcallTimerTicks(MinQuantumNs), 0);
}

/// The redirect's C++: the svc's number from the record.
u64 UpcallSvcRedirect(u32 r0, u32 r1, u32 r2, u32 r3, u32 r4, u32 r5, u32 r6, u32 r7) {
#ifdef VITA_DIAGNOSTICS
    census.upcall_redirects++;
#endif
    return ARM_VitaNative::UpcallSvc(upcall_redirect.svc, {r0, r1, r2, r3, r4, r5, r6, r7});
}

/// The thread switch the module makes from an upcall (AZAHAR_SWITCH_SVC).
AzaharSwitch upcall_switch{};

/// The words on the upcall stack that point into the app's code, deepest first: the C++'s return
/// addresses, for an assert's message.
std::string UpcallReturns(const AzaharContext& c) {
    std::string list;
    const u32 stack_lo = static_cast<u32>(reinterpret_cast<uintptr_t>(upcall_stack.data()));
    const u32 stack_hi = stack_lo + static_cast<u32>(upcall_stack.size());
    if (c.r[13] >= stack_lo && c.r[13] < stack_hi) {
        for (u32 a = c.r[13] & ~3u; a < stack_hi && list.size() < 400; a += 4) {
            u32 w;
            std::memcpy(&w, upcall_stack.data() + (a - stack_lo), 4);
            if (w >= 0x81000000u && w < 0x82000000u) {
                list += fmt::format(" {:08X}", w);
            }
        }
    }
    return list;
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

/// A thread stopped in an upcall thunk stands in a Thumb site's moved instruction: it goes on at
/// the guest's own code.
void FinishUpcallThunk(Memory::MemorySystem& memory, AzaharContext& c) {
    // The moved instruction, or its branch back: the site's next instruction, or the one after.
    const auto unmove = [](u32 pc) -> std::optional<u32> {
        const auto it = upcall_thunks.upper_bound(pc);
        if (it == upcall_thunks.begin()) {
            return std::nullopt;
        }
        const UpcallThunk& t = std::prev(it)->second;
        if (t.site == 0 || pc < t.moved || pc >= t.end) {
            return std::nullopt;
        }
        return pc == t.moved ? t.site + 2 : t.site + 4;
    };
    const u32 pc = c.r[15];
    const auto back = unmove(pc);
    ASSERT_MSG(back.has_value(), "a thread at {:08X} inside an upcall thunk", pc);
    c.r[15] = *back;
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
        FinishUpcallThunk(memory, c);
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
    // The guest's memory svcs run on host 0.
    DEBUG_ASSERT_MSG(op == AZAHAR_EDIT_SYNC_CODE || Core::ThisHost() == 0,
                     "azaharEdit op {} of {:#010x}+{:#x} on the second emulation thread", op,
                     guest_va, size);
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
    announced.insert(base);
    const auto existing = live_mappings.find(base);
    if (existing != live_mappings.end() && existing->second == wanted) {
        return;
    }

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
    // Nothing unmapped here: the ranges are announced again and OnMirroring unmaps those that did not
    // come back. Unmapping first would leave a gap the other thread's guest fetches code from.
    mirrored = nullptr;
    announced.clear();
}

void VitaGuestMap::OnMirroring(const Memory::PageTable& page_table) {
    std::vector<std::pair<VAddr, u32>> gone;
    for (const auto& [base, mapping] : live_mappings) {
        if (!announced.contains(base)) {
            gone.emplace_back(base, mapping.size);
        }
    }
    for (const auto& [base, size] : gone) {
        ForgetRange(base, size, installed);
    }
    announced.clear();
    mirrored = &page_table;
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
                                 extra.size, GuestPerm);
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

void VitaGuestMap::SyncViewCode(const u8* host, VAddr guest, u32 size) const {
    if (!installed) {
        return;
    }
    const bool synced = Edit(AZAHAR_EDIT_SYNC_CODE, reinterpret_cast<u32>(host), guest, size, 0);
    ASSERT_MSG(synced, "the code at {:08X}+{:#x} of another core's view was not synced", guest,
               size);
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
        const bool mapped =
            Edit(AZAHAR_EDIT_MAP, reinterpret_cast<u32>(host), base, size, GuestPerm);
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
    // A thread pinned to a core the module holds never runs again: this thread steps aside before the
    // take, and back if the module folded the guest into its caller.
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
    // The scheduler asks for a stop right before a new slice: that request was the last slice's.
    stop_requested = false;

    const u64 quantum =
        std::clamp(TicksToNanoseconds(timer->GetDowncount()), MinQuantumNs, MaxQuantumNs);
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
    ClaimThunk(*map, base, pc, insn, thumb, ClaimKind::Vfp);
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
        ResetLeases();
        Kernel::mutex_lease_hooks = {&LeaseClosed, &LeaseFreed, &LeaseReclaimHeld};
        Kernel::thread_id_hooks = {&ThreadIdAnswered, &ThreadIdClosed, &ThreadIdsCleared};
    }
    if (thunk_area_map != map.get()) {
        ForgetThunks(thunk_area_map); // another guest's
        map->MapExtra(ThunkAreaBase, ThunkAreaSize, thunk_area->Data());
        thunk_area_map = map.get();
    }
    if (host1_area_stale) {
        ResetHost1Area(*map);
        host1_area_stale = false;
    }
    // Host 1's view ends at the thread IDs' and leases' pages (Host1ThunkSize).
    if (thunk_area_used + size > Host1ThunkSize) {
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
    constexpr u32 EQ = 0x0, NE = 0x1, HS = 0x2;
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
    // GetSystemTick's counters' address (two movw/movt pairs) and its branch to the idle skip's
    // way out, filled in once both have an address; where the answer starts.
    std::array<u32, 2> data_mov{};
    u32 to_idle = 0, normal = 0;
    // GetThreadId's branch from its other handles' answer to the way back.
    u32 to_done = 0;
    switch (kind) {
    case AZAHAR_FAST_TICK: {
        // The call counted in its caller's slot (the guest's LR, pushed; CheckTickPolls reads
        // them); the caller that spins on the clock goes out to the idle skip.
        data_mov[0] = n;
        put(0);
        put(0);
        put(ldr(3, 13, 12)); // the caller
        put(0xE203201Cu);    // and r2, r3, #0x1c: its slot's index times 4
        put(0xE08C2082u);    // add r2, r12, r2, lsl #1: its slot
        put(ldr(12, 2, 0));  // the slot's caller
        put(cmp(12, 3));
        put(0x0592C004u); // ldreq r12, [r2, #4]
        put(0x028CC001u); // addeq r12, r12, #1
        put(0x13A0C001u); // movne r12, #1
        put(0xE582C004u); // str r12, [r2, #4]: its calls
        put(0x15823000u); // strne r3, [r2]: a caller new to the slot
        data_mov[1] = n;
        put(0);
        put(0);
        put(ldr(2, 12, 4 * TickSkipWord)); // the caller sent to the skip, 0 for none
        put(cmp(2, 3));
        to_idle = n;
        put(0); // beq idle
        normal = at();
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
    case AZAHAR_FAST_THREAD_ID: {
        // r0 = 0, r1 = the ID: the current thread's (0xFFFF8000) from the page, another handle's
        // from the IDs the svc gave before (ThreadIds), while the handle's slot holds its
        // generation and the leases' process runs.
        put(movw(12, 0x8000));
        put(movt(12, 0xFFFF8000u));
        put(cmp(1, 12));
        const u32 to_current = n;
        put(0);
        page();
        put(ldr(2, 12, offsetof(AzaharUserPage, process_id)));
        put(movw(12, (ThunkAreaBase + LeaseTableOffset) & 0xFFFF));
        put(movt(12, ThunkAreaBase + LeaseTableOffset));
        put(ldr(3, 12, 0));
        put(cmp(2, 3));
        branch_miss(NE);
        put(0xE1A027A1u); // mov r2, r1, lsr #15: the handle's slot
        put(0xE3520A01u); // cmp r2, #0x1000
        branch_miss(HS);
        static_assert(LeaseTableOffset - ThreadIdsOffset == 0x4000 && LeaseSlots == 0x1000);
        put(0xE24C3901u); // sub r3, r12, #0x4000
        put(0xE7932102u); // ldr r2, [r3, r2, lsl #2]
        put(0xE0223001u); // eor r3, r2, r1
        put(0xE1B03883u); // movs r3, r3, lsl #17: the generations differ
        branch_miss(NE);
        put(0xE1B03881u); // movs r3, r1, lsl #17: generation 0, an empty word's
        branch_miss(EQ);
        put(0xE3A00000u); // mov r0, #0
        put(0xE1A017A2u); // mov r1, r2, lsr #15
        to_done = n;
        put(0);
        buf[to_current] = ArmBranch(EQ, base + to_current * 4, at()).value();
        page();
        answers();
        put(0xE3A00000u); // mov r0, #0
        put(ldr(1, 12, offsetof(AzaharUserPage, thread_id)));
        t.answered = at();
        break;
    }
    case AZAHAR_FAST_PROCESS_ID:
        // r0 = 0, r1 = the ID, for the current process (0xFFFF8001).
        handle(1, 0xFFFF8001u);
        page();
        answers();
        put(0xE3A00000u); // mov r0, #0
        put(ldr(1, 12, offsetof(AzaharUserPage, process_id)));
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
    if (to_done != 0) {
        buf[to_done] = ArmBranch(0xE, base + to_done * 4, t.done).value();
    }
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
    if (kind == AZAHAR_FAST_TICK) {
        // idle: the guest's flags and registers back, then the idle skip's upcall (EnableIdleSkip
        // branches there; until then the miss's svc). answer: the skip's resume, the thunk again
        // past its check, the clock moved on. Then the counters.
        t.idle = at();
        put(0xE128F00Eu); // msr apsr_nzcvq, lr
        t.idle_pop = at();
        put(0xE8BD500Cu); // pop {r2, r3, r12, lr}
        t.idle_b = at();
        put(ArmBranch(0xE, t.idle_b, t.miss_svc).value());
        t.answer = at();
        put(0xE92D500Cu); // push {r2, r3, r12, lr}
        t.answer_mrs = at();
        put(0xE10FE000u); // mrs lr, apsr
        t.answer_b = at();
        put(ArmBranch(0xE, t.answer_b, normal).value());
        t.data = at();
        for (u32 k = 0; k < TickDataWords; k++) {
            put(0);
        }
        for (const u32 w : data_mov) {
            buf[w] = movw(12, t.data);
            buf[w + 1] = movt(12, t.data);
        }
        buf[to_idle] = ArmBranch(EQ, base + to_idle * 4, t.idle).value();
    }
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
    if (kind == AZAHAR_FAST_TICK) {
        tick_thunks.push_back(TickThunk{base, pc, t.data});
    }
    ClaimThunk(*map, base, pc, insn, false, ClaimKind::Svc);
    memory.Write32(pc, *site);
    map->SyncCode(pc, 4);
    patched_sites[pc] = PatchedSite{insn, false};
#ifdef VITA_DIAGNOSTICS
    census.svc_thunks++;
#endif
}

namespace {
u32 ArmMovw(u32 rd, u32 v) {
    return 0xE3000000u | (v >> 12 & 0xF) << 16 | rd << 12 | (v & 0xFFF);
}
u32 ArmMovt(u32 rd, u32 v) {
    return 0xE3400000u | (v >> 28 & 0xF) << 16 | rd << 12 | (v >> 16 & 0xFFF);
}
/// Thumb-2 MOVW (`op` 0xF240) or MOVT (0xF2C0) of the 16-bit `v` into `rd`, its halfwords.
std::array<u16, 2> ThumbMov16(u16 op, u32 rd, u32 v) {
    return {static_cast<u16>(op | (v >> 11 & 1) << 10 | (v >> 12 & 0xF)),
            static_cast<u16>((v >> 8 & 7) << 12 | rd << 8 | (v & 0xFF))};
}
/// Thumb-2 BLX (T2) at `from` to the ARM code at `to`, its halfwords, if in reach.
std::optional<std::array<u16, 2>> ThumbBlxArm(u32 from, u32 to) {
    const s32 off = static_cast<s32>(to - ((from + 4) & ~3u));
    if ((off & 3) != 0 || off < -(1 << 24) || off >= (1 << 24)) {
        return std::nullopt;
    }
    const u32 u = static_cast<u32>(off);
    const u32 sign = u >> 31 & 1, i1 = u >> 23 & 1, i2 = u >> 22 & 1;
    const u32 j1 = ~(i1 ^ sign) & 1, j2 = ~(i2 ^ sign) & 1;
    return std::array<u16, 2>{static_cast<u16>(0xF000u | sign << 10 | (u >> 12 & 0x3FFu)),
                              static_cast<u16>(0xC000u | j1 << 13 | j2 << 11 | (u >> 1 & 0x7FEu))};
}
u32 HostAddress(const void* p) {
    return static_cast<u32>(reinterpret_cast<uintptr_t>(p));
}
u32 HostAddress(UpcallFn fn) {
    return static_cast<u32>(reinterpret_cast<uintptr_t>(fn));
}
/// A thunk's way into the body, from `base`: the guest's SP into a slot of its own, SP to the
/// frame's SP word, r0-r12, LR and the guest's SP into the frame, `resume` (instructions that
/// leave the resume PC in r12) and the resume PC into it, SP to r0's word, the C++ (`fn`) in r12
/// and a branch to the body; then the frame's address and the slot. Nothing below the guest's
/// SP is written.
std::optional<std::vector<u32>> UpcallEntry(u32 base, std::initializer_list<u32> resume, u32 fn,
                                            u32 body) {
    std::vector<u32> w;
    w.push_back(0);           // str sp, [pc, #slot]
    w.push_back(0);           // ldr sp, [pc, #frame_sp]
    w.push_back(0xE90D1FFFu); // stmdb sp, {r0-r12}
    w.push_back(0xE58DE004u); // str lr, [sp, #4]
    const std::size_t load_slot = w.size();
    w.push_back(0);           // ldr r12, [pc, #slot]: the guest's SP
    w.push_back(0xE58DC000u); // str r12, [sp]
    w.insert(w.end(), resume);
    w.push_back(0xE58DC008u); // str r12, [sp, #8]: the resume PC
    w.push_back(0xE24DD034u); // sub sp, sp, #52: r0's word
    w.push_back(ArmMovw(12, fn));
    w.push_back(ArmMovt(12, fn));
    const std::size_t branch = w.size();
    w.push_back(0);
    const std::size_t frame_sp = w.size();
    w.push_back(HostAddress(&UpcallFrame()[FrameSp]));
    const std::size_t slot = w.size();
    w.push_back(0);
    const auto pc_rel = [](std::size_t at, std::size_t target) {
        return static_cast<u32>((target - at) * 4 - 8);
    };
    w[0] = 0xE58FD000u | pc_rel(0, slot);
    w[1] = 0xE59FD000u | pc_rel(1, frame_sp);
    w[load_slot] = 0xE59FC000u | pc_rel(load_slot, slot);
    const auto to_body = ArmBranch(0xE, base + static_cast<u32>(branch) * 4, body);
    if (!to_body) {
        return std::nullopt;
    }
    w[branch] = *to_body;
    return w;
}

/// The guest's own lock or unlock of a leased mutex (Kernel::Mutex::lease) at an ARM site of
/// ReleaseMutex (svc 0x14) or WaitSynchronization1 (0x24), from `base`: five words for r1-r3, r12
/// and the flags (the guest's stack may be held), then the code, in at `base` + 20. In the
/// leases' process, the handle's lease word from the table (LeaseEntries), then with
/// LDREX/STREX: a free mutex taken (the
/// thread's ID << 8, count 1) or one the thread holds counted up; counted down, and freed at 0.
/// r0 0, every other register and the flags as they came, on at `resume`. Anything else (no
/// lease, the kernel's, another thread's, a count of 255) falls through past the end to the
/// upcall with every register as it came. A thread stopped inside runs on alone (InUpcall).
/// Nothing when `resume` is out of a branch's reach.
std::optional<std::vector<u32>> MutexFastPath(u32 svc, u32 base, u32 resume) {
    std::vector<u32> w(5, 0);
    const auto at = [&](std::size_t i) { return base + static_cast<u32>(i) * 4; };
    // STR or LDR of rt to slot k, PC-relative (the slots lie behind).
    const auto slot = [&](u32 op, u32 rt, u32 k) {
        w.push_back(op | rt << 12 | (at(w.size()) + 8 - at(k)));
    };
    constexpr u32 Str = 0xE50F0000u, Ldr = 0xE51F0000u;
    struct Fixup {
        std::size_t at;
        u32 cond;
        int label;
    };
    std::vector<Fixup> fixups;
    std::array<std::size_t, 4> labels{}; // retry, store, slow_clrex, slow
    enum { Retry, Store, SlowClrex, Slow };
    const auto branch = [&](u32 cond, int label) {
        fixups.push_back({w.size(), cond, label});
        w.push_back(0);
    };
    const u32 table = ThunkAreaBase + LeaseTableOffset;
    slot(Str, 1, 0);
    slot(Str, 2, 1);
    slot(Str, 3, 2);
    slot(Str, 12, 3);
    w.push_back(0xE10F3000u); // mrs r3, apsr
    slot(Str, 3, 4);
    w.push_back(ArmMovw(1, table));
    w.push_back(ArmMovt(1, table));
    w.push_back(ArmMovw(3, AZAHAR_USER_PAGE_VA));
    w.push_back(ArmMovt(3, AZAHAR_USER_PAGE_VA));
    static_assert(offsetof(AzaharUserPage, process_id) == 0x20);
    w.push_back(0xE5932020u); // ldr r2, [r3, #0x20]: the running process's ID
    w.push_back(0xE591C000u); // ldr r12, [r1]: the leases'
    w.push_back(0xE152000Cu); // cmp r2, r12
    branch(0x1, Slow);        // bne
    w.push_back(0xE2811010u); // add r1, r1, #16: the entries
    w.push_back(0xE1A027A0u); // mov r2, r0, lsr #15: the handle's slot
    w.push_back(0xE3520A01u); // cmp r2, #0x1000
    branch(0x2, Slow);        // bhs
    w.push_back(0xE0811182u); // add r1, r1, r2, lsl #3
    w.push_back(0xE5912000u); // ldr r2, [r1]: the entry's handle
    w.push_back(0xE1520000u); // cmp r2, r0
    branch(0x1, Slow);        // bne
    // The word's address made to depend on the handle read: host 0 publishes the word before
    // the handle (LeaseMutex), and a branch alone does not keep this thread's two loads in that
    // order (host 1 reads while host 0 writes).
    w.push_back(0xE022C002u); // eor r12, r2, r2
    w.push_back(0xE081100Cu); // add r1, r1, r12
    w.push_back(0xE5911004u); // ldr r1, [r1, #4]: the lease word
    // 0: the entry cleared after its handle was read (LeaseClosed, LeaseFreed from host 0 while
    // host 1 runs a lock through the same handle): the svc answers for the handle as it is now.
    w.push_back(0xE3510000u); // cmp r1, #0
    branch(0x0, Slow);        // beq
    static_assert(offsetof(AzaharUserPage, thread_id) == 0x1c);
    w.push_back(0xE593301Cu); // ldr r3, [r3, #0x1c]: the running thread's ID
    labels[Retry] = w.size();
    w.push_back(0xE1912F9Fu); // ldrex r2, [r1]
    // Held by this thread whether or not another waits (bit 31, Kernel::Mutex::LeaseWaiters):
    // its nested locks and unlocks stay here; its last unlock with a waiter goes to the kernel.
    if (svc == 0x14) {
        w.push_back(0xE3C2C102u); // bic r12, r2, #0x80000000
        w.push_back(0xE023C42Cu); // eor r12, r3, r12, lsr #8
        w.push_back(0xE35C0000u); // cmp r12, #0: held by this thread
        branch(0x1, SlowClrex);
        w.push_back(0xE202C0FFu); // and r12, r2, #0xff
        w.push_back(0xE35C0001u); // cmp r12, #1
        w.push_back(0x12422001u); // subne r2, r2, #1: a nested unlock
        branch(0x1, Store);
        w.push_back(0xE3120102u); // tst r2, #0x80000000: a waiter
        branch(0x1, SlowClrex);
        w.push_back(0xE3A02000u); // mov r2, #0: free
        labels[Store] = w.size();
        w.push_back(0xE181CF92u); // strex r12, r2, [r1]
        w.push_back(0xE35C0000u); // cmp r12, #0
        branch(0x1, Retry);
    } else {
        w.push_back(0xE3520000u); // cmp r2, #0: free
        w.push_back(0x01A0C403u); // moveq r12, r3, lsl #8
        w.push_back(0x038CC001u); // orreq r12, r12, #1
        branch(0x0, Store);
        w.push_back(0xE3C2C102u); // bic r12, r2, #0x80000000
        w.push_back(0xE023C42Cu); // eor r12, r3, r12, lsr #8
        w.push_back(0xE35C0000u); // cmp r12, #0: held by this thread
        branch(0x1, SlowClrex);
        w.push_back(0xE202C0FFu); // and r12, r2, #0xff
        w.push_back(0xE35C00FFu); // cmp r12, #0xff
        branch(0x0, SlowClrex);
        w.push_back(0xE282C001u); // add r12, r2, #1
        labels[Store] = w.size();
        w.push_back(0xE1812F9Cu); // strex r2, r12, [r1]
        w.push_back(0xE3520000u); // cmp r2, #0
        branch(0x1, Retry);
    }
    w.push_back(0xE3A00000u); // mov r0, #0: success
    slot(Ldr, 3, 4);
    w.push_back(0xE12CF003u); // msr apsr_nzcvqg, r3
    slot(Ldr, 1, 0);
    slot(Ldr, 2, 1);
    slot(Ldr, 3, 2);
    slot(Ldr, 12, 3);
    const std::size_t to_resume = w.size();
    w.push_back(0);
    labels[SlowClrex] = w.size();
    w.push_back(0xF57FF01Fu); // clrex
    labels[Slow] = w.size();
    slot(Ldr, 3, 4);
    w.push_back(0xE12CF003u); // msr apsr_nzcvqg, r3
    slot(Ldr, 1, 0);
    slot(Ldr, 2, 1);
    slot(Ldr, 3, 2);
    slot(Ldr, 12, 3);
    for (const Fixup& f : fixups) {
        w[f.at] = *ArmBranch(f.cond, at(f.at), at(labels[f.label]));
    }
    const auto back = ArmBranch(0xE, at(to_resume), resume);
    if (!back) {
        return std::nullopt;
    }
    w[to_resume] = *back;
    return w;
}

} // Anonymous namespace

void ARM_VitaNative::EnsureUpcallBody() {
    if (upcall_body != 0 && thunk_area_map == map.get()) {
        return;
    }
    constexpr u32 BodyMax = 384;
    const auto room = ThunkRoom(BodyMax); // may forget another guest's thunks, this body's too
    ASSERT_MSG(room.has_value(), "no room for the upcall thunks' body");
    const u32 base = *room;
    std::array<u32, BodyMax / 4> buf{};
    u32 n = 0;
    const auto at = [&] { return base + n * 4; };
    const auto put = [&](u32 w) {
        ASSERT(n < buf.size());
        buf[n++] = w;
    };
    const auto put_mov32 = [&](u32 rd, u32 v) {
        put(ArmMovw(rd, v));
        put(ArmMovt(rd, v));
    };
#ifdef VITA_DIAGNOSTICS
    std::memset(upcall_stack.data(), UpcallStackFill, upcall_stack.size() - 256);
    upcall_stack_filled = true;
#endif
    // The body, entered from a thunk's UpcallEntry: the frame complete but for the flags, SP at
    // its r0 word, the C++ in r12.
    put(0xE10FE000u); // mrs lr, apsr
    put(0xE52DE004u); // str lr, [sp, #-4]!: the flags, SP at the frame
    put(0xEE1DEF70u); // mrc p15, 0, lr, c13, c0, 3: the guest's TPIDRURO
    put(0xE52DE004u); // push {lr}
    put(0xED2D8B10u); // vpush {d8-d15}: the guest's callee-saved VFP registers
    put(0xEEF1EA10u); // vmrs lr, fpscr
    put(0xE52DE004u); // push {lr}: the guest's FPSCR
    put(0xE52DC004u); // push {r12}: the C++
    put_mov32(14, HostAddress(&upcall_host_fpscr));
    put(0xE59EE000u); // ldr lr, [lr]
    put(0xEEE1EA10u); // vmsr fpscr, lr: the emulation thread's
    put_mov32(12, HostAddress(&azahar_upcall_host_tp));
    put(0xE59CC000u);                  // ldr r12, [r12]
    put(0xEF000000u | AZAHAR_TLS_SVC); // TPIDRURO = r12: the emulation thread's
    put(0xE59DC000u);                  // ldr r12, [sp]: the C++
    put(0xE92D00F0u); // push {r4-r7}: an svc's fifth to eighth inputs, the C++'s stack arguments
    put(0xE12FFF3Cu); // blx r12: the arguments in r0-r7; r0 the result, r1 UpcallStatus
    put(0xE28DD014u); // add sp, sp, #20: past r4-r7 and the C++
    put(0xE49D2004u); // pop {r2}: the guest's FPSCR
    put(0xEEE12A10u); // vmsr fpscr, r2
    put(0xE28DD040u); // add sp, sp, #64: past d8-d15 (the C++ kept them)
    put(0xE49DC004u); // pop {r12}: the guest's TPIDRURO
    put(0xEF000000u | AZAHAR_TLS_SVC);
    put(0xE49DC004u); // pop {r12}: the guest's flags; SP at r0's word
    put(0xE3510001u); // cmp r1, #1
    put(0xE58D0000u); // str r0, [sp]: the result over the guest's r0
    const u32 to_reschedule = n;
    put(0); // beq reschedule
    put(0xE12CF00Cu); // msr apsr_nzcvqg, r12
    put(0xE89DFFFFu); // ldm sp, {r0-r12, sp, lr, pc}: the guest's, at the resume PC
    // reschedule: the guest's registers but its PC, then svc AZAHAR_SWITCH_SVC: the module
    // switches to the thread the upcall armed, or exits, and the exit puts the resume PC in
    // (RunSliceOnce). Nothing runs after the svc.
    const u32 reschedule = at();
    put(0xE12CF00Cu); // msr apsr_nzcvqg, r12
    put(0xE89D7FFFu); // ldm sp, {r0-r12, sp, lr}
    put(0xEF000000u | UpcallRescheduleSvc);
    const u32 after_svc = at();
    put(0xE7F000F0u); // udf
    const auto beq = ArmBranch(0x0, base + to_reschedule * 4, reschedule);
    ASSERT(beq.has_value());
    buf[to_reschedule] = *beq;
    const u32 body_end = at();
    // The redirect's entry: a thunk's way in with the record's resume PC, its C++ the one that
    // reads the svc's number from the record.
    const u32 redirect = at();
    const auto entry = UpcallEntry(redirect,
                                   {ArmMovw(12, HostAddress(&upcall_redirect)),
                                    ArmMovt(12, HostAddress(&upcall_redirect)),
                                    0xE59CC008u /* ldr r12, [r12, #8]: AzaharRedirect.pc */},
                                   HostAddress(&UpcallSvcRedirect), base);
    ASSERT(entry.has_value());
    for (const u32 w : *entry) {
        put(w);
    }
    const u32 end = at();
    std::memcpy(thunk_area->Data() + thunk_area_used, buf.data(), n * 4);
    map->SyncCode(base, n * 4);
    thunk_area_used += n * 4;
    upcall_thunks[base] = UpcallThunk{0, body_end, body_end};
    upcall_thunks[redirect] = UpcallThunk{0, end, end};
    upcall_body = base;
    upcall_after_svc = after_svc;
    upcall_redirect.entry = redirect;
}

namespace {
/// The idle skip's C++ (EnableIdleSkip's stub): the upcall's calling convention.
u64 UpcallIdleSkipFn(u32 r0, u32, u32, u32, u32, u32, u32, u32) {
    return ARM_VitaNative::UpcallIdleSkip(r0);
}
} // Anonymous namespace

void ARM_VitaNative::CheckTickPolls() {
    if (tick_thunks.empty()) {
        return;
    }
    // Each caller's reads over a vblank of guest time at least: a slot that changed caller, or
    // that a new caller took and gave back, starts its window again.
    const u64 now = timer->GetTicks();
    for (TickThunk& tt : tick_thunks) {
        const u32* const words =
            reinterpret_cast<const u32*>(thunk_area->Data() + (tt.data - ThunkAreaBase));
        if (words[TickSkipWord] != 0) {
            continue; // one caller a site goes to the skip
        }
        for (u32 k = 0; k < TickSlots; k++) {
            const u32 caller = words[2 * k], calls = words[2 * k + 1];
            TickWindow& w = tt.window[k];
            if (caller == 0 || caller != w.caller || calls < w.calls) {
                w = TickWindow{caller, calls, now};
                continue;
            }
            if (now - w.since < VideoCore::FRAME_TICKS) {
                continue;
            }
            const u64 per_vblank = u64{calls - w.calls} * VideoCore::FRAME_TICKS / (now - w.since);
            w = TickWindow{caller, calls, now};
            if (per_vblank >= IdleReadsPerVblank) {
                LOG_INFO(Core_ARM11,
                         "idle loop: GetSystemTick at {:08X} from {:08X}, {} reads a vblank: "
                         "skipped to the next event from now on",
                         tt.site, caller, per_vblank);
#ifdef VITA_DIAGNOSTICS
                census.idle_waits++;
#endif
                EnableIdleSkip(tt.base, caller);
                break;
            }
        }
    }
}

void ARM_VitaNative::EnableIdleSkip(u32 base, u32 caller) {
    SvcThunk& t = svc_thunks.at(base);
    if (t.skip == 0) {
        // The skip's upcall stub, entered with the guest's registers as at the svc and resuming
        // at the thunk's answer; the thunk's way out branched to it.
        EnsureUpcallBody();
        const auto room = ThunkRoom(32 * 4);
        if (!room) {
            LOG_WARNING(Core_ARM11, "idle loop: no room for the skip of the thunk at {:08X}", base);
            return;
        }
        const u32 stub = *room;
        const auto entry = UpcallEntry(stub, {ArmMovw(12, t.answer), ArmMovt(12, t.answer)},
                                       HostAddress(&UpcallIdleSkipFn), upcall_body);
        const auto out = ArmBranch(0xE, t.idle_b, stub);
        ASSERT_MSG(entry.has_value() && out.has_value(), "idle loop: the skip's stub at {:08X}",
                   stub);
        const u32 bytes = static_cast<u32>(entry->size() * 4);
        std::memcpy(thunk_area->Data() + thunk_area_used, entry->data(), bytes);
        map->SyncCode(stub, bytes);
        thunk_area_used += bytes;
        upcall_thunks[stub] = UpcallThunk{t.site, stub + bytes, stub + bytes};
        std::memcpy(thunk_area->Data() + (t.idle_b - ThunkAreaBase), &*out, 4);
        map->SyncCode(t.idle_b, 4);
        t.skip = stub;
    }
    u32* const words = reinterpret_cast<u32*>(thunk_area->Data() + (t.data - ThunkAreaBase));
    words[TickSkipWord] = caller;
}

u64 ARM_VitaNative::UpcallIdleSkip(u32 r0) {
    ARM_VitaNative& core = *upcall_core;
    Core::Timing::Timer& timer = *core.timer;
    // The guest's time so far, then the slice's rest: the caller spins on the clock
    // (CheckTickPolls), so the clock goes to the slice's end, the next event, as if it had spun
    // there. The slice ends (the tail's exit) for RunLoop's events; the thread goes on into the
    // thunk's answer at the new time.
    timer.AddScaledTicks(PauseUpcallClock());
    const s64 rest = timer.GetDowncount();
    if (rest > 0) {
        timer.Idle();
    }
#ifdef VITA_DIAGNOSTICS
    census.idle_skips++;
    census.idle_skip_ticks += rest > 0 ? static_cast<u64>(rest) : 0;
    upcall_system->GPU().GuestIdle();
#endif
    ResumeUpcallClock(timer, MinQuantumNs);
    return UpcallAnswer(r0, UpcallReschedule);
}

void ARM_VitaNative::PatchUpcall(u32 pc, bool thumb) {
    if (InThunkArea(pc) || patched_sites.contains(pc) || svc_refused.contains(pc)) {
        return;
    }
    if (thumb) {
        PatchUpcallThumb(pc);
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
    EnsureUpcallBody();
    // ReleaseMutex and WaitSynchronization1: the guest's own unlock and lock first.
    const bool fast = number == 0x14 || number == 0x24;
    const u32 room_bytes = fast ? 96 * 4 : 32 * 4;
    const auto room = ThunkRoom(room_bytes);
    if (!room) {
        svc_refused.insert(pc);
        return;
    }
    const u32 base = *room;
    const u32 fn = HostAddress(upcall_table[number]);
    const u32 resume = pc + 4;
    std::vector<u32> words;
    u32 in = base;
    if (fast) {
        auto fast_path = MutexFastPath(number, base, resume);
        if (!fast_path) {
            svc_refused.insert(pc);
            return;
        }
        words = std::move(*fast_path);
        in = base + 20;
    }
    const auto entry = UpcallEntry(base + static_cast<u32>(words.size()) * 4,
                                   {ArmMovw(12, resume), ArmMovt(12, resume)}, fn, upcall_body);
    const auto site = ArmBranch(cond, pc, in);
    if (!entry || !site) {
        svc_refused.insert(pc);
        return;
    }
    words.insert(words.end(), entry->begin(), entry->end());
    const u32 bytes = static_cast<u32>(words.size() * 4);
    ASSERT(bytes <= room_bytes);
    std::memcpy(thunk_area->Data() + thunk_area_used, words.data(), bytes);
    map->SyncCode(base, bytes);
    thunk_area_used += bytes;
    upcall_thunks[base] = UpcallThunk{pc, base + bytes, base + bytes};
    ClaimThunk(*map, in, pc, insn, false, fast ? ClaimKind::Mutex : ClaimKind::Svc);
    memory.Write32(pc, *site);
    map->SyncCode(pc, 4);
    patched_sites[pc] = PatchedSite{insn, false};
#ifdef VITA_DIAGNOSTICS
    census.upcall_sites++;
#endif
}

void ARM_VitaNative::PatchUpcallThumb(u32 pc) {
    // A Thumb svc is 2 bytes and a B.W 4: the instruction after it moves into the stub, which
    // runs it on resuming and branches back past it. Only an instruction that means the same
    // anywhere: no PC-relative load, ADR or PC operand, no branch but `bx lr` and pop, no IT or
    // 32-bit instruction. (Nothing here can see whether it is another branch's target: an svc
    // wrapper's `bx lr` or pop is not one.)
    const u16 svc = memory.Read16(pc);
    const u16 next = memory.Read16(pc + 2);
    const auto movable = [](u16 h) {
        if (h < 0x4400 || (h >= 0x5000 && h < 0xA000) || (h >= 0xA800 && h < 0xB100) ||
            (h >= 0xB200 && h < 0xB300) || (h >= 0xB400 && h < 0xB600) ||
            (h >= 0xBA00 && h < 0xBB00) || (h >= 0xBC00 && h < 0xBE00) ||
            (h >= 0xC000 && h < 0xD000)) {
            return true; // no PC in these forms (push/pop {..., pc} returns)
        }
        if (h == 0x4770 || h == 0xBF00) {
            return true; // bx lr, nop
        }
        if (h >= 0x4400 && h < 0x4700) {
            // add/cmp/mov with high registers: not with PC as an operand
            const u32 rm = (h >> 3) & 0xF;
            const u32 rdn = ((h >> 4) & 0x8) | (h & 0x7);
            return rm != 15 && rdn != 15;
        }
        return false;
    };
    // An svc an IT block covers (an IT up to four instructions before it) stays: the branch in
    // its place would take its condition, and the moved instruction, outside the block, would
    // not run when it fails. A halfword that only looks like an IT refuses a site too.
    bool in_it = false;
    for (u32 back = 2; back <= 14; back += 2) {
        const u16 h = memory.Read16(pc - back);
        in_it |= (h & 0xFF00u) == 0xBF00u && (h & 0xFu) != 0;
    }
    if ((svc & 0xFF00u) != 0xDF00u || !movable(next) || in_it) {
        svc_refused.insert(pc);
        return;
    }
    const u32 number = svc & 0xFFu;
    EnsureUpcallBody();
    // bx pc; nop: the ARM way in at base + 4 (the thunks are word-aligned), with the resume PC
    // `moved | 1` (UpcallEntry); moved: the instruction; b.w site + 4.
    const auto room = ThunkRoom(48 * 4);
    if (!room) {
        svc_refused.insert(pc);
        return;
    }
    const u32 base = *room;
    ASSERT((base & 3u) == 0);
    const u32 fn = HostAddress(upcall_table[number]);
    const auto sized = UpcallEntry(base + 4, {ArmMovw(12, 0), ArmMovt(12, 0)}, fn, upcall_body);
    if (!sized) {
        svc_refused.insert(pc);
        return;
    }
    const u32 moved = base + 4 + static_cast<u32>(sized->size() * 4);
    const auto entry =
        UpcallEntry(base + 4, {ArmMovw(12, moved | 1u), ArmMovt(12, moved | 1u)}, fn, upcall_body);
    const auto back = ThumbBranchW(moved + 2, pc + 4);
    const auto site = ThumbBranchW(pc, base);
    if (!entry || !back || !site) {
        svc_refused.insert(pc);
        return;
    }
    std::vector<u8> stub(moved + 6 - base);
    const u16 to_arm[2] = {0x4778, 0x46C0}; // bx pc; nop
    std::memcpy(stub.data(), to_arm, 4);
    std::memcpy(stub.data() + 4, entry->data(), entry->size() * 4);
    const u16 tail[3] = {next, (*back)[0], (*back)[1]};
    std::memcpy(stub.data() + (moved - base), tail, 6);
    std::memcpy(thunk_area->Data() + thunk_area_used, stub.data(), stub.size());
    map->SyncCode(base, static_cast<u32>(stub.size()));
    thunk_area_used += (static_cast<u32>(stub.size()) + 3) & ~3u;
    upcall_thunks[base] = UpcallThunk{pc, base + static_cast<u32>(stub.size()), moved};
    ClaimThunk(*map, base, pc, u32{svc} << 16 | next, true, ClaimKind::Svc);
    memory.Write16(pc, (*site)[0]);
    memory.Write16(pc + 2, (*site)[1]);
    map->SyncCode(pc, 4);
    patched_sites[pc] = PatchedSite{u32{svc} << 16 | next, true};
#ifdef VITA_DIAGNOSTICS
    census.upcall_sites++;
    census.upcall_thumb_sites++;
#endif
}

u64 ARM_VitaNative::UpcallSvc(u32 svc, const std::array<u32, 8>& in) {
    ARM_VitaNative& core = *upcall_core;
#ifdef VITA_DIAGNOSTICS
    census.upcall_runs++;
    census.upcall_svcs[svc & 0xff]++;
#endif
    // The svc's register file for the HLE handler: r0-r7 as they came, the rest from the frame.
    u32* const frame = UpcallFrame();
    const u32 guest_sp = frame[FrameSp];
#ifdef VITA_DIAGNOSTICS
    if (svc == 0x0a) {
        const u64 ns = in[0] | u64{in[1]} << 32;
        census.sleep_zero += ns == 0 ? 1 : 0;
        const u32 lr = frame[FrameLr];
        auto it = std::find_if(census.sleeps.begin(), census.sleeps.end(),
                               [&](const auto& s) { return s.count == 0 || s.lr == lr; });
        if (it == census.sleeps.end()) {
            census.sleep_other++;
        } else {
            it->lr = lr;
            it->count++;
            it->ns += ns;
        }
    }
#endif
    // Its own register file, not `run.ctx`: an exit inside the upcall writes the live state there.
    auto& regs = core.upcall_regs;
    for (u32 i = 0; i < in.size(); i++) {
        regs[i] = in[i];
    }
    for (u32 i = 8; i < 13; i++) {
        regs[i] = frame[FrameR0 + i];
    }
    regs[13] = guest_sp;
    regs[14] = frame[FrameLr];
    regs[15] = frame[FrameResume] & ~1u;
    core.upcall_regs_live = true;
    auto& kernel = upcall_system->Kernel();
    Kernel::Thread* const thread = kernel.GetCurrentThreadManager().GetCurrentThread();
    upcall_reschedule_asked.store(false, std::memory_order_relaxed);
    // The timer up to the guest's time, then the guest's clock stops for the C++: the emulator's own
    // time (its waits for the render thread among it) must not count as the guest's.
    Core::Timing::Timer& slice_timer = *core.timer;
    slice_timer.AddScaledTicks(PauseUpcallClock());
    const auto rest_of_slice = [&] {
        return std::clamp(TicksToNanoseconds(slice_timer.GetDowncount()), MinQuantumNs,
                          MaxQuantumNs);
    };
    // Back into the guest while the slice has time left, else the slice ends here: a fresh quantum at
    // every svc would never let the timer fire, and RunLoop's events and Sony's SGIs would stand
    // still.
    const auto back_to_guest = [&] {
        if (slice_timer.GetDowncount() > 0) {
            ResumeUpcallClock(slice_timer, rest_of_slice());
            return UpcallAnswer(regs[0], UpcallDone);
        }
#ifdef VITA_DIAGNOSTICS
        census.upcall_exits_slice_up++;
#endif
        ResumeUpcallClock(slice_timer, MinQuantumNs);
        return UpcallAnswer(regs[0], UpcallReschedule);
    };
    // A mutex the thunk did not handle itself: lent to it from the next call on.
    if (svc == 0x14 || svc == 0x24) {
        LeaseMutex(kernel, regs[0]);
    }
    core.svc_context.CallSVCForUpcall(svc);
#ifdef VITA_DIAGNOSTICS
    upcall_system->GPU().CheckHoldsSent("an svc's upcall");
#endif
    core.upcall_regs_live = false;
    // The outputs: r0 the result, r1-r12 into the frame. Some svcs' outputs reach past r3
    // (QueryMemory's go to r5).
    constexpr u32 Outputs = 12;
    for (u32 i = 1; i <= Outputs; i++) {
        frame[FrameR0 + i] = regs[i];
    }
    const bool reschedule = upcall_reschedule_asked.load(std::memory_order_relaxed) ||
                            thread == nullptr || thread->status != Kernel::ThreadStatus::Running ||
                            kernel.GetCurrentThreadManager().GetCurrentThread() != thread;
    if (!reschedule) {
        return back_to_guest();
    }
#ifdef VITA_DIAGNOSTICS
    census.upcall_reschedules++;
#endif
    // The next thread runs by the module's switch, no exit; another process's, or the slice's time
    // up, exits to reschedule (two threads rescheduling each other would keep the slice going).
    if (slice_timer.GetDowncount() <= 0) {
#ifdef VITA_DIAGNOSTICS
        census.upcall_exits_slice_up++;
#endif
        ResumeUpcallClock(slice_timer, MinQuantumNs);
        return UpcallAnswer(regs[0], UpcallReschedule);
    }
    auto& threads = kernel.GetCurrentThreadManager();
    std::unique_lock lock{kernel.GetHLELock()};
    // A thread that exited has current_thread as its last owner: held until its context is written.
    std::shared_ptr<Kernel::Thread> exited;
    if (thread != nullptr && thread->status == Kernel::ThreadStatus::Dead) {
        exited = Kernel::SharedFrom(thread);
    }
    Kernel::Thread* next = threads.SwitchForNative();
    if (next == thread && thread != nullptr) {
        return back_to_guest();
    }
    const bool idle = next == nullptr && thread != nullptr && !threads.HaveReadyThreads();
    if ((next == nullptr && !idle) || thread == nullptr) {
#ifdef VITA_DIAGNOSTICS
        census.upcall_exits_no_next++;
#endif
        ResumeUpcallClock(slice_timer, MinQuantumNs);
        return UpcallAnswer(regs[0], UpcallReschedule);
    }
    // The outgoing thread as it stands after the svc: its answer, its registers from the frame, d8-d15
    // and FPSCR from the thunk's save. Saved before an idle too: a wait it ends writes its result here.
    auto& out = thread->context;
    for (u32 i = 0; i < 4; i++) {
        out.cpu_registers[i] = regs[i];
    }
    for (u32 i = 4; i < 13; i++) {
        out.cpu_registers[i] = frame[FrameR0 + i];
    }
    const u32 resume = frame[FrameResume];
    out.cpu_registers[13] = guest_sp;
    out.cpu_registers[14] = frame[FrameLr];
    out.cpu_registers[15] = resume & ~1u;
    out.cpsr = (frame[FrameFlags] & 0xF80F0000u) | 0x10u | ((resume & 1u) << 5);
    std::memcpy(&out.fpu_registers[16], UpcallVfpSave(), 64);
    out.fpscr = UpcallGuestFpscr();
    exited.reset();
    // With the second emulation thread an idle is RunLoop's (System::HostIdle), unless it sleeps with
    // nothing due before this core's next event.
    const bool multi = Core::multi_host.load(std::memory_order_relaxed);
    const auto own_next = [&] {
        return slice_timer.GetTicks() +
               static_cast<u64>(std::max<s64>(slice_timer.GetMaxSliceLength(), 0));
    };
    if (idle && multi && !upcall_system->SecondHostQuiet(own_next())) {
        ResumeUpcallClock(slice_timer, MinQuantumNs);
#ifdef VITA_DIAGNOSTICS
        census.upcall_idle_exits++;
        census.upcall_idle_exit_why[upcall_system->SecondHostBusyWhy(own_next())]++;
#endif
        return UpcallAnswer(regs[0], UpcallReschedule);
    }
    if (idle) {
        // No thread to run: the guest idles here with no exit. Every core's timer moves on together,
        // event by event, until a thread of this core's is ready; another core with work, or an HLE host
        // thread at work, ends the idle in an exit. Page holds whose work is done are released later by
        // RunLoop (each release is a module call, an exit from here).
        lock.unlock();
        Core::Timing::Timer& timer = *core.timer;
        PauseUpcallClock(); // the C++'s time so far: none of the guest's
        const u32 self = core.GetID();
        const u32 cores = upcall_system->GetNumCores();
        const Core::TimingEventType* const vblank = upcall_system->GPU().VBlankEventType();
        const auto others_busy = [&] {
            if (multi) {
                return !upcall_system->SecondHostQuiet(own_next());
            }
            std::scoped_lock others_lock{kernel.GetHLELock()};
            for (u32 c = 0; c < cores; c++) {
                if (c == self) {
                    continue;
                }
                auto& other = kernel.GetThreadManager(c);
                const auto* const t = other.GetCurrentThread();
                if (other.HaveReadyThreads() ||
                    (t != nullptr && t->status == Kernel::ThreadStatus::Running)) {
                    return true;
                }
            }
            return false;
        };
#ifdef VITA_DIAGNOSTICS
        const u64 idle_from = timer.GetTicks();
        u32 idle_events = 0;
#endif
        do {
#ifdef VITA_DIAGNOSTICS
            // An idle no event ends: every thread, what it waits on, and the timer.
            if (++idle_events == 1200) {
                std::scoped_lock dump_lock{kernel.GetHLELock()};
                std::string dump;
                for (const auto& t : threads.GetThreadList()) {
                    dump +=
                        fmt::format(" [{} prio {} status {} pc {:08x} lr {:08x} r0 {:08x} "
                                    "addr {:08x} waits",
                                    t->thread_id, t->current_priority, static_cast<u32>(t->status),
                                    t->context.cpu_registers[15], t->context.cpu_registers[14],
                                    t->context.cpu_registers[0], t->wait_address);
                    for (const auto& o : t->wait_objects) {
                        dump += fmt::format(" {}:{}", o->GetTypeName(), o->GetName());
                    }
                    dump += fmt::format(" callback {}]", t->wakeup_callback != nullptr);
                }
                ASSERT_MSG(false,
                           "upcall idle: 1200 timer advances with no thread to run, svc {:#x} "
                           "of thread {}; ticks {} -> {}, next event in {}; ready {}:{}",
                           svc, thread->thread_id, idle_from, timer.GetTicks(),
                           timer.GetMaxSliceLength(), threads.HaveReadyThreads(), dump);
            }
#endif
            if (others_busy()) {
                ResumeUpcallClock(timer, MinQuantumNs);
#ifdef VITA_DIAGNOSTICS
                census.upcall_idle_exits++;
                census.upcall_idle_exits_other_busy++;
#endif
                return UpcallAnswer(regs[0], UpcallReschedule);
            }
            // Where every core goes next: this core's next event, or another's if sooner.
            u64 to = timer.GetTicks() + static_cast<u64>(std::max<s64>(timer.GetDowncount(), 0));
            for (u32 c = 0; c < cores; c++) {
                if (c != self && !multi) {
                    const auto& other = upcall_system->GetCore(c).GetTimer();
                    to = std::min(to,
                                  other.GetTicks() + static_cast<u64>(other.GetMaxSliceLength()));
                }
            }
            // The vblank is RunLoop's: its many Sony syscalls would each be an exit from here.
            for (u32 c = 0; c < cores; c++) {
                if (multi && c != self) {
                    continue; // the second thread's timers are its own
                }
                const auto& t = upcall_system->GetCore(c).GetTimer();
                if (t.NextEventType() == vblank &&
                    t.GetTicks() + static_cast<u64>(t.GetMaxSliceLength()) <= to) {
                    ResumeUpcallClock(timer, MinQuantumNs);
#ifdef VITA_DIAGNOSTICS
                    census.upcall_idle_exits++;
                    census.upcall_idle_exits_vblank++;
#endif
                    return UpcallAnswer(regs[0], UpcallReschedule);
                }
            }
            // A slice already past it keeps what it ran over.
            if (to > timer.GetTicks()) {
                timer.ForceExceptionCheck(static_cast<s64>(to - timer.GetTicks()));
                timer.Idle();
            } else {
                timer.ForceExceptionCheck(0);
            }
            timer.Advance();
            // The other cores to the same time, their events their own.
            for (u32 c = 0; c < cores; c++) {
                if (multi) {
                    break; // the second thread's cores follow on their own (HostIdle)
                }
                auto& other = upcall_system->GetCore(c).GetTimer();
                if (c == self || other.GetTicks() >= to) {
                    continue;
                }
                upcall_system->SelectCore(c);
                other.SetNextSlice(static_cast<s64>(to - other.GetTicks()));
                other.Idle();
                other.Advance();
            }
#ifdef VITA_DIAGNOSTICS
            // Every core to the next event: the guest's clock jumped (GPU::GuestIdle).
            upcall_system->GPU().GuestIdle();
#endif
            upcall_system->SelectCore(self);
            if (multi) {
                upcall_system->PublishClock(0);
            }
            lock.lock();
            next = threads.SwitchForNative();
            DEBUG_ASSERT_MSG(next != nullptr || !threads.HaveReadyThreads(),
                             "upcall idle: another process's thread is ready");
            lock.unlock();
            timer.SetNextSlice(Core::Timing::MAX_SLICE_LENGTH);
#ifdef VITA_DIAGNOSTICS
            census.upcall_idle_events++;
#endif
            // An HLE request's host thread at work may share this core, which takes no interrupt in the
            // guest's world but the module's timer: the core goes back to Sony's world to idle.
            if (next == nullptr && kernel.AreAsyncOperationsPending()) {
                ResumeUpcallClock(timer, MinQuantumNs);
#ifdef VITA_DIAGNOSTICS
                census.upcall_idle_exits++;
#endif
                return UpcallAnswer(regs[0], UpcallReschedule);
            }
        } while (next == nullptr);
        // Another core with a thread to run: a minimal quantum, and back to RunLoop.
        if (others_busy()) {
            timer.ForceExceptionCheck(0);
        }
        const u64 quantum_ns =
            std::clamp(TicksToNanoseconds(timer.GetDowncount()), MinQuantumNs, MaxQuantumNs);
        ResumeUpcallClock(timer, quantum_ns);
#ifdef VITA_DIAGNOSTICS
        census.upcall_idles++;
#endif
    }
    // The incoming thread, loaded by the module in place with its TLS and TPIDRURW. A thread that
    // never ran has its entry's Thumb bit in the PC.
    auto& incoming = next->context;
    incoming.cpu_registers[15] &= ~1u;
    if ((incoming.cpsr & 0x20u) != 0) {
        incoming.cpu_registers[15] = core.PastPatchedThumbSvc(incoming.cpu_registers[15]);
    }
    incoming.tpidruro = next->GetTLSAddress();
    u32 tpidrurw;
    asm volatile("mrc p15, 0, %0, c13, c0, 2" : "=r"(tpidrurw));
    incoming.tpidrurw = tpidrurw;
    upcall_switch.in = AsAzahar(incoming);
    upcall_switch.thread_id = next->GetThreadId();
    upcall_switch.armed = 1;
    if (upcall_clock_paused) {
        ResumeUpcallClock(slice_timer, rest_of_slice());
    }
#ifdef VITA_DIAGNOSTICS
    census.upcall_switches++;
#endif
    return UpcallAnswer(regs[0], UpcallReschedule);
}

u32 ARM_VitaNative::FetchCode(u32 pc, bool thumb) const {
    if (InThunkArea(pc) && thunk_area) {
        // Host 1 runs its own thunks there.
        const bool own = Core::ThisHost() == 1 && pc - ThunkAreaBase < Host1ThunkSize;
        const u8* const p = (own ? host1_area : thunk_area)->Data() + (pc - ThunkAreaBase);
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

void ARM_VitaNative::LeaveThunks(u32 first_core, bool host1_ran) {
    // Each core's running thread through the core, the others through their saved contexts; in host
    // 1's own thunks once it ran there, else host 0's.
    const auto finish = [&](u32 core_id, AzaharContext& c) {
        const u32 pc = c.r[15];
        const bool host1 = core_id != 0 && host1_ran;
        if (host1) {
            c.r[15] = Host1ThunkReturn(c.r[15]);
        } else {
            FinishThunk(memory, c);
        }
        LOG_INFO(Core_ARM11, "thunks: core {} left a thunk at {:08X} for {:08X} ({})", core_id, pc,
                 c.r[15], host1 ? "host 1's" : "host 0's");
    };
    for (u32 i = first_core; i < system.GetNumCores(); i++) {
        ARM_Interface& core = system.GetCore(i);
        if (!InThunkArea(core.GetPC())) {
            continue;
        }
        ThreadContext t;
        core.SaveContext(t);
        AzaharContext c{};
        ToAzahar(t, c);
        finish(i, c);
        FromAzahar(c, t);
        core.LoadContext(t);
    }
    auto& kernel = system.Kernel();
    for (u32 i = first_core; i < system.GetNumCores(); i++) {
        auto& manager = kernel.GetThreadManager(i);
        const Kernel::Thread* const running = manager.GetCurrentThread();
        for (const auto& thread : manager.GetThreadList()) {
            if (thread.get() == running || !InThunkArea(thread->context.cpu_registers[15] & ~1u)) {
                continue;
            }
            AzaharContext c{};
            ToAzahar(thread->context, c);
            finish(i, c);
            FromAzahar(c, thread->context);
        }
    }
}

void ARM_VitaNative::PrepareSaveState() {
    if (patched_sites.empty()) {
        return;
    }
    // Every thread that stands in a thunk leaves it.
    LeaveThunks(0, host1_area_used);
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
    tick_thunks.clear();
    upcall_thunks.clear();
    upcall_body = upcall_after_svc = 0;
    upcall_redirect.entry = 0;
    thunk_area_used = 0;
    host1_area_stale = true;
}

bool ARM_VitaNative::RunSlice(u64 quantum_ns) {
    if (Core::ThisHost() == 1) {
        // A thread stopped in a mutex thunk runs on until it is out: another thread through the same
        // site would overwrite the registers the thunk keeps in its slots.
        for (;;) {
            host1_in_thunk = false;
            const bool go_on = RunSliceSecond(quantum_ns);
            if (!host1_in_thunk) {
                return go_on;
            }
            quantum_ns = MinQuantumNs;
        }
    }
    // A slice that ended inside an upcall runs the same thread on at once, a minimal quantum at a
    // time, until it is back in its own code (RunSliceOnce sets upcall_again).
    for (;;) {
#ifdef VITA_DIAGNOSTICS
        upcall_held = upcall_again;
#endif
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

    {
        u32 tp, fpscr;
        asm volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(tp));
        asm volatile("vmrs %0, fpscr" : "=r"(fpscr));
        azahar_upcall_host_tp = tp;
        upcall_host_fpscr = fpscr;
    }
    upcall_core = this;
    upcall_tick_hz = run.tick_hz;
    // A quantum ending inside an upcall restarts in the vector; counted against the same guard.
    run.upcall_rearm_us = static_cast<u32>(MinQuantumNs / 1000);
    run.upcall_rearm_max =
        upcall_quantums < MaxUpcallQuantums ? MaxUpcallQuantums - upcall_quantums : 0;
    run.upcall_switch = &upcall_switch;
    EnsureUpcallBody();
    run.upcall_redirect = &upcall_redirect;
#ifdef VITA_DIAGNOSTICS
    // A held upcall's C++ may stand halfway through a change of the holds: checked when it is back.
    if (!upcall_held) {
        system.GPU().CheckHoldsSent("a slice");
    }
#endif
    const int ret = azaharRun(&run);
    if (ret < 0) {
        system.SetStatus(System::ResultStatus::ErrorCoreExceptionRaised,
                         fmt::format("azaharRun failed: {}", ret).c_str());
        usable = false;
        return false;
    }

    cp15[CP15_THREAD_UPRW] = run.ctx.tpidrurw;
    // The module may have switched threads in the slice: the TLS is the running one's.
    cp15[CP15_THREAD_URO] = run.ctx.tpidruro;
    // A slice stopped inside an upcall: its C++ is this thread, held maybe inside the logger or malloc
    // with their locks. Nothing here may lock or allocate until it goes on.
    if (!SgxdRenderer::Vprof::Capturing()) {
        upcall_quantums += run.upcall_rearms;
    }
#ifdef VITA_DIAGNOSTICS
    census.upcall_preempted += run.upcall_rearms;
    census.upcall_rearms += run.upcall_rearms;
#endif
    if (InUpcall(run.ctx.r[15]) &&
        !(run.reason == AZAHAR_EXIT_SVC && IsUpcallTailExit(run.ctx.r[15]))) {
        // The C++'s own time is not the guest's (UpcallSvc stops the clock for it).
        if (!upcall_clock_paused) {
            timer->AddScaledTicks(run.guest_ticks);
        }
        // A Sony syscall from the app's code in the guest's world: made in Sony's, then the thread on.
        if (run.reason == AZAHAR_EXIT_SVC && run.svc_number == 0 && run.ctx.r[15] >= 0x40000000u) {
            upcall_last_syscall = run.ctx.r[12];
            upcall_last_syscall_lr = run.ctx.r[14];
            upcall_forwards++;
#ifdef VITA_DIAGNOSTICS
            {
                // By caller, in place. A call from Sony's library code also names the app's code above it.
                u32 app = 0;
                if (run.ctx.r[14] >= 0xE0000000u) {
                    const auto* const stack =
                        reinterpret_cast<const u32*>(static_cast<uintptr_t>(run.ctx.r[13] & ~3u));
                    for (u32 i = 0; i < 64 && app == 0; i++) {
                        if (stack[i] >= 0x81000000u && stack[i] < 0x82000000u) {
                            app = stack[i];
                        }
                    }
                }
                auto it = std::find_if(
                    census.forwards.begin(), census.forwards.end(), [&](const auto& f) {
                        return f.count == 0 ||
                               (f.lr == run.ctx.r[14] && f.number == run.ctx.r[12] && f.app == app);
                    });
                if (it == census.forwards.end()) {
                    census.forward_other++;
                } else {
                    it->lr = run.ctx.r[14];
                    it->number = run.ctx.r[12];
                    it->app = app;
                    it->count++;
                }
            }
#endif
            ASSERT_MSG(upcall_forwards < 100000,
                       "upcall: 100000 Sony syscalls in one, the last {:#x} from {:08X} (r0 "
                       "{:08X}); app code on the upcall stack:{}",
                       run.ctx.r[12], run.ctx.r[14], run.ctx.r[0], UpcallReturns(run.ctx));
            ForwardSonySyscall(run.ctx);
#ifdef VITA_DIAGNOSTICS
            census.upcall_syscalls++;
#endif
            upcall_again = true;
            return true;
        }
        // A block the app mapped after azaharMap (vprof's capture buffer): the module takes the megabyte
        // from the process's table now and the access runs again.
        if ((run.reason == AZAHAR_EXIT_DABT || run.reason == AZAHAR_EXIT_PABT) &&
            run.far >= 0x40000000u) {
            const u32 status = (run.fsr & 0xFu) | ((run.fsr >> 6) & 0x10u);
            if ((status == 0x5 || status == 0x7) && azaharSyncUpper(run.far) > 0) {
#ifdef VITA_DIAGNOSTICS
                census.upcall_synced++;
#endif
                upcall_again = true;
                return true;
            }
        }
        // Anything but a quantum's end inside an upcall.
        if (run.reason != AZAHAR_EXIT_TIMER) {
            ASSERT_MSG(false,
                       "upcall: {} at {:08X} (svc {:#x}, lr {:08X}, sp {:08X}, fsr {:08X}, far "
                       "{:08X}); app code on the upcall stack:{}",
                       ReasonName(run.reason), run.ctx.r[15], run.svc_number, run.ctx.r[14],
                       run.ctx.r[13], run.fsr, run.far, UpcallReturns(run.ctx));
        }
        // An upcall that never comes back (a spin on another thread's lock) stops at five seconds, not
        // while a frame capture slows the render thread it may wait on.
        if (!SgxdRenderer::Vprof::Capturing()) {
            upcall_quantums++;
        }
        ASSERT_MSG(upcall_quantums < MaxUpcallQuantums,
                   "upcall: 20000 quantums in one, pc {:08X} lr {:08X}, last forwarded syscall "
                   "{:#x} from {:08X}; app code on the upcall stack:{}",
                   run.ctx.r[15], run.ctx.r[14], upcall_last_syscall, upcall_last_syscall_lr,
                   UpcallReturns(run.ctx));
#ifdef VITA_DIAGNOSTICS
        census.upcall_preempted++;
#endif
        upcall_again = true; // the same thread on, until it is back in its own code
        return true;
    }
    if (run.fast_site != 0) {
        PatchSvc(run.fast_site - 4);
    }
    CheckTickPolls();
    // The last site the module redirected: patched to its own stub if it can be.
    if (upcall_redirect.site != 0) {
        const u32 next = upcall_redirect.site;
        upcall_redirect.site = 0;
        const bool thumb = (next & 1u) != 0;
        PatchUpcall((next & ~1u) - (thumb ? 2u : 4u), thumb);
        if ((run.ctx.cpsr & 0x20u) != 0) {
            run.ctx.r[15] = PastPatchedThumbSvc(run.ctx.r[15]);
        }
    }
    upcall_quantums = 0;
    upcall_forwards = 0;
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
    census.kernel_cycles += run.kernel_cycles;
    if (run.reason == AZAHAR_EXIT_SVC) {
        census.svcs[run.svc_number < census.svcs.size() ? run.svc_number : 0]++;
    }
#endif

    // The monitor an LDREX made past the holds left holds for this exit alone (Exclusive).
    exclusive_held = std::exchange(exclusive, Exclusive{});
    switch (run.reason) {
    case AZAHAR_EXIT_SVC:
        // An upcall asked for a reschedule: this exit is for it alone.
        if (run.svc_number == UpcallRescheduleSvc &&
            IsUpcallTailExit(run.ctx.r[15])) {
            // The resume PC from the frame (Thumb by bit 0), before another upcall uses it.
            const u32 resume = UpcallFrame()[FrameResume];
            run.ctx.r[15] = (resume & 1u) != 0 ? PastPatchedThumbSvc(resume & ~1u) : resume;
            run.ctx.cpsr = (run.ctx.cpsr & ~(0x20u | 0x0600FC00u)) | ((resume & 1u) << 5);
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
        if (UpcallFor(run.svc_number)) {
            // Not a Thumb svc an IT block covers.
            const bool thumb = (run.ctx.cpsr & 0x20u) != 0;
            if (!thumb) {
                PatchUpcall(run.ctx.r[15] - 4, false);
            } else if ((run.ctx.cpsr & 0x0600FC00u) == 0) {
                PatchUpcall(run.ctx.r[15] - 2, true);
                run.ctx.r[15] = PastPatchedThumbSvc(run.ctx.r[15]);
            }
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
        if (status == 0xFu && TakeHeldPage(run.far, write, run.ctx.r[15])) {
            return !break_flag && !stop_requested;
        }
        if (LumaAliasAccess(write)) {
            return !break_flag && !stop_requested;
        }
#ifdef VITA_DIAGNOSTICS
        // A permission fault no hold took: the holds' record of the page.
        if (const u8* const host = map->HostAt(run.far); status == 0xFu && host != nullptr) {
            LOG_CRITICAL(Core_ARM11, "page holds at {:08X}: {}", run.far,
                         system.GPU().GuestHoldState(host));
        }
        if (const u32 next_page = (run.far & ~0xFFFu) + 0x1000u;
            status == 0xFu && (run.far & 0xFFFu) + AccessReach(run.ctx.r[15]) > 0x1000u) {
            if (const u8* const host = map->HostAt(next_page); host != nullptr) {
                LOG_CRITICAL(Core_ARM11, "page holds at {:08X} (the next page): {}", next_page,
                             system.GPU().GuestHoldState(host));
            }
        }
#endif
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

void ARM_VitaNative::PrepareSecondHost() {
    ASSERT(GuestTableInstalled());
    ThunkRoom(0); // the areas, host 1's reset if it is stale
    // The other cores ran on host 0 until now (after a state load): a thread stopped in one of host
    // 0's thunks leaves it, since host 1's view has its own area at those addresses.
    if (!patched_sites.empty()) {
        LeaveThunks(1, false);
    }
}

bool ARM_VitaNative::AttachSecondHost() {
    ASSERT(host1_area && !host1_area_stale);
    AzaharAttachRequest req{};
    req.count = 1;
    // RWX as host 0's: a mutex thunk keeps the guest's registers in it.
    req.overrides[0] = AzaharRange{reinterpret_cast<u32>(host1_area->Data()), ThunkAreaBase,
                                   Host1ThunkSize, GuestPerm};
    const int ret = azaharAttach(&req);
    if (ret < 0) {
        LOG_CRITICAL(Core_ARM11, "azaharAttach failed: {:#010x}", static_cast<u32>(ret));
        return false;
    }
    host1_area_used = true;
    LOG_INFO(Core_ARM11, "second host: attached on Vita core {}", sceKernelGetCpuId());
    return true;
}

void ARM_VitaNative::CallForwardedSvc(u32 immediate) {
    // Patched as host 0's svc exits patch theirs; host 1 builds its own thunk there when it next runs it.
    if (UpcallFor(immediate)) {
        const bool thumb = (run.ctx.cpsr & 0x20u) != 0;
        if (!thumb) {
            PatchUpcall(run.ctx.r[15] - 4, false);
        } else if ((run.ctx.cpsr & 0x0600FC00u) == 0) {
            PatchUpcall(run.ctx.r[15] - 2, true);
            run.ctx.r[15] = PastPatchedThumbSvc(run.ctx.r[15]);
        }
    }
    if (immediate == 0x14 || immediate == 0x24) {
        // No HLE lock: host 1 waits meanwhile, and nothing else touches the leases.
        LeaseMutex(system.Kernel(), run.ctx.r[0]);
    }
    svc_context.CallSVC(immediate);
}

bool ARM_VitaNative::TakeForwardedHeldPage() {
    const bool write = (run.fsr & (1u << 11)) != 0;
    forward_raced = false;
    if (TakeHeldPage(run.far, write, run.ctx.r[15])) {
        return true;
    }
    // The holds' record opens a page before its descriptor does: an access between the two aborts on
    // a page the record has open. Sent now, the descriptor agrees and the access runs again; the same
    // access aborting right after is a real disagreement (RunSliceSecond).
    const u8* const host = map->HostAt(run.far);
    if (host == nullptr || !system.GPU().GuestAccessOpen(host, write)) {
        return false;
    }
    forward_raced = true;
    raced_far = run.far;
    raced_pc = run.ctx.r[15];
    return true;
}

void ARM_VitaNative::DetachSecondHost() {
    const int ret = azaharDetach();
    ASSERT_MSG(ret >= 0, "azaharDetach failed: {:#010x}", static_cast<u32>(ret));
}

void ARM_VitaNative::BuildHost1Thunk() {
    const bool thumb = (run.ctx.cpsr & 0x20u) != 0;
    const u32 entry = run.ctx.r[15] - (thumb ? 2u : 4u);
    const ThunkClaim* const c = Host1ClaimAt(entry);
    ASSERT_MSG(c != nullptr && c->entry == entry && c->thumb == thumb,
               "host 1 trapped at {:08X} ({}), no thunk begins there", entry,
               thumb ? "Thumb" : "ARM");
    WriteHost1Thunk(*c, *map);
    run.ctx.r[15] = entry;
}

bool ARM_VitaNative::SvcSitePatched() {
    const bool thumb = (run.ctx.cpsr & 0x20u) != 0;
    const u32 site = run.ctx.r[15] - (thumb ? 2u : 4u);
    // The thunk area is this host's own: its svcs are not read through the guest's memory.
    if (InThunkArea(site) || (thumb ? (memory.Read16(site) & 0xFF00u) == 0xDF00u
                                    : (memory.Read32(site) & 0x0F000000u) == 0x0F000000u)) {
        return false;
    }
#ifdef VITA_DIAGNOSTICS
    LOG_INFO(Core_ARM11, "second host: the svc site {:08X} was patched under its svc (read as {:#x})",
             site, run.svc_number);
#endif
    run.ctx.r[15] = site;
    return true;
}

u32 ARM_VitaNative::PastPatchedThumbSvc(u32 pc) {
    // A Thumb site patched to a B.W after a thread stopped past the svc: the PC is the B.W's second
    // halfword, and the moved instruction runs in the thunk.
    const u32 site = pc - 2;
    const u16 hw1 = memory.Read16(site);
    if ((hw1 & 0xF800u) != 0xF000u) [[likely]] {
        return pc;
    }
    const u16 hw2 = memory.Read16(pc);
    if ((hw2 & 0xD000u) != 0x9000u) {
        return pc;
    }
    const u32 s = hw1 >> 10 & 1u, j1 = hw2 >> 13 & 1u, j2 = hw2 >> 11 & 1u;
    const u32 i1 = ~(j1 ^ s) & 1u, i2 = ~(j2 ^ s) & 1u;
    u32 off = s << 24 | i1 << 23 | i2 << 22 | (hw1 & 0x3FFu) << 12 | (hw2 & 0x7FFu) << 1;
    if (s != 0) {
        off |= 0xFE000000u;
    }
    const u32 entry = site + 4 + off;
    const ThunkClaim* const c = InThunkArea(entry) ? Host1ClaimAt(entry) : nullptr;
    if (c == nullptr || c->entry != entry || c->site != site || !c->thumb ||
        c->kind != ClaimKind::Svc) {
        return pc;
    }
    if (HostOfCore(GetID()) == 1) {
        WriteHost1Thunk(*c, *map);
        return entry + 2;
    }
    const auto it = upcall_thunks.find(entry);
    ASSERT_MSG(it != upcall_thunks.end() && it->second.site == site,
               "the Thumb svc site {:08X} branches to {:08X}, no upcall thunk of it", site, entry);
    return it->second.moved;
}

namespace {

void WriteHost1Thunk(const ThunkClaim& claim, const VitaGuestMap& map) {
    const ThunkClaim* const c = &claim;
    const u32 entry = c->entry;
    const bool thumb = c->thumb;
    u8* p = host1_area->Data() + (entry - ThunkAreaBase);
    u32 bytes = 8;
    // The site's own instruction(s), then the branch back: an ARM svc unconditional, a VFP instruction
    // as it was, a Thumb svc with its moved instruction. A mutex svc's fast path first.
    if (c->kind == ClaimKind::Mutex) {
        const Host1Mutex m = Host1MutexThunk(*c);
        p = host1_area->Data() + (m.base - ThunkAreaBase);
        bytes = static_cast<u32>(m.words.size()) * 4;
        std::memcpy(p, m.words.data(), bytes);
    } else if (thumb) {
        const auto back = ThumbBranchW(entry + 4, c->site + 4);
        ASSERT(back.has_value());
        const u16 hw[4] = {static_cast<u16>(c->insn >> 16), static_cast<u16>(c->insn),
                           (*back)[0], (*back)[1]};
        std::memcpy(p, hw, sizeof(hw));
    } else {
        const auto back = ArmBranch(0xE, entry + 4, c->site + 4);
        ASSERT(back.has_value());
        const u32 w[2] = {c->kind == ClaimKind::Svc ? 0xEF000000u | (c->insn & 0x00FFFFFFu) : c->insn, *back};
        std::memcpy(p, w, sizeof(w));
    }
    const u32 first = static_cast<u32>(p - host1_area->Data()) & ~0xFFFu;
    const u32 last = (static_cast<u32>(p - host1_area->Data()) + bytes - 1) & ~0xFFFu;
    map.SyncViewCode(host1_area->Data() + first, ThunkAreaBase + first, last + 0x1000 - first);
}

} // namespace

bool ARM_VitaNative::RunSliceSecond(u64 quantum_ns) {
    if (!usable) [[unlikely]] {
        system.SetStatus(System::ResultStatus::ErrorCoreExceptionRaised,
                         "Vita native backend is not usable");
        return false;
    }
    run.ctx.tpidruro = cp15[CP15_THREAD_URO];
    run.ctx.tpidrurw = cp15[CP15_THREAD_UPRW];
    DEBUG_ASSERT(quantum_ns <= MaxQuantumNs);
    run.quantum_us = static_cast<u32>(quantum_ns) / 1000u;
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
    run.upcall_rearm_us = run.upcall_rearm_max = 0;
    run.upcall_switch = nullptr;
    run.upcall_redirect = nullptr;

    const int ret = azaharRun(&run);
    if (ret < 0) {
        system.SetStatus(System::ResultStatus::ErrorCoreExceptionRaised,
                         fmt::format("azaharRun failed on the second emulation thread: {}", ret)
                             .c_str());
        usable = false;
        return false;
    }
    cp15[CP15_THREAD_UPRW] = run.ctx.tpidrurw;
    cp15[CP15_THREAD_URO] = run.ctx.tpidruro;
    timer->AddScaledTicks(run.guest_ticks);
    // An access host 0 let run again after a raced hold runs first.
    const bool after_race = std::exchange(forward_raced, false);

    // An LDREX's monitor past the holds lasts for this exit alone.
    exclusive_held = std::exchange(exclusive, Exclusive{});
    switch (run.reason) {
    case AZAHAR_EXIT_SVC:
        if (run.svc_number == ThunkTrapSvc && InThunkArea(run.ctx.r[15])) {
            BuildHost1Thunk();
            return !break_flag && !stop_requested;
        }
        if (SvcSitePatched()) {
            return !break_flag && !stop_requested;
        }
        // A VFP instruction patched to the module's svc that the module did not run: not seen here yet.
        ASSERT_MSG(run.svc_number != AZAHAR_VFP_SVC,
                   "svc {:#x} at {:08X} on the second emulation thread", AZAHAR_VFP_SVC,
                   run.ctx.r[15]);
        system.SecondHostSvc(GetID(), svc_context, run.svc_number);
        return !break_flag && !stop_requested;

    case AZAHAR_EXIT_TIMER:
        host1_in_thunk = InHost1MutexSlots(run.ctx.r[15]);
        return false;

    case AZAHAR_EXIT_DABT: {
        // A small-page permission fault is a GPU page hold's, which host 0 takes while this thread waits.
        const u32 status = (run.fsr & 0xFu) | ((run.fsr >> 6) & 0x10u);
        ASSERT_MSG(!after_race || run.far != raced_far || run.ctx.r[15] != raced_pc,
                   "the second thread's access at {:08X} (pc {:08X}) aborted again on a page the "
                   "holds' record has open, its changes sent",
                   run.far, run.ctx.r[15]);
        if (status == 0xFu && system.SecondHostHeldPage(GetID())) {
            return !break_flag && !stop_requested;
        }
        if (LumaAliasAccess((run.fsr & (1u << 11)) != 0)) {
            return !break_flag && !stop_requested;
        }
        ReportFault();
        return false;
    }
    case AZAHAR_EXIT_UND: {
        // RunSliceOnce's VFP short vectors, nothing patched.
        if ((run.ctx.fpscr & FpscrVectorBits) != 0) {
            u32 pc = run.ctx.r[15];
            const bool thumb = run.ctx.cpsr >> 5 & 1;
            u32 ran = 0;
            for (; ran < MaxVfpRun; ran++) {
                const u32 insn = FetchCode(pc, thumb);
                const u32 cond = thumb ? 0xEu : insn >> 28;
                if (ran > 0 && (thumb ? (insn >> 24) != 0xEE : !IsVfpDataProcessing(insn))) {
                    break;
                }
                if (cond == 0xF ||
                    !(!ConditionPasses(cond, run.ctx.cpsr) || RunVfpVector(run.ctx, insn))) {
                    break;
                }
                pc += 4;
            }
            if (ran > 0) {
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
    // The upcall stack's deepest use: an overflow would write over the globals below it.
    if (upcall_stack_filled) {
        u32 untouched = 0;
        while (untouched < upcall_stack.size() && upcall_stack[untouched] == UpcallStackFill) {
            untouched++;
        }
        census.upcall_stack_used = static_cast<u32>(upcall_stack.size()) - untouched;
        DEBUG_ASSERT_MSG(untouched > 1024, "upcall: the stack's deepest use left {} bytes of {}",
                         untouched, upcall_stack.size());
    }
    census.hold_pages = static_cast<u32>(hold_page_accesses.size());
    std::vector<std::array<u32, 3>> pages;
    for (const auto& [page, access] : hold_page_accesses) {
        pages.push_back({page, access.first, access.second});
    }
    std::sort(pages.begin(), pages.end(), [](const auto& a, const auto& b) { return a[1] > b[1]; });
    for (std::size_t i = 0; i < std::min(pages.size(), census.hold_top.size()); i++) {
        census.hold_top[i] = pages[i];
    }
    hold_page_accesses.clear();
    return std::exchange(census, VitaNativeCensus{});
}
#endif

bool ARM_VitaNative::TakeHeldPage(u32 va, bool write, u32 pc) {
    // A page held for the display alone opens; otherwise an access clear of the bytes the holds
    // protect is made here, the page held still.
    const u8* const host = map->HostAt(va);
    if (host != nullptr && system.GPU().GuestOpensDisplayPage(host, write, AccessReach(pc))) {
        return true;
    }
    // A page the guest keeps accessing past its holds opens without a wait where it can.
    HotPage* hot = nullptr;
    for (HotPage& h : hot_pages) {
        if (h.page == va >> 12) {
            hot = &h;
            break;
        }
    }
    if (hot == nullptr) {
        hot = &hot_pages[hot_next++ % hot_pages.size()];
        *hot = {va >> 12, 0};
    }
    if (hot->accesses >= AccessesPerOpen && host != nullptr) {
        hot->accesses = 0;
        if (system.GPU().GuestOpensHeldPage(host, std::min(AccessReach(pc), 0x1000u - (va & 0xFFFu)),
                                            write)) {
#ifdef VITA_DIAGNOSTICS
            census.hold_opens++;
#endif
            return true;
        }
    }
#ifdef VITA_DIAGNOSTICS
    // Before the access: a load into the PC may change the state.
    const bool thumb = (run.ctx.cpsr & 0x20u) != 0;
#endif
    refused_va = 0;
    if (AccessPastHolds(pc, write)) {
        hot->accesses++;
#ifdef VITA_DIAGNOSTICS
        (write ? census.hold_stores : census.hold_loads)++;
        census.hold_thumb += thumb ? 1 : 0;
        auto& [count, at] = hold_page_accesses[va & ~0xFFFu];
        count++;
        at = pc;
#endif
        return true;
    }
    // The A9 reports a multi-word store's first address even when a later word's page faulted.
    const u32 at = refused_va != 0 ? refused_va : va;
    const u8* const refused = map->HostAt(at);
    return refused != nullptr && system.GPU().GuestAbort(refused, write, pc, at);
}

bool ARM_VitaNative::TakeHoldBudget(u32 page, u32 ticket) {
    HoldBudget* entry = nullptr;
    for (HoldBudget& b : hold_budgets) {
        if (b.page == page) {
            entry = &b;
            break;
        }
    }
    if (entry == nullptr) {
        entry = &hold_budgets[hold_budget_next++ % hold_budgets.size()];
        *entry = {page, ticket, 0};
    } else if (entry->ticket != ticket) {
        entry->ticket = ticket;
        entry->accesses = 0;
    }
    if (entry->accesses == AccessesPerHold) {
#ifdef VITA_DIAGNOSTICS
        census.hold_load_waits++;
#endif
        return false;
    }
    entry->accesses++;
    return true;
}

bool ARM_VitaNative::LumaAliasAccess(bool write) {
    // Luma plugins access FCRAM through its physical address with bit 31 set, which native
    // guest code cannot reach: served here, a fault each (rare enough not to map 128 MiB more).
    if (!IsLumaAlias(run.far) || !AccessPastHolds(run.ctx.r[15], write)) {
        return false;
    }
#ifdef VITA_DIAGNOSTICS
    static u32 accesses;
    if (std::has_single_bit(++accesses)) {
        LOG_INFO(Core_ARM11, "Luma alias: access {}, {} {:08X}", accesses, write ? "to" : "from",
                 run.far);
    }
#endif
    return true;
}

bool ARM_VitaNative::AccessPastHolds(u32 pc, bool write) {
    const bool thumb = (run.ctx.cpsr & 0x20u) != 0;
    const u32 next = pc + (thumb ? 2 : 4);
    // The app's view of the guest's page: the guest's slot, open to the app. A load needs the
    // GPU's writes missed. A store goes as every write onto a held page does: past the holds,
    // missing everything the queued work touches, or onto the page its own effects or a version
    // opened (ticket 0: no hold to count it against).
    const auto misses = [&](u32 address, u32 bytes) -> u8* {
        if (IsLumaAlias(address)) {
            // Physical memory: no slot of the guest's, and no hold counts it (LumaAliasAccess).
            const PAddr physical = address & 0x7FFFFFFFu;
            return memory.IsValidPhysicalAddress(physical) ? memory.GetPhysicalPointer(physical)
                                                           : nullptr;
        }
        const u8* const host = map->HostAt(address);
        u32 ticket = 0;
        if (host == nullptr ||
            !(write ? system.GPU().GuestStoreMissesHolds(host, bytes, ticket)
                    : system.GPU().GuestReadMissesWrites(host, bytes, ticket)) ||
            (ticket != 0 && !TakeHoldBudget(address >> 12, ticket))) {
            return nullptr;
        }
        return const_cast<u8*>(host);
    };
    const auto span = [&](u32 address, u32 bytes, bool store, Span& s) {
        if (bytes == 0 || run.far - address >= bytes) {
            return false;
        }
        s.split = std::min(bytes, 0x1000u - (address & 0xFFFu));
        for (u32 k = 0; k < 2; k++) {
            const u32 at = k == 0 ? address : address + s.split;
            const u32 n = k == 0 ? s.split : bytes - s.split;
            if (n == 0) {
                break;
            }
            if (IsLumaAlias(at)) {
                const PAddr physical = at & 0x7FFFFFFFu;
                if (!memory.IsValidPhysicalAddress(physical)) {
                    return false;
                }
                s.host[k] = memory.GetPhysicalPointer(physical);
                continue;
            }
            const u8* const host = map->HostAt(at);
            if (host == nullptr || !system.GPU().GuestAccessMisses(host, n, store)) {
                refused_va = host != nullptr ? at : 0;
                return false;
            }
            s.host[k] = const_cast<u8*>(host);
        }
        return true;
    };
    // n bytes at `offset` into the span, from or to `data`, split at the page boundary.
    const auto put = [](const Span& s, u32 offset, const void* data, u32 n) {
        const u8* const from = static_cast<const u8*>(data);
        const u32 first = offset >= s.split ? 0 : std::min(n, s.split - offset);
        if (first != 0) {
            std::memcpy(s.host[0] + offset, from, first);
        }
        if (first != n) {
            std::memcpy(s.host[1] + (offset + first - s.split), from + first, n - first);
        }
    };
    const auto get = [](const Span& s, u32 offset, void* data, u32 n) {
        u8* const to = static_cast<u8*>(data);
        const u32 first = offset >= s.split ? 0 : std::min(n, s.split - offset);
        if (first != 0) {
            std::memcpy(to, s.host[0] + offset, first);
        }
        if (first != n) {
            std::memcpy(to + first, s.host[1] + (offset + first - s.split), n - first);
        }
    };
    // A load into the PC: a branch with ARMv5T's interworking, bit 0 the Thumb state.
    const auto load_pc = [&](u32 value) {
        if ((value & 1u) != 0) {
            run.ctx.cpsr |= 0x20u;
            run.ctx.r[15] = value & ~1u;
        } else {
            run.ctx.cpsr &= ~0x20u;
            run.ctx.r[15] = value & ~3u;
        }
    };
    // The PC as a store reads it: + 8 in ARM, + 4 in Thumb.
    const u32 pc_read = pc + (thumb ? 4 : 8);
    // Words [start, start + 4 count) to or from regs[0..count), the base written back.
    const auto core_block = [&](u32 start, const std::array<u32, 16>& regs, u32 count, u32 base,
                                bool writeback, u32 written_back) {
        Span s;
        if (count == 0 || !span(start, count * 4, write, s)) {
            return false;
        }
        bool to_pc = false;
        u32 pc_value = 0;
        for (u32 k = 0; k < count; k++) {
            const u32 r = regs[k];
            if (write) {
                const u32 v = r == 15 ? pc_read : run.ctx.r[r];
                put(s, 4 * k, &v, 4);
            } else {
                u32 v;
                get(s, 4 * k, &v, 4);
                if (r == 15) {
                    to_pc = true;
                    pc_value = v;
                } else {
                    run.ctx.r[r] = v;
                }
            }
        }
        if (writeback) {
            run.ctx.r[base] = written_back;
        }
        if (to_pc) {
            load_pc(pc_value);
        } else {
            run.ctx.r[15] = next;
        }
        return true;
    };
    // One value of size bytes to or from register t (a pair: t, t + 1), signed loads sign-extended.
    const auto one = [&](u32 address, u32 size, u32 t, bool sign, bool pair) {
        Span s;
        if (!span(address, size, write, s)) {
            return false;
        }
        if (write) {
            if (pair) {
                put(s, 0, &run.ctx.r[t], 4);
                put(s, 4, &run.ctx.r[t + 1], 4);
            } else if (size == 4) {
                const u32 v = t == 15 ? pc_read : run.ctx.r[t];
                put(s, 0, &v, 4);
            } else if (size == 2) {
                const u16 half = static_cast<u16>(run.ctx.r[t]);
                put(s, 0, &half, 2);
            } else {
                const u8 byte = static_cast<u8>(run.ctx.r[t]);
                put(s, 0, &byte, 1);
            }
        } else if (pair) {
            get(s, 0, &run.ctx.r[t], 4);
            get(s, 4, &run.ctx.r[t + 1], 4);
        } else if (size == 4) {
            u32 v;
            get(s, 0, &v, 4);
            if (t == 15) {
                load_pc(v);
                return true;
            }
            run.ctx.r[t] = v;
        } else if (size == 2) {
            u16 half;
            get(s, 0, &half, 2);
            run.ctx.r[t] =
                sign ? static_cast<u32>(static_cast<s32>(static_cast<s16>(half))) : half;
        } else {
            u8 byte;
            get(s, 0, &byte, 1);
            run.ctx.r[t] = sign ? static_cast<u32>(static_cast<s32>(static_cast<s8>(byte))) : byte;
        }
        return true;
    };
    const auto regs_of = [](u32 list, std::array<u32, 16>& regs) {
        u32 count = 0;
        for (u32 r = 0; r < 16; r++) {
            if ((list >> r & 1u) != 0) {
                regs[count++] = r;
            }
        }
        return count;
    };
    // A base in a store's list with writeback stores its original value only as the lowest register
    // (else UNKNOWN); a load's is unpredictable.
    const auto base_listed_ok = [&](u32 list, u32 base, bool load) {
        return (list >> base & 1u) == 0 || (!load && (list & ((1u << base) - 1)) == 0);
    };
    if (thumb) {
        // The ARM11's Thumb (ARMv6K): 16-bit, its loads and stores unconditional.
        const u32 insn = memory.Read16(pc);
        const bool l = (insn >> 11 & 1u) != 0;
        if ((insn & 0xF000u) == 0xC000u || (insn & 0xF600u) == 0xB400u) {
            // LDMIA, STMIA: 1100 L Rn list. PUSH: 1011 010 R list; POP: 1011 110 P list.
            const bool stack = (insn & 0xF000u) == 0xB000u;
            const bool extra = stack && (insn >> 8 & 1u) != 0;
            const u32 base = stack ? 13 : insn >> 8 & 7u;
            const u32 list = (insn & 0xFFu) | (extra ? (l ? 1u << 15 : 1u << 14) : 0);
            // An LDMIA's base in its list takes the loaded value (no writeback).
            if (l == write || (!stack && !l && !base_listed_ok(list, base, false))) {
                return false;
            }
            std::array<u32, 16> regs{};
            const u32 count = regs_of(list, regs);
            const u32 b = run.ctx.r[base];
            const u32 start = stack && !l ? b - 4 * count : b;
            // An LDMIA's base in its list takes the loaded value; an STMIA always writes its base back.
            return core_block(start, regs, count, base,
                              stack || !l || (list >> base & 1u) == 0,
                              stack && !l ? start : b + 4 * count);
        }
        const u32 rn = insn >> 3 & 7u, imm5 = insn >> 6 & 0x1Fu;
        u32 address = 0, size = 0, t = insn & 7u;
        bool load_form = l, sign = false;
        switch (insn >> 11) {
        case 0b01001u: // LDR Rt, [PC, #imm8 * 4]: from the word-aligned PC + 4
            t = insn >> 8 & 7u;
            address = ((pc + 4) & ~3u) + (insn & 0xFFu) * 4;
            size = 4;
            load_form = true;
            break;
        case 0b01010u:
        case 0b01011u: {
            // The register offsets: 0101 op Rm Rn Rt (STR, STRH, STRB, LDRSB, LDR, LDRH, LDRB, LDRSH).
            static constexpr std::array<u8, 8> sizes{4, 2, 1, 1, 4, 2, 1, 2};
            const u32 op = insn >> 9 & 7u;
            address = run.ctx.r[rn] + run.ctx.r[insn >> 6 & 7u];
            size = sizes[op];
            load_form = op >= 3;
            sign = op == 3 || op == 7;
            break;
        }
        case 0b01100u:
        case 0b01101u: // STR, LDR Rt, [Rn, #imm5 * 4]
            address = run.ctx.r[rn] + imm5 * 4;
            size = 4;
            break;
        case 0b01110u:
        case 0b01111u: // STRB, LDRB Rt, [Rn, #imm5]
            address = run.ctx.r[rn] + imm5;
            size = 1;
            break;
        case 0b10000u:
        case 0b10001u: // STRH, LDRH Rt, [Rn, #imm5 * 2]
            address = run.ctx.r[rn] + imm5 * 2;
            size = 2;
            break;
        case 0b10010u:
        case 0b10011u: // STR, LDR Rt, [SP, #imm8 * 4]
            t = insn >> 8 & 7u;
            address = run.ctx.r[13] + (insn & 0xFFu) * 4;
            size = 4;
            break;
        default:
            return false;
        }
        if (load_form == write || !one(address, size, t, sign, false)) {
            return false;
        }
        run.ctx.r[15] = next;
        return true;
    }
    // The condition passed: a failing one makes no access.
    const u32 insn = memory.Read32(pc);
    if (insn >> 28 == 0xFu) {
        return false;
    }
    const u32 n = insn >> 16 & 0xFu, t = insn >> 12 & 0xFu;
    const bool pre = (insn >> 24 & 1u) != 0, up = (insn >> 23 & 1u) != 0,
               w = (insn >> 21 & 1u) != 0;
    // The L bit (bit 20) of the forms below: set for the loads.
    const u32 load = write ? 0 : 0x00100000u;
    if ((insn & 0x0E100000u) == (0x08000000u | load)) {
        // LDM, STM: cond 100P USWL Rn register_list (S not built).
        const u32 list = insn & 0xFFFFu;
        if ((insn >> 22 & 1u) != 0 || n == 15 || (w && !base_listed_ok(list, n, !write))) {
            return false;
        }
        std::array<u32, 16> regs{};
        const u32 count = regs_of(list, regs);
        const u32 rn = run.ctx.r[n];
        const u32 start = up ? (pre ? rn + 4 : rn) : (pre ? rn - 4 * count : rn - 4 * count + 4);
        return core_block(start, regs, count, n, w, up ? rn + 4 * count : rn - 4 * count);
    }
    if ((insn & 0x0F800FF0u) == 0x01800F90u) {
        // LDREX, STREX and their D, B, H forms: cond 0001 1 sz L Rn Rt 1111 1001 Rt2. Aligned, or they
        // fault for that and never get here.
        static constexpr std::array<u32, 4> sizes{4, 8, 1, 2};
        const u32 sz = insn >> 21 & 3u;
        const u32 size = sizes[sz];
        const bool l = (insn >> 20 & 1u) != 0;
        const u32 address = run.ctx.r[n];
        if (n == 15 || l == write) {
            return false;
        }
        Span s;
        if (l) {
            if ((insn & 0xFu) != 0xFu || t == 15 || (sz == 1 && ((t & 1u) != 0 || t == 14)) ||
                !span(address, size, false, s) || s.split != size) {
                return false;
            }
            // Single-copy atomic as the hardware's.
            u64 value = 0;
            switch (size) {
            case 1:
                value = __atomic_load_n(s.host[0], __ATOMIC_SEQ_CST);
                break;
            case 2:
                value = __atomic_load_n(reinterpret_cast<u16*>(s.host[0]), __ATOMIC_SEQ_CST);
                break;
            case 4:
                value = __atomic_load_n(reinterpret_cast<u32*>(s.host[0]), __ATOMIC_SEQ_CST);
                break;
            default:
                value = __atomic_load_n(reinterpret_cast<u64*>(s.host[0]), __ATOMIC_SEQ_CST);
                break;
            }
            run.ctx.r[t] = static_cast<u32>(value);
            if (sz == 1) {
                run.ctx.r[t + 1] = static_cast<u32>(value >> 32);
            }
            exclusive = {address, size, value, true};
            run.ctx.r[15] = next;
            return true;
        }
        const u32 rt = insn & 0xFu;
        if (t == 15 || rt == 15 || t == n || t == rt ||
            (sz == 1 && ((rt & 1u) != 0 || rt == 14 || t == rt + 1))) {
            return false;
        }
        // Only right after its LDREX here, else the fault's path.
        if (!exclusive_held.valid || exclusive_held.address != address ||
            exclusive_held.size != size || !span(address, size, true, s) || s.split != size) {
            return false;
        }
        const u64 desired = sz == 1 ? u64{run.ctx.r[rt + 1]} << 32 | run.ctx.r[rt] : run.ctx.r[rt];
        // A compare and swap against the value its LDREX loaded: another core's store meanwhile fails it.
        const auto swap = [&]<typename T>(T*) {
            T* const at = reinterpret_cast<T*>(s.host[0]);
            for (u32 tries = 0; tries < 64; tries++) {
                T expected = static_cast<T>(exclusive_held.value);
                if (__atomic_compare_exchange_n(at, &expected, static_cast<T>(desired), true,
                                                __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
                    return true;
                }
                if (expected != static_cast<T>(exclusive_held.value)) {
                    return false;
                }
            }
            return false;
        };
        bool stored;
        switch (size) {
        case 1:
            stored = swap(static_cast<u8*>(nullptr));
            break;
        case 2:
            stored = swap(static_cast<u16*>(nullptr));
            break;
        case 4:
            stored = swap(static_cast<u32*>(nullptr));
            break;
        default:
            stored = swap(static_cast<u64*>(nullptr));
            break;
        }
        exclusive_held = {};
        run.ctx.r[t] = stored ? 0 : 1;
        run.ctx.r[15] = next;
        return true;
    }
    if ((insn & 0x0E100E00u) == (0x0C000A00u | load) && (pre || up)) {
        // VLDR, VSTR (P 1, W 0), VLDM, VSTM IA (P 0, U 1) and DB (P 1, U 0, W 1): cond 110P UDWL Rn Vd
        // 101 sz imm8.
        const bool dbl = (insn >> 8 & 1u) != 0;
        const u32 d = insn >> 22 & 1u, vd = insn >> 12 & 0xFu, imm8 = insn & 0xFFu;
        const u32 first = dbl ? ((d << 4) | vd) * 2 : (vd << 1) | d;
        const u32 base = n == 15 ? (pc + 8) & ~3u : run.ctx.r[n];
        u32 start = 0, words = 0, written_back = 0;
        if (pre && !w) {
            words = dbl ? 2 : 1;
            start = up ? base + imm8 * 4 : base - imm8 * 4;
        } else if (!pre && up) {
            words = imm8;
            start = base;
            written_back = base + imm8 * 4;
        } else if (pre && !up && w) {
            words = imm8;
            start = base - imm8 * 4;
            written_back = start;
        } else {
            return false;
        }
        Span s;
        if (words == 0 || (dbl && words % 2 != 0) || first + words > (dbl ? 64u : 32u) ||
            (w && n == 15) || !span(start, words * 4, write, s)) {
            return false;
        }
        for (u32 k = 0; k < words; k++) {
            if (write) {
                put(s, 4 * k, &run.ctx.fpregs[first + k], 4);
            } else {
                get(s, 4 * k, &run.ctx.fpregs[first + k], 4);
            }
        }
        if (w) {
            run.ctx.r[n] = written_back;
        }
        run.ctx.r[15] = next;
        return true;
    }
    u32 offset = 0, size = 0;
    bool sign = false, pair = false;
    const u32 m = insn & 0xFu;
    if ((insn & 0x0C100000u) == (0x04000000u | load) &&
        ((insn >> 25 & 1u) == 0 || (insn >> 4 & 1u) == 0)) {
        // LDR, LDRB, STR, STRB: cond 010P UBWL Rn Rt imm12, or 011P UBWL Rn Rt imm5 type 0 Rm. The T forms
        // (P 0, W 1) are not built.
        if (!pre && w) {
            return false;
        }
        size = (insn >> 22 & 1u) != 0 ? 1 : 4;
        if ((insn >> 25 & 1u) != 0) {
            const u32 amount = insn >> 7 & 0x1Fu, type = insn >> 5 & 3u, rm = run.ctx.r[m];
            if (m == 15) {
                return false;
            }
            switch (type) {
            case 0:
                offset = rm << amount;
                break;
            case 1:
                offset = amount == 0 ? 0 : rm >> amount;
                break;
            case 2:
                offset = static_cast<u32>(static_cast<s32>(rm) >> (amount == 0 ? 31 : amount));
                break;
            default:
                offset = amount == 0 ? (run.ctx.cpsr >> 29 & 1u) << 31 | rm >> 1
                                     : std::rotr(rm, static_cast<int>(amount));
                break;
            }
        } else {
            offset = insn & 0xFFFu;
        }
        // A byte to or from the PC is unpredictable.
        if (t == 15 && size == 1) {
            return false;
        }
    } else if ((insn & 0x0E000090u) == 0x00000090u && (insn & 0x60u) != 0 &&
               ((insn >> 22 & 1u) != 0 || (insn & 0xF00u) == 0)) {
        // The extra loads and stores: cond 000P U1WL Rn Rt imm4H 1 op2 1 imm4L, or cond 000P U0WL Rn Rt
        // 0000 1 op2 1 Rm.
        const u32 op2 = insn >> 5 & 3u;
        const bool l = (insn >> 20 & 1u) != 0;
        if (!pre && w) {
            return false;
        }
        if (write) {
            if (!l && op2 == 1) {
                size = 2; // STRH
            } else if (!l && op2 == 3) {
                size = 8, pair = true; // STRD
            } else {
                return false;
            }
        } else if (l && op2 == 1) {
            size = 2; // LDRH
        } else if (l && op2 == 2) {
            size = 1, sign = true; // LDRSB
        } else if (l && op2 == 3) {
            size = 2, sign = true; // LDRSH
        } else if (!l && op2 == 2) {
            size = 8, pair = true; // LDRD
        } else {
            return false; // the stores
        }
        if ((insn >> 22 & 1u) != 0) {
            offset = (insn >> 4 & 0xF0u) | (insn & 0xFu);
        } else if (m == 15) {
            return false;
        } else {
            offset = run.ctx.r[m];
        }
        if (t == 15) {
            return false;
        }
    } else {
        return false;
    }
    const u32 base = n == 15 ? pc + 8 : run.ctx.r[n];
    const u32 indexed = up ? base + offset : base - offset;
    const u32 address = pre ? indexed : base;
    const bool writeback = !pre || w;
    if ((pair && ((t & 1u) != 0 || t == 14)) ||
        (writeback && (n == 15 || n == t || (pair && n == t + 1)))) {
        return false;
    }
    // The access first, the base after.
    if (!one(address, size, t, sign, pair)) {
        return false;
    }
    if (writeback) {
        run.ctx.r[n] = indexed;
    }
    if (t != 15 || write) {
        run.ctx.r[15] = next;
    }
    return true;
}

u32 ARM_VitaNative::AccessReach(u32 pc) const {
    constexpr u32 Single = 8, Block = 128;
    if ((run.ctx.cpsr & 0x20u) != 0) {
        // The ARM11's Thumb is 16-bit: LDMIA/STMIA 1100, PUSH 1011010, POP 1011110.
        const u16 insn = memory.Read16(pc);
        return (insn & 0xF000u) == 0xC000u || (insn & 0xF600u) == 0xB400u ? Block : 4;
    }
    const u32 insn = memory.Read32(pc);
    if (insn >> 28 == 0xFu) {
        return Block; // the unconditional space: SRS, RFE
    }
    switch (insn >> 25 & 7u) {
    case 0b100u:
        return Block; // LDM/STM
    case 0b110u:
        // LDC/STC: VLDR/VSTR (P set, W clear) one value, the others a block (VLDM/VSTM).
        return (insn >> 24 & 1u) != 0 && (insn >> 21 & 1u) == 0 ? Single : Block;
    default:
        return Single;
    }
}

std::vector<std::string> ModuleTablesAt(u32 va) {
    AzaharVaReport t{};
    azaharVaReport(va, &t);
    std::string cores;
    for (u32 n = 0; n < 4; n++) {
        cores += fmt::format("; core {}: attached {}, view {:08X}/{:08X}, {} overrides", n,
                             t.attached[n], t.view_l1[n], t.view_l2[n], t.view_overrides[n]);
    }
    std::string overrides;
    for (u32 i = 0; i < t.view_overrides[t.core & 3] && i < AZAHAR_ATTACH_OVERRIDES; i++) {
        const AzaharRange& r = t.overrides[i];
        overrides +=
            fmt::format(" {:08X}+{:X} <- {:08X} ({:X})", r.guest_va, r.size, r.user_va, r.perm);
    }
    return {fmt::format("tables at {:08X} from core {} (azaharMap's {}): shared {:08X}/{:08X}{}; "
                        "failed view builds {} (the last {:08X} on core {})",
                        va, t.core, t.map_core, t.shared_l1, t.shared_l2, cores, t.view_failures,
                        static_cast<u32>(t.view_last_error), t.view_last_core),
            fmt::format("core {}'s view overrides:{}", t.core,
                        overrides.empty() ? std::string{" none"} : overrides)};
}

void ARM_VitaNative::ReportFault() {
    std::string registers;
    for (int i = 0; i < 16; i++) {
        registers += fmt::format("r{:02d} = {:08X}\n", i, run.ctx.r[i]);
    }
    const std::string message =
        fmt::format("{}Guest {} at PC {:08X} (cpsr {:08X}, fpscr {:08X}, fpexc {:08X}), fsr {:08X} "
                    "far {:08X}; core {}, host {}",
                    registers, ReasonName(run.reason), run.ctx.r[15], run.ctx.cpsr, run.ctx.fpscr,
                    fpexc, run.fsr, run.far, GetID(), Core::ThisHost());
    LOG_CRITICAL(Core_ARM11, "{}", message);
    // The thunk area's code as this host fetches it, not guest memory.
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
    for (const std::string& line : ModuleTablesAt(run.ctx.r[15])) {
        LOG_CRITICAL(Core_ARM11, "{}", line);
    }
    if (run.far != run.ctx.r[15]) {
        for (const std::string& line : ModuleTablesAt(run.far)) {
            LOG_CRITICAL(Core_ARM11, "{}", line);
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
    // Some titles invalidate their data's icache every frame: a sync is only owed where the process's
    // view of the range can execute.
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
    // Whether an exception return clears the local monitor is IMPLEMENTATION DEFINED on ARMv7-A:
    // entry.S runs CLREX before every guest entry. AccessPastHolds' own monitor goes too.
    exclusive = {};
    exclusive_held = {};
}

void ARM_VitaNative::SetPageTable(const std::shared_ptr<Memory::PageTable>& page_table) {
    current_page_table = page_table;
    // The kernel updates the current table only for the core it considers running, so the table is
    // given here for every core.
    if (map && page_table && map->MirroredTable() != page_table.get()) {
        memory.SyncMappingObserver(*page_table);
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
    if (upcall_regs_live) {
        return upcall_regs[index];
    }
    return index == 15 ? GetPC() : run.ctx.r[index];
}

void ARM_VitaNative::SetReg(int index, u32 value) {
    ASSERT(index >= 0 && index < 16);
    if (upcall_regs_live) {
        upcall_regs[index] = value;
    } else if (index == 15) {
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
    // Another thread's registers: AccessPastHolds' monitor was the last thread's.
    exclusive = {};
    exclusive_held = {};
    ToAzahar(ctx, run.ctx);
    if ((run.ctx.cpsr & 0x20u) != 0) {
        run.ctx.r[15] = PastPatchedThumbSvc(run.ctx.r[15]);
    }
    fpexc = ctx.fpexc;
}

void ARM_VitaNative::PrepareReschedule() {
    // From the HLE while the guest is stopped at an exit, or from another thread while it runs
    // (bounded by the quantum). An upcall takes an exit for it.
    stop_requested = true;
    // The upcalls are host 0's.
    if (Core::ThisHost() == 0) {
        upcall_reschedule_asked.store(true, std::memory_order_relaxed);
    }
}

} // namespace Core
