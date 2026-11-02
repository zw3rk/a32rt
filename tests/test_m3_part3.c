/*
 * tests/test_m3_part3.c — M3 threads & signals tests (part 3: tests 17..19)
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_m3_common.h"

static volatile uint32_t g_r6_usr2_a_hits = 0U;
static volatile uint32_t g_r6_usr2_b_hits = 0U;
static volatile uint32_t g_r6_autodisarm_result = 0U;

static void r6_usr2_handler_a(int sig, void *info, void *ucontext)
{
    (void)sig;
    (void)info;
    (void)ucontext;
    g_r6_usr2_a_hits++;
}

static void r6_usr2_handler_b(int sig, void *info, void *ucontext)
{
    (void)sig;
    (void)info;
    (void)ucontext;
    g_r6_usr2_b_hits++;
}

static void r6_autodisarm_handler(int sig, void *info, void *ucontext)
{
    (void)sig;
    (void)info;
    (void)ucontext;

    uint32_t cur_sp = 0U;
    __asm__ volatile("mov %0, sp" : "=r"(cur_sp));

    if (cur_sp < g_altstack_base || cur_sp >= g_altstack_base + g_altstack_size) {
        g_r6_autodisarm_result = 1U;
        return;
    }

    struct stack32_m3 oss = { 0U, 0, 0U };
    if (sys2(186U /* sigaltstack */, 0U, (uint32_t)(uintptr_t)&oss) != 0) {
        g_r6_autodisarm_result = 2U;
        return;
    }
    /* Inside handler with SS_AUTODISARM, sigaltstack must report SS_DISABLE (2) */
    if (oss.ss_flags != 2 /* SS_DISABLE */) {
        g_r6_autodisarm_result = 3U;
        return;
    }

    g_r6_autodisarm_result = 100U;
}

