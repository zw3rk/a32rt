/*
 * src/ios/ios_stubs_ext.c — Native ARM32 leaf stubs, atexit, and compiler_rt
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Extended synthetic libSystem handlers behind `svc #0x81`:
 * C `exit`/`_exit` with LIFO atexit callbacks, `strtol`/`strtoul`,
 * `compiler_rt` integer division helpers (stub ids 120..129), and
 * native ARM32 leaf implementations (`_strlen`, `_strcmp`, `_strncmp`,
 * `_memcmp`, `_memset`, `_bzero`, `_memcpy`, `_memmove`, `_pthread_self`)
 * emitted into `[A32_IOS_LEAF_STUB_VA, +A32_IOS_LEAF_STUB_SIZE)`.
 * The DBT translates that ARM32 code inline, so leaf calls run with
 * zero `svc #0x81` host exits. Curated C11, no VLAs.
 */

#if !defined(__APPLE__)
#define _GNU_SOURCE
#else
#define _DARWIN_C_SOURCE
#endif

#include "a32rt.h"
#include "ios/ios.h"
#include "os/sys_priv.h"
#include "vm/vm32.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* ---- Process exit: atexit table + teardown ---- */

#define AX_N 64U
#define AX_RESUME_ID 0xFBU
typedef struct { uint32_t fn; uint32_t arg; bool is_cxa; } ax_ent_t;
static ax_ent_t g_ax_ents[AX_N];
static uint32_t g_ax_count = 0U;
static pthread_mutex_t g_ax_mu = PTHREAD_MUTEX_INITIALIZER;
static bool g_ax_exiting = false;
static uint32_t g_ax_saved[16];
static uint32_t g_ax_cpsr = 0U;
static uint32_t g_ax_status = 0U;
static uint32_t g_ax_next = 0U;

void a32_ios_exit_teardown(a32_ctx_t *ctx, int code)
{
    if (ctx != NULL && ctx->auto_aot && !ctx->aot_loaded &&
        ctx->blocks_compiled > 0U && ctx->exec_path != NULL &&
        ctx->exec_path[0] != '\0') {
        (void)a32_aot_save_cache(ctx, ctx->exec_path);
    }
    if (ctx != NULL && ctx->stats_enabled) {
        fprintf(stderr,
            "[a32rt stats] blocks_compiled=%llu blocks_executed=%llu chain_hits=%llu "
            "ibtc_hits=%llu ras_hits=%llu flags_elided=%llu syscalls=%llu "
            "icache_flushes=%llu wx_transitions=%llu window=0x%llx\n",
            (unsigned long long)ctx->blocks_compiled,
            (unsigned long long)ctx->blocks_executed,
            (unsigned long long)ctx->chain_hits,
            (unsigned long long)ctx->ibtc_hits,
            (unsigned long long)ctx->ras_hits,
            (unsigned long long)ctx->flags_elided,
            (unsigned long long)ctx->syscalls_executed,
            (unsigned long long)ctx->icache_flushes,
            (unsigned long long)ctx->wx_transitions,
            (unsigned long long)ctx->guest_base);
    }
    fflush(NULL);
    _exit(code & 0xFF);
}

static void ax_call_next(a32_ctx_t *ctx)
{
    ax_ent_t e;
    if (g_ax_next == 0U) {
        a32_ios_exit_teardown(ctx, (int)g_ax_status);
        return;
    }
    g_ax_next--;
    e = g_ax_ents[g_ax_next];
    memcpy(ctx->regs, g_ax_saved, sizeof(g_ax_saved));
    ctx->cpsr = g_ax_cpsr;
    ctx->regs[0] = e.arg;
    ctx->regs[14] = (A32_IOS_STUB_BASE_VA + AX_RESUME_ID * 8U) | 1U;
    a32_set_pc_interwork(ctx, e.fn);
}

