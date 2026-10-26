/*
 * src/core/core_emit_mem.c - split-TU part 2/3 of src/a64_emit.c (lines 603-1367).
 * Compiled only via the src/a64_emit.c wrapper; do not add to Makefile SRCS.
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

/* Guest memory accesses: [xG, wAddr, uxtw #0] */
void a64_emit_guest_ldr_w(a64_buf_t *b, unsigned rt, unsigned addr_w)
{
    emit_guest_mem(b, 0xB8604800U, rt, addr_w);
}

void a64_emit_guest_str_w(a64_buf_t *b, unsigned rt, unsigned addr_w)
{
    emit_guest_mem(b, 0xB8204800U, rt, addr_w);
}

void a64_emit_guest_ldrd_w(a64_buf_t *b, unsigned rt, unsigned rt2, unsigned addr_w)
{
    if (!b->self_loop_vfp_active && b->vfp_slot_dirty != 0U) {
        a64_vfp_cache_flush(b, false);
    }
    a64_emit_add_uxtw_x(b, A64_TMP2, A64_XG, addr_w);
    if (rt != rt2) {
        a64_emit(b, 0x29400000U | ((rt2 & 31U) << 10) | (A64_TMP2 << 5) | (rt & 31U));
    } else {
        a64_emit(b, 0xB9400000U | (1U << 10) | (A64_TMP2 << 5) | (rt & 31U));
    }
}

void a64_emit_guest_strd_w(a64_buf_t *b, unsigned rt, unsigned rt2, unsigned addr_w)
{
    if (!b->self_loop_vfp_active && b->vfp_slot_dirty != 0U) {
        a64_vfp_cache_flush(b, false);
    }
    a64_emit_add_uxtw_x(b, A64_TMP2, A64_XG, addr_w);
    a64_emit(b, 0x29000000U | ((rt2 & 31U) << 10) | (A64_TMP2 << 5) | (rt & 31U));
}

void a64_emit_guest_ldrb_w(a64_buf_t *b, unsigned rt, unsigned addr_w)
{
    emit_guest_mem(b, 0x38604800U, rt, addr_w);
}

void a64_emit_guest_ldrsb_w(a64_buf_t *b, unsigned rt, unsigned addr_w)
{
    emit_guest_mem(b, 0x38A04800U, rt, addr_w);
}

void a64_emit_guest_strb_w(a64_buf_t *b, unsigned rt, unsigned addr_w)
{
    emit_guest_mem(b, 0x38204800U, rt, addr_w);
}

void a64_emit_guest_ldrh_w(a64_buf_t *b, unsigned rt, unsigned addr_w)
{
    emit_guest_mem(b, 0x78604800U, rt, addr_w);
}

void a64_emit_guest_ldrsh_w(a64_buf_t *b, unsigned rt, unsigned addr_w)
{
    emit_guest_mem(b, 0x78A04800U, rt, addr_w);
}

