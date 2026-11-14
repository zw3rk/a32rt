/*
 * src/interp/interp_vfp_neon_int.c — NEON integer shifts, long/wide/narrow, VEXT & VDUP(core)
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Reference interpreter for the NEON integer datapath claimed by
 * a32_decode_neon_alu (src/interp/interp_neon_alu.c) and the shift-imm /
 * narrowing lowerings in interp_neon_mem.c. Handles:
 *   Part A: VDUP from ARM core register, VEXT.8, narrowing (VMOVN,
 *           VQMOVUN, VQMOVN) + VSHLL maximum shift, 3-register same-length
 *           integer ALU (VHADD/VRHADD/VHSUB/VQSUB/VCGT/VCGE/VCEQ/VTST,
 *           VSHL/VQSHL register, VMAX/VMIN/VABD/VABA/VPADD/VPMAX/VPMIN).
 *   Part B: NEON shifts by immediate, 3-register different-length and
 *           scalar integer multiply (VADDL/VADDW/VSUBL/VSUBW/VABAL/VABDL/
 *           VMLAL/VMLSL/VMULL, scalar VMUL/VMLA/VMLS/VMULL/VMLAL/VMLSL).
 * Lane indexing follows the (dd & ~1U) Q-pair convention of
 * interp_vfp_neon_ext.c; saturating ops set FPSCR.QC (bit 27).
 */

#include "interp.h"
#include "interp_priv.h"
#include "interp/decode_priv.h"

/* FPSCR.QC: cumulative saturation flag set by VQ*, VQMOV*, VQSHL*, VQSHRN*. */
#define NI_FPSCR_QC (1U << 27)

static unsigned ni_per(unsigned esize)
{
    if (esize == 0U) {
        return 8U;
    }
    if (esize == 1U) {
        return 4U;
    }
    if (esize == 2U) {
        return 2U;
    }
    return 1U;
}

static uint64_t ni_emask(unsigned bits)
{
    if (bits >= 64U) {
        return ~0ULL;
    }
    return (1ULL << bits) - 1ULL;
}

static uint64_t ni_get(uint64_t v, unsigned lane, unsigned esize)
{
    unsigned sh = lane * (8U << esize);
    return (v >> sh) & ni_emask(8U << esize);
}

static uint64_t ni_put(uint64_t v, unsigned lane, unsigned esize, uint64_t nv)
{
    unsigned sh = lane * (8U << esize);
    uint64_t m = ni_emask(8U << esize);
    v &= ~(m << sh);
    v |= (nv & m) << sh;
    return v;
}

static int64_t ni_sext(uint64_t v, unsigned bits)
{
    uint64_t u = v & ni_emask(bits);
    if (bits < 64U && ((u >> (bits - 1U)) & 1ULL) != 0ULL) {
        u |= ~ni_emask(bits);
    }
    return (int64_t)u;
}

/* Arithmetic shift right with floor semantics (portable for negatives). */
static int64_t ni_asr(int64_t v, unsigned t)
{
    uint64_t mag;
    uint64_t q;
    if (t == 0U) {
        return v;
    }
    if (t >= 64U) {
        return (v >= 0) ? 0 : -1;
    }
    if (v >= 0) {
        return (int64_t)((uint64_t)v >> t);
    }
    mag = ~(uint64_t)v + 1ULL;
    q = (mag >> t) + (((mag & (ni_emask(t))) != 0ULL) ? 1ULL : 0ULL);
    return -(int64_t)q;
}

static uint64_t ni_sat_s(int64_t v, unsigned bits, bool *sat)
{
    int64_t smax = (int64_t)(ni_emask(bits) >> 1);
    int64_t smin = -smax - 1;
    *sat = false;
    if (v > smax) {
        *sat = true;
        v = smax;
    } else if (v < smin) {
        *sat = true;
        v = smin;
    }
    return ((uint64_t)v) & ni_emask(bits);
}

static uint64_t ni_sat_u(uint64_t v, unsigned bits, bool *sat)
{
    uint64_t max = ni_emask(bits);
    *sat = false;
    if (v > max) {
        *sat = true;
        v = max;
    }
    return v;
}

