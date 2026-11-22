// azahar-native — native.c: the emulator half. Takes one PS Vita core and runs guest code on it natively,
// at the guest's own addresses, under translation tables and exception vectors of ours.
//
// Included from main.c, whose helpers it uses (emit, translate, the cp15 accessors,
// resolve_export, the mask getter and setter, gic_locate). Every exported call is here.
//
//   azaharTakeCore   occupy core 2 with a resident kernel thread, then clear core 2 from the
//                 scheduler's mask
//   azaharMap        the app owns the guest memory as ordinary user memblocks; the plugin resolves
//                 their physical pages, builds an N=0 translation table — the kernel's own two
//                 tables merged, with the guest windows as coarse L2s over the app's page lists
//                 and per-page W^X — plus a vector page, and installs both on core 2
//   azaharRun        enter the guest in user mode with a register file, leave it on the first
//                 exception (svc, undefined, prefetch or data abort), return the register file
//                 and the reason (entry.S). The svcs the module can answer from what it
//                 has (AZAHAR_FAST_*: the guest clock, the cache svcs, the current IDs) are
//                 answered in the vector, and the guest runs on
//   azaharRelease    put Sony's registers back on core 2, end the resident thread, and hand the
//                 core back to the scheduler. The thread is stopped and waited for before the
//                 mask is restored: the other order leaves the core unschedulable and freezes
//                 the app at exit. A release that cannot end the thread leaves the core
//                 detached and refuses to be taken again.
//
// Everything the resident thread does after the detach avoids kernel calls: the scheduler is no
// longer managing it, and a blocking call there is a way to never come back. It talks to the
// syscall side through volatiles, and every wait on it is bounded.
//
// Not here yet: a preemption timer (a guest that never takes an exception holds the core until
// reboot), the HLE bridge, and the guest scheduler. One guest thread, run to its next exception.

#include <psp2kern/kernel/proc_event.h>
#include <psp2kern/kernel/sysmem/data_transfers.h>
#include "azahar_native.h"

// ---------------------------------------------------------------------------- configuration

#define AZAHAR_TARGET_CORE 2
#define AZAHAR_LOG_PATH OUT_DIR "/native.txt"

#define AZAHAR_L1_ENTRIES 4096      // TTBCR.N = 0: the whole 4 GB through our table
#define AZAHAR_L2_COUNT 256         // one 1 KB L2 per guest megabyte: 256 MB of windows, 256 KB
#define AZAHAR_DOMAIN 8u            // a Client domain under this console's DACR (15450000)
#define AZAHAR_WAIT_STEP_US 50000
#define AZAHAR_WAIT_STEPS 40        // 2 s
#define AZAHAR_RUN_WAIT_STEPS 200   // 10 s: no preemption yet, so a guest may take a while to svc
#define AZAHAR_PMR_MASK 0x10u       // passes the private timer, masks everything else

#ifndef GICC_PMR
#define GICC_PMR 0x104u
#endif
// Cortex-A9 MPCore private timer, per core, at PERIPHBASE + 0x600; its interrupt is PPI 29.
#ifndef PTIMER_LOAD
#define PTIMER_LOAD 0x600u
#endif
#ifndef PTIMER_COUNTER
#define PTIMER_COUNTER 0x604u
#endif
#ifndef PTIMER_CONTROL
#define PTIMER_CONTROL 0x608u
#endif
#ifndef PTIMER_STATUS
#define PTIMER_STATUS 0x60Cu
#endif
#define PTIMER_CTRL_ENABLE 1u
#define PTIMER_CTRL_RELOAD 2u
#define PTIMER_CTRL_IRQ 4u
#define PTIMER_PPI 29u
#ifndef GICD_ISENABLER0
#define GICD_ISENABLER0 0x1100u   // banked: this core's IDs 0-31
#endif
#ifndef GICD_ICENABLER0
#define GICD_ICENABLER0 0x1180u
#endif
#ifndef GICD_ISPENDR0
#define GICD_ISPENDR0 0x1200u     // banked: this core's pending IDs 0-31
#endif
#define AZAHAR_TIMER_MEASURE_US 300000u

// 1: the table holds only what core 2 needs — guest windows, the vector page, this module's
//    megabytes, the resident thread's stack, the GIC alias — and nothing of the kernel's.
// 0: the kernel's two tables merged. With interrupts masked from install nothing of Sony's
//    executes on core 2, so the merge has no purpose; without the seal a guest that runs off its
//    window executes kernel-mapped memory.
#ifndef AZAHAR_SEAL
#define AZAHAR_SEAL 1
#endif

// ---------------------------------------------------------------------------- log
//
// emit() writes to out_fd. The calls here keep a file of their own open from the first call
// to the release — the emulator
// makes thousands of these calls a second, and an open per call would cost more than the
// guest work between them — and point out_fd at it for the duration of each call. Lines are
// still synced as written; azaharRun writes none on the success path for the same reason.
static SceUID azahar_fd = -1;

// Logging is opt-in: ux0:data/azahar/log present means native.txt and the kernel printf
// mirror as before; absent (the default) means no file, no printf, and emit() returns at
// once. Checked once per take.
// static int azahar_log_checked, azahar_log_wanted;
static int azahar_log_enabled(void) {
    // if (!azahar_log_checked) {
    //     azahar_log_checked = 1;
    //     const SceUID fd = ksceIoOpen(OUT_DIR "/log", SCE_O_RDONLY, 0);
    //     azahar_log_wanted = fd >= 0;
    //     if (fd >= 0) {
    //         ksceIoClose(fd);
    //     }
    // }
    // return azahar_log_wanted;
    return 0;
}

static void log_open(void) {
    if (!azahar_log_enabled()) {
        // Muted from the first native call on, and never unmuted by one: emit's buffer must not
        // see a second writer. The probe stages in main.c run at module_start, before any native
        // call.
        azahar_mute = 1;
        return;
    }
    azahar_mute = 0;
    if (azahar_fd < 0) {
        ksceIoMkdir(OUT_DIR, 0777);
        azahar_fd = ksceIoOpen(AZAHAR_LOG_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
    }
    out_fd = azahar_fd;
    buf_len = 0;
    log_sync = 1;
}

static void log_close(void) {
    if (!azahar_log_enabled()) {
        return;
    }
    if (azahar_mute) {
        // The mute lasts exactly as long as the native call: the probe stages in main.c share
        // emit() and log for their own reasons.
        azahar_mute = 0;
        return;
    }
    flush_buf();
    out_fd = -1;
    log_sync = 0;
}

static void log_release(void) {
    log_close();
    if (azahar_fd >= 0) {
        ksceIoClose(azahar_fd);
        azahar_fd = -1;
    }
}

// ---------------------------------------------------------------------------- cp15 extras

CP15_READ(tpidruro, c13, 0, c0, 3)

static inline void wr_tpidruro(uint32_t v) {
    asm volatile("mcr p15, 0, %0, c13, c0, 3 \n isb" ::"r"(v) : "memory");
}

static inline uint32_t rd_cpsr(void) {
    uint32_t v;
    asm volatile("mrs %0, cpsr" : "=r"(v));
    return v;
}

// ---------------------------------------------------------------------------- descriptors
//
// Short-descriptor format with SCTLR.AFE = 1: AP[0] is the Access Flag and must be set, the
// permission is AP[2:1]. Memory attributes: TEX=001 C=1
// B=1 (write-back, write-allocate), S=1 so the other cores see what the guest writes, unless a
// range asks for the app's own (AZAHAR_PERM_APP_ATTR). Domain 8 throughout — D0 is No-access under
// the console's DACR and hangs the console.

#define AZAHAR_DOMAIN_FIELD (AZAHAR_DOMAIN << 5)

// Section, PL1 RW only, executable: for the vector page. AP[2:1] = 00, AF = 1 -> bits 11:10 = 01.
#define AZAHAR_SEC_PRIV_RWX(pa) (((pa) & 0xFFF00000u) | (1u << 16) | (1u << 12) | (1u << 10) | \
                              AZAHAR_DOMAIN_FIELD | (1u << 3) | (1u << 2) | 2u)

// Coarse page table descriptor: L2 base (1 KB aligned), domain, type 01.
#define AZAHAR_COARSE(l2_pa) (((l2_pa) & 0xFFFFFC00u) | AZAHAR_DOMAIN_FIELD | 1u)

// A small page's memory attribute bits: S, TEX[2:0], C, B.
#define AZAHAR_ATTR_MASK ((1u << 10) | (7u << 6) | (1u << 3) | (1u << 2))
// The default: TEX=001 C=1 B=1 S=1.
#define AZAHAR_ATTR_WB ((1u << 10) | (1u << 6) | (1u << 3) | (1u << 2))

// A short-descriptor PAR's attributes as a small page's S, TEX, C and B. SCTLR.TRE is 0 on this
// console, so TEX[2] = 1 selects Normal memory with the outer policy in TEX[1:0] and the inner in
// C, B (00 non-cacheable, 01 WBWA, 10 WT, 11 WB). PAR has Inner in bits 6:4 (000 NC, 101 WBWA,
// 110 WT, 111 WB), Outer in 3:2 (the same order as TEX[1:0]) and SH in bit 7. Strongly ordered and
// device memory give 0: not for a guest window.
static uint32_t azahar_attr_from_par(uint32_t par) {
    uint32_t cb;
    switch ((par >> 4) & 7u) {
        case 0: cb = 0; break;
        case 5: cb = 1; break;
        case 6: cb = 2; break;
        case 7: cb = 3; break;
        default: return 0;
    }
    return (((par >> 7) & 1u) << 10) | ((4u | ((par >> 2) & 3u)) << 6) | ((cb >> 1) << 3) |
           ((cb & 1u) << 2);
}

// Small page for a guest window: reachable from PL0 (AP[1] = 1), AF set, XN and APX per perm,
// memory attributes attr (AZAHAR_ATTR_MASK bits).
static uint32_t azahar_small_page(uint32_t pa, uint32_t perm, uint32_t attr) {
    uint32_t d = (pa & 0xFFFFF000u) | (attr & AZAHAR_ATTR_MASK) | (1u << 4) /* AF */ |
                 (1u << 5) /* PL0 */ | 2u;
    if (!(perm & AZAHAR_PERM_W)) {
        d |= (1u << 9); // APX: read-only
    }
    if (!(perm & AZAHAR_PERM_X)) {
        d |= 1u; // XN
    }
    return d;
}

// ---------------------------------------------------------------------------- kernel exports

static int resolve_exports(void) {
    sysinfo_ready();
    return 1;
}

// ---------------------------------------------------------------------------- GIC
//
// gic_locate() (main.c) finds PERIPHBASE's alias and sets gic_va / gic_has_dist. It normally runs
// as part of the probe; the native calls run it themselves if it has not.
static void azahar_gic(void) {
    if (!gic_va) {
        gic_locate();
    }
    if (gic_va && !gic_has_dist) {
        emit("  gic: CPU interface reachable but not the distributor; PMR masking still works\n");
    }
    emit("  gic va %08x%s\n", gic_va,
         gic_va ? "" : " (not found: the guest will run with interrupts unmasked at the GIC)");
}

// ---------------------------------------------------------------------------- state

enum { ST_NONE, ST_TAKEN, ST_MAPPED, ST_RELEASED };
static int azahar_state;

static SceUID azahar_l1_uid = -1, azahar_l2_uid = -1, azahar_vec_uid = -1;
static uint32_t *azahar_l1, *azahar_l2, *azahar_vec;
static uintptr_t azahar_l1_pa, azahar_l2_pa, azahar_vec_pa;
static uint32_t azahar_vec_va;      // where the vector page appears in our table
static unsigned azahar_l2_used;
static uint32_t azahar_l2_slot[AZAHAR_L2_COUNT]; // which L1 index each L2 serves

// ---------------------------------------------------------------------------- windows

#include "windows.h"

// The guest's windows onto the app's memory, as map_range made them and edits cut them.
#define AZAHAR_WINDOWS 256u
static AzaharWindow azahar_window_list[AZAHAR_WINDOWS];
static AzaharWindows azahar_windows = {azahar_window_list, 0, AZAHAR_WINDOWS};

static SceUID azahar_thread = -1;
static SceUID azahar_client_pid = -1;      // the process azaharTakeCore ran for
static SceUID azahar_proc_handler = -1;    // registered once, on the first take
static uint32_t azahar_saved_mask;         // the active CPU mask before the detach
static uint32_t azahar_sony[8]; // Sony's handler per vector slot, decoded at map time

// Resident thread <-> syscall side. Everything volatile, no kernel calls on the core-2 side.
enum { CMD_NONE, CMD_INSTALL, CMD_RUN, CMD_UNINSTALL };
static volatile uint32_t azahar_cmd, azahar_cmd_done, azahar_cmd_status;
static volatile uint32_t azahar_heartbeat, azahar_on_core, azahar_stop, azahar_park;

static volatile uint32_t azahar_core2_vbar, azahar_core2_cpsr, azahar_core2_ttbr1, azahar_core2_sp;


static inline uint32_t rd_tpidrurw(void) {
    uint32_t v;
    asm volatile("mrc p15, 0, %0, c13, c0, 2" : "=r"(v));
    return v;
}
static inline void wr_tpidrurw(uint32_t v) {
    asm volatile("mcr p15, 0, %0, c13, c0, 2 \n isb" ::"r"(v) : "memory");
}
static inline uint32_t rd_fpexc_here(void) {
    uint32_t v;
    asm volatile(".fpu neon \n vmrs %0, fpexc" : "=r"(v));
    return v;
}
static inline void wr_fpexc(uint32_t v) {
    asm volatile(".fpu neon \n vmsr fpexc, %0 \n isb" ::"r"(v) : "memory");
}
static inline uint32_t rd_fpscr_here(void) {
    uint32_t v;
    asm volatile(".fpu neon \n vmrs %0, fpscr" : "=r"(v));
    return v;
}
static inline void wr_fpscr(uint32_t v) {
    asm volatile(".fpu neon \n vmsr fpscr, %0" ::"r"(v));
}
static void vfp_bank_save(uint64_t *p) {
    asm volatile(".fpu neon \n vstmia %0!, {d0-d15} \n vstmia %0, {d16-d31}"
                 : "+r"(p)
                 :
                 : "memory");
}
static void vfp_bank_load(const uint64_t *p) {
    asm volatile(".fpu neon \n vldmia %0!, {d0-d15} \n vldmia %0, {d16-d31}" : "+r"(p));
}
// The svcs answered in the guest's own world (azahar_native.h, AZAHAR_FAST_*): entry.S's
// azahar_svc_fast reads a core's and counts in it; azaharRun fills it for each slice. The
// offsets are entry.S's FAST_* and are checked below.
typedef struct AzaharFast {
    uint32_t on;          // AZAHAR_FAST_* bits allowed this slice
    uint32_t timer;       // the private timer's counter register, 0 when the slice has no timer
    uint32_t quantum;     // the counter's start
    uint32_t mult_int;    // guest ticks per private-timer tick, whole part
    uint32_t mult_frac;   // and fraction, in 1/2^32
    uint32_t base_lo, base_hi;        // the guest clock at entry
    uint32_t thread_id, process_id;   // the running guest thread's and its process's IDs
    uint32_t count[AZAHAR_FAST_KINDS]; // answered this slice
    uint32_t save[4];     // the guest's r4-r7 while the vector works
} AzaharFast;
_Static_assert(__builtin_offsetof(AzaharFast, mult_int) == 0x0c, "entry.S FAST_MULT_INT");
_Static_assert(__builtin_offsetof(AzaharFast, base_lo) == 0x14, "entry.S FAST_BASE_LO");
_Static_assert(__builtin_offsetof(AzaharFast, process_id) == 0x20, "entry.S FAST_PROCESS_ID");
_Static_assert(__builtin_offsetof(AzaharFast, count) == 0x24, "entry.S FAST_COUNT");
_Static_assert(__builtin_offsetof(AzaharFast, save) == 0x38, "entry.S FAST_SAVE");

