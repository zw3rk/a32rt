/*
 * src/os/backend_darwin.c -- macOS host backend (Wave U3 A32-4, Backend 2)
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Standalone macOS arm64 Mach-O (zero Hypervisor.framework dependency):
 * libSystem mmap, per-thread MAP_JIT APRR W^X publication, pthread_kill
 * preemption for quiescence. Apple-only paths sit behind __APPLE__
 * guards with -ENOSYS fallbacks so this TU stays syntax-clean under
 * make check on Linux. Curated C11.
 */

#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#include "os/backend.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <libkern/OSCacheControl.h>
#include <mach/vm_inherit.h>
#endif

a32_backend_kind_t a32_backend_kind(void)
{
#if defined(__APPLE__)
    return A32_BACKEND_DARWIN;
#else
    return A32_BACKEND_LINUX;
#endif
}

size_t a32_backend_page_size(void)
{
    long h = sysconf(_SC_PAGESIZE);
    if (h != 4096L && h != 16384L && h != 65536L) {
        return 16384U;
    }
    return (size_t)h;
}

static int host_prot(int linux_prot)
{
    int p = 0;
    if ((linux_prot & 0x1) != 0) {
        p |= PROT_READ;
    }
    if ((linux_prot & 0x2) != 0) {
        p |= PROT_WRITE;
    }
    if ((linux_prot & 0x4) != 0) {
        p |= PROT_READ;
    }
    return p;
}

