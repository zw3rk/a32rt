/*
 * src/interp/interp_neon_mem.c — NEON memory, imm & shift lowering
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "interp/decode_priv.h"

bool a32_decode_neon(a64_buf_t *b, uint32_t insn, uint32_t pc)
{
    /* 1. VLD1 / VST1 / VLD2 / VST2 (0xF4...) */
    if ((insn & 0xFF100000U) == 0xF4000000U) {
        bool l_bit    = ((insn >> 21) & 1U) != 0U;
        unsigned rn   = (insn >> 16) & 0xFU;
        unsigned d_start = neon_reg_d(insn);
        unsigned type = (insn >> 8) & 0xFU;
        unsigned rm   = insn & 0xFU;

        if ((insn & 0x00800000U) != 0U) {
            /* VLD1..4 / VST1..4 single element to one lane or all lanes */
            unsigned size        = (insn >> 10) & 3U;
            unsigned n_structs   = ((insn >> 8) & 3U) + 1U;
            unsigned index_align = (insn >> 4) & 0xFU;
            unsigned dup_size = (insn >> 6) & 3U;
            if (n_structs == 4U && dup_size == 3U && (insn & 0x10U) != 0U) {
                dup_size = 2U;
            }
            if (size == 3U && (!l_bit || dup_size > 2U)) {
                return false;
            }
            unsigned base_reg = a32_load_reg(b, rn, pc + 8U, A64_TMP0);
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
                neon_emit_wb(b, rn, rm, (uint16_t)(n_structs * elem_bytes));
                return true;
            }
            if (size == 3U && l_bit) {
                /* VLD1..4.<8/16/32> {Dd[], ...}, [Rn] */
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
                neon_emit_wb(b, rn, rm, (uint16_t)(n_structs * elem_bytes));
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
                unsigned base_reg = a32_load_reg(b, rn, pc + 8U, A64_TMP0);
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
                neon_emit_wb(b, rn, rm, (uint16_t)(nregs * 8U));
                return true;
            }

            /* VLD2 / VST2 (type == 0x8, 0x9, or 0x3) */
            if (type == 0x8U || type == 0x9U || type == 0x3U) {
                unsigned elem_sz = (insn >> 6) & 3U;
                if (elem_sz > 2U) {
                    return false;
                }
                unsigned base_reg = a32_load_reg(b, rn, pc + 8U, A64_TMP0);
                a64_emit_mov_w(b, A64_ADDR, base_reg);
                if (type == 0x3U) {
                    a64_emit_guest_addr_x(b, A64_TMP1, A64_ADDR, 32U, !l_bit);
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
                neon_emit_wb(b, rn, rm, (type == 0x3U) ? 32U : 16U);
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
                unsigned base_reg = a32_load_reg(b, rn, pc + 8U, A64_TMP0);
                a64_emit_mov_w(b, A64_ADDR, base_reg);
                a64_emit_guest_addr_x(b, A64_TMP1, A64_ADDR, n_st * 8U, !l_bit);
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
                neon_emit_wb(b, rn, rm, (uint16_t)(n_st * 8U));
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
        unsigned d    = neon_reg_d(insn);
        if (is_q) {
            d &= ~1U;
        }

        uint64_t val64 = 0ULL;
        if (neon_expand_imm(cmode, op, imm8, &val64)) {
            unsigned count_d = is_q ? 2U : 1U;
            if ((cmode & 1U) == 0U || cmode >= 12U) { /* VMOV or VMVN */
                if (op != 0U && cmode != 14U) {
                    val64 = ~val64;
                }
                a64_emit_mov_imm32(b, A64_TMP0, (uint32_t)val64);
                a64_emit_mov_imm32(b, A64_TMP1, (uint32_t)(val64 >> 32));
                a64_emit_bfi_x(b, A64_TMP0, A64_TMP1, 32U, 32U);
                a64_emit(b, 0x9E670000U | (A64_TMP0 << 5) | 0U); /* FMOV d0, x15 */
                for (unsigned i = 0U; i < count_d; i++) {
                    a64_emit_str_d_ctx(b, 0U, d + i);
                }
            } else { /* VORR or VBIC */
                a64_emit_mov_imm32(b, A64_TMP0, (uint32_t)val64);
                a64_emit_mov_imm32(b, A64_TMP1, (uint32_t)(val64 >> 32));
                a64_emit_bfi_x(b, A64_TMP0, A64_TMP1, 32U, 32U);
                a64_emit(b, 0x9E670000U | (A64_TMP0 << 5) | 1U); /* FMOV d1, x15 */
                uint32_t vec_op = (op == 0U) ? 0x0EA01C00U : 0x0E601C00U;
                for (unsigned i = 0U; i < count_d; i++) {
                    a64_emit_ldr_d_ctx(b, 0U, d + i);
                    a64_emit(b, vec_op | (1U << 16) | (0U << 5) | 0U);
                    a64_emit_str_d_ctx(b, 0U, d + i);
                }
            }
            return true;
        }
    }

    /* 3. NEON 2-register misc, VTBL/VTBX, and VDUP scalar */
    if ((insn & 0x0FB00010U) == 0x03B00000U) {
        bool is_q     = ((insn >> 6) & 1U) != 0U;
        unsigned d    = neon_reg_d(insn);
        unsigned m    = neon_reg_m(insn);
        unsigned opc1 = (insn >> 16) & 3U;
        unsigned size = (insn >> 18) & 3U;

        /* VTBL / VTBX (bits 11..10 == 0b10) */
        if (((insn >> 10) & 3U) == 2U) {
            unsigned len  = ((insn >> 8) & 3U) + 1U;
            unsigned dn   = neon_reg_n(insn);
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
            if (is_q && (d & 1U) != 0U) {
                return false;
            }
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
            neon_str_ctx(b, is_q, 0U, d);
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
                neon_emit_unary(b, is_q, d, m, rev_base | (size << 22), false);
                return true;
            }
            /* VPADDL (op5 == 4 signed, op5 == 5 unsigned) */
            if ((op5 == 4U || op5 == 5U) && size < 3U) {
                uint32_t base = (op5 == 5U) ? 0x2E202800U : 0x0E202800U;
                neon_emit_unary(b, is_q, d, m, base | (size << 22), false);
                return true;
            }
            /* VCLS (op5 == 8), VCLZ (op5 == 9) */
            if ((op5 == 8U || op5 == 9U) && size < 3U) {
                uint32_t base = (op5 == 9U) ? 0x2E204800U : 0x0E204800U;
                neon_emit_unary(b, is_q, d, m, base | (size << 22), false);
                return true;
            }
            /* VCNT (op5 == 10, size == 0) */
            if (op5 == 10U && size == 0U) {
                neon_emit_unary(b, is_q, d, m, 0x0E205800U, false);
                return true;
            }
            /* VMVN (op5 == 11, size == 0) */
            if (op5 == 11U && size == 0U) {
                neon_emit_unary(b, is_q, d, m, 0x2E205800U, false);
                return true;
            }
            /* VPADAL (op5 == 12 signed, op5 == 13 unsigned) */
            if ((op5 == 12U || op5 == 13U) && size < 3U) {
                uint32_t base = (op5 == 13U) ? 0x2E206800U : 0x0E206800U;
                neon_emit_unary(b, is_q, d, m, base | (size << 22), true);
                return true;
            }
            /* VQABS (op5 == 14), VQNEG (op5 == 15) */
            if ((op5 == 14U || op5 == 15U) && size < 3U) {
                uint32_t base = (op5 == 15U) ? 0x2E207800U : 0x0E207800U;
                neon_emit_unary(b, is_q, d, m, base | (size << 22), false);
                return true;
            }
            /* ARMv8-A Crypto: AESE.8, AESD.8, AESMC.8, AESIMC.8 (size == 0, op5 == 6 or 7) */
            if (((insn >> 24) & 1U) != 0U && size == 0U && (op5 == 6U || op5 == 7U)) {
                if ((d & 1U) != 0U || (m & 1U) != 0U) {
                    return false;
                }
                unsigned qd = d >> 1;
                unsigned qm = m >> 1;
                unsigned opc = (insn >> 6) & 0x1FU;
                if (opc == 0x0CU || opc == 0x0DU) { /* AESE.8 / AESD.8 Qd, Qm */
                    a64_emit_ldr_q_ctx(b, 0U, qd);
                    a64_emit_ldr_q_ctx(b, 1U, qm);
                    if (opc == 0x0CU) a64_emit_aese(b, 0U, 1U);
                    else a64_emit_aesd(b, 0U, 1U);
                    a64_emit_str_q_ctx(b, 0U, qd);
                    return true;
                }
                if (opc == 0x0EU || opc == 0x0FU) { /* AESMC.8 / AESIMC.8 Qd, Qm */
                    a64_emit_ldr_q_ctx(b, 1U, qm);
                    if (opc == 0x0EU) a64_emit_aesmc(b, 0U, 1U);
                    else a64_emit_aesimc(b, 0U, 1U);
                    a64_emit_str_q_ctx(b, 0U, qd);
                    return true;
                }
            }
        }

        if (opc1 == 1U) {
            /* ARMv8-A Crypto: SHA1H.32 Qd, Qm (size == 2, opc1 == 1, op5 == 5) */
            if (((insn >> 24) & 1U) != 0U && size == 2U && op5 == 5U) {
                if ((d & 1U) != 0U || (m & 1U) != 0U) {
                    return false;
                }
                a64_emit_ldr_q_ctx(b, 1U, m >> 1);
                a64_emit_sha1h(b, 0U, 1U);
                a64_emit_str_q_ctx(b, 0U, d >> 1);
                return true;
            }
            /* Integer compare-with-zero: VCGT (0), VCGE (1), VCEQ (2), VCLE (3), VCLT (4) #0 */
            if (op5 <= 4U && size < 3U) {
                uint32_t base = ((op5 & 1U) ? 0x2E200800U : 0x0E200800U) |
                                (((8U + (op5 >> 1)) & 0x1FU) << 12) | (size << 22);
                neon_emit_unary(b, is_q, d, m, base, false);
                return true;
            }
            /* VABS.S<8/16/32> (op5 == 6), VNEG.S<8/16/32> (op5 == 7) */
            if ((op5 == 6U || op5 == 7U) && size < 3U) {
                uint32_t base = ((op5 == 7U) ? 0x2E20B800U : 0x0E20B800U) | (size << 22);
                neon_emit_unary(b, is_q, d, m, base, false);
                return true;
            }
            /* Float compare-with-zero: VCGT (8), VCGE (9), VCEQ (10), VCLE (11), VCLT (12) #0.
             * Standard FPSCR: FZ affects FP inputs (A32-R7-24). */
            if (op5 >= 8U && op5 <= 12U && size == 2U) {
                uint32_t base = ((op5 & 1U) ? 0x2EA00800U : 0x0EA00800U) |
                                (((8U + (op5 >> 1)) & 0x1FU) << 12);
                neon_fp_fpcr_begin(b);
                neon_emit_unary(b, is_q, d, m, base, false);
                neon_fp_fpcr_end(b);
                return true;
            }
            /* VABS.F32 (op5 == 14), VNEG.F32 (op5 == 15) */
            if ((op5 == 14U || op5 == 15U) && size == 2U) {
                uint32_t base = (op5 == 15U) ? 0x2EA0F800U : 0x0EA0F800U;
                neon_fp_fpcr_begin(b);
                neon_emit_unary(b, is_q, d, m, base, false);
                neon_fp_fpcr_end(b);
                return true;
            }
        }

        if (opc1 == 2U) {
            /* ARMv8-A Crypto: SHA1SU1.32 & SHA256SU0.32 Qd, Qm (size == 2, opc1 == 2, op5 == 7) */
            if (((insn >> 24) & 1U) != 0U && size == 2U && op5 == 7U) {
                if ((d & 1U) != 0U || (m & 1U) != 0U) {
                    return false;
                }
                unsigned qd = d >> 1;
                unsigned qm = m >> 1;
                a64_emit_ldr_q_ctx(b, 0U, qd);
                a64_emit_ldr_q_ctx(b, 1U, qm);
                if (!is_q) {
                    a64_emit_sha1su1(b, 0U, 1U);
                } else {
                    a64_emit_sha256su0(b, 0U, 1U);
                }
                a64_emit_str_q_ctx(b, 0U, qd);
                return true;
            }

            /* VSWP (op5 == 0, size == 0) */
            if (op5 == 0U && size == 0U) {
                neon_ldr_ctx(b, is_q, 0U, d);
                neon_ldr_ctx(b, is_q, 1U, m);
                neon_str_ctx(b, is_q, 1U, d);
                neon_str_ctx(b, is_q, 0U, m);
                return true;
            }

            /* VTRN (op5 == 1), VUZP (op5 == 2), VZIP (op5 == 3) */
            if (op5 >= 1U && op5 <= 3U && size < 3U) {
                uint32_t q_bit = is_q ? 0x40000000U : 0U;
                uint32_t op1_base = ((op5 == 1U) ? 0x0E002800U : (op5 == 2U) ? 0x0E001800U : 0x0E003800U) | q_bit;
                uint32_t op2_base = ((op5 == 1U) ? 0x0E006800U : (op5 == 2U) ? 0x0E005800U : 0x0E007800U) | q_bit;
                neon_ldr_ctx(b, is_q, 1U, d);
                neon_ldr_ctx(b, is_q, 2U, m);
                a64_emit(b, op1_base | (size << 22) | (2U << 16) | (1U << 5) | 0U);
                a64_emit(b, op2_base | (size << 22) | (2U << 16) | (1U << 5) | 3U);
                neon_str_ctx(b, is_q, 0U, d);
                neon_str_ctx(b, is_q, 3U, m);
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

            /* VCVT.F16.F32 Dd, Qm (op5 == 12) and VCVT.F32.F16 Qd, Dm (op5 == 14).
             * Standard FPSCR around the FP conversion (A32-R7-24). */
            if (size == 1U && !is_q && (op5 == 12U || op5 == 14U)) {
                neon_fp_fpcr_begin(b);
                if (op5 == 12U) {
                    a64_emit_ldr_q_ctx(b, 1U, m >> 1);
                    a64_emit(b, 0x0E216800U | (1U << 5) | 0U);
                    a64_emit_str_d_ctx(b, 0U, d);
                } else {
                    a64_emit_ldr_d_ctx(b, 1U, m);
                    a64_emit(b, 0x0E217800U | (1U << 5) | 0U);
                    a64_emit_str_q_ctx(b, 0U, d >> 1);
                }
                neon_fp_fpcr_end(b);
                return true;
            }
        }

        if (opc1 == 3U && size == 2U && op5 >= 8U && op5 <= 15U) {
            /* VRECPE/VRSQRTE (8..11) and VCVT.F32<->S32/U32 (12..15).
             * Standard FPSCR around the FP op (A32-R7-24). */
            neon_fp_fpcr_begin(b);
            uint32_t base = (op5 == 8U)  ? 0x0EA1C800U
                          : (op5 == 9U)  ? 0x2EA1C800U
                          : (op5 == 10U) ? 0x0EA1D800U
                          : (op5 == 11U) ? 0x2EA1D800U
                          : (op5 == 12U) ? 0x0E21D800U
                          : (op5 == 13U) ? 0x2E21D800U
                          : (op5 == 14U) ? 0x0EA1B800U
                                         : 0x2EA1B800U;
            neon_emit_unary(b, is_q, d, m, base, false);
            neon_fp_fpcr_end(b);
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
        unsigned d = neon_reg_d(insn);
        unsigned n = neon_reg_n(insn);
        unsigned m = neon_reg_m(insn);
        neon_ldr_ctx(b, is_q, 1U, n);
        neon_ldr_ctx(b, is_q, 2U, m);
        uint32_t ext_op = is_q ? (0x6E000000U | ((imm4 & 15U) << 11))
                               : (0x2E000000U | ((imm4 & 7U) << 11));
        a64_emit(b, ext_op | (2U << 16) | (1U << 5) | 0U);
        neon_str_ctx(b, is_q, 0U, d);
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
            unsigned qd = neon_reg_d(insn) >> 1;
            unsigned dm = neon_reg_m(insn);
            uint32_t base = is_u ? 0x2F00A400U : 0x0F00A400U;
            a64_emit_ldr_d_ctx(b, 1U, dm);
            a64_emit(b, base | (imm6 << 16) | (1U << 5) | 0U);
            a64_emit_str_q_ctx(b, 0U, qd);
            return true;
        }

        if ((sh_op == 0x8U || sh_op == 0x9U) && l_sh == 0U && imm6 >= 8U) {
            /* VSHRN / VRSHRN / VQSHRUN / VQRSHRUN (sh_op == 8), VQSHRN / VQRSHRN (sh_op == 9) */
            unsigned dd = neon_reg_d(insn);
            unsigned qm = neon_reg_m(insn) >> 1;
            uint32_t base = (is_u ? 0x2F000000U : 0x0F000000U) |
                            ((sh_op == 0x9U) ? 0x9400U : 0x8400U) |
                            (is_q ? 0x0800U : 0U);
            a64_emit_ldr_q_ctx(b, 1U, qm);
            a64_emit(b, base | (imm6 << 16) | (1U << 5) | 0U);
            a64_emit_str_d_ctx(b, 0U, dd);
            return true;
        }

        if ((sh_op == 0xEU || sh_op == 0xFU) && l_sh == 0U && imm6 >= 32U) {
            /* Vector fixed-point VCVT.F32.<S32/U32> (0xE) and VCVT.<S32/U32>.F32 (0xF).
             * Standard FPSCR around the FP conversion (A32-R7-24). */
            neon_fp_fpcr_begin(b);
            unsigned d = neon_reg_d(insn);
            unsigned m = neon_reg_m(insn);
            uint32_t base = (is_u ? 0x2F000000U : 0x0F000000U) |
                            ((sh_op == 0xFU) ? 0xFC00U : 0xE400U) |
                            (imm6 << 16);
            neon_emit_unary(b, is_q, d, m, base, false);
            neon_fp_fpcr_end(b);
            return true;
        }

        if ((sh_op <= 0x7U) && (l_sh != 0U || imm6 >= 8U)) {
            if ((sh_op == 0x4U || sh_op == 0x6U) && !is_u) {
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
            unsigned d = neon_reg_d(insn);
            unsigned m = neon_reg_m(insn);
            if (needs_dst) {
                neon_ldr_ctx(b, is_q, 0U, d);
            }
            neon_ldr_ctx(b, is_q, 1U, m);
            uint32_t base = is_q ? ((is_u ? 0x6F000000U : 0x4F000000U) | opc)
                          : (l_sh != 0U) ? ((is_u ? 0x7F000000U : 0x5F000000U) | opc)
                                         : ((is_u ? 0x2F000000U : 0x0F000000U) | opc);
            a64_emit(b, base | (imm7 << 16) | (1U << 5) | 0U);
            neon_str_ctx(b, is_q, 0U, d);
            return true;
        }
    }

    return a32_decode_neon_alu(b, insn);
}
