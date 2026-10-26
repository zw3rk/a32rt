/*
 * src/dbt/difftest.c — --interp / --dbt / --difftest execution-mode harness
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wiring note (RC-U3): src/main.c installs a32_dbt_single_step() via
 * a32_difftest_set_dbt_step() at startup and dispatches --interp / --dbt /
 * --difftest to a32_difftest_run_* instead of a32_jit_run, so --difftest
 * always executes DBT against the reference interpreter lockstep.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "difftest.h"
#include "os/backend.h"
#include "vm/vm32.h"

#include <errno.h>
#include <sys/mman.h>

static a32_dbt_step_fn g_dbt_step = NULL;

void a32_difftest_set_dbt_step(a32_dbt_step_fn fn)
{
    g_dbt_step = fn;
}

bool a32_difftest_parse_arg(const char *arg, a32_exec_mode_t *out)
{
    if (arg == NULL || out == NULL) {
        return false;
    }
    if (strcmp(arg, "--interp") == 0 || strcmp(arg, "interp") == 0) {
        *out = A32_EXEC_INTERP;
        return true;
    }
    if (strcmp(arg, "--dbt") == 0 || strcmp(arg, "dbt") == 0) {
        *out = A32_EXEC_DBT;
        return true;
    }
    if (strcmp(arg, "--difftest") == 0 || strcmp(arg, "difftest") == 0) {
        *out = A32_EXEC_DIFFTEST;
        return true;
    }
    return false;
}

const char *a32_difftest_mode_name(a32_exec_mode_t mode)
{
    switch (mode) {
    case A32_EXEC_INTERP: return "interp";
    case A32_EXEC_DIFFTEST: return "difftest";
    default: return "dbt";
    }
}

/* --interp: pure reference interpreter to completion. */
int a32_difftest_run_interp(a32_ctx_t *ctx, uint64_t max_steps,
    a32_difftest_stats_t *stats)
{
    a32_difftest_stats_t s;
    memset(&s, 0, sizeof(s));
    int rc = a32_interp_run(ctx, max_steps);
    s.steps = ctx->blocks_executed;
    s.interp_hash = a32_interp_state_hash(ctx);
    if (stats != NULL) {
        *stats = s;
    }
    return rc;
}

/* --dbt: existing JIT engine unchanged (reference for --difftest). */
int a32_difftest_run_dbt(a32_ctx_t *ctx, a32_difftest_stats_t *stats)
{
    int rc = a32_jit_run(ctx);
    if (stats != NULL) {
        memset(stats, 0, sizeof(*stats));
        stats->steps = ctx->blocks_executed;
        stats->dbt_hash = a32_interp_state_hash(ctx);
    }
    return rc;
}

/* --difftest: lockstep interp vs DBT from the same entry state.
 *
 * Both contexts start as copies of *ctx. Each iteration single-steps the
 * interpreter on the ref copy and one DBT instruction on the dut copy, then
 * compares guest-visible state (GPRs, CPSR, FPSCR, VFP). The first
 * divergence (PC included, exit reasons included) is reported. Returns the
 * guest exit code when both sides exit cleanly, 1 on any register/PC
 * divergence, on zero compared instructions, or on any unexpected
 * UNDEF/fault on either side.
 *
 * Without an installed DBT hook this degrades to interp coverage mode:
 * it runs the reference stepper and counts EXIT_UNDEF fallbacks, which
 * is exactly the metric U3 needs before JIT lockstep lands.
 */

static int difftest_done(a32_ctx_t *ctx, const a32_ctx_t *ref,
    const a32_ctx_t *dut, a32_difftest_stats_t *s,
    a32_difftest_stats_t *stats, int rc)
{
    s->interp_hash = a32_interp_state_hash(ref);
    s->dbt_hash = a32_interp_state_hash(dut);
    memcpy(ctx, ref, sizeof(*ctx));
    if (stats != NULL) {
        *stats = *s;
    }
    return rc;
}
/* ---- RC-U3-r2 lockstep memory/syscall determinism ----
 *
 * 1:1 regime: a32_dbt_single_step caps translation at max_insns = 1 and the
 * dut ibtc/ras stay zeroed so the trampoline cannot chain a second block
 * inside one run_block call. One interp step is compared against exactly
 * one DBT instruction; mismatched exit reasons diverge.
 *
 * On the A1 direct regime the dut runs in a private 4 GiB window that is
 * remirrored from ref after every syscall, with a memory hash compared
 * before each sync. Syscalls execute exactly once (on ref) and the result
 * is mirrored, so host-side effects cannot diverge. On A2 (arena-global
 * Soft-TLB addends) both copies share the window and only the
 * single-execution/mirror applies. Zero compared instructions or any
 * divergence fails. */

