/*
 * src/interp/interp_t16.c — T16 (Thumb-1) instruction decoder and AArch64 lowering
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "interp/decode_priv.h"
extern uint8_t a32_pred_cond_mask(unsigned cond);

bool a32_translate_t16_insn(a32_ctx_t *ctx, a64_buf_t *b, uint32_t pc, uint16_t insn,
                            bool in_it, uint32_t itstate, uint32_t next_itstate, bool is_uncond,
                            bool flags_live, bool *out_ends_block)
{
    uint32_t pc_val = pc + 4U;
    uint32_t pc_align4 = pc_val & ~3U;
    bool set_flags = !in_it;

    /* Intra-block dead-NZCV flag liveness elimination */
    bool sets_flags_conditionally = false;
    if ((insn >> 14) == 0U) {
        unsigned op = (insn >> 11) & 7U;
        if (op != 5U) { /* not CMP Rn, #imm8 */
            sets_flags_conditionally = true;
        }
    } else if ((insn >> 10) == 0x10U) {
        unsigned alu_op = (insn >> 6) & 0xFU;
        /* Exclude ADC (0x5), SBC (0x6), TST (0x8), CMP (0xA), CMN (0xB) */
        if (alu_op != 0x5U && alu_op != 0x6U && alu_op != 0x8U &&
            alu_op != 0xAU && alu_op != 0xBU) {
            sets_flags_conditionally = true;
        }
    }

    if (sets_flags_conditionally && !in_it && !flags_live) {
        set_flags = false;
        ctx->flags_elided++;
    }

    /* 1. Shift immediate, add, subtract, move, compare: bits 15:14 == 00 */
    if ((insn >> 14) == 0U) {
        unsigned op = (insn >> 11) & 7U;
        if (op < 3U) {
            /* LSL / LSR / ASR Rd, Rm, #imm5 */
            unsigned imm5 = (insn >> 6) & 31U;
            unsigned rm   = (insn >> 3) & 7U;
            unsigned rd   = insn & 7U;
            unsigned carry_reg = A64_ZR;
            a64_emit_shift_imm_w(b, rd, rm, op, imm5, set_flags, &carry_reg);
            if (set_flags) {
                a64_emit_logical_flags(b, rd, false, 0U, carry_reg);
            }
            return true;
        }
        if (op == 3U) {
            /* ADD / SUB register or 3-bit immediate */
            bool is_imm = ((insn >> 10) & 1U) != 0U;
            bool is_sub = ((insn >> 9) & 1U) != 0U;
            unsigned rm_imm3 = (insn >> 6) & 7U;
            unsigned rn = (insn >> 3) & 7U;
            unsigned rd = insn & 7U;
            if (is_imm) {
                if (is_sub) {
                    a64_emit_sub_imm_w(b, set_flags, rd, rn, (uint16_t)rm_imm3, 0U);
                } else {
                    a64_emit_add_imm_w(b, set_flags, rd, rn, (uint16_t)rm_imm3, 0U);
                }
            } else {
                if (is_sub) {
                    a64_emit_sub_reg_w(b, set_flags, rd, rn, rm_imm3, A64_SHIFT_LSL, 0U);
                } else {
                    a64_emit_add_reg_w(b, set_flags, rd, rn, rm_imm3, A64_SHIFT_LSL, 0U);
                }
            }
            return true;
        }
        /* Move / compare / add / subtract 8-bit immediate (op == 4..7) */
        unsigned rdn = (insn >> 8) & 7U;
        uint16_t imm8 = (uint16_t)(insn & 0xFFU);
        if (op == 4U) {
            /* MOVS Rd, #imm8 */
            a64_emit_movz_w(b, rdn, imm8, 0U);
            if (set_flags) {
                a64_emit_update_nz_keep_cv(b, rdn);
            }
        } else if (op == 5U) {
            /* CMP Rn, #imm8 */
            a64_emit_sub_imm_w(b, true, A64_ZR, rdn, imm8, 0U);
        } else if (op == 6U) {
            /* ADDS Rdn, #imm8 */
            a64_emit_add_imm_w(b, set_flags, rdn, rdn, imm8, 0U);
        } else {
            /* SUBS Rdn, #imm8 */
            a64_emit_sub_imm_w(b, set_flags, rdn, rdn, imm8, 0U);
        }
        return true;
    }

    /* 2. 16-bit ALU operations: bits 15:10 == 010000 */
    if ((insn >> 10) == 0x10U) {
        unsigned alu_op = (insn >> 6) & 0xFU;
        unsigned rm = (insn >> 3) & 7U;
        unsigned rdn = insn & 7U;
        switch (alu_op) {
        case 0x0U: /* AND */
            a64_emit_and_reg_w(b, false, rdn, rdn, rm, A64_SHIFT_LSL, 0U);
            if (set_flags) {
                a64_emit_update_nz_keep_cv(b, rdn);
            }
            return true;
        case 0x1U: /* EOR */
            a64_emit_eor_reg_w(b, rdn, rdn, rm, A64_SHIFT_LSL, 0U);
            if (set_flags) {
                a64_emit_update_nz_keep_cv(b, rdn);
            }
            return true;
        case 0x2U: /* LSL */
            a64_emit_arm_shift_reg_w(b, rdn, rdn, rm, A64_SHIFT_LSL, set_flags);
            if (set_flags) {
                a64_emit_update_nzc_keep_v_reg(b, rdn, A64_TMP2);
            }
            return true;
        case 0x3U: /* LSR */
            a64_emit_arm_shift_reg_w(b, rdn, rdn, rm, A64_SHIFT_LSR, set_flags);
            if (set_flags) {
                a64_emit_update_nzc_keep_v_reg(b, rdn, A64_TMP2);
            }
            return true;
        case 0x4U: /* ASR */
            a64_emit_arm_shift_reg_w(b, rdn, rdn, rm, A64_SHIFT_ASR, set_flags);
            if (set_flags) {
                a64_emit_update_nzc_keep_v_reg(b, rdn, A64_TMP2);
            }
            return true;
        case 0x5U: /* ADC */
            a64_emit_adc_w(b, set_flags, rdn, rdn, rm);
            return true;
        case 0x6U: /* SBC */
            a64_emit_sbc_w(b, set_flags, rdn, rdn, rm);
            return true;
        case 0x7U: /* ROR */
            a64_emit_arm_shift_reg_w(b, rdn, rdn, rm, A64_SHIFT_ROR, set_flags);
            if (set_flags) {
                a64_emit_update_nzc_keep_v_reg(b, rdn, A64_TMP2);
            }
            return true;
        case 0x8U: /* TST */
            a64_emit_and_reg_w(b, false, A64_TMP0, rdn, rm, A64_SHIFT_LSL, 0U);
            a64_emit_update_nz_keep_cv(b, A64_TMP0);
            return true;
        case 0x9U: /* RSB / NEG Rd, Rm (#0 - Rm) */
            a64_emit_sub_reg_w(b, set_flags, rdn, A64_ZR, rm, A64_SHIFT_LSL, 0U);
            return true;
        case 0xAU: /* CMP */
            a64_emit_sub_reg_w(b, true, A64_ZR, rdn, rm, A64_SHIFT_LSL, 0U);
            return true;
        case 0xBU: /* CMN */
            a64_emit_add_reg_w(b, true, A64_ZR, rdn, rm, A64_SHIFT_LSL, 0U);
            return true;
        case 0xCU: /* ORR */
            a64_emit_orr_reg_w(b, rdn, rdn, rm, A64_SHIFT_LSL, 0U);
            if (set_flags) {
                a64_emit_update_nz_keep_cv(b, rdn);
            }
            return true;
        case 0xDU: /* MUL */
            a64_emit_madd_w(b, rdn, rm, rdn, A64_ZR);
            if (set_flags) {
                a64_emit_update_nz_keep_cv(b, rdn);
            }
            return true;
        case 0xEU: /* BIC */
            a64_emit_bic_reg_w(b, false, rdn, rdn, rm, A64_SHIFT_LSL, 0U);
            if (set_flags) {
                a64_emit_update_nz_keep_cv(b, rdn);
            }
            return true;
        case 0xFU: /* MVN */
            a64_emit_mvn_reg_w(b, rdn, rm, A64_SHIFT_LSL, 0U);
            if (set_flags) {
                a64_emit_update_nz_keep_cv(b, rdn);
            }
            return true;
        default:
            break;
        }
    }

    /* 3. Special data instructions and branch-and-exchange: bits 15:10 == 010001 */
    if ((insn >> 10) == 0x11U) {
        unsigned op = (insn >> 8) & 3U;
        unsigned rm = (insn >> 3) & 0xFU;
        unsigned rdn = ((insn >> 4) & 8U) | (insn & 7U);
        if (op == 0U) {
            /* ADD Rdn, Rm */
            unsigned src_n = a32_load_reg(b, rdn, pc_val, A64_TMP0);
            unsigned src_m = a32_load_reg(b, rm, pc_val, A64_TMP1);
            unsigned dst = (rdn == 15U) ? A64_TMP0 : rdn;
            a64_emit_add_reg_w(b, false, dst, src_n, src_m, A64_SHIFT_LSL, 0U);
            if (rdn == 15U) {
                a64_emit_exit_branch_reg(b, A64_TMP0, false);
                if (is_uncond) {
                    *out_ends_block = true;
                }
            }
            return true;
        }
        if (op == 1U) {
            /* CMP Rn, Rm */
            unsigned src_n = a32_load_reg(b, rdn, pc_val, A64_TMP0);
            unsigned src_m = a32_load_reg(b, rm, pc_val, A64_TMP1);
            a64_emit_sub_reg_w(b, true, A64_ZR, src_n, src_m, A64_SHIFT_LSL, 0U);
            return true;
        }
        if (op == 2U) {
            /* MOV Rd, Rm (BranchWritePC when Rd == PC: stays in Thumb state) */
            unsigned src_m = a32_load_reg(b, rm, pc_val, A64_TMP1);
            if (rdn == 15U) {
                a64_emit_exit_branch_reg(b, src_m, false);
                if (is_uncond) {
                    *out_ends_block = true;
                }
            } else {
                if (is_uncond && rdn == 14U && rm == 15U) {
                    b->lr_written_from_pc = true;
                }
                a64_emit_mov_w(b, rdn, src_m);
            }
            return true;
        }
        /* BX Rm / BLX Rm (op == 3) */
        bool is_blx = ((insn >> 7) & 1U) != 0U;
        if ((insn & 7U) != 0U || (is_blx && rm == 15U)) {
            return false;
        }
        unsigned src_m = a32_load_reg(b, rm, pc_val, A64_TMP0);
        if (is_blx) {
            if (rm == 14U) {
                a64_emit_mov_w(b, A64_TMP0, src_m);
                src_m = A64_TMP0;
            }
            a64_emit_mov_pc32(b, 14U, (pc + 2U) | 1U);
            if (is_uncond) {
                b->is_call = true;
            }
        }
        a64_emit_exit_branch_reg(b, src_m, true);
        if (is_uncond) {
            *out_ends_block = true;
        }
        return true;
    }

    /* 4. LDR Rt, [PC, #imm8*4] (literal): bits 15:11 == 01001 */
    if ((insn >> 11) == 0x09U) {
        unsigned rt = (insn >> 8) & 7U;
        uint32_t addr = pc_align4 + ((uint32_t)(insn & 0xFFU) << 2);
        a64_emit_mov_pc32(b, A64_ADDR, addr);
        a64_emit_guest_ldr_w(b, rt, A64_ADDR);
        return true;
    }

    /* 5. Load/store register offset: bits 15:12 == 0101 */
    if ((insn >> 12) == 0x5U) {
        unsigned op = (insn >> 9) & 7U;
        unsigned rm = (insn >> 6) & 7U;
        unsigned rn = (insn >> 3) & 7U;
        unsigned rt = insn & 7U;
        a64_emit_add_reg_w(b, false, A64_ADDR, rn, rm, A64_SHIFT_LSL, 0U);
        switch (op) {
        case 0U: a64_emit_guest_str_w(b, rt, A64_ADDR); break;
        case 1U: a64_emit_guest_strh_w(b, rt, A64_ADDR); break;
        case 2U: a64_emit_guest_strb_w(b, rt, A64_ADDR); break;
        case 3U: a64_emit_guest_ldrsb_w(b, rt, A64_ADDR); break;
        case 4U: a64_emit_guest_ldr_w(b, rt, A64_ADDR); break;
        case 5U: a64_emit_guest_ldrh_w(b, rt, A64_ADDR); break;
        case 6U: a64_emit_guest_ldrb_w(b, rt, A64_ADDR); break;
        default: a64_emit_guest_ldrsh_w(b, rt, A64_ADDR); break;
        }
        return true;
    }

    /* 6. Load/store word/byte immediate offset: bits 15:13 == 011 */
    if ((insn >> 13) == 0x3U) {
        bool is_byte = ((insn >> 12) & 1U) != 0U;
        bool is_load = ((insn >> 11) & 1U) != 0U;
        unsigned imm5 = (insn >> 6) & 31U;
        unsigned rn   = (insn >> 3) & 7U;
        unsigned rt   = insn & 7U;
        uint16_t offset = (uint16_t)(is_byte ? imm5 : (imm5 << 2));
        a64_emit_add_imm_w(b, false, A64_ADDR, rn, offset, 0U);
        if (is_load) {
            if (is_byte) {
                a64_emit_guest_ldrb_w(b, rt, A64_ADDR);
            } else {
                a64_emit_guest_ldr_w(b, rt, A64_ADDR);
            }
        } else {
            if (is_byte) {
                a64_emit_guest_strb_w(b, rt, A64_ADDR);
            } else {
                a64_emit_guest_str_w(b, rt, A64_ADDR);
            }
        }
        return true;
    }

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
        uint32_t target = pc_val + ((uint32_t)imm11 << 1);
        a64_emit_exit_branch_imm(b, target, false);
        if (is_uncond) {
            *out_ends_block = true;
        }
        return true;
    }

    return false;
}

