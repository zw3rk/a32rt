/*
 * src/signals.c — 3-class signal routing, side-table recovery & arm32 rt_sigframe
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#define _GNU_SOURCE
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#define _XOPEN_SOURCE 600
#endif
#include "a32rt.h"
#include "ios/ios.h"
#include "signals_frame.h"
#include "vm/vm32.h"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

static inline bool guest_read(a32_ctx_t *ctx, void *dst, uint32_t va, uint32_t len)
{
    if (!a32_guest_range_ok(ctx, va, len, PROT_READ)) {
        return false;
    }
    memcpy(dst, a32_guest_ptr(ctx, va), len);
    return true;
}

static inline bool guest_write(a32_ctx_t *ctx, uint32_t va, const void *src, uint32_t len)
{
    if (!a32_guest_range_ok(ctx, va, len, PROT_WRITE)) {
        return false;
    }
    memcpy(a32_guest_ptr(ctx, va), src, len);
    /* Break reservations and flush JIT on EXEC pages (A32-R9-5, A32-R9-6). */
    a32_vm32_copyout_invalidate_exec(ctx, va, len);
    return true;
}

static _Thread_local a32_ctx_t *g_tls_ctx = NULL;
static guest_sigaction_t        g_guest_sigactions[65];
static _Thread_local siginfo_t  g_pending_siginfo[65];
static siginfo_t                g_preset_siginfo[65];
static uint64_t                 g_host_handler_installed_mask = 0ULL;

void a32_signals_set_thread_ctx(a32_ctx_t *ctx)
{
    g_tls_ctx = ctx;
}

a32_ctx_t *a32_signals_get_thread_ctx(void)
{
    return g_tls_ctx;
}

static bool sig_is_ignored(int sig, uint32_t handler)
{
    return (handler == A32_SIG_IGN || (handler == A32_SIG_DFL &&
            (sig == SIGCHLD || sig == SIGURG || sig == SIGWINCH || sig == SIGCONT)));
}

static void enqueue_async_signal(a32_ctx_t *ctx, int sig, const siginfo_t *info)
{
    if (sig < 1 || sig > 64) {
        return;
    }
    if (sig_is_ignored(sig, g_guest_sigactions[sig].handler)) {
        return;
    }
    if (g_preset_siginfo[sig].si_signo == sig) {
        g_pending_siginfo[sig] = g_preset_siginfo[sig];
        memset(&g_preset_siginfo[sig], 0, sizeof(siginfo_t));
    } else if (info != NULL) {
        g_pending_siginfo[sig] = *info;
    } else {
        memset(&g_pending_siginfo[sig], 0, sizeof(siginfo_t));
        g_pending_siginfo[sig].si_signo = sig;
    }
    __atomic_fetch_or(&ctx->pending_signals, 1ULL << (sig - 1), __ATOMIC_SEQ_CST);
}

/* si_code classifier: user-sent (enqueue) vs genuine fault (deliver). */
static bool sig_code_is_user_sent(int sig, const siginfo_t *info)
{
    if (sig >= 1 && sig <= 64 && g_preset_siginfo[sig].si_signo == sig) {
        return true;
    }
    if (info == NULL) {
        return false;
    }
#ifdef __APPLE__
    return info->si_code <= 0 || info->si_code >= SI_USER;
#else
    return info->si_code <= 0;
#endif
}

static void append_u32_hex(char *buf, size_t *pos, uint32_t val)
{
    static const char hex[] = "0123456789abcdef";
    for (int shift = 28; shift >= 0; shift -= 4) {
        buf[(*pos)++] = hex[(val >> shift) & 0xFU];
    }
}

static void write_unhandled_sig_msg(int sig, uint32_t guest_pc, uint32_t fault_addr)
{
    char buf[128];
    size_t pos = 0U;
    const char *p1 = "a32rt: unhandled guest signal ";
    const char *p2 = " at guest pc=0x";
    const char *p3 = ", fault_addr=0x";
    while (*p1) buf[pos++] = *p1++;
    if (sig >= 10) buf[pos++] = (char)('0' + (sig / 10) % 10);
    buf[pos++] = (char)('0' + (sig % 10));
    while (*p2) buf[pos++] = *p2++;
    append_u32_hex(buf, &pos, guest_pc);
    while (*p3) buf[pos++] = *p3++;
    append_u32_hex(buf, &pos, fault_addr);
    buf[pos++] = '\n';
    (void)write(STDERR_FILENO, buf, pos);
}

