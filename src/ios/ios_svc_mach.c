/*
 * src/ios/ios_svc_mach.c — 32-bit iOS ARM machine traps & Mach traps
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
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
#include <sched.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define MACH_KERN_SUCCESS 0U
#define MACH_KERN_INVALID_ADDRESS 1U
#define MACH_KERN_PROTECTION_FAILURE 2U
#define MACH_KERN_NO_SPACE 3U
#define MACH_KERN_INVALID_ARGUMENT 4U
#define MACH_KERN_FAILURE 5U
#define MACH_KERN_RESOURCE_SHORTAGE 6U
#define MACH_KERN_ABORTED 14U
#define MACH_KERN_INVALID_NAME 15U
#define MACH_KERN_INVALID_TASK 16U
#define MACH_KERN_INVALID_RIGHT 17U
#define MACH_KERN_INVALID_VALUE 18U
#define MACH_KERN_TIMED_OUT 49U
#define MACH_KERN_TERMINATED 37U
#define MACH_SEND_MSG 1U
#define MACH_RCV_MSG 2U
#define MACH_RCV_TIMEOUT 0x100U
#define MACH_MSG_SUCCESS 0U
#define MACH_SEND_INVALID_DEST 0x10000003U
#define MACH_SEND_INVALID_DATA 0x10000002U
#define MACH_SEND_INVALID_SIZE 0x10000008U
#define MACH_SEND_NO_BUFFER 0x1000000dU
#define MACH_RCV_INVALID_NAME 0x10004002U
#define MACH_RCV_TIMED_OUT 0x10004003U
#define MACH_RCV_TOO_LARGE 0x10004004U
#define MACH_RCV_LARGE 0x00000400U
#define MACH_RCV_INTERRUPTED 0x10004007U
#define MACH_RCV_PORT_DIED 0x10004009U
#define MACH_PORT_TASK_SELF 0x103U
#define MACH_PORT_HOST_SELF 0x107U
#define MACH_PORT_BOOTSTRAP 0x10BU
#define MACH_PORT_THREAD_BASE 0x203U
#define MACH_PORT_CLOCK 0x303U
#define MACH_PORT_USER_BASE 0x1000U
#define MACH_PORT_COUNT 256U
#define MACH_QUEUE_DEPTH 16U
#define MACH_MSG_MAX 4096U
typedef enum { MACH_KIND_FREE = 0, MACH_KIND_TASK = 1, MACH_KIND_HOST = 2,
    MACH_KIND_THREAD = 3, MACH_KIND_BOOTSTRAP = 4, MACH_KIND_USER_QUEUE = 5,
    MACH_KIND_SEMAPHORE = 6, MACH_KIND_TIMER = 7, MACH_KIND_CLOCK = 8 } mach_kind_t;
typedef struct {
    bool used; uint32_t name; unsigned kind; uint32_t refs; int sem_count;
    uint32_t gen; uintptr_t owner_ctx;
    pthread_cond_t cond; bool cond_live; uint8_t *queue[MACH_QUEUE_DEPTH];
    uint32_t qsize[MACH_QUEUE_DEPTH]; unsigned qhead, qtail, qcount;
} a32_mach_slot_t;
static a32_mach_slot_t g_ports[MACH_PORT_COUNT];
static pthread_mutex_t g_mach_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_mach_init = false;
static uint32_t g_user_next = MACH_PORT_USER_BASE;
typedef struct {
    uint32_t bits, size, remote, local, voucher, id;
} mach_hdr_t;

static bool mach_r_ok(const a32_ctx_t *ctx, uint32_t va, uint32_t len)
{
    if (len == 0U) { return true; }
    return a32_guest_range_ok((a32_ctx_t *)ctx, va, len, (uint32_t)PROT_READ);
}
static bool mach_w_ok(const a32_ctx_t *ctx, uint32_t va, uint32_t len)
{
    if (len == 0U) { return true; }
    return a32_guest_range_ok((a32_ctx_t *)ctx, va, len, (uint32_t)PROT_WRITE);
}
static bool mach_rw_ok(const a32_ctx_t *ctx, uint32_t va, uint32_t len)
{
    if (len == 0U) { return true; }
    return a32_guest_range_ok((a32_ctx_t *)ctx, va, len,
        (uint32_t)(PROT_READ | PROT_WRITE));
}
static bool mach_read(const a32_ctx_t *ctx, void *dst, uint32_t va, uint32_t len)
{
    if (!mach_r_ok(ctx, va, len)) { return false; }
    if (len > 0U) { memcpy(dst, a32_guest_ptr(ctx, va), len); }
    return true;
}
static bool mach_write(a32_ctx_t *ctx, uint32_t va, const void *src, uint32_t len)
{
    if (!mach_w_ok(ctx, va, len)) { return false; }
    if (len > 0U) {
        memcpy(a32_guest_ptr(ctx, va), src, len);
        a32_vm32_copyout_invalidate_exec(ctx, va, (size_t)len);
    }
    return true;
}
static bool mach_write_u32(a32_ctx_t *ctx, uint32_t va, uint32_t v)
{
    return mach_write(ctx, va, &v, 4U);
}
static bool mach_sig_pending(const a32_ctx_t *ctx) { return (__atomic_load_n(&ctx->pending_signals, __ATOMIC_ACQUIRE) & ~ctx->sig_mask) != 0ULL; }
static void mach_step_ts(struct timespec *ts) { struct timespec n; (void)clock_gettime(CLOCK_REALTIME, &n); n.tv_nsec += 25000000L; if (n.tv_nsec >= 1000000000L) { n.tv_sec += 1; n.tv_nsec -= 1000000000L; } *ts = n; }
static void mach_cap_ts(struct timespec *ts, const struct timespec *dl) { if (ts->tv_sec > dl->tv_sec || (ts->tv_sec == dl->tv_sec && ts->tv_nsec > dl->tv_nsec)) { *ts = *dl; } }

static int mach_find_locked(uint32_t name)
{
    for (unsigned i = 0U; i < MACH_PORT_COUNT; i++) {
        if (g_ports[i].used && g_ports[i].name == name) {
            return (int)i;
        }
    }
    return -1;
}

static int mach_alloc_slot_locked(unsigned kind, uint32_t want_name,
    uint32_t *out_name)
{
    for (unsigned i = 0U; i < MACH_PORT_COUNT; i++) {
        if (!g_ports[i].used) {
            uint32_t nm = want_name;
            if (nm == 0U) {
                nm = g_user_next;
                g_user_next += 4U;
                if (g_user_next < MACH_PORT_USER_BASE) {
                    g_user_next = MACH_PORT_USER_BASE;
                }
            }
            g_ports[i].gen++; g_ports[i].owner_ctx = 0;
            g_ports[i].used = true;
            g_ports[i].name = nm;
            g_ports[i].kind = kind;
            g_ports[i].refs = 1U;
            g_ports[i].sem_count = 0;
            g_ports[i].qhead = 0U;
            g_ports[i].qtail = 0U;
            g_ports[i].qcount = 0U;
            for (unsigned q = 0U; q < MACH_QUEUE_DEPTH; q++) {
                g_ports[i].queue[q] = NULL;
                g_ports[i].qsize[q] = 0U;
            }
            if (!g_ports[i].cond_live) {
                if (pthread_cond_init(&g_ports[i].cond, NULL) != 0) {
                    g_ports[i].used = false;
                    return -1;
                }
                g_ports[i].cond_live = true;
            }
            *out_name = nm;
            return (int)i;
        }
    }
    return -1;
}

static void mach_drop_queue_locked(int idx)
{
    if (idx < 0) {
        return;
    }
    for (unsigned q = 0U; q < MACH_QUEUE_DEPTH; q++) {
        free(g_ports[idx].queue[q]);
        g_ports[idx].queue[q] = NULL;
        g_ports[idx].qsize[q] = 0U;
    }
    g_ports[idx].qhead = 0U;
    g_ports[idx].qtail = 0U;
    g_ports[idx].qcount = 0U;
}

static void mach_ensure_init(void)
{
    pthread_mutex_lock(&g_mach_lock);
    if (!g_mach_init) {
        uint32_t junk = 0U;
        for (unsigned i = 0U; i < MACH_PORT_COUNT; i++) {
            g_ports[i].used = false;
            g_ports[i].cond_live = false;
        }
        (void)mach_alloc_slot_locked(MACH_KIND_TASK, MACH_PORT_TASK_SELF,
            &junk);
        (void)mach_alloc_slot_locked(MACH_KIND_HOST, MACH_PORT_HOST_SELF,
            &junk);
        (void)mach_alloc_slot_locked(MACH_KIND_BOOTSTRAP,
            MACH_PORT_BOOTSTRAP, &junk);
        (void)mach_alloc_slot_locked(MACH_KIND_CLOCK, MACH_PORT_CLOCK,
            &junk);
        g_mach_init = true;
    }
    pthread_mutex_unlock(&g_mach_lock);
}

static void mach_ensure_thread_port(a32_ctx_t *ctx, uint32_t *out_name)
{
    uintptr_t me = (uintptr_t)ctx; uint32_t got = 0U, want = 0x203U + ((((uint32_t)get_host_tid(ctx)) & 0xFFU) << 2); int hit = -1, o;
    pthread_mutex_lock(&g_mach_lock);
    for (unsigned i = 0U; i < MACH_PORT_COUNT; i++) {
        if (g_ports[i].used && g_ports[i].kind == MACH_KIND_THREAD && g_ports[i].owner_ctx == me) { got = g_ports[i].name; hit = (int)i; break; }
    }
    if (hit < 0) {
        if ((o = mach_find_locked(want)) >= 0 && g_ports[o].used && g_ports[o].owner_ctx != 0U && g_ports[o].owner_ctx != me) { want = 0U; }
        if (mach_alloc_slot_locked(MACH_KIND_THREAD, want, &got) >= 0) { hit = mach_find_locked(got); if (hit >= 0) { g_ports[hit].owner_ctx = me; } }
        else { got = 0U; }
    }
    pthread_mutex_unlock(&g_mach_lock);
    if (out_name != NULL) { *out_name = got; }
}

uint32_t a32_ios_mach_thread_self_port(a32_ctx_t *ctx)
{
    uint32_t nm = 0U;
    if (ctx == NULL) { return 0U; }
    mach_ensure_init();
    mach_ensure_thread_port(ctx, &nm);
    return nm;
}

static void mach_trap_port_destruct(a32_ctx_t *ctx)
{
    uint32_t name = ctx->regs[1];
    int idx;
    pthread_mutex_lock(&g_mach_lock);
    idx = mach_find_locked(name);
    if (idx >= 0) { if (g_ports[idx].refs > 0U) { g_ports[idx].refs--; }
        if (g_ports[idx].refs == 0U) { mach_drop_queue_locked(idx); g_ports[idx].gen++; g_ports[idx].used = false; }
        pthread_cond_broadcast(&g_ports[idx].cond); }
    pthread_mutex_unlock(&g_mach_lock);
    ctx->regs[0] = MACH_KERN_SUCCESS;
}

/* Caller must hold g_mach_lock. Enqueues a copy; returns -2 without
 * storing anything when the ring is full (MACH_SEND_NO_BUFFER). */
