/*
 * src/a64_emit.c — Direct 32-bit AArch64 instruction encoder implementation
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "a64_emit.h"

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

static void emit_guest_mem(a64_buf_t *b, uint32_t opc, unsigned rt, unsigned addr_w)
{
    if (!b->self_loop_vfp_active && b->vfp_slot_dirty != 0U) {
        a64_vfp_cache_flush(b, false);
    }
    a64_emit(b, opc | ((addr_w & 31U) << 16) | (A64_XG << 5) | (rt & 31U));
}

/* Guest memory accesses: [xG, wAddr, uxtw #0] */
void a64_emit_guest_ldr_w(a64_buf_t *b, unsigned rt, unsigned addr_w)
{
    emit_guest_mem(b, 0xB8604800U, rt, addr_w);
}

void a64_emit_guest_str_w(a64_buf_t *b, unsigned rt, unsigned addr_w)
{
    emit_guest_mem(b, 0xB8204800U, rt, addr_w);
}

void a64_emit_guest_ldrb_w(a64_buf_t *b, unsigned rt, unsigned addr_w)
{
    emit_guest_mem(b, 0x38604800U, rt, addr_w);
}

void a64_emit_guest_ldrsb_w(a64_buf_t *b, unsigned rt, unsigned addr_w)
{
    emit_guest_mem(b, 0x38A04800U, rt, addr_w);
}

void a64_emit_guest_strb_w(a64_buf_t *b, unsigned rt, unsigned addr_w)
{
    emit_guest_mem(b, 0x38204800U, rt, addr_w);
}

void a64_emit_guest_ldrh_w(a64_buf_t *b, unsigned rt, unsigned addr_w)
{
    emit_guest_mem(b, 0x78604800U, rt, addr_w);
}

void a64_emit_guest_ldrsh_w(a64_buf_t *b, unsigned rt, unsigned addr_w)
{
    emit_guest_mem(b, 0x78A04800U, rt, addr_w);
}

void a64_emit_guest_strh_w(a64_buf_t *b, unsigned rt, unsigned addr_w)
{
    emit_guest_mem(b, 0x78204800U, rt, addr_w);
}

static void emit_ctx_mem_imm(a64_buf_t *b, uint32_t opc, unsigned shift, bool is_store,
                             unsigned rt, unsigned rn_x, uint32_t byte_offset)
{
    if (rn_x == A64_XCTX && byte_offset >= A32_CTX_VFP_OFFSET && byte_offset < A32_CTX_VFP_OFFSET + 256U) {
        b->vfp_preheader_ineligible = true;
        a64_vfp_cache_flush(b, is_store);
    }
    uint32_t uimm12 = (byte_offset >> shift) & 0xFFFU;
    a64_emit(b, opc | (uimm12 << 10) | ((rn_x & 31U) << 5) | (rt & 31U));
}

/* Host unsigned-offset loads/stores */
void a64_emit_ldrb_imm_w(a64_buf_t *b, unsigned rt, unsigned rn_x, uint32_t byte_offset)
{
    emit_ctx_mem_imm(b, 0x39400000U, 0U, false, rt, rn_x, byte_offset);
}

void a64_emit_strb_imm_w(a64_buf_t *b, unsigned rt, unsigned rn_x, uint32_t byte_offset)
{
    emit_ctx_mem_imm(b, 0x39000000U, 0U, true, rt, rn_x, byte_offset);
}

void a64_emit_ldrh_imm_w(a64_buf_t *b, unsigned rt, unsigned rn_x, uint32_t byte_offset)
{
    emit_ctx_mem_imm(b, 0x79400000U, 1U, false, rt, rn_x, byte_offset);
}

void a64_emit_strh_imm_w(a64_buf_t *b, unsigned rt, unsigned rn_x, uint32_t byte_offset)
{
    emit_ctx_mem_imm(b, 0x79000000U, 1U, true, rt, rn_x, byte_offset);
}

void a64_emit_ldr_imm_w(a64_buf_t *b, unsigned rt, unsigned rn_x, uint32_t byte_offset)
{
    emit_ctx_mem_imm(b, 0xB9400000U, 2U, false, rt, rn_x, byte_offset);
}

void a64_emit_str_imm_w(a64_buf_t *b, unsigned rt, unsigned rn_x, uint32_t byte_offset)
{
    emit_ctx_mem_imm(b, 0xB9000000U, 2U, true, rt, rn_x, byte_offset);
}

/* VFPv4-D32 / NEON context & guest memory loads/stores */
static void vfp_ldr_ctx(a64_buf_t *b, uint8_t kind, unsigned vt, unsigned idx)
{
    vfp_record_preheader_access(b, kind, (uint8_t)idx, false);
    if (b->self_loop_vfp_active) {
        for (unsigned i = 0U; i < A64_VFP_CACHE_SLOTS; i++) {
            if (b->vfp_slot_kind[i] == kind && b->vfp_slot_reg[i] == idx) {
                vfp_emit_reg_mov(b, kind, vt, 8U + i);
                return;
            }
        }
        vfp_emit_raw_ldr(b, kind, vt, idx);
        return;
    }
    for (unsigned i = 0U; i < A64_VFP_CACHE_SLOTS; i++) {
        if (b->vfp_slot_kind[i] != A64_VFP_KIND_NONE && b->vfp_slot_kind[i] != kind &&
            vfp_regs_alias(b->vfp_slot_kind[i], b->vfp_slot_reg[i], kind, (uint8_t)idx)) {
            vfp_flush_slot(b, i, true);
        }
    }
    for (unsigned i = 0U; i < A64_VFP_CACHE_SLOTS; i++) {
        if (b->vfp_slot_kind[i] == kind && b->vfp_slot_reg[i] == idx) {
            vfp_emit_reg_mov(b, kind, vt, 8U + i);
            return;
        }
    }
    vfp_emit_raw_ldr(b, kind, vt, idx);
    if (b->cond_branch_depth == 0U) {
        unsigned slot = vfp_alloc_slot(b, kind, (uint8_t)idx);
        vfp_emit_reg_mov(b, kind, 8U + slot, vt);
    }
}

static void vfp_str_ctx(a64_buf_t *b, uint8_t kind, unsigned vt, unsigned idx)
{
    vfp_record_preheader_access(b, kind, (uint8_t)idx, true);
    if (b->self_loop_vfp_active) {
        for (unsigned i = 0U; i < A64_VFP_CACHE_SLOTS; i++) {
            if (b->vfp_slot_kind[i] == kind && b->vfp_slot_reg[i] == idx) {
                vfp_emit_reg_mov(b, kind, 8U + i, vt);
                b->vfp_slot_dirty |= (1U << i);
                return;
            }
        }
        vfp_emit_raw_str(b, kind, vt, idx);
        return;
    }
    for (unsigned i = 0U; i < A64_VFP_CACHE_SLOTS; i++) {
        if (b->vfp_slot_kind[i] != A64_VFP_KIND_NONE && b->vfp_slot_kind[i] != kind &&
            vfp_regs_alias(b->vfp_slot_kind[i], b->vfp_slot_reg[i], kind, (uint8_t)idx)) {
            if (b->vfp_slot_kind[i] < kind && b->cond_branch_depth == 0U) {
                b->vfp_slot_dirty &= ~(1U << i);
                b->vfp_slot_kind[i] = A64_VFP_KIND_NONE;
            } else {
                vfp_flush_slot(b, i, true);
            }
        }
    }
    if (b->cond_branch_depth == 0U) {
        for (unsigned i = 0U; i < A64_VFP_CACHE_SLOTS; i++) {
            if (b->vfp_slot_kind[i] == kind && b->vfp_slot_reg[i] == idx) {
                vfp_emit_reg_mov(b, kind, 8U + i, vt);
                b->vfp_slot_dirty |= (1U << i);
                return;
            }
        }
        unsigned slot = vfp_alloc_slot(b, kind, (uint8_t)idx);
        vfp_emit_reg_mov(b, kind, 8U + slot, vt);
        b->vfp_slot_dirty |= (1U << slot);
        return;
    }
    for (unsigned i = 0U; i < A64_VFP_CACHE_SLOTS; i++) {
        if (b->vfp_slot_kind[i] == kind && b->vfp_slot_reg[i] == idx) {
            b->vfp_slot_dirty &= ~(1U << i);
            b->vfp_slot_kind[i] = A64_VFP_KIND_NONE;
        }
    }
    vfp_emit_raw_str(b, kind, vt, idx);
}

