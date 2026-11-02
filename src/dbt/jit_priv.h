/*
 * src/dbt/jit_priv.h — Internal declarations for JIT cache, state, query, and execution
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef A32RT_JIT_PRIV_H
#define A32RT_JIT_PRIV_H

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
#define A32_AOT_MAX_WORDS   4096U
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

#define A32_PENDING_BUCKETS 4096U
#define A32_PENDING_MASK    (A32_PENDING_BUCKETS - 1U)
#define A32_PENDING_WAYS    4U

typedef struct a32_pending_bucket {
    a32_cache_entry_t *callers[A32_PENDING_WAYS];
    uint8_t            next_way;
} a32_pending_bucket_t;

extern _Atomic uint32_t     g_jit_lock;
extern _Atomic uint32_t     g_cache_epoch;
extern _Atomic bool         g_multithreaded;
extern _Atomic uint32_t     g_jit_exec_readers;
extern _Atomic uint32_t     g_jit_reset_pending;

extern uint8_t             *g_code_cache;
extern size_t               g_code_used;
extern bool                 g_page_is_rw[A32_CACHE_PAGES];
extern size_t               g_first_rw_page;
extern uintptr_t            g_batch_min;
extern uintptr_t            g_batch_max;

extern a32_cache_entry_t   *g_ht;
extern uint32_t             g_active_ht_idx[A32_HT_SIZE];
extern size_t               g_active_ht_count;
extern uint64_t             g_code_page_bitmap[16384];
extern a32_cache_entry_t    g_dummy_entry;
extern a32_pending_bucket_t g_pending_slot_callers[A32_PENDING_BUCKETS];
extern a32_pending_bucket_t g_pending_ret_callers[A32_PENDING_BUCKETS];

extern a32_side_entry_t    *g_side_table;
extern _Atomic size_t       g_side_table_count;

extern a32_pc_map_stage_t   g_stage_pc_map[MAX_STAGE_PC_MAP];
extern size_t               g_stage_pc_map_count;

static inline void jit_lock_acquire(void)
{
    a32_spin_lock(&g_jit_lock);
}

static inline void jit_lock_release(void)
{
    a32_spin_unlock(&g_jit_lock);
}

static inline void a32_jit_exec_enter(void)
{
    for (;;) {
        while (atomic_load_explicit(&g_jit_reset_pending, memory_order_acquire) != 0U) {
            __asm__ volatile("yield" ::: "memory");
        }
        atomic_fetch_add_explicit(&g_jit_exec_readers, 1U, memory_order_acq_rel);
        if (atomic_load_explicit(&g_jit_reset_pending, memory_order_acquire) == 0U) {
            return;
        }
        atomic_fetch_sub_explicit(&g_jit_exec_readers, 1U, memory_order_acq_rel);
    }
}

static inline void a32_jit_exec_leave(void)
{
    atomic_fetch_sub_explicit(&g_jit_exec_readers, 1U, memory_order_acq_rel);
}

static inline uint32_t hash_key(uint32_t key)
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

a32_cache_entry_t *a32_jit_find_ht_slot_for_insert_locked(uint32_t key, uint32_t cur_epoch,
                                                          bool *out_exists);
a32_cache_entry_t *a32_jit_find_ht_entry_locked(uint32_t key, uint32_t cur_epoch);
void a32_jit_reset_cache_locked(a32_ctx_t *ctx);
void a32_jit_fork_prepare(void);
void a32_jit_fork_parent(void);
void a32_jit_fork_child(void);
int a32_jit_flush_batch_locked(a32_ctx_t *ctx);
uint8_t *a32_jit_allocate_code_space_locked(a32_ctx_t *ctx, size_t code_bytes);
void a32_jit_commit_side_table_locked(const void *dst, size_t code_bytes,
                                      uint32_t block_guest_pc, uint32_t block_itstate,
                                      size_t *out_side_start, size_t *out_side_count);
void a32_jit_populate_ht_entry_locked(a32_cache_entry_t *entry, uint32_t key, const void *dst,
                                      uint32_t guest_pc, uint32_t guest_end_pc, uint32_t code_bytes,
                                      bool is_call, size_t side_start, size_t side_count,
                                      uint32_t cur_epoch,
                                      const uint32_t *words, size_t word_count,
                                      bool link_reverse);
a32_cache_entry_t *a32_jit_lookup_or_compile_entry(a32_ctx_t *ctx, uint32_t pc, bool is_thumb,
                                                   a32_cache_entry_t *oneshot_entry, bool flush_now,
                                                   a32_block_snap_t *out_snap);

#endif /* A32RT_JIT_PRIV_H */
