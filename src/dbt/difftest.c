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
 * compares guest-visible state (GPRs, CPSR, FPSCR, VFP). The first
 * divergence (PC included) is reported. Returns the guest exit code when
 * both sides exit cleanly, 1 on any register/PC divergence or any
 * unexpected UNDEF/fault on either side.
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
    memcpy(&ref, ctx, sizeof(ref));
    memcpy(&dut, ctx, sizeof(dut));
    for (uint64_t i = 0ULL; i < max_steps; i++) {
        uint32_t pc = ref.regs[15];
        uint32_t r_rc = a32_interp_step(&ref);
        uint32_t d_rc = g_dbt_step(&dut);
        s.steps++;
        if (r_rc == A32_EXIT_SVC || d_rc == A32_EXIT_SVC) {
            int r_ex = 0;
            int d_ex = 0;
            bool r_out = false;
            bool d_out = false;
            if (r_rc == A32_EXIT_SVC) {
                s.svc_count++;
                ref.syscalls_executed++;
                if (a32_handle_syscall(&ref) != 0) {
                    r_out = true;
                    r_ex = (int)ref.regs[0];
                }
            }
            if (d_rc == A32_EXIT_SVC) {
                dut.syscalls_executed++;
                if (a32_handle_syscall(&dut) != 0) {
                    d_out = true;
                    d_ex = (int)dut.regs[0];
                }
            }
            if (r_out || d_out) {
                if (!r_out || !d_out || r_ex != d_ex ||
                    !a32_interp_state_equal(&ref, &dut)) {
                    s.diverged = true;
                    s.first_diverge_pc = pc;
                    fprintf(stderr,
                        "[difftest] DIVERGENCE on guest exit at pc=0x%08x step=%llu\n",
                        pc, (unsigned long long)s.steps);
                    return difftest_done(ctx, &ref, &dut, &s, stats, 1);
                }
                return difftest_done(ctx, &ref, &dut, &s, stats, r_ex);
            }
            if (!a32_interp_state_equal(&ref, &dut)) {
                s.diverged = true;
                s.first_diverge_pc = pc;
                fprintf(stderr,
                    "[difftest] DIVERGENCE after syscall at pc=0x%08x step=%llu rc_interp=%u rc_dbt=%u\n",
                    pc, (unsigned long long)s.steps, r_rc, d_rc);
                return difftest_done(ctx, &ref, &dut, &s, stats, 1);
            }
            continue;
        }
        if (!a32_interp_state_equal(&ref, &dut)) {
            s.diverged = true;
            s.first_diverge_pc = pc;
            fprintf(stderr,
                "[difftest] DIVERGENCE at pc=0x%08x step=%llu rc_interp=%u rc_dbt=%u\n",
                pc, (unsigned long long)s.steps, r_rc, d_rc);
            return difftest_done(ctx, &ref, &dut, &s, stats, 1);
        }
        if (r_rc == A32_EXIT_UNDEF || d_rc == A32_EXIT_UNDEF ||
            r_rc == A32_EXIT_FAULT || d_rc == A32_EXIT_FAULT) {
            if (r_rc == A32_EXIT_UNDEF || d_rc == A32_EXIT_UNDEF) {
                s.undef_fallbacks++;
                if (s.first_undef_pc == 0U) {
                    s.first_undef_pc = pc;
                }
            }
            if (r_rc == A32_EXIT_FAULT || d_rc == A32_EXIT_FAULT) {
                s.fault_count++;
            }
            fprintf(stderr,
                "[difftest] unexpected %s at pc=0x%08x step=%llu rc_interp=%u rc_dbt=%u\n",
                (r_rc == A32_EXIT_FAULT || d_rc == A32_EXIT_FAULT) ?
                    "fault" : "UNDEF",
                pc, (unsigned long long)s.steps, r_rc, d_rc);
            return difftest_done(ctx, &ref, &dut, &s, stats, 1);
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
    return difftest_done(ctx, &ref, &dut, &s, stats, s.diverged ? 1 : 0);
}
