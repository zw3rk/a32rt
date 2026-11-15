/*
 * src/core/core_emit_exit.c — Direct 32-bit AArch64 exit, RAS & chain emitters
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "a64_emit.h"
#include "dbt/jit_priv.h"

/* Hardcoded immediates used below must match a32rt.h / jit_priv.h. */
_Static_assert(offsetof(a32_ctx_t, pending_signals) == 368, "sig_pending==368");
_Static_assert(offsetof(a32_ctx_t, cpsr) == 64, "cpsr==64");
_Static_assert(offsetof(a32_ctx_t, blocks_executed) == 384, "blocks==384");
_Static_assert(offsetof(a32_ctx_t, chain_hits) == 392, "chain_hits==392");
_Static_assert(offsetof(a32_ctx_t, ibtc_hits) == 408, "ibtc_hits==408");
_Static_assert(offsetof(a32_ctx_t, ras_hits) == 416, "ras_hits==416");
_Static_assert(offsetof(a32_ctx_t, ras_top) == 424, "ras_top==424");
_Static_assert(offsetof(a32_ctx_t, ras) + offsetof(a32_ibtc_entry_t, entry) == 448, "ras+entry==448");
_Static_assert(sizeof(a32_ibtc_entry_t) == 32, "ibtc entry stride==32");
_Static_assert(offsetof(a32_ctx_t, ibtc) + offsetof(a32_ibtc_entry_t, entry) == 2496, "ibtc+entry==2496");
_Static_assert(offsetof(a32_cache_entry_t, ret_entry) == 96, "ret_entry==96");
_Static_assert(offsetof(a32_cache_entry_t, slot_entry) == 152, "slot_entry==152");
_Static_assert(offsetof(a32_ctx_t, trace_enabled) == 376, "trace==376");

void a64_emit_ret(a64_buf_t *b)
{
    a64_vfp_emit_exit_flush(b);
    a64_emit(b, 0xD65F03C0U);
}

static size_t emit_exit_sig_check(a64_buf_t *b, uint32_t bail_disp)
{
    /* [0] ldp x17, x23, [x19, #368] (pending_signals, flags|itstate) */
    a64_emit(b, 0xA9400000U | (1U << 31) | ((368U / 8U) << 15) | (23U << 10) | (A64_XCTX << 5) | 17U);
    /* [1] orr x17, x17, x23 */
    a64_emit(b, 0xAA170231U);
    /* [2] cbnz x17, .Lbail */
    size_t patch_idx = b->count;
    a64_emit(b, 0xB5000011U | ((bail_disp & 0x7FFFFU) << 5));
    return patch_idx;
}

static void emit_ras_push_inline(a64_buf_t *b)
{
    /* [0] ldr x22, [x27, #96] (cur_entry->ret_entry, uimm = 96/8 = 12) */
    a64_emit(b, 0xF9403376U);
    /* [1] cbz x22, .Lskip_push (+7 words) */
    a64_emit(b, 0xB4000016U | (7U << 5));
    /* [2] ldr w21, [x19, #424] (w21 = ctx->ras_top, uimm = 424/4 = 106) */
    a64_emit(b, 0xB9400000U | (106U << 10) | (A64_XCTX << 5) | 21U);
    /* [3] add w16, w21, #1 */
    a64_emit(b, 0x110006B0U);
    /* [4] and w16, w16, #0x3f */
    a64_emit(b, 0x12001610U);
    /* [5] str w16, [x19, #424] (ctx->ras_top = (ras_top + 1) & 63) */
    a64_emit(b, 0xB9000000U | (106U << 10) | (A64_XCTX << 5) | 16U);
    /* [6] add x21, x19, x21, lsl #5 (x21 = ctx + ras_top * 32) */
    a64_emit(b, 0x8B151675U);
    /* [7] str x22, [x21, #448] (ras[top].entry = ret_entry, uimm = 448/8 = 56) */
    a64_emit(b, 0xF9000000U | (56U << 10) | (21U << 5) | 22U);
}

