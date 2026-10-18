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
        if (shift_type == A64_SHIFT_LSL) {
            a64_emit_shift_reg_w(b, A64_TMP2, src_m, A64_TMP3, A64_SHIFT_LSL);
            a64_emit_ubfm_w(b, A64_TMP2, A64_TMP2, 31U, 31U);
            a64_emit_sub_reg_w(b, false, A64_TMP3, A64_ZR, A64_TMP3, A64_SHIFT_LSR, 5U);
            a64_emit_bic_reg_w(b, false, A64_TMP2, A64_TMP2, A64_TMP3, A64_SHIFT_LSR, 31U);
        } else if (shift_type == A64_SHIFT_LSR) {
            a64_emit_shift_reg_w(b, A64_TMP2, src_m, A64_TMP3, A64_SHIFT_LSR);
            a64_emit_ubfm_w(b, A64_TMP2, A64_TMP2, 0U, 0U);
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
    /* Save old C and V in w21[29:28], compute new N and Z via ANDS wzr, res_reg, res_reg */
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

static void vfp_emit_raw_ldr_s(a64_buf_t *b, unsigned vt, unsigned s_idx)
{
    uint32_t byte_offset = A32_CTX_VFP_OFFSET + (s_idx * 4U);
    a64_emit(b, 0xBD400000U | (((byte_offset / 4U) & 0xFFFU) << 10) | (A64_XCTX << 5) | (vt & 31U));
}

static void vfp_emit_raw_str_s(a64_buf_t *b, unsigned vt, unsigned s_idx)
{
    uint32_t byte_offset = A32_CTX_VFP_OFFSET + (s_idx * 4U);
    a64_emit(b, 0xBD000000U | (((byte_offset / 4U) & 0xFFFU) << 10) | (A64_XCTX << 5) | (vt & 31U));
}

static void vfp_emit_raw_ldr_d(a64_buf_t *b, unsigned vt, unsigned d_idx)
{
    uint32_t byte_offset = A32_CTX_VFP_OFFSET + (d_idx * 8U);
    a64_emit(b, 0xFD400000U | (((byte_offset / 8U) & 0xFFFU) << 10) | (A64_XCTX << 5) | (vt & 31U));
}

static void vfp_emit_raw_str_d(a64_buf_t *b, unsigned vt, unsigned d_idx)
{
    uint32_t byte_offset = A32_CTX_VFP_OFFSET + (d_idx * 8U);
    a64_emit(b, 0xFD000000U | (((byte_offset / 8U) & 0xFFFU) << 10) | (A64_XCTX << 5) | (vt & 31U));
}

static void vfp_emit_raw_ldr_q(a64_buf_t *b, unsigned vt, unsigned q_idx)
{
    uint32_t byte_offset = A32_CTX_VFP_OFFSET + (q_idx * 16U);
    a64_emit(b, 0x3DC00000U | (((byte_offset / 16U) & 0xFFFU) << 10) | (A64_XCTX << 5) | (vt & 31U));
}

static void vfp_emit_raw_str_q(a64_buf_t *b, unsigned vt, unsigned q_idx)
{
    uint32_t byte_offset = A32_CTX_VFP_OFFSET + (q_idx * 16U);
    a64_emit(b, 0x3D800000U | (((byte_offset / 16U) & 0xFFFU) << 10) | (A64_XCTX << 5) | (vt & 31U));
}

static void vfp_emit_mov_q(a64_buf_t *b, unsigned vd, unsigned vn)
{
    a64_emit(b, 0x4EA01C00U | ((vn & 31U) << 16) | ((vn & 31U) << 5) | (vd & 31U));
}

static void vfp_flush_slot(a64_buf_t *b, unsigned slot, bool invalidate)
{
    if ((b->vfp_slot_dirty & (1U << slot)) != 0U) {
        unsigned hreg = 8U + slot;
        if (b->vfp_slot_kind[slot] == A64_VFP_KIND_S) {
            vfp_emit_raw_str_s(b, hreg, b->vfp_slot_reg[slot]);
        } else if (b->vfp_slot_kind[slot] == A64_VFP_KIND_D) {
            vfp_emit_raw_str_d(b, hreg, b->vfp_slot_reg[slot]);
        } else if (b->vfp_slot_kind[slot] == A64_VFP_KIND_Q) {
            vfp_emit_raw_str_q(b, hreg, b->vfp_slot_reg[slot]);
        }
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
    if (k1 == k2) {
        return r1 == r2;
    }
    if (k1 > k2) {
        uint8_t tk = k1; k1 = k2; k2 = tk;
        uint8_t tr = r1; r1 = r2; r2 = tr;
    }
    if (k1 == A64_VFP_KIND_S && k2 == A64_VFP_KIND_D) {
        return (r1 >> 1) == r2;
    }
    if (k1 == A64_VFP_KIND_S && k2 == A64_VFP_KIND_Q) {
        return (r1 >> 2) == r2;
    }
    if (k1 == A64_VFP_KIND_D && k2 == A64_VFP_KIND_Q) {
        return (r1 >> 1) == r2;
    }
    return false;
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
        b->vfp_preheader_ineligible = true;
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
                uint8_t kind = b->vfp_slot_kind[i];
                uint8_t reg  = b->vfp_slot_reg[i];
                if (kind == A64_VFP_KIND_S) {
                    vfp_emit_raw_str_s(b, 8U + i, reg);
                } else if (kind == A64_VFP_KIND_D) {
                    vfp_emit_raw_str_d(b, 8U + i, reg);
                } else if (kind == A64_VFP_KIND_Q) {
                    vfp_emit_raw_str_q(b, 8U + i, reg);
                }
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
        if (kinds[i] == A64_VFP_KIND_S) {
            vfp_emit_raw_ldr_s(b, 8U + i, regs[i]);
        } else if (kinds[i] == A64_VFP_KIND_D) {
            vfp_emit_raw_ldr_d(b, 8U + i, regs[i]);
        } else if (kinds[i] == A64_VFP_KIND_Q) {
            vfp_emit_raw_ldr_q(b, 8U + i, regs[i]);
        }
    }
    b->loop_body_offset = b->count;
}

/* Guest memory accesses: [xG, wAddr, uxtw #0] */
void a64_emit_guest_ldr_w(a64_buf_t *b, unsigned rt, unsigned addr_w)
{
    if (!b->self_loop_vfp_active && b->vfp_slot_dirty != 0U) {
        a64_vfp_cache_flush(b, false);
    }
    a64_emit(b, 0xB8604800U | ((addr_w & 31U) << 16) | (A64_XG << 5) | (rt & 31U));
}

void a64_emit_guest_str_w(a64_buf_t *b, unsigned rt, unsigned addr_w)
{
    if (!b->self_loop_vfp_active && b->vfp_slot_dirty != 0U) {
        a64_vfp_cache_flush(b, false);
    }
    a64_emit(b, 0xB8204800U | ((addr_w & 31U) << 16) | (A64_XG << 5) | (rt & 31U));
}

void a64_emit_guest_ldrb_w(a64_buf_t *b, unsigned rt, unsigned addr_w)
{
    if (!b->self_loop_vfp_active && b->vfp_slot_dirty != 0U) {
        a64_vfp_cache_flush(b, false);
    }
    a64_emit(b, 0x38604800U | ((addr_w & 31U) << 16) | (A64_XG << 5) | (rt & 31U));
}

void a64_emit_guest_ldrsb_w(a64_buf_t *b, unsigned rt, unsigned addr_w)
{
    if (!b->self_loop_vfp_active && b->vfp_slot_dirty != 0U) {
        a64_vfp_cache_flush(b, false);
    }
    a64_emit(b, 0x38A04800U | ((addr_w & 31U) << 16) | (A64_XG << 5) | (rt & 31U));
}

void a64_emit_guest_strb_w(a64_buf_t *b, unsigned rt, unsigned addr_w)
{
    if (!b->self_loop_vfp_active && b->vfp_slot_dirty != 0U) {
        a64_vfp_cache_flush(b, false);
    }
    a64_emit(b, 0x38204800U | ((addr_w & 31U) << 16) | (A64_XG << 5) | (rt & 31U));
}

void a64_emit_guest_ldrh_w(a64_buf_t *b, unsigned rt, unsigned addr_w)
{
    if (!b->self_loop_vfp_active && b->vfp_slot_dirty != 0U) {
        a64_vfp_cache_flush(b, false);
    }
    a64_emit(b, 0x78604800U | ((addr_w & 31U) << 16) | (A64_XG << 5) | (rt & 31U));
}

void a64_emit_guest_ldrsh_w(a64_buf_t *b, unsigned rt, unsigned addr_w)
{
    if (!b->self_loop_vfp_active && b->vfp_slot_dirty != 0U) {
        a64_vfp_cache_flush(b, false);
    }
    a64_emit(b, 0x78A04800U | ((addr_w & 31U) << 16) | (A64_XG << 5) | (rt & 31U));
}

void a64_emit_guest_strh_w(a64_buf_t *b, unsigned rt, unsigned addr_w)
{
    if (!b->self_loop_vfp_active && b->vfp_slot_dirty != 0U) {
        a64_vfp_cache_flush(b, false);
    }
    a64_emit(b, 0x78204800U | ((addr_w & 31U) << 16) | (A64_XG << 5) | (rt & 31U));
}

/* Host unsigned-offset loads/stores */
void a64_emit_ldrb_imm_w(a64_buf_t *b, unsigned rt, unsigned rn_x, uint32_t byte_offset)
{
    if (rn_x == A64_XCTX && byte_offset >= A32_CTX_VFP_OFFSET && byte_offset < A32_CTX_VFP_OFFSET + 256U) {
        b->vfp_preheader_ineligible = true;
        a64_vfp_cache_flush(b, false);
    }
    uint32_t uimm12 = byte_offset & 0xFFFU;
    a64_emit(b, 0x39400000U | (uimm12 << 10) | ((rn_x & 31U) << 5) | (rt & 31U));
}

void a64_emit_strb_imm_w(a64_buf_t *b, unsigned rt, unsigned rn_x, uint32_t byte_offset)
{
    if (rn_x == A64_XCTX && byte_offset >= A32_CTX_VFP_OFFSET && byte_offset < A32_CTX_VFP_OFFSET + 256U) {
        b->vfp_preheader_ineligible = true;
        a64_vfp_cache_flush(b, true);
    }
    uint32_t uimm12 = byte_offset & 0xFFFU;
    a64_emit(b, 0x39000000U | (uimm12 << 10) | ((rn_x & 31U) << 5) | (rt & 31U));
}

void a64_emit_ldrh_imm_w(a64_buf_t *b, unsigned rt, unsigned rn_x, uint32_t byte_offset)
{
    if (rn_x == A64_XCTX && byte_offset >= A32_CTX_VFP_OFFSET && byte_offset < A32_CTX_VFP_OFFSET + 256U) {
        b->vfp_preheader_ineligible = true;
        a64_vfp_cache_flush(b, false);
    }
    uint32_t uimm12 = (byte_offset >> 1) & 0xFFFU;
    a64_emit(b, 0x79400000U | (uimm12 << 10) | ((rn_x & 31U) << 5) | (rt & 31U));
}

void a64_emit_strh_imm_w(a64_buf_t *b, unsigned rt, unsigned rn_x, uint32_t byte_offset)
{
    if (rn_x == A64_XCTX && byte_offset >= A32_CTX_VFP_OFFSET && byte_offset < A32_CTX_VFP_OFFSET + 256U) {
        b->vfp_preheader_ineligible = true;
        a64_vfp_cache_flush(b, true);
    }
    uint32_t uimm12 = (byte_offset >> 1) & 0xFFFU;
    a64_emit(b, 0x79000000U | (uimm12 << 10) | ((rn_x & 31U) << 5) | (rt & 31U));
}

void a64_emit_ldr_imm_w(a64_buf_t *b, unsigned rt, unsigned rn_x, uint32_t byte_offset)
{
    if (rn_x == A64_XCTX && byte_offset >= A32_CTX_VFP_OFFSET && byte_offset < A32_CTX_VFP_OFFSET + 256U) {
        b->vfp_preheader_ineligible = true;
        a64_vfp_cache_flush(b, false);
    }
    uint32_t uimm12 = (byte_offset >> 2) & 0xFFFU;
    a64_emit(b, 0xB9400000U | (uimm12 << 10) | ((rn_x & 31U) << 5) | (rt & 31U));
}

void a64_emit_str_imm_w(a64_buf_t *b, unsigned rt, unsigned rn_x, uint32_t byte_offset)
{
    if (rn_x == A64_XCTX && byte_offset >= A32_CTX_VFP_OFFSET && byte_offset < A32_CTX_VFP_OFFSET + 256U) {
        b->vfp_preheader_ineligible = true;
        a64_vfp_cache_flush(b, true);
    }
    uint32_t uimm12 = (byte_offset >> 2) & 0xFFFU;
    a64_emit(b, 0xB9000000U | (uimm12 << 10) | ((rn_x & 31U) << 5) | (rt & 31U));
}

/* VFPv4-D32 / NEON context & guest memory loads/stores (M4) */
void a64_emit_ldr_s_ctx(a64_buf_t *b, unsigned vt, unsigned s_idx)
{
    vfp_record_preheader_access(b, A64_VFP_KIND_S, (uint8_t)s_idx, false);
    if (b->self_loop_vfp_active) {
        for (unsigned i = 0U; i < A64_VFP_CACHE_SLOTS; i++) {
            if (b->vfp_slot_kind[i] == A64_VFP_KIND_S && b->vfp_slot_reg[i] == s_idx) {
                a64_emit_fmov_s(b, vt, 8U + i);
                return;
            }
        }
    }
    unsigned d_alias = s_idx >> 1;
    unsigned q_alias = s_idx >> 2;
    for (unsigned i = 0U; i < A64_VFP_CACHE_SLOTS; i++) {
        if ((b->vfp_slot_kind[i] == A64_VFP_KIND_D && b->vfp_slot_reg[i] == d_alias) ||
            (b->vfp_slot_kind[i] == A64_VFP_KIND_Q && b->vfp_slot_reg[i] == q_alias)) {
            vfp_flush_slot(b, i, true);
        }
    }
    for (unsigned i = 0U; i < A64_VFP_CACHE_SLOTS; i++) {
        if (b->vfp_slot_kind[i] == A64_VFP_KIND_S && b->vfp_slot_reg[i] == s_idx) {
            a64_emit_fmov_s(b, vt, 8U + i);
            return;
        }
    }
    vfp_emit_raw_ldr_s(b, vt, s_idx);
    if (b->cond_branch_depth == 0U) {
        unsigned slot = vfp_alloc_slot(b, A64_VFP_KIND_S, (uint8_t)s_idx);
        a64_emit_fmov_s(b, 8U + slot, vt);
    }
}

void a64_emit_str_s_ctx(a64_buf_t *b, unsigned vt, unsigned s_idx)
{
    vfp_record_preheader_access(b, A64_VFP_KIND_S, (uint8_t)s_idx, true);
    if (b->self_loop_vfp_active) {
        for (unsigned i = 0U; i < A64_VFP_CACHE_SLOTS; i++) {
            if (b->vfp_slot_kind[i] == A64_VFP_KIND_S && b->vfp_slot_reg[i] == s_idx) {
                a64_emit_fmov_s(b, 8U + i, vt);
                b->vfp_slot_dirty |= (1U << i);
                return;
            }
        }
    }
    unsigned d_alias = s_idx >> 1;
    unsigned q_alias = s_idx >> 2;
    for (unsigned i = 0U; i < A64_VFP_CACHE_SLOTS; i++) {
        if ((b->vfp_slot_kind[i] == A64_VFP_KIND_D && b->vfp_slot_reg[i] == d_alias) ||
            (b->vfp_slot_kind[i] == A64_VFP_KIND_Q && b->vfp_slot_reg[i] == q_alias)) {
            vfp_flush_slot(b, i, true);
        }
    }
    if (b->cond_branch_depth == 0U) {
        for (unsigned i = 0U; i < A64_VFP_CACHE_SLOTS; i++) {
            if (b->vfp_slot_kind[i] == A64_VFP_KIND_S && b->vfp_slot_reg[i] == s_idx) {
                a64_emit_fmov_s(b, 8U + i, vt);
                b->vfp_slot_dirty |= (1U << i);
                return;
            }
        }
        unsigned slot = vfp_alloc_slot(b, A64_VFP_KIND_S, (uint8_t)s_idx);
        a64_emit_fmov_s(b, 8U + slot, vt);
        b->vfp_slot_dirty |= (1U << slot);
        return;
    }
    for (unsigned i = 0U; i < A64_VFP_CACHE_SLOTS; i++) {
        if (b->vfp_slot_kind[i] == A64_VFP_KIND_S && b->vfp_slot_reg[i] == s_idx) {
            b->vfp_slot_dirty &= ~(1U << i);
            b->vfp_slot_kind[i] = A64_VFP_KIND_NONE;
        }
    }
    vfp_emit_raw_str_s(b, vt, s_idx);
}

void a64_emit_ldr_d_ctx(a64_buf_t *b, unsigned vt, unsigned d_idx)
{
    vfp_record_preheader_access(b, A64_VFP_KIND_D, (uint8_t)d_idx, false);
    if (b->self_loop_vfp_active) {
        for (unsigned i = 0U; i < A64_VFP_CACHE_SLOTS; i++) {
            if (b->vfp_slot_kind[i] == A64_VFP_KIND_D && b->vfp_slot_reg[i] == d_idx) {
                a64_emit_fmov_d(b, vt, 8U + i);
                return;
            }
        }
    }
    unsigned s0 = d_idx * 2U;
    unsigned s1 = s0 + 1U;
    unsigned q_alias = d_idx >> 1;
    for (unsigned i = 0U; i < A64_VFP_CACHE_SLOTS; i++) {
        if (d_idx < 16U && b->vfp_slot_kind[i] == A64_VFP_KIND_S &&
            (b->vfp_slot_reg[i] == s0 || b->vfp_slot_reg[i] == s1)) {
            vfp_flush_slot(b, i, true);
        } else if (b->vfp_slot_kind[i] == A64_VFP_KIND_Q && b->vfp_slot_reg[i] == q_alias) {
            vfp_flush_slot(b, i, true);
        }
    }
    for (unsigned i = 0U; i < A64_VFP_CACHE_SLOTS; i++) {
        if (b->vfp_slot_kind[i] == A64_VFP_KIND_D && b->vfp_slot_reg[i] == d_idx) {
            a64_emit_fmov_d(b, vt, 8U + i);
            return;
        }
    }
    vfp_emit_raw_ldr_d(b, vt, d_idx);
    if (b->cond_branch_depth == 0U) {
        unsigned slot = vfp_alloc_slot(b, A64_VFP_KIND_D, (uint8_t)d_idx);
        a64_emit_fmov_d(b, 8U + slot, vt);
    }
}

void a64_emit_str_d_ctx(a64_buf_t *b, unsigned vt, unsigned d_idx)
{
    vfp_record_preheader_access(b, A64_VFP_KIND_D, (uint8_t)d_idx, true);
    if (b->self_loop_vfp_active) {
        for (unsigned i = 0U; i < A64_VFP_CACHE_SLOTS; i++) {
            if (b->vfp_slot_kind[i] == A64_VFP_KIND_D && b->vfp_slot_reg[i] == d_idx) {
                a64_emit_fmov_d(b, 8U + i, vt);
                b->vfp_slot_dirty |= (1U << i);
                return;
            }
        }
    }
    unsigned s0 = d_idx * 2U;
    unsigned s1 = s0 + 1U;
    unsigned q_alias = d_idx >> 1;
    for (unsigned i = 0U; i < A64_VFP_CACHE_SLOTS; i++) {
        if (b->vfp_slot_kind[i] == A64_VFP_KIND_Q && b->vfp_slot_reg[i] == q_alias) {
            vfp_flush_slot(b, i, true);
        } else if (d_idx < 16U && b->vfp_slot_kind[i] == A64_VFP_KIND_S &&
                   (b->vfp_slot_reg[i] == s0 || b->vfp_slot_reg[i] == s1)) {
            if (b->cond_branch_depth == 0U) {
                b->vfp_slot_dirty &= ~(1U << i);
                b->vfp_slot_kind[i] = A64_VFP_KIND_NONE;
            } else {
                vfp_flush_slot(b, i, true);
            }
        }
    }
    if (b->cond_branch_depth == 0U) {
        for (unsigned i = 0U; i < A64_VFP_CACHE_SLOTS; i++) {
            if (b->vfp_slot_kind[i] == A64_VFP_KIND_D && b->vfp_slot_reg[i] == d_idx) {
                a64_emit_fmov_d(b, 8U + i, vt);
                b->vfp_slot_dirty |= (1U << i);
                return;
            }
        }
        unsigned slot = vfp_alloc_slot(b, A64_VFP_KIND_D, (uint8_t)d_idx);
        a64_emit_fmov_d(b, 8U + slot, vt);
        b->vfp_slot_dirty |= (1U << slot);
        return;
    }
    for (unsigned i = 0U; i < A64_VFP_CACHE_SLOTS; i++) {
        if (b->vfp_slot_kind[i] == A64_VFP_KIND_D && b->vfp_slot_reg[i] == d_idx) {
            b->vfp_slot_dirty &= ~(1U << i);
            b->vfp_slot_kind[i] = A64_VFP_KIND_NONE;
        }
    }
    vfp_emit_raw_str_d(b, vt, d_idx);
}

void a64_emit_ldr_q_ctx(a64_buf_t *b, unsigned vt, unsigned q_idx)
{
    vfp_record_preheader_access(b, A64_VFP_KIND_Q, (uint8_t)q_idx, false);
    if (b->self_loop_vfp_active) {
        for (unsigned i = 0U; i < A64_VFP_CACHE_SLOTS; i++) {
            if (b->vfp_slot_kind[i] == A64_VFP_KIND_Q && b->vfp_slot_reg[i] == q_idx) {
                vfp_emit_mov_q(b, vt, 8U + i);
                return;
            }
        }
    }
    unsigned d0 = q_idx * 2U;
    unsigned d1 = d0 + 1U;
    unsigned s_base = q_idx * 4U;
    for (unsigned i = 0U; i < A64_VFP_CACHE_SLOTS; i++) {
        if (b->vfp_slot_kind[i] == A64_VFP_KIND_D &&
            (b->vfp_slot_reg[i] == d0 || b->vfp_slot_reg[i] == d1)) {
            vfp_flush_slot(b, i, true);
        } else if (q_idx < 8U && b->vfp_slot_kind[i] == A64_VFP_KIND_S &&
                   b->vfp_slot_reg[i] >= s_base && b->vfp_slot_reg[i] < s_base + 4U) {
            vfp_flush_slot(b, i, true);
        }
    }
    for (unsigned i = 0U; i < A64_VFP_CACHE_SLOTS; i++) {
        if (b->vfp_slot_kind[i] == A64_VFP_KIND_Q && b->vfp_slot_reg[i] == q_idx) {
            vfp_emit_mov_q(b, vt, 8U + i);
            return;
        }
    }
    vfp_emit_raw_ldr_q(b, vt, q_idx);
    if (b->cond_branch_depth == 0U) {
        unsigned slot = vfp_alloc_slot(b, A64_VFP_KIND_Q, (uint8_t)q_idx);
        vfp_emit_mov_q(b, 8U + slot, vt);
    }
}

void a64_emit_str_q_ctx(a64_buf_t *b, unsigned vt, unsigned q_idx)
{
    vfp_record_preheader_access(b, A64_VFP_KIND_Q, (uint8_t)q_idx, true);
    if (b->self_loop_vfp_active) {
        for (unsigned i = 0U; i < A64_VFP_CACHE_SLOTS; i++) {
            if (b->vfp_slot_kind[i] == A64_VFP_KIND_Q && b->vfp_slot_reg[i] == q_idx) {
                vfp_emit_mov_q(b, 8U + i, vt);
                b->vfp_slot_dirty |= (1U << i);
                return;
            }
        }
    }
    unsigned d0 = q_idx * 2U;
    unsigned d1 = d0 + 1U;
    unsigned s_base = q_idx * 4U;
    for (unsigned i = 0U; i < A64_VFP_CACHE_SLOTS; i++) {
        if (b->vfp_slot_kind[i] == A64_VFP_KIND_D &&
            (b->vfp_slot_reg[i] == d0 || b->vfp_slot_reg[i] == d1)) {
            if (b->cond_branch_depth == 0U) {
                b->vfp_slot_dirty &= ~(1U << i);
                b->vfp_slot_kind[i] = A64_VFP_KIND_NONE;
            } else {
                vfp_flush_slot(b, i, true);
            }
        } else if (q_idx < 8U && b->vfp_slot_kind[i] == A64_VFP_KIND_S &&
                   b->vfp_slot_reg[i] >= s_base && b->vfp_slot_reg[i] < s_base + 4U) {
            if (b->cond_branch_depth == 0U) {
                b->vfp_slot_dirty &= ~(1U << i);
                b->vfp_slot_kind[i] = A64_VFP_KIND_NONE;
            } else {
                vfp_flush_slot(b, i, true);
            }
        }
    }
    if (b->cond_branch_depth == 0U) {
        for (unsigned i = 0U; i < A64_VFP_CACHE_SLOTS; i++) {
            if (b->vfp_slot_kind[i] == A64_VFP_KIND_Q && b->vfp_slot_reg[i] == q_idx) {
                vfp_emit_mov_q(b, 8U + i, vt);
                b->vfp_slot_dirty |= (1U << i);
                return;
            }
        }
        unsigned slot = vfp_alloc_slot(b, A64_VFP_KIND_Q, (uint8_t)q_idx);
        vfp_emit_mov_q(b, 8U + slot, vt);
        b->vfp_slot_dirty |= (1U << slot);
        return;
    }
    for (unsigned i = 0U; i < A64_VFP_CACHE_SLOTS; i++) {
        if (b->vfp_slot_kind[i] == A64_VFP_KIND_Q && b->vfp_slot_reg[i] == q_idx) {
            b->vfp_slot_dirty &= ~(1U << i);
            b->vfp_slot_kind[i] = A64_VFP_KIND_NONE;
        }
    }
    vfp_emit_raw_str_q(b, vt, q_idx);
}

void a64_emit_guest_ldr_s(a64_buf_t *b, unsigned vt, unsigned addr_w)
{
    if (!b->self_loop_vfp_active && b->vfp_slot_dirty != 0U) {
        a64_vfp_cache_flush(b, false);
    }
    a64_emit(b, 0xBC604800U | ((addr_w & 31U) << 16) | (A64_XG << 5) | (vt & 31U));
}

void a64_emit_guest_str_s(a64_buf_t *b, unsigned vt, unsigned addr_w)
{
    if (!b->self_loop_vfp_active && b->vfp_slot_dirty != 0U) {
        a64_vfp_cache_flush(b, false);
    }
    a64_emit(b, 0xBC204800U | ((addr_w & 31U) << 16) | (A64_XG << 5) | (vt & 31U));
}

void a64_emit_guest_ldr_d(a64_buf_t *b, unsigned vt, unsigned addr_w)
{
    if (!b->self_loop_vfp_active && b->vfp_slot_dirty != 0U) {
        a64_vfp_cache_flush(b, false);
    }
    a64_emit(b, 0xFC604800U | ((addr_w & 31U) << 16) | (A64_XG << 5) | (vt & 31U));
}

void a64_emit_guest_str_d(a64_buf_t *b, unsigned vt, unsigned addr_w)
{
    if (!b->self_loop_vfp_active && b->vfp_slot_dirty != 0U) {
        a64_vfp_cache_flush(b, false);
    }
    a64_emit(b, 0xFC204800U | ((addr_w & 31U) << 16) | (A64_XG << 5) | (vt & 31U));
}

/* Scalar FP instructions (M4) */
void a64_emit_fmov_w_to_s(a64_buf_t *b, unsigned vd, unsigned rn_w)
{
    a64_emit(b, 0x1E270000U | ((rn_w & 31U) << 5) | (vd & 31U));
}

void a64_emit_fmov_s_to_w(a64_buf_t *b, unsigned rd_w, unsigned vn)
{
    a64_emit(b, 0x1E260000U | ((vn & 31U) << 5) | (rd_w & 31U));
}

void a64_emit_fmov_s(a64_buf_t *b, unsigned vd, unsigned vn)
{
    a64_emit(b, 0x1E204000U | ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_fmov_d(a64_buf_t *b, unsigned vd, unsigned vn)
{
    a64_emit(b, 0x1E604000U | ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_fadd_s(a64_buf_t *b, unsigned vd, unsigned vn, unsigned vm)
{
    a64_emit(b, 0x1E202800U | ((vm & 31U) << 16) | ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_fadd_d(a64_buf_t *b, unsigned vd, unsigned vn, unsigned vm)
{
    a64_emit(b, 0x1E602800U | ((vm & 31U) << 16) | ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_fsub_s(a64_buf_t *b, unsigned vd, unsigned vn, unsigned vm)
{
    a64_emit(b, 0x1E203800U | ((vm & 31U) << 16) | ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_fsub_d(a64_buf_t *b, unsigned vd, unsigned vn, unsigned vm)
{
    a64_emit(b, 0x1E603800U | ((vm & 31U) << 16) | ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_fmul_s(a64_buf_t *b, unsigned vd, unsigned vn, unsigned vm)
{
    a64_emit(b, 0x1E200800U | ((vm & 31U) << 16) | ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_fmul_d(a64_buf_t *b, unsigned vd, unsigned vn, unsigned vm)
{
    a64_emit(b, 0x1E600800U | ((vm & 31U) << 16) | ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_fdiv_s(a64_buf_t *b, unsigned vd, unsigned vn, unsigned vm)
{
    a64_emit(b, 0x1E201800U | ((vm & 31U) << 16) | ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_fdiv_d(a64_buf_t *b, unsigned vd, unsigned vn, unsigned vm)
{
    a64_emit(b, 0x1E601800U | ((vm & 31U) << 16) | ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_fabs_s(a64_buf_t *b, unsigned vd, unsigned vn)
{
    a64_emit(b, 0x1E20C000U | ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_fabs_d(a64_buf_t *b, unsigned vd, unsigned vn)
{
    a64_emit(b, 0x1E60C000U | ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_fneg_s(a64_buf_t *b, unsigned vd, unsigned vn)
{
    a64_emit(b, 0x1E214000U | ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_fneg_d(a64_buf_t *b, unsigned vd, unsigned vn)
{
    a64_emit(b, 0x1E614000U | ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_fsqrt_s(a64_buf_t *b, unsigned vd, unsigned vn)
{
    a64_emit(b, 0x1E21C000U | ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_fsqrt_d(a64_buf_t *b, unsigned vd, unsigned vn)
{
    a64_emit(b, 0x1E61C000U | ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_fmadd_s(a64_buf_t *b, unsigned vd, unsigned vn, unsigned vm, unsigned va)
{
    a64_emit(b, 0x1F000000U | ((vm & 31U) << 16) | ((va & 31U) << 10) |
                    ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_fmadd_d(a64_buf_t *b, unsigned vd, unsigned vn, unsigned vm, unsigned va)
{
    a64_emit(b, 0x1F400000U | ((vm & 31U) << 16) | ((va & 31U) << 10) |
                    ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_fmsub_s(a64_buf_t *b, unsigned vd, unsigned vn, unsigned vm, unsigned va)
{
    a64_emit(b, 0x1F008000U | ((vm & 31U) << 16) | ((va & 31U) << 10) |
                    ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_fmsub_d(a64_buf_t *b, unsigned vd, unsigned vn, unsigned vm, unsigned va)
{
    a64_emit(b, 0x1F408000U | ((vm & 31U) << 16) | ((va & 31U) << 10) |
                    ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_fnmadd_s(a64_buf_t *b, unsigned vd, unsigned vn, unsigned vm, unsigned va)
{
    a64_emit(b, 0x1F200000U | ((vm & 31U) << 16) | ((va & 31U) << 10) |
                    ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_fnmadd_d(a64_buf_t *b, unsigned vd, unsigned vn, unsigned vm, unsigned va)
{
    a64_emit(b, 0x1F600000U | ((vm & 31U) << 16) | ((va & 31U) << 10) |
                    ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_fnmsub_s(a64_buf_t *b, unsigned vd, unsigned vn, unsigned vm, unsigned va)
{
    a64_emit(b, 0x1F208000U | ((vm & 31U) << 16) | ((va & 31U) << 10) |
                    ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_fnmsub_d(a64_buf_t *b, unsigned vd, unsigned vn, unsigned vm, unsigned va)
{
    a64_emit(b, 0x1F608000U | ((vm & 31U) << 16) | ((va & 31U) << 10) |
                    ((vn & 31U) << 5) | (vd & 31U));
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

void a64_emit_exit_branch(a64_buf_t *b, uint32_t target_pc)
{
    if (target_pc == b->guest_start_pc && b->guest_start_pc != 0U && !b->is_call) {
        b->has_self_loop = true;
        if (!b->self_loop_vfp_active) {
            a64_vfp_cache_flush(b, false);
        }
        /* [0] ldp x17, x23, [x19, #368] (pending_signals, flags|itstate) */
        a64_emit(b, 0xA9400000U | (1U << 31) | ((368U / 8U) << 15) | (23U << 10) | (A64_XCTX << 5) | 17U);
        /* [1] orr x17, x17, x23 */
        a64_emit(b, 0xAA170231U);
        /* [2] cbnz x17, .Lbail (+7 words) */
        a64_emit(b, 0xB5000011U | (7U << 5));
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
        if (!b->is_call) {
            /* [0] ldp x17, x23, [x19, #368] (pending_signals, flags|itstate) */
            a64_emit(b, 0xA9400000U | (1U << 31) | ((368U / 8U) << 15) | (23U << 10) | (A64_XCTX << 5) | 17U);
            /* [1] orr x17, x17, x23 */
            a64_emit(b, 0xAA170231U);
            /* [2] cbnz x17, .Lbail (+10 words) */
            a64_emit(b, 0xB5000011U | (10U << 5));
            /* [3] ldr x23, [x27, #(152 + s*8)] (cur_entry->slot_entry[s], uimm = 19 + s) */
            a64_emit(b, 0xF9400000U | (((19U + s) & 0xFFFU) << 10) | (27U << 5) | 23U);
            /* [4] cbz x23, .Lbail (+8 words) */
            a64_emit(b, 0xB4000017U | (8U << 5));
            /* [5] ldar w21, [x28] (cur_epoch = g_cache_epoch) */
            a64_emit(b, 0x88DFFC00U | (28U << 5) | 21U);
            /* [6] ldp x22, x17, [x23, #0] (x22 = key|epoch, x17 = host_code) */
            a64_emit(b, 0xA9400000U | (1U << 31) | (17U << 10) | (23U << 5) | 22U);
            /* [7] eor x21, x21, x22, lsr #32 */
            a64_emit(b, 0xCA5682B5U);
            /* [8] cbnz x21, .Lbail (+4 words) */
            a64_emit(b, 0xB5000015U | (4U << 5));
            /* [9] mov x27, x23 (cur_entry = next_entry) */
            a64_emit(b, 0xAA1703FBU);
            /* [10] add x24, x24, #1 (chain_hits_delta++) */
            a64_emit(b, 0x91000718U);
            /* [11] br x17 (jump directly to next block in g_code_cache!) */
            a64_emit(b, 0xD61F0220U);
        } else {
            /* [0] ldp x17, x23, [x19, #368] (pending_signals, flags|itstate) */
            a64_emit(b, 0xA9400000U | (1U << 31) | ((368U / 8U) << 15) | (23U << 10) | (A64_XCTX << 5) | 17U);
            /* [1] orr x17, x17, x23 */
            a64_emit(b, 0xAA170231U);
            /* [2] cbnz x17, .Lbail (+21 words) */
            a64_emit(b, 0xB5000011U | (21U << 5));
            /* [3] ldr x23, [x27, #(152 + s*8)] (cur_entry->slot_entry[s], uimm = 19 + s) */
            a64_emit(b, 0xF9400000U | (((19U + s) & 0xFFFU) << 10) | (27U << 5) | 23U);
            /* [4] cbz x23, .Lbail (+19 words) */
            a64_emit(b, 0xB4000017U | (19U << 5));
            /* [5] ldar w21, [x28] (cur_epoch = g_cache_epoch) */
            a64_emit(b, 0x88DFFC00U | (28U << 5) | 21U);
            /* [6] ldp x22, x17, [x23, #0] (x22 = key|epoch, x17 = host_code) */
            a64_emit(b, 0xA9400000U | (1U << 31) | (17U << 10) | (23U << 5) | 22U);
            /* [7] eor x21, x21, x22, lsr #32 */
            a64_emit(b, 0xCA5682B5U);
            /* [8] cbnz x21, .Lbail (+15 words) */
            a64_emit(b, 0xB5000015U | (15U << 5));
            /* [9] ldr x16, [x27, #80] (cur_entry->ret_key | (ret_epoch << 32)) */
            a64_emit(b, 0xF9402B70U);
            /* [10] ldp x21, x22, [x27, #88] (cur_entry->ret_code, cur_entry->ret_entry) */
            a64_emit(b, 0xA945DB75U);
            /* [11] cbz x22, .Lskip_push (+9 words -> [20] mov x27, x23) */
            a64_emit(b, 0xB4000016U | (9U << 5));
            /* [12] ldr w21, [x19, #424] (w21 = ctx->ras_top, uimm = 424/4 = 106) */
            a64_emit(b, 0xB9400000U | (106U << 10) | (A64_XCTX << 5) | 21U);
            /* [13] add w22, w21, #1 */
            a64_emit(b, 0x110006B6U);
            /* [14] and w22, w22, #0x3f */
            a64_emit(b, 0x120016D6U);
            /* [15] str w22, [x19, #424] (ctx->ras_top = (ras_top + 1) & 63) */
            a64_emit(b, 0xB9000000U | (106U << 10) | (A64_XCTX << 5) | 22U);
            /* [16] add x21, x19, x21, lsl #5 (x21 = ctx + ras_top * 32) */
            a64_emit(b, 0x8B151675U);
            /* [17] str x16, [x21, #432] (ras[top].key | (ras[top].epoch << 32), uimm = 432/8 = 54) */
            a64_emit(b, 0xF9000000U | (54U << 10) | (21U << 5) | 16U);
            /* [18] ldp x16, x22, [x27, #88] (cur_entry->ret_code, cur_entry->ret_entry) */
            a64_emit(b, 0xA945DB70U);
            /* [19] stp x16, x22, [x21, #440] (ras[top].host_code, ras[top].entry, imm7 = 440/8 = 55) */
            a64_emit(b, 0xA9000000U | (1U << 31) | (55U << 15) | (22U << 10) | (21U << 5) | 16U);
            /* [20] .Lskip_push: mov x27, x23 (cur_entry = next_entry) */
            a64_emit(b, 0xAA1703FBU);
            /* [21] add x24, x24, #1 (chain_hits_delta++) */
            a64_emit(b, 0x91000718U);
            /* [22] br x17 */
            a64_emit(b, 0xD61F0220U);
        }
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
    vfp_emit_exit_flush(b);
    a64_emit_mov_w(b, A64_TMP0, target_reg);

    /* [0] ldp x17, x23, [x19, #368] (pending_signals, flags|itstate) */
    a64_emit(b, 0xA9400000U | (1U << 31) | ((368U / 8U) << 15) | (23U << 10) | (A64_XCTX << 5) | 17U);
    /* [1] orr x17, x17, x23 */
    a64_emit(b, 0xAA170231U);
    /* [2] cbnz x17, .Lbail */
    size_t patch_sig = b->count;
    a64_emit(b, 0xB5000011U);

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
        /* mov x27, x23 */
        a64_emit(b, 0xAA1703FBU);
        /* add x24, x24, #1 */
        a64_emit(b, 0x91000718U);
        /* br x17 */
        a64_emit(b, 0xD61F0220U);

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
    /* ldr x23, [x22, #2496] */
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
        /* [9] ldr x16, [x27, #80] (cur_entry->ret_key | (ret_epoch << 32)) */
        a64_emit(b, 0xF9402B70U);
        /* [10] ldp x21, x22, [x27, #88] (cur_entry->ret_code, cur_entry->ret_entry) */
        a64_emit(b, 0xA945DB75U);
        /* [11] cbz x22, .Lskip_push (+9 words -> [20] mov x27, x23) */
        a64_emit(b, 0xB4000016U | (9U << 5));
        /* [12] ldr w21, [x19, #424] (w21 = ctx->ras_top, uimm = 424/4 = 106) */
        a64_emit(b, 0xB9400000U | (106U << 10) | (A64_XCTX << 5) | 21U);
        /* [13] add w22, w21, #1 */
        a64_emit(b, 0x110006B6U);
        /* [14] and w22, w22, #0x3f */
        a64_emit(b, 0x120016D6U);
        /* [15] str w22, [x19, #424] (ctx->ras_top = (ras_top + 1) & 63) */
        a64_emit(b, 0xB9000000U | (106U << 10) | (A64_XCTX << 5) | 22U);
        /* [16] add x21, x19, x21, lsl #5 (x21 = ctx + ras_top * 32) */
        a64_emit(b, 0x8B151675U);
        /* [17] str x16, [x21, #432] (ras[top].key | (ras[top].epoch << 32), uimm = 432/8 = 54) */
        a64_emit(b, 0xF9000000U | (54U << 10) | (21U << 5) | 16U);
        /* [18] ldp x16, x22, [x27, #88] (cur_entry->ret_code, cur_entry->ret_entry) */
        a64_emit(b, 0xA945DB70U);
        /* [19] stp x16, x22, [x21, #440] (ras[top].host_code, ras[top].entry, imm7 = 440/8 = 55) */
        a64_emit(b, 0xA9000000U | (1U << 31) | (55U << 15) | (22U << 10) | (21U << 5) | 16U);
    }

    /* mov x27, x23 */
    a64_emit(b, 0xAA1703FBU);
    /* add x24, x24, #1 */
    a64_emit(b, 0x91000718U);
    /* br x17 */
    a64_emit(b, 0xD61F0220U);

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
