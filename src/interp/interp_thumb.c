/*
 * src/interp/interp_thumb.c — Thumb-1 / Thumb-2 instruction step.
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "interp.h"
#include "interp_priv.h"

#include <string.h>
#include <sys/mman.h>

static uint8_t it_next(uint8_t it)
{
    uint8_t mask = (uint8_t)((it << 1) & 0x1FU);
    if ((mask & 0x0FU) == 0U) {
        return 0U;
    }
    return (uint8_t)((it & 0xE0U) | mask);
}

/* 16-bit Thumb single step. Returns exit code; sets regs[15]. */

static uint32_t t16_step(a32_ctx_t *ctx, uint32_t pc, bool *in_it, unsigned *it_cond)
{
    uint16_t hw = 0U;
    if (!i_fetch16(ctx, pc, &hw)) {
        return A32_EXIT_FAULT;
    }
    bool use_it = *in_it;
    unsigned cond = *it_cond;
    uint32_t next = pc + 2U;

    /* IT instruction itself. */
    if ((hw & 0xFF00U) == 0xBF00U && (hw & 0x0FU) != 0U) {
        if (use_it) {
            ctx->regs[15] = next;
            i_undef(ctx);
            return A32_EXIT_UNDEF;
        }
        ctx->itstate = (uint32_t)(hw & 0xFFU);
        a32_ctx_normalize_itstate(ctx, true);
        ctx->regs[15] = next;
        return A32_EXIT_NONE;
    }

    /* BKPT is unconditional inside an IT block: it always traps, even when
     * the current IT condition fails, and preserves pre-instruction IT.
     * Report the instruction PC explicitly like the ARM BKPT handler
     * instead of relying on the regs[15] entry invariant. */
    if ((hw & 0xFF00U) == 0xBE00U) {
        ctx->regs[15] = pc;
        ctx->exit_reason = A32_EXIT_BKPT;
        return A32_EXIT_BKPT;
    }

    bool skip = false;
    if (use_it) {
        skip = !a32_interp_cond_passes(ctx->cpsr, cond);
    }

    if (!skip) {
        if ((hw >> 11) == 0x1CU) {
            int32_t off = (int32_t)((uint32_t)(hw & 0x7FFU) << 21U) >> 21;
            i_set_pc_branch(ctx, (pc + 4U) + ((uint32_t)off << 1U), false);
            goto adv_it;
        }
        if ((hw & 0xFF00U) == 0xDF00U) {
            ctx->svc_imm = hw & 0xFFU;
            ctx->regs[15] = next;
            ctx->exit_reason = A32_EXIT_SVC;
            goto adv_it_ret;
        }
        if (hw == 0xBF00U) {
            /* Thumb NOP: successful completion, IT advances normally. */
            ctx->regs[15] = next;
            goto adv_it;
        }
        if ((hw & 0xFF00U) == 0xDE00U || hw == 0xBFA0U) {
            i_undef(ctx);
            goto trap_ret;
        }
        if ((hw >> 11) < 4U) {
            /* 000xx: LSL/LSR/ASR imm5, then ADD/SUB (register/imm3). */
            unsigned top = (hw >> 11) & 3U;
            unsigned rd = hw & 7U;
            if (top == 3U) {
                /* Bit 10 selects immediate (1) vs register (0); bit 9 selects
                 * SUB (1) vs ADD (0). */
                unsigned sel = (hw >> 9) & 3U;
                unsigned rn = (hw >> 3) & 7U;
                bool is_imm = ((sel >> 1) & 1U) != 0U;
                bool is_sub = (sel & 1U) != 0U;
                uint32_t op2 = is_imm ? (uint32_t)((hw >> 6) & 7U) :
                    ctx->regs[(hw >> 6) & 7U];
                bool c = false;
                bool o = false;
                uint32_t r;
                if (is_sub) {
                    r = i_add_carry(ctx->regs[rn], ~op2, 1U, &c, &o);
                } else {
                    r = i_add_carry(ctx->regs[rn], op2, 0U, &c, &o);
                }
                ctx->regs[rd] = r;
                if (!use_it) {
                    i_set_nzcv(&ctx->cpsr, r, c, o);
                }
            } else {
                unsigned op = top;
                unsigned rm = (hw >> 3) & 7U;
                unsigned amt = (hw >> 6) & 31U;
                uint32_t v = ctx->regs[rm];
                bool carry = (ctx->cpsr & A32_CPSR_C) != 0U;
                bool ov = (ctx->cpsr & A32_CPSR_V) != 0U;
                uint32_t r;
                if (op == 0U) {
                    if (amt == 0U) {
                        r = v;
                    } else {
                        carry = ((v >> (32U - amt)) & 1U) != 0U;
                        r = v << amt;
                    }
                } else if (op == 1U) {
                    if (amt == 0U) {
                        carry = ((v >> 31U) & 1U) != 0U;
                        r = 0U;
                    } else {
                        carry = ((v >> (amt - 1U)) & 1U) != 0U;
                        r = v >> amt;
                    }
                } else {
                    if (amt == 0U) {
                        carry = ((v >> 31U) & 1U) != 0U;
                        r = ((v & 0x80000000U) != 0U) ? 0xFFFFFFFFU : 0U;
                    } else {
                        carry = ((v >> (amt - 1U)) & 1U) != 0U;
                        r = (uint32_t)((int32_t)v >> amt);
                    }
                }
                ctx->regs[rd] = r;
                if (!use_it) {
                    i_set_nzcv(&ctx->cpsr, r, carry, ov);
                }
            }
        } else if ((hw >> 13) == 0x1U) {
            /* 001xx: MOVS/CMP/ADDS/SUBS immediate. */
            unsigned op = (hw >> 11) & 3U;
            unsigned rd = (hw >> 8) & 7U;
            uint32_t imm = hw & 0xFFU;
            if (op == 0U) {
                ctx->regs[rd] = imm;
                if (!use_it) {
                    i_set_nz(&ctx->cpsr, imm);
                }
            } else if (op == 1U) {
                uint32_t a = ctx->regs[rd];
                bool c = false;
                bool o = false;
                uint32_t r = i_add_carry(a, ~imm, 1U, &c, &o);
                (void)r;
                i_set_nzcv(&ctx->cpsr, r, c, o);
            } else if (op == 2U) {
                bool c = false;
                bool o = false;
                uint32_t r = i_add_carry(ctx->regs[rd], imm, 0U, &c, &o);
                ctx->regs[rd] = r;
                if (!use_it) {
                    i_set_nzcv(&ctx->cpsr, r, c, o);
                }
            } else {
                bool c = false;
                bool o = false;
                uint32_t r = i_add_carry(ctx->regs[rd], ~imm, 1U, &c, &o);
                ctx->regs[rd] = r;
                if (!use_it) {
                    i_set_nzcv(&ctx->cpsr, r, c, o);
                }
            }
        } else if ((hw & 0xF800U) == 0x4000U) {
            /* 01000x: ALU ops / hi-reg ops / BX / BLX. The 01001x literal
             * loads (0x4800-0x4FFF) decode separately below. */
            if (((hw >> 10) & 3U) == 0U) {
                /* ALU ops on low regs. */
                unsigned op = (hw >> 6) & 0xFU;
                unsigned rm = (hw >> 3) & 7U;
                unsigned rd = hw & 7U;
                uint32_t a = ctx->regs[rd];
                uint32_t b = ctx->regs[rm];
                uint32_t r = a;
                bool c = (ctx->cpsr & A32_CPSR_C) != 0U;
                bool o = false;
                bool w = true;
                switch (op) {
                case 0x0U: r = a & b; break;
                case 0x1U: r = a ^ b; break;
                case 0x2U: {
                    unsigned sh = b & 0xFFU;
                    if (sh != 0U) {
                        c = (sh <= 32U) ? (((a >> (32U - sh)) & 1U) != 0U) : false;
                        r = (sh < 32U) ? (a << sh) : 0U;
                    }
                    break;
                }
                case 0x3U: {
                    unsigned sh = b & 0xFFU;
                    if (sh != 0U) {
                        c = (sh <= 32U) ? (((a >> (sh - 1U)) & 1U) != 0U) : false;
                        r = (sh < 32U) ? (a >> sh) : 0U;
                    }
                    break;
                }
                case 0x4U: {
                    unsigned sh = b & 0xFFU;
                    if (sh != 0U) {
                        c = (sh < 32U) ? (((a >> (sh - 1U)) & 1U) != 0U) : ((a & 0x80000000U) != 0U);
                        r = (sh < 32U) ? (uint32_t)((int32_t)a >> sh) : ((a & 0x80000000U) ? 0xFFFFFFFFU : 0U);
                    }
                    break;
                }
                case 0x5U: r = i_add_carry(a, b, c ? 1U : 0U, &c, &o); break;
                case 0x6U: r = i_add_carry(a, ~b, c ? 1U : 0U, &c, &o); break;
                case 0x7U: {
                    unsigned sh = b & 0xFFU;
                    if (sh != 0U) {
                        unsigned rot = sh & 31U;
                        r = (rot != 0U) ? ((a >> rot) | (a << (32U - rot))) : a;
                        c = (r & 0x80000000U) != 0U;
                    }
                    break;
                }
                case 0x8U: r = a & b; w = false; break;
                case 0x9U: r = i_add_carry(0U, ~b, 1U, &c, &o); break;
                case 0xAU: r = i_add_carry(a, ~b, 1U, &c, &o); w = false; break;
                case 0xBU: r = i_add_carry(a, b, 0U, &c, &o); w = false; break;
                case 0xCU: r = a | b; break;
                case 0xDU: r = a * b; break;
                case 0xEU: r = a & ~b; break;
                default:   r = ~b; break;
                }
                if (w) {
                    ctx->regs[rd] = r;
                }
                /* T16 ALU (T1) encodings suppress implicit flag writes inside
                 * an IT block; explicit TST/CMP/CMN keep updating flags. */
                if (op == 0x8U || op == 0xAU || op == 0xBU) {
                    i_set_nzcv(&ctx->cpsr, r, c, o);
                } else if (!use_it) {
                    if (op == 0x2U || op == 0x3U || op == 0x4U || op == 0x7U) {
                        if ((b & 0xFFU) == 0U) {
                            i_set_nz(&ctx->cpsr, r);
                        } else {
                            i_set_nzcv(&ctx->cpsr, r, c, (ctx->cpsr & A32_CPSR_V) != 0U);
                        }
                    } else if (op == 0x5U || op == 0x6U || op == 0x9U) {
                        i_set_nzcv(&ctx->cpsr, r, c, o);
                    } else {
                        i_set_nz(&ctx->cpsr, r);
                    }
                }
            } else {
                /* Hi-reg ops / BX / BLX. */
                unsigned op = (hw >> 8) & 3U;
                unsigned hd = (hw >> 7) & 1U;
                unsigned hs = (hw >> 6) & 1U;
                unsigned rd = (hw & 7U) | (hd << 3);
                unsigned rs = ((hw >> 3) & 7U) | (hs << 3);
                uint32_t rv = (rs == 15U) ? (pc + 4U) : ctx->regs[rs];
                if (op == 3U) {
                    if (((hw >> 7) & 1U) != 0U) {
                        ctx->regs[14] = next | 1U;
                    }
                    i_set_pc_branch(ctx, rv, true);
                    goto adv_it;
                }
                uint32_t dv = (rd == 15U) ? (pc + 4U) : ctx->regs[rd];
                uint32_t r = dv;
                bool wback = true;
                if (op == 0U) {
                    r = dv + rv;
                } else if (op == 1U) {
                    bool cc = false;
                    bool oo = false;
                    r = i_add_carry(dv, ~rv, 1U, &cc, &oo);
                    i_set_nzcv(&ctx->cpsr, r, cc, oo);
                    wback = false;
                } else {
                    r = rv;
                }
                if (rd == 15U) {
                    if (op == 0U || op == 2U) {
                        /* Thumb BranchWritePC: an even target stays in
                         * Thumb state; never clear CPSR.T here. */
                        i_set_pc_branch(ctx, r, false);
                        goto adv_it;
                    }
                    i_undef(ctx);
                    goto trap_ret;
                }
                if (wback) {
                    ctx->regs[rd] = r;
                }
            }
        } else if ((hw >> 11) == 0x09U) {
            /* LDR literal. */
            unsigned rt = (hw >> 8) & 7U;
            uint32_t base = (pc + 4U) & ~3U;
            uint32_t v = 0U;
            if (!i_load_u32(ctx, base + ((hw & 0xFFU) << 2), &v)) {
                return A32_EXIT_FAULT;
            }
            ctx->regs[rt] = v;
        } else if ((hw >> 12) == 0x5U) {
            /* Register offset (0101): STR/STRH/STRB/LDSB/LDR/LDRH/LDRB/LDSH. */
            {
                unsigned op = (hw >> 9) & 7U;
                unsigned ro = (hw >> 6) & 7U;
                unsigned rb = (hw >> 3) & 7U;
                unsigned rt = hw & 7U;
                uint32_t ea = ctx->regs[rb] + ctx->regs[ro];
                if (op == 0U) {
                    if (!i_store_u32(ctx, ea, ctx->regs[rt])) {
                        return A32_EXIT_FAULT;
                    }
                } else if (op == 1U) {
                    uint16_t hv = (uint16_t)ctx->regs[rt];
                    if (!i_store_bytes(ctx, ea, &hv, 2U)) {
                        return A32_EXIT_FAULT;
                    }
                } else if (op == 2U) {
                    uint8_t bv = (uint8_t)ctx->regs[rt];
                    if (!i_store_bytes(ctx, ea, &bv, 1U)) {
                        return A32_EXIT_FAULT;
                    }
                } else if (op == 3U) {
                    int8_t bv = 0;
                    if (!i_load_bytes(ctx, ea, &bv, 1U)) {
                        return A32_EXIT_FAULT;
                    }
                    ctx->regs[rt] = (uint32_t)(int32_t)bv;
                } else if (op == 4U) {
                    uint32_t v = 0U;
                    if (!i_load_u32(ctx, ea, &v)) {
                        return A32_EXIT_FAULT;
                    }
                    ctx->regs[rt] = v;
                } else if (op == 5U) {
                    uint16_t hv = 0U;
                    if (!i_load_bytes(ctx, ea, &hv, 2U)) {
                        return A32_EXIT_FAULT;
                    }
                    ctx->regs[rt] = hv;
                } else if (op == 6U) {
                    uint8_t bv = 0U;
                    if (!i_load_bytes(ctx, ea, &bv, 1U)) {
                        return A32_EXIT_FAULT;
                    }
                    ctx->regs[rt] = bv;
                } else {
                    int16_t hv = 0;
                    if (!i_load_bytes(ctx, ea, &hv, 2U)) {
                        return A32_EXIT_FAULT;
                    }
                    ctx->regs[rt] = (uint32_t)(int32_t)hv;
                }
            }
        } else if ((hw >> 13) == 0x3U) {
            /* Word/byte immediate offset (011x): STR/LDR/STRB/LDRB. */
            {
                bool l = ((hw >> 11) & 1U) != 0U;
                bool byte = ((hw >> 12) & 1U) != 0U;
                unsigned rt = hw & 7U;
                unsigned rb = (hw >> 3) & 7U;
                uint32_t ea = ctx->regs[rb] + (((hw >> 6) & 31U) << (byte ? 0U : 2U));
                if (l) {
                    if (byte) {
                        uint8_t bv = 0U;
                        if (!i_load_bytes(ctx, ea, &bv, 1U)) {
                            return A32_EXIT_FAULT;
                        }
                        ctx->regs[rt] = bv;
                    } else {
                        uint32_t v = 0U;
                        if (!i_load_u32(ctx, ea, &v)) {
                            return A32_EXIT_FAULT;
                        }
                        ctx->regs[rt] = v;
                    }
                } else {
                    if (byte) {
                        uint8_t bv = (uint8_t)ctx->regs[rt];
                        if (!i_store_bytes(ctx, ea, &bv, 1U)) {
                            return A32_EXIT_FAULT;
                        }
                    } else {
                        if (!i_store_u32(ctx, ea, ctx->regs[rt])) {
                            return A32_EXIT_FAULT;
                        }
                    }
                }
            }
        } else if ((hw >> 12) == 0x8U) {
            /* Halfword immediate offset (1000): STRH/LDRH. */
            {
                bool l = ((hw >> 11) & 1U) != 0U;
                unsigned rt = hw & 7U;
                unsigned rb = (hw >> 3) & 7U;
                uint32_t ea = ctx->regs[rb] + (((hw >> 6) & 31U) << 1);
                if (l) {
                    uint16_t hv = 0U;
                    if (!i_load_bytes(ctx, ea, &hv, 2U)) {
                        return A32_EXIT_FAULT;
                    }
                    ctx->regs[rt] = hv;
                } else {
                    uint16_t hv = (uint16_t)ctx->regs[rt];
                    if (!i_store_bytes(ctx, ea, &hv, 2U)) {
                        return A32_EXIT_FAULT;
                    }
                }
            }
        } else if ((hw >> 12) == 0x9U) {
            /* SP-relative word transfer (1001): STR/LDR. */
            {
                bool l = ((hw >> 11) & 1U) != 0U;
                unsigned rt = (hw >> 8) & 7U;
                uint32_t ea = ctx->regs[13] + ((hw & 0xFFU) << 2);
                if (l) {
                    uint32_t v = 0U;
                    if (!i_load_u32(ctx, ea, &v)) {
                        return A32_EXIT_FAULT;
                    }
                    ctx->regs[rt] = v;
                } else {
                    if (!i_store_u32(ctx, ea, ctx->regs[rt])) {
                        return A32_EXIT_FAULT;
                    }
                }
            }
        } else if ((hw & 0xF800U) == 0xA000U) {
            /* ADR: ADD Rd, PC/SP, #imm8 << 2. No flags. */
            unsigned rt = (hw >> 8) & 7U;
            uint32_t base = ((hw >> 11) & 1U) != 0U ? ctx->regs[13] :
                ((pc + 4U) & ~3U);
            ctx->regs[rt] = base + ((hw & 0xFFU) << 2);
        } else if ((hw & 0xF000U) == 0xC000U) {
            /* STMIA / LDMIA with writeback. */
            bool l = ((hw >> 11) & 1U) != 0U;
            unsigned rn = (hw >> 8) & 7U;
            uint8_t list = (uint8_t)hw;
            if (list == 0U) {
                ctx->regs[15] = next;
                i_undef(ctx);
                goto trap_ret;
            }
            unsigned n = 0U;
            for (unsigned k = 0U; k < 8U; k++) {
                if ((list >> k) & 1U) {
                    n++;
                }
            }
            uint32_t addr = ctx->regs[rn];
            uint32_t cur = addr;
            /* Retain Rn's value from its own list position (A32-R7-21);
             * writeback is suppressed when Rn is in the list. */
            uint32_t rn_val = 0U;
            for (unsigned r = 0U; r < 8U; r++) {
                if (((list >> r) & 1U) == 0U) {
                    continue;
                }
                if (l) {
                    uint32_t v = 0U;
                    if (!i_load_u32(ctx, cur, &v)) {
                        return A32_EXIT_FAULT;
                    }
                    if (r != rn) {
                        ctx->regs[r] = v;
                    } else {
                        rn_val = v;
                    }
                } else {
                    if (!i_store_u32(ctx, cur, ctx->regs[r])) {
                        return A32_EXIT_FAULT;
                    }
                }
                cur += 4U;
            }
            if (l && ((list >> rn) & 1U) != 0U) {
                ctx->regs[rn] = rn_val;
            } else {
                ctx->regs[rn] = addr + 4U * n;
            }
        } else if ((hw & 0xFF00U) == 0xB200U || (hw & 0xFF00U) == 0xBA00U) {
            /* SXTH/SXTB/UXTH/UXTB and REV/REV16/REVSH. */
            bool is_rev = (hw & 0xFF00U) == 0xBA00U;
            unsigned op = (hw >> 6) & 3U;
            unsigned rm = (hw >> 3) & 7U;
            unsigned rd = hw & 7U;
            uint32_t v = ctx->regs[rm];
            uint32_t r = v;
            if (is_rev) {
                if (op == 0U) {
                    r = ((v & 0xFFU) << 24) | ((v & 0xFF00U) << 8) |
                        ((v >> 8) & 0xFF00U) | ((v >> 24) & 0xFFU);
                } else if (op == 1U) {
                    r = ((v & 0xFFU) << 8) | ((v >> 8) & 0xFFU) |
                        ((v & 0xFF0000U) << 8) | ((v >> 8) & 0xFF0000U);
                } else if (op == 3U) {
                    uint32_t half = v & 0xFFFFU;
                    uint32_t swapped = ((half & 0xFFU) << 8) | ((half >> 8) & 0xFFU);
                    r = (uint32_t)(int32_t)(int16_t)(uint16_t)swapped;
                } else {
                    ctx->regs[15] = next;
                    i_undef(ctx);
                    goto trap_ret;
                }
            } else {
                if (op == 0U) {
                    r = (uint32_t)(int32_t)(int16_t)(uint16_t)(v & 0xFFFFU);
                } else if (op == 1U) {
                    r = (uint32_t)(int32_t)(int8_t)(uint8_t)(v & 0xFFU);
                } else if (op == 2U) {
                    r = (uint32_t)(uint16_t)(v & 0xFFFFU);
                } else {
                    r = (uint32_t)(uint8_t)(v & 0xFFU);
                }
            }
            ctx->regs[rd] = r;
        } else if ((hw & 0xFE00U) == 0xB400U || (hw & 0xFE00U) == 0xBC00U) {
            bool l = ((hw >> 11) & 1U) != 0U;
            uint8_t list = (uint8_t)hw;
            uint32_t sp = ctx->regs[13];
            if (!l) {
                unsigned n = 0U;
                for (unsigned k = 0U; k < 8U; k++) {
                    if ((list >> k) & 1U) {
                        n++;
                    }
                }
                bool rbit = ((hw >> 8) & 1U) != 0U;
                if (rbit) {
                    n++;
                }
                uint32_t addr = sp - 4U * n;
                uint32_t cur = addr;
                for (unsigned r = 0U; r < 8U; r++) {
                    if (((list >> r) & 1U) == 0U) {
                        continue;
                    }
                    if (!i_store_u32(ctx, cur, ctx->regs[r])) {
                        return A32_EXIT_FAULT;
                    }
                    cur += 4U;
                }
                if (rbit) {
                    if (!i_store_u32(ctx, cur, ctx->regs[14])) {
                        return A32_EXIT_FAULT;
                    }
                }
                ctx->regs[13] = addr;
            } else {
                uint32_t cur = sp;
                for (unsigned r = 0U; r < 8U; r++) {
                    if (((list >> r) & 1U) == 0U) {
                        continue;
                    }
                    uint32_t v = 0U;
                    if (!i_load_u32(ctx, cur, &v)) {
                        return A32_EXIT_FAULT;
                    }
                    ctx->regs[r] = v;
                    cur += 4U;
                }
                if (((hw >> 8) & 1U) != 0U) {
                    uint32_t v = 0U;
                    if (!i_load_u32(ctx, cur, &v)) {
                        return A32_EXIT_FAULT;
                    }
                    i_set_pc_branch(ctx, v, true);
                    ctx->regs[13] = cur + 4U;
                    goto adv_it;
                }
                ctx->regs[13] = cur;
            }
        } else if ((hw & 0xF600U) == 0xB000U) {
            /* ADD/SUB SP immediate. */
            int32_t adj = (int32_t)((hw & 0x7FU) << 2);
            if (((hw >> 7) & 1U) != 0U) {
                ctx->regs[13] -= (uint32_t)adj;
            } else {
                ctx->regs[13] += (uint32_t)adj;
            }
        } else if ((hw >> 12) == 0xDU) {
            unsigned cond8 = (hw >> 8) & 0xFU;
            int32_t off = (int32_t)((uint32_t)(hw & 0xFFU) << 24U) >> 24;
            if (a32_interp_cond_passes(ctx->cpsr, cond8)) {
                i_set_pc_branch(ctx, (pc + 4U) + ((uint32_t)off << 1U), false);
                goto adv_it;
            }
        } else {
            ctx->regs[15] = next;
            i_undef(ctx);
            goto trap_ret;
        }
    }
    ctx->regs[15] = next;
adv_it:
    if (*in_it) {
        uint8_t cur = (uint8_t)(ctx->itstate & 0xFFU);
        ctx->itstate = it_next(cur);
        a32_ctx_normalize_itstate(ctx, true);
    }
    return A32_EXIT_NONE;
adv_it_ret:
    if (*in_it) {
        uint8_t cur = (uint8_t)(ctx->itstate & 0xFFU);
        ctx->itstate = it_next(cur);
        a32_ctx_normalize_itstate(ctx, true);
    }
    return ctx->exit_reason;
trap_ret:
    /* Retryable trap (UNDEF): preserve pre-instruction IT state and let
     * the outer stepper restore the faulting PC (A32-R7-13). */
    return ctx->exit_reason;
}

