/*
 * tests/ios/test_ios_proc_net.c — proc/net qualification (fork, execve, pipe,
 * sigaltstack, socketpair, poll, kqueue).
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Linked with: ld -arch armv7 -pie -ios_version_min 9.0 -rpath
 *   @executable_path/rpath_sub .../rpath_sub/librpath_ios.dylib libSystem.tbd
 * --no-fork skips fork/exec so --difftest runs deterministically in lockstep.
 */

#include "ios_guest.h"

extern int rpath_ctor_ran(void);
extern int rpath_combine(int a, int b);

static volatile uint32_t g_handler_sp = 0U;
static volatile int g_handler_ran = 0;
static volatile int g_handler_onstack = 0;

static void alt_handler(int sig, void *info, void *ctx)
{
    char marker;
    ios_stack32_t cur;
    uint32_t carry = 0U;
    int32_t rc = 0;
    uint32_t i = 0U;
    (void)sig;
    (void)info;
    (void)ctx;
    g_handler_sp = (uint32_t)(uintptr_t)&marker;
    for (i = 0U; i < (uint32_t)sizeof(cur); i++) {
        ((char *)&cur)[i] = 0;
    }
    rc = ios_bsd6((uint32_t)IOS_SYS_sigaltstack, 0U,
        (uint32_t)(uintptr_t)&cur, 0U, 0U, 0U, 0U, &carry);
    if (carry == 0U && rc == 0 && (cur.ss_flags & IOS_SS_ONSTACK) != 0) {
        g_handler_onstack = 1;
    } else {
        g_handler_onstack = 0;
    }
    g_handler_ran = 1;
}

static int check_rpath(void)
{
    if (rpath_ctor_ran() != 77) {
        printf("FAIL rpath ctor ran=%d\n", rpath_ctor_ran());
        return 1;
    }
    if (rpath_combine(3, 5) != 382) {
        printf("FAIL rpath combine=%d\n", rpath_combine(3, 5));
        return 1;
    }
    return 0;
}

