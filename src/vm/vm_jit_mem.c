/*
 * src/vm/vm_jit_mem.c — W^X code cache allocation, I-cache maintenance, and side table
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#define _GNU_SOURCE
#include "dbt/jit_priv.h"

_Atomic uint32_t     g_jit_lock = 0;
_Atomic uint32_t     g_cache_epoch = 1;
_Atomic bool         g_multithreaded = false;
_Atomic uint32_t     g_jit_exec_readers = 0;
_Atomic uint32_t     g_jit_reset_pending = 0;

uint8_t             *g_code_cache = NULL;
size_t               g_code_used  = 0U;
bool                 g_page_is_rw[A32_CACHE_PAGES];
size_t               g_first_rw_page = 0U;
uintptr_t            g_batch_min  = UINTPTR_MAX;
uintptr_t            g_batch_max  = 0U;

a32_cache_entry_t   *g_ht = NULL;
uint32_t             g_active_ht_idx[A32_HT_SIZE];
size_t               g_active_ht_count = 0U;
uint64_t             g_code_page_bitmap[16384];
a32_cache_entry_t    g_dummy_entry;
a32_pending_bucket_t g_pending_slot_callers[A32_PENDING_BUCKETS];
a32_pending_bucket_t g_pending_ret_callers[A32_PENDING_BUCKETS];

a32_side_entry_t    *g_side_table = NULL;
_Atomic size_t       g_side_table_count = 0U;

a32_pc_map_stage_t   g_stage_pc_map[MAX_STAGE_PC_MAP];
size_t               g_stage_pc_map_count = 0U;

a32_cache_entry_t *a32_jit_find_ht_slot_for_insert_locked(uint32_t key, uint32_t cur_epoch,
                                                          bool *out_exists)
{
    if (out_exists != NULL) {
        *out_exists = false;
    }
    uint32_t slot          = hash_key(key);
    a32_cache_entry_t *res = NULL;
    for (size_t probe = 0U; probe < 32U; probe++) {
        uint32_t idx   = (slot + (uint32_t)probe) & A32_HT_MASK;
        uint32_t k     = atomic_load_explicit(&g_ht[idx].key, memory_order_relaxed);
        const void *hc = atomic_load_explicit(&g_ht[idx].host_code, memory_order_relaxed);
        uint32_t ep    = atomic_load_explicit(&g_ht[idx].epoch, memory_order_relaxed);
        if (k == key && hc != NULL && ep == cur_epoch) {
            if (out_exists != NULL) {
                *out_exists = true;
            }
            return &g_ht[idx];
        }
        if (res == NULL && (k == 0U || hc == NULL || ep != cur_epoch)) {
            res = &g_ht[idx];
        }
    }
    return res;
}

a32_cache_entry_t *a32_jit_find_ht_entry_locked(uint32_t key, uint32_t cur_epoch)
{
    if (key == 0U || cur_epoch == 0U) {
        return NULL;
    }
    bool exists = false;
    a32_cache_entry_t *e = a32_jit_find_ht_slot_for_insert_locked(key, cur_epoch, &exists);
    return exists ? e : NULL;
}

static void flush_icache_range(a32_ctx_t *ctx, const void *start, size_t len)
{
    ctx->icache_flushes++;
#if defined(__APPLE__)
    sys_icache_invalidate((void *)(uintptr_t)start, len);
    return;
#else
    uintptr_t addr = (uintptr_t)start & ~(uintptr_t)63U;
    uintptr_t end  = (uintptr_t)start + len;

    if (ctx->has_host_info && (ctx->host_info.caps & A32H_CAP_ICACHE_TRAP_IALLU)) {
        /* Host (hl) invalidates entire I-cache on a single trapped IC IVAU */
        __asm__ volatile("ic ivau, %0\n\t"
                         "dsb ish\n\t"
                         "isb"
                         :
                         : "r"(addr)
                         : "memory");
        return;
    }

    for (uintptr_t p = addr; p < end; p += 64U) {
        __asm__ volatile("dc cvau, %0" : : "r"(p) : "memory");
    }
    __asm__ volatile("dsb ish" : : : "memory");
    for (uintptr_t p = addr; p < end; p += 64U) {
        __asm__ volatile("ic ivau, %0" : : "r"(p) : "memory");
    }
    __asm__ volatile("dsb ish\n\t"
                     "isb"
                     :
                     :
                     : "memory");
#endif
}

