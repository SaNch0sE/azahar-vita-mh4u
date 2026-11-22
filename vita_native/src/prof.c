// azaharnative — the CPU sampler (azaharProf*): every core's program counter, read from the
// outside through each Cortex-A9's CoreSight debug block, core 2 included.
//
// Sony's DTrace samples from each core's own timer interrupt, so it cannot see a core whose
// interrupts are masked: core 2 while it runs the guest (native.c). The debug block's DBGPCSR
// needs nothing from the sampled core: any core reading it gets a recent PC of that core, and
// DBGCIDSR its CONTEXTIDR at the same moment (which reads 0 on the Vita; the thread comes from
// ThreadMgr's per-CPU blocks instead, below). This file runs a kernel thread on core 3, the
// system core, that reads both for every core in the mask at a set rate into a ring the app
// drains (azaharProfRead).
//
// The four debug blocks are at 0xE3310000 + 0x2000 * core, each core's PMU in the page after.
// DBGPCSR is
// register 33 (0x084) and DBGCIDSR register 41 (0x0A4), Cortex-A9 TRM "Debug register summary";
// the v7.1 alias of DBGPCSR, register 40 (0x0A0), reads zero on the Vita's cores.
// A PC read back is the instruction's address plus 8 in ARM state and plus 4 in Thumb state
// (bit 0 set), the ARMv7 debug architecture's offset; 0xFFFFFFFF means the core could not be
// sampled (non-invasive debug disabled, or the core in debug state). Nothing is written to the
// debug blocks: reading needs no unlock.

#define PROF_DBG_PA 0xE3310000u
#define PROF_DBG_SIZE 0x8000u
#define PROF_DBGPCSR 0x084u
#define PROF_DBGDIDR 0x000u
#define PROF_DBGCIDR1 0xFF4u
#define PROF_DBGCIDSR 0x0A4u
#define PROF_DBGAUTHSTATUS 0xFB8u
#define PROF_RING 65536u // samples, 1 MiB

// The thread each core runs, from SceKernelThreadMgr's per-CPU blocks: the first word of its
// data segment points at its private block, whose cpuCB[4] are 0x880 bytes apart. At +0x14 and +0x18 are the running thread's pid
// and thid (current[1]), which _sceKernelGetThreadRunStatus returns for old SDKs.
// That export
// hides another process's thread from a kernel caller (-1), so the sampler reads the words
// itself; they are plain word stores, and a stale one names the thread just before.
#define PROF_CPUCB_STRIDE 0x880u
#define PROF_CPUCB_THREAD 0x00u
#define PROF_CPUCB_PID 0x14u
#define PROF_CPUCB_THID 0x18u
#define PROF_CPUCB_IDLE 0x34u
static volatile const uint8_t *prof_cpucb;

// The running thread's syscall. Word 0 of a cpuCB is the running thread's object, the value the
// context switch also gives TPIDRPRW (SceThreadmgrForKernel_FEF07EC0). While the thread is in
// a syscall, its +0x44 points at the frame Sony's SVC handler saved on the thread's kernel stack
// (IntrSvcEntry, 0 again on the way out): user r12 (the syscall number) at +0x30, then the user
// SP, LR and PC at +0x50, +0x54, +0x58. The words change under the sampler: a thread switch or
// a syscall's end between two reads gives a frame that is stale or not the thread's, so each
// address is translated before it is read (a thread that exits takes its stack with it), and a
// number above the table's 0x1000 entries is dropped.
#define PROF_THREAD_SVC_FRAME 0x44u
#define PROF_FRAME_R12 0x30u
#define PROF_FRAME_SP 0x50u
#define PROF_FRAME_LR 0x54u
#define PROF_FRAME_PC 0x58u
#define PROF_FRAME_SIZE 0x60u

// Whether this core's MMU maps va for a privileged read (ATS1CPR, PAR bit 0 the fault), with
// interrupts off so nothing else uses PAR between the two.
static int prof_mapped(uint32_t va) {
    uint32_t cpsr, par;
    asm volatile("mrs %0, cpsr\n\tcpsid i" : "=r"(cpsr)::"memory");
    asm volatile("mcr p15, 0, %1, c7, c8, 0\n\tisb\n\tmrc p15, 0, %0, c7, c4, 0"
                 : "=r"(par)
                 : "r"(va)
                 : "memory");
    asm volatile("msr cpsr_c, %0" ::"r"(cpsr) : "memory");
    return !(par & 1u);
}

