/*
 * ios_signals.c — Darwin signal mapping, disposition, and arm32 ucontext delivery
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
#include "vm/vm32.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#include <unistd.h>

/* Darwin signal numbers (1..31) and disposition constants. */
#define IOS_SIG_BLOCK 1
#define IOS_SIG_UNBLOCK 2
#define IOS_SIG_SETMASK 3

#define IOS_SA_ONSTACK 0x0001U
#define IOS_SA_RESETHAND 0x0004U
#define IOS_SA_NODEFER 0x0010U
#define IOS_SA_SIGINFO 0x0040U

#define IOS_SS_ONSTACK 0x0001U
#define IOS_SS_DISABLE 0x0004U

#define IOS_UC_TRADITIONAL 30U

#define IOS_UNMASKABLE ((1ULL << (9 - 1)) | (1ULL << (17 - 1))) /* SIGKILL, SIGSTOP */

#define IOS_EPERM 1
#define IOS_ENOMEM 12
#define IOS_EFAULT 14
#define IOS_EINVAL 22
#define IOS_EINTR 4
#define IOS_ESRCH 3

#if defined(__APPLE__)
static int ios_d2h(int dsig)
{
    return (dsig >= 1 && dsig <= 31) ? dsig : -1;
}

static int ios_h2d(int host_sig)
{
    return (host_sig >= 1 && host_sig <= 31) ? host_sig : -1;
}
#else
/* Darwin -> Linux host signal mapping (index = Darwin signal). */
static const int8_t kIosD2H[32] = {
    -1,
    1, 2, 3, 4, 5, 6, 7, 8, 9, 7,
    11, 31, 13, 14, 15, 23, 19, 20, 18, 17,
    21, 22, 29, 24, 25, 26, 27, 28, 29, 10,
    12
};

static int ios_d2h(int dsig)
{
    if (dsig < 1 || dsig > 31) {
        return -1;
    }
#if defined(__linux__)
    if ((dsig == 7 || dsig == 29) && SIGRTMIN + 7 <= SIGRTMAX) {
        return (dsig == 7) ? (SIGRTMIN + 6) : (SIGRTMIN + 7);
    }
#endif
    return (int)kIosD2H[dsig];
}

/* Linux host -> Darwin signal mapping. Unmappable host signals yield -1. */
static int ios_h2d(int host_sig)
{
#if defined(__linux__)
    if (SIGRTMIN + 7 <= SIGRTMAX) {
        if (host_sig == SIGRTMIN + 6) {
            return 7;
        }
        if (host_sig == SIGRTMIN + 7) {
            return 29;
        }
    }
#endif
    switch (host_sig) {
    case 1: return 1;
    case 2: return 2;
    case 3: return 3;
    case 4: return 4;
    case 5: return 5;
    case 6: return 6;
    case 7: return 10;
    case 8: return 8;
    case 9: return 9;
    case 10: return 30;
    case 11: return 11;
    case 12: return 31;
    case 13: return 13;
    case 14: return 14;
    case 15: return 15;
    case 17: return 20;
    case 18: return 19;
    case 19: return 17;
    case 20: return 18;
    case 21: return 21;
    case 22: return 22;
    case 23: return 16;
    case 24: return 24;
    case 25: return 25;
    case 26: return 26;
    case 27: return 27;
    case 28: return 28;
    case 29: return 23;
    case 31: return 12;
    default: return -1;
    }
}
#endif

/* Kernel-side disposition: {sa_handler, sa_tramp, sa_mask, sa_flags}. */
typedef struct {
    uint32_t handler;
    uint32_t tramp;
    uint32_t mask;
    int32_t flags;
} ios_act_t;

static ios_act_t g_ios_acts[32];
static _Thread_local siginfo_t g_ios_pending_info[33];
static __thread bool g_has_sigsuspend_mask;
static __thread uint64_t g_sigsuspend_mask;

typedef struct __attribute__((packed, aligned(4))) ios_nsv {
    uint32_t handler;
    uint32_t tramp;
    uint32_t mask;
    int32_t flags;
} ios_nsv_t;

