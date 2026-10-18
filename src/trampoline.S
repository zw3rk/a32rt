/*
 * src/trampoline.S — Host <-> guest context switch for a32rt
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * uint32_t a32_jit_run_block(a32_ctx_t *ctx, const void *code_ptr,
 *                            void *cur_entry, const void *epoch_ptr);
 */

    .text
    .align 4
    .globl a32_jit_run_block
    .type a32_jit_run_block, %function
a32_jit_run_block:
    stp     x29, x30, [sp, #-160]!
    mov     x29, sp
    stp     x19, x20, [sp, #16]
    stp     x21, x22, [sp, #32]
    stp     x23, x24, [sp, #48]
    stp     x25, x26, [sp, #64]
    stp     x27, x28, [sp, #80]
    stp     d8,  d9,  [sp, #96]
    stp     d10, d11, [sp, #112]
    stp     d12, d13, [sp, #128]
    stp     d14, d15, [sp, #144]
    mov     x24, #0                 /* x24 = accumulated in-trampoline chain_hits delta */

    mov     x19, x0                 /* x19 = xCtx (a32_ctx_t *) */
    ldr     x20, [x19, #72]         /* x20 = xG (ctx->guest_base) */
    mov     x27, x2                 /* x27 = cur_entry (a32_cache_entry_t *) */
    mov     x28, x3                 /* x28 = &g_cache_epoch */

    /* Extract is_thumb (w26) and align_mask (w25), and restore hardware NZCV */
    ldr     w16, [x19, #64]
    ubfx    w26, w16, #5, #1        /* w26 = is_thumb (0 or 1) */
    sub     w25, wzr, w26           /* w25 = is_thumb ? 0xffffffff : 0 */
    mov     w15, #2
    bic     w15, w15, w25           /* w15 = is_thumb ? 0 : 2 */
    orr     w15, w15, #1            /* w15 = is_thumb ? 1 : 3 */
    mvn     w25, w15                /* w25 = is_thumb ? ~1u : ~3u */

    and     w16, w16, #0xf0000000
    msr     nzcv, x16

    /* Restore hardware FPCR / FPSR from ctx->fpscr (offset 352) when needed */
    ldr     w16, [x19, #352]
    and     w17, w16, #0x07c00000
    cbz     w17, 1f
    msr     fpcr, x17
1:
    movz    w15, #0x009f
    movk    w15, #0x0800, lsl #16
    and     w17, w16, w15
    mrs     x23, fpsr
    and     w23, w23, w15
    eor     w23, w23, w17
    cbz     w23, 2f
    msr     fpsr, x17
2:
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

.Lrun_loop:
    /* Execute translated block; block returns via `ret` (to x30) */
    blr     x17

    /* Fast in-trampoline direct chain dispatch (preserves w0..w14, NZCV, FPCR/FPSR) */
    cbz     x27, .Lslow_exit
    eor     w17, w16, #1            /* A32_EXIT_BRANCH == 1 */
    cbz     w17, .Lmode_ok
    eor     w17, w16, #2            /* A32_EXIT_INTERWORK == 2 */
    cbnz    w17, .Lslow_exit
    and     w17, w15, #1
    eor     w17, w17, w26           /* bit 0 must match current is_thumb */
    cbnz    w17, .Lslow_exit
.Lmode_ok:
    ldp     x17, x23, [x19, #368]   /* x17 = pending_signals (64b), x23 = flags (lo32) | itstate (hi32) */
    orr     x22, x17, x23
    cbnz    x22, .Lcheck_signals_slow
.Lsignals_ok:
    ldar    w21, [x28]              /* cur_epoch = g_cache_epoch (acquire load) */
    and     w30, w15, w25           /* aligned_pc = w15 & align_mask */
    cbz     w21, .Lslow_exit
    orr     w30, w30, w26           /* next_key = aligned_pc | is_thumb */
    orr     x30, x30, x21, lsl #32  /* x30 = next_key (lo32) | cur_epoch (hi32) */

.Ltry_chain0:
    ldr     x23, [x27, #48]         /* x23 = cur_entry->chain_entry[0] */
    cbz     x23, .Lslow_exit        /* slot 0 empty -> let C chain it */
    ldp     x21, x17, [x23]         /* next_entry->key|epoch, next_entry->host_code */
    eor     x21, x21, x30
    cbnz    x21, .Ltry_chain1
    cbz     x17, .Lslow_exit
    mov     x27, x23                /* cur_entry = next_entry */
    add     x24, x24, #1            /* chain_hits_delta++ */
    b       .Lrun_loop

.Ltry_chain1:
    lsr     x21, x21, #32           /* if slot 0 missed due to stale epoch, re-chain in C */
    cbnz    x21, .Lslow_exit
    ldr     x23, [x27, #56]         /* x23 = cur_entry->chain_entry[1] */
    cbz     x23, .Lslow_exit        /* slot 1 empty -> let C chain it */
    ldp     x21, x17, [x23]         /* next_entry->key|epoch, next_entry->host_code */
    eor     x21, x21, x30
    cbnz    x21, .Ltry_ibtc
    cbz     x17, .Lslow_exit
    mov     x27, x23                /* cur_entry = next_entry */
    add     x24, x24, #1            /* chain_hits_delta++ */
    b       .Lrun_loop

.Ltry_ibtc:
    lsr     x21, x21, #32           /* if slot 1 missed due to stale epoch, re-chain in C */
    cbnz    x21, .Lslow_exit
    eor     w17, w26, w30, lsr #1   /* (next_key >> 1) ^ is_thumb */
    and     w17, w17, #0x3ff        /* ibtc_idx = ... & 1023 */
    add     x22, x19, x17, lsl #5   /* x22 = (uint8_t *)ctx + ibtc_idx * 32 */
    ldr     x23, [x22, #2496]       /* x23 = ibtc.entry */
    cbz     x23, .Lcheck_ras_pop
    ldp     x21, x17, [x23]         /* next_entry->key|epoch, next_entry->host_code */
    eor     x21, x21, x30
    cbnz    x21, .Lcheck_ras_pop
    cbz     x17, .Lslow_exit
    mov     x27, x23                /* cur_entry = next_entry */
    ldr     x21, [x19, #384]        /* blocks_executed */
    ldr     x22, [x19, #408]        /* ibtc_hits */
    add     x21, x21, #1
    add     x22, x22, #1
    str     x21, [x19, #384]
    str     x22, [x19, #408]
    b       .Lrun_loop

.Lcheck_ras_pop:
    ldr     w17, [x19, #424]        /* w17 = ctx->ras_top */
    sub     w17, w17, #1
    and     w17, w17, #0x3f         /* r_idx = (ras_top - 1) & 63 */
    add     x22, x19, x17, lsl #5   /* x22 = ctx + r_idx * 32 */
    ldr     x23, [x22, #432]        /* ras[r_idx].key | (ras[r_idx].epoch << 32) */
    eor     x23, x23, x30
    cbnz    x23, .Lslow_exit
    str     w17, [x19, #424]        /* ctx->ras_top = r_idx */
    str     xzr, [x22, #432]        /* clear popped RAS slot */
    ldp     x17, x23, [x22, #440]   /* x17 = host_code, x23 = entry */
    cbz     x17, .Lslow_exit
    cbz     x23, .Lslow_exit
    ldp     x21, x22, [x23]         /* next_entry->key|epoch, next_entry->host_code */
    eor     x21, x21, x30
    eor     x22, x22, x17
    orr     x21, x21, x22
    cbnz    x21, .Lslow_exit
    mov     x27, x23                /* cur_entry = next_entry */
    ldr     x21, [x19, #384]        /* blocks_executed */
    ldr     x23, [x19, #416]        /* ras_hits */
    add     x21, x21, #1
    add     x23, x23, #1
    str     x21, [x19, #384]
    str     x23, [x19, #416]
    b       .Lrun_loop

.Lcheck_signals_slow:
    cbnz    x23, .Lslow_exit        /* flags or itstate non-zero -> slow exit */
    ldr     x23, [x19, #360]        /* ctx->sig_mask */
    bic     x17, x17, x23           /* unmasked pending_signals */
    cbnz    x17, .Lslow_exit
    b       .Lsignals_ok

.Lslow_exit:
    cbz     x24, 4f
    ldp     x21, x22, [x19, #384]   /* blocks_executed, chain_hits */
    add     x21, x21, x24
    add     x22, x22, x24
    stp     x21, x22, [x19, #384]
4:
    str     x27, [x19, #400]        /* ctx->last_exec_entry = cur_entry */

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

    /* Merge cumulative FPSR bits into ctx->fpscr and reset host FPCR if needed */
    mrs     x17, fpsr
    movz    w15, #0x009f
    movk    w15, #0x0800, lsl #16
    and     w17, w17, w15
    ldr     w14, [x19, #352]
    orr     w14, w14, w17
    str     w14, [x19, #352]
    and     w17, w14, #0x07c00000
    cbz     w17, 3f
    msr     fpcr, xzr
3:
    /* Return exit_reason in w0 */
    mov     w0, w16

    ldp     d8,  d9,  [sp, #96]
    ldp     d10, d11, [sp, #112]
    ldp     d12, d13, [sp, #128]
    ldp     d14, d15, [sp, #144]
    ldp     x19, x20, [sp, #16]
    ldp     x21, x22, [sp, #32]
    ldp     x23, x24, [sp, #48]
    ldp     x25, x26, [sp, #64]
    ldp     x27, x28, [sp, #80]
    ldp     x29, x30, [sp], #160
    ret
    .size a32_jit_run_block, .-a32_jit_run_block
