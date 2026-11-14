/*
 * src/interp/interp_t32_step.c — 32-bit Thumb-2 (T32) instruction step
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "interp.h"
#include "interp_priv.h"
#include "interp/decode_priv.h"

/* Thumb PC read: Rn == 15 observes pc_val (pc + 4), never a stale r15. */
static uint32_t t32_reg(const a32_ctx_t *ctx, unsigned r, uint32_t pc_val)
{
    return (r == 15U) ? pc_val : ctx->regs[r];
}

static uint32_t t32_undef(a32_ctx_t *ctx, uint32_t next)
{
    ctx->regs[15] = next;
    i_undef(ctx);
    return A32_EXIT_UNDEF;
}

/* Thumb modified-immediate expansion (mirrors interp_t32_dp.c). */
static uint32_t t32_expand_imm(uint32_t imm12, bool *out_hc, unsigned *out_c)
{
    *out_hc = false;
    *out_c = 0U;
    if ((imm12 >> 10) == 0U) {
        uint32_t b = imm12 & 0xFFU;
        switch ((imm12 >> 8) & 3U) {
        case 0U: return b;
        case 1U: return (b << 16) | b;
        case 2U: return (b << 24) | (b << 8);
        default: return (b << 24) | (b << 16) | (b << 8) | b;
        }
    }
    uint32_t unrot = 0x80U | (imm12 & 0x7FU);
    unsigned rot = (imm12 >> 7) & 0x1FU;
    uint32_t res = i_ror32(unrot, rot);
    *out_hc = true;
    *out_c = (res >> 31) & 1U;
    return res;
}

/* Immediate shift (LSL/LSR/ASR/ROR#imm, RRX when st == 3 and imm5 == 0).
 * Returns the shifted value and resolves the carry-out (*cout). */
static uint32_t t32_imm_shift(uint32_t v, unsigned st, unsigned imm5,
    uint32_t cpsr, bool *cout)
{
    bool cin = (cpsr & A32_CPSR_C) != 0U;
    switch (st) {
    case 0U:
        if (imm5 == 0U) {
            *cout = cin;
            return v;
        }
        *cout = ((v >> (32U - imm5)) & 1U) != 0U;
        return v << imm5;
    case 1U:
        if (imm5 == 0U) {
            *cout = ((v >> 31) & 1U) != 0U;
            return 0U;
        }
        *cout = ((v >> (imm5 - 1U)) & 1U) != 0U;
        return v >> imm5;
    case 2U:
        if (imm5 == 0U) {
            *cout = ((v >> 31) & 1U) != 0U;
            return ((v & 0x80000000U) != 0U) ? 0xFFFFFFFFU : 0U;
        }
        *cout = ((v >> (imm5 - 1U)) & 1U) != 0U;
        if ((v & 0x80000000U) != 0U) {
            return (v >> imm5) | (0xFFFFFFFFU << (32U - imm5));
        }
        return v >> imm5;
    default:
        if (imm5 == 0U) {
            *cout = (v & 1U) != 0U;
            return (v >> 1U) | (cin ? 0x80000000U : 0U);
        }
        *cout = ((v >> (imm5 - 1U)) & 1U) != 0U;
        return i_ror32(v, imm5);
    }
}

/* Shared T32 data-processing body for modified-immediate and shifted
 * register forms. cbit is the resolved C (shifter/imm carry for logicals
 * with S, old C otherwise and the ADC/SBC input). op comes from
 * (hw1 >> 5) & 0xF; rnv is 0 for MOV/MVN (rn == 15 unread). */
static uint32_t t32_dp(a32_ctx_t *ctx, uint32_t next, unsigned op, bool s,
    unsigned rd, uint32_t rnv, uint32_t op2, bool cbit)
{
    uint32_t res = 0U;
    bool cout = cbit;
    bool ov = false;
    bool arith = true;
    switch (op) {
    case 0x0U: res = rnv & op2; arith = false; break;
    case 0x1U: res = rnv & ~op2; arith = false; break;
    case 0x2U: res = rnv | op2; arith = false; break;
    case 0x3U: res = rnv | ~op2; arith = false; break;
    case 0x4U: res = rnv ^ op2; arith = false; break;
    case 0x8U: res = i_add_carry(rnv, op2, 0U, &cout, &ov); break;
    case 0xAU: res = i_add_carry(rnv, op2, cbit ? 1U : 0U, &cout, &ov); break;
    case 0xBU: res = i_add_carry(rnv, ~op2, cbit ? 1U : 0U, &cout, &ov); break;
    case 0xDU: res = i_add_carry(rnv, ~op2, 1U, &cout, &ov); break;
    case 0xEU: res = i_add_carry(op2, ~rnv, 1U, &cout, &ov); break;
    default: return t32_undef(ctx, next);
    }
    if (s) {
        if (arith) {
            i_set_nzcv(&ctx->cpsr, res, cout, ov);
        } else {
            i_set_nz(&ctx->cpsr, res);
            if (cbit) {
                ctx->cpsr |= A32_CPSR_C;
            } else {
                ctx->cpsr &= ~A32_CPSR_C;
            }
        }
    }
    if (rd == 15U) {
        if (s) {
            ctx->regs[15] = next;
            return A32_EXIT_NONE;
        }
        i_set_pc_branch(ctx, res, false);
        return A32_EXIT_NONE;
    }
    ctx->regs[rd] = res;
    ctx->regs[15] = next;
    return A32_EXIT_NONE;
}

/* Register-controlled shift (LSL/LSR/ASR/ROR.W). amt is already masked to
 * 8 bits. Resolves N/Z/C, preserves V. */
