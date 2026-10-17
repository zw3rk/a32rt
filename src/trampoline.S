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
    stp     x29, x30, [sp, #-96]!
    mov     x29, sp
    stp     x19, x20, [sp, #16]
    stp     x21, x22, [sp, #32]
    stp     x23, x24, [sp, #48]
    stp     x25, x26, [sp, #64]
    stp     x27, x28, [sp, #80]

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
    ldr     w17, [x19, #88]         /* ctx->itstate */
    ldr     w23, [x19, #376]        /* trace | gdb | single_step | sync_sig_delivered */
    orr     w17, w17, w23
    cbnz    w17, .Lslow_exit
    ldp     x17, x23, [x19, #360]   /* sig_mask, pending_signals */
    bic     x17, x23, x17
    cbnz    x17, .Lslow_exit

    and     w24, w15, w25           /* aligned_pc = w15 & align_mask */
    orr     w24, w24, w26           /* next_key = aligned_pc | is_thumb */

    ldar    w21, [x28]              /* cur_epoch = g_cache_epoch */
    cbz     w21, .Lslow_exit
    orr     x24, x24, x21, lsl #32  /* x24 = next_key (lo32) | cur_epoch (hi32) */

.Lcheck_ras_pop:
    add     x30, x19, #8, lsl #12   /* x30 = x19 + 0x8000 */
    ldr     w17, [x30, #0x9a8]      /* w17 = ctx->ras_top */
    sub     w17, w17, #1
    ubfx    x22, x17, #0, #6        /* x22 = r_idx = (ras_top - 1) & 63 */
    add     x22, x30, x22, lsl #5   /* x22 = x19 + 0x8000 + r_idx * 32 */
    ldr     x23, [x22, #0x1a0]      /* ras[r_idx].key | (ras[r_idx].epoch << 32) */
    eor     x23, x23, x24
    cbnz    x23, .Ltry_chain0
    /* RAS match: x23 == 0 */
    and     w17, w17, #0x3f
    str     w17, [x30, #0x9a8]      /* ctx->ras_top = r_idx */
    str     xzr, [x22, #0x1a0]      /* clear popped RAS slot */
    ldp     x17, x23, [x22, #0x1a8] /* x17 = host_code, x23 = entry */
    mov     w22, #2                 /* 2 = ras hit */
    b       .Lverify_entry

.Ltry_chain0:
    ldr     w23, [x27, #24]         /* chain_epoch[0] */
    eor     w23, w23, w21
    cbnz    w23, .Lslow_exit        /* slot 0 empty/stale -> let C chain it */
    ldr     w17, [x27, #16]         /* chain_key[0] */
    eor     w17, w17, w24
    cbnz    w17, .Ltry_chain1
    ldr     x17, [x27, #32]         /* chain_code[0] */
    ldr     x23, [x27, #48]         /* chain_entry[0] */
    mov     w22, #0                 /* 0 = chain hit */
    b       .Lverify_entry

.Ltry_chain1:
    ldr     w23, [x27, #28]         /* chain_epoch[1] */
    eor     w23, w23, w21
    cbnz    w23, .Lslow_exit        /* slot 1 empty/stale -> let C chain it */
    ldr     w17, [x27, #20]         /* chain_key[1] */
    eor     w17, w17, w24
    cbnz    w17, .Ltry_ibtc
    ldr     x17, [x27, #40]         /* chain_code[1] */
    ldr     x23, [x27, #56]         /* chain_entry[1] */
    mov     w22, #0                 /* 0 = chain hit */
    b       .Lverify_entry

.Ltry_ibtc:
    eor     w17, w26, w24, lsr #1   /* (next_key >> 1) ^ is_thumb */
    and     w17, w17, #0x3ff        /* ibtc_idx = ... & 1023 */
    add     x22, x19, x17, lsl #5   /* x22 = (uint8_t *)ctx + ibtc_idx * 32 */
    ldr     x17, [x22, #416]        /* ibtc.key (lo32) | ibtc.epoch (hi32) */
    eor     x17, x17, x24
    cbnz    x17, .Lslow_exit
    ldp     x17, x23, [x22, #424]   /* ibtc.host_code, ibtc.entry */
    mov     w22, #1                 /* 1 = ibtc hit */

.Lverify_entry:
    cbz     x17, .Lslow_exit
    cbz     x23, .Lslow_exit
    ldar    x21, [x23]              /* next_entry->key (lo32) | next_entry->epoch (hi32) */
    eor     x24, x24, x21
    cbnz    x24, .Lslow_exit
    ldr     x24, [x23, #8]          /* next_entry->host_code */
    eor     x24, x24, x17
    cbnz    x24, .Lslow_exit
    ldar    w24, [x28]              /* re-verify g_cache_epoch */
    eor     x24, x24, x21, lsr #32
    cbnz    w24, .Lslow_exit

    /* In-trampoline RAS push when cur_entry->is_call != 0 (only after fast transition verified) */
    ldrb    w16, [x27, #76]         /* cur_entry->is_call */
    cbz     w16, .Lfinish_step
    and     w16, w14, w25           /* lr_aligned = w14 & align_mask */
    ldr     w24, [x27, #68]         /* cur_entry->guest_end_pc */
    eor     w24, w24, w16           /* lr_aligned must equal guest_end_pc */
    cbnz    w24, .Lfinish_step
    orr     w16, w16, w26           /* ret_key = lr_aligned | is_thumb */
    eor     w24, w16, w21           /* w21 lo32 is next_key; if ret_key == next_key, not a call */
    cbz     w24, .Lfinish_step
    lsr     x21, x21, #32           /* w21 = cur_epoch */
    ldr     w24, [x27, #84]         /* cur_entry->ret_epoch */
    eor     w24, w24, w21           /* == cur_epoch? */
    cbnz    w24, .Lras_lookup_ibtc
    ldr     w24, [x27, #80]         /* cur_entry->ret_key */
    eor     w24, w24, w16
    cbnz    w24, .Lras_lookup_ibtc
    ldp     x15, x24, [x27, #88]    /* x15 = ret_code, x24 = ret_entry */
    b       .Lras_do_push

.Lras_lookup_ibtc:
    eor     w24, w26, w16, lsr #1   /* ibtc_idx = ((ret_key >> 1) ^ is_thumb) & 1023 */
    and     w24, w24, #0x3ff
    add     x24, x19, x24, lsl #5   /* x24 = ctx + ibtc_idx * 32 */
    orr     x15, x16, x21, lsl #32  /* x15 = ret_key | (cur_epoch << 32) */
    ldr     x30, [x24, #416]        /* ibtc.key | (ibtc.epoch << 32) */
    eor     x30, x30, x15
    cbnz    x30, .Lfinish_step      /* Not in IBTC yet -> skip fast push */
    ldp     x15, x24, [x24, #424]   /* x15 = ibtc.host_code, x24 = ibtc.entry */
    cbz     x15, .Lfinish_step
    cbz     x24, .Lfinish_step
    /* Cache in cur_entry->ret_* for next time */
    str     wzr, [x27, #84]
    str     w16, [x27, #80]
    stp     x15, x24, [x27, #88]
    add     x30, x27, #84
    stlr    w21, [x30]

.Lras_do_push:
    orr     x16, x16, x21, lsl #32  /* x16 = ret_key | (cur_epoch << 32) */
    add     x30, x19, #8, lsl #12   /* x30 = x19 + 0x8000 */
    ldr     w21, [x30, #0x9a8]      /* w21 = ras_top */
    and     w21, w21, #0x3f
    add     x30, x30, x21, lsl #5   /* x30 = x19 + 0x8000 + ras_top * 32 */
    str     x16, [x30, #0x1a0]      /* ras.key | (ras.epoch << 32) */
    stp     x15, x24, [x30, #0x1a8] /* ras.host_code, ras.entry */
    add     w21, w21, #1
    and     w21, w21, #0x3f
    add     x30, x19, #8, lsl #12
    str     w21, [x30, #0x9a8]      /* ctx->ras_top = (ras_top + 1) & 63 */

.Lfinish_step:
    mov     x27, x23                /* cur_entry = next_entry */
    ldr     x21, [x19, #384]        /* blocks_executed */
    add     x21, x21, #1
    str     x21, [x19, #384]
    cbz     w22, .Linc_chain
    tbz     w22, #0, .Linc_ras
    ldr     x21, [x19, #408]        /* ibtc_hits */
    add     x21, x21, #1
    str     x21, [x19, #408]
    b       .Lrun_loop
.Linc_chain:
    ldr     x21, [x19, #392]        /* chain_hits */
    add     x21, x21, #1
    str     x21, [x19, #392]
    b       .Lrun_loop
.Linc_ras:
    add     x30, x19, #8, lsl #12
    ldr     x21, [x30, #0x9a0]      /* ras_hits */
    add     x21, x21, #1
    str     x21, [x30, #0x9a0]
    b       .Lrun_loop

.Lslow_exit:
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

    ldp     x19, x20, [sp, #16]
    ldp     x21, x22, [sp, #32]
    ldp     x23, x24, [sp, #48]
    ldp     x25, x26, [sp, #64]
    ldp     x27, x28, [sp, #80]
    ldp     x29, x30, [sp], #96
    ret
    .size a32_jit_run_block, .-a32_jit_run_block
