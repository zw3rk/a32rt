/*
 * src/core/core_emit_mem.c — Direct 32-bit AArch64 memory, VFP & branch encoder
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "a64_emit.h"
#include "dbt/aot/aot.h"

extern uint32_t a32_jit_last_record_pc(void);
extern uint32_t a32_jit_last_record_itstate(void);

/* A2 Soft-TLB fault exits must preserve the pre-instruction ITSTATE so a
 * faulting LDR/STR inside a Thumb IT block retains it for signal handling
 * and retry, matching the UNDEF/BKPT/SVC exits. */
static void emit_soft_tlb_fault_itstate(a64_buf_t *b, uint32_t itstate) {
    if ((itstate & 0x0FU) != 0U) {
        a64_emit_mov_imm32(b, A64_TMP0, itstate);
        a64_emit_str_imm_w(b, A64_TMP0, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, itstate));
    }
}

/* ---- Out-of-line cold stubs (A32-R20-P4) ---- */
#define COLD_KIND_HOT_CBNZ 0U
#define COLD_KIND_COLD_B 1U

static void cold_emit_u32(a64_buf_t *b, uint32_t w) {
    if (b->cold_count < 512U) {
        b->cold_words[b->cold_count++] = w;
    } else { b->overflow = true; }
}

static bool cold_reserve(a64_buf_t *b, size_t need_words, size_t need_patches) {
    if (b->cold_count + need_words > 460U) { return false; }
    if (b->cold_patch_count + need_patches >= 124U) { return false; }
    return true;
}

static void cold_record(a64_buf_t *b, uint32_t src, uint32_t dst, uint8_t kind) {
    if (b->cold_patch_count >= 128U) {
        b->overflow = true;
        return;
    }
    b->cold_patch_src[b->cold_patch_count] = src;
    b->cold_patch_dst[b->cold_patch_count] = dst;
    b->cold_patch_kind[b->cold_patch_count] = kind;
    b->cold_patch_count++;
}

static void cold_movz_w(a64_buf_t *b, unsigned rd, uint16_t imm16, unsigned hw)
{ cold_emit_u32(b, 0x52800000U | ((hw & 1U) << 21) | ((uint32_t)imm16 << 5) | (rd & 31U)); }
static void cold_movk_w(a64_buf_t *b, unsigned rd, uint16_t imm16, unsigned hw)
{ cold_emit_u32(b, 0x72800000U | ((hw & 1U) << 21) | ((uint32_t)imm16 << 5) | (rd & 31U)); }

static void cold_mov_imm32(a64_buf_t *b, unsigned rd, uint32_t val) {
    uint16_t lo = (uint16_t)(val & 0xFFFFU);
    uint16_t hi = (uint16_t)(val >> 16);
    if (lo == 0U && hi != 0U) {
        cold_movz_w(b, rd, hi, 1U);
        return;
    }
    cold_movz_w(b, rd, lo, 0U);
    if (hi != 0U) { cold_movk_w(b, rd, hi, 1U); }
}

static void cold_mov_pc32(a64_buf_t *b, unsigned rd, uint32_t pc_val) {
    cold_movz_w(b, rd, (uint16_t)(pc_val >> 16U), 1U);
    cold_movk_w(b, rd, (uint16_t)(pc_val & 0xFFFFU), 0U);
}

static void cold_str_imm_w(a64_buf_t *b, unsigned rt, unsigned rn_x, uint32_t off) {
    uint32_t uimm12 = (off >> 2) & 0xFFFU;
    cold_emit_u32(b, 0xB9000000U | (uimm12 << 10) | ((rn_x & 31U) << 5) | (rt & 31U));
}

static void cold_ldr_imm_w(a64_buf_t *b, unsigned rt, unsigned rn_x, uint32_t off) {
    uint32_t uimm12 = (off >> 2) & 0xFFFU;
    cold_emit_u32(b, 0xB9400000U | (uimm12 << 10) | ((rn_x & 31U) << 5) | (rt & 31U));
}

static void cold_ubfm_w(a64_buf_t *b, unsigned rd, unsigned rn, unsigned immr, unsigned imms)
{ cold_emit_u32(b, 0x53000000U | ((immr & 31U) << 16) | ((imms & 31U) << 10) | ((rn & 31U) << 5) | (rd & 31U)); }
static void cold_add_imm_w(a64_buf_t *b, bool sf, unsigned rd, unsigned rn, uint16_t imm12)
{ cold_emit_u32(b, 0x11000000U | (sf ? (1U << 29) : 0U) | (((uint32_t)imm12 & 0xFFFU) << 10) | ((rn & 31U) << 5) | (rd & 31U)); }
static void cold_sub_imm_w(a64_buf_t *b, unsigned rd, unsigned rn, uint16_t imm12)
{ cold_emit_u32(b, 0x51000000U | (((uint32_t)imm12 & 0xFFFU) << 10) | ((rn & 31U) << 5) | (rd & 31U)); }
static void cold_eor_w(a64_buf_t *b, unsigned rd, unsigned rn, unsigned rm)
{ cold_emit_u32(b, 0x4A000000U | ((rm & 31U) << 16) | ((rn & 31U) << 5) | (rd & 31U)); }

static void cold_mov_w(a64_buf_t *b, unsigned rd, unsigned rm) {
    if (rd == rm) { return; }
    cold_emit_u32(b, 0x2A000000U | ((rm & 31U) << 16) | ((31U & 31U) << 5) | (rd & 31U));
}

static void cold_add_uxtw_x(a64_buf_t *b, unsigned rd_x, unsigned rn_x, unsigned rm_w)
{ cold_emit_u32(b, 0x8B204000U | ((rm_w & 31U) << 16) | ((rn_x & 31U) << 5) | (rd_x & 31U)); }
static void cold_add_shift_x(a64_buf_t *b, unsigned rd_x, unsigned rn_x, unsigned rm_x, unsigned amt)
{ cold_emit_u32(b, 0x8B000000U | ((rm_x & 31U) << 16) | ((amt & 63U) << 10) | ((rn_x & 31U) << 5) | (rd_x & 31U)); }
static void cold_add_imm_x(a64_buf_t *b, unsigned rd_x, unsigned rn_x, uint16_t imm12)
{ cold_emit_u32(b, 0x91000000U | (((uint32_t)imm12 & 0xFFFU) << 10) | ((rn_x & 31U) << 5) | (rd_x & 31U)); }

static void cold_emit_vfp_flush(a64_buf_t *b, uint32_t dirty, bool fpcr_active) {
    static const uint32_t k_str_opc[4] = { 0U, 0xBD000000U, 0xFD000000U, 0x3D800000U };
    if (dirty != 0U) {
        for (unsigned i = 0U; i < 24U; i++) {
            if ((dirty & (1U << i)) == 0U) { continue; }
            uint8_t kind = b->vfp_slot_kind[i];
            uint8_t reg = b->vfp_slot_reg[i];
            if (kind < 1U || kind > 3U) { continue; }
            unsigned shift = (unsigned)kind + 1U;
            uint32_t off = 0x60U + ((uint32_t)reg << shift);
            cold_emit_u32(b, k_str_opc[kind] | (((off >> shift) & 0xFFFU) << 10) | (19U << 5) | (8U + i));
        }
    }
    if (fpcr_active) {
        cold_ldr_imm_w(b, 23U, 19U, 352U);
        cold_mov_imm32(b, 17U, 0x07C00000U);
        cold_emit_u32(b, 0x0A000000U | ((17U & 31U) << 16) | ((23U & 31U) << 5) | (23U & 31U));
        cold_emit_u32(b, 0xD51B4400U | 23U);
    }
}

