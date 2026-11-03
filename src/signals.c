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

#ifndef SI_TKILL
#define SI_TKILL (-6)
#endif
#ifndef SI_KERNEL
#define SI_KERNEL 0x80
#endif

#define A32_SIGFRAME_MAGIC 0x53494746U /* "SIGF" */
#define A32_VFP_MAGIC      0x56465001U /* VFP magic */
#define A32_FPEXC_EN       0x40000000U /* FPEXC_EN */

#define A32_SIG_DFL      0U
#define A32_SIG_IGN      1U

#define A32_SA_NOCLDSTOP 0x00000001U
#define A32_SA_NOCLDWAIT 0x00000002U
#define A32_SA_SIGINFO   0x00000004U
#define A32_SA_RESTORER  0x04000000U
#define A32_SA_ONSTACK   0x08000000U
#define A32_SA_RESTART   0x10000000U
#define A32_SA_NODEFER   0x40000000U
#define A32_SA_RESETHAND 0x80000000U

#define A32_SS_ONSTACK   1U
#define A32_SS_DISABLE   2U

#define A32_UNMASKABLE_SIGNALS ((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)))

typedef struct __attribute__((packed, aligned(4))) guest_sigaction {
    uint32_t handler;
    uint32_t flags;
    uint32_t restorer;
    uint64_t mask;
} guest_sigaction_t;

_Static_assert(sizeof(guest_sigaction_t) == 20, "guest_sigaction_t must be 20 bytes");

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
_Static_assert(offsetof(a32_sigcontext_t, arm_pc) - offsetof(a32_sigcontext_t, arm_r0) ==
               15U * sizeof(uint32_t),
               "a32_sigcontext_t arm_r0..arm_pc must be contiguous");

typedef struct a32_stack {
    uint32_t ss_sp;
    int32_t  ss_flags;
    uint32_t ss_size;
} a32_stack_t;

/* VFP sigframe in uc_regspace: 0x120 bytes. */
typedef struct a32_vfp_sigframe {
    uint32_t magic; uint32_t size; uint64_t fpregs[32];
    uint32_t fpscr; uint32_t fpexc; uint32_t fpinst; uint32_t fpinst2;
    uint32_t pad[2];
} a32_vfp_sigframe_t;

_Static_assert(sizeof(a32_vfp_sigframe_t) == 0x120,
               "a32_vfp_sigframe_t must be 288 (0x120) bytes");

typedef struct a32_ucontext {
    uint32_t         uc_flags;
    uint32_t         uc_link;
    a32_stack_t      uc_stack;
    a32_sigcontext_t uc_mcontext;
    uint64_t         uc_sigmask;
    uint32_t         _unused[30];
    _Alignas(8) uint32_t uc_regspace[128];
} a32_ucontext_t;

_Static_assert(offsetof(a32_ucontext_t, uc_mcontext.arm_r0) == 32,
               "arm_r0 must be at offset 32 from start of ucontext");
_Static_assert(offsetof(a32_ucontext_t, uc_regspace) == 232,
               "uc_regspace must be at offset 232 from start of ucontext");

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
    uint32_t       saved_altstack_sp;
    uint32_t       saved_altstack_size;
    uint32_t       saved_altstack_flags;
    uint32_t       saved_altstack_pad;
} a32_rt_sigframe_t;

_Static_assert(offsetof(a32_rt_sigframe_t, info) == 0,
               "rt_sigframe info must be at offset 0");
_Static_assert(offsetof(a32_rt_sigframe_t, uc) == 128,
               "rt_sigframe uc must be at offset 128");
_Static_assert(offsetof(a32_rt_sigframe_t, saved_mask) == 968,
               "rt_sigframe saved_mask must be at offset 968");

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

static bool is_on_altstack(const a32_ctx_t *ctx, uint32_t sp)
{
    return (ctx->altstack_size > 0U &&
            !(ctx->altstack_flags & A32_SS_DISABLE) &&
            sp >= ctx->altstack_sp &&
            (sp - ctx->altstack_sp) < ctx->altstack_size);
}