static int test17_r6_signal_aliasing_ign_autodisarm(void)
{
    const uint32_t sigusr2 = 12U;
    const uint64_t usr2_bit = 1ULL << (sigusr2 - 1U);
    uint32_t tid = (uint32_t)sys1(224U /* gettid */, 0U);

    /* Ensure SIGUSR2 starts unblocked */
    uint64_t saved_mask = 0ULL;
    if (sys4(175U /* rt_sigprocmask */, 1U /* SIG_UNBLOCK */,
             (uint32_t)(uintptr_t)&usr2_bit,
             (uint32_t)(uintptr_t)&saved_mask, 8U) != 0) {
        return 138;
    }

    /*
     * 17a-1: rt_sigaction(SIGUSR2, &sa, &sa) in-place aliasing (act_va == oldact_va)
     */
    struct sigaction32 orig_sa;
    struct sigaction32 sa;
    sa.sa_handler  = (uint32_t)(uintptr_t)r6_usr2_handler_a;
    sa.sa_flags    = 0x04000004U; /* SA_SIGINFO | SA_RESTORER */
    sa.sa_restorer = (uint32_t)(uintptr_t)my_rt_restorer;
    sa.sa_mask[0]  = 0U;
    sa.sa_mask[1]  = 0U;

    if (sys4(174U /* rt_sigaction */, sigusr2,
             (uint32_t)(uintptr_t)&sa, (uint32_t)(uintptr_t)&orig_sa, 8U) != 0) {
        return 139;
    }

    /* Now replace handler_a with handler_b using the SAME struct for act and oldact */
    struct sigaction32 sa_inplace;
    sa_inplace.sa_handler  = (uint32_t)(uintptr_t)r6_usr2_handler_b;
    sa_inplace.sa_flags    = 0x04000004U;
    sa_inplace.sa_restorer = (uint32_t)(uintptr_t)my_rt_restorer;
    sa_inplace.sa_mask[0]  = 0U;
    sa_inplace.sa_mask[1]  = 0U;

    if (sys4(174U /* rt_sigaction */, sigusr2,
             (uint32_t)(uintptr_t)&sa_inplace,
             (uint32_t)(uintptr_t)&sa_inplace, 8U) != 0) {
        sys4(174U /* rt_sigaction */, sigusr2, (uint32_t)(uintptr_t)&orig_sa, 0U, 8U);
        return 140;
    }
    /* Verify old handler (handler_a) was written back into sa_inplace */
    if (sa_inplace.sa_handler != (uint32_t)(uintptr_t)r6_usr2_handler_a) {
        sys4(174U /* rt_sigaction */, sigusr2, (uint32_t)(uintptr_t)&orig_sa, 0U, 8U);
        return 141;
    }

    /* Verify new handler (handler_b) was actually installed and fires on tkill */
    g_r6_usr2_a_hits = 0U;
    g_r6_usr2_b_hits = 0U;
    if (sys2(238U /* tkill */, tid, sigusr2) != 0 ||
        g_r6_usr2_a_hits != 0U || g_r6_usr2_b_hits != 1U) {
        sys4(174U /* rt_sigaction */, sigusr2, (uint32_t)(uintptr_t)&orig_sa, 0U, 8U);
        return 142;
    }

    /*
     * 17a-2: rt_sigprocmask(SIG_BLOCK, &mask, &mask) in-place aliasing (set_va == oldset_va)
     */
    uint64_t mask_inplace = usr2_bit;
    if (sys4(175U /* rt_sigprocmask */, 0U /* SIG_BLOCK */,
             (uint32_t)(uintptr_t)&mask_inplace,
             (uint32_t)(uintptr_t)&mask_inplace, 8U) != 0) {
        sys4(174U /* rt_sigaction */, sigusr2, (uint32_t)(uintptr_t)&orig_sa, 0U, 8U);
        return 143;
    }
    /* Previous mask must have had SIGUSR2 unblocked */
    if ((mask_inplace & usr2_bit) != 0ULL) {
        sys4(175U /* rt_sigprocmask */, 2U /* SIG_SETMASK */,
             (uint32_t)(uintptr_t)&saved_mask, 0U, 8U);
        sys4(174U /* rt_sigaction */, sigusr2, (uint32_t)(uintptr_t)&orig_sa, 0U, 8U);
        return 144;
    }

    /* Query current mask and verify SIGUSR2 is now blocked */
    uint64_t cur_mask = 0ULL;
    if (sys4(175U /* rt_sigprocmask */, 0U /* SIG_BLOCK */, 0U,
             (uint32_t)(uintptr_t)&cur_mask, 8U) != 0 ||
        (cur_mask & usr2_bit) == 0ULL) {
        sys4(175U /* rt_sigprocmask */, 2U /* SIG_SETMASK */,
             (uint32_t)(uintptr_t)&saved_mask, 0U, 8U);
        sys4(174U /* rt_sigaction */, sigusr2, (uint32_t)(uintptr_t)&orig_sa, 0U, 8U);
        return 145;
    }

    /*
     * 17b: Setting a pending blocked signal to SIG_IGN via rt_sigaction must
     *      discard the pending signal so it does NOT fire when unblocked.
     */
    g_r6_usr2_b_hits = 0U;
    /* Send SIGUSR2 while it is blocked */
    if (sys2(238U /* tkill */, tid, sigusr2) != 0 || g_r6_usr2_b_hits != 0U) {
        sys4(175U /* rt_sigprocmask */, 2U /* SIG_SETMASK */,
             (uint32_t)(uintptr_t)&saved_mask, 0U, 8U);
        sys4(174U /* rt_sigaction */, sigusr2, (uint32_t)(uintptr_t)&orig_sa, 0U, 8U);
        return 146;
    }

    /* Set disposition to SIG_IGN (1) while SIGUSR2 is pending and blocked */
    struct sigaction32 sa_ign;
    sa_ign.sa_handler  = 1U; /* SIG_IGN */
    sa_ign.sa_flags    = 0U;
    sa_ign.sa_restorer = 0U;
    sa_ign.sa_mask[0]  = 0U;
    sa_ign.sa_mask[1]  = 0U;
    if (sys4(174U /* rt_sigaction */, sigusr2,
             (uint32_t)(uintptr_t)&sa_ign, 0U, 8U) != 0) {
        sys4(175U /* rt_sigprocmask */, 2U /* SIG_SETMASK */,
             (uint32_t)(uintptr_t)&saved_mask, 0U, 8U);
        sys4(174U /* rt_sigaction */, sigusr2, (uint32_t)(uintptr_t)&orig_sa, 0U, 8U);
        return 147;
    }

    /* Unblock SIGUSR2 in-place (&unblock_mask, &unblock_mask): must NOT fire */
    uint64_t unblock_inplace = usr2_bit;
    if (sys4(175U /* rt_sigprocmask */, 1U /* SIG_UNBLOCK */,
             (uint32_t)(uintptr_t)&unblock_inplace,
             (uint32_t)(uintptr_t)&unblock_inplace, 8U) != 0 ||
        (unblock_inplace & usr2_bit) == 0ULL ||
        g_r6_usr2_b_hits != 0U) {
        sys4(175U /* rt_sigprocmask */, 2U /* SIG_SETMASK */,
             (uint32_t)(uintptr_t)&saved_mask, 0U, 8U);
        sys4(174U /* rt_sigaction */, sigusr2, (uint32_t)(uintptr_t)&orig_sa, 0U, 8U);
        return 148;
    }

    /*
     * Also verify that if SIGUSR2 is blocked, queued via tkill, set to SIG_IGN
     * (discarding the pending signal), and then re-bound to handler_b BEFORE
     * unblocking, unblocking still does NOT fire handler_b!
     */
    struct sigaction32 sa_b;
    sa_b.sa_handler  = (uint32_t)(uintptr_t)r6_usr2_handler_b;
    sa_b.sa_flags    = 0x04000004U;
    sa_b.sa_restorer = (uint32_t)(uintptr_t)my_rt_restorer;
    sa_b.sa_mask[0]  = 0U;
    sa_b.sa_mask[1]  = 0U;
    if (sys4(174U /* rt_sigaction */, sigusr2, (uint32_t)(uintptr_t)&sa_b, 0U, 8U) != 0 ||
        sys4(175U /* rt_sigprocmask */, 0U /* SIG_BLOCK */,
             (uint32_t)(uintptr_t)&usr2_bit, 0U, 8U) != 0 ||
        sys2(238U /* tkill */, tid, sigusr2) != 0 ||
        sys4(174U /* rt_sigaction */, sigusr2, (uint32_t)(uintptr_t)&sa_ign, 0U, 8U) != 0 ||
        sys4(174U /* rt_sigaction */, sigusr2, (uint32_t)(uintptr_t)&sa_b, 0U, 8U) != 0 ||
        sys4(175U /* rt_sigprocmask */, 1U /* SIG_UNBLOCK */,
             (uint32_t)(uintptr_t)&usr2_bit, 0U, 8U) != 0 ||
        g_r6_usr2_b_hits != 0U) {
        sys4(175U /* rt_sigprocmask */, 2U /* SIG_SETMASK */,
             (uint32_t)(uintptr_t)&saved_mask, 0U, 8U);
        sys4(174U /* rt_sigaction */, sigusr2, (uint32_t)(uintptr_t)&orig_sa, 0U, 8U);
        return 149;
    }

    /* Restore SIGUSR2 disposition and signal mask */
    sys4(174U /* rt_sigaction */, sigusr2, (uint32_t)(uintptr_t)&orig_sa, 0U, 8U);
    sys4(175U /* rt_sigprocmask */, 2U /* SIG_SETMASK */,
         (uint32_t)(uintptr_t)&saved_mask, 0U, 8U);

    /*
     * 17c: sigaltstack with SS_AUTODISARM (0x80000000U):
     *      - Inside SA_ONSTACK signal handler, sigaltstack(NULL, &oss) must
     *        report SS_DISABLE (2)
     *      - After returning via rt_sigreturn, sigaltstack(NULL, &oss) must
     *        report the re-armed alternate stack with SS_AUTODISARM (0x80000000U)
     */
    uint32_t alt_mem = (uint32_t)sys6(192U /* mmap2 */, 0U, 16384U,
                                      3U /* PROT_READ|PROT_WRITE */,
                                      0x22U /* MAP_PRIVATE|MAP_ANONYMOUS */,
                                      0xFFFFFFFFU, 0U);
    if (alt_mem > 0xFFFFF000U) {
        return 150;
    }

    g_altstack_base = alt_mem;
    g_altstack_size = 16384U;
    g_r6_autodisarm_result = 0U;

    struct stack32_m3 ss;
    ss.ss_sp = alt_mem;
    ss.ss_flags = (int32_t)0x80000000U; /* SS_AUTODISARM */
    ss.ss_size = 16384U;
    struct stack32_m3 oss = { 0U, 0, 0U };
    if (sys2(186U /* sigaltstack */, (uint32_t)(uintptr_t)&ss,
             (uint32_t)(uintptr_t)&oss) != 0) {
        sys2(91U /* munmap */, alt_mem, 16384U);
        return 151;
    }

    /* Verify sigaltstack query before signal reports SS_AUTODISARM */
    if (sys2(186U /* sigaltstack */, 0U, (uint32_t)(uintptr_t)&oss) != 0 ||
        oss.ss_sp != alt_mem || oss.ss_size != 16384U ||
        (uint32_t)oss.ss_flags != 0x80000000U) {
        ss.ss_sp = 0U;
        ss.ss_flags = 2 /* SS_DISABLE */;
        ss.ss_size = 0U;
        sys2(186U /* sigaltstack */, (uint32_t)(uintptr_t)&ss, 0U);
        sys2(91U /* munmap */, alt_mem, 16384U);
        return 152;
    }

    struct sigaction32 act_ad;
    struct sigaction32 old_usr1;
    act_ad.sa_handler  = (uint32_t)(uintptr_t)r6_autodisarm_handler;
    act_ad.sa_flags    = 0x08000000U | 0x04000000U | 0x00000004U; /* SA_ONSTACK | SA_RESTORER | SA_SIGINFO */
    act_ad.sa_restorer = (uint32_t)(uintptr_t)my_rt_restorer;
    act_ad.sa_mask[0]  = 0U;
    act_ad.sa_mask[1]  = 0U;

    if (sys4(174U /* rt_sigaction */, 10U /* SIGUSR1 */,
             (uint32_t)(uintptr_t)&act_ad, (uint32_t)(uintptr_t)&old_usr1, 8U) != 0) {
        ss.ss_sp = 0U;
        ss.ss_flags = 2 /* SS_DISABLE */;
        ss.ss_size = 0U;
        sys2(186U /* sigaltstack */, (uint32_t)(uintptr_t)&ss, 0U);
        sys2(91U /* munmap */, alt_mem, 16384U);
        return 153;
    }

    if (sys2(238U /* tkill */, tid, 10U /* SIGUSR1 */) != 0) {
        sys4(174U /* rt_sigaction */, 10U, (uint32_t)(uintptr_t)&old_usr1, 0U, 8U);
        ss.ss_sp = 0U;
        ss.ss_flags = 2 /* SS_DISABLE */;
        ss.ss_size = 0U;
        sys2(186U /* sigaltstack */, (uint32_t)(uintptr_t)&ss, 0U);
        sys2(91U /* munmap */, alt_mem, 16384U);
        return 154;
    }

    sys4(174U /* rt_sigaction */, 10U, (uint32_t)(uintptr_t)&old_usr1, 0U, 8U);

    if (g_r6_autodisarm_result != 100U) {
        ss.ss_sp = 0U;
        ss.ss_flags = 2 /* SS_DISABLE */;
        ss.ss_size = 0U;
        sys2(186U /* sigaltstack */, (uint32_t)(uintptr_t)&ss, 0U);
        sys2(91U /* munmap */, alt_mem, 16384U);
        return 155;
    }

    /* After rt_sigreturn, alternate stack must be re-armed with SS_AUTODISARM */
    oss.ss_sp = 0U;
    oss.ss_flags = 0;
    oss.ss_size = 0U;
    if (sys2(186U /* sigaltstack */, 0U, (uint32_t)(uintptr_t)&oss) != 0 ||
        oss.ss_sp != alt_mem || oss.ss_size != 16384U ||
        (uint32_t)oss.ss_flags != 0x80000000U) {
        ss.ss_sp = 0U;
        ss.ss_flags = 2 /* SS_DISABLE */;
        ss.ss_size = 0U;
        sys2(186U /* sigaltstack */, (uint32_t)(uintptr_t)&ss, 0U);
        sys2(91U /* munmap */, alt_mem, 16384U);
        return 156;
    }

    /* Clean up alternate stack */
    ss.ss_sp = 0U;
    ss.ss_flags = 2 /* SS_DISABLE */;
    ss.ss_size = 0U;
    sys2(186U /* sigaltstack */, (uint32_t)(uintptr_t)&ss, 0U);
    sys2(91U /* munmap */, alt_mem, 16384U);
    return 0;
}

