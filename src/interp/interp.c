/*
 * src/interp/interp.c — Core stepper, run loop and state hashing.
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "interp.h"
#include "interp_priv.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "vm/vm32.h"

bool a32_interp_cond_passes(uint32_t cpsr, unsigned cond)
{
    bool n = (cpsr & A32_CPSR_N) != 0U;
    bool z = (cpsr & A32_CPSR_Z) != 0U;
    bool c = (cpsr & A32_CPSR_C) != 0U;
    bool v = (cpsr & A32_CPSR_V) != 0U;
    switch (cond & 0xFU) {
    case 0x0U: return z;
    case 0x1U: return !z;
    case 0x2U: return c;
    case 0x3U: return !c;
    case 0x4U: return n;
    case 0x5U: return !n;
    case 0x6U: return v;
    case 0x7U: return !v;
    case 0x8U: return c && !z;
    case 0x9U: return !c || z;
    case 0xAU: return n == v;
    case 0xBU: return n != v;
    case 0xCU: return !z && (n == v);
    case 0xDU: return z || (n != v);
    case 0xEU: return true;
    default: return true;
    }
}


uint32_t i_ror32(uint32_t v, unsigned r)
{
    r &= 31U;
    if (r == 0U) {
        return v;
    }
    return (v >> r) | (v << ((32U - r) & 31U));
}


void i_set_nz(uint32_t *cpsr, uint32_t result)
{
    if (result == 0U) {
        *cpsr |= A32_CPSR_Z;
    } else {
        *cpsr &= ~A32_CPSR_Z;
    }
    if ((result & 0x80000000U) != 0U) {
        *cpsr |= A32_CPSR_N;
    } else {
        *cpsr &= ~A32_CPSR_N;
    }
}


void i_set_nzcv(uint32_t *cpsr, uint32_t result, bool carry, bool overflow)
{
    i_set_nz(cpsr, result);
    if (carry) {
        *cpsr |= A32_CPSR_C;
    } else {
        *cpsr &= ~A32_CPSR_C;
    }
    if (overflow) {
        *cpsr |= A32_CPSR_V;
    } else {
        *cpsr &= ~A32_CPSR_V;
    }
}


uint32_t i_add_carry(uint32_t a, uint32_t b, uint32_t cin,
    bool *carry_out, bool *ov_out)
{
    uint64_t s = (uint64_t)a + (uint64_t)b + (uint64_t)(cin & 1U);
    uint32_t r = (uint32_t)s;
    bool so = ((~(a ^ b) & (a ^ r)) & 0x80000000U) != 0U;
    *carry_out = (s >> 32) != 0ULL;
    *ov_out = so;
    return r;
}

/* Guest memory helpers through the [G, G+4GiB) window. */

bool i_mem_ok(a32_ctx_t *ctx, uint32_t va, uint32_t len, uint32_t prot)
{
    if (len == 0U) {
        return true;
    }
    return a32_guest_range_ok(ctx, va, len, (int)prot);
}


bool i_load_bytes(a32_ctx_t *ctx, uint32_t va, void *out, uint32_t len)
{
    if (!i_mem_ok(ctx, va, len, (uint32_t)PROT_READ)) {
        ctx->fault_addr = va;
        ctx->exit_reason = A32_EXIT_FAULT;
        return false;
    }
    memcpy(out, a32_guest_ptr(ctx, va), len);
    return true;
}


bool i_store_bytes(a32_ctx_t *ctx, uint32_t va, const void *in, uint32_t len)
{
    if (!i_mem_ok(ctx, va, len, (uint32_t)PROT_WRITE)) {
        ctx->fault_addr = va;
        ctx->exit_reason = A32_EXIT_FAULT;
        return false;
    }
    memcpy(a32_guest_ptr(ctx, va), in, len);
    a32_vm32_copyout_invalidate_exec(ctx, va, (size_t)len);
    return true;
}

/* Instruction fetch: PROT_EXEC across all bytes (allows X-only pages). */
static bool i_fetch_bytes(a32_ctx_t *ctx, uint32_t va, void *out, uint32_t len)
{
    if (!i_mem_ok(ctx, va, len, (uint32_t)PROT_EXEC)) {
        ctx->fault_addr = va;
        ctx->exit_reason = A32_EXIT_FAULT;
        return false;
    }
    memcpy(out, a32_guest_ptr(ctx, va), len);
    return true;
}


bool i_fetch32(a32_ctx_t *ctx, uint32_t va, uint32_t *out)
{
    uint32_t v = 0U;
    if (!i_fetch_bytes(ctx, va & ~3U, &v, 4U)) {
        return false;
    }
    *out = v;
    return true;
}


bool i_fetch16(a32_ctx_t *ctx, uint32_t va, uint16_t *out)
{
    uint16_t v = 0U;
    if (!i_fetch_bytes(ctx, va & ~1U, &v, 2U)) {
        return false;
    }
    *out = v;
    return true;
}


bool i_load_u32(a32_ctx_t *ctx, uint32_t va, uint32_t *out)
{
    return i_load_bytes(ctx, va, out, 4U);
}


bool i_store_u32(a32_ctx_t *ctx, uint32_t va, uint32_t v)
{
    return i_store_bytes(ctx, va, &v, 4U);
}