static void cold_emit_fault(a64_buf_t *b, unsigned fault_reg_w, uint32_t pc,
    uint32_t itstate, uint32_t vfp_dirty, bool fpcr_active, bool self_loop) {
    if ((itstate & 0x0FU) != 0U) {
        cold_mov_imm32(b, 15U, itstate);
        cold_str_imm_w(b, 15U, 19U, 380U);
    }
    cold_str_imm_w(b, fault_reg_w, 19U, (uint32_t)offsetof(a32_ctx_t, fault_addr));
    cold_mov_pc32(b, 15U, pc);
    cold_movz_w(b, 16U, 5U, 0U);
    cold_emit_vfp_flush(b, self_loop ? vfp_dirty : 0U, fpcr_active);
    if (!self_loop && vfp_dirty != 0U) { cold_emit_vfp_flush(b, vfp_dirty, false); }
    cold_emit_u32(b, 0xD65F03C0U);
}

/* ---- Per-block AOT site staging (P2 #7, A32-IOS-D4) ----
 * Thread-local like g_compile_buf: concurrent pthread_create/clone block
 * translations without g_jit_lock must never corrupt each other's patch
 * sites or PC maps (the PC map side already lives in thread-local
 * t_stage_pc_map in vm_jit_state.c). */
static _Thread_local a32_vm32_mem_site_t g_site_stage[A32_AOT_SITE_PER_BLOCK];
static _Thread_local uint32_t g_site_stage_base = 0U;
static _Thread_local uint32_t g_site_stage_count = 0U;
static _Thread_local bool g_site_stage_valid = false;

void a32_emit_sites_flush(void) {
    if (g_site_stage_valid && g_site_stage_count > 0U) {
        (void)a32_aot_site_track(g_site_stage_base, g_site_stage, g_site_stage_count);
    }
    g_site_stage_valid = false;
    g_site_stage_count = 0U;
}

/* Discard this thread's staged sites for one guest block without
 * publishing them (DBT epoch-retry retranslation in dbt_jit_query.c). */
void a32_emit_sites_reset_block(uint32_t guest_start_pc) {
    if (g_site_stage_valid && g_site_stage_base == guest_start_pc) {
        g_site_stage_valid = false;
        g_site_stage_count = 0U;
    }
}

static void emit_site_record(a64_buf_t *b, uint32_t opc, unsigned rt, unsigned addr_w, uint32_t host_off, bool is_store) {
    a32_vm32_mem_site_t rec;
    uint32_t cur_pc;
    if (g_site_stage_valid && b->guest_start_pc != g_site_stage_base) { a32_emit_sites_flush(); }
    if (g_site_stage_count >= A32_AOT_SITE_PER_BLOCK) { return; }
    cur_pc = a32_jit_last_record_pc();
    memset(&rec, 0, sizeof(rec));
    rec.seq_start_offset = 0U;
    rec.host_insn_offset = (uint16_t)(host_off & 0xFFFFU);
    rec.cont_insn_offset = 0U;
    rec.guest_pc_delta = cur_pc - b->guest_start_pc;
    rec.npc_delta = 4;
    rec.insn_word = 0U;
    rec.sp_rollback_delta = 0;
    rec.npc_mode = (uint8_t)A32_VM32_NPC_SEQUENTIAL;
    rec.width = (uint8_t)(1U << ((opc >> 30U) & 3U));
    rec.flags = is_store ? (uint8_t)A32_VM32_SITE_STORE : 0U;
    if (rec.width <= 2U && (opc & 0x800000U) != 0U) { rec.flags |= (uint8_t)A32_VM32_SITE_SIGNED; }
    rec.val_or_dst_reg = (uint8_t)(rt & 0xFFU);
    rec.addr_base_reg = (uint8_t)(addr_w & 0xFFU);
    rec.reserved = 0U;
    g_site_stage[g_site_stage_count++] = rec;
    g_site_stage_base = b->guest_start_pc;
    g_site_stage_valid = true;
}

static void emit_hot_check(a64_buf_t *b, unsigned addr_w, uint32_t span, uint32_t tag_off) {
    a64_emit_ubfm_w(b, 23U, addr_w, 12U, 31U);
    a64_emit(b, 0x8B000000U | ((23U & 31U) << 16) | ((4U & 63U) << 10) | ((25U & 31U) << 5) | (23U & 31U));
    a64_emit(b, 0xF9400000U | ((tag_off / 8U) << 10) | (23U << 5) | 23U);
    a64_emit_add_uxtw_x(b, 23U, 23U, addr_w);
    if (span > 1U) { a64_emit(b, 0x91000000U | (((span - 1U) & 0xFFFU) << 10) | ((23U & 31U) << 5) | (23U & 31U)); }
    a64_emit(b, 0xD34CFC00U | (23U << 5) | 23U);
}

static void queue_cold_span1(a64_buf_t *b, unsigned addr_w, uint32_t tag_off,
    uint32_t cur_pc, uint32_t itstate, uint32_t dirty, bool fpcr,
    bool self_loop, size_t hot_cbnz, size_t cold_start) {
    (void)tag_off;
    cold_record(b, (uint32_t)hot_cbnz, (uint32_t)cold_start, COLD_KIND_HOT_CBNZ);
    cold_emit_fault(b, addr_w, cur_pc, itstate, dirty, fpcr, self_loop);
}

