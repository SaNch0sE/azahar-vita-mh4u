// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// azahar-native reback.c: the app's guest FCRAM under translation tables of ours, and pages
// of it moved onto other physical pages (CDRAM under a GPU-written range: no cache maintenance).
// azaharAdopt takes a block's megabytes over once: L2 tables of ours with Sony's pages and
// attributes, the process's L1 entries pointed at them, put back at azaharRebackRestore or the
// process's end. A move rewrites our descriptors and the guest's (native.c's azahar_windows);
// the app has the GPU store its new PTEs in queue order (sgxdPte), so no move waits for idle.

#define REBACK_MAX_MB 256u            // adopted megabytes
#define REBACK_MAX_PAGES 8192u        // 32 MiB moved in all (the app's pool grows: version 64)
#define REBACK_BATCH 256u             // pages per move

static uint32_t reback_base;          // the adopted range: its first address and megabytes
static uint32_t reback_mbs;
static uint32_t reback_old_l1[REBACK_MAX_MB];
static uint32_t reback_new_l1[REBACK_MAX_MB];
static volatile uint32_t *reback_l1;  // the process's level-1 entry for reback_base, mapped
static SceUID reback_l1_uid = -1;
static SceUID reback_l2_uid = -1;
static uint32_t *reback_l2;           // reback_mbs level-2 tables of 256 descriptors
static uintptr_t reback_l2_pa;
static SceUID reback_owner = -1;
static SceUID reback_mutex = -1;
static uint8_t reback_copy_buf[0x1000] __attribute__((aligned(64)));

// A moved page by its new physical address, for the guest descriptors made later (map_range),
// and the page it came from, for azaharRebackReturn.
typedef struct RebackMoved {
    uint32_t new_pa;
    uint32_t attr;      // AZAHAR_ATTR_MASK bits of the new page's mapping
    uint32_t old_pa;
    uint32_t old_attr;  // the app's page's, as azaharAdopt found them
} RebackMoved;
// REBACK_MAX_PAGES of them, sorted by new_pa: a kernel block made at the first azaharAdopt and
// kept (128 KiB, not .bss).
static RebackMoved *reback_moved;
static SceUID reback_moved_uid = -1;
static unsigned reback_moved_count;

// A flipped page (azaharFlip): the app's address, its home (the page the GPU's mapping keeps,
// with the attributes the app's view had on it) and the version it shows now.
#define REBACK_MAX_FLIPPED 1024u
typedef struct RebackFlipped {
    uint32_t uva;
    uint32_t home_pa;
    uint32_t home_attr;
    uint32_t version_pa;
    uint32_t version_attr;
} RebackFlipped;
static RebackFlipped reback_flipped[REBACK_MAX_FLIPPED];
static unsigned reback_flipped_count;

static unsigned reback_flipped_find(uint32_t uva) {
    for (unsigned i = 0; i < reback_flipped_count; i++) {
        if (reback_flipped[i].uva == uva) {
            return i;
        }
    }
    return reback_flipped_count;
}

typedef struct RebackPair {
    uint32_t uva;
    uint32_t old_pa;
    uint32_t new_pa;
    uint32_t attr;
} RebackPair;
static RebackPair reback_pairs[REBACK_BATCH];
static unsigned reback_pair_count;

static int reback_lock(void) {
    if (reback_mutex < 0) {
        reback_mutex = ksceKernelCreateMutex("SceAzaharReback", 0, 0, NULL);
        if (reback_mutex < 0) {
            return AZAHAR_ERR_ALLOC;
        }
    }
    ksceKernelLockMutex(reback_mutex, 1, NULL);
    return 0;
}

static void reback_unlock(void) {
    ksceKernelUnlockMutex(reback_mutex, 1);
}

static void reback_tlb_flush_all(void) {
    asm volatile("dsb \n mcr p15, 0, %0, c8, c3, 0 \n dsb \n isb" ::"r"(0) : "memory"); // TLBIALLIS
}

