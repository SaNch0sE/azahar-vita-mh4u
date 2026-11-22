// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.
//
// The PICA command-list fast path, hand-written for Thumb-2.
//
// The C++ version batches four commands through stack arrays so the compiler can try to vectorize;
// on ARM32 it never does, and the staging becomes pure stack traffic around every LUT access. This
// loop applies each plain register write immediately instead: one LDRD for the {value, header}
// pair walked by pointer (no PLD: the list's lines are mostly in the cache by now), UBFX/TST field
// checks, one flags-LUT halfword, the mask-expansion word, an EOR-blended read-modify-write of the
// register file, and a byte-wise dirty-bitmap update, both only when the write changes a bit
// (games rewrite most registers before each draw with the values they hold), so the dirty marks
// name the registers that changed.
//
// A group write (extra data with group_commands set) whose registers are all plain is applied here
// too, register by register with the same mask. It exits at the first command needing special
// handling (a flags-LUT hit, an out-of-range id, a same-register batch, a group that reaches a
// special register or past the register file or the list's end) and the C++ caller runs that one
// through the slow path.
//
//   extern "C" u32 PicaCmdListFastLoop(const u32* head, u32 index, u32 length,
//                                      const u16* flags_lut, const u32* expand_lut,
//                                      u32* reg_array, u8* dirty_bytes, u32* out_count);
//
// Returns the new command index (past the list's last word when a group ended it unpadded).
// *out_count is the number of register writes applied. The caller re-checks index+1 < length
// to distinguish list-end from a special command.

#if defined(__arm__)

.syntax unified
.thumb
.text
// Unwind info (EHABI and DWARF in .debug_frame) so that a profiler's samples here unwind to
// ProcessCmdList.
.cfi_sections .debug_frame

