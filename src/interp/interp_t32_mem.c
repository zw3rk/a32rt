/*
 * src/interp/interp_t32_mem.c - split-TU part 3/3 of src/decode_t32.c (lines 1157-1824).
 * Compiled only via the src/decode_t32.c wrapper; do not add to Makefile SRCS.
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

        if ((hw1 & 0xFF80U) == 0xFA00U && (hw2 & 0xF0F0U) == 0xF000U) {
            /* LSL.W / LSR.W / ASR.W / ROR.W Rd, Rn, Rm */
            unsigned shift_type = (hw1 >> 5) & 3U;
            bool set_flags = ((hw1 >> 4) & 1U) != 0U;
            /* Intra-block dead-NZCV flag liveness elimination */
            if (set_flags && rd != 15U) {
                if (!flags_live) {
                    set_flags = false;
                    ctx->flags_elided++;
                }
            }
            a64_emit_arm_shift_reg_w(b, rd, rn, rm, shift_type, set_flags);
            if (set_flags) {
                a64_emit_update_nzc_keep_v_reg(b, rd, A64_TMP2);
            }
            return true;
        }
        if ((hw1 & 0xFF80U) == 0xFA00U && (hw2 & 0xF0C0U) == 0xF080U) {
            /* SXTH / UXTH / SXTB16 / UXTB16 / SXTB / UXTB (+ add variants) */
            unsigned op = (hw1 >> 4) & 7U;
            if (op > 5U) {
                return false;
            }
            a64_emit_xt_add(b, (op & 1U) == 0U, op < 2U, op == 2U || op == 3U,
                            rd, rn, rm, ((hw2 >> 4) & 3U) * 8U);
            return true;
        }
        if ((hw1 & 0xFFF0U) == 0xFA80U && (hw2 & 0xF0C0U) == 0xF080U) {
            /* QADD / QDADD / QSUB / QDSUB Rd, Rm, Rn */
            unsigned subop = (hw2 >> 4) & 3U;
            a64_emit_qaddsub(b, rd, rm, rn, (subop & 2U) != 0U, (subop & 1U) != 0U);
            return true;
        }
        if ((hw1 & 0xFFF0U) == 0xFAB0U && (hw2 & 0xF0C0U) == 0xF080U) {
            /* CLZ Rd, Rm */
            a64_emit_clz_w(b, rd, rm);
            return true;
        }
        if ((hw1 & 0xFFE0U) == 0xFAC0U && (hw2 & 0xF0C0U) == 0xF080U) {
            /* CRC32{B,H,W} / CRC32C{B,H,W} Rd, Rn, Rm */
            unsigned c_bit = (hw1 >> 4) & 1U;
            unsigned sz    = (hw2 >> 4) & 3U;
            if (rd == 13U || rd == 15U || rn == 13U || rn == 15U ||
                rm == 13U || rm == 15U || sz == 3U) {
                return false;
            }
            a64_emit_crc32_w(b, rd, rn, rm, sz, c_bit);
            return true;
        }
        if ((hw1 & 0xFFF0U) == 0xFA90U && (hw2 & 0xF0C0U) == 0xF080U) {
            /* REV / REV16 / RBIT / REVSH */
            unsigned subop = (hw2 >> 4) & 3U;
            if (subop == 0U) {
                a64_emit_rev_w(b, rd, rm);
            } else if (subop == 1U) {
                a64_emit_rev16_w(b, rd, rm);
            } else if (subop == 2U) {
                a64_emit_rbit_w(b, rd, rm);
            } else {
                a64_emit_rev16_w(b, rd, rm);
                a64_emit_sbfm_w(b, rd, rd, 0U, 15U);
            }
            return true;
        }
        if ((hw1 & 0xFFF0U) == 0xFAA0U && (hw2 & 0xF0F0U) == 0xF080U) {
            /* SEL Rd, Rn, Rm */
            a64_emit_sel(b, rd, rn, rm);
            return true;
        }
        if (((hw1 & 0xFFF0U) == 0xFA80U || (hw1 & 0xFFF0U) == 0xFAC0U ||
             (hw1 & 0xFFF0U) == 0xFA90U || (hw1 & 0xFFF0U) == 0xFAD0U ||
             (hw1 & 0xFFF0U) == 0xFAA0U || (hw1 & 0xFFF0U) == 0xFAE0U) &&
            (hw2 & 0xF080U) == 0xF000U && ((hw2 >> 4) & 7U) != 3U && ((hw2 >> 4) & 7U) != 7U) {
            unsigned op1 = (hw1 >> 4) & 7U;
            unsigned op2 = (hw2 >> 4) & 7U;
            bool is_16 = (op1 != 0U && op1 != 4U);
            bool swap_rm = is_16 && (op1 == 2U || op1 == 6U);
            unsigned sub_mask = 0U;
            if (!is_16) {
                sub_mask = ((hw1 & 0x0040U) != 0U) ? 0xFU : 0U;
            } else if (op1 == 5U) {
                sub_mask = 0x3U;
            } else if (op1 == 2U) {
                sub_mask = 0x1U;
            } else if (op1 == 6U) {
                sub_mask = 0x2U;
            }
            a64_emit_parallel_addsub(b, op2, is_16, swap_rm, sub_mask, rd, rn, rm);
            return true;
        }
        if ((hw1 & 0xFFF0U) == 0xFB00U) {
            /* MUL / MLA / MLS */
            unsigned ra = (hw2 >> 12) & 0xFU;
            unsigned subop = (hw2 >> 4) & 0xFU;
            if (subop == 0U) {
                a64_emit_madd_w(b, rd, rn, rm, (ra == 15U) ? A64_ZR : ra);
            } else if (subop == 1U) {
                a64_emit_msub_w(b, rd, rn, rm, ra);
            } else {
                return false;
            }
            return true;
        }
        if ((hw1 & 0xFFF0U) == 0xFB10U && (hw2 & 0x00C0U) == 0U) {
            /* SMUL<x><y> and SMLA<x><y> */
            a64_emit_smla_xy(b, rd, rn, rm, (hw2 >> 12) & 0xFU,
                             ((hw2 >> 5) & 1U) != 0U, ((hw2 >> 4) & 1U) != 0U);
            return true;
        }
        if (((hw1 & 0xFFF0U) == 0xFB20U || (hw1 & 0xFFF0U) == 0xFB40U) && (hw2 & 0x00E0U) == 0U) {
            /* SMUAD{X} / SMUSD{X} / SMLAD{X} / SMLSD{X} */
            a64_emit_smlad(b, rd, rn, rm, (hw2 >> 12) & 0xFU,
                           (hw1 & 0xFFF0U) == 0xFB40U, ((hw2 >> 4) & 1U) != 0U);
            return true;
        }
        if ((hw1 & 0xFFF0U) == 0xFB30U && (hw2 & 0x00E0U) == 0U) {
            /* SMULW<y> and SMLAW<y> */
            a64_emit_smlaw_y(b, rd, rn, rm, (hw2 >> 12) & 0xFU, ((hw2 >> 4) & 1U) != 0U);
            return true;
        }
        if (((hw1 & 0xFFF0U) == 0xFB50U || (hw1 & 0xFFF0U) == 0xFB60U) && (hw2 & 0x00E0U) == 0U) {
            /* SMMUL{R} / SMMLA{R} / SMMLS{R} */
            a64_emit_smmla(b, rd, rn, rm, (hw2 >> 12) & 0xFU,
                           (hw1 & 0xFFF0U) == 0xFB60U, ((hw2 >> 4) & 1U) != 0U);
            return true;
        }
        if ((hw1 & 0xFFF0U) == 0xFB70U && (hw2 & 0x00F0U) == 0U) {
            /* USAD8 / USADA8 */
            a64_emit_usad8(b, rd, rn, rm, (hw2 >> 12) & 0xFU);
            return true;
        }
        if ((hw1 & 0xFF80U) == 0xFB80U) {
            /* SMULL / SDIV / UMULL / UDIV / SMLAL / SMLAL<x><y> / SMLALD / SMLSLD / UMLAL / UMAAL */
            unsigned op = (hw1 >> 4) & 7U;
            unsigned rd_lo = (hw2 >> 12) & 0xFU;
            unsigned subop = (hw2 >> 4) & 0xFU;
            if (op == 1U && subop == 0xFU) {
                a64_emit_sdiv_w(b, rd, rn, rm);
                return true;
            }
            if (op == 3U && subop == 0xFU) {
                a64_emit_udiv_w(b, rd, rn, rm);
                return true;
            }
            if (op == 6U && subop == 6U) {
                /* UMAAL RdLo, RdHi, Rn, Rm */
                a64_emit_umaal(b, rd_lo, rd, rn, rm);
                return true;
            }
            if (op == 4U && (subop & 0xCU) == 8U) {
                /* SMLAL<x><y> RdLo, RdHi, Rn, Rm */
                a64_emit_smlal_xy(b, rd_lo, rd, rn, rm,
                                  ((subop >> 1) & 1U) != 0U, (subop & 1U) != 0U);
                return true;
            }
            if ((op == 4U || op == 5U) && (subop & 0xEU) == 0xCU) {
                /* SMLALD{X} / SMLSLD{X} RdLo, RdHi, Rn, Rm */
                a64_emit_smlald(b, rd_lo, rd, rn, rm, op == 5U, (subop & 1U) != 0U);
                return true;
            }
            if (subop != 0U || (op != 0U && op != 2U && op != 4U && op != 6U)) {
                return false;
            }
            a64_emit_mull(b, op == 0U || op == 4U, op >= 4U, false, rd_lo, rd, rn, rm);
            return true;
        }
    }

    /* 6. Load/store multiple, dual, exclusive, table branch: (hw1 & 0xFE00) == 0xE800 */
    if ((hw1 & 0xFE00U) == 0xE800U) {
        if ((hw1 & 0xFE40U) == 0xE800U &&
            (((hw1 >> 7) & 3U) == 1U || ((hw1 >> 7) & 3U) == 2U)) {
            /* LDMIA.W / STMIA.W / LDMDB.W / STMDB.W (PUSH.W / POP.W) */
            bool u_bit = ((hw1 >> 7) & 1U) != 0U;
            bool w_bit = ((hw1 >> 5) & 1U) != 0U;
            bool l_bit = ((hw1 >> 4) & 1U) != 0U;
            unsigned rn = hw1 & 0xFU;
            uint32_t mask = hw2 & 0xDFFFU;
            if (mask == 0U || rn == 15U || (!l_bit && (hw2 & 0x8000U) != 0U)) {
                return false;
            }
            a64_emit_ldm_stm(b, rn, mask, l_bit, !u_bit, u_bit, w_bit,
                             pc_val, is_uncond, out_ends_block);
            return true;
        }
        /* Exclusives / LDRD / STRD / TBB / TBH */
        if ((hw1 & 0xFFF0U) == 0xE8D0U && (hw2 & 0xFFE0U) == 0xF000U) {
            /* TBB / TBH [Rn, Rm] */
            bool is_tbh = ((hw2 >> 4) & 1U) != 0U;
            unsigned rn = hw1 & 0xFU;
            unsigned rm = hw2 & 0xFU;
            unsigned base = a32_load_reg(b, rn, pc_val, A64_TMP0);
            a64_emit_add_reg_w(b, false, A64_ADDR, base, rm, A64_SHIFT_LSL, is_tbh ? 1U : 0U);
            if (is_tbh) {
                a64_emit_guest_ldrh_w(b, A64_TMP1, A64_ADDR);
            } else {
                a64_emit_guest_ldrb_w(b, A64_TMP1, A64_ADDR);
            }
            a64_emit_mov_pc32(b, A64_TMP0, pc_val);
            a64_emit_add_reg_w(b, false, A64_TMP0, A64_TMP0, A64_TMP1, A64_SHIFT_LSL, 1U);
            a64_emit_exit_branch_reg(b, A64_TMP0, false);
            if (is_uncond) {
                *out_ends_block = true;
            }
            return true;
        }
        if ((hw1 & 0xFFE0U) == 0xE840U) {
            /* LDREX / STREX Rt, [Rn, #imm8*4] */
            bool is_load = ((hw1 >> 4) & 1U) != 0U;
            unsigned rn = hw1 & 0xFU;
            unsigned rt = (hw2 >> 12) & 0xFU;
            unsigned rd = (hw2 >> 8) & 0xFU;
            uint16_t offset = (uint16_t)((hw2 & 0xFFU) << 2);
            a64_emit_add_imm_w(b, false, A64_ADDR, rn, offset, 0U);
            a64_emit_add_uxtw_x(b, A64_ADDR, A64_XG, A64_ADDR);
            if (is_load) {
                a64_emit_ldxr_w(b, rt, A64_ADDR);
            } else {
                a64_emit_stxr_w(b, rd, rt, A64_ADDR);
            }
            return true;
        }
        if ((hw1 & 0xFFE0U) == 0xE8C0U) {
            /* LDREXB / STREXB / LDREXH / STREXH / LDREXD / STREXD */
            bool is_load = ((hw1 >> 4) & 1U) != 0U;
            unsigned rn  = hw1 & 0xFU;
            unsigned rt  = (hw2 >> 12) & 0xFU;
            unsigned rt2 = (hw2 >> 8) & 0xFU;
            unsigned op3 = (hw2 >> 4) & 0xFU;
            unsigned rd  = hw2 & 0xFU;
            a64_emit_add_uxtw_x(b, A64_ADDR, A64_XG, rn);
            if (op3 == 4U) {
                if (is_load) {
                    a64_emit_ldxr_b(b, rt, A64_ADDR);
                } else {
                    a64_emit_stxr_b(b, rd, rt, A64_ADDR);
                }
                return true;
            }
            if (op3 == 5U) {
                if (is_load) {
                    a64_emit_ldxr_h(b, rt, A64_ADDR);
                } else {
                    a64_emit_stxr_h(b, rd, rt, A64_ADDR);
                }
                return true;
            }
            if (op3 == 7U) {
                if (is_load) {
                    a64_emit_ldxr_x(b, A64_TMP0, A64_ADDR);
                    a64_emit_mov_w(b, rt, A64_TMP0);
                    a64_emit_ubfx_x_to_w(b, rt2, A64_TMP0, 32U, 32U);
                } else {
                    a64_emit_mov_w(b, A64_TMP0, rt);
                    a64_emit_bfi_x(b, A64_TMP0, rt2, 32U, 32U);
                    a64_emit_stxr_x(b, rd, A64_TMP0, A64_ADDR);
                }
                return true;
            }
        }
        /* LDRD / STRD Rt, Rt2, [Rn, #+/-imm8*4] */
        if ((hw1 & 0x0040U) == 0U || (hw1 & 0x0120U) == 0U) {
            return false;
        }
        bool p_bit = ((hw1 >> 8) & 1U) != 0U;
        bool u_bit = ((hw1 >> 7) & 1U) != 0U;
        bool w_bit = ((hw1 >> 5) & 1U) != 0U;
        bool l_bit = ((hw1 >> 4) & 1U) != 0U;
        unsigned rn  = hw1 & 0xFU;
        unsigned rt  = (hw2 >> 12) & 0xFU;
        unsigned rt2 = (hw2 >> 8) & 0xFU;
        uint16_t offset = (uint16_t)((hw2 & 0xFFU) << 2);
        unsigned base = a32_load_reg(b, rn, pc_align4, A64_TMP2);
        if (p_bit) {
            if (u_bit) {
                a64_emit_add_imm_w(b, false, A64_ADDR, base, offset, 0U);
            } else {
                a64_emit_sub_imm_w(b, false, A64_ADDR, base, offset, 0U);
            }
        } else {
            a64_emit_mov_w(b, A64_ADDR, base);
        }
        if (l_bit) {
            a64_emit_guest_ldrd_w(b, rt, rt2, A64_ADDR);
        } else {
            a64_emit_guest_strd_w(b, rt, rt2, A64_ADDR);
        }
        if (rn != 15U && !(l_bit && (rn == rt || rn == rt2))) {
            if (!p_bit) {
                if (u_bit) {
                    a64_emit_add_imm_w(b, false, rn, base, offset, 0U);
                } else {
                    a64_emit_sub_imm_w(b, false, rn, base, offset, 0U);
                }
            } else if (w_bit) {
                a64_emit_mov_w(b, rn, A64_ADDR);
            }
        }
        return true;
    }

    /* 7. Load/store single: (hw1 & 0xFE00) == 0xF800 */
    if ((hw1 & 0xFE00U) == 0xF800U) {
        bool sign_ext = ((hw1 >> 8) & 1U) != 0U;
        bool u_imm12  = ((hw1 >> 7) & 1U) != 0U;
        unsigned size = (hw1 >> 5) & 3U; /* 0=byte, 1=half, 2=word */
        bool is_load  = ((hw1 >> 4) & 1U) != 0U;
        unsigned rn   = hw1 & 0xFU;
        unsigned rt   = (hw2 >> 12) & 0xFU;

        if (size > 2U || (size == 2U && sign_ext)) {
            return false;
        }
        if (!is_load && (sign_ext || rt == 15U)) {
            return false;
        }
        if (rn == 15U && !is_load) {
            return false;
        }

        bool p_bit = true;
        bool u_bit = false;
        bool w_bit = false;
        uint16_t imm8 = 0U;

        if (rn == 15U) {
            /* Literal load [PC, #+/-imm12] */
            uint16_t imm12 = (uint16_t)(hw2 & 0xFFFU);
            uint32_t addr = u_imm12 ? (pc_align4 + imm12) : (pc_align4 - imm12);
            a64_emit_mov_pc32(b, A64_ADDR, addr);
        } else if (u_imm12) {
            /* 12-bit unsigned offset [Rn, #imm12] */
            uint16_t imm12 = (uint16_t)(hw2 & 0xFFFU);
            a64_emit_add_imm_w(b, false, A64_ADDR, rn, imm12, 0U);
        } else if ((hw2 & 0x0800U) == 0U) {
            /* Shifted register offset [Rn, Rm, LSL #imm2] */
            if ((hw2 & 0x0FC0U) != 0U) {
                return false;
            }
            unsigned rm = hw2 & 0xFU;
            unsigned shift = (hw2 >> 4) & 3U;
            a64_emit_add_reg_w(b, false, A64_ADDR, rn, rm, A64_SHIFT_LSL, shift);
        } else {
            /* 8-bit pre/post/negative immediate [Rn, #+/-imm8] */
            p_bit = ((hw2 >> 10) & 1U) != 0U;
            u_bit = ((hw2 >> 9) & 1U) != 0U;
            w_bit = ((hw2 >> 8) & 1U) != 0U;
            if (!p_bit && !w_bit) {
                return false;
            }
            if (rt == 15U && is_load && size != 2U && (hw2 & 0x0F00U) != 0x0C00U) {
                return false;
            }
            imm8 = (uint16_t)(hw2 & 0xFFU);
            if (p_bit) {
                if (u_bit) {
                    a64_emit_add_imm_w(b, false, A64_ADDR, rn, imm8, 0U);
                } else {
                    a64_emit_sub_imm_w(b, false, A64_ADDR, rn, imm8, 0U);
                }
            } else {
                a64_emit_mov_w(b, A64_ADDR, rn);
            }
        }

        if (rt == 15U && is_load && size != 2U) {
            /* PLD / PLI hint */
            return true;
        }

        unsigned dst = (is_load && rt == 15U) ? A64_TMP0 : rt;
        if (is_load) {
            if (size == 0U) {
                if (sign_ext) {
                    a64_emit_guest_ldrsb_w(b, dst, A64_ADDR);
                } else {
                    a64_emit_guest_ldrb_w(b, dst, A64_ADDR);
                }
            } else if (size == 1U) {
                if (sign_ext) {
                    a64_emit_guest_ldrsh_w(b, dst, A64_ADDR);
                } else {
                    a64_emit_guest_ldrh_w(b, dst, A64_ADDR);
                }
            } else {
                a64_emit_guest_ldr_w(b, dst, A64_ADDR);
            }
        } else {
            if (size == 0U) {
                a64_emit_guest_strb_w(b, rt, A64_ADDR);
            } else if (size == 1U) {
                a64_emit_guest_strh_w(b, rt, A64_ADDR);
            } else {
                a64_emit_guest_str_w(b, rt, A64_ADDR);
            }
        }

        if (!p_bit && (rn != rt || !is_load)) {
            if (u_bit) {
                a64_emit_add_imm_w(b, false, rn, rn, imm8, 0U);
            } else {
                a64_emit_sub_imm_w(b, false, rn, rn, imm8, 0U);
            }
        } else if (p_bit && w_bit && (rn != rt || !is_load)) {
            a64_emit_mov_w(b, rn, A64_ADDR);
        }

        if (is_load && rt == 15U) {
            a64_emit_exit_branch_reg(b, A64_TMP0, true);
            if (is_uncond) {
                *out_ends_block = true;
            }
        }
        return true;
    }

    /* 8. Coprocessor p15 */
    if ((hw1 & 0xFFEFU) == 0xEE0DU || (hw1 & 0xFFEFU) == 0xEE07U) {
        return a64_emit_cp15(b, ((uint32_t)hw1 << 16) | hw2);
    }

    return false;
}

