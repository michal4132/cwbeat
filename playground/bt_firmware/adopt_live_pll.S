.syntax unified
.thumb
.cpu cortex-m3

.section .hook,"ax",%progbits
.global patch_hook
.type patch_hook,%function
patch_hook:
    bl adopt_live_pll
.size patch_hook,.-patch_hook

.section .stub,"ax",%progbits
.balign 4
.global adopt_live_pll
.type adopt_live_pll,%function
adopt_live_pll:
    /* Called under original PRIMASK guard AFTER conservative flash timing.
     * r4=old PRIMASK, r5=record, r6=requested Hz, r7=divider; preserve r4-r8/SP.
     * LR=0x7c4a9. No nested calls; mismatch reproduces displaced r0/r1 prep.
     */
    ldr r0, =192000000
    cmp r6, r0
    bne fallback
    cmp r7, #4
    bne fallback
    ldr r0, =0x20029614
    ldrb r1, [r0, #0x14]
    cmp r1, #0
    bne fallback
    /* Require the complete verified 36-byte profile, including source bits. */
    adr r1, expected_record
    movs r0, #0
record_loop:
    ldr r2, [r5, r0]
    ldr r3, [r1, r0]
    cmp r2, r3
    bne fallback
    adds r0, #4
    cmp r0, #36
    bne record_loop

    ldr r0, =0x40000040
    ldr r0, [r0]
    ldr r1, =0x8000001c
    ands r0, r1
    ldr r1, =0x80000010
    cmp r0, r1
    bne fallback
    ldr r0, =0x40000050
    ldr r0, [r0]
    cmp r0, #5
    bne fallback
    ldr r0, =0x40000010
    ldr r0, [r0]
    movw r1, #0x101
    cmp r0, r1
    bne fallback
    ldr r0, =0x400000a4
    ldr r0, [r0]
    tst r0, #3
    bne fallback
    ldr r0, =0x40028000
    ldr r0, [r0]
    and r0, r0, #0x600
    cmp r0, #0x600
    bne fallback
    ldr r0, =0x4002802c
    ldr r0, [r0]
    ldr r1, =0x00800733
    cmp r0, r1
    bne fallback
    ldr r0, =0x4002803c
    ldr r0, [r0]
    ldr r1, =0x40415000
    cmp r0, r1
    bne fallback
    ldr r0, =0x40028040
    ldr r0, [r0]
    ldr r1, =0x12100f00
    cmp r0, r1
    bne fallback
    ldr r0, =0x40028048
    ldr r0, [r0]
    and r0, r0, #0x3c00
    cmp r0, #0x800
    bne fallback
    /* Reject a source/select change across the bounded compatibility reads. */
    ldr r0, =0x40000040
    ldr r0, [r0]
    ldr r1, =0x8000001c
    ands r0, r1
    ldr r1, =0x80000010
    cmp r0, r1
    bne fallback
    ldr r0, =0x40000050
    ldr r0, [r0]
    cmp r0, #5
    bne fallback

    /* Adopt compatible inherited PLL. Keep all original final clock/rate work. */
    ldr r0, =0x20029614
    movs r1, #1
    strb r1, [r0, #0x14]
    b.w native_adopt_resume
fallback:
    movs r1, #0
    mov r0, r1
    bx lr
.balign 4
expected_record:
    .byte 0x00,0xb0,0x71,0x0b,0x04,0x04,0x04,0x00
    .byte 0x00,0xb0,0x71,0x0b,0x80,0xba,0x8c,0x01
    .byte 0x33,0x07,0x80,0x00,0x00,0x50,0x41,0x40
    .byte 0x00,0x0f,0x10,0x12,0x02,0x00,0x00,0x00
    .byte 0x00,0x00,0x04,0x00
.ltorg
.size adopt_live_pll,.-adopt_live_pll
