/*
 * src/ios/ios_threads.c — XNU thread/sync BSD + pthread libSystem stubs
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * 64-slot thread table, TSD/key table, guest mutex/cond/once table,
 * spawn_child, bsdthread/psynch/ulock dispatch, synthetic _pthread_*
 * stubs (svc #0x81 ids 88..105, 0xFC, 0xFF). Curated C11, no VLAs.
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
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#define RD PROT_READ
#define WR PROT_WRITE
#define DE_PERM 1
#define DE_SRCH 3
#define DE_AGAIN 35
#define DE_NOMEM 12
#define DE_BUSY 16
#define DE_INVAL 22
#define DE_TIMEDOUT 60
#define DE_DEADLK 11
#define MX_SIG_INIT 0x32AAABA7U
#define MX_SIG_ERRCHK 0x32AAABA1U
#define MX_SIG_REC 0x32AAABA2U
#define CV_SIG_INIT 0x3CB0B1BBU
#define THR_EXIT_ID 0xFFU
#define ONCE_RESUME_ID 0xFCU
/* 64-slot thread table; slot 0 is main (pva 0xFFFF5400). */
#define THR_N 64U
#define THR_STACK_SZ (1024U * 1024U)
typedef struct {
    bool used, done, detached, joinable, joining;
    uint32_t pva, stva, stlen, tsd, rv, dtor_n;
    a32_ctx_t *ch; pthread_t ht;
} thr_t;
static thr_t g_thr[THR_N];
static pthread_mutex_t g_thr_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_thr_cv = PTHREAD_COND_INITIALIZER;
static thr_t *thr_by_pthread(uint32_t pva)
{
    uint32_t i;
    for (i = 0U; i < THR_N; i++)
        if (g_thr[i].used && g_thr[i].pva == pva)
            return &g_thr[i];
    return NULL;
}
static thr_t *thr_by_ctx(a32_ctx_t *c)
{
    uint32_t i, idx;
    if (c != NULL && c->ios_thr_slot_plus1 != 0U) {
        idx = c->ios_thr_slot_plus1 - 1U;
        if (idx < THR_N && g_thr[idx].used && g_thr[idx].ch == c)
            return &g_thr[idx];
    }
    for (i = 0U; i < THR_N; i++)
        if (g_thr[i].used && g_thr[i].ch == c)
            return &g_thr[i];
    return NULL;
}
/* Per-thread TSD: 64 keys x 64 slots; main thread uses slot 0. */
#define KEY_N 64U
static bool g_key_used[KEY_N];
static uint32_t g_key_dtor[KEY_N];
static void *g_tsd[KEY_N][THR_N];
static pthread_mutex_t g_key_mu = PTHREAD_MUTEX_INITIALIZER;
static void tsd_clear(uint32_t slot)
{
    uint32_t k;
    if (slot >= THR_N)
        return;
    for (k = 0U; k < KEY_N; k++)
        g_tsd[k][slot] = NULL;
}
/* Guest mutex table: host objects keyed by guest VA, never written back. */
#define MX_N 64U
typedef struct { bool used, inited, rec; uint32_t gva; uint32_t waiters; pthread_mutex_t mu; } mxe_t;
static mxe_t g_mx[MX_N];
static pthread_mutex_t g_mx_mu = PTHREAD_MUTEX_INITIALIZER;
static mxe_t *mx_lazy(a32_ctx_t *c, uint32_t gva, bool want_rec)
{
    mxe_t *e = NULL, *fr = NULL;
    pthread_mutexattr_t at;
    uint32_t i, sig = 1U;
    if (c == NULL || gva == 0U || !a32_guest_range_ok(c, gva, 4U, (uint32_t)RD))
        return NULL;
    memcpy(&sig, a32_guest_ptr(c, gva), 4);
    if (sig != 0U && sig != MX_SIG_INIT && sig != MX_SIG_ERRCHK && sig != MX_SIG_REC)
        return NULL;
    want_rec = want_rec || sig == MX_SIG_REC;
    (void)pthread_mutex_lock(&g_mx_mu);
    for (i = 0U; i < MX_N; i++)
        if (g_mx[i].used) {
            if (g_mx[i].gva == gva) { e = &g_mx[i]; break; }
        } else if (fr == NULL && g_mx[i].waiters == 0U) fr = &g_mx[i];
    if (e == NULL && fr == NULL) { (void)pthread_mutex_unlock(&g_mx_mu); return NULL; }
    if (e == NULL) { fr->used = true; fr->gva = gva; e = fr; }
    if (!e->inited || (want_rec && !e->rec)) {
        if (e->inited)
            (void)pthread_mutex_destroy(&e->mu);
        if (want_rec) {
            (void)pthread_mutexattr_init(&at);
            (void)pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE);
            (void)pthread_mutex_init(&e->mu, &at);
            (void)pthread_mutexattr_destroy(&at);
        } else (void)pthread_mutex_init(&e->mu, NULL);
        e->inited = true;
        e->rec = want_rec;
    }
    (void)pthread_mutex_unlock(&g_mx_mu);
    return e;
}
static bool mx_remove(uint32_t gva)
{
    uint32_t i;
    bool hit = false;
    (void)pthread_mutex_lock(&g_mx_mu);
    for (i = 0U; i < MX_N; i++) {
        if (g_mx[i].used && g_mx[i].gva == gva) {
            g_mx[i].used = false;
            g_mx[i].gva = 0U;
            hit = true;
        }
    }
    (void)pthread_mutex_unlock(&g_mx_mu);
    return hit;
}
typedef struct { bool used, inited; uint32_t gva; uint32_t waiters; pthread_cond_t cv; } cve_t;
static cve_t g_cv[MX_N];
static pthread_mutex_t g_cv_mu = PTHREAD_MUTEX_INITIALIZER;
static cve_t *cv_lazy(a32_ctx_t *c, uint32_t gva)
{
    cve_t *e = NULL, *fr = NULL;
    uint32_t i, sig = 1U;
    if (c == NULL || gva == 0U || !a32_guest_range_ok(c, gva, 4U, (uint32_t)RD))
        return NULL;
    memcpy(&sig, a32_guest_ptr(c, gva), 4);
    if (sig != 0U && sig != CV_SIG_INIT)
        return NULL;
    (void)pthread_mutex_lock(&g_cv_mu);
    for (i = 0U; i < MX_N; i++)
        if (g_cv[i].used) {
            if (g_cv[i].gva == gva) { e = &g_cv[i]; break; }
        } else if (fr == NULL && g_cv[i].waiters == 0U) fr = &g_cv[i];
    if (e == NULL && fr == NULL) { (void)pthread_mutex_unlock(&g_cv_mu); return NULL; }
    if (e == NULL) {
        fr->used = true; fr->gva = gva;
        if (!fr->inited) { (void)pthread_cond_init(&fr->cv, NULL); fr->inited = true; }
        e = fr;
    }
    (void)pthread_mutex_unlock(&g_cv_mu);
    return e;
}
static bool cv_remove(uint32_t gva)
{
    uint32_t i;
    bool hit = false;
    (void)pthread_mutex_lock(&g_cv_mu);
    for (i = 0U; i < MX_N; i++) {
        if (g_cv[i].used && g_cv[i].gva == gva) {
            g_cv[i].used = false;
            g_cv[i].gva = 0U;
            hit = true;
        }
    }
    (void)pthread_mutex_unlock(&g_cv_mu);
    return hit;
}
/* Static initializers: a guest word holding a valid _PTHREAD_*_SIG_init
 * signature (or zero) materializes the backing host object on first use. */
