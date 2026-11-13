/*
 * src/interp/interp_t32_mem.c — T32 (Thumb-2) memory lowering and block translator
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "interp/decode_priv.h"

#include <stdio.h>

#define T32_MAX_BLOCK_INSNS   128U
#define T32_MAX_PRESCAN_INSNS (T32_MAX_BLOCK_INSNS + 8U)

typedef struct {
    uint32_t pc;
    uint16_t hw1;
    uint16_t hw2;
    uint8_t  itstate;
    bool     is_32bit;
    bool     is_it;
} t32_insn_info_t;

bool a32_translate_t32_mem_insn(a64_buf_t *b, uint16_t hw1, uint16_t hw2,
                                uint32_t pc_val, uint32_t pc_align4, bool is_uncond,
                                bool *out_ends_block)
{
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
            if (mask == 0U || rn == 15U || (!l_bit && (hw2 & 0x8000U) != 0U)) {
                return false;
            }
            a64_emit_ldm_stm(b, rn, mask, l_bit, !u_bit, u_bit, w_bit,
                             pc_val, is_uncond, out_ends_block);
            return true;
        }
        /* Exclusives / LDRD / STRD / TBB / TBH */
        if ((hw1 & 0xFFF0U) == 0xE8D0U && (hw2 & 0xFFE0U) == 0xF000U) {
            /* TBB / TBH [Rn, Rm] */
            bool is_tbh = ((hw2 >> 4) & 1U) != 0U;
            unsigned rn = hw1 & 0xFU;
            unsigned rm = hw2 & 0xFU;
            unsigned base = a32_load_reg(b, rn, pc_val, A64_TMP0);
            a64_emit_add_reg_w(b, false, A64_ADDR, base, rm, A64_SHIFT_LSL, is_tbh ? 1U : 0U);
            if (is_tbh) {
                a64_emit_guest_ldrh_w(b, A64_TMP1, A64_ADDR);
            } else {
                a64_emit_guest_ldrb_w(b, A64_TMP1, A64_ADDR);
            }
            a64_emit_mov_pc32(b, A64_TMP0, pc_val);
            a64_emit_add_reg_w(b, false, A64_TMP0, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 1U);
            a64_emit_exit_branch_reg(b, A64_TMP0, false);
            if (is_uncond) {
                *out_ends_block = true;
            }
            return true;
        }
        if ((hw1 & 0xFFE0U) == 0xE840U) {
            /* LDREX / STREX Rt, [Rn, #imm8*4] */
            bool is_load = ((hw1 >> 4) & 1U) != 0U;
            unsigned rn = hw1 & 0xFU;
            unsigned rt = (hw2 >> 12) & 0xFU;
            unsigned rd = (hw2 >> 8) & 0xFU;
            uint16_t offset = (uint16_t)((hw2 & 0xFFU) << 2);
            a64_emit_add_imm_w(b, false, A64_ADDR, rn, offset, 0U);
            a64_emit_guest_addr_x(b, A64_ADDR, A64_ADDR, 4U, !is_load);
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
            if (op3 != 4U && op3 != 5U && op3 != 7U) {
                return false;
            }
            uint32_t ex_span = (op3 == 4U) ? 1U : (op3 == 5U) ? 2U : 8U;
            a64_emit_guest_addr_x(b, A64_ADDR, rn, ex_span, !is_load);
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
        unsigned base = a32_load_reg(b, rn, pc_align4, A64_TMP2);
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
            a64_emit_guest_ldrd_w(b, rt, rt2, A64_ADDR);
        } else {
            a64_emit_guest_strd_w(b, rt, rt2, A64_ADDR);
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
        if (!is_load && (sign_ext || rt == 15U)) {
            return false;
        }
        if (rn == 15U && !is_load) {
            return false;
        }

        /* Addressing mode via the shared T32 helper (A32-R13-10). */
        t32_ls_dec_t ls = t32_decode_ls(rn, rt, u_imm12, size, is_load, sign_ext, hw2);
        if (ls.undef) {
            return false;
        }

        if (ls.kind == 0U) {
            /* Literal load [PC, #+/-imm12] */
            uint32_t addr = ls.up ? (pc_align4 + ls.imm) : (pc_align4 - ls.imm);
            a64_emit_mov_pc32(b, A64_ADDR, addr);
        } else if (ls.kind == 1U) {
            /* 12-bit unsigned offset [Rn, #imm12] */
            a64_emit_add_imm_w(b, false, A64_ADDR, rn, ls.imm, 0U);
        } else if (ls.kind == 2U) {
            /* Shifted register offset [Rn, Rm, LSL #imm2] */
            a64_emit_add_reg_w(b, false, A64_ADDR, rn, ls.rm, A64_SHIFT_LSL, ls.shift);
        } else if (ls.pre) {
            /* 8-bit pre-indexed immediate [Rn, #+/-imm8] */
            if (ls.up) {
                a64_emit_add_imm_w(b, false, A64_ADDR, rn, ls.imm, 0U);
            } else {
                a64_emit_sub_imm_w(b, false, A64_ADDR, rn, ls.imm, 0U);
            }
        } else {
            a64_emit_mov_w(b, A64_ADDR, rn);
        }

        if (ls.hint) {
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

        if (!ls.pre && ls.wb) {
            if (ls.up) {
                a64_emit_add_imm_w(b, false, rn, rn, ls.imm, 0U);
            } else {
                a64_emit_sub_imm_w(b, false, rn, rn, ls.imm, 0U);
            }
        } else if (ls.wb) {
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
        return a64_emit_cp15(b, ((uint32_t)hw1 << 16) | hw2);
    }

    return false;
}

static bool t16_insn_uncond_ends_block(uint16_t insn)
{
    if ((insn >> 11) == 0x1CU) {
        return true;
    }
    if ((insn & 0xFF07U) == 0x4700U) {
        return true;
    }
    if ((insn & 0xFF00U) == 0xDF00U || (insn & 0xFF00U) == 0xDE00U) {
        return true;
    }
    if ((insn & 0xFF00U) == 0xBE00U) {
        return true;
    }
    if ((insn & 0xFF00U) == 0xBD00U) {
        return true;
    }
    if ((insn & 0xFF87U) == 0x4487U || (insn & 0xFF87U) == 0x4687U) {
        return true;
    }
    return false;
}

static bool t32_insn_uncond_ends_block(uint16_t hw1, uint16_t hw2)
{
    if ((hw1 & 0xF800U) == 0xF000U && (hw2 & 0x8000U) != 0U) {
        if ((hw2 & 0xD000U) == 0xD000U || (hw2 & 0xD000U) == 0xC000U ||
            (hw2 & 0xD000U) == 0x9000U) {
            return true;
        }
    }
    if ((hw1 & 0xFFF0U) == 0xF7F0U && (hw2 & 0xF000U) == 0xA000U) {
        return true;
    }
    if ((hw1 & 0xFFF0U) == 0xE8D0U && (hw2 & 0xFFE0U) == 0xF000U) {
        return true;
    }
    if ((hw1 & 0xFE40U) == 0xE800U &&
        (((hw1 >> 7) & 3U) == 1U || ((hw1 >> 7) & 3U) == 2U) &&
        ((hw1 >> 4) & 1U) != 0U && (hw2 & 0x8000U) != 0U) {
        return true;
    }
    if ((hw1 & 0xFF00U) == 0xF800U) {
        unsigned size = (hw1 >> 5) & 3U;
        bool is_load = ((hw1 >> 4) & 1U) != 0U;
        unsigned rt = (hw2 >> 12) & 0xFU;
        if (is_load && size == 2U && rt == 15U) {
            return true;
        }
    }
    if ((hw1 & 0xFA00U) == 0xF000U && (hw2 & 0x8000U) == 0U) {
        bool s_bit = ((hw1 >> 4) & 1U) != 0U;
        unsigned rd = (hw2 >> 8) & 0xFU;
        if (rd == 15U && !s_bit) {
            return true;
        }
    }
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
        if (top5 == 0x03U) {
            return true;
        }
        if (top5 >= 0x05U && top5 <= 0x07U) {
            return true;
        }
        uint16_t top10 = hw1 & 0xFFC0U;
        if (top10 == 0x4240U || top10 == 0x4280U || top10 == 0x42C0U) {
            return true;
        }
        if ((hw1 & 0xFF00U) == 0x4500U) {
            return true;
        }
        return false;
    }

    uint16_t hw1 = info->hw1;
    uint16_t hw2 = info->hw2;

    if ((hw1 & 0xFA00U) == 0xF000U && (hw2 & 0x8000U) == 0U) {
        bool s_bit = ((hw1 >> 4) & 1U) != 0U;
        unsigned op = (hw1 >> 5) & 0xFU;
        if (s_bit && (op == 0x8U || op == 0xDU || op == 0xEU)) {
            return true;
        }
    }

    if ((hw1 & 0xFE00U) == 0xEA00U && !((hw1 & 0xFFF0U) == 0xEAC0U && (hw2 & 0x8010U) == 0U)) {
        bool s_bit = ((hw1 >> 4) & 1U) != 0U;
        unsigned op = (hw1 >> 5) & 0xFU;
        bool is_rrx = (((hw2 >> 4) & 3U) == 3U && (((hw2 >> 12) & 7U) == 0U && ((hw2 >> 6) & 3U) == 0U));
        if (s_bit && (op == 0x8U || op == 0xDU || op == 0xEU) && !is_rrx) {
            return true;
        }
    }

    if (hw1 == 0xEEF1U && hw2 == 0xFA10U) {
        return true;
    }

    if ((hw1 & 0xFFF0U) == 0xF380U && (hw2 & 0xF0FFU) == 0x8000U && (hw2 & 0x0800U) != 0U) {
        return true;
    }

    return false;
}