static unsigned reback_moved_find(uint32_t pa) {
    unsigned lo = 0, hi = reback_moved_count;
    while (lo < hi) {
        const unsigned mid = (lo + hi) / 2;
        if (reback_moved[mid].new_pa < pa) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo < reback_moved_count && reback_moved[lo].new_pa == pa ? lo : reback_moved_count;
}

// The new page's attributes, if `pa` is a page this module moved something onto, or a version a
// flipped page shows; 0 otherwise. map_range asks, so that a guest window made after the move
// maps it as the app does.
static uint32_t reback_attr_for(uint32_t pa) {
    const unsigned i = reback_moved_find(pa);
    if (i < reback_moved_count) {
        return reback_moved[i].attr;
    }
    for (unsigned f = 0; f < reback_flipped_count; f++) {
        if (reback_flipped[f].version_pa == pa) {
            return reback_flipped[f].version_attr;
        }
    }
    return 0;
}

// The process's level-1 entry for `uva`, through a mapping of its table's page of our own:
// Normal, write-back, shareable, as the kernel's own view (coherent with the walker). One page
// of entries covers 1 GiB. The mapping's block goes to *uid (freed by the caller).
static volatile uint32_t *reback_map_l1(uint32_t uva, uint32_t mbs, SceUID *uid_out) {
    const uint32_t n = rd_ttbcr() & 7u;
    uint32_t pa;
    if (n != 0 && uva >= (1u << (32 - n))) {
        pa = (rd_ttbr1() & 0xFFFFC000u) + (uva >> 20) * 4u;
    } else {
        const uint32_t mask = ~((1u << (14 - n)) - 1u);
        pa = (rd_ttbr0() & mask) + ((uva >> 20) & ((1u << (12 - n)) - 1u)) * 4u;
    }
    if ((pa & 0xFFFu) + mbs * 4u > 0x1000u) {
        emit("  adopt: the level-1 entries at pa %08x cross a page\n", pa);
        return NULL;
    }
    SceKernelAllocMemBlockKernelOpt opt;
    __builtin_memset(&opt, 0, sizeof(opt));
    opt.size = sizeof(opt);
    opt.attr = 2;   // physical address given
    opt.paddr = pa & 0xFFFFF000u;
    const SceUID uid = ksceKernelAllocMemBlock("SceAzaharL1", 0x6020D006, 0x1000, &opt);
    void *va = NULL;
    if (uid < 0 || ksceKernelGetMemBlockBase(uid, &va) < 0 || !va) {
        emit("  adopt: level-1 page pa %08x not mapped (0x%08x)\n", pa & 0xFFFFF000u, (uint32_t)uid);
        if (uid >= 0) {
            ksceKernelFreeMemBlock(uid);
        }
        return NULL;
    }
    const uint32_t par = translate((uint32_t)(uintptr_t)va, AT_PRIV_W);
    const uint32_t inner = (par >> 4) & 7u, outer = (par >> 2) & 3u, shared = (par >> 7) & 1u;
    if (PAR_FAULTED(par) || PAR_PA(par) != (pa & 0xFFFFF000u) || (inner != 5u && inner != 7u) ||
        (outer != 1u && outer != 3u) || !shared) {
        emit("  adopt: level-1 page pa %08x mapped at %08x with par %08x: not a write-back "
             "shareable view, refused\n",
             pa & 0xFFFFF000u, (uint32_t)(uintptr_t)va, par);
        ksceKernelFreeMemBlock(uid);
        return NULL;
    }
    *uid_out = uid;
    return (volatile uint32_t *)((uintptr_t)va + (pa & 0xFFFu));
}

// A page of the app's, as the app reaches it: ATS1CUR under the domain grant (the app's
// pages are No-access to the kernel inside a syscall; see azahar_user_page).
static uint32_t reback_user_par(uint32_t uva) {
    SceKernelIntrStatus st = ksceKernelCpuSuspendIntr();
    const uint32_t d = rd_dacr();
    wr_dacr(dacr_grant_all(d));
    asm volatile("mcr p15, 0, %0, c7, c8, 3 \n isb" ::"r"(uva) : "memory"); // ATS1CUW
    const uint32_t par = rd_par();
    // Its 64 KiB base (par_large_page).
    asm volatile("mcr p15, 0, %0, c7, c8, 3 \n isb" ::"r"(uva & ~0xFFFFu) : "memory");
    const uint32_t base_par = rd_par();
    wr_dacr(d);
    ksceKernelCpuResumeIntr(st);
    return par_large_page(uva, par, base_par);
}

// Our descriptor for an app page: user read/write, never executed (the guest runs through its
// own windows), not global (it is this process's), with the attributes given.
static uint32_t reback_host_page(uint32_t pa, uint32_t attr) {
    return azahar_small_page(pa, AZAHAR_PERM_R | AZAHAR_PERM_W, attr) | (1u << 11) /* nG */;
}

static void reback_restore_locked(void) {
    unsigned restored = 0, skipped = 0;
    if (reback_l1) {
        for (uint32_t i = 0; i < reback_mbs; i++) {
            // Put back only what is still ours: an entry the kernel has rewritten since (the
            // block freed) is left to it.
            if (reback_l1[i] == reback_new_l1[i]) {
                reback_l1[i] = reback_old_l1[i];
                restored++;
            } else {
                skipped++;
            }
        }
        reback_tlb_flush_all();
        ksceKernelPrintf("[azaharnative] reback: %u level-1 entr%s restored, %u left to the "
                         "kernel; %u page(s) had moved\n",
                         restored, restored == 1 ? "y" : "ies", skipped, reback_moved_count);
    }
    if (reback_l1_uid >= 0) {
        ksceKernelFreeMemBlock(reback_l1_uid);
        reback_l1_uid = -1;
    }
    if (reback_l2_uid >= 0) {
        ksceKernelFreeMemBlock(reback_l2_uid);
        reback_l2_uid = -1;
    }
    reback_l1 = NULL;
    reback_l2 = NULL;
    reback_base = 0;
    reback_mbs = 0;
    reback_moved_count = 0;
    reback_flipped_count = 0;
    reback_owner = -1;
}

// Every guest descriptor (our L2 pool) naming an old page of this move names its new one, with
// its attributes; permissions stay. The descriptors are the ones the guest's windows put the
// page's app address under (native.c's azahar_windows), a guest address can alias it more than
// once; each must still name the old page.
static unsigned reback_retarget_guest(int code) {
    if (azahar_state != ST_MAPPED) {
        return 0;
    }
    unsigned changed = 0;
    for (unsigned n = 0; n < reback_pair_count; n++) {
        const RebackPair *p = &reback_pairs[n];
        for (unsigned w = 0; w < azahar_windows.count; w++) {
            const AzaharWindow *win = &azahar_windows.w[w];
            if (p->uva - win->uva >= win->size) {
                continue;
            }
            const uint32_t gva = win->va + (p->uva - win->uva);
            const int t = l2_for(gva >> 20, 0);
            if (t < 0) {
                continue;
            }
            uint32_t *const d = &azahar_l2[(unsigned)t * 256u + ((gva >> 12) & 0xFFu)];
            if ((*d & 2u) && (*d & 0xFFFFF000u) == p->old_pa) {
                *d = p->new_pa | (*d & 0xFFFu & ~AZAHAR_ATTR_MASK) | p->attr;
                changed++;
            }
        }
    }
    if (changed) {
        // A moved page can hold code.
        tables_changed(code);
    }
    return changed;
}

static void reback_moved_insert(uint32_t new_pa, uint32_t attr, uint32_t old_pa,
                                uint32_t old_attr) {
    unsigned at = reback_moved_count;
    while (at > 0 && reback_moved[at - 1].new_pa > new_pa) {
        reback_moved[at] = reback_moved[at - 1];
        at--;
    }
    reback_moved[at].new_pa = new_pa;
    reback_moved[at].attr = attr;
    reback_moved[at].old_pa = old_pa;
    reback_moved[at].old_attr = old_attr;
    reback_moved_count++;
}

static void reback_moved_remove(unsigned at) {
    for (; at + 1 < reback_moved_count; at++) {
        reback_moved[at] = reback_moved[at + 1];
    }
    reback_moved_count--;
}

int azaharAdopt(uint32_t user_va, uint32_t size) {
    uint32_t state;
    ENTER_SYSCALL(state);
    const uint32_t mbs = size >> 20;
    if (((user_va | size) & 0xFFFFFu) || !mbs || mbs > REBACK_MAX_MB ||
        user_va + (size - 1) < user_va) {
        EXIT_SYSCALL(state);
        return AZAHAR_ERR_ARG;
    }
    int ret = reback_lock();
    if (ret) {
        EXIT_SYSCALL(state);
        return ret;
    }
    log_open();
    emit("== azaharAdopt user %08x size %08x\n", user_va, size);
    if (reback_owner >= 0) {
        ret = AZAHAR_ERR_STATE;
        goto out;
    }
    if (reback_moved_uid < 0) {
        reback_moved_uid =
            ksceKernelAllocMemBlock("AzaharRebackMoved", SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RW,
                                    (REBACK_MAX_PAGES * sizeof(RebackMoved) + 0xFFFu) & ~0xFFFu,
                                    NULL);
        if (reback_moved_uid < 0 ||
            ksceKernelGetMemBlockBase(reback_moved_uid, (void **)&reback_moved) < 0) {
            emit("  adopt: no block for the moved pages' table (%08x)\n",
                 (uint32_t)reback_moved_uid);
            reback_moved_uid = -1;
            ret = AZAHAR_ERR_ALLOC;
            goto fail;
        }
    }
    if ((ret = alloc_block("azahar_host_l2", mbs * 0x400u < 0x1000u ? 0x1000u : mbs * 0x400u,
                           0x1000, &reback_l2_uid, &reback_l2, &reback_l2_pa)) != 0) {
        goto fail;
    }
    if (!(reback_l1 = reback_map_l1(user_va, mbs, &reback_l1_uid))) {
        ret = AZAHAR_ERR_TABLE;
        goto fail;
    }
    // Every page resolved first; nothing of the app's changes unless all of them do.
    unsigned sections = 0, coarse = 0;
    for (uint32_t mb = 0; mb < mbs; mb++) {
        const uint32_t l1 = reback_l1[mb];
        if ((l1 & 3u) == 1u) {
            coarse++;
        } else if ((l1 & 3u) == 2u) {
            sections++;
        } else {
            emit("  adopt: megabyte %08x has level-1 entry %08x\n", user_va + (mb << 20), l1);
            ret = AZAHAR_ERR_TABLE;
            goto fail;
        }
        for (uint32_t i = 0; i < 256; i++) {
            const uint32_t uva = user_va + (mb << 20) + (i << 12);
            const uint32_t par = reback_user_par(uva);
            const uint32_t attr = PAR_FAULTED(par) ? 0 : azahar_attr_from_par(par);
            if (!attr) {
                emit("  adopt: user %08x is not writable Normal memory (par %08x)\n", uva, par);
                ret = AZAHAR_ERR_PAGE;
                goto fail;
            }
            reback_l2[mb * 256u + i] = reback_host_page(PAR_PA(par), attr);
        }
        // The same domain (bits 8:5) and security as the kernel's entry, section or table.
        const uint32_t ns = (l1 & 3u) == 2u ? (l1 >> 19) & 1u : (l1 >> 3) & 1u;
        reback_old_l1[mb] = l1;
        reback_new_l1[mb] = ((uint32_t)reback_l2_pa + mb * 0x400u) | (l1 & (0xFu << 5)) |
                            (ns << 3) | 1u;
    }
    asm volatile("dsb" ::: "memory");
    for (uint32_t mb = 0; mb < mbs; mb++) {
        reback_l1[mb] = reback_new_l1[mb];
    }
    reback_tlb_flush_all();
    reback_base = user_va;
    reback_mbs = mbs;
    reback_owner = ksceKernelGetProcessId();
    emit("  adopted %u MB (%u were sections, %u tables) under our tables at pa %08x\n", mbs,
         sections, coarse, (uint32_t)reback_l2_pa);
    ksceKernelPrintf("[azaharnative] adopt: user %08x+%08x under our tables (%u section(s), %u "
                     "table(s) before)\n",
                     user_va, size, sections, coarse);
    goto out;

fail:
    if (reback_l1_uid >= 0) {
        ksceKernelFreeMemBlock(reback_l1_uid);
        reback_l1_uid = -1;
    }
    reback_l1 = NULL;
    if (reback_l2_uid >= 0) {
        ksceKernelFreeMemBlock(reback_l2_uid);
        reback_l2_uid = -1;
    }
    reback_l2 = NULL;
out:
    emit("  -> %d\n", ret);
    log_close();
    reback_unlock();
    EXIT_SYSCALL(state);
    return ret;
}

int azaharReback(const AzaharRebackRequest *user_req) {
    uint32_t state;
    ENTER_SYSCALL(state);
    AzaharRebackRequest req;
    if (ksceKernelCopyFromUser(&req, user_req, sizeof(req)) < 0) {
        EXIT_SYSCALL(state);
        return AZAHAR_ERR_ARG;
    }
    const uint32_t pages = req.size / 0x1000u;
    if (((req.user_va | req.pool_va | req.size) & 0xFFFu) || !pages || pages > REBACK_BATCH ||
        (req.flags & ~AZAHAR_REBACK_COPY) || req.pool_va + (req.size - 1) < req.pool_va) {
        EXIT_SYSCALL(state);
        return AZAHAR_ERR_ARG;
    }
    int ret = reback_lock();
    if (ret) {
        EXIT_SYSCALL(state);
        return ret;
    }
    unsigned moved = 0;
    if (reback_owner != ksceKernelGetProcessId() || req.user_va < reback_base ||
        req.user_va - reback_base + (uint64_t)req.size > (uint64_t)reback_mbs << 20) {
        ret = AZAHAR_ERR_STATE;
        goto out;
    }
    if (reback_moved_count + pages > REBACK_MAX_PAGES) {
        ret = AZAHAR_ERR_ALLOC;
        goto out;
    }
    for (uint32_t i = 0; i < pages; i++) {
        if (reback_flipped_find(req.user_va + i * 0x1000u) != reback_flipped_count) {
            ret = AZAHAR_ERR_STATE;
            goto out;
        }
    }
    log_open();

    // The new pages and their attributes, from the app's own mapping of them.
    for (uint32_t i = 0; i < pages; i++) {
        const uint32_t par = reback_user_par(req.pool_va + i * 0x1000u);
        const uint32_t attr = PAR_FAULTED(par) ? 0 : azahar_attr_from_par(par);
        if (!attr) {
            emit("  reback: pool %08x is not writable Normal memory (par %08x)\n",
                 req.pool_va + i * 0x1000u, par);
            ret = AZAHAR_ERR_PAGE;
            goto out_log;
        }
        reback_pairs[i].uva = req.user_va + i * 0x1000u;
        reback_pairs[i].new_pa = PAR_PA(par);
        reback_pairs[i].attr = attr;
    }
    // The old contents go first, while the app's addresses still reach them: through the cache,
    // so what a core holds dirty comes along.
    if (req.flags & AZAHAR_REBACK_COPY) {
        for (uint32_t i = 0; i < pages; i++) {
            if (ksceKernelCopyFromUser(reback_copy_buf,
                                       (const void *)(uintptr_t)(req.user_va + i * 0x1000u),
                                       0x1000) < 0 ||
                ksceKernelCopyToUser((void *)(uintptr_t)(req.pool_va + i * 0x1000u),
                                     reback_copy_buf, 0x1000) < 0) {
                emit("  reback: copy of user %08x failed\n", req.user_va + i * 0x1000u);
                ret = AZAHAR_ERR_PAGE;
                goto out_log;
            }
        }
    }
    // Our descriptors for the app's pages. The walk sees them after the invalidation below.
    for (uint32_t i = 0; i < pages; i++) {
        uint32_t *d = &reback_l2[(req.user_va - reback_base) / 0x1000u + i];
        reback_pairs[i].old_pa = *d & 0xFFFFF000u;
        const uint32_t old_attr = *d & AZAHAR_ATTR_MASK;
        *d = reback_host_page(reback_pairs[i].new_pa, reback_pairs[i].attr);
        reback_moved_insert(reback_pairs[i].new_pa, reback_pairs[i].attr, reback_pairs[i].old_pa,
                            old_attr);
        moved++;
    }
    reback_tlb_flush_all();
    // The guest's descriptors for these app pages follow. The GPU's keep the old pages: the app
    // remaps them in its queue's order (sgxdPte), after the work that still reads them.
    reback_pair_count = moved;
    const unsigned guest = reback_retarget_guest(1);
    emit("== azaharReback user %08x pool %08x size %08x%s: %u page(s) moved, %u guest "
         "descriptor(s) retargeted\n",
         req.user_va, req.pool_va, req.size, (req.flags & AZAHAR_REBACK_COPY) ? " copy" : "",
         moved, guest);

out_log:
    log_close();
out:
    reback_unlock();
    EXIT_SYSCALL(state);
    return ret < 0 ? ret : (int)moved;
}

int azaharRebackReturn(uint32_t user_va, uint32_t size) {
    uint32_t state;
    ENTER_SYSCALL(state);
    const uint32_t pages = size / 0x1000u;
    if (((user_va | size) & 0xFFFu) || !pages || pages > REBACK_BATCH) {
        EXIT_SYSCALL(state);
        return AZAHAR_ERR_ARG;
    }
    int ret = reback_lock();
    if (ret) {
        EXIT_SYSCALL(state);
        return ret;
    }
    unsigned returned = 0;
    if (reback_owner != ksceKernelGetProcessId() || user_va < reback_base ||
        user_va - reback_base + (uint64_t)size > (uint64_t)reback_mbs << 20) {
        ret = AZAHAR_ERR_STATE;
        goto out;
    }
    log_open();
    // Every page must be a moved one, and none flipped: nothing changes otherwise.
    for (uint32_t i = 0; i < pages; i++) {
        if (reback_flipped_find(user_va + i * 0x1000u) != reback_flipped_count) {
            emit("  return: user %08x is flipped\n", user_va + i * 0x1000u);
            ret = AZAHAR_ERR_STATE;
            goto out_log;
        }
        const uint32_t d = reback_l2[(user_va - reback_base) / 0x1000u + i];
        if (reback_moved_find(d & 0xFFFFF000u) == reback_moved_count) {
            emit("  return: user %08x (pa %08x) was not moved\n", user_va + i * 0x1000u,
                 d & 0xFFFFF000u);
            ret = AZAHAR_ERR_PAGE;
            goto out_log;
        }
    }
// Page by page: the contents out of the pool page through the app's address, the address
// back on its old page, the contents in through the cache (overwriting stale lines whole).
    for (uint32_t i = 0; i < pages; i++) {
        const uint32_t uva = user_va + i * 0x1000u;
        uint32_t *d = &reback_l2[(uva - reback_base) / 0x1000u];
        const unsigned at = reback_moved_find(*d & 0xFFFFF000u);
        const RebackMoved m = reback_moved[at];
        if (ksceKernelCopyFromUser(reback_copy_buf, (const void *)(uintptr_t)uva, 0x1000) < 0) {
            emit("  return: copy out of user %08x failed\n", uva);
            ret = AZAHAR_ERR_PAGE;
            break;
        }
        *d = reback_host_page(m.old_pa, m.old_attr);
        reback_tlb_flush_all();
        if (ksceKernelCopyToUser((void *)(uintptr_t)uva, reback_copy_buf, 0x1000) < 0) {
            // The page is back on its old one already; its contents are lost.
            emit("  return: copy into user %08x failed\n", uva);
            ret = AZAHAR_ERR_PAGE;
        }
        reback_pairs[returned].uva = uva;
        reback_pairs[returned].old_pa = m.new_pa;   // the other views name the pool page
        reback_pairs[returned].new_pa = m.old_pa;
        reback_pairs[returned].attr = m.old_attr;
        reback_moved_remove(at);
        returned++;
        if (ret < 0) {
            break;
        }
    }
    // The guest's view follows, as a move's; the GPU's is the app's to remap.
    reback_pair_count = returned;
    const unsigned guest = reback_retarget_guest(1);
    emit("== azaharRebackReturn user %08x size %08x: %u page(s) back, %u guest descriptor(s) "
         "retargeted\n",
         user_va, size, returned, guest);

out_log:
    log_close();
out:
    reback_unlock();
    EXIT_SYSCALL(state);
    return ret < 0 ? ret : (int)returned;
}

static AzaharFlipRun reback_flip_runs[AZAHAR_FLIP_MAX_RUNS];

int azaharFlip(const AzaharFlipRun *user_runs, uint32_t count) {
    uint32_t state;
    ENTER_SYSCALL(state);
    if (count == 0 || count > AZAHAR_FLIP_MAX_RUNS) {
        EXIT_SYSCALL(state);
        return AZAHAR_ERR_ARG;
    }
    int ret = reback_lock();
    if (ret) {
        EXIT_SYSCALL(state);
        return ret;
    }
    unsigned flipped = 0;
    if (reback_owner != ksceKernelGetProcessId()) {
        ret = AZAHAR_ERR_STATE;
        goto out;
    }
    if (ksceKernelCopyFromUser(reback_flip_runs, user_runs, count * sizeof(AzaharFlipRun)) < 0) {
        ret = AZAHAR_ERR_ARG;
        goto out;
    }
    for (uint32_t r = 0; r < count; r++) {
        const AzaharFlipRun *run = &reback_flip_runs[r];
        const uint32_t uva = run->user_va;
        if (((uva | run->version_va) & 0xFFFu) || uva < reback_base ||
            uva - reback_base >= reback_mbs << 20) {
            ret = AZAHAR_ERR_ARG;
            break;
        }
        uint32_t *const d = &reback_l2[(uva - reback_base) / 0x1000u];
        const uint32_t cur_pa = *d & 0xFFFFF000u;
        const unsigned f = reback_flipped_find(uva);
        uint32_t new_pa, attr;
        if (run->version_va) {
            const uint32_t par = reback_user_par(run->version_va);
            attr = PAR_FAULTED(par) ? 0 : azahar_attr_from_par(par);
            if (!attr) {
                ret = AZAHAR_ERR_PAGE;
                break;
            }
            new_pa = PAR_PA(par);
            if (f == reback_flipped_count) {
                if (reback_flipped_count == REBACK_MAX_FLIPPED) {
                    ret = AZAHAR_ERR_ALLOC;
                    break;
                }
                // The home's lines leave the cache through the app's address while it still
                // shows the home: the GPU alone writes it from now on.
                SceKernelIntrStatus st = ksceKernelCpuSuspendIntr();
                const uint32_t dacr = rd_dacr();
                wr_dacr(dacr_grant_all(dacr));
                ksceKernelDcacheCleanInvalidateRange((void *)(uintptr_t)uva, 0x1000);
                wr_dacr(dacr);
                ksceKernelCpuResumeIntr(st);
                RebackFlipped *const e = &reback_flipped[reback_flipped_count++];
                e->uva = uva;
                e->home_pa = cur_pa;
                e->home_attr = *d & AZAHAR_ATTR_MASK;
            }
            RebackFlipped *const e = &reback_flipped[reback_flipped_find(uva)];
            e->version_pa = new_pa;
            e->version_attr = attr;
        } else {
            if (f == reback_flipped_count) {
                ret = AZAHAR_ERR_PAGE;
                break;
            }
            new_pa = reback_flipped[f].home_pa;
            attr = reback_flipped[f].home_attr;
            reback_flipped[f] = reback_flipped[--reback_flipped_count];
        }
        *d = reback_host_page(new_pa, attr);
        reback_pairs[flipped].uva = uva;
        reback_pairs[flipped].old_pa = cur_pa;
        reback_pairs[flipped].new_pa = new_pa;
        reback_pairs[flipped].attr = attr;
        flipped++;
    }
    if (flipped) {
        reback_tlb_flush_all();
        // The guest's view follows; the GPU's keeps its pages.
        reback_pair_count = flipped;
        const unsigned guest = reback_retarget_guest(0);
        if (ret == 0) {
            ret = (int)guest;
        }
    }

out:
    reback_unlock();
    EXIT_SYSCALL(state);
    return ret;
}

int azaharRebackRestore(void) {
    uint32_t state;
    ENTER_SYSCALL(state);
    int ret = reback_lock();
    if (ret == 0) {
        if (reback_owner >= 0 && reback_owner != ksceKernelGetProcessId()) {
            ret = AZAHAR_ERR_STATE;
        } else {
            reback_restore_locked();
        }
        reback_unlock();
    }
    EXIT_SYSCALL(state);
    return ret;
}

// The owner process is ending: the kernel's level-1 entries go back before it frees the block.
static void reback_on_proc_gone(SceUID pid) {
    if (reback_mutex < 0 || pid != reback_owner) {
        return;
    }
    ksceKernelLockMutex(reback_mutex, 1, NULL);
    if (pid == reback_owner) {
        reback_restore_locked();
    }
    ksceKernelUnlockMutex(reback_mutex, 1);
}
