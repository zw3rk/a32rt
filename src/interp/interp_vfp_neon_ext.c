/*
 * src/interp/interp_vfp_neon_ext.c — Extended ARMv7 NEON reference interpreter
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "interp/interp_priv.h"

bool i_vfp_neon_ext(a32_ctx_t *ctx, uint32_t pc, uint32_t insn, bool *faulted)
{
    (void)ctx;
    (void)pc;
    (void)insn;
    (void)faulted;
    return false;
}