static bool t32_may_fault_raw(uint16_t hw1, uint16_t hw2, bool is_32bit)
{
    if (!is_32bit) {
        if ((hw1 & 0xFF00U) == 0xDF00U) {
            return true;
        }
        if ((hw1 & 0xFF00U) == 0xDE00U) {
            return true;
        }
        if ((hw1 & 0xFF00U) == 0xBE00U) {
            return true;
        }
        if ((hw1 >> 11) == 0x09U || (hw1 >> 12) == 0x5U) {
            return true;
        }
        if ((hw1 >> 13) == 0x3U || (hw1 >> 12) == 0x8U) {
            return true;
        }
        if ((hw1 >> 12) == 0x9U || (hw1 & 0xF000U) == 0xC000U) {
            return true;
        }
        if ((hw1 & 0xFE00U) == 0xB400U || (hw1 & 0xFE00U) == 0xBC00U) {
            return true;
        }
        return false;
    }
    uint8_t h1hi = (uint8_t)(hw1 >> 8);
    if (h1hi >= 0xECU && h1hi < 0xF0U) {
        return true;
    }
    if (h1hi >= 0xF3U) {
        return true;
    }
    if (h1hi == 0xF2U && (hw2 & 0x8000U) != 0U) {
        return true;
    }
    if ((hw1 & 0xF000U) == 0xE000U && (hw1 & 0x0E00U) == 0x0800U) {
        return true;
    }
    if ((hw1 & 0xFFF0U) == 0xEAC0U && (hw2 & 0x8010U) == 0U) {
        return true;
    }
    return false;
}