static void queue_cold_spanN(a64_buf_t *b, unsigned addr_w, uint32_t span,
    uint32_t tag_off, uint32_t cur_pc, uint32_t itstate, uint32_t dirty,
    bool fpcr, bool self_loop, size_t hot_cbnz, size_t cold_start,
    size_t hot_join, bool is_addr_x, unsigned dst_rt, uint32_t mem_opc) {
    size_t c0;
    size_t bcs;
    size_t c2;
    size_t bjoin;
    cold_record(b, (uint32_t)hot_cbnz, (uint32_t)cold_start, COLD_KIND_HOT_CBNZ);
    c0 = b->cold_count;
    cold_ubfm_w(b, 23U, addr_w, 12U, 31U);
    cold_add_shift_x(b, 23U, 25U, 23U, 4U);
    cold_emit_u32(b, 0xF9400000U | ((tag_off / 8U) << 10) | (23U << 5) | 23U);
    cold_add_uxtw_x(b, 23U, 23U, addr_w);
    cold_emit_u32(b, 0xD34CFC00U | (23U << 5) | 23U);
    cold_emit_u32(b, 0xB5000017U);
    (void)c0;
    {
        size_t src = b->cold_count - 1U;
        size_t dst = b->cold_count + 4U;
        uint32_t orig = b->cold_words[src];
        int32_t d = (int32_t)dst - (int32_t)src;
        b->cold_words[src] = (orig & 0xFF00001FU) | (((uint32_t)d & 0x7FFFFU) << 5);
    }
    cold_add_imm_w(b, true, 31U, addr_w, (uint16_t)(span - 1U));
    bcs = b->cold_count;
    cold_emit_u32(b, 0x54000002U);
    cold_ubfm_w(b, 23U, addr_w, 12U, 31U);
    cold_add_imm_w(b, false, 23U, 23U, 1U);
    cold_add_shift_x(b, 23U, 25U, 23U, 4U);
    cold_emit_u32(b, 0xF9400000U | ((tag_off / 8U) << 10) | (23U << 5) | 23U);
    cold_add_uxtw_x(b, 23U, 23U, addr_w);
    cold_add_imm_x(b, 23U, 23U, (uint16_t)(span - 1U));
    cold_emit_u32(b, 0xD34CFC00U | (23U << 5) | 23U);
    c2 = b->cold_count;
    cold_emit_u32(b, 0xB5000017U);
    if (!is_addr_x) {
        cold_emit_u32(b, mem_opc | ((addr_w & 31U) << 16) | ((20U & 31U) << 5) | (dst_rt & 31U));
    } else { cold_emit_u32(b, 0x8B204000U | ((addr_w & 31U) << 16) | ((20U & 31U) << 5) | (dst_rt & 31U)); }
    bjoin = b->cold_count;
    cold_emit_u32(b, 0x14000000U);
    cold_record(b, (uint32_t)bjoin, (uint32_t)hot_join, COLD_KIND_COLD_B); {
        size_t dst = b->cold_count;
        uint32_t orig = b->cold_words[bcs];
        int32_t d = (int32_t)dst - (int32_t)bcs;
        b->cold_words[bcs] = 0x54000000U | (((uint32_t)d & 0x7FFFFU) << 5) | 2U;
        orig = b->cold_words[c2];
        d = (int32_t)dst - (int32_t)c2;
        b->cold_words[c2] = (orig & 0xFF00001FU) | (((uint32_t)d & 0x7FFFFU) << 5);
    } {
        size_t f1 = b->cold_count;
        size_t src0 = cold_start + 5U;
        uint32_t o0 = b->cold_words[src0];
        int32_t d0 = (int32_t)f1 - (int32_t)src0;
        b->cold_words[src0] = (o0 & 0xFF00001FU) | (((uint32_t)d0 & 0x7FFFFU) << 5);
        cold_emit_fault(b, addr_w, cur_pc, itstate, dirty, fpcr, self_loop);
        size_t f2 = b->cold_count;
        uint32_t o1 = b->cold_words[bcs];
        (void)o1;
        cold_add_imm_w(b, false, 23U, addr_w, (uint16_t)(span - 1U));
        cold_emit_fault(b, 23U, cur_pc, itstate, dirty, fpcr, self_loop);
        (void)f2;
    }
}

static void emit_soft_tlb_check(a64_buf_t *b, unsigned addr_w, uint32_t byte_span, bool is_store,
    bool is_addr_x, unsigned dst_rt, uint32_t mem_opc) {
    uint32_t cur_pc = a32_jit_last_record_pc();
    uint32_t itstate = a32_jit_last_record_itstate();
    uint32_t tag_off = is_store ? 8U : 0U;
    uint32_t dirty;
    bool fpcr;
    bool self_loop;
    size_t hot_cbnz;
    size_t cold_start;
    size_t need_c;
    size_t need_p;
    if (b->ctx != NULL) { b->ctx->tlb_checks_emitted++; }
    if (!b->self_loop_vfp_active && b->vfp_slot_dirty != 0U) { a64_vfp_cache_flush(b, false); }
    dirty = b->vfp_slot_dirty;
    fpcr = b->fpcr_neon_active;
    self_loop = b->self_loop_vfp_active;
    need_c = (byte_span <= 1U) ? 12U : 48U;
    need_p = (byte_span <= 1U) ? 1U : 2U;
    if (!cold_reserve(b, need_c, need_p)) {
        emit_hot_check(b, addr_w, byte_span, tag_off); {
            size_t cbz = b->count;
            a64_emit(b, 0xB4000017U);
            emit_soft_tlb_fault_itstate(b, itstate);
            a64_emit_str_imm_w(b, addr_w, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, fault_addr));
            a64_emit_mov_pc32(b, 15U, cur_pc);
            a64_emit_movz_w(b, 16U, 5U, 0U);
            a64_emit_ret(b); {
                int32_t d = (int32_t)b->count - (int32_t)cbz;
                uint32_t o = b->words[cbz];
                b->words[cbz] = (o & 0xFF00001FU) | (((uint32_t)d & 0x7FFFFU) << 5);
            }
        }
        return;
    }
    emit_hot_check(b, addr_w, byte_span, tag_off);
    hot_cbnz = b->count;
    a64_emit(b, 0xB5000017U);
    cold_start = b->cold_count;
    if (byte_span <= 1U) {
        queue_cold_span1(b, addr_w, tag_off, cur_pc, itstate, dirty, fpcr, self_loop, hot_cbnz, cold_start);
        return;
    }
    queue_cold_spanN(b, addr_w, byte_span, tag_off, cur_pc, itstate, dirty,
        fpcr, self_loop, hot_cbnz, cold_start, hot_cbnz + 2U, is_addr_x, dst_rt, mem_opc);
}

static _Thread_local uint32_t t_grp_expected = 0U;
static _Thread_local size_t t_grp_cold_snap = 0U;
static _Thread_local size_t t_grp_patch_snap = 0U;
static _Thread_local uint32_t t_grp_site_snap = 0U;

static bool grp_decode_w22(uint32_t w, unsigned *rn, int32_t *off) {
    unsigned rd = w & 31U;
    if (rd != 22U) { return false; }
    if ((w & 0xFF000000U) == 0x11000000U) {
        unsigned sh = (w >> 22) & 1U;
        unsigned r = (w >> 5) & 31U;
        unsigned imm = (w >> 10) & 0xFFFU;
        if (sh != 0U || r >= 15U) { return false; }
        *rn = r;
        *off = (int32_t)imm;
        return true;
    }
    if ((w & 0xFF000000U) == 0x51000000U) {
        unsigned sh = (w >> 22) & 1U;
        unsigned r = (w >> 5) & 31U;
        unsigned imm = (w >> 10) & 0xFFFU;
        if (sh != 0U || r >= 15U) { return false; }
        *rn = r;
        *off = -(int32_t)imm;
        return true;
    } {
        unsigned shift = (w >> 22) & 3U;
        unsigned imm6 = (w >> 10) & 63U;
        unsigned rnn = (w >> 5) & 31U;
        unsigned rm = (w >> 16) & 31U;
        if (((w >> 24) & 0xFFU) != 0x2AU) { return false; }
        if (shift != 0U || imm6 != 0U || rnn != 31U || rm >= 15U) { return false; }
        *rn = rm;
        *off = 0;
        return true;
    }
}

