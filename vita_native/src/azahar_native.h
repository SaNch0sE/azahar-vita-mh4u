// azahar-native — the ABI between the kernel plugin and the emulator. Included by both.
//
// The PS Vita is Sony's handheld games console; this is a hobby emulator project running on a
// console the author owns. The plugin runs guest (Nintendo 3DS) code natively on the calling
// core, one slice at a time, at the guest's own addresses. docs/native-execution.md has the
// design.

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
#define AZAHAR_FAST_VFP 4u        // svc AZAHAR_VFP_SVC in place of a VFP instruction the app
                                  // registered (azaharVfpPatch): run it in the vector, under
                                  // the guest's LEN and STRIDE. Since version 30.
#define AZAHAR_FAST_KINDS 5u
#define AZAHAR_FAST_ALL ((1u << AZAHAR_FAST_KINDS) - 1u)

// The fast svcs' answers where the guest's own code reads them (version 35): two pages azaharMap
// maps into the guest's table, read-only at every level, for code the app writes into the
// guest's world (a branch in place of an svc the module answered, AzaharRunRequest.fast_site, to
// a thunk) to answer the svc without its exception:
// - AZAHAR_USER_TIMER_VA: the first page of the private memory region at PERIPHBASE (the SCU, the
//   GIC's CPU interface, the global timer, the private timer and watchdog), device memory as the
//   kernel maps it. The private timer's counter is at +0x604, each core's own. A read of the CPU
//   interface's ICCIAR (+0x10c) acknowledges an interrupt: only the thunks read this page.
// - AZAHAR_USER_PAGE_VA: AzaharUserPage, written before each entry into the guest (sub-slices
//   included) with `gen` changed. A reader that sees the same gen before and after its reads
//   read one entry's values: an exit is the only time they change. Each core has its own page
//   there (version 68: an attached core's own view of the guest's first megabyte, azaharAttach).
// GetSystemTick's answer is base + (quantum - counter) * mult, as azahar_svc_fast computes it;
// the counter is each core's own at one address. azaharMap refuses a guest range over either page.
#define AZAHAR_USER_TIMER_VA 0x000BE000u
#define AZAHAR_USER_PAGE_VA 0x000BF000u
// A thread switch the app arms from the guest's world (version 37): the emulator's upcalls (C++
// it runs at user level in the guest's world) answer an svc that
// stops the thread or wakes a more urgent one with the next thread's state here, `armed` set,
// and make svc AZAHAR_SWITCH_SVC. The module's vector then loads `in` (r0-r15, the CPSR in user
// mode, TPIDRURO, TPIDRURW, FPSCR and the VFP bank), clears `armed`, sets GetThreadId's answer
// (AzaharUserPage.thread_id and its own) to `thread_id`, and returns into it: no exit. The
// outgoing thread's state is the app's to keep (the upcall saved it). Not armed, the svc exits as
// AZAHAR_EXIT_SVC. AzaharRunRequest.upcall_switch is where it is in the app.
#define AZAHAR_SWITCH_SVC 0xFDu
typedef struct AzaharSwitch {
    uint32_t armed;
    uint32_t thread_id;
    const AzaharContext *in;
    uint32_t reserved;
} AzaharSwitch;

// A guest svc the vector does not answer, redirected to the app's upcall instead of exiting
// (version 38): with AzaharRunRequest.upcall_redirect set, an svc from the guest's code (below
// 0x40000000, outside an IT block; not svc 0, AZAHAR_SWITCH_SVC or AZAHAR_VFP_SVC) returns into
// `entry` (ARM state, the flags and every register the guest's) with `svc` its number and `pc`
// the instruction after it (bit 0 set for Thumb), and `site` the same, for the app to patch the
// site into a branch (the app clears it). The app's thunk at `entry` answers the svc and goes
// back to `pc`.
typedef struct AzaharRedirect {
    uint32_t entry;
    uint32_t svc;
    uint32_t pc;
    uint32_t site;
} AzaharRedirect;

// TPIDRURO from the guest's world (version 39): svc AZAHAR_TLS_SVC sets it to r12 and returns,
// every other register and the flags kept. The app's upcalls run with its own thread's (Sony's
// user libraries find the thread through it) and put the guest's back before they return.
#define AZAHAR_TLS_SVC 0xFCu

