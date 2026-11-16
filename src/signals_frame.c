/*
 * src/signals_frame.c — arm32 rt_sigframe build/restore & sigaltstack
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Factored out of src/signals.c so every .c file stays under the
 * 900-line limit. Covers the guest-visible sigframe layout
 * (sigcontext, ucontext, VFP regspace), frame delivery, rt_sigreturn,
 * and sigaltstack.
 */

#define _GNU_SOURCE
#include "signals_frame.h"
#include "vm/vm32.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

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

static bool is_on_altstack(const a32_ctx_t *ctx, uint32_t sp)
{
    return (ctx->altstack_size > 0U && !(ctx->altstack_flags & A32_SS_DISABLE) &&
            sp >= ctx->altstack_sp && (sp - ctx->altstack_sp) < ctx->altstack_size);
}

static int32_t get_altstack_ss_flags(const a32_ctx_t *ctx, bool on_altstack)
{
    if (ctx->altstack_size == 0U || (ctx->altstack_flags & A32_SS_DISABLE)) {
        return (int32_t)A32_SS_DISABLE;
    }
    return (int32_t)((on_altstack ? A32_SS_ONSTACK : 0U) | (ctx->altstack_flags & 0x80000000U));
}

void a32_frame_fill_siginfo(a32_siginfo_t *dst, int sig,
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

void a32_frame_deliver(a32_ctx_t *ctx, int sig, guest_sigaction_t *act,
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

    a32_frame_fill_siginfo(&frame->info, sig, host_info, ctx->fault_addr, false);

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

    /* Pack CPSR.IT from ctx->itstate (A32-R7-14): [7:2]->[15:10], [1:0]->[26:25]. */
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

int a32_sys_rt_sigreturn(a32_ctx_t *ctx)
{
    if (!ctx) return -EINVAL;
    uint32_t sp = ctx->regs[13];
    if (!a32_guest_range_ok(ctx, sp, (uint32_t)sizeof(a32_rt_sigframe_t), (uint32_t)PROT_READ)) return -EFAULT;
    a32_rt_sigframe_t *frame = (a32_rt_sigframe_t *)a32_guest_ptr(ctx, sp);
    const a32_sigcontext_t *sc = &frame->uc.uc_mcontext;
    memcpy(ctx->regs, &sc->arm_r0, sizeof(ctx->regs));
    /* Restore predication from architectural CPSR.IT bits in ucontext (A32-R7-14). */
    uint32_t user_it = ((sc->arm_cpsr >> 8) & 0xFCU) | ((sc->arm_cpsr >> 25) & 0x03U);
    if ((user_it & 0x0FU) == 0U || (user_it & 0xF0U) == 0xF0U) user_it = 0U;
    ctx->cpsr     = (sc->arm_cpsr & 0xF80F0020U) | 0x10U;
    ctx->sig_mask = frame->uc.uc_sigmask & ~A32_UNMASKABLE_SIGNALS;
    if (frame->magic == A32_SIGFRAME_MAGIC) {
        if (frame->saved_pad == 1U && (frame->saved_altstack_flags & 0x80000000U) != 0U) {
            ctx->altstack_sp    = frame->saved_altstack_sp;
            ctx->altstack_size  = frame->saved_altstack_size;
            ctx->altstack_flags = frame->saved_altstack_flags;
        }
        ctx->fault_addr = frame->saved_fault;
    }
    ctx->itstate = user_it;

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
        if (on_altstack) return -EPERM;
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
            if (new_st.ss_size < 2048U) return -ENOMEM;
            ctx->altstack_sp    = new_st.ss_sp;
            ctx->altstack_size  = new_st.ss_size;
            ctx->altstack_flags = uflags;
        }
    }

    if (uoss_va != 0U) memcpy(a32_guest_ptr(ctx, uoss_va), &old_st, sizeof(old_st));

    return 0;
}
