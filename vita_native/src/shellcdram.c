// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// azahar-native: the shell's CDRAM (ScePhyMemPartShellCdram, 16 MiB at 0x27000000), the app's
// while sgxd holds the GPU. The shell's GPU work waits at SceGpu's gate meanwhile, so the range
// is copied to main memory and put under the app's addresses; sgxd copies it back after its own
// side is down and before SceGpu has the GPU again (release, a dead owner, suspend).
// The app's view: a block of its uncached main memory, its 16 level-1 entries pointed at the
// range's sections. A block of another type mirroring the range gets pages of its own instead.
// The shell's CPU keeps writing its CDRAM (PAF's text cache), so its window (0x60000000) is put
// on the copy for the session through level-2 tables of ours. The copy back and the suspend
// exchanges move its pages one at a time (shellcdram_move), carrying over writes in between. A
// write during the first copy can still be lost, and a block the shell maps meanwhile would not
// show through our tables.

#define SHELLCDRAM_PA 0x27000000u
#define SHELLCDRAM_SIZE 0x01000000u
#define SHELLCDRAM_MBS (SHELLCDRAM_SIZE >> 20)

static SceUID shellcdram_alias = -1; // the kernel's view of the range, normal non-cacheable
static SceUID shellcdram_save = -1;  // the shell's contents, in main memory
static SceUID shellcdram_user = -1;  // the app's block whose addresses show the range
static SceUID shellcdram_l1_uid = -1;
static volatile uint32_t *shellcdram_l1; // the app's level-1 entries for the block, once written
static uint32_t shellcdram_old_l1[SHELLCDRAM_MBS];
static SceUID shellcdram_pid = -1;
static uint32_t shellcdram_user_va;
#define SHELLCDRAM_SHELL_VA 0x60000000u // the shell's user CDRAM window
static SceUID shellcdram_shell_pid = -1;
static SceUID shellcdram_shell_l1_uid = -1;
static volatile uint32_t *shellcdram_shell_l1; // the shell's level-1 entries, once written
static uint32_t shellcdram_shell_old_l1[SHELLCDRAM_MBS];
static SceUID shellcdram_shell_l2_uid = -1;
static uint32_t *shellcdram_shell_l2;
static uintptr_t shellcdram_shell_l2_pa;
// While the shell's window is on our tables (shellcdram_shell_l2, its live level-2 tables): each
// of its pages' descriptor onto the range and onto the copy, after the live tables in the same
// block, and the range's page it shows (SHELLCDRAM_NO_PAGE for none), for shellcdram_move.
#define SHELLCDRAM_PAGES (SHELLCDRAM_SIZE >> 12)
#define SHELLCDRAM_SLOTS (SHELLCDRAM_MBS * 256u)
#define SHELLCDRAM_NO_PAGE 0xFFFFu
static uint32_t *shellcdram_shell_on_range, *shellcdram_shell_on_copy;
static uint16_t shellcdram_shell_page[SHELLCDRAM_SLOTS];

// A 1 MiB section of `pa` with the attributes of a level-2 descriptor `d` under the level-1
// page-table entry `table` (ARMv7 short descriptors: its domain and NS come from the table).
static uint32_t shellcdram_section(uint32_t pa, uint32_t table, uint32_t d) {
    const int large = (d & 3u) == 1u;
    const uint32_t xn = large ? (d >> 15) & 1u : d & 1u;
    const uint32_t tex = large ? (d >> 12) & 7u : (d >> 6) & 7u;
    return pa | 2u | (d & 0xCu) /* B, C */ | xn << 4 | (table & 0x1E0u) /* domain */ |
           ((d >> 4) & 3u) << 10 /* AP[1:0] */ | tex << 12 | ((d >> 9) & 1u) << 15 /* AP[2] */ |
           ((d >> 10) & 1u) << 16 /* S */ | ((d >> 11) & 1u) << 17 /* nG */ |
           ((table >> 3) & 1u) << 19 /* NS */;
}

