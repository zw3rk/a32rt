/*
 * src/dbt/difftest.c — --interp / --dbt / --difftest execution-mode harness
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wiring note (for the coordinator merging U3-A32-1): this file adds only
 * new symbols. Enabling the flags in src/main.c is a 3-line patch:
 * parse with a32_difftest_parse_arg, then dispatch to a32_difftest_run_*
 * instead of a32_jit_run. No existing file is modified by this lane.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "difftest.h"

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
 * interpreter on the ref copy and the DBT hook on the dut copy, then
 * compares guest-visible state. The first divergence (PC included) is
 * reported; the function returns 0 when no divergence was observed, 1 on
 * divergence, or the guest exit code when both sides exit cleanly.
 *
 * Without an installed DBT hook this degrades to interp coverage mode:
 * it runs the reference stepper and counts EXIT_UNDEF fallbacks, which
 * is exactly the metric U3 needs before JIT lockstep lands.
 */
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
        memcpy(ctx, &ref, sizeof(*ctx));
        if (stats != NULL) {
            *stats = s;
        }
        if (s.undef_fallbacks > 0U) {
            fprintf(stderr,
                "[difftest] coverage: steps=%llu svc=%llu undef=%llu first_undef=0x%08x hash=0x%llx\n",
                (unsigned long long)s.steps, (unsigned long long)s.svc_count,
                (unsigned long long)s.undef_fallbacks, s.first_undef_pc,
                (unsigned long long)s.interp_hash);
        }
        return 0;
    }
    static a32_ctx_t ref;
    static a32_ctx_t dut;
    memcpy(&ref, ctx, sizeof(ref));
    memcpy(&dut, ctx, sizeof(dut));
    for (uint64_t i = 0ULL; i < max_steps; i++) {
        uint32_t pc = ref.regs[15];
        uint32_t r_rc = a32_interp_step(&ref);
        uint32_t d_rc = g_dbt_step(&dut);
        s.steps++;
        if (r_rc == A32_EXIT_SVC) {
            s.svc_count++;
        }
        if (r_rc == A32_EXIT_UNDEF) {
            s.undef_fallbacks++;
            if (s.first_undef_pc == 0U) {
                s.first_undef_pc = pc;
            }
        }
        if (!a32_interp_state_equal(&ref, &dut)) {
            s.diverged = true;
            s.first_diverge_pc = pc;
            s.interp_hash = a32_interp_state_hash(&ref);
            s.dbt_hash = a32_interp_state_hash(&dut);
            fprintf(stderr,
                "[difftest] DIVERGENCE at pc=0x%08x step=%llu rc_interp=%u rc_dbt=%u\n",
                pc, (unsigned long long)s.steps, r_rc, d_rc);
            break;
        }
        if (r_rc != A32_EXIT_NONE && r_rc != A32_EXIT_BRANCH &&
            r_rc != A32_EXIT_INTERWORK) {
            break;
        }
        if (d_rc != A32_EXIT_NONE && d_rc != A32_EXIT_BRANCH &&
            d_rc != A32_EXIT_INTERWORK) {
            break;
        }
    }
    memcpy(ctx, &ref, sizeof(*ctx));
    if (stats != NULL) {
        *stats = s;
    }
    return s.diverged ? 1 : 0;
}
