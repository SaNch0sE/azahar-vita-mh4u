// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// sgxd: the ABI between azaharnative's direct-SGX calls and the app. Included by both.
// docs/sgxd/takeover.md describes the take and this file.

#ifndef SGXD_H
#define SGXD_H

#include <stdint.h>

#include "azahar_native.h"

// What a take reports. Filled on success and, as far as it got, on failure.
typedef struct SgxdInfo {
    uint32_t size;           // in: sizeof(SgxdInfo)
    uint32_t module_nid;     // SceGpuEs4's module NID (0xC0F361A3 on 3.65)
    uint32_t step;           // SGXD_STEP_*: the last step started; on failure, the one that failed
    int32_t step_status;     // that step's own status (a kernel error or a register value)
    uint32_t quiesce_tries;  // (0, 0x402) calls until SceGpu reported no bridge call inside
    uint32_t take_us;        // the whole take, microseconds
    uint32_t core_id;        // CORE_ID 0x0020 (0x01174403)
    uint32_t core_revision;  // CORE_REVISION 0x0024 (0x00020106)
    uint32_t cores;          // (0x4000 & 3) + 1, as Sony's own register dump computes it
    uint32_t bif_int_stat;   // 0x0c04 after the start sequence
    uint32_t bif_fault;      // 0x0c08
    uint32_t master_bif_fault; // 0x4c08
} SgxdInfo;

#define SGXD_STEP_IDENTIFY 1   // SceGpuEs4 and ScePervasive's GPU power calls
#define SGXD_STEP_GATE 2       // (0, 0x400): SceGpu's suspend gate
#define SGXD_STEP_QUIESCE 3    // (0, 0x402) until 0
#define SGXD_STEP_POWER_OFF 4  // (0, 0x4005): SceGpu to power state 3
#define SGXD_STEP_POWER_ON 5   // ScePervasive's GPU power-on, then the register block
#define SGXD_STEP_START 6      // the start sequence and the per-core cache reset
#define SGXD_STEP_VERIFY 7     // CORE_ID and CORE_REVISION
#define SGXD_STEP_DONE 8

// One step of an sgxdSubmit list. Registers are offsets from 0xE8400000, 4-byte aligned, below
// 0x18000 (the master bank and the four cores' banks). Writes skip the BIF directory, bank and
// MMU-invalidate registers, which sgxd owns (sgxdMap).
typedef struct SgxdOp {
    uint32_t op;          // SGXD_OP_*
    uint32_t reg;         // register offset
    uint32_t value;       // READ: out; WRITE: in; POLL: the wanted value in, the last read out;
                          // DELAY: microseconds
    uint32_t mask;        // POLL: the bits compared
    uint32_t timeout_us;  // POLL: at most 1 s
} SgxdOp;

#define SGXD_OP_READ 1u
#define SGXD_OP_DELAY 2u
#define SGXD_OP_WRITE 3u
#define SGXD_OP_POLL 4u   // until (reg & mask) == value; a timeout stops the list there
// The async lists' own ops, which the app's microkernel compiles (vita_native/src/sgxd_mk.h);
// sgxdSubmit refuses them. WAIT_TWOD: until the microkernel's PTLA count reaches value.
// WAIT_EVENT: until MASTER_EVENT_STATUS has a bit of mask (TA_FINISHED 0x2000 or
// PIXELBE_END_RENDER 0x40000), then the bit cleared through 0x4134. SIGNAL counts (value
// SGXD_SIGNAL_*).
#define SGXD_OP_WAIT_TWOD 5u
#define SGXD_OP_WAIT_EVENT 6u
#define SGXD_OP_SIGNAL 7u
#define SGXD_SIGNAL_TA 0u      // the render's vertices are read
#define SGXD_SIGNAL_DONE 1u    // the list is complete
// The two streams' (sgxd_mk.h): a TA kicked (TA_BUSY), a scene binned and its TA side done
// (TA_BUSY cleared, TA_SCENES counted), a 3D kicked (D3_KICKS counted).
#define SGXD_SIGNAL_TA_START 2u
#define SGXD_SIGNAL_TA_SCENE 3u
#define SGXD_SIGNAL_D3_KICK 4u
// STORE (an async list's, version 14): the word `value` stored at GPU address `mask`, both from
// the job's data slot, the store fenced. For a page table entry the GPU itself rewrites, in the
// queue's order (sgxdMapTables, sgxdPte), before the MMU invalidate after it.
#define SGXD_OP_STORE 8u
// The two streams' waits (sgxd_mk.h): WAIT_COUNT until the microkernel's state word `reg` reaches
// value (the job's, as WAIT_TWOD's); WAIT_TA_IDLE until no TA is in flight; WAIT_PAGES until the
// TA context has at most `value` pages (PAGE_STATUS's high half), or no binned scene waits for
// its 3D.
// STREAM_3D splits a job's list: the TA stream runs the ops before it, the 3D stream the ops
// after it.
#define SGXD_OP_WAIT_COUNT 9u
#define SGXD_OP_WAIT_TA_IDLE 10u
#define SGXD_OP_WAIT_PAGES 11u
#define SGXD_OP_STREAM_3D 12u
#define SGXD_MAX_OPS 256u
// An async list's ops at most (the app's rule).
#define SGXD_MAX_CHAIN_OPS 1024u
#define SGXD_MAX_POLL_US 1000000u

