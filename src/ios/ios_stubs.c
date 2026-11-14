/*
 * src/ios/ios_stubs.c — synthetic libSystem.B stubs (svc #0x81, ids 0..87)
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Worker-F4 unit: synthetic libSystem.B stubs (svc #0x81). Curated C11.
 */
#if !defined(__APPLE__)
#define _GNU_SOURCE
#else
#define _DARWIN_C_SOURCE
#endif
#include "ios/ios.h"
#include "os/sys_priv.h"
#include "vm/vm32.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#define RD PROT_READ
#define WR PROT_WRITE
#define DE_PERM 1
#define DE_SRCH 3
#define DE_AGAIN 35
#define DE_NOMEM 12
#define DE_BUSY 16
#define DE_INVAL 22
#define DE_TIMEDOUT 60
static void *gmem(a32_ctx_t *c, uint32_t va, uint32_t len, int prot)
{
    if (len == 0U) return a32_guest_ptr(c, va);
    if (!a32_guest_range_ok(c, va, len, prot)) return NULL;
    return a32_guest_ptr(c, va);
}
static const char *gstr(a32_ctx_t *c, uint32_t va)
{
    uint32_t p = va;
    int i;
    if (va == 0U)
        return NULL;
    for (i = 0; i < 16; i++) {
        uint32_t rem = 0x1000U - (p & 0xFFFU);
        const char *s;
        if (!a32_guest_range_ok(c, p, 1U, RD))
            return NULL;
        s = (const char *)a32_guest_ptr(c, p);
        if (memchr(s, '\0', (size_t)rem) != NULL)
            return (const char *)a32_guest_ptr(c, va);
        p += rem;
        if (p < va)
            return NULL;
    }
    return NULL;
}
static void serr(a32_ctx_t *c, int host_err)
{
    int32_t de = (int32_t)a32_ios_host_to_darwin_errno(host_err);
    if (c->ios_errno_va != 0U)
        (void)guest_write(c, c->ios_errno_va, &de, 4U);
    c->regs[0] = 0xFFFFFFFFU;
}
static uint32_t g_scratch_va(a32_ctx_t *c)
{
    static uint32_t va = 0U;
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    (void)pthread_mutex_lock(&mu);
    if (va == 0U) {
        uint32_t r = a32_vm_mmap2(c, 0U, 0x1000U,
            (uint32_t)(PROT_READ | PROT_WRITE), 0x22U, -1, 0U);
        if (r <= 0xFFFFF000U)
            va = r;
    }
    (void)pthread_mutex_unlock(&mu);
    return va;
}
/* libSystem.B stubs id 0..105; 0xFC/0xFD resumes, 0xFF thread-exit. */
typedef struct { const char *name; uint8_t id; } synth_ent_t;
static const synth_ent_t kSynth[] = {
    {"dyld_stub_binder", 0}, {"_dyld_stub_binder", 1},
    {"___stack_chk_fail", 2}, {"___error", 3}, {"___assert_rtn", 4},
    {"_exit", 5}, {"_abort", 6}, {"__exit", 7},
    {"_atexit", 8}, {"___cxa_atexit", 9}, {"_malloc", 10}, {"_calloc", 11},
    {"_realloc", 12},
    {"_free", 13}, {"_posix_memalign", 14}, {"_strlen", 15},
    {"_strnlen", 16}, {"_strcmp", 17}, {"_strncmp", 18}, {"_strcpy", 19},
    {"_strncpy", 20}, {"_strcat", 21}, {"_strchr", 22}, {"_strrchr", 23},
    {"_strstr", 24}, {"_strdup", 25}, {"_memcpy", 26}, {"_memmove", 27},
    {"_memset", 28}, {"_memcmp", 29}, {"_bzero", 30}, {"___memcpy_chk", 31},
    {"___memmove_chk", 32}, {"___memset_chk", 33}, {"___strcpy_chk", 34},
    {"_atoi", 35}, {"_strtol", 36}, {"_strtoul", 37}, {"_qsort", 38},
    {"_puts", 39}, {"_putchar", 40}, {"_fputs", 41}, {"_fputc", 42},
    {"_fwrite", 43}, {"_fread", 44}, {"_fflush", 45}, {"_fopen", 46},
    {"_fclose", 47}, {"_printf", 48}, {"_fprintf", 49}, {"_snprintf", 50},
    {"___snprintf_chk", 51}, {"_vsnprintf", 52}, {"_open", 53},
    {"_close", 54}, {"_unlink", 55}, {"_read", 56}, {"_write", 57},
    {"_lseek", 58}, {"_stat", 59}, {"_stat$INODE64", 60}, {"_fstat", 61},
    {"_lstat", 62}, {"_mmap", 63}, {"_munmap", 64}, {"_mprotect", 65},
    {"_getpid", 66}, {"_getppid", 67}, {"_getuid", 68}, {"_geteuid", 69},
    {"_gettimeofday", 70}, {"_usleep", 71}, {"_nanosleep", 72},
    {"_sleep", 73}, {"_sysctl", 74}, {"_sysctlbyname", 75},
    {"_getentropy", 76}, {"_getenv", 77}, {"_mach_absolute_time", 78},
    {"_mach_timebase_info", 79}, {"_mach_task_self", 80},
    {"_mach_host_self", 81}, {"_mach_thread_self", 82},
    {"_sys_icache_invalidate", 83}, {"_sigaction", 84},
    {"_sigprocmask", 85}, {"_kill", 86}, {"_raise", 87},
    {"_pthread_self", 88}, {"_pthread_create", 89}, {"_pthread_join", 90},
    {"_pthread_detach", 91}, {"_pthread_mutex_init", 92},
    {"_pthread_mutex_lock", 93}, {"_pthread_mutex_trylock", 94},
    {"_pthread_mutex_unlock", 95}, {"_pthread_mutex_destroy", 96},
    {"_pthread_cond_init", 97}, {"_pthread_cond_wait", 98},
    {"_pthread_cond_signal", 99}, {"_pthread_cond_broadcast", 100},
    {"_pthread_cond_destroy", 101}, {"_pthread_once", 102},
    {"_pthread_key_create", 103}, {"_pthread_setspecific", 104},
    {"_pthread_getspecific", 105},
};
#define QSORT_RESUME_ID 0xFDU
#define ONCE_RESUME_ID 0xFCU
#define THR_EXIT_ID 0xFFU
static const synth_ent_t kData[] = {
    {"___stack_chk_guard", 0x10U}, {"_environ", 0x20U},
    {"___stdinp", 0x30U}, {"___stdoutp", 0x34U}, {"___stderrp", 0x38U},
    {"_optind", 0x40U}, {"_optarg", 0x44U}, {"_opterr", 0x48U},
    {"_optopt", 0x4CU},
};
uint32_t a32_ios_lookup_synth_sym(a32_ctx_t *ctx, const char *sym_name,
    bool *out_found)
{
    size_t i;
    uint32_t ev;
    bool ef = false;
    if (out_found != NULL)
        *out_found = false;
    if (ctx == NULL || sym_name == NULL)
        return 0U;
    ev = a32_ios_lookup_synth_ext(ctx, sym_name, &ef);
    if (ef) { if (out_found != NULL) *out_found = true; return ev; }
    for (i = 0U; i < sizeof(kData) / sizeof(kData[0]); i++) {
        if (strcmp(sym_name, kData[i].name) == 0) {
            if (out_found != NULL)
                *out_found = true;
            return 0xFFFF5500U + (uint32_t)kData[i].id;
        }
    }
    if (strcmp(sym_name, "_mach_task_self_") == 0) {
        uint32_t v = 0x103U;
        (void)guest_write(ctx, 0xFFFF5550U, &v, 4U);
        if (out_found != NULL)
            *out_found = true;
        return 0xFFFF5550U;
    }
    for (i = 0U; i < sizeof(kSynth) / sizeof(kSynth[0]); i++) {
        if (strcmp(sym_name, kSynth[i].name) == 0) {
            if (out_found != NULL)
                *out_found = true;
            return (A32_IOS_STUB_BASE_VA + (uint32_t)kSynth[i].id * 8U) | 1U;
        }
    }
    return 0U;
}
/* Per-host-thread guest-call state (once init, qsort compar). */
typedef struct { bool active;
    uint32_t kind, a, b, c, d, e, f, saved_cpsr, saved[16]; void *tmp; } gcall_t;
