/*
 * src/signals.c — 3-class signal routing, side-table recovery & arm32 rt_sigframe
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#define _GNU_SOURCE
#include "a32rt.h"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

#define A32_SIGFRAME_MAGIC 0x53494746U /* "SIGF" */

#define A32_SA_NOCLDSTOP 0x00000001U
#define A32_SA_NOCLDWAIT 0x00000002U
#define A32_SA_SIGINFO   0x00000004U
#define A32_SA_THIRTYTWO 0x02000000U
#define A32_SA_RESTORER  0x04000000U
#define A32_SA_ONSTACK   0x08000000U
#define A32_SA_RESTART   0x10000000U
#define A32_SA_NODEFER   0x40000000U
#define A32_SA_RESETHAND 0x80000000U

#define A32_SS_ONSTACK   1U
#define A32_SS_DISABLE   2U

typedef struct guest_sigaction {
    uint32_t handler;
    uint32_t flags;
    uint32_t restorer;
    uint64_t mask;
} guest_sigaction_t;

typedef struct a32_sigcontext {
    uint32_t trap_no;
    uint32_t error_code;
    uint32_t oldmask;
    uint32_t arm_r0;
    uint32_t arm_r1;
    uint32_t arm_r2;
    uint32_t arm_r3;
    uint32_t arm_r4;
    uint32_t arm_r5;
    uint32_t arm_r6;
    uint32_t arm_r7;
    uint32_t arm_r8;
    uint32_t arm_r9;
    uint32_t arm_r10;
    uint32_t arm_fp;
    uint32_t arm_ip;
    uint32_t arm_sp;
    uint32_t arm_lr;
    uint32_t arm_pc;
    uint32_t arm_cpsr;
    uint32_t fault_address;
} a32_sigcontext_t;

_Static_assert(sizeof(a32_sigcontext_t) == 84, "a32_sigcontext_t must be 84 bytes");

typedef struct a32_stack {
    uint32_t ss_sp;
    int32_t  ss_flags;
    uint32_t ss_size;
} a32_stack_t;

typedef struct a32_ucontext {
    uint32_t         uc_flags;
    uint32_t         uc_link;
    a32_stack_t      uc_stack;
    a32_sigcontext_t uc_mcontext;
    uint64_t         uc_sigmask;
    uint32_t         _pad[32];
} a32_ucontext_t;

_Static_assert(offsetof(a32_ucontext_t, uc_mcontext.arm_r0) == 32,
               "arm_r0 must be at offset 32 from start of ucontext");

typedef struct a32_siginfo {
    int32_t  si_signo;
    int32_t  si_errno;
    int32_t  si_code;
    uint32_t _pad[29];
} a32_siginfo_t;

typedef struct a32_rt_sigframe {
    a32_siginfo_t  info;
    a32_ucontext_t uc;
    uint32_t       retcode[2];
    uint32_t       magic;
    uint32_t       saved_regs[16];
    uint32_t       saved_cpsr;
    uint32_t       saved_fpscr;
    uint32_t       saved_itstate;
    uint32_t       saved_fault;
    uint32_t       saved_pad;
    uint64_t       saved_mask;
    uint64_t       saved_vfp_d[32];
} a32_rt_sigframe_t;

_Static_assert(offsetof(a32_rt_sigframe_t, info) == 0,
               "rt_sigframe info must be at offset 0");
_Static_assert(offsetof(a32_rt_sigframe_t, uc) == 128,
               "rt_sigframe uc must be at offset 128");

static _Thread_local a32_ctx_t *g_tls_ctx = NULL;
static guest_sigaction_t        g_guest_sigactions[65];

void a32_signals_set_thread_ctx(a32_ctx_t *ctx)
{
    g_tls_ctx = ctx;
}

a32_ctx_t *a32_signals_get_thread_ctx(void)
{
    return g_tls_ctx;
}

