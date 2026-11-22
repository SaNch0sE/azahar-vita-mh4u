@ SPDX-FileCopyrightText: 2026 Willi Ye
@ SPDX-License-Identifier: GPL-2.0-or-later

@ hold_cases_test's ARM forms: each a function f(base, out) making one load or store form on
@ base's bytes (8-aligned, [0, 0x20) used) and leaving what it loaded in out[0..3] (plain
@ memory), run on a page the GPU holds and on plain memory, compared.
@
@ The user-privilege forms LDRT/STRT are left out: upstream Azahar takes them as undefined, so no
@ title relies on them. SWP is undefined on the Vita (SCTLR.SW clear) and no title's code has one.
	.syntax unified
	.arch armv6k
	.fpu vfpv2
	.arm
	.text

	.macro form name
	.align 2
	.global \name
	.type \name, %function
\name:
	.endm

@ A register offset by RRX, C clear: [r7, r8, rrx] is r7 + r8 / 2.
form f_rrx
	push	{r4-r8, lr}
	add	r7, r0, #4
	mov	r8, #8
	ldr	r2, =0x0EEE0003
	msr	CPSR_f, #0
	str	r2, [r7, r8, rrx]
	msr	CPSR_f, #0
	ldr	r3, [r7, r8, rrx]
	str	r3, [r1, #0]
	pop	{r4-r8, pc}

@ LDR to the PC, an ARM address.
form f_ldr_pc
	push	{r4-r6, lr}
	adr	r2, 1f
	str	r2, [r0, #0]
	mov	r6, #0
	ldr	pc, [r0, #0]
	mov	r6, #0x99
1:	add	r6, r6, #1
	str	r6, [r1, #0]		@ 1
	pop	{r4-r6, pc}

@ LDR to the PC, a Thumb address (interworking).
form f_ldr_pc_thumb
	push	{r4-r9, lr}
	adr	r9, 2f
	ldr	r2, =thumb_back_r6_2
	str	r2, [r0, #0]
	mov	r6, #0
	ldr	pc, [r0, #0]
2:	str	r6, [r1, #0]		@ 2, the stub's
	pop	{r4-r9, pc}

@ LDR to the PC post-indexed, as POP {pc} is: the base written back.
form f_ldr_pc_post
	push	{r4-r6, lr}
	adr	r2, 3f
	str	r2, [r0, #0]
	mov	r4, r0
	mov	r6, #0
	ldr	pc, [r4], #4
	mov	r6, #0x97
3:	add	r6, r6, #3
	str	r6, [r1, #0]		@ 3
	sub	r4, r4, r0
	str	r4, [r1, #4]		@ 4
	pop	{r4-r6, pc}

@ LDM with the PC.
form f_ldm_pc
	push	{r4-r6, lr}
	ldr	r2, =0x5150
	str	r2, [r0, #0]
	adr	r2, 4f
	str	r2, [r0, #4]
	mov	r6, #0
	ldmia	r0, {r6, pc}
	mov	r6, #0x98
4:	str	r6, [r1, #0]		@ 0x5150
	pop	{r4-r6, pc}

@ STM written back, its base the lowest register in the list (the original base stored).
form f_stm_base
	push	{r4-r6, lr}
	mov	r4, r0
	mov	r2, r0
	ldr	r6, =0x600D
	stmia	r2!, {r2, r6}
	ldr	r3, [r4, #0]
	sub	r3, r3, r4
	str	r3, [r1, #0]		@ 0
	ldr	r3, [r4, #4]
	str	r3, [r1, #4]		@ 0x600D
	sub	r3, r2, r4
	str	r3, [r1, #8]		@ 8
	pop	{r4-r6, pc}

@ STR of the PC (the instruction's address + 8, as the ARM11 and the Cortex-A9 store it).
form f_str_pc
	push	{r4, lr}
5:	str	pc, [r0, #0]
	ldr	r3, [r0, #0]
	adr	r2, 5b
	sub	r3, r3, r2
	str	r3, [r1, #0]		@ 8
	pop	{r4, pc}

@ STM with the PC in the list.
form f_stm_pc
	push	{r4-r6, lr}
	ldr	r6, =0x5717
6:	stmia	r0, {r6, pc}
	ldr	r3, [r0, #0]
	str	r3, [r1, #0]		@ 0x5717
	ldr	r3, [r0, #4]
	adr	r2, 6b
	sub	r3, r3, r2
	str	r3, [r1, #4]		@ 8
	pop	{r4-r6, pc}

@ LDREX, STREX: an increment, at most 100 tries (out[1] 1 when out of tries).
form f_ldrex
	push	{r4-r10, lr}
	mov	r2, #5
	str	r2, [r0, #0]
	mov	r10, #0
7:	add	r10, r10, #1
	cmp	r10, #100
	bhs	8f
	ldrex	r6, [r0]
	add	r6, r6, #1
	strex	r7, r6, [r0]
	cmp	r7, #0
	bne	7b
8:	ldr	r6, [r0, #0]
	str	r6, [r1, #0]		@ 6
	cmp	r10, #100
	movlo	r10, #0
	movhs	r10, #1
	str	r10, [r1, #4]
	pop	{r4-r10, pc}

form f_ldrexb
	push	{r4-r10, lr}
	mov	r2, #0x40
	strb	r2, [r0, #0]
	mov	r10, #0
9:	add	r10, r10, #1
	cmp	r10, #100
	bhs	10f
	ldrexb	r6, [r0]
	add	r6, r6, #1
	strexb	r7, r6, [r0]
	cmp	r7, #0
	bne	9b
10:	ldrb	r6, [r0, #0]
	str	r6, [r1, #0]		@ 0x41
	cmp	r10, #100
	movlo	r10, #0
	movhs	r10, #1
	str	r10, [r1, #4]
	pop	{r4-r10, pc}

form f_ldrexh
	push	{r4-r10, lr}
	ldr	r2, =0x1230
	strh	r2, [r0, #0]
	mov	r10, #0
11:	add	r10, r10, #1
	cmp	r10, #100
	bhs	12f
	ldrexh	r6, [r0]
	add	r6, r6, #1
	strexh	r7, r6, [r0]
	cmp	r7, #0
	bne	11b
12:	ldrh	r6, [r0, #0]
	str	r6, [r1, #0]		@ 0x1231
	cmp	r10, #100
	movlo	r10, #0
	movhs	r10, #1
	str	r10, [r1, #4]
	pop	{r4-r10, pc}

form f_ldrexd
	push	{r4-r10, lr}
	ldr	r2, =0x10001
	ldr	r3, =0x20002
	strd	r2, r3, [r0, #0]
	mov	r10, #0
13:	add	r10, r10, #1
	cmp	r10, #100
	bhs	14f
	ldrexd	r6, r7, [r0]
	add	r6, r6, #1
	add	r7, r7, #1
	strexd	r8, r6, r7, [r0]
	cmp	r8, #0
	bne	13b
14:	ldrd	r6, r7, [r0, #0]
	str	r6, [r1, #0]		@ 0x10002
	str	r7, [r1, #4]		@ 0x20003
	cmp	r10, #100
	movlo	r10, #0
	movhs	r10, #1
	str	r10, [r1, #8]
	pop	{r4-r10, pc}

@ An exclusive load alone (cleared), then a plain store and load of the word.
form f_ldrex_alone
	push	{r4-r6, lr}
	ldr	r2, =0x7007
	str	r2, [r0, #0]
	ldrex	r6, [r0]
	clrex
	str	r6, [r1, #0]		@ 0x7007
	ldr	r2, =0x8008
	str	r2, [r0, #0]
	ldr	r6, [r0, #0]
	str	r6, [r1, #4]		@ 0x8008
	pop	{r4-r6, pc}
	.ltorg

@ Across a page boundary: f(b, out) with b 4 KiB aligned, the 16 bytes [b - 8, b + 8) used.
@ Each loads back what it stored (the loads' words set by single stores first where the form is
@ a load).
form x_stm
	push	{r4-r9, lr}
	ldr	r6, =0x0A000001
	add	r7, r6, #1
	add	r8, r6, #2
	add	r9, r6, #3
	sub	r4, r0, #8
	stmia	r4, {r6-r9}
	ldr	r2, [r4, #0]
	str	r2, [r1, #0]
	ldr	r2, [r4, #4]
	str	r2, [r1, #4]
	ldr	r2, [r4, #8]
	str	r2, [r1, #8]
	ldr	r2, [r4, #12]
	str	r2, [r1, #12]
	pop	{r4-r9, pc}

form x_ldm
	push	{r4-r9, lr}
	sub	r4, r0, #8
	ldr	r2, =0x0B000001
	str	r2, [r4, #0]
	add	r2, r2, #1
	str	r2, [r4, #4]
	add	r2, r2, #1
	str	r2, [r4, #8]
	add	r2, r2, #1
	str	r2, [r4, #12]
	ldmia	r4, {r6-r9}
	stmia	r1, {r6-r9}
	pop	{r4-r9, pc}

form x_strd
	push	{r4-r7, lr}
	ldr	r6, =0x0C000001
	add	r7, r6, #1
	strd	r6, r7, [r0, #-4]
	ldr	r2, [r0, #-4]
	str	r2, [r1, #0]
	ldr	r2, [r0, #0]
	str	r2, [r1, #4]
	pop	{r4-r7, pc}

form x_ldrd
	push	{r4-r7, lr}
	ldr	r2, =0x0D000001
	str	r2, [r0, #-4]
	add	r2, r2, #1
	str	r2, [r0, #0]
	ldrd	r6, r7, [r0, #-4]
	str	r6, [r1, #0]
	str	r7, [r1, #4]
	pop	{r4-r7, pc}

form x_vstm
	push	{r4-r9, lr}
	ldr	r6, =0x0E000001
	add	r7, r6, #1
	add	r8, r6, #2
	add	r9, r6, #3
	vmov	d0, r6, r7
	vmov	d1, r8, r9
	sub	r4, r0, #8
	vstmia	r4, {d0-d1}
	ldr	r2, [r4, #0]
	str	r2, [r1, #0]
	ldr	r2, [r4, #4]
	str	r2, [r1, #4]
	ldr	r2, [r4, #8]
	str	r2, [r1, #8]
	ldr	r2, [r4, #12]
	str	r2, [r1, #12]
	pop	{r4-r9, pc}

form x_vldm
	push	{r4-r9, lr}
	sub	r4, r0, #8
	ldr	r2, =0x0F000001
	str	r2, [r4, #0]
	add	r2, r2, #1
	str	r2, [r4, #4]
	add	r2, r2, #1
	str	r2, [r4, #8]
	add	r2, r2, #1
	str	r2, [r4, #12]
	vldmia	r4, {d0-d1}
	vmov	r6, r7, d0
	vmov	r8, r9, d1
	stmia	r1, {r6-r9}
	pop	{r4-r9, pc}
	.ltorg

	.thumb
	.thumb_func
	.align 2
	.type thumb_back_r6_2, %function
thumb_back_r6_2:
	movs	r6, #2
	bx	r9

@ x-held: six words from base - 20 (stmia, five on the page before and one on base's page) or
@ base - 12 (vstmia of s0-s5, three and three), base's first bytes written by a queued transfer.
@ The store is the first access to base's page: a
@ load first waits there, its fault at the page's own address, and opens it. out[0] the words read
@ back, XORed; out[1] 0; out[2] the base written back less the start (24); out[3] 0.
	.arm
	.macro xor6 dst
	eor	\dst, r5, r6
	eor	\dst, \dst, r7
	eor	\dst, \dst, r8
	eor	\dst, \dst, r9
	eor	\dst, \dst, r10
	.endm

	.macro xh_end
	sub	r3, r11, r4
	str	r3, [r1, #8]
	ldmia	r4, {r5-r10}
	xor6	r3
	str	r3, [r1, #0]
	mov	r3, #0
	str	r3, [r1, #4]
	str	r3, [r1, #12]
	pop	{r4-r11, pc}
	.endm

form xh_stm6
	push	{r4-r11, lr}
	sub	r4, r0, #20
	ldr	r5, =0x0C000001
	add	r6, r5, #1
	add	r7, r5, #2
	add	r8, r5, #3
	add	r9, r5, #4
	add	r10, r5, #5
	mov	r11, r4
	stmia	r11!, {r5-r10}
	xh_end

form xh_vstm6
	push	{r4-r11, lr}
	sub	r4, r0, #12
	ldr	r5, =0x0D000001
	add	r6, r5, #1
	add	r7, r5, #2
	add	r8, r5, #3
	add	r9, r5, #4
	add	r10, r5, #5
	vmov	s0, s1, r5, r6
	vmov	s2, s3, r7, r8
	vmov	s4, s5, r9, r10
	mov	r11, r4
	vstmia	r11!, {s0-s5}
	xh_end
	.ltorg