void a32_jit_reset_cache_locked(a32_ctx_t *ctx)
{
    uint32_t old_epoch = atomic_load_explicit(&g_cache_epoch, memory_order_relaxed);
    atomic_store_explicit(&g_jit_reset_pending, 1U, memory_order_seq_cst);
    atomic_store_explicit(&g_cache_epoch, 0U, memory_order_seq_cst);
    while (atomic_load_explicit(&g_jit_exec_readers, memory_order_acquire) != 0U) {
        __asm__ volatile("yield" ::: "memory");
    }
#if defined(__APPLE__)
    pthread_jit_write_protect_np(0);
#else
    if (mprotect(g_code_cache, A32_CODE_CACHE_SIZE, PROT_READ | PROT_WRITE) != 0) {
        fprintf(stderr, "a32rt: reset_cache mprotect(RW) failed: %s\n", strerror(errno));
    }
#endif
    memset(g_page_is_rw, 1, sizeof(g_page_is_rw));
    g_first_rw_page    = 0U;
    g_code_used        = 0U;
    g_batch_min        = UINTPTR_MAX;
    g_batch_max        = 0U;
    for (size_t a = 0U; a < g_active_ht_count; a++) {
        size_t i = g_active_ht_idx[a];
        clear_ht_entry_locked(&g_ht[i]);
        atomic_store_explicit(&g_ht[i].guest_pc, 0U, memory_order_relaxed);
        atomic_store_explicit(&g_ht[i].guest_end_pc, 0U, memory_order_relaxed);
        atomic_store_explicit(&g_ht[i].code_bytes, 0U, memory_order_relaxed);
        atomic_store_explicit(&g_ht[i].is_call, false, memory_order_relaxed);
        g_ht[i].in_active_list = false;
        g_ht[i].side_start     = 0U;
        g_ht[i].side_count     = 0U;
    }
    g_active_ht_count = 0U;
    memset(g_code_page_bitmap, 0, sizeof(g_code_page_bitmap));
    if (ctx != NULL) {
        memset(ctx->ibtc, 0, sizeof(ctx->ibtc));
        memset(ctx->ras, 0, sizeof(ctx->ras));
        ctx->ras_top = 0U;
    }
    memset(g_pending_slot_callers, 0, sizeof(g_pending_slot_callers));
    memset(g_pending_ret_callers, 0, sizeof(g_pending_ret_callers));
    atomic_store_explicit(&g_side_table_count, 0U, memory_order_release);
    uint32_t new_epoch = old_epoch + 1U;
    if (new_epoch == 0U) {
        new_epoch = 1U;
    }
    atomic_store_explicit(&g_cache_epoch, new_epoch, memory_order_release);
    atomic_store_explicit(&g_jit_reset_pending, 0U, memory_order_release);
}

int a32_jit_flush_batch_locked(a32_ctx_t *ctx)
{
    size_t used_pages = (g_code_used + A32_PAGE_SIZE - 1U) / A32_PAGE_SIZE;
    if (used_pages > A32_CACHE_PAGES) {
        used_pages = A32_CACHE_PAGES;
    }
    size_t p = g_first_rw_page;
    while (p < used_pages) {
        if (!g_page_is_rw[p]) {
            p++;
            continue;
        }
        size_t start_p = p;
        while (p < used_pages && g_page_is_rw[p]) {
            p++;
        }
        void  *range_start = g_code_cache + (start_p * A32_PAGE_SIZE);
        size_t range_len   = (p - start_p) * A32_PAGE_SIZE;
#if defined(__APPLE__)
        (void)range_start;
        (void)range_len;
#else
        if (mprotect(range_start, range_len, PROT_READ | PROT_EXEC) != 0) {
            fprintf(stderr, "a32rt: code cache mprotect(RX) failed: %s\n", strerror(errno));
            return -1;
        }
#endif
        for (size_t q = start_p; q < p; q++) {
            g_page_is_rw[q] = false;
        }
        ctx->wx_transitions++;
    }
    g_first_rw_page = used_pages;
#if defined(__APPLE__)
    pthread_jit_write_protect_np(1);
#endif

    if (g_batch_max > g_batch_min) {
        flush_icache_range(ctx, (const void *)g_batch_min, g_batch_max - g_batch_min);
        g_batch_min = UINTPTR_MAX;
        g_batch_max = 0U;
    }

    return 0;
}