static void grp_flush(a64_buf_t *b) {
    if (b->mem_grp_active) { b->mem_grp_active = false; }
}

static void grp_emit_grouped(a64_buf_t *b, unsigned rn, int32_t min_off, int32_t max_off, bool has_store, unsigned n, int32_t *offs,
    uint8_t *rts, uint8_t *stores, uint32_t *pcs, uint32_t *its,
    uint32_t dirty, bool fpcr, bool self_loop) {
    uint32_t range_len = (uint32_t)(max_off + 4 - min_off);
    uint32_t tag_off = has_store ? 8U : 0U;
    size_t hot_cbnz;
    size_t cold_start;
    size_t hot_join;
    unsigned i;
    b->count = b->mem_grp_words_idx;
    b->cold_count = t_grp_cold_snap;
    b->cold_patch_count = t_grp_patch_snap;
    g_site_stage_count = t_grp_site_snap;
    if (min_off == 0) {
        a64_emit_mov_w(b, 22U, rn);
    } else if (min_off > 0) {
        a64_emit_add_imm_w(b, false, 22U, rn, (uint16_t)min_off, 0U);
    } else { a64_emit_sub_imm_w(b, false, 22U, rn, (uint16_t)(-min_off), 0U); }
    if (b->ctx != NULL) { b->ctx->tlb_checks_emitted++; }
    emit_hot_check(b, 22U, range_len, tag_off);
    hot_cbnz = b->count;
    a64_emit(b, 0xB5000017U);
    cold_start = b->cold_count;
    cold_record(b, (uint32_t)hot_cbnz, (uint32_t)cold_start, COLD_KIND_HOT_CBNZ);
    a64_emit_add_uxtw_x(b, 23U, 20U, 22U);
    for (i = 0U; i < n; i++) {
        int32_t rel = offs[i] - min_off;
        uint32_t uoff = (uint32_t)rel;
        bool cur_store = stores[i] != 0U;
        unsigned rt = rts[i];
        if (i + 1U < n && stores[i] == stores[i + 1U] && offs[i + 1] == offs[i] + 4 && rt != rts[i + 1U] &&
            (uoff & 3U) == 0U && uoff <= 252U) {
            int32_t rel2 = offs[i + 1] - min_off;
            uint32_t uoff2 = (uint32_t)rel2;
            (void)uoff2;
            if (!cur_store) {
                a64_emit(b, 0x29400000U | (((uoff >> 2) & 0x7FU) << 15) | (((uint32_t)rts[i + 1U] & 31U) << 10) |
                    ((23U & 31U) << 5) | (rt & 31U));
            } else {
                a64_emit(b, 0x29000000U | (((uoff >> 2) & 0x7FU) << 15) | (((uint32_t)rts[i + 1U] & 31U) << 10) |
                    ((23U & 31U) << 5) | (rt & 31U));
            }
            i++;
        } else {
            if (!cur_store) {
                a64_emit(b, 0xB9400000U | (((uoff >> 2) & 0xFFFU) << 10) | ((23U & 31U) << 5) | (rt & 31U));
            } else { a64_emit(b, 0xB9000000U | (((uoff >> 2) & 0xFFFU) << 10) | ((23U & 31U) << 5) | (rt & 31U)); }
        }
    }
    hot_join = b->count; {
        size_t per_op_mem[16];
        size_t c_first[16];
        size_t c_same[16];
        size_t c_wrap[16];
        size_t c_second[16];
        size_t f_first[16];
        size_t f_second[16];
        for (i = 0U; i < n; i++) {
            int32_t o = offs[i];
            uint32_t pc = pcs[i];
            uint32_t it = its[i];
            bool st = stores[i] != 0U;
            uint32_t toff = st ? 8U : 0U;
            if (o == 0) {
                cold_mov_w(b, 22U, rn);
            } else if (o > 0) {
                cold_add_imm_w(b, false, 22U, rn, (uint16_t)o);
            } else { cold_sub_imm_w(b, 22U, rn, (uint16_t)(-o)); }
            cold_ubfm_w(b, 23U, 22U, 12U, 31U);
            cold_add_shift_x(b, 23U, 25U, 23U, 4U);
            cold_emit_u32(b, 0xF9400000U | ((toff / 8U) << 10) | (23U << 5) | 23U);
            cold_add_uxtw_x(b, 23U, 23U, 22U);
            cold_emit_u32(b, 0xD34CFC00U | (23U << 5) | 23U);
            c_first[i] = b->cold_count;
            cold_emit_u32(b, 0xB5000017U);
            cold_add_imm_w(b, false, 23U, 22U, 3U);
            cold_eor_w(b, 23U, 23U, 22U);
            cold_ubfm_w(b, 23U, 23U, 12U, 31U);
            c_same[i] = b->cold_count;
            cold_emit_u32(b, 0x34000017U);
            cold_add_imm_w(b, true, 31U, 22U, 3U);
            c_wrap[i] = b->cold_count;
            cold_emit_u32(b, 0x54000002U);
            cold_ubfm_w(b, 23U, 22U, 12U, 31U);
            cold_add_imm_w(b, false, 23U, 23U, 1U);
            cold_add_shift_x(b, 23U, 25U, 23U, 4U);
            cold_emit_u32(b, 0xF9400000U | ((toff / 8U) << 10) | (23U << 5) | 23U);
            cold_add_uxtw_x(b, 23U, 23U, 22U);
            cold_add_imm_x(b, 23U, 23U, 3U);
            cold_emit_u32(b, 0xD34CFC00U | (23U << 5) | 23U);
            c_second[i] = b->cold_count;
            cold_emit_u32(b, 0xB5000017U);
            per_op_mem[i] = b->cold_count;
            if (!st) {
                cold_emit_u32(b, 0xB8604800U | ((22U & 31U) << 16) | ((20U & 31U) << 5) | (rts[i] & 31U));
            } else {
                cold_emit_u32(b, 0xB8204800U | ((22U & 31U) << 16) | ((20U & 31U) << 5) | (rts[i] & 31U));
            } {
                int32_t d = (int32_t)per_op_mem[i] - (int32_t)c_same[i];
                b->cold_words[c_same[i]] = 0x34000000U | (((uint32_t)d & 0x7FFFFU) << 5) | 23U;
            }
            f_first[i] = b->cold_count; {
                int32_t d = (int32_t)f_first[i] - (int32_t)c_first[i];
                uint32_t o0 = b->cold_words[c_first[i]];
                b->cold_words[c_first[i]] = (o0 & 0xFF00001FU) | (((uint32_t)d & 0x7FFFFU) << 5);
            }
            cold_emit_fault(b, 22U, pc, it, dirty, fpcr, self_loop);
            f_second[i] = b->cold_count;
            cold_add_imm_w(b, false, 23U, 22U, 3U); {
                int32_t d = (int32_t)f_second[i] - (int32_t)c_wrap[i];
                b->cold_words[c_wrap[i]] = 0x54000000U | (((uint32_t)d & 0x7FFFFU) << 5) | 2U;
                d = (int32_t)f_second[i] - (int32_t)c_second[i];
                uint32_t o1 = b->cold_words[c_second[i]];
                b->cold_words[c_second[i]] = (o1 & 0xFF00001FU) | (((uint32_t)d & 0x7FFFFU) << 5);
            }
            cold_emit_fault(b, 23U, pc, it, dirty, fpcr, self_loop);
        } {
            size_t bj = b->cold_count;
            cold_emit_u32(b, 0x14000000U);
            cold_record(b, (uint32_t)bj, (uint32_t)hot_join, COLD_KIND_COLD_B);
        }
    }
    if (b->ctx != NULL) { b->ctx->tlb_group_guards++; }
}

