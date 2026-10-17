/*
 * tests/test_m5_ltp_gdb.c — M5 / M5-hl LTP compatibility & conformance test
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stddef.h>

struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

struct iovec32 {
    uint32_t iov_base;
    uint32_t iov_len;
};

struct msghdr32 {
    uint32_t msg_name;
    uint32_t msg_namelen;
    uint32_t msg_iov;
    uint32_t msg_iovlen;
    uint32_t msg_control;
    uint32_t msg_controllen;
    uint32_t msg_flags;
};

struct cmsghdr32 {
    uint32_t cmsg_len;
    int32_t  cmsg_level;
    int32_t  cmsg_type;
};

struct epoll_event {
    uint32_t events;
    uint64_t data;
};

struct pollfd {
    int32_t fd;
    int16_t events;
    int16_t revents;
};

struct __attribute__((packed, aligned(4))) statfs64 {
    uint32_t f_type;
    uint32_t f_bsize;
    uint64_t f_blocks;
    uint64_t f_bfree;
    uint64_t f_bavail;
    uint64_t f_files;
    uint64_t f_ffree;
    uint32_t f_fsid[2];
    uint32_t f_namelen;
    uint32_t f_frsize;
    uint32_t f_flags;
    uint32_t f_spare[4];
};

_Static_assert(sizeof(struct statfs64) == 84U, "ARM EABI statfs64 must be 84 bytes");

struct statfs64_canary {
    struct statfs64 stfs;
    uint32_t        canary;
};

_Static_assert(offsetof(struct statfs64_canary, canary) == 84U,
               "statfs64 canary must be at byte offset 84");

struct timespec32 {
    int32_t tv_sec;
    int32_t tv_nsec;
};

struct timeval32 {
    int32_t tv_sec;
    int32_t tv_usec;
};

struct itimerspec32 {
    struct timespec32 it_interval;
    struct timespec32 it_value;
};

_Static_assert(sizeof(struct timespec32) == 8U, "timespec32 must be 8 bytes");
_Static_assert(sizeof(struct timeval32) == 8U, "timeval32 must be 8 bytes");
_Static_assert(sizeof(struct itimerspec32) == 16U, "itimerspec32 must be 16 bytes");

struct itimerspec32_canary {
    struct itimerspec32 its;
    uint32_t            canary[4];
};

struct statx_timestamp {
    int64_t  tv_sec;
    uint32_t tv_nsec;
    int32_t  __statx_pad1;
};

struct statx {
    uint32_t stx_mask;
    uint32_t stx_blksize;
    uint64_t stx_attributes;
    uint32_t stx_nlink;
    uint32_t stx_uid;
    uint32_t stx_gid;
    uint16_t stx_mode;
    uint16_t __statx_pad1[1];
    uint64_t stx_ino;
    uint64_t stx_size;
    uint64_t stx_blocks;
    uint64_t stx_attributes_mask;
    struct statx_timestamp stx_atime;
    struct statx_timestamp stx_btime;
    struct statx_timestamp stx_ctime;
    struct statx_timestamp stx_mtime;
    uint32_t stx_rdev_major;
    uint32_t stx_rdev_minor;
    uint32_t stx_dev_major;
    uint32_t stx_dev_minor;
    uint64_t __statx_pad2[14];
};

__asm__(
    ".text\n"
    ".globl __aeabi_uidiv\n"
    ".type __aeabi_uidiv, %function\n"
    "__aeabi_uidiv:\n"
    "    udiv r0, r0, r1\n"
    "    bx lr\n"
    ".globl __aeabi_idiv\n"
    ".type __aeabi_idiv, %function\n"
    "__aeabi_idiv:\n"
    "    sdiv r0, r0, r1\n"
    "    bx lr\n"
);

static inline int32_t sys1(uint32_t nr, uint32_t a0)
{
    register uint32_t r7 __asm__("r7") = nr;
    register uint32_t r0 __asm__("r0") = a0;
    __asm__ volatile("svc #0" : "+r"(r0) : "r"(r7) : "memory", "cc");
    return (int32_t)r0;
}

static inline int32_t sys2(uint32_t nr, uint32_t a0, uint32_t a1)
{
    register uint32_t r7 __asm__("r7") = nr;
    register uint32_t r0 __asm__("r0") = a0;
    register uint32_t r1 __asm__("r1") = a1;
    __asm__ volatile("svc #0" : "+r"(r0) : "r"(r7), "r"(r1) : "memory", "cc");
    return (int32_t)r0;
}

static inline int32_t sys3(uint32_t nr, uint32_t a0, uint32_t a1, uint32_t a2)
{
    register uint32_t r7 __asm__("r7") = nr;
    register uint32_t r0 __asm__("r0") = a0;
    register uint32_t r1 __asm__("r1") = a1;
    register uint32_t r2 __asm__("r2") = a2;
    __asm__ volatile("svc #0"
                     : "+r"(r0)
                     : "r"(r7), "r"(r1), "r"(r2)
                     : "memory", "cc");
    return (int32_t)r0;
}

static inline int32_t sys4(uint32_t nr, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
    register uint32_t r7 __asm__("r7") = nr;
    register uint32_t r0 __asm__("r0") = a0;
    register uint32_t r1 __asm__("r1") = a1;
    register uint32_t r2 __asm__("r2") = a2;
    register uint32_t r3 __asm__("r3") = a3;
    __asm__ volatile("svc #0"
                     : "+r"(r0)
                     : "r"(r7), "r"(r1), "r"(r2), "r"(r3)
                     : "memory", "cc");
    return (int32_t)r0;
}

static inline int32_t sys5(uint32_t nr, uint32_t a0, uint32_t a1, uint32_t a2,
                           uint32_t a3, uint32_t a4)
{
    register uint32_t r7 __asm__("r7") = nr;
    register uint32_t r0 __asm__("r0") = a0;
    register uint32_t r1 __asm__("r1") = a1;
    register uint32_t r2 __asm__("r2") = a2;
    register uint32_t r3 __asm__("r3") = a3;
    register uint32_t r4 __asm__("r4") = a4;
    __asm__ volatile("svc #0"
                     : "+r"(r0)
                     : "r"(r7), "r"(r1), "r"(r2), "r"(r3), "r"(r4)
                     : "memory", "cc");
    return (int32_t)r0;
}

static inline int32_t sys6(uint32_t nr, uint32_t a0, uint32_t a1, uint32_t a2,
                           uint32_t a3, uint32_t a4, uint32_t a5)
{
    register uint32_t r7 __asm__("r7") = nr;
    register uint32_t r0 __asm__("r0") = a0;
    register uint32_t r1 __asm__("r1") = a1;
    register uint32_t r2 __asm__("r2") = a2;
    register uint32_t r3 __asm__("r3") = a3;
    register uint32_t r4 __asm__("r4") = a4;
    register uint32_t r5 __asm__("r5") = a5;
    __asm__ volatile("svc #0"
                     : "+r"(r0)
                     : "r"(r7), "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5)
                     : "memory", "cc");
    return (int32_t)r0;
}

static void my_memset(void *dst, int c, size_t n)
{
    uint8_t *p = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) {
        p[i] = (uint8_t)c;
    }
}

static int my_memcmp(const void *s1, const void *s2, size_t n)
{
    const uint8_t *p1 = (const uint8_t *)s1;
    const uint8_t *p2 = (const uint8_t *)s2;
    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i]) {
            return (int)p1[i] - (int)p2[i];
        }
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Test 1: uname (122)
 * -------------------------------------------------------------------------- */