static void emit_chain_jump_tail(a64_buf_t *b, uint32_t stats_hit_off)
{
    /* mov x27, x23 (cur_entry = next_entry) */
    a64_emit(b, 0xAA1703FBU);
    if (!b->stats_enabled || stats_hit_off == 0U) {
        /* add x24, x24, #1 (chain_hits_delta++) */
        a64_emit(b, 0x91000718U);
    } else {
        uint32_t uimm_hit = stats_hit_off / 8U;
        /* ldr x21, [x19, #384] (blocks_executed) */
        a64_emit(b, 0xF9400000U | ((384U / 8U) << 10) | (A64_XCTX << 5) | 21U);
        /* add x21, x21, #1 */
        a64_emit(b, 0x910006B5U);
        /* str x21, [x19, #384] */
        a64_emit(b, 0xF9000000U | ((384U / 8U) << 10) | (A64_XCTX << 5) | 21U);
        /* ldr x21, [x19, #stats_hit_off] */
        a64_emit(b, 0xF9400000U | (uimm_hit << 10) | (A64_XCTX << 5) | 21U);
        /* add x21, x21, #1 */
        a64_emit(b, 0x910006B5U);
        /* str x21, [x19, #stats_hit_off] */
        a64_emit(b, 0xF9000000U | (uimm_hit << 10) | (A64_XCTX << 5) | 21U);
    }
    /* br x17 */
    a64_emit(b, 0xD61F0220U);
}

static void emit_ras_hit_tail(a64_buf_t *b)
{
    /* mov x27, x23 (cur_entry = next_entry) */
    a64_emit(b, 0xAA1703FBU);
    if (!b->stats_enabled) {
        /* add x24, x24, #1 (chain_hits_delta++) */
        a64_emit(b, 0x91000718U);
    } else {
        /* ldr x21, [x19, #384] (blocks_executed) */
        a64_emit(b, 0xF9400000U | ((384U / 8U) << 10) | (A64_XCTX << 5) | 21U);
        /* add x21, x21, #1 */
        a64_emit(b, 0x910006B5U);
        /* str x21, [x19, #384] */
        a64_emit(b, 0xF9000000U | ((384U / 8U) << 10) | (A64_XCTX << 5) | 21U);
        /* ldr x21, [x19, #416] (ras_hits) */
        a64_emit(b, 0xF9400000U | ((416U / 8U) << 10) | (A64_XCTX << 5) | 21U);
        /* add x21, x21, #1 */
        a64_emit(b, 0x910006B5U);
        /* str x21, [x19, #416] */
        a64_emit(b, 0xF9000000U | ((416U / 8U) << 10) | (A64_XCTX << 5) | 21U);
    }
    /* br x22 */
    a64_emit(b, 0xD61F02C0U);
}

