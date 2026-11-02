/*
 * tests/test_m5_part3.c — M5 LTP compatibility tests (part 3: tests 20..24)
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_m5_common.h"

struct timezone32_m5 {
    int32_t tz_minuteswest;
    int32_t tz_dsttime;
};

static int test20_r7_truncate_affinity_gettimeofday(void)
{
    /*
     * 20a: truncate (92) and ftruncate (93):
     *   - Create a 64-byte file, ftruncate (93) to 24 bytes, verify size via
     *     lseek(SEEK_END) == 24, then extend via ftruncate (93) to 48 bytes.
     *   - Negative ftruncate (93) with negative length (-1) -> -EINVAL (-22),
     *     and invalid fd (9999) -> -EBADF (-9).
     *   - Positive truncate (92) by path to 12 bytes and then 0 bytes,
     *     verifying lseek(SEEK_END) == 12 and 0.
     *   - Negative truncate (92) with negative length (-1) -> -EINVAL (-22),
     *     and invalid path pointer (0, 0x100) -> -EFAULT (-14).
     */
    static const char trunc_path[] = "/tmp/a32rt_m5_r7_trunc32_test";
    sys1(10U /* unlink */, (uint32_t)(uintptr_t)trunc_path);

    int32_t tfd = sys3(5U /* open */, (uint32_t)(uintptr_t)trunc_path,
                       0x242U /* O_CREAT | O_TRUNC | O_RDWR */, 0644U);
    if (tfd < 0) {
        return 174;
    }

    static char init_data[64];
    for (int i = 0; i < 64; i++) {
        init_data[i] = (char)('A' + (i & 15));
    }
    if (sys3(4U /* write */, (uint32_t)tfd, (uint32_t)(uintptr_t)init_data, 64U) != 64) {
        sys1(6U /* close */, (uint32_t)tfd);
        sys1(10U /* unlink */, (uint32_t)(uintptr_t)trunc_path);
        return 175;
    }

    /* Negative: ftruncate (93) with negative length (-1) -> -EINVAL (-22), bad fd -> -EBADF (-9) */
    if (sys2(93U /* ftruncate */, (uint32_t)tfd, 0xFFFFFFFFU /* -1 */) != -22 /* -EINVAL */ ||
        sys2(93U /* ftruncate */, (uint32_t)tfd, 0x80000000U /* INT32_MIN */) != -22 /* -EINVAL */ ||
        sys2(93U /* ftruncate */, 9999U, 16U) != -9 /* -EBADF */) {
        sys1(6U /* close */, (uint32_t)tfd);
        sys1(10U /* unlink */, (uint32_t)(uintptr_t)trunc_path);
        return 176;
    }

    /* Positive: ftruncate (93) shrink to 24 bytes and extend to 48 bytes */
    if (sys2(93U /* ftruncate */, (uint32_t)tfd, 24U) != 0 ||
        sys3(19U /* lseek */, (uint32_t)tfd, 0U, 2U /* SEEK_END */) != 24) {
        sys1(6U /* close */, (uint32_t)tfd);
        sys1(10U /* unlink */, (uint32_t)(uintptr_t)trunc_path);
        return 177;
    }
    if (sys2(93U /* ftruncate */, (uint32_t)tfd, 48U) != 0 ||
        sys3(19U /* lseek */, (uint32_t)tfd, 0U, 2U /* SEEK_END */) != 48) {
        sys1(6U /* close */, (uint32_t)tfd);
        sys1(10U /* unlink */, (uint32_t)(uintptr_t)trunc_path);
        return 178;
    }

    /* Negative: truncate (92) with negative length (-1) -> -EINVAL (-22), bad path -> -EFAULT (-14) */
    if (sys2(92U /* truncate */, (uint32_t)(uintptr_t)trunc_path, 0xFFFFFFFFU /* -1 */) != -22 /* -EINVAL */ ||
        sys2(92U /* truncate */, (uint32_t)(uintptr_t)trunc_path, 0x80000000U /* INT32_MIN */) != -22 /* -EINVAL */ ||
        sys2(92U /* truncate */, 0U, 10U) != -14 /* -EFAULT */ ||
        sys2(92U /* truncate */, 0x100U, 10U) != -14 /* -EFAULT */) {
        sys1(6U /* close */, (uint32_t)tfd);
        sys1(10U /* unlink */, (uint32_t)(uintptr_t)trunc_path);
        return 179;
    }

    /* Positive: truncate (92) by path to 12 bytes and then 0 bytes */
    if (sys2(92U /* truncate */, (uint32_t)(uintptr_t)trunc_path, 12U) != 0 ||
        sys3(19U /* lseek */, (uint32_t)tfd, 0U, 2U /* SEEK_END */) != 12) {
        sys1(6U /* close */, (uint32_t)tfd);
        sys1(10U /* unlink */, (uint32_t)(uintptr_t)trunc_path);
        return 180;
    }
    if (sys2(92U /* truncate */, (uint32_t)(uintptr_t)trunc_path, 0U) != 0 ||
        sys3(19U /* lseek */, (uint32_t)tfd, 0U, 2U /* SEEK_END */) != 0) {
        sys1(6U /* close */, (uint32_t)tfd);
        sys1(10U /* unlink */, (uint32_t)(uintptr_t)trunc_path);
        return 181;
    }

    sys1(6U /* close */, (uint32_t)tfd);
    sys1(10U /* unlink */, (uint32_t)(uintptr_t)trunc_path);

    /*
     * 20b: sched_getaffinity (242):
     *   - Negative: invalid mask pointer (0, 0x100) -> -EFAULT (-14);
     *     non-existent pid (999999) -> -ESRCH (-3)
     *   - Positive: pid == 0 and pid == gettid() return > 0 with non-zero mask
     */
    static uint64_t cpu_mask[16];
    my_memset(cpu_mask, 0, sizeof(cpu_mask));

    if (sys3(242U /* sched_getaffinity */, 0U, (uint32_t)sizeof(cpu_mask), 0U) != -14 /* -EFAULT */ ||
        sys3(242U /* sched_getaffinity */, 0U, (uint32_t)sizeof(cpu_mask), 0x100U) != -14 /* -EFAULT */) {
        return 182;
    }
    if (sys3(242U /* sched_getaffinity */, 999999U,
             (uint32_t)sizeof(cpu_mask), (uint32_t)(uintptr_t)cpu_mask) != -3 /* -ESRCH */) {
        return 183;
    }

    int32_t aff_rc = sys3(242U /* sched_getaffinity */, 0U,
                          (uint32_t)sizeof(cpu_mask), (uint32_t)(uintptr_t)cpu_mask);
    if (aff_rc <= 0 || cpu_mask[0] == 0ULL) {
        return 184;
    }

    /*
     * 20c: gettimeofday (78) with non-NULL tz pointer:
     *   - Negative: invalid tv pointer (0x100) or invalid tz pointer (0x100) -> -EFAULT (-14)
     *   - Positive: both tv and tz non-NULL -> returns 0, populates tv and tz
     *     without overflowing 8-byte struct timezone into canary words;
     *     also NULL tv with non-NULL tz -> returns 0
     */
    static struct timeval32 gt_tv;
    static struct {
        struct timezone32_m5 tz;
        uint32_t             canary[2];
    } gt_tz_buf;

    gt_tv.tv_sec  = -1;
    gt_tv.tv_usec = -1;
    gt_tz_buf.tz.tz_minuteswest = 0x12345678;
    gt_tz_buf.tz.tz_dsttime     = 0x12345678;
    gt_tz_buf.canary[0]         = 0xDEADBEEFU;
    gt_tz_buf.canary[1]         = 0xCAFEBABEU;

    if (sys2(78U /* gettimeofday */, 0x100U, (uint32_t)(uintptr_t)&gt_tz_buf.tz) != -14 /* -EFAULT */ ||
        sys2(78U /* gettimeofday */, (uint32_t)(uintptr_t)&gt_tv, 0x100U) != -14 /* -EFAULT */) {
        return 185;
    }

    if (sys2(78U /* gettimeofday */,
             (uint32_t)(uintptr_t)&gt_tv,
             (uint32_t)(uintptr_t)&gt_tz_buf.tz) != 0) {
        return 186;
    }
    if (gt_tv.tv_sec <= 0 ||
        gt_tv.tv_usec < 0 ||
        gt_tv.tv_usec >= 1000000 ||
        gt_tz_buf.tz.tz_minuteswest == 0x12345678 ||
        gt_tz_buf.canary[0] != 0xDEADBEEFU ||
        gt_tz_buf.canary[1] != 0xCAFEBABEU) {
        return 187;
    }

    /* Also verify gettimeofday(NULL, &tz) succeeds */
    gt_tz_buf.tz.tz_minuteswest = 0x12345678;
    gt_tz_buf.tz.tz_dsttime     = 0x12345678;
    if (sys2(78U /* gettimeofday */, 0U, (uint32_t)(uintptr_t)&gt_tz_buf.tz) != 0 ||
        gt_tz_buf.tz.tz_minuteswest == 0x12345678 ||
        gt_tz_buf.canary[0] != 0xDEADBEEFU ||
        gt_tz_buf.canary[1] != 0xCAFEBABEU) {
        return 188;
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * Test 21: Round 8 regressions —
 *   21a: setitimer (104) and getitimer (105) positive (ITIMER_REAL arm,
 *        query, in-place aliasing disarm, 16-byte struct canary check) and
 *        negative (-EFAULT on 0x100 pointer, -EINVAL on bad which or
 *        tv_usec < 0 / tv_usec >= 1000000 without clobbering old_value)
 *   21b: nanosleep (162) and clock_nanosleep (265) regression test verifying
 *        that when failing with -EINVAL (e.g. req.tv_nsec = -1 or 1000000000),
 *        the guest rem struct is NOT overwritten with stack garbage
 * -------------------------------------------------------------------------- */
struct itimerval32 {
    struct timeval32 it_interval;
    struct timeval32 it_value;
};

_Static_assert(sizeof(struct itimerval32) == 16U, "itimerval32 must be 16 bytes");

struct itimerval32_canary {
    struct itimerval32 itv;
    uint32_t           canary[2];
};

static int test21_r8_itimer_nanosleep_rem(void)
{
    static struct itimerval32 arm_itv;
    static struct itimerval32 bad_itv;
    static struct itimerval32_canary old_buf;
    static struct itimerval32_canary cur_buf;

    /* 21a-1: Negative checks for getitimer (105) and setitimer (104) */
    if (sys2(105U /* getitimer */, 0U /* ITIMER_REAL */, 0U) != -14 /* -EFAULT */ ||
        sys2(105U /* getitimer */, 0U /* ITIMER_REAL */, 0x100U) != -14 /* -EFAULT */ ||
        sys2(105U /* getitimer */, 99U /* invalid which */,
             (uint32_t)(uintptr_t)&cur_buf.itv) != -22 /* -EINVAL */) {
        return 189;
    }

    arm_itv.it_interval.tv_sec  = 0;
    arm_itv.it_interval.tv_usec = 0;
    arm_itv.it_value.tv_sec     = 0;
    arm_itv.it_value.tv_usec    = 0;

    if (sys3(104U /* setitimer */, 0U /* ITIMER_REAL */, 0x100U, 0U) != -14 /* -EFAULT */ ||
        sys3(104U /* setitimer */, 0U /* ITIMER_REAL */,
             (uint32_t)(uintptr_t)&arm_itv, 0x100U) != -14 /* -EFAULT */ ||
        sys3(104U /* setitimer */, 99U /* invalid which */,
             (uint32_t)(uintptr_t)&arm_itv, 0U) != -22 /* -EINVAL */) {
        return 190;
    }

    /* Negative: tv_usec >= 1000000 or < 0 must return -EINVAL without clobbering old_value */
    old_buf.itv.it_interval.tv_sec  = 0x11112222;
    old_buf.itv.it_interval.tv_usec = 0x33334444;
    old_buf.itv.it_value.tv_sec     = 0x55556666;
    old_buf.itv.it_value.tv_usec    = 0x77778888;
    old_buf.canary[0]               = 0xDEADBEEFU;
    old_buf.canary[1]               = 0xCAFEBABEU;

    bad_itv.it_interval.tv_sec  = 0;
    bad_itv.it_interval.tv_usec = 0;
    bad_itv.it_value.tv_sec     = 1;
    bad_itv.it_value.tv_usec    = 1000000;
    if (sys3(104U /* setitimer */, 0U /* ITIMER_REAL */,
             (uint32_t)(uintptr_t)&bad_itv,
             (uint32_t)(uintptr_t)&old_buf.itv) != -22 /* -EINVAL */) {
        return 191;
    }

    bad_itv.it_value.tv_usec = -1;
    if (sys3(104U /* setitimer */, 0U /* ITIMER_REAL */,
             (uint32_t)(uintptr_t)&bad_itv,
             (uint32_t)(uintptr_t)&old_buf.itv) != -22 /* -EINVAL */) {
        return 192;
    }

    bad_itv.it_value.tv_usec    = 0;
    bad_itv.it_interval.tv_usec = -1;
    if (sys3(104U /* setitimer */, 0U /* ITIMER_REAL */,
             (uint32_t)(uintptr_t)&bad_itv,
             (uint32_t)(uintptr_t)&old_buf.itv) != -22 /* -EINVAL */) {
        return 193;
    }
    if (old_buf.itv.it_interval.tv_sec != 0x11112222 ||
        old_buf.itv.it_interval.tv_usec != 0x33334444 ||
        old_buf.itv.it_value.tv_sec != 0x55556666 ||
        old_buf.itv.it_value.tv_usec != 0x77778888 ||
        old_buf.canary[0] != 0xDEADBEEFU ||
        old_buf.canary[1] != 0xCAFEBABEU) {
        return 194;
    }

    /* 21a-2: Positive arm, query via getitimer, and in-place disarm via setitimer */
    arm_itv.it_interval.tv_sec  = 10;
    arm_itv.it_interval.tv_usec = 250000;
    arm_itv.it_value.tv_sec     = 60;
    arm_itv.it_value.tv_usec    = 500000;

    if (sys3(104U /* setitimer */, 0U /* ITIMER_REAL */,
             (uint32_t)(uintptr_t)&arm_itv,
             (uint32_t)(uintptr_t)&old_buf.itv) != 0 ||
        old_buf.canary[0] != 0xDEADBEEFU ||
        old_buf.canary[1] != 0xCAFEBABEU) {
        return 195;
    }

    cur_buf.itv.it_interval.tv_sec  = -1;
    cur_buf.itv.it_interval.tv_usec = -1;
    cur_buf.itv.it_value.tv_sec     = -1;
    cur_buf.itv.it_value.tv_usec    = -1;
    cur_buf.canary[0]               = 0xDEADBEEFU;
    cur_buf.canary[1]               = 0xCAFEBABEU;

    if (sys2(105U /* getitimer */, 0U /* ITIMER_REAL */,
             (uint32_t)(uintptr_t)&cur_buf.itv) != 0 ||
        cur_buf.itv.it_interval.tv_sec != 10 ||
        cur_buf.itv.it_interval.tv_usec != 250000 ||
        cur_buf.itv.it_value.tv_sec < 50 ||
        cur_buf.itv.it_value.tv_sec > 60 ||
        cur_buf.itv.it_value.tv_usec < 0 ||
        cur_buf.itv.it_value.tv_usec >= 1000000 ||
        cur_buf.canary[0] != 0xDEADBEEFU ||
        cur_buf.canary[1] != 0xCAFEBABEU) {
        /* Ensure timer is disarmed before failing */
        arm_itv.it_interval.tv_sec  = 0;
        arm_itv.it_interval.tv_usec = 0;
        arm_itv.it_value.tv_sec     = 0;
        arm_itv.it_value.tv_usec    = 0;
        sys3(104U /* setitimer */, 0U, (uint32_t)(uintptr_t)&arm_itv, 0U);
        return 196;
    }

    /* Disarm using in-place aliasing (new_value == old_value) */
    old_buf.itv.it_interval.tv_sec  = 0;
    old_buf.itv.it_interval.tv_usec = 0;
    old_buf.itv.it_value.tv_sec     = 0;
    old_buf.itv.it_value.tv_usec    = 0;
    old_buf.canary[0]               = 0xDEADBEEFU;
    old_buf.canary[1]               = 0xCAFEBABEU;

    if (sys3(104U /* setitimer */, 0U /* ITIMER_REAL */,
             (uint32_t)(uintptr_t)&old_buf.itv,
             (uint32_t)(uintptr_t)&old_buf.itv) != 0 ||
        old_buf.itv.it_interval.tv_sec != 10 ||
        old_buf.itv.it_interval.tv_usec != 250000 ||
        old_buf.itv.it_value.tv_sec < 50 ||
        old_buf.itv.it_value.tv_sec > 60 ||
        old_buf.canary[0] != 0xDEADBEEFU ||
        old_buf.canary[1] != 0xCAFEBABEU) {
        arm_itv.it_interval.tv_sec  = 0;
        arm_itv.it_interval.tv_usec = 0;
        arm_itv.it_value.tv_sec     = 0;
        arm_itv.it_value.tv_usec    = 0;
        sys3(104U /* setitimer */, 0U, (uint32_t)(uintptr_t)&arm_itv, 0U);
        return 197;
    }

    /* Verify getitimer now reports disarmed (0, 0) */
    if (sys2(105U /* getitimer */, 0U /* ITIMER_REAL */,
             (uint32_t)(uintptr_t)&cur_buf.itv) != 0 ||
        cur_buf.itv.it_interval.tv_sec != 0 ||
        cur_buf.itv.it_interval.tv_usec != 0 ||
        cur_buf.itv.it_value.tv_sec != 0 ||
        cur_buf.itv.it_value.tv_usec != 0 ||
        cur_buf.canary[0] != 0xDEADBEEFU ||
        cur_buf.canary[1] != 0xCAFEBABEU) {
        return 198;
    }

    /*
     * 21b: Regression test for nanosleep (162) and clock_nanosleep (265)
     *      with invalid req.tv_nsec = -1 (or >= 1000000000, or tv_sec < 0):
     *      verify that when the syscall fails with -EINVAL (-22), the guest
     *      rem struct is NOT overwritten with uninitialized stack garbage!
     */
    static struct timespec32 req_ts;
    static struct {
        struct timespec32 rem;
        uint32_t          canary[2];
    } rem_buf;

    rem_buf.rem.tv_sec  = 0x13572468;
    rem_buf.rem.tv_nsec = 0x24681357;
    rem_buf.canary[0]   = 0xDEADBEEFU;
    rem_buf.canary[1]   = 0xCAFEBABEU;

    /* Invalid req.tv_nsec = -1 */
    req_ts.tv_sec  = 0;
    req_ts.tv_nsec = -1;
    if (sys2(162U /* nanosleep */,
             (uint32_t)(uintptr_t)&req_ts,
             (uint32_t)(uintptr_t)&rem_buf.rem) != -22 /* -EINVAL */ ||
        rem_buf.rem.tv_sec != 0x13572468 ||
        rem_buf.rem.tv_nsec != 0x24681357 ||
        rem_buf.canary[0] != 0xDEADBEEFU ||
        rem_buf.canary[1] != 0xCAFEBABEU) {
        return 199;
    }

    if (sys4(265U /* clock_nanosleep */, 1U /* CLOCK_MONOTONIC */, 0U,
             (uint32_t)(uintptr_t)&req_ts,
             (uint32_t)(uintptr_t)&rem_buf.rem) != -22 /* -EINVAL */ ||
        rem_buf.rem.tv_sec != 0x13572468 ||
        rem_buf.rem.tv_nsec != 0x24681357 ||
        rem_buf.canary[0] != 0xDEADBEEFU ||
        rem_buf.canary[1] != 0xCAFEBABEU) {
        return 200;
    }

    /* Invalid req.tv_nsec = 1000000000 and req.tv_sec = -1 */
    req_ts.tv_sec  = 0;
    req_ts.tv_nsec = 1000000000;
    if (sys2(162U /* nanosleep */,
             (uint32_t)(uintptr_t)&req_ts,
             (uint32_t)(uintptr_t)&rem_buf.rem) != -22 /* -EINVAL */ ||
        sys4(265U /* clock_nanosleep */, 1U /* CLOCK_MONOTONIC */, 0U,
             (uint32_t)(uintptr_t)&req_ts,
             (uint32_t)(uintptr_t)&rem_buf.rem) != -22 /* -EINVAL */ ||
        rem_buf.rem.tv_sec != 0x13572468 ||
        rem_buf.rem.tv_nsec != 0x24681357) {
        return 201;
    }

    req_ts.tv_sec  = -1;
    req_ts.tv_nsec = 0;
    if (sys2(162U /* nanosleep */,
             (uint32_t)(uintptr_t)&req_ts,
             (uint32_t)(uintptr_t)&rem_buf.rem) != -22 /* -EINVAL */ ||
        sys4(265U /* clock_nanosleep */, 1U /* CLOCK_MONOTONIC */, 0U,
             (uint32_t)(uintptr_t)&req_ts,
             (uint32_t)(uintptr_t)&rem_buf.rem) != -22 /* -EINVAL */ ||
        rem_buf.rem.tv_sec != 0x13572468 ||
        rem_buf.rem.tv_nsec != 0x24681357) {
        return 202;
    }

    /* Positive short sleep (1 us) for nanosleep and clock_nanosleep */
    req_ts.tv_sec  = 0;
    req_ts.tv_nsec = 1000;
    if (sys2(162U /* nanosleep */,
             (uint32_t)(uintptr_t)&req_ts,
             (uint32_t)(uintptr_t)&rem_buf.rem) != 0 ||
        sys4(265U /* clock_nanosleep */, 1U /* CLOCK_MONOTONIC */, 0U,
             (uint32_t)(uintptr_t)&req_ts,
             (uint32_t)(uintptr_t)&rem_buf.rem) != 0 ||
        rem_buf.canary[0] != 0xDEADBEEFU ||
        rem_buf.canary[1] != 0xCAFEBABEU) {
        return 203;
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * Test 22: Round 9 regressions —
 *   22a: mmap2 (192) with mutually exclusive MAP_SHARED | MAP_PRIVATE (0x23U)
 *        returning -EINVAL (-22)
 *   22b: open (5) and stat64 (195) with a 4096-character path (>= PATH_MAX)
 *        returning -ENAMETOOLONG (-36) while invalid pointer 0x100U still
 *        returns -EFAULT (-14)
 *   22c: epoll_wait (252) with maxevents = 0 and events = NULL returning
 *        -EINVAL (-22, NOT -EFAULT), ppoll (336) and pselect6 (335) with
 *        invalid tv_nsec = -1 returning -EINVAL (-22), and ppoll / pselect6
 *        with a static const struct timespec32 in .rodata succeeding (0)
 *        without crashing on timeout writeback
 * -------------------------------------------------------------------------- */
static const struct timespec32 g_r9_rodata_zero_ts    = { 0, 0 };
static const struct timespec32 g_r9_rodata_subusec_ts = { 0, 1000 };

static int test22_r9_mmap_flags_enametoolong_epoll_pwait(void)
{
    static char long_path[4100];
    static _Alignas(8) uint8_t st64_buf[104];
    static struct timespec32 bad_ts;
    static struct pollfd dummy_pfd;

    /*
     * 22a: Negative mmap2 (192) with mutually exclusive MAP_SHARED | MAP_PRIVATE
     *      (0x23U = MAP_SHARED | MAP_PRIVATE | MAP_ANONYMOUS) -> -EINVAL (-22),
     *      and neither MAP_SHARED nor MAP_PRIVATE (0x20U = MAP_ANONYMOUS) -> -EINVAL (-22).
     */
    int32_t mm_both = sys6(192U /* mmap2 */, 0U, 4096U,
                           3U /* PROT_READ|PROT_WRITE */,
                           0x23U /* MAP_SHARED | MAP_PRIVATE | MAP_ANONYMOUS */,
                           0xFFFFFFFFU, 0U);
    if (mm_both != -22 /* -EINVAL */) {
        if ((uint32_t)mm_both <= 0xFFFFF000U) {
            sys2(91U /* munmap */, (uint32_t)mm_both, 4096U);
        }
        return 204;
    }

    int32_t mm_neither = sys6(192U /* mmap2 */, 0U, 4096U,
                              3U /* PROT_READ|PROT_WRITE */,
                              0x20U /* MAP_ANONYMOUS without SHARED/PRIVATE */,
                              0xFFFFFFFFU, 0U);
    if (mm_neither != -22 /* -EINVAL */) {
        if ((uint32_t)mm_neither <= 0xFFFFF000U) {
            sys2(91U /* munmap */, (uint32_t)mm_neither, 4096U);
        }
        return 205;
    }

    /*
     * 22b: Negative open (5) and stat64 (195) with a 4096-character path
     *      (>= PATH_MAX) -> -ENAMETOOLONG (-36), while an invalid pointer
     *      (0x100U) still returns -EFAULT (-14).
     */
    for (uint32_t i = 0U; i < 4096U; i++) {
        long_path[i] = ((i & 63U) == 0U) ? '/' : 'a';
    }
    long_path[4096] = '\0';
    my_memset(st64_buf, 0, sizeof(st64_buf));

    if (sys3(5U /* open */, (uint32_t)(uintptr_t)long_path, 0U /* O_RDONLY */, 0U) != -36 /* -ENAMETOOLONG */ ||
        sys4(322U /* openat */, 0xFFFFFF9CU /* AT_FDCWD */,
             (uint32_t)(uintptr_t)long_path, 0U /* O_RDONLY */, 0U) != -36 /* -ENAMETOOLONG */ ||
        sys2(195U /* stat64 */, (uint32_t)(uintptr_t)long_path,
             (uint32_t)(uintptr_t)st64_buf) != -36 /* -ENAMETOOLONG */) {
        return 206;
    }

    if (sys3(5U /* open */, 0x100U, 0U /* O_RDONLY */, 0U) != -14 /* -EFAULT */ ||
        sys2(195U /* stat64 */, 0x100U, (uint32_t)(uintptr_t)st64_buf) != -14 /* -EFAULT */) {
        return 207;
    }

    /*
     * 22c-1: Negative epoll_wait (252) with maxevents = 0 (and maxevents = -1)
     *        and events = NULL (0U) must return -EINVAL (-22), NOT -EFAULT (-14).
     */
    int32_t epfd = sys1(357U /* epoll_create1 */, 0U);
    if (epfd < 0) {
        return 208;
    }
    int32_t ep_rc0 = sys4(252U /* epoll_wait */, (uint32_t)epfd,
                          0U /* events = NULL */, 0U /* maxevents = 0 */, 0U);
    int32_t ep_rc_neg = sys4(252U /* epoll_wait */, (uint32_t)epfd,
                             0U /* events = NULL */, 0xFFFFFFFFU /* maxevents = -1 */, 0U);
    sys1(6U /* close */, (uint32_t)epfd);
    if (ep_rc0 != -22 /* -EINVAL */ || ep_rc_neg != -22 /* -EINVAL */) {
        return 209;
    }

    /*
     * 22c-2: Negative ppoll (336) and pselect6 (335) with invalid tv_nsec = -1
     *        (as well as tv_nsec = 1000000000 and tv_sec = -1) -> -EINVAL (-22).
     */
    bad_ts.tv_sec  = 0;
    bad_ts.tv_nsec = -1;
    if (sys5(336U /* ppoll */, 0U, 0U, (uint32_t)(uintptr_t)&bad_ts, 0U, 8U) != -22 /* -EINVAL */ ||
        sys6(335U /* pselect6 */, 0U, 0U, 0U, 0U, (uint32_t)(uintptr_t)&bad_ts, 0U) != -22 /* -EINVAL */) {
        return 210;
    }

    bad_ts.tv_sec  = 0;
    bad_ts.tv_nsec = 1000000000;
    if (sys5(336U /* ppoll */, 0U, 0U, (uint32_t)(uintptr_t)&bad_ts, 0U, 8U) != -22 /* -EINVAL */ ||
        sys6(335U /* pselect6 */, 0U, 0U, 0U, 0U, (uint32_t)(uintptr_t)&bad_ts, 0U) != -22 /* -EINVAL */) {
        return 211;
    }

    bad_ts.tv_sec  = -1;
    bad_ts.tv_nsec = 0;
    if (sys5(336U /* ppoll */, 0U, 0U, (uint32_t)(uintptr_t)&bad_ts, 0U, 8U) != -22 /* -EINVAL */ ||
        sys6(335U /* pselect6 */, 0U, 0U, 0U, 0U, (uint32_t)(uintptr_t)&bad_ts, 0U) != -22 /* -EINVAL */) {
        return 212;
    }

    /*
     * 22c-3: Positive ppoll (336) and pselect6 (335) with a static const
     *        struct timespec32 in .rodata (read-only memory) must succeed (0)
     *        without crashing on timeout writeback!
     */
    dummy_pfd.fd      = -1;
    dummy_pfd.events  = 0;
    dummy_pfd.revents = 0;

    if (sys5(336U /* ppoll */, (uint32_t)(uintptr_t)&dummy_pfd, 1U,
             (uint32_t)(uintptr_t)&g_r9_rodata_zero_ts, 0U, 8U) != 0 ||
        sys5(336U /* ppoll */, (uint32_t)(uintptr_t)&dummy_pfd, 1U,
             (uint32_t)(uintptr_t)&g_r9_rodata_subusec_ts, 0U, 8U) != 0) {
        return 213;
    }

    if (sys6(335U /* pselect6 */, 0U, 0U, 0U, 0U,
             (uint32_t)(uintptr_t)&g_r9_rodata_zero_ts, 0U) != 0 ||
        sys6(335U /* pselect6 */, 0U, 0U, 0U, 0U,
             (uint32_t)(uintptr_t)&g_r9_rodata_subusec_ts, 0U) != 0) {
        return 214;
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * Test 23: Round 10 regressions —
 *   23a: Negative LTP EFAULT (-14) regression tests for high pointer
 *        0xFFFFFFFFU ((void *)-1) and unmapped mid-window pointer (mmap2 +
 *        munmap) across open (5), openat (322), rt_sigaction (174),
 *        rt_sigprocmask (175), sigaltstack (186), uname (122), pipe (42),
 *        and stat64 (195)
 *   23b: Positive regression test for setresuid (164), setresgid (170),
 *        setresuid32 (208), and setresgid32 (210)
 *        with (-1, -1, -1) succeeding (0) even after registering a custom
 *        handler on signal 34 (SIGRTMIN)
 * -------------------------------------------------------------------------- */
struct sigaction32_m5 {
    uint32_t sa_handler;
    uint32_t sa_flags;
    uint32_t sa_restorer;
    uint32_t sa_mask[2];
};

static volatile uint32_t g_m5_r10_rt34_hits = 0U;

static void m5_r10_rt34_handler(int sig, void *info, void *ucontext)
{
    (void)sig;
    (void)info;
    (void)ucontext;
    g_m5_r10_rt34_hits++;
}

static int test23_r10_efault_high_unmapped_and_setresuid_rt34(void)
{
    static _Alignas(8) uint8_t st64_buf[104];
    static struct sigaction32_m5 sa34;
    static struct sigaction32_m5 orig_sa34;
    const uint32_t high_ptr = 0xFFFFFFFFU;

    /* Allocate a mid-window page via mmap2 and unmap it to get an unmapped VA */
    uint32_t unmapped_ptr = (uint32_t)sys6(192U /* mmap2 */, 0U, 4096U,
                                           3U /* PROT_READ | PROT_WRITE */,
                                           0x22U /* MAP_PRIVATE | MAP_ANONYMOUS */,
                                           0xFFFFFFFFU, 0U);
    if (unmapped_ptr > 0xFFFFF000U) {
        return 215;
    }
    if (sys2(91U /* munmap */, unmapped_ptr, 4096U) != 0) {
        return 216;
    }

    my_memset(st64_buf, 0, sizeof(st64_buf));

    /* 1. open (5) with 0xFFFFFFFFU and unmapped_ptr -> -EFAULT (-14) */
    if (sys3(5U /* open */, high_ptr, 0U /* O_RDONLY */, 0U) != -14 /* -EFAULT */ ||
        sys3(5U /* open */, unmapped_ptr, 0U /* O_RDONLY */, 0U) != -14 /* -EFAULT */) {
        return 217;
    }

    /* 2. openat (322) with 0xFFFFFFFFU and unmapped_ptr -> -EFAULT (-14) */
    if (sys4(322U /* openat */, 0xFFFFFF9CU /* AT_FDCWD */, high_ptr, 0U, 0U) != -14 /* -EFAULT */ ||
        sys4(322U /* openat */, 0xFFFFFF9CU /* AT_FDCWD */, unmapped_ptr, 0U, 0U) != -14 /* -EFAULT */) {
        return 218;
    }

    /* 3. rt_sigaction (174) with 0xFFFFFFFFU and unmapped_ptr -> -EFAULT (-14) */
    if (sys4(174U /* rt_sigaction */, 10U /* SIGUSR1 */, high_ptr, 0U, 8U) != -14 /* -EFAULT */ ||
        sys4(174U /* rt_sigaction */, 10U /* SIGUSR1 */, 0U, high_ptr, 8U) != -14 /* -EFAULT */ ||
        sys4(174U /* rt_sigaction */, 10U /* SIGUSR1 */, unmapped_ptr, 0U, 8U) != -14 /* -EFAULT */ ||
        sys4(174U /* rt_sigaction */, 10U /* SIGUSR1 */, 0U, unmapped_ptr, 8U) != -14 /* -EFAULT */) {
        return 219;
    }

    /* 4. rt_sigprocmask (175) with 0xFFFFFFFFU and unmapped_ptr -> -EFAULT (-14) */
    if (sys4(175U /* rt_sigprocmask */, 0U /* SIG_BLOCK */, high_ptr, 0U, 8U) != -14 /* -EFAULT */ ||
        sys4(175U /* rt_sigprocmask */, 0U /* SIG_BLOCK */, 0U, high_ptr, 8U) != -14 /* -EFAULT */ ||
        sys4(175U /* rt_sigprocmask */, 0U /* SIG_BLOCK */, unmapped_ptr, 0U, 8U) != -14 /* -EFAULT */ ||
        sys4(175U /* rt_sigprocmask */, 0U /* SIG_BLOCK */, 0U, unmapped_ptr, 8U) != -14 /* -EFAULT */) {
        return 220;
    }

    /* 5. sigaltstack (186) with 0xFFFFFFFFU and unmapped_ptr -> -EFAULT (-14) */
    if (sys2(186U /* sigaltstack */, high_ptr, 0U) != -14 /* -EFAULT */ ||
        sys2(186U /* sigaltstack */, 0U, high_ptr) != -14 /* -EFAULT */ ||
        sys2(186U /* sigaltstack */, unmapped_ptr, 0U) != -14 /* -EFAULT */ ||
        sys2(186U /* sigaltstack */, 0U, unmapped_ptr) != -14 /* -EFAULT */) {
        return 221;
    }

    /* 6. uname (122) with 0xFFFFFFFFU and unmapped_ptr -> -EFAULT (-14) */
    if (sys1(122U /* uname */, high_ptr) != -14 /* -EFAULT */ ||
        sys1(122U /* uname */, unmapped_ptr) != -14 /* -EFAULT */) {
        return 222;
    }

    /* 7. pipe (42) with 0xFFFFFFFFU and unmapped_ptr -> -EFAULT (-14) */
    if (sys1(42U /* pipe */, high_ptr) != -14 /* -EFAULT */ ||
        sys1(42U /* pipe */, unmapped_ptr) != -14 /* -EFAULT */) {
        return 223;
    }

    /* 8. stat64 (195) with 0xFFFFFFFFU and unmapped_ptr -> -EFAULT (-14) */
    if (sys2(195U /* stat64 */, high_ptr, (uint32_t)(uintptr_t)st64_buf) != -14 /* -EFAULT */ ||
        sys2(195U /* stat64 */, unmapped_ptr, (uint32_t)(uintptr_t)st64_buf) != -14 /* -EFAULT */ ||
        sys2(195U /* stat64 */, (uint32_t)(uintptr_t)"/", high_ptr) != -14 /* -EFAULT */ ||
        sys2(195U /* stat64 */, (uint32_t)(uintptr_t)"/", unmapped_ptr) != -14 /* -EFAULT */) {
        return 224;
    }

    /*
     * 23b: Positive regression test for setresuid (164), setresgid (170),
     *      setresuid32 (208), and setresgid32 (210)
     *      with (-1, -1, -1) succeeding (0) even after registering a handler on
     *      signal 34 (SIGRTMIN).
     */
    g_m5_r10_rt34_hits = 0U;
    sa34.sa_handler  = (uint32_t)(uintptr_t)m5_r10_rt34_handler;
    sa34.sa_flags    = 0x00000004U; /* SA_SIGINFO */
    sa34.sa_restorer = 0U;
    sa34.sa_mask[0]  = 0U;
    sa34.sa_mask[1]  = 0U;

    if (sys4(174U /* rt_sigaction */, 34U /* SIGRTMIN */,
             (uint32_t)(uintptr_t)&sa34,
             (uint32_t)(uintptr_t)&orig_sa34, 8U) != 0) {
        return 225;
    }

    int32_t sru16_rc = sys3(164U /* setresuid */,
                            0xFFFFU /* -1 */,
                            0xFFFFU /* -1 */,
                            0xFFFFU /* -1 */);
    int32_t srg16_rc = sys3(170U /* setresgid */,
                            0xFFFFU /* -1 */,
                            0xFFFFU /* -1 */,
                            0xFFFFU /* -1 */);
    int32_t sru32_rc = sys3(208U /* setresuid32 */,
                            0xFFFFFFFFU /* -1 */,
                            0xFFFFFFFFU /* -1 */,
                            0xFFFFFFFFU /* -1 */);
    int32_t srg32_rc = sys3(210U /* setresgid32 */,
                            0xFFFFFFFFU /* -1 */,
                            0xFFFFFFFFU /* -1 */,
                            0xFFFFFFFFU /* -1 */);

    sys4(174U /* rt_sigaction */, 34U /* SIGRTMIN */,
         (uint32_t)(uintptr_t)&orig_sa34, 0U, 8U);

    if (sru16_rc != 0 || srg16_rc != 0 ||
        sru32_rc != 0 || srg32_rc != 0 ||
        g_m5_r10_rt34_hits != 0U) {
        return 226;
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * Test 24: Extended ioctl (SIOCGIF*, SIOCGIFCONF, BLK*) & AT_HWCAP2 in auxv
 * -------------------------------------------------------------------------- */
struct test_ifreq32 {
    char ifr_name[16];
    union {
        int32_t  ivalue;
        int16_t  flags;
        uint8_t  raw[16];
    } ifr_u;
};

struct test_ifconf32 {
    int32_t  ifc_len;
    uint32_t ifc_req;
};

static int test24_ioctl_siocgif_and_auxv_hwcap2(void)
{
    int32_t sock = sys3(281U /* socket */, 2U /* AF_INET */, 2U /* SOCK_DGRAM */, 0U);
    if (sock < 0) {
        return 230;
    }

    /* 1. SIOCGIFINDEX (0x8933) on "lo" */
    struct test_ifreq32 ifr;
    for (uint32_t i = 0U; i < 16U; i++) {
        ifr.ifr_name[i] = '\0';
        ifr.ifr_u.raw[i] = 0U;
    }
    ifr.ifr_name[0] = 'l';
    ifr.ifr_name[1] = 'o';

    int32_t rc = sys3(54U /* ioctl */, (uint32_t)sock, 0x8933U /* SIOCGIFINDEX */,
                      (uint32_t)(uintptr_t)&ifr);
    if (rc != -25 /* -ENOTTY on lightweight hypervisors without netif ioctls */) {
        if (rc != 0 || ifr.ifr_u.ivalue < 1) {
            sys1(6U /* close */, (uint32_t)sock);
            return 231;
        }
        int32_t lo_index = ifr.ifr_u.ivalue;

        /* 2. SIOCGIFNAME (0x8910) from lo_index */
        for (uint32_t i = 0U; i < 16U; i++) {
            ifr.ifr_name[i] = '\0';
            ifr.ifr_u.raw[i] = 0U;
        }
        ifr.ifr_u.ivalue = lo_index;
        rc = sys3(54U /* ioctl */, (uint32_t)sock, 0x8910U /* SIOCGIFNAME */,
                  (uint32_t)(uintptr_t)&ifr);
        if (rc != 0 || ifr.ifr_name[0] != 'l' || ifr.ifr_name[1] != 'o' ||
            ifr.ifr_name[2] != '\0') {
            sys1(6U /* close */, (uint32_t)sock);
            return 232;
        }

        /* 3. SIOCGIFFLAGS (0x8913) on "lo" (IFF_LOOPBACK == 0x8) */
        for (uint32_t i = 0U; i < 16U; i++) {
            ifr.ifr_u.raw[i] = 0U;
        }
        rc = sys3(54U /* ioctl */, (uint32_t)sock, 0x8913U /* SIOCGIFFLAGS */,
                  (uint32_t)(uintptr_t)&ifr);
        if (rc != 0 || ((uint16_t)ifr.ifr_u.flags & 0x8U) == 0U) {
            sys1(6U /* close */, (uint32_t)sock);
            return 233;
        }

        /* 4. SIOCGIFCONF (0x8912) with 32-byte arm_ifreq_t entries */
        struct test_ifreq32 reqs[8];
        for (uint32_t k = 0U; k < 8U; k++) {
            for (uint32_t i = 0U; i < 16U; i++) {
                reqs[k].ifr_name[i] = '\0';
                reqs[k].ifr_u.raw[i] = 0U;
            }
        }
        struct test_ifconf32 ifc;
        ifc.ifc_len = (int32_t)sizeof(reqs);
        ifc.ifc_req = (uint32_t)(uintptr_t)reqs;
        rc = sys3(54U /* ioctl */, (uint32_t)sock, 0x8912U /* SIOCGIFCONF */,
                  (uint32_t)(uintptr_t)&ifc);
        if (rc != 0 || ifc.ifc_len < 0 || ((uint32_t)ifc.ifc_len % 32U) != 0U) {
            sys1(6U /* close */, (uint32_t)sock);
            return 234;
        }
        if (ifc.ifc_len >= 32 && reqs[0].ifr_name[0] == '\0') {
            sys1(6U /* close */, (uint32_t)sock);
            return 235;
        }
    }

    /* 5. Negative EFAULT checks for ioctl device families */
    int32_t ef1 = sys3(54U /* ioctl */, (uint32_t)sock, 0x8933U /* SIOCGIFINDEX */, 0U);
    int32_t ef2 = sys3(54U /* ioctl */, (uint32_t)sock, 0x8912U /* SIOCGIFCONF */, 0U);
    int32_t ef3 = sys3(54U /* ioctl */, (uint32_t)sock, 0x1260U /* BLKGETSIZE */, 0U);
    int32_t ef4 = sys3(54U /* ioctl */, (uint32_t)sock, 0x80041272U /* BLKGETSIZE64 */, 0U);
    sys1(6U /* close */, (uint32_t)sock);
    if (ef1 != -14 || ef2 != -14 || ef3 != -14 || ef4 != -14) {
        return 236;
    }

    /* 6. Verify AT_HWCAP2 (tag 26) == 0x1f in /proc/self/auxv */
    static const char auxv_path[] = "/proc/self/auxv";
    int32_t afd = sys4(322U /* openat */, 0xFFFFFF9CU /* AT_FDCWD */,
                       (uint32_t)(uintptr_t)auxv_path, 0U /* O_RDONLY */, 0U);
    if (afd < 0) {
        return 237;
    }
    uint32_t auxv_buf[64];
    int32_t nread = sys3(3U /* read */, (uint32_t)afd,
                         (uint32_t)(uintptr_t)auxv_buf, (uint32_t)sizeof(auxv_buf));
    sys1(6U /* close */, (uint32_t)afd);
    if (nread < 16 || ((uint32_t)nread % 8U) != 0U) {
        return 238;
    }
    uint32_t hwcap2_val = 0U;
    uint32_t found_hwcap2 = 0U;
    uint32_t count = (uint32_t)nread / 8U;
    for (uint32_t i = 0U; i < count; i++) {
        uint32_t tag = auxv_buf[i * 2U];
        uint32_t val = auxv_buf[i * 2U + 1U];
        if (tag == 0U /* AT_NULL */) {
            break;
        }
        if (tag == 26U /* AT_HWCAP2 */) {
            hwcap2_val = val;
            found_hwcap2 = 1U;
        }
    }
    if (found_hwcap2 != 1U || (hwcap2_val & 0x1FU) != 0x1FU) {
        return 239;
    }

    return 0;
}


int run_test_m5_part3(void)
{
    int rc = test20_r7_truncate_affinity_gettimeofday();
    if (rc != 0) return rc;
    rc = test21_r8_itimer_nanosleep_rem();
    if (rc != 0) return rc;
    rc = test22_r9_mmap_flags_enametoolong_epoll_pwait();
    if (rc != 0) return rc;
    rc = test23_r10_efault_high_unmapped_and_setresuid_rt34();
    if (rc != 0) return rc;
    return test24_ioctl_siocgif_and_auxv_hwcap2();
}
