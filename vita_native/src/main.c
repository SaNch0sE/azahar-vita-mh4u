// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// azahar-native: the kernel side of azahar's ARM32 native execution on the PS Vita, run
// under translation tables and exception vectors this module owns.
// The support half (report, cp15, translation, export resolver, peripheral block); native.c
// is the emulator half. One translation unit: the taken core avoids calls into other objects.
// DEVELOPMENT.md describes how the whole thing works.

#include <psp2kern/io/fcntl.h>
#include <taihen.h>
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/io/stat.h>
#include <psp2kern/kernel/cpu.h>
#include <psp2kern/kernel/debug.h>
#include <psp2kern/kernel/processmgr.h>
#include <psp2kern/kernel/cpu/cache.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/sysmem/address_space.h>
#include <psp2kern/kernel/sysmem/memtype.h>
#include <psp2kern/kernel/sysmem/uid_class.h>
#include <psp2kern/kernel/threadmgr.h>

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include "azahar_native.h"

int vsnprintf(char *s, size_t n, const char *fmt, va_list ap);

#define OUT_DIR "ux0:data/azahar"

// ---------------------------------------------------------------------------- cp15

// ---------------------------------------------------------------------------- cp15

#define CP15_READ(name, cn, op1, crm, op2)                                       \
    static inline uint32_t rd_##name(void) {                                     \
        uint32_t v;                                                              \
        asm volatile("mrc p15, " #op1 ", %0, " #cn ", " #crm ", " #op2 : "=r"(v));\
        return v;                                                                \
    }

CP15_READ(cpacr, c1, 0, c0, 2)
CP15_READ(ttbr0, c2, 0, c0, 0)
CP15_READ(ttbr1, c2, 0, c0, 1)
CP15_READ(ttbcr, c2, 0, c0, 2)
CP15_READ(dacr, c3, 0, c0, 0)
CP15_READ(par, c7, 0, c4, 0)
CP15_READ(vbar, c12, 0, c0, 0)
CP15_READ(contextidr, c13, 0, c0, 1)
CP15_READ(mpidr, c0, 0, c0, 5)
CP15_READ(cbar, c15, 4, c0, 0)

static inline void wr_cpacr(uint32_t v) {
    asm volatile("mcr p15, 0, %0, c1, c0, 2" ::"r"(v));
    asm volatile("isb" ::: "memory");
}

static inline void wr_dacr(uint32_t v) {
    asm volatile("mcr p15, 0, %0, c3, c0, 0" ::"r"(v) : "memory");
    asm volatile("isb" ::: "memory");
}

enum { AT_PRIV_R, AT_PRIV_W, AT_USER_R, AT_USER_W };

// PAR is a scratch register the kernel may clobber from an interrupt handler that does
// its own translation, so the MCR/MRC pair is kept interrupt-free.
static uint32_t translate(uint32_t va, int which) {
    SceKernelIntrStatus prev = ksceKernelCpuSuspendIntr();
    switch (which) {
        case AT_PRIV_R: asm volatile("mcr p15, 0, %0, c7, c8, 0" ::"r"(va) : "memory"); break;
        case AT_PRIV_W: asm volatile("mcr p15, 0, %0, c7, c8, 1" ::"r"(va) : "memory"); break;
        case AT_USER_R: asm volatile("mcr p15, 0, %0, c7, c8, 2" ::"r"(va) : "memory"); break;
        default:        asm volatile("mcr p15, 0, %0, c7, c8, 3" ::"r"(va) : "memory"); break;
    }
    asm volatile("isb" ::: "memory");
    uint32_t par = rd_par();
    ksceKernelCpuResumeIntr(prev);
    return par;
}

#define PAR_FAULTED(p) ((p) & 1)
#define PAR_FS(p) (((p) >> 1) & 0x3F)
#define PAR_PA(p) ((p) & 0xFFFFF000u)

// An ATS through a 64 KiB large-page descriptor returns the large page's first page (PAR bits
// 15:12 zero). base_par translates the 64 KiB-aligned VA; where both agree, the VA's bits go in.
static inline uint32_t par_large_page(uint32_t va, uint32_t par, uint32_t base_par) {
    if ((va & 0xF000u) && !PAR_FAULTED(par) && !PAR_FAULTED(base_par) &&
        PAR_PA(base_par) == PAR_PA(par)) {
        return par | (va & 0xF000u);
    }
    return par;
}


// The affinity mask bit for one core, as SceKernelThreadOptParam wants it.
#define CPU_AFFINITY(n) (0x10000 << (n))