static int mach_enqueue_locked(int idx, const uint8_t *msg, uint32_t len)
{
    uint8_t *copy = NULL;
    if (idx < 0 || msg == NULL || len == 0U || len > MACH_MSG_MAX) {
        return -1;
    }
    if (g_ports[idx].qcount >= MACH_QUEUE_DEPTH) {
        return -2;
    }
    copy = (uint8_t *)malloc(len);
    if (copy == NULL) {
        return -1;
    }
    memcpy(copy, msg, len);
    g_ports[idx].queue[g_ports[idx].qtail] = copy;
    g_ports[idx].qsize[g_ports[idx].qtail] = len;
    g_ports[idx].qtail = (g_ports[idx].qtail + 1U) % MACH_QUEUE_DEPTH;
    g_ports[idx].qcount++;
    pthread_cond_broadcast(&g_ports[idx].cond);
    return 0;
}

static bool mach_is_server_port(uint32_t name)
{
    if (name == MACH_PORT_TASK_SELF || name == MACH_PORT_HOST_SELF ||
        name == MACH_PORT_BOOTSTRAP || name == MACH_PORT_CLOCK) { return true; }
    for (unsigned i = 0U; i < MACH_PORT_COUNT; i++) {
        if (g_ports[i].used && g_ports[i].kind == MACH_KIND_THREAD && g_ports[i].name == name) { return true; }
    }
    return false;
}