bool a32_translate_t32_block(a32_ctx_t *ctx, a64_buf_t *b, uint32_t start_pc)
{
    uint32_t pc = start_pc & ~1U;
    uint8_t  itstate = (uint8_t)(ctx->itstate & 0xFFU);
    if (itstate != 0U) {
        a64_emit_str_imm_w(b, A64_ZR, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, itstate));
    }

    t32_insn_info_t block_insns[T32_MAX_PRESCAN_INSNS];
    bool            flags_live[T32_MAX_PRESCAN_INSNS];
    unsigned        num_insns = 0U;
    unsigned        max_insns = ctx->single_step ? 1U : T32_MAX_BLOCK_INSNS;

    uint32_t cur_page = pc & ~0xFFFU;
    uint32_t scan_pc = pc;
    uint8_t  scan_itstate = itstate;

    /* Pre-pass: scan guest instructions in the basic block */
    for (unsigned count = 0U; (count < max_insns || (!ctx->single_step && scan_itstate != 0U)) && num_insns < T32_MAX_PRESCAN_INSNS; count++) {
        if (count > 0U && scan_itstate == 0U && (scan_pc - pc) >= 496U) {
            break;
        }
        if ((scan_pc & ~0xFFFU) != cur_page) {
            if (!a32_jit_range_executable(ctx, scan_pc, 2U)) {
                break;
            }
            cur_page = scan_pc & ~0xFFFU;
        }
        if (ctx->gdb_enabled && count > 0U && a32_gdb_has_breakpoint(scan_pc)) {
            break;
        }
        const uint16_t *guest_hw = (const uint16_t *)a32_guest_ptr(ctx, scan_pc);
        uint16_t hw1 = guest_hw[0];

        t32_insn_info_t *info = &block_insns[num_insns];
        info->pc = scan_pc;
        info->hw1 = hw1;
        info->hw2 = 0U;
        info->itstate = scan_itstate;

        /* Check for 16-bit IT instruction (0xBF00 where mask != 0) */
        if ((hw1 & 0xFF00U) == 0xBF00U && (hw1 & 0x000FU) != 0U && (scan_itstate & 0x0FU) == 0U) {
            unsigned firstcond = (hw1 >> 4) & 0xFU;
            unsigned it_mask = hw1 & 0xFU;
            if (firstcond != 0xFU &&
                !(firstcond == 0xEU && (it_mask & (it_mask - 1U)) != 0U)) {
                info->is_it = true;
                info->is_32bit = false;
                scan_itstate = (uint8_t)(hw1 & 0xFFU);
                scan_pc += 2U;
                num_insns++;
                continue;
            }
        }

        info->is_it = false;
        bool is_32bit = ((hw1 >> 11) == 0x1DU || (hw1 >> 11) == 0x1EU || (hw1 >> 11) == 0x1FU);
        info->is_32bit = is_32bit;

        bool in_it = (scan_itstate & 0x0FU) != 0U;
        bool scan_uncond = !in_it || (((scan_itstate >> 4) & 0xFU) == A64_COND_AL);
        bool ends_uncond = false;

        if (is_32bit) {
            if (((scan_pc + 2U) & ~0xFFFU) != cur_page) {
                if (!a32_jit_range_executable(ctx, scan_pc + 2U, 2U)) {
                    if (count > 0U) {
                        break;
                    }
                    if (itstate != 0U) {
                        a64_emit_mov_imm32(b, A64_TMP0, itstate);
                        a64_emit_str_imm_w(b, A64_TMP0, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, itstate));
                    }
                    a64_emit_exit_undef(b, pc);
                    b->guest_end_pc = pc + 2U;
                    return true;
                }
                cur_page = (scan_pc + 2U) & ~0xFFFU;
            }
            uint16_t hw2 = guest_hw[1];
            info->hw2 = hw2;
            scan_pc += 4U;
            if (scan_uncond && t32_insn_uncond_ends_block(hw1, hw2)) {
                ends_uncond = true;
            } else if (!in_it && (hw1 & 0xF800U) == 0xF000U && (hw2 & 0xD000U) == 0x8000U &&
                       ((hw1 >> 6) & 0xFU) < 14U) {
                uint32_t s  = (hw1 >> 10) & 1U;
                uint32_t j1 = (hw2 >> 13) & 1U;
                uint32_t j2 = (hw2 >> 11) & 1U;
                uint32_t imm6  = hw1 & 0x3FU;
                uint32_t imm11 = hw2 & 0x7FFU;
                int32_t offset = (int32_t)((s << 20) | (j2 << 19) | (j1 << 18) |
                                           (imm6 << 12) | (imm11 << 1));
                if (s) {
                    offset |= (int32_t)0xFFE00000U;
                }
                if ((info->pc + 4U + (uint32_t)offset) == pc) {
                    ends_uncond = true;
                }
            }
        } else {
            scan_pc += 2U;
            if ((scan_uncond && t16_insn_uncond_ends_block(hw1)) || (hw1 & 0xFF00U) == 0xBE00U) {
                ends_uncond = true;
            } else if (!in_it && (hw1 >> 12) == 0xDU && ((hw1 >> 8) & 0xFU) < 14U) {
                int32_t offset = ((int32_t)(int8_t)(hw1 & 0xFFU)) << 1;
                if ((info->pc + 4U + (uint32_t)offset) == pc) {
                    ends_uncond = true;
                }
            }
        }

        if (in_it) {
            /* Advance ITSTATE: shift bottom 5 bits left by 1; if bottom 4 bits become 0, clear */
            uint8_t mask = (uint8_t)((scan_itstate << 1) & 0x1FU);
            if ((mask & 0x0FU) == 0U) {
                scan_itstate = 0U;
            } else {
                scan_itstate = (uint8_t)((scan_itstate & 0xE0U) | mask);
            }
        }

        num_insns++;
        if (ends_uncond) {
            break;
        }
    }

    /* Backward pass: determine NZCV flag liveness */
    bool live = true;
    for (int i = (int)num_insns - 1; i >= 0; i--) {
        const t32_insn_info_t *info = &block_insns[i];
        flags_live[i] = live;
        if (t32_insn_overwrites_all_flags(info)) {
            live = false;
        } else if (info->is_it || info->itstate != 0U || t32_insn_reads_flags(info) ||
                   (live && t32_insn_partially_updates_flags(info))) {
            live = true;
        }
    }

    /* Translation & emission loop */
    for (unsigned idx = 0U; idx < num_insns; idx++) {
        const t32_insn_info_t *info = &block_insns[idx];
        uint32_t cur_pc = info->pc;
        uint16_t hw1 = info->hw1;

        if (idx > 0U && itstate == 0U && b->cap > b->count && b->cap - b->count < 128U) {
            b->guest_end_pc = cur_pc;
            a64_emit_exit_branch_imm(b, cur_pc, false);
            return true;
        }

        if (info->is_it) {
            itstate = (uint8_t)(hw1 & 0xFFU);
            continue;
        }

        /* Record host_word_idx -> (guest_pc, itstate) in JIT side-table */
        a32_jit_record_pc_map((uint32_t)b->count, cur_pc, itstate);

        bool in_it = (itstate & 0x0FU) != 0U;
        if (!info->is_32bit && (hw1 & 0xFF00U) == 0xBE00U) {
            if (in_it) {
                a64_emit_mov_imm32(b, A64_TMP0, itstate);
                a64_emit_str_imm_w(b, A64_TMP0, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, itstate));
            }
            a64_emit_exit_bkpt(b, cur_pc);
            b->guest_end_pc = cur_pc + 2U;
            return true;
        }

        uint8_t next_itstate = 0U;
        if (in_it) {
            uint8_t mask = (uint8_t)((itstate << 1) & 0x1FU);
            if ((mask & 0x0FU) != 0U) {
                next_itstate = (uint8_t)((itstate & 0xE0U) | mask);
            }
        }

        unsigned cond = A64_COND_AL;
        size_t cond_skip = 0U;
        if (in_it) {
            cond = (itstate >> 4) & 0xFU;
            if (cond != A64_COND_AL) {
                cond_skip = a64_emit_b_cond_placeholder(b, cond ^ 1U);
            }
        }

        bool is_uncond = !in_it || (cond == A64_COND_AL);
        bool ends_block = false;
        bool ok = false;

        if (info->is_32bit) {
            uint16_t hw2 = info->hw2;
            ok = translate_t32_insn(ctx, b, cur_pc, hw1, hw2, is_uncond, flags_live[idx], &ends_block);
            if (!ok) {
                if (ctx->trace_enabled) {
                    fprintf(stderr, "a32rt: unhandled T32 instruction 0x%04x 0x%04x at pc=0x%08x\n",
                            hw1, hw2, cur_pc);
                }
                if (in_it) {
                    a64_emit_mov_imm32(b, A64_TMP0, itstate);
                    a64_emit_str_imm_w(b, A64_TMP0, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, itstate));
                }
                a64_emit_exit_undef(b, cur_pc);
            }
        } else {
            ok = translate_t16_insn(ctx, b, cur_pc, hw1, in_it, itstate, next_itstate, is_uncond, flags_live[idx], &ends_block);
            if (!ok) {
                if (ctx->trace_enabled) {
                    fprintf(stderr, "a32rt: unhandled T16 instruction 0x%04x at pc=0x%08x\n",
                            hw1, cur_pc);
                }
                if (in_it) {
                    a64_emit_mov_imm32(b, A64_TMP0, itstate);
                    a64_emit_str_imm_w(b, A64_TMP0, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, itstate));
                }
                a64_emit_exit_undef(b, cur_pc);
            }
        }

        if (in_it) {
            if (cond != A64_COND_AL) {
                a64_patch_b_cond(b, cond_skip, b->count);
            }
            itstate = next_itstate;
        }

        if ((ends_block && (itstate == 0U || is_uncond)) || (!ok && is_uncond)) {
            b->guest_end_pc = cur_pc + (info->is_32bit ? 4U : 2U);
            return true;
        }
    }

    if (itstate != 0U) {
        a64_emit_mov_imm32(b, A64_TMP0, itstate);
        a64_emit_str_imm_w(b, A64_TMP0, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, itstate));
    }
    b->guest_end_pc = scan_pc;
    a64_emit_exit_branch_imm(b, scan_pc, false);
    return true;
}