// The slice's clock from the guest's world (version 40): svc AZAHAR_CLOCK_SVC answers in r1:r0
// the guest ticks the slice's clock counted since it last started (the slice's entry, or this
// svc), and starts it again: the private timer over r1 timer ticks (at least 1), the guest clock
// from where it is (r0 = 0) or from r3:r2 (r0 = 1). The fast svcs' answers and AzaharUserPage
// follow, and the exit's AzaharRunRequest.guest_ticks counts from the restart. r2, r3 and r12
// are the svc's; the other registers and the flags are kept. The app's upcalls take the slice's
// time with it when the guest goes idle, move its own clock on to the next thread's, and start
// the slice again from there.
#define AZAHAR_CLOCK_SVC 0xFBu

typedef struct AzaharUserPage {
    uint32_t gen;        // changed at every entry into the guest
    uint32_t on;         // 1 << AZAHAR_FAST_* for each answer below that holds; TICK only with a timer
    uint32_t quantum;    // the private timer's start this entry
    uint32_t mult_int;   // guest ticks per timer tick, whole part
    uint32_t mult_frac;  // and fraction, in 1/2^32
    uint32_t base_lo, base_hi; // the guest clock at the timer's start
    uint32_t thread_id;  // GetThreadId(0xFFFF8000)'s answer
    uint32_t process_id; // GetProcessId(0xFFFF8001)'s
} AzaharUserPage;

// azaharRun's request (version 48): the app's own block, one per emulated core, kept from call to
// call. The module maps it into the kernel at its first call and reads and writes it in place
// from then on, with no copy in or out; up to four stay mapped (the least recently run gives its
// slot to a fifth) until azaharRelease or the process's end. 8-byte aligned.
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
    uint64_t tick_hz;     // in: the guest clock's rate, in ticks a second (AZAHAR_FAST_TICK)
    uint64_t tick_base;   // in: the guest clock at entry
    uint64_t guest_ticks; // out: the slice's length on the guest clock, timed by the private
                          // timer. No AZAHAR_FAST_TICK answer exceeds tick_base + guest_ticks
    uint32_t fast_count[AZAHAR_FAST_KINDS]; // out: the svcs answered in the guest, by kind
    uint32_t kernel_cycles; // out: azaharRun's own time in the cycle counter's cycles, the
                            // guest's included (version 45)
    uint32_t fast_site;   // out: the address after the last ARM-mode svc the module answered in
                          // the guest this slice (AZAHAR_FAST_TICK to AZAHAR_FAST_PROCESS_ID), 0
                          // for none: the app's to patch into a branch (AZAHAR_USER_PAGE_VA).
                          // Version 35
    AzaharSwitch *upcall_switch; // in: the app's switch request (AZAHAR_SWITCH_SVC), or 0
                                 // (version 37)
    AzaharRedirect *upcall_redirect; // in: the app's svc redirect (AzaharRedirect), or 0
                                     // (version 38)
    // Version 42: a quantum that ends inside an upcall (the app's code, at or above 0x40000000)
    // is started again in the vector over upcall_rearm_us, at most upcall_rearm_max times a
    // call, with no exit unless an SGI is pending; 0 for none. guest_ticks includes those
    // quantums.
    uint32_t upcall_rearm_us;  // in
    uint32_t upcall_rearm_max; // in
    uint32_t upcall_rearms;    // out: the quantums started again this call
} AzaharRunRequest;

