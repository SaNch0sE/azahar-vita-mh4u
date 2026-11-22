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
// Until SgxdEvents.twod has reached value (sgxdEvents): the ISR's count of PTLA completions.
// timeout_us at most 1 s; a timeout stops the list there, value then the count last read.
#define SGXD_OP_WAIT_TWOD 5u
// sgxdSubmitAsync only. WAIT_EVENT: until MASTER_EVENT_STATUS has a bit of mask (TA_FINISHED
// 0x2000 or PIXELBE_END_RENDER 0x40000); the list parks there with the bit enabled in
// MASTER_EVENT_HOST_ENABLE, IRQ 52 runs the rest, and the bit is cleared through 0x4134.
// WAIT_TWOD parks the same way until the PTLA count reaches value. SIGNAL counts in the events
// page (value SGXD_SIGNAL_*); STAMP stores the system time in stamp[value & 3] and logs it,
// with value, in stamp_log (version 6).
#define SGXD_OP_WAIT_EVENT 6u
#define SGXD_OP_SIGNAL 7u
#define SGXD_OP_STAMP 8u
#define SGXD_SIGNAL_TA 0u      // SgxdEvents.chain_ta: the render's vertices are read
#define SGXD_SIGNAL_DONE 1u    // SgxdEvents.chains: the list is complete
#define SGXD_MAX_OPS 256u
#define SGXD_MAX_CHAIN_OPS 1024u
// sgxdSubmitAsync's queue (version 6): lists of up to SGXD_MAX_CHAIN_OPS ops, this many in all.
#define SGXD_CHAIN_RING 8192u
#define SGXD_MAX_POLL_US 1000000u
// SgxdEvents.stamp_log's entries: the last STAMP ops, so queued lists do not lose each other's.
#define SGXD_STAMP_LOG 64u

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

#define SGXD_VERSION 6

// sgxdEvents (version 4): the SGX's host interrupt (GIC 0x34, SceGpu's "SceGpuLisr" otherwise)
// is sgxd's while the app has registered this page; the ISR updates it, so any thread of the
// app sees a GPU completion with a plain load. Only TWOD_COMPLETE is enabled
// (MASTER_EVENT_HOST_ENABLE bit 27): for each, the ISR clears it, writes the SLC back and
// empties it (as a PTLA op's own list did), then counts it.
typedef struct SgxdEvents {
    volatile uint32_t twod;          // TWOD_COMPLETE interrupts handled, the SLC done
    volatile uint32_t irqs;          // interrupts taken
    volatile uint32_t unexpected;    // MASTER_EVENT_STATUS bits seen in an interrupt that was not ours
    volatile uint32_t slc_timeouts;  // SLC write-backs or invalidates in the ISR that gave up
    volatile uint32_t slc_us_max;    // the longest SLC write-back and invalidate in the ISR
    volatile uint32_t slc_us_total;
    volatile uint32_t ta;            // TA_FINISHED interrupts (master bit 13)
    volatile uint32_t d3;            // PIXELBE_END_RENDER interrupts (master bit 18)
    volatile uint32_t chain_ta;      // SGXD_SIGNAL_TA ops run (sgxdSubmitAsync)
    volatile uint32_t chains;        // SGXD_SIGNAL_DONE ops run
    volatile uint32_t chain_fault;   // 0, or 1 + the index of the op an async list stopped at
    volatile uint32_t chain_value;   // the last READ's value, or the stopped poll's last read
    volatile uint32_t stamp[4];      // STAMP ops' system times (microseconds)
    volatile uint32_t chain_isr_us_max;  // the longest stretch of an async list run by the ISR
    // Version 6: STAMP ops run; the op numbered n (from 0) is stamp_log[n % SGXD_STAMP_LOG],
    // {system time, value}, written before the count moves past it.
    volatile uint32_t stamps;
    volatile uint32_t stamp_log[SGXD_STAMP_LOG][2];
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
// sgxdFaultDump: the dump sgxdSubmit took itself the moment a poll timed out, before the app
// could do anything else, in sgxdDump's layout with header words [2] SGXD_DUMP_FAULT, [3] the
// op's index, [4] its register, [5] the value it wanted, [6] its mask, [7] the last read, [8]
// the microseconds waited, [9] the system time's low word. The status registers a hang is read
// by go to the kernel console at the same moment. Returns SGXD_DUMP_WORDS, or 0 when no poll
// has timed out since the module loaded; the next timeout replaces it.
int sgxdFaultDump(uint32_t *words, uint32_t count);
// sgxdReadRegs: count consecutive registers from reg (4-byte aligned, the master bank and the
// four cores' banks, below 0x18000), at most SGXD_MAX_READ_REGS. Returns count.
int sgxdReadRegs(uint32_t reg, uint32_t count, uint32_t *words);
// sgxdEvents (version 4): events is the app's own SgxdEvents, zeroed; NULL gives the interrupt
// back to SceGpu's handler (so does the release, a suspend or the owner's exit).
int sgxdEvents(SgxdEvents *events);
// sgxdSubmitAsync (version 6): queues ops behind the lists queued before (SGXD_CHAIN_RING ops
// in all) and returns the index of its first op in the queue's free-running count (31 bits), or
// AZAHAR_ERR_FULL while there is no room. A list runs when the one before has ended: as
// sgxdSubmit does until a WAIT_EVENT or WAIT_TWOD that is not met yet, then from IRQ 52.
// Needs sgxdEvents. sgxdSubmit refuses while the queue runs. A POLL that times out stops the
// queue (chain_fault is 1 + the op's index, and the fault dump sgxdFaultDump returns);
// sgxdSubmitAsync then refuses with AZAHAR_ERR_STATE. NULL stops the queue where it waits, takes
// the fault dump there, drops what is queued, and returns 1 + the op's index (0: none ran).
int sgxdSubmitAsync(const SgxdOp *ops, uint32_t count);
// sgxdTranslate: the GPU's view of gpu_va: out[0] its directory entry, out[1] its page table
// entry (0 where no page table covers it). A PTE is the page's physical address | flags | VALID.
int sgxdTranslate(uint32_t gpu_va, uint32_t out[2]);
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