static void prof_syscall(uint32_t cpu, AzaharProfSample *s) {
    s->svc = s->user_pc = s->user_lr = s->user_sp = 0;
    const uint32_t thread =
        *(volatile const uint32_t *)(prof_cpucb + cpu * PROF_CPUCB_STRIDE + PROF_CPUCB_THREAD);
    if (!thread || !prof_mapped(thread + PROF_THREAD_SVC_FRAME)) {
        return;
    }
    const uint32_t frame = *(volatile const uint32_t *)(uintptr_t)(thread + PROF_THREAD_SVC_FRAME);
    // The frame sits inside one page or across two.
    if (!frame || (frame & 3u) || !prof_mapped(frame) ||
        !prof_mapped(frame + PROF_FRAME_SIZE - 4)) {
        return;
    }
    const volatile uint32_t *f = (const volatile uint32_t *)(uintptr_t)frame;
    const uint32_t number = f[PROF_FRAME_R12 / 4];
    if (number >= 0x1000u) {
        return;
    }
    s->user_sp = f[PROF_FRAME_SP / 4];
    s->user_lr = f[PROF_FRAME_LR / 4];
    s->user_pc = f[PROF_FRAME_PC / 4];
    s->svc = AZAHAR_PROF_SVC | number;
}

static SceUID prof_dbg_uid = -1;
static volatile uint32_t *prof_dbg;
static SceUID prof_ring_uid = -1;
static AzaharProfSample *prof_ring;
static volatile uint32_t prof_head, prof_tail, prof_dropped, prof_stop;
static SceUID prof_thread = -1;
// The last sampler's thid: its samples (core 3) are named after it is deleted.
static SceUID prof_last_thread = -1;
static uint32_t prof_period_us, prof_mask;

static int prof_sampler(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    uint32_t seq = 0;
    while (!prof_stop) {
        const uint32_t now = ksceKernelGetSystemTimeLow();
        for (uint32_t cpu = 0; cpu < 4; cpu++) {
            if (!(prof_mask & (1u << cpu))) {
                continue;
            }
            const volatile uint32_t *d = prof_dbg + (cpu * 0x2000u) / 4;
            const uint32_t pc = d[PROF_DBGPCSR / 4];
            // The thread the core runs (DBGCIDSR's CONTEXTIDR reads 0 on the Vita).
            const uint32_t thid =
                prof_cpucb ? *(volatile const uint32_t *)(prof_cpucb + cpu * PROF_CPUCB_STRIDE +
                                                          PROF_CPUCB_THID)
                           : 0;
            // Whether this core was running the guest at the read.
            const uint32_t on = azahar_guest_on;
            const uint32_t guest = on == (0x100u | cpu) ? 4u : 0u;
            const uint32_t h = prof_head;
            if (h - prof_tail >= PROF_RING) {
                prof_dropped++;
                continue;
            }
            AzaharProfSample *s = &prof_ring[h & (PROF_RING - 1)];
            s->time = now;
            s->pc = pc;
            s->thid = thid;
            s->cpu_seq = cpu | guest | (seq << 3);
            // Whose syscall, if the thread is in one (a guest slice never is).
            if (prof_cpucb && !guest && pc != 0xFFFFFFFFu) {
                prof_syscall(cpu, s);
            } else {
                s->svc = s->user_pc = s->user_lr = s->user_sp = 0;
            }
            asm volatile("dmb" ::: "memory");
            prof_head = h + 1;
        }
        seq++;
        ksceKernelDelayThread(prof_period_us);
    }
    return 0;
}

static void prof_free(void) {
    if (prof_thread >= 0) {
        prof_stop = 1;
        ksceKernelWaitThreadEnd(prof_thread, NULL, NULL);
        ksceKernelDeleteThread(prof_thread);
        prof_thread = -1;
    }
    if (prof_ring_uid >= 0) {
        ksceKernelFreeMemBlock(prof_ring_uid);
        prof_ring_uid = -1;
        prof_ring = NULL;
    }
    if (prof_dbg_uid >= 0) {
        ksceKernelFreeMemBlock(prof_dbg_uid);
        prof_dbg_uid = -1;
        prof_dbg = NULL;
    }
}

// ThreadMgr's cpuCB[0], or NULL when the block does not look like one (another firmware): four
// idle threads, each its own and in kernel memory.
static volatile const uint8_t *prof_find_cpucb(void) {
    tai_module_info_t tai = {.size = sizeof(tai)};
    uintptr_t data = 0;
    if (taiGetModuleInfoForKernel(AZAHAR_KERNEL_PID, "SceKernelThreadMgr", &tai) < 0 ||
        module_get_offset(AZAHAR_KERNEL_PID, tai.modid, 1, 0, &data) < 0 || !data) {
        return NULL;
    }
    const uint8_t *priv = *(const uint8_t *const *)data;
    if (!priv) {
        return NULL;
    }
    uint32_t idle[4];
    for (uint32_t cpu = 0; cpu < 4; cpu++) {
        idle[cpu] = *(const uint32_t *)(priv + cpu * PROF_CPUCB_STRIDE + PROF_CPUCB_IDLE);
        if (idle[cpu] < 0x00100000u || idle[cpu] >= 0x40000000u) {
            return NULL;
        }
        for (uint32_t k = 0; k < cpu; k++) {
            if (idle[k] == idle[cpu]) {
                return NULL;
            }
        }
    }
    ksceKernelPrintf("[azaharnative] prof: threadmgr %08x, cpuCB at %08x (idle %08x %08x %08x "
                     "%08x)\n",
                     tai.module_nid, (uint32_t)(uintptr_t)priv, idle[0], idle[1], idle[2], idle[3]);
    return priv;
}