static void ax_record(a32_ctx_t *ctx, uint32_t fn, uint32_t arg, bool is_cxa)
{
    (void)pthread_mutex_lock(&g_ax_mu);
    /* Entries added while exiting land past the snapshot and never run,
     * so a handler that calls atexit cannot loop forever. */
    if (g_ax_count < AX_N) {
        g_ax_ents[g_ax_count].fn = fn;
        g_ax_ents[g_ax_count].arg = arg;
        g_ax_ents[g_ax_count].is_cxa = is_cxa;
        g_ax_count++;
        ctx->regs[0] = 0U;
    } else {
        ctx->regs[0] = 0xFFFFFFFFU;
    }
    (void)pthread_mutex_unlock(&g_ax_mu);
}

static void ax_exit(a32_ctx_t *ctx, uint32_t status)
{
    (void)pthread_mutex_lock(&g_ax_mu);
    if (g_ax_exiting) {
        (void)pthread_mutex_unlock(&g_ax_mu);
        a32_ios_exit_teardown(ctx, (int)status);
        return;
    }
    g_ax_exiting = true;
    g_ax_status = status;
    g_ax_next = g_ax_count;
    memcpy(g_ax_saved, ctx->regs, sizeof(g_ax_saved));
    g_ax_cpsr = ctx->cpsr;
    (void)pthread_mutex_unlock(&g_ax_mu);
    ax_call_next(ctx);
}

/* ---- strtol / strtoul (stub ids 36/37) ---- */

static const char *ex_gstr(a32_ctx_t *ctx, uint32_t va)
{
    uint32_t p = va;
    int i;
    if (va == 0U) {
        return NULL;
    }
    for (i = 0; i < 16; i++) {
        uint32_t rem = 0x1000U - (p & 0xFFFU);
        const char *s;
        if (!a32_guest_range_ok(ctx, p, 1U, (uint32_t)PROT_READ)) {
            return NULL;
        }
        s = (const char *)a32_guest_ptr(ctx, p);
        if (memchr(s, '\0', (size_t)rem) != NULL) {
            return (const char *)a32_guest_ptr(ctx, va);
        }
        p += rem;
        if (p < va) {
            return NULL;
        }
    }
    return NULL;
}

static void ex_strto(a32_ctx_t *ctx, uint32_t id)
{
    const char *x = ex_gstr(ctx, ctx->regs[0]);
    uint32_t r1 = ctx->regs[1], r2 = ctx->regs[2];
    char *endp = NULL;
    int saved = errno;
    uint32_t out = 0U;
    bool ov = false;
    if (r1 != 0U &&
        !a32_guest_range_ok(ctx, r1, 4U, (uint32_t)PROT_WRITE)) {
        int32_t de = 22;
        (void)guest_write(ctx, ctx->ios_errno_va, &de, 4U);
        ctx->regs[0] = 0xFFFFFFFFU;
        return;
    }
    if (x == NULL) {
        if (r1 != 0U) {
            uint32_t ge = ctx->regs[0];
            (void)guest_write(ctx, r1, &ge, 4U);
        }
        errno = saved;
        ctx->regs[0] = 0U;
        return;
    }
    errno = 0;
    if (id == 36U) {
        int64_t t = (int64_t)strtol(x, &endp, (int)r2);
        if (errno == ERANGE || t < -2147483648LL || t > 2147483647LL) {
            ov = true;
            out = (t < 0) ? 0x80000000U : 0x7FFFFFFFU;
        } else {
            out = (uint32_t)(int32_t)t;
        }
    } else {
        const char *p = x;
        unsigned long uv = strtoul(x, &endp, (int)r2);
        int e = errno;
        bool neg = false;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\v' ||
            *p == '\f' || *p == '\r') {
            p++;
        }
        neg = (*p == '-');
        if (e == ERANGE || (!neg && uv > 0xFFFFFFFFUL)) {
            ov = true;
            out = 0xFFFFFFFFU;
        } else {
            out = (uint32_t)uv;
        }
    }
    errno = saved;
    if (ov && ctx->ios_errno_va != 0U) {
        int32_t de = 34;
        (void)guest_write(ctx, ctx->ios_errno_va, &de, 4U);
    }
    if (r1 != 0U) {
        uint32_t ge = (endp == NULL) ? ctx->regs[0] :
            (ctx->regs[0] + (uint32_t)(endp - x));
        if (guest_write(ctx, r1, &ge, 4U)) {
            a32_vm32_copyout_invalidate_exec(ctx, r1, 4U);
        }
    }
    ctx->regs[0] = out;
}

