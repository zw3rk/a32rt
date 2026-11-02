/*
 * src/interp/interp_t32_dp.c — T32 (Thumb-2) control and data-processing lowering
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "interp/decode_priv.h"

static uint32_t thumb_expand_imm(uint32_t imm12, bool *out_has_carry, unsigned *out_carry)
{
    *out_has_carry = false;
    *out_carry = 0U;
    if ((imm12 >> 10) == 0U) {
        uint32_t b = imm12 & 0xFFU;
        switch ((imm12 >> 8) & 3U) {
        case 0U:
            return b;
        case 1U:
            return (b << 16) | b;
        case 2U:
            return (b << 24) | (b << 8);
        default:
            return (b << 24) | (b << 16) | (b << 8) | b;
        }
    }
    uint32_t unrotated = 0x80U | (imm12 & 0x7FU);
    unsigned rot = (imm12 >> 7) & 0x1FU;
    uint32_t res = a32_ror32(unrotated, rot);
    *out_has_carry = true;
    *out_carry = (res >> 31) & 1U;
    return res;
}

static bool emit_t32_dp_op(a64_buf_t *b, unsigned op, bool set_flags,
                           unsigned rd, unsigned rn, unsigned src_n, unsigned src_m,
                           bool has_imm_carry, unsigned imm_carry, unsigned shift_carry_reg,
                           bool is_uncond, bool *out_ends_block)
{
    unsigned dst = (rd == 15U) ? A64_TMP0 : rd;
    bool is_logical = false;

    switch (op) {
    case 0x0U: /* AND / TST */
        a64_emit_and_reg_w(b, false, dst, src_n, src_m, A64_SHIFT_LSL, 0U);
        is_logical = true;
        break;
    case 0x1U: /* BIC */
        a64_emit_bic_reg_w(b, false, dst, src_n, src_m, A64_SHIFT_LSL, 0U);
        is_logical = true;
        break;
    case 0x2U: /* ORR / MOV */
        if (rn == 15U) {
            a64_emit_mov_w(b, dst, src_m);
        } else {
            a64_emit_orr_reg_w(b, dst, src_n, src_m, A64_SHIFT_LSL, 0U);
        }
        is_logical = true;
        break;
    case 0x3U: /* ORN / MVN */
        if (rn == 15U) {
            a64_emit_mvn_reg_w(b, dst, src_m, A64_SHIFT_LSL, 0U);
        } else {
            a64_emit_orn_reg_w(b, dst, src_n, src_m, A64_SHIFT_LSL, 0U);
        }
        is_logical = true;
        break;
    case 0x4U: /* EOR / TEQ */
        a64_emit_eor_reg_w(b, dst, src_n, src_m, A64_SHIFT_LSL, 0U);
        is_logical = true;
        break;
    case 0x8U: /* ADD / CMN */
        a64_emit_add_reg_w(b, set_flags, (rd == 15U && set_flags) ? A64_ZR : dst,
                           src_n, src_m, A64_SHIFT_LSL, 0U);
        break;
    case 0xAU: /* ADC */
        a64_emit_adc_w(b, set_flags, dst, src_n, src_m);
        break;
    case 0xBU: /* SBC */
        a64_emit_sbc_w(b, set_flags, dst, src_n, src_m);
        break;
    case 0xDU: /* SUB / CMP */
        a64_emit_sub_reg_w(b, set_flags, (rd == 15U && set_flags) ? A64_ZR : dst,
                           src_n, src_m, A64_SHIFT_LSL, 0U);
        break;
    case 0xEU: /* RSB */
        a64_emit_sub_reg_w(b, set_flags, dst, src_m, src_n, A64_SHIFT_LSL, 0U);
        break;
    default:
        return false;
    }

    if (is_logical && set_flags) {
        a64_emit_logical_flags(b, dst, has_imm_carry, imm_carry, shift_carry_reg);
    }
    if (rd == 15U && !set_flags) {
        a64_emit_exit_branch_reg(b, dst, false);
        if (is_uncond) {
            *out_ends_block = true;
        }
    }
    return true;
}

