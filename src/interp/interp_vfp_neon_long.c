// Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
// SPDX-License-Identifier: Apache-2.0
// NEON integer long, widening, narrowing, and scalar operations.
#include "interp.h"
#include "interp_priv.h"

#include <fenv.h>
#include <math.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#ifndef NI_FPSCR_QC
#define NI_FPSCR_QC (1U << 27)
#endif
bool i_vfp_neon_long(a32_ctx_t *ctx, uint32_t pc, uint32_t insn);
static uint64_t ni_mask(unsigned b) { return (b >= 64U) ? ~0ULL : ((1ULL << b) - 1ULL); }
static uint64_t ni_g64(uint64_t w, unsigned l, unsigned b) { return (w >> (l * b)) & ni_mask(b); }
static uint64_t ni_g128(uint64_t lo, uint64_t hi, unsigned l, unsigned b)
{
	unsigned p = l * b;
	return (p < 64U) ? ((lo >> p) & ni_mask(b)) : ((hi >> (p - 64U)) & ni_mask(b));
}
static int64_t ni_sx(uint64_t v, unsigned b)
{
	uint64_t m = 1ULL << (b - 1U);
	return (int64_t)(((v & ni_mask(b)) ^ m) - m);
}
static int32_t ni_sa32(int32_t a, int32_t b, bool *s)
{
	int64_t t = (int64_t)a + (int64_t)b;
	if (t > INT32_MAX) { *s = true; return INT32_MAX; }
	if (t < INT32_MIN) { *s = true; return INT32_MIN; }
	return (int32_t)t;
}
static int32_t ni_ss32(int32_t a, int32_t b, bool *s)
{
	int64_t t = (int64_t)a - (int64_t)b;
	if (t > INT32_MAX) { *s = true; return INT32_MAX; }
	if (t < INT32_MIN) { *s = true; return INT32_MIN; }
	return (int32_t)t;
}
static int64_t ni_sa64(int64_t a, int64_t b, bool *s)
{
	if (b >= 0) {
		if (a > INT64_MAX - b) { *s = true; return INT64_MAX; }
	} else if (a < INT64_MIN - b) { *s = true; return INT64_MIN; }
	return a + b;
}
static int64_t ni_ss64(int64_t a, int64_t b, bool *s)
{
	if (b >= 0) {
		if (a < INT64_MIN + b) { *s = true; return INT64_MIN; }
	} else if (a > INT64_MAX + b) { *s = true; return INT64_MAX; }
	return a - b;
}
static uint16_t ni_p8(uint8_t a, uint8_t b)
{
	uint16_t r = 0U;
	for (unsigned i = 0U; i < 8U; i++) {
		if ((b >> i) & 1U) { r ^= (uint16_t)((uint16_t)a << i); }
	}
	return r;
}
static uint32_t ni_f16_to_f32(uint16_t h, bool dn)
{
	uint32_t s = (uint32_t)(h & 0x8000U) << 16;
	unsigned e = (h >> 10) & 0x1FU;
	uint32_t m = h & 0x03FFU;
	if (e == 0U) {
		if (m == 0U) { return s; }
		e = 1U;
		while ((m & 0x0400U) == 0U) { m <<= 1; e--; }
		m &= 0x03FFU;
		return s | ((e + 112U) << 23) | (m << 13);
	}
	if (e == 31U) {
		if (m == 0U) { return s | 0x7F800000U; }
		return dn ? 0x7FC00000U : (s | 0x7FC00000U | (m << 13));
	}
	return s | ((e + 112U) << 23) | (m << 13);
}
static uint16_t ni_f32_to_f16(uint32_t f, bool dn)
{
	uint16_t s = (uint16_t)((f >> 16) & 0x8000U);
	int e = (int)((f >> 23) & 0xFFU);
	uint32_t m = f & 0x7FFFFFU;
	if (e == 0) { return s; }
	if (e == 255) {
		if (m == 0U) { return (uint16_t)(s | 0x7C00U); }
		return dn ? 0x7E00U : (uint16_t)(s | 0x7E00U | (m >> 13));
	}
	int he = e - 112;
	if (he >= 31) { return (uint16_t)(s | 0x7C00U); }
	uint32_t sig = m | 0x800000U;
	if (he <= 0) {
		if (he < -10) { return s; }
		unsigned sh = (unsigned)(14 - he);
		uint32_t q = sig >> sh;
		uint32_t rem = sig & ((1U << sh) - 1U);
		uint32_t half = 1U << (sh - 1U);
		if (rem > half || (rem == half && (q & 1U) != 0U)) { q++; }
		return (uint16_t)(s + q);
	}
	uint32_t q = (sig >> 13) & 0x3FFU;
	uint32_t rem = sig & 0x1FFFU;
	uint16_t res = (uint16_t)(s | ((uint16_t)he << 10) | (uint16_t)q);
	if (rem > 0x1000U || (rem == 0x1000U && (q & 1U) != 0U)) { res++; }
	return res;
}
bool i_vfp_neon_long(a32_ctx_t *ctx, uint32_t pc, uint32_t insn)
{
	bool u_bit; unsigned size, op; bool is_q; unsigned dd, dn, dm;
	unsigned nbits, wbits;
	if ((insn & 0xFFB00010U) == 0xF3B00000U) {
		unsigned m_sz = (insn >> 18) & 3U;
		unsigned m_opc1 = (insn >> 16) & 3U;
		unsigned m_op5 = (insn >> 7) & 31U;
		bool m_q = ((insn >> 6) & 1U) != 0U;
		unsigned m_d = (((insn >> 22) & 1U) << 4) | ((insn >> 12) & 0xFU);
		unsigned m_m = (((insn >> 5) & 1U) << 4) | (insn & 0xFU);
		if (m_opc1 == 2U && m_sz == 1U && !m_q && (m_op5 == 12U || m_op5 == 14U)) {
			if (m_op5 == 12U) {
				if ((m_m & 1U) != 0U) { return false; }
				uint64_t q0 = ctx->vfp_d[m_m & 31U];
				uint64_t q1 = ctx->vfp_d[(m_m + 1U) & 31U];
				uint64_t out = 0ULL;
				for (unsigned i = 0U; i < 4U; i++) {
					uint32_t fw = (uint32_t)((i < 2U) ? (q0 >> (i * 32U)) : (q1 >> ((i - 2U) * 32U)));
					if ((fw & 0x7F800000U) == 0U) { fw &= 0x80000000U; }
					out |= (uint64_t)ni_f32_to_f16(fw, true) << (i * 16U);
				}
				ctx->vfp_d[m_d & 31U] = out;
			} else {
				if ((m_d & 1U) != 0U) { return false; }
				uint64_t dw = ctx->vfp_d[m_m & 31U];
				uint64_t o0 = 0ULL, o1 = 0ULL;
				for (unsigned i = 0U; i < 4U; i++) {
					uint32_t fw = ni_f16_to_f32((uint16_t)(dw >> (i * 16U)), true);
					if (i < 2U) { o0 |= (uint64_t)fw << (i * 32U); }
					else { o1 |= (uint64_t)fw << ((i - 2U) * 32U); }
				}
				ctx->vfp_d[m_d & 31U] = o0;
				ctx->vfp_d[(m_d + 1U) & 31U] = o1;
			}
			ctx->regs[15] = pc + 4U;
			return true;
		}
		return false;
	}
	if ((insn & 0xFE800010U) != 0xF2800000U) { return false; }
	u_bit = ((insn >> 24) & 1U) != 0U; size = (insn >> 20) & 3U;
	op = (insn >> 8) & 0xFU; is_q = ((insn >> 6) & 1U) != 0U;
	dd = (((insn >> 22) & 1U) << 4) | ((insn >> 12) & 0xFU);
	dn = (((insn >> 7) & 1U) << 4) | ((insn >> 16) & 0xFU);
	dm = (((insn >> 5) & 1U) << 4) | (insn & 0xFU);
	if (!is_q && size < 3U) {
		nbits = 8U << size; wbits = 16U << size;
		if (op == 0x4U || op == 0x6U) {
			uint64_t qn_lo, qn_hi, qm_lo, qm_hi, round, out = 0ULL;
			unsigned lanes = 128U / wbits;
			if (((dn & 1U) != 0U) || ((dm & 1U) != 0U)) { return false; }
			qn_lo = ctx->vfp_d[dn]; qn_hi = ctx->vfp_d[dn + 1U];
			qm_lo = ctx->vfp_d[dm]; qm_hi = ctx->vfp_d[dm + 1U];
			round = u_bit ? (1ULL << (nbits - 1U)) : 0ULL;
			for (unsigned i = 0U; i < lanes; i++) {
				uint64_t a = ni_g128(qn_lo, qn_hi, i, wbits);
				uint64_t b = ni_g128(qm_lo, qm_hi, i, wbits);
				uint64_t t = (op == 0x4U) ? (a + b + round) : (a - b + round);
				out |= ((t >> nbits) & ni_mask(nbits)) << (i * nbits);
			}
			ctx->vfp_d[dd] = out; ctx->regs[15] = pc + 4U; return true;
		}
		if (op == 0x1U || op == 0x3U) {
			uint64_t dmw, qn_lo, qn_hi, o_lo = 0ULL, o_hi = 0ULL, wm = ni_mask(wbits);
			unsigned lanes = 64U / nbits;
			if (((dd & 1U) != 0U) || ((dn & 1U) != 0U)) { return false; }
			dmw = ctx->vfp_d[dm]; qn_lo = ctx->vfp_d[dn]; qn_hi = ctx->vfp_d[dn + 1U];
			for (unsigned i = 0U; i < lanes; i++) {
				uint64_t bv = ni_g64(dmw, i, nbits);
				uint64_t be = u_bit ? bv : ((uint64_t)ni_sx(bv, nbits) & wm);
				uint64_t av = ni_g128(qn_lo, qn_hi, i, wbits);
				uint64_t rv = (op == 0x1U) ? ((av + be) & wm) : ((av - be) & wm);
				if ((i * wbits) < 64U) { o_lo |= rv << (i * wbits); }
				else { o_hi |= rv << ((i * wbits) - 64U); }
			}
			ctx->vfp_d[dd] = o_lo; ctx->vfp_d[dd + 1U] = o_hi;
			ctx->regs[15] = pc + 4U; return true;
		}
		if (op == 0xEU && !u_bit && size == 2U) {
			uint64_t a, b, lo = 0ULL, hi = 0ULL;
			if ((dd & 1U) != 0U) { return false; }
			a = ctx->vfp_d[dn]; b = ctx->vfp_d[dm];
			for (unsigned i = 0U; i < 64U; i++) {
				if (((b >> i) & 1ULL) != 0ULL) {
					lo ^= (i == 0U) ? a : (a << i);
					hi ^= (i == 0U) ? 0ULL : (a >> (64U - i));
				}
			}
			ctx->vfp_d[dd] = lo; ctx->vfp_d[dd + 1U] = hi;
			ctx->regs[15] = pc + 4U; return true;
		}
		if ((dd & 1U) != 0U) { return false; }
		if (op == 0xEU) {
			uint64_t dnw, dmw, o_lo = 0ULL, o_hi = 0ULL;
			if (u_bit || size != 0U) { return false; }
			dnw = ctx->vfp_d[dn]; dmw = ctx->vfp_d[dm];
			for (unsigned i = 0U; i < 8U; i++) {
				uint64_t r = (uint64_t)ni_p8((uint8_t)ni_g64(dnw, i, 8U), (uint8_t)ni_g64(dmw, i, 8U));
				if (i < 4U) { o_lo |= r << (i * 16U); } else { o_hi |= r << ((i * 16U) - 64U); }
			}
			ctx->vfp_d[dd] = o_lo; ctx->vfp_d[dd + 1U] = o_hi;
			ctx->regs[15] = pc + 4U; return true;
		}
		if (op == 0x9U || op == 0xBU || op == 0xDU) {
			uint64_t dnw, dmw, ql, qh, wm = ni_mask(wbits), o_lo = 0ULL, o_hi = 0ULL;
			unsigned lanes = 8U >> size;
			int64_t nmin = (nbits == 16U) ? -32768LL : (int64_t)INT32_MIN;
			int64_t smax = (wbits == 32U) ? (int64_t)INT32_MAX : INT64_MAX;
			if (u_bit || (size != 1U && size != 2U)) { return false; }
			dnw = ctx->vfp_d[dn]; dmw = ctx->vfp_d[dm];
			ql = ctx->vfp_d[dd]; qh = ctx->vfp_d[dd + 1U];
			for (unsigned i = 0U; i < lanes; i++) {
				int64_t sa = ni_sx(ni_g64(dnw, i, nbits), nbits);
				int64_t sb = ni_sx(ni_g64(dmw, i, nbits), nbits);
				int64_t qv = ni_sx(ni_g128(ql, qh, i, wbits), wbits);
				int64_t prod = ((sa == nmin) && (sb == nmin)) ? smax : (2LL * sa * sb);
				uint64_t rv;
				if (sa == nmin && sb == nmin) { ctx->fpscr |= NI_FPSCR_QC; }
				if (op == 0xDU) { rv = (uint64_t)prod & wm; }
				else {
					bool as = false; int64_t acc;
					if (wbits == 32U) {
						int32_t r32 = (op == 0x9U) ? ni_sa32((int32_t)qv, (int32_t)prod, &as)
						                           : ni_ss32((int32_t)qv, (int32_t)prod, &as);
						acc = (int64_t)r32;
					} else {
						acc = (op == 0x9U) ? ni_sa64(qv, prod, &as) : ni_ss64(qv, prod, &as);
					}
					if (as) { ctx->fpscr |= NI_FPSCR_QC; }
					rv = (uint64_t)acc & wm;
				}
				if ((i * wbits) < 64U) { o_lo |= rv << (i * wbits); }
				else { o_hi |= rv << ((i * wbits) - 64U); }
			}
			ctx->vfp_d[dd] = o_lo; ctx->vfp_d[dd + 1U] = o_hi;
			ctx->regs[15] = pc + 4U; return true;
		}
		if (op == 0x0U || op == 0x2U || op == 0x5U || op == 0x7U || op == 0x8U || op == 0xAU ||
		    op == 0xCU) {
			uint64_t dnw = ctx->vfp_d[dn], dmw = ctx->vfp_d[dm];
			uint64_t ql = ctx->vfp_d[dd], qh = ctx->vfp_d[dd + 1U];
			uint64_t wm = ni_mask(wbits), o_lo = 0ULL, o_hi = 0ULL;
			unsigned lanes = 8U >> size;
			for (unsigned i = 0U; i < lanes; i++) {
				uint64_t av = ni_g64(dnw, i, nbits), bv = ni_g64(dmw, i, nbits);
				uint64_t qv = ni_g128(ql, qh, i, wbits), rv;
				if (!u_bit) {
					int64_t sa = ni_sx(av, nbits), sb = ni_sx(bv, nbits);
					if (op == 0x0U) { rv = (uint64_t)(sa + sb) & wm; }
					else if (op == 0x2U) { rv = (uint64_t)(sa - sb) & wm; }
					else if (op == 0x5U || op == 0x7U) {
						int64_t d = sa - sb; uint64_t mg = (uint64_t)(d >= 0 ? d : -d);
						rv = ((op == 0x5U) ? (qv + mg) : mg) & wm;
					} else if (op == 0x8U) { rv = (qv + (uint64_t)(sa * sb)) & wm; }
					else if (op == 0xAU) { rv = (qv - (uint64_t)(sa * sb)) & wm; }
					else { rv = (uint64_t)(sa * sb) & wm; }
				} else {
					if (op == 0x0U) { rv = (av + bv) & wm; }
					else if (op == 0x2U) { rv = (av - bv) & wm; }
					else if (op == 0x5U || op == 0x7U) {
						uint64_t mg = (av >= bv) ? (av - bv) : (bv - av);
						rv = ((op == 0x5U) ? (qv + mg) : mg) & wm;
					} else if (op == 0x8U) { rv = (qv + av * bv) & wm; }
					else if (op == 0xAU) { rv = (qv - av * bv) & wm; }
					else { rv = (av * bv) & wm; }
				}
				if ((i * wbits) < 64U) { o_lo |= rv << (i * wbits); }
				else { o_hi |= rv << ((i * wbits) - 64U); }
			}
			ctx->vfp_d[dd] = o_lo; ctx->vfp_d[dd + 1U] = o_hi;
			ctx->regs[15] = pc + 4U; return true;
		}
		return false;
	}
	if (is_q && (size == 1U || size == 2U)) {
		unsigned sc_dm, sc_lane;
		uint64_t sc_val;
		nbits = 8U << size; wbits = 16U << size;
		sc_dm = (size == 1U) ? (insn & 7U) : (insn & 0xFU);
		sc_lane = (size == 1U) ? ((((insn >> 5) & 1U) << 1) | ((insn >> 3) & 1U)) : ((insn >> 5) & 1U);
		sc_val = ni_g64(ctx->vfp_d[sc_dm], sc_lane, nbits);
		if (op == 0x2U || op == 0x3U || op == 0x6U || op == 0x7U || op == 0xAU || op == 0xBU) {
			uint64_t dnw, ql, qh, wm = ni_mask(wbits), o_lo = 0ULL, o_hi = 0ULL;
			unsigned lanes = 64U / nbits;
			int64_t nmin = (nbits == 16U) ? -32768LL : (int64_t)INT32_MIN;
			int64_t smax = (wbits == 32U) ? (int64_t)INT32_MAX : INT64_MAX;
			if ((dd & 1U) != 0U) { return false; }
			if ((op == 0x3U || op == 0x7U || op == 0xBU) && u_bit) { return false; }
			dnw = ctx->vfp_d[dn]; ql = ctx->vfp_d[dd]; qh = ctx->vfp_d[dd + 1U];
			for (unsigned i = 0U; i < lanes; i++) {
				uint64_t av = ni_g64(dnw, i, nbits);
				uint64_t qv = ni_g128(ql, qh, i, wbits), rv;
				if (!u_bit) {
					int64_t sa = ni_sx(av, nbits), ss = ni_sx(sc_val, nbits);
					int64_t qs = ni_sx(qv, wbits);
					if (op == 0x2U) { rv = (uint64_t)(qs + sa * ss) & wm; }
					else if (op == 0x6U) { rv = (uint64_t)(qs - sa * ss) & wm; }
					else if (op == 0xAU) { rv = (uint64_t)(sa * ss) & wm; }
					else {
						int64_t prod = ((sa == nmin) && (ss == nmin)) ? smax : (2LL * sa * ss);
						if (sa == nmin && ss == nmin) { ctx->fpscr |= NI_FPSCR_QC; }
						if (op == 0xBU) { rv = (uint64_t)prod & wm; }
						else {
							bool as = false; int64_t acc;
							if (wbits == 32U) {
								int32_t r32 = (op == 0x3U)
								    ? ni_sa32((int32_t)qs, (int32_t)prod, &as)
								    : ni_ss32((int32_t)qs, (int32_t)prod, &as);
								acc = (int64_t)r32;
							} else {
								acc = (op == 0x3U) ? ni_sa64(qs, prod, &as) : ni_ss64(qs, prod, &as);
							}
							if (as) { ctx->fpscr |= NI_FPSCR_QC; }
							rv = (uint64_t)acc & wm;
						}
					}
				} else {
					if (op == 0x2U) { rv = (qv + av * sc_val) & wm; }
					else if (op == 0x6U) { rv = (qv - av * sc_val) & wm; }
					else if (op == 0xAU) { rv = (av * sc_val) & wm; }
					else { return false; }
				}
				if ((i * wbits) < 64U) { o_lo |= rv << (i * wbits); }
				else { o_hi |= rv << ((i * wbits) - 64U); }
			}
			ctx->vfp_d[dd] = o_lo; ctx->vfp_d[dd + 1U] = o_hi;
			ctx->regs[15] = pc + 4U; return true;
		}
		if (op == 0x0U || op == 0x4U || op == 0x8U || op == 0xCU || op == 0xDU) {
			bool vec_q = u_bit;
			uint64_t sl, sh = 0ULL, dl, dh = 0ULL, o_lo = 0ULL, o_hi = 0ULL, nm = ni_mask(nbits);
			unsigned lanes;
			int64_t ss = ni_sx(sc_val, nbits);
			int64_t smin = (nbits == 16U) ? -32768LL : (int64_t)INT32_MIN;
			int64_t smax = (nbits == 16U) ? 32767LL : (int64_t)INT32_MAX;
			if (vec_q && (((dd & 1U) != 0U) || ((dn & 1U) != 0U))) { return false; }
			sl = ctx->vfp_d[dn]; dl = ctx->vfp_d[dd];
			if (vec_q) { sh = ctx->vfp_d[dn + 1U]; dh = ctx->vfp_d[dd + 1U]; lanes = 128U / nbits; }
			else { lanes = 64U / nbits; }
			for (unsigned i = 0U; i < lanes; i++) {
				uint64_t av = vec_q ? ni_g128(sl, sh, i, nbits) : ni_g64(sl, i, nbits);
				uint64_t dv = vec_q ? ni_g128(dl, dh, i, nbits) : ni_g64(dl, i, nbits);
				uint64_t rv;
				if (op == 0x0U) { rv = (dv + av * sc_val) & nm; }
				else if (op == 0x4U) { rv = (dv - av * sc_val) & nm; }
				else if (op == 0x8U) { rv = (av * sc_val) & nm; }
				else {
					int64_t sa = ni_sx(av, nbits);
					if (sa == smin && ss == smin) {
						ctx->fpscr |= NI_FPSCR_QC; rv = (uint64_t)smax & nm;
					} else {
						int64_t t = 2LL * sa * ss;
						if (op == 0xDU) { t += (1LL << (nbits - 1U)); }
						rv = (uint64_t)(t >> nbits) & nm;
					}
				}
				if (!vec_q) { o_lo |= rv << (i * nbits); }
				else if ((i * nbits) < 64U) { o_lo |= rv << (i * nbits); }
				else { o_hi |= rv << ((i * nbits) - 64U); }
			}
			if (vec_q) { ctx->vfp_d[dd] = o_lo; ctx->vfp_d[dd + 1U] = o_hi; }
			else { ctx->vfp_d[dd] = o_lo; }
			ctx->regs[15] = pc + 4U; return true;
		}
		return false;
	}
	return false;
}

