/*
 * src/interp/interp_vfp_neon.c — NEON reference subset for the interpreter
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Split from interp_vfp.c (line-limit split): VLDn/VSTn, VMOV immediate,
 * VREV and the integer VADD/VSUB/VALU/VMOV subset. Called from
 * a32_interp_vfp_step for cond==0xF words.
 */

#include "interp.h"
#include "interp_priv.h"

#include "interp/decode_priv.h"

#include <fenv.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif

static void neon_fenv_begin(fenv_t *saved_env)
{
    (void)fegetenv(saved_env);
    (void)feclearexcept(FE_ALL_EXCEPT);
    (void)fesetround(FE_TONEAREST);
}

static void neon_fenv_end(a32_ctx_t *ctx, const fenv_t *saved_env)
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

static float neon_f32_canon_in(a32_ctx_t *ctx, float x)
{
    uint32_t w = 0U;
    memcpy(&w, &x, 4U);
    if ((w & 0x7F800000U) == 0U && (w & 0x007FFFFFU) != 0U) {
        ctx->fpscr |= (1U << 7);
        w &= 0x80000000U;
        memcpy(&x, &w, 4U);
    }
    return x;
}

static float neon_f32_finish(a32_ctx_t *ctx, float r)
{
    uint32_t w = 0U;
    memcpy(&w, &r, 4U);
    if ((w & 0x7F800000U) == 0U && (w & 0x007FFFFFU) != 0U) {
        ctx->fpscr |= (1U << 3);
        w &= 0x80000000U;
        memcpy(&r, &w, 4U);
        return r;
    }
    if ((w & 0x7F800000U) == 0x7F800000U && (w & 0x007FFFFFU) != 0U) {
        w = 0x7FC00000U;
        memcpy(&r, &w, 4U);
    }
    return r;
}

static uint32_t neon_f32_bits(float f)
{
    uint32_t w = 0U;
    memcpy(&w, &f, 4U);
    return w;
}

static float neon_bits_f32(uint32_t w)
{
    float f = 0.0f;
    memcpy(&f, &w, 4U);
    return f;
}