static uint64_t ni_sat_su(int64_t v, unsigned bits, bool *sat)
{
    uint64_t max = ni_emask(bits);
    *sat = false;
    if (v < 0) {
        *sat = true;
        return 0ULL;
    }
    if ((uint64_t)v > max) {
        *sat = true;
        return max;
    }
    return (uint64_t)v;
}

/* Saturating left shift, s >= 0, v masked to e bits; sat_u selects range. */
static uint64_t ni_qshl(uint64_t v, unsigned e, unsigned s, bool sat_u, bool *sat)
{
    *sat = false;
    if (sat_u) {
        uint64_t max = ni_emask(e);
        if (s >= 64U) {
            if (v != 0ULL) {
                *sat = true;
                return max;
            }
            return 0ULL;
        }
        if (v > (max >> s)) {
            *sat = true;
            return max;
        }
        return ((v << s) & max);
    }
    {
        int64_t sv = ni_sext(v, e);
        int64_t smax = (int64_t)(ni_emask(e) >> 1);
        int64_t smin = -smax - 1;
        if (e < 64U && s >= 64U) {
            if (sv == 0) {
                return 0ULL;
            }
            *sat = true;
            return ((uint64_t)(sv > 0 ? smax : smin)) & ni_emask(e);
        }
        if (e >= 64U && s >= 64U) {
            if (sv == 0) {
                return 0ULL;
            }
            *sat = true;
            return (sv > 0) ? 0x7FFFFFFFFFFFFFFFULL : 0x8000000000000000ULL;
        }
        if (sv >= 0) {
            if (sv > (smax >> s)) {
                *sat = true;
                return ((uint64_t)smax) & ni_emask(e);
            }
        } else {
            int64_t thr = -((smax + 1) >> s);
            if (sv < thr) {
                *sat = true;
                return ((uint64_t)smin) & ni_emask(e);
            }
        }
        return (((uint64_t)sv) << s) & ni_emask(e);
    }
}

/* Round-half-up shift right, 1 <= t <= e <= 64; v masked to e bits. */
static uint64_t ni_rshr(uint64_t v, unsigned e, unsigned t, bool su)
{
    uint64_t lo;
    uint64_t base;
    uint64_t carry;
    if (t > e) {
        return 0ULL;
    }
    if (t >= 64U) {
        if (!su) {
            return (v >= (1ULL << 63)) ? 1ULL : 0ULL;
        }
        return 0ULL;
    }
    lo = v & ni_emask(t);
    if (su) {
        base = (uint64_t)ni_asr(ni_sext(v, e), t);
    } else {
        base = v >> t;
    }
    carry = (lo >= (1ULL << (t - 1U))) ? 1ULL : 0ULL;
    return (base + carry) & ni_emask(e);
}

/* VDUP from ARM core register (cp11, bit4 = 1, L = 0, bit23 = 1, bit6 = 0). */
static bool ni_vdup_core(a32_ctx_t *ctx, uint32_t pc, uint32_t insn)
{
    bool b_bit;
    bool q_bit;
    bool e_bit;
    unsigned size;
    unsigned vd;
    unsigned rt;
    uint32_t w;
    uint64_t pat;
    unsigned per;
    if (((insn >> 8) & 0xFU) != 11U || ((insn >> 4) & 1U) == 0U ||
        ((insn >> 20) & 1U) != 0U || (insn & 0x0F800040U) != 0x0E800000U) {
        return false;
    }
    b_bit = ((insn >> 22) & 1U) != 0U;
    q_bit = ((insn >> 21) & 1U) != 0U;
    e_bit = ((insn >> 5) & 1U) != 0U;
    if (b_bit && e_bit) {
        return false;
    }
    size = b_bit ? 0U : (e_bit ? 1U : 2U);
    vd = (((insn >> 7) & 1U) << 4) | ((insn >> 16) & 0xFU);
    rt = (insn >> 12) & 0xFU;
    if (rt == 15U || (q_bit && (vd & 1U) != 0U)) {
        return false;
    }
    w = ctx->regs[rt] & (uint32_t)ni_emask(8U << size);
    pat = 0ULL;
    per = ni_per(size);
    for (unsigned k = 0U; k < per; k++) {
        pat = ni_put(pat, k, size, w);
    }
    if (q_bit) {
        ctx->vfp_d[(vd & ~1U) & 31U] = pat;
        ctx->vfp_d[((vd & ~1U) + 1U) & 31U] = pat;
    } else {
        ctx->vfp_d[vd & 31U] = pat;
    }
    ctx->regs[15] = pc + 4U;
    return true;
}