uint8_t a32_pred_use_t32(uint16_t hw1, uint16_t hw2, uint8_t itst, bool is32, bool isit)
{
    if (isit) {
        return 0U;
    }
    if (t32_may_fault_raw(hw1, hw2, is32)) {
        return 0xFU;
    }
    bool in_it = (itst & 0x0FU) != 0U;
    uint8_t use = 0U;
    if (!is32) {
        if ((hw1 >> 12) == 0xDU && ((hw1 >> 8) & 0xFU) < 14U) {
            use |= a32_pred_cond_mask((hw1 >> 8) & 0xFU);
        }
        if ((hw1 >> 10) == 0x10U) {
            unsigned ao = (hw1 >> 6) & 0xFU;
            if (ao == 0x5U || ao == 0x6U) {
                use |= 0x2U;
            }
        }
        if (in_it) {
            use |= a32_pred_cond_mask((itst >> 4) & 0xFU);
        }
        return use;
    }
    if ((hw1 & 0xF800U) == 0xF000U && (hw2 & 0xD000U) == 0x8000U) {
        unsigned c = (hw1 >> 6) & 0xFU;
        if (c < 14U) {
            return a32_pred_cond_mask(c);
        }
    }
    if (in_it) {
        use |= a32_pred_cond_mask((itst >> 4) & 0xFU);
    }
    if ((hw1 & 0xFA00U) == 0xF000U && (hw2 & 0x8000U) == 0U) {
        unsigned op = (hw1 >> 5) & 0xFU;
        if (op == 0xAU || op == 0xBU) {
            use |= 0x2U;
        }
    }
    if ((hw1 & 0xFE00U) == 0xEA00U) {
        unsigned op = (hw1 >> 5) & 0xFU;
        if (op == 0xAU || op == 0xBU) {
            use |= 0x2U;
        }
        bool rrx = (((hw2 >> 4) & 3U) == 3U && (((hw2 >> 12) & 7U) == 0U && ((hw2 >> 6) & 3U) == 0U));
        if (rrx) {
            use |= 0x2U;
        }
    }
    return use;
}

