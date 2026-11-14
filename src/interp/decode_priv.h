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

/* NEON modified-immediate expansion shared by the JIT (interp_neon_mem.c)
 * and the reference interpreter (interp_vfp_neon.c) so both agree on
 * every cmode (A32-R7-26). Mirrors AdvSIMDExpandImm: cmodes 0-7 are
 * 32-bit shifted by 8*cmode[2:1], 8-11 are 16-bit shifted replicated,
 * 12/13 are shifting ones (0x00imm8FF / 0x00imm8FFFF... as 32-bit), 14
 * is byte-replicate (op 0) or shifting ones (op 1), and 15/op 0 is an
 * F32 immediate. Returns false for 15/op 1 (UNDEFINED). */
static inline bool neon_expand_imm(uint8_t cmode, uint8_t op, uint8_t imm8, uint64_t *out_val64)
{
    uint32_t w = 0U;
    if ((cmode & 0xEU) == 0U) { /* 000x: 32-bit 000000imm8 */
        w = imm8;
        *out_val64 = ((uint64_t)w << 32) | w;
        return true;
    }
    if ((cmode & 0xEU) == 2U) { /* 001x: 32-bit 0000imm800 */
        w = (uint32_t)imm8 << 8;
        *out_val64 = ((uint64_t)w << 32) | w;
        return true;
    }
    if ((cmode & 0xEU) == 4U) { /* 010x: 32-bit 00imm80000 */
        w = (uint32_t)imm8 << 16;
        *out_val64 = ((uint64_t)w << 32) | w;
        return true;
    }
    if ((cmode & 0xEU) == 6U) { /* 011x: 32-bit imm8000000 */
        w = (uint32_t)imm8 << 24;
        *out_val64 = ((uint64_t)w << 32) | w;
        return true;
    }
    if ((cmode & 0xEU) == 8U) { /* 100x: 16-bit 00imm8 */
        uint32_t h = imm8;
        w = (h << 16) | h;
        *out_val64 = ((uint64_t)w << 32) | w;
        return true;
    }
    if ((cmode & 0xEU) == 10U) { /* 101x: 16-bit imm800 */
        uint32_t h = (uint32_t)imm8 << 8;
        w = (h << 16) | h;
        *out_val64 = ((uint64_t)w << 32) | w;
        return true;
    }
    if (cmode == 12U) { /* 1100: 32-bit 0000imm8FF */
        w = ((uint32_t)imm8 << 8) | 0xFFU;
        *out_val64 = ((uint64_t)w << 32) | w;
        return true;
    }
    if (cmode == 13U) { /* 1101: 32-bit 00imm8FFFF */
        w = ((uint32_t)imm8 << 16) | 0xFFFFU;
        *out_val64 = ((uint64_t)w << 32) | w;
        return true;
    }
    if (cmode == 14U) { /* 1110 */
        if (op == 0U) {
            uint32_t b = imm8;
            w = (b << 24) | (b << 16) | (b << 8) | b;
            *out_val64 = ((uint64_t)w << 32) | w;
            return true;
        }
        uint64_t val = 0ULL;
        for (unsigned i = 0U; i < 8U; i++) {
            if ((imm8 >> i) & 1U) {
                val |= (0xFFULL << (i * 8U));
            }
        }
        *out_val64 = val;
        return true;
    }
    if (cmode == 15U && op == 0U) { /* 1111: float imm */
        w = vfp_expand_imm_single(imm8);
        *out_val64 = ((uint64_t)w << 32) | w;
        return true;
    }
    return false;
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

/* NEON FP environment isolation (A32-R7-24, A32-IOS-P1): Advanced SIMD
 * arithmetic always uses the Standard FPSCR (RMode=RN, FZ=1, DN=1, AHP
 * from the guest FPSCR), never the scalar VFP rounding/flush controls
 * the trampoline installed in host FPCR on block entry. The install is
 * coalesced per block: the first NEON FP op at cond_branch_depth == 0
 * installs Standard FPSCR once (fpcr_neon_active = true) and later ops
 * emit nothing; the scalar restore is emitted only before scalar VFP
 * ops, conditional skip boundaries, and block/side exits. Inside a
 * conditional skip region the pair stays balanced per op without
 * touching the flag. Only NEON vector instructions sit between install
 * and restore, so TMP1/TMP2 are never clobbered (vector ops never write
 * general registers). */
static inline void neon_fp_fpcr_begin(a64_buf_t *b)
{
    b->saw_neon_fp = true;
    if (b->cond_branch_depth > 0U) {
        if (!b->fpcr_neon_active) {
            a64_emit_fpcr_install_neon(b);
        }
        return;
    }
    if (!b->fpcr_neon_active) {
        a64_emit_fpcr_install_neon(b);
        b->fpcr_neon_active = true;
    }
}

static inline void neon_fp_fpcr_end(a64_buf_t *b)
{
    if (b->cond_branch_depth > 0U && !b->fpcr_neon_active) {
        a64_emit_fpcr_restore_scalar(b);
    }
}

/* Scalar VFP data-processing observes the scalar FPSCR rounding/flush
 * controls, never the NEON Standard FPSCR (A32-IOS-P1). Call at entry to
 * scalar VFP data-processing (and after VMSR FPSCR): restore scalar
 * FPCR when NEON Standard FPSCR is active, clearing the flag outside
 * conditional skip regions (inside, the flag stays set so fallthrough
 * keeps Standard FPSCR; the pre-placeholder flush guarantees the flag
 * is clear on region entry unless hoisted for a pure-NEON self-loop). */
static inline void vfp_scalar_fpcr_sync(a64_buf_t *b)
{
    b->saw_scalar_vfp = true;
    if (b->fpcr_neon_active) {
        a64_emit_fpcr_restore_scalar(b);
        if (b->cond_branch_depth == 0U) {
            b->fpcr_neon_active = false;
        }
    }
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

/* Shared T32 single-transfer addressing-mode decode (A32-R13-10): the DBT
 * lowering (interp_t32_mem.c) and the reference interpreter
 * (interp_thumb.c) must agree on literal, imm12, shifted-register and
 * imm8 pre/post encodings, PLD/PLI hints, UNDEF forms, and rn == rt load
 * writeback suppression. Both sides decode through this helper and only
 * differ in actuation (emit vs execute). */
typedef struct {
    uint8_t  kind;  /* 0 literal, 1 imm12, 2 shifted-reg, 3 imm8 pre/post */
    bool     undef; /* encoding is UNDEF / unhandled */
    bool     hint;  /* PLD/PLI: valid encoding, no transfer */
    bool     up;    /* add (true) or subtract (false) the offset */
    bool     pre;   /* pre-indexed (true) or post-indexed (false) */
    bool     wb;    /* writeback applies (rn == rt suppression folded in) */
    uint16_t imm;   /* imm12 or imm8 */
    uint8_t  rm;
    uint8_t  shift;
} t32_ls_dec_t;

static inline t32_ls_dec_t t32_decode_ls(unsigned rn, unsigned rt,
    bool u_imm12, unsigned size, bool is_load, bool sign_ext, uint16_t hw2)
{
    t32_ls_dec_t d;
    d.kind = 0U;
    d.undef = false;
    d.hint = false;
    d.up = u_imm12;
    d.pre = true;
    d.wb = false;
    d.imm = 0U;
    d.rm = 0U;
    d.shift = 0U;
    if (size > 2U || (size == 2U && sign_ext)) {
        d.undef = true;
        return d;
    }
    if (!is_load && (sign_ext || rt == 15U || rn == 15U)) {
        d.undef = true;
        return d;
    }
    if (rn == 15U) {
        d.kind = 0U;
        d.imm = (uint16_t)(hw2 & 0xFFFU);
    } else if (u_imm12) {
        d.kind = 1U;
        d.imm = (uint16_t)(hw2 & 0xFFFU);
    } else if ((hw2 & 0x0800U) == 0U) {
        if ((hw2 & 0x0FC0U) != 0U) {
            d.undef = true;
            return d;
        }
        d.kind = 2U;
        d.rm = (uint8_t)(hw2 & 0xFU);
        d.shift = (uint8_t)((hw2 >> 4) & 3U);
    } else {
        bool p_bit = ((hw2 >> 10) & 1U) != 0U;
        bool w_bit = ((hw2 >> 8) & 1U) != 0U;
        if (!p_bit && !w_bit) {
            d.undef = true;
            return d;
        }
        if (rt == 15U && is_load && size != 2U && (hw2 & 0x0F00U) != 0x0C00U) {
            d.undef = true;
            return d;
        }
        d.kind = 3U;
        d.pre = p_bit;
        d.up = ((hw2 >> 9) & 1U) != 0U;
        d.imm = (uint16_t)(hw2 & 0xFFU);
        d.wb = (!p_bit || w_bit) && (rn != rt || !is_load);
    }
    if (rt == 15U && is_load && size != 2U) {
        d.hint = true;
    }
    return d;
}

#endif /* A32RT_DECODE_PRIV_H */