bool i_vfp_neon(a32_ctx_t *ctx, uint32_t pc, uint32_t insn, bool *faulted)
{
    if ((insn & 0xFF900000U) == 0xF4000000U) {
        bool l = ((insn >> 21) & 1U) != 0U;
        unsigned rn = (insn >> 16) & 0xFU;
        unsigned d_start = (((insn >> 22) & 1U) << 4) | ((insn >> 12) & 0xFU);
        unsigned type = (insn >> 8) & 0xFU;
        unsigned rm = insn & 0xFU;
        unsigned nregs = (type == 0x7U) ? 1U : (type == 0xAU) ? 2U :
                         (type == 0x6U) ? 3U : (type == 0x2U) ? 4U : 0U;
        if (nregs != 0U) {
            uint32_t addr = ctx->regs[rn];
            if (!i_mem_ok(ctx, addr, nregs * 8U, l ? (uint32_t)PROT_READ : (uint32_t)PROT_WRITE)) {
                *faulted = true;
                (void)i_fault(ctx, addr, !l);
                return true;
            }
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
    }
    if ((insn & 0xFEB80090U) == 0xF2800010U) {
        uint8_t imm8 = (uint8_t)((((insn >> 24) & 1U) << 7) |
                                 (((insn >> 16) & 7U) << 4) | (insn & 0xFU));
        uint8_t cmode = (uint8_t)((insn >> 8) & 0xFU);
        uint8_t op = (uint8_t)((insn >> 5) & 1U);
        bool is_q = ((insn >> 6) & 1U) != 0U;
        unsigned dd = (((insn >> 22) & 1U) << 4) | ((insn >> 12) & 0xFU);
        /* Full modified-immediate decode shared with the JIT (A32-R7-26):
         * even cmodes < 8 and 12+ overwrite (VMOV/VMVN, inverted when
         * op != 0 except the 0xE shifting-ones form), odd cmodes < 12
         * read-modify-write (VORR/VBIC), 0xF/op 0 is an F32 immediate.
         * Undecodable forms (0xF/op 1) are UNDEFINED, never zero. */
        uint64_t val64 = 0ULL;
        if (!neon_expand_imm(cmode, op, imm8, &val64)) {
            i_undef(ctx);
            return false;
        }
        if ((cmode & 1U) == 0U || cmode >= 12U) { /* VMOV or VMVN */
            if (op != 0U && cmode != 14U) {
                val64 = ~val64;
            }
            ctx->vfp_d[dd & 31U] = val64;
            if (is_q) {
                ctx->vfp_d[(dd + 1U) & 31U] = val64;
            }
        } else { /* VORR or VBIC */
            for (unsigned r = 0U; r < (is_q ? 2U : 1U); r++) {
                unsigned d = (dd + r) & 31U;
                if (op == 0U) {
                    ctx->vfp_d[d] |= val64;
                } else {
                    ctx->vfp_d[d] &= ~val64;
                }
            }
        }
        ctx->regs[15] = pc + 4U;
        return true;
    }
    if ((insn & 0xFFB00010U) == 0xF3B00000U) {
        unsigned size = (insn >> 18) & 3U;
        unsigned opc1 = (insn >> 16) & 3U;
        unsigned op = (insn >> 7) & 31U;
        if (opc1 != 0U) {
            goto neon_after_vrev;
        }
        bool is_q = ((insn >> 6) & 1U) != 0U;
        unsigned dd = (((insn >> 22) & 1U) << 4) | ((insn >> 12) & 0xFU);
        unsigned dm = (((insn >> 5) & 1U) << 4) | (insn & 0xFU);
        if (op > 2U || size > 2U || (op + size) >= 3U) {
            goto neon_after_vrev;
        }
        unsigned regs = is_q ? 2U : 1U;
        for (unsigned r = 0U; r < regs; r++) {
            uint64_t v = ctx->vfp_d[(dm + r) & 31U];
            uint64_t out = 0ULL;
            if (op == 0U) {
                if (size == 0U) {
                    out = __builtin_bswap64(v);
                } else if (size == 1U) {
                    uint64_t h0 = (v >> 48) & 0xFFFFU;
                    uint64_t h1 = (v >> 32) & 0xFFFFU;
                    uint64_t h2 = (v >> 16) & 0xFFFFU;
                    uint64_t h3 = v & 0xFFFFU;
                    out = (h3 << 48) | (h2 << 32) | (h1 << 16) | h0;
                } else {
                    out = (v >> 32) | (v << 32);
                }
            } else if (op == 1U) {
                if (size == 0U) {
                    for (unsigned c = 0U; c < 2U; c++) {
                        uint32_t w = (uint32_t)(v >> (c * 32U));
                        w = __builtin_bswap32(w);
                        out |= (uint64_t)w << (c * 32U);
                    }
                } else {
                    for (unsigned c = 0U; c < 2U; c++) {
                        uint32_t w = (uint32_t)(v >> (c * 32U));
                        uint16_t h0 = (uint16_t)(w >> 16);
                        uint16_t h1 = (uint16_t)w;
                        w = ((uint32_t)h1 << 16) | h0;
                        out |= (uint64_t)w << (c * 32U);
                    }
                }
            } else {
                for (unsigned c = 0U; c < 4U; c++) {
                    uint16_t h = (uint16_t)(v >> (c * 16U));
                    h = (uint16_t)(((h & 0xFFU) << 8) | ((h >> 8) & 0xFFU));
                    out |= (uint64_t)h << (c * 16U);
                }
            }
            ctx->vfp_d[(dd + r) & 31U] = out;
        }
        ctx->regs[15] = pc + 4U;
        return true;
    }
neon_after_vrev:;
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
    if ((insn & 0x0E000000U) == 0x02000000U && (insn & 0x00800000U) == 0U) {
        bool u_bit = ((insn >> 24) & 1U) != 0U;
        unsigned size = (insn >> 20) & 3U;
        unsigned op = (insn >> 8) & 0xFU;
        bool b4 = ((insn >> 4) & 1U) != 0U;
        bool is_q = ((insn >> 6) & 1U) != 0U;
        unsigned dd = neon_reg_d(insn);
        unsigned dn = neon_reg_n(insn);
        unsigned dm = neon_reg_m(insn);
        unsigned qd = dd >> 1;
        unsigned qn = dn >> 1;
        unsigned qm = dm >> 1;
        unsigned lanes = is_q ? 4U : 2U;
        unsigned dbase_d = is_q ? (qd * 2U) : dd;
        unsigned dbase_n = is_q ? (qn * 2U) : dn;
        unsigned dbase_m = is_q ? (qm * 2U) : dm;
        uint32_t saved_fpscr = ctx->fpscr;
        fenv_t saved_env;
        neon_fenv_begin(&saved_env);
        bool needs_acc = !u_bit && b4 && (op == 0xCU || op == 0xDU) && (size == 0U || size == 2U);
        float av[4];
        float bv[4];
        float dv[4];
        for (unsigned k = 0U; k < lanes; k++) {
            uint64_t vw;
            vw = ctx->vfp_d[(dbase_n + (k >> 1)) & 31U];
            uint32_t w = (uint32_t)(vw >> ((k & 1U) * 32U));
            float f = 0.0f;
            memcpy(&f, &w, 4U);
            av[k] = neon_f32_canon_in(ctx, f);
            vw = ctx->vfp_d[(dbase_m + (k >> 1)) & 31U];
            w = (uint32_t)(vw >> ((k & 1U) * 32U));
            memcpy(&f, &w, 4U);
            bv[k] = neon_f32_canon_in(ctx, f);
            if (needs_acc) {
                vw = ctx->vfp_d[(dbase_d + (k >> 1)) & 31U];
                w = (uint32_t)(vw >> ((k & 1U) * 32U));
                memcpy(&f, &w, 4U);
                dv[k] = neon_f32_canon_in(ctx, f);
            } else {
                dv[k] = 0.0f;
            }
        }
        float rv[4];
        uint32_t rm[4];
        bool is_cmp = false;
        bool handled_fp = true;
        if (!u_bit && size == 0U && op == 0xDU && !b4) {
            for (unsigned k = 0U; k < lanes; k++) {
                rv[k] = neon_f32_finish(ctx, av[k] + bv[k]);
            }
        } else if (!u_bit && size == 2U && op == 0xDU && !b4) {
            for (unsigned k = 0U; k < lanes; k++) {
                rv[k] = neon_f32_finish(ctx, av[k] - bv[k]);
            }
        } else if (u_bit && size == 2U && op == 0xDU && !b4) {
            for (unsigned k = 0U; k < lanes; k++) {
                uint32_t aw = neon_f32_bits(av[k]);
                uint32_t bw = neon_f32_bits(bv[k]);
                bool an = ((aw & 0x7F800000U) == 0x7F800000U) && ((aw & 0x007FFFFFU) != 0U);
                bool bn = ((bw & 0x7F800000U) == 0x7F800000U) && ((bw & 0x007FFFFFU) != 0U);
                if (an || bn) {
                    rv[k] = neon_bits_f32(0x7FC00000U);
                } else {
                    float d = av[k] - bv[k];
                    d = neon_f32_finish(ctx, d);
                    uint32_t dw = neon_f32_bits(d) & 0x7FFFFFFFU;
                    rv[k] = neon_bits_f32(dw);
                }
            }
        } else if (u_bit && size == 0U && op == 0xDU && b4) {
            for (unsigned k = 0U; k < lanes; k++) {
                rv[k] = neon_f32_finish(ctx, av[k] * bv[k]);
            }
        } else if (!u_bit && (size == 0U || size == 2U) && op == 0xDU && b4) {
            for (unsigned k = 0U; k < lanes; k++) {
                float p = neon_f32_finish(ctx, av[k] * bv[k]);
                float r = (size == 0U) ? (dv[k] + p) : (dv[k] - p);
                rv[k] = neon_f32_finish(ctx, r);
            }
        } else if (!u_bit && size == 0U && op == 0xCU && b4) {
            for (unsigned k = 0U; k < lanes; k++) {
                rv[k] = neon_f32_finish(ctx, fmaf(av[k], bv[k], dv[k]));
            }
        } else if (!u_bit && size == 2U && op == 0xCU && b4) {
            for (unsigned k = 0U; k < lanes; k++) {
                float na = neon_bits_f32(neon_f32_bits(av[k]) ^ 0x80000000U);
                rv[k] = neon_f32_finish(ctx, fmaf(na, bv[k], dv[k]));
            }
        } else if (!u_bit && size == 0U && op == 0xEU && !b4) {
            is_cmp = true;
            for (unsigned k = 0U; k < lanes; k++) {
                uint32_t aw = neon_f32_bits(av[k]);
                uint32_t bw = neon_f32_bits(bv[k]);
                bool an = ((aw & 0x7F800000U) == 0x7F800000U) && ((aw & 0x007FFFFFU) != 0U);
                bool bn = ((bw & 0x7F800000U) == 0x7F800000U) && ((bw & 0x007FFFFFU) != 0U);
                bool eq = !an && !bn && (av[k] == bv[k]);
                rm[k] = eq ? 0xFFFFFFFFU : 0U;
            }
        } else if (u_bit && size == 0U && op == 0xEU && !b4) {
            is_cmp = true;
            for (unsigned k = 0U; k < lanes; k++) {
                uint32_t aw = neon_f32_bits(av[k]);
                uint32_t bw = neon_f32_bits(bv[k]);
                bool an = ((aw & 0x7F800000U) == 0x7F800000U) && ((aw & 0x007FFFFFU) != 0U);
                bool bn = ((bw & 0x7F800000U) == 0x7F800000U) && ((bw & 0x007FFFFFU) != 0U);
                bool ge = !an && !bn && (av[k] >= bv[k]);
                rm[k] = ge ? 0xFFFFFFFFU : 0U;
            }
        } else if (u_bit && size == 2U && op == 0xEU && !b4) {
            is_cmp = true;
            for (unsigned k = 0U; k < lanes; k++) {
                uint32_t aw = neon_f32_bits(av[k]);
                uint32_t bw = neon_f32_bits(bv[k]);
                bool an = ((aw & 0x7F800000U) == 0x7F800000U) && ((aw & 0x007FFFFFU) != 0U);
                bool bn = ((bw & 0x7F800000U) == 0x7F800000U) && ((bw & 0x007FFFFFU) != 0U);
                bool gt = !an && !bn && (av[k] > bv[k]);
                rm[k] = gt ? 0xFFFFFFFFU : 0U;
            }
        } else if (u_bit && (size == 0U || size == 2U) && op == 0xEU && b4) {
            is_cmp = true;
            for (unsigned k = 0U; k < lanes; k++) {
                uint32_t aw = neon_f32_bits(av[k]) & 0x7FFFFFFFU;
                uint32_t bw = neon_f32_bits(bv[k]) & 0x7FFFFFFFU;
                float aa = neon_bits_f32(aw);
                float bb = neon_bits_f32(bw);
                bool an = ((aw & 0x7F800000U) == 0x7F800000U) && ((aw & 0x007FFFFFU) != 0U);
                bool bn = ((bw & 0x7F800000U) == 0x7F800000U) && ((bw & 0x007FFFFFU) != 0U);
                bool r = !an && !bn;
                if (r) {
                    r = (size == 0U) ? (aa >= bb) : (aa > bb);
                }
                rm[k] = r ? 0xFFFFFFFFU : 0U;
            }
        } else if (!u_bit && size == 0U && op == 0xFU && !b4) {
            for (unsigned k = 0U; k < lanes; k++) {
                uint32_t aw = neon_f32_bits(av[k]);
                uint32_t bw = neon_f32_bits(bv[k]);
                bool an = ((aw & 0x7F800000U) == 0x7F800000U) && ((aw & 0x007FFFFFU) != 0U);
                bool bn = ((bw & 0x7F800000U) == 0x7F800000U) && ((bw & 0x007FFFFFU) != 0U);
                if (an || bn) {
                    rv[k] = neon_bits_f32(0x7FC00000U);
                } else if (av[k] == bv[k]) {
                    uint32_t ab = aw & 0x80000000U;
                    uint32_t bb = bw & 0x80000000U;
                    rv[k] = neon_bits_f32((ab == 0U || bb == 0U) ? 0U : 0x80000000U);
                    if (aw == bw) {
                        rv[k] = av[k];
                    } else {
                        rv[k] = neon_bits_f32(0U);
                    }
                } else {
                    rv[k] = (av[k] > bv[k]) ? av[k] : bv[k];
                }
            }
        } else if (!u_bit && size == 2U && op == 0xFU && !b4) {
            for (unsigned k = 0U; k < lanes; k++) {
                uint32_t aw = neon_f32_bits(av[k]);
                uint32_t bw = neon_f32_bits(bv[k]);
                bool an = ((aw & 0x7F800000U) == 0x7F800000U) && ((aw & 0x007FFFFFU) != 0U);
                bool bn = ((bw & 0x7F800000U) == 0x7F800000U) && ((bw & 0x007FFFFFU) != 0U);
                if (an || bn) {
                    rv[k] = neon_bits_f32(0x7FC00000U);
                } else if (av[k] == bv[k]) {
                    if (aw == bw) {
                        rv[k] = av[k];
                    } else {
                        rv[k] = neon_bits_f32(0x80000000U);
                    }
                } else {
                    rv[k] = (av[k] < bv[k]) ? av[k] : bv[k];
                }
            }
        } else if (!u_bit && size == 0U && op == 0xFU && b4) {
            for (unsigned k = 0U; k < lanes; k++) {
                float p = ((isinf(av[k]) && bv[k] == 0.0f) || (av[k] == 0.0f && isinf(bv[k])))
                              ? 0.0f
                              : neon_f32_finish(ctx, av[k] * bv[k]);
                rv[k] = neon_f32_finish(ctx, 2.0f - p);
            }
        } else if (!u_bit && size == 2U && op == 0xFU && b4) {
            for (unsigned k = 0U; k < lanes; k++) {
                float p = ((isinf(av[k]) && bv[k] == 0.0f) || (av[k] == 0.0f && isinf(bv[k])))
                              ? 0.0f
                              : neon_f32_finish(ctx, av[k] * bv[k]);
                rv[k] = neon_f32_finish(ctx, (3.0f - p) * 0.5f);
            }
        } else if (u_bit && !is_q && size == 0U && op == 0xFU && !b4) {
            float p0 = av[0] > av[1] ? av[0] : av[1];
            float p1 = bv[0] > bv[1] ? bv[0] : bv[1];
            uint32_t a0w = neon_f32_bits(av[0]);
            uint32_t a1w = neon_f32_bits(av[1]);
            uint32_t b0w = neon_f32_bits(bv[0]);
            uint32_t b1w = neon_f32_bits(bv[1]);
            bool a0n = ((a0w & 0x7F800000U) == 0x7F800000U) && ((a0w & 0x007FFFFFU) != 0U);
            bool a1n = ((a1w & 0x7F800000U) == 0x7F800000U) && ((a1w & 0x007FFFFFU) != 0U);
            bool b0n = ((b0w & 0x7F800000U) == 0x7F800000U) && ((b0w & 0x007FFFFFU) != 0U);
            bool b1n = ((b1w & 0x7F800000U) == 0x7F800000U) && ((b1w & 0x007FFFFFU) != 0U);
            if (a0n || a1n) {
                p0 = neon_bits_f32(0x7FC00000U);
            } else if (av[0] == av[1]) {
                p0 = (a0w == a1w) ? av[0] : neon_bits_f32(0U);
            }
            if (b0n || b1n) {
                p1 = neon_bits_f32(0x7FC00000U);
            } else if (bv[0] == bv[1]) {
                p1 = (b0w == b1w) ? bv[0] : neon_bits_f32(0U);
            }
            rv[0] = p0;
            rv[1] = p1;
        } else if (u_bit && !is_q && size == 2U && op == 0xFU && !b4) {
            float p0 = av[0] < av[1] ? av[0] : av[1];
            float p1 = bv[0] < bv[1] ? bv[0] : bv[1];
            uint32_t a0w = neon_f32_bits(av[0]);
            uint32_t a1w = neon_f32_bits(av[1]);
            uint32_t b0w = neon_f32_bits(bv[0]);
            uint32_t b1w = neon_f32_bits(bv[1]);
            bool a0n = ((a0w & 0x7F800000U) == 0x7F800000U) && ((a0w & 0x007FFFFFU) != 0U);
            bool a1n = ((a1w & 0x7F800000U) == 0x7F800000U) && ((a1w & 0x007FFFFFU) != 0U);
            bool b0n = ((b0w & 0x7F800000U) == 0x7F800000U) && ((b0w & 0x007FFFFFU) != 0U);
            bool b1n = ((b1w & 0x7F800000U) == 0x7F800000U) && ((b1w & 0x007FFFFFU) != 0U);
            if (a0n || a1n) {
                p0 = neon_bits_f32(0x7FC00000U);
            } else if (av[0] == av[1]) {
                p0 = (a0w == a1w) ? av[0] : neon_bits_f32(0x80000000U);
            }
            if (b0n || b1n) {
                p1 = neon_bits_f32(0x7FC00000U);
            } else if (bv[0] == bv[1]) {
                p1 = (b0w == b1w) ? bv[0] : neon_bits_f32(0x80000000U);
            }
            rv[0] = p0;
            rv[1] = p1;
        } else if (u_bit && (size == 0U || size == 2U) && op == 0xFU && b4) {
            for (unsigned k = 0U; k < lanes; k++) {
                uint32_t aw = neon_f32_bits(av[k]);
                uint32_t bw = neon_f32_bits(bv[k]);
                bool an = ((aw & 0x7F800000U) == 0x7F800000U) && ((aw & 0x007FFFFFU) != 0U);
                bool bn = ((bw & 0x7F800000U) == 0x7F800000U) && ((bw & 0x007FFFFFU) != 0U);
                float r;
                if (an && bn) {
                    r = neon_bits_f32(0x7FC00000U);
                } else if (an) {
                    r = bv[k];
                } else if (bn) {
                    r = av[k];
                } else if (av[k] == bv[k]) {
                    if (aw == bw) {
                        r = av[k];
                    } else {
                        r = (size == 0U) ? neon_bits_f32(0U) : neon_bits_f32(0x80000000U);
                    }
                } else {
                    bool gt = av[k] > bv[k];
                    if (size == 0U) {
                        r = gt ? av[k] : bv[k];
                    } else {
                        r = gt ? bv[k] : av[k];
                    }
                }
                rv[k] = r;
            }
        } else if (u_bit && !is_q && size == 0U && op == 0xDU && !b4) {
            float s0 = neon_f32_finish(ctx, av[0] + av[1]);
            float s1 = neon_f32_finish(ctx, bv[0] + bv[1]);
            rv[0] = s0;
            rv[1] = s1;
        } else {
            handled_fp = false;
        }
        if (handled_fp) {
            neon_fenv_end(ctx, &saved_env);
            if (is_cmp) {
                for (unsigned k = 0U; k < lanes; k++) {
                    unsigned d = (dbase_d + (k >> 1)) & 31U;
                    uint64_t cur = ctx->vfp_d[d];
                    uint32_t w = rm[k];
                    if ((k & 1U) == 0U) {
                        cur = (cur & 0xFFFFFFFF00000000ULL) | w;
                    } else {
                        cur = (cur & 0xFFFFFFFFULL) | ((uint64_t)w << 32);
                    }
                    ctx->vfp_d[d] = cur;
                }
            } else {
                unsigned out_lanes = (u_bit && !is_q && (op == 0xFU || (op == 0xDU && size == 0U))) ? 2U : lanes;
                if (u_bit && !is_q && size == 0U && op == 0xDU && !b4) {
                    out_lanes = 2U;
                }
                for (unsigned k = 0U; k < out_lanes && k < lanes; k++) {
                    unsigned d = (dbase_d + (k >> 1)) & 31U;
                    uint64_t cur = ctx->vfp_d[d];
                    uint32_t w = neon_f32_bits(rv[k]);
                    if ((k & 1U) == 0U) {
                        cur = (cur & 0xFFFFFFFF00000000ULL) | w;
                    } else {
                        cur = (cur & 0xFFFFFFFFULL) | ((uint64_t)w << 32);
                    }
                    ctx->vfp_d[d] = cur;
                }
            }
            ctx->regs[15] = pc + 4U;
            return true;
        }
        ctx->fpscr = saved_fpscr;
        (void)fesetenv(&saved_env);
    }
    if ((insn & 0xFFB00010U) == 0xF3B00000U) {
        unsigned opc1 = (insn >> 16) & 3U;
        unsigned size = (insn >> 18) & 3U;
        unsigned op5 = (insn >> 7) & 31U;
        bool is_q = ((insn >> 6) & 1U) != 0U;
        unsigned dd = neon_reg_d(insn);
        unsigned dm = neon_reg_m(insn);
        unsigned qd = dd >> 1;
        unsigned qm = dm >> 1;
        unsigned lanes = is_q ? 4U : 2U;
        unsigned db_d = is_q ? (qd * 2U) : dd;
        unsigned db_m = is_q ? (qm * 2U) : dm;
        if (opc1 == 1U && size == 2U && op5 >= 8U && op5 <= 12U) {
            fenv_t saved_env;
            neon_fenv_begin(&saved_env);
            for (unsigned k = 0U; k < lanes; k++) {
                uint64_t vw = ctx->vfp_d[(db_m + (k >> 1)) & 31U];
                uint32_t w = (uint32_t)(vw >> ((k & 1U) * 32U));
                float f = 0.0f;
                memcpy(&f, &w, 4U);
                f = neon_f32_canon_in(ctx, f);
                uint32_t fw = neon_f32_bits(f);
                bool isn = ((fw & 0x7F800000U) == 0x7F800000U) && ((fw & 0x007FFFFFU) != 0U);
                bool r = false;
                if (isn) {
                    if (op5 != 10U || (fw & 0x00400000U) == 0U) {
                        ctx->fpscr |= 1U;
                    }
                } else {
                    if (op5 == 8U) {
                        r = f > 0.0f;
                    } else if (op5 == 9U) {
                        r = f >= 0.0f;
                    } else if (op5 == 10U) {
                        r = f == 0.0f;
                    } else if (op5 == 11U) {
                        r = f <= 0.0f;
                    } else {
                        r = f < 0.0f;
                    }
                }
                uint32_t m = r ? 0xFFFFFFFFU : 0U;
                uint64_t cur = ctx->vfp_d[(db_d + (k >> 1)) & 31U];
                if ((k & 1U) == 0U) {
                    cur = (cur & 0xFFFFFFFF00000000ULL) | m;
                } else {
                    cur = (cur & 0xFFFFFFFFULL) | ((uint64_t)m << 32);
                }
                ctx->vfp_d[(db_d + (k >> 1)) & 31U] = cur;
            }
            neon_fenv_end(ctx, &saved_env);
            ctx->regs[15] = pc + 4U;
            return true;
        }
        if (opc1 == 1U && size == 2U && (op5 == 14U || op5 == 15U)) {
            for (unsigned k = 0U; k < lanes; k++) {
                uint64_t vw = ctx->vfp_d[(db_m + (k >> 1)) & 31U];
                uint32_t w = (uint32_t)(vw >> ((k & 1U) * 32U));
                uint32_t rw = (op5 == 14U) ? (w & 0x7FFFFFFFU) : (w ^ 0x80000000U);
                uint64_t cur = ctx->vfp_d[(db_d + (k >> 1)) & 31U];
                if ((k & 1U) == 0U) {
                    cur = (cur & 0xFFFFFFFF00000000ULL) | rw;
                } else {
                    cur = (cur & 0xFFFFFFFFULL) | ((uint64_t)rw << 32);
                }
                ctx->vfp_d[(db_d + (k >> 1)) & 31U] = cur;
            }
            ctx->regs[15] = pc + 4U;
            return true;
        }
        if (opc1 == 3U && size == 2U && op5 >= 8U && op5 <= 15U) {
            fenv_t saved_env;
            if (op5 >= 10U) {
                neon_fenv_begin(&saved_env);
            }
            for (unsigned k = 0U; k < lanes; k++) {
                uint64_t vw = ctx->vfp_d[(db_m + (k >> 1)) & 31U];
                uint32_t w = (uint32_t)(vw >> ((k & 1U) * 32U));
                if (op5 == 8U || op5 == 9U) {
                    uint32_t rw;
#if defined(__aarch64__)
                    rw = (op5 == 8U) ? vget_lane_u32(vrecpe_u32(vdup_n_u32(w)), 0)
                                     : vget_lane_u32(vrsqrte_u32(vdup_n_u32(w)), 0);
#else
                    rw = (w < 0x80000000U) ? 0xFFFFFFFFU : 0x80000000U;
#endif
                    uint64_t cur = ctx->vfp_d[(db_d + (k >> 1)) & 31U];
                    if ((k & 1U) == 0U) {
                        cur = (cur & 0xFFFFFFFF00000000ULL) | rw;
                    } else {
                        cur = (cur & 0xFFFFFFFFULL) | ((uint64_t)rw << 32);
                    }
                    ctx->vfp_d[(db_d + (k >> 1)) & 31U] = cur;
                } else if (op5 == 10U || op5 == 11U) {
                    float f = 0.0f;
                    memcpy(&f, &w, 4U);
                    f = neon_f32_canon_in(ctx, f);
                    float r;
#if defined(__aarch64__)
                    r = (op5 == 10U) ? vget_lane_f32(vrecpe_f32(vdup_n_f32(f)), 0)
                                     : vget_lane_f32(vrsqrte_f32(vdup_n_f32(f)), 0);
#else
                    r = (op5 == 10U) ? (1.0f / f) : (1.0f / (float)sqrt((double)f));
#endif
                    r = neon_f32_finish(ctx, r);
                    uint32_t rw = neon_f32_bits(r);
                    uint64_t cur = ctx->vfp_d[(db_d + (k >> 1)) & 31U];
                    if ((k & 1U) == 0U) {
                        cur = (cur & 0xFFFFFFFF00000000ULL) | rw;
                    } else {
                        cur = (cur & 0xFFFFFFFFULL) | ((uint64_t)rw << 32);
                    }
                    ctx->vfp_d[(db_d + (k >> 1)) & 31U] = cur;
                } else if (op5 == 12U || op5 == 13U) {
                    int32_t sv = (int32_t)w;
                    uint32_t uv = w;
                    float r = (op5 == 12U) ? (float)sv : (float)uv;
                    r = neon_f32_finish(ctx, r);
                    uint32_t rw = neon_f32_bits(r);
                    uint64_t cur = ctx->vfp_d[(db_d + (k >> 1)) & 31U];
                    if ((k & 1U) == 0U) {
                        cur = (cur & 0xFFFFFFFF00000000ULL) | rw;
                    } else {
                        cur = (cur & 0xFFFFFFFFULL) | ((uint64_t)rw << 32);
                    }
                    ctx->vfp_d[(db_d + (k >> 1)) & 31U] = cur;
                } else {
                    float f = 0.0f;
                    memcpy(&f, &w, 4U);
                    f = neon_f32_canon_in(ctx, f);
                    uint32_t fw = neon_f32_bits(f);
                    bool isn = ((fw & 0x7F800000U) == 0x7F800000U) && ((fw & 0x007FFFFFU) != 0U);
                    uint32_t r;
                    if (isn) {
                        ctx->fpscr |= 1U;
                        r = 0U;
                    } else if (op5 == 14U) {
                        double df = (double)f;
                        double tr = trunc(df);
                        if (tr > 2147483647.0) {
                            ctx->fpscr |= 1U;
                            r = 0x7FFFFFFFU;
                        } else if (tr < -2147483648.0) {
                            ctx->fpscr |= 1U;
                            r = 0x80000000U;
                        } else {
                            if (tr != df) {
                                ctx->fpscr |= (1U << 4);
                            }
                            r = (uint32_t)(int32_t)tr;
                        }
                    } else {
                        double df = (double)f;
                        double tr = trunc(df);
                        if (df < 0.0 && tr <= -1.0) {
                            ctx->fpscr |= 1U;
                            r = 0U;
                        } else if (df < 0.0) {
                            ctx->fpscr |= (1U << 4);
                            r = 0U;
                        } else if (tr >= 4294967296.0) {
                            ctx->fpscr |= 1U;
                            r = 0xFFFFFFFFU;
                        } else {
                            if (tr != df) {
                                ctx->fpscr |= (1U << 4);
                            }
                            r = (uint32_t)tr;
                        }
                    }
                    uint64_t cur = ctx->vfp_d[(db_d + (k >> 1)) & 31U];
                    if ((k & 1U) == 0U) {
                        cur = (cur & 0xFFFFFFFF00000000ULL) | r;
                    } else {
                        cur = (cur & 0xFFFFFFFFULL) | ((uint64_t)r << 32);
                    }
                    ctx->vfp_d[(db_d + (k >> 1)) & 31U] = cur;
                }
            }
            if (op5 >= 10U) {
                neon_fenv_end(ctx, &saved_env);
            }
            ctx->regs[15] = pc + 4U;
            return true;
        }
    }
    if ((insn & 0x0E000000U) == 0x02000000U && (insn & 0x00800000U) != 0U) {
        bool is_q = ((insn >> 6) & 1U) != 0U;
        unsigned size = (insn >> 20) & 3U;
        unsigned op = (insn >> 8) & 0xFU;
        bool b4 = ((insn >> 4) & 1U) != 0U;
        if (is_q && !b4 && (size == 1U || size == 2U)) {
            bool vec_q = ((insn >> 24) & 1U) != 0U;
            if ((op == 0x9U && size == 2U) ||
                ((op == 0x1U || op == 0x5U) && size == 2U)) {
                fenv_t saved_env;
                neon_fenv_begin(&saved_env);
                unsigned dm_s = insn & 0xFU;
                unsigned lane = (insn >> 5) & 1U;
                uint64_t sv = ctx->vfp_d[dm_s & 31U];
                uint32_t sw = (lane == 0U) ? (uint32_t)sv : (uint32_t)(sv >> 32);
                float sc = 0.0f;
                memcpy(&sc, &sw, 4U);
                sc = neon_f32_canon_in(ctx, sc);
                unsigned dd = neon_reg_d(insn);
                unsigned dn = neon_reg_n(insn);
                unsigned qd = dd >> 1;
                unsigned qn = dn >> 1;
                unsigned lanes = vec_q ? 4U : 2U;
                unsigned bd_d = vec_q ? (qd * 2U) : dd;
                unsigned bd_n = vec_q ? (qn * 2U) : dn;
                for (unsigned k = 0U; k < lanes; k++) {
                    uint64_t vw = ctx->vfp_d[(bd_n + (k >> 1)) & 31U];
                    uint32_t w = (uint32_t)(vw >> ((k & 1U) * 32U));
                    float a = 0.0f;
                    memcpy(&a, &w, 4U);
                    a = neon_f32_canon_in(ctx, a);
                    float r;
                    if (op == 0x9U) {
                        r = neon_f32_finish(ctx, a * sc);
                    } else {
                        uint64_t vd = ctx->vfp_d[(bd_d + (k >> 1)) & 31U];
                        uint32_t dw = (uint32_t)(vd >> ((k & 1U) * 32U));
                        float d = 0.0f;
                        memcpy(&d, &dw, 4U);
                        d = neon_f32_canon_in(ctx, d);
                        float p = neon_f32_finish(ctx, a * sc);
                        r = (op == 0x1U) ? neon_f32_finish(ctx, d + p) :
                            neon_f32_finish(ctx, d - p);
                    }
                    uint32_t rw = neon_f32_bits(r);
                    uint64_t cur = ctx->vfp_d[(bd_d + (k >> 1)) & 31U];
                    if ((k & 1U) == 0U) {
                        cur = (cur & 0xFFFFFFFF00000000ULL) | rw;
                    } else {
                        cur = (cur & 0xFFFFFFFFULL) | ((uint64_t)rw << 32);
                    }
                    ctx->vfp_d[(bd_d + (k >> 1)) & 31U] = cur;
                }
                neon_fenv_end(ctx, &saved_env);
                ctx->regs[15] = pc + 4U;
                return true;
            }
        }
    }
    if (i_vfp_neon_ext(ctx, pc, insn, faulted)) {
        return true;
    }
    return false;
}
