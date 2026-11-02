/*
 * tests/test_m3_part2.c — M3 threads & signals tests (part 2: tests 12..16)
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_m3_common.h"

static int test12_chaining_partial_cacheflush(void)
{
    uint32_t pages = (uint32_t)sys6(192U /* mmap2 */, 0U, 12288U,
                                    3U /* PROT_READ|PROT_WRITE */,
                                    0x22U /* MAP_PRIVATE|MAP_ANONYMOUS */,
                                    0xFFFFFFFFU, 0U);
    if (pages > 0xFFFFF000U) {
        return 105;
    }
    uint32_t page_a = pages;
    uint32_t page_b = pages + 4096U;
    uint32_t page_c = pages + 8192U;

    uint32_t *code_a = (uint32_t *)(uintptr_t)page_a;
    uint32_t *code_b = (uint32_t *)(uintptr_t)page_b;
    uint32_t *code_c = (uint32_t *)(uintptr_t)page_c;

    /* Page A: add r0, r0, #5; b page_b */
    int32_t disp = ((int32_t)page_b - (int32_t)(page_a + 12U)) >> 2;
    code_a[0] = 0xE2800005U; /* add r0, r0, #5 */
    code_a[1] = 0xEA000000U | ((uint32_t)disp & 0x00FFFFFFU); /* b page_b */

    /* Page B: add r0, r0, #10; bx lr */
    code_b[0] = 0xE280000AU; /* add r0, r0, #10 */
    code_b[1] = 0xE12FFF1EU; /* bx lr */

    /* Page C: mov r0, #77; bx lr */
    code_c[0] = 0xE3A0004DU; /* mov r0, #77 */
    code_c[1] = 0xE12FFF1EU; /* bx lr */

    if (sys3(125U /* mprotect */, pages, 12288U, 5U /* PROT_READ|PROT_EXEC */) != 0) {
        sys2(91U /* munmap */, pages, 12288U);
        return 106;
    }
    sys3(0x0F0002U /* cacheflush */, pages, pages + 12288U, 0U);

    typedef uint32_t (*chain_fn_t)(uint32_t);
    chain_fn_t fn_a = (chain_fn_t)(uintptr_t)page_a;

    /* First call translates A & B and patches A->B chain; second runs chained */
    if (fn_a(100U) != 115U || fn_a(100U) != 115U) {
        sys2(91U /* munmap */, pages, 12288U);
        return 107;
    }

    /* Partial cacheflush of unrelated page C must preserve A->B chain */
    sys3(0x0F0002U /* cacheflush */, page_c, page_c + 16U, 0U);
    if (fn_a(100U) != 115U || fn_a(100U) != 115U) {
        sys2(91U /* munmap */, pages, 12288U);
        return 108;
    }

    /*
     * Now patch ONLY page B (add r0, r0, #37) and cacheflush ONLY page B.
     * Invalidation of page B must unchain page A's outgoing chain slot so
     * calling fn_a(100) re-translates page B and returns 100 + 5 + 37 = 142.
     */
    if (sys3(125U /* mprotect */, page_b, 4096U, 3U /* PROT_READ|PROT_WRITE */) != 0) {
        sys2(91U /* munmap */, pages, 12288U);
        return 109;
    }
    code_b[0] = 0xE2800025U; /* add r0, r0, #37 */
    if (sys3(125U /* mprotect */, page_b, 4096U, 5U /* PROT_READ|PROT_EXEC */) != 0) {
        sys2(91U /* munmap */, pages, 12288U);
        return 110;
    }
    sys3(0x0F0002U /* cacheflush */, page_b, page_b + 16U, 0U);

    if (fn_a(100U) != 142U || fn_a(100U) != 142U) {
        sys2(91U /* munmap */, pages, 12288U);
        return 111;
    }

    sys2(91U /* munmap */, pages, 12288U);
    return 0;
}

/* --------------------------------------------------------------------------
 * Test 13: FUTEX_WAIT_BITSET timeout & FUTEX_CMP_REQUEUE val2 regression
 * -------------------------------------------------------------------------- */
static volatile uint32_t g_requeue_f1 = 77U;
static volatile uint32_t g_requeue_f2 = 0U;
static volatile uint32_t g_requeue_entered = 0U;
static volatile uint32_t g_requeue_woken = 0U;

