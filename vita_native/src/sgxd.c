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
//   sgxdSubmit   an op list against the registers: reads, writes, polls and delays.
//   sgxdEvents   IRQ 52 from SceGpu's handler while we own the GPU: the app's microkernel runs
//                the async lists on the GPU and raises SW_EVENT for its wakes and faults.
//   sgxdKick     core 0's KICK2, which starts the microkernel.
//   sgxdDump     Sony's own register lists, read out of SceGpuEs4's data segment.
//   sgxdMap      the app's pages into the GPU's address space: one directory and its page
//                tables in a kernel block, the same directory for every requestor.
//   sgxdCacheRange  the CPU's data cache over the app's range.
//
// The renders themselves are the app's op lists (src/sgxd, sgxd_session): synchronous ones the
// kernel checks and runs, async ones the app's microkernel runs on the GPU
// (vita_native/src/sgxd_mk.h).

#include <psp2kern/kernel/cpu/spinlock.h>
#include <psp2kern/kernel/intrmgr.h>
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
#define SGX_EVENT_STATUS2 0x0118u
#define SGX_EVENT_STATUS 0x012cu
#define SGX_EVENT_HOST_ENABLE 0x0130u
#define SGX_EVENT_HOST_CLEAR 0x0134u
#define SGX_SW_EVENT 0x00004000u
// Core 0's KICK2: starts core 0's event program (IR0 0x10000). Sony's per-command EVENT_KICK
// (0x8ac8, after its kicker word) started no run of sgxd's microkernel; KICK2 did.
#define SGX_CORE0_KICK2 0x8ac0u
#define SGX_BIF_CTRL 0x0c00u
#define SGX_BIF_INT_STAT 0x0c04u
#define SGX_BIF_FAULT 0x0c08u
#define SGX_BIF_CTRL_INVAL 0x0c34u
#define SGX_BIF_INVAL_ALL 0x8u
#define SGX_BIF_DIR_LIST_BASE7 0x0c50u
#define SGX_BIF_BANK_SET 0x0c74u
#define SGX_BIF_BANK0 0x0c78u
#define SGX_BIF_DIR_LIST_BASE0 0x0c84u
#define SGX_MASTER_CORE_CONFIG 0x4000u
#define SGX_MASTER_EVENT_STATUS 0x412cu
#define SGX_MASTER_EVENT_HOST_ENABLE 0x4130u
#define SGX_MASTER_EVENT_HOST_CLEAR 0x4134u
#define SGX_MASTER_BIF_INT_STAT 0x4c04u
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
// SceGpu's host interrupt: registered as "SceGpuLisr" for interrupt 0x34 with its
// device, priority 0x50, every core.
#define SGXD_GPU_LISR 0x715Au         // text, Thumb
#define SGXD_GPU_INTR_CODE 0x41C8u    // data
#define SGXD_GPU_DEVICE 0x41E0u       // data
#define SGXD_INTR 0x34
#define SGXD_LOWIO_PERVASIVE 0xE692C727u
#define SGXD_GPU_POWER_ON 0x84EAB5BCu
#define SGXD_GPU_POWER_OFF 0xE8656ADFu

// The registers ops may name: the master bank and the four cores' banks.
#define SGXD_OP_REG_LIMIT 0x18000u

// The GPU's page directory and its page tables, in one physically contiguous uncached block.
#define SGXD_MMU_SIZE ((1u + SGXD_MAP_TABLES) * 0x1000u)
#define SGXD_PTE_VALID 0x1u

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
static SceKernelIntrHandler sgxd_gpu_lisr;
static const uint32_t *sgxd_gpu_device;   // SceGpu's device pointer, its handler's context

// sgxdEvents: the app's page, mapped for the ISR, and whether our handler is registered.
static SceUID sgxd_events_uid = -1;
static SgxdEvents *sgxd_events;
static int sgxd_isr_ours;

static SceUID sgxd_reg_uid = -1;
static volatile uint32_t *sgxd_reg;

static SceUID sgxd_mmu_uid = -1;
static volatile uint32_t *sgxd_mmu;        // [0, 1024) the directory, then the page tables
static uint32_t sgxd_mmu_pa;
static uint8_t sgxd_mmu_table[1024];       // directory index -> page table 1..255, 0 = none
static unsigned sgxd_mmu_tables;           // page tables handed out
// What sgxdMap mapped, GPU address onto the app's (native.c's windows): a page move finds the
// PTEs naming a page from these (reback.c).
#define SGXD_RANGES 128u
static AzaharWindow sgxd_range_list[SGXD_RANGES];
static AzaharWindows sgxd_ranges = {sgxd_range_list, 0, SGXD_RANGES};
static uint32_t sgxd_mmu_pages;            // pages mapped