// One block per Vita core (MPIDR & 3): a slice on a core reads and writes its own only. This is
// entry.S's part, its CPU_* offsets checked below, with the context first so that a stub's SP
// after it saved the user file is the block. VBAR is banked per core, and each core's vector
// table (azahar_vec + AZAHAR_VEC_STRIDE * core) names that core's copy of the stubs.
typedef struct AzaharCpu {
    AzaharContext ctx;
    uint32_t host[4];             // host cpsr, host sp, Sony's SP_svc, ctx
    volatile uint32_t guest_active;
    // Where the slice got to: 1-3 C before entry; 10-11 entry asm; 31-35 the user file was
    // saved; 40-41 back in host mode; 42-50 back in C.
    volatile uint32_t progress;
    // A stub's word equal to its own address means that exception was taken.
    volatile uint32_t prog_und, prog_svc, prog_pabt, prog_dabt, prog_irq;
    volatile uint32_t stray_retry;
    uint32_t exit_info[7];        // reason, raw lr, spsr, dfsr, dfar, ifsr, ifar
    volatile uint32_t irq_last, irq_stray;
    // {saved SP for the mode, Sony's handler} per vector, filled at install
    uint32_t chain_und[2], chain_svc[2], chain_pabt[2], chain_dabt[2], chain_irq[2];
    volatile uint32_t stray[8];   // count, reason, lr, spsr, dfsr, dfar, ifsr, ifar
    uint32_t pad0;
    AzaharFast fast;
    uint8_t stray_stack[64];
    uint8_t irq_stack[64];
    uint32_t guard[2];            // both stacks' tops stay inside the block
    uint8_t vfp_stack[1024];      // azahar_vfp_svc's, from azahar_svc_fast
    uint32_t guard2[2];
} AzaharCpu;
#define AZAHAR_CORES 4u
extern AzaharCpu azahar_cpu[AZAHAR_CORES]; // entry.S's .bss
_Static_assert(__builtin_offsetof(AzaharCpu, host) == 0x150, "entry.S CPU_HOST");
_Static_assert(__builtin_offsetof(AzaharCpu, guest_active) == 0x160, "entry.S CPU_ACTIVE");
_Static_assert(__builtin_offsetof(AzaharCpu, progress) == 0x164, "entry.S CPU_PROGRESS");
_Static_assert(__builtin_offsetof(AzaharCpu, prog_und) == 0x168, "entry.S CPU_PROG_UND");
_Static_assert(__builtin_offsetof(AzaharCpu, prog_irq) == 0x178, "entry.S CPU_PROG_IRQ");
_Static_assert(__builtin_offsetof(AzaharCpu, stray_retry) == 0x17c, "entry.S CPU_STRAY_RETRY");
_Static_assert(__builtin_offsetof(AzaharCpu, exit_info) == 0x180, "entry.S CPU_EXIT");
_Static_assert(__builtin_offsetof(AzaharCpu, irq_last) == 0x19c, "entry.S CPU_IRQ_LAST");
_Static_assert(__builtin_offsetof(AzaharCpu, irq_stray) == 0x1a0, "entry.S CPU_IRQ_STRAY");
_Static_assert(__builtin_offsetof(AzaharCpu, chain_und) == 0x1a4, "entry.S CPU_CHAIN_UND");
_Static_assert(__builtin_offsetof(AzaharCpu, chain_svc) == 0x1ac, "entry.S CPU_CHAIN_SVC");
_Static_assert(__builtin_offsetof(AzaharCpu, chain_pabt) == 0x1b4, "entry.S CPU_CHAIN_PABT");
_Static_assert(__builtin_offsetof(AzaharCpu, chain_dabt) == 0x1bc, "entry.S CPU_CHAIN_DABT");
_Static_assert(__builtin_offsetof(AzaharCpu, chain_irq) == 0x1c4, "entry.S CPU_CHAIN_IRQ");
_Static_assert(__builtin_offsetof(AzaharCpu, stray) == 0x1cc, "entry.S CPU_STRAY");
_Static_assert(__builtin_offsetof(AzaharCpu, fast) == 0x1f0, "entry.S CPU_FAST");
_Static_assert(__builtin_offsetof(AzaharCpu, stray_stack) + 64 == 0x278, "entry.S CPU_STRAY_STACK_TOP");
_Static_assert(__builtin_offsetof(AzaharCpu, irq_stack) + 64 == 0x2b8, "entry.S CPU_IRQ_STACK_TOP");
_Static_assert(__builtin_offsetof(AzaharCpu, vfp_stack) + 1024 == 0x6c0, "entry.S CPU_VFP_STACK_TOP");
_Static_assert(sizeof(AzaharCpu) == 0x6c8, "entry.S CPU_SIZE");

// The rest of a core's state, C's alone: Sony's registers saved at install and put back at
// uninstall, the private timer's clock, and the slice's figures.
typedef struct AzaharCore {
    volatile uint32_t sv_ttbcr, sv_ttbr0, sv_ctxid, sv_vbar, sv_sp_und, sv_sp_abt, sv_sp_svc;
    volatile uint32_t sv_sp_irq, sv_tpidruro, sv_tpidrurw, sv_cpacr, sv_fpexc, sv_pmr;
    volatile uint32_t sv_isenabler_timer;
    uint32_t sv_fpscr;
    uint64_t sv_vfp_bank[32];
    volatile uint32_t ttbcr_seen;
    uint32_t timer_hz;            // this core's private timer, measured on it
    volatile uint32_t quantum_ticks, timer_left;
    volatile uint32_t run_reason, run_svc, run_fsr, run_far, run_spsr, run_rawlr;
    volatile uint32_t run_pmr_before, run_pmr_after;
    // The guest clock rate and timer clock fast.mult_int/mult_frac were made for.
    uint64_t fast_tick_hz;
    uint32_t fast_timer_hz;
    // azaharMap's core: azaharRun runs a slice here only then.
    uint32_t attached;
    // The azahar_code_gen this core's instruction cache last caught up with (core2_install).
    uint32_t code_gen;
    AzaharRunRequest run_req;
} AzaharCore;
static AzaharCore azahar_core[AZAHAR_CORES];

// The core this runs on. Not ksceKernelCpuId: nothing of Sony's kernel is mapped once a core is
// sealed (a call to it there takes a prefetch abort).
static inline unsigned this_core(void) {
    uint32_t mpidr;
    asm volatile("mrc p15, 0, %0, c0, c0, 5" : "=r"(mpidr));
    return mpidr & 3u;
}

// Private-timer ticks as guest ticks, as azahar_svc_fast computes them: monotonic in `spent`.
static uint64_t fast_guest_ticks(const AzaharFast *fast, uint32_t spent) {
    return (uint64_t)spent * fast->mult_int + (((uint64_t)spent * fast->mult_frac) >> 32);
}

extern int azahar_enter(AzaharCpu *cpu);
extern uint32_t azahar_get_banked_sp(uint32_t mode);
extern void azahar_set_banked_sp(uint32_t mode, uint32_t sp);
extern volatile uint32_t azahar_gic_va, azahar_seal_flag;
// Each core's stubs, in the order its vector table names them: und, svc, pabt, dabt, irq.
#define AZAHAR_STUBS(n)                                                                        \
    extern void azahar_vec_und_##n(void), azahar_vec_svc_##n(void), azahar_vec_pabt_##n(void), \
        azahar_vec_dabt_##n(void), azahar_vec_irq_##n(void)
