/*
 * src/core/core_emit_alu.c - split-TU part 1/3 of src/a64_emit.c (lines 1-602).
 * Compiled only via the src/a64_emit.c wrapper; do not add to Makefile SRCS.
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * src/a64_emit.c — Direct 32-bit AArch64 instruction encoder implementation
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "a64_emit.h"
#include "dbt/aot/aot.h"

/* Last-PC stash maintained by a32_jit_record_pc_map (src/vm/vm_jit_state.c):
 * both translators record (count, cur_pc) immediately before emitting each
 * guest instruction, so this is the faulting instruction PC at emission. */
extern uint32_t a32_jit_last_record_pc(void);

void a64_emit_movz_w(a64_buf_t *b, unsigned rd, uint16_t imm16, unsigned hw)
{
    a64_emit(b, 0x52800000U | ((hw & 1U) << 21) | ((uint32_t)imm16 << 5) | (rd & 31U));
}

void a64_emit_movk_w(a64_buf_t *b, unsigned rd, uint16_t imm16, unsigned hw)
{
    a64_emit(b, 0x72800000U | ((hw & 1U) << 21) | ((uint32_t)imm16 << 5) | (rd & 31U));
}

void a64_emit_mov_imm32(a64_buf_t *b, unsigned rd, uint32_t val)
{
    uint16_t lo = (uint16_t)(val & 0xFFFFU);
    uint16_t hi = (uint16_t)(val >> 16);
    if (lo == 0U && hi != 0U) {
        a64_emit_movz_w(b, rd, hi, 1U);
        return;
    }
    a64_emit_movz_w(b, rd, lo, 0U);
    if (hi != 0U) {
        a64_emit_movk_w(b, rd, hi, 1U);
    }
}

void a64_emit_mov_pc32(a64_buf_t *b, unsigned rd, uint32_t pc_val)
{
    /* Emit high halfword first (MOVZ hw=1) followed by low halfword (MOVK hw=0).
     * Normal a64_emit_mov_imm32 never emits MOVK hw=0, making guest-PC immediates
     * 100% self-identifying in AOT code streams for base-address relocation. */
    a64_emit_movz_w(b, rd, (uint16_t)(pc_val >> 16U), 1U);
    a64_emit_movk_w(b, rd, (uint16_t)(pc_val & 0xFFFFU), 0U);
}

void a64_emit_mov_w(a64_buf_t *b, unsigned rd, unsigned rm)
{
    if (rd == rm) {
        return;
    }
    a64_emit_orr_reg_w(b, rd, A64_ZR, rm, A64_SHIFT_LSL, 0U);
}

void a64_emit_mvn_reg_w(a64_buf_t *b, unsigned rd, unsigned rm, unsigned shift, unsigned amount)
{
    a64_emit_orn_reg_w(b, rd, A64_ZR, rm, shift, amount);
}

void a64_emit_add_imm_w(a64_buf_t *b, bool set_flags, unsigned rd, unsigned rn,
                        uint16_t imm12, unsigned sh)
{
    uint32_t s = set_flags ? (1U << 29) : 0U;
    a64_emit(b, 0x11000000U | s | ((sh & 1U) << 22) | (((uint32_t)imm12 & 0xFFFU) << 10) |
                    ((rn & 31U) << 5) | (rd & 31U));
}

void a64_emit_sub_imm_w(a64_buf_t *b, bool set_flags, unsigned rd, unsigned rn,
                        uint16_t imm12, unsigned sh)
{
    uint32_t s = set_flags ? (1U << 29) : 0U;
    a64_emit(b, 0x51000000U | s | ((sh & 1U) << 22) | (((uint32_t)imm12 & 0xFFFU) << 10) |
                    ((rn & 31U) << 5) | (rd & 31U));
}

void a64_emit_add_reg_w(a64_buf_t *b, bool set_flags, unsigned rd, unsigned rn,
                        unsigned rm, unsigned shift, unsigned amount)
{
    uint32_t s = set_flags ? (1U << 29) : 0U;
    a64_emit(b, 0x0B000000U | s | ((shift & 3U) << 22) | ((rm & 31U) << 16) |
                    ((amount & 31U) << 10) | ((rn & 31U) << 5) | (rd & 31U));
}

void a64_emit_sub_reg_w(a64_buf_t *b, bool set_flags, unsigned rd, unsigned rn,
                        unsigned rm, unsigned shift, unsigned amount)
{
    uint32_t s = set_flags ? (1U << 29) : 0U;
    a64_emit(b, 0x4B000000U | s | ((shift & 3U) << 22) | ((rm & 31U) << 16) |
                    ((amount & 31U) << 10) | ((rn & 31U) << 5) | (rd & 31U));
}

void a64_emit_adc_w(a64_buf_t *b, bool set_flags, unsigned rd, unsigned rn, unsigned rm)
{
    uint32_t s = set_flags ? (1U << 29) : 0U;
    a64_emit(b, 0x1A000000U | s | ((rm & 31U) << 16) | ((rn & 31U) << 5) | (rd & 31U));
}