_Static_assert(sizeof(ios_nsv_t) == 16, "kernel nsv must be 16 bytes");

typedef struct __attribute__((packed, aligned(4))) ios_osv {
    uint32_t handler;
    uint32_t mask;
    int32_t flags;
} ios_osv_t;

_Static_assert(sizeof(ios_osv_t) == 12, "kernel osv must be 12 bytes");

typedef struct __attribute__((packed, aligned(4))) ios_mc {
    uint32_t es_exception;
    uint32_t es_fsr;
    uint32_t es_far;
    uint32_t r[13];
    uint32_t sp;
    uint32_t lr;
    uint32_t pc;
    uint32_t cpsr;
    uint64_t vfp_d[32];
    uint32_t fpscr;
    uint32_t pad[8];
} ios_mc_t;

_Static_assert(sizeof(ios_mc_t) == 372, "ios_mcontext32_t must be 372 bytes");

typedef struct __attribute__((packed, aligned(4))) ios_uc {
    int32_t uc_onstack;
    uint32_t uc_sigmask;
    uint32_t ss_sp;
    uint32_t ss_size;
    int32_t ss_flags;
    uint32_t uc_link;
    uint32_t uc_mcsize;
    uint32_t uc_mcontext;
} ios_uc_t;

_Static_assert(sizeof(ios_uc_t) == 32, "ios_ucontext32_t must be 32 bytes");

typedef struct __attribute__((packed, aligned(4))) ios_si {
    int32_t signo;
    int32_t err_no;
    int32_t code;
    int32_t pid;
    uint32_t uid;
    int32_t status;
    uint32_t addr;
    uint32_t pad[9];
} ios_si_t;

_Static_assert(sizeof(ios_si_t) == 64, "ios_siginfo32_t must be 64 bytes");

#define IOS_MC_SIZE 372U
#define IOS_TOTAL_FRAME 476U
#define IOS_FRAME_ROUND 480U

static bool ios_range(a32_ctx_t *ctx, uint32_t va, uint32_t len, uint32_t prot)
{
    return a32_guest_range_ok(ctx, va, len, prot);
}

static bool ios_is_on_altstack(const a32_ctx_t *ctx, uint32_t sp)
{
    return (ctx->altstack_size > 0U &&
            (ctx->altstack_flags & (uint32_t)IOS_SS_DISABLE) == 0U &&
            sp >= ctx->altstack_sp &&
            (sp - ctx->altstack_sp) < ctx->altstack_size);
}

static bool ios_act_default_ignored(int dsig)
{
    /* SIGCHLD, SIGCONT, SIGURG, SIGWINCH, SIGIO, SIGINFO. */
    return (dsig == 20 || dsig == 19 || dsig == 16 || dsig == 28 ||
            dsig == 23 || dsig == 29);
}

static bool ios_act_default_stop(int dsig)
{
    /* SIGTSTP, SIGTTIN, SIGTTOU: default stop; never terminate the guest. */
    return (dsig == 18 || dsig == 21 || dsig == 22);
}

static void ios_fill_siginfo(a32_ctx_t *ctx, ios_si_t *dst, int dsig)
{
    const siginfo_t *info = &g_ios_pending_info[dsig];
    memset(dst, 0, sizeof(*dst));
    dst->signo = dsig;
    if (info->si_signo != 0) {
        dst->err_no = info->si_errno;
        dst->code = info->si_code;
        dst->pid = (int32_t)info->si_pid;
        dst->uid = (uint32_t)info->si_uid;
        if (info->si_code > 0 && info->si_addr != NULL) {
            dst->addr = (uint32_t)((uintptr_t)info->si_addr -
                                   (uintptr_t)ctx->guest_base);
        } else {
            dst->addr = ctx->fault_addr;
        }
    } else {
        dst->code = 0;
        dst->addr = ctx->fault_addr;
    }
}