bool i_vfp_fxcvt(a32_ctx_t *ctx, uint32_t pc, uint32_t insn)
{
	bool dbl = (((insn >> 8) & 0xFU) == 11U);
	unsigned opc2 = (insn >> 16) & 0xFU;
	bool d_bit = ((insn >> 22) & 1U) != 0U;
	unsigned vd_b = (insn >> 12) & 0xFU;
	bool n_bit = ((insn >> 7) & 1U) != 0U;
	bool m_bit = ((insn >> 5) & 1U) != 0U;
	unsigned vm_b = insn & 0xFU;
	if (!dbl && (opc2 == 2U || opc2 == 3U)) {
		unsigned sd = (vd_b << 1) | (d_bit ? 1U : 0U);
		unsigned sm = (vm_b << 1) | (m_bit ? 1U : 0U);
		uint32_t *sw = (uint32_t *)(void *)ctx->vfp_d;
		bool dn = ((ctx->fpscr >> 25) & 1U) != 0U;
		if (opc2 == 2U) {
			uint16_t h = (uint16_t)((sw[sm] >> (n_bit ? 16U : 0U)) & 0xFFFFU);
			sw[sd] = ni_f16_to_f32(h, dn);
		} else {
			uint32_t fw = sw[sm];
			if (((ctx->fpscr >> 24) & 1U) != 0U && (fw & 0x7F800000U) == 0U) {
				fw &= 0x80000000U;
			}
			uint16_t h = ni_f32_to_f16(fw, dn);
			sw[sd] = n_bit ? ((sw[sd] & 0x0000FFFFU) | ((uint32_t)h << 16))
			               : ((sw[sd] & 0xFFFF0000U) | (uint32_t)h);
		}
		ctx->regs[15] = pc + 4U;
		return true;
	}
	if (opc2 != 10U && opc2 != 11U && opc2 != 14U && opc2 != 15U) {
		return false;
	}
	bool to_fixed = (opc2 >= 14U);
	bool is_unsigned = ((opc2 & 1U) != 0U);
	unsigned int_bits = n_bit ? 32U : 16U;
	unsigned imm5_raw = (vm_b << 1) | (m_bit ? 1U : 0U);
	if (imm5_raw >= int_bits) {
		return false;
	}
	unsigned fbits = int_bits - imm5_raw;
	unsigned d_idx = dbl ? ((d_bit ? 16U : 0U) | vd_b) : ((vd_b << 1) | (d_bit ? 1U : 0U));
	uint32_t *s_words = (uint32_t *)(void *)ctx->vfp_d;

	if (!to_fixed) {
		uint32_t raw = dbl ? (uint32_t)ctx->vfp_d[d_idx] : s_words[d_idx];
		double iv;
		if (!n_bit) {
			iv = is_unsigned ? (double)(raw & 0xFFFFU) : (double)(int16_t)(uint16_t)raw;
		} else {
			iv = is_unsigned ? (double)raw : (double)(int32_t)raw;
		}
		fenv_t env;
		(void)feholdexcept(&env);
		unsigned rm = (ctx->fpscr >> 22) & 3U;
		(void)fesetround(rm == 0U ? FE_TONEAREST : rm == 1U ? FE_UPWARD : rm == 2U ? FE_DOWNWARD : FE_TOWARDZERO);
		if (dbl) {
			double out_d = ldexp(iv, -(int)fbits);
			if (fetestexcept(FE_INEXACT) != 0) { ctx->fpscr |= (1U << 4); }
			memcpy(&ctx->vfp_d[d_idx], &out_d, 8U);
		} else {
			float out_f = (float)ldexp(iv, -(int)fbits);
			if (fetestexcept(FE_INEXACT) != 0) { ctx->fpscr |= (1U << 4); }
			memcpy(&s_words[d_idx], &out_f, 4U);
		}
		(void)fesetenv(&env);
		ctx->regs[15] = pc + 4U;
		return true;
	}

	double v;
	if (dbl) {
		memcpy(&v, &ctx->vfp_d[d_idx], 8U);
		uint64_t bw = ctx->vfp_d[d_idx];
		if ((ctx->fpscr & (1U << 24)) != 0U && ((bw & 0x7FF0000000000000ULL) == 0ULL) && ((bw & 0x000FFFFFFFFFFFFFULL) != 0ULL)) {
			ctx->fpscr |= (1U << 7);
			v = (bw >> 63) ? -0.0 : 0.0;
		}
	} else {
		float fv = 0.0f;
		uint32_t bw = s_words[d_idx];
		memcpy(&fv, &bw, 4U);
		if ((ctx->fpscr & (1U << 24)) != 0U && ((bw & 0x7F800000U) == 0U) && ((bw & 0x007FFFFFU) != 0U)) {
			ctx->fpscr |= (1U << 7);
			fv = (bw >> 31) ? -0.0f : 0.0f;
		}
		v = (double)fv;
	}

	uint32_t out32 = 0U;
	if (isnan(v)) {
		ctx->fpscr |= 1U;
		out32 = 0U;
	} else {
		double sv = isfinite(v) ? ldexp(v, (int)fbits) : v;
		double tr = isfinite(sv) ? trunc(sv) : sv;
		double hi = is_unsigned ? (double)((int_bits == 32U) ? 0xFFFFFFFFULL : 0xFFFFULL)
		                        : (double)((1ULL << (int_bits - 1U)) - 1ULL);
		double lo = is_unsigned ? 0.0 : -(double)(1ULL << (int_bits - 1U));
		if (!isfinite(sv) || tr > hi) {
			ctx->fpscr |= 1U;
			out32 = (uint32_t)(is_unsigned ? (uint64_t)hi : (uint64_t)(int64_t)hi);
		} else if (tr < lo) {
			ctx->fpscr |= 1U;
			out32 = is_unsigned ? 0U : (uint32_t)(int32_t)(int64_t)lo;
		} else {
			if (tr != sv) { ctx->fpscr |= (1U << 4); }
			out32 = is_unsigned ? (uint32_t)(uint64_t)tr : (uint32_t)(int32_t)(int64_t)tr;
		}
	}
	if (dbl) {
		ctx->vfp_d[d_idx] = is_unsigned ? (uint64_t)out32 : (uint64_t)(int64_t)(int32_t)out32;
	} else {
		s_words[d_idx] = out32;
	}
	ctx->regs[15] = pc + 4U;
	return true;
}
