/*
 * src/vfp_neon.c — VFPv4-D32 and NEON instruction decoder & AArch64 lowering
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "a64_emit.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static uint32_t vfp_expand_imm_single(uint8_t imm8)
{
    uint32_t a = ((uint32_t)imm8 >> 7) & 1U;
    uint32_t b = ((uint32_t)imm8 >> 6) & 1U;
    uint32_t c = ((uint32_t)imm8 >> 5) & 1U;
    uint32_t d = ((uint32_t)imm8 >> 4) & 1U;
    uint32_t efgh = (uint32_t)imm8 & 0xFU;
    uint32_t exp = ((1U - b) << 7) | (b << 6) | (b << 5) | (b << 4) | (b << 3) |
                   (b << 2) | (c << 1) | d;
    uint32_t frac = efgh << 19;
    return (a << 31) | (exp << 23) | frac;
}

static uint64_t vfp_expand_imm_double(uint8_t imm8)
{
    uint64_t a = ((uint64_t)imm8 >> 7) & 1ULL;
    uint64_t b = ((uint64_t)imm8 >> 6) & 1ULL;
    uint64_t c = ((uint64_t)imm8 >> 5) & 1ULL;
    uint64_t d = ((uint64_t)imm8 >> 4) & 1ULL;
    uint64_t efgh = (uint64_t)imm8 & 0xFULL;
    uint64_t exp = ((1ULL - b) << 10) | (((b ? 0xFFULL : 0ULL) & 0xFFULL) << 2) |
                   (c << 1) | d;
    uint64_t frac = efgh << 48;
    return (a << 63) | (exp << 52) | frac;
}

static bool neon_expand_imm(uint8_t cmode, uint8_t op, uint8_t imm8, uint64_t *out_val64)
{
    uint32_t w = 0U;
    if ((cmode & 0xEU) == 0U) { /* 000x: 32-bit 000000imm8 */
        w = imm8;
        *out_val64 = ((uint64_t)w << 32) | w;
        return true;
    }
    if ((cmode & 0xEU) == 2U) { /* 001x: 32-bit 0000imm800 */
        w = (uint32_t)imm8 << 8;
        *out_val64 = ((uint64_t)w << 32) | w;
        return true;
    }
    if ((cmode & 0xEU) == 4U) { /* 010x: 32-bit 00imm80000 */
        w = (uint32_t)imm8 << 16;
        *out_val64 = ((uint64_t)w << 32) | w;
        return true;
    }
    if ((cmode & 0xEU) == 6U) { /* 011x: 32-bit imm8000000 */
        w = (uint32_t)imm8 << 24;
        *out_val64 = ((uint64_t)w << 32) | w;
        return true;
    }
    if ((cmode & 0xEU) == 8U) { /* 100x: 16-bit 00imm8 */
        uint32_t h = imm8;
        w = (h << 16) | h;
        *out_val64 = ((uint64_t)w << 32) | w;
        return true;
    }
    if ((cmode & 0xEU) == 10U) { /* 101x: 16-bit imm800 */
        uint32_t h = (uint32_t)imm8 << 8;
        w = (h << 16) | h;
        *out_val64 = ((uint64_t)w << 32) | w;
        return true;
    }
    if (cmode == 12U) { /* 1100: 32-bit 0000imm8FF */
        w = ((uint32_t)imm8 << 8) | 0xFFU;
        *out_val64 = ((uint64_t)w << 32) | w;
        return true;
    }
    if (cmode == 13U) { /* 1101: 32-bit 00imm8FFFF */
        w = ((uint32_t)imm8 << 16) | 0xFFFFU;
        *out_val64 = ((uint64_t)w << 32) | w;
        return true;
    }
    if (cmode == 14U) { /* 1110 */
        if (op == 0U) {
            uint32_t b = imm8;
            w = (b << 24) | (b << 16) | (b << 8) | b;
            *out_val64 = ((uint64_t)w << 32) | w;
            return true;
        }
        uint64_t val = 0ULL;
        for (unsigned i = 0U; i < 8U; i++) {
            if ((imm8 >> i) & 1U) {
                val |= (0xFFULL << (i * 8U));
            }
        }
        *out_val64 = val;
        return true;
    }
    if (cmode == 15U && op == 0U) { /* 1111: float imm */
        w = vfp_expand_imm_single(imm8);
        *out_val64 = ((uint64_t)w << 32) | w;
        return true;
    }
    return false;
}

