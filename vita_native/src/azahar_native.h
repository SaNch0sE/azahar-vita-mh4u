// azahar-native — the ABI between the kernel plugin and the emulator. Included by both.
//
// The PS Vita is Sony's handheld games console; this is a hobby emulator project running on a
// console the author owns. The plugin takes one CPU core away from the Vita's scheduler and
// runs guest (Nintendo 3DS) code on it natively, at the guest's own addresses. DEVELOPMENT.md
// beside this file is the design.

#ifndef AZAHAR_NATIVE_H
#define AZAHAR_NATIVE_H

#include <stdint.h>

#define AZAHAR_MAX_RANGES 8

// Permissions for a mapped range. Every guest page is reachable from user mode (the guest runs
// at PL0); these decide read/write/execute. W without R is not supported.
#define AZAHAR_PERM_R 1u
#define AZAHAR_PERM_W 2u
#define AZAHAR_PERM_X 4u
// The range's pages take the memory attributes the app's own mapping of them has (read back with
// ATS1CUR), not write-back: for memory the app holds uncached, such as a CDRAM block. Two views
// of one page with different cacheability are unpredictable on ARMv7. Since version 5.
#define AZAHAR_PERM_APP_ATTR 8u

// One range of the app's memory to appear at a guest virtual address. user_va, guest_va and size
// are 4 KB aligned; the pages behind user_va need not be physically contiguous — the plugin
// builds coarse page tables over whatever pages the app's memblock has.
typedef struct AzaharRange {
    uint32_t user_va;
    uint32_t guest_va;
    uint32_t size;
    uint32_t perm;
} AzaharRange;

typedef struct AzaharMapRequest {
    uint32_t count;             // may be 0: install the table with no windows, add them with azaharEdit
    AzaharRange ranges[AZAHAR_MAX_RANGES];
} AzaharMapRequest;

// The guest register file. r[13] and r[14] are the user-mode SP and LR; r[15] is the PC to
// resume at; cpsr is the full user-mode CPSR (mode bits 0x10, T for Thumb). tpidruro is the
// guest's TLS register.
// Layout is shared with entry.S: r0-r15 at 0x00, cpsr 0x40, tpidruro 0x44, tpidrurw 0x48,
// fpscr 0x4c, the 32 double-precision VFP registers as 64 words at 0x50. 0x150 bytes.
typedef struct AzaharContext {
    uint32_t r[16];
    uint32_t cpsr;
    uint32_t tpidruro;
    uint32_t tpidrurw;
    uint32_t fpscr;
    uint32_t fpregs[64];
} AzaharContext;

// Why the guest stopped. The context is updated in place; r[15] is:
//   SVC   the instruction after the svc (resume here to continue)
//   UND   the undefined instruction itself
//   PABT  the address whose fetch aborted
//   DABT  the instruction whose data access aborted
#define AZAHAR_EXIT_SVC 1u
#define AZAHAR_EXIT_UND 2u
#define AZAHAR_EXIT_PABT 3u
#define AZAHAR_EXIT_DABT 4u
#define AZAHAR_EXIT_TIMER 5u   // the preemption quantum expired; r[15] is the next instruction

// The svcs the module answers in the guest's own world, without an exit (entry.S's
// azahar_svc_fast): the guest resumes at the next instruction with the result in its registers,
// and the slice goes on. AzaharRunRequest.fast says which; anything else, or the same svc with
// another handle, exits as AZAHAR_EXIT_SVC. Since version 14.
#define AZAHAR_FAST_TICK 0u       // 0x28 GetSystemTick: tick_base plus the slice's time so far
#define AZAHAR_FAST_CACHE 1u      // 0x52-0x54 Invalidate/Store/FlushProcessDataCache on the
                                  // current process (0xFFFF8001): success, as the HLE kernel
                                  // answers them
#define AZAHAR_FAST_THREAD_ID 2u  // 0x37 GetThreadId on the current thread (0xFFFF8000)
#define AZAHAR_FAST_PROCESS_ID 3u // 0x35 GetProcessId on the current process (0xFFFF8001)
#define AZAHAR_FAST_KINDS 4u
#define AZAHAR_FAST_ALL ((1u << AZAHAR_FAST_KINDS) - 1u)

