/*
 * src/interp/interp_pred_imm.c — Predicated immediates & logical-imm helpers (A32-R20-P7..P10)
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "a64_emit.h"

void a64_emit_movn_w(a64_buf_t *b, unsigned rd, uint16_t imm16, unsigned hw)
{
    a64_emit(b, 0x12800000U | ((hw & 1U) << 21) | ((uint32_t)imm16 << 5) | (rd & 31U));
}

static uint32_t ror_elem(uint32_t v, unsigned rot, unsigned esize)
{
    rot %= esize;
    if (rot == 0U) {
        return v & ((esize >= 32U) ? 0xFFFFFFFFU : ((1U << esize) - 1U));
    }
    uint32_t mask = (esize >= 32U) ? 0xFFFFFFFFU : ((1U << esize) - 1U);
    v &= mask;
    return ((v >> rot) | (v << (esize - rot))) & mask;
}

bool a64_encode_logical_imm32(uint32_t imm, unsigned *out_imms, unsigned *out_immr)
{
    static const unsigned k_sizes[5] = { 32U, 16U, 8U, 4U, 2U };
    for (unsigned si = 0U; si < 5U; si++) {
        unsigned esize = k_sizes[si];
        uint32_t emask = (esize >= 32U) ? 0xFFFFFFFFU : ((1U << esize) - 1U);
        uint32_t elem = imm & emask;
        unsigned reps = 32U / esize;
        uint32_t rep = 0U;
        for (unsigned r = 0U; r < reps; r++) {
            rep = (rep << esize) | elem;
        }
        if (rep != imm) {
            continue;
        }
        for (unsigned ones = 1U; ones < esize; ones++) {
            uint32_t base;
            if (ones >= 32U) {
                base = 0xFFFFFFFFU;
            } else if (esize >= 32U) {
                base = (ones == 32U) ? 0xFFFFFFFFU : ((1U << ones) - 1U);
            } else {
                base = (uint32_t)((1U << ones) - 1U);
            }
            for (unsigned rot = 0U; rot < esize; rot++) {
                if (ror_elem(base, rot, esize) != elem) {
                    continue;
                }
                unsigned s = ones - 1U;
                unsigned imms = 0U;
                unsigned immr = 0U;
                if (esize == 32U) {
                    imms = s & 0x3FU;
                    immr = rot & 0x3FU;
                } else if (esize == 16U) {
                    imms = 0x20U | (s & 0xFU);
                    immr = rot & 0xFU;
                } else if (esize == 8U) {
                    imms = 0x30U | (s & 0x7U);
                    immr = rot & 0x7U;
                } else if (esize == 4U) {
                    imms = 0x38U | (s & 0x3U);
                    immr = rot & 0x3U;
                } else {
                    imms = 0x3CU | (s & 0x1U);
                    immr = rot & 0x1U;
                }
                if (out_imms != NULL) {
                    *out_imms = imms;
                }
                if (out_immr != NULL) {
                    *out_immr = immr;
                }
                return true;
            }
        }
    }
    return false;
}

void a64_emit_logical_imm_w(a64_buf_t *b, unsigned opc, unsigned rd, unsigned rn, uint32_t imm)
{
    unsigned imms = 0U;
    unsigned immr = 0U;
    if (!a64_encode_logical_imm32(imm, &imms, &immr)) {
        return;
    }
    a64_emit(b, ((opc & 3U) << 29) | 0x12000000U | ((immr & 0x3FU) << 16) |
        ((imms & 0x3FU) << 10) | ((rn & 31U) << 5) | (rd & 31U));
}

void a64_emit_csinc_w(a64_buf_t *b, unsigned rd, unsigned rn, unsigned rm, unsigned cond)
{
    a64_emit(b, 0x1A800400U | ((rm & 31U) << 16) | ((cond & 15U) << 12) |
        ((rn & 31U) << 5) | (rd & 31U));
}

void a64_emit_csinv_w(a64_buf_t *b, unsigned rd, unsigned rn, unsigned rm, unsigned cond)
{
    a64_emit(b, 0x5A800000U | ((rm & 31U) << 16) | ((cond & 15U) << 12) |
        ((rn & 31U) << 5) | (rd & 31U));
}

/* 4-bit NZCV condition use mask: bit3=N, bit2=Z, bit1=C, bit0=V. */
uint8_t a32_pred_cond_mask(unsigned cond)
{
    switch (cond & 15U) {
    case 0U:
    case 1U:
        return 0x4U;
    case 2U:
    case 3U:
        return 0x2U;
    case 4U:
    case 5U:
        return 0x8U;
    case 6U:
    case 7U:
        return 0x1U;
    case 8U:
    case 9U:
        return 0x6U;
    case 10U:
    case 11U:
        return 0x9U;
    case 12U:
    case 13U:
        return 0xDU;
    default:
        return 0U;
    }
}