// The shell's window back on the range: its level-1 entries as they were, in its MMU context.
static void shellcdram_shell_restore(void) {
    if (shellcdram_shell_l1) {
        SceKernelProcessContext *ctx = NULL, prev;
        if (ksceKernelProcessGetContext(shellcdram_shell_pid, &ctx) >= 0 && ctx) {
            ksceKernelProcessSwitchContext(ctx, &prev);
            for (uint32_t i = 0; i < SHELLCDRAM_MBS; i++) {
                shellcdram_shell_l1[i] = shellcdram_shell_old_l1[i];
            }
            reback_tlb_flush_all();
            ksceKernelProcessSwitchContext(&prev, NULL);
        } else {
            SGXD_LOG("shell CDRAM: the shell's context is gone: its tables not put back\n");
        }
        shellcdram_shell_l1 = NULL;
    }
    if (shellcdram_shell_l1_uid >= 0) {
        ksceKernelFreeMemBlock(shellcdram_shell_l1_uid);
        shellcdram_shell_l1_uid = -1;
    }
    if (shellcdram_shell_l2_uid >= 0) {
        ksceKernelFreeMemBlock(shellcdram_shell_l2_uid);
        shellcdram_shell_l2_uid = -1;
    }
    shellcdram_shell_l2 = NULL;
    shellcdram_shell_on_range = shellcdram_shell_on_copy = NULL;
    shellcdram_shell_pid = -1;
}

// The shell's window onto the range put on our level-2 tables, its pages on the copy `save` (a
// kernel block) with `on_copy`, else still on the range for shellcdram_move: every page of it
// resolved in the shell's MMU context first, nothing changed unless all are.
static int shellcdram_shell_redirect(const uint8_t *save, int on_copy) {
    const SceUID pid = ksceKernelSysrootGetShellPid();
    SceKernelProcessContext *ctx = NULL, prev;
    int ret = alloc_block("AzaharShellL2", SHELLCDRAM_SLOTS * 4u * 3u, 0x1000,
                          &shellcdram_shell_l2_uid, &shellcdram_shell_l2, &shellcdram_shell_l2_pa);
    if (ret) {
        return ret;
    }
    shellcdram_shell_on_range = shellcdram_shell_l2 + SHELLCDRAM_SLOTS;
    shellcdram_shell_on_copy = shellcdram_shell_l2 + 2u * SHELLCDRAM_SLOTS;
    for (uint32_t slot = 0; slot < SHELLCDRAM_SLOTS; slot++) {
        shellcdram_shell_page[slot] = SHELLCDRAM_NO_PAGE;
    }
    if (pid < 0 || ksceKernelProcessGetContext(pid, &ctx) < 0 || !ctx) {
        SGXD_LOG("shell CDRAM: no shell context (pid %08x)\n", (uint32_t)pid);
        return AZAHAR_ERR_STATE;
    }
    shellcdram_shell_pid = pid;
    // The copy's pages and their attributes as the kernel maps them: the shell's view shares
    // them, so the data cache (physically tagged) serves both and a clean by the kernel's
    // address writes the shell's lines back too.
    const uint32_t copy_attr =
        azahar_attr_from_par(translate((uint32_t)(uintptr_t)save, AT_PRIV_W));
    uint32_t moved = 0, kept = 0, read_only = 0;
    uint32_t new_l1[SHELLCDRAM_MBS];
    ksceKernelProcessSwitchContext(ctx, &prev);
    volatile uint32_t *const l1 = reback_map_l1(SHELLCDRAM_SHELL_VA, SHELLCDRAM_MBS, &shellcdram_shell_l1_uid);
    ret = l1 && copy_attr ? 0 : AZAHAR_ERR_TABLE;
    for (uint32_t mb = 0; ret == 0 && mb < SHELLCDRAM_MBS; mb++) {
        const uint32_t e = l1[mb];
        shellcdram_shell_old_l1[mb] = e;
        if ((e & 3u) != 1u && (e & 3u) != 2u) {
            for (uint32_t i = 0; i < 256; i++) {
                shellcdram_shell_l2[mb * 256u + i] = 0;
                shellcdram_shell_on_range[mb * 256u + i] = shellcdram_shell_on_copy[mb * 256u + i] = 0;
            }
            new_l1[mb] = e; // nothing mapped there
            continue;
        }
        for (uint32_t i = 0; i < 256; i++) {
            const uint32_t uva = SHELLCDRAM_SHELL_VA + (mb << 20) + (i << 12);
            uint32_t par = reback_user_par(uva);
            uint32_t perm = AZAHAR_PERM_R | AZAHAR_PERM_W;
            if (PAR_FAULTED(par)) {
                par = sgxd_user_par(uva);
                perm = AZAHAR_PERM_R;
            }
            uint32_t on_range = 0, on_copy_d = 0;
            if (!PAR_FAULTED(par)) {
                const uint32_t pa = PAR_PA(par);
                // APX: read-only, as it was.
                const uint32_t apx = perm & AZAHAR_PERM_W ? 0 : 1u << 9;
                on_range = reback_host_page(pa, azahar_attr_from_par(par)) | apx;
                on_copy_d = on_range;
                if (pa - SHELLCDRAM_PA < SHELLCDRAM_SIZE) {
                    uintptr_t copy_pa = 0;
                    ksceKernelVAtoPA(save + (pa - SHELLCDRAM_PA), &copy_pa);
                    on_copy_d = reback_host_page((uint32_t)copy_pa, copy_attr) | apx;
                    shellcdram_shell_page[mb * 256u + i] = (uint16_t)((pa - SHELLCDRAM_PA) >> 12);
                    moved++;
                } else {
                    kept++;
                }
                read_only += apx != 0;
            }
            shellcdram_shell_on_range[mb * 256u + i] = on_range;
            shellcdram_shell_on_copy[mb * 256u + i] = on_copy_d;
            shellcdram_shell_l2[mb * 256u + i] = on_copy ? on_copy_d : on_range;
        }
        const uint32_t ns = (e & 3u) == 2u ? (e >> 19) & 1u : (e >> 3) & 1u;
        new_l1[mb] = ((uint32_t)shellcdram_shell_l2_pa + mb * 0x400u) | (e & (0xFu << 5)) |
                     (ns << 3) | 1u;
    }
    if (ret == 0) {
        asm volatile("dsb" ::: "memory");
        shellcdram_shell_l1 = l1;
        for (uint32_t mb = 0; mb < SHELLCDRAM_MBS; mb++) {
            shellcdram_shell_l1[mb] = new_l1[mb];
        }
        reback_tlb_flush_all();
    }
    ksceKernelProcessSwitchContext(&prev, NULL);
    SGXD_LOG("shell CDRAM: the shell's window (pid %08x, level-1 %08x) on %s: %u pages of the "
             "range, %u others, %u read-only -> %d\n",
             (uint32_t)pid, shellcdram_shell_old_l1[0], on_copy ? "the copy" : "our tables",
             moved, kept, read_only, ret);
    return ret;
}