/* ARM machine traps 0x80000000..0x80000003. */
static void mach_arm_trap(a32_ctx_t *ctx, uint32_t trap)
{
    uint32_t start;
    uint32_t len;
    if (trap == 0x80000000U) {
        start = ctx->regs[0];
        len = ctx->regs[1];
        if (len > 0U) {
            uint64_t e64 = (uint64_t)start + (uint64_t)len;
            uint32_t inv_end = (e64 > 0xFFFFFFFFULL) ? 0xFFFFFFFFU :
                (uint32_t)e64;
            (void)a32_jit_invalidate_range(ctx, start, inv_end);
            if (a32_guest_range_ok(ctx, start, len,
                    (uint32_t)PROT_READ)) {
                char *b = (char *)a32_guest_ptr(ctx, start);
                __builtin___clear_cache(b, b + len);
            }
        }
        ctx->regs[0] = 0U;
    } else if (trap == 0x80000001U) {
        ctx->tls_tp = ctx->regs[0];
        ctx->regs[0] = 0U;
    } else if (trap == 0x80000002U) {
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        ctx->regs[0] = 0U;
    } else {
        ctx->regs[0] = ctx->tls_tp;
    }
}

static uint32_t mach_errno_to_kern(int err)
{
    if (err == ENOMEM || err == ENOSPC || err == EEXIST) { return MACH_KERN_NO_SPACE; }
    if (err == EINVAL) { return MACH_KERN_INVALID_ARGUMENT; }
    if (err == EACCES) { return MACH_KERN_PROTECTION_FAILURE; }
    return MACH_KERN_FAILURE;
}
static uint32_t mach_round_page_u64(uint64_t v)
{
    uint64_t r = (v + 0xFFFULL) & ~0xFFFULL;
    if (r > 0xFFFFFFFFULL) { r = 0x100000000ULL; }
    return (uint32_t)r;
}
static void mach_trap_vm_allocate(a32_ctx_t *ctx)
{
    uint32_t addr_va = ctx->regs[1];
    uint32_t size_lo = ctx->regs[2];
    uint32_t size_hi = ctx->regs[3];
    uint32_t flags = ctx->regs[4];
    uint64_t size64;
    uint64_t hint = 0ULL;
    uint32_t hint32 = 0U;
    uint32_t size32;
    uint32_t va;
    uint32_t mflags = 0x22U;
    bool anywhere;
    if (!mach_rw_ok(ctx, addr_va, 8U) ||
        !mach_read(ctx, &hint, addr_va, 8U)) {
        ctx->regs[0] = MACH_KERN_INVALID_ADDRESS;
        return;
    }
    size64 = ((uint64_t)size_hi << 32) | (uint64_t)size_lo;
    if (size64 == 0ULL) { ctx->regs[0] = MACH_KERN_SUCCESS; return; }
    if (size64 > 0xF0000000ULL) { ctx->regs[0] = MACH_KERN_NO_SPACE; return; }
    anywhere = (flags & 0x0001U) != 0U;
    if (!anywhere) {
        if (hint > 0xFFFFFFFFULL) { ctx->regs[0] = MACH_KERN_INVALID_ADDRESS; return; }
        if (hint >= (uint64_t)A32_KUSER_BASE_32 ||
            size64 > (uint64_t)A32_KUSER_BASE_32 - hint) {
            ctx->regs[0] = MACH_KERN_NO_SPACE; return;
        }
        hint32 = ((uint32_t)hint) & ~0xFFFU;
        if (hint32 < 0x1000U) { ctx->regs[0] = MACH_KERN_NO_SPACE; return; }
        size32 = mach_round_page_u64(hint + size64) - hint32;
        mflags |= ((flags & 0x4000U) != 0U) ? 0x10U : 0x100000U;
    } else {
        size32 = mach_round_page_u64(size64);
    }
    if (size32 == 0U) { ctx->regs[0] = MACH_KERN_NO_SPACE; return; }
    va = a32_vm_mmap2(ctx, anywhere ? 0U : hint32, size32,
        (uint32_t)(PROT_READ | PROT_WRITE), mflags, -1, 0U);
    if (va > 0xFFFFF000U) {
        int e = -(int32_t)va;
        ctx->regs[0] = mach_errno_to_kern(e);
        return;
    }
    {
        uint64_t out = (uint64_t)va;
        if (!mach_write(ctx, addr_va, &out, 8U)) {
            ctx->regs[0] = MACH_KERN_INVALID_ADDRESS;
            return;
        }
        a32_vm32_copyout_invalidate_exec(ctx, addr_va, 8U);
    }
    ctx->regs[0] = MACH_KERN_SUCCESS;
}