/* ---- compiler_rt integer division helpers (stub ids 120..129) ---- */

static void ex_rt64(a32_ctx_t *ctx, uint32_t id)
{
    int64_t a = (int64_t)(((uint64_t)ctx->regs[1] << 32) | (uint64_t)ctx->regs[0]);
    int64_t b = (int64_t)(((uint64_t)ctx->regs[3] << 32) | (uint64_t)ctx->regs[2]);
    uint64_t ua = (uint64_t)a, ub = (uint64_t)b, r = 0U;
    if (id == 120U || id == 122U) {
        if (b == 0) {
            r = 0U;
        } else if (a == INT64_MIN && b == -1) {
            r = (id == 120U) ? (uint64_t)INT64_MIN : 0U;
        } else {
            int64_t q = a / b;
            r = (id == 120U) ? (uint64_t)q : (uint64_t)(a - q * b);
        }
    } else {
        uint64_t q = (ub == 0U) ? 0U : ua / ub;
        r = (id == 121U) ? q : ((ub == 0U) ? 0U : ua - q * ub);
    }
    ctx->regs[0] = (uint32_t)r;
    ctx->regs[1] = (uint32_t)(r >> 32);
}

static void ex_rt32(a32_ctx_t *ctx, uint32_t id)
{
    int32_t a = (int32_t)ctx->regs[0], b = (int32_t)ctx->regs[1];
    uint32_t ua = ctx->regs[0], ub = ctx->regs[1], r = 0U;
    if (id == 124U || id == 126U) {
        if (b == 0) {
            r = 0U;
        } else if (a == INT32_MIN && b == -1) {
            r = (id == 124U) ? (uint32_t)INT32_MIN : 0U;
        } else {
            int32_t q = a / b;
            r = (id == 124U) ? (uint32_t)q : (uint32_t)(a - q * b);
        }
    } else {
        uint32_t q = (ub == 0U) ? 0U : ua / ub;
        r = (id == 125U) ? q : ((ub == 0U) ? 0U : ua - q * ub);
    }
    ctx->regs[0] = r;
}

static void ex_rt(a32_ctx_t *ctx, uint32_t id)
{
    if (id <= 123U) {
        ex_rt64(ctx, id);
        return;
    }
    if (id <= 127U) {
        ex_rt32(ctx, id);
        return;
    }
    if (id == 128U) {
        int32_t a = (int32_t)ctx->regs[0], b = (int32_t)ctx->regs[1];
        int32_t q = 0, m = a;
        uint32_t rem_va = ctx->regs[2];
        if (b != 0 && !(a == INT32_MIN && b == -1)) {
            q = a / b;
            m = a - q * b;
        } else if (a == INT32_MIN && b == -1) {
            q = INT32_MIN;
            m = 0;
        }
        if (rem_va != 0U && guest_write(ctx, rem_va, &m, 4U)) {
            a32_vm32_copyout_invalidate_exec(ctx, rem_va, 4U);
        }
        ctx->regs[0] = (uint32_t)q;
        return;
    }
    {
        uint32_t q = (ctx->regs[1] == 0U) ? 0U : ctx->regs[0] / ctx->regs[1];
        uint32_t m = (ctx->regs[1] == 0U) ? ctx->regs[0] :
            ctx->regs[0] - q * ctx->regs[1];
        uint32_t rem_va = ctx->regs[2];
        if (rem_va != 0U && guest_write(ctx, rem_va, &m, 4U)) {
            a32_vm32_copyout_invalidate_exec(ctx, rem_va, 4U);
        }
        ctx->regs[0] = q;
    }
}