static void deliver_guest_signal(a32_ctx_t *ctx, int sig, guest_sigaction_t *act,
                                 const siginfo_t *host_info)
{
    uint32_t sp = ctx->regs[13];
    if ((act->flags & A32_SA_ONSTACK) &&
        !(ctx->altstack_flags & A32_SS_ONSTACK) &&
        ctx->altstack_size > 0U) {
        sp = ctx->altstack_sp + ctx->altstack_size;
        ctx->altstack_flags |= A32_SS_ONSTACK;
    }

    sp = (sp - (uint32_t)sizeof(a32_rt_sigframe_t)) & ~7U;
    uint32_t frame_va = sp;
    a32_rt_sigframe_t *frame = (a32_rt_sigframe_t *)a32_guest_ptr(ctx, frame_va);
    memset(frame, 0, sizeof(*frame));

    frame->retcode[0]    = 0xE3A070ADU; /* mov r7, #173 (ARM_NR_rt_sigreturn) */
    frame->retcode[1]    = 0xEF000000U; /* svc #0 */
    frame->magic         = A32_SIGFRAME_MAGIC;
    memcpy(frame->saved_regs, ctx->regs, sizeof(ctx->regs));
    frame->saved_cpsr    = ctx->cpsr;
    frame->saved_mask    = ctx->sig_mask;
    frame->saved_fault   = ctx->fault_addr;
    frame->saved_itstate = ctx->itstate;
    frame->saved_fpscr   = ctx->fpscr;
    memcpy(frame->saved_vfp_d, ctx->vfp_d, sizeof(ctx->vfp_d));

    frame->info.si_signo = sig;
    frame->info.si_errno = host_info ? host_info->si_errno : 0;
    if (host_info) {
        frame->info.si_code = host_info->si_code;
    } else {
        frame->info.si_code = 128; /* SI_KERNEL */
    }
    if (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE || sig == SIGTRAP) {
        frame->info._pad[0] = ctx->fault_addr;
    }

    frame->uc.uc_flags = 0;
    frame->uc.uc_link  = 0;
    frame->uc.uc_stack.ss_sp    = ctx->altstack_sp;
    frame->uc.uc_stack.ss_size  = ctx->altstack_size;
    frame->uc.uc_stack.ss_flags = (int32_t)ctx->altstack_flags;

    frame->uc.uc_mcontext.trap_no       = 0;
    frame->uc.uc_mcontext.error_code    = 0;
    frame->uc.uc_mcontext.oldmask       = (uint32_t)ctx->sig_mask;
    frame->uc.uc_mcontext.arm_r0        = ctx->regs[0];
    frame->uc.uc_mcontext.arm_r1        = ctx->regs[1];
    frame->uc.uc_mcontext.arm_r2        = ctx->regs[2];
    frame->uc.uc_mcontext.arm_r3        = ctx->regs[3];
    frame->uc.uc_mcontext.arm_r4        = ctx->regs[4];
    frame->uc.uc_mcontext.arm_r5        = ctx->regs[5];
    frame->uc.uc_mcontext.arm_r6        = ctx->regs[6];
    frame->uc.uc_mcontext.arm_r7        = ctx->regs[7];
    frame->uc.uc_mcontext.arm_r8        = ctx->regs[8];
    frame->uc.uc_mcontext.arm_r9        = ctx->regs[9];
    frame->uc.uc_mcontext.arm_r10       = ctx->regs[10];
    frame->uc.uc_mcontext.arm_fp        = ctx->regs[11];
    frame->uc.uc_mcontext.arm_ip        = ctx->regs[12];
    frame->uc.uc_mcontext.arm_sp        = ctx->regs[13];
    frame->uc.uc_mcontext.arm_lr        = ctx->regs[14];
    frame->uc.uc_mcontext.arm_pc        = ctx->regs[15];
    frame->uc.uc_mcontext.arm_cpsr      = ctx->cpsr;
    frame->uc.uc_mcontext.fault_address = ctx->fault_addr;
    frame->uc.uc_sigmask                = ctx->sig_mask;

    if (!(act->flags & A32_SA_NODEFER)) {
        ctx->sig_mask |= (1ULL << (sig - 1));
    }
    ctx->sig_mask |= act->mask;

    uint32_t info_va = frame_va + (uint32_t)offsetof(a32_rt_sigframe_t, info);
    uint32_t uc_va   = frame_va + (uint32_t)offsetof(a32_rt_sigframe_t, uc);
    uint32_t restorer_va = (act->flags & A32_SA_RESTORER && act->restorer != 0U)
                           ? act->restorer : 0xFFFF0FA8U;

    ctx->regs[0]  = (uint32_t)sig;
    ctx->regs[1]  = info_va;
    ctx->regs[2]  = uc_va;
    ctx->regs[13] = frame_va;
    ctx->regs[14] = restorer_va;
    ctx->itstate  = 0U;

    uint32_t handler = act->handler;
    if (act->flags & A32_SA_RESETHAND) {
        act->handler = 0U;
    }
    if (handler & 1U) {
        ctx->cpsr |= A32_CPSR_T;
        ctx->regs[15] = handler & ~1U;
    } else {
        ctx->cpsr &= ~A32_CPSR_T;
        ctx->regs[15] = handler & ~3U;
    }
    ctx->exit_reason = A32_EXIT_BRANCH;
}