static void host_sig_handler(int sig, siginfo_t *info, void *ucontext)
{
    a32_ctx_t *ctx = g_tls_ctx;
    if (!ctx) {
        if (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE) {
            _exit(128 + sig);
        }
        return;
    }

    bool is_sync_trap = (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL ||
                         sig == SIGFPE  || sig == SIGTRAP);
    if (!is_sync_trap || sig_code_is_user_sent(sig, info)) {
        if (ctx->darwin_external) {
            /* Stash siginfo for the host (SA_SIGINFO/CHLD frames); the
             * bridge run_loop drains pending_signals via poll traps. */
            if (sig >= 1 && sig <= 64) {
                if (info != NULL) g_pending_siginfo[sig] = *info;
                else {
                    memset(&g_pending_siginfo[sig], 0, sizeof(siginfo_t));
                    g_pending_siginfo[sig].si_signo = sig;
                }
            }
            a32_darwin_enqueue_signal(ctx, sig);
        }
#if A32RT_EMBED_IOS
        else if (ctx->is_ios) a32_ios_signals_enqueue_host(ctx, sig, info);
#endif
        else enqueue_async_signal(ctx, sig, info);
        return;
    }

    ucontext_t *uc = (ucontext_t *)ucontext;
#if defined(__APPLE__)
    uintptr_t host_pc = (uintptr_t)uc->uc_mcontext->__ss.__pc;
#else
    uintptr_t host_pc = (uintptr_t)uc->uc_mcontext.pc;
#endif

    uint32_t guest_pc = 0U;
    uint32_t itstate  = 0U;
    bool in_block = a32_jit_lookup_host_pc(host_pc, &guest_pc, &itstate);

    if (!in_block) {
        _exit(128 + sig); /* sync trap outside translated block: terminate. */
    }

    {
        for (int i = 0; i < 15; i++) {
#if defined(__APPLE__)
            ctx->regs[i] = (uint32_t)uc->uc_mcontext->__ss.__x[i];
#else
            ctx->regs[i] = (uint32_t)uc->uc_mcontext.regs[i];
#endif
        }
        ctx->regs[15] = guest_pc;
        ctx->itstate  = itstate;

#if defined(__APPLE__)
        uint32_t pstate = (uint32_t)uc->uc_mcontext->__ss.__cpsr;
#else
        uint32_t pstate = (uint32_t)uc->uc_mcontext.pstate;
#endif
        ctx->cpsr = (ctx->cpsr & ~A32_CPSR_NZCV_MASK) | (pstate & A32_CPSR_NZCV_MASK);

        /* Merge native FPSR cumulative bits into guest FPSCR (A32-R7-5). */
#if defined(__APPLE__)
        uint32_t host_fpsr = uc->uc_mcontext->__ns.__fpsr;
#else
        const uint32_t *fpctx = (const uint32_t *)uc->uc_mcontext.__reserved;
        uint32_t host_fpsr = (fpctx[0] == 0x46508001U) ? fpctx[2] : 0U;
#endif
        ctx->fpscr |= host_fpsr & 0x0800009FU;

        if ((sig == SIGSEGV || sig == SIGBUS) && info != NULL) {
            int wnr = a32_darwin_esr_wnr(ucontext);
            ctx->fault_addr = (uint32_t)((uintptr_t)info->si_addr - ctx->guest_base);
            ctx->fault_is_write = (wnr == 1) ? 1U : 0U;
            ctx->fault_is_prot = a32_guest_range_ok(ctx, ctx->fault_addr, 1U, 0U) ? 1U : 0U;
            ctx->fault_host_valid = (wnr < 0) ? 0U : 1U;
        } else {
            ctx->fault_addr = guest_pc;
            ctx->fault_host_valid = 0U;
        }

        uint32_t exit_code = A32_EXIT_INTERWORK;
        if (ctx->difftest_trap_fault && (sig == SIGSEGV || sig == SIGBUS)) {
            exit_code = A32_EXIT_FAULT;
        } else if (ctx->darwin_external) {
            exit_code = a32_darwin_sync_exit(ctx, sig);
        }
#if A32RT_EMBED_IOS
        else if (ctx->is_ios) {
            /* XNU rule: valid mapping (even PROT_NONE) is SIGBUS, else SIGSEGV. */
            int ios_sig = (a32_guest_range_ok(ctx, ctx->fault_addr, 1U, 0U) &&
                    ctx->fault_addr >= a32_min_va(ctx)) ? SIGBUS : SIGSEGV;
            if (!a32_ios_signals_deliver_sync(ctx, ios_sig, ctx->fault_addr)) {
                write_unhandled_sig_msg(ios_sig, guest_pc, ctx->fault_addr);
                _exit(128 + ios_sig);
            }
        }
#endif
        else {
            guest_sigaction_t *act = &g_guest_sigactions[sig];
            if (act->handler == A32_SIG_IGN) {
                if (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE) {
                    _exit(128 + sig);
                }
                return;
            }
            if (act->handler == A32_SIG_DFL) {
                write_unhandled_sig_msg(sig, guest_pc, ctx->fault_addr);
                _exit(128 + sig);
            }
            a32_frame_deliver(ctx, sig, act, info);
        }
        uint32_t exit_r15 = (exit_code == A32_EXIT_FAULT)
                            ? guest_pc
                            : (ctx->regs[15] | ((ctx->cpsr & A32_CPSR_T) ? 1U : 0U));
        for (int i = 0; i < 15; i++) {
#if defined(__APPLE__)
            uc->uc_mcontext->__ss.__x[i] = (uint64_t)ctx->regs[i];
#else
            uc->uc_mcontext.regs[i] = (uint64_t)ctx->regs[i];
#endif
        }
#if defined(__APPLE__)
        uc->uc_mcontext->__ss.__x[15] = (uint64_t)exit_r15;
        uc->uc_mcontext->__ss.__x[16] = (uint64_t)exit_code;
        uc->uc_mcontext->__ss.__pc    = uc->uc_mcontext->__ss.__lr;
        /* Scalar FPCR restore: a faulting NEON block may leave Standard FPSCR live. */
        uc->uc_mcontext->__ns.__fpcr = ctx->fpscr & 0x07C00000U;
#else
        uc->uc_mcontext.regs[15] = (uint64_t)exit_r15;
        uc->uc_mcontext.regs[16] = (uint64_t)exit_code;
        uc->uc_mcontext.pc       = uc->uc_mcontext.regs[30];
        { /* fpsimd_context scan: fpcr is word 3 past magic 0x46508001. */
            uint32_t *fpw = (uint32_t *)uc->uc_mcontext.__reserved;
            size_t n = sizeof(uc->uc_mcontext.__reserved) >> 2U, i = 0U;
            while (i + 4U <= n && fpw[i] != 0x46508001U) {
                size_t adv = (size_t)(fpw[i + 1U] / 4U);
                if (adv < 4U || adv > n) break;
                i += adv;
            }
            if (i + 4U <= n) fpw[i + 3U] = ctx->fpscr & 0x07C00000U;
        }
#endif
        return;
    }
}

