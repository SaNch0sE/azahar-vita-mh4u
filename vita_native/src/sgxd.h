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

// One step of an sgxdSubmit list. B1 has reads and delays only; writes and polls arrive with
// the register allowlist.
typedef struct SgxdOp {
    uint32_t op;      // SGXD_OP_*
    uint32_t reg;     // register offset from 0xE8400000, 4-byte aligned, below 0x20000
    uint32_t value;   // READ: out; DELAY: microseconds
    uint32_t mask;    // unused in B1
} SgxdOp;

#define SGXD_OP_READ 1u
#define SGXD_OP_DELAY 2u
#define SGXD_MAX_OPS 256u

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

#define SGXD_VERSION 1

// Errors, beyond azahar_native.h's own and the kernel's; SCE-style like those (AZAHAR_ERROR).
#define SGXD_ERR_FIRMWARE AZAHAR_ERROR(1100)  // SceGpuEs4 is not the build sgxd knows (3.65)
#define SGXD_ERR_BUSY AZAHAR_ERROR(1101)      // a SceGpu bridge call stayed inside for the whole quiesce wait
#define SGXD_ERR_LOST AZAHAR_ERROR(1102)      // the system suspended while owned; take again
#define SGXD_ERR_HW AZAHAR_ERROR(1103)        // the GPU did not answer with its IDs after the power-up
#define SGXD_ERR_OWNER AZAHAR_ERROR(1104)     // another process holds the GPU

#ifdef __cplusplus
extern "C" {
#endif

int sgxdTake(SgxdInfo *info);
int sgxdRelease(void);
int sgxdSubmit(SgxdOp *ops, uint32_t count);
int sgxdDump(uint32_t *words, uint32_t count);

#ifdef __cplusplus
}
#endif

#endif