static int check_altstack(void)
{
    char *alt = 0;
    ios_stack32_t nss;
    ios_stack32_t oss;
    ios_stack32_t dis;
    ios_sigaction_t sa;
    ios_sigaction_t cur;
    uint32_t carry = 0U;
    int32_t rc = 0;
    uint32_t i = 0U;
    uint32_t lo = 0U;
    uint32_t hi = 0U;
    uint32_t hsp = 0U;
    alt = (char *)mmap(0, 16384, IOS_PROT_READ | IOS_PROT_WRITE,
        IOS_MAP_PRIVATE | IOS_MAP_ANON, -1, (int64_t)0);
    if (alt == IOS_MAP_FAILED || alt == 0) {
        printf("FAIL altstack mmap\n");
        return 1;
    }
    for (i = 0U; i < (uint32_t)sizeof(nss); i++) {
        ((char *)&nss)[i] = 0;
    }
    for (i = 0U; i < (uint32_t)sizeof(oss); i++) {
        ((char *)&oss)[i] = 0;
    }
    nss.ss_sp = (uint32_t)(uintptr_t)alt;
    nss.ss_size = 16384U;
    nss.ss_flags = 0;
    rc = ios_bsd6((uint32_t)IOS_SYS_sigaltstack,
        (uint32_t)(uintptr_t)&nss, (uint32_t)(uintptr_t)&oss,
        0U, 0U, 0U, 0U, &carry);
    if (carry != 0U || rc != 0) {
        printf("FAIL sigaltstack install rc=%d carry=%u\n", rc, carry);
        munmap(alt, 16384);
        return 1;
    }
    if (oss.ss_flags != IOS_SS_DISABLE) {
        printf("FAIL sigaltstack oss flags=%d\n", oss.ss_flags);
        munmap(alt, 16384);
        return 1;
    }
    for (i = 0U; i < (uint32_t)sizeof(sa); i++) {
        ((char *)&sa)[i] = 0;
    }
    for (i = 0U; i < (uint32_t)sizeof(cur); i++) {
        ((char *)&cur)[i] = 0;
    }
    sa.sa_sigaction = alt_handler;
    sa.sa_mask = 0U;
    sa.sa_flags = IOS_SA_SIGINFO | IOS_SA_ONSTACK | IOS_SA_RESETHAND;
    g_handler_ran = 0;
    g_handler_onstack = 0;
    g_handler_sp = 0U;
    if (sigaction(IOS_SIGUSR1, &sa, 0) != 0) {
        printf("FAIL sigaction altstack\n");
        munmap(alt, 16384);
        return 1;
    }
    if (kill(getpid(), IOS_SIGUSR1) != 0) {
        printf("FAIL kill usr1 altstack\n");
        munmap(alt, 16384);
        return 1;
    }
    if (g_handler_ran == 0) {
        printf("FAIL altstack handler not run\n");
        munmap(alt, 16384);
        return 1;
    }
    if (g_handler_onstack == 0) {
        printf("FAIL altstack not onstack\n");
        munmap(alt, 16384);
        return 1;
    }
    hsp = g_handler_sp;
    lo = (uint32_t)(uintptr_t)alt;
    hi = lo + 16384U;
    if (hsp < lo || hsp >= hi) {
        printf("FAIL handler_sp outside altstack\n");
        munmap(alt, 16384);
        return 1;
    }
    if (sigaction(IOS_SIGUSR1, 0, &cur) != 0) {
        printf("FAIL sigaction query\n");
        munmap(alt, 16384);
        return 1;
    }
    if (cur.sa_sigaction != 0) {
        printf("FAIL resethand not reset\n");
        munmap(alt, 16384);
        return 1;
    }
    for (i = 0U; i < (uint32_t)sizeof(dis); i++) {
        ((char *)&dis)[i] = 0;
    }
    dis.ss_sp = 0U;
    dis.ss_size = 0U;
    dis.ss_flags = IOS_SS_DISABLE;
    carry = 0U;
    rc = ios_bsd6((uint32_t)IOS_SYS_sigaltstack,
        (uint32_t)(uintptr_t)&dis, 0U, 0U, 0U, 0U, 0U, &carry);
    if (carry != 0U || rc != 0) {
        printf("FAIL sigaltstack disable\n");
        munmap(alt, 16384);
        return 1;
    }
    if (munmap(alt, 16384) != 0) {
        printf("FAIL altstack munmap\n");
        return 1;
    }
    return 0;
}

#define T_SYS_sysctl 202U
#define T_SYS_sendmsg 28U
#define T_SYS_recvmsg 27U
#define T_SYS_sysctlbyname 274U
#define T_EVFILT_WRITE (-2)
#define T_EVFILT_TIMER (-7)
#define T_EV_DELETE 0x0002U
#define T_EV_EOF 0x8000U
#define T_ENOMEM 12

typedef struct __attribute__((packed, aligned(4))) t_iov {
    uint32_t base;
    uint32_t len;
} t_iov_t;

typedef struct __attribute__((packed, aligned(4))) t_msg {
    uint32_t msg_name;
    uint32_t msg_namelen;
    uint32_t msg_iov;
    int32_t msg_iovlen;
    uint32_t msg_control;
    uint32_t msg_controllen;
    int32_t msg_flags;
} t_msg_t;

typedef struct __attribute__((packed, aligned(4))) t_cmsg {
    uint32_t cmsg_len;
    int32_t cmsg_level;
    int32_t cmsg_type;
    uint8_t cmsg_data[4];
} t_cmsg_t;

_Static_assert(sizeof(t_iov_t) == 8, "t_iov_t must be 8 bytes");
_Static_assert(sizeof(t_msg_t) == 28, "t_msg_t must be 28 bytes");
_Static_assert(sizeof(t_cmsg_t) == 16, "t_cmsg_t must be 16 bytes");