void a64_emit_sbc_w(a64_buf_t *b, bool set_flags, unsigned rd, unsigned rn, unsigned rm)
{
    uint32_t s = set_flags ? (1U << 29) : 0U;
    a64_emit(b, 0x5A000000U | s | ((rm & 31U) << 16) | ((rn & 31U) << 5) | (rd & 31U));
}

void a64_emit_add_uxtw_x(a64_buf_t *b, unsigned rd_x, unsigned rn_x, unsigned rm_w)
{
    /* ADD Xd, Xn, Wm, UXTW #0 */
    a64_emit(b, 0x8B204000U | ((rm_w & 31U) << 16) | ((rn_x & 31U) << 5) | (rd_x & 31U));
}

void a64_emit_and_reg_w(a64_buf_t *b, bool set_flags, unsigned rd, unsigned rn,
                        unsigned rm, unsigned shift, unsigned amount)
{
    uint32_t base = set_flags ? 0x6A000000U : 0x0A000000U;
    a64_emit(b, base | ((shift & 3U) << 22) | ((rm & 31U) << 16) |
                    ((amount & 31U) << 10) | ((rn & 31U) << 5) | (rd & 31U));
}

void a64_emit_bic_reg_w(a64_buf_t *b, bool set_flags, unsigned rd, unsigned rn,
                        unsigned rm, unsigned shift, unsigned amount)
{
    uint32_t base = set_flags ? 0x6A200000U : 0x0A200000U;
    a64_emit(b, base | ((shift & 3U) << 22) | ((rm & 31U) << 16) |
                    ((amount & 31U) << 10) | ((rn & 31U) << 5) | (rd & 31U));
}

void a64_emit_orr_reg_w(a64_buf_t *b, unsigned rd, unsigned rn,
                        unsigned rm, unsigned shift, unsigned amount)
{
    a64_emit(b, 0x2A000000U | ((shift & 3U) << 22) | ((rm & 31U) << 16) |
                    ((amount & 31U) << 10) | ((rn & 31U) << 5) | (rd & 31U));
}

void a64_emit_orn_reg_w(a64_buf_t *b, unsigned rd, unsigned rn,
                        unsigned rm, unsigned shift, unsigned amount)
{
    a64_emit(b, 0x2A200000U | ((shift & 3U) << 22) | ((rm & 31U) << 16) |
                    ((amount & 31U) << 10) | ((rn & 31U) << 5) | (rd & 31U));
}

void a64_emit_eor_reg_w(a64_buf_t *b, unsigned rd, unsigned rn,
                        unsigned rm, unsigned shift, unsigned amount)
{
    a64_emit(b, 0x4A000000U | ((shift & 3U) << 22) | ((rm & 31U) << 16) |
                    ((amount & 31U) << 10) | ((rn & 31U) << 5) | (rd & 31U));
}

void a64_emit_shift_reg_w(a64_buf_t *b, unsigned rd, unsigned rn, unsigned rm, unsigned shift_type)
{
    a64_emit(b, 0x1AC02000U | ((rm & 31U) << 16) | ((shift_type & 3U) << 10) |
                    ((rn & 31U) << 5) | (rd & 31U));
}

