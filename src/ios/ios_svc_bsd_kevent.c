/*
 * src/ios/ios_svc_bsd_kevent.c — iOS BSD kqueue / kevent / kevent64 / kevent_qos
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Curated C11, host-portable (macOS arm64 AND aarch64-linux-musl).
 * Portable user-space kqueue: 8 kqueues x 32 knotes backed by real host
 * pipe fds, poll() for READ/WRITE, CLOCK_MONOTONIC for TIMER.
 */

#if !defined(__APPLE__)
#define _GNU_SOURCE
#else
#define _DARWIN_C_SOURCE
#endif

#include "a32rt.h"
#include "ios/ios.h"
#include "vm/vm32.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* Darwin ABI constants (stable XNU ABI, valid on both hosts). */
#define KQ_EV_READ -1
#define KQ_EV_WRITE -2
#define KQ_EV_TIMER -7
#define KQ_EV_ADD 0x0001U
#define KQ_EV_DELETE 0x0002U
#define KQ_EV_ENABLE 0x0004U
#define KQ_EV_DISABLE 0x0008U
#define KQ_EV_ONESHOT 0x0010U
#define KQ_EV_CLEAR 0x0020U
#define KQ_EV_RECEIPT 0x0040U
#define KQ_EV_DISPATCH 0x0080U
#define KQ_EV_ERROR 0x4000U
#define KQ_EV_EOF 0x8000U
#define KQ_ENOENT 2
#define KQ_EBADF 9
#define KQ_EFAULT 14
#define KQ_EINVAL 22
#define KQ_ENOMEM 12
#define KQ_MAX 8U
#define KQ_KNOTE_MAX 32U
#define KQ_EV_CAP 256U
#define KQ_WAIT_CAP_MS 60000
#define KQ_INF_NS 0xFFFFFFFFFFFFFFFFULL

static bool kq_access_ok(a32_ctx_t *ctx, uint32_t va, uint32_t len,
    uint32_t prot)
{
    return len == 0U || a32_guest_range_ok(ctx, va, len, prot);
}

static void kq_fail(a32_ctx_t *ctx)
{
    a32_ios_bsd_err(ctx, a32_ios_host_to_darwin_errno(errno));
}

/* Portable kqueue: 8 kqueues x 32 knotes, real host pipe fds.
 * Filters are keyed by (ident, filter) so EVFILT_READ and EVFILT_WRITE
 * on the same fd coexist without overwriting each other. */
typedef struct {
    bool used; bool oneshot; bool disabled;
    bool clear; bool dispatch; bool armed_seen;
    int16_t filter; /* KQ_EV_READ / KQ_EV_WRITE / KQ_EV_TIMER */
    uint32_t ident; /* fd for READ/WRITE, timer id for TIMER */
    uint32_t fflags; uint64_t udata; uint64_t deadline_ns; uint64_t period_ns;
    int32_t last_data;
} kq_knote_t;
typedef struct {
    bool used; int rfd; int wfd; /* rfd is the kqueue fd itself */
    kq_knote_t notes[KQ_KNOTE_MAX];
} kq_kq_t;

static kq_kq_t g_kq[KQ_MAX]; /* +1 pipe-backed fd per kqueue */
/* Plain spinlock (not pthread): the fork child resets it without
 * touching another thread's lock state. */
static _Atomic uint32_t g_kq_mu = 0U;