// sgxdMap: the app's pages into the GPU's address space, 4 KiB pages, one directory for every
// requestor. A GPU address's PTE is the page's physical address | flags | VALID. sgxdMap and
// sgxdUnmap run the MMU invalidate themselves: call them with the GPU idle.
#define SGXD_MAP_READONLY 0x4u         // PTE READONLY
#define SGXD_MAP_CACHECONSISTENT 0x8u  // PTE CACHECONSISTENT (sgxd_test 02 looks)
#define SGXD_MAP_FLAGS (SGXD_MAP_READONLY | SGXD_MAP_CACHECONSISTENT)
#define SGXD_MAP_TABLES 255u  // page tables: 4 MiB of GPU addresses each, 1020 MiB in all

// sgxdCacheRange: the CPU's data cache over the app's own range, by the app's addresses.
#define SGXD_CACHE_CLEAN 1u       // before the GPU reads what the CPU wrote
#define SGXD_CACHE_INVALIDATE 2u  // before the CPU reads what the GPU wrote; 64-byte lines only
#define SGXD_CACHE_CLEAN_INVALIDATE 3u

// sgxdDump's layout follows SceGpuEs4's own register dump (SGXDumpDebugState):
// reading Sony's three lists out of SceGpuEs4's data segment:
//   [0] SGXD_DUMP_MAGIC  [1] SGXD_VERSION  [10] cores  [11] bank registers (266)
//   [12] per-core registers (399)  [14 + 2i] offset, value  [546 + 5i] offset, core 0..3
//   [2541 + 5i] debug-bus select ID, core 0..3 (109); ID 0x25 bit 4 is Sony's clipper-bug class
#define SGXD_DUMP_MAGIC 0x44584753u   // "SGXD"
#define SGXD_DUMP_DEBUG_AT 2541u
#define SGXD_DUMP_DEBUG_IDS 109u
#define SGXD_DUMP_WORDS (SGXD_DUMP_DEBUG_AT + SGXD_DUMP_DEBUG_IDS * 5u)
#define SGXD_DUMP_BANK_REGS 266u
#define SGXD_DUMP_CORE_REGS 399u

#define SGXD_VERSION 15

// sgxdEvents (version 4; 13: the microkernel's alone): the SGX's host interrupt (GIC 0x34,
// SceGpu's "SceGpuLisr" otherwise) is sgxd's while the app has registered this page. The async
// lists are run by the app's microkernel on the GPU (vita_native/src/sgxd_mk.h, core 0's event
// program): the master's TA, 3D and PTLA completions start it and interrupt no one, and IRQ 52
// takes bank 0's SW_EVENT, which the microkernel raises for a wake or a fault: the ISR clears it,
// counts it and sets the waker's event flag (sgxdWaker; the microkernel has made the armed check).
typedef struct SgxdEvents {
    volatile uint32_t irqs;          // interrupts taken
    volatile uint32_t sw_events;     // bank 0's SW_EVENT interrupts (the GPU's wakes and faults)
    volatile uint32_t unexpected;    // MASTER_EVENT_STATUS bits seen in an interrupt without SW_EVENT
} SgxdEvents;

// sgxdFaultDump: header word 2 of the dump sgxdSubmit takes itself when a poll times out.
#define SGXD_DUMP_FAULT 0x544c5546u   // "FULT"
// sgxdReadRegs: registers a call at most.
#define SGXD_MAX_READ_REGS 4096u

