/*
 * tests/test_m3_part4.c — M3 threads & signals tests (part 4: tests 20..23)
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_m3_common.h"

static int test20_r9_sigtimedwait_and_clone_validation(void)
{
    const uint32_t sigusr1  = 10U;
    const uint64_t usr1_bit = 1ULL << (sigusr1 - 1U);
    uint32_t tid = (uint32_t)sys1(224U /* gettid */, 0U);

    static uint64_t orig_mask;
    static uint64_t wait_set;
    static uint64_t pend_mask;
    static struct sigaction32 orig_usr1;
    static struct sigaction32 sa_usr1;
    static struct siginfo32 si;
    static struct timespec32_m3 ts32;
    static struct timespec64_m3 ts64;
    static _Alignas(16) uint64_t dummy_clone_stack[64];

    wait_set = usr1_bit;
    ts32.tv_sec  = 0;
    ts32.tv_nsec = 0;
    ts64.tv_sec  = 0;
    ts64.tv_nsec = 0;

    /*
     * 20a-1: Negative checks for rt_sigtimedwait (177) and rt_sigtimedwait_time64 (421):
     *   - sigsetsize != 8 (0, 4) -> -EINVAL (-22)
     *   - invalid pointers (0, 0x100) -> -EFAULT (-14)
     *   - invalid timespec (tv_nsec = -1, tv_nsec = 1000000000, tv_sec = -1) -> -EINVAL (-22)
     */
    if (sys4(177U /* rt_sigtimedwait */, (uint32_t)(uintptr_t)&wait_set,
             0U, (uint32_t)(uintptr_t)&ts32, 0U) != -22 /* -EINVAL */ ||
        sys4(177U /* rt_sigtimedwait */, (uint32_t)(uintptr_t)&wait_set,
             0U, (uint32_t)(uintptr_t)&ts32, 4U) != -22 /* -EINVAL */ ||
        sys4(421U /* rt_sigtimedwait_time64 */, (uint32_t)(uintptr_t)&wait_set,
             0U, (uint32_t)(uintptr_t)&ts64, 0U) != -22 /* -EINVAL */ ||
        sys4(421U /* rt_sigtimedwait_time64 */, (uint32_t)(uintptr_t)&wait_set,
             0U, (uint32_t)(uintptr_t)&ts64, 4U) != -22 /* -EINVAL */) {
        return 189;
    }

    if (sys4(177U /* rt_sigtimedwait */, 0U,
             0U, (uint32_t)(uintptr_t)&ts32, 8U) != -14 /* -EFAULT */ ||
        sys4(177U /* rt_sigtimedwait */, 0x100U,
             0U, (uint32_t)(uintptr_t)&ts32, 8U) != -14 /* -EFAULT */ ||
        sys4(177U /* rt_sigtimedwait */, (uint32_t)(uintptr_t)&wait_set,
             0x100U, (uint32_t)(uintptr_t)&ts32, 8U) != -14 /* -EFAULT */ ||
        sys4(177U /* rt_sigtimedwait */, (uint32_t)(uintptr_t)&wait_set,
             0U, 0x100U, 8U) != -14 /* -EFAULT */ ||
        sys4(421U /* rt_sigtimedwait_time64 */, 0U,
             0U, (uint32_t)(uintptr_t)&ts64, 8U) != -14 /* -EFAULT */ ||
        sys4(421U /* rt_sigtimedwait_time64 */, 0x100U,
             0U, (uint32_t)(uintptr_t)&ts64, 8U) != -14 /* -EFAULT */ ||
        sys4(421U /* rt_sigtimedwait_time64 */, (uint32_t)(uintptr_t)&wait_set,
             0x100U, (uint32_t)(uintptr_t)&ts64, 8U) != -14 /* -EFAULT */ ||
        sys4(421U /* rt_sigtimedwait_time64 */, (uint32_t)(uintptr_t)&wait_set,
             0U, 0x100U, 8U) != -14 /* -EFAULT */) {
        return 190;
    }

    /* Invalid timespec: tv_nsec = -1, tv_nsec = 1000000000, tv_sec = -1 -> -EINVAL (-22) */
    ts32.tv_sec  = 0;
    ts32.tv_nsec = -1;
    ts64.tv_sec  = 0;
    ts64.tv_nsec = -1;
    if (sys4(177U /* rt_sigtimedwait */, (uint32_t)(uintptr_t)&wait_set,
             0U, (uint32_t)(uintptr_t)&ts32, 8U) != -22 /* -EINVAL */ ||
        sys4(421U /* rt_sigtimedwait_time64 */, (uint32_t)(uintptr_t)&wait_set,
             0U, (uint32_t)(uintptr_t)&ts64, 8U) != -22 /* -EINVAL */) {
        return 191;
    }

    ts32.tv_sec  = 0;
    ts32.tv_nsec = 1000000000;
    ts64.tv_sec  = 0;
    ts64.tv_nsec = 1000000000LL;
    if (sys4(177U /* rt_sigtimedwait */, (uint32_t)(uintptr_t)&wait_set,
             0U, (uint32_t)(uintptr_t)&ts32, 8U) != -22 /* -EINVAL */ ||
        sys4(421U /* rt_sigtimedwait_time64 */, (uint32_t)(uintptr_t)&wait_set,
             0U, (uint32_t)(uintptr_t)&ts64, 8U) != -22 /* -EINVAL */) {
        return 192;
    }

    ts32.tv_sec  = -1;
    ts32.tv_nsec = 0;
    ts64.tv_sec  = -1;
    ts64.tv_nsec = 0;
    if (sys4(177U /* rt_sigtimedwait */, (uint32_t)(uintptr_t)&wait_set,
             0U, (uint32_t)(uintptr_t)&ts32, 8U) != -22 /* -EINVAL */ ||
        sys4(421U /* rt_sigtimedwait_time64 */, (uint32_t)(uintptr_t)&wait_set,
             0U, (uint32_t)(uintptr_t)&ts64, 8U) != -22 /* -EINVAL */) {
        return 193;
    }

    /*
     * 20a-2: Zero-timeout poll when no signal is pending -> -EAGAIN (-11),
     *        followed by positive consumption of blocked pending SIGUSR1 (10)
     *        via both rt_sigtimedwait (177) and rt_sigtimedwait_time64 (421).
     */
    orig_mask = 0ULL;
    if (sys4(175U /* rt_sigprocmask */, 0U /* SIG_BLOCK */,
             (uint32_t)(uintptr_t)&wait_set,
             (uint32_t)(uintptr_t)&orig_mask, 8U) != 0) {
        return 194;
    }

    g_r8_usr1_hits = 0U;
    sa_usr1.sa_handler  = (uint32_t)(uintptr_t)r8_usr1_handler;
    sa_usr1.sa_flags    = 0x04000004U; /* SA_SIGINFO | SA_RESTORER */
    sa_usr1.sa_restorer = (uint32_t)(uintptr_t)my_rt_restorer;
    sa_usr1.sa_mask[0]  = 0U;
    sa_usr1.sa_mask[1]  = 0U;
    if (sys4(174U /* rt_sigaction */, sigusr1,
             (uint32_t)(uintptr_t)&sa_usr1,
             (uint32_t)(uintptr_t)&orig_usr1, 8U) != 0) {
        sys4(175U /* rt_sigprocmask */, 2U /* SIG_SETMASK */,
             (uint32_t)(uintptr_t)&orig_mask, 0U, 8U);
        return 195;
    }

    ts32.tv_sec  = 0;
    ts32.tv_nsec = 0;
    ts64.tv_sec  = 0;
    ts64.tv_nsec = 0;
    if (sys4(177U /* rt_sigtimedwait */, (uint32_t)(uintptr_t)&wait_set,
             (uint32_t)(uintptr_t)&si, (uint32_t)(uintptr_t)&ts32, 8U) != -11 /* -EAGAIN */ ||
        sys4(421U /* rt_sigtimedwait_time64 */, (uint32_t)(uintptr_t)&wait_set,
             (uint32_t)(uintptr_t)&si, (uint32_t)(uintptr_t)&ts64, 8U) != -11 /* -EAGAIN */) {
        sys4(174U /* rt_sigaction */, sigusr1, (uint32_t)(uintptr_t)&orig_usr1, 0U, 8U);
        sys4(175U /* rt_sigprocmask */, 2U /* SIG_SETMASK */,
             (uint32_t)(uintptr_t)&orig_mask, 0U, 8U);
        return 196;
    }

    /* Positive: rt_sigtimedwait (177) */
    si.si_signo = 0;
    si.si_errno = -1;
    si.si_code  = 0;
    pend_mask   = 0ULL;
    if (sys2(238U /* tkill */, tid, sigusr1) != 0 ||
        sys2(176U /* rt_sigpending */, (uint32_t)(uintptr_t)&pend_mask, 8U) != 0 ||
        (pend_mask & usr1_bit) == 0ULL) {
        sys4(174U /* rt_sigaction */, sigusr1, (uint32_t)(uintptr_t)&orig_usr1, 0U, 8U);
        sys4(175U /* rt_sigprocmask */, 2U /* SIG_SETMASK */,
             (uint32_t)(uintptr_t)&orig_mask, 0U, 8U);
        return 197;
    }

    int32_t stw_rc = sys4(177U /* rt_sigtimedwait */,
                          (uint32_t)(uintptr_t)&wait_set,
                          (uint32_t)(uintptr_t)&si,
                          (uint32_t)(uintptr_t)&ts32, 8U);
    pend_mask = 0xFFFFFFFFFFFFFFFFULL;
    if (stw_rc != (int32_t)sigusr1 ||
        si.si_signo != (int32_t)sigusr1 ||
        g_r8_usr1_hits != 0U ||
        sys2(176U /* rt_sigpending */, (uint32_t)(uintptr_t)&pend_mask, 8U) != 0 ||
        (pend_mask & usr1_bit) != 0ULL) {
        sys4(174U /* rt_sigaction */, sigusr1, (uint32_t)(uintptr_t)&orig_usr1, 0U, 8U);
        sys4(175U /* rt_sigprocmask */, 2U /* SIG_SETMASK */,
             (uint32_t)(uintptr_t)&orig_mask, 0U, 8U);
        return 198;
    }

    /* Positive: rt_sigtimedwait_time64 (421) */
    si.si_signo = 0;
    si.si_errno = -1;
    si.si_code  = 0;
    pend_mask   = 0ULL;
    if (sys2(238U /* tkill */, tid, sigusr1) != 0 ||
        sys2(176U /* rt_sigpending */, (uint32_t)(uintptr_t)&pend_mask, 8U) != 0 ||
        (pend_mask & usr1_bit) == 0ULL) {
        sys4(174U /* rt_sigaction */, sigusr1, (uint32_t)(uintptr_t)&orig_usr1, 0U, 8U);
        sys4(175U /* rt_sigprocmask */, 2U /* SIG_SETMASK */,
             (uint32_t)(uintptr_t)&orig_mask, 0U, 8U);
        return 199;
    }

    int32_t stw64_rc = sys4(421U /* rt_sigtimedwait_time64 */,
                            (uint32_t)(uintptr_t)&wait_set,
                            (uint32_t)(uintptr_t)&si,
                            (uint32_t)(uintptr_t)&ts64, 8U);
    pend_mask = 0xFFFFFFFFFFFFFFFFULL;
    int32_t pend_rc = sys2(176U /* rt_sigpending */, (uint32_t)(uintptr_t)&pend_mask, 8U);

    sys4(174U /* rt_sigaction */, sigusr1, (uint32_t)(uintptr_t)&orig_usr1, 0U, 8U);
    sys4(175U /* rt_sigprocmask */, 2U /* SIG_SETMASK */,
         (uint32_t)(uintptr_t)&orig_mask, 0U, 8U);

    if (stw64_rc != (int32_t)sigusr1 ||
        si.si_signo != (int32_t)sigusr1 ||
        g_r8_usr1_hits != 0U ||
        pend_rc != 0 ||
        (pend_mask & usr1_bit) != 0ULL) {
        return 200;
    }

    /*
     * 20b: Negative clone (120) flag & pointer validation:
     *   - CLONE_SIGHAND (0x00000800U) without CLONE_VM (0x00000100U) -> -EINVAL (-22)
     *   - CLONE_THREAD (0x00010000U) without CLONE_SIGHAND (0x00000800U) -> -EINVAL (-22)
     *   - CLONE_VM | CLONE_SIGHAND | CLONE_PARENT_SETTID with ptid = 0x100U -> -EFAULT (-14)
     */
    uint32_t valid_sp = (uint32_t)(uintptr_t)&dummy_clone_stack[64];

    int32_t c_rc1 = sys6(120U /* clone */, 0x00000800U /* CLONE_SIGHAND */,
                         valid_sp, 0U, 0U, 0U, 0U);
    if (c_rc1 == 0) {
        sys1(1U /* exit */, 99U);
    }
    if (c_rc1 != -22 /* -EINVAL */) {
        return 201;
    }

    int32_t c_rc2 = sys6(120U /* clone */,
                         0x00000100U | 0x00010000U /* CLONE_VM | CLONE_THREAD */,
                         valid_sp, 0U, 0U, 0U, 0U);
    if (c_rc2 == 0) {
        sys1(1U /* exit */, 99U);
    }
    if (c_rc2 != -22 /* -EINVAL */) {
        return 202;
    }

    int32_t c_rc3 = sys6(120U /* clone */, 0x00010000U /* CLONE_THREAD */,
                         valid_sp, 0U, 0U, 0U, 0U);
    if (c_rc3 == 0) {
        sys1(1U /* exit */, 99U);
    }
    if (c_rc3 != -22 /* -EINVAL */) {
        return 203;
    }

    int32_t c_rc4 = sys6(120U /* clone */,
                         0x00000100U | 0x00000800U | 0x00100000U /* CLONE_VM | CLONE_SIGHAND | CLONE_PARENT_SETTID */,
                         valid_sp, 0x100U /* invalid ptid */, 0U, 0U, 0U);
    if (c_rc4 == 0) {
        sys1(1U /* exit */, 99U);
    }
    if (c_rc4 != -14 /* -EFAULT */) {
        return 204;
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * Test 21: Round 10 regressions —
 *   21a: Catchable guest SIGSEGV (11) on instruction fetch to NULL (0x0) and
 *        to a non-executable PROT_READ | PROT_WRITE page (mmap2 without
 *        PROT_EXEC), verifying si_signo == 11 and uc_mcontext.arm_pc == target_pc,
 *        setting uc_mcontext.arm_pc = uc_mcontext.arm_lr, and resuming cleanly
 *   21b: rt_sigtimedwait (177) with rt_sigqueueinfo (178) on blocked signal 36
 *        (SIGRTMIN+2, left at SIG_DFL):
 *        - Negative: rt_sigtimedwait with u_info = 0xFFFFFFFFU returns -EFAULT
 *          (-14) without losing the queued signal
 *        - Positive: rt_sigtimedwait with valid out_info returns 36 with
 *          si_code == -1 (SI_QUEUE), si_pid == getpid(), si_value == 0xCAFEBABE
 *   21c: set_tid_address (256) with unmapped address (0x25000000U) in a forked
 *        child calling exit(0), verifying wait4 reports clean exit status == 0
 * -------------------------------------------------------------------------- */
static volatile uint32_t g_r10_ifetch_hits = 0U;
static volatile uint32_t g_r10_ifetch_ok = 0U;
static volatile uint32_t g_r10_ifetch_expected_pc = 0U;

static void r10_ifetch_sigsegv_handler(int sig, void *info, void *uctx_ptr)
{
    struct ucontext32_m3 *uc = (struct ucontext32_m3 *)uctx_ptr;
    const struct siginfo32 *si = (const struct siginfo32 *)info;
    uint32_t expected_pc = g_r10_ifetch_expected_pc;

    if (sig == 11 && si != NULL &&
        si->si_signo == 11 &&
        uc->uc_mcontext.arm_pc == expected_pc &&
        (uint32_t)(uintptr_t)si->si_addr == expected_pc) {
        g_r10_ifetch_ok++;
    }

    /* Resume at caller return address in lr */
    uc->uc_mcontext.arm_pc = uc->uc_mcontext.arm_lr;
    g_r10_ifetch_hits++;
}

static int test21_r10_ifetch_sigsegv_sigqueueinfo_tid_address(void)
{
    static struct sigaction32 act_segv;
    static struct sigaction32 orig_segv;

    /*
     * 21a: Catchable guest SIGSEGV on instruction fetch to NULL (0x0) and to
     *      a non-executable PROT_READ | PROT_WRITE page (mmap2 without PROT_EXEC).
     */
    act_segv.sa_handler  = (uint32_t)(uintptr_t)r10_ifetch_sigsegv_handler;
    act_segv.sa_flags    = 0x04000004U; /* SA_SIGINFO | SA_RESTORER */
    act_segv.sa_restorer = (uint32_t)(uintptr_t)my_rt_restorer;
    act_segv.sa_mask[0]  = 0U;
    act_segv.sa_mask[1]  = 0U;

    if (sys4(174U /* rt_sigaction */, 11U /* SIGSEGV */,
             (uint32_t)(uintptr_t)&act_segv,
             (uint32_t)(uintptr_t)&orig_segv, 8U) != 0) {
        return 205;
    }

    /* 21a-1: Branch via bx to NULL (0x0U), keeping return address in lr */
    g_r10_ifetch_hits        = 0U;
    g_r10_ifetch_ok          = 0U;
    g_r10_ifetch_expected_pc = 0x0U;
    uint32_t target_null     = 0x0U;
    volatile uint32_t resumed_null = 0U;

    __asm__ volatile(
        "mov r0, %[target]\n\t"
        "adr lr, 1f\n\t"
        "bx  r0\n"
        "1:\n\t"
        "mov %[res], #1\n\t"
        : [res] "=&r"(resumed_null)
        : [target] "r"(target_null)
        : "r0", "lr", "memory", "cc"
    );

    if (resumed_null != 1U || g_r10_ifetch_hits != 1U || g_r10_ifetch_ok != 1U) {
        sys4(174U /* rt_sigaction */, 11U, (uint32_t)(uintptr_t)&orig_segv, 0U, 8U);
        return 206;
    }

    /* 21a-2: Branch via bx to a non-executable PROT_READ | PROT_WRITE page */
    uint32_t rw_page = (uint32_t)sys6(192U /* mmap2 */, 0U, 4096U,
                                      3U /* PROT_READ | PROT_WRITE (no PROT_EXEC) */,
                                      0x22U /* MAP_PRIVATE | MAP_ANONYMOUS */,
                                      0xFFFFFFFFU, 0U);
    if (rw_page > 0xFFFFF000U) {
        sys4(174U /* rt_sigaction */, 11U, (uint32_t)(uintptr_t)&orig_segv, 0U, 8U);
        return 207;
    }

    /* Place valid A32 'mov r0, #99; bx lr' bytes in the RW-only page */
    volatile uint32_t *rw_words = (volatile uint32_t *)(uintptr_t)rw_page;
    rw_words[0] = 0xE3A00063U; /* mov r0, #99 */
    rw_words[1] = 0xE12FFF1EU; /* bx lr */

    g_r10_ifetch_expected_pc = rw_page;
    volatile uint32_t resumed_nx = 0U;

    __asm__ volatile(
        "mov r0, %[target]\n\t"
        "adr lr, 1f\n\t"
        "bx  r0\n"
        "1:\n\t"
        "mov %[res], #1\n\t"
        : [res] "=&r"(resumed_nx)
        : [target] "r"(rw_page)
        : "r0", "lr", "memory", "cc"
    );

    sys2(91U /* munmap */, rw_page, 4096U);
    sys4(174U /* rt_sigaction */, 11U, (uint32_t)(uintptr_t)&orig_segv, 0U, 8U);

    if (resumed_nx != 1U || g_r10_ifetch_hits != 2U || g_r10_ifetch_ok != 2U) {
        return 208;
    }

    /*
     * 21b: rt_sigtimedwait (177) with rt_sigqueueinfo (178):
     *   - Block signal 36 (SIGRTMIN+2) via rt_sigprocmask without registering
     *     a handler (leaving it SIG_DFL).
     *   - Send queued signal 36 via rt_sigqueueinfo(getpid(), 36, &uinfo) with
     *     si_code = -1 (SI_QUEUE), si_pid = getpid(), sival_int = 0xCAFEBABE.
     *   - Negative: rt_sigtimedwait with u_info = 0xFFFFFFFFU -> -EFAULT (-14)
     *     and verify queued signal is NOT lost.
     *   - Positive: rt_sigtimedwait with valid out_info -> returns 36 with
     *     out_info.si_code == -1, out_info.si_pid == getpid(), and
     *     out_info.si_value == 0xCAFEBABE.
     */
    const uint32_t sigrt36  = 36U; /* SIGRTMIN + 2 */
    const uint64_t rt36_bit = 1ULL << (sigrt36 - 1U);
    int32_t my_pid = sys1(20U /* getpid */, 0U);
    uint32_t my_uid = (uint32_t)sys1(199U /* getuid32 */, 0U);

    static uint64_t orig_mask;
    static uint64_t wait_set36;
    static struct siginfo32_rt_m3 uinfo;
    static struct siginfo32_rt_m3 out_info;
    static struct timespec32_m3 ts_wait;

    orig_mask  = 0ULL;
    wait_set36 = rt36_bit;
    if (sys4(175U /* rt_sigprocmask */, 0U /* SIG_BLOCK */,
             (uint32_t)(uintptr_t)&wait_set36,
             (uint32_t)(uintptr_t)&orig_mask, 8U) != 0) {
        return 209;
    }

    for (uint32_t i = 0U; i < 26U; i++) {
        uinfo._pad[i]    = 0U;
        out_info._pad[i] = 0U;
    }
    uinfo.si_signo = (int32_t)sigrt36;
    uinfo.si_errno = 0;
    uinfo.si_code  = -1; /* SI_QUEUE */
    uinfo.si_pid   = my_pid;
    uinfo.si_uid   = my_uid;
    uinfo.si_value = 0xCAFEBABEU;

    if (sys3(178U /* rt_sigqueueinfo */, (uint32_t)my_pid, sigrt36,
             (uint32_t)(uintptr_t)&uinfo) != 0) {
        sys4(175U /* rt_sigprocmask */, 2U /* SIG_SETMASK */,
             (uint32_t)(uintptr_t)&orig_mask, 0U, 8U);
        return 210;
    }

    ts_wait.tv_sec  = 0;
    ts_wait.tv_nsec = 0;

    /* Negative: u_info = 0xFFFFFFFFU must return -EFAULT (-14) and NOT lose signal 36 */
    int32_t bad_stw = sys4(177U /* rt_sigtimedwait */,
                           (uint32_t)(uintptr_t)&wait_set36,
                           0xFFFFFFFFU /* invalid u_info */,
                           (uint32_t)(uintptr_t)&ts_wait, 8U);
    if (bad_stw != -14 /* -EFAULT */) {
        sys4(175U /* rt_sigprocmask */, 2U /* SIG_SETMASK */,
             (uint32_t)(uintptr_t)&orig_mask, 0U, 8U);
        return 211;
    }

    /* Positive: valid out_info buffer consumes queued signal 36 with full siginfo */
    out_info.si_signo = 0;
    out_info.si_errno = -1;
    out_info.si_code  = 0;
    out_info.si_pid   = 0;
    out_info.si_uid   = 0U;
    out_info.si_value = 0U;

    int32_t ok_stw = sys4(177U /* rt_sigtimedwait */,
                          (uint32_t)(uintptr_t)&wait_set36,
                          (uint32_t)(uintptr_t)&out_info,
                          (uint32_t)(uintptr_t)&ts_wait, 8U);

    sys4(175U /* rt_sigprocmask */, 2U /* SIG_SETMASK */,
         (uint32_t)(uintptr_t)&orig_mask, 0U, 8U);

    if (ok_stw != (int32_t)sigrt36 ||
        out_info.si_signo != (int32_t)sigrt36 ||
        out_info.si_code != -1 /* SI_QUEUE */ ||
        out_info.si_pid != my_pid ||
        out_info.si_value != 0xCAFEBABEU) {
        return 212;
    }

    /*
     * 21c: Positive regression test for set_tid_address (256) with an unmapped
     *      address (0x25000000U) in a forked child calling exit(0), verifying
     *      wait4 reports clean exit status == 0 (!WIFSIGNALED).
     */
    int32_t fpid = sys1(2U /* fork */, 0U);
    if (fpid < 0) {
        return 213;
    }
    if (fpid == 0) {
        int32_t ctid = sys1(256U /* set_tid_address */, 0x25000000U);
        if (ctid <= 0) {
            sys1(1U /* exit */, 97U);
        }
        sys1(1U /* exit */, 0U);
        __builtin_unreachable();
    }

    static uint32_t wstatus;
    wstatus = 0xDEADBEEFU;
    int32_t wrc = sys4(114U /* wait4 */, (uint32_t)fpid,
                       (uint32_t)(uintptr_t)&wstatus, 0U, 0U);
    if (wrc != fpid || wstatus != 0U) {
        return 214;
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * Test 22: VFP_MAGIC (0x56465001) Coprocessor Frame in ucontext.uc_regspace
 * -------------------------------------------------------------------------- */
static volatile uint32_t g_vfp_sigframe_ok = 0U;

static void vfp_uc_sigusr1_handler(int sig, void *info, void *ucontext)
{
    (void)sig;
    (void)info;
    if (ucontext == NULL) {
        return;
    }
    uint8_t *uc = (uint8_t *)ucontext;
    /* uc_regspace starts at byte offset 232 of arm32 struct ucontext */
    uint32_t magic = *(uint32_t *)(void *)(uc + 232U);
    uint32_t size  = *(uint32_t *)(void *)(uc + 236U);
    uint64_t d0_in  = *(uint64_t *)(void *)(uc + 240U + 0U * 8U);
    uint64_t d16_in = *(uint64_t *)(void *)(uc + 240U + 16U * 8U);
    uint32_t fpexc  = *(uint32_t *)(void *)(uc + 240U + 32U * 8U + 4U);
    uint32_t end_magic = *(uint32_t *)(void *)(uc + 232U + 0x120U);

    /* 123.5 == 0x405EE00000000000ULL, -456.25 == 0xC07C840000000000ULL */
    if (magic == 0x56465001U && size == 0x120U &&
        d0_in == 0x405EE00000000000ULL &&
        d16_in == 0xC07C840000000000ULL &&
        (fpexc & 0x40000000U) != 0U &&
        end_magic == 0U) {
        /* Mutate fpregs[0] = 789.125 (0x4088A90000000000ULL) and
         * fpregs[16] = 1024.5 (0x4090020000000000ULL) */
        *(uint64_t *)(void *)(uc + 240U + 0U * 8U)  = 0x4088A90000000000ULL;
        *(uint64_t *)(void *)(uc + 240U + 16U * 8U) = 0x4090020000000000ULL;
        g_vfp_sigframe_ok = 1U;
    }
}

static int test22_vfp_sigframe_uc_regspace(void)
{
    g_vfp_sigframe_ok = 0U;

    struct sigaction32 act;
    act.sa_handler = (uint32_t)(uintptr_t)vfp_uc_sigusr1_handler;
    act.sa_flags = 0x04000004U; /* SA_SIGINFO | SA_RESTORER */
    act.sa_restorer = (uint32_t)(uintptr_t)my_rt_restorer;
    act.sa_mask[0] = 0U;
    act.sa_mask[1] = 0U;

    if (sys4(174U /* rt_sigaction */, 10U /* SIGUSR1 */,
             (uint32_t)(uintptr_t)&act, 0U, 8U) != 0) {
        return 220;
    }

    uint32_t tid = (uint32_t)sys1(224U /* gettid */, 0U);
    uint64_t in_d0  = 0x405EE00000000000ULL; /* 123.5 */
    uint64_t in_d16 = 0xC07C840000000000ULL; /* -456.25 */
    uint64_t out_d0  = 0ULL;
    uint64_t out_d16 = 0ULL;
    int32_t tkill_rc = -1;

    const uint64_t *p_in_d0  = &in_d0;
    const uint64_t *p_in_d16 = &in_d16;
    uint64_t *p_out_d0  = &out_d0;
    uint64_t *p_out_d16 = &out_d16;

    __asm__ volatile(
        "vldmia %1, {d0}\n"
        "vldmia %2, {d16}\n"
        "mov r0, %3\n"
        "mov r1, #10\n"
        "mov r7, #238\n"
        "svc #0\n"
        "mov %0, r0\n"
        "vstmia %4, {d0}\n"
        "vstmia %5, {d16}\n"
        : "=&r"(tkill_rc)
        : "r"(p_in_d0), "r"(p_in_d16), "r"(tid), "r"(p_out_d0), "r"(p_out_d16)
        : "r0", "r1", "r7", "d0", "d16", "memory", "cc"
    );

    struct sigaction32 dfl;
    dfl.sa_handler = 0U; /* SIG_DFL */
    dfl.sa_flags = 0U;
    dfl.sa_restorer = 0U;
    dfl.sa_mask[0] = 0U;
    dfl.sa_mask[1] = 0U;
    sys4(174U /* rt_sigaction */, 10U /* SIGUSR1 */,
         (uint32_t)(uintptr_t)&dfl, 0U, 8U);

    if (tkill_rc != 0 || g_vfp_sigframe_ok != 1U) {
        return 221;
    }
    if (out_d0 != 0x4088A90000000000ULL || out_d16 != 0x4090020000000000ULL) {
        return 222;
    }
    return 0;
}

static volatile uint32_t g_m6_sigill_count = 0U;

static void m6_sigill_advance4_handler(int sig, void *info, void *ucontext)
{
    if (sig != 4 /* SIGILL */ || info == NULL || ucontext == NULL) {
        return;
    }
    const struct siginfo32 *si = (const struct siginfo32 *)info;
    uint8_t *uc = (uint8_t *)ucontext;
    if (si->si_signo == 4 && si->si_code == 1 /* ILL_ILLOPC */) {
        g_m6_sigill_count++;
    }
    *(uint32_t *)(void *)(uc + 92U) += 4U;
}

static void m6_corrupt_vfp_uc_handler(int sig, void *info, void *ucontext)
{
    (void)sig;
    (void)info;
    if (ucontext == NULL) {
        return;
    }
    uint8_t *uc = (uint8_t *)ucontext;
    /* Corrupt VFP_MAGIC and fpregs[0] so rt_sigreturn must reject the corrupted frame */
    *(uint32_t *)(void *)(uc + 232U) = 0xDEADBEEFU;
    *(uint64_t *)(void *)(uc + 240U) = 0x409999999999999AULL;
}

static int test23_m6_adversarial_regressions(void)
{
    /* 1. Adversarial Finding 2.1: futex uaddr2 bounds check on FUTEX_CMP_REQUEUE / FUTEX_WAKE_OP */
    uint32_t fword = 42U;
    /* FUTEX_CMP_REQUEUE (4) and FUTEX_WAKE_OP (5) with unmapped uaddr2=0x100 must return -EFAULT (-14) */
    if (sys6(240U /* futex */, (uint32_t)(uintptr_t)&fword, 4U /* FUTEX_CMP_REQUEUE */,
             1U, 1U, 0x100U /* invalid uaddr2 */, 42U) != -14 /* -EFAULT */) {
        return 230;
    }
    if (sys6(240U /* futex */, (uint32_t)(uintptr_t)&fword, 5U /* FUTEX_WAKE_OP */,
             1U, 1U, 0x100U /* invalid uaddr2 */, 0U) != -14 /* -EFAULT */) {
        return 231;
    }
    /* FUTEX_WAIT (0) ignores uaddr2 (a4), so passing a4=0x100 when fword != val must return -EAGAIN (-11), NOT -EFAULT */
    if (sys6(240U /* futex */, (uint32_t)(uintptr_t)&fword, 0U /* FUTEX_WAIT */,
             99U /* != 42 */, 0U, 0x100U, 0U) != -11 /* -EAGAIN */) {
        return 232;
    }

    /* 2. Adversarial Finding 3.1 & M6 Decoder SIGILL negative checks:
     *    - invalid VDUP scalar, imm4=8 (0xf3b80c00)
     *    - invalid VDUP.32 Q, is_q=1 and odd Qd d=17 (0xf3f41c40)
     *    - CRC32 with sz=3 (0xe1600040)
     *    - AESE.8 with odd Qd (d=1: 0xf3b01300)
     */
    g_m6_sigill_count = 0U;
    struct sigaction32 act;
    act.sa_handler  = (uint32_t)(uintptr_t)m6_sigill_advance4_handler;
    act.sa_flags    = 0x04000004U; /* SA_SIGINFO | SA_RESTORER */
    act.sa_restorer = (uint32_t)(uintptr_t)my_rt_restorer;
    act.sa_mask[0]  = 0U;
    act.sa_mask[1]  = 0U;
    if (sys4(174U /* rt_sigaction */, 4U /* SIGILL */,
             (uint32_t)(uintptr_t)&act, 0U, 8U) != 0) {
        return 233;
    }

    __asm__ volatile(
        ".inst 0xf3b80c00\n\t" /* invalid VDUP scalar with imm4=8 ((imm4 & 7) == 0) */
        ".inst 0xf3f41c40\n\t" /* invalid VDUP.32 Q with is_q=1 (bit 6) and odd Qd (d=17) */
        ".inst 0xe1600040\n\t" /* invalid CRC32 with sz=3 */
        ".inst 0xf3b01300\n\t" /* invalid AESE.8 with odd Qd (d=1) */
        :
        :
        : "memory", "cc"
    );

    struct sigaction32 dfl;
    dfl.sa_handler  = 0U;
    dfl.sa_flags    = 0U;
    dfl.sa_restorer = 0U;
    dfl.sa_mask[0]  = 0U;
    dfl.sa_mask[1]  = 0U;
    sys4(174U /* rt_sigaction */, 4U /* SIGILL */,
         (uint32_t)(uintptr_t)&dfl, 0U, 8U);

    if (g_m6_sigill_count != 4U) {
        return 234;
    }

    /* 3. Negative VFP_MAGIC uc_regspace test: corrupted magic in uc_regspace is ignored on rt_sigreturn */
    act.sa_handler = (uint32_t)(uintptr_t)m6_corrupt_vfp_uc_handler;
    if (sys4(174U /* rt_sigaction */, 10U /* SIGUSR1 */,
             (uint32_t)(uintptr_t)&act, 0U, 8U) != 0) {
        return 235;
    }
    uint32_t tid = (uint32_t)sys1(224U /* gettid */, 0U);
    uint64_t in_d0  = 0x405EE00000000000ULL; /* 123.5 */
    uint64_t out_d0 = 0ULL;
    const uint64_t *p_in_d0 = &in_d0;
    uint64_t *p_out_d0 = &out_d0;
    __asm__ volatile(
        "vldmia %1, {d0}\n\t"
        "mov r0, %2\n\t"
        "mov r1, #10\n\t"
        "mov r7, #238\n\t"
        "svc #0\n\t"
        "vstmia %0, {d0}\n\t"
        :
        : "r"(p_out_d0), "r"(p_in_d0), "r"(tid)
        : "r0", "r1", "r7", "d0", "memory", "cc"
    );
    sys4(174U /* rt_sigaction */, 10U /* SIGUSR1 */,
         (uint32_t)(uintptr_t)&dfl, 0U, 8U);
    if (out_d0 != 0x405EE00000000000ULL) {
        return 236;
    }

    /* 4. SMC (cacheflush) invalidation of an active inline RAS return block */
    uint32_t rwx = (uint32_t)sys6(192U /* mmap2 */, 0U, 4096U,
                                  7U /* PROT_READ|PROT_WRITE|PROT_EXEC */,
                                  0x22U /* MAP_PRIVATE|MAP_ANONYMOUS */,
                                  0xFFFFFFFFU, 0U);
    if (rwx > 0xFFFFF000U) {
        return 237;
    }
    uint32_t *code = (uint32_t *)(uintptr_t)rwx;
    /* caller at code[0..3]:
     *   [0] push {r4, lr}      (0xe92d4010)
     *   [1] bl callee (+12)    (0xeb000002 -> code[5])
     *   [2] mov r0, #11        (0xe3a0000b)  <-- return site in RAS, patched by callee to mov r0, #77!
     *   [3] pop {r4, pc}       (0xe8bd8010)
     *   [4] nop                (0xe1a00000)
     * callee at code[5..12]:
     *   [5] cmp r0, #0         (0xe3500000)
     *   [6] bxeq lr            (0x012fff1e)  <-- warmup call returns without patching so RAS/ret_entry is linked!
     *   [7] ldr r1, [pc, #24]  (0xe59f1018) loads the literal at code[15] (mov r0, #77)
     *   [8] str r1, [lr]       (0xe58e1000) overwrites code[2] with that mov
     */
    code[0]  = 0xE92D4010U; /* push {r4, lr} */
    code[1]  = 0xEB000002U; /* bl code[5] */
    code[2]  = 0xE3A0000BU; /* mov r0, #11 */
    code[3]  = 0xE8BD8010U; /* pop {r4, pc} */
    code[4]  = 0xE1A00000U; /* nop */
    code[5]  = 0xE3500000U; /* cmp r0, #0 */
    code[6]  = 0x012FFF1EU; /* bxeq lr */
    code[7]  = 0xE59F1014U; /* ldr r1, [pc, #20] -> code[14] */
    code[8]  = 0xE58E1000U; /* str r1, [lr] (overwrite code[2] with mov r0, #77) */
    code[9]  = 0xE1A0000EU; /* mov r0, lr */
    code[10] = 0xE2801004U; /* add r1, r0, #4 */
    code[11] = 0xE3A02000U; /* mov r2, #0 */
    code[12] = 0xE59F7004U; /* ldr r7, [pc, #4] -> code[15] (0x0f0002 cacheflush) */
    code[13] = 0xEF000000U; /* svc #0 */
    code[14] = 0xE12FFF1EU; /* bx lr */
    code[15] = 0xE3A0004DU; /* literal: mov r0, #77 */
    code[16] = 0x000F0002U; /* literal: __ARM_NR_cacheflush */
    /* Fix PC-relative offsets for code[7] and code[12]:
     * At code[7], PC is code + 9*4; code[15] is at offset (15 - 9)*4 = 24 (0x18).
     * At code[12], PC is code + 14*4; code[16] is at offset (16 - 14)*4 = 8 (0x08). */
    code[7]  = 0xE59F1018U;
    code[12] = 0xE59F7008U;

    sys3(0x0F0002U /* cacheflush */, rwx, rwx + 128U, 0U);
    uint32_t (*smc_fn)(uint32_t) = (uint32_t (*)(uint32_t))(uintptr_t)rwx;
    /* Warmup call (arg=0): compiles caller, callee, and return block code[2..3], linking ret_entry and RAS */
    uint32_t res_warm = smc_fn(0U);
    /* Second call (arg=1): callee overwrites return block at [lr] and calls cacheflush before bx lr */
    uint32_t res_patched = smc_fn(1U);
    sys2(91U /* munmap */, rwx, 4096U);
    if (res_warm != 11U || res_patched != 77U) {
        return 238;
    }

    return 0;
}


int run_test_m3_part4(void)
{
    int rc = test20_r9_sigtimedwait_and_clone_validation();
    if (rc != 0) return rc;
    rc = test21_r10_ifetch_sigsegv_sigqueueinfo_tid_address();
    if (rc != 0) return rc;
    rc = test22_vfp_sigframe_uc_regspace();
    if (rc != 0) return rc;
    return test23_m6_adversarial_regressions();
}
