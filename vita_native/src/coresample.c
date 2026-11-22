// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// azaharCoreSample: what a core runs, for the app's stall report, read from outside: its
// CoreSight DBGPCSR (no unlock, halts nothing) and SceKernelThreadMgr's per-CPU block.

#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/threadmgr.h>

// The four debug blocks, 0x2000 apart. DBGPCSR: a recent PC of the core (ARM: +8, Thumb: +4 with
// bit 0 set), 0xFFFFFFFF when the core cannot be sampled.
#define CORESAMPLE_DBG_PA 0xE3310000u
#define CORESAMPLE_DBG_SIZE 0x8000u
#define CORESAMPLE_DBGPCSR 0x084u

// SceKernelThreadMgr's per-CPU blocks (firmware 3.65; 3.60, 3.68 and 3.73 alike): its data segment's first word points at
// cpuCB[0], the four 0x880 bytes apart; +0x00 the running thread's object, +0x14 its pid, +0x18
// its thread, +0x34 the core's idle thread.
#define CORESAMPLE_CPUCB_STRIDE 0x880u
#define CORESAMPLE_CPUCB_THREAD 0x00u
#define CORESAMPLE_CPUCB_PID 0x14u
#define CORESAMPLE_CPUCB_THID 0x18u
#define CORESAMPLE_CPUCB_IDLE 0x34u

static SceUID coresample_dbg_uid = -1;
static volatile const uint32_t *coresample_dbg;
static volatile const uint8_t *coresample_cpucb;

static volatile const uint8_t *coresample_find_cpucb(void) {
    tai_module_info_t tai = {.size = sizeof(tai)};
    uintptr_t data = 0;
    if (taiGetModuleInfoForKernel(AZAHAR_KERNEL_PID, "SceKernelThreadMgr", &tai) < 0 ||
        module_get_offset(AZAHAR_KERNEL_PID, tai.modid, 1, 0, &data) < 0 || !data) {
        return NULL;
    }
    const uint8_t *const priv = *(const uint8_t *const *)data;
    if (!priv) {
        return NULL;
    }
    // Four idle threads, each its own and in kernel memory, or it is not the block.
    uint32_t idle[4];
    for (uint32_t cpu = 0; cpu < 4; cpu++) {
        idle[cpu] = *(const uint32_t *)(priv + cpu * CORESAMPLE_CPUCB_STRIDE + CORESAMPLE_CPUCB_IDLE);
        if (idle[cpu] < 0x00100000u || idle[cpu] >= 0x40000000u) {
            return NULL;
        }
        for (uint32_t k = 0; k < cpu; k++) {
            if (idle[k] == idle[cpu]) {
                return NULL;
            }
        }
    }
    return priv;
}

// The kernel modules a stopped core's PC was looked for in: the idle loops' (SceKernelThreadMgr's
// basic one, SceLowio's low-power one) and what an interrupt or a power state runs.
static const char *const coresample_modules[] = {
    "SceSysmem",      "SceKernelThreadMgr", "SceKernelIntrMgr", "SceExcpmgr",  "SceLowio",
    "SceSystimer",    "SceKernelDmacMgr",   "SceSyscon",        "ScePower",    "SceDisplay",
    "SceGpuEs4",      "SceProcessmgr",      "SceModulemgr",     "SceIofilemgr", "SceDeci4pDfMgr",
    "SceDeci4pSDfMgr", "SceDeci4pSDfCtl",   "SceDeci4pDbgp",    "SceSdbgSdio", "SceKernelBusError",
    "SceCoredump",    "azaharnative",       "SceDebugLed",      "SceSblSsMgr", "SceSblACMgr",
};

// The listed module whose text starts closest below `pc`: its name and text base.
static void coresample_module(uint32_t pc, char *name, uint32_t name_size, uint32_t *text_out) {
    uint32_t best = 0;
    for (uint32_t i = 0; i < sizeof(coresample_modules) / sizeof(coresample_modules[0]); i++) {
        tai_module_info_t tai = {.size = sizeof(tai)};
        uintptr_t text = 0;
        if (taiGetModuleInfoForKernel(AZAHAR_KERNEL_PID, coresample_modules[i], &tai) < 0 ||
            module_get_offset(AZAHAR_KERNEL_PID, tai.modid, 0, 0, &text) < 0 || !text) {
            continue;
        }
        if (text <= pc && text > best) {
            best = (uint32_t)text;
            strncpy(name, coresample_modules[i], name_size - 1);
            name[name_size - 1] = '\0';
        }
    }
    *text_out = best;
}