static bool grp_start_new(a64_buf_t *b, unsigned rn, int32_t off, unsigned rt, bool is_store, uint32_t pc, uint32_t it) {
    if (off < -4095 || off + 3 > 4095) {
        grp_flush(b);
        return false;
    }
    b->mem_grp_words_idx = b->count - 1U;
    b->mem_grp_rn = (uint8_t)rn;
    b->mem_grp_count = 1U;
    b->mem_grp_off[0] = off;
    b->mem_grp_rt[0] = (uint8_t)rt;
    b->mem_grp_op[0] = is_store ? 1U : 0U;
    b->mem_grp_pc[0] = pc;
    b->mem_grp_itstate[0] = it;
    b->mem_grp_cond_depth = b->cond_branch_depth;
    b->mem_grp_active = true;
    t_grp_cold_snap = b->cold_count;
    t_grp_patch_snap = b->cold_patch_count;
    t_grp_site_snap = g_site_stage_count;
    t_grp_expected = b->mem_grp_words_idx + 9U;
    return false;
}

static bool grp_try_word(a64_buf_t *b, unsigned rt, bool is_store) {
    unsigned rn = 0U;
    int32_t off = 0;
    uint32_t cur_pc;
    uint32_t cur_it;
    if (b->count == 0U) {
        grp_flush(b);
        return false;
    }
    if (!grp_decode_w22(b->words[b->count - 1U], &rn, &off)) {
        grp_flush(b);
        return false;
    }
    cur_pc = a32_jit_last_record_pc();
    cur_it = a32_jit_last_record_itstate();
    if (!b->mem_grp_active) {
        return grp_start_new(b, rn, off, rt, is_store, cur_pc, cur_it);
    } {
        unsigned n = b->mem_grp_count;
        unsigned grn = b->mem_grp_rn;
        int32_t cur_min = b->mem_grp_off[0];
        int32_t cur_max = b->mem_grp_off[0];
        bool has_store = false;
        int32_t new_min;
        int32_t new_max;
        uint32_t range_len;
        unsigned i;
        if (b->count != t_grp_expected + 1U) {
            grp_flush(b);
            return grp_start_new(b, rn, off, rt, is_store, cur_pc, cur_it);
        }
        if (rn != grn || n >= 16U) {
            grp_flush(b);
            return grp_start_new(b, rn, off, rt, is_store, cur_pc, cur_it);
        }
        for (i = 0U; i < n; i++) {
            if (b->mem_grp_off[i] < cur_min) { cur_min = b->mem_grp_off[i]; }
            if (b->mem_grp_off[i] > cur_max) { cur_max = b->mem_grp_off[i]; }
            if (b->mem_grp_op[i] != 0U) { has_store = true; }
            if (b->mem_grp_op[i] == 0U && b->mem_grp_rt[i] == grn) {
                grp_flush(b);
                return grp_start_new(b, rn, off, rt, is_store, cur_pc, cur_it);
            }
        }
        if (is_store) { has_store = true; }
        new_min = (off < cur_min) ? off : cur_min;
        new_max = (off > cur_max) ? off : cur_max;
        range_len = (uint32_t)(new_max + 4 - new_min);
        if (range_len > 256U || new_min < -4095 || new_max + 3 > 4095) {
            grp_flush(b);
            return grp_start_new(b, rn, off, rt, is_store, cur_pc, cur_it);
        }
        if (((off - cur_min) % 4) != 0) {
            grp_flush(b);
            return grp_start_new(b, rn, off, rt, is_store, cur_pc, cur_it);
        }
        if (!cold_reserve(b, 80U, 3U)) {
            grp_flush(b);
            return false;
        } {
            int32_t offs[16];
            uint8_t rts[16];
            uint8_t stores[16];
            uint32_t pcs[16];
            uint32_t its[16];
            for (i = 0U; i < n; i++) {
                offs[i] = b->mem_grp_off[i];
                rts[i] = b->mem_grp_rt[i];
                stores[i] = b->mem_grp_op[i];
                pcs[i] = b->mem_grp_pc[i];
                its[i] = b->mem_grp_itstate[i];
            }
            offs[n] = off;
            rts[n] = (uint8_t)rt;
            stores[n] = is_store ? 1U : 0U;
            pcs[n] = cur_pc;
            its[n] = cur_it;
            grp_emit_grouped(b, grn, new_min, new_max, has_store, n + 1U, offs, rts, stores, pcs, its, b->vfp_slot_dirty,
                b->fpcr_neon_active, b->self_loop_vfp_active);
            for (i = 0U; i <= n; i++) {
                b->mem_grp_off[i] = offs[i];
                b->mem_grp_rt[i] = rts[i];
                b->mem_grp_op[i] = stores[i];
                b->mem_grp_pc[i] = pcs[i];
                b->mem_grp_itstate[i] = its[i];
            }
            b->mem_grp_count = n + 1U;
            t_grp_expected = b->count;
            if (!is_store && rt == grn) { b->mem_grp_active = false; }
            if (n + 1U >= 16U) { b->mem_grp_active = false; }
            return true;
        }
    }
}

void a64_emit_guest_addr_x(a64_buf_t *b, unsigned dst_x, unsigned addr_w, uint32_t byte_span, bool is_store) {
    if (b->mem_grp_active) { b->mem_grp_active = false; }
    if (a32_vm32_current_regime() != A32_VM32_REGIME_A2_SOFT_TLB || a32_vm32_tlb_base == 0U) {
        a64_emit_add_uxtw_x(b, dst_x, A64_XG, addr_w);
        return;
    }
    emit_soft_tlb_check(b, addr_w, byte_span, is_store, true, dst_x, 0U);
    a64_emit_add_uxtw_x(b, dst_x, A64_XG, addr_w);
}

