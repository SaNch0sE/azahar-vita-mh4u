// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// azahar-native cycles.c: the PMU's cycle counter readable in user mode, for every thread of the
// calling process (azaharCycleCounter).
//
// ThreadMgr saves PMCR, PMCCNTR and PMUSERENR per thread, so the setting goes where it keeps
// them: the process's defaults for later threads (process object +0x28c PMUSERENR, +0x290 PMCR,
// written directly: ProcessMgr's setters need the debug dipsw), and every existing thread through
// ThreadMgr's driver calls. PMCNTENSET's C is not saved and a resume clears it: user code sets it
// itself (azaharCyclesStart).

#define CYCLES_PMUSERENR_EN 1u
#define CYCLES_PMCR_E 1u
// ThreadMgr's objects, 3.65 (SceThreadmgrForDriver_5053B005); 3.60, 3.68 and 3.73 alike.
#define CYCLES_THREAD_PROCESS 0x18u
#define CYCLES_PROCESS_PMUSERENR 0x28cu
#define CYCLES_PROCESS_PMCR 0x290u
// SceThreadmgrForDriver: (pid, value), pid 0 the caller's process; every existing thread's
// saved PMUSERENR or PMCR, and for pid 0 the register.
#define CYCLES_NID_SET_PMUSERENR 0x5053B005u
#define CYCLES_NID_SET_PMCR 0x1AAFA818u

typedef int (*fn_set_process_pmu)(SceUID pid, uint32_t value);

int azaharCycleCounter(void) {
    uint32_t state;
    ENTER_SYSCALL(state);
    static fn_set_process_pmu set_pmuserenr, set_pmcr;
    int ret = 0;
    if (!set_pmuserenr) {
        uintptr_t user = 0, control = 0;
        if (module_get_export_func(AZAHAR_KERNEL_PID, "SceKernelThreadMgr", 0xE2C40624,
                                   CYCLES_NID_SET_PMUSERENR, &user) < 0 || !user ||
            module_get_export_func(AZAHAR_KERNEL_PID, "SceKernelThreadMgr", 0xE2C40624,
                                   CYCLES_NID_SET_PMCR, &control) < 0 || !control) {
            ret = AZAHAR_ERR_RESOLVE;
            goto out;
        }
        set_pmuserenr = (fn_set_process_pmu)user;
        set_pmcr = (fn_set_process_pmu)control;
    }
    // The caller's process as ThreadMgr finds it: TPIDRPRW is the current thread, its +0x18 the
    // process.
    uint32_t thread;
    asm volatile("mrc p15, 0, %0, c13, c0, 4" : "=r"(thread));
    volatile uint8_t *const process =
        *(volatile uint8_t **)(uintptr_t)(thread + CYCLES_THREAD_PROCESS);
    *(volatile uint32_t *)(process + CYCLES_PROCESS_PMCR) = CYCLES_PMCR_E;
    *(volatile uint32_t *)(process + CYCLES_PROCESS_PMUSERENR) = CYCLES_PMUSERENR_EN;
    ret = set_pmcr(0, CYCLES_PMCR_E);
    if (ret >= 0) {
        ret = set_pmuserenr(0, CYCLES_PMUSERENR_EN);
    }
    ksceKernelPrintf("[azahar] cycle counter: process %p, its threads -> %08x\n", (void *)process,
                     (uint32_t)ret);
out:
    EXIT_SYSCALL(state);
    return ret;
}
