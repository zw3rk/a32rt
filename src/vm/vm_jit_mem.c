/*
 * src/vm/vm_jit_mem.c - split-TU part 1/4 of src/jit.c (lines 1-488).
 * Compiled only via the src/jit.c wrapper; do not add to Makefile SRCS.
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * src/jit.c — W^X code cache, I-cache maintenance, chaining, IBTC, and execution dispatcher
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#define _GNU_SOURCE
#include "a64_emit.h"
#include "a32rt.h"

#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#if defined(__APPLE__)
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#endif

#define A32_CODE_CACHE_SIZE (32U * 1024U * 1024U)                 /* 32 MiB RX code cache */
#define A32_CACHE_PAGES     (A32_CODE_CACHE_SIZE / A32_PAGE_SIZE) /* 8192 pages */
#define A32_HT_SIZE         131072U                               /* Must be a power of 2 */
#define A32_HT_MASK         (A32_HT_SIZE - 1U)
#define A32_SIDE_TABLE_SIZE 262144U
#define MAX_STAGE_PC_MAP    256U
#define A32_AOT_MAX_WORDS   2048U
#define A32_MAX_EXIT_SLOTS  8U

typedef struct a32_cache_entry {
    _Atomic uint32_t                 key;
    _Atomic uint32_t                 epoch;
    const void *_Atomic              host_code;
    _Atomic uint32_t                 chain_key[2];
    _Atomic uint32_t                 chain_epoch[2];
    const void *_Atomic              chain_code[2];
    struct a32_cache_entry *_Atomic  chain_entry[2];
    _Atomic uint32_t                 guest_pc;
    _Atomic uint32_t                 guest_end_pc;
    _Atomic uint32_t                 code_bytes;
    _Atomic bool                     is_call;
    bool                             in_active_list;
    uint8_t                          pad_call[2];
    _Atomic uint32_t                 ret_key;
    _Atomic uint32_t                 ret_epoch;
    const void *_Atomic              ret_code;
    struct a32_cache_entry *_Atomic  ret_entry;
    size_t                           side_start;
    size_t                           side_count;
    _Atomic uint32_t                 slot_key[A32_MAX_EXIT_SLOTS];
    struct a32_cache_entry *_Atomic  slot_entry[A32_MAX_EXIT_SLOTS];
} a32_cache_entry_t;

_Static_assert(offsetof(a32_cache_entry_t, key) == 0, "key at 0");
_Static_assert(offsetof(a32_cache_entry_t, epoch) == 4, "epoch at 4");
_Static_assert(offsetof(a32_cache_entry_t, host_code) == 8, "host_code at 8");
_Static_assert(offsetof(a32_cache_entry_t, chain_key) == 16, "chain_key at 16");
_Static_assert(offsetof(a32_cache_entry_t, chain_epoch) == 24, "chain_epoch at 24");
_Static_assert(offsetof(a32_cache_entry_t, chain_code) == 32, "chain_code at 32");
_Static_assert(offsetof(a32_cache_entry_t, chain_entry) == 48, "chain_entry at 48");
_Static_assert(offsetof(a32_cache_entry_t, is_call) == 76, "is_call at 76");
_Static_assert(offsetof(a32_cache_entry_t, ret_key) == 80, "ret_key at 80");
_Static_assert(offsetof(a32_cache_entry_t, ret_epoch) == 84, "ret_epoch at 84");
_Static_assert(offsetof(a32_cache_entry_t, ret_code) == 88, "ret_code at 88");
_Static_assert(offsetof(a32_cache_entry_t, ret_entry) == 96, "ret_entry at 96");
_Static_assert(offsetof(a32_cache_entry_t, slot_key) == 120, "slot_key offset must be 120");
_Static_assert(offsetof(a32_cache_entry_t, slot_entry) == 152, "slot_entry offset must be 152");

typedef struct a32_block_snap {
    const void *code;
    bool        is_call;
    uint32_t    guest_pc;
    uint32_t    guest_end_pc;
    uint32_t    code_bytes;
    uint32_t    epoch;
} a32_block_snap_t;

typedef struct a32_side_entry {
    uintptr_t host_pc_start;
    uintptr_t host_pc_end;
    uint32_t  guest_pc;
    uint32_t  itstate;
} a32_side_entry_t;

static _Atomic uint32_t   g_jit_lock = 0;
static _Atomic uint32_t   g_cache_epoch = 1;
static _Atomic bool       g_multithreaded = false;

static uint8_t           *g_code_cache = NULL;
static size_t             g_code_used  = 0U;
static bool               g_page_is_rw[A32_CACHE_PAGES];
static uintptr_t          g_batch_min  = UINTPTR_MAX;
static uintptr_t          g_batch_max  = 0U;

