// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// azahar-native — sgxd.c: the direct-SGX calls. Takes the GPU from SceGpuEs4 and gives it back.
//
// Included from main.c after native.c, whose process-event handler and state it shares (one
// translation unit, as main.c explains). sgxd.h is the ABI; the design is
// docs/sgxd/takeover.md, and every SceGpuEs4 address below is from its 3.65 build.
//
//   sgxdTake     SceGpu's own suspend path through its sysevent handler 0x81007764:
//                (0, 0x400) arms its gate, so every other process's GPU call sleeps at bridge
//                entry; (0, 0x402) until 0, so none is still inside; (0, 0x4005) saves its
//                microkernel, powers the GPU off and frees its register block. Then ours:
//                ScePervasive's GPU power-on, our own mapping of the block, the start sequence
//                SceGpu's resume writes before its first read, a per-core cache reset, the IDs.
//   sgxdRelease  the reverse: cache reset, unmap, power-off, then (1, 0x10005), which is how the
//                system resumes SceGpu: power state 0, its microkernel reloaded, the gate open.
//   sgxdSubmit   an op list against the registers (B1: reads and delays).
//   sgxdDump     Sony's own register lists, read out of SceGpuEs4's data segment.
//
// Step B1 of the plan: no GPU memory and no rendering yet.

#include <psp2kern/kernel/suspend.h>
#include <psp2kern/kernel/threadmgr/mutex.h>
#include "sgxd.h"

// SceKernelSuspendForDriver 0xDD61D621 (the stub has it; the SDK's header does not).
int ksceKernelUnregisterSysEventHandler(SceUID uid);

#define SGXD_LOG(...) ksceKernelPrintf("[sgxd] " __VA_ARGS__)

#define SGXD_REG_PA 0xE8400000u
#define SGXD_REG_SIZE 0x20000u
#define SGXD_CORES 4u
#define SGXD_CORE_BANK(core) (0x8000u + (core) * 0x4000u)

#define SGX_CORE_ID 0x0020u
#define SGX_CORE_REVISION 0x0024u
#define SGX_SOFT_RESET 0x0080u
#define SGX_SOFT_RESET_CACHES 0x000c8d00u
#define SGX_BIF_INT_STAT 0x0c04u
#define SGX_BIF_FAULT 0x0c08u
#define SGX_MASTER_CORE_CONFIG 0x4000u
#define SGX_MASTER_BIF_FAULT 0x4c08u
#define SGX_MASTER_SLC_EVENT_STATUS 0x4d10u
#define SGX_MASTER_SLC_EVENT_CLEAR 0x4d14u
#define SGX_MASTER_SLC_CTRL_INVAL 0x4d28u
#define SGX_MASTER_SLC_CTRL_FLUSH 0x4d2cu
#define SGX_SLC_FLUSH_ALL 0x10u
#define SGX_SLC_INVAL_ALL 0x01u
#define SGX_SLC_EVENT_INVAL 0x1u
#define SGX_SLC_EVENT_FLUSH 0x2u

#define SGXD_EXPECT_CORE_ID 0x01174403u
#define SGXD_EXPECT_CORE_REVISION 0x00020106u

// SceGpuEs4 3.65. The module NID pins the build; the fingerprints pin what is called and read.
#define SGXD_GPU_MODULE_NID 0xC0F361A3u
#define SGXD_GPU_SYSEVENT 0x7764u         // GpuSysEventHandler, Thumb
#define SGXD_GPU_SYSEVENT_CODE 0x70u      // its code, up to the literal pool the loader relocates
#define SGXD_GPU_SYSEVENT_PRINT 0x35E469F5u
#define SGXD_GPU_REGLISTS 0x1ECu          // data: 399 per-core offsets, then 266 bank offsets
#define SGXD_GPU_REGLISTS_SIZE 0xA64u
#define SGXD_GPU_REGLISTS_PRINT 0x68ACDAB9u
#define SGXD_GPU_BANK_LIST 0x63Cu         // the bank list's start within the lists
#define SGXD_GPU_DEBUG_IDS 0x38u          // data: SGXDumpDebugState's 109 debug-bus select IDs,
                                          // one a word (its low byte)

// SceLowio's ScePervasiveForDriver: the GPU domain on and off (SMC 0x110, bus 0), the calls
// SceGpu's device power callbacks make. Not in the SDK's stubs, so resolved at runtime.
#define SGXD_LOWIO_PERVASIVE 0xE692C727u
#define SGXD_GPU_POWER_ON 0x84EAB5BCu
#define SGXD_GPU_POWER_OFF 0xE8656ADFu