AZAHAR_STUBS(0);
AZAHAR_STUBS(1);
AZAHAR_STUBS(2);
AZAHAR_STUBS(3);
#define AZAHAR_STUB_ROW(n)                                                                     \
    {azahar_vec_und_##n, azahar_vec_svc_##n, azahar_vec_pabt_##n, azahar_vec_dabt_##n,         \
     azahar_vec_irq_##n}
static void (*const azahar_stubs[AZAHAR_CORES][5])(void) = {
    AZAHAR_STUB_ROW(0), AZAHAR_STUB_ROW(1), AZAHAR_STUB_ROW(2), AZAHAR_STUB_ROW(3)};
// One vector table per core in the vector page: Sony's 16 words and our five handler words.
#define AZAHAR_VEC_STRIDE 0x80u
// taiHEN's module utility, imported but not declared by any header vitasdk installs.
extern int module_get_offset(SceUID pid, SceUID modid, int segidx, size_t offset,
                             uintptr_t *addr);

#define MODE_IRQ 0x12u
static volatile uint32_t azahar_timer_sample;   // idle loop: the free-running counter
static volatile uint32_t azahar_tlb_dirty;      // an edit happened since the guest last ran
static uint32_t azahar_run_count;
// Core-2 side: our table/vectors/PMR are in (set at the end of core2_install, cleared first
// thing in core2_uninstall), so the resident loop knows when the SGI poll applies.
static volatile uint32_t azahar_installed;
// SGI service windows (core2_sgi_window): how many opened, the last pending mask that opened
// one, and how many timed out with a bit still pending. Read on the syscall core at release.
static volatile uint32_t azahar_sgi_windows, azahar_sgi_last_pend, azahar_sgi_stuck;
// 1: every azaharRun logs its entry and exit (the trigger app's test); 0: only faults, preemptions
// and errors (the emulator). The test app asks for it through azaharMap's count sign — see azaharMap.
static int azahar_verbose;
// Folded (the default): no resident thread. azaharRun switches the calling core into the guest
// world for one slice and back, so the emulation thread and the guest share a core and the
// other three are the kernel's. ux0:data/azahar/resident selects the old design, a kernel
// thread holding core 2 and the emulation thread posting commands to it from another core.
static int azahar_folded = 1;
// The instruction cache is invalidated when the guest's code may have changed under us, not
// on every install: folded mode installs once per slice, and an ICIALLU each time would throw
// away the emulation thread's own cached code. Every edit that
// can change what an address executes bumps the generation (tables_changed); each core's
// install, and core2_run's azahar_tlb_dirty path, catch up with it on that core.
static volatile uint32_t azahar_code_gen = 1;
// Serialises the table and L2 pool edits (azaharEdit, azaharHold). Created by the first folded
// azaharTakeCore.
static SceUID azahar_edit_mutex = -1;

static void detach_all(void) {
    for (unsigned n = 0; n < AZAHAR_CORES; n++) {
        azahar_core[n].attached = 0;
    }
}

// After descriptors changed (`code`: and what an address executes may have): the attached core
// drops its stale translations at its next install (core2_switch's TLBIALL) and, for a new code
// generation, its instruction cache.
static void tables_changed(int code) {
    asm volatile("dsb ish" ::: "memory");
    azahar_tlb_dirty = 1;
    if (code) {
        azahar_code_gen++;
    }
}

static int azahar_resident_requested(void) {
    const SceUID fd = ksceIoOpen(OUT_DIR "/resident", SCE_O_RDONLY, 0);
    if (fd < 0) {
        return 0;
    }
    ksceIoClose(fd);
    return 1;
}

#define MODE_SVC 0x13u

#define MODE_UND 0x1Bu
#define MODE_ABT 0x17u

// ---------------------------------------------------------------------------- core 2 side

// The table switch, both directions. ARM ARM B3.10.4 example B3-3 with erratum 754322's dsb on
// each side; TTBCR and TTBR0 change inside one break-before-make block because between them the
// CPU would read a 1024-entry table as though it had 4096 (vaprobe's STAGE_OWN, 2026-08-26).
static void core2_switch(uint32_t ttbcr, uint32_t ttbr0, uint32_t ctxid, int icache) {
    asm volatile("dsb                             \n"
                 "mcr p15, 0, %[zero], c13, c0, 1 \n"
                 "isb                             \n"
                 "mcr p15, 0, %[tcr],  c2, c0, 2  \n"
                 "mcr p15, 0, %[ttb],  c2, c0, 0  \n"
                 "isb                             \n"
                 "mcr p15, 0, %[ctx],  c13, c0, 1 \n"
                 "dsb                             \n"
                 "mcr p15, 0, %[zero], c8, c7, 0  \n" // TLBIALL
                 "mcr p15, 0, %[zero], c7, c5, 6  \n" // BPIALL
                 "dsb                             \n"
                 "isb                             \n"
                 :
                 : [tcr] "r"(ttbcr), [ttb] "r"(ttbr0), [ctx] "r"(ctxid), [zero] "r"(0u)
                 : "memory");
    if (icache) {
        // Guest code was written as data: the instruction cache is physically tagged, so this
        // is only needed on the way in, and Sony's code on the way out has not changed.
        asm volatile("mcr p15, 0, %0, c7, c5, 0 \n dsb \n isb" ::"r"(0u) : "memory");
    }
}

static void core2_install(unsigned n) {
    AzaharCpu *const cpu = &azahar_cpu[n];
    AzaharCore *const core = &azahar_core[n];
    // Own the core from here on: nothing of Sony's is delivered to it while our table is in.
    // Interrupts left open between install and the first run take a prefetch abort: Sony's IRQ
    // path runs under a copy of its L1 that has gone stale.
    volatile uint32_t *const pmr = gic_va ? (volatile uint32_t *)(gic_va + GICC_PMR) : NULL;
    if (pmr) {
        core->sv_pmr = *pmr & 0xFFu;
        *pmr = AZAHAR_PMR_MASK;
        asm volatile("dsb \n isb" ::: "memory");
    }

    core->sv_ttbcr = rd_ttbcr();
    core->sv_ttbr0 = rd_ttbr0();
    core->sv_ctxid = rd_contextidr();
    core->sv_vbar = rd_vbar();
    core->sv_tpidruro = rd_tpidruro();
    core->sv_tpidrurw = rd_tpidrurw();
    // The VFP, for the guest and for entry.S's own vldm/vstm: cp10/cp11 open at both privilege
    // levels, FPEXC.EN set. Sony's threads are lazily given the VFP on first use; this core is
    // no longer Sony's, so it is simply on.
    core->sv_cpacr = rd_cpacr();
    wr_cpacr(core->sv_cpacr | (0xFu << 20));
    core->sv_fpexc = rd_fpexc_here();
    wr_fpexc(core->sv_fpexc | (1u << 30));
    // The whole register bank and FPSCR belong to whichever Sony thread lazily owned the VFP
    // on this core, and the guest is about to clobber both. Hand back at uninstall exactly
    // what was here: a guest FPSCR with exception bits left live panics Sony's first VFP
    // operation after release ("VFP/NEON exception occurred in kernel thread").
    core->sv_fpscr = rd_fpscr_here();
    vfp_bank_save(core->sv_vfp_bank);
    core->sv_sp_und = azahar_get_banked_sp(MODE_UND);
    core->sv_sp_abt = azahar_get_banked_sp(MODE_ABT);
    core->sv_sp_svc = azahar_get_banked_sp(MODE_SVC);
    core->sv_sp_irq = azahar_get_banked_sp(MODE_IRQ);
    cpu->chain_irq[0] = core->sv_sp_irq;
    azahar_gic_va = gic_va;
    azahar_seal_flag = AZAHAR_SEAL;

    // The private timer: enable its PPI at this core's banked GIC enable register (measured
    // disabled on every core), and start it free-running with no interrupt so its clock can be
    // measured against the system clock from the idle loop's samples.
    if (pmr) {
        volatile uint32_t *const isen = (volatile uint32_t *)(gic_va + GICD_ISENABLER0);
        core->sv_isenabler_timer = (*isen >> PTIMER_PPI) & 1u;
        *(volatile uint32_t *)(gic_va + PTIMER_CONTROL) = 0;
        *(volatile uint32_t *)(gic_va + PTIMER_STATUS) = 1;
        *(volatile uint32_t *)(gic_va + PTIMER_LOAD) = 0xFFFFFFFFu;
        *(volatile uint32_t *)(gic_va + PTIMER_CONTROL) = PTIMER_CTRL_ENABLE | PTIMER_CTRL_RELOAD;
        *isen = 1u << PTIMER_PPI;
        asm volatile("dsb \n isb" ::: "memory");
    }

    // The stray-exception chains: this mode's SP and Sony's handler, decoded from Sony's own
    // vector instruction (azahar_sony_target, at map time).
    cpu->chain_und[0] = core->sv_sp_und;
    cpu->chain_und[1] = azahar_sony[1];
    cpu->chain_svc[0] = core->sv_sp_svc;
    cpu->chain_svc[1] = azahar_sony[2];
    cpu->chain_pabt[0] = core->sv_sp_abt;
    cpu->chain_pabt[1] = azahar_sony[3];
    cpu->chain_dabt[0] = core->sv_sp_abt;
    cpu->chain_dabt[1] = azahar_sony[4];
    cpu->guest_active = 0;
    cpu->stray_retry = 0;
    for (unsigned i = 0; i < 8; i++) {
        cpu->stray[i] = 0;
    }
    asm volatile("dsb" ::: "memory");

    // N = 0: the table base is bits [31:14].
    const uint32_t ours = ((uint32_t)azahar_l1_pa & 0xFFFFC000u) | (core->sv_ttbr0 & 0x3FFFu);
    const uint32_t gen = azahar_code_gen;
    core2_switch(core->sv_ttbcr & ~7u, ours, core->sv_ctxid, core->code_gen != gen);
    core->code_gen = gen;
    core->ttbcr_seen = rd_ttbcr();

    // Our vectors, this core's table, executable only because our table says so.
    asm volatile("mcr p15, 0, %0, c12, c0, 0 \n isb" ::"r"(azahar_vec_va + n * AZAHAR_VEC_STRIDE)
                 : "memory");
    azahar_installed = 1;
    azahar_cmd_status = 0;
}

static void core2_uninstall(unsigned n) {
    AzaharCore *const core = &azahar_core[n];
    azahar_installed = 0;
    asm volatile("mcr p15, 0, %0, c12, c0, 0 \n isb" ::"r"(core->sv_vbar) : "memory");
    core2_switch(core->sv_ttbcr, core->sv_ttbr0, core->sv_ctxid, 0);
    azahar_set_banked_sp(MODE_UND, core->sv_sp_und);
    azahar_set_banked_sp(MODE_ABT, core->sv_sp_abt);
    // Never the mode we are running in: azahar_set_banked_sp writes the live stack pointer, and
    // install and uninstall do not stand at the same depth, so this would move the stack under
    // the caller and it would return into nothing. Both designs install from SVC, so SVC is
    // the entry that has to be skipped; UND, ABT and IRQ are always foreign here.
    if ((rd_cpsr() & 0x1Fu) != MODE_SVC) {
        azahar_set_banked_sp(MODE_SVC, core->sv_sp_svc);
    }
    azahar_set_banked_sp(MODE_IRQ, core->sv_sp_irq);
    wr_tpidruro(core->sv_tpidruro);
    wr_tpidrurw(core->sv_tpidrurw);
    // Bank and FPSCR back before FPEXC/CPACR close the door (and before the PMR reopens
    // interrupts: Sony's IRQ path must never see the guest's VFP state).
    vfp_bank_load(core->sv_vfp_bank);
    wr_fpscr(core->sv_fpscr);
    wr_fpexc(core->sv_fpexc);
    wr_cpacr(core->sv_cpacr);
    volatile uint32_t *const pmr = gic_va ? (volatile uint32_t *)(gic_va + GICC_PMR) : NULL;
    if (pmr) {
        // Timer off and its PPI back to disabled before the mask lifts.
        *(volatile uint32_t *)(gic_va + PTIMER_CONTROL) = 0;
        *(volatile uint32_t *)(gic_va + PTIMER_STATUS) = 1;
        if (!core->sv_isenabler_timer) {
            *(volatile uint32_t *)(gic_va + GICD_ICENABLER0) = 1u << PTIMER_PPI;
        }
        asm volatile("dsb \n isb" ::: "memory");
        *pmr = core->sv_pmr;
        asm volatile("dsb \n isb" ::: "memory");
    }
    azahar_cmd_status = 0;
}

// The kernel's cross-core calls — Threadmgr on SGI 2/7, Processmgr on SGI 6 — broadcast to every
// core, and the sender spins until each core's handler has run. With ICCPMR at AZAHAR_PMR_MASK a
// taken core never takes them, so the sender spins forever holding whatever lock it holds, and
// the whole console freezes. A system process spawned mid-title does it: module loading does an
// all-core icache rendezvous. Steady-state gameplay never triggers it (no foreign SGI at all).
//
// The answer is to service the SGI: put Sony's world back for a moment (core2_uninstall
// restores the PMR last, so the pending SGI vectors straight into Sony's handler), then take
// the core again. The active-mask bit stays clear, so the scheduler dispatches nothing here and
// the resident thread resumes when the handler returns. A window opens only when an SGI pends;
// the latency is bounded by one guest slice.
static void core2_sgi_window(unsigned n) {
    core2_uninstall(n);
    // The interrupt delivers within a few cycles of the PMR write above. Stay open while
    // pending bits keep appearing — a process start loads many modules back to back — bounded
    // in case a bit sticks (a pending-but-disabled SGI would otherwise pin the loop).
    uint32_t spins = 0;
    while (spins < 100000u) {
        const uint32_t pend = *(volatile uint32_t *)(gic_va + GICD_ISPENDR0) &
                              *(volatile uint32_t *)(gic_va + GICD_ISENABLER0) & 0x7FFFu;
        if (!pend) {
            break;
        }
        spins++;
    }
    if (spins >= 100000u) {
        azahar_sgi_stuck++;
    }
    core2_install(n);
    azahar_sgi_windows++;
}

// One trip into the guest. Interrupts are masked at the GIC for its duration — the core is
// owned, not shared, while guest code runs — and put back afterwards.
static void core2_run(unsigned n) {
    AzaharCpu *const cpu = &azahar_cpu[n];
    AzaharCore *const core = &azahar_core[n];
    volatile uint32_t *const pmr = gic_va ? (volatile uint32_t *)(gic_va + GICC_PMR) : NULL;
    cpu->progress = 1;
    cpu->prog_und = cpu->prog_svc = cpu->prog_pabt = cpu->prog_dabt = cpu->prog_irq = 0;
    if (pmr) {
        core->run_pmr_before = *pmr & 0xFFu; // masked since install
    }

    if (azahar_tlb_dirty) {
        // Descriptors changed under us since the last run, so a translation may be stale and
        // the TLB always goes. What else is stale depends on the edit: mapping a guest code
        // range needs all three, since the app wrote those instructions through its own view
        // and this core may have executed different code at the address before. A permission
        // change moves no memory and rewrites no instruction, so the
        // instruction cache and the branch predictor are still correct for it.
        asm volatile("dsb                       \n"
                     "mcr p15, 0, %0, c8, c7, 0 \n" // TLBIALL
                     :
                     : "r"(0u)
                     : "memory");
        const uint32_t gen = azahar_code_gen;
        if (core->code_gen != gen) {
            asm volatile("mcr p15, 0, %0, c7, c5, 6 \n"  // BPIALL
                         "mcr p15, 0, %0, c7, c5, 0 \n"  // ICIALLU
                         :
                         : "r"(0u)
                         : "memory");
            core->code_gen = gen;
        }
        asm volatile("dsb \n isb" ::: "memory");
        azahar_tlb_dirty = 0;
    }
    cpu->progress = 2;
    wr_tpidruro(cpu->ctx.tpidruro);
    wr_tpidrurw(cpu->ctx.tpidrurw);
    // IRQs off at the CPU from the timer's start until the guest runs: azahar_enter returns
    // from exception into it with the guest's CPSR, which lets a timer that has already
    // expired in as an ordinary preemption. An IRQ taken here, with CPU_ACTIVE set, would save
    // this host code's registers as the guest's. Restored once the timer is stopped below.
    uint32_t irq_was;
    asm volatile("mrs %0, cpsr \n cpsid i" : "=r"(irq_was)::"memory");
    if (pmr && core->quantum_ticks) {
        *(volatile uint32_t *)(gic_va + PTIMER_CONTROL) = 0;
        *(volatile uint32_t *)(gic_va + PTIMER_STATUS) = 1;
        *(volatile uint32_t *)(gic_va + PTIMER_LOAD) = core->quantum_ticks;
        *(volatile uint32_t *)(gic_va + PTIMER_CONTROL) =
            PTIMER_CTRL_ENABLE | PTIMER_CTRL_IRQ; // one shot: no reload
        asm volatile("dsb \n isb" ::: "memory");
    }
    cpu->progress = 3;
    const uint32_t reason = (uint32_t)azahar_enter(cpu);
    cpu->progress = 42;
    if (pmr) {
        core->timer_left = *(volatile uint32_t *)(gic_va + PTIMER_COUNTER);
        *(volatile uint32_t *)(gic_va + PTIMER_CONTROL) = 0;
        *(volatile uint32_t *)(gic_va + PTIMER_STATUS) = 1;
        asm volatile("dsb \n isb" ::: "memory");
    }
    if (!(irq_was & 0x80u)) {
        // A timer IRQ still pending from the slice goes to the stray path, which ends it.
        asm volatile("cpsie i" ::: "memory");
    }
    cpu->ctx.tpidruro = rd_tpidruro();
    cpu->ctx.tpidrurw = rd_tpidrurw();

    if (pmr) {
        core->run_pmr_after = *pmr & 0xFFu;
    }

    // entry.S left the raw exception LR in ctx.r[15] and the SPSR in ctx.cpsr. Turn the LR into
    // the address the reason means (azahar_native.h), and pull the svc immediate out of the
    // instruction — readable here because our table is still installed and the code window is
    // readable at PL1.
    const uint32_t thumb = cpu->ctx.cpsr & (1u << 5);
    const uint32_t raw_lr = cpu->ctx.r[15];
    uint32_t pc = raw_lr, svc = 0;
    switch (reason) {
        case AZAHAR_EXIT_SVC:
            pc = raw_lr; // the instruction after the svc
            if (thumb) {
                svc = *(const volatile uint16_t *)(uintptr_t)(raw_lr - 2) & 0xFFu;
            } else {
                svc = *(const volatile uint32_t *)(uintptr_t)(raw_lr - 4) & 0x00FFFFFFu;
            }
            break;
        case AZAHAR_EXIT_UND: pc = raw_lr - (thumb ? 2u : 4u); break;
        case AZAHAR_EXIT_PABT: pc = raw_lr - 4u; break;
        case AZAHAR_EXIT_DABT: pc = raw_lr - 8u; break;
        case AZAHAR_EXIT_TIMER: pc = raw_lr - 4u; break; // IRQ: LR is the next instruction + 4
        default: break;
    }
    cpu->ctx.r[15] = pc;
    core->run_reason = reason;
    core->run_svc = svc;
    core->run_rawlr = raw_lr;
    core->run_spsr = cpu->exit_info[2];
    core->run_fsr = (reason == AZAHAR_EXIT_DABT) ? cpu->exit_info[3]
                  : (reason == AZAHAR_EXIT_PABT) ? cpu->exit_info[5] : 0u;
    core->run_far = (reason == AZAHAR_EXIT_DABT) ? cpu->exit_info[4]
                  : (reason == AZAHAR_EXIT_PABT) ? cpu->exit_info[6] : 0u;
    cpu->progress = 50;
    azahar_cmd_status = 0;
}

// Folded mode: one slice on the calling core. The same install, run and uninstall the
// resident thread does once at map, once per command and once at release, done here per
// slice around azahar_enter. The PMR mask holds from install to uninstall, so nothing of Sony's
// runs on this core in between; the moment the mask lifts, anything that pended (the tick,
// a cross-core call) is delivered into Sony's own handler, which replaces the SGI window.
// The syscall's kernel stack is the host stack azahar_enter saves and the vectors come back to.
static void fold_run(unsigned n) {
    core2_install(n);
    core2_run(n);
    core2_uninstall(n);
}

// Folded mode's timer calibration: the private timer of this core, free-running against the
// system clock for AZAHAR_TIMER_MEASURE_US. The thread is pinned, so the counter read after the
// delay is the same core's.
static void fold_measure_timer(unsigned n) {
    AzaharCore *const core = &azahar_core[n];
    core->timer_hz = 0;
    if (!gic_va) {
        return;
    }
    volatile uint32_t *const isen = (volatile uint32_t *)(gic_va + GICD_ISENABLER0);
    const uint32_t was_enabled = (*isen >> PTIMER_PPI) & 1u;
    *(volatile uint32_t *)(gic_va + PTIMER_CONTROL) = 0;
    *(volatile uint32_t *)(gic_va + PTIMER_STATUS) = 1;
    *(volatile uint32_t *)(gic_va + PTIMER_LOAD) = 0xFFFFFFFFu;
    *(volatile uint32_t *)(gic_va + PTIMER_CONTROL) = PTIMER_CTRL_ENABLE | PTIMER_CTRL_RELOAD;
    asm volatile("dsb \n isb" ::: "memory");
    const uint32_t s0 = *(volatile uint32_t *)(gic_va + PTIMER_COUNTER);
    const uint32_t t0 = ksceKernelGetSystemTimeLow();
    ksceKernelDelayThread(AZAHAR_TIMER_MEASURE_US);
    const uint32_t s1 = *(volatile uint32_t *)(gic_va + PTIMER_COUNTER);
    const uint32_t t1 = ksceKernelGetSystemTimeLow();
    *(volatile uint32_t *)(gic_va + PTIMER_CONTROL) = 0;
    *(volatile uint32_t *)(gic_va + PTIMER_STATUS) = 1;
    if (!was_enabled) {
        *(volatile uint32_t *)(gic_va + GICD_ICENABLER0) = 1u << PTIMER_PPI;
    }
    asm volatile("dsb \n isb" ::: "memory");
    const uint32_t ticks = s0 - s1, us = t1 - t0; // counts down
    if (us) {
        core->timer_hz = (uint32_t)(((uint64_t)ticks * 1000000ull) / us);
    }
    emit("  private timer (folded, cpu%u): %u ticks in %u us -> %u Hz\n", n, ticks, us,
         core->timer_hz);
}

static int core2_entry(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    azahar_on_core = (uint32_t)ksceKernelCpuId() & 3;
    azahar_core2_vbar = rd_vbar();
    azahar_core2_cpsr = rd_cpsr();
    azahar_core2_ttbr1 = rd_ttbr1();
    {
        uint32_t sp;
        asm volatile("mov %0, sp" : "=r"(sp));
        azahar_core2_sp = sp;
    }

    while (!azahar_stop) {
        azahar_heartbeat++;
        if (azahar_park) {
            // WFE, not WFI: a taken core sees no interrupt for seconds at a time, and the
            // next azaharTakeCore has to be able to wake this. SEV from any core does.
            asm volatile("wfe" ::: "memory");
            continue;
        }
        if (azahar_gic_va) {
            azahar_timer_sample = *(volatile uint32_t *)(azahar_gic_va + PTIMER_COUNTER);
        }
        // An SGI pending against this core while our world is installed is a cross-core call
        // the kernel is spinning on (core2_sgi_window). Checked between commands only — during
        // a slice the guest owns the core and the pend bit simply waits for the next exit.
        if (azahar_installed && gic_va) {
            const uint32_t pend = *(volatile uint32_t *)(gic_va + GICD_ISPENDR0) &
                                  *(volatile uint32_t *)(gic_va + GICD_ISENABLER0) & 0x7FFFu;
            if (pend) {
                azahar_sgi_last_pend = pend;
                core2_sgi_window(AZAHAR_TARGET_CORE);
            }
        }
        const uint32_t cmd = azahar_cmd;
        if (cmd == CMD_NONE || azahar_cmd_done) {
            continue;
        }
        switch (cmd) {
            case CMD_INSTALL: core2_install(AZAHAR_TARGET_CORE); break;
            case CMD_RUN: core2_run(AZAHAR_TARGET_CORE); break;
            case CMD_UNINSTALL: core2_uninstall(AZAHAR_TARGET_CORE); break;
            default: azahar_cmd_status = 1; break;
        }
        // Publish the results before the done flag: without the barrier the other core can
        // observe done set while the status (and the exit context) are still the old values,
        // which read as a failed command once in a while.
        asm volatile("dsb" ::: "memory");
        azahar_cmd_done = 1;
    }
    return 0;
}

// ---------------------------------------------------------------------------- syscall side

static void report_stray(void) {
    static const char *const names[] = {"?", "svc", "undefined", "prefetch abort", "data abort"};
    for (unsigned n = 0; n < AZAHAR_CORES; n++) {
        const AzaharCpu *const cpu = &azahar_cpu[n];
        if (!cpu->stray[0]) {
            continue;
        }
        emit("  STRAY exception(s) on cpu%u with no guest active: %u, first %s lr %08x spsr %08x "
             "dfsr %08x dfar %08x ifsr %08x ifar %08x (%u retried store(s))\n",
             n, cpu->stray[0], cpu->stray[1] < 5 ? names[cpu->stray[1]] : "?", cpu->stray[2],
             cpu->stray[3], cpu->stray[4], cpu->stray[5], cpu->stray[6], cpu->stray[7],
             cpu->stray_retry);
    }
}

static int core2_command(uint32_t cmd, unsigned steps) {
    azahar_cmd_done = 0;
    azahar_cmd_status = 0xFFFFFFFFu;
    asm volatile("dsb" ::: "memory");
    azahar_cmd = cmd;
    // A guest exit is microseconds away most of the time — an svc round trip measured ~1.5 us
    // — and the emulator waits here on every one of them, so the wait spins first and only
    // sleeps once it is clear the guest is running for real. Sleeping in 1 ms steps after that
    // keeps a preemption quantum's worth of latency bounded.
    int ret = AZAHAR_ERR_TIMEOUT;
    for (uint32_t spin = 0; spin < 400000u && !azahar_cmd_done; spin++) {
        asm volatile("" ::: "memory");
    }
    for (unsigned i = 0; i < steps * (AZAHAR_WAIT_STEP_US / 1000u); i++) {
        if (azahar_cmd_done) {
            break;
        }
        ksceKernelDelayThread(1000);
    }
    if (azahar_cmd_done) {
        // Pair with the worker's dsb: nothing written before its done flag may be read ahead
        // of the flag itself.
        asm volatile("dmb" ::: "memory");
        azahar_cmd = CMD_NONE;
        ret = (int)azahar_cmd_status == 0 ? 0 : AZAHAR_ERR_CORE;
    }
    if (ret == AZAHAR_ERR_TIMEOUT) {
        emit("  command %u did not complete (heartbeat %u)\n", cmd, azahar_heartbeat);
    }
    report_stray();
    return ret;
}

static int alloc_block(const char *name, SceSize size, SceSize align, SceUID *uid, uint32_t **va,
                       uintptr_t *pa) {
    SceKernelAllocMemBlockKernelOpt opt;
    __builtin_memset(&opt, 0, sizeof(opt));
    opt.size = sizeof(opt);
    opt.attr = 0x4;
    opt.alignment = align;
    *uid = ksceKernelAllocMemBlock(name, SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_ROOT_PHYCONT_RW, size, &opt);
    if (*uid < 0) {
        emit("  %s: alloc failed 0x%08x\n", name, (uint32_t)*uid);
        return AZAHAR_ERR_ALLOC;
    }
    if (ksceKernelGetMemBlockBase(*uid, (void **)va) < 0 || ksceKernelVAtoPA(*va, pa) < 0) {
        emit("  %s: base/pa lookup failed\n", name);
        return AZAHAR_ERR_ALLOC;
    }
    return 0;
}

static void free_blocks(void) {
    if (azahar_vec_uid >= 0) { ksceKernelFreeMemBlock(azahar_vec_uid); azahar_vec_uid = -1; }
    if (azahar_l2_uid >= 0) { ksceKernelFreeMemBlock(azahar_l2_uid); azahar_l2_uid = -1; }
    if (azahar_l1_uid >= 0) { ksceKernelFreeMemBlock(azahar_l1_uid); azahar_l1_uid = -1; }
}

// Find the kernel virtual address at which physical page `pa` is currently mapped, by asking
// the MMU about each page of the low 8 MB — where the kernel keeps its translation tables, at
// a different virtual address every boot.
static const uint32_t *find_kernel_page(uint32_t pa) {
    for (uint32_t va = 0; va < (8u << 20); va += 0x1000) {
        const uint32_t par = translate(va, AT_PRIV_R);
        if (!PAR_FAULTED(par) && PAR_PA(par) == pa) {
            return (const uint32_t *)(uintptr_t)va;
        }
    }
    return NULL;
}

// One page of the app's memory: its physical address, and its contents pushed to memory so core
// 2 can fetch it as instructions through a different mapping.
//
// The app's pages live in a domain the kernel holds at No-access while a syscall runs (that is
// why kernel code copies user memory through ksceKernelCopyFromUser), so from here ATS1CUR
// answers with a domain fault — PAR 00000017 — and a by-VA cache clean
// would fault the same way, inside the syscall. ksceKernelVAtoPA is tried first. Failing that,
// every No-access domain is granted Client for one ATS and one page clean, with interrupts
// suspended: DACR is process context, and a reschedule mid-window would put the old value back
// under the clean. Grant-only, never revoke — the direction measured safe.
static uint32_t dacr_grant_all(uint32_t d) {
    uint32_t g = d;
    for (unsigned k = 0; k < 16; k++) {
        if (((d >> (2 * k)) & 3u) == 0) {
            g |= 1u << (2 * k);
        }
    }
    return g;
}

// want_par: translate with ATS1CUR even when VAtoPA would do, for the page's attributes in PAR.
static int azahar_user_page(uint32_t uva, uint32_t *pa, int *how, uint32_t *par_out,
                            int want_par) {
    uintptr_t p = 0;
    uint32_t par = 0;
    if (!want_par && ksceKernelVAtoPA((const void *)(uintptr_t)uva, &p) >= 0 && p) {
        *pa = (uint32_t)p & 0xFFFFF000u;
        // Sony's VAtoPA may be an ATS as well: the 64 KiB base the same way (par_large_page).
        uintptr_t base = 0;
        if ((uva & 0xF000u) &&
            ksceKernelVAtoPA((const void *)(uintptr_t)(uva & ~0xFFFFu), &base) >= 0 &&
            ((uint32_t)base & 0xFFFFF000u) == *pa) {
            *pa |= uva & 0xF000u;
        }
        *how = 0;
    } else {
        SceKernelIntrStatus st = ksceKernelCpuSuspendIntr();
        const uint32_t d = rd_dacr();
        wr_dacr(dacr_grant_all(d));
        asm volatile("mcr p15, 0, %0, c7, c8, 2 \n isb" ::"r"(uva) : "memory"); // ATS1CUR
        par = rd_par();
        // Its 64 KiB base (par_large_page).
        asm volatile("mcr p15, 0, %0, c7, c8, 2 \n isb" ::"r"(uva & ~0xFFFFu) : "memory");
        par = par_large_page(uva, par, rd_par());
        wr_dacr(d);
        ksceKernelCpuResumeIntr(st);
        *par_out = par;
        if (PAR_FAULTED(par)) {
            return AZAHAR_ERR_PAGE;
        }
        *pa = PAR_PA(par);
        *how = 1;
    }
    // Clean the page to the point of unification under the same grant.
    SceKernelIntrStatus st = ksceKernelCpuSuspendIntr();
    const uint32_t d = rd_dacr();
    wr_dacr(dacr_grant_all(d));
    ksceKernelDcacheCleanRange((void *)(uintptr_t)uva, 0x1000);
    wr_dacr(d);
    ksceKernelCpuResumeIntr(st);
    return 0;
}

// End the resident thread and, if this session took the core out of the mask, put the mask
// back. Only a detached session has a second half, and only once its client is gone: see
// azaharRelease.
static int azahar_stop_resident(void) {
    azahar_park = 0;
    azahar_stop = 1;
    asm volatile("dsb \n sev" ::: "memory");
    SceUInt timeout = 1000000;
    const int ended = ksceKernelWaitThreadEnd(azahar_thread, NULL, &timeout);
    ksceKernelDeleteThread(azahar_thread);
    azahar_thread = -1;
    if (ended < 0) {
        emit("  resident thread did not end (0x%08x); core %u stays detached\n",
             (uint32_t)ended, AZAHAR_TARGET_CORE);
        return AZAHAR_ERR_CORE;
    }
    return 0;
}

static void sgxd_on_proc_gone(SceUID pid);   // sgxd.c
static void reback_on_proc_gone(SceUID pid); // reback.c
static uint32_t reback_attr_for(uint32_t pa); // reback.c

// The client process is gone: same teardown as azaharRelease, which the two paths share below.
static int azahar_on_proc_gone(SceUID pid) {
    sgxd_on_proc_gone(pid);
    reback_on_proc_gone(pid);
    if (pid != azahar_client_pid || azahar_state == ST_NONE) {
        return 0;
    }
    log_open();
    emit("== client process gone (state %d)\n", azahar_state);
    if (azahar_folded) {
        free_blocks();
        detach_all();
        azahar_state = ST_NONE;
        azahar_client_pid = -1;
        log_release();
        return 0;
    }
    if (azahar_state == ST_MAPPED) {
        const int r = core2_command(CMD_UNINSTALL, AZAHAR_WAIT_STEPS);
        emit("  uninstall -> %d\n", r);
    }
    const int ended = azahar_stop_resident();
    free_blocks();
    azahar_state = ended == 0 ? ST_NONE : ST_RELEASED;
    azahar_client_pid = -1;
    log_release();
    return 0;
}

static int azahar_proc_exit(SceUID pid, SceProcEventInvokeParam1 *p, int unk) {
    (void)p; (void)unk;
    return azahar_on_proc_gone(pid);
}

static int azahar_proc_kill(SceUID pid, SceProcEventInvokeParam1 *p, int unk) {
    (void)p; (void)unk;
    return azahar_on_proc_gone(pid);
}

static const SceProcEventHandler azahar_proc_events = {
    .size = sizeof(SceProcEventHandler),
    .create = NULL,
    .exit = azahar_proc_exit,
    .kill = azahar_proc_kill,
    .stop = NULL,
    .start = NULL,
    .switch_process = NULL,
};

int azaharTakeCore(void) {
    uint32_t state;
    ENTER_SYSCALL(state);
    log_open();
    emit("== azaharTakeCore\n");
    int ret = 0;

    if (azahar_state != ST_NONE && azahar_state != ST_RELEASED) {
        ret = AZAHAR_ERR_STATE;
        goto out;
    }
    if (!resolve_exports()) {
        ret = AZAHAR_ERR_RESOLVE;
        goto out;
    }
    azahar_gic();
    azahar_folded = !azahar_resident_requested();
    if (azahar_folded) {
        if (azahar_state == ST_RELEASED) {
            // A parked resident thread from a session that ran in the other mode.
            ret = AZAHAR_ERR_STATE;
            goto out;
        }
        if ((ret = alloc_block("azahar_l1", AZAHAR_L1_ENTRIES * 4, 0x4000, &azahar_l1_uid, &azahar_l1, &azahar_l1_pa)) ||
            (ret = alloc_block("azahar_l2", AZAHAR_L2_COUNT * 0x400, 0x1000, &azahar_l2_uid, &azahar_l2, &azahar_l2_pa)) ||
            (ret = alloc_block("azahar_vec", 0x100000, 0x100000, &azahar_vec_uid, &azahar_vec, &azahar_vec_pa))) {
            free_blocks();
            goto out;
        }
        if (azahar_edit_mutex < 0) {
            azahar_edit_mutex = ksceKernelCreateMutex("SceAzaharEdit", 0, 0, NULL);
            if (azahar_edit_mutex < 0) {
                free_blocks();
                ret = AZAHAR_ERR_ALLOC;
                goto out;
            }
        }
        azahar_installed = 0;
        detach_all();
        azahar_client_pid = ksceKernelGetProcessId();
        if (azahar_proc_handler < 0) {
            azahar_proc_handler = ksceKernelRegisterProcEventHandler("azaharnative", &azahar_proc_events, 0);
            emit("  process-event handler: 0x%08x\n", (uint32_t)azahar_proc_handler);
        }
        emit("  folded: no resident thread, the caller's core runs the guest\n");
        azahar_state = ST_TAKEN;
        ret = AZAHAR_TAKE_FOLDED;
        goto out;
    }

    if (azahar_state == ST_RELEASED) {
        // A previous title released the core without handing it back (azaharRelease): the
        // scheduler still excludes it, and the resident thread is idling on it. Prove the
        // thread is alive and take it over as it stands.
        if (azahar_thread < 0) {
            ret = AZAHAR_ERR_STATE;
            goto out;
        }
        const uint32_t beat = azahar_heartbeat;
        azahar_park = 0;
        asm volatile("dsb \n sev" ::: "memory");
        unsigned w;
        for (w = 0; w < 20 && azahar_heartbeat == beat; w++) {
            ksceKernelDelayThread(10000);
        }
        if (azahar_heartbeat == beat) {
            emit("  resident thread is not running on the released core\n");
            ret = AZAHAR_ERR_CORE;
            goto out;
        }
        if ((ret = alloc_block("azahar_l1", AZAHAR_L1_ENTRIES * 4, 0x4000, &azahar_l1_uid, &azahar_l1, &azahar_l1_pa)) ||
            (ret = alloc_block("azahar_l2", AZAHAR_L2_COUNT * 0x400, 0x1000, &azahar_l2_uid, &azahar_l2, &azahar_l2_pa)) ||
            (ret = alloc_block("azahar_vec", 0x100000, 0x100000, &azahar_vec_uid, &azahar_vec, &azahar_vec_pa))) {
            free_blocks();
            goto out;
        }
        azahar_cmd = azahar_cmd_done = 0;
        emit("  reusing the detached core: heartbeat %u -> %u\n", beat, azahar_heartbeat);
        azahar_state = ST_TAKEN;
        goto out;
    }

    if ((ret = alloc_block("azahar_l1", AZAHAR_L1_ENTRIES * 4, 0x4000, &azahar_l1_uid, &azahar_l1, &azahar_l1_pa)) ||
        (ret = alloc_block("azahar_l2", AZAHAR_L2_COUNT * 0x400, 0x1000, &azahar_l2_uid, &azahar_l2, &azahar_l2_pa)) ||
        (ret = alloc_block("azahar_vec", 0x100000, 0x100000, &azahar_vec_uid, &azahar_vec, &azahar_vec_pa))) {
        free_blocks();
        goto out;
    }

    // Occupy first, detach second: a thread pinned to an already-detached core never runs.
    azahar_heartbeat = azahar_stop = azahar_park = azahar_cmd = azahar_cmd_done = 0;
    azahar_thread = ksceKernelCreateThread("azaharnative_core2", core2_entry, 0x10000100, 0x4000, 0,
                                        CPU_AFFINITY(AZAHAR_TARGET_CORE), NULL);
    if (azahar_thread < 0 || ksceKernelStartThread(azahar_thread, 0, NULL) < 0) {
        emit("  core-2 thread create/start failed 0x%08x\n", (uint32_t)azahar_thread);
        free_blocks();
        ret = AZAHAR_ERR_CORE;
        goto out;
    }
    unsigned i;
    for (i = 0; i < AZAHAR_WAIT_STEPS && !azahar_heartbeat; i++) {
        ksceKernelDelayThread(AZAHAR_WAIT_STEP_US);
    }
    if (!azahar_heartbeat) {
        emit("  core-2 thread never ran\n");
        azahar_stop = 1;
        free_blocks();
        ret = AZAHAR_ERR_CORE;
        goto out;
    }
    emit("  resident thread on cpu%u, cpsr %08x (mode %02x), vbar %08x, ttbr1 %08x (syscall's %08x)\n",
         azahar_on_core, azahar_core2_cpsr, azahar_core2_cpsr & 0x1Fu, azahar_core2_vbar, azahar_core2_ttbr1,
         rd_ttbr1());

    uint32_t mask = active_cpu_mask();
    if (!mask) {
        mask = 0xFu;
        emit("  mask unreadable; assuming %08x\n", mask);
    }
    const uint32_t beat = azahar_heartbeat;
    // The same gate either way: 250 ms later the resident thread must still be ticking.
    ksceKernelDelayThread(250000);
    if (azahar_heartbeat == beat) {
        emit("  resident thread STOPPED; restoring the mask\n");
        azahar_stop = 1;
        free_blocks();
        ret = AZAHAR_ERR_CORE;
        goto out;
    }
    emit("  heartbeat %u -> %u: core %u is ours\n", beat, azahar_heartbeat, AZAHAR_TARGET_CORE);
    azahar_saved_mask = mask;
    azahar_client_pid = ksceKernelGetProcessId();
    if (azahar_proc_handler < 0) {
        // So the core comes back when the app dies, however it dies. Registered here rather
        // than at module_start: a fault here costs one launch, a fault there bootloops.
        azahar_proc_handler = ksceKernelRegisterProcEventHandler("azaharnative", &azahar_proc_events, 0);
        emit("  process-event handler: 0x%08x\n", (uint32_t)azahar_proc_handler);
    }
    azahar_state = ST_TAKEN;

out:
    emit("  -> %d\n", ret);
    log_close();
    EXIT_SYSCALL(state);
    return ret;
}

// Where Sony's vector `slot` (0..7) goes. Its vectors are `ldr pc, [pc, #imm]` for most slots
// and not for all — Data Abort's reads a pointer word of 0 taken as one — so the instruction is
// decoded: a PC-relative ldr, or a branch.
static uint32_t azahar_sony_target(uint32_t vbar, unsigned slot) {
    const uint32_t addr = vbar + slot * 4u;
    const uint32_t w = *(const volatile uint32_t *)(uintptr_t)addr;
    if ((w & 0x0F7F0000u) == 0x051F0000u) { // ldr pc, [pc, #+/-imm12]
        const uint32_t imm = w & 0xFFFu;
        const uint32_t at = (w & (1u << 23)) ? addr + 8u + imm : addr + 8u - imm;
        return *(const volatile uint32_t *)(uintptr_t)at;
    }
    if ((w & 0x0F000000u) == 0x0A000000u) { // b
        int32_t off = (int32_t)(w << 8) >> 6;
        return addr + 8u + (uint32_t)off;
    }
    return 0;
}

// The L2 serving guest megabyte `idx`: existing, or a fresh one from the pool with its L1
// entry written. Returns the table index or -1 when the pool is exhausted.
static int l2_for(uint32_t idx, int create) {
    for (unsigned t = 0; t < azahar_l2_used; t++) {
        if (azahar_l2_slot[t] == idx) {
            return (int)t;
        }
    }
    if (!create || azahar_l2_used == AZAHAR_L2_COUNT) {
        return -1;
    }
    const unsigned t = azahar_l2_used++;
    azahar_l2_slot[t] = idx;
    for (unsigned i = 0; i < 256; i++) {
        azahar_l2[t * 256u + i] = 0;
    }
    azahar_l1[idx] = AZAHAR_COARSE((uint32_t)azahar_l2_pa + t * 0x400u);
    return (int)t;
}

// The guest's window for the first `done` bytes of a range map_range wrote; the pages past
// them still show what they showed.
static int map_range_window(const AzaharRange *rg, uint32_t done) {
    return done ? windows_add(&azahar_windows, rg->guest_va, rg->user_va, done) : 0;
}

// One range of the app's memory into the guest's table: every page resolved and cleaned, its
// descriptor written, the megabyte's L2 created on first use. Used at map and for MAP edits.
static int map_range(const AzaharRange *rg, unsigned label) {
    unsigned pages = 0;
    uint32_t first_pa = 0, first_par = 0, first_attr = 0;
    int first_how = -1;
    for (uint32_t off = 0; off < rg->size; off += 0x1000) {
        const uint32_t gva = rg->guest_va + off;
        const uint32_t uva = rg->user_va + off;
        const int t = l2_for(gva >> 20, 1);
        if (t < 0) {
            emit("  out of L2 tables at guest %08x\n", gva);
            map_range_window(rg, off);
            return AZAHAR_ERR_TABLE;
        }
        uint32_t pa = 0, par = 0;
        int how = -1;
        const int app_attr = (rg->perm & AZAHAR_PERM_APP_ATTR) != 0;
        if (azahar_user_page(uva, &pa, &how, &par, app_attr) != 0) {
            emit("  range %u: user va %08x does not translate (par %08x, VAtoPA refused)\n",
                 label, uva, par);
            map_range_window(rg, off);
            return AZAHAR_ERR_PAGE;
        }
        // A page azaharReback moved onto CDRAM is mapped as the app now maps it, whatever the
        // range asked for: two views of one page with different cacheability are unpredictable.
        const uint32_t moved = reback_attr_for(pa);
        const uint32_t attr = moved ? moved : app_attr ? azahar_attr_from_par(par) : AZAHAR_ATTR_WB;
        if (!attr) {
            emit("  range %u: user va %08x is not Normal memory (par %08x)\n", label, uva, par);
            map_range_window(rg, off);
            return AZAHAR_ERR_PAGE;
        }
        if (off == 0) {
            first_pa = pa;
            first_how = how;
            first_par = par;
            first_attr = attr;
        }
        azahar_l2[(unsigned)t * 256u + ((gva >> 12) & 0xFFu)] =
            azahar_small_page(pa, rg->perm, attr);
        pages++;
    }
    if (map_range_window(rg, rg->size) != 0) {
        emit("  range %u: out of window records (%u)\n", label, AZAHAR_WINDOWS);
        return AZAHAR_ERR_ALLOC;
    }
    emit("  range %u: user %08x -> guest %08x, %u page(s) over %u MB, perm %c%c%c, first pa "
         "%08x (%s)\n",
         label, rg->user_va, rg->guest_va, pages,
         ((rg->guest_va + rg->size - 1) >> 20) - (rg->guest_va >> 20) + 1,
         (rg->perm & AZAHAR_PERM_R) ? 'r' : '-', (rg->perm & AZAHAR_PERM_W) ? 'w' : '-',
         (rg->perm & AZAHAR_PERM_X) ? 'x' : '-', first_pa,
         first_how == 0 ? "VAtoPA" : "ATS1CUR under a domain grant");
    if (rg->perm & AZAHAR_PERM_APP_ATTR) {
        // Always printed: these ranges are few (guest VRAM), and which attributes the kernel
        // gave the app's block is the point of the flag.
        ksceKernelPrintf("[azaharnative] guest %08x+%08x: the app's attributes, first page par "
                         "%08x -> S %u TEX %u C %u B %u\n",
                         rg->guest_va, rg->size, first_par, (first_attr >> 10) & 1u,
                         (first_attr >> 6) & 7u, (first_attr >> 3) & 1u, (first_attr >> 2) & 1u);
    }
    return 0;
}

static int check_range(const AzaharRange *rg, int need_user) {
    if ((need_user && (rg->user_va & 0xFFFu)) || (rg->guest_va & 0xFFFu) || (rg->size & 0xFFFu) ||
        !rg->size || rg->guest_va + rg->size > 0x40000000u ||
        rg->guest_va + rg->size < rg->guest_va) {
        return 0;
    }
    return 1;
}

// Build the N=0 table: the kernel's TTBR0 entries below 0x40000000 and its TTBR1 entries above,
// as this process sees them, then our vector page in a free 1 MB slot at the top of the low
// quarter, then the guest windows as coarse L2s.
static int build_table(const AzaharMapRequest *req) {
    // TTBR0 is one global table; TTBR1 is per process, and the one core 2 needs is the resident
    // kernel thread's, not this syscall's — the app's TTBR1, whose pages are not where the
    // kernel keeps its own, has nothing to merge.
    if (azahar_core2_vbar == 0 || azahar_core2_sp == 0) {
        // Nothing captured the core's context: in the resident design core2_entry does it,
        // in the folded one azaharMap does it for the caller. Without it this function would
        // read Sony's vectors from address zero, which kills the syscall.
        emit("  no core context: vbar %08x sp %08x ttbr1 %08x\n", azahar_core2_vbar, azahar_core2_sp,
             azahar_core2_ttbr1);
        return AZAHAR_ERR_STATE;
    }
    const uint32_t kernel_l1_pa = rd_ttbr0() & 0xFFFFF000u;
    const uint32_t ttbr1_pa = azahar_core2_ttbr1 & 0xFFFFC000u;
    const uint32_t *kernel_l1 = find_kernel_page(kernel_l1_pa);
    if (!kernel_l1) {
        emit("  kernel L1 pa %08x not found in the low 8 MB\n", kernel_l1_pa);
        return AZAHAR_ERR_TABLE;
    }
    // Only megabytes at or above 0x40000000 come from the upper table; the kernel's code, its
    // thread stacks and this module all live in the low gigabyte, which TTBR0 covers globally.
    // A page that cannot be located is therefore left out rather than fatal: the folded design
    // reads the app process's TTBR1, whose pages are not where the kernel keeps its own. The
    // preflight below refuses an address that needed one.
    const uint32_t *upper[4] = {NULL, NULL, NULL, NULL};
    for (unsigned pg = 1; pg < 4; pg++) {
        upper[pg] = find_kernel_page(ttbr1_pa + pg * 0x1000u);
        if (!upper[pg]) {
            emit("  TTBR1 page %u (pa %08x) not found; nothing above %08x can be kept\n", pg,
                 ttbr1_pa + pg * 0x1000u, pg << 30);
        }
    }
    // The kernel's entry for L1 index `idx`, from whichever of its two tables holds it.
#define KERNEL_L1(idx)                                                                             \
    ((idx) < 1024u ? kernel_l1[(idx)]                                                              \
                   : (upper[(idx) >> 10] != NULL ? upper[(idx) >> 10][(idx) & 1023u] : 0u))

    for (unsigned i = 0; i < AZAHAR_L1_ENTRIES; i++) {
        azahar_l1[i] = 0;
    }
#if AZAHAR_SEAL
    // What core 2 touches while our table is in, by the addresses of the things it touches:
    // this module's code and data, its stack, the GIC. Each address names one megabyte, and
    // that megabyte's entry — the kernel's own, pointing at the kernel's own L2 — is kept.
    // The module's own megabytes come from its segment table — code and data/bss both,
    struct KeepRange { uint32_t addr, len; };
    struct KeepRange keep[40];
    unsigned nkeep = 0;
    {
        // Segment bases from taiHEN (already imported; the module manager's own
        // ksceKernelGetModuleInfo lives in ForKernel, whose NIDs vary per firmware). taiHEN
        // gives no sizes, so each segment runs from its base to the farthest symbol this file
        // knows in it, plus slack; the base closes the front, which no symbol list can.
        tai_module_info_t tinfo;
        __builtin_memset(&tinfo, 0, sizeof(tinfo));
        tinfo.size = sizeof(tinfo);
        if (taiGetModuleInfoForKernel(0x10005 /* the kernel */, "azaharnative", &tinfo) >= 0) {
            const uint32_t text_syms[] = {
                (uint32_t)(uintptr_t)&azahar_enter,         (uint32_t)(uintptr_t)&azahar_vec_irq_3,
                (uint32_t)(uintptr_t)&azahar_get_banked_sp, (uint32_t)(uintptr_t)&core2_entry,
                (uint32_t)(uintptr_t)&core2_run,         (uint32_t)(uintptr_t)&core2_install,
                (uint32_t)(uintptr_t)&core2_uninstall,   (uint32_t)(uintptr_t)&translate,
            };
            const uint32_t data_syms[] = {
                (uint32_t)(uintptr_t)&azahar_cpu[0],    (uint32_t)(uintptr_t)azahar_cpu[3].guard,
                (uint32_t)(uintptr_t)&azahar_core[0],   (uint32_t)(uintptr_t)&azahar_core[3].run_req,
                (uint32_t)(uintptr_t)&azahar_cmd,       (uint32_t)(uintptr_t)&azahar_heartbeat,
                (uint32_t)(uintptr_t)&azahar_timer_sample, (uint32_t)(uintptr_t)&gic_va,
                (uint32_t)(uintptr_t)&azahar_l1_pa,     (uint32_t)(uintptr_t)azahar_sony,
                (uint32_t)(uintptr_t)&azahar_state,
            };
            uint32_t bases[2] = {0, 0};
            for (unsigned seg = 0; seg < 2; seg++) {
                uintptr_t base = 0;
                if (module_get_offset(0x10005, tinfo.modid, (int)seg, 0, &base) >= 0) {
                    bases[seg] = (uint32_t)base;
                } else {
                    emit("  seal: segment %u base lookup failed\n", seg);
                }
            }
            // A symbol belongs to whichever segment starts at or below it, nearest. The two
            // lists only say what this file *thinks* is text or data, and the compiler is
            // free to disagree (.rodata lives in the text segment, for one); bucketing by
            // list would stretch a segment's range across the gap to the other, into
            // megabytes the kernel never mapped, and the install would fail on them.
            const unsigned n_text = sizeof(text_syms) / sizeof(text_syms[0]);
            const unsigned n_data = sizeof(data_syms) / sizeof(data_syms[0]);
            int bucket[32];
            uint32_t ends[2] = {bases[0], bases[1]};
            for (unsigned i = 0; i < n_text + n_data; i++) {
                const uint32_t sym = i < n_text ? text_syms[i] : data_syms[i - n_text];
                int best = -1;
                for (unsigned seg = 0; seg < 2; seg++) {
                    if (bases[seg] != 0 && sym >= bases[seg] &&
                        (best < 0 || bases[seg] > bases[best])) {
                        best = (int)seg;
                    }
                }
                bucket[i] = best;
                if (best >= 0 && sym > ends[best]) {
                    ends[best] = sym;
                }
            }
            // A range longer than any plausible module segment means the bucketing went
            // wrong: when one segment's base lookup fails, its symbols fall into the other
            // segment's bucket and stretch that range across the gap between the two.
            // Such a range is discarded and its symbols are kept one megabyte at a time,
            // like the spot-list fallback.
            for (unsigned seg = 0; seg < 2; seg++) {
                if (bases[seg] == 0) {
                    continue;
                }
                const uint32_t len = (ends[seg] - bases[seg]) + 0x1000u;
                if (len > 0x01000000u) {
                    emit("  seal: segment %u range %08x..%08x implausible, keeping its "
                         "symbols one by one\n", seg, bases[seg], bases[seg] + len);
                    bases[seg] = 0; /* rejected: its symbols fall through to spots below */
                    continue;
                }
                keep[nkeep].addr = bases[seg];
                keep[nkeep].len = len;
                emit("  seal: segment %u %08x..%08x\n", seg, bases[seg], bases[seg] + len);
                nkeep++;
            }
            for (unsigned i = 0; i < n_text + n_data; i++) {
                if (bucket[i] >= 0 && bases[bucket[i]] != 0) {
                    continue; /* covered by a kept segment range */
                }
                if (nkeep < 36) {
                    keep[nkeep].addr = i < n_text ? text_syms[i] : data_syms[i - n_text];
                    keep[nkeep].len = 4;
                    nkeep++;
                }
            }
        }
    }
    if (nkeep == 0) {
        emit("  seal: module segment lookup failed; falling back to the spot list\n");
        const uint32_t spot[] = {
            (uint32_t)(uintptr_t)&azahar_enter,       (uint32_t)(uintptr_t)&azahar_vec_irq_3,
            (uint32_t)(uintptr_t)&azahar_get_banked_sp, (uint32_t)(uintptr_t)&core2_entry,
            (uint32_t)(uintptr_t)&core2_run,       (uint32_t)(uintptr_t)&core2_install,
            (uint32_t)(uintptr_t)&core2_uninstall, (uint32_t)(uintptr_t)&translate,
            (uint32_t)(uintptr_t)&azahar_cpu[0],      (uint32_t)(uintptr_t)azahar_cpu[3].guard,
            (uint32_t)(uintptr_t)&azahar_core[0],     (uint32_t)(uintptr_t)&azahar_cmd,
            (uint32_t)(uintptr_t)&azahar_heartbeat,   (uint32_t)(uintptr_t)&azahar_timer_sample,
            (uint32_t)(uintptr_t)&gic_va,
            (uint32_t)(uintptr_t)&azahar_l1_pa,       (uint32_t)(uintptr_t)&azahar_core[3].run_req,
            (uint32_t)(uintptr_t)azahar_sony,         (uint32_t)(uintptr_t)&azahar_state,
        };
        for (unsigned i = 0; i < sizeof(spot) / sizeof(spot[0]); i++) {
            keep[nkeep].addr = spot[i];
            keep[nkeep].len = 4;
            nkeep++;
        }
    }
    // Always: every core's block with its asm stacks (entry.S's .bss), the worker's kernel
    // stack, and the GIC window.
    keep[nkeep].addr = (uint32_t)(uintptr_t)azahar_cpu;
    keep[nkeep].len = sizeof(AzaharCpu) * AZAHAR_CORES;
    nkeep++;
    // The stack the slice runs on: the resident thread's own, or in folded mode the caller's
    // syscall stack, whose depth differs between azaharMap and azaharRun - hence the slack each way.
    keep[nkeep].addr = azahar_core2_sp - 0x8000u;
    keep[nkeep].len = 0x10000u;
    nkeep++;
    if (gic_va != 0) {
        keep[nkeep].addr = gic_va;
        keep[nkeep].len = 0x2000u;
        nkeep++;
    }
    unsigned kept = 0;
    char kept_list[256];
    int kl = 0;
    for (unsigned i = 0; i < nkeep; i++) {
        if (keep[i].addr == 0 || keep[i].len == 0) {
            continue;
        }
        const uint32_t first = keep[i].addr >> 20;
        const uint32_t last = (keep[i].addr + keep[i].len - 1u) >> 20;
        if (last - first >= 64u) {
            // No legitimate keep range spans 64 megabytes; this also catches wrap-around
            // (last < first underflows huge). Refusing here is the backstop that keeps a
            // miscomputed range from sweeping the whole table one log line at a time.
            emit("  seal: range %08x+%x implausible, refused\n", keep[i].addr, keep[i].len);
            continue;
        }
        unsigned skipped = 0;
        for (uint32_t idx = first; idx <= last; idx++) {
            if (azahar_l1[idx] & 3u) {
                continue;
            }
            const uint32_t e = KERNEL_L1(idx);
            if ((e & 3u) == 0) {
                // A megabyte the kernel's own table does not map cannot be one anything on
                // this core needs - the kernel could not touch it either. Ranges may
                // legitimately brush such megabytes; skip them rather than fail the install.
                skipped++;
                continue;
            }
            azahar_l1[idx] = e;
            kept++;
            if (kl < (int)sizeof(kept_list) - 8) {
                kl += snprintf(kept_list + kl, sizeof(kept_list) - kl, " %03x", idx);
            }
        }
        if (skipped != 0) {
            emit("  seal: %u unmapped megabyte(s) inside %08x+%x skipped\n", skipped,
                 keep[i].addr, keep[i].len);
        }
    }
    emit("  L1 sealed: %u kernel megabyte(s) kept (%s ), the rest invalid; kernel ttbr0 table at "
         "va %p, ttbr1 pa %08x, core-2 sp %08x\n",
         kept, kept_list, (const void *)kernel_l1, ttbr1_pa, azahar_core2_sp);
#else
    for (unsigned i = 0; i < AZAHAR_L1_ENTRIES; i++) {
        azahar_l1[i] = KERNEL_L1(i);
    }
    emit("  L1 merged from the kernel's two tables (AZAHAR_SEAL=0): ttbr0 table at va %p, ttbr1 pa "
         "%08x\n", (const void *)kernel_l1, ttbr1_pa);
#endif

    // The vector page: the highest free megabyte below 0x40000000 that no guest range wants.
    // The top half of the low quarter is fully populated on the console (no free entry in
    // 0x200-0x3ff), so the scan runs down to 1. "Free" is type bits 00, not a zero word: every
    // entry in 0x200-0x3ff is non-zero and none of them valid.
    unsigned upper_used = 0;
    for (uint32_t idx = 0x200; idx < 0x400; idx++) {
        upper_used += (KERNEL_L1(idx) & 3u) != 0;
    }
    azahar_vec_va = 0;
    for (uint32_t idx = 0x3FF; idx >= 1; idx--) {
        if ((KERNEL_L1(idx) & 3u) != 0 || (azahar_l1[idx] & 3u) != 0) {
            continue;
        }
        int clash = 0;
        for (unsigned r = 0; r < req->count; r++) {
            const uint32_t lo = req->ranges[r].guest_va >> 20;
            const uint32_t hi = (req->ranges[r].guest_va + req->ranges[r].size - 1) >> 20;
            if (idx >= lo && idx <= hi) {
                clash = 1;
            }
        }
        if (!clash) {
            azahar_vec_va = idx << 20;
            break;
        }
    }
    if (!azahar_vec_va) {
        emit("  no free megabyte for the vector page (%u/512 entries used in 0x200-0x3ff)\n",
             upper_used);
        return AZAHAR_ERR_TABLE;
    }
    emit("  vector page slot: %08x (kernel's entry there %08x; %u/512 kernel entries valid in "
         "0x200-0x3ff)\n",
         azahar_vec_va, KERNEL_L1(azahar_vec_va >> 20), upper_used);

    // Per core, at AZAHAR_VEC_STRIDE: Sony's 16 words, four slots replaced with
    // `ldr pc, [pc, #0x34]` reading that core's handlers' addresses from 0x40..0x4c: UND at 0x04,
    // SVC at 0x08, PABT at 0x0c, DABT at 0x10.
    const volatile uint32_t *sony = (const volatile uint32_t *)(uintptr_t)azahar_core2_vbar;
    for (unsigned n = 0; n < AZAHAR_CORES; n++) {
        uint32_t *const vec = azahar_vec + n * (AZAHAR_VEC_STRIDE / 4);
        for (unsigned i = 0; i < 16; i++) {
            vec[i] = sony[i];
        }
        for (unsigned h = 0; h < 5; h++) {
            vec[0x40 / 4 + h] = (uint32_t)(uintptr_t)azahar_stubs[n][h];
        }
        vec[0x04 / 4] = 0xE59FF034u;
        vec[0x08 / 4] = 0xE59FF034u;
        vec[0x0C / 4] = 0xE59FF034u;
        vec[0x10 / 4] = 0xE59FF034u;
        vec[0x18 / 4] = 0xE59FF030u; // IRQ: at 0x18 the PC reads 0x20; pointer at 0x50
    }
    for (unsigned i = 0; i < 8; i++) {
        azahar_sony[i] = azahar_sony_target(azahar_core2_vbar, i);
    }
    emit("  sony vectors: rst %08x und %08x svc %08x pabt %08x dabt %08x irq %08x fiq %08x\n",
         azahar_sony[0], azahar_sony[1], azahar_sony[2], azahar_sony[3], azahar_sony[4], azahar_sony[6],
         azahar_sony[7]);
    azahar_l1[azahar_vec_va >> 20] = AZAHAR_SEC_PRIV_RWX((uint32_t)azahar_vec_pa);
    emit("  vectors: copied from %08x, ours at %08x (pa %08x); und %08x svc %08x pabt %08x dabt %08x irq %08x\n",
         azahar_core2_vbar, azahar_vec_va, (uint32_t)azahar_vec_pa, azahar_vec[0x40 / 4], azahar_vec[0x44 / 4],
         azahar_vec[0x48 / 4], azahar_vec[0x4C / 4], azahar_vec[0x50 / 4]);

    // The guest windows.
    azahar_l2_used = 0;
    azahar_windows.count = 0;
    for (unsigned i = 0; i < AZAHAR_L2_COUNT * 256; i++) {
        azahar_l2[i] = 0;
    }
    for (unsigned r = 0; r < req->count; r++) {
        const int e = map_range(&req->ranges[r], r);
        if (e) {
            return e;
        }
    }
    emit("  %u L2 table(s) in use of %u\n", azahar_l2_used, AZAHAR_L2_COUNT);

    // Preflight: at N=0 there is no TTBR1 to catch a gap, so what core 2 will touch must resolve.
    const uint32_t crit[] = {(uint32_t)(uintptr_t)&azahar_enter, (uint32_t)(uintptr_t)azahar_cpu,
                             azahar_core2_sp, gic_va, (uint32_t)(uintptr_t)&core2_run};
    static const char *const crit_name[] = {"entry code", "context", "core-2 stack", "gic",
                                            "core-2 C code"};
    // The GIC may legitimately be absent (azahar_gic says so and the guest runs unmasked); the
    // rest may not, and a zero address is not skipped.
    static const uint8_t crit_required[] = {1, 1, 1, 0, 1};
    for (unsigned i = 0; i < sizeof(crit) / sizeof(crit[0]); i++) {
        if (crit[i] == 0) {
            if (!crit_required[i]) {
                continue;
            }
            emit("  preflight: %s address is zero\n", crit_name[i]);
            return AZAHAR_ERR_TABLE;
        }
        if ((azahar_l1[crit[i] >> 20] & 3u) == 0) {
            emit("  preflight: %s at %08x -> l1[%03x] invalid\n", crit_name[i], crit[i],
                 crit[i] >> 20);
            return AZAHAR_ERR_TABLE;
        }
    }

    // The app's pages were cleaned one by one above; the tables and vectors go with them.
    // The vector page is code and core 2 fetches it through a mapping of its own, so it is
    // cleaned; the tables are read by the walker, which is coherent with these caches.
    ksceKernelDcacheCleanRange(azahar_vec, AZAHAR_VEC_STRIDE * AZAHAR_CORES);
    asm volatile("dsb" ::: "memory");
    return 0;
}

static AzaharMapRequest azahar_req;

void azahar_vfp_reset(void);

int azaharMap(const AzaharMapRequest *user_req) {
    uint32_t state;
    ENTER_SYSCALL(state);
    log_open();
    emit("== azaharMap\n");
    int ret = 0;

    if (azahar_state != ST_TAKEN) {
        ret = AZAHAR_ERR_STATE;
        goto out;
    }
    if (ksceKernelCopyFromUser(&azahar_req, user_req, sizeof(azahar_req)) < 0) {
        ret = AZAHAR_ERR_ARG;
        goto out;
    }
    if (azahar_req.count > AZAHAR_MAX_RANGES) {
        ret = AZAHAR_ERR_ARG;
        goto out;
    }
    // The trigger app maps its windows here and wants every run logged; the emulator installs an
    // empty table and adds windows with azaharEdit, and cannot afford a synced line per exit.
    azahar_verbose = azahar_req.count > 0;
    azahar_run_count = 0;
    azahar_vfp_reset(); // a new guest: the last one's patched VFP instructions go
    for (unsigned r = 0; r < azahar_req.count; r++) {
        const AzaharRange *rg = &azahar_req.ranges[r];
        if (!check_range(rg, 1) || !(rg->perm & AZAHAR_PERM_R) || (rg->perm & ~0xFu)) {
            emit("  range %u rejected: user %08x guest %08x size %08x perm %u\n", r, rg->user_va,
                 rg->guest_va, rg->size, rg->perm);
            ret = AZAHAR_ERR_ARG;
            goto out;
        }
    }

    if (azahar_folded) {
        // The slice runs on this thread, on this core, on this stack: what core2_entry records
        // about the resident thread is recorded here about the caller. The stack is the
        // syscall's, and the tables are the ones in force while the app runs.
        uint32_t sp;
        asm volatile("mov %0, sp" : "=r"(sp));
        azahar_on_core = (uint32_t)ksceKernelCpuId() & 3;
        azahar_core2_vbar = rd_vbar();
        azahar_core2_cpsr = rd_cpsr();
        azahar_core2_ttbr1 = rd_ttbr1();
        azahar_core2_sp = sp;
        emit("  folded: cpu%u vbar %08x ttbr1 %08x sp %08x\n", azahar_on_core, azahar_core2_vbar,
             azahar_core2_ttbr1, azahar_core2_sp);
    }
    if ((ret = build_table(&azahar_req)) != 0) {
        goto out;
    }
    if (azahar_folded) {
        // Prove the world switch on this core once, both ways, before any guest runs.
        const unsigned n = this_core();
        const AzaharCore *const core = &azahar_core[n];
        core2_install(n);
        core2_uninstall(n);
        emit("  installed and removed once on cpu%u: ttbcr %08x -> %08x, vbar %08x -> %08x, "
             "pmr %02x -> %02x, cpacr %08x fpexc %08x\n",
             n, core->sv_ttbcr, core->ttbcr_seen, core->sv_vbar,
             azahar_vec_va + n * AZAHAR_VEC_STRIDE, core->sv_pmr, AZAHAR_PMR_MASK, core->sv_cpacr,
             core->sv_fpexc);
        fold_measure_timer(n);
        // The mapping thread's core is the attached one.
        detach_all();
        azahar_core[n].attached = 1;
        azahar_state = ST_MAPPED;
        goto out;
    }
    if ((ret = core2_command(CMD_INSTALL, AZAHAR_WAIT_STEPS)) != 0) {
        goto out;
    }
    AzaharCore *const target = &azahar_core[AZAHAR_TARGET_CORE];
    emit("  installed on core %u: ttbcr %08x -> %08x, vbar %08x -> %08x, sp_und %08x sp_abt %08x "
         "sp_svc %08x sp_irq %08x, pmr %02x -> %02x, timer ppi was %s, cpacr %08x fpexc %08x\n",
         AZAHAR_TARGET_CORE, target->sv_ttbcr, target->ttbcr_seen, target->sv_vbar,
         azahar_vec_va + AZAHAR_TARGET_CORE * AZAHAR_VEC_STRIDE, target->sv_sp_und,
         target->sv_sp_abt, target->sv_sp_svc, target->sv_sp_irq, target->sv_pmr, AZAHAR_PMR_MASK,
         target->sv_isenabler_timer ? "enabled" : "disabled", target->sv_cpacr, target->sv_fpexc);

    // The private timer's clock, from the idle loop's samples of the free-running counter
    // against the system clock. A device-tree claim of a fixed 144 MHz against the A9
    // ratio exists; this is the number the quantum is actually converted with.
    target->timer_hz = 0;
    if (gic_va) {
        const uint32_t s0 = azahar_timer_sample, t0 = ksceKernelGetSystemTimeLow();
        ksceKernelDelayThread(AZAHAR_TIMER_MEASURE_US);
        const uint32_t s1 = azahar_timer_sample, t1 = ksceKernelGetSystemTimeLow();
        const uint32_t ticks = s0 - s1, us = t1 - t0; // counts down
        if (us) {
            target->timer_hz = (uint32_t)(((uint64_t)ticks * 1000000ull) / us);
        }
        emit("  private timer: %u ticks in %u us -> %u Hz (%u.%02u MHz)\n", ticks, us,
             target->timer_hz, target->timer_hz / 1000000u, (target->timer_hz / 10000u) % 100u);
    }
    azahar_state = ST_MAPPED;

out:
    emit("  -> %d\n", ret);
    log_close();
    EXIT_SYSCALL(state);
    return ret;
}

int azaharRun(AzaharRunRequest *user_req) {
    uint32_t state;
    ENTER_SYSCALL(state);
    const uint32_t start_us = ksceKernelGetSystemTimeLow();
    log_open();
    int ret = 0;
    // Folded, the slice runs on the caller's core (its thread is pinned); resident, on the
    // resident thread's.
    const unsigned n = azahar_folded ? this_core() : AZAHAR_TARGET_CORE;
    AzaharCpu *const cpu = &azahar_cpu[n];
    AzaharCore *const core = &azahar_core[n];
    AzaharRunRequest *const req = &core->run_req;
    AzaharFast *const fast = &cpu->fast;

    if (azahar_state != ST_MAPPED) {
        ret = AZAHAR_ERR_STATE;
        goto out;
    }
    if (azahar_folded && !core->attached) {
        // The saved banked SPs and the stack's seal entries are the attached thread's: a
        // thread that moved cores, or one never attached, would run on what is not its own.
        emit("  azaharRun on cpu%u, which azaharMap did not attach\n", n);
        ret = AZAHAR_ERR_CORE;
        goto out;
    }
    if (ksceKernelCopyFromUser(req, user_req, sizeof(*req)) < 0) {
        ret = AZAHAR_ERR_ARG;
        goto out;
    }
    // User mode only, and interrupts unmasked at the CPU: the GIC mask does the owning.
    cpu->ctx = req->ctx;
    cpu->ctx.cpsr = (cpu->ctx.cpsr & ~0x1DFu) | 0x10u; // keep T (bit 5); mode user; A/I/F clear
    core->quantum_ticks = 0;
    if (req->quantum_us) {
        if (!core->timer_hz) {
            emit("  quantum requested but cpu%u's timer clock is unmeasured\n", n);
            ret = AZAHAR_ERR_STATE;
            goto out;
        }
        const uint64_t t = ((uint64_t)req->quantum_us * core->timer_hz) / 1000000ull;
        core->quantum_ticks = t > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)t;
        if (!core->quantum_ticks) {
            core->quantum_ticks = 1;
        }
    }
    core->timer_left = 0;
    // What azahar_svc_fast may answer this slice. GetSystemTick needs the slice's timer, and the
    // conversion to guest ticks, made again only when one of the two clocks changes.
    fast->on = req->fast & AZAHAR_FAST_ALL;
    fast->timer = 0;
    const int timed = core->quantum_ticks && req->tick_hz;
    if (timed) {
        if (req->tick_hz != core->fast_tick_hz || core->timer_hz != core->fast_timer_hz) {
            core->fast_tick_hz = req->tick_hz;
            core->fast_timer_hz = core->timer_hz;
            fast->mult_int = (uint32_t)(core->fast_tick_hz / core->timer_hz);
            fast->mult_frac =
                (uint32_t)(((core->fast_tick_hz % core->timer_hz) << 32) / core->timer_hz);
        }
        if (gic_va) {
            fast->timer = gic_va + PTIMER_COUNTER;
        }
    }
    fast->quantum = core->quantum_ticks;
    fast->base_lo = (uint32_t)req->tick_base;
    fast->base_hi = (uint32_t)(req->tick_base >> 32);
    fast->thread_id = req->thread_id;
    fast->process_id = req->process_id;
    for (unsigned i = 0; i < AZAHAR_FAST_KINDS; i++) {
        fast->count[i] = 0;
    }
    if (azahar_verbose) {
        emit("== azaharRun  cpu%u pc %08x cpsr %08x sp %08x r0 %08x r1 %08x  quantum %u us = %u "
             "ticks\n",
             n, cpu->ctx.r[15], cpu->ctx.cpsr, cpu->ctx.r[13], cpu->ctx.r[0], cpu->ctx.r[1],
             req->quantum_us, core->quantum_ticks);
    }

    if (azahar_folded) {
        azahar_on_core = n;
        fold_run(n);
        ret = 0;
    } else if ((ret = core2_command(CMD_RUN, AZAHAR_RUN_WAIT_STEPS)) != 0) {
        emit("== azaharRun  pc %08x cpsr %08x sp %08x r0 %08x r1 %08x  quantum %u us = %u ticks\n",
             req->ctx.r[15], cpu->ctx.cpsr, cpu->ctx.r[13], cpu->ctx.r[0], cpu->ctx.r[1],
             req->quantum_us, core->quantum_ticks);
        // Where the core got to (AzaharCpu.progress), and which stubs were reached.
        emit("  progress %u  stubs und %s svc %s pabt %s dabt %s irq %s  irq id %08x stray %u\n",
             cpu->progress,
             cpu->prog_und == (uint32_t)(uintptr_t)&cpu->prog_und ? "hit" : "-",
             cpu->prog_svc == (uint32_t)(uintptr_t)&cpu->prog_svc ? "hit" : "-",
             cpu->prog_pabt == (uint32_t)(uintptr_t)&cpu->prog_pabt ? "hit" : "-",
             cpu->prog_dabt == (uint32_t)(uintptr_t)&cpu->prog_dabt ? "hit" : "-",
             cpu->prog_irq == (uint32_t)(uintptr_t)&cpu->prog_irq ? "hit" : "-", cpu->irq_last,
             cpu->irq_stray);
        emit("  ctx now: r0 %08x r1 %08x r2 %08x r4 %08x sp %08x lr %08x pc/lr %08x cpsr/spsr %08x\n",
             cpu->ctx.r[0], cpu->ctx.r[1], cpu->ctx.r[2], cpu->ctx.r[4], cpu->ctx.r[13],
             cpu->ctx.r[14], cpu->ctx.r[15], cpu->ctx.cpsr);
        emit("  exit info: reason %u rawlr %08x spsr %08x dfsr %08x dfar %08x ifsr %08x ifar %08x\n",
             cpu->exit_info[0], cpu->exit_info[1], cpu->exit_info[2], cpu->exit_info[3],
             cpu->exit_info[4], cpu->exit_info[5], cpu->exit_info[6]);
        goto out;
    }
    req->ctx = cpu->ctx;
    req->reason = core->run_reason;
    req->svc_number = core->run_svc;
    req->fsr = core->run_fsr;
    req->far = core->run_far;
    req->spsr = core->run_spsr;
    req->raw_lr = core->run_rawlr;
    req->timer_hz = core->timer_hz;
    req->timer_left = core->timer_left;
    // The counter read at the exit is below every one azahar_svc_fast read, so no GetSystemTick
    // it answered is ahead of tick_base + guest_ticks.
    req->guest_ticks = timed ? fast_guest_ticks(fast, core->quantum_ticks - core->timer_left) : 0;
    for (unsigned i = 0; i < AZAHAR_FAST_KINDS; i++) {
        req->fast_count[i] = fast->count[i];
    }
    req->kernel_us = ksceKernelGetSystemTimeLow() - start_us;
    azahar_run_count++;
    if (azahar_verbose || (core->run_reason > AZAHAR_EXIT_SVC && core->run_reason != AZAHAR_EXIT_TIMER)) {
        // Faults are rare enough to record. svc exits are the common case, and a guest that
        // busy-waits through its quantum makes preemptions just as common (hundreds a minute
        // in the first commercial title tried), so both stay silent unless verbose.
        emit("  run %u (cpu%u): exit reason %u  svc %06x  pc %08x  raw lr %08x  spsr %08x  fsr %08x "
             "far %08x  r0 %08x r1 %08x sp %08x  timer left %u  irq id %08x stray %u\n",
             azahar_run_count, n, core->run_reason, core->run_svc, cpu->ctx.r[15], core->run_rawlr,
             core->run_spsr, core->run_fsr, core->run_far, cpu->ctx.r[0], cpu->ctx.r[1],
             cpu->ctx.r[13], core->timer_left, cpu->irq_last, cpu->irq_stray);
    }
    if (ksceKernelCopyToUser(user_req, req, sizeof(*req)) < 0) {
        ret = AZAHAR_ERR_ARG;
    }

out:
    if (ret) {
        emit("  -> %d\n", ret);
    }
    log_close();
    EXIT_SYSCALL(state);
    return ret;
}