static bool thr_lazy_sig(a32_ctx_t *c, uint32_t gva, uint32_t *out)
{
    if (c == NULL || gva == 0U || !a32_guest_range_ok(c, gva, 4U, (uint32_t)RD))
        return false;
    memcpy(out, a32_guest_ptr(c, gva), 4);
    return true;
}
/* Guest once table + per-host-thread in-flight once-call stack. */
typedef struct { bool used, running, done; uint32_t gva; } oncee_t;
static oncee_t g_once[MX_N];
static pthread_mutex_t g_once_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_once_cv = PTHREAD_COND_INITIALIZER;
static oncee_t *once_lookup(uint32_t gva, bool create)
{
    uint32_t i;
    oncee_t *fr = NULL;
    (void)pthread_mutex_lock(&g_once_mu);
    for (i = 0U; i < MX_N; i++) {
        if (g_once[i].used) {
            if (g_once[i].gva == gva) {
                (void)pthread_mutex_unlock(&g_once_mu);
                return &g_once[i];
            }
        } else if (fr == NULL && create) {
            fr = &g_once[i];
        }
    }
    if (fr != NULL) {
        fr->used = true;
        fr->running = false;
        fr->done = false;
        fr->gva = gva;
    }
    (void)pthread_mutex_unlock(&g_once_mu);
    return fr;
}
typedef struct {
    uint32_t depth;
    struct { uint32_t gva, cpsr, regs[16]; } st[8];
} ocall_t;
static pthread_key_t g_oc_key;
static pthread_once_t g_oc_once = PTHREAD_ONCE_INIT;
static void oc_free(void *p) { free(p); }
static void oc_make(void) { (void)pthread_key_create(&g_oc_key, oc_free); }
static ocall_t *oc_self(void)
{
    ocall_t *o;
    (void)pthread_once(&g_oc_once, oc_make);
    o = (ocall_t *)pthread_getspecific(g_oc_key);
    if (o == NULL) {
        o = (ocall_t *)calloc(1, sizeof(*o));
        (void)pthread_setspecific(g_oc_key, o);
    }
    return o;
}
static void *ios_child_entry(void *arg);
typedef struct {
    a32_ctx_t *ch; thr_t *slot; bool set_port_r1; volatile int started;
} spawn_t;
static void *ios_child_entry(void *arg)
{
    spawn_t *sp = (spawn_t *)arg;
    a32_ctx_t *ch = sp->ch;
    thr_t *slot = sp->slot;
    uint32_t rv; bool det; uint32_t st_va, st_sz, tl_va, idx;
    a32_signals_set_thread_ctx(ch);
    ch->host_tid = 0;
    (void)get_host_tid(ch);
    if (sp->set_port_r1)
        ch->regs[1] = 0x203U + ((uint32_t)(ch->host_tid & 0xFF) * 4U);
    (void)pthread_mutex_lock(&g_thr_mu);
    slot->ht = pthread_self();
    (void)pthread_mutex_unlock(&g_thr_mu);
    __atomic_store_n(&sp->started, 1, __ATOMIC_SEQ_CST);
    (void)a32_jit_run(ch);
    rv = ch->regs[0];
    (void)pthread_mutex_lock(&g_thr_mu);
    slot->rv = rv; slot->done = true; slot->ch = NULL;
    det = slot->detached;
    st_va = slot->stva; st_sz = slot->stlen; tl_va = slot->tsd;
    idx = (uint32_t)(slot - g_thr);
    if (det) { slot->ht = (pthread_t)0; slot->used = false; }
    (void)pthread_cond_broadcast(&g_thr_cv);
    (void)pthread_mutex_unlock(&g_thr_mu);
    a32_signals_set_thread_ctx(NULL);
    tsd_clear(idx);
    if (det) {
        if (st_va != 0U) (void)a32_vm_munmap(ch, st_va, (size_t)st_sz);
        if (tl_va != 0U) (void)a32_vm_munmap(ch, tl_va, 0x1000U);
        free(ch);
        pthread_exit(NULL);
    }
    free(ch);
    return (void *)(uintptr_t)rv;
}
static thr_t *spawn_child(a32_ctx_t *parent, uint32_t entry, uint32_t sp,
    uint32_t tls, const uint32_t rv6[6], bool joinable, uint32_t pthread_va, bool custom_stack)
{
    a32_ctx_t *ch;
    thr_t *slot = NULL;
    spawn_t args;
    pthread_t ht;
    pthread_attr_t at;
    uint32_t i, pva, stva = 0U, stlen = 0U, tsd = tls, zero = 0U, self;
    if (sp == 0U) {
        stva = a32_vm_mmap2(parent, 0U, THR_STACK_SZ,
            (uint32_t)(PROT_READ | PROT_WRITE), 0x22U, -1, 0U);
        if (stva > 0xFFFFF000U)
            return NULL;
        stlen = THR_STACK_SZ;
        sp = stva + stlen;
    }
    if (tsd == 0U) {
        tsd = a32_vm_mmap2(parent, 0U, 0x1000U,
            (uint32_t)(PROT_READ | PROT_WRITE), 0x22U, -1, 0U);
        if (tsd > 0xFFFFF000U) {
            if (stva != 0U)
                (void)a32_vm_munmap(parent, stva, stlen);
            return NULL;
        }
    }
    ch = (a32_ctx_t *)calloc(1, sizeof(*ch));
    if (ch == NULL) {
        if (stva != 0U) (void)a32_vm_munmap(parent, stva, stlen);
        if (tls == 0U && tsd != 0U) (void)a32_vm_munmap(parent, tsd, 0x1000U);
        return NULL;
    }
    a32_vm_sync_ctx(parent);
    memcpy(ch, parent, sizeof(*ch));
    ch->host_tid = 0; ch->regs[13] = sp; ch->tls_tp = tsd;
    ch->pending_signals = 0ULL; ch->clear_child_tid = 0U;
    ch->robust_list_head = 0U; ch->robust_list_len = 0U;
    ch->altstack_sp = 0U; ch->altstack_size = 0U;
    ch->ll_epoch_snap = 0ULL; ch->excl_valid = false; ch->ras_top = 0U;
    memset(ch->ras, 0, sizeof(ch->ras));
    memset(ch->ibtc, 0, sizeof(ch->ibtc));
    for (i = 0U; i < 6U; i++) ch->regs[i] = rv6[i];
    pva = tsd + 0x100U;
    if (!custom_stack) {
        ch->regs[4] = stlen;
        if (pthread_va != 0U) pva = pthread_va;
        ch->regs[0] = pva;
    }
    ch->ios_pthread_va = pva;
    ch->ios_errno_va = tsd + 0x200U;
    ch->itstate = 0U;
    a32_set_pc_interwork(ch, entry);
    ch->regs[13] = sp;
    self = pva;
    (void)guest_write(ch, tsd, &self, 4U);
    (void)guest_write(ch, tsd + 8U, &ch->ios_errno_va, 4U);
    (void)guest_write(ch, ch->ios_errno_va, &zero, 4U);
    (void)pthread_mutex_lock(&g_thr_mu);
    if (!g_thr[0].used) { g_thr[0].used = true; g_thr[0].pva = 0xFFFF5400U; g_thr[0].ch = parent; parent->ios_thr_slot_plus1 = 1U; }
    for (i = 0U; i < THR_N && slot == NULL; i++) {
        if (!g_thr[i].used) {
            slot = &g_thr[i];
            slot->used = true; slot->done = false; slot->joining = false;
            slot->detached = !joinable; slot->joinable = joinable;
            slot->pva = pva; slot->stva = stva; slot->stlen = stlen;
            slot->tsd = (tls == 0U) ? tsd : 0U; slot->rv = 0U; slot->ch = ch;
            slot->ht = (pthread_t)0; slot->dtor_n = 0U;
            ch->ios_thr_slot_plus1 = (uint32_t)(slot - g_thr) + 1U;
        }
    }
    (void)pthread_mutex_unlock(&g_thr_mu);
    if (slot == NULL) {
        if (stva != 0U) (void)a32_vm_munmap(parent, stva, stlen);
        if (tls == 0U && tsd != 0U) (void)a32_vm_munmap(parent, tsd, 0x1000U);
        free(ch);
        return NULL;
    }
    (void)a32_jit_init(ch);
    args.ch = ch; args.slot = slot; args.set_port_r1 = !joinable; args.started = 0;
    (void)pthread_attr_init(&at);
    if (!joinable) (void)pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&ht, &at, ios_child_entry, &args) != 0) {
        pthread_attr_destroy(&at);
        (void)pthread_mutex_lock(&g_thr_mu);
        slot->ch = NULL; slot->used = false;
        (void)pthread_mutex_unlock(&g_thr_mu);
        if (stva != 0U) (void)a32_vm_munmap(parent, stva, stlen);
        if (tls == 0U && tsd != 0U) (void)a32_vm_munmap(parent, tsd, 0x1000U);
        free(ch);
        return NULL;
    }
    pthread_attr_destroy(&at);
    while (__atomic_load_n(&args.started, __ATOMIC_SEQ_CST) == 0) sched_yield();
    return slot;
}
#define SYNC_B 64U
static pthread_mutex_t g_sync_mu[SYNC_B];
static pthread_cond_t g_sync_cv[SYNC_B];
static pthread_once_t g_sync_once = PTHREAD_ONCE_INIT;
static void sync_init_once(void)
{
    for (uint32_t i = 0U; i < SYNC_B; i++) {
        (void)pthread_mutex_init(&g_sync_mu[i], NULL);
        (void)pthread_cond_init(&g_sync_cv[i], NULL);
    }
}
static void sync_ensure(void)
{
    (void)pthread_once(&g_sync_once, sync_init_once);
}
/* Waiter table keyed by guest VA: waiters park a stack node under the bucket
 * lock and sleep until their own woken flag is set, so colliding keys never
 * consume each other's wakeups. wake_all bumps bcast_seq and wakes every
 * matching waiter even when *uaddr is unchanged. */
