/*
 * src/interp/interp_neon_alu.c — NEON ALU lowering and top-level VFP/NEON dispatch
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "interp/decode_priv.h"

bool a32_decode_neon_alu(a64_buf_t *b, uint32_t insn)
{
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
                unsigned dd = neon_reg_d(insn);
                unsigned qn = neon_reg_n(insn) >> 1;
                unsigned qm = neon_reg_m(insn) >> 1;
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
                unsigned qd = neon_reg_d(insn) >> 1;
                unsigned qn = neon_reg_n(insn) >> 1;
                unsigned dm = neon_reg_m(insn);
                uint32_t w_op = (u_bit ? 0x2E200000U : 0x0E200000U) |
                                ((op == 0x3U) ? 0x3000U : 0x1000U) | (size << 22);
                a64_emit_ldr_q_ctx(b, 1U, qn);
                a64_emit_ldr_d_ctx(b, 2U, dm);
                a64_emit(b, w_op | (2U << 16) | (1U << 5) | 0U);
                a64_emit_str_q_ctx(b, 0U, qd);
                return true;
            }
            /* ARMv8-A Crypto: VMULL.P64 Qd, Dn, Dm */
            if (op == 0xEU && !u_bit && size == 2U) {
                unsigned d_full = neon_reg_d(insn);
                if ((d_full & 1U) != 0U) {
                    return false;
                }
                unsigned qd = d_full >> 1;
                unsigned dn = neon_reg_n(insn);
                unsigned dm = neon_reg_m(insn);
                a64_emit_ldr_d_ctx(b, 1U, dn);
                a64_emit_ldr_d_ctx(b, 2U, dm);
                a64_emit_pmull_1q(b, 0U, 1U, 2U);
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
            } else if (op == 0xEU && !u_bit && size == 0U) { /* VMULL.P8 */
                dl_op = 0x0E20E000U;
            }
            if (dl_op != 0U) {
                unsigned qd = neon_reg_d(insn) >> 1;
                unsigned dn = neon_reg_n(insn);
                unsigned dm = neon_reg_m(insn);
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
                    unsigned qd = neon_reg_d(insn) >> 1;
                    unsigned dn = neon_reg_n(insn);
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
                if ((op == 0x1U || op == 0x5U) && size == 2U) {
                    /* Chained (unfused) VMLA.F32 / VMLS.F32 by scalar */
                    uint32_t fadd_op = vec_q ? 0x4E20D400U : 0x0E20D400U;
                    a64_emit_ldr_d_ctx(b, 2U, dm);
                    a64_emit(b, (vec_q ? 0x4E000400U : 0x0E000400U) |
                                (imm5 << 16) | (2U << 5) | 2U); /* DUP v2, v2[lane] */
                    if (vec_q) {
                        unsigned qd = neon_reg_d(insn) >> 1;
                        unsigned qn = neon_reg_n(insn) >> 1;
                        a64_emit_ldr_q_ctx(b, 1U, qn);
                        a64_emit(b, 0x6E20DC00U | (2U << 16) | (1U << 5) | 3U); /* FMUL v3.4s, v1.4s, v2.4s */
                        a64_emit_ldr_q_ctx(b, 0U, qd);
                        if (op == 0x5U) {
                            a64_emit(b, 0x6EA0F800U | (3U << 5) | 3U); /* FNEG v3.4s, v3.4s */
                        }
                        a64_emit(b, fadd_op | (3U << 16) | (0U << 5) | 0U);
                        a64_emit_str_q_ctx(b, 0U, qd);
                    } else {
                        unsigned dd = neon_reg_d(insn);
                        unsigned dn = neon_reg_n(insn);
                        a64_emit_ldr_d_ctx(b, 1U, dn);
                        a64_emit(b, 0x2E20DC00U | (2U << 16) | (1U << 5) | 3U); /* FMUL v3.2s, v1.2s, v2.2s */
                        a64_emit_ldr_d_ctx(b, 0U, dd);
                        if (op == 0x5U) {
                            a64_emit(b, 0x2EA0F800U | (3U << 5) | 3U); /* FNEG v3.2s, v3.2s */
                        }
                        a64_emit(b, fadd_op | (3U << 16) | (0U << 5) | 0U);
                        a64_emit_str_d_ctx(b, 0U, dd);
                    }
                    return true;
                }
                if (op == 0x0U) { /* VMLA.I<16/32> */
                    sc_op  = (vec_q ? 0x4E209400U : 0x0E209400U) | (size << 22);
                    sc_acc = true;
                } else if (op == 0x4U) { /* VMLS.I<16/32> */
                    sc_op  = (vec_q ? 0x6E209400U : 0x2E209400U) | (size << 22);
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
                        unsigned qd = neon_reg_d(insn) >> 1;
                        unsigned qn = neon_reg_n(insn) >> 1;
                        a64_emit_ldr_q_ctx(b, 1U, qn);
                        if (sc_acc) {
                            a64_emit_ldr_q_ctx(b, 0U, qd);
                        }
                        a64_emit(b, sc_op | (2U << 16) | (1U << 5) | 0U);
                        a64_emit_str_q_ctx(b, 0U, qd);
                    } else {
                        unsigned dd = neon_reg_d(insn);
                        unsigned dn = neon_reg_n(insn);
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

        /* ARMv8-A 3-register Crypto: SHA1C/P/M/SU0 & SHA256H/H2/SU1 (op == 0xC, !b4) */
        if (op == 0xCU && !b4) {
            unsigned d_full = neon_reg_d(insn);
            unsigned n_full = neon_reg_n(insn);
            unsigned m_full = neon_reg_m(insn);
            if ((d_full & 1U) != 0U || (n_full & 1U) != 0U || (m_full & 1U) != 0U) {
                return false;
            }
            if (u_bit && size == 3U) {
                return false;
            }
            unsigned qd = d_full >> 1;
            unsigned qn = n_full >> 1;
            unsigned qm = m_full >> 1;
            a64_emit_ldr_q_ctx(b, 0U, qd);
            a64_emit_ldr_q_ctx(b, 1U, qn);
            a64_emit_ldr_q_ctx(b, 2U, qm);
            if (!u_bit) {
                if (size == 0U) {
                    a64_emit_sha1c(b, 0U, 1U, 2U);
                } else if (size == 1U) {
                    a64_emit_sha1p(b, 0U, 1U, 2U);
                } else if (size == 2U) {
                    a64_emit_sha1m(b, 0U, 1U, 2U);
                } else {
                    a64_emit_sha1su0(b, 0U, 1U, 2U);
                }
            } else {
                if (size == 0U) {
                    a64_emit_sha256h(b, 0U, 1U, 2U);
                } else if (size == 1U) {
                    a64_emit_sha256h2(b, 0U, 1U, 2U);
                } else {
                    a64_emit_sha256su1(b, 0U, 1U, 2U);
                }
            }
            a64_emit_str_q_ctx(b, 0U, qd);
            return true;
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
        /* VMUL.P8 */
        else if (u_bit && op == 0x9U && b4 && size == 0U) {
            a64_op = is_q ? 0x6E209C00U : 0x2E209C00U;
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
        /* Chained (unfused) VMLA.F32 / VMLS.F32 (op == 0xD, !u_bit, b4, size == 0 or 2) */
        else if (!u_bit && (size == 0U || size == 2U) && op == 0xDU && b4) {
            uint32_t fadd_op = is_q ? 0x4E20D400U : 0x0E20D400U;
            if (is_q) {
                unsigned qd = neon_reg_d(insn) >> 1;
                unsigned qn = neon_reg_n(insn) >> 1;
                unsigned qm = neon_reg_m(insn) >> 1;
                a64_emit_ldr_q_ctx(b, 1U, qn);
                a64_emit_ldr_q_ctx(b, 2U, qm);
                a64_emit(b, 0x6E20DC00U | (2U << 16) | (1U << 5) | 3U); /* FMUL v3.4s, v1.4s, v2.4s */
                a64_emit_ldr_q_ctx(b, 0U, qd);
                if (size == 2U) {
                    a64_emit(b, 0x6EA0F800U | (3U << 5) | 3U); /* FNEG v3.4s, v3.4s */
                }
                a64_emit(b, fadd_op | (3U << 16) | (0U << 5) | 0U);
                a64_emit_str_q_ctx(b, 0U, qd);
            } else {
                unsigned dd = neon_reg_d(insn);
                unsigned dn = neon_reg_n(insn);
                unsigned dm = neon_reg_m(insn);
                a64_emit_ldr_d_ctx(b, 1U, dn);
                a64_emit_ldr_d_ctx(b, 2U, dm);
                a64_emit(b, 0x2E20DC00U | (2U << 16) | (1U << 5) | 3U); /* FMUL v3.2s, v1.2s, v2.2s */
                a64_emit_ldr_d_ctx(b, 0U, dd);
                if (size == 2U) {
                    a64_emit(b, 0x2EA0F800U | (3U << 5) | 3U); /* FNEG v3.2s, v3.2s */
                }
                a64_emit(b, fadd_op | (3U << 16) | (0U << 5) | 0U);
                a64_emit_str_d_ctx(b, 0U, dd);
            }
            return true;
        }
        /* Fused VFMA.F32 (op == 0xC, !u_bit, b4, size == 0) */
        else if (!u_bit && size == 0U && op == 0xCU && b4) {
            a64_op = is_q ? 0x4E20CC00U : 0x0E20CC00U;
            needs_acc = true;
            matched = true;
        }
        /* Fused VFMS.F32 (op == 0xC, !u_bit, b4, size == 2) */
        else if (!u_bit && size == 2U && op == 0xCU && b4) {
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
        /* VMAXNM.F32 / VMINNM.F32 (u_bit, op == 0xF, b4, size == 0 or 2) */
        else if (u_bit && (size == 0U || size == 2U) && op == 0xFU && b4) {
            uint32_t base = (size == 2U) ? 0x0EA0C400U : 0x0E20C400U;
            a64_op = base | (is_q ? 0x40000000U : 0U);
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
                unsigned qd = neon_reg_d(insn) >> 1;
                unsigned qn = neon_reg_n(insn) >> 1;
                unsigned qm = neon_reg_m(insn) >> 1;
                a64_emit_ldr_q_ctx(b, 1U, qn);
                a64_emit_ldr_q_ctx(b, 2U, qm);
                if (needs_acc) {
                    a64_emit_ldr_q_ctx(b, 0U, qd);
                }
                a64_emit(b, a64_op | rn_rm | 0U);
                a64_emit_str_q_ctx(b, 0U, qd);
            } else {
                unsigned dd = neon_reg_d(insn);
                unsigned dn = neon_reg_n(insn);
                unsigned dm = neon_reg_m(insn);
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

static bool decode_vfp_uncond(a64_buf_t *b, uint32_t insn)
{
    unsigned cp_num = (insn >> 8) & 0xFU;
    if (cp_num != 10U && cp_num != 11U) {
        return false;
    }
    bool is_double = (cp_num == 11U);
    bool d_bit     = ((insn >> 22) & 1U) != 0U;
    bool n_bit     = ((insn >> 7) & 1U) != 0U;
    bool m_bit     = ((insn >> 5) & 1U) != 0U;
    unsigned vn_b  = (insn >> 16) & 0xFU;
    unsigned vd_b  = (insn >> 12) & 0xFU;
    unsigned vm_b  = insn & 0xFU;
    unsigned d_idx = vfp_reg_idx(is_double, d_bit, vd_b);
    unsigned n_idx = vfp_reg_idx(is_double, n_bit, vn_b);
    unsigned m_idx = vfp_reg_idx(is_double, m_bit, vm_b);

    /* A. VSEL{EQ,VS,GE,GT}.F32 / .F64 */
    if ((insn & 0xFF800E50U) == 0xFE000A00U) {
        unsigned cc = (insn >> 20) & 3U;
        static const uint32_t vsel_cond_map[4] = {
            A64_COND_EQ, /* 0: EQ */
            A64_COND_VS, /* 1: VS */
            A64_COND_GE, /* 2: GE */
            A64_COND_GT  /* 3: GT */
        };
        uint32_t a64_cond = vsel_cond_map[cc];

        vfp_ldr_ctx(b, is_double, 0U, n_idx);
        vfp_ldr_ctx(b, is_double, 1U, m_idx);
        a64_emit(b, (is_double ? 0x1E600C00U : 0x1E200C00U) | (1U << 16) | (a64_cond << 12) | (0U << 5) | 0U);
        vfp_str_ctx(b, is_double, 0U, d_idx);
        return true;
    }

    /* B. VMAXNM.F32 / .F64 and VMINNM.F32 / .F64 */
    if ((insn & 0xFFB00E10U) == 0xFE800A00U) {
        bool is_min = ((insn >> 6) & 1U) != 0U;
        vfp_ldr_ctx(b, is_double, 0U, n_idx);
        vfp_ldr_ctx(b, is_double, 1U, m_idx);
        a64_emit(b, (is_double ? 0x1E616800U : 0x1E216800U) | (is_min ? 0x00001000U : 0U));
        vfp_str_ctx(b, is_double, 0U, d_idx);
        return true;
    }

    /* C. VCVT{A,N,P,M}.S32 / .U32 from .F32 / .F64 */
    if ((insn & 0xFFBC0E50U) == 0xFEBC0A40U) {
        unsigned rm = (insn >> 16) & 3U;
        bool is_signed = ((insn >> 7) & 1U) != 0U;
        unsigned sd = (vd_b << 1) | (d_bit ? 1U : 0U);

        vfp_ldr_ctx(b, is_double, 1U, m_idx);

        uint32_t base;
        switch (rm) {
        case 0U: base = 0x1E240000U; break; /* FCVTA */
        case 1U: base = 0x1E200000U; break; /* FCVTN */
        case 2U: base = 0x1E280000U; break; /* FCVTP */
        default: base = 0x1E300000U; break; /* FCVTM */
        }
        uint32_t insn_a64 = base | (is_double ? 0x00400000U : 0U) |
                            (is_signed ? 0U : 0x00010000U) | (1U << 5) | A64_TMP0;
        a64_emit(b, insn_a64);
        a64_emit_fmov_w_to_s(b, 0U, A64_TMP0);
        a64_emit_str_s_ctx(b, 0U, sd);
        return true;
    }

    /* D. VRINT{A,N,P,M}.F32 / .F64 */
    if ((insn & 0xFFBC0ED0U) == 0xFEB80A40U) {
        unsigned rm = (insn >> 16) & 3U;
        uint32_t a64_base;
        switch (rm) {
        case 0U: a64_base = 0x1E264000U; break; /* FRINTA */
        case 1U: a64_base = 0x1E244000U; break; /* FRINTN */
        case 2U: a64_base = 0x1E24C000U; break; /* FRINTP */
        default: a64_base = 0x1E254000U; break; /* FRINTM */
        }
        vfp_emit_unary(b, is_double, d_idx, m_idx, a64_base);
        return true;
    }

    return false;
}

bool a32_decode_vfp_neon_a32(a64_buf_t *b, uint32_t insn, uint32_t pc)
{
    unsigned cond = (insn >> 28) & 0xFU;

    /* Unconditional: NEON instructions and ARMv8-A unconditional VFP (0xFE......) */
    if (cond == 15U) {
        if (a32_decode_neon(b, insn, pc)) {
            return true;
        }
        return decode_vfp_uncond(b, insn);
    }

    /* Conditional: VFPv4-D32 instructions */
    return a32_decode_vfp(b, insn, pc);
}

bool a32_decode_vfp_neon_t32(a64_buf_t *b, uint16_t hw1, uint16_t hw2, uint32_t pc)
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
    return a32_decode_vfp_neon_a32(b, insn, thumb_pc_base);
}