static int32_t get_altstack_ss_flags(const a32_ctx_t *ctx, bool on_altstack)
{
    if (ctx->altstack_size == 0U || (ctx->altstack_flags & A32_SS_DISABLE)) {
        return (int32_t)A32_SS_DISABLE;
    }
    return (int32_t)((on_altstack ? A32_SS_ONSTACK : 0U) |
                     (ctx->altstack_flags & 0x80000000U));
}

static bool sig_is_ignored(int sig, uint32_t handler)
{
    return (handler == A32_SIG_IGN ||
            (handler == A32_SIG_DFL &&
             (sig == SIGCHLD || sig == SIGURG || sig == SIGWINCH || sig == SIGCONT)));
}

static void fill_guest_siginfo(a32_siginfo_t *dst, int sig,
                               const siginfo_t *host_info,
                               uint32_t fault_addr, bool for_sigwait)
{
    dst->si_signo = sig;
    if (host_info != NULL) {
        dst->si_errno = host_info->si_errno;
        dst->si_code  = host_info->si_code;
        if (host_info->si_code < 0) {
            dst->_pad[0] = (uint32_t)host_info->si_pid;
            dst->_pad[1] = (uint32_t)host_info->si_uid;
            if (!for_sigwait || host_info->si_code != SI_TKILL) {
                dst->_pad[2] = (uint32_t)host_info->si_value.sival_int;
            }
        } else if (host_info->si_code == 0 || sig == SIGCHLD) {
            dst->_pad[0] = (uint32_t)host_info->si_pid;
            dst->_pad[1] = (uint32_t)host_info->si_uid;
            dst->_pad[2] = (uint32_t)host_info->si_status;
        } else if (!for_sigwait &&
                   (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL ||
                    sig == SIGFPE || sig == SIGTRAP) &&
                   host_info->si_code > 0) {
            dst->_pad[0] = fault_addr;
        }
    } else if (for_sigwait) {
        dst->si_code = SI_TKILL;
    } else {
        dst->si_code = (sig == SIGSEGV ? SEGV_MAPERR :
                        sig == SIGILL  ? ILL_ILLOPC  :
                        sig == SIGFPE  ? FPE_INTDIV  :
                        sig == SIGTRAP ? TRAP_BRKPT  :
                        SI_KERNEL);
    }
}