/* 32-bit Thumb-2 single step (subset + VFP routing + UNDEF fallback). */

static uint32_t t32_step(a32_ctx_t *ctx, uint32_t pc, uint16_t hw1, uint16_t hw2)
{
    uint32_t next = pc + 4U;
    /* VFP/NEON coprocessor patterns route to interp_vfp via synthetic word. */
    uint8_t h1hi = (uint8_t)(hw1 >> 8);
    if (h1hi == 0xEDU || h1hi == 0xFDU || h1hi == 0xEEU || h1hi == 0xFEU) {
        uint32_t word = ((uint32_t)hw1 << 16) | hw2;
        /* Rebuild the A32-style coprocessor view for the VFP unit. */
        if (a32_interp_vfp_step(ctx, word)) {
            if (ctx->exit_reason != A32_EXIT_NONE) {
                return ctx->exit_reason;
            }
            if (ctx->regs[15] == pc) {
                ctx->regs[15] = next;
            }
            return A32_EXIT_NONE;
        }
        ctx->regs[15] = next;
        i_undef(ctx);
        return A32_EXIT_UNDEF;
    }
    /* B.W T4 (unconditional, hw2[15:14,12]=101) and B<cond>.W T3
     * (conditional, hw2[15:14,12]=100, cond=hw1[9:6] < 14).
     * Data-processing modified-immediate has hw2[15]=0 and does not match. */
    if ((hw1 & 0xF800U) == 0xF000U && (hw2 & 0xD000U) == 0x9000U) {
        uint32_t s = (uint32_t)((hw1 >> 10U) & 1U);
        uint32_t j1 = (uint32_t)((hw2 >> 13U) & 1U);
        uint32_t j2 = (uint32_t)((hw2 >> 11U) & 1U);
        uint32_t i1 = ~(j1 ^ s) & 1U;
        uint32_t i2 = ~(j2 ^ s) & 1U;
        uint32_t uoff = (s << 24U) | (i1 << 23U) | (i2 << 22U) |
            ((uint32_t)(hw1 & 0x3FFU) << 12U) | ((uint32_t)(hw2 & 0x7FFU) << 1U);
        int32_t off = (int32_t)(uoff << 7U) >> 7;
        i_set_pc_branch(ctx, (pc + 4U) + (uint32_t)off, false);
        return A32_EXIT_NONE;
    }
    if ((hw1 & 0xF800U) == 0xF000U && (hw2 & 0xD000U) == 0x8000U &&
        ((hw1 >> 6U) & 0xFU) < 14U) {
        unsigned cond = (hw1 >> 6U) & 0xFU;
        if (!a32_interp_cond_passes(ctx->cpsr, cond)) {
            ctx->regs[15] = next;
            return A32_EXIT_NONE;
        }
        uint32_t s = (uint32_t)((hw1 >> 10U) & 1U);
        uint32_t j1 = (uint32_t)((hw2 >> 13U) & 1U);
        uint32_t j2 = (uint32_t)((hw2 >> 11U) & 1U);
        uint32_t uoff = (s << 20U) | (j2 << 19U) | (j1 << 18U) |
            ((uint32_t)(hw1 & 0x3FU) << 12U) | ((uint32_t)(hw2 & 0x7FFU) << 1U);
        int32_t off = (int32_t)(uoff << 11U) >> 11;
        i_set_pc_branch(ctx, (pc + 4U) + (uint32_t)off, false);
        return A32_EXIT_NONE;
    }
    if ((hw1 & 0xFFF0U) == 0xF3C0U && (hw2 & 0xFF00U) == 0xBF00U) {
        ctx->itstate = hw2 & 0xFFU;
        a32_ctx_normalize_itstate(ctx, true);
        ctx->regs[15] = next;
        return A32_EXIT_NONE;
    }
    if ((hw1 & 0xFE00U) == 0xF800U) {
        bool sign_ext = ((hw1 >> 8) & 1U) != 0U;
        bool u_imm12  = ((hw1 >> 7) & 1U) != 0U;
        unsigned size = (hw1 >> 5) & 3U; /* 0=byte, 1=half, 2=word */
        bool is_load  = ((hw1 >> 4) & 1U) != 0U;
        unsigned rn   = hw1 & 0xFU;
        unsigned rt   = (hw2 >> 12) & 0xFU;

        if (size > 2U || (size == 2U && sign_ext) ||
            (!is_load && (sign_ext || rt == 15U || rn == 15U))) {
            ctx->regs[15] = next;
            i_undef(ctx);
            return A32_EXIT_UNDEF;
        }

        uint32_t ea = 0U;
        uint32_t wb_val = 0U;
        bool do_wb = false;

        if (rn == 15U) {
            uint32_t base = (pc + 4U) & ~3U;
            uint32_t imm12 = (uint32_t)(hw2 & 0xFFFU);
            ea = u_imm12 ? (base + imm12) : (base - imm12);
        } else if (u_imm12) {
            uint32_t imm12 = (uint32_t)(hw2 & 0xFFFU);
            ea = ctx->regs[rn] + imm12;
        } else if ((hw2 & 0x0800U) == 0U) {
            if ((hw2 & 0x0FC0U) != 0U) {
                ctx->regs[15] = next;
                i_undef(ctx);
                return A32_EXIT_UNDEF;
            }
            unsigned rm = hw2 & 0xFU;
            unsigned shift = (hw2 >> 4) & 3U;
            ea = ctx->regs[rn] + (ctx->regs[rm] << shift);
        } else {
            bool p_bit = ((hw2 >> 10) & 1U) != 0U;
            bool u_bit = ((hw2 >> 9) & 1U) != 0U;
            bool w_bit = ((hw2 >> 8) & 1U) != 0U;
            if (!p_bit && !w_bit) {
                ctx->regs[15] = next;
                i_undef(ctx);
                return A32_EXIT_UNDEF;
            }
            if (rt == 15U && is_load && size != 2U && (hw2 & 0x0F00U) != 0x0C00U) {
                ctx->regs[15] = next;
                i_undef(ctx);
                return A32_EXIT_UNDEF;
            }
            uint32_t imm8 = (uint32_t)(hw2 & 0xFFU);
            uint32_t base = ctx->regs[rn];
            wb_val = u_bit ? (base + imm8) : (base - imm8);
            ea = p_bit ? wb_val : base;
            do_wb = (!p_bit || w_bit) && (rn != rt || !is_load);
        }

        if (rt == 15U && is_load && size != 2U) {
            /* PLD / PLI hint */
            ctx->regs[15] = next;
            return A32_EXIT_NONE;
        }

        if (is_load) {
            uint32_t v = 0U;
            if (size == 0U) {
                uint8_t b = 0U;
                if (!i_load_bytes(ctx, ea, &b, 1U)) {
                    return A32_EXIT_FAULT;
                }
                v = sign_ext ? (uint32_t)(int32_t)(int8_t)b : (uint32_t)b;
            } else if (size == 1U) {
                uint16_t h = 0U;
                if (!i_load_bytes(ctx, ea, &h, 2U)) {
                    return A32_EXIT_FAULT;
                }
                v = sign_ext ? (uint32_t)(int32_t)(int16_t)h : (uint32_t)h;
            } else {
                if (!i_load_u32(ctx, ea, &v)) {
                    return A32_EXIT_FAULT;
                }
            }
            if (do_wb) {
                ctx->regs[rn] = wb_val;
            }
            if (rt == 15U) {
                i_set_pc_branch(ctx, v, true);
                return A32_EXIT_NONE;
            }
            ctx->regs[rt] = v;
        } else {
            if (size == 0U) {
                uint8_t b = (uint8_t)ctx->regs[rt];
                if (!i_store_bytes(ctx, ea, &b, 1U)) {
                    return A32_EXIT_FAULT;
                }
            } else if (size == 1U) {
                uint16_t h = (uint16_t)ctx->regs[rt];
                if (!i_store_bytes(ctx, ea, &h, 2U)) {
                    return A32_EXIT_FAULT;
                }
            } else {
                if (!i_store_u32(ctx, ea, ctx->regs[rt])) {
                    return A32_EXIT_FAULT;
                }
            }
            if (do_wb) {
                ctx->regs[rn] = wb_val;
            }
        }
        ctx->regs[15] = next;
        return A32_EXIT_NONE;
    }
    /* UDF.W (F7F0 A000): permanent undefined instruction. It must trap
     * as UNDEF and never dispatch the syscall selected by r7 (A32-R7-1).
     * The outer stepper restores the faulting PC; IT is preserved by the
     * caller so retry uses the pre-instruction slot (A32-R7-13). */
    if (hw1 == 0xF7F0U && (hw2 & 0xFF00U) == 0xA000U) {
        ctx->regs[15] = next;
        i_undef(ctx);
        return A32_EXIT_UNDEF;
    }
    ctx->regs[15] = next;
    i_undef(ctx);
    return A32_EXIT_UNDEF;
}