// The app's level-1 entries back (when they were written), then every block freed.
static void shellcdram_free(int written) {
    shellcdram_shell_restore();
    if (written && shellcdram_l1) {
        for (uint32_t i = 0; i < SHELLCDRAM_MBS; i++) {
            shellcdram_l1[i] = shellcdram_old_l1[i];
        }
        reback_tlb_flush_all();
    }
    shellcdram_l1 = NULL;
    if (shellcdram_l1_uid >= 0) {
        ksceKernelFreeMemBlock(shellcdram_l1_uid);
        shellcdram_l1_uid = -1;
    }
    if (shellcdram_user >= 0) {
        ksceKernelFreeMemBlock(shellcdram_user);
        shellcdram_user = -1;
    }
    if (shellcdram_save >= 0) {
        ksceKernelFreeMemBlock(shellcdram_save);
        shellcdram_save = -1;
    }
    if (shellcdram_alias >= 0) {
        ksceKernelFreeMemBlock(shellcdram_alias);
        shellcdram_alias = -1;
    }
    shellcdram_pid = -1;
    shellcdram_user_va = 0;
}

// The shell's contents back, the app's view gone. sgxd's mutex is held and the GPU is idle
// (sgxd_hw_down ran).
static void shellcdram_give_back(void) {
    if (shellcdram_pid < 0) {
        return;
    }
    void *alias = NULL, *save = NULL;
    ksceKernelGetMemBlockBase(shellcdram_alias, &alias);
    ksceKernelGetMemBlockBase(shellcdram_save, &save);
    // Which 64 KiB of the range differ from the save, before it goes back: the app's writes,
    // and anyone else's while it was the app's (the shell's own CPU, the kernel's drivers).
    uint32_t differ[SHELLCDRAM_SIZE / 0x10000 / 32] = {0};
    uint32_t chunks = 0;
    for (uint32_t c = 0; c < SHELLCDRAM_SIZE / 0x10000; c++) {
        const volatile uint32_t *a = (const volatile uint32_t *)((uintptr_t)alias + c * 0x10000);
        const uint32_t *b = (const uint32_t *)((uintptr_t)save + c * 0x10000);
        for (uint32_t w = 0; w < 0x10000 / 4; w++) {
            if (a[w] != b[w]) {
                differ[c / 32] |= 1u << (c % 32);
                chunks++;
                break;
            }
        }
    }
    SGXD_LOG("shell CDRAM: %u of 256 64 KiB chunks differ from the copy: %08x %08x %08x %08x "
             "%08x %08x %08x %08x\n",
             chunks, differ[0], differ[1], differ[2], differ[3], differ[4], differ[5], differ[6],
             differ[7]);
    shellcdram_move(0);
    shellcdram_shell_restore();
    SGXD_LOG("shell CDRAM given back to the shell (process %08x's view gone)\n",
             (uint32_t)shellcdram_pid);
    shellcdram_free(1);
}