void a64_emit_arm_shift_reg_w(a64_buf_t *b, unsigned dst_w, unsigned src_m, unsigned src_s,
                              unsigned shift_type, bool want_carry)
{
    /* amt = src_s & 0xFF in A64_ADDR (0..255) */
    a64_emit_ubfm_w(b, A64_ADDR, src_s, 0U, 7U);

    if (want_carry) {
        /* am1 = amt - 1 in A64_TMP3 (0..254 when amt > 0; 0xFFFFFFFF when amt == 0) */
        a64_emit_sub_imm_w(b, false, A64_TMP3, A64_ADDR, 1U, 0U);
        if (shift_type == A64_SHIFT_LSL || shift_type == A64_SHIFT_LSR) {
            unsigned bit = (shift_type == A64_SHIFT_LSL) ? 31U : 0U;
            a64_emit_shift_reg_w(b, A64_TMP2, src_m, A64_TMP3, shift_type);
            a64_emit_ubfm_w(b, A64_TMP2, A64_TMP2, bit, bit);
            a64_emit_sub_reg_w(b, false, A64_TMP3, A64_ZR, A64_TMP3, A64_SHIFT_LSR, 5U);
            a64_emit_bic_reg_w(b, false, A64_TMP2, A64_TMP2, A64_TMP3, A64_SHIFT_LSR, 31U);
        } else if (shift_type == A64_SHIFT_ASR) {
            a64_emit_sub_reg_w(b, false, A64_TMP2, A64_ZR, A64_TMP3, A64_SHIFT_LSR, 5U);
            a64_emit_orr_reg_w(b, A64_TMP3, A64_TMP3, A64_TMP2, A64_SHIFT_ASR, 31U);
            a64_emit_shift_reg_w(b, A64_TMP2, src_m, A64_TMP3, A64_SHIFT_LSR);
            a64_emit_ubfm_w(b, A64_TMP2, A64_TMP2, 0U, 0U);
        } else {
            a64_emit_shift_reg_w(b, A64_TMP2, src_m, A64_TMP3, A64_SHIFT_LSR);
            a64_emit_ubfm_w(b, A64_TMP2, A64_TMP2, 0U, 0U);
        }
        /* When amt == 0, carry_out = old_C (preserving NZCV and A64_TMP0) */
        a64_emit_mrs_nzcv(b, A64_TMP3);
        a64_emit_ubfm_w(b, A64_TMP3, A64_TMP3, 29U, 29U);
        a64_emit_bfm_w(b, A64_TMP2, A64_TMP3, 31U, 0U);
        a64_emit_sub_imm_w(b, false, A64_TMP3, A64_ADDR, 1U, 0U);
        a64_emit_ubfm_w(b, A64_TMP3, A64_TMP3, 31U, 31U);
        a64_emit_shift_reg_w(b, A64_TMP2, A64_TMP2, A64_TMP3, A64_SHIFT_LSR);
        a64_emit_ubfm_w(b, A64_TMP2, A64_TMP2, 0U, 0U);
    }

    if (shift_type == A64_SHIFT_LSL || shift_type == A64_SHIFT_LSR) {
        a64_emit_sub_reg_w(b, false, A64_TMP3, A64_ZR, A64_ADDR, A64_SHIFT_LSR, 5U);
        a64_emit_shift_reg_w(b, dst_w, src_m, A64_ADDR, shift_type);
        a64_emit_bic_reg_w(b, false, dst_w, dst_w, A64_TMP3, A64_SHIFT_ASR, 31U);
    } else if (shift_type == A64_SHIFT_ASR) {
        a64_emit_sub_reg_w(b, false, A64_TMP3, A64_ZR, A64_ADDR, A64_SHIFT_LSR, 5U);
        a64_emit_orr_reg_w(b, A64_TMP3, A64_ADDR, A64_TMP3, A64_SHIFT_ASR, 31U);
        a64_emit_shift_reg_w(b, dst_w, src_m, A64_TMP3, A64_SHIFT_ASR);
    } else {
        a64_emit_shift_reg_w(b, dst_w, src_m, A64_ADDR, A64_SHIFT_ROR);
    }
}

void a64_emit_shift_imm_w(a64_buf_t *b, unsigned dst_w, unsigned src_m, unsigned shift_type,
                          unsigned imm5, bool want_carry, unsigned *out_carry_reg)
{
    if (out_carry_reg) {
        *out_carry_reg = A64_ZR;
    }
    if (shift_type == A64_SHIFT_LSL) {
        if (imm5 == 0U) {
            a64_emit_mov_w(b, dst_w, src_m);
        } else {
            if (want_carry && out_carry_reg) {
                a64_emit_ubfm_w(b, A64_TMP2, src_m, 32U - imm5, 32U - imm5);
                *out_carry_reg = A64_TMP2;
            }
            a64_emit_ubfm_w(b, dst_w, src_m, (32U - imm5) & 31U, 31U - imm5);
        }
    } else if (shift_type == A64_SHIFT_LSR) {
        if (imm5 == 0U) {
            if (want_carry && out_carry_reg) {
                a64_emit_ubfm_w(b, A64_TMP2, src_m, 31U, 31U);
                *out_carry_reg = A64_TMP2;
            }
            a64_emit_movz_w(b, dst_w, 0U, 0U);
        } else {
            if (want_carry && out_carry_reg) {
                a64_emit_ubfm_w(b, A64_TMP2, src_m, imm5 - 1U, imm5 - 1U);
                *out_carry_reg = A64_TMP2;
            }
            a64_emit_ubfm_w(b, dst_w, src_m, imm5, 31U);
        }
    } else if (shift_type == A64_SHIFT_ASR) {
        unsigned amt = (imm5 == 0U) ? 31U : imm5;
        if (want_carry && out_carry_reg) {
            unsigned cbit = (imm5 == 0U) ? 31U : (imm5 - 1U);
            a64_emit_ubfm_w(b, A64_TMP2, src_m, cbit, cbit);
            *out_carry_reg = A64_TMP2;
        }
        a64_emit_sbfm_w(b, dst_w, src_m, amt, 31U);
    } else {
        if (imm5 == 0U) {
            /* RRX: (C << 31) | (src_m >> 1), carry_out = src_m[0] */
            if (want_carry && out_carry_reg) {
                a64_emit_ubfm_w(b, A64_TMP2, src_m, 0U, 0U);
                *out_carry_reg = A64_TMP2;
            }
            a64_emit_mrs_nzcv(b, A64_TMP3);
            a64_emit_ubfm_w(b, A64_TMP3, A64_TMP3, 29U, 29U);
            a64_emit_ubfm_w(b, dst_w, src_m, 1U, 31U);
            a64_emit_orr_reg_w(b, dst_w, dst_w, A64_TMP3, A64_SHIFT_LSL, 31U);
        } else {
            if (want_carry && out_carry_reg) {
                a64_emit_ubfm_w(b, A64_TMP2, src_m, imm5 - 1U, imm5 - 1U);
                *out_carry_reg = A64_TMP2;
            }
            a64_emit(b, 0x13800000U | ((src_m & 31U) << 16) | ((imm5 & 31U) << 10) |
                            ((src_m & 31U) << 5) | (dst_w & 31U));
        }
    }
}