/* Fold ADD/SUB immediate into 1 A64 instruction. Returns true when emitted. */
bool a32_pred_fold_addsub_imm(a64_buf_t *b, bool is_sub, bool set_flags,
    unsigned dst, unsigned src_n, uint32_t imm32)
{
    if (imm32 < 4096U) {
        if (is_sub) {
            a64_emit_sub_imm_w(b, set_flags, dst, src_n, (uint16_t)imm32, 0U);
        } else {
            a64_emit_add_imm_w(b, set_flags, dst, src_n, (uint16_t)imm32, 0U);
        }
        return true;
    }
    if ((imm32 & 0xFFFU) == 0U && (imm32 >> 12) < 4096U) {
        uint16_t hi = (uint16_t)(imm32 >> 12);
        if (is_sub) {
            a64_emit_sub_imm_w(b, set_flags, dst, src_n, hi, 1U);
        } else {
            a64_emit_add_imm_w(b, set_flags, dst, src_n, hi, 1U);
        }
        return true;
    }
    return false;
}

/* Emit 1-instruction logical immediate for S=0. Returns true when encodable. */
bool a32_pred_try_logical_imm(a64_buf_t *b, unsigned opc,
    unsigned dst, unsigned src_n, uint32_t eff_imm)
{
    unsigned imms = 0U;
    unsigned immr = 0U;
    if (!a64_encode_logical_imm32(eff_imm, &imms, &immr)) {
        return false;
    }
    a64_emit_logical_imm_w(b, opc, dst, src_n, eff_imm);
    return true;
}

/* S=1 logical immediate with 4-bit liveness. Handles elision, ANDS fast path,
 * and ORR/EOR 2-instruction fast path. Caller guarantees eff_imm encodable. */
void a32_pred_logical_imm_s1(a64_buf_t *b, a32_ctx_t *ctx, unsigned base_opc,
    unsigned dst, unsigned src_n, uint32_t eff_imm, bool has_imm_carry,
    unsigned imm_carry, uint8_t live_out, bool is_test)
{
    unsigned real_dst = is_test ? A64_TMP0 : dst;
    if (live_out == 0U) {
        a64_emit_logical_imm_w(b, base_opc, real_dst, src_n, eff_imm);
        ctx->flags_elided++;
        return;
    }
    if ((live_out & 0x3U) == 0U) {
        if (base_opc == 0U) {
            unsigned fast_dst = is_test ? (unsigned)A64_ZR : dst;
            a64_emit_logical_imm_w(b, 3U, fast_dst, src_n, eff_imm);
            return;
        }
        a64_emit_logical_imm_w(b, base_opc, real_dst, src_n, eff_imm);
        a64_emit_and_reg_w(b, true, A64_ZR, real_dst, real_dst,
            A64_SHIFT_LSL, 0U);
        return;
    }
    a64_emit_logical_imm_w(b, base_opc, real_dst, src_n, eff_imm);
    a64_emit_logical_flags(b, real_dst, has_imm_carry, imm_carry, A64_ZR);
}

/* S=1 logical register with 4-bit liveness. op_class: 0=AND,1=BIC,2=ORR,
 * 3=ORN,4=EOR. MOV is ORR with zr, MVN is ORN with zr. */