static int check_sysctl_short_buffer(void)
{
    /* sysctl hw.ncpu (mib {6,3}): full query works ... */
    int32_t mib[2];
    uint32_t oldlen = 0U;
    int32_t val = 0;
    char name[16];
    uint32_t namelen = 0U;
    uint32_t carry = 0U;
    int32_t rc = 0;
    uint32_t i = 0U;
    mib[0] = 6;
    mib[1] = 3;
    oldlen = 4U;
    carry = 0U;
    rc = ios_bsd6((uint32_t)T_SYS_sysctl, (uint32_t)(uintptr_t)mib, 2U,
        (uint32_t)(uintptr_t)&val, (uint32_t)(uintptr_t)&oldlen, 0U, 0U,
        &carry);
    if (carry != 0U || rc != 0 || oldlen != 4U) {
        printf("FAIL sysctl full rc=%d carry=%u oldlen=%u\n", rc, carry,
            oldlen);
        return 1;
    }
    /* ... but a 1-byte buffer must fail with ENOMEM and still report 4. */
    oldlen = 1U;
    carry = 0U;
    rc = ios_bsd6((uint32_t)T_SYS_sysctl, (uint32_t)(uintptr_t)mib, 2U,
        (uint32_t)(uintptr_t)&val, (uint32_t)(uintptr_t)&oldlen, 0U, 0U,
        &carry);
    if (carry == 0U || rc != (int32_t)T_ENOMEM || oldlen != 4U) {
        printf("FAIL sysctl short rc=%d carry=%u oldlen=%u\n", rc, carry,
            oldlen);
        return 1;
    }
    /* Same contract via sysctlbyname. */
    for (i = 0U; i < (uint32_t)sizeof(name); i++) {
        name[i] = 0;
    }
    name[0] = 'h';
    name[1] = 'w';
    name[2] = '.';
    name[3] = 'n';
    name[4] = 'c';
    name[5] = 'p';
    name[6] = 'u';
    namelen = 7U;
    oldlen = 1U;
    carry = 0U;
    rc = ios_bsd6((uint32_t)T_SYS_sysctlbyname,
        (uint32_t)(uintptr_t)name, namelen, (uint32_t)(uintptr_t)&val,
        (uint32_t)(uintptr_t)&oldlen, 0U, 0U, &carry);
    if (carry == 0U || rc != (int32_t)T_ENOMEM || oldlen != 4U) {
        printf("FAIL sysctlbyname short rc=%d carry=%u oldlen=%u\n", rc,
            carry, oldlen);
        return 1;
    }
    return 0;
}