static void mach_trap_vm_deallocate(a32_ctx_t *ctx)
{
    uint64_t addr64 = ((uint64_t)ctx->regs[2] << 32) | ctx->regs[1];
    uint32_t size_hi = ctx->regs[4];
    uint64_t size64;
    uint32_t a_page;
    uint32_t len_page;
    uint64_t e64;
    size64 = ((uint64_t)size_hi << 32) | (uint64_t)ctx->regs[3];
    if (size64 == 0ULL) { ctx->regs[0] = MACH_KERN_SUCCESS; return; }
    if (addr64 > 0xFFFFFFFFULL) { ctx->regs[0] = MACH_KERN_INVALID_ADDRESS; return; }
    if (addr64 >= (uint64_t)A32_KUSER_BASE_32) { ctx->regs[0] = MACH_KERN_INVALID_ADDRESS; return; }
    a_page = ((uint32_t)addr64) & ~0xFFFU;
    e64 = (size64 > (uint64_t)A32_KUSER_BASE_32 - addr64)
          ? (uint64_t)A32_KUSER_BASE_32 : (addr64 + size64);
    len_page = mach_round_page_u64(e64) - a_page;
    if (len_page == 0U) { ctx->regs[0] = MACH_KERN_SUCCESS; return; }
    if (a32_vm_munmap(ctx, a_page, len_page) != 0) {
        ctx->regs[0] = MACH_KERN_INVALID_ADDRESS; return;
    }
    ctx->regs[0] = MACH_KERN_SUCCESS;
}
static void mach_trap_vm_protect(a32_ctx_t *ctx)
{
    uint64_t addr64 = ((uint64_t)ctx->regs[2] << 32) | ctx->regs[1];
    uint32_t size_hi = ctx->regs[4];
    uint32_t set_max = ctx->regs[5];
    uint32_t prot = ctx->regs[6];
    uint64_t size64;
    uint32_t a_page;
    uint32_t len_page;
    uint64_t e64;
    (void)set_max;
    size64 = ((uint64_t)size_hi << 32) | (uint64_t)ctx->regs[3];
    if (size64 == 0ULL) { ctx->regs[0] = MACH_KERN_SUCCESS; return; }
    if (addr64 > 0xFFFFFFFFULL) { ctx->regs[0] = MACH_KERN_INVALID_ADDRESS; return; }
    if (addr64 >= (uint64_t)A32_KUSER_BASE_32) { ctx->regs[0] = MACH_KERN_PROTECTION_FAILURE; return; }
    prot &= 7U;
    a_page = ((uint32_t)addr64) & ~0xFFFU;
    e64 = (size64 > (uint64_t)A32_KUSER_BASE_32 - addr64)
          ? (uint64_t)A32_KUSER_BASE_32 : (addr64 + size64);
    len_page = mach_round_page_u64(e64) - a_page;
    if (len_page == 0U) { ctx->regs[0] = MACH_KERN_SUCCESS; return; }
    if (a32_vm_mprotect(ctx, a_page, len_page, prot) != 0) {
        ctx->regs[0] = MACH_KERN_PROTECTION_FAILURE; return;
    }
    ctx->regs[0] = MACH_KERN_SUCCESS;
}
static void mach_trap_vm_map(a32_ctx_t *ctx)
{
    uint32_t addr_va = ctx->regs[1];
    uint32_t size_lo = ctx->regs[2];
    uint32_t size_hi = ctx->regs[3];
    uint32_t mask_lo = ctx->regs[4];
    uint32_t mask_hi = ctx->regs[5];
    uint32_t flags = ctx->regs[6];
    uint32_t prot = 0U;
    uint64_t size64;
    uint64_t hint = 0ULL;
    uint32_t hint32 = 0U;
    uint32_t size32;
    uint32_t va;
    uint32_t mflags = 0x22U;
    uint32_t eff_mask = mask_lo & ~0xFFFU;
    bool anywhere;
    /* All 32-bit guest VAs have bits 32..63 clear, so mask_hi is satisfied. */
    (void)mask_hi;
    /* XNU 32-bit ARM `_kernel_trap_6to9`: args 4..7 arrive in r4,r5,r6,r8
     * (r7 is the frame pointer), so cur_protection is regs[8], not [sp]. */
    prot = ctx->regs[8];
    if (!mach_rw_ok(ctx, addr_va, 8U) ||
        !mach_read(ctx, &hint, addr_va, 8U)) {
        ctx->regs[0] = MACH_KERN_INVALID_ADDRESS;
        return;
    }
    size64 = ((uint64_t)size_hi << 32) | (uint64_t)size_lo;
    if (size64 == 0ULL) { ctx->regs[0] = MACH_KERN_SUCCESS; return; }
    if (size64 > 0xF0000000ULL) { ctx->regs[0] = MACH_KERN_NO_SPACE; return; }
    prot &= 7U;
    anywhere = (flags & 0x0001U) != 0U;
    if (!anywhere) {
        if (hint > 0xFFFFFFFFULL) { ctx->regs[0] = MACH_KERN_INVALID_ADDRESS; return; }
        if (hint >= (uint64_t)A32_KUSER_BASE_32 ||
            size64 > (uint64_t)A32_KUSER_BASE_32 - hint) {
            ctx->regs[0] = MACH_KERN_NO_SPACE; return;
        }
        hint32 = ((uint32_t)hint) & ~0xFFFU;
        if (hint32 < 0x1000U || (hint32 & eff_mask) != 0U) {
            ctx->regs[0] = MACH_KERN_NO_SPACE; return;
        }
        size32 = mach_round_page_u64(hint + size64) - hint32;
        mflags |= ((flags & 0x4000U) != 0U) ? 0x10U : 0x100000U;
    } else {
        size32 = mach_round_page_u64(size64);
    }
    if (size32 == 0U) { ctx->regs[0] = MACH_KERN_NO_SPACE; return; }
    if (anywhere && eff_mask != 0U) {
        uint64_t m = (uint64_t)(eff_mask | 0xFFFU);
        uint64_t align;
        uint64_t over;
        uint32_t raw_va;
        m |= (m >> 1); m |= (m >> 2); m |= (m >> 4); m |= (m >> 8); m |= (m >> 16);
        align = m + 1ULL;
        if (align > 0x80000000ULL) { ctx->regs[0] = MACH_KERN_NO_SPACE; return; }
        over = (uint64_t)size32 + align - 0x1000ULL;
        if (over > 0xF0000000ULL) { ctx->regs[0] = MACH_KERN_NO_SPACE; return; }
        raw_va = a32_vm_mmap2(ctx, 0U, (uint32_t)over, prot, mflags, -1, 0U);
        if (raw_va > 0xFFFFF000U) {
            ctx->regs[0] = mach_errno_to_kern(-(int32_t)raw_va); return;
        }
        va = (uint32_t)(((uint64_t)raw_va + align - 1ULL) & ~(align - 1ULL));
        if (va > raw_va) { (void)a32_vm_munmap(ctx, raw_va, va - raw_va); }
        if ((uint64_t)va + (uint64_t)size32 < (uint64_t)raw_va + over) {
            uint32_t tail = va + size32;
            (void)a32_vm_munmap(ctx, tail, (uint32_t)(((uint64_t)raw_va + over) - (uint64_t)tail));
        }
    } else {
        va = a32_vm_mmap2(ctx, anywhere ? 0U : hint32, size32, prot, mflags, -1, 0U);
        if (va > 0xFFFFF000U) {
            ctx->regs[0] = mach_errno_to_kern(-(int32_t)va); return;
        }
    }
    {
        uint64_t out = (uint64_t)va;
        if (!mach_write(ctx, addr_va, &out, 8U)) {
            ctx->regs[0] = MACH_KERN_INVALID_ADDRESS;
            return;
        }
        a32_vm32_copyout_invalidate_exec(ctx, addr_va, 8U);
    }
    ctx->regs[0] = MACH_KERN_SUCCESS;
}