static void emit_guest_mem(a64_buf_t *b, uint32_t opc, unsigned rt, unsigned addr_w, bool is_store) {
    bool is_word = (opc == 0xB8604800U || opc == 0xB8204800U);
    bool do_grp = is_word && addr_w == 22U && a32_vm32_current_regime() == A32_VM32_REGIME_A2_SOFT_TLB &&
        a32_vm32_tlb_base != 0U && b->cond_branch_depth == 0U;
    if (do_grp) {
        if (grp_try_word(b, rt, is_store)) { return; }
    } else if (b->mem_grp_active) {
        b->mem_grp_active = false;
    }
    if (b->vfp_slot_dirty != 0U) { a64_vfp_cache_flush(b, false); }
    emit_site_record(b, opc, rt, addr_w, (uint32_t)b->count, is_store);
    if (a32_vm32_current_regime() != A32_VM32_REGIME_A2_SOFT_TLB || a32_vm32_tlb_base == 0U) {
        a64_emit(b, opc | ((addr_w & 31U) << 16) | (A64_XG << 5) | (rt & 31U));
        return;
    } {
        uint32_t width = 1U << ((opc >> 30U) & 3U);
        if (do_grp && width == 4U) {
            emit_soft_tlb_check(b, addr_w, width, is_store, false, rt, opc);
            a64_emit(b, opc | ((addr_w & 31U) << 16) | (A64_XG << 5) | (rt & 31U));
            return;
        }
        emit_soft_tlb_check(b, addr_w, width, is_store, false, rt, opc);
        a64_emit(b, opc | ((addr_w & 31U) << 16) | (A64_XG << 5) | (rt & 31U));
    }
}

/* Guest memory accesses: [xG, wAddr, uxtw #0] */
void a64_emit_guest_ldr_w(a64_buf_t *b, unsigned rt, unsigned addr_w) { emit_guest_mem(b, 0xB8604800U, rt, addr_w, false); }
void a64_emit_guest_str_w(a64_buf_t *b, unsigned rt, unsigned addr_w) { emit_guest_mem(b, 0xB8204800U, rt, addr_w, true); }

void a64_emit_guest_ldrd_w(a64_buf_t *b, unsigned rt, unsigned rt2, unsigned addr_w) {
    if (b->vfp_slot_dirty != 0U) { a64_vfp_cache_flush(b, false); }
    a64_emit_guest_addr_x(b, A64_TMP2, addr_w, 8U, false);
    if (rt != rt2) {
        a64_emit(b, 0x29400000U | ((rt2 & 31U) << 10) | (A64_TMP2 << 5) | (rt & 31U));
    } else { a64_emit(b, 0xB9400000U | (1U << 10) | (A64_TMP2 << 5) | (rt & 31U)); }
}

void a64_emit_guest_strd_w(a64_buf_t *b, unsigned rt, unsigned rt2, unsigned addr_w) {
    if (b->vfp_slot_dirty != 0U) { a64_vfp_cache_flush(b, false); }
    a64_emit_guest_addr_x(b, A64_TMP2, addr_w, 8U, true);
    a64_emit(b, 0x29000000U | ((rt2 & 31U) << 10) | (A64_TMP2 << 5) | (rt & 31U));
}

void a64_emit_guest_ldrb_w(a64_buf_t *b, unsigned rt, unsigned addr_w) { emit_guest_mem(b, 0x38604800U, rt, addr_w, false); }
void a64_emit_guest_ldrsb_w(a64_buf_t *b, unsigned rt, unsigned addr_w) { emit_guest_mem(b, 0x38A04800U, rt, addr_w, false); }
void a64_emit_guest_strb_w(a64_buf_t *b, unsigned rt, unsigned addr_w) { emit_guest_mem(b, 0x38204800U, rt, addr_w, true); }
void a64_emit_guest_ldrh_w(a64_buf_t *b, unsigned rt, unsigned addr_w) { emit_guest_mem(b, 0x78604800U, rt, addr_w, false); }
void a64_emit_guest_ldrsh_w(a64_buf_t *b, unsigned rt, unsigned addr_w) { emit_guest_mem(b, 0x78A04800U, rt, addr_w, false); }
void a64_emit_guest_strh_w(a64_buf_t *b, unsigned rt, unsigned addr_w) { emit_guest_mem(b, 0x78204800U, rt, addr_w, true); }

static void emit_ctx_mem_imm(a64_buf_t *b, uint32_t opc, unsigned shift, bool is_store,
    unsigned rt, unsigned rn_x, uint32_t byte_offset) {
    if (rn_x == A64_XCTX && byte_offset >= A32_CTX_VFP_OFFSET && byte_offset < A32_CTX_VFP_OFFSET + 256U) {
        b->vfp_preheader_ineligible = true;
        a64_vfp_cache_flush(b, is_store);
    }
    uint32_t uimm12 = (byte_offset >> shift) & 0xFFFU;
    a64_emit(b, opc | (uimm12 << 10) | ((rn_x & 31U) << 5) | (rt & 31U));
}

/* Host unsigned-offset loads/stores */
void a64_emit_ldrb_imm_w(a64_buf_t *b, unsigned rt, unsigned rn_x, uint32_t byte_offset)
{ emit_ctx_mem_imm(b, 0x39400000U, 0U, false, rt, rn_x, byte_offset); }
void a64_emit_strb_imm_w(a64_buf_t *b, unsigned rt, unsigned rn_x, uint32_t byte_offset)
{ emit_ctx_mem_imm(b, 0x39000000U, 0U, true, rt, rn_x, byte_offset); }
void a64_emit_ldrh_imm_w(a64_buf_t *b, unsigned rt, unsigned rn_x, uint32_t byte_offset)
{ emit_ctx_mem_imm(b, 0x79400000U, 1U, false, rt, rn_x, byte_offset); }
void a64_emit_strh_imm_w(a64_buf_t *b, unsigned rt, unsigned rn_x, uint32_t byte_offset)
{ emit_ctx_mem_imm(b, 0x79000000U, 1U, true, rt, rn_x, byte_offset); }
void a64_emit_ldr_imm_w(a64_buf_t *b, unsigned rt, unsigned rn_x, uint32_t byte_offset)
{ emit_ctx_mem_imm(b, 0xB9400000U, 2U, false, rt, rn_x, byte_offset); }
void a64_emit_str_imm_w(a64_buf_t *b, unsigned rt, unsigned rn_x, uint32_t byte_offset)
{ emit_ctx_mem_imm(b, 0xB9000000U, 2U, true, rt, rn_x, byte_offset); }

/* VFPv4-D32 / NEON guest memory loads/stores */
void a64_emit_guest_ldr_s(a64_buf_t *b, unsigned vt, unsigned addr_w) { emit_guest_mem(b, 0xBC604800U, vt, addr_w, false); }
void a64_emit_guest_str_s(a64_buf_t *b, unsigned vt, unsigned addr_w) { emit_guest_mem(b, 0xBC204800U, vt, addr_w, true); }
void a64_emit_guest_ldr_d(a64_buf_t *b, unsigned vt, unsigned addr_w) { emit_guest_mem(b, 0xFC604800U, vt, addr_w, false); }
void a64_emit_guest_str_d(a64_buf_t *b, unsigned vt, unsigned addr_w) { emit_guest_mem(b, 0xFC204800U, vt, addr_w, true); }

