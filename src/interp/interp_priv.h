/*
 * src/interp/interp_priv.h — Shared helpers between interpreter translation units
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal to src/interp/. Defined in interp.c, used by interp_a32.c and
 * interp_thumb.c. Not part of the public difftest API (see interp.h).
 */

#ifndef A32_INTERP_PRIV_H
#define A32_INTERP_PRIV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "a32rt.h"

uint32_t i_ror32(uint32_t v, unsigned r);
void i_set_nz(uint32_t *cpsr, uint32_t result);
void i_set_nzcv(uint32_t *cpsr, uint32_t result, bool carry, bool overflow);
uint32_t i_add_carry(uint32_t a, uint32_t b, uint32_t cin,
    bool *carry_out, bool *ov_out);
bool i_load_bytes(a32_ctx_t *ctx, uint32_t va, void *out, uint32_t len);
bool i_store_bytes(a32_ctx_t *ctx, uint32_t va, const void *in, uint32_t len);
bool i_fetch32(a32_ctx_t *ctx, uint32_t va, uint32_t *out);
bool i_fetch16(a32_ctx_t *ctx, uint32_t va, uint16_t *out);
bool i_load_u32(a32_ctx_t *ctx, uint32_t va, uint32_t *out);
bool i_store_u32(a32_ctx_t *ctx, uint32_t va, uint32_t v);
bool i_mem_ok(a32_ctx_t *ctx, uint32_t va, uint32_t len, uint32_t prot);
void i_undef(a32_ctx_t *ctx);
void i_set_pc_branch(a32_ctx_t *ctx, uint32_t target, bool interwork);
uint32_t i_a32_step(a32_ctx_t *ctx, uint32_t pc);
uint32_t i_thumb_step(a32_ctx_t *ctx, uint32_t pc);

#endif /* A32_INTERP_PRIV_H */