// ---------------------------------------------------------------------------- report

// Streamed, so the upper-half scan cannot overflow a fixed report buffer.
static char buf[16 * 1024];
static int buf_len;
static SceUID out_fd = -1;
static int total_written;

// When set, every emitted line is written and synced to the media before anything else runs.
// That is the whole debugging strategy for a boot-path fault: the probe runs from
// module_start, so a fault there bootloops the console and the only evidence is whatever
// reached the memory card. Buffered output would be lost on the hard power-off that follows.
static int log_sync;

static void flush_buf(void) {
    if (buf_len > 0 && out_fd >= 0) {
        ksceIoWrite(out_fd, buf, buf_len);
        total_written += buf_len;
        if (log_sync) {
            int status = 0;
            ksceIoSyncByFd(out_fd, &status);
        }
    }
    buf_len = 0;
}

// The native calls mute everything by default (native.c, azahar_log_wanted): a title makes
// thousands of them a second, and every line written costs a file write and a kernel printf.
static int azahar_mute;

static void emit(const char *fmt, ...) {
    if (azahar_mute) {
        return;
    }
    if (buf_len > (int)sizeof(buf) - 512) {
        flush_buf();
    }
    int room = (int)sizeof(buf) - buf_len;
    if (room <= 1) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + buf_len, (size_t)room, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    // vsnprintf returns the length it *wanted*. Advancing by that past the end of the buffer
    // would make the next call's `room` negative and its size_t cast enormous.
    if (n >= room) {
        n = room - 1;
    }

    // Mirror to the kernel's own printf, which is visible live rather than only after the run.
    // The file stays the record — it survives a hang and a hard reset — but on a retail console
    // with no UART and no usable LED, this is the only channel that says "still working" while
    // the app is blocked inside the syscall. kubridge logs the same way.
    ksceKernelPrintf("[azaharnative] %s", buf + buf_len);

    buf_len += n;
    if (log_sync) {
        flush_buf();
    }
}

