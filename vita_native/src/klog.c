// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// The kernel's printf, made visible: no handler of
// the system's shows it. azaharKlogRead registers ours on its first call: each
// printf is formatted into a ring, and the app drains it into its own console output.
//
// Where the app's TTY ends in ksceKernelPrintf on the writing thread, the ring would feed
// itself: from the first text of the thread that drained last, everything goes to the previous
// handler, which the next azaharKlogRead puts back.

#include <psp2kern/kernel/cpu/spinlock.h>
#include <psp2kern/kernel/threadmgr.h>

#define KLOG_SIZE 0x8000u        // a power of two
#define KLOG_LINE 256u
#define KLOG_READ_MAX 0x1000u

typedef int (*klog_handler_fn)(void *argp, const char *fmt, va_list ap);
typedef int (*klog_register_fn)(klog_handler_fn handler, void *argp);

static char klog_ring[KLOG_SIZE];
static uint32_t klog_head, klog_tail;   // running byte counts: written, read
static uint32_t klog_dropped;           // bytes overwritten before the app read them
static SceKernelSpinlock klog_lock;
static klog_register_fn klog_register;   // set while klog's handler is registered
static int klog_started;                 // klog_start ran, capturing or not
// The handler ours replaced, and its argument, put back when klog stands down.
static klog_handler_fn klog_prev;
static void *klog_prev_argp;
static volatile SceUID klog_drainer;     // the thread the last azaharKlogRead gave text to
static volatile int klog_looped;         // that thread's printf came back to the handler
static char klog_out[KLOG_READ_MAX];

// Runs inside ksceKernelPrintf, under SceSysmem's spinlock with interrupts off, on any core and
// in any context: no blocking, no allocation.
static int klog_handler(void *argp, const char *fmt, va_list ap) {
    (void)argp;
    if (klog_looped || (klog_drainer > 0 && ksceKernelGetThreadId() == klog_drainer)) {
        klog_looped = 1;
        return klog_prev ? klog_prev(klog_prev_argp, fmt, ap) : 0;
    }
    char line[KLOG_LINE];
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    if (n <= 0) {
        return n;
    }
    if ((uint32_t)n >= sizeof(line)) {
        n = sizeof(line) - 1;
    }
    const SceKernelIntrStatus s = ksceKernelSpinlockLowLockCpuSuspendIntr(&klog_lock);
    for (int i = 0; i < n; i++) {
        klog_ring[klog_head++ & (KLOG_SIZE - 1)] = line[i];
    }
    if (klog_head - klog_tail > KLOG_SIZE) {
        klog_dropped += klog_head - klog_tail - KLOG_SIZE;
        klog_tail = klog_head - KLOG_SIZE;
    }
    ksceKernelSpinlockLowUnlockCpuResumeIntr(&klog_lock, s);
    return n;
}

// Where ksceKernelRegisterKprintfHandler keeps the handler and its argument (no getter): read
// from its movw/movt pairs and `str r0, [rH]`, `str r1, [rA]`. Anything else is refused.
static int klog_find_slots(uintptr_t fn, uint32_t **handler_slot, uint32_t **argp_slot) {
    const uint16_t *code = (const uint16_t *)(fn & ~(uintptr_t)1);
    uint32_t reg[8] = {0};
    unsigned low = 0, high = 0;   // registers whose movw / movt were seen
    int h = -1, a = -1;
    for (unsigned i = 0; i < 12; i++) {
        const uint16_t hw1 = code[i];
        if ((hw1 & 0xFBF0u) == 0xF240u || (hw1 & 0xFBF0u) == 0xF2C0u) {
            const uint16_t hw2 = code[++i];
            const unsigned rd = (hw2 >> 8) & 0xFu;
            if ((hw2 & 0x8000u) || rd > 7) {
                return -1;
            }
            const uint32_t imm = (uint32_t)(hw1 & 0xFu) << 12 | (uint32_t)((hw1 >> 10) & 1u) << 11 |
                                 (uint32_t)((hw2 >> 12) & 7u) << 8 | (hw2 & 0xFFu);
            if ((hw1 & 0xFBF0u) == 0xF240u) {
                reg[rd] = (reg[rd] & 0xFFFF0000u) | imm;
                low |= 1u << rd;
            } else {
                reg[rd] = (reg[rd] & 0xFFFFu) | imm << 16;
                high |= 1u << rd;
            }
        } else if ((hw1 & 0xFFC0u) == 0x6000u && (hw1 & 7u) <= 1) {   // str rT, [rN, #0]
            const unsigned rn = (hw1 >> 3) & 7u;
            if (((low & high) >> rn & 1u) == 0) {
                return -1;
            }
            if ((hw1 & 7u) == 0) {
                h = (int)rn;
            } else {
                a = (int)rn;
            }
        } else if (hw1 == 0x4770u) {   // bx lr
            break;
        } else {
            return -1;
        }
    }
    if (h < 0 || a < 0) {
        return -1;
    }
    *handler_slot = (uint32_t *)(uintptr_t)reg[h];
    *argp_slot = (uint32_t *)(uintptr_t)reg[a];
    return 0;
}