#define DIFFTEST_WINDOW_SIZE 0x100000000ULL

static uint64_t difftest_mix(uint64_t h, uint64_t v)
{
    h ^= v + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    return h;
}

/* Hash over mapped guest memory: region bounds/prot plus contents.
 * Unreadable regions contribute bounds only (neither engine can observe
 * their bytes without faulting identically). */
static uint64_t difftest_mem_hash(const a32_ctx_t *ctx)
{
    uint64_t h = 0xCBF29CE484222325ULL;
    size_t n = (ctx->region_count <= A32_MAX_REGIONS) ? ctx->region_count : A32_MAX_REGIONS;
    size_t i;
    h = difftest_mix(h, (uint64_t)n);
    for (i = 0U; i < n; i++) {
        const a32_region_t *r = &ctx->regions[i];
        uint64_t len = (uint64_t)r->end - (uint64_t)r->start;
        const uint8_t *p;
        uint64_t off;
        h = difftest_mix(h, (uint64_t)r->start);
        h = difftest_mix(h, (uint64_t)r->end);
        h = difftest_mix(h, (uint64_t)r->prot);
        if (len == 0U || len > DIFFTEST_WINDOW_SIZE || (r->prot & 0x1U) == 0U) {
            continue;
        }
        p = (const uint8_t *)a32_guest_ptr(ctx, r->start);
        for (off = 0U; off < len; off++) {
            h = difftest_mix(h, (uint64_t)p[off]);
        }
    }
    return h;
}

/* Replicate one ref region into the dut window: RW stage, byte copy when
 * readable, then the final guest protection. Best effort; failures are
 * reported and abort the comparison. */
static int difftest_replicate_region(const a32_ctx_t *ref, uint64_t dut_base,
    const a32_region_t *r)
{
    uint64_t len = (uint64_t)r->end - (uint64_t)r->start;
    void *dst = (void *)(uintptr_t)(dut_base + (uint64_t)r->start);
    void *out = NULL;
    int final_prot = a32_host_prot(ref, r->prot);
    int map_rc;
    if (len == 0U || len > DIFFTEST_WINDOW_SIZE) {
        return -EINVAL;
    }
    map_rc = a32_backend_map_anon(dst, (size_t)len, PROT_READ | PROT_WRITE,
        1, 1, &out);
    if (map_rc != 0 || out != dst) {
        return (map_rc != 0) ? map_rc : -EIO;
    }
    if ((r->prot & 0x1U) != 0U) {
        memcpy(dst, a32_guest_ptr(ref, r->start), (size_t)len);
    }
    if (final_prot != (PROT_READ | PROT_WRITE)) {
        int prot_rc = a32_backend_protect(dst, (size_t)len, final_prot);
        if (prot_rc != 0) {
            return prot_rc;
        }
    }
    return 0;
}

static void difftest_mirror_regs(const a32_ctx_t *ref, a32_ctx_t *dut);

/* Mirror the ref address space into the dut window: drop ranges that
 * vanished, replicate current regions, then copy register state (keeping
 * the dut window base and a zeroed ibtc/ras so single-step cannot chain).
 * Returns 0 on success, negative errno otherwise. */
static int difftest_sync_dut(const a32_ctx_t *ref, a32_ctx_t *dut)
{
    size_t rn = (ref->region_count <= A32_MAX_REGIONS) ? ref->region_count : A32_MAX_REGIONS;
    size_t dn = (dut->region_count <= A32_MAX_REGIONS) ? dut->region_count : A32_MAX_REGIONS;
    uint64_t dut_base = dut->guest_base;
    size_t i;
    size_t j;
    for (i = 0U; i < dn; i++) {
        bool gone = true;
        for (j = 0U; j < rn; j++) {
            if (dut->regions[i].start == ref->regions[j].start &&
                dut->regions[i].end == ref->regions[j].end) {
                gone = false;
                break;
            }
        }
        if (gone) {
            void *dst = (void *)(uintptr_t)(dut_base + (uint64_t)dut->regions[i].start);
            uint64_t len = (uint64_t)dut->regions[i].end - (uint64_t)dut->regions[i].start;
            void *out = NULL;
            if (len > 0U && len <= DIFFTEST_WINDOW_SIZE) {
                (void)a32_backend_map_anon(dst, (size_t)len, 0, 1, 1, &out);
            }
        }
    }
    for (j = 0U; j < rn; j++) {
        int rc = difftest_replicate_region(ref, dut_base, &ref->regions[j]);
        if (rc != 0) {
            return rc;
        }
    }
    difftest_mirror_regs(ref, dut);
    return 0;
}