static uint32_t t32_rshift(uint32_t v, unsigned st, unsigned amt, bool old_c,
    bool *cout)
{
    if (amt == 0U) {
        *cout = old_c;
        return v;
    }
    switch (st) {
    case 0U:
        if (amt >= 32U) {
            *cout = (amt == 32U) ? ((v & 1U) != 0U) : false;
            return 0U;
        }
        *cout = ((v >> (32U - amt)) & 1U) != 0U;
        return v << amt;
    case 1U:
        if (amt >= 32U) {
            *cout = (amt == 32U) ? (((v >> 31) & 1U) != 0U) : false;
            return 0U;
        }
        *cout = ((v >> (amt - 1U)) & 1U) != 0U;
        return v >> amt;
    case 2U: {
        if (amt >= 32U) {
            *cout = ((v >> 31) & 1U) != 0U;
            return ((v & 0x80000000U) != 0U) ? 0xFFFFFFFFU : 0U;
        }
        *cout = ((v >> (amt - 1U)) & 1U) != 0U;
        if ((v & 0x80000000U) != 0U) {
            return (v >> amt) | (0xFFFFFFFFU << (32U - amt));
        }
        return v >> amt;
    }
    default: {
        unsigned r = amt & 31U;
        if (r == 0U) {
            *cout = ((v >> 31) & 1U) != 0U;
        } else {
            *cout = ((v >> (r - 1U)) & 1U) != 0U;
        }
        return i_ror32(v, r);
    }
    }
}

/* Signed / unsigned saturating casts with sticky Q (NZCV preserved). */
static uint32_t t32_sat_s(a32_ctx_t *ctx, int64_t v, unsigned bits)
{
    int64_t mx = (bits >= 32U) ? 2147483647LL : ((1LL << (bits - 1U)) - 1LL);
    int64_t mn = (bits >= 32U) ? -2147483648LL : -(1LL << (bits - 1U));
    if (v > mx) {
        ctx->cpsr |= A32_CPSR_Q;
        return (uint32_t)(int32_t)mx;
    }
    if (v < mn) {
        ctx->cpsr |= A32_CPSR_Q;
        return (uint32_t)(int32_t)mn;
    }
    return (uint32_t)(int32_t)v;
}

static uint32_t t32_sat_u(a32_ctx_t *ctx, int64_t v, unsigned bits)
{
    int64_t mx = (bits >= 31U) ? 2147483647LL : ((1LL << bits) - 1LL);
    if (v < 0LL) {
        ctx->cpsr |= A32_CPSR_Q;
        return 0U;
    }
    if (v > mx) {
        ctx->cpsr |= A32_CPSR_Q;
        return (uint32_t)mx;
    }
    return (uint32_t)v;
}

static int64_t t32_sx_lane(uint32_t v, unsigned w)
{
    return (w == 16U) ? (int64_t)(int16_t)(uint16_t)v : (int64_t)(int8_t)(uint8_t)v;
}

static uint32_t t32_crc32(uint32_t crc, uint32_t data, unsigned nbits,
    uint32_t poly)
{
    for (unsigned i = 0U; i < nbits; i++) {
        uint32_t bit = (crc ^ (data >> i)) & 1U;
        crc = (crc >> 1) ^ (bit != 0U ? poly : 0U);
    }
    return crc;
}

static uint32_t t32_rbit(uint32_t v)
{
    v = ((v >> 1) & 0x55555555U) | ((v & 0x55555555U) << 1);
    v = ((v >> 2) & 0x33333333U) | ((v & 0x33333333U) << 2);
    v = ((v >> 4) & 0x0F0F0F0FU) | ((v & 0x0F0F0F0FU) << 4);
    v = ((v >> 8) & 0x00FF00FFU) | ((v & 0x00FF00FFU) << 8);
    return (v >> 16) | (v << 16);
}

/* Parallel add/sub (S/Q/SH/U/UQ/UH, 8- or 16-bit lanes). kind is op2,
 * sub_mask selects subtracting lanes. S/U update CPSR.GE per lane;
 * Q saturates with sticky Q; SH/UH halve. NZCV is always preserved. */
static uint32_t t32_padd(a32_ctx_t *ctx, uint32_t next, unsigned kind,
    bool is16, bool swap, unsigned sub, unsigned rd, uint32_t anv,
    uint32_t bnv)
{
    unsigned lanes = is16 ? 2U : 4U;
    unsigned w = is16 ? 16U : 8U;
    uint32_t mask = is16 ? 0xFFFFU : 0xFFU;
    uint32_t res = 0U;
    uint32_t ge = 0U;
    for (unsigned l = 0U; l < lanes; l++) {
        unsigned la = l * w;
        unsigned lb = (swap ? (1U - l) : l) * w;
        uint32_t av = (anv >> la) & mask;
        uint32_t bv = (bnv >> lb) & mask;
        bool is_sub = ((sub >> l) & 1U) != 0U;
        uint32_t t = 0U;
        if (kind == 0U) {
            int64_t s = is_sub ? t32_sx_lane(av, w) - t32_sx_lane(bv, w) :
                                 t32_sx_lane(av, w) + t32_sx_lane(bv, w);
            t = (uint32_t)s & mask;
            if (s >= 0LL) {
                ge |= is16 ? (3U << (16U + l * 2U)) : (1U << (16U + l));
            }
        } else if (kind == 4U) {
            if (is_sub) {
                t = (av - bv) & mask;
                if (av >= bv) {
                    ge |= is16 ? (3U << (16U + l * 2U)) : (1U << (16U + l));
                }
            } else {
                uint32_t f = av + bv;
                t = f & mask;
                if (f >= (is16 ? 0x10000U : 0x100U)) {
                    ge |= is16 ? (3U << (16U + l * 2U)) : (1U << (16U + l));
                }
            }
        } else if (kind == 1U) {
            int64_t s = is_sub ? t32_sx_lane(av, w) - t32_sx_lane(bv, w) :
                                 t32_sx_lane(av, w) + t32_sx_lane(bv, w);
            int64_t mx = (w == 16U) ? 32767LL : 127LL;
            int64_t mn = (w == 16U) ? -32768LL : -128LL;
            int64_t c = (s > mx) ? mx : ((s < mn) ? mn : s);
            t = (uint32_t)c & mask;
        } else if (kind == 5U) {
            if (is_sub) {
                t = (av >= bv) ? (av - bv) : 0U;
            } else {
                uint32_t f = av + bv;
                t = (f > mask) ? mask : f;
            }
        } else if (kind == 2U) {
            int64_t s = is_sub ? t32_sx_lane(av, w) - t32_sx_lane(bv, w) :
                                 t32_sx_lane(av, w) + t32_sx_lane(bv, w);
            int64_t h = s / 2LL;
            if (s < 0LL && (s % 2LL) != 0LL) {
                h -= 1LL;
            }
            t = (uint32_t)h & mask;
        } else {
            uint32_t f = is_sub ? (av - bv) : (av + bv);
            t = (f >> 1) & mask;
        }
        res |= t << la;
    }
    if (kind == 0U || kind == 4U) {
        ctx->cpsr = (ctx->cpsr & ~A32_CPSR_GE_MASK) | ge;
    }
    if (rd != 15U) {
        ctx->regs[rd] = res;
    }
    ctx->regs[15] = next;
    return A32_EXIT_NONE;
}