struct a64_kernel_sigaction {
    void (*k_sa_handler)(int, siginfo_t *, void *);
    unsigned long k_sa_flags;
    void (*k_sa_restorer)(void);
    uint64_t k_sa_mask;
};

static int install_host_sigaction(int signum, unsigned long flags)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = host_sig_handler;
    sa.sa_flags     = (int)flags;
    sigfillset(&sa.sa_mask);
    if (sigaction(signum, &sa, NULL) == 0) {
        g_host_handler_installed_mask |= (1ULL << (signum - 1));
        return 0;
    }
#if defined(__linux__)
    if (signum >= 32 && signum <= 34) {
        struct a64_kernel_sigaction ksa;
        memset(&ksa, 0, sizeof(ksa));
        if (syscall(SYS_rt_sigaction, SIGUSR1, NULL, &ksa, (size_t)8) == 0) {
            ksa.k_sa_handler = host_sig_handler;
            ksa.k_sa_flags   = flags | (ksa.k_sa_flags & 0x04000000UL);
            ksa.k_sa_mask    = ~0ULL;
            if (syscall(SYS_rt_sigaction, signum, &ksa, NULL, (size_t)8) == 0) {
                g_host_handler_installed_mask |= (1ULL << (signum - 1));
                return 0;
            }
        }
    }
#endif
    return -1;
}

void a32_signals_ensure_host_handler(int sig)
{
    if (sig >= 1 && sig <= 64 && sig != SIGKILL && sig != SIGSTOP) {
        if (g_guest_sigactions[sig].handler == A32_SIG_DFL &&
            (g_host_handler_installed_mask & (1ULL << (sig - 1))) == 0ULL) {
            (void)install_host_sigaction(sig, (unsigned long)(SA_SIGINFO | SA_NODEFER | SA_RESTART));
        }
    }
}

void a32_signals_preset_siginfo(int sig, const void *host_si)
{
    if (sig >= 1 && sig <= 64 && host_si != NULL) {
        memcpy(&g_preset_siginfo[sig], host_si, sizeof(siginfo_t));
    }
}

