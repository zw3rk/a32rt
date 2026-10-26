/*
 * src/vm/vm_jit_state.c - split-TU part 2/4 of src/jit.c (lines 489-1004).
 * Compiled only via the src/jit.c wrapper; do not add to Makefile SRCS.
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "os/backend.h"

/* Last guest PC handed to a32_jit_record_pc_map during block translation.
 * Both translators record (count, cur_pc) immediately before emitting each
 * guest instruction, so the DBT memory emitters can recover the faulting
 * instruction PC (A2 Soft-TLB miss exit) and the per-block site base
 * (AOT site tracking) without threading new parameters through the
 * frozen emitter headers. Translation runs under the JIT lock. */
static uint32_t g_last_record_pc = 0U;

uint32_t a32_jit_last_record_pc(void)
{
    return g_last_record_pc;
}

void a32_jit_record_pc_map(uint32_t host_word_idx, uint32_t guest_pc, uint32_t itstate)
{
    g_last_record_pc = guest_pc;
    if (g_stage_pc_map_count > 0U &&
        g_stage_pc_map[g_stage_pc_map_count - 1U].host_word_idx == host_word_idx) {
        g_stage_pc_map[g_stage_pc_map_count - 1U].guest_pc = guest_pc;
        g_stage_pc_map[g_stage_pc_map_count - 1U].itstate  = itstate;
        return;
    }
    if (g_stage_pc_map_count < MAX_STAGE_PC_MAP) {
        g_stage_pc_map[g_stage_pc_map_count].host_word_idx = host_word_idx;
        g_stage_pc_map[g_stage_pc_map_count].guest_pc      = guest_pc;
        g_stage_pc_map[g_stage_pc_map_count].itstate       = itstate;
        g_stage_pc_map_count++;
    }
}

bool a32_jit_lookup_host_pc(uintptr_t host_pc, uint32_t *out_guest_pc, uint32_t *out_itstate)
{
    if (g_code_cache == NULL ||
        host_pc < (uintptr_t)g_code_cache ||
        host_pc >= (uintptr_t)g_code_cache + A32_CODE_CACHE_SIZE) {
        return false;
    }
    jit_lock_acquire();
    size_t total    = g_side_table_count;
    size_t count    = (total <= A32_SIDE_TABLE_SIZE) ? total : A32_SIDE_TABLE_SIZE;
    size_t base_idx = (total <= A32_SIDE_TABLE_SIZE) ? 0U : (total % A32_SIDE_TABLE_SIZE);
    size_t lo       = 0U;
    size_t hi       = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2U;
        const a32_side_entry_t *e = &g_side_table[(base_idx + mid) % A32_SIDE_TABLE_SIZE];
        if (e->host_pc_start <= host_pc) {
            lo = mid + 1U;
        } else {
            hi = mid;
        }
    }
    if (lo > 0U) {
        const a32_side_entry_t *e = &g_side_table[(base_idx + lo - 1U) % A32_SIDE_TABLE_SIZE];
        if (host_pc >= e->host_pc_start && host_pc < e->host_pc_end) {
            if (out_guest_pc != NULL) {
                *out_guest_pc = e->guest_pc;
            }
            if (out_itstate != NULL) {
                *out_itstate = e->itstate;
            }
            jit_lock_release();
            return true;
        }
    }
    jit_lock_release();
    return false;
}

int a32_jit_flush_batch(a32_ctx_t *ctx)
{
    if (!ctx) {
        return -1;
    }
    jit_lock_acquire();
    int ret = flush_batch_locked(ctx);
    jit_lock_release();
    return ret;
}