typedef struct AzaharRunRequest {
    AzaharContext ctx;       // in: the state to enter with; out: the state at exit
    uint32_t reason;      // out: AZAHAR_EXIT_*
    uint32_t svc_number;  // out: for AZAHAR_EXIT_SVC, the immediate (24-bit ARM, 8-bit Thumb)
    uint32_t fsr;         // out: DFSR (DABT) or IFSR (PABT), else 0
    uint32_t far;         // out: DFAR (DABT) or IFAR (PABT), else 0
    uint32_t spsr;        // out: the SPSR the exception saw (the guest CPSR at the exception)
    uint32_t raw_lr;      // out: the exception-mode LR before adjustment, for cross-checking
    uint32_t quantum_us;  // in: preemption quantum; 0 runs without a timer (the guest must trap)
    uint32_t timer_hz;    // out: the private timer's measured clock
    uint32_t timer_left;  // out: timer counter at exit (ticks not yet spent)
    // Since version 14.
    uint32_t fast;        // in: 1 << AZAHAR_FAST_* for each svc the module may answer itself
    uint32_t thread_id;   // in: the running guest thread's ID (AZAHAR_FAST_THREAD_ID)
    uint32_t process_id;  // in: its process's ID (AZAHAR_FAST_PROCESS_ID)
    uint32_t reserved;
    uint64_t tick_hz;     // in: the guest clock's rate, in ticks a second (AZAHAR_FAST_TICK)
    uint64_t tick_base;   // in: the guest clock at entry
    uint64_t guest_ticks; // out: the slice's length on the guest clock, timed by the private
                          // timer. No AZAHAR_FAST_TICK answer exceeds tick_base + guest_ticks
    uint32_t fast_count[AZAHAR_FAST_KINDS]; // out: the svcs answered in the guest, by kind
    uint32_t kernel_us;   // out: azaharRun's own time, the guest's included
    uint32_t reserved2;
} AzaharRunRequest;

// Runtime edits to the guest's mapping, for svcControlMemory / svcMapMemoryBlock and friends.
// Only between runs: the guest is stopped at an exit while the app handles its svc. MAP adds
// pages (a new megabyte gets an L2 from the pool); UNMAP clears them; PROTECT rewrites the
// permission of pages already mapped, keeping their physical pages. The next azaharRun flushes
// core 2's TLB, branch predictor and instruction cache before entering the guest.
#define AZAHAR_EDIT_MAP 1u
#define AZAHAR_EDIT_UNMAP 2u
#define AZAHAR_EDIT_PROTECT 3u
#define AZAHAR_EDIT_SYNC_CODE 4u  // the app wrote code into mapped pages (a CRO link, a loader):
                               // clean them to memory and have core 2 drop its icache. user_va
                               // names the app's view of the range; guest_va/perm are ignored

typedef struct AzaharEditRequest {
    uint32_t op;        // AZAHAR_EDIT_*
    AzaharRange range;     // user_va is ignored for UNMAP; perm is ignored for UNMAP
} AzaharEditRequest;

// Holds on the guest's view of the app's pages, named by the app's address: every guest
// descriptor whose window shows a page of [user_va, user_va + size) gets the access `hold`
// leaves the guest, at every guest address the page appears at; its memory attributes and XN
// stay. A page no window shows is skipped. The emulator's GPU page holds
// (SgxdRenderer::PageHolds): a page queued GPU work reads takes WRITES, one it writes
// takes ALL, and a guest access the hold refuses leaves as AZAHAR_EXIT_DABT (a permission
// fault) until the emulator lets go. Every guest window is mapped read-write, which NONE gives
// back. Like azaharEdit, between slices, or on a core inside one with two attached. Version 25.
#define AZAHAR_HOLD_NONE 0u   // the guest reads and writes the page
#define AZAHAR_HOLD_WRITES 1u // the guest reads it; a write aborts
#define AZAHAR_HOLD_ALL 2u    // any guest access aborts
#define AZAHAR_HOLD_MAX_RUNS 64u
typedef struct AzaharHoldRun {
    uint32_t user_va; // page aligned
    uint32_t size;    // page aligned, not 0
    uint32_t hold;    // AZAHAR_HOLD_*
} AzaharHoldRun;