void a64_emit_ubfm_w(a64_buf_t *b, unsigned rd, unsigned rn, unsigned immr, unsigned imms)
{
    a64_emit(b, 0x53000000U | ((immr & 31U) << 16) | ((imms & 31U) << 10) |
                    ((rn & 31U) << 5) | (rd & 31U));
}

void a64_emit_sbfm_w(a64_buf_t *b, unsigned rd, unsigned rn, unsigned immr, unsigned imms)
{
    a64_emit(b, 0x13000000U | ((immr & 31U) << 16) | ((imms & 31U) << 10) |
                    ((rn & 31U) << 5) | (rd & 31U));
}

void a64_emit_bfm_w(a64_buf_t *b, unsigned rd, unsigned rn, unsigned immr, unsigned imms)
{
    a64_emit(b, 0x33000000U | ((immr & 31U) << 16) | ((imms & 31U) << 10) |
                    ((rn & 31U) << 5) | (rd & 31U));
}


void a64_emit_ubfx_x_to_w(a64_buf_t *b, unsigned rd_w, unsigned rn_x, unsigned lsb, unsigned width)
{
    /* 64-bit UBFM Xd, Xn, #lsb, #(lsb + width - 1) */
    unsigned immr = lsb & 63U;
    unsigned imms = (lsb + width - 1U) & 63U;
    a64_emit(b, 0xD3400000U | (immr << 16) | (imms << 10) | ((rn_x & 31U) << 5) | (rd_w & 31U));
}

void a64_emit_bfi_x(a64_buf_t *b, unsigned rd_x, unsigned rn_x, unsigned lsb, unsigned width)
{
    /* 64-bit BFM Xd, Xn, #((64 - lsb) & 63), #(width - 1) */
    unsigned immr = (64U - lsb) & 63U;
    unsigned imms = (width - 1U) & 63U;
    a64_emit(b, 0xB3400000U | (immr << 16) | (imms << 10) | ((rn_x & 31U) << 5) | (rd_x & 31U));
}

void a64_emit_clz_w(a64_buf_t *b, unsigned rd, unsigned rn)
{
    a64_emit(b, 0x5AC01000U | ((rn & 31U) << 5) | (rd & 31U));
}

void a64_emit_rbit_w(a64_buf_t *b, unsigned rd, unsigned rn)
{
    a64_emit(b, 0x5AC00000U | ((rn & 31U) << 5) | (rd & 31U));
}

void a64_emit_rev_w(a64_buf_t *b, unsigned rd, unsigned rn)
{
    a64_emit(b, 0x5AC00800U | ((rn & 31U) << 5) | (rd & 31U));
}

void a64_emit_rev16_w(a64_buf_t *b, unsigned rd, unsigned rn)
{
    a64_emit(b, 0x5AC00400U | ((rn & 31U) << 5) | (rd & 31U));
}

void a64_emit_madd_w(a64_buf_t *b, unsigned rd, unsigned rn, unsigned rm, unsigned ra)
{
    a64_emit(b, 0x1B000000U | ((rm & 31U) << 16) | ((ra & 31U) << 10) |
                    ((rn & 31U) << 5) | (rd & 31U));
}

void a64_emit_msub_w(a64_buf_t *b, unsigned rd, unsigned rn, unsigned rm, unsigned ra)
{
    a64_emit(b, 0x1B008000U | ((rm & 31U) << 16) | ((ra & 31U) << 10) |
                    ((rn & 31U) << 5) | (rd & 31U));
}

void a64_emit_umaddl_x(a64_buf_t *b, unsigned rd_x, unsigned rn_w, unsigned rm_w, unsigned ra_x)
{
    a64_emit(b, 0x9BA00000U | ((rm_w & 31U) << 16) | ((ra_x & 31U) << 10) |
                    ((rn_w & 31U) << 5) | (rd_x & 31U));
}

void a64_emit_smaddl_x(a64_buf_t *b, unsigned rd_x, unsigned rn_w, unsigned rm_w, unsigned ra_x)
{
    a64_emit(b, 0x9B200000U | ((rm_w & 31U) << 16) | ((ra_x & 31U) << 10) |
                    ((rn_w & 31U) << 5) | (rd_x & 31U));
}

void a64_emit_smsubl_x(a64_buf_t *b, unsigned rd_x, unsigned rn_w, unsigned rm_w, unsigned ra_x)
{
    a64_emit(b, 0x9B208000U | ((rm_w & 31U) << 16) | ((ra_x & 31U) << 10) |
                    ((rn_w & 31U) << 5) | (rd_x & 31U));
}

void a64_emit_udiv_w(a64_buf_t *b, unsigned rd, unsigned rn, unsigned rm)
{
    a64_emit(b, 0x1AC00800U | ((rm & 31U) << 16) | ((rn & 31U) << 5) | (rd & 31U));
}

