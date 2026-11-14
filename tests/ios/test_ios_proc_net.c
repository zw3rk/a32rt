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
