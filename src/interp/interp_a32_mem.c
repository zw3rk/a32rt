/*
 * src/interp/interp_a32_mem.c — A32 extra load/store + LDM/STM step.
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Split from interp_a32.c (line-limit split): the LDRH/STRH/LDRSB/LDRSH/
 * LDRD/STRD block and the LDM/STM block. Called from i_a32_step between
 * SWP and the media instructions so dispatch order is unchanged.
 */

#include "interp.h"
#include "interp_priv.h"

#include <sys/mman.h>

uint32_t i_a32_mem_step(a32_ctx_t *ctx, uint32_t pc, uint32_t insn,
    bool *claimed)
{
    *claimed = true;
    if ((insn & 0x0E000090U) == 0x00000090U && ((insn >> 5) & 3U) != 0U) {
        /* Extra load/store: LDRH/STRH/LDRSB/LDRSH/LDRD/STRD. */
        bool p = ((insn >> 24) & 1U) != 0U;
        bool u = ((insn >> 23) & 1U) != 0U;
        bool i_bit = ((insn >> 22) & 1U) != 0U;
        bool w = ((insn >> 21) & 1U) != 0U;
        bool l = ((insn >> 20) & 1U) != 0U;
        unsigned rn = (insn >> 16) & 0xFU;
        unsigned rt = (insn >> 12) & 0xFU;
        unsigned op2 = (insn >> 5) & 3U;
        uint32_t off = i_bit ? ((((insn >> 8) & 0xFU) << 4) | (insn & 0xFU))
                             : ctx->regs[insn & 0xFU];
        uint32_t base = (rn == 15U) ? pc + 8U : ctx->regs[rn];
        uint32_t ea = u ? base + off : base - off;
        uint32_t wb = ea;
        uint32_t acc = p ? ea : base;
        if (!l && (op2 == 2U || op2 == 3U)) {
            bool is_ldrd = (op2 == 2U);
            /* Doubleword transfers require word alignment (A32-R7-6). */
            if ((acc & 3U) != 0U) {
                ctx->fault_addr = acc;
                ctx->exit_reason = A32_EXIT_FAULT;
                return A32_EXIT_FAULT;
            }
            if (!i_mem_ok(ctx, acc, 8U, is_ldrd ? (uint32_t)PROT_READ : (uint32_t)PROT_WRITE)) {
                ctx->fault_addr = acc;
                ctx->exit_reason = A32_EXIT_FAULT;
                return A32_EXIT_FAULT;
            }
            if (is_ldrd) {
                uint32_t lo = 0U;
                uint32_t hi = 0U;
                if (!i_load_u32(ctx, acc, &lo) || !i_load_u32(ctx, acc + 4U, &hi)) {
                    return A32_EXIT_FAULT;
                }
                ctx->regs[rt] = lo;
                ctx->regs[(rt + 1U) & 0xFU] = hi;
            } else {
                /* Route through the checked store path so the write
                 * participates in reservation invalidation (A32-R7-7). */
                if (!i_store_u32(ctx, acc, ctx->regs[rt]) ||
                    !i_store_u32(ctx, acc + 4U, ctx->regs[(rt + 1U) & 0xFU])) {
                    return A32_EXIT_FAULT;
                }
            }
        } else if (l) {
            uint32_t v = 0U;
            if (op2 == 1U) {
                uint16_t h = 0U;
                if (!i_load_bytes(ctx, acc, &h, 2U)) {
                    return A32_EXIT_FAULT;
                }
                v = h;
            } else if (op2 == 2U) {
                int8_t b = 0;
                if (!i_load_bytes(ctx, acc, &b, 1U)) {
                    return A32_EXIT_FAULT;
                }
                v = (uint32_t)(int32_t)b;
            } else {
                int16_t h = 0;
                if (!i_load_bytes(ctx, acc, &h, 2U)) {
                    return A32_EXIT_FAULT;
                }
                v = (uint32_t)(int32_t)h;
            }
            if (rt == 15U) {
                i_set_pc_branch(ctx, v, false);
                return A32_EXIT_NONE;
            }
            ctx->regs[rt] = v;
        } else {
            uint16_t h = (uint16_t)ctx->regs[rt];
            if (!i_store_bytes(ctx, acc, &h, 2U)) {
                return A32_EXIT_FAULT;
            }
        }
        {
            bool is_load_op = l || (!l && op2 == 2U);
            bool loads_rn = is_load_op && (rn == rt || (!l && op2 == 2U && rn == ((rt + 1U) & 0xFU)));
            if ((!p || w) && rn != 15U && !loads_rn) {
                ctx->regs[rn] = wb;
            }
        }
        ctx->regs[15] = pc + 4U;
        return A32_EXIT_NONE;
    }
    /* LDM / STM. */
    if ((insn & 0x0E000000U) == 0x08000000U) {
        bool p = ((insn >> 24) & 1U) != 0U;
        bool u = ((insn >> 23) & 1U) != 0U;
        bool w = ((insn >> 21) & 1U) != 0U;
        bool l = ((insn >> 20) & 1U) != 0U;
        unsigned rn = (insn >> 16) & 0xFU;
        uint16_t list = (uint16_t)insn;
        unsigned n = 0U;
        for (unsigned k = 0U; k < 16U; k++) {
            if ((list >> k) & 1U) {
                n++;
            }
        }
        if (n == 0U) {
            ctx->regs[15] = pc + 4U;
            i_undef(ctx);
            return A32_EXIT_UNDEF;
        }
        uint32_t base = ctx->regs[rn];
        uint32_t addr;
        if (u) {
            addr = p ? base + 4U : base;
        } else {
            addr = p ? base - 4U * n : base - 4U * n + 4U;
        }
        /* Block transfers require a word-aligned start address (A32-R7-6). */
        if ((addr & 3U) != 0U) {
            ctx->fault_addr = addr;
            ctx->exit_reason = A32_EXIT_FAULT;
            return A32_EXIT_FAULT;
        }
        /* Validate the full word span before touching any register or
         * memory so a mid-list fault leaves no partial update behind. */
        {
            uint64_t span_end = (uint64_t)addr + (uint64_t)n * 4U;
            if (span_end > 0x100000000ULL ||
                !i_mem_ok(ctx, addr, n * 4U,
                    l ? (uint32_t)PROT_READ : (uint32_t)PROT_WRITE)) {
                ctx->fault_addr = addr;
                ctx->exit_reason = A32_EXIT_FAULT;
                return A32_EXIT_FAULT;
            }
        }
        uint32_t cur = addr;
        bool pc_in = (list & 0x8000U) != 0U;
        for (unsigned r = 0U; r < 16U; r++) {
            if (((list >> r) & 1U) == 0U) {
                continue;
            }
            if (l) {
                uint32_t v = 0U;
                if (!i_load_u32(ctx, cur, &v)) {
                    return A32_EXIT_FAULT;
                }
                if (r == 15U) {
                    i_set_pc_branch(ctx, v, pc_in ? true : false);
                } else if (!(w && r == rn)) {
                    ctx->regs[r] = v;
                } else if (r == rn) {
                    ctx->regs[r] = v;
                }
            } else {
                uint32_t v = (r == 15U) ? pc + 8U : ctx->regs[r];
                if (!i_store_u32(ctx, cur, v)) {
                    return A32_EXIT_FAULT;
                }
            }
            cur += 4U;
        }
        if (w && !(l && ((list >> rn) & 1U) != 0U)) {
            ctx->regs[rn] = u ? base + 4U * n : base - 4U * n;
        }
        if (l && pc_in) {
            return A32_EXIT_NONE;
        }
        ctx->regs[15] = pc + 4U;
        return A32_EXIT_NONE;
    }
    *claimed = false;
    return A32_EXIT_NONE;
}
