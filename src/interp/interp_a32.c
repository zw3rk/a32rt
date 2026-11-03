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
 * addresses cannot reserve: the caller raises an alignment fault. */
static bool do_ldrex(a32_ctx_t *ctx, uint32_t va, unsigned size,
    uint32_t *out)
{
    void *hva;
    uint32_t v = 0U;
    if (ctx == NULL || out == NULL || (size != 1U && size != 2U && size != 4U) ||
        (va & (size - 1U)) != 0U) {
        return false;
    }
    hva = a32_vm32_hva(ctx, va);
    /* Seqlock around the load and the reservation snapshot: sample the
     * line epoch before and after the load and retry while they differ,
     * so no intervening participating write can slip between the load
     * and the snapshot (A32-R7-7). */
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

/* 64-bit LDREXD reservation (doubleword exclusives, A32-R7-3). */
static bool do_ldrex64(a32_ctx_t *ctx, uint32_t va, uint64_t *out)
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

static uint32_t do_strex(a32_ctx_t *ctx, uint32_t va, unsigned size,
    uint32_t val)
{
    bool have;
    if (ctx == NULL || (size != 1U && size != 2U && size != 4U)) {
        return 1U;
    }
    if ((va & (size - 1U)) != 0U) {
        return 1U;
    }
    have = ctx->excl_valid && ctx->excl_addr == va && ctx->excl_size == size &&
        a32_vm32_sc_attempt(ctx, va, size, val);
    ctx->excl_valid = false;
    return have ? 0U : 1U;
}

static uint32_t do_strex64(a32_ctx_t *ctx, uint32_t va, uint64_t val)
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
        *carry = (rm_val & 1U) != 0U;
        return (rm_val >> 1U) | (cin ? 0x80000000U : 0U);
    }
    if (rs_shift) {
        if (amt == 0U) {
            /* Register-controlled zero shift: result and carry pass
             * through unchanged (and no UB shift count executes). */
            *carry = cin;
            return rm_val;
        }
        if (amt >= 32U) {
            switch (st) {
            case 0U:
                *carry = (amt == 32U) ? ((rm_val & 1U) != 0U) : false;
                return 0U;
            case 1U:
                *carry = (amt == 32U) ? (((rm_val >> 31) & 1U) != 0U) : false;
                return 0U;
            case 2U:
                *carry = ((rm_val >> 31) & 1U) != 0U;
                return ((rm_val & 0x80000000U) != 0U) ? 0xFFFFFFFFU : 0U;
            default: {
                /* ROR by register rotates by amt mod 32. */
                unsigned r = amt & 31U;
                if (r == 0U) {
                    *carry = ((rm_val >> 31) & 1U) != 0U;
                    return rm_val;
                }
                *carry = ((rm_val >> (r - 1U)) & 1U) != 0U;
                return i_ror32(rm_val, r);
            }
            }
        }
    }
    switch (st) {
    case 0U: *carry = ((rm_val >> (32U - amt)) & 1U) != 0U; return rm_val << amt;
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
    } else if (((insn >> 4) & 1U) != 0U) {
        /* Bit4 set: register-shifted operand; multiply picked out inside. */
        if ((insn & 0x0E000090U) == 0x00000090U) {
            /* Multiply: MUL/MLA/MLS/UMAAL/UMULL/UMLAL/SMULL/SMLAL. */
            unsigned rn = insn & 0xFU;
            unsigned rm = (insn >> 8) & 0xFU;
            unsigned rd = (insn >> 16) & 0xFU;
            unsigned ra = (insn >> 12) & 0xFU;
            if ((insn & 0x0FF000F0U) == 0x00600090U) {
                ctx->regs[rd] = ctx->regs[ra] - (ctx->regs[rn] * ctx->regs[rm]);
                ctx->regs[15] = pc + 4U;
                return A32_EXIT_NONE;
            }
            if ((insn & 0x0FF000F0U) == 0x00400090U) {
                uint64_t p = (uint64_t)ctx->regs[rn] * (uint64_t)ctx->regs[rm] +
                    (uint64_t)ctx->regs[ra] + (uint64_t)ctx->regs[rd];
                ctx->regs[ra] = (uint32_t)p;
                ctx->regs[rd] = (uint32_t)(p >> 32);
                ctx->regs[15] = pc + 4U;
                return A32_EXIT_NONE;
            }
            bool lg = ((insn >> 23) & 1U) != 0U;
            bool sg = ((insn >> 22) & 1U) != 0U;
            bool a = ((insn >> 21) & 1U) != 0U;
            uint32_t nv = ctx->regs[rn];
            uint32_t mv = ctx->regs[rm];
            if (!lg) {
                uint32_t r = nv * mv;
                if (a) {
                    r += ctx->regs[ra];
                }
                ctx->regs[rd] = r;
                if (s) {
                    i_set_nz(&ctx->cpsr, r);
                }
                ctx->regs[15] = pc + 4U;
                return A32_EXIT_NONE;
            }
            if (!sg) {
                uint64_t p = (uint64_t)nv * (uint64_t)mv;
                if (a) {
                    p += ((uint64_t)ctx->regs[rd] << 32) | ctx->regs[ra];
                }
                ctx->regs[ra] = (uint32_t)p;
                ctx->regs[rd] = (uint32_t)(p >> 32);
            } else {
                /* Wrapping modular arithmetic in unsigned types: the
                 * accumulator addition cannot overflow in C (A32-R7-32). */
                int64_t prod = (int64_t)(int32_t)nv * (int64_t)(int32_t)mv;
                uint64_t acc = ((uint64_t)ctx->regs[rd] << 32) | ctx->regs[ra];
                uint64_t full = acc + (uint64_t)prod;
                if (a) {
                    ctx->regs[ra] = (uint32_t)full;
                    ctx->regs[rd] = (uint32_t)(full >> 32);
                } else {
                    ctx->regs[ra] = (uint32_t)(uint64_t)prod;
                    ctx->regs[rd] = (uint32_t)((uint64_t)prod >> 32);
                }
            }
            if (s) {
                uint64_t full = ((uint64_t)ctx->regs[rd] << 32) | ctx->regs[ra];
                if (full == 0ULL) {
                    ctx->cpsr |= A32_CPSR_Z;
                } else {
                    ctx->cpsr &= ~A32_CPSR_Z;
                }
                if ((ctx->regs[rd] & 0x80000000U) != 0U) {
                    ctx->cpsr |= A32_CPSR_N;
                } else {
                    ctx->cpsr &= ~A32_CPSR_N;
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
    if (is_test && !s) {
        i_undef(ctx);
        return A32_EXIT_UNDEF;
    }
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
        /* Logical operations (AND/EOR/TST/TEQ/ORR/MOV/BIC/MVN) preserve V
         * and update NZ plus the shifter carry; arithmetic classes produce
         * their own overflow (A32-R7-19). */
        if (opc == 0xDU || opc == 0xFU) {
            carry = sh_carry;
        }
        if (opc == 0x0U || opc == 0x1U || opc == 0x8U || opc == 0x9U || opc >= 0xCU) {
            ov = (ctx->cpsr & A32_CPSR_V) != 0U;
        }
        i_set_nzcv(&ctx->cpsr, res, carry, ov);
    }
    if (write) {
        if (rd == 15U) {
            i_set_pc_branch(ctx, res, !s);
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
            /* BLX immediate: SignExtend(imm24:H:'00') added to PC+8, then
             * an explicit Thumb interworking branch (A32-R7-15). The H bit
             * contributes bit 1 of the offset; the target's bit 0 selects
             * Thumb state. Shifts stay unsigned to avoid C UB (A32-R7-32). */
            int32_t imm24 = (int32_t)(insn & 0x00FFFFFFU);
            if ((imm24 & 0x00800000) != 0) {
                imm24 |= (int32_t)0xFF000000U;
            }
            uint32_t h_bit = ((insn >> 24) & 1U) << 1;
            uint32_t target = (pc + 8U) + ((uint32_t)imm24 << 2) + h_bit;
            ctx->regs[14] = pc + 4U;
            i_set_pc_branch(ctx, target | 1U, true);
            return A32_EXIT_NONE;
        }
        if (((insn & 0xFE000000U) == 0xF2000000U || (insn & 0xFF100000U) == 0xF4000000U) &&
            a32_interp_vfp_step(ctx, insn)) {
            return (ctx->exit_reason == A32_EXIT_FAULT) ? A32_EXIT_FAULT : A32_EXIT_NONE;
        }
        /* Unconditional hints (mask set mirrors the DBT contract in
         * interp_a32_ctl.c decode_a32_insn exactly): barriers and
         * CLREX/PLD are NOPs under the interpreter. */
        if ((insn & 0xFFF000F0U) == 0xF5700050U || (insn & 0xFFF000F0U) == 0xF5700040U ||
            (insn & 0xFFF000F0U) == 0xF5700060U || insn == 0xF57FF01FU ||
            (insn & 0xFD70F000U) == 0xF450F000U || (insn & 0xFD30F000U) == 0xF510F000U) {
            if (insn == 0xF57FF01FU) {
                ctx->excl_valid = false;
            }
            ctx->regs[15] = pc + 4U;
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
        /* BKPT reports its own instruction PC; the outer stepper does not
         * restore it (A32-R7-22). */
        ctx->regs[15] = pc;
        ctx->exit_reason = A32_EXIT_BKPT;
        return A32_EXIT_BKPT;
    }
    if (is_vfp_word(insn)) {
        if (a32_interp_vfp_step(ctx, insn)) {
            return ctx->exit_reason;
        }
        ctx->regs[15] = pc + 4U;
        i_undef(ctx);
        return A32_EXIT_UNDEF;
    }
    /* B / BL. */
    if ((insn & 0x0E000000U) == 0x0A000000U) {
        bool link = ((insn >> 24) & 1U) != 0U;
        int32_t off = (int32_t)((insn & 0x00FFFFFFU) << 8) >> 8;
        uint32_t target = (pc + 8U) + ((uint32_t)off << 2U);
        if (link) {
            ctx->regs[14] = pc + 4U;
        }
        i_set_pc_branch(ctx, target, false);
        return A32_EXIT_NONE;
    }
    /* BX / BLX register. A PC read observes the architectural pipeline
     * offset (A32-R7-18). */
    if ((insn & 0x0FFFFFF0U) == 0x012FFF10U) {
        uint32_t t = reg_rd_pc_adjust(ctx, insn & 0xFU, pc);
        i_set_pc_branch(ctx, t, true);
        return A32_EXIT_NONE;
    }
    if ((insn & 0x0FFFFFF0U) == 0x012FFF30U) {
        uint32_t t = reg_rd_pc_adjust(ctx, insn & 0xFU, pc);
        ctx->regs[14] = pc + 4U;
        i_set_pc_branch(ctx, t, true);
        return A32_EXIT_NONE;
    }
    /* MOVW / MOVT / NOP / CLZ / SMUL<x><y> / SMLA<x><y>. */
    if ((insn & 0x0FB00000U) == 0x03000000U) {
        bool is_movt = ((insn >> 22) & 1U) != 0U;
        unsigned rd = (insn >> 12) & 0xFU;
        uint32_t imm16 = ((insn >> 4) & 0xF000U) | (insn & 0xFFFU);
        if (is_movt) {
            ctx->regs[rd] = (ctx->regs[rd] & 0x0000FFFFU) | (imm16 << 16);
        } else {
            ctx->regs[rd] = imm16;
        }
        ctx->regs[15] = pc + 4U;
        return A32_EXIT_NONE;
    }
    if ((insn & 0x0FFFFFF0U) == 0x0320F000U) {
        ctx->regs[15] = pc + 4U;
        return A32_EXIT_NONE;
    }
    if ((insn & 0x0FFF0FF0U) == 0x016F0F10U) {
        uint32_t v = ctx->regs[insn & 0xFU];
        ctx->regs[(insn >> 12) & 0xFU] = (v == 0U) ? 32U : (uint32_t)__builtin_clz(v);
        ctx->regs[15] = pc + 4U;
        return A32_EXIT_NONE;
    }
    if ((insn & 0x0FF00090U) == 0x01600080U || (insn & 0x0FF00090U) == 0x01000080U) {
        bool is_mla = ((insn >> 21) & 3U) == 0U;
        unsigned rd = (insn >> 16) & 0xFU;
        unsigned ra = (insn >> 12) & 0xFU;
        unsigned rs = (insn >> 8) & 0xFU;
        unsigned rm = insn & 0xFU;
        bool m_hi = ((insn >> 5) & 1U) != 0U;
        bool s_hi = ((insn >> 6) & 1U) != 0U;
        int16_t lhs = m_hi ? (int16_t)(ctx->regs[rm] >> 16) : (int16_t)(ctx->regs[rm] & 0xFFFFU);
        int16_t rhs = s_hi ? (int16_t)(ctx->regs[rs] >> 16) : (int16_t)(ctx->regs[rs] & 0xFFFFU);
        int32_t prod = (int32_t)lhs * (int32_t)rhs;
        if (is_mla) {
            int64_t sum = (int64_t)prod + (int64_t)(int32_t)ctx->regs[ra];
            if (sum > 2147483647LL || sum < -2147483648LL) {
                ctx->cpsr |= A32_CPSR_Q;
            }
            prod = (int32_t)(uint32_t)sum;
        }
        ctx->regs[rd] = (uint32_t)prod;
        ctx->regs[15] = pc + 4U;
        return A32_EXIT_NONE;
    }
    /* MRS / MSR (CPSR subset: NZCVQ + GE only; mode bits ignored).
     * MRS masks out Rd so every destination register decodes (A32-R7-20).
     * MSR updates only the status fields selected by its field mask:
     * bit 19 selects NZCVQ, bit 18 selects GE (A32-R7-20). */
    if ((insn & 0x0FBF0FFFU) == 0x010F0000U) {
        unsigned rd = (insn >> 12) & 0xFU;
        if (rd != 15U) {
            ctx->regs[rd] = ctx->cpsr;
        }
        ctx->regs[15] = pc + 4U;
        return A32_EXIT_NONE;
    }
    /* The mask ignores bit 25, so this one clause matches both the
     * register (0x0120F000) and immediate (0x0320F000) MSR forms; the
     * operand source is selected by bit 25 below. */
    if ((insn & 0x0DB0FF00U) == 0x0120F000U) {
        unsigned field = (insn >> 16) & 0xFU;
        uint32_t v;
        uint32_t want = 0U;
        if (((insn >> 25) & 1U) != 0U) {
            v = i_ror32(insn & 0xFFU, ((insn >> 8) & 0xFU) * 2U);
        } else {
            v = ctx->regs[insn & 0xFU];
        }
        if ((field & 8U) != 0U) {
            want |= A32_CPSR_NZCV_MASK | A32_CPSR_Q;
        }
        if ((field & 4U) != 0U) {
            want |= A32_CPSR_GE_MASK;
        }
        ctx->cpsr = (ctx->cpsr & ~want) | (v & want);
        ctx->regs[15] = pc + 4U;
        return A32_EXIT_NONE;
    }
    /* LDREX / STREX family (word, doubleword, byte, halfword). One mask
     * mirrors the DBT contract in interp_a32_dp.c; ex_op selects size and
     * direction (A32-R7-3). Misalignment delivers a guest alignment fault,
     * never UNDEF (A32-R7-6). */
    if ((insn & 0x0F8000F0U) == 0x01800090U) {
        unsigned ex_op = (insn >> 20) & 7U;
        unsigned rn = (insn >> 16) & 0xFU;
        unsigned rt = (insn >> 12) & 0xFU;
        unsigned size = (ex_op < 2U) ? 4U : (ex_op < 4U) ? 8U : (ex_op < 6U) ? 1U : 2U;
        bool is_load = (ex_op & 1U) != 0U;
        uint32_t va = (rn == 15U) ? pc + 8U : ctx->regs[rn];
        unsigned align_mask = (size == 8U) ? 7U : (size - 1U);
        if ((va & align_mask) != 0U) {
            ctx->fault_addr = va;
            ctx->exit_reason = A32_EXIT_FAULT;
            return A32_EXIT_FAULT;
        }
        if (is_load && rt == 15U) {
            ctx->regs[15] = pc + 4U;
            i_undef(ctx);
            return A32_EXIT_UNDEF;
        }
        if (!is_load && rt == 15U) {
            ctx->regs[15] = pc + 4U;
            i_undef(ctx);
            return A32_EXIT_UNDEF;
        }
        if (!i_mem_ok(ctx, va, size, is_load ? (uint32_t)PROT_READ : (uint32_t)PROT_WRITE)) {
            ctx->fault_addr = va;
            ctx->exit_reason = A32_EXIT_FAULT;
            return A32_EXIT_FAULT;
        }
        if (size == 8U) {
            if (is_load) {
                unsigned rt2 = (rt + 1U) & 0xFU;
                uint64_t v = 0U;
                if (rt2 == 15U || !do_ldrex64(ctx, va, &v)) {
                    ctx->regs[15] = pc + 4U;
                    i_undef(ctx);
                    return A32_EXIT_UNDEF;
                }
                ctx->regs[rt] = (uint32_t)v;
                ctx->regs[rt2] = (uint32_t)(v >> 32);
            } else {
                unsigned rs = insn & 0xFU;
                uint64_t v = ((uint64_t)ctx->regs[(rs + 1U) & 0xFU] << 32) |
                    (uint64_t)ctx->regs[rs];
                ctx->regs[rt] = do_strex64(ctx, va, v);
            }
        } else if (is_load) {
            uint32_t v = 0U;
            if (!do_ldrex(ctx, va, size, &v)) {
                ctx->regs[15] = pc + 4U;
                i_undef(ctx);
                return A32_EXIT_UNDEF;
            }
            ctx->regs[rt] = v;
        } else {
            unsigned rm = insn & 0xFU;
            ctx->regs[rt] = do_strex(ctx, va, size, ctx->regs[rm]);
        }
        ctx->regs[15] = pc + 4U;
        return A32_EXIT_NONE;
    }
    /* SWP / SWPB: single-copy atomic swap. The loaded value is held in a
     * temporary and committed to Rd only after the store succeeds, so a
     * faulting swap never exposes a partial update (A32-R7-3, R7-9). */
    if ((insn & 0x0FB00FF0U) == 0x01000090U) {
        bool b_bit = ((insn >> 22) & 1U) != 0U;
        unsigned rn = (insn >> 16) & 0xFU;
        unsigned rd = (insn >> 12) & 0xFU;
        unsigned rm = insn & 0xFU;
        uint32_t va;
        unsigned size;
        void *hva;
        uint32_t tmp;
        if (rd == 15U || rn == 15U || rm == 15U) {
            ctx->regs[15] = pc + 4U;
            i_undef(ctx);
            return A32_EXIT_UNDEF;
        }
        va = ctx->regs[rn];
        size = b_bit ? 1U : 4U;
        if ((va & (size - 1U)) != 0U) {
            ctx->fault_addr = va;
            ctx->exit_reason = A32_EXIT_FAULT;
            return A32_EXIT_FAULT;
        }
        if (!i_mem_ok(ctx, va, size, (uint32_t)PROT_READ) ||
            !i_mem_ok(ctx, va, size, (uint32_t)PROT_WRITE)) {
            ctx->fault_addr = va;
            ctx->exit_reason = A32_EXIT_FAULT;
            return A32_EXIT_FAULT;
        }
        hva = a32_vm32_hva(ctx, va);
        if (b_bit) {
            tmp = __atomic_load_n((uint8_t *)hva, __ATOMIC_SEQ_CST);
            __atomic_store_n((uint8_t *)hva, (uint8_t)ctx->regs[rm], __ATOMIC_SEQ_CST);
        } else {
            tmp = __atomic_load_n((uint32_t *)(void *)hva, __ATOMIC_SEQ_CST);
            __atomic_store_n((uint32_t *)(void *)hva, ctx->regs[rm], __ATOMIC_SEQ_CST);
        }
        a32_vm32_copyout_invalidate(va, size);
        ctx->regs[rd] = tmp;
        ctx->regs[15] = pc + 4U;
        return A32_EXIT_NONE;
    }
    /* Extra load/store + LDM/STM live in interp_a32_mem.c (line-limit
     * split); dispatch order around the media block below is unchanged. */
    {
        bool claimed = false;
        uint32_t rc = i_a32_mem_step(ctx, pc, insn, &claimed);
        if (claimed) {
            return rc;
        }
    }
    /* Media instructions (op1 == 3, bit 4 == 1). */
    if ((insn & 0x0E000010U) == 0x06000010U) {
        if ((insn & 0x0FF0F0F0U) == 0x0710F010U) {
            int32_t num = (int32_t)ctx->regs[insn & 0xFU];
            int32_t den = (int32_t)ctx->regs[(insn >> 8) & 0xFU];
            int32_t q = (den == 0) ? 0 : ((num == (-2147483647 - 1) && den == -1) ? num : (num / den));
            ctx->regs[(insn >> 16) & 0xFU] = (uint32_t)q;
            ctx->regs[15] = pc + 4U;
            return A32_EXIT_NONE;
        }
        if ((insn & 0x0FF0F0F0U) == 0x0730F010U) {
            uint32_t num = ctx->regs[insn & 0xFU];
            uint32_t den = ctx->regs[(insn >> 8) & 0xFU];
            ctx->regs[(insn >> 16) & 0xFU] = (den == 0U) ? 0U : (num / den);
            ctx->regs[15] = pc + 4U;
            return A32_EXIT_NONE;
        }
        if ((insn & 0x0FE00070U) == 0x07E00050U || (insn & 0x0FE00070U) == 0x07A00050U) {
            bool is_signed = ((insn >> 22) & 1U) == 0U;
            unsigned width = ((insn >> 16) & 31U) + 1U;
            unsigned lsb = (insn >> 7) & 31U;
            unsigned rd = (insn >> 12) & 0xFU;
            uint32_t v = ctx->regs[insn & 0xFU];
            if (lsb + width <= 32U) {
                uint32_t sh = 32U - (lsb + width);
                if (is_signed) {
                    ctx->regs[rd] = (uint32_t)(((int32_t)(v << sh)) >> (32U - width));
                } else {
                    ctx->regs[rd] = (v << sh) >> (32U - width);
                }
                ctx->regs[15] = pc + 4U;
                return A32_EXIT_NONE;
            }
        }
        if ((insn & 0x0FE00070U) == 0x07C00010U) {
            unsigned msb = (insn >> 16) & 31U;
            unsigned lsb = (insn >> 7) & 31U;
            unsigned rd = (insn >> 12) & 0xFU;
            unsigned rn = insn & 0xFU;
            if (msb >= lsb) {
                unsigned width = msb - lsb + 1U;
                uint32_t mask = (width == 32U) ? 0xFFFFFFFFU : (((1U << width) - 1U) << lsb);
                uint32_t src = (rn == 15U) ? 0U : ((ctx->regs[rn] << lsb) & mask);
                ctx->regs[rd] = (ctx->regs[rd] & ~mask) | src;
                ctx->regs[15] = pc + 4U;
                return A32_EXIT_NONE;
            }
        }
        if ((insn & 0x0FFF0FF0U) == 0x06BF0F30U) {
            ctx->regs[(insn >> 12) & 0xFU] = __builtin_bswap32(ctx->regs[insn & 0xFU]);
            ctx->regs[15] = pc + 4U;
            return A32_EXIT_NONE;
        }
        if ((insn & 0x0FFF0FF0U) == 0x06BF0FB0U) {
            uint32_t v = ctx->regs[insn & 0xFU];
            ctx->regs[(insn >> 12) & 0xFU] = ((v >> 8) & 0x00FF00FFU) | ((v & 0x00FF00FFU) << 8);
            ctx->regs[15] = pc + 4U;
            return A32_EXIT_NONE;
        }
        if ((insn & 0x0FFF0FF0U) == 0x06FF0FB0U) {
            uint16_t v = (uint16_t)(ctx->regs[insn & 0xFU] & 0xFFFFU);
            int16_t s = (int16_t)((v >> 8) | (v << 8));
            ctx->regs[(insn >> 12) & 0xFU] = (uint32_t)(int32_t)s;
            ctx->regs[15] = pc + 4U;
            return A32_EXIT_NONE;
        }
        if ((insn & 0x0F8003F0U) == 0x06800070U) {
            bool is_signed = ((insn >> 22) & 1U) == 0U;
            unsigned kind = (insn >> 20) & 3U;
            unsigned rd = (insn >> 12) & 0xFU;
            unsigned rn = (insn >> 16) & 0xFU;
            uint32_t v = i_ror32(ctx->regs[insn & 0xFU], ((insn >> 10) & 3U) * 8U);
            uint32_t base = (rn == 15U) ? 0U : ctx->regs[rn];
            if (kind == 2U) {
                uint32_t ext = is_signed ? (uint32_t)(int32_t)(int8_t)(v & 0xFFU) : (v & 0xFFU);
                ctx->regs[rd] = base + ext;
                ctx->regs[15] = pc + 4U;
                return A32_EXIT_NONE;
            }
            if (kind == 3U) {
                uint32_t ext = is_signed ? (uint32_t)(int32_t)(int16_t)(v & 0xFFFFU) : (v & 0xFFFFU);
                ctx->regs[rd] = base + ext;
                ctx->regs[15] = pc + 4U;
                return A32_EXIT_NONE;
            }
        }
        ctx->regs[15] = pc + 4U;
        i_undef(ctx);
        return A32_EXIT_UNDEF;
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
        /* Unsigned wraparound arithmetic: valid modular guest addresses
         * must not invoke signed-negation UB (A32-R7-32). */
        uint32_t wb = p ? ea : (u ? base + off : base - off);
        uint32_t acc = p ? ea : base;
        if (l) {
            if (b) {
                uint8_t bv = 0U;
                if (!i_load_bytes(ctx, acc, &bv, 1U)) {
                    return A32_EXIT_FAULT;
                }
                if (rt == 15U) {
                    /* LoadWritePC: interworking branch first, then the
                     * permitted base writeback (A32-R7-16). */
                    if ((!p || w) && rn != 15U) {
                        ctx->regs[rn] = wb;
                    }
                    i_set_pc_branch(ctx, bv, true);
                    return A32_EXIT_NONE;
                }
                ctx->regs[rt] = bv;
            } else {
                uint32_t v = 0U;
                if (!i_load_u32(ctx, acc, &v)) {
                    return A32_EXIT_FAULT;
                }
                if (rt == 15U) {
                    if ((!p || w) && rn != 15U) {
                        ctx->regs[rn] = wb;
                    }
                    i_set_pc_branch(ctx, v, true);
                    return A32_EXIT_NONE;
                }
                ctx->regs[rt] = v;
            }
        } else {
            /* A PC store observes the pipeline offset (A32-R7-18). */
            uint32_t stv = (rt == 15U) ? pc + 8U : ctx->regs[rt];
            if (b) {
                uint8_t bv = (uint8_t)stv;
                if (!i_store_bytes(ctx, acc, &bv, 1U)) {
                    return A32_EXIT_FAULT;
                }
            } else {
                if (!i_store_u32(ctx, acc, stv)) {
                    return A32_EXIT_FAULT;
                }
            }
        }
        if ((!p || w) && rn != 15U && (!l || rn != rt)) {
            ctx->regs[rn] = wb;
        }
        ctx->regs[15] = pc + 4U;
        return A32_EXIT_NONE;
    }
    /* Data processing / misc. */
    if (((insn >> 25) & 7U) <= 1U) {
        return a32_dp_step(ctx, pc, insn);
    }
    /* MRC / MCR p15 (TPIDRURO/RW + barriers).  Mask set mirrors the
     * DBT contract in core_emit_shared.c a64_emit_cp15 exactly (the
     * 0x0FFF0FFF mask drops cond + Rt; Rt is decoded separately).
     * Conditional execution already passed at function entry. */
    {
        uint32_t masked = insn & 0x0FFF0FFFU;
        unsigned rt = (insn >> 12) & 0xFU;
        if (masked == 0x0E1D0F70U) {
            ctx->regs[rt] = ctx->tls_tp; /* TPIDRURO read (c13, c0, 3) */
            ctx->regs[15] = pc + 4U;
            return A32_EXIT_NONE;
        }
        if (masked == 0x0E1D0F50U) {
            ctx->regs[rt] = ctx->tls_tpidrurw; /* TPIDRURW read (c13, c0, 2) */
            ctx->regs[15] = pc + 4U;
            return A32_EXIT_NONE;
        }
        if (masked == 0x0E0D0F50U) {
            ctx->tls_tpidrurw = ctx->regs[rt]; /* TPIDRURW write (c13, c0, 2) */
            ctx->regs[15] = pc + 4U;
            return A32_EXIT_NONE;
        }
        if (masked == 0x0E070FBAU || masked == 0x0E070F9AU ||
            masked == 0x0E070F95U) {
            ctx->regs[15] = pc + 4U; /* DMB / DSB / ISB: nop */
            return A32_EXIT_NONE;
        }
    }
    ctx->regs[15] = pc + 4U;
    i_undef(ctx);
    return A32_EXIT_UNDEF;
}

