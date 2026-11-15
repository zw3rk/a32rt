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
    if (b->ctx != NULL) {
        b->ctx->tlb_checks_emitted++;
    }

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

void a64_buf_flush_cold_stubs(a64_buf_t *b)
{
    if (b == NULL || b->cold_count == 0U) {
        return;
    }
}

