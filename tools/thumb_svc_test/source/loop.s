@ SPDX-FileCopyrightText: 2026 Willi Ye
@ SPDX-License-Identifier: GPL-2.0-or-later

@ thumb_svc_loop(n): svcSleepThread(1000 ns) n times from one Thumb svc site whose next
@ instruction (adds r4, #1) is the kind the emulator's Thumb svc patch moves into its stub;
@ returns how many times that instruction ran (n when the patched site resumes right).
    .syntax unified
    .thumb
    .text
    .global thumb_svc_loop
    .type thumb_svc_loop, %function
    .align 2
thumb_svc_loop:
    push {r4, r5, r6, lr}
    movs r4, #0
    mov r5, r0
1:
    movs r0, #250
    lsls r0, r0, #2         @ 1000 ns
    movs r1, #0
    svc 0x0A
    adds r4, #1
    cmp r4, r5
    bne 1b
    mov r0, r4
    pop {r4, r5, r6, pc}
    .size thumb_svc_loop, . - thumb_svc_loop

@ thumb_svc_long(n): svcSleepThread(4 ms) n times from a second Thumb svc site (the same moved
@ instruction): each sleep idles the core past the slice, so the slice ends while the thread waits
@ in the svc; returns how many times the moved instruction ran.
    .global thumb_svc_long
    .type thumb_svc_long, %function
    .align 2
thumb_svc_long:
    push {r4, r5, r6, lr}
    movs r4, #0
    mov r5, r0
1:
    ldr r0, =4000000        @ 4 ms
    movs r1, #0
    svc 0x0A
    adds r4, #1
    cmp r4, r5
    bne 1b
    mov r0, r4
    pop {r4, r5, r6, pc}
    .ltorg
    .size thumb_svc_long, . - thumb_svc_long