#define SGXD_BUSY 0x804C0006u    // SceGpu's own answer to (0, 0x402) while a call is inside
#define SGXD_QUIESCE_TRIES 2000u // 1 ms apart
#define SGXD_POLL_US 100000u     // SLC flush and invalidate

typedef int (*sgxd_sysevent_fn)(int resume, int eventid, void *args, void *opt);
typedef void (*sgxd_void_fn)(void);

enum { SGXD_NONE, SGXD_OWNED, SGXD_LOST };
static int sgxd_state;
static SceUID sgxd_owner = -1;
static SceUID sgxd_mutex = -1;
static SceUID sgxd_sysevent_uid = -1;

static sgxd_sysevent_fn sgxd_gpu_sysevent;
static sgxd_void_fn sgxd_power_on, sgxd_power_off;
static const uint32_t *sgxd_core_list, *sgxd_bank_list;
static const uint32_t *sgxd_debug_ids;
static uint32_t sgxd_module_nid;

static SceUID sgxd_reg_uid = -1;
static volatile uint32_t *sgxd_reg;

static SgxdOp sgxd_ops[SGXD_MAX_OPS];
static uint32_t sgxd_dump_words[SGXD_DUMP_WORDS];

static uint32_t sgxd_fingerprint(const uint8_t *data, unsigned size) {
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < size; i++) {
        hash = (hash ^ data[i]) * 16777619u;
    }
    return hash;
}

static uint32_t sgxd_fold(uint32_t hash, uint16_t halfword) {
    hash = (hash ^ (halfword & 0xffu)) * 16777619u;
    return (hash ^ (halfword >> 8)) * 16777619u;
}

// FNV-1a over Thumb code with MOVW/MOVT immediates masked out, which pin the kernel's
// code: those carry relocated addresses, everything else must match the build.
static uint32_t sgxd_code_fingerprint(const uint16_t *code, unsigned size) {
    const unsigned count = size / 2;
    uint32_t hash = 2166136261u;
    for (unsigned i = 0; i < count; i++) {
        uint16_t first = code[i];
        if ((first >> 11) >= 0x1d && i + 1 < count) {
            uint16_t second = code[++i];
            if ((first & 0xfbf0) == 0xf240 || (first & 0xfbf0) == 0xf2c0) {
                first &= 0xfbf0;
                second &= 0x0f00;
            }
            hash = sgxd_fold(sgxd_fold(hash, first), second);
        } else {
            hash = sgxd_fold(hash, first);
        }
    }
    return hash;
}

// Once per boot: the module is never unloaded, and neither is SceGpuEs4.
static int sgxd_identify(SgxdInfo *info) {
    if (sgxd_gpu_sysevent) {
        info->module_nid = sgxd_module_nid;
        return 0;
    }
    tai_module_info_t tai = {.size = sizeof(tai)};
    int r = taiGetModuleInfoForKernel(AZAHAR_KERNEL_PID, "SceGpuEs4", &tai);
    if (r < 0) {
        info->step_status = r;
        return AZAHAR_ERR_RESOLVE;
    }
    info->module_nid = tai.module_nid;
    if (tai.module_nid != SGXD_GPU_MODULE_NID) {
        SGXD_LOG("SceGpuEs4 module nid %08x, want %08x\n", tai.module_nid, SGXD_GPU_MODULE_NID);
        return SGXD_ERR_FIRMWARE;
    }
    uintptr_t text = 0, data = 0;
    if ((r = module_get_offset(AZAHAR_KERNEL_PID, tai.modid, 0, 0, &text)) < 0 ||
        (r = module_get_offset(AZAHAR_KERNEL_PID, tai.modid, 1, 0, &data)) < 0) {
        info->step_status = r;
        return AZAHAR_ERR_RESOLVE;
    }
    const uint32_t code = sgxd_code_fingerprint((const uint16_t *)(text + SGXD_GPU_SYSEVENT),
                                                SGXD_GPU_SYSEVENT_CODE);
    const uint32_t lists = sgxd_fingerprint((const uint8_t *)(data + SGXD_GPU_REGLISTS),
                                            SGXD_GPU_REGLISTS_SIZE);
    SGXD_LOG("SceGpuEs4 text %08x data %08x, handler %08x, register lists %08x\n",
             (uint32_t)text, (uint32_t)data, code, lists);
    if (code != SGXD_GPU_SYSEVENT_PRINT || lists != SGXD_GPU_REGLISTS_PRINT) {
        info->step_status = (int32_t)(code != SGXD_GPU_SYSEVENT_PRINT ? code : lists);
        return SGXD_ERR_FIRMWARE;
    }
    uintptr_t on = 0, off = 0;
    if (module_get_export_func(AZAHAR_KERNEL_PID, "SceLowio", SGXD_LOWIO_PERVASIVE,
                               SGXD_GPU_POWER_ON, &on) < 0 || !on ||
        module_get_export_func(AZAHAR_KERNEL_PID, "SceLowio", SGXD_LOWIO_PERVASIVE,
                               SGXD_GPU_POWER_OFF, &off) < 0 || !off) {
        SGXD_LOG("ScePervasiveForDriver GPU power calls not found\n");
        return AZAHAR_ERR_RESOLVE;
    }
    sgxd_power_on = (sgxd_void_fn)on;
    sgxd_power_off = (sgxd_void_fn)off;
    sgxd_core_list = (const uint32_t *)(data + SGXD_GPU_REGLISTS);
    sgxd_bank_list = (const uint32_t *)(data + SGXD_GPU_REGLISTS + SGXD_GPU_BANK_LIST);
    sgxd_debug_ids = (const uint32_t *)(data + SGXD_GPU_DEBUG_IDS);
    sgxd_module_nid = tai.module_nid;
    sgxd_gpu_sysevent = (sgxd_sysevent_fn)(text + SGXD_GPU_SYSEVENT + 1);
    return 0;
}