static pthread_key_t g_gc_key;
static pthread_once_t g_gc_once = PTHREAD_ONCE_INIT;
static void gc_make_key(void) { (void)pthread_key_create(&g_gc_key, free); }
static gcall_t *gc_self(void) { gcall_t *g;
    (void)pthread_once(&g_gc_once, gc_make_key);
    g = (gcall_t *)pthread_getspecific(g_gc_key);
    if (g == NULL) { g = (gcall_t *)calloc(1, sizeof(*g)); (void)pthread_setspecific(g_gc_key, g); }
    return g;
}
#define ARENA_SZ (1024U * 1024U)
static uint32_t g_arena_va = 0U, g_arena_end = 0U, g_arena_free = 0U, g_arena_off = 0U;
static pthread_mutex_t g_heap_mu = PTHREAD_MUTEX_INITIALIZER;
static bool heap_ensure_locked(a32_ctx_t *c) { if (g_arena_va != 0U) return true;
    g_arena_va = a32_vm_mmap2(c, 0U, ARENA_SZ, (uint32_t)(RD | WR), 0x22U, -1, 0U);
    if (g_arena_va > 0xFFFFF000U) { g_arena_va = 0U; return false; }
    g_arena_end = g_arena_va + ARENA_SZ; g_arena_off = 0U;
    return true;
}
static uint32_t gmalloc(a32_ctx_t *c, uint32_t n)
{
    uint32_t need = (n == 0U ? 1U : n), cur, prev = 0xFFFFFFFFU, out, brk;
    uint32_t h[4]; uint32_t mg = 0xA110C8EDU; uint32_t steps = 0U;
    if (n > ARENA_SZ - 64U) return 0U;
    need = (need + 15U) & ~15U;
    (void)pthread_mutex_lock(&g_heap_mu);
    if (!heap_ensure_locked(c)) { (void)pthread_mutex_unlock(&g_heap_mu); return 0U; }
    for (cur = g_arena_free; cur != 0U; prev = cur, cur = h[2]) {
        bool bad = (++steps > 4096U || cur < g_arena_va + 16U ||
            cur > g_arena_va + g_arena_off ||
            !a32_guest_range_ok(c, cur - 16U, 16U, PROT_READ | PROT_WRITE));
        if (!bad) memcpy(h, a32_guest_ptr(c, cur - 16U), 16U);
        if (!bad && h[1] != 0xF2EEF2EEU) bad = true;
        if (bad) {
            if (prev == 0xFFFFFFFFU) g_arena_free = 0U;
            else memset(a32_guest_ptr(c, prev - 8U), 0, 4U);
            break;
        }
        if (h[0] >= need) {
            if (prev == 0xFFFFFFFFU) g_arena_free = h[2];
            else memcpy(a32_guest_ptr(c, prev - 8U), &h[2], 4U);
            h[1] = mg;
            memcpy(a32_guest_ptr(c, cur - 16U), h, 8U);
            (void)pthread_mutex_unlock(&g_heap_mu);
            return cur;
        }
    }
    brk = g_arena_va + g_arena_off;
    if ((uint64_t)brk + need + 16U > (uint64_t)g_arena_end) {
        (void)pthread_mutex_unlock(&g_heap_mu);
        return 0U;
    }
    out = brk + 16U; h[0] = need; h[1] = 0xA110C8EDU; h[2] = 0U; h[3] = 0U;
    memcpy(a32_guest_ptr(c, brk), h, 16U);
    g_arena_off = (brk + need + 16U) - g_arena_va;
    (void)pthread_mutex_unlock(&g_heap_mu);
    return out;
}
static void gfree(a32_ctx_t *c, uint32_t p) { uint32_t h[4], nx, q;
    if (p == 0U) return;
    (void)pthread_mutex_lock(&g_heap_mu);
    if (!(g_arena_va != 0U && p >= g_arena_va + 16U && p <= g_arena_va + g_arena_off)) goto gout;
    if (!a32_guest_range_ok(c, p - 16U, 16U, PROT_READ | PROT_WRITE)) goto gout;
    memcpy(h, a32_guest_ptr(c, p - 16U), 16U);
    if (h[0] == 0xA110F00DU) { memcpy(&q, a32_guest_ptr(c, p - 8U), 4U);
        { uint32_t z = 0U; memcpy(a32_guest_ptr(c, p - 16U), &z, 4U); } p = q;
        if (!(g_arena_va != 0U && p >= g_arena_va + 16U && p <= g_arena_va + g_arena_off)) goto gout;
        if (!a32_guest_range_ok(c, p - 16U, 16U, PROT_READ | PROT_WRITE)) goto gout;
        memcpy(h, a32_guest_ptr(c, p - 16U), 16U); }
    if (h[1] != 0xA110C8EDU) goto gout;
    { uint32_t f = 0xF2EEF2EEU; memcpy((char *)a32_guest_ptr(c, p - 16U) + 4U, &f, 4U); }
    nx = g_arena_free; memcpy(a32_guest_ptr(c, p - 8U), &nx, 4U); g_arena_free = p;
gout: (void)pthread_mutex_unlock(&g_heap_mu);
}
static uint32_t grealloc(a32_ctx_t *c, uint32_t p, uint32_t n)
{
    uint32_t orig_p = p, q, h[4], hr[4], cp, old_sz, np;
    if (p == 0U) return gmalloc(c, n);
    if (n == 0U) { gfree(c, p); return 0U; }
    if (!(g_arena_va != 0U && p >= g_arena_va + 16U && p <= g_arena_va + g_arena_off)) return 0U;
    if (!a32_guest_range_ok(c, p - 16U, 16U, PROT_READ | PROT_WRITE)) return 0U;
    memcpy(h, a32_guest_ptr(c, p - 16U), 16U);
    if (h[0] == 0xA110F00DU) {
        uint32_t fsz = h[1], end_va;
        memcpy(&q, a32_guest_ptr(c, p - 8U), 4U);
        if (!a32_guest_range_ok(c, q - 16U, 16U, PROT_READ | PROT_WRITE)) return 0U;
        memcpy(hr, a32_guest_ptr(c, q - 16U), 16U);
        if (hr[1] != 0xA110C8EDU) return 0U;
        end_va = q + hr[0]; old_sz = (end_va > orig_p) ? (end_va - orig_p) : 0U;
        if (fsz != 0U && fsz < old_sz) old_sz = fsz;
        np = gmalloc(c, n); if (np == 0U) return 0U;
        cp = (old_sz < n) ? old_sz : n;
        if (cp != 0U && a32_guest_range_ok(c, orig_p, cp, (uint32_t)RD))
            memcpy(a32_guest_ptr(c, np), a32_guest_ptr(c, orig_p), cp);
        gfree(c, orig_p); return np;
    }
    if (h[1] != 0xA110C8EDU) return 0U;
    old_sz = h[0]; np = gmalloc(c, n); if (np == 0U) return 0U;
    cp = (old_sz < n) ? old_sz : n;
    if (cp != 0U && a32_guest_range_ok(c, orig_p, cp, (uint32_t)RD))
        memcpy(a32_guest_ptr(c, np), a32_guest_ptr(c, orig_p), cp);
    gfree(c, orig_p); return np;
}
/* String & memory stubs (ids 15..37); args in r0..r2. */
static void stub_strmem(a32_ctx_t *c, uint32_t id)
{
    uint32_t a0 = c->regs[0], a1 = c->regs[1], a2 = c->regs[2];
    const char *x, *y;
    void *d;
    switch (id) {
    case 15U: x = gstr(c, a0); c->regs[0] = (x == NULL) ? 0U : (uint32_t)strlen(x); return;
    case 16U: x = gstr(c, a0); { uint32_t i = 0U; if (x != NULL) while (i < a2 && x[i] != '\0') i++; c->regs[0] = (x == NULL) ? 0U : i; } return;
    case 17U: x = gstr(c, a0); y = gstr(c, a1); c->regs[0] = (uint32_t)(int32_t)((x && y) ? strcmp(x, y) : -1); return;
    case 18U: x = gstr(c, a0); y = gstr(c, a1); c->regs[0] = (uint32_t)(int32_t)((x && y) ? strncmp(x, y, (size_t)a2) : -1); return;
    case 19U: case 34U: x = gstr(c, a1); if (x == NULL || gmem(c, a0, (uint32_t)strlen(x) + 1U, WR) == NULL) { serr(c, DE_INVAL); return; } memcpy(a32_guest_ptr(c, a0), x, strlen(x) + 1U); a32_vm32_copyout_invalidate_exec(c, a0, (uint32_t)strlen(x) + 1U); c->regs[0] = a0; return;
    case 20U: x = gstr(c, a1); d = gmem(c, a0, a2, WR); if (x == NULL || a2 == 0U || d == NULL) { serr(c, DE_INVAL); return; } { uint32_t i = 0U; while (i + 1U < a2 && x[i] != '\0') { ((char *)d)[i] = x[i]; i++; } ((char *)d)[i] = '\0'; } a32_vm32_copyout_invalidate_exec(c, a0, a2); c->regs[0] = a0; return;
    case 21U: { size_t lx, need; x = gstr(c, a0); y = gstr(c, a1); if (x == NULL || y == NULL) { serr(c, DE_INVAL); return; } lx = strlen(x); need = (uint32_t)(lx + strlen(y) + 1U); d = gmem(c, a0, need, WR); if (d == NULL) { serr(c, DE_INVAL); return; } memcpy((char *)d + lx, y, strlen(y) + 1U); a32_vm32_copyout_invalidate_exec(c, a0, need); c->regs[0] = a0; return; }
    case 22U: x = gstr(c, a0); { const char *f = (x == NULL) ? NULL : strchr(x, (int)(a1 & 0xFFU)); c->regs[0] = (f == NULL) ? 0U : (uint32_t)((uintptr_t)(f - x) + (uintptr_t)a0); } return;
    case 23U: x = gstr(c, a0); { const char *f = (x == NULL) ? NULL : strrchr(x, (int)(a1 & 0xFFU)); c->regs[0] = (f == NULL) ? 0U : (uint32_t)((uintptr_t)(f - x) + (uintptr_t)a0); } return;
    case 24U: x = gstr(c, a0); y = gstr(c, a1); { const char *f = (x && y) ? strstr(x, y) : NULL; c->regs[0] = (f == NULL) ? 0U : (uint32_t)((uintptr_t)(f - x) + (uintptr_t)a0); } return;
    case 25U: x = gstr(c, a0); { uint32_t p = (x == NULL) ? 0U : gmalloc(c, (uint32_t)strlen(x) + 1U); if (p == 0U) { serr(c, DE_NOMEM); return; } memcpy(a32_guest_ptr(c, p), x, strlen(x) + 1U); c->regs[0] = p; } return;
    case 26U: case 31U: case 27U: case 32U: { void *s = gmem(c, a1, a2, RD); d = gmem(c, a0, a2, WR); if (d == NULL || (a2 != 0U && s == NULL)) { serr(c, DE_INVAL); return; } if (id == 26U || id == 31U) memcpy(d, s, (size_t)a2); else memmove(d, s, (size_t)a2); a32_vm32_copyout_invalidate_exec(c, a0, a2); c->regs[0] = a0; return; }
    case 28U: case 33U: d = gmem(c, a0, a2, WR); if (d == NULL) { serr(c, DE_INVAL); return; } memset(d, (int)(a1 & 0xFFU), (size_t)a2); a32_vm32_copyout_invalidate_exec(c, a0, a2); c->regs[0] = a0; return;
    case 29U: { const void *p = gmem(c, a0, a2, RD), *q = gmem(c, a1, a2, RD); if (a2 != 0U && (p == NULL || q == NULL)) { serr(c, DE_INVAL); return; } c->regs[0] = (uint32_t)(int32_t)memcmp(p, q, (size_t)a2); return; }
    case 30U: d = gmem(c, a0, a1, WR); if (d == NULL) { serr(c, DE_INVAL); return; } memset(d, 0, (size_t)a1); a32_vm32_copyout_invalidate_exec(c, a0, a1); return;
    case 35U: x = gstr(c, a0); c->regs[0] = (uint32_t)(int32_t)((x == NULL) ? 0L : strtol(x, NULL, 10)); return;
    default: c->regs[0] = 0xFFFFFFFFU; return;
    }
}
typedef uint32_t (*fetch_fn)(a32_ctx_t *, uint32_t, unsigned);
static uint32_t fetch_rs(a32_ctx_t *c, uint32_t sp, unsigned i) { uint32_t v = 0U;
    if (i < 3U) return c->regs[1 + i];
    (void)guest_read(c, &v, sp + (i - 3U) * 4U, 4U); return v; }