static void host_sig_handler(int sig, siginfo_t *info, void *ucontext)
{
    a32_ctx_t *ctx = g_tls_ctx;
    if (!ctx) {
        return;
    }

    bool is_sync_trap = (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL ||
                         sig == SIGFPE  || sig == SIGTRAP);
    if (!is_sync_trap) {
        if (sig >= 1 && sig <= 64) {
            __atomic_fetch_or(&ctx->pending_signals, 1ULL << (sig - 1), __ATOMIC_SEQ_CST);
        }
        return;
    }

    ucontext_t *uc = (ucontext_t *)ucontext;
    uintptr_t host_pc = (uintptr_t)uc->uc_mcontext.pc;

    uint32_t guest_pc = 0U;
    uint32_t itstate  = 0U;
    bool in_block = a32_jit_lookup_host_pc(host_pc, &guest_pc, &itstate);

    if (in_block) {
        for (int i = 0; i < 15; i++) {
            ctx->regs[i] = (uint32_t)uc->uc_mcontext.regs[i];
        }
        ctx->regs[15] = guest_pc;
        ctx->itstate  = itstate;

        uint32_t pstate = (uint32_t)uc->uc_mcontext.pstate;
        ctx->cpsr = (ctx->cpsr & ~A32_CPSR_NZCV_MASK) | (pstate & A32_CPSR_NZCV_MASK);

        if ((sig == SIGSEGV || sig == SIGBUS) && info != NULL) {
            ctx->fault_addr = (uint32_t)((uintptr_t)info->si_addr - ctx->guest_base);
        } else {
            ctx->fault_addr = guest_pc;
        }

        guest_sigaction_t *act = &g_guest_sigactions[sig];
        if (act->handler == 1U /* SIG_IGN */) {
            return;
        }
        if (act->handler == 0U /* SIG_DFL */) {
            fprintf(stderr, "a32rt: unhandled guest signal %d at guest pc=0x%08x, fault_addr=0x%08x\n",
                    sig, guest_pc, ctx->fault_addr);
            _exit(128 + sig);
        }

        deliver_guest_signal(ctx, sig, act, info);

        for (int i = 0; i < 15; i++) {
            uc->uc_mcontext.regs[i] = (uint64_t)ctx->regs[i];
        }
        uc->uc_mcontext.regs[15] = (uint64_t)(ctx->regs[15] | ((ctx->cpsr & A32_CPSR_T) ? 1U : 0U));
        uc->uc_mcontext.regs[16] = (uint64_t)A32_EXIT_INTERWORK;
        uc->uc_mcontext.pc       = uc->uc_mcontext.regs[30];
        return;
    }

    if (sig >= 1 && sig <= 64) {
        __atomic_fetch_or(&ctx->pending_signals, 1ULL << (sig - 1), __ATOMIC_SEQ_CST);
    }
}

int a32_signals_init(a32_ctx_t *ctx)
{
    (void)ctx;
    static const int monitored_signals[] = {
        SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP,
        SIGINT, SIGTERM, SIGQUIT, SIGHUP, SIGCHLD,
        SIGUSR1, SIGUSR2, SIGPIPE, SIGALRM, SIGWINCH
    };

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = host_sig_handler;
    sa.sa_flags     = SA_SIGINFO | SA_NODEFER | SA_RESTART;
    sigfillset(&sa.sa_mask);

    for (size_t i = 0; i < sizeof(monitored_signals) / sizeof(monitored_signals[0]); i++) {
        int s = monitored_signals[i];
        if (sigaction(s, &sa, NULL) != 0) {
            fprintf(stderr, "a32rt: failed to install host signal handler for signal %d: %s\n",
                    s, strerror(errno));
            return -1;
        }
    }

    return 0;
}