// VFP short vectors (version 30). The Cortex-A9 has none: with the guest's FPSCR LEN or STRIDE
// set, every VFP data-processing instruction is undefined. The app patches such an instruction,
// once it trapped, into svc AZAHAR_VFP_SVC (ARM: the instruction's own condition; Thumb: the svc
// and a nop) and registers its decoded form here by its guest address; azahar_svc_fast then runs
// it in the vector without an exit. The form: op (AZAHAR_VFP_OP_*) | dp << 4 | two << 5 | d << 8
// | n << 16 | m << 24, with register numbers counted in the precision's own registers (singles
// 0-31, doubles 0-15) and `two` an operation of one source (m). LEN and STRIDE are the live
// FPSCR's when it runs: a destination in the first bank is a scalar, a second operand there the
// same scalar for every element (VFPv2's vectors).
#define AZAHAR_VFP_SVC 0xFEu
#define AZAHAR_VFP_OP_MLA 0u
#define AZAHAR_VFP_OP_MLS 1u
#define AZAHAR_VFP_OP_NMLS 2u
#define AZAHAR_VFP_OP_NMLA 3u
#define AZAHAR_VFP_OP_MUL 4u
#define AZAHAR_VFP_OP_NMUL 5u
#define AZAHAR_VFP_OP_ADD 6u
#define AZAHAR_VFP_OP_SUB 7u
#define AZAHAR_VFP_OP_DIV 8u
#define AZAHAR_VFP_OP_CPY 9u
#define AZAHAR_VFP_OP_ABS 10u
#define AZAHAR_VFP_OP_NEG 11u
#define AZAHAR_VFP_OP_SQRT 12u
#define AZAHAR_VFP_TABLE 4096u  // the registered instructions the module holds

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

// Versions of the app's pages (reback.c; the emulator's page versions): the adopted app page at
// user_va, and every guest descriptor showing it, take the physical page behind version_va with
// that mapping's attributes, while the GPU's mapping keeps the page it names (the page's home).
// version_va 0 puts the page back on its home with the home's attributes. The first flip of a
// page cleans and invalidates its home's lines through the app's address first: from then on
// only the GPU writes the home until the page goes back. Contents do not move unless the run
// asks (AZAHAR_FLIP_COPY: the bytes the page shows go onto the new page before the guest's
// descriptors follow, the guest's writes refused meanwhile, as at a move), and the guest's
// descriptors keep their access (azaharHold). A page moved (azaharReback) or returned while
// flipped is refused. Version 26.
#define AZAHAR_FLIP_MAX_RUNS 64u
#define AZAHAR_FLIP_COPY 1u  // in user_va's low bits (version 71)
typedef struct AzaharFlipRun {
    uint32_t user_va;    // page aligned, in the adopted block, with AZAHAR_FLIP_COPY or not
    uint32_t version_va; // page aligned app memory, or 0: back home
} AzaharFlipRun;