// The debug blocks mapped and the per-CPU blocks found, once: 0 or the mapping's error.
static int coresample_map(void) {
    if (coresample_dbg_uid < 0) {
        SceKernelAllocMemBlockKernelOpt opt;
        memset(&opt, 0, sizeof(opt));
        opt.size = sizeof(opt);
        opt.attr = 2; // at the physical address given
        opt.paddr = CORESAMPLE_DBG_PA;
        void *base = NULL;
        const SceUID uid =
            ksceKernelAllocMemBlock("azahar_coresample_dbg", 0x20100206, CORESAMPLE_DBG_SIZE, &opt);
        if (uid < 0 || ksceKernelGetMemBlockBase(uid, &base) < 0) {
            return uid < 0 ? uid : AZAHAR_ERR_STATE;
        }
        coresample_dbg_uid = uid;
        coresample_dbg = (volatile const uint32_t *)base;
    }
    if (!coresample_cpucb) {
        coresample_cpucb = coresample_find_cpucb();
    }
    return 0;
}

int azaharCoreSample(uint32_t core, AzaharCoreSample *user_out) {
    uint32_t state;
    ENTER_SYSCALL(state);
    int ret = 0;
    AzaharCoreSample s;
    memset(&s, 0, sizeof(s));
    if (core > 3 || !user_out) {
        ret = AZAHAR_ERR_ARG;
        goto out;
    }
    if ((ret = coresample_map()) != 0) {
        goto out;
    }
    for (uint32_t i = 0; i < AZAHAR_CORESAMPLE_PCS; i++) {
        s.pcs[i] = coresample_dbg[(core * 0x2000u + CORESAMPLE_DBGPCSR) / 4];
    }
    if (coresample_cpucb) {
        const volatile uint8_t *const cb = coresample_cpucb + core * CORESAMPLE_CPUCB_STRIDE;
        const uint32_t thread = *(volatile const uint32_t *)(cb + CORESAMPLE_CPUCB_THREAD);
        s.pid = *(volatile const uint32_t *)(cb + CORESAMPLE_CPUCB_PID);
        s.thid = *(volatile const uint32_t *)(cb + CORESAMPLE_CPUCB_THID);
        s.idle = thread == *(volatile const uint32_t *)(cb + CORESAMPLE_CPUCB_IDLE);
        SceKernelThreadInfo info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (ksceKernelGetThreadInfo((SceUID)s.thid, &info) >= 0) {
            memcpy(s.thread_name, info.name, sizeof(s.thread_name) - 1);
        }
    } else {
        s.pid = s.thid = 0xFFFFFFFFu;
    }
    coresample_module(s.pcs[0] & ~1u, s.module_name, sizeof(s.module_name), &s.module_text);
    if (ksceKernelCopyToUser(user_out, &s, sizeof(s)) < 0) {
        ret = AZAHAR_ERR_ARG;
    }
out:
    EXIT_SYSCALL(state);
    return ret;
}

// SceKernelBusError takes interrupt 32 (non-secure bus error) on any core: SMC 0x10D dumps the
// bus's error registers to the shared memory's +0x6000, then the core is parked for good (`b .`
// at text +0xe8), interrupts masked. SceBusErrorForKernel_EBB1F135(dst, size) copies the dump
// out: the first two words' sum of bytes, at most `size`.
#define BUSERROR_LIBRARY 0x193156B1u
#define BUSERROR_COPY 0xEBB1F135u
#define BUSERROR_MAX 0x3000u

static uint8_t buserror_dump[BUSERROR_MAX];

int azaharBusError(void *user_out, uint32_t size) {
    uint32_t state;
    ENTER_SYSCALL(state);
    int ret;
    uintptr_t fn = 0;
    if (!user_out) {
        ret = AZAHAR_ERR_ARG;
    } else if (module_get_export_func(AZAHAR_KERNEL_PID, "SceKernelBusError", BUSERROR_LIBRARY,
                                      BUSERROR_COPY, &fn) < 0 || !fn) {
        ret = AZAHAR_ERR_RESOLVE;
    } else {
        uint32_t (*const copy)(void *, uint32_t) = (uint32_t (*)(void *, uint32_t))fn;
        const uint32_t got = copy(buserror_dump, size < BUSERROR_MAX ? size : BUSERROR_MAX);
        ret = ksceKernelCopyToUser(user_out, buserror_dump, got) < 0 ? AZAHAR_ERR_ARG : (int)got;
    }
    EXIT_SYSCALL(state);
    return ret;
}

// azaharTableScan: the guest's translation tables at a stall. The unsealed guest's table holds
// a copy of the process's level-1 entries from 0x40000000 up (azaharMap, azaharSyncUpper); a
// megabyte the process remaps later keeps its old entry there, and a coarse one may name a
// level-2 table the kernel freed and reused. Read-only.

