/*
 * src/interp/interp_vfp_neon_shift.c — NEON shift-by-immediate interpreter
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Two-register Advanced SIMD shifts by immediate (guard 0xF2800010 masked
 * with 0xFE800010): VMOVL/VSHLL, VSHRN/VRSHRN/VQSHRN/VQRSHRN narrowing, and
 * VSHR/VSRA/VRSHR/VRSRA/VSRI/VSHL/VSLI/VQSHLU/VQSHL. Saturating ops set
 * FPSCR.QC (bit 27). Op 0xE/0xF stays in interp_vfp_neon_ext.c.
 */
#include "interp.h"
#include "interp_priv.h"
/* FPSCR.QC: cumulative saturation flag set by VQSHL*, VQSHLU and VQSHRN*. */
#define NI_FPSCR_QC (1U << 27)
static uint64_t sh_mask(unsigned b)
{
    return (b >= 64U) ? ~0ULL : ((1ULL << b) - 1ULL);
}
static uint64_t sh_get(uint64_t d, unsigned lane, unsigned bits)
{
    return (d >> (lane * bits)) & sh_mask(bits);
}
static uint64_t sh_put(uint64_t d, unsigned lane, unsigned bits, uint64_t v)
{
    uint64_t m = sh_mask(bits);
    unsigned sh = lane * bits;
    return (d & ~(m << sh)) | ((v & m) << sh);
}
static int64_t sh_sext(uint64_t v, unsigned bits)
{
    uint64_t m;
    if (bits >= 64U) {
        return (int64_t)v;
    }
    m = 1ULL << (bits - 1U);
    return (int64_t)((v ^ m) - m);
}
static int64_t sh_asr(int64_t v, unsigned rsh) /* exact arith >> by 0..64 */
{
    uint64_t res;
    if (rsh >= 64U) {
        return (v < 0) ? -1LL : 0LL;
    }
    if (rsh == 0U) {
        return v;
    }
    res = (uint64_t)v >> rsh;
    if (v < 0) {
        res |= ~((~0ULL) >> rsh);
    }
    return (int64_t)res;
}
static uint64_t sh_rshr_u(uint64_t x, unsigned rsh) /* (x+2^(r-1))>>r, 1..64 */
{
    uint64_t base = (rsh >= 64U) ? 0ULL : (x >> rsh);
    return base + ((x >> (rsh - 1U)) & 1ULL);
}
static int64_t sh_rshr_s(int64_t v, unsigned rsh) /* signed form, exact */
{
    uint64_t base = (uint64_t)sh_asr(v, rsh);
    return (int64_t)(base + ((((uint64_t)v) >> (rsh - 1U)) & 1ULL));
}
static bool sh_bits(unsigned imm6, unsigned *bits) /* imm6<5:3> -> width */
{
    if ((imm6 & 0x38U) == 0x08U) {
        *bits = 8U;
        return true;
    }
    if ((imm6 & 0x30U) == 0x10U) {
        *bits = 16U;
        return true;
    }
    if ((imm6 & 0x20U) == 0x20U) {
        *bits = 32U;
        return true;
    }
    return false;
}
static uint64_t sh_sat_s(int64_t v, unsigned bits, uint32_t *fpscr)
{
    int64_t hi;
    int64_t lo;
    if (bits >= 64U) {
        return (uint64_t)v;
    }
    hi = (int64_t)((1ULL << (bits - 1U)) - 1ULL);
    lo = -hi - 1LL;
    if (v < lo) {
        *fpscr |= NI_FPSCR_QC;
        return (uint64_t)lo;
    }
    if (v > hi) {
        *fpscr |= NI_FPSCR_QC;
        return (uint64_t)hi;
    }
    return (uint64_t)v;
}
static uint64_t sh_sat_uu(uint64_t v, unsigned bits, uint32_t *fpscr)
{
    uint64_t hi = sh_mask(bits);
    if (v > hi) {
        *fpscr |= NI_FPSCR_QC;
        return hi;
    }
    return v;
}
bool i_vfp_neon_shift(a32_ctx_t *ctx, uint32_t pc, uint32_t insn)
{
    unsigned sh_op = (insn >> 8) & 0xFU;
    unsigned imm6 = (insn >> 16) & 0x3FU;
    unsigned l_sh = (insn >> 7) & 1U;
    bool is_q = ((insn >> 6) & 1U) != 0U;
    bool is_u = ((insn >> 24) & 1U) != 0U;
    unsigned dd = (((insn >> 22) & 1U) << 4) | ((insn >> 12) & 0xFU);
    unsigned dm = (((insn >> 5) & 1U) << 4) | (insn & 0xFU);
    if ((insn & 0xFE800010U) != 0xF2800010U) {
        return false;
    }
    if (sh_op == 0xAU) {
        /* VMOVL / VSHLL.<S/U><8/16/32> Qd, Dm, #shift: widen and shift. */
        unsigned bits, sh, lanes, i;
        uint64_t m, lo = 0ULL, hi = 0ULL, wide;
        if ((l_sh != 0U) || is_q || ((dd & 1U) != 0U)) {
            return false;
        }
        if (!sh_bits(imm6, &bits)) {
            return false;
        }
        sh = imm6 - bits;
        m = ctx->vfp_d[dm & 31U];
        lanes = 64U / bits;
        wide = sh_mask(2U * bits);
        for (i = 0U; i < lanes; i++) {
            uint64_t lane = sh_get(m, i, bits);
            uint64_t ext = is_u ? lane : (uint64_t)sh_sext(lane, bits);
            ext = (ext << sh) & wide;
            if (i < (lanes / 2U)) {
                lo |= ext << (i * 2U * bits);
            } else {
                hi |= ext << ((i - (lanes / 2U)) * 2U * bits);
            }
        }
        ctx->vfp_d[dd & 31U] = lo;
        ctx->vfp_d[(dd + 1U) & 31U] = hi;
        ctx->regs[15] = pc + 4U;
        return true;
    }
    if ((sh_op == 0x8U || sh_op == 0x9U) && (l_sh == 0U) && (imm6 >= 8U)) {
        /* VSHRN/VRSHRN, VQSHRUN/VQRSHRUN, VQSHRN.S/U: Dd, Qm, #rsh. */
        unsigned sbits, dst_bits, src_bits, rsh, lanes, half, i;
        uint64_t qlo, qhi, out = 0ULL;
        if (((dm & 1U) != 0U) || !sh_bits(imm6, &sbits)) {
            return false;
        }
        dst_bits = sbits;
        src_bits = 2U * sbits;
        rsh = 2U * sbits - imm6;
        qlo = ctx->vfp_d[dm & 31U];
        qhi = ctx->vfp_d[(dm + 1U) & 31U];
        lanes = 64U / dst_bits;
        half = lanes / 2U;
        for (i = 0U; i < lanes; i++) {
            uint64_t raw = sh_get((i < half) ? qlo : qhi, i % half, src_bits);
            int64_t sv = sh_sext(raw, src_bits);
            int64_t rv = is_q ? sh_rshr_s(sv, rsh) : sh_asr(sv, rsh);
            uint64_t trunc;
            if ((sh_op == 0x8U) && !is_u) {
                trunc = ((uint64_t)rv) & sh_mask(dst_bits);
            } else if (sh_op == 0x8U) { /* VQSHRUN: signed -> unsigned */
                uint64_t uv = (rv < 0) ? 0ULL : (uint64_t)rv;
                if (rv < 0) {
                    ctx->fpscr |= NI_FPSCR_QC;
                }
                trunc = sh_sat_uu(uv, dst_bits, &ctx->fpscr);
            } else if (!is_u) {
                trunc = sh_sat_s(rv, dst_bits, &ctx->fpscr);
            } else {
                uint64_t uv = is_q ? sh_rshr_u(raw, rsh) : (raw >> rsh);
                trunc = sh_sat_uu(uv, dst_bits, &ctx->fpscr);
            }
            out = sh_put(out, i, dst_bits, trunc);
        }
        ctx->vfp_d[dd & 31U] = out;
        ctx->regs[15] = pc + 4U;
        return true;
    }
    if ((sh_op <= 0x7U) && ((l_sh != 0U) || (imm6 >= 8U))) {
        unsigned bits, lsh, rsh, nregs, k;
        uint64_t srcs[2];
        uint64_t dsts[2];
        if (((sh_op == 0x4U) || (sh_op == 0x6U)) && !is_u) {
            return false;
        }
        if (is_q && (((dd & 1U) != 0U) || ((dm & 1U) != 0U))) {
            return false;
        }
        if (l_sh != 0U) {
            bits = 64U;
            lsh = imm6;
            rsh = 64U - imm6;
        } else {
            if (!sh_bits(imm6, &bits)) {
                return false;
            }
            lsh = imm6 - bits;
            rsh = 2U * bits - imm6;
        }
        /* Snapshot: dest Q may overlap source Q (parallel update). */
        nregs = is_q ? 2U : 1U;
        srcs[0] = ctx->vfp_d[dm & 31U];
        dsts[0] = ctx->vfp_d[dd & 31U];
        srcs[1] = ctx->vfp_d[(dm + 1U) & 31U];
        dsts[1] = ctx->vfp_d[(dd + 1U) & 31U];
        for (k = 0U; k < nregs; k++) {
            uint64_t dst = dsts[k];
            uint64_t src = srcs[k];
            uint64_t out = 0ULL;
            uint64_t emask = sh_mask(bits);
            unsigned lanes = 64U / bits;
            unsigned i;
            for (i = 0U; i < lanes; i++) {
                uint64_t dl = sh_get(dst, i, bits);
                uint64_t sl = sh_get(src, i, bits);
                int64_t ss = sh_sext(sl, bits);
                uint64_t rl = 0ULL;
                switch (sh_op) {
                case 0x0U: /* VSHR */
                    rl = is_u ? ((rsh >= 64U) ? 0ULL : (sl >> rsh)) :
                                ((uint64_t)sh_asr(ss, rsh));
                    break;
                case 0x1U: /* VSRA: Dd += VSHR(Dm) mod 2^bits */
                    rl = (dl + (is_u ? ((rsh >= 64U) ? 0ULL : (sl >> rsh)) :
                                ((uint64_t)sh_asr(ss, rsh)))) & emask;
                    break;
                case 0x2U: /* VRSHR: rounded, carry-safe for 64-bit lanes */
                    rl = (is_u ? sh_rshr_u(sl, rsh) :
                          (uint64_t)sh_rshr_s(ss, rsh)) & emask;
                    break;
                case 0x3U: /* VRSRA: Dd += VRSHR(Dm) mod 2^bits */
                    rl = (dl + (is_u ? sh_rshr_u(sl, rsh) :
                                ((uint64_t)sh_rshr_s(ss, rsh)))) & emask;
                    break;
                case 0x4U: { /* VSRI: insert shifted bits */
                    uint64_t mask = (rsh >= bits) ? 0ULL : (emask >> rsh);
                    uint64_t shifted = (rsh >= 64U) ? 0ULL : (sl >> rsh);
                    rl = (dl & ~mask) | (shifted & mask);
                    break;
                }
                case 0x5U: /* VSHL (U==0) / VSLI (U==1) */
                    if (!is_u) {
                        rl = (sl << lsh) & emask;
                    } else {
                        uint64_t mask = (emask << lsh) & emask;
                        rl = (dl & ~mask) | ((sl << lsh) & mask);
                    }
                    break;
                case 0x6U: { /* VQSHLU: signed -> unsigned saturate */
                    uint64_t t = (uint64_t)ss << lsh;
                    if ((ss < 0) || ((t >> lsh) != (uint64_t)ss)) {
                        ctx->fpscr |= NI_FPSCR_QC;
                        rl = (ss < 0) ? 0ULL : emask;
                    } else {
                        rl = sh_sat_uu(t, bits, &ctx->fpscr);
                    }
                    break;
                }
                default: { /* 0x7U VQSHL imm: saturating left shift */
                    if (!is_u) {
                        int64_t v = (int64_t)((uint64_t)ss << lsh);
                        int64_t hi = (bits >= 64U) ? INT64_MAX :
                            ((int64_t)((1ULL << (bits - 1U)) - 1ULL));
                        if (sh_asr(v, lsh) != ss) {
                            ctx->fpscr |= NI_FPSCR_QC;
                            rl = (uint64_t)((ss < 0) ? -hi - 1LL : hi);
                        } else {
                            rl = sh_sat_s(v, bits, &ctx->fpscr);
                        }
                    } else if ((((sl << lsh) >> lsh) != sl)) {
                        ctx->fpscr |= NI_FPSCR_QC;
                        rl = emask;
                    } else {
                        rl = sh_sat_uu(sl << lsh, bits, &ctx->fpscr);
                    }
                    break;
                }
                }
                out = sh_put(out, i, bits, rl);
            }
            ctx->vfp_d[(dd + k) & 31U] = out;
        }
        ctx->regs[15] = pc + 4U;
        return true;
    }
    return false;
}
