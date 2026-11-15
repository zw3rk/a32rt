/*
 * ios_commpage_stack.c — XNU commpage, trampolines, and Darwin stack setup
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#define _GNU_SOURCE
#include "a32rt.h"
#include "ios/ios.h"
#include "os/backend.h"
#include "vm/vm32.h"
#include "vm/vm_priv.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define IOS_STACK_TOP 0xE0000000u
#define IOS_STACK_SIZE (8u * 1024u * 1024u)
#define IOS_STACK_BASE (IOS_STACK_TOP - IOS_STACK_SIZE)
#define IOS_PATH_MAX 1024u

static int ios_map_rw(a32_ctx_t *ctx, uint32_t va, uint32_t len)
{
    void *res = NULL;
    if (a32_backend_map_anon(a32_guest_ptr(ctx, va), len,
                             PROT_READ | PROT_WRITE, 1, 1, 0, &res) != 0) {
        fprintf(stderr, "a32rt: failed to map guest [0x%x, 0x%x): %s\n", va,
                va + len, strerror(errno));
        return -1;
    }
    return 0;
}

static int ios_seal(a32_ctx_t *ctx, uint32_t va, uint32_t len, uint32_t prot,
                    const char *name)
{
    if (a32_vm_record_region(ctx, va, va + len, prot, 0x12u, name) != 0) {
        fprintf(stderr, "a32rt: failed to record %s\n", name);
        return -1;
    }
    if (a32_vm32_protect(ctx, va, (size_t)len, prot) != 0) {
        fprintf(stderr, "a32rt: failed to seal %s: %s\n", name,
                strerror(errno));
        return -1;
    }
    return 0;
}

static void ios_w8(a32_ctx_t *ctx, uint32_t va, uint8_t v)
{
    *(uint8_t *)a32_guest_ptr(ctx, va) = v;
}

static void ios_w16(a32_ctx_t *ctx, uint32_t va, uint16_t v)
{
    uint8_t *p = (uint8_t *)a32_guest_ptr(ctx, va);
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void ios_w32(a32_ctx_t *ctx, uint32_t va, uint32_t v)
{
    uint8_t *p = (uint8_t *)a32_guest_ptr(ctx, va);
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void ios_w64(a32_ctx_t *ctx, uint32_t va, uint64_t v)
{
    ios_w32(ctx, va, (uint32_t)(v & 0xFFFFFFFFULL));
    ios_w32(ctx, va + 4u, (uint32_t)(v >> 32));
}

static void ios_wstr(a32_ctx_t *ctx, uint32_t va, const char *s)
{
    size_t n = strlen(s) + 1u;
    memcpy(a32_guest_ptr(ctx, va), s, n);
}

/* Populate one 4 KiB XNU 32-bit _COMM_PAGE. */
static void ios_fill_commpage(a32_ctx_t *ctx, uint32_t va)
{
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    uint8_t ncpus;
    if (ncpu < 1L) {
        ncpu = 1L;
    }
    if (ncpu > 64L) {
        ncpu = 64L;
    }
    ncpus = (uint8_t)ncpu;
    memset(a32_guest_ptr(ctx, va), 0, 0x1000u);
    ios_wstr(ctx, va + 0x000u, "commpage 32-bit");
    ios_w16(ctx, va + 0x01Eu, 13);
    ios_w8(ctx, va + 0x022u, ncpus);
    ios_w16(ctx, va + 0x026u, 64);
    ios_w8(ctx, va + 0x034u, ncpus);
    ios_w8(ctx, va + 0x035u, ncpus);
    ios_w8(ctx, va + 0x036u, ncpus);
    ios_w64(ctx, va + 0x058u, 0x100000000ULL);
    ios_w8(ctx, va + 0x085u, 12);
    ios_w8(ctx, va + 0x087u, 12);
}