// The write of \val to register \id under the mask in r12: register file and dirty bit, only
// when a bit changes. Clobbers \val, r10 and \tmp.
.macro APPLY id, val, tmp, done
    ldr     r10, [r5, \id, lsl #2]  // old register value
    eor     \val, \val, r10         // EOR-blend: old ^ ((old ^ new) & mask)
    ands    \val, \val, r12         // the bits the write changes
    beq     \done                   // none: the register and its dirty bit stay as they are
    eor     r10, r10, \val
    str     r10, [r5, \id, lsl #2]
    lsr     \tmp, \id, #3           // dirty byte index
    and     \val, \id, #7
    lsl     \val, lr, \val
    ldrb    r10, [r6, \tmp]
    orr     r10, r10, \val
    strb    r10, [r6, \tmp]
.endm

.global PicaCmdListFastLoop
.hidden PicaCmdListFastLoop
.type PicaCmdListFastLoop, %function
.thumb_func
.p2align 2
PicaCmdListFastLoop:
    .fnstart
    .cfi_startproc
    // r0 = head, r1 = index, r2 = length, r3 = flags_lut
    .save   {r4-r11, lr}
    push    {r4-r11, lr}
    .cfi_def_cfa_offset 36
    .cfi_offset r4, -36
    .cfi_offset r5, -32
    .cfi_offset r6, -28
    .cfi_offset r7, -24
    .cfi_offset r8, -20
    .cfi_offset r9, -16
    .cfi_offset r10, -12
    .cfi_offset r11, -8
    .cfi_offset lr, -4
    ldr     r4, [sp, #36]           // expand_lut
    ldr     r5, [sp, #40]           // reg_array
    ldr     r6, [sp, #44]           // dirty_bytes
    movs    r7, #0                  // applied-write count
    mov     lr, #1                  // dirty-bit seed, hoisted out of the loop
    // Walk by pointer: r1 = &head[index], r2 = &head[length - 1]. A pair read needs
    // index + 1 < length, i.e. r1 < r2.
    add     r2, r0, r2, lsl #2
    add     r1, r0, r1, lsl #2
    subs    r2, r2, #4

.p2align 2
1:  // Per-command loop. Advance only after all checks pass, so on a special-command exit r1
    // still points at that pair for the slow path.
    cmp     r1, r2
    bhs     3f

    ldrd    r9, r10, [r1]           // value, header

    ubfx    r11, r10, #0, #16       // cmd_id
    cmp     r11, #0x300             // RegsInternal::NUM_REGS
    bhs     3f
    ldrh    r12, [r3, r11, lsl #1]  // RegImplInfo flags
    tst     r10, #0x0ff00000        // extra_data_length (header bits 20-27)
    bne     5f
    tst     r12, #1                 // NeedsSpecialHandling
    bne     3f

    ubfx    r12, r10, #16, #4       // parameter mask nibble
    ldr     r12, [r4, r12, lsl #2]  // ExpandBitsToBytes[mask]
    APPLY   r11, r9, r8, 4f
4:  adds    r1, #8
    adds    r7, #1
    b       1b

5:  // Extra data: only a group write (header bit 31) of plain registers stays here.
    tst     r10, #0x80000000
    beq     3f
    ubfx    r8, r10, #20, #8        // n, the extra words: registers id..id+n
    lsrs    r12, r12, #3            // RegsUntilSpecial: id..id+until-1 are plain
    cmp     r12, r8
    bls     3f                      // until <= n: the group reaches a special register
    add     r12, r11, r8
    cmp     r12, #0x300
    bhs     3f                      // past the register file
    add     r12, r1, r8, lsl #2     // its last word, &head[i + 1 + n], within the list:
    adds    r12, #4                 // r1 + 4 + 4n <= &head[length - 1]
    cmp     r12, r2
    bhi     3f

    ubfx    r12, r10, #16, #4
    ldr     r12, [r4, r12, lsl #2]  // the mask, the same for every register of the group
    add     r7, r7, r8
    adds    r7, #1
    adds    r1, #8                  // the first extra word
    APPLY   r11, r9, r4, 6f
6:  ldr     r9, [r1], #4
    adds    r11, #1
    APPLY   r11, r9, r4, 7f
7:  subs    r8, #1
    bne     6b
    // The next command on an even index (the C++ loop's alignment), relative to head.
    sub     r10, r1, r0
    tst     r10, #4
    it      ne
    addne   r1, #4
    ldr     r4, [sp, #36]           // expand_lut back (APPLY used r4)
    b       1b

3:  // Done: list end or a command for the slow path.
    ldr     r12, [sp, #48]          // out_count
    str     r7, [r12]
    sub     r0, r1, r0              // back to a command index
    lsrs    r0, r0, #2
    pop     {r4-r11, pc}
    .cfi_endproc
    .fnend
.size PicaCmdListFastLoop, . - PicaCmdListFastLoop

// The scan's walk (CmdListMode::Scan, the emulation thread): the same commands, for a register
// file nothing reads the dirty marks of and only some registers of. A plain write is stored
// as it lands (a full mask, nearly every write, needs neither the old value nor the mask word),
// with no dirty mark. A group or a same-register batch of the data ports (flags bit 2: the
// uniform, program, swizzle and LUT uploads, which the render side takes; their register
// values are never read) is stepped over whole, its words counted. It exits as
// PicaCmdListFastLoop does, and also at a batch of any other register.
//
//   extern "C" u32 PicaCmdListScanLoop(const u32* head, u32 index, u32 length,
//                                      const u16* flags_lut, const u32* expand_lut,
//                                      u32* reg_array, u32* out_count);
.global PicaCmdListScanLoop
.hidden PicaCmdListScanLoop
.type PicaCmdListScanLoop, %function
.thumb_func
.p2align 2
PicaCmdListScanLoop:
    .fnstart
    .cfi_startproc
    // r0 = head, r1 = index, r2 = length, r3 = flags_lut
    .save   {r4-r11, lr}
    push    {r4-r11, lr}
    .cfi_def_cfa_offset 36
    .cfi_offset r4, -36
    .cfi_offset r5, -32
    .cfi_offset r6, -28
    .cfi_offset r7, -24
    .cfi_offset r8, -20
    .cfi_offset r9, -16
    .cfi_offset r10, -12
    .cfi_offset r11, -8
    .cfi_offset lr, -4
    ldr     r4, [sp, #36]           // expand_lut
    ldr     r5, [sp, #40]           // reg_array
    movs    r7, #0                  // applied-write count
    add     r2, r0, r2, lsl #2
    add     r1, r0, r1, lsl #2
    subs    r2, r2, #4              // r1 < r2: a pair to read

.p2align 2
1:  cmp     r1, r2
    bhs     3f
    pld     [r1, #256]              // the scan is the first to read the list's lines
    ldrd    r9, r10, [r1]           // value, header
    ubfx    r11, r10, #0, #16       // cmd_id
    cmp     r11, #0x300
    bhs     3f
    ldrh    r12, [r3, r11, lsl #1]  // RegImplInfo flags
    ubfx    lr, r10, #16, #4        // parameter mask nibble
    tst     r10, #0x0ff00000        // extra data
    bne     5f
    tst     r12, #1                 // NeedsSpecialHandling
    bne     3f
    cmp     lr, #15
    bne     4f
    str     r9, [r5, r11, lsl #2]   // a full mask: the value
    adds    r1, #8
    adds    r7, #1
    b       1b

4:  // A partial mask: old ^ ((old ^ new) & mask).
    ldr     lr, [r4, lr, lsl #2]
    ldr     r8, [r5, r11, lsl #2]
    eor     r9, r9, r8
    and     r9, r9, lr
    eor     r8, r8, r9
    str     r8, [r5, r11, lsl #2]
    adds    r1, #8
    adds    r7, #1
    b       1b

5:  // Extra data: n words after the pair.
    ubfx    r8, r10, #20, #8        // n
    add     r6, r1, r8, lsl #2      // its last word, &head[i + 1 + n], within the list:
    adds    r6, #4                  // r1 + 4 + 4n <= &head[length - 1]
    cmp     r6, r2
    bhi     3f
    tst     r12, #4                 // a data port
    bne     8f
    tst     r10, #0x80000000        // a group (else a batch of the register: the slow path)
    beq     3f
    tst     r12, #1
    bne     3f
    lsrs    r12, r12, #3            // RegsUntilSpecial: id..id+until-1 are plain
    cmp     r12, r8
    bls     3f                      // the group reaches a special register
    add     r12, r11, r8
    cmp     r12, #0x300
    bhs     3f                      // past the register file
    ldr     r12, [r4, lr, lsl #2]   // the mask, the same for every register of the group
    add     r7, r7, r8
    adds    r7, #1
    adds    r1, #8                  // the first extra word
6:  ldr     r6, [r5, r11, lsl #2]
    eor     r9, r9, r6
    and     r9, r9, r12
    eor     r6, r6, r9
    str     r6, [r5, r11, lsl #2]
    subs    r8, #1
    bmi     7f
    ldr     r9, [r1], #4
    adds    r11, #1
    b       6b

8:  // A data port: a batch of it, or a group within its run of ports (for a port the flags'
    // RegsUntilSpecial holds that run), is stepped over.
    tst     r10, #0x80000000
    beq     9f
    lsrs    r12, r12, #3
    cmp     r12, r8
    bls     3f                      // the group leaves the ports
9:  add     r7, r7, r8
    adds    r7, #1
    add     r1, r1, r8, lsl #2
    adds    r1, #8
7:  // The next command on an even index (the C++ loop's alignment), relative to head.
    sub     r10, r1, r0
    tst     r10, #4
    it      ne
    addne   r1, #4
    b       1b

3:  // Done: list end or a command for the slow path.
    ldr     r12, [sp, #44]          // out_count
    str     r7, [r12]
    sub     r0, r1, r0              // back to a command index
    lsrs    r0, r0, #2
    pop     {r4-r11, pc}
    .cfi_endproc
    .fnend
.size PicaCmdListScanLoop, . - PicaCmdListScanLoop

.section .note.GNU-stack, "", %progbits

#endif