static int check_kqueue_read_write_timer(void)
{
    int sv[2];
    uint32_t carry = 0U;
    int32_t rc = 0;
    int kq = -1;
    ios_kevent32_t chg[2];
    ios_kevent32_t ev[2];
    ios_kevent32_t tchg;
    ios_kevent32_t tev;
    uint32_t i = 0U;
    int got_read = 0;
    int got_write = 0;
    sv[0] = -1;
    sv[1] = -1;
    rc = ios_bsd6((uint32_t)IOS_SYS_socketpair, 1U, 1U, 0U,
        (uint32_t)(uintptr_t)sv, 0U, 0U, &carry);
    if (carry != 0U || rc != 0 || sv[0] < 0 || sv[1] < 0) {
        printf("FAIL kqueue-rw socketpair\n");
        return 1;
    }
    if (write(sv[0], "rw!", 3) != 3) {
        printf("FAIL kqueue-rw write\n");
        close(sv[0]);
        close(sv[1]);
        return 1;
    }
    carry = 0U;
    rc = ios_bsd0((uint32_t)IOS_SYS_kqueue, &carry);
    if (carry != 0U || rc < 0) {
        printf("FAIL kqueue-rw kqueue\n");
        close(sv[0]);
        close(sv[1]);
        return 1;
    }
    kq = rc;
    for (i = 0U; i < (uint32_t)sizeof(chg); i++) {
        ((char *)chg)[i] = 0;
    }
    for (i = 0U; i < (uint32_t)sizeof(ev); i++) {
        ((char *)ev)[i] = 0;
    }
    /* READ and WRITE on the SAME fd must coexist as separate knotes. */
    chg[0].ident = (uint32_t)sv[1];
    chg[0].filter = (int16_t)IOS_EVFILT_READ;
    chg[0].flags = (uint16_t)IOS_EV_ADD;
    chg[0].udata = 0x11111111U;
    chg[1].ident = (uint32_t)sv[1];
    chg[1].filter = (int16_t)T_EVFILT_WRITE;
    chg[1].flags = (uint16_t)IOS_EV_ADD;
    chg[1].udata = 0x22222222U;
    carry = 0U;
    rc = ios_bsd6((uint32_t)IOS_SYS_kevent, (uint32_t)kq,
        (uint32_t)(uintptr_t)chg, 2U, (uint32_t)(uintptr_t)ev, 2U, 0U,
        &carry);
    if (carry != 0U || rc != 2) {
        printf("FAIL kqueue-rw kevent rc=%d carry=%u\n", rc, carry);
        close(kq);
        close(sv[0]);
        close(sv[1]);
        return 1;
    }
    for (i = 0U; i < 2U; i++) {
        if (ev[i].filter == (int16_t)IOS_EVFILT_READ &&
                ev[i].udata == 0x11111111U) {
            got_read = 1;
        }
        if (ev[i].filter == (int16_t)T_EVFILT_WRITE &&
                ev[i].udata == 0x22222222U) {
            got_write = 1;
        }
        if ((ev[i].flags & (uint16_t)T_EV_EOF) != 0U) {
            printf("FAIL kqueue-rw eof\n");
            close(kq);
            close(sv[0]);
            close(sv[1]);
            return 1;
        }
    }
    if (got_read == 0 || got_write == 0) {
        printf("FAIL kqueue-rw events read=%d write=%d\n", got_read,
            got_write);
        close(kq);
        close(sv[0]);
        close(sv[1]);
        return 1;
    }
    /* Drop the sv[1] READ/WRITE knotes before the timer wait: sv[1] is
       still writable, so a live EVFILT_WRITE knote would make the NULL-
       timeout kevent return immediately instead of waiting for the timer. */
    chg[0].flags = (uint16_t)T_EV_DELETE;
    chg[1].flags = (uint16_t)T_EV_DELETE;
    carry = 0U;
    (void)ios_bsd6((uint32_t)IOS_SYS_kevent, (uint32_t)kq,
        (uint32_t)(uintptr_t)chg, 2U, 0U, 0U, 0U, &carry);
    /* One-shot 100 ms timer must fire with a NULL (infinite) timeout. */
    for (i = 0U; i < (uint32_t)sizeof(tchg); i++) {
        ((char *)&tchg)[i] = 0;
    }
    for (i = 0U; i < (uint32_t)sizeof(tev); i++) {
        ((char *)&tev)[i] = 0;
    }
    tchg.ident = 7U;
    tchg.filter = (int16_t)T_EVFILT_TIMER;
    tchg.flags = (uint16_t)((uint32_t)IOS_EV_ADD | (uint32_t)IOS_EV_ONESHOT);
    tchg.data = 100;
    tchg.udata = 0x33333333U;
    carry = 0U;
    rc = ios_bsd6((uint32_t)IOS_SYS_kevent, (uint32_t)kq,
        (uint32_t)(uintptr_t)&tchg, 1U, (uint32_t)(uintptr_t)&tev, 1U, 0U,
        &carry);
    if (carry != 0U || rc != 1) {
        printf("FAIL kqueue timer rc=%d carry=%u\n", rc, carry);
        close(kq);
        close(sv[0]);
        close(sv[1]);
        return 1;
    }
    if (tev.filter != (int16_t)T_EVFILT_TIMER || tev.ident != 7U ||
            tev.udata != 0x33333333U) {
        printf("FAIL kqueue timer event\n");
        close(kq);
        close(sv[0]);
        close(sv[1]);
        return 1;
    }
    close(kq);
    close(sv[0]);
    close(sv[1]);
    return 0;
}