static AzaharEditRequest azahar_edit_req;

// ---------------------------------------------------------------------------- VFP short vectors
// azaharVfpPatch's registered instructions, by guest address (open addressing; pc 0 is empty),
// and azahar_vfp_svc, which azahar_svc_fast calls for svc AZAHAR_VFP_SVC: Supervisor mode, IRQs
// masked, the core's VFP stack, the guest's VFP bank live in the registers. The bank and FPSCR
// are saved first and put back last (the compiler may use any VFP register in between), and the
// elements run with LEN and STRIDE cleared, as scalars, the way the app's RunVfpVector runs
// them; FPSCR keeps the guest's LEN and STRIDE and takes the elements' exception flags.
typedef struct AzaharVfpEntry {
    uint32_t pc, form;
} AzaharVfpEntry;
static AzaharVfpEntry azahar_vfp_table[AZAHAR_VFP_TABLE];
#define AZAHAR_FPSCR_VECTOR 0x00370000u // STRIDE (21:20) and LEN (18:16)

static uint32_t vfp_slot(uint32_t pc) {
    return ((pc >> 1) * 2654435761u) >> 20; // 12 bits: AZAHAR_VFP_TABLE
}
_Static_assert(AZAHAR_VFP_TABLE == 4096u, "vfp_slot's 12 bits");