static uint32_t fetch_rs2(a32_ctx_t *c, uint32_t sp, unsigned i) { uint32_t v = 0U;
    if (i < 2U) return c->regs[2 + i];
    (void)guest_read(c, &v, sp + (i - 2U) * 4U, 4U); return v; }
static uint32_t fetch_rs3(a32_ctx_t *c, uint32_t sp, unsigned i) { uint32_t v = 0U;
    if (i < 1U) return c->regs[3 + i];
    (void)guest_read(c, &v, sp + (i - 1U) * 4U, 4U); return v; }
static uint32_t fetch_ap(a32_ctx_t *c, uint32_t ap, unsigned i) { uint32_t v = 0U;
    (void)guest_read(c, &v, ap + i * 4U, 4U); return v; }
static int pfmt1(a32_ctx_t *c, char *out, size_t ocap, size_t *o, size_t *tot,
    fetch_fn f, uint32_t base, unsigned *ai, const char **fp)
{
    char hfmt[32], tmp[256];
    size_t hl = 0U;
    int width = -4097, prec = -1, is_ll = 0, w = 0, left = 0, zero = 0, pad = 0;
    char conv;
    const char *f0 = *fp;
    hfmt[hl++] = '%';
    while (*f0 == '-' || *f0 == '+' || *f0 == ' ' || *f0 == '0' || *f0 == '#') {
        if (hl + 1U < sizeof(hfmt))
            hfmt[hl++] = *f0;
        f0++;
    }
    if (*f0 == '*') {
        width = (int)(int32_t)f(c, base, (*ai)++); f0++;
        width = (width < -4096) ? -4096 : ((width > 4096) ? 4096 : width);
    } else {
        while (*f0 >= '0' && *f0 <= '9') {
            width = (width < 0) ? 0 : width;
            if (width < 4096) width = width * 10 + (*f0 - '0');
            f0++;
        }
    }
    if (*f0 == '.') {
        f0++;
        if (*f0 == '*') {
            prec = (int)(int32_t)f(c, base, (*ai)++); f0++;
            prec = (prec < 0) ? -1 : ((prec > 4096) ? 4096 : prec);
        } else {
            for (prec = 0; *f0 >= '0' && *f0 <= '9'; f0++)
                if (prec < 4096) prec = prec * 10 + (*f0 - '0');
        }
    }
    if (*f0 == 'l') { f0++; if (*f0 == 'l') { is_ll = 1; f0++; } }
    else if (*f0 == 'h' || *f0 == 'z' || *f0 == 't') { f0++; if (*f0 == 'h') f0++; }
    conv = *f0;
    if (conv == '\0')
        return -1;
    *fp = f0 + 1;
    if (conv != 's' && prec > 200) prec = 200;
    { size_t j = 1U, k = 1U; for (; j < hl; j++) { if (hfmt[j] == '-') left = 1; else if (hfmt[j] == '0') zero = 1; else hfmt[k++] = hfmt[j]; } hl = k; }
    if (width < 0 && width != -4097) { left = 1; width = -width; }
    if (hl + 16U < sizeof(hfmt)) {
        if (prec >= 0)
            hl += (size_t)snprintf(hfmt + hl, sizeof(hfmt) - hl, ".%d", prec);
        if (is_ll) { hfmt[hl++] = 'l'; hfmt[hl++] = 'l'; }
        hfmt[hl++] = conv; hfmt[hl] = '\0';
    } else { hfmt[1] = conv; hfmt[2] = '\0'; }
    if (conv == 's') {
        const char *s = gstr(c, f(c, base, (*ai)++));
        size_t sl, pn, k2; int pd = 0;
        if (s == NULL) s = "(null)";
        sl = strlen(s); pn = sl;
        if (prec >= 0 && (size_t)prec < pn) pn = (size_t)prec;
        if (width > (int)pn) pd = width - (int)pn;
        *tot += pn + (size_t)pd;
        if (!left) for (k2 = 0U; k2 < (size_t)pd && *o + 1U < ocap; k2++) out[(*o)++] = ' ';
        for (k2 = 0U; k2 < pn; k2++) { if (*o + 1U < ocap) out[(*o)++] = s[k2]; }
        if (left) for (k2 = 0U; k2 < (size_t)pd && *o + 1U < ocap; k2++) out[(*o)++] = ' ';
        return 0;
    } else if (conv == 'c') {
        w = snprintf(tmp, sizeof(tmp), hfmt, (int)f(c, base, (*ai)++));
    } else if (conv == 'p') {
        w = snprintf(tmp, sizeof(tmp), "0x%lx", (unsigned long)f(c, base, (*ai)++));
    } else if (is_ll) {
        uint32_t lo = f(c, base, (*ai)++), hi = f(c, base, (*ai)++);
        unsigned long long u = (((unsigned long long)hi) << 32) | lo;
        w = (conv == 'd' || conv == 'i') ? snprintf(tmp, sizeof(tmp), hfmt, (long long)u) : snprintf(tmp, sizeof(tmp), hfmt, u);
    } else if (conv == 'd' || conv == 'i') {
        w = snprintf(tmp, sizeof(tmp), hfmt, (int)f(c, base, (*ai)++));
    } else if (conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o') {
        w = snprintf(tmp, sizeof(tmp), hfmt, (unsigned)f(c, base, (*ai)++));
    } else {
        w = snprintf(tmp, sizeof(tmp), "%%(%c)", conv);
    }
    if (w < 0) w = 0;
    if ((size_t)w >= sizeof(tmp)) w = (int)(sizeof(tmp) - 1U);
    pad = (width > w) ? (width - w) : 0; *tot += (size_t)(w + pad);
    if (!left && pad > 0 && zero && prec < 0 && (conv == 'd' || conv == 'i' || conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o' || conv == 'p')) {
        size_t pre = 0U, k = 0U;
        if (w > 0 && (tmp[0] == '-' || tmp[0] == '+' || tmp[0] == ' ')) pre = 1U;
        if ((size_t)w > pre + 1U && tmp[pre] == '0' && (tmp[pre + 1U] == 'x' || tmp[pre + 1U] == 'X')) pre += 2U;
        for (k = 0U; k < pre && *o + 1U < ocap; k++) out[(*o)++] = tmp[k];
        for (k = 0U; k < (size_t)pad && *o + 1U < ocap; k++) out[(*o)++] = '0';
        for (k = pre; k < (size_t)w && *o + 1U < ocap; k++) out[(*o)++] = tmp[k];
    } else {
        size_t k = 0U;
        if (!left) for (k = 0U; k < (size_t)pad && *o + 1U < ocap; k++) out[(*o)++] = ' ';
        for (k = 0U; k < (size_t)w && *o + 1U < ocap; k++) out[(*o)++] = tmp[k];
        if (left) for (k = 0U; k < (size_t)pad && *o + 1U < ocap; k++) out[(*o)++] = ' ';
    }
    return 0;
}
static int pfmt(a32_ctx_t *c, char *out, size_t ocap, const char *fmt,
    fetch_fn fetch, uint32_t base, unsigned start)
{
    size_t o = 0U, total = 0U;
    unsigned ai = start;
    while (*fmt != '\0') {
        if (*fmt != '%') {
            if (o + 1U < ocap)
                out[o] = *fmt;
            o++;
            total++;
            fmt++;
            continue;
        }
        fmt++;
        if (*fmt == '%') {
            if (o + 1U < ocap)
                out[o] = '%';
            o++;
            total++;
            fmt++;
            continue;
        }
        if (pfmt1(c, out, ocap, &o, &total, fetch, base, &ai, &fmt) != 0)
            break;
    }
    if (ocap > 0U)
        out[(o < ocap) ? o : ocap - 1U] = '\0';
    return (int)total;
}
/* Synthetic FILE*: 0xFFFF5600/680/700 are fds 0..2; fopen VAs at 0xFFFF5800+i*0x40. */
#define FOPEN_N 32U
static int g_fopen_fd[FOPEN_N];
static bool g_fopen_used[FOPEN_N];
static pthread_mutex_t g_stdio_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_sigstub_mu = PTHREAD_MUTEX_INITIALIZER;
static int file_fd_of(uint32_t fp)
{
    uint32_t i; int r = -1;
    if (fp == 0xFFFF5600U) return 0;
    if (fp == 0xFFFF5680U) return 1;
    if (fp == 0xFFFF5700U) return 2;
    (void)pthread_mutex_lock(&g_stdio_mu);
    for (i = 0U; i < FOPEN_N; i++)
        if (g_fopen_used[i] && fp == 0xFFFF5800U + i * 0x40U) { r = g_fopen_fd[i]; break; }
    (void)pthread_mutex_unlock(&g_stdio_mu);
    return r;
}
static void ios_bye(uint32_t status) { fflush(NULL); _exit((int)status); for (;;) { } }
/* qsort insertion sort; guest compar runs with lr = QSORT_RESUME stub. */
static void qsort_step(a32_ctx_t *c, gcall_t *g)
{
    uint32_t base = g->a, n = g->b, sz = g->c, cmp = g->d, i = g->e, j = g->f;
    if (sz == 0U || n < 2U)
        goto qdone;
    while (i < n) {
        if (j == 0U) j = i;
        if (j > 0U) {
            memcpy(g->saved, c->regs, sizeof(g->saved));
            g->saved_cpsr = c->cpsr; g->active = true; g->kind = 2U; g->e = i; g->f = j;
            c->regs[0] = base + (j - 1U) * sz;
            c->regs[1] = base + j * sz;
            c->regs[14] = (A32_IOS_STUB_BASE_VA + QSORT_RESUME_ID * 8U) | 1U;
            a32_set_pc_interwork(c, cmp);
            return;
        }
        i++;
    }
qdone:
    if (g->b > 1U && g->c != 0U && a32_jit_range_executable(c, g->a, g->b * g->c))
        a32_vm32_copyout_invalidate_exec(c, g->a, g->b * g->c);
    c->regs[0] = 0U; free(g->tmp); g->tmp = NULL; g->active = false; }
static void qsort_resume(a32_ctx_t *c, gcall_t *g)
{
    int32_t r = (int32_t)c->regs[0];
    uint32_t base = g->a, sz = g->c, i = g->e, j = g->f;
    uint8_t *tmp = (uint8_t *)g->tmp;
    memcpy(c->regs, g->saved, sizeof(g->saved)); c->cpsr = g->saved_cpsr;
    if (r > 0 && j > 0U) {
        void *p1 = a32_guest_ptr(c, base + (j - 1U) * sz);
        void *p2 = a32_guest_ptr(c, base + j * sz);
        memcpy(tmp, p1, (size_t)sz); memcpy(p1, p2, (size_t)sz); memcpy(p2, tmp, (size_t)sz);
        g->e = i; g->f = j - 1U;
    } else { g->e = i + 1U; g->f = 0U; }
    qsort_step(c, g);
}
/* stdio/file stubs (ids 39..47) on raw host fds. */
static void stub_file(a32_ctx_t *c, uint32_t id)
{
    uint32_t a0 = c->regs[0], a1 = c->regs[1], a2 = c->regs[2];
    if (id == 39U) {
        const char *s = gstr(c, a0);
        size_t n = (s == NULL) ? 0U : strlen(s);
        if (s != NULL)
            (void)write(1, s, n);
        (void)write(1, "\n", 1U);
        c->regs[0] = 0U;
        return;
    }
    if (id == 40U) {
        char ch = (char)(a0 & 0xFFU);
        (void)write(1, &ch, 1U);
        c->regs[0] = a0 & 0xFFU;
        return;
    }
    if (id == 41U || id == 42U) {
        int fd = file_fd_of((id == 41U) ? a1 : a0);
        if (id == 41U) {
            const char *s = gstr(c, a0);
            size_t n = (s == NULL) ? 0U : strlen(s);
            if (fd < 0 || s == NULL) {
                serr(c, 9);
                return;
            }
            c->regs[0] = (write(fd, s, n) < 0) ? 0xFFFFFFFFU : 0U;
            if (c->regs[0] == 0xFFFFFFFFU)
                serr(c, errno);
        } else {
            char ch = (char)(a1 & 0xFFU);
            if (fd < 0 || write(fd, &ch, 1U) != 1) {
                serr(c, (fd < 0) ? 9 : errno);
                return;
            }
            c->regs[0] = (uint32_t)(unsigned char)ch;
        }
        return;
    }
    if (id == 43U || id == 44U) {
        uint32_t n = a1 * a2, done = 0U;
        int fd = file_fd_of(c->regs[3]);
        void *p = (n == 0U) ? NULL : gmem(c, a0, n, (id == 43U) ? RD : WR);
        if (fd < 0 || (n != 0U && p == NULL)) {
            serr(c, 9);
            return;
        }
        if (id == 43U) {
            ssize_t w = write(fd, p, (size_t)n);
            done = (w < 0) ? 0U : (uint32_t)w;
        } else {
            ssize_t r = read(fd, p, (size_t)n);
            done = (r < 0) ? 0U : (uint32_t)r;
            if (r > 0)
                a32_vm32_copyout_invalidate_exec(c, a0, (uint32_t)r);
        }
        c->regs[0] = (a1 == 0U) ? 0U : done / a1;
        return;
    }
    if (id == 45U) {
        c->regs[0] = 0U;
        return;
    }
    if (id == 46U) {
        const char *pa = gstr(c, a0), *mo = gstr(c, a1);
        char rb[1024];
        int f = -1, fd;
        uint32_t i;
        if (pa == NULL || mo == NULL || mo[0] == '\0') {
            serr(c, DE_INVAL);
            return;
        }
        if (mo[0] == 'r')
            f = (mo[1] == '+') ? O_RDWR : O_RDONLY;
        else if (mo[0] == 'w')
            f = ((mo[1] == '+') ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC;
        else if (mo[0] == 'a')
            f = ((mo[1] == '+') ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND;
        if (f < 0 || a32_resolve_guest_path(c, pa, rb, sizeof(rb)) != 0) {
            serr(c, (f < 0) ? DE_INVAL : 2);
            return;
        }
        fd = open(rb, f, 0644);
        if (fd < 0) {
            serr(c, errno);
            return;
        }
        (void)pthread_mutex_lock(&g_stdio_mu);
        for (i = 0; i < FOPEN_N; i++) {
            if (!g_fopen_used[i]) {
                g_fopen_used[i] = true;
                g_fopen_fd[i] = fd;
                c->regs[0] = 0xFFFF5800U + (uint32_t)i * 0x40U;
                (void)pthread_mutex_unlock(&g_stdio_mu); return;
            }
        }
        (void)pthread_mutex_unlock(&g_stdio_mu);
        (void)close(fd);
        serr(c, DE_NOMEM);
        return;
    }
    {
        int fd = file_fd_of(a0);
        uint32_t i;
        if (fd < 0 || fd > 2) {
            (void)pthread_mutex_lock(&g_stdio_mu);
            for (i = 0; i < FOPEN_N; i++) {
                if (g_fopen_used[i] && a0 == 0xFFFF5800U + (uint32_t)i * 0x40U)
                    g_fopen_used[i] = false;
            }
            (void)pthread_mutex_unlock(&g_stdio_mu);
            if (fd >= 0)
                (void)close(fd);
        }
        c->regs[0] = (fd < 0) ? 0xFFFFFFFFU : 0U;
    }
}
static void sn_direct(a32_ctx_t *c, const char *fmt, uint32_t dst, uint32_t cap,
    fetch_fn f, uint32_t base)
{
    char zc = '\0';
    int n;
    if (cap == 0U) {
        n = pfmt(c, &zc, 0U, fmt, f, base, 0U);
    } else {
        char *d;
        uint32_t wn;
        if (!a32_guest_range_ok(c, dst, cap, (uint32_t)WR)) { serr(c, DE_INVAL); return; }
        d = (char *)a32_guest_ptr(c, dst);
        n = pfmt(c, d, (size_t)cap, fmt, f, base, 0U);
        wn = (n < 0) ? 0U : (uint32_t)n;
        a32_vm32_copyout_invalidate_exec(c, dst, (wn + 1U <= cap) ? wn + 1U : cap);
    }
    c->regs[0] = (uint32_t)n;
}
static void stub_printf(a32_ctx_t *c, uint32_t id)
{
    char buf[2048];
    const char *fmt;
    int n;
    if (id == 48U) {
        fmt = gstr(c, c->regs[0]);
        if (fmt == NULL) {
            serr(c, DE_INVAL);
            return;
        }
        n = pfmt(c, buf, sizeof(buf), fmt, fetch_rs, c->regs[13], 0U);
        if (n > 0)
            (void)write(1, buf, (size_t)((n < 2048) ? n : 2047));
        c->regs[0] = (uint32_t)n;
        return;
    }
    if (id == 49U) {
        int fd = file_fd_of(c->regs[0]);
        fmt = gstr(c, c->regs[1]);
        if (fd < 0 || fmt == NULL) {
            serr(c, DE_INVAL);
            return;
        }
        n = pfmt(c, buf, sizeof(buf), fmt, fetch_rs2, c->regs[13], 0U);
        if (n > 0)
            (void)write(fd, buf, (size_t)((n < 2048) ? n : 2047));
        c->regs[0] = (uint32_t)n;
        return;
    }
    if (id == 50U) {
        fmt = gstr(c, c->regs[2]);
        if (fmt == NULL) { serr(c, DE_INVAL); return; }
        sn_direct(c, fmt, c->regs[0], c->regs[1], fetch_rs3, c->regs[13]);
        return;
    }
    if (id == 51U) {
        uint32_t sp = c->regs[13], fva = 0U;
        (void)guest_read(c, &fva, sp, 4U);
        fmt = gstr(c, fva);
        if (fmt == NULL) { serr(c, DE_INVAL); return; }
        sn_direct(c, fmt, c->regs[0], c->regs[1], fetch_ap, sp + 4U);
        return;
    }
    {
        fmt = gstr(c, c->regs[2]);
        if (fmt == NULL) { serr(c, DE_INVAL); return; }
        sn_direct(c, fmt, c->regs[0], c->regs[1], fetch_ap, c->regs[3]);
    }
}
/* 53..87 POSIX/Mach/signal + svc #0x81 dispatch (compact). */
static int xl_ofl(uint32_t g) { int h = (int)(g & 3U);
    if (g & 0x4U) { h |= O_NONBLOCK; } if (g & 0x8U) { h |= O_APPEND; }
    if (g & 0x200U) { h |= O_CREAT; } if (g & 0x400U) { h |= O_TRUNC; }
    if (g & 0x800U) { h |= O_EXCL; } if (g & 0x100U) { h |= O_NOFOLLOW; }
#ifdef O_DIRECTORY
    if (g & 0x100000U) h |= O_DIRECTORY;
#endif
#ifdef O_CLOEXEC
    if (g & 0x1000000U) h |= O_CLOEXEC;
#endif
    return h; }
static uint32_t sld(a32_ctx_t *c, uint32_t o) { uint32_t v = 0U, va = c->regs[13] + o;
    if (va >= c->regs[13] && a32_guest_range_ok(c, va, 4U, (uint32_t)RD)) memcpy(&v, a32_guest_ptr(c, va), 4);
    return v; }
static void sigerr(a32_ctx_t *c, int r) { int32_t e = (int32_t)(r < 0 ? -r : r); (void)guest_write(c, c->ios_errno_va, &e, 4U); c->regs[0] = 0xFFFFFFFFU; }
static void fst64(const struct stat *h, ios_stat64_t *o) { memset(o, 0, sizeof(*o));
    o->st_dev = (int32_t)h->st_dev; o->st_mode = (uint16_t)h->st_mode;
    o->st_nlink = (uint16_t)h->st_nlink; o->st_ino = (uint64_t)h->st_ino;
    o->st_uid = (uint32_t)h->st_uid; o->st_gid = (uint32_t)h->st_gid;
    o->st_rdev = (int32_t)h->st_rdev;
#if defined(__APPLE__)
    o->st_atime_sec = h->st_atimespec.tv_sec; o->st_atime_nsec = (int32_t)h->st_atimespec.tv_nsec;
    o->st_mtime_sec = h->st_mtimespec.tv_sec; o->st_mtime_nsec = (int32_t)h->st_mtimespec.tv_nsec;
    o->st_ctime_sec = h->st_ctimespec.tv_sec; o->st_ctime_nsec = (int32_t)h->st_ctimespec.tv_nsec;
    o->st_btime_sec = h->st_birthtimespec.tv_sec; o->st_btime_nsec = (int32_t)h->st_birthtimespec.tv_nsec;
#else
    o->st_atime_sec = (int32_t)h->st_atim.tv_sec; o->st_atime_nsec = (int32_t)h->st_atim.tv_nsec;
    o->st_mtime_sec = (int32_t)h->st_mtim.tv_sec; o->st_mtime_nsec = (int32_t)h->st_mtim.tv_nsec;
    o->st_ctime_sec = (int32_t)h->st_ctim.tv_sec; o->st_ctime_nsec = (int32_t)h->st_ctim.tv_nsec;
#endif
    o->st_size = (int64_t)h->st_size; o->st_blocks = (int64_t)h->st_blocks;
    o->st_blksize = (int32_t)h->st_blksize; }
static void dostat(a32_ctx_t *c, uint32_t pa, uint32_t bv, bool nf, bool fd_) { struct stat h;
    ios_stat64_t o; int r;
    if (!a32_guest_range_ok(c, bv, 108U, (uint32_t)WR)) { serr(c, DE_INVAL); return; }
    if (fd_) r = fstat((int)(int32_t)c->regs[0], &h);
    else { const char *g = gstr(c, pa); char hb[1024];
        if (g == NULL || a32_resolve_guest_path(c, g, hb, sizeof(hb)) != 0) { serr(c, 2); return; }
        r = nf ? lstat(hb, &h) : stat(hb, &h); }
    if (r != 0) { serr(c, errno); return; }
    fst64(&h, &o); memcpy(a32_guest_ptr(c, bv), &o, sizeof(o)); a32_vm32_copyout_invalidate_exec(c, bv,
        (uint32_t)sizeof(o)); c->regs[0] = 0U; }
static void doposix(a32_ctx_t *c, uint32_t id) { uint32_t r0 = c->regs[0], r1 = c->regs[1];
    uint32_t r2 = c->regs[2], r3 = c->regs[3];
    if (id == 53U) { const char *g = gstr(c, r0); char h[1024]; int f;
        if (g == NULL || a32_resolve_guest_path(c, g, h, sizeof(h)) != 0) { serr(c, 2); return; }
        f = open(h, xl_ofl(r1), (mode_t)r2); if (f < 0) serr(c, errno); else c->regs[0] = (uint32_t)f;
    } else if (id == 54U) { a32_ios_kq_close((int)c->regs[0]); if (close((int)(int32_t)r0) != 0) serr(c, errno); else c->regs[0] = 0U;
    } else if (id == 55U) { const char *g = gstr(c, r0); char h[1024];
        if (g == NULL || a32_resolve_guest_path(c, g, h, sizeof(h)) != 0) { serr(c, 2); return; }
        if (unlink(h) != 0) serr(c, errno); else c->regs[0] = 0U;
    } else if (id == 56U) { ssize_t n;
        if (r2 == 0U) { c->regs[0] = 0U; return; }
        if (!a32_guest_range_ok(c, r1, r2, (uint32_t)WR)) { serr(c, DE_INVAL); return; }
        n = read((int)(int32_t)r0, a32_guest_ptr(c, r1), (size_t)r2);
        if (n < 0) { serr(c, errno); return; }
        a32_vm32_copyout_invalidate_exec(c, r1, (uint32_t)n); c->regs[0] = (uint32_t)n;
    } else if (id == 57U) { ssize_t n;
        if (r2 == 0U) { c->regs[0] = 0U; return; }
        if (!a32_guest_range_ok(c, r1, r2, (uint32_t)RD)) { serr(c, DE_INVAL); return; }
        n = write((int)(int32_t)r0, a32_guest_ptr(c, r1), (size_t)r2);
        if (n < 0) serr(c, errno); else c->regs[0] = (uint32_t)n;
    } else if (id == 58U) { uint64_t u = (uint64_t)r1 | ((uint64_t)r2 << 32);
        off_t q = lseek((int)(int32_t)r0, (off_t)u, (int)(int32_t)r3);
        if (q < 0) { serr(c, errno); return; }
        c->regs[0] = (uint32_t)(uint64_t)q; c->regs[1] = (uint32_t)((uint64_t)q >> 32);
    } else if (id == 59U || id == 60U) dostat(c, r0, r1, false, false);
    else if (id == 61U) dostat(c, 0U, r1, false, true);
    else if (id == 62U) dostat(c, r0, r1, true, false);
    else if (id == 63U) { uint32_t L = r1, F = r3, lf = F & 0x13U, fd, pg, res; uint64_t of;
        if (L == 0U || L > 0x80000000U) { serr(c, DE_INVAL); return; }
        if (F & 0x1000U) { lf |= 0x20U; } fd = sld(c, 0U); if (lf & 0x20U) { fd = 0xFFFFFFFFU; }
        of = (uint64_t)sld(c, 4U) | ((uint64_t)sld(c, 8U) << 32);
        if (of & 4095ULL) { serr(c, DE_INVAL); return; }
        pg = (uint32_t)(of >> 12); res = a32_vm_mmap2(c, r0, L, r2 & 7U, lf, (int)(int32_t)fd, pg);
        if (res > 0xFFFFF000U) serr(c, a32_ios_host_to_darwin_errno(-(int)(int32_t)res));
        else c->regs[0] = res;
    } else if (id == 64U) { if (a32_vm_munmap(c, r0, r1)) serr(c, errno); else c->regs[0] = 0U;
    } else if (id == 65U) { if (a32_vm_mprotect(c, r0, r1, r2 & 7U)) serr(c, errno); else c->regs[0] = 0U;
    } else if (id <= 69U) { c->regs[0] = (id == 66U ? (uint32_t)getpid() : id == 67U ? (uint32_t)getppid() :
        id == 68U ? (uint32_t)getuid() : (uint32_t)geteuid());
    } else if (id == 70U) { struct timeval t; int32_t w[2];
        if (r0 && !a32_guest_range_ok(c, r0, 8U, (uint32_t)WR)) { serr(c, DE_INVAL); return; }
        if (gettimeofday(&t, NULL) != 0) { serr(c, errno); return; }
        if (r0) { w[0] = (int32_t)t.tv_sec; w[1] = (int32_t)t.tv_usec;
            memcpy(a32_guest_ptr(c, r0), w, 8); a32_vm32_copyout_invalidate_exec(c, r0, 8U); }
        c->regs[0] = 0U;
    } else if (id == 71U) { (void)usleep((useconds_t)r0); c->regs[0] = 0U;
    } else if (id == 72U) { struct timespec q, m; int32_t g[2], e = 4;
        if (!a32_guest_range_ok(c, r0, 8U, (uint32_t)RD)) { serr(c, DE_INVAL); return; }
        memcpy(g, a32_guest_ptr(c, r0), 8); q.tv_sec = (time_t)g[0]; q.tv_nsec = (long)g[1];
        m.tv_sec = 0; m.tv_nsec = 0;
        if (nanosleep(&q, &m) != 0) {
            if (errno != EINTR) { serr(c, errno); return; }
            if (r1 != 0U && a32_guest_range_ok(c, r1, 8U, (uint32_t)WR)) { int32_t o[2];
                o[0] = (int32_t)m.tv_sec; o[1] = (int32_t)m.tv_nsec;
                memcpy(a32_guest_ptr(c, r1), o, 8); a32_vm32_copyout_invalidate_exec(c, r1, 8U); }
            (void)guest_write(c, c->ios_errno_va, &e, 4U); c->regs[0] = 0xFFFFFFFFU; return;
        }
        if (r1 && a32_guest_range_ok(c, r1, 8U, (uint32_t)WR)) { int32_t o[2];
            o[0] = (int32_t)m.tv_sec; o[1] = (int32_t)m.tv_nsec;
            memcpy(a32_guest_ptr(c, r1), o, 8); a32_vm32_copyout_invalidate_exec(c, r1, 8U); }
        c->regs[0] = 0U;
    } else if (id == 73U) c->regs[0] = (uint32_t)sleep(r0);
    else if (id == 74U || id == 75U) { uint32_t a[8], oc; bool hd = false;
        if (id == 74U) {
            a[0] = r0; a[1] = r1; a[2] = r2; a[3] = r3;
            a[4] = sld(c, 0U); a[5] = sld(c, 4U); a[6] = sld(c, 8U); a[7] = sld(c, 12U);
        } else {
            const char *nm = gstr(c, r0);
            if (nm == NULL) { serr(c, DE_INVAL); return; }
            a[0] = r0; a[1] = (uint32_t)strlen(nm); a[2] = r1; a[3] = r2;
            a[4] = r3; a[5] = sld(c, 0U); a[6] = 0U; a[7] = 0U;
        }
        oc = c->cpsr;
        (void)a32_ios_dispatch_bsd_proc_net(c, id == 74U ? 202U : 274U, a, &hd);
        if ((c->cpsr >> 29U) & 1U) { sigerr(c, (int)(int32_t)c->regs[0]); }
        c->cpsr = oc;
    } else if (id == 76U) { int f; uint8_t b[256]; ssize_t n;
        if (r1 > 256U) { serr(c, 5); return; }
        if (r1 == 0U) { c->regs[0] = 0U; return; }
        if (!a32_guest_range_ok(c, r0, r1, (uint32_t)WR)) { serr(c, DE_INVAL); return; }
        f = open("/dev/urandom", O_RDONLY); if (f < 0) { serr(c, errno); return; }
        n = read(f, b, (size_t)r1); (void)close(f);
        if (n != (ssize_t)r1) { serr(c, 5); return; }
        memcpy(a32_guest_ptr(c, r0), b, (size_t)r1);
        a32_vm32_copyout_invalidate_exec(c, r0, r1); c->regs[0] = 0U;
    } else if (id == 77U) { const char *nm = gstr(c, r0); uint32_t ev = 0U, i; size_t nl;
        if (nm == NULL) { serr(c, DE_INVAL); return; }
        if (!a32_guest_range_ok(c, 0xFFFF5520U, 4U, (uint32_t)RD)) { c->regs[0] = 0U; return; }
        memcpy(&ev, a32_guest_ptr(c, 0xFFFF5520U), 4); nl = strlen(nm);
        for (i = 0U; i < 512U; i++) { uint32_t va = 0U; const char *e;
            if (!a32_guest_range_ok(c, ev + i * 4U, 4U, (uint32_t)RD)) break;
            memcpy(&va, a32_guest_ptr(c, ev + i * 4U), 4); if (va == 0U) break;
            e = gstr(c, va);
            if (e != NULL && strncmp(e, nm, nl) == 0 && e[nl] == '=') { c->regs[0] = va + (uint32_t)nl + 1U; return; } }
        c->regs[0] = 0U;
    } else if (id == 78U) { struct timespec t; uint64_t ns;
        (void)clock_gettime(CLOCK_MONOTONIC, &t); ns = (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
        c->regs[0] = (uint32_t)ns; c->regs[1] = (uint32_t)(ns >> 32);
    } else if (id == 79U) { uint32_t w[2] = { 1U, 1U };
        if (!a32_guest_range_ok(c, r0, 8U, (uint32_t)WR)) { serr(c, DE_INVAL); return; }
        memcpy(a32_guest_ptr(c, r0), w, 8); a32_vm32_copyout_invalidate_exec(c, r0, 8U); c->regs[0] = 0U;
    } else if (id <= 81U) c->regs[0] = (id == 80U) ? 0x103U : 0x107U;
    else if (id == 82U) c->regs[0] = a32_ios_mach_thread_self_port(c);
    else if (id == 83U) {
        if (r1 && !a32_guest_range_ok(c, r0, r1, (uint32_t)RD)) { serr(c, DE_INVAL); return; }
        a32_vm32_copyout_invalidate_exec(c, r0, (size_t)r1); c->regs[0] = 0U;
    } else if (id == 84U) { uint32_t u[3], k[4], nv = 0U; int rc;
        (void)pthread_mutex_lock(&g_sigstub_mu);
        if (r1) { if (!a32_guest_range_ok(c, r1, 12U, (uint32_t)RD)) { (void)pthread_mutex_unlock(&g_sigstub_mu); serr(c, DE_INVAL); return; }
            memcpy(u, a32_guest_ptr(c, r1), 12); k[0] = u[0]; k[1] = 0U; k[2] = u[1]; k[3] = u[2];
            nv = g_scratch_va(c); if (!nv || !guest_write(c, nv, k, 16U)) { (void)pthread_mutex_unlock(&g_sigstub_mu); serr(c, DE_NOMEM); return; } }
        rc = a32_ios_sys_sigaction(c, (int)(int32_t)r0, nv, r2);
        (void)pthread_mutex_unlock(&g_sigstub_mu);
        if (rc) sigerr(c, rc); else c->regs[0] = 0U;
    } else if (id == 85U) { int rc = a32_ios_sys_sigprocmask(c, (int)(int32_t)r0, r1, r2);
        if (rc) sigerr(c, rc); else c->regs[0] = 0U; a32_ios_signals_check_pending(c);
    } else if (id == 86U) { int rc = a32_ios_sys_kill(c, (int)(int32_t)r0, (int)(int32_t)r1, 1);
        if (rc) sigerr(c, rc); else c->regs[0] = 0U; a32_ios_signals_check_pending(c);
    } else if (id == 87U) { int rc = a32_ios_sys_pthread_kill(c, 0U, (int)(int32_t)r0);
        if (rc) sigerr(c, rc); else c->regs[0] = 0U; a32_ios_signals_check_pending(c);
    } else serr(c, DE_INVAL); }
int a32_ios_handle_stub_svc(a32_ctx_t *ctx) { uint32_t id, r0; bool hd = false, xh = false;
    if (ctx == NULL) return 0;
    id = ctx->regs[12]; r0 = ctx->regs[0];
    if (a32_ios_handle_stub_ext(ctx, id, &xh) == 0 && xh) return 0;
    if (id == 0xFAU) { a32_ios_exit_teardown(ctx, 134); return 0; }
    if (id == 0xFDU) { qsort_resume(ctx, gc_self()); return 0; } if (id <= 1U) { ctx->regs[0] = 0U; return 0; }
    if (id == 2U || id == 4U) { ios_bye(134U); return 0; }
    if (id == 6U) { ctx->exit_reason = 0U; ctx->regs[15] = A32_IOS_STUB_BASE_VA + 0xFAU * 8U; ctx->cpsr |= (uint32_t)A32_CPSR_T;
        (void)a32_ios_sys_pthread_kill(ctx, 0U, 6); (void)a32_ios_signals_check_pending(ctx);
        if (ctx->exit_reason == (uint32_t)A32_EXIT_BRANCH) return 0;
        a32_ios_exit_teardown(ctx, 134); return 0; }
    if (id == 3U) { ctx->regs[0] = ctx->ios_errno_va; return 0; }
    if (id <= 14U) {
        if (id == 10U) { uint32_t p = gmalloc(ctx, r0); if (!p) serr(ctx, DE_NOMEM); else ctx->regs[0] = p;
        } else if (id == 11U) { uint64_t n = (uint64_t)ctx->regs[0] * (uint64_t)ctx->regs[1]; uint32_t p;
            p = (n > 0x1000000ULL) ? 0U : gmalloc(ctx, (uint32_t)n); if (!p) serr(ctx, DE_NOMEM);
            else { memset(a32_guest_ptr(ctx, p), 0, (size_t)n); ctx->regs[0] = p; }
        } else if (id == 12U) { uint32_t p = grealloc(ctx, r0, ctx->regs[1]);
            if (!p && ctx->regs[1]) serr(ctx, DE_NOMEM); else ctx->regs[0] = p;
        } else if (id == 13U) { gfree(ctx, r0); ctx->regs[0] = 0U;
        } else { uint32_t mp = r0, al = ctx->regs[1], sz = ctx->regs[2], q, a;
            if (!(al >= 4U && (al & (al - 1U)) == 0U && al <= 0x100000U)) { serr(ctx, DE_INVAL); return 0; }
            if (!(sz <= ARENA_SZ - 64U - al)) { serr(ctx, DE_NOMEM); return 0; }
            if (al <= 16U) { q = gmalloc(ctx, sz ? sz : 16U); if (!q) { serr(ctx, DE_NOMEM); return 0; } a = q; }
            else { q = gmalloc(ctx, sz + al + 16U); if (!q) { serr(ctx, DE_NOMEM); return 0; }
                a = (q + 16U + al - 1U) & ~(al - 1U);
                if (a != q) { uint32_t fh[4]; fh[0] = 0xA110F00DU; fh[1] = sz; fh[2] = q; fh[3] = 0U;
                    memcpy(a32_guest_ptr(ctx, a - 16U), fh, 16U); } }
            if (!a32_guest_range_ok(ctx, mp, 4U, (uint32_t)WR)) { gfree(ctx, q); serr(ctx, DE_INVAL); return 0; }
            memcpy(a32_guest_ptr(ctx, mp), &a, 4); ctx->regs[0] = 0U; }
        return 0; }
    if (id <= 37U) { stub_strmem(ctx, id); return 0; }
    if (id == 38U) { gcall_t *g = gc_self(); uint32_t b = r0, n = ctx->regs[1];
        uint32_t sz = ctx->regs[2], cp = ctx->regs[3]; uint64_t t;
        if (!g) { serr(ctx, DE_NOMEM); return 0; }
        if (!sz || n < 2U) { ctx->regs[0] = 0U; return 0; }
        t = (uint64_t)n * (uint64_t)sz;
        if (t > 0x1000000ULL || !a32_guest_range_ok(ctx, b, (uint32_t)t, (uint32_t)(RD | WR))) {
            serr(ctx, DE_INVAL); return 0; }
        free(g->tmp); g->tmp = malloc((size_t)sz);
        if (!g->tmp) { serr(ctx, DE_NOMEM); return 0; }
        g->a = b; g->b = n; g->c = sz; g->d = cp; g->e = 0U; g->f = 0U;
        qsort_step(ctx, g); return 0; }
    if (id <= 47U) { stub_file(ctx, id); return 0; } if (id <= 52U) { stub_printf(ctx, id); return 0; }
    if (id <= 87U) { doposix(ctx, id); return 0; }
    (void)a32_ios_stub_pthread(ctx, id, &hd); return 0; }