static void ios_deliver_signal(a32_ctx_t *ctx, int dsig)
{
    ios_act_t *act = &g_ios_acts[dsig];
    uint32_t orig_sp = ctx->regs[13];
    bool was_onstack = ios_is_on_altstack(ctx, orig_sp);
    uint32_t sp = orig_sp;
    if (((uint32_t)act->flags & (uint32_t)IOS_SA_ONSTACK) != 0U && !was_onstack &&
        ctx->altstack_size > 0U &&
        (ctx->altstack_flags & (uint32_t)IOS_SS_DISABLE) == 0U) {
        sp = ctx->altstack_sp + ctx->altstack_size;
    }
    sp = (sp - (uint32_t)IOS_FRAME_ROUND) & ~15U;
    if (!ios_range(ctx, sp, (uint32_t)IOS_FRAME_ROUND, (uint32_t)PROT_WRITE)) {
        _exit(128 + dsig);
    }
    uint8_t *base = (uint8_t *)a32_guest_ptr(ctx, sp);
    memset(base, 0, IOS_FRAME_ROUND);
    uint32_t mc_va = sp + 8U;
    uint32_t uc_va = mc_va + (uint32_t)IOS_MC_SIZE;
    uint32_t si_va = uc_va + 32U;
    ios_mc_t mc;
    ios_uc_t uc;
    ios_si_t si;
    memset(&mc, 0, sizeof(mc));
    mc.es_exception = (uint32_t)dsig;
    mc.es_fsr = 0U;
    mc.es_far = ctx->fault_addr;
    memcpy(mc.r, ctx->regs, sizeof(mc.r));
    mc.sp = orig_sp;
    mc.lr = ctx->regs[14];
    mc.pc = ctx->regs[15];
    {
        uint32_t it = ctx->itstate & 0xFFU;
        mc.cpsr = (ctx->cpsr & ~(uint32_t)A32_CPSR_IT_MASK) |
                  (((it & 0xFCU) << 8) | ((it & 0x03U) << 25));
    }
    memcpy(mc.vfp_d, ctx->vfp_d, sizeof(mc.vfp_d));
    mc.fpscr = ctx->fpscr;
    memset(&uc, 0, sizeof(uc));
    uc.uc_onstack = was_onstack ? (int32_t)IOS_SS_ONSTACK : 0;
    if (g_has_sigsuspend_mask) {
        uc.uc_sigmask = (uint32_t)g_sigsuspend_mask;
        g_has_sigsuspend_mask = false;
    } else {
        uc.uc_sigmask = (uint32_t)ctx->sig_mask;
    }
    uc.ss_sp = ctx->altstack_sp;
    uc.ss_size = ctx->altstack_size;
    uc.ss_flags = (int32_t)ctx->altstack_flags;
    uc.uc_link = 0U;
    uc.uc_mcsize = (uint32_t)IOS_MC_SIZE;
    uc.uc_mcontext = mc_va;
    ios_fill_siginfo(ctx, &si, dsig);
    memcpy(base, &uc_va, 4);
    {
        uint32_t flavor = (uint32_t)IOS_UC_TRADITIONAL;
        memcpy(base + 4U, &flavor, 4);
    }
    memcpy(base + 8U, &mc, sizeof(mc));
    memcpy(base + 8U + (uint32_t)IOS_MC_SIZE, &uc, sizeof(uc));
    memcpy(base + 8U + (uint32_t)IOS_MC_SIZE + 32U, &si, sizeof(si));
    a32_vm32_copyout_invalidate_exec(ctx, sp, (uint32_t)IOS_FRAME_ROUND);
    uint32_t handler = act->handler;
    uint32_t tramp = act->tramp;
    if (tramp != 0U) {
        ctx->regs[0] = handler;
        ctx->regs[1] = (uint32_t)IOS_UC_TRADITIONAL;
        ctx->regs[2] = (uint32_t)dsig;
        ctx->regs[3] = si_va;
    } else {
        ctx->regs[0] = (uint32_t)dsig;
        ctx->regs[1] = (((uint32_t)act->flags & (uint32_t)IOS_SA_SIGINFO) != 0U) ? si_va : 0U;
        ctx->regs[2] = uc_va;
    }
    ctx->regs[13] = sp;
    ctx->regs[14] = (uint32_t)A32_IOS_SIGTRAMP_VA | 1U;
    ctx->itstate = 0U;
    ctx->sig_mask |= (uint64_t)act->mask;
    if (((uint32_t)act->flags & (uint32_t)IOS_SA_NODEFER) == 0U) {
        ctx->sig_mask |= (1ULL << (dsig - 1));
    }
    ctx->sig_mask &= ~(uint64_t)IOS_UNMASKABLE;
    if (((uint32_t)act->flags & (uint32_t)IOS_SA_RESETHAND) != 0U) {
        act->handler = 0U;
    }
    if (tramp != 0U) {
        a32_set_pc_interwork(ctx, tramp);
    } else {
        a32_set_pc_interwork(ctx, handler);
    }
    ctx->sync_sig_delivered = true;
    ctx->exit_reason = (uint32_t)A32_EXIT_BRANCH;
}