void a64_emit_guest_strh_w(a64_buf_t *b, unsigned rt, unsigned addr_w)
{
    emit_guest_mem(b, 0x78204800U, rt, addr_w);
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

/* VFPv4-D32 / NEON context & guest memory loads/stores */
static void vfp_ldr_ctx(a64_buf_t *b, uint8_t kind, unsigned vt, unsigned idx)
{
    vfp_record_preheader_access(b, kind, (uint8_t)idx, false);
    if (b->self_loop_vfp_active) {
        for (unsigned i = 0U; i < A64_VFP_CACHE_SLOTS; i++) {
            if (b->vfp_slot_kind[i] == kind && b->vfp_slot_reg[i] == idx) {
                vfp_emit_reg_mov(b, kind, vt, 8U + i);
                return;
            }
        }
        vfp_emit_raw_ldr(b, kind, vt, idx);
        return;
    }
    for (unsigned i = 0U; i < A64_VFP_CACHE_SLOTS; i++) {
        if (b->vfp_slot_kind[i] != A64_VFP_KIND_NONE && b->vfp_slot_kind[i] != kind &&
            vfp_regs_alias(b->vfp_slot_kind[i], b->vfp_slot_reg[i], kind, (uint8_t)idx)) {
            vfp_flush_slot(b, i, true);
        }
    }
    for (unsigned i = 0U; i < A64_VFP_CACHE_SLOTS; i++) {
        if (b->vfp_slot_kind[i] == kind && b->vfp_slot_reg[i] == idx) {
            vfp_emit_reg_mov(b, kind, vt, 8U + i);
            return;
        }
    }
    vfp_emit_raw_ldr(b, kind, vt, idx);
    if (b->cond_branch_depth == 0U) {
        unsigned slot = vfp_alloc_slot(b, kind, (uint8_t)idx);
        vfp_emit_reg_mov(b, kind, 8U + slot, vt);
    }
}

static void vfp_str_ctx(a64_buf_t *b, uint8_t kind, unsigned vt, unsigned idx)
{
    vfp_record_preheader_access(b, kind, (uint8_t)idx, true);
    if (b->self_loop_vfp_active) {
        for (unsigned i = 0U; i < A64_VFP_CACHE_SLOTS; i++) {
            if (b->vfp_slot_kind[i] == kind && b->vfp_slot_reg[i] == idx) {
                vfp_emit_reg_mov(b, kind, 8U + i, vt);
                b->vfp_slot_dirty |= (1U << i);
                return;
            }
        }
        vfp_emit_raw_str(b, kind, vt, idx);
        return;
    }
    for (unsigned i = 0U; i < A64_VFP_CACHE_SLOTS; i++) {
        if (b->vfp_slot_kind[i] != A64_VFP_KIND_NONE && b->vfp_slot_kind[i] != kind &&
            vfp_regs_alias(b->vfp_slot_kind[i], b->vfp_slot_reg[i], kind, (uint8_t)idx)) {
            if (b->vfp_slot_kind[i] < kind && b->cond_branch_depth == 0U) {
                b->vfp_slot_dirty &= ~(1U << i);
                b->vfp_slot_kind[i] = A64_VFP_KIND_NONE;
            } else {
                vfp_flush_slot(b, i, true);
            }
        }
    }
    if (b->cond_branch_depth == 0U) {
        for (unsigned i = 0U; i < A64_VFP_CACHE_SLOTS; i++) {
            if (b->vfp_slot_kind[i] == kind && b->vfp_slot_reg[i] == idx) {
                vfp_emit_reg_mov(b, kind, 8U + i, vt);
                b->vfp_slot_dirty |= (1U << i);
                return;
            }
        }
        unsigned slot = vfp_alloc_slot(b, kind, (uint8_t)idx);
        vfp_emit_reg_mov(b, kind, 8U + slot, vt);
        b->vfp_slot_dirty |= (1U << slot);
        return;
    }
    for (unsigned i = 0U; i < A64_VFP_CACHE_SLOTS; i++) {
        if (b->vfp_slot_kind[i] == kind && b->vfp_slot_reg[i] == idx) {
            b->vfp_slot_dirty &= ~(1U << i);
            b->vfp_slot_kind[i] = A64_VFP_KIND_NONE;
        }
    }
    vfp_emit_raw_str(b, kind, vt, idx);
}

void a64_emit_ldr_s_ctx(a64_buf_t *b, unsigned vt, unsigned s_idx)
{
    vfp_ldr_ctx(b, A64_VFP_KIND_S, vt, s_idx);
}

void a64_emit_str_s_ctx(a64_buf_t *b, unsigned vt, unsigned s_idx)
{
    vfp_str_ctx(b, A64_VFP_KIND_S, vt, s_idx);
}

void a64_emit_ldr_d_ctx(a64_buf_t *b, unsigned vt, unsigned d_idx)
{
    vfp_ldr_ctx(b, A64_VFP_KIND_D, vt, d_idx);
}

void a64_emit_str_d_ctx(a64_buf_t *b, unsigned vt, unsigned d_idx)
{
    vfp_str_ctx(b, A64_VFP_KIND_D, vt, d_idx);
}

void a64_emit_ldr_q_ctx(a64_buf_t *b, unsigned vt, unsigned q_idx)
{
    vfp_ldr_ctx(b, A64_VFP_KIND_Q, vt, q_idx);
}

void a64_emit_str_q_ctx(a64_buf_t *b, unsigned vt, unsigned q_idx)
{
    vfp_str_ctx(b, A64_VFP_KIND_Q, vt, q_idx);
}

void a64_emit_guest_ldr_s(a64_buf_t *b, unsigned vt, unsigned addr_w)
{
    emit_guest_mem(b, 0xBC604800U, vt, addr_w);
}

void a64_emit_guest_str_s(a64_buf_t *b, unsigned vt, unsigned addr_w)
{
    emit_guest_mem(b, 0xBC204800U, vt, addr_w);
}

void a64_emit_guest_ldr_d(a64_buf_t *b, unsigned vt, unsigned addr_w)
{
    emit_guest_mem(b, 0xFC604800U, vt, addr_w);
}

void a64_emit_guest_str_d(a64_buf_t *b, unsigned vt, unsigned addr_w)
{
    emit_guest_mem(b, 0xFC204800U, vt, addr_w);
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
    if (patch_idx >= b->count) {
        return;
    }
    int32_t disp = (int32_t)target_idx - (int32_t)patch_idx;
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
    if (patch_idx >= b->count) {
        return;
    }
    int32_t disp = (int32_t)target_idx - (int32_t)patch_idx;
    uint32_t op_rt = b->words[patch_idx] & 0xFF00001FU;
    b->words[patch_idx] = op_rt | (((uint32_t)disp & 0x7FFFFU) << 5);
}

void a64_emit_ret(a64_buf_t *b)
{
    vfp_emit_exit_flush(b);
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
            vfp_emit_exit_flush(b);
        }
        a64_emit_mov_pc32(b, A64_TMP0, target_pc);
        a64_emit_movz_w(b, A64_TMP1, A32_EXIT_BRANCH, 0U);
        a64_emit(b, 0xD65F03C0U); /* ret */
        return;
    }

    vfp_emit_exit_flush(b);

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
    vfp_emit_exit_flush(b);
    if (target_reg != A64_TMP0) {
        a64_emit_mov_w(b, A64_TMP0, target_reg);
    }

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
