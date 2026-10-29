/*
 * src/interp/interp_a32_ctl.c - split-TU part 1/2 of src/decode_a32.c (lines 1-645).
 * Compiled only via the src/decode_a32.c wrapper; do not add to Makefile SRCS.
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * src/decode_a32.c — A32 instruction decoder and AArch64 lowering
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "a64_emit.h"

#include <stdio.h>

#define A32_MAX_BLOCK_INSNS 128U

/*
 * Compute a shifted register operand into `out_reg` (or return `rm` directly
 * when no shift is applied and `rm != 15`).
 */
static unsigned emit_a32_shifted_reg(a64_buf_t *b, uint32_t insn, uint32_t pc_val,
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

/*
 * Check if an instruction unconditionally terminates a translation block.
 */
static bool a32_insn_uncond_ends_block(uint32_t insn)
{
    unsigned cond = (insn >> 28) & 0xFU;
    if (cond == 0xFU) {
        /* BLX <label> */
        if ((insn & 0xFE000000U) == 0xFA000000U) {
            return true;
        }
        return false;
    }

    if ((insn & 0x0FF000F0U) == 0x01200070U) {
        /* BKPT */
        return true;
    }

    if (cond != A64_COND_AL) {
        return false;
    }

    if ((insn & 0x0FF000F0U) == 0x07F000F0U) {
        /* UDF */
        return true;
    }

    unsigned op1 = (insn >> 25) & 7U;
    if (op1 == 5U) {
        /* B / BL */
        return true;
    }
    if (op1 == 7U) {
        if ((insn & 0x0F000000U) == 0x0F000000U) {
            /* SVC */
            return true;
        }
    }
    if (op1 == 4U) {
        /* LDM with PC */
        bool l_bit = ((insn >> 20) & 1U) != 0U;
        if (l_bit && (insn & (1U << 15))) {
            return true;
        }
    }
    if (op1 == 2U || (op1 == 3U && ((insn & 0x10U) == 0U))) {
        /* LDR with Rt == 15 */
        bool l_bit = ((insn >> 20) & 1U) != 0U;
        unsigned rt = (insn >> 12) & 0xFU;
        if (l_bit && rt == 15U) {
            return true;
        }
    }
    if (op1 == 0U) {
        if ((insn & 0x0FFFFFF0U) == 0x012FFF10U || (insn & 0x0FFFFFF0U) == 0x012FFF30U) {
            /* BX Rm or BLX Rm */
            return true;
        }
    }
    if (op1 == 0U || op1 == 1U) {
        /* Data processing with Rd == 15 */
        unsigned opc = (insn >> 21) & 0xFU;
        unsigned rd = (insn >> 12) & 0xFU;
        if (rd == 15U && (opc < 8U || opc >= 12U)) {
            if (op1 == 0U && (insn & 0x0E000090U) == 0x00000090U) {
                return false;
            }
            if (op1 == 0U && (insn & 0x0F8000F0U) == 0x01800090U) {
                return false;
            }
            return true;
        }
    }
    return false;
}

/*
 * Check if an instruction reads NZCV flags (either for condition evaluation,
 * or as a carry consumer / CPSR reader).
 */
static bool a32_insn_reads_flags(uint32_t insn)
{
    unsigned cond = (insn >> 28) & 0xFU;
    if (cond != 14U && cond != 15U) {
        /* Any conditional instruction reads NZCV */
        return true;
    }
    if (cond == 15U) {
        /* ARMv8-A VSEL{EQ,VS,GE,GT}.F32/.F64 reads NZCV despite cond == 15 */
        if ((insn & 0xFF800E50U) == 0xFE000A00U) {
            return true;
        }
        return false;
    }
    if ((insn & 0x0FF000F0U) == 0x07F000F0U) {
        /* UDF trap must observe architectural NZCV */
        return true;
    }

    /* cond == 14 (AL): check carry consumers */
    unsigned op1 = (insn >> 25) & 7U;
    if (op1 == 0U || op1 == 1U) {
        if (op1 == 0U && (insn & 0x0E000090U) == 0x00000090U) {
            return false;
        }
        if (op1 == 0U && (insn & 0x0F8000F0U) == 0x01800090U) {
            return false;
        }
        if (op1 == 0U && (insn & 0x0FBF0FFFU) == 0x010F0000U) {
            /* MRS Rd, CPSR reads flags */
            return true;
        }
        unsigned opc = (insn >> 21) & 0xFU;
        if (opc == 5U || opc == 6U || opc == 7U) {
            /* ADC, SBC, RSC */
            return true;
        }
        /* Check for RRX shift: op1 == 0, bit 4 == 0, shift_type == 3 (ROR), imm5 == 0 */
        if (op1 == 0U && ((insn & 0x00000FF0U) == 0x00000060U)) {
            return true;
        }
        /* Register-shifted logical ops with S=1 read old_C when Rs[7:0] == 0 */
        if (op1 == 0U && ((insn & 0x00100090U) == 0x00100010U) &&
            (opc == 0U || opc == 1U || opc == 8U || opc == 9U || opc >= 12U)) {
            return true;
        }
    } else if (op1 == 3U && ((insn & 0x10U) == 0U)) {
        /* Single data transfer register offset shifted by RRX */
        if ((insn & 0x00000FF0U) == 0x00000060U) {
            return true;
        }
    }
    return false;
}

/*
 * Check if an instruction unconditionally overwrites all 4 NZCV flags.
 */
static bool a32_insn_overwrites_all_flags(uint32_t insn)
{
    unsigned cond = (insn >> 28) & 0xFU;
    if (cond != 14U) {
        return false;
    }
    unsigned op1 = (insn >> 25) & 7U;
    if (op1 == 0U || op1 == 1U) {
        if (op1 == 0U && (insn & 0x0E000090U) == 0x00000090U) {
            return false;
        }
        if (op1 == 0U && (insn & 0x0F8000F0U) == 0x01800090U) {
            return false;
        }
        /* MSR CPSR_f, Rm */
        if ((insn & 0x0FF0FFF0U) == 0x0120F000U && ((insn >> 19) & 1U) != 0U) {
            return true;
        }
        /* MSR CPSR_f, #imm */
        if (op1 == 1U && (insn & 0x0FF0F000U) == 0x0320F000U && ((insn >> 19) & 1U) != 0U) {
            return true;
        }
        unsigned opc = (insn >> 21) & 0xFU;
        bool s_bit = ((insn >> 20) & 1U) != 0U;
        if (s_bit && (opc == 10U || opc == 11U)) {
            /* CMP / CMN always set NZCV */
            return true;
        }
        if (s_bit && (opc >= 2U && opc <= 7U)) {
            /* SUB, RSB, ADD, ADC, SBC, RSC with S=1 */
            return true;
        }
    } else if (op1 == 7U) {
        /* VMRS APSR_nzcv, FPSCR: 0xEEF1FA10 */
        if ((insn & 0x0FFFFFFFU) == 0x0EF1FA10U) {
            return true;
        }
    }
    return false;
}

static bool decode_a32_insn(a32_ctx_t *ctx, a64_buf_t *b, uint32_t pc,
                            uint32_t insn, bool flags_live, bool *out_ends_block)
{
    *out_ends_block = false;
    uint32_t pc_val = pc + 8U;
    unsigned cond = (insn >> 28) & 0xFU;

    /* Unconditional space (cond == 0xF) */
    if (cond == 0xFU) {
        if ((insn & 0xFFF000F0U) == 0xF5700050U) {
            a64_emit_dmb_ish(b);
            return true;
        }
        if ((insn & 0xFFF000F0U) == 0xF5700040U) {
            a64_emit_dsb_ish(b);
            return true;
        }
        if ((insn & 0xFFF000F0U) == 0xF5700060U) {
            a64_emit_isb(b);
            return true;
        }
        if (insn == 0xF57FF01FU) {
            a64_emit_clrex(b);
            return true;
        }
        if ((insn & 0xFD70F000U) == 0xF450F000U || (insn & 0xFD30F000U) == 0xF510F000U) {
            /* PLD / PLDW / PLI hints -> NOP */
            return true;
        }
        if ((insn & 0xFE000000U) == 0xFA000000U) {
            /* BLX <label> (ARM -> Thumb interworking call) */
            int32_t imm24 = (int32_t)(insn & 0x00FFFFFFU);
            if (imm24 & 0x00800000) {
                imm24 |= (int32_t)0xFF000000U;
            }
            uint32_t h_bit = ((insn >> 24) & 1U) << 1;
            uint32_t target = (pc_val + (uint32_t)(imm24 << 2) + h_bit) | 1U;
            a64_emit_mov_pc32(b, 14U, pc + 4U);
            b->is_call = true;
            a64_emit_exit_branch_imm(b, target, true);
            *out_ends_block = true;
            return true;
        }
        /* VFPv4-D32 / NEON unconditional instructions */
        if ((insn & 0xFE000000U) == 0xF2000000U || (insn & 0xFF100000U) == 0xF4000000U ||
            (insn & 0xFE000000U) == 0xFE000000U) {
            return a32_decode_vfp_neon_a32(b, insn, pc);
        }
        return false;
    }

    /* BKPT #imm16 (unconditional trap) */
    if ((insn & 0x0FF000F0U) == 0x01200070U) {
        a64_emit_exit_bkpt(b, pc);
        *out_ends_block = true;
        return true;
    }

    /* Branchless CSEL lowering for conditional MOV Rd, Rm (S=0, unshifted, Rd<15, Rm<15). */
    if (cond < 14U && (insn & 0x0FF00FF0U) == 0x01A00000U) {
        unsigned rd = (insn >> 12) & 0xFU;
        unsigned rm = insn & 0xFU;
        if (rd < 15U && rm < 15U) {
            a64_emit_csel_w(b, rd, rm, rd, cond);
            return true;
        }
    }

    size_t cond_skip = 0U;
    if (cond != A64_COND_AL) {
        cond_skip = a64_emit_b_cond_placeholder(b, cond ^ 1U);
    }

    unsigned op1 = (insn >> 25) & 7U;
    bool handled = false;

    /* 1. Branch B / BL (op1 == 5) */
    if (op1 == 5U) {
        bool link = ((insn >> 24) & 1U) != 0U;
        int32_t imm24 = (int32_t)(insn & 0x00FFFFFFU);
        if (imm24 & 0x00800000) {
            imm24 |= (int32_t)0xFF000000U;
        }
        uint32_t target = pc_val + (uint32_t)(imm24 << 2);
        if (link) {
            a64_emit_mov_pc32(b, 14U, pc + 4U);
            if (cond == A64_COND_AL) {
                b->is_call = true;
            }
        }
        a64_emit_exit_branch_imm(b, target, false);
        if (cond == A64_COND_AL) {
            *out_ends_block = true;
        }
        handled = true;
    }
    /* 2. Coprocessor & SVC (op1 == 6 or op1 == 7) */
    else if (op1 == 6U || op1 == 7U) {
        if (op1 == 7U && (insn & 0x0F000000U) == 0x0F000000U) {
            /* SVC #imm24 */
            a64_emit_exit_svc(b, pc + 4U, insn & 0x00FFFFFFU);
            if (cond == A64_COND_AL) {
                *out_ends_block = true;
            }
            handled = true;
        } else {
            unsigned cp = (insn >> 8) & 0xFU;
            if (cp == 10U || cp == 11U) {
                /* VFPv4-D32 / NEON delegation */
                handled = a32_decode_vfp_neon_a32(b, insn, pc);
            } else if (cp == 15U) {
                handled = a64_emit_cp15(b, insn);
            }
        }
    }
    /* 3. Block Data Transfer LDM / STM / PUSH / POP (op1 == 4) */
    else if (op1 == 4U) {
        bool p_bit = ((insn >> 24) & 1U) != 0U;
        bool u_bit = ((insn >> 23) & 1U) != 0U;
        bool w_bit = ((insn >> 21) & 1U) != 0U;
        bool l_bit = ((insn >> 20) & 1U) != 0U;
        unsigned rn = (insn >> 16) & 0xFU;
        uint32_t reg_mask = insn & 0xFFFFU;
        if (reg_mask != 0U && rn != 15U) {
            a64_emit_ldm_stm(b, rn, reg_mask, l_bit, p_bit, u_bit, w_bit,
                             pc_val, cond == A64_COND_AL, out_ends_block);
            handled = true;
        }
    }
    /* 4. Single Data Transfer LDR / STR / LDRB / STRB (op1 == 2 or op1 == 3, bit 4 == 0) */
    else if (op1 == 2U || (op1 == 3U && ((insn & 0x10U) == 0U))) {
        bool reg_off = (op1 == 3U);
        bool p_bit = ((insn >> 24) & 1U) != 0U;
        bool u_bit = ((insn >> 23) & 1U) != 0U;
        bool b_bit = ((insn >> 22) & 1U) != 0U;
        bool w_bit = ((insn >> 21) & 1U) != 0U;
        bool l_bit = ((insn >> 20) & 1U) != 0U;
        unsigned rn = (insn >> 16) & 0xFU;
        unsigned rt = (insn >> 12) & 0xFU;

        unsigned base_reg = a32_load_reg(b, rn, pc_val, A64_TMP2);
        uint16_t imm12 = (uint16_t)(insn & 0xFFFU);
        unsigned off_reg = A64_ZR;

        if (reg_off) {
            off_reg = emit_a32_shifted_reg(b, insn & ~0x10U, pc_val, A64_TMP1, false, NULL);
            if (!p_bit && off_reg == rt) {
                a64_emit_mov_w(b, A64_TMP1, off_reg);
                off_reg = A64_TMP1;
            }
        }

        if (p_bit) {
            if (!reg_off) {
                if (u_bit) {
                    a64_emit_add_imm_w(b, false, A64_ADDR, base_reg, imm12, 0U);
                } else {
                    a64_emit_sub_imm_w(b, false, A64_ADDR, base_reg, imm12, 0U);
                }
            } else {
                if (u_bit) {
                    a64_emit_add_reg_w(b, false, A64_ADDR, base_reg, off_reg, A64_SHIFT_LSL, 0U);
                } else {
                    a64_emit_sub_reg_w(b, false, A64_ADDR, base_reg, off_reg, A64_SHIFT_LSL, 0U);
                }
            }
        } else {
            a64_emit_mov_w(b, A64_ADDR, base_reg);
        }

        if (l_bit) {
            unsigned dst = (rt == 15U) ? A64_TMP0 : rt;
            if (b_bit) {
                a64_emit_guest_ldrb_w(b, dst, A64_ADDR);
            } else {
                a64_emit_guest_ldr_w(b, dst, A64_ADDR);
            }
        } else {
            unsigned src = a32_load_reg(b, rt, pc_val, A64_TMP0);
            if (b_bit) {
                a64_emit_guest_strb_w(b, src, A64_ADDR);
            } else {
                a64_emit_guest_str_w(b, src, A64_ADDR);
            }
        }

        if (!p_bit && rn != 15U && (rn != rt || !l_bit)) {
            if (!reg_off) {
                if (u_bit) {
                    a64_emit_add_imm_w(b, false, rn, base_reg, imm12, 0U);
                } else {
                    a64_emit_sub_imm_w(b, false, rn, base_reg, imm12, 0U);
                }
            } else {
                if (u_bit) {
                    a64_emit_add_reg_w(b, false, rn, base_reg, off_reg, A64_SHIFT_LSL, 0U);
                } else {
                    a64_emit_sub_reg_w(b, false, rn, base_reg, off_reg, A64_SHIFT_LSL, 0U);
                }
            }
        } else if (p_bit && w_bit && rn != 15U && (rn != rt || !l_bit)) {
            a64_emit_mov_w(b, rn, A64_ADDR);
        }

        if (l_bit && rt == 15U) {
            a64_emit_exit_branch_reg(b, A64_TMP0, true);
            if (cond == A64_COND_AL) {
                *out_ends_block = true;
            }
        }
        handled = true;
    }
    /* 5. Media instructions (op1 == 3, bit 4 == 1) */
    else if (op1 == 3U && ((insn & 0x10U) != 0U)) {
        if ((insn & 0x0FF0F0F0U) == 0x0710F010U) {
            /* SDIV Rd, Rn, Rm (A32 Rd:19:16, Rm:11:8, Rn:3:0) */
            unsigned rd = (insn >> 16) & 0xFU;
            unsigned rm = (insn >> 8) & 0xFU;
            unsigned rn = insn & 0xFU;
            a64_emit_sdiv_w(b, rd, rn, rm);
            handled = true;
        } else if ((insn & 0x0FF0F0F0U) == 0x0730F010U) {
            /* UDIV Rd, Rn, Rm */
            unsigned rd = (insn >> 16) & 0xFU;
            unsigned rm = (insn >> 8) & 0xFU;
            unsigned rn = insn & 0xFU;
            a64_emit_udiv_w(b, rd, rn, rm);
            handled = true;
        } else if ((insn & 0x0FE00070U) == 0x07E00050U) {
            /* UBFX Rd, Rn, #lsb, #width */
            unsigned widthm1 = (insn >> 16) & 31U;
            unsigned rd = (insn >> 12) & 0xFU;
            unsigned lsb = (insn >> 7) & 31U;
            unsigned rn = insn & 0xFU;
            if (lsb + widthm1 <= 31U) {
                a64_emit_ubfm_w(b, rd, rn, lsb, lsb + widthm1);
                handled = true;
            }
        } else if ((insn & 0x0FE00070U) == 0x07A00050U) {
            /* SBFX Rd, Rn, #lsb, #width */
            unsigned widthm1 = (insn >> 16) & 31U;
            unsigned rd = (insn >> 12) & 0xFU;
            unsigned lsb = (insn >> 7) & 31U;
            unsigned rn = insn & 0xFU;
            if (lsb + widthm1 <= 31U) {
                a64_emit_sbfm_w(b, rd, rn, lsb, lsb + widthm1);
                handled = true;
            }
        } else if ((insn & 0x0FE00070U) == 0x07C00010U) {
            /* BFI / BFC Rd, Rn, #lsb, #width */
            unsigned msb = (insn >> 16) & 31U;
            unsigned rd = (insn >> 12) & 0xFU;
            unsigned lsb = (insn >> 7) & 31U;
            unsigned rn = insn & 0xFU;
            if (msb >= lsb) {
                unsigned src = (rn == 15U) ? A64_ZR : rn;
                a64_emit_bfm_w(b, rd, src, (32U - lsb) & 31U, msb - lsb);
                handled = true;
            }
        } else if ((insn & 0x0FFF0FF0U) == 0x06BF0F30U) {
            /* REV Rd, Rm */
            a64_emit_rev_w(b, (insn >> 12) & 0xFU, insn & 0xFU);
            handled = true;
        } else if ((insn & 0x0FFF0FF0U) == 0x06BF0FB0U) {
            /* REV16 Rd, Rm */
            a64_emit_rev16_w(b, (insn >> 12) & 0xFU, insn & 0xFU);
            handled = true;
        } else if ((insn & 0x0FFF0FF0U) == 0x06FF0FB0U) {
            /* REVSH Rd, Rm */
            unsigned rd = (insn >> 12) & 0xFU;
            unsigned rm = insn & 0xFU;
            a64_emit_rev16_w(b, A64_TMP0, rm);
            a64_emit_sbfm_w(b, rd, A64_TMP0, 0U, 15U);
            handled = true;
        } else if ((insn & 0x0FFF0FF0U) == 0x06FF0F30U) {
            /* RBIT Rd, Rm */
            a64_emit_rbit_w(b, (insn >> 12) & 0xFU, insn & 0xFU);
            handled = true;
        } else if ((insn & 0x0FF003F0U) == 0x06E00070U || (insn & 0x0FF003F0U) == 0x06F00070U ||
                   (insn & 0x0FF003F0U) == 0x06A00070U || (insn & 0x0FF003F0U) == 0x06B00070U ||
                   (insn & 0x0FF003F0U) == 0x06C00070U || (insn & 0x0FF003F0U) == 0x06800070U) {
            /* UXT{A}{B,H,B16} / SXT{A}{B,H,B16} */
            bool is_signed = ((insn >> 22) & 1U) == 0U;
            bool is_16x2 = ((insn >> 20) & 3U) == 0U;
            bool is_half = ((insn >> 20) & 3U) == 3U;
            a64_emit_xt_add(b, is_signed, is_half, is_16x2, (insn >> 12) & 0xFU,
                            (insn >> 16) & 0xFU, insn & 0xFU, ((insn >> 10) & 3U) * 8U);
            handled = true;
        } else if ((insn & 0x0FF00030U) == 0x06800010U) {
            /* PKHBT / PKHTB Rd, Rn, Rm, {LSL/ASR} #imm5 */
            a64_emit_pkh(b, ((insn >> 6) & 1U) != 0U, (insn >> 12) & 0xFU,
                         (insn >> 16) & 0xFU, insn & 0xFU, (insn >> 7) & 31U);
            handled = true;
        } else if ((insn & 0x0FE00030U) == 0x06A00010U) {
            /* SSAT Rd, #sat_imm, Rn, {LSL/ASR} #shift */
            unsigned src = a32_load_reg(b, insn & 0xFU, pc_val, A64_TMP0);
            a64_emit_ssat(b, (insn >> 12) & 0xFU, ((insn >> 16) & 31U) + 1U, src,
                          ((insn >> 6) & 1U) != 0U, (insn >> 7) & 31U);
            handled = true;
        } else if ((insn & 0x0FE00030U) == 0x06E00010U) {
            /* USAT Rd, #sat_imm, Rn, {LSL/ASR} #shift */
            unsigned src = a32_load_reg(b, insn & 0xFU, pc_val, A64_TMP0);
            a64_emit_usat(b, (insn >> 12) & 0xFU, (insn >> 16) & 31U, src,
                          ((insn >> 6) & 1U) != 0U, (insn >> 7) & 31U);
            handled = true;
        } else if ((insn & 0x0FB00FF0U) == 0x06A00F30U) {
            /* SSAT16 / USAT16 Rd, #sat_imm, Rn */
            bool is_signed = ((insn >> 22) & 1U) == 0U;
            unsigned sat_bits = ((insn >> 16) & 15U) + (is_signed ? 1U : 0U);
            unsigned src = a32_load_reg(b, insn & 0xFU, pc_val, A64_TMP0);
            a64_emit_sat16(b, is_signed, (insn >> 12) & 0xFU, sat_bits, src);
            handled = true;
        } else if ((insn & 0x0FF00FF0U) == 0x06800FB0U) {
            /* SEL Rd, Rn, Rm */
            a64_emit_sel(b, (insn >> 12) & 0xFU, (insn >> 16) & 0xFU, insn & 0xFU);
            handled = true;
        } else if ((((insn >> 20) & 7U) >= 1U && ((insn >> 20) & 7U) != 4U) &&
                   (((insn & 0x0F800FF0U) == 0x06000F90U) ||
                    ((insn & 0x0F800FF0U) == 0x06000FF0U) ||
                    ((insn & 0x0F800F90U) == 0x06000F10U))) {
            /* Parallel Add/Sub: {S,Q,SH,U,UQ,UH}{ADD8,SUB8,ADD16,ASX,SAX,SUB16} */
            unsigned kind = ((insn >> 20) & 7U) - 1U;
            bool is_16 = ((insn >> 7) & 1U) == 0U;
            unsigned op2 = (insn >> 5) & 3U;
            bool swap_rm = is_16 && (op2 == 1U || op2 == 2U);
            unsigned sub_mask = is_16 ? op2 : ((op2 != 0U) ? 0xFU : 0U);
            a64_emit_parallel_addsub(b, kind, is_16, swap_rm, sub_mask,
                                     (insn >> 12) & 0xFU, (insn >> 16) & 0xFU, insn & 0xFU);
            handled = true;
        } else if ((insn & 0x0FF00090U) == 0x07000010U) {
            /* SMUAD{X} / SMUSD{X} / SMLAD{X} / SMLSD{X} */
            a64_emit_smlad(b, (insn >> 16) & 0xFU, insn & 0xFU, (insn >> 8) & 0xFU,
                           (insn >> 12) & 0xFU, ((insn >> 6) & 1U) != 0U, ((insn >> 5) & 1U) != 0U);
            handled = true;
        } else if ((insn & 0x0FF00090U) == 0x07400010U) {
            /* SMLALD{X} / SMLSLD{X} RdLo, RdHi, Rn, Rm */
            a64_emit_smlald(b, (insn >> 12) & 0xFU, (insn >> 16) & 0xFU, insn & 0xFU,
                            (insn >> 8) & 0xFU, ((insn >> 6) & 1U) != 0U, ((insn >> 5) & 1U) != 0U);
            handled = true;
        } else if ((insn & 0x0FF00010U) == 0x07500010U) {
            /* SMMUL{R} / SMMLA{R} / SMMLS{R} */
            unsigned op2 = (insn >> 6) & 3U;
            if (op2 == 0U || op2 == 3U) {
                a64_emit_smmla(b, (insn >> 16) & 0xFU, insn & 0xFU, (insn >> 8) & 0xFU,
                               (insn >> 12) & 0xFU, op2 == 3U, ((insn >> 5) & 1U) != 0U);
                handled = true;
            }
        } else if ((insn & 0x0FF000F0U) == 0x07800010U) {
            /* USAD8 / USADA8 */
            a64_emit_usad8(b, (insn >> 16) & 0xFU, insn & 0xFU, (insn >> 8) & 0xFU,
                           (insn >> 12) & 0xFU);
            handled = true;
        }
    }
    /* 6. Data-Processing, Multiply, Exclusives, Extra Load/Store (op1 == 0 or op1 == 1) */
    else if (op1 == 0U || op1 == 1U) {
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
            /* SWP / SWPB */
            bool b_bit = ((insn >> 22) & 1U) != 0U;
            unsigned rn = (insn >> 16) & 0xFU;
            unsigned rd = (insn >> 12) & 0xFU;
            unsigned rm = insn & 0xFU;