/* --------------------------------------------------------------------------
 * Test 18: Round 7 regressions —
 *   18a: rt_sigpending (176) and rt_sigsuspend (179) negative (-EINVAL / -EFAULT)
 *        and positive flow, plus blocking real-time signal 34 (SIGRTMIN) via
 *        rt_sigprocmask(SIG_BLOCK, ...) BEFORE calling rt_sigaction(34, ...)
 *   18b: SS_AUTODISARM when a non-SA_ONSTACK signal handler runs on the normal
 *        stack and calls sigaltstack(SS_DISABLE): verify rt_sigreturn does NOT
 *        overwrite that change back to the old SS_AUTODISARM altstack
 * -------------------------------------------------------------------------- */
static volatile uint32_t g_r7_rt34_hits = 0U;
static volatile int32_t  g_r7_rt34_last_sig = 0;
static volatile uint32_t g_r7_non_onstack_result = 0U;

static void r7_sigrt34_handler(int sig, void *info, void *ucontext)
{
    (void)info;
    (void)ucontext;
    g_r7_rt34_hits++;
    g_r7_rt34_last_sig = sig;
}

static void r7_non_onstack_disarm_handler(int sig, void *info, void *ucontext)
{
    (void)sig;
    (void)info;
    (void)ucontext;

    uint32_t cur_sp = 0U;
    __asm__ volatile("mov %0, sp" : "=r"(cur_sp));

    /* Because SA_ONSTACK is NOT set, handler must execute on the normal stack */
    if (cur_sp >= g_altstack_base && cur_sp < g_altstack_base + g_altstack_size) {
        g_r7_non_onstack_result = 1U;
        return;
    }

    /* Before calling sigaltstack(SS_DISABLE), altstack is still SS_AUTODISARM */
    struct stack32_m3 oss = { 0U, 0, 0U };
    if (sys2(186U /* sigaltstack */, 0U, (uint32_t)(uintptr_t)&oss) != 0 ||
        oss.ss_sp != g_altstack_base ||
        (uint32_t)oss.ss_flags != 0x80000000U) {
        g_r7_non_onstack_result = 2U;
        return;
    }

    /* Explicitly disable the alternate stack from inside the non-SA_ONSTACK handler */
    struct stack32_m3 dis_ss;
    dis_ss.ss_sp = 0U;
    dis_ss.ss_flags = 2 /* SS_DISABLE */;
    dis_ss.ss_size = 0U;
    if (sys2(186U /* sigaltstack */, (uint32_t)(uintptr_t)&dis_ss, 0U) != 0) {
        g_r7_non_onstack_result = 3U;
        return;
    }

    g_r7_non_onstack_result = 100U;
}