uint8_t *a32_jit_allocate_code_space_locked(a32_ctx_t *ctx, size_t code_bytes)
{
    if (g_code_cache == NULL || code_bytes > A32_CODE_CACHE_SIZE) {
        return NULL;
    }
    if (atomic_load_explicit(&g_multithreaded, memory_order_acquire) &&
        (g_code_used & (A32_PAGE_SIZE - 1U)) != 0U) {
        size_t cur_page = g_code_used / A32_PAGE_SIZE;
        if (cur_page < A32_CACHE_PAGES && !g_page_is_rw[cur_page]) {
            g_code_used = (g_code_used + A32_PAGE_SIZE - 1U) & ~(size_t)(A32_PAGE_SIZE - 1U);
        }
    }
    if (g_code_used + code_bytes > A32_CODE_CACHE_SIZE) {
        a32_jit_reset_cache_locked(ctx);
    }

    uint8_t *dst    = g_code_cache + g_code_used;
    uintptr_t start = (uintptr_t)dst;
    uintptr_t end   = start + code_bytes;

    size_t p_start = (start - (uintptr_t)g_code_cache) / A32_PAGE_SIZE;
    size_t p_end   = (end + A32_PAGE_SIZE - 1U - (uintptr_t)g_code_cache) / A32_PAGE_SIZE;
    if (p_end > A32_CACHE_PAGES) {
        p_end = A32_CACHE_PAGES;
    }

    for (size_t p = p_start; p < p_end; p++) {
#if defined(__APPLE__)
        pthread_jit_write_protect_np(0);
        if (!g_page_is_rw[p]) {
            g_page_is_rw[p] = true;
            ctx->wx_transitions++;
        }
#else
        if (!g_page_is_rw[p]) {
            void *page_addr = g_code_cache + (p * A32_PAGE_SIZE);
            if (mprotect(page_addr, A32_PAGE_SIZE, PROT_READ | PROT_WRITE) != 0) {
                fprintf(stderr, "a32rt: code cache mprotect(RW) failed: %s\n", strerror(errno));
                return NULL;
            }
            g_page_is_rw[p] = true;
            ctx->wx_transitions++;
        }
#endif
        if (p < g_first_rw_page) {
            g_first_rw_page = p;
        }
    }

    if (start < g_batch_min) {
        g_batch_min = start;
    }
    if (end > g_batch_max) {
        g_batch_max = end;
    }

    g_code_used += (code_bytes + 15U) & ~(size_t)15U;
    return dst;
}

void a32_jit_commit_side_table_locked(const void *dst, size_t code_bytes,
                                      uint32_t block_guest_pc, uint32_t block_itstate,
                                      size_t *out_side_start, size_t *out_side_count)
{
    uintptr_t base      = (uintptr_t)dst;
    size_t    cur_count = atomic_load_explicit(&g_side_table_count, memory_order_relaxed);
    size_t    start_idx = cur_count;

    if (g_stage_pc_map_count == 0U) {
        size_t idx = cur_count++ % A32_SIDE_TABLE_SIZE;
        g_side_table[idx].host_pc_start = base;
        g_side_table[idx].host_pc_end   = base + code_bytes;
        g_side_table[idx].guest_pc      = block_guest_pc;
        g_side_table[idx].itstate       = block_itstate;
        atomic_store_explicit(&g_side_table_count, cur_count, memory_order_release);
        if (out_side_start != NULL) {
            *out_side_start = start_idx;
        }
        if (out_side_count != NULL) {
            *out_side_count = 1U;
        }
        return;
    }

    for (size_t i = 0U; i < g_stage_pc_map_count; i++) {
        uintptr_t seg_start = base + (uintptr_t)g_stage_pc_map[i].host_word_idx * 4U;
        uintptr_t seg_end;
        if (i + 1U < g_stage_pc_map_count) {
            seg_end = base + (uintptr_t)g_stage_pc_map[i + 1U].host_word_idx * 4U;
        } else {
            seg_end = base + code_bytes;
        }
        if (seg_end <= seg_start) {
            seg_end = seg_start + 4U;
        }
        size_t idx = cur_count++ % A32_SIDE_TABLE_SIZE;
        g_side_table[idx].host_pc_start = seg_start;
        g_side_table[idx].host_pc_end   = seg_end;
        g_side_table[idx].guest_pc      = g_stage_pc_map[i].guest_pc;
        g_side_table[idx].itstate       = g_stage_pc_map[i].itstate;
    }
    atomic_store_explicit(&g_side_table_count, cur_count, memory_order_release);

    if (out_side_start != NULL) {
        *out_side_start = start_idx;
    }
    if (out_side_count != NULL) {
        *out_side_count = g_stage_pc_map_count;
    }
}
