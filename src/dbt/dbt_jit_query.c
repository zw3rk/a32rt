/*
 * src/dbt/dbt_jit_query.c — JIT block lookup and lookahead compilation
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#define _GNU_SOURCE
#include "dbt/jit_priv.h"

/* Staged site publish/discard helpers (src/core/core_emit_mem.c). */
extern void a32_emit_sites_reset_block(uint32_t guest_start_pc);

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

    /* A32-IOS-P4: lockless g_ht fast path before taking jit_lock. Mirrors
     * the dbt_jit_run.c RAS-miss probe: atomic-acquire loads validated by
     * validate_cache_candidate. A lock-free reader lease pins the cache
     * against reset recycling across the probe; on a hit with an already
     * sealed batch the lease is kept as the execution lease and we return
     * without ever taking jit_lock. Anything else falls through to the
     * locked path below (which re-probes under the lock). */
    if (!uncached && key != 0U) {
        a32_jit_exec_enter();
        uint32_t pre_epoch = atomic_load_explicit(&g_cache_epoch, memory_order_acquire);
        a32_cache_entry_t *hit_entry = NULL;
        const void *hit_code = NULL;
        bool hit_is_call = false;
        uint32_t hit_start = 0U;
        uint32_t hit_end = 0U;
        if (pre_epoch != 0U) {
            uint32_t slot = hash_key(key);
            for (unsigned probe = 0U; probe < 4U; probe++) {
                a32_cache_entry_t *cand = &g_ht[(slot + probe) & A32_HT_MASK];
                const void *cand_code =
                    atomic_load_explicit(&cand->host_code, memory_order_acquire);
                const void *found_code = NULL;
                a32_cache_entry_t *found_entry = NULL;
                if (validate_cache_candidate(cand, cand_code, key, pre_epoch,
                                             &found_code, &found_entry, &hit_is_call,
                                             &hit_start, &hit_end)) {
                    hit_entry = found_entry;
                    hit_code = found_code;
                    break;
                }
            }
        }
        if (hit_entry != NULL && hit_code != NULL) {
            /* The batch flags were stored before this entry was published,
             * so an empty batch means this entry's pages are already RX
             * sealed; re-validate the epoch after reading them. */
            bool sealed = (__atomic_load_n(&g_batch_max, __ATOMIC_ACQUIRE) <=
                           __atomic_load_n(&g_batch_min, __ATOMIC_ACQUIRE));
            bool still_valid =
                (atomic_load_explicit(&g_cache_epoch, memory_order_acquire) == pre_epoch) &&
                (atomic_load_explicit(&hit_entry->epoch, memory_order_acquire) == pre_epoch) &&
                (atomic_load_explicit(&hit_entry->host_code, memory_order_relaxed) == hit_code);
            if (still_valid && (!flush_now || sealed)) {
                if (out_snap != NULL) {
                    out_snap->code = hit_code;
                    out_snap->is_call = hit_is_call;
                    out_snap->guest_pc = hit_start;
                    out_snap->guest_end_pc = hit_end;
                    out_snap->code_bytes =
                        atomic_load_explicit(&hit_entry->code_bytes, memory_order_relaxed);
                    out_snap->epoch = pre_epoch;
                }
                if (!flush_now) {
                    a32_jit_exec_leave();
                }
                return hit_entry;
            }
        }
        a32_jit_exec_leave();
    }

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
    /* A32-R13-8: drop the lock before translating; block compilation runs
     * on thread-local staging and re-locks only to commit. */
    jit_lock_release();

    uint32_t q_pcs[A32_JIT_LOOKAHEAD_MAX];
    unsigned q_head    = 0U;
    unsigned q_tail    = 1U;
    q_pcs[0]           = aligned_pc;
    bool     is_mt     = atomic_load_explicit(&g_multithreaded, memory_order_acquire);
    unsigned max_batch = (!is_mt && !uncached && flush_now &&
                          range_is_readonly_exec(ctx, aligned_pc, is_thumb ? 2U : 4U))
                             ? A32_JIT_LOOKAHEAD_MAX
                             : 1U;

    /* A32-R13-8: per-thread staging so translation runs without jit_lock. */
    static _Thread_local uint32_t stage_words[A32_AOT_MAX_WORDS];
    a32_cache_entry_t *first_entry = NULL;

    for (q_head = 0U; q_head < q_tail; q_head++) {
        uint32_t cur_b_pc  = q_pcs[q_head];
        uint32_t cur_b_key = a32_make_key(cur_b_pc, is_thumb);

        /* Locked pre-checks; translation itself runs unlocked below. */
        jit_lock_acquire();
        cur_epoch = atomic_load_explicit(&g_cache_epoch, memory_order_relaxed);

        if (q_head > 0U) {
            if (a32_jit_find_ht_entry_locked(cur_b_key, cur_epoch) != NULL) {
                jit_lock_release();
                continue;
            }
            if (g_code_used + (A32_AOT_MAX_WORDS * sizeof(uint32_t)) > A32_CODE_CACHE_SIZE ||
                g_side_table_count + MAX_STAGE_PC_MAP > A32_SIDE_TABLE_SIZE ||
                a32_jit_find_ht_slot_for_insert_locked(cur_b_key, cur_epoch, NULL) == NULL) {
                jit_lock_release();
                break;
            }
            if (!range_is_readonly_exec(ctx, cur_b_pc, is_thumb ? 2U : 4U)) {
                jit_lock_release();
                continue;
            }
        } else if (!uncached && a32_jit_find_ht_slot_for_insert_locked(cur_b_key, cur_epoch, NULL) == NULL) {
            a32_jit_reset_cache_locked(ctx);
            cur_epoch = atomic_load_explicit(&g_cache_epoch, memory_order_relaxed);
        }

        if (!a32_jit_range_executable(ctx, cur_b_pc, is_thumb ? 2U : 4U)) {
            ctx->fault_addr  = cur_b_pc;
            ctx->exit_reason = A32_EXIT_FAULT;
            jit_lock_release();
            if (q_head == 0U) {
                return NULL;
            }
            break;
        }
        uint32_t use_epoch = cur_epoch;
        jit_lock_release();

        /* A32-IOS-D4: lockless translation with epoch retry. A concurrent
         * munmap/mprotect/SMC invalidation during translation leaves the
         * staged bytes stale: drop the lock and retry under the new epoch
         * (bounded), then compile once more with the lock held as the
         * guaranteed forward-progress fallback (invalidation takes the
         * lock, so no epoch change can interleave). Staging is per-thread
         * (stage_words, site stage, pc-map stage), so retries never touch
         * another thread's in-flight block. */
        a64_buf_t buf;
        size_t code_bytes = 0U;
        bool hold_lock   = false;
        bool queue_done  = false;
        uint32_t entry_itstate = (q_head == 0U && is_thumb) ? ctx->itstate : 0U;
        for (unsigned attempt = 0U;; attempt++) {
            a32_emit_sites_reset_block(cur_b_pc);
            /* Translation into thread-local staging. */
            a32_jit_stage_reset();
            a32_jit_record_pc_map(0U, cur_b_pc, entry_itstate);

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

            bool can_promote_regs = !buf.vfp_preheader_ineligible && (buf.vfp_preheader_count > 0U);
            bool pass1_neon_only  = buf.saw_neon_fp && !buf.saw_scalar_vfp;
            if (!uncached && !buf.overflow && buf.has_self_loop &&
                (can_promote_regs || pass1_neon_only)) {
                uint8_t  saved_kinds[A64_VFP_CACHE_SLOTS];
                uint8_t  saved_regs[A64_VFP_CACHE_SLOTS];
                unsigned saved_count   = can_promote_regs ? buf.vfp_preheader_count : 0U;
                uint32_t saved_written = can_promote_regs ? buf.vfp_preheader_written : 0U;
                if (saved_count > 0U) {
                    memcpy(saved_kinds, buf.vfp_preheader_kinds, saved_count);
                    memcpy(saved_regs, buf.vfp_preheader_regs, saved_count);
                }

                a32_jit_stage_reset();
                a32_emit_sites_reset_block(cur_b_pc);
                a32_jit_record_pc_map(0U, cur_b_pc, entry_itstate);
                a64_buf_init(&buf, stage_words, A32_AOT_MAX_WORDS);
                buf.guest_start_pc = cur_b_pc;
                a64_buf_start_self_loop(&buf, saved_kinds, saved_regs, saved_count,
                                        saved_written, pass1_neon_only);
                if (is_thumb) {
                    a32_translate_t32_block(ctx, &buf, cur_b_pc);
                } else {
                    a32_translate_a32_block(ctx, &buf, cur_b_pc);
                }
            }

            if (buf.overflow) {
                a32_emit_sites_reset_block(cur_b_pc);
                if (hold_lock) {
                    jit_lock_release();
                }
                if (q_head == 0U) {
                    fprintf(stderr, "a32rt: block buffer overflow at guest pc=0x%08x\n", pc);
                    return NULL;
                }
                queue_done = true;
                break;
            }

            code_bytes = buf.count * sizeof(uint32_t);

            /* Locked commit pre-check: re-verify the epoch in case another
             * thread invalidated (or compiled this block) mid-translation. */
            if (!hold_lock) {
                jit_lock_acquire();
            }
            cur_epoch = atomic_load_explicit(&g_cache_epoch, memory_order_relaxed);
            if (cur_epoch == use_epoch) {
                break;
            }
            if (!a32_jit_range_executable(ctx, cur_b_pc, is_thumb ? 2U : 4U)) {
                a32_emit_sites_reset_block(cur_b_pc);
                ctx->fault_addr  = cur_b_pc;
                ctx->exit_reason = A32_EXIT_FAULT;
                jit_lock_release();
                if (q_head == 0U) {
                    return NULL;
                }
                queue_done = true;
                break;
            }
            if (attempt >= 3U) {
                hold_lock = true;
                use_epoch = cur_epoch;
                continue;
            }
            jit_lock_release();
            use_epoch = cur_epoch;
        }
        if (queue_done) {
            break;
        }
        if (!uncached) {
            a32_cache_entry_t *raced = a32_jit_find_ht_entry_locked(cur_b_key, cur_epoch);
            if (raced != NULL) {
                a32_emit_sites_reset_block(cur_b_pc);
                if (q_head == 0U) {
                    first_entry = raced;
                    if (out_snap != NULL) {
                        out_snap->code         = atomic_load_explicit(&raced->host_code, memory_order_relaxed);
                        out_snap->is_call      = atomic_load_explicit(&raced->is_call, memory_order_relaxed);
                        out_snap->guest_pc     = atomic_load_explicit(&raced->guest_pc, memory_order_relaxed);
                        out_snap->guest_end_pc = atomic_load_explicit(&raced->guest_end_pc, memory_order_relaxed);
                        out_snap->code_bytes   = atomic_load_explicit(&raced->code_bytes, memory_order_relaxed);
                        out_snap->epoch        = cur_epoch;
                    }
                }
                jit_lock_release();
                continue;
            }
        }
        uint8_t *dst = a32_jit_allocate_code_space_locked(ctx, code_bytes);
        if (dst == NULL) {
            a32_emit_sites_reset_block(cur_b_pc);
            jit_lock_release();
            if (q_head == 0U) {
                return NULL;
            }
            break;
        }

        memcpy(dst, stage_words, code_bytes);
        size_t side_start = 0U;
        size_t side_count = 0U;
        a32_jit_stage_publish_locked();
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
            a32_emit_sites_reset_block(cur_b_pc);
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
        jit_lock_release();
    }

    jit_lock_acquire();
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

/* Lease-bearing compile (A32-R3-6): compilation, batch sealing, and
 * execution-lease acquisition happen inside one locked operation, so the
 * returned code cannot be recycled by another thread's reset before the
 * caller runs it. The caller must pair success with a32_jit_exec_leave. */
const void *a32_jit_compile_block_leased(a32_ctx_t *ctx, uint32_t pc,
    bool is_thumb, uint32_t *out_code_bytes)
{
    if (!ctx) {
        return NULL;
    }
    return compile_block_internal(ctx, pc, is_thumb, out_code_bytes, true);
}