static int check_scm_rights(void)
{
    int sv[2];
    int pf[2];
    uint32_t carry = 0U;
    uint32_t r1 = 0U;
    int32_t rc = 0;
    char payload = 'Q';
    char got = 0;
    t_iov_t siov;
    t_msg_t smsg;
    t_cmsg_t scmsg;
    t_iov_t riov;
    t_msg_t rmsg;
    uint8_t rctl[32];
    int recvd = -1;
    uint32_t i = 0U;
    sv[0] = -1;
    sv[1] = -1;
    pf[0] = -1;
    pf[1] = -1;
    rc = ios_bsd6((uint32_t)IOS_SYS_socketpair, 1U, 1U, 0U,
        (uint32_t)(uintptr_t)sv, 0U, 0U, &carry);
    if (carry != 0U || rc != 0 || sv[0] < 0 || sv[1] < 0) {
        printf("FAIL scm socketpair\n");
        return 1;
    }
    rc = ios_bsd0_r01((uint32_t)IOS_SYS_pipe, &r1, &carry);
    if (carry != 0U || rc < 0) {
        printf("FAIL scm pipe\n");
        close(sv[0]);
        close(sv[1]);
        return 1;
    }
    pf[0] = rc;
    pf[1] = (int)r1;
    siov.base = (uint32_t)(uintptr_t)&payload;
    siov.len = 1U;
    scmsg.cmsg_len = 16U;
    scmsg.cmsg_level = (int32_t)0xFFFFU;
    scmsg.cmsg_type = 0x01;
    memcpy(scmsg.cmsg_data, &pf[0], 4);
    smsg.msg_name = 0U;
    smsg.msg_namelen = 0U;
    smsg.msg_iov = (uint32_t)(uintptr_t)&siov;
    smsg.msg_iovlen = 1;
    smsg.msg_control = (uint32_t)(uintptr_t)&scmsg;
    smsg.msg_controllen = 16U;
    smsg.msg_flags = 0;
    carry = 0U;
    rc = ios_bsd3((uint32_t)T_SYS_sendmsg, (uint32_t)sv[0],
        (uint32_t)(uintptr_t)&smsg, 0U, &carry);
    if (carry != 0U || rc != 1) {
        printf("FAIL scm sendmsg rc=%d carry=%u\n", rc, carry);
        close(pf[0]);
        close(pf[1]);
        close(sv[0]);
        close(sv[1]);
        return 1;
    }
    for (i = 0U; i < (uint32_t)sizeof(rctl); i++) {
        rctl[i] = 0;
    }
    riov.base = (uint32_t)(uintptr_t)&got;
    riov.len = 1U;
    rmsg.msg_name = 0U;
    rmsg.msg_namelen = 0U;
    rmsg.msg_iov = (uint32_t)(uintptr_t)&riov;
    rmsg.msg_iovlen = 1;
    rmsg.msg_control = (uint32_t)(uintptr_t)rctl;
    rmsg.msg_controllen = (uint32_t)sizeof(rctl);
    rmsg.msg_flags = 0;
    carry = 0U;
    rc = ios_bsd3((uint32_t)T_SYS_recvmsg, (uint32_t)sv[1],
        (uint32_t)(uintptr_t)&rmsg, 0U, &carry);
    if (carry != 0U || rc != 1 || got != 'Q' ||
            rmsg.msg_controllen < 16U) {
        printf("FAIL scm recvmsg rc=%d carry=%u c=%u\n", rc, carry,
            rmsg.msg_controllen);
        close(pf[0]);
        close(pf[1]);
        close(sv[0]);
        close(sv[1]);
        return 1;
    }
    {
        uint32_t gl = 0U;
        int32_t lv = 0;
        int32_t tp = 0;
        memcpy(&gl, rctl, 4);
        memcpy(&lv, rctl + 4U, 4);
        memcpy(&tp, rctl + 8U, 4);
        memcpy(&recvd, rctl + 12U, 4);
        if (gl != 16U || lv != (int32_t)0xFFFFU || tp != 0x01 ||
                recvd < 0) {
            printf("FAIL scm cmsg len=%u lv=%d tp=%d fd=%d\n", gl, lv, tp,
                recvd);
            close(pf[0]);
            close(pf[1]);
            close(sv[0]);
            close(sv[1]);
            return 1;
        }
    }
    /* The received fd must be a live alias of the pipe read end. */
    if (write(pf[1], "Z", 1) != 1) {
        printf("FAIL scm pipe write\n");
        close(recvd);
        close(pf[0]);
        close(pf[1]);
        close(sv[0]);
        close(sv[1]);
        return 1;
    }
    {
        char c = 0;
        if (read(recvd, &c, 1) != 1 || c != 'Z') {
            printf("FAIL scm fd alias\n");
            close(recvd);
            close(pf[0]);
            close(pf[1]);
            close(sv[0]);
            close(sv[1]);
            return 1;
        }
    }
    close(recvd);
    close(pf[0]);
    close(pf[1]);
    close(sv[0]);
    close(sv[1]);
    return 0;
}