/* ---- Native ARM32 leaf stubs in the trampoline RX window ----
 *
 * Pure ARM32 (bit 0 of the bound VA stays clear) so BLX interworking
 * just works and no Thumb-2 halfword branch bookkeeping is needed.
 * Register use follows the APCS: r0..r2 args, r0 result, lr return.
 * The DBT decodes these standard encodings inline, giving zero host
 * exits per call. `_pthread_self` reads TPIDRURO slot 0, which holds
 * `ios_pthread_va` on the main thread (commpage runtime data) and on
 * every child thread (per-thread TSD page), verified before binding.
 */

typedef struct { uint32_t *w; uint32_t n; uint32_t cap; } leaf_em_t;
typedef struct { const char *name; uint32_t off; } leaf_sym_t;
#define LEAF_N 16U
static leaf_sym_t g_leaf_syms[LEAF_N];
static uint32_t g_leaf_count = 0U;
static bool g_leaf_ok = false;
static pthread_mutex_t g_leaf_mu = PTHREAD_MUTEX_INITIALIZER;

static void le_put(leaf_em_t *e, uint32_t v)
{
    if (e->n < e->cap) {
        e->w[e->n++] = v;
    }
}

static uint32_t le_mark(leaf_em_t *e) { return e->n; }

static void le_b(leaf_em_t *e, uint32_t cond, uint32_t target)
{
    int32_t off = (int32_t)target - (int32_t)(e->n + 2U);
    le_put(e, (cond << 28) | 0x0A000000U | ((uint32_t)off & 0x00FFFFFFU));
}

static uint32_t le_b_fwd(leaf_em_t *e, uint32_t cond)
{
    uint32_t p = e->n;
    le_put(e, (cond << 28) | 0x0A000000U);
    return p;
}

static void le_patch(leaf_em_t *e, uint32_t pos, uint32_t target)
{
    int32_t off = (int32_t)target - (int32_t)(pos + 2U);
    e->w[pos] |= ((uint32_t)off & 0x00FFFFFFU);
}

static void le_reg(const char *name, leaf_em_t *e)
{
    if (g_leaf_count < LEAF_N) {
        g_leaf_syms[g_leaf_count].name = name;
        g_leaf_syms[g_leaf_count].off = e->n;
        g_leaf_count++;
    }
}

static void le_alias(const char *name, const char *target)
{
    uint32_t i;
    for (i = 0U; i < g_leaf_count; i++) {
        if (strcmp(g_leaf_syms[i].name, target) == 0 &&
            g_leaf_count < LEAF_N) {
            g_leaf_syms[g_leaf_count].name = name;
            g_leaf_syms[g_leaf_count].off = g_leaf_syms[i].off;
            g_leaf_count++;
            return;
        }
    }
}