static bool decode_vfp(a64_buf_t *b, uint32_t insn, uint32_t pc)
{
    unsigned cp_num = (insn >> 8) & 0xFU;
    if (cp_num != 10U && cp_num != 11U) {
        return false;
    }
    bool is_double = (cp_num == 11U);

    /* 1. Two core registers <-> VFP registers: VMOV Rt, Rt2, Dm or Sm, Sm1 */
    if ((insn & 0x0FE000D0U) == 0x0C400010U) {
        bool l_bit = ((insn >> 20) & 1U) != 0U;
        unsigned rt2 = (insn >> 16) & 0xFU;
        unsigned rt  = (insn >> 12) & 0xFU;
        uint32_t off;
        if (!is_double) {
            unsigned sm = ((insn & 0xFU) << 1) | ((insn >> 5) & 1U);
            off = A32_CTX_VFP_OFFSET + (sm * 4U);
        } else {
            unsigned dm = (((insn >> 5) & 1U) << 4) | (insn & 0xFU);
            off = A32_CTX_VFP_OFFSET + (dm * 8U);
        }
        if (l_bit) {
            a64_emit_ldr_imm_w(b, rt,  A64_XCTX, off);
            a64_emit_ldr_imm_w(b, rt2, A64_XCTX, off + 4U);
        } else {
            a64_emit_str_imm_w(b, rt,  A64_XCTX, off);
            a64_emit_str_imm_w(b, rt2, A64_XCTX, off + 4U);
        }
        return true;
    }

    /* 2. Core register <-> Single / Scalar move, or VMRS / VMSR, or VDUP core */
    if ((insn & 0x0F000010U) == 0x0E000010U) {
        unsigned opc1 = (insn >> 21) & 7U;
        bool l_bit    = ((insn >> 20) & 1U) != 0U;
        unsigned rt   = (insn >> 12) & 0xFU;

        /* VMRS / VMSR (opc1 == 7, cp10) */
        if (opc1 == 7U && !is_double) {
            if (l_bit) {
                if (rt == 15U) { /* APSR_nzcv */
                    a64_emit_ldr_imm_w(b, A64_TMP0, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, fpscr));
                    a64_emit_msr_nzcv(b, A64_TMP0);
                } else {
                    a64_emit_ldr_imm_w(b, rt, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, fpscr));
                }
            } else {
                a64_emit_str_imm_w(b, rt, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, fpscr));
            }
            return true;
        }

        /* VMOV Sn, Rt / VMOV Rt, Sn */
        if (opc1 == 0U && !is_double) {
            unsigned sn = (((insn >> 16) & 0xFU) << 1) | ((insn >> 7) & 1U);
            uint32_t off = A32_CTX_VFP_OFFSET + (sn * 4U);
            if (l_bit) {
                a64_emit_ldr_imm_w(b, rt, A64_XCTX, off);
            } else {
                a64_emit_str_imm_w(b, rt, A64_XCTX, off);
            }
            return true;
        }

        /* VDUP from core register (bits 11..8 = cp11, bit 23 = 1, bit 20 = 0, bit 6 = 0, bit 4 = 1) */
        if (is_double && !l_bit && ((insn & 0x00800040U) == 0x00800000U)) {
            bool b_bit = ((insn >> 22) & 1U) != 0U;
            bool q_bit = ((insn >> 21) & 1U) != 0U;
            bool e_bit = ((insn >> 5) & 1U) != 0U;
            unsigned size = b_bit ? 0U : (e_bit ? 1U : 2U);
            unsigned vd = (((insn >> 7) & 1U) << 4) | ((insn >> 16) & 0xFU);
            if (q_bit) {
                vd >>= 1;
            }
            uint32_t a64_insn = (q_bit ? 0x4E000C00U : 0x0E000C00U) |
                                ((1U << size) << 16) | ((rt & 31U) << 5) | 0U;
            a64_emit(b, a64_insn);
            if (q_bit) {
                a64_emit_str_q_ctx(b, 0U, vd);
            } else {
                a64_emit_str_d_ctx(b, 0U, vd);
            }
            return true;
        }

        /* VMOV Dm[x], Rt / VMOV Rt, Dm[x] (32-bit, 16-bit, or 8-bit lane) */
        if (is_double && (l_bit || ((insn & 0x00800000U) == 0U))) {
            unsigned opc1_lo2 = (insn >> 21) & 3U;
            unsigned opc2     = (insn >> 5) & 3U;
            unsigned dm       = (((insn >> 7) & 1U) << 4) | ((insn >> 16) & 0xFU);
            bool is_u         = ((insn >> 23) & 1U) != 0U;

            if ((opc1_lo2 & 2U) == 0U && opc2 == 0U && !is_u) {
                /* 32-bit lane */
                unsigned index = opc1_lo2 & 1U;
                uint32_t off = A32_CTX_VFP_OFFSET + (dm * 8U) + (index ? 4U : 0U);
                if (l_bit) {
                    a64_emit_ldr_imm_w(b, rt, A64_XCTX, off);
                } else {
                    a64_emit_str_imm_w(b, rt, A64_XCTX, off);
                }
                return true;
            }
            if ((opc1_lo2 & 2U) != 0U || (opc2 & 1U) != 0U) {
                /* 8-bit lane (opc1<1> == 1) or 16-bit lane (opc1<1> == 0, opc2<0> == 1) */
                bool is_byte = (opc1_lo2 & 2U) != 0U;
                unsigned index = is_byte ? (((opc1_lo2 & 1U) << 2) | opc2)
                                         : (((opc1_lo2 & 1U) << 1) | ((opc2 >> 1) & 1U));
                unsigned imm5 = is_byte ? ((index << 1) | 1U) : ((index << 2) | 2U);
                if (l_bit) {
                    a64_emit_ldr_d_ctx(b, 1U, dm);
                    uint32_t a64_insn = (is_u ? 0x0E003C00U : 0x0E002C00U) |
                                        (imm5 << 16) | (1U << 5) | (rt & 31U);
                    a64_emit(b, a64_insn);
                } else {
                    a64_emit_ldr_d_ctx(b, 0U, dm);
                    uint32_t a64_insn = 0x4E001C00U | (imm5 << 16) |
                                        ((rt & 31U) << 5) | 0U;
                    a64_emit(b, a64_insn);
                    a64_emit_str_d_ctx(b, 0U, dm);
                }
                return true;
            }
        }
    }

    /* 3. VFP loads / stores (VLDR, VSTR, VLDM, VSTM, VPUSH, VPOP) */
    if ((insn & 0x0E000000U) == 0x0C000000U) {
        bool p_bit = ((insn >> 24) & 1U) != 0U;
        bool u_bit = ((insn >> 23) & 1U) != 0U;
        bool d_bit = ((insn >> 22) & 1U) != 0U;
        bool w_bit = ((insn >> 21) & 1U) != 0U;
        bool l_bit = ((insn >> 20) & 1U) != 0U;
        unsigned rn = (insn >> 16) & 0xFU;
        unsigned vd_bits = (insn >> 12) & 0xFU;
        uint32_t imm8 = insn & 0xFFU;

        /* VLDR / VSTR */
        if (p_bit && !w_bit) {
            uint32_t byte_disp = imm8 * 4U;
            unsigned base_reg = (rn == 15U) ? A64_TMP0 : rn;
            if (rn == 15U) {
                a64_emit_mov_imm32(b, A64_TMP0, pc + 8U);
            }
            if (u_bit) {
                a64_emit_add_imm_w(b, false, A64_ADDR, base_reg, (uint16_t)byte_disp, 0U);
            } else {
                a64_emit_sub_imm_w(b, false, A64_ADDR, base_reg, (uint16_t)byte_disp, 0U);
            }

            if (is_double) {
                unsigned d = (d_bit << 4) | vd_bits;
                if (l_bit) {
                    a64_emit_guest_ldr_d(b, 0U, A64_ADDR);
                    a64_emit_str_d_ctx(b, 0U, d);
                } else {
                    a64_emit_ldr_d_ctx(b, 0U, d);
                    a64_emit_guest_str_d(b, 0U, A64_ADDR);
                }
            } else {
                unsigned s = (vd_bits << 1) | d_bit;
                if (l_bit) {
                    a64_emit_guest_ldr_s(b, 0U, A64_ADDR);
                    a64_emit_str_s_ctx(b, 0U, s);
                } else {
                    a64_emit_ldr_s_ctx(b, 0U, s);
                    a64_emit_guest_str_s(b, 0U, A64_ADDR);
                }
            }
            return true;
        }

        /* VLDM / VSTM / VPUSH / VPOP */
        if (imm8 == 0U || p_bit == u_bit) {
            return false;
        }
        uint32_t total_bytes = imm8 * 4U;
        unsigned base_reg = (rn == 15U) ? A64_TMP0 : rn;
        if (rn == 15U) {
            a64_emit_mov_imm32(b, A64_TMP0, pc + 8U);
        }
        if (!u_bit) {
            a64_emit_sub_imm_w(b, false, A64_ADDR, base_reg, (uint16_t)total_bytes, 0U);
        } else {
            a64_emit_mov_w(b, A64_ADDR, base_reg);
        }

        if (is_double) {
            unsigned d_start = (d_bit << 4) | vd_bits;
            unsigned num_d = imm8 / 2U;
            for (unsigned i = 0U; i < num_d; i++) {
                unsigned reg = (d_start + i) & 31U;
                if (l_bit) {
                    a64_emit_guest_ldr_d(b, 0U, A64_ADDR);
                    a64_emit_str_d_ctx(b, 0U, reg);
                } else {
                    a64_emit_ldr_d_ctx(b, 0U, reg);
                    a64_emit_guest_str_d(b, 0U, A64_ADDR);
                }
                a64_emit_add_imm_w(b, false, A64_ADDR, A64_ADDR, 8U, 0U);
            }
        } else {
            unsigned s_start = (vd_bits << 1) | d_bit;
            unsigned num_s = imm8;
            for (unsigned i = 0U; i < num_s; i++) {
                unsigned reg = (s_start + i) & 31U;
                if (l_bit) {
                    a64_emit_guest_ldr_s(b, 0U, A64_ADDR);
                    a64_emit_str_s_ctx(b, 0U, reg);
                } else {
                    a64_emit_ldr_s_ctx(b, 0U, reg);
                    a64_emit_guest_str_s(b, 0U, A64_ADDR);
                }
                a64_emit_add_imm_w(b, false, A64_ADDR, A64_ADDR, 4U, 0U);
            }
        }

        if (w_bit && rn != 15U) {
            if (u_bit) {
                a64_emit_add_imm_w(b, false, rn, rn, (uint16_t)total_bytes, 0U);
            } else {
                a64_emit_sub_imm_w(b, false, rn, rn, (uint16_t)total_bytes, 0U);
            }
        }
        return true;
    }

    /* 4. VFP data-processing (bit 4 == 0) */
    if ((insn & 0x0F000010U) == 0x0E000000U) {
        unsigned opc1 = ((insn >> 20) & 0xFU) & ~4U;
        unsigned opc2 = (insn >> 16) & 0xFU;
        bool d_bit    = ((insn >> 22) & 1U) != 0U;
        unsigned vd_b = (insn >> 12) & 0xFU;
        unsigned vn_b = (insn >> 16) & 0xFU;
        unsigned vm_b = insn & 0xFU;
        bool n_bit    = ((insn >> 7) & 1U) != 0U;
        bool op_bit   = ((insn >> 6) & 1U) != 0U;
        bool m_bit    = ((insn >> 5) & 1U) != 0U;

        /* Unary, VCVT, VCMP, VMOV imm (opc1 == 0b1011) */
        if (opc1 == 0xBU) {
            /* VMOV immediate */
            if ((insn & 0xC0U) == 0U) {
                uint8_t imm8 = (uint8_t)(((insn >> 16) & 0xFU) << 4 | (insn & 0xFU));
                if (is_double) {
                    uint64_t w = vfp_expand_imm_double(imm8);
                    unsigned d = (d_bit << 4) | vd_b;
                    a64_emit_mov_imm32(b, A64_TMP0, (uint32_t)w);
                    a64_emit_str_imm_w(b, A64_TMP0, A64_XCTX, A32_CTX_VFP_OFFSET + (d * 8U));
                    a64_emit_mov_imm32(b, A64_TMP0, (uint32_t)(w >> 32));
                    a64_emit_str_imm_w(b, A64_TMP0, A64_XCTX, A32_CTX_VFP_OFFSET + (d * 8U) + 4U);
                } else {
                    uint32_t w = vfp_expand_imm_single(imm8);
                    unsigned s = (vd_b << 1) | d_bit;
                    a64_emit_mov_imm32(b, A64_TMP0, w);
                    a64_emit_str_imm_w(b, A64_TMP0, A64_XCTX, A32_CTX_VFP_OFFSET + (s * 4U));
                }
                return true;
            }

            if (!op_bit) {
                return false;
            }

            /* VMOV reg / VABS */
            if (opc2 == 0U) {
                if (is_double) {
                    unsigned d = (d_bit << 4) | vd_b;
                    unsigned m = (m_bit << 4) | vm_b;
                    a64_emit_ldr_d_ctx(b, 1U, m);
                    if (n_bit) {
                        a64_emit_fabs_d(b, 0U, 1U);
                    } else {
                        a64_emit_fmov_d(b, 0U, 1U);
                    }
                    a64_emit_str_d_ctx(b, 0U, d);
                } else {
                    unsigned s = (vd_b << 1) | d_bit;
                    unsigned m = (vm_b << 1) | m_bit;
                    a64_emit_ldr_s_ctx(b, 1U, m);
                    if (n_bit) {
                        a64_emit_fabs_s(b, 0U, 1U);
                    } else {
                        a64_emit_fmov_s(b, 0U, 1U);
                    }
                    a64_emit_str_s_ctx(b, 0U, s);
                }
                return true;
            }

            /* VNEG / VSQRT */
            if (opc2 == 1U) {
                if (is_double) {
                    unsigned d = (d_bit << 4) | vd_b;
                    unsigned m = (m_bit << 4) | vm_b;
                    a64_emit_ldr_d_ctx(b, 1U, m);
                    if (n_bit) {
                        a64_emit_fsqrt_d(b, 0U, 1U);
                    } else {
                        a64_emit_fneg_d(b, 0U, 1U);
                    }
                    a64_emit_str_d_ctx(b, 0U, d);
                } else {
                    unsigned s = (vd_b << 1) | d_bit;
                    unsigned m = (vm_b << 1) | m_bit;
                    a64_emit_ldr_s_ctx(b, 1U, m);
                    if (n_bit) {
                        a64_emit_fsqrt_s(b, 0U, 1U);
                    } else {
                        a64_emit_fneg_s(b, 0U, 1U);
                    }
                    a64_emit_str_s_ctx(b, 0U, s);
                }
                return true;
            }

            /* VCVTB / VCVTT between half-precision and single-precision */
            if (!is_double && (opc2 == 2U || opc2 == 3U)) {
                unsigned s = (vd_b << 1) | d_bit;
                unsigned m = (vm_b << 1) | m_bit;
                if (opc2 == 2U) { /* VCVTB.F32.F16 / VCVTT.F32.F16 Sd, Sm */
                    a64_emit_ldr_s_ctx(b, 1U, m);
                    a64_emit_fmov_s_to_w(b, A64_TMP0, 1U);
                    if (n_bit) {
                        a64_emit_ubfm_w(b, A64_TMP0, A64_TMP0, 16U, 31U);
                    } else {
                        a64_emit_ubfm_w(b, A64_TMP0, A64_TMP0, 0U, 15U);
                    }
                    a64_emit_fmov_w_to_s(b, 1U, A64_TMP0);
                    a64_emit(b, 0x1EE24000U | (1U << 5) | 0U); /* FCVT s0, h1 */
                    a64_emit_str_s_ctx(b, 0U, s);
                } else { /* VCVTB.F16.F32 / VCVTT.F16.F32 Sd, Sm */
                    a64_emit_ldr_s_ctx(b, 1U, m);
                    a64_emit_ldr_s_ctx(b, 2U, s);
                    a64_emit(b, 0x1E23C000U | (1U << 5) | 0U); /* FCVT h0, s1 */
                    a64_emit_fmov_s_to_w(b, A64_TMP0, 0U);
                    a64_emit_fmov_s_to_w(b, A64_TMP1, 2U);
                    if (n_bit) {
                        a64_emit_bfm_w(b, A64_TMP1, A64_TMP0, 16U, 15U);
                    } else {
                        a64_emit_bfm_w(b, A64_TMP1, A64_TMP0, 0U, 15U);
                    }
                    a64_emit_fmov_w_to_s(b, 0U, A64_TMP1);
                    a64_emit_str_s_ctx(b, 0U, s);
                }
                return true;
            }

            /* VCMP / VCMPE */
            if (opc2 == 4U || opc2 == 5U) {
                bool zero = (opc2 == 5U);
                a64_emit_mrs_nzcv(b, A64_TMP1);
                if (is_double) {
                    unsigned d = (d_bit << 4) | vd_b;
                    unsigned m = (m_bit << 4) | vm_b;
                    a64_emit_ldr_d_ctx(b, 0U, d);
                    if (!zero) {
                        a64_emit_ldr_d_ctx(b, 1U, m);
                    }
                    a64_emit_fcmp_d(b, 0U, 1U, zero);
                } else {
                    unsigned s = (vd_b << 1) | d_bit;
                    unsigned m = (vm_b << 1) | m_bit;
                    a64_emit_ldr_s_ctx(b, 0U, s);
                    if (!zero) {
                        a64_emit_ldr_s_ctx(b, 1U, m);
                    }
                    a64_emit_fcmp_s(b, 0U, 1U, zero);
                }
                a64_emit_mrs_nzcv(b, A64_TMP0);
                a64_emit_ldr_imm_w(b, A64_TMP2, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, fpscr));
                a64_emit_movz_w(b, A64_TMP3, 0xF000U, 1U);
                a64_emit_and_reg_w(b, false, A64_TMP0, A64_TMP0, A64_TMP3, A64_SHIFT_LSL, 0U);
                a64_emit_bic_reg_w(b, false, A64_TMP2, A64_TMP2, A64_TMP3, A64_SHIFT_LSL, 0U);
                a64_emit_orr_reg_w(b, A64_TMP2, A64_TMP2, A64_TMP0, A64_SHIFT_LSL, 0U);
                a64_emit_str_imm_w(b, A64_TMP2, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, fpscr));
                a64_emit_msr_nzcv(b, A64_TMP1);
                return true;
            }

            /* VCVT between single and double */
            if (opc2 == 7U && n_bit) {
                if (is_double) { /* vcvt.f32.f64 Sd, Dm */
                    unsigned s = (vd_b << 1) | d_bit;
                    unsigned m = (m_bit << 4) | vm_b;
                    a64_emit_ldr_d_ctx(b, 1U, m);
                    a64_emit_fcvt_d_to_s(b, 0U, 1U);
                    a64_emit_str_s_ctx(b, 0U, s);
                } else { /* vcvt.f64.f32 Dd, Sm */
                    unsigned d = (d_bit << 4) | vd_b;
                    unsigned m = (vm_b << 1) | m_bit;
                    a64_emit_ldr_s_ctx(b, 1U, m);
                    a64_emit_fcvt_s_to_d(b, 0U, 1U);
                    a64_emit_str_d_ctx(b, 0U, d);
                }
                return true;
            }

            /* VCVT int to float */
            if (opc2 == 8U) {
                bool is_signed = n_bit;
                if (is_double) {
                    unsigned d = (d_bit << 4) | vd_b;
                    unsigned m = (vm_b << 1) | m_bit;
                    a64_emit_ldr_imm_w(b, A64_TMP0, A64_XCTX, A32_CTX_VFP_OFFSET + (m * 4U));
                    if (is_signed) {
                        a64_emit_scvtf_w_to_d(b, 0U, A64_TMP0);
                    } else {
                        a64_emit_ucvtf_w_to_d(b, 0U, A64_TMP0);
                    }
                    a64_emit_str_d_ctx(b, 0U, d);
                } else {
                    unsigned s = (vd_b << 1) | d_bit;
                    unsigned m = (vm_b << 1) | m_bit;
                    a64_emit_ldr_imm_w(b, A64_TMP0, A64_XCTX, A32_CTX_VFP_OFFSET + (m * 4U));
                    if (is_signed) {
                        a64_emit_scvtf_w_to_s(b, 0U, A64_TMP0);
                    } else {
                        a64_emit_ucvtf_w_to_s(b, 0U, A64_TMP0);
                    }
                    a64_emit_str_s_ctx(b, 0U, s);
                }
                return true;
            }

            /* VCVT between floating-point and fixed-point */
            if (opc2 == 10U || opc2 == 11U || opc2 == 14U || opc2 == 15U) {
                bool to_fixed     = (opc2 >= 14U);
                bool is_unsigned  = ((opc2 & 1U) != 0U);
                unsigned int_bits = n_bit ? 32U : 16U;
                unsigned imm5_raw = (vm_b << 1) | (m_bit ? 1U : 0U);
                if (imm5_raw >= int_bits) {
                    return false;
                }
                unsigned fbits   = int_bits - imm5_raw;
                unsigned scale   = 64U - fbits;
                unsigned reg_idx = is_double ? ((d_bit << 4) | vd_b) : ((vd_b << 1) | d_bit);
                uint32_t ctx_off = A32_CTX_VFP_OFFSET + (reg_idx * (is_double ? 8U : 4U));
                if (!to_fixed) {
                    a64_emit_ldr_imm_w(b, A64_TMP0, A64_XCTX, ctx_off);
                    if (!n_bit) {
                        if (is_unsigned) {
                            a64_emit_ubfm_w(b, A64_TMP0, A64_TMP0, 0U, 15U);
                        } else {
                            a64_emit_sbfm_w(b, A64_TMP0, A64_TMP0, 0U, 15U);
                        }
                    }
                    uint32_t base = (is_double ? 0x1E400000U : 0x1E000000U) |
                                    (is_unsigned ? 0x00030000U : 0x00020000U);
                    a64_emit(b, base | (scale << 10) | (A64_TMP0 << 5) | 0U);
                    if (is_double) {
                        a64_emit_str_d_ctx(b, 0U, reg_idx);
                    } else {
                        a64_emit_str_s_ctx(b, 0U, reg_idx);
                    }
                } else {
                    if (is_double) {
                        a64_emit_ldr_d_ctx(b, 0U, reg_idx);
                    } else {
                        a64_emit_ldr_s_ctx(b, 0U, reg_idx);
                    }
                    uint32_t base = (is_double ? 0x1E580000U : 0x1E180000U) |
                                    (is_unsigned ? 0x00010000U : 0x00000000U);
                    a64_emit(b, base | (scale << 10) | (0U << 5) | A64_TMP0);
                    a64_emit_str_imm_w(b, A64_TMP0, A64_XCTX, ctx_off);
                    if (is_double) {
                        a64_emit_str_imm_w(b, A64_ZR, A64_XCTX, ctx_off + 4U);
                    }
                }
                return true;
            }

            /* VCVT / VCVTR float to int */
            if (opc2 == 12U || opc2 == 13U) {
                bool is_signed  = (opc2 == 13U);
                bool round_zero = ((insn >> 7) & 1U) != 0U;
                if (is_double) {
                    unsigned s = (vd_b << 1) | d_bit;
                    unsigned m = (m_bit << 4) | vm_b;
                    a64_emit_ldr_d_ctx(b, 1U, m);
                    if (round_zero) {
                        if (is_signed) {
                            a64_emit_fcvtzs_d_to_w(b, A64_TMP0, 1U);
                        } else {
                            a64_emit_fcvtzu_d_to_w(b, A64_TMP0, 1U);
                        }
                    } else {
                        uint32_t base = is_signed ? 0x1E600000U : 0x1E610000U;
                        a64_emit(b, base | (1U << 5) | A64_TMP0);
                    }
                    a64_emit_str_imm_w(b, A64_TMP0, A64_XCTX, A32_CTX_VFP_OFFSET + (s * 4U));
                } else {
                    unsigned s = (vd_b << 1) | d_bit;
                    unsigned m = (vm_b << 1) | m_bit;
                    a64_emit_ldr_s_ctx(b, 1U, m);
                    if (round_zero) {
                        if (is_signed) {
                            a64_emit_fcvtzs_s_to_w(b, A64_TMP0, 1U);
                        } else {
                            a64_emit_fcvtzu_s_to_w(b, A64_TMP0, 1U);
                        }
                    } else {
                        uint32_t base = is_signed ? 0x1E200000U : 0x1E210000U;
                        a64_emit(b, base | (1U << 5) | A64_TMP0);
                    }
                    a64_emit_str_imm_w(b, A64_TMP0, A64_XCTX, A32_CTX_VFP_OFFSET + (s * 4U));
                }
                return true;
            }

            return false;
        }

        /* Binary arithmetic operations */
        unsigned d_reg = 0U;
        if (is_double) {
            d_reg = (d_bit << 4) | vd_b;
            unsigned n_reg = (n_bit << 4) | vn_b;
            unsigned m_reg = (m_bit << 4) | vm_b;
            a64_emit_ldr_d_ctx(b, 1U, n_reg);
            a64_emit_ldr_d_ctx(b, 2U, m_reg);
        } else {
            d_reg = (vd_b << 1) | d_bit;
            unsigned n_reg = (vn_b << 1) | n_bit;
            unsigned m_reg = (vm_b << 1) | m_bit;
            a64_emit_ldr_s_ctx(b, 1U, n_reg);
            a64_emit_ldr_s_ctx(b, 2U, m_reg);
        }

        bool handled = false;
        if (opc1 == 3U) {
            if (!op_bit) { /* VADD */
                if (is_double) a64_emit_fadd_d(b, 0U, 1U, 2U);
                else a64_emit_fadd_s(b, 0U, 1U, 2U);
            } else { /* VSUB */
                if (is_double) a64_emit_fsub_d(b, 0U, 1U, 2U);
                else a64_emit_fsub_s(b, 0U, 1U, 2U);
            }
            handled = true;
        } else if (opc1 == 2U) {
            if (!op_bit) { /* VMUL */
                if (is_double) a64_emit_fmul_d(b, 0U, 1U, 2U);
                else a64_emit_fmul_s(b, 0U, 1U, 2U);
            } else { /* VNMUL -> FNMUL */
                if (is_double) {
                    a64_emit(b, 0x1E608800U | (2U << 16) | (1U << 5) | 0U);
                } else {
                    a64_emit(b, 0x1E208800U | (2U << 16) | (1U << 5) | 0U);
                }
            }
            handled = true;
        } else if (opc1 == 8U && !op_bit) { /* VDIV */
            if (is_double) a64_emit_fdiv_d(b, 0U, 1U, 2U);
            else a64_emit_fdiv_s(b, 0U, 1U, 2U);
            handled = true;
        } else if (opc1 == 0U) {
            if (is_double) a64_emit_ldr_d_ctx(b, 3U, d_reg);
            else a64_emit_ldr_s_ctx(b, 3U, d_reg);

            if (!op_bit) { /* VMLA: d + n*m */
                if (is_double) {
                    a64_emit_fmul_d(b, 0U, 1U, 2U);
                    a64_emit_fadd_d(b, 0U, 3U, 0U);
                } else {
                    a64_emit_fmul_s(b, 0U, 1U, 2U);
                    a64_emit_fadd_s(b, 0U, 3U, 0U);
                }
            } else { /* VMLS: d - n*m */
                if (is_double) {
                    a64_emit_fmul_d(b, 0U, 1U, 2U);
                    a64_emit_fsub_d(b, 0U, 3U, 0U);
                } else {
                    a64_emit_fmul_s(b, 0U, 1U, 2U);
                    a64_emit_fsub_s(b, 0U, 3U, 0U);
                }
            }
            handled = true;
        } else if (opc1 == 1U) {
            if (is_double) a64_emit_ldr_d_ctx(b, 3U, d_reg);
            else a64_emit_ldr_s_ctx(b, 3U, d_reg);

            if (op_bit) { /* VNMLA: -d - n*m */
                if (is_double) {
                    a64_emit_fmul_d(b, 0U, 1U, 2U);
                    a64_emit_fadd_d(b, 0U, 3U, 0U);
                    a64_emit_fneg_d(b, 0U, 0U);
                } else {
                    a64_emit_fmul_s(b, 0U, 1U, 2U);
                    a64_emit_fadd_s(b, 0U, 3U, 0U);
                    a64_emit_fneg_s(b, 0U, 0U);
                }
            } else { /* VNMLS: -d + n*m */
                if (is_double) {
                    a64_emit_fmul_d(b, 0U, 1U, 2U);
                    a64_emit_fsub_d(b, 0U, 0U, 3U);
                } else {
                    a64_emit_fmul_s(b, 0U, 1U, 2U);
                    a64_emit_fsub_s(b, 0U, 0U, 3U);
                }
            }
            handled = true;
        } else if (opc1 == 10U) { /* VFMA / VFMS */
            if (is_double) a64_emit_ldr_d_ctx(b, 3U, d_reg);
            else a64_emit_ldr_s_ctx(b, 3U, d_reg);

            if (!op_bit) { /* VFMA: d + n*m */
                if (is_double) a64_emit_fmadd_d(b, 0U, 1U, 2U, 3U);
                else a64_emit_fmadd_s(b, 0U, 1U, 2U, 3U);
            } else { /* VFMS: d - n*m */
                if (is_double) a64_emit_fmsub_d(b, 0U, 1U, 2U, 3U);
                else a64_emit_fmsub_s(b, 0U, 1U, 2U, 3U);
            }
            handled = true;
        } else if (opc1 == 9U) { /* VFNMA / VFNMS */
            if (is_double) a64_emit_ldr_d_ctx(b, 3U, d_reg);
            else a64_emit_ldr_s_ctx(b, 3U, d_reg);

            if (op_bit) { /* VFNMA: -(d + n*m) */
                if (is_double) a64_emit_fnmadd_d(b, 0U, 1U, 2U, 3U);
                else a64_emit_fnmadd_s(b, 0U, 1U, 2U, 3U);
            } else { /* VFNMS: -d + n*m */
                if (is_double) a64_emit_fnmsub_d(b, 0U, 1U, 2U, 3U);
                else a64_emit_fnmsub_s(b, 0U, 1U, 2U, 3U);
            }
            handled = true;
        }

        if (handled) {
            if (is_double) a64_emit_str_d_ctx(b, 0U, d_reg);
            else a64_emit_str_s_ctx(b, 0U, d_reg);
            return true;
        }
    }

    return false;
}