// Starts sampling the cores in `cpu_mask` (bit n: core n) every `period_us` microseconds (the
// scheduler's delay is the floor). Returns core 0's DBGAUTHSTATUS (bits 2-3: non-invasive
// debug implemented and enabled) on success, which the caller logs; a second start restarts.
int azaharProfStart(uint32_t period_us, uint32_t cpu_mask) {
    uint32_t state;
    ENTER_SYSCALL(state);
    int ret = 0;
    prof_free();
    if (cpu_mask == 0 || cpu_mask > 0xFu || period_us == 0) {
        ret = AZAHAR_ERR_ARG;
        goto out;
    }
    SceKernelAllocMemBlockKernelOpt opt = {0};
    opt.size = sizeof(opt);
    opt.attr = 2; // physical address given
    opt.paddr = PROF_DBG_PA;
    prof_dbg_uid = ksceKernelAllocMemBlock("AzaharProfDbg", 0x20100206, PROF_DBG_SIZE, &opt);
    void *base = NULL;
    if (prof_dbg_uid < 0 || ksceKernelGetMemBlockBase(prof_dbg_uid, &base) < 0) {
        ret = AZAHAR_ERR_ALLOC;
        goto out;
    }
    prof_dbg = base;
    prof_ring_uid = ksceKernelAllocMemBlock("AzaharProfRing", SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RW,
                                            PROF_RING * sizeof(AzaharProfSample), NULL);
    base = NULL;
    if (prof_ring_uid < 0 || ksceKernelGetMemBlockBase(prof_ring_uid, &base) < 0) {
        ret = AZAHAR_ERR_ALLOC;
        goto out;
    }
    prof_ring = base;
    prof_cpucb = prof_find_cpucb();
    prof_head = prof_tail = prof_dropped = prof_stop = 0;
    prof_period_us = period_us;
    prof_mask = cpu_mask;
    // Core 3 is the system's: the app's threads and the guest run on cores 0-2.
    prof_thread = ksceKernelCreateThread("azaharnative_prof", prof_sampler, 0x10000100, 0x1000,
                                         0, CPU_AFFINITY(3), NULL);
    prof_last_thread = prof_thread;
    if (prof_thread < 0 || ksceKernelStartThread(prof_thread, 0, NULL) < 0) {
        ret = AZAHAR_ERR_CORE;
        goto out;
    }
    for (uint32_t cpu = 0; cpu < 4; cpu++) {
        const volatile uint32_t *d = prof_dbg + (cpu * 0x2000u) / 4;
        ksceKernelPrintf("[azaharnative] prof: core %u DBGDIDR %08x CIDR1 %08x AUTH %08x "
                         "PCSR84 %08x PCSRA0 %08x CIDSR %08x\n",
                         cpu, d[PROF_DBGDIDR / 4], d[PROF_DBGCIDR1 / 4],
                         d[PROF_DBGAUTHSTATUS / 4], d[0x084 / 4], d[0x0A0 / 4], d[0x0A4 / 4]);
    }
    ret = (int)(prof_dbg[PROF_DBGAUTHSTATUS / 4] & 0xFFu);
out:
    if (ret < 0) {
        prof_free();
    }
    EXIT_SYSCALL(state);
    return ret;
}

// Copies up to `max` samples, oldest first, into `user_samples`; `user_dropped` (may be NULL)
// gets the samples lost to a full ring since the last read. Returns the count.
int azaharProfRead(AzaharProfSample *user_samples, uint32_t max, uint32_t *user_dropped) {
    uint32_t state;
    ENTER_SYSCALL(state);
    int ret = 0;
    if (!prof_ring) {
        ret = AZAHAR_ERR_STATE;
        goto out;
    }
    uint32_t t = prof_tail;
    const uint32_t h = prof_head;
    uint32_t n = h - t;
    if (n > max) {
        n = max;
    }
    // At most two runs: to the ring's end, then from its start.
    uint32_t done = 0;
    while (done < n) {
        const uint32_t at = (t + done) & (PROF_RING - 1);
        uint32_t run = PROF_RING - at;
        if (run > n - done) {
            run = n - done;
        }
        if (ksceKernelCopyToUser(user_samples + done, &prof_ring[at],
                                 run * sizeof(AzaharProfSample)) < 0) {
            ret = AZAHAR_ERR_ARG;
            goto out;
        }
        done += run;
    }
    prof_tail = t + n;
    if (user_dropped) {
        const uint32_t d = prof_dropped;
        prof_dropped = 0;
        ksceKernelCopyToUser(user_dropped, &d, sizeof(d));
    }
    ret = (int)n;
out:
    EXIT_SYSCALL(state);
    return ret;
}