/* Scalar FP instructions */
void a64_emit_fmov_w_to_s(a64_buf_t *b, unsigned vd, unsigned rn_w) { a64_emit(b, 0x1E270000U | ((rn_w & 31U) << 5) | (vd & 31U)); }
void a64_emit_fmov_s_to_w(a64_buf_t *b, unsigned rd_w, unsigned vn) { a64_emit(b, 0x1E260000U | ((vn & 31U) << 5) | (rd_w & 31U)); }
void a64_emit_fcvt_s_to_d(a64_buf_t *b, unsigned vd, unsigned vn) { a64_emit(b, 0x1E22C000U | ((vn & 31U) << 5) | (vd & 31U)); }
void a64_emit_fcvt_d_to_s(a64_buf_t *b, unsigned vd, unsigned vn) { a64_emit(b, 0x1E624000U | ((vn & 31U) << 5) | (vd & 31U)); }
void a64_emit_fcvtzs_s_to_w(a64_buf_t *b, unsigned rd_w, unsigned vn) { a64_emit(b, 0x1E380000U | ((vn & 31U) << 5) | (rd_w & 31U)); }
void a64_emit_fcvtzu_s_to_w(a64_buf_t *b, unsigned rd_w, unsigned vn) { a64_emit(b, 0x1E390000U | ((vn & 31U) << 5) | (rd_w & 31U)); }
void a64_emit_fcvtzs_d_to_w(a64_buf_t *b, unsigned rd_w, unsigned vn) { a64_emit(b, 0x1E780000U | ((vn & 31U) << 5) | (rd_w & 31U)); }
void a64_emit_fcvtzu_d_to_w(a64_buf_t *b, unsigned rd_w, unsigned vn) { a64_emit(b, 0x1E790000U | ((vn & 31U) << 5) | (rd_w & 31U)); }
void a64_emit_scvtf_w_to_s(a64_buf_t *b, unsigned vd, unsigned rn_w) { a64_emit(b, 0x1E220000U | ((rn_w & 31U) << 5) | (vd & 31U)); }
void a64_emit_ucvtf_w_to_s(a64_buf_t *b, unsigned vd, unsigned rn_w) { a64_emit(b, 0x1E230000U | ((rn_w & 31U) << 5) | (vd & 31U)); }
void a64_emit_scvtf_w_to_d(a64_buf_t *b, unsigned vd, unsigned rn_w) { a64_emit(b, 0x1E620000U | ((rn_w & 31U) << 5) | (vd & 31U)); }
void a64_emit_ucvtf_w_to_d(a64_buf_t *b, unsigned vd, unsigned rn_w) { a64_emit(b, 0x1E630000U | ((rn_w & 31U) << 5) | (vd & 31U)); }

/* ARMv8-A Crypto extensions (AES, PMULL, SHA1, SHA256) */
void a64_emit_aese(a64_buf_t *b, unsigned vd, unsigned vn) { a64_emit(b, 0x4E284800U | ((vn & 31U) << 5) | (vd & 31U)); }
void a64_emit_aesd(a64_buf_t *b, unsigned vd, unsigned vn) { a64_emit(b, 0x4E285800U | ((vn & 31U) << 5) | (vd & 31U)); }
void a64_emit_aesmc(a64_buf_t *b, unsigned vd, unsigned vn) { a64_emit(b, 0x4E286800U | ((vn & 31U) << 5) | (vd & 31U)); }
void a64_emit_aesimc(a64_buf_t *b, unsigned vd, unsigned vn) { a64_emit(b, 0x4E287800U | ((vn & 31U) << 5) | (vd & 31U)); }
void a64_emit_pmull_1q(a64_buf_t *b, unsigned vd, unsigned vn, unsigned vm)
{ a64_emit(b, 0x0EE0E000U | ((vm & 31U) << 16) | ((vn & 31U) << 5) | (vd & 31U)); }
void a64_emit_sha1c(a64_buf_t *b, unsigned vd, unsigned vn, unsigned vm)
{ a64_emit(b, 0x5E000000U | ((vm & 31U) << 16) | ((vn & 31U) << 5) | (vd & 31U)); }
void a64_emit_sha1p(a64_buf_t *b, unsigned vd, unsigned vn, unsigned vm)
{ a64_emit(b, 0x5E001000U | ((vm & 31U) << 16) | ((vn & 31U) << 5) | (vd & 31U)); }
void a64_emit_sha1m(a64_buf_t *b, unsigned vd, unsigned vn, unsigned vm)
{ a64_emit(b, 0x5E002000U | ((vm & 31U) << 16) | ((vn & 31U) << 5) | (vd & 31U)); }
void a64_emit_sha1su0(a64_buf_t *b, unsigned vd, unsigned vn, unsigned vm)
{ a64_emit(b, 0x5E003000U | ((vm & 31U) << 16) | ((vn & 31U) << 5) | (vd & 31U)); }
void a64_emit_sha1h(a64_buf_t *b, unsigned vd, unsigned vn) { a64_emit(b, 0x5E280800U | ((vn & 31U) << 5) | (vd & 31U)); }
void a64_emit_sha1su1(a64_buf_t *b, unsigned vd, unsigned vn) { a64_emit(b, 0x5E281800U | ((vn & 31U) << 5) | (vd & 31U)); }
void a64_emit_sha256h(a64_buf_t *b, unsigned vd, unsigned vn, unsigned vm)
{ a64_emit(b, 0x5E004000U | ((vm & 31U) << 16) | ((vn & 31U) << 5) | (vd & 31U)); }
void a64_emit_sha256h2(a64_buf_t *b, unsigned vd, unsigned vn, unsigned vm)
{ a64_emit(b, 0x5E005000U | ((vm & 31U) << 16) | ((vn & 31U) << 5) | (vd & 31U)); }
void a64_emit_sha256su0(a64_buf_t *b, unsigned vd, unsigned vn) { a64_emit(b, 0x5E282800U | ((vn & 31U) << 5) | (vd & 31U)); }
void a64_emit_sha256su1(a64_buf_t *b, unsigned vd, unsigned vn, unsigned vm)
{ a64_emit(b, 0x5E006000U | ((vm & 31U) << 16) | ((vn & 31U) << 5) | (vd & 31U)); }

/* Exclusives & barriers */
void a64_emit_ldxr_b(a64_buf_t *b, unsigned rt_w, unsigned rn_x) {
    if (b->vfp_slot_dirty != 0U) { a64_vfp_cache_flush(b, false); }
    a64_emit(b, 0x085F7C00U | ((rn_x & 31U) << 5) | (rt_w & 31U));
}

void a64_emit_stxr_b(a64_buf_t *b, unsigned rs_w, unsigned rt_w, unsigned rn_x)
{ a64_emit(b, 0x08007C00U | ((rs_w & 31U) << 16) | ((rn_x & 31U) << 5) | (rt_w & 31U)); }

void a64_emit_ldxr_h(a64_buf_t *b, unsigned rt_w, unsigned rn_x) {
    if (b->vfp_slot_dirty != 0U) { a64_vfp_cache_flush(b, false); }
    a64_emit(b, 0x485F7C00U | ((rn_x & 31U) << 5) | (rt_w & 31U));
}

void a64_emit_stxr_h(a64_buf_t *b, unsigned rs_w, unsigned rt_w, unsigned rn_x)
{ a64_emit(b, 0x48007C00U | ((rs_w & 31U) << 16) | ((rn_x & 31U) << 5) | (rt_w & 31U)); }

