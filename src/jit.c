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
#define A32_IBTC_SIZE       1024U
#define A32_SIDE_TABLE_SIZE 65536U
#define MAX_STAGE_PC_MAP    256U

typedef struct a32_cache_entry {
    uint32_t                 key;        /* (guest_pc & ~1u) | T; 0 = empty */
    uint32_t                 guest_pc;
    uint32_t                 code_bytes;
    uint32_t                 epoch;
    const void              *host_code;
    uint32_t                 chain_key[2];
    uint32_t                 chain_epoch[2];
    const void              *chain_code[2];
    struct a32_cache_entry  *chain_entry[2];
} a32_cache_entry_t;

typedef struct a32_ibtc_entry {
    uint32_t           key;
    uint32_t           epoch;
    const void        *host_code;
    a32_cache_entry_t *entry;
} a32_ibtc_entry_t;

typedef struct a32_side_entry {
    uintptr_t host_pc_start;
    uintptr_t host_pc_end;
    uint32_t  guest_pc;
    uint32_t  itstate;
} a32_side_entry_t;

typedef struct a32_pc_map_stage {
    uint32_t host_word_idx;
    uint32_t guest_pc;
    uint32_t itstate;
} a32_pc_map_stage_t;

static _Atomic uint32_t   g_jit_lock = 0;
static _Atomic uint32_t   g_cache_epoch = 1;

static uint8_t           *g_code_cache = NULL;
static size_t             g_code_used  = 0U;
static bool               g_page_is_rw[A32_CACHE_PAGES];
static uintptr_t          g_batch_min  = UINTPTR_MAX;
static uintptr_t          g_batch_max  = 0U;

static a32_cache_entry_t  g_ht[A32_HT_SIZE];
static a32_ibtc_entry_t   g_ibtc[A32_IBTC_SIZE];

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
    (void)ctx;
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
    memset(g_ht, 0, sizeof(g_ht));
    memset(g_ibtc, 0, sizeof(g_ibtc));
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

    uint8_t *dst   = g_code_cache + g_code_used;
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
                                     uint32_t block_guest_pc, uint32_t block_itstate)
{
    uintptr_t base = (uintptr_t)dst;
    if (g_stage_pc_map_count == 0U) {
        size_t idx = (g_side_table_count < A32_SIDE_TABLE_SIZE) ?
                     g_side_table_count++ :
                     (g_side_table_count++ % A32_SIDE_TABLE_SIZE);
        g_side_table[idx].host_pc_start = base;
        g_side_table[idx].host_pc_end   = base + code_bytes;
        g_side_table[idx].guest_pc      = block_guest_pc;
        g_side_table[idx].itstate       = block_itstate;
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
        size_t idx = (g_side_table_count < A32_SIDE_TABLE_SIZE) ?
                     g_side_table_count++ :
                     (g_side_table_count++ % A32_SIDE_TABLE_SIZE);
        g_side_table[idx].host_pc_start = seg_start;
        g_side_table[idx].host_pc_end   = seg_end;
        g_side_table[idx].guest_pc      = g_stage_pc_map[i].guest_pc;
        g_side_table[idx].itstate       = g_stage_pc_map[i].itstate;
    }
}

void a32_jit_record_pc_map(uint32_t host_word_idx, uint32_t guest_pc, uint32_t itstate)
{
    if (g_stage_pc_map_count < MAX_STAGE_PC_MAP) {
        g_stage_pc_map[g_stage_pc_map_count].host_word_idx = host_word_idx;
        g_stage_pc_map[g_stage_pc_map_count].guest_pc      = guest_pc;
        g_stage_pc_map[g_stage_pc_map_count].itstate       = itstate;
        g_stage_pc_map_count++;
    }
}