#define A32_PENDING_BUCKETS 4096U
#define A32_PENDING_MASK    (A32_PENDING_BUCKETS - 1U)
#define A32_PENDING_WAYS    4U

typedef struct a32_pending_bucket {
    a32_cache_entry_t *callers[A32_PENDING_WAYS];
    uint8_t            next_way;
} a32_pending_bucket_t;

static a32_cache_entry_t   *g_ht = NULL;
static uint32_t             g_active_ht_idx[A32_HT_SIZE];
static size_t               g_active_ht_count = 0U;
static uint64_t             g_code_page_bitmap[16384]; /* 1 bit per 4 KiB guest page in 4 GiB */
static a32_cache_entry_t    g_dummy_entry; /* Zeroed sentinel for x27 on uncached blocks */
static a32_pending_bucket_t g_pending_slot_callers[A32_PENDING_BUCKETS];
static a32_pending_bucket_t g_pending_ret_callers[A32_PENDING_BUCKETS];

static a32_side_entry_t  *g_side_table = NULL;
static size_t             g_side_table_count = 0U;

static a32_pc_map_stage_t g_stage_pc_map[MAX_STAGE_PC_MAP];
static size_t             g_stage_pc_map_count = 0U;

static inline void jit_lock_acquire(void)
{
    a32_spin_lock(&g_jit_lock);
}

static inline void jit_lock_release(void)
{
    a32_spin_unlock(&g_jit_lock);
}

static uint32_t hash_key(uint32_t key)
{
    key ^= key >> 16;
    key *= 0x7feb352dU;
    key ^= key >> 15;
    key *= 0x846ca68bU;
    key ^= key >> 16;
    return key & A32_HT_MASK;
}

static inline void record_pending_caller_locked(a32_pending_bucket_t *table,
                                                uint32_t target_key,
                                                a32_cache_entry_t *caller)
{
    a32_pending_bucket_t *b = &table[hash_key(target_key) & A32_PENDING_MASK];
    for (unsigned w = 0U; w < A32_PENDING_WAYS; w++) {
        if (b->callers[w] == caller) {
            return;
        }
        if (b->callers[w] == NULL) {
            b->callers[w] = caller;
            return;
        }
    }
    unsigned victim = b->next_way & (A32_PENDING_WAYS - 1U);
    b->next_way = (uint8_t)(victim + 1U);
    b->callers[victim] = caller;
}

static inline void clear_ht_chain_slot_locked(a32_cache_entry_t *e, size_t c)
{
    atomic_store_explicit(&e->chain_epoch[c], 0U, memory_order_release);
    atomic_store_explicit(&e->chain_entry[c], NULL, memory_order_release);
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&e->chain_key[c], 0U, memory_order_relaxed);
    atomic_store_explicit(&e->chain_code[c], NULL, memory_order_relaxed);
}

static inline void set_ht_chain_slot_locked(a32_cache_entry_t *e, size_t c,
                                            uint32_t key, const void *code,
                                            a32_cache_entry_t *target, uint32_t epoch)
{
    atomic_store_explicit(&e->chain_epoch[c], 0U, memory_order_release);
    atomic_store_explicit(&e->chain_entry[c], NULL, memory_order_release);
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&e->chain_key[c], key, memory_order_relaxed);
    atomic_store_explicit(&e->chain_code[c], code, memory_order_relaxed);
    atomic_store_explicit(&e->chain_entry[c], target, memory_order_release);
    atomic_store_explicit(&e->chain_epoch[c], epoch, memory_order_release);
}

static inline void clear_ht_ret_slot_locked(a32_cache_entry_t *e)
{
    atomic_store_explicit(&e->ret_epoch, 0U, memory_order_release);
    atomic_store_explicit(&e->ret_key, 0U, memory_order_relaxed);
    atomic_store_explicit(&e->ret_code, NULL, memory_order_relaxed);
    atomic_store_explicit(&e->ret_entry, NULL, memory_order_relaxed);
}

static inline void set_ht_ret_slot_locked(a32_cache_entry_t *e, uint32_t key,
                                          const void *code, a32_cache_entry_t *target,
                                          uint32_t epoch)
{
    atomic_store_explicit(&e->ret_epoch, 0U, memory_order_release);
    atomic_store_explicit(&e->ret_key, key, memory_order_relaxed);
    atomic_store_explicit(&e->ret_code, code, memory_order_relaxed);
    atomic_store_explicit(&e->ret_entry, target, memory_order_relaxed);
    atomic_store_explicit(&e->ret_epoch, epoch, memory_order_release);
}

