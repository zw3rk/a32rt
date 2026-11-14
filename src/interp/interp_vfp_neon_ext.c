/*
 * src/interp/interp_vfp_neon_ext.c — Extended ARMv7 NEON reference interpreter
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "interp.h"
#include "interp_priv.h"

#include "interp/decode_priv.h"

#include <math.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif

static int64_t x_sat_s(int64_t v, unsigned bits)
{
    int64_t lo = -(1LL << (bits - 1U));
    int64_t hi = (1LL << (bits - 1U)) - 1LL;
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static uint64_t x_sat_u(int64_t v, unsigned bits)
{
    if (v < 0) {
        return 0ULL;
    }
    uint64_t hi = (bits >= 64U) ? ~0ULL : ((1ULL << bits) - 1ULL);
    if ((uint64_t)v > hi) {
        return hi;
    }
    return (uint64_t)v;
}

static unsigned x_per_d(unsigned esize)
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

static uint64_t x_glane(uint64_t v, unsigned lane, unsigned esize)
{
    if (esize == 0U) {
        return (v >> (lane * 8U)) & 0xFFULL;
    }
    if (esize == 1U) {
        return (v >> (lane * 16U)) & 0xFFFFULL;
    }
    if (esize == 2U) {
        return (v >> (lane * 32U)) & 0xFFFFFFFFULL;
    }
    return v;
}

static uint64_t x_plane(uint64_t v, unsigned lane, unsigned esize, uint64_t nv)
{
    if (esize == 0U) {
        v &= ~(0xFFULL << (lane * 8U));
        v |= (nv & 0xFFULL) << (lane * 8U);
    } else if (esize == 1U) {
        v &= ~(0xFFFFULL << (lane * 16U));
        v |= (nv & 0xFFFFULL) << (lane * 16U);
    } else if (esize == 2U) {
        v &= ~(0xFFFFFFFFULL << (lane * 32U));
        v |= (nv & 0xFFFFFFFFULL) << (lane * 32U);
    } else {
        v = nv;
    }
    return v;
}

static bool x_range(a32_ctx_t *ctx, uint32_t addr, uint32_t len, bool is_load,
    bool *faulted)
{
    uint32_t prot = is_load ? (uint32_t)PROT_READ : (uint32_t)PROT_WRITE;
    if (!i_mem_ok(ctx, addr, len, prot)) {
        *faulted = true;
        (void)i_fault(ctx, addr, !is_load);
        return false;
    }
    return true;
}

static bool x_vld_vst(a32_ctx_t *ctx, uint32_t pc, uint32_t insn, bool *faulted)
{
    if ((insn & 0xFF100000U) != 0xF4000000U) {
        return false;
    }
    bool l = ((insn >> 21) & 1U) != 0U;
    unsigned rn = (insn >> 16) & 0xFU;
    unsigned d_start = neon_reg_d(insn);
    unsigned type = (insn >> 8) & 0xFU;
    unsigned rm = insn & 0xFU;
    uint32_t base = ctx->regs[rn];
    if ((insn & 0x00800000U) != 0U) {
        unsigned size = (insn >> 10) & 3U;
        unsigned n_structs = ((insn >> 8) & 3U) + 1U;
        unsigned index_align = (insn >> 4) & 0xFU;
        unsigned dup_size = (insn >> 6) & 3U;
        if (n_structs == 4U && dup_size == 3U && (insn & 0x10U) != 0U) {
            dup_size = 2U;
        }
        if (size == 3U && (!l || dup_size > 2U)) {
            return false;
        }
        if (size < 3U) {
            unsigned lane = (size == 0U) ? ((insn >> 5) & 7U) :
                (size == 1U) ? ((insn >> 6) & 3U) : ((insn >> 7) & 1U);
            unsigned step = 1U;
            if (size == 1U && (index_align & 2U) != 0U) {
                step = 2U;
            } else if (size == 2U && (index_align & 4U) != 0U) {
                step = 2U;
            }
            uint32_t eb = 1U << size;
            uint32_t total = n_structs * eb;
            if (!x_range(ctx, base, total, l, faulted)) {
                return true;
            }
            uint32_t addr = base;
            for (unsigned s = 0U; s < n_structs; s++) {
                unsigned dr = (d_start + s * step) & 31U;
                uint64_t cur = ctx->vfp_d[dr];
                if (l) {
                    if (size == 0U) {
                        uint8_t bv = 0U;
                        if (!mem_r(ctx, addr, &bv, 1U)) {
                            *faulted = true;
                            return true;
                        }
                        cur &= ~(0xFFULL << (lane * 8U));
                        cur |= (uint64_t)bv << (lane * 8U);
                    } else if (size == 1U) {
                        uint16_t hv = 0U;
                        if (!mem_r(ctx, addr, &hv, 2U)) {
                            *faulted = true;
                            return true;
                        }
                        cur &= ~(0xFFFFULL << (lane * 16U));
                        cur |= (uint64_t)hv << (lane * 16U);
                    } else {
                        uint32_t w = 0U;
                        if (!mem_r(ctx, addr, &w, 4U)) {
                            *faulted = true;
                            return true;
                        }
                        if (lane == 0U) {
                            cur = (cur & 0xFFFFFFFF00000000ULL) | w;
                        } else {
                            cur = (cur & 0xFFFFFFFFULL) | ((uint64_t)w << 32);
                        }
                    }
                    ctx->vfp_d[dr] = cur;
                } else {
                    if (size == 0U) {
                        uint8_t bv = (uint8_t)(cur >> (lane * 8U));
                        if (!mem_w(ctx, addr, &bv, 1U)) {
                            *faulted = true;
                            return true;
                        }
                    } else if (size == 1U) {
                        uint16_t hv = (uint16_t)(cur >> (lane * 16U));
                        if (!mem_w(ctx, addr, &hv, 2U)) {
                            *faulted = true;
                            return true;
                        }
                    } else {
                        uint32_t w = (lane == 0U) ? (uint32_t)cur :
                            (uint32_t)(cur >> 32);
                        if (!mem_w(ctx, addr, &w, 4U)) {
                            *faulted = true;
                            return true;
                        }
                    }
                }
                addr += eb;
            }
            if (rm != 0xFU && rn != 15U) {
                ctx->regs[rn] = base + ((rm == 0xDU) ? total : ctx->regs[rm]);
            }
            ctx->regs[15] = pc + 4U;
            return true;
        }
        bool t_bit = ((insn >> 5) & 1U) != 0U;
        uint32_t eb = 1U << dup_size;
        uint32_t total = n_structs * eb;
        if (!x_range(ctx, base, total, l, faulted)) {
            return true;
        }
        uint32_t addr = base;
        unsigned step = t_bit ? 2U : 1U;
        for (unsigned s = 0U; s < n_structs; s++) {
            uint64_t dup = 0ULL;
            if (dup_size == 0U) {
                uint8_t bv = 0U;
                if (!mem_r(ctx, addr, &bv, 1U)) {
                    *faulted = true;
                    return true;
                }
                uint64_t b = bv;
                dup = b | (b << 8) | (b << 16) | (b << 24) | (b << 32) |
                    (b << 40) | (b << 48) | (b << 56);
            } else if (dup_size == 1U) {
                uint16_t hv = 0U;
                if (!mem_r(ctx, addr, &hv, 2U)) {
                    *faulted = true;
                    return true;
                }
                uint64_t h = hv;
                dup = h | (h << 16) | (h << 32) | (h << 48);
            } else {
                uint32_t w = 0U;
                if (!mem_r(ctx, addr, &w, 4U)) {
                    *faulted = true;
                    return true;
                }
                dup = (uint64_t)w | ((uint64_t)w << 32);
            }
            unsigned dr = (d_start + s * step) & 31U;
            ctx->vfp_d[dr] = dup;
            if (t_bit && n_structs == 1U) {
                ctx->vfp_d[(dr + 1U) & 31U] = dup;
            }
            addr += eb;
        }
        if (rm != 0xFU && rn != 15U) {
            ctx->regs[rn] = base + ((rm == 0xDU) ? total : ctx->regs[rm]);
        }
        ctx->regs[15] = pc + 4U;
        return true;
    }
    if (type == 0x7U || type == 0xAU || type == 0x6U || type == 0x2U) {
        return false;
    }
    if (type == 0x8U || type == 0x9U || type == 0x3U) {
        unsigned esz = (insn >> 6) & 3U;
        if (esz > 2U) {
            return false;
        }
        unsigned eb = 1U << esz;
        unsigned per = x_per_d(esz);
        if (type == 0x3U) {
            if (!x_range(ctx, base, 32U, l, faulted)) {
                return true;
            }
            uint8_t b[32];
            if (l) {
                for (unsigned i = 0U; i < 4U; i++) {
                    uint64_t w = 0ULL;
                    if (!mem_r(ctx, base + i * 8U, &w, 8U)) {
                        *faulted = true;
                        return true;
                    }
                    memcpy(&b[i * 8U], &w, 8U);
                }
                for (unsigned h = 0U; h < 2U; h++) {
                    for (unsigned k = 0U; k < per; k++) {
                        uint32_t e0 = 0U;
                        uint32_t e1 = 0U;
                        memcpy(&e0, &b[h * 16U + (2U * k) * eb], eb);
                        memcpy(&e1, &b[h * 16U + (2U * k + 1U) * eb], eb);
                        unsigned d0 = (d_start + 2U * h) & 31U;
                        unsigned d1 = (d_start + 2U * h + 1U) & 31U;
                        uint64_t c0 = ctx->vfp_d[d0];
                        uint64_t c1 = ctx->vfp_d[d1];
                        c0 = x_plane(c0, k, esz, e0);
                        c1 = x_plane(c1, k, esz, e1);
                        ctx->vfp_d[d0] = c0;
                        ctx->vfp_d[d1] = c1;
                    }
                }
            } else {
                for (unsigned h = 0U; h < 2U; h++) {
                    unsigned d0 = (d_start + 2U * h) & 31U;
                    unsigned d1 = (d_start + 2U * h + 1U) & 31U;
                    uint64_t c0 = ctx->vfp_d[d0];
                    uint64_t c1 = ctx->vfp_d[d1];
                    for (unsigned k = 0U; k < per; k++) {
                        uint32_t e0 = (uint32_t)x_glane(c0, k, esz);
                        uint32_t e1 = (uint32_t)x_glane(c1, k, esz);
                        memcpy(&b[h * 16U + (2U * k) * eb], &e0, eb);
                        memcpy(&b[h * 16U + (2U * k + 1U) * eb], &e1, eb);
                    }
                }
                for (unsigned i = 0U; i < 4U; i++) {
                    uint64_t w = 0ULL;
                    memcpy(&w, &b[i * 8U], 8U);
                    if (!mem_w(ctx, base + i * 8U, &w, 8U)) {
                        *faulted = true;
                        return true;
                    }
                }
            }
            if (rm != 0xFU && rn != 15U) {
                ctx->regs[rn] = base + ((rm == 0xDU) ? 32U : ctx->regs[rm]);
            }
            ctx->regs[15] = pc + 4U;
            return true;
        }
        if (!x_range(ctx, base, 16U, l, faulted)) {
            return true;
        }
        unsigned step = (type == 0x9U) ? 2U : 1U;
        unsigned d0 = d_start & 31U;
        unsigned d1 = (d_start + step) & 31U;
        if (l) {
            uint8_t b[16];
            for (unsigned i = 0U; i < 2U; i++) {
                uint64_t w = 0ULL;
                if (!mem_r(ctx, base + i * 8U, &w, 8U)) {
                    *faulted = true;
                    return true;
                }
                memcpy(&b[i * 8U], &w, 8U);
            }
            uint64_t o0 = 0ULL;
            uint64_t o1 = 0ULL;
            for (unsigned k = 0U; k < per; k++) {
                uint32_t e0 = 0U;
                uint32_t e1 = 0U;
                memcpy(&e0, &b[(2U * k) * eb], eb);
                memcpy(&e1, &b[(2U * k + 1U) * eb], eb);
                o0 = x_plane(o0, k, esz, e0);
                o1 = x_plane(o1, k, esz, e1);
            }
            ctx->vfp_d[d0] = o0;
            ctx->vfp_d[d1] = o1;
        } else {
            uint64_t c0 = ctx->vfp_d[d0];
            uint64_t c1 = ctx->vfp_d[d1];
            uint8_t b[16];
            for (unsigned k = 0U; k < per; k++) {
                uint32_t e0 = (uint32_t)x_glane(c0, k, esz);
                uint32_t e1 = (uint32_t)x_glane(c1, k, esz);
                memcpy(&b[(2U * k) * eb], &e0, eb);
                memcpy(&b[(2U * k + 1U) * eb], &e1, eb);
            }
            for (unsigned i = 0U; i < 2U; i++) {
                uint64_t w = 0ULL;
                memcpy(&w, &b[i * 8U], 8U);
                if (!mem_w(ctx, base + i * 8U, &w, 8U)) {
                    *faulted = true;
                    return true;
                }
            }
        }
        if (rm != 0xFU && rn != 15U) {
            ctx->regs[rn] = base + ((rm == 0xDU) ? 16U : ctx->regs[rm]);
        }
        ctx->regs[15] = pc + 4U;
        return true;
    }
    if (type == 0x4U || type == 0x5U || type == 0x0U || type == 0x1U) {
        unsigned esz = (insn >> 6) & 3U;
        if (esz > 2U) {
            return false;
        }
        unsigned eb = 1U << esz;
        unsigned per = x_per_d(esz);
        unsigned nst = (type == 0x4U || type == 0x5U) ? 3U : 4U;
        unsigned step = ((type & 1U) != 0U) ? 2U : 1U;
        uint32_t total = nst * 8U;
        if (!x_range(ctx, base, total, l, faulted)) {
            return true;
        }
        if (l) {
            uint8_t b[32];
            for (unsigned i = 0U; i < nst; i++) {
                uint64_t w = 0ULL;
                if (!mem_r(ctx, base + i * 8U, &w, 8U)) {
                    *faulted = true;
                    return true;
                }
                memcpy(&b[i * 8U], &w, 8U);
            }
            for (unsigned s = 0U; s < nst; s++) {
                unsigned d = (d_start + s * step) & 31U;
                uint64_t out = 0ULL;
                for (unsigned k = 0U; k < per; k++) {
                    uint32_t e = 0U;
                    memcpy(&e, &b[(nst * k + s) * eb], eb);
                    out = x_plane(out, k, esz, e);
                }
                ctx->vfp_d[d] = out;
            }
        } else {
            uint8_t b[32];
            for (unsigned i = 0U; i < sizeof(b); i++) {
                b[i] = 0U;
            }
            for (unsigned s = 0U; s < nst; s++) {
                uint64_t c = ctx->vfp_d[(d_start + s * step) & 31U];
                for (unsigned k = 0U; k < per; k++) {
                    uint32_t e = (uint32_t)x_glane(c, k, esz);
                    memcpy(&b[(nst * k + s) * eb], &e, eb);
                }
            }
            for (unsigned i = 0U; i < nst; i++) {
                uint64_t w = 0ULL;
                memcpy(&w, &b[i * 8U], 8U);
                if (!mem_w(ctx, base + i * 8U, &w, 8U)) {
                    *faulted = true;
                    return true;
                }
            }
        }
        if (rm != 0xFU && rn != 15U) {
            ctx->regs[rn] = base + ((rm == 0xDU) ? total : ctx->regs[rm]);
        }
        ctx->regs[15] = pc + 4U;
        return true;
    }
    return false;
}

static bool x_misc(a32_ctx_t *ctx, uint32_t pc, uint32_t insn)
{
    if ((insn & 0xFFB00010U) != 0xF3B00000U) {
        return false;
    }
    bool is_q = ((insn >> 6) & 1U) != 0U;
    unsigned d = neon_reg_d(insn);
    unsigned m = neon_reg_m(insn);
    unsigned opc1 = (insn >> 16) & 3U;
    unsigned size = (insn >> 18) & 3U;
    if (((insn >> 10) & 3U) == 2U) {
        unsigned len = ((insn >> 8) & 3U) + 1U;
        unsigned dn = neon_reg_n(insn);
        bool is_tbx = is_q;
        uint64_t tab[4];
        for (unsigned i = 0U; i < len && i < 4U; i++) {
            tab[i] = ctx->vfp_d[(dn + i) & 31U];
        }
        uint64_t idx = ctx->vfp_d[m & 31U];
        uint64_t out = is_tbx ? ctx->vfp_d[d & 31U] : 0ULL;
        for (unsigned k = 0U; k < 8U; k++) {
            uint8_t ix = (uint8_t)(idx >> (k * 8U));
            if (ix < len * 8U) {
                uint8_t r = (uint8_t)(tab[ix >> 3] >> ((ix & 7U) * 8U));
                out &= ~(0xFFULL << (k * 8U));
                out |= (uint64_t)r << (k * 8U);
            } else if (!is_tbx) {
                out &= ~(0xFFULL << (k * 8U));
            }
        }
        ctx->vfp_d[d & 31U] = out;
        ctx->regs[15] = pc + 4U;
        return true;
    }
    if (((insn >> 7) & 0x1FU) == 0x18U) {
        if (is_q && (d & 1U) != 0U) {
            return false;
        }
        unsigned imm4 = (insn >> 16) & 0xFU;
        if ((imm4 & 7U) == 0U) {
            return false;
        }
        unsigned dsz = ((imm4 & 1U) != 0U) ? 0U : ((imm4 & 2U) != 0U) ? 1U : 2U;
        unsigned index = (dsz == 0U) ? (imm4 >> 1) : (dsz == 1U) ? (imm4 >> 2) : (imm4 >> 3);
        uint64_t e = x_glane(ctx->vfp_d[m & 31U], index, dsz);
        unsigned per = is_q ? x_per_d(dsz) * 2U : x_per_d(dsz);
        for (unsigned k = 0U; k < per; k++) {
            unsigned dd = is_q ? ((d & ~1U) + (k >= x_per_d(dsz) ? 1U : 0U)) & 31U : d & 31U;
            unsigned kk = is_q ? (k >= x_per_d(dsz) ? k - x_per_d(dsz) : k) : k;
            uint64_t cur = (kk == 0U) ? 0ULL : ctx->vfp_d[dd];
            if (kk == 0U && is_q && k >= x_per_d(dsz)) {
                cur = 0ULL;
            }
            if (kk == 0U) {
                cur = 0ULL;
            } else {
                cur = ctx->vfp_d[dd];
            }
            cur = x_plane(cur, kk, dsz, e);
            ctx->vfp_d[dd] = cur;
        }
        ctx->regs[15] = pc + 4U;
        return true;
    }
    if (((insn >> 11) & 1U) != 0U) {
        return false;
    }
    unsigned op5 = (insn >> 7) & 31U;
    if (opc1 != 0U) {
        return false;
    }
    if ((op5 == 4U || op5 == 5U) && size < 3U) {
        bool su = (op5 == 5U);
        unsigned per = is_q ? x_per_d(size) * 2U : x_per_d(size);
        unsigned out_n = per / 2U;
        for (unsigned k = 0U; k < out_n; k++) {
            uint64_t s0 = is_q ? ctx->vfp_d[(m & ~1U) & 31U] : ctx->vfp_d[m & 31U];
            uint64_t s1 = is_q ? ctx->vfp_d[((m & ~1U) + 1U) & 31U] : 0ULL;
            unsigned nk0 = 2U * k;
            unsigned nk1 = 2U * k + 1U;
            uint64_t v0 = (!is_q) ? x_glane(s0, nk0, size) :
                ((nk0 < x_per_d(size)) ? x_glane(s0, nk0, size) : x_glane(s1, nk0 - x_per_d(size), size));
            uint64_t v1 = (!is_q) ? x_glane(s0, nk1, size) :
                ((nk1 < x_per_d(size)) ? x_glane(s0, nk1, size) : x_glane(s1, nk1 - x_per_d(size), size));
            int64_t a = su ? (int64_t)v0 : ((size == 0U) ? (int64_t)(int8_t)(uint8_t)v0 :
                (size == 1U) ? (int64_t)(int16_t)(uint16_t)v0 : (int64_t)(int32_t)(uint32_t)v0);
            int64_t b = su ? (int64_t)v1 : ((size == 0U) ? (int64_t)(int8_t)(uint8_t)v1 :
                (size == 1U) ? (int64_t)(int16_t)(uint16_t)v1 : (int64_t)(int32_t)(uint32_t)v1);
            uint64_t out = (uint64_t)(a + b);
            if (!is_q) {
                uint64_t cur = (k == 0U) ? 0ULL : ctx->vfp_d[d & 31U];
                if (size == 0U) {
                    cur = (k == 0U) ? (out & 0xFFFFULL) : (cur & ~(0xFFFFULL << (k * 16U))) | ((out & 0xFFFFULL) << (k * 16U));
                } else if (size == 1U) {
                    cur = (k == 0U) ? (cur & 0xFFFFFFFF00000000ULL) | (out & 0xFFFFFFFFULL) :
                        (cur & 0xFFFFFFFFULL) | ((out & 0xFFFFFFFFULL) << 32);
                } else {
                    cur = out;
                }
                ctx->vfp_d[d & 31U] = cur;
            } else {
                unsigned dd = ((d & ~1U) + (k >= out_n / 2U ? 1U : 0U)) & 31U;
                unsigned kk = (k >= out_n / 2U) ? k - out_n / 2U : k;
                uint64_t cur = (kk == 0U) ? 0ULL : ctx->vfp_d[dd];
                if (size == 0U) {
                    cur = (kk == 0U) ? (out & 0xFFFFULL) : (cur & ~(0xFFFFULL << (kk * 16U))) | ((out & 0xFFFFULL) << (kk * 16U));
                } else if (size == 1U) {
                    cur = (kk == 0U) ? (cur & 0xFFFFFFFF00000000ULL) | (out & 0xFFFFFFFFULL) :
                        (cur & 0xFFFFFFFFULL) | ((out & 0xFFFFFFFFULL) << 32);
                } else {
                    cur = out;
                }
                ctx->vfp_d[dd] = cur;
            }
        }
        ctx->regs[15] = pc + 4U;
        return true;
    }
    if ((op5 == 8U || op5 == 9U) && size < 3U) {
        unsigned per = is_q ? x_per_d(size) * 2U : x_per_d(size);
        for (unsigned k = 0U; k < per; k++) {
            unsigned dd = is_q ? ((d & ~1U) + (k >= x_per_d(size) ? 1U : 0U)) & 31U : d & 31U;
            unsigned kk = is_q ? (k >= x_per_d(size) ? k - x_per_d(size) : k) : k;
            uint64_t sv = ctx->vfp_d[(is_q ? ((m & ~1U) + (k >= x_per_d(size) ? 1U : 0U)) : m) & 31U];
            uint64_t u = x_glane(sv, kk, size);
            unsigned bits = (size == 0U) ? 8U : (size == 1U) ? 16U : 32U;
            unsigned r;
            if (op5 == 9U) {
                if (u == 0U) {
                    r = bits;
                } else if (size == 0U) {
                    r = (unsigned)__builtin_clz((unsigned)u) - 24U;
                } else if (size == 1U) {
                    r = (unsigned)__builtin_clz((unsigned)u) - 16U;
                } else {
                    r = (unsigned)__builtin_clz((unsigned)u);
                }
            } else {
                unsigned top = (size == 0U) ? ((u >> 7) & 1U) : (size == 1U) ? ((u >> 15) & 1U) : ((u >> 31) & 1U);
                r = 0U;
                for (unsigned b = 0U; b < bits; b++) {
                    unsigned bit = (size == 0U) ? ((u >> (7U - b)) & 1U) : (size == 1U) ? ((u >> (15U - b)) & 1U) : ((u >> (31U - b)) & 1U);
                    if (bit != top) {
                        break;
                    }
                    r++;
                }
            }
            uint64_t cur = ctx->vfp_d[dd];
            ctx->vfp_d[dd] = x_plane(cur, kk, size, r);
        }
        ctx->regs[15] = pc + 4U;
        return true;
    }
    if (op5 == 10U && size == 0U) {
        unsigned per = is_q ? 16U : 8U;
        for (unsigned k = 0U; k < per; k++) {
            unsigned dd = is_q ? ((d & ~1U) + (k >= 8U ? 1U : 0U)) & 31U : d & 31U;
            unsigned kk = is_q ? (k >= 8U ? k - 8U : k) : k;
            uint64_t sv = ctx->vfp_d[(is_q ? ((m & ~1U) + (k >= 8U ? 1U : 0U)) : m) & 31U];
            uint8_t v = (uint8_t)(sv >> (kk * 8U));
            unsigned c = 0U;
            for (unsigned b = 0U; b < 8U; b++) {
                c += (v >> b) & 1U;
            }
            uint64_t cur = ctx->vfp_d[dd];
            cur &= ~(0xFFULL << (kk * 8U));
            cur |= (uint64_t)(c & 0xFFU) << (kk * 8U);
            ctx->vfp_d[dd] = cur;
        }
        ctx->regs[15] = pc + 4U;
        return true;
    }
    if (op5 == 11U && size == 0U) {
        unsigned per = is_q ? 16U : 8U;
        for (unsigned k = 0U; k < per; k++) {
            unsigned dd = is_q ? ((d & ~1U) + (k >= 8U ? 1U : 0U)) & 31U : d & 31U;
            unsigned kk = is_q ? (k >= 8U ? k - 8U : k) : k;
            uint64_t sv = ctx->vfp_d[(is_q ? ((m & ~1U) + (k >= 8U ? 1U : 0U)) : m) & 31U];
            uint64_t v = (sv >> (kk * 8U)) & 0xFFULL;
            uint64_t cur = ctx->vfp_d[dd];
            cur &= ~(0xFFULL << (kk * 8U));
            cur |= (~v & 0xFFULL) << (kk * 8U);
            ctx->vfp_d[dd] = cur;
        }
        ctx->regs[15] = pc + 4U;
        return true;
    }
    if ((op5 == 12U || op5 == 13U) && size < 3U) {
        bool su = (op5 == 13U);
        unsigned per = is_q ? x_per_d(size) * 2U : x_per_d(size);
        for (unsigned k = 0U; k < per; k++) {
            unsigned sdd = is_q ? ((d & ~1U) + (k >= x_per_d(size) ? 1U : 0U)) & 31U : d & 31U;
            unsigned skk = is_q ? (k >= x_per_d(size) ? k - x_per_d(size) : k) : k;
            uint64_t sv = ctx->vfp_d[(is_q ? ((m & ~1U) + (k >= x_per_d(size) ? 1U : 0U)) : m) & 31U];
            uint64_t u = x_glane(sv, skk, size);
            int64_t e = su ? (int64_t)u : ((size == 0U) ? (int64_t)(int8_t)(uint8_t)u :
                (size == 1U) ? (int64_t)(int16_t)(uint16_t)u : (int64_t)(int32_t)(uint32_t)u);
            uint64_t dv = ctx->vfp_d[sdd];
            int64_t cur = (size == 0U) ? (int64_t)(int16_t)(dv >> (skk * 16U)) :
                (size == 1U) ? ((skk == 0U) ? (int64_t)(int32_t)(uint32_t)dv : (int64_t)(int32_t)(uint32_t)(dv >> 32)) : (int64_t)dv;
            int64_t s = cur + e;
            if (size == 0U) {
                s = x_sat_s(s, 16);
            } else if (size == 1U) {
                s = x_sat_s(s, 32);
            }
            if (size == 0U) {
                dv &= ~(0xFFFFULL << (skk * 16U));
                dv |= ((uint64_t)(uint16_t)s) << (skk * 16U);
            } else if (size == 1U) {
                if (skk == 0U) {
                    dv = (dv & 0xFFFFFFFF00000000ULL) | (uint32_t)s;
                } else {
                    dv = (dv & 0xFFFFFFFFULL) | ((uint64_t)(uint32_t)s << 32);
                }
            } else {
                dv = (uint64_t)s;
            }
            ctx->vfp_d[sdd] = dv;
        }
        ctx->regs[15] = pc + 4U;
        return true;
    }
    if ((op5 == 14U || op5 == 15U) && size < 3U) {
        bool neg = (op5 == 15U);
        unsigned per = is_q ? x_per_d(size) * 2U : x_per_d(size);
        for (unsigned k = 0U; k < per; k++) {
            unsigned dd = is_q ? ((d & ~1U) + (k >= x_per_d(size) ? 1U : 0U)) & 31U : d & 31U;
            unsigned kk = is_q ? (k >= x_per_d(size) ? k - x_per_d(size) : k) : k;
            uint64_t sv = ctx->vfp_d[(is_q ? ((m & ~1U) + (k >= x_per_d(size) ? 1U : 0U)) : m) & 31U];
            uint64_t u = x_glane(sv, kk, size);
            int64_t e = (size == 0U) ? (int64_t)(int8_t)(uint8_t)u :
                (size == 1U) ? (int64_t)(int16_t)(uint16_t)u : (int64_t)(int32_t)(uint32_t)u;
            int64_t mn = (size == 0U) ? -128LL : (size == 1U) ? -32768LL : -2147483648LL;
            int64_t mx = -mn - 1LL;
            int64_t v = neg ? ((e == mn) ? mx : -e) : ((e >= 0 || e == mn) ? ((e == mn) ? mx : e) : -e);
            uint64_t cur = ctx->vfp_d[dd];
            ctx->vfp_d[dd] = x_plane(cur, kk, size, (uint64_t)v);
        }
        ctx->regs[15] = pc + 4U;
        return true;
    }
    return false;
}
static bool x_perm(a32_ctx_t *ctx, uint32_t pc, uint32_t insn)
{
    if ((insn & 0xFFB00010U) != 0xF3B00000U) {
        return false;
    }
    bool is_q = ((insn >> 6) & 1U) != 0U;
    unsigned d = neon_reg_d(insn);
    unsigned m = neon_reg_m(insn);
    unsigned opc1 = (insn >> 16) & 3U;
    unsigned size = (insn >> 18) & 3U;
    unsigned op5 = (insn >> 7) & 31U;
    if (opc1 == 2U && op5 == 0U && size == 0U) {
        if (is_q) {
            uint64_t a0 = ctx->vfp_d[(d & ~1U) & 31U];
            uint64_t a1 = ctx->vfp_d[((d & ~1U) + 1U) & 31U];
            uint64_t b0 = ctx->vfp_d[(m & ~1U) & 31U];
            uint64_t b1 = ctx->vfp_d[(((m & ~1U)) + 1U) & 31U];
            ctx->vfp_d[(d & ~1U) & 31U] = b0;
            ctx->vfp_d[((d & ~1U) + 1U) & 31U] = b1;
            ctx->vfp_d[(m & ~1U) & 31U] = a0;
            ctx->vfp_d[(((m & ~1U)) + 1U) & 31U] = a1;
        } else {
            uint64_t a = ctx->vfp_d[d & 31U];
            ctx->vfp_d[d & 31U] = ctx->vfp_d[m & 31U];
            ctx->vfp_d[m & 31U] = a;
        }
        ctx->regs[15] = pc + 4U;
        return true;
    }
    if (opc1 == 2U && op5 >= 1U && op5 <= 3U && size < 3U) {
        unsigned per = is_q ? x_per_d(size) * 2U : x_per_d(size);
        unsigned hp = per / 2U;
        uint64_t nn[4] = { 0ULL, 0ULL, 0ULL, 0ULL };
        uint64_t mm[4] = { 0ULL, 0ULL, 0ULL, 0ULL };
        unsigned nregs = is_q ? 2U : 1U;
        for (unsigned r = 0U; r < nregs; r++) {
            nn[r] = ctx->vfp_d[((is_q ? (d & ~1U) : d) + r) & 31U];
            mm[r] = ctx->vfp_d[((is_q ? (m & ~1U) : m) + r) & 31U];
        }
        uint64_t nl[32];
        uint64_t ml[32];
        for (unsigned k = 0U; k < per; k++) {
            unsigned r = is_q ? (k >= x_per_d(size) ? 1U : 0U) : 0U;
            unsigned kk = is_q ? (k >= x_per_d(size) ? k - x_per_d(size) : k) : k;
            nl[k] = x_glane(nn[r], kk, size);
            ml[k] = x_glane(mm[r], kk, size);
        }
        uint64_t dl[32];
        uint64_t el[32];
        for (unsigned k = 0U; k < per; k++) {
            dl[k] = 0ULL;
            el[k] = 0ULL;
        }
        if (op5 == 1U) {
            for (unsigned k = 0U; k < hp; k++) {
                dl[2U * k] = nl[2U * k];
                dl[2U * k + 1U] = ml[2U * k];
                el[2U * k] = nl[2U * k + 1U];
                el[2U * k + 1U] = ml[2U * k + 1U];
            }
        } else if (op5 == 2U) {
            for (unsigned k = 0U; k < hp; k++) {
                dl[k] = nl[2U * k];
                dl[k + hp] = ml[2U * k];
                el[k] = nl[2U * k + 1U];
                el[k + hp] = ml[2U * k + 1U];
            }
        } else {
            for (unsigned k = 0U; k < hp; k++) {
                dl[2U * k] = nl[k];
                dl[2U * k + 1U] = ml[k];
                el[2U * k] = nl[k + hp];
                el[2U * k + 1U] = ml[k + hp];
            }
        }
        for (unsigned k = 0U; k < per; k++) {
            unsigned rd = is_q ? ((d & ~1U) + (k >= x_per_d(size) ? 1U : 0U)) & 31U : d & 31U;
            unsigned kk = is_q ? (k >= x_per_d(size) ? k - x_per_d(size) : k) : k;
            uint64_t cur = ctx->vfp_d[rd];
            cur = x_plane(cur, kk, size, dl[k]);
            ctx->vfp_d[rd] = cur;
            unsigned re = is_q ? ((m & ~1U) + (k >= x_per_d(size) ? 1U : 0U)) & 31U : m & 31U;
            cur = ctx->vfp_d[re];
            cur = x_plane(cur, kk, size, el[k]);
            ctx->vfp_d[re] = cur;
        }
        ctx->regs[15] = pc + 4U;
        return true;
    }
    return false;
}

static bool x_alu(a32_ctx_t *ctx, uint32_t pc, uint32_t insn)
{
    if ((insn & 0x0E000000U) != 0x02000000U) {
        return false;
    }
    bool u = ((insn >> 24) & 1U) != 0U;
    unsigned size = (insn >> 20) & 3U;
    unsigned op = (insn >> 8) & 0xFU;
    bool b4 = ((insn >> 4) & 1U) != 0U;
    bool is_q = ((insn >> 6) & 1U) != 0U;
    if ((insn & 0x00800000U) != 0U) {
        return false;
    }
    unsigned dd = neon_reg_d(insn);
    unsigned dn = neon_reg_n(insn);
    unsigned dm = neon_reg_m(insn);
    unsigned per = is_q ? x_per_d(size) * 2U : x_per_d(size);
    if (size > 2U && !(op == 0x0U && b4)) {
        return false;
    }
    if (!u && op == 0x9U && b4 && size < 3U) {
        for (unsigned k = 0U; k < per; k++) {
            unsigned d0 = is_q ? ((dd & ~1U) + (k >= x_per_d(size) ? 1U : 0U)) & 31U : dd & 31U;
            unsigned k0 = is_q ? (k >= x_per_d(size) ? k - x_per_d(size) : k) : k;
            uint64_t sv0 = ctx->vfp_d[(is_q ? ((dn & ~1U) + (k >= x_per_d(size) ? 1U : 0U)) : dn) & 31U];
            uint64_t sv1 = ctx->vfp_d[(is_q ? ((dm & ~1U) + (k >= x_per_d(size) ? 1U : 0U)) : dm) & 31U];
            uint64_t a = x_glane(sv0, k0, size);
            uint64_t b = x_glane(sv1, k0, size);
            uint64_t r = (size == 0U) ? (uint64_t)(uint8_t)((uint8_t)a * (uint8_t)b) :
                (size == 1U) ? (uint64_t)(uint16_t)((uint16_t)a * (uint16_t)b) :
                (uint64_t)(uint32_t)((uint32_t)a * (uint32_t)b);
            uint64_t cur = ctx->vfp_d[d0];
            ctx->vfp_d[d0] = x_plane(cur, k0, size, r);
        }
        ctx->regs[15] = pc + 4U;
        return true;
    }
    if ((!u && op == 0x9U && !b4) || (u && op == 0x9U && !b4)) {
        bool is_sub = u;
        for (unsigned k = 0U; k < per; k++) {
            unsigned d0 = is_q ? ((dd & ~1U) + (k >= x_per_d(size) ? 1U : 0U)) & 31U : dd & 31U;
            unsigned k0 = is_q ? (k >= x_per_d(size) ? k - x_per_d(size) : k) : k;
            uint64_t s0 = ctx->vfp_d[(is_q ? ((dn & ~1U) + (k >= x_per_d(size) ? 1U : 0U)) : dn) & 31U];
            uint64_t s1 = ctx->vfp_d[(is_q ? ((dm & ~1U) + (k >= x_per_d(size) ? 1U : 0U)) : dm) & 31U];
            uint64_t dv = ctx->vfp_d[d0];
            uint64_t a = x_glane(s0, k0, size);
            uint64_t b = x_glane(s1, k0, size);
            uint64_t p = 0ULL;
            if (size == 0U) {
                p = (uint8_t)((uint8_t)a * (uint8_t)b);
            } else if (size == 1U) {
                p = (uint16_t)((uint16_t)a * (uint16_t)b);
            } else {
                p = (uint32_t)((uint32_t)a * (uint32_t)b);
            }
            uint64_t cur = x_glane(dv, k0, size);
            uint64_t r;
            if (size == 0U) {
                r = (uint8_t)(is_sub ? (uint8_t)cur - (uint8_t)p : (uint8_t)cur + (uint8_t)p);
            } else if (size == 1U) {
                r = (uint16_t)(is_sub ? (uint16_t)cur - (uint16_t)p : (uint16_t)cur + (uint16_t)p);
            } else {
                r = is_sub ? (uint32_t)cur - (uint32_t)p : (uint32_t)cur + (uint32_t)p;
            }
            dv = x_plane(dv, k0, size, r);
            ctx->vfp_d[d0] = dv;
        }
        ctx->regs[15] = pc + 4U;
        return true;
    }
    if (op == 0x0U && b4) {
        for (unsigned k = 0U; k < per; k++) {
            unsigned d0 = is_q ? ((dd & ~1U) + (k >= x_per_d(size) ? 1U : 0U)) & 31U : dd & 31U;
            unsigned k0 = is_q ? (k >= x_per_d(size) ? k - x_per_d(size) : k) : k;
            uint64_t s0 = ctx->vfp_d[(is_q ? ((dn & ~1U) + (k >= x_per_d(size) ? 1U : 0U)) : dn) & 31U];
            uint64_t s1 = ctx->vfp_d[(is_q ? ((dm & ~1U) + (k >= x_per_d(size) ? 1U : 0U)) : dm) & 31U];
            int64_t a = (size == 0U) ? (int64_t)(int8_t)(uint8_t)x_glane(s0, k0, size) :
                (size == 1U) ? (int64_t)(int16_t)(uint16_t)x_glane(s0, k0, size) :
                (size == 2U) ? (int64_t)(int32_t)(uint32_t)x_glane(s0, k0, size) : (int64_t)x_glane(s0, k0, size);
            int64_t b = (size == 0U) ? (int64_t)(int8_t)(uint8_t)x_glane(s1, k0, size) :
                (size == 1U) ? (int64_t)(int16_t)(uint16_t)x_glane(s1, k0, size) :
                (size == 2U) ? (int64_t)(int32_t)(uint32_t)x_glane(s1, k0, size) : (int64_t)x_glane(s1, k0, size);
            if (u) {
                uint64_t ua = x_glane(s0, k0, size);
                uint64_t ub = x_glane(s1, k0, size);
                a = (int64_t)ua;
                b = (int64_t)ub;
            }
            unsigned bits = (size == 0U) ? 8U : (size == 1U) ? 16U : (size == 2U) ? 32U : 64U;
            int64_t s = a + b;
            uint64_t r = u ? x_sat_u(s, bits) : (uint64_t)x_sat_s(s, bits);
            uint64_t cur = ctx->vfp_d[d0];
            cur = x_plane(cur, k0, size, r);
            ctx->vfp_d[d0] = cur;
        }
        ctx->regs[15] = pc + 4U;
        return true;
    }
    return false;
}

bool i_vfp_neon_ext(a32_ctx_t *ctx, uint32_t pc, uint32_t insn, bool *faulted)
{
    if (x_vld_vst(ctx, pc, insn, faulted)) {
        return true;
    }
    if (x_misc(ctx, pc, insn)) {
        return true;
    }
    if (x_perm(ctx, pc, insn)) {
        return true;
    }
    if (x_alu(ctx, pc, insn)) {
        return true;
    }
    return i_vfp_neon_int(ctx, pc, insn);
}
