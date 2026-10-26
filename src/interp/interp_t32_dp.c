/*
 * src/interp/interp_t32_dp.c - split-TU part 2/3 of src/decode_t32.c (lines 629-1156).
 * Compiled only via the src/decode_t32.c wrapper; do not add to Makefile SRCS.
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

    /* 7. Load/store halfword immediate offset: bits 15:12 == 1000 */
    if ((insn >> 12) == 0x8U) {
        bool is_load = ((insn >> 11) & 1U) != 0U;
        uint16_t offset = (uint16_t)(((insn >> 6) & 31U) << 1);
        unsigned rn = (insn >> 3) & 7U;
        unsigned rt = insn & 7U;
        a64_emit_add_imm_w(b, false, A64_ADDR, rn, offset, 0U);
        if (is_load) {
            a64_emit_guest_ldrh_w(b, rt, A64_ADDR);
        } else {
            a64_emit_guest_strh_w(b, rt, A64_ADDR);
        }
        return true;
    }

    /* 8. Load/store SP-relative: bits 15:12 == 1001 */
    if ((insn >> 12) == 0x9U) {
        bool is_load = ((insn >> 11) & 1U) != 0U;
        unsigned rt  = (insn >> 8) & 7U;
        uint16_t offset = (uint16_t)((insn & 0xFFU) << 2);
        a64_emit_add_imm_w(b, false, A64_ADDR, 13U, offset, 0U);
        if (is_load) {
            a64_emit_guest_ldr_w(b, rt, A64_ADDR);
        } else {
            a64_emit_guest_str_w(b, rt, A64_ADDR);
        }
        return true;
    }

    /* 9. ADR Rd, label / ADD Rd, SP, #imm8*4: bits 15:12 == 1010 */
    if ((insn >> 12) == 0xAU) {
        bool from_sp = ((insn >> 11) & 1U) != 0U;
        unsigned rd  = (insn >> 8) & 7U;
        uint16_t offset = (uint16_t)((insn & 0xFFU) << 2);
        if (from_sp) {
            a64_emit_add_imm_w(b, false, rd, 13U, offset, 0U);
        } else {
            a64_emit_mov_pc32(b, rd, pc_align4 + offset);
        }
        return true;
    }

    /* 10. Miscellaneous 16-bit instructions: bits 15:12 == 1011 */
    if ((insn >> 12) == 0xBU) {
        if ((insn & 0xFF00U) == 0xB000U) {
            /* ADD SP, #imm7*4 / SUB SP, #imm7*4 */
            bool is_sub = ((insn >> 7) & 1U) != 0U;
            uint16_t imm = (uint16_t)((insn & 0x7FU) << 2);
            if (is_sub) {
                a64_emit_sub_imm_w(b, false, 13U, 13U, imm, 0U);
            } else {
                a64_emit_add_imm_w(b, false, 13U, 13U, imm, 0U);
            }
            return true;
        }
        if ((insn & 0xF500U) == 0xB100U) {
            /* CBZ / CBNZ Rn, label */
            bool is_cbnz = ((insn >> 11) & 1U) != 0U;
            unsigned rn = insn & 7U;
            uint32_t offset = (((insn >> 9) & 1U) << 6) | (((insn >> 3) & 31U) << 1);
            uint32_t target = pc_val + offset;
            size_t skip = a64_emit_cbz_w_placeholder(b, !is_cbnz, rn);
            a64_emit_exit_branch_imm(b, target, false);
            a64_patch_cbz_w(b, skip, b->count);
            return true;
        }
        if ((insn & 0xFF00U) == 0xB200U) {
            /* SXTH / SXTB / UXTH / UXTB */
            unsigned op = (insn >> 6) & 3U;
            unsigned rm = (insn >> 3) & 7U;
            unsigned rd = insn & 7U;
            if (op == 0U) {
                a64_emit_sbfm_w(b, rd, rm, 0U, 15U);
            } else if (op == 1U) {
                a64_emit_sbfm_w(b, rd, rm, 0U, 7U);
            } else if (op == 2U) {
                a64_emit_ubfm_w(b, rd, rm, 0U, 15U);
            } else {
                a64_emit_ubfm_w(b, rd, rm, 0U, 7U);
            }
            return true;
        }
        if ((insn & 0xFE00U) == 0xB400U) {
            /* PUSH {reglist, lr} */
            uint32_t mask = (insn & 0xFFU) | (((insn >> 8) & 1U) << 14);
            a64_emit_ldm_stm(b, 13U, mask, false, true, false, true, pc_val, is_uncond, out_ends_block);
            return true;
        }
        if ((insn & 0xFF00U) == 0xBA00U) {
            /* REV / REV16 / REVSH */
            unsigned op = (insn >> 6) & 3U;
            unsigned rm = (insn >> 3) & 7U;
            unsigned rd = insn & 7U;
            if (op == 0U) {
                a64_emit_rev_w(b, rd, rm);
            } else if (op == 1U) {
                a64_emit_rev16_w(b, rd, rm);
            } else if (op == 3U) {
                a64_emit_rev16_w(b, rd, rm);
                a64_emit_sbfm_w(b, rd, rd, 0U, 15U);
            } else {
                return false;
            }
            return true;
        }
        if ((insn & 0xFE00U) == 0xBC00U) {
            /* POP {reglist, pc} */
            uint32_t mask = (insn & 0xFFU) | (((insn >> 8) & 1U) << 15);
            a64_emit_ldm_stm(b, 13U, mask, true, false, true, true, pc_val, is_uncond, out_ends_block);
            return true;
        }
        if ((insn & 0xFF00U) == 0xBE00U) {
            /* BKPT #imm8 */
            a64_emit_exit_bkpt(b, pc);
            *out_ends_block = true;
            return true;
        }
        if ((insn & 0xFF0FU) == 0xBF00U) {
            /* NOP / hints */
            return true;
        }
    }

    /* 11. STMIA / LDMIA: bits 15:12 == 1100 */
    if ((insn >> 12) == 0xCU) {
        a64_emit_ldm_stm(b, (insn >> 8) & 7U, insn & 0xFFU, ((insn >> 11) & 1U) != 0U,
                         false, true, true, pc_val, is_uncond, out_ends_block);
        return true;
    }

    /* 12. Conditional branch B<cond> / SVC: bits 15:12 == 1101 */
    if ((insn >> 12) == 0xDU) {
        unsigned cond = (insn >> 8) & 0xFU;
        if (cond == 0xFU) {
            /* SVC #imm8 */
            if (next_itstate != 0U) {
                a64_emit_mov_imm32(b, A64_TMP0, next_itstate);
                a64_emit_str_imm_w(b, A64_TMP0, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, itstate));
            }
            a64_emit_exit_svc(b, pc + 2U, insn & 0xFFU);
            if (is_uncond) {
                *out_ends_block = true;
            }
            return true;
        }
        if (cond == 0xEU) {
            /* UDF #imm8 */
            if (in_it) {
                a64_emit_mov_imm32(b, A64_TMP0, itstate);
                a64_emit_str_imm_w(b, A64_TMP0, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, itstate));
            }
            a64_emit_exit_undef(b, pc);
            if (is_uncond) {
                *out_ends_block = true;
            }
            return true;
        }
        int32_t imm8 = (int32_t)(int8_t)(insn & 0xFFU);
        uint32_t target = pc_val + (uint32_t)(imm8 << 1);
        size_t skip = a64_emit_b_cond_placeholder(b, cond ^ 1U);
        a64_emit_exit_branch_imm(b, target, false);
        a64_patch_b_cond(b, skip, b->count);
        return true;
    }

    /* 13. Unconditional branch B: bits 15:11 == 11100 */
    if ((insn >> 11) == 0x1CU) {
        int32_t imm11 = (int32_t)(insn & 0x7FFU);
        if (imm11 & 0x400) {
            imm11 |= (int32_t)0xFFFFF800U;
        }
        uint32_t target = pc_val + (uint32_t)(imm11 << 1);
        a64_emit_exit_branch_imm(b, target, false);
        if (is_uncond) {
            *out_ends_block = true;
        }
        return true;
    }

    return false;
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

static bool translate_t32_insn(a32_ctx_t *ctx, a64_buf_t *b, uint32_t pc, uint16_t hw1, uint16_t hw2,
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
