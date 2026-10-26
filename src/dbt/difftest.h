/*
 * src/dbt/difftest.h -- --interp / --dbt / --difftest execution-mode API
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Canonical declarations for src/dbt/difftest.c. Included by src/main.c
 * for CLI parsing and engine dispatch.
 */

#ifndef A32_DIFFTEST_H
#define A32_DIFFTEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../interp/interp.h"

typedef enum a32_exec_mode {
    A32_EXEC_DBT = 0,
    A32_EXEC_INTERP = 1,
    A32_EXEC_DIFFTEST = 2
} a32_exec_mode_t;

typedef struct a32_difftest_stats {
    uint64_t steps;
    uint64_t svc_count;
    uint64_t undef_fallbacks;
    uint64_t fault_count;
    uint32_t first_undef_pc;
    uint32_t first_diverge_pc;
    uint64_t interp_hash;
    uint64_t dbt_hash;
    bool diverged;
} a32_difftest_stats_t;

/* Single DBT step hook, installed by src/main.c at startup.
 * Defaults to NULL (coverage mode: interp only). */
typedef uint32_t (*a32_dbt_step_fn)(a32_ctx_t *ctx);

void a32_difftest_set_dbt_step(a32_dbt_step_fn fn);

/* Execute exactly one DBT block at ctx->regs[15] (src/dbt/dbt_jit_run.c).
 * Returns the block exit reason; never dispatches syscalls or signals. */
uint32_t a32_dbt_single_step(a32_ctx_t *ctx);
bool a32_difftest_parse_arg(const char *arg, a32_exec_mode_t *out);
const char *a32_difftest_mode_name(a32_exec_mode_t mode);
int a32_difftest_run_interp(a32_ctx_t *ctx, uint64_t max_steps,
    a32_difftest_stats_t *stats);
int a32_difftest_run_dbt(a32_ctx_t *ctx, a32_difftest_stats_t *stats);
int a32_difftest_run_compare(a32_ctx_t *ctx, uint64_t max_steps,
    a32_difftest_stats_t *stats);

#endif /* A32_DIFFTEST_H */