static inline uint32_t sgxd_read(uint32_t reg) {
    const uint32_t v = sgxd_reg[reg / 4];
    asm volatile("dmb" ::: "memory");
    return v;
}

static inline void sgxd_write(uint32_t reg, uint32_t value) {
    sgxd_reg[reg / 4] = value;
    asm volatile("dsb" ::: "memory");
}

// The same memblock SceGpu's SGXConfigureSystem (0x8100d1c4) allocates as "SceSGX543Reg".
static int sgxd_map(void) {
    SceKernelAllocMemBlockKernelOpt opt = {0};
    opt.size = sizeof(opt);
    opt.attr = 2;   // physical address given
    opt.paddr = SGXD_REG_PA;
    sgxd_reg_uid = ksceKernelAllocMemBlock("SceSgxdReg", 0x20100206, SGXD_REG_SIZE, &opt);
    if (sgxd_reg_uid < 0) {
        return sgxd_reg_uid;
    }
    void *base = NULL;
    const int r = ksceKernelGetMemBlockBase(sgxd_reg_uid, &base);
    if (r < 0) {
        ksceKernelFreeMemBlock(sgxd_reg_uid);
        sgxd_reg_uid = -1;
        return r;
    }
    sgxd_reg = base;
    return 0;
}

static void sgxd_unmap(void) {
    if (sgxd_reg_uid >= 0) {
        ksceKernelFreeMemBlock(sgxd_reg_uid);
    }
    sgxd_reg_uid = -1;
    sgxd_reg = NULL;
}

// What SceGpu's SGXStart writes after the power-up and before its first read: the
// register writes of its resume up to the core clock gates. Its waits are 100 GPU clocks.
static void sgxd_start_sequence(void) {
    sgxd_write(0x4000, 3);
    sgxd_write(0x4020, 0x2a9);
    sgxd_write(0x4004, 0xaa);
    sgxd_write(0x4080, 0x7ff);
    ksceKernelDelayThread(10);
    sgxd_write(0x4c00, 0);
    sgxd_write(0x4d00, 0x4c800);
    sgxd_write(0x4d04, 0x4000000);
    ksceKernelDelayThread(10);
    sgxd_write(0x4080, 0);
    ksceKernelDelayThread(10);
    sgxd_write(0x0000, 0x2aaaaa);
    sgxd_write(0x0004, 0xa8a8aaa);
    ksceKernelDelayThread(10);
}

// Every core's caches emptied through its own bank (the PDS keeps event programs across
// everything but a soft reset).
static void sgxd_reset_cores(void) {
    for (uint32_t core = 0; core < SGXD_CORES; core++) {
        sgxd_write(SGXD_CORE_BANK(core) + SGX_SOFT_RESET, SGX_SOFT_RESET_CACHES);
        ksceKernelDelayThread(10);
        sgxd_write(SGXD_CORE_BANK(core) + SGX_SOFT_RESET, 0);
        ksceKernelDelayThread(10);
    }
}