static SgxdOp sgxd_ops[SGXD_MAX_OPS];
// sgxdWaker: the app's event flag (its kernel UID) and the bits to set, under sgxd_isr_lock.
static SceUID sgxd_wake_flag = -1;
static uint32_t sgxd_wake_bits;
static SceKernelSpinlock sgxd_isr_lock;
static uint32_t sgxd_dump_words[SGXD_DUMP_WORDS];
// The dump sgxdSubmit took when a poll last timed out (sgxdFaultDump); [0] 0 when none.
static uint32_t sgxd_fault_words[SGXD_DUMP_WORDS];
static uint32_t sgxd_read_words[SGXD_MAX_READ_REGS];

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
    if (*(const uint32_t *)(data + SGXD_GPU_INTR_CODE) == SGXD_INTR) {
        sgxd_gpu_lisr = (SceKernelIntrHandler)(text + SGXD_GPU_LISR + 1);
        sgxd_gpu_device = (const uint32_t *)(data + SGXD_GPU_DEVICE);
    } else {
        SGXD_LOG("SceGpu's interrupt code is %08x, not 0x34: no sgxdEvents\n",
                 *(const uint32_t *)(data + SGXD_GPU_INTR_CODE));
    }
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

// IRQ 52: bank 0's SW_EVENT, which the app's microkernel raises for a wake or a fault; the
// master's completions start the microkernel, not us. Bank 0's event registers read as the
// master's (both statuses 0x80004000 and both host enables the same at once),
// so the host enable is SW_EVENT alone and stays so.
static int sgxd_isr(int unk, void *ctx) {
    (void)unk;
    (void)ctx;
    SgxdEvents *const ev = sgxd_events;
    if (!ev || !sgxd_reg) {
        return -1;
    }
    ksceKernelSpinlockLowLock(&sgxd_isr_lock);
    SceUID wake = -1;
    ev->irqs++;
    const uint32_t status = sgxd_read(SGX_MASTER_EVENT_STATUS);
    if (status & SGX_SW_EVENT) {
        sgxd_write(SGX_EVENT_HOST_CLEAR, SGX_SW_EVENT);
        ev->sw_events++;
        wake = sgxd_wake_flag;
    } else {
        ev->unexpected |= status;
    }
    const uint32_t bits = sgxd_wake_bits;
    ksceKernelSpinlockLowUnlock(&sgxd_isr_lock);
    // As SceGpuEs4's own interrupt handler wakes its thread.
    if (wake >= 0) {
        ksceKernelSetEventFlag(wake, bits);
    }
    return -1;
}

// SceGpu's handler out, ours in; the interrupt enabled. SceGpu's suspend left it disabled.
static int sgxd_isr_take(void) {
    if (sgxd_isr_ours) {
        return 0;
    }
    ksceKernelDisableIntr(SGXD_INTR);
    int r = ksceKernelReleaseIntrHandler(SGXD_INTR);
    if (r < 0) {
        SGXD_LOG("release of SceGpuLisr: %08x\n", (uint32_t)r);
        return r;
    }
    r = ksceKernelRegisterIntrHandler(SGXD_INTR, "SceSgxdLisr", 0, sgxd_isr, NULL, 0x50, 0xf, NULL);
    if (r < 0) {
        SGXD_LOG("register of SceSgxdLisr: %08x\n", (uint32_t)r);
        ksceKernelRegisterIntrHandler(SGXD_INTR, "SceGpuLisr", 0, sgxd_gpu_lisr,
                                      (void *)*sgxd_gpu_device, 0x50, 0xf, NULL);
        return r;
    }
    sgxd_isr_ours = 1;
    ksceKernelEnableIntr(SGXD_INTR);
    return 0;
}

