@ azahar-native — entering the guest and leaving it.
@
@ ARM state throughout: exception entry is ARM, and `ldm ^` / `rfe` / `movs pc` have no useful
@ Thumb forms. Called from Thumb C on the core that runs the slice, returned to with interworking.
@
@ azahar_enter(cpu): loads the guest's user-mode register file from cpu->ctx and returns from
@ exception into user mode at ctx.r15 with ctx.cpsr. It "returns" only when the guest takes an
@ exception — svc, undefined instruction, prefetch abort or data abort — through one of the four
@ vector stubs below, which save the user registers back into ctx, record why, and unwind to the
@ C caller as if azahar_enter had returned. The return value is the reason. The svcs
@ azahar_svc_fast answers are the exception: the guest gets its result and runs on.
@
@ One block per Vita core (azahar_cpu, AzaharCpu in native.c, CPU_* below): the guest context
@ first, then the host block, the exit record, the stray chains, the fast-path block and two small
@ stacks. VBAR is banked per core and each core's vector table names its own copy of the stubs,
@ whose literals name that core's block: a stub has only SP free and cannot read MPIDR. From the
@ stubs on, everything is addressed from the block, so the common code is shared.
@
@ Layout of AzaharContext: r0..r15 at 0x00..0x3c, cpsr at 0x40, tpidruro at 0x44.
@ Layout of the host block: 0x00 host cpsr, 0x04 host sp, 0x08 original SP_svc, 0x0c ctx address.
@
@ Modes. The calling thread runs in whatever PL1 mode Sony's kernel threads use — System or
@ Supervisor; the entry code saves the CPSR and switches to Supervisor, because `ldm {..}^` from
@ System mode is unpredictable (System already uses the user registers). SP_svc is borrowed as
@ the pointer to ctx for the `ldm ^` + `rfe` pair and put back on exit. The exception stubs load
@ their mode's SP from a literal, so SP_und and SP_abt are clobbered — the C side saves Sony's
@ values before installing the vectors and restores them before handing the core back.

    .syntax unified
    .arm
    .fpu neon
    .text