static int sgxd_slc_op(uint32_t ctrl, uint32_t value, uint32_t event) {
    sgxd_write(ctrl, value);
    const uint32_t t0 = ksceKernelGetSystemTimeLow();
    while (!(sgxd_read(SGX_MASTER_SLC_EVENT_STATUS) & event)) {
        if (ksceKernelGetSystemTimeLow() - t0 > SGXD_POLL_US) {
            return -1;
        }
        ksceKernelDelayThread(10);
    }
    sgxd_write(SGX_MASTER_SLC_EVENT_CLEAR, event);
    return 0;
}

// Our side down: the SLC written back and emptied, the cores reset, the block unmapped, the
// domain off. Sony's side is not touched.
static void sgxd_hw_down(void) {
    if (sgxd_reg) {
        const int flush = sgxd_slc_op(SGX_MASTER_SLC_CTRL_FLUSH, SGX_SLC_FLUSH_ALL, SGX_SLC_EVENT_FLUSH);
        const int inval = sgxd_slc_op(SGX_MASTER_SLC_CTRL_INVAL, SGX_SLC_INVAL_ALL, SGX_SLC_EVENT_INVAL);
        if (flush || inval) {
            SGXD_LOG("SLC flush %d invalidate %d (timeout -1)\n", flush, inval);
        }
        sgxd_reset_cores();
    }
    sgxd_unmap();
    if (sgxd_power_off) {
        sgxd_power_off();
    }
}

// SceGpu back to power state 0 with its microkernel, the gate open: the system's resume.
static void sgxd_hand_back(void) {
    sgxd_gpu_sysevent(1, 0x10005, NULL, NULL);
}

// Registered once, after SceGpu's "SceGpu", so it runs after SceGpu's in every phase
// (ksceKernelSysEventDispatch walks the list in order). SceGpu's own suspend meets the state
// the take left: its gate already armed, power state 3 already (a no-op). Ours then takes our
// side down. On resume SceGpu comes back first and owns the GPU again; the app's next call
// sees SGXD_ERR_LOST and takes again.
static int sgxd_sysevent(int resume, int eventid, void *args, void *opt) {
    (void)args; (void)opt;
    if (resume == 0 && eventid == 0x402) {
        // A syscall of ours in progress holds the mutex; the system asks again later.
        if (sgxd_mutex >= 0 && ksceKernelTryLockMutex(sgxd_mutex, 1) < 0) {
            return (int)SGXD_BUSY;
        }
        if (sgxd_mutex >= 0) {
            ksceKernelUnlockMutex(sgxd_mutex, 1);
        }
    } else if (resume == 0 && eventid == 0x4005 && sgxd_mutex >= 0) {
        // A call that started after the 0x402 query is waited for; they are all bounded.
        ksceKernelLockMutex(sgxd_mutex, 1, NULL);
        if (sgxd_state == SGXD_OWNED) {
            sgxd_hw_down();
            sgxd_state = SGXD_LOST;
            SGXD_LOG("suspend while owned: our side down, lost until the next take\n");
        }
        ksceKernelUnlockMutex(sgxd_mutex, 1);
    }
    return 0;
}

static int sgxd_enter(void) {
    if (sgxd_mutex < 0) {
        sgxd_mutex = ksceKernelCreateMutex("SceSgxdMutex", 0, 0, NULL);
        if (sgxd_mutex < 0) {
            return AZAHAR_ERR_ALLOC;
        }
    }
    ksceKernelLockMutex(sgxd_mutex, 1, NULL);
    return 0;
}

static void sgxd_leave(void) {
    ksceKernelUnlockMutex(sgxd_mutex, 1);
}