void a64_emit_ldxr_w(a64_buf_t *b, unsigned rt_w, unsigned rn_x) {
    if (b->vfp_slot_dirty != 0U) { a64_vfp_cache_flush(b, false); }
    a64_emit(b, 0x885F7C00U | ((rn_x & 31U) << 5) | (rt_w & 31U));
}

void a64_emit_stxr_w(a64_buf_t *b, unsigned rs_w, unsigned rt_w, unsigned rn_x)
{ a64_emit(b, 0x88007C00U | ((rs_w & 31U) << 16) | ((rn_x & 31U) << 5) | (rt_w & 31U)); }

void a64_emit_ldxr_x(a64_buf_t *b, unsigned rt_x, unsigned rn_x) {
    if (b->vfp_slot_dirty != 0U) { a64_vfp_cache_flush(b, false); }
    a64_emit(b, 0xC85F7C00U | ((rn_x & 31U) << 5) | (rt_x & 31U));
}

void a64_emit_stxr_x(a64_buf_t *b, unsigned rs_w, unsigned rt_x, unsigned rn_x)
{ a64_emit(b, 0xC8007C00U | ((rs_w & 31U) << 16) | ((rn_x & 31U) << 5) | (rt_x & 31U)); }
void a64_emit_clrex(a64_buf_t *b) { a64_emit(b, 0xD5033F5FU); }
void a64_emit_dmb_ish(a64_buf_t *b) { a64_emit(b, 0xD5033BBFU); }
void a64_emit_dsb_ish(a64_buf_t *b) { a64_emit(b, 0xD5033B9FU); }
void a64_emit_isb(a64_buf_t *b) { a64_emit(b, 0xD5033FDFU); }

size_t a64_emit_b_cond_placeholder(a64_buf_t *b, unsigned cond) {
    if (b->mem_grp_active) { b->mem_grp_active = false; }
    if (!b->self_loop_vfp_active && b->vfp_slot_dirty != 0U) { a64_vfp_cache_flush(b, false); }
    /* A32-IOS-P1: a conditional skip region may execute scalar VFP ops
     * under fpcr_neon_active, so restore scalar FPCR up front (outside
     * the region) and clear the flag. Skipped for a pure-NEON self-loop
     * whose preheader hoisted Standard FPSCR for the whole loop body. */
    if (b->cond_branch_depth == 0U && b->fpcr_neon_active && !(b->preheader_neon_fpcr && !b->saw_scalar_vfp)) {
        a64_emit_fpcr_restore_scalar(b);
        b->fpcr_neon_active = false;
    }
    b->cond_branch_depth++;
    size_t idx = b->count;
    a64_emit(b, 0x54000000U | (cond & 15U));
    return idx;
}

void a64_patch_b_cond(a64_buf_t *b, size_t patch_idx, size_t target_idx) {
    if (b->mem_grp_active) { b->mem_grp_active = false; }
    if (!b->self_loop_vfp_active && b->vfp_slot_dirty != 0U) { a64_vfp_cache_flush(b, false); }
    if (b->cond_branch_depth > 0U) { b->cond_branch_depth--; }
    if (patch_idx >= b->count || patch_idx >= b->cap) {
        b->overflow = true;
        return;
    }
    int32_t disp = (int32_t)target_idx - (int32_t)patch_idx;
    if (disp < -(1 << 18) || disp >= (1 << 18)) {
        b->overflow = true;
        return;
    }
    uint32_t cond = b->words[patch_idx] & 15U;
    b->words[patch_idx] = 0x54000000U | (((uint32_t)disp & 0x7FFFFU) << 5) | cond;
}

size_t a64_emit_cbz_w_placeholder(a64_buf_t *b, bool is_cbnz, unsigned rt) {
    if (b->mem_grp_active) { b->mem_grp_active = false; }
    if (!b->self_loop_vfp_active && b->vfp_slot_dirty != 0U) { a64_vfp_cache_flush(b, false); }
    b->cond_branch_depth++;
    size_t idx = b->count;
    uint32_t base = is_cbnz ? 0x35000000U : 0x34000000U;
    a64_emit(b, base | (rt & 31U));
    return idx;
}

void a64_patch_cbz_w(a64_buf_t *b, size_t patch_idx, size_t target_idx) {
    if (b->mem_grp_active) { b->mem_grp_active = false; }
    if (!b->self_loop_vfp_active && b->vfp_slot_dirty != 0U) { a64_vfp_cache_flush(b, false); }
    if (b->cond_branch_depth > 0U) { b->cond_branch_depth--; }
    if (patch_idx >= b->count || patch_idx >= b->cap) {
        b->overflow = true;
        return;
    }
    int32_t disp = (int32_t)target_idx - (int32_t)patch_idx;
    if (disp < -(1 << 18) || disp >= (1 << 18)) {
        b->overflow = true;
        return;
    }
    uint32_t op_rt = b->words[patch_idx] & 0xFF00001FU;
    b->words[patch_idx] = op_rt | (((uint32_t)disp & 0x7FFFFU) << 5);
}

void a64_buf_flush_cold_stubs(a64_buf_t *b) {
    size_t hot_count;
    size_t i;
    if (b == NULL) { return; }
    if (b->mem_grp_active) { b->mem_grp_active = false; }
    if (b->cold_count == 0U) { return; }
    hot_count = b->count;
    if (hot_count + b->cold_count + 1U > b->cap) {
        b->overflow = true;
        return;
    }
    for (i = 0U; i < b->cold_count; i++) { b->words[b->count++] = b->cold_words[i]; }
    for (i = 0U; i < b->cold_patch_count; i++) {
        uint32_t src = b->cold_patch_src[i];
        uint32_t dst = b->cold_patch_dst[i];
        uint8_t kind = b->cold_patch_kind[i];
        if (kind == COLD_KIND_HOT_CBNZ) {
            size_t s = (size_t)src;
            size_t d = hot_count + (size_t)dst;
            int32_t disp = (int32_t)d - (int32_t)s;
            if (disp < -(1 << 18) || disp >= (1 << 18)) {
                b->overflow = true;
                continue;
            }
            if (s >= b->count) {
                b->overflow = true;
                continue;
            }
            b->words[s] = (b->words[s] & 0xFF00001FU) | (((uint32_t)disp & 0x7FFFFU) << 5);
        } else if (kind == COLD_KIND_COLD_B) {
            size_t s = hot_count + (size_t)src;
            size_t d = (size_t)dst;
            int32_t disp = (int32_t)d - (int32_t)s;
            if (disp < -(1 << 25) || disp >= (1 << 25)) {
                b->overflow = true;
                continue;
            }
            if (s >= b->count) {
                b->overflow = true;
                continue;
            }
            b->words[s] = 0x14000000U | ((uint32_t)disp & 0x3FFFFFFU);
        } else { b->overflow = true; }
    }
    if (b->count == 0U || b->words[b->count - 1U] != 0xD65F03C0U) {
        if (b->count < b->cap) {
            a64_emit(b, 0xD65F03C0U);
        } else { b->overflow = true; }
    }
}