static void requeue_worker_thread(void)
{
    g_requeue_entered = 1U;
    while (g_requeue_f1 == 77U) {
        sys6(240U /* futex */, (uint32_t)(uintptr_t)&g_requeue_f1,
             0U | 128U /* FUTEX_WAIT_PRIVATE */, 77U, 0U, 0U, 0U);
    }
    g_requeue_woken = 1U;
}

static int test13_futex_bitset_requeue(void)
{
    volatile uint32_t fword = 42U;
    struct timespec32_m3 ts;

    /*
     * 13a: Negative — FUTEX_WAIT_BITSET (9 | 128) with bitset == 0 must return
     * -EINVAL (-22), and mismatched val must return -EAGAIN (-11).
     */
    ts.tv_sec = 0;
    ts.tv_nsec = 1000000;
    int32_t rc = sys6(240U /* futex */, (uint32_t)(uintptr_t)&fword,
                      9U | 128U /* FUTEX_WAIT_BITSET_PRIVATE */, 42U,
                      (uint32_t)(uintptr_t)&ts, 0U, 0U /* bitset == 0 */);
    if (rc != -22 /* -EINVAL */) {
        return 112;
    }

    rc = sys6(240U /* futex */, (uint32_t)(uintptr_t)&fword,
              9U | 128U /* FUTEX_WAIT_BITSET_PRIVATE */, 43U /* mismatch */,
              (uint32_t)(uintptr_t)&ts, 0U, 0xFFFFFFFFU);
    if (rc != -11 /* -EAGAIN */) {
        return 113;
    }

    /*
     * 13b: Positive — FUTEX_WAIT_BITSET (9 | 128) with 1 ms timeout must convert
     * the 32-bit struct timespec and return -ETIMEDOUT (-110) rather than
     * passing NULL to the host kernel and blocking forever.
     */
    if (sys2(263U /* clock_gettime */, 1U /* CLOCK_MONOTONIC */,
             (uint32_t)(uintptr_t)&ts) != 0) {
        return 114;
    }
    ts.tv_nsec += 1000000; /* +1 ms */
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000;
    }
    rc = sys6(240U /* futex */, (uint32_t)(uintptr_t)&fword,
              9U | 128U /* FUTEX_WAIT_BITSET_PRIVATE */, 42U,
              (uint32_t)(uintptr_t)&ts, 0U, 0xFFFFFFFFU);
    if (rc != -110 /* -ETIMEDOUT */) {
        return 115;
    }

    /*
     * 13c: Negative — FUTEX_CMP_REQUEUE (4 | 128) with wrong expected val3
     * must return -EAGAIN (-11).
     */
    g_requeue_f1 = 77U;
    g_requeue_f2 = 0U;
    rc = sys6(240U /* futex */, (uint32_t)(uintptr_t)&g_requeue_f1,
              4U | 128U /* FUTEX_CMP_REQUEUE_PRIVATE */,
              1U /* nr_wake */, 2U /* val2 = nr_requeue */,
              (uint32_t)(uintptr_t)&g_requeue_f2, 99U /* wrong val3 */);
    if (rc != -11 /* -EAGAIN */) {
        return 116;
    }

    /*
     * 13d: Positive — FUTEX_CMP_REQUEUE (4 | 128) with nr_wake = 0 and
     * val2 = nr_requeue = 1 on a live waiter thread must requeue 1 waiter from
     * g_requeue_f1 to g_requeue_f2 (returning 1) and allow waking it via
     * g_requeue_f2.
     */
    g_requeue_entered = 0U;
    g_requeue_woken = 0U;
    uint32_t stack_mem = (uint32_t)sys6(192U /* mmap2 */, 0U, 65536U,
                                        3U /* PROT_READ|PROT_WRITE */,
                                        0x22U /* MAP_PRIVATE|MAP_ANONYMOUS */,
                                        0xFFFFFFFFU, 0U);
    if (stack_mem > 0xFFFFF000U) {
        return 117;
    }
    uint32_t child_sp = (stack_mem + 65536U) & ~7U;
    volatile uint32_t parent_tid = 0U;
    volatile uint32_t child_tid = 1U;
    uint32_t flags = 0x013D0F00U;

    int clone_rc = arm_clone_trampoline(flags, (void *)(uintptr_t)child_sp,
                                        (uint32_t *)(uintptr_t)&parent_tid,
                                        NULL,
                                        (uint32_t *)(uintptr_t)&child_tid,
                                        requeue_worker_thread);
    if (clone_rc <= 0) {
        sys2(91U /* munmap */, stack_mem, 65536U);
        return 117;
    }

    while (g_requeue_entered == 0U) {
        sys1(158U /* sched_yield */, 0U);
    }

    int32_t req_rc = 0;
    for (int attempt = 0; attempt < 2000; attempt++) {
        req_rc = sys6(240U /* futex */, (uint32_t)(uintptr_t)&g_requeue_f1,
                      4U | 128U /* FUTEX_CMP_REQUEUE_PRIVATE */,
                      0U /* nr_wake = 0 */, 1U /* val2 = nr_requeue = 1 */,
                      (uint32_t)(uintptr_t)&g_requeue_f2, 77U /* val3 */);
        if (req_rc == 1) {
            break;
        }
        sys1(158U /* sched_yield */, 0U);
    }

    g_requeue_f1 = 88U;
    int32_t wake_rc = sys6(240U /* futex */, (uint32_t)(uintptr_t)&g_requeue_f2,
                           1U | 128U /* FUTEX_WAKE_PRIVATE */, 1U, 0U, 0U, 0U);
    /* Also wake g_requeue_f1 in case requeue failed so worker never hangs */
    sys6(240U /* futex */, (uint32_t)(uintptr_t)&g_requeue_f1,
         1U | 128U /* FUTEX_WAKE_PRIVATE */, 1U, 0U, 0U, 0U);

    while (child_tid != 0U) {
        sys6(240U /* futex */, (uint32_t)(uintptr_t)&child_tid, 0U /* FUTEX_WAIT */,
             child_tid, 0U, 0U, 0U);
    }
    sys2(91U /* munmap */, stack_mem, 65536U);

    if (req_rc != 1 || wake_rc != 1 || g_requeue_woken != 1U) {
        return 118;
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * Test 14: sigaltstack (SS_ONSTACK, EPERM while active, EINVAL flags)
 * -------------------------------------------------------------------------- */
volatile uint32_t g_altstack_base = 0U;
volatile uint32_t g_altstack_size = 0U;
static volatile uint32_t g_altstack_result = 0U;

static void altstack_sigusr1_handler(int sig, void *info, void *ucontext)
{
    (void)sig;
    (void)info;
    (void)ucontext;

    uint32_t cur_sp = 0U;
    __asm__ volatile("mov %0, sp" : "=r"(cur_sp));

    if (cur_sp < g_altstack_base || cur_sp >= g_altstack_base + g_altstack_size) {
        g_altstack_result = 1U;
        return;
    }

    struct stack32_m3 oss = { 0U, 0, 0U };
    if (sys2(186U /* sigaltstack */, 0U, (uint32_t)(uintptr_t)&oss) != 0) {
        g_altstack_result = 2U;
        return;
    }
    if ((oss.ss_flags & 1 /* SS_ONSTACK */) == 0 ||
        oss.ss_sp != g_altstack_base ||
        oss.ss_size != g_altstack_size) {
        g_altstack_result = 3U;
        return;
    }

    /* Negative: modifying sigaltstack while executing on it must return -EPERM (-1) */
    struct stack32_m3 try_ss;
    try_ss.ss_sp = g_altstack_base;
    try_ss.ss_flags = 2 /* SS_DISABLE */;
    try_ss.ss_size = 0U;
    if (sys2(186U /* sigaltstack */, (uint32_t)(uintptr_t)&try_ss, 0U) != -1 /* -EPERM */) {
        g_altstack_result = 4U;
        return;
    }

    g_altstack_result = 100U;
}

static int test14_sigaltstack(void)
{
    /* Negative: invalid ss_flags must return -EINVAL (-22) */
    struct stack32_m3 bad_ss;
    bad_ss.ss_sp = 0x10000U;
    bad_ss.ss_flags = 0x1234;
    bad_ss.ss_size = 16384U;
    if (sys2(186U /* sigaltstack */, (uint32_t)(uintptr_t)&bad_ss, 0U) != -22 /* -EINVAL */) {
        return 119;
    }

    uint32_t alt_mem = (uint32_t)sys6(192U /* mmap2 */, 0U, 16384U,
                                      3U /* PROT_READ|PROT_WRITE */,
                                      0x22U /* MAP_PRIVATE|MAP_ANONYMOUS */,
                                      0xFFFFFFFFU, 0U);
    if (alt_mem > 0xFFFFF000U) {
        return 120;
    }

    g_altstack_base = alt_mem;
    g_altstack_size = 16384U;
    g_altstack_result = 0U;

    struct stack32_m3 ss;
    ss.ss_sp = alt_mem;
    ss.ss_flags = 0;
    ss.ss_size = 16384U;
    struct stack32_m3 oss = { 0U, 0, 0U };
    if (sys2(186U /* sigaltstack */, (uint32_t)(uintptr_t)&ss,
             (uint32_t)(uintptr_t)&oss) != 0) {
        sys2(91U /* munmap */, alt_mem, 16384U);
        return 121;
    }

    /* Before signal delivery, SS_ONSTACK (1) must NOT be set */
    if (sys2(186U /* sigaltstack */, 0U, (uint32_t)(uintptr_t)&oss) != 0 ||
        (oss.ss_flags & 1) != 0 || oss.ss_sp != alt_mem || oss.ss_size != 16384U) {
        sys2(91U /* munmap */, alt_mem, 16384U);
        return 122;
    }

    struct sigaction32 act;
    struct sigaction32 old_act;
    act.sa_handler = (uint32_t)(uintptr_t)altstack_sigusr1_handler;
    /* SA_ONSTACK (0x08000000) | SA_RESTORER (0x04000000) | SA_SIGINFO (0x4) */
    act.sa_flags = 0x08000000U | 0x04000000U | 0x00000004U;
    act.sa_restorer = (uint32_t)(uintptr_t)my_rt_restorer;
    act.sa_mask[0] = 0U;
    act.sa_mask[1] = 0U;

    if (sys4(174U /* rt_sigaction */, 10U /* SIGUSR1 */,
             (uint32_t)(uintptr_t)&act, (uint32_t)(uintptr_t)&old_act, 8U) != 0) {
        sys2(91U /* munmap */, alt_mem, 16384U);
        return 123;
    }

    uint32_t tid = (uint32_t)sys1(224U /* gettid */, 0U);
    if (sys2(238U /* tkill */, tid, 10U /* SIGUSR1 */) != 0) {
        sys4(174U /* rt_sigaction */, 10U, (uint32_t)(uintptr_t)&old_act, 0U, 8U);
        sys2(91U /* munmap */, alt_mem, 16384U);
        return 124;
    }

    sys4(174U /* rt_sigaction */, 10U, (uint32_t)(uintptr_t)&old_act, 0U, 8U);

    if (g_altstack_result != 100U) {
        sys2(91U /* munmap */, alt_mem, 16384U);
        return 125;
    }

    /* After rt_sigreturn from alternate stack, SS_ONSTACK (1) must be cleared */
    if (sys2(186U /* sigaltstack */, 0U, (uint32_t)(uintptr_t)&oss) != 0 ||
        (oss.ss_flags & 1) != 0 || oss.ss_sp != alt_mem || oss.ss_size != 16384U) {
        sys2(91U /* munmap */, alt_mem, 16384U);
        return 126;
    }

    /* Disable alternate stack via SS_DISABLE (2) */
    ss.ss_sp = 0U;
    ss.ss_flags = 2 /* SS_DISABLE */;
    ss.ss_size = 0U;
    if (sys2(186U /* sigaltstack */, (uint32_t)(uintptr_t)&ss,
             (uint32_t)(uintptr_t)&oss) != 0) {
        sys2(91U /* munmap */, alt_mem, 16384U);
        return 127;
    }
    if (sys2(186U /* sigaltstack */, 0U, (uint32_t)(uintptr_t)&oss) != 0 ||
        oss.ss_flags != 2 /* SS_DISABLE */) {
        sys2(91U /* munmap */, alt_mem, 16384U);
        return 128;
    }

    sys2(91U /* munmap */, alt_mem, 16384U);
    return 0;
}

/* --------------------------------------------------------------------------
 * Test 15: UDF inside a Thumb IT block (SIGILL si_code, si_addr, arm_pc += 2,
 *          and preserved ITSTATE across rt_sigreturn)
 * -------------------------------------------------------------------------- */
uint32_t thumb_it_udf_helper(uint32_t dummy_r0, uint32_t *out_apsr);

__asm__(
    ".text\n"
    ".balign 4\n"
    ".thumb\n"
    ".thumb_func\n"
    ".globl thumb_it_udf_helper\n"
    ".type thumb_it_udf_helper, %function\n"
    "thumb_it_udf_helper:\n"
    "    .short 0x4280\n"             /* +0:  cmp r0, r0 (Z=1, C=1, N=0, V=0) */
    "    .short 0xbf01\n"             /* +2:  itttt eq (mask 0001: four following insns) */
    "    .short 0x200a\n"             /* +4:  moveq r0, #10 */
    "    .short 0xde00\n"             /* +6:  udf #0 (2-byte T16 undefined) */
    "    .short 0x2014\n"             /* +8:  moveq r0, #20 */
    "    .short 0x3005\n"             /* +10: addeq r0, r0, #5 */
    "    .short 0xf3ef, 0x8200\n"     /* +12: mrs r2, apsr */
    "    .short 0x600a\n"             /* +16: str r2, [r1, #0] */
    "    .short 0x4770\n"             /* +18: bx lr */
    ".arm\n"
);

static volatile uint32_t g_it_udf_handled = 0U;
static volatile uint32_t g_it_udf_ok = 0U;
static volatile uint32_t g_it_udf_expected_pc = 0U;

static void it_udf_sigill_handler(int sig, void *info, void *uctx_ptr)
{
    uint8_t *uc = (uint8_t *)uctx_ptr;
    uint32_t arm_r0     = *(uint32_t *)(uc + 32);
    uint32_t arm_pc     = *(uint32_t *)(uc + 92);
    uint32_t fault_addr = *(uint32_t *)(uc + 100);

    /* Advance past the 2-byte Thumb UDF instruction */
    *(uint32_t *)(uc + 92) = arm_pc + 2U;

    const struct siginfo32 *si = (const struct siginfo32 *)info;
    if (sig == 4 && si != NULL &&
        si->si_signo == 4 &&
        si->si_code == 1 /* ILL_ILLOPC */ &&
        (uint32_t)(uintptr_t)si->si_addr == g_it_udf_expected_pc &&
        arm_pc == g_it_udf_expected_pc &&
        fault_addr == g_it_udf_expected_pc &&
        arm_r0 == 10U) {
        g_it_udf_ok = 1U;
    }
    g_it_udf_handled = 1U;
}

static int test15_thumb_it_udf_sigill(void)
{
    g_it_udf_handled = 0U;
    g_it_udf_ok = 0U;
    uint32_t fn_base = ((uint32_t)(uintptr_t)thumb_it_udf_helper) & ~1U;
    g_it_udf_expected_pc = fn_base + 6U;

    struct sigaction32 act;
    struct sigaction32 old_act;
    act.sa_handler  = (uint32_t)(uintptr_t)it_udf_sigill_handler;
    act.sa_flags    = 0x04000004U; /* SA_SIGINFO | SA_RESTORER */
    act.sa_restorer = (uint32_t)(uintptr_t)my_rt_restorer;
    act.sa_mask[0]  = 0U;
    act.sa_mask[1]  = 0U;

    if (sys4(174U /* rt_sigaction */, 4U /* SIGILL */,
             (uint32_t)(uintptr_t)&act, (uint32_t)(uintptr_t)&old_act, 8U) != 0) {
        return 129;
    }

    uint32_t apsr_after = 0U;
    uint32_t r0_after = thumb_it_udf_helper(0U, &apsr_after);

    sys4(174U /* rt_sigaction */, 4U /* SIGILL */,
         (uint32_t)(uintptr_t)&old_act, 0U, 8U);

    if (g_it_udf_handled != 1U || g_it_udf_ok != 1U) {
        return 130;
    }
    /*
     * Verify the remaining two instructions in `itttt eq` (`moveq r0, #20` and
     * `addeq r0, r0, #5`) executed to produce r0 == 25, AND that ITSTATE was
     * preserved across rt_sigreturn (16-bit moveq/addeq inside an IT block do
     * not modify NZCV, so Z=1 / C=1 == 0x60000000U remains intact).
     */
    if (r0_after != 25U || (apsr_after & 0xF0000000U) != 0x60000000U) {
        return 131;
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * Test 16: Asynchronous kill(getpid(), SIGUSR1) / tgkill SA_SIGINFO si_code & si_pid
 * -------------------------------------------------------------------------- */
static volatile uint32_t g_kill_siginfo_calls = 0U;
static volatile int32_t  g_kill_last_signo = 0;
static volatile int32_t  g_kill_last_code = 999;
static volatile int32_t  g_kill_last_pid = -1;

static void kill_siginfo_handler(int sig, void *info, void *ucontext)
{
    (void)ucontext;
    g_kill_siginfo_calls++;
    g_kill_last_signo = sig;
    if (info != NULL) {
        const struct siginfo32_kill *si = (const struct siginfo32_kill *)info;
        g_kill_last_code = si->si_code;
        g_kill_last_pid  = si->si_pid;
    }
}

static int test16_async_kill_tgkill_siginfo(void)
{
    struct sigaction32 act;
    struct sigaction32 old_act;
    act.sa_handler  = (uint32_t)(uintptr_t)kill_siginfo_handler;
    act.sa_flags    = 0x04000004U; /* SA_SIGINFO | SA_RESTORER */
    act.sa_restorer = (uint32_t)(uintptr_t)my_rt_restorer;
    act.sa_mask[0]  = 0U;
    act.sa_mask[1]  = 0U;

    if (sys4(174U /* rt_sigaction */, 10U /* SIGUSR1 */,
             (uint32_t)(uintptr_t)&act, (uint32_t)(uintptr_t)&old_act, 8U) != 0) {
        return 132;
    }

    int32_t my_pid = sys1(20U /* getpid */, 0U);
    int32_t my_tid = sys1(224U /* gettid */, 0U);
    if (my_pid <= 0 || my_tid <= 0) {
        sys4(174U /* rt_sigaction */, 10U, (uint32_t)(uintptr_t)&old_act, 0U, 8U);
        return 133;
    }

    /* 16a: kill(getpid(), SIGUSR1) -> si_code <= 0 (SI_USER == 0) and si_pid == getpid() */
    g_kill_siginfo_calls = 0U;
    g_kill_last_signo = 0;
    g_kill_last_code = 999;
    g_kill_last_pid = -1;

    if (sys2(37U /* kill */, (uint32_t)my_pid, 10U /* SIGUSR1 */) != 0) {
        sys4(174U /* rt_sigaction */, 10U, (uint32_t)(uintptr_t)&old_act, 0U, 8U);
        return 134;
    }
    if (g_kill_siginfo_calls != 1U ||
        g_kill_last_signo != 10 ||
        g_kill_last_code > 0 ||
        (g_kill_last_code != 0 /* SI_USER */ && g_kill_last_code != -6 /* SI_TKILL */) ||
        g_kill_last_pid != my_pid) {
        sys4(174U /* rt_sigaction */, 10U, (uint32_t)(uintptr_t)&old_act, 0U, 8U);
        return 135;
    }

    /* 16b: tgkill(getpid(), gettid(), SIGUSR1) -> si_code <= 0 (SI_TKILL == -6) and si_pid == getpid() */
    g_kill_siginfo_calls = 0U;
    g_kill_last_signo = 0;
    g_kill_last_code = 999;
    g_kill_last_pid = -1;

    if (sys3(268U /* tgkill */, (uint32_t)my_pid, (uint32_t)my_tid, 10U /* SIGUSR1 */) != 0) {
        sys4(174U /* rt_sigaction */, 10U, (uint32_t)(uintptr_t)&old_act, 0U, 8U);
        return 136;
    }
    sys4(174U /* rt_sigaction */, 10U, (uint32_t)(uintptr_t)&old_act, 0U, 8U);

    if (g_kill_siginfo_calls != 1U ||
        g_kill_last_signo != 10 ||
        g_kill_last_code > 0 ||
        (g_kill_last_code != -6 /* SI_TKILL */ && g_kill_last_code != 0 /* SI_USER */) ||
        g_kill_last_pid != my_pid) {
        return 137;
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * Test 17: Round 6 regressions —
 *   17a: In-place aliasing in rt_sigaction(SIGUSR2, &sa, &sa) and
 *        rt_sigprocmask(SIG_BLOCK, &mask, &mask)
 *   17b: Pending blocked signal set to SIG_IGN via rt_sigaction is discarded
 *        and does NOT fire when unblocked via rt_sigprocmask(SIG_UNBLOCK)
 *   17c: sigaltstack with SS_AUTODISARM (0x80000000U) reports SS_DISABLE
 *        inside SA_ONSTACK handler and re-arms after rt_sigreturn
 * -------------------------------------------------------------------------- */

int run_test_m3_part2(void)
{
    int rc = test12_chaining_partial_cacheflush();
    if (rc != 0) return rc;
    rc = test13_futex_bitset_requeue();
    if (rc != 0) return rc;
    rc = test14_sigaltstack();
    if (rc != 0) return rc;
    rc = test15_thumb_it_udf_sigill();
    if (rc != 0) return rc;
    return test16_async_kill_tgkill_siginfo();
}