void a64_emit_ldr_s_ctx(a64_buf_t *b, unsigned vt, unsigned s_idx)
{
    vfp_ldr_ctx(b, A64_VFP_KIND_S, vt, s_idx);
}

void a64_emit_str_s_ctx(a64_buf_t *b, unsigned vt, unsigned s_idx)
{
    vfp_str_ctx(b, A64_VFP_KIND_S, vt, s_idx);
}

void a64_emit_ldr_d_ctx(a64_buf_t *b, unsigned vt, unsigned d_idx)
{
    vfp_ldr_ctx(b, A64_VFP_KIND_D, vt, d_idx);
}

void a64_emit_str_d_ctx(a64_buf_t *b, unsigned vt, unsigned d_idx)
{
    vfp_str_ctx(b, A64_VFP_KIND_D, vt, d_idx);
}

void a64_emit_ldr_q_ctx(a64_buf_t *b, unsigned vt, unsigned q_idx)
{
    vfp_ldr_ctx(b, A64_VFP_KIND_Q, vt, q_idx);
}

void a64_emit_str_q_ctx(a64_buf_t *b, unsigned vt, unsigned q_idx)
{
    vfp_str_ctx(b, A64_VFP_KIND_Q, vt, q_idx);
}

void a64_emit_guest_ldr_s(a64_buf_t *b, unsigned vt, unsigned addr_w)
{
    emit_guest_mem(b, 0xBC604800U, vt, addr_w);
}

void a64_emit_guest_str_s(a64_buf_t *b, unsigned vt, unsigned addr_w)
{
    emit_guest_mem(b, 0xBC204800U, vt, addr_w);
}

void a64_emit_guest_ldr_d(a64_buf_t *b, unsigned vt, unsigned addr_w)
{
    emit_guest_mem(b, 0xFC604800U, vt, addr_w);
}

void a64_emit_guest_str_d(a64_buf_t *b, unsigned vt, unsigned addr_w)
{
    emit_guest_mem(b, 0xFC204800U, vt, addr_w);
}

/* Scalar FP instructions */
void a64_emit_fmov_w_to_s(a64_buf_t *b, unsigned vd, unsigned rn_w)
{
    a64_emit(b, 0x1E270000U | ((rn_w & 31U) << 5) | (vd & 31U));
}

void a64_emit_fmov_s_to_w(a64_buf_t *b, unsigned rd_w, unsigned vn)
{
    a64_emit(b, 0x1E260000U | ((vn & 31U) << 5) | (rd_w & 31U));
}