// A descriptor's output address (a level-1 entry's table or section, a level-2 entry's page),
// or 0 for a fault entry.
static uint32_t tablescan_out(uint32_t d, int level1) {
    if (level1) {
        switch (d & 3u) {
            case 1: return d & 0xFFFFFC00u;
            case 2: return (d & (1u << 18)) ? (d & 0xFF000000u) : (d & 0xFFF00000u);
            default: return 0;
        }
    }
    if ((d & 3u) == 1u) {
        return d & 0xFFFF0000u;
    }
    return (d & 2u) ? (d & 0xFFFFF000u) : 0;
}

// Main memory ends at 0x80000000 (CDRAM at 0x20000000, the GIC's page at 0x1A000000).
static int tablescan_wild(uint32_t out) {
    return out >= 0x80000000u;
}

static void tablescan_ours(AzaharTableScan *s, const uint32_t *t, uint32_t n, uint32_t pa,
                           int level1) {
    for (uint32_t i = 0; i < n; i++) {
        if (tablescan_wild(tablescan_out(t[i], level1))) {
            if (s->ours_wild < AZAHAR_TABLESCAN_N) {
                s->ours_wild_at[s->ours_wild] = pa + i * 4u;
                s->ours_wild_desc[s->ours_wild] = t[i];
            }
            s->ours_wild++;
        }
    }
}

// A coarse entry's level-2 table, read through a mapping of its page: how many descriptors name
// something outside main memory (0xFFFFFFFF: not mapped), and the first.
static uint32_t tablescan_l2(uint32_t coarse, uint32_t *first) {
    SceKernelAllocMemBlockKernelOpt opt;
    memset(&opt, 0, sizeof(opt));
    opt.size = sizeof(opt);
    opt.attr = 2;
    opt.paddr = coarse & 0xFFFFF000u;
    void *va = NULL;
    uint32_t wild = 0;
    const SceUID uid = ksceKernelAllocMemBlock("SceAzaharScan", 0x6020D006, 0x1000, &opt);
    if (uid < 0 || ksceKernelGetMemBlockBase(uid, &va) < 0 || !va) {
        wild = 0xFFFFFFFFu;
    } else {
        const volatile uint32_t *const l2 =
            (const volatile uint32_t *)((uintptr_t)va + (coarse & 0xC00u));
        for (uint32_t j = 0; j < 256u; j++) {
            const uint32_t d = l2[j];
            if (tablescan_wild(tablescan_out(d, 0))) {
                if (!wild) {
                    *first = d;
                }
                wild++;
            }
        }
    }
    if (uid >= 0) {
        ksceKernelFreeMemBlock(uid);
    }
    return wild;
}

