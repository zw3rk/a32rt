/*
 * src/interp/interp_a32_dp.c — A32 data-processing, multiply, exclusive, and extra L/S lowering
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "interp/decode_priv.h"

extern uint8_t a32_pred_cond_mask(unsigned cond);
extern bool a32_pred_fold_addsub_imm(a64_buf_t *b, bool is_sub, bool set_flags,
    unsigned dst, unsigned src_n, uint32_t imm32);
extern bool a32_pred_try_logical_imm(a64_buf_t *b, unsigned opc,
    unsigned dst, unsigned src_n, uint32_t eff_imm);
extern void a32_pred_logical_imm_s1(a64_buf_t *b, a32_ctx_t *ctx, unsigned base_opc,
    unsigned dst, unsigned src_n, uint32_t eff_imm, bool has_imm_carry,
    unsigned imm_carry, uint8_t live_out, bool is_test);
extern void a32_pred_logical_reg_s1(a64_buf_t *b, a32_ctx_t *ctx, unsigned op_class,
    unsigned dst, unsigned src_n, unsigned src_m, unsigned shift,
    unsigned amount, bool has_imm_carry, unsigned imm_carry,
    unsigned shift_carry_reg, uint8_t live_out);
extern bool a32_pred_fuse_imm_shift(unsigned shift_type, unsigned imm5, bool is_logical,
    bool want_carry, unsigned *out_shift, unsigned *out_amount);
extern bool a32_pred_branchless(a64_buf_t *b, unsigned cond, unsigned pred_op,
    unsigned rd, unsigned rn, unsigned rm, bool is_imm, uint32_t imm32,
    unsigned shift_type, unsigned shift_amt);

unsigned emit_a32_shifted_reg(a64_buf_t *b, uint32_t insn, uint32_t pc_val,
                              unsigned out_reg, bool want_carry, unsigned *out_carry_reg)
{
    unsigned rm = insn & 0xFU;
    unsigned shift_type = (insn >> 5) & 3U;
    bool by_reg = ((insn >> 4) & 1U) != 0U;
    unsigned src_m = a32_load_reg(b, rm, pc_val, out_reg);

    if (!by_reg) {
        a64_emit_shift_imm_w(b, out_reg, src_m, shift_type, (insn >> 7) & 31U,
                             want_carry, out_carry_reg);
        return out_reg;
    }

    if (out_carry_reg) {
        *out_carry_reg = A64_ZR;
    }
    unsigned rs = (insn >> 8) & 0xFU;
    unsigned src_s = a32_load_reg(b, rs, pc_val, A64_TMP2);
    bool calc_carry = want_carry && (out_carry_reg != NULL);
    a64_emit_arm_shift_reg_w(b, out_reg, src_m, src_s, shift_type, calc_carry);
    if (calc_carry) {
        *out_carry_reg = A64_TMP2;
    }
    return out_reg;
}

bool a32_decode_dp_op01_nzcv(a32_ctx_t *ctx, a64_buf_t *b, uint32_t pc, uint32_t pc_val,
                        uint32_t insn, unsigned cond, unsigned op1, uint8_t live_out,
                        bool *out_ends_block)
{
    bool handled = false;
    {
        if (op1 == 0U && (insn & 0x0FFFFFF0U) == 0x012FFF10U) {
            /* BX Rm */
            unsigned rm = insn & 0xFU;
            unsigned target = a32_load_reg(b, rm, pc_val, A64_TMP0);
            a64_emit_exit_branch_reg(b, target, true);
            if (cond == A64_COND_AL) {
                *out_ends_block = true;
            }
            handled = true;
        } else if (op1 == 0U && (insn & 0x0FFFFFF0U) == 0x012FFF30U) {
            /* BLX Rm */
            unsigned rm = insn & 0xFU;
            unsigned target = a32_load_reg(b, rm, pc_val, A64_TMP0);
            if (rm == 14U) {
                a64_emit_mov_w(b, A64_TMP0, target);
                target = A64_TMP0;
            }
            a64_emit_mov_pc32(b, 14U, pc + 4U);
            if (cond == A64_COND_AL) {
                b->is_call = true;
                *out_ends_block = true;
            }
            a64_emit_exit_branch_reg(b, target, true);
            handled = true;
        } else if (op1 == 0U && (insn & 0x0FFF0FF0U) == 0x016F0F10U) {
            /* CLZ Rd, Rm */
            a64_emit_clz_w(b, (insn >> 12) & 0xFU, insn & 0xFU);
            handled = true;
        } else if (op1 == 1U && ((insn & 0x0FB00000U) == 0x03000000U)) {
            /* MOVW (bit 22 == 0) / MOVT (bit 22 == 1) */
            bool is_movt = ((insn >> 22) & 1U) != 0U;
            unsigned rd = (insn >> 12) & 0xFU;
            uint16_t imm16 = (uint16_t)(((insn >> 4) & 0xF000U) | (insn & 0xFFFU));
            if (is_movt) {
                a64_emit_movk_w(b, rd, imm16, 1U);
            } else {
                a64_emit_movz_w(b, rd, imm16, 0U);
            }
            handled = true;
        } else if (op1 == 1U && ((insn & 0x0FFFFFF0U) == 0x0320F000U)) {
            /* NOP / YIELD / WFE / WFI hints */
            handled = true;
        } else if (op1 == 0U && ((insn & 0x0FBF0FFFU) == 0x010F0000U)) {
            /* MRS Rd, CPSR */
            a64_emit_mrs_cpsr(b, (insn >> 12) & 0xFU);
            handled = true;
        } else if (op1 == 0U && (insn & 0x0FF0FFF0U) == 0x0120F000U &&
                   ((insn >> 16) & 0xFU) != 0U) {
            /* MSR APSR/CPSR_<fields>, Rm */
            unsigned src = a32_load_reg(b, insn & 0xFU, pc_val, A64_TMP0);
            a64_emit_msr_cpsr_reg(b, src, (insn >> 16) & 0xFU);
            handled = true;
        } else if (op1 == 1U && (insn & 0x0FF0F000U) == 0x0320F000U &&
                   ((insn >> 16) & 0xFU) != 0U) {
            /* MSR APSR/CPSR_<fields>, #imm */
            uint32_t imm32 = a32_ror32(insn & 0xFFU, ((insn >> 8) & 0xFU) * 2U);
            a64_emit_mov_imm32(b, A64_TMP0, imm32);
            a64_emit_msr_cpsr_reg(b, A64_TMP0, (insn >> 16) & 0xFU);
            handled = true;
        } else if (op1 == 0U && ((insn & 0x0FB00FF0U) == 0x01000090U)) {
            /* SWP / SWPB. The loaded value stays in a scratch register
             * until the store succeeds, so a faulting swap never exposes
             * a partial update in Rd (A32-R7-9). */
            bool b_bit = ((insn >> 22) & 1U) != 0U;
            unsigned rn = (insn >> 16) & 0xFU;
            unsigned rd = (insn >> 12) & 0xFU;
            unsigned rm = insn & 0xFU;
            unsigned src_raw = a32_load_reg(b, rm, pc_val, A64_TMP1);
            a64_emit_mov_w(b, A64_TMP1, src_raw);

            /* Compute 64-bit host address in x22 = xG + uxtw(wRn) */
            a64_emit_guest_addr_x(b, A64_ADDR, rn, b_bit ? 1U : 4U, true);

            size_t loop_top = b->count;
            if (b_bit) {
                a64_emit_ldxr_b(b, A64_TMP0, A64_ADDR);
                a64_emit_stxr_b(b, A64_TMP2, A64_TMP1, A64_ADDR);
            } else {
                a64_emit_ldxr_w(b, A64_TMP0, A64_ADDR);
                a64_emit_stxr_w(b, A64_TMP2, A64_TMP1, A64_ADDR);
            }
            a64_emit(b, 0x35000000U |
                            ((((uint32_t)((int32_t)loop_top - (int32_t)b->count)) & 0x7FFFFU) << 5) |
                            A64_TMP2);
            if (rd != 15U) {
                a64_emit_mov_w(b, rd, A64_TMP0);
            }
            handled = true;
        } else if (op1 == 0U && ((insn & 0x0F8000F0U) == 0x01800090U)) {
            /* Exclusives: LDREX / STREX / LDREXD / STREXD / LDREXB / STREXB / LDREXH / STREXH */
            unsigned ex_op = (insn >> 20) & 0x7U;
            unsigned rn = (insn >> 16) & 0xFU;
            unsigned rd = (insn >> 12) & 0xFU;
            unsigned rt = insn & 0xFU;
            uint32_t ex_span = (ex_op < 2U) ? 4U : (ex_op < 4U) ? 8U : (ex_op < 6U) ? 1U : 2U;

            a64_emit_guest_addr_x(b, A64_ADDR, rn, ex_span, (ex_op & 1U) == 0U);

            switch (ex_op) {
            case 0U: /* STREX Rd, Rt, [Rn] */
                a64_emit_stxr_w(b, rd, rt, A64_ADDR);
                handled = true;
                break;
            case 1U: /* LDREX Rd, [Rn] */
                a64_emit_ldxr_w(b, rd, A64_ADDR);
                handled = true;
                break;
            case 2U: /* STREXD Rd, Rt, Rt2, [Rn] -> 64-bit STXR */
                a64_emit_mov_w(b, A64_TMP0, rt);
                a64_emit_bfi_x(b, A64_TMP0, (rt + 1U) & 0xFU, 32U, 32U);
                a64_emit_stxr_x(b, rd, A64_TMP0, A64_ADDR);
                handled = true;
                break;
            case 3U: /* LDREXD Rd, Rd2, [Rn] -> 64-bit LDXR */
                a64_emit_ldxr_x(b, A64_TMP0, A64_ADDR);
                a64_emit_mov_w(b, rd, A64_TMP0);
                a64_emit_ubfx_x_to_w(b, (rd + 1U) & 0xFU, A64_TMP0, 32U, 32U);
                handled = true;
                break;
            case 4U: /* STREXB */
                a64_emit_stxr_b(b, rd, rt, A64_ADDR);
                handled = true;
                break;
            case 5U: /* LDREXB */
                a64_emit_ldxr_b(b, rd, A64_ADDR);
                handled = true;
                break;
            case 6U: /* STREXH */
                a64_emit_stxr_h(b, rd, rt, A64_ADDR);
                handled = true;
                break;
            case 7U: /* LDREXH */
                a64_emit_ldxr_h(b, rd, A64_ADDR);
                handled = true;
                break;
            default:
                break;
            }
        } else if (op1 == 0U && ((insn & 0x0F900FF0U) == 0x01000050U)) {
            /* QADD / QSUB / QDADD / QDSUB Rd, Rm, Rn */
            unsigned op = (insn >> 21) & 3U;
            a64_emit_qaddsub(b, (insn >> 12) & 0xFU, insn & 0xFU, (insn >> 16) & 0xFU,
                             (op & 1U) != 0U, (op & 2U) != 0U);
            handled = true;
        } else if (op1 == 0U && ((insn & 0x0F900DF0U) == 0x01000040U)) {
            /* CRC32{B,H,W} / CRC32C{B,H,W} Rd, Rn, Rm */
            unsigned sz    = (insn >> 21) & 3U;
            unsigned rn    = (insn >> 16) & 0xFU;
            unsigned rd    = (insn >> 12) & 0xFU;
            unsigned c_bit = (insn >> 9) & 1U;
            unsigned rm    = insn & 0xFU;
            if (rd != 15U && rn != 15U && rm != 15U && sz != 3U) {
                a64_emit_crc32_w(b, rd, rn, rm, sz, c_bit);
                handled = true;
            }
        } else if (op1 == 0U && ((insn & 0x0FF0F090U) == 0x01600080U)) {
            /* SMUL<x><y> Rd, Rm, Rs */
            a64_emit_smla_xy(b, (insn >> 16) & 0xFU, insn & 0xFU, (insn >> 8) & 0xFU, 15U,
                             ((insn >> 5) & 1U) != 0U, ((insn >> 6) & 1U) != 0U);
            handled = true;
        } else if (op1 == 0U && ((insn & 0x0FF00090U) == 0x01000080U)) {
            /* SMLA<x><y> Rd, Rm, Rs, Rn */
            a64_emit_smla_xy(b, (insn >> 16) & 0xFU, insn & 0xFU, (insn >> 8) & 0xFU,
                             (insn >> 12) & 0xFU, ((insn >> 5) & 1U) != 0U, ((insn >> 6) & 1U) != 0U);
            handled = true;
        } else if (op1 == 0U && ((insn & 0x0FF0F0B0U) == 0x012000A0U)) {
            /* SMULW<y> Rd, Rm, Rs */
            a64_emit_smlaw_y(b, (insn >> 16) & 0xFU, insn & 0xFU, (insn >> 8) & 0xFU, 15U,
                             ((insn >> 6) & 1U) != 0U);
            handled = true;
        } else if (op1 == 0U && ((insn & 0x0FF000B0U) == 0x01200080U)) {
            /* SMLAW<y> Rd, Rm, Rs, Rn */
            a64_emit_smlaw_y(b, (insn >> 16) & 0xFU, insn & 0xFU, (insn >> 8) & 0xFU,
                             (insn >> 12) & 0xFU, ((insn >> 6) & 1U) != 0U);
            handled = true;
        } else if (op1 == 0U && ((insn & 0x0FF00090U) == 0x01400080U)) {
            /* SMLAL<x><y> RdLo, RdHi, Rm, Rs */
            a64_emit_smlal_xy(b, (insn >> 12) & 0xFU, (insn >> 16) & 0xFU, insn & 0xFU,
                              (insn >> 8) & 0xFU, ((insn >> 5) & 1U) != 0U, ((insn >> 6) & 1U) != 0U);
            handled = true;
        } else if (op1 == 0U && ((insn & 0x0FC000F0U) == 0x00000090U)) {
            /* MUL / MLA */
            bool accumulate = ((insn >> 21) & 1U) != 0U;
            bool set_flags  = ((insn >> 20) & 1U) != 0U;
            unsigned rd = (insn >> 16) & 0xFU;
            unsigned ra = (insn >> 12) & 0xFU;
            unsigned rm = (insn >> 8) & 0xFU;
            unsigned rn = insn & 0xFU;
            if (set_flags && live_out == 0U) {
                set_flags = false;
                ctx->flags_elided++;
            }
            a64_emit_madd_w(b, rd, rn, rm, accumulate ? ra : A64_ZR);
            if (set_flags) {
                a64_emit_update_nz_keep_cv(b, rd);
            }
            handled = true;
        } else if (op1 == 0U && ((insn & 0x0FF000F0U) == 0x00400090U)) {
            /* UMAAL RdLo, RdHi, Rn, Rm */
            a64_emit_umaal(b, (insn >> 12) & 0xFU, (insn >> 16) & 0xFU,
                           insn & 0xFU, (insn >> 8) & 0xFU);
            handled = true;
        } else if (op1 == 0U && ((insn & 0x0FF000F0U) == 0x00600090U)) {
            /* MLS Rd, Rn, Rm, Ra */
            a64_emit_msub_w(b, (insn >> 16) & 0xFU, insn & 0xFU, (insn >> 8) & 0xFU,
                            (insn >> 12) & 0xFU);
            handled = true;
        } else if (op1 == 0U && ((insn & 0x0F8000F0U) == 0x00800090U)) {
            /* UMULL / UMLAL / SMULL / SMLAL */
            bool set_flags = ((insn >> 20) & 1U) != 0U;
            if (set_flags && live_out == 0U) {
                set_flags = false;
                ctx->flags_elided++;
            }
            a64_emit_mull(b, ((insn >> 22) & 1U) != 0U, ((insn >> 21) & 1U) != 0U, set_flags,
                          (insn >> 12) & 0xFU, (insn >> 16) & 0xFU, insn & 0xFU, (insn >> 8) & 0xFU);
            handled = true;
        } else if (op1 == 0U && ((insn & 0x0E000090U) == 0x00000090U) &&
                   (((insn >> 5) & 3U) != 0U)) {
            /* Extra load/store: LDRH, STRH, LDRSB, LDRSH, LDRD, STRD */
            bool p_bit = ((insn >> 24) & 1U) != 0U;
            bool u_bit = ((insn >> 23) & 1U) != 0U;
            bool i_bit = ((insn >> 22) & 1U) != 0U;
            bool w_bit = ((insn >> 21) & 1U) != 0U;
            bool l_bit = ((insn >> 20) & 1U) != 0U;
            unsigned rn = (insn >> 16) & 0xFU;
            unsigned rt = (insn >> 12) & 0xFU;
            unsigned op2 = (insn >> 5) & 3U;

            if (!(!l_bit && (op2 == 2U || op2 == 3U) && ((rt & 1U) != 0U || rt == 14U))) {
                unsigned base_reg = a32_load_reg(b, rn, pc_val, A64_TMP2);
                uint16_t imm8 = (uint16_t)(((insn >> 4) & 0xF0U) | (insn & 0xFU));
                unsigned rm = insn & 0xFU;
                unsigned post_off_reg = rm;

                if (p_bit) {
                    if (i_bit) {
                        if (u_bit) {
                            a64_emit_add_imm_w(b, false, A64_ADDR, base_reg, imm8, 0U);
                        } else {
                            a64_emit_sub_imm_w(b, false, A64_ADDR, base_reg, imm8, 0U);
                        }
                    } else {
                        unsigned off_reg = a32_load_reg(b, rm, pc_val, A64_TMP1);
                        if (u_bit) {
                            a64_emit_add_reg_w(b, false, A64_ADDR, base_reg, off_reg, A64_SHIFT_LSL, 0U);
                        } else {
                            a64_emit_sub_reg_w(b, false, A64_ADDR, base_reg, off_reg, A64_SHIFT_LSL, 0U);
                        }
                    }
                } else {
                    a64_emit_mov_w(b, A64_ADDR, base_reg);
                    if (!i_bit && (l_bit || op2 == 2U) &&
                        (rm == rt || (!l_bit && op2 == 2U && rm == ((rt + 1U) & 0xFU)))) {
                        a64_emit_mov_w(b, A64_TMP0, rm);
                        post_off_reg = A64_TMP0;
                    }
                }

                if (l_bit) {
                    if (op2 == 1U) {
                        a64_emit_guest_ldrh_w(b, rt, A64_ADDR);
                    } else if (op2 == 2U) {
                        a64_emit_guest_ldrsb_w(b, rt, A64_ADDR);
                    } else {
                        a64_emit_guest_ldrsh_w(b, rt, A64_ADDR);
                    }
                } else {
                    if (op2 == 1U) {
                        a64_emit_guest_strh_w(b, rt, A64_ADDR);
                    } else if (op2 == 2U) {
                        /* LDRD Rt, Rt2, [addr] */
                        a64_emit_guest_ldrd_w(b, rt, (rt + 1U) & 0xFU, A64_ADDR);
                    } else {
                        /* STRD Rt, Rt2, [addr] */
                        a64_emit_guest_strd_w(b, rt, (rt + 1U) & 0xFU, A64_ADDR);
                    }
                }

                bool is_actual_load = l_bit || (op2 == 2U);
                bool loads_rn = is_actual_load &&
                                (rn == rt || (!l_bit && op2 == 2U && rn == ((rt + 1U) & 0xFU)));

                if (!p_bit) {
                    if (rn != 15U && !loads_rn) {
                        if (i_bit) {
                            if (u_bit) {
                                a64_emit_add_imm_w(b, false, rn, base_reg, imm8, 0U);
                            } else {
                                a64_emit_sub_imm_w(b, false, rn, base_reg, imm8, 0U);
                            }
                        } else {
                            unsigned off_reg = (post_off_reg == A64_TMP0)
                                                   ? A64_TMP0
                                                   : a32_load_reg(b, post_off_reg, pc_val, A64_TMP1);
                            if (u_bit) {
                                a64_emit_add_reg_w(b, false, rn, base_reg, off_reg, A64_SHIFT_LSL, 0U);
                            } else {
                                a64_emit_sub_reg_w(b, false, rn, base_reg, off_reg, A64_SHIFT_LSL, 0U);
                            }
                        }
                    }
                } else if (w_bit && rn != 15U && !loads_rn) {
                    a64_emit_mov_w(b, rn, A64_ADDR);
                }
                handled = true;
            }
        } else if (!(op1 == 0U && (insn & 0x00000090U) == 0x00000090U) &&
                   !((insn & 0x01900000U) == 0x01000000U)) {
            /* Standard Data-Processing (Immediate or Shifted Register) */
            bool is_imm    = (op1 == 1U);
            unsigned opc   = (insn >> 21) & 0xFU;
            bool s_bit     = ((insn >> 20) & 1U) != 0U;
            unsigned rn    = (insn >> 16) & 0xFU;
            unsigned rd    = (insn >> 12) & 0xFU;
            bool set_flags = s_bit;
            if (s_bit && rd != 15U && !(opc >= 8U && opc <= 11U)) {
                if (live_out == 0U) {
                    set_flags = false;
                    ctx->flags_elided++;
                }
            }
            unsigned src_n = a32_load_reg(b, rn, pc_val, A64_TMP0);
            unsigned src_m = A64_TMP1;
            bool has_imm_carry = false;
            unsigned imm_carry = 0U;
            unsigned shift_carry_reg = A64_ZR;
            bool is_logical_op = (opc == 0x0U || opc == 0x1U || opc == 0x8U || opc == 0x9U ||
                                  opc >= 0xCU);
            uint32_t imm32 = 0U;
            unsigned fused_shift = 0U;
            unsigned fused_amt = 0U;
            bool fused = false;
            bool is_fusable_reg = false;
            if (is_imm) {
                unsigned rot = ((insn >> 8) & 0xFU) * 2U;
                imm32 = a32_ror32(insn & 0xFFU, rot);
                if (rot != 0U) {
                    has_imm_carry = true;
                    imm_carry = (imm32 >> 31) & 1U;
                }
                if (!s_bit || !is_logical_op) {
                    has_imm_carry = false;
                }
            } else {
                unsigned rm = insn & 0xFU;
                bool by_reg = ((insn >> 4) & 1U) != 0U;
                unsigned st = (insn >> 5) & 3U;
                unsigned imm5 = (insn >> 7) & 31U;
                bool is_rsb_adc = (opc == 0x3U || opc == 0x5U || opc == 0x6U || opc == 0x7U);
                if (!by_reg && rm != 15U && !is_rsb_adc) {
                    bool want_c = set_flags && is_logical_op && ((live_out & 0x2U) != 0U);
                    if (opc == 0x2U || opc == 0x4U || opc == 0xAU || opc == 0xBU) {
                        want_c = false;
                    }
                    bool lg = (opc == 0x0U || opc == 0x1U || opc == 0x8U || opc == 0x9U || opc >= 0xCU);
                    if (a32_pred_fuse_imm_shift(st, imm5, lg, want_c, &fused_shift, &fused_amt)) {
                        fused = true;
                        is_fusable_reg = true;
                        src_m = a32_load_reg(b, rm, pc_val, A64_TMP1);
                        shift_carry_reg = A64_ZR;
                        has_imm_carry = false;
                    }
                }
                if (!fused) {
                    src_m = emit_a32_shifted_reg(b, insn, pc_val, A64_TMP1,
                                                 set_flags && is_logical_op, &shift_carry_reg);
                }
            }
            unsigned dst = (rd == 15U) ? A64_TMP0 : rd;
            bool is_logical = false;
            if (is_imm && rd != 15U) {
                if ((opc == 0x2U || opc == 0x4U || opc == 0xAU || opc == 0xBU)) {
                    bool is_sub = (opc == 0x2U || opc == 0xAU);
                    unsigned d2 = (opc == 0xAU || opc == 0xBU) ? (unsigned)A64_ZR : dst;
                    bool sf2 = (opc == 0xAU || opc == 0xBU) ? true : set_flags;
                    if (rn == 15U && (opc == 0x2U || opc == 0x4U) && !s_bit) {
                        uint32_t c = (opc == 0x2U) ? (pc_val - imm32) : (pc_val + imm32);
                        a64_emit_mov_imm32(b, dst, c);
                        handled = true;
                        goto dp_done;
                    }
                    if (a32_pred_fold_addsub_imm(b, is_sub, sf2, d2, src_n, imm32)) {
                        handled = true;
                        goto dp_done;
                    }
                }
                if (opc == 0x3U && imm32 == 0U && rd != 15U) {
                    a64_emit_sub_reg_w(b, set_flags, dst, A64_ZR, src_n, A64_SHIFT_LSL, 0U);
                    handled = true;
                    goto dp_done;
                }
                if ((opc == 0xDU || opc == 0xFU) && rd != 15U) {
                    uint32_t v = (opc == 0xDU) ? imm32 : ~imm32;
                    a64_emit_mov_imm32(b, dst, v);
                    is_logical = true;
                    if (set_flags) {
                        if (live_out == 0U) {
                            ctx->flags_elided++;
                        } else if ((live_out & 0x3U) == 0U) {
                            a64_emit_and_reg_w(b, true, A64_ZR, dst, dst, A64_SHIFT_LSL, 0U);
                        } else {
                            a64_emit_logical_flags(b, dst, has_imm_carry, imm_carry, A64_ZR);
                        }
                    }
                    handled = true;
                    goto dp_done;
                }
                if ((opc == 0x0U || opc == 0x1U || opc == 0x8U || opc == 0x9U || opc == 0xCU || opc == 0xEU)) {
                    bool is_test = (opc == 0x8U || opc == 0x9U);
                    unsigned base = (opc == 0x0U || opc == 0x8U) ? 0U : (opc == 0x1U || opc == 0x9U) ? 2U : 1U;
                    uint32_t eff = imm32;
                    if (opc == 0xEU) {
                        eff = ~imm32;
                        base = 0U;
                    }
                    unsigned imms = 0U;
                    unsigned immr = 0U;
                    if (a64_encode_logical_imm32(eff, &imms, &immr)) {
                        if (!set_flags) {
                            a64_emit_logical_imm_w(b, base, is_test ? A64_TMP0 : dst, src_n, eff);
                        } else {
                            a32_pred_logical_imm_s1(b, ctx, base, is_test ? A64_TMP0 : dst,
                                src_n, eff, has_imm_carry, imm_carry, live_out, is_test);
                        }
                        if (!is_test) {
                            is_logical = false;
                        } else {
                            is_logical = false;
                        }
                        handled = true;
                        goto dp_done;
                    }
                    a64_emit_mov_imm32(b, A64_TMP1, imm32);
                    src_m = A64_TMP1;
                } else {
                    a64_emit_mov_imm32(b, A64_TMP1, imm32);
                    src_m = A64_TMP1;
                }
            }
            switch (opc) {
            case 0x0U:
                if (set_flags && is_fusable_reg) {
                    a32_pred_logical_reg_s1(b, ctx, 0U, dst, src_n, src_m, fused_shift, fused_amt,
                        false, 0U, A64_ZR, live_out);
                } else if (set_flags && !is_imm) {
                    a32_pred_logical_reg_s1(b, ctx, 0U, dst, src_n, src_m, A64_SHIFT_LSL, 0U,
                        has_imm_carry, imm_carry, shift_carry_reg, live_out);
                } else {
                    a64_emit_and_reg_w(b, false, dst, src_n, src_m,
                        fused ? fused_shift : A64_SHIFT_LSL, fused ? fused_amt : 0U);
                }
                is_logical = !set_flags;
                if (set_flags && (is_imm || !is_fusable_reg)) {
                    is_logical = false;
                }
                handled = true;
                break;
            case 0x1U:
                if (set_flags) {
                    if (is_fusable_reg) {
                        a32_pred_logical_reg_s1(b, ctx, 4U, dst, src_n, src_m, fused_shift, fused_amt,
                            false, 0U, A64_ZR, live_out);
                    } else if (!is_imm) {
                        a32_pred_logical_reg_s1(b, ctx, 4U, dst, src_n, src_m, A64_SHIFT_LSL, 0U,
                            has_imm_carry, imm_carry, shift_carry_reg, live_out);
                    } else {
                        a64_emit_eor_reg_w(b, dst, src_n, src_m, A64_SHIFT_LSL, 0U);
                        a32_pred_logical_reg_s1(b, ctx, 4U, dst, dst, dst, A64_SHIFT_LSL, 0U,
                            false, 0U, A64_ZR, live_out);
                        is_logical = false;
                        handled = true;
                        break;
                    }
                    is_logical = false;
                } else {
                    a64_emit_eor_reg_w(b, dst, src_n, src_m,
                        fused ? fused_shift : A64_SHIFT_LSL, fused ? fused_amt : 0U);
                    is_logical = true;
                }
                handled = true;
                break;
            case 0x2U:
                a64_emit_sub_reg_w(b, set_flags, dst, src_n, src_m,
                    fused ? fused_shift : A64_SHIFT_LSL, fused ? fused_amt : 0U);
                handled = true;
                break;
            case 0x3U:
                a64_emit_sub_reg_w(b, set_flags, dst, src_m, src_n, A64_SHIFT_LSL, 0U);
                handled = true;
                break;
            case 0x4U:
                a64_emit_add_reg_w(b, set_flags, dst, src_n, src_m,
                    fused ? fused_shift : A64_SHIFT_LSL, fused ? fused_amt : 0U);
                handled = true;
                break;
            case 0x5U:
                a64_emit_adc_w(b, set_flags, dst, src_n, src_m);
                handled = true;
                break;
            case 0x6U:
                a64_emit_sbc_w(b, set_flags, dst, src_n, src_m);
                handled = true;
                break;
            case 0x7U:
                a64_emit_sbc_w(b, set_flags, dst, src_m, src_n);
                handled = true;
                break;
            case 0x8U:
                if (set_flags) {
                    if (is_fusable_reg) {
                        a64_emit_and_reg_w(b, false, A64_TMP0, src_n, src_m, fused_shift, fused_amt);
                        a32_pred_logical_reg_s1(b, ctx, 0U, A64_TMP0, src_n, src_m, fused_shift, fused_amt,
                            false, 0U, A64_ZR, live_out);
                    } else if (!is_imm) {
                        a64_emit_and_reg_w(b, false, A64_TMP0, src_n, src_m, A64_SHIFT_LSL, 0U);
                        a64_emit_logical_flags(b, A64_TMP0, has_imm_carry, imm_carry, shift_carry_reg);
                    } else {
                        a64_emit_and_reg_w(b, false, A64_TMP0, src_n, src_m, A64_SHIFT_LSL, 0U);
                        a64_emit_logical_flags(b, A64_TMP0, has_imm_carry, imm_carry, shift_carry_reg);
                    }
                    is_logical = false;
                } else {
                    a64_emit_and_reg_w(b, false, A64_TMP0, src_n, src_m,
                        fused ? fused_shift : A64_SHIFT_LSL, fused ? fused_amt : 0U);
                    is_logical = true;
                }
                handled = true;
                break;
            case 0x9U:
                if (set_flags) {
                    a64_emit_eor_reg_w(b, A64_TMP0, src_n, src_m,
                        fused ? fused_shift : A64_SHIFT_LSL, fused ? fused_amt : 0U);
                    if ((live_out & 0x3U) == 0U && live_out != 0U) {
                        a64_emit_and_reg_w(b, true, A64_ZR, A64_TMP0, A64_TMP0, A64_SHIFT_LSL, 0U);
                    } else if (live_out != 0U) {
                        a64_emit_logical_flags(b, A64_TMP0, has_imm_carry, imm_carry, shift_carry_reg);
                    } else {
                        ctx->flags_elided++;
                    }
                    is_logical = false;
                } else {
                    a64_emit_eor_reg_w(b, A64_TMP0, src_n, src_m,
                        fused ? fused_shift : A64_SHIFT_LSL, fused ? fused_amt : 0U);
                    is_logical = true;
                }
                handled = true;
                break;
            case 0xAU:
                a64_emit_sub_reg_w(b, true, A64_ZR, src_n, src_m,
                    fused ? fused_shift : A64_SHIFT_LSL, fused ? fused_amt : 0U);
                handled = true;
                break;
            case 0xBU:
                a64_emit_add_reg_w(b, true, A64_ZR, src_n, src_m,
                    fused ? fused_shift : A64_SHIFT_LSL, fused ? fused_amt : 0U);
                handled = true;
                break;
            case 0xCU:
                if (set_flags) {
                    if (is_fusable_reg) {
                        a32_pred_logical_reg_s1(b, ctx, 2U, dst, src_n, src_m, fused_shift, fused_amt,
                            false, 0U, A64_ZR, live_out);
                    } else if (!is_imm) {
                        a32_pred_logical_reg_s1(b, ctx, 2U, dst, src_n, src_m, A64_SHIFT_LSL, 0U,
                            has_imm_carry, imm_carry, shift_carry_reg, live_out);
                    } else {
                        a64_emit_orr_reg_w(b, dst, src_n, src_m, A64_SHIFT_LSL, 0U);
                        a32_pred_logical_reg_s1(b, ctx, 2U, dst, dst, dst, A64_SHIFT_LSL, 0U,
                            false, 0U, A64_ZR, live_out);
                        is_logical = false;
                        handled = true;
                        break;
                    }
                    is_logical = false;
                } else {
                    a64_emit_orr_reg_w(b, dst, src_n, src_m,
                        fused ? fused_shift : A64_SHIFT_LSL, fused ? fused_amt : 0U);
                    is_logical = true;
                }
                handled = true;
                break;
            case 0xDU:
                if (!fused && !is_imm) {
                    a64_emit_mov_w(b, dst, src_m);
                } else if (fused) {
                    a64_emit_orr_reg_w(b, dst, A64_ZR, src_m, fused_shift, fused_amt);
                } else if (!is_imm) {
                    a64_emit_mov_w(b, dst, src_m);
                }
                if (set_flags && !is_imm) {
                    if (live_out == 0U) {
                        ctx->flags_elided++;
                    } else if ((live_out & 0x3U) == 0U) {
                        a64_emit_and_reg_w(b, true, A64_ZR, dst, dst, A64_SHIFT_LSL, 0U);
                    } else {
                        a64_emit_logical_flags(b, dst, has_imm_carry, imm_carry, shift_carry_reg);
                    }
                    is_logical = false;
                } else {
                    is_logical = !set_flags;
                }
                handled = true;
                break;
            case 0xEU:
                if (set_flags) {
                    if (is_fusable_reg) {
                        a32_pred_logical_reg_s1(b, ctx, 1U, dst, src_n, src_m, fused_shift, fused_amt,
                            false, 0U, A64_ZR, live_out);
                    } else if (!is_imm) {
                        a32_pred_logical_reg_s1(b, ctx, 1U, dst, src_n, src_m, A64_SHIFT_LSL, 0U,
                            has_imm_carry, imm_carry, shift_carry_reg, live_out);
                    } else {
                        a64_emit_bic_reg_w(b, false, dst, src_n, src_m, A64_SHIFT_LSL, 0U);
                        a32_pred_logical_reg_s1(b, ctx, 1U, dst, dst, dst, A64_SHIFT_LSL, 0U,
                            false, 0U, A64_ZR, live_out);
                        is_logical = false;
                        handled = true;
                        break;
                    }
                    is_logical = false;
                } else {
                    a64_emit_bic_reg_w(b, false, dst, src_n, src_m,
                        fused ? fused_shift : A64_SHIFT_LSL, fused ? fused_amt : 0U);
                    is_logical = true;
                }
                handled = true;
                break;
            case 0xFU:
                if (!fused && !is_imm) {
                    a64_emit_mvn_reg_w(b, dst, src_m, A64_SHIFT_LSL, 0U);
                } else if (fused) {
                    a64_emit_orn_reg_w(b, dst, A64_ZR, src_m, fused_shift, fused_amt);
                } else if (!is_imm) {
                    a64_emit_mvn_reg_w(b, dst, src_m, A64_SHIFT_LSL, 0U);
                }
                if (set_flags && !is_imm) {
                    if (live_out == 0U) {
                        ctx->flags_elided++;
                    } else if ((live_out & 0x3U) == 0U) {
                        a64_emit_and_reg_w(b, true, A64_ZR, dst, dst, A64_SHIFT_LSL, 0U);
                    } else {
                        a64_emit_logical_flags(b, dst, has_imm_carry, imm_carry, shift_carry_reg);
                    }
                    is_logical = false;
                } else {
                    is_logical = !set_flags;
                }
                handled = true;
                break;
            default:
                break;
            }
            dp_done:;
            if (false) {
                (void)is_logical;
            }

            if (handled && cond == A64_COND_AL && rd == 14U && (opc < 0x8U || opc >= 0xCU) &&
                (rn == 15U || (!is_imm && (insn & 0xFU) == 15U))) {
                b->lr_written_from_pc = true;
            }
            if (handled && rd == 15U && (opc < 0x8U || opc >= 0xCU)) {
                a64_emit_exit_branch_reg(b, A64_TMP0, true);
                if (cond == A64_COND_AL) {
                    *out_ends_block = true;
                }
            }
        }
    }
    return handled;
}

bool a32_decode_dp_op01(a32_ctx_t *ctx, a64_buf_t *b, uint32_t pc, uint32_t pc_val,
    uint32_t insn, unsigned cond, unsigned op1, bool flags_live, bool *out_ends_block)
{
    uint8_t live = flags_live ? 0xFU : 0U;
    return a32_decode_dp_op01_nzcv(ctx, b, pc, pc_val, insn, cond, op1, live, out_ends_block);
}