uint8_t a32_pred_kill_t32(uint16_t hw1, uint16_t hw2, uint8_t itst, bool is32, bool isit)
{
    if (isit || (itst & 0x0FU) != 0U) {
        return 0U;
    }
    if (!is32) {
        unsigned top5 = (hw1 >> 11) & 0x1FU;
        if (top5 == 0x03U || (top5 >= 0x05U && top5 <= 0x07U)) {
            return 0xFU;
        }
        return 0U;
    }
    if ((hw1 & 0xFA00U) == 0xF000U && (hw2 & 0x8000U) == 0U) {
        bool s = ((hw1 >> 4) & 1U) != 0U;
        unsigned op = (hw1 >> 5) & 0xFU;
        if (!s) {
            return 0U;
        }
        if (op == 0x8U || op == 0xDU || op == 0xEU) {
            return 0xFU;
        }
        if (op <= 0x4U) {
            return 0xCU;
        }
        return 0U;
    }
    if ((hw1 & 0xFE00U) == 0xEA00U) {
        bool s = ((hw1 >> 4) & 1U) != 0U;
        unsigned op = (hw1 >> 5) & 0xFU;
        if (!s) {
            return 0U;
        }
        bool rrx = (((hw2 >> 4) & 3U) == 3U && (((hw2 >> 12) & 7U) == 0U && ((hw2 >> 6) & 3U) == 0U));
        if (op == 0x8U || op == 0xDU || op == 0xEU) {
            return rrx ? 0xCU : 0xFU;
        }
        if (op <= 0x4U && !rrx) {
            unsigned st = (hw2 >> 4) & 3U;
            unsigned imm5 = (((hw2 >> 12) & 7U) << 2) | ((hw2 >> 6) & 3U);
            if (st == 0U && imm5 == 0U) {
                return 0xCU;
            }
            return 0xEU;
        }
        if (op <= 0x4U) {
            return 0xCU;
        }
    }
    return 0U;
}