static float vfp_single(uint32_t op, float d, float n, float m) {
    switch (op) {
    case AZAHAR_VFP_OP_MLA: asm volatile("vmla.f32 %0, %1, %2" : "+t"(d) : "t"(n), "t"(m)); break;
    case AZAHAR_VFP_OP_MLS: asm volatile("vmls.f32 %0, %1, %2" : "+t"(d) : "t"(n), "t"(m)); break;
    case AZAHAR_VFP_OP_NMLS: asm volatile("vnmls.f32 %0, %1, %2" : "+t"(d) : "t"(n), "t"(m)); break;
    case AZAHAR_VFP_OP_NMLA: asm volatile("vnmla.f32 %0, %1, %2" : "+t"(d) : "t"(n), "t"(m)); break;
    case AZAHAR_VFP_OP_MUL: asm volatile("vmul.f32 %0, %1, %2" : "=t"(d) : "t"(n), "t"(m)); break;
    case AZAHAR_VFP_OP_NMUL: asm volatile("vnmul.f32 %0, %1, %2" : "=t"(d) : "t"(n), "t"(m)); break;
    case AZAHAR_VFP_OP_ADD: asm volatile("vadd.f32 %0, %1, %2" : "=t"(d) : "t"(n), "t"(m)); break;
    case AZAHAR_VFP_OP_SUB: asm volatile("vsub.f32 %0, %1, %2" : "=t"(d) : "t"(n), "t"(m)); break;
    case AZAHAR_VFP_OP_DIV: asm volatile("vdiv.f32 %0, %1, %2" : "=t"(d) : "t"(n), "t"(m)); break;
    case AZAHAR_VFP_OP_CPY: asm volatile("vmov.f32 %0, %1" : "=t"(d) : "t"(m)); break;
    case AZAHAR_VFP_OP_ABS: asm volatile("vabs.f32 %0, %1" : "=t"(d) : "t"(m)); break;
    case AZAHAR_VFP_OP_NEG: asm volatile("vneg.f32 %0, %1" : "=t"(d) : "t"(m)); break;
    case AZAHAR_VFP_OP_SQRT: asm volatile("vsqrt.f32 %0, %1" : "=t"(d) : "t"(m)); break;
    }
    return d;
}