static uint32_t le_emit_all(uint32_t *w, uint32_t cap)
{
    leaf_em_t e;
    uint32_t loop, done, zero, diff, back, fwd, floop, p1, p2, p3;
    e.w = w;
    e.n = 0U;
    e.cap = cap;
    g_leaf_count = 0U;
    /* _pthread_self: mrc p15,0,r0,c13,c0,3; ldr r0,[r0]; bx lr */
    le_reg("_pthread_self", &e);
    le_put(&e, 0xEE1D0F70U);
    le_put(&e, 0xE5900000U);
    le_put(&e, 0xE12FFF1EU);
    /* ___error: errno addr = [TPIDRURO,#8]; mrc; ldr r0,[r0,#8]; bx lr */
    le_reg("___error", &e);
    le_put(&e, 0xEE1D0F70U);
    le_put(&e, 0xE5900008U);
    le_put(&e, 0xE12FFF1EU);
    /* _strlen(r0): scan for NUL, length = end - start - 1. */
    le_reg("_strlen", &e);
    le_put(&e, 0xE1A01000U);
    loop = le_mark(&e);
    le_put(&e, 0xE4D12001U);
    le_put(&e, 0xE3520000U);
    le_b(&e, 1U, loop);
    le_put(&e, 0xE0410000U);
    le_put(&e, 0xE2400001U);
    le_put(&e, 0xE12FFF1EU);
    /* _strcmp(r0, r1): byte loop, diff on mismatch or NUL. */
    le_reg("_strcmp", &e);
    loop = le_mark(&e);
    le_put(&e, 0xE4D02001U);
    le_put(&e, 0xE4D13001U);
    le_put(&e, 0xE1520003U);
    p1 = le_b_fwd(&e, 1U);
    le_put(&e, 0xE3520000U);
    le_b(&e, 1U, loop);
    done = le_mark(&e);
    le_patch(&e, p1, done);
    le_put(&e, 0xE0420003U);
    le_put(&e, 0xE12FFF1EU);
    /* _strncmp(r0, r1, r2): bounded loop, 0 when n runs out (uses r12, never callee-saved r4). */
    le_reg("_strncmp", &e);
    loop = le_mark(&e);
    le_put(&e, 0xE2522001U);
    p1 = le_b_fwd(&e, 3U);
    le_put(&e, 0xE4D03001U);
    le_put(&e, 0xE4D1C001U);
    le_put(&e, 0xE153000CU);
    p2 = le_b_fwd(&e, 1U);
    le_put(&e, 0xE3530000U);
    le_b(&e, 1U, loop);
    diff = le_mark(&e);
    le_patch(&e, p2, diff);
    le_put(&e, 0xE043000CU);
    le_put(&e, 0xE12FFF1EU);
    zero = le_mark(&e);
    le_patch(&e, p1, zero);
    le_put(&e, 0xE3A00000U);
    le_put(&e, 0xE12FFF1EU);
    /* _memcmp(r0, r1, r2): bounded loop without NUL check (uses r12, never callee-saved r4). */
    le_reg("_memcmp", &e);
    loop = le_mark(&e);
    le_put(&e, 0xE2522001U);
    p1 = le_b_fwd(&e, 3U);
    le_put(&e, 0xE4D03001U);
    le_put(&e, 0xE4D1C001U);
    le_put(&e, 0xE153000CU);
    p2 = le_b_fwd(&e, 1U);
    le_b(&e, 14U, loop);
    diff = le_mark(&e);
    le_patch(&e, p2, diff);
    le_put(&e, 0xE043000CU);
    le_put(&e, 0xE12FFF1EU);
    zero = le_mark(&e);
    le_patch(&e, p1, zero);
    le_put(&e, 0xE3A00000U);
    le_put(&e, 0xE12FFF1EU);
    /* _memset(r0, r1, r2): byte fill, returns dst. */
    le_reg("_memset", &e);
    le_put(&e, 0xE1A03000U);
    le_put(&e, 0xE3520000U);
    p1 = le_b_fwd(&e, 0U);
    loop = le_mark(&e);
    le_put(&e, 0xE4C01001U);
    le_put(&e, 0xE2522001U);
    le_b(&e, 1U, loop);
    done = le_mark(&e);
    le_patch(&e, p1, done);
    le_put(&e, 0xE1A00003U);
    le_put(&e, 0xE12FFF1EU);
    /* _bzero(r0, r1): zero fill. */
    le_reg("_bzero", &e);
    le_put(&e, 0xE3A02000U);
    le_put(&e, 0xE3510000U);
    p1 = le_b_fwd(&e, 0U);
    loop = le_mark(&e);
    le_put(&e, 0xE4C02001U);
    le_put(&e, 0xE2511001U);
    le_b(&e, 1U, loop);
    done = le_mark(&e);
    le_patch(&e, p1, done);
    le_put(&e, 0xE12FFF1EU);
    /* _memcpy(r0, r1, r2): forward copy, returns dst. */
    le_reg("_memcpy", &e);
    le_put(&e, 0xE1A03000U);
    le_put(&e, 0xE3520000U);
    p1 = le_b_fwd(&e, 0U);
    loop = le_mark(&e);
    le_put(&e, 0xE4D1C001U);
    le_put(&e, 0xE4C0C001U);
    le_put(&e, 0xE2522001U);
    le_b(&e, 1U, loop);
    done = le_mark(&e);
    le_patch(&e, p1, done);
    le_put(&e, 0xE1A00003U);
    le_put(&e, 0xE12FFF1EU);
    le_alias("___memcpy_chk", "_memcpy");
    /* _memmove(r0, r1, r2): forward when dst <= src, else backward. */
    le_reg("_memmove", &e);
    le_put(&e, 0xE1A03000U);
    le_put(&e, 0xE1500001U);
    p1 = le_b_fwd(&e, 9U);
    back = le_mark(&e);
    le_put(&e, 0xE2522001U);
    p2 = le_b_fwd(&e, 3U);
    le_put(&e, 0xE7D1C002U);
    le_put(&e, 0xE7C0C002U);
    le_b(&e, 14U, back);
    fwd = le_mark(&e);
    le_patch(&e, p1, fwd);
    le_put(&e, 0xE3520000U);
    p3 = le_b_fwd(&e, 0U);
    floop = le_mark(&e);
    le_put(&e, 0xE4D1C001U);
    le_put(&e, 0xE4C0C001U);
    le_put(&e, 0xE2522001U);
    le_b(&e, 1U, floop);
    done = le_mark(&e);
    le_patch(&e, p2, done);
    le_patch(&e, p3, done);
    le_put(&e, 0xE1A00003U);
    le_put(&e, 0xE12FFF1EU);
    le_alias("___memmove_chk", "_memmove");
    return e.n;
}

