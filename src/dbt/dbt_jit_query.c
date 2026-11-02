/*
 * src/dbt/dbt_jit_query.c — JIT block lookup and lookahead compilation
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#define _GNU_SOURCE
#include "dbt/jit_priv.h"

static bool range_is_readonly_exec(const a32_ctx_t *ctx, uint32_t addr, uint32_t len)
{
    if (ctx == NULL || len == 0U || addr + len < addr) {
        return false;
    }
    uint32_t end = addr + len;
    uint32_t cur = addr;
    for (size_t i = 0U; i < ctx->region_count && cur < end; i++) {
        const a32_region_t *r = &ctx->regions[i];
        if (r->end <= cur) {
            continue;
        }
        if (r->start > cur || (r->prot & 0x4U) == 0U || (r->prot & 0x2U) != 0U) {
            return false;
        }
        cur = (r->end < end) ? r->end : end;
    }
    return cur >= end;
}

#define A32_JIT_LOOKAHEAD_MAX 16U

a32_cache_entry_t *a32_jit_lookup_or_compile_entry(a32_ctx_t *ctx, uint32_t pc, bool is_thumb,
                                                   a32_cache_entry_t *oneshot_entry, bool flush_now,
                                                   a32_block_snap_t *out_snap)
{
    uint32_t aligned_pc = pc & (is_thumb ? ~1U : ~3U);
    uint32_t key        = a32_make_key(pc, is_thumb);
    a32_ctx_normalize_itstate(ctx, is_thumb);
    bool uncached = (ctx->itstate != 0U) || ctx->single_step;

    jit_lock_acquire();
    uint32_t cur_epoch = atomic_load_explicit(&g_cache_epoch, memory_order_relaxed);
    if (!uncached) {
        a32_cache_entry_t *existing = a32_jit_find_ht_entry_locked(key, cur_epoch);
        if (existing != NULL) {
            if (flush_now && g_batch_max > g_batch_min && a32_jit_flush_batch_locked(ctx) != 0) {
                jit_lock_release();
                return NULL;
            }
            if (out_snap != NULL) {
                out_snap->code         = atomic_load_explicit(&existing->host_code, memory_order_relaxed);
                out_snap->is_call      = atomic_load_explicit(&existing->is_call, memory_order_relaxed);
                out_snap->guest_pc     = atomic_load_explicit(&existing->guest_pc, memory_order_relaxed);
                out_snap->guest_end_pc = atomic_load_explicit(&existing->guest_end_pc, memory_order_relaxed);
                out_snap->code_bytes   = atomic_load_explicit(&existing->code_bytes, memory_order_relaxed);
                out_snap->epoch        = cur_epoch;
            }
            if (flush_now) {
                atomic_fetch_add_explicit(&g_jit_exec_readers, 1U, memory_order_acq_rel);
            }
            jit_lock_release();
            return existing;
        }
    }

    if (!a32_jit_range_executable(ctx, aligned_pc, is_thumb ? 2U : 4U)) {
        ctx->fault_addr  = aligned_pc;
        ctx->exit_reason = A32_EXIT_FAULT;
        jit_lock_release();
        return NULL;
    }

    uint32_t q_pcs[A32_JIT_LOOKAHEAD_MAX];
    unsigned q_head    = 0U;
    unsigned q_tail    = 1U;
    q_pcs[0]           = aligned_pc;
    bool     is_mt     = atomic_load_explicit(&g_multithreaded, memory_order_acquire);
    unsigned max_batch = (!is_mt && !uncached && flush_now &&
                          range_is_readonly_exec(ctx, aligned_pc, is_thumb ? 2U : 4U))
                             ? A32_JIT_LOOKAHEAD_MAX
                             : 1U;

    static uint32_t stage_words[A32_AOT_MAX_WORDS];
    a32_cache_entry_t *first_entry = NULL;

    for (q_head = 0U; q_head < q_tail; q_head++) {
        uint32_t cur_b_pc  = q_pcs[q_head];
        uint32_t cur_b_key = a32_make_key(cur_b_pc, is_thumb);
        cur_epoch = atomic_load_explicit(&g_cache_epoch, memory_order_relaxed);

        if (q_head > 0U) {
            if (a32_jit_find_ht_entry_locked(cur_b_key, cur_epoch) != NULL) {
                continue;
            }
            if (g_code_used + (A32_AOT_MAX_WORDS * sizeof(uint32_t)) > A32_CODE_CACHE_SIZE ||
                g_side_table_count + MAX_STAGE_PC_MAP > A32_SIDE_TABLE_SIZE ||
                a32_jit_find_ht_slot_for_insert_locked(cur_b_key, cur_epoch, NULL) == NULL) {
                break;
            }
            if (!range_is_readonly_exec(ctx, cur_b_pc, is_thumb ? 2U : 4U)) {
                continue;
            }
        } else if (!uncached && a32_jit_find_ht_slot_for_insert_locked(cur_b_key, cur_epoch, NULL) == NULL) {
            a32_jit_reset_cache_locked(ctx);
            cur_epoch = atomic_load_explicit(&g_cache_epoch, memory_order_relaxed);
        }

        uint32_t entry_itstate = (q_head == 0U && is_thumb) ? ctx->itstate : 0U;
        g_stage_pc_map_count   = 0U;
        a32_jit_record_pc_map(0U, cur_b_pc, entry_itstate);

        a64_buf_t buf;
        a64_buf_init(&buf, stage_words, A32_AOT_MAX_WORDS);
        buf.guest_start_pc = cur_b_pc;
        if (uncached) {
            buf.vfp_preheader_ineligible = true;
        }

        if (is_thumb) {
            a32_translate_t32_block(ctx, &buf, cur_b_pc);
        } else {
            a32_translate_a32_block(ctx, &buf, cur_b_pc);
        }

        if (!uncached && !buf.overflow && buf.has_self_loop &&
            !buf.vfp_preheader_ineligible && buf.vfp_preheader_count > 0U) {
            uint8_t  saved_kinds[A64_VFP_CACHE_SLOTS];
            uint8_t  saved_regs[A64_VFP_CACHE_SLOTS];
            unsigned saved_count   = buf.vfp_preheader_count;
            uint32_t saved_written = buf.vfp_preheader_written;
            memcpy(saved_kinds, buf.vfp_preheader_kinds, saved_count);
            memcpy(saved_regs, buf.vfp_preheader_regs, saved_count);

            g_stage_pc_map_count = 0U;
            a32_jit_record_pc_map(0U, cur_b_pc, entry_itstate);
            a64_buf_init(&buf, stage_words, A32_AOT_MAX_WORDS);
            buf.guest_start_pc = cur_b_pc;
            a64_buf_start_self_loop(&buf, saved_kinds, saved_regs, saved_count, saved_written);
            if (is_thumb) {
                a32_translate_t32_block(ctx, &buf, cur_b_pc);
            } else {
                a32_translate_a32_block(ctx, &buf, cur_b_pc);
            }
        }

        if (buf.overflow) {
            if (q_head == 0U) {
                fprintf(stderr, "a32rt: block buffer overflow at guest pc=0x%08x\n", pc);
                jit_lock_release();
                return NULL;
            }
            break;
        }

        size_t code_bytes = buf.count * sizeof(uint32_t);
        uint8_t *dst = a32_jit_allocate_code_space_locked(ctx, code_bytes);
        if (dst == NULL) {
            if (q_head == 0U) {
                jit_lock_release();
                return NULL;
            }
            break;
        }

        memcpy(dst, stage_words, code_bytes);
        size_t side_start = 0U;
        size_t side_count = 0U;
        a32_jit_commit_side_table_locked(dst, code_bytes, cur_b_pc, entry_itstate,
                                         &side_start, &side_count);

        if (is_mt && flush_now && g_batch_max > g_batch_min && a32_jit_flush_batch_locked(ctx) != 0) {
            jit_lock_release();
            return NULL;
        }

        uint32_t end_pc = (buf.guest_end_pc > cur_b_pc) ? buf.guest_end_pc : (cur_b_pc + 4U);
        cur_epoch = atomic_load_explicit(&g_cache_epoch, memory_order_relaxed);
        a32_cache_entry_t *res_entry = NULL;

        if (uncached && oneshot_entry != NULL) {
            a32_jit_populate_ht_entry_locked(oneshot_entry, cur_b_key, dst, cur_b_pc, end_pc,
                                             (uint32_t)code_bytes, buf.is_call, side_start, side_count,
                                             0U, stage_words, buf.count, false);
            res_entry = oneshot_entry;
        } else {
            res_entry = a32_jit_find_ht_slot_for_insert_locked(cur_b_key, cur_epoch, NULL);
            if (res_entry == NULL) {
                res_entry = &g_ht[hash_key(cur_b_key)];
            }
            a32_jit_populate_ht_entry_locked(res_entry, cur_b_key, dst, cur_b_pc, end_pc,
                                             (uint32_t)code_bytes, buf.is_call, side_start, side_count,
                                             cur_epoch, stage_words, buf.count, flush_now);
        }

        ctx->blocks_compiled++;

        if (q_head == 0U) {
            first_entry = res_entry;
            if (out_snap != NULL) {
                out_snap->code         = dst;
                out_snap->is_call      = buf.is_call;
                out_snap->guest_pc     = cur_b_pc;
                out_snap->guest_end_pc = end_pc;
                out_snap->code_bytes   = (uint32_t)code_bytes;
                out_snap->epoch        = cur_epoch;
            }
        }

        if (q_tail < max_batch) {
            uint32_t cands[A64_MAX_EXIT_SLOTS + 1U];
            unsigned n_cands = 0U;
            for (unsigned s = 0U; s < buf.exit_slot_count && s < A64_MAX_EXIT_SLOTS; s++) {
                cands[n_cands++] = buf.exit_slot_pcs[s];
            }
            if (buf.is_call && end_pc > cur_b_pc) {
                cands[n_cands++] = end_pc;
            }
            for (unsigned c = 0U; c < n_cands && q_tail < max_batch; c++) {
                uint32_t c_aligned = cands[c] & (is_thumb ? ~1U : ~3U);
                if (c_aligned < 0x1000U || c_aligned >= 0xFFFF0000U) {
                    continue;
                }
                bool seen = false;
                for (unsigned q = 0U; q < q_tail; q++) {
                    if (q_pcs[q] == c_aligned) {
                        seen = true;
                        break;
                    }
                }
                if (seen) {
                    continue;
                }
                uint32_t c_key = a32_make_key(c_aligned, is_thumb);
                if (a32_jit_find_ht_entry_locked(c_key, cur_epoch) == NULL &&
                    range_is_readonly_exec(ctx, c_aligned, is_thumb ? 2U : 4U)) {
                    q_pcs[q_tail++] = c_aligned;
                }
            }
        }
    }

    if (flush_now && g_batch_max > g_batch_min && a32_jit_flush_batch_locked(ctx) != 0) {
        jit_lock_release();
        return NULL;
    }

    if (flush_now && first_entry != NULL) {
        atomic_fetch_add_explicit(&g_jit_exec_readers, 1U, memory_order_acq_rel);
    }
    jit_lock_release();
    return first_entry;
}

static const void *compile_block_internal(a32_ctx_t *ctx, uint32_t guest_pc, bool is_thumb,
                                          uint32_t *out_code_bytes, bool flush_now)
{
    a32_cache_entry_t oneshot_entry;
    a32_block_snap_t  snap;
    a32_cache_entry_t *entry = a32_jit_lookup_or_compile_entry(ctx, guest_pc, is_thumb,
                                                               &oneshot_entry, flush_now, &snap);
    if (entry == NULL) {
        return NULL;
    }
    if (out_code_bytes != NULL) {
        *out_code_bytes = snap.code_bytes;
    }
    return snap.code;
}

const void *a32_jit_compile_block(a32_ctx_t *ctx, uint32_t pc, bool is_thumb, uint32_t *out_code_bytes)
{
    if (!ctx) {
        return NULL;
    }
    return compile_block_internal(ctx, pc, is_thumb, out_code_bytes, false);
}