static int check_sock_poll_kqueue(void)
{
    int sv[2];
    uint32_t carry = 0U;
    int32_t rc = 0;
    int kq = -1;
    ios_pollfd_t pfd;
    ios_kevent32_t chg;
    ios_kevent32_t ev;
    ios_timespec32_t ts;
    uint32_t i = 0U;
    char buf[8];
    int n = 0;
    sv[0] = -1;
    sv[1] = -1;
    rc = ios_bsd6((uint32_t)IOS_SYS_socketpair, 1U, 1U, 0U,
        (uint32_t)(uintptr_t)sv, 0U, 0U, &carry);
    if (carry != 0U || rc != 0) {
        printf("FAIL socketpair rc=%d carry=%u\n", rc, carry);
        return 1;
    }
    if (sv[0] < 0 || sv[1] < 0) {
        printf("FAIL socketpair fds\n");
        return 1;
    }
    if (write(sv[0], "ping!", 5) != 5) {
        printf("FAIL socket write\n");
        close(sv[0]);
        close(sv[1]);
        return 1;
    }
    for (i = 0U; i < (uint32_t)sizeof(pfd); i++) {
        ((char *)&pfd)[i] = 0;
    }
    pfd.fd = (int32_t)sv[1];
    pfd.events = (int16_t)IOS_POLLIN;
    pfd.revents = 0;
    carry = 0U;
    rc = ios_bsd3((uint32_t)IOS_SYS_poll, (uint32_t)(uintptr_t)&pfd,
        1U, 1000U, &carry);
    if (carry != 0U || rc != 1) {
        printf("FAIL poll rc=%d carry=%u\n", rc, carry);
        close(sv[0]);
        close(sv[1]);
        return 1;
    }
    if ((pfd.revents & (int16_t)IOS_POLLIN) == 0) {
        printf("FAIL poll revents=%d\n", pfd.revents);
        close(sv[0]);
        close(sv[1]);
        return 1;
    }
    carry = 0U;
    rc = ios_bsd0((uint32_t)IOS_SYS_kqueue, &carry);
    if (carry != 0U || rc < 0) {
        printf("FAIL kqueue rc=%d\n", rc);
        close(sv[0]);
        close(sv[1]);
        return 1;
    }
    kq = rc;
    for (i = 0U; i < (uint32_t)sizeof(chg); i++) {
        ((char *)&chg)[i] = 0;
    }
    for (i = 0U; i < (uint32_t)sizeof(ev); i++) {
        ((char *)&ev)[i] = 0;
    }
    chg.ident = (uint32_t)sv[1];
    chg.filter = (int16_t)IOS_EVFILT_READ;
    chg.flags = (uint16_t)((uint32_t)IOS_EV_ADD | (uint32_t)IOS_EV_ONESHOT);
    chg.fflags = 0U;
    chg.data = 0;
    chg.udata = 0xCAFE1234U;
    ts.tv_sec = 1;
    ts.tv_nsec = 0;
    carry = 0U;
    rc = ios_bsd6((uint32_t)IOS_SYS_kevent, (uint32_t)kq,
        (uint32_t)(uintptr_t)&chg, 1U, (uint32_t)(uintptr_t)&ev, 1U,
        (uint32_t)(uintptr_t)&ts, &carry);
    if (carry != 0U || rc != 1) {
        printf("FAIL kevent rc=%d carry=%u\n", rc, carry);
        close(kq);
        close(sv[0]);
        close(sv[1]);
        return 1;
    }
    if (ev.ident != (uint32_t)sv[1]) {
        printf("FAIL kevent ident\n");
        close(kq);
        close(sv[0]);
        close(sv[1]);
        return 1;
    }
    if (ev.udata != 0xCAFE1234U) {
        printf("FAIL kevent udata\n");
        close(kq);
        close(sv[0]);
        close(sv[1]);
        return 1;
    }
    for (i = 0U; i < (uint32_t)sizeof(buf); i++) {
        buf[i] = 0;
    }
    n = read(sv[1], buf, 5);
    if (n != 5 || memcmp(buf, "ping!", 5) != 0) {
        printf("FAIL socket read\n");
        close(kq);
        close(sv[0]);
        close(sv[1]);
        return 1;
    }
    close(kq);
    close(sv[0]);
    close(sv[1]);
    return 0;
}

