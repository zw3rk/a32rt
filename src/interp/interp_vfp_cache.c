/*
 * src/interp/interp_vfp_cache.c — VFP register-cache helpers (A32-R20-P0)
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "a64_emit.h"

unsigned a64_vfp_src_s(a64_buf_t *b, unsigned scratch_vt, unsigned s_idx)
{
    a64_emit_ldr_s_ctx(b, scratch_vt, s_idx);
    return scratch_vt;
}

unsigned a64_vfp_dst_s(a64_buf_t *b, unsigned scratch_vt, unsigned s_idx)
{
    (void)b;
    (void)s_idx;
    return scratch_vt;
}

void a64_vfp_commit_s(a64_buf_t *b, unsigned host_vt, unsigned s_idx)
{
    a64_emit_str_s_ctx(b, host_vt, s_idx);
}

unsigned a64_vfp_src_d(a64_buf_t *b, unsigned scratch_vt, unsigned d_idx)
{
    a64_emit_ldr_d_ctx(b, scratch_vt, d_idx);
    return scratch_vt;
}

unsigned a64_vfp_dst_d(a64_buf_t *b, unsigned scratch_vt, unsigned d_idx)
{
    (void)b;
    (void)d_idx;
    return scratch_vt;
}

void a64_vfp_commit_d(a64_buf_t *b, unsigned host_vt, unsigned d_idx)
{
    a64_emit_str_d_ctx(b, host_vt, d_idx);
}

unsigned a64_vfp_src_q(a64_buf_t *b, unsigned scratch_vt, unsigned q_idx)
{
    a64_emit_ldr_q_ctx(b, scratch_vt, q_idx);
    return scratch_vt;
}

unsigned a64_vfp_dst_q(a64_buf_t *b, unsigned scratch_vt, unsigned q_idx)
{
    (void)b;
    (void)q_idx;
    return scratch_vt;
}

void a64_vfp_commit_q(a64_buf_t *b, unsigned host_vt, unsigned q_idx)
{
    a64_emit_str_q_ctx(b, host_vt, q_idx);
}
