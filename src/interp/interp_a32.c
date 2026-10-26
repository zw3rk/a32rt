/*
 * src/interp/interp_a32.c — A32 (ARM mode) instruction step.
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "interp.h"
#include "interp_priv.h"

#include <string.h>
#include <sys/mman.h>

#include "vm/vm32.h"

/* LDREX/STREX exclusives ride on the shared vm32 reservation table so
 * the interpreter and the DBT slow path observe one atomic order
 * (section 4.4). All RMWs are C11 __atomic SEQ_CST on HVA. Unaligned
 * addresses cannot reserve: the caller raises UNDEF. */
static bool g_excl_valid = false;
static uint32_t g_excl_addr = 0U;
static unsigned g_excl_size = 0U;

static bool do_ldrex(a32_ctx_t *ctx, uint32_t va, unsigned size,
    uint32_t *out)
{
    void *hva;
    uint32_t v = 0U;
    if (out == NULL || (size != 1U && size != 2U && size != 4U) ||
        (va & (size - 1U)) != 0U) {
        return false;
    }
    hva = a32_vm32_hva(ctx, va);
    if (size == 1U) {
        v = __atomic_load_n((uint8_t *)hva, __ATOMIC_SEQ_CST);
    } else if (size == 2U) {
        v = __atomic_load_n((uint16_t *)(void *)hva, __ATOMIC_SEQ_CST);
    } else {
        v = __atomic_load_n((uint32_t *)(void *)hva, __ATOMIC_SEQ_CST);
    }
    a32_vm32_ll_record(ctx, va, size, v);
    g_excl_valid = true;
    g_excl_addr = va;
    g_excl_size = size;
    *out = v;
    return true;
}

static uint32_t do_strex(a32_ctx_t *ctx, uint32_t va, unsigned size,
    uint32_t val)
{
    bool have;
    if (size != 1U && size != 2U && size != 4U) {
        return 1U;
    }
    if ((va & (size - 1U)) != 0U) {
        return 1U;
    }
    have = g_excl_valid && g_excl_addr == va && g_excl_size == size &&
        a32_vm32_sc_attempt(ctx, va, size, val);
    g_excl_valid = false;
    return have ? 0U : 1U;
}


static uint32_t a32_shift_op(uint32_t insn, uint32_t rm_val, uint32_t rs_val,
    bool rs_shift, bool *carry, uint32_t cpsr)
{
    unsigned st = (insn >> 5) & 3U;
    unsigned amt = rs_shift ? (rs_val & 0xFFU) : ((insn >> 7) & 31U);
    bool cin = (cpsr & A32_CPSR_C) != 0U;
    if (!rs_shift && amt == 0U) {
        if (st == 0U) {
            *carry = cin;
            return rm_val;
        }
        if (st == 1U) {
            *carry = (rm_val >> 31) & 1U;
            return 0U;
        }
        if (st == 2U) {
            *carry = (rm_val >> 31) & 1U;
            return ((rm_val & 0x80000000U) != 0U) ? 0xFFFFFFFFU : 0U;
        }
        *carry = cin;
        return (cin ? 0xFFFFFFFFU : 0U) | (rm_val << 1);
    }
    if (amt >= 32U && (st == 0U || st == 1U)) {
        if (st == 0U) {
            *carry = (amt == 32U) ? ((rm_val & 1U) != 0U) : false;
            return (amt == 32U) ? (rm_val & 1U ? 0x80000000U : 0U) : 0U;
        }
        *carry = (rm_val >> 31) & 1U;
        return 0U;
    }
    switch (st) {
    case 0U: *carry = ((rm_val >> (amt - 1U)) & 1U) != 0U; return rm_val << amt;
    case 1U: *carry = ((rm_val >> (amt - 1U)) & 1U) != 0U; return rm_val >> amt;
    case 2U: {
        uint32_t r;
        *carry = ((rm_val >> (amt - 1U)) & 1U) != 0U;
        if ((rm_val & 0x80000000U) != 0U) {
            r = (rm_val >> amt) | (0xFFFFFFFFU << (32U - amt));
        } else {
            r = rm_val >> amt;
        }
        return r;
    }
    default:
        if (amt == 0U) {
            amt = 1U;
        }
        *carry = ((rm_val >> (amt - 1U)) & 1U) != 0U;
        return i_ror32(rm_val, amt);
    }
}