// Moving pages of the app's memory onto other physical pages of its own (reback.c). First
// azaharAdopt takes a block of the app's (1 MiB aligned, at most 256 MiB, all of it the app's
// writable memory) under translation tables of the module's: the same pages and attributes.
// Then azaharReback moves pages of it: the pages at user_va take the physical pages behind
// pool_va, at the same virtual address, with the pool's memory attributes, and the guest's
// windows follow. The GPU's mapping keeps the old pages: the app remaps it (sgxdPte) in its
// queue's order, so the GPU need not be idle (version 53). For guest framebuffers in CDRAM: the
// GPU writes and reads them with no cache maintenance. Page aligned, at most 256 pages a call;
// the pool pages must stay the app's for as long as the move holds. Returns the pages moved.
// A move with its copy, and a return, refuse the guest's writes to the pages until the guest's
// windows follow (version 71): a store meanwhile aborts as on a held page (azaharHold).
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
#define AZAHAR_ERR_ROUTE AZAHAR_ERROR(1009)    // the sound goes where no system volume is (HDMI, USB)
#define AZAHAR_ERR_TAKEN AZAHAR_ERROR(1010)    // the USB port is another driver's (a plugin's device)


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
/// A second emulation thread's core (folded mode only, version 68): the caller, pinned to the one
/// core it runs on, runs slices there from now on, as azaharMap's caller does on its own. Each
/// attached core runs its slices at the same time as the others'; a table edit reaches a core
/// inside a slice (the inner-shareable maintenance). In-kernel scheduling stays on azaharMap's
/// core, and so do each core's switch and redirect as its own run request gives them.
/// An attached core has its own view of the guest's first megabyte, which the guest never maps
/// (the module's pages and the app's thunks): the mapping core's, its own user page at
/// AZAHAR_USER_PAGE_VA, and each of `overrides` there instead (app memory, page aligned, at
/// most AZAHAR_ATTACH_OVERRIDES): the app's own code for the core where the mapping core runs
/// the app's thunks. The view follows every later edit of the first megabyte. azaharRun refuses
/// a core no one attached (AZAHAR_ERR_CORE); azaharDetach ends the caller's core's part.
#define AZAHAR_ATTACH_OVERRIDES 8u
typedef struct AzaharAttachRequest {
    uint32_t count;
    AzaharRange overrides[AZAHAR_ATTACH_OVERRIDES]; // guest_va below 1 MiB, user_va, size, perm
} AzaharAttachRequest;
int azaharAttach(const AzaharAttachRequest *req);
int azaharDetach(void);
int azaharEdit(const AzaharEditRequest *req);
/// Registers the decoded VFP instruction `form` at `guest_pc` (see AZAHAR_VFP_SVC), replacing one
/// registered there before. Between slices. 0, or AZAHAR_ERR_ARG for a bad form and
/// AZAHAR_ERR_STATE when the table is full. Version 30.
int azaharVfpPatch(uint32_t guest_pc, uint32_t form);
/// The process's own megabyte at `user_va` (0x40000000 up) into the guest's table as the process's
/// table has it now: a block the app mapped after azaharMap, which an upcall then touched (the
/// guest's world holds the app's megabytes). Between slices. 1 when the entry was added or changed,
/// 0 when it was already there or the process has nothing there, AZAHAR_ERR_* otherwise. Version
/// 44.
int azaharSyncUpper(uint32_t user_va);
/// See AzaharHoldRun: `count` runs, 1 to AZAHAR_HOLD_MAX_RUNS. Returns the guest descriptors it
/// changed. Version 25.
int azaharHold(const AzaharHoldRun *runs, uint32_t count);
/// See AzaharFlipRun: `count` runs, 1 to AZAHAR_FLIP_MAX_RUNS. Returns the guest descriptors it
/// retargeted. Version 26.
int azaharFlip(const AzaharFlipRun *runs, uint32_t count);
/// Prints the CDRAM partitions of the caller's address space and every memblock of 64 KiB or
/// more in the system (owner, name, type, base, size) to the kernel console, once per call
/// (memreport.c). Read-only. Version 28.
int azaharMemReport(void);
/// SceShell's 16 MiB of CDRAM (ScePhyMemPartShellCdram, 0x27000000) for the caller, which must
/// hold the GPU (sgxdTake): the shell's contents go to main memory, and sgxd's release copies
/// them back before SceGpu has the GPU again. `*user_va` gets the range's address in the app.
/// Version 29.
int azaharShellCdram(uint32_t *user_va);
/// The system volume one step up (`step` > 0) or down (< 0), or as it is (0), while the caller
/// holds the GPU (sgxdTake): the volume keys are the app's then, and SceShell, which would step
/// it, sleeps at SceGpu's gate (shell.c). Returns the level, 0 to 30 (AVLS stops the built-in
/// output's at 21), or AZAHAR_ERR_ROUTE when the sound goes where SceShell steps nothing either
/// (HDMI, USB). Version 50.
int azaharSystemVolume(int step);
/// The volume keys held (SCE_CTRL_VOLUP, SCE_CTRL_VOLDOWN) while the caller holds the GPU
/// (sgxdTake), read from Syscon every 20 ms (shell.c): out of SceShell's pad data then, they are
/// in no app's either. Version 63.
int azaharVolumeKeys(void);
/// A core dump of the calling process, as the system writes a crashed process's (coredump.c):
/// the process suspended for its first part, running again for the rest; the call returns once
/// the file is written (minutes over host0). `path` (path_size bytes, may be NULL)
/// gets the dump's path. Returns the dump's result (0 written), or a negative error. Version 77.
int azaharCoredump(char *path, uint32_t path_size);
/// One of the 3DS's screens as a USB webcam (uvc.c, version 82): a UVC device of one YUY2 format,
/// `width` x `height` (even width, at most 400 x 240) at 60 frames a second. Starting it takes the
/// USB port. A port another driver has (azaharUvcPortTaken) is taken only with `take`, which stops
/// every driver on it; without it the call returns AZAHAR_ERR_TAKEN and touches nothing
/// (version 83). A failed step of the start is its result.
int azaharUvcStart(uint32_t width, uint32_t height, int take);
/// 1 when another driver has the USB port: its device activated while none of the system's own
/// functions is started, as a plugin that streams the screen leaves it; else 0 (version 83).
int azaharUvcPortTaken(void);
/// The next frame, `size` bytes of YUY2 (width x height x 2). Returns 1 queued, 0 when nobody is
/// watching (the host has not committed a stream: the app need not convert), or an error.
int azaharUvcFrame(const void *yuy2, uint32_t size);
/// Whether a host is streaming now (no syscall cost beyond the call: a load).
int azaharUvcStreaming(void);
/// Core 3, which the system keeps from game applications, for the calling app's threads (caps.c,
/// version 85): AZAHAR_CORES_CAPUNLOCKER where CapUnlocker is loaded (nothing done), else
/// AZAHAR_CORES_OWN once SceKernelThreadMgr's affinity check allows it (until the module stops),
/// or AZAHAR_ERR_RESOLVE where that check is not the code known (another firmware's).
#define AZAHAR_CORES_CAPUNLOCKER 1
#define AZAHAR_CORES_OWN 2
int azaharUnlockCores(void);
/// The kernel modules loaded from outside the firmware (plugins.c, version 86): taiHEN, HENkaku
/// and the plugins a taiHEN config.txt loads, as "name path" lines in `out` (`size` bytes, ended
/// in a NUL, cut where they do not fit). Returns the count listed, or AZAHAR_ERR_*.
int azaharKernelPlugins(char *out, uint32_t size);
/// How the module's tables map `va` (native.c, version 87), for a guest fault's report: the shared
/// table's level-1 and level-2 entries (0: none of the module's), and per core whether it is
/// attached and its own view's entries (a view's first megabyte is its own; 0 without a view),
/// the overrides of the calling core's view, and how many view builds failed (the last one's
/// error and core: a failed rebuild keeps the megabyte as it was). Read without the lock.
typedef struct AzaharVaReport {
    uint32_t va;
    uint32_t core;     ///< the calling thread's core
    uint32_t map_core; ///< azaharMap's core, which runs in the shared table
    uint32_t shared_l1, shared_l2;
    uint32_t attached[4];
    uint32_t view_l1[4], view_l2[4];
    uint32_t view_overrides[4];
    AzaharRange overrides[AZAHAR_ATTACH_OVERRIDES]; ///< the calling core's view's
    uint32_t view_failures;
    int32_t view_last_error;
    uint32_t view_last_core;
} AzaharVaReport;
int azaharVaReport(uint32_t va, AzaharVaReport *out);
/// Whether SceShell's display plane shows a picture (shell.c, version 88): 1 while the shell has
/// one up, 0 once it has blanked the plane, negative when the plane could not be read. Only with
/// the display on.
int azaharShellPlaneShown(void);
/// What a core runs (coresample.c), for a stall report: its debug block's PC samples and the
/// thread SceKernelThreadMgr's per-CPU block names, read from outside the core. Version 54; the
/// module of the first PC, version 55.
#define AZAHAR_CORESAMPLE_PCS 8
typedef struct AzaharCoreSample {
    uint32_t pid;  ///< the running thread's process
    uint32_t thid; ///< the running thread
    uint32_t idle; ///< the core's idle thread runs
    char thread_name[32];
    uint32_t pcs[AZAHAR_CORESAMPLE_PCS]; ///< 0xFFFFFFFF: the core could not be sampled
    char module_name[28];  ///< of the listed kernel modules, the one whose text the first PC follows
    uint32_t module_text;  ///< that module's text base (0: none below it)
} AzaharCoreSample;
int azaharCoreSample(uint32_t core, AzaharCoreSample *out);
/// SceKernelBusError's dump of the bus's error registers (coresample.c), up to `size` bytes: the
/// count, AZAHAR_ERR_RESOLVE without the module. Its handler parks the core that took the bus
/// error for good; the dump says which master and address. Version 56.
int azaharBusError(void *out, uint32_t size);
/// The guest's translation tables at a stall (coresample.c): the guest's copy of the process's
/// level-1 entries from 0x40000000 up against the process's own (stale: it maps otherwise now;
/// missing: the copy lacks it), a stale coarse entry's table read for descriptors outside main
/// memory (0x80000000 up), and our own tables' descriptors outside it. Version 57.
#define AZAHAR_TABLESCAN_N 4
typedef struct AzaharTableScan {
    uint32_t stale, missing;
    uint32_t stale_va[AZAHAR_TABLESCAN_N];
    uint32_t stale_guest[AZAHAR_TABLESCAN_N];   ///< the copy's entry
    uint32_t stale_process[AZAHAR_TABLESCAN_N]; ///< the process's
    uint32_t stale_wild[AZAHAR_TABLESCAN_N];    ///< a coarse copy's table: descriptors outside memory
    uint32_t stale_wild_desc[AZAHAR_TABLESCAN_N];
    uint32_t ours_wild;                         ///< our tables' descriptors outside memory
    uint32_t ours_wild_at[AZAHAR_TABLESCAN_N];  ///< the descriptor's physical address
    uint32_t ours_wild_desc[AZAHAR_TABLESCAN_N];
    /// Page 0xFFFFF000 in the guest's world (version 59): the guest's level-1 entry for its
    /// megabyte, the process's own, and the level-2 descriptor a coarse guest entry gives it.
    uint32_t top_l1, top_process, top_l2;
    /// The lower gigabyte's copy of the kernel's own entries against the kernel's table now
    /// (version 60): stale ones, and a stale coarse copy's table read as above.
    uint32_t kstale;
    uint32_t kstale_va[AZAHAR_TABLESCAN_N];
    uint32_t kstale_guest[AZAHAR_TABLESCAN_N];
    uint32_t kstale_kernel[AZAHAR_TABLESCAN_N];
    uint32_t kstale_wild[AZAHAR_TABLESCAN_N];
    uint32_t kstale_wild_desc[AZAHAR_TABLESCAN_N];
    /// The guest's level-1 entries by domain (version 61): valid entries, the first one, and,
    /// for a domain the syscall's DACR holds at No-access (the ones the guest's slice grants
    /// Client), sections and level-2 descriptors outside main memory. Sony's own DACRs keep
    /// domains 10 and 15 No-access in user mode as well (user 0x15455551, kernel 0x15450000).
    uint32_t dacr;
    uint32_t dom_valid[16], dom_first_va[16], dom_first_desc[16];
    uint32_t dom_wild[16], dom_wild_desc[16];
} AzaharTableScan;
int azaharTableScan(AzaharTableScan *out);
/// The moment of a bus error (coresample.c): the first call hooks SceKernelBusError's handler,
/// which from then on samples every core's PC and running thread before it parks its core; each
/// call copies what the first bus error caught (count 0: none yet) to `out` (may be NULL), with
/// the kernel module of each core's first PC. Version 58.
#define AZAHAR_BUSWATCH_PCS 16
typedef struct AzaharBusErrorCatch {
    uint32_t count; ///< bus error interrupts taken
    uint32_t core;  ///< the core that took the first
    uint32_t pid[4], thid[4];
    uint32_t pcs[4][AZAHAR_BUSWATCH_PCS]; ///< per core, the cores interleaved in time
    char module_name[4][28];
    uint32_t module_text[4];
} AzaharBusErrorCatch;
int azaharBusErrorWatch(AzaharBusErrorCatch *out);
/// This module's version, and the first version that can be stopped and unloaded: from it on,
/// module_stop lets go of everything the module registered or allocated, and refuses while a
/// process holds any of it, so an update can replace a loaded module without a reboot.
#define AZAHAR_NATIVE_VERSION 93
#define AZAHAR_NATIVE_UNLOADABLE 67
int azaharVersion();
/// This module as a UID of the calling process, for taiStopUnloadKernelModule: taiHEN stops and
/// unloads a kernel module only by the UID its load gave the loading process, and an update's
/// process is not that one. Version 67.
int azaharModuleUid(void);
/// Kernel printf text (every module's), oldest first, up to `size` bytes, no NUL: the count, or
/// 0 when there is none. The first call starts the capture (klog.c). Version 6.
int azaharKlogRead(char *buf, uint32_t size);
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
