/*
 * src/core/core_emit_shared.c — Shared A32/T32 lowering helpers
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "a64_emit.h"

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
    if (masked == 0x0E1E0F10U && rt < 15U) {
        a64_emit(b, 0xD53BE000U | (rt & 31U));
        return true;
    }
    {
        uint32_t mrrc_masked = insn28 & 0x0FF00FFFU;
        unsigned rt2 = (insn28 >> 16) & 0xFU;
        unsigned rt_lo = (insn28 >> 12) & 0xFU;
        if (mrrc_masked == 0x0C500F1EU && rt_lo < 15U && rt2 < 15U && rt_lo != rt2) {
            a64_emit(b, 0xD53BE050U);
            a64_emit_mov_w(b, rt_lo, 16U);
            a64_emit(b, 0xD360FE11U);
            a64_emit_mov_w(b, rt2, 17U);
            return true;
        }
    }
    if (masked == 0x0E1D0F70U) {
        a64_emit_ldr_imm_w(b, rt, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, tls_tp));
        return true;
    }
    if (masked == 0x0E1D0F50U) {
        a64_emit_ldr_imm_w(b, rt, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, tls_tpidrurw));
        return true;
    }
    if (masked == 0x0E0D0F50U) {
        a64_emit_str_imm_w(b, rt, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, tls_tpidrurw));
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
    /* Flush even inside self-loops so a faulting transfer observes current
     * FP registers in ctx (A32-R7-5). */
    if (b->vfp_slot_dirty != 0U) {
        a64_vfp_cache_flush(b, false);
    }

    unsigned regs[16];
    unsigned count = 0U;
    for (unsigned r = 0U; r < 16U; r++) {
        if ((reg_mask & (1U << r)) != 0U) {
            regs[count++] = r;
        }
    }
    uint32_t total_bytes = count * 4U;
    uint32_t span = (total_bytes > 0U) ? total_bytes : 4U;
    uint32_t start_offset = u_bit ? (p_bit ? 4U : 0U) : (total_bytes - (p_bit ? 0U : 4U));

    if (!u_bit) {
        a64_emit_sub_imm_w(b, false, A64_ADDR, rn, (uint16_t)start_offset, 0U);
        a64_emit_guest_addr_x(b, A64_TMP2, A64_ADDR, span, !l_bit);
    } else if (start_offset != 0U) {
        a64_emit_add_imm_w(b, false, A64_ADDR, rn, (uint16_t)start_offset, 0U);
        a64_emit_guest_addr_x(b, A64_TMP2, A64_ADDR, span, !l_bit);
    } else {
        a64_emit_guest_addr_x(b, A64_TMP2, rn, span, !l_bit);
    }

    /*
     * First/last-byte pre-validation: touch both ends of the transfer range
     * before mutating any guest register, so an unmapped page faults with
     * clean architectural state instead of a partially-updated register
     * file. Probes are reads only and never modify guest memory: a
     * read-modify-write probe could interleave with a peer store and
     * expose a torn word assembled from neither architectural store
     * (A32-R7-8).
     */
    if (count > 0U) {
        a64_emit_ldrb_imm_w(b, A64_ZR, A64_TMP2, 0U);
        if (total_bytes > 1U) {
            a64_emit_ldrb_imm_w(b, A64_ZR, A64_TMP2, total_bytes - 1U);
        }
    }

    bool loads_pc = l_bit && ((reg_mask & (1U << 15)) != 0U);
    unsigned idx = 0U;
    while (idx < count) {
        uint32_t byte_off = idx * 4U;
        if (idx + 1U < count) {
            unsigned r1 = regs[idx];
            unsigned r2 = regs[idx + 1U];
            if (l_bit) {
                unsigned dst1 = (r1 == 15U) ? A64_TMP0 : ((r1 == rn) ? A64_TMP1 : r1);
                unsigned dst2 = (r2 == 15U) ? A64_TMP0 : ((r2 == rn) ? A64_TMP1 : r2);
                a64_emit(b, 0x29400000U | (((byte_off / 4U) & 0x7FU) << 15) |
                                ((dst2 & 31U) << 10) | (A64_TMP2 << 5) | (dst1 & 31U));
                idx += 2U;
                continue;
            }
            if (r1 != 15U && r2 != 15U) {
                a64_emit(b, 0x29000000U | (((byte_off / 4U) & 0x7FU) << 15) |
                                ((r2 & 31U) << 10) | (A64_TMP2 << 5) | (r1 & 31U));
                idx += 2U;
                continue;
            }
        }
        unsigned r = regs[idx];
        if (l_bit) {
            unsigned dst = (r == 15U) ? A64_TMP0 : ((r == rn) ? A64_TMP1 : r);
            a64_emit(b, 0xB9400000U | (((byte_off / 4U) & 0xFFFU) << 10) |
                            (A64_TMP2 << 5) | (dst & 31U));
        } else {
            unsigned src = a32_load_reg(b, r, pc_val, A64_TMP1);
            a64_emit(b, 0xB9000000U | (((byte_off / 4U) & 0xFFFU) << 10) |
                            (A64_TMP2 << 5) | (src & 31U));
        }
        idx++;
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