static uint32_t g_leaf_code[256];
static uint32_t g_leaf_nwords = 0U;

static bool le_ensure_code_locked(void)
{
    if (!g_leaf_ok) {
        g_leaf_nwords = le_emit_all(g_leaf_code, 256U);
        if (g_leaf_nwords == 0U ||
            (size_t)g_leaf_nwords * 4U > A32_IOS_LEAF_STUB_SIZE) {
            g_leaf_nwords = 0U;
            return false;
        }
        g_leaf_ok = true;
    }
    return true;
}

int a32_ios_emit_native_leaf_stubs(a32_ctx_t *ctx)
{
    uint32_t nwords;
    uint32_t tp0 = 0U;
    uint32_t tp8 = 0U;
    uint8_t *base;
    if (ctx == NULL) {
        return -1;
    }
    (void)pthread_mutex_lock(&g_leaf_mu);
    if (!le_ensure_code_locked()) {
        (void)pthread_mutex_unlock(&g_leaf_mu);
        return -1;
    }
    nwords = g_leaf_nwords;
    /* Verify TPIDRURO slot 0 holds the main pthread pointer. */
    if (ctx->tls_tp == 0U || ctx->ios_pthread_va == 0U ||
        !a32_guest_range_ok(ctx, ctx->tls_tp, 4U, (uint32_t)PROT_READ)) {
        (void)pthread_mutex_unlock(&g_leaf_mu);
        return -1;
    }
    memcpy(&tp0, a32_guest_ptr(ctx, ctx->tls_tp), 4U);
    if (tp0 != ctx->ios_pthread_va) {
        (void)pthread_mutex_unlock(&g_leaf_mu);
        return -1;
    }
    /* Verify TPIDRURO slot 2 holds the errno VA for the ___error leaf. */
    if (ctx->ios_errno_va == 0U ||
        !a32_guest_range_ok(ctx, ctx->tls_tp + 8U, 4U,
            (uint32_t)PROT_READ)) {
        (void)pthread_mutex_unlock(&g_leaf_mu);
        return -1;
    }
    memcpy(&tp8, a32_guest_ptr(ctx, ctx->tls_tp + 8U), 4U);
    if (tp8 != ctx->ios_errno_va) {
        (void)pthread_mutex_unlock(&g_leaf_mu);
        return -1;
    }
    /* Called from ios_setup_tramp while [A32_IOS_TRAMP_BASE_VA, +SIZE) is RW
     * prior to ios_seal(..., 0x5u, "ios_tramp"). */
    base = (uint8_t *)a32_guest_ptr(ctx, A32_IOS_LEAF_STUB_VA);
    memcpy(base, g_leaf_code, (size_t)nwords * 4U);
    (void)pthread_mutex_unlock(&g_leaf_mu);
    return 0;
}

