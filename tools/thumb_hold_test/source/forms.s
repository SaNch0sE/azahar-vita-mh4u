@ SPDX-FileCopyrightText: 2026 Willi Ye
@ SPDX-License-Identifier: GPL-2.0-or-later

@ thumb_forms(base, out): each ARMv6K Thumb load and store form on base's bytes [0, 0x110), the
@ values loaded stored into out[0..19] (plain memory). base must be 8-aligned. The SP forms (SP
@ relative, PUSH, POP) run with SP at base + 0x100.
	.syntax unified
	.arch armv6k
	.thumb
	.text
	.align 2
	.global thumb_forms
	.thumb_func
	.type thumb_forms, %function
thumb_forms:
	push	{r4-r7, lr}
	mov	r4, r0
	mov	r5, r1
	@ STR, LDR Rt, [Rn, #imm5 * 4]
	ldr	r2, =0x11223344
	str	r2, [r4, #4]
	ldr	r3, [r4, #4]
	str	r3, [r5, #0]
	@ STRB, LDRB Rt, [Rn, #imm5]
	movs	r2, #0xA5
	strb	r2, [r4, #9]
	ldrb	r3, [r4, #9]
	str	r3, [r5, #4]
	@ STRH, LDRH Rt, [Rn, #imm5 * 2]
	ldr	r2, =0x8001
	strh	r2, [r4, #12]
	ldrh	r3, [r4, #12]
	str	r3, [r5, #8]
	@ The register offsets: STR, LDR
	movs	r6, #16
	ldr	r2, =0xCAFEBABE
	str	r2, [r4, r6]
	ldr	r3, [r4, r6]
	str	r3, [r5, #12]
	@ STRH, LDRSH, LDRH
	movs	r6, #20
	ldr	r2, =0x1234F00D
	strh	r2, [r4, r6]
	ldrsh	r3, [r4, r6]
	str	r3, [r5, #16]
	ldrh	r3, [r4, r6]
	str	r3, [r5, #20]
	@ STRB, LDRSB, LDRB
	movs	r6, #23
	movs	r2, #0x80
	strb	r2, [r4, r6]
	ldrsb	r3, [r4, r6]
	str	r3, [r5, #24]
	ldrb	r3, [r4, r6]
	str	r3, [r5, #28]
	@ STMIA, LDMIA, both written back
	mov	r6, r4
	adds	r6, #32
	movs	r0, #0x51
	movs	r1, #0x52
	movs	r2, #0x53
	movs	r3, #0x54
	stmia	r6!, {r0-r3}
	subs	r6, #16
	movs	r0, #0
	movs	r1, #0
	movs	r2, #0
	movs	r3, #0
	ldmia	r6!, {r0-r3}
	str	r0, [r5, #32]
	str	r1, [r5, #36]
	str	r2, [r5, #40]
	str	r3, [r5, #44]
	subs	r0, r6, r4
	str	r0, [r5, #48]
	@ LDMIA with its base in the list: the base takes the loaded value, no writeback
	subs	r6, #16
	ldm	r6, {r0, r6}
	str	r0, [r5, #52]
	str	r6, [r5, #56]
	@ The SP forms, SP at base + 0x100
	mov	r7, sp
	mov	r0, r4
	adds	r0, #255
	adds	r0, #1
	mov	sp, r0
	ldr	r0, =0x5A5A0001
	str	r0, [sp, #8]
	ldr	r1, [sp, #8]
	ldr	r2, =0x77777777
	mov	lr, r2
	push	{r0-r2, lr}
	movs	r0, #0
	movs	r1, #0
	movs	r2, #0
	pop	{r0-r3}
	mov	r6, sp
	mov	sp, r7
	str	r0, [r5, #60]
	str	r1, [r5, #64]
	str	r2, [r5, #68]
	str	r3, [r5, #72]
	subs	r6, r6, r4
	str	r6, [r5, #76]
	pop	{r4-r7, pc}
	.ltorg
	.size thumb_forms, . - thumb_forms