// SceDebugForKernel's registration: 3.65's NIDs, then 3.60's. Without the slots, which the
// handler has to be put back from, klog stays out.
static int klog_start(void) {
    static const uint32_t nids[2][2] = {{0x13D793B7, 0x88AD6D0C}, {0x88C17370, 0x10067B7B}};
    klog_register_fn reg = NULL;
    uintptr_t fn = 0;
    for (unsigned i = 0; i < 2 && !reg; i++) {
        if (module_get_export_func(AZAHAR_KERNEL_PID, "SceSysmem", nids[i][0], nids[i][1],
                                   &fn) >= 0 && fn) {
            reg = (klog_register_fn)fn;
        }
    }
    if (!reg) {
        return AZAHAR_ERR_RESOLVE;
    }
    klog_started = 1;
    uint32_t *handler_slot = NULL, *argp_slot = NULL;
    if (klog_find_slots(fn, &handler_slot, &argp_slot) < 0) {
        ksceKernelPrintf("[azaharnative] klog: the kprintf handler's slot could not be read; not "
                         "captured\n");
        return 0;
    }
    const SceKernelIntrStatus s = ksceKernelCpuSuspendIntr();
    klog_prev = (klog_handler_fn)(uintptr_t)*handler_slot;
    klog_prev_argp = (void *)(uintptr_t)*argp_slot;
    reg(klog_handler, NULL);
    klog_register = reg;
    ksceKernelCpuResumeIntr(s);
    ksceKernelPrintf("[azaharnative] klog: kernel printf captured (the handler before ours: "
                     "%p)\n", (void *)klog_prev);
    return 0;
}

static void klog_stop(void) {
    if (klog_register) {
        klog_register(klog_prev, klog_prev_argp);
        klog_register = NULL;
    }
}

// Copies up to `size` bytes of kernel printf text, oldest first, into `user_buf` (no NUL).
// Returns the count, 0 when the ring is empty (for good, once klog has stood down and the ring
// is drained). A gap is marked in the text itself. The first call starts the capture.
int azaharKlogRead(char *user_buf, uint32_t size) {
    uint32_t state;
    ENTER_SYSCALL(state);
    int ret = 0;
    if (!klog_started) {
        ret = klog_start();
    }
    if (klog_looped && klog_register) {
        klog_stop();
        ksceKernelPrintf("[azaharnative] klog: the app's TTY comes back through kernel printf; "
                         "the handler before ours is back\n");
    }
    if (ret == 0 && user_buf && size) {
        if (size > KLOG_READ_MAX) {
            size = KLOG_READ_MAX;
        }
        uint32_t n = 0;
        const SceKernelIntrStatus s = ksceKernelSpinlockLowLockCpuSuspendIntr(&klog_lock);
        if (klog_dropped) {
            n = (uint32_t)snprintf(klog_out, size, "[klog] %u bytes lost\n", klog_dropped);
            n = n < size ? n : size;
            klog_dropped = 0;
        }
        while (n < size && klog_tail != klog_head) {
            klog_out[n++] = klog_ring[klog_tail++ & (KLOG_SIZE - 1)];
        }
        ksceKernelSpinlockLowUnlockCpuResumeIntr(&klog_lock, s);
        ret = n && ksceKernelCopyToUser(user_buf, klog_out, n) < 0 ? AZAHAR_ERR_ARG : (int)n;
        if (ret > 0) {
            klog_drainer = ksceKernelGetThreadId();
        }
    }
    EXIT_SYSCALL(state);
    return ret;
}
