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

/* MCR/MRC between ARM core and VFP system regs. */
static bool vfp_mcr_mrc(a32_ctx_t *ctx, uint32_t pc, uint32_t insn)
{
    bool load = ((insn >> 20) & 1U) != 0U;
    unsigned opc1 = (insn >> 21) & 7U;
    unsigned crn = (insn >> 16) & 0xFU;
    unsigned rd = (insn >> 12) & 0xFU;
    unsigned crm = insn & 0xFU;
    unsigned opc2 = (insn >> 5) & 7U;

    if (load) {
        /* MRC: VFP -> ARM. */
        if (opc1 == 7U && crn == 1U && crm == 0U && opc2 == 0U && rd == 15U) {
            /* VMRS APSR_nzcv, FPSCR (FMSTAT). */
            fpscr_to_cpsr(ctx);
            ctx->regs[15] = pc + 4U;
            return true;
        }
        if (opc1 == 7U && crn == 1U && crm == 0U && opc2 == 0U) {
            /* VMRS Rd, FPSCR. */
            if (rd != 15U) {
                ctx->regs[rd] = ctx->fpscr;
            }
            ctx->regs[15] = pc + 4U;
            return true;
        }
        if (opc1 == 0U && (crm == 10U || crm == 11U)) {
            /* VMOV Rd, Sn. */
            unsigned s = ((insn >> 15) & 0x1EU) | ((insn >> 7) & 1U);
            (void)crn;
            uint32_t w = 0U;
            float f = get_s(ctx, s);
            memcpy(&w, &f, 4U);
            if (rd != 15U) {
                ctx->regs[rd] = w;
            }
            ctx->regs[15] = pc + 4U;
            return true;
        }
        if (opc1 == 4U) {
            /* VMOV Rd, Dn[32]: crm selects low/high word. */
            unsigned d = (insn >> 16) & 0xFU;
            d = ((crn >> 1) & 0xFU) | ((crn & 1U) << 4);
            (void)d;
            unsigned dd = ((insn >> 16) & 0xFU) | (((insn >> 7) & 1U) << 4);
            uint64_t v = ctx->vfp_d[dd & 31U];
            uint32_t w = (crm & 1U) ? (uint32_t)(v >> 32) : (uint32_t)v;
            if (rd != 15U) {
                ctx->regs[rd] = w;
            }
            ctx->regs[15] = pc + 4U;
            return true;
        }
        return false;
    }
    /* MCR: ARM -> VFP. */
    if (opc1 == 7U && crn == 1U && crm == 0U && opc2 == 0U) {
        /* VMSR FPSCR, Rd. */
        ctx->fpscr = (rd == 15U) ? 0U : ctx->regs[rd];
        ctx->regs[15] = pc + 4U;
        return true;
    }
    if (opc1 == 0U && (crm == 10U || crm == 11U)) {
        unsigned s = ((insn >> 15) & 0x1EU) | ((insn >> 7) & 1U);
        uint32_t w = (rd == 15U) ? 0U : ctx->regs[rd];
        float f = 0.0f;
        memcpy(&f, &w, 4U);
        put_s(ctx, s, f);
        ctx->regs[15] = pc + 4U;
        return true;
    }
    if (opc1 == 4U) {
        unsigned dd = ((insn >> 16) & 0xFU) | ((((insn >> 7) & 1U)) << 4);
        uint32_t w = (rd == 15U) ? 0U : ctx->regs[rd];
        uint64_t v = ctx->vfp_d[dd & 31U];
        if ((crm & 1U) != 0U) {
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

/* LDC/STC forms: VLDR/VSTR single/double. */
static bool vfp_ldc_stc(a32_ctx_t *ctx, uint32_t pc, uint32_t insn, bool *faulted)
{
    bool p = ((insn >> 24) & 1U) != 0U;
    bool u = ((insn >> 23) & 1U) != 0U;
    bool w = ((insn >> 21) & 1U) != 0U;
    bool l = ((insn >> 20) & 1U) != 0U;
    bool dbl = ((insn >> 8) & 1U) != 0U;
    unsigned rn = (insn >> 16) & 0xFU;
    unsigned vd = ((insn >> 12) & 0xFU) | (((insn >> 22) & 1U) << 4);
    uint32_t off = (insn & 0xFFU) << 2;
    uint32_t base = (rn == 15U) ? (pc + 8U) : ctx->regs[rn];
    uint32_t ea = u ? base + off : base - off;
    if (!p) {
        ea = base;
    }
    uint32_t len = dbl ? 8U : 4U;
    if (l) {
        if (dbl) {
            uint64_t v = 0ULL;
            if (!mem_r(ctx, ea, &v, 8U)) {
                *faulted = true;
                return true;
            }
            ctx->vfp_d[vd & 31U] = v;
        } else {
            uint32_t word = 0U;
            if (!mem_r(ctx, ea, &word, 4U)) {
                *faulted = true;
                return true;
            }
            float f = 0.0f;
            memcpy(&f, &word, 4U);
            put_s(ctx, vd & 31U, f);
        }
    } else {
        if (dbl) {
            uint64_t v = ctx->vfp_d[vd & 31U];
            if (!mem_w(ctx, ea, &v, 8U)) {
                *faulted = true;
                return true;
            }
        } else {
            float f = get_s(ctx, vd & 31U);
            uint32_t word = 0U;
            memcpy(&word, &f, 4U);
            if (!mem_w(ctx, ea, &word, 4U)) {
                *faulted = true;
                return true;
            }
        }
    }
    if (w || !p) {
        uint32_t nb = u ? base + off : base - off;
        if (!p) {
            nb = base + (u ? off : (uint32_t)(-(int32_t)off));
        }
        if (rn != 15U) {
            ctx->regs[rn] = nb;
        }
    }
    if (!p) {
        (void)len;
    }
    ctx->regs[15] = pc + 4U;
    return true;
}

/* CDP data-processing: VADD/VSUB/VMUL/VDIV/VMOV/VABS/VNEG/VSQRT/VCMP/VCVT. */
static bool vfp_cdp(a32_ctx_t *ctx, uint32_t pc, uint32_t insn)
{
    bool dbl = ((insn >> 8) & 1U) != 0U;
    unsigned opc1 = (insn >> 20) & 0xFU;
    unsigned vd = ((insn >> 12) & 0xFU) | (((insn >> 22) & 1U) << 4);
    unsigned vn = ((insn >> 16) & 0xFU) | (((insn >> 7) & 1U) << 4);
    unsigned vm = (insn & 0xFU) | ((((insn >> 5) & 1U)) << 4);
    vm &= 31U;
    vd &= 31U;
    vn &= 31U;

    if (opc1 == 0xBU && ((insn >> 16) & 0xFU) == 0xBU) {
        /* VCMP(E): bit7 E, bit6 quiet. */
        if (dbl) {
            double a = get_d(ctx, vn);
            double b = ((((insn >> 5) & 1U) == 0U) && ((insn & 0xFU) == 0U) && (((insn >> 1) & 1U) == 0U))
                ? 0.0 : get_d(ctx, vm);
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
            float a = get_s(ctx, vn);
            float b = ((((insn >> 5) & 1U) == 0U) && ((insn & 0xFU) == 0U) && (((insn >> 1) & 1U) == 0U))
                ? 0.0f : get_s(ctx, vm);
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
    if (opc1 == 0xBU && ((insn >> 16) & 0xFU) == 0xAU) {
        /* VCVT / VCVTR / VJCVT subset: double<->single and int<->fp. */
        bool to_int = ((insn >> 18) & 1U) != 0U;
        bool sx16 = ((insn >> 5) & 1U) != 0U;
        (void)sx16;
        if (!to_int) {
            if (dbl) {
                float a = get_s(ctx, vn);
                put_d(ctx, vd, (double)a);
            } else {
                double a = get_d(ctx, vn);
                put_s(ctx, vd, (float)a);
            }
            ctx->regs[15] = pc + 4U;
            return true;
        }
        return false;
    }
    if (opc1 == 0xFU) {
        /* VSQRT / VABS / VNEG / VMOV. */
        unsigned fn = (insn >> 16) & 0xFU;
        if (fn == 0x1U) {
            if (dbl) {
                put_d(ctx, vd, sqrt(get_d(ctx, vm)));
            } else {
                put_s(ctx, vd, sqrtf(get_s(ctx, vm)));
            }
            ctx->regs[15] = pc + 4U;
            return true;
        }
        if (fn == 0x0U) {
            if (dbl) {
                double a = get_d(ctx, vm);
                put_d(ctx, vd, (insn & 1U) ? -a : (a < 0 ? -a : a));
            } else {
                float a = get_s(ctx, vm);
                put_s(ctx, vd, (insn & 1U) ? -a : fabsf(a));
            }
            ctx->regs[15] = pc + 4U;
            return true;
        }
        return false;
    }
    if (opc1 <= 0x3U) {
        if (dbl) {
            double a = get_d(ctx, vn);
            double b = get_d(ctx, vm);
            double r = a;
            if (opc1 == 0x0U) {
                r = a + b;
            } else if (opc1 == 0x1U) {
                r = a - b;
            } else if (opc1 == 0x2U) {
                r = a * b;
            } else {
                r = (b == 0.0) ? ((a == 0.0) ? 0.0 / 0.0 : a * 1.0 / 0.0) : a / b;
            }
            put_d(ctx, vd, r);
        } else {
            float a = get_s(ctx, vn);
            float b = get_s(ctx, vm);
            float r = a;
            if (opc1 == 0x0U) {
                r = a + b;
            } else if (opc1 == 0x1U) {
                r = a - b;
            } else if (opc1 == 0x2U) {
                r = a * b;
            } else {
                r = a / b;
            }
            put_s(ctx, vd, r);
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
