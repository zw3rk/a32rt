/*
 * src/decode_t32.c — T32 (Thumb-1 / Thumb-2) instruction decoder and AArch64 lowering
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "a64_emit.h"

#include <stdio.h>

#define T32_MAX_BLOCK_INSNS 64U

static uint32_t ror32_t(uint32_t val, unsigned rot)
{
    rot &= 31U;
    if (rot == 0U) {
        return val;
    }
    return (val >> rot) | (val << (32U - rot));
}

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
    uint32_t res = ror32_t(unrotated, rot);
    *out_has_carry = true;
    *out_carry = (res >> 31) & 1U;
    return res;
}

static unsigned load_t32_reg(a64_buf_t *b, unsigned reg, uint32_t pc_val, unsigned scratch)
{
    if (reg == 15U) {
        a64_emit_mov_imm32(b, scratch, pc_val);
        return scratch;
    }
    return reg;
}

#define T32_MAX_PRESCAN_INSNS 128U

typedef struct {
    uint32_t pc;
    uint16_t hw1;
    uint16_t hw2;
    uint8_t  itstate;
    bool     is_32bit;
    bool     is_it;
} t32_insn_info_t;

static bool t16_insn_uncond_ends_block(uint16_t insn)
{
    /* 16-bit unconditional B (11100 imm11) */
    if ((insn >> 11) == 0x1CU) {
        return true;
    }
    /* BX Rm / BLX Rm (0100 0111 x Rm 000) */
    if ((insn & 0xFF07U) == 0x4700U) {
        return true;
    }
    /* SVC #imm8 / UDF #imm8 */
    if ((insn & 0xFF00U) == 0xDF00U || (insn & 0xFF00U) == 0xDE00U) {
        return true;
    }
    /* BKPT #imm8 */
    if ((insn & 0xFF00U) == 0xBE00U) {
        return true;
    }
    /* POP {..., pc} (1011 1101 reglist) */
    if ((insn & 0xFF00U) == 0xBD00U) {
        return true;
    }
    /* ADD pc, Rm (0x4487) / MOV pc, Rm (0x4687) */
    if ((insn & 0xFF87U) == 0x4487U || (insn & 0xFF87U) == 0x4687U) {
        return true;
    }
    return false;
}

static bool t32_insn_uncond_ends_block(uint16_t hw1, uint16_t hw2)
{
    /* B.W / BL / BLX */
    if ((hw1 & 0xF800U) == 0xF000U && (hw2 & 0x8000U) != 0U) {
        if ((hw2 & 0xD000U) == 0xD000U || (hw2 & 0xD000U) == 0xC000U ||
            (hw2 & 0xD000U) == 0x9000U) {
            return true;
        }
    }
    /* UDF.W #imm16 */
    if ((hw1 & 0xFFF0U) == 0xF7F0U && (hw2 & 0xF000U) == 0xA000U) {
        return true;
    }
    /* Table branch: TBB / TBH */
    if ((hw1 & 0xFFF0U) == 0xE8D0U && (hw2 & 0xFFE0U) == 0xF000U) {
        return true;
    }
    /* LDM.W / POP.W with PC */
    if ((hw1 & 0xFE40U) == 0xE800U &&
        (((hw1 >> 7) & 3U) == 1U || ((hw1 >> 7) & 3U) == 2U) &&
        ((hw1 >> 4) & 1U) != 0U && (hw2 & 0x8000U) != 0U) {
        return true;
    }
    /* LDR.W pc, ... */
    if ((hw1 & 0xFF00U) == 0xF800U) {
        unsigned size = (hw1 >> 5) & 3U;
        bool is_load = ((hw1 >> 4) & 1U) != 0U;
        unsigned rt = (hw2 >> 12) & 0xFU;
        if (is_load && size == 2U && rt == 15U) {
            return true;
        }
    }
    /* 32-bit modified immediate DP with Rd == 15 and S == 0 (ALUWritePC) */
    if ((hw1 & 0xFA00U) == 0xF000U && (hw2 & 0x8000U) == 0U) {
        bool s_bit = ((hw1 >> 4) & 1U) != 0U;
        unsigned rd = (hw2 >> 8) & 0xFU;
        if (rd == 15U && !s_bit) {
            return true;
        }
    }
    /* 32-bit shifted register DP with Rd == 15 and S == 0 (ALUWritePC) */
    if ((hw1 & 0xFE00U) == 0xEA00U && !((hw1 & 0xFFF0U) == 0xEAC0U && (hw2 & 0x8010U) == 0U)) {
        bool s_bit = ((hw1 >> 4) & 1U) != 0U;
        unsigned rd = (hw2 >> 8) & 0xFU;
        if (rd == 15U && !s_bit) {
            return true;
        }
    }
    return false;
}

static bool t32_insn_overwrites_all_flags(const t32_insn_info_t *info)
{
    if (info->itstate != 0U || info->is_it) {
        return false;
    }
    if (!info->is_32bit) {
        uint16_t hw1 = info->hw1;
        unsigned top5 = (hw1 >> 11) & 0x1FU;
        /* 16-bit ADD/SUB Rd, Rn, Rm/#imm3 */
        if (top5 == 0x03U) {
            return true;
        }
        /* 16-bit CMP Rn, #imm8 (0x05), ADDS Rdn, #imm8 (0x06), SUBS Rdn, #imm8 (0x07) */
        if (top5 >= 0x05U && top5 <= 0x07U) {
            return true;
        }
        /* 16-bit ALU RSBS/NEG (0x4240), CMP (0x4280), CMN (0x42C0) */
        uint16_t top10 = hw1 & 0xFFC0U;
        if (top10 == 0x4240U || top10 == 0x4280U || top10 == 0x42C0U) {
            return true;
        }
        /* 16-bit special CMP Rn, Rm */
        if ((hw1 & 0xFF00U) == 0x4500U) {
            return true;
        }
        return false;
    }

    uint16_t hw1 = info->hw1;
    uint16_t hw2 = info->hw2;

    /* 32-bit modified immediate with S=1 and op in {8, 0xD, 0xE} */
    if ((hw1 & 0xFA00U) == 0xF000U && (hw2 & 0x8000U) == 0U) {
        bool s_bit = ((hw1 >> 4) & 1U) != 0U;
        unsigned op = (hw1 >> 5) & 0xFU;
        if (s_bit && (op == 0x8U || op == 0xDU || op == 0xEU)) {
            return true;
        }
    }

    /* 32-bit shifted register with S=1, op in {8, 0xD, 0xE}, and not RRX */
    if ((hw1 & 0xFE00U) == 0xEA00U && !((hw1 & 0xFFF0U) == 0xEAC0U && (hw2 & 0x8010U) == 0U)) {
        bool s_bit = ((hw1 >> 4) & 1U) != 0U;
        unsigned op = (hw1 >> 5) & 0xFU;
        bool is_rrx = (((hw2 >> 4) & 3U) == 3U && (((hw2 >> 12) & 7U) == 0U && ((hw2 >> 6) & 3U) == 0U));
        if (s_bit && (op == 0x8U || op == 0xDU || op == 0xEU) && !is_rrx) {
            return true;
        }
    }

    /* VMRS APSR_nzcv, FPSCR */
    if (hw1 == 0xEEF1U && hw2 == 0xFA10U) {
        return true;
    }

    /* MSR APSR/CPSR_f, Rn */
    if ((hw1 & 0xFFF0U) == 0xF380U && (hw2 & 0xF3FFU) == 0x8000U && (hw2 & 0x0800U) != 0U) {
        return true;
    }

    return false;
}

static bool t32_insn_reads_flags(const t32_insn_info_t *info)
{
    uint16_t hw1 = info->hw1;
    if (!info->is_32bit) {
        /* B<cond> (cond < 14) */
        if ((hw1 >> 12) == 0xDU && ((hw1 >> 8) & 0xFU) < 14U) {
            return true;
        }
        /* CBZ / CBNZ mid-block exit must preserve live NZCV for the taken target */
        if ((hw1 & 0xF500U) == 0xB100U) {
            return true;
        }
        /* 16-bit ALU ADC (5), SBC (6), or register shifts outside IT (2, 3, 4, 7) */
        if ((hw1 >> 10) == 0x10U) {
            unsigned alu_op = (hw1 >> 6) & 0xFU;
            if (alu_op == 0x5U || alu_op == 0x6U) {
                return true;
            }
            if (info->itstate == 0U &&
                (alu_op == 0x2U || alu_op == 0x3U || alu_op == 0x4U || alu_op == 0x7U)) {
                return true;
            }
        }
        return false;
    }

    uint16_t hw2 = info->hw2;

    /* B<cond>.W */
    if ((hw1 & 0xF800U) == 0xF000U && (hw2 & 0xD000U) == 0x8000U) {
        unsigned cond = (hw1 >> 6) & 0xFU;
        if (cond < 14U) {
            return true;
        }
    }

    /* 32-bit modified immediate ADC (op == 0xA) or SBC (op == 0xB) */
    if ((hw1 & 0xFA00U) == 0xF000U && (hw2 & 0x8000U) == 0U) {
        unsigned op = (hw1 >> 5) & 0xFU;
        if (op == 0xAU || op == 0xBU) {
            return true;
        }
    }

    /* 32-bit shifted register ADC (op == 0xA), SBC (op == 0xB), or RRX shift */
    if ((hw1 & 0xFE00U) == 0xEA00U && !((hw1 & 0xFFF0U) == 0xEAC0U && (hw2 & 0x8010U) == 0U)) {
        unsigned op = (hw1 >> 5) & 0xFU;
        if (op == 0xAU || op == 0xBU) {
            return true;
        }
        bool is_rrx = (((hw2 >> 4) & 3U) == 3U && (((hw2 >> 12) & 7U) == 0U && ((hw2 >> 6) & 3U) == 0U));
        if (is_rrx) {
            return true;
        }
    }

    /* 32-bit register shift with S=1 (LSLS.W, LSRS.W, ASRS.W, RORS.W) reads old_C when Rm[7:0] == 0 */
    if ((hw1 & 0xFF90U) == 0xFA10U && (hw2 & 0xF0F0U) == 0xF000U) {
        return true;
    }

    /* MRS Rd, CPSR */
    if (hw1 == 0xF3EFU && (hw2 & 0xF0FFU) == 0x8000U) {
        return true;
    }

    return false;
}

static bool t32_insn_partially_updates_flags(const t32_insn_info_t *info)
{
    uint16_t hw1 = info->hw1;
    if (!info->is_32bit) {
        unsigned top5 = (hw1 >> 11) & 0x1FU;
        /* LSLS / LSRS / ASRS #imm5 */
        if ((hw1 >> 14) == 0U && top5 < 3U) {
            return true;
        }
        /* MOVS #imm8 */
        if (top5 == 0x04U) {
            return true;
        }
        /* 16-bit ALU operations */
        if ((hw1 >> 10) == 0x10U) {
            unsigned alu_op = (hw1 >> 6) & 0xFU;
            if (alu_op == 0x0U || alu_op == 0x1U || alu_op == 0x2U ||
                alu_op == 0x3U || alu_op == 0x4U || alu_op == 0x7U ||
                alu_op == 0x8U || alu_op == 0xCU || alu_op == 0xDU ||
                alu_op == 0xEU || alu_op == 0xFU) {
                return true;
            }
        }
        return false;
    }

    uint16_t hw2 = info->hw2;

    /* 32-bit modified immediate with S=1: ANDS, BICS, ORRS, ORNS, EORS (op 0..4) */
    if ((hw1 & 0xFA00U) == 0xF000U && (hw2 & 0x8000U) == 0U) {
        bool s_bit = ((hw1 >> 4) & 1U) != 0U;
        unsigned op = (hw1 >> 5) & 0xFU;
        if (s_bit && op <= 0x4U) {
            return true;
        }
    }

    /* 32-bit shifted register with S=1: ANDS, BICS, ORRS, ORNS, EORS (op 0..4) */
    if ((hw1 & 0xFE00U) == 0xEA00U && !((hw1 & 0xFFF0U) == 0xEAC0U && (hw2 & 0x8010U) == 0U)) {
        bool s_bit = ((hw1 >> 4) & 1U) != 0U;
        unsigned op = (hw1 >> 5) & 0xFU;
        if (s_bit && op <= 0x4U) {
            return true;
        }
    }

    /* 32-bit register shift with S=1: LSLS.W, LSRS.W, ASRS.W, RORS.W */
    if ((hw1 & 0xFF80U) == 0xFA00U && (hw2 & 0xF0F0U) == 0xF000U) {
        bool s_bit = ((hw1 >> 4) & 1U) != 0U;
        if (s_bit) {
            return true;
        }
    }

    return false;
}