uint32_t i_thumb_step(a32_ctx_t *ctx, uint32_t pc)
{
    uint8_t it = (uint8_t)(ctx->itstate & 0xFFU);
    bool in_it = (it & 0x0FU) != 0U;
    unsigned itc = (unsigned)((it >> 4) & 0xFU);
    uint16_t hw1 = 0U;
    if (!i_fetch16(ctx, pc, &hw1)) {
        return A32_EXIT_FAULT;
    }
    bool is32 = (hw1 & 0xE000U) == 0xE000U && (hw1 & 0x1800U) != 0U;
    if (!is32) {
        return t16_step(ctx, pc, &in_it, &itc);
    }
    uint16_t hw2 = 0U;
    if (!i_fetch16(ctx, pc + 2U, &hw2)) {
        return A32_EXIT_FAULT;
    }
    if (in_it && !a32_interp_cond_passes(ctx->cpsr, itc)) {
        uint8_t cur = (uint8_t)(ctx->itstate & 0xFFU);
        ctx->itstate = it_next(cur);
        a32_ctx_normalize_itstate(ctx, true);
        ctx->regs[15] = pc + 4U;
        return A32_EXIT_NONE;
    }
    uint32_t rc = t32_step(ctx, pc, hw1, hw2);
    if (in_it && rc != A32_EXIT_SVC && rc != A32_EXIT_BKPT) {
        if (rc == A32_EXIT_NONE) {
            /* Advance IT on completion independently of the branch
             * destination; a taken branch still consumes its IT slot. */
            uint8_t cur = (uint8_t)(ctx->itstate & 0xFFU);
            ctx->itstate = it_next(cur);
            a32_ctx_normalize_itstate(ctx, true);
        }
        /* UNDEF preserves pre-instruction IT so retry uses the same slot. */
    }
    return rc;
}