/* Thumb-2 MOVW/MOVT for rd (Armv7-M/A encoding); advances *pp. */
static void ios_emit_movw(uint16_t **pp, uint32_t rd, uint32_t imm16)
{
    uint32_t i = (imm16 >> 11) & 1u;
    uint32_t imm4 = (imm16 >> 12) & 0xFu;
    uint32_t imm3 = (imm16 >> 8) & 7u;
    uint32_t imm8 = imm16 & 0xFFu;
    uint16_t hw0 = (uint16_t)(0xF240u | (i << 10) | imm4);
    uint16_t hw1 = (uint16_t)((imm3 << 12) | (rd << 8) | imm8);
    *(*pp)++ = hw0;
    *(*pp)++ = hw1;
}

static void ios_emit_movt(uint16_t **pp, uint32_t rd, uint32_t imm16)
{
    uint32_t i = (imm16 >> 11) & 1u;
    uint32_t imm4 = (imm16 >> 12) & 0xFu;
    uint32_t imm3 = (imm16 >> 8) & 7u;
    uint32_t imm8 = imm16 & 0xFFu;
    uint16_t hw0 = (uint16_t)(0xF2C0u | (i << 10) | imm4);
    uint16_t hw1 = (uint16_t)((imm3 << 12) | (rd << 8) | imm8);
    *(*pp)++ = hw0;
    *(*pp)++ = hw1;
}

/* mov r0-r3 from r4-r7, then movw/movt r8 + blx r8 to target. */
static void ios_emit_call_target(uint16_t **pp, uint32_t target)
{
    *(*pp)++ = 0x4620u;
    *(*pp)++ = 0x4629u;
    *(*pp)++ = 0x4632u;
    *(*pp)++ = 0x463Bu;
    ios_emit_movw(pp, 8u, target & 0xFFFFu);
    ios_emit_movt(pp, 8u, (target >> 16) & 0xFFFFu);
    *(*pp)++ = 0x47C0u;
}

static int ios_setup_tramp(a32_ctx_t *ctx)
{
    uint16_t *p;
    uint16_t *crt0_end;
    uint32_t k;
    if (ios_map_rw(ctx, A32_IOS_TRAMP_BASE_VA, A32_IOS_TRAMP_SIZE) != 0) {
        return -1;
    }
    memset(a32_guest_ptr(ctx, A32_IOS_TRAMP_BASE_VA), 0, A32_IOS_TRAMP_SIZE);
    /* CRT0: argc/argv/envp/apple runner + LC_MAIN call. */
    p = (uint16_t *)a32_guest_ptr(ctx, A32_IOS_CRT0_VA);
    crt0_end = (uint16_t *)a32_guest_ptr(ctx, A32_IOS_SIGTRAMP_VA);
    if (ctx->ios_has_crt0) {
        *p++ = 0x9C00u; /* ldr r4, [sp, #0]  (argc) */
        *p++ = 0xAD01u; /* add r5, sp, #4    (argv) */
        *p++ = 0x1C60u;
        *p++ = 0x0080u;
        *p++ = 0x182Eu;
        *p++ = 0x4637u;
        *p++ = 0x6838u;
        *p++ = 0x3704u;
        *p++ = 0x2800u;
        *p++ = 0xD1FBu;
        for (k = 0; k < ctx->ios_init_count && k < 64u; k++) {
            if ((size_t)(crt0_end - p) < 9u) {
                return -1;
            }
            ios_emit_call_target(&p, ctx->ios_init_va[k]);
        }
        if ((size_t)(crt0_end - p) < 20u) {
            return -1;
        }
        ios_emit_call_target(&p, ctx->ios_main_va);
        ios_emit_movw(&p, 12u, A32_IOS_C_EXIT_STUB_VA & 0xFFFFu);
        ios_emit_movt(&p, 12u, (A32_IOS_C_EXIT_STUB_VA >> 16) & 0xFFFFu);
        *p++ = 0x4760u;
        *p++ = 0xF04Fu;
        *p++ = 0x0C01u;
        *p++ = 0xDF80u;
        *p++ = 0xDE00u;
    }
    /* __sigtramp: SYS_sigreturn via svc #0x80. */
    p = (uint16_t *)a32_guest_ptr(ctx, A32_IOS_SIGTRAMP_VA);
    *p++ = 0x9800u;
    *p++ = 0x9901u;
    *p++ = 0xF04Fu;
    *p++ = 0x0CB8u;
    *p++ = 0xDF80u;
    *p++ = 0xDE00u;
    /* pthread start wrapper. */
    p = (uint16_t *)a32_guest_ptr(ctx, A32_IOS_THRTRAMP_VA);
    *p++ = 0x4604u;
    *p++ = 0x4608u;
    *p++ = 0x47A0u;
    *p++ = 0xF04Fu;
    *p++ = 0x0CFFu;
    *p++ = 0xDF81u;
    *p++ = 0xDE00u;
    /* Stub island: mov.w r12, #id; svc #0x81; bx lr. */
    p = (uint16_t *)a32_guest_ptr(ctx, A32_IOS_STUB_BASE_VA);
    for (k = 0; k < A32_IOS_MAX_STUBS; k++) {
        *p++ = 0xF04Fu;
        *p++ = (uint16_t)(0x0C00u | k);
        *p++ = 0xDF81u;
        *p++ = 0x4770u;
    }
    if (a32_ios_emit_native_leaf_stubs(ctx) != 0) {
        return -1;
    }
    if (ios_seal(ctx, A32_IOS_TRAMP_BASE_VA, A32_IOS_TRAMP_SIZE, 0x5u,
                 "ios_tramp") != 0) {
        return -1;
    }
    return 0;
}