static int test18_r7_rt_sigpending_sigsuspend_autodisarm(void)
{
    const uint32_t sigrt34 = 34U; /* SIGRTMIN */
    const uint64_t rt34_bit = 1ULL << (sigrt34 - 1U); /* 1ULL << 33 */
    uint32_t tid = (uint32_t)sys1(224U /* gettid */, 0U);
    uint64_t pend_mask = 0ULL;
    uint64_t saved_mask = 0ULL;

    /*
     * 18a-1: Negative checks for rt_sigpending (176) and rt_sigsuspend (179):
     *   - sigsetsize != 8 (0, 4, 16) -> -EINVAL (-22)
     *   - set / mask < 0x1000U (0, 0x100U) -> -EFAULT (-14)
     */
    if (sys2(176U /* rt_sigpending */, (uint32_t)(uintptr_t)&pend_mask, 0U) != -22 ||
        sys2(176U /* rt_sigpending */, (uint32_t)(uintptr_t)&pend_mask, 4U) != -22 ||
        sys2(176U /* rt_sigpending */, (uint32_t)(uintptr_t)&pend_mask, 16U) != -22) {
        return 157;
    }
    if (sys2(176U /* rt_sigpending */, 0U, 8U) != -14 ||
        sys2(176U /* rt_sigpending */, 0x100U, 8U) != -14) {
        return 158;
    }
    if (sys2(179U /* rt_sigsuspend */, (uint32_t)(uintptr_t)&pend_mask, 0U) != -22 ||
        sys2(179U /* rt_sigsuspend */, (uint32_t)(uintptr_t)&pend_mask, 4U) != -22 ||
        sys2(179U /* rt_sigsuspend */, (uint32_t)(uintptr_t)&pend_mask, 16U) != -22) {
        return 159;
    }
    if (sys2(179U /* rt_sigsuspend */, 0U, 8U) != -14 ||
        sys2(179U /* rt_sigsuspend */, 0x100U, 8U) != -14) {
        return 160;
    }

    /*
     * 18a-2: Block signal 34 (SIGRTMIN) FIRST via rt_sigprocmask(SIG_BLOCK, ...)
     *        BEFORE calling rt_sigaction(34, ...), send signal 34 via tkill,
     *        verify rt_sigpending reports bit 33 (1ULL << 33), then install the
     *        handler for signal 34 and call rt_sigsuspend with an empty mask to
     *        deliver signal 34 and verify -EINTR (-4).
     */
    g_r7_rt34_hits = 0U;
    g_r7_rt34_last_sig = 0;

    if (sys4(175U /* rt_sigprocmask */, 0U /* SIG_BLOCK */,
             (uint32_t)(uintptr_t)&rt34_bit,
             (uint32_t)(uintptr_t)&saved_mask, 8U) != 0) {
        return 161;
    }

    /* Send signal 34 to self while blocked and BEFORE rt_sigaction(34, ...) */
    if (sys2(238U /* tkill */, tid, sigrt34) != 0) {
        sys4(175U /* rt_sigprocmask */, 2U /* SIG_SETMASK */,
             (uint32_t)(uintptr_t)&saved_mask, 0U, 8U);
        return 162;
    }

    /* Verify rt_sigpending reports bit 33 (1ULL << 33) set */
    pend_mask = 0ULL;
    if (sys2(176U /* rt_sigpending */, (uint32_t)(uintptr_t)&pend_mask, 8U) != 0 ||
        (pend_mask & rt34_bit) == 0ULL) {
        sys4(175U /* rt_sigprocmask */, 2U /* SIG_SETMASK */,
             (uint32_t)(uintptr_t)&saved_mask, 0U, 8U);
        return 163;
    }

    /* Now install a handler for signal 34 via rt_sigaction while still blocked */
    struct sigaction32 act34;
    struct sigaction32 old_act34;
    act34.sa_handler  = (uint32_t)(uintptr_t)r7_sigrt34_handler;
    act34.sa_flags    = 0x04000004U; /* SA_SIGINFO | SA_RESTORER */
    act34.sa_restorer = (uint32_t)(uintptr_t)my_rt_restorer;
    act34.sa_mask[0]  = 0U;
    act34.sa_mask[1]  = 0U;

    if (sys4(174U /* rt_sigaction */, sigrt34,
             (uint32_t)(uintptr_t)&act34,
             (uint32_t)(uintptr_t)&old_act34, 8U) != 0 ||
        g_r7_rt34_hits != 0U) {
        sys4(175U /* rt_sigprocmask */, 2U /* SIG_SETMASK */,
             (uint32_t)(uintptr_t)&saved_mask, 0U, 8U);
        return 164;
    }

    /* Call rt_sigsuspend with an empty mask to deliver pending signal 34 */
    uint64_t empty_mask = 0ULL;
    int32_t susp_rc = sys2(179U /* rt_sigsuspend */,
                           (uint32_t)(uintptr_t)&empty_mask, 8U);
    if (susp_rc != -4 /* -EINTR */ ||
        g_r7_rt34_hits != 1U ||
        g_r7_rt34_last_sig != 34) {
        sys4(174U /* rt_sigaction */, sigrt34, (uint32_t)(uintptr_t)&old_act34, 0U, 8U);
        sys4(175U /* rt_sigprocmask */, 2U /* SIG_SETMASK */,
             (uint32_t)(uintptr_t)&saved_mask, 0U, 8U);
        return 165;
    }

    /* After rt_sigsuspend returns, signal 34 must no longer be pending and must still be blocked */
    pend_mask = 0xFFFFFFFFFFFFFFFFULL;
    uint64_t post_mask = 0ULL;
    if (sys2(176U /* rt_sigpending */, (uint32_t)(uintptr_t)&pend_mask, 8U) != 0 ||
        (pend_mask & rt34_bit) != 0ULL ||
        sys4(175U /* rt_sigprocmask */, 0U /* SIG_BLOCK */, 0U,
             (uint32_t)(uintptr_t)&post_mask, 8U) != 0 ||
        (post_mask & rt34_bit) == 0ULL) {
        sys4(174U /* rt_sigaction */, sigrt34, (uint32_t)(uintptr_t)&old_act34, 0U, 8U);
        sys4(175U /* rt_sigprocmask */, 2U /* SIG_SETMASK */,
             (uint32_t)(uintptr_t)&saved_mask, 0U, 8U);
        return 166;
    }

    sys4(174U /* rt_sigaction */, sigrt34, (uint32_t)(uintptr_t)&old_act34, 0U, 8U);
    sys4(175U /* rt_sigprocmask */, 2U /* SIG_SETMASK */,
         (uint32_t)(uintptr_t)&saved_mask, 0U, 8U);

    /*
     * 18b: Regression test for SS_AUTODISARM when a non-SA_ONSTACK signal
     *      handler runs on the normal stack and calls sigaltstack(SS_DISABLE):
     *      verify rt_sigreturn does NOT overwrite that change back to the old
     *      SS_AUTODISARM altstack!
     */
    uint32_t alt_mem = (uint32_t)sys6(192U /* mmap2 */, 0U, 16384U,
                                      3U /* PROT_READ|PROT_WRITE */,
                                      0x22U /* MAP_PRIVATE|MAP_ANONYMOUS */,
                                      0xFFFFFFFFU, 0U);
    if (alt_mem > 0xFFFFF000U) {
        return 167;
    }

    g_altstack_base = alt_mem;
    g_altstack_size = 16384U;
    g_r7_non_onstack_result = 0U;

    struct stack32_m3 ss;
    ss.ss_sp = alt_mem;
    ss.ss_flags = (int32_t)0x80000000U; /* SS_AUTODISARM */
    ss.ss_size = 16384U;
    struct stack32_m3 oss = { 0U, 0, 0U };
    if (sys2(186U /* sigaltstack */, (uint32_t)(uintptr_t)&ss,
             (uint32_t)(uintptr_t)&oss) != 0) {
        sys2(91U /* munmap */, alt_mem, 16384U);
        return 168;
    }

    struct sigaction32 act_no_onstack;
    struct sigaction32 old_usr1;
    act_no_onstack.sa_handler  = (uint32_t)(uintptr_t)r7_non_onstack_disarm_handler;
    act_no_onstack.sa_flags    = 0x04000004U; /* SA_RESTORER | SA_SIGINFO (NO SA_ONSTACK) */
    act_no_onstack.sa_restorer = (uint32_t)(uintptr_t)my_rt_restorer;
    act_no_onstack.sa_mask[0]  = 0U;
    act_no_onstack.sa_mask[1]  = 0U;

    if (sys4(174U /* rt_sigaction */, 10U /* SIGUSR1 */,
             (uint32_t)(uintptr_t)&act_no_onstack,
             (uint32_t)(uintptr_t)&old_usr1, 8U) != 0) {
        ss.ss_sp = 0U;
        ss.ss_flags = 2 /* SS_DISABLE */;
        ss.ss_size = 0U;
        sys2(186U /* sigaltstack */, (uint32_t)(uintptr_t)&ss, 0U);
        sys2(91U /* munmap */, alt_mem, 16384U);
        return 169;
    }

    if (sys2(238U /* tkill */, tid, 10U /* SIGUSR1 */) != 0) {
        sys4(174U /* rt_sigaction */, 10U, (uint32_t)(uintptr_t)&old_usr1, 0U, 8U);
        ss.ss_sp = 0U;
        ss.ss_flags = 2 /* SS_DISABLE */;
        ss.ss_size = 0U;
        sys2(186U /* sigaltstack */, (uint32_t)(uintptr_t)&ss, 0U);
        sys2(91U /* munmap */, alt_mem, 16384U);
        return 170;
    }

    sys4(174U /* rt_sigaction */, 10U, (uint32_t)(uintptr_t)&old_usr1, 0U, 8U);

    if (g_r7_non_onstack_result != 100U) {
        ss.ss_sp = 0U;
        ss.ss_flags = 2 /* SS_DISABLE */;
        ss.ss_size = 0U;
        sys2(186U /* sigaltstack */, (uint32_t)(uintptr_t)&ss, 0U);
        sys2(91U /* munmap */, alt_mem, 16384U);
        return 171;
    }

    /*
     * Verify that after rt_sigreturn from the non-SA_ONSTACK handler, the
     * handler's sigaltstack(SS_DISABLE) change remains in effect (oss.ss_flags == 2)
     * and was NOT overwritten back to the old SS_AUTODISARM altstack!
     */
    oss.ss_sp = 0U;
    oss.ss_flags = 0;
    oss.ss_size = 0U;
    if (sys2(186U /* sigaltstack */, 0U, (uint32_t)(uintptr_t)&oss) != 0 ||
        oss.ss_flags != 2 /* SS_DISABLE */) {
        ss.ss_sp = 0U;
        ss.ss_flags = 2 /* SS_DISABLE */;
        ss.ss_size = 0U;
        sys2(186U /* sigaltstack */, (uint32_t)(uintptr_t)&ss, 0U);
        sys2(91U /* munmap */, alt_mem, 16384U);
        return 172;
    }

    sys2(91U /* munmap */, alt_mem, 16384U);
    return 0;
}