/* VEXT.8: byte extract across Dn:Dm (D) or Qn:Qm (Q). */
static bool ni_vext(a32_ctx_t *ctx, uint32_t pc, uint32_t insn)
{
    unsigned imm4;
    bool is_q;
    unsigned d;
    unsigned n;
    unsigned m;
    if ((insn & 0xFFB00010U) != 0xF2B00000U) {
        return false;
    }
    imm4 = (insn >> 8) & 0xFU;
    is_q = ((insn >> 6) & 1U) != 0U;
    if (!is_q && (imm4 & 8U) != 0U) {
        return false;
    }
    d = neon_reg_d(insn);
    n = neon_reg_n(insn);
    m = neon_reg_m(insn);
    if (!is_q) {
        uint64_t lo = ctx->vfp_d[m & 31U];
        uint64_t hi = ctx->vfp_d[n & 31U];
        uint64_t out;
        unsigned s = imm4 * 8U;
        if (s == 0U) {
            out = lo;
        } else {
            out = (lo >> s) | (hi << (64U - s));
        }
        ctx->vfp_d[d & 31U] = out;
    } else {
        uint8_t src[32];
        uint64_t out_lo = 0ULL;
        uint64_t out_hi = 0ULL;
        unsigned mb = (m & ~1U) & 31U;
        unsigned nb = (n & ~1U) & 31U;
        uint64_t words[4];
        words[0] = ctx->vfp_d[mb];
        words[1] = ctx->vfp_d[(mb + 1U) & 31U];
        words[2] = ctx->vfp_d[nb];
        words[3] = ctx->vfp_d[(nb + 1U) & 31U];
        for (unsigned i = 0U; i < 32U; i++) {
            src[i] = (uint8_t)(words[i >> 3] >> ((i & 7U) * 8U));
        }
        for (unsigned k = 0U; k < 8U; k++) {
            out_lo |= (uint64_t)src[k + imm4] << (k * 8U);
            out_hi |= (uint64_t)src[k + 8U + imm4] << (k * 8U);
        }
        ctx->vfp_d[(d & ~1U) & 31U] = out_lo;
        ctx->vfp_d[((d & ~1U) + 1U) & 31U] = out_hi;
    }
    ctx->regs[15] = pc + 4U;
    return true;
}