void a64_emit_sdiv_w(a64_buf_t *b, unsigned rd, unsigned rn, unsigned rm)
{
    a64_emit(b, 0x1AC00C00U | ((rm & 31U) << 16) | ((rn & 31U) << 5) | (rd & 31U));
}

void a64_emit_crc32_w(a64_buf_t *b, unsigned rd, unsigned rn, unsigned rm, unsigned sz, unsigned c_bit)
{
    a64_emit(b, 0x1AC04000U | ((rm & 31U) << 16) | ((c_bit & 1U) << 12) |
                    ((sz & 3U) << 10) | ((rn & 31U) << 5) | (rd & 31U));
}

void a64_emit_csel_w(a64_buf_t *b, unsigned rd, unsigned rn, unsigned rm, unsigned cond)
{
    a64_emit(b, 0x1A800000U | ((rm & 31U) << 16) | ((cond & 15U) << 12) |
                    ((rn & 31U) << 5) | (rd & 31U));
}

void a64_emit_csneg_w(a64_buf_t *b, unsigned rd, unsigned rn, unsigned rm, unsigned cond)
{
    a64_emit(b, 0x5A800400U | ((rm & 31U) << 16) | ((cond & 15U) << 12) |
                    ((rn & 31U) << 5) | (rd & 31U));
}

void a64_emit_set_q_cond(a64_buf_t *b, unsigned cond)
{
    a64_emit_movz_w(b, A64_TMP1, 0x0800U, 1U);
    a64_emit_csel_w(b, A64_TMP1, A64_TMP1, A64_ZR, cond);
    a64_emit_ldr_imm_w(b, A64_TMP2, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, cpsr));
    a64_emit_orr_reg_w(b, A64_TMP2, A64_TMP2, A64_TMP1, A64_SHIFT_LSL, 0U);
    a64_emit_str_imm_w(b, A64_TMP2, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, cpsr));
}

void a64_emit_mrs_nzcv(a64_buf_t *b, unsigned rt)
{
    a64_emit(b, 0xD53B4200U | (rt & 31U));
}

void a64_emit_msr_nzcv(a64_buf_t *b, unsigned rt)
{
    a64_emit(b, 0xD51B4200U | (rt & 31U));
}

/*
 * Logical flag helpers:
 *   A32/T32 logical ops with S=1 set N and Z from the 32-bit result,
 *   optionally update C from the barrel-shifter carry-out, and leave V unchanged.
 */
void a64_emit_update_nz_keep_cv(a64_buf_t *b, unsigned res_reg)
{
    /* Save old C and V in TMP3[29:28], compute new N and Z via ANDS wzr, res_reg, res_reg */
    a64_emit_mrs_nzcv(b, A64_TMP3);
    a64_emit_and_reg_w(b, true, A64_ZR, res_reg, res_reg, A64_SHIFT_LSL, 0U);
    a64_emit_mrs_nzcv(b, A64_TMP2);
    /* Merge old C and V (bits 29:28 of TMP3) into TMP2 (which has C=0, V=0) */
    a64_emit_ubfm_w(b, A64_TMP3, A64_TMP3, 28U, 29U); /* TMP3 = old_CV[1:0] */
    a64_emit_orr_reg_w(b, A64_TMP2, A64_TMP2, A64_TMP3, A64_SHIFT_LSL, 28U);
    a64_emit_msr_nzcv(b, A64_TMP2);
}

void a64_emit_update_nzc_keep_v_imm(a64_buf_t *b, unsigned res_reg, unsigned carry_bit)
{
    a64_emit_mrs_nzcv(b, A64_TMP3);
    a64_emit_and_reg_w(b, true, A64_ZR, res_reg, res_reg, A64_SHIFT_LSL, 0U);
    a64_emit_mrs_nzcv(b, A64_TMP2);
    /* Keep old V (bit 28 of TMP3) */
    a64_emit_ubfm_w(b, A64_TMP3, A64_TMP3, 28U, 28U);
    a64_emit_orr_reg_w(b, A64_TMP2, A64_TMP2, A64_TMP3, A64_SHIFT_LSL, 28U);
    if (carry_bit & 1U) {
        /* Set C (bit 29) */
        a64_emit_movz_w(b, A64_TMP3, 0x2000U, 1U);
        a64_emit_orr_reg_w(b, A64_TMP2, A64_TMP2, A64_TMP3, A64_SHIFT_LSL, 0U);
    }
    a64_emit_msr_nzcv(b, A64_TMP2);
}

void a64_emit_update_nzc_keep_v_reg(a64_buf_t *b, unsigned res_reg, unsigned carry_reg)
{
    a64_emit_mrs_nzcv(b, A64_TMP3);
    a64_emit_ubfm_w(b, A64_TMP3, A64_TMP3, 28U, 28U);
    a64_emit_bfm_w(b, A64_TMP3, carry_reg, 31U, 0U);
    a64_emit_and_reg_w(b, true, A64_ZR, res_reg, res_reg, A64_SHIFT_LSL, 0U);
    a64_emit_mrs_nzcv(b, A64_TMP2);
    a64_emit_orr_reg_w(b, A64_TMP2, A64_TMP2, A64_TMP3, A64_SHIFT_LSL, 28U);
    a64_emit_msr_nzcv(b, A64_TMP2);
}