/* --------------------------------------------------------------------------
 * Test 19: Round 8 regressions —
 *   19a: SIG_IGN (SIGUSR1 = 10) and default-ignored SIGWINCH (28) while
 *        blocked in rt_sigprocmask must NOT become pending in rt_sigpending
 *        and must NOT fire when later given a handler and unblocked
 *   19b: rt_sigsuspend (179) active signal mask inside the signal handler
 *        (temp_mask + sa_mask + sig bit) and restoration of saved_mask on return
 * -------------------------------------------------------------------------- */
volatile uint32_t g_r8_usr1_hits = 0U;
static volatile uint32_t g_r8_winch_hits = 0U;
static volatile int32_t  g_r8_handler_mask_rc = -1;
static volatile uint64_t g_r8_handler_mask = 0ULL;

void r8_usr1_handler(int sig, void *info, void *ucontext)
{
    (void)sig;
    (void)info;
    (void)ucontext;
    g_r8_usr1_hits++;
}

static void r8_winch_handler(int sig, void *info, void *ucontext)
{
    (void)sig;
    (void)info;
    (void)ucontext;
    g_r8_winch_hits++;
}

static void r8_sigsuspend_usr1_handler(int sig, void *info, void *ucontext)
{
    (void)sig;
    (void)info;
    (void)ucontext;
    static uint64_t cur_mask;
    cur_mask = 0ULL;
    g_r8_handler_mask_rc = sys4(175U /* rt_sigprocmask */, 0U /* SIG_BLOCK */,
                                0U, (uint32_t)(uintptr_t)&cur_mask, 8U);
    g_r8_handler_mask = cur_mask;
    g_r8_usr1_hits++;
}

