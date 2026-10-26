/*
 * src/dbt/dbt_jit_run.c - split-TU part 4/4 of src/jit.c (lines 1267-1740).
 * Compiled only via the src/jit.c wrapper; do not add to Makefile SRCS.
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "dbt/difftest.h"

void a32_jit_iter_blocks(a32_block_iter_fn fn, void *arg)
{
    if (!fn) {
        return;
    }
    jit_lock_acquire();
    uint32_t cur_epoch = atomic_load_explicit(&g_cache_epoch, memory_order_relaxed);
    a32_pc_map_stage_t pc_map[MAX_STAGE_PC_MAP];
    for (size_t a = 0U; a < g_active_ht_count; a++) {
        size_t i              = g_active_ht_idx[a];
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
                set_ht_chain_slot_locked(prev_entry, 0U, key, code, cur_entry, epoch);
            }
        } else {
            bool c1_dead = (c1 == NULL || e1 != epoch || ce1 == NULL ||
                            atomic_load_explicit(&ce1->epoch, memory_order_relaxed) != epoch ||
                            atomic_load_explicit(&ce1->key, memory_order_relaxed) != k1 ||
                            atomic_load_explicit(&ce1->host_code, memory_order_relaxed) != c1);
            if ((c1_dead || k1 == key) && (c1_dead || c1 != code || ce1 != cur_entry)) {
                set_ht_chain_slot_locked(prev_entry, 1U, key, code, cur_entry, epoch);
            }
        }
    }
    jit_lock_release();
}

static int handle_guest_fault(a32_ctx_t *ctx)
{
    if (ctx->gdb_enabled && a32_gdb_handle_stop(ctx, SIGSEGV) == 0) {
        return 0;
    }
    if (a32_signals_deliver_sync(ctx, SIGSEGV, ctx->fault_addr)) {
        a32_signals_check_pending(ctx);
        return 0;
    }
    fprintf(stderr, "a32rt: guest fault at 0x%08x\n", ctx->fault_addr);
    return 128 + SIGSEGV;
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
                a32_gdb_handle_stop(ctx, SIGTRAP);
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

        a32_ctx_normalize_itstate(ctx, is_thumb);
        uint32_t key      = a32_make_key(pc, is_thumb);
        uint32_t epoch    = atomic_load_explicit(&g_cache_epoch, memory_order_acquire);
        bool     uncached = (ctx->itstate != 0U) || ctx->single_step;

        if (prev_entry != NULL &&
            (epoch == 0U || epoch != prev_epoch ||
             atomic_load_explicit(&prev_entry->key, memory_order_relaxed) != prev_key)) {
            prev_entry = NULL;
        }

        const void        *code        = NULL;
        a32_cache_entry_t *cur_entry   = NULL;
        bool               cur_is_call = false;
        uint32_t           cur_start   = 0U;
        uint32_t           cur_end     = 0U;
        a32_cache_entry_t  oneshot_entry;
        uint32_t           ibtc_idx    = a32_ibtc_hash(key, is_thumb);

        if (uncached) {
            prev_entry = NULL;
        }

        /* 1. Return Address Stack (RAS) prediction for return targets */
        uint32_t r_idx = (ctx->ras_top - 1U) & (A32_RAS_SIZE - 1U);
        a32_cache_entry_t *r_ent = (a32_cache_entry_t *)ctx->ras[r_idx].entry;
        uint32_t r_key = (r_ent != NULL)
                             ? atomic_load_explicit(&r_ent->key, memory_order_relaxed)
                             : ctx->ras[r_idx].key;
        if (key != 0U && r_key == key) {
            ctx->ras_top = r_idx;
            ctx->ras[r_idx].key = 0U;
            ctx->ras[r_idx].epoch = 0U;
            ctx->ras[r_idx].host_code = NULL;
            ctx->ras[r_idx].entry = NULL;
            if (!uncached && epoch != 0U && r_ent != NULL) {
                const void *r_code = atomic_load_explicit(&r_ent->host_code, memory_order_relaxed);
                if (validate_cache_candidate(r_ent, r_code, key, epoch,
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
            for (size_t c = 0U; c < 2U; c++) {
                if (atomic_load_explicit(&prev_entry->chain_epoch[c], memory_order_acquire) == epoch &&
                    atomic_load_explicit(&prev_entry->chain_key[c], memory_order_relaxed) == key) {
                    const void *cc        = atomic_load_explicit(&prev_entry->chain_code[c], memory_order_relaxed);
                    a32_cache_entry_t *ec = atomic_load_explicit(&prev_entry->chain_entry[c], memory_order_relaxed);
                    if (validate_cache_candidate(ec, cc, key, epoch,
                                                 &code, &cur_entry, &cur_is_call, &cur_start, &cur_end)) {
                        ctx->chain_hits++;
                        break;
                    }
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
                    int fault_rc = handle_guest_fault(ctx);
                    if (fault_rc == 0) {
                        continue;
                    }
                    return fault_rc;
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
        void    *run_entry      = uncached ? (void *)&g_dummy_entry : (void *)cur_entry;
        ctx->last_exec_entry    = run_entry;
        uint32_t reason         = a32_jit_run_block(ctx, code, run_entry, &g_cache_epoch);
        is_thumb                = (ctx->cpsr & A32_CPSR_T) != 0U;
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
                uint32_t ret_key = a32_make_key(lr_pc, is_thumb);
                uint32_t r_ibtc  = a32_ibtc_hash(ret_key, is_thumb);
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
                        set_ht_ret_slot_locked(cur_entry, ret_key, found_code, found_e, epoch);
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
            ctx->regs[15] &= is_thumb ? ~1U : ~3U;
            break;

        case A32_EXIT_INTERWORK:
            a32_set_pc_interwork(ctx, ctx->regs[15]);
            break;

        case A32_EXIT_SVC: {
            prev_entry = NULL;
            ctx->syscalls_executed++;
            uint32_t pre_stop_pc = ctx->regs[15];
            bool     is_bkpt_svc = (ctx->regs[7] == 0x0f0001U);
            if (a32_handle_syscall(ctx) != 0) {
                return (int)ctx->regs[0];
            }
            if (ctx->gdb_enabled && is_bkpt_svc) {
                gdb_stopped_at_pc = (ctx->regs[15] == pre_stop_pc);
                gdb_stopped_pc    = pre_stop_pc;
            }
            break;
        }

        case A32_EXIT_BKPT: {
            prev_entry = NULL;
            if (ctx->gdb_enabled) {
                uint32_t pre_stop_pc = ctx->regs[15];
                a32_gdb_handle_stop(ctx, SIGTRAP);
                gdb_stopped_at_pc = (ctx->regs[15] == pre_stop_pc);
                gdb_stopped_pc    = pre_stop_pc;
            } else if (!a32_signals_deliver_sync(ctx, SIGTRAP, ctx->regs[15])) {
                raise(SIGTRAP);
            }
            break;
        }

        case A32_EXIT_FAULT: {
            prev_entry = NULL;
            int fault_rc = handle_guest_fault(ctx);
            if (fault_rc == 0) {
                continue;
            }
            return fault_rc;
        }

        case A32_EXIT_UNDEF: {
            prev_entry = NULL;
            /* RC-U3: single-step the faulting insn through the reference
             * interpreter before raising SIGILL, so encodings the DBT
             * has not implemented yet still execute. */
            {
                uint32_t step_rc = a32_interp_step(ctx);
                if (step_rc == A32_EXIT_NONE ||
                    step_rc == A32_EXIT_BRANCH ||
                    step_rc == A32_EXIT_INTERWORK) {
                    break;
                }
                if (step_rc == A32_EXIT_SVC) {
                    ctx->syscalls_executed++;
                    uint32_t pre_step_pc = ctx->regs[15];
                    bool pre_step_bkpt = (ctx->regs[7] == 0x0f0001U);
                    if (a32_handle_syscall(ctx) != 0) {
                        return (int)ctx->regs[0];
                    }
                    if (ctx->gdb_enabled && pre_step_bkpt) {
                        gdb_stopped_at_pc = (ctx->regs[15] == pre_step_pc);
                        gdb_stopped_pc = pre_step_pc;
                    }
                    break;
                }
                if (step_rc == A32_EXIT_FAULT) {
                    int step_fault_rc = handle_guest_fault(ctx);
                    if (step_fault_rc == 0) {
                        continue;
                    }
                    return step_fault_rc;
                }
            }
            if (ctx->gdb_enabled) {
                uint32_t pre_stop_pc = ctx->regs[15];
                a32_gdb_handle_stop(ctx, SIGILL);
                gdb_stopped_at_pc = (ctx->regs[15] == pre_stop_pc);
                gdb_stopped_pc    = pre_stop_pc;
                break;
            }
            if (a32_signals_deliver_sync(ctx, SIGILL, ctx->regs[15])) {
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

/* RC-U3: exactly one DBT block for the --difftest lockstep hook. Compiles
 * (or finds) the block at the current PC and runs it to its exit reason
 * without dispatching syscalls or signals; the difftest loop owns that
 * policy identically on both copies. */
uint32_t a32_dbt_single_step(a32_ctx_t *ctx)
{
    bool is_thumb;
    uint32_t pc;
    uint32_t code_bytes = 0U;
    const void *code;
    uint32_t reason;
    if (ctx == NULL) {
        return A32_EXIT_UNDEF;
    }
    is_thumb = (ctx->cpsr & A32_CPSR_T) != 0U;
    pc = ctx->regs[15] & (is_thumb ? ~1U : ~3U);
    ctx->exit_reason = A32_EXIT_NONE;
    code = a32_jit_compile_block(ctx, pc, is_thumb, &code_bytes);
    if (code == NULL || code_bytes == 0U) {
        if (ctx->exit_reason == A32_EXIT_NONE) {
            ctx->exit_reason = A32_EXIT_UNDEF;
        }
        return ctx->exit_reason;
    }
    ctx->blocks_executed++;
    ctx->sync_sig_delivered = false;
    reason = a32_jit_run_block(ctx, code, (void *)&g_dummy_entry,
        &g_cache_epoch);
    ctx->exit_reason = reason;
    return reason;
}
