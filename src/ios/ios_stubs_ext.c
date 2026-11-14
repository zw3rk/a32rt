/*
 * src/ios/ios_stubs_ext.c — Native Thumb-2 leaf stubs, atexit, and compiler_rt
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ios/ios.h"

#include <stdio.h>
#include <unistd.h>

int a32_ios_emit_native_leaf_stubs(a32_ctx_t *ctx)
{
    (void)ctx;
    return 0;
}

uint32_t a32_ios_lookup_synth_ext(a32_ctx_t *ctx, const char *sym_name,
    bool *out_found)
{
    (void)ctx;
    (void)sym_name;
    if (out_found != NULL) {
        *out_found = false;
    }
    return 0U;
}

int a32_ios_handle_stub_ext(a32_ctx_t *ctx, uint32_t stub_id, bool *out_handled)
{
    (void)ctx;
    (void)stub_id;
    if (out_handled != NULL) {
        *out_handled = false;
    }
    return 0;
}

void a32_ios_exit_teardown(a32_ctx_t *ctx, int code)
{
    (void)ctx;
    fflush(NULL);
    _exit(code);
}