void a64_emit_exit_branch(a64_buf_t *b, uint32_t target_pc)
{
    if (target_pc == b->guest_start_pc && b->guest_start_pc != 0U && !b->is_call) {
        b->has_self_loop = true;
        if (!b->self_loop_vfp_active) {
            a64_vfp_cache_flush(b, false);
        }
        /* A non-hoisted self-loop back-edge runs under whatever FPCR the
         * body left active; restore scalar controls before the exit poll
         * (pass-2 preheader hoists cover the preheader_neon_fpcr case, and
         * .Lbail restores via the exit flush below). */
        if (b->fpcr_neon_active && !b->preheader_neon_fpcr) {
            a64_emit_fpcr_restore_scalar(b);
            b->fpcr_neon_active = false;
        }
        emit_exit_sig_check(b, 7U);
        /* [3] ldr w21, [x28] (cur_epoch = g_cache_epoch) */
        a64_emit(b, 0xB9400395U);
        /* [4] ldr x22, [x27, #0] (cur_entry->key | (cur_entry->epoch << 32)) */
        a64_emit(b, 0xF9400376U);
        /* [5] eor x21, x21, x22, lsr #32 */
        a64_emit(b, 0xCA5682B5U);
        /* [6] cbnz x21, .Lbail (+3 words) */
        a64_emit(b, 0xB5000015U | (3U << 5));
        /* [7] add x24, x24, #1 (chain_hits_delta++) */
        a64_emit(b, 0x91000718U);
        /* [8] b loop_body_offset (direct PC-relative branch inside block!) */
        int32_t back_disp = (int32_t)b->loop_body_offset - (int32_t)b->count;
        a64_emit(b, 0x14000000U | ((uint32_t)back_disp & 0x03FFFFFFU));
        /* [9] .Lbail: flush VFP / restore hoisted FPCR, then return to trampoline */
        if (b->self_loop_vfp_active) {
            a64_vfp_emit_exit_flush(b);
        } else if (b->preheader_neon_fpcr && b->fpcr_neon_active) {
            a64_emit_fpcr_restore_scalar(b);
        }
        a64_emit_mov_pc32(b, A64_TMP0, target_pc);
        a64_emit_movz_w(b, A64_TMP1, A32_EXIT_BRANCH, 0U);
        a64_emit(b, 0xD65F03C0U); /* ret */
        return;
    }

    a64_vfp_emit_exit_flush(b);

    unsigned s = A64_MAX_EXIT_SLOTS;
    for (unsigned i = 0U; i < b->exit_slot_count; i++) {
        if (b->exit_slot_pcs[i] == target_pc) {
            s = i;
            break;
        }
    }
    if (s == A64_MAX_EXIT_SLOTS && b->exit_slot_count < A64_MAX_EXIT_SLOTS) {
        s = b->exit_slot_count++;
        b->exit_slot_pcs[s] = target_pc;
    }

    if (s < A64_MAX_EXIT_SLOTS) {
        uint32_t call_extra = b->is_call ? 8U : 0U;
        bool same_mode = true;
        bool forward = same_mode && (b->cur_insn_pc != 0U) && ((target_pc & ~1U) > b->cur_insn_pc);
        if (!forward) {
            emit_exit_sig_check(b, 10U + call_extra);
        }
        /* ldr x23, [x27, #(152 + s*8)] (cur_entry->slot_entry[s], uimm = 19 + s) */
        a64_emit(b, 0xF9400000U | (((19U + s) & 0xFFFU) << 10) | (27U << 5) | 23U);
        /* cbz x23, .Lbail */
        a64_emit(b, 0xB4000017U | ((8U + call_extra) << 5));
        /* ldr w21, [x28] (cur_epoch = g_cache_epoch) */
        a64_emit(b, 0xB9400395U);
        /* ldp x22, x17, [x23, #0] (x22 = key|epoch, x17 = host_code) */
        a64_emit(b, 0xA9400000U | (1U << 31) | (17U << 10) | (23U << 5) | 22U);
        /* eor x21, x21, x22, lsr #32 */
        a64_emit(b, 0xCA5682B5U);
        /* cbnz x21, .Lbail */
        a64_emit(b, 0xB5000015U | ((4U + call_extra) << 5));
        if (b->is_call) {
            emit_ras_push_inline(b);
        }
        emit_chain_jump_tail(b, 0U);
    }

    /* .Lbail */
    a64_emit_mov_pc32(b, A64_TMP0, target_pc);
    a64_emit_movz_w(b, A64_TMP1, A32_EXIT_BRANCH, 0U);
    a64_emit(b, 0xD65F03C0U); /* ret */
}