int a32_ios_host_to_darwin_sig(int host_sig)
{
    return ios_h2d(host_sig);
}

int a32_ios_darwin_to_host_sig(int darwin_sig)
{
    return ios_d2h(darwin_sig);
}

void a32_ios_signals_enqueue_host(a32_ctx_t *ctx, int host_sig,
                                  const void *host_siginfo)
{
    int dsig;
    if (!ctx) {
        return;
    }
    dsig = ios_h2d(host_sig);
    if (dsig < 1 || dsig > 31) {
        return;
    }
    if (g_ios_acts[dsig].handler == 1U) {
        return;
    }
    if (g_ios_acts[dsig].handler == 0U &&
        (ios_act_default_ignored(dsig) || ios_act_default_stop(dsig))) {
        return;
    }
    if (host_siginfo != NULL) {
        memcpy(&g_ios_pending_info[dsig], host_siginfo, sizeof(siginfo_t));
    } else {
        memset(&g_ios_pending_info[dsig], 0, sizeof(siginfo_t));
    }
    __atomic_fetch_or(&ctx->pending_signals, (1ULL << (dsig - 1)),
                      __ATOMIC_SEQ_CST);
}

void a32_ios_signals_check_pending(a32_ctx_t *ctx)
{
    uint64_t unmasked;
    if (!ctx) {
        return;
    }
    unmasked = __atomic_load_n(&ctx->pending_signals, __ATOMIC_ACQUIRE) &
               ~ctx->sig_mask;
    while (unmasked != 0ULL) {
        int dsig = __builtin_ctzll(unmasked) + 1;
        uint64_t bit = 1ULL << (dsig - 1);
        unmasked &= ~bit;
        __atomic_fetch_and(&ctx->pending_signals, ~bit, __ATOMIC_SEQ_CST);
        if (dsig < 1 || dsig > 31) {
            continue;
        }
        if (g_ios_acts[dsig].handler == 1U ||
            (g_ios_acts[dsig].handler == 0U &&
             (ios_act_default_ignored(dsig) || ios_act_default_stop(dsig)))) {
            memset(&g_ios_pending_info[dsig], 0, sizeof(siginfo_t));
            continue;
        }
        if (g_ios_acts[dsig].handler == 0U) {
            _exit(128 + dsig);
        }
        ios_deliver_signal(ctx, dsig);
        memset(&g_ios_pending_info[dsig], 0, sizeof(siginfo_t));
        break;
    }
}

bool a32_ios_signals_deliver_sync(a32_ctx_t *ctx, int host_sig,
                                  uint32_t fault_addr)
{
    int dsig;
    if (!ctx) {
        return false;
    }
    dsig = ios_h2d(host_sig);
    if (dsig < 1 || dsig > 31) {
        return false;
    }
    ctx->fault_addr = fault_addr;
    if (g_ios_acts[dsig].handler == 0U || g_ios_acts[dsig].handler == 1U) {
        return false;
    }
    memset(&g_ios_pending_info[dsig], 0, sizeof(siginfo_t));
    g_ios_pending_info[dsig].si_signo = host_sig;
    g_ios_pending_info[dsig].si_code = 1;
    ios_deliver_signal(ctx, dsig);
    return true;
}