/* Narrowing (VMOVN/VQMOVUN/VQMOVN) and VSHLL maximum shift (F3B, opc1 == 2). */
static bool ni_narrow(a32_ctx_t *ctx, uint32_t pc, uint32_t insn)
{
    unsigned opc1;
    unsigned size;
    unsigned op5;
    bool is_q;
    unsigned d;
    unsigned m;
    if ((insn & 0xFFB00010U) != 0xF3B00000U) {
        return false;
    }
    opc1 = (insn >> 16) & 3U;
    size = (insn >> 18) & 3U;
    op5 = (insn >> 7) & 0x1FU;
    is_q = ((insn >> 6) & 1U) != 0U;
    d = neon_reg_d(insn);
    m = neon_reg_m(insn);
    if (opc1 != 2U || size > 2U) {
        return false;
    }
    if (op5 == 4U || op5 == 5U) {
        bool src_u = (op5 == 5U) && is_q;
        bool dst_u = (op5 == 4U) ? is_q : true;
        bool sat_op = (op5 == 4U) ? is_q : true;
        unsigned per = ni_per(size);
        unsigned e = 8U << size;
        unsigned mb = (m & ~1U) & 31U;
        uint64_t lo = ctx->vfp_d[mb];
        uint64_t hi = ctx->vfp_d[(mb + 1U) & 31U];
        uint64_t out = 0ULL;
        if (op5 == 4U && !is_q) {
            sat_op = false;
        }
        for (unsigned k = 0U; k < per; k++) {
            uint64_t sk = (k < per / 2U) ? ni_get(lo, k, size + 1U)
                                         : ni_get(hi, k - per / 2U, size + 1U);
            uint64_t r;
            if (!sat_op) {
                r = sk & ni_emask(e);
            } else if (!src_u && !dst_u) {
                bool s = false;
                r = ni_sat_s(ni_sext(sk, e * 2U), e, &s);
                if (s) {
                    ctx->fpscr |= NI_FPSCR_QC;
                }
            } else if (src_u) {
                bool s = false;
                r = ni_sat_u(sk, e, &s);
                if (s) {
                    ctx->fpscr |= NI_FPSCR_QC;
                }
            } else {
                bool s = false;
                r = ni_sat_su(ni_sext(sk, e * 2U), e, &s);
                if (s) {
                    ctx->fpscr |= NI_FPSCR_QC;
                }
            }
            out = ni_put(out, k, size, r);
        }
        ctx->vfp_d[d & 31U] = out;
        ctx->regs[15] = pc + 4U;
        return true;
    }
    if (op5 == 6U && !is_q) {
        bool is_u = ((insn >> 24) & 1U) != 0U;
        unsigned per = ni_per(size);
        unsigned e = 8U << size;
        uint64_t src = ctx->vfp_d[m & 31U];
        unsigned db = (d & ~1U) & 31U;
        uint64_t out_lo = 0ULL;
        uint64_t out_hi = 0ULL;
        for (unsigned k = 0U; k < per; k++) {
            uint64_t s = ni_get(src, k, size);
            uint64_t w = is_u ? s : (uint64_t)ni_sext(s, e);
            uint64_t r = (w << e) & ni_emask(e * 2U);
            if (k < per / 2U) {
                out_lo = ni_put(out_lo, k, size + 1U, r);
            } else {
                out_hi = ni_put(out_hi, k - per / 2U, size + 1U, r);
            }
        }
        ctx->vfp_d[db] = out_lo;
        ctx->vfp_d[(db + 1U) & 31U] = out_hi;
        ctx->regs[15] = pc + 4U;
        return true;
    }
    return false;
}

