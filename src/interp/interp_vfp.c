/*
 * src/interp/interp_vfp.c — VFPv4-D32 / NEON reference subset for the interpreter
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Handles the coprocessor 10/11 words the integer core delegates:
 * single/double moves, arithmetic, sqrt, compare-to-APSR, FPSCR access,
 * conversions and VLDR/VSTR. Anything else returns false so the caller
 * reports A32_EXIT_UNDEF (single-step fallback).
 */

#include "interp.h"
#include "interp_priv.h"

#include <fenv.h>
#include <math.h>
#include <string.h>
#include <sys/mman.h>

#define FPSCR_N (1U << 31)
#define FPSCR_Z (1U << 30)
#define FPSCR_C (1U << 29)
#define FPSCR_V (1U << 28)
#define FPSCR_IDC (1U << 7)
#define FPSCR_IOC (1U << 0)
#define FPSCR_FZ (1U << 24)
#define FPSCR_DN (1U << 25)

/* Narrow arithmetic to separately-rounded operations: the host compiler
 * must not contract a * b + d into a fused multiply-add, otherwise VMLA
 * (non-fused) and VFMA (fused) become indistinguishable (A32-R7-29). */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
#pragma STDC FP_CONTRACT OFF
#endif

static bool vfp_fz(const a32_ctx_t *ctx)
{
    return (ctx->fpscr & FPSCR_FZ) != 0U;
}

static bool vfp_dn(const a32_ctx_t *ctx)
{
    return (ctx->fpscr & FPSCR_DN) != 0U;
}

/* Flush-to-zero with IDC accumulation on subnormal inputs (A32-R7-25). */
static float vfp_fz_in_f(a32_ctx_t *ctx, float f)
{
    if (vfp_fz(ctx) && fpclassify(f) == FP_SUBNORMAL) {
        ctx->fpscr |= FPSCR_IDC;
        return copysignf(0.0f, f);
    }
    return f;
}

static double vfp_fz_in_d(a32_ctx_t *ctx, double v)
{
    if (vfp_fz(ctx) && fpclassify(v) == FP_SUBNORMAL) {
        ctx->fpscr |= FPSCR_IDC;
        return copysign(0.0, v);
    }
    return v;
}

static float vfp_fz_out_f(a32_ctx_t *ctx, float f)
{
    if (vfp_fz(ctx) && fpclassify(f) == FP_SUBNORMAL) {
        return copysignf(0.0f, f);
    }
    return f;
}

static double vfp_fz_out_d(a32_ctx_t *ctx, double v)
{
    if (vfp_fz(ctx) && fpclassify(v) == FP_SUBNORMAL) {
        return copysign(0.0, v);
    }
    return v;
}

/* Default-NaN substitution for produced NaN results (A32-R7-25). */
static float vfp_dn_f(const a32_ctx_t *ctx, float f)
{
    if (vfp_dn(ctx) && f != f) {
        uint32_t w = 0x7FC00000U;
        memcpy(&f, &w, 4U);
    }
    return f;
}

static double vfp_dn_d(const a32_ctx_t *ctx, double v)
{
    if (vfp_dn(ctx) && v != v) {
        uint64_t w = 0x7FF8000000000000ULL;
        memcpy(&v, &w, 8U);
    }
    return v;
}

/* Signaling-NaN classification from payload bits (A32-R7-30): a NaN is
 * signaling when its mantissa top bit is clear. */
static bool vfp_snan_f(float f)
{
    uint32_t w = 0U;
    memcpy(&w, &f, 4U);
    return ((w & 0x7F800000U) == 0x7F800000U) && ((w & 0x007FFFFFU) != 0U) &&
        ((w & 0x00400000U) == 0U);
}