int a32_ios_sys_sigaction(a32_ctx_t *ctx, int dsig, uint32_t nsv_va,
                          uint32_t osv_va)
{
    if (!ctx || dsig < 1 || dsig > 31 || dsig == 9 || dsig == 17) {
        return -IOS_EINVAL;
    }
    if ((nsv_va != 0U &&
         !ios_range(ctx, nsv_va, (uint32_t)sizeof(ios_nsv_t),
                    (uint32_t)PROT_READ)) ||
        (osv_va != 0U &&
         !ios_range(ctx, osv_va, (uint32_t)sizeof(ios_osv_t),
                    (uint32_t)PROT_WRITE))) {
        return -IOS_EFAULT;
    }
    ios_nsv_t nsv;
    bool have_nsv = false;
    if (nsv_va != 0U) {
        memcpy(&nsv, a32_guest_ptr(ctx, nsv_va), sizeof(nsv));
        have_nsv = true;
    }
    if (osv_va != 0U) {
        ios_osv_t osv;
        osv.handler = g_ios_acts[dsig].handler;
        osv.mask = g_ios_acts[dsig].mask;
        osv.flags = g_ios_acts[dsig].flags;
        memcpy(a32_guest_ptr(ctx, osv_va), &osv, sizeof(osv));
        a32_vm32_copyout_invalidate_exec(ctx, osv_va, (uint32_t)sizeof(osv));
    }
    if (have_nsv) {
        int host_sig;
        host_sig = ios_d2h(dsig);
        if (host_sig > 0) {
            a32_signals_ensure_host_handler(host_sig);
        }
        g_ios_acts[dsig].handler = nsv.handler;
        g_ios_acts[dsig].tramp = nsv.tramp;
        g_ios_acts[dsig].mask = nsv.mask & ~(uint32_t)IOS_UNMASKABLE;
        g_ios_acts[dsig].flags = nsv.flags;
        if (nsv.handler == 1U) {
            __atomic_fetch_and(&ctx->pending_signals, ~(1ULL << (dsig - 1)),
                               __ATOMIC_SEQ_CST);
        }
    }
    return 0;
}

int a32_ios_sys_sigprocmask(a32_ctx_t *ctx, int how, uint32_t set_va,
                            uint32_t oset_va)
{
    uint32_t old_mask;
    if (!ctx || how < (int)IOS_SIG_BLOCK || how > (int)IOS_SIG_SETMASK) {
        return -IOS_EINVAL;
    }
    if ((set_va != 0U &&
         !ios_range(ctx, set_va, 4U, (uint32_t)PROT_READ)) ||
        (oset_va != 0U &&
         !ios_range(ctx, oset_va, 4U, (uint32_t)PROT_WRITE))) {
        return -IOS_EFAULT;
    }
    old_mask = (uint32_t)ctx->sig_mask;
    if (set_va != 0U) {
        uint32_t v = 0U;
        int d;
        memcpy(&v, a32_guest_ptr(ctx, set_va), 4);
        v &= ~(uint32_t)IOS_UNMASKABLE;
        if (how == (int)IOS_SIG_BLOCK) {
            ctx->sig_mask |= (uint64_t)v;
        } else if (how == (int)IOS_SIG_UNBLOCK) {
            ctx->sig_mask &= ~(uint64_t)v;
        } else {
            ctx->sig_mask = (uint64_t)v;
        }
        for (d = 1; d <= 31; d++) {
            if ((ctx->sig_mask & (1ULL << (d - 1))) != 0ULL) {
                int h = ios_d2h(d);
                if (h > 0) {
                    a32_signals_ensure_host_handler(h);
                }
            }
        }
    }
    if (oset_va != 0U) {
        memcpy(a32_guest_ptr(ctx, oset_va), &old_mask, 4);
        a32_vm32_copyout_invalidate_exec(ctx, oset_va, 4U);
    }
    return 0;
}

int a32_ios_sys_sigpending(a32_ctx_t *ctx, uint32_t osv_va)
{
    uint64_t pending;
    uint32_t v;
    if (!ctx) {
        return -IOS_EINVAL;
    }
    if (osv_va == 0U ||
        !ios_range(ctx, osv_va, 4U, (uint32_t)PROT_WRITE)) {
        return -IOS_EFAULT;
    }
    pending = __atomic_load_n(&ctx->pending_signals, __ATOMIC_ACQUIRE);
    v = (uint32_t)pending;
    memcpy(a32_guest_ptr(ctx, osv_va), &v, 4);
    a32_vm32_copyout_invalidate_exec(ctx, osv_va, 4U);
    return 0;
}