bool a32_translate_t32_insn(a32_ctx_t *ctx, a64_buf_t *b, uint32_t pc, uint16_t hw1, uint16_t hw2,
                            bool is_uncond, bool flags_live, bool *out_ends_block)
{
    /* VFPv4-D32 / NEON delegation */
    if ((hw1 & 0xEF00U) == 0xEF00U ||
        (hw1 & 0xFF10U) == 0xF900U ||
        (((hw1 & 0xEE00U) == 0xEC00U || (hw1 & 0xFF00U) == 0xEE00U || (hw1 & 0xFF00U) == 0xFE00U) &&
         (((hw2 >> 8) & 0xEU) == 0xAU))) {
        return a32_decode_vfp_neon_t32(b, hw1, hw2, pc);
    }

    uint32_t pc_val = pc + 4U;
    uint32_t pc_align4 = pc_val & ~3U;

    /* 1. Branches and miscellaneous control: (hw1 & 0xF800) == 0xF000 && (hw2 & 0x8000) != 0 */
    if ((hw1 & 0xF800U) == 0xF000U && (hw2 & 0x8000U) != 0U) {
        if ((hw2 & 0xD000U) == 0xD000U || (hw2 & 0xD000U) == 0xC000U ||
            (hw2 & 0xD000U) == 0x9000U) {
            /* BL / BLX / B.W */
            bool is_blx = (hw2 & 0xD000U) == 0xC000U;
            bool is_link = (hw2 & 0x4000U) != 0U;
            uint32_t s  = (hw1 >> 10) & 1U;
            uint32_t j1 = (hw2 >> 13) & 1U;
            uint32_t j2 = (hw2 >> 11) & 1U;
            uint32_t i1 = ~(j1 ^ s) & 1U;
            uint32_t i2 = ~(j2 ^ s) & 1U;
            uint32_t imm10 = hw1 & 0x3FFU;
            uint32_t imm11 = hw2 & 0x7FFU;
            int32_t offset = (int32_t)((s << 24) | (i1 << 23) | (i2 << 22) |
                                       (imm10 << 12) | (imm11 << 1));
            if (s) {
                offset |= (int32_t)0xFE000000U;
            }
            if (is_link) {
                a64_emit_mov_pc32(b, 14U, (pc + 4U) | 1U);
                if (is_uncond) {
                    b->is_call = true;
                }
            }
            if (is_blx) {
                uint32_t target = (pc_align4 + (uint32_t)offset) & ~3U;
                a64_emit_exit_branch_imm(b, target, true);
            } else {
                uint32_t target = pc_val + (uint32_t)offset;
                a64_emit_exit_branch_imm(b, target, false);
            }
            if (is_uncond) {
                *out_ends_block = true;
            }
            return true;
        }
        if ((hw2 & 0xD000U) == 0x8000U) {
            unsigned cond = (hw1 >> 6) & 0xFU;
            if (cond < 14U) {
                /* B<cond>.W */
                uint32_t s  = (hw1 >> 10) & 1U;
                uint32_t j1 = (hw2 >> 13) & 1U;
                uint32_t j2 = (hw2 >> 11) & 1U;
                uint32_t imm6  = hw1 & 0x3FU;
                uint32_t imm11 = hw2 & 0x7FFU;
                int32_t offset = (int32_t)((s << 20) | (j2 << 19) | (j1 << 18) |
                                           (imm6 << 12) | (imm11 << 1));
                if (s) {
                    offset |= (int32_t)0xFFE00000U;
                }
                uint32_t target = pc_val + (uint32_t)offset;
                size_t skip = a64_emit_b_cond_placeholder(b, cond ^ 1U);
                a64_emit_exit_branch_imm(b, target, false);
                a64_patch_b_cond(b, skip, b->count);
                return true;
            }
            if (hw1 == 0xF3BFU) {
                /* DMB / DSB / ISB / CLREX */
                unsigned op = (hw2 >> 4) & 0xFU;
                if (op == 2U) {
                    a64_emit_clrex(b);
                } else if (op == 4U) {
                    a64_emit_dsb_ish(b);
                } else if (op == 5U) {
                    a64_emit_dmb_ish(b);
                } else if (op == 6U) {
                    a64_emit_isb(b);
                } else {
                    return false;
                }
                return true;
            }
            if (hw1 == 0xF3AFU) {
                /* NOP.W / hints */
                return true;
            }
            if (hw1 == 0xF3EFU && (hw2 & 0xF0FFU) == 0x8000U) {
                /* MRS Rd, CPSR */
                a64_emit_mrs_cpsr(b, (hw2 >> 8) & 0xFU);
                return true;
            }
            if ((hw1 & 0xFFF0U) == 0xF380U && (hw2 & 0xF0FFU) == 0x8000U &&
                ((hw2 >> 8) & 0xFU) != 0U) {
                /* MSR APSR/CPSR_<fields>, Rn */
                unsigned rn = hw1 & 0xFU;
                unsigned src = a32_load_reg(b, rn, pc_val, A64_TMP0);
                a64_emit_msr_cpsr_reg(b, src, (hw2 >> 8) & 0xFU);
                return true;
            }
        }
    }

    /* 2. Data-processing (modified immediate): (hw1 & 0xFA00) == 0xF000 && (hw2 & 0x8000) == 0 */
    if ((hw1 & 0xFA00U) == 0xF000U && (hw2 & 0x8000U) == 0U) {
        unsigned op = (hw1 >> 5) & 0xFU;
        bool set_flags = ((hw1 >> 4) & 1U) != 0U;
        unsigned rn = hw1 & 0xFU;
        unsigned rd = (hw2 >> 8) & 0xFU;

        /* Intra-block dead-NZCV flag liveness elimination */
        if (set_flags && rd != 15U && op != 0xAU && op != 0xBU) {
            if (!flags_live) {
                set_flags = false;
                ctx->flags_elided++;
            }
        }
        uint32_t imm12 = (((hw1 >> 10) & 1U) << 11) | (((hw2 >> 12) & 7U) << 8) | (hw2 & 0xFFU);
        bool has_carry = false;
        unsigned carry = 0U;
        uint32_t imm32 = thumb_expand_imm(imm12, &has_carry, &carry);
        a64_emit_mov_imm32(b, A64_TMP1, imm32);

        unsigned src_n = (op != 0x2U && op != 0x3U) ? a32_load_reg(b, rn, pc_val, A64_TMP2) : rn;
        if (emit_t32_dp_op(b, op, set_flags, rd, rn, src_n, A64_TMP1,
                           has_carry, carry, A64_ZR, is_uncond, out_ends_block)) {
            return true;
        }
    }

    /* 3. Data-processing (plain binary immediate): (hw1 & 0xFA00) == 0xF200 && (hw2 & 0x8000) == 0 */
    if ((hw1 & 0xFA00U) == 0xF200U && (hw2 & 0x8000U) == 0U) {
        unsigned op = (hw1 >> 4) & 0x1FU;
        unsigned rn = hw1 & 0xFU;
        unsigned rd = (hw2 >> 8) & 0xFU;
        uint16_t imm12 = (uint16_t)((((hw1 >> 10) & 1U) << 11) |
                                    (((hw2 >> 12) & 7U) << 8) | (hw2 & 0xFFU));
        uint16_t imm16 = (uint16_t)(((rn & 0xFU) << 12) | imm12);

        if (op == 0x00U) {
            /* ADDW Rd, Rn, #imm12 / ADR.W (when Rn == 15) */
            if (rn == 15U) {
                a64_emit_mov_pc32(b, rd, pc_align4 + imm12);
            } else {
                a64_emit_add_imm_w(b, false, rd, rn, imm12, 0U);
            }
            return true;
        }
        if (op == 0x04U) {
            /* MOVW Rd, #imm16 */
            a64_emit_movz_w(b, rd, imm16, 0U);
            return true;
        }
        if (op == 0x0AU) {
            /* SUBW Rd, Rn, #imm12 / ADR.W (when Rn == 15) */
            if (rn == 15U) {
                a64_emit_mov_pc32(b, rd, pc_align4 - imm12);
            } else {
                a64_emit_sub_imm_w(b, false, rd, rn, imm12, 0U);
            }
            return true;
        }
        if (op == 0x0CU) {
            /* MOVT Rd, #imm16 */
            a64_emit_movk_w(b, rd, imm16, 1U);
            return true;
        }
        if (op == 0x16U) {
            /* BFI / BFC */
            unsigned lsb = (((hw2 >> 12) & 7U) << 2) | ((hw2 >> 6) & 3U);
            unsigned msb = hw2 & 31U;
            if (msb < lsb) {
                return false;
            }
            unsigned src = (rn == 15U) ? A64_ZR : rn;
            a64_emit_bfm_w(b, rd, src, (32U - lsb) & 31U, msb - lsb);
            return true;
        }
        if (op == 0x14U) {
            /* SBFX */
            unsigned lsb = (((hw2 >> 12) & 7U) << 2) | ((hw2 >> 6) & 3U);
            unsigned widthm1 = hw2 & 31U;
            if (lsb + widthm1 > 31U) {
                return false;
            }
            a64_emit_sbfm_w(b, rd, rn, lsb, lsb + widthm1);
            return true;
        }
        if (op == 0x1CU) {
            /* UBFX */
            unsigned lsb = (((hw2 >> 12) & 7U) << 2) | ((hw2 >> 6) & 3U);
            unsigned widthm1 = hw2 & 31U;
            if (lsb + widthm1 > 31U) {
                return false;
            }
            a64_emit_ubfm_w(b, rd, rn, lsb, lsb + widthm1);
            return true;
        }
        if (op == 0x10U || op == 0x12U) {
            /* SSAT / SSAT16 */
            bool is_asr = (op == 0x12U);
            unsigned shift_imm = (((hw2 >> 12) & 7U) << 2) | ((hw2 >> 6) & 3U);
            unsigned src = a32_load_reg(b, rn, pc_val, A64_TMP2);
            if (is_asr && shift_imm == 0U) {
                a64_emit_sat16(b, true, rd, (hw2 & 15U) + 1U, src);
                return true;
            }
            a64_emit_ssat(b, rd, (hw2 & 31U) + 1U, src, is_asr, shift_imm);
            return true;
        }
        if (op == 0x18U || op == 0x1AU) {
            /* USAT / USAT16 */
            bool is_asr = (op == 0x1AU);
            unsigned shift_imm = (((hw2 >> 12) & 7U) << 2) | ((hw2 >> 6) & 3U);
            unsigned src = a32_load_reg(b, rn, pc_val, A64_TMP2);
            if (is_asr && shift_imm == 0U) {
                a64_emit_sat16(b, false, rd, hw2 & 15U, src);
                return true;
            }
            a64_emit_usat(b, rd, hw2 & 31U, src, is_asr, shift_imm);
            return true;
        }
    }

    /* 4. Data-processing (shifted register): (hw1 & 0xFE00) == 0xEA00 */
    if ((hw1 & 0xFE00U) == 0xEA00U) {
        if ((hw1 & 0xFFF0U) == 0xEAC0U && (hw2 & 0x8010U) == 0U) {
            /* PKHBT / PKHTB Rd, Rn, Rm {, LSL/ASR #imm} */
            unsigned rn = hw1 & 0xFU;
            unsigned rd = (hw2 >> 8) & 0xFU;
            unsigned rm = hw2 & 0xFU;
            unsigned shift_imm = (((hw2 >> 12) & 7U) << 2) | ((hw2 >> 6) & 3U);
            bool is_tb = ((hw2 >> 5) & 1U) != 0U;
            a64_emit_pkh(b, is_tb, rd, rn, rm, shift_imm);
            return true;
        }

        unsigned op = (hw1 >> 5) & 0xFU;
        bool set_flags = ((hw1 >> 4) & 1U) != 0U;
        unsigned rn = hw1 & 0xFU;
        unsigned rd = (hw2 >> 8) & 0xFU;
        unsigned rm = hw2 & 0xFU;

        /* Intra-block dead-NZCV flag liveness elimination */
        if (set_flags && rd != 15U && op != 0xAU && op != 0xBU) {
            if (!flags_live) {
                set_flags = false;
                ctx->flags_elided++;
            }
        }
        unsigned shift_type = (hw2 >> 4) & 3U;
        unsigned imm5 = (((hw2 >> 12) & 7U) << 2) | ((hw2 >> 6) & 3U);

        unsigned shift_carry_reg = A64_ZR;
        unsigned src_m = a32_load_reg(b, rm, pc_val, A64_TMP1);
        a64_emit_shift_imm_w(b, A64_TMP1, src_m, shift_type, imm5,
                             set_flags && op <= 0x4U, &shift_carry_reg);
        src_m = A64_TMP1;

        unsigned src_n = (op != 0x2U && op != 0x3U) ? a32_load_reg(b, rn, pc_val, A64_TMP0) : rn;
        if (emit_t32_dp_op(b, op, set_flags, rd, rn, src_n, src_m,
                           false, 0U, shift_carry_reg, is_uncond, out_ends_block)) {
            return true;
        }
    }

    /* 5. Data-processing (register, multiply, divide): (hw1 & 0xFE00) == 0xFA00 */
    if ((hw1 & 0xFE00U) == 0xFA00U) {
        unsigned rn = hw1 & 0xFU;
        unsigned rd = (hw2 >> 8) & 0xFU;
        unsigned rm = hw2 & 0xFU;
        if ((hw1 & 0xFF80U) == 0xFA00U && (hw2 & 0xF0F0U) == 0xF000U) {
            /* LSL.W / LSR.W / ASR.W / ROR.W Rd, Rn, Rm */
            unsigned shift_type = (hw1 >> 5) & 3U;
            bool set_flags = ((hw1 >> 4) & 1U) != 0U;
            /* Intra-block dead-NZCV flag liveness elimination */
            if (set_flags && rd != 15U) {
                if (!flags_live) {
                    set_flags = false;
                    ctx->flags_elided++;
                }
            }
            a64_emit_arm_shift_reg_w(b, rd, rn, rm, shift_type, set_flags);
            if (set_flags) {
                a64_emit_update_nzc_keep_v_reg(b, rd, A64_TMP2);
            }
            return true;
        }
        if ((hw1 & 0xFF80U) == 0xFA00U && (hw2 & 0xF0C0U) == 0xF080U) {
            /* SXTH / UXTH / SXTB16 / UXTB16 / SXTB / UXTB (+ add variants) */
            unsigned op = (hw1 >> 4) & 7U;
            if (op > 5U) {
                return false;
            }
            a64_emit_xt_add(b, (op & 1U) == 0U, op < 2U, op == 2U || op == 3U,
                            rd, rn, rm, ((hw2 >> 4) & 3U) * 8U);
            return true;
        }
        if ((hw1 & 0xFFF0U) == 0xFA80U && (hw2 & 0xF0C0U) == 0xF080U) {
            /* QADD / QDADD / QSUB / QDSUB Rd, Rm, Rn */
            unsigned subop = (hw2 >> 4) & 3U;
            a64_emit_qaddsub(b, rd, rm, rn, (subop & 2U) != 0U, (subop & 1U) != 0U);
            return true;
        }
        if ((hw1 & 0xFFF0U) == 0xFAB0U && (hw2 & 0xF0C0U) == 0xF080U) {
            /* CLZ Rd, Rm */
            a64_emit_clz_w(b, rd, rm);
            return true;
        }
        if ((hw1 & 0xFFE0U) == 0xFAC0U && (hw2 & 0xF0C0U) == 0xF080U) {
            /* CRC32{B,H,W} / CRC32C{B,H,W} Rd, Rn, Rm */
            unsigned c_bit = (hw1 >> 4) & 1U;
            unsigned sz    = (hw2 >> 4) & 3U;
            if (rd == 13U || rd == 15U || rn == 13U || rn == 15U ||
                rm == 13U || rm == 15U || sz == 3U) {
                return false;
            }
            a64_emit_crc32_w(b, rd, rn, rm, sz, c_bit);
            return true;
        }
        if ((hw1 & 0xFFF0U) == 0xFA90U && (hw2 & 0xF0C0U) == 0xF080U) {
            /* REV / REV16 / RBIT / REVSH */
            unsigned subop = (hw2 >> 4) & 3U;
            if (subop == 0U) {
                a64_emit_rev_w(b, rd, rm);
            } else if (subop == 1U) {
                a64_emit_rev16_w(b, rd, rm);
            } else if (subop == 2U) {
                a64_emit_rbit_w(b, rd, rm);
            } else {
                a64_emit_rev16_w(b, rd, rm);
                a64_emit_sbfm_w(b, rd, rd, 0U, 15U);
            }
            return true;
        }
        if ((hw1 & 0xFFF0U) == 0xFAA0U && (hw2 & 0xF0F0U) == 0xF080U) {
            /* SEL Rd, Rn, Rm */
            a64_emit_sel(b, rd, rn, rm);
            return true;
        }
        if (((hw1 & 0xFFF0U) == 0xFA80U || (hw1 & 0xFFF0U) == 0xFAC0U ||
             (hw1 & 0xFFF0U) == 0xFA90U || (hw1 & 0xFFF0U) == 0xFAD0U ||
             (hw1 & 0xFFF0U) == 0xFAA0U || (hw1 & 0xFFF0U) == 0xFAE0U) &&
            (hw2 & 0xF080U) == 0xF000U && ((hw2 >> 4) & 7U) != 3U && ((hw2 >> 4) & 7U) != 7U) {
            unsigned op1 = (hw1 >> 4) & 7U;
            unsigned op2 = (hw2 >> 4) & 7U;
            bool is_16 = (op1 != 0U && op1 != 4U);
            bool swap_rm = is_16 && (op1 == 2U || op1 == 6U);
            unsigned sub_mask = 0U;
            if (!is_16) {
                sub_mask = ((hw1 & 0x0040U) != 0U) ? 0xFU : 0U;
            } else if (op1 == 5U) {
                sub_mask = 0x3U;
            } else if (op1 == 2U) {
                sub_mask = 0x1U;
            } else if (op1 == 6U) {
                sub_mask = 0x2U;
            }
            a64_emit_parallel_addsub(b, op2, is_16, swap_rm, sub_mask, rd, rn, rm);
            return true;
        }
        if ((hw1 & 0xFFF0U) == 0xFB00U) {
            /* MUL / MLA / MLS */
            unsigned ra = (hw2 >> 12) & 0xFU;
            unsigned subop = (hw2 >> 4) & 0xFU;
            if (subop == 0U) {
                a64_emit_madd_w(b, rd, rn, rm, (ra == 15U) ? A64_ZR : ra);
            } else if (subop == 1U) {
                a64_emit_msub_w(b, rd, rn, rm, ra);
            } else {
                return false;
            }
            return true;
        }
        if ((hw1 & 0xFFF0U) == 0xFB10U && (hw2 & 0x00C0U) == 0U) {
            /* SMUL<x><y> and SMLA<x><y> */
            a64_emit_smla_xy(b, rd, rn, rm, (hw2 >> 12) & 0xFU,
                             ((hw2 >> 5) & 1U) != 0U, ((hw2 >> 4) & 1U) != 0U);
            return true;
        }
        if (((hw1 & 0xFFF0U) == 0xFB20U || (hw1 & 0xFFF0U) == 0xFB40U) && (hw2 & 0x00E0U) == 0U) {
            /* SMUAD{X} / SMUSD{X} / SMLAD{X} / SMLSD{X} */
            a64_emit_smlad(b, rd, rn, rm, (hw2 >> 12) & 0xFU,
                           (hw1 & 0xFFF0U) == 0xFB40U, ((hw2 >> 4) & 1U) != 0U);
            return true;
        }
        if ((hw1 & 0xFFF0U) == 0xFB30U && (hw2 & 0x00E0U) == 0U) {
            /* SMULW<y> and SMLAW<y> */
            a64_emit_smlaw_y(b, rd, rn, rm, (hw2 >> 12) & 0xFU, ((hw2 >> 4) & 1U) != 0U);
            return true;
        }
        if (((hw1 & 0xFFF0U) == 0xFB50U || (hw1 & 0xFFF0U) == 0xFB60U) && (hw2 & 0x00E0U) == 0U) {
            /* SMMUL{R} / SMMLA{R} / SMMLS{R} */
            a64_emit_smmla(b, rd, rn, rm, (hw2 >> 12) & 0xFU,
                           (hw1 & 0xFFF0U) == 0xFB60U, ((hw2 >> 4) & 1U) != 0U);
            return true;
        }
        if ((hw1 & 0xFFF0U) == 0xFB70U && (hw2 & 0x00F0U) == 0U) {
            /* USAD8 / USADA8 */
            a64_emit_usad8(b, rd, rn, rm, (hw2 >> 12) & 0xFU);
            return true;
        }
        if ((hw1 & 0xFF80U) == 0xFB80U) {
            /* SMULL / SDIV / UMULL / UDIV / SMLAL / SMLAL<x><y> / SMLALD / SMLSLD / UMLAL / UMAAL */
            unsigned op = (hw1 >> 4) & 7U;
            unsigned rd_lo = (hw2 >> 12) & 0xFU;
            unsigned subop = (hw2 >> 4) & 0xFU;
            if (op == 1U && subop == 0xFU) {
                a64_emit_sdiv_w(b, rd, rn, rm);
                return true;
            }
            if (op == 3U && subop == 0xFU) {
                a64_emit_udiv_w(b, rd, rn, rm);
                return true;
            }
            if (op == 6U && subop == 6U) {
                /* UMAAL RdLo, RdHi, Rn, Rm */
                a64_emit_umaal(b, rd_lo, rd, rn, rm);
                return true;
            }
            if (op == 4U && (subop & 0xCU) == 8U) {
                /* SMLAL<x><y> RdLo, RdHi, Rn, Rm */
                a64_emit_smlal_xy(b, rd_lo, rd, rn, rm,
                                  ((subop >> 1) & 1U) != 0U, (subop & 1U) != 0U);
                return true;
            }
            if ((op == 4U || op == 5U) && (subop & 0xEU) == 0xCU) {
                /* SMLALD{X} / SMLSLD{X} RdLo, RdHi, Rn, Rm */
                a64_emit_smlald(b, rd_lo, rd, rn, rm, op == 5U, (subop & 1U) != 0U);
                return true;
            }
            if (subop != 0U || (op != 0U && op != 2U && op != 4U && op != 6U)) {
                return false;
            }
            a64_emit_mull(b, op == 0U || op == 4U, op >= 4U, false, rd_lo, rd, rn, rm);
            return true;
        }
    }

    return a32_translate_t32_mem_insn(b, hw1, hw2, pc_val, pc_align4, is_uncond, out_ends_block);
}
