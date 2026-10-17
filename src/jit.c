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
#define A32_HT_SIZE         16384U                                /* Must be a power of 2 */
#define A32_HT_MASK         (A32_HT_SIZE - 1U)
#define A32_SIDE_TABLE_SIZE 65536U
#define MAX_STAGE_PC_MAP    256U

typedef struct a32_cache_entry {
    _Atomic uint32_t                 key;        /* (guest_pc & ~1u) | T; 0 = empty */
    uint32_t                         guest_pc;
    uint32_t                         guest_end_pc;
    uint32_t                         code_bytes;
    bool                             is_call;
    size_t                           side_start;
    size_t                           side_count;
    _Atomic uint32_t                 epoch;
    const void *_Atomic              host_code;
    _Atomic uint32_t                 chain_key[2];
    _Atomic uint32_t                 chain_epoch[2];
    const void *_Atomic              chain_code[2];
    struct a32_cache_entry *_Atomic  chain_entry[2];
} a32_cache_entry_t;

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
    atomic_fetch_add_explicit(&g_cache_epoch, 1U, memory_order_acq_rel);
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
        atomic_store_explicit(&g_ht[i].epoch, 0U, memory_order_release);
        atomic_store_explicit(&g_ht[i].host_code, NULL, memory_order_release);
        atomic_store_explicit(&g_ht[i].key, 0U, memory_order_relaxed);
        atomic_store_explicit(&g_ht[i].chain_epoch[0], 0U, memory_order_release);
        atomic_store_explicit(&g_ht[i].chain_key[0], 0U, memory_order_relaxed);
        atomic_store_explicit(&g_ht[i].chain_code[0], NULL, memory_order_relaxed);
        atomic_store_explicit(&g_ht[i].chain_entry[0], NULL, memory_order_relaxed);
        atomic_store_explicit(&g_ht[i].chain_epoch[1], 0U, memory_order_release);
        atomic_store_explicit(&g_ht[i].chain_key[1], 0U, memory_order_relaxed);
        atomic_store_explicit(&g_ht[i].chain_code[1], NULL, memory_order_relaxed);
        atomic_store_explicit(&g_ht[i].chain_entry[1], NULL, memory_order_relaxed);
        g_ht[i].guest_pc     = 0U;
        g_ht[i].guest_end_pc = 0U;
        g_ht[i].code_bytes   = 0U;
        g_ht[i].is_call      = false;
        g_ht[i].side_start   = 0U;
        g_ht[i].side_count   = 0U;
    }
    if (ctx != NULL) {
        memset(ctx->ibtc, 0, sizeof(ctx->ibtc));
        memset(ctx->ras_key, 0, sizeof(ctx->ras_key));
        memset(ctx->ras_epoch, 0, sizeof(ctx->ras_epoch));
        memset(ctx->ras_code, 0, sizeof(ctx->ras_code));
        memset(ctx->ras_entry, 0, sizeof(ctx->ras_entry));
        ctx->ras_top = 0U;
    }
    g_side_table_count = 0U;
    atomic_fetch_add_explicit(&g_cache_epoch, 1U, memory_order_acq_rel);
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
            g_page_is_rw[p] = false;
            p++;
        }
        void  *range_start = g_code_cache + (start_p * A32_PAGE_SIZE);
        size_t range_len   = (p - start_p) * A32_PAGE_SIZE;
        if (mprotect(range_start, range_len, PROT_READ | PROT_EXEC) != 0) {
            fprintf(stderr, "a32rt: code cache mprotect(RX) failed: %s\n", strerror(errno));
            return -1;
        }
        ctx->wx_transitions++;
    }

    if (g_batch_max > g_batch_min) {
        if (atomic_load_explicit(&g_multithreaded, memory_order_acquire)) {
            /*
             * Align g_code_used up to the next 4 KiB page boundary in multithreaded
             * mode so subsequent allocations never re-mprotect(PROT_READ | PROT_WRITE)
             * a page that already holds live executable code being run by another thread.
             */
            g_code_used = (g_code_used + A32_PAGE_SIZE - 1U) & ~(size_t)(A32_PAGE_SIZE - 1U);
        }
        flush_icache_range(ctx, (const void *)g_batch_min, g_batch_max - g_batch_min);
        g_batch_min = UINTPTR_MAX;
        g_batch_max = 0U;
    }
    return 0;
}