bool a32_jit_lookup_host_pc(uintptr_t host_pc, uint32_t *out_guest_pc, uint32_t *out_itstate)
{
    jit_lock_acquire();
    size_t count = g_side_table_count;
    if (count > A32_SIDE_TABLE_SIZE) {
        count = A32_SIDE_TABLE_SIZE;
    }
    for (size_t i = count; i > 0U; i--) {
        size_t idx = i - 1U;
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
    memset(g_ibtc, 0, sizeof(g_ibtc));
    g_side_table_count = 0U;

    if (ctx->exec_path != NULL) {
        (void)a32_aot_load_cache(ctx, ctx->exec_path);
    }
    return 0;
}

int a32_jit_invalidate_range(a32_ctx_t *ctx, uint32_t start, uint32_t end)
{
    (void)ctx;
    jit_lock_acquire();
    atomic_fetch_add_explicit(&g_cache_epoch, 1U, memory_order_acq_rel);
    for (size_t i = 0U; i < A32_HT_SIZE; i++) {
        if (g_ht[i].key != 0U) {
            uint32_t pc = g_ht[i].guest_pc;
            if (pc >= start && pc < end) {
                g_ht[i].key           = 0U;
                g_ht[i].host_code     = NULL;
                g_ht[i].chain_key[0]  = 0U;
                g_ht[i].chain_code[0] = NULL;
                g_ht[i].chain_entry[0]= NULL;
                g_ht[i].chain_key[1]  = 0U;
                g_ht[i].chain_code[1] = NULL;
                g_ht[i].chain_entry[1]= NULL;
            }
        }
    }
    jit_lock_release();
    return 0;
}

int a32_jit_install_aot_block(a32_ctx_t *ctx, uint32_t guest_pc, bool is_thumb,
                              const uint32_t *words, size_t word_count)
{
    if (words == NULL || word_count == 0U) {
        return -1;
    }

    uint32_t key = (guest_pc & ~1U) | (is_thumb ? 1U : 0U);
    uint32_t slot = hash_key(key);

    jit_lock_acquire();

    for (size_t probe = 0U; probe < 16U; probe++) {
        uint32_t idx = (slot + (uint32_t)probe) & A32_HT_MASK;
        if (g_ht[idx].key == key && g_ht[idx].host_code != NULL) {
            jit_lock_release();
            return 0;
        }
        if (g_ht[idx].key == 0U) {
            break;
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
    commit_side_table_locked(dst, code_bytes, guest_pc & ~1U, 0U);

    uint32_t cur_epoch = atomic_load_explicit(&g_cache_epoch, memory_order_relaxed);

    bool inserted = false;
    for (size_t probe = 0U; probe < 16U; probe++) {
        uint32_t idx = (slot + (uint32_t)probe) & A32_HT_MASK;
        if (g_ht[idx].key == 0U || g_ht[idx].key == key) {
            g_ht[idx].key            = key;
            g_ht[idx].host_code      = dst;
            g_ht[idx].guest_pc       = guest_pc & ~1U;
            g_ht[idx].code_bytes     = (uint32_t)code_bytes;
            g_ht[idx].epoch          = cur_epoch;
            g_ht[idx].chain_key[0]   = 0U;
            g_ht[idx].chain_epoch[0] = 0U;
            g_ht[idx].chain_code[0]  = NULL;
            g_ht[idx].chain_entry[0] = NULL;
            g_ht[idx].chain_key[1]   = 0U;
            g_ht[idx].chain_epoch[1] = 0U;
            g_ht[idx].chain_code[1]  = NULL;
            g_ht[idx].chain_entry[1] = NULL;
            inserted = true;
            break;
        }
    }
    if (!inserted) {
        g_ht[slot].key            = key;
        g_ht[slot].host_code      = dst;
        g_ht[slot].guest_pc       = guest_pc & ~1U;
        g_ht[slot].code_bytes     = (uint32_t)code_bytes;
        g_ht[slot].epoch          = cur_epoch;
        g_ht[slot].chain_key[0]   = 0U;
        g_ht[slot].chain_epoch[0] = 0U;
        g_ht[slot].chain_code[0]  = NULL;
        g_ht[slot].chain_entry[0] = NULL;
        g_ht[slot].chain_key[1]   = 0U;
        g_ht[slot].chain_epoch[1] = 0U;
        g_ht[slot].chain_code[1]  = NULL;
        g_ht[slot].chain_entry[1] = NULL;
    }

    jit_lock_release();
    return 0;
}

static a32_cache_entry_t *lookup_or_compile_entry(a32_ctx_t *ctx, uint32_t pc, bool is_thumb)
{
    uint32_t key  = (pc & ~1U) | (is_thumb ? 1U : 0U);
    uint32_t slot = hash_key(key);

    jit_lock_acquire();
    for (size_t probe = 0U; probe < 16U; probe++) {
        uint32_t idx = (slot + (uint32_t)probe) & A32_HT_MASK;
        if (g_ht[idx].key == key && g_ht[idx].host_code != NULL) {
            jit_lock_release();
            return &g_ht[idx];
        }
        if (g_ht[idx].key == 0U) {
            break;
        }
    }

    g_stage_pc_map_count = 0U;
    a32_jit_record_pc_map(0U, pc & ~1U, is_thumb ? ctx->itstate : 0U);

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
    commit_side_table_locked(dst, code_bytes, pc & ~1U, is_thumb ? ctx->itstate : 0U);

    uint32_t cur_epoch = atomic_load_explicit(&g_cache_epoch, memory_order_relaxed);
    a32_cache_entry_t *res_entry = NULL;

    for (size_t probe = 0U; probe < 16U; probe++) {
        uint32_t idx = (slot + (uint32_t)probe) & A32_HT_MASK;
        if (g_ht[idx].key == 0U || g_ht[idx].key == key) {
            g_ht[idx].key            = key;
            g_ht[idx].host_code      = dst;
            g_ht[idx].guest_pc       = pc & ~1U;
            g_ht[idx].code_bytes     = (uint32_t)code_bytes;
            g_ht[idx].epoch          = cur_epoch;
            g_ht[idx].chain_key[0]   = 0U;
            g_ht[idx].chain_epoch[0] = 0U;
            g_ht[idx].chain_code[0]  = NULL;
            g_ht[idx].chain_entry[0] = NULL;
            g_ht[idx].chain_key[1]   = 0U;
            g_ht[idx].chain_epoch[1] = 0U;
            g_ht[idx].chain_code[1]  = NULL;
            g_ht[idx].chain_entry[1] = NULL;
            res_entry = &g_ht[idx];
            break;
        }
    }
    if (res_entry == NULL) {
        g_ht[slot].key            = key;
        g_ht[slot].host_code      = dst;
        g_ht[slot].guest_pc       = pc & ~1U;
        g_ht[slot].code_bytes     = (uint32_t)code_bytes;
        g_ht[slot].epoch          = cur_epoch;
        g_ht[slot].chain_key[0]   = 0U;
        g_ht[slot].chain_epoch[0] = 0U;
        g_ht[slot].chain_code[0]  = NULL;
        g_ht[slot].chain_entry[0] = NULL;
        g_ht[slot].chain_key[1]   = 0U;
        g_ht[slot].chain_epoch[1] = 0U;
        g_ht[slot].chain_code[1]  = NULL;
        g_ht[slot].chain_entry[1] = NULL;
        res_entry = &g_ht[slot];
    }

    flush_batch_locked(ctx);
    ctx->blocks_compiled++;

    jit_lock_release();
    return res_entry;
}

const void *a32_jit_compile_block(a32_ctx_t *ctx, uint32_t pc, bool is_thumb, uint32_t *out_code_bytes)
{
    a32_cache_entry_t *entry = lookup_or_compile_entry(ctx, pc, is_thumb);
    if (entry == NULL) {
        return NULL;
    }
    if (out_code_bytes != NULL) {
        *out_code_bytes = entry->code_bytes;
    }
    return entry->host_code;
}

typedef void (*a32_block_iter_fn)(uint32_t guest_pc, bool is_thumb,
                                  const uint32_t *words, size_t word_count, void *arg);

void a32_jit_iter_blocks(a32_block_iter_fn fn, void *arg)
{
    jit_lock_acquire();
    for (size_t i = 0U; i < A32_HT_SIZE; i++) {
        if (g_ht[i].key != 0U && g_ht[i].host_code != NULL && g_ht[i].code_bytes > 0U) {
            uint32_t guest_pc = g_ht[i].guest_pc;
            bool is_thumb = (g_ht[i].key & 1U) != 0U;
            const uint32_t *words = (const uint32_t *)g_ht[i].host_code;
            size_t word_count = g_ht[i].code_bytes / sizeof(uint32_t);
            fn(guest_pc, is_thumb, words, word_count, arg);
        }
    }
    jit_lock_release();
}

int a32_jit_run(a32_ctx_t *ctx)
{
    a32_cache_entry_t *prev_entry = NULL;

    for (;;) {
        if (ctx->pending_signals != 0) {
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

        uint32_t key   = (pc & ~1U) | (is_thumb ? 1U : 0U);
        uint32_t epoch = atomic_load_explicit(&g_cache_epoch, memory_order_relaxed);

        const void        *code      = NULL;
        a32_cache_entry_t *cur_entry = NULL;

        /* 1. Fast-path direct chain on prev_entry */
        if (prev_entry != NULL) {
            if (prev_entry->chain_key[0] == key &&
                prev_entry->chain_epoch[0] == epoch &&
                prev_entry->chain_code[0] != NULL) {
                code      = prev_entry->chain_code[0];
                cur_entry = prev_entry->chain_entry[0];
                ctx->chain_hits++;
            } else if (prev_entry->chain_key[1] == key &&
                       prev_entry->chain_epoch[1] == epoch &&
                       prev_entry->chain_code[1] != NULL) {
                code      = prev_entry->chain_code[1];
                cur_entry = prev_entry->chain_entry[1];
                ctx->chain_hits++;
            }
        }

        /* 1.5. Return Address Stack (RAS) prediction for return targets */
        bool ras_matched = false;
        uint32_t r_idx = (ctx->ras_top - 1U) & (A32_RAS_SIZE - 1U);
        if (ctx->ras_key[r_idx] == key) {
            ctx->ras_top = r_idx;
            ctx->ras_key[r_idx] = 0U;
            ras_matched = true;
            if (code == NULL && ctx->ras_epoch[r_idx] == epoch && ctx->ras_code[r_idx] != NULL) {
                code      = ctx->ras_code[r_idx];
                cur_entry = (a32_cache_entry_t *)ctx->ras_entry[r_idx];
                ctx->ras_hits++;
            }
        }

        /* 2. Check 1024-entry IBTC */
        uint32_t ibtc_idx = ((key >> 1U) ^ (key & 1U)) & (A32_IBTC_SIZE - 1U);
        if (code == NULL) {
            if (g_ibtc[ibtc_idx].key == key &&
                g_ibtc[ibtc_idx].epoch == epoch &&
                g_ibtc[ibtc_idx].host_code != NULL) {
                code      = g_ibtc[ibtc_idx].host_code;
                cur_entry = g_ibtc[ibtc_idx].entry;
                ctx->ibtc_hits++;
                if (ras_matched) {
                    ctx->ras_hits++;
                }

                if (prev_entry != NULL && cur_entry != NULL) {
                    if (prev_entry->chain_code[0] == NULL || prev_entry->chain_epoch[0] != epoch) {
                        prev_entry->chain_key[0]   = key;
                        prev_entry->chain_epoch[0] = epoch;
                        prev_entry->chain_code[0]  = code;
                        prev_entry->chain_entry[0] = cur_entry;
                    } else if (prev_entry->chain_key[0] != key &&
                               (prev_entry->chain_code[1] == NULL || prev_entry->chain_epoch[1] != epoch)) {
                        prev_entry->chain_key[1]   = key;
                        prev_entry->chain_epoch[1] = epoch;
                        prev_entry->chain_code[1]  = code;
                        prev_entry->chain_entry[1] = cur_entry;
                    }
                }
            }
        }

        /* 3. Otherwise lookup or compile */
        if (code == NULL) {
            cur_entry = lookup_or_compile_entry(ctx, pc, is_thumb);
            if (cur_entry == NULL) {
                return 1;
            }
            code = cur_entry->host_code;
            g_ibtc[ibtc_idx].key       = key;
            g_ibtc[ibtc_idx].epoch     = epoch;
            g_ibtc[ibtc_idx].host_code = code;
            g_ibtc[ibtc_idx].entry     = cur_entry;
            if (ras_matched) {
                ctx->ras_hits++;
            }

            if (prev_entry != NULL && cur_entry != NULL) {
                if (prev_entry->chain_code[0] == NULL || prev_entry->chain_epoch[0] != epoch) {
                    prev_entry->chain_key[0]   = key;
                    prev_entry->chain_epoch[0] = epoch;
                    prev_entry->chain_code[0]  = code;
                    prev_entry->chain_entry[0] = cur_entry;
                } else if (prev_entry->chain_key[0] != key &&
                           (prev_entry->chain_code[1] == NULL || prev_entry->chain_epoch[1] != epoch)) {
                    prev_entry->chain_key[1]   = key;
                    prev_entry->chain_epoch[1] = epoch;
                    prev_entry->chain_code[1]  = code;
                    prev_entry->chain_entry[1] = cur_entry;
                }
            }
        }

        prev_entry = cur_entry;
        ctx->blocks_executed++;
        uint32_t prev_lr = ctx->regs[14];
        uint32_t reason = a32_jit_run_block(ctx, code);

        if (reason == A32_EXIT_BRANCH || reason == A32_EXIT_INTERWORK) {
            if (ctx->regs[14] != prev_lr) {
                uint32_t ret_key = (ctx->regs[14] & ~1U) | (ctx->regs[14] & 1U);
                uint32_t r_ibtc = ((ret_key >> 1U) ^ (ret_key & 1U)) & (A32_IBTC_SIZE - 1U);
                uint32_t top = ctx->ras_top;
                if (g_ibtc[r_ibtc].key == ret_key &&
                    g_ibtc[r_ibtc].epoch == epoch &&
                    g_ibtc[r_ibtc].host_code != NULL) {
                    ctx->ras_key[top]   = ret_key;
                    ctx->ras_epoch[top] = epoch;
                    ctx->ras_code[top]  = g_ibtc[r_ibtc].host_code;
                    ctx->ras_entry[top] = g_ibtc[r_ibtc].entry;
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
            } else {
                raise(5 /* SIGTRAP */);
            }
            break;

        case A32_EXIT_FAULT:
            fprintf(stderr, "a32rt: guest fault at 0x%08x\n", ctx->fault_addr);
            return 139;

        case A32_EXIT_UNDEF:
        default:
            fprintf(stderr, "a32rt: aborting on exit_reason=%u at guest pc=0x%08x\n",
                    reason, ctx->regs[15]);
            return 127;
        }
    }
}