static void deliver_guest_signal(a32_ctx_t *ctx, int sig, guest_sigaction_t *act,
                                 const siginfo_t *host_info)
{
    uint32_t sp = ctx->regs[13];
    bool on_altstack = is_on_altstack(ctx, sp);
    bool switched_to_altstack = false;
    if ((act->flags & A32_SA_ONSTACK) &&
        !on_altstack &&
        ctx->altstack_size > 0U &&
        !(ctx->altstack_flags & A32_SS_DISABLE)) {
        sp = ctx->altstack_sp + ctx->altstack_size;
        switched_to_altstack = true;
    }

    sp = (sp - (uint32_t)sizeof(a32_rt_sigframe_t)) & ~7U;
    uint32_t frame_va = sp;
    bool frame_ok = (a32_vm32_arena_base() != 0U)
        ? a32_vm32_range_prot_ok(frame_va, (uint32_t)sizeof(a32_rt_sigframe_t), (uint32_t)PROT_WRITE)
        : a32_guest_range_ok(ctx, frame_va, (uint32_t)sizeof(a32_rt_sigframe_t), (uint32_t)PROT_WRITE);
    if (frame_va < 0x1000U || (uint64_t)frame_va + sizeof(a32_rt_sigframe_t) > 0xFFFF0000ULL || !frame_ok) {
        _exit(128 + SIGSEGV);
    }
    a32_rt_sigframe_t *frame = (a32_rt_sigframe_t *)a32_guest_ptr(ctx, frame_va);
    memset(frame, 0, sizeof(*frame));
    a32_vm32_copyout_invalidate_exec(ctx, frame_va, sizeof(*frame));

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

    fill_guest_siginfo(&frame->info, sig, host_info, ctx->fault_addr, false);

    frame->uc.uc_flags = 0;
    frame->uc.uc_link  = 0;
    frame->uc.uc_stack.ss_sp    = ctx->altstack_sp;
    frame->uc.uc_stack.ss_size  = ctx->altstack_size;
    frame->uc.uc_stack.ss_flags = get_altstack_ss_flags(ctx, on_altstack);
    if ((switched_to_altstack || on_altstack) &&
        (act->flags & A32_SA_ONSTACK) != 0U &&
        (ctx->altstack_flags & 0x80000000U) != 0U) {
        frame->saved_pad            = 1U;
        frame->saved_altstack_sp    = ctx->altstack_sp;
        frame->saved_altstack_size  = ctx->altstack_size;
        frame->saved_altstack_flags = ctx->altstack_flags;
        ctx->altstack_sp            = 0U;
        ctx->altstack_size          = 0U;
        ctx->altstack_flags         = A32_SS_DISABLE;
    }

    /* Pack the architectural CPSR.IT bits from ctx->itstate so the handler
     * observes and may replace the interrupted predication state through
     * its ucontext (A32-R7-14). ITSTATE[7:2] -> CPSR[15:10], ITSTATE[1:0]
     * -> CPSR[26:25]. */
    uint32_t it = ctx->itstate & 0xFFU;
    uint32_t arm_cpsr = (ctx->cpsr & ~A32_CPSR_IT_MASK) |
        (((it & 0xFCU) << 8) | ((it & 0x03U) << 25));

    frame->uc.uc_mcontext.trap_no       = 0;
    frame->uc.uc_mcontext.error_code    = 0;
    frame->uc.uc_mcontext.oldmask       = (uint32_t)ctx->sig_mask;
    memcpy(&frame->uc.uc_mcontext.arm_r0, ctx->regs, sizeof(ctx->regs));
    frame->uc.uc_mcontext.arm_cpsr      = arm_cpsr;
    frame->uc.uc_mcontext.fault_address = ctx->fault_addr;
    frame->uc.uc_sigmask                = ctx->sig_mask;

    a32_vfp_sigframe_t *vfp = (a32_vfp_sigframe_t *)(void *)frame->uc.uc_regspace;
    vfp->magic   = A32_VFP_MAGIC;
    vfp->size    = (uint32_t)sizeof(a32_vfp_sigframe_t);
    memcpy(vfp->fpregs, ctx->vfp_d, sizeof(vfp->fpregs));
    vfp->fpscr   = ctx->fpscr;
    vfp->fpexc   = A32_FPEXC_EN;
    vfp->fpinst  = 0U;
    vfp->fpinst2 = 0U;
    frame->uc.uc_regspace[sizeof(a32_vfp_sigframe_t) / sizeof(uint32_t)] = 0U;

    if (!(act->flags & A32_SA_NODEFER)) {
        ctx->sig_mask |= (1ULL << (sig - 1));
    }
    ctx->sig_mask |= act->mask;
    ctx->sig_mask &= ~A32_UNMASKABLE_SIGNALS;

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
        act->handler = A32_SIG_DFL;
    }
    a32_set_pc_interwork(ctx, handler);
    ctx->sync_sig_delivered = true;
    ctx->exit_reason = A32_EXIT_BRANCH;
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
        enqueue_async_signal(ctx, sig, info);
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

        /* Merge native FPSR cumulative exception bits into guest FPSCR
         * before constructing the signal frame (A32-R7-5). */
#if defined(__APPLE__)
        uint32_t host_fpsr = uc->uc_mcontext->__ns.__fpsr;
#else
        const uint32_t *fpctx = (const uint32_t *)uc->uc_mcontext.__reserved;
        uint32_t host_fpsr = (fpctx[0] == 0x46508001U) ? fpctx[2] : 0U;
#endif
        ctx->fpscr |= host_fpsr & 0x0800009FU;

        if ((sig == SIGSEGV || sig == SIGBUS) && info != NULL) {
            ctx->fault_addr = (uint32_t)((uintptr_t)info->si_addr - ctx->guest_base);
        } else {
            ctx->fault_addr = guest_pc;
        }

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

        deliver_guest_signal(ctx, sig, act, info);

        for (int i = 0; i < 15; i++) {
#if defined(__APPLE__)
            uc->uc_mcontext->__ss.__x[i] = (uint64_t)ctx->regs[i];
#else
            uc->uc_mcontext.regs[i] = (uint64_t)ctx->regs[i];
#endif
        }
