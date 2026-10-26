/*
 * src/interp/interp_t16.c - split-TU part 1/3 of src/decode_t32.c (lines 1-628).
 * Compiled only via the src/decode_t32.c wrapper; do not add to Makefile SRCS.
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * src/decode_t32.c — T32 (Thumb-1 / Thumb-2) instruction decoder and AArch64 lowering
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "a64_emit.h"

#include <stdio.h>

#define T32_MAX_BLOCK_INSNS 128U

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

#define T32_MAX_PRESCAN_INSNS (T32_MAX_BLOCK_INSNS + 8U)

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
    if ((hw1 & 0xFFF0U) == 0xF380U && (hw2 & 0xF0FFU) == 0x8000U && (hw2 & 0x0800U) != 0U) {
        return true;
    }

    return false;
}

static bool t32_insn_reads_flags(const t32_insn_info_t *info)
{
    uint16_t hw1 = info->hw1;
    if (!info->is_32bit) {
        /* UDF #imm8 */
        if ((hw1 & 0xFF00U) == 0xDE00U) {
            return true;
        }
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

    /* UDF.W #imm16 */
    if ((hw1 & 0xFFF0U) == 0xF7F0U && (hw2 & 0xF000U) == 0xA000U) {
        return true;
    }

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

    /* ARMv8-A VSEL{EQ,VS,GE,GT}.F32/.F64 reads NZCV */
    if ((hw1 & 0xFF80U) == 0xFE00U && (hw2 & 0x0E50U) == 0x0A00U) {
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