/* 3-register same-length integer ALU (bit23 == 0). */
static bool ni_same(a32_ctx_t *ctx, uint32_t pc, uint32_t insn)
{
    bool u;
    unsigned size;
    unsigned op;
    bool b4;
    bool is_q;
    unsigned dd;
    unsigned dn;
    unsigned dm;
    if ((insn & 0x0E800000U) != 0x02000000U) {
        return false;
    }
    u = ((insn >> 24) & 1U) != 0U;
    size = (insn >> 20) & 3U;
    op = (insn >> 8) & 0xFU;
    b4 = ((insn >> 4) & 1U) != 0U;
    is_q = ((insn >> 6) & 1U) != 0U;
    dd = neon_reg_d(insn);
    dn = neon_reg_n(insn);
    dm = neon_reg_m(insn);
    /* VHADD / VRHADD / VHSUB */
    if (!b4 && op <= 2U && size < 3U) {
        unsigned p = ni_per(size);
        unsigned per = is_q ? p * 2U : p;
        for (unsigned k = 0U; k < per; k++) {
            unsigned half = (is_q && k >= p) ? 1U : 0U;
            unsigned k0 = (is_q && k >= p) ? k - p : k;
            unsigned d0 = is_q ? (((dd & ~1U) + half) & 31U) : dd & 31U;
            unsigned n0 = is_q ? (((dn & ~1U) + half) & 31U) : dn & 31U;
            unsigned m0 = is_q ? (((dm & ~1U) + half) & 31U) : dm & 31U;
            uint64_t a = ni_get(ctx->vfp_d[n0], k0, size);
            uint64_t b = ni_get(ctx->vfp_d[m0], k0, size);
            uint64_t r;
            if (!u) {
                int64_t sa = ni_sext(a, 8U << size);
                int64_t sb = ni_sext(b, 8U << size);
                int64_t t = (op == 1U) ? sa + sb + 1 : sa + (op == 0U ? sb : -sb);
                r = (uint64_t)ni_asr(t, 1U);
            } else {
                uint64_t t = (op == 1U) ? a + b + 1ULL : (op == 0U ? a + b : a - b);
                r = t >> 1;
            }
            ctx->vfp_d[d0] = ni_put(ctx->vfp_d[d0], k0, size, r);
        }
        ctx->regs[15] = pc + 4U;
        return true;
    }
    /* VQSUB (VQADD lives in x_alu) */
    if (op == 2U && b4) {
        if (size > 3U || (size == 3U && is_q)) {
            return false;
        }
        if (size == 3U) {
            uint64_t a = ctx->vfp_d[dn & 31U];
            uint64_t b = ctx->vfp_d[dm & 31U];
            uint64_t r;
            bool sat = false;
            if (!u) {
                int64_t sa = (int64_t)a;
                int64_t sb = (int64_t)b;
                bool ov = (sb > 0 && sa < (int64_t)0x8000000000000000ULL + sb) ||
                          (sb < 0 && sa > 0x7FFFFFFFFFFFFFFFLL + sb);
                if (ov) {
                    sat = true;
                    r = (sa >= 0) ? 0x7FFFFFFFFFFFFFFFULL : 0x8000000000000000ULL;
                } else {
                    r = (uint64_t)(sa - sb);
                }
            } else {
                if (a < b) {
                    sat = true;
                    r = 0ULL;
                } else {
                    r = a - b;
                }
            }
            if (sat) {
                ctx->fpscr |= NI_FPSCR_QC;
            }
            ctx->vfp_d[dd & 31U] = r;
        } else {
            unsigned e = 8U << size;
            unsigned p = ni_per(size);
            unsigned per = is_q ? p * 2U : p;
            for (unsigned k = 0U; k < per; k++) {
                unsigned half = (is_q && k >= p) ? 1U : 0U;
                unsigned k0 = (is_q && k >= p) ? k - p : k;
                unsigned d0 = is_q ? (((dd & ~1U) + half) & 31U) : dd & 31U;
                unsigned n0 = is_q ? (((dn & ~1U) + half) & 31U) : dn & 31U;
                unsigned m0 = is_q ? (((dm & ~1U) + half) & 31U) : dm & 31U;
                uint64_t a = ni_get(ctx->vfp_d[n0], k0, size);
                uint64_t b = ni_get(ctx->vfp_d[m0], k0, size);
                uint64_t r;
                if (!u) {
                    bool s = false;
                    r = ni_sat_s(ni_sext(a, e) - ni_sext(b, e), e, &s);
                    if (s) {
                        ctx->fpscr |= NI_FPSCR_QC;
                    }
                } else {
                    if (a < b) {
                        ctx->fpscr |= NI_FPSCR_QC;
                        r = 0ULL;
                    } else {
                        r = a - b;
                    }
                }
                ctx->vfp_d[d0] = ni_put(ctx->vfp_d[d0], k0, size, r);
            }
        }
        ctx->regs[15] = pc + 4U;
        return true;
    }
    /* VCGT / VCGE register */
    if (op == 3U && size < 3U) {
        bool want_ge = b4;
        unsigned p = ni_per(size);
        unsigned per = is_q ? p * 2U : p;
        for (unsigned k = 0U; k < per; k++) {
            unsigned half = (is_q && k >= p) ? 1U : 0U;
            unsigned k0 = (is_q && k >= p) ? k - p : k;
            unsigned d0 = is_q ? (((dd & ~1U) + half) & 31U) : dd & 31U;
            unsigned n0 = is_q ? (((dn & ~1U) + half) & 31U) : dn & 31U;
            unsigned m0 = is_q ? (((dm & ~1U) + half) & 31U) : dm & 31U;
            uint64_t a = ni_get(ctx->vfp_d[n0], k0, size);
            uint64_t b = ni_get(ctx->vfp_d[m0], k0, size);
            bool t;
            if (!u) {
                int64_t sa = ni_sext(a, 8U << size);
                int64_t sb = ni_sext(b, 8U << size);
                t = want_ge ? sa >= sb : sa > sb;
            } else {
                t = want_ge ? a >= b : a > b;
            }
            ctx->vfp_d[d0] = ni_put(ctx->vfp_d[d0], k0, size, t ? ni_emask(8U << size) : 0ULL);
        }
        ctx->regs[15] = pc + 4U;
        return true;
    }
    /* VCEQ register (U) / VTST (!U) */
    if (op == 8U && b4 && size < 3U) {
        unsigned p = ni_per(size);
        unsigned per = is_q ? p * 2U : p;
        for (unsigned k = 0U; k < per; k++) {
            unsigned half = (is_q && k >= p) ? 1U : 0U;
            unsigned k0 = (is_q && k >= p) ? k - p : k;
            unsigned d0 = is_q ? (((dd & ~1U) + half) & 31U) : dd & 31U;
            unsigned n0 = is_q ? (((dn & ~1U) + half) & 31U) : dn & 31U;
            unsigned m0 = is_q ? (((dm & ~1U) + half) & 31U) : dm & 31U;
            uint64_t a = ni_get(ctx->vfp_d[n0], k0, size);
            uint64_t b = ni_get(ctx->vfp_d[m0], k0, size);
            bool t = u ? (a == b) : ((a & b) != 0ULL);
            ctx->vfp_d[d0] = ni_put(ctx->vfp_d[d0], k0, size, t ? ni_emask(8U << size) : 0ULL);
        }
        ctx->regs[15] = pc + 4U;
        return true;
    }
    /* VSHL / VRSHL / VQSHL / VQRSHL register (op 4 / 5) */
    if ((op == 4U || op == 5U) && (size < 3U || !is_q)) {
        unsigned e = (size == 3U) ? 64U : 8U << size;
        unsigned esz = (size == 3U) ? 3U : size;
        unsigned p = ni_per(esz);
        unsigned per = is_q ? p * 2U : p;
        bool rounding = b4;
        bool sat_op = (op == 5U);
        for (unsigned k = 0U; k < per; k++) {
            unsigned half = (is_q && k >= p) ? 1U : 0U;
            unsigned k0 = (is_q && k >= p) ? k - p : k;
            unsigned d0 = is_q ? (((dd & ~1U) + half) & 31U) : dd & 31U;
            unsigned n0 = is_q ? (((dn & ~1U) + half) & 31U) : dn & 31U;
            unsigned m0 = is_q ? (((dm & ~1U) + half) & 31U) : dm & 31U;
            uint64_t a = ni_get(ctx->vfp_d[n0], k0, esz);
            uint64_t mb = ni_get(ctx->vfp_d[m0], k0, esz);
            int s = (int)(int8_t)(mb & 0xFFU);
            uint64_t r;
            if (s >= 0) {
                unsigned us = (unsigned)s;
                if (!sat_op) {
                    r = (us >= e) ? 0ULL : (a << us) & ni_emask(e);
                } else {
                    bool st = false;
                    r = ni_qshl(a, e, us, u, &st);
                    if (st) {
                        ctx->fpscr |= NI_FPSCR_QC;
                    }
                }
            } else {
                unsigned t = (unsigned)(-s);
                if (!rounding) {
                    if (t >= e) {
                        r = 0ULL;
                    } else if (!u) {
                        r = (uint64_t)ni_asr(ni_sext(a, e), t) & ni_emask(e);
                    } else {
                        r = (a >> t) & ni_emask(e);
                    }
                } else {
                    if (t > e) {
                        r = 0ULL;
                    } else {
                        r = ni_rshr(a, e, t, !u);
                    }
                }
            }
            ctx->vfp_d[d0] = ni_put(ctx->vfp_d[d0], k0, esz, r);
        }
        ctx->regs[15] = pc + 4U;
        return true;
    }
    /* VMAX / VMIN */
    if (op == 6U && size < 3U) {
        bool want_min = b4;
        unsigned p = ni_per(size);
        unsigned per = is_q ? p * 2U : p;
        for (unsigned k = 0U; k < per; k++) {
            unsigned half = (is_q && k >= p) ? 1U : 0U;
            unsigned k0 = (is_q && k >= p) ? k - p : k;
            unsigned d0 = is_q ? (((dd & ~1U) + half) & 31U) : dd & 31U;
            unsigned n0 = is_q ? (((dn & ~1U) + half) & 31U) : dn & 31U;
            unsigned m0 = is_q ? (((dm & ~1U) + half) & 31U) : dm & 31U;
            uint64_t a = ni_get(ctx->vfp_d[n0], k0, size);
            uint64_t b = ni_get(ctx->vfp_d[m0], k0, size);
            bool take_b;
            if (!u) {
                int64_t sa = ni_sext(a, 8U << size);
                int64_t sb = ni_sext(b, 8U << size);
                take_b = want_min ? sb < sa : sb > sa;
            } else {
                take_b = want_min ? b < a : b > a;
            }
            ctx->vfp_d[d0] = ni_put(ctx->vfp_d[d0], k0, size, take_b ? b : a);
        }
        ctx->regs[15] = pc + 4U;
        return true;
    }
    /* VABD / VABA */
    if (op == 7U && size < 3U) {
        unsigned p = ni_per(size);
        unsigned per = is_q ? p * 2U : p;
        for (unsigned k = 0U; k < per; k++) {
            unsigned half = (is_q && k >= p) ? 1U : 0U;
            unsigned k0 = (is_q && k >= p) ? k - p : k;
            unsigned d0 = is_q ? (((dd & ~1U) + half) & 31U) : dd & 31U;
            unsigned n0 = is_q ? (((dn & ~1U) + half) & 31U) : dn & 31U;
            unsigned m0 = is_q ? (((dm & ~1U) + half) & 31U) : dm & 31U;
            uint64_t a = ni_get(ctx->vfp_d[n0], k0, size);
            uint64_t b = ni_get(ctx->vfp_d[m0], k0, size);
            uint64_t abd;
            if (!u) {
                int64_t df = ni_sext(a, 8U << size) - ni_sext(b, 8U << size);
                abd = (uint64_t)(df < 0 ? -df : df);
            } else {
                abd = (a >= b) ? a - b : b - a;
            }
            if (b4) {
                uint64_t cur = ni_get(ctx->vfp_d[d0], k0, size);
                abd = (cur + abd) & ni_emask(8U << size);
            }
            ctx->vfp_d[d0] = ni_put(ctx->vfp_d[d0], k0, size, abd);
        }
        ctx->regs[15] = pc + 4U;
        return true;
    }
    /* VPADD integer (D only, !U) */
    if (op == 0xBU && b4 && !u && !is_q && size < 3U) {
        unsigned per = ni_per(size);
        unsigned half = per / 2U;
        uint64_t a = ctx->vfp_d[dn & 31U];
        uint64_t b = ctx->vfp_d[dm & 31U];
        uint64_t out = 0ULL;
        for (unsigned k = 0U; k < per; k++) {
            uint64_t x;
            uint64_t y;
            if (k < half) {
                x = ni_get(a, 2U * k, size);
                y = ni_get(a, 2U * k + 1U, size);
            } else {
                x = ni_get(b, 2U * (k - half), size);
                y = ni_get(b, 2U * (k - half) + 1U, size);
            }
            out = ni_put(out, k, size, x + y);
        }
        ctx->vfp_d[dd & 31U] = out;
        ctx->regs[15] = pc + 4U;
        return true;
    }
    /* VPMAX / VPMIN integer (D only) */
    if (op == 0xAU && !is_q && size < 3U) {
        bool want_min = b4;
        unsigned per = ni_per(size);
        unsigned half = per / 2U;
        uint64_t a = ctx->vfp_d[dn & 31U];
        uint64_t b = ctx->vfp_d[dm & 31U];
        uint64_t out = 0ULL;
        for (unsigned k = 0U; k < per; k++) {
            uint64_t x;
            uint64_t y;
            bool take_y;
            if (k < half) {
                x = ni_get(a, 2U * k, size);
                y = ni_get(a, 2U * k + 1U, size);
            } else {
                x = ni_get(b, 2U * (k - half), size);
                y = ni_get(b, 2U * (k - half) + 1U, size);
            }
            if (!u) {
                int64_t sx = ni_sext(x, 8U << size);
                int64_t sy = ni_sext(y, 8U << size);
                take_y = want_min ? sy < sx : sy > sx;
            } else {
                take_y = want_min ? y < x : y > x;
            }
            out = ni_put(out, k, size, take_y ? y : x);
        }
        ctx->vfp_d[dd & 31U] = out;
        ctx->regs[15] = pc + 4U;
        return true;
    }
    return false;
}

bool i_vfp_neon_int(a32_ctx_t *ctx, uint32_t pc, uint32_t insn)
{
    if (ni_vdup_core(ctx, pc, insn)) {
        return true;
    }
    if (ni_vext(ctx, pc, insn)) {
        return true;
    }
    if (ni_narrow(ctx, pc, insn)) {
        return true;
    }
    if (ni_same(ctx, pc, insn)) {
        return true;
    }
    return false;
}