bool a32_pred_mergeable_t32(uint16_t hw1, uint16_t hw2, uint8_t itst, bool is32, bool isit, unsigned *outc)
{
    if (isit) {
        return false;
    }
    bool in_it = (itst & 0x0FU) != 0U;
    unsigned cond = 14U;
    if (!is32 && !in_it) {
        return false;
    }
    if (in_it) {
        cond = (itst >> 4) & 0xFU;
        if (cond >= 14U) {
            return false;
        }
    } else if (is32 && (hw1 & 0xF800U) == 0xF000U && (hw2 & 0xD000U) == 0x8000U) {
        cond = (hw1 >> 6) & 0xFU;
        if (cond >= 14U) {
            return false;
        }
    } else {
        return false;
    }
    if (a32_pred_kill_t32(hw1, hw2, itst, is32, isit) != 0U) {
        return false;
    }
    if (!is32) {
        if ((hw1 >> 12) == 0xDU || (hw1 >> 11) == 0x1CU || (hw1 & 0xFF00U) == 0xDF00U) {
            return false;
        }
        if ((hw1 & 0xFF00U) == 0xBE00U || (hw1 & 0xFF00U) == 0xDE00U) {
            return false;
        }
        if (outc != NULL) {
            *outc = cond;
        }
        return true;
    }
    if ((hw1 & 0xFE00U) == 0xEA00U || (hw1 & 0xFA00U) == 0xF000U) {
        return false;
    }
    if ((hw1 & 0xFF00U) == 0xF800U) {
        unsigned rt = (hw2 >> 12) & 0xFU;
        if (rt == 15U) {
            return false;
        }
        if (outc != NULL) {
            *outc = cond;
        }
        return true;
    }
    if ((hw1 & 0xFE00U) == 0xE800U) {
        if ((hw2 & 0x8000U) != 0U) {
            return false;
        }
        if (outc != NULL) {
            *outc = cond;
        }
        return true;
    }
    return false;
}