void a64_emit_exit_branch_imm(a64_buf_t *b, uint32_t target_pc, bool interwork)
{
    bool same_mode = !interwork || (((target_pc & 1U) != 0U) == b->is_thumb);
    bool forward = same_mode && (b->cur_insn_pc != 0U) && ((target_pc & ~1U) > b->cur_insn_pc);
    if (!interwork) {
        (void)forward;
        a64_emit_exit_branch(b, target_pc);
        return;
    }
    (void)forward;
    a64_emit_mov_pc32(b, A64_TMP0, target_pc);
    a64_emit_movz_w(b, A64_TMP1, A32_EXIT_INTERWORK, 0U);
    a64_emit_ret(b);
}

void a64_emit_exit_branch_reg(a64_buf_t *b, unsigned target_reg, bool interwork)
{
    if (b->lr_written_from_pc) {
        b->is_call = true;
    }
    /* Copy target_reg into TMP0 first: the exit flush below touches TMP2,
     * so a target_reg of TMP2 must already be safe in TMP0 (w15). */
    if (target_reg != A64_TMP0) {
        a64_emit_mov_w(b, A64_TMP0, target_reg);
    }
    a64_vfp_emit_exit_flush(b);

    size_t patch_sig = emit_exit_sig_check(b, 0U);

    size_t patch_iw = 0U;
    if (interwork) {
        /* and w17, w15, #1 */
        a64_emit(b, 0x120001F1U);
        /* eor w17, w17, w26 */
        a64_emit(b, 0x4A1A0231U);
        /* cbnz w17, .Lbail */
        patch_iw = b->count;
        a64_emit(b, 0x35000011U);
    }

    /* ldr w21, [x28] */
    a64_emit(b, 0xB9400395U);
    /* cbz w21, .Lbail */
    size_t patch_ep0 = b->count;
    a64_emit(b, 0x34000015U);

    /* Derive next_key from static block ISA mode (frees w25 for Soft-TLB) */
    if (b->is_thumb) {
        /* orr w16, w15, #1 ((w15 & ~1U) | 1U == w15 | 1U) */
        a64_emit(b, 0x320001F0U);
    } else {
        /* and w16, w15, #0xfffffffc ((w15 & ~3U) | 0U == w15 & ~3U) */
        a64_emit(b, 0x121E75F0U);
    }
    /* orr x16, x16, x21, lsl #32 (x16 = next_key | (cur_epoch << 32)) */
    a64_emit(b, 0xAA158210U);

    if (!b->is_call && (b->is_return || target_reg == 14U)) {
        /* Inline RAS Pop */
        /* ldr w17, [x19, #424] (w17 = ctx->ras_top) */
        a64_emit(b, 0xB9400000U | (106U << 10) | (A64_XCTX << 5) | 17U);
        /* sub w17, w17, #1 */
        a64_emit(b, 0x51000631U);
        /* and w17, w17, #0x3f */
        a64_emit(b, 0x12001631U);
        /* add x22, x19, x17, lsl #5 */
        a64_emit(b, 0x8B111676U);
        /* ldr x23, [x22, #448] (ras[r_idx].entry, uimm = 448/8 = 56) */
        a64_emit(b, 0xF9400000U | (56U << 10) | (22U << 5) | 23U);
        /* cbz x23, .Ltry_ibtc */
        size_t patch_ras_null = b->count;
        a64_emit(b, 0xB4000017U);
        /* ldp x21, x22, [x23, #0] (x21 = entry->key|epoch, x22 = entry->host_code) */
        a64_emit(b, 0xA9400000U | (1U << 31) | (22U << 10) | (23U << 5) | 21U);
        /* eor x21, x21, x16 */
        a64_emit(b, 0xCA1002B5U);
        /* cbnz x21, .Ltry_ibtc */
        size_t patch_ras_miss = b->count;
        a64_emit(b, 0xB5000015U);
        /* str w17, [x19, #424] (commit pop: ctx->ras_top = r_idx) */
        a64_emit(b, 0xB9000000U | (106U << 10) | (A64_XCTX << 5) | 17U);
        emit_ras_hit_tail(b);

        /* Patch patch_ras_null and patch_ras_miss to here (.Ltry_ibtc) */
        size_t try_ibtc_idx = b->count;
        b->words[patch_ras_null] |= (((uint32_t)(try_ibtc_idx - patch_ras_null) & 0x7FFFFU) << 5);
        b->words[patch_ras_miss] |= (((uint32_t)(try_ibtc_idx - patch_ras_miss) & 0x7FFFFU) << 5);
    }

    /* .Ltry_ibtc: */
    /* ubfx w17, w16, #1, #10 */
    a64_emit(b, 0x53012A11U);
    /* add x22, x19, x17, lsl #5 */
    a64_emit(b, 0x8B111676U);
    /* ldr x23, [x22, #2496] (offsetof(a32_ctx_t, ibtc) + offsetof(a32_ibtc_entry_t, entry), uimm = 312) */
    a64_emit(b, 0xF9400000U | (312U << 10) | (22U << 5) | 23U);
    /* cbz x23, .Lbail */
    size_t patch_ibtc_null = b->count;
    a64_emit(b, 0xB4000017U);
    /* ldp x21, x17, [x23, #0] */
    a64_emit(b, 0xA9400000U | (1U << 31) | (17U << 10) | (23U << 5) | 21U);
    /* eor x21, x21, x16 */
    a64_emit(b, 0xCA1002B5U);
    /* cbnz x21, .Lbail */
    size_t patch_ibtc_miss = b->count;
    a64_emit(b, 0xB5000015U);

    if (b->is_call) {
        emit_ras_push_inline(b);
    }
    emit_chain_jump_tail(b, 408U);

    /* .Lbail: */
    size_t bail_idx = b->count;
    b->words[patch_sig] |= (((uint32_t)(bail_idx - patch_sig) & 0x7FFFFU) << 5);
    if (interwork) {
        b->words[patch_iw] |= (((uint32_t)(bail_idx - patch_iw) & 0x7FFFFU) << 5);
    }
    b->words[patch_ep0] |= (((uint32_t)(bail_idx - patch_ep0) & 0x7FFFFU) << 5);
    b->words[patch_ibtc_null] |= (((uint32_t)(bail_idx - patch_ibtc_null) & 0x7FFFFU) << 5);
    b->words[patch_ibtc_miss] |= (((uint32_t)(bail_idx - patch_ibtc_miss) & 0x7FFFFU) << 5);

    a64_emit_movz_w(b, A64_TMP1, interwork ? A32_EXIT_INTERWORK : A32_EXIT_BRANCH, 0U);
    a64_emit(b, 0xD65F03C0U); /* ret */
}

void a64_emit_exit_svc(a64_buf_t *b, uint32_t next_pc, uint32_t svc_imm)
{
    a64_emit_mov_imm32(b, A64_TMP0, svc_imm);
    a64_emit_str_imm_w(b, A64_TMP0, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, svc_imm));
    a64_emit_mov_pc32(b, A64_TMP0, next_pc);
    a64_emit_movz_w(b, A64_TMP1, A32_EXIT_SVC, 0U);
    a64_emit_ret(b);
}

void a64_emit_exit_undef(a64_buf_t *b, uint32_t fault_pc)
{
    a64_emit_mov_pc32(b, A64_TMP0, fault_pc);
    a64_emit_movz_w(b, A64_TMP1, A32_EXIT_UNDEF, 0U);
    a64_emit_ret(b);
}

void a64_emit_exit_bkpt(a64_buf_t *b, uint32_t bkpt_pc)
{
    a64_emit_mov_pc32(b, A64_TMP0, bkpt_pc);
    a64_emit_str_imm_w(b, A64_TMP0, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, regs[15]));
    a64_emit_movz_w(b, A64_TMP1, A32_EXIT_BKPT, 0U);
    a64_emit_ret(b);
}