int azaharTableScan(AzaharTableScan *user_out) {
    uint32_t state;
    ENTER_SYSCALL(state);
    int ret = 0;
    AzaharTableScan s;
    memset(&s, 0, sizeof(s));
    if (!user_out) {
        ret = AZAHAR_ERR_ARG;
        goto out;
    }
    if (azahar_state != ST_MAPPED) {
        ret = AZAHAR_ERR_STATE;
        goto out;
    }
    // The copy against the process's own, and a stale coarse entry's
    // table read through a mapping of its page.
    for (uint32_t pg = 1; pg < 4; pg++) {
        SceUID uid = -1;
        const volatile uint32_t *const t = reback_map_l1(pg << 30, 1024u, &uid);
        if (!t) {
            ret = AZAHAR_ERR_TABLE;
            goto out;
        }
        for (uint32_t i = 0; i < 1024u; i++) {
            const uint32_t idx = pg * 1024u + i;
            const uint32_t e = t[i], g = azahar_l1[idx];
            if (g == e) {
                continue;
            }
            if ((g & 3u) == 0) {
                s.missing++;
                continue;
            }
            const uint32_t k = s.stale++;
            if (k >= AZAHAR_TABLESCAN_N) {
                continue;
            }
            s.stale_va[k] = idx << 20;
            s.stale_guest[k] = g;
            s.stale_process[k] = e;
            if ((g & 3u) == 1u && !tablescan_wild(g & 0xFFFFFC00u)) {
                s.stale_wild[k] = tablescan_l2(g, &s.stale_wild_desc[k]);
            }
        }
        ksceKernelFreeMemBlock(uid);
    }
    // Page 0xFFFFF000, where bus errors' reads went (small offsets below zero): what the guest's
    // world maps there.
    s.top_l1 = azahar_l1[0xFFFu];
    {
        SceUID uid = -1;
        const volatile uint32_t *const t = reback_map_l1(0xFFF00000u, 1u, &uid);
        if (t) {
            s.top_process = *t;
            ksceKernelFreeMemBlock(uid);
        }
    }
    if ((s.top_l1 & 3u) == 1u && !tablescan_wild(s.top_l1 & 0xFFFFFC00u)) {
        SceKernelAllocMemBlockKernelOpt opt;
        memset(&opt, 0, sizeof(opt));
        opt.size = sizeof(opt);
        opt.attr = 2;
        opt.paddr = s.top_l1 & 0xFFFFF000u;
        void *va = NULL;
        const SceUID l2_uid =
            ksceKernelAllocMemBlock("SceAzaharScan", 0x6020D006, 0x1000, &opt);
        if (l2_uid >= 0 && ksceKernelGetMemBlockBase(l2_uid, &va) >= 0 && va) {
            s.top_l2 = ((const volatile uint32_t *)((uintptr_t)va + (s.top_l1 & 0xC00u)))[0xFFu];
        }
        if (l2_uid >= 0) {
            ksceKernelFreeMemBlock(l2_uid);
        }
    }
    // The lower gigabyte's copy of the kernel's own entries (azaharMap's, wherever the guest's
    // tables and the vector page are not) against the kernel's table now: a megabyte the kernel
    // unmapped since keeps its old entry here, and a coarse one names a table the kernel may
    // have freed and reused.
    {
        const uint32_t *const kl1 = find_kernel_page(rd_ttbr0() & 0xFFFFF000u);
        const uint32_t pool_lo = (uint32_t)azahar_l2_pa;
        const uint32_t pool_hi = pool_lo + AZAHAR_L2_COUNT * 0x400u;
        if (!kl1) {
            s.kstale = 0xFFFFFFFFu;
        }
        for (uint32_t i = 0; kl1 && i < 1024u; i++) {
            const uint32_t g = azahar_l1[i];
            const uint32_t base = g & 0xFFFFFC00u;
            // Ours, the same, or a fault (a megabyte mapped since: it faults, it walks nothing).
            if (i == (azahar_vec_va >> 20) ||
                ((g & 3u) == 1u && base >= pool_lo && base < pool_hi) || g == kl1[i] ||
                (g & 3u) == 0) {
                continue;
            }
            const uint32_t k = s.kstale++;
            if (k >= AZAHAR_TABLESCAN_N) {
                continue;
            }
            s.kstale_va[k] = i << 20;
            s.kstale_guest[k] = g;
            s.kstale_kernel[k] = kl1[i];
            if ((g & 3u) == 1u && !tablescan_wild(base)) {
                s.kstale_wild[k] = tablescan_l2(g, &s.kstale_wild_desc[k]);
            }
        }
    }
    // By domain. A supersection has no domain field (domain 0).
    s.dacr = rd_dacr();
    for (uint32_t i = 0; i < AZAHAR_L1_ENTRIES; i++) {
        const uint32_t g = azahar_l1[i];
        if ((g & 3u) == 0) {
            continue;
        }
        const uint32_t dom = ((g & 3u) == 2u && (g & (1u << 18))) ? 0u : (g >> 5) & 0xFu;
        if (s.dom_valid[dom]++ == 0) {
            s.dom_first_va[dom] = i << 20;
            s.dom_first_desc[dom] = g;
        }
        if (((s.dacr >> (2 * dom)) & 3u) != 0) {
            continue;
        }
        uint32_t first = 0, wild = 0;
        if ((g & 3u) == 1u) {
            const uint32_t base = g & 0xFFFFFC00u;
            if (tablescan_wild(base)) {
                wild = 1;
                first = g;
            } else if ((wild = tablescan_l2(g, &first)) == 0xFFFFFFFFu) {
                wild = 0;
            }
        } else if (tablescan_wild(tablescan_out(g, 1))) {
            wild = 1;
            first = g;
        }
        if (wild) {
            if (!s.dom_wild[dom]) {
                s.dom_wild_desc[dom] = first;
            }
            s.dom_wild[dom] += wild;
        }
    }
    // Our own: the guest's lower gigabyte, the L2 pool, reback's tables.
    tablescan_ours(&s, azahar_l1, 1024u, (uint32_t)azahar_l1_pa, 1);
    tablescan_ours(&s, azahar_l2, azahar_l2_used * 256u, (uint32_t)azahar_l2_pa, 0);
    if (reback_l2 && reback_owner >= 0) {
        tablescan_ours(&s, reback_l2, reback_mbs * 256u, (uint32_t)reback_l2_pa, 0);
    }
    if (ksceKernelCopyToUser(user_out, &s, sizeof(s)) < 0) {
        ret = AZAHAR_ERR_ARG;
    }
out:
    EXIT_SYSCALL(state);
    return ret;
}

