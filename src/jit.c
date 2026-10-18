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

#define A32_CODE_CACHE_SIZE (16U * 1024U * 1024U) /* 16 MiB RX code cache */
#define A32_PAGE_SIZE       4096U
#define A32_CACHE_PAGES     (A32_CODE_CACHE_SIZE / A32_PAGE_SIZE) /* 4096 pages */
#define A32_HT_SIZE         32768U                                /* Must be a power of 2 */
#define A32_HT_MASK         (A32_HT_SIZE - 1U)
#define A32_SIDE_TABLE_SIZE 262144U
#define MAX_STAGE_PC_MAP    256U
#define A32_AOT_MAX_WORDS   2048U
#define A32_MAX_EXIT_SLOTS  8U

typedef struct a32_cache_entry {
    _Atomic uint32_t                 key;            /* +0: (guest_pc & ~1u) | T; 0 = empty */
    _Atomic uint32_t                 epoch;          /* +4 */
    const void *_Atomic              host_code;      /* +8 */
    _Atomic uint32_t                 chain_key[2];   /* +16 */
    _Atomic uint32_t                 chain_epoch[2]; /* +24 */
    const void *_Atomic              chain_code[2];  /* +32 */
    struct a32_cache_entry *_Atomic  chain_entry[2]; /* +48 */
    _Atomic uint32_t                 guest_pc;       /* +64 */
    _Atomic uint32_t                 guest_end_pc;   /* +68 */
    _Atomic uint32_t                 code_bytes;     /* +72 */
    _Atomic bool                     is_call;        /* +76 */
    uint8_t                          pad_call[3];    /* +77 */
    _Atomic uint32_t                 ret_key;        /* +80 */
    _Atomic uint32_t                 ret_epoch;      /* +84 */
    const void *_Atomic              ret_code;       /* +88 */
    struct a32_cache_entry *_Atomic  ret_entry;      /* +96 */
    size_t                           side_start;     /* +104 */
    size_t                           side_count;     /* +112 */
    _Atomic uint32_t                 slot_key[A32_MAX_EXIT_SLOTS];   /* +120 */
    struct a32_cache_entry *_Atomic  slot_entry[A32_MAX_EXIT_SLOTS]; /* +152 */
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

static a32_cache_entry_t  g_ht[A32_HT_SIZE];
static a32_cache_entry_t  g_dummy_entry;

static a32_side_entry_t   g_side_table[A32_SIDE_TABLE_SIZE];
static size_t             g_side_table_count = 0U;

static a32_pc_map_stage_t g_stage_pc_map[MAX_STAGE_PC_MAP];
static size_t             g_stage_pc_map_count = 0U;

static inline void jit_lock_acquire(void)
{
    while (atomic_exchange_explicit(&g_jit_lock, 1U, memory_order_acquire) != 0U) {
#if defined(__aarch64__)
        __asm__ volatile("yield");
#endif
    }
}

static inline void jit_lock_release(void)
{
    atomic_store_explicit(&g_jit_lock, 0U, memory_order_release);
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

static inline void clear_ht_chain_slot_locked(a32_cache_entry_t *e, size_t c)
{
    atomic_store_explicit(&e->chain_epoch[c], 0U, memory_order_release);
    atomic_store_explicit(&e->chain_entry[c], NULL, memory_order_release);
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&e->chain_key[c], 0U, memory_order_relaxed);
    atomic_store_explicit(&e->chain_code[c], NULL, memory_order_relaxed);
}

static inline void clear_ht_entry_locked(a32_cache_entry_t *e)
{
    atomic_store_explicit(&e->epoch, 0U, memory_order_release);
    atomic_store_explicit(&e->host_code, NULL, memory_order_release);
    atomic_store_explicit(&e->key, 0U, memory_order_relaxed);
    clear_ht_chain_slot_locked(e, 0U);
    clear_ht_chain_slot_locked(e, 1U);
    atomic_store_explicit(&e->ret_epoch, 0U, memory_order_release);
    atomic_store_explicit(&e->ret_key, 0U, memory_order_relaxed);
    atomic_store_explicit(&e->ret_code, NULL, memory_order_relaxed);
    atomic_store_explicit(&e->ret_entry, NULL, memory_order_relaxed);
    for (size_t s = 0U; s < A32_MAX_EXIT_SLOTS; s++) {
        atomic_store_explicit(&e->slot_key[s], 0U, memory_order_relaxed);
        atomic_store_explicit(&e->slot_entry[s], NULL, memory_order_relaxed);
    }
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
}

static void reset_cache_locked(a32_ctx_t *ctx)
{
    uint32_t old_epoch = atomic_load_explicit(&g_cache_epoch, memory_order_relaxed);
    atomic_store_explicit(&g_cache_epoch, 0U, memory_order_release);
    for (size_t p = 0U; p < A32_CACHE_PAGES; p++) {
        if (g_page_is_rw[p]) {
            void *page_addr = g_code_cache + (p * A32_PAGE_SIZE);
            if (mprotect(page_addr, A32_PAGE_SIZE, PROT_READ | PROT_EXEC) != 0) {
                fprintf(stderr, "a32rt: reset_cache mprotect(RX) failed: %s\n", strerror(errno));
            }
            g_page_is_rw[p] = false;
        }
    }
    g_code_used        = 0U;
    g_batch_min        = UINTPTR_MAX;
    g_batch_max        = 0U;
    for (size_t i = 0U; i < A32_HT_SIZE; i++) {
        clear_ht_entry_locked(&g_ht[i]);
        atomic_store_explicit(&g_ht[i].guest_pc, 0U, memory_order_relaxed);
        atomic_store_explicit(&g_ht[i].guest_end_pc, 0U, memory_order_relaxed);
        atomic_store_explicit(&g_ht[i].code_bytes, 0U, memory_order_relaxed);
        atomic_store_explicit(&g_ht[i].is_call, false, memory_order_relaxed);
        g_ht[i].side_start = 0U;
        g_ht[i].side_count = 0U;
    }
    if (ctx != NULL) {
        memset(ctx->ibtc, 0, sizeof(ctx->ibtc));
        memset(ctx->ras, 0, sizeof(ctx->ras));
        ctx->ras_top = 0U;
    }
    g_side_table_count = 0U;
    uint32_t new_epoch = old_epoch + 1U;
    if (new_epoch == 0U) {
        new_epoch = 1U;
    }
    atomic_store_explicit(&g_cache_epoch, new_epoch, memory_order_release);
}

static int flush_batch_locked(a32_ctx_t *ctx)
{
    size_t p = 0U;
    while (p < A32_CACHE_PAGES) {
        if (!g_page_is_rw[p]) {
            p++;
            continue;
        }
        size_t start_p = p;
        while (p < A32_CACHE_PAGES && g_page_is_rw[p]) {
            p++;
        }
        void  *range_start = g_code_cache + (start_p * A32_PAGE_SIZE);
        size_t range_len   = (p - start_p) * A32_PAGE_SIZE;
        if (mprotect(range_start, range_len, PROT_READ | PROT_EXEC) != 0) {
            fprintf(stderr, "a32rt: code cache mprotect(RX) failed: %s\n", strerror(errno));
            return -1;
        }
        for (size_t q = start_p; q < p; q++) {
            g_page_is_rw[q] = false;
        }
        ctx->wx_transitions++;
    }

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
        if (!g_page_is_rw[p]) {
            void *page_addr = g_code_cache + (p * A32_PAGE_SIZE);
            if (mprotect(page_addr, A32_PAGE_SIZE, PROT_READ | PROT_WRITE) != 0) {
                fprintf(stderr, "a32rt: code cache mprotect(RW) failed: %s\n", strerror(errno));
                return NULL;
            }
            g_page_is_rw[p] = true;
            ctx->wx_transitions++;
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

void a32_jit_record_pc_map(uint32_t host_word_idx, uint32_t guest_pc, uint32_t itstate)
{
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
    uint32_t cur_epoch = atomic_load_explicit(&g_cache_epoch, memory_order_relaxed);
    if (ret == 0 && cur_epoch != 0U) {
        for (size_t i = 0U; i < A32_HT_SIZE; i++) {
            if (atomic_load_explicit(&g_ht[i].epoch, memory_order_relaxed) != cur_epoch) {
                continue;
            }
            if (atomic_load_explicit(&g_ht[i].is_call, memory_order_relaxed) &&
                atomic_load_explicit(&g_ht[i].ret_epoch, memory_order_relaxed) != cur_epoch) {
                uint32_t k      = atomic_load_explicit(&g_ht[i].key, memory_order_relaxed);
                uint32_t end_pc = atomic_load_explicit(&g_ht[i].guest_end_pc, memory_order_relaxed);
                uint32_t rk     = (end_pc & ~1U) | (k & 1U);
                uint32_t r_slot = hash_key(rk);
                for (size_t probe = 0U; probe < 16U; probe++) {
                    uint32_t r_idx = (r_slot + (uint32_t)probe) & A32_HT_MASK;
                    if (atomic_load_explicit(&g_ht[r_idx].epoch, memory_order_relaxed) == cur_epoch &&
                        atomic_load_explicit(&g_ht[r_idx].key, memory_order_relaxed) == rk) {
                        const void *rc = atomic_load_explicit(&g_ht[r_idx].host_code, memory_order_relaxed);
                        if (rc != NULL) {
                            atomic_store_explicit(&g_ht[i].ret_epoch, 0U, memory_order_release);
                            atomic_store_explicit(&g_ht[i].ret_key, rk, memory_order_relaxed);
                            atomic_store_explicit(&g_ht[i].ret_code, rc, memory_order_relaxed);
                            atomic_store_explicit(&g_ht[i].ret_entry, &g_ht[r_idx], memory_order_relaxed);
                            atomic_store_explicit(&g_ht[i].ret_epoch, cur_epoch, memory_order_release);
                        }
                        break;
                    }
                }
            }
            for (size_t s = 0U; s < A32_MAX_EXIT_SLOTS; s++) {
                uint32_t s_key = atomic_load_explicit(&g_ht[i].slot_key[s], memory_order_relaxed);
                if (s_key == 0U || atomic_load_explicit(&g_ht[i].slot_entry[s], memory_order_relaxed) != NULL) {
                    continue;
                }
                uint32_t s_slot = hash_key(s_key);
                for (size_t probe = 0U; probe < 16U; probe++) {
                    uint32_t s_idx = (s_slot + (uint32_t)probe) & A32_HT_MASK;
                    if (atomic_load_explicit(&g_ht[s_idx].epoch, memory_order_relaxed) == cur_epoch &&
                        atomic_load_explicit(&g_ht[s_idx].key, memory_order_relaxed) == s_key) {
                        const void *target_code = atomic_load_explicit(&g_ht[s_idx].host_code, memory_order_relaxed);
                        if (target_code != NULL) {
                            atomic_store_explicit(&g_ht[i].slot_entry[s], &g_ht[s_idx], memory_order_release);
                            if (atomic_load_explicit(&g_ht[i].chain_entry[0], memory_order_relaxed) == NULL) {
                                atomic_store_explicit(&g_ht[i].chain_key[0], s_key, memory_order_relaxed);
                                atomic_store_explicit(&g_ht[i].chain_code[0], target_code, memory_order_relaxed);
                                atomic_store_explicit(&g_ht[i].chain_entry[0], &g_ht[s_idx], memory_order_release);
                                atomic_store_explicit(&g_ht[i].chain_epoch[0], cur_epoch, memory_order_release);
                            } else if (atomic_load_explicit(&g_ht[i].chain_entry[1], memory_order_relaxed) == NULL &&
                                       atomic_load_explicit(&g_ht[i].chain_key[0], memory_order_relaxed) != s_key) {
                                atomic_store_explicit(&g_ht[i].chain_key[1], s_key, memory_order_relaxed);
                                atomic_store_explicit(&g_ht[i].chain_code[1], target_code, memory_order_relaxed);
                                atomic_store_explicit(&g_ht[i].chain_entry[1], &g_ht[s_idx], memory_order_release);
                                atomic_store_explicit(&g_ht[i].chain_epoch[1], cur_epoch, memory_order_release);
                            }
                        }
                        break;
                    }
                }
            }
        }
    }
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
    void *mem = mmap(NULL, A32_CODE_CACHE_SIZE, PROT_READ | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        fprintf(stderr, "a32rt: failed to allocate code cache: %s\n", strerror(errno));
        return -1;
    }
    g_code_cache = (uint8_t *)mem;
    g_code_used  = 0U;
    memset(g_page_is_rw, 0, sizeof(g_page_is_rw));
    g_batch_min  = UINTPTR_MAX;
    g_batch_max  = 0U;
    memset(g_ht, 0, sizeof(g_ht));
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
    uint32_t old_epoch = atomic_load_explicit(&g_cache_epoch, memory_order_relaxed);
    bool any_overlap   = false;
    for (size_t i = 0U; i < A32_HT_SIZE; i++) {
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
    for (size_t i = 0U; i < A32_HT_SIZE; i++) {
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
    for (size_t i = 0U; i < A32_HT_SIZE; i++) {
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
            atomic_store_explicit(&g_ht[i].ret_epoch, 0U, memory_order_release);
            atomic_store_explicit(&g_ht[i].ret_key, 0U, memory_order_relaxed);
            atomic_store_explicit(&g_ht[i].ret_code, NULL, memory_order_relaxed);
            atomic_store_explicit(&g_ht[i].ret_entry, NULL, memory_order_relaxed);
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
        for (size_t i = 0U; i < A32_HT_SIZE; i++) {
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
                atomic_store_explicit(&g_ht[i].ret_epoch, 0U, memory_order_release);
                atomic_store_explicit(&g_ht[i].ret_key, 0U, memory_order_relaxed);
                atomic_store_explicit(&g_ht[i].ret_code, NULL, memory_order_relaxed);
                atomic_store_explicit(&g_ht[i].ret_entry, NULL, memory_order_relaxed);
            }
        }
    }
    atomic_store_explicit(&entry->epoch, 0U, memory_order_release);
    atomic_store_explicit(&entry->host_code, NULL, memory_order_release);
    clear_ht_chain_slot_locked(entry, 0U);
    clear_ht_chain_slot_locked(entry, 1U);
    atomic_store_explicit(&entry->ret_epoch, 0U, memory_order_release);
    atomic_store_explicit(&entry->ret_key, 0U, memory_order_relaxed);
    atomic_store_explicit(&entry->ret_code, NULL, memory_order_relaxed);
    atomic_store_explicit(&entry->ret_entry, NULL, memory_order_relaxed);
    for (size_t s = 0U; s < A32_MAX_EXIT_SLOTS; s++) {
        atomic_store_explicit(&entry->slot_key[s], 0U, memory_order_relaxed);
        atomic_store_explicit(&entry->slot_entry[s], NULL, memory_order_relaxed);
    }
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&entry->guest_pc, guest_pc, memory_order_relaxed);
    atomic_store_explicit(&entry->guest_end_pc, guest_end_pc, memory_order_relaxed);
    atomic_store_explicit(&entry->code_bytes, code_bytes, memory_order_relaxed);
    atomic_store_explicit(&entry->is_call, is_call, memory_order_relaxed);
    entry->side_start = side_start;
    entry->side_count = side_count;

    if (words != NULL && word_count > 0U) {
        for (size_t i = 0U; i + 9U < word_count; i++) {
            if ((words[i] & 0xFFC003FFU) == (0xF9400000U | (27U << 5) | 23U)) {
                unsigned uimm = (words[i] >> 10U) & 0xFFFU;
                if (uimm >= 19U && uimm < 19U + A32_MAX_EXIT_SLOTS) {
                    size_t off_movz = (i + 9U < word_count &&
                                       (words[i + 9U] & 0xFFE0001FU) == (0x52A00000U | 15U))
                                          ? (i + 9U)
                                          : (i + 20U);
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
                                uint32_t h_slot = hash_key(s_key);
                                for (size_t probe = 0U; probe < 16U; probe++) {
                                    uint32_t h_idx = (h_slot + (uint32_t)probe) & A32_HT_MASK;
                                    if (&g_ht[h_idx] != entry &&
                                        atomic_load_explicit(&g_ht[h_idx].epoch, memory_order_relaxed) == cur_epoch &&
                                        atomic_load_explicit(&g_ht[h_idx].key, memory_order_relaxed) == s_key &&
                                        atomic_load_explicit(&g_ht[h_idx].host_code, memory_order_relaxed) != NULL) {
                                        atomic_store_explicit(&entry->slot_entry[s_idx_slot], &g_ht[h_idx], memory_order_relaxed);
                                        break;
                                    }
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
        if (cur_epoch != 0U && r_key != 0U) {
            uint32_t h_slot = hash_key(r_key);
            for (size_t probe = 0U; probe < 16U; probe++) {
                uint32_t h_idx = (h_slot + (uint32_t)probe) & A32_HT_MASK;
                if (&g_ht[h_idx] != entry &&
                    atomic_load_explicit(&g_ht[h_idx].epoch, memory_order_relaxed) == cur_epoch &&
                    atomic_load_explicit(&g_ht[h_idx].key, memory_order_relaxed) == r_key &&
                    atomic_load_explicit(&g_ht[h_idx].host_code, memory_order_relaxed) != NULL) {
                    atomic_store_explicit(&entry->ret_code,
                                          atomic_load_explicit(&g_ht[h_idx].host_code, memory_order_relaxed),
                                          memory_order_relaxed);
                    atomic_store_explicit(&entry->ret_entry, &g_ht[h_idx], memory_order_relaxed);
                    atomic_store_explicit(&entry->ret_epoch, cur_epoch, memory_order_relaxed);
                    break;
                }
            }
        }
    }

    if (link_reverse && cur_epoch != 0U) {
        for (size_t i = 0U; i < A32_HT_SIZE; i++) {
            if (&g_ht[i] == entry) {
                continue;
            }
            if (atomic_load_explicit(&g_ht[i].epoch, memory_order_relaxed) != cur_epoch) {
                continue;
            }
            for (unsigned s = 0U; s < A32_MAX_EXIT_SLOTS; s++) {
                if (atomic_load_explicit(&g_ht[i].slot_key[s], memory_order_relaxed) == key &&
                    atomic_load_explicit(&g_ht[i].slot_entry[s], memory_order_relaxed) == NULL) {
                    atomic_store_explicit(&g_ht[i].slot_entry[s], entry, memory_order_release);
                }
            }
            if (atomic_load_explicit(&g_ht[i].is_call, memory_order_relaxed) &&
                atomic_load_explicit(&g_ht[i].ret_key, memory_order_relaxed) == key &&
                atomic_load_explicit(&g_ht[i].ret_entry, memory_order_relaxed) == NULL) {
                atomic_store_explicit(&g_ht[i].ret_code, dst, memory_order_relaxed);
                atomic_store_explicit(&g_ht[i].ret_entry, entry, memory_order_relaxed);
                atomic_store_explicit(&g_ht[i].ret_epoch, cur_epoch, memory_order_release);
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

    uint32_t key        = aligned_pc | (is_thumb ? 1U : 0U);
    uint32_t slot       = hash_key(key);

    jit_lock_acquire();
    uint32_t cur_epoch = atomic_load_explicit(&g_cache_epoch, memory_order_relaxed);

    bool has_free_slot = false;
    for (size_t probe = 0U; probe < 16U; probe++) {
        uint32_t idx   = (slot + (uint32_t)probe) & A32_HT_MASK;
        uint32_t k     = atomic_load_explicit(&g_ht[idx].key, memory_order_relaxed);
        const void *hc = atomic_load_explicit(&g_ht[idx].host_code, memory_order_relaxed);
        uint32_t ep    = atomic_load_explicit(&g_ht[idx].epoch, memory_order_relaxed);
        if (k == key && hc != NULL && ep == cur_epoch) {
            jit_lock_release();
            return 0;
        }
        if (k == 0U || hc == NULL || ep != cur_epoch) {
            has_free_slot = true;
        }
    }
    if (!has_free_slot) {
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

    cur_epoch = atomic_load_explicit(&g_cache_epoch, memory_order_relaxed);

    for (size_t probe = 0U; probe < 16U; probe++) {
        uint32_t idx   = (slot + (uint32_t)probe) & A32_HT_MASK;
        uint32_t k     = atomic_load_explicit(&g_ht[idx].key, memory_order_relaxed);
        const void *hc = atomic_load_explicit(&g_ht[idx].host_code, memory_order_relaxed);
        uint32_t ep    = atomic_load_explicit(&g_ht[idx].epoch, memory_order_relaxed);
        if (k == 0U || hc == NULL || ep != cur_epoch || k == key) {
            populate_ht_entry_locked(&g_ht[idx], key, dst, aligned_pc, end_pc,
                                     (uint32_t)code_bytes, is_call, side_start, side_count,
                                     cur_epoch, words, word_count, false);
            break;
        }
    }

    if (atomic_load_explicit(&g_multithreaded, memory_order_relaxed) && g_batch_max > g_batch_min) {
        (void)flush_batch_locked(ctx);
    }

    jit_lock_release();
    return 0;
}

static bool jit_range_executable(a32_ctx_t *ctx, uint32_t addr, uint32_t len)
{
    if (ctx == NULL || len == 0U || addr + len < addr) {
        return false;
    }
    bool     synced = false;
    uint32_t cur    = addr;
    uint32_t end    = addr + len;
    while (cur < end) {
        bool found = false;
        for (size_t i = 0U; i < ctx->region_count; i++) {
            const a32_region_t *r = &ctx->regions[i];
            if ((r->prot & 0x4U) != 0U && cur >= r->start && cur < r->end) {
                cur   = (r->end < end) ? r->end : end;
                found = true;
                break;
            }
        }
        if (!found) {
            if (!synced) {
                a32_vm_sync_ctx(ctx);
                synced = true;
                cur    = addr;
                continue;
            }
            return false;
        }
    }
    return true;
}

static a32_cache_entry_t *lookup_or_compile_entry(a32_ctx_t *ctx, uint32_t pc, bool is_thumb,
                                                  a32_cache_entry_t *oneshot_entry, bool flush_now,
                                                  a32_block_snap_t *out_snap)
{
    uint32_t aligned_pc = pc & (is_thumb ? ~1U : ~3U);
    uint32_t key        = aligned_pc | (is_thumb ? 1U : 0U);
    uint32_t slot       = hash_key(key);
    if (!is_thumb || (ctx->itstate & 0x0FU) == 0U) {
        ctx->itstate = 0U;
    } else {
        ctx->itstate &= 0xFFU;
    }
    bool     uncached   = (ctx->itstate != 0U) || ctx->single_step;

    jit_lock_acquire();
    uint32_t cur_epoch = atomic_load_explicit(&g_cache_epoch, memory_order_relaxed);
    if (!uncached) {
        for (size_t probe = 0U; probe < 16U; probe++) {
            uint32_t idx   = (slot + (uint32_t)probe) & A32_HT_MASK;
            const void *hc = atomic_load_explicit(&g_ht[idx].host_code, memory_order_relaxed);
            if (atomic_load_explicit(&g_ht[idx].key, memory_order_relaxed) == key &&
                hc != NULL &&
                atomic_load_explicit(&g_ht[idx].epoch, memory_order_relaxed) == cur_epoch) {
                if (flush_now && g_batch_max > g_batch_min) {
                    if (flush_batch_locked(ctx) != 0) {
                        jit_lock_release();
                        return NULL;
                    }
                }
                if (out_snap != NULL) {
                    out_snap->code         = hc;
                    out_snap->is_call      = atomic_load_explicit(&g_ht[idx].is_call, memory_order_relaxed);
                    out_snap->guest_pc     = atomic_load_explicit(&g_ht[idx].guest_pc, memory_order_relaxed);
                    out_snap->guest_end_pc = atomic_load_explicit(&g_ht[idx].guest_end_pc, memory_order_relaxed);
                    out_snap->code_bytes   = atomic_load_explicit(&g_ht[idx].code_bytes, memory_order_relaxed);
                    out_snap->epoch        = cur_epoch;
                }
                jit_lock_release();
                return &g_ht[idx];
            }
        }
    }

    if (!jit_range_executable(ctx, aligned_pc, is_thumb ? 2U : 4U)) {
        ctx->fault_addr  = aligned_pc;
        ctx->exit_reason = A32_EXIT_FAULT;
        jit_lock_release();
        return NULL;
    }

    uint32_t entry_itstate = is_thumb ? ctx->itstate : 0U;
    g_stage_pc_map_count   = 0U;
    a32_jit_record_pc_map(0U, aligned_pc, entry_itstate);

    uint32_t stage_words[A32_AOT_MAX_WORDS];
    a64_buf_t buf;
    a64_buf_init(&buf, stage_words, A32_AOT_MAX_WORDS);
    buf.guest_start_pc = aligned_pc;
    if (uncached) {
        buf.vfp_preheader_ineligible = true;
    }

    if (is_thumb) {
        a32_translate_t32_block(ctx, &buf, aligned_pc);
    } else {
        a32_translate_a32_block(ctx, &buf, aligned_pc);
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
        a32_jit_record_pc_map(0U, aligned_pc, entry_itstate);
        a64_buf_init(&buf, stage_words, A32_AOT_MAX_WORDS);
        buf.guest_start_pc = aligned_pc;
        a64_buf_start_self_loop(&buf, saved_kinds, saved_regs, saved_count, saved_written);
        if (is_thumb) {
            a32_translate_t32_block(ctx, &buf, aligned_pc);
        } else {
            a32_translate_a32_block(ctx, &buf, aligned_pc);
        }
    }

    if (buf.overflow) {
        fprintf(stderr, "a32rt: block buffer overflow at guest pc=0x%08x\n", pc);
        jit_lock_release();
        return NULL;
    }

    size_t code_bytes = buf.count * sizeof(uint32_t);
    uint8_t *dst = allocate_code_space_locked(ctx, code_bytes);
    if (dst == NULL) {
        jit_lock_release();
        return NULL;
    }

    memcpy(dst, stage_words, code_bytes);
    size_t side_start = 0U;
    size_t side_count = 0U;
    commit_side_table_locked(dst, code_bytes, aligned_pc, entry_itstate,
                             &side_start, &side_count);

    if (flush_now) {
        if (flush_batch_locked(ctx) != 0) {
            jit_lock_release();
            return NULL;
        }
    }

    uint32_t end_pc = (buf.guest_end_pc > aligned_pc) ? buf.guest_end_pc : (aligned_pc + 4U);
    cur_epoch = atomic_load_explicit(&g_cache_epoch, memory_order_relaxed);
    a32_cache_entry_t *res_entry = NULL;

    if (uncached && oneshot_entry != NULL) {
        populate_ht_entry_locked(oneshot_entry, key, dst, aligned_pc, end_pc,
                                 (uint32_t)code_bytes, buf.is_call, side_start, side_count,
                                 0U, stage_words, buf.count, false);
        res_entry = oneshot_entry;
    } else {
        for (size_t probe = 0U; probe < 16U; probe++) {
            uint32_t idx   = (slot + (uint32_t)probe) & A32_HT_MASK;
            uint32_t k     = atomic_load_explicit(&g_ht[idx].key, memory_order_relaxed);
            const void *hc = atomic_load_explicit(&g_ht[idx].host_code, memory_order_relaxed);
            uint32_t ep    = atomic_load_explicit(&g_ht[idx].epoch, memory_order_relaxed);
            if (k == 0U || hc == NULL || ep != cur_epoch || k == key) {
                populate_ht_entry_locked(&g_ht[idx], key, dst, aligned_pc, end_pc,
                                         (uint32_t)code_bytes, buf.is_call, side_start, side_count,
                                         cur_epoch, stage_words, buf.count, flush_now);
                res_entry = &g_ht[idx];
                break;
            }
        }
        if (res_entry == NULL) {
            populate_ht_entry_locked(&g_ht[slot], key, dst, aligned_pc, end_pc,
                                     (uint32_t)code_bytes, buf.is_call, side_start, side_count,
                                     cur_epoch, stage_words, buf.count, flush_now);
            res_entry = &g_ht[slot];
        }
    }

    if (out_snap != NULL) {
        out_snap->code         = dst;
        out_snap->is_call      = buf.is_call;
        out_snap->guest_pc     = aligned_pc;
        out_snap->guest_end_pc = end_pc;
        out_snap->code_bytes   = (uint32_t)code_bytes;
        out_snap->epoch        = cur_epoch;
    }

    ctx->blocks_compiled++;

    jit_lock_release();
    return res_entry;
}

static const void *compile_block_internal(a32_ctx_t *ctx, uint32_t guest_pc, bool is_thumb,
                                          uint32_t *out_code_bytes, bool flush_now)
{
    a32_cache_entry_t oneshot_entry;
    a32_block_snap_t  snap;
    a32_cache_entry_t *entry = lookup_or_compile_entry(ctx, guest_pc, is_thumb,
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

void a32_jit_iter_blocks(a32_block_iter_fn fn, void *arg)
{
    if (!fn) {
        return;
    }
    jit_lock_acquire();
    uint32_t cur_epoch = atomic_load_explicit(&g_cache_epoch, memory_order_relaxed);
    a32_pc_map_stage_t pc_map[MAX_STAGE_PC_MAP];
    for (size_t i = 0U; i < A32_HT_SIZE; i++) {
        uint32_t key          = atomic_load_explicit(&g_ht[i].key, memory_order_relaxed);
        const void *host_code = atomic_load_explicit(&g_ht[i].host_code, memory_order_relaxed);
        uint32_t ep           = atomic_load_explicit(&g_ht[i].epoch, memory_order_relaxed);
        uint32_t cb           = atomic_load_explicit(&g_ht[i].code_bytes, memory_order_relaxed);
        if (key != 0U && host_code != NULL && ep == cur_epoch && cb > 0U) {
            uint32_t guest_pc     = atomic_load_explicit(&g_ht[i].guest_pc, memory_order_relaxed);
            uint32_t end_pc_raw   = atomic_load_explicit(&g_ht[i].guest_end_pc, memory_order_relaxed);
            uint32_t guest_end_pc = (end_pc_raw > guest_pc) ? end_pc_raw : (guest_pc + 4U);
            bool is_thumb         = (key & 1U) != 0U;
            bool is_call          = atomic_load_explicit(&g_ht[i].is_call, memory_order_relaxed);
            const uint32_t *words = (const uint32_t *)host_code;
            size_t word_count     = cb / sizeof(uint32_t);
            uintptr_t base        = (uintptr_t)host_code;
            uintptr_t end         = base + cb;
            size_t map_count      = 0U;

            if (g_side_table_count >= g_ht[i].side_start + g_ht[i].side_count &&
                g_side_table_count - g_ht[i].side_start <= A32_SIDE_TABLE_SIZE) {
                for (size_t s = 0U; s < g_ht[i].side_count && map_count < MAX_STAGE_PC_MAP - 1U; s++) {
                    size_t st_idx = (g_ht[i].side_start + s) % A32_SIDE_TABLE_SIZE;
                    if (g_side_table[st_idx].host_pc_start >= base &&
                        g_side_table[st_idx].host_pc_start < end) {
                        pc_map[map_count].host_word_idx =
                            (uint32_t)((g_side_table[st_idx].host_pc_start - base) / sizeof(uint32_t));
                        pc_map[map_count].guest_pc = g_side_table[st_idx].guest_pc;
                        pc_map[map_count].itstate  = g_side_table[st_idx].itstate;
                        map_count++;
                    }
                }
            }

            fn(guest_pc, guest_end_pc, is_thumb, is_call, words, word_count,
               map_count > 0U ? pc_map : NULL, map_count, arg);
        }
    }
    jit_lock_release();
}

static void try_chain_entries(a32_cache_entry_t *prev_entry, uint32_t prev_key,
                              a32_cache_entry_t *cur_entry, uint32_t key,
                              uint32_t epoch, const void *code)
{
    if (prev_entry == NULL || prev_key == 0U || cur_entry == NULL || code == NULL) {
        return;
    }
    for (size_t s = 0U; s < A32_MAX_EXIT_SLOTS; s++) {
        if (atomic_load_explicit(&prev_entry->slot_key[s], memory_order_relaxed) == key) {
            atomic_store_explicit(&prev_entry->slot_entry[s], cur_entry, memory_order_release);
        }
    }
    uint32_t           e0  = atomic_load_explicit(&prev_entry->chain_epoch[0], memory_order_acquire);
    const void        *c0  = atomic_load_explicit(&prev_entry->chain_code[0], memory_order_relaxed);
    uint32_t           k0  = atomic_load_explicit(&prev_entry->chain_key[0], memory_order_relaxed);
    a32_cache_entry_t *ce0 = atomic_load_explicit(&prev_entry->chain_entry[0], memory_order_relaxed);
    bool slot0_live = (e0 == epoch && c0 != NULL && ce0 != NULL &&
                       atomic_load_explicit(&ce0->epoch, memory_order_acquire) == epoch &&
                       atomic_load_explicit(&ce0->key, memory_order_relaxed) == k0 &&
                       atomic_load_explicit(&ce0->host_code, memory_order_relaxed) == c0);
    if (slot0_live) {
        if (k0 == key) {
            if (c0 == code && ce0 == cur_entry) {
                return;
            }
        } else {
            uint32_t           e1  = atomic_load_explicit(&prev_entry->chain_epoch[1], memory_order_acquire);
            const void        *c1  = atomic_load_explicit(&prev_entry->chain_code[1], memory_order_relaxed);
            uint32_t           k1  = atomic_load_explicit(&prev_entry->chain_key[1], memory_order_relaxed);
            a32_cache_entry_t *ce1 = atomic_load_explicit(&prev_entry->chain_entry[1], memory_order_relaxed);
            bool slot1_live = (e1 == epoch && c1 != NULL && ce1 != NULL &&
                               atomic_load_explicit(&ce1->epoch, memory_order_acquire) == epoch &&
                               atomic_load_explicit(&ce1->key, memory_order_relaxed) == k1 &&
                               atomic_load_explicit(&ce1->host_code, memory_order_relaxed) == c1);
            if (slot1_live) {
                if (k1 != key || (c1 == code && ce1 == cur_entry)) {
                    return;
                }
            }
        }
    }
    jit_lock_acquire();
    uint32_t cur_epoch = atomic_load_explicit(&g_cache_epoch, memory_order_relaxed);
    if (cur_epoch == epoch &&
        atomic_load_explicit(&prev_entry->epoch, memory_order_relaxed) == epoch &&
        atomic_load_explicit(&prev_entry->key, memory_order_relaxed) == prev_key &&
        atomic_load_explicit(&prev_entry->host_code, memory_order_relaxed) != NULL &&
        atomic_load_explicit(&cur_entry->epoch, memory_order_relaxed) == epoch &&
        atomic_load_explicit(&cur_entry->key, memory_order_relaxed) == key &&
        atomic_load_explicit(&cur_entry->host_code, memory_order_relaxed) == code) {
        for (size_t s = 0U; s < A32_MAX_EXIT_SLOTS; s++) {
            if (atomic_load_explicit(&prev_entry->slot_key[s], memory_order_relaxed) == key) {
                atomic_store_explicit(&prev_entry->slot_entry[s], cur_entry, memory_order_release);
            }
        }
        e0  = atomic_load_explicit(&prev_entry->chain_epoch[0], memory_order_relaxed);
        c0  = atomic_load_explicit(&prev_entry->chain_code[0], memory_order_relaxed);
        k0  = atomic_load_explicit(&prev_entry->chain_key[0], memory_order_relaxed);
        ce0 = atomic_load_explicit(&prev_entry->chain_entry[0], memory_order_relaxed);
        uint32_t           e1  = atomic_load_explicit(&prev_entry->chain_epoch[1], memory_order_relaxed);
        const void        *c1  = atomic_load_explicit(&prev_entry->chain_code[1], memory_order_relaxed);
        uint32_t           k1  = atomic_load_explicit(&prev_entry->chain_key[1], memory_order_relaxed);
        a32_cache_entry_t *ce1 = atomic_load_explicit(&prev_entry->chain_entry[1], memory_order_relaxed);
        bool c0_dead = (c0 == NULL || e0 != epoch || ce0 == NULL ||
                        atomic_load_explicit(&ce0->epoch, memory_order_relaxed) != epoch ||
                        atomic_load_explicit(&ce0->key, memory_order_relaxed) != k0 ||
                        atomic_load_explicit(&ce0->host_code, memory_order_relaxed) != c0);
        if (k0 == key || (c0_dead && k1 != key)) {
            if (c0_dead || c0 != code || ce0 != cur_entry) {
                atomic_store_explicit(&prev_entry->chain_epoch[0], 0U, memory_order_release);
                atomic_store_explicit(&prev_entry->chain_entry[0], NULL, memory_order_release);
                atomic_thread_fence(memory_order_release);
                atomic_store_explicit(&prev_entry->chain_key[0], key, memory_order_relaxed);
                atomic_store_explicit(&prev_entry->chain_code[0], code, memory_order_relaxed);
                atomic_store_explicit(&prev_entry->chain_entry[0], cur_entry, memory_order_release);
                atomic_store_explicit(&prev_entry->chain_epoch[0], epoch, memory_order_release);
            }
        } else {
            bool c1_dead = (c1 == NULL || e1 != epoch || ce1 == NULL ||
                            atomic_load_explicit(&ce1->epoch, memory_order_relaxed) != epoch ||
                            atomic_load_explicit(&ce1->key, memory_order_relaxed) != k1 ||
                            atomic_load_explicit(&ce1->host_code, memory_order_relaxed) != c1);
            if (c1_dead || k1 == key) {
                if (c1_dead || c1 != code || ce1 != cur_entry) {
                    atomic_store_explicit(&prev_entry->chain_epoch[1], 0U, memory_order_release);
                    atomic_store_explicit(&prev_entry->chain_entry[1], NULL, memory_order_release);
                    atomic_thread_fence(memory_order_release);
                    atomic_store_explicit(&prev_entry->chain_key[1], key, memory_order_relaxed);
                    atomic_store_explicit(&prev_entry->chain_code[1], code, memory_order_relaxed);
                    atomic_store_explicit(&prev_entry->chain_entry[1], cur_entry, memory_order_release);
                    atomic_store_explicit(&prev_entry->chain_epoch[1], epoch, memory_order_release);
                }
            }
        }
    }
    jit_lock_release();
}

int a32_jit_run(a32_ctx_t *ctx)
{
    if (!ctx) {
        return -1;
    }
    jit_lock_acquire();
    if (ctx->is_thread_clone) {
        if (!atomic_load_explicit(&g_multithreaded, memory_order_relaxed)) {
            atomic_store_explicit(&g_multithreaded, true, memory_order_release);
            g_code_used = (g_code_used + A32_PAGE_SIZE - 1U) & ~(size_t)(A32_PAGE_SIZE - 1U);
        }
    }
    if (g_batch_max > g_batch_min) {
        (void)flush_batch_locked(ctx);
    }
    jit_lock_release();

    a32_cache_entry_t *prev_entry        = NULL;
    uint32_t           prev_key          = 0U;
    uint32_t           prev_epoch        = 0U;
    bool               gdb_stopped_at_pc = false;
    uint32_t           gdb_stopped_pc    = 0U;

    for (;;) {
        if ((ctx->pending_signals & ~ctx->sig_mask) != 0ULL) {
            uint32_t pre_stop_pc    = ctx->regs[15];
            ctx->sync_sig_delivered = false;
            a32_signals_check_pending(ctx);
            if (ctx->sync_sig_delivered || ctx->regs[15] != pre_stop_pc) {
                gdb_stopped_at_pc = false;
            }
            prev_entry = NULL;
        }

        bool is_thumb = (ctx->cpsr & A32_CPSR_T) != 0U;
        uint32_t pc   = ctx->regs[15];

        if (ctx->gdb_enabled && (ctx->single_step || a32_gdb_has_breakpoint(pc))) {
            if (gdb_stopped_at_pc && pc == gdb_stopped_pc) {
                gdb_stopped_at_pc = false;
            } else {
                gdb_stopped_at_pc = false;
                a32_gdb_handle_stop(ctx, 5 /* SIGTRAP */);
                is_thumb   = (ctx->cpsr & A32_CPSR_T) != 0U;
                pc         = ctx->regs[15];
                prev_entry = NULL;
            }
        } else {
            gdb_stopped_at_pc = false;
        }

        if (ctx->trace_enabled) {
            fprintf(stderr, "[a32rt] block pc=0x%08x (%s) r0=0x%x r1=0x%x r2=0x%x sp=0x%x lr=0x%x\n",
                    pc, is_thumb ? "T32" : "A32",
                    ctx->regs[0], ctx->regs[1], ctx->regs[2], ctx->regs[13], ctx->regs[14]);
        }

        if (!is_thumb || (ctx->itstate & 0x0FU) == 0U) {
            ctx->itstate = 0U;
        } else {
            ctx->itstate &= 0xFFU;
        }
        uint32_t key      = (pc & (is_thumb ? ~1U : ~3U)) | (is_thumb ? 1U : 0U);
        uint32_t epoch    = atomic_load_explicit(&g_cache_epoch, memory_order_acquire);
        bool     uncached = (ctx->itstate != 0U) || ctx->single_step;

        if (prev_entry != NULL &&
            (epoch == 0U || epoch != prev_epoch ||
             atomic_load_explicit(&prev_entry->key, memory_order_relaxed) != prev_key)) {
            prev_entry = NULL;
        }

        const void        *code          = NULL;
        a32_cache_entry_t *cur_entry     = NULL;
        bool               cur_is_call = false;
        uint32_t           cur_start   = 0U;
        uint32_t           cur_end     = 0U;
        a32_cache_entry_t  oneshot_entry;
        uint32_t           ibtc_idx    = ((key >> 1U) ^ (key & 1U)) & (A32_IBTC_SIZE - 1U);

        if (uncached) {
            prev_entry = NULL;
        }

        /* 1. Return Address Stack (RAS) prediction for return targets */
        uint32_t r_idx = (ctx->ras_top - 1U) & (A32_RAS_SIZE - 1U);
        uint32_t r_key = ctx->ras[r_idx].key;
        if (key != 0U && r_key == key) {
            uint32_t r_epoch = ctx->ras[r_idx].epoch;
            const void *r_code = ctx->ras[r_idx].host_code;
            void *r_ent = ctx->ras[r_idx].entry;
            ctx->ras_top = r_idx;
            ctx->ras[r_idx].key = 0U;
            ctx->ras[r_idx].epoch = 0U;
            ctx->ras[r_idx].host_code = NULL;
            ctx->ras[r_idx].entry = NULL;
            if (!uncached && epoch != 0U && r_epoch == epoch && r_code != NULL) {
                if (validate_cache_candidate((a32_cache_entry_t *)r_ent, r_code, key, epoch,
                                             &code, &cur_entry, &cur_is_call, &cur_start, &cur_end)) {
                    ctx->ras_hits++;
                    ctx->ibtc[ibtc_idx].key       = key;
                    ctx->ibtc[ibtc_idx].epoch     = epoch;
                    ctx->ibtc[ibtc_idx].host_code = code;
                    ctx->ibtc[ibtc_idx].entry     = cur_entry;
                    try_chain_entries(prev_entry, prev_key, cur_entry, key, epoch, code);
                }
            }
        }

        /* 1.5. Fast-path direct chain on prev_entry */
        if (!uncached && epoch != 0U && code == NULL && prev_entry != NULL &&
            atomic_load_explicit(&prev_entry->epoch, memory_order_acquire) == epoch &&
            atomic_load_explicit(&prev_entry->key, memory_order_acquire) == prev_key &&
            atomic_load_explicit(&prev_entry->host_code, memory_order_acquire) != NULL) {
            if (atomic_load_explicit(&prev_entry->chain_epoch[0], memory_order_acquire) == epoch &&
                atomic_load_explicit(&prev_entry->chain_key[0], memory_order_relaxed) == key) {
                const void *c0        = atomic_load_explicit(&prev_entry->chain_code[0], memory_order_relaxed);
                a32_cache_entry_t *e0 = atomic_load_explicit(&prev_entry->chain_entry[0], memory_order_relaxed);
                if (validate_cache_candidate(e0, c0, key, epoch,
                                             &code, &cur_entry, &cur_is_call, &cur_start, &cur_end)) {
                    ctx->chain_hits++;
                }
            } else if (atomic_load_explicit(&prev_entry->chain_epoch[1], memory_order_acquire) == epoch &&
                       atomic_load_explicit(&prev_entry->chain_key[1], memory_order_relaxed) == key) {
                const void *c1        = atomic_load_explicit(&prev_entry->chain_code[1], memory_order_relaxed);
                a32_cache_entry_t *e1 = atomic_load_explicit(&prev_entry->chain_entry[1], memory_order_relaxed);
                if (validate_cache_candidate(e1, c1, key, epoch,
                                             &code, &cur_entry, &cur_is_call, &cur_start, &cur_end)) {
                    ctx->chain_hits++;
                }
            }
        }

        /* 2. Check per-thread 1024-entry IBTC */
        if (!uncached && epoch != 0U && code == NULL) {
            if (ctx->ibtc[ibtc_idx].key == key &&
                ctx->ibtc[ibtc_idx].epoch == epoch &&
                ctx->ibtc[ibtc_idx].host_code != NULL) {
                if (validate_cache_candidate((a32_cache_entry_t *)ctx->ibtc[ibtc_idx].entry,
                                             ctx->ibtc[ibtc_idx].host_code, key, epoch,
                                             &code, &cur_entry, &cur_is_call, &cur_start, &cur_end)) {
                    ctx->ibtc_hits++;
                    try_chain_entries(prev_entry, prev_key, cur_entry, key, epoch, code);
                }
            }
        }

        /* 3. Otherwise lookup or compile */
        if (code == NULL) {
            a32_block_snap_t snap;
            ctx->exit_reason = A32_EXIT_NONE;
            cur_entry = lookup_or_compile_entry(ctx, pc, is_thumb, &oneshot_entry, true, &snap);
            if (cur_entry == NULL || snap.code == NULL) {
                if (ctx->exit_reason == A32_EXIT_FAULT) {
                    prev_entry = NULL;
                    if (ctx->gdb_enabled) {
                        int sig = a32_gdb_handle_stop(ctx, 11);
                        if (sig == 0) {
                            continue;
                        }
                    }
                    if (a32_signals_deliver_sync(ctx, 11 /* SIGSEGV */, ctx->fault_addr)) {
                        a32_signals_check_pending(ctx);
                        continue;
                    }
                    fprintf(stderr, "a32rt: guest fault at 0x%08x\n", ctx->fault_addr);
                    return 139;
                }
                return 1;
            }
            if (snap.epoch != epoch ||
                (prev_entry != NULL &&
                 atomic_load_explicit(&prev_entry->key, memory_order_relaxed) != prev_key)) {
                epoch      = snap.epoch;
                prev_entry = NULL;
            }
            code        = snap.code;
            cur_is_call = snap.is_call;
            cur_start   = snap.guest_pc;
            cur_end     = snap.guest_end_pc;
            if (!uncached) {
                ctx->ibtc[ibtc_idx].key       = key;
                ctx->ibtc[ibtc_idx].epoch     = epoch;
                ctx->ibtc[ibtc_idx].host_code = code;
                ctx->ibtc[ibtc_idx].entry     = cur_entry;
                try_chain_entries(prev_entry, prev_key, cur_entry, key, epoch, code);
            }
        }

        if (uncached) {
            prev_entry = NULL;
            prev_key   = 0U;
            prev_epoch = 0U;
        } else {
            prev_entry = cur_entry;
            prev_key   = key;
            prev_epoch = epoch;
        }

        ctx->blocks_executed++;
        ctx->sync_sig_delivered = false;
        ctx->last_exec_entry    = uncached ? (void *)&g_dummy_entry : (void *)cur_entry;
        uint32_t reason         = a32_jit_run_block(ctx, code, uncached ? (void *)&g_dummy_entry : (void *)cur_entry, &g_cache_epoch);
        bool     sync_sig_taken = ctx->sync_sig_delivered;
        if (sync_sig_taken) {
            prev_entry = NULL;
        } else if (!uncached) {
            a32_cache_entry_t *last_e = (a32_cache_entry_t *)ctx->last_exec_entry;
            if (last_e != NULL && last_e != &g_dummy_entry && last_e != cur_entry) {
                cur_entry   = last_e;
                cur_is_call = atomic_load_explicit(&last_e->is_call, memory_order_relaxed);
                cur_start   = atomic_load_explicit(&last_e->guest_pc, memory_order_relaxed);
                cur_end     = atomic_load_explicit(&last_e->guest_end_pc, memory_order_relaxed);
                prev_entry  = last_e;
                prev_key    = atomic_load_explicit(&last_e->key, memory_order_relaxed);
            }
        }

        if ((reason == A32_EXIT_BRANCH || reason == A32_EXIT_INTERWORK) && !sync_sig_taken) {
            uint32_t lr_pc   = ctx->regs[14] & ~1U;
            uint32_t next_pc = ctx->regs[15] & ~1U;
            if (cur_is_call && ctx->regs[14] != 0xFFFF0FA8U &&
                lr_pc > cur_start && lr_pc == cur_end && next_pc != lr_pc) {
                uint32_t ret_key = lr_pc | (is_thumb ? 1U : 0U);
                uint32_t r_ibtc  = ((ret_key >> 1U) ^ (ret_key & 1U)) & (A32_IBTC_SIZE - 1U);
                uint32_t top     = ctx->ras_top;
                a32_cache_entry_t *found_e    = NULL;
                const void        *found_code = NULL;
                if (ret_key != 0U && epoch != 0U &&
                    ctx->ibtc[r_ibtc].key == ret_key &&
                    ctx->ibtc[r_ibtc].epoch == epoch &&
                    ctx->ibtc[r_ibtc].host_code != NULL) {
                    found_code = ctx->ibtc[r_ibtc].host_code;
                    found_e    = (a32_cache_entry_t *)ctx->ibtc[r_ibtc].entry;
                } else if (ret_key != 0U && epoch != 0U) {
                    uint32_t r_slot = hash_key(ret_key);
                    for (size_t probe = 0U; probe < 4U; probe++) {
                        uint32_t r_idx_ht       = (r_slot + (uint32_t)probe) & A32_HT_MASK;
                        a32_cache_entry_t *ht_e = &g_ht[r_idx_ht];
                        const void *ht_code     = atomic_load_explicit(&ht_e->host_code, memory_order_relaxed);
                        bool is_call_dummy;
                        uint32_t start_dummy, end_dummy;
                        if (validate_cache_candidate(ht_e, ht_code, ret_key, epoch,
                                                     &found_code, &found_e, &is_call_dummy,
                                                     &start_dummy, &end_dummy)) {
                            break;
                        }
                    }
                }
                if (found_e != NULL && found_code != NULL) {
                    ctx->ibtc[r_ibtc].key       = ret_key;
                    ctx->ibtc[r_ibtc].epoch     = epoch;
                    ctx->ibtc[r_ibtc].host_code = found_code;
                    ctx->ibtc[r_ibtc].entry     = found_e;
                    ctx->ras[top].key           = ret_key;
                    ctx->ras[top].epoch         = epoch;
                    ctx->ras[top].host_code     = found_code;
                    ctx->ras[top].entry         = found_e;
                    if (cur_entry != NULL && !uncached) {
                        atomic_store_explicit(&cur_entry->ret_epoch, 0U, memory_order_release);
                        atomic_store_explicit(&cur_entry->ret_key, ret_key, memory_order_relaxed);
                        atomic_store_explicit(&cur_entry->ret_code, found_code, memory_order_relaxed);
                        atomic_store_explicit(&cur_entry->ret_entry, found_e, memory_order_relaxed);
                        atomic_store_explicit(&cur_entry->ret_epoch, epoch, memory_order_release);
                    }
                } else {
                    ctx->ras[top].key       = ret_key;
                    ctx->ras[top].epoch     = 0U;
                    ctx->ras[top].host_code = NULL;
                    ctx->ras[top].entry     = NULL;
                }
                ctx->ras_top = (top + 1U) & (A32_RAS_SIZE - 1U);
            }
        }

        switch (reason) {
        case A32_EXIT_BRANCH:
            if (is_thumb) {
                ctx->regs[15] &= ~1U;
            } else {
                ctx->regs[15] &= ~3U;
            }
            break;

        case A32_EXIT_INTERWORK: {
            uint32_t target = ctx->regs[15];
            if ((target & 1U) != 0U) {
                ctx->cpsr |= A32_CPSR_T;
                ctx->regs[15] = target & ~1U;
            } else {
                ctx->cpsr &= ~A32_CPSR_T;
                ctx->regs[15] = target & ~3U;
            }
            break;
        }

        case A32_EXIT_SVC: {
            prev_entry = NULL;
            ctx->syscalls_executed++;
            uint32_t pre_stop_pc = ctx->regs[15];
            bool     is_bkpt_svc = (ctx->regs[7] == 0x0f0001U);
            if (a32_handle_syscall(ctx) != 0) {
                return (int)ctx->regs[0];
            }
            if (ctx->gdb_enabled && is_bkpt_svc) {
                if (ctx->regs[15] == pre_stop_pc) {
                    gdb_stopped_at_pc = true;
                    gdb_stopped_pc    = pre_stop_pc;
                } else {
                    gdb_stopped_at_pc = false;
                }
            }
            break;
        }

        case A32_EXIT_BKPT: {
            prev_entry = NULL;
            if (ctx->gdb_enabled) {
                uint32_t pre_stop_pc = ctx->regs[15];
                a32_gdb_handle_stop(ctx, 5 /* SIGTRAP */);
                if (ctx->regs[15] == pre_stop_pc) {
                    gdb_stopped_at_pc = true;
                    gdb_stopped_pc    = pre_stop_pc;
                } else {
                    gdb_stopped_at_pc = false;
                }
            } else if (!a32_signals_deliver_sync(ctx, 5 /* SIGTRAP */, ctx->regs[15])) {
                raise(5 /* SIGTRAP */);
            }
            break;
        }

        case A32_EXIT_FAULT: {
            prev_entry = NULL;
            if (ctx->gdb_enabled) {
                int sig = a32_gdb_handle_stop(ctx, 11);
                if (sig == 0) {
                    continue;
                }
            }
            if (a32_signals_deliver_sync(ctx, 11 /* SIGSEGV */, ctx->fault_addr)) {
                a32_signals_check_pending(ctx);
                continue;
            }
            fprintf(stderr, "a32rt: guest fault at 0x%08x\n", ctx->fault_addr);
            return 139;
        }

        case A32_EXIT_UNDEF: {
            prev_entry = NULL;
            if (ctx->gdb_enabled) {
                uint32_t pre_stop_pc = ctx->regs[15];
                a32_gdb_handle_stop(ctx, 4 /* SIGILL */);
                if (ctx->regs[15] == pre_stop_pc) {
                    gdb_stopped_at_pc = true;
                    gdb_stopped_pc    = pre_stop_pc;
                } else {
                    gdb_stopped_at_pc = false;
                }
                break;
            }
            if (a32_signals_deliver_sync(ctx, 4 /* SIGILL */, ctx->regs[15])) {
                break;
            }
            fprintf(stderr, "a32rt: aborting on exit_reason=%u at guest pc=0x%08x\n",
                    reason, ctx->regs[15]);
            return 128 + SIGILL;
        }

        default:
            fprintf(stderr, "a32rt: aborting on exit_reason=%u at guest pc=0x%08x\n",
                    reason, ctx->regs[15]);
            return 127;
        }
    }
}