void a32_pred_logical_reg_s1(a64_buf_t *b, a32_ctx_t *ctx, unsigned op_class,
    unsigned dst, unsigned src_n, unsigned src_m, unsigned shift,
    unsigned amount, bool has_imm_carry, unsigned imm_carry,
    unsigned shift_carry_reg, uint8_t live_out)
{
    if (live_out == 0U) {
        switch (op_class) {
        case 0U:
            a64_emit_and_reg_w(b, false, dst, src_n, src_m, shift, amount);
            break;
        case 1U:
            a64_emit_bic_reg_w(b, false, dst, src_n, src_m, shift, amount);
            break;
        case 2U:
            a64_emit_orr_reg_w(b, dst, src_n, src_m, shift, amount);
            break;
        case 3U:
            a64_emit_orn_reg_w(b, dst, src_n, src_m, shift, amount);
            break;
        default:
            a64_emit_eor_reg_w(b, dst, src_n, src_m, shift, amount);
            break;
        }
        ctx->flags_elided++;
        return;
    }
    if ((live_out & 0x3U) == 0U) {
        if (op_class == 0U) {
            a64_emit_and_reg_w(b, true, dst, src_n, src_m, shift, amount);
            return;
        }
        if (op_class == 1U) {
            a64_emit_bic_reg_w(b, true, dst, src_n, src_m, shift, amount);
            return;
        }
        switch (op_class) {
        case 2U:
            a64_emit_orr_reg_w(b, dst, src_n, src_m, shift, amount);
            break;
        case 3U:
            a64_emit_orn_reg_w(b, dst, src_n, src_m, shift, amount);
            break;
        default:
            a64_emit_eor_reg_w(b, dst, src_n, src_m, shift, amount);
            break;
        }
        a64_emit_and_reg_w(b, true, A64_ZR, dst, dst, A64_SHIFT_LSL, 0U);
        return;
    }
    switch (op_class) {
    case 0U:
        a64_emit_and_reg_w(b, false, dst, src_n, src_m, shift, amount);
        break;
    case 1U:
        a64_emit_bic_reg_w(b, false, dst, src_n, src_m, shift, amount);
        break;
    case 2U:
        a64_emit_orr_reg_w(b, dst, src_n, src_m, shift, amount);
        break;
    case 3U:
        a64_emit_orn_reg_w(b, dst, src_n, src_m, shift, amount);
        break;
    default:
        a64_emit_eor_reg_w(b, dst, src_n, src_m, shift, amount);
        break;
    }
    a64_emit_logical_flags(b, dst, has_imm_carry, imm_carry, shift_carry_reg);
}

/* Fuse immediate barrel-shifter into A64 shifted-register form.
 * Returns true with out_shift/out_amount when fusable. */
bool a32_pred_fuse_imm_shift(unsigned shift_type, unsigned imm5, bool is_logical,
    bool want_carry, unsigned *out_shift, unsigned *out_amount)
{
    if (want_carry) {
        return false;
    }
    if (shift_type == 0U) {
        if (imm5 == 0U) {
            if (out_shift != NULL) {
                *out_shift = A64_SHIFT_LSL;
            }
            if (out_amount != NULL) {
                *out_amount = 0U;
            }
            return true;
        }
        if (out_shift != NULL) {
            *out_shift = A64_SHIFT_LSL;
        }
        if (out_amount != NULL) {
            *out_amount = imm5;
        }
        return true;
    }
    if (shift_type == 1U) {
        if (imm5 == 0U) {
            return false;
        }
        if (out_shift != NULL) {
            *out_shift = A64_SHIFT_LSR;
        }
        if (out_amount != NULL) {
            *out_amount = imm5;
        }
        return true;
    }
    if (shift_type == 2U) {
        if (imm5 == 0U) {
            if (out_shift != NULL) {
                *out_shift = A64_SHIFT_ASR;
            }
            if (out_amount != NULL) {
                *out_amount = 31U;
            }
            return true;
        }
        if (out_shift != NULL) {
            *out_shift = A64_SHIFT_ASR;
        }
        if (out_amount != NULL) {
            *out_amount = imm5;
        }
        return true;
    }
    if (imm5 == 0U) {
        return false;
    }
    if (!is_logical) {
        return false;
    }
    if (out_shift != NULL) {
        *out_shift = A64_SHIFT_ROR;
    }
    if (out_amount != NULL) {
        *out_amount = imm5;
    }
    return true;
}

