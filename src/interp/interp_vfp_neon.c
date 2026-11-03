/*
 * src/interp/interp_vfp_neon.c — NEON reference subset for the interpreter
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Split from interp_vfp.c (line-limit split): VLDn/VSTn, VMOV immediate,
 * VREV and the integer VADD/VSUB/VALU/VMOV subset. Called from
 * a32_interp_vfp_step for cond==0xF words.
 */

#include "interp.h"
#include "interp_priv.h"

#include <stdint.h>

bool i_vfp_neon(a32_ctx_t *ctx, uint32_t pc, uint32_t insn, bool *faulted)
{
    if ((insn & 0xFF900000U) == 0xF4000000U) {
        bool l = ((insn >> 21) & 1U) != 0U;
        unsigned rn = (insn >> 16) & 0xFU;
        unsigned d_start = (((insn >> 22) & 1U) << 4) | ((insn >> 12) & 0xFU);
        unsigned type = (insn >> 8) & 0xFU;
        unsigned rm = insn & 0xFU;
        unsigned nregs = (type == 0x7U) ? 1U : (type == 0xAU) ? 2U :
                         (type == 0x6U) ? 3U : (type == 0x2U) ? 4U : 0U;
        if (nregs == 0U) {
            return false;
        }
        uint32_t addr = ctx->regs[rn];
        for (unsigned r = 0U; r < nregs; r++) {
            unsigned d = (d_start + r) & 31U;
            if (l) {
                if (!mem_r(ctx, addr + r * 8U, &ctx->vfp_d[d], 8U)) {
                    *faulted = true;
                    return true;
                }
            } else {
                if (!mem_w(ctx, addr + r * 8U, &ctx->vfp_d[d], 8U)) {
                    *faulted = true;
                    return true;
                }
            }
        }
        if (rm != 0xFU) {
            ctx->regs[rn] = addr + ((rm == 0xDU) ? (nregs * 8U) : ctx->regs[rm]);
        }
        ctx->regs[15] = pc + 4U;
        return true;
    }
    if ((insn & 0xFEB80090U) == 0xF2800010U) {
        uint32_t imm8 = (((insn >> 24) & 1U) << 7) | (((insn >> 16) & 7U) << 4) | (insn & 0xFU);
        unsigned cmode = (insn >> 8) & 0xFU;
        unsigned op = (insn >> 5) & 1U;
        bool is_q = ((insn >> 6) & 1U) != 0U;
        unsigned dd = (((insn >> 22) & 1U) << 4) | ((insn >> 12) & 0xFU);
        uint64_t val64 = 0ULL;
        if ((cmode & 1U) == 0U && cmode < 8U) {
            uint32_t w = imm8 << ((cmode >> 1) * 8U);
            if (op != 0U) {
                w = ~w;
            }
            val64 = ((uint64_t)w << 32) | w;
        } else if (cmode == 0xEU && op == 0U) {
            for (unsigned b = 0U; b < 8U; b++) {
                val64 |= (uint64_t)imm8 << (b * 8U);
            }
        } else if (cmode == 0xEU && op == 1U) {
            for (unsigned b = 0U; b < 8U; b++) {
                if (((imm8 >> b) & 1U) != 0U) {
                    val64 |= 0xFFULL << (b * 8U);
                }
            }
        }
        ctx->vfp_d[dd & 31U] = val64;
        if (is_q) {
            ctx->vfp_d[(dd + 1U) & 31U] = val64;
        }
        ctx->regs[15] = pc + 4U;
        return true;
    }
    if ((insn & 0xFFB30FD0U) == 0xF3B00000U) {
        unsigned size = (insn >> 18) & 3U;
        bool is_q = ((insn >> 6) & 1U) != 0U;
        unsigned dd = (((insn >> 22) & 1U) << 4) | ((insn >> 12) & 0xFU);
        unsigned dm = (((insn >> 5) & 1U) << 4) | (insn & 0xFU);
        unsigned regs = is_q ? 2U : 1U;
        for (unsigned r = 0U; r < regs; r++) {
            uint64_t v = ctx->vfp_d[(dm + r) & 31U];
            if (size == 0U) {
                v = __builtin_bswap64(v);
            } else if (size == 1U) {
                v = ((uint64_t)__builtin_bswap16((uint16_t)(v >> 48))) |
                    ((uint64_t)__builtin_bswap16((uint16_t)(v >> 32)) << 16) |
                    ((uint64_t)__builtin_bswap16((uint16_t)(v >> 16)) << 32) |
                    ((uint64_t)__builtin_bswap16((uint16_t)v) << 48);
            } else if (size == 2U) {
                v = (v >> 32) | (v << 32);
            }
            ctx->vfp_d[(dd + r) & 31U] = v;
        }
        ctx->regs[15] = pc + 4U;
        return true;
    }
    if ((insn & 0xFE800F10U) == 0xF2000800U) {
        bool is_sub = ((insn >> 24) & 1U) != 0U;
        unsigned size = (insn >> 20) & 3U;
        bool is_q = ((insn >> 6) & 1U) != 0U;
        unsigned dd = (((insn >> 22) & 1U) << 4) | ((insn >> 12) & 0xFU);
        unsigned dn = (((insn >> 7) & 1U) << 4) | ((insn >> 16) & 0xFU);
        unsigned dm = (((insn >> 5) & 1U) << 4) | (insn & 0xFU);
        unsigned regs = is_q ? 2U : 1U;
        for (unsigned r = 0U; r < regs; r++) {
            uint64_t a = ctx->vfp_d[(dn + r) & 31U];
            uint64_t b = ctx->vfp_d[(dm + r) & 31U];
            uint64_t out = 0ULL;
            if (size == 0U) {
                for (unsigned k = 0U; k < 8U; k++) {
                    uint8_t x = (uint8_t)(a >> (k * 8U));
                    uint8_t y = (uint8_t)(b >> (k * 8U));
                    uint8_t z = is_sub ? (uint8_t)(x - y) : (uint8_t)(x + y);
                    out |= (uint64_t)z << (k * 8U);
                }
            } else if (size == 1U) {
                for (unsigned k = 0U; k < 4U; k++) {
                    uint16_t x = (uint16_t)(a >> (k * 16U));
                    uint16_t y = (uint16_t)(b >> (k * 16U));
                    uint16_t z = is_sub ? (uint16_t)(x - y) : (uint16_t)(x + y);
                    out |= (uint64_t)z << (k * 16U);
                }
            } else if (size == 2U) {
                for (unsigned k = 0U; k < 2U; k++) {
                    uint32_t x = (uint32_t)(a >> (k * 32U));
                    uint32_t y = (uint32_t)(b >> (k * 32U));
                    uint32_t z = is_sub ? (x - y) : (x + y);
                    out |= (uint64_t)z << (k * 32U);
                }
            } else {
                out = is_sub ? (a - b) : (a + b);
            }
            ctx->vfp_d[(dd + r) & 31U] = out;
        }
        ctx->regs[15] = pc + 4U;
        return true;
    }
    if ((insn & 0xFE800F10U) == 0xF2000110U) {
        bool u = ((insn >> 24) & 1U) != 0U;
        unsigned op2 = (insn >> 20) & 3U;
        bool is_q = ((insn >> 6) & 1U) != 0U;
        unsigned dd = (((insn >> 22) & 1U) << 4) | ((insn >> 12) & 0xFU);
        unsigned dn = (((insn >> 7) & 1U) << 4) | ((insn >> 16) & 0xFU);
        unsigned dm = (((insn >> 5) & 1U) << 4) | (insn & 0xFU);
        unsigned regs = is_q ? 2U : 1U;
        for (unsigned r = 0U; r < regs; r++) {
            uint64_t a = ctx->vfp_d[(dn + r) & 31U];
            uint64_t b = ctx->vfp_d[(dm + r) & 31U];
            uint64_t d = ctx->vfp_d[(dd + r) & 31U];
            uint64_t out = 0ULL;
            if (!u) {
                out = (op2 == 0U) ? (a & b) : (op2 == 1U) ? (a & ~b) :
                      (op2 == 2U) ? (a | b) : (a | ~b);
            } else {
                out = (op2 == 0U) ? (a ^ b) : (op2 == 1U) ? ((a & d) | (b & ~d)) :
                      (op2 == 2U) ? ((d & ~b) | (a & b)) : ((d & b) | (a & ~b));
            }
            ctx->vfp_d[(dd + r) & 31U] = out;
        }
        ctx->regs[15] = pc + 4U;
        return true;
    }
    return false;
}