static int sgxd_take_locked(SgxdInfo *info) {
    const uint32_t t0 = ksceKernelGetSystemTimeLow();
    int r;

    info->step = SGXD_STEP_IDENTIFY;
    if ((r = sgxd_identify(info)) < 0) {
        return r;
    }
    if (sgxd_sysevent_uid < 0) {
        r = ksceKernelRegisterSysEventHandler("SceSgxd", sgxd_sysevent, NULL);
        SGXD_LOG("sysevent handler: 0x%08x\n", (uint32_t)r);
        sgxd_sysevent_uid = r;
    }
    if (azahar_proc_handler < 0) {
        azahar_proc_handler = ksceKernelRegisterProcEventHandler("azaharnative", &azahar_proc_events, 0);
    }

    info->step = SGXD_STEP_GATE;
    info->step_status = sgxd_gpu_sysevent(0, 0x400, NULL, NULL);

    info->step = SGXD_STEP_QUIESCE;
    for (info->quiesce_tries = 1;; info->quiesce_tries++) {
        info->step_status = sgxd_gpu_sysevent(0, 0x402, NULL, NULL);
        if (info->step_status == 0) {
            break;
        }
        if (info->quiesce_tries >= SGXD_QUIESCE_TRIES) {
            sgxd_hand_back();
            return SGXD_ERR_BUSY;
        }
        ksceKernelDelayThread(1000);
    }

    info->step = SGXD_STEP_POWER_OFF;
    info->step_status = sgxd_gpu_sysevent(0, 0x4005, NULL, NULL);

    info->step = SGXD_STEP_POWER_ON;
    sgxd_power_on();
    if ((r = sgxd_map()) < 0) {
        info->step_status = r;
        sgxd_power_off();
        sgxd_hand_back();
        return AZAHAR_ERR_ALLOC;
    }

    info->step = SGXD_STEP_START;
    sgxd_start_sequence();
    sgxd_reset_cores();

    info->step = SGXD_STEP_VERIFY;
    info->core_id = sgxd_read(SGX_CORE_ID);
    info->core_revision = sgxd_read(SGX_CORE_REVISION);
    info->cores = (sgxd_read(SGX_MASTER_CORE_CONFIG) & 3u) + 1u;
    info->bif_int_stat = sgxd_read(SGX_BIF_INT_STAT);
    info->bif_fault = sgxd_read(SGX_BIF_FAULT);
    info->master_bif_fault = sgxd_read(SGX_MASTER_BIF_FAULT);
    info->take_us = ksceKernelGetSystemTimeLow() - t0;
    if (info->core_id != SGXD_EXPECT_CORE_ID || info->core_revision != SGXD_EXPECT_CORE_REVISION) {
        info->step_status = (int32_t)info->core_id;
        sgxd_hw_down();
        sgxd_hand_back();
        return SGXD_ERR_HW;
    }
    info->step = SGXD_STEP_DONE;
    info->step_status = 0;
    return 0;
}

int sgxdTake(SgxdInfo *user_info) {
    uint32_t state;
    ENTER_SYSCALL(state);
    SgxdInfo info = {0};
    int ret = sgxd_enter();
    if (ret < 0) {
        goto out;
    }
    const SceUID pid = ksceKernelGetProcessId();
    if (sgxd_state == SGXD_OWNED || (sgxd_state == SGXD_LOST && pid != sgxd_owner)) {
        ret = pid == sgxd_owner ? AZAHAR_ERR_STATE : SGXD_ERR_OWNER;
    } else {
        ret = sgxd_take_locked(&info);
        if (ret == 0) {
            sgxd_state = SGXD_OWNED;
            sgxd_owner = pid;
        } else {
            sgxd_state = SGXD_NONE;
            sgxd_owner = -1;
        }
    }
    SGXD_LOG("take -> %d: step %u status %08x, core id %08x revision %08x, %u cores, "
             "quiesce %u, bif %08x/%08x/%08x, %u us\n",
             ret, info.step, (uint32_t)info.step_status, info.core_id, info.core_revision,
             info.cores, info.quiesce_tries, info.bif_int_stat, info.bif_fault,
             info.master_bif_fault, info.take_us);
    sgxd_leave();
out:
    info.size = sizeof(info);
    if (user_info) {
        ksceKernelCopyToUser(user_info, &info, sizeof(info));
    }
    EXIT_SYSCALL(state);
    return ret;
}

// Shared by the call and a dead owner.
static int sgxd_release_locked(void) {
    if (sgxd_state == SGXD_NONE) {
        return AZAHAR_ERR_STATE;
    }
    if (sgxd_state == SGXD_OWNED) {
        sgxd_hw_down();
        sgxd_hand_back();
    }
    // LOST: the system's resume already gave SceGpu its GPU back.
    const int was = sgxd_state;
    sgxd_state = SGXD_NONE;
    sgxd_owner = -1;
    SGXD_LOG("release (%s)\n", was == SGXD_OWNED ? "handed back" : "was lost");
    return 0;
}