void a64_emit_logical_flags(a64_buf_t *b, unsigned res_reg, bool has_imm_carry,
                            unsigned imm_carry, unsigned shift_carry_reg)
{
    if (has_imm_carry) {
        a64_emit_update_nzc_keep_v_imm(b, res_reg, imm_carry);
    } else if (shift_carry_reg != A64_ZR) {
        a64_emit_update_nzc_keep_v_reg(b, res_reg, shift_carry_reg);
    } else {
        a64_emit_update_nz_keep_cv(b, res_reg);
    }
}

static void vfp_emit_raw_ldr(a64_buf_t *b, uint8_t kind, unsigned vt, unsigned idx)
{
    static const uint32_t k_ldr_opc[4] = { 0U, 0xBD400000U, 0xFD400000U, 0x3DC00000U };
    unsigned shift = (unsigned)kind + 1U;
    uint32_t byte_offset = A32_CTX_VFP_OFFSET + (idx << shift);
    a64_emit(b, k_ldr_opc[kind] | (((byte_offset >> shift) & 0xFFFU) << 10) |
                    (A64_XCTX << 5) | (vt & 31U));
}

static void vfp_emit_raw_str(a64_buf_t *b, uint8_t kind, unsigned vt, unsigned idx)
{
    static const uint32_t k_str_opc[4] = { 0U, 0xBD000000U, 0xFD000000U, 0x3D800000U };
    unsigned shift = (unsigned)kind + 1U;
    uint32_t byte_offset = A32_CTX_VFP_OFFSET + (idx << shift);
    a64_emit(b, k_str_opc[kind] | (((byte_offset >> shift) & 0xFFFU) << 10) |
                    (A64_XCTX << 5) | (vt & 31U));
}

static void a64_emit_fmov_s(a64_buf_t *b, unsigned vd, unsigned vn)
{
    a64_emit(b, 0x1E204000U | ((vn & 31U) << 5) | (vd & 31U));
}

static void a64_emit_fmov_d(a64_buf_t *b, unsigned vd, unsigned vn)
{
    a64_emit(b, 0x1E604000U | ((vn & 31U) << 5) | (vd & 31U));
}

static void vfp_emit_reg_mov(a64_buf_t *b, uint8_t kind, unsigned vd, unsigned vn)
{
    if (kind == A64_VFP_KIND_S) {
        a64_emit_fmov_s(b, vd, vn);
    } else if (kind == A64_VFP_KIND_D) {
        a64_emit_fmov_d(b, vd, vn);
    } else {
        a64_emit(b, 0x4EA01C00U | ((vn & 31U) << 16) | ((vn & 31U) << 5) | (vd & 31U));
    }
}

static void vfp_flush_slot(a64_buf_t *b, unsigned slot, bool invalidate)
{
    if ((b->vfp_slot_dirty & (1U << slot)) != 0U) {
        vfp_emit_raw_str(b, b->vfp_slot_kind[slot], 8U + slot, b->vfp_slot_reg[slot]);
        b->vfp_slot_dirty &= ~(1U << slot);
    }
    if (invalidate) {
        b->vfp_slot_kind[slot] = A64_VFP_KIND_NONE;
    }
}

void a64_vfp_cache_flush(a64_buf_t *b, bool invalidate)
{
    if (b->vfp_slot_dirty != 0U) {
        for (unsigned i = 0U; i < A64_VFP_CACHE_SLOTS; i++) {
            if ((b->vfp_slot_dirty & (1U << i)) != 0U) {
                vfp_flush_slot(b, i, invalidate);
            } else if (invalidate) {
                b->vfp_slot_kind[i] = A64_VFP_KIND_NONE;
            }
        }
    } else if (invalidate) {
        for (unsigned i = 0U; i < A64_VFP_CACHE_SLOTS; i++) {
            b->vfp_slot_kind[i] = A64_VFP_KIND_NONE;
        }
    }
}

static unsigned vfp_alloc_slot(a64_buf_t *b, uint8_t kind, uint8_t reg)
{
    for (unsigned i = 0U; i < A64_VFP_CACHE_SLOTS; i++) {
        if (b->vfp_slot_kind[i] == A64_VFP_KIND_NONE) {
            b->vfp_slot_kind[i] = kind;
            b->vfp_slot_reg[i]  = reg;
            return i;
        }
    }
    for (unsigned step = 0U; step < A64_VFP_CACHE_SLOTS; step++) {
        unsigned idx = (b->vfp_next_evict + step) % A64_VFP_CACHE_SLOTS;
        if ((b->vfp_slot_dirty & (1U << idx)) == 0U) {
            b->vfp_next_evict = (uint8_t)((idx + 1U) % A64_VFP_CACHE_SLOTS);
            b->vfp_slot_kind[idx] = kind;
            b->vfp_slot_reg[idx]  = reg;
            return idx;
        }
    }
    unsigned idx = b->vfp_next_evict % A64_VFP_CACHE_SLOTS;
    b->vfp_next_evict = (uint8_t)((idx + 1U) % A64_VFP_CACHE_SLOTS);
    vfp_flush_slot(b, idx, true);
    b->vfp_slot_kind[idx] = kind;
    b->vfp_slot_reg[idx]  = reg;
    return idx;
}