// The interrupt back to SceGpu's handler, disabled, as its suspend left it (its resume enables
// it); the app's page unmapped.
static void sgxd_isr_give_back(void) {
    const SceKernelIntrStatus st = ksceKernelSpinlockLowLockCpuSuspendIntr(&sgxd_isr_lock);
    sgxd_wake_flag = -1;
    if (sgxd_reg) {
        sgxd_write(SGX_MASTER_EVENT_HOST_ENABLE, 0);
    }
    ksceKernelSpinlockLowUnlockCpuResumeIntr(&sgxd_isr_lock, st);
    if (sgxd_isr_ours) {
        ksceKernelDisableIntr(SGXD_INTR);
        ksceKernelReleaseIntrHandler(SGXD_INTR);
        const int r = ksceKernelRegisterIntrHandler(SGXD_INTR, "SceGpuLisr", 0, sgxd_gpu_lisr,
                                                    (void *)*sgxd_gpu_device, 0x50, 0xf, NULL);
        if (r < 0) {
            SGXD_LOG("SceGpuLisr back: %08x\n", (uint32_t)r);
        }
        sgxd_isr_ours = 0;
    }
    sgxd_events = NULL;
    if (sgxd_events_uid >= 0) {
        ksceKernelUserUnmap(sgxd_events_uid);
        sgxd_events_uid = -1;
    }
}

