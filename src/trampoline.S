/*
 * src/trampoline.S — Host <-> guest context switch for a32rt
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * uint32_t a32_jit_run_block(a32_ctx_t *ctx, const void *code_ptr);
 */

    .text
    .align 4
    .globl a32_jit_run_block
    .type a32_jit_run_block, %function
a32_jit_run_block:
    stp     x29, x30, [sp, #-96]!
    mov     x29, sp
    stp     x19, x20, [sp, #16]
    stp     x21, x22, [sp, #32]
    stp     x23, x24, [sp, #48]
    stp     x25, x26, [sp, #64]
    stp     x27, x28, [sp, #80]

    mov     x19, x0                 /* x19 = xCtx (a32_ctx_t *) */
    ldr     x20, [x19, #72]         /* x20 = xG (ctx->guest_base) */

    /* Restore hardware NZCV from ctx->cpsr (offset 64) */
    ldr     w16, [x19, #64]
    and     w16, w16, #0xf0000000
    msr     nzcv, x16

    /* Restore hardware FPCR / FPSR from ctx->fpscr (offset 352) */
    ldr     w16, [x19, #352]
    and     w17, w16, #0x07c00000
    msr     fpcr, x17
    movz    w15, #0x009f
    movk    w15, #0x0800, lsl #16
    and     w17, w16, w15
    msr     fpsr, x17

    mov     x17, x1                 /* x17 = translated block entry */

    /* Load guest r0..r14 into host w0..w14 */
    ldp     w0,  w1,  [x19, #0]
    ldp     w2,  w3,  [x19, #8]
    ldp     w4,  w5,  [x19, #16]
    ldp     w6,  w7,  [x19, #24]
    ldp     w8,  w9,  [x19, #32]
    ldp     w10, w11, [x19, #40]
    ldp     w12, w13, [x19, #48]
    ldr     w14,      [x19, #56]

    /* Execute translated block; block returns via `ret` (to x30) */
    blr     x17

    /* Capture hardware NZCV immediately */
    mrs     x17, nzcv

    /* Save guest w0..w14 back to ctx->regs[0..14] */
    stp     w0,  w1,  [x19, #0]
    stp     w2,  w3,  [x19, #8]
    stp     w4,  w5,  [x19, #16]
    stp     w6,  w7,  [x19, #24]
    stp     w8,  w9,  [x19, #32]
    stp     w10, w11, [x19, #40]
    stp     w12, w13, [x19, #48]
    str     w14,      [x19, #56]

    /* Save next_pc (w15) and exit_reason (w16) */
    str     w15, [x19, #60]
    str     w16, [x19, #80]

    /* Merge NZCV (w17[31:28]) into ctx->cpsr */
    ldr     w0,  [x19, #64]
    bic     w0,  w0,  #0xf0000000
    and     w17, w17, #0xf0000000
    orr     w0,  w0,  w17
    str     w0,  [x19, #64]

    /* Merge cumulative FPSR bits into ctx->fpscr and reset host FPCR */
    mrs     x17, fpsr
    movz    w15, #0x009f
    movk    w15, #0x0800, lsl #16
    and     w17, w17, w15
    ldr     w14, [x19, #352]
    orr     w14, w14, w17
    str     w14, [x19, #352]
    msr     fpcr, xzr

    /* Return exit_reason in w0 */
    mov     w0, w16

    ldp     x19, x20, [sp, #16]
    ldp     x21, x22, [sp, #32]
    ldp     x23, x24, [sp, #48]
    ldp     x25, x26, [sp, #64]
    ldp     x27, x28, [sp, #80]
    ldp     x29, x30, [sp], #96
    ret
    .size a32_jit_run_block, .-a32_jit_run_block