/* A32 use mask: cond + C consumers + 0xF for may-exit observation points. */
static bool a32_pred_may_exit_raw(uint32_t insn)
{
    if ((insn & 0x0F000000U) == 0x0F000000U) {
        return true;
    }
    if ((insn & 0x0FF000F0U) == 0x01200070U) {
        return true;
    }
    if ((insn & 0x0FF000F0U) == 0x07F000F0U) {
        return true;
    }
    if ((insn & 0x0FB00FF0U) == 0x01000090U) {
        return true;
    }
    if ((insn & 0x0F8000F0U) == 0x01800090U) {
        return true;
    }
    if ((insn & 0x0E000000U) == 0x0C000000U) {
        return true;
    }
    if ((insn & 0x0F000000U) == 0x0E000000U) {
        return true;
    }
    if (((insn >> 28) & 0xFU) == 0xFU) {
        if ((insn & 0xFE000000U) == 0xF2000000U ||
            (insn & 0xFF100000U) == 0xF4000000U ||
            (insn & 0xFE000000U) == 0xFE000000U ||
            (insn & 0x0FFFFFF0U) == 0x07F000F0U) {
            return true;
        }
        return false;
    }
    unsigned op1 = (insn >> 25) & 7U;
    if (op1 == 4U) {
        return true;
    }
    if (op1 == 2U || (op1 == 3U && ((insn & 0x10U) == 0U))) {
        return true;
    }
    if (op1 == 3U) {
        return true;
    }
    return false;
}

uint8_t a32_pred_use_a32(uint32_t insn)
{
    if (a32_pred_may_exit_raw(insn)) {
        return 0xFU;
    }
    {
        unsigned op1e = (insn >> 25) & 7U;
        if (op1e == 0U && (insn & 0x90U) == 0x90U && (insn & 0x60U) != 0U) {
            return 0xFU;
        }
        if (op1e == 5U) {
            return 0xFU;
        }
        if (((insn >> 28) & 0xFU) == 0xFU && (insn & 0xFE000000U) == 0xFA000000U) {
            return 0xFU;
        }
        if ((insn & 0x0FFFFFF0U) == 0x012FFF10U ||
            (insn & 0x0FFFFFF0U) == 0x012FFF20U ||
            (insn & 0x0FFFFFF0U) == 0x012FFF30U) {
            return 0xFU;
        }
        if (op1e == 4U && ((insn >> 20) & 1U) != 0U && (insn & (1U << 15)) != 0U) {
            return 0xFU;
        }
        if ((op1e == 2U || (op1e == 3U && ((insn & 0x10U) == 0U))) &&
            (((insn >> 20) & 1U) != 0U) && (((insn >> 12) & 0xFU) == 15U)) {
            return 0xFU;
        }
        if ((op1e == 0U || op1e == 1U) && (((insn >> 12) & 0xFU) == 15U)) {
            unsigned opc = (insn >> 21) & 0xFU;
            if (opc < 8U || opc >= 12U) {
                return 0xFU;
            }
        }
    }
    unsigned cond = (insn >> 28) & 0xFU;
    uint8_t use = 0U;
    if (cond < 14U) {
        use |= a32_pred_cond_mask(cond);
    } else if (cond == 15U) {
        if ((insn & 0xFF800E50U) == 0xFE000A00U) {
            use |= 0xFU;
        }
        return use;
    }
    if (cond == 14U) {
        unsigned op1 = (insn >> 25) & 7U;
        if (op1 == 0U || op1 == 1U) {
            if (op1 == 0U && (insn & 0x0E000090U) == 0x00000090U) {
                return use;
            }
            if (op1 == 0U && (insn & 0x0F8000F0U) == 0x01800090U) {
                return use;
            }
            if (op1 == 0U && (insn & 0x0FBF0FFFU) == 0x010F0000U) {
                return (uint8_t)(use | 0xFU);
            }
            if ((insn & 0x0FF000F0U) == 0x07F000F0U) {
                return (uint8_t)(use | 0xFU);
            }
            unsigned opc = (insn >> 21) & 0xFU;
            if (opc == 5U || opc == 6U || opc == 7U) {
                use |= 0x2U;
            }
            if (op1 == 0U && ((insn & 0x00000FF0U) == 0x00000060U)) {
                use |= 0x2U;
            }
            if (op1 == 0U && ((insn & 0x00100090U) == 0x00100010U) &&
                (opc == 0U || opc == 1U || opc == 8U || opc == 9U || opc >= 12U)) {
                use |= 0x2U;
            }
        } else if (op1 == 3U && ((insn & 0x10U) == 0U)) {
            if ((insn & 0x00000FF0U) == 0x00000060U) {
                use |= 0x2U;
            }
        }
    } else {
        use |= a32_pred_cond_mask(cond);
        unsigned op1 = (insn >> 25) & 7U;
        if (op1 == 0U && (insn & 0x0FBF0FFFU) == 0x010F0000U) {
            return (uint8_t)(use | 0xFU);
        }
        if ((op1 == 0U || op1 == 1U)) {
            unsigned opc = (insn >> 21) & 0xFU;
            if (opc == 5U || opc == 6U || opc == 7U) {
                use |= 0x2U;
            }
            if (op1 == 0U && ((insn & 0x00000FF0U) == 0x00000060U)) {
                use |= 0x2U;
            }
            if (op1 == 0U && ((insn & 0x00100090U) == 0x00100010U) &&
                (opc == 0U || opc == 1U || opc == 8U || opc == 9U || opc >= 12U)) {
                use |= 0x2U;
            }
        } else if (op1 == 3U && ((insn & 0x10U) == 0U)) {
            if ((insn & 0x00000FF0U) == 0x00000060U) {
                use |= 0x2U;
            }
        }
    }
    return use;
}