int a32_ios_sys_sigaltstack(a32_ctx_t *ctx, uint32_t nss_va, uint32_t oss_va)
{
    uint32_t old_sp;
    uint32_t old_size;
    uint32_t cur_flags;
    bool on_alt;
    if (!ctx) {
        return -IOS_EINVAL;
    }
    if ((nss_va != 0U && !ios_range(ctx, nss_va, 12U, (uint32_t)PROT_READ)) ||
        (oss_va != 0U && !ios_range(ctx, oss_va, 12U, (uint32_t)PROT_WRITE))) {
        return -IOS_EFAULT;
    }
    on_alt = ios_is_on_altstack(ctx, ctx->regs[13]);
    old_sp = ctx->altstack_sp;
    old_size = ctx->altstack_size;
    if (old_size == 0U || (ctx->altstack_flags & (uint32_t)IOS_SS_DISABLE) != 0U) {
        cur_flags = (uint32_t)IOS_SS_DISABLE;
    } else {
        cur_flags = on_alt ? (uint32_t)IOS_SS_ONSTACK : 0U;
    }
    if (nss_va != 0U) {
        uint32_t in_sp = 0U;
        uint32_t in_size = 0U;
        int32_t in_flags = 0;
        uint8_t raw[12];
        memcpy(raw, a32_guest_ptr(ctx, nss_va), sizeof(raw));
        memcpy(&in_sp, raw, 4);
        memcpy(&in_size, raw + 4U, 4);
        memcpy(&in_flags, raw + 8U, 4);
        if (((uint32_t)in_flags & ~(uint32_t)IOS_SS_DISABLE) != 0U) {
            return -IOS_EINVAL;
        }
        if (on_alt) {
            return -IOS_EPERM;
        }
        if (((uint32_t)in_flags & (uint32_t)IOS_SS_DISABLE) != 0U) {
            ctx->altstack_sp = 0U;
            ctx->altstack_size = 0U;
            ctx->altstack_flags = (uint32_t)IOS_SS_DISABLE;
        } else {
            if (in_size < 2048U) {
                return -IOS_ENOMEM;
            }
            ctx->altstack_sp = in_sp;
            ctx->altstack_size = in_size;
            ctx->altstack_flags = 0U;
        }
    }
    if (oss_va != 0U) {
        uint8_t out[12];
        memcpy(out, &old_sp, 4);
        memcpy(out + 4U, &old_size, 4);
        memcpy(out + 8U, &cur_flags, 4);
        memcpy(a32_guest_ptr(ctx, oss_va), out, sizeof(out));
        a32_vm32_copyout_invalidate_exec(ctx, oss_va, (uint32_t)sizeof(out));
    }
    return 0;
}

int a32_ios_sys_sigsuspend(a32_ctx_t *ctx, uint32_t mask)
{
    uint64_t saved;
    uint64_t suspended;
    sigset_t hs;
    sigset_t all;
    sigset_t prev;
    int d;
    if (!ctx) {
        return -IOS_EINVAL;
    }
    saved = ctx->sig_mask;
    suspended = (uint64_t)mask & ~(uint64_t)IOS_UNMASKABLE;
    (void)sigfillset(&all);
    (void)pthread_sigmask(SIG_BLOCK, &all, &prev);
    ctx->sig_mask = suspended;
    sigemptyset(&hs);
    for (d = 1; d <= 31; d++) {
        if (((mask >> (d - 1)) & 1U) != 0U) {
            int h = ios_d2h(d);
            if (h > 0) {
                (void)sigaddset(&hs, h);
            }
        }
    }
    if ((__atomic_load_n(&ctx->pending_signals, __ATOMIC_ACQUIRE) &
         ~ctx->sig_mask) == 0ULL) {
        (void)sigsuspend(&hs);
    }
    (void)pthread_sigmask(SIG_SETMASK, &prev, NULL);
    if ((__atomic_load_n(&ctx->pending_signals, __ATOMIC_ACQUIRE) &
         ~ctx->sig_mask) != 0ULL) {
        g_has_sigsuspend_mask = true;
        g_sigsuspend_mask = saved;
        a32_ios_bsd_err(ctx, (int)IOS_EINTR);
        a32_ios_signals_check_pending(ctx);
        if (g_has_sigsuspend_mask) {
            g_has_sigsuspend_mask = false;
            ctx->sig_mask = saved;
        }
    } else {
        ctx->sig_mask = saved;
    }
    return -IOS_EINTR;
}

