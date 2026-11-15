/*
 * src/ios/ios_stubs_time.c — Time & PID leaf stubs (A32-R20-P0)
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "a32rt.h"
#include "ios/ios.h"

void a32_ios_refresh_runtime_pid(a32_ctx_t *ctx)
{
    (void)ctx;
}

int a32_ios_emit_time_pid_leaf_stubs(a32_ctx_t *ctx, uint32_t *io_cur_va)
{
    (void)ctx;
    (void)io_cur_va;
    return 0;
}

uint32_t a32_ios_lookup_time_pid_leaf(const char *sym_name, bool *out_found)
{
    if (out_found != NULL) {
        *out_found = false;
    }
    (void)sym_name;
    return 0U;
}