void a32_signals_check_pending(a32_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }

    uint64_t pending = __atomic_load_n(&ctx->pending_signals, __ATOMIC_SEQ_CST);
    uint64_t unmasked = pending & ~ctx->sig_mask;
    if (unmasked == 0ULL) {
        return;
    }

    for (int sig = 1; sig <= 64; sig++) {
        uint64_t bit = 1ULL << (sig - 1);
        if (unmasked & bit) {
            __atomic_fetch_and(&ctx->pending_signals, ~bit, __ATOMIC_SEQ_CST);
            guest_sigaction_t *act = &g_guest_sigactions[sig];
            if (act->handler == 1U /* SIG_IGN */) {
                continue;
            }
            if (act->handler == 0U /* SIG_DFL */) {
                if (sig == SIGCHLD || sig == SIGURG || sig == SIGWINCH) {
                    continue;
                }
                _exit(128 + sig);
            }
            deliver_guest_signal(ctx, sig, act, NULL);
            break;
        }
    }
}

bool a32_signals_deliver_sync(a32_ctx_t *ctx, int sig, uint32_t fault_addr)
{
    if (!ctx || sig < 1 || sig > 64) {
        return false;
    }
    ctx->fault_addr = fault_addr;
    guest_sigaction_t *act = &g_guest_sigactions[sig];
    if (act->handler == 0U /* SIG_DFL */) {
        return false;
    }
    if (act->handler == 1U /* SIG_IGN */) {
        return true;
    }
    deliver_guest_signal(ctx, sig, act, NULL);
    return true;
}

int a32_sys_rt_sigaction(a32_ctx_t *ctx, int signum, uint32_t act_va,
                        uint32_t oldact_va, uint32_t sigsetsize)
{
    if (!ctx || signum < 1 || signum > 64 || signum == SIGKILL || signum == SIGSTOP) {
        return -EINVAL;
    }
    if (sigsetsize != sizeof(uint64_t)) {
        return -EINVAL;
    }
    if ((act_va != 0U && act_va < 0x1000U) || (oldact_va != 0U && oldact_va < 0x1000U)) {
        return -EFAULT;
    }

    if (oldact_va != 0U) {
        guest_sigaction_t cur = g_guest_sigactions[signum];
        uint32_t old_words[5];
        old_words[0] = cur.handler;
        old_words[1] = cur.flags;
        old_words[2] = cur.restorer;
        memcpy(&old_words[3], &cur.mask, sizeof(uint64_t));
        memcpy(a32_guest_ptr(ctx, oldact_va), old_words, sizeof(old_words));
    }

    if (act_va != 0U) {
        uint32_t new_words[5];
        memcpy(new_words, a32_guest_ptr(ctx, act_va), sizeof(new_words));
        g_guest_sigactions[signum].handler  = new_words[0];
        g_guest_sigactions[signum].flags    = new_words[1];
        g_guest_sigactions[signum].restorer = new_words[2];
        memcpy(&g_guest_sigactions[signum].mask, &new_words[3], sizeof(uint64_t));

        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = host_sig_handler;
        sa.sa_flags     = SA_SIGINFO | SA_NODEFER;
        if (new_words[0] <= 1U || (new_words[1] & A32_SA_RESTART) != 0U) {
            sa.sa_flags |= SA_RESTART;
        }
        sigfillset(&sa.sa_mask);
        sigaction(signum, &sa, NULL);
    }

    return 0;
}

int a32_sys_rt_sigprocmask(a32_ctx_t *ctx, int how, uint32_t set_va,
                           uint32_t oldset_va, uint32_t sigsetsize)
{
    if (!ctx || sigsetsize != sizeof(uint64_t)) {
        return -EINVAL;
    }
    if ((set_va != 0U && set_va < 0x1000U) || (oldset_va != 0U && oldset_va < 0x1000U)) {
        return -EFAULT;
    }

    if (oldset_va != 0U) {
        uint64_t cur = ctx->sig_mask;
        memcpy(a32_guest_ptr(ctx, oldset_va), &cur, sizeof(cur));
    }

    if (set_va != 0U) {
        uint64_t set = 0ULL;
        memcpy(&set, a32_guest_ptr(ctx, set_va), sizeof(set));
        set &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));

        switch (how) {
        case 0: /* SIG_BLOCK */
            ctx->sig_mask |= set;
            break;
        case 1: /* SIG_UNBLOCK */
            ctx->sig_mask &= ~set;
            break;
        case 2: /* SIG_SETMASK */
            ctx->sig_mask = set;
            break;
        default:
            return -EINVAL;
        }
    }

    return 0;
}