// azaharBusErrorWatch: a hook on SceKernelBusError's handler (text +0x94, checked against its
// first two instructions) samples every core's PC and thread before the handler parks its
// core: the dump names the address, not the code. Interrupt context: register and memory
// reads only.
#define BUSWATCH_HANDLER 0x94u

static tai_hook_ref_t buswatch_ref;
static SceUID buswatch_uid = -1;
static AzaharBusErrorCatch buswatch_catch;

static uint32_t buswatch_cpu(void) {
    uint32_t mpidr;
    asm volatile("mrc p15, 0, %0, c0, c0, 5" : "=r"(mpidr));
    return mpidr & 3u;
}

static int buswatch_hook(int code, void *arg) {
    AzaharBusErrorCatch *const c = &buswatch_catch;
    if (c->count++ == 0) {
        c->core = buswatch_cpu();
        if (coresample_dbg) {
            for (uint32_t k = 0; k < AZAHAR_BUSWATCH_PCS; k++) {
                for (uint32_t core = 0; core < 4; core++) {
                    c->pcs[core][k] = coresample_dbg[(core * 0x2000u + CORESAMPLE_DBGPCSR) / 4];
                }
            }
        }
        if (coresample_cpucb) {
            for (uint32_t core = 0; core < 4; core++) {
                const volatile uint8_t *const cb =
                    coresample_cpucb + core * CORESAMPLE_CPUCB_STRIDE;
                c->pid[core] = *(volatile const uint32_t *)(cb + CORESAMPLE_CPUCB_PID);
                c->thid[core] = *(volatile const uint32_t *)(cb + CORESAMPLE_CPUCB_THID);
            }
        }
        asm volatile("dsb" ::: "memory");
    }
    // TAI_CONTINUE's walk, through a typed pointer (the macro's `int (*)()` takes no arguments
    // in this C standard).
    typedef int (*buswatch_fn)(int, void *);
    const struct _tai_hook_user *const cur = (const struct _tai_hook_user *)buswatch_ref;
    const struct _tai_hook_user *const next = (const struct _tai_hook_user *)cur->next;
    return next ? ((buswatch_fn)next->func)(code, arg) : ((buswatch_fn)cur->old)(code, arg);
}

// At module_stop: the hook goes before the code it runs.
static void buswatch_release(void) {
    if (buswatch_uid >= 0) {
        taiHookReleaseForKernel(buswatch_uid, buswatch_ref);
        buswatch_uid = -1;
    }
}

int azaharBusErrorWatch(AzaharBusErrorCatch *user_out) {
    uint32_t state;
    ENTER_SYSCALL(state);
    int ret = 0;
    if (buswatch_uid < 0) {
        tai_module_info_t tai = {.size = sizeof(tai)};
        uintptr_t text = 0;
        if ((ret = coresample_map()) != 0) {
            goto out;
        }
        if (taiGetModuleInfoForKernel(AZAHAR_KERNEL_PID, "SceKernelBusError", &tai) < 0 ||
            module_get_offset(AZAHAR_KERNEL_PID, tai.modid, 0, 0, &text) < 0 || !text) {
            ret = AZAHAR_ERR_RESOLVE;
            goto out;
        }
        // push {r3-r7, lr}; movs r0, #0 (3.65's buserror.skprx).
        const volatile uint16_t *const h =
            (const volatile uint16_t *)((text & ~1u) + BUSWATCH_HANDLER);
        if (h[0] != 0xB5F8u || h[1] != 0x2000u) {
            ret = AZAHAR_ERR_STATE;
            goto out;
        }
        const SceUID uid = taiHookFunctionOffsetForKernel(AZAHAR_KERNEL_PID, &buswatch_ref,
                                                          tai.modid, 0, BUSWATCH_HANDLER, 1,
                                                          buswatch_hook);
        if (uid < 0) {
            ret = uid;
            goto out;
        }
        buswatch_uid = uid;
    }
    if (user_out) {
        AzaharBusErrorCatch c = buswatch_catch;
        for (uint32_t core = 0; core < 4; core++) {
            coresample_module(c.pcs[core][0] & ~1u, c.module_name[core],
                              sizeof(c.module_name[core]), &c.module_text[core]);
        }
        if (ksceKernelCopyToUser(user_out, &c, sizeof(c)) < 0) {
            ret = AZAHAR_ERR_ARG;
        }
    }
out:
    EXIT_SYSCALL(state);
    return ret;
}