@ AzaharCpu (native.c checks every offset). The context is at 0, so a stub's SP after it saved
@ the user file is the block.
    .equ CPU_HOST, 0x150            @ uint32_t[4]
    .equ CPU_ACTIVE, 0x160          @ a guest is running on this core
    .equ CPU_PROGRESS, 0x164
    .equ CPU_PROG_UND, 0x168        @ each becomes its own address when its stub is reached
    .equ CPU_PROG_SVC, 0x16c
    .equ CPU_PROG_PABT, 0x170
    .equ CPU_PROG_DABT, 0x174
    .equ CPU_PROG_IRQ, 0x178
    .equ CPU_STRAY_RETRY, 0x17c
    .equ CPU_EXIT, 0x180            @ reason, raw lr, spsr, dfsr, dfar, ifsr, ifar
    .equ CPU_IRQ_LAST, 0x19c
    .equ CPU_IRQ_STRAY, 0x1a0
    .equ CPU_CHAIN_UND, 0x1a4       @ {saved SP for the mode, Sony's handler} per vector
    .equ CPU_CHAIN_SVC, 0x1ac
    .equ CPU_CHAIN_PABT, 0x1b4
    .equ CPU_CHAIN_DABT, 0x1bc
    .equ CPU_CHAIN_IRQ, 0x1c4
    .equ CPU_STRAY, 0x1cc           @ count, reason, lr, spsr, dfsr, dfar, ifsr, ifar
    .equ CPU_FAST, 0x1f0            @ AzaharFast
    .equ CPU_STRAY_STACK_TOP, 0x2a0 @ 64 bytes below each
    .equ CPU_IRQ_STACK_TOP, 0x2e0
    .equ CPU_VFP_STACK_TOP, 0x6e8   @ azahar_vfp_svc's stack (1 KiB below)
    .equ CPU_SIZE, 0x6f0            @ the last 8 bytes keep the tops inside the block
@ AzaharFast, at CPU_FAST (native.c): on 0x00, timer 0x04, quantum 0x08, mult_int 0x0c,
@ mult_frac 0x10, base 0x14 (low) and 0x18, thread_id 0x1c, process_id 0x20, count[5] 0x24,
@ save[4] 0x38. AzaharCpu.fast_site is the word before it.
    .equ FAST_ON, 0x00
    .equ FAST_TIMER, 0x04
    .equ FAST_QUANTUM, 0x08
    .equ FAST_MULT_INT, 0x0c
    .equ FAST_MULT_FRAC, 0x10
    .equ FAST_BASE_LO, 0x14
    .equ FAST_BASE_HI, 0x18
    .equ FAST_THREAD_ID, 0x1c
    .equ FAST_PROCESS_ID, 0x20
    .equ FAST_COUNT, 0x24
    .equ FAST_SAVE, 0x38
    .equ FAST_REARM_TICKS, 0x48
    .equ FAST_REARM_LEFT, 0x4c
    .equ FAST_ACCRUED_LO, 0x50
    .equ FAST_ACCRUED_HI, 0x54
    .equ FAST_REARMS, 0x58
    .equ FAST_USER_KVA, 0x5c        @ version 68: this core's user page, switch, redirect
    .equ FAST_SWITCH_VA, 0x60
    .equ FAST_REDIRECT_VA, 0x64
    .equ FAST_SITE, -4
    .equ USER_GEN, 0x00             @ AzaharUserPage
    .equ USER_QUANTUM, 0x08
    .equ USER_BASE_LO, 0x14
    .equ USER_BASE_HI, 0x18
    .equ USER_THREAD_ID, 0x1c
    .equ PTIMER_LOAD_FROM_COUNTER, -4      @ the private timer's registers by its counter's
    .equ PTIMER_CONTROL_FROM_COUNTER, 4
    .equ PTIMER_STATUS_FROM_COUNTER, 8
    .equ SWITCH_ARMED, 0x00         @ AzaharSwitch
    .equ SWITCH_THREAD_ID, 0x04
    .equ SWITCH_IN, 0x08
    .equ REDIRECT_ENTRY, 0x00       @ AzaharRedirect
    .equ REDIRECT_SVC, 0x04
    .equ REDIRECT_PC, 0x08
    .equ REDIRECT_SITE, 0x0c

    .global azahar_enter
    .type azahar_enter, %function
azahar_enter:
    @ r0 = this core's block (its context first)
    push    {r4-r11, lr}
    mov     r3, #10
    str     r3, [r0, #CPU_PROGRESS]
    add     r1, r0, #CPU_HOST
    mrs     r2, cpsr
    str     r2, [r1, #0x00]         @ host cpsr: mode and interrupt bits to come back to
    @ Masked before CPU_ACTIVE is set: an IRQ between the two would find a guest "active" and
    @ save this host code's registers as the guest's (core2_run masks IRQs before it starts
    @ the timer as well).
    cpsid   if
    str     sp, [r1, #0x04]         @ host sp, in the host's mode
    str     r0, [r1, #0x0c]         @ ctx
    mov     r3, #1
    str     r3, [r0, #CPU_ACTIVE]
    @ The guest's VFP state. The core is the guest's alone during a slice, so the register file
    @ simply is the guest's; loading it here and storing it at exit is what makes a guest thread
    @ switch (SaveContext/LoadContext on the app side) carry it.
    add     r2, r0, #0x50
    vldmia  r2!, {d0-d15}
    vldmia  r2, {d16-d31}
    ldr     r3, [r0, #0x4c]
    vmsr    fpscr, r3
    @ Drop any exclusive reservation before the guest resumes. Whether an exception return
    @ clears the local monitor is IMPLEMENTATION DEFINED on ARMv7-A, so it cannot be assumed:
    @ guest thread A can take its exit between an ldrex and its strex, and if the reservation
    @ survives into thread B then B's strex to the same word succeeds when the architecture
    @ says it must fail. That is a 3DS LightLock handed to two owners at once, and it is how
    @ guest threads end up parked on address arbiters nobody will signal. One instruction per entry,
    @ and it also makes ClearExclusiveState's no-op honest. A spurious strex failure is
    @ architecturally permitted and every lock loop retries, so clearing on every entry costs
    @ nothing but the instruction.
    clrex
    cps     #0x13                   @ Supervisor (IRQs masked above: nothing may interrupt
                                    @ the banked-register dance)
    str     sp, [r1, #0x08]         @ Sony's SP_svc, put back on exit
    mov     r3, #11
    str     r3, [r0, #CPU_PROGRESS]
    mov     sp, r0                  @ SP_svc = ctx
    ldmia   sp, {r0-r14}^           @ the guest's r0-r12 and its user-mode sp and lr
    nop                             @ no banked access in the instruction after ldm ^
    add     sp, sp, #0x3c           @ -> ctx.r15, ctx.cpsr
    rfeia   sp                      @ pc = ctx.r15, cpsr = ctx.cpsr: user mode, guest runs

@ ---------------------------------------------------------------------------- vector stubs
@ Each runs in its own exception mode with that mode's LR and SPSR. They save the user register
@ file first — before any register is touched — then set the reason and fall into the common tail.

@ One stub per exception mode and core. With a guest active: save the user register file into
@ the core's context, set the reason, fall into the common tail. With no guest active — an
@ exception taken by whatever else ran on this core while our vectors were installed — record it
@ once, put this mode's SP back, and chain to Sony's handler for that vector through a {sp, pc}
@ pair C filled in at install. Without this, a prefetch abort between install and the first run
@ loops through the stubs storing to a null ctx pointer. The svc
@ stub with a guest active goes to azahar_svc_fast first, which answers some svcs without leaving.
    .macro AZAHAR_STUB core, name, reason, prog, chain
    .global azahar_vec_\name\()_\core
    .type azahar_vec_\name\()_\core, %function
azahar_vec_\name\()_\core:
    ldr     sp, =azahar_cpu + \core * CPU_SIZE + \prog  @ the word becomes its own address
    str     sp, [sp]
    ldr     sp, =azahar_cpu + \core * CPU_SIZE + CPU_ACTIVE
    ldr     sp, [sp]
    cmp     sp, #0
    beq     9f
    .if \reason == 1
    ldr     sp, =azahar_cpu + \core * CPU_SIZE + CPU_FAST + FAST_SAVE
    b       azahar_svc_fast
    .else
    ldr     sp, =azahar_cpu + \core * CPU_SIZE
    stmia   sp, {r0-r14}^
    nop
    mov     r0, #\reason
    b       azahar_exit_common
    .endif
9:  ldr     sp, =azahar_cpu + \core * CPU_SIZE + CPU_STRAY_STACK_TOP
    push    {r0-r3}
    ldr     r0, =azahar_cpu + \core * CPU_SIZE + CPU_STRAY
    ldr     r1, [r0]                @ count: only the first is recorded in full
    cmp     r1, #0
    bne     8f
    mov     r2, #\reason
    str     r2, [r0, #4]
    str     lr, [r0, #8]
    mrs     r2, spsr
    str     r2, [r0, #12]
    mrc     p15, 0, r2, c5, c0, 0   @ DFSR
    str     r2, [r0, #16]
    mrc     p15, 0, r2, c6, c0, 0   @ DFAR
    str     r2, [r0, #20]
    mrc     p15, 0, r2, c5, c0, 1   @ IFSR
    str     r2, [r0, #24]
    mrc     p15, 0, r2, c6, c0, 2   @ IFAR
    str     r2, [r0, #28]
8:  add     r1, r1, #1
    str     r1, [r0]
    pop     {r0-r3}
    ldr     sp, =azahar_seal_flag
    ldr     sp, [sp]
    cmp     sp, #0
    beq     7f
    .if \reason == 4
    @ Sealed data aborts are retried a few times before parking. Kept as a generic guard:
    @ re-executing the aborted store costs nothing, and a recoverable one-off (spurious
    @ abort, transient walk hiccup) then survives instead of wedging the core. The counter
    @ is cumulative per install and shown in the stray report; past the cap the stub parks
    @ as before. (A wedge that looks like this can be a dropped relocation - see azahar_cpu
    @ below - not a transient fault.)
    ldr     sp, =azahar_cpu + \core * CPU_SIZE + CPU_STRAY_STACK_TOP
    push    {r0-r1}
    ldr     r0, =azahar_cpu + \core * CPU_SIZE + CPU_STRAY_RETRY
    ldr     r1, [r0]
    add     r1, r1, #1
    str     r1, [r0]
    cmp     r1, #16
    pop     {r0-r1}
    bgt     6f
    subs    pc, lr, #8              @ back to the aborted store, mode and cpsr restored
    .endif
6:  b       6b                      @ sealed: nothing of Sony's is mapped to chain to; the
                                    @ record above is the report, the syscall side times out
7:  ldr     sp, =azahar_cpu + \core * CPU_SIZE + \chain
    ldm     sp, {sp, pc}            @ this mode's SP as Sony left it, then Sony's handler
    .endm

@ IRQ. With ICCPMR at 0x10 the only interrupt that reaches this core is our private timer (PPI
@ 29, banked priority 00), so this is the preemption tick. With a guest active: save the user
@ file, acknowledge at the GIC, stop the timer, exit with reason 5. Without one — the timer
@ fired while the host was between runs — acknowledge, stop it, and return to the interrupted
@ instruction with every register intact. Uses its own stack; Sony's SP_irq is put back before
@ the return, and at uninstall.
    .macro AZAHAR_IRQ_STUB core
    .global azahar_vec_irq_\core
    .type azahar_vec_irq_\core, %function
azahar_vec_irq_\core:
    ldr     sp, =azahar_cpu + \core * CPU_SIZE + CPU_PROG_IRQ
    str     sp, [sp]
    ldr     sp, =azahar_cpu + \core * CPU_SIZE + CPU_ACTIVE
    ldr     sp, [sp]
    cmp     sp, #0
    beq     9f
    ldr     sp, =azahar_cpu + \core * CPU_SIZE
    stmia   sp, {r0-r14}^
    nop
    mov     r3, lr                  @ LR_irq: exit_common needs it after the call
    mov     r0, sp
    bl      azahar_irq_ack          @ r0-r2 free now; r3 preserved by it
    mov     lr, r3
    b       azahar_irq_rearm        @ inside an upcall, the timer again; else the exit (5)
9:  ldr     sp, =azahar_cpu + \core * CPU_SIZE + CPU_IRQ_STACK_TOP
    push    {r0-r3, lr}
    ldr     r0, =azahar_cpu + \core * CPU_SIZE
    bl      azahar_irq_ack
    ldr     r0, =azahar_cpu + \core * CPU_SIZE
    ldr     r1, [r0, #CPU_IRQ_STRAY]
    add     r1, r1, #1
    str     r1, [r0, #CPU_IRQ_STRAY]
    pop     {r0-r3, lr}
    ldr     sp, =azahar_cpu + \core * CPU_SIZE + CPU_CHAIN_IRQ
    ldr     sp, [sp]                @ Sony's SP_irq
    subs    pc, lr, #4
    .endm

@ A core's five stubs, the addresses its vector table names (native.c, azahar_stubs), and their
@ literals.
    .macro AZAHAR_CORE core
    AZAHAR_STUB \core, und, 2, CPU_PROG_UND, CPU_CHAIN_UND
    AZAHAR_STUB \core, svc, 1, CPU_PROG_SVC, CPU_CHAIN_SVC
    AZAHAR_STUB \core, pabt, 3, CPU_PROG_PABT, CPU_CHAIN_PABT
    AZAHAR_STUB \core, dabt, 4, CPU_PROG_DABT, CPU_CHAIN_DABT
    AZAHAR_IRQ_STUB \core
    .ltorg
    .endm

    AZAHAR_CORE 0
    AZAHAR_CORE 1
    AZAHAR_CORE 2
    AZAHAR_CORE 3

@ ---------------------------------------------------------------------------- svc fast path
@ The svcs answered without leaving the guest (azahar_native.h, AZAHAR_FAST_*). Reached from a
@ core's svc stub with a guest active: Supervisor mode, IRQs masked, SP that core's AzaharFast
@ save words, LR_svc the instruction after the svc, SPSR_svc the guest's CPSR. The guest's r4-r7
@ go to the save words while this works, and an answer goes straight back to the guest with
@ `movs pc, lr`: nothing is stored to ctx, the table, vectors and VFP bank stay, and the slice
@ and its timer run on. Any other svc - or one of these with another handle, or one azaharRun
@ did not allow - takes the ordinary exit with every register as the guest left it. Touches only
@ what the guest's table maps: the svc instruction (guest code is readable at PL1), the core's
@ block (this module's data) and the private timer (the GIC window); and no VFP register, since
@ the bank is the guest's.
azahar_svc_fast:
    stmia   sp, {r4-r7}
    sub     r6, sp, #FAST_SAVE          @ r6 = this core's AzaharFast from here on
    mrs     r4, spsr
    tst     r4, #0x20                   @ Thumb: a 16-bit svc, 8-bit immediate
    ldrhne  r5, [lr, #-2]
    andne   r5, r5, #0xff
    ldreq   r5, [lr, #-4]
    biceq   r5, r5, #0xff000000
    cmp     r5, #0xfc                   @ AZAHAR_TLS_SVC
    beq     10f
    cmp     r5, #0xfb                   @ AZAHAR_CLOCK_SVC
    beq     11f
    ldr     r7, [r6, #FAST_ON]
    cmp     r7, #0
    beq     8f
    cmp     r5, #0x28
    beq     1f
    cmp     r5, #0xfe                   @ AZAHAR_VFP_SVC
    beq     5f
    cmp     r5, #0xfd                   @ AZAHAR_SWITCH_SVC
    beq     9f
    cmp     r5, #0x37
    beq     3f
    cmp     r5, #0x35
    beq     4f
    sub     r4, r5, #0x52
    cmp     r4, #2
    bls     2f
    b       8f

1:  @ GetSystemTick: r1:r0 = base + spent * mult, spent the timer ticks since entry. native.c
    @ computes guest_ticks at the exit the same way, from a smaller counter, so no answer here
    @ is ever ahead of the clock the app keeps.
    tst     r7, #(1 << 0)
    beq     8f
    ldr     r4, [r6, #FAST_TIMER]
    cmp     r4, #0
    beq     8f
    ldr     r4, [r4]                    @ the counter, down from the quantum (0 once expired)
    ldr     r5, [r6, #FAST_QUANTUM]
    sub     r5, r5, r4                  @ spent
    ldr     r4, [r6, #FAST_MULT_FRAC]
    umull   r0, r1, r5, r4              @ r1 = spent * frac >> 32
    ldr     r4, [r6, #FAST_MULT_INT]
    umull   r0, r7, r5, r4              @ r7:r0 = spent * int
    adds    r0, r0, r1
    adc     r7, r7, #0
    ldr     r4, [r6, #FAST_BASE_LO]
    ldr     r5, [r6, #FAST_BASE_HI]
    adds    r0, r0, r4
    adc     r1, r7, r5
    mov     r4, #(FAST_COUNT + 0 * 4)
    b       7f

2:  @ Invalidate/Store/FlushProcessDataCache on the current process: success
    tst     r7, #(1 << 1)
    beq     8f
    ldr     r4, =0xffff8001
    cmp     r0, r4
    bne     8f
    mov     r0, #0
    mov     r4, #(FAST_COUNT + 1 * 4)
    b       7f

3:  @ GetThreadId on the current thread: r0 = 0, r1 = the ID
    tst     r7, #(1 << 2)
    beq     8f
    ldr     r4, =0xffff8000
    cmp     r1, r4
    bne     8f
    mov     r0, #0
    ldr     r1, [r6, #FAST_THREAD_ID]
    mov     r4, #(FAST_COUNT + 2 * 4)
    b       7f

4:  @ GetProcessId on the current process: r0 = 0, r1 = the ID
    tst     r7, #(1 << 3)
    beq     8f
    ldr     r4, =0xffff8001
    cmp     r1, r4
    bne     8f
    mov     r0, #0
    ldr     r1, [r6, #FAST_PROCESS_ID]
    mov     r4, #(FAST_COUNT + 3 * 4)
    b       7f

5:  @ A VFP instruction the app patched into this svc (azaharVfpPatch): azahar_vfp_svc runs it
    @ by the svc's address, on the core's VFP stack, with the guest's r0-r3, r12 and LR_svc
    @ kept around the call (r4-r7 are in the save words; r6 and r7 survive it). Not registered:
    @ the ordinary svc exit, which the app answers itself.
    tst     r7, #(1 << 4)
    beq     8f
    mrs     r4, spsr
    tst     r4, #0x20
    subne   r4, lr, #2                  @ the svc's address: Thumb's is 16 bits
    subeq   r4, lr, #4
    add     sp, r6, #(CPU_VFP_STACK_TOP - CPU_FAST - 8) @ two adds: no one immediate takes it
    add     sp, sp, #8
    push    {r0-r3, r12, lr}
    mov     r0, r4
    bl      azahar_vfp_svc
    mov     r5, r0
    pop     {r0-r3, r12, lr}
    cmp     r5, #0
    beq     8f
    mov     r4, #(FAST_COUNT + 4 * 4)

7:  @ answered: count it, record an ARM site for the app to patch (AzaharRunRequest.fast_site;
    @ the VFP svc's are patched already, a Thumb svc has no room for a branch), put r4-r7 back,
    @ resume the guest after its svc
    ldr     r5, [r6, r4]
    add     r5, r5, #1
    str     r5, [r6, r4]
    cmp     r4, #(FAST_COUNT + 4 * 4)
    beq     6f
    mrs     r5, spsr
    tst     r5, #0x20
    streq   lr, [r6, #FAST_SITE]
6:  add     sp, r6, #FAST_SAVE
    ldmia   sp, {r4-r7}
    movs    pc, lr

10: @ AZAHAR_TLS_SVC: TPIDRURO = r12, for the app's upcalls (their C++ runs with the app
    @ thread's; the thunk puts the guest's back)
    mcr     p15, 0, r12, c13, c0, 3
    add     sp, r6, #FAST_SAVE
    ldmia   sp, {r4-r7}
    movs    pc, lr

11: @ AZAHAR_CLOCK_SVC: r1:r0 = the guest ticks since the clock's last start; the private timer
    @ again over r1 ticks (one shot, its interrupt on); the guest clock from where it is
    @ (r0 = 0) or from r3:r2; the user page follows. r2, r3 and r12 are the svc's. No timer
    @ (an untimed slice): r1:r0 = 0 and nothing changes.
    ldr     r4, [r6, #FAST_TIMER]
    cmp     r4, #0
    moveq   r0, #0
    moveq   r1, #0
    beq     12f
    ldr     r5, [r4]                    @ the counter, down from the quantum (0 once expired)
    ldr     r7, [r6, #FAST_QUANTUM]
    sub     r5, r7, r5                  @ spent
    mov     r7, #0
    str     r7, [r4, #PTIMER_CONTROL_FROM_COUNTER]
    mov     r7, #1
    str     r7, [r4, #PTIMER_STATUS_FROM_COUNTER]
    str     r1, [r4, #PTIMER_LOAD_FROM_COUNTER]
    str     r1, [r6, #FAST_QUANTUM]
    mov     r7, #5                      @ enable, interrupt, no reload
    str     r7, [r4, #PTIMER_CONTROL_FROM_COUNTER]
    ldr     r4, [r6, #FAST_MULT_FRAC]   @ r4:r1 = spent * mult, as GetSystemTick's
    umull   r7, r12, r5, r4
    ldr     r4, [r6, #FAST_MULT_INT]
    umull   r1, r4, r5, r4
    adds    r1, r1, r12
    adc     r4, r4, #0
    ldr     r7, [r6, #FAST_ACCRUED_LO]  @ and the quantums re-armed since the last start
    ldr     r12, [r6, #FAST_ACCRUED_HI]
    adds    r1, r1, r7
    adc     r4, r4, r12
    mov     r7, #0
    str     r7, [r6, #FAST_ACCRUED_LO]
    str     r7, [r6, #FAST_ACCRUED_HI]
    cmp     r0, #0
    ldreq   r2, [r6, #FAST_BASE_LO]
    ldreq   r3, [r6, #FAST_BASE_HI]
    addseq  r2, r2, r1
    adceq   r3, r3, r4
    str     r2, [r6, #FAST_BASE_LO]
    str     r3, [r6, #FAST_BASE_HI]
    ldr     r7, [r6, #FAST_USER_KVA]
    ldr     r12, [r6, #FAST_QUANTUM]
    str     r12, [r7, #USER_QUANTUM]
    str     r2, [r7, #USER_BASE_LO]
    str     r3, [r7, #USER_BASE_HI]
    ldr     r12, [r7, #USER_GEN]
    add     r12, r12, #1
    str     r12, [r7, #USER_GEN]
    mov     r0, r1
    mov     r1, r4
12: add     sp, r6, #FAST_SAVE
    ldmia   sp, {r4-r7}
    movs    pc, lr

9:  @ AZAHAR_SWITCH_SVC: the thread switch the app armed from the guest's world (AzaharSwitch):
    @ the next thread's state loaded and resumed, no exit. The outgoing thread's state is the
    @ app's (the upcall saved it). Not armed: the ordinary exit, which reschedules in the app.
    ldr     r4, [r6, #FAST_SWITCH_VA]
    cmp     r4, #0
    beq     8f
    ldr     r5, [r4, #SWITCH_ARMED]
    cmp     r5, #0
    beq     8f
    mov     r5, #0
    str     r5, [r4, #SWITCH_ARMED]
    ldr     r7, [r4, #SWITCH_THREAD_ID]
    ldr     r5, [r4, #SWITCH_IN]
    str     r7, [r6, #FAST_THREAD_ID]
    ldr     r4, [r6, #FAST_USER_KVA]
    str     r7, [r4, #USER_THREAD_ID]
    add     r4, r5, #0x50
    vldmia  r4!, {d0-d15}
    vldmia  r4, {d16-d31}
    ldr     r4, [r5, #0x4c]
    vmsr    fpscr, r4
    ldr     r4, [r5, #0x44]
    mcr     p15, 0, r4, c13, c0, 3      @ TPIDRURO
    ldr     r4, [r5, #0x48]
    mcr     p15, 0, r4, c13, c0, 2      @ TPIDRURW
    ldr     r4, [r5, #0x40]
    bic     r4, r4, #0x1d0              @ user mode, A/I/F clear, T kept (as azaharRun does)
    bic     r4, r4, #0x00f
    orr     r4, r4, #0x10
    msr     spsr_cxsf, r4
    ldr     lr, [r5, #0x3c]             @ its pc
    clrex                               @ no reservation crosses a thread switch
    add     r4, r5, #0x34
    ldmia   r4, {r13, r14}^             @ its user sp and lr
    nop
    ldmia   r5, {r0-r12}
    movs    pc, lr

8:  @ not answered here. With the app's redirect (AzaharRedirect): a guest svc goes to the app's
    @ upcall thunk instead of exiting (its number and return address in the record). Not one from
    @ the app's own code (svc 0, a Sony syscall the app forwards), the module's own (0xFD, 0xFE),
    @ or one in an IT block, whose state user mode cannot put back: those exit.
    ldr     r4, [r6, #FAST_REDIRECT_VA]
    cmp     r4, #0
    beq     18f
    cmp     lr, #0x40000000
    bhs     18f
    mrs     r7, spsr
    tst     r7, #0x06000000             @ IT[1:0]
    bne     18f
    tst     r7, #0x0000fc00             @ IT[7:2]
    bne     18f
    tst     r7, #0x20                   @ the immediate again: the paths here changed r5
    ldrhne  r5, [lr, #-2]
    andne   r5, r5, #0xff
    ldreq   r5, [lr, #-4]
    biceq   r5, r5, #0xff000000
    cmp     r5, #0
    beq     18f
    cmp     r5, #0xfd
    beq     18f
    cmp     r5, #0xfe
    beq     18f
    str     r5, [r4, #REDIRECT_SVC]
    tst     r7, #0x20
    orrne   r5, lr, #1
    moveq   r5, lr
    str     r5, [r4, #REDIRECT_PC]
    str     r5, [r4, #REDIRECT_SITE]
    ldr     lr, [r4, #REDIRECT_ENTRY]
    bic     r7, r7, #0x20               @ the entry is ARM
    msr     spsr_cxsf, r7
    add     sp, r6, #FAST_SAVE
    ldmia   sp, {r4-r7}
    movs    pc, lr

18: @ the ordinary svc exit, from the core's block
    add     sp, r6, #FAST_SAVE
    ldmia   sp, {r4-r7}
    sub     sp, sp, #(CPU_FAST + FAST_SAVE)
    stmia   sp, {r0-r14}^
    nop
    mov     r0, #1
    b       azahar_exit_common

@ Acknowledge whatever is at the CPU interface, stop the private timer, record the ID in the
@ block at r0. Clobbers r0-r2 only.
azahar_irq_ack:
    ldr     r1, =azahar_gic_va
    ldr     r1, [r1]
    cmp     r1, #0
    bxeq    lr
    ldr     r2, [r1, #0x10c]        @ GICC_IAR
    str     r2, [r0, #CPU_IRQ_LAST]
    lsl     r0, r2, #22
    lsr     r0, r0, #22             @ the ID field, bits [9:0]
    add     r0, r0, #1
    cmp     r0, #0x400              @ ID 0x3ff is spurious: nothing to end
    strne   r2, [r1, #0x110]        @ GICC_EOIR
    mov     r2, #0
    str     r2, [r1, #0x608]        @ private timer control: off
    mov     r2, #1
    str     r2, [r1, #0x60c]        @ interrupt status: clear
    dsb     sy
    bx      lr

@ The private timer's quantum ended inside an upcall (the app's code, at or above 0x40000000,
@ which the guest's never is): the timer again over AzaharFast.rearm_ticks and back into the
@ upcall, no exit, the ended quantum added to the guest clock (the fast svcs' base, the user page)
@ and to AzaharFast.accrued for the exit to report. An exit instead for the guest's own code, any
@ other interrupt, a pending SGI (a cross-core call waits for the core to be Sony's), or no
@ re-arms left. sp = the core's block with the user file saved, lr = LR_irq; r0-r12 are free
@ (put back from the block).
azahar_irq_rearm:
    ldr     r4, [sp, #(CPU_FAST + FAST_REARM_TICKS)]
    cmp     r4, #0
    beq     8f
    sub     r1, lr, #4
    cmp     r1, #0x40000000
    blo     8f
    ldr     r1, [sp, #CPU_IRQ_LAST]
    lsl     r1, r1, #22
    lsr     r1, r1, #22
    cmp     r1, #29                 @ the private timer's PPI
    bne     8f
    ldr     r1, [sp, #(CPU_FAST + FAST_REARM_LEFT)]
    cmp     r1, #0
    beq     8f
    ldr     r5, =azahar_gic_va
    ldr     r5, [r5]
    add     r6, r5, #0x1000         @ the distributor
    ldr     r2, [r6, #0x200]        @ GICD_ISPENDR0
    ldr     r7, [r6, #0x100]        @ GICD_ISENABLER0
    and     r2, r2, r7
    lsls    r2, r2, #17             @ SGIs 0-14, as core2_sgi_window
    bne     8f
    sub     r1, r1, #1
    str     r1, [sp, #(CPU_FAST + FAST_REARM_LEFT)]
    ldr     r7, [sp, #(CPU_FAST + FAST_QUANTUM)]
    ldr     r2, [sp, #(CPU_FAST + FAST_MULT_FRAC)]
    umull   r0, r1, r7, r2          @ r1 = quantum * frac >> 32
    ldr     r2, [sp, #(CPU_FAST + FAST_MULT_INT)]
    umull   r2, r3, r7, r2          @ r3:r2 = quantum * int
    adds    r2, r2, r1
    adc     r3, r3, #0              @ r3:r2 = the quantum on the guest clock
    ldr     r0, [sp, #(CPU_FAST + FAST_BASE_LO)]
    ldr     r1, [sp, #(CPU_FAST + FAST_BASE_HI)]
    adds    r0, r0, r2
    adc     r1, r1, r3
    str     r0, [sp, #(CPU_FAST + FAST_BASE_LO)]
    str     r1, [sp, #(CPU_FAST + FAST_BASE_HI)]
    ldr     r8, [sp, #(CPU_FAST + FAST_ACCRUED_LO)]
    ldr     r9, [sp, #(CPU_FAST + FAST_ACCRUED_HI)]
    adds    r8, r8, r2
    adc     r9, r9, r3
    str     r8, [sp, #(CPU_FAST + FAST_ACCRUED_LO)]
    str     r9, [sp, #(CPU_FAST + FAST_ACCRUED_HI)]
    str     r4, [sp, #(CPU_FAST + FAST_QUANTUM)]
    mov     r2, #0
    str     r2, [r5, #0x608]        @ private timer: off (the ack stopped it)
    mov     r2, #1
    str     r2, [r5, #0x60c]        @ its event cleared
    str     r4, [r5, #0x600]        @ load
    mov     r2, #5
    str     r2, [r5, #0x608]        @ enable, interrupt, one shot
    ldr     r6, [sp, #(CPU_FAST + FAST_USER_KVA)]
    str     r4, [r6, #USER_QUANTUM]
    str     r0, [r6, #USER_BASE_LO]
    str     r1, [r6, #USER_BASE_HI]
    ldr     r2, [r6, #USER_GEN]
    add     r2, r2, #1
    str     r2, [r6, #USER_GEN]
    ldr     r2, [sp, #(CPU_FAST + FAST_REARMS)]
    add     r2, r2, #1
    str     r2, [sp, #(CPU_FAST + FAST_REARMS)]
    dsb     sy
    isb
    ldmia   sp, {r0-r14}^           @ the upcall's registers, and on
    nop
    subs    pc, lr, #4
8:  mov     r0, #5
    b       azahar_exit_common

@ sp = the core's block (its ctx, this mode's SP), r0 = reason, guest r0-r14 already in ctx.
azahar_exit_common:
    add     r2, r0, #30             @ 31..35: the user file is saved
    str     r2, [sp, #CPU_PROGRESS]
    mov     r2, #0
    str     r2, [sp, #CPU_ACTIVE]
    str     lr, [sp, #0x3c]         @ raw exception LR; C adjusts it per reason
    mrs     r1, spsr
    str     r1, [sp, #0x40]         @ the guest CPSR at the exception
    add     r2, sp, #0x50
    vstmia  r2!, {d0-d15}
    vstmia  r2, {d16-d31}
    vmrs    r3, fpscr
    str     r3, [sp, #0x4c]
    add     r1, sp, #CPU_EXIT
    str     r0, [r1, #0x00]         @ reason
    str     lr, [r1, #0x04]         @ raw lr
    mrs     r2, spsr
    str     r2, [r1, #0x08]         @ spsr
    mrc     p15, 0, r2, c5, c0, 0   @ DFSR
    str     r2, [r1, #0x0c]
    mrc     p15, 0, r2, c6, c0, 0   @ DFAR
    str     r2, [r1, #0x10]
    mrc     p15, 0, r2, c5, c0, 1   @ IFSR
    str     r2, [r1, #0x14]
    mrc     p15, 0, r2, c6, c0, 2   @ IFAR
    str     r2, [r1, #0x18]

    @ Back to the host: Sony's SP_svc first, then the host's own mode, sp and callee-saved
    @ registers, then return to the C caller of azahar_enter with the reason in r0.
    add     r1, sp, #CPU_HOST
    cps     #0x13
    ldr     sp, [r1, #0x08]         @ SP_svc as it was
    mov     r3, #40
    str     r3, [r1, #(CPU_PROGRESS - CPU_HOST)]
    ldr     r2, [r1, #0x00]
    bic     r2, r2, #0x20           @ MSR ignores T anyway; keep the write honest
    msr     cpsr_c, r2              @ host mode, host I/F
    ldr     sp, [r1, #0x04]         @ host sp
    mov     r3, #41
    str     r3, [r1, #(CPU_PROGRESS - CPU_HOST)]
    pop     {r4-r11, pc}            @ r0 = reason; interworks back to Thumb

@ ---------------------------------------------------------------------------- banked SPs
@ uint32_t azahar_get_banked_sp(uint32_t mode); void azahar_set_banked_sp(uint32_t mode, uint32_t sp).
@ Switches to the named mode with interrupts masked, reads or writes its SP, and comes back.
@ Used to save Sony's SP_und and SP_abt before our vectors clobber them and restore them after.

    .global azahar_get_banked_sp
    .type azahar_get_banked_sp, %function
azahar_get_banked_sp:
    mrs     r1, cpsr
    cpsid   if
    mrs     r2, cpsr
    bic     r2, r2, #0x1f
    orr     r2, r2, r0
    msr     cpsr_c, r2
    mov     r0, sp
    msr     cpsr_c, r1
    bx      lr

    .global azahar_set_banked_sp
    .type azahar_set_banked_sp, %function
azahar_set_banked_sp:
    mrs     r2, cpsr
    cpsid   if
    mrs     r3, cpsr
    bic     r3, r3, #0x1f
    orr     r3, r3, r0
    msr     cpsr_c, r3
    mov     sp, r1
    msr     cpsr_c, r2
    bx      lr

    .ltorg

@ ---------------------------------------------------------------------------- data
    .data
    .align 2
    .global azahar_gic_va
azahar_gic_va:     .word 0
    .global azahar_seal_flag
azahar_seal_flag:  .word 0
    .bss
    .align 3
    .global azahar_cpu
azahar_cpu:        .space 4 * CPU_SIZE
    .space 8                        @ keeps every address the stubs name strictly inside the
                                    @ segment: a symbol at the exact end (one-past-the-end
                                    @ address) gets its relocations silently DROPPED by
                                    @ vita-elf-create - it cannot attribute the address to
                                    @ a segment - so every reference to it stays at the
                                    @ link-time address after load (the stray pushes abort
                                    @ on the unmapped link VA, the seal ranges stretch
                                    @ toward it, and a C dereference of it crashes the
                                    @ resident thread outright)

    .section .note.GNU-stack,"",%progbits