/* A32 kill mask: only unconditional outside-predication kills. */
uint8_t a32_pred_kill_a32(uint32_t insn)
{
    unsigned cond = (insn >> 28) & 0xFU;
    if (cond != 14U) {
        return 0U;
    }
    unsigned op1 = (insn >> 25) & 7U;
    if (op1 == 0U || op1 == 1U) {
        if (op1 == 0U && (insn & 0x0E000090U) == 0x00000090U) {
            return 0U;
        }
        if (op1 == 0U && (insn & 0x0F8000F0U) == 0x01800090U) {
            return 0U;
        }
        if ((insn & 0x0FF0FFF0U) == 0x0120F000U && ((insn >> 19) & 1U) != 0U) {
            return 0xFU;
        }
        if (op1 == 1U && (insn & 0x0FF0F000U) == 0x0320F000U && ((insn >> 19) & 1U) != 0U) {
            return 0xFU;
        }
        unsigned opc = (insn >> 21) & 0xFU;
        bool s_bit = ((insn >> 20) & 1U) != 0U;
        if (!s_bit) {
            return 0U;
        }
        if (opc == 10U || opc == 11U) {
            return 0xFU;
        }
        if (opc >= 2U && opc <= 7U) {
            return 0xFU;
        }
        if (opc == 0U || opc == 1U || opc == 8U || opc == 9U || opc >= 12U) {
            uint8_t kill = 0xCU;
            if (op1 == 1U) {
                unsigned rot = ((insn >> 8) & 0xFU) * 2U;
                if (rot != 0U) {
                    kill |= 0x2U;
                }
            } else {
                bool by_reg = ((insn >> 4) & 1U) != 0U;
                if (!by_reg) {
                    unsigned st = (insn >> 5) & 3U;
                    unsigned imm5 = (insn >> 7) & 31U;
                    bool defines_c = true;
                    if (st == 0U && imm5 == 0U) {
                        defines_c = false;
                    }
                    if (defines_c) {
                        kill |= 0x2U;
                    }
                }
            }
            return kill;
        }
    } else if (op1 == 7U) {
        if ((insn & 0x0FFFFFFFU) == 0x0EF1FA10U) {
            return 0xFU;
        }
    }
    return 0U;
}