/* Exclusive reservation helpers built only on __atomic_* builtins. */

void i_undef(a32_ctx_t *ctx)
{
    ctx->exit_reason = A32_EXIT_UNDEF;
}


uint32_t i_fault(a32_ctx_t *ctx, uint32_t va, bool is_write)
{
    (void)is_write;
    ctx->fault_addr = va;
    ctx->exit_reason = A32_EXIT_FAULT;
    return A32_EXIT_FAULT;
}


void i_set_pc_branch(a32_ctx_t *ctx, uint32_t target, bool interwork)
{
    if (interwork) {
        a32_set_pc_interwork(ctx, target);
    } else {
        bool t = (ctx->cpsr & A32_CPSR_T) != 0U;
        ctx->regs[15] = target & (t ? ~1U : ~3U);
    }
}

/* Shifted-register operand for A32 data processing. */

uint32_t a32_interp_step(a32_ctx_t *ctx)
{
    if (ctx == NULL) {
        return A32_EXIT_UNDEF;
    }
    ctx->exit_reason = A32_EXIT_NONE;
    uint32_t pc = ctx->regs[15];
    bool is_thumb = (ctx->cpsr & A32_CPSR_T) != 0U;
    uint32_t rc;
    if (is_thumb) {
        pc &= ~1U;
        rc = i_thumb_step(ctx, pc);
    } else {
        pc &= ~3U;
        rc = i_a32_step(ctx, pc);
    }
    if (rc == A32_EXIT_UNDEF || rc == A32_EXIT_FAULT) {
        ctx->regs[15] = pc;
    }
    return rc;
}


int a32_interp_run(a32_ctx_t *ctx, uint64_t max_steps)
{
    if (ctx == NULL) {
        return 127;
    }
    if (max_steps == 0U) {
        max_steps = ~0ULL;
    }
    for (uint64_t i = 0ULL; i < max_steps; i++) {
        if ((ctx->pending_signals & ~ctx->sig_mask) != 0ULL) {
            a32_signals_check_pending(ctx);
        }
        uint32_t rc = a32_interp_step(ctx);
        if (rc == A32_EXIT_NONE) {
            continue;
        }
        if (rc == A32_EXIT_BRANCH || rc == A32_EXIT_INTERWORK) {
            continue;
        }
        if (rc == A32_EXIT_SVC) {
            ctx->syscalls_executed++;
            if (a32_handle_syscall(ctx) != 0) {
                return (int)ctx->regs[0];
            }
            continue;
        }
        if (rc == A32_EXIT_BKPT) {
            if (ctx->gdb_enabled) {
                if (a32_gdb_handle_stop(ctx, SIGTRAP) != 0) {
                    return 128 + SIGTRAP;
                }
                continue;
            }
            if (a32_signals_deliver_sync(ctx, SIGTRAP, ctx->regs[15])) {
                continue;
            }
            return 128 + SIGTRAP;
        }
        if (rc == A32_EXIT_FAULT) {
            if (ctx->gdb_enabled) {
                if (a32_gdb_handle_stop(ctx, SIGSEGV) != 0) {
                    return 128 + SIGSEGV;
                }
                continue;
            }
            if (a32_signals_deliver_sync(ctx, SIGSEGV, ctx->fault_addr)) {
                a32_signals_check_pending(ctx);
                continue;
            }
            return 128 + SIGSEGV;
        }
        /* UNDEF single-step fallback: deliver SIGILL, else abort like JIT. */
        if (ctx->gdb_enabled) {
            uint32_t stop = ctx->regs[15];
            (void)stop;
            if (a32_gdb_handle_stop(ctx, SIGILL) != 0) {
                return 128 + SIGILL;
            }
            continue;
        }
        if (a32_signals_deliver_sync(ctx, SIGILL, ctx->regs[15])) {
            continue;
        }
        if (ctx->trace_enabled) {
            fprintf(stderr, "a32rt-interp: undef at pc=0x%08x\n", ctx->regs[15]);
        }
        return 128 + SIGILL;
    }
    return 124;
}


static uint64_t mix64(uint64_t h, uint64_t v)
{
    h ^= v + 0x9E3779B97F4A7C15ULL + (h << 12) + (h >> 4);
    h *= 0xBF58476D1CE4E5B9ULL;
    h ^= h >> 29;
    return h;
}


uint64_t a32_interp_state_hash(const a32_ctx_t *ctx)
{
    uint64_t h = 0xCBF29CE484222325ULL;
    for (unsigned i = 0U; i < 16U; i++) {
        h = mix64(h, ctx->regs[i]);
    }
    h = mix64(h, ctx->cpsr);
    h = mix64(h, ctx->fpscr);
    for (unsigned i = 0U; i < 32U; i++) {
        h = mix64(h, ctx->vfp_d[i]);
    }
    return h;
}


bool a32_interp_state_equal(const a32_ctx_t *a, const a32_ctx_t *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    for (unsigned i = 0U; i < 16U; i++) {
        if (a->regs[i] != b->regs[i]) {
            return false;
        }
    }
    if (a->cpsr != b->cpsr || a->fpscr != b->fpscr) {
        return false;
    }
    for (unsigned i = 0U; i < 32U; i++) {
        if (a->vfp_d[i] != b->vfp_d[i]) {
            return false;
        }
    }
    return true;
}
