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
static void emit_soft_tlb_fault_itstate(a64_buf_t *b, uint32_t itstate)
{
    if ((itstate & 0x0FU) != 0U) {
        a64_emit_mov_imm32(b, A64_TMP0, itstate);
        a64_emit_str_imm_w(b, A64_TMP0, A64_XCTX,
            (uint32_t)offsetof(a32_ctx_t, itstate));
    }
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

void a32_emit_sites_flush(void)
{
    if (g_site_stage_valid && g_site_stage_count > 0U) {
        (void)a32_aot_site_track(g_site_stage_base, g_site_stage,
            g_site_stage_count);
    }
    g_site_stage_valid = false;
    g_site_stage_count = 0U;
}

/* Discard this thread's staged sites for one guest block without
 * publishing them (DBT epoch-retry retranslation in dbt_jit_query.c). */
void a32_emit_sites_reset_block(uint32_t guest_start_pc)
{
    if (g_site_stage_valid && g_site_stage_base == guest_start_pc) {
        g_site_stage_valid = false;
        g_site_stage_count = 0U;
    }
}

static void emit_site_record(a64_buf_t *b, uint32_t opc, unsigned rt,
    unsigned addr_w, uint32_t host_off, bool is_store)
{
    a32_vm32_mem_site_t rec;
    uint32_t cur_pc;
    if (g_site_stage_valid && b->guest_start_pc != g_site_stage_base) {
        a32_emit_sites_flush();
    }
    if (g_site_stage_count >= A32_AOT_SITE_PER_BLOCK) {
        return;
    }
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
    if (rec.width <= 2U && (opc & 0x800000U) != 0U) {
        rec.flags |= (uint8_t)A32_VM32_SITE_SIGNED;
    }
    rec.val_or_dst_reg = (uint8_t)(rt & 0xFFU);
    rec.addr_base_reg = (uint8_t)(addr_w & 0xFFU);
    rec.reserved = 0U;
    g_site_stage[g_site_stage_count++] = rec;
    g_site_stage_base = b->guest_start_pc;
    g_site_stage_valid = true;
}

static void emit_soft_tlb_check(a64_buf_t *b, unsigned addr_w, uint32_t byte_span, bool is_store)
{
    uint32_t cur_pc = a32_jit_last_record_pc();
    uint32_t itstate = a32_jit_last_record_itstate();
    uint32_t tag_off = is_store ? 16U : 0U;

    /* A32-IOS-P4: single-instruction Soft-TLB base reload. The trampoline
     * spills the regime base once per run_block entry at [sp, #160], so
     * translated code stays position-independent across ASLR runs. The
     * host sp is constant for the whole JIT block residency. */
    a64_emit(b, 0xF94053F5U); /* ldr x21, [sp, #160] */
    /* 20-bit Soft-TLB index: gva >> 12 (into x21 = &g_tlb[idx]). */
    a64_emit_ubfm_w(b, 23U, addr_w, 12U, 31U);
    a64_emit_orr_reg_w(b, 23U, A64_ZR, 23U, A64_SHIFT_LSL, 5U);
    a64_emit_add_uxtw_x(b, 21U, 21U, 23U);
    /* 64-bit tag load: ldr x23, [x21, #tag_off]; lsr x23, x23, #12 */
    a64_emit(b, 0xF9400000U | ((tag_off / 8U) << 10) | (21U << 5) | 23U);
    a64_emit(b, 0xD34CFC00U | (23U << 5) | 23U);
    a64_emit_eor_reg_w(b, 23U, 23U, addr_w, A64_SHIFT_LSR, 12U);

    if (byte_span <= 1U) {
        size_t cbz = a64_emit_cbz_w_placeholder(b, false, 23U);
        emit_soft_tlb_fault_itstate(b, itstate);
        a64_emit_str_imm_w(b, addr_w, A64_XCTX,
            (uint32_t)offsetof(a32_ctx_t, fault_addr));
        a64_emit_mov_pc32(b, 15U, cur_pc);
        a64_emit_movz_w(b, 16U, (uint16_t)A32_EXIT_FAULT, 0U);
        a64_emit_ret(b);
        a64_patch_cbz_w(b, cbz, b->count);
    } else {
        size_t cbnz_miss0 = a64_emit_cbz_w_placeholder(b, true, 23U);
        /* Fast-path check: does [addr_w, addr_w + byte_span - 1] cross a 4 KiB page? */
        a64_emit_add_imm_w(b, false, 23U, addr_w, (uint16_t)(byte_span - 1U), 0U);
        a64_emit_eor_reg_w(b, 23U, 23U, addr_w, A64_SHIFT_LSL, 0U);
        a64_emit_ubfm_w(b, 23U, 23U, 12U, 31U);
        size_t cbz_same = a64_emit_cbz_w_placeholder(b, false, 23U);

        /* Cross-page slow path: reject 32-bit wraparound at 0xFFFFFFFF -> 0 */
        a64_emit_add_imm_w(b, false, 23U, addr_w, (uint16_t)(byte_span - 1U), 0U);
        a64_emit_ubfm_w(b, 23U, 23U, 12U, 31U);
        size_t cbz_wrap = a64_emit_cbz_w_placeholder(b, false, 23U);

        /* Verify second page tag at [x21, #(32 + tag_off)] == (addr_w >> 12) + 1 */
        a64_emit(b, 0xF9400000U | (((32U + tag_off) / 8U) << 10) | (21U << 5) | 23U);
        a64_emit(b, 0xD34CFC00U | (23U << 5) | 23U);
        a64_emit_sub_imm_w(b, false, 23U, 23U, 1U, 0U);
        a64_emit_eor_reg_w(b, 23U, 23U, addr_w, A64_SHIFT_LSR, 12U);
        size_t cbz_hit2 = a64_emit_cbz_w_placeholder(b, false, 23U);

        /* Second-page fault: fault_addr = addr_w + byte_span - 1 */
        a64_patch_cbz_w(b, cbz_wrap, b->count);
        a64_emit_add_imm_w(b, false, 23U, addr_w, (uint16_t)(byte_span - 1U), 0U);
        emit_soft_tlb_fault_itstate(b, itstate);
        a64_emit_str_imm_w(b, 23U, A64_XCTX,
            (uint32_t)offsetof(a32_ctx_t, fault_addr));
        a64_emit_mov_pc32(b, 15U, cur_pc);
        a64_emit_movz_w(b, 16U, (uint16_t)A32_EXIT_FAULT, 0U);
        a64_emit_ret(b);

        /* First-page fault: fault_addr = addr_w */
        a64_patch_cbz_w(b, cbnz_miss0, b->count);
        emit_soft_tlb_fault_itstate(b, itstate);
        a64_emit_str_imm_w(b, addr_w, A64_XCTX,
            (uint32_t)offsetof(a32_ctx_t, fault_addr));
        a64_emit_mov_pc32(b, 15U, cur_pc);
        a64_emit_movz_w(b, 16U, (uint16_t)A32_EXIT_FAULT, 0U);
        a64_emit_ret(b);

        a64_patch_cbz_w(b, cbz_same, b->count);
        a64_patch_cbz_w(b, cbz_hit2, b->count);
    }
    /* Hit: ldr x23, [x21, #8] (addend). */
    a64_emit(b, 0xF9400000U | (1U << 10) | (21U << 5) | 23U);
}

void a64_emit_guest_addr_x(a64_buf_t *b, unsigned dst_x, unsigned addr_w,
                           uint32_t byte_span, bool is_store)
{
    if (a32_vm32_current_regime() != A32_VM32_REGIME_A2_SOFT_TLB ||
        a32_vm32_tlb_base == 0U) {
        a64_emit_add_uxtw_x(b, dst_x, A64_XG, addr_w);
        return;
    }
    emit_soft_tlb_check(b, addr_w, byte_span, is_store);
    a64_emit_add_uxtw_x(b, dst_x, 23U, addr_w);
}

static void emit_guest_mem(a64_buf_t *b, uint32_t opc, unsigned rt,
                           unsigned addr_w, bool is_store)
{
    /* Flush even inside self-loops: a faulting access must observe
     * current FP registers in ctx, since fault recovery cannot replay
     * the loop's retained host SIMD state (A32-R7-5). Write-through keeps
     * the slot mapping, so later iterations still avoid reloads. */
    if (b->vfp_slot_dirty != 0U) {
        a64_vfp_cache_flush(b, false);
    }
    emit_site_record(b, opc, rt, addr_w, (uint32_t)b->count, is_store);
    if (a32_vm32_current_regime() != A32_VM32_REGIME_A2_SOFT_TLB ||
        a32_vm32_tlb_base == 0U) {
        a64_emit(b, opc | ((addr_w & 31U) << 16) | (A64_XG << 5) | (rt & 31U));
        return;
    }
    {
        uint32_t width = 1U << ((opc >> 30U) & 3U);
        emit_soft_tlb_check(b, addr_w, width, is_store);
        a64_emit(b, opc | ((addr_w & 31U) << 16) | (23U << 5) | (rt & 31U));
    }
}

/* Guest memory accesses: [xG, wAddr, uxtw #0] */
void a64_emit_guest_ldr_w(a64_buf_t *b, unsigned rt, unsigned addr_w)
{
    emit_guest_mem(b, 0xB8604800U, rt, addr_w, false);
}

void a64_emit_guest_str_w(a64_buf_t *b, unsigned rt, unsigned addr_w)
{
    emit_guest_mem(b, 0xB8204800U, rt, addr_w, true);
}

void a64_emit_guest_ldrd_w(a64_buf_t *b, unsigned rt, unsigned rt2, unsigned addr_w)
{
    if (b->vfp_slot_dirty != 0U) {
        a64_vfp_cache_flush(b, false);
    }
    a64_emit_guest_addr_x(b, A64_TMP2, addr_w, 8U, false);
    if (rt != rt2) {
        a64_emit(b, 0x29400000U | ((rt2 & 31U) << 10) | (A64_TMP2 << 5) | (rt & 31U));
    } else {
        a64_emit(b, 0xB9400000U | (1U << 10) | (A64_TMP2 << 5) | (rt & 31U));
    }
}

void a64_emit_guest_strd_w(a64_buf_t *b, unsigned rt, unsigned rt2, unsigned addr_w)
{
    if (b->vfp_slot_dirty != 0U) {
        a64_vfp_cache_flush(b, false);
    }
    a64_emit_guest_addr_x(b, A64_TMP2, addr_w, 8U, true);
    a64_emit(b, 0x29000000U | ((rt2 & 31U) << 10) | (A64_TMP2 << 5) | (rt & 31U));
}

void a64_emit_guest_ldrb_w(a64_buf_t *b, unsigned rt, unsigned addr_w)
{
    emit_guest_mem(b, 0x38604800U, rt, addr_w, false);
}

void a64_emit_guest_ldrsb_w(a64_buf_t *b, unsigned rt, unsigned addr_w)
{
    emit_guest_mem(b, 0x38A04800U, rt, addr_w, false);
}

void a64_emit_guest_strb_w(a64_buf_t *b, unsigned rt, unsigned addr_w)
{
    emit_guest_mem(b, 0x38204800U, rt, addr_w, true);
}

void a64_emit_guest_ldrh_w(a64_buf_t *b, unsigned rt, unsigned addr_w)
{
    emit_guest_mem(b, 0x78604800U, rt, addr_w, false);
}

void a64_emit_guest_ldrsh_w(a64_buf_t *b, unsigned rt, unsigned addr_w)
{
    emit_guest_mem(b, 0x78A04800U, rt, addr_w, false);
}

void a64_emit_guest_strh_w(a64_buf_t *b, unsigned rt, unsigned addr_w)
{
    emit_guest_mem(b, 0x78204800U, rt, addr_w, true);
}

static void emit_ctx_mem_imm(a64_buf_t *b, uint32_t opc, unsigned shift, bool is_store,
                             unsigned rt, unsigned rn_x, uint32_t byte_offset)
{
    if (rn_x == A64_XCTX && byte_offset >= A32_CTX_VFP_OFFSET && byte_offset < A32_CTX_VFP_OFFSET + 256U) {
        b->vfp_preheader_ineligible = true;
        a64_vfp_cache_flush(b, is_store);
    }
    uint32_t uimm12 = (byte_offset >> shift) & 0xFFFU;
    a64_emit(b, opc | (uimm12 << 10) | ((rn_x & 31U) << 5) | (rt & 31U));
}

/* Host unsigned-offset loads/stores */
void a64_emit_ldrb_imm_w(a64_buf_t *b, unsigned rt, unsigned rn_x, uint32_t byte_offset)
{
    emit_ctx_mem_imm(b, 0x39400000U, 0U, false, rt, rn_x, byte_offset);
}

void a64_emit_strb_imm_w(a64_buf_t *b, unsigned rt, unsigned rn_x, uint32_t byte_offset)
{
    emit_ctx_mem_imm(b, 0x39000000U, 0U, true, rt, rn_x, byte_offset);
}

void a64_emit_ldrh_imm_w(a64_buf_t *b, unsigned rt, unsigned rn_x, uint32_t byte_offset)
{
    emit_ctx_mem_imm(b, 0x79400000U, 1U, false, rt, rn_x, byte_offset);
}

void a64_emit_strh_imm_w(a64_buf_t *b, unsigned rt, unsigned rn_x, uint32_t byte_offset)
{
    emit_ctx_mem_imm(b, 0x79000000U, 1U, true, rt, rn_x, byte_offset);
}

void a64_emit_ldr_imm_w(a64_buf_t *b, unsigned rt, unsigned rn_x, uint32_t byte_offset)
{
    emit_ctx_mem_imm(b, 0xB9400000U, 2U, false, rt, rn_x, byte_offset);
}

void a64_emit_str_imm_w(a64_buf_t *b, unsigned rt, unsigned rn_x, uint32_t byte_offset)
{
    emit_ctx_mem_imm(b, 0xB9000000U, 2U, true, rt, rn_x, byte_offset);
}

/* VFPv4-D32 / NEON guest memory loads/stores */
void a64_emit_guest_ldr_s(a64_buf_t *b, unsigned vt, unsigned addr_w)
{
    emit_guest_mem(b, 0xBC604800U, vt, addr_w, false);
}

void a64_emit_guest_str_s(a64_buf_t *b, unsigned vt, unsigned addr_w)
{
    emit_guest_mem(b, 0xBC204800U, vt, addr_w, true);
}

void a64_emit_guest_ldr_d(a64_buf_t *b, unsigned vt, unsigned addr_w)
{
    emit_guest_mem(b, 0xFC604800U, vt, addr_w, false);
}

void a64_emit_guest_str_d(a64_buf_t *b, unsigned vt, unsigned addr_w)
{
    emit_guest_mem(b, 0xFC204800U, vt, addr_w, true);
}

/* Scalar FP instructions */
void a64_emit_fmov_w_to_s(a64_buf_t *b, unsigned vd, unsigned rn_w)
{
    a64_emit(b, 0x1E270000U | ((rn_w & 31U) << 5) | (vd & 31U));
}

void a64_emit_fmov_s_to_w(a64_buf_t *b, unsigned rd_w, unsigned vn)
{
    a64_emit(b, 0x1E260000U | ((vn & 31U) << 5) | (rd_w & 31U));
}

void a64_emit_fcvt_s_to_d(a64_buf_t *b, unsigned vd, unsigned vn)
{
    a64_emit(b, 0x1E22C000U | ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_fcvt_d_to_s(a64_buf_t *b, unsigned vd, unsigned vn)
{
    a64_emit(b, 0x1E624000U | ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_fcvtzs_s_to_w(a64_buf_t *b, unsigned rd_w, unsigned vn)
{
    a64_emit(b, 0x1E380000U | ((vn & 31U) << 5) | (rd_w & 31U));
}

void a64_emit_fcvtzu_s_to_w(a64_buf_t *b, unsigned rd_w, unsigned vn)
{
    a64_emit(b, 0x1E390000U | ((vn & 31U) << 5) | (rd_w & 31U));
}

void a64_emit_fcvtzs_d_to_w(a64_buf_t *b, unsigned rd_w, unsigned vn)
{
    a64_emit(b, 0x1E780000U | ((vn & 31U) << 5) | (rd_w & 31U));
}

void a64_emit_fcvtzu_d_to_w(a64_buf_t *b, unsigned rd_w, unsigned vn)
{
    a64_emit(b, 0x1E790000U | ((vn & 31U) << 5) | (rd_w & 31U));
}

void a64_emit_scvtf_w_to_s(a64_buf_t *b, unsigned vd, unsigned rn_w)
{
    a64_emit(b, 0x1E220000U | ((rn_w & 31U) << 5) | (vd & 31U));
}

void a64_emit_ucvtf_w_to_s(a64_buf_t *b, unsigned vd, unsigned rn_w)
{
    a64_emit(b, 0x1E230000U | ((rn_w & 31U) << 5) | (vd & 31U));
}

void a64_emit_scvtf_w_to_d(a64_buf_t *b, unsigned vd, unsigned rn_w)
{
    a64_emit(b, 0x1E620000U | ((rn_w & 31U) << 5) | (vd & 31U));
}

void a64_emit_ucvtf_w_to_d(a64_buf_t *b, unsigned vd, unsigned rn_w)
{
    a64_emit(b, 0x1E630000U | ((rn_w & 31U) << 5) | (vd & 31U));
}

/* ARMv8-A Crypto extensions (AES, PMULL, SHA1, SHA256) */
void a64_emit_aese(a64_buf_t *b, unsigned vd, unsigned vn)
{
    a64_emit(b, 0x4E284800U | ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_aesd(a64_buf_t *b, unsigned vd, unsigned vn)
{
    a64_emit(b, 0x4E285800U | ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_aesmc(a64_buf_t *b, unsigned vd, unsigned vn)
{
    a64_emit(b, 0x4E286800U | ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_aesimc(a64_buf_t *b, unsigned vd, unsigned vn)
{
    a64_emit(b, 0x4E287800U | ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_pmull_1q(a64_buf_t *b, unsigned vd, unsigned vn, unsigned vm)
{
    a64_emit(b, 0x0EE0E000U | ((vm & 31U) << 16) | ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_sha1c(a64_buf_t *b, unsigned vd, unsigned vn, unsigned vm)
{
    a64_emit(b, 0x5E000000U | ((vm & 31U) << 16) | ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_sha1p(a64_buf_t *b, unsigned vd, unsigned vn, unsigned vm)
{
    a64_emit(b, 0x5E001000U | ((vm & 31U) << 16) | ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_sha1m(a64_buf_t *b, unsigned vd, unsigned vn, unsigned vm)
{
    a64_emit(b, 0x5E002000U | ((vm & 31U) << 16) | ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_sha1su0(a64_buf_t *b, unsigned vd, unsigned vn, unsigned vm)
{
    a64_emit(b, 0x5E003000U | ((vm & 31U) << 16) | ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_sha1h(a64_buf_t *b, unsigned vd, unsigned vn)
{
    a64_emit(b, 0x5E280800U | ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_sha1su1(a64_buf_t *b, unsigned vd, unsigned vn)
{
    a64_emit(b, 0x5E281800U | ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_sha256h(a64_buf_t *b, unsigned vd, unsigned vn, unsigned vm)
{
    a64_emit(b, 0x5E004000U | ((vm & 31U) << 16) | ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_sha256h2(a64_buf_t *b, unsigned vd, unsigned vn, unsigned vm)
{
    a64_emit(b, 0x5E005000U | ((vm & 31U) << 16) | ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_sha256su0(a64_buf_t *b, unsigned vd, unsigned vn)
{
    a64_emit(b, 0x5E282800U | ((vn & 31U) << 5) | (vd & 31U));
}

void a64_emit_sha256su1(a64_buf_t *b, unsigned vd, unsigned vn, unsigned vm)
{
    a64_emit(b, 0x5E006000U | ((vm & 31U) << 16) | ((vn & 31U) << 5) | (vd & 31U));
}

/* Exclusives & barriers */
void a64_emit_ldxr_b(a64_buf_t *b, unsigned rt_w, unsigned rn_x)
{
    if (b->vfp_slot_dirty != 0U) {
        a64_vfp_cache_flush(b, false);
    }
    a64_emit(b, 0x085F7C00U | ((rn_x & 31U) << 5) | (rt_w & 31U));
}

void a64_emit_stxr_b(a64_buf_t *b, unsigned rs_w, unsigned rt_w, unsigned rn_x)
{
    a64_emit(b, 0x08007C00U | ((rs_w & 31U) << 16) | ((rn_x & 31U) << 5) | (rt_w & 31U));
}

void a64_emit_ldxr_h(a64_buf_t *b, unsigned rt_w, unsigned rn_x)
{
    if (b->vfp_slot_dirty != 0U) {
        a64_vfp_cache_flush(b, false);
    }
    a64_emit(b, 0x485F7C00U | ((rn_x & 31U) << 5) | (rt_w & 31U));
}

void a64_emit_stxr_h(a64_buf_t *b, unsigned rs_w, unsigned rt_w, unsigned rn_x)
{
    a64_emit(b, 0x48007C00U | ((rs_w & 31U) << 16) | ((rn_x & 31U) << 5) | (rt_w & 31U));
}

void a64_emit_ldxr_w(a64_buf_t *b, unsigned rt_w, unsigned rn_x)
{
    if (b->vfp_slot_dirty != 0U) {
        a64_vfp_cache_flush(b, false);
    }
    a64_emit(b, 0x885F7C00U | ((rn_x & 31U) << 5) | (rt_w & 31U));
}

void a64_emit_stxr_w(a64_buf_t *b, unsigned rs_w, unsigned rt_w, unsigned rn_x)
{
    a64_emit(b, 0x88007C00U | ((rs_w & 31U) << 16) | ((rn_x & 31U) << 5) | (rt_w & 31U));
}

void a64_emit_ldxr_x(a64_buf_t *b, unsigned rt_x, unsigned rn_x)
{
    if (b->vfp_slot_dirty != 0U) {
        a64_vfp_cache_flush(b, false);
    }
    a64_emit(b, 0xC85F7C00U | ((rn_x & 31U) << 5) | (rt_x & 31U));
}

void a64_emit_stxr_x(a64_buf_t *b, unsigned rs_w, unsigned rt_x, unsigned rn_x)
{
    a64_emit(b, 0xC8007C00U | ((rs_w & 31U) << 16) | ((rn_x & 31U) << 5) | (rt_x & 31U));
}

void a64_emit_clrex(a64_buf_t *b)
{
    a64_emit(b, 0xD5033F5FU);
}

void a64_emit_dmb_ish(a64_buf_t *b)
{
    a64_emit(b, 0xD5033BBFU);
}

void a64_emit_dsb_ish(a64_buf_t *b)
{
    a64_emit(b, 0xD5033B9FU);
}

void a64_emit_isb(a64_buf_t *b)
{
    a64_emit(b, 0xD5033FDFU);
}

size_t a64_emit_b_cond_placeholder(a64_buf_t *b, unsigned cond)
{
    if (!b->self_loop_vfp_active && b->vfp_slot_dirty != 0U) {
        a64_vfp_cache_flush(b, false);
    }
    /* A32-IOS-P1: a conditional skip region may execute scalar VFP ops
     * under fpcr_neon_active, so restore scalar FPCR up front (outside
     * the region) and clear the flag. Skipped for a pure-NEON self-loop
     * whose preheader hoisted Standard FPSCR for the whole loop body. */
    if (b->cond_branch_depth == 0U && b->fpcr_neon_active &&
        !(b->preheader_neon_fpcr && !b->saw_scalar_vfp)) {
        a64_emit_fpcr_restore_scalar(b);
        b->fpcr_neon_active = false;
    }
    b->cond_branch_depth++;
    size_t idx = b->count;
    a64_emit(b, 0x54000000U | (cond & 15U));
    return idx;
}

void a64_patch_b_cond(a64_buf_t *b, size_t patch_idx, size_t target_idx)
{
    if (!b->self_loop_vfp_active && b->vfp_slot_dirty != 0U) {
        a64_vfp_cache_flush(b, false);
    }
    if (b->cond_branch_depth > 0U) {
        b->cond_branch_depth--;
    }
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

size_t a64_emit_cbz_w_placeholder(a64_buf_t *b, bool is_cbnz, unsigned rt)
{
    if (!b->self_loop_vfp_active && b->vfp_slot_dirty != 0U) {
        a64_vfp_cache_flush(b, false);
    }
    b->cond_branch_depth++;
    size_t idx = b->count;
    uint32_t base = is_cbnz ? 0x35000000U : 0x34000000U;
    a64_emit(b, base | (rt & 31U));
    return idx;
}

void a64_patch_cbz_w(a64_buf_t *b, size_t patch_idx, size_t target_idx)
{
    if (!b->self_loop_vfp_active && b->vfp_slot_dirty != 0U) {
        a64_vfp_cache_flush(b, false);
    }
    if (b->cond_branch_depth > 0U) {
        b->cond_branch_depth--;
    }
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

void a64_emit_ret(a64_buf_t *b)
{
    a64_vfp_emit_exit_flush(b);
    a64_emit(b, 0xD65F03C0U);
}

static size_t emit_exit_sig_check(a64_buf_t *b, uint32_t bail_disp)
{
    /* [0] ldp x17, x23, [x19, #368] (pending_signals, flags|itstate) */
    a64_emit(b, 0xA9400000U | (1U << 31) | ((368U / 8U) << 15) | (23U << 10) | (A64_XCTX << 5) | 17U);
    /* [1] orr x17, x17, x23 */
    a64_emit(b, 0xAA170231U);
    /* [2] cbnz x17, .Lbail */
    size_t patch_idx = b->count;
    a64_emit(b, 0xB5000011U | ((bail_disp & 0x7FFFFU) << 5));
    return patch_idx;
}

static void emit_ras_push_inline(a64_buf_t *b)
{
    /* [0] ldr x22, [x27, #96] (cur_entry->ret_entry, uimm = 96/8 = 12) */
    a64_emit(b, 0xF9403376U);
    /* [1] cbz x22, .Lskip_push (+7 words) */
    a64_emit(b, 0xB4000016U | (7U << 5));
    /* [2] ldr w21, [x19, #424] (w21 = ctx->ras_top, uimm = 424/4 = 106) */
    a64_emit(b, 0xB9400000U | (106U << 10) | (A64_XCTX << 5) | 21U);
    /* [3] add w16, w21, #1 */
    a64_emit(b, 0x110006B0U);
    /* [4] and w16, w16, #0x3f */
    a64_emit(b, 0x12001610U);
    /* [5] str w16, [x19, #424] (ctx->ras_top = (ras_top + 1) & 63) */
    a64_emit(b, 0xB9000000U | (106U << 10) | (A64_XCTX << 5) | 16U);
    /* [6] add x21, x19, x21, lsl #5 (x21 = ctx + ras_top * 32) */
    a64_emit(b, 0x8B151675U);
    /* [7] str x22, [x21, #448] (ras[top].entry = ret_entry, uimm = 448/8 = 56) */
    a64_emit(b, 0xF9000000U | (56U << 10) | (21U << 5) | 22U);
}

static void emit_chain_jump_tail(a64_buf_t *b)
{
    /* mov x27, x23 (cur_entry = next_entry) */
    a64_emit(b, 0xAA1703FBU);
    /* add x24, x24, #1 (chain_hits_delta++) */
    a64_emit(b, 0x91000718U);
    /* br x17 */
    a64_emit(b, 0xD61F0220U);
}

void a64_emit_exit_branch(a64_buf_t *b, uint32_t target_pc)
{
    if (target_pc == b->guest_start_pc && b->guest_start_pc != 0U && !b->is_call) {
        b->has_self_loop = true;
        if (!b->self_loop_vfp_active) {
            a64_vfp_cache_flush(b, false);
        }
        /* A non-hoisted self-loop back-edge runs under whatever FPCR the
         * body left active; restore scalar controls before the exit poll
         * (pass-2 preheader hoists cover the preheader_neon_fpcr case, and
         * .Lbail restores via the exit flush below). */
        if (b->fpcr_neon_active && !b->preheader_neon_fpcr) {
            a64_emit_fpcr_restore_scalar(b);
            b->fpcr_neon_active = false;
        }
        emit_exit_sig_check(b, 7U);
        /* [3] ldar w21, [x28] (cur_epoch = g_cache_epoch) */
        a64_emit(b, 0x88DFFC00U | (28U << 5) | 21U);
        /* [4] ldr x22, [x27, #0] (cur_entry->key | (cur_entry->epoch << 32)) */
        a64_emit(b, 0xF9400376U);
        /* [5] eor x21, x21, x22, lsr #32 */
        a64_emit(b, 0xCA5682B5U);
        /* [6] cbnz x21, .Lbail (+3 words) */
        a64_emit(b, 0xB5000015U | (3U << 5));
        /* [7] add x24, x24, #1 (chain_hits_delta++) */
        a64_emit(b, 0x91000718U);
        /* [8] b loop_body_offset (direct PC-relative branch inside block!) */
        int32_t back_disp = (int32_t)b->loop_body_offset - (int32_t)b->count;
        a64_emit(b, 0x14000000U | ((uint32_t)back_disp & 0x03FFFFFFU));
        /* [9] .Lbail: flush VFP if self_loop_vfp_active, then return to trampoline */
        if (b->self_loop_vfp_active) {
            a64_vfp_emit_exit_flush(b);
        }
        a64_emit_mov_pc32(b, A64_TMP0, target_pc);
        a64_emit_movz_w(b, A64_TMP1, A32_EXIT_BRANCH, 0U);
        a64_emit(b, 0xD65F03C0U); /* ret */
        return;
    }

    a64_vfp_emit_exit_flush(b);

    unsigned s = A64_MAX_EXIT_SLOTS;
    for (unsigned i = 0U; i < b->exit_slot_count; i++) {
        if (b->exit_slot_pcs[i] == target_pc) {
            s = i;
            break;
        }
    }
    if (s == A64_MAX_EXIT_SLOTS && b->exit_slot_count < A64_MAX_EXIT_SLOTS) {
        s = b->exit_slot_count++;
        b->exit_slot_pcs[s] = target_pc;
    }

    if (s < A64_MAX_EXIT_SLOTS) {
        uint32_t call_extra = b->is_call ? 8U : 0U;
        emit_exit_sig_check(b, 10U + call_extra);
        /* ldr x23, [x27, #(152 + s*8)] (cur_entry->slot_entry[s], uimm = 19 + s) */
        a64_emit(b, 0xF9400000U | (((19U + s) & 0xFFFU) << 10) | (27U << 5) | 23U);
        /* cbz x23, .Lbail */
        a64_emit(b, 0xB4000017U | ((8U + call_extra) << 5));
        /* ldar w21, [x28] (cur_epoch = g_cache_epoch) */
        a64_emit(b, 0x88DFFC00U | (28U << 5) | 21U);
        /* ldp x22, x17, [x23, #0] (x22 = key|epoch, x17 = host_code) */
        a64_emit(b, 0xA9400000U | (1U << 31) | (17U << 10) | (23U << 5) | 22U);
        /* eor x21, x21, x22, lsr #32 */
        a64_emit(b, 0xCA5682B5U);
        /* cbnz x21, .Lbail */
        a64_emit(b, 0xB5000015U | ((4U + call_extra) << 5));
        if (b->is_call) {
            emit_ras_push_inline(b);
        }
        emit_chain_jump_tail(b);
    }

    /* .Lbail */
    a64_emit_mov_pc32(b, A64_TMP0, target_pc);
    a64_emit_movz_w(b, A64_TMP1, A32_EXIT_BRANCH, 0U);
    a64_emit(b, 0xD65F03C0U); /* ret */
}

void a64_emit_exit_branch_imm(a64_buf_t *b, uint32_t target_pc, bool interwork)
{
    if (!interwork) {
        a64_emit_exit_branch(b, target_pc);
        return;
    }
    a64_emit_mov_pc32(b, A64_TMP0, target_pc);
    a64_emit_movz_w(b, A64_TMP1, A32_EXIT_INTERWORK, 0U);
    a64_emit_ret(b);
}

void a64_emit_exit_branch_reg(a64_buf_t *b, unsigned target_reg, bool interwork)
{
    if (b->lr_written_from_pc) {
        b->is_call = true;
    }
    /* Copy target_reg into TMP0 first: the exit flush below touches TMP2,
     * so a target_reg of TMP2 must already be safe in TMP0 (w15). */
    if (target_reg != A64_TMP0) {
        a64_emit_mov_w(b, A64_TMP0, target_reg);
    }
    a64_vfp_emit_exit_flush(b);

    size_t patch_sig = emit_exit_sig_check(b, 0U);

    size_t patch_iw = 0U;
    if (interwork) {
        /* and w17, w15, #1 */
        a64_emit(b, 0x120001F1U);
        /* eor w17, w17, w26 */
        a64_emit(b, 0x4A1A0231U);
        /* cbnz w17, .Lbail */
        patch_iw = b->count;
        a64_emit(b, 0x35000011U);
    }

    /* ldar w21, [x28] */
    a64_emit(b, 0x88DFFC00U | (28U << 5) | 21U);
    /* cbz w21, .Lbail */
    size_t patch_ep0 = b->count;
    a64_emit(b, 0x34000015U);

    /* and w16, w15, w25 */
    a64_emit(b, 0x0A1901F0U);
    /* orr w16, w16, w26 */
    a64_emit(b, 0x2A1A0210U);
    /* orr x16, x16, x21, lsl #32 (x16 = next_key | (cur_epoch << 32)) */
    a64_emit(b, 0xAA158210U);

    if (!b->is_call) {
        /* Inline RAS Pop */
        /* ldr w17, [x19, #424] (w17 = ctx->ras_top) */
        a64_emit(b, 0xB9400000U | (106U << 10) | (A64_XCTX << 5) | 17U);
        /* sub w17, w17, #1 */
        a64_emit(b, 0x51000631U);
        /* and w17, w17, #0x3f */
        a64_emit(b, 0x12001631U);
        /* add x22, x19, x17, lsl #5 */
        a64_emit(b, 0x8B111676U);
        /* ldr x23, [x22, #448] (ras[r_idx].entry, uimm = 448/8 = 56) */
        a64_emit(b, 0xF9400000U | (56U << 10) | (22U << 5) | 23U);
        /* cbz x23, .Ltry_ibtc */
        size_t patch_ras_null = b->count;
        a64_emit(b, 0xB4000017U);
        /* ldp x21, x22, [x23, #0] (x21 = entry->key|epoch, x22 = entry->host_code) */
        a64_emit(b, 0xA9400000U | (1U << 31) | (22U << 10) | (23U << 5) | 21U);
        /* eor x21, x21, x16 */
        a64_emit(b, 0xCA1002B5U);
        /* cbnz x21, .Ltry_ibtc */
        size_t patch_ras_miss = b->count;
        a64_emit(b, 0xB5000015U);
        /* str w17, [x19, #424] (commit pop: ctx->ras_top = r_idx) */
        a64_emit(b, 0xB9000000U | (106U << 10) | (A64_XCTX << 5) | 17U);
        /* mov x27, x23 (cur_entry = next_entry) */
        a64_emit(b, 0xAA1703FBU);
        /* add x24, x24, #1 (chain_hits_delta++) */
        a64_emit(b, 0x91000718U);
        /* br x22 */
        a64_emit(b, 0xD61F02C0U);

        /* Patch patch_ras_null and patch_ras_miss to here (.Ltry_ibtc) */
        size_t try_ibtc_idx = b->count;
        b->words[patch_ras_null] |= (((uint32_t)(try_ibtc_idx - patch_ras_null) & 0x7FFFFU) << 5);
        b->words[patch_ras_miss] |= (((uint32_t)(try_ibtc_idx - patch_ras_miss) & 0x7FFFFU) << 5);
    }

    /* .Ltry_ibtc: */
    /* ubfx w17, w16, #1, #10 */
    a64_emit(b, 0x53012A11U);
    /* add x22, x19, x17, lsl #5 */
    a64_emit(b, 0x8B111676U);
    /* ldr x23, [x22, #2496] (offsetof(a32_ctx_t, ibtc) + offsetof(a32_ibtc_entry_t, entry), uimm = 312) */
    a64_emit(b, 0xF9400000U | (312U << 10) | (22U << 5) | 23U);
    /* cbz x23, .Lbail */
    size_t patch_ibtc_null = b->count;
    a64_emit(b, 0xB4000017U);
    /* ldp x21, x17, [x23, #0] */
    a64_emit(b, 0xA9400000U | (1U << 31) | (17U << 10) | (23U << 5) | 21U);
    /* eor x21, x21, x16 */
    a64_emit(b, 0xCA1002B5U);
    /* cbnz x21, .Lbail */
    size_t patch_ibtc_miss = b->count;
    a64_emit(b, 0xB5000015U);

    if (b->is_call) {
        emit_ras_push_inline(b);
    }
    emit_chain_jump_tail(b);

    /* .Lbail: */
    size_t bail_idx = b->count;
    b->words[patch_sig] |= (((uint32_t)(bail_idx - patch_sig) & 0x7FFFFU) << 5);
    if (interwork) {
        b->words[patch_iw] |= (((uint32_t)(bail_idx - patch_iw) & 0x7FFFFU) << 5);
    }
    b->words[patch_ep0] |= (((uint32_t)(bail_idx - patch_ep0) & 0x7FFFFU) << 5);
    b->words[patch_ibtc_null] |= (((uint32_t)(bail_idx - patch_ibtc_null) & 0x7FFFFU) << 5);
    b->words[patch_ibtc_miss] |= (((uint32_t)(bail_idx - patch_ibtc_miss) & 0x7FFFFU) << 5);

    a64_emit_movz_w(b, A64_TMP1, interwork ? A32_EXIT_INTERWORK : A32_EXIT_BRANCH, 0U);
    a64_emit(b, 0xD65F03C0U); /* ret */
}

void a64_emit_exit_svc(a64_buf_t *b, uint32_t next_pc, uint32_t svc_imm)
{
    a64_emit_mov_imm32(b, A64_TMP0, svc_imm);
    a64_emit_str_imm_w(b, A64_TMP0, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, svc_imm));
    a64_emit_mov_pc32(b, A64_TMP0, next_pc);
    a64_emit_movz_w(b, A64_TMP1, A32_EXIT_SVC, 0U);
    a64_emit_ret(b);
}

void a64_emit_exit_undef(a64_buf_t *b, uint32_t fault_pc)
{
    a64_emit_mov_pc32(b, A64_TMP0, fault_pc);
    a64_emit_movz_w(b, A64_TMP1, A32_EXIT_UNDEF, 0U);
    a64_emit_ret(b);
}

void a64_emit_exit_bkpt(a64_buf_t *b, uint32_t bkpt_pc)
{
    a64_emit_mov_pc32(b, A64_TMP0, bkpt_pc);
    a64_emit_str_imm_w(b, A64_TMP0, A64_XCTX, (uint32_t)offsetof(a32_ctx_t, regs[15]));
    a64_emit_movz_w(b, A64_TMP1, A32_EXIT_BKPT, 0U);
    a64_emit_ret(b);
}