static void mach_trap_vm_purgable(a32_ctx_t *ctx)
{
    uint32_t state_va = ctx->regs[4];
    uint32_t zero = 0U;
    if (state_va != 0U) {
        if (!mach_w_ok(ctx, state_va, 4U)) {
            ctx->regs[0] = MACH_KERN_INVALID_ADDRESS;
            return;
        }
        (void)mach_write_u32(ctx, state_va, zero);
        a32_vm32_copyout_invalidate_exec(ctx, state_va, 4U);
    }
    ctx->regs[0] = MACH_KERN_SUCCESS;
}

static void mach_trap_port_allocate(a32_ctx_t *ctx)
{
    uint32_t name_va = ctx->regs[2];
    uint32_t name = 0U;
    int idx;
    if (!mach_w_ok(ctx, name_va, 4U)) {
        ctx->regs[0] = MACH_KERN_INVALID_ADDRESS;
        return;
    }
    pthread_mutex_lock(&g_mach_lock);
    idx = mach_alloc_slot_locked(MACH_KIND_USER_QUEUE, 0U, &name);
    pthread_mutex_unlock(&g_mach_lock);
    if (idx < 0) {
        ctx->regs[0] = MACH_KERN_RESOURCE_SHORTAGE;
        return;
    }
    if (!mach_write_u32(ctx, name_va, name)) {
        ctx->regs[0] = MACH_KERN_INVALID_ADDRESS;
        return;
    }
    a32_vm32_copyout_invalidate_exec(ctx, name_va, 4U);
    ctx->regs[0] = MACH_KERN_SUCCESS;
}

static void mach_trap_port_destroy(a32_ctx_t *ctx)
{
    uint32_t name = ctx->regs[1];
    int idx;
    pthread_mutex_lock(&g_mach_lock);
    idx = mach_find_locked(name);
    if (idx >= 0) { mach_drop_queue_locked(idx); g_ports[idx].gen++; g_ports[idx].used = false; pthread_cond_broadcast(&g_ports[idx].cond); }
    pthread_mutex_unlock(&g_mach_lock);
    ctx->regs[0] = (idx >= 0) ? MACH_KERN_SUCCESS : MACH_KERN_INVALID_NAME;
}
static void mach_trap_port_deallocate(a32_ctx_t *ctx)
{
    uint32_t name = ctx->regs[1];
    int idx;
    pthread_mutex_lock(&g_mach_lock);
    idx = mach_find_locked(name);
    if (idx >= 0) {
        if (g_ports[idx].refs > 0U) { g_ports[idx].refs--; }
        if (g_ports[idx].refs == 0U) { mach_drop_queue_locked(idx); g_ports[idx].gen++; g_ports[idx].used = false; pthread_cond_broadcast(&g_ports[idx].cond); }
    }
    pthread_mutex_unlock(&g_mach_lock);
    ctx->regs[0] = (idx >= 0) ? MACH_KERN_SUCCESS : MACH_KERN_INVALID_NAME;
}

static void mach_trap_port_mod_refs(a32_ctx_t *ctx)
{
    uint32_t name = ctx->regs[1];
    int32_t delta = (int32_t)ctx->regs[3];
    int idx;
    pthread_mutex_lock(&g_mach_lock);
    idx = mach_find_locked(name);
    if (idx >= 0) {
        int64_t nr = (int64_t)g_ports[idx].refs + (int64_t)delta;
        if (nr <= 0) { mach_drop_queue_locked(idx); g_ports[idx].gen++; g_ports[idx].used = false; pthread_cond_broadcast(&g_ports[idx].cond); }
        else { g_ports[idx].refs = (uint32_t)nr; }
    }
    pthread_mutex_unlock(&g_mach_lock);
    ctx->regs[0] = (idx >= 0) ? MACH_KERN_SUCCESS : MACH_KERN_INVALID_NAME;
}
static void mach_trap_port_construct(a32_ctx_t *ctx)
{
    uint32_t name_va = ctx->regs[4];
    uint32_t name = 0U;
    int idx;
    if (!mach_w_ok(ctx, name_va, 4U)) {
        ctx->regs[0] = MACH_KERN_INVALID_ADDRESS;
        return;
    }
    pthread_mutex_lock(&g_mach_lock);
    idx = mach_alloc_slot_locked(MACH_KIND_USER_QUEUE, 0U, &name);
    pthread_mutex_unlock(&g_mach_lock);
    if (idx < 0) {
        ctx->regs[0] = MACH_KERN_RESOURCE_SHORTAGE;
        return;
    }
    if (!mach_write_u32(ctx, name_va, name)) {
        ctx->regs[0] = MACH_KERN_INVALID_ADDRESS;
        return;
    }
    a32_vm32_copyout_invalidate_exec(ctx, name_va, 4U);
    ctx->regs[0] = MACH_KERN_SUCCESS;
}

static void mach_trap_port_stub(a32_ctx_t *ctx, uint32_t name)
{
    bool ok;
    if (name == 0U) {
        ctx->regs[0] = MACH_KERN_SUCCESS;
        return;
    }
    pthread_mutex_lock(&g_mach_lock);
    ok = (mach_find_locked(name) >= 0 || mach_is_server_port(name));
    pthread_mutex_unlock(&g_mach_lock);
    ctx->regs[0] = ok ? MACH_KERN_SUCCESS : MACH_KERN_INVALID_NAME;
}