static int test1_uname(void)
{
    struct utsname u;
    my_memset(&u, 0, sizeof(u));
    if (sys1(122U /* uname */, (uint32_t)(uintptr_t)&u) != 0) {
        return 10;
    }
    if (u.machine[0] != 'a' || u.machine[1] != 'r' ||
        u.machine[2] != 'm' || u.machine[3] != 'v') {
        return 11;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Test 2: socketpair (288) + send/recv (289/291) + sendmsg/recvmsg (296/297) SCM_RIGHTS
 * -------------------------------------------------------------------------- */
static int test2_sockets(void)
{
    int sv[2];
    sv[0] = -1;
    sv[1] = -1;
    if (sys4(288U /* socketpair */, 1U /* AF_UNIX */, 1U /* SOCK_STREAM */, 0U,
             (uint32_t)(uintptr_t)sv) != 0) {
        return 20;
    }

    /* Basic send / recv */
    static const char ping[] = "ping";
    if (sys4(289U /* send */, (uint32_t)sv[0], (uint32_t)(uintptr_t)ping, 4U, 0U) != 4) {
        return 21;
    }
    char pong[8];
    my_memset(pong, 0, sizeof(pong));
    if (sys4(291U /* recv */, (uint32_t)sv[1], (uint32_t)(uintptr_t)pong, sizeof(pong), 0U) != 4) {
        return 22;
    }
    if (my_memcmp(pong, "ping", 4U) != 0) {
        return 23;
    }

    /* Pass pipe fd across socketpair with SCM_RIGHTS */
    int pfd[2];
    if (sys2(359U /* pipe2 */, (uint32_t)(uintptr_t)pfd, 0U) != 0) {
        return 24;
    }
    static const char payload[] = "scm-rights-ok";
    if (sys3(4U /* write */, (uint32_t)pfd[1], (uint32_t)(uintptr_t)payload, 13U) != 13) {
        return 25;
    }

    char send_cbuf[16];
    my_memset(send_cbuf, 0, sizeof(send_cbuf));
    struct cmsghdr32 *cm = (struct cmsghdr32 *)(void *)send_cbuf;
    cm->cmsg_len   = 16U;
    cm->cmsg_level = 1; /* SOL_SOCKET */
    cm->cmsg_type  = 1; /* SCM_RIGHTS */
    *(int32_t *)(void *)(send_cbuf + 12) = pfd[0];

    char dummy = 'x';
    struct iovec32 s_iov;
    s_iov.iov_base = (uint32_t)(uintptr_t)&dummy;
    s_iov.iov_len  = 1U;

    struct msghdr32 s_msg;
    my_memset(&s_msg, 0, sizeof(s_msg));
    s_msg.msg_iov        = (uint32_t)(uintptr_t)&s_iov;
    s_msg.msg_iovlen     = 1U;
    s_msg.msg_control    = (uint32_t)(uintptr_t)send_cbuf;
    s_msg.msg_controllen = 16U;

    if (sys3(296U /* sendmsg */, (uint32_t)sv[0], (uint32_t)(uintptr_t)&s_msg, 0U) != 1) {
        return 26;
    }

    char recv_cbuf[32];
    my_memset(recv_cbuf, 0, sizeof(recv_cbuf));
    char r_dummy = 0;
    struct iovec32 r_iov;
    r_iov.iov_base = (uint32_t)(uintptr_t)&r_dummy;
    r_iov.iov_len  = 1U;

    struct msghdr32 r_msg;
    my_memset(&r_msg, 0, sizeof(r_msg));
    r_msg.msg_iov        = (uint32_t)(uintptr_t)&r_iov;
    r_msg.msg_iovlen     = 1U;
    r_msg.msg_control    = (uint32_t)(uintptr_t)recv_cbuf;
    r_msg.msg_controllen = (uint32_t)sizeof(recv_cbuf);

    if (sys3(297U /* recvmsg */, (uint32_t)sv[1], (uint32_t)(uintptr_t)&r_msg, 0U) != 1) {
        return 27;
    }

    int32_t rec_fd = *(const int32_t *)(const void *)(recv_cbuf + 12);
    if (rec_fd < 0) {
        return 28;
    }

    char read_back[16];
    my_memset(read_back, 0, sizeof(read_back));
    int32_t n_read = sys3(3U /* read */, (uint32_t)rec_fd, (uint32_t)(uintptr_t)read_back, 13U);
    if (n_read != 13 || my_memcmp(read_back, "scm-rights-ok", 13U) != 0) {
        return 29;
    }

    sys1(6U /* close */, (uint32_t)rec_fd);
    sys1(6U /* close */, (uint32_t)pfd[0]);
    sys1(6U /* close */, (uint32_t)pfd[1]);
    sys1(6U /* close */, (uint32_t)sv[0]);
    sys1(6U /* close */, (uint32_t)sv[1]);
    return 0;
}

/* --------------------------------------------------------------------------
 * Test 3 & 4: epoll (357/251/252) + eventfd2 (356) + poll (168)
 * -------------------------------------------------------------------------- */
static int test3_4_epoll_eventfd_poll(void)
{
    int epfd = sys1(357U /* epoll_create1 */, 0U);
    if (epfd < 0) {
        return 30;
    }
    int efd = sys2(356U /* eventfd2 */, 0U, 0U);
    if (efd < 0) {
        sys1(6U /* close */, (uint32_t)epfd);
        return 31;
    }

    struct epoll_event ev;
    ev.events = 1U; /* EPOLLIN */
    ev.data   = 0x12345678ULL;
    if (sys4(251U /* epoll_ctl */, (uint32_t)epfd, 1U /* EPOLL_CTL_ADD */,
             (uint32_t)efd, (uint32_t)(uintptr_t)&ev) != 0) {
        sys1(6U, (uint32_t)efd);
        sys1(6U, (uint32_t)epfd);
        return 32;
    }

    /* Signal eventfd */
    uint64_t val = 1ULL;
    if (sys3(4U /* write */, (uint32_t)efd, (uint32_t)(uintptr_t)&val, sizeof(val)) != (int32_t)sizeof(val)) {
        sys1(6U, (uint32_t)efd);
        sys1(6U, (uint32_t)epfd);
        return 33;
    }

    /* Test 3: epoll_wait */
    struct epoll_event out_ev;
    out_ev.events = 0U;
    out_ev.data   = 0ULL;
    int ew_r = sys4(252U /* epoll_wait */, (uint32_t)epfd,
                    (uint32_t)(uintptr_t)&out_ev, 1U, 1000U);
    if (ew_r != 1 || (out_ev.events & 1U) == 0U || out_ev.data != 0x12345678ULL) {
        sys1(6U, (uint32_t)efd);
        sys1(6U, (uint32_t)epfd);
        return 34;
    }

    /* Test 4: poll on the still-ready eventfd */
    struct pollfd pfd;
    pfd.fd = efd;
    pfd.events = 1; /* POLLIN */
    pfd.revents = 0;
    int p_r = sys3(168U /* poll */, (uint32_t)(uintptr_t)&pfd, 1U, 1000U);
    if (p_r != 1 || (pfd.revents & 1) == 0) {
        sys1(6U, (uint32_t)efd);
        sys1(6U, (uint32_t)epfd);
        return 35;
    }

    /* Consume the eventfd */
    uint64_t consumed = 0ULL;
    sys3(3U /* read */, (uint32_t)efd, (uint32_t)(uintptr_t)&consumed, sizeof(consumed));

    /* Test 5: fcntl64 */
    int fl = sys3(221U /* fcntl64 */, (uint32_t)efd, 3U /* F_GETFL */, 0U);
    if (fl < 0) {
        sys1(6U, (uint32_t)efd);
        sys1(6U, (uint32_t)epfd);
        return 36;
    }
    if (sys3(221U, (uint32_t)efd, 4U /* F_SETFL */, (uint32_t)(fl | 0x800 /* O_NONBLOCK */)) != 0) {
        sys1(6U, (uint32_t)efd);
        sys1(6U, (uint32_t)epfd);
        return 37;
    }
    int fl2 = sys3(221U, (uint32_t)efd, 3U /* F_GETFL */, 0U);
    if ((fl2 & 0x800) != 0x800) {
        sys1(6U, (uint32_t)efd);
        sys1(6U, (uint32_t)epfd);
        return 38;
    }

    int dupfd = sys3(221U, (uint32_t)efd, 1030U /* F_DUPFD_CLOEXEC */, 0U);
    if (dupfd < 0) {
        sys1(6U, (uint32_t)efd);
        sys1(6U, (uint32_t)epfd);
        return 39;
    }
    sys1(6U /* close */, (uint32_t)dupfd);
    sys1(6U /* close */, (uint32_t)efd);
    sys1(6U /* close */, (uint32_t)epfd);
    return 0;
}

/* --------------------------------------------------------------------------
 * Test 6: statx (397) and 84-byte statfs64 (266) / fstatfs64 (267) with canary
 * -------------------------------------------------------------------------- */
static int test6_statx_statfs(void)
{
    struct statfs64_canary sfs_buf;
    my_memset(&sfs_buf.stfs, 0, sizeof(sfs_buf.stfs));
    sfs_buf.canary = 0xDEADBEEFU;

    int sfs_r = sys3(266U /* statfs64 */, (uint32_t)(uintptr_t)"/",
                     84U /* sizeof(struct statfs64) */,
                     (uint32_t)(uintptr_t)&sfs_buf.stfs);
    if (sfs_r != 0 || sfs_buf.stfs.f_type == 0U || sfs_buf.stfs.f_bsize == 0U) {
        return 40;
    }
    if (sfs_buf.canary != 0xDEADBEEFU) {
        return 42;
    }

    int root_fd = sys4(322U /* openat */, 0xFFFFFF9CU /* AT_FDCWD */,
                       (uint32_t)(uintptr_t)"/", 0U /* O_RDONLY */, 0U);
    if (root_fd < 0) {
        return 43;
    }
    my_memset(&sfs_buf.stfs, 0, sizeof(sfs_buf.stfs));
    sfs_buf.canary = 0xDEADBEEFU;
    int fsfs_r = sys3(267U /* fstatfs64 */, (uint32_t)root_fd,
                      84U /* sizeof(struct statfs64) */,
                      (uint32_t)(uintptr_t)&sfs_buf.stfs);
    sys1(6U /* close */, (uint32_t)root_fd);
    if (fsfs_r != 0 || sfs_buf.stfs.f_type == 0U || sfs_buf.stfs.f_bsize == 0U) {
        return 44;
    }
    if (sfs_buf.canary != 0xDEADBEEFU) {
        return 45;
    }

    struct statx stx;
    my_memset(&stx, 0, sizeof(stx));
    int sx_r = sys5(397U /* statx */, 0xFFFFFF9CU /* AT_FDCWD */,
                    (uint32_t)(uintptr_t)"/", 0U, 0x7FFU /* STATX_BASIC_STATS */,
                    (uint32_t)(uintptr_t)&stx);
    if (sx_r != 0) {
        return 41;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Test 7: timerfd_create (350), timerfd_settime (353), timerfd_gettime (354)
 *         with 16-byte itimerspec32
 * -------------------------------------------------------------------------- */
static int test7_timerfd(void)
{
    int tfd = sys2(350U /* timerfd_create */, 1U /* CLOCK_MONOTONIC */, 0U);
    if (tfd < 0) {
        return 50;
    }

    struct itimerspec32 arm_its;
    arm_its.it_interval.tv_sec  = 0;
    arm_its.it_interval.tv_nsec = 0;
    arm_its.it_value.tv_sec     = 0;
    arm_its.it_value.tv_nsec    = 50000000;

    struct itimerspec32_canary old_its;
    my_memset(&old_its.its, 0x5A, sizeof(old_its.its));
    old_its.canary[0] = 0xDEADBEEFU;
    old_its.canary[1] = 0xDEADBEEFU;
    old_its.canary[2] = 0xDEADBEEFU;
    old_its.canary[3] = 0xDEADBEEFU;

    if (sys4(353U /* timerfd_settime */, (uint32_t)tfd, 0U,
             (uint32_t)(uintptr_t)&arm_its,
             (uint32_t)(uintptr_t)&old_its.its) != 0) {
        sys1(6U /* close */, (uint32_t)tfd);
        return 51;
    }
    if (old_its.canary[0] != 0xDEADBEEFU || old_its.its.it_value.tv_sec != 0) {
        sys1(6U /* close */, (uint32_t)tfd);
        return 52;
    }

    struct itimerspec32_canary cur_its;
    my_memset(&cur_its.its, 0x5A, sizeof(cur_its.its));
    cur_its.canary[0] = 0xDEADBEEFU;
    cur_its.canary[1] = 0xDEADBEEFU;
    cur_its.canary[2] = 0xDEADBEEFU;
    cur_its.canary[3] = 0xDEADBEEFU;

    if (sys2(354U /* timerfd_gettime */, (uint32_t)tfd,
             (uint32_t)(uintptr_t)&cur_its.its) != 0) {
        sys1(6U /* close */, (uint32_t)tfd);
        return 53;
    }
    if (cur_its.canary[0] != 0xDEADBEEFU ||
        cur_its.its.it_interval.tv_sec != 0 ||
        cur_its.its.it_interval.tv_nsec != 0 ||
        cur_its.its.it_value.tv_sec != 0 ||
        cur_its.its.it_value.tv_nsec < 0 ||
        cur_its.its.it_value.tv_nsec > 50000000) {
        sys1(6U /* close */, (uint32_t)tfd);
        return 54;
    }

    sys1(6U /* close */, (uint32_t)tfd);
    return 0;
}

/* --------------------------------------------------------------------------
 * Test 8: utimensat(fd, NULL, times, 0) (futimens) in offset mode
 * -------------------------------------------------------------------------- */
static int test8_utimensat_futimens(void)
{
    static const char tmp_path[] = "/tmp/a32rt_m5_utimens_test";
    int fd = sys4(322U /* openat */, 0xFFFFFF9CU /* AT_FDCWD */,
                  (uint32_t)(uintptr_t)tmp_path,
                  0x242U /* O_CREAT | O_TRUNC | O_RDWR */, 0600U);
    if (fd < 0) {
        return 60;
    }

    /* futimens(fd, NULL): pathname = NULL (0), times = NULL (0) */
    int r1 = sys4(348U /* utimensat */, (uint32_t)fd, 0U /* NULL */, 0U /* NULL */, 0U);
    if (r1 != 0) {
        sys1(6U /* close */, (uint32_t)fd);
        sys3(328U /* unlinkat */, 0xFFFFFF9CU, (uint32_t)(uintptr_t)tmp_path, 0U);
        return 61;
    }

    /* futimens(fd, times): pathname = NULL (0), explicit timespec32[2] */
    struct timespec32 ts[2];
    ts[0].tv_sec  = 1700000000;
    ts[0].tv_nsec = 123456789;
    ts[1].tv_sec  = 1700000000;
    ts[1].tv_nsec = 987654321;
    int r2 = sys4(348U /* utimensat */, (uint32_t)fd, 0U /* NULL */,
                  (uint32_t)(uintptr_t)ts, 0U);
    sys1(6U /* close */, (uint32_t)fd);
    sys3(328U /* unlinkat */, 0xFFFFFF9CU /* AT_FDCWD */,
         (uint32_t)(uintptr_t)tmp_path, 0U);
    if (r2 != 0) {
        return 62;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Test 9: pselect6 (335) and _newselect (142) with pipe fd
 * -------------------------------------------------------------------------- */
static int test9_pselect6_newselect(void)
{
    int pfd[2];
    pfd[0] = -1;
    pfd[1] = -1;
    if (sys2(359U /* pipe2 */, (uint32_t)(uintptr_t)pfd, 0U) != 0) {
        return 70;
    }

    char byte_in = 'Q';
    if (sys3(4U /* write */, (uint32_t)pfd[1], (uint32_t)(uintptr_t)&byte_in, 1U) != 1) {
        sys1(6U /* close */, (uint32_t)pfd[0]);
        sys1(6U /* close */, (uint32_t)pfd[1]);
        return 71;
    }

    uint32_t fd_idx = (uint32_t)pfd[0] / 32U;
    uint32_t fd_bit = 1U << ((uint32_t)pfd[0] % 32U);

    /* 1. SYS_pselect6 (335) */
    uint32_t rfds[32];
    my_memset(rfds, 0, sizeof(rfds));
    rfds[fd_idx] |= fd_bit;

    struct timespec32 ts;
    ts.tv_sec  = 0;
    ts.tv_nsec = 50000000;

    int ps_r = sys6(335U /* pselect6 */, (uint32_t)(pfd[0] + 1),
                    (uint32_t)(uintptr_t)rfds, 0U, 0U,
                    (uint32_t)(uintptr_t)&ts, 0U);
    if (ps_r != 1 || (rfds[fd_idx] & fd_bit) == 0U) {
        sys1(6U /* close */, (uint32_t)pfd[0]);
        sys1(6U /* close */, (uint32_t)pfd[1]);
        return 72;
    }

    /* 2. SYS__newselect (142) on the still-ready pipe fd */
    my_memset(rfds, 0, sizeof(rfds));
    rfds[fd_idx] |= fd_bit;

    struct timeval32 tv;
    tv.tv_sec  = 0;
    tv.tv_usec = 50000;

    int ns_r = sys5(142U /* _newselect */, (uint32_t)(pfd[0] + 1),
                    (uint32_t)(uintptr_t)rfds, 0U, 0U,
                    (uint32_t)(uintptr_t)&tv);
    if (ns_r != 1 || (rfds[fd_idx] & fd_bit) == 0U) {
        sys1(6U /* close */, (uint32_t)pfd[0]);
        sys1(6U /* close */, (uint32_t)pfd[1]);
        return 73;
    }

    char byte_out = 0;
    if (sys3(3U /* read */, (uint32_t)pfd[0], (uint32_t)(uintptr_t)&byte_out, 1U) != 1 ||
        byte_out != 'Q') {
        sys1(6U /* close */, (uint32_t)pfd[0]);
        sys1(6U /* close */, (uint32_t)pfd[1]);
        return 74;
    }

    sys1(6U /* close */, (uint32_t)pfd[0]);
    sys1(6U /* close */, (uint32_t)pfd[1]);
    return 0;
}

/* --------------------------------------------------------------------------
 * Test 10: Negative (-EFAULT / -EINVAL) syscall tests
 * -------------------------------------------------------------------------- */
static int test10_negative_syscalls(void)
{
    /* openat (322) with NULL (0) and low-guard pointer (0x100) -> -EFAULT (-14) */
    if (sys4(322U /* openat */, 0xFFFFFF9CU /* AT_FDCWD */, 0U, 0U, 0U) != -14) {
        return 80;
    }
    if (sys4(322U /* openat */, 0xFFFFFF9CU /* AT_FDCWD */, 0x100U, 0U, 0U) != -14) {
        return 81;
    }

    /* stat64 (195) with NULL (0) and low-guard pointer (0x100) -> -EFAULT (-14) */
    _Alignas(8) uint8_t st64_buf[104];
    my_memset(st64_buf, 0, sizeof(st64_buf));
    if (sys2(195U /* stat64 */, 0U, (uint32_t)(uintptr_t)st64_buf) != -14) {
        return 82;
    }
    if (sys2(195U /* stat64 */, 0x100U, (uint32_t)(uintptr_t)st64_buf) != -14) {
        return 83;
    }
    if (sys2(195U /* stat64 */, (uint32_t)(uintptr_t)"/", 0U) != -14) {
        return 84;
    }
    if (sys2(195U /* stat64 */, (uint32_t)(uintptr_t)"/", 0x100U) != -14) {
        return 85;
    }

    /* readlinkat (332) with NULL (0) and low-guard pointer (0x100) -> -EFAULT (-14) */
    char rl_buf[64];
    my_memset(rl_buf, 0, sizeof(rl_buf));
    if (sys4(332U /* readlinkat */, 0xFFFFFF9CU /* AT_FDCWD */, 0U,
             (uint32_t)(uintptr_t)rl_buf, sizeof(rl_buf)) != -14) {
        return 86;
    }
    if (sys4(332U /* readlinkat */, 0xFFFFFF9CU /* AT_FDCWD */, 0x100U,
             (uint32_t)(uintptr_t)rl_buf, sizeof(rl_buf)) != -14) {
        return 87;
    }
    if (sys4(332U /* readlinkat */, 0xFFFFFF9CU /* AT_FDCWD */,
             (uint32_t)(uintptr_t)"/proc/self/exe", 0U, sizeof(rl_buf)) != -14) {
        return 88;
    }
    if (sys4(332U /* readlinkat */, 0xFFFFFF9CU /* AT_FDCWD */,
             (uint32_t)(uintptr_t)"/proc/self/exe", 0x100U, sizeof(rl_buf)) != -14) {
        return 89;
    }

    /* Negative -EINVAL (-22): statfs64 with wrong size (0) */
    struct statfs64 sfs;
    if (sys3(266U /* statfs64 */, (uint32_t)(uintptr_t)"/", 0U,
             (uint32_t)(uintptr_t)&sfs) != -22) {
        return 90;
    }

    /* Additional negative -EFAULT (-14) tests with low-guard pointer (0x100) */
    if (sys1(122U /* uname */, 0x100U) != -14) {
        return 91;
    }
    if (sys2(183U /* getcwd */, 0x100U, 64U) != -14) {
        return 92;
    }
    if (sys2(191U /* ugetrlimit */, 3U /* RLIMIT_STACK */, 0x100U) != -14) {
        return 93;
    }
    if (sys1(116U /* sysinfo */, 0x100U) != -14) {
        return 94;
    }
    if (sys2(263U /* clock_gettime32 */, 0U /* CLOCK_REALTIME */, 0x100U) != -14) {
        return 95;
    }
    if (sys2(359U /* pipe2 */, 0x100U, 0U) != -14) {
        return 96;
    }

    /* Negative -EFAULT (-14): mremap (163) on unmapped guest address */
    if (sys4(163U /* mremap */, 0x25000000U, 4096U, 8192U,
             1U /* MREMAP_MAYMOVE */) != -14) {
        return 97;
    }

    /* Negative -EINVAL (-22): readlink (85) and readlinkat (332) with bufsiz == 0 */
    if (sys3(85U /* readlink */, (uint32_t)(uintptr_t)"/proc/self/exe",
             (uint32_t)(uintptr_t)rl_buf, 0U) != -22) {
        return 98;
    }
    if (sys4(332U /* readlinkat */, 0xFFFFFF9CU /* AT_FDCWD */,
             (uint32_t)(uintptr_t)"/proc/self/exe",
             (uint32_t)(uintptr_t)rl_buf, 0U) != -22) {
        return 99;
    }

    return 0;
}

static const char *my_strstr(const char *haystack, const char *needle)
{
    if (!*needle) {
        return haystack;
    }
    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n && (*h == *n)) {
            h++;
            n++;
        }
        if (!*n) {
            return haystack;
        }
    }
    return NULL;
}

/* --------------------------------------------------------------------------
 * Test 11: MAP_FIXED_NOREPLACE (0x100000U) regression test
 * -------------------------------------------------------------------------- */
static int test11_map_fixed_noreplace(void)
{
    uint32_t p = (uint32_t)sys6(192U /* mmap2 */, 0U, 4096U,
                                3U /* PROT_READ|PROT_WRITE */,
                                0x22U /* MAP_PRIVATE|MAP_ANONYMOUS */,
                                0xFFFFFFFFU, 0U);
    if (p > 0xFFFFF000U) {
        return 100;
    }

    volatile uint32_t *vp = (volatile uint32_t *)(uintptr_t)p;
    vp[0] = 0x13579BDFU;

    /* Negative: MAP_FIXED_NOREPLACE on already-mapped page p must return -EEXIST (-17) */
    int32_t dup_rc = sys6(192U /* mmap2 */, p, 4096U,
                          3U /* PROT_READ|PROT_WRITE */,
                          0x22U | 0x100000U /* MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE */,
                          0xFFFFFFFFU, 0U);
    if (dup_rc != -17 /* -EEXIST */) {
        sys2(91U /* munmap */, p, 4096U);
        return 101;
    }

    /* Verify existing mapping at p was not overwritten */
    if (vp[0] != 0x13579BDFU) {
        sys2(91U /* munmap */, p, 4096U);
        return 102;
    }

    if (sys2(91U /* munmap */, p, 4096U) != 0) {
        return 103;
    }

    /* Positive: MAP_FIXED_NOREPLACE on now-unmapped page p must succeed at p */
    uint32_t p2 = (uint32_t)sys6(192U /* mmap2 */, p, 4096U,
                                 3U /* PROT_READ|PROT_WRITE */,
                                 0x22U | 0x100000U /* MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE */,
                                 0xFFFFFFFFU, 0U);
    if (p2 != p) {
        if (p2 <= 0xFFFFF000U) {
            sys2(91U /* munmap */, p2, 4096U);
        }
        return 104;
    }
    sys2(91U /* munmap */, p2, 4096U);
    return 0;
}

/* --------------------------------------------------------------------------
 * Test 12: mremap with PROT_READ | PROT_EXEC (W^X compliance under hl)
 * -------------------------------------------------------------------------- */
static int test12_mremap_rx_wx(void)
{
    /* Map 8 KiB (2 pages) RW so the second page blocks in-place growth of the first page */
    uint32_t base = (uint32_t)sys6(192U /* mmap2 */, 0U, 8192U,
                                   3U /* PROT_READ|PROT_WRITE */,
                                   0x22U /* MAP_PRIVATE|MAP_ANONYMOUS */,
                                   0xFFFFFFFFU, 0U);
    if (base > 0xFFFFF000U) {
        return 105;
    }
    uint32_t blocker = base + 4096U;

    uint32_t *code = (uint32_t *)(uintptr_t)base;
    /* A32: mov r0, #85 (0xE3A00055); bx lr (0xE12FFF1E) */
    code[0] = 0xE3A00055U;
    code[1] = 0xE12FFF1EU;

    if (sys3(125U /* mprotect */, base, 4096U, 5U /* PROT_READ|PROT_EXEC */) != 0) {
        sys2(91U /* munmap */, base, 8192U);
        return 106;
    }
    sys3(0x0F0002U /* cacheflush */, base, base + 16U, 0U);

    typedef uint32_t (*rx_fn_t)(void);
    rx_fn_t fn_old = (rx_fn_t)(uintptr_t)base;
    if (fn_old() != 85U) {
        sys2(91U /* munmap */, base, 8192U);
        return 107;
    }

    /*
     * Grow RX page from 4 KiB to 8 KiB with MREMAP_MAYMOVE (1).
     * Because `blocker` occupies base + 4096, mremap must relocate the RX
     * mapping while respecting W^X (never requesting PROT_WRITE | PROT_EXEC).
     */
    uint32_t new_rx = (uint32_t)sys4(163U /* mremap */, base, 4096U, 8192U,
                                     1U /* MREMAP_MAYMOVE */);
    if (new_rx > 0xFFFFF000U || (new_rx & 0xFFFU) != 0U) {
        sys2(91U /* munmap */, base, 8192U);
        return 108;
    }
    sys3(0x0F0002U /* cacheflush */, new_rx, new_rx + 16U, 0U);

    rx_fn_t fn_new = (rx_fn_t)(uintptr_t)new_rx;
    if (fn_new() != 85U) {
        sys2(91U /* munmap */, new_rx, 8192U);
        sys2(91U /* munmap */, blocker, 4096U);
        return 109;
    }

    sys2(91U /* munmap */, new_rx, 8192U);
    sys2(91U /* munmap */, blocker, 4096U);
    return 0;
}

/* --------------------------------------------------------------------------
 * Test 13: /proc/self/maps containing [heap] after brk expansion
 * -------------------------------------------------------------------------- */
static int test13_proc_maps_heap(void)
{
    uint32_t cur_brk = (uint32_t)sys1(45U /* brk */, 0U);
    if (cur_brk == 0U) {
        return 110;
    }
    uint32_t new_brk = (uint32_t)sys1(45U /* brk */, cur_brk + 4096U);
    if (new_brk != cur_brk + 4096U) {
        return 111;
    }

    int32_t mfd = sys4(322U /* openat */, 0xFFFFFF9CU /* AT_FDCWD */,
                       (uint32_t)(uintptr_t)"/proc/self/maps", 0U /* O_RDONLY */, 0U);
    if (mfd < 0) {
        (void)sys1(45U /* brk */, cur_brk);
        return 112;
    }

    char maps_buf[4096];
    my_memset(maps_buf, 0, sizeof(maps_buf));
    int32_t mread = 0;
    while (mread < (int32_t)(sizeof(maps_buf) - 1U)) {
        int32_t nr = sys3(3U /* read */, (uint32_t)mfd,
                          (uint32_t)(uintptr_t)(maps_buf + mread),
                          (uint32_t)(sizeof(maps_buf) - 1U - (uint32_t)mread));
        if (nr <= 0) {
            break;
        }
        mread += nr;
    }
    sys1(6U /* close */, (uint32_t)mfd);
    (void)sys1(45U /* brk */, cur_brk);

    if (mread <= 0) {
        return 113;
    }
    maps_buf[mread] = '\0';
    if (!my_strstr(maps_buf, "[heap]")) {
        return 114;
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * Test 14: Guest RWX (PROT_READ|PROT_WRITE|PROT_EXEC) mmap2 & mprotect under W^X
 * -------------------------------------------------------------------------- */
static int test14_rwx_mmap_mprotect(void)
{
    /* Part A: mmap2 directly with PROT_READ|PROT_WRITE|PROT_EXEC (7U) */
    uint32_t rwx_page = (uint32_t)sys6(192U /* mmap2 */, 0U, 4096U,
                                       7U /* PROT_READ|PROT_WRITE|PROT_EXEC */,
                                       0x22U /* MAP_PRIVATE|MAP_ANONYMOUS */,
                                       0xFFFFFFFFU, 0U);
    if (rwx_page > 0xFFFFF000U) {
        return 115;
    }

    volatile uint32_t *code = (volatile uint32_t *)(uintptr_t)rwx_page;
    code[0] = 0xE3A0003AU; /* mov r0, #58 */
    code[1] = 0xE12FFF1EU; /* bx lr */
    sys3(0x0F0002U /* cacheflush */, rwx_page, rwx_page + 16U, 0U);

    typedef uint32_t (*rwx_fn_t)(void);
    rwx_fn_t fn = (rwx_fn_t)(uintptr_t)rwx_page;
    if (fn() != 58U) {
        sys2(91U /* munmap */, rwx_page, 4096U);
        return 116;
    }

    /* Modify in place WITHOUT calling mprotect (proves guest RWX remains writable) */
    code[0] = 0xE3A0005BU; /* mov r0, #91 */
    sys3(0x0F0002U /* cacheflush */, rwx_page, rwx_page + 16U, 0U);
    if (fn() != 91U) {
        sys2(91U /* munmap */, rwx_page, 4096U);
        return 117;
    }
    sys2(91U /* munmap */, rwx_page, 4096U);

    /* Part B: mmap2 PROT_READ then mprotect to PROT_READ|PROT_WRITE|PROT_EXEC (7U) */
    uint32_t p2 = (uint32_t)sys6(192U /* mmap2 */, 0U, 4096U,
                                 1U /* PROT_READ */,
                                 0x22U /* MAP_PRIVATE|MAP_ANONYMOUS */,
                                 0xFFFFFFFFU, 0U);
    if (p2 > 0xFFFFF000U) {
        return 118;
    }
    if (sys3(125U /* mprotect */, p2, 4096U, 7U /* PROT_READ|PROT_WRITE|PROT_EXEC */) != 0) {
        sys2(91U /* munmap */, p2, 4096U);
        return 119;
    }

    volatile uint32_t *code2 = (volatile uint32_t *)(uintptr_t)p2;
    code2[0] = 0xE3A0004CU; /* mov r0, #76 */
    code2[1] = 0xE12FFF1EU; /* bx lr */
    sys3(0x0F0002U /* cacheflush */, p2, p2 + 16U, 0U);

    rwx_fn_t fn2 = (rwx_fn_t)(uintptr_t)p2;
    if (fn2() != 76U) {
        sys2(91U /* munmap */, p2, 4096U);
        return 120;
    }

    code2[0] = 0xE3A00063U; /* mov r0, #99 */
    sys3(0x0F0002U /* cacheflush */, p2, p2 + 16U, 0U);
    if (fn2() != 99U) {
        sys2(91U /* munmap */, p2, 4096U);
        return 121;
    }

    sys2(91U /* munmap */, p2, 4096U);
    return 0;
}

/* --------------------------------------------------------------------------
 * Test 15: ppoll (336) sigsetsize validation (positive == 8, negative != 8)
 * -------------------------------------------------------------------------- */
static int test15_ppoll_sigsetsize(void)
{
    int32_t efd = sys2(356U /* eventfd2 */, 1U, 0x800U /* EFD_NONBLOCK */);
    if (efd < 0) {
        return 122;
    }

    struct pollfd pfd;
    pfd.fd      = efd;
    pfd.events  = 1; /* POLLIN */
    pfd.revents = 0;

    struct timespec32 ts;
    ts.tv_sec  = 0;
    ts.tv_nsec = 0;

    uint64_t sigmask = 0ULL;

    /* Negative: sigsetsize != 8 (0, 4, 16) must return -EINVAL (-22) */
    if (sys5(336U /* ppoll */, (uint32_t)(uintptr_t)&pfd, 1U,
             (uint32_t)(uintptr_t)&ts, (uint32_t)(uintptr_t)&sigmask, 0U) != -22) {
        sys1(6U /* close */, (uint32_t)efd);
        return 123;
    }
    if (sys5(336U /* ppoll */, (uint32_t)(uintptr_t)&pfd, 1U,
             (uint32_t)(uintptr_t)&ts, (uint32_t)(uintptr_t)&sigmask, 4U) != -22) {
        sys1(6U /* close */, (uint32_t)efd);
        return 124;
    }
    if (sys5(336U /* ppoll */, (uint32_t)(uintptr_t)&pfd, 1U,
             (uint32_t)(uintptr_t)&ts, (uint32_t)(uintptr_t)&sigmask, 16U) != -22) {
        sys1(6U /* close */, (uint32_t)efd);
        return 125;
    }

    /* Positive: sigsetsize == 8 must succeed and report POLLIN */
    int32_t prc = sys5(336U /* ppoll */, (uint32_t)(uintptr_t)&pfd, 1U,
                       (uint32_t)(uintptr_t)&ts, (uint32_t)(uintptr_t)&sigmask, 8U);
    sys1(6U /* close */, (uint32_t)efd);
    if (prc != 1 || (pfd.revents & 1) == 0) {
        return 126;
    }

    return 0;
}

static int run_all_m5(void)
{
    int rc = test1_uname();
    if (rc != 0) {
        return rc;
    }
    rc = test2_sockets();
    if (rc != 0) {
        return rc;
    }
    rc = test3_4_epoll_eventfd_poll();
    if (rc != 0) {
        return rc;
    }
    rc = test6_statx_statfs();
    if (rc != 0) {
        return rc;
    }
    rc = test7_timerfd();
    if (rc != 0) {
        return rc;
    }
    rc = test8_utimensat_futimens();
    if (rc != 0) {
        return rc;
    }
    rc = test9_pselect6_newselect();
    if (rc != 0) {
        return rc;
    }
    rc = test10_negative_syscalls();
    if (rc != 0) {
        return rc;
    }
    rc = test11_map_fixed_noreplace();
    if (rc != 0) {
        return rc;
    }
    rc = test12_mremap_rx_wx();
    if (rc != 0) {
        return rc;
    }
    rc = test13_proc_maps_heap();
    if (rc != 0) {
        return rc;
    }
    rc = test14_rwx_mmap_mprotect();
    if (rc != 0) {
        return rc;
    }
    rc = test15_ppoll_sigsetsize();
    if (rc != 0) {
        return rc;
    }

    static const char pass_msg[] = "M5 LTP Compat Conformance: PASS\n";
    sys3(4U /* write */, 1U, (uint32_t)(uintptr_t)pass_msg, (uint32_t)(sizeof(pass_msg) - 1U));
    return 0;
}

void __attribute__((noreturn)) _start_c(void)
{
    int rc = run_all_m5();
    sys1(248U /* exit_group */, (uint32_t)rc);
    __builtin_unreachable();
}

__asm__(
    ".text\n"
    ".globl _start\n"
    ".type _start, %function\n"
    "_start:\n"
    "    mov r0, sp\n"
    "    bic r1, r0, #7\n"
    "    mov sp, r1\n"
    "    bl _start_c\n"
);