/* Register-state mirror for the shared-window path: copy everything except
 * the window base, and keep ibtc/ras zeroed so single-step cannot chain. */
static void difftest_mirror_regs(const a32_ctx_t *ref, a32_ctx_t *dut)
{
    uint64_t dut_base = dut->guest_base;
    memcpy(dut, ref, sizeof(*dut));
    dut->guest_base = dut_base;
    memset(dut->ibtc, 0, sizeof(dut->ibtc));
    memset(dut->ras, 0, sizeof(dut->ras));
    dut->ras_top = 0U;
}

int a32_difftest_run_compare(a32_ctx_t *ctx, uint64_t max_steps,
    a32_difftest_stats_t *stats)
{
    a32_difftest_stats_t s;
    memset(&s, 0, sizeof(s));
    if (ctx == NULL) {
        return 127;
    }
    if (max_steps == 0U) {
        max_steps = 1000000ULL;
    }
    if (g_dbt_step == NULL) {
        /* Coverage mode: single interp copy, classify every step exit. */
        static a32_ctx_t ref;
        memcpy(&ref, ctx, sizeof(ref));
        for (uint64_t i = 0ULL; i < max_steps; i++) {
            uint32_t pc = ref.regs[15];
            uint32_t rc = a32_interp_step(&ref);
            s.steps++;
            if (rc == A32_EXIT_NONE) {
                continue;
            }
            if (rc == A32_EXIT_BRANCH || rc == A32_EXIT_INTERWORK) {
                continue;
            }
            if (rc == A32_EXIT_SVC) {
                s.svc_count++;
                ref.syscalls_executed++;
                if (a32_handle_syscall(&ref) != 0) {
                    break;
                }
                continue;
            }
            if (rc == A32_EXIT_UNDEF) {
                s.undef_fallbacks++;
                if (s.first_undef_pc == 0U) {
                    s.first_undef_pc = pc;
                }
                break;
            }
            if (rc == A32_EXIT_FAULT) {
                s.fault_count++;
                break;
            }
            break;
        }
        s.interp_hash = a32_interp_state_hash(&ref);
        s.dbt_hash = s.interp_hash;
        memcpy(ctx, &ref, sizeof(*ctx));
        if (stats != NULL) {
            *stats = s;
        }
        if (s.undef_fallbacks > 0U || s.fault_count > 0U) {
            fprintf(stderr,
                "[difftest] coverage: steps=%llu svc=%llu undef=%llu fault=%llu first_undef=0x%08x hash=0x%llx\n",
                (unsigned long long)s.steps, (unsigned long long)s.svc_count,
                (unsigned long long)s.undef_fallbacks,
                (unsigned long long)s.fault_count, s.first_undef_pc,
                (unsigned long long)s.interp_hash);
        }
        return (s.undef_fallbacks > 0U || s.fault_count > 0U) ? 1 : 0;
    }
    static a32_ctx_t ref;
    static a32_ctx_t dut;
    bool separate = false;
    uint64_t compared = 0U;
    int final_rc = 0;
    void *win_out = NULL;
    memcpy(&ref, ctx, sizeof(ref));
    memcpy(&dut, ctx, sizeof(dut));
    /* A1 runs dut in a private window; A2 (and reserve failure) shares the
     * window with single-execution syscall mirroring (see header notes). */
    separate = (a32_vm32_current_regime() != A32_VM32_REGIME_A2_SOFT_TLB);
    if (separate) {
        if (a32_backend_map_anon(NULL, (size_t)DIFFTEST_WINDOW_SIZE, 0,
                0, 0, &win_out) != 0 || win_out == NULL) {
            separate = false;
        } else {
            dut.guest_base = (uint64_t)(uintptr_t)win_out;
        }
    }
    if (separate) {
        if (difftest_sync_dut(&ref, &dut) != 0) {
            fprintf(stderr, "[difftest] initial window mirror failed\n");
            (void)a32_backend_unmap(win_out, (size_t)DIFFTEST_WINDOW_SIZE);
            return difftest_done(ctx, &ref, &dut, &s, stats, 1);
        }
    } else {
        difftest_mirror_regs(&ref, &dut);
    }
    for (uint64_t i = 0ULL; i < max_steps; i++) {
        uint32_t pc = ref.regs[15];
        uint32_t r_rc = a32_interp_step(&ref);
        uint32_t d_rc = g_dbt_step(&dut);
        bool dut_fallback = false;
        bool r_cont = (r_rc == A32_EXIT_NONE || r_rc == A32_EXIT_BRANCH ||
            r_rc == A32_EXIT_INTERWORK);
        bool d_cont = (d_rc == A32_EXIT_NONE || d_rc == A32_EXIT_BRANCH ||
            d_rc == A32_EXIT_INTERWORK);
        s.steps++;
        /* NONE/BRANCH/INTERWORK all mean "advanced one instruction, keep
         * going" (interp returns NONE even for taken branches while DBT
         * exits BRANCH), so they form one equivalence class. */
        if (r_cont != d_cont || (!r_cont && r_rc != d_rc)) {
            /* Production parity: a DBT UNDEF falls back to the reference
             * interpreter (a32_jit_run does the same). Reported, but not
             * counted as a compared instruction. */
            if (d_rc == A32_EXIT_UNDEF && r_rc != A32_EXIT_UNDEF) {
                s.undef_fallbacks++;
                if (s.first_undef_pc == 0U) {
                    s.first_undef_pc = pc;
                }
                d_rc = a32_interp_step(&dut);
                dut_fallback = true;
            }
            if (!dut_fallback || d_rc != r_rc ||
                !a32_interp_state_equal(&ref, &dut)) {
                s.diverged = true;
                s.first_diverge_pc = pc;
                fprintf(stderr,
                    "[difftest] DIVERGENCE at pc=0x%08x step=%llu rc_interp=%u rc_dbt=%u\n",
                    pc, (unsigned long long)s.steps, r_rc, d_rc);
                final_rc = 1;
                goto difftest_out;
            }
        }
        if (r_rc == A32_EXIT_SVC) {
            int ex;
            s.svc_count++;
            if (separate &&
                difftest_mem_hash(&ref) != difftest_mem_hash(&dut)) {
                s.diverged = true;
                s.first_diverge_pc = pc;
                fprintf(stderr,
                    "[difftest] DIVERGENCE in memory before syscall at pc=0x%08x step=%llu\n",
                    pc, (unsigned long long)s.steps);
                final_rc = 1;
                goto difftest_out;
            }
            /* Exactly-once syscall on ref, then mirror the result. */
            ref.syscalls_executed++;
            ex = a32_handle_syscall(&ref);
            if (separate) {
                if (difftest_sync_dut(&ref, &dut) != 0) {
                    fprintf(stderr,
                        "[difftest] window mirror failed at pc=0x%08x\n", pc);
                    final_rc = 1;
                    goto difftest_out;
                }
            } else {
                difftest_mirror_regs(&ref, &dut);
            }
            if (ex != 0) {
                if (!a32_interp_state_equal(&ref, &dut) ||
                    (separate &&
                     difftest_mem_hash(&ref) != difftest_mem_hash(&dut))) {
                    s.diverged = true;
                    s.first_diverge_pc = pc;
                    fprintf(stderr,
                        "[difftest] DIVERGENCE on guest exit at pc=0x%08x step=%llu\n",
                        pc, (unsigned long long)s.steps);
                    final_rc = 1;
                } else {
                    final_rc = (int)ref.regs[0];
                }
                goto difftest_out;
            }
            compared++;
            continue;
        }
        if (!a32_interp_state_equal(&ref, &dut)) {
            s.diverged = true;
            s.first_diverge_pc = pc;
            fprintf(stderr,
                "[difftest] DIVERGENCE at pc=0x%08x step=%llu rc_interp=%u rc_dbt=%u\n",
                pc, (unsigned long long)s.steps, r_rc, d_rc);
            final_rc = 1;
            goto difftest_out;
        }
        if (!dut_fallback) {
            compared++;
        }
        if (r_rc == A32_EXIT_NONE || r_rc == A32_EXIT_BRANCH ||
            r_rc == A32_EXIT_INTERWORK) {
            continue;
        }
        if (r_rc == A32_EXIT_UNDEF) {
            s.undef_fallbacks++;
            if (s.first_undef_pc == 0U) {
                s.first_undef_pc = pc;
            }
        }
        if (r_rc == A32_EXIT_FAULT) {
            s.fault_count++;
        }
        break;
    }
    if (separate && difftest_mem_hash(&ref) != difftest_mem_hash(&dut)) {
        s.diverged = true;
        fprintf(stderr, "[difftest] DIVERGENCE in final memory state\n");
        final_rc = 1;
        goto difftest_out;
    }
    if (compared == 0U || s.diverged) {
        final_rc = 1;
        goto difftest_out;
    }
    final_rc = 0;
difftest_out:
    if (separate && win_out != NULL) {
        (void)a32_backend_unmap(win_out, (size_t)DIFFTEST_WINDOW_SIZE);
    }
    if (final_rc == 0 && !a32_interp_state_equal(&ref, &dut)) {
        s.diverged = true;
        final_rc = 1;
    }
    return difftest_done(ctx, &ref, &dut, &s, stats, final_rc);
}