static double vfp_double(uint32_t op, double d, double n, double m) {
    switch (op) {
    case AZAHAR_VFP_OP_MLA: asm volatile("vmla.f64 %P0, %P1, %P2" : "+w"(d) : "w"(n), "w"(m)); break;
    case AZAHAR_VFP_OP_MLS: asm volatile("vmls.f64 %P0, %P1, %P2" : "+w"(d) : "w"(n), "w"(m)); break;
    case AZAHAR_VFP_OP_NMLS: asm volatile("vnmls.f64 %P0, %P1, %P2" : "+w"(d) : "w"(n), "w"(m)); break;
    case AZAHAR_VFP_OP_NMLA: asm volatile("vnmla.f64 %P0, %P1, %P2" : "+w"(d) : "w"(n), "w"(m)); break;
    case AZAHAR_VFP_OP_MUL: asm volatile("vmul.f64 %P0, %P1, %P2" : "=w"(d) : "w"(n), "w"(m)); break;
    case AZAHAR_VFP_OP_NMUL: asm volatile("vnmul.f64 %P0, %P1, %P2" : "=w"(d) : "w"(n), "w"(m)); break;
    case AZAHAR_VFP_OP_ADD: asm volatile("vadd.f64 %P0, %P1, %P2" : "=w"(d) : "w"(n), "w"(m)); break;
    case AZAHAR_VFP_OP_SUB: asm volatile("vsub.f64 %P0, %P1, %P2" : "=w"(d) : "w"(n), "w"(m)); break;
    case AZAHAR_VFP_OP_DIV: asm volatile("vdiv.f64 %P0, %P1, %P2" : "=w"(d) : "w"(n), "w"(m)); break;
    case AZAHAR_VFP_OP_CPY: asm volatile("vmov.f64 %P0, %P1" : "=w"(d) : "w"(m)); break;
    case AZAHAR_VFP_OP_ABS: asm volatile("vabs.f64 %P0, %P1" : "=w"(d) : "w"(m)); break;
    case AZAHAR_VFP_OP_NEG: asm volatile("vneg.f64 %P0, %P1" : "=w"(d) : "w"(m)); break;
    case AZAHAR_VFP_OP_SQRT: asm volatile("vsqrt.f64 %P0, %P1" : "=w"(d) : "w"(m)); break;
    }
    return d;
}