static uint8_t *allocate_code_space_locked(a32_ctx_t *ctx, size_t code_bytes)
{
    if (code_bytes > A32_CODE_CACHE_SIZE) {
        return NULL;
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
    size_t total = g_side_table_count;
    size_t count = (total > A32_SIDE_TABLE_SIZE) ? A32_SIDE_TABLE_SIZE : total;
    for (size_t i = 0U; i < count; i++) {
        size_t idx = (total - 1U - i) % A32_SIDE_TABLE_SIZE;
        if (host_pc >= g_side_table[idx].host_pc_start && host_pc < g_side_table[idx].host_pc_end) {
            if (out_guest_pc != NULL) {
                *out_guest_pc = g_side_table[idx].guest_pc;
            }
            if (out_itstate != NULL) {
                *out_itstate = g_side_table[idx].itstate;
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
    jit_lock_acquire();
    int ret = flush_batch_locked(ctx);
    jit_lock_release();
    return ret;
}

int a32_jit_init(a32_ctx_t *ctx)
{
    if (ctx != NULL) {
        memset(ctx->ibtc, 0, sizeof(ctx->ibtc));
        memset(ctx->ras_key, 0, sizeof(ctx->ras_key));
        memset(ctx->ras_epoch, 0, sizeof(ctx->ras_epoch));
        memset(ctx->ras_code, 0, sizeof(ctx->ras_code));
        memset(ctx->ras_entry, 0, sizeof(ctx->ras_entry));
        ctx->ras_top = 0U;
    }
    if (g_code_cache != NULL) {
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
            uint32_t b_start = g_ht[i].guest_pc;
            uint32_t b_end   = (g_ht[i].guest_end_pc > b_start) ? g_ht[i].guest_end_pc : (b_start + 4U);
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
        memset(ctx->ras_key, 0, sizeof(ctx->ras_key));
        memset(ctx->ras_epoch, 0, sizeof(ctx->ras_epoch));
        memset(ctx->ras_code, 0, sizeof(ctx->ras_code));
        memset(ctx->ras_entry, 0, sizeof(ctx->ras_entry));
        ctx->ras_top = 0U;
    }

    uint32_t new_epoch = old_epoch + 1U;
    if (new_epoch == 0U) {
        new_epoch = 1U;
    }
    for (size_t i = 0U; i < A32_HT_SIZE; i++) {
        uint32_t k     = atomic_load_explicit(&g_ht[i].key, memory_order_relaxed);
        const void *hc = atomic_load_explicit(&g_ht[i].host_code, memory_order_relaxed);
        if (k != 0U && hc != NULL &&
            atomic_load_explicit(&g_ht[i].epoch, memory_order_relaxed) == old_epoch) {
            uint32_t b_start = g_ht[i].guest_pc;
            uint32_t b_end   = (g_ht[i].guest_end_pc > b_start) ? g_ht[i].guest_end_pc : (b_start + 4U);
            if (b_start < end && b_end > start) {
                atomic_store_explicit(&g_ht[i].epoch, 0U, memory_order_release);
                atomic_store_explicit(&g_ht[i].host_code, NULL, memory_order_release);
                atomic_store_explicit(&g_ht[i].key, 0U, memory_order_relaxed);
                atomic_store_explicit(&g_ht[i].chain_epoch[0], 0U, memory_order_release);
                atomic_store_explicit(&g_ht[i].chain_key[0], 0U, memory_order_relaxed);
                atomic_store_explicit(&g_ht[i].chain_code[0], NULL, memory_order_relaxed);
                atomic_store_explicit(&g_ht[i].chain_entry[0], NULL, memory_order_relaxed);
                atomic_store_explicit(&g_ht[i].chain_epoch[1], 0U, memory_order_release);
                atomic_store_explicit(&g_ht[i].chain_key[1], 0U, memory_order_relaxed);
                atomic_store_explicit(&g_ht[i].chain_code[1], NULL, memory_order_relaxed);
                atomic_store_explicit(&g_ht[i].chain_entry[1], NULL, memory_order_relaxed);
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
                atomic_store_explicit(&g_ht[i].chain_epoch[c], 0U, memory_order_release);
                atomic_store_explicit(&g_ht[i].chain_key[c], 0U, memory_order_relaxed);
                atomic_store_explicit(&g_ht[i].chain_code[c], NULL, memory_order_relaxed);
                atomic_store_explicit(&g_ht[i].chain_entry[c], NULL, memory_order_relaxed);
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
                                     uint32_t cur_epoch)
{
    atomic_store_explicit(&entry->epoch, 0U, memory_order_release);
    atomic_store_explicit(&entry->host_code, NULL, memory_order_release);
    entry->guest_pc     = guest_pc;
    entry->guest_end_pc = guest_end_pc;
    entry->code_bytes   = code_bytes;
    entry->is_call      = is_call;
    entry->side_start   = side_start;
    entry->side_count   = side_count;
    atomic_store_explicit(&entry->chain_epoch[0], 0U, memory_order_relaxed);
    atomic_store_explicit(&entry->chain_key[0], 0U, memory_order_relaxed);
    atomic_store_explicit(&entry->chain_code[0], NULL, memory_order_relaxed);
    atomic_store_explicit(&entry->chain_entry[0], NULL, memory_order_relaxed);
    atomic_store_explicit(&entry->chain_epoch[1], 0U, memory_order_relaxed);
    atomic_store_explicit(&entry->chain_key[1], 0U, memory_order_relaxed);
    atomic_store_explicit(&entry->chain_code[1], NULL, memory_order_relaxed);
    atomic_store_explicit(&entry->chain_entry[1], NULL, memory_order_relaxed);
    atomic_store_explicit(&entry->key, key, memory_order_release);
    atomic_store_explicit(&entry->host_code, dst, memory_order_release);
    atomic_store_explicit(&entry->epoch, cur_epoch, memory_order_release);
}

int a32_jit_install_aot_block(a32_ctx_t *ctx, uint32_t guest_pc, uint32_t guest_end_pc,
                              bool is_thumb, bool is_call,
                              const uint32_t *words, size_t word_count,
                              const a32_pc_map_stage_t *pc_map, size_t pc_map_count)
{
    if (words == NULL || word_count == 0U) {
        return -1;
    }

    uint32_t aligned_pc = guest_pc & ~1U;
    uint32_t end_pc     = (guest_end_pc > aligned_pc) ? guest_end_pc : (aligned_pc + 4U);
    uint32_t key        = aligned_pc | (is_thumb ? 1U : 0U);
    uint32_t slot       = hash_key(key);

    jit_lock_acquire();

    for (size_t probe = 0U; probe < 16U; probe++) {
        uint32_t idx = (slot + (uint32_t)probe) & A32_HT_MASK;
        if (atomic_load_explicit(&g_ht[idx].key, memory_order_relaxed) == key &&
            atomic_load_explicit(&g_ht[idx].host_code, memory_order_relaxed) != NULL) {
            jit_lock_release();
            return 0;
        }
    }

    size_t code_bytes = word_count * sizeof(uint32_t);
    uint8_t *dst = allocate_code_space_locked(ctx, code_bytes);
    if (dst == NULL) {
        jit_lock_release();
        return -1;
    }

    memcpy(dst, words, code_bytes);

    g_stage_pc_map_count = 0U;
    if (pc_map != NULL && pc_map_count > 0U) {
        size_t n = (pc_map_count <= MAX_STAGE_PC_MAP) ? pc_map_count : MAX_STAGE_PC_MAP;
        for (size_t i = 0U; i < n; i++) {
            if (pc_map[i].host_word_idx < word_count) {
                g_stage_pc_map[g_stage_pc_map_count++] = pc_map[i];
            }
        }
    }
    size_t side_start = 0U;
    size_t side_count = 0U;
    commit_side_table_locked(dst, code_bytes, aligned_pc, 0U, &side_start, &side_count);

    uint32_t cur_epoch = atomic_load_explicit(&g_cache_epoch, memory_order_relaxed);

    bool inserted = false;
    for (size_t probe = 0U; probe < 16U; probe++) {
        uint32_t idx = (slot + (uint32_t)probe) & A32_HT_MASK;
        uint32_t k   = atomic_load_explicit(&g_ht[idx].key, memory_order_relaxed);
        const void *hc = atomic_load_explicit(&g_ht[idx].host_code, memory_order_relaxed);
        if (k == 0U || hc == NULL || k == key) {
            populate_ht_entry_locked(&g_ht[idx], key, dst, aligned_pc, end_pc,
                                     (uint32_t)code_bytes, is_call, side_start, side_count,
                                     cur_epoch);
            inserted = true;
            break;
        }
    }
    if (!inserted) {
        populate_ht_entry_locked(&g_ht[slot], key, dst, aligned_pc, end_pc,
                                 (uint32_t)code_bytes, is_call, side_start, side_count,
                                 cur_epoch);
    }

    jit_lock_release();
    return 0;
}

static a32_cache_entry_t *lookup_or_compile_entry(a32_ctx_t *ctx, uint32_t pc, bool is_thumb,
                                                  a32_cache_entry_t *oneshot_entry, bool flush_now)
{
    uint32_t aligned_pc = pc & ~1U;
    uint32_t key        = aligned_pc | (is_thumb ? 1U : 0U);
    uint32_t slot       = hash_key(key);
    bool     uncached   = (ctx->itstate != 0U) || ctx->single_step;

    jit_lock_acquire();
    uint32_t cur_epoch = atomic_load_explicit(&g_cache_epoch, memory_order_relaxed);
    if (!uncached) {
        for (size_t probe = 0U; probe < 16U; probe++) {
            uint32_t idx = (slot + (uint32_t)probe) & A32_HT_MASK;
            if (atomic_load_explicit(&g_ht[idx].key, memory_order_relaxed) == key &&
                atomic_load_explicit(&g_ht[idx].host_code, memory_order_relaxed) != NULL &&
                atomic_load_explicit(&g_ht[idx].epoch, memory_order_relaxed) == cur_epoch) {
                if (flush_now && g_batch_max > g_batch_min) {
                    if (flush_batch_locked(ctx) != 0) {
                        jit_lock_release();
                        return NULL;
                    }
                }
                jit_lock_release();
                return &g_ht[idx];
            }
        }
    }

    uint32_t entry_itstate = is_thumb ? ctx->itstate : 0U;
    g_stage_pc_map_count   = 0U;
    a32_jit_record_pc_map(0U, aligned_pc, entry_itstate);

    uint32_t stage_words[1024];
    a64_buf_t buf;
    a64_buf_init(&buf, stage_words, 1024U);

    if (is_thumb) {
        a32_translate_t32_block(ctx, &buf, pc & ~1U);
    } else {
        a32_translate_a32_block(ctx, &buf, pc & ~3U);
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
                                 0U);
        res_entry = oneshot_entry;
    } else {
        for (size_t probe = 0U; probe < 16U; probe++) {
            uint32_t idx = (slot + (uint32_t)probe) & A32_HT_MASK;
            uint32_t k   = atomic_load_explicit(&g_ht[idx].key, memory_order_relaxed);
            const void *hc = atomic_load_explicit(&g_ht[idx].host_code, memory_order_relaxed);
            if (k == 0U || hc == NULL || k == key) {
                populate_ht_entry_locked(&g_ht[idx], key, dst, aligned_pc, end_pc,
                                         (uint32_t)code_bytes, buf.is_call, side_start, side_count,
                                         cur_epoch);
                res_entry = &g_ht[idx];
                break;
            }
        }
        if (res_entry == NULL) {
            populate_ht_entry_locked(&g_ht[slot], key, dst, aligned_pc, end_pc,
                                     (uint32_t)code_bytes, buf.is_call, side_start, side_count,
                                     cur_epoch);
            res_entry = &g_ht[slot];
        }
    }

    ctx->blocks_compiled++;

    jit_lock_release();
    return res_entry;
}

static const void *compile_block_internal(a32_ctx_t *ctx, uint32_t guest_pc, bool is_thumb,
                                          uint32_t *out_code_bytes, bool flush_now)
{
    a32_cache_entry_t oneshot_entry;
    a32_cache_entry_t *entry = lookup_or_compile_entry(ctx, guest_pc, is_thumb,
                                                       &oneshot_entry, flush_now);
    if (entry == NULL) {
        return NULL;
    }
    if (out_code_bytes != NULL) {
        *out_code_bytes = entry->code_bytes;
    }
    return atomic_load_explicit(&entry->host_code, memory_order_acquire);
}

const void *a32_jit_compile_block(a32_ctx_t *ctx, uint32_t pc, bool is_thumb, uint32_t *out_code_bytes)
{
    return compile_block_internal(ctx, pc, is_thumb, out_code_bytes, false);
}

void a32_jit_iter_blocks(a32_block_iter_fn fn, void *arg)
{
    jit_lock_acquire();
    uint32_t cur_epoch = atomic_load_explicit(&g_cache_epoch, memory_order_relaxed);
    a32_pc_map_stage_t pc_map[MAX_STAGE_PC_MAP];
    for (size_t i = 0U; i < A32_HT_SIZE; i++) {
        uint32_t key          = atomic_load_explicit(&g_ht[i].key, memory_order_relaxed);
        const void *host_code = atomic_load_explicit(&g_ht[i].host_code, memory_order_relaxed);
        uint32_t ep           = atomic_load_explicit(&g_ht[i].epoch, memory_order_relaxed);
        if (key != 0U && host_code != NULL && ep == cur_epoch && g_ht[i].code_bytes > 0U) {
            uint32_t guest_pc     = g_ht[i].guest_pc;
            uint32_t guest_end_pc = (g_ht[i].guest_end_pc > guest_pc) ?
                                    g_ht[i].guest_end_pc : (guest_pc + 4U);
            bool is_thumb         = (key & 1U) != 0U;
            bool is_call          = g_ht[i].is_call;
            const uint32_t *words = (const uint32_t *)host_code;
            size_t word_count     = g_ht[i].code_bytes / sizeof(uint32_t);
            uintptr_t base        = (uintptr_t)host_code;
            uintptr_t end         = base + g_ht[i].code_bytes;
            size_t map_count      = 0U;

            if (g_side_table_count >= g_ht[i].side_start + g_ht[i].side_count &&
                g_side_table_count - g_ht[i].side_start <= A32_SIDE_TABLE_SIZE) {
                for (size_t s = 0U; s < g_ht[i].side_count && map_count < MAX_STAGE_PC_MAP; s++) {
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
    jit_lock_acquire();
    uint32_t cur_epoch = atomic_load_explicit(&g_cache_epoch, memory_order_relaxed);
    if (cur_epoch == epoch &&
        atomic_load_explicit(&prev_entry->epoch, memory_order_relaxed) == epoch &&
        atomic_load_explicit(&prev_entry->key, memory_order_relaxed) == prev_key &&
        atomic_load_explicit(&prev_entry->host_code, memory_order_relaxed) != NULL &&
        atomic_load_explicit(&cur_entry->epoch, memory_order_relaxed) == epoch &&
        atomic_load_explicit(&cur_entry->key, memory_order_relaxed) == key &&
        atomic_load_explicit(&cur_entry->host_code, memory_order_relaxed) == code) {
        uint32_t e0    = atomic_load_explicit(&prev_entry->chain_epoch[0], memory_order_relaxed);
        const void *c0 = atomic_load_explicit(&prev_entry->chain_code[0], memory_order_relaxed);
        uint32_t k0    = atomic_load_explicit(&prev_entry->chain_key[0], memory_order_relaxed);
        if (c0 == NULL || e0 != epoch) {
            atomic_store_explicit(&prev_entry->chain_epoch[0], 0U, memory_order_release);
            atomic_store_explicit(&prev_entry->chain_key[0], key, memory_order_relaxed);
            atomic_store_explicit(&prev_entry->chain_code[0], code, memory_order_relaxed);
            atomic_store_explicit(&prev_entry->chain_entry[0], cur_entry, memory_order_relaxed);
            atomic_store_explicit(&prev_entry->chain_epoch[0], epoch, memory_order_release);
        } else if (k0 != key) {
            uint32_t e1    = atomic_load_explicit(&prev_entry->chain_epoch[1], memory_order_relaxed);
            const void *c1 = atomic_load_explicit(&prev_entry->chain_code[1], memory_order_relaxed);
            if (c1 == NULL || e1 != epoch) {
                atomic_store_explicit(&prev_entry->chain_epoch[1], 0U, memory_order_release);
                atomic_store_explicit(&prev_entry->chain_key[1], key, memory_order_relaxed);
                atomic_store_explicit(&prev_entry->chain_code[1], code, memory_order_relaxed);
                atomic_store_explicit(&prev_entry->chain_entry[1], cur_entry, memory_order_relaxed);
                atomic_store_explicit(&prev_entry->chain_epoch[1], epoch, memory_order_release);
            }
        }
    }
    jit_lock_release();
}

int a32_jit_run(a32_ctx_t *ctx)
{
    if (ctx->is_thread_clone) {
        jit_lock_acquire();
        if (!atomic_load_explicit(&g_multithreaded, memory_order_relaxed)) {
            g_code_used = (g_code_used + A32_PAGE_SIZE - 1U) & ~(size_t)(A32_PAGE_SIZE - 1U);
        }
        atomic_store_explicit(&g_multithreaded, true, memory_order_release);
        jit_lock_release();
    }

    a32_cache_entry_t *prev_entry = NULL;
    uint32_t           prev_key   = 0U;
    uint32_t           prev_epoch = 0U;

    for (;;) {
        if ((ctx->pending_signals & ~ctx->sig_mask) != 0ULL) {
            a32_signals_check_pending(ctx);
            prev_entry = NULL;
        }

        bool is_thumb = (ctx->cpsr & A32_CPSR_T) != 0U;
        uint32_t pc   = ctx->regs[15];

        if (ctx->gdb_enabled && (ctx->single_step || a32_gdb_has_breakpoint(pc))) {
            a32_gdb_handle_stop(ctx, 5 /* SIGTRAP */);
            is_thumb   = (ctx->cpsr & A32_CPSR_T) != 0U;
            pc         = ctx->regs[15];
            prev_entry = NULL;
        }

        if (ctx->trace_enabled) {
            fprintf(stderr, "[a32rt] block pc=0x%08x (%s) r0=0x%x r1=0x%x r2=0x%x sp=0x%x lr=0x%x\n",
                    pc, is_thumb ? "T32" : "A32",
                    ctx->regs[0], ctx->regs[1], ctx->regs[2], ctx->regs[13], ctx->regs[14]);
        }

        uint32_t key      = (pc & ~1U) | (is_thumb ? 1U : 0U);
        uint32_t epoch    = atomic_load_explicit(&g_cache_epoch, memory_order_acquire);
        bool     uncached = (ctx->itstate != 0U) || ctx->single_step;

        if (prev_entry != NULL &&
            (epoch != prev_epoch ||
             atomic_load_explicit(&prev_entry->key, memory_order_relaxed) != prev_key)) {
            prev_entry = NULL;
        }

        const void        *code          = NULL;
        a32_cache_entry_t *cur_entry     = NULL;
        a32_cache_entry_t  oneshot_entry;

        if (uncached) {
            prev_entry = NULL;
        }

        /* 1. Fast-path direct chain on prev_entry */
        if (!uncached && prev_entry != NULL &&
            atomic_load_explicit(&prev_entry->epoch, memory_order_acquire) == epoch &&
            atomic_load_explicit(&prev_entry->key, memory_order_relaxed) == prev_key &&
            atomic_load_explicit(&prev_entry->host_code, memory_order_acquire) != NULL) {
            if (atomic_load_explicit(&prev_entry->chain_epoch[0], memory_order_acquire) == epoch &&
                atomic_load_explicit(&prev_entry->chain_key[0], memory_order_relaxed) == key) {
                const void *c0        = atomic_load_explicit(&prev_entry->chain_code[0], memory_order_relaxed);
                a32_cache_entry_t *e0 = atomic_load_explicit(&prev_entry->chain_entry[0], memory_order_relaxed);
                if (c0 != NULL && e0 != NULL &&
                    atomic_load_explicit(&e0->epoch, memory_order_acquire) == epoch &&
                    atomic_load_explicit(&e0->key, memory_order_relaxed) == key &&
                    atomic_load_explicit(&e0->host_code, memory_order_acquire) == c0) {
                    code      = c0;
                    cur_entry = e0;
                    ctx->chain_hits++;
                }
            } else if (atomic_load_explicit(&prev_entry->chain_epoch[1], memory_order_acquire) == epoch &&
                       atomic_load_explicit(&prev_entry->chain_key[1], memory_order_relaxed) == key) {
                const void *c1        = atomic_load_explicit(&prev_entry->chain_code[1], memory_order_relaxed);
                a32_cache_entry_t *e1 = atomic_load_explicit(&prev_entry->chain_entry[1], memory_order_relaxed);
                if (c1 != NULL && e1 != NULL &&
                    atomic_load_explicit(&e1->epoch, memory_order_acquire) == epoch &&
                    atomic_load_explicit(&e1->key, memory_order_relaxed) == key &&
                    atomic_load_explicit(&e1->host_code, memory_order_acquire) == c1) {
                    code      = c1;
                    cur_entry = e1;
                    ctx->chain_hits++;
                }
            }
        }

        /* 1.5. Return Address Stack (RAS) prediction for return targets */
        uint32_t r_idx = (ctx->ras_top - 1U) & (A32_RAS_SIZE - 1U);
        if (key != 0U && ctx->ras_key[r_idx] == key) {
            ctx->ras_top        = r_idx;
            ctx->ras_key[r_idx] = 0U;
            if (!uncached && code == NULL &&
                ctx->ras_epoch[r_idx] == epoch && ctx->ras_code[r_idx] != NULL) {
                a32_cache_entry_t *r_entry = (a32_cache_entry_t *)ctx->ras_entry[r_idx];
                if (r_entry != NULL &&
                    atomic_load_explicit(&r_entry->epoch, memory_order_acquire) == epoch &&
                    atomic_load_explicit(&r_entry->key, memory_order_relaxed) == key &&
                    atomic_load_explicit(&r_entry->host_code, memory_order_acquire) == ctx->ras_code[r_idx]) {
                    code      = ctx->ras_code[r_idx];
                    cur_entry = r_entry;
                    ctx->ras_hits++;
                }
            }
        }

        /* 2. Check per-thread 1024-entry IBTC */
        uint32_t ibtc_idx = ((key >> 1U) ^ (key & 1U)) & (A32_IBTC_SIZE - 1U);
        if (!uncached && code == NULL) {
            if (ctx->ibtc[ibtc_idx].key == key &&
                ctx->ibtc[ibtc_idx].epoch == epoch &&
                ctx->ibtc[ibtc_idx].host_code != NULL) {
                a32_cache_entry_t *i_entry = (a32_cache_entry_t *)ctx->ibtc[ibtc_idx].entry;
                if (i_entry != NULL &&
                    atomic_load_explicit(&i_entry->epoch, memory_order_acquire) == epoch &&
                    atomic_load_explicit(&i_entry->key, memory_order_relaxed) == key &&
                    atomic_load_explicit(&i_entry->host_code, memory_order_acquire) == ctx->ibtc[ibtc_idx].host_code) {
                    code      = ctx->ibtc[ibtc_idx].host_code;
                    cur_entry = i_entry;
                    ctx->ibtc_hits++;
                    try_chain_entries(prev_entry, prev_key, cur_entry, key, epoch, code);
                }
            }
        }

        /* 3. Otherwise lookup or compile */
        if (code == NULL) {
            cur_entry = lookup_or_compile_entry(ctx, pc, is_thumb, &oneshot_entry, true);
            if (cur_entry == NULL) {
                return 1;
            }
            uint32_t new_epoch = atomic_load_explicit(&g_cache_epoch, memory_order_acquire);
            if (new_epoch != epoch ||
                (prev_entry != NULL &&
                 atomic_load_explicit(&prev_entry->key, memory_order_relaxed) != prev_key)) {
                epoch      = new_epoch;
                prev_entry = NULL;
            }
            code = atomic_load_explicit(&cur_entry->host_code, memory_order_acquire);
            if (code == NULL) {
                return 1;
            }
            if (!uncached) {
                ctx->ibtc[ibtc_idx].key       = key;
                ctx->ibtc[ibtc_idx].epoch     = epoch;
                ctx->ibtc[ibtc_idx].host_code = code;
                ctx->ibtc[ibtc_idx].entry     = cur_entry;
                try_chain_entries(prev_entry, prev_key, cur_entry, key, epoch, code);
            }
        }

        bool     cur_is_call = (cur_entry != NULL) ? cur_entry->is_call : false;
        uint32_t cur_start   = (cur_entry != NULL) ? cur_entry->guest_pc : 0U;
        uint32_t cur_end     = (cur_entry != NULL) ? cur_entry->guest_end_pc : 0U;

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
        uint32_t prev_fault_addr = ctx->fault_addr;
        uint64_t prev_sig_mask   = ctx->sig_mask;
        ctx->fault_addr          = 0xFFFFFFFFU;
        uint32_t reason          = a32_jit_run_block(ctx, code);
        bool     sync_sig_taken  = (reason != A32_EXIT_FAULT && ctx->fault_addr != 0xFFFFFFFFU) ||
                                   (ctx->sig_mask != prev_sig_mask) ||
                                   (ctx->regs[14] == 0xFFFF0FA8U);
        if (reason != A32_EXIT_FAULT && ctx->fault_addr == 0xFFFFFFFFU) {
            ctx->fault_addr = prev_fault_addr;
        }
        if (sync_sig_taken) {
            prev_entry = NULL;
        }

        if ((reason == A32_EXIT_BRANCH || reason == A32_EXIT_INTERWORK) && !sync_sig_taken) {
            uint32_t lr_pc   = ctx->regs[14] & ~1U;
            uint32_t next_pc = ctx->regs[15] & ~1U;
            if (cur_is_call && ctx->regs[14] != 0xFFFF0FA8U &&
                lr_pc > cur_start && lr_pc <= cur_end + 4U && next_pc != lr_pc) {
                uint32_t ret_key = lr_pc | (is_thumb ? 1U : 0U);
                uint32_t r_ibtc  = ((ret_key >> 1U) ^ (ret_key & 1U)) & (A32_IBTC_SIZE - 1U);
                uint32_t top     = ctx->ras_top;
                if (ret_key != 0U &&
                    ctx->ibtc[r_ibtc].key == ret_key &&
                    ctx->ibtc[r_ibtc].epoch == epoch &&
                    ctx->ibtc[r_ibtc].host_code != NULL) {
                    ctx->ras_key[top]   = ret_key;
                    ctx->ras_epoch[top] = epoch;
                    ctx->ras_code[top]  = ctx->ibtc[r_ibtc].host_code;
                    ctx->ras_entry[top] = ctx->ibtc[r_ibtc].entry;
                } else {
                    ctx->ras_key[top]   = ret_key;
                    ctx->ras_epoch[top] = 0U;
                    ctx->ras_code[top]  = NULL;
                    ctx->ras_entry[top] = NULL;
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

        case A32_EXIT_SVC:
            prev_entry = NULL;
            ctx->syscalls_executed++;
            if (a32_handle_syscall(ctx) != 0) {
                return (int)ctx->regs[0];
            }
            break;

        case A32_EXIT_BKPT:
            prev_entry = NULL;
            if (ctx->gdb_enabled) {
                a32_gdb_handle_stop(ctx, 5 /* SIGTRAP */);
            } else if (!a32_signals_deliver_sync(ctx, 5 /* SIGTRAP */, ctx->regs[15])) {
                raise(5 /* SIGTRAP */);
            }
            break;

        case A32_EXIT_FAULT:
            fprintf(stderr, "a32rt: guest fault at 0x%08x\n", ctx->fault_addr);
            return 139;

        case A32_EXIT_UNDEF:
            prev_entry = NULL;
            if (ctx->gdb_enabled) {
                a32_gdb_handle_stop(ctx, 4 /* SIGILL */);
                break;
            }
            if (a32_signals_deliver_sync(ctx, 4 /* SIGILL */, ctx->regs[15])) {
                break;
            }
            fprintf(stderr, "a32rt: aborting on exit_reason=%u at guest pc=0x%08x\n",
                    reason, ctx->regs[15]);
            return 127;

        default:
            fprintf(stderr, "a32rt: aborting on exit_reason=%u at guest pc=0x%08x\n",
                    reason, ctx->regs[15]);
            return 127;
        }
    }
}