static inline bool vfp_regs_alias(uint8_t k1, uint8_t r1, uint8_t k2, uint8_t r2)
{
    return (k1 <= k2) ? ((r1 >> (k2 - k1)) == r2) : ((r2 >> (k1 - k2)) == r1);
}

static void vfp_record_preheader_access(a64_buf_t *b, uint8_t kind, uint8_t reg, bool is_write)
{
    if (b->vfp_preheader_ineligible || b->self_loop_vfp_active) {
        return;
    }
    for (unsigned i = 0U; i < b->vfp_preheader_count; i++) {
        if (b->vfp_preheader_kinds[i] == kind && b->vfp_preheader_regs[i] == reg) {
            if (is_write) {
                b->vfp_preheader_written |= (1U << i);
            }
            return;
        }
        if (vfp_regs_alias(b->vfp_preheader_kinds[i], b->vfp_preheader_regs[i], kind, reg)) {
            b->vfp_preheader_ineligible = true;
            return;
        }
    }
    if (b->vfp_preheader_count >= A64_VFP_CACHE_SLOTS) {
        return;
    }
    unsigned idx = b->vfp_preheader_count++;
    b->vfp_preheader_kinds[idx] = kind;
    b->vfp_preheader_regs[idx]  = reg;
    if (is_write) {
        b->vfp_preheader_written |= (1U << idx);
    }
}

static void vfp_emit_exit_flush(a64_buf_t *b)
{
    if (b->self_loop_vfp_active) {
        for (unsigned i = 0U; i < A64_VFP_CACHE_SLOTS; i++) {
            if ((b->vfp_slot_dirty & (1U << i)) != 0U) {
                vfp_emit_raw_str(b, b->vfp_slot_kind[i], 8U + i, b->vfp_slot_reg[i]);
            }
        }
    } else {
        a64_vfp_cache_flush(b, false);
    }
}

void a64_buf_start_self_loop(a64_buf_t *b, const uint8_t *kinds, const uint8_t *regs,
                             unsigned count, uint32_t written_mask)
{
    b->self_loop_vfp_active = true;
    b->vfp_slot_dirty       = written_mask;
    for (unsigned i = 0U; i < count; i++) {
        b->vfp_slot_kind[i] = kinds[i];
        b->vfp_slot_reg[i]  = regs[i];
        vfp_emit_raw_ldr(b, kinds[i], 8U + i, regs[i]);
    }
    b->loop_body_offset = b->count;
}

/* ---- Per-block AOT site staging (P2 #7) ----
 * The single-access memory emitters below append one 24-byte site record
 * per access; the batch is handed to a32_aot_site_track() when the block
 * changes or when a32_emit_sites_flush() runs (AOT save path). Records use
 * the fault-recovery layout of section 4.3.3; insn_word stays 0 because
 * the opcode word is not threaded to this layer, and npc_delta is the A32
 * 4-byte step (Thumb variable-length NPC is approximated). */
static a32_vm32_mem_site_t g_site_stage[A32_AOT_SITE_PER_BLOCK];
static uint32_t g_site_stage_base = 0U;
static uint32_t g_site_stage_count = 0U;
static bool g_site_stage_valid = false;

void a32_emit_sites_flush(void)
{
    if (g_site_stage_valid && g_site_stage_count > 0U) {
        (void)a32_aot_site_track(g_site_stage_base, g_site_stage,
            g_site_stage_count);
    }
    g_site_stage_valid = false;
    g_site_stage_count = 0U;
}

static void emit_site_record(a64_buf_t *b, uint32_t opc, unsigned rt,
    unsigned addr_w, uint32_t host_off)
{
    a32_vm32_mem_site_t rec;
    uint32_t cur_pc;
    if (g_site_stage_valid && b->guest_start_pc != g_site_stage_base) {
        a32_emit_sites_flush();
    }
    if (g_site_stage_count >= A32_AOT_SITE_PER_BLOCK) {
        return;
    }
    cur_pc = a32_jit_last_record_pc();
    memset(&rec, 0, sizeof(rec));
    rec.seq_start_offset = 0U;
    rec.host_insn_offset = (uint16_t)(host_off & 0xFFFFU);
    rec.cont_insn_offset = 0U;
    rec.guest_pc_delta = cur_pc - b->guest_start_pc;
    rec.npc_delta = 4;
    rec.insn_word = 0U;
    rec.sp_rollback_delta = 0;
    rec.npc_mode = (uint8_t)A32_VM32_NPC_SEQUENTIAL;
    rec.width = (uint8_t)(1U << ((opc >> 30U) & 3U));
    rec.flags = (((opc & 0x400000U) == 0U) ? (uint8_t)A32_VM32_SITE_STORE : 0U);
    if (rec.width <= 2U && (opc & 0x800000U) != 0U) {
        rec.flags |= (uint8_t)A32_VM32_SITE_SIGNED;
    }
    rec.val_or_dst_reg = (uint8_t)(rt & 0xFFU);
    rec.addr_base_reg = (uint8_t)(addr_w & 0xFFU);
    rec.reserved = 0U;
    g_site_stage[g_site_stage_count++] = rec;
    g_site_stage_base = b->guest_start_pc;
    g_site_stage_valid = true;
}

