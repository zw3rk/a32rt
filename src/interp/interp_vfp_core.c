/*
 * src/interp/interp_vfp_core.c — VFPv4-D32 instruction decoder & AArch64 lowering
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "interp/decode_priv.h"

bool a32_decode_vfp(a64_buf_t *b, uint32_t insn, uint32_t pc)
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
        if (rt == 15U || rt2 == 15U || (l_bit && rt == rt2)) {
            return false;
        }
        if (!is_double) {
            unsigned sm = ((insn & 0xFU) << 1) | ((insn >> 5) & 1U);
            if (sm == 31U) {
                return false;
            }
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
            if (((insn >> 16) & 0xFU) != 1U) {
                return false;
            }
            if (l_bit) {
                if (rt == 15U) { /* APSR_nzcv */
                    a64_emit_ldr_imm_w(b, A64_TMP0, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, fpscr));
                    a64_emit_msr_nzcv(b, A64_TMP0);
                } else {
                    a64_emit_ldr_imm_w(b, A64_TMP0, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, fpscr));
                    a64_emit(b, 0xD53B4420U | A64_TMP1); /* MRS xTmp1, FPSR */
                    a64_emit_mov_imm32(b, A64_TMP2, 0x0800009FU);
                    a64_emit_and_reg_w(b, false, A64_TMP1, A64_TMP1, A64_TMP2, A64_SHIFT_LSL, 0U);
                    a64_emit_orr_reg_w(b, A64_TMP0, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
                    a64_emit_str_imm_w(b, A64_TMP0, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, fpscr));
                    a64_emit_mov_w(b, rt, A64_TMP0);
                }
            } else {
                if (rt == 15U) {
                    return false;
                }
                a64_emit_str_imm_w(b, rt, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, fpscr));
                a64_emit_mov_imm32(b, A64_TMP1, 0x07C00000U);
                a64_emit_and_reg_w(b, false, A64_TMP0, rt, A64_TMP1, A64_SHIFT_LSL, 0U);
                a64_emit(b, 0xD51B4400U | A64_TMP0); /* MSR FPCR, xTmp0 */
                a64_emit_mov_imm32(b, A64_TMP1, 0x0800009FU);
                a64_emit_and_reg_w(b, false, A64_TMP0, rt, A64_TMP1, A64_SHIFT_LSL, 0U);
                a64_emit(b, 0xD51B4420U | A64_TMP0); /* MSR FPSR, xTmp0 */
            }
            return true;
        }

        /* VMOV Sn, Rt / VMOV Rt, Sn */
        if (opc1 == 0U && !is_double) {
            if (rt == 15U) {
                return false;
            }
            unsigned sn = (((insn >> 16) & 0xFU) << 1) | ((insn >> 7) & 1U);
            if (l_bit) {
                a64_emit_ldr_s_ctx(b, 0U, sn);
                a64_emit_fmov_s_to_w(b, rt, 0U);
            } else {
                a64_emit_fmov_w_to_s(b, 0U, rt);
                a64_emit_str_s_ctx(b, 0U, sn);
            }
            return true;
        }

        /* VDUP from core register (bits 11..8 = cp11, bit 23 = 1, bit 20 = 0, bit 6 = 0, bit 4 = 1) */
        if (is_double && !l_bit && ((insn & 0x00800040U) == 0x00800000U)) {
            bool b_bit = ((insn >> 22) & 1U) != 0U;
            bool q_bit = ((insn >> 21) & 1U) != 0U;
            bool e_bit = ((insn >> 5) & 1U) != 0U;
            if (b_bit && e_bit) {
                return false;
            }
            unsigned size = b_bit ? 0U : (e_bit ? 1U : 2U);
            unsigned vd = (((insn >> 7) & 1U) << 4) | ((insn >> 16) & 0xFU);
            if (rt == 15U || (q_bit && (vd & 1U) != 0U)) {
                return false;
            }
            uint32_t a64_insn = (q_bit ? 0x4E000C00U : 0x0E000C00U) |
                                ((1U << size) << 16) | ((rt & 31U) << 5) | 0U;
            a64_emit(b, a64_insn);
            neon_str_ctx(b, q_bit, 0U, vd);
            return true;
        }

        /* VMOV Dm[x], Rt / VMOV Rt, Dm[x] (32-bit, 16-bit, or 8-bit lane) */
        if (is_double && (l_bit || ((insn & 0x00800000U) == 0U))) {
            if (rt == 15U) {
                return false;
            }
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
            unsigned base_reg = a32_load_reg(b, rn, pc + 8U, A64_TMP0);
            if (u_bit) {
                a64_emit_add_imm_w(b, false, A64_ADDR, base_reg, (uint16_t)byte_disp, 0U);
            } else {
                a64_emit_sub_imm_w(b, false, A64_ADDR, base_reg, (uint16_t)byte_disp, 0U);
            }

            unsigned reg = vfp_reg_idx(is_double, d_bit, vd_bits);
            if (l_bit) {
                if (is_double) a64_emit_guest_ldr_d(b, 0U, A64_ADDR);
                else a64_emit_guest_ldr_s(b, 0U, A64_ADDR);
                vfp_str_ctx(b, is_double, 0U, reg);
            } else {
                vfp_ldr_ctx(b, is_double, 0U, reg);
                if (is_double) a64_emit_guest_str_d(b, 0U, A64_ADDR);
                else a64_emit_guest_str_s(b, 0U, A64_ADDR);
            }
            return true;
        }

        /* VLDM / VSTM / VPUSH / VPOP */
        if (imm8 == 0U || p_bit == u_bit) {
            return false;
        }
        uint32_t d_start = ((insn >> 18) & 0x10U) | ((insn >> 12) & 0xFU);
        uint32_t num_d = imm8 / 2U;
        uint32_t s_start = (((insn >> 12) & 0xFU) << 1) | ((insn >> 22) & 1U);
        if (is_double) {
            if ((imm8 & 1U) != 0U || num_d == 0U || num_d > 16U || (d_start + num_d) > 32U) {
                return false;
            }
        } else {
            if ((s_start + imm8) > 32U) {
                return false;
            }
        }
        uint32_t total_bytes = imm8 * 4U;
        unsigned base_reg = a32_load_reg(b, rn, pc + 8U, A64_TMP0);
        if (!u_bit) {
            a64_emit_sub_imm_w(b, false, A64_ADDR, base_reg, (uint16_t)total_bytes, 0U);
        } else {
            a64_emit_mov_w(b, A64_ADDR, base_reg);
        }

        unsigned count = is_double ? num_d : imm8;
        unsigned start = is_double ? d_start : s_start;
        uint16_t step_bytes = is_double ? 8U : 4U;
        for (unsigned i = 0U; i < count; i++) {
            unsigned reg = (start + i) & 31U;
            if (l_bit) {
                if (is_double) a64_emit_guest_ldr_d(b, 0U, A64_ADDR);
                else a64_emit_guest_ldr_s(b, 0U, A64_ADDR);
                vfp_str_ctx(b, is_double, 0U, reg);
            } else {
                vfp_ldr_ctx(b, is_double, 0U, reg);
                if (is_double) a64_emit_guest_str_d(b, 0U, A64_ADDR);
                else a64_emit_guest_str_s(b, 0U, A64_ADDR);
            }
            a64_emit_add_imm_w(b, false, A64_ADDR, A64_ADDR, step_bytes, 0U);
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
        unsigned d_idx = vfp_reg_idx(is_double, d_bit, vd_b);
        unsigned m_idx = vfp_reg_idx(is_double, m_bit, vm_b);

        /* Unary, VCVT, VCMP, VMOV imm (opc1 == 0b1011) */
        if (opc1 == 0xBU) {
            /* VMOV immediate: AArch64 FMOV S0/D0, #imm8 uses identical 8-bit encoding */
            if ((insn & 0xC0U) == 0U) {
                uint32_t imm8 = (((insn >> 16) & 0xFU) << 4) | (insn & 0xFU);
                a64_emit(b, (is_double ? 0x1E601000U : 0x1E201000U) | (imm8 << 13));
                vfp_str_ctx(b, is_double, 0U, d_idx);
                return true;
            }

            if (!op_bit) {
                return false;
            }

            /* VMOV reg / VABS */
            if (opc2 == 0U) {
                vfp_emit_unary(b, is_double, d_idx, m_idx, n_bit ? 0x1E20C000U : 0x1E204000U);
                return true;
            }

            /* VNEG / VSQRT */
            if (opc2 == 1U) {
                vfp_emit_unary(b, is_double, d_idx, m_idx, n_bit ? 0x1E21C000U : 0x1E214000U);
                return true;
            }

            /* VCVTB / VCVTT between half-precision and single-precision */
            if (!is_double && (opc2 == 2U || opc2 == 3U)) {
                if (opc2 == 2U) { /* VCVTB.F32.F16 / VCVTT.F32.F16 Sd, Sm */
                    a64_emit_ldr_s_ctx(b, 1U, m_idx);
                    a64_emit_fmov_s_to_w(b, A64_TMP0, 1U);
                    if (n_bit) {
                        a64_emit_ubfm_w(b, A64_TMP0, A64_TMP0, 16U, 31U);
                    } else {
                        a64_emit_ubfm_w(b, A64_TMP0, A64_TMP0, 0U, 15U);
                    }
                    a64_emit_fmov_w_to_s(b, 1U, A64_TMP0);
                    a64_emit(b, 0x1EE24000U | (1U << 5) | 0U); /* FCVT s0, h1 */
                    a64_emit_str_s_ctx(b, 0U, d_idx);
                } else { /* VCVTB.F16.F32 / VCVTT.F16.F32 Sd, Sm */
                    a64_emit_ldr_s_ctx(b, 1U, m_idx);
                    a64_emit_ldr_s_ctx(b, 2U, d_idx);
                    a64_emit(b, 0x1E23C000U | (1U << 5) | 0U); /* FCVT h0, s1 */
                    a64_emit_fmov_s_to_w(b, A64_TMP0, 0U);
                    a64_emit_fmov_s_to_w(b, A64_TMP1, 2U);
                    if (n_bit) {
                        a64_emit_bfm_w(b, A64_TMP1, A64_TMP0, 16U, 15U);
                    } else {
                        a64_emit_bfm_w(b, A64_TMP1, A64_TMP0, 0U, 15U);
                    }
                    a64_emit_fmov_w_to_s(b, 0U, A64_TMP1);
                    a64_emit_str_s_ctx(b, 0U, d_idx);
                }
                return true;
            }

            /* VCMP / VCMPE */
            if (opc2 == 4U || opc2 == 5U) {
                bool zero = (opc2 == 5U);
                uint32_t e_bit = n_bit ? 0x00000010U : 0U;
                uint32_t sz_bit = is_double ? 0x00400000U : 0U;
                a64_emit_mrs_nzcv(b, A64_TMP1);
                vfp_ldr_ctx(b, is_double, 0U, d_idx);
                if (zero) {
                    a64_emit(b, 0x1E202008U | sz_bit | e_bit);
                } else {
                    vfp_ldr_ctx(b, is_double, 1U, m_idx);
                    a64_emit(b, 0x1E202000U | sz_bit | e_bit | (1U << 16));
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
                    a64_emit_ldr_d_ctx(b, 1U, m_idx);
                    a64_emit_fcvt_d_to_s(b, 0U, 1U);
                    a64_emit_str_s_ctx(b, 0U, s);
                } else { /* vcvt.f64.f32 Dd, Sm */
                    unsigned d = (d_bit << 4) | vd_b;
                    a64_emit_ldr_s_ctx(b, 1U, m_idx);
                    a64_emit_fcvt_s_to_d(b, 0U, 1U);
                    a64_emit_str_d_ctx(b, 0U, d);
                }
                return true;
            }

            /* VRINTZ / VRINTR (opc2 == 6) and VRINTX (opc2 == 7 && !n_bit) */
            if (opc2 == 6U || (opc2 == 7U && !n_bit)) {
                uint32_t a64_op = (opc2 == 6U) ? (n_bit ? 0x1E25C000U : 0x1E27C000U) : 0x1E274000U;
                vfp_emit_unary(b, is_double, d_idx, m_idx, a64_op);
                return true;
            }

            /* VCVT int to float */
            if (opc2 == 8U) {
                unsigned m = (vm_b << 1) | m_bit;
                a64_emit_ldr_s_ctx(b, 1U, m);
                a64_emit_fmov_s_to_w(b, A64_TMP0, 1U);
                if (is_double) {
                    if (n_bit) a64_emit_scvtf_w_to_d(b, 0U, A64_TMP0);
                    else a64_emit_ucvtf_w_to_d(b, 0U, A64_TMP0);
                } else {
                    if (n_bit) a64_emit_scvtf_w_to_s(b, 0U, A64_TMP0);
                    else a64_emit_ucvtf_w_to_s(b, 0U, A64_TMP0);
                }
                vfp_str_ctx(b, is_double, 0U, d_idx);
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
                uint32_t ctx_off = A32_CTX_VFP_OFFSET + (d_idx * (is_double ? 8U : 4U));
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
                    vfp_str_ctx(b, is_double, 0U, d_idx);
                } else {
                    vfp_ldr_ctx(b, is_double, 0U, d_idx);
                    uint32_t base = (is_double ? 0x1E580000U : 0x1E180000U) |
                                    (is_unsigned ? 0x00010000U : 0x00000000U);
                    a64_emit(b, base | (scale << 10) | (0U << 5) | A64_TMP0);
                    if (!n_bit) {
                        a64_emit_mrs_nzcv(b, A64_TMP3);
                        if (is_unsigned) {
                            a64_emit_mov_imm32(b, A64_TMP1, 0xFFFFU);
                            a64_emit_sub_reg_w(b, true, A64_ZR, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
                            a64_emit_csel_w(b, A64_TMP0, A64_TMP1, A64_TMP0, A64_COND_HI);
                        } else {
                            a64_emit_mov_imm32(b, A64_TMP1, 0x7FFFU);
                            a64_emit_sub_reg_w(b, true, A64_ZR, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
                            a64_emit_csel_w(b, A64_TMP0, A64_TMP1, A64_TMP0, A64_COND_GT);
                            a64_emit_mov_imm32(b, A64_TMP1, 0xFFFF8000U);
                            a64_emit_sub_reg_w(b, true, A64_ZR, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 0U);
                            a64_emit_csel_w(b, A64_TMP0, A64_TMP1, A64_TMP0, A64_COND_LT);
                        }
                        a64_emit_msr_nzcv(b, A64_TMP3);
                    }
                    a64_emit_str_imm_w(b, A64_TMP0, A64_XCTX, ctx_off);
                    if (is_double) {
                        if (is_unsigned) {
                            a64_emit_str_imm_w(b, A64_ZR, A64_XCTX, ctx_off + 4U);
                        } else {
                            a64_emit_sbfm_w(b, A64_TMP1, A64_TMP0, 31U, 31U);
                            a64_emit_str_imm_w(b, A64_TMP1, A64_XCTX, ctx_off + 4U);
                        }
                    }
                }
                return true;
            }

            /* VCVT / VCVTR float to int */
            if (opc2 == 12U || opc2 == 13U) {
                bool is_signed  = (opc2 == 13U);
                bool round_zero = n_bit;
                unsigned s = (vd_b << 1) | d_bit;
                vfp_ldr_ctx(b, is_double, 1U, m_idx);
                if (!round_zero) {
                    a64_emit(b, (is_double ? 0x1E674000U : 0x1E274000U) | (1U << 5) | 1U);
                }
                if (is_double) {
                    if (is_signed) a64_emit_fcvtzs_d_to_w(b, A64_TMP0, 1U);
                    else a64_emit_fcvtzu_d_to_w(b, A64_TMP0, 1U);
                } else {
                    if (is_signed) a64_emit_fcvtzs_s_to_w(b, A64_TMP0, 1U);
                    else a64_emit_fcvtzu_s_to_w(b, A64_TMP0, 1U);
                }
                a64_emit_fmov_w_to_s(b, 0U, A64_TMP0);
                a64_emit_str_s_ctx(b, 0U, s);
                return true;
            }

            return false;
        }

        /* Binary arithmetic operations */
        if (!(opc1 <= 3U || (opc1 == 8U && !op_bit) || opc1 == 9U || opc1 == 10U)) {
            return false;
        }
        unsigned n_idx = vfp_reg_idx(is_double, n_bit, vn_b);
        vfp_ldr_ctx(b, is_double, 1U, n_idx);
        vfp_ldr_ctx(b, is_double, 2U, m_idx);
        uint32_t sz_bit = is_double ? 0x00400000U : 0U;

        if (opc1 == 3U) { /* VADD / VSUB */
            a64_emit(b, (op_bit ? 0x1E203800U : 0x1E202800U) | sz_bit | (2U << 16) | (1U << 5) | 0U);
        } else if (opc1 == 2U) { /* VMUL / VNMUL */
            a64_emit(b, (op_bit ? 0x1E208800U : 0x1E200800U) | sz_bit | (2U << 16) | (1U << 5) | 0U);
        } else if (opc1 == 8U) { /* VDIV */
            a64_emit(b, 0x1E201800U | sz_bit | (2U << 16) | (1U << 5) | 0U);
        } else if (opc1 == 0U) { /* VMLA / VMLS */
            vfp_ldr_ctx(b, is_double, 3U, d_idx);
            a64_emit(b, (op_bit ? 0x1E208800U : 0x1E200800U) | sz_bit | (2U << 16) | (1U << 5) | 0U);
            a64_emit(b, 0x1E202800U | sz_bit | (0U << 16) | (3U << 5) | 0U);
        } else if (opc1 == 1U) { /* VNMLA / VNMLS */
            vfp_ldr_ctx(b, is_double, 3U, d_idx);
            a64_emit(b, (op_bit ? 0x1E208800U : 0x1E200800U) | sz_bit | (2U << 16) | (1U << 5) | 0U);
            a64_emit(b, 0x1E214000U | sz_bit | (3U << 5) | 3U);
            a64_emit(b, 0x1E202800U | sz_bit | (0U << 16) | (3U << 5) | 0U);
        } else if (opc1 == 10U) { /* VFMA / VFMS */
            vfp_ldr_ctx(b, is_double, 3U, d_idx);
            a64_emit(b, (op_bit ? 0x1F008000U : 0x1F000000U) | sz_bit | (2U << 16) | (3U << 10) | (1U << 5) | 0U);
        } else { /* VFNMA / VFNMS (opc1 == 9U) */
            vfp_ldr_ctx(b, is_double, 3U, d_idx);
            a64_emit(b, (op_bit ? 0x1F200000U : 0x1F208000U) | sz_bit | (2U << 16) | (3U << 10) | (1U << 5) | 0U);
        }

        vfp_str_ctx(b, is_double, 0U, d_idx);
        return true;
    }

    return false;
}