int a32_backend_map_anon(void *hint_addr, size_t len, int linux_prot,
    int fixed, int replace, int shared, void **out_host_addr)
{
    int flags = (shared ? MAP_SHARED : MAP_PRIVATE) | MAP_ANONYMOUS;
    void *res;
    if (len == 0U || out_host_addr == NULL) {
        return -EINVAL;
    }
    if (replace && hint_addr != NULL) {
        /* A32-R4-2: install genuinely fresh anonymous backing instead
         * of patching the previous mapping in place. Zeroing in place
         * writes through a shared-file mapping into the file itself
         * and leaves the range attached to the old object; skipping
         * the zeroing for PROT_NONE leaves stale bytes that a later
         * mprotect re-exposes. A fresh MAP_FIXED anonymous mapping
         * detaches both cases. */
        size_t hp = a32_backend_page_size();
        uintptr_t start = (uintptr_t)hint_addr & ~(uintptr_t)(hp - 1U);
        uintptr_t end = ((uintptr_t)hint_addr + len + hp - 1U) &
            ~(uintptr_t)(hp - 1U);
        if (shared != 0 && hp > 4096U &&
            (((uintptr_t)hint_addr & (hp - 1U)) != 0U)) {
            return -EINVAL;
        }
        if (start == (uintptr_t)hint_addr && end == (uintptr_t)hint_addr + len) {
            res = mmap(hint_addr, len, host_prot(linux_prot),
                flags | MAP_FIXED, -1, 0);
            if (res == MAP_FAILED) {
                return -errno;
            }
#if defined(__APPLE__)
            if (minherit(hint_addr, len,
                    shared != 0 ? VM_INHERIT_SHARE : VM_INHERIT_COPY) != 0) {
                return -errno;
            }
#endif
            *out_host_addr = hint_addr;
            return 0;
        }
        /* Sub-page replacement (a 4 KiB guest range inside a larger
         * host page): replacing a partial host page with MAP_FIXED
         * would momentarily unmap and overwrite live 4 KiB siblings
         * (e.g. an adjacent thread's stack). Full interior host pages
         * have no siblings and get a fresh MAP_FIXED anonymous mapping;
         * partial head/tail host pages are already private anonymous
         * (A32-R5-2 rejects shared siblings upstream), so zeroing the
         * target sub-pages in place preserves concurrent sibling state. */
        {
            uintptr_t h_addr = (uintptr_t)hint_addr;
            uintptr_t h_end = h_addr + len;
            uintptr_t inner_start = (h_addr + hp - 1U) & ~(uintptr_t)(hp - 1U);
            uintptr_t inner_end = h_end & ~(uintptr_t)(hp - 1U);
            size_t hlen = end - start;
            if (shared != 0) {
                return -EINVAL;
            }
            if (mprotect((void *)start, hlen, PROT_READ | PROT_WRITE) != 0) {
                return -errno;
            }
            if (inner_end > inner_start) {
                res = mmap((void *)inner_start, inner_end - inner_start,
                    PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
                if (res == MAP_FAILED) {
                    return -errno;
                }
            }
            if (h_addr < inner_start) {
                uintptr_t head_end = h_end < inner_start ? h_end : inner_start;
                memset((void *)h_addr, 0, head_end - h_addr);
            }
            if (h_end > inner_end && inner_end >= inner_start) {
                memset((void *)inner_end, 0, h_end - inner_end);
            }
#if defined(__APPLE__)
            if (minherit((void *)start, hlen, VM_INHERIT_COPY) != 0) {
                return -errno;
            }
#endif
            /* Large-page hosts keep guest pages RW at host level (see
             * a32_backend_protect); per-4 KiB guest protection stays a
             * Soft-TLB matter. */
            *out_host_addr = hint_addr;
            return 0;
        }
    }
    if (replace) {
        flags |= MAP_FIXED;
    } else if (fixed) {
#if defined(MAP_FIXED_NOREPLACE)
        flags |= MAP_FIXED_NOREPLACE;
#endif
    }
    res = mmap(hint_addr, len, host_prot(linux_prot), flags, -1, 0);
    if (res == MAP_FAILED) {
        return -errno;
    }
    if (!replace && fixed && hint_addr != NULL && res != hint_addr) {
        munmap(res, len);
        return -EEXIST;
    }
    *out_host_addr = res;
    return 0;
}

int a32_backend_map_file(void *hint_addr, size_t len, int linux_prot,
    int shared, int fixed, int replace, int host_fd,
    uint64_t aligned_file_offset, void **out_host_addr)
{
    int flags = shared ? MAP_SHARED : MAP_PRIVATE;
    void *res;
    if (len == 0U || host_fd < 0 || out_host_addr == NULL) {
        return -EINVAL;
    }
    if ((replace || fixed) && hint_addr != NULL) {
        size_t hp = a32_backend_page_size();
        if ((((uintptr_t)hint_addr & (hp - 1U)) != 0U) ||
            ((aligned_file_offset & (uint64_t)(hp - 1U)) != 0ULL)) {
            return -EINVAL;
        }
    }
    if (replace) {
        flags |= MAP_FIXED;
    } else if (fixed) {
#if defined(MAP_FIXED_NOREPLACE)
        flags |= MAP_FIXED_NOREPLACE;
#endif
    }
    res = mmap(hint_addr, len, host_prot(linux_prot), flags, host_fd,
        (off_t)aligned_file_offset);
    if (res == MAP_FAILED) {
        return -errno;
    }
    if (!replace && fixed && hint_addr != NULL && res != hint_addr) {
        munmap(res, len);
        return -EEXIST;
    }
#if defined(__APPLE__)
    if (shared == 0) {
        size_t hp = a32_backend_page_size();
        uintptr_t start = (uintptr_t)res & ~(uintptr_t)(hp - 1U);
        uintptr_t end = ((uintptr_t)res + len + hp - 1U) &
            ~(uintptr_t)(hp - 1U);
        (void)minherit((void *)start, end - start, VM_INHERIT_COPY);
    }
#endif
    *out_host_addr = res;
    return 0;
}

int a32_backend_protect(void *host_addr, size_t len, int linux_prot)
{
    size_t hp;
    uintptr_t start;
    uintptr_t end;
    if (host_addr == NULL || len == 0U) {
        return -EINVAL;
    }
    hp = a32_backend_page_size();
    start = (uintptr_t)host_addr & ~(uintptr_t)(hp - 1U);
    end = ((uintptr_t)host_addr + len + hp - 1U) & ~(uintptr_t)(hp - 1U);
    if (hp > 4096U) {
        /* On 16 KiB Darwin hosts, 4 KiB guest page permissions are enforced
         * by the Soft-TLB (vm32); keep shared 16 KiB host pages RW so a
         * sub-page mprotect never faults sibling 4 KiB guest pages. */
        (void)linux_prot;
        if (mprotect((void *)start, end - start,
                PROT_READ | PROT_WRITE) != 0) {
            return -errno;
        }
        return 0;
    }
    if (mprotect((void *)start, end - start, host_prot(linux_prot)) != 0) {
        return -errno;
    }
    return 0;
}

int a32_backend_unmap(void *host_addr, size_t len)
{
    size_t hp;
    if (host_addr == NULL || len == 0U) {
        return -EINVAL;
    }
    hp = a32_backend_page_size();
    if (hp > 4096U &&
        (((uintptr_t)host_addr & (hp - 1U)) != 0U || (len & (hp - 1U)) != 0U)) {
        return 0;
    }
    if (munmap(host_addr, len) != 0) {
        return -errno;
    }
    return 0;
}

int a32_backend_msync(void *host_addr, size_t len)
{
    if (host_addr == NULL || len == 0U) {
        return -EINVAL;
    }
    if (msync(host_addr, len, MS_SYNC) != 0) {
        return -errno;
    }
    return 0;
}

void *a32_backend_jit_alloc(size_t size)
{
    void *res;
    if (size == 0U) {
        return NULL;
    }
#if defined(__APPLE__)
    res = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0);
#else
    res = mmap(NULL, size, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
    if (res == MAP_FAILED) {
        return NULL;
    }
    return res;
}

void a32_backend_jit_thread_init(void)
{
#if defined(__APPLE__)
    pthread_jit_write_protect_np(1);
#endif
}

void a32_backend_jit_begin_write(void *buf, size_t size)
{
    (void)size;
#if defined(__APPLE__)
    if (buf == NULL) {
        return;
    }
    pthread_jit_write_protect_np(0);
#else
    (void)buf;
#endif
}

int a32_backend_jit_seal_rx(void *buf, size_t size)
{
    if (buf == NULL || size == 0U) {
        return -EINVAL;
    }
#if defined(__APPLE__)
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(buf, size);
#else
    if (mprotect(buf, size, PROT_READ | PROT_EXEC) != 0) {
        return -errno;
    }
    __builtin___clear_cache((char *)buf, (char *)buf + size);
#endif
    return 0;
}

void a32_backend_jit_free(void *buf, size_t size)
{
    if (buf == NULL || size == 0U) {
        return;
    }
    munmap(buf, size);
}

/* Worker wake-pipe helpers live in src/os/backend_sync.c (A32-R13-11). */

/* ---- 6. Darwin host futex wait/wake table (A32-IOS-D11) ----
 *
 * Linux futex semantics emulated with pthread condvars: FUTEX_WAIT and
 * FUTEX_WAIT_BITSET park the caller on a table slot keyed by guest uaddr;
 * FUTEX_WAKE and FUTEX_WAKE_BITSET set woken flags and signal the matching
 * slots; FUTEX_REQUEUE and FUTEX_CMP_REQUEUE move waiter keys to uaddr2;
 * FUTEX_WAKE_OP performs its atomic op on uaddr2 under the table lock.
 * Returns negative Linux errnos, like the kernel path. */
#if !defined(__linux__)

#define A32_FUTEX_SLOTS 128U

#define A32_FUTEX_WAIT 0U
#define A32_FUTEX_WAKE 1U
#define A32_FUTEX_REQUEUE 3U
#define A32_FUTEX_CMP_REQUEUE 4U
#define A32_FUTEX_WAKE_OP 5U
#define A32_FUTEX_WAIT_BITSET 9U
#define A32_FUTEX_WAKE_BITSET 10U
#define A32_FUTEX_ALL_BITS 0xFFFFFFFFU
#define A32_FUTEX_NSEC_PER_SEC 1000000000ULL

typedef struct {
    bool in_use;
    bool woken;
    uint32_t uaddr_va;
    uint32_t bitset;
    pthread_cond_t cv;
} a32_futex_slot_t;

static a32_futex_slot_t g_futex_slots[A32_FUTEX_SLOTS];
static pthread_mutex_t g_futex_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t g_futex_once = PTHREAD_ONCE_INIT;

static void futex_slots_init(void)
{
    uint32_t i;
    for (i = 0U; i < A32_FUTEX_SLOTS; i++) {
        (void)pthread_cond_init(&g_futex_slots[i].cv, NULL);
        g_futex_slots[i].in_use = false;
        g_futex_slots[i].woken = false;
        g_futex_slots[i].uaddr_va = 0U;
        g_futex_slots[i].bitset = 0U;
    }
}

static bool futex_ts_ok(const struct timespec *ts)
{
    return ts->tv_sec >= 0 && ts->tv_nsec >= 0 &&
        ts->tv_nsec < 1000000000L;
}

static uint32_t futex_read_u32(a32_ctx_t *ctx, uint32_t va, bool *ok)
{
    uint32_t v = 0U;
    if (!a32_guest_range_ok(ctx, va, 4U, (uint32_t)PROT_READ)) {
        *ok = false;
        return 0U;
    }
    memcpy(&v, a32_guest_ptr(ctx, va), 4U);
    *ok = true;
    return v;
}

/* Wake up to budget unserved waiters keyed by uaddr_va whose bitset
 * intersects mask. Caller holds g_futex_mu. Returns the number woken. */
static uint32_t futex_wake_locked(uint32_t uaddr_va, uint32_t mask,
    uint32_t budget)
{
    uint32_t i;
    uint32_t n = 0U;
    for (i = 0U; i < A32_FUTEX_SLOTS && n < budget; i++) {
        a32_futex_slot_t *s = &g_futex_slots[i];
        if (s->in_use && !s->woken && s->uaddr_va == uaddr_va &&
            (s->bitset & mask) != 0U) {
            s->woken = true;
            (void)pthread_cond_signal(&s->cv);
            n++;
        }
    }
    return n;
}

/* Move up to max_move unserved waiters from one key to another.
 * Caller holds g_futex_mu. Returns the number requeued. */
static uint32_t futex_requeue_locked(uint32_t from_va, uint32_t to_va,
    uint32_t max_move)
{
    uint32_t i;
    uint32_t n = 0U;
    for (i = 0U; i < A32_FUTEX_SLOTS && n < max_move; i++) {
        a32_futex_slot_t *s = &g_futex_slots[i];
        if (s->in_use && !s->woken && s->uaddr_va == from_va) {
            s->uaddr_va = to_va;
            n++;
        }
    }
    return n;
}

static void futex_add_ns(const struct timespec *base, uint64_t add_ns,
    struct timespec *out)
{
    uint64_t b = (uint64_t)base->tv_sec * A32_FUTEX_NSEC_PER_SEC +
        (uint64_t)base->tv_nsec;
    uint64_t sum = b + add_ns;
    if (sum < b) {
        sum = UINT64_MAX;
    }
    out->tv_sec = (time_t)(sum / A32_FUTEX_NSEC_PER_SEC);
    out->tv_nsec = (long)(sum % A32_FUTEX_NSEC_PER_SEC);
}

int a32_backend_darwin_futex(a32_ctx_t *ctx, uint32_t uaddr_va, uint32_t op,
    uint32_t val, const void *timeout_ts, uint32_t val2,
    uint32_t uaddr2_va, uint32_t val3)
{
    uint32_t basic;
    if (ctx == NULL) {
        return -EFAULT;
    }
    (void)pthread_once(&g_futex_once, futex_slots_init);
    basic = op & 0x7FU;
    switch (basic) {
    case A32_FUTEX_WAIT:
    case A32_FUTEX_WAIT_BITSET: {
        const struct timespec *ts = (const struct timespec *)timeout_ts;
        uint32_t bitset = A32_FUTEX_ALL_BITS;
        bool has_deadline = false;
        struct timespec deadline = { 0, 0 };
        uint32_t cur = 0U;
        bool ok = false;
        uint32_t i;
        a32_futex_slot_t *slot = NULL;
        bool timed_out = false;
        if (basic == A32_FUTEX_WAIT_BITSET) {
            if (val3 == 0U) {
                return -EINVAL;
            }
            bitset = val3;
        }
        cur = futex_read_u32(ctx, uaddr_va, &ok);
        if (!ok) {
            return -EFAULT;
        }
        if (cur != val) {
            return -EAGAIN;
        }
        if (ts != NULL) {
            struct timespec now;
            int64_t d_sec;
            long d_nsec;
            clockid_t clk;
            if (!futex_ts_ok(ts)) {
                return -EINVAL;
            }
            clk = (basic == A32_FUTEX_WAIT ||
                (op & 0x100U) == 0U) ? CLOCK_MONOTONIC : CLOCK_REALTIME;
            (void)clock_gettime(clk, &now);
            d_sec = (int64_t)ts->tv_sec - (int64_t)now.tv_sec;
            d_nsec = ts->tv_nsec - now.tv_nsec;
            if (d_nsec < 0L) {
                d_sec -= 1;
                d_nsec += 1000000000L;
            }
            if (d_sec < 0) {
                return -ETIMEDOUT;
            }
            if (basic == A32_FUTEX_WAIT || clk == CLOCK_MONOTONIC) {
                struct timespec now_r;
                (void)clock_gettime(CLOCK_REALTIME, &now_r);
                futex_add_ns(&now_r,
                    (uint64_t)d_sec * A32_FUTEX_NSEC_PER_SEC +
                    (uint64_t)d_nsec, &deadline);
            } else {
                deadline = *ts;
            }
            has_deadline = true;
        }
        if ((__atomic_load_n(&ctx->pending_signals, __ATOMIC_SEQ_CST) &
             ~ctx->sig_mask) != 0ULL) {
            return -EINTR;
        }
        (void)pthread_mutex_lock(&g_futex_mu);
        cur = futex_read_u32(ctx, uaddr_va, &ok);
        if (!ok || cur != val) {
            (void)pthread_mutex_unlock(&g_futex_mu);
            return ok ? -EAGAIN : -EFAULT;
        }
        for (i = 0U; i < A32_FUTEX_SLOTS; i++) {
            if (!g_futex_slots[i].in_use) {
                slot = &g_futex_slots[i];
                break;
            }
        }
        if (slot == NULL) {
            (void)pthread_mutex_unlock(&g_futex_mu);
            return -EAGAIN;
        }
        slot->in_use = true;
        slot->woken = false;
        slot->uaddr_va = uaddr_va;
        slot->bitset = bitset;
        while (!slot->woken) {
            int rc;
            if (!has_deadline) {
                rc = pthread_cond_wait(&slot->cv, &g_futex_mu);
                (void)rc;
            } else {
                rc = pthread_cond_timedwait(&slot->cv, &g_futex_mu,
                    &deadline);
                if (rc == ETIMEDOUT && !slot->woken) {
                    timed_out = true;
                    break;
                }
            }
        }
        slot->in_use = false;
        (void)pthread_mutex_unlock(&g_futex_mu);
        return timed_out ? -ETIMEDOUT : 0;
    }
    case A32_FUTEX_WAKE:
    case A32_FUTEX_WAKE_BITSET: {
        uint32_t mask = (basic == A32_FUTEX_WAKE_BITSET) ? val3 :
            A32_FUTEX_ALL_BITS;
        uint32_t n;
        if (!a32_guest_range_ok(ctx, uaddr_va, 4U, (uint32_t)PROT_READ)) {
            return -EFAULT;
        }
        (void)pthread_mutex_lock(&g_futex_mu);
        n = futex_wake_locked(uaddr_va, mask, val);
        (void)pthread_mutex_unlock(&g_futex_mu);
        return (int)n;
    }
    case A32_FUTEX_REQUEUE: {
        bool ok = false;
        uint32_t woken = 0U;
        uint32_t moved = 0U;
        (void)futex_read_u32(ctx, uaddr_va, &ok);
        if (!ok) {
            return -EFAULT;
        }
        if (!a32_guest_range_ok(ctx, uaddr2_va, 4U, (uint32_t)PROT_READ)) {
            return -EFAULT;
        }
        (void)pthread_mutex_lock(&g_futex_mu);
        woken = futex_wake_locked(uaddr_va, A32_FUTEX_ALL_BITS, val);
        moved = futex_requeue_locked(uaddr_va, uaddr2_va, val3);
        (void)pthread_mutex_unlock(&g_futex_mu);
        return (int)(woken + moved);
    }
    case A32_FUTEX_CMP_REQUEUE: {
        bool ok = false;
        uint32_t cur = futex_read_u32(ctx, uaddr_va, &ok);
        uint32_t woken = 0U;
        uint32_t moved = 0U;
        if (!ok) {
            return -EFAULT;
        }
        if (cur != val3) {
            return -EAGAIN;
        }
        if (!a32_guest_range_ok(ctx, uaddr2_va, 4U, (uint32_t)PROT_READ)) {
            return -EFAULT;
        }
        (void)pthread_mutex_lock(&g_futex_mu);
        woken = futex_wake_locked(uaddr_va, A32_FUTEX_ALL_BITS, val);
        moved = futex_requeue_locked(uaddr_va, uaddr2_va, 0xFFFFFFFFU);
        (void)pthread_mutex_unlock(&g_futex_mu);
        return (int)(woken + moved);
    }
    case A32_FUTEX_WAKE_OP: {
        uint32_t old = 0U;
        uint32_t newv = 0U;
        bool ok = false;
        uint32_t op_op;
        uint32_t oparg;
        uint32_t cmp;
        uint32_t cmparg;
        bool do_wake2 = false;
        uint32_t n1 = 0U;
        uint32_t n2 = 0U;
        if (!a32_guest_range_ok(ctx, uaddr_va, 4U, (uint32_t)PROT_READ)) {
            return -EFAULT;
        }
        if (!a32_guest_range_ok(ctx, uaddr2_va, 4U,
                (uint32_t)(PROT_READ | PROT_WRITE))) {
            return -EFAULT;
        }
        op_op = (val3 >> 28) & 0xFU;
        oparg = (val3 >> 12) & 0xFFFU;
        cmp = (val3 >> 24) & 0xFU;
        cmparg = val3 & 0xFFFU;
        (void)pthread_mutex_lock(&g_futex_mu);
        n1 = futex_wake_locked(uaddr_va, A32_FUTEX_ALL_BITS, val);
        old = futex_read_u32(ctx, uaddr2_va, &ok);
        if (!ok) {
            (void)pthread_mutex_unlock(&g_futex_mu);
            return -EFAULT;
        }
        switch (op_op) {
        case 0U:
            newv = oparg;
            break;
        case 1U:
            newv = old + oparg;
            break;
        case 2U:
            newv = old | oparg;
            break;
        case 3U:
            newv = old & ~oparg;
            break;
        case 4U:
            newv = old ^ oparg;
            break;
        default:
            (void)pthread_mutex_unlock(&g_futex_mu);
            return -EINVAL;
        }
        memcpy(a32_guest_ptr(ctx, uaddr2_va), &newv, 4U);
        switch (cmp) {
        case 0U:
            do_wake2 = (old == cmparg);
            break;
        case 1U:
            do_wake2 = (old != cmparg);
            break;
        case 2U:
            do_wake2 = (old < cmparg);
            break;
        case 3U:
            do_wake2 = (old <= cmparg);
            break;
        case 4U:
            do_wake2 = (old > cmparg);
            break;
        case 5U:
            do_wake2 = (old >= cmparg);
            break;
        default:
            (void)pthread_mutex_unlock(&g_futex_mu);
            return -EINVAL;
        }
        if (do_wake2) {
            n2 = futex_wake_locked(uaddr2_va, A32_FUTEX_ALL_BITS, val2);
        }
        (void)pthread_mutex_unlock(&g_futex_mu);
        return (int)(n1 + n2);
    }
    default:
        return -ENOSYS;
    }
}

void a32_backend_darwin_futex_wake(a32_ctx_t *ctx, uint32_t uaddr_va,
    uint32_t count)
{
    if (ctx == NULL) {
        return;
    }
    (void)pthread_once(&g_futex_once, futex_slots_init);
    if (!a32_guest_range_ok(ctx, uaddr_va, 4U, (uint32_t)PROT_READ)) {
        return;
    }
    (void)pthread_mutex_lock(&g_futex_mu);
    (void)futex_wake_locked(uaddr_va, A32_FUTEX_ALL_BITS, count);
    (void)pthread_mutex_unlock(&g_futex_mu);
}

#endif /* !defined(__linux__) */