// Our side down: the SLC written back and emptied, the cores reset, the block unmapped, the
// domain off. SceGpu's interrupt handler goes back first.
static void sgxd_hw_down(void) {
    sgxd_isr_give_back();
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

// One directory for the whole address space, 4 KiB pages: PDE = page table | VALID, PTE = page
// | flags | VALID (docs/sgxd/memory.md). The
// block is uncached, so the GPU reads what the CPU wrote once a DSB has drained it.

static int sgxd_mmu_alloc(void) {
    SceKernelAllocMemBlockKernelOpt opt = {0};
    opt.size = sizeof(opt);
    opt.attr = 0x4;   // alignment given
    opt.alignment = 0x1000;
    sgxd_mmu_uid = ksceKernelAllocMemBlock("SceSgxdMmu", SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_ROOT_PHYCONT_NC_RW,
                                           SGXD_MMU_SIZE, &opt);
    if (sgxd_mmu_uid < 0) {
        return sgxd_mmu_uid;
    }
    void *base = NULL;
    uintptr_t pa = 0;
    int r = ksceKernelGetMemBlockBase(sgxd_mmu_uid, &base);
    if (r >= 0) {
        r = ksceKernelVAtoPA(base, &pa);
    }
    if (r < 0) {
        ksceKernelFreeMemBlock(sgxd_mmu_uid);
        sgxd_mmu_uid = -1;
        return r;
    }
    sgxd_mmu = base;
    sgxd_mmu_pa = (uint32_t)pa;
    for (uint32_t i = 0; i < 1024; i++) {
        sgxd_mmu[i] = 0;
    }
    asm volatile("dsb" ::: "memory");
    __builtin_memset(sgxd_mmu_table, 0, sizeof(sgxd_mmu_table));
    sgxd_mmu_tables = 0;
    sgxd_ranges.count = 0;
    sgxd_mmu_pages = 0;
    return 0;
}

static void sgxd_mmu_free(void) {
    if (sgxd_mmu_uid >= 0) {
        ksceKernelFreeMemBlock(sgxd_mmu_uid);
    }
    sgxd_mmu_uid = -1;
    sgxd_mmu = NULL;
    sgxd_mmu_pa = 0;
}

// The master bank and every core's, with cores 2 and 3 written directly.
static const uint32_t sgxd_bif_banks[] = {0x0000u, 0x4000u, 0x8000u, 0xc000u, 0x10000u, 0x14000u};
#define SGXD_BIF_BANKS (sizeof(sgxd_bif_banks) / sizeof(sgxd_bif_banks[0]))

static void sgxd_mmu_invalidate(void) {
    for (unsigned b = 0; b < SGXD_BIF_BANKS; b++) {
        sgxd_write(sgxd_bif_banks[b] + SGX_BIF_CTRL_INVAL, 0);
        sgxd_write(sgxd_bif_banks[b] + SGX_BIF_CTRL_INVAL, SGX_BIF_INVAL_ALL);
    }
}

// SceGpu's init script clears the BIF set-up and points bank 0 at directory 7; the event data
// master reads through it, everything else through directory 0. Both are ours, in every bank.
static void sgxd_mmu_program(void) {
    sgxd_write(SGX_BIF_CTRL, 0);
    sgxd_write(SGX_BIF_BANK_SET, 0);
    sgxd_write(SGX_BIF_BANK0, 7);
    for (unsigned b = 0; b < SGXD_BIF_BANKS; b++) {
        sgxd_write(sgxd_bif_banks[b] + SGX_BIF_DIR_LIST_BASE0, sgxd_mmu_pa);
        sgxd_write(sgxd_bif_banks[b] + SGX_BIF_DIR_LIST_BASE7, sgxd_mmu_pa);
    }
    sgxd_mmu_invalidate();
}

static volatile uint32_t *sgxd_mmu_pt(unsigned table) {
    return sgxd_mmu + table * 1024u;
}

static volatile uint32_t *sgxd_mmu_pte(uint32_t gpu_va) {
    const unsigned table = sgxd_mmu_table[gpu_va >> 22];
    return table ? &sgxd_mmu_pt(table)[(gpu_va >> 12) & 1023u] : NULL;
}

// A page of the app's memory as the app itself reaches it: a user-mode translation (ATS1CUR, or
// ATS1CUW for a writable GPU mapping) under the same one-page domain grant as azahar_user_page,
// so a kernel page, or a read-only one mapped writable, faults here and is refused. Then the page
// is cleaned to memory, as azahar_user_page does. ksceKernelFindProcMemBlockByAddr was not enough:
// it accepts kernel address 0x00100000.
static uint32_t sgxd_user_par(uint32_t uva) {
    SceKernelIntrStatus st = ksceKernelCpuSuspendIntr();
    const uint32_t d = rd_dacr();
    wr_dacr(dacr_grant_all(d));
    asm volatile("mcr p15, 0, %0, c7, c8, 2 \n isb" ::"r"(uva) : "memory");
    const uint32_t par = rd_par();
    wr_dacr(d);
    ksceKernelCpuResumeIntr(st);
    return par;
}

static int sgxd_user_page(uint32_t uva, int write, uint32_t *pa, uint32_t *par_out) {
    SceKernelIntrStatus st = ksceKernelCpuSuspendIntr();
    const uint32_t d = rd_dacr();
    wr_dacr(dacr_grant_all(d));
    uint32_t par = 0, base_par = 0;
    for (int k = 0; k < 2; k++) {
        const uint32_t at = k ? uva & ~0xFFFFu : uva;
        if (write) {
            asm volatile("mcr p15, 0, %0, c7, c8, 3 \n isb" ::"r"(at) : "memory");   // ATS1CUW
        } else {
            asm volatile("mcr p15, 0, %0, c7, c8, 2 \n isb" ::"r"(at) : "memory");   // ATS1CUR
        }
        *(k ? &base_par : &par) = rd_par();
    }
    par = par_large_page(uva, par, base_par);
    if (!PAR_FAULTED(par)) {
        ksceKernelDcacheCleanRange((void *)(uintptr_t)uva, 0x1000);
    }
    wr_dacr(d);
    ksceKernelCpuResumeIntr(st);
    *par_out = par;
    *pa = PAR_PA(par);
    return PAR_FAULTED(par) ? AZAHAR_ERR_PAGE : 0;
}

static int sgxd_map_locked(uint32_t gpu_va, uint32_t uva, uint32_t size, uint32_t flags) {
    if ((gpu_va | uva | size) & 0xfffu || !size || gpu_va + (size - 1) < gpu_va ||
        uva + (size - 1) < uva || (flags & ~SGXD_MAP_FLAGS)) {
        return AZAHAR_ERR_ARG;
    }
    unsigned need = 0;
    for (uint32_t pde = gpu_va >> 22; pde <= (gpu_va + (size - 1)) >> 22; pde++) {
        need += !sgxd_mmu_table[pde];
    }
    // A new range adjoins at most two others, so one free record is enough.
    if (sgxd_mmu_tables + need > SGXD_MAP_TABLES || sgxd_ranges.count == sgxd_ranges.cap) {
        return SGXD_ERR_TABLES;
    }
    for (uint32_t off = 0; off < size; off += 0x1000) {
        const volatile uint32_t *pte = sgxd_mmu_pte(gpu_va + off);
        if (pte && (*pte & SGXD_PTE_VALID)) {
            return SGXD_ERR_MAPPED;
        }
    }
    for (uint32_t pde = gpu_va >> 22; pde <= (gpu_va + (size - 1)) >> 22; pde++) {
        if (!sgxd_mmu_table[pde]) {
            const unsigned table = ++sgxd_mmu_tables;
            volatile uint32_t *pt = sgxd_mmu_pt(table);
            for (unsigned i = 0; i < 1024; i++) {
                pt[i] = 0;
            }
            sgxd_mmu_table[pde] = (uint8_t)table;
            sgxd_mmu[pde] = (sgxd_mmu_pa + table * 0x1000u) | SGXD_PTE_VALID;
        }
    }
    for (uint32_t off = 0; off < size; off += 0x1000) {
        uint32_t pa = 0, par = 0;
        if (sgxd_user_page(uva + off, !(flags & SGXD_MAP_READONLY), &pa, &par) != 0) {
            SGXD_LOG("map: user %08x does not translate (par %08x)\n", uva + off, par);
            for (uint32_t undo = 0; undo < off; undo += 0x1000) {
                *sgxd_mmu_pte(gpu_va + undo) = 0;
            }
            sgxd_mmu_pages -= off / 0x1000;
            asm volatile("dsb" ::: "memory");
            return AZAHAR_ERR_PAGE;
        }
        *sgxd_mmu_pte(gpu_va + off) = pa | flags | SGXD_PTE_VALID;
        sgxd_mmu_pages++;
    }
    windows_add(&sgxd_ranges, gpu_va, uva, size);
    asm volatile("dsb" ::: "memory");
    sgxd_mmu_invalidate();
    return 0;
}

static int sgxd_unmap_locked(uint32_t gpu_va, uint32_t size) {
    if ((gpu_va | size) & 0xfffu || !size || gpu_va + (size - 1) < gpu_va) {
        return AZAHAR_ERR_ARG;
    }
    for (uint32_t off = 0; off < size; off += 0x1000) {
        const volatile uint32_t *pte = sgxd_mmu_pte(gpu_va + off);
        if (!pte || !(*pte & SGXD_PTE_VALID)) {
            return SGXD_ERR_MAPPED;
        }
    }
    // Only an unmap from the middle of a range needs a record more.
    if (windows_remove(&sgxd_ranges, gpu_va, size) != 0) {
        return SGXD_ERR_TABLES;
    }
    for (uint32_t off = 0; off < size; off += 0x1000) {
        *sgxd_mmu_pte(gpu_va + off) = 0;
    }
    sgxd_mmu_pages -= size / 0x1000;
    asm volatile("dsb" ::: "memory");
    sgxd_mmu_invalidate();
    return 0;
}

// A write sgxd allows: not the BIF directory, bank and MMU-invalidate registers of any bank.
static int sgxd_writable(uint32_t reg) {
    const uint32_t r = reg & 0x3fffu;
    return !(reg & 3u) && reg < SGXD_OP_REG_LIMIT && r != SGX_BIF_CTRL &&
           !(r >= SGX_BIF_CTRL_INVAL && r <= SGX_BIF_DIR_LIST_BASE0);
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
    // A first take starts with no GPU mappings. A take after a suspend (LOST) keeps them: the
    // directory and the pages stayed in RAM, and the start sequence programs the directory
    // again (docs/sgxd/takeover.md). Freed at release.
    if (sgxd_mmu_uid < 0 && (r = sgxd_mmu_alloc()) < 0) {
        info->step_status = r;
        return AZAHAR_ERR_ALLOC;
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
    sgxd_mmu_program();

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
            sgxd_mmu_free();
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
    const uint32_t pages = sgxd_mmu_pages;
    sgxd_mmu_free();
    sgxd_state = SGXD_NONE;
    sgxd_owner = -1;
    SGXD_LOG("release (%s), %u GPU page(s) were mapped\n", was == SGXD_OWNED ? "handed back" : "was lost",
             pages);
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

// sgxdDump's layout: SceGpuEs4's two register lists, read now. Header words 2-9 are the
// caller's (the fault snapshot's).
static void sgxd_fill_dump(uint32_t *w) {
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
    // The debug bus, as SGXDumpDebugState reads it: each core's 0x78 set to 1, each ID
    // selected through 0x90 and 0x94 and its word read at 0x70; the three put back after.
    const uint32_t cores = w[10] < SGXD_CORES ? w[10] : SGXD_CORES;
    uint32_t saved[SGXD_CORES][3];
    for (uint32_t core = 0; core < cores; core++) {
        const uint32_t bank = SGXD_CORE_BANK(core);
        saved[core][0] = sgxd_read(bank + 0x78u);
        saved[core][1] = sgxd_read(bank + 0x90u);
        saved[core][2] = sgxd_read(bank + 0x94u);
        sgxd_write(bank + 0x78u, 1);
    }
    for (uint32_t i = 0; i < SGXD_DUMP_DEBUG_IDS; i++) {
        const uint32_t id = sgxd_debug_ids[i] & 0xffu, sel = id | id << 16;
        w[SGXD_DUMP_DEBUG_AT + i * 5] = id;
        for (uint32_t core = 0; core < cores; core++) {
            const uint32_t bank = SGXD_CORE_BANK(core);
            sgxd_write(bank + 0x90u, sel | 0x08000000u);
            sgxd_write(bank + 0x94u, sel | 0x18001000u);
            w[SGXD_DUMP_DEBUG_AT + 1 + i * 5 + core] = sgxd_read(bank + 0x70u);
        }
    }
    for (uint32_t core = 0; core < cores; core++) {
        const uint32_t bank = SGXD_CORE_BANK(core);
        sgxd_write(bank + 0x90u, saved[core][1]);
        sgxd_write(bank + 0x94u, saved[core][2]);
        sgxd_write(bank + 0x78u, saved[core][0]);
    }
}

// A poll gave up: the whole dump at once, before anything else touches the GPU, and the
// status registers a hang is read by on the console, which outlives the app.
static void sgxd_fault_snapshot(uint32_t index, const SgxdOp *op, uint32_t wanted,
                                uint32_t waited) {
    uint32_t *w = sgxd_fault_words;
    sgxd_fill_dump(w);
    w[2] = SGXD_DUMP_FAULT;
    w[3] = index;
    w[4] = op->reg;
    w[5] = wanted;
    w[6] = op->mask;
    w[7] = op->value;
    w[8] = waited;
    w[9] = ksceKernelGetSystemTimeLow();
    w[13] = op->op;
    SGXD_LOG("poll timeout: op %u reg %05x want %08x mask %08x last %08x after %u us; "
             "fault dump taken\n", index, op->reg, wanted, op->mask, op->value, waited);
    for (uint32_t core = 0; core < SGXD_CORES; core++) {
        const uint32_t b = SGXD_CORE_BANK(core);
        SGXD_LOG("core %u: event status %08x/%08x, bif int %08x fault %08x\n", core,
                 sgxd_read(b + SGX_EVENT_STATUS), sgxd_read(b + SGX_EVENT_STATUS2),
                 sgxd_read(b + SGX_BIF_INT_STAT), sgxd_read(b + SGX_BIF_FAULT));
    }
    SGXD_LOG("bank 0: event status %08x/%08x, bif int %08x fault %08x; master bif int %08x "
             "fault %08x\n", sgxd_read(SGX_EVENT_STATUS), sgxd_read(SGX_EVENT_STATUS2),
             sgxd_read(SGX_BIF_INT_STAT), sgxd_read(SGX_BIF_FAULT),
             sgxd_read(SGX_MASTER_BIF_INT_STAT), sgxd_read(SGX_MASTER_BIF_FAULT));
}

// Returns the number of ops done, or a negative error. A bad op, or a poll that timed out, stops
// the list there: the return is its index, and a poll's value is the register's last read.
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
            } else if (op->op == SGXD_OP_WRITE && sgxd_writable(op->reg)) {
                sgxd_write(op->reg, op->value);
            } else if (op->op == SGXD_OP_POLL && !(op->reg & 3u) && op->reg < SGXD_REG_SIZE &&
                       op->timeout_us <= SGXD_MAX_POLL_US) {
                // Spins for the first 100 us, the TA and 3D kicks' own scale, then sleeps. The
                // time is taken before each read, so a wake-up past the timeout still reads the
                // register once more (a slow wake would time out on an SLC flush that had
                // completed).
                const uint32_t t0 = ksceKernelGetSystemTimeLow();
                uint32_t v, waited;
                for (;;) {
                    waited = ksceKernelGetSystemTimeLow() - t0;
                    v = sgxd_read(op->reg);
                    if ((v & op->mask) == op->value || waited > op->timeout_us) {
                        break;
                    }
                    if (waited > 100u) {
                        ksceKernelDelayThread(20);
                    }
                }
                const int met = (v & op->mask) == op->value;
                const uint32_t wanted = op->value;
                op->value = v;
                if (!met) {
                    sgxd_fault_snapshot(i, op, wanted, waited);
                    break;
                }
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

int sgxdWaker(int32_t user_flag, uint32_t bits) {
    uint32_t state;
    ENTER_SYSCALL(state);
    int ret = sgxd_enter();
    if (ret < 0) {
        goto out;
    }
    if (sgxd_owner != ksceKernelGetProcessId()) {
        ret = SGXD_ERR_OWNER;
    } else if (!sgxd_events) {
        ret = AZAHAR_ERR_STATE;
    } else {
        SceUID flag = -1;
        ret = 0;
        if (user_flag >= 0) {
            flag = kscePUIDtoGUID(ksceKernelGetProcessId(), user_flag);
            if (flag < 0) {
                ret = flag;
            }
        }
        if (ret == 0) {
            const SceKernelIntrStatus st =
                ksceKernelSpinlockLowLockCpuSuspendIntr(&sgxd_isr_lock);
            sgxd_wake_flag = flag;
            sgxd_wake_bits = bits;
            ksceKernelSpinlockLowUnlockCpuResumeIntr(&sgxd_isr_lock, st);
        }
    }
    SGXD_LOG("waker %08x bits %x -> %08x\n", (uint32_t)user_flag, bits, (uint32_t)ret);
    sgxd_leave();
out:
    EXIT_SYSCALL(state);
    return ret;
}

int sgxdKick(void) {
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
    } else if (!sgxd_events) {
        ret = AZAHAR_ERR_STATE;
    } else {
        // The microkernel's queue: the app has published its jobs and set its BUSY word.
        sgxd_write(SGX_CORE0_KICK2, 1);
        ret = 0;
    }
    sgxd_leave();
out:
    EXIT_SYSCALL(state);
    return ret;
}

int sgxdEvents(SgxdEvents *user_events) {
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
    } else if (!user_events) {
        sgxd_isr_give_back();
        ret = 0;
    } else if (!sgxd_gpu_lisr || sgxd_events_uid >= 0) {
        ret = !sgxd_gpu_lisr ? SGXD_ERR_FIRMWARE : AZAHAR_ERR_STATE;
    } else {
        void *page = NULL;
        SceSize size = 0;
        SceUInt32 offset = 0;
        const SceUID uid = ksceKernelUserMap("SceSgxdEvents", 3, user_events, sizeof(SgxdEvents),
                                             &page, &size, &offset);
        if (uid < 0) {
            ret = uid;
        } else {
            sgxd_events_uid = uid;
            sgxd_events = (SgxdEvents *)((uintptr_t)page + offset);
            // The master's completions start the microkernel and interrupt no one; the host
            // takes bank 0's SW_EVENT alone.
            sgxd_write(SGX_MASTER_EVENT_HOST_ENABLE, 0);
            sgxd_write(SGX_EVENT_HOST_CLEAR, SGX_SW_EVENT);
            sgxd_write(SGX_EVENT_HOST_ENABLE, SGX_SW_EVENT);
            ret = sgxd_isr_take();
            if (ret < 0) {
                sgxd_isr_give_back();
            }
        }
    }
    SGXD_LOG("events %p -> %08x\n", user_events, (uint32_t)ret);
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
        sgxd_fill_dump(sgxd_dump_words);
        ret = ksceKernelCopyToUser(user_words, sgxd_dump_words, SGXD_DUMP_WORDS * 4) < 0
                  ? AZAHAR_ERR_ARG : (int)SGXD_DUMP_WORDS;
    }
    sgxd_leave();
out:
    EXIT_SYSCALL(state);
    return ret;
}