uint32_t i_t32_step_ext(a32_ctx_t *ctx, uint32_t pc, uint16_t hw1,
    uint16_t hw2)
{
    uint32_t next = pc + 4U;
    uint32_t pc_val = pc + 4U;
    uint32_t pc_align4 = pc_val & ~3U;

    /* VFP / NEON / CP15 delegation (matches interp_t32_dp.c ordering). */
    bool is_vfp = ((hw1 & 0xEF00U) == 0xEF00U) ||
        ((hw1 & 0xFF10U) == 0xF900U) ||
        ((((hw1 & 0xEE00U) == 0xEC00U) ||
            ((hw1 & 0xFF00U) == 0xEE00U) ||
            ((hw1 & 0xFE00U) == 0xFC00U) ||
            ((hw1 & 0xFF00U) == 0xFE00U)) &&
            (((hw2 >> 8) & 0xEU) == 0xAU)) ||
        ((hw1 & 0xFFEFU) == 0xEE0DU) || ((hw1 & 0xFFEFU) == 0xEE07U);
    if (is_vfp) {
        uint32_t word;
        if ((hw1 & 0xFF00U) == 0xEF00U) {
            word = 0xF2000000U | (((uint32_t)hw1 & 0x00FFU) << 16) | hw2;
        } else if ((hw1 & 0xFF00U) == 0xFF00U) {
            word = 0xF3000000U | (((uint32_t)hw1 & 0x00FFU) << 16) | hw2;
        } else if ((hw1 & 0xFF10U) == 0xF900U) {
            word = 0xF4000000U | (((uint32_t)hw1 & 0x00FFU) << 16) | hw2;
        } else {
            word = ((uint32_t)hw1 << 16) | hw2;
        }
        if (a32_interp_vfp_step(ctx, word)) {
            if (ctx->exit_reason != A32_EXIT_NONE) {
                return ctx->exit_reason;
            }
            if (ctx->regs[15] == pc || (ctx->regs[15] & ~1U) == pc) {
                ctx->regs[15] = next;
            }
            return A32_EXIT_NONE;
        }
        return t32_undef(ctx, next);
    }

    /* UDF.W */
    if (hw1 == 0xF7F0U && (hw2 & 0xF000U) == 0xA000U) {
        return t32_undef(ctx, next);
    }

    /* Branches and miscellaneous control. */
    if ((hw1 & 0xF800U) == 0xF000U && (hw2 & 0x8000U) != 0U) {
        unsigned h2o = (unsigned)hw2 & 0xD000U;
        if (h2o == 0xD000U || h2o == 0xC000U || h2o == 0x9000U) {
            bool is_blx = (h2o == 0xC000U);
            uint32_t s = (hw1 >> 10) & 1U;
            uint32_t j1 = (hw2 >> 13) & 1U;
            uint32_t j2 = (hw2 >> 11) & 1U;
            uint32_t i1 = ~(j1 ^ s) & 1U;
            uint32_t i2 = ~(j2 ^ s) & 1U;
            uint32_t uoff = (s << 24) | (i1 << 23) | (i2 << 22) |
                (((uint32_t)hw1 & 0x3FFU) << 12) | (((uint32_t)hw2 & 0x7FFU) << 1);
            uint32_t soff = (s != 0U) ? (uoff | 0xFE000000U) : uoff;
            if (is_blx && (hw2 & 1U) != 0U) {
                return t32_undef(ctx, next);
            }
            if ((hw2 & 0x4000U) != 0U) {
                ctx->regs[14] = (pc + 4U) | 1U;
            }
            if (is_blx) {
                i_set_pc_branch(ctx, (pc_align4 + soff) & ~3U, true);
            } else {
                i_set_pc_branch(ctx, pc_val + soff, false);
            }
            return A32_EXIT_NONE;
        }
        if (h2o == 0x8000U) {
            unsigned cond = ((unsigned)hw1 >> 6) & 0xFU;
            if (cond < 14U) {
                if (!a32_interp_cond_passes(ctx->cpsr, cond)) {
                    ctx->regs[15] = next;
                    return A32_EXIT_NONE;
                }
                uint32_t s = (hw1 >> 10) & 1U;
                uint32_t j1 = (hw2 >> 13) & 1U;
                uint32_t j2 = (hw2 >> 11) & 1U;
                uint32_t uoff = (s << 20) | (j2 << 19) | (j1 << 18) |
                    (((uint32_t)hw1 & 0x3FU) << 12) |
                    (((uint32_t)hw2 & 0x7FFU) << 1);
                uint32_t soff = (s != 0U) ? (uoff | 0xFFE00000U) : uoff;
                i_set_pc_branch(ctx, pc_val + soff, false);
                return A32_EXIT_NONE;
            }
            if (hw1 == 0xF3BFU) {
                unsigned op = (hw2 >> 4) & 0xFU;
                if (op == 2U) {
                    ctx->excl_valid = false;
                } else if (op == 4U || op == 5U || op == 6U) {
                    __atomic_thread_fence(__ATOMIC_SEQ_CST);
                } else {
                    return t32_undef(ctx, next);
                }
                ctx->regs[15] = next;
                return A32_EXIT_NONE;
            }
            if (hw1 == 0xF3AFU) {
                ctx->regs[15] = next;
                return A32_EXIT_NONE;
            }
            if (hw1 == 0xF3EFU && (hw2 & 0xF0FFU) == 0x8000U) {
                unsigned rd = (hw2 >> 8) & 0xFU;
                if (rd != 15U) {
                    ctx->regs[rd] = ctx->cpsr;
                }
                ctx->regs[15] = next;
                return A32_EXIT_NONE;
            }
            if ((hw1 & 0xFFF0U) == 0xF380U && (hw2 & 0xF0FFU) == 0x8000U &&
                ((hw2 >> 8) & 0xFU) != 0U) {
                unsigned mask = (hw2 >> 8) & 0xFU;
                unsigned rn = hw1 & 0xFU;
                uint32_t src = t32_reg(ctx, rn, pc_val);
                if ((mask & 8U) != 0U) {
                    ctx->cpsr = (ctx->cpsr &
                        ~(A32_CPSR_NZCV_MASK | A32_CPSR_Q)) |
                        (src & (A32_CPSR_NZCV_MASK | A32_CPSR_Q));
                }
                if ((mask & 4U) != 0U) {
                    ctx->cpsr = (ctx->cpsr & ~A32_CPSR_GE_MASK) |
                        (src & A32_CPSR_GE_MASK);
                }
                ctx->regs[15] = next;
                return A32_EXIT_NONE;
            }
        }
        return t32_undef(ctx, next);
    }

    if ((hw1 & 0xFA00U) == 0xF000U && (hw2 & 0x8000U) == 0U) {
        unsigned op = (hw1 >> 5) & 0xFU, rn = hw1 & 0xFU, rd = (hw2 >> 8) & 0xFU;
        bool s = ((hw1 >> 4) & 1U) != 0U, hc = false;
        unsigned ic = 0U;
        uint32_t imm12 = (((hw1 >> 10) & 1U) << 11) | (((hw2 >> 12) & 7U) << 8) | (hw2 & 0xFFU);
        uint32_t imm32 = t32_expand_imm(imm12, &hc, &ic);
        uint32_t rnv = (op == 0x2U || op == 0x3U) ? ((rn == 15U) ? 0U : ctx->regs[rn]) : t32_reg(ctx, rn, pc_val);
        bool cbit = (op <= 0x4U && s && hc) ? (ic != 0U) : ((ctx->cpsr & A32_CPSR_C) != 0U);
        return t32_dp(ctx, next, op, s, rd, rnv, imm32, cbit);
    }
    if ((hw1 & 0xFA00U) == 0xF200U && (hw2 & 0x8000U) == 0U) {
        unsigned op = (hw1 >> 4) & 0x1FU, rn = hw1 & 0xFU, rd = (hw2 >> 8) & 0xFU;
        uint32_t imm12 = (((hw1 >> 10) & 1U) << 11) | (((hw2 >> 12) & 7U) << 8) | (hw2 & 0xFFU);
        if (op == 0x00U) {
            ctx->regs[rd] = (rn == 15U) ? (pc_align4 + imm12) : (ctx->regs[rn] + imm12);
            ctx->regs[15] = next; return A32_EXIT_NONE;
        }
        if (op == 0x04U) {
            ctx->regs[rd] = ((rn & 0xFU) << 12) | imm12;
            ctx->regs[15] = next; return A32_EXIT_NONE;
        }
        if (op == 0x0AU) {
            ctx->regs[rd] = (rn == 15U) ? (pc_align4 - imm12) : (ctx->regs[rn] - imm12);
            ctx->regs[15] = next; return A32_EXIT_NONE;
        }
        if (op == 0x0CU) {
            uint32_t imm16 = ((rn & 0xFU) << 12) | imm12;
            ctx->regs[rd] = (ctx->regs[rd] & 0xFFFFU) | (imm16 << 16);
            ctx->regs[15] = next; return A32_EXIT_NONE;
        }
        if (op == 0x16U) {
            unsigned lsb = (((hw2 >> 12) & 7U) << 2) | ((hw2 >> 6) & 3U), msb = hw2 & 31U;
            if (msb < lsb) return t32_undef(ctx, next);
            uint32_t src = (rn == 15U) ? 0U : ctx->regs[rn];
            unsigned w = msb - lsb + 1U;
            uint32_t mask = (w == 32U) ? 0xFFFFFFFFU : (((1U << w) - 1U) << lsb);
            ctx->regs[rd] = (ctx->regs[rd] & ~mask) | ((src << lsb) & mask);
            ctx->regs[15] = next; return A32_EXIT_NONE;
        }
        if (op == 0x14U) {
            unsigned lsb = (((hw2 >> 12) & 7U) << 2) | ((hw2 >> 6) & 3U), wm1 = hw2 & 31U;
            if (lsb + wm1 > 31U) return t32_undef(ctx, next);
            uint32_t v = t32_reg(ctx, rn, pc_val);
            unsigned w = wm1 + 1U;
            ctx->regs[rd] = (w == 32U) ? v : (uint32_t)(((int32_t)(v << (32U - lsb - w))) >> (32U - w));
            ctx->regs[15] = next; return A32_EXIT_NONE;
        }
        if (op == 0x1CU) {
            unsigned lsb = (((hw2 >> 12) & 7U) << 2) | ((hw2 >> 6) & 3U), wm1 = hw2 & 31U;
            if (lsb + wm1 > 31U) return t32_undef(ctx, next);
            uint32_t v = t32_reg(ctx, rn, pc_val);
            unsigned w = wm1 + 1U;
            ctx->regs[rd] = (w == 32U) ? v : ((v >> lsb) & ((1U << w) - 1U));
            ctx->regs[15] = next; return A32_EXIT_NONE;
        }
        if (op == 0x10U || op == 0x12U) {
            bool is_asr = (op == 0x12U);
            unsigned shim = (((hw2 >> 12) & 7U) << 2) | ((hw2 >> 6) & 3U);
            uint32_t src = t32_reg(ctx, rn, pc_val);
            if (is_asr && shim == 0U) {
                unsigned sat = (hw2 & 15U) + 1U;
                uint32_t rl = t32_sat_s(ctx, (int16_t)(src & 0xFFFFU), sat) & 0xFFFFU;
                uint32_t rh = t32_sat_s(ctx, (int16_t)(src >> 16), sat) & 0xFFFFU;
                ctx->regs[rd] = rl | (rh << 16);
            } else {
                int32_t sh = is_asr ? ((int32_t)src >> shim) : (int32_t)(src << shim);
                ctx->regs[rd] = t32_sat_s(ctx, sh, (hw2 & 31U) + 1U);
            }
            ctx->regs[15] = next; return A32_EXIT_NONE;
        }
        if (op == 0x18U || op == 0x1AU) {
            bool is_asr = (op == 0x1AU);
            unsigned shim = (((hw2 >> 12) & 7U) << 2) | ((hw2 >> 6) & 3U);
            uint32_t src = t32_reg(ctx, rn, pc_val);
            if (is_asr && shim == 0U) {
                unsigned sat = hw2 & 15U;
                uint32_t rl = t32_sat_u(ctx, (int16_t)(src & 0xFFFFU), sat) & 0xFFFFU;
                uint32_t rh = t32_sat_u(ctx, (int16_t)(src >> 16), sat) & 0xFFFFU;
                ctx->regs[rd] = rl | (rh << 16);
            } else {
                int32_t sh = is_asr ? ((int32_t)src >> shim) : (int32_t)(src << shim);
                ctx->regs[rd] = t32_sat_u(ctx, sh, hw2 & 31U);
            }
            ctx->regs[15] = next; return A32_EXIT_NONE;
        }
        return t32_undef(ctx, next);
    }
    if ((hw1 & 0xFE00U) == 0xEA00U) {
        if ((hw1 & 0xFFF0U) == 0xEAC0U && (hw2 & 0x8010U) == 0U) {
            unsigned rn = hw1 & 0xFU, rd = (hw2 >> 8) & 0xFU, rm = hw2 & 0xFU;
            unsigned shim = (((hw2 >> 12) & 7U) << 2) | ((hw2 >> 6) & 3U);
            bool is_tb = ((hw2 >> 5) & 1U) != 0U;
            uint32_t rnv = t32_reg(ctx, rn, pc_val), rmv = t32_reg(ctx, rm, pc_val), res;
            if (!is_tb) res = ((rmv << shim) & 0xFFFF0000U) | (rnv & 0xFFFFU);
            else {
                unsigned amt = (shim == 0U) ? 31U : shim;
                res = (rnv & 0xFFFF0000U) | (((uint32_t)((int32_t)rmv >> amt)) & 0xFFFFU);
            }
            ctx->regs[rd] = res; ctx->regs[15] = next; return A32_EXIT_NONE;
        }
        unsigned op = (hw1 >> 5) & 0xFU, rn = hw1 & 0xFU, rd = (hw2 >> 8) & 0xFU, rm = hw2 & 0xFU;
        bool s = ((hw1 >> 4) & 1U) != 0U, shc = false;
        unsigned st = (hw2 >> 4) & 3U, imm5 = (((hw2 >> 12) & 7U) << 2) | ((hw2 >> 6) & 3U);
        uint32_t shv = t32_imm_shift(t32_reg(ctx, rm, pc_val), st, imm5, ctx->cpsr, &shc);
        uint32_t rnv = (op == 0x2U || op == 0x3U) ? ((rn == 15U) ? 0U : ctx->regs[rn]) : t32_reg(ctx, rn, pc_val);
        bool cbit = (op <= 0x4U && s) ? shc : ((ctx->cpsr & A32_CPSR_C) != 0U);
        return t32_dp(ctx, next, op, s, rd, rnv, shv, cbit);
    }
    if ((hw1 & 0xFE00U) == 0xFA00U) {
        unsigned rn = hw1 & 0xFU, rd = (hw2 >> 8) & 0xFU, rm = hw2 & 0xFU;
        if ((hw1 & 0xFF80U) == 0xFA00U && (hw2 & 0xF0F0U) == 0xF000U) {
            unsigned st = (hw1 >> 5) & 3U;
            bool s = ((hw1 >> 4) & 1U) != 0U, oldc = (ctx->cpsr & A32_CPSR_C) != 0U, cout = oldc;
            uint32_t res = t32_rshift(t32_reg(ctx, rn, pc_val), st, t32_reg(ctx, rm, pc_val) & 0xFFU, oldc, &cout);
            if (s) {
                i_set_nz(&ctx->cpsr, res);
                if (cout) ctx->cpsr |= A32_CPSR_C; else ctx->cpsr &= ~A32_CPSR_C;
            }
            ctx->regs[rd] = res; ctx->regs[15] = next; return A32_EXIT_NONE;
        }
        if ((hw1 & 0xFF80U) == 0xFA00U && (hw2 & 0xF0C0U) == 0xF080U) {
            unsigned op = (hw1 >> 4) & 7U;
            if (op > 5U) return t32_undef(ctx, next);
            unsigned rot = ((hw2 >> 4) & 3U) * 8U;
            uint32_t rmv = i_ror32(t32_reg(ctx, rm, pc_val), rot);
            bool is_signed = (op & 1U) == 0U;
            if (op == 2U || op == 3U) {
                int32_t loe = is_signed ? (int32_t)(int8_t)(rmv & 0xFFU) : (int32_t)(rmv & 0xFFU);
                int32_t hie = is_signed ? (int32_t)(int8_t)((rmv >> 16) & 0xFFU) : (int32_t)((rmv >> 16) & 0xFFU);
                uint32_t rnv = (rn == 15U) ? 0U : ctx->regs[rn];
                uint32_t lo = ((uint32_t)loe + (rn == 15U ? 0U : (rnv & 0xFFFFU))) & 0xFFFFU;
                uint32_t hi = ((uint32_t)hie + (rn == 15U ? 0U : ((rnv >> 16) & 0xFFFFU))) & 0xFFFFU;
                ctx->regs[rd] = lo | (hi << 16);
            } else {
                uint32_t ext;
                if (op < 2U) ext = is_signed ? (uint32_t)(int32_t)(int16_t)(rmv & 0xFFFFU) : (rmv & 0xFFFFU);
                else ext = is_signed ? (uint32_t)(int32_t)(int8_t)(rmv & 0xFFU) : (rmv & 0xFFU);
                if (rn != 15U) ext += ctx->regs[rn];
                ctx->regs[rd] = ext;
            }
            ctx->regs[15] = next; return A32_EXIT_NONE;
        }
        if ((hw1 & 0xFFF0U) == 0xFA80U && (hw2 & 0xF0C0U) == 0xF080U) {
            unsigned subop = (hw2 >> 4) & 3U;
            bool is_sub = (subop & 2U) != 0U, is_dbl = (subop & 1U) != 0U;
            int64_t b = (int32_t)t32_reg(ctx, rn, pc_val);
            int32_t mv = (int32_t)t32_reg(ctx, rm, pc_val);
            if (is_dbl) {
                b *= 2LL;
                if (b > 2147483647LL) { b = 2147483647LL; ctx->cpsr |= A32_CPSR_Q; }
                else if (b < -2147483648LL) { b = -2147483648LL; ctx->cpsr |= A32_CPSR_Q; }
            }
            int64_t r = is_sub ? ((int64_t)mv - b) : ((int64_t)mv + b);
            if (r > 2147483647LL) { r = 2147483647LL; ctx->cpsr |= A32_CPSR_Q; }
            else if (r < -2147483648LL) { r = -2147483648LL; ctx->cpsr |= A32_CPSR_Q; }
            ctx->regs[rd] = (uint32_t)(int32_t)r; ctx->regs[15] = next; return A32_EXIT_NONE;
        }
        if ((hw1 & 0xFFF0U) == 0xFAB0U && (hw2 & 0xF0C0U) == 0xF080U) {
            uint32_t v = t32_reg(ctx, rm, pc_val);
            ctx->regs[rd] = (v == 0U) ? 32U : (uint32_t)__builtin_clz(v);
            ctx->regs[15] = next; return A32_EXIT_NONE;
        }
        if ((hw1 & 0xFFE0U) == 0xFAC0U && (hw2 & 0xF0C0U) == 0xF080U) {
            unsigned cbit = (hw1 >> 4) & 1U, sz = (hw2 >> 4) & 3U;
            if (rd == 13U || rd == 15U || rn == 13U || rn == 15U || rm == 13U || rm == 15U || sz == 3U) return t32_undef(ctx, next);
            uint32_t poly = (cbit != 0U) ? 0x82F63B78U : 0xEDB88320U;
            ctx->regs[rd] = t32_crc32(ctx->regs[rn], ctx->regs[rm], 8U << sz, poly);
            ctx->regs[15] = next; return A32_EXIT_NONE;
        }
        if ((hw1 & 0xFFF0U) == 0xFA90U && (hw2 & 0xF0C0U) == 0xF080U) {
            unsigned subop = (hw2 >> 4) & 3U;
            uint32_t v = t32_reg(ctx, rm, pc_val), r;
            if (subop == 0U) r = ((v & 0xFFU) << 24) | ((v & 0xFF00U) << 8) | ((v >> 8) & 0xFF00U) | ((v >> 24) & 0xFFU);
            else if (subop == 1U) r = ((v & 0xFFU) << 8) | ((v >> 8) & 0xFFU) | ((v & 0xFF0000U) << 8) | ((v >> 8) & 0xFF0000U);
            else if (subop == 2U) r = t32_rbit(v);
            else r = (uint32_t)(int32_t)(int16_t)(uint16_t)(((v & 0xFFU) << 8) | ((v >> 8) & 0xFFU));
            ctx->regs[rd] = r; ctx->regs[15] = next; return A32_EXIT_NONE;
        }
        if ((hw1 & 0xFFF0U) == 0xFAA0U && (hw2 & 0xF0F0U) == 0xF080U) {
            uint32_t ge = (ctx->cpsr >> 16) & 0xFU, anv = t32_reg(ctx, rn, pc_val), bnv = t32_reg(ctx, rm, pc_val), res = 0U;
            for (unsigned i = 0U; i < 4U; i++) res |= ((((ge >> i) & 1U) != 0U ? ((anv >> (i * 8U)) & 0xFFU) : ((bnv >> (i * 8U)) & 0xFFU)) << (i * 8U));
            ctx->regs[rd] = res; ctx->regs[15] = next; return A32_EXIT_NONE;
        }
        if (((hw1 & 0xFFF0U) == 0xFA80U || (hw1 & 0xFFF0U) == 0xFAC0U || (hw1 & 0xFFF0U) == 0xFA90U || (hw1 & 0xFFF0U) == 0xFAD0U || (hw1 & 0xFFF0U) == 0xFAA0U || (hw1 & 0xFFF0U) == 0xFAE0U) && (hw2 & 0xF080U) == 0xF000U && ((hw2 >> 4) & 7U) != 3U && ((hw2 >> 4) & 7U) != 7U) {
            unsigned op1 = (hw1 >> 4) & 7U, op2 = (hw2 >> 4) & 7U, sub = 0U;
            bool is16 = op1 != 0U && op1 != 4U, swap = is16 && (op1 == 2U || op1 == 6U);
            if (!is16) sub = ((hw1 & 0x40U) != 0U) ? 0xFU : 0U;
            else if (op1 == 5U) sub = 0x3U; else if (op1 == 2U) sub = 0x1U; else if (op1 == 6U) sub = 0x2U;
            return t32_padd(ctx, next, op2, is16, swap, sub, rd, t32_reg(ctx, rn, pc_val), t32_reg(ctx, rm, pc_val));
        }
        if ((hw1 & 0xFFF0U) == 0xFB00U) {
            unsigned ra = (hw2 >> 12) & 0xFU, subop = (hw2 >> 4) & 0xFU;
            uint32_t rnv = t32_reg(ctx, rn, pc_val), rmv = t32_reg(ctx, rm, pc_val);
            if (subop == 0U) ctx->regs[rd] = rnv * rmv + ((ra == 15U) ? 0U : ctx->regs[ra]);
            else if (subop == 1U) ctx->regs[rd] = t32_reg(ctx, ra, pc_val) - rnv * rmv;
            else return t32_undef(ctx, next);
            ctx->regs[15] = next; return A32_EXIT_NONE;
        }
        if ((hw1 & 0xFFF0U) == 0xFB10U && (hw2 & 0xC0U) == 0U) {
            unsigned ra = (hw2 >> 12) & 0xFU;
            bool ntop = ((hw2 >> 5) & 1U) != 0U, mtop = ((hw2 >> 4) & 1U) != 0U;
            uint32_t rnv = t32_reg(ctx, rn, pc_val), rmv = t32_reg(ctx, rm, pc_val);
            int32_t prod = (ntop ? (int16_t)(rnv >> 16) : (int16_t)(rnv & 0xFFFFU)) * (mtop ? (int16_t)(rmv >> 16) : (int16_t)(rmv & 0xFFFFU));
            if (ra == 15U) ctx->regs[rd] = (uint32_t)prod;
            else {
                int64_t sum = (int64_t)prod + (int64_t)(int32_t)ctx->regs[ra];
                if (sum > 2147483647LL || sum < -2147483648LL) ctx->cpsr |= A32_CPSR_Q;
                ctx->regs[rd] = (uint32_t)(int32_t)sum;
            }
            ctx->regs[15] = next; return A32_EXIT_NONE;
        }
        if (((hw1 & 0xFFF0U) == 0xFB20U || (hw1 & 0xFFF0U) == 0xFB40U) && (hw2 & 0xE0U) == 0U) {
            unsigned ra = (hw2 >> 12) & 0xFU;
            bool is_sub = (hw1 & 0xFFF0U) == 0xFB40U;
            bool mswap = ((hw2 >> 4) & 1U) != 0U;
            uint32_t rnv = t32_reg(ctx, rn, pc_val);
            uint32_t rmv = t32_reg(ctx, rm, pc_val);
            int32_t rnlo = (int16_t)(rnv & 0xFFFFU);
            int32_t rnhi = (int16_t)(rnv >> 16);
            int32_t m1 = mswap ? (int16_t)(rmv >> 16) : (int16_t)(rmv & 0xFFFFU);
            int32_t m2 = mswap ? (int16_t)(rmv & 0xFFFFU) : (int16_t)(rmv >> 16);
            int64_t p1 = (int64_t)rnlo * (int64_t)m1;
            int64_t p2 = (int64_t)rnhi * (int64_t)m2;
            int64_t acc = (ra == 15U) ? 0LL : (int64_t)(int32_t)ctx->regs[ra];
            int64_t sum = acc + p1 + (is_sub ? -p2 : p2);
            if ((!is_sub || ra != 15U) && (sum > 2147483647LL || sum < -2147483648LL)) {
                ctx->cpsr |= A32_CPSR_Q;
            }
            ctx->regs[rd] = (uint32_t)(int32_t)sum;
            ctx->regs[15] = next;
            return A32_EXIT_NONE;
        }
        if ((hw1 & 0xFFF0U) == 0xFB30U && (hw2 & 0xE0U) == 0U) {
            unsigned ra = (hw2 >> 12) & 0xFU;
            bool mtop = ((hw2 >> 4) & 1U) != 0U;
            uint32_t rnv = t32_reg(ctx, rn, pc_val);
            uint32_t rmv = t32_reg(ctx, rm, pc_val);
            int32_t half = mtop ? (int16_t)(rmv >> 16) : (int16_t)(rmv & 0xFFFFU);
            int64_t prod = (int64_t)(int32_t)rnv * (int64_t)half;
            int32_t hi = (int32_t)(prod >> 16);
            if (ra == 15U) {
                ctx->regs[rd] = (uint32_t)hi;
            } else {
                int64_t sum = (int64_t)hi + (int64_t)(int32_t)ctx->regs[ra];
                if (sum > 2147483647LL || sum < -2147483648LL) {
                    ctx->cpsr |= A32_CPSR_Q;
                }
                ctx->regs[rd] = (uint32_t)(int32_t)sum;
            }
            ctx->regs[15] = next;
            return A32_EXIT_NONE;
        }
        if (((hw1 & 0xFFF0U) == 0xFB50U || (hw1 & 0xFFF0U) == 0xFB60U) && (hw2 & 0xE0U) == 0U) {
            unsigned ra = (hw2 >> 12) & 0xFU;
            bool is_sub = (hw1 & 0xFFF0U) == 0xFB60U;
            bool round = ((hw2 >> 4) & 1U) != 0U;
            uint32_t rnv = t32_reg(ctx, rn, pc_val);
            uint32_t rmv = t32_reg(ctx, rm, pc_val);
            int64_t prod = (int64_t)(int32_t)rnv * (int64_t)(int32_t)rmv;
            uint32_t rc = round ? 0x80000000U : 0U;
            uint64_t uacc;
            if (!is_sub && ra == 15U) {
                uacc = (uint64_t)rc;
            } else {
                uint32_t rav = (ra == 15U) ? 0U : ctx->regs[ra];
                uacc = ((uint64_t)rav << 32) | (uint64_t)rc;
            }
            uint64_t utmp = is_sub ? (uacc - (uint64_t)prod) : (uacc + (uint64_t)prod);
            ctx->regs[rd] = (uint32_t)(utmp >> 32);
            ctx->regs[15] = next;
            return A32_EXIT_NONE;
        }
        if ((hw1 & 0xFFF0U) == 0xFB70U && (hw2 & 0xF0U) == 0U) {
            unsigned ra = (hw2 >> 12) & 0xFU;
            uint32_t rnv = t32_reg(ctx, rn, pc_val);
            uint32_t rmv = t32_reg(ctx, rm, pc_val);
            uint32_t sum = (ra == 15U) ? 0U : ctx->regs[ra];
            for (unsigned i = 0U; i < 4U; i++) {
                uint32_t a = (rnv >> (i * 8U)) & 0xFFU;
                uint32_t b = (rmv >> (i * 8U)) & 0xFFU;
                sum += (a >= b) ? (a - b) : (b - a);
            }
            ctx->regs[rd] = sum;
            ctx->regs[15] = next;
            return A32_EXIT_NONE;
        }
        if ((hw1 & 0xFF80U) == 0xFB80U) {
            unsigned op = (hw1 >> 4) & 7U;
            unsigned rd_lo = (hw2 >> 12) & 0xFU;
            unsigned rd_hi = rd;
            unsigned subop = (hw2 >> 4) & 0xFU;
            uint32_t rnv = t32_reg(ctx, rn, pc_val);
            uint32_t rmv = t32_reg(ctx, rm, pc_val);
            if (op == 1U && subop == 0xFU) {
                int32_t num = (int32_t)rnv;
                int32_t den = (int32_t)rmv;
                int32_t q = 0;
                if (den != 0) {
                    q = (num == (-2147483647 - 1) && den == -1) ? num : (num / den);
                }
                ctx->regs[rd] = (uint32_t)q;
                ctx->regs[15] = next;
                return A32_EXIT_NONE;
            }
            if (op == 3U && subop == 0xFU) {
                ctx->regs[rd] = (rmv == 0U) ? 0U : (rnv / rmv);
                ctx->regs[15] = next;
                return A32_EXIT_NONE;
            }
            if (op == 6U && subop == 6U) {
                uint32_t lo0 = ctx->regs[rd_lo];
                uint32_t hi0 = ctx->regs[rd_hi];
                uint64_t p = (uint64_t)rnv * (uint64_t)rmv + (uint64_t)lo0 + (uint64_t)hi0;
                ctx->regs[rd_lo] = (uint32_t)p;
                ctx->regs[rd_hi] = (uint32_t)(p >> 32);
                ctx->regs[15] = next;
                return A32_EXIT_NONE;
            }
            if (op == 4U && (subop & 0xCU) == 8U) {
                bool ntop = ((subop >> 1) & 1U) != 0U;
                bool mtop = (subop & 1U) != 0U;
                int32_t lhs = ntop ? (int16_t)(rnv >> 16) : (int16_t)(rnv & 0xFFFFU);
                int32_t rhs = mtop ? (int16_t)(rmv >> 16) : (int16_t)(rmv & 0xFFFFU);
                int64_t prod = (int64_t)lhs * (int64_t)rhs;
                uint64_t acc = ((uint64_t)ctx->regs[rd_hi] << 32) | (uint64_t)ctx->regs[rd_lo];
                uint64_t full = acc + (uint64_t)prod;
                ctx->regs[rd_lo] = (uint32_t)full;
                ctx->regs[rd_hi] = (uint32_t)(full >> 32);
                ctx->regs[15] = next;
                return A32_EXIT_NONE;
            }
            if ((op == 4U || op == 5U) && (subop & 0xEU) == 0xCU) {
                bool is_sub = op == 5U;
                bool mswap = (subop & 1U) != 0U;
                int32_t rnlo = (int16_t)(rnv & 0xFFFFU);
                int32_t rnhi = (int16_t)(rnv >> 16);
                int32_t m1 = mswap ? (int16_t)(rmv >> 16) : (int16_t)(rmv & 0xFFFFU);
                int32_t m2 = mswap ? (int16_t)(rmv & 0xFFFFU) : (int16_t)(rmv >> 16);
                int64_t p1 = (int64_t)rnlo * (int64_t)m1;
                int64_t p2 = (int64_t)rnhi * (int64_t)m2;
                uint64_t acc = ((uint64_t)ctx->regs[rd_hi] << 32) | (uint64_t)ctx->regs[rd_lo];
                uint64_t full = acc + (uint64_t)p1 + (is_sub ? (uint64_t)(-p2) : (uint64_t)p2);
                ctx->regs[rd_lo] = (uint32_t)full;
                ctx->regs[rd_hi] = (uint32_t)(full >> 32);
                ctx->regs[15] = next;
                return A32_EXIT_NONE;
            }
            if (subop != 0U || (op != 0U && op != 2U && op != 4U && op != 6U)) {
                return t32_undef(ctx, next);
            }
            {
                bool is_signed = op == 0U || op == 4U;
                bool accum = op >= 4U;
                uint64_t acc = 0ULL;
                if (accum) {
                    acc = ((uint64_t)ctx->regs[rd_hi] << 32) | (uint64_t)ctx->regs[rd_lo];
                }
                uint64_t prod = is_signed ?
                    (uint64_t)(int64_t)((int64_t)(int32_t)rnv * (int64_t)(int32_t)rmv) :
                    (uint64_t)rnv * (uint64_t)rmv;
                uint64_t full = acc + prod;
                ctx->regs[rd_lo] = (uint32_t)full;
                ctx->regs[rd_hi] = (uint32_t)(full >> 32);
                ctx->regs[15] = next;
                return A32_EXIT_NONE;
            }
        }
        return t32_undef(ctx, next);
    }

    return i_t32_mem_step(ctx, pc, hw1, hw2);
}