static bool vfp_snan_d(double v)
{
    uint64_t w = 0ULL;
    memcpy(&w, &v, 8U);
    return ((w & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL) &&
        ((w & 0x000FFFFFFFFFFFFFULL) != 0ULL) &&
        ((w & 0x0008000000000000ULL) == 0ULL);
}

static void vfp_fenv_begin(const a32_ctx_t *ctx, fenv_t *saved_env)
{
    (void)fegetenv(saved_env);
    (void)feclearexcept(FE_ALL_EXCEPT);
    switch ((ctx->fpscr >> 22) & 3U) {
    case 1U: (void)fesetround(FE_UPWARD); break;
    case 2U: (void)fesetround(FE_DOWNWARD); break;
    case 3U: (void)fesetround(FE_TOWARDZERO); break;
    default: (void)fesetround(FE_TONEAREST); break;
    }
}

static void vfp_fenv_end(a32_ctx_t *ctx, const fenv_t *saved_env)
{
    int ex = fetestexcept(FE_ALL_EXCEPT);
    if ((ex & FE_INVALID) != 0) {
        ctx->fpscr |= (1U << 0);
    }
    if ((ex & FE_DIVBYZERO) != 0) {
        ctx->fpscr |= (1U << 1);
    }
    if ((ex & FE_OVERFLOW) != 0) {
        ctx->fpscr |= (1U << 2);
    }
    if ((ex & FE_UNDERFLOW) != 0) {
        ctx->fpscr |= (1U << 3);
    }
    if ((ex & FE_INEXACT) != 0) {
        ctx->fpscr |= (1U << 4);
    }
    (void)fesetenv(saved_env);
}

static float get_s(const a32_ctx_t *ctx, unsigned s)
{
    uint32_t w = (uint32_t)(ctx->vfp_d[s >> 1] >> ((s & 1U) * 32U));
    float f = 0.0f;
    memcpy(&f, &w, 4U);
    return f;
}

static void put_s(a32_ctx_t *ctx, unsigned s, float f)
{
    uint32_t w = 0U;
    memcpy(&w, &f, 4U);
    uint64_t d = ctx->vfp_d[s >> 1];
    if ((s & 1U) == 0U) {
        d = (d & 0xFFFFFFFF00000000ULL) | w;
    } else {
        d = (d & 0x00000000FFFFFFFFULL) | ((uint64_t)w << 32);
    }
    ctx->vfp_d[s >> 1] = d;
}

static double get_d(const a32_ctx_t *ctx, unsigned d)
{
    double v = 0.0;
    memcpy(&v, &ctx->vfp_d[d], 8U);
    return v;
}

static void put_d(a32_ctx_t *ctx, unsigned d, double v)
{
    memcpy(&ctx->vfp_d[d], &v, 8U);
}

static void fpscr_to_cpsr(a32_ctx_t *ctx)
{
    uint32_t f = ctx->fpscr;
    if ((f & FPSCR_N) != 0U) {
        ctx->cpsr |= A32_CPSR_N;
    } else {
        ctx->cpsr &= ~A32_CPSR_N;
    }
    if ((f & FPSCR_Z) != 0U) {
        ctx->cpsr |= A32_CPSR_Z;
    } else {
        ctx->cpsr &= ~A32_CPSR_Z;
    }
    if ((f & FPSCR_C) != 0U) {
        ctx->cpsr |= A32_CPSR_C;
    } else {
        ctx->cpsr &= ~A32_CPSR_C;
    }
    if ((f & FPSCR_V) != 0U) {
        ctx->cpsr |= A32_CPSR_V;
    } else {
        ctx->cpsr &= ~A32_CPSR_V;
    }
}

static void cmp_set_fpscr(a32_ctx_t *ctx, bool n, bool z, bool c, bool v)
{
    ctx->fpscr &= ~(FPSCR_N | FPSCR_Z | FPSCR_C | FPSCR_V);
    if (n) {
        ctx->fpscr |= FPSCR_N;
    }
    if (z) {
        ctx->fpscr |= FPSCR_Z;
    }
    if (c) {
        ctx->fpscr |= FPSCR_C;
    }
    if (v) {
        ctx->fpscr |= FPSCR_V;
    }
}

static inline unsigned vfp_idx(bool is_double, bool hi_bit, unsigned lo4)
{
    return is_double ? ((hi_bit ? 16U : 0U) | (lo4 & 0xFU))
                     : (((lo4 & 0xFU) << 1) | (hi_bit ? 1U : 0U));
}

static float expand_imm_f32(uint32_t imm8)
{
    uint32_t a = (imm8 >> 7) & 1U;
    uint32_t b = (imm8 >> 6) & 1U;
    uint32_t c = (imm8 >> 5) & 1U;
    uint32_t d = (imm8 >> 4) & 1U;
    uint32_t efgh = imm8 & 0xFU;
    uint32_t exp = ((1U - b) << 7) | (b ? 0x7CU : 0U) | (c << 1) | d;
    uint32_t w = (a << 31) | (exp << 23) | (efgh << 19);
    float f = 0.0f;
    memcpy(&f, &w, 4U);
    return f;
}

static double expand_imm_f64(uint32_t imm8)
{
    uint64_t a = (imm8 >> 7) & 1U;
    uint64_t b = (imm8 >> 6) & 1U;
    uint64_t c = (imm8 >> 5) & 1U;
    uint64_t d = (imm8 >> 4) & 1U;
    uint64_t efgh = imm8 & 0xFU;
    uint64_t exp = ((1ULL - b) << 10) | (b ? (0xFFULL << 2) : 0ULL) | (c << 1) | d;
    uint64_t w = (a << 63) | (exp << 52) | (efgh << 48);
    double v = 0.0;
    memcpy(&v, &w, 8U);
    return v;
}

/* MCR/MRC between ARM core and VFP system regs. Scalar byte/halfword
 * lane moves decode element width, lane index and signedness from opc1
 * and opc2 (verified against assembler output): U8 opc1 6/7, U16 opc1
 * 4/5, S8 opc1 2/3, S16 opc1 0/1 with opc2 != 0. 8-bit lanes use
 * ((opc1 & 1) << 2) | (opc2 & 3); 16-bit lanes use
 * ((opc1 & 1) << 1) | ((opc2 >> 1) & 1) (A32-R7-27). */
static bool vfp_mcr_mrc(a32_ctx_t *ctx, uint32_t pc, uint32_t insn)
{
    bool load = ((insn >> 20) & 1U) != 0U;
    bool is_double = (((insn >> 8) & 0xFU) == 11U);
    unsigned opc1 = (insn >> 21) & 7U;
    unsigned crn = (insn >> 16) & 0xFU;
    unsigned rd = (insn >> 12) & 0xFU;
    unsigned opc2 = (insn >> 5) & 7U;

    if (load) {
        if (opc1 == 7U && !is_double && crn == 1U) {
            if (rd == 15U) {
                fpscr_to_cpsr(ctx);
            } else {
                ctx->regs[rd] = ctx->fpscr;
            }
            ctx->regs[15] = pc + 4U;
            return true;
        }
        if (opc1 == 0U && !is_double) {
            unsigned s = (crn << 1) | ((insn >> 7) & 1U);
            uint32_t w = 0U;
            float f = get_s(ctx, s);
            memcpy(&w, &f, 4U);
            if (rd != 15U) {
                ctx->regs[rd] = w;
            }
            ctx->regs[15] = pc + 4U;
            return true;
        }
        if (is_double && (opc1 == 6U || opc1 == 7U)) {
            unsigned dd = crn | (((insn >> 7) & 1U) << 4);
            unsigned lane = ((opc1 & 1U) << 2) | (opc2 & 3U);
            uint64_t v = ctx->vfp_d[dd & 31U];
            if (rd != 15U) {
                ctx->regs[rd] = (uint32_t)((v >> (lane * 8U)) & 0xFFU);
            }
            ctx->regs[15] = pc + 4U;
            return true;
        }
        if (is_double && (opc1 == 4U || opc1 == 5U)) {
            unsigned dd = crn | (((insn >> 7) & 1U) << 4);
            unsigned lane = ((opc1 & 1U) << 1) | ((opc2 >> 1) & 1U);
            uint64_t v = ctx->vfp_d[dd & 31U];
            if (rd != 15U) {
                ctx->regs[rd] = (uint32_t)((v >> (lane * 16U)) & 0xFFFFU);
            }
            ctx->regs[15] = pc + 4U;
            return true;
        }
        if (is_double && (opc1 == 2U || opc1 == 3U)) {
            unsigned dd = crn | (((insn >> 7) & 1U) << 4);
            unsigned lane = ((opc1 & 1U) << 2) | (opc2 & 3U);
            uint64_t v = ctx->vfp_d[dd & 31U];
            if (rd != 15U) {
                ctx->regs[rd] = (uint32_t)(int32_t)(int8_t)((v >> (lane * 8U)) & 0xFFU);
            }
            ctx->regs[15] = pc + 4U;
            return true;
        }
        if (is_double && (opc1 == 0U || opc1 == 1U) && opc2 != 0U) {
            unsigned dd = crn | (((insn >> 7) & 1U) << 4);
            unsigned lane = ((opc1 & 1U) << 1) | ((opc2 >> 1) & 1U);
            uint64_t v = ctx->vfp_d[dd & 31U];
            if (rd != 15U) {
                ctx->regs[rd] = (uint32_t)(int32_t)(int16_t)((v >> (lane * 16U)) & 0xFFFFU);
            }
            ctx->regs[15] = pc + 4U;
            return true;
        }
        if (is_double && ((opc1 & 2U) == 0U)) {
            unsigned dd = crn | (((insn >> 7) & 1U) << 4);
            uint64_t v = ctx->vfp_d[dd & 31U];
            uint32_t w = (opc1 & 1U) ? (uint32_t)(v >> 32) : (uint32_t)v;
            if (rd != 15U) {
                ctx->regs[rd] = w;
            }
            ctx->regs[15] = pc + 4U;
            return true;
        }
        return false;
    }
    if (opc1 == 7U && !is_double && crn == 1U) {
        ctx->fpscr = (rd == 15U) ? 0U : ctx->regs[rd];
        ctx->regs[15] = pc + 4U;
        return true;
    }
    if (opc1 == 0U && !is_double) {
        unsigned s = (crn << 1) | ((insn >> 7) & 1U);
        uint32_t w = (rd == 15U) ? 0U : ctx->regs[rd];
        float f = 0.0f;
        memcpy(&f, &w, 4U);
        put_s(ctx, s, f);
        ctx->regs[15] = pc + 4U;
        return true;
    }
    if (is_double && (opc1 == 2U || opc1 == 3U)) {
        unsigned dd = crn | (((insn >> 7) & 1U) << 4);
        unsigned lane = ((opc1 & 1U) << 2) | (opc2 & 3U);
        uint32_t w = (rd == 15U) ? 0U : ctx->regs[rd];
        uint64_t v = ctx->vfp_d[dd & 31U];
        uint64_t mask = ~(0xFFULL << (lane * 8U));
        v = (v & mask) | (((uint64_t)(w & 0xFFU)) << (lane * 8U));
        ctx->vfp_d[dd & 31U] = v;
        ctx->regs[15] = pc + 4U;
        return true;
    }
    if (is_double && (opc1 == 0U || opc1 == 1U) && opc2 != 0U) {
        unsigned dd = crn | (((insn >> 7) & 1U) << 4);
        unsigned lane = ((opc1 & 1U) << 1) | ((opc2 >> 1) & 1U);
        uint32_t w = (rd == 15U) ? 0U : ctx->regs[rd];
        uint64_t v = ctx->vfp_d[dd & 31U];
        uint64_t mask = ~(0xFFFFULL << (lane * 16U));
        v = (v & mask) | (((uint64_t)(w & 0xFFFFU)) << (lane * 16U));
        ctx->vfp_d[dd & 31U] = v;
        ctx->regs[15] = pc + 4U;
        return true;
    }
    if (is_double && ((opc1 & 2U) == 0U)) {
        unsigned dd = crn | (((insn >> 7) & 1U) << 4);
        uint32_t w = (rd == 15U) ? 0U : ctx->regs[rd];
        uint64_t v = ctx->vfp_d[dd & 31U];
        if ((opc1 & 1U) != 0U) {
            v = (v & 0xFFFFFFFFULL) | ((uint64_t)w << 32);
        } else {
            v = (v & 0xFFFFFFFF00000000ULL) | w;
        }
        ctx->vfp_d[dd & 31U] = v;
        ctx->regs[15] = pc + 4U;
        return true;
    }
    return false;
}

/* LDC/STC forms: VLDR/VSTR/VLDM/VSTM/VPUSH/VPOP single/double, plus VMOV Rt, Rt2. */
static bool vfp_ldc_stc(a32_ctx_t *ctx, uint32_t pc, uint32_t insn, bool *faulted)
{
    bool p = ((insn >> 24) & 1U) != 0U;
    bool u = ((insn >> 23) & 1U) != 0U;
    bool d_bit = ((insn >> 22) & 1U) != 0U;
    bool w = ((insn >> 21) & 1U) != 0U;
    bool l = ((insn >> 20) & 1U) != 0U;
    bool dbl = (((insn >> 8) & 0xFU) == 11U);
    unsigned rn = (insn >> 16) & 0xFU;
    unsigned vd_bits = (insn >> 12) & 0xFU;
    uint32_t imm8 = insn & 0xFFU;

    /* VMOV Rt, Rt2, Dm / Sm, Sm1 */
    if ((insn & 0x0FE000D0U) == 0x0C400010U) {
        unsigned rt2 = rn;
        unsigned rt = vd_bits;
        if (rt == 15U || rt2 == 15U) {
            return false;
        }
        if (dbl) {
            unsigned dm = (((insn >> 5) & 1U) << 4) | (insn & 0xFU);
            if (l) {
                uint64_t v = ctx->vfp_d[dm & 31U];
                ctx->regs[rt] = (uint32_t)v;
                ctx->regs[rt2] = (uint32_t)(v >> 32);
            } else {
                ctx->vfp_d[dm & 31U] = (uint64_t)ctx->regs[rt] | ((uint64_t)ctx->regs[rt2] << 32);
            }
        } else {
            unsigned sm = ((insn & 0xFU) << 1) | ((insn >> 5) & 1U);
            if (sm >= 31U) {
                return false;
            }
            if (l) {
                float f0 = get_s(ctx, sm);
                float f1 = get_s(ctx, sm + 1U);
                memcpy(&ctx->regs[rt], &f0, 4U);
                memcpy(&ctx->regs[rt2], &f1, 4U);
            } else {
                float f0 = 0.0f;
                float f1 = 0.0f;
                memcpy(&f0, &ctx->regs[rt], 4U);
                memcpy(&f1, &ctx->regs[rt2], 4U);
                put_s(ctx, sm, f0);
                put_s(ctx, sm + 1U, f1);
            }
        }
        ctx->regs[15] = pc + 4U;
        return true;
    }

    /* Literal base is Align(PC + 4, 4) in Thumb mode and PC + 8 in ARM
     * mode (A32-R7-18). */
    bool is_thumb = (ctx->cpsr & A32_CPSR_T) != 0U;
    uint32_t base = (rn == 15U) ? (is_thumb ? ((pc + 4U) & ~3U) : (pc + 8U)) : ctx->regs[rn];
    if (p && !w) {
        uint32_t off = imm8 * 4U;
        uint32_t ea = u ? (base + off) : (base - off);
        /* VLDR/VSTR requires a word-aligned effective address (A32-R7-6). */
        if ((ea & 3U) != 0U) {
            i_fault(ctx, ea, !l);
            *faulted = true;
            return true;
        }
        unsigned reg = vfp_idx(dbl, d_bit, vd_bits);
        if (l) {
            if (dbl) {
                uint64_t v = 0ULL;
                if (!mem_r(ctx, ea, &v, 8U)) {
                    *faulted = true;
                    return true;
                }
                ctx->vfp_d[reg & 31U] = v;
            } else {
                uint32_t word = 0U;
                if (!mem_r(ctx, ea, &word, 4U)) {
                    *faulted = true;
                    return true;
                }
                float f = 0.0f;
                memcpy(&f, &word, 4U);
                put_s(ctx, reg & 31U, f);
            }
        } else {
            if (dbl) {
                uint64_t v = ctx->vfp_d[reg & 31U];
                if (!mem_w(ctx, ea, &v, 8U)) {
                    *faulted = true;
                    return true;
                }
            } else {
                float f = get_s(ctx, reg & 31U);
                uint32_t word = 0U;
                memcpy(&word, &f, 4U);
                if (!mem_w(ctx, ea, &word, 4U)) {
                    *faulted = true;
                    return true;
                }
            }
        }
        ctx->regs[15] = pc + 4U;
        return true;
    }

    /* VLDM / VSTM / VPUSH / VPOP */
    if (imm8 == 0U || p == u) {
        return false;
    }
    unsigned start = vfp_idx(dbl, d_bit, vd_bits);
    unsigned count = dbl ? (imm8 / 2U) : imm8;
    uint32_t total_bytes = imm8 * 4U;
    uint32_t addr = u ? base : (base - total_bytes);
    uint32_t step = dbl ? 8U : 4U;
    for (unsigned i = 0U; i < count; i++) {
        unsigned reg = (start + i) & 31U;
        if (l) {
            if (dbl) {
                uint64_t v = 0ULL;
                if (!mem_r(ctx, addr, &v, 8U)) {
                    *faulted = true;
                    return true;
                }
                ctx->vfp_d[reg] = v;
            } else {
                uint32_t word = 0U;
                if (!mem_r(ctx, addr, &word, 4U)) {
                    *faulted = true;
                    return true;
                }
                float f = 0.0f;
                memcpy(&f, &word, 4U);
                put_s(ctx, reg, f);
            }
        } else {
            if (dbl) {
                uint64_t v = ctx->vfp_d[reg];
                if (!mem_w(ctx, addr, &v, 8U)) {
                    *faulted = true;
                    return true;
                }
            } else {
                float f = get_s(ctx, reg);
                uint32_t word = 0U;
                memcpy(&word, &f, 4U);
                if (!mem_w(ctx, addr, &word, 4U)) {
                    *faulted = true;
                    return true;
                }
            }
        }
        addr += step;
    }
    if (w && rn != 15U) {
        ctx->regs[rn] = u ? (base + total_bytes) : (base - total_bytes);
    }
    ctx->regs[15] = pc + 4U;
    return true;
}

/* CDP data-processing: VADD/VSUB/VMUL/VNMUL/VDIV/VMLA/VMLS/VFMA/VFMS/VMOV/VABS/VNEG/VSQRT/VCMP/VCVT. */
static bool vfp_cdp(a32_ctx_t *ctx, uint32_t pc, uint32_t insn)
{
    bool dbl = (((insn >> 8) & 0xFU) == 11U);
    unsigned opc1 = ((insn >> 20) & 0xFU) & ~4U;
    unsigned opc2 = (insn >> 16) & 0xFU;
    bool d_bit = ((insn >> 22) & 1U) != 0U;
    unsigned vd_b = (insn >> 12) & 0xFU;
    unsigned vn_b = (insn >> 16) & 0xFU;
    unsigned vm_b = insn & 0xFU;
    bool n_bit = ((insn >> 7) & 1U) != 0U;
    bool op_bit = ((insn >> 6) & 1U) != 0U;
    bool m_bit = ((insn >> 5) & 1U) != 0U;
    unsigned vd = vfp_idx(dbl, d_bit, vd_b);
    unsigned vn = vfp_idx(dbl, n_bit, vn_b);
    unsigned vm = vfp_idx(dbl, m_bit, vm_b);

    if (opc1 == 0xBU) {
        if ((insn & 0xC0U) == 0U) {
            uint32_t imm8 = (vn_b << 4) | vm_b;
            if (dbl) {
                put_d(ctx, vd, expand_imm_f64(imm8));
            } else {
                put_s(ctx, vd, expand_imm_f32(imm8));
            }
            ctx->regs[15] = pc + 4U;
            return true;
        }
        if (!op_bit) {
            return false;
        }
        if (opc2 == 0U) {
            if (dbl) {
                double a = get_d(ctx, vm);
                put_d(ctx, vd, n_bit ? fabs(a) : a);
            } else {
                float a = get_s(ctx, vm);
                put_s(ctx, vd, n_bit ? fabsf(a) : a);
            }
            ctx->regs[15] = pc + 4U;
            return true;
        }
        if (opc2 == 1U) {
            if (n_bit) {
                fenv_t saved_env;
                vfp_fenv_begin(ctx, &saved_env);
                if (dbl) {
                    double a = vfp_fz_in_d(ctx, get_d(ctx, vm));
                    put_d(ctx, vd, vfp_fz_out_d(ctx, vfp_dn_d(ctx, sqrt(a))));
                } else {
                    float a = vfp_fz_in_f(ctx, get_s(ctx, vm));
                    put_s(ctx, vd, vfp_fz_out_f(ctx, vfp_dn_f(ctx, sqrtf(a))));
                }
                vfp_fenv_end(ctx, &saved_env);
            } else {
                if (dbl) {
                    put_d(ctx, vd, -get_d(ctx, vm));
                } else {
                    put_s(ctx, vd, -get_s(ctx, vm));
                }
            }
            ctx->regs[15] = pc + 4U;
            return true;
        }
        if (opc2 == 4U || opc2 == 5U) {
            bool zero = (opc2 == 5U);
            if (dbl) {
                double a = get_d(ctx, vd);
                double b = zero ? 0.0 : get_d(ctx, vm);
                /* IOC for VCMPE, and for either form on signaling-NaN
                 * inputs classified from payload bits (A32-R7-30). */
                if (a != a || b != b) {
                    if (n_bit || vfp_snan_d(a) || vfp_snan_d(b)) {
                        ctx->fpscr |= (1U << 0);
                    }
                    cmp_set_fpscr(ctx, false, false, true, true);
                } else if (a == b) {
                    cmp_set_fpscr(ctx, false, true, true, false);
                } else if (a < b) {
                    cmp_set_fpscr(ctx, true, false, false, false);
                } else {
                    cmp_set_fpscr(ctx, false, false, true, false);
                }
            } else {
                float a = get_s(ctx, vd);
                float b = zero ? 0.0f : get_s(ctx, vm);
                if (a != a || b != b) {
                    if (n_bit || vfp_snan_f(a) || vfp_snan_f(b)) {
                        ctx->fpscr |= (1U << 0);
                    }
                    cmp_set_fpscr(ctx, false, false, true, true);
                } else if (a == b) {
                    cmp_set_fpscr(ctx, false, true, true, false);
                } else if (a < b) {
                    cmp_set_fpscr(ctx, true, false, false, false);
                } else {
                    cmp_set_fpscr(ctx, false, false, true, false);
                }
            }
            ctx->regs[15] = pc + 4U;
            return true;
        }
        if (opc2 == 7U && n_bit) {
            fenv_t saved_env;
            vfp_fenv_begin(ctx, &saved_env);
            if (dbl) {
                unsigned s = (vd_b << 1) | (d_bit ? 1U : 0U);
                double a = vfp_fz_in_d(ctx, get_d(ctx, vm));
                put_s(ctx, s, vfp_fz_out_f(ctx, vfp_dn_f(ctx, (float)a)));
            } else {
                unsigned d = (d_bit ? 16U : 0U) | vd_b;
                float a = vfp_fz_in_f(ctx, get_s(ctx, vm));
                put_d(ctx, d, vfp_fz_out_d(ctx, vfp_dn_d(ctx, (double)a)));
            }
            vfp_fenv_end(ctx, &saved_env);
            ctx->regs[15] = pc + 4U;
            return true;
        }
        if (opc2 == 8U) {
            fenv_t saved_env;
            vfp_fenv_begin(ctx, &saved_env);
            unsigned m = (vm_b << 1) | (m_bit ? 1U : 0U);
            float f = get_s(ctx, m);
            uint32_t raw = 0U;
            memcpy(&raw, &f, 4U);
            if (dbl) {
                put_d(ctx, vd, n_bit ? (double)(int32_t)raw : (double)raw);
            } else {
                put_s(ctx, vd, n_bit ? (float)(int32_t)raw : (float)raw);
            }
            vfp_fenv_end(ctx, &saved_env);
            ctx->regs[15] = pc + 4U;
            return true;
        }
        if (opc2 == 12U || opc2 == 13U) {
            fenv_t saved_env;
            vfp_fenv_begin(ctx, &saved_env);
            bool is_signed = (opc2 == 13U);
            unsigned s = (vd_b << 1) | (d_bit ? 1U : 0U);
            double v = dbl ? vfp_fz_in_d(ctx, get_d(ctx, vm)) :
                (double)vfp_fz_in_f(ctx, get_s(ctx, vm));
            if (!n_bit) {
                v = rint(v);
            } else {
                double tr = trunc(v);
                if (tr != v && isfinite(v)) {
                    (void)feraiseexcept(FE_INEXACT);
                }
                v = tr;
            }
            uint32_t out_w = 0U;
            if (v != v) {
                (void)feraiseexcept(FE_INVALID);
                out_w = 0U;
            } else if (is_signed) {
                if (v > 2147483647.0) {
                    (void)feraiseexcept(FE_INVALID);
                    out_w = 0x7FFFFFFFU;
                } else if (v >= 2147483647.0) {
                    out_w = 0x7FFFFFFFU;
                } else if (v < -2147483648.0) {
                    (void)feraiseexcept(FE_INVALID);
                    out_w = 0x80000000U;
                } else if (v <= -2147483648.0) {
                    out_w = 0x80000000U;
                } else {
                    out_w = (uint32_t)(int32_t)v;
                }
            } else {
                if (v > 4294967295.0) {
                    (void)feraiseexcept(FE_INVALID);
                    out_w = 0xFFFFFFFFU;
                } else if (v >= 4294967295.0) {
                    out_w = 0xFFFFFFFFU;
                } else if (v < 0.0) {
                    (void)feraiseexcept(FE_INVALID);
                    out_w = 0U;
                } else if (v <= 0.0) {
                    out_w = 0U;
                } else {
                    out_w = (uint32_t)v;
                }
            }
            vfp_fenv_end(ctx, &saved_env);
            float out_f = 0.0f;
            memcpy(&out_f, &out_w, 4U);
            put_s(ctx, s, out_f);
            ctx->regs[15] = pc + 4U;
            return true;
        }
        return false;
    }

    if (opc1 <= 3U || (opc1 == 8U && !op_bit) || opc1 == 9U || opc1 == 10U) {
        /* opc1 bit 3 (8) selects fused (VFMA/VFMS) over non-fused
         * (VMLA/VMLS): fused forms use one rounding via fma(), non-fused
         * use separately-rounded multiply and add (A32-R7-29). With
         * FP_CONTRACT OFF the C expressions below cannot contract. */
        bool fused = (opc1 & 8U) != 0U;
        fenv_t saved_env;
        vfp_fenv_begin(ctx, &saved_env);
        if (dbl) {
            double a = vfp_fz_in_d(ctx, get_d(ctx, vn));
            double b = vfp_fz_in_d(ctx, get_d(ctx, vm));
            double d = vfp_fz_in_d(ctx, get_d(ctx, vd));
            double r = d;
            if (opc1 == 3U) {
                r = op_bit ? (a - b) : (a + b);
            } else if (opc1 == 2U) {
                r = op_bit ? -(a * b) : (a * b);
            } else if (opc1 == 8U) {
                r = a / b;
            } else if (opc1 == 0U || opc1 == 10U) {
                if (fused) {
                    r = fma(a, b, d);
                    if (op_bit) {
                        r = -fma(a, b, -d);
                    }
                } else {
                    r = op_bit ? (d - a * b) : (d + a * b);
                }
            } else if (opc1 == 1U || opc1 == 9U) {
                if (fused) {
                    r = op_bit ? -fma(a, b, d) : fma(a, b, -d);
                } else {
                    r = op_bit ? (-d - a * b) : (-d + a * b);
                }
            }
            put_d(ctx, vd, vfp_fz_out_d(ctx, vfp_dn_d(ctx, r)));
        } else {
            float a = vfp_fz_in_f(ctx, get_s(ctx, vn));
            float b = vfp_fz_in_f(ctx, get_s(ctx, vm));
            float d = vfp_fz_in_f(ctx, get_s(ctx, vd));
            float r = d;
            if (opc1 == 3U) {
                r = op_bit ? (a - b) : (a + b);
            } else if (opc1 == 2U) {
                r = op_bit ? -(a * b) : (a * b);
            } else if (opc1 == 8U) {
                r = a / b;
            } else if (opc1 == 0U || opc1 == 10U) {
                if (fused) {
                    r = fmaf(a, b, d);
                    if (op_bit) {
                        r = -fmaf(a, b, -d);
                    }
                } else {
                    r = op_bit ? (d - a * b) : (d + a * b);
                }
            } else if (opc1 == 1U || opc1 == 9U) {
                if (fused) {
                    r = op_bit ? -fmaf(a, b, d) : fmaf(a, b, -d);
                } else {
                    r = op_bit ? (-d - a * b) : (-d + a * b);
                }
            }
            put_s(ctx, vd, vfp_fz_out_f(ctx, vfp_dn_f(ctx, r)));
        }
        vfp_fenv_end(ctx, &saved_env);
        ctx->regs[15] = pc + 4U;
        return true;
    }
    return false;
}

bool a32_interp_vfp_step(a32_ctx_t *ctx, uint32_t insn)
{
    if (ctx == NULL) {
        return false;
    }
    uint32_t pc = ctx->regs[15] & ~3U;
    /* Thumb-2 synthetic words arrive with pc already masked by caller. */
    if ((ctx->cpsr & A32_CPSR_T) != 0U) {
        pc = ctx->regs[15] & ~1U;
    }
    if (((insn >> 28) & 0xFU) == 0xFU) {
        bool faulted = false;
        return i_vfp_neon(ctx, ctx->regs[15] & ~1U, insn, &faulted);
    }
    unsigned cls = (insn >> 24) & 0xFU;
    if (cls == 0xCU || cls == 0xDU) {
        bool faulted = false;
        if (vfp_ldc_stc(ctx, ctx->regs[15] & ~1U, insn, &faulted)) {
            if (faulted) {
                return true;
            }
            /* vfp_ldc_stc advanced regs[15] from the raw value it was given. */
            if ((ctx->cpsr & A32_CPSR_T) != 0U && ctx->regs[15] == (pc & ~1U)) {
                ctx->regs[15] = (pc & ~1U) + 4U;
            }
            return true;
        }
        return false;
    }
    if (cls == 0xEU) {
        unsigned cp = (insn >> 8) & 0xFU;
        if (cp != 10U && cp != 11U) {
            return false;
        }
        if (((insn >> 4) & 1U) != 0U) {
            return vfp_mcr_mrc(ctx, ctx->regs[15] & ~1U, insn);
        }
        return vfp_cdp(ctx, ctx->regs[15] & ~1U, insn);
    }
    (void)pc;
    return false;
}