typedef struct syncw { uint32_t key; bool woken; struct syncw *next; } syncw_t;
static syncw_t *g_syncw[SYNC_B];
static uint64_t g_syncseq[SYNC_B];
static void sync_link(uint32_t b, syncw_t *w) { w->next = g_syncw[b]; g_syncw[b] = w; }
static void sync_unlink(uint32_t b, syncw_t *w)
{
    syncw_t **p = &g_syncw[b];
    while (*p != NULL && *p != w)
        p = &(*p)->next;
    if (*p != NULL)
        *p = w->next;
}
static void sync_kick(uint32_t b, uint32_t key, bool all)
{
    syncw_t *w;
    g_syncseq[b]++;
    for (w = g_syncw[b]; w != NULL; w = w->next)
        if (w->key == key && !w->woken) {
            w->woken = true;
            if (!all)
                break;
        }
    (void)pthread_cond_broadcast(&g_sync_cv[b]);
}
int a32_ios_dispatch_thread_sync_bsd(a32_ctx_t *ctx, uint32_t bsd_nr,
    const uint32_t *args, bool *out_handled)
{
    uint32_t b;
    if (out_handled != NULL)
        *out_handled = false;
    if (ctx == NULL || args == NULL)
        return 0;
    sync_ensure();
    switch (bsd_nr) {
    case 366U:
        ctx->ios_threadstart_va = args[0];
        ctx->ios_wqthread_va = args[1];
        if (out_handled != NULL)
            *out_handled = true;
        a32_ios_bsd_ok(ctx, 0x40000000U, 0U, false);
        return 0;
    case 372U:
        if (out_handled != NULL)
            *out_handled = true;
        a32_ios_bsd_ok(ctx, (uint32_t)get_host_tid(ctx), 0U, true);
        return 0;
    case 360U: {
        uint32_t rv6[6], entry, sp = args[2], custom, ret;
        thr_t *slot;
        if (out_handled != NULL) *out_handled = true;
        custom = ((args[4] & 0x01000000U) != 0U) && sp != 0U;
        entry = (ctx->ios_threadstart_va != 0U) ? ctx->ios_threadstart_va : args[0];
        rv6[0] = custom ? ((args[3] != 0U) ? args[3] : sp) : 0U;
        rv6[1] = 0U; rv6[2] = args[0]; rv6[3] = args[1]; rv6[4] = 0U; rv6[5] = args[4];
        slot = spawn_child(ctx, entry, custom ? sp : 0U, 0U, rv6, false, args[3], custom != 0U);
        if (slot == NULL) { a32_ios_bsd_err(ctx, DE_AGAIN); return 0; }
        if (custom != 0U) { ret = rv6[0]; } else { (void)pthread_mutex_lock(&g_thr_mu); ret = slot->pva; (void)pthread_mutex_unlock(&g_thr_mu); }
        a32_ios_bsd_ok(ctx, ret, 0U, false);
        return 0;
    }
    case 361U: {
        thr_t *self;
        if (out_handled != NULL) *out_handled = true;
        if (args[0] != 0U && args[1] != 0U) (void)a32_vm_munmap(ctx, args[0], (size_t)args[1]);
        (void)pthread_mutex_lock(&g_thr_mu);
        self = thr_by_ctx(ctx);
        if (self != NULL && self != &g_thr[0]) {
            uint32_t st_va = self->stva, st_sz = self->stlen, tl_va = self->tsd;
            uint32_t idx = (uint32_t)(self - g_thr); bool det = self->detached;
            self->done = true; self->ch = NULL;
            if (det) self->used = false;
            (void)pthread_cond_broadcast(&g_thr_cv);
            (void)pthread_mutex_unlock(&g_thr_mu);
            a32_signals_set_thread_ctx(NULL);
            tsd_clear(idx);
            if (det) {
                if (st_va != 0U && st_va != args[0]) (void)a32_vm_munmap(ctx, st_va, (size_t)st_sz);
                if (tl_va != 0U) (void)a32_vm_munmap(ctx, tl_va, 0x1000U);
            }
            free(ctx);
            pthread_exit(NULL);
        }
        (void)pthread_mutex_unlock(&g_thr_mu);
        a32_signals_set_thread_ctx(NULL);
        free(ctx);
        pthread_exit(NULL);
        return 0;
    }
    case 301U:
    case 305U:
    case 306U:
    case 307U: {
        uint32_t key = args[0], cur = args[1];
        syncw_t w;
        b = (key >> 2) & (SYNC_B - 1U);
        if (out_handled != NULL)
            *out_handled = true;
        (void)pthread_mutex_lock(&g_sync_mu[b]);
        if (a32_guest_range_ok(ctx, key, 4U, (uint32_t)RD))
            memcpy(&cur, a32_guest_ptr(ctx, key), 4U);
        w.key = key; w.woken = false; w.next = NULL;
        if (cur == args[1]) {
            sync_link(b, &w);
            while (!w.woken)
                (void)pthread_cond_wait(&g_sync_cv[b], &g_sync_mu[b]);
            sync_unlink(b, &w);
        }
        (void)pthread_mutex_unlock(&g_sync_mu[b]);
        a32_ios_bsd_ok(ctx, 0U, 0U, false);
        return 0;
    }
    case 302U:
    case 304U:
    case 308U:
    case 303U:
        b = (args[0] >> 2) & (SYNC_B - 1U);
        if (out_handled != NULL)
            *out_handled = true;
        (void)pthread_mutex_lock(&g_sync_mu[b]);
        sync_kick(b, args[0], bsd_nr == 303U);
        (void)pthread_mutex_unlock(&g_sync_mu[b]);
        a32_ios_bsd_ok(ctx, 0U, 0U, false);
        return 0;
    case 515U: {
        uint32_t addr = args[1], cur = 0U;
        int rc = 0;
        syncw_t w;
        struct timespec ts;
        b = (addr >> 2) & (SYNC_B - 1U);
        if (out_handled != NULL)
            *out_handled = true;
        if ((addr & 3U) != 0U || !a32_guest_range_ok(ctx, addr, 4U, (uint32_t)RD)) {
            a32_ios_bsd_err(ctx, DE_INVAL);
            return 0;
        }
        if (args[3] != 0U) {
            (void)clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += (time_t)(args[3] / 1000000U);
            ts.tv_nsec += (long)((args[3] % 1000000U) * 1000U);
            if (ts.tv_nsec >= 1000000000L) { ts.tv_sec += 1; ts.tv_nsec -= 1000000000L; }
        }
        (void)pthread_mutex_lock(&g_sync_mu[b]);
        w.key = addr; w.woken = false; w.next = NULL;
        sync_link(b, &w);
        for (;;) {
            if (!a32_guest_range_ok(ctx, addr, 4U, (uint32_t)RD))
                break;
            memcpy(&cur, a32_guest_ptr(ctx, addr), 4U);
            if (cur != args[2] || w.woken)
                break;
            if (args[3] == 0U)
                rc = pthread_cond_wait(&g_sync_cv[b], &g_sync_mu[b]);
            else if ((rc = pthread_cond_timedwait(&g_sync_cv[b], &g_sync_mu[b], &ts)) != 0 && !w.woken)
                break;
            else
                rc = 0;
        }
        sync_unlink(b, &w);
        (void)pthread_mutex_unlock(&g_sync_mu[b]);
        if (rc == ETIMEDOUT)
            a32_ios_bsd_err(ctx, DE_TIMEDOUT);
        else
            a32_ios_bsd_ok(ctx, 0U, 0U, false);
        return 0;
    }
    case 516U:
        b = (args[1] >> 2) & (SYNC_B - 1U);
        if (out_handled != NULL)
            *out_handled = true;
        if ((args[1] & 3U) != 0U ||
            !a32_guest_range_ok(ctx, args[1], 4U, (uint32_t)RD)) {
            a32_ios_bsd_err(ctx, DE_INVAL);
            return 0;
        }
        (void)pthread_mutex_lock(&g_sync_mu[b]);
        sync_kick(b, args[1], (args[0] & 0x00000100U) != 0U);
        (void)pthread_mutex_unlock(&g_sync_mu[b]);
        a32_ios_bsd_ok(ctx, 0U, 0U, false);
        return 0;
    default:
        return 0;
    }
}
static uint32_t mx_rc(int rc)
{
    return (rc == 0) ? 0U : (uint32_t)a32_ios_host_to_darwin_errno(rc);
}