static int test19_r8_sigign_sigwinch_and_sigsuspend_mask(void)
{
    const uint32_t sigusr1  = 10U;
    const uint32_t sigusr2  = 12U;
    const uint32_t sigwinch = 28U;
    const uint64_t usr1_bit  = 1ULL << (sigusr1 - 1U);  /* 1ULL << 9  */
    const uint64_t usr2_bit  = 1ULL << (sigusr2 - 1U);  /* 1ULL << 11 */
    const uint64_t winch_bit = 1ULL << (sigwinch - 1U); /* 1ULL << 27 */

    uint32_t tid = (uint32_t)sys1(224U /* gettid */, 0U);
    static uint64_t orig_mask;
    static uint64_t block_mask;
    static uint64_t pend_mask;
    static struct sigaction32 orig_usr1;
    static struct sigaction32 orig_winch;
    static struct sigaction32 sa;

    orig_mask = 0ULL;
    if (sys4(175U /* rt_sigprocmask */, 0U /* SIG_BLOCK */, 0U,
             (uint32_t)(uintptr_t)&orig_mask, 8U) != 0) {
        return 173;
    }
    if (sys4(174U /* rt_sigaction */, sigusr1, 0U,
             (uint32_t)(uintptr_t)&orig_usr1, 8U) != 0 ||
        sys4(174U /* rt_sigaction */, sigwinch, 0U,
             (uint32_t)(uintptr_t)&orig_winch, 8U) != 0) {
        return 174;
    }

    /*
     * 19a: Set SIGUSR1 (10) to SIG_IGN (1) and SIGWINCH (28) to SIG_DFL (0,
     *      whose default action is ignore). Block both via rt_sigprocmask,
     *      send both via tkill, and verify rt_sigpending reports NEITHER as
     *      pending. Then install handlers for both and unblock both, verifying
     *      neither handler fires.
     */
    sa.sa_handler  = 1U; /* SIG_IGN */
    sa.sa_flags    = 0U;
    sa.sa_restorer = 0U;
    sa.sa_mask[0]  = 0U;
    sa.sa_mask[1]  = 0U;
    if (sys4(174U /* rt_sigaction */, sigusr1, (uint32_t)(uintptr_t)&sa, 0U, 8U) != 0) {
        return 175;
    }

    sa.sa_handler = 0U; /* SIG_DFL (default-ignored for SIGWINCH) */
    if (sys4(174U /* rt_sigaction */, sigwinch, (uint32_t)(uintptr_t)&sa, 0U, 8U) != 0) {
        sys4(174U /* rt_sigaction */, sigusr1, (uint32_t)(uintptr_t)&orig_usr1, 0U, 8U);
        return 176;
    }

    block_mask = usr1_bit | winch_bit;
    if (sys4(175U /* rt_sigprocmask */, 0U /* SIG_BLOCK */,
             (uint32_t)(uintptr_t)&block_mask, 0U, 8U) != 0) {
        sys4(174U /* rt_sigaction */, sigusr1, (uint32_t)(uintptr_t)&orig_usr1, 0U, 8U);
        sys4(174U /* rt_sigaction */, sigwinch, (uint32_t)(uintptr_t)&orig_winch, 0U, 8U);
        return 177;
    }

    /* Send both SIGUSR1 (SIG_IGN) and SIGWINCH (SIG_DFL == ignore) while blocked */
    if (sys2(238U /* tkill */, tid, sigusr1) != 0 ||
        sys2(238U /* tkill */, tid, sigwinch) != 0) {
        sys4(175U /* rt_sigprocmask */, 2U /* SIG_SETMASK */,
             (uint32_t)(uintptr_t)&orig_mask, 0U, 8U);
        sys4(174U /* rt_sigaction */, sigusr1, (uint32_t)(uintptr_t)&orig_usr1, 0U, 8U);
        sys4(174U /* rt_sigaction */, sigwinch, (uint32_t)(uintptr_t)&orig_winch, 0U, 8U);
        return 178;
    }

    /* Verify rt_sigpending reports NEITHER SIGUSR1 nor SIGWINCH as pending */
    pend_mask = 0xFFFFFFFFFFFFFFFFULL;
    if (sys2(176U /* rt_sigpending */, (uint32_t)(uintptr_t)&pend_mask, 8U) != 0 ||
        (pend_mask & (usr1_bit | winch_bit)) != 0ULL) {
        sys4(175U /* rt_sigprocmask */, 2U /* SIG_SETMASK */,
             (uint32_t)(uintptr_t)&orig_mask, 0U, 8U);
        sys4(174U /* rt_sigaction */, sigusr1, (uint32_t)(uintptr_t)&orig_usr1, 0U, 8U);
        sys4(174U /* rt_sigaction */, sigwinch, (uint32_t)(uintptr_t)&orig_winch, 0U, 8U);
        return 179;
    }

    /* Install handlers for SIGUSR1 and SIGWINCH, then unblock both: must NOT fire */
    g_r8_usr1_hits = 0U;
    g_r8_winch_hits = 0U;
    sa.sa_handler  = (uint32_t)(uintptr_t)r8_usr1_handler;
    sa.sa_flags    = 0x04000004U; /* SA_SIGINFO | SA_RESTORER */
    sa.sa_restorer = (uint32_t)(uintptr_t)my_rt_restorer;
    sa.sa_mask[0]  = 0U;
    sa.sa_mask[1]  = 0U;
    if (sys4(174U /* rt_sigaction */, sigusr1, (uint32_t)(uintptr_t)&sa, 0U, 8U) != 0) {
        sys4(175U /* rt_sigprocmask */, 2U /* SIG_SETMASK */,
             (uint32_t)(uintptr_t)&orig_mask, 0U, 8U);
        sys4(174U /* rt_sigaction */, sigusr1, (uint32_t)(uintptr_t)&orig_usr1, 0U, 8U);
        sys4(174U /* rt_sigaction */, sigwinch, (uint32_t)(uintptr_t)&orig_winch, 0U, 8U);
        return 180;
    }
    sa.sa_handler = (uint32_t)(uintptr_t)r8_winch_handler;
    if (sys4(174U /* rt_sigaction */, sigwinch, (uint32_t)(uintptr_t)&sa, 0U, 8U) != 0) {
        sys4(175U /* rt_sigprocmask */, 2U /* SIG_SETMASK */,
             (uint32_t)(uintptr_t)&orig_mask, 0U, 8U);
        sys4(174U /* rt_sigaction */, sigusr1, (uint32_t)(uintptr_t)&orig_usr1, 0U, 8U);
        sys4(174U /* rt_sigaction */, sigwinch, (uint32_t)(uintptr_t)&orig_winch, 0U, 8U);
        return 181;
    }

    if (sys4(175U /* rt_sigprocmask */, 1U /* SIG_UNBLOCK */,
             (uint32_t)(uintptr_t)&block_mask, 0U, 8U) != 0 ||
        g_r8_usr1_hits != 0U ||
        g_r8_winch_hits != 0U) {
        sys4(175U /* rt_sigprocmask */, 2U /* SIG_SETMASK */,
             (uint32_t)(uintptr_t)&orig_mask, 0U, 8U);
        sys4(174U /* rt_sigaction */, sigusr1, (uint32_t)(uintptr_t)&orig_usr1, 0U, 8U);
        sys4(174U /* rt_sigaction */, sigwinch, (uint32_t)(uintptr_t)&orig_winch, 0U, 8U);
        return 182;
    }

    /*
     * Positive check: now that SIGWINCH has a custom handler installed, blocking
     * SIGWINCH and sending it via tkill MUST mark it pending in rt_sigpending
     * and deliver it upon SIG_UNBLOCK.
     */
    block_mask = winch_bit;
    pend_mask = 0ULL;
    if (sys4(175U /* rt_sigprocmask */, 0U /* SIG_BLOCK */,
             (uint32_t)(uintptr_t)&block_mask, 0U, 8U) != 0 ||
        sys2(238U /* tkill */, tid, sigwinch) != 0 ||
        sys2(176U /* rt_sigpending */, (uint32_t)(uintptr_t)&pend_mask, 8U) != 0 ||
        (pend_mask & winch_bit) == 0ULL ||
        sys4(175U /* rt_sigprocmask */, 1U /* SIG_UNBLOCK */,
             (uint32_t)(uintptr_t)&block_mask, 0U, 8U) != 0 ||
        g_r8_winch_hits != 1U) {
        sys4(175U /* rt_sigprocmask */, 2U /* SIG_SETMASK */,
             (uint32_t)(uintptr_t)&orig_mask, 0U, 8U);
        sys4(174U /* rt_sigaction */, sigusr1, (uint32_t)(uintptr_t)&orig_usr1, 0U, 8U);
        sys4(174U /* rt_sigaction */, sigwinch, (uint32_t)(uintptr_t)&orig_winch, 0U, 8U);
        return 183;
    }

    /*
     * 19b: Regression test for rt_sigsuspend (179) active signal mask inside
     *      the handler:
     *      - Block SIGUSR1 (10) and unblock SIGUSR2 (12) in normal sig_mask
     *      - Queue SIGUSR1 via tkill while blocked
     *      - Call rt_sigsuspend with temp_mask that blocks SIGUSR2 (12) and
     *        unblocks SIGUSR1 (10)
     *      - Verify inside the SIGUSR1 handler that SIGUSR2 (12) IS blocked
     *        ((g_r8_handler_mask & usr2_bit) != 0)
     *      - Verify after rt_sigsuspend returns -EINTR (-4) that the original
     *        mask (SIGUSR1 blocked, SIGUSR2 unblocked) is restored!
     */
    sa.sa_handler  = (uint32_t)(uintptr_t)r8_sigsuspend_usr1_handler;
    sa.sa_flags    = 0x04000004U; /* SA_SIGINFO | SA_RESTORER */
    sa.sa_restorer = (uint32_t)(uintptr_t)my_rt_restorer;
    sa.sa_mask[0]  = 0U;
    sa.sa_mask[1]  = 0U;
    if (sys4(174U /* rt_sigaction */, sigusr1, (uint32_t)(uintptr_t)&sa, 0U, 8U) != 0) {
        sys4(175U /* rt_sigprocmask */, 2U /* SIG_SETMASK */,
             (uint32_t)(uintptr_t)&orig_mask, 0U, 8U);
        sys4(174U /* rt_sigaction */, sigusr1, (uint32_t)(uintptr_t)&orig_usr1, 0U, 8U);
        sys4(174U /* rt_sigaction */, sigwinch, (uint32_t)(uintptr_t)&orig_winch, 0U, 8U);
        return 184;
    }

    static uint64_t normal_mask;
    static uint64_t temp_mask;
    static uint64_t post_mask;
    normal_mask = (orig_mask | usr1_bit) & ~usr2_bit;
    if (sys4(175U /* rt_sigprocmask */, 2U /* SIG_SETMASK */,
             (uint32_t)(uintptr_t)&normal_mask, 0U, 8U) != 0) {
        sys4(175U /* rt_sigprocmask */, 2U /* SIG_SETMASK */,
             (uint32_t)(uintptr_t)&orig_mask, 0U, 8U);
        sys4(174U /* rt_sigaction */, sigusr1, (uint32_t)(uintptr_t)&orig_usr1, 0U, 8U);
        sys4(174U /* rt_sigaction */, sigwinch, (uint32_t)(uintptr_t)&orig_winch, 0U, 8U);
        return 185;
    }

    g_r8_usr1_hits = 0U;
    g_r8_handler_mask_rc = -1;
    g_r8_handler_mask = 0ULL;

    /* Send SIGUSR1 while blocked in normal_mask */
    if (sys2(238U /* tkill */, tid, sigusr1) != 0 || g_r8_usr1_hits != 0U) {
        sys4(175U /* rt_sigprocmask */, 2U /* SIG_SETMASK */,
             (uint32_t)(uintptr_t)&orig_mask, 0U, 8U);
        sys4(174U /* rt_sigaction */, sigusr1, (uint32_t)(uintptr_t)&orig_usr1, 0U, 8U);
        sys4(174U /* rt_sigaction */, sigwinch, (uint32_t)(uintptr_t)&orig_winch, 0U, 8U);
        return 186;
    }

    /* temp_mask blocks SIGUSR2 (12) and unblocks SIGUSR1 (10) */
    temp_mask = (orig_mask & ~usr1_bit) | usr2_bit;
    int32_t susp_rc = sys2(179U /* rt_sigsuspend */,
                           (uint32_t)(uintptr_t)&temp_mask, 8U);
    post_mask = 0ULL;
    int32_t pm_rc = sys4(175U /* rt_sigprocmask */, 0U /* SIG_BLOCK */, 0U,
                         (uint32_t)(uintptr_t)&post_mask, 8U);

    /* Restore original dispositions and signal mask */
    sys4(174U /* rt_sigaction */, sigusr1, (uint32_t)(uintptr_t)&orig_usr1, 0U, 8U);
    sys4(174U /* rt_sigaction */, sigwinch, (uint32_t)(uintptr_t)&orig_winch, 0U, 8U);
    sys4(175U /* rt_sigprocmask */, 2U /* SIG_SETMASK */,
         (uint32_t)(uintptr_t)&orig_mask, 0U, 8U);

    if (susp_rc != -4 /* -EINTR */ ||
        g_r8_usr1_hits != 1U ||
        g_r8_handler_mask_rc != 0 ||
        (g_r8_handler_mask & usr2_bit) == 0ULL ||
        (g_r8_handler_mask & usr1_bit) == 0ULL) {
        return 187;
    }
    if (pm_rc != 0 ||
        (post_mask & usr1_bit) == 0ULL ||
        (post_mask & usr2_bit) != 0ULL) {
        return 188;
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * Test 20: Round 9 regressions —
 *   20a: rt_sigtimedwait (177) and rt_sigtimedwait_time64 (421) negative
 *        (-EINVAL on sigsetsize != 8 or invalid timespec, -EFAULT on invalid
 *        pointers, -EAGAIN on zero-timeout poll with no signal pending) and
 *        positive (consume pending blocked SIGUSR1, populate siginfo32, and
 *        clear pending bit in rt_sigpending)
 *   20b: clone (120) negative flag & pointer validation:
 *        - CLONE_SIGHAND without CLONE_VM -> -EINVAL (-22)
 *        - CLONE_THREAD without CLONE_SIGHAND -> -EINVAL (-22)
 *        - CLONE_VM | CLONE_SIGHAND | CLONE_PARENT_SETTID with ptid = 0x100U
 *          -> -EFAULT (-14)
 * -------------------------------------------------------------------------- */

int run_test_m3_part3(void)
{
    int rc = test17_r6_signal_aliasing_ign_autodisarm();
    if (rc != 0) return rc;
    rc = test18_r7_rt_sigpending_sigsuspend_autodisarm();
    if (rc != 0) return rc;
    return test19_r8_sigign_sigwinch_and_sigsuspend_mask();
}