/* True when an A32 insn would write NZCV if its condition passed. */
bool a32_pred_writes_nzcv_a32(uint32_t insn)
{
    unsigned op1 = (insn >> 25) & 7U;
    if (op1 == 0U || op1 == 1U) {
        if (op1 == 0U && (insn & 0x0E000090U) == 0x00000090U) {
            if ((insn & 0x0FC000F0U) == 0x00000090U ||
                (insn & 0x0F8000F0U) == 0x00800090U) {
                return ((insn & (1U << 20)) != 0U);
            }
            return false;
        }
        if (op1 == 0U && (insn & 0x0F8000F0U) == 0x01800090U) {
            return false;
        }
        unsigned opc = (insn >> 21) & 0xFU;
        bool s_bit = ((insn >> 20) & 1U) != 0U;
        if (s_bit && (opc <= 15U)) {
            if (op1 == 0U && (insn & 0x0E000090U) == 0x00000090U) {
                if ((insn & 0x0FC000F0U) == 0x00000090U ||
                    (insn & 0x0F8000F0U) == 0x00800090U) {
                    return ((insn & (1U << 20)) != 0U);
                }
                return false;
            }
            return true;
        }
        if ((insn & 0x0FF0FFF0U) == 0x0120F000U && ((insn >> 19) & 1U) != 0U) {
            return true;
        }
        if (op1 == 1U && (insn & 0x0FF0F000U) == 0x0320F000U && ((insn >> 19) & 1U) != 0U) {
            return true;
        }
        return false;
    }
    if (op1 == 7U && (insn & 0x0FFFFFFFU) == 0x0EF1FA10U) {
        return true;
    }
    return false;
}

/* True when an A32 insn writes PC (Rd==15, Rt==15 load, LDM-PC, B/BL/BX). */
bool a32_pred_writes_pc_a32(uint32_t insn)
{
    unsigned cond = (insn >> 28) & 0xFU;
    if (cond == 15U) {
        if ((insn & 0xFE000000U) == 0xFA000000U) {
            return true;
        }
        return false;
    }
    unsigned op1 = (insn >> 25) & 7U;
    if (op1 == 5U) {
        return true;
    }
    if (op1 == 7U && (insn & 0x0F000000U) == 0x0F000000U) {
        return true;
    }
    if ((insn & 0x0FF000F0U) == 0x01200070U) {
        return true;
    }
    if ((insn & 0x0FF000F0U) == 0x07F000F0U) {
        return true;
    }
    if (op1 == 0U && ((insn & 0x0FFFFFF0U) == 0x012FFF10U ||
        (insn & 0x0FFFFFF0U) == 0x012FFF20U ||
        (insn & 0x0FFFFFF0U) == 0x012FFF30U)) {
        return true;
    }
    if (op1 == 4U) {
        bool l_bit = ((insn >> 20) & 1U) != 0U;
        if (l_bit && (insn & (1U << 15)) != 0U) {
            return true;
        }
        return false;
    }
    if (op1 == 2U || (op1 == 3U && ((insn & 0x10U) == 0U))) {
        bool l_bit = ((insn >> 20) & 1U) != 0U;
        unsigned rt = (insn >> 12) & 0xFU;
        if (l_bit && rt == 15U) {
            return true;
        }
        return false;
    }
    if (op1 == 0U || op1 == 1U) {
        if (op1 == 0U && (insn & 0x0E000090U) == 0x00000090U) {
            return false;
        }
        if (op1 == 0U && (insn & 0x0F8000F0U) == 0x01800090U) {
            return false;
        }
        unsigned opc = (insn >> 21) & 0xFU;
        unsigned rd = (insn >> 12) & 0xFU;
        if (rd == 15U && (opc < 8U || opc >= 12U)) {
            return true;
        }
    }
    if ((insn & 0x0E000000U) == 0x0C000000U) {
        return true;
    }
    if ((insn & 0x0F000000U) == 0x0E000000U) {
        return true;
    }
    return false;
}

/* Mergeable A32 predicated op: same-cond skip region candidate. Excludes
 * flag writers, PC writers, branches/traps and coprocessor/VFP. */
bool a32_pred_mergeable_a32(uint32_t insn)
{
    unsigned cond = (insn >> 28) & 0xFU;
    if (cond >= 14U) {
        return false;
    }
    if (a32_pred_writes_nzcv_a32(insn)) {
        return false;
    }
    if (a32_pred_writes_pc_a32(insn)) {
        return false;
    }
    unsigned op1 = (insn >> 25) & 7U;
    if (op1 == 5U) {
        return false;
    }
    if (op1 == 6U || op1 == 7U) {
        return false;
    }
    if (op1 == 0U && (insn & 0x0FFFFFF0U) == 0x012FFF10U) {
        return false;
    }
    if (op1 == 0U && (insn & 0x0FFFFFF0U) == 0x012FFF30U) {
        return false;
    }
    if ((insn & 0x0FF000F0U) == 0x01200070U) {
        return false;
    }
    if ((insn & 0x0FF000F0U) == 0x07F000F0U) {
        return false;
    }
    if ((insn & 0x0E000000U) == 0x0C000000U) {
        return false;
    }
    if ((insn & 0x0F000000U) == 0x0E000000U) {
        return false;
    }
    if (op1 == 4U || op1 == 2U || (op1 == 3U)) {
        return true;
    }
    if (op1 == 0U || op1 == 1U) {
        return true;
    }
    return false;
}