static bool decode_neon(a64_buf_t *b, uint32_t insn, uint32_t pc)
{
    (void)pc;
    /* 1. VLD1 / VST1 / VLD2 / VST2 (0xF4...) */
    if ((insn >> 24) == 0xF4U) {
        bool l_bit    = ((insn >> 21) & 1U) != 0U;
        bool d_bit    = ((insn >> 22) & 1U) != 0U;
        unsigned rn   = (insn >> 16) & 0xFU;
        unsigned vd_b = (insn >> 12) & 0xFU;
        unsigned d_start = (d_bit << 4) | vd_b;
        unsigned type = (insn >> 8) & 0xFU;
        unsigned rm   = insn & 0xFU;

        if ((insn & 0x00800000U) != 0U) {
            /* VLD1..4 / VST1..4 single element to one lane or all lanes */
            unsigned size        = (insn >> 10) & 3U;
            unsigned n_structs   = ((insn >> 8) & 3U) + 1U;
            unsigned index_align = (insn >> 4) & 0xFU;
            unsigned base_reg    = (rn == 15U) ? A64_TMP0 : rn;
            if (rn == 15U) {
                a64_emit_mov_imm32(b, A64_TMP0, pc + 8U);
            }
            a64_emit_mov_w(b, A64_ADDR, base_reg);

            if (size < 3U) {
                unsigned lane = (size == 0U) ? ((insn >> 5) & 7U)
                              : (size == 1U) ? ((insn >> 6) & 3U)
                                             : ((insn >> 7) & 1U);
                unsigned step = 1U;
                if (size == 1U && (index_align & 2U) != 0U) {
                    step = 2U;
                } else if (size == 2U && (index_align & 4U) != 0U) {
                    step = 2U;
                }
                uint32_t elem_bytes = 1U << size;
                for (unsigned s = 0U; s < n_structs; s++) {
                    unsigned d_reg   = (d_start + s * step) & 31U;
                    uint32_t ctx_off = A32_CTX_VFP_OFFSET + (d_reg * 8U) + (lane * elem_bytes);
                    if (l_bit) {
                        if (size == 0U) {
                            a64_emit_guest_ldrb_w(b, A64_TMP1, A64_ADDR);
                            a64_emit_strb_imm_w(b, A64_TMP1, A64_XCTX, ctx_off);
                        } else if (size == 1U) {
                            a64_emit_guest_ldrh_w(b, A64_TMP1, A64_ADDR);
                            a64_emit_strh_imm_w(b, A64_TMP1, A64_XCTX, ctx_off);
                        } else {
                            a64_emit_guest_ldr_w(b, A64_TMP1, A64_ADDR);
                            a64_emit_str_imm_w(b, A64_TMP1, A64_XCTX, ctx_off);
                        }
                    } else {
                        if (size == 0U) {
                            a64_emit_ldrb_imm_w(b, A64_TMP1, A64_XCTX, ctx_off);
                            a64_emit_guest_strb_w(b, A64_TMP1, A64_ADDR);
                        } else if (size == 1U) {
                            a64_emit_ldrh_imm_w(b, A64_TMP1, A64_XCTX, ctx_off);
                            a64_emit_guest_strh_w(b, A64_TMP1, A64_ADDR);
                        } else {
                            a64_emit_ldr_imm_w(b, A64_TMP1, A64_XCTX, ctx_off);
                            a64_emit_guest_str_w(b, A64_TMP1, A64_ADDR);
                        }
                    }
                    if (s + 1U < n_structs) {
                        a64_emit_add_imm_w(b, false, A64_ADDR, A64_ADDR, (uint16_t)elem_bytes, 0U);
                    }
                }
                if (rm != 0xFU && rn != 15U) {
                    if (rm == 0xDU) {
                        a64_emit_add_imm_w(b, false, rn, rn, (uint16_t)(n_structs * elem_bytes), 0U);
                    } else {
                        a64_emit_add_reg_w(b, false, rn, rn, rm, A64_SHIFT_LSL, 0U);
                    }
                }
                return true;
            }
            if (size == 3U && l_bit) {
                /* VLD1..4.<8/16/32> {Dd[], ...}, [Rn] */
                unsigned dup_size = (insn >> 6) & 3U;
                if (dup_size > 2U) {
                    return false;
                }
                bool t_bit = ((insn >> 5) & 1U) != 0U;
                uint32_t elem_bytes = 1U << dup_size;
                if (n_structs == 1U) {
                    if (dup_size == 0U) {
                        a64_emit_guest_ldrb_w(b, A64_TMP1, A64_ADDR);
                    } else if (dup_size == 1U) {
                        a64_emit_guest_ldrh_w(b, A64_TMP1, A64_ADDR);
                    } else {
                        a64_emit_guest_ldr_w(b, A64_TMP1, A64_ADDR);
                    }
                    uint32_t a64_dup = (t_bit ? 0x4E000C00U : 0x0E000C00U) |
                                       ((1U << dup_size) << 16) | (A64_TMP1 << 5) | 0U;
                    a64_emit(b, a64_dup);
                    a64_emit_str_d_ctx(b, 0U, d_start);
                    if (t_bit) {
                        a64_emit_str_d_ctx(b, 0U, (d_start + 1U) & 31U);
                    }
                } else {
                    unsigned step = t_bit ? 2U : 1U;
                    uint32_t a64_dup = 0x0E000C00U | ((1U << dup_size) << 16) | (A64_TMP1 << 5) | 0U;
                    for (unsigned s = 0U; s < n_structs; s++) {
                        if (dup_size == 0U) {
                            a64_emit_guest_ldrb_w(b, A64_TMP1, A64_ADDR);
                        } else if (dup_size == 1U) {
                            a64_emit_guest_ldrh_w(b, A64_TMP1, A64_ADDR);
                        } else {
                            a64_emit_guest_ldr_w(b, A64_TMP1, A64_ADDR);
                        }
                        a64_emit(b, a64_dup);
                        a64_emit_str_d_ctx(b, 0U, (d_start + s * step) & 31U);
                        if (s + 1U < n_structs) {
                            a64_emit_add_imm_w(b, false, A64_ADDR, A64_ADDR, (uint16_t)elem_bytes, 0U);
                        }
                    }
                }
                if (rm != 0xFU && rn != 15U) {
                    if (rm == 0xDU) {
                        a64_emit_add_imm_w(b, false, rn, rn, (uint16_t)(n_structs * elem_bytes), 0U);
                    } else {
                        a64_emit_add_reg_w(b, false, rn, rn, rm, A64_SHIFT_LSL, 0U);
                    }
                }
                return true;
            }
        } else {
            unsigned nregs = 0U;
            if (type == 0x7U) {
                nregs = 1U;
            } else if (type == 0xAU) {
                nregs = 2U;
            } else if (type == 0x6U) {
                nregs = 3U;
            } else if (type == 0x2U) {
                nregs = 4U;
            }

            if (nregs > 0U) {
                unsigned base_reg = (rn == 15U) ? A64_TMP0 : rn;
                if (rn == 15U) {
                    a64_emit_mov_imm32(b, A64_TMP0, pc + 8U);
                }
                a64_emit_mov_w(b, A64_ADDR, base_reg);
                for (unsigned i = 0U; i < nregs; i++) {
                    unsigned reg = (d_start + i) & 31U;
                    if (l_bit) {
                        a64_emit_guest_ldr_d(b, 0U, A64_ADDR);
                        a64_emit_str_d_ctx(b, 0U, reg);
                    } else {
                        a64_emit_ldr_d_ctx(b, 0U, reg);
                        a64_emit_guest_str_d(b, 0U, A64_ADDR);
                    }
                    if (i + 1U < nregs) {
                        a64_emit_add_imm_w(b, false, A64_ADDR, A64_ADDR, 8U, 0U);
                    }
                }

                if (rm != 0xFU && rn != 15U) {
                    if (rm == 0xDU) {
                        a64_emit_add_imm_w(b, false, rn, rn, (uint16_t)(nregs * 8U), 0U);
                    } else {
                        a64_emit_add_reg_w(b, false, rn, rn, rm, A64_SHIFT_LSL, 0U);
                    }
                }
                return true;
            }

            /* VLD2 / VST2 (type == 0x8, 0x9, or 0x3) */
            if (type == 0x8U || type == 0x9U || type == 0x3U) {
                unsigned elem_sz = (insn >> 6) & 3U;
                if (elem_sz > 2U) {
                    return false;
                }
                unsigned base_reg = (rn == 15U) ? A64_TMP0 : rn;
                if (rn == 15U) {
                    a64_emit_mov_imm32(b, A64_TMP0, pc + 8U);
                }
                a64_emit_mov_w(b, A64_ADDR, base_reg);
                a64_emit_add_uxtw_x(b, A64_TMP1, A64_XG, A64_ADDR);
                if (type == 0x3U) {
                    /* 4-register VLD2 / VST2 ({Dd, Dd+1, Dd+2, Dd+3}) */
                    if (l_bit) {
                        a64_emit(b, 0x4C408000U | (elem_sz << 10) | (A64_TMP1 << 5) | 0U);
                        a64_emit_str_d_ctx(b, 0U, d_start);
                        a64_emit_str_d_ctx(b, 1U, (d_start + 2U) & 31U);
                        /* INS v2.d[0], v0.d[1] and INS v3.d[0], v1.d[1] */
                        a64_emit(b, 0x6E084400U | (0U << 5) | 2U);
                        a64_emit(b, 0x6E084400U | (1U << 5) | 3U);
                        a64_emit_str_d_ctx(b, 2U, (d_start + 1U) & 31U);
                        a64_emit_str_d_ctx(b, 3U, (d_start + 3U) & 31U);
                    } else {
                        a64_emit_ldr_d_ctx(b, 0U, d_start);
                        a64_emit_ldr_d_ctx(b, 2U, (d_start + 1U) & 31U);
                        a64_emit_ldr_d_ctx(b, 1U, (d_start + 2U) & 31U);
                        a64_emit_ldr_d_ctx(b, 3U, (d_start + 3U) & 31U);
                        a64_emit(b, 0x6E180400U | (2U << 5) | 0U); /* INS v0.d[1], v2.d[0] */
                        a64_emit(b, 0x6E180400U | (3U << 5) | 1U); /* INS v1.d[1], v3.d[0] */
                        a64_emit(b, 0x4C008000U | (elem_sz << 10) | (A64_TMP1 << 5) | 0U);
                    }
                } else {
                    unsigned step = (type == 0x9U) ? 2U : 1U;
                    unsigned d0 = d_start;
                    unsigned d1 = (d_start + step) & 31U;
                    if (l_bit) {
                        a64_emit_guest_ldr_d(b, 1U, A64_ADDR);
                        a64_emit_add_imm_w(b, false, A64_ADDR, A64_ADDR, 8U, 0U);
                        a64_emit_guest_ldr_d(b, 2U, A64_ADDR);
                        /* UZP1 v0, v1, v2 and UZP2 v3, v1, v2 */
                        a64_emit(b, 0x0E001800U | (elem_sz << 22) | (2U << 16) | (1U << 5) | 0U);
                        a64_emit(b, 0x0E005800U | (elem_sz << 22) | (2U << 16) | (1U << 5) | 3U);
                        a64_emit_str_d_ctx(b, 0U, d0);
                        a64_emit_str_d_ctx(b, 3U, d1);
                    } else {
                        a64_emit_ldr_d_ctx(b, 1U, d0);
                        a64_emit_ldr_d_ctx(b, 2U, d1);
                        /* ZIP1 v0, v1, v2 and ZIP2 v3, v1, v2 */
                        a64_emit(b, 0x0E003800U | (elem_sz << 22) | (2U << 16) | (1U << 5) | 0U);
                        a64_emit(b, 0x0E007800U | (elem_sz << 22) | (2U << 16) | (1U << 5) | 3U);
                        a64_emit_guest_str_d(b, 0U, A64_ADDR);
                        a64_emit_add_imm_w(b, false, A64_ADDR, A64_ADDR, 8U, 0U);
                        a64_emit_guest_str_d(b, 3U, A64_ADDR);
                    }
                }
                if (rm != 0xFU && rn != 15U) {
                    uint16_t wb_bytes = (type == 0x3U) ? 32U : 16U;
                    if (rm == 0xDU) {
                        a64_emit_add_imm_w(b, false, rn, rn, wb_bytes, 0U);
                    } else {
                        a64_emit_add_reg_w(b, false, rn, rn, rm, A64_SHIFT_LSL, 0U);
                    }
                }
                return true;
            }

            /* VLD3 / VST3 (type == 0x4 or 0x5) and VLD4 / VST4 (type == 0x0 or 0x1) */
            if (type == 0x4U || type == 0x5U || type == 0x0U || type == 0x1U) {
                unsigned elem_sz = (insn >> 6) & 3U;
                if (elem_sz > 2U) {
                    return false;
                }
                unsigned n_st = (type == 0x4U || type == 0x5U) ? 3U : 4U;
                unsigned step = ((type & 1U) != 0U) ? 2U : 1U;
                uint32_t opc  = (n_st == 3U) ? 0x00004000U : 0x00000000U;
                unsigned base_reg = (rn == 15U) ? A64_TMP0 : rn;
                if (rn == 15U) {
                    a64_emit_mov_imm32(b, A64_TMP0, pc + 8U);
                }
                a64_emit_mov_w(b, A64_ADDR, base_reg);
                a64_emit_add_uxtw_x(b, A64_TMP1, A64_XG, A64_ADDR);
                if (l_bit) {
                    a64_emit(b, 0x0C400000U | opc | (elem_sz << 10) | (A64_TMP1 << 5) | 0U);
                    for (unsigned i = 0U; i < n_st; i++) {
                        a64_emit_str_d_ctx(b, i, (d_start + i * step) & 31U);
                    }
                } else {
                    for (unsigned i = 0U; i < n_st; i++) {
                        a64_emit_ldr_d_ctx(b, i, (d_start + i * step) & 31U);
                    }
                    a64_emit(b, 0x0C000000U | opc | (elem_sz << 10) | (A64_TMP1 << 5) | 0U);
                }
                if (rm != 0xFU && rn != 15U) {
                    if (rm == 0xDU) {
                        a64_emit_add_imm_w(b, false, rn, rn, (uint16_t)(n_st * 8U), 0U);
                    } else {
                        a64_emit_add_reg_w(b, false, rn, rn, rm, A64_SHIFT_LSL, 0U);
                    }
                }
                return true;
            }
        }
    }

    /* 2. NEON modified immediate: VMOV.I*, VMVN.I*, VORR.I*, VBIC.I* */
    if ((insn & 0x0EB80090U) == 0x02800010U) {
        uint8_t imm8 = (uint8_t)((((insn >> 24) & 1U) << 7) |
                                (((insn >> 16) & 7U) << 4) |
                                (insn & 0xFU));
        uint8_t cmode = (uint8_t)((insn >> 8) & 0xFU);
        uint8_t op    = (uint8_t)((insn >> 5) & 1U);
        bool is_q     = ((insn >> 6) & 1U) != 0U;
        unsigned d    = (((insn >> 22) & 1U) << 4) | ((insn >> 12) & 0xFU);
        if (is_q) {
            d &= ~1U;
        }

        uint64_t val64 = 0ULL;
        if (neon_expand_imm(cmode, op, imm8, &val64)) {
            if ((cmode & 1U) == 0U || cmode >= 12U) { /* VMOV or VMVN */
                if (op != 0U && cmode != 14U) {
                    val64 = ~val64;
                }
                unsigned count_d = is_q ? 2U : 1U;
                for (unsigned i = 0U; i < count_d; i++) {
                    unsigned reg = d + i;
                    a64_emit_mov_imm32(b, A64_TMP0, (uint32_t)val64);
                    a64_emit_str_imm_w(b, A64_TMP0, A64_XCTX, A32_CTX_VFP_OFFSET + (reg * 8U));
                    a64_emit_mov_imm32(b, A64_TMP0, (uint32_t)(val64 >> 32));
                    a64_emit_str_imm_w(b, A64_TMP0, A64_XCTX, A32_CTX_VFP_OFFSET + (reg * 8U) + 4U);
                }
            } else { /* VORR or VBIC */
                unsigned count_d = is_q ? 2U : 1U;
                for (unsigned i = 0U; i < count_d; i++) {
                    unsigned reg = d + i;
                    /* Low word */
                    a64_emit_ldr_imm_w(b, A64_TMP0, A64_XCTX, A32_CTX_VFP_OFFSET + (reg * 8U));
                    a64_emit_mov_imm32(b, A64_TMP1, (uint32_t)val64);
                    if (op == 0U) {
                        a64_emit_orr_reg_w(b, A64_TMP0, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
                    } else {
                        a64_emit_bic_reg_w(b, false, A64_TMP0, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
                    }
                    a64_emit_str_imm_w(b, A64_TMP0, A64_XCTX, A32_CTX_VFP_OFFSET + (reg * 8U));
                    /* High word */
                    a64_emit_ldr_imm_w(b, A64_TMP0, A64_XCTX, A32_CTX_VFP_OFFSET + (reg * 8U) + 4U);
                    a64_emit_mov_imm32(b, A64_TMP1, (uint32_t)(val64 >> 32));
                    if (op == 0U) {
                        a64_emit_orr_reg_w(b, A64_TMP0, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
                    } else {
                        a64_emit_bic_reg_w(b, false, A64_TMP0, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
                    }
                    a64_emit_str_imm_w(b, A64_TMP0, A64_XCTX, A32_CTX_VFP_OFFSET + (reg * 8U) + 4U);
                }
            }
            return true;
        }
    }

    /* 3. NEON 2-register misc, VTBL/VTBX, and VDUP scalar */
    if ((insn & 0x0FB00010U) == 0x03B00000U) {
        bool is_q     = ((insn >> 6) & 1U) != 0U;
        unsigned d    = (((insn >> 22) & 1U) << 4) | ((insn >> 12) & 0xFU);
        unsigned m    = (((insn >> 5) & 1U) << 4) | (insn & 0xFU);
        unsigned opc1 = (insn >> 16) & 3U;
        unsigned size = (insn >> 18) & 3U;

        /* VTBL / VTBX (bits 11..10 == 0b10) */
        if (((insn >> 10) & 3U) == 2U) {
            unsigned len  = ((insn >> 8) & 3U) + 1U;
            unsigned dn   = (((insn >> 7) & 1U) << 4) | ((insn >> 16) & 0xFU);
            bool is_tbx   = is_q;
            a64_emit_ldr_d_ctx(b, 1U, dn);
            if (len >= 2U) {
                a64_emit_ldr_d_ctx(b, 3U, (dn + 1U) & 31U);
                a64_emit(b, 0x6E180400U | (3U << 5) | 1U); /* INS v1.d[1], v3.d[0] */
            }
            if (len >= 3U) {
                a64_emit_ldr_d_ctx(b, 2U, (dn + 2U) & 31U);
            }
            if (len == 4U) {
                a64_emit_ldr_d_ctx(b, 3U, (dn + 3U) & 31U);
                a64_emit(b, 0x6E180400U | (3U << 5) | 2U); /* INS v2.d[1], v3.d[0] */
            }
            a64_emit_ldr_d_ctx(b, 4U, m);
            if (len == 1U || len == 3U) {
                a64_emit_mov_imm32(b, A64_TMP0, len * 8U);
                a64_emit(b, 0x0E010C00U | (A64_TMP0 << 5) | 5U);           /* DUP v5.8b, w15 */
                a64_emit(b, 0x2E203C00U | (5U << 16) | (4U << 5) | 5U);     /* CMHS v5.8b, v4.8b, v5.8b */
                a64_emit(b, 0x0EA01C00U | (5U << 16) | (4U << 5) | 4U);     /* ORR v4.8b, v4.8b, v5.8b */
            }
            if (is_tbx) {
                a64_emit_ldr_d_ctx(b, 0U, d);
            }
            unsigned a64_len = (len > 2U) ? 1U : 0U;
            uint32_t tbl_op = (is_tbx ? 0x0E001000U : 0x0E000000U) |
                              (4U << 16) | (a64_len << 13) | (1U << 5) | 0U;
            a64_emit(b, tbl_op);
            a64_emit_str_d_ctx(b, 0U, d);
            return true;
        }

        /* VDUP scalar (bits 11..7 == 0b11000) */
        if (((insn >> 7) & 0x1FU) == 0x18U) {
            unsigned imm4 = (insn >> 16) & 0xFU;
            if ((imm4 & 7U) == 0U) {
                return false;
            }
            unsigned dup_sz = 0U, index = 0U;
            if (imm4 & 1U) {
                dup_sz = 0U;
                index  = imm4 >> 1;
            } else if (imm4 & 2U) {
                dup_sz = 1U;
                index  = imm4 >> 2;
            } else {
                dup_sz = 2U;
                index  = imm4 >> 3;
            }
            unsigned imm5 = (dup_sz == 0U) ? ((index << 1) | 1U)
                          : (dup_sz == 1U) ? ((index << 2) | 2U)
                                           : ((index << 3) | 4U);
            a64_emit_ldr_d_ctx(b, 1U, m);
            uint32_t a64_insn = (is_q ? 0x4E000400U : 0x0E000400U) |
                                (imm5 << 16) | (1U << 5) | 0U;
            a64_emit(b, a64_insn);
            if (is_q) {
                a64_emit_str_q_ctx(b, 0U, d >> 1);
            } else {
                a64_emit_str_d_ctx(b, 0U, d);
            }
            return true;
        }

        if (((insn >> 11) & 1U) != 0U) {
            return false;
        }

        unsigned op5 = (insn >> 7) & 0x1FU;

        if (opc1 == 0U) {
            /* VREV64 (0), VREV32 (1), VREV16 (2) */
            if (op5 <= 2U && (op5 + size) < 3U) {
                uint32_t rev_base = (op5 == 0U) ? 0x0E200800U
                                  : (op5 == 1U) ? 0x2E200800U
                                                : 0x0E201800U;
                if (is_q) {
                    a64_emit_ldr_q_ctx(b, 1U, m >> 1);
                    a64_emit(b, rev_base | 0x40000000U | (size << 22) | (1U << 5) | 0U);
                    a64_emit_str_q_ctx(b, 0U, d >> 1);
                } else {
                    a64_emit_ldr_d_ctx(b, 1U, m);
                    a64_emit(b, rev_base | (size << 22) | (1U << 5) | 0U);
                    a64_emit_str_d_ctx(b, 0U, d);
                }
                return true;
            }
            /* VPADDL (op5 == 4 signed, op5 == 5 unsigned) */
            if ((op5 == 4U || op5 == 5U) && size < 3U) {
                uint32_t base = (op5 == 5U) ? 0x2E202800U : 0x0E202800U;
                if (is_q) {
                    a64_emit_ldr_q_ctx(b, 1U, m >> 1);
                    a64_emit(b, base | 0x40000000U | (size << 22) | (1U << 5) | 0U);
                    a64_emit_str_q_ctx(b, 0U, d >> 1);
                } else {
                    a64_emit_ldr_d_ctx(b, 1U, m);
                    a64_emit(b, base | (size << 22) | (1U << 5) | 0U);
                    a64_emit_str_d_ctx(b, 0U, d);
                }
                return true;
            }
            /* VCLS (op5 == 8), VCLZ (op5 == 9) */
            if ((op5 == 8U || op5 == 9U) && size < 3U) {
                uint32_t base = (op5 == 9U) ? 0x2E204800U : 0x0E204800U;
                if (is_q) {
                    a64_emit_ldr_q_ctx(b, 1U, m >> 1);
                    a64_emit(b, base | 0x40000000U | (size << 22) | (1U << 5) | 0U);
                    a64_emit_str_q_ctx(b, 0U, d >> 1);
                } else {
                    a64_emit_ldr_d_ctx(b, 1U, m);
                    a64_emit(b, base | (size << 22) | (1U << 5) | 0U);
                    a64_emit_str_d_ctx(b, 0U, d);
                }
                return true;
            }
            /* VCNT (op5 == 10, size == 0) */
            if (op5 == 10U && size == 0U) {
                if (is_q) {
                    a64_emit_ldr_q_ctx(b, 1U, m >> 1);
                    a64_emit(b, 0x4E205800U | (1U << 5) | 0U);
                    a64_emit_str_q_ctx(b, 0U, d >> 1);
                } else {
                    a64_emit_ldr_d_ctx(b, 1U, m);
                    a64_emit(b, 0x0E205800U | (1U << 5) | 0U);
                    a64_emit_str_d_ctx(b, 0U, d);
                }
                return true;
            }
            /* VMVN (op5 == 11, size == 0) */
            if (op5 == 11U && size == 0U) {
                if (is_q) {
                    a64_emit_ldr_q_ctx(b, 1U, m >> 1);
                    a64_emit(b, 0x6E205800U | (1U << 5) | 0U);
                    a64_emit_str_q_ctx(b, 0U, d >> 1);
                } else {
                    a64_emit_ldr_d_ctx(b, 1U, m);
                    a64_emit(b, 0x2E205800U | (1U << 5) | 0U);
                    a64_emit_str_d_ctx(b, 0U, d);
                }
                return true;
            }
            /* VPADAL (op5 == 12 signed, op5 == 13 unsigned) */
            if ((op5 == 12U || op5 == 13U) && size < 3U) {
                uint32_t base = (op5 == 13U) ? 0x2E206800U : 0x0E206800U;
                if (is_q) {
                    a64_emit_ldr_q_ctx(b, 0U, d >> 1);
                    a64_emit_ldr_q_ctx(b, 1U, m >> 1);
                    a64_emit(b, base | 0x40000000U | (size << 22) | (1U << 5) | 0U);
                    a64_emit_str_q_ctx(b, 0U, d >> 1);
                } else {
                    a64_emit_ldr_d_ctx(b, 0U, d);
                    a64_emit_ldr_d_ctx(b, 1U, m);
                    a64_emit(b, base | (size << 22) | (1U << 5) | 0U);
                    a64_emit_str_d_ctx(b, 0U, d);
                }
                return true;
            }
            /* VQABS (op5 == 14), VQNEG (op5 == 15) */
            if ((op5 == 14U || op5 == 15U) && size < 3U) {
                uint32_t base = (op5 == 15U) ? 0x2E207800U : 0x0E207800U;
                if (is_q) {
                    a64_emit_ldr_q_ctx(b, 1U, m >> 1);
                    a64_emit(b, base | 0x40000000U | (size << 22) | (1U << 5) | 0U);
                    a64_emit_str_q_ctx(b, 0U, d >> 1);
                } else {
                    a64_emit_ldr_d_ctx(b, 1U, m);
                    a64_emit(b, base | (size << 22) | (1U << 5) | 0U);
                    a64_emit_str_d_ctx(b, 0U, d);
                }
                return true;
            }
        }

        if (opc1 == 1U) {
            /* Integer compare-with-zero: VCGT (0), VCGE (1), VCEQ (2), VCLE (3), VCLT (4) #0 */
            if (op5 <= 4U && size < 3U) {
                uint32_t base = ((op5 & 1U) ? 0x2E200800U : 0x0E200800U) |
                                (((8U + (op5 >> 1)) & 0x1FU) << 12);
                if (is_q) {
                    a64_emit_ldr_q_ctx(b, 1U, m >> 1);
                    a64_emit(b, base | 0x40000000U | (size << 22) | (1U << 5) | 0U);
                    a64_emit_str_q_ctx(b, 0U, d >> 1);
                } else {
                    a64_emit_ldr_d_ctx(b, 1U, m);
                    a64_emit(b, base | (size << 22) | (1U << 5) | 0U);
                    a64_emit_str_d_ctx(b, 0U, d);
                }
                return true;
            }
            /* VABS.S<8/16/32> (op5 == 6), VNEG.S<8/16/32> (op5 == 7) */
            if ((op5 == 6U || op5 == 7U) && size < 3U) {
                uint32_t base = (op5 == 7U) ? 0x2E20B800U : 0x0E20B800U;
                if (is_q) {
                    a64_emit_ldr_q_ctx(b, 1U, m >> 1);
                    a64_emit(b, base | 0x40000000U | (size << 22) | (1U << 5) | 0U);
                    a64_emit_str_q_ctx(b, 0U, d >> 1);
                } else {
                    a64_emit_ldr_d_ctx(b, 1U, m);
                    a64_emit(b, base | (size << 22) | (1U << 5) | 0U);
                    a64_emit_str_d_ctx(b, 0U, d);
                }
                return true;
            }
            /* Float compare-with-zero: VCGT (8), VCGE (9), VCEQ (10), VCLE (11), VCLT (12) #0 */
            if (op5 >= 8U && op5 <= 12U && size == 2U) {
                uint32_t base = ((op5 & 1U) ? 0x2EA00800U : 0x0EA00800U) |
                                (((8U + (op5 >> 1)) & 0x1FU) << 12);
                if (is_q) {
                    a64_emit_ldr_q_ctx(b, 1U, m >> 1);
                    a64_emit(b, base | 0x40000000U | (1U << 5) | 0U);
                    a64_emit_str_q_ctx(b, 0U, d >> 1);
                } else {
                    a64_emit_ldr_d_ctx(b, 1U, m);
                    a64_emit(b, base | (1U << 5) | 0U);
                    a64_emit_str_d_ctx(b, 0U, d);
                }
                return true;
            }
            /* VABS.F32 (op5 == 14), VNEG.F32 (op5 == 15) */
            if ((op5 == 14U || op5 == 15U) && size == 2U) {
                uint32_t base = (op5 == 15U) ? 0x2EA0F800U : 0x0EA0F800U;
                if (is_q) {
                    a64_emit_ldr_q_ctx(b, 1U, m >> 1);
                    a64_emit(b, base | 0x40000000U | (1U << 5) | 0U);
                    a64_emit_str_q_ctx(b, 0U, d >> 1);
                } else {
                    a64_emit_ldr_d_ctx(b, 1U, m);
                    a64_emit(b, base | (1U << 5) | 0U);
                    a64_emit_str_d_ctx(b, 0U, d);
                }
                return true;
            }
        }

        if (opc1 == 2U) {
            /* VSWP (op5 == 0, size == 0) */
            if (op5 == 0U && size == 0U) {
                if (is_q) {
                    a64_emit_ldr_q_ctx(b, 0U, d >> 1);
                    a64_emit_ldr_q_ctx(b, 1U, m >> 1);
                    a64_emit_str_q_ctx(b, 1U, d >> 1);
                    a64_emit_str_q_ctx(b, 0U, m >> 1);
                } else {
                    a64_emit_ldr_d_ctx(b, 0U, d);
                    a64_emit_ldr_d_ctx(b, 1U, m);
                    a64_emit_str_d_ctx(b, 1U, d);
                    a64_emit_str_d_ctx(b, 0U, m);
                }
                return true;
            }

            /* VTRN (op5 == 1), VUZP (op5 == 2), VZIP (op5 == 3) */
            if (op5 >= 1U && op5 <= 3U && size < 3U) {
                uint32_t op1_base = (op5 == 1U) ? 0x0E002800U : (op5 == 2U) ? 0x0E001800U : 0x0E003800U;
                uint32_t op2_base = (op5 == 1U) ? 0x0E006800U : (op5 == 2U) ? 0x0E005800U : 0x0E007800U;
                if (is_q) {
                    op1_base |= 0x40000000U;
                    op2_base |= 0x40000000U;
                    a64_emit_ldr_q_ctx(b, 1U, d >> 1);
                    a64_emit_ldr_q_ctx(b, 2U, m >> 1);
                    a64_emit(b, op1_base | (size << 22) | (2U << 16) | (1U << 5) | 0U);
                    a64_emit(b, op2_base | (size << 22) | (2U << 16) | (1U << 5) | 3U);
                    a64_emit_str_q_ctx(b, 0U, d >> 1);
                    a64_emit_str_q_ctx(b, 3U, m >> 1);
                } else {
                    a64_emit_ldr_d_ctx(b, 1U, d);
                    a64_emit_ldr_d_ctx(b, 2U, m);
                    a64_emit(b, op1_base | (size << 22) | (2U << 16) | (1U << 5) | 0U);
                    a64_emit(b, op2_base | (size << 22) | (2U << 16) | (1U << 5) | 3U);
                    a64_emit_str_d_ctx(b, 0U, d);
                    a64_emit_str_d_ctx(b, 3U, m);
                }
                return true;
            }

            /* VMOVN / VQMOVUN (op5 == 4), VQMOVN.S / VQMOVN.U (op5 == 5) */
            if ((op5 == 4U || op5 == 5U) && size < 3U) {
                uint32_t base = (op5 == 4U)
                                    ? (is_q ? 0x2E212800U : 0x0E212800U)
                                    : (is_q ? 0x2E214800U : 0x0E214800U);
                a64_emit_ldr_q_ctx(b, 1U, m >> 1);
                a64_emit(b, base | (size << 22) | (1U << 5) | 0U);
                a64_emit_str_d_ctx(b, 0U, d);
                return true;
            }

            /* VSHLL.I<8/16/32> Qd, Dm, #<8/16/32> (op5 == 6, !is_q) */
            if (op5 == 6U && !is_q && size < 3U) {
                a64_emit_ldr_d_ctx(b, 1U, m);
                a64_emit(b, 0x2E213800U | (size << 22) | (1U << 5) | 0U);
                a64_emit_str_q_ctx(b, 0U, d >> 1);
                return true;
            }
        }

        if (opc1 == 3U && size == 2U && op5 >= 8U && op5 <= 15U) {
            /* VRECPE/VRSQRTE (8..11) and VCVT.F32<->S32/U32 (12..15) */
            uint32_t base = (op5 == 8U)  ? 0x0EA1C800U
                          : (op5 == 9U)  ? 0x2EA1C800U
                          : (op5 == 10U) ? 0x0EA1D800U
                          : (op5 == 11U) ? 0x2EA1D800U
                          : (op5 == 12U) ? 0x0E21D800U
                          : (op5 == 13U) ? 0x2E21D800U
                          : (op5 == 14U) ? 0x0EA1B800U
                                         : 0x2EA1B800U;
            if (is_q) {
                a64_emit_ldr_q_ctx(b, 1U, m >> 1);
                a64_emit(b, base | 0x40000000U | (1U << 5) | 0U);
                a64_emit_str_q_ctx(b, 0U, d >> 1);
            } else {
                a64_emit_ldr_d_ctx(b, 1U, m);
                a64_emit(b, base | (1U << 5) | 0U);
                a64_emit_str_d_ctx(b, 0U, d);
            }
            return true;
        }
    }

    /* 4. VEXT */
    if ((insn & 0x0FB00010U) == 0x02B00000U) {
        unsigned imm4 = (insn >> 8) & 0xFU;
        bool is_q     = ((insn >> 6) & 1U) != 0U;
        if (!is_q && (imm4 & 8U) != 0U) {
            return false;
        }
        if (is_q) {
            unsigned qd = ((((insn >> 22) & 1U) << 4) | ((insn >> 12) & 0xFU)) >> 1;
            unsigned qn = ((((insn >> 7) & 1U) << 4) | ((insn >> 16) & 0xFU)) >> 1;
            unsigned qm = ((((insn >> 5) & 1U) << 4) | (insn & 0xFU)) >> 1;
            a64_emit_ldr_q_ctx(b, 1U, qn);
            a64_emit_ldr_q_ctx(b, 2U, qm);
            a64_emit(b, 0x6E000000U | (2U << 16) | ((imm4 & 15U) << 11) | (1U << 5) | 0U);
            a64_emit_str_q_ctx(b, 0U, qd);
        } else {
            unsigned dd = (((insn >> 22) & 1U) << 4) | ((insn >> 12) & 0xFU);
            unsigned dn = (((insn >> 7) & 1U) << 4) | ((insn >> 16) & 0xFU);
            unsigned dm = (((insn >> 5) & 1U) << 4) | (insn & 0xFU);
            a64_emit_ldr_d_ctx(b, 1U, dn);
            a64_emit_ldr_d_ctx(b, 2U, dm);
            a64_emit(b, 0x2E000000U | (2U << 16) | ((imm4 & 7U) << 11) | (1U << 5) | 0U);
            a64_emit_str_d_ctx(b, 0U, dd);
        }
        return true;
    }

    /* 5. NEON shifts by immediate (bit 23 = 1, bit 4 = 1) */
    if (((insn & 0x0F800010U) == 0x02800010U || (insn & 0x0F800010U) == 0x03800010U)) {
        unsigned sh_op = (insn >> 8) & 0xFU;
        unsigned imm6  = (insn >> 16) & 0x3FU;
        unsigned l_sh  = (insn >> 7) & 1U;
        unsigned imm7  = (l_sh << 6) | imm6;
        bool is_q      = ((insn >> 6) & 1U) != 0U;
        bool is_u      = ((insn >> 24) & 1U) != 0U;

        if ((sh_op == 0xAU) && (insn & 0x000000C0U) == 0x00000000U && (imm6 >= 8U)) {
            /* VMOVL / VSHLL.<s/u><8/16/32> Qd, Dm */
            unsigned qd = ((((insn >> 22) & 1U) << 4) | ((insn >> 12) & 0xFU)) >> 1;
            unsigned dm = (((insn >> 5) & 1U) << 4) | (insn & 0xFU);
            uint32_t base = is_u ? 0x2F00A400U : 0x0F00A400U;
            a64_emit_ldr_d_ctx(b, 1U, dm);
            a64_emit(b, base | (imm6 << 16) | (1U << 5) | 0U);
            a64_emit_str_q_ctx(b, 0U, qd);
            return true;
        }

        if ((sh_op == 0x8U || sh_op == 0x9U) && l_sh == 0U && imm6 >= 8U) {
            /* VSHRN / VRSHRN / VQSHRUN / VQRSHRUN (sh_op == 8), VQSHRN / VQRSHRN (sh_op == 9) */
            unsigned dd = (((insn >> 22) & 1U) << 4) | ((insn >> 12) & 0xFU);
            unsigned qm = ((((insn >> 5) & 1U) << 4) | (insn & 0xFU)) >> 1;
            uint32_t base = (is_u ? 0x2F000000U : 0x0F000000U) |
                            ((sh_op == 0x9U) ? 0x9400U : 0x8400U) |
                            (is_q ? 0x0800U : 0U);
            a64_emit_ldr_q_ctx(b, 1U, qm);
            a64_emit(b, base | (imm6 << 16) | (1U << 5) | 0U);
            a64_emit_str_d_ctx(b, 0U, dd);
            return true;
        }

        if ((sh_op == 0xEU || sh_op == 0xFU) && l_sh == 0U && imm6 >= 32U) {
            /* Vector fixed-point VCVT.F32.<S32/U32> (0xE) and VCVT.<S32/U32>.F32 (0xF) */
            uint32_t opc = (sh_op == 0xFU) ? 0xFC00U : 0xE400U;
            if (is_q) {
                unsigned qd = ((((insn >> 22) & 1U) << 4) | ((insn >> 12) & 0xFU)) >> 1;
                unsigned qm = ((((insn >> 5) & 1U) << 4) | (insn & 0xFU)) >> 1;
                a64_emit_ldr_q_ctx(b, 1U, qm);
                uint32_t base = (is_u ? 0x6F000000U : 0x4F000000U) | opc;
                a64_emit(b, base | (imm6 << 16) | (1U << 5) | 0U);
                a64_emit_str_q_ctx(b, 0U, qd);
            } else {
                unsigned dd = (((insn >> 22) & 1U) << 4) | ((insn >> 12) & 0xFU);
                unsigned dm = (((insn >> 5) & 1U) << 4) | (insn & 0xFU);
                a64_emit_ldr_d_ctx(b, 1U, dm);
                uint32_t base = (is_u ? 0x2F000000U : 0x0F000000U) | opc;
                a64_emit(b, base | (imm6 << 16) | (1U << 5) | 0U);
                a64_emit_str_d_ctx(b, 0U, dd);
            }
            return true;
        }

        if ((sh_op <= 0x7U) && (l_sh != 0U || imm6 >= 8U)) {
            if (sh_op == 0x4U && !is_u) {
                return false;
            }
            if (sh_op == 0x6U && !is_u) {
                return false;
            }
            uint32_t opc = (sh_op == 0x0U) ? 0x0400U   /* VSHR: SSHR / USHR */
                         : (sh_op == 0x1U) ? 0x1400U   /* VSRA: SSRA / USRA */
                         : (sh_op == 0x2U) ? 0x2400U   /* VRSHR: SRSHR / URSHR */
                         : (sh_op == 0x3U) ? 0x3400U   /* VRSRA: SRSRA / URSRA */
                         : (sh_op == 0x4U) ? 0x4400U   /* VSRI: SRI */
                         : (sh_op == 0x5U) ? 0x5400U   /* VSHL (!is_u) / VSLI (is_u) */
                         : (sh_op == 0x6U) ? 0x6400U   /* VQSHLU: SQSHLU */
                                           : 0x7400U;  /* VQSHL: SQSHL / UQSHL */
            bool needs_dst = (sh_op == 0x1U || sh_op == 0x3U || sh_op == 0x4U ||
                              (sh_op == 0x5U && is_u));
            if (is_q) {
                unsigned qd = ((((insn >> 22) & 1U) << 4) | ((insn >> 12) & 0xFU)) >> 1;
                unsigned qm = ((((insn >> 5) & 1U) << 4) | (insn & 0xFU)) >> 1;
                if (needs_dst) {
                    a64_emit_ldr_q_ctx(b, 0U, qd);
                }
                a64_emit_ldr_q_ctx(b, 1U, qm);
                uint32_t base = (is_u ? 0x6F000000U : 0x4F000000U) | opc;
                a64_emit(b, base | (imm7 << 16) | (1U << 5) | 0U);
                a64_emit_str_q_ctx(b, 0U, qd);
            } else {
                unsigned dd = (((insn >> 22) & 1U) << 4) | ((insn >> 12) & 0xFU);
                unsigned dm = (((insn >> 5) & 1U) << 4) | (insn & 0xFU);
                if (needs_dst) {
                    a64_emit_ldr_d_ctx(b, 0U, dd);
                }
                a64_emit_ldr_d_ctx(b, 1U, dm);
                uint32_t base = (l_sh != 0U)
                                    ? ((is_u ? 0x7F000000U : 0x5F000000U) | opc)
                                    : ((is_u ? 0x2F000000U : 0x0F000000U) | opc);
                a64_emit(b, base | (imm7 << 16) | (1U << 5) | 0U);
                a64_emit_str_d_ctx(b, 0U, dd);
            }
            return true;
        }
    }

    /* 6. NEON 3-register different length, 2-register + scalar, & 3-register same */
    if ((insn & 0x0E000000U) == 0x02000000U) {
        bool u_bit    = ((insn >> 24) & 1U) != 0U;
        unsigned size = (insn >> 20) & 3U;
        unsigned op   = (insn >> 8) & 0xFU;
        bool b4       = ((insn >> 4) & 1U) != 0U;
        bool is_q     = ((insn >> 6) & 1U) != 0U;

        if ((insn & 0x00800000U) != 0U && size < 3U && !b4 && !is_q) {
            /* Narrowing 3-register different length: Dd, Qn, Qm (VADDHN/VRADDHN, VSUBHN/VRSUBHN) */
            if (op == 0x4U || op == 0x6U) {
                unsigned dd = (((insn >> 22) & 1U) << 4) | ((insn >> 12) & 0xFU);
                unsigned qn = ((((insn >> 7) & 1U) << 4) | ((insn >> 16) & 0xFU)) >> 1;
                unsigned qm = ((((insn >> 5) & 1U) << 4) | (insn & 0xFU)) >> 1;
                uint32_t hn_op = (u_bit ? 0x2E200000U : 0x0E200000U) |
                                 ((op == 0x6U) ? 0x6000U : 0x4000U) | (size << 22);
                a64_emit_ldr_q_ctx(b, 1U, qn);
                a64_emit_ldr_q_ctx(b, 2U, qm);
                a64_emit(b, hn_op | (2U << 16) | (1U << 5) | 0U);
                a64_emit_str_d_ctx(b, 0U, dd);
                return true;
            }
            /* Widening 3-register different length: Qd, Qn, Dm (VADDW, VSUBW) */
            if (op == 0x1U || op == 0x3U) {
                unsigned qd = ((((insn >> 22) & 1U) << 4) | ((insn >> 12) & 0xFU)) >> 1;
                unsigned qn = ((((insn >> 7) & 1U) << 4) | ((insn >> 16) & 0xFU)) >> 1;
                unsigned dm = (((insn >> 5) & 1U) << 4) | (insn & 0xFU);
                uint32_t w_op = (u_bit ? 0x2E200000U : 0x0E200000U) |
                                ((op == 0x3U) ? 0x3000U : 0x1000U) | (size << 22);
                a64_emit_ldr_q_ctx(b, 1U, qn);
                a64_emit_ldr_d_ctx(b, 2U, dm);
                a64_emit(b, w_op | (2U << 16) | (1U << 5) | 0U);
                a64_emit_str_q_ctx(b, 0U, qd);
                return true;
            }
            /* Long 3-register different length: Qd, Dn, Dm */
            uint32_t dl_op = 0U;
            bool dl_acc = false;
            if (op == 0x0U) { /* VADDL */
                dl_op = (u_bit ? 0x2E200000U : 0x0E200000U) | (size << 22);
            } else if (op == 0x2U) { /* VSUBL */
                dl_op = (u_bit ? 0x2E202000U : 0x0E202000U) | (size << 22);
            } else if (op == 0x5U) { /* VABAL */
                dl_op = (u_bit ? 0x2E205000U : 0x0E205000U) | (size << 22);
                dl_acc = true;
            } else if (op == 0x7U) { /* VABDL */
                dl_op = (u_bit ? 0x2E207000U : 0x0E207000U) | (size << 22);
            } else if (op == 0x8U) { /* VMLAL */
                dl_op = (u_bit ? 0x2E208000U : 0x0E208000U) | (size << 22);
                dl_acc = true;
            } else if (op == 0x9U && !u_bit && (size == 1U || size == 2U)) { /* VQDMLAL */
                dl_op = 0x0E209000U | (size << 22);
                dl_acc = true;
            } else if (op == 0xAU) { /* VMLSL */
                dl_op = (u_bit ? 0x2E20A000U : 0x0E20A000U) | (size << 22);
                dl_acc = true;
            } else if (op == 0xBU && !u_bit && (size == 1U || size == 2U)) { /* VQDMLSL */
                dl_op = 0x0E20B000U | (size << 22);
                dl_acc = true;
            } else if (op == 0xCU) { /* VMULL */
                dl_op = (u_bit ? 0x2E20C000U : 0x0E20C000U) | (size << 22);
            } else if (op == 0xDU && !u_bit && (size == 1U || size == 2U)) { /* VQDMULL */
                dl_op = 0x0E20D000U | (size << 22);
            }
            if (dl_op != 0U) {
                unsigned qd = ((((insn >> 22) & 1U) << 4) | ((insn >> 12) & 0xFU)) >> 1;
                unsigned dn = (((insn >> 7) & 1U) << 4) | ((insn >> 16) & 0xFU);
                unsigned dm = (((insn >> 5) & 1U) << 4) | (insn & 0xFU);
                a64_emit_ldr_d_ctx(b, 1U, dn);
                a64_emit_ldr_d_ctx(b, 2U, dm);
                if (dl_acc) {
                    a64_emit_ldr_q_ctx(b, 0U, qd);
                }
                a64_emit(b, dl_op | (2U << 16) | (1U << 5) | 0U);
                a64_emit_str_q_ctx(b, 0U, qd);
                return true;
            }
        }

        if ((insn & 0x00800000U) != 0U && (size == 1U || size == 2U) && !b4 && is_q) {
            /* Two registers and a scalar: VMLA, VMLS, VMUL, VMLAL, VMLSL, VMULL, VQ* by Dm[x] */
            unsigned dm   = (size == 1U) ? (insn & 7U) : (insn & 0xFU);
            unsigned lane = (size == 1U) ? ((((insn >> 5) & 1U) << 1) | ((insn >> 3) & 1U))
                                         : ((insn >> 5) & 1U);
            unsigned imm5 = (size == 1U) ? ((lane << 2) | 2U) : ((lane << 3) | 4U);
            bool is_long  = (op == 0x2U || op == 0x3U || op == 0x6U ||
                             op == 0x7U || op == 0xAU || op == 0xBU);
            uint32_t sc_op = 0U;
            bool sc_acc    = false;

            if (is_long) {
                if (op == 0x2U) { /* VMLAL */
                    sc_op  = (u_bit ? 0x2E208000U : 0x0E208000U) | (size << 22);
                    sc_acc = true;
                } else if (op == 0x3U && !u_bit) { /* VQDMLAL */
                    sc_op  = 0x0E209000U | (size << 22);
                    sc_acc = true;
                } else if (op == 0x6U) { /* VMLSL */
                    sc_op  = (u_bit ? 0x2E20A000U : 0x0E20A000U) | (size << 22);
                    sc_acc = true;
                } else if (op == 0x7U && !u_bit) { /* VQDMLSL */
                    sc_op  = 0x0E20B000U | (size << 22);
                    sc_acc = true;
                } else if (op == 0xAU) { /* VMULL */
                    sc_op  = (u_bit ? 0x2E20C000U : 0x0E20C000U) | (size << 22);
                } else if (op == 0xBU && !u_bit) { /* VQDMULL */
                    sc_op  = 0x0E20D000U | (size << 22);
                }
                if (sc_op != 0U) {
                    unsigned qd = ((((insn >> 22) & 1U) << 4) | ((insn >> 12) & 0xFU)) >> 1;
                    unsigned dn = (((insn >> 7) & 1U) << 4) | ((insn >> 16) & 0xFU);
                    a64_emit_ldr_d_ctx(b, 1U, dn);
                    a64_emit_ldr_d_ctx(b, 2U, dm);
                    a64_emit(b, 0x0E000400U | (imm5 << 16) | (2U << 5) | 2U); /* DUP v2, v2[lane] */
                    if (sc_acc) {
                        a64_emit_ldr_q_ctx(b, 0U, qd);
                    }
                    a64_emit(b, sc_op | (2U << 16) | (1U << 5) | 0U);
                    a64_emit_str_q_ctx(b, 0U, qd);
                    return true;
                }
            } else {
                bool vec_q = u_bit;
                if (op == 0x0U) { /* VMLA.I<16/32> */
                    sc_op  = (vec_q ? 0x4E209400U : 0x0E209400U) | (size << 22);
                    sc_acc = true;
                } else if (op == 0x1U && size == 2U) { /* VMLA.F32 */
                    sc_op  = vec_q ? 0x4E20CC00U : 0x0E20CC00U;
                    sc_acc = true;
                } else if (op == 0x4U) { /* VMLS.I<16/32> */
                    sc_op  = (vec_q ? 0x6E209400U : 0x2E209400U) | (size << 22);
                    sc_acc = true;
                } else if (op == 0x5U && size == 2U) { /* VMLS.F32 */
                    sc_op  = vec_q ? 0x4EA0CC00U : 0x0EA0CC00U;
                    sc_acc = true;
                } else if (op == 0x8U) { /* VMUL.I<16/32> */
                    sc_op  = (vec_q ? 0x4E209C00U : 0x0E209C00U) | (size << 22);
                } else if (op == 0x9U && size == 2U) { /* VMUL.F32 */
                    sc_op  = vec_q ? 0x6E20DC00U : 0x2E20DC00U;
                } else if (op == 0xCU) { /* VQDMULH.S<16/32> */
                    sc_op  = (vec_q ? 0x4E20B400U : 0x0E20B400U) | (size << 22);
                } else if (op == 0xDU) { /* VQRDMULH.S<16/32> */
                    sc_op  = (vec_q ? 0x6E20B400U : 0x2E20B400U) | (size << 22);
                }
                if (sc_op != 0U) {
                    a64_emit_ldr_d_ctx(b, 2U, dm);
                    a64_emit(b, (vec_q ? 0x4E000400U : 0x0E000400U) |
                                (imm5 << 16) | (2U << 5) | 2U); /* DUP v2, v2[lane] */
                    if (vec_q) {
                        unsigned qd = ((((insn >> 22) & 1U) << 4) | ((insn >> 12) & 0xFU)) >> 1;
                        unsigned qn = ((((insn >> 7) & 1U) << 4) | ((insn >> 16) & 0xFU)) >> 1;
                        a64_emit_ldr_q_ctx(b, 1U, qn);
                        if (sc_acc) {
                            a64_emit_ldr_q_ctx(b, 0U, qd);
                        }
                        a64_emit(b, sc_op | (2U << 16) | (1U << 5) | 0U);
                        a64_emit_str_q_ctx(b, 0U, qd);
                    } else {
                        unsigned dd = (((insn >> 22) & 1U) << 4) | ((insn >> 12) & 0xFU);
                        unsigned dn = (((insn >> 7) & 1U) << 4) | ((insn >> 16) & 0xFU);
                        a64_emit_ldr_d_ctx(b, 1U, dn);
                        if (sc_acc) {
                            a64_emit_ldr_d_ctx(b, 0U, dd);
                        }
                        a64_emit(b, sc_op | (2U << 16) | (1U << 5) | 0U);
                        a64_emit_str_d_ctx(b, 0U, dd);
                    }
                    return true;
                }
            }
        }

        if ((insn & 0x00800000U) != 0U) {
            return false;
        }

        uint32_t a64_op = 0U;
        bool needs_acc  = false;
        bool swap_rn_rm = false;
        bool matched    = false;

        /* VHADD (op == 0, !b4) */
        if (op == 0x0U && !b4 && size < 3U) {
            uint32_t base = u_bit ? (is_q ? 0x6E200400U : 0x2E200400U)
                                  : (is_q ? 0x4E200400U : 0x0E200400U);
            a64_op = base | (size << 22);
            matched = true;
        }
        /* VQADD (op == 0, b4) */
        else if (op == 0x0U && b4) {
            if (size == 3U && !is_q) {
                a64_op = 0x5EE00C00U | (u_bit ? 0x20000000U : 0U);
            } else {
                uint32_t base = u_bit ? (is_q ? 0x6E200C00U : 0x2E200C00U)
                                      : (is_q ? 0x4E200C00U : 0x0E200C00U);
                a64_op = base | (size << 22);
            }
            matched = true;
        }
        /* VRHADD (op == 1, !b4) */
        else if (op == 0x1U && !b4 && size < 3U) {
            uint32_t base = u_bit ? (is_q ? 0x6E201400U : 0x2E201400U)
                                  : (is_q ? 0x4E201400U : 0x0E201400U);
            a64_op = base | (size << 22);
            matched = true;
        }
        /* VHSUB (op == 2, !b4) */
        else if (op == 0x2U && !b4 && size < 3U) {
            uint32_t base = u_bit ? (is_q ? 0x6E202400U : 0x2E202400U)
                                  : (is_q ? 0x4E202400U : 0x0E202400U);
            a64_op = base | (size << 22);
            matched = true;
        }
        /* VQSUB (op == 2, b4) */
        else if (op == 0x2U && b4) {
            if (size == 3U && !is_q) {
                a64_op = 0x5EE02C00U | (u_bit ? 0x20000000U : 0U);
            } else {
                uint32_t base = u_bit ? (is_q ? 0x6E202C00U : 0x2E202C00U)
                                      : (is_q ? 0x4E202C00U : 0x0E202C00U);
                a64_op = base | (size << 22);
            }
            matched = true;
        }
        /* VADD.I* */
        else if (!u_bit && op == 0x8U && !b4) {
            if (size == 3U && !is_q) {
                a64_op = 0x5EE08400U;
            } else {
                a64_op = (is_q ? 0x4E208400U : 0x0E208400U) | (size << 22);
            }
            matched = true;
        }
        /* VSUB.I* */
        else if (u_bit && op == 0x8U && !b4) {
            if (size == 3U && !is_q) {
                a64_op = 0x7EE08400U;
            } else {
                a64_op = (is_q ? 0x6E208400U : 0x2E208400U) | (size << 22);
            }
            matched = true;
        }
        /* VTST (op == 8, b4, !u_bit) */
        else if (!u_bit && op == 0x8U && b4 && size < 3U) {
            a64_op = (is_q ? 0x4E208C00U : 0x0E208C00U) | (size << 22);
            matched = true;
        }
        /* VMUL.I* */
        else if (!u_bit && op == 0x9U && b4 && size < 3U) {
            a64_op = (is_q ? 0x4E209C00U : 0x0E209C00U) | (size << 22);
            matched = true;
        }
        /* VMLA.I* */
        else if (!u_bit && op == 0x9U && !b4 && size < 3U) {
            a64_op = (is_q ? 0x4E209400U : 0x0E209400U) | (size << 22);
            needs_acc = true;
            matched = true;
        }
        /* VMLS.I* */
        else if (u_bit && op == 0x9U && !b4 && size < 3U) {
            a64_op = (is_q ? 0x6E209400U : 0x2E209400U) | (size << 22);
            needs_acc = true;
            matched = true;
        }
        /* VABD.I* (op == 7, !b4) */
        else if (op == 0x7U && !b4 && size < 3U) {
            uint32_t base = u_bit ? (is_q ? 0x6E207400U : 0x2E207400U)
                                  : (is_q ? 0x4E207400U : 0x0E207400U);
            a64_op = base | (size << 22);
            matched = true;
        }
        /* VABA.I* (op == 7, b4) */
        else if (op == 0x7U && b4 && size < 3U) {
            uint32_t base = u_bit ? (is_q ? 0x6E207C00U : 0x2E207C00U)
                                  : (is_q ? 0x4E207C00U : 0x0E207C00U);
            a64_op = base | (size << 22);
            needs_acc = true;
            matched = true;
        }
        /* VPMAX / VPMIN integer (!is_q, op == 0xA) */
        else if (!is_q && op == 0xAU && size < 3U) {
            uint32_t base = b4 ? (u_bit ? 0x2E20AC00U : 0x0E20AC00U)
                               : (u_bit ? 0x2E20A400U : 0x0E20A400U);
            a64_op = base | (size << 22);
            matched = true;
        }
        /* VQDMULH / VQRDMULH (op == 0xB, !b4, size == 1 or 2) */
        else if (op == 0xBU && !b4 && (size == 1U || size == 2U)) {
            uint32_t base = u_bit ? (is_q ? 0x6E20B400U : 0x2E20B400U)
                                  : (is_q ? 0x4E20B400U : 0x0E20B400U);
            a64_op = base | (size << 22);
            matched = true;
        }
        /* VPADD integer (!is_q, !u_bit, op == 0xB, b4) */
        else if (!is_q && !u_bit && op == 0xBU && b4 && size < 3U) {
            a64_op = 0x0E20BC00U | (size << 22);
            matched = true;
        }
        /* VADD.F32 */
        else if (!u_bit && size == 0U && op == 0xDU && !b4) {
            a64_op = is_q ? 0x4E20D400U : 0x0E20D400U;
            matched = true;
        }
        /* VSUB.F32 */
        else if (!u_bit && size == 2U && op == 0xDU && !b4) {
            a64_op = is_q ? 0x4EA0D400U : 0x0EA0D400U;
            matched = true;
        }
        /* VPADD.F32 (u_bit, !is_q, size == 0, op == 0xD, !b4) */
        else if (u_bit && !is_q && size == 0U && op == 0xDU && !b4) {
            a64_op = 0x2E20D400U;
            matched = true;
        }
        /* VABD.F32 (u_bit, size == 2, op == 0xD, !b4) */
        else if (u_bit && size == 2U && op == 0xDU && !b4) {
            a64_op = is_q ? 0x6EA0D400U : 0x2EA0D400U;
            matched = true;
        }
        /* VMUL.F32 */
        else if (u_bit && size == 0U && op == 0xDU && b4) {
            a64_op = is_q ? 0x6E20DC00U : 0x2E20DC00U;
            matched = true;
        }
        /* VMLA.F32 / VFMA.F32 */
        else if (!u_bit && size == 0U && (op == 0xDU || op == 0xCU) && b4) {
            a64_op = is_q ? 0x4E20CC00U : 0x0E20CC00U;
            needs_acc = true;
            matched = true;
        }
        /* VMLS.F32 / VFMS.F32 */
        else if (!u_bit && size == 2U && (op == 0xDU || op == 0xCU) && b4) {
            a64_op = is_q ? 0x4EA0CC00U : 0x0EA0CC00U;
            needs_acc = true;
            matched = true;
        }
        /* VCEQ.F32 */
        else if (!u_bit && size == 0U && op == 0xEU && !b4) {
            a64_op = is_q ? 0x4E20E400U : 0x0E20E400U;
            matched = true;
        }
        /* VCGE.F32 */
        else if (u_bit && size == 0U && op == 0xEU && !b4) {
            a64_op = is_q ? 0x6E20E400U : 0x2E20E400U;
            matched = true;
        }
        /* VCGT.F32 */
        else if (u_bit && size == 2U && op == 0xEU && !b4) {
            a64_op = is_q ? 0x6EA0E400U : 0x2EA0E400U;
            matched = true;
        }
        /* VACGE.F32 / VACGT.F32 (u_bit, op == 0xE, b4) */
        else if (u_bit && (size == 0U || size == 2U) && op == 0xEU && b4) {
            uint32_t base = (size == 2U) ? 0x2EA0EC00U : 0x2E20EC00U;
            a64_op = base | (is_q ? 0x40000000U : 0U);
            matched = true;
        }
        /* VMAX.F32 / VMIN.F32 (!u_bit, op == 0xF, !b4) */
        else if (!u_bit && (size == 0U || size == 2U) && op == 0xFU && !b4) {
            uint32_t base = (size == 2U) ? 0x0EA0F400U : 0x0E20F400U;
            a64_op = base | (is_q ? 0x40000000U : 0U);
            matched = true;
        }
        /* VRECPS.F32 / VRSQRTS.F32 (!u_bit, op == 0xF, b4) */
        else if (!u_bit && (size == 0U || size == 2U) && op == 0xFU && b4) {
            uint32_t base = (size == 2U) ? 0x0EA0FC00U : 0x0E20FC00U;
            a64_op = base | (is_q ? 0x40000000U : 0U);
            matched = true;
        }
        /* VPMAX.F32 / VPMIN.F32 (u_bit, !is_q, op == 0xF, !b4) */
        else if (u_bit && !is_q && (size == 0U || size == 2U) && op == 0xFU && !b4) {
            a64_op = (size == 2U) ? 0x2EA0F400U : 0x2E20F400U;
            matched = true;
        }
        /* Bitwise: VAND, VBIC, VORR, VORN, VEOR, VBSL, VBIT, VBIF */
        else if (op == 0x1U && b4) {
            if (!u_bit && size == 0U) { /* VAND */
                a64_op = is_q ? 0x4E201C00U : 0x0E201C00U;
            } else if (!u_bit && size == 1U) { /* VBIC */
                a64_op = is_q ? 0x4E601C00U : 0x0E601C00U;
            } else if (!u_bit && size == 2U) { /* VORR */
                a64_op = is_q ? 0x4EA01C00U : 0x0EA01C00U;
            } else if (!u_bit && size == 3U) { /* VORN */
                a64_op = is_q ? 0x4EE01C00U : 0x0EE01C00U;
            } else if (u_bit && size == 0U) { /* VEOR */
                a64_op = is_q ? 0x6E201C00U : 0x2E201C00U;
            } else if (u_bit && size == 1U) { /* VBSL */
                a64_op = is_q ? 0x6E601C00U : 0x2E601C00U;
                needs_acc = true;
            } else if (u_bit && size == 2U) { /* VBIT */
                a64_op = is_q ? 0x6EA01C00U : 0x2EA01C00U;
                needs_acc = true;
            } else if (u_bit && size == 3U) { /* VBIF */
                a64_op = is_q ? 0x6EE01C00U : 0x2EE01C00U;
                needs_acc = true;
            }
            matched = (a64_op != 0U);
        }
        /* VCEQ register */
        else if (u_bit && op == 0x8U && b4 && size < 3U) {
            a64_op = (is_q ? 0x6E208C00U : 0x2E208C00U) | (size << 22);
            matched = true;
        }
        /* VCGE */
        else if (op == 0x3U && b4 && size < 3U) {
            uint32_t base = u_bit ? (is_q ? 0x6E203C00U : 0x2E203C00U)
                                  : (is_q ? 0x4E203C00U : 0x0E203C00U);
            a64_op = base | (size << 22);
            matched = true;
        }
        /* VCGT */
        else if (op == 0x3U && !b4 && size < 3U) {
            uint32_t base = u_bit ? (is_q ? 0x6E203400U : 0x2E203400U)
                                  : (is_q ? 0x4E203400U : 0x0E203400U);
            a64_op = base | (size << 22);
            matched = true;
        }
        /* VMAX / VMIN */
        else if (op == 0x6U && size < 3U) {
            bool is_min = b4;
            if (is_min) {
                uint32_t base = u_bit ? (is_q ? 0x6E206C00U : 0x2E206C00U)
                                      : (is_q ? 0x4E206C00U : 0x0E206C00U);
                a64_op = base | (size << 22);
            } else {
                uint32_t base = u_bit ? (is_q ? 0x6E206400U : 0x2E206400U)
                                      : (is_q ? 0x4E206400U : 0x0E206400U);
                a64_op = base | (size << 22);
            }
            matched = true;
        }
        /* VSHL / VQSHL / VRSHL / VQRSHL register (op == 4 or 5) */
        else if (op == 0x4U || op == 0x5U) {
            uint32_t sh_opc = (op == 0x4U) ? (b4 ? 0x4C00U : 0x4400U)
                                           : (b4 ? 0x5C00U : 0x5400U);
            if (size == 3U && !is_q) {
                a64_op = 0x5EE00000U | sh_opc | (u_bit ? 0x20000000U : 0U);
                swap_rn_rm = true;
                matched = true;
            } else {
                uint32_t base = u_bit ? (is_q ? 0x6E200000U : 0x2E200000U)
                                      : (is_q ? 0x4E200000U : 0x0E200000U);
                a64_op = base | sh_opc | (size << 22);
                swap_rn_rm = true;
                matched = true;
            }
        }

        if (matched) {
            uint32_t rn_rm = swap_rn_rm ? ((1U << 16) | (2U << 5))
                                        : ((2U << 16) | (1U << 5));
            if (is_q) {
                unsigned qd = ((((insn >> 22) & 1U) << 4) | ((insn >> 12) & 0xFU)) >> 1;
                unsigned qn = ((((insn >> 7) & 1U) << 4) | ((insn >> 16) & 0xFU)) >> 1;
                unsigned qm = ((((insn >> 5) & 1U) << 4) | (insn & 0xFU)) >> 1;
                a64_emit_ldr_q_ctx(b, 1U, qn);
                a64_emit_ldr_q_ctx(b, 2U, qm);
                if (needs_acc) {
                    a64_emit_ldr_q_ctx(b, 0U, qd);
                }
                a64_emit(b, a64_op | rn_rm | 0U);
                a64_emit_str_q_ctx(b, 0U, qd);
            } else {
                unsigned dd = (((insn >> 22) & 1U) << 4) | ((insn >> 12) & 0xFU);
                unsigned dn = (((insn >> 7) & 1U) << 4) | ((insn >> 16) & 0xFU);
                unsigned dm = (((insn >> 5) & 1U) << 4) | (insn & 0xFU);
                a64_emit_ldr_d_ctx(b, 1U, dn);
                a64_emit_ldr_d_ctx(b, 2U, dm);
                if (needs_acc) {
                    a64_emit_ldr_d_ctx(b, 0U, dd);
                }
                a64_emit(b, a64_op | rn_rm | 0U);
                a64_emit_str_d_ctx(b, 0U, dd);
            }
            return true;
        }
    }

    return false;
}

bool a32_decode_vfp_neon_a32(a32_ctx_t *ctx, a64_buf_t *b, uint32_t insn, uint32_t pc)
{
    (void)ctx;
    unsigned cond = (insn >> 28) & 0xFU;

    /* Unconditional: NEON instructions */
    if (cond == 15U) {
        return decode_neon(b, insn, pc);
    }

    /* Conditional: VFPv4-D32 instructions */
    return decode_vfp(b, insn, pc);
}

bool a32_decode_vfp_neon_t32(a32_ctx_t *ctx, a64_buf_t *b, uint16_t hw1, uint16_t hw2, uint32_t pc)
{
    uint32_t insn = ((uint32_t)hw1 << 16) | hw2;
    if ((hw1 & 0xFF00U) == 0xEF00U) {
        insn = 0xF2000000U | (((uint32_t)hw1 & 0x00FFU) << 16) | hw2;
    } else if ((hw1 & 0xFF00U) == 0xFF00U) {
        insn = 0xF3000000U | (((uint32_t)hw1 & 0x00FFU) << 16) | hw2;
    } else if ((hw1 & 0xFF00U) == 0xF900U) {
        insn = 0xF4000000U | (((uint32_t)hw1 & 0x00FFU) << 16) | hw2;
    }
    uint32_t thumb_pc_base = ((pc + 4U) & ~3U) - 8U;
    return a32_decode_vfp_neon_a32(ctx, b, insn, thumb_pc_base);
}