/* X-form MOVZ/MOVK for 64-bit immediates (TLB-base materialization). */
static void emit_movz_x(a64_buf_t *b, unsigned rd, uint16_t imm16, unsigned hw)
{
    a64_emit(b, 0xD2800000U | ((hw & 3U) << 21) | ((uint32_t)imm16 << 5) | (rd & 31U));
}

static void emit_movk_x(a64_buf_t *b, unsigned rd, uint16_t imm16, unsigned hw)
{
    a64_emit(b, 0xF2800000U | ((hw & 3U) << 21) | ((uint32_t)imm16 << 5) | (rd & 31U));
}

static void emit_guest_mem(a64_buf_t *b, uint32_t opc, unsigned rt, unsigned addr_w)
{
    if (!b->self_loop_vfp_active && b->vfp_slot_dirty != 0U) {
        a64_vfp_cache_flush(b, false);
    }
    emit_site_record(b, opc, rt, addr_w, (uint32_t)b->count);
    if (a32_vm32_current_regime() != A32_VM32_REGIME_A2_SOFT_TLB ||
        a32_vm32_tlb_base == 0U) {
        a64_emit(b, opc | ((addr_w & 31U) << 16) | (A64_XG << 5) | (rt & 31U));
        return;
    }
    /* Regime A2: 7-instruction Soft-TLB lookup on x21 (section 4.3).
     * x21 doubles as ALU scratch (TMP3), so the base is materialized per
     * access; the 7 lookup instructions themselves never touch PSTATE or
     * w0/w1, and a tag miss exits cleanly to the fault path below with
     * (fault_addr, faulting PC, EXIT_FAULT) in (mem, w15, w16). Full TLB
     * coverage (1M entries) makes every miss a genuine fault: no mid-block
     * resume is required. Multi-word LDM/STM/LDRD/STRD keep direct
     * addressing (correct but union-mapped); see the header notes. */
    {
        uint64_t base = (uint64_t)a32_vm32_tlb_base;
        unsigned s0 = 15U;
        unsigned s1 = 16U;
        uint16_t h0 = (uint16_t)(base & 0xFFFFU);
        uint16_t h1 = (uint16_t)((base >> 16) & 0xFFFFU);
        uint16_t h2 = (uint16_t)((base >> 32) & 0xFFFFU);
        uint16_t h3 = (uint16_t)((base >> 48) & 0xFFFFU);
        uint32_t cur_pc = a32_jit_last_record_pc();
        uint32_t tag_off = (((opc & 0x400000U) == 0U) ? 16U : 0U);
        if (addr_w == 15U) {
            s0 = 16U;
            s1 = 17U;
        } else if (addr_w == 16U) {
            s0 = 15U;
            s1 = 17U;
        } else if (addr_w == 17U) {
            s0 = 15U;
            s1 = 16U;
        }
        emit_movz_x(b, 21U, h0, 0U);
        if (h1 != 0U) {
            emit_movk_x(b, 21U, h1, 1U);
        }
        if (h2 != 0U) {
            emit_movk_x(b, 21U, h2, 2U);
        }
        if (h3 != 0U) {
            emit_movk_x(b, 21U, h3, 3U);
        }
        a64_emit_ubfm_w(b, s0, addr_w, 12U, 19U);
        a64_emit_orr_reg_w(b, s0, A64_ZR, s0, A64_SHIFT_LSL, 5U);
        a64_emit_add_uxtw_x(b, s0, 21U, s0);
        a64_emit_ldr_imm_w(b, s1, s0, tag_off);
        a64_emit_ubfm_w(b, s1, s1, 12U, 19U);
        a64_emit_eor_reg_w(b, s1, s1, s0, A64_SHIFT_LSL, 0U);
        /* CBZ s1, +5 words: tag hit skips the 5-instruction miss stub. */
        a64_emit(b, 0x34000000U | (5U << 5) | (s1 & 31U));
        a64_emit_str_imm_w(b, addr_w, A64_XCTX,
            (uint32_t)offsetof(a32_ctx_t, fault_addr));
        a64_emit_mov_pc32(b, 15U, cur_pc);
        a64_emit_movz_w(b, 16U, (uint16_t)A32_EXIT_FAULT, 0U);
        a64_emit(b, 0xD65F03C0U);
        /* Hit: addend load plus the original single access via [addend, wA]. */
        a64_emit(b, 0xF9400000U | (1U << 10) | ((s0 & 31U) << 5) | (s1 & 31U));
        a64_emit(b, opc | ((addr_w & 31U) << 16) | ((s1 & 31U) << 5) | (rt & 31U));
    }
}
