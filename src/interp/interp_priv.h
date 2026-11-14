/*
 * src/interp/interp_priv.h — Shared helpers between interpreter translation units
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal to src/interp/. The i_* helpers are defined in interp.c and
 * used by interp_a32.c and interp_thumb.c; i_a32_mem_step lives in
 * interp_a32_mem.c and i_vfp_neon lives in interp_vfp_neon.c, both
 * called from interp_a32.c / interp_vfp.c. Not part of the public
 * difftest API (see interp.h).
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
/* Record a guest memory fault at va and return A32_EXIT_FAULT. is_write
 * marks store faults for future fault-status reporting (A32-R7-6). */
uint32_t i_fault(a32_ctx_t *ctx, uint32_t va, bool is_write);
void i_set_pc_branch(a32_ctx_t *ctx, uint32_t target, bool interwork);
uint32_t i_a32_step(a32_ctx_t *ctx, uint32_t pc);
uint32_t i_thumb_step(a32_ctx_t *ctx, uint32_t pc);
uint32_t i_t32_step_ext(a32_ctx_t *ctx, uint32_t pc, uint16_t hw1, uint16_t hw2);
uint32_t i_t32_mem_step(a32_ctx_t *ctx, uint32_t pc, uint16_t hw1, uint16_t hw2);
/* Extra load/store + LDM/STM step (interp_a32_mem.c). Sets *claimed when
 * insn is a memory-block encoding and returns its exit code. */
uint32_t i_a32_mem_step(a32_ctx_t *ctx, uint32_t pc, uint32_t insn,
    bool *claimed);
/* NEON memory/VMOV-imm/VREV/VALU subset (interp_vfp_neon.c & interp_vfp_neon_ext.c). */
bool i_vfp_neon(a32_ctx_t *ctx, uint32_t pc, uint32_t insn, bool *faulted);
bool i_vfp_neon_ext(a32_ctx_t *ctx, uint32_t pc, uint32_t insn, bool *faulted);
bool i_vfp_neon_int(a32_ctx_t *ctx, uint32_t pc, uint32_t insn);
bool i_vfp_neon_shift(a32_ctx_t *ctx, uint32_t pc, uint32_t insn);
bool i_vfp_neon_long(a32_ctx_t *ctx, uint32_t pc, uint32_t insn);
bool i_vfp_fxcvt(a32_ctx_t *ctx, uint32_t pc, uint32_t insn);

/* Narrow guest memory accessors shared by the VFP/NEON step files. */
static inline bool mem_r(a32_ctx_t *ctx, uint32_t va, void *out, uint32_t len)
{
    return i_load_bytes(ctx, va, out, len);
}

static inline bool mem_w(a32_ctx_t *ctx, uint32_t va, const void *in, uint32_t len)
{
    return i_store_bytes(ctx, va, in, len);
}

#endif /* A32_INTERP_PRIV_H */