static void mach_trap_sema(a32_ctx_t *ctx, bool is_wait, bool is_timed, bool sig_first)
{
    uint32_t name = ctx->regs[0]; struct timespec dl = { 0, 0 }; bool has_dl = false;
    int idx; uint32_t gen0;
    if (is_timed) {
        uint32_t sec = sig_first ? ctx->regs[2] : ctx->regs[1];
        uint32_t nsec = sig_first ? ctx->regs[3] : ctx->regs[2];
        struct timespec now;
        if (nsec >= 1000000000U) { ctx->regs[0] = MACH_KERN_INVALID_VALUE; return; }
        (void)clock_gettime(CLOCK_REALTIME, &now);
        dl.tv_sec = now.tv_sec + (time_t)sec; dl.tv_nsec = now.tv_nsec + (long)nsec;
        if (dl.tv_nsec >= 1000000000L) { dl.tv_sec += 1; dl.tv_nsec -= 1000000000L; }
        has_dl = true;
    }
    if (sig_first && ctx->regs[1] != 0U) {
        int si;
        pthread_mutex_lock(&g_mach_lock);
        if (is_wait && mach_find_locked(name) < 0) {
            pthread_mutex_unlock(&g_mach_lock); ctx->regs[0] = MACH_KERN_INVALID_NAME; return;
        }
        si = mach_find_locked(ctx->regs[1]);
        if (si < 0) { pthread_mutex_unlock(&g_mach_lock); ctx->regs[0] = MACH_KERN_INVALID_NAME; return; }
        g_ports[si].sem_count++; pthread_cond_broadcast(&g_ports[si].cond);
        pthread_mutex_unlock(&g_mach_lock);
    }
    if (!is_wait) {
        uint32_t sn = (name != 0U) ? name : ctx->regs[1];
        pthread_mutex_lock(&g_mach_lock);
        idx = mach_find_locked(sn);
        if (idx < 0) { pthread_mutex_unlock(&g_mach_lock); ctx->regs[0] = MACH_KERN_INVALID_NAME; return; }
        g_ports[idx].sem_count++; pthread_cond_broadcast(&g_ports[idx].cond);
        pthread_mutex_unlock(&g_mach_lock); ctx->regs[0] = MACH_KERN_SUCCESS; return;
    }
    pthread_mutex_lock(&g_mach_lock);
    idx = mach_find_locked(name);
    if (idx < 0) { pthread_mutex_unlock(&g_mach_lock); ctx->regs[0] = MACH_KERN_INVALID_NAME; return; }
    gen0 = g_ports[idx].gen;
    while (g_ports[idx].used && g_ports[idx].gen == gen0 && g_ports[idx].sem_count == 0) {
        struct timespec step, now;
        if (mach_sig_pending(ctx)) { pthread_mutex_unlock(&g_mach_lock); ctx->regs[0] = MACH_KERN_ABORTED; return; }
        mach_step_ts(&step); if (has_dl) { mach_cap_ts(&step, &dl); }
        (void)pthread_cond_timedwait(&g_ports[idx].cond, &g_mach_lock, &step);
        if (!g_ports[idx].used || g_ports[idx].gen != gen0) { pthread_mutex_unlock(&g_mach_lock); ctx->regs[0] = MACH_KERN_TERMINATED; return; }
        if (mach_sig_pending(ctx)) { pthread_mutex_unlock(&g_mach_lock); ctx->regs[0] = MACH_KERN_ABORTED; return; }
        if (has_dl) {
            (void)clock_gettime(CLOCK_REALTIME, &now);
            if ((now.tv_sec > dl.tv_sec || (now.tv_sec == dl.tv_sec && now.tv_nsec >= dl.tv_nsec)) && g_ports[idx].sem_count == 0) {
                pthread_mutex_unlock(&g_mach_lock); ctx->regs[0] = MACH_KERN_TIMED_OUT; return;
            }
        }
    }
    if (!g_ports[idx].used || g_ports[idx].gen != gen0) { pthread_mutex_unlock(&g_mach_lock); ctx->regs[0] = MACH_KERN_TERMINATED; return; }
    if (mach_sig_pending(ctx)) { pthread_mutex_unlock(&g_mach_lock); ctx->regs[0] = MACH_KERN_ABORTED; return; }
    if (g_ports[idx].sem_count == 0) { bool exp = false; if (has_dl) { struct timespec now; (void)clock_gettime(CLOCK_REALTIME, &now); exp = (now.tv_sec > dl.tv_sec || (now.tv_sec == dl.tv_sec && now.tv_nsec >= dl.tv_nsec)); } pthread_mutex_unlock(&g_mach_lock); ctx->regs[0] = exp ? MACH_KERN_TIMED_OUT : MACH_KERN_ABORTED; return; }
    g_ports[idx].sem_count--; pthread_mutex_unlock(&g_mach_lock); ctx->regs[0] = MACH_KERN_SUCCESS;
}