int sgxdRelease(void) {
    uint32_t state;
    ENTER_SYSCALL(state);
    int ret = sgxd_enter();
    if (ret == 0) {
        ret = sgxd_owner == ksceKernelGetProcessId() ? sgxd_release_locked() : SGXD_ERR_OWNER;
        sgxd_leave();
    }
    EXIT_SYSCALL(state);
    return ret;
}

// The owner died without releasing: azahar_on_proc_gone calls this first.
static void sgxd_on_proc_gone(SceUID pid) {
    if (pid != sgxd_owner || sgxd_mutex < 0) {
        return;
    }
    ksceKernelLockMutex(sgxd_mutex, 1, NULL);
    if (pid == sgxd_owner) {
        SGXD_LOG("owner process gone\n");
        sgxd_release_locked();
    }
    ksceKernelUnlockMutex(sgxd_mutex, 1);
}

// Returns the number of ops done, or a negative error; a bad op stops the list there.
int sgxdSubmit(SgxdOp *user_ops, uint32_t count) {
    uint32_t state;
    ENTER_SYSCALL(state);
    int ret = sgxd_enter();
    if (ret < 0) {
        goto out;
    }
    if (sgxd_owner != ksceKernelGetProcessId()) {
        ret = SGXD_ERR_OWNER;
    } else if (sgxd_state == SGXD_LOST) {
        ret = SGXD_ERR_LOST;
    } else if (count > SGXD_MAX_OPS ||
               ksceKernelCopyFromUser(sgxd_ops, user_ops, count * sizeof(SgxdOp)) < 0) {
        ret = AZAHAR_ERR_ARG;
    } else {
        uint32_t i;
        for (i = 0; i < count; i++) {
            SgxdOp *op = &sgxd_ops[i];
            if (op->op == SGXD_OP_READ && !(op->reg & 3u) && op->reg < SGXD_REG_SIZE) {
                op->value = sgxd_read(op->reg);
            } else if (op->op == SGXD_OP_DELAY && op->value <= 1000000u) {
                ksceKernelDelayThread(op->value);
            } else {
                break;
            }
        }
        ret = ksceKernelCopyToUser(user_ops, sgxd_ops, count * sizeof(SgxdOp)) < 0
                  ? AZAHAR_ERR_ARG : (int)i;
    }
    sgxd_leave();
out:
    EXIT_SYSCALL(state);
    return ret;
}

int sgxdDump(uint32_t *user_words, uint32_t count) {
    uint32_t state;
    ENTER_SYSCALL(state);
    int ret = sgxd_enter();
    if (ret < 0) {
        goto out;
    }
    if (sgxd_owner != ksceKernelGetProcessId()) {
        ret = SGXD_ERR_OWNER;
    } else if (sgxd_state == SGXD_LOST) {
        ret = SGXD_ERR_LOST;
    } else if (count < SGXD_DUMP_WORDS) {
        ret = AZAHAR_ERR_ARG;
    } else {
        uint32_t *w = sgxd_dump_words;
        for (uint32_t i = 0; i < SGXD_DUMP_WORDS; i++) {
            w[i] = 0;
        }
        w[0] = SGXD_DUMP_MAGIC;
        w[1] = SGXD_VERSION;
        w[10] = (sgxd_read(SGX_MASTER_CORE_CONFIG) & 3u) + 1u;
        w[11] = SGXD_DUMP_BANK_REGS;
        w[12] = SGXD_DUMP_CORE_REGS;
        for (uint32_t i = 0; i < SGXD_DUMP_BANK_REGS; i++) {
            const uint32_t reg = sgxd_bank_list[i];
            w[14 + i * 2] = reg;
            w[15 + i * 2] = reg < SGXD_REG_SIZE && !(reg & 3u) ? sgxd_read(reg) : 0xdeadbeefu;
        }
        for (uint32_t i = 0; i < SGXD_DUMP_CORE_REGS; i++) {
            const uint32_t reg = sgxd_core_list[i];
            w[546 + i * 5] = reg;
            for (uint32_t core = 0; core < w[10] && core < SGXD_CORES; core++) {
                w[547 + i * 5 + core] = reg < 0x4000u && !(reg & 3u)
                                            ? sgxd_read(SGXD_CORE_BANK(core) + reg) : 0xdeadbeefu;
            }
        }
        ret = ksceKernelCopyToUser(user_words, w, SGXD_DUMP_WORDS * 4) < 0 ? AZAHAR_ERR_ARG
                                                                           : (int)SGXD_DUMP_WORDS;
    }
    sgxd_leave();
out:
    EXIT_SYSCALL(state);
    return ret;
}