// Returns 1 when the instruction at pc ran, 0 when none is registered there.
uint32_t azahar_vfp_svc(uint32_t pc) {
    const AzaharVfpEntry *e = NULL;
    for (uint32_t i = vfp_slot(pc), k = 0; k < AZAHAR_VFP_TABLE; i = (i + 1) & (AZAHAR_VFP_TABLE - 1), k++) {
        if (azahar_vfp_table[i].pc == pc) {
            e = &azahar_vfp_table[i];
            break;
        }
        if (azahar_vfp_table[i].pc == 0) {
            return 0;
        }
    }
    if (!e) {
        return 0;
    }
    uint64_t bank[32] __attribute__((aligned(8)));
    uint32_t fpscr, flags;
    asm volatile(".fpu neon \n vmrs %0, fpscr" : "=r"(fpscr));
    vfp_bank_save(bank);
    asm volatile(".fpu neon \n vmsr fpscr, %0" ::"r"(fpscr & ~AZAHAR_FPSCR_VECTOR) : "memory");
    const uint32_t form = e->form;
    const uint32_t op = form & 15, dp = form >> 4 & 1, two = form >> 5 & 1;
    const uint32_t d = form >> 8 & 31, n = form >> 16 & 31, m = form >> 24 & 31;
    const uint32_t bank_size = dp ? 4 : 8;
    const uint32_t stride = (fpscr >> 20 & 3) == 3 ? 2 : 1;
    const uint32_t len = d < bank_size ? 1 : (fpscr >> 16 & 7) + 1;
    const int m_scalar = m < bank_size;
    uint32_t di = d, ni = n, mi = m;
    uint32_t *const s = (uint32_t *)bank;
    for (uint32_t k = 0; k < len; k++) {
        if (dp) {
            double vd, vn, vm;
            __builtin_memcpy(&vd, &bank[di], 8);
            __builtin_memcpy(&vn, &bank[ni], 8);
            __builtin_memcpy(&vm, &bank[mi], 8);
            const double r = vfp_double(op, vd, two ? 0.0 : vn, vm);
            __builtin_memcpy(&bank[di], &r, 8);
        } else {
            float vd, vn, vm;
            __builtin_memcpy(&vd, &s[di], 4);
            __builtin_memcpy(&vn, &s[ni], 4);
            __builtin_memcpy(&vm, &s[mi], 4);
            const float r = vfp_single(op, vd, two ? 0.0f : vn, vm);
            __builtin_memcpy(&s[di], &r, 4);
        }
        di = di / bank_size * bank_size + (di % bank_size + stride) % bank_size;
        ni = ni / bank_size * bank_size + (ni % bank_size + stride) % bank_size;
        if (!m_scalar) {
            mi = mi / bank_size * bank_size + (mi % bank_size + stride) % bank_size;
        }
    }
    asm volatile(".fpu neon \n vmrs %0, fpscr" : "=r"(flags));
    vfp_bank_load(bank);
    asm volatile(".fpu neon \n vmsr fpscr, %0" ::"r"((fpscr & AZAHAR_FPSCR_VECTOR) |
                                                     (flags & ~AZAHAR_FPSCR_VECTOR))
                 : "memory");
    return 1;
}