int a32_sys_rt_sigreturn(a32_ctx_t *ctx)
{
    if (!ctx) {
        return -EINVAL;
    }

    uint32_t sp = ctx->regs[13];
    if (sp < 0x1000U || (uint64_t)sp + sizeof(a32_rt_sigframe_t) > 0xFFFF0000ULL) {
        return -EFAULT;
    }
    a32_rt_sigframe_t *frame = (a32_rt_sigframe_t *)a32_guest_ptr(ctx, sp);

    const a32_sigcontext_t *sc = &frame->uc.uc_mcontext;
    ctx->regs[0]  = sc->arm_r0;
    ctx->regs[1]  = sc->arm_r1;
    ctx->regs[2]  = sc->arm_r2;
    ctx->regs[3]  = sc->arm_r3;
    ctx->regs[4]  = sc->arm_r4;
    ctx->regs[5]  = sc->arm_r5;
    ctx->regs[6]  = sc->arm_r6;
    ctx->regs[7]  = sc->arm_r7;
    ctx->regs[8]  = sc->arm_r8;
    ctx->regs[9]  = sc->arm_r9;
    ctx->regs[10] = sc->arm_r10;
    ctx->regs[11] = sc->arm_fp;
    ctx->regs[12] = sc->arm_ip;
    ctx->regs[13] = sc->arm_sp;
    ctx->regs[14] = sc->arm_lr;
    ctx->regs[15] = sc->arm_pc;
    ctx->cpsr     = sc->arm_cpsr;
    ctx->sig_mask = frame->uc.uc_sigmask;
    if (frame->magic == A32_SIGFRAME_MAGIC) {
        ctx->fault_addr = frame->saved_fault;
        ctx->itstate    = ((sc->arm_pc & ~1U) == (frame->saved_regs[15] & ~1U))
                          ? frame->saved_itstate : 0U;
        ctx->fpscr      = frame->saved_fpscr;
        memcpy(ctx->vfp_d, frame->saved_vfp_d, sizeof(ctx->vfp_d));
    } else {
        ctx->itstate = 0U;
    }

    if (ctx->regs[15] & 1U) {
        ctx->cpsr |= A32_CPSR_T;
        ctx->regs[15] &= ~1U;
    } else if (!(ctx->cpsr & A32_CPSR_T)) {
        ctx->regs[15] &= ~3U;
    }

    if (ctx->altstack_flags & A32_SS_ONSTACK) {
        uint32_t curr_sp = ctx->regs[13];
        if (curr_sp < ctx->altstack_sp || curr_sp >= ctx->altstack_sp + ctx->altstack_size) {
            ctx->altstack_flags &= ~A32_SS_ONSTACK;
        }
    }

    ctx->exit_reason = A32_EXIT_BRANCH;
    return 0;
}

int a32_sys_sigaltstack(a32_ctx_t *ctx, uint32_t uss_va, uint32_t uoss_va)
{
    if (!ctx) {
        return -EINVAL;
    }
    if ((uss_va != 0U && uss_va < 0x1000U) || (uoss_va != 0U && uoss_va < 0x1000U)) {
        return -EFAULT;
    }

    if (uoss_va != 0U) {
        a32_stack_t old_st;
        old_st.ss_sp    = ctx->altstack_sp;
        old_st.ss_flags = (int32_t)(ctx->altstack_flags ? ctx->altstack_flags
                                                        : (ctx->altstack_size == 0U ? A32_SS_DISABLE : 0U));
        old_st.ss_size  = ctx->altstack_size;
        memcpy(a32_guest_ptr(ctx, uoss_va), &old_st, sizeof(old_st));
    }

    if (uss_va != 0U) {
        a32_stack_t new_st;
        memcpy(&new_st, a32_guest_ptr(ctx, uss_va), sizeof(new_st));

        if (new_st.ss_flags & (int32_t)A32_SS_DISABLE) {
            ctx->altstack_sp    = 0U;
            ctx->altstack_size  = 0U;
            ctx->altstack_flags = A32_SS_DISABLE;
        } else {
            if (new_st.ss_size < 2048U) {
                return -ENOMEM;
            }
            ctx->altstack_sp    = new_st.ss_sp;
            ctx->altstack_size  = new_st.ss_size;
            ctx->altstack_flags = (uint32_t)new_st.ss_flags;
        }
    }

    return 0;
}