static bool t32_insn_reads_flags(const t32_insn_info_t *info)
{
    uint16_t hw1 = info->hw1;
    if (!info->is_32bit) {
        if ((hw1 & 0xFF00U) == 0xDE00U) {
            return true;
        }
        if ((hw1 >> 12) == 0xDU && ((hw1 >> 8) & 0xFU) < 14U) {
            return true;
        }
        if ((hw1 & 0xF500U) == 0xB100U) {
            return true;
        }
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

    if ((hw1 & 0xFFF0U) == 0xF7F0U && (hw2 & 0xF000U) == 0xA000U) {
        return true;
    }

    if ((hw1 & 0xF800U) == 0xF000U && (hw2 & 0xD000U) == 0x8000U) {
        unsigned cond = (hw1 >> 6) & 0xFU;
        if (cond < 14U) {
            return true;
        }
    }

    if ((hw1 & 0xFA00U) == 0xF000U && (hw2 & 0x8000U) == 0U) {
        unsigned op = (hw1 >> 5) & 0xFU;
        if (op == 0xAU || op == 0xBU) {
            return true;
        }
    }

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

    if ((hw1 & 0xFF90U) == 0xFA10U && (hw2 & 0xF0F0U) == 0xF000U) {
        return true;
    }

    if (hw1 == 0xF3EFU && (hw2 & 0xF0FFU) == 0x8000U) {
        return true;
    }

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
        if ((hw1 >> 14) == 0U && top5 < 3U) {
            return true;
        }
        if (top5 == 0x04U) {
            return true;
        }
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

    if ((hw1 & 0xFA00U) == 0xF000U && (hw2 & 0x8000U) == 0U) {
        bool s_bit = ((hw1 >> 4) & 1U) != 0U;
        unsigned op = (hw1 >> 5) & 0xFU;
        if (s_bit && op <= 0x4U) {
            return true;
        }
    }

    if ((hw1 & 0xFE00U) == 0xEA00U && !((hw1 & 0xFFF0U) == 0xEAC0U && (hw2 & 0x8010U) == 0U)) {
        bool s_bit = ((hw1 >> 4) & 1U) != 0U;
        unsigned op = (hw1 >> 5) & 0xFU;
        if (s_bit && op <= 0x4U) {
            return true;
        }
    }

    if ((hw1 & 0xFF80U) == 0xFA00U && (hw2 & 0xF0F0U) == 0xF000U) {
        bool s_bit = ((hw1 >> 4) & 1U) != 0U;
        if (s_bit) {
            return true;
        }
    }

    return false;
}

/* Synchronous-exit observation points for flag liveness (A32-R7-4):
 * memory accesses, trap entrances and partially-decoded coprocessor
 * classes whose handler observes the host NZCV state. Pure ALU and
 * branch instructions are fully decoded for defined encodings and
 * fault-free, so they are not observation points. */
static bool t32_insn_may_fault(const t32_insn_info_t *info)
{
    uint16_t hw1 = info->hw1;
    if (!info->is_32bit) {
        if ((hw1 & 0xFF00U) == 0xDF00U) {
            return true;
        }
        if ((hw1 & 0xFF00U) == 0xDE00U) {
            return true;
        }
        if ((hw1 & 0xFF00U) == 0xBE00U) {
            return true;
        }
        if ((hw1 >> 11) == 0x09U) {
            return true;
        }
        if ((hw1 >> 12) == 0x5U) {
            return true;
        }
        if ((hw1 >> 13) == 0x3U) {
            return true;
        }
        if ((hw1 >> 12) == 0x8U) {
            return true;
        }
        if ((hw1 >> 12) == 0x9U) {
            return true;
        }
        if ((hw1 & 0xF000U) == 0xC000U) {
            return true;
        }
        if ((hw1 & 0xFE00U) == 0xB400U) {
            return true;
        }
        if ((hw1 & 0xFE00U) == 0xBC00U) {
            return true;
        }
        return false;
    }
    /* EC-EF coprocessor / VFP / NEON, F3 misc and traps, F4-FF SIMD,
     * memory and syscall-trap families: faultable or trap. F0/F1/F2 DP
     * immediate, EA/EB DP register and FA/FB multiply/parallel groups are
     * fully decoded for defined encodings and fault-free. */
    uint8_t h1hi = (uint8_t)(hw1 >> 8);
    if (h1hi >= 0xECU && h1hi < 0xF0U) {
        return true;
    }
    if (h1hi >= 0xF3U) {
        return true;
    }
    if (h1hi == 0xF2U && (info->hw2 & 0x8000U) != 0U) {
        return true;
    }
    /* T32 LDMIA/STMIA/PUSH/POP (E8/E9). */
    if ((hw1 & 0xF000U) == 0xE000U && (hw1 & 0x0E00U) == 0x0800U) {
        return true;
    }
    /* T32 LDREX/STREX family. */
    if ((hw1 & 0xFFF0U) == 0xEAC0U && (info->hw2 & 0x8010U) == 0U) {
        return true;
    }
    /* Remaining 32-bit data processing and branches: fully decoded for
     * defined encodings and fault-free, so not observation points. */
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

    uint32_t cur_page = pc & ~0xFFFU;
    uint32_t scan_pc = pc;
    uint8_t  scan_itstate = itstate;

    /* Pre-pass: scan guest instructions in the basic block */
    for (unsigned count = 0U; (count < max_insns || (!ctx->single_step && scan_itstate != 0U)) && num_insns < T32_MAX_PRESCAN_INSNS; count++) {
        if (count > 0U && scan_itstate == 0U && (scan_pc - pc) >= 496U) {
            break;
        }
        if ((scan_pc & ~0xFFFU) != cur_page) {
            if (!a32_jit_range_executable(ctx, scan_pc, 2U)) {
                break;
            }
            cur_page = scan_pc & ~0xFFFU;
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
        if ((hw1 & 0xFF00U) == 0xBF00U && (hw1 & 0x000FU) != 0U && (scan_itstate & 0x0FU) == 0U) {
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
            if (((scan_pc + 2U) & ~0xFFFU) != cur_page) {
                if (!a32_jit_range_executable(ctx, scan_pc + 2U, 2U)) {
                    if (count > 0U) {
                        break;
                    }
                    if (itstate != 0U) {
                        a64_emit_mov_imm32(b, A64_TMP0, itstate);
                        a64_emit_str_imm_w(b, A64_TMP0, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, itstate));
                    }
                    a64_emit_exit_undef(b, pc);
                    b->guest_end_pc = pc + 2U;
                    return true;
                }
                cur_page = (scan_pc + 2U) & ~0xFFFU;
            }
            uint16_t hw2 = guest_hw[1];
            info->hw2 = hw2;
            scan_pc += 4U;
            if (scan_uncond && t32_insn_uncond_ends_block(hw1, hw2)) {
                ends_uncond = true;
            } else if (!in_it && (hw1 & 0xF800U) == 0xF000U && (hw2 & 0xD000U) == 0x8000U &&
                       ((hw1 >> 6) & 0xFU) < 14U) {
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
                if ((info->pc + 4U + (uint32_t)offset) == pc) {
                    ends_uncond = true;
                }
            }
        } else {
            scan_pc += 2U;
            if ((scan_uncond && t16_insn_uncond_ends_block(hw1)) || (hw1 & 0xFF00U) == 0xBE00U) {
                ends_uncond = true;
            } else if (!in_it && (hw1 >> 12) == 0xDU && ((hw1 >> 8) & 0xFU) < 14U) {
                int32_t offset = ((int32_t)(int8_t)(hw1 & 0xFFU)) << 1;
                if ((info->pc + 4U + (uint32_t)offset) == pc) {
                    ends_uncond = true;
                }
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

    /* Backward pass: determine NZCV flag liveness. Synchronous exits
     * observe host NZCV in the fault handler, so faultable instructions
     * keep earlier flag writes live (A32-R7-4). */
    bool live = true;
    for (int i = (int)num_insns - 1; i >= 0; i--) {
        const t32_insn_info_t *info = &block_insns[i];
        flags_live[i] = live;
        if (t32_insn_overwrites_all_flags(info)) {
            live = false;
        } else if (info->is_it || info->itstate != 0U || t32_insn_reads_flags(info) ||
                   (live && t32_insn_partially_updates_flags(info)) ||
                   t32_insn_may_fault(info)) {
            live = true;
        }
    }

    /* Translation & emission loop */
    for (unsigned idx = 0U; idx < num_insns; idx++) {
        const t32_insn_info_t *info = &block_insns[idx];
        uint32_t cur_pc = info->pc;
        uint16_t hw1 = info->hw1;

        if (idx > 0U && itstate == 0U && (b->count + 1536U >= b->cap)) {
            b->guest_end_pc = cur_pc;
            a64_emit_exit_branch_imm(b, cur_pc, false);
            return true;
        }

        if (info->is_it) {
            itstate = (uint8_t)(hw1 & 0xFFU);
            continue;
        }

        /* Record host_word_idx -> (guest_pc, itstate) in JIT side-table */
        a32_jit_record_pc_map((uint32_t)b->count, cur_pc, itstate);

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
            ok = a32_translate_t32_insn(ctx, b, cur_pc, hw1, hw2, is_uncond, flags_live[idx], &ends_block);
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
            ok = a32_translate_t16_insn(ctx, b, cur_pc, hw1, in_it, itstate, next_itstate, is_uncond, flags_live[idx], &ends_block);
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