// The name of thread `thid` (a sample's thid) into `user_name` (32 bytes), and its process into
// `user_pid` (may be NULL). Returns 0, or the kernel's error for a thread that is gone.
int azaharProfThreadName(int32_t thid, char *user_name, int32_t *user_pid) {
    uint32_t state;
    ENTER_SYSCALL(state);
    SceKernelThreadInfo info;
    __builtin_memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    int ret = ksceKernelGetThreadInfo(thid, &info);
    if (ret < 0 && thid == prof_last_thread) {
        __builtin_memcpy(info.name, "azaharnative_prof", sizeof("azaharnative_prof"));
        info.processId = 0x10005; // the kernel process, as the idle threads report
        ret = 0;
    }
    if (ret >= 0) {
        info.name[sizeof(info.name) - 1] = 0;
        char name[32];
        __builtin_memcpy(name, info.name, sizeof(name));
        ret = ksceKernelCopyToUser(user_name, name, sizeof(name));
        if (ret >= 0 && user_pid) {
            ret = ksceKernelCopyToUser(user_pid, &info.processId, sizeof(info.processId));
        }
        ret = ret < 0 ? ret : 0;
    }
    EXIT_SYSCALL(state);
    return ret;
}

// The kernel's modules, for naming kernel samples: SceModulemgrForKernel's GetModuleList and
// GetModuleInfo, resolved through taiHEN (their NIDs changed after 3.60; these are 3.63's, which
// 3.65 keeps: vita-headers db/363/SceKernelModulemgr.yml).
typedef int (*ProfGetModuleList)(SceUID pid, int flags1, int flags2, SceUID *modids, SceSize *num);
typedef int (*ProfGetModuleInfo)(SceUID pid, SceUID modid, SceKernelModuleInfo *info);
static SceUID prof_kmods[256];
static SceSize prof_nkmods;

// Segment `index` of the kernel's modules (0 lists them afresh), into the user's `out`
// (AzaharProfModule). Returns 1, 0 past the last, or a negative error.
int azaharProfKernelModule(uint32_t index, AzaharProfModule *user_out) {
    uint32_t state;
    ENTER_SYSCALL(state);
    int ret;
    uintptr_t list = 0, get = 0;
    if (module_get_export_func(AZAHAR_KERNEL_PID, "SceKernelModulemgr", 0x92C9FFC2, 0xB72C75A4,
                               &list) < 0 ||
        module_get_export_func(AZAHAR_KERNEL_PID, "SceKernelModulemgr", 0x92C9FFC2, 0xDAA90093,
                               &get) < 0) {
        ret = -1;
        goto out;
    }
    if (index == 0) {
        prof_nkmods = sizeof(prof_kmods) / sizeof(prof_kmods[0]);
        ret = ((ProfGetModuleList)list)(AZAHAR_KERNEL_PID, 0x7FFFFFFF, 1, prof_kmods, &prof_nkmods);
        if (ret < 0) {
            prof_nkmods = 0;
            goto out;
        }
    }
    // index counts segments: walk the modules' until it is reached
    ret = 0;
    uint32_t seen = 0;
    for (SceSize m = 0; m < prof_nkmods; m++) {
        static SceKernelModuleInfo info;
        __builtin_memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (((ProfGetModuleInfo)get)(AZAHAR_KERNEL_PID, prof_kmods[m], &info) < 0) {
            continue;
        }
        for (uint32_t seg = 0; seg < 4; seg++) {
            if (info.segments[seg].memsz == 0 || seen++ != index) {
                continue;
            }
            AzaharProfModule mod;
            __builtin_memset(&mod, 0, sizeof(mod));
            mod.vaddr = (uint32_t)(uintptr_t)info.segments[seg].vaddr;
            mod.memsz = info.segments[seg].memsz;
            mod.perms = info.segments[seg].perms | seg << 8;
            __builtin_memcpy(mod.name, info.module_name, sizeof(info.module_name));
            mod.name[sizeof(mod.name) - 1] = 0;
            ret = ksceKernelCopyToUser(user_out, &mod, sizeof(mod));
            ret = ret < 0 ? ret : 1;
            goto out;
        }
    }
out:
    EXIT_SYSCALL(state);
    return ret;
}

int azaharProfStop(void) {
    uint32_t state;
    ENTER_SYSCALL(state);
    prof_free();
    EXIT_SYSCALL(state);
    return 0;
}
