/*
 * src/interp/decode_priv.h — Internal declarations for A32/T32/VFP/NEON decoders
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef A32RT_DECODE_PRIV_H
#define A32RT_DECODE_PRIV_H

#include "a64_emit.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static inline uint32_t vfp_expand_imm_single(uint8_t imm8)
{
    uint32_t a = ((uint32_t)imm8 >> 7) & 1U;
    uint32_t b = ((uint32_t)imm8 >> 6) & 1U;
    uint32_t c = ((uint32_t)imm8 >> 5) & 1U;
    uint32_t d = ((uint32_t)imm8 >> 4) & 1U;
    uint32_t efgh = (uint32_t)imm8 & 0xFU;
    uint32_t exp = ((1U - b) << 7) | (b << 6) | (b << 5) | (b << 4) | (b << 3) |
                   (b << 2) | (c << 1) | d;
    uint32_t frac = efgh << 19;
    return (a << 31) | (exp << 23) | frac;
}

static inline unsigned vfp_reg_idx(bool is_double, bool hi_bit, unsigned lo4)
{
    return is_double ? ((hi_bit ? 16U : 0U) | lo4) : ((lo4 << 1) | (hi_bit ? 1U : 0U));
}

static inline unsigned neon_reg_d(uint32_t insn)
{
    return (((insn >> 22) & 1U) << 4) | ((insn >> 12) & 0xFU);
}

static inline unsigned neon_reg_n(uint32_t insn)
{
    return (((insn >> 7) & 1U) << 4) | ((insn >> 16) & 0xFU);
}

static inline unsigned neon_reg_m(uint32_t insn)
{
    return (((insn >> 5) & 1U) << 4) | (insn & 0xFU);
}

static inline void vfp_ldr_ctx(a64_buf_t *b, bool is_double, unsigned vt, unsigned idx)
{
    if (is_double) {
        a64_emit_ldr_d_ctx(b, vt, idx);
    } else {
        a64_emit_ldr_s_ctx(b, vt, idx);
    }
}

static inline void vfp_str_ctx(a64_buf_t *b, bool is_double, unsigned vt, unsigned idx)
{
    if (is_double) {
        a64_emit_str_d_ctx(b, vt, idx);
    } else {
        a64_emit_str_s_ctx(b, vt, idx);
    }
}

static inline void vfp_emit_unary(a64_buf_t *b, bool is_double, unsigned d_idx, unsigned m_idx, uint32_t op_s)
{
    vfp_ldr_ctx(b, is_double, 1U, m_idx);
    a64_emit(b, op_s | (is_double ? 0x00400000U : 0U) | (1U << 5) | 0U);
    vfp_str_ctx(b, is_double, 0U, d_idx);
}

static inline void neon_ldr_ctx(a64_buf_t *b, bool is_q, unsigned vt, unsigned d_idx)
{
    if (is_q) {
        a64_emit_ldr_q_ctx(b, vt, d_idx >> 1);
    } else {
        a64_emit_ldr_d_ctx(b, vt, d_idx);
    }
}

static inline void neon_str_ctx(a64_buf_t *b, bool is_q, unsigned vt, unsigned d_idx)
{
    if (is_q) {
        a64_emit_str_q_ctx(b, vt, d_idx >> 1);
    } else {
        a64_emit_str_d_ctx(b, vt, d_idx);
    }
}

static inline void neon_emit_unary(a64_buf_t *b, bool is_q, unsigned d, unsigned m,
                                   uint32_t base_op, bool needs_dst)
{
    if (needs_dst) {
        neon_ldr_ctx(b, is_q, 0U, d);
    }
    neon_ldr_ctx(b, is_q, 1U, m);
    a64_emit(b, base_op | (is_q ? 0x40000000U : 0U) | (1U << 5) | 0U);
    neon_str_ctx(b, is_q, 0U, d);
}

static inline void neon_emit_wb(a64_buf_t *b, unsigned rn, unsigned rm, uint16_t imm_bytes)
{
    if (rm != 0xFU && rn != 15U) {
        if (rm == 0xDU) {
            a64_emit_add_imm_w(b, false, rn, rn, imm_bytes, 0U);
        } else {
            a64_emit_add_reg_w(b, false, rn, rn, rm, A64_SHIFT_LSL, 0U);
        }
    }
}

unsigned a32_emit_shifted_reg(a64_buf_t *b, uint32_t insn, uint32_t pc_val,
                              unsigned out_reg, bool want_carry, unsigned *out_carry_reg);
#define emit_a32_shifted_reg a32_emit_shifted_reg

bool a32_decode_dp_op01(a32_ctx_t *ctx, a64_buf_t *b, uint32_t pc, uint32_t pc_val,
                        uint32_t insn, unsigned cond, unsigned op1, bool flags_live,
                        bool *out_ends_block);

bool a32_translate_t16_insn(a32_ctx_t *ctx, a64_buf_t *b, uint32_t pc, uint16_t insn,
                            bool in_it, uint32_t itstate, uint32_t next_itstate, bool is_uncond,
                            bool flags_live, bool *out_ends_block);
bool a32_translate_t32_insn(a32_ctx_t *ctx, a64_buf_t *b, uint32_t pc, uint16_t hw1,
                            uint16_t hw2, bool is_uncond, bool flags_live, bool *out_ends_block);
bool a32_translate_t32_mem_insn(a64_buf_t *b, uint16_t hw1, uint16_t hw2,
                                uint32_t pc_val, uint32_t pc_align4, bool is_uncond,
                                bool *out_ends_block);

bool a32_decode_vfp(a64_buf_t *b, uint32_t insn, uint32_t pc);
bool a32_decode_neon(a64_buf_t *b, uint32_t insn, uint32_t pc);
bool a32_decode_neon_alu(a64_buf_t *b, uint32_t insn);

#endif /* A32RT_DECODE_PRIV_H */