void azahar_vfp_reset(void) {
    for (uint32_t i = 0; i < AZAHAR_VFP_TABLE; i++) {
        azahar_vfp_table[i].pc = 0;
    }
}

int azaharVfpPatch(uint32_t guest_pc, uint32_t form) {
    uint32_t state;
    ENTER_SYSCALL(state);
    int ret = 0;
    const uint32_t op = form & 15, dp = form >> 4 & 1;
    const uint32_t d = form >> 8 & 31, n = form >> 16 & 31, m = form >> 24 & 31;
    if (guest_pc == 0 || op > AZAHAR_VFP_OP_SQRT || (form & 0xc0c0c0c0u) != 0 ||
        (dp && (d > 15 || n > 15 || m > 15))) {
        ret = AZAHAR_ERR_ARG;
        goto out;
    }
    // Between slices only: the guest is stopped at an exit while the app handles it.
    ret = AZAHAR_ERR_STATE;
    for (uint32_t i = vfp_slot(guest_pc), k = 0; k < AZAHAR_VFP_TABLE; i = (i + 1) & (AZAHAR_VFP_TABLE - 1), k++) {
        if (azahar_vfp_table[i].pc == guest_pc || azahar_vfp_table[i].pc == 0) {
            azahar_vfp_table[i].form = form;
            azahar_vfp_table[i].pc = guest_pc;
            ret = 0;
            break;
        }
    }
out:
    EXIT_SYSCALL(state);
    return ret;
}

int azaharEdit(const AzaharEditRequest *user_req) {
    uint32_t state;
    ENTER_SYSCALL(state);
    log_open();
    int ret = 0;
    int quiet = 0; /* SYNC_CODE comes once a frame on some titles: logged only when it fails */
    int locked = 0;

    if (azahar_state != ST_MAPPED) {
        ret = AZAHAR_ERR_STATE;
        goto out;
    }
    // The request copy, the descriptors, the L2 pool and the maintenance after them, one edit
    // at a time.
    ksceKernelLockMutex(azahar_edit_mutex, 1, NULL);
    locked = 1;
    if (ksceKernelCopyFromUser(&azahar_edit_req, user_req, sizeof(azahar_edit_req)) < 0) {
        ret = AZAHAR_ERR_ARG;
        goto out;
    }
    const AzaharRange *rg = &azahar_edit_req.range;
    const uint32_t op = azahar_edit_req.op;
    /* SYNC_CODE and PROTECT can come many times a second: logged only when they fail */
    quiet = op == AZAHAR_EDIT_SYNC_CODE || op == AZAHAR_EDIT_PROTECT;
    if (!quiet)
        emit("== azaharEdit %s guest %08x user %08x size %08x\n",
             op == AZAHAR_EDIT_MAP ? "MAP" : op == AZAHAR_EDIT_UNMAP ? "UNMAP" : op == AZAHAR_EDIT_PROTECT ? "PROTECT"
             : op == AZAHAR_EDIT_SYNC_CODE ? "SYNC_CODE" : "?",
             rg->guest_va, rg->user_va, rg->size);
    if (op == AZAHAR_EDIT_SYNC_CODE) {
        if ((rg->user_va & 0xFFFu) || (rg->size & 0xFFFu) || !rg->size) {
            ret = AZAHAR_ERR_ARG;
            goto out;
        }
        // The app wrote instructions through its own view; they are in some core's data cache.
        // Clean them to the point of unification page by page under the domain grant, and let
        // the next run drop core 2's instruction cache and branch predictor.
        for (uint32_t off = 0; off < rg->size; off += 0x1000) {
            SceKernelIntrStatus st = ksceKernelCpuSuspendIntr();
            const uint32_t d = rd_dacr();
            wr_dacr(dacr_grant_all(d));
            ksceKernelDcacheCleanRange((void *)(uintptr_t)(rg->user_va + off), 0x1000);
            wr_dacr(d);
            ksceKernelCpuResumeIntr(st);
        }
        tables_changed(1); // the whole point of this op: the guest rewrote instructions
        goto out;
    }
    if (!check_range(rg, op == AZAHAR_EDIT_MAP) ||
        (op != AZAHAR_EDIT_UNMAP && (!(rg->perm & AZAHAR_PERM_R) || (rg->perm & ~0xFu)))) {
        ret = AZAHAR_ERR_ARG;
        goto out;
    }

    switch (op) {
        case AZAHAR_EDIT_MAP:
            ret = map_range(rg, 0xEDu);
            break;
        case AZAHAR_EDIT_UNMAP:
        case AZAHAR_EDIT_PROTECT: {
            if (op == AZAHAR_EDIT_UNMAP &&
                windows_remove(&azahar_windows, rg->guest_va, rg->size) != 0) {
                emit("  out of window records (%u)\n", AZAHAR_WINDOWS);
                ret = AZAHAR_ERR_ALLOC;
                break;
            }
            unsigned done = 0, missing = 0;
            // The descriptors this edit actually writes, so the clean below can cover them
            // rather than the whole table array.
            uint32_t *lo = NULL, *hi = NULL;
            for (uint32_t off = 0; off < rg->size; off += 0x1000) {
                const uint32_t gva = rg->guest_va + off;
                const int t = l2_for(gva >> 20, 0);
                uint32_t *d = t < 0 ? NULL : &azahar_l2[(unsigned)t * 256u + ((gva >> 12) & 0xFFu)];
                if (!d || (*d & 3u) == 0) {
                    missing++;
                    continue;
                }
                // A reprotect keeps the page's memory attributes (AZAHAR_PERM_APP_ATTR ranges).
                *d = (op == AZAHAR_EDIT_UNMAP)
                         ? 0u
                         : azahar_small_page(*d & 0xFFFFF000u, rg->perm, *d & AZAHAR_ATTR_MASK);
                if (lo == NULL || d < lo) {
                    lo = d;
                }
                if (hi == NULL || d > hi) {
                    hi = d;
                }
                done++;
            }
            if (!quiet || missing)
                emit("  %u page(s) %s, %u not mapped\n", done,
                     op == AZAHAR_EDIT_UNMAP ? "unmapped" : "reprotected", missing);
            if (op == AZAHAR_EDIT_PROTECT && missing) {
                ret = AZAHAR_ERR_ARG;
            }
            (void)lo;
            (void)hi;
            break;
        }
        default:
            ret = AZAHAR_ERR_ARG;
            break;
    }
    if (ret == 0) {
        // No cache maintenance on the descriptors, only the barrier that orders them before
        // the invalidation the next slice does. The tables live in ordinary cacheable kernel
        // memory and our TTBR0 keeps Sony's own walk attributes (core2_install takes the low
        // fourteen bits of the kernel's), so the table walker reads them coherently, the way
        // the kernel's own tables are read: it edits those constantly and cleans nothing.
        // Cleaning the whole array on every call would be 256 KB of descriptors plus 16 KB of
        // the top level, whatever the edit had touched.
        // A map can bring different code to an address a core has executed at, and an unmap
        // is cheap enough not to argue about.
        tables_changed(op != AZAHAR_EDIT_PROTECT);
    }

out:
    if (locked) {
        ksceKernelUnlockMutex(azahar_edit_mutex, 1);
    }
    if (!quiet || ret != 0)
        emit("  -> %d\n", ret);
    log_close();
    EXIT_SYSCALL(state);
    return ret;
}

// A guest small page's descriptor with the access a hold leaves the guest (AZAHAR_HOLD_*), in
// AP[2:1] (AFE = 1): NONE 01 (PL0 read-write), WRITES 11 (read-only), ALL 10 (PL1 read-only,
// nothing at PL0). The rest of it, the memory attributes and XN included, stays.
static uint32_t azahar_hold_desc(uint32_t d, uint32_t hold) {
    d &= ~((1u << 9) | (1u << 5));
    switch (hold) {
        case AZAHAR_HOLD_NONE: return d | (1u << 5);
        case AZAHAR_HOLD_WRITES: return d | (1u << 9) | (1u << 5);
        default: return d | (1u << 9);
    }
}

static AzaharHoldRun azahar_hold_runs[AZAHAR_HOLD_MAX_RUNS];

int azaharHold(const AzaharHoldRun *user_runs, uint32_t count) {
    uint32_t state;
    ENTER_SYSCALL(state);
    int ret = 0;
    int locked = 0;
    if (azahar_state != ST_MAPPED) {
        ret = AZAHAR_ERR_STATE;
        goto out;
    }
    if (count == 0 || count > AZAHAR_HOLD_MAX_RUNS) {
        ret = AZAHAR_ERR_ARG;
        goto out;
    }
    // The windows, the descriptors and the maintenance after them, as azaharEdit's.
    ksceKernelLockMutex(azahar_edit_mutex, 1, NULL);
    locked = 1;
    if (ksceKernelCopyFromUser(azahar_hold_runs, user_runs, count * sizeof(AzaharHoldRun)) < 0) {
        ret = AZAHAR_ERR_ARG;
        goto out;
    }
    unsigned changed = 0;
    for (uint32_t r = 0; r < count; r++) {
        const AzaharHoldRun *h = &azahar_hold_runs[r];
        const uint64_t lo = h->user_va, hi = lo + h->size;
        if ((h->user_va & 0xFFFu) || (h->size & 0xFFFu) || h->size == 0 ||
            h->hold > AZAHAR_HOLD_ALL || hi > 0x100000000ull) {
            ret = AZAHAR_ERR_ARG;
            goto out;
        }
        // Every window that shows a page of the run: the same page at each guest address.
        for (unsigned w = 0; w < azahar_windows.count; w++) {
            const AzaharWindow *win = &azahar_windows.w[w];
            const uint64_t a = lo > win->uva ? lo : win->uva;
            const uint64_t b = hi < (uint64_t)win->uva + win->size ? hi : (uint64_t)win->uva + win->size;
            for (uint64_t uva = a; uva < b; uva += 0x1000) {
                const uint32_t gva = win->va + (uint32_t)(uva - win->uva);
                const int t = l2_for(gva >> 20, 0);
                if (t < 0) {
                    continue;
                }
                uint32_t *const d = &azahar_l2[(unsigned)t * 256u + ((gva >> 12) & 0xFFu)];
                if ((*d & 2u) == 0) {
                    continue;
                }
                const uint32_t nd = azahar_hold_desc(*d, h->hold);
                if (nd != *d) {
                    *d = nd;
                    changed++;
                }
            }
        }
    }
    if (changed) {
        // The walker reads the tables coherently (azaharEdit); the TLB goes at the next slice,
        // or now on a core inside one.
        tables_changed(0);
    }
    ret = (int)changed;

out:
    if (locked) {
        ksceKernelUnlockMutex(azahar_edit_mutex, 1);
    }
    EXIT_SYSCALL(state);
    return ret;
}

int azaharRelease(void) {
    uint32_t state;
    ENTER_SYSCALL(state);
    log_open();
    emit("== azaharRelease\n");
    int ret = 0;

    if (azahar_folded) {
        if (azahar_state != ST_MAPPED && azahar_state != ST_TAKEN) {
            ret = AZAHAR_ERR_STATE;
            goto out;
        }
        emit("  %u run(s) since the map (folded)\n", azahar_run_count);
        free_blocks();
        detach_all();
        azahar_state = ST_NONE;
        azahar_client_pid = -1;
        goto out;
    }
    if (azahar_state == ST_MAPPED) {
        ret = core2_command(CMD_UNINSTALL, AZAHAR_WAIT_STEPS);
        emit("  uninstall -> %d (vbar back to %08x)\n", ret,
             azahar_core[AZAHAR_TARGET_CORE].sv_vbar);
    } else if (azahar_state != ST_TAKEN) {
        ret = AZAHAR_ERR_STATE;
        goto out;
    }
    emit("  %u run(s) since the map\n", azahar_run_count);
    emit("  sgi windows %u  last pend %04x  stuck %u\n", azahar_sgi_windows, azahar_sgi_last_pend,
         azahar_sgi_stuck);
    const int ended = azahar_stop_resident();
    free_blocks();
    azahar_state = ended == 0 ? ST_NONE : ST_RELEASED;
    azahar_client_pid = -1;

out:
    emit("  -> %d\n", ret);
    log_release();
    EXIT_SYSCALL(state);
    return ret;
}