// Searches the whole 32-bit space for a VA that translates to `pa`. It has to be the whole
// space and not just the low quarter: the low quarter is the kernel's shared table, but a
// device alias like the private peripheral block is just as likely to sit in per-process
// TTBR1 space, and a search that misses it silently costs the entire routing section — the
// one measurement that most depends on it.
//
// A megabyte whose L1 entry is absent (FS 0x05) cannot contain the alias, so it is skipped
// whole. That reduces a 1M-page sweep to 4096 probes plus 256 per claimed megabyte.
//
// VA 0 doubles as "not found" and is never returned; page 0 is unmapped in every context
// measured so far, and a guest null dereference must fault there anyway.
static uint32_t find_va_for_pa(uint32_t pa) {
    for (uint32_t mb = 0; mb < 0x1000; mb++) {
        uint32_t base = mb << 20;
        uint32_t par = translate(base, AT_PRIV_R);
        if (PAR_FAULTED(par) && PAR_FS(par) == 0x05) {
            continue;
        }
        for (uint32_t off = 0; off < 0x100000u; off += 0x1000) {
            uint32_t va = base + off;
            if (!va) {
                continue;
            }
            uint32_t p = (off == 0) ? par : translate(va, AT_PRIV_R);
            if (!PAR_FAULTED(p) && PAR_PA(p) == pa) {
                return va;
            }
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------- private peripherals

static uint32_t gic_va;       // VA alias of PERIPHBASE, 0 if not found
static int gic_has_dist;      // the second page maps too, so the distributor is reachable

// CBAR gives PERIPHBASE as a physical address and nothing hands out a virtual alias for it, so
// the alias is searched for. The GIC CPU interface is what masks interrupts on the taken core,
// and the distributor page is what enables the private timer there, so both matter.
static void gic_locate(void) {
    uint32_t periphbase = rd_cbar() & 0xFFFFE000u;
    if (!periphbase) {
        return;
    }
    gic_va = find_va_for_pa(periphbase);
    if (!gic_va) {
        return;
    }
    uint32_t next = translate(gic_va + 0x1000, AT_PRIV_R);
    gic_has_dist = !PAR_FAULTED(next) && PAR_PA(next) == periphbase + 0x1000;
}

// ---------------------------------------------------------------------------- kernel exports

// [SceProcessmgrForKernel, ver=1]" — which costs every other stage as well. So they are
// resolved at runtime through taiHEN's exporter, tried against both firmware generations, and
// a failure disables this stage alone.
extern int module_get_export_func(SceUID pid, const char *modname, uint32_t libnid,
                                  uint32_t funcnid, uintptr_t *func);

#define AZAHAR_KERNEL_PID 0x10005


// SceKernelSystemInfo lives in the user headers (psp2/kernel/threadmgr/thread.h) and a kernel
// plugin only has psp2kern, so the layout is restated here. It is asserted at 0x48 bytes
// upstream: 4 + 4 + 4 * (8 + 4 + 4) = 72.
typedef struct AzaharSystemInfo {
    SceSize size;
    SceUInt32 activeCpuMask;
    struct {
        SceUInt64 idleClock;
        SceUInt32 comesOutOfIdleCount;
        SceUInt32 threadSwitchCount;
    } cpuInfo[4];
} AzaharSystemInfo;

typedef int (*fn_get_system_info)(AzaharSystemInfo *info);
typedef int (*fn_change_active_cpu_mask)(int mask);

static fn_get_system_info p_GetSystemInfo;


// The user NID 0x80544E0C copies out to a user pointer and fails from kernel context with
// 0xc0022005; the driver NID 0x7E280B69 (undocumented) takes a kernel struct with .size set.
#define SYSINFO_NID_DRIVER 0x7E280B69u
#define SYSINFO_NID_USER 0x80544E0Cu

static int sysinfo_resolved;

static int sysinfo_ready(void) {
    if (sysinfo_resolved) {
        return p_GetSystemInfo != NULL;
    }
    sysinfo_resolved = 1;

    uintptr_t f = 0;
    if (module_get_export_func(AZAHAR_KERNEL_PID, "SceKernelThreadMgr", 0xE2C40624,
                               SYSINFO_NID_DRIVER, &f) >= 0 && f) {
        emit("    %-26s resolved, driver NID (lib e2c40624 fn %08x)\n", "GetSystemInfo",
             SYSINFO_NID_DRIVER);
    } else if (module_get_export_func(AZAHAR_KERNEL_PID, "SceKernelThreadMgr", 0xE2C40624,
                                      SYSINFO_NID_USER, &f) >= 0 && f) {
        emit("    %-26s resolved, user NID (lib e2c40624 fn %08x) — this one copies out to a "
             "user pointer and has failed from kernel context before\n",
             "GetSystemInfo", SYSINFO_NID_USER);
    } else {
        emit("    %-26s NOT FOUND\n", "GetSystemInfo");
        return 0;
    }
    p_GetSystemInfo = (fn_get_system_info)f;
    return 1;
}

// The mask getter that is documented as not existing.
//
// That section records: "A companion sceKernelGetActiveCpuMask (0x0C3CBB8B) existed at 1.69 but
// is gone by 3.60; read activeCpuMask through sceKernelGetSystemInfo instead." True of the named
// export, and it sent both detach stages down the GetSystemInfo path — which then failed from
// kernel context and left the mask assumed for three runs.
//
// The henkaku wiki lists SceThreadmgrForDriver_86DAE59B, unnamed, described only as "Get global
// CPU affinity mask", guessed name sceKernelGetAllowedCpuAffinityMaskForDriver, prototype
// `int f(void)`. That is allowCpuMask, returned directly: no struct, no size field, and no
// copy-out semantics to get wrong. It is the right primary for the one value the detach stages
// cannot proceed without.
//
// Both are kept. Reading the same quantity two independent ways is worth the few bytes, and a
// disagreement between them is itself a finding.
#define ACTIVEMASK_NID 0x86DAE59Bu

typedef int (*fn_get_active_mask)(void);
static fn_get_active_mask p_GetActiveCpuMask;
static int activemask_resolved;

// Returns the mask, or 0 when unavailable — 0 is never a valid answer, since it would name a
// console with no cores at all.
static uint32_t active_cpu_mask(void) {
    if (!activemask_resolved) {
        activemask_resolved = 1;
        uintptr_t f = 0;
        if (module_get_export_func(AZAHAR_KERNEL_PID, "SceKernelThreadMgr", 0xE2C40624,
                                   ACTIVEMASK_NID, &f) >= 0 && f) {
            p_GetActiveCpuMask = (fn_get_active_mask)f;
            emit("    %-26s resolved, driver NID (lib e2c40624 fn %08x)\n", "GetActiveCpuMask",
                 ACTIVEMASK_NID);
        } else {
            emit("    %-26s NOT FOUND\n", "GetActiveCpuMask");
        }
    }
    return p_GetActiveCpuMask ? (uint32_t)p_GetActiveCpuMask() : 0u;
}

// 2: the sgxd calls (sgxd.c). 3: SCE-style error codes (azahar_native.h, AZAHAR_ERROR).
// 7: azaharAdopt, azaharReback and azaharRebackRestore: guest FCRAM under tables of ours, and
//    pages of it moved onto other physical pages (reback.c).
// 8: azaharRebackReturn, moved pages back on their own.
// 9: the ATS/PAR fix for 64 KiB pages.
// 10: azaharProfStart, azaharProfRead, azaharProfStop: the CPU sampler (prof.c).
// 11: the samples carry the running thread (thid for cid), azaharProfThreadName.
// 12: azaharProfKernelModule, the kernel's module segments.
// 13: sgxdSubmitAsync queues lists (SGXD_CHAIN_RING) and returns the first op's index.
// 14: azaharRun answers some svcs in the guest's own world (AZAHAR_FAST_*), and reports the
//     slice's guest ticks and its own time.
// 15: one world per Vita core, azaharAttach / azaharDetach: slices on several cores at once.
// 19: AzaharProfSample's syscall (svc, user PC, LR and SP) from the thread's SVC frame.
// 27: azaharAttach and azaharDetach gone, and the cross-core maintenance with them: one
//     emulation thread, one attached core.
int azaharVersion() {
    return 5;
}

// One read into `out`. Returns the call's own status; the caller decides what a failure means.

// ---------------------------------------------------------------------------- the emulator

#include "native.c"
#include "sgxd.c"
#include "reback.c"

// ---------------------------------------------------------------------------- module

int azaharModuleUid(void) {
    uint32_t state;
    ENTER_SYSCALL(state);
    tai_module_info_t info = {.size = sizeof(info)};
    int ret = taiGetModuleInfoForKernel(AZAHAR_KERNEL_PID, "azaharnative", &info);
    if (ret >= 0) {
        ret = ksceKernelCreateUserUid(ksceKernelGetProcessId(), info.modid);
    }
    EXIT_SYSCALL(state);
    return ret;
}

int module_start(SceSize argc, const void *args) {
    (void)argc;
    (void)args;
    // Nothing runs at load. The module waits for the emulator's syscalls, which is deliberate:
    // code on the boot path that faults bootloops the console, and the only evidence is
    // whatever reached the memory card first.
    return SCE_KERNEL_START_SUCCESS;
}

// Stopped only when no process holds any of it; nothing it registered or allocated outlives it.
int module_stop(SceSize argc, const void *args) {
    (void)argc;
    (void)args;
    if (azahar_state != ST_NONE || azahar_client_pid >= 0 || azahar_thread >= 0 ||
        sgxd_state != SGXD_NONE || sgxd_owner >= 0 || sgxd_isr_ours || reback_owner >= 0 ||
        shellcdram_pid >= 0 || shell_thread >= 0) {
        ksceKernelPrintf("[azaharnative] stop refused: in use (state %d, sgxd %d)\n", azahar_state,
                         sgxd_state);
        return SCE_KERNEL_STOP_CANCEL;
    }
    if (azahar_proc_handler >= 0) {
        ksceKernelUnregisterProcEventHandler(azahar_proc_handler);
        azahar_proc_handler = -1;
    }
    if (sgxd_sysevent_uid >= 0) {
        ksceKernelUnregisterSysEventHandler(sgxd_sysevent_uid);
        sgxd_sysevent_uid = -1;
    }
    sgxd_unmap();
    const SceUID blocks[] = {coresample_dbg_uid, reback_moved_uid};
    for (unsigned i = 0; i < sizeof(blocks) / sizeof(blocks[0]); i++) {
        if (blocks[i] >= 0) {
            ksceKernelFreeMemBlock(blocks[i]);
        }
    }
    coresample_dbg_uid = reback_moved_uid = -1;
    const SceUID mutexes[] = {sgxd_mutex, reback_mutex, azahar_edit_mutex};
    for (unsigned i = 0; i < sizeof(mutexes) / sizeof(mutexes[0]); i++) {
        if (mutexes[i] >= 0) {
            ksceKernelDeleteMutex(mutexes[i]);
        }
    }
    sgxd_mutex = reback_mutex = azahar_edit_mutex = -1;
    ksceKernelPrintf("[azaharnative] stopped (version %d)\n", AZAHAR_NATIVE_VERSION);
    return SCE_KERNEL_STOP_SUCCESS;
}

int _start(SceSize argc, const void *args) __attribute__((weak, alias("module_start")));