int a32_ios_sys_kill(a32_ctx_t *ctx, int pid, int dsig, int posix)
{
    int host_sig = 0;
    (void)posix;
    if (dsig != 0) {
        if (dsig < 1 || dsig > 31) {
            return -IOS_EINVAL;
        }
        host_sig = ios_d2h(dsig);
        if (host_sig < 0) {
            return -IOS_EINVAL;
        }
    }
    if (host_sig > 0 && ctx != NULL && (pid_t)pid == getpid()) {
        a32_ios_signals_enqueue_host(ctx, host_sig, NULL);
        return 0;
    }
    if (kill((pid_t)pid, host_sig) != 0) {
        return -a32_ios_host_to_darwin_errno(errno);
    }
    return 0;
}

int a32_ios_sys_pthread_kill(a32_ctx_t *ctx, uint32_t port_or_tid, int dsig)
{
    int host_sig;
    if (!ctx) {
        return -IOS_EINVAL;
    }
    if (dsig < 0 || dsig > 31) {
        return -IOS_EINVAL;
    }
    host_sig = (dsig == 0) ? 0 : ios_d2h(dsig);
    if (dsig != 0 && host_sig < 0) {
        return -IOS_EINVAL;
    }
    if (a32_ios_thread_kill_target(ctx, port_or_tid, host_sig) < 0) {
        return -IOS_ESRCH;
    }
    return 0;
}

int a32_ios_sys_sigreturn(a32_ctx_t *ctx, uint32_t uctx_va, int infostyle)
{
    ios_uc_t uc;
    ios_mc_t mc;
    uint32_t it;
    (void)infostyle;
    if (!ctx || uctx_va < 0x1000U) {
        return -IOS_EFAULT;
    }
    if (!ios_range(ctx, uctx_va, (uint32_t)sizeof(ios_uc_t),
                   (uint32_t)PROT_READ)) {
        return -IOS_EFAULT;
    }
    memcpy(&uc, a32_guest_ptr(ctx, uctx_va), sizeof(uc));
    if (uc.uc_mcontext < 0x1000U ||
        !ios_range(ctx, uc.uc_mcontext, (uint32_t)sizeof(ios_mc_t),
                   (uint32_t)PROT_READ)) {
        return -IOS_EFAULT;
    }
    memcpy(&mc, a32_guest_ptr(ctx, uc.uc_mcontext), sizeof(mc));
    memcpy(ctx->regs, mc.r, sizeof(mc.r));
    ctx->regs[13] = mc.sp;
    ctx->regs[14] = mc.lr;
    ctx->regs[15] = mc.pc;
    it = ((mc.cpsr >> 8) & 0xFCU) | ((mc.cpsr >> 25) & 0x03U);
    if ((it & 0x0FU) == 0U || (it & 0xF0U) == 0xF0U) {
        it = 0U;
    }
    ctx->cpsr = (mc.cpsr & 0xF80F0020U) | 0x10U;
    ctx->itstate = it;
    memcpy(ctx->vfp_d, mc.vfp_d, sizeof(ctx->vfp_d));
    ctx->fpscr = mc.fpscr;
    ctx->sig_mask = (uint64_t)uc.uc_sigmask & ~(uint64_t)IOS_UNMASKABLE;
    ctx->fault_addr = mc.es_far;
    a32_normalize_pc(ctx);
    ctx->exit_reason = (uint32_t)A32_EXIT_BRANCH;
    return 0;
}