static int ios_setup_runtime_data(a32_ctx_t *ctx)
{
    static const uint16_t fds[3] = { 0, 1, 2 };
    static const uint32_t std_offs[3] = { 0x600u, 0x680u, 0x700u };
    uint32_t i;
    if (ios_map_rw(ctx, A32_IOS_RUNTIME_DATA_VA, 0x1000u) != 0) {
        return -1;
    }
    memset(a32_guest_ptr(ctx, A32_IOS_RUNTIME_DATA_VA), 0, 0x1000u);
    ios_w32(ctx, A32_IOS_RUNTIME_DATA_VA + 0x000u, 0xFFFF5400u);
    ios_w32(ctx, A32_IOS_RUNTIME_DATA_VA + 0x004u, 1u);
    ios_w32(ctx, A32_IOS_RUNTIME_DATA_VA + 0x008u, 0xFFFF5500u);
    ios_w32(ctx, A32_IOS_RUNTIME_DATA_VA + 0x400u, 0x54485244u);
    ios_w32(ctx, A32_IOS_RUNTIME_DATA_VA + 0x500u, 0);
    ios_w32(ctx, A32_IOS_RUNTIME_DATA_VA + 0x510u, 0x000A0DFFu);
    ios_w32(ctx, A32_IOS_RUNTIME_DATA_VA + 0x530u, 0xFFFF5600u);
    ios_w32(ctx, A32_IOS_RUNTIME_DATA_VA + 0x534u, 0xFFFF5680u);
    ios_w32(ctx, A32_IOS_RUNTIME_DATA_VA + 0x538u, 0xFFFF5700u);
    ios_w32(ctx, A32_IOS_RUNTIME_DATA_VA + 0x540u, 1u);
    ios_w32(ctx, A32_IOS_RUNTIME_DATA_VA + 0x548u, 1u);
    for (i = 0; i < 3u; i++) {
        uint32_t fva = A32_IOS_RUNTIME_DATA_VA + std_offs[i];
        uint8_t *f = (uint8_t *)a32_guest_ptr(ctx, fva);
        memset(f, 0, 0x60u);
        f[0] = (uint8_t)(fds[i] & 0xFFu);
        f[1] = (uint8_t)((fds[i] >> 8) & 0xFFu);
        f[2] = (uint8_t)((fds[i] >> 16) & 0xFFu);
        f[3] = (uint8_t)((fds[i] >> 24) & 0xFFu);
        f[14] = (uint8_t)(fds[i] & 0xFFu);
        f[15] = (uint8_t)((fds[i] >> 8) & 0xFFu);
    }
    ctx->tls_tp = A32_IOS_RUNTIME_DATA_VA;
    ctx->ios_pthread_va = 0xFFFF5400u;
    ctx->ios_errno_va = 0xFFFF5500u;
    if (ios_seal(ctx, A32_IOS_RUNTIME_DATA_VA, 0x1000u, 0x3u,
                 "ios_runtime") != 0) {
        return -1;
    }
    a32_ios_refresh_runtime_pid(ctx);
    return 0;
}