#if defined(__APPLE__)
        uc->uc_mcontext->__ss.__x[15] = (uint64_t)(ctx->regs[15] | ((ctx->cpsr & A32_CPSR_T) ? 1U : 0U));
        uc->uc_mcontext->__ss.__x[16] = (uint64_t)A32_EXIT_INTERWORK;
        uc->uc_mcontext->__ss.__pc    = uc->uc_mcontext->__ss.__lr;
#else
        uc->uc_mcontext.regs[15] = (uint64_t)(ctx->regs[15] | ((ctx->cpsr & A32_CPSR_T) ? 1U : 0U));
        uc->uc_mcontext.regs[16] = (uint64_t)A32_EXIT_INTERWORK;
        uc->uc_mcontext.pc       = uc->uc_mcontext.regs[30];
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
        fill_guest_siginfo(&info, sig, host_info, 0U, true);
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
    if (!ctx) {
        return;
    }

    uint64_t pending = __atomic_load_n(&ctx->pending_signals, __ATOMIC_SEQ_CST);
    uint64_t unmasked = pending & ~ctx->sig_mask;
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
        if (act->handler == A32_SIG_DFL) {
            _exit(128 + sig);
        }
        deliver_guest_signal(ctx, sig, act, &g_pending_siginfo[sig]);
        memset(&g_pending_siginfo[sig], 0, sizeof(siginfo_t));
        break;
    }
}

bool a32_signals_deliver_sync(a32_ctx_t *ctx, int sig, uint32_t fault_addr)
{
    if (!ctx || sig < 1 || sig > 64) {
        return false;
    }
    ctx->fault_addr = fault_addr;
    guest_sigaction_t *act = &g_guest_sigactions[sig];
    if (act->handler == A32_SIG_DFL || act->handler == A32_SIG_IGN) {
        return false;
    }
    siginfo_t si;
    memset(&si, 0, sizeof(si));
    si.si_signo = sig;
    si.si_code  = 1;
    deliver_guest_signal(ctx, sig, act, &si);
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
        (oldact_va != 0U && !a32_guest_range_ok(ctx, oldact_va, sizeof(guest_sigaction_t), PROT_WRITE))) {
        return -EFAULT;
    }

    guest_sigaction_t cur = g_guest_sigactions[signum];

    if (act_va != 0U) {
        guest_sigaction_t new_act;
        if (!guest_read(ctx, &new_act, act_va, sizeof(new_act))) {
            return -EFAULT;
        }
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
        if (!guest_write(ctx, oldact_va, &cur, sizeof(cur))) {
            return -EFAULT;
        }
    }

    return 0;
}