int sgxdFaultDump(uint32_t *user_words, uint32_t count) {
    uint32_t state;
    ENTER_SYSCALL(state);
    int ret = sgxd_enter();
    if (ret < 0) {
        goto out;
    }
    if (sgxd_owner != ksceKernelGetProcessId()) {
        ret = SGXD_ERR_OWNER;
    } else if (count < SGXD_DUMP_WORDS) {
        ret = AZAHAR_ERR_ARG;
    } else if (sgxd_fault_words[0] != SGXD_DUMP_MAGIC) {
        ret = 0;
    } else {
        ret = ksceKernelCopyToUser(user_words, sgxd_fault_words, SGXD_DUMP_WORDS * 4) < 0
                  ? AZAHAR_ERR_ARG : (int)SGXD_DUMP_WORDS;
    }
    sgxd_leave();
out:
    EXIT_SYSCALL(state);
    return ret;
}

int sgxdReadRegs(uint32_t reg, uint32_t count, uint32_t *user_words) {
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
    } else if ((reg & 3u) || !count || count > SGXD_MAX_READ_REGS || reg >= SGXD_OP_REG_LIMIT ||
               count > (SGXD_OP_REG_LIMIT - reg) / 4u) {
        ret = AZAHAR_ERR_ARG;
    } else {
        for (uint32_t i = 0; i < count; i++) {
            sgxd_read_words[i] = sgxd_read(reg + i * 4u);
        }
        ret = ksceKernelCopyToUser(user_words, sgxd_read_words, count * 4u) < 0 ? AZAHAR_ERR_ARG
                                                                               : (int)count;
    }
    sgxd_leave();