int a32_ios_commpage_setup(a32_ctx_t *ctx)
{
    if (ctx == NULL) {
        return -1;
    }
    if (ios_map_rw(ctx, A32_IOS_COMMPAGE_LEGACY_VA, 0x1000u) != 0) {
        return -1;
    }
    ios_fill_commpage(ctx, A32_IOS_COMMPAGE_LEGACY_VA);
    if (ios_seal(ctx, A32_IOS_COMMPAGE_LEGACY_VA, 0x1000u, 0x1u,
                 "ios_commpage") != 0) {
        return -1;
    }
    if (ios_map_rw(ctx, A32_IOS_COMMPAGE_VA, 0x1000u) != 0) {
        return -1;
    }
    ios_fill_commpage(ctx, A32_IOS_COMMPAGE_VA);
    if (ios_seal(ctx, A32_IOS_COMMPAGE_VA, 0x1000u, 0x1u, "ios_commpage") != 0) {
        return -1;
    }
    if (ios_setup_runtime_data(ctx) != 0) {
        return -1;
    }
    if (ios_setup_tramp(ctx) != 0) {
        return -1;
    }
    return 0;
}

/* ---- Darwin stack setup ---- */

static uint32_t ios_push_bytes(a32_ctx_t *ctx, uint32_t *sp, const void *data,
                               size_t len)
{
    if (*sp < ctx->stack_base || len > (size_t)(*sp - ctx->stack_base)) {
        return 0;
    }
    *sp -= (uint32_t)len;
    memcpy(a32_guest_ptr(ctx, *sp), data, len);
    return *sp;
}

static uint32_t ios_push_str(a32_ctx_t *ctx, uint32_t *sp, const char *s)
{
    return ios_push_bytes(ctx, sp, s, strlen(s) + 1u);
}

