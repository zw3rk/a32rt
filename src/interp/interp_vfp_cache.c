/*
 * src/interp/interp_vfp_cache.c — VFP register-cache helpers (A32-R20-P0)
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "a64_emit.h"

static inline uint32_t cache_dmask(uint8_t kind, uint8_t reg)
{
    if (kind == A64_VFP_KIND_S) {
        return 1U << (reg >> 1);
    }
    if (kind == A64_VFP_KIND_D) {
        return 1U << reg;
    }
    return 3U << (reg << 1);
}

void a32_vfp_record_preheader(a64_buf_t *b, uint8_t kind, uint8_t reg, bool is_write)
{
    if (b->vfp_preheader_ineligible || b->self_loop_vfp_active) {
        return;
    }
    for (unsigned i = 0U; i < b->vfp_preheader_count; i++) {
        if (b->vfp_preheader_kinds[i] == kind && b->vfp_preheader_regs[i] == reg) {
            if (is_write) {
                b->vfp_preheader_written |= (1U << i);
            }
            return;
        }
    }
    uint32_t nmask = cache_dmask(kind, reg);
    unsigned hit[24];
    unsigned nhit = 0U;
    uint32_t union_mask = nmask;
    bool union_written = is_write;
    for (unsigned i = 0U; i < b->vfp_preheader_count; i++) {
        uint32_t m = cache_dmask(b->vfp_preheader_kinds[i], b->vfp_preheader_regs[i]);
        if ((m & nmask) != 0U) {
            hit[nhit++] = i;
            union_mask |= m;
            if ((b->vfp_preheader_written & (1U << i)) != 0U) {
                union_written = true;
            }
        }
    }
    if (nhit == 0U) {
        if (b->vfp_preheader_count >= 22U) {
            return;
        }
        unsigned idx = b->vfp_preheader_count++;
        b->vfp_preheader_kinds[idx] = kind;
        b->vfp_preheader_regs[idx] = reg;
        if (is_write) {
            b->vfp_preheader_written |= (1U << idx);
        }
        return;
    }
    uint8_t mkind;
    uint8_t mreg;
    if ((union_mask & (union_mask - 1U)) == 0U) {
        mkind = A64_VFP_KIND_D;
        mreg = (uint8_t)__builtin_ctz(union_mask);
    } else {
        unsigned lo = (unsigned)__builtin_ctz(union_mask);
        lo &= ~1U;
        mkind = A64_VFP_KIND_Q;
        mreg = (uint8_t)(lo >> 1);
    }
    uint32_t kept = 0U;
    unsigned dst = 0U;
    for (unsigned i = 0U; i < b->vfp_preheader_count; i++) {
        bool drop = false;
        for (unsigned h = 0U; h < nhit; h++) {
            if (hit[h] == i) {
                drop = true;
                break;
            }
        }
        if (!drop) {
            b->vfp_preheader_kinds[dst] = b->vfp_preheader_kinds[i];
            b->vfp_preheader_regs[dst] = b->vfp_preheader_regs[i];
            if ((b->vfp_preheader_written & (1U << i)) != 0U) {
                kept |= (1U << dst);
            }
            dst++;
        }
    }
    b->vfp_preheader_count = dst;
    b->vfp_preheader_written = kept;
    if (b->vfp_preheader_count >= 22U) {
        return;
    }
    unsigned idx = b->vfp_preheader_count++;
    b->vfp_preheader_kinds[idx] = mkind;
    b->vfp_preheader_regs[idx] = mreg;
    if (union_written) {
        b->vfp_preheader_written |= (1U << idx);
    }
}

static inline bool cache_alias(uint8_t k1, uint8_t r1, uint8_t k2, uint8_t r2)
{
    return (k1 <= k2) ? ((r1 >> (k2 - k1)) == r2) : ((r2 >> (k1 - k2)) == r1);
}

static void cache_raw_ldr(a64_buf_t *b, uint8_t kind, unsigned vt, unsigned idx)
{
    static const uint32_t opc[4] = { 0U, 0xBD400000U, 0xFD400000U, 0x3DC00000U };
    unsigned shift = (unsigned)kind + 1U;
    uint32_t off = A32_CTX_VFP_OFFSET + (idx << shift);
    a64_emit(b, opc[kind] | (((off >> shift) & 0xFFFU) << 10) | (A64_XCTX << 5) | (vt & 31U));
}

static void cache_raw_str(a64_buf_t *b, uint8_t kind, unsigned vt, unsigned idx)
{
    static const uint32_t opc[4] = { 0U, 0xBD000000U, 0xFD000000U, 0x3D800000U };
    unsigned shift = (unsigned)kind + 1U;
    uint32_t off = A32_CTX_VFP_OFFSET + (idx << shift);
    a64_emit(b, opc[kind] | (((off >> shift) & 0xFFFU) << 10) | (A64_XCTX << 5) | (vt & 31U));
}

static unsigned cache_alloc(a64_buf_t *b, uint8_t kind, uint8_t reg)
{
    for (unsigned i = 0U; i < 22U; i++) {
        if (b->vfp_slot_kind[i] == A64_VFP_KIND_NONE) {
            b->vfp_slot_kind[i] = kind;
            b->vfp_slot_reg[i] = reg;
            return i;
        }
    }
    for (unsigned s = 0U; s < 22U; s++) {
        unsigned idx = (b->vfp_next_evict + s) % 22U;
        if ((b->vfp_slot_dirty & (1U << idx)) == 0U) {
            b->vfp_next_evict = (uint8_t)((idx + 1U) % 22U);
            b->vfp_slot_kind[idx] = kind;
            b->vfp_slot_reg[idx] = reg;
            return idx;
        }
    }
    unsigned idx = b->vfp_next_evict % 22U;
    b->vfp_next_evict = (uint8_t)((idx + 1U) % 22U);
    if ((b->vfp_slot_dirty & (1U << idx)) != 0U) {
        cache_raw_str(b, b->vfp_slot_kind[idx], 8U + idx, b->vfp_slot_reg[idx]);
        b->vfp_slot_dirty &= ~(1U << idx);
    }
    b->vfp_slot_kind[idx] = kind;
    b->vfp_slot_reg[idx] = reg;
    return idx;
}

static void cache_flush_alias(a64_buf_t *b, uint8_t kind, uint8_t reg)
{
    for (unsigned i = 0U; i < A64_VFP_CACHE_SLOTS; i++) {
        if (b->vfp_slot_kind[i] != A64_VFP_KIND_NONE && b->vfp_slot_kind[i] != kind &&
            cache_alias(b->vfp_slot_kind[i], b->vfp_slot_reg[i], kind, reg)) {
            if ((b->vfp_slot_dirty & (1U << i)) != 0U) {
                cache_raw_str(b, b->vfp_slot_kind[i], 8U + i, b->vfp_slot_reg[i]);
                b->vfp_slot_dirty &= ~(1U << i);
            }
            b->vfp_slot_kind[i] = A64_VFP_KIND_NONE;
        }
    }
}

static unsigned cache_src(a64_buf_t *b, unsigned scratch, uint8_t kind, unsigned idx)
{
    int s = a64_vfp_find_exact_slot(b, kind, (uint8_t)idx);
    if (s >= 0) {
        return 8U + (unsigned)s;
    }
    if (kind == A64_VFP_KIND_S) {
        a64_emit_ldr_s_ctx(b, scratch, idx);
    } else if (kind == A64_VFP_KIND_D) {
        a64_emit_ldr_d_ctx(b, scratch, idx);
    } else {
        a64_emit_ldr_q_ctx(b, scratch, idx);
    }
    return scratch;
}

static unsigned cache_dst(a64_buf_t *b, unsigned scratch, uint8_t kind, unsigned idx, bool load_old)
{
    if (b->cond_branch_depth != 0U) {
        if (load_old) {
            if (kind == A64_VFP_KIND_S) {
                a64_emit_ldr_s_ctx(b, scratch, idx);
            } else if (kind == A64_VFP_KIND_D) {
                a64_emit_ldr_d_ctx(b, scratch, idx);
            } else {
                a64_emit_ldr_q_ctx(b, scratch, idx);
            }
        }
        return scratch;
    }
    int s = a64_vfp_find_exact_slot(b, kind, (uint8_t)idx);
    if (s >= 0) {
        b->vfp_slot_dirty |= (1U << (unsigned)s);
        return 8U + (unsigned)s;
    }
    if (b->self_loop_vfp_active) {
        if (load_old) {
            if (kind == A64_VFP_KIND_S) {
                a64_emit_ldr_s_ctx(b, scratch, idx);
            } else if (kind == A64_VFP_KIND_D) {
                a64_emit_ldr_d_ctx(b, scratch, idx);
            } else {
                a64_emit_ldr_q_ctx(b, scratch, idx);
            }
        }
        return scratch;
    }
    cache_flush_alias(b, kind, (uint8_t)idx);
    unsigned slot = cache_alloc(b, kind, (uint8_t)idx);
    if (load_old) {
        cache_raw_ldr(b, kind, 8U + slot, idx);
    }
    b->vfp_slot_dirty |= (1U << slot);
    return 8U + slot;
}

static void cache_commit(a64_buf_t *b, unsigned host, uint8_t kind, unsigned idx)
{
    if (host >= 8U) {
        if (b->cond_branch_depth > 0U) {
            unsigned slot = host - 8U;
            if (slot < A64_VFP_CACHE_SLOTS && b->vfp_slot_kind[slot] == kind &&
                b->vfp_slot_reg[slot] == (uint8_t)idx) {
                cache_raw_str(b, kind, host, idx);
            } else if (kind == A64_VFP_KIND_S) {
                a64_emit_str_s_ctx(b, host, idx);
            } else if (kind == A64_VFP_KIND_D) {
                a64_emit_str_d_ctx(b, host, idx);
            } else {
                a64_emit_str_q_ctx(b, host, idx);
            }
        }
        return;
    }
    if (kind == A64_VFP_KIND_S) {
        a64_emit_str_s_ctx(b, host, idx);
    } else if (kind == A64_VFP_KIND_D) {
        a64_emit_str_d_ctx(b, host, idx);
    } else {
        a64_emit_str_q_ctx(b, host, idx);
    }
}

unsigned a64_vfp_src_s(a64_buf_t *b, unsigned scratch_vt, unsigned s_idx)
{
    return cache_src(b, scratch_vt, A64_VFP_KIND_S, s_idx);
}

unsigned a64_vfp_dst_s(a64_buf_t *b, unsigned scratch_vt, unsigned s_idx)
{
    return cache_dst(b, scratch_vt, A64_VFP_KIND_S, s_idx, false);
}

unsigned a64_vfp_inout_s(a64_buf_t *b, unsigned scratch_vt, unsigned s_idx)
{
    return cache_dst(b, scratch_vt, A64_VFP_KIND_S, s_idx, true);
}

void a64_vfp_commit_s(a64_buf_t *b, unsigned host_vt, unsigned s_idx)
{
    cache_commit(b, host_vt, A64_VFP_KIND_S, s_idx);
}

unsigned a64_vfp_src_d(a64_buf_t *b, unsigned scratch_vt, unsigned d_idx)
{
    return cache_src(b, scratch_vt, A64_VFP_KIND_D, d_idx);
}

unsigned a64_vfp_dst_d(a64_buf_t *b, unsigned scratch_vt, unsigned d_idx)
{
    return cache_dst(b, scratch_vt, A64_VFP_KIND_D, d_idx, false);
}

unsigned a64_vfp_inout_d(a64_buf_t *b, unsigned scratch_vt, unsigned d_idx)
{
    return cache_dst(b, scratch_vt, A64_VFP_KIND_D, d_idx, true);
}

void a64_vfp_commit_d(a64_buf_t *b, unsigned host_vt, unsigned d_idx)
{
    cache_commit(b, host_vt, A64_VFP_KIND_D, d_idx);
}

unsigned a64_vfp_src_q(a64_buf_t *b, unsigned scratch_vt, unsigned q_idx)
{
    return cache_src(b, scratch_vt, A64_VFP_KIND_Q, q_idx);
}

unsigned a64_vfp_dst_q(a64_buf_t *b, unsigned scratch_vt, unsigned q_idx)
{
    return cache_dst(b, scratch_vt, A64_VFP_KIND_Q, q_idx, false);
}

unsigned a64_vfp_inout_q(a64_buf_t *b, unsigned scratch_vt, unsigned q_idx)
{
    return cache_dst(b, scratch_vt, A64_VFP_KIND_Q, q_idx, true);
}

void a64_vfp_commit_q(a64_buf_t *b, unsigned host_vt, unsigned q_idx)
{
    cache_commit(b, host_vt, A64_VFP_KIND_Q, q_idx);
}

static _Thread_local uint32_t g_last_pc;
static _Thread_local uint16_t g_last_n_neon;
static _Thread_local uint16_t g_last_n_scalar;
static _Thread_local bool g_last_saw_neon;
static _Thread_local bool g_last_saw_scalar;
static _Thread_local bool g_last_saw_vmsr;

static void reset_last_if_new_pc(const a64_buf_t *b)
{
    if (b->guest_start_pc != g_last_pc) {
        g_last_pc = b->guest_start_pc;
        g_last_n_neon = 0U;
        g_last_n_scalar = 0U;
        g_last_saw_neon = false;
        g_last_saw_scalar = false;
        g_last_saw_vmsr = false;
    }
}

void a64_vfp_note_neon_fp(a64_buf_t *b)
{
    reset_last_if_new_pc(b);
    if (b->n_neon_fp < 0xFFFFU) {
        b->n_neon_fp++;
    }
    if (g_last_n_neon < 0xFFFFU) {
        g_last_n_neon++;
    }
    g_last_saw_neon = true;
}

void a64_vfp_note_scalar_fp(a64_buf_t *b)
{
    reset_last_if_new_pc(b);
    if (b->n_scalar_fp < 0xFFFFU) {
        b->n_scalar_fp++;
    }
    if (g_last_n_scalar < 0xFFFFU) {
        g_last_n_scalar++;
    }
    g_last_saw_scalar = true;
}

void a64_vfp_note_vmsr(a64_buf_t *b)
{
    reset_last_if_new_pc(b);
    g_last_saw_vmsr = true;
    b->vfp_preheader_ineligible = true;
    b->fpscr_zero_spec = false;
}

void a64_vfp_preheader_fpcr_check(a64_buf_t *b)
{
    bool saw_neon = b->saw_neon_fp;
    bool saw_scalar = b->saw_scalar_vfp;
    bool saw_vmsr = false;
    if (b->guest_start_pc == g_last_pc) {
        saw_neon = g_last_saw_neon || b->saw_neon_fp;
        saw_scalar = g_last_saw_scalar || b->saw_scalar_vfp;
        saw_vmsr = g_last_saw_vmsr;
    }
    if (!(saw_neon && saw_scalar) || saw_vmsr) {
        return;
    }
    if (b->ctx == NULL || ((b->ctx->fpscr & 0x07C00000U) != 0U) ||
        ((b->ctx->cpsr & 0x0600FC00U) != 0U)) {
        return;
    }
    a64_emit_ldr_imm_w(b, 23U, A64_XCTX, (uint32_t)__builtin_offsetof(a32_ctx_t, fpscr));
    a64_emit_mov_imm32(b, A64_TMP2, 0x07C00000U);
    a64_emit_and_reg_w(b, false, 23U, 23U, A64_TMP2, A64_SHIFT_LSL, 0U);
    size_t cbz = a64_emit_cbz_w_placeholder(b, false, 23U);
    a64_emit_mov_pc32(b, A64_TMP0, b->guest_start_pc);
    a64_emit_movz_w(b, A64_TMP1, A32_EXIT_UNDEF, 0U);
    a64_emit_ret(b);
    a64_patch_cbz_w(b, cbz, b->count);
    /* Mixed NEON+scalar self-loop with (fpscr & 0x07C00000) == 0: keep host
     * FPCR == 0 (RN, FZ=0, DN=0) across the whole loop with zero MSR
     * instructions. Scalar VFP ops observe FPCR == 0 directly; NEON
     * VADD/VSUB/VABD/VMUL use the range-guarded helper and VCVT.F32.S32/U32
     * run directly, both bit-identical to Standard FPSCR on the fast path. */
    b->fpscr_zero_spec = true;
}