out:
    EXIT_SYSCALL(state);
    return ret;
}

int sgxdTranslate(uint32_t gpu_va, uint32_t user_out[2]) {
    uint32_t state;
    ENTER_SYSCALL(state);
    int ret = sgxd_enter();
    if (ret < 0) {
        goto out;
    }
    if (sgxd_owner != ksceKernelGetProcessId()) {
        ret = SGXD_ERR_OWNER;
    } else if (!sgxd_mmu) {
        ret = AZAHAR_ERR_ARG;
    } else {
        const volatile uint32_t *pte = sgxd_mmu_pte(gpu_va);
        const uint32_t out[2] = {sgxd_mmu[gpu_va >> 22], pte ? *pte : 0};
        ret = ksceKernelCopyToUser(user_out, out, sizeof(out)) < 0 ? AZAHAR_ERR_ARG : 0;
    }
    sgxd_leave();
out:
    EXIT_SYSCALL(state);
    return ret;
}

static int sgxd_owned_call(void) {
    if (sgxd_owner != ksceKernelGetProcessId()) {
        return SGXD_ERR_OWNER;
    }
    return sgxd_state == SGXD_LOST ? SGXD_ERR_LOST : 0;
}

int sgxdMap(uint32_t gpu_va, const void *user_va, uint32_t size, uint32_t flags) {
    uint32_t state;
    ENTER_SYSCALL(state);
    int ret = sgxd_enter();
    if (ret == 0) {
        ret = sgxd_owned_call();
        if (ret == 0) {
            ret = sgxd_map_locked(gpu_va, (uint32_t)(uintptr_t)user_va, size, flags);
        }
        SGXD_LOG("map gpu %08x <- user %08x, %08x bytes, flags %x -> %08x (%u tables, %u pages)\n",
                 gpu_va, (uint32_t)(uintptr_t)user_va, size, flags, (uint32_t)ret,
                 sgxd_mmu_tables, sgxd_mmu_pages);
        sgxd_leave();
    }
    EXIT_SYSCALL(state);
    return ret;
}