/* Build a minimal MIG reply for server ports; returns reply length. */
static uint32_t mach_build_reply(const mach_hdr_t *req, const uint8_t *raw,
                                 uint32_t raw_len, uint8_t *rep)
{
    static const uint8_t ndr[8] = { 0U, 0U, 0U, 1U, 0U, 0U, 0U, 0U };
    mach_hdr_t rh;
    uint32_t ret = MACH_KERN_SUCCESS;
    uint32_t extra = 0U;
    uint32_t flavor = 0U;
    if (raw_len >= 36U) {
        memcpy(&flavor, raw + 32U, 4U);
    }
    if (req->remote == MACH_PORT_BOOTSTRAP) {
        ret = MACH_KERN_FAILURE;
    } else if (req->remote == MACH_PORT_HOST_SELF && req->id != 3409U) {
        extra = 24U;
    } else if (req->id == 3409U) {
        extra = 4U;
    }
    rh.bits = 0U;
    rh.size = 36U + extra;
    rh.remote = req->local;
    rh.local = 0U;
    rh.voucher = 0U;
    rh.id = req->id + 100U;
    if (extra == 4U) {
        /* MIG reply carrying a port right: complex message, 40 bytes:
         * header(24) + body(count=1)(4) + port descriptor(12). */
        uint32_t sp = (flavor == 4U) ? MACH_PORT_BOOTSTRAP : 0U;
        uint32_t n = 1U, z = 0U;
        uint16_t pz = 0U;
        rh.bits |= 0x80000000U;
        rh.size = 40U;
        memcpy(rep, &rh, 24U);
        memcpy(rep + 24U, &n, 4U);
        memcpy(rep + 28U, &sp, 4U);
        memcpy(rep + 32U, &z, 4U);
        memcpy(rep + 36U, &pz, 2U);
        rep[38] = 17U;
        rep[39] = 0U;
        return rh.size;
    }
    memcpy(rep, &rh, 24U);
    memcpy(rep + 24U, ndr, 8U);
    memcpy(rep + 32U, &ret, 4U);
    if (extra == 24U) {
        static const uint32_t info[5] = { 4U, 4U, 0x20000000U, 12U, 9U };
        uint32_t cnt = 5U;
        memcpy(rep + 36U, &cnt, 4U);
        memcpy(rep + 40U, info, 20U);
    }
    return rh.size;
}
static void mach_msg_trap(a32_ctx_t *ctx, bool overw) {
    uint32_t mv = ctx->regs[0], op = ctx->regs[1], ss = ctx->regs[2];
    uint32_t rs = ctx->regs[3], rn = ctx->regs[4], to = ctx->regs[5];
    uint32_t nt = ctx->regs[6], rv = mv;
    uint8_t tmp[MACH_MSG_MAX]; uint8_t rep[64]; mach_hdr_t h;
    uint32_t rl = 0U; int di = -1, si = -1, eq = 0;
    (void)nt;
    /* `_kernel_trap_6to9`: the 8th arg (rcv_msg) arrives in r8, not [sp]. */
    if (overw) { rv = ctx->regs[8]; }
    if (rv == 0U) { rv = mv; }
    if ((op & MACH_SEND_MSG) != 0U) {
        if (ss < 24U || ss > MACH_MSG_MAX || !mach_r_ok(ctx, mv, ss)) {
            ctx->regs[0] = MACH_SEND_INVALID_SIZE; return;
        }
        memcpy(tmp, a32_guest_ptr(ctx, mv), ss);
        memcpy(&h, tmp, 24U);
        if (h.size > ss || h.size < 24U) { h.size = ss; }
        pthread_mutex_lock(&g_mach_lock);
        if (mach_is_server_port(h.remote)) {
            rl = mach_build_reply(&h, tmp, ss, rep);
            di = mach_find_locked(h.local);
            if (di >= 0) { (void)mach_enqueue_locked(di, rep, rl); }
            pthread_mutex_unlock(&g_mach_lock);
        } else {
            uint8_t fwd[MACH_MSG_MAX]; mach_hdr_t *fh = (mach_hdr_t *)fwd;
            si = mach_find_locked(h.remote);
            if (si < 0) {
                pthread_mutex_unlock(&g_mach_lock);
                ctx->regs[0] = MACH_SEND_INVALID_DEST; return;
            }
            memcpy(fwd, tmp, h.size);
            fh->remote = h.local; fh->local = h.remote;
            eq = mach_enqueue_locked(si, fwd, h.size);
            pthread_mutex_unlock(&g_mach_lock);
            if (eq == -2) { ctx->regs[0] = MACH_SEND_NO_BUFFER; return; }
        }
    }
    if ((op & MACH_RCV_MSG) == 0U) { ctx->regs[0] = MACH_MSG_SUCCESS; return; }
    pthread_mutex_lock(&g_mach_lock);
    di = mach_find_locked(rn);
    if (di < 0) { pthread_mutex_unlock(&g_mach_lock); ctx->regs[0] = MACH_RCV_INVALID_NAME; return; }
    {
        struct timespec dl = { 0, 0 }; bool has_dl = ((op & MACH_RCV_TIMEOUT) != 0U);
        uint32_t gen0 = g_ports[di].gen;
        if (has_dl) { struct timespec now; uint64_t ns; (void)clock_gettime(CLOCK_REALTIME, &now); ns = (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec + (uint64_t)to * 1000000ULL; dl.tv_sec = (time_t)(ns / 1000000000ULL); dl.tv_nsec = (long)(ns % 1000000000ULL); }
        while (g_ports[di].used && g_ports[di].gen == gen0 && g_ports[di].qcount == 0U) {
            struct timespec step, now;
            if (mach_sig_pending(ctx)) { pthread_mutex_unlock(&g_mach_lock); ctx->regs[0] = MACH_RCV_INTERRUPTED; return; }
            mach_step_ts(&step); if (has_dl) { mach_cap_ts(&step, &dl); }
            (void)pthread_cond_timedwait(&g_ports[di].cond, &g_mach_lock, &step);
            if (!g_ports[di].used || g_ports[di].gen != gen0) { pthread_mutex_unlock(&g_mach_lock); ctx->regs[0] = MACH_RCV_PORT_DIED; return; }
            if (mach_sig_pending(ctx)) { pthread_mutex_unlock(&g_mach_lock); ctx->regs[0] = MACH_RCV_INTERRUPTED; return; }
            if (has_dl) { (void)clock_gettime(CLOCK_REALTIME, &now); if ((now.tv_sec > dl.tv_sec || (now.tv_sec == dl.tv_sec && now.tv_nsec >= dl.tv_nsec)) && g_ports[di].qcount == 0U) { pthread_mutex_unlock(&g_mach_lock); ctx->regs[0] = MACH_RCV_TIMED_OUT; return; } }
        }
        if (!g_ports[di].used || g_ports[di].gen != gen0) { pthread_mutex_unlock(&g_mach_lock); ctx->regs[0] = MACH_RCV_PORT_DIED; return; }
        if (g_ports[di].qcount == 0U) { bool exp = false; if (has_dl) { struct timespec now; (void)clock_gettime(CLOCK_REALTIME, &now); exp = (now.tv_sec > dl.tv_sec || (now.tv_sec == dl.tv_sec && now.tv_nsec >= dl.tv_nsec)); } pthread_mutex_unlock(&g_mach_lock); ctx->regs[0] = exp ? MACH_RCV_TIMED_OUT : MACH_RCV_INVALID_NAME; return; }
    }
    {
        unsigned hd = g_ports[di].qhead; uint32_t ml = g_ports[di].qsize[hd]; uint8_t *mp;
        if (rs < ml) {
            if ((op & MACH_RCV_LARGE) != 0U) {
                uint32_t wsz = ml + 8U; pthread_mutex_unlock(&g_mach_lock);
                if (rs >= 24U && mach_w_ok(ctx, rv, 24U)) { (void)mach_write_u32(ctx, rv + 4U, wsz); a32_vm32_copyout_invalidate_exec(ctx, rv + 4U, 4U); }
                ctx->regs[0] = MACH_RCV_TOO_LARGE; return;
            }
            mp = g_ports[di].queue[hd]; g_ports[di].queue[hd] = NULL; g_ports[di].qhead = (hd + 1U) % MACH_QUEUE_DEPTH; g_ports[di].qcount--;
            pthread_mutex_unlock(&g_mach_lock); free(mp); ctx->regs[0] = MACH_RCV_TOO_LARGE; return;
        }
        mp = g_ports[di].queue[hd];
        g_ports[di].queue[hd] = NULL; g_ports[di].qhead = (hd + 1U) % MACH_QUEUE_DEPTH;
        g_ports[di].qcount--; pthread_mutex_unlock(&g_mach_lock);
        if (rv == 0U || !mach_w_ok(ctx, rv, rs)) { free(mp); ctx->regs[0] = MACH_KERN_INVALID_ADDRESS; return; }
        {
            uint32_t cp = (ml <= rs) ? ml : rs;
            memcpy(a32_guest_ptr(ctx, rv), mp, cp);
            if (rs >= ml + 8U) {
                uint8_t z[8] = { 0U, 0U, 0U, 0U, 8U, 0U, 0U, 0U };
                memcpy((uint8_t *)a32_guest_ptr(ctx, rv) + ml, z, 8U);
                cp = ml + 8U;
            }
            free(mp);
            a32_vm32_copyout_invalidate_exec(ctx, rv, cp);
        }
        ctx->regs[0] = MACH_MSG_SUCCESS; return;
    }
}
static uint64_t mach_now_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
int a32_ios_dispatch_mach_or_arm_trap(a32_ctx_t *ctx, uint32_t raw_r12) {
    int32_t s; int neg;
    if (ctx == NULL) { return -1; }
    mach_ensure_init();
    if (raw_r12 >= 0x80000000U && raw_r12 <= 0x80000003U) { mach_arm_trap(ctx, raw_r12); return 0; }
    s = (int32_t)raw_r12; if (s >= 0) { ctx->regs[0] = MACH_KERN_FAILURE; return 0; }
    neg = -s;
    switch (neg) {
    case 10: mach_trap_vm_allocate(ctx); return 0;
    case 12: mach_trap_vm_deallocate(ctx); return 0;
    case 14: mach_trap_vm_protect(ctx); return 0;
    case 15: mach_trap_vm_map(ctx); return 0;
    case 13: mach_trap_vm_purgable(ctx); return 0;
    case 16: mach_trap_port_allocate(ctx); return 0;
    case 17: mach_trap_port_destroy(ctx); return 0;
    case 18: mach_trap_port_deallocate(ctx); return 0;
    case 19: mach_trap_port_mod_refs(ctx); return 0;
    case 24: mach_trap_port_construct(ctx); return 0;
    case 20: case 21: case 22: case 23: case 40:
        mach_trap_port_stub(ctx, ctx->regs[1]); return 0;
    case 25: mach_trap_port_destruct(ctx); return 0;
    case 26: {
        uint32_t nm = 0U; int ix;
        pthread_mutex_lock(&g_mach_lock); ix = mach_alloc_slot_locked(MACH_KIND_USER_QUEUE, 0U, &nm);
        pthread_mutex_unlock(&g_mach_lock);
        ctx->regs[0] = (ix >= 0) ? nm : MACH_KERN_RESOURCE_SHORTAGE; return 0;
    }
    case 27: ctx->regs[0] = a32_ios_mach_thread_self_port(ctx); return 0;
    case 28: ctx->regs[0] = MACH_PORT_TASK_SELF; return 0;
    case 29: ctx->regs[0] = MACH_PORT_HOST_SELF; return 0;
    case 31: mach_msg_trap(ctx, false); return 0;
    case 32: mach_msg_trap(ctx, true); return 0;
    case 33: case 34: case 35: mach_trap_sema(ctx, false, false, false); return 0;
    case 36: mach_trap_sema(ctx, true, false, false); return 0;
    case 37: mach_trap_sema(ctx, true, false, true); return 0;
    case 38: mach_trap_sema(ctx, true, true, false); return 0;
    case 39: mach_trap_sema(ctx, true, true, true); return 0;
    case 89: {
        uint32_t iv = ctx->regs[0]; uint32_t tb[2] = { 1U, 1U };
        if (!mach_w_ok(ctx, iv, 8U)) { ctx->regs[0] = MACH_KERN_INVALID_ADDRESS; return 0; }
        (void)mach_write(ctx, iv, tb, 8U);
        a32_vm32_copyout_invalidate_exec(ctx, iv, 8U);
        ctx->regs[0] = MACH_KERN_SUCCESS; return 0;
    }
    case 90: {
        uint64_t dl = ((uint64_t)ctx->regs[1] << 32) | (uint64_t)ctx->regs[0];
        while (mach_now_ns() < dl) {
            uint64_t rem = dl - mach_now_ns(); struct timespec rq;
            if (mach_sig_pending(ctx)) { ctx->regs[0] = MACH_KERN_ABORTED; return 0; }
            rq.tv_sec = (time_t)(rem / 1000000000ULL); rq.tv_nsec = (long)(rem % 1000000000ULL);
            if (rq.tv_sec > 0 || rq.tv_nsec > 25000000L) { rq.tv_sec = 0; rq.tv_nsec = 25000000L; }
            (void)nanosleep(&rq, NULL);
        }
        if (mach_sig_pending(ctx)) { ctx->regs[0] = MACH_KERN_ABORTED; return 0; }
        ctx->regs[0] = MACH_KERN_SUCCESS; return 0;
    }
    case 91: {
        uint32_t nm = 0U; int ix;
        pthread_mutex_lock(&g_mach_lock); ix = mach_alloc_slot_locked(MACH_KIND_TIMER, 0U, &nm);
        pthread_mutex_unlock(&g_mach_lock);
        ctx->regs[0] = (ix >= 0) ? nm : MACH_KERN_RESOURCE_SHORTAGE; return 0;
    }
    case 92: case 93: case 94: ctx->regs[0] = MACH_KERN_SUCCESS; return 0;
    case 59: case 60: case 61: (void)sched_yield(); ctx->regs[0] = MACH_KERN_SUCCESS; return 0;
    default: ctx->regs[0] = MACH_KERN_FAILURE; return 0;
    }
}