static inline void clear_ht_entry_locked(a32_cache_entry_t *e)
{
    atomic_store_explicit(&e->epoch, 0U, memory_order_release);
    atomic_store_explicit(&e->host_code, NULL, memory_order_release);
    atomic_store_explicit(&e->key, 0U, memory_order_relaxed);
    clear_ht_chain_slot_locked(e, 0U);
    clear_ht_chain_slot_locked(e, 1U);
    clear_ht_ret_slot_locked(e);
    for (size_t s = 0U; s < A32_MAX_EXIT_SLOTS; s++) {
        atomic_store_explicit(&e->slot_key[s], 0U, memory_order_relaxed);
        atomic_store_explicit(&e->slot_entry[s], NULL, memory_order_relaxed);
    }
}

static a32_cache_entry_t *find_ht_slot_for_insert_locked(uint32_t key, uint32_t cur_epoch,
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

static a32_cache_entry_t *find_ht_entry_locked(uint32_t key, uint32_t cur_epoch)
{
    if (key == 0U || cur_epoch == 0U) {
        return NULL;
    }
    bool exists = false;
    a32_cache_entry_t *e = find_ht_slot_for_insert_locked(key, cur_epoch, &exists);
    return exists ? e : NULL;
}

static inline bool validate_cache_candidate(a32_cache_entry_t *cand, const void *cand_code,
                                            uint32_t key, uint32_t epoch,
                                            const void **out_code, a32_cache_entry_t **out_entry,
                                            bool *out_is_call, uint32_t *out_start, uint32_t *out_end)
{
    if (cand == NULL || cand_code == NULL) {
        return false;
    }
    if (atomic_load_explicit(&cand->epoch, memory_order_acquire) != epoch ||
        atomic_load_explicit(&cand->key, memory_order_acquire) != key ||
        atomic_load_explicit(&cand->host_code, memory_order_acquire) != cand_code) {
        return false;
    }
    bool is_call = atomic_load_explicit(&cand->is_call, memory_order_relaxed);
    uint32_t start = atomic_load_explicit(&cand->guest_pc, memory_order_relaxed);
    uint32_t end = atomic_load_explicit(&cand->guest_end_pc, memory_order_relaxed);
    atomic_thread_fence(memory_order_acquire);
    if (atomic_load_explicit(&g_cache_epoch, memory_order_acquire) != epoch ||
        atomic_load_explicit(&cand->epoch, memory_order_acquire) != epoch ||
        atomic_load_explicit(&cand->host_code, memory_order_relaxed) != cand_code) {
        return false;
    }
    *out_code    = cand_code;
    *out_entry   = cand;
    *out_is_call = is_call;
    *out_start   = start;
    *out_end     = end;
    return true;
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

static size_t g_first_rw_page = 0U;

static void reset_cache_locked(a32_ctx_t *ctx)
{
    uint32_t old_epoch = atomic_load_explicit(&g_cache_epoch, memory_order_relaxed);
    atomic_store_explicit(&g_cache_epoch, 0U, memory_order_release);
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
    g_side_table_count = 0U;
    uint32_t new_epoch = old_epoch + 1U;
    if (new_epoch == 0U) {
        new_epoch = 1U;
    }
    atomic_store_explicit(&g_cache_epoch, new_epoch, memory_order_release);
}

static int flush_batch_locked(a32_ctx_t *ctx)
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

static uint8_t *allocate_code_space_locked(a32_ctx_t *ctx, size_t code_bytes)
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
        reset_cache_locked(ctx);
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

static void commit_side_table_locked(const void *dst, size_t code_bytes,
                                     uint32_t block_guest_pc, uint32_t block_itstate,
                                     size_t *out_side_start, size_t *out_side_count)
{
    uintptr_t base      = (uintptr_t)dst;
    size_t    start_idx = g_side_table_count;

    if (g_stage_pc_map_count == 0U) {
        size_t idx = g_side_table_count++ % A32_SIDE_TABLE_SIZE;
        g_side_table[idx].host_pc_start = base;
        g_side_table[idx].host_pc_end   = base + code_bytes;
        g_side_table[idx].guest_pc      = block_guest_pc;
        g_side_table[idx].itstate       = block_itstate;
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
        size_t idx = g_side_table_count++ % A32_SIDE_TABLE_SIZE;
        g_side_table[idx].host_pc_start = seg_start;
        g_side_table[idx].host_pc_end   = seg_end;
        g_side_table[idx].guest_pc      = g_stage_pc_map[i].guest_pc;
        g_side_table[idx].itstate       = g_stage_pc_map[i].itstate;
    }

    if (out_side_start != NULL) {
        *out_side_start = start_idx;
    }
    if (out_side_count != NULL) {
        *out_side_count = g_stage_pc_map_count;
    }
}