static bool translate_t16_insn(a32_ctx_t *ctx, a64_buf_t *b, uint32_t pc, uint16_t insn,
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
            bool has_carry = false;
            if (op == 0U) {
                if (imm5 != 0U && set_flags) {
                    a64_emit_ubfm_w(b, A64_TMP2, rm, 32U - imm5, 32U - imm5);
                    has_carry = true;
                }
                a64_emit_orr_reg_w(b, rd, A64_ZR, rm, A64_SHIFT_LSL, imm5);
            } else if (op == 1U) {
                if (set_flags) {
                    unsigned bit = (imm5 == 0U) ? 31U : (imm5 - 1U);
                    a64_emit_ubfm_w(b, A64_TMP2, rm, bit, bit);
                    has_carry = true;
                }
                if (imm5 == 0U) {
                    a64_emit_movz_w(b, rd, 0U, 0U);
                } else {
                    a64_emit_orr_reg_w(b, rd, A64_ZR, rm, A64_SHIFT_LSR, imm5);
                }
            } else {
                if (set_flags) {
                    unsigned bit = (imm5 == 0U) ? 31U : (imm5 - 1U);
                    a64_emit_ubfm_w(b, A64_TMP2, rm, bit, bit);
                    has_carry = true;
                }
                a64_emit_orr_reg_w(b, rd, A64_ZR, rm, A64_SHIFT_ASR, imm5 == 0U ? 31U : imm5);
            }
            if (set_flags) {
                if (has_carry) {
                    a64_emit_update_nzc_keep_v_reg(b, rd, A64_TMP2);
                } else {
                    a64_emit_update_nz_keep_cv(b, rd);
                }
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
            unsigned src_n = load_t32_reg(b, rdn, pc_val, A64_TMP0);
            unsigned src_m = load_t32_reg(b, rm, pc_val, A64_TMP1);
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
            unsigned src_n = load_t32_reg(b, rdn, pc_val, A64_TMP0);
            unsigned src_m = load_t32_reg(b, rm, pc_val, A64_TMP1);
            a64_emit_sub_reg_w(b, true, A64_ZR, src_n, src_m, A64_SHIFT_LSL, 0U);
            return true;
        }
        if (op == 2U) {
            /* MOV Rd, Rm (BranchWritePC when Rd == PC: stays in Thumb state) */
            unsigned src_m = load_t32_reg(b, rm, pc_val, A64_TMP1);
            if (rdn == 15U) {
                a64_emit_exit_branch_reg(b, src_m, false);
                if (is_uncond) {
                    *out_ends_block = true;
                }
            } else {
                if (is_uncond && rdn == 14U && rm == 15U) {
                    b->is_call = true;
                }
                a64_emit_mov_w(b, rdn, src_m);
            }
            return true;
        }
        /* BX Rm / BLX Rm (op == 3) */
        bool is_blx = ((insn >> 7) & 1U) != 0U;
        unsigned src_m = load_t32_reg(b, rm, pc_val, A64_TMP0);
        a64_emit_mov_w(b, A64_TMP0, src_m);
        if (is_blx) {
            a64_emit_mov_imm32(b, 14U, (pc + 2U) | 1U);
            if (is_uncond) {
                b->is_call = true;
            }
        }
        a64_emit_exit_branch_reg(b, A64_TMP0, true);
        if (is_uncond) {
            *out_ends_block = true;
        }
        return true;
    }

    /* 4. LDR Rt, [PC, #imm8*4] (literal): bits 15:11 == 01001 */
    if ((insn >> 11) == 0x09U) {
        unsigned rt = (insn >> 8) & 7U;
        uint32_t addr = pc_align4 + ((uint32_t)(insn & 0xFFU) << 2);
        a64_emit_mov_imm32(b, A64_ADDR, addr);
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
            a64_emit_mov_imm32(b, rd, pc_align4 + offset);
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
            unsigned count = 0U;
            for (unsigned i = 0U; i < 15U; i++) {
                if (mask & (1U << i)) {
                    count++;
                }
            }
            uint16_t bytes = (uint16_t)(count * 4U);
            a64_emit_sub_imm_w(b, false, A64_ADDR, 13U, bytes, 0U);
            for (unsigned i = 0U; i < 15U; i++) {
                if (mask & (1U << i)) {
                    a64_emit_guest_str_w(b, i, A64_ADDR);
                    a64_emit_add_imm_w(b, false, A64_ADDR, A64_ADDR, 4U, 0U);
                }
            }
            a64_emit_sub_imm_w(b, false, 13U, 13U, bytes, 0U);
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
            a64_emit_mov_w(b, A64_ADDR, 13U);
            for (unsigned i = 0U; i < 16U; i++) {
                if (mask & (1U << i)) {
                    unsigned dst = (i == 15U) ? A64_TMP0 : i;
                    a64_emit_guest_ldr_w(b, dst, A64_ADDR);
                    a64_emit_add_imm_w(b, false, A64_ADDR, A64_ADDR, 4U, 0U);
                }
            }
            a64_emit_mov_w(b, 13U, A64_ADDR);
            if (mask & (1U << 15)) {
                a64_emit_exit_branch_reg(b, A64_TMP0, true);
                if (is_uncond) {
                    *out_ends_block = true;
                }
            }
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
        bool is_load = ((insn >> 11) & 1U) != 0U;
        unsigned rn = (insn >> 8) & 7U;
        uint32_t mask = insn & 0xFFU;
        a64_emit_mov_w(b, A64_ADDR, rn);
        for (unsigned i = 0U; i < 8U; i++) {
            if (mask & (1U << i)) {
                if (is_load) {
                    unsigned dst = (i == rn) ? A64_TMP1 : i;
                    a64_emit_guest_ldr_w(b, dst, A64_ADDR);
                } else {
                    a64_emit_guest_str_w(b, i, A64_ADDR);
                }
                a64_emit_add_imm_w(b, false, A64_ADDR, A64_ADDR, 4U, 0U);
            }
        }
        if (is_load && (mask & (1U << rn)) != 0U) {
            a64_emit_mov_w(b, rn, A64_TMP1);
        } else {
            a64_emit_mov_w(b, rn, A64_ADDR);
        }
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

static bool translate_t32_insn(a32_ctx_t *ctx, a64_buf_t *b, uint32_t pc, uint16_t hw1, uint16_t hw2,
                               bool is_uncond, bool flags_live, bool *out_ends_block)
{
    /* M4: VFPv4-D32 / NEON delegation */
    if ((hw1 & 0xEF00U) == 0xEF00U ||
        (hw1 & 0xFF10U) == 0xF900U ||
        (((hw1 & 0xEE00U) == 0xEC00U || (hw1 & 0xFF00U) == 0xEE00U) &&
         (((hw2 >> 8) & 0xEU) == 0xAU))) {
        return a32_decode_vfp_neon_t32(ctx, b, hw1, hw2, pc);
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
                a64_emit_mov_imm32(b, 14U, (pc + 4U) | 1U);
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
                unsigned rd = (hw2 >> 8) & 0xFU;
                a64_emit_mrs_nzcv(b, A64_TMP0);
                a64_emit_ldr_imm_w(b, rd, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, cpsr));
                a64_emit_ubfm_w(b, rd, rd, 0U, 27U);
                a64_emit_movz_w(b, A64_TMP1, 0xF000U, 1U);
                a64_emit_and_reg_w(b, false, A64_TMP0, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
                a64_emit_orr_reg_w(b, rd, rd, A64_TMP0, A64_SHIFT_LSL, 0U);
                return true;
            }
            if ((hw1 & 0xFFF0U) == 0xF380U && (hw2 & 0xF3FFU) == 0x8000U &&
                ((hw2 >> 10) & 3U) != 0U) {
                /* MSR APSR/CPSR_<fields>, Rn */
                unsigned rn = hw1 & 0xFU;
                unsigned src = load_t32_reg(b, rn, pc_val, A64_TMP0);
                if ((hw2 & 0x0800U) != 0U) {
                    a64_emit_movz_w(b, A64_TMP1, 0xF000U, 1U);
                    a64_emit_and_reg_w(b, false, A64_TMP1, src, A64_TMP1, A64_SHIFT_LSL, 0U);
                    a64_emit_msr_nzcv(b, A64_TMP1);
                    a64_emit_ldr_imm_w(b, A64_TMP1, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, cpsr));
                    a64_emit_ubfm_w(b, A64_TMP2, src, 27U, 27U);
                    a64_emit_bfm_w(b, A64_TMP1, A64_TMP2, 5U, 0U);
                    a64_emit_str_imm_w(b, A64_TMP1, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, cpsr));
                }
                if ((hw2 & 0x0400U) != 0U) {
                    a64_emit_ldr_imm_w(b, A64_TMP1, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, cpsr));
                    a64_emit_ubfm_w(b, A64_TMP2, src, 16U, 19U);
                    a64_emit_bfm_w(b, A64_TMP1, A64_TMP2, 16U, 3U);
                    a64_emit_str_imm_w(b, A64_TMP1, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, cpsr));
                }
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

        unsigned src_n = (op != 0x2U && op != 0x3U) ? load_t32_reg(b, rn, pc_val, A64_TMP2) : rn;
        unsigned dst = (rd == 15U) ? A64_TMP0 : rd;
        bool handled = false;

        switch (op) {
        case 0x0U: /* AND / TST */
            a64_emit_and_reg_w(b, false, dst, src_n, A64_TMP1, A64_SHIFT_LSL, 0U);
            if (set_flags) {
                if (has_carry) {
                    a64_emit_update_nzc_keep_v_imm(b, dst, carry);
                } else {
                    a64_emit_update_nz_keep_cv(b, dst);
                }
            }
            handled = true;
            break;
        case 0x1U: /* BIC */
            a64_emit_bic_reg_w(b, false, dst, src_n, A64_TMP1, A64_SHIFT_LSL, 0U);
            if (set_flags) {
                if (has_carry) {
                    a64_emit_update_nzc_keep_v_imm(b, dst, carry);
                } else {
                    a64_emit_update_nz_keep_cv(b, dst);
                }
            }
            handled = true;
            break;
        case 0x2U: /* ORR / MOV */
            if (rn == 15U) {
                a64_emit_mov_w(b, dst, A64_TMP1);
            } else {
                a64_emit_orr_reg_w(b, dst, src_n, A64_TMP1, A64_SHIFT_LSL, 0U);
            }
            if (set_flags) {
                if (has_carry) {
                    a64_emit_update_nzc_keep_v_imm(b, dst, carry);
                } else {
                    a64_emit_update_nz_keep_cv(b, dst);
                }
            }
            handled = true;
            break;
        case 0x3U: /* ORN / MVN */
            if (rn == 15U) {
                a64_emit_mvn_reg_w(b, dst, A64_TMP1, A64_SHIFT_LSL, 0U);
            } else {
                a64_emit_orn_reg_w(b, dst, src_n, A64_TMP1, A64_SHIFT_LSL, 0U);
            }
            if (set_flags) {
                if (has_carry) {
                    a64_emit_update_nzc_keep_v_imm(b, dst, carry);
                } else {
                    a64_emit_update_nz_keep_cv(b, dst);
                }
            }
            handled = true;
            break;
        case 0x4U: /* EOR / TEQ */
            a64_emit_eor_reg_w(b, dst, src_n, A64_TMP1, A64_SHIFT_LSL, 0U);
            if (set_flags) {
                if (has_carry) {
                    a64_emit_update_nzc_keep_v_imm(b, dst, carry);
                } else {
                    a64_emit_update_nz_keep_cv(b, dst);
                }
            }
            handled = true;
            break;
        case 0x8U: /* ADD / CMN */
            a64_emit_add_reg_w(b, set_flags, (rd == 15U && set_flags) ? A64_ZR : dst,
                               src_n, A64_TMP1, A64_SHIFT_LSL, 0U);
            handled = true;
            break;
        case 0xAU: /* ADC */
            a64_emit_adc_w(b, set_flags, dst, src_n, A64_TMP1);
            handled = true;
            break;
        case 0xBU: /* SBC */
            a64_emit_sbc_w(b, set_flags, dst, src_n, A64_TMP1);
            handled = true;
            break;
        case 0xDU: /* SUB / CMP */
            a64_emit_sub_reg_w(b, set_flags, (rd == 15U && set_flags) ? A64_ZR : dst,
                               src_n, A64_TMP1, A64_SHIFT_LSL, 0U);
            handled = true;
            break;
        case 0xEU: /* RSB */
            a64_emit_sub_reg_w(b, set_flags, dst, A64_TMP1, src_n, A64_SHIFT_LSL, 0U);
            handled = true;
            break;
        default:
            break;
        }
        if (handled) {
            if (rd == 15U && !set_flags) {
                a64_emit_exit_branch_reg(b, dst, false);
                if (is_uncond) {
                    *out_ends_block = true;
                }
            }
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
                a64_emit_mov_imm32(b, rd, pc_align4 + imm12);
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
                a64_emit_mov_imm32(b, rd, pc_align4 - imm12);
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
            unsigned src = (rn == 15U) ? A64_ZR : rn;
            a64_emit_bfm_w(b, rd, src, (32U - lsb) & 31U, msb - lsb);
            return true;
        }
        if (op == 0x14U) {
            /* SBFX */
            unsigned lsb = (((hw2 >> 12) & 7U) << 2) | ((hw2 >> 6) & 3U);
            unsigned widthm1 = hw2 & 31U;
            a64_emit_sbfm_w(b, rd, rn, lsb, lsb + widthm1);
            return true;
        }
        if (op == 0x1CU) {
            /* UBFX */
            unsigned lsb = (((hw2 >> 12) & 7U) << 2) | ((hw2 >> 6) & 3U);
            unsigned widthm1 = hw2 & 31U;
            a64_emit_ubfm_w(b, rd, rn, lsb, lsb + widthm1);
            return true;
        }
        if (op == 0x10U || op == 0x12U) {
            /* SSAT / SSAT16 */
            bool is_asr = (op == 0x12U);
            unsigned shift_imm = (((hw2 >> 12) & 7U) << 2) | ((hw2 >> 6) & 3U);
            unsigned src = load_t32_reg(b, rn, pc_val, A64_TMP2);

            if (is_asr && shift_imm == 0U) {
                /* SSAT16 Rd, #sat_imm, Rn */
                unsigned sat_bits16 = (hw2 & 15U) + 1U;
                int32_t max_val = (int32_t)((1U << (sat_bits16 - 1U)) - 1U);
                int32_t min_val = -(int32_t)(1U << (sat_bits16 - 1U));
                a64_emit_mrs_nzcv(b, A64_TMP3);
                a64_emit_mov_imm32(b, A64_TMP1, (uint32_t)max_val);
                a64_emit_mov_imm32(b, A64_ADDR, (uint32_t)min_val);

                a64_emit_sbfm_w(b, A64_TMP0, src, 0U, 15U);
                a64_emit_sub_reg_w(b, true, A64_ZR, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
                a64_emit_csel_w(b, A64_TMP0, A64_TMP1, A64_TMP0, A64_COND_GT);
                a64_emit_sub_reg_w(b, true, A64_ZR, A64_TMP0, A64_ADDR, A64_SHIFT_LSL, 0U);
                a64_emit_csel_w(b, A64_TMP0, A64_ADDR, A64_TMP0, A64_COND_LT);

                a64_emit_sbfm_w(b, A64_TMP2, src, 16U, 31U);
                a64_emit_sub_reg_w(b, true, A64_ZR, A64_TMP2, A64_TMP1, A64_SHIFT_LSL, 0U);
                a64_emit_csel_w(b, A64_TMP2, A64_TMP1, A64_TMP2, A64_COND_GT);
                a64_emit_sub_reg_w(b, true, A64_ZR, A64_TMP2, A64_ADDR, A64_SHIFT_LSL, 0U);
                a64_emit_csel_w(b, A64_TMP2, A64_ADDR, A64_TMP2, A64_COND_LT);

                a64_emit_bfm_w(b, A64_TMP0, A64_TMP2, 16U, 15U);
                unsigned orig_src = load_t32_reg(b, rn, pc_val, A64_TMP1);
                a64_emit_sub_reg_w(b, true, A64_ZR, A64_TMP0, orig_src, A64_SHIFT_LSL, 0U);
                a64_emit_movz_w(b, A64_TMP1, 0x0800U, 1U);
                a64_emit_csel_w(b, A64_TMP1, A64_TMP1, A64_ZR, A64_COND_NE);
                a64_emit_ldr_imm_w(b, A64_TMP2, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, cpsr));
                a64_emit_orr_reg_w(b, A64_TMP2, A64_TMP2, A64_TMP1, A64_SHIFT_LSL, 0U);
                a64_emit_str_imm_w(b, A64_TMP2, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, cpsr));
                a64_emit_msr_nzcv(b, A64_TMP3);
                a64_emit_mov_w(b, rd, A64_TMP0);
                return true;
            }

            unsigned sat_bits = (hw2 & 31U) + 1U;
            if (is_asr) {
                a64_emit_orr_reg_w(b, A64_TMP0, A64_ZR, src, A64_SHIFT_ASR, shift_imm);
            } else if (shift_imm != 0U) {
                a64_emit_orr_reg_w(b, A64_TMP0, A64_ZR, src, A64_SHIFT_LSL, shift_imm);
            } else {
                a64_emit_mov_w(b, A64_TMP0, src);
            }

            if (sat_bits < 32U) {
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
                a64_emit_movz_w(b, A64_TMP1, 0x0800U, 1U);
                a64_emit_csel_w(b, A64_TMP1, A64_TMP1, A64_ZR, A64_COND_NE);
                a64_emit_ldr_imm_w(b, A64_TMP2, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, cpsr));
                a64_emit_orr_reg_w(b, A64_TMP2, A64_TMP2, A64_TMP1, A64_SHIFT_LSL, 0U);
                a64_emit_str_imm_w(b, A64_TMP2, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, cpsr));
                a64_emit_msr_nzcv(b, A64_TMP3);
            } else {
                a64_emit_mov_w(b, rd, A64_TMP0);
            }
            return true;
        }
        if (op == 0x18U || op == 0x1AU) {
            /* USAT / USAT16 */
            bool is_asr = (op == 0x1AU);
            unsigned shift_imm = (((hw2 >> 12) & 7U) << 2) | ((hw2 >> 6) & 3U);
            unsigned src = load_t32_reg(b, rn, pc_val, A64_TMP2);

            if (is_asr && shift_imm == 0U) {
                /* USAT16 Rd, #sat_imm, Rn */
                unsigned sat_bits16 = hw2 & 15U;
                uint32_t max_val = (1U << sat_bits16) - 1U;
                a64_emit_mrs_nzcv(b, A64_TMP3);
                a64_emit_mov_imm32(b, A64_TMP1, max_val);

                a64_emit_sbfm_w(b, A64_TMP0, src, 0U, 15U);
                a64_emit_sub_imm_w(b, true, A64_ZR, A64_TMP0, 0U, 0U);
                a64_emit_csel_w(b, A64_TMP0, A64_ZR, A64_TMP0, A64_COND_LT);
                a64_emit_sub_reg_w(b, true, A64_ZR, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
                a64_emit_csel_w(b, A64_TMP0, A64_TMP1, A64_TMP0, A64_COND_HI);

                a64_emit_sbfm_w(b, A64_TMP2, src, 16U, 31U);
                a64_emit_sub_imm_w(b, true, A64_ZR, A64_TMP2, 0U, 0U);
                a64_emit_csel_w(b, A64_TMP2, A64_ZR, A64_TMP2, A64_COND_LT);
                a64_emit_sub_reg_w(b, true, A64_ZR, A64_TMP2, A64_TMP1, A64_SHIFT_LSL, 0U);
                a64_emit_csel_w(b, A64_TMP2, A64_TMP1, A64_TMP2, A64_COND_HI);

                a64_emit_bfm_w(b, A64_TMP0, A64_TMP2, 16U, 15U);
                unsigned orig_src = load_t32_reg(b, rn, pc_val, A64_TMP1);
                a64_emit_sub_reg_w(b, true, A64_ZR, A64_TMP0, orig_src, A64_SHIFT_LSL, 0U);
                a64_emit_movz_w(b, A64_TMP1, 0x0800U, 1U);
                a64_emit_csel_w(b, A64_TMP1, A64_TMP1, A64_ZR, A64_COND_NE);
                a64_emit_ldr_imm_w(b, A64_TMP2, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, cpsr));
                a64_emit_orr_reg_w(b, A64_TMP2, A64_TMP2, A64_TMP1, A64_SHIFT_LSL, 0U);
                a64_emit_str_imm_w(b, A64_TMP2, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, cpsr));
                a64_emit_msr_nzcv(b, A64_TMP3);
                a64_emit_mov_w(b, rd, A64_TMP0);
                return true;
            }

            unsigned sat_bits = hw2 & 31U;
            if (is_asr) {
                a64_emit_orr_reg_w(b, A64_TMP0, A64_ZR, src, A64_SHIFT_ASR, shift_imm);
            } else if (shift_imm != 0U) {
                a64_emit_orr_reg_w(b, A64_TMP0, A64_ZR, src, A64_SHIFT_LSL, shift_imm);
            } else {
                a64_emit_mov_w(b, A64_TMP0, src);
            }

            uint32_t max_val = (sat_bits == 31U) ? 0x7FFFFFFFU : ((1U << sat_bits) - 1U);
            a64_emit_mrs_nzcv(b, A64_TMP3);
            a64_emit_sub_imm_w(b, true, A64_ZR, A64_TMP0, 0U, 0U);
            a64_emit_csel_w(b, A64_TMP2, A64_ZR, A64_TMP0, A64_COND_LT);
            a64_emit_mov_imm32(b, A64_TMP1, max_val);
            a64_emit_sub_reg_w(b, true, A64_ZR, A64_TMP2, A64_TMP1, A64_SHIFT_LSL, 0U);
            a64_emit_csel_w(b, rd, A64_TMP1, A64_TMP2, A64_COND_HI);
            a64_emit_sub_reg_w(b, true, A64_ZR, rd, A64_TMP0, A64_SHIFT_LSL, 0U);
            a64_emit_movz_w(b, A64_TMP1, 0x0800U, 1U);
            a64_emit_csel_w(b, A64_TMP1, A64_TMP1, A64_ZR, A64_COND_NE);
            a64_emit_ldr_imm_w(b, A64_TMP2, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, cpsr));
            a64_emit_orr_reg_w(b, A64_TMP2, A64_TMP2, A64_TMP1, A64_SHIFT_LSL, 0U);
            a64_emit_str_imm_w(b, A64_TMP2, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, cpsr));
            a64_emit_msr_nzcv(b, A64_TMP3);
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
            unsigned imm3 = (hw2 >> 12) & 7U;
            unsigned imm2 = (hw2 >> 6) & 3U;
            unsigned shift_imm = (imm3 << 2) | imm2;
            bool is_tb = ((hw2 >> 5) & 1U) != 0U;

            if (!is_tb) {
                /* PKHBT: Rd[15:0] = Rn[15:0], Rd[31:16] = (Rm << shift_imm)[31:16] */
                a64_emit_orr_reg_w(b, A64_TMP1, A64_ZR, rm, A64_SHIFT_LSL, shift_imm);
                a64_emit_bfm_w(b, A64_TMP1, rn, 0U, 15U);
                a64_emit_mov_w(b, rd, A64_TMP1);
            } else {
                /* PKHTB: Rd[15:0] = (Rm ASR shift_imm)[15:0], Rd[31:16] = Rn[31:16] */
                unsigned amt = (shift_imm == 0U) ? 31U : shift_imm;
                a64_emit_orr_reg_w(b, A64_TMP1, A64_ZR, rm, A64_SHIFT_ASR, amt);
                a64_emit_mov_w(b, A64_TMP0, rn);
                a64_emit_bfm_w(b, A64_TMP0, A64_TMP1, 0U, 15U);
                a64_emit_mov_w(b, rd, A64_TMP0);
            }
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

        /* Compute shifted rm into A64_TMP1 and shifter carry-out into A64_TMP2 */
        unsigned src_m = load_t32_reg(b, rm, pc_val, A64_TMP0);
        bool has_carry = false;
        if (shift_type == A64_SHIFT_LSL && imm5 == 0U) {
            a64_emit_mov_w(b, A64_TMP1, src_m);
        } else if (shift_type == A64_SHIFT_LSL) {
            if (set_flags) {
                a64_emit_ubfm_w(b, A64_TMP2, src_m, 32U - imm5, 32U - imm5);
                has_carry = true;
            }
            a64_emit_orr_reg_w(b, A64_TMP1, A64_ZR, src_m, A64_SHIFT_LSL, imm5);
        } else if (shift_type == A64_SHIFT_LSR) {
            if (set_flags) {
                unsigned bit = (imm5 == 0U) ? 31U : (imm5 - 1U);
                a64_emit_ubfm_w(b, A64_TMP2, src_m, bit, bit);
                has_carry = true;
            }
            if (imm5 == 0U) {
                a64_emit_movz_w(b, A64_TMP1, 0U, 0U);
            } else {
                a64_emit_orr_reg_w(b, A64_TMP1, A64_ZR, src_m, A64_SHIFT_LSR, imm5);
            }
        } else if (shift_type == A64_SHIFT_ASR) {
            if (set_flags) {
                unsigned bit = (imm5 == 0U) ? 31U : (imm5 - 1U);
                a64_emit_ubfm_w(b, A64_TMP2, src_m, bit, bit);
                has_carry = true;
            }
            a64_emit_orr_reg_w(b, A64_TMP1, A64_ZR, src_m, A64_SHIFT_ASR, imm5 == 0U ? 31U : imm5);
        } else {
            if (set_flags) {
                unsigned bit = (imm5 == 0U) ? 0U : (imm5 - 1U);
                a64_emit_ubfm_w(b, A64_TMP2, src_m, bit, bit);
                has_carry = true;
            }
            if (imm5 == 0U) {
                a64_emit_mrs_nzcv(b, A64_TMP3);
                a64_emit_ubfm_w(b, A64_TMP3, A64_TMP3, 29U, 29U);
                a64_emit_orr_reg_w(b, A64_TMP1, A64_ZR, src_m, A64_SHIFT_LSR, 1U);
                a64_emit_orr_reg_w(b, A64_TMP1, A64_TMP1, A64_TMP3, A64_SHIFT_LSL, 31U);
            } else {
                a64_emit_orr_reg_w(b, A64_TMP1, A64_ZR, src_m, A64_SHIFT_ROR, imm5);
            }
        }

        unsigned src_n = (op != 0x2U && op != 0x3U) ? load_t32_reg(b, rn, pc_val, A64_TMP0) : rn;
        unsigned dst = (rd == 15U) ? A64_TMP0 : rd;
        bool handled = false;

        switch (op) {
        case 0x0U: /* AND / TST */
            a64_emit_and_reg_w(b, false, dst, src_n, A64_TMP1, A64_SHIFT_LSL, 0U);
            if (set_flags) {
                if (has_carry) {
                    a64_emit_update_nzc_keep_v_reg(b, dst, A64_TMP2);
                } else {
                    a64_emit_update_nz_keep_cv(b, dst);
                }
            }
            handled = true;
            break;
        case 0x1U: /* BIC */
            a64_emit_bic_reg_w(b, false, dst, src_n, A64_TMP1, A64_SHIFT_LSL, 0U);
            if (set_flags) {
                if (has_carry) {
                    a64_emit_update_nzc_keep_v_reg(b, dst, A64_TMP2);
                } else {
                    a64_emit_update_nz_keep_cv(b, dst);
                }
            }
            handled = true;
            break;
        case 0x2U: /* ORR / MOV */
            if (rn == 15U) {
                a64_emit_mov_w(b, dst, A64_TMP1);
            } else {
                a64_emit_orr_reg_w(b, dst, src_n, A64_TMP1, A64_SHIFT_LSL, 0U);
            }
            if (set_flags) {
                if (has_carry) {
                    a64_emit_update_nzc_keep_v_reg(b, dst, A64_TMP2);
                } else {
                    a64_emit_update_nz_keep_cv(b, dst);
                }
            }
            handled = true;
            break;
        case 0x3U: /* ORN / MVN */
            if (rn == 15U) {
                a64_emit_mvn_reg_w(b, dst, A64_TMP1, A64_SHIFT_LSL, 0U);
            } else {
                a64_emit_orn_reg_w(b, dst, src_n, A64_TMP1, A64_SHIFT_LSL, 0U);
            }
            if (set_flags) {
                if (has_carry) {
                    a64_emit_update_nzc_keep_v_reg(b, dst, A64_TMP2);
                } else {
                    a64_emit_update_nz_keep_cv(b, dst);
                }
            }
            handled = true;
            break;
        case 0x4U: /* EOR / TEQ */
            a64_emit_eor_reg_w(b, dst, src_n, A64_TMP1, A64_SHIFT_LSL, 0U);
            if (set_flags) {
                if (has_carry) {
                    a64_emit_update_nzc_keep_v_reg(b, dst, A64_TMP2);
                } else {
                    a64_emit_update_nz_keep_cv(b, dst);
                }
            }
            handled = true;
            break;
        case 0x8U: /* ADD / CMN */
            a64_emit_add_reg_w(b, set_flags, (rd == 15U && set_flags) ? A64_ZR : dst,
                               src_n, A64_TMP1, A64_SHIFT_LSL, 0U);
            handled = true;
            break;
        case 0xAU: /* ADC */
            a64_emit_adc_w(b, set_flags, dst, src_n, A64_TMP1);
            handled = true;
            break;
        case 0xBU: /* SBC */
            a64_emit_sbc_w(b, set_flags, dst, src_n, A64_TMP1);
            handled = true;
            break;
        case 0xDU: /* SUB / CMP */
            a64_emit_sub_reg_w(b, set_flags, (rd == 15U && set_flags) ? A64_ZR : dst,
                               src_n, A64_TMP1, A64_SHIFT_LSL, 0U);
            handled = true;
            break;
        case 0xEU: /* RSB */
            a64_emit_sub_reg_w(b, set_flags, dst, A64_TMP1, src_n, A64_SHIFT_LSL, 0U);
            handled = true;
            break;
        default:
            break;
        }
        if (handled) {
            if (rd == 15U && !set_flags) {
                a64_emit_exit_branch_reg(b, dst, false);
                if (is_uncond) {
                    *out_ends_block = true;
                }
            }
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
            unsigned rot = ((hw2 >> 4) & 3U) * 8U;
            a64_emit_orr_reg_w(b, A64_TMP0, A64_ZR, rm, A64_SHIFT_ROR, rot);
            if (op == 2U || op == 3U) {
                if (op == 2U) {
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
                return true;
            }
            if (op == 0U) {
                a64_emit_sbfm_w(b, A64_TMP0, A64_TMP0, 0U, 15U);
            } else if (op == 1U) {
                a64_emit_ubfm_w(b, A64_TMP0, A64_TMP0, 0U, 15U);
            } else if (op == 4U) {
                a64_emit_sbfm_w(b, A64_TMP0, A64_TMP0, 0U, 7U);
            } else {
                a64_emit_ubfm_w(b, A64_TMP0, A64_TMP0, 0U, 7U);
            }
            if (rn != 15U) {
                a64_emit_add_reg_w(b, false, rd, rn, A64_TMP0, A64_SHIFT_LSL, 0U);
            } else {
                a64_emit_mov_w(b, rd, A64_TMP0);
            }
            return true;
        }
        if ((hw1 & 0xFFF0U) == 0xFA80U && (hw2 & 0xF0C0U) == 0xF080U) {
            /* QADD / QDADD / QSUB / QDSUB Rd, Rm, Rn */
            unsigned subop = (hw2 >> 4) & 3U;
            bool is_double = (subop & 1U) != 0U;
            bool is_sub = (subop & 2U) != 0U;
            a64_emit_mrs_nzcv(b, A64_TMP3);
            a64_emit_movz_w(b, A64_ADDR, 0x0800U, 1U);
            a64_emit_mov_w(b, A64_TMP0, rn);
            if (is_double) {
                a64_emit_add_reg_w(b, true, A64_TMP0, A64_TMP0, A64_TMP0, A64_SHIFT_LSL, 0U);
                a64_emit_movz_w(b, A64_TMP2, 0x8000U, 1U);
                a64_emit_eor_reg_w(b, A64_TMP2, A64_TMP2, A64_TMP0, A64_SHIFT_ASR, 31U);
                a64_emit_csel_w(b, A64_TMP0, A64_TMP2, A64_TMP0, A64_COND_VS);
                a64_emit_csel_w(b, A64_TMP1, A64_ADDR, A64_ZR, A64_COND_VS);
            }
            if (is_sub) {
                a64_emit_sub_reg_w(b, true, A64_TMP0, rm, A64_TMP0, A64_SHIFT_LSL, 0U);
            } else {
                a64_emit_add_reg_w(b, true, A64_TMP0, rm, A64_TMP0, A64_SHIFT_LSL, 0U);
            }
            a64_emit_movz_w(b, A64_TMP2, 0x8000U, 1U);
            a64_emit_eor_reg_w(b, A64_TMP2, A64_TMP2, A64_TMP0, A64_SHIFT_ASR, 31U);
            a64_emit_csel_w(b, rd, A64_TMP2, A64_TMP0, A64_COND_VS);
            a64_emit_csel_w(b, A64_TMP1, A64_ADDR, is_double ? A64_TMP1 : A64_ZR, A64_COND_VS);
            a64_emit_ldr_imm_w(b, A64_TMP2, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, cpsr));
            a64_emit_orr_reg_w(b, A64_TMP2, A64_TMP2, A64_TMP1, A64_SHIFT_LSL, 0U);
            a64_emit_str_imm_w(b, A64_TMP2, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, cpsr));
            a64_emit_msr_nzcv(b, A64_TMP3);
            return true;
        }
        if ((hw1 & 0xFFF0U) == 0xFAB0U && (hw2 & 0xF0C0U) == 0xF080U) {
            /* CLZ Rd, Rm */
            a64_emit_clz_w(b, rd, rm);
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
            a64_emit_mrs_nzcv(b, A64_TMP3);
            a64_emit_ldr_imm_w(b, A64_TMP2, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, cpsr));
            a64_emit_movz_w(b, A64_TMP0, 0U, 0U);
            for (unsigned i = 0U; i < 4U; i++) {
                a64_emit_ubfm_w(b, A64_TMP1, A64_TMP2, 16U + i, 16U + i);
                a64_emit_sub_imm_w(b, true, A64_ZR, A64_TMP1, 1U, 0U);
                a64_emit_mov_imm32(b, A64_TMP1, 0xFFU << (i * 8U));
                a64_emit_csel_w(b, A64_TMP1, A64_TMP1, A64_ZR, A64_COND_EQ);
                a64_emit_orr_reg_w(b, A64_TMP0, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
            }
            a64_emit_msr_nzcv(b, A64_TMP3);
            a64_emit_and_reg_w(b, false, A64_TMP1, rn, A64_TMP0, A64_SHIFT_LSL, 0U);
            a64_emit_bic_reg_w(b, false, rd, rm, A64_TMP0, A64_SHIFT_LSL, 0U);
            a64_emit_orr_reg_w(b, rd, rd, A64_TMP1, A64_SHIFT_LSL, 0U);
            return true;
        }
        if (((hw1 & 0xFFF0U) == 0xFA80U || (hw1 & 0xFFF0U) == 0xFAC0U ||
             (hw1 & 0xFFF0U) == 0xFA90U || (hw1 & 0xFFF0U) == 0xFAD0U) &&
            (hw2 & 0xF0F0U) == 0xF000U) {
            /* SADD8 / SSUB8 / SADD16 / SSUB16 Rd, Rn, Rm */
            bool is_16  = (hw1 & 0x0010U) != 0U;
            bool is_sub = (hw1 & 0x0040U) != 0U;
            a64_emit_mrs_nzcv(b, A64_TMP3);
            a64_emit_ldr_imm_w(b, A64_ADDR, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, cpsr));
            a64_emit_mov_imm32(b, A64_TMP0, 0x000F0000U);
            a64_emit_bic_reg_w(b, false, A64_ADDR, A64_ADDR, A64_TMP0, A64_SHIFT_LSL, 0U);
            a64_emit_movz_w(b, A64_TMP2, 0U, 0U);
            if (is_16) {
                for (unsigned i = 0U; i < 2U; i++) {
                    a64_emit_sbfm_w(b, A64_TMP0, rn, i * 16U, i * 16U + 15U);
                    a64_emit_sbfm_w(b, A64_TMP1, rm, i * 16U, i * 16U + 15U);
                    if (is_sub) {
                        a64_emit_sub_reg_w(b, true, A64_TMP0, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
                    } else {
                        a64_emit_add_reg_w(b, true, A64_TMP0, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
                    }
                    a64_emit_bfm_w(b, A64_TMP2, A64_TMP0, (32U - (i * 16U)) & 31U, 15U);
                    a64_emit_mov_imm32(b, A64_TMP1, 3U << (16U + i * 2U));
                    a64_emit_csel_w(b, A64_TMP1, A64_TMP1, A64_ZR, A64_COND_GE);
                    a64_emit_orr_reg_w(b, A64_ADDR, A64_ADDR, A64_TMP1, A64_SHIFT_LSL, 0U);
                }
            } else {
                for (unsigned i = 0U; i < 4U; i++) {
                    a64_emit_sbfm_w(b, A64_TMP0, rn, i * 8U, i * 8U + 7U);
                    a64_emit_sbfm_w(b, A64_TMP1, rm, i * 8U, i * 8U + 7U);
                    if (is_sub) {
                        a64_emit_sub_reg_w(b, true, A64_TMP0, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
                    } else {
                        a64_emit_add_reg_w(b, true, A64_TMP0, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
                    }
                    a64_emit_bfm_w(b, A64_TMP2, A64_TMP0, (32U - (i * 8U)) & 31U, 7U);
                    a64_emit_mov_imm32(b, A64_TMP1, 1U << (16U + i));
                    a64_emit_csel_w(b, A64_TMP1, A64_TMP1, A64_ZR, A64_COND_GE);
                    a64_emit_orr_reg_w(b, A64_ADDR, A64_ADDR, A64_TMP1, A64_SHIFT_LSL, 0U);
                }
            }
            a64_emit_str_imm_w(b, A64_ADDR, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, cpsr));
            a64_emit_msr_nzcv(b, A64_TMP3);
            a64_emit_mov_w(b, rd, A64_TMP2);
            return true;
        }
        if (((hw1 & 0xFFF0U) == 0xFA80U || (hw1 & 0xFFF0U) == 0xFAC0U ||
             (hw1 & 0xFFF0U) == 0xFA90U || (hw1 & 0xFFF0U) == 0xFAD0U) &&
            (hw2 & 0xF0F0U) == 0xF040U) {
            /* UADD8 / USUB8 / UADD16 / USUB16 Rd, Rn, Rm */
            bool is_16  = (hw1 & 0x0010U) != 0U;
            bool is_sub = (hw1 & 0x0040U) != 0U;
            a64_emit_mrs_nzcv(b, A64_TMP3);
            a64_emit_ldr_imm_w(b, A64_ADDR, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, cpsr));
            a64_emit_mov_imm32(b, A64_TMP0, 0x000F0000U);
            a64_emit_bic_reg_w(b, false, A64_ADDR, A64_ADDR, A64_TMP0, A64_SHIFT_LSL, 0U);
            a64_emit_movz_w(b, A64_TMP2, 0U, 0U);
            if (is_16) {
                for (unsigned i = 0U; i < 2U; i++) {
                    a64_emit_ubfm_w(b, A64_TMP0, rn, i * 16U, i * 16U + 15U);
                    a64_emit_ubfm_w(b, A64_TMP1, rm, i * 16U, i * 16U + 15U);
                    if (is_sub) {
                        a64_emit_sub_reg_w(b, true, A64_TMP0, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
                    } else {
                        a64_emit_add_reg_w(b, false, A64_TMP0, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
                        a64_emit_sub_imm_w(b, true, A64_ZR, A64_TMP0, 0x10U, 1U);
                    }
                    a64_emit_bfm_w(b, A64_TMP2, A64_TMP0, (32U - (i * 16U)) & 31U, 15U);
                    a64_emit_mov_imm32(b, A64_TMP1, 3U << (16U + i * 2U));
                    a64_emit_csel_w(b, A64_TMP1, A64_TMP1, A64_ZR, A64_COND_CS);
                    a64_emit_orr_reg_w(b, A64_ADDR, A64_ADDR, A64_TMP1, A64_SHIFT_LSL, 0U);
                }
            } else {
                for (unsigned i = 0U; i < 4U; i++) {
                    a64_emit_ubfm_w(b, A64_TMP0, rn, i * 8U, i * 8U + 7U);
                    a64_emit_ubfm_w(b, A64_TMP1, rm, i * 8U, i * 8U + 7U);
                    if (is_sub) {
                        a64_emit_sub_reg_w(b, true, A64_TMP0, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
                    } else {
                        a64_emit_add_reg_w(b, false, A64_TMP0, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
                        a64_emit_sub_imm_w(b, true, A64_ZR, A64_TMP0, 256U, 0U);
                    }
                    a64_emit_bfm_w(b, A64_TMP2, A64_TMP0, (32U - (i * 8U)) & 31U, 7U);
                    a64_emit_mov_imm32(b, A64_TMP1, 1U << (16U + i));
                    a64_emit_csel_w(b, A64_TMP1, A64_TMP1, A64_ZR, A64_COND_CS);
                    a64_emit_orr_reg_w(b, A64_ADDR, A64_ADDR, A64_TMP1, A64_SHIFT_LSL, 0U);
                }
            }
            a64_emit_str_imm_w(b, A64_ADDR, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, cpsr));
            a64_emit_msr_nzcv(b, A64_TMP3);
            a64_emit_mov_w(b, rd, A64_TMP2);
            return true;
        }
        if (((hw1 & 0xFFF0U) == 0xFA80U || (hw1 & 0xFFF0U) == 0xFAC0U ||
             (hw1 & 0xFFF0U) == 0xFA90U || (hw1 & 0xFFF0U) == 0xFAD0U) &&
            (hw2 & 0xF0F0U) == 0xF020U) {
            /* SHADD8 / SHSUB8 / SHADD16 / SHSUB16 Rd, Rn, Rm */
            bool is_16  = (hw1 & 0x0010U) != 0U;
            bool is_sub = (hw1 & 0x0040U) != 0U;
            a64_emit_movz_w(b, A64_TMP2, 0U, 0U);
            if (is_16) {
                for (unsigned i = 0U; i < 2U; i++) {
                    a64_emit_sbfm_w(b, A64_TMP0, rn, i * 16U, i * 16U + 15U);
                    a64_emit_sbfm_w(b, A64_TMP1, rm, i * 16U, i * 16U + 15U);
                    if (is_sub) {
                        a64_emit_sub_reg_w(b, false, A64_TMP1, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
                    } else {
                        a64_emit_add_reg_w(b, false, A64_TMP1, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
                    }
                    a64_emit_sbfm_w(b, A64_TMP1, A64_TMP1, 1U, 31U);
                    a64_emit_ubfm_w(b, A64_TMP1, A64_TMP1, 0U, 15U);
                    a64_emit_bfm_w(b, A64_TMP2, A64_TMP1, (32U - (i * 16U)) & 31U, 15U);
                }
            } else {
                for (unsigned i = 0U; i < 4U; i++) {
                    a64_emit_sbfm_w(b, A64_TMP0, rn, i * 8U, i * 8U + 7U);
                    a64_emit_sbfm_w(b, A64_TMP1, rm, i * 8U, i * 8U + 7U);
                    if (is_sub) {
                        a64_emit_sub_reg_w(b, false, A64_TMP1, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
                    } else {
                        a64_emit_add_reg_w(b, false, A64_TMP1, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
                    }
                    a64_emit_sbfm_w(b, A64_TMP1, A64_TMP1, 1U, 31U);
                    a64_emit_ubfm_w(b, A64_TMP1, A64_TMP1, 0U, 7U);
                    a64_emit_bfm_w(b, A64_TMP2, A64_TMP1, (32U - (i * 8U)) & 31U, 7U);
                }
            }
            a64_emit_mov_w(b, rd, A64_TMP2);
            return true;
        }
        if (((hw1 & 0xFFF0U) == 0xFA80U || (hw1 & 0xFFF0U) == 0xFAC0U ||
             (hw1 & 0xFFF0U) == 0xFA90U || (hw1 & 0xFFF0U) == 0xFAD0U) &&
            (hw2 & 0xF0F0U) == 0xF060U) {
            /* UHADD8 / UHSUB8 / UHADD16 / UHSUB16 Rd, Rn, Rm */
            bool is_16  = (hw1 & 0x0010U) != 0U;
            bool is_sub = (hw1 & 0x0040U) != 0U;
            a64_emit_movz_w(b, A64_TMP2, 0U, 0U);
            if (is_16) {
                for (unsigned i = 0U; i < 2U; i++) {
                    a64_emit_ubfm_w(b, A64_TMP0, rn, i * 16U, i * 16U + 15U);
                    a64_emit_ubfm_w(b, A64_TMP1, rm, i * 16U, i * 16U + 15U);
                    if (is_sub) {
                        a64_emit_sub_reg_w(b, false, A64_TMP1, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
                    } else {
                        a64_emit_add_reg_w(b, false, A64_TMP1, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
                    }
                    a64_emit_ubfm_w(b, A64_TMP1, A64_TMP1, 1U, 16U);
                    a64_emit_bfm_w(b, A64_TMP2, A64_TMP1, (32U - (i * 16U)) & 31U, 15U);
                }
            } else {
                for (unsigned i = 0U; i < 4U; i++) {
                    a64_emit_ubfm_w(b, A64_TMP0, rn, i * 8U, i * 8U + 7U);
                    a64_emit_ubfm_w(b, A64_TMP1, rm, i * 8U, i * 8U + 7U);
                    if (is_sub) {
                        a64_emit_sub_reg_w(b, false, A64_TMP1, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
                    } else {
                        a64_emit_add_reg_w(b, false, A64_TMP1, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
                    }
                    a64_emit_ubfm_w(b, A64_TMP1, A64_TMP1, 1U, 8U);
                    a64_emit_bfm_w(b, A64_TMP2, A64_TMP1, (32U - (i * 8U)) & 31U, 7U);
                }
            }
            a64_emit_mov_w(b, rd, A64_TMP2);
            return true;
        }
        if (((hw1 & 0xFFF0U) == 0xFA80U || (hw1 & 0xFFF0U) == 0xFAC0U ||
             (hw1 & 0xFFF0U) == 0xFA90U || (hw1 & 0xFFF0U) == 0xFAD0U) &&
            (hw2 & 0xF0F0U) == 0xF010U) {
            /* QADD8 / QSUB8 / QADD16 / QSUB16 Rd, Rn, Rm */
            bool is_16  = (hw1 & 0x0010U) != 0U;
            bool is_sub = (hw1 & 0x0040U) != 0U;
            if (flags_live) {
                a64_emit_mrs_nzcv(b, A64_TMP3);
            }
            a64_emit_movz_w(b, A64_TMP2, 0U, 0U);
            if (is_16) {
                a64_emit_mov_imm32(b, A64_ADDR, (uint32_t)-32768);
                for (unsigned i = 0U; i < 2U; i++) {
                    a64_emit_sbfm_w(b, A64_TMP0, rn, i * 16U, i * 16U + 15U);
                    a64_emit_sbfm_w(b, A64_TMP1, rm, i * 16U, i * 16U + 15U);
                    if (is_sub) {
                        a64_emit_sub_reg_w(b, false, A64_TMP0, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
                    } else {
                        a64_emit_add_reg_w(b, false, A64_TMP0, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
                    }
                    a64_emit_mov_imm32(b, A64_TMP1, 32767U);
                    a64_emit_sub_reg_w(b, true, A64_ZR, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
                    a64_emit_csel_w(b, A64_TMP0, A64_TMP1, A64_TMP0, A64_COND_GT);
                    a64_emit_sub_reg_w(b, true, A64_ZR, A64_TMP0, A64_ADDR, A64_SHIFT_LSL, 0U);
                    a64_emit_csel_w(b, A64_TMP0, A64_ADDR, A64_TMP0, A64_COND_LT);
                    a64_emit_bfm_w(b, A64_TMP2, A64_TMP0, (32U - (i * 16U)) & 31U, 15U);
                }
            } else {
                a64_emit_mov_imm32(b, A64_ADDR, (uint32_t)-128);
                for (unsigned i = 0U; i < 4U; i++) {
                    a64_emit_sbfm_w(b, A64_TMP0, rn, i * 8U, i * 8U + 7U);
                    a64_emit_sbfm_w(b, A64_TMP1, rm, i * 8U, i * 8U + 7U);
                    if (is_sub) {
                        a64_emit_sub_reg_w(b, false, A64_TMP0, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
                    } else {
                        a64_emit_add_reg_w(b, false, A64_TMP0, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
                    }
                    a64_emit_mov_imm32(b, A64_TMP1, 127U);
                    a64_emit_sub_reg_w(b, true, A64_ZR, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
                    a64_emit_csel_w(b, A64_TMP0, A64_TMP1, A64_TMP0, A64_COND_GT);
                    a64_emit_sub_reg_w(b, true, A64_ZR, A64_TMP0, A64_ADDR, A64_SHIFT_LSL, 0U);
                    a64_emit_csel_w(b, A64_TMP0, A64_ADDR, A64_TMP0, A64_COND_LT);
                    a64_emit_bfm_w(b, A64_TMP2, A64_TMP0, (32U - (i * 8U)) & 31U, 7U);
                }
            }
            if (flags_live) {
                a64_emit_msr_nzcv(b, A64_TMP3);
            }
            a64_emit_mov_w(b, rd, A64_TMP2);
            return true;
        }
        if (((hw1 & 0xFFF0U) == 0xFA80U || (hw1 & 0xFFF0U) == 0xFAC0U ||
             (hw1 & 0xFFF0U) == 0xFA90U || (hw1 & 0xFFF0U) == 0xFAD0U) &&
            (hw2 & 0xF0F0U) == 0xF050U) {
            /* UQADD8 / UQSUB8 / UQADD16 / UQSUB16 Rd, Rn, Rm */
            bool is_16  = (hw1 & 0x0010U) != 0U;
            bool is_sub = (hw1 & 0x0040U) != 0U;
            if (flags_live) {
                a64_emit_mrs_nzcv(b, A64_TMP3);
            }
            a64_emit_movz_w(b, A64_TMP2, 0U, 0U);
            if (is_16) {
                a64_emit_mov_imm32(b, A64_ADDR, 65535U);
                for (unsigned i = 0U; i < 2U; i++) {
                    a64_emit_ubfm_w(b, A64_TMP0, rn, i * 16U, i * 16U + 15U);
                    a64_emit_ubfm_w(b, A64_TMP1, rm, i * 16U, i * 16U + 15U);
                    if (is_sub) {
                        a64_emit_sub_reg_w(b, true, A64_TMP0, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
                        a64_emit_csel_w(b, A64_TMP0, A64_TMP0, A64_ZR, A64_COND_CS);
                    } else {
                        a64_emit_add_reg_w(b, false, A64_TMP0, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
                        a64_emit_sub_reg_w(b, true, A64_ZR, A64_TMP0, A64_ADDR, A64_SHIFT_LSL, 0U);
                        a64_emit_csel_w(b, A64_TMP0, A64_ADDR, A64_TMP0, A64_COND_HI);
                    }
                    a64_emit_bfm_w(b, A64_TMP2, A64_TMP0, (32U - (i * 16U)) & 31U, 15U);
                }
            } else {
                a64_emit_mov_imm32(b, A64_ADDR, 255U);
                for (unsigned i = 0U; i < 4U; i++) {
                    a64_emit_ubfm_w(b, A64_TMP0, rn, i * 8U, i * 8U + 7U);
                    a64_emit_ubfm_w(b, A64_TMP1, rm, i * 8U, i * 8U + 7U);
                    if (is_sub) {
                        a64_emit_sub_reg_w(b, true, A64_TMP0, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
                        a64_emit_csel_w(b, A64_TMP0, A64_TMP0, A64_ZR, A64_COND_CS);
                    } else {
                        a64_emit_add_reg_w(b, false, A64_TMP0, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
                        a64_emit_sub_reg_w(b, true, A64_ZR, A64_TMP0, A64_ADDR, A64_SHIFT_LSL, 0U);
                        a64_emit_csel_w(b, A64_TMP0, A64_ADDR, A64_TMP0, A64_COND_HI);
                    }
                    a64_emit_bfm_w(b, A64_TMP2, A64_TMP0, (32U - (i * 8U)) & 31U, 7U);
                }
            }
            if (flags_live) {
                a64_emit_msr_nzcv(b, A64_TMP3);
            }
            a64_emit_mov_w(b, rd, A64_TMP2);
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
            unsigned ra = (hw2 >> 12) & 0xFU;
            bool n_top = ((hw2 >> 5) & 1U) != 0U;
            bool m_top = ((hw2 >> 4) & 1U) != 0U;
            a64_emit_sbfm_w(b, A64_TMP0, rn, n_top ? 16U : 0U, n_top ? 31U : 15U);
            a64_emit_sbfm_w(b, A64_TMP1, rm, m_top ? 16U : 0U, m_top ? 31U : 15U);
            if (ra != 15U) {
                a64_emit_mrs_nzcv(b, A64_TMP3);
                a64_emit_madd_w(b, A64_TMP2, A64_TMP0, A64_TMP1, A64_ZR);
                a64_emit_add_reg_w(b, true, rd, A64_TMP2, ra, A64_SHIFT_LSL, 0U);
                a64_emit_movz_w(b, A64_TMP1, 0x0800U, 1U);
                a64_emit_csel_w(b, A64_TMP1, A64_TMP1, A64_ZR, A64_COND_VS);
                a64_emit_ldr_imm_w(b, A64_TMP2, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, cpsr));
                a64_emit_orr_reg_w(b, A64_TMP2, A64_TMP2, A64_TMP1, A64_SHIFT_LSL, 0U);
                a64_emit_str_imm_w(b, A64_TMP2, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, cpsr));
                a64_emit_msr_nzcv(b, A64_TMP3);
            } else {
                a64_emit_madd_w(b, rd, A64_TMP0, A64_TMP1, A64_ZR);
            }
            return true;
        }
        if (((hw1 & 0xFFF0U) == 0xFB20U || (hw1 & 0xFFF0U) == 0xFB40U) && (hw2 & 0x00E0U) == 0U) {
            /* SMUAD{X} / SMUSD{X} / SMLAD{X} / SMLSD{X} */
            bool is_sub = (hw1 & 0xFFF0U) == 0xFB40U;
            bool m_swap = ((hw2 >> 4) & 1U) != 0U;
            unsigned ra = (hw2 >> 12) & 0xFU;
            unsigned m1_lsb = m_swap ? 16U : 0U;
            unsigned m2_lsb = m_swap ? 0U : 16U;
            if (ra != 15U) {
                /* SXTW X17, W_ra */
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
                /* SXTW X15, W_rd */
                a64_emit(b, 0x93407C00U | ((rd & 31U) << 5) | A64_TMP0);
                /* SUBS XZR, X15, X17 */
                a64_emit(b, 0xEB000000U | (A64_TMP2 << 16) | (A64_TMP0 << 5) | A64_ZR);
                a64_emit_movz_w(b, A64_TMP1, 0x0800U, 1U);
                a64_emit_csel_w(b, A64_TMP1, A64_TMP1, A64_ZR, A64_COND_NE);
                a64_emit_ldr_imm_w(b, A64_TMP2, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, cpsr));
                a64_emit_orr_reg_w(b, A64_TMP2, A64_TMP2, A64_TMP1, A64_SHIFT_LSL, 0U);
                a64_emit_str_imm_w(b, A64_TMP2, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, cpsr));
                a64_emit_msr_nzcv(b, A64_TMP3);
            }
            return true;
        }
        if ((hw1 & 0xFFF0U) == 0xFB30U && (hw2 & 0x00E0U) == 0U) {
            /* SMULW<y> and SMLAW<y> */
            unsigned ra = (hw2 >> 12) & 0xFU;
            bool m_top = ((hw2 >> 4) & 1U) != 0U;
            a64_emit_sbfm_w(b, A64_TMP1, rm, m_top ? 16U : 0U, m_top ? 31U : 15U);
            a64_emit_smaddl_x(b, A64_TMP0, rn, A64_TMP1, A64_ZR);
            if (ra != 15U) {
                a64_emit_mrs_nzcv(b, A64_TMP3);
                a64_emit_ubfx_x_to_w(b, A64_TMP2, A64_TMP0, 16U, 32U);
                a64_emit_add_reg_w(b, true, rd, A64_TMP2, ra, A64_SHIFT_LSL, 0U);
                a64_emit_movz_w(b, A64_TMP1, 0x0800U, 1U);
                a64_emit_csel_w(b, A64_TMP1, A64_TMP1, A64_ZR, A64_COND_VS);
                a64_emit_ldr_imm_w(b, A64_TMP2, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, cpsr));
                a64_emit_orr_reg_w(b, A64_TMP2, A64_TMP2, A64_TMP1, A64_SHIFT_LSL, 0U);
                a64_emit_str_imm_w(b, A64_TMP2, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, cpsr));
                a64_emit_msr_nzcv(b, A64_TMP3);
            } else {
                a64_emit_ubfx_x_to_w(b, rd, A64_TMP0, 16U, 32U);
            }
            return true;
        }
        if (((hw1 & 0xFFF0U) == 0xFB50U || (hw1 & 0xFFF0U) == 0xFB60U) && (hw2 & 0x00E0U) == 0U) {
            /* SMMUL{R} / SMMLA{R} / SMMLS{R} */
            bool is_sub = (hw1 & 0xFFF0U) == 0xFB60U;
            bool round = ((hw2 >> 4) & 1U) != 0U;
            unsigned ra = (hw2 >> 12) & 0xFU;
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
            return true;
        }
        if ((hw1 & 0xFFF0U) == 0xFB70U && (hw2 & 0x00F0U) == 0U) {
            /* USAD8 / USADA8 */
            unsigned ra = (hw2 >> 12) & 0xFU;
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
                a64_emit_mov_w(b, A64_TMP1, rd_lo);
                a64_emit_umaddl_x(b, A64_TMP0, rn, rm, A64_TMP1);
                a64_emit_add_uxtw_x(b, A64_TMP0, A64_TMP0, rd);
                a64_emit_mov_w(b, rd_lo, A64_TMP0);
                a64_emit_ubfx_x_to_w(b, rd, A64_TMP0, 32U, 32U);
                return true;
            }
            if (op == 4U && (subop & 0xCU) == 8U) {
                /* SMLAL<x><y> RdLo, RdHi, Rn, Rm */
                unsigned lsb_n = ((subop >> 1) & 1U) ? 16U : 0U;
                unsigned lsb_m = (subop & 1U) ? 16U : 0U;
                a64_emit_mov_w(b, A64_TMP2, rd_lo);
                a64_emit_bfi_x(b, A64_TMP2, rd, 32U, 32U);
                a64_emit_sbfm_w(b, A64_TMP0, rn, lsb_n, lsb_n + 15U);
                a64_emit_sbfm_w(b, A64_TMP1, rm, lsb_m, lsb_m + 15U);
                a64_emit_smaddl_x(b, A64_TMP2, A64_TMP0, A64_TMP1, A64_TMP2);
                a64_emit_mov_w(b, rd_lo, A64_TMP2);
                a64_emit_ubfx_x_to_w(b, rd, A64_TMP2, 32U, 32U);
                return true;
            }
            if ((op == 4U || op == 5U) && (subop & 0xEU) == 0xCU) {
                /* SMLALD{X} / SMLSLD{X} RdLo, RdHi, Rn, Rm */
                bool is_sub = (op == 5U);
                bool m_swap = (subop & 1U) != 0U;
                unsigned m1_lsb = m_swap ? 16U : 0U;
                unsigned m2_lsb = m_swap ? 0U : 16U;
                a64_emit_mov_w(b, A64_TMP2, rd_lo);
                a64_emit_bfi_x(b, A64_TMP2, rd, 32U, 32U);
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
                a64_emit_ubfx_x_to_w(b, rd, A64_TMP2, 32U, 32U);
                return true;
            }
            if (subop != 0U || (op != 0U && op != 2U && op != 4U && op != 6U)) {
                return false;
            }
            bool is_signed = (op == 0U || op == 4U);
            bool accumulate = (op >= 4U);
            unsigned acc_x = A64_ZR;
            if (accumulate) {
                a64_emit_mov_w(b, A64_TMP1, rd_lo);
                a64_emit_bfi_x(b, A64_TMP1, rd, 32U, 32U);
                acc_x = A64_TMP1;
            }
            if (is_signed) {
                a64_emit_smaddl_x(b, A64_TMP0, rn, rm, acc_x);
            } else {
                a64_emit_umaddl_x(b, A64_TMP0, rn, rm, acc_x);
            }
            a64_emit_mov_w(b, rd_lo, A64_TMP0);
            a64_emit_ubfx_x_to_w(b, rd, A64_TMP0, 32U, 32U);
            return true;
        }
    }

    /* 6. Load/store multiple, dual, exclusive, table branch: (hw1 & 0xFE00) == 0xE800 */
    if ((hw1 & 0xFE00U) == 0xE800U) {
        if ((hw1 & 0xFE40U) == 0xE800U &&
            (((hw1 >> 7) & 3U) == 1U || ((hw1 >> 7) & 3U) == 2U)) {
            /* LDMIA.W / STMIA.W / LDMDB.W / STMDB.W (PUSH.W / POP.W) */
            bool u_bit = ((hw1 >> 7) & 1U) != 0U;
            bool w_bit = ((hw1 >> 5) & 1U) != 0U;
            bool l_bit = ((hw1 >> 4) & 1U) != 0U;
            unsigned rn = hw1 & 0xFU;
            uint32_t mask = hw2 & 0xDFFFU;
            unsigned count = 0U;
            for (unsigned i = 0U; i < 16U; i++) {
                if (mask & (1U << i)) {
                    count++;
                }
            }
            uint16_t bytes = (uint16_t)(count * 4U);
            if (u_bit) {
                a64_emit_mov_w(b, A64_ADDR, rn);
            } else {
                a64_emit_sub_imm_w(b, false, A64_ADDR, rn, bytes, 0U);
            }
            for (unsigned i = 0U; i < 16U; i++) {
                if ((mask & (1U << i)) == 0U) {
                    continue;
                }
                if (l_bit) {
                    unsigned dst = (i == 15U) ? A64_TMP0 : ((i == rn) ? A64_TMP1 : i);
                    a64_emit_guest_ldr_w(b, dst, A64_ADDR);
                } else {
                    a64_emit_guest_str_w(b, i, A64_ADDR);
                }
                a64_emit_add_imm_w(b, false, A64_ADDR, A64_ADDR, 4U, 0U);
            }
            if (l_bit && (mask & (1U << rn)) != 0U && rn != 15U) {
                a64_emit_mov_w(b, rn, A64_TMP1);
            } else if (w_bit) {
                if (u_bit) {
                    a64_emit_add_imm_w(b, false, rn, rn, bytes, 0U);
                } else {
                    a64_emit_sub_imm_w(b, false, rn, rn, bytes, 0U);
                }
            }
            if (l_bit && (mask & (1U << 15))) {
                a64_emit_exit_branch_reg(b, A64_TMP0, true);
                if (is_uncond) {
                    *out_ends_block = true;
                }
            }
            return true;
        }
        /* Exclusives / LDRD / STRD / TBB / TBH */
        if ((hw1 & 0xFFF0U) == 0xE8D0U && (hw2 & 0xFFE0U) == 0xF000U) {
            /* TBB / TBH [Rn, Rm] */
            bool is_tbh = ((hw2 >> 4) & 1U) != 0U;
            unsigned rn = hw1 & 0xFU;
            unsigned rm = hw2 & 0xFU;
            unsigned base = load_t32_reg(b, rn, pc_val, A64_TMP0);
            a64_emit_add_reg_w(b, false, A64_ADDR, base, rm, A64_SHIFT_LSL, is_tbh ? 1U : 0U);
            if (is_tbh) {
                a64_emit_guest_ldrh_w(b, A64_TMP1, A64_ADDR);
            } else {
                a64_emit_guest_ldrb_w(b, A64_TMP1, A64_ADDR);
            }
            a64_emit_mov_imm32(b, A64_TMP0, pc_val);
            a64_emit_add_reg_w(b, false, A64_TMP0, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 1U);
            a64_emit_exit_branch_reg(b, A64_TMP0, false);
            if (is_uncond) {
                *out_ends_block = true;
            }
            return true;
        }
        if ((hw1 & 0xFFE0U) == 0xE840U) {
            /* LDREX / STREXRt, [Rn, #imm8*4] */
            bool is_load = ((hw1 >> 4) & 1U) != 0U;
            unsigned rn = hw1 & 0xFU;
            unsigned rt = (hw2 >> 12) & 0xFU;
            unsigned rd = (hw2 >> 8) & 0xFU;
            uint16_t offset = (uint16_t)((hw2 & 0xFFU) << 2);
            a64_emit_add_imm_w(b, false, A64_ADDR, rn, offset, 0U);
            a64_emit_add_uxtw_x(b, A64_ADDR, A64_XG, A64_ADDR);
            if (is_load) {
                a64_emit_ldxr_w(b, rt, A64_ADDR);
            } else {
                a64_emit_stxr_w(b, rd, rt, A64_ADDR);
            }
            return true;
        }
        if ((hw1 & 0xFFE0U) == 0xE8C0U) {
            /* LDREXB / STREXB / LDREXH / STREXH / LDREXD / STREXD */
            bool is_load = ((hw1 >> 4) & 1U) != 0U;
            unsigned rn  = hw1 & 0xFU;
            unsigned rt  = (hw2 >> 12) & 0xFU;
            unsigned rt2 = (hw2 >> 8) & 0xFU;
            unsigned op3 = (hw2 >> 4) & 0xFU;
            unsigned rd  = hw2 & 0xFU;
            a64_emit_add_uxtw_x(b, A64_ADDR, A64_XG, rn);
            if (op3 == 4U) {
                if (is_load) {
                    a64_emit_ldxr_b(b, rt, A64_ADDR);
                } else {
                    a64_emit_stxr_b(b, rd, rt, A64_ADDR);
                }
                return true;
            }
            if (op3 == 5U) {
                if (is_load) {
                    a64_emit_ldxr_h(b, rt, A64_ADDR);
                } else {
                    a64_emit_stxr_h(b, rd, rt, A64_ADDR);
                }
                return true;
            }
            if (op3 == 7U) {
                if (is_load) {
                    a64_emit_ldxr_x(b, A64_TMP0, A64_ADDR);
                    a64_emit_mov_w(b, rt, A64_TMP0);
                    a64_emit_ubfx_x_to_w(b, rt2, A64_TMP0, 32U, 32U);
                } else {
                    a64_emit_mov_w(b, A64_TMP0, rt);
                    a64_emit_bfi_x(b, A64_TMP0, rt2, 32U, 32U);
                    a64_emit_stxr_x(b, rd, A64_TMP0, A64_ADDR);
                }
                return true;
            }
        }
        /* LDRD / STRD Rt, Rt2, [Rn, #+/-imm8*4] */
        if ((hw1 & 0x0040U) == 0U || (hw1 & 0x0120U) == 0U) {
            return false;
        }
        bool p_bit = ((hw1 >> 8) & 1U) != 0U;
        bool u_bit = ((hw1 >> 7) & 1U) != 0U;
        bool w_bit = ((hw1 >> 5) & 1U) != 0U;
        bool l_bit = ((hw1 >> 4) & 1U) != 0U;
        unsigned rn  = hw1 & 0xFU;
        unsigned rt  = (hw2 >> 12) & 0xFU;
        unsigned rt2 = (hw2 >> 8) & 0xFU;
        uint16_t offset = (uint16_t)((hw2 & 0xFFU) << 2);
        unsigned base = load_t32_reg(b, rn, pc_align4, A64_TMP2);
        if (p_bit) {
            if (u_bit) {
                a64_emit_add_imm_w(b, false, A64_ADDR, base, offset, 0U);
            } else {
                a64_emit_sub_imm_w(b, false, A64_ADDR, base, offset, 0U);
            }
        } else {
            a64_emit_mov_w(b, A64_ADDR, base);
        }
        if (l_bit) {
            a64_emit_guest_ldr_w(b, A64_TMP1, A64_ADDR);
            a64_emit_add_imm_w(b, false, A64_TMP0, A64_ADDR, 4U, 0U);
            a64_emit_guest_ldr_w(b, rt2, A64_TMP0);
            a64_emit_mov_w(b, rt, A64_TMP1);
        } else {
            a64_emit_guest_str_w(b, rt, A64_ADDR);
            a64_emit_add_imm_w(b, false, A64_TMP0, A64_ADDR, 4U, 0U);
            a64_emit_guest_str_w(b, rt2, A64_TMP0);
        }
        if (rn != 15U && !(l_bit && (rn == rt || rn == rt2))) {
            if (!p_bit) {
                if (u_bit) {
                    a64_emit_add_imm_w(b, false, rn, base, offset, 0U);
                } else {
                    a64_emit_sub_imm_w(b, false, rn, base, offset, 0U);
                }
            } else if (w_bit) {
                a64_emit_mov_w(b, rn, A64_ADDR);
            }
        }
        return true;
    }

    /* 7. Load/store single: (hw1 & 0xFE00) == 0xF800 */
    if ((hw1 & 0xFE00U) == 0xF800U) {
        bool sign_ext = ((hw1 >> 8) & 1U) != 0U;
        bool u_imm12  = ((hw1 >> 7) & 1U) != 0U;
        unsigned size = (hw1 >> 5) & 3U; /* 0=byte, 1=half, 2=word */
        bool is_load  = ((hw1 >> 4) & 1U) != 0U;
        unsigned rn   = hw1 & 0xFU;
        unsigned rt   = (hw2 >> 12) & 0xFU;

        if (size > 2U || (size == 2U && sign_ext)) {
            return false;
        }
        if (rn == 15U && !is_load) {
            return false;
        }

        bool p_bit = true;
        bool u_bit = false;
        bool w_bit = false;
        uint16_t imm8 = 0U;

        if (rn == 15U) {
            /* Literal load [PC, #+/-imm12] */
            uint16_t imm12 = (uint16_t)(hw2 & 0xFFFU);
            uint32_t addr = u_imm12 ? (pc_align4 + imm12) : (pc_align4 - imm12);
            a64_emit_mov_imm32(b, A64_ADDR, addr);
        } else if (u_imm12) {
            /* 12-bit unsigned offset [Rn, #imm12] */
            uint16_t imm12 = (uint16_t)(hw2 & 0xFFFU);
            a64_emit_add_imm_w(b, false, A64_ADDR, rn, imm12, 0U);
        } else if ((hw2 & 0x0800U) == 0U) {
            /* Shifted register offset [Rn, Rm, LSL #imm2] */
            if ((hw2 & 0x0FC0U) != 0U) {
                return false;
            }
            unsigned rm = hw2 & 0xFU;
            unsigned shift = (hw2 >> 4) & 3U;
            a64_emit_add_reg_w(b, false, A64_ADDR, rn, rm, A64_SHIFT_LSL, shift);
        } else {
            /* 8-bit pre/post/negative immediate [Rn, #+/-imm8] */
            p_bit = ((hw2 >> 10) & 1U) != 0U;
            u_bit = ((hw2 >> 9) & 1U) != 0U;
            w_bit = ((hw2 >> 8) & 1U) != 0U;
            if (!p_bit && !w_bit) {
                return false;
            }
            if (rt == 15U && is_load && size != 2U && (hw2 & 0x0F00U) != 0x0C00U) {
                return false;
            }
            imm8 = (uint16_t)(hw2 & 0xFFU);
            if (p_bit) {
                if (u_bit) {
                    a64_emit_add_imm_w(b, false, A64_ADDR, rn, imm8, 0U);
                } else {
                    a64_emit_sub_imm_w(b, false, A64_ADDR, rn, imm8, 0U);
                }
            } else {
                a64_emit_mov_w(b, A64_ADDR, rn);
            }
        }

        if (rt == 15U && is_load && size != 2U) {
            /* PLD / PLI hint */
            return true;
        }

        unsigned dst = (is_load && rt == 15U) ? A64_TMP0 : rt;
        if (is_load) {
            if (size == 0U) {
                if (sign_ext) {
                    a64_emit_guest_ldrsb_w(b, dst, A64_ADDR);
                } else {
                    a64_emit_guest_ldrb_w(b, dst, A64_ADDR);
                }
            } else if (size == 1U) {
                if (sign_ext) {
                    a64_emit_guest_ldrsh_w(b, dst, A64_ADDR);
                } else {
                    a64_emit_guest_ldrh_w(b, dst, A64_ADDR);
                }
            } else {
                a64_emit_guest_ldr_w(b, dst, A64_ADDR);
            }
        } else {
            if (size == 0U) {
                a64_emit_guest_strb_w(b, rt, A64_ADDR);
            } else if (size == 1U) {
                a64_emit_guest_strh_w(b, rt, A64_ADDR);
            } else {
                a64_emit_guest_str_w(b, rt, A64_ADDR);
            }
        }

        if (!p_bit && (rn != rt || !is_load)) {
            if (u_bit) {
                a64_emit_add_imm_w(b, false, rn, rn, imm8, 0U);
            } else {
                a64_emit_sub_imm_w(b, false, rn, rn, imm8, 0U);
            }
        } else if (p_bit && w_bit && (rn != rt || !is_load)) {
            a64_emit_mov_w(b, rn, A64_ADDR);
        }

        if (is_load && rt == 15U) {
            a64_emit_exit_branch_reg(b, A64_TMP0, true);
            if (is_uncond) {
                *out_ends_block = true;
            }
        }
        return true;
    }

    /* 8. Coprocessor p15 */
    if ((hw1 & 0xFFEFU) == 0xEE0DU || (hw1 & 0xFFEFU) == 0xEE07U) {
        unsigned rt = (hw2 >> 12) & 0xFU;
        if (hw1 == 0xEE1DU && ((hw2 & 0x0FFFU) == 0x0F70U || (hw2 & 0x0FFFU) == 0x0F50U)) {
            /* MRC p15, 0, Rt, c13, c0, {2,3} (TPIDRURW / TPIDRURO) */
            a64_emit_ldr_imm_w(b, rt, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, tls_tp));
            return true;
        }
        if (hw1 == 0xEE0DU && (hw2 & 0x0FFFU) == 0x0F50U) {
            /* MCR p15, 0, Rt, c13, c0, 2 (TPIDRURW write) */
            a64_emit_str_imm_w(b, rt, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, tls_tp));
            return true;
        }
        if (hw1 == 0xEE07U && (hw2 & 0x0FFFU) == 0x0FBAU) {
            /* MCR p15, 0, Rt, c7, c10, 5 (DMB) */
            a64_emit_dmb_ish(b);
            return true;
        }
        if (hw1 == 0xEE07U && (hw2 & 0x0FFFU) == 0x0F9AU) {
            /* MCR p15, 0, Rt, c7, c10, 4 (DSB) */
            a64_emit_dsb_ish(b);
            return true;
        }
        if (hw1 == 0xEE07U && (hw2 & 0x0FFFU) == 0x0F95U) {
            /* MCR p15, 0, Rt, c7, c5, 4 (ISB) */
            a64_emit_isb(b);
            return true;
        }
    }

    return false;
}

bool a32_translate_t32_block(a32_ctx_t *ctx, a64_buf_t *b, uint32_t start_pc)
{
    uint32_t pc = start_pc & ~1U;
    uint8_t  itstate = (uint8_t)(ctx->itstate & 0xFFU);
    if (itstate != 0U) {
        a64_emit_str_imm_w(b, A64_ZR, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, itstate));
    }

    t32_insn_info_t block_insns[T32_MAX_PRESCAN_INSNS];
    bool            flags_live[T32_MAX_PRESCAN_INSNS];
    unsigned        num_insns = 0U;
    unsigned        max_insns = ctx->single_step ? 1U : T32_MAX_BLOCK_INSNS;

    uint32_t start_page = pc & ~0xFFFU;
    uint32_t scan_pc = pc;
    uint8_t  scan_itstate = itstate;

    /* Pre-pass: scan guest instructions in the basic block */
    for (unsigned count = 0U; (count < max_insns || (!ctx->single_step && scan_itstate != 0U)) && num_insns < T32_MAX_PRESCAN_INSNS; count++) {
        if (count > 0U && (scan_pc & ~0xFFFU) != start_page) {
            break;
        }
        if (ctx->gdb_enabled && count > 0U && a32_gdb_has_breakpoint(scan_pc)) {
            break;
        }
        const uint16_t *guest_hw = (const uint16_t *)a32_guest_ptr(ctx, scan_pc);
        uint16_t hw1 = guest_hw[0];

        t32_insn_info_t *info = &block_insns[num_insns];
        info->pc = scan_pc;
        info->hw1 = hw1;
        info->hw2 = 0U;
        info->itstate = scan_itstate;

        /* Check for 16-bit IT instruction (0xBF00 where mask != 0) */
        if ((hw1 & 0xFF00U) == 0xBF00U && (hw1 & 0x000FU) != 0U) {
            unsigned firstcond = (hw1 >> 4) & 0xFU;
            unsigned it_mask = hw1 & 0xFU;
            if (firstcond != 0xFU &&
                !(firstcond == 0xEU && (it_mask & (it_mask - 1U)) != 0U)) {
                info->is_it = true;
                info->is_32bit = false;
                scan_itstate = (uint8_t)(hw1 & 0xFFU);
                scan_pc += 2U;
                num_insns++;
                continue;
            }
        }

        info->is_it = false;
        bool is_32bit = ((hw1 >> 11) == 0x1DU || (hw1 >> 11) == 0x1EU || (hw1 >> 11) == 0x1FU);
        info->is_32bit = is_32bit;

        bool in_it = (scan_itstate & 0x0FU) != 0U;
        bool scan_uncond = !in_it || (((scan_itstate >> 4) & 0xFU) == A64_COND_AL);
        bool ends_uncond = false;

        if (is_32bit) {
            if (((scan_pc + 2U) & ~0xFFFU) != start_page) {
                if (count > 0U) {
                    break;
                }
                bool mapped = false;
                uint32_t hw2_pc = scan_pc + 2U;
                for (size_t r_idx = 0U; r_idx < ctx->region_count; r_idx++) {
                    const a32_region_t *r = &ctx->regions[r_idx];
                    if (hw2_pc >= r->start && hw2_pc < r->end && (r->prot & 0x5U) != 0U) {
                        mapped = true;
                        break;
                    }
                }
                if (!mapped) {
                    if (itstate != 0U) {
                        a64_emit_mov_imm32(b, A64_TMP0, itstate);
                        a64_emit_str_imm_w(b, A64_TMP0, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, itstate));
                    }
                    a64_emit_exit_undef(b, pc);
                    b->guest_end_pc = pc + 2U;
                    return true;
                }
            }
            uint16_t hw2 = guest_hw[1];
            info->hw2 = hw2;
            scan_pc += 4U;
            if (scan_uncond && t32_insn_uncond_ends_block(hw1, hw2)) {
                ends_uncond = true;
            }
        } else {
            scan_pc += 2U;
            if ((scan_uncond && t16_insn_uncond_ends_block(hw1)) || (hw1 & 0xFF00U) == 0xBE00U) {
                ends_uncond = true;
            }
        }

        if (in_it) {
            /* Advance ITSTATE: shift bottom 5 bits left by 1; if bottom 4 bits become 0, clear */
            uint8_t mask = (uint8_t)((scan_itstate << 1) & 0x1FU);
            if ((mask & 0x0FU) == 0U) {
                scan_itstate = 0U;
            } else {
                scan_itstate = (uint8_t)((scan_itstate & 0xE0U) | mask);
            }
        }

        num_insns++;
        if (ends_uncond) {
            break;
        }
    }

    /* Backward pass: determine NZCV flag liveness */
    bool live = true;
    for (int i = (int)num_insns - 1; i >= 0; i--) {
        const t32_insn_info_t *info = &block_insns[i];
        flags_live[i] = live;
        if (t32_insn_overwrites_all_flags(info)) {
            live = false;
        } else if (info->is_it || info->itstate != 0U || t32_insn_reads_flags(info) ||
                   (live && t32_insn_partially_updates_flags(info))) {
            live = true;
        }
    }

    /* Translation & emission loop */
    for (unsigned idx = 0U; idx < num_insns; idx++) {
        const t32_insn_info_t *info = &block_insns[idx];
        uint32_t cur_pc = info->pc;
        uint16_t hw1 = info->hw1;

        if (info->is_it) {
            itstate = (uint8_t)(hw1 & 0xFFU);
            continue;
        }

        /* M3: Record host_word_idx -> (guest_pc, itstate) in JIT side-table */
        uint32_t it_cond = (itstate >> 4) & 0xFU;
        uint32_t it_mask = itstate & 0xFU;
        uint32_t itstate_enc = (it_cond << 4) | it_mask;
        a32_jit_record_pc_map((uint32_t)b->count, cur_pc, itstate_enc);

        bool in_it = (itstate & 0x0FU) != 0U;
        if (!info->is_32bit && (hw1 & 0xFF00U) == 0xBE00U) {
            if (in_it) {
                a64_emit_mov_imm32(b, A64_TMP0, itstate);
                a64_emit_str_imm_w(b, A64_TMP0, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, itstate));
            }
            a64_emit_exit_bkpt(b, cur_pc);
            b->guest_end_pc = cur_pc + 2U;
            return true;
        }

        uint8_t next_itstate = 0U;
        if (in_it) {
            uint8_t mask = (uint8_t)((itstate << 1) & 0x1FU);
            if ((mask & 0x0FU) != 0U) {
                next_itstate = (uint8_t)((itstate & 0xE0U) | mask);
            }
        }

        unsigned cond = A64_COND_AL;
        size_t cond_skip = 0U;
        if (in_it) {
            cond = (itstate >> 4) & 0xFU;
            if (cond != A64_COND_AL) {
                cond_skip = a64_emit_b_cond_placeholder(b, cond ^ 1U);
            }
        }

        bool is_uncond = !in_it || (cond == A64_COND_AL);
        bool ends_block = false;
        bool ok = false;

        if (info->is_32bit) {
            uint16_t hw2 = info->hw2;
            ok = translate_t32_insn(ctx, b, cur_pc, hw1, hw2, is_uncond, flags_live[idx], &ends_block);
            if (!ok) {
                if (ctx->trace_enabled) {
                    fprintf(stderr, "a32rt: unhandled T32 instruction 0x%04x 0x%04x at pc=0x%08x\n",
                            hw1, hw2, cur_pc);
                }
                if (in_it) {
                    a64_emit_mov_imm32(b, A64_TMP0, itstate);
                    a64_emit_str_imm_w(b, A64_TMP0, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, itstate));
                }
                a64_emit_exit_undef(b, cur_pc);
            }
        } else {
            ok = translate_t16_insn(ctx, b, cur_pc, hw1, in_it, itstate, next_itstate, is_uncond, flags_live[idx], &ends_block);
            if (!ok) {
                if (ctx->trace_enabled) {
                    fprintf(stderr, "a32rt: unhandled T16 instruction 0x%04x at pc=0x%08x\n",
                            hw1, cur_pc);
                }
                if (in_it) {
                    a64_emit_mov_imm32(b, A64_TMP0, itstate);
                    a64_emit_str_imm_w(b, A64_TMP0, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, itstate));
                }
                a64_emit_exit_undef(b, cur_pc);
            }
        }

        if (in_it) {
            if (cond != A64_COND_AL) {
                a64_patch_b_cond(b, cond_skip, b->count);
            }
            itstate = next_itstate;
        }

        if ((ends_block && (itstate == 0U || is_uncond)) || (!ok && is_uncond)) {
            b->guest_end_pc = cur_pc + (info->is_32bit ? 4U : 2U);
            return true;
        }
    }

    if (itstate != 0U) {
        a64_emit_mov_imm32(b, A64_TMP0, itstate);
        a64_emit_str_imm_w(b, A64_TMP0, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, itstate));
    }
    b->guest_end_pc = scan_pc;
    a64_emit_exit_branch_imm(b, scan_pc, false);
    return true;
}