static uint32_t reg_rd_pc_adjust(const a32_ctx_t *ctx, unsigned r, uint32_t pc)
{
    if (r == 15U) {
        bool t = (ctx->cpsr & A32_CPSR_T) != 0U;
        return pc + (t ? 4U : 8U);
    }
    return ctx->regs[r];
}

/* Single A32 data-processing / multiply / misc step. */

static uint32_t a32_dp_step(a32_ctx_t *ctx, uint32_t pc, uint32_t insn)
{
    unsigned rd = (insn >> 12) & 0xFU;
    unsigned rn = (insn >> 16) & 0xFU;
    unsigned opc = (insn >> 21) & 0xFU;
    bool s = ((insn >> 20) & 1U) != 0U;
    bool imm = ((insn >> 25) & 1U) != 0U;
    uint32_t op2 = 0U;
    bool sh_carry = (ctx->cpsr & A32_CPSR_C) != 0U;

    if (imm) {
        op2 = i_ror32(insn & 0xFFU, ((insn >> 8) & 0xFU) * 2U);
        if (((insn >> 8) & 0xFU) != 0U) {
            sh_carry = (op2 >> 31) & 1U;
        }
    } else if (((insn >> 4) & 1U) != 0U && ((insn & 0x90U) != 0x90U || true)) {
        /* Register-shifted or multiply encodings share this space. */
        if ((insn & 0x0E000090U) == 0x00000090U) {
            /* Multiply: MUL/MLA/UMULL/SMULL. */
            unsigned rm = insn & 0xFU;
            unsigned rs = (insn >> 8) & 0xFU;
            unsigned rdh = (insn >> 16) & 0xFU;
            unsigned rdl = (insn >> 12) & 0xFU;
            bool u = ((insn >> 23) & 1U) != 0U;
            bool a = ((insn >> 21) & 1U) != 0U;
            bool lg = ((insn >> 22) & 1U) != 0U;
            uint32_t mv = ctx->regs[rm];
            uint32_t sv = ctx->regs[rs];
            if (!lg) {
                uint32_t r = mv * sv;
                if (a) {
                    r += ctx->regs[rdh];
                }
                ctx->regs[rdl] = r;
                if (s) {
                    i_set_nz(&ctx->cpsr, r);
                }
                ctx->regs[15] = pc + 4U;
                return A32_EXIT_NONE;
            }
            if (u) {
                uint64_t p = (uint64_t)mv * (uint64_t)sv;
                if (a) {
                    p += ((uint64_t)ctx->regs[rdh] << 32) | ctx->regs[rdl];
                }
                ctx->regs[rdl] = (uint32_t)p;
                ctx->regs[rdh] = (uint32_t)(p >> 32);
                if (s) {
                    i_set_nz(&ctx->cpsr, ctx->regs[rdh]);
                }
            } else {
                int64_t p = (int64_t)(int32_t)mv * (int64_t)(int32_t)sv;
                if (a) {
                    p += (int64_t)(((uint64_t)ctx->regs[rdh] << 32) | ctx->regs[rdl]);
                }
                ctx->regs[rdl] = (uint32_t)(uint64_t)p;
                ctx->regs[rdh] = (uint32_t)((uint64_t)p >> 32);
                if (s) {
                    i_set_nz(&ctx->cpsr, ctx->regs[rdh]);
                }
            }
            ctx->regs[15] = pc + 4U;
            return A32_EXIT_NONE;
        }
        {
            uint32_t rmv = reg_rd_pc_adjust(ctx, insn & 0xFU, pc);
            uint32_t rsv = ctx->regs[(insn >> 8) & 0xFU];
            op2 = a32_shift_op(insn, rmv, rsv, true, &sh_carry, ctx->cpsr);
        }
    } else {
        uint32_t rmv = reg_rd_pc_adjust(ctx, insn & 0xFU, pc);
        op2 = a32_shift_op(insn, rmv, 0U, false, &sh_carry, ctx->cpsr);
    }

    uint32_t rnv = reg_rd_pc_adjust(ctx, rn, pc);
    bool is_test = (opc == 8U || opc == 9U || opc == 10U || opc == 11U);
    bool want_w = s || is_test;
    uint32_t res = 0U;
    bool carry = sh_carry;
    bool ov = false;
    bool write = !is_test;

    switch (opc) {
    case 0x0U: res = rnv & op2; break;
    case 0x1U: res = rnv ^ op2; break;
    case 0x2U: res = i_add_carry(rnv, ~op2, 1U, &carry, &ov); break;
    case 0x3U: res = i_add_carry(~rnv, op2, 1U, &carry, &ov); break;
    case 0x4U: res = i_add_carry(rnv, op2, 0U, &carry, &ov); break;
    case 0x5U: res = i_add_carry(rnv, op2, (ctx->cpsr & A32_CPSR_C) ? 1U : 0U, &carry, &ov); break;
    case 0x6U: res = i_add_carry(rnv, ~op2, (ctx->cpsr & A32_CPSR_C) ? 1U : 0U, &carry, &ov); break;
    case 0x7U: res = i_add_carry(~rnv, op2, (ctx->cpsr & A32_CPSR_C) ? 1U : 0U, &carry, &ov); break;
    case 0x8U: res = rnv & op2; break;
    case 0x9U: res = rnv ^ op2; break;
    case 0xAU: res = i_add_carry(rnv, ~op2, 1U, &carry, &ov); break;
    case 0xBU: res = i_add_carry(rnv, op2, 0U, &carry, &ov); break;
    case 0xCU: res = rnv | op2; break;
    case 0xDU: res = op2; carry = sh_carry; break;
    case 0xEU: res = rnv & ~op2; break;
    default: res = ~op2; carry = sh_carry; break;
    }
    if (want_w) {
        if (opc == 0xDU || opc == 0xFU) {
            carry = sh_carry;
            ov = false;
        } else if (opc <= 0x1U || opc == 0xCU || opc == 0xEU) {
            ov = false;
        }
        i_set_nzcv(&ctx->cpsr, res, carry, ov);
    }
    if (write) {
        if (rd == 15U) {
            i_set_pc_branch(ctx, res, false);
            return A32_EXIT_NONE;
        }
        ctx->regs[rd] = res;
    }
    ctx->regs[15] = pc + 4U;
    return A32_EXIT_NONE;
}


