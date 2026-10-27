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

#include <math.h>
#include <string.h>
#include <sys/mman.h>

#define FPSCR_N (1U << 31)
#define FPSCR_Z (1U << 30)
#define FPSCR_C (1U << 29)
#define FPSCR_V (1U << 28)

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

static bool mem_r(a32_ctx_t *ctx, uint32_t va, void *out, uint32_t len)
{
    if (!a32_guest_range_ok(ctx, va, len, 1)) {
        ctx->fault_addr = va;
        ctx->exit_reason = A32_EXIT_FAULT;
        return false;
    }
    memcpy(out, a32_guest_ptr(ctx, va), len);
    return true;
}

static bool mem_w(a32_ctx_t *ctx, uint32_t va, const void *in, uint32_t len)
{
    if (!a32_guest_range_ok(ctx, va, len, 2)) {
        ctx->fault_addr = va;
        ctx->exit_reason = A32_EXIT_FAULT;
        return false;
    }
    memcpy(a32_guest_ptr(ctx, va), in, len);
    return true;
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

/* MCR/MRC between ARM core and VFP system regs. */
static bool vfp_mcr_mrc(a32_ctx_t *ctx, uint32_t pc, uint32_t insn)
{
    bool load = ((insn >> 20) & 1U) != 0U;
    bool is_double = (((insn >> 8) & 0xFU) == 11U);
    unsigned opc1 = (insn >> 21) & 7U;
    unsigned crn = (insn >> 16) & 0xFU;
    unsigned rd = (insn >> 12) & 0xFU;
    unsigned crm = insn & 0xFU;
    unsigned opc2 = (insn >> 5) & 7U;
    (void)crm;
    (void)opc2;

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

    uint32_t base = (rn == 15U) ? (pc + 8U) : ctx->regs[rn];
    if (p && !w) {
        uint32_t off = imm8 * 4U;
        uint32_t ea = u ? (base + off) : (base - off);
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
            if (dbl) {
                double a = get_d(ctx, vm);
                put_d(ctx, vd, n_bit ? sqrt(a) : -a);
            } else {
                float a = get_s(ctx, vm);
                put_s(ctx, vd, n_bit ? sqrtf(a) : -a);
            }
            ctx->regs[15] = pc + 4U;
            return true;
        }
        if (opc2 == 4U || opc2 == 5U) {
            bool zero = (opc2 == 5U);
            if (dbl) {
                double a = get_d(ctx, vd);
                double b = zero ? 0.0 : get_d(ctx, vm);
                if (a != a || b != b) {
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
            if (dbl) {
                unsigned s = (vd_b << 1) | (d_bit ? 1U : 0U);
                put_s(ctx, s, (float)get_d(ctx, vm));
            } else {
                unsigned d = (d_bit ? 16U : 0U) | vd_b;
                put_d(ctx, d, (double)get_s(ctx, vm));
            }
            ctx->regs[15] = pc + 4U;
            return true;
        }
        if (opc2 == 8U) {
            unsigned m = (vm_b << 1) | (m_bit ? 1U : 0U);
            float f = get_s(ctx, m);
            uint32_t raw = 0U;
            memcpy(&raw, &f, 4U);
            if (dbl) {
                put_d(ctx, vd, n_bit ? (double)(int32_t)raw : (double)raw);
            } else {
                put_s(ctx, vd, n_bit ? (float)(int32_t)raw : (float)raw);
            }
            ctx->regs[15] = pc + 4U;
            return true;
        }
        if (opc2 == 12U || opc2 == 13U) {
            bool is_signed = (opc2 == 13U);
            unsigned s = (vd_b << 1) | (d_bit ? 1U : 0U);
            double v = dbl ? get_d(ctx, vm) : (double)get_s(ctx, vm);
            if (!n_bit) {
                v = nearbyint(v);
            }
            uint32_t out_w = 0U;
            if (v != v) {
                out_w = 0U;
            } else if (is_signed) {
                if (v >= 2147483647.0) {
                    out_w = 0x7FFFFFFFU;
                } else if (v <= -2147483648.0) {
                    out_w = 0x80000000U;
                } else {
                    out_w = (uint32_t)(int32_t)v;
                }
            } else {
                if (v >= 4294967295.0) {
                    out_w = 0xFFFFFFFFU;
                } else if (v <= 0.0) {
                    out_w = 0U;
                } else {
                    out_w = (uint32_t)v;
                }
            }
            float out_f = 0.0f;
            memcpy(&out_f, &out_w, 4U);
            put_s(ctx, s, out_f);
            ctx->regs[15] = pc + 4U;
            return true;
        }
        return false;
    }

    if (opc1 <= 3U || (opc1 == 8U && !op_bit) || opc1 == 9U || opc1 == 10U) {
        if (dbl) {
            double a = get_d(ctx, vn);
            double b = get_d(ctx, vm);
            double d = get_d(ctx, vd);
            double r = d;
            if (opc1 == 3U) {
                r = op_bit ? (a - b) : (a + b);
            } else if (opc1 == 2U) {
                r = op_bit ? -(a * b) : (a * b);
            } else if (opc1 == 8U) {
                r = a / b;
            } else if (opc1 == 0U || opc1 == 10U) {
                r = op_bit ? (d - a * b) : (d + a * b);
            } else if (opc1 == 1U || opc1 == 9U) {
                r = op_bit ? (-d - a * b) : (-d + a * b);
            }
            put_d(ctx, vd, r);
        } else {
            float a = get_s(ctx, vn);
            float b = get_s(ctx, vm);
            float d = get_s(ctx, vd);
            float r = d;
            if (opc1 == 3U) {
                r = op_bit ? (a - b) : (a + b);
            } else if (opc1 == 2U) {
                r = op_bit ? -(a * b) : (a * b);
            } else if (opc1 == 8U) {
                r = a / b;
            } else if (opc1 == 0U || opc1 == 10U) {
                r = op_bit ? (d - a * b) : (d + a * b);
            } else if (opc1 == 1U || opc1 == 9U) {
                r = op_bit ? (-d - a * b) : (-d + a * b);
            }
            put_s(ctx, vd, r);
        }
        ctx->regs[15] = pc + 4U;
        return true;
    }
    return false;
}

static bool vfp_neon(a32_ctx_t *ctx, uint32_t pc, uint32_t insn, bool *faulted)
{
    if ((insn & 0xFF900000U) == 0xF4000000U) {
        bool l = ((insn >> 21) & 1U) != 0U;
        unsigned rn = (insn >> 16) & 0xFU;
        unsigned d_start = (((insn >> 22) & 1U) << 4) | ((insn >> 12) & 0xFU);
        unsigned type = (insn >> 8) & 0xFU;
        unsigned rm = insn & 0xFU;
        unsigned nregs = (type == 0x7U) ? 1U : (type == 0xAU) ? 2U :
                         (type == 0x6U) ? 3U : (type == 0x2U) ? 4U : 0U;
        if (nregs == 0U) {
            return false;
        }
        uint32_t addr = ctx->regs[rn];
        for (unsigned r = 0U; r < nregs; r++) {
            unsigned d = (d_start + r) & 31U;
            if (l) {
                if (!mem_r(ctx, addr + r * 8U, &ctx->vfp_d[d], 8U)) {
                    *faulted = true;
                    return true;
                }
            } else {
                if (!mem_w(ctx, addr + r * 8U, &ctx->vfp_d[d], 8U)) {
                    *faulted = true;
                    return true;
                }
            }
        }
        if (rm != 0xFU) {
            ctx->regs[rn] = addr + ((rm == 0xDU) ? (nregs * 8U) : ctx->regs[rm]);
        }
        ctx->regs[15] = pc + 4U;
        return true;
    }
    if ((insn & 0xFEB80090U) == 0xF2800010U) {
        uint32_t imm8 = (((insn >> 24) & 1U) << 7) | (((insn >> 16) & 7U) << 4) | (insn & 0xFU);
        unsigned cmode = (insn >> 8) & 0xFU;
        unsigned op = (insn >> 5) & 1U;
        bool is_q = ((insn >> 6) & 1U) != 0U;
        unsigned dd = (((insn >> 22) & 1U) << 4) | ((insn >> 12) & 0xFU);
        uint64_t val64 = 0ULL;
        if ((cmode & 1U) == 0U && cmode < 8U) {
            uint32_t w = imm8 << ((cmode >> 1) * 8U);
            if (op != 0U) {
                w = ~w;
            }
            val64 = ((uint64_t)w << 32) | w;
        } else if (cmode == 0xEU && op == 0U) {
            for (unsigned b = 0U; b < 8U; b++) {
                val64 |= (uint64_t)imm8 << (b * 8U);
            }
        } else if (cmode == 0xEU && op == 1U) {
            for (unsigned b = 0U; b < 8U; b++) {
                if (((imm8 >> b) & 1U) != 0U) {
                    val64 |= 0xFFULL << (b * 8U);
                }
            }
        }
        ctx->vfp_d[dd & 31U] = val64;
        if (is_q) {
            ctx->vfp_d[(dd + 1U) & 31U] = val64;
        }
        ctx->regs[15] = pc + 4U;
        return true;
    }
    if ((insn & 0xFFB30FD0U) == 0xF3B00000U) {
        unsigned size = (insn >> 18) & 3U;
        bool is_q = ((insn >> 6) & 1U) != 0U;
        unsigned dd = (((insn >> 22) & 1U) << 4) | ((insn >> 12) & 0xFU);
        unsigned dm = (((insn >> 5) & 1U) << 4) | (insn & 0xFU);
        unsigned regs = is_q ? 2U : 1U;
        for (unsigned r = 0U; r < regs; r++) {
            uint64_t v = ctx->vfp_d[(dm + r) & 31U];
            if (size == 0U) {
                v = __builtin_bswap64(v);
            } else if (size == 1U) {
                v = ((uint64_t)__builtin_bswap16((uint16_t)(v >> 48))) |
                    ((uint64_t)__builtin_bswap16((uint16_t)(v >> 32)) << 16) |
                    ((uint64_t)__builtin_bswap16((uint16_t)(v >> 16)) << 32) |
                    ((uint64_t)__builtin_bswap16((uint16_t)v) << 48);
            } else if (size == 2U) {
                v = (v >> 32) | (v << 32);
            }
            ctx->vfp_d[(dd + r) & 31U] = v;
        }
        ctx->regs[15] = pc + 4U;
        return true;
    }
    if ((insn & 0xFE800F10U) == 0xF2000800U) {
        bool is_sub = ((insn >> 24) & 1U) != 0U;
        unsigned size = (insn >> 20) & 3U;
        bool is_q = ((insn >> 6) & 1U) != 0U;
        unsigned dd = (((insn >> 22) & 1U) << 4) | ((insn >> 12) & 0xFU);
        unsigned dn = (((insn >> 7) & 1U) << 4) | ((insn >> 16) & 0xFU);
        unsigned dm = (((insn >> 5) & 1U) << 4) | (insn & 0xFU);
        unsigned regs = is_q ? 2U : 1U;
        for (unsigned r = 0U; r < regs; r++) {
            uint64_t a = ctx->vfp_d[(dn + r) & 31U];
            uint64_t b = ctx->vfp_d[(dm + r) & 31U];
            uint64_t out = 0ULL;
            if (size == 0U) {
                for (unsigned k = 0U; k < 8U; k++) {
                    uint8_t x = (uint8_t)(a >> (k * 8U));
                    uint8_t y = (uint8_t)(b >> (k * 8U));
                    uint8_t z = is_sub ? (uint8_t)(x - y) : (uint8_t)(x + y);
                    out |= (uint64_t)z << (k * 8U);
                }
            } else if (size == 1U) {
                for (unsigned k = 0U; k < 4U; k++) {
                    uint16_t x = (uint16_t)(a >> (k * 16U));
                    uint16_t y = (uint16_t)(b >> (k * 16U));
                    uint16_t z = is_sub ? (uint16_t)(x - y) : (uint16_t)(x + y);
                    out |= (uint64_t)z << (k * 16U);
                }
            } else if (size == 2U) {
                for (unsigned k = 0U; k < 2U; k++) {
                    uint32_t x = (uint32_t)(a >> (k * 32U));
                    uint32_t y = (uint32_t)(b >> (k * 32U));
                    uint32_t z = is_sub ? (x - y) : (x + y);
                    out |= (uint64_t)z << (k * 32U);
                }
            } else {
                out = is_sub ? (a - b) : (a + b);
            }
            ctx->vfp_d[(dd + r) & 31U] = out;
        }
        ctx->regs[15] = pc + 4U;
        return true;
    }
    if ((insn & 0xFE800F10U) == 0xF2000110U) {
        bool u = ((insn >> 24) & 1U) != 0U;
        unsigned op2 = (insn >> 20) & 3U;
        bool is_q = ((insn >> 6) & 1U) != 0U;
        unsigned dd = (((insn >> 22) & 1U) << 4) | ((insn >> 12) & 0xFU);
        unsigned dn = (((insn >> 7) & 1U) << 4) | ((insn >> 16) & 0xFU);
        unsigned dm = (((insn >> 5) & 1U) << 4) | (insn & 0xFU);
        unsigned regs = is_q ? 2U : 1U;
        for (unsigned r = 0U; r < regs; r++) {
            uint64_t a = ctx->vfp_d[(dn + r) & 31U];
            uint64_t b = ctx->vfp_d[(dm + r) & 31U];
            uint64_t d = ctx->vfp_d[(dd + r) & 31U];
            uint64_t out = 0ULL;
            if (!u) {
                out = (op2 == 0U) ? (a & b) : (op2 == 1U) ? (a & ~b) :
                      (op2 == 2U) ? (a | b) : (a | ~b);
            } else {
                out = (op2 == 0U) ? (a ^ b) : (op2 == 1U) ? ((a & d) | (b & ~d)) :
                      (op2 == 2U) ? ((d & ~b) | (a & b)) : ((d & b) | (a & ~b));
            }
            ctx->vfp_d[(dd + r) & 31U] = out;
        }
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
        return vfp_neon(ctx, ctx->regs[15] & ~1U, insn, &faulted);
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