// The app's level-2 descriptor for `uva`, through a mapping of its table's page; 0 if none.
static uint32_t shellcdram_l2_descriptor(uint32_t table, uint32_t uva) {
    const uint32_t pa = (table & 0xFFFFFC00u) + ((uva >> 12) & 0xFFu) * 4u;
    SceKernelAllocMemBlockKernelOpt opt = {0};
    opt.size = sizeof(opt);
    opt.attr = SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_PADDR;
    opt.paddr = pa & 0xFFFFF000u;
    const SceUID uid = ksceKernelAllocMemBlock("SceAzaharL2", 0x6020D006, 0x1000, &opt);
    void *va = NULL;
    uint32_t d = 0;
    if (uid >= 0 && ksceKernelGetMemBlockBase(uid, &va) >= 0 && va) {
        d = *(volatile uint32_t *)((uintptr_t)va + (pa & 0xFFFu));
    }
    if (uid >= 0) {
        ksceKernelFreeMemBlock(uid);
    }
    return d;
}

// The range for the calling process, which must hold the GPU: its address in the app, 16 MiB.
int azaharShellCdram(uint32_t *user_va) {
    uint32_t state;
    ENTER_SYSCALL(state);
    int ret = sgxd_enter();
    if (ret < 0) {
        goto out;
    }
    const SceUID pid = ksceKernelGetProcessId();
    if (sgxd_state != SGXD_OWNED || sgxd_owner != pid) {
        ret = SGXD_ERR_OWNER;
        goto leave;
    }
    if (shellcdram_pid < 0) {
        SceKernelAllocMemBlockKernelOpt opt = {0};
        opt.size = sizeof(opt);
        opt.attr = SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_PADDR;
        opt.paddr = SHELLCDRAM_PA;
        // The I/O partition's type maps any physical address; 0x8000 is normal non-cacheable.
        // Strongly-ordered (0x0200) made shellcdram_move take seconds; the move's barriers keep
        // the order it needs.
        shellcdram_alias =
            ksceKernelAllocMemBlock("AzaharShellCdram", 0x20108006, SHELLCDRAM_SIZE, &opt);
        // The kernel's block in the game's main memory: ScePhyMemPartKD is too small, and an app
        // block would go with a killed app before sgxd's teardown copies it back.
        shellcdram_save = ksceKernelAllocMemBlock("AzaharShellCdramSave", SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_ROOT_GAME_RW,
                                                  SHELLCDRAM_SIZE, NULL);
        // The app's addresses: a 1 MiB-aligned block of its uncached main memory.
        SceKernelAllocMemBlockKernelOpt user = {0};
        user.size = sizeof(user);
        user.attr = SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_PID | SCE_KERNEL_ALLOC_MEMBLOCK_ATTR_HAS_ALIGNMENT;
        user.pid = pid;
        user.alignment = 0x100000;
        shellcdram_user = ksceKernelAllocMemBlock("azahar-shell-cdram", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,
                                                  SHELLCDRAM_SIZE, &user);
        void *alias = NULL, *save = NULL, *base = NULL;
        if (shellcdram_alias < 0 || shellcdram_save < 0 || shellcdram_user < 0 ||
            ksceKernelGetMemBlockBase(shellcdram_alias, &alias) < 0 ||
            ksceKernelGetMemBlockBase(shellcdram_save, &save) < 0 ||
            ksceKernelGetMemBlockBase(shellcdram_user, &base) < 0 || !base ||
            ((uintptr_t)base & 0xFFFFFu)) {
            SGXD_LOG("shell CDRAM: alias %08x save %08x view %08x at %p\n",
                     (uint32_t)shellcdram_alias, (uint32_t)shellcdram_save,
                     (uint32_t)shellcdram_user, base);
            shellcdram_free(0);
            ret = AZAHAR_ERR_ALLOC;
            goto leave;
        }
        const uint32_t uva = (uint32_t)(uintptr_t)base;
        volatile uint32_t *const l1 = reback_map_l1(uva, SHELLCDRAM_MBS, &shellcdram_l1_uid);
        if (!l1) {
            shellcdram_free(0);
            ret = AZAHAR_ERR_ALLOC;
            goto leave;
        }
        // The block's attributes, from each megabyte's first descriptor: a section as it is,
        // or a level-2 page under a page-table entry.
        uint32_t sections[SHELLCDRAM_MBS];
        for (uint32_t i = 0; i < SHELLCDRAM_MBS; i++) {
            const uint32_t e = shellcdram_old_l1[i] = l1[i];
            const uint32_t pa = SHELLCDRAM_PA + (i << 20);
            uint32_t d = 0;
            if ((e & 3u) == 2u && !(e & (1u << 18))) {
                sections[i] = (e & 0xFFFFFu) | pa;
            } else if ((e & 3u) == 1u && ((d = shellcdram_l2_descriptor(e, uva + (i << 20))) & 3u)) {
                sections[i] = shellcdram_section(pa, e, d);
            } else {
                SGXD_LOG("shell CDRAM: the view's level-1 entry %u is %08x (level 2 %08x): no "
                         "section for it\n",
                         i, e, d);
                shellcdram_free(0);
                ret = AZAHAR_ERR_STATE;
                goto leave;
            }
        }
        memcpy(save, alias, SHELLCDRAM_SIZE);
        ksceKernelDcacheCleanRange(save, SHELLCDRAM_SIZE);
        asm volatile("dsb" ::: "memory");
        // The shell onto the copy before the app writes the range.
        ret = shellcdram_shell_redirect(save, 1);
        if (ret) {
            shellcdram_free(0);
            goto leave;
        }
        shellcdram_l1 = l1;
        for (uint32_t i = 0; i < SHELLCDRAM_MBS; i++) {
            shellcdram_l1[i] = sections[i];
        }
        reback_tlb_flush_all();
        // The view must be the shell's pages.
        uint32_t first = 0, last = 0, par = 0;
        int got = sgxd_user_page(uva, 1, &first, &par);
        if (got >= 0) {
            got = sgxd_user_page(uva + SHELLCDRAM_SIZE - 0x1000, 1, &last, &par);
        }
        SGXD_LOG("shell CDRAM: saved to %p; the app's view at %08x (level-1 %08x -> %08x), "
                 "physical %08x..%08x\n",
                 save, uva, shellcdram_old_l1[0], sections[0], first, last);
        if (got < 0 || first != SHELLCDRAM_PA || last != SHELLCDRAM_PA + SHELLCDRAM_SIZE - 0x1000) {
            // Nothing was written into the range: the save copy is the shell's as it is.
            shellcdram_free(1);
            ret = AZAHAR_ERR_STATE;
            goto leave;
        }
        shellcdram_pid = pid;
        shellcdram_user_va = uva;
    }
    if (user_va) {
        ksceKernelCopyToUser(user_va, &shellcdram_user_va, sizeof(shellcdram_user_va));
    }
leave:
    sgxd_leave();
out:
    EXIT_SYSCALL(state);
    return ret;
}