static uint64_t kq_now_ns(void)
{
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static kq_kq_t *kq_alloc(void) /* reclaims pipes closed behind us */
{
    uint32_t i;
    for (i = 0U; i < KQ_MAX; i++) {
        if (!g_kq[i].used) { return &g_kq[i]; }
        if (fcntl(g_kq[i].rfd, F_GETFD) < 0 && errno == EBADF) {
            (void)close(g_kq[i].wfd);
            memset(&g_kq[i], 0, sizeof(g_kq[i]));
            return &g_kq[i];
        }
    }
    return NULL;
}

/* Wake a poll() sleeping in kq_do_kevent. The wfd is non-blocking, so
 * a full pipe (poll already awake) is a harmless EAGAIN. Call with the
 * kq lock held. */
static void kq_wake(const kq_kq_t *kq)
{
    ssize_t w;
    if (kq == NULL || kq->wfd < 0) { return; }
    w = write(kq->wfd, "", 1);
    (void)w;
}

void a32_ios_kq_close(int fd)
{
    uint32_t i;
    if (fd < 0) {
        return;
    }
    a32_spin_lock(&g_kq_mu);
    for (i = 0U; i < KQ_MAX; i++) {
        if (g_kq[i].used && g_kq[i].rfd == fd) {
            if (g_kq[i].wfd >= 0 && g_kq[i].wfd != fd) {
                kq_wake(&g_kq[i]);
                (void)close(g_kq[i].wfd);
            }
            memset(&g_kq[i], 0, sizeof(g_kq[i]));
        }
    }
    a32_spin_unlock(&g_kq_mu);
}

void a32_ios_kq_atfork_reset(void)
{
    /* Only the forking thread survives; tracked fds are inherited and
     * stay valid, so only the lock state is reinitialized. */
    atomic_store_explicit(&g_kq_mu, 0U, memory_order_release);
}

/* Mutate one knote; returns a Darwin errno (0 on success). Call with
 * the kq lock held; wakes a sleeper so the change takes effect now. */
static int kq_knote_change(kq_kq_t *kq, int16_t filter, uint16_t flags,
    uint32_t fflags, int64_t data, uint32_t ident, uint64_t udata)
{
    kq_knote_t *kn = NULL;
    kq_knote_t *slot = NULL;
    uint32_t i;
    uint64_t period = 0U, mult = 1000000ULL, now;
    if (filter != (int16_t)KQ_EV_READ && filter != (int16_t)KQ_EV_WRITE &&
            filter != (int16_t)KQ_EV_TIMER) { return KQ_EINVAL; }
    for (i = 0U; i < KQ_KNOTE_MAX; i++) {
        if (kq->notes[i].used) {
            if (kq->notes[i].filter == filter && kq->notes[i].ident == ident) {
                kn = &kq->notes[i];
                break;
            }
        } else if (slot == NULL) {
            slot = &kq->notes[i];
        }
    }
    if ((flags & KQ_EV_DELETE) != 0U) {
        if (kn == NULL) { return KQ_ENOENT; }
        memset(kn, 0, sizeof(*kn));
        kq_wake(kq);
        return 0;
    }
    if ((flags & KQ_EV_ADD) == 0U) {
        if (kn == NULL) { return KQ_ENOENT; }
        if ((flags & KQ_EV_DISABLE) != 0U) { kn->disabled = true; }
        if ((flags & KQ_EV_ENABLE) != 0U) {
            kn->disabled = false;
            kn->armed_seen = false;
            kn->last_data = 0;
        }
        kq_wake(kq);
        return 0;
    }
    if (filter == (int16_t)KQ_EV_TIMER) {
        uint32_t unit;
        if ((fflags & 0x8U) != 0U || (fflags & 0x10U) != 0U) {
            return KQ_EINVAL; /* NOTE_ABSOLUTE / NOTE_MACHTIME */
        }
        unit = fflags & 0x7U;
        if (unit == 1U) {
            mult = 1000000000ULL; /* NOTE_SECONDS */
        } else if (unit == 2U) {
            mult = 1000ULL; /* NOTE_USECONDS */
        } else if (unit == 4U) {
            mult = 1ULL; /* NOTE_NSECONDS */
        } else if (unit == 0U) {
            mult = 1000000ULL; /* default: milliseconds */
        } else {
            return KQ_EINVAL;
        }
        if (data > 0) {
            period = (uint64_t)data > 0xFFFFFFFFFFFFFFFFULL / mult ?
                0xFFFFFFFFFFFFFFFFULL : (uint64_t)data * mult;
        }
    }
    if (kn == NULL) {
        if (slot == NULL) { return KQ_ENOMEM; }
        kn = slot;
        memset(kn, 0, sizeof(*kn));
        kn->used = true;
        kn->filter = filter;
        kn->ident = ident;
    }
    kn->fflags = fflags;
    kn->udata = udata;
    kn->disabled = (flags & KQ_EV_DISABLE) != 0U;
    if ((flags & KQ_EV_ENABLE) != 0U) { kn->disabled = false; }
    kn->oneshot = (flags & KQ_EV_ONESHOT) != 0U; /* one-shot or periodic */
    kn->clear = (flags & KQ_EV_CLEAR) != 0U;
    kn->dispatch = (flags & KQ_EV_DISPATCH) != 0U;
    kn->armed_seen = false;
    kn->last_data = 0;
    if (filter != (int16_t)KQ_EV_TIMER) {
        kq_wake(kq);
        return 0;
    }
    kn->period_ns = ((flags & KQ_EV_ONESHOT) != 0U) ? 0U : period;
    now = kq_now_ns();
    kn->deadline_ns = period > 0xFFFFFFFFFFFFFFFFULL - now ?
        0xFFFFFFFFFFFFFFFFULL : now + period;
    kq_wake(kq);
    return 0;
}

/* Readiness probe (zero-timeout poll, never blocks). A bad fd reports
 * through *out_ebadf (guest sees EV_ERROR/data=EBADF); a hangup/error
 * reports through *out_eof (guest sees EV_EOF, no EV_ERROR). */
static bool kq_knote_ready(const kq_knote_t *kn, int64_t *data,
    bool *out_eof, bool *out_ebadf)
{
    struct pollfd pfd = { (int)kn->ident,
        (short)(kn->filter == (int16_t)KQ_EV_READ ? POLLIN : POLLOUT), 0 };
    *out_eof = false;
    *out_ebadf = false;
    *data = 1;
    if (kn->filter == (int16_t)KQ_EV_TIMER) {
        return kq_now_ns() >= kn->deadline_ns;
    }
    if (poll(&pfd, 1, 0) < 0 || (pfd.revents & POLLNVAL) != 0) {
        *out_ebadf = true;
        return true;
    }
    if ((pfd.revents & (POLLHUP | POLLERR)) != 0) {
        *out_eof = true;
        if (kn->filter == (int16_t)KQ_EV_READ) {
            int n = 0;
            *data = 0;
            if (ioctl((int)kn->ident, FIONREAD, &n) == 0 && n > 0) {
                *data = (int64_t)n;
            }
        }
        return true;
    }
    if ((pfd.revents & pfd.events) == 0) { return false; }
    if (kn->filter == (int16_t)KQ_EV_READ) {
        int n = 0;
        *data = 1;
        if (ioctl((int)kn->ident, FIONREAD, &n) == 0 && n > 0) {
            *data = (int64_t)n;
        }
    }
    return true;
}

/* 20-byte kevent / 72-byte kevent_qos_s / 48-byte kevent64
 * (4-byte aligned, packed). The 72-byte entry is the exact XNU
 * `#pragma pack(4)` struct kevent_qos_s layout. */
typedef struct __attribute__((packed, aligned(4))) {
    uint32_t ident; int16_t filter; uint16_t flags; uint32_t fflags;
    int32_t data; uint32_t udata;
} kq_ev32_t;
typedef struct __attribute__((packed, aligned(4))) {
    uint64_t ident;
    int16_t filter;
    uint16_t flags;
    int32_t qos;
    uint64_t udata;
    uint32_t fflags;
    uint32_t xflags;
    int64_t data;
    uint64_t ext[4];
} kq_evqos_t;
typedef struct __attribute__((packed, aligned(4))) {
    uint64_t ident; int16_t filter; uint16_t flags; uint32_t fflags;
    int64_t data; uint64_t udata; uint64_t ext[2];
} kq_ev64_t;

_Static_assert(sizeof(kq_ev32_t) == 20, "kevent must be 20 bytes");
_Static_assert(sizeof(kq_evqos_t) == 72, "kevent_qos_s must be 72 bytes");
_Static_assert(sizeof(kq_ev64_t) == 48, "kevent64 must be 48 bytes");

static void kq_emit(a32_ctx_t *ctx, uint32_t base, uint32_t idx, uint32_t esz,
    uint64_t ident, int16_t filter, uint16_t flags, uint32_t fflags,
    int64_t data, uint64_t udata)
{
    if (esz == 20U) {
        kq_ev32_t v = { (uint32_t)ident, filter, flags, fflags,
            (int32_t)data, (uint32_t)udata };
        memcpy(a32_guest_ptr(ctx, base + idx * 20U), &v, 20);
        return;
    }
    if (esz == 48U) {
        kq_ev64_t v = { ident, filter, flags, fflags, data, udata,
            { 0U, 0U } };
        memcpy(a32_guest_ptr(ctx, base + idx * 48U), &v, 48);
        return;
    }
    if (esz == 72U) {
        kq_evqos_t v = { ident, filter, flags, 0, udata, fflags, 0U,
            data, { 0U, 0U, 0U, 0U } };
        memcpy(a32_guest_ptr(ctx, base + idx * 72U), &v, 72);
    }
}

/* Shared kevent/kevent64/kevent_qos scan; esz is the guest entry stride
 * (20 for kevent, 72 for kevent_qos_s, 48 for kevent64). Ready knotes
 * are scanned before sleeping so an already-ready event never incurs
 * a blocking wait; otherwise poll (in 60 s slices, woken at once by the
 * rfd pipe on any concurrent change or close) runs until user_deadline.
 * A NULL timeout arrives as KQ_INF_NS. */
static void kq_do_kevent(a32_ctx_t *ctx, int kd, uint32_t chl, int nch,
    uint32_t evl, int nev, uint64_t timeout_ns, uint32_t esz)
{
    kq_kq_t *kq = NULL;
    uint32_t i, nout = 0U;
    struct pollfd pfds[KQ_KNOTE_MAX + 1U];
    uint64_t start_ns, user_deadline, wait_ns, now;
    uint64_t kfds;
    bool has_timer, use_cap;
    nfds_t nfds;
    int wait_ms;
    a32_spin_lock(&g_kq_mu);
    for (i = 0U; i < KQ_MAX; i++) {
        if (g_kq[i].used && g_kq[i].rfd == kd) { kq = &g_kq[i]; break; }
    }
    if (kq == NULL) {
        a32_spin_unlock(&g_kq_mu);
        a32_ios_bsd_err(ctx, KQ_EBADF);
        return;
    }
    if (nch < 0 || nev < 0 || (uint32_t)nch > KQ_EV_CAP ||
            (uint32_t)nev > KQ_EV_CAP) {
        a32_spin_unlock(&g_kq_mu);
        a32_ios_bsd_err(ctx, KQ_EINVAL);
        return;
    }
    if ((nch > 0 && !kq_access_ok(ctx, chl, (uint32_t)nch * esz,
                (uint32_t)PROT_READ)) ||
            (nev > 0 && !kq_access_ok(ctx, evl, (uint32_t)nev * esz,
                (uint32_t)PROT_WRITE))) {
        a32_spin_unlock(&g_kq_mu);
        a32_ios_bsd_err(ctx, KQ_EFAULT);
        return;
    }
    for (i = 0U; i < (uint32_t)nch; i++) {
        const uint8_t *e = (const uint8_t *)a32_guest_ptr(ctx, chl + i * esz);
        uint64_t ident, udata;
        int64_t data;
        int16_t filter;
        uint16_t flags;
        uint32_t fflags, v32;
        int32_t d32;
        int crc;
        if (esz == 20U) {
            memcpy(&v32, e, 4);
            ident = v32;
            memcpy(&filter, e + 4U, 2);
            memcpy(&flags, e + 6U, 2);
            memcpy(&fflags, e + 8U, 4);
            memcpy(&d32, e + 12U, 4);
            data = d32;
            memcpy(&v32, e + 16U, 4);
            udata = v32;
        } else if (esz == 72U) {
            kq_evqos_t q;
            memcpy(&q, e, 72);
            ident = q.ident;
            filter = q.filter;
            flags = q.flags;
            fflags = q.fflags;
            data = q.data;
            udata = q.udata;
        } else {
            memcpy(&ident, e, 8);
            memcpy(&filter, e + 8U, 2);
            memcpy(&flags, e + 10U, 2);
            memcpy(&fflags, e + 12U, 4);
            memcpy(&data, e + 16U, 8);
            memcpy(&udata, e + 24U, 8);
        }
        crc = kq_knote_change(kq, filter, flags, fflags, data,
            (uint32_t)ident, udata);
        if (nev > 0 && nout < (uint32_t)nev &&
                (((flags & KQ_EV_RECEIPT) != 0U) || crc != 0)) {
            kq_emit(ctx, evl, nout++, esz, ident, filter,
                (uint16_t)(KQ_EV_ERROR | (flags & KQ_EV_RECEIPT)), 0U,
                (int64_t)crc, udata);
        }
    }
    if (nout > 0U) {
        /* Receipts only: report immediately, never block. */
        a32_vm32_copyout_invalidate_exec(ctx, evl, nout * esz);
        a32_spin_unlock(&g_kq_mu);
        a32_ios_bsd_ok(ctx, nout, 0U, false);
        return;
    }
    if (nev == 0) {
        a32_spin_unlock(&g_kq_mu);
        a32_ios_bsd_ok(ctx, 0U, 0U, false);
        return;
    }
    start_ns = kq_now_ns();
    user_deadline = timeout_ns == KQ_INF_NS ? KQ_INF_NS :
        (timeout_ns > KQ_INF_NS - start_ns ? KQ_INF_NS :
         start_ns + timeout_ns);
    for (;;) {
        /* Scan ready knotes first: already-ready events never block. */
        for (i = 0U; i < KQ_KNOTE_MAX && nout < (uint32_t)nev; i++) {
            kq_knote_t *kn = &kq->notes[i];
            kq_knote_t ev;
            int64_t rdata;
            bool reof, rebadf;
            if (!kn->used || kn->disabled) { continue; }
            if (!kq_knote_ready(kn, &rdata, &reof, &rebadf)) {
                if (kn->clear && kn->filter != (int16_t)KQ_EV_TIMER) {
                    kn->armed_seen = false;
                    kn->last_data = 0;
                }
                continue;
            }
            if (rebadf) {
                ev = *kn;
                kq_emit(ctx, evl, nout, esz, (uint64_t)ev.ident,
                    ev.filter, KQ_EV_ERROR, ev.fflags, KQ_EBADF,
                    ev.udata);
                nout++;
                continue;
            }
            if (kn->clear && kn->filter != (int16_t)KQ_EV_TIMER &&
                    !reof && kn->armed_seen &&
                    rdata == (int64_t)kn->last_data) {
                continue; /* edge-triggered: unchanged since delivery */
            }
            ev = *kn;
            if (ev.filter == (int16_t)KQ_EV_TIMER) {
                kn->deadline_ns = kq_now_ns() + ev.period_ns;
                if (ev.period_ns == 0U) { memset(kn, 0, sizeof(*kn)); }
            } else if (ev.oneshot) {
                memset(kn, 0, sizeof(*kn));
            }
            if (kn->used && kn->clear &&
                    kn->filter != (int16_t)KQ_EV_TIMER && !reof) {
                kn->armed_seen = true;
                kn->last_data = (int32_t)rdata;
            }
            kq_emit(ctx, evl, nout, esz, (uint64_t)ev.ident, ev.filter,
                reof ? KQ_EV_EOF : 0U, ev.fflags, rdata, ev.udata);
            if (kn->used && kn->dispatch) { kn->disabled = true; }
            nout++;
        }
        if (nout > 0U) { break; }
        /* Nothing ready: how long may we sleep? */
        now = kq_now_ns();
        if (user_deadline != KQ_INF_NS && now >= user_deadline) { break; }
        has_timer = false;
        kfds = 0U;
        for (i = 0U; i < KQ_KNOTE_MAX; i++) {
            if (!kq->notes[i].used || kq->notes[i].disabled) { continue; }
            if (kq->notes[i].filter == (int16_t)KQ_EV_TIMER) {
                has_timer = true;
            } else {
                kfds++;
            }
        }
        use_cap = timeout_ns == KQ_INF_NS && !has_timer && kfds == 0U;
        if (use_cap && now - start_ns >= 60000000000ULL) { break; }
        wait_ns = user_deadline == KQ_INF_NS ? KQ_INF_NS :
            user_deadline - now;
        if (use_cap) {
            uint64_t left = 60000000000ULL - (now - start_ns);
            if (left < wait_ns) { wait_ns = left; }
        }
        for (i = 0U; i < KQ_KNOTE_MAX; i++) {
            if (!kq->notes[i].used || kq->notes[i].disabled ||
                    kq->notes[i].filter != (int16_t)KQ_EV_TIMER) {
                continue;
            }
            if (kq->notes[i].deadline_ns <= now) {
                wait_ns = 0U;
                break;
            }
            if (kq->notes[i].deadline_ns - now < wait_ns) {
                wait_ns = kq->notes[i].deadline_ns - now;
            }
        }
        if (wait_ns == 0U) { continue; } /* re-scan: timer just fired */
        /* rfd first: a concurrent change/close wakes poll at once.
         * Unchanged EV_CLEAR notes stay out so poll blocks instead of
         * spinning on their level-triggered readiness. */
        nfds = 1U;
        pfds[0].fd = kq->rfd;
        pfds[0].events = POLLIN;
        pfds[0].revents = 0;
        for (i = 0U; i < KQ_KNOTE_MAX && nfds <= KQ_KNOTE_MAX; i++) {
            kq_knote_t *kn = &kq->notes[i];
            int64_t rdata;
            bool reof, rebadf;
            if (!kn->used || kn->disabled ||
                    kn->filter == (int16_t)KQ_EV_TIMER) {
                continue;
            }
            if (kn->clear && kn->armed_seen &&
                    kq_knote_ready(kn, &rdata, &reof, &rebadf) &&
                    !reof && !rebadf &&
                    rdata == (int64_t)kn->last_data) {
                continue;
            }
            pfds[nfds].fd = (int)kn->ident;
            pfds[nfds].events = (kn->filter == (int16_t)KQ_EV_WRITE) ?
                POLLOUT : POLLIN;
            pfds[nfds].revents = 0;
            nfds++;
        }
        wait_ms = wait_ns >= 60000000000ULL ? KQ_WAIT_CAP_MS :
            (int)((wait_ns + 999999ULL) / 1000000ULL);
        a32_spin_unlock(&g_kq_mu);
        {
            int prc = poll(pfds, nfds, wait_ms);
            int perr = prc < 0 ? errno : 0;
            if (prc > 0 && (pfds[0].revents & POLLIN) != 0) {
                char tmp[64];
                ssize_t z;
                do { z = read(pfds[0].fd, tmp, sizeof(tmp)); } while (z > 0);
            }
            a32_spin_lock(&g_kq_mu);
            kq = NULL;
            for (i = 0U; i < KQ_MAX; i++) {
                if (g_kq[i].used && g_kq[i].rfd == kd) {
                    kq = &g_kq[i];
                    break;
                }
            }
            if (kq == NULL) {
                a32_spin_unlock(&g_kq_mu);
                a32_ios_bsd_err(ctx, KQ_EBADF);
                return;
            }
            if (prc < 0 && perr == EINTR) {
                a32_spin_unlock(&g_kq_mu);
                a32_ios_bsd_err(ctx, 4 /* EINTR */);
                return;
            }
        }
    }
    a32_vm32_copyout_invalidate_exec(ctx, evl, nout * esz);
    a32_spin_unlock(&g_kq_mu);
    a32_ios_bsd_ok(ctx, nout, 0U, false);
}

static void kq_do_kqueue(a32_ctx_t *ctx)
{
    kq_kq_t *kq;
    int fds[2];
    a32_spin_lock(&g_kq_mu);
    kq = kq_alloc();
    if (kq == NULL) {
        a32_spin_unlock(&g_kq_mu);
        a32_ios_bsd_err(ctx, KQ_ENOMEM);
        return;
    }
    if (pipe(fds) != 0) {
        a32_spin_unlock(&g_kq_mu);
        kq_fail(ctx);
        return;
    }
    /* Non-blocking so kq_wake never stalls a changer and the drain
     * read in kq_do_kevent never blocks; close-on-exec for hygiene. */
    (void)fcntl(fds[0], F_SETFL, O_NONBLOCK);
    (void)fcntl(fds[1], F_SETFL, O_NONBLOCK);
    (void)fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    (void)fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    memset(kq, 0, sizeof(*kq));
    kq->used = true;
    kq->rfd = fds[0];
    kq->wfd = fds[1];
    a32_spin_unlock(&g_kq_mu);
    a32_ios_bsd_ok(ctx, (uint32_t)fds[0], 0U, false);
}

/* Read a guest timespec; NULL means infinite (KQ_INF_NS). */
static int kq_timeout(a32_ctx_t *ctx, uint32_t ts_va, uint64_t *out_ns)
{
    int32_t sec = 0, nsec = 0;
    if (ts_va == 0U) {
        *out_ns = KQ_INF_NS;
        return 0;
    }
    if (!kq_access_ok(ctx, ts_va, 8U, (uint32_t)PROT_READ)) {
        return -KQ_EFAULT;
    }
    memcpy(&sec, a32_guest_ptr(ctx, ts_va), 4);
    memcpy(&nsec, a32_guest_ptr(ctx, ts_va + 4U), 4);
    if (sec < 0 || nsec < 0 || nsec >= 1000000000) {
        return -KQ_EINVAL;
    }
    *out_ns = (uint64_t)sec * 1000000000ULL + (uint64_t)nsec;
    return 0;
}

int a32_ios_dispatch_bsd_kevent(a32_ctx_t *ctx, uint32_t bsd_nr,
    const uint32_t *args, bool *out_handled)
{
    if (out_handled != NULL) {
        *out_handled = false;
    }
    if (ctx == NULL || args == NULL) {
        return 0;
    }
    switch (bsd_nr) {
    case 362U:
        kq_do_kqueue(ctx);
        break;
    case 363U: {
        uint64_t tns = KQ_INF_NS;
        int trc = kq_timeout(ctx, args[5], &tns);
        if (trc != 0) {
            a32_ios_bsd_err(ctx, -trc);
            break;
        }
        kq_do_kevent(ctx, (int)(int32_t)args[0], args[1],
            (int)(int32_t)args[2], args[3], (int)(int32_t)args[4], tns, 20U);
        break;
    }
    case 369U: {
        /* kevent64 (kq, changelist, nchanges, eventlist, nevents,
         * flags, timeout): the timeout sits one slot later than kevent. */
        uint64_t tns = KQ_INF_NS;
        int trc = kq_timeout(ctx, args[6], &tns);
        if (trc != 0) {
            a32_ios_bsd_err(ctx, -trc);
            break;
        }
        kq_do_kevent(ctx, (int)(int32_t)args[0], args[1],
            (int)(int32_t)args[2], args[3], (int)(int32_t)args[4], tns, 48U);
        break;
    }
    case 374U: {
        /* kevent_qos (kq, changelist, nchanges, eventlist, nevents,
         * data_out, data_available, flags): no timespec. Probes succeed;
         * anything else scans the 72B kevent_qos_s entries without
         * blocking. */
        if ((int)(int32_t)args[2] == 0 && (int)(int32_t)args[4] == 0) {
            a32_ios_bsd_ok(ctx, 0U, 0U, false);
            break;
        }
        kq_do_kevent(ctx, (int)(int32_t)args[0], args[1],
            (int)(int32_t)args[2], args[3], (int)(int32_t)args[4], 0U, 72U);
        break;
    }
    default:
        return 0;
    }
    if (out_handled != NULL) {
        *out_handled = true;
    }
    return 0;
}