int a32_jit_init(a32_ctx_t *ctx)
{
    if (ctx != NULL) {
        memset(ctx->ibtc, 0, sizeof(ctx->ibtc));
        memset(ctx->ras, 0, sizeof(ctx->ras));
        ctx->ras_top = 0U;
    }
    if (g_code_cache != NULL) {
        jit_lock_acquire();
        if (!atomic_load_explicit(&g_multithreaded, memory_order_relaxed)) {
            atomic_store_explicit(&g_multithreaded, true, memory_order_release);
            if (ctx != NULL && g_batch_max > g_batch_min) {
                (void)flush_batch_locked(ctx);
            }
            g_code_used = (g_code_used + A32_PAGE_SIZE - 1U) & ~(size_t)(A32_PAGE_SIZE - 1U);
        } else if (ctx != NULL && g_batch_max > g_batch_min) {
            (void)flush_batch_locked(ctx);
        }
        jit_lock_release();
        return 0;
    }
    /* Route the executable code-cache allocation through the host
     * backend so Darwin uses MAP_JIT/APRR publication while Linux keeps
     * a plain RW mapping (sealed RX later by the W^X batch path). */
    void *mem = a32_backend_jit_alloc(A32_CODE_CACHE_SIZE);
    if (mem == NULL) {
        fprintf(stderr, "a32rt: failed to allocate code cache: %s\n", strerror(errno));
        return -1;
    }
    size_t ht_bytes = ((size_t)A32_HT_SIZE * sizeof(a32_cache_entry_t) + A32_PAGE_SIZE - 1U) &
                      ~(size_t)(A32_PAGE_SIZE - 1U);
    void *ht_mem = mmap(NULL, ht_bytes, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ht_mem == MAP_FAILED) {
        fprintf(stderr, "a32rt: failed to allocate JIT hash table: %s\n", strerror(errno));
        return -1;
    }
    size_t st_bytes = ((size_t)A32_SIDE_TABLE_SIZE * sizeof(a32_side_entry_t) + A32_PAGE_SIZE - 1U) &
                      ~(size_t)(A32_PAGE_SIZE - 1U);
    void *st_mem = mmap(NULL, st_bytes, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (st_mem == MAP_FAILED) {
        fprintf(stderr, "a32rt: failed to allocate JIT side table: %s\n", strerror(errno));
        return -1;
    }
    g_code_cache       = (uint8_t *)mem;
    g_ht               = (a32_cache_entry_t *)ht_mem;
    g_side_table       = (a32_side_entry_t *)st_mem;
    g_code_used        = 0U;
    memset(g_page_is_rw, 1, sizeof(g_page_is_rw));
    g_first_rw_page    = 0U;
    g_batch_min        = UINTPTR_MAX;
    g_batch_max        = 0U;
    g_active_ht_count  = 0U;
    g_side_table_count = 0U;
    return 0;
}

int a32_jit_invalidate_range(a32_ctx_t *ctx, uint32_t start, uint32_t end)
{
    start &= ~1U;
    if (end <= start) {
        end = (start <= 0xFFFFFFFCU) ? (start + 4U) : 0xFFFFFFFFU;
    }

    jit_lock_acquire();
    if (g_ht == NULL || g_active_ht_count == 0U) {
        jit_lock_release();
        return 0;
    }

    uint32_t p_first = (start >= 512U ? (start - 512U) : 0U) >> 12U;
    uint32_t p_last  = (end - 1U) >> 12U;
    bool page_hit    = false;
    for (uint32_t p = p_first; p <= p_last; p++) {
        if ((g_code_page_bitmap[p >> 6U] & (1ULL << (p & 63U))) != 0ULL) {
            page_hit = true;
            break;
        }
    }
    if (!page_hit) {
        jit_lock_release();
        return 0;
    }

    uint32_t old_epoch = atomic_load_explicit(&g_cache_epoch, memory_order_relaxed);
    bool any_overlap   = false;
    for (size_t a = 0U; a < g_active_ht_count; a++) {
        size_t i       = g_active_ht_idx[a];
        uint32_t k     = atomic_load_explicit(&g_ht[i].key, memory_order_relaxed);
        const void *hc = atomic_load_explicit(&g_ht[i].host_code, memory_order_relaxed);
        if (k != 0U && hc != NULL &&
            atomic_load_explicit(&g_ht[i].epoch, memory_order_relaxed) == old_epoch) {
            uint32_t b_start = atomic_load_explicit(&g_ht[i].guest_pc, memory_order_relaxed);
            uint32_t b_end_r = atomic_load_explicit(&g_ht[i].guest_end_pc, memory_order_relaxed);
            uint32_t b_end   = (b_end_r > b_start) ? b_end_r : (b_start + 4U);
            if (b_start < end && b_end > start) {
                any_overlap = true;
                break;
            }
        }
    }
    if (!any_overlap) {
        jit_lock_release();
        return 0;
    }

    if (ctx != NULL) {
        memset(ctx->ibtc, 0, sizeof(ctx->ibtc));
        memset(ctx->ras, 0, sizeof(ctx->ras));
    }

    uint32_t new_epoch = old_epoch + 1U;
    if (new_epoch == 0U) {
        new_epoch = 1U;
    }
    atomic_store_explicit(&g_cache_epoch, 0U, memory_order_release);
    for (size_t a = 0U; a < g_active_ht_count; a++) {
        size_t i       = g_active_ht_idx[a];
        uint32_t k     = atomic_load_explicit(&g_ht[i].key, memory_order_relaxed);
        const void *hc = atomic_load_explicit(&g_ht[i].host_code, memory_order_relaxed);
        if (k != 0U && hc != NULL &&
            atomic_load_explicit(&g_ht[i].epoch, memory_order_relaxed) == old_epoch) {
            uint32_t b_start = atomic_load_explicit(&g_ht[i].guest_pc, memory_order_relaxed);
            uint32_t b_end_r = atomic_load_explicit(&g_ht[i].guest_end_pc, memory_order_relaxed);
            uint32_t b_end   = (b_end_r > b_start) ? b_end_r : (b_start + 4U);
            if (b_start < end && b_end > start) {
                clear_ht_entry_locked(&g_ht[i]);
            } else {
                atomic_store_explicit(&g_ht[i].epoch, new_epoch, memory_order_release);
            }
        }
    }
    for (size_t a = 0U; a < g_active_ht_count; a++) {
        size_t i = g_active_ht_idx[a];
        if (atomic_load_explicit(&g_ht[i].epoch, memory_order_relaxed) != new_epoch) {
            continue;
        }
        for (size_t c = 0U; c < 2U; c++) {
            uint32_t ce_epoch     = atomic_load_explicit(&g_ht[i].chain_epoch[c], memory_order_relaxed);
            uint32_t ce_key       = atomic_load_explicit(&g_ht[i].chain_key[c], memory_order_relaxed);
            const void *ce_code   = atomic_load_explicit(&g_ht[i].chain_code[c], memory_order_relaxed);
            a32_cache_entry_t *ce = atomic_load_explicit(&g_ht[i].chain_entry[c], memory_order_relaxed);
            if (ce != NULL && ce_code != NULL && ce_epoch == old_epoch &&
                atomic_load_explicit(&ce->epoch, memory_order_relaxed) == new_epoch &&
                atomic_load_explicit(&ce->host_code, memory_order_relaxed) == ce_code &&
                atomic_load_explicit(&ce->key, memory_order_relaxed) == ce_key) {
                atomic_store_explicit(&g_ht[i].chain_epoch[c], new_epoch, memory_order_release);
            } else {
                clear_ht_chain_slot_locked(&g_ht[i], c);
            }
        }
        uint32_t re_epoch     = atomic_load_explicit(&g_ht[i].ret_epoch, memory_order_relaxed);
        uint32_t re_key       = atomic_load_explicit(&g_ht[i].ret_key, memory_order_relaxed);
        const void *re_code   = atomic_load_explicit(&g_ht[i].ret_code, memory_order_relaxed);
        a32_cache_entry_t *re = atomic_load_explicit(&g_ht[i].ret_entry, memory_order_relaxed);
        if (re != NULL && re_code != NULL && re_epoch == old_epoch &&
            atomic_load_explicit(&re->epoch, memory_order_relaxed) == new_epoch &&
            atomic_load_explicit(&re->host_code, memory_order_relaxed) == re_code &&
            atomic_load_explicit(&re->key, memory_order_relaxed) == re_key) {
            atomic_store_explicit(&g_ht[i].ret_epoch, new_epoch, memory_order_release);
        } else {
            clear_ht_ret_slot_locked(&g_ht[i]);
        }
        for (size_t s = 0U; s < A32_MAX_EXIT_SLOTS; s++) {
            a32_cache_entry_t *se = atomic_load_explicit(&g_ht[i].slot_entry[s], memory_order_relaxed);
            if (se != NULL) {
                uint32_t expected_key = atomic_load_explicit(&g_ht[i].slot_key[s], memory_order_relaxed);
                if (atomic_load_explicit(&se->epoch, memory_order_relaxed) != new_epoch ||
                    atomic_load_explicit(&se->key, memory_order_relaxed) != expected_key) {
                    atomic_store_explicit(&g_ht[i].slot_entry[s], NULL, memory_order_release);
                }
            }
        }
    }
    atomic_store_explicit(&g_cache_epoch, new_epoch, memory_order_release);
    jit_lock_release();
    return 0;
}

static inline void link_entry_chain_slot_locked(a32_cache_entry_t *caller, uint32_t s_key,
                                                const void *target_code, a32_cache_entry_t *target,
                                                uint32_t cur_epoch)
{
    if (atomic_load_explicit(&caller->chain_entry[0], memory_order_relaxed) == NULL) {
        set_ht_chain_slot_locked(caller, 0U, s_key, target_code, target, cur_epoch);
    } else if (atomic_load_explicit(&caller->chain_entry[1], memory_order_relaxed) == NULL &&
               atomic_load_explicit(&caller->chain_key[0], memory_order_relaxed) != s_key) {
        set_ht_chain_slot_locked(caller, 1U, s_key, target_code, target, cur_epoch);
    }
}

static void populate_ht_entry_locked(a32_cache_entry_t *entry, uint32_t key, const void *dst,
                                     uint32_t guest_pc, uint32_t guest_end_pc, uint32_t code_bytes,
                                     bool is_call, size_t side_start, size_t side_count,
                                     uint32_t cur_epoch,
                                     const uint32_t *words, size_t word_count,
                                     bool link_reverse)
{
    uint32_t old_ep  = atomic_load_explicit(&entry->epoch, memory_order_relaxed);
    uint32_t old_key = atomic_load_explicit(&entry->key, memory_order_relaxed);
    if (cur_epoch != 0U && old_ep == cur_epoch && old_key != 0U && old_key != key) {
        /* Evicting a live entry with a different key: unlink any incoming references */
        for (size_t a = 0U; a < g_active_ht_count; a++) {
            size_t i = g_active_ht_idx[a];
            if (atomic_load_explicit(&g_ht[i].epoch, memory_order_relaxed) != cur_epoch) {
                continue;
            }
            for (size_t s = 0U; s < A32_MAX_EXIT_SLOTS; s++) {
                if (atomic_load_explicit(&g_ht[i].slot_entry[s], memory_order_relaxed) == entry) {
                    atomic_store_explicit(&g_ht[i].slot_entry[s], NULL, memory_order_release);
                }
            }
            for (size_t c = 0U; c < 2U; c++) {
                if (atomic_load_explicit(&g_ht[i].chain_entry[c], memory_order_relaxed) == entry) {
                    clear_ht_chain_slot_locked(&g_ht[i], c);
                }
            }
            if (atomic_load_explicit(&g_ht[i].ret_entry, memory_order_relaxed) == entry) {
                clear_ht_ret_slot_locked(&g_ht[i]);
            }
        }
    }
    clear_ht_entry_locked(entry);
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&entry->guest_pc, guest_pc, memory_order_relaxed);
    atomic_store_explicit(&entry->guest_end_pc, guest_end_pc, memory_order_relaxed);
    atomic_store_explicit(&entry->code_bytes, code_bytes, memory_order_relaxed);
    atomic_store_explicit(&entry->is_call, is_call, memory_order_relaxed);
    entry->side_start = side_start;
    entry->side_count = side_count;
    if (cur_epoch != 0U && g_ht != NULL &&
        entry >= g_ht && entry < g_ht + A32_HT_SIZE) {
        if (!entry->in_active_list && g_active_ht_count < A32_HT_SIZE) {
            entry->in_active_list = true;
            g_active_ht_idx[g_active_ht_count++] = (uint32_t)(entry - g_ht);
        }
        uint32_t p0 = guest_pc >> 12U;
        uint32_t p1 = ((guest_end_pc > guest_pc) ? (guest_end_pc - 1U) : guest_pc) >> 12U;
        g_code_page_bitmap[p0 >> 6U] |= (1ULL << (p0 & 63U));
        g_code_page_bitmap[p1 >> 6U] |= (1ULL << (p1 & 63U));
    }

    if (words != NULL && word_count > 0U) {
        for (size_t i = 0U; i + 9U < word_count; i++) {
            if ((words[i] & 0xFFC003FFU) == (0xF9400000U | (27U << 5) | 23U)) {
                unsigned uimm = (words[i] >> 10U) & 0xFFFU;
                if (uimm >= 19U && uimm < 19U + A32_MAX_EXIT_SLOTS) {
                    size_t off_movz = (i + 9U < word_count &&
                                       (words[i + 9U] & 0xFFE0001FU) == (0x52A00000U | 15U))
                                          ? (i + 9U)
                                          : (i + 17U);
                    if (off_movz + 1U < word_count &&
                        (words[off_movz] & 0xFFE0001FU) == (0x52A00000U | 15U) &&
                        (words[off_movz + 1U] & 0xFFE0001FU) == (0x72800000U | 15U)) {
                        uint32_t target_pc = (((words[off_movz] >> 5U) & 0xFFFFU) << 16U) |
                                             ((words[off_movz + 1U] >> 5U) & 0xFFFFU);
                        uint32_t is_t = key & 1U;
                        uint32_t s_key = (target_pc & (is_t ? ~1U : ~3U)) | is_t;
                        unsigned s_idx_slot = uimm - 19U;
                        atomic_store_explicit(&entry->slot_key[s_idx_slot], s_key, memory_order_relaxed);
                        if (cur_epoch != 0U && s_key != 0U) {
                            if (s_key == key) {
                                atomic_store_explicit(&entry->slot_entry[s_idx_slot], entry, memory_order_relaxed);
                            } else {
                                a32_cache_entry_t *se = find_ht_entry_locked(s_key, cur_epoch);
                                if (se != NULL && se != entry) {
                                    atomic_store_explicit(&entry->slot_entry[s_idx_slot], se, memory_order_relaxed);
                                    link_entry_chain_slot_locked(entry, s_key,
                                                                 atomic_load_explicit(&se->host_code, memory_order_relaxed),
                                                                 se, cur_epoch);
                                } else {
                                    record_pending_caller_locked(g_pending_slot_callers, s_key, entry);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    if (is_call && guest_end_pc > guest_pc) {
        uint32_t is_t = key & 1U;
        uint32_t r_key = (guest_end_pc & (is_t ? ~1U : ~3U)) | is_t;
        atomic_store_explicit(&entry->ret_key, r_key, memory_order_relaxed);
        a32_cache_entry_t *re = find_ht_entry_locked(r_key, cur_epoch);
        if (re != NULL && re != entry) {
            set_ht_ret_slot_locked(entry, r_key,
                                   atomic_load_explicit(&re->host_code, memory_order_relaxed),
                                   re, cur_epoch);
        } else if (cur_epoch != 0U && r_key != 0U) {
            record_pending_caller_locked(g_pending_ret_callers, r_key, entry);
        }
    }

    (void)link_reverse;
    if (cur_epoch != 0U && key != 0U) {
        uint32_t p_idx = hash_key(key) & A32_PENDING_MASK;
        a32_pending_bucket_t *sb = &g_pending_slot_callers[p_idx];
        for (unsigned w = 0U; w < A32_PENDING_WAYS; w++) {
            a32_cache_entry_t *ce = sb->callers[w];
            if (ce == NULL || ce == entry ||
                atomic_load_explicit(&ce->epoch, memory_order_relaxed) != cur_epoch) {
                continue;
            }
            for (unsigned s = 0U; s < A32_MAX_EXIT_SLOTS; s++) {
                if (atomic_load_explicit(&ce->slot_key[s], memory_order_relaxed) == key &&
                    atomic_load_explicit(&ce->slot_entry[s], memory_order_relaxed) == NULL) {
                    atomic_store_explicit(&ce->slot_entry[s], entry, memory_order_release);
                    link_entry_chain_slot_locked(ce, key, dst, entry, cur_epoch);
                }
            }
        }
        a32_pending_bucket_t *rb = &g_pending_ret_callers[p_idx];
        for (unsigned w = 0U; w < A32_PENDING_WAYS; w++) {
            a32_cache_entry_t *ce = rb->callers[w];
            if (ce == NULL || ce == entry ||
                atomic_load_explicit(&ce->epoch, memory_order_relaxed) != cur_epoch) {
                continue;
            }
            if (atomic_load_explicit(&ce->is_call, memory_order_relaxed) &&
                atomic_load_explicit(&ce->ret_key, memory_order_relaxed) == key &&
                atomic_load_explicit(&ce->ret_entry, memory_order_relaxed) == NULL) {
                set_ht_ret_slot_locked(ce, key, dst, entry, cur_epoch);
                rb->callers[w] = NULL;
            }
        }
    }

    atomic_store_explicit(&entry->key, key, memory_order_release);
    atomic_store_explicit(&entry->host_code, dst, memory_order_release);
    atomic_store_explicit(&entry->epoch, cur_epoch, memory_order_release);
}

int a32_jit_install_aot_block(a32_ctx_t *ctx, uint32_t guest_pc, uint32_t guest_end_pc,
                              bool is_thumb, bool is_call,
                              const uint32_t *words, size_t word_count,
                              const a32_pc_map_stage_t *pc_map, size_t pc_map_count)
{
    if (!ctx) {
        return -1;
    }
    uint32_t align_mask = is_thumb ? 1U : 3U;
    if (words == NULL || word_count == 0U || word_count > A32_AOT_MAX_WORDS ||
        pc_map_count >= MAX_STAGE_PC_MAP ||
        (pc_map_count > 0U && pc_map == NULL) ||
        (guest_pc & align_mask) != 0U ||
        (guest_end_pc & align_mask) != 0U ||
        guest_end_pc <= guest_pc ||
        guest_end_pc - guest_pc > 512U ||
        words[word_count - 1U] != 0xD65F03C0U /* RET */) {
        return -1;
    }

    uint32_t aligned_pc = guest_pc;
    uint32_t end_pc     = guest_end_pc;

    if (pc_map != NULL && pc_map_count > 0U) {
        for (size_t i = 0U; i < pc_map_count; i++) {
            uint32_t it = pc_map[i].itstate;
            if (pc_map[i].host_word_idx >= word_count ||
                (i > 0U && pc_map[i].host_word_idx < pc_map[i - 1U].host_word_idx) ||
                pc_map[i].guest_pc < aligned_pc ||
                pc_map[i].guest_pc >= end_pc ||
                (pc_map[i].guest_pc & align_mask) != 0U ||
                (!is_thumb && it != 0U) ||
                (is_thumb && ((it & ~0xFFU) != 0U ||
                              (it != 0U && (it & 0x0FU) == 0U) ||
                              (it & 0xF0U) == 0xF0U))) {
                return -1;
            }
        }
    }

    uint32_t key = a32_make_key(aligned_pc, is_thumb);

    jit_lock_acquire();
    uint32_t cur_epoch = atomic_load_explicit(&g_cache_epoch, memory_order_relaxed);
    bool     exists    = false;
    if (find_ht_slot_for_insert_locked(key, cur_epoch, &exists) == NULL || exists) {
        jit_lock_release();
        return 0;
    }

    size_t code_bytes = word_count * sizeof(uint32_t);
    uint8_t *dst = allocate_code_space_locked(ctx, code_bytes);
    if (dst == NULL) {
        jit_lock_release();
        return -1;
    }

    memcpy(dst, words, code_bytes);

    g_stage_pc_map_count = 0U;
    a32_jit_record_pc_map(0U, aligned_pc, 0U);
    if (pc_map != NULL && pc_map_count > 0U) {
        for (size_t i = 0U; i < pc_map_count; i++) {
            a32_jit_record_pc_map(pc_map[i].host_word_idx, pc_map[i].guest_pc, pc_map[i].itstate);
        }
    }
    size_t side_start = 0U;
    size_t side_count = 0U;
    commit_side_table_locked(dst, code_bytes, aligned_pc, 0U, &side_start, &side_count);

    if (atomic_load_explicit(&g_multithreaded, memory_order_acquire) && g_batch_max > g_batch_min) {
        if (flush_batch_locked(ctx) != 0) {
            jit_lock_release();
            return -1;
        }
    }

    cur_epoch = atomic_load_explicit(&g_cache_epoch, memory_order_relaxed);
    a32_cache_entry_t *target_slot = find_ht_slot_for_insert_locked(key, cur_epoch, NULL);
    if (target_slot != NULL) {
        populate_ht_entry_locked(target_slot, key, dst, aligned_pc, end_pc,
                                 (uint32_t)code_bytes, is_call, side_start, side_count,
                                 cur_epoch, words, word_count, false);
    }

    jit_lock_release();
    return 0;
}

bool a32_jit_range_executable(const a32_ctx_t *ctx, uint32_t addr, uint32_t len)
{
    if (ctx == NULL || len == 0U || addr + len < addr) {
        return false;
    }
    uint32_t end = addr + len;
    for (unsigned attempt = 0U; attempt < 2U; attempt++) {
        uint32_t cur = addr;
        for (size_t i = 0U; i < ctx->region_count && cur < end; i++) {
            const a32_region_t *r = &ctx->regions[i];
            if (r->end <= cur) {
                continue;
            }
            if (r->start > cur || (r->prot & 0x4U) == 0U) {
                break;
            }
            cur = (r->end < end) ? r->end : end;
        }
        if (cur >= end) {
            return true;
        }
        if (attempt == 0U) {
            a32_vm_sync_ctx((a32_ctx_t *)ctx);
        }
    }
    return false;
}