// Errors, beyond azahar_native.h's own and the kernel's; SCE-style like those (AZAHAR_ERROR).
#define SGXD_ERR_FIRMWARE AZAHAR_ERROR(1100)  // SceGpuEs4 is not the build sgxd knows (3.65)
#define SGXD_ERR_BUSY AZAHAR_ERROR(1101)      // a SceGpu bridge call stayed inside for the whole quiesce wait
#define SGXD_ERR_LOST AZAHAR_ERROR(1102)      // the system suspended while owned; take again
#define SGXD_ERR_HW AZAHAR_ERROR(1103)        // the GPU did not answer with its IDs after the power-up
#define SGXD_ERR_OWNER AZAHAR_ERROR(1104)     // another process holds the GPU
#define SGXD_ERR_MAPPED AZAHAR_ERROR(1105)    // sgxdMap over a mapped page, or sgxdUnmap of an unmapped one
#define SGXD_ERR_TABLES AZAHAR_ERROR(1106)    // out of GPU page tables

#ifdef __cplusplus
extern "C" {
#endif

int sgxdTake(SgxdInfo *info);
int sgxdRelease(void);
int sgxdSubmit(SgxdOp *ops, uint32_t count);
int sgxdDump(uint32_t *words, uint32_t count);
int sgxdMap(uint32_t gpu_va, const void *user_va, uint32_t size, uint32_t flags);
int sgxdUnmap(uint32_t gpu_va, uint32_t size);
int sgxdCacheRange(const void *user_va, uint32_t size, uint32_t op);

// Debugging (version 3).
// sgxdFaultDump: the dump sgxdSubmit took when a poll timed out, in sgxdDump's layout with
// header words [2] SGXD_DUMP_FAULT, [3] the op's index, [4] its register, [5] the value it
// wanted, [6] its mask, [7] the last read, [8] the microseconds waited, [9] the system time's
// low word, [13] its kind (SGXD_OP_*, version 7). Returns SGXD_DUMP_WORDS, or 0 when no poll
// has timed out since the module loaded; the next timeout replaces it.
int sgxdFaultDump(uint32_t *words, uint32_t count);
// sgxdReadRegs: count consecutive registers from reg (4-byte aligned, the master bank and the
// four cores' banks, below 0x18000), at most SGXD_MAX_READ_REGS. Returns count.
int sgxdReadRegs(uint32_t reg, uint32_t count, uint32_t *words);
// sgxdEvents (version 4): events is the app's own SgxdEvents, zeroed; NULL gives the interrupt
// back to SceGpu's handler (so does the release, a suspend or the owner's exit).
int sgxdEvents(SgxdEvents *events);
// sgxdKick: core 0's KICK2, which starts the microkernel (IR0 0x10000) once the app has published
// its jobs and set its BUSY word. Needs sgxdEvents.
int sgxdKick(void);
// sgxdWaker (version 9): event_flag is the app's own event flag (a user UID), whose bits the
// kernel sets at each SW_EVENT. A negative event_flag stops it. Needs sgxdEvents; the
// interrupt's release drops it.
int sgxdWaker(int32_t event_flag, uint32_t bits);
// sgxdTranslate: the GPU's view of gpu_va: out[0] its directory entry, out[1] its page table
// entry (0 where no page table covers it). A PTE is the page's physical address | flags | VALID.
int sgxdTranslate(uint32_t gpu_va, uint32_t out[2]);
// Remaps the GPU does itself (version 14; azahar's page versions): sgxdMapTables maps sgxd's
// page directory and tables (SGXD_MMU_BYTES, physically one block, uncached for the CPU) at
// gpu_va, CACHECONSISTENT, so that a list's STORE reaches memory past the SLC and the walker
// reads it after an invalidate. With the GPU idle, once. sgxdPte gives a mapped page's remap:
// out[0] the GPU address of gpu_va's PTE in that mapping, out[1] the PTE naming the physical
// page behind the app's user_va with gpu_va's flags. The GPU stores it in its queue's order; the
// kernel's own bookkeeping of the mapping (sgxdMap's ranges, a page move's retarget) does not
// follow, so the app puts every such PTE back before it moves or unmaps the page.
#define SGXD_MMU_BYTES ((1u + SGXD_MAP_TABLES) * 0x1000u)
int sgxdMapTables(uint32_t gpu_va);
int sgxdPte(uint32_t gpu_va, const void *user_va, uint32_t out[2]);
// Every unit of the GPU reset as the take leaves it, without its power cycle (version 92): the
// start sequence again (its MASTER_SOFT_RESET pulse resets the four cores and the master's DPM,
// VDM, SLC, BIF, MCI and PTLA), every core's caches, and the MMU's banks and directory, which the
// BIF reset cleared. For a backend's start: the GPU idle, no events page registered (the
// microkernel stopped); AZAHAR_ERR_STATE otherwise. The DPM's lists are empty after it.
int sgxdResetAll(void);

#ifdef __cplusplus
}
#endif

#endif