int a32_ios_thread_kill_target(a32_ctx_t *caller, uint32_t target_id, int host_sig)
{
    uint32_t self_port;
    a32_ctx_t *ch;
    pthread_t ht;
    if (caller == NULL) return -1;
    (void)get_host_tid(caller);
    self_port = 0x203U + (((uint32_t)caller->host_tid & 0xFFU) << 2);
    if (target_id == 0U || target_id == self_port || target_id == (uint32_t)caller->host_tid ||
        (caller->tls_tp == (uint32_t)A32_IOS_RUNTIME_DATA_VA &&
         target_id == (uint32_t)(A32_IOS_RUNTIME_DATA_VA + A32_IOS_MAIN_PTHREAD_OFF))) {
        if (host_sig > 0) a32_ios_signals_enqueue_host(caller, host_sig, NULL);
        return 0;
    }
    (void)pthread_mutex_lock(&g_thr_mu);
    for (uint32_t i = 0U; i < THR_N; i++) {
        if (!g_thr[i].used || (ch = g_thr[i].ch) == NULL) continue;
        if (g_thr[i].pva != target_id &&
            (0x203U + (((uint32_t)ch->host_tid & 0xFFU) << 2)) != target_id &&
            (uint32_t)ch->host_tid != target_id) continue;
        ht = g_thr[i].ht;
        if (host_sig > 0) a32_ios_signals_enqueue_host(ch, host_sig, NULL);
        if (ch != caller && host_sig > 0 && ht != (pthread_t)0) (void)pthread_kill(ht, host_sig);
        (void)pthread_mutex_unlock(&g_thr_mu);
        return 0;
    }
    (void)pthread_mutex_unlock(&g_thr_mu); return -1;
}
/* Synthetic _pthread_* stubs (ids 88..105) plus 0xFC/0xFF resumes. */