int a32_ios_stack_setup(a32_ctx_t *ctx, const a32_elf_info_t *info,
                        int guest_argc, char **guest_argv, char **host_envp,
                        uint32_t *out_sp)
{
    void *host_stack;
    void *res = NULL;
    uint32_t sp;
    uint32_t arg_va[512];
    uint32_t env_va[512];
    uint32_t apple_va[4];
    int argc = 0;
    int envc = 0;
    uint32_t i;
    char exec_path[IOS_PATH_MAX];
    char apple0[IOS_PATH_MAX];
    uint32_t words[8 + 512 + 512 + 8];
    size_t nwords = 0;
    uint32_t table_va;
    uint32_t envp0 = 0;
    (void)info;
    if (ctx == NULL || out_sp == NULL || guest_argc < 1 || guest_argv == NULL ||
        guest_argv[0] == NULL) {
        return -1;
    }
    host_stack = a32_guest_ptr(ctx, IOS_STACK_BASE);
    if (a32_vm_range_overlaps(IOS_STACK_BASE, IOS_STACK_TOP) &&
        a32_vm_munmap(ctx, IOS_STACK_BASE, IOS_STACK_SIZE) != 0) {
        fprintf(stderr, "a32rt: failed to retire stack overlap\n");
        return -1;
    }
    if (a32_backend_map_anon(host_stack, IOS_STACK_SIZE,
                             PROT_READ | PROT_WRITE, 1, 1, 0, &res) != 0) {
        fprintf(stderr, "a32rt: failed to map iOS stack: %s\n",
                strerror(errno));
        return -1;
    }
    ctx->stack_top = IOS_STACK_TOP;
    ctx->stack_base = IOS_STACK_BASE;
    a32_vm_record_region(ctx, IOS_STACK_BASE, IOS_STACK_TOP,
                         (uint32_t)(PROT_READ | PROT_WRITE), 0x12u, "[stack]");
    sp = IOS_STACK_TOP;
    if (ctx->exec_path != NULL && ctx->exec_path[0] != '\0') {
        size_t n = strlen(ctx->exec_path);
        if (n >= sizeof exec_path) {
            n = sizeof exec_path - 1u;
        }
        memcpy(exec_path, ctx->exec_path, n);
        exec_path[n] = '\0';
    } else {
        size_t n = strlen(guest_argv[0]);
        if (n >= sizeof exec_path) {
            n = sizeof exec_path - 1u;
        }
        memcpy(exec_path, guest_argv[0], n);
        exec_path[n] = '\0';
    }
    {
        static const char pfx[] = "executable_path=";
        size_t np = sizeof pfx - 1u;
        size_t ne = strlen(exec_path);
        if (np + ne + 1u > sizeof apple0) {
            return -1;
        }
        memcpy(apple0, pfx, np);
        memcpy(apple0 + np, exec_path, ne + 1u);
    }
    {
        char guard_str[64];
        uint32_t guard = 0x000A0DFFu;
        uint8_t *gp = (uint8_t *)a32_guest_ptr(ctx,
            A32_IOS_RUNTIME_DATA_VA + A32_IOS_STACK_GUARD_OFF);
        memcpy(&guard, gp, 4);
        if (guard == 0u) {
            guard = 0x000A0DFFu;
            memcpy(gp, &guard, 4);
        }
        snprintf(guard_str, sizeof guard_str, "stack_guard=0x%08x", guard);
        apple_va[0] = ios_push_str(ctx, &sp, apple0);
        apple_va[1] = ios_push_str(ctx, &sp, "pfz=0xffff4000");
        apple_va[2] = ios_push_str(ctx, &sp, guard_str);
        apple_va[3] = ios_push_str(ctx, &sp,
            "malloc_entropy=0x11223344,0x55667788");
    }
    if (apple_va[0] == 0u || apple_va[1] == 0u || apple_va[2] == 0u ||
        apple_va[3] == 0u) {
        return -1;
    }
    if (host_envp != NULL) {
        while (envc < 511 && host_envp[envc] != NULL) {
            envc++;
        }
        for (i = (uint32_t)envc; i > 0u; i--) {
            env_va[i - 1u] = ios_push_str(ctx, &sp, host_envp[i - 1u]);
            if (env_va[i - 1u] == 0u) {
                return -1;
            }
        }
    }
    if (guest_argv != NULL) {
        argc = 0;
        while (argc < guest_argc && argc < 511 && guest_argv[argc] != NULL) {
            argc++;
        }
        if (argc < 1) {
            return -1;
        }
        for (i = (uint32_t)argc; i > 0u; i--) {
            arg_va[i - 1u] = ios_push_str(ctx, &sp, guest_argv[i - 1u]);
            if (arg_va[i - 1u] == 0u) {
                return -1;
            }
        }
    } else {
        return -1;
    }
    sp &= ~0xFu;
    if (ctx->ios_uses_dyld) {
        words[nwords++] = ctx->ios_mh_va;
    }
    words[nwords++] = (uint32_t)argc;
    for (i = 0; i < (uint32_t)argc; i++) {
        words[nwords++] = arg_va[i];
    }
    words[nwords++] = 0;
    for (i = 0; i < (uint32_t)envc; i++) {
        words[nwords++] = env_va[i];
    }
    words[nwords++] = 0;
    for (i = 0; i < 4u; i++) {
        words[nwords++] = apple_va[i];
    }
    words[nwords++] = 0;
    if (sp < ctx->stack_base ||
        (size_t)(sp - ctx->stack_base) < nwords * 4u + 16u) {
        return -1;
    }
    table_va = (sp - (uint32_t)(nwords * 4u)) & ~0xFu;
    memcpy(a32_guest_ptr(ctx, table_va), words, nwords * 4u);
    envp0 = table_va +
            ((ctx->ios_uses_dyld ? 1u : 0u) + 1u + (uint32_t)argc + 1u) * 4u;
    ios_w32(ctx, A32_IOS_RUNTIME_DATA_VA + A32_IOS_ENVIRON_PTR_OFF, envp0);
    ctx->guest_argc = guest_argc;
    ctx->guest_argv = guest_argv;
    ctx->host_envp = host_envp;
    *out_sp = table_va;
    return 0;
}