/* Branchless predication kinds: 0=MOV,1=MVN,2=ADD,3=SUB,4=RSB,5=AND,6=ORR,
 * 7=EOR,8=BIC,9=ORN. Returns true when emitted without any b.cond. */
bool a32_pred_branchless(a64_buf_t *b, unsigned cond, unsigned pred_op,
    unsigned rd, unsigned rn, unsigned rm, bool is_imm, uint32_t imm32,
    unsigned shift_type, unsigned shift_amt)
{
    if (cond >= 14U) {
        return false;
    }
    if (rd >= 15U || rn >= 15U || (!is_imm && rm >= 15U)) {
        return false;
    }
    if (pred_op > 9U) {
        return false;
    }
    bool needs_rn = (pred_op != 0U || is_imm) && pred_op != 1U;
    if (pred_op == 0U && !is_imm) {
        needs_rn = false;
    }
    if (pred_op == 1U && !is_imm) {
        needs_rn = false;
    }
    if (needs_rn && rn >= 15U) {
        return false;
    }
    if (!is_imm && shift_type != A64_SHIFT_LSL && shift_amt == 0U) {
        return false;
    }
    if (!is_imm && (pred_op == 2U || pred_op == 3U) && shift_type == A64_SHIFT_ROR) {
        return false;
    }
    unsigned inv = cond ^ 1U;
    if (pred_op == 0U && !is_imm && shift_type == 0U && shift_amt == 0U) {
        a64_emit_csel_w(b, rd, rm, rd, cond);
        return true;
    }
    if (pred_op == 0U && is_imm && imm32 == 0U) {
        a64_emit_csel_w(b, rd, A64_ZR, rd, cond);
        return true;
    }
    if (pred_op == 0U && is_imm && imm32 == 1U) {
        a64_emit_csinc_w(b, rd, rd, A64_ZR, inv);
        return true;
    }
    if (pred_op == 1U && is_imm && imm32 == 0U) {
        a64_emit_csinv_w(b, rd, rd, A64_ZR, inv);
        return true;
    }
    if (pred_op == 0U && is_imm && imm32 == 0xFFFFFFFFU) {
        a64_emit_csinv_w(b, rd, rd, A64_ZR, inv);
        return true;
    }
    if (pred_op == 1U && !is_imm && shift_type == 0U && shift_amt == 0U) {
        a64_emit_csinv_w(b, rd, rd, rm, inv);
        return true;
    }
    if (pred_op == 4U && is_imm && imm32 == 0U) {
        a64_emit_csneg_w(b, rd, rd, rn, inv);
        return true;
    }
    if (pred_op == 2U && is_imm && imm32 == 1U) {
        a64_emit_csinc_w(b, rd, rd, rn, inv);
        return true;
    }
    if (is_imm) {
        unsigned src_n = rn;
        switch (pred_op) {
        case 0U:
            a64_emit_mov_imm32(b, A64_TMP0, imm32);
            break;
        case 1U:
            a64_emit_mov_imm32(b, A64_TMP0, ~imm32);
            break;
        case 2U:
            if (!a32_pred_fold_addsub_imm(b, false, false, A64_TMP0, src_n, imm32)) {
                a64_emit_mov_imm32(b, A64_TMP1, imm32);
                a64_emit_add_reg_w(b, false, A64_TMP0, src_n, A64_TMP1,
                    A64_SHIFT_LSL, 0U);
            }
            break;
        case 3U:
            if (!a32_pred_fold_addsub_imm(b, true, false, A64_TMP0, src_n, imm32)) {
                a64_emit_mov_imm32(b, A64_TMP1, imm32);
                a64_emit_sub_reg_w(b, false, A64_TMP0, src_n, A64_TMP1,
                    A64_SHIFT_LSL, 0U);
            }
            break;
        case 4U: {
            a64_emit_mov_imm32(b, A64_TMP1, imm32);
            a64_emit_sub_reg_w(b, false, A64_TMP0, A64_TMP1, src_n,
                A64_SHIFT_LSL, 0U);
            break;
        }
        case 5U:
            if (!a32_pred_try_logical_imm(b, 0U, A64_TMP0, src_n, imm32)) {
                a64_emit_mov_imm32(b, A64_TMP1, imm32);
                a64_emit_and_reg_w(b, false, A64_TMP0, src_n, A64_TMP1,
                    A64_SHIFT_LSL, 0U);
            }
            break;
        case 6U:
            if (!a32_pred_try_logical_imm(b, 1U, A64_TMP0, src_n, imm32)) {
                a64_emit_mov_imm32(b, A64_TMP1, imm32);
                a64_emit_orr_reg_w(b, A64_TMP0, src_n, A64_TMP1,
                    A64_SHIFT_LSL, 0U);
            }
            break;
        case 7U:
            if (!a32_pred_try_logical_imm(b, 2U, A64_TMP0, src_n, imm32)) {
                a64_emit_mov_imm32(b, A64_TMP1, imm32);
                a64_emit_eor_reg_w(b, A64_TMP0, src_n, A64_TMP1,
                    A64_SHIFT_LSL, 0U);
            }
            break;
        case 8U: {
            uint32_t eff = ~imm32;
            if (!a32_pred_try_logical_imm(b, 0U, A64_TMP0, src_n, eff)) {
                a64_emit_mov_imm32(b, A64_TMP1, imm32);
                a64_emit_bic_reg_w(b, false, A64_TMP0, src_n, A64_TMP1,
                    A64_SHIFT_LSL, 0U);
            }
            break;
        }
        default: {
            uint32_t eff = ~imm32;
            if (!a32_pred_try_logical_imm(b, 1U, A64_TMP0, src_n, eff)) {
                a64_emit_mov_imm32(b, A64_TMP1, imm32);
                a64_emit_orn_reg_w(b, A64_TMP0, src_n, A64_TMP1,
                    A64_SHIFT_LSL, 0U);
            }
            break;
        }
        }
        a64_emit_csel_w(b, rd, A64_TMP0, rd, cond);
        return true;
    }
    switch (pred_op) {
    case 0U:
        if (shift_type == 0U && shift_amt == 0U) {
            a64_emit_csel_w(b, rd, rm, rd, cond);
            return true;
        }
        a64_emit_orr_reg_w(b, A64_TMP0, A64_ZR, rm, shift_type, shift_amt);
        break;
    case 1U:
        a64_emit_orn_reg_w(b, A64_TMP0, A64_ZR, rm, shift_type, shift_amt);
        break;
    case 2U:
        a64_emit_add_reg_w(b, false, A64_TMP0, rn, rm, shift_type, shift_amt);
        break;
    case 3U:
        a64_emit_sub_reg_w(b, false, A64_TMP0, rn, rm, shift_type, shift_amt);
        break;
    case 4U:
        if (shift_type == 0U && shift_amt == 0U) {
            a64_emit_sub_reg_w(b, false, A64_TMP0, rm, rn, A64_SHIFT_LSL, 0U);
        } else {
            a64_emit_shift_imm_w(b, A64_TMP1, rm, shift_type, shift_amt,
                false, NULL);
            a64_emit_sub_reg_w(b, false, A64_TMP0, A64_TMP1, rn,
                A64_SHIFT_LSL, 0U);
        }
        break;
    case 5U:
        a64_emit_and_reg_w(b, false, A64_TMP0, rn, rm, shift_type, shift_amt);
        break;
    case 6U:
        a64_emit_orr_reg_w(b, A64_TMP0, rn, rm, shift_type, shift_amt);
        break;
    case 7U:
        a64_emit_eor_reg_w(b, A64_TMP0, rn, rm, shift_type, shift_amt);
        break;
    case 8U:
        a64_emit_bic_reg_w(b, false, A64_TMP0, rn, rm, shift_type, shift_amt);
        break;
    default:
        a64_emit_orn_reg_w(b, A64_TMP0, rn, rm, shift_type, shift_amt);
        break;
    }
    a64_emit_csel_w(b, rd, A64_TMP0, rd, cond);
    return true;
}