static int check_pipe_fork_wait(void)
{
    uint32_t r1 = 0U;
    uint32_t carry = 0U;
    int32_t pr = 0;
    int rfd = -1;
    int wfd = -1;
    int32_t pid = 0;
    uint32_t fr1 = 0U;
    uint32_t fcarry = 0U;
    pr = ios_bsd0_r01((uint32_t)IOS_SYS_pipe, &r1, &carry);
    if (carry != 0U) {
        printf("FAIL pipe carry\n");
        return 1;
    }
    rfd = pr;
    wfd = (int)r1;
    if (rfd < 0 || wfd < 0) {
        printf("FAIL pipe fds\n");
        return 1;
    }
    pid = ios_bsd0_r01((uint32_t)IOS_SYS_fork, &fr1, &fcarry);
    if (fcarry != 0U) {
        printf("FAIL fork\n");
        close(rfd);
        close(wfd);
        return 1;
    }
    if (pid == 0) {
        if (fr1 != 1U) {
            _exit(98);
        }
        close(rfd);
        if (write(wfd, "child-ok", 8) != 8) {
            _exit(97);
        }
        close(wfd);
        _exit(19);
    }
    close(wfd);
    {
        char buf[8];
        uint32_t i = 0U;
        int got = 0;
        int status = 0;
        int32_t w = 0;
        for (i = 0U; i < (uint32_t)sizeof(buf); i++) {
            buf[i] = 0;
        }
        while (got < 8) {
            int n = read(rfd, buf + got, (size_t)(8 - got));
            if (n <= 0) {
                break;
            }
            got += n;
        }
        close(rfd);
        if (got != 8 || memcmp(buf, "child-ok", 8) != 0) {
            printf("FAIL pipe read got=%d\n", got);
            return 1;
        }
        carry = 0U;
        w = ios_bsd6((uint32_t)IOS_SYS_wait4, (uint32_t)pid,
            (uint32_t)(uintptr_t)&status, 0U, 0U, 0U, 0U, &carry);
        if (carry != 0U || w != pid) {
            printf("FAIL wait4\n");
            return 1;
        }
        if ((status & 0x7F) != 0) {
            printf("FAIL wait4 signal status=%d\n", status);
            return 1;
        }
        if (((status >> 8) & 0xFF) != 19) {
            printf("FAIL wait4 exit status=%d\n", status);
            return 1;
        }
    }
    return 0;
}