void a64_emit_fcvt_s_to_d(a64_buf_t *b, unsigned vd, unsigned vn)
{
    a64_emit(b, 0x1E22C000U | ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_fcvt_d_to_s(a64_buf_t *b, unsigned vd, unsigned vn)
{
    a64_emit(b, 0x1E624000U | ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_fcvtzs_s_to_w(a64_buf_t *b, unsigned rd_w, unsigned vn)
{
    a64_emit(b, 0x1E380000U | ((vn & 31U) << 5) | (rd_w & 31U));
}

void a64_emit_fcvtzu_s_to_w(a64_buf_t *b, unsigned rd_w, unsigned vn)
{
    a64_emit(b, 0x1E390000U | ((vn & 31U) << 5) | (rd_w & 31U));
}

void a64_emit_fcvtzs_d_to_w(a64_buf_t *b, unsigned rd_w, unsigned vn)
{
    a64_emit(b, 0x1E780000U | ((vn & 31U) << 5) | (rd_w & 31U));
}

void a64_emit_fcvtzu_d_to_w(a64_buf_t *b, unsigned rd_w, unsigned vn)
{
    a64_emit(b, 0x1E790000U | ((vn & 31U) << 5) | (rd_w & 31U));
}

void a64_emit_scvtf_w_to_s(a64_buf_t *b, unsigned vd, unsigned rn_w)
{
    a64_emit(b, 0x1E220000U | ((rn_w & 31U) << 5) | (vd & 31U));
}

void a64_emit_ucvtf_w_to_s(a64_buf_t *b, unsigned vd, unsigned rn_w)
{
    a64_emit(b, 0x1E230000U | ((rn_w & 31U) << 5) | (vd & 31U));
}

void a64_emit_scvtf_w_to_d(a64_buf_t *b, unsigned vd, unsigned rn_w)
{
    a64_emit(b, 0x1E620000U | ((rn_w & 31U) << 5) | (vd & 31U));
}

void a64_emit_ucvtf_w_to_d(a64_buf_t *b, unsigned vd, unsigned rn_w)
{
    a64_emit(b, 0x1E630000U | ((rn_w & 31U) << 5) | (vd & 31U));
}

/* ARMv8-A Crypto extensions (AES, PMULL, SHA1, SHA256) */
void a64_emit_aese(a64_buf_t *b, unsigned vd, unsigned vn)
{
    a64_emit(b, 0x4E284800U | ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_aesd(a64_buf_t *b, unsigned vd, unsigned vn)
{
    a64_emit(b, 0x4E285800U | ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_aesmc(a64_buf_t *b, unsigned vd, unsigned vn)
{
    a64_emit(b, 0x4E286800U | ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_aesimc(a64_buf_t *b, unsigned vd, unsigned vn)
{
    a64_emit(b, 0x4E287800U | ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_pmull_1q(a64_buf_t *b, unsigned vd, unsigned vn, unsigned vm)
{
    a64_emit(b, 0x0EE0E000U | ((vm & 31U) << 16) | ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_sha1c(a64_buf_t *b, unsigned vd, unsigned vn, unsigned vm)
{
    a64_emit(b, 0x5E000000U | ((vm & 31U) << 16) | ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_sha1p(a64_buf_t *b, unsigned vd, unsigned vn, unsigned vm)
{
    a64_emit(b, 0x5E001000U | ((vm & 31U) << 16) | ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_sha1m(a64_buf_t *b, unsigned vd, unsigned vn, unsigned vm)
{
    a64_emit(b, 0x5E002000U | ((vm & 31U) << 16) | ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_sha1su0(a64_buf_t *b, unsigned vd, unsigned vn, unsigned vm)
{
    a64_emit(b, 0x5E003000U | ((vm & 31U) << 16) | ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_sha1h(a64_buf_t *b, unsigned vd, unsigned vn)
{
    a64_emit(b, 0x5E280800U | ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_sha1su1(a64_buf_t *b, unsigned vd, unsigned vn)
{
    a64_emit(b, 0x5E281800U | ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_sha256h(a64_buf_t *b, unsigned vd, unsigned vn, unsigned vm)
{
    a64_emit(b, 0x5E004000U | ((vm & 31U) << 16) | ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_sha256h2(a64_buf_t *b, unsigned vd, unsigned vn, unsigned vm)
{
    a64_emit(b, 0x5E005000U | ((vm & 31U) << 16) | ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_sha256su0(a64_buf_t *b, unsigned vd, unsigned vn)
{
    a64_emit(b, 0x5E282800U | ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_sha256su1(a64_buf_t *b, unsigned vd, unsigned vn, unsigned vm)
{
    a64_emit(b, 0x5E006000U | ((vm & 31U) << 16) | ((vn & 31U) << 5) | (vd & 31U));
}

/* Exclusives & barriers */
void a64_emit_ldxr_b(a64_buf_t *b, unsigned rt_w, unsigned rn_x)
{
    if (b->vfp_slot_dirty != 0U) {
        a64_vfp_cache_flush(b, false);
    }
    a64_emit(b, 0x085F7C00U | ((rn_x & 31U) << 5) | (rt_w & 31U));
}

void a64_emit_stxr_b(a64_buf_t *b, unsigned rs_w, unsigned rt_w, unsigned rn_x)
{
    a64_emit(b, 0x08007C00U | ((rs_w & 31U) << 16) | ((rn_x & 31U) << 5) | (rt_w & 31U));
}

void a64_emit_ldxr_h(a64_buf_t *b, unsigned rt_w, unsigned rn_x)
{
    if (b->vfp_slot_dirty != 0U) {
        a64_vfp_cache_flush(b, false);
    }
    a64_emit(b, 0x485F7C00U | ((rn_x & 31U) << 5) | (rt_w & 31U));
}

void a64_emit_stxr_h(a64_buf_t *b, unsigned rs_w, unsigned rt_w, unsigned rn_x)
{
    a64_emit(b, 0x48007C00U | ((rs_w & 31U) << 16) | ((rn_x & 31U) << 5) | (rt_w & 31U));
}

void a64_emit_ldxr_w(a64_buf_t *b, unsigned rt_w, unsigned rn_x)
{
    if (b->vfp_slot_dirty != 0U) {
        a64_vfp_cache_flush(b, false);
    }
    a64_emit(b, 0x885F7C00U | ((rn_x & 31U) << 5) | (rt_w & 31U));
}

void a64_emit_stxr_w(a64_buf_t *b, unsigned rs_w, unsigned rt_w, unsigned rn_x)
{
    a64_emit(b, 0x88007C00U | ((rs_w & 31U) << 16) | ((rn_x & 31U) << 5) | (rt_w & 31U));
}

void a64_emit_ldxr_x(a64_buf_t *b, unsigned rt_x, unsigned rn_x)
{
    if (b->vfp_slot_dirty != 0U) {
        a64_vfp_cache_flush(b, false);
    }
    a64_emit(b, 0xC85F7C00U | ((rn_x & 31U) << 5) | (rt_x & 31U));
}

void a64_emit_stxr_x(a64_buf_t *b, unsigned rs_w, unsigned rt_x, unsigned rn_x)
{
    a64_emit(b, 0xC8007C00U | ((rs_w & 31U) << 16) | ((rn_x & 31U) << 5) | (rt_x & 31U));
}

void a64_emit_clrex(a64_buf_t *b)
{
    a64_emit(b, 0xD5033F5FU);
}

void a64_emit_dmb_ish(a64_buf_t *b)
{
    a64_emit(b, 0xD5033BBFU);
}

void a64_emit_dsb_ish(a64_buf_t *b)
{
    a64_emit(b, 0xD5033B9FU);
}

void a64_emit_isb(a64_buf_t *b)
{
    a64_emit(b, 0xD5033FDFU);
}

size_t a64_emit_b_cond_placeholder(a64_buf_t *b, unsigned cond)
{
    if (!b->self_loop_vfp_active && b->vfp_slot_dirty != 0U) {
        a64_vfp_cache_flush(b, false);
    }
    b->cond_branch_depth++;
    size_t idx = b->count;
    a64_emit(b, 0x54000000U | (cond & 15U));
    return idx;
}

void a64_patch_b_cond(a64_buf_t *b, size_t patch_idx, size_t target_idx)
{
    if (!b->self_loop_vfp_active && b->vfp_slot_dirty != 0U) {
        a64_vfp_cache_flush(b, false);
    }
    if (b->cond_branch_depth > 0U) {
        b->cond_branch_depth--;
    }
    if (patch_idx >= b->count) {
        return;
    }
    int32_t disp = (int32_t)target_idx - (int32_t)patch_idx;
    uint32_t cond = b->words[patch_idx] & 15U;
    b->words[patch_idx] = 0x54000000U | (((uint32_t)disp & 0x7FFFFU) << 5) | cond;
}

size_t a64_emit_cbz_w_placeholder(a64_buf_t *b, bool is_cbnz, unsigned rt)
{
    if (!b->self_loop_vfp_active && b->vfp_slot_dirty != 0U) {
        a64_vfp_cache_flush(b, false);
    }
    b->cond_branch_depth++;
    size_t idx = b->count;
    uint32_t base = is_cbnz ? 0x35000000U : 0x34000000U;
    a64_emit(b, base | (rt & 31U));
    return idx;
}

void a64_patch_cbz_w(a64_buf_t *b, size_t patch_idx, size_t target_idx)
{
    if (!b->self_loop_vfp_active && b->vfp_slot_dirty != 0U) {
        a64_vfp_cache_flush(b, false);
    }
    if (b->cond_branch_depth > 0U) {
        b->cond_branch_depth--;
    }
    if (patch_idx >= b->count) {
        return;
    }
    int32_t disp = (int32_t)target_idx - (int32_t)patch_idx;
    uint32_t op_rt = b->words[patch_idx] & 0xFF00001FU;
    b->words[patch_idx] = op_rt | (((uint32_t)disp & 0x7FFFFU) << 5);
}

void a64_emit_ret(a64_buf_t *b)
{
    vfp_emit_exit_flush(b);
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
    /* ldr x16, [x27, #80] (cur_entry->ret_key | (ret_epoch << 32)) */
    a64_emit(b, 0xF9402B70U);
    /* ldr x22, [x27, #96] (cur_entry->ret_entry) */
    a64_emit(b, 0xF9403376U);
    /* cbz x22, .Lskip_push (+9 words) */
    a64_emit(b, 0xB4000016U | (9U << 5));
    /* ldr w21, [x19, #424] (w21 = ctx->ras_top, uimm = 424/4 = 106) */
    a64_emit(b, 0xB9400000U | (106U << 10) | (A64_XCTX << 5) | 21U);
    /* add w22, w21, #1 */
    a64_emit(b, 0x110006B6U);
    /* and w22, w22, #0x3f */
    a64_emit(b, 0x120016D6U);
    /* str w22, [x19, #424] (ctx->ras_top = (ras_top + 1) & 63) */
    a64_emit(b, 0xB9000000U | (106U << 10) | (A64_XCTX << 5) | 22U);
    /* add x21, x19, x21, lsl #5 (x21 = ctx + ras_top * 32) */
    a64_emit(b, 0x8B151675U);
    /* str x16, [x21, #432] (ras[top].key|epoch, uimm = 432/8 = 54) */
    a64_emit(b, 0xF9000000U | (54U << 10) | (21U << 5) | 16U);
    /* ldp x16, x22, [x27, #88] (cur_entry->ret_code, cur_entry->ret_entry) */
    a64_emit(b, 0xA945DB70U);
    /* stp x16, x22, [x21, #440] (ras[top].host_code, ras[top].entry, imm7 = 440/8 = 55) */
    a64_emit(b, 0xA9000000U | (1U << 31) | (55U << 15) | (22U << 10) | (21U << 5) | 16U);
}

static void emit_chain_jump_tail(a64_buf_t *b)
{
    /* mov x27, x23 (cur_entry = next_entry) */
    a64_emit(b, 0xAA1703FBU);
    /* add x24, x24, #1 (chain_hits_delta++) */
    a64_emit(b, 0x91000718U);
    /* br x17 */
    a64_emit(b, 0xD61F0220U);
}

void a64_emit_exit_branch(a64_buf_t *b, uint32_t target_pc)
{
    if (target_pc == b->guest_start_pc && b->guest_start_pc != 0U && !b->is_call) {
        b->has_self_loop = true;
        if (!b->self_loop_vfp_active) {
            a64_vfp_cache_flush(b, false);
        }
        emit_exit_sig_check(b, 7U);
        /* [3] ldar w21, [x28] (cur_epoch = g_cache_epoch) */
        a64_emit(b, 0x88DFFC00U | (28U << 5) | 21U);
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
        /* [9] .Lbail: flush VFP if self_loop_vfp_active, then return to trampoline */
        if (b->self_loop_vfp_active) {
            vfp_emit_exit_flush(b);
        }
        a64_emit_mov_pc32(b, A64_TMP0, target_pc);
        a64_emit_movz_w(b, A64_TMP1, A32_EXIT_BRANCH, 0U);
        a64_emit(b, 0xD65F03C0U); /* ret */
        return;
    }

    vfp_emit_exit_flush(b);

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
        uint32_t call_extra = b->is_call ? 11U : 0U;
        emit_exit_sig_check(b, 10U + call_extra);
        /* ldr x23, [x27, #(152 + s*8)] (cur_entry->slot_entry[s], uimm = 19 + s) */
        a64_emit(b, 0xF9400000U | (((19U + s) & 0xFFFU) << 10) | (27U << 5) | 23U);
        /* cbz x23, .Lbail */
        a64_emit(b, 0xB4000017U | ((8U + call_extra) << 5));
        /* ldar w21, [x28] (cur_epoch = g_cache_epoch) */
        a64_emit(b, 0x88DFFC00U | (28U << 5) | 21U);
        /* ldp x22, x17, [x23, #0] (x22 = key|epoch, x17 = host_code) */
        a64_emit(b, 0xA9400000U | (1U << 31) | (17U << 10) | (23U << 5) | 22U);
        /* eor x21, x21, x22, lsr #32 */
        a64_emit(b, 0xCA5682B5U);
        /* cbnz x21, .Lbail */
        a64_emit(b, 0xB5000015U | ((4U + call_extra) << 5));
        if (b->is_call) {
            emit_ras_push_inline(b);
        }
        emit_chain_jump_tail(b);
    }

    /* .Lbail */
    a64_emit_mov_pc32(b, A64_TMP0, target_pc);
    a64_emit_movz_w(b, A64_TMP1, A32_EXIT_BRANCH, 0U);
    a64_emit(b, 0xD65F03C0U); /* ret */
}

void a64_emit_exit_branch_imm(a64_buf_t *b, uint32_t target_pc, bool interwork)
{
    if (!interwork) {
        a64_emit_exit_branch(b, target_pc);
        return;
    }
    a64_emit_mov_pc32(b, A64_TMP0, target_pc);
    a64_emit_movz_w(b, A64_TMP1, A32_EXIT_INTERWORK, 0U);
    a64_emit_ret(b);
}

void a64_emit_exit_branch_reg(a64_buf_t *b, unsigned target_reg, bool interwork)
{
    if (b->lr_written_from_pc) {
        b->is_call = true;
    }
    vfp_emit_exit_flush(b);
    a64_emit_mov_w(b, A64_TMP0, target_reg);

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

    /* ldar w21, [x28] */
    a64_emit(b, 0x88DFFC00U | (28U << 5) | 21U);
    /* cbz w21, .Lbail */
    size_t patch_ep0 = b->count;
    a64_emit(b, 0x34000015U);

    /* and w16, w15, w25 */
    a64_emit(b, 0x0A1901F0U);
    /* orr w16, w16, w26 */
    a64_emit(b, 0x2A1A0210U);
    /* orr x16, x16, x21, lsl #32 (x16 = next_key | (cur_epoch << 32)) */
    a64_emit(b, 0xAA158210U);

    size_t patch_ras_stale = 0U;
    if (!b->is_call) {
        /* Inline RAS Pop */
        /* ldr w17, [x19, #424] (w17 = ctx->ras_top) */
        a64_emit(b, 0xB9400000U | (106U << 10) | (A64_XCTX << 5) | 17U);
        /* sub w17, w17, #1 */
        a64_emit(b, 0x51000631U);
        /* and w17, w17, #0x3f */
        a64_emit(b, 0x12001631U);
        /* add x22, x19, x17, lsl #5 */
        a64_emit(b, 0x8B111676U);
        /* ldr x23, [x22, #432] */
        a64_emit(b, 0xF9400000U | (54U << 10) | (22U << 5) | 23U);
        /* eor x23, x23, x16 */
        a64_emit(b, 0xCA1002F7U);
        /* cbnz x23, .Ltry_ibtc */
        size_t patch_ras_miss = b->count;
        a64_emit(b, 0xB5000017U);
        /* str w17, [x19, #424] */
        a64_emit(b, 0xB9000000U | (106U << 10) | (A64_XCTX << 5) | 17U);
        /* str xzr, [x22, #432] */
        a64_emit(b, 0xF9000000U | (54U << 10) | (22U << 5) | 31U);
        /* ldr x23, [x22, #448] */
        a64_emit(b, 0xF9400000U | (56U << 10) | (22U << 5) | 23U);
        /* cbz x23, .Ltry_ibtc */
        size_t patch_ras_null = b->count;
        a64_emit(b, 0xB4000017U);
        /* ldp x21, x17, [x23, #0] */
        a64_emit(b, 0xA9400000U | (1U << 31) | (17U << 10) | (23U << 5) | 21U);
        /* eor x21, x21, x16 */
        a64_emit(b, 0xCA1002B5U);
        /* cbnz x21, .Lbail */
        patch_ras_stale = b->count;
        a64_emit(b, 0xB5000015U);
        emit_chain_jump_tail(b);

        /* Patch patch_ras_miss and patch_ras_null to here (.Ltry_ibtc) */
        size_t try_ibtc_idx = b->count;
        b->words[patch_ras_miss] |= (((uint32_t)(try_ibtc_idx - patch_ras_miss) & 0x7FFFFU) << 5);
        b->words[patch_ras_null] |= (((uint32_t)(try_ibtc_idx - patch_ras_null) & 0x7FFFFU) << 5);
    }

    /* .Ltry_ibtc: */
    /* eor w17, w26, w16, lsr #1 */
    a64_emit(b, 0x4A500751U);
    /* and w17, w17, #0x3ff */
    a64_emit(b, 0x12002631U);
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
    emit_chain_jump_tail(b);

    /* .Lbail: */
    size_t bail_idx = b->count;
    b->words[patch_sig] |= (((uint32_t)(bail_idx - patch_sig) & 0x7FFFFU) << 5);
    if (interwork) {
        b->words[patch_iw] |= (((uint32_t)(bail_idx - patch_iw) & 0x7FFFFU) << 5);
    }
    b->words[patch_ep0] |= (((uint32_t)(bail_idx - patch_ep0) & 0x7FFFFU) << 5);
    if (!b->is_call) {
        b->words[patch_ras_stale] |= (((uint32_t)(bail_idx - patch_ras_stale) & 0x7FFFFU) << 5);
    }
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

/* Shared A32/T32 lowering helpers */
void a64_emit_parallel_addsub(a64_buf_t *b, unsigned kind, bool is_16, bool swap_rm,
                              unsigned sub_mask, unsigned rd, unsigned rn, unsigned rm)
{
    unsigned lanes = is_16 ? 2U : 4U;
    unsigned width = is_16 ? 16U : 8U;
    if (kind != A32_PADD_SH && kind != A32_PADD_UH) {
        a64_emit_mrs_nzcv(b, A64_TMP3);
    }
    if (kind == A32_PADD_S || kind == A32_PADD_U) {
        a64_emit_ldr_imm_w(b, A64_ADDR, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, cpsr));
        a64_emit_mov_imm32(b, A64_TMP0, 0x000F0000U);
        a64_emit_bic_reg_w(b, false, A64_ADDR, A64_ADDR, A64_TMP0, A64_SHIFT_LSL, 0U);
    } else if (kind == A32_PADD_Q) {
        a64_emit_mov_imm32(b, A64_ADDR, is_16 ? 0xFFFF8000U : 0xFFFFFF80U);
    } else if (kind == A32_PADD_UQ) {
        a64_emit_mov_imm32(b, A64_ADDR, is_16 ? 65535U : 255U);
    }
    a64_emit_movz_w(b, A64_TMP2, 0U, 0U);

    for (unsigned lane = 0U; lane < lanes; lane++) {
        unsigned lsb    = lane * width;
        unsigned rm_lsb = (swap_rm ? (1U - lane) : lane) * width;
        bool lane_sub   = ((sub_mask >> lane) & 1U) != 0U;

        if (kind < A32_PADD_U) {
            a64_emit_sbfm_w(b, A64_TMP0, rn, lsb, lsb + width - 1U);
            a64_emit_sbfm_w(b, A64_TMP1, rm, rm_lsb, rm_lsb + width - 1U);
        } else {
            a64_emit_ubfm_w(b, A64_TMP0, rn, lsb, lsb + width - 1U);
            a64_emit_ubfm_w(b, A64_TMP1, rm, rm_lsb, rm_lsb + width - 1U);
        }

        if (kind == A32_PADD_S) {
            if (lane_sub) {
                a64_emit_sub_reg_w(b, true, A64_TMP0, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
            } else {
                a64_emit_add_reg_w(b, true, A64_TMP0, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
            }
            a64_emit_bfm_w(b, A64_TMP2, A64_TMP0, (32U - lsb) & 31U, width - 1U);
            a64_emit_mov_imm32(b, A64_TMP1, is_16 ? (3U << (16U + lane * 2U)) : (1U << (16U + lane)));
            a64_emit_csel_w(b, A64_TMP1, A64_TMP1, A64_ZR, A64_COND_GE);
            a64_emit_orr_reg_w(b, A64_ADDR, A64_ADDR, A64_TMP1, A64_SHIFT_LSL, 0U);
        } else if (kind == A32_PADD_U) {
            if (lane_sub) {
                a64_emit_sub_reg_w(b, true, A64_TMP0, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
            } else {
                a64_emit_add_reg_w(b, false, A64_TMP0, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
                if (is_16) {
                    a64_emit_sub_imm_w(b, true, A64_ZR, A64_TMP0, 0x10U, 1U);
                } else {
                    a64_emit_sub_imm_w(b, true, A64_ZR, A64_TMP0, 256U, 0U);
                }
            }
            a64_emit_bfm_w(b, A64_TMP2, A64_TMP0, (32U - lsb) & 31U, width - 1U);
            a64_emit_mov_imm32(b, A64_TMP1, is_16 ? (3U << (16U + lane * 2U)) : (1U << (16U + lane)));
            a64_emit_csel_w(b, A64_TMP1, A64_TMP1, A64_ZR, A64_COND_CS);
            a64_emit_orr_reg_w(b, A64_ADDR, A64_ADDR, A64_TMP1, A64_SHIFT_LSL, 0U);
        } else if (kind == A32_PADD_SH || kind == A32_PADD_UH) {
            if (lane_sub) {
                a64_emit_sub_reg_w(b, false, A64_TMP1, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
            } else {
                a64_emit_add_reg_w(b, false, A64_TMP1, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
            }
            if (kind == A32_PADD_SH) {
                a64_emit_sbfm_w(b, A64_TMP1, A64_TMP1, 1U, 31U);
                a64_emit_ubfm_w(b, A64_TMP1, A64_TMP1, 0U, width - 1U);
            } else {
                a64_emit_ubfm_w(b, A64_TMP1, A64_TMP1, 1U, width);
            }
            a64_emit_bfm_w(b, A64_TMP2, A64_TMP1, (32U - lsb) & 31U, width - 1U);
        } else if (kind == A32_PADD_Q) {
            if (lane_sub) {
                a64_emit_sub_reg_w(b, false, A64_TMP0, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
            } else {
                a64_emit_add_reg_w(b, false, A64_TMP0, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
            }
            a64_emit_mov_imm32(b, A64_TMP1, is_16 ? 32767U : 127U);
            a64_emit_sub_reg_w(b, true, A64_ZR, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
            a64_emit_csel_w(b, A64_TMP0, A64_TMP1, A64_TMP0, A64_COND_GT);
            a64_emit_sub_reg_w(b, true, A64_ZR, A64_TMP0, A64_ADDR, A64_SHIFT_LSL, 0U);
            a64_emit_csel_w(b, A64_TMP0, A64_ADDR, A64_TMP0, A64_COND_LT);
            a64_emit_bfm_w(b, A64_TMP2, A64_TMP0, (32U - lsb) & 31U, width - 1U);
        } else if (kind == A32_PADD_UQ) {
            if (lane_sub) {
                a64_emit_sub_reg_w(b, true, A64_TMP0, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
                a64_emit_csel_w(b, A64_TMP0, A64_TMP0, A64_ZR, A64_COND_CS);
            } else {
                a64_emit_add_reg_w(b, false, A64_TMP0, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
                a64_emit_sub_reg_w(b, true, A64_ZR, A64_TMP0, A64_ADDR, A64_SHIFT_LSL, 0U);
                a64_emit_csel_w(b, A64_TMP0, A64_ADDR, A64_TMP0, A64_COND_HI);
            }
            a64_emit_bfm_w(b, A64_TMP2, A64_TMP0, (32U - lsb) & 31U, width - 1U);
        }
    }

    if (kind == A32_PADD_S || kind == A32_PADD_U) {
        a64_emit_str_imm_w(b, A64_ADDR, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, cpsr));
    }
    if (kind != A32_PADD_SH && kind != A32_PADD_UH) {
        a64_emit_msr_nzcv(b, A64_TMP3);
    }
    a64_emit_mov_w(b, rd, A64_TMP2);
}

static void emit_apply_sat_shift(a64_buf_t *b, unsigned dst, unsigned src, bool is_asr, unsigned shift_imm)
{
    if (is_asr) {
        unsigned amt = (shift_imm == 0U) ? 31U : shift_imm;
        a64_emit_orr_reg_w(b, dst, A64_ZR, src, A64_SHIFT_ASR, amt);
    } else if (shift_imm != 0U) {
        a64_emit_orr_reg_w(b, dst, A64_ZR, src, A64_SHIFT_LSL, shift_imm);
    } else {
        a64_emit_mov_w(b, dst, src);
    }
}

void a64_emit_ssat(a64_buf_t *b, unsigned rd, unsigned sat_bits, unsigned src_reg,
                   bool is_asr, unsigned shift_imm)
{
    emit_apply_sat_shift(b, A64_TMP0, src_reg, is_asr, shift_imm);
    if (sat_bits >= 32U) {
        a64_emit_mov_w(b, rd, A64_TMP0);
        return;
    }
    int32_t max_val = (int32_t)((1U << (sat_bits - 1U)) - 1U);
    int32_t min_val = -(int32_t)(1U << (sat_bits - 1U));
    a64_emit_mrs_nzcv(b, A64_TMP3);
    a64_emit_mov_imm32(b, A64_TMP1, (uint32_t)max_val);
    a64_emit_sub_reg_w(b, true, A64_ZR, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
    a64_emit_csel_w(b, A64_TMP1, A64_TMP1, A64_TMP0, A64_COND_GT);
    a64_emit_mov_imm32(b, A64_TMP2, (uint32_t)min_val);
    a64_emit_sub_reg_w(b, true, A64_ZR, A64_TMP1, A64_TMP2, A64_SHIFT_LSL, 0U);
    a64_emit_csel_w(b, rd, A64_TMP2, A64_TMP1, A64_COND_LT);
    a64_emit_sub_reg_w(b, true, A64_ZR, rd, A64_TMP0, A64_SHIFT_LSL, 0U);
    a64_emit_set_q_cond(b, A64_COND_NE);
    a64_emit_msr_nzcv(b, A64_TMP3);
}

void a64_emit_usat(a64_buf_t *b, unsigned rd, unsigned sat_bits, unsigned src_reg,
                   bool is_asr, unsigned shift_imm)
{
    emit_apply_sat_shift(b, A64_TMP0, src_reg, is_asr, shift_imm);
    uint32_t max_val = (sat_bits >= 31U) ? 0x7FFFFFFFU : ((1U << sat_bits) - 1U);
    a64_emit_mrs_nzcv(b, A64_TMP3);
    a64_emit_sub_imm_w(b, true, A64_ZR, A64_TMP0, 0U, 0U);
    a64_emit_csel_w(b, A64_TMP2, A64_ZR, A64_TMP0, A64_COND_LT);
    a64_emit_mov_imm32(b, A64_TMP1, max_val);
    a64_emit_sub_reg_w(b, true, A64_ZR, A64_TMP2, A64_TMP1, A64_SHIFT_LSL, 0U);
    a64_emit_csel_w(b, rd, A64_TMP1, A64_TMP2, A64_COND_HI);
    a64_emit_sub_reg_w(b, true, A64_ZR, rd, A64_TMP0, A64_SHIFT_LSL, 0U);
    a64_emit_set_q_cond(b, A64_COND_NE);
    a64_emit_msr_nzcv(b, A64_TMP3);
}

void a64_emit_sat16(a64_buf_t *b, bool is_signed, unsigned rd, unsigned sat_bits, unsigned src_reg)
{
    uint32_t max_val = is_signed ? ((1U << (sat_bits - 1U)) - 1U) : ((1U << sat_bits) - 1U);
    uint32_t min_val = is_signed ? (uint32_t)(-(int32_t)(1U << (sat_bits - 1U))) : 0U;
    a64_emit_mrs_nzcv(b, A64_TMP3);
    a64_emit_mov_w(b, A64_ADDR, src_reg);

    for (unsigned lane = 0U; lane < 2U; lane++) {
        unsigned dst_lane = (lane == 0U) ? A64_TMP0 : A64_TMP2;
        a64_emit_sbfm_w(b, dst_lane, A64_ADDR, lane * 16U, lane * 16U + 15U);
        if (is_signed) {
            a64_emit_mov_imm32(b, A64_TMP1, max_val);
            a64_emit_sub_reg_w(b, true, A64_ZR, dst_lane, A64_TMP1, A64_SHIFT_LSL, 0U);
            a64_emit_csel_w(b, dst_lane, A64_TMP1, dst_lane, A64_COND_GT);
            a64_emit_mov_imm32(b, A64_TMP1, min_val);
            a64_emit_sub_reg_w(b, true, A64_ZR, dst_lane, A64_TMP1, A64_SHIFT_LSL, 0U);
            a64_emit_csel_w(b, dst_lane, A64_TMP1, dst_lane, A64_COND_LT);
        } else {
            a64_emit_sub_imm_w(b, true, A64_ZR, dst_lane, 0U, 0U);
            a64_emit_csel_w(b, dst_lane, A64_ZR, dst_lane, A64_COND_LT);
            a64_emit_mov_imm32(b, A64_TMP1, max_val);
            a64_emit_sub_reg_w(b, true, A64_ZR, dst_lane, A64_TMP1, A64_SHIFT_LSL, 0U);
            a64_emit_csel_w(b, dst_lane, A64_TMP1, dst_lane, A64_COND_HI);
        }
    }

    a64_emit_bfm_w(b, A64_TMP0, A64_TMP2, 16U, 15U);
    a64_emit_sub_reg_w(b, true, A64_ZR, A64_TMP0, A64_ADDR, A64_SHIFT_LSL, 0U);
    a64_emit_set_q_cond(b, A64_COND_NE);
    a64_emit_msr_nzcv(b, A64_TMP3);
    a64_emit_mov_w(b, rd, A64_TMP0);
}

void a64_emit_qaddsub(a64_buf_t *b, unsigned rd, unsigned rm, unsigned rn, bool is_sub, bool is_double)
{
    a64_emit_mrs_nzcv(b, A64_TMP3);
    a64_emit_mov_w(b, A64_TMP0, rn);
    if (is_double) {
        a64_emit_add_reg_w(b, true, A64_TMP0, A64_TMP0, A64_TMP0, A64_SHIFT_LSL, 0U);
        a64_emit_movz_w(b, A64_TMP2, 0x8000U, 1U);
        a64_emit_eor_reg_w(b, A64_TMP2, A64_TMP2, A64_TMP0, A64_SHIFT_ASR, 31U);
        a64_emit_csel_w(b, A64_TMP0, A64_TMP2, A64_TMP0, A64_COND_VS);
        a64_emit_set_q_cond(b, A64_COND_VS);
    }
    if (is_sub) {
        a64_emit_sub_reg_w(b, true, A64_TMP0, rm, A64_TMP0, A64_SHIFT_LSL, 0U);
    } else {
        a64_emit_add_reg_w(b, true, A64_TMP0, rm, A64_TMP0, A64_SHIFT_LSL, 0U);
    }
    a64_emit_movz_w(b, A64_TMP2, 0x8000U, 1U);
    a64_emit_eor_reg_w(b, A64_TMP2, A64_TMP2, A64_TMP0, A64_SHIFT_ASR, 31U);
    a64_emit_csel_w(b, rd, A64_TMP2, A64_TMP0, A64_COND_VS);
    a64_emit_set_q_cond(b, A64_COND_VS);
    a64_emit_msr_nzcv(b, A64_TMP3);
}

void a64_emit_xt_add(a64_buf_t *b, bool is_signed, bool is_16, bool is_dual8,
                     unsigned rd, unsigned rn, unsigned rm, unsigned rot)
{
    a64_emit_orr_reg_w(b, A64_TMP0, A64_ZR, rm, A64_SHIFT_ROR, rot);
    if (is_dual8) {
        if (is_signed) {
            a64_emit_sbfm_w(b, A64_TMP1, A64_TMP0, 0U, 7U);
            a64_emit_sbfm_w(b, A64_TMP0, A64_TMP0, 16U, 23U);
        } else {
            a64_emit_ubfm_w(b, A64_TMP1, A64_TMP0, 0U, 7U);
            a64_emit_ubfm_w(b, A64_TMP0, A64_TMP0, 16U, 23U);
        }
        if (rn != 15U) {
            a64_emit_add_reg_w(b, false, A64_TMP1, A64_TMP1, rn, A64_SHIFT_LSL, 0U);
            a64_emit_add_reg_w(b, false, A64_TMP0, A64_TMP0, rn, A64_SHIFT_LSR, 16U);
        }
        a64_emit_bfm_w(b, A64_TMP1, A64_TMP0, 16U, 15U);
        a64_emit_mov_w(b, rd, A64_TMP1);
    } else {
        unsigned imms = is_16 ? 15U : 7U;
        if (is_signed) {
            a64_emit_sbfm_w(b, A64_TMP0, A64_TMP0, 0U, imms);
        } else {
            a64_emit_ubfm_w(b, A64_TMP0, A64_TMP0, 0U, imms);
        }
        if (rn != 15U) {
            a64_emit_add_reg_w(b, false, rd, rn, A64_TMP0, A64_SHIFT_LSL, 0U);
        } else {
            a64_emit_mov_w(b, rd, A64_TMP0);
        }
    }
}

void a64_emit_pkh(a64_buf_t *b, bool is_tb, unsigned rd, unsigned rn, unsigned rm, unsigned shift_imm)
{
    if (!is_tb) {
        a64_emit_orr_reg_w(b, A64_TMP0, A64_ZR, rm, A64_SHIFT_LSL, shift_imm);
        a64_emit_bfm_w(b, A64_TMP0, rn, 0U, 15U);
        a64_emit_mov_w(b, rd, A64_TMP0);
    } else {
        unsigned amt = (shift_imm == 0U) ? 31U : shift_imm;
        a64_emit_orr_reg_w(b, A64_TMP1, A64_ZR, rm, A64_SHIFT_ASR, amt);
        a64_emit_mov_w(b, A64_TMP0, rn);
        a64_emit_bfm_w(b, A64_TMP0, A64_TMP1, 0U, 15U);
        a64_emit_mov_w(b, rd, A64_TMP0);
    }
}

void a64_emit_sel(a64_buf_t *b, unsigned rd, unsigned rn, unsigned rm)
{
    a64_emit_ldr_imm_w(b, A64_TMP0, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, cpsr));
    a64_emit_movz_w(b, A64_TMP3, 0U, 0U);
    for (unsigned byte = 0U; byte < 4U; byte++) {
        a64_emit_sbfm_w(b, A64_TMP1, A64_TMP0, 16U + byte, 16U + byte);
        a64_emit_ubfm_w(b, A64_TMP2, rn, byte * 8U, byte * 8U + 7U);
        a64_emit_ubfm_w(b, A64_ADDR, rm, byte * 8U, byte * 8U + 7U);
        a64_emit_and_reg_w(b, false, A64_TMP2, A64_TMP2, A64_TMP1, A64_SHIFT_LSL, 0U);
        a64_emit_bic_reg_w(b, false, A64_ADDR, A64_ADDR, A64_TMP1, A64_SHIFT_LSL, 0U);
        a64_emit_orr_reg_w(b, A64_TMP2, A64_TMP2, A64_ADDR, A64_SHIFT_LSL, 0U);
        a64_emit_bfm_w(b, A64_TMP3, A64_TMP2, (32U - byte * 8U) & 31U, 7U);
    }
    a64_emit_mov_w(b, rd, A64_TMP3);
}

void a64_emit_usad8(a64_buf_t *b, unsigned rd, unsigned rn, unsigned rm, unsigned ra)
{
    a64_emit_mrs_nzcv(b, A64_TMP3);
    if (ra != 15U) {
        a64_emit_mov_w(b, A64_TMP2, ra);
    } else {
        a64_emit_movz_w(b, A64_TMP2, 0U, 0U);
    }
    for (unsigned i = 0U; i < 4U; i++) {
        a64_emit_ubfm_w(b, A64_TMP0, rn, i * 8U, i * 8U + 7U);
        a64_emit_ubfm_w(b, A64_TMP1, rm, i * 8U, i * 8U + 7U);
        a64_emit_sub_reg_w(b, true, A64_TMP0, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
        a64_emit_csneg_w(b, A64_TMP0, A64_TMP0, A64_TMP0, A64_COND_PL);
        a64_emit_add_reg_w(b, false, A64_TMP2, A64_TMP2, A64_TMP0, A64_SHIFT_LSL, 0U);
    }
    a64_emit_msr_nzcv(b, A64_TMP3);
    a64_emit_mov_w(b, rd, A64_TMP2);
}

void a64_emit_smla_xy(a64_buf_t *b, unsigned rd, unsigned rn, unsigned rm, unsigned ra,
                      bool n_top, bool m_top)
{
    a64_emit_sbfm_w(b, A64_TMP0, rn, n_top ? 16U : 0U, n_top ? 31U : 15U);
    a64_emit_sbfm_w(b, A64_TMP1, rm, m_top ? 16U : 0U, m_top ? 31U : 15U);
    if (ra == 15U) {
        a64_emit_madd_w(b, rd, A64_TMP0, A64_TMP1, A64_ZR);
    } else {
        a64_emit_mrs_nzcv(b, A64_TMP3);
        a64_emit_madd_w(b, A64_TMP2, A64_TMP0, A64_TMP1, A64_ZR);
        a64_emit_add_reg_w(b, true, rd, A64_TMP2, ra, A64_SHIFT_LSL, 0U);
        a64_emit_set_q_cond(b, A64_COND_VS);
        a64_emit_msr_nzcv(b, A64_TMP3);
    }
}

void a64_emit_smlaw_y(a64_buf_t *b, unsigned rd, unsigned rn, unsigned rm, unsigned ra, bool m_top)
{
    a64_emit_sbfm_w(b, A64_TMP1, rm, m_top ? 16U : 0U, m_top ? 31U : 15U);
    a64_emit_smaddl_x(b, A64_TMP0, rn, A64_TMP1, A64_ZR);
    if (ra == 15U) {
        a64_emit_ubfx_x_to_w(b, rd, A64_TMP0, 16U, 32U);
    } else {
        a64_emit_mrs_nzcv(b, A64_TMP3);
        a64_emit_ubfx_x_to_w(b, A64_TMP2, A64_TMP0, 16U, 32U);
        a64_emit_add_reg_w(b, true, rd, A64_TMP2, ra, A64_SHIFT_LSL, 0U);
        a64_emit_set_q_cond(b, A64_COND_VS);
        a64_emit_msr_nzcv(b, A64_TMP3);
    }
}

void a64_emit_smlal_xy(a64_buf_t *b, unsigned rd_lo, unsigned rd_hi, unsigned rn, unsigned rm,
                       bool n_top, bool m_top)
{
    a64_emit_mov_w(b, A64_TMP2, rd_lo);
    a64_emit_bfi_x(b, A64_TMP2, rd_hi, 32U, 32U);
    a64_emit_sbfm_w(b, A64_TMP0, rn, n_top ? 16U : 0U, n_top ? 31U : 15U);
    a64_emit_sbfm_w(b, A64_TMP1, rm, m_top ? 16U : 0U, m_top ? 31U : 15U);
    a64_emit_smaddl_x(b, A64_TMP2, A64_TMP0, A64_TMP1, A64_TMP2);
    a64_emit_mov_w(b, rd_lo, A64_TMP2);
    a64_emit_ubfx_x_to_w(b, rd_hi, A64_TMP2, 32U, 32U);
}

void a64_emit_smlad(a64_buf_t *b, unsigned rd, unsigned rn, unsigned rm, unsigned ra,
                    bool is_sub, bool m_swap)
{
    unsigned m1_lsb = m_swap ? 16U : 0U;
    unsigned m2_lsb = m_swap ? 0U : 16U;
    if (ra != 15U) {
        a64_emit(b, 0x93407C00U | ((ra & 31U) << 5) | A64_TMP2);
    } else {
        a64_emit_movz_w(b, A64_TMP2, 0U, 0U);
    }
    a64_emit_sbfm_w(b, A64_TMP0, rn, 0U, 15U);
    a64_emit_sbfm_w(b, A64_TMP1, rm, m1_lsb, m1_lsb + 15U);
    a64_emit_smaddl_x(b, A64_TMP2, A64_TMP0, A64_TMP1, A64_TMP2);
    a64_emit_sbfm_w(b, A64_TMP0, rn, 16U, 31U);
    a64_emit_sbfm_w(b, A64_TMP1, rm, m2_lsb, m2_lsb + 15U);
    if (is_sub) {
        a64_emit_smsubl_x(b, A64_TMP2, A64_TMP0, A64_TMP1, A64_TMP2);
    } else {
        a64_emit_smaddl_x(b, A64_TMP2, A64_TMP0, A64_TMP1, A64_TMP2);
    }
    a64_emit_mov_w(b, rd, A64_TMP2);
    if (!is_sub || ra != 15U) {
        a64_emit_mrs_nzcv(b, A64_TMP3);
        a64_emit(b, 0x93407C00U | ((rd & 31U) << 5) | A64_TMP0);
        a64_emit(b, 0xEB000000U | (A64_TMP2 << 16) | (A64_TMP0 << 5) | A64_ZR);
        a64_emit_set_q_cond(b, A64_COND_NE);
        a64_emit_msr_nzcv(b, A64_TMP3);
    }
}

void a64_emit_smlald(a64_buf_t *b, unsigned rd_lo, unsigned rd_hi, unsigned rn, unsigned rm,
                     bool is_sub, bool m_swap)
{
    unsigned m1_lsb = m_swap ? 16U : 0U;
    unsigned m2_lsb = m_swap ? 0U : 16U;
    a64_emit_mov_w(b, A64_TMP2, rd_lo);
    a64_emit_bfi_x(b, A64_TMP2, rd_hi, 32U, 32U);
    a64_emit_sbfm_w(b, A64_TMP0, rn, 0U, 15U);
    a64_emit_sbfm_w(b, A64_TMP1, rm, m1_lsb, m1_lsb + 15U);
    a64_emit_smaddl_x(b, A64_TMP2, A64_TMP0, A64_TMP1, A64_TMP2);
    a64_emit_sbfm_w(b, A64_TMP0, rn, 16U, 31U);
    a64_emit_sbfm_w(b, A64_TMP1, rm, m2_lsb, m2_lsb + 15U);
    if (is_sub) {
        a64_emit_smsubl_x(b, A64_TMP2, A64_TMP0, A64_TMP1, A64_TMP2);
    } else {
        a64_emit_smaddl_x(b, A64_TMP2, A64_TMP0, A64_TMP1, A64_TMP2);
    }
    a64_emit_mov_w(b, rd_lo, A64_TMP2);
    a64_emit_ubfx_x_to_w(b, rd_hi, A64_TMP2, 32U, 32U);
}

void a64_emit_smmla(a64_buf_t *b, unsigned rd, unsigned rn, unsigned rm, unsigned ra,
                    bool is_sub, bool round)
{
    if (round) {
        a64_emit_movz_w(b, A64_TMP1, 0x8000U, 1U);
    } else {
        a64_emit_movz_w(b, A64_TMP1, 0U, 0U);
    }
    if (is_sub || ra != 15U) {
        a64_emit_bfi_x(b, A64_TMP1, ra, 32U, 32U);
    }
    if (is_sub) {
        a64_emit_smsubl_x(b, A64_TMP0, rn, rm, A64_TMP1);
    } else {
        a64_emit_smaddl_x(b, A64_TMP0, rn, rm, A64_TMP1);
    }
    a64_emit_ubfx_x_to_w(b, rd, A64_TMP0, 32U, 32U);
}

void a64_emit_umaal(a64_buf_t *b, unsigned rd_lo, unsigned rd_hi, unsigned rn, unsigned rm)
{
    a64_emit_mov_w(b, A64_TMP1, rd_lo);
    a64_emit_umaddl_x(b, A64_TMP0, rn, rm, A64_TMP1);
    a64_emit_add_uxtw_x(b, A64_TMP0, A64_TMP0, rd_hi);
    a64_emit_mov_w(b, rd_lo, A64_TMP0);
    a64_emit_ubfx_x_to_w(b, rd_hi, A64_TMP0, 32U, 32U);
}

void a64_emit_mull(a64_buf_t *b, bool is_signed, bool accumulate, bool set_flags,
                   unsigned rd_lo, unsigned rd_hi, unsigned rn, unsigned rm)
{
    unsigned acc_x = A64_ZR;
    if (accumulate) {
        a64_emit_mov_w(b, A64_TMP1, rd_lo);
        a64_emit_bfi_x(b, A64_TMP1, rd_hi, 32U, 32U);
        acc_x = A64_TMP1;
    }
    if (is_signed) {
        a64_emit_smaddl_x(b, A64_TMP0, rn, rm, acc_x);
    } else {
        a64_emit_umaddl_x(b, A64_TMP0, rn, rm, acc_x);
    }
    a64_emit_mov_w(b, rd_lo, A64_TMP0);
    a64_emit_ubfx_x_to_w(b, rd_hi, A64_TMP0, 32U, 32U);
    if (set_flags) {
        a64_emit_mrs_nzcv(b, A64_TMP3);
        a64_emit(b, 0xEA000000U | (A64_TMP0 << 16) | (A64_TMP0 << 5) | A64_ZR);
        a64_emit_mrs_nzcv(b, A64_TMP2);
        a64_emit_ubfm_w(b, A64_TMP3, A64_TMP3, 28U, 29U);
        a64_emit_orr_reg_w(b, A64_TMP2, A64_TMP2, A64_TMP3, A64_SHIFT_LSL, 28U);
        a64_emit_msr_nzcv(b, A64_TMP2);
    }
}

void a64_emit_mrs_cpsr(a64_buf_t *b, unsigned rd)
{
    a64_emit_mrs_nzcv(b, A64_TMP0);
    a64_emit_ldr_imm_w(b, rd, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, cpsr));
    a64_emit_ubfm_w(b, rd, rd, 0U, 27U);
    a64_emit_movz_w(b, A64_TMP1, 0xF000U, 1U);
    a64_emit_and_reg_w(b, false, A64_TMP0, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
    a64_emit_orr_reg_w(b, rd, rd, A64_TMP0, A64_SHIFT_LSL, 0U);
}

void a64_emit_msr_cpsr_reg(a64_buf_t *b, unsigned src_reg, unsigned mask)
{
    if ((mask & 8U) != 0U) {
        a64_emit_movz_w(b, A64_TMP1, 0xF000U, 1U);
        a64_emit_and_reg_w(b, false, A64_TMP1, src_reg, A64_TMP1, A64_SHIFT_LSL, 0U);
        a64_emit_msr_nzcv(b, A64_TMP1);
        a64_emit_ldr_imm_w(b, A64_TMP1, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, cpsr));
        a64_emit_ubfm_w(b, A64_TMP2, src_reg, 27U, 27U);
        a64_emit_bfm_w(b, A64_TMP1, A64_TMP2, 5U, 0U);
        a64_emit_str_imm_w(b, A64_TMP1, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, cpsr));
    }
    if ((mask & 4U) != 0U) {
        a64_emit_ldr_imm_w(b, A64_TMP1, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, cpsr));
        a64_emit_ubfm_w(b, A64_TMP2, src_reg, 16U, 19U);
        a64_emit_bfm_w(b, A64_TMP1, A64_TMP2, 16U, 3U);
        a64_emit_str_imm_w(b, A64_TMP1, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, cpsr));
    }
}

bool a64_emit_cp15(a64_buf_t *b, uint32_t insn28)
{
    unsigned rt = (insn28 >> 12) & 0xFU;
    uint32_t masked = insn28 & 0x0FFF0FFFU;
    if (masked == 0x0E1D0F70U || masked == 0x0E1D0F50U) {
        a64_emit_ldr_imm_w(b, rt, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, tls_tp));
        return true;
    }
    if (masked == 0x0E0D0F50U) {
        a64_emit_str_imm_w(b, rt, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, tls_tp));
        return true;
    }
    if (masked == 0x0E070FBAU) {
        a64_emit_dmb_ish(b);
        return true;
    }
    if (masked == 0x0E070F9AU) {
        a64_emit_dsb_ish(b);
        return true;
    }
    if (masked == 0x0E070F95U) {
        a64_emit_isb(b);
        return true;
    }
    return false;
}

void a64_emit_ldm_stm(a64_buf_t *b, unsigned rn, uint32_t reg_mask,
                      bool l_bit, bool p_bit, bool u_bit, bool w_bit,
                      uint32_t pc_val, bool is_uncond, bool *out_ends_block)
{
    unsigned count = 0U;
    for (unsigned i = 0U; i < 16U; i++) {
        if (reg_mask & (1U << i)) {
            count++;
        }
    }
    uint32_t total_bytes = count * 4U;
    uint32_t start_offset = u_bit ? (p_bit ? 4U : 0U) : (total_bytes - (p_bit ? 0U : 4U));

    if (!u_bit) {
        a64_emit_sub_imm_w(b, false, A64_ADDR, rn, (uint16_t)start_offset, 0U);
    } else if (start_offset != 0U) {
        a64_emit_add_imm_w(b, false, A64_ADDR, rn, (uint16_t)start_offset, 0U);
    } else {
        a64_emit_mov_w(b, A64_ADDR, rn);
    }

    bool loads_pc = l_bit && ((reg_mask & (1U << 15)) != 0U);
    for (unsigned r = 0U; r < 16U; r++) {
        if ((reg_mask & (1U << r)) == 0U) {
            continue;
        }
        if (l_bit) {
            unsigned dst = (r == 15U) ? A64_TMP0 : ((r == rn) ? A64_TMP1 : r);
            a64_emit_guest_ldr_w(b, dst, A64_ADDR);
        } else {
            unsigned src = a32_load_reg(b, r, pc_val, A64_TMP1);
            a64_emit_guest_str_w(b, src, A64_ADDR);
        }
        a64_emit_add_imm_w(b, false, A64_ADDR, A64_ADDR, 4U, 0U);
    }

    if (l_bit && (reg_mask & (1U << rn)) != 0U && rn != 15U) {
        a64_emit_mov_w(b, rn, A64_TMP1);
    } else if (w_bit) {
        if (u_bit) {
            a64_emit_add_imm_w(b, false, rn, rn, (uint16_t)total_bytes, 0U);
        } else {
            a64_emit_sub_imm_w(b, false, rn, rn, (uint16_t)total_bytes, 0U);
        }
    }

    if (loads_pc) {
        a64_emit_exit_branch_reg(b, A64_TMP0, true);
        if (is_uncond && out_ends_block) {
            *out_ends_block = true;
        }
    }
}