static bool is_vfp_word(uint32_t insn)
{
    if ((insn & 0x0E000000U) == 0x0C000000U) {
        return true;
    }
    if ((insn & 0x0F000000U) == 0x0E000000U) {
        unsigned cp = (insn >> 8) & 0xFU;
        if (cp == 10U || cp == 11U) {
            return true;
        }
    }
    return false;
}

/* A32 single step for ARM mode. */

uint32_t i_a32_step(a32_ctx_t *ctx, uint32_t pc)
{
    uint32_t insn = 0U;
    if (!i_fetch32(ctx, pc, &insn)) {
        return A32_EXIT_FAULT;
    }
    unsigned cond = (insn >> 28) & 0xFU;
    if (cond == 0xFU) {
        if ((insn & 0xFE000000U) == 0xFA000000U) {
            int32_t off = (int32_t)((insn & 0x00FFFFFFU) << 8) >> 8;
            uint32_t target = (uint32_t)((int32_t)(pc + 8U) + (off << 2));
            ctx->regs[14] = pc + 4U;
            i_set_pc_branch(ctx, target, true);
            return A32_EXIT_NONE;
        }
        if ((insn & 0x0FFFFFF0U) == 0x07F000F0U) {
            ctx->regs[15] = pc + 4U;
            i_undef(ctx);
            return A32_EXIT_UNDEF;
        }
        ctx->regs[15] = pc + 4U;
        i_undef(ctx);
        return A32_EXIT_UNDEF;
    }
    if (!a32_interp_cond_passes(ctx->cpsr, cond)) {
        ctx->regs[15] = pc + 4U;
        return A32_EXIT_NONE;
    }
    /* SVC / BKPT share op1==7. */
    if ((insn & 0x0F000000U) == 0x0F000000U) {
        ctx->svc_imm = insn & 0x00FFFFFFU;
        ctx->regs[15] = pc + 4U;
        ctx->exit_reason = A32_EXIT_SVC;
        return A32_EXIT_SVC;
    }
    if ((insn & 0x0FF000F0U) == 0x01200070U) {
        ctx->regs[15] = pc + 4U;
        ctx->exit_reason = A32_EXIT_BKPT;
        return A32_EXIT_BKPT;
    }
    if (is_vfp_word(insn)) {
        if (a32_interp_vfp_step(ctx, insn)) {
            return A32_EXIT_NONE;
        }
        ctx->regs[15] = pc + 4U;
        i_undef(ctx);
        return A32_EXIT_UNDEF;
    }
    /* B / BL. */
    if ((insn & 0x0E000000U) == 0x0A000000U) {
        bool link = ((insn >> 24) & 1U) != 0U;
        int32_t off = (int32_t)((insn & 0x00FFFFFFU) << 8) >> 8;
        uint32_t target = (uint32_t)((int32_t)(pc + 8U) + (off << 2));
        if (link) {
            ctx->regs[14] = pc + 4U;
        }
        i_set_pc_branch(ctx, target, false);
        return A32_EXIT_NONE;
    }
    /* BX / BLX register. */
    if ((insn & 0x0FFFFFF0U) == 0x012FFF10U) {
        uint32_t t = ctx->regs[insn & 0xFU];
        i_set_pc_branch(ctx, t, true);
        return A32_EXIT_NONE;
    }
    if ((insn & 0x0FFFFFF0U) == 0x012FFF30U) {
        uint32_t t = ctx->regs[insn & 0xFU];
        ctx->regs[14] = pc + 4U;
        i_set_pc_branch(ctx, t, true);
        return A32_EXIT_NONE;
    }
    /* MRS / MSR (CPSR subset: NZCVQ + GE only; mode bits ignored). */
    if ((insn & 0x0FBFFFF0U) == 0x010F0000U) {
        unsigned rd = (insn >> 12) & 0xFU;
        if (rd != 15U) {
            ctx->regs[rd] = ctx->cpsr;
        }
        ctx->regs[15] = pc + 4U;
        return A32_EXIT_NONE;
    }
    if ((insn & 0x0DBFF000U) == 0x0128F000U || (insn & 0x0DBFF000U) == 0x012AF000U) {
        uint32_t v;
        if (((insn >> 25) & 1U) != 0U) {
            v = i_ror32(insn & 0xFFU, ((insn >> 8) & 0xFU) * 2U);
        } else {
            v = ctx->regs[insn & 0xFU];
        }
        ctx->cpsr = (ctx->cpsr & ~ (A32_CPSR_NZCV_MASK | A32_CPSR_Q | A32_CPSR_GE_MASK)) |
            (v & (A32_CPSR_NZCV_MASK | A32_CPSR_Q | A32_CPSR_GE_MASK));
        ctx->regs[15] = pc + 4U;
        return A32_EXIT_NONE;
    }
    /* LDREX / STREX. */
    if ((insn & 0x0FF000F0U) == 0x01900090U) {
        unsigned rn = (insn >> 16) & 0xFU;
        unsigned rt = (insn >> 12) & 0xFU;
        uint32_t va = ctx->regs[rn];
        uint32_t v = 0U;
        if ((va & 3U) != 0U) {
            i_undef(ctx);
            return A32_EXIT_UNDEF;
        }
        if (!i_mem_ok(ctx, va & ~3U, 4U, (uint32_t)PROT_READ)) {
            ctx->fault_addr = va;
            ctx->exit_reason = A32_EXIT_FAULT;
            return A32_EXIT_FAULT;
        }
        if (!do_ldrex(ctx, va, 4U, &v)) {
            i_undef(ctx);
            return A32_EXIT_UNDEF;
        }
        ctx->regs[rt] = v;
        ctx->regs[15] = pc + 4U;
        return A32_EXIT_NONE;
    }
    if ((insn & 0x0FF000F0U) == 0x01800090U) {
        unsigned rn = (insn >> 16) & 0xFU;
        unsigned rd = (insn >> 12) & 0xFU;
        unsigned rm = insn & 0xFU;
        uint32_t va = ctx->regs[rn];
        if ((va & 3U) != 0U) {
            i_undef(ctx);
            return A32_EXIT_UNDEF;
        }
        if (!i_mem_ok(ctx, va & ~3U, 4U, (uint32_t)PROT_WRITE)) {
            ctx->fault_addr = va;
            ctx->exit_reason = A32_EXIT_FAULT;
            return A32_EXIT_FAULT;
        }
        ctx->regs[rd] = do_strex(ctx, va, 4U, ctx->regs[rm]);
        ctx->regs[15] = pc + 4U;
        return A32_EXIT_NONE;
    }
    if ((insn & 0x0FF000F0U) == 0x019000E0U || (insn & 0x0FF000F0U) == 0x018000E0U ||
        (insn & 0x0FF000F0U) == 0x019000C0U || (insn & 0x0FF000F0U) == 0x018000C0U) {
        bool is_load = ((insn >> 20) & 1U) != 0U;
        bool half = ((insn >> 5) & 1U) != 0U;
        unsigned sz = half ? 2U : 1U;
        unsigned rn = (insn >> 16) & 0xFU;
        unsigned rt = (insn >> 12) & 0xFU;
        uint32_t va = ctx->regs[rn];
        if ((va & (sz - 1U)) != 0U) {
            i_undef(ctx);
            return A32_EXIT_UNDEF;
        }
        if (!i_mem_ok(ctx, va, sz, is_load ? (uint32_t)PROT_READ : (uint32_t)PROT_WRITE)) {
            ctx->fault_addr = va;
            ctx->exit_reason = A32_EXIT_FAULT;
            return A32_EXIT_FAULT;
        }
        if (is_load) {
            uint32_t v = 0U;
            if (!do_ldrex(ctx, va, sz, &v)) {
                i_undef(ctx);
                return A32_EXIT_UNDEF;
            }
            ctx->regs[rt] = v;
        } else {
            unsigned rm = insn & 0xFU;
            ctx->regs[rt] = do_strex(ctx, va, sz, ctx->regs[rm]);
        }
        ctx->regs[15] = pc + 4U;
        return A32_EXIT_NONE;
    }
    if ((insn & 0x0E400090U) == 0x00000090U) {
        /* Extra load/store: LDRH/STRH/LDRSB/LDRSH/LDRD/STRD. */
        bool p = ((insn >> 24) & 1U) != 0U;
        bool u = ((insn >> 23) & 1U) != 0U;
        bool w = ((insn >> 21) & 1U) != 0U;
        bool l = ((insn >> 20) & 1U) != 0U;
        unsigned rn = (insn >> 16) & 0xFU;
        unsigned rt = (insn >> 12) & 0xFU;
        unsigned kind = ((insn >> 5) & 3U);
        uint32_t off = (((insn >> 8) & 0xFU) << 4) | (insn & 0xFU);
        uint32_t base = (rn == 15U) ? pc + 8U : ctx->regs[rn];
        uint32_t ea = u ? base + off : base - off;
        uint32_t wb = (!p) ? base : ea;
        uint32_t acc = p ? ea : base;
        if (kind == 3U && ((insn >> 20) & 1U) == 0U) {
            kind = 2U;
        }
        if (kind == 2U) {
            /* LDRD/STRD doubleword. */
            if (!i_mem_ok(ctx, acc & ~3U, 8U, l ? (uint32_t)PROT_READ : (uint32_t)PROT_WRITE)) {
                ctx->fault_addr = acc;
                ctx->exit_reason = A32_EXIT_FAULT;
                return A32_EXIT_FAULT;
            }
            if (l) {
                uint32_t lo = 0U;
                uint32_t hi = 0U;
                memcpy(&lo, a32_guest_ptr(ctx, acc), 4U);
                memcpy(&hi, a32_guest_ptr(ctx, acc + 4U), 4U);
                ctx->regs[rt] = lo;
                ctx->regs[rt + 1U] = hi;
            } else {
                uint32_t lo = ctx->regs[rt];
                uint32_t hi = ctx->regs[rt + 1U];
                memcpy(a32_guest_ptr(ctx, acc), &lo, 4U);
                memcpy(a32_guest_ptr(ctx, acc + 4U), &hi, 4U);
            }
        } else if (l) {
            uint32_t v = 0U;
            if (kind == 1U) {
                uint16_t h = 0U;
                if (!i_load_bytes(ctx, acc & ~1U, &h, 2U)) {
                    return A32_EXIT_FAULT;
                }
                v = h;
            } else if (kind == 3U) {
                int8_t b = 0;
                if (!i_load_bytes(ctx, acc, &b, 1U)) {
                    return A32_EXIT_FAULT;
                }
                v = (uint32_t)(int32_t)b;
            } else {
                int16_t h = 0;
                if (!i_load_bytes(ctx, acc & ~1U, &h, 2U)) {
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
            if (kind == 1U) {
                uint16_t h = (uint16_t)ctx->regs[rt];
                if (!i_store_bytes(ctx, acc & ~1U, &h, 2U)) {
                    return A32_EXIT_FAULT;
                }
            } else {
                uint8_t b = (uint8_t)ctx->regs[rt];
                if (!i_store_bytes(ctx, acc, &b, 1U)) {
                    return A32_EXIT_FAULT;
                }
            }
        }
        if (!p || w) {
            if (rn != 15U) {
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
    /* Single word/byte transfer. */
    if ((insn & 0x0C000000U) == 0x04000000U) {
        bool p = ((insn >> 24) & 1U) != 0U;
        bool u = ((insn >> 23) & 1U) != 0U;
        bool b = ((insn >> 22) & 1U) != 0U;
        bool w = ((insn >> 21) & 1U) != 0U;
        bool l = ((insn >> 20) & 1U) != 0U;
        unsigned rn = (insn >> 16) & 0xFU;
        unsigned rt = (insn >> 12) & 0xFU;
        uint32_t off;
        if (((insn >> 25) & 1U) != 0U) {
            bool shc = false;
            uint32_t rmv = reg_rd_pc_adjust(ctx, insn & 0xFU, pc);
            off = a32_shift_op(insn, rmv, 0U, false, &shc, ctx->cpsr);
        } else {
            off = insn & 0xFFFU;
        }
        uint32_t base = (rn == 15U) ? pc + 8U : ctx->regs[rn];
        uint32_t ea = u ? base + off : base - off;
        uint32_t wb = p ? ea : base + (u ? off : (uint32_t)(-(int32_t)off));
        uint32_t acc = p ? ea : base;
        if (l) {
            if (b) {
                uint8_t bv = 0U;
                if (!i_load_bytes(ctx, acc, &bv, 1U)) {
                    return A32_EXIT_FAULT;
                }
                if (rt == 15U) {
                    i_set_pc_branch(ctx, bv, false);
                    return A32_EXIT_NONE;
                }
                ctx->regs[rt] = bv;
            } else {
                uint32_t v = 0U;
                if (!i_load_u32(ctx, acc & ~3U, &v)) {
                    return A32_EXIT_FAULT;
                }
                uint32_t rot = (acc & 3U) * 8U;
                v = i_ror32(v, rot);
                if (rt == 15U) {
                    i_set_pc_branch(ctx, v, false);
                    return A32_EXIT_NONE;
                }
                ctx->regs[rt] = v;
            }
        } else {
            if (b) {
                uint8_t bv = (uint8_t)ctx->regs[rt];
                if (!i_store_bytes(ctx, acc, &bv, 1U)) {
                    return A32_EXIT_FAULT;
                }
            } else {
                if (!i_store_u32(ctx, acc & ~3U, ctx->regs[rt])) {
                    return A32_EXIT_FAULT;
                }
            }
        }
        if ((!p || w) && rn != 15U) {
            ctx->regs[rn] = wb;
        }
        ctx->regs[15] = pc + 4U;
        return A32_EXIT_NONE;
    }
    /* Data processing / misc. */
    if (((insn >> 25) & 7U) <= 1U) {
        return a32_dp_step(ctx, pc, insn);
    }
    ctx->regs[15] = pc + 4U;
    i_undef(ctx);
    return A32_EXIT_UNDEF;
}