static int check_fork_exec(const char *exec_path, char **envp)
{
    uint32_t fr1 = 0U;
    uint32_t carry = 0U;
    int32_t pid = 0;
    pid = ios_bsd0_r01((uint32_t)IOS_SYS_fork, &fr1, &carry);
    if (carry != 0U) {
        printf("FAIL fork exec\n");
        return 1;
    }
    if (pid == 0) {
        char a0[] = "custom-ios-child";
        char a1[] = "--exec-child";
        char *child_argv[3];
        char **e = envp;
        child_argv[0] = a0;
        child_argv[1] = a1;
        child_argv[2] = 0;
        if (e == 0) {
            e = environ;
        }
        carry = 0U;
        ios_bsd6((uint32_t)IOS_SYS_execve,
            (uint32_t)(uintptr_t)exec_path,
            (uint32_t)(uintptr_t)child_argv,
            (uint32_t)(uintptr_t)e, 0U, 0U, 0U, &carry);
        _exit(99);
    }
    {
        int status = 0;
        int32_t w = 0;
        carry = 0U;
        w = ios_bsd6((uint32_t)IOS_SYS_wait4, (uint32_t)pid,
            (uint32_t)(uintptr_t)&status, 0U, 0U, 0U, 0U, &carry);
        if (carry != 0U || w != pid) {
            printf("FAIL wait4 exec\n");
            return 1;
        }
        if ((status & 0x7F) != 0) {
            printf("FAIL exec signal status=%d\n", status);
            return 1;
        }
        if (((status >> 8) & 0xFF) != 42) {
            printf("FAIL exec exit status=%d\n", status);
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv, char **envp)
{
    int i = 0;
    int no_fork = 0;
    const char *exec_path = 0;
    char **e = 0;
    if (argc >= 2 && argv != 0 && argv[1] != 0 &&
        strcmp(argv[1], "--exec-child") == 0) {
        if (argv[0] == 0 || strcmp(argv[0], "custom-ios-child") != 0) {
            printf("FAIL exec-child argv0\n");
            return 1;
        }
        _exit(42);
    }
    for (i = 1; i < argc; i++) {
        if (argv[i] != 0 && strcmp(argv[i], "--no-fork") == 0) {
            no_fork = 1;
        }
    }
    if (check_rpath() != 0) {
        return 1;
    }
    if (check_altstack() != 0) {
        return 1;
    }
    if (check_sock_poll_kqueue() != 0) {
        return 1;
    }
    if (check_sysctl_short_buffer() != 0) {
        return 1;
    }
    if (check_kqueue_read_write_timer() != 0) {
        return 1;
    }
    if (check_scm_rights() != 0) {
        return 1;
    }
    if (no_fork == 0) {
        if (check_pipe_fork_wait() != 0) {
            return 1;
        }
        if (argc < 1 || argv == 0 || argv[0] == 0) {
            printf("FAIL exec-path missing\n");
            return 1;
        }
        exec_path = argv[0];
        e = envp;
        if (e == 0) {
            e = environ;
        }
        if (check_fork_exec(exec_path, e) != 0) {
            return 1;
        }
    }
    printf("PASS test_ios_proc_net\n");
    return 0;
}