void a32_signals_consume_siginfo(int sig, void *guest_si32)
{
    if (sig < 1 || sig > 64) {
        return;
    }
    if (guest_si32 != NULL) {
        a32_siginfo_t info;
        memset(&info, 0, sizeof(info));
        const siginfo_t *host_info = (g_pending_siginfo[sig].si_signo != 0)
                                     ? &g_pending_siginfo[sig] : NULL;
        a32_frame_fill_siginfo(&info, sig, host_info, 0U, true);
        memcpy(guest_si32, &info, sizeof(info));
    }
    memset(&g_pending_siginfo[sig], 0, sizeof(siginfo_t));
}

int a32_signals_init(a32_ctx_t *ctx)
{
    (void)ctx;
    static const int monitored_signals[] = {
        SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP,
        SIGINT, SIGTERM, SIGQUIT, SIGHUP, SIGCHLD,
        SIGUSR1, SIGUSR2, SIGPIPE, SIGALRM, SIGWINCH
    };

    g_host_handler_installed_mask = 0ULL;
    for (size_t i = 0; i < sizeof(monitored_signals) / sizeof(monitored_signals[0]); i++) {
        int s = monitored_signals[i];
        if (install_host_sigaction(s, (unsigned long)(SA_SIGINFO | SA_NODEFER | SA_RESTART)) != 0) {
            fprintf(stderr, "a32rt: failed to install host signal handler for signal %d: %s\n",
                    s, strerror(errno));
            return -1;
        }
    }

    return 0;
}

void a32_signals_check_pending(a32_ctx_t *ctx)
{
    if (!ctx || ctx->darwin_external) return; /* host drains via traps */
#if A32RT_EMBED_IOS
    if (ctx->is_ios) { a32_ios_signals_check_pending(ctx); return; }
#endif
    uint64_t unmasked = __atomic_load_n(&ctx->pending_signals, __ATOMIC_SEQ_CST) & ~ctx->sig_mask;
    while (unmasked != 0ULL) {
        int sig = __builtin_ctzll(unmasked) + 1;
        uint64_t bit = 1ULL << (sig - 1);
        unmasked &= ~bit;
        __atomic_fetch_and(&ctx->pending_signals, ~bit, __ATOMIC_SEQ_CST);
        guest_sigaction_t *act = &g_guest_sigactions[sig];
        if (sig_is_ignored(sig, act->handler)) {
            memset(&g_pending_siginfo[sig], 0, sizeof(siginfo_t));
            continue;
        }
        if (act->handler == A32_SIG_DFL) _exit(128 + sig);
        a32_frame_deliver(ctx, sig, act, &g_pending_siginfo[sig]);
        memset(&g_pending_siginfo[sig], 0, sizeof(siginfo_t));
        break;
    }
}

bool a32_signals_deliver_sync(a32_ctx_t *ctx, int sig, uint32_t fault_addr)
{
    if (!ctx || sig < 1 || sig > 64) return false;
    if (ctx->darwin_external) {
        /* External guests never raise() or build Linux frames: steer
         * sync SIGILL/TRAP/FPE/SEGV/BUS into the bridge trap loop, which
         * reports ILLEGAL/BKPT/FAULT to the host. Never _exit here. */
        ctx->fault_addr = fault_addr;
        ctx->fault_host_valid = 0U;
        if (sig == SIGTRAP) ctx->exit_reason = A32_EXIT_BKPT;
        else if (sig == SIGILL || sig == SIGFPE) ctx->exit_reason = A32_EXIT_UNDEF;
        else ctx->exit_reason = A32_EXIT_FAULT;
        return true;
    }
#if A32RT_EMBED_IOS
    if (ctx->is_ios) return a32_ios_signals_deliver_sync(ctx, sig, fault_addr);
#endif
    ctx->fault_addr = fault_addr;
    guest_sigaction_t *act = &g_guest_sigactions[sig];
    if (act->handler == A32_SIG_DFL || act->handler == A32_SIG_IGN) return false;
    siginfo_t si;
    memset(&si, 0, sizeof(si));
    si.si_signo = sig; si.si_code = 1;
    a32_frame_deliver(ctx, sig, act, &si);
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
    if ((act_va != 0U && !a32_guest_range_ok(ctx, act_va, sizeof(guest_sigaction_t), PROT_READ)) ||
        (oldact_va != 0U && !a32_guest_range_ok(ctx, oldact_va, sizeof(guest_sigaction_t), PROT_WRITE))) return -EFAULT;

    guest_sigaction_t cur = g_guest_sigactions[signum];

    if (act_va != 0U) {
        guest_sigaction_t new_act;
        if (!guest_read(ctx, &new_act, act_va, sizeof(new_act))) return -EFAULT;
        unsigned long sa_flags = (unsigned long)(SA_SIGINFO | SA_NODEFER);
        if (new_act.handler <= 1U || (new_act.flags & A32_SA_RESTART) != 0U) {
            sa_flags |= (unsigned long)SA_RESTART;
        }
        if (signum == SIGCHLD) {
            if ((new_act.flags & A32_SA_NOCLDSTOP) != 0U) {
                sa_flags |= (unsigned long)SA_NOCLDSTOP;
            }
            if ((new_act.flags & A32_SA_NOCLDWAIT) != 0U || new_act.handler == A32_SIG_IGN) {
                sa_flags |= (unsigned long)SA_NOCLDWAIT;
            }
        }
#if defined(__linux__)
        if (install_host_sigaction(signum, sa_flags) != 0) {
            return -EINVAL;
        }
#else
        if (signum < NSIG && install_host_sigaction(signum, sa_flags) != 0) {
            return -EINVAL;
        }
#endif
        g_guest_sigactions[signum] = new_act;
        g_guest_sigactions[signum].mask &= ~A32_UNMASKABLE_SIGNALS;
        ctx->sig_mask &= ~A32_UNMASKABLE_SIGNALS;

        if (sig_is_ignored(signum, g_guest_sigactions[signum].handler)) {
            __atomic_fetch_and(&ctx->pending_signals, ~(1ULL << (signum - 1)), __ATOMIC_SEQ_CST);
        }
    }

    if (oldact_va != 0U) {
        if (!guest_write(ctx, oldact_va, &cur, sizeof(cur))) return -EFAULT;
    }

    return 0;
}