// Moving pages of the app's memory onto other physical pages of its own (reback.c). First
// azaharAdopt takes a block of the app's (1 MiB aligned, at most 256 MiB, all of it the app's
// writable memory) under translation tables of the module's: the same pages and attributes.
// Then azaharReback moves pages of it: the pages at user_va take the physical pages behind
// pool_va, at the same virtual address, with the pool's memory attributes, and the guest's
// windows follow. The GPU's mapping keeps the old pages: the app remaps it (sgxdPte) in its
// queue's order, so the GPU need not be idle (version 53). For guest framebuffers in CDRAM: the
// GPU writes and reads them with no cache maintenance. Page aligned, at most 256 pages a call;
// the pool pages must stay the app's for as long as the move holds. Returns the pages moved.
// Version 7.
#define AZAHAR_REBACK_COPY 1u   // the old contents go along; without it the new pages keep theirs
typedef struct AzaharRebackRequest {
    uint32_t user_va;
    uint32_t pool_va;
    uint32_t size;
    uint32_t flags;             // AZAHAR_REBACK_*
} AzaharRebackRequest;
// Error codes, beyond the kernel's own. They are SCE-style, bit 31 set and bit 30 clear: the
// syscall return path clears bit 30 of a negative result, so a plain negative number reaches
// user code changed (measured: -1102, 0xFFFFFBB2, arrives as 0xBFFFFBB2) and would
// not compare equal there.
#define AZAHAR_ERROR(n) ((int)(0x80A50000u | (n)))
#define AZAHAR_ERR_STATE AZAHAR_ERROR(1000)    // call out of order (map before take, run before map...)
#define AZAHAR_ERR_ARG AZAHAR_ERROR(1001)      // bad request (alignment, count, perm)
#define AZAHAR_ERR_RESOLVE AZAHAR_ERROR(1002)  // a kernel export needed did not resolve
#define AZAHAR_ERR_ALLOC AZAHAR_ERROR(1003)    // a kernel allocation failed
#define AZAHAR_ERR_CORE AZAHAR_ERROR(1004)     // the core could not be taken, or the thread did not answer
#define AZAHAR_ERR_TABLE AZAHAR_ERROR(1005)    // the table could not be built (kernel L1 not found, gap)
#define AZAHAR_ERR_PAGE AZAHAR_ERROR(1006)     // a user page did not translate
#define AZAHAR_ERR_TIMEOUT AZAHAR_ERROR(1007)  // the core-2 thread did not complete a command in time
#define AZAHAR_ERR_FULL AZAHAR_ERROR(1008)     // a queue has no room yet (the microkernel's jobs)


#ifdef __cplusplus
extern "C" {
#endif

// User-side prototypes (the kernel exports these as syscalls).
/// azaharTakeCore's success value when the module folded the guest into the caller: no core is
/// held, and each slice runs on whichever core calls azaharRun. Plain 0 means a core was taken
/// and the caller must stay off it.
#define AZAHAR_TAKE_FOLDED 1
int azaharTakeCore(void);
int azaharMap(const AzaharMapRequest *req);
int azaharRun(AzaharRunRequest *req);
int azaharRelease(void);
int azaharEdit(const AzaharEditRequest *req);
/// See AzaharHoldRun: `count` runs, 1 to AZAHAR_HOLD_MAX_RUNS. Returns the guest descriptors it
/// changed. Version 25.
int azaharHold(const AzaharHoldRun *runs, uint32_t count);
/// This module's version, and the first version that can be stopped and unloaded: from it on,
/// module_stop lets go of everything the module registered or allocated, and refuses while a
/// process holds any of it, so an update can replace a loaded module without a reboot.
#define AZAHAR_NATIVE_VERSION 67
#define AZAHAR_NATIVE_UNLOADABLE 67
int azaharVersion();
/// This module as a UID of the calling process, for taiStopUnloadKernelModule: taiHEN stops and
/// unloads a kernel module only by the UID its load gave the loading process, and an update's
/// process is not that one. Version 67.
int azaharModuleUid(void);
/// See AzaharRebackRequest. Version 7.
int azaharAdopt(uint32_t user_va, uint32_t size);
int azaharReback(const AzaharRebackRequest *req);
/// Puts moved pages of [user_va, user_va + size) back on the physical pages they had before
/// azaharReback, with their attributes and contents, and the guest's windows follow; the GPU's
/// mapping is the app's to remap, as a move's (version 53), and the pool pages are free for
/// another move once the GPU no longer reads them. Every page must be a moved one; page
/// aligned, at most 256 pages. Returns the pages returned. Version 8.
int azaharRebackReturn(uint32_t user_va, uint32_t size);
/// Gives the adopted block back to the kernel's tables, for the teardown: after it the app's
/// pages are its old ones again, with whatever they held before any move. Also done when the
/// process ends. Version 7.
int azaharRebackRestore(void);

/// User-mode access to the PMU for every thread of the calling process, now and created later
/// (cycles.c). ThreadMgr keeps the PMU per thread, so a thread's cycle counter counts that
/// thread's cycles only: its user time and its syscalls, not interrupts or the time it is not
/// running. Version 17.
int azaharCycleCounter(void);

/// The calling thread's cycle counter (azaharCycleCounter first): 32 bits, so take differences.
static inline uint32_t azaharCycles(void) {
    uint32_t c;
    __asm__ volatile("mrc p15, 0, %0, c9, c13, 0" : "=r"(c));
    return c;
}
/// The same, with the cycle counter's enable (PMCNTENSET's C) set first: the enable is not kept
/// with the thread and a resume clears it, so a measurement starts here.
static inline uint32_t azaharCyclesStart(void) {
    __asm__ volatile("mcr p15, 0, %0, c9, c12, 1" ::"r"(1u << 31));
    return azaharCycles();
}

#ifdef __cplusplus
}
#endif


#endif