int sgxdUnmap(uint32_t gpu_va, uint32_t size) {
    uint32_t state;
    ENTER_SYSCALL(state);
    int ret = sgxd_enter();
    if (ret == 0) {
        ret = sgxd_owned_call();
        if (ret == 0) {
            ret = sgxd_unmap_locked(gpu_va, size);
        }
        sgxd_leave();
    }
    EXIT_SYSCALL(state);
    return ret;
}

// By the app's addresses, under the same one-page domain grant as azahar_user_page: the app's
// pages are No-access to the kernel inside a syscall. 16 KiB at a time with interrupts off.
int sgxdCacheRange(const void *user_va, uint32_t size, uint32_t op) {
    uint32_t state;
    ENTER_SYSCALL(state);
    const uint32_t uva = (uint32_t)(uintptr_t)user_va;
    int ret = 0;
    // An invalidate takes whole 64-byte lines only: a partial one would drop the rest of its line.
    if (!size || uva + (size - 1) < uva || op < SGXD_CACHE_CLEAN || op > SGXD_CACHE_CLEAN_INVALIDATE ||
        (op == SGXD_CACHE_INVALIDATE && ((uva | size) & 63u))) {
        ret = AZAHAR_ERR_ARG;
    } else {
        // Only memory the app reads itself (sgxd_user_page's check): an invalidate of a kernel
        // line would drop the kernel's data.
        for (uint32_t page = uva & ~0xfffu; page - (uva & ~0xfffu) < size + (uva & 0xfffu); page += 0x1000) {
            if (PAR_FAULTED(sgxd_user_par(page))) {
                ret = AZAHAR_ERR_PAGE;
                break;
            }
        }
    }
    if (ret == 0) {
        uint32_t at = uva & ~63u;
        const uint32_t end = uva + size;
        while (at < end) {
            const uint32_t n = end - at < 0x4000u ? ((end - at) + 63u) & ~63u : 0x4000u;
            SceKernelIntrStatus st = ksceKernelCpuSuspendIntr();
            const uint32_t d = rd_dacr();
            wr_dacr(dacr_grant_all(d));
            if (op == SGXD_CACHE_CLEAN) {
                ksceKernelDcacheCleanRange((void *)(uintptr_t)at, n);
            } else if (op == SGXD_CACHE_INVALIDATE) {
                ksceKernelDcacheInvalidateRange((void *)(uintptr_t)at, n);
            } else {
                ksceKernelDcacheCleanInvalidateRange((void *)(uintptr_t)at, n);
            }
            wr_dacr(d);
            ksceKernelCpuResumeIntr(st);
            at += n;
        }
    }
    EXIT_SYSCALL(state);
    return ret;
}
