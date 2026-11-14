/*
 * src/interp/interp_t32_mem_step.c — T32 memory / block / exclusive step.
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "interp/interp_priv.h"
#include "interp/decode_priv.h"
#include <sys/mman.h>
#include "vm/vm32.h"
/* Seqlock exclusives on the shared vm32 reservation table (mirrors the
 * do_ldrex family in interp_a32.c). Hosts are LE (see vm32.h), so narrow
 * guest loads land in the low bytes of a zeroed word. */
static bool t32_ldrex(a32_ctx_t *ctx, uint32_t va, unsigned size,
    uint32_t *out)
{
    void *hva;
    uint32_t v = 0U;
    if (ctx == NULL || out == NULL ||
        (size != 1U && size != 2U && size != 4U) ||
        (va & (size - 1U)) != 0U) {
        return false;
    }
    hva = a32_vm32_hva(ctx, va);
    for (;;) {
        uint64_t e0 = a32_vm32_ll_epoch(va);
        if (size == 1U) {
            v = __atomic_load_n((uint8_t *)hva, __ATOMIC_SEQ_CST);
        } else if (size == 2U) {
            v = __atomic_load_n((uint16_t *)(void *)hva, __ATOMIC_SEQ_CST);
        } else {
            v = __atomic_load_n((uint32_t *)(void *)hva, __ATOMIC_SEQ_CST);
        }
        a32_vm32_ll_record(ctx, va, size, v);
        if (e0 == a32_vm32_ll_epoch(va)) {
            break;
        }
    }
    ctx->excl_valid = true;
    ctx->excl_addr = va;
    ctx->excl_size = size;
    *out = v;
    return true;
}
static uint32_t t32_strex(a32_ctx_t *ctx, uint32_t va, unsigned size,
    uint32_t val)
{
    bool have;
    if (ctx == NULL || (size != 1U && size != 2U && size != 4U) ||
        (va & (size - 1U)) != 0U) {
        return 1U;
    }
    have = ctx->excl_valid && ctx->excl_addr == va && ctx->excl_size == size &&
        a32_vm32_sc_attempt(ctx, va, size, val);
    ctx->excl_valid = false;
    return have ? 0U : 1U;
}
static bool t32_ldrex64(a32_ctx_t *ctx, uint32_t va, uint64_t *out)
{
    void *hva;
    uint64_t v = 0U;
    if (ctx == NULL || out == NULL || (va & 7U) != 0U) {
        return false;
    }
    hva = a32_vm32_hva(ctx, va);
    for (;;) {
        uint64_t e0 = a32_vm32_ll_epoch(va);
        v = __atomic_load_n((uint64_t *)(void *)hva, __ATOMIC_SEQ_CST);
        a32_vm32_ll_record64(ctx, va, v);
        if (e0 == a32_vm32_ll_epoch(va)) {
            break;
        }
    }
    ctx->excl_valid = true;
    ctx->excl_addr = va;
    ctx->excl_size = 8U;
    *out = v;
    return true;
}
static uint32_t t32_strex64(a32_ctx_t *ctx, uint32_t va, uint64_t val)
{
    bool have;
    if (ctx == NULL || (va & 7U) != 0U) {
        return 1U;
    }
    have = ctx->excl_valid && ctx->excl_addr == va && ctx->excl_size == 8U &&
        a32_vm32_sc_attempt64(ctx, va, val);
    ctx->excl_valid = false;
    return have ? 0U : 1U;
}
static uint32_t t32_undef(a32_ctx_t *ctx, uint32_t next)
{
    ctx->regs[15] = next;
    i_undef(ctx);
    return A32_EXIT_UNDEF;
}
uint32_t i_t32_mem_step(a32_ctx_t *ctx, uint32_t pc, uint16_t hw1,
    uint16_t hw2)
{
    uint32_t next = pc + 4U, pc_val = pc + 4U;
    uint32_t pc_align4 = pc_val & ~3U;
    if ((hw1 & 0xFE00U) == 0xE800U) {
        if ((hw1 & 0xFE40U) == 0xE800U &&
            (((hw1 >> 7) & 3U) == 1U || ((hw1 >> 7) & 3U) == 2U)) {
            bool u_bit = ((hw1 >> 7) & 1U) != 0U, w_bit = ((hw1 >> 5) & 1U) != 0U;
            bool l_bit = ((hw1 >> 4) & 1U) != 0U;
            unsigned rn = hw1 & 0xFU;
            uint32_t mask = (uint32_t)hw2 & 0xDFFFU, base, addr, cur, v = 0U;
            unsigned n = 0U;
            uint32_t loaded_pc = 0U;
            if (mask == 0U || rn == 15U || (!l_bit && (hw2 & 0x8000U) != 0U)) {
                return t32_undef(ctx, next);
            }
            for (unsigned k = 0U; k < 16U; k++) {
                if (((mask >> k) & 1U) != 0U) {
                    n++;
                }
            }
            base = ctx->regs[rn];
            addr = u_bit ? base : (base - 4U * n);
            if ((addr & 3U) != 0U) {
                return i_fault(ctx, addr, !l_bit);
            }
            if ((uint64_t)addr + (uint64_t)n * 4U > 0x100000000ULL ||
                !i_mem_ok(ctx, addr, n * 4U,
                    l_bit ? (uint32_t)PROT_READ : (uint32_t)PROT_WRITE)) {
                return i_fault(ctx, addr, !l_bit);
            }
            cur = addr;
            for (unsigned r = 0U; r < 16U; r++) {
                if (((mask >> r) & 1U) == 0U) {
                    continue;
                }
                if (l_bit) {
                    if (!i_load_u32(ctx, cur, &v)) {
                        return A32_EXIT_FAULT;
                    }
                    if (r == 15U) {
                        loaded_pc = v;
                    } else {
                        ctx->regs[r] = v;
                    }
                } else if (!i_store_u32(ctx, cur, ctx->regs[r])) {
                    return A32_EXIT_FAULT;
                }
                cur += 4U;
            }
            if (w_bit && !(l_bit && ((mask >> rn) & 1U) != 0U)) {
                ctx->regs[rn] = u_bit ? (base + 4U * n) : (base - 4U * n);
            }
            if (l_bit && (mask & 0x8000U) != 0U) {
                i_set_pc_branch(ctx, loaded_pc, true);
                return A32_EXIT_NONE;
            }
            ctx->regs[15] = next;
            return A32_EXIT_NONE;
        }
        if ((hw1 & 0xFFF0U) == 0xE8D0U && (hw2 & 0xFFE0U) == 0xF000U) {
            bool is_tbh = ((hw2 >> 4) & 1U) != 0U;
            unsigned rn = hw1 & 0xFU, rm = hw2 & 0xFU;
            uint32_t base = (rn == 15U) ? pc_val : ctx->regs[rn];
            uint32_t ea = base + (ctx->regs[rm] << (is_tbh ? 1U : 0U));
            uint32_t idx = 0U;
            if (!i_load_bytes(ctx, ea, &idx, is_tbh ? 2U : 1U)) {
                return A32_EXIT_FAULT;
            }
            i_set_pc_branch(ctx, pc_val + (idx << 1U), false);
            return A32_EXIT_NONE;
        }
        if ((hw1 & 0xFFE0U) == 0xE840U) {
            bool is_load = ((hw1 >> 4) & 1U) != 0U;
            unsigned rn = hw1 & 0xFU, rt = (hw2 >> 12) & 0xFU;
            unsigned rd = (hw2 >> 8) & 0xFU;
            uint32_t va;
            if (rn == 15U || rt == 15U || (!is_load && rd == 15U)) {
                return t32_undef(ctx, next);
            }
            va = ctx->regs[rn] + ((((uint32_t)hw2) & 0xFFU) << 2);
            if ((va & 3U) != 0U ||
                !i_mem_ok(ctx, va, 4U,
                    is_load ? (uint32_t)PROT_READ : (uint32_t)PROT_WRITE)) {
                return i_fault(ctx, va, !is_load);
            }
            if (is_load) {
                uint32_t v = 0U;
                t32_ldrex(ctx, va, 4U, &v);
                ctx->regs[rt] = v;
            } else {
                ctx->regs[rd] = t32_strex(ctx, va, 4U, ctx->regs[rt]);
            }
            ctx->regs[15] = next;
            return A32_EXIT_NONE;
        }
        if ((hw1 & 0xFFE0U) == 0xE8C0U) {
            bool is_load = ((hw1 >> 4) & 1U) != 0U;
            unsigned rn = hw1 & 0xFU, rt = (hw2 >> 12) & 0xFU;
            unsigned rt2 = (hw2 >> 8) & 0xFU, op3 = (hw2 >> 4) & 0xFU;
            unsigned rd = hw2 & 0xFU;
            unsigned size = (op3 == 4U) ? 1U : (op3 == 5U) ? 2U :
                (op3 == 7U) ? 8U : 0U;
            uint32_t va;
            if (size == 0U) {
                return t32_undef(ctx, next);
            }
            if (rn == 15U || rt == 15U || (!is_load && rd == 15U) ||
                (size == 8U && rt2 == 15U)) {
                return t32_undef(ctx, next);
            }
            va = ctx->regs[rn];
            if ((va & (size - 1U)) != 0U ||
                !i_mem_ok(ctx, va, size,
                    is_load ? (uint32_t)PROT_READ : (uint32_t)PROT_WRITE)) {
                return i_fault(ctx, va, !is_load);
            }
            if (size == 8U) {
                if (is_load) {
                    uint64_t dv = 0ULL;
                    t32_ldrex64(ctx, va, &dv);
                    ctx->regs[rt] = (uint32_t)dv;
                    ctx->regs[rt2] = (uint32_t)(dv >> 32);
                } else {
                    uint64_t dv = ((uint64_t)ctx->regs[rt2] << 32) |
                        (uint64_t)ctx->regs[rt];
                    ctx->regs[rd] = t32_strex64(ctx, va, dv);
                }
            } else if (is_load) {
                uint32_t v = 0U;
                t32_ldrex(ctx, va, size, &v);
                ctx->regs[rt] = v;
            } else {
                ctx->regs[rd] = t32_strex(ctx, va, size, ctx->regs[rt]);
            }
            ctx->regs[15] = next;
            return A32_EXIT_NONE;
        }
        if ((hw1 & 0x0040U) != 0U && (hw1 & 0x0120U) != 0U) {
            bool p_bit = ((hw1 >> 8) & 1U) != 0U, u_bit = ((hw1 >> 7) & 1U) != 0U;
            bool w_bit = ((hw1 >> 5) & 1U) != 0U, l_bit = ((hw1 >> 4) & 1U) != 0U;
            unsigned rn = hw1 & 0xFU, rt = (hw2 >> 12) & 0xFU;
            unsigned rt2 = (hw2 >> 8) & 0xFU;
            uint32_t off = ((uint32_t)hw2 & 0xFFU) << 2, base, ea, acc;
            if (rt == 15U || rt2 == 15U || (!l_bit && rn == 15U)) {
                return t32_undef(ctx, next);
            }
            base = (rn == 15U) ? pc_align4 : ctx->regs[rn];
            ea = u_bit ? (base + off) : (base - off);
            acc = p_bit ? ea : base;
            if ((acc & 3U) != 0U ||
                !i_mem_ok(ctx, acc, 8U,
                    l_bit ? (uint32_t)PROT_READ : (uint32_t)PROT_WRITE)) {
                return i_fault(ctx, acc, !l_bit);
            }
            if (l_bit) {
                uint32_t v0 = 0U, v1 = 0U;
                if (!i_load_u32(ctx, acc, &v0) ||
                    !i_load_u32(ctx, acc + 4U, &v1)) {
                    return A32_EXIT_FAULT;
                }
                ctx->regs[rt] = v0;
                ctx->regs[rt2] = v1;
            } else if (!i_store_u32(ctx, acc, ctx->regs[rt]) ||
                !i_store_u32(ctx, acc + 4U, ctx->regs[rt2])) {
                return A32_EXIT_FAULT;
            }
            if ((!p_bit || w_bit) && rn != 15U &&
                !(l_bit && (rn == rt || rn == rt2))) {
                ctx->regs[rn] = ea;
            }
            ctx->regs[15] = next;
            return A32_EXIT_NONE;
        }
    }
    if ((hw1 & 0xFE00U) == 0xF800U) {
        bool sign_ext = ((hw1 >> 8) & 1U) != 0U, u_imm12 = ((hw1 >> 7) & 1U) != 0U;
        bool is_load = ((hw1 >> 4) & 1U) != 0U;
        unsigned size = (unsigned)((hw1 >> 5) & 3U);
        unsigned rn = hw1 & 0xFU, rt = (hw2 >> 12) & 0xFU;
        t32_ls_dec_t ls = t32_decode_ls(rn, rt, u_imm12, size, is_load,
            sign_ext, hw2);
        uint32_t base = 0U, addr = 0U, wb_val = 0U, v = 0U;
        if (ls.undef) {
            return t32_undef(ctx, next);
        }
        if (ls.hint) {
            ctx->regs[15] = next;
            return A32_EXIT_NONE;
        }
        if (ls.kind == 0U) {
            addr = ls.up ? (pc_align4 + (uint32_t)ls.imm) :
                (pc_align4 - (uint32_t)ls.imm);
        } else if (ls.kind == 1U) {
            base = ctx->regs[rn];
            addr = ls.up ? (base + (uint32_t)ls.imm) : (base - (uint32_t)ls.imm);
        } else if (ls.kind == 2U) {
            base = ctx->regs[rn];
            addr = base + (ctx->regs[ls.rm] << ls.shift);
        } else if (ls.pre) {
            base = ctx->regs[rn];
            addr = ls.up ? (base + (uint32_t)ls.imm) : (base - (uint32_t)ls.imm);
            wb_val = addr;
        } else {
            base = ctx->regs[rn];
            addr = base;
            wb_val = ls.up ? (base + (uint32_t)ls.imm) :
                (base - (uint32_t)ls.imm);
        }
        if (is_load) {
            if (size == 2U) {
                if (!i_load_u32(ctx, addr, &v)) {
                    return A32_EXIT_FAULT;
                }
            } else {
                uint16_t h = 0U;
                if (!i_load_bytes(ctx, addr, &h, (size == 0U) ? 1U : 2U)) {
                    return A32_EXIT_FAULT;
                }
                if (size == 0U) {
                    v = sign_ext ? (uint32_t)(int32_t)(int8_t)(uint8_t)h :
                        (uint32_t)(uint8_t)h;
                } else {
                    v = sign_ext ? (uint32_t)(int32_t)(int16_t)h : (uint32_t)h;
                }
            }
        } else if (size == 2U) {
            if (!i_store_u32(ctx, addr, ctx->regs[rt])) {
                return A32_EXIT_FAULT;
            }
        } else {
            uint16_t h = (uint16_t)ctx->regs[rt];
            if (!i_store_bytes(ctx, addr, &h, (size == 0U) ? 1U : 2U)) {
                return A32_EXIT_FAULT;
            }
        }
        if (ls.wb) {
            ctx->regs[rn] = wb_val;
        }
        if (is_load && rt == 15U) {
            i_set_pc_branch(ctx, v, true);
            return A32_EXIT_NONE;
        }
        if (is_load) {
            ctx->regs[rt] = v;
        }
        ctx->regs[15] = next;
        return A32_EXIT_NONE;
    }
    return t32_undef(ctx, next);
}
