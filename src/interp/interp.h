/*
 * src/interp/interp.h — Pure C11 A32/Thumb reference interpreter public API
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Single-step reference model for the JIT (src/jit.c) and the difftest
 * harness (src/dbt/difftest.c). No assembly, no x18, no host syscalls
 * here: memory is accessed through the [G, G+4GiB) window only.
 */

#ifndef A32_INTERP_H
#define A32_INTERP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "a32rt.h"

/* Step exactly one guest instruction at ctx->regs[15].
 *
 * On success regs[15] advances to the next instruction and the function
 * returns A32_EXIT_NONE. Control-flow, SVC, BKPT, faults and unhandled
 * encodings return the matching A32_EXIT_* code and leave a full record
 * in ctx (exit_reason, svc_imm / fault_addr). The caller (interp_run or
 * difftest) owns syscall/signal policy; step itself never calls out.
 */
uint32_t a32_interp_step(a32_ctx_t *ctx);

/* Run until a non-NONE exit, a pending-signal stop, or max_steps.
 * SVC exits are dispatched through a32_handle_syscall like a32_jit_run;
 * UNDEF exits attempt a32_signals_deliver_sync(SIGILL) first. Returns the
 * process exit code (guest r0 on ARM_NR_exit, 0 on exit_group) or a
 * 128+signal abort code, mirroring a32_jit_run.
 */
int a32_interp_run(a32_ctx_t *ctx, uint64_t max_steps);

/* ARM condition-code predicate against the current CPSR NZCV bits. */
bool a32_interp_cond_passes(uint32_t cpsr, unsigned cond);

/* Coprocessor 10/11 (VFPv4-D32 / NEON) single-step. Returns true when the
 * 32-bit A32 coprocessor word was claimed (regs[15] already advanced);
 * false means "not a VFP/NEON encoding" and the caller must fall back to
 * A32_EXIT_UNDEF. Implemented in interp_vfp.c.
 */
bool a32_interp_vfp_step(a32_ctx_t *ctx, uint32_t insn);

/* Deterministic guest-state digest over r0-r15, cpsr, fpscr and d0-d31.
 * Used by difftest to compare interp against DBT after each step.
 */
uint64_t a32_interp_state_hash(const a32_ctx_t *ctx);

/* True when two contexts agree on all guest-visible state. */
bool a32_interp_state_equal(const a32_ctx_t *a, const a32_ctx_t *b);

#endif /* A32_INTERP_H */