int a32_sys_rt_sigprocmask(a32_ctx_t *ctx, int how, uint32_t set_va,
                           uint32_t oldset_va, uint32_t sigsetsize)
{
    if (!ctx || sigsetsize != sizeof(uint64_t)) return -EINVAL;
    if ((set_va != 0U && !a32_guest_range_ok(ctx, set_va, 8U, (uint32_t)PROT_READ)) ||
        (oldset_va != 0U && !a32_guest_range_ok(ctx, oldset_va, 8U, (uint32_t)PROT_WRITE))) return -EFAULT;

    uint64_t old_mask = ctx->sig_mask;

    if (set_va != 0U) {
        uint64_t set = 0ULL;
        memcpy(&set, a32_guest_ptr(ctx, set_va), sizeof(set));
        set &= ~A32_UNMASKABLE_SIGNALS;

        switch (how) {
        case 0: /* Linux SIG_BLOCK */
            ctx->sig_mask |= set;
            break;
        case 1: /* Linux SIG_UNBLOCK */
            ctx->sig_mask &= ~set;
            break;
        case 2: /* Linux SIG_SETMASK */
            ctx->sig_mask = set;
            break;
        default:
            return -EINVAL;
        }

        if (how == 0 || how == 2) {
            uint64_t need_install = ctx->sig_mask & ~g_host_handler_installed_mask;
            while (need_install != 0ULL) {
                int s = __builtin_ctzll(need_install) + 1;
                need_install &= need_install - 1ULL;
                if (s == SIGKILL || s == SIGSTOP || s == SIGTSTP ||
                    s == SIGTTIN || s == SIGTTOU || s == SIGCONT) {
                    continue;
                }
                (void)install_host_sigaction(s, (unsigned long)(SA_SIGINFO | SA_NODEFER | SA_RESTART));
            }
        }
    }

    if (oldset_va != 0U) {
        memcpy(a32_guest_ptr(ctx, oldset_va), &old_mask, sizeof(old_mask));
    }

    return 0;
}

/* Ignored-disposition snapshot for execve carry. */
uint64_t a32_signals_ignored_mask(void)
{
    uint64_t mask = 0ULL;
    for (int s = 1; s <= 64; s++) {
        if (g_guest_sigactions[s].handler == A32_SIG_IGN) {
            mask |= 1ULL << (s - 1);
        }
    }
    return mask;
}

/* Post-exec restore of ignored dispositions carried across re-exec. */
void a32_signals_mark_ignored(uint64_t ign_mask)
{
    for (int s = 1; s <= 64; s++) {
        if ((ign_mask & (1ULL << (s - 1))) != 0ULL &&
            s != SIGKILL && s != SIGSTOP) {
            g_guest_sigactions[s].handler = A32_SIG_IGN;
            (void)install_host_sigaction(s,
                (unsigned long)(SA_SIGINFO | SA_NODEFER | SA_RESTART));
        }
    }
}