int a32_sys_rt_sigprocmask(a32_ctx_t *ctx, int how, uint32_t set_va,
                           uint32_t oldset_va, uint32_t sigsetsize)
{
    if (!ctx || sigsetsize != sizeof(uint64_t)) {
        return -EINVAL;
    }
    if ((set_va != 0U && !a32_guest_range_ok(ctx, set_va, 8U, (uint32_t)PROT_READ)) ||
        (oldset_va != 0U && !a32_guest_range_ok(ctx, oldset_va, 8U, (uint32_t)PROT_WRITE))) {
        return -EFAULT;
    }

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

int a32_sys_rt_sigreturn(a32_ctx_t *ctx)
{
    if (!ctx) {
        return -EINVAL;
    }

    uint32_t sp = ctx->regs[13];
    if (!a32_guest_range_ok(ctx, sp, (uint32_t)sizeof(a32_rt_sigframe_t), (uint32_t)PROT_READ)) {
        return -EFAULT;
    }
    a32_rt_sigframe_t *frame = (a32_rt_sigframe_t *)a32_guest_ptr(ctx, sp);

    const a32_sigcontext_t *sc = &frame->uc.uc_mcontext;
    memcpy(ctx->regs, &sc->arm_r0, sizeof(ctx->regs));
    /* Restore predication from the architectural CPSR.IT bits supplied in
     * the user context and keep them out of the stored CPSR, which holds
     * IT separately in ctx->itstate (A32-R7-14). */
    uint32_t user_it = ((sc->arm_cpsr >> 8) & 0xFCU) | ((sc->arm_cpsr >> 25) & 0x03U);
    if ((user_it & 0x0FU) == 0U || (user_it & 0xF0U) == 0xF0U) {
        user_it = 0U;
    }
    ctx->cpsr     = sc->arm_cpsr & ~A32_CPSR_IT_MASK;
    ctx->sig_mask = frame->uc.uc_sigmask & ~A32_UNMASKABLE_SIGNALS;
    if (frame->magic == A32_SIGFRAME_MAGIC) {
        if (frame->saved_pad == 1U && (frame->saved_altstack_flags & 0x80000000U) != 0U) {
            ctx->altstack_sp    = frame->saved_altstack_sp;
            ctx->altstack_size  = frame->saved_altstack_size;
            ctx->altstack_flags = frame->saved_altstack_flags;
        }
        ctx->fault_addr = frame->saved_fault;
        ctx->itstate = user_it;
    } else {
        ctx->itstate = user_it;
    }

    const a32_vfp_sigframe_t *vfp = (const a32_vfp_sigframe_t *)(const void *)frame->uc.uc_regspace;
    if (vfp->magic == A32_VFP_MAGIC && vfp->size == (uint32_t)sizeof(a32_vfp_sigframe_t)) {
        memcpy(ctx->vfp_d, vfp->fpregs, sizeof(ctx->vfp_d));
        ctx->fpscr = vfp->fpscr;
    } else if (frame->magic == A32_SIGFRAME_MAGIC) {
        memcpy(ctx->vfp_d, frame->saved_vfp_d, sizeof(ctx->vfp_d));
        ctx->fpscr = frame->saved_fpscr;
    }

    a32_normalize_pc(ctx);

    ctx->exit_reason = A32_EXIT_BRANCH;
    return 0;
}

int a32_sys_sigaltstack(a32_ctx_t *ctx, uint32_t uss_va, uint32_t uoss_va)
{
    if (!ctx) {
        return -EINVAL;
    }
    if ((uss_va != 0U && !a32_guest_range_ok(ctx, uss_va, 12U, (uint32_t)PROT_READ)) ||
        (uoss_va != 0U && !a32_guest_range_ok(ctx, uoss_va, 12U, (uint32_t)PROT_WRITE))) {
        return -EFAULT;
    }

    bool on_altstack = is_on_altstack(ctx, ctx->regs[13]);

    a32_stack_t old_st;
    old_st.ss_sp    = ctx->altstack_sp;
    old_st.ss_flags = get_altstack_ss_flags(ctx, on_altstack);
    old_st.ss_size  = ctx->altstack_size;

    if (uss_va != 0U) {
        if (on_altstack) {
            return -EPERM;
        }
        a32_stack_t new_st;
        memcpy(&new_st, a32_guest_ptr(ctx, uss_va), sizeof(new_st));

        uint32_t uflags = (uint32_t)new_st.ss_flags;
        if ((uflags & ~(A32_SS_DISABLE | 0x80000000U)) != 0U) {
            return -EINVAL;
        }

        if (uflags & A32_SS_DISABLE) {
            ctx->altstack_sp    = 0U;
            ctx->altstack_size  = 0U;
            ctx->altstack_flags = A32_SS_DISABLE;
        } else {
            if (new_st.ss_size < 2048U) {
                return -ENOMEM;
            }
            ctx->altstack_sp    = new_st.ss_sp;
            ctx->altstack_size  = new_st.ss_size;
            ctx->altstack_flags = uflags;
        }
    }

    if (uoss_va != 0U) {
        memcpy(a32_guest_ptr(ctx, uoss_va), &old_st, sizeof(old_st));
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
