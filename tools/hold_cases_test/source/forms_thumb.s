@ SPDX-FileCopyrightText: 2026 Willi Ye
@ SPDX-License-Identifier: GPL-2.0-or-later

@ hold_cases_test's Thumb forms, as forms_arm.s's: f(base, out), and across a page boundary
@ f(b, out) on [b - 8, b + 8).
	.syntax unified
	.arch armv6k
	.thumb
	.text

	.macro tform name
	.align 2
	.global \name
	.thumb_func
	.type \name, %function
\name:
	.endm

@ STMIA written back, its base the lowest register in the list (the original base stored).
tform t_stm_base
	push	{r4-r7, lr}
	mov	r4, r0
	mov	r2, r0
	movs	r3, #0x77
	stmia	r2!, {r2, r3}
	ldr	r0, [r4, #0]
	subs	r0, r0, r4
	str	r0, [r1, #0]		@ 0
	ldr	r0, [r4, #4]
	str	r0, [r1, #4]		@ 0x77
	subs	r0, r2, r4
	str	r0, [r1, #8]		@ 8
	pop	{r4-r7, pc}

@ POP with the PC from a stack at base: r0's value, then the return (a Thumb address).
tform t_pop_pc
	push	{r4-r7, lr}
	mov	r4, r0
	mov	r5, r1
	ldr	r2, =0x4321
	str	r2, [r4, #0]
	adr	r2, 1f
	adds	r2, #1
	str	r2, [r4, #4]
	mov	r7, sp
	mov	sp, r4
	movs	r0, #0
	pop	{r0, pc}
	.align 2
1:	mov	r6, sp
	mov	sp, r7
	str	r0, [r5, #0]		@ 0x4321
	subs	r6, r6, r4
	str	r6, [r5, #4]		@ 8
	pop	{r4-r7, pc}
	.ltorg

@ STMIA, then LDMIA, over [b - 8, b + 8).
tform xt_stmia
	push	{r4-r7, lr}
	mov	r4, r0
	subs	r4, #8
	ldr	r0, =0x0D100001
	adds	r2, r0, #1
	mov	r5, r1
	adds	r1, r0, #1
	adds	r2, r0, #2
	adds	r3, r0, #3
	mov	r6, r4
	stmia	r6!, {r0-r3}
	ldr	r0, [r4, #0]
	str	r0, [r5, #0]
	ldr	r0, [r4, #4]
	str	r0, [r5, #4]
	ldr	r0, [r4, #8]
	str	r0, [r5, #8]
	ldr	r0, [r4, #12]
	str	r0, [r5, #12]
	pop	{r4-r7, pc}

tform xt_ldmia
	push	{r4-r7, lr}
	mov	r4, r0
	subs	r4, #8
	mov	r5, r1
	ldr	r0, =0x0D200001
	str	r0, [r4, #0]
	adds	r0, #1
	str	r0, [r4, #4]
	adds	r0, #1
	str	r0, [r4, #8]
	adds	r0, #1
	str	r0, [r4, #12]
	mov	r6, r4
	ldmia	r6!, {r0-r3}
	stmia	r5!, {r0-r3}
	pop	{r4-r7, pc}

@ PUSH, then POP, with SP at b + 8.
tform xt_push
	push	{r4-r7, lr}
	mov	r4, r0
	mov	r5, r1
	ldr	r0, =0x0D300001
	adds	r1, r0, #1
	adds	r2, r0, #2
	adds	r3, r0, #3
	mov	r7, sp
	mov	r6, r4
	adds	r6, #8
	mov	sp, r6
	push	{r0-r3}
	mov	sp, r7
	subs	r4, #8
	ldr	r0, [r4, #0]
	str	r0, [r5, #0]
	ldr	r0, [r4, #4]
	str	r0, [r5, #4]
	ldr	r0, [r4, #8]
	str	r0, [r5, #8]
	ldr	r0, [r4, #12]
	str	r0, [r5, #12]
	pop	{r4-r7, pc}

tform xt_pop
	push	{r4-r7, lr}
	mov	r4, r0
	mov	r5, r1
	subs	r4, #8
	ldr	r0, =0x0D400001
	str	r0, [r4, #0]
	adds	r0, #1
	str	r0, [r4, #4]
	adds	r0, #1
	str	r0, [r4, #8]
	adds	r0, #1
	str	r0, [r4, #12]
	mov	r7, sp
	mov	sp, r4
	pop	{r0-r3}
	mov	sp, r7
	stmia	r5!, {r0-r3}
	pop	{r4-r7, pc}
	.ltorg
