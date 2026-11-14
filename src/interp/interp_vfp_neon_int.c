/*
 * src/interp/interp_vfp_neon_int.c — NEON integer shifts, long/wide/narrow, VEXT & VDUP(core)
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "interp.h"
#include "interp_priv.h"
#include "interp/decode_priv.h"

bool i_vfp_neon_int(a32_ctx_t *ctx, uint32_t pc, uint32_t insn)
{
    (void)ctx;
    (void)pc;
    (void)insn;
    return false;
}