void a64_vfp_emit_neon_f32_guarded(a64_buf_t *b, bool is_q, uint32_t a64_op,
                                   unsigned rd, unsigned r_n, unsigned r_m)
{
    uint32_t shl_base = is_q ? 0x4F215400U : 0x0F215400U;
    uint32_t cmeq_base = is_q ? 0x4EA09800U : 0x0EA09800U;
    uint32_t sub_base = is_q ? 0x6EA08400U : 0x2EA08400U;
    uint32_t cmhi_base = is_q ? 0x6EA03400U : 0x2EA03400U;
    uint32_t bic_base = is_q ? 0x4E601C00U : 0x0E601C00U;
    uint32_t orr_base = is_q ? 0x4EA01C00U : 0x0EA01C00U;
    a64_emit(b, is_q ? 0x4F026446U : 0x0F026446U);
    a64_emit(b, is_q ? 0x4F036747U : 0x0F036747U);
    a64_emit(b, shl_base | ((r_n & 31U) << 5) | 3U);
    a64_emit(b, cmeq_base | (3U << 5) | 4U);
    a64_emit(b, sub_base | (6U << 16) | (3U << 5) | 3U);
    a64_emit(b, cmhi_base | (7U << 16) | (3U << 5) | 3U);
    a64_emit(b, bic_base | (4U << 16) | (3U << 5) | 3U);
    if (r_m != r_n) {
        a64_emit(b, shl_base | ((r_m & 31U) << 5) | 5U);
        a64_emit(b, cmeq_base | (5U << 5) | 4U);
        a64_emit(b, sub_base | (6U << 16) | (5U << 5) | 5U);
        a64_emit(b, cmhi_base | (7U << 16) | (5U << 5) | 5U);
        a64_emit(b, bic_base | (4U << 16) | (5U << 5) | 5U);
        a64_emit(b, orr_base | ((5U) << 16) | (3U << 5) | 3U);
    }
    if (is_q) {
        a64_emit(b, 0x6EA3A463U); /* umaxp v3.4s, v3.4s, v3.4s */
    }
    a64_emit(b, 0x9E660075U);
    size_t cbnz_idx = b->count;
    a64_emit(b, 0xB5000015U);
    a64_emit(b, a64_op | ((r_m & 31U) << 16) | ((r_n & 31U) << 5) | (rd & 31U));
    size_t b_idx = b->count;
    a64_emit(b, 0x14000000U);
    size_t slow_idx = b->count;
    b->words[cbnz_idx] = 0xB5000015U | ((((uint32_t)slow_idx - (uint32_t)cbnz_idx) & 0x7FFFFU) << 5);
    a64_emit(b, 0x52A06011U);
    a64_emit(b, 0xD51B4411U);
    a64_emit(b, a64_op | ((r_m & 31U) << 16) | ((r_n & 31U) << 5) | (rd & 31U));
    a64_emit(b, 0xD2800011U);
    a64_emit(b, 0xD51B4411U);
    if (b->stats_enabled) {
        a64_emit_mov_imm32(b, 17U, (uint32_t)__builtin_offsetof(a32_ctx_t, fpcr_guard_slow_hits));
        a64_emit(b, 0x8B110271U);
        a64_emit(b, 0xF9400237U);
        a64_emit(b, 0x910006F7U);
        a64_emit(b, 0xF9000237U);
    }
    size_t done_idx = b->count;
    b->words[b_idx] = 0x14000000U | (((uint32_t)done_idx - (uint32_t)b_idx) & 0x3FFFFFFU);
}