int a32_ios_stub_pthread(a32_ctx_t *ctx, uint32_t stub_id, bool *out_handled)
{
    uint32_t r0, r1, r2, r3;
    if (out_handled != NULL)
        *out_handled = false;
    if (ctx == NULL)
        return 0;
    r0 = ctx->regs[0];
    r1 = ctx->regs[1];
    r2 = ctx->regs[2];
    r3 = ctx->regs[3];
    switch (stub_id) {
    case 88U: {
        thr_t *t; uint32_t pv;
        (void)pthread_mutex_lock(&g_thr_mu);
        if (!g_thr[0].used) { g_thr[0].used = true; g_thr[0].pva = 0xFFFF5400U; g_thr[0].ch = ctx; ctx->ios_thr_slot_plus1 = 1U; }
        t = thr_by_ctx(ctx); pv = (t != NULL) ? t->pva : ctx->ios_pthread_va;
        (void)pthread_mutex_unlock(&g_thr_mu); ctx->regs[0] = pv; break;
    }
    case 89U: {
        uint32_t rv6[6]; thr_t *slot; uint32_t pva;
        if (r0 == 0U || !a32_guest_range_ok(ctx, r0, 4U, (uint32_t)WR)) {
            ctx->regs[0] = (uint32_t)DE_INVAL; break;
        }
        rv6[0] = r2; rv6[1] = r3; rv6[2] = 0U; rv6[3] = 0U; rv6[4] = 0U; rv6[5] = 0U;
        slot = spawn_child(ctx, A32_IOS_THRTRAMP_VA | 1U, 0U, 0U, rv6, true, 0U, true);
        if (slot == NULL) { ctx->regs[0] = (uint32_t)DE_AGAIN; break; }
        (void)pthread_mutex_lock(&g_thr_mu); pva = slot->pva; (void)pthread_mutex_unlock(&g_thr_mu);
        if (!guest_write(ctx, r0, &pva, 4U)) {
            uint32_t sa = 0U, sl = 0U, ta = 0U, idx = 0U; bool rel = false;
            (void)pthread_mutex_lock(&g_thr_mu);
            if (slot->joinable && slot->ht != (pthread_t)0) (void)pthread_detach(slot->ht);
            slot->detached = true;
            if (slot->done) { sa = slot->stva; sl = slot->stlen; ta = slot->tsd; idx = (uint32_t)(slot - g_thr); slot->ht = (pthread_t)0; slot->used = false; rel = true; }
            (void)pthread_mutex_unlock(&g_thr_mu);
            if (rel) { if (sa != 0U) (void)a32_vm_munmap(ctx, sa, sl); if (ta != 0U) (void)a32_vm_munmap(ctx, ta, 0x1000U); tsd_clear(idx); }
            ctx->regs[0] = (uint32_t)DE_INVAL; break;
        }
        ctx->regs[0] = 0U; break;
    }
    case 90U: {
        thr_t *t, *self; uint32_t rv, idx, sa, sl, ta; a32_ctx_t *lc; pthread_t ht;
        if (r0 != 0U && r0 == ctx->ios_pthread_va) { ctx->regs[0] = (uint32_t)DE_DEADLK; break; }
        (void)pthread_mutex_lock(&g_thr_mu);
        if (!g_thr[0].used && ctx->ios_pthread_va == 0xFFFF5400U) { g_thr[0].used = true; g_thr[0].pva = 0xFFFF5400U; g_thr[0].ch = ctx; ctx->ios_thr_slot_plus1 = 1U; }
        t = thr_by_pthread(r0);
        self = thr_by_ctx(ctx);
        if (t == NULL) { (void)pthread_mutex_unlock(&g_thr_mu); ctx->regs[0] = (uint32_t)DE_SRCH; break; }
        if (self != NULL && t == self) { (void)pthread_mutex_unlock(&g_thr_mu); ctx->regs[0] = (uint32_t)DE_DEADLK; break; }
        if (t->detached || t->joining || t->ht == (pthread_t)0) { (void)pthread_mutex_unlock(&g_thr_mu); ctx->regs[0] = (uint32_t)DE_INVAL; break; }
        t->joining = true; ht = t->ht;
        while (!t->done) (void)pthread_cond_wait(&g_thr_cv, &g_thr_mu);
        (void)pthread_mutex_unlock(&g_thr_mu);
        (void)pthread_join(ht, NULL);
        (void)pthread_mutex_lock(&g_thr_mu);
        rv = t->rv; idx = (uint32_t)(t - g_thr); sa = t->stva; sl = t->stlen; ta = t->tsd; lc = t->ch;
        t->ch = NULL; t->used = false;
        (void)pthread_mutex_unlock(&g_thr_mu);
        if (r1 != 0U && !guest_write(ctx, r1, &rv, 4U)) {
            if (sa != 0U) (void)a32_vm_munmap(ctx, sa, sl);
            if (ta != 0U) (void)a32_vm_munmap(ctx, ta, 0x1000U);
            tsd_clear(idx); free(lc); ctx->regs[0] = (uint32_t)DE_INVAL; break;
        }
        if (sa != 0U) (void)a32_vm_munmap(ctx, sa, sl);
        if (ta != 0U) (void)a32_vm_munmap(ctx, ta, 0x1000U);
        tsd_clear(idx); free(lc); ctx->regs[0] = 0U; break;
    }
    case 91U: {
        thr_t *t; uint32_t sa = 0U, sl = 0U, ta = 0U, idx = 0U; a32_ctx_t *lc = NULL; bool rel = false;
        (void)pthread_mutex_lock(&g_thr_mu);
        t = thr_by_pthread(r0);
        if (t == NULL) { (void)pthread_mutex_unlock(&g_thr_mu); ctx->regs[0] = (uint32_t)DE_SRCH; break; }
        if (t->detached || t->joining) { (void)pthread_mutex_unlock(&g_thr_mu); ctx->regs[0] = (uint32_t)DE_INVAL; break; }
        if (t->joinable) (void)pthread_detach(t->ht);
        t->detached = true;
        if (t->done) { sa = t->stva; sl = t->stlen; ta = t->tsd; idx = (uint32_t)(t - g_thr); lc = t->ch; t->ch = NULL; t->used = false; rel = true; }
        (void)pthread_mutex_unlock(&g_thr_mu);
        if (rel) { if (sa != 0U) (void)a32_vm_munmap(ctx, sa, sl); if (ta != 0U) (void)a32_vm_munmap(ctx, ta, 0x1000U); tsd_clear(idx); free(lc); }
        ctx->regs[0] = 0U; break;
    }
    case 92U: {
        mxe_t *e; uint32_t aw = 0U; bool wr = thr_lazy_sig(ctx, r1, &aw) && (aw == 2U || aw == MX_SIG_REC);
        e = mx_lazy(ctx, r0, wr);
        ctx->regs[0] = (e == NULL) ? (uint32_t)DE_NOMEM : 0U;
        break;
    }
    case 93U: { mxe_t *e = mx_lazy(ctx, r0, false); ctx->regs[0] = (e == NULL) ? (uint32_t)DE_INVAL : mx_rc(pthread_mutex_lock(&e->mu)); break; }
    case 94U: { mxe_t *e = mx_lazy(ctx, r0, false); ctx->regs[0] = (e == NULL) ? (uint32_t)DE_INVAL : mx_rc(pthread_mutex_trylock(&e->mu)); break; }
    case 95U: { mxe_t *e = mx_lazy(ctx, r0, false); ctx->regs[0] = (e == NULL) ? (uint32_t)DE_INVAL : mx_rc(pthread_mutex_unlock(&e->mu)); break; }
    case 96U: { mxe_t *e = mx_lazy(ctx, r0, false); if (e == NULL) { ctx->regs[0] = (uint32_t)DE_INVAL; break; } (void)mx_remove(r0); ctx->regs[0] = 0U; break; }
    case 97U: {
        cve_t *e = cv_lazy(ctx, r0); uint32_t sig = 1U;
        ctx->regs[0] = (e != NULL) ? 0U : ((thr_lazy_sig(ctx, r0, &sig) && (sig == 0U || sig == CV_SIG_INIT)) ? (uint32_t)DE_NOMEM : (uint32_t)DE_INVAL);
        break;
    }
    case 98U: {
        cve_t *ce = cv_lazy(ctx, r0);
        mxe_t *me = mx_lazy(ctx, r1, false);
        if (ce == NULL || me == NULL) { ctx->regs[0] = (uint32_t)DE_INVAL; break; }
        __atomic_fetch_add(&ce->waiters, 1U, __ATOMIC_SEQ_CST);
        ctx->regs[0] = mx_rc(pthread_cond_wait(&ce->cv, &me->mu));
        __atomic_fetch_sub(&ce->waiters, 1U, __ATOMIC_SEQ_CST);
        break;
    }
    case 99U: { cve_t *ce = cv_lazy(ctx, r0); ctx->regs[0] = (ce == NULL) ? (uint32_t)DE_INVAL : mx_rc(pthread_cond_signal(&ce->cv)); break; }
    case 100U: { cve_t *ce = cv_lazy(ctx, r0); ctx->regs[0] = (ce == NULL) ? (uint32_t)DE_INVAL : mx_rc(pthread_cond_broadcast(&ce->cv)); break; }
    case 101U: { cve_t *e = cv_lazy(ctx, r0); if (e == NULL) { ctx->regs[0] = (uint32_t)DE_INVAL; break; } (void)cv_remove(r0); ctx->regs[0] = 0U; break; }
    case 102U: {
        oncee_t *oe;
        ocall_t *oc;
        if (r0 == 0U || r1 == 0U ||
            !a32_guest_range_ok(ctx, r0, 4U, (uint32_t)WR)) {
            ctx->regs[0] = (uint32_t)DE_INVAL;
            break;
        }
        oe = once_lookup(r0, true);
        if (oe == NULL) {
            ctx->regs[0] = (uint32_t)DE_NOMEM;
            break;
        }
        (void)pthread_mutex_lock(&g_once_mu);
        while (oe->running && !oe->done)
            (void)pthread_cond_wait(&g_once_cv, &g_once_mu);
        if (oe->done) {
            (void)pthread_mutex_unlock(&g_once_mu);
            ctx->regs[0] = 0U;
            break;
        }
        oc = oc_self();
        if (oc == NULL || oc->depth >= 8U) {
            (void)pthread_mutex_unlock(&g_once_mu);
            ctx->regs[0] = (uint32_t)DE_NOMEM;
            break;
        }
        oe->running = true;
        (void)pthread_mutex_unlock(&g_once_mu);
        memcpy(oc->st[oc->depth].regs, ctx->regs, sizeof(ctx->regs));
        oc->st[oc->depth].cpsr = ctx->cpsr;
        oc->st[oc->depth].gva = r0;
        oc->depth++;
        ctx->regs[14] = (A32_IOS_STUB_BASE_VA + ONCE_RESUME_ID * 8U) | 1U;
        a32_set_pc_interwork(ctx, r1);
        ctx->exit_reason = (uint32_t)A32_EXIT_BRANCH;
        break;
    }
    case 103U: {
        uint32_t i, key = 0xFFFFFFFFU;
        (void)pthread_mutex_lock(&g_key_mu);
        for (i = 0U; i < KEY_N; i++) {
            if (!g_key_used[i]) {
                g_key_used[i] = true;
                g_key_dtor[i] = r1;
                key = i;
                break;
            }
        }
        (void)pthread_mutex_unlock(&g_key_mu);
        if (key == 0xFFFFFFFFU) {
            ctx->regs[0] = (uint32_t)DE_AGAIN;
            break;
        }
        if (!guest_write(ctx, r0, &key, 4U)) {
            (void)pthread_mutex_lock(&g_key_mu);
            g_key_used[key] = false;
            g_key_dtor[key] = 0U;
            (void)pthread_mutex_unlock(&g_key_mu);
            ctx->regs[0] = (uint32_t)DE_INVAL;
            break;
        }
        ctx->regs[0] = 0U;
        break;
    }
    case 104U: {
        uint32_t slot; bool ok; void *v = (void *)(uintptr_t)r1;
        (void)pthread_mutex_lock(&g_key_mu); ok = (r0 < KEY_N && g_key_used[r0]); (void)pthread_mutex_unlock(&g_key_mu);
        if (!ok) { ctx->regs[0] = (uint32_t)DE_INVAL; break; }
        slot = (ctx->ios_thr_slot_plus1 != 0U && ctx->ios_thr_slot_plus1 <= THR_N) ? (ctx->ios_thr_slot_plus1 - 1U) : 0U;
        __atomic_store_n(&g_tsd[r0][slot], v, __ATOMIC_SEQ_CST); ctx->regs[0] = 0U; break;
    }
    case 105U: {
        uint32_t slot; bool ok; void *v;
        (void)pthread_mutex_lock(&g_key_mu); ok = (r0 < KEY_N && g_key_used[r0]); (void)pthread_mutex_unlock(&g_key_mu);
        if (!ok) { ctx->regs[0] = 0U; break; }
        slot = (ctx->ios_thr_slot_plus1 != 0U && ctx->ios_thr_slot_plus1 <= THR_N) ? (ctx->ios_thr_slot_plus1 - 1U) : 0U;
        v = __atomic_load_n(&g_tsd[r0][slot], __ATOMIC_SEQ_CST);
        ctx->regs[0] = (uint32_t)(uintptr_t)v; break;
    }
    case ONCE_RESUME_ID: {
        ocall_t *oc = oc_self();
        uint32_t i, d;
        if (oc == NULL || oc->depth == 0U) {
            ctx->regs[0] = (uint32_t)DE_INVAL;
            break;
        }
        d = --oc->depth;
        (void)pthread_mutex_lock(&g_once_mu);
        for (i = 0U; i < MX_N; i++) {
            if (g_once[i].used && g_once[i].gva == oc->st[d].gva) {
                g_once[i].done = true;
                g_once[i].running = false;
            }
        }
        (void)pthread_cond_broadcast(&g_once_cv);
        (void)pthread_mutex_unlock(&g_once_mu);
        memcpy(ctx->regs, oc->st[d].regs, sizeof(ctx->regs));
        ctx->cpsr = oc->st[d].cpsr;
        ctx->regs[0] = 0U;
        break;
    }
    case THR_EXIT_ID: {
        thr_t *t; uint32_t idx, sa, sl, ta, k, dk = KEY_N, dv = 0U, dtor = 0U; bool det; a32_ctx_t *lc;
        (void)pthread_mutex_lock(&g_thr_mu);
        t = thr_by_ctx(ctx);
        if (t == NULL) { (void)pthread_mutex_unlock(&g_thr_mu); ctx->regs[0] = 0U; break; }
        idx = (uint32_t)(t - g_thr);
        if (t->dtor_n == 0U)
            t->rv = ctx->regs[0];
        if (t->dtor_n < 4U * KEY_N) {
            (void)pthread_mutex_lock(&g_key_mu);
            for (k = 0U; k < KEY_N && dk == KEY_N; k++)
                if (g_key_used[k] && g_key_dtor[k] != 0U && g_tsd[k][idx] != NULL) {
                    dk = k; dv = (uint32_t)(uintptr_t)g_tsd[k][idx]; dtor = g_key_dtor[k];
                    g_tsd[k][idx] = NULL;
                }
            (void)pthread_mutex_unlock(&g_key_mu);
        }
        if (dk != KEY_N) {
            t->dtor_n++;
            (void)pthread_mutex_unlock(&g_thr_mu);
            ctx->regs[0] = dv;
            ctx->regs[14] = (A32_IOS_STUB_BASE_VA + (THR_EXIT_ID * 8U)) | 1U;
            a32_set_pc_interwork(ctx, dtor);
            ctx->exit_reason = (uint32_t)A32_EXIT_BRANCH;
            break;
        }
        t->done = true; t->ch = NULL;
        det = t->detached; sa = t->stva; sl = t->stlen; ta = t->tsd;
        lc = ctx;
        if (det) t->used = false;
        (void)pthread_cond_broadcast(&g_thr_cv);
        (void)pthread_mutex_unlock(&g_thr_mu);
        a32_signals_set_thread_ctx(NULL);
        tsd_clear(idx);
        if (det) {
            if (sa != 0U) (void)a32_vm_munmap(lc, sa, (size_t)sl);
            if (ta != 0U) (void)a32_vm_munmap(lc, ta, 0x1000U);
            free(lc); pthread_exit(NULL);
        }
        free(lc); pthread_exit(NULL);
        ctx->regs[0] = 0U; break;
    }
    default:
        return 0;
    }
    if (out_handled != NULL)
        *out_handled = true;
    return 0;
}
