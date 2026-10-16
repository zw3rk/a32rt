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

struct statfs64 {
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
 * Test 6: statx (397) and statfs64 (266) on "/"
 * -------------------------------------------------------------------------- */
static int test6_statx_statfs(void)
{
    struct statfs64 stfs;
    my_memset(&stfs, 0, sizeof(stfs));
    int sfs_r = sys3(266U /* statfs64 */, (uint32_t)(uintptr_t)"/",
                     (uint32_t)sizeof(stfs), (uint32_t)(uintptr_t)&stfs);
    if (sfs_r != 0 || stfs.f_type == 0U) {
        return 40;
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