/* ---- Extended symbol lookup + stub dispatch ---- */

typedef struct { const char *name; uint32_t id; } rt_sym_t;
static const rt_sym_t kRtSyms[] = {
    { "___divdi3", 120U }, { "___udivdi3", 121U },
    { "___moddi3", 122U }, { "___umoddi3", 123U },
    { "___divsi3", 124U }, { "___udivsi3", 125U },
    { "___modsi3", 126U }, { "___umodsi3", 127U },
    { "___divmodsi4", 128U }, { "___udivmodsi4", 129U },
};

uint32_t a32_ios_lookup_synth_ext(a32_ctx_t *ctx, const char *sym_name,
    bool *out_found)
{
    uint32_t i;
    if (out_found != NULL) {
        *out_found = false;
    }
    if (ctx == NULL || sym_name == NULL) {
        return 0U;
    }
    (void)pthread_mutex_lock(&g_leaf_mu);
    (void)le_ensure_code_locked();
    for (i = 0U; i < g_leaf_count; i++) {
        if (strcmp(sym_name, g_leaf_syms[i].name) == 0) {
            uint32_t va = A32_IOS_LEAF_STUB_VA + g_leaf_syms[i].off * 4U;
            (void)pthread_mutex_unlock(&g_leaf_mu);
            if (out_found != NULL) {
                *out_found = true;
            }
            return va;
        }
    }
    (void)pthread_mutex_unlock(&g_leaf_mu);
    for (i = 0U; i < sizeof(kRtSyms) / sizeof(kRtSyms[0]); i++) {
        if (strcmp(sym_name, kRtSyms[i].name) == 0) {
            if (out_found != NULL) {
                *out_found = true;
            }
            return (A32_IOS_STUB_BASE_VA + kRtSyms[i].id * 8U) | 1U;
        }
    }
    return 0U;
}

int a32_ios_handle_stub_ext(a32_ctx_t *ctx, uint32_t stub_id, bool *out_handled)
{
    if (out_handled != NULL) {
        *out_handled = false;
    }
    if (ctx == NULL) {
        return 0;
    }
    switch (stub_id) {
    case 5U:
        if (out_handled != NULL) {
            *out_handled = true;
        }
        ax_exit(ctx, ctx->regs[0]);
        return 0;
    case 7U:
        if (out_handled != NULL) {
            *out_handled = true;
        }
        a32_ios_exit_teardown(ctx, (int)ctx->regs[0]);
        return 0;
    case 8U:
        if (out_handled != NULL) {
            *out_handled = true;
        }
        ax_record(ctx, ctx->regs[0], 0U, false);
        return 0;
    case 9U:
        if (out_handled != NULL) {
            *out_handled = true;
        }
        ax_record(ctx, ctx->regs[0], ctx->regs[1], true);
        return 0;
    case 0xFBU:
        if (!g_ax_exiting) {
            return 0;
        }
        if (out_handled != NULL) {
            *out_handled = true;
        }
        memcpy(ctx->regs, g_ax_saved, sizeof(g_ax_saved));
        ctx->cpsr = g_ax_cpsr;
        ax_call_next(ctx);
        return 0;
    case 36U:
    case 37U:
        if (out_handled != NULL) {
            *out_handled = true;
        }
        ex_strto(ctx, stub_id);
        return 0;
    default:
        break;
    }
    if (stub_id >= 120U && stub_id <= 129U) {
        if (out_handled != NULL) {
            *out_handled = true;
        }
        ex_rt(ctx, stub_id);
        return 0;
    }
    return 0;
}
