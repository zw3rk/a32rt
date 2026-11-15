/*
 * src/ios/ios_svc_bsd_proc.c — iOS BSD syscalls: process, identity, time, mux, sysctl, probes
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#if !defined(__APPLE__)
#define _GNU_SOURCE
#else
#define _DARWIN_C_SOURCE
#endif

#include "a32rt.h"
#include "ios/ios.h"
#include "vm/vm32.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#if !defined(PATH_MAX)
#define PATH_MAX 4096
#endif

/* Darwin errno values used on the guest return path. */
#define D_EPERM 1
#define D_ENOENT 2
#define D_ESRCH 3
#define D_EINTR 4
#define D_EFAULT 14
#define D_EINVAL 22
#define D_ENOMEM 12
#define D_EAGAIN 35
#define D_ENAMETOOLONG 63
#define D_ENOSYS 78
#define D_EBADF 9

#define D_RLIM_INF 0x7FFFFFFFFFFFFFFFULL

extern char **environ;

/* ---- Guest memory helpers ---- */

static int host_fail(a32_ctx_t *ctx)
{
    a32_ios_bsd_err(ctx, a32_ios_host_to_darwin_errno(errno));
    return 0;
}

static bool g_read(a32_ctx_t *ctx, void *dst, uint32_t va, size_t len)
{
    if (len == 0U) {
        return true;
    }
    if (len > 0x1000000U || dst == NULL) {
        return false;
    }
    if (!a32_guest_range_ok(ctx, va, (uint32_t)len, (uint32_t)PROT_READ)) {
        return false;
    }
    memcpy(dst, a32_guest_ptr(ctx, va), len);
    return true;
}

static bool g_write(a32_ctx_t *ctx, uint32_t va, const void *src, size_t len)
{
    if (len == 0U) {
        return true;
    }
    if (len > 0x1000000U || src == NULL) {
        return false;
    }
    if (!a32_guest_range_ok(ctx, va, (uint32_t)len, (uint32_t)PROT_WRITE)) {
        return false;
    }
    memcpy(a32_guest_ptr(ctx, va), src, len);
    a32_vm32_copyout_invalidate_exec(ctx, va, len);
    return true;
}

/* Bounded guest C string copy. Returns length or negative Darwin errno. */
static int g_cstr(a32_ctx_t *ctx, uint32_t va, char *out, size_t cap)
{
    size_t pos = 0U;
    uint32_t cur = va;
    if (cap == 0U || out == NULL) {
        return -D_EINVAL;
    }
    for (;;) {
        uint32_t rem = 0x1000U - (cur & 0xFFFU);
        const char *blk;
        size_t want;
        const char *hit;
        if (pos + 1U >= cap) {
            return -D_ENAMETOOLONG;
        }
        if (!a32_guest_range_ok(ctx, cur, rem, (uint32_t)PROT_READ)) {
            return -D_EFAULT;
        }
        blk = (const char *)a32_guest_ptr(ctx, cur);
        want = cap - pos - 1U;
        if (want > rem) {
            want = rem;
        }
        hit = (const char *)memchr(blk, 0, want);
        if (hit != NULL) {
            size_t k = (size_t)(hit - blk);
            memcpy(out + pos, blk, k + 1U);
            return (int)(pos + k);
        }
        if (pos + rem >= cap) {
            return -D_ENAMETOOLONG;
        }
        memcpy(out + pos, blk, rem);
        pos += rem;
        cur += rem;
    }
}

/* Concise checked guest access: fault the guest syscall on failure. */
#define P_RD(d_, va_, n_) do { if (!g_read(ctx, (d_), (va_), (n_))) { a32_ios_bsd_err(ctx, D_EFAULT); return 0; } } while (0)
#define P_WR(va_, p_, n_) do { if (!g_write(ctx, (va_), (p_), (n_))) { a32_ios_bsd_err(ctx, D_EFAULT); return 0; } } while (0)
#define P_DIE(e_) do { a32_ios_bsd_err(ctx, (e_)); return 0; } while (0)

/* ---- Process lifecycle: exit / fork / vfork / wait4 ---- */

static int do_fork_child(a32_ctx_t *ctx, const sigset_t *saved)
{
    a32_vm_fork_child_reset();
    a32_vmfile_fork_child_reset();
    a32_ios_kq_atfork_reset();
    a32_vm32_ll_clear_owner(ctx);
    ctx->host_tid = 0;
    ctx->is_thread_clone = false;
    ctx->clear_child_tid = 0U;
    ctx->robust_list_head = 0U;
    ctx->robust_list_len = 0U;
    ctx->ll_epoch_snap = 0ULL;
    ctx->excl_valid = false;
    ctx->pending_signals = 0ULL;
    a32_signals_set_thread_ctx(ctx);
    (void)pthread_sigmask(SIG_SETMASK, saved, NULL);
    a32_ios_refresh_runtime_pid(ctx);
    /* XNU fork dual-return convention: child sees r0 = 0, r1 = 1. */
    a32_ios_bsd_ok(ctx, 0U, 1U, true);
    return 0;
}

/* Serves fork(2) and vfork(66): vfork suspends the parent until exec/exit on
 * XNU, but plain fork is a safe superset for guests that vfork+exec. */
static int do_fork(a32_ctx_t *ctx)
{
    sigset_t all, saved;
    pid_t pid;
    (void)sigfillset(&all);
    (void)pthread_sigmask(SIG_BLOCK, &all, &saved);
    pid = fork();
    if (pid == 0) {
        return do_fork_child(ctx, &saved);
    }
    (void)pthread_sigmask(SIG_SETMASK, &saved, NULL);
    if (pid < 0) {
        return host_fail(ctx);
    }
    a32_ios_bsd_ok(ctx, (uint32_t)pid, 0U, true);
    return 0;
}

static void host_rusage_to_darwin(const struct rusage *in, uint8_t out[72])
{
    int32_t v[18];
    v[0] = (int32_t)in->ru_utime.tv_sec;
    v[1] = (int32_t)in->ru_utime.tv_usec;
    v[2] = (int32_t)in->ru_stime.tv_sec;
    v[3] = (int32_t)in->ru_stime.tv_usec;
    v[4] = (int32_t)in->ru_maxrss;
    v[5] = (int32_t)in->ru_ixrss;
    v[6] = (int32_t)in->ru_idrss;
    v[7] = (int32_t)in->ru_isrss;
    v[8] = (int32_t)in->ru_minflt;
    v[9] = (int32_t)in->ru_majflt;
    v[10] = (int32_t)in->ru_nswap;
    v[11] = (int32_t)in->ru_inblock;
    v[12] = (int32_t)in->ru_oublock;
    v[13] = (int32_t)in->ru_msgsnd;
    v[14] = (int32_t)in->ru_msgrcv;
    v[15] = (int32_t)in->ru_nsignals;
    v[16] = (int32_t)in->ru_nvcsw;
    v[17] = (int32_t)in->ru_nivcsw;
    memcpy(out, v, sizeof(v));
}

static int do_wait4(a32_ctx_t *ctx, const uint32_t *args)
{
    pid_t pid = (pid_t)(int32_t)args[0];
    uint32_t sva = args[1];
    int opts = (int)(args[2] & 3U); /* WNOHANG=1, WUNTRACED=2 on both */
    uint32_t rva = args[3];
    struct rusage ru;
    int status = 0;
    pid_t r;
    memset(&ru, 0, sizeof(ru));
    r = wait4(pid, &status, opts, (rva != 0U) ? &ru : NULL);
    if (r < 0) {
        return host_fail(ctx);
    }
    if (sva != 0U) {
        int32_t s = (int32_t)status;
        if (!g_write(ctx, sva, &s, sizeof(s))) {
            a32_ios_bsd_err(ctx, D_EFAULT);
            return 0;
        }
    }
    if (rva != 0U) {
        uint8_t buf[72];
        host_rusage_to_darwin(&ru, buf);
        if (!g_write(ctx, rva, buf, sizeof(buf))) {
            a32_ios_bsd_err(ctx, D_EFAULT);
            return 0;
        }
    }
    a32_ios_bsd_ok(ctx, (uint32_t)r, 0U, false);
    return 0;
}

/* ---- Exit & exec ---- */

static int do_exit(a32_ctx_t *ctx, const uint32_t *args)
{
    a32_ios_exit_teardown(ctx, (int)args[0]);
    return 0;
}

/* True once a NUL byte is seen within readable guest pages (<= 256 KiB). */
static bool str_ok(a32_ctx_t *ctx, uint32_t va)
{
    uint32_t i;
    for (i = 0U; i < 262144U; i++) {
        char c = 0;
        if (!g_read(ctx, &c, va + i, 1U)) { return false; }
        if (c == 0) { return true; }
    }
    return false;
}

/* Guest argv/envp vectors hold host-usable pointers into the mapped window;
 * only the slots and NUL-termination are validated here. */
static bool rd_strv(a32_ctx_t *ctx, uint32_t vec, char **out, int max, int *n)
{
    int c = 0;
    if (n != NULL) {
        *n = 0;
    }
    if (vec == 0U) {
        out[0] = NULL;
        return true;
    }
    for (;;) {
        uint32_t w = 0U;
        if (!g_read(ctx, &w, vec + (uint32_t)c * 4U, 4U)) { return false; }
        if (w == 0U) { break; }
        if (c >= max || !str_ok(ctx, w)) { return false; }
        out[c++] = (char *)a32_guest_ptr(ctx, w);
    }
    out[c] = NULL;
    if (n != NULL) {
        *n = c;
    }
    return true;
}

static int do_execve(a32_ctx_t *ctx, const uint32_t *args)
{
    char guest[PATH_MAX];
    char resolved[PATH_MAX];
    char self[PATH_MAX];
    char *av[130];
    char *ev[257];
    char *ra[140];
    char **envp;
    char fd_buf[16];
    int ac = 0;
    int fd;
    int rp = 0;
    int i;
    bool macho;
    bool elf = false;
    if (g_cstr(ctx, args[0], guest, sizeof(guest)) < 0) { P_DIE(D_EFAULT); }
    if (a32_resolve_guest_path(ctx, guest, resolved, sizeof(resolved)) != 0) { P_DIE(D_ENOENT); }
    if (!rd_strv(ctx, args[1], av, 129, &ac) || !rd_strv(ctx, args[2], ev, 256, NULL)) { P_DIE(D_EFAULT); }
    if (ac == 0) {
        av[0] = resolved;
        av[1] = NULL;
        ac = 1;
    }
    envp = (args[2] != 0U) ? ev :
        (ctx->host_envp != NULL ? ctx->host_envp : environ);
    fd = open(resolved, O_RDONLY | O_CLOEXEC);
    macho = a32_ios_is_macho_fd(fd, resolved, ctx);
    if (fd >= 0) {
        uint8_t hdr[20];
        if (a32_ios_pread_full(fd, hdr, sizeof hdr, 0) == 0 &&
            hdr[0] == 0x7FU && hdr[1] == (uint8_t)'E' &&
            hdr[2] == (uint8_t)'L' && hdr[3] == (uint8_t)'F' &&
            hdr[4] == 1u && hdr[5] == 1u &&
            hdr[18] == 40u && hdr[19] == 0u) {
            uint16_t et = (uint16_t)hdr[16] | (uint16_t)((uint16_t)hdr[17] << 8);
            if (et == 2u || et == 3u) {
                elf = true;
            }
        }
    }
    if (!macho && !elf) {
        if (fd >= 0) {
            close(fd);
        }
        execve(resolved, av, envp);
        return host_fail(ctx);
    }
    if (fd < 0) { P_DIE(D_ENOENT); }
    (void)fcntl(fd, F_SETFD, 0);
    snprintf(fd_buf, sizeof fd_buf, "%d", fd);
#if defined(__APPLE__)
    {
        uint32_t sz = (uint32_t)sizeof(self);
        if (_NSGetExecutablePath(self, &sz) != 0) {
            (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
            close(fd);
            P_DIE(D_ENOENT);
        }
    }
#else
    {
        ssize_t sn = readlink("/proc/self/exe", self, sizeof(self) - 1U);
        if (sn <= 0) {
            (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
            close(fd);
            return host_fail(ctx);
        }
        self[sn] = '\0';
    }
#endif
    ra[rp++] = self;
    if (ctx->sysroot != NULL && ctx->sysroot[0] != '\0') {
        ra[rp++] = (char *)"--sysroot";
        ra[rp++] = (char *)ctx->sysroot;
    }
    ra[rp++] = (char *)"--image-fd";
    ra[rp++] = fd_buf;
    ra[rp++] = (char *)"--execfn";
    ra[rp++] = resolved;
    ra[rp++] = (char *)"--";
    for (i = 0; i < ac && rp < 139; i++) {
        ra[rp++] = av[i];
    }
    ra[rp] = NULL;
    execve(self, ra, envp);
    {
        int saved = errno;
        (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
        close(fd);
        errno = saved;
        return host_fail(ctx);
    }
}

/* ---- Identity, groups, sessions, limits ---- */

static int ok_or_fail(a32_ctx_t *ctx, int r, uint32_t v)
{
    if (r != 0) {
        return host_fail(ctx);
    }
    a32_ios_bsd_ok(ctx, v, 0U, false);
    return 0;
}

static int ok_pid(a32_ctx_t *ctx, pid_t r)
{
    if (r < 0) {
        return host_fail(ctx);
    }
    a32_ios_bsd_ok(ctx, (uint32_t)r, 0U, false);
    return 0;
}

static int do_getgroups(a32_ctx_t *ctx, const uint32_t *args)
{
    int n = (int)args[0];
    gid_t *g;
    int r;
    if (n < 0) { P_DIE(D_EINVAL); }
    if (n == 0 || args[1] == 0U) {
        r = getgroups(0, NULL);
        if (r < 0) { return host_fail(ctx); }
        a32_ios_bsd_ok(ctx, (uint32_t)r, 0U, false);
        return 0;
    }
    g = (gid_t *)malloc((size_t)n * sizeof(*g));
    if (g == NULL) { P_DIE(D_ENOMEM); }
    r = getgroups(n, g);
    if (r < 0) {
        free(g);
        return host_fail(ctx);
    }
    if (!g_write(ctx, args[1], g, (size_t)r * sizeof(*g))) {
        free(g);
        P_DIE(D_EFAULT);
    }
    free(g);
    a32_ios_bsd_ok(ctx, (uint32_t)r, 0U, false);
    return 0;
}

static int do_setgroups(a32_ctx_t *ctx, const uint32_t *args)
{
    int n = (int)args[0];
    gid_t *g;
    int r;
    if (n < 0 || n > 512) { P_DIE(D_EINVAL); }
    if (n == 0) { return ok_or_fail(ctx, setgroups(0, NULL), 0U); }
    g = (gid_t *)malloc((size_t)n * sizeof(*g));
    if (g == NULL) { P_DIE(D_ENOMEM); }
    if (!g_read(ctx, g, args[1], (size_t)n * sizeof(*g))) {
        free(g);
        P_DIE(D_EFAULT);
    }
    r = setgroups((size_t)n, g);
    free(g);
    return ok_or_fail(ctx, r, 0U);
}

static int do_getrusage(a32_ctx_t *ctx, const uint32_t *args)
{
    struct rusage ru;
    uint8_t buf[72];
    if (getrusage((int)args[0], &ru) != 0) {
        return host_fail(ctx);
    }
    if (args[1] == 0U) {
        a32_ios_bsd_ok(ctx, 0U, 0U, false);
        return 0;
    }
    host_rusage_to_darwin(&ru, buf);
    P_WR(args[1], buf, sizeof(buf));
    a32_ios_bsd_ok(ctx, 0U, 0U, false);
    return 0;
}

/* Darwin numbers: CPU 0, FSIZE 1, DATA 2, STACK 3, CORE 4, AS 5,
 * RSS/MEMLOCK 6, NPROC 7, NOFILE 8. Linux permutes AS/RSS/NPROC/NOFILE,
 * so translate via <sys/resource.h> symbols instead of hardcoding. */
static int rl_host(int d)
{
    /* Darwin libSystem ORs _RLIMIT_POSIX_FLAG (0x1000) into resource. */
    d &= ~0x1000;
#if defined(__APPLE__)
    return d;
#elif defined(__linux__)
    switch (d) {
    case 0: return RLIMIT_CPU;
    case 1: return RLIMIT_FSIZE;
    case 2: return RLIMIT_DATA;
    case 3: return RLIMIT_STACK;
    case 4: return RLIMIT_CORE;
    case 5: return RLIMIT_AS;
    case 6: return RLIMIT_MEMLOCK;
    case 7: return RLIMIT_NPROC;
    case 8: return RLIMIT_NOFILE;
    default: return d;
    }
#else
    return d;
#endif
}

static int do_getrlimit(a32_ctx_t *ctx, const uint32_t *args)
{
    struct rlimit rl;
    uint64_t o[2];
    if (getrlimit(rl_host((int)args[0]), &rl) != 0) {
        return host_fail(ctx);
    }
    o[0] = (rl.rlim_cur == RLIM_INFINITY) ? D_RLIM_INF : (uint64_t)rl.rlim_cur;
    o[1] = (rl.rlim_max == RLIM_INFINITY) ? D_RLIM_INF : (uint64_t)rl.rlim_max;
    if (args[1] != 0U) { P_WR(args[1], o, sizeof(o)); }
    a32_ios_bsd_ok(ctx, 0U, 0U, false);
    return 0;
}

static int do_setrlimit(a32_ctx_t *ctx, const uint32_t *args)
{
    uint64_t v[2];
    struct rlimit rl;
    if (args[1] == 0U) { P_DIE(D_EFAULT); }
    P_RD(v, args[1], sizeof(v));
    rl.rlim_cur = (v[0] == D_RLIM_INF) ? RLIM_INFINITY : (rlim_t)v[0];
    rl.rlim_max = (v[1] == D_RLIM_INF) ? RLIM_INFINITY : (rlim_t)v[1];
    return ok_or_fail(ctx, setrlimit(rl_host((int)args[0]), &rl), 0U);
}

/* ---- Time & multiplexing ---- */

static int do_gettimeofday(a32_ctx_t *ctx, const uint32_t *args)
{
    static const int32_t kZero[2] = { 0, 0 };
    struct timeval tv;
    int32_t o[2];
    if (gettimeofday(&tv, NULL) != 0) {
        return host_fail(ctx);
    }
    o[0] = (int32_t)tv.tv_sec;
    o[1] = (int32_t)tv.tv_usec;
    if (args[0] != 0U) { P_WR(args[0], o, sizeof(o)); }
    if (args[1] != 0U) { P_WR(args[1], kZero, sizeof(kZero)); }
    a32_ios_bsd_ok(ctx, 0U, 0U, false);
    return 0;
}

/* XNU fd_set is 1024 bits like Linux; on LE the covered bytes match, so
 * they are copied verbatim. Guest timeval is 2x int32. */
static int do_select(a32_ctx_t *ctx, const uint32_t *args)
{
    int nfds = (int)args[0];
    uint32_t nB;
    fd_set ss[3];
    struct timeval tv;
    struct timeval *tp = NULL;
    uint32_t i;
    int r;
    if (nfds < 0 || nfds > 1024) { P_DIE(D_EINVAL); }
    nB = (uint32_t)(((nfds + 63) / 64) * 8);
    for (i = 0U; i < 3U; i++) {
        FD_ZERO(&ss[i]);
        if (args[1U + i] != 0U) { P_RD(&ss[i], args[1U + i], nB); }
    }
    if (args[4] != 0U) {
        int32_t t[2];
        P_RD(t, args[4], sizeof(t));
        tv.tv_sec = (time_t)t[0];
        tv.tv_usec = (suseconds_t)t[1];
        tp = &tv;
    }
    r = select(nfds, (args[1] != 0U) ? &ss[0] : NULL,
        (args[2] != 0U) ? &ss[1] : NULL, (args[3] != 0U) ? &ss[2] : NULL, tp);
    if (r < 0) { return host_fail(ctx); }
    for (i = 0U; i < 3U; i++) {
        if (args[1U + i] != 0U) { P_WR(args[1U + i], &ss[i], nB); }
    }
    a32_ios_bsd_ok(ctx, (uint32_t)r, 0U, false);
    return 0;
}

/* Guest pollfd is 8B {int, short, short} like the host; poll runs in place. */
static int do_poll(a32_ctx_t *ctx, const uint32_t *args)
{
    uint32_t nf = args[1];
    int r;
    if (nf > 4096U) { P_DIE(D_EINVAL); }
    if (nf == 0U) {
        r = poll(NULL, 0, (int)args[2]);
    } else {
        uint32_t nB = nf * 8U;
        if (!a32_guest_range_ok(ctx, args[0], nB, (uint32_t)(PROT_READ | PROT_WRITE))) { P_DIE(D_EFAULT); }
        r = poll((struct pollfd *)a32_guest_ptr(ctx, args[0]), (nfds_t)nf, (int)args[2]);
        a32_vm32_copyout_invalidate_exec(ctx, args[0], nB);
    }
    if (r < 0) { return host_fail(ctx); }
    a32_ios_bsd_ok(ctx, (uint32_t)r, 0U, false);
    return 0;
}

/* ---- Deterministic sysctl table ---- */

typedef struct {
    const char *name;
    int32_t m0; /* First oid level, or -1 for name-only entries. */
    int32_t m1;
    uint32_t kind; /* 0 = string, 1 = int32, 2 = uint32, 3 = uint64. */
    uint64_t u;
    const char *s;
} sysc_t;

static const sysc_t kSysctl[] = {
    { "kern.ostype", 1, 1, 0U, 0ULL, "Darwin" }, { "kern.osrelease", 1, 2, 0U, 0ULL, "16.7.0" },
    { "kern.version", 1, 4, 0U, 0ULL, "Darwin Kernel Version 19.6.0" }, { "kern.argmax", 1, 8, 1U, 262144ULL, NULL },
    { "kern.hostname", 1, 10, 0U, 0ULL, "iphone" }, { "kern.usrstack", 1, 35, 2U, 0xE0000000ULL, NULL },
    { "kern.osversion", 1, 65, 0U, 0ULL, "14G61" }, { "kern.osproductversion", -1, -1, 0U, 0ULL, "10.3.4" },
    { "hw.machine", 6, 1, 0U, 0ULL, "iPhone5,2" }, { "hw.model", 6, 2, 0U, 0ULL, "N42AP" },
    { "hw.ncpu", 6, 3, 1U, 4ULL, NULL }, { "hw.byteorder", 6, 4, 1U, 1234ULL, NULL },
    { "hw.pagesize", 6, 7, 1U, 4096ULL, NULL }, { "hw.cachelinesize", 6, 16, 1U, 64ULL, NULL },
    { "hw.memsize", 6, 24, 3U, 1073741824ULL, NULL }, { "hw.availcpu", 6, 25, 1U, 4ULL, NULL },
    { "hw.logicalcpu", -1, -1, 1U, 4ULL, NULL }, { "hw.physicalcpu", -1, -1, 1U, 4ULL, NULL },
    { "hw.cputype", -1, -1, 1U, 12ULL, NULL }, { "hw.cpusubtype", -1, -1, 1U, 11ULL, NULL },
    { "hw.optional.neon", -1, -1, 1U, 1ULL, NULL },
};

static const sysc_t *sysc_by_name(const char *nm)
{
    size_t i;
    for (i = 0U; i < sizeof(kSysctl) / sizeof(kSysctl[0]); i++) {
        if (strcmp(kSysctl[i].name, nm) == 0) {
            return &kSysctl[i];
        }
    }
    return NULL;
}

static const sysc_t *sysc_by_oid(int32_t a, int32_t b)
{
    size_t i;
    for (i = 0U; i < sizeof(kSysctl) / sizeof(kSysctl[0]); i++) {
        if (kSysctl[i].m0 == a && kSysctl[i].m1 == b) {
            return &kSysctl[i];
        }
    }
    return NULL;
}

/* Length words are 32-bit guest size_t. NULL oldp sizes via *oldlenp. */
static int sysc_emit(a32_ctx_t *ctx, const sysc_t *e, uint32_t op, uint32_t olp)
{
    uint8_t buf[64];
    uint32_t need;
    uint32_t avail = 0U;
    if (e->kind == 0U) {
        need = (uint32_t)strlen(e->s) + 1U;
        if (need > sizeof(buf)) {
            need = (uint32_t)sizeof(buf);
        }
        memcpy(buf, e->s, need);
    } else if (e->kind == 3U) {
        need = 8U;
        memcpy(buf, &e->u, need);
    } else {
        uint32_t v = (uint32_t)e->u;
        need = 4U;
        memcpy(buf, &v, need);
    }
    if (olp == 0U) {
        if (op != 0U) { P_DIE(D_EFAULT); }
        a32_ios_bsd_ok(ctx, 0U, 0U, false);
        return 0;
    }
    P_RD(&avail, olp, 4U);
    if (op == 0U) {
        P_WR(olp, &need, 4U);
        a32_ios_bsd_ok(ctx, 0U, 0U, false);
        return 0;
    }
    if (avail < need) {
        /* Darwin sysctl(3): short buffer copies what fits, reports the
         * required length, and fails with ENOMEM. */
        if (avail > 0U) { P_WR(op, buf, avail); }
        P_WR(olp, &need, 4U);
        a32_ios_bsd_err(ctx, D_ENOMEM);
        return 0;
    }
    P_WR(op, buf, need);
    P_WR(olp, &need, 4U);
    a32_ios_bsd_ok(ctx, 0U, 0U, false);
    return 0;
}

/* Shared core: resolve nm (length wl, not necessarily NUL-terminated),
 * reject writes (read-only table), emit via sysc_emit. */
static int sysc_named(a32_ctx_t *ctx, uint32_t nva, uint32_t nl,
    uint32_t op, uint32_t olp, uint32_t np)
{
    char nm[128];
    const sysc_t *e;
    if (nva == 0U || nl == 0U || nl >= sizeof(nm)) { P_DIE(D_EINVAL); }
    P_RD(nm, nva, nl);
    nm[nl] = '\0';
    e = sysc_by_name(nm);
    if (e == NULL) { P_DIE(D_ENOENT); }
    if (np != 0U) { P_DIE(D_EPERM); }
    return sysc_emit(ctx, e, op, olp);
}

static int do_sysctl(a32_ctx_t *ctx, const uint32_t *args)
{
    uint32_t nl = args[1];
    int32_t mib[8];
    const sysc_t *e;
    if (nl < 2U || nl > 8U) { P_DIE(D_EINVAL); }
    P_RD(mib, args[0], nl * 4U);
    if (mib[0] == 0 && mib[1] == 3) {
        /* sysctl.name2oid: name bytes in newp/newlen, two mib ints to oldp. */
        char nm[128];
        uint32_t wl = args[5];
        uint32_t avail = 0U;
        uint32_t need = 8U;
        int32_t om[2];
        const sysc_t *ne;
        if (args[4] == 0U || wl == 0U || wl >= sizeof(nm) || args[3] == 0U) { P_DIE(D_EINVAL); }
        P_RD(nm, args[4], wl);
        P_RD(&avail, args[3], 4U);
        nm[wl] = '\0';
        ne = sysc_by_name(nm);
        if (ne == NULL || ne->m0 < 0) { P_DIE(D_ENOENT); }
        om[0] = ne->m0;
        om[1] = ne->m1;
        if (args[2] != 0U) {
            if (avail < need) {
                if (avail > 0U) { P_WR(args[2], om, avail); }
                P_WR(args[3], &need, 4U);
                a32_ios_bsd_err(ctx, D_ENOMEM);
                return 0;
            }
            P_WR(args[2], om, sizeof(om));
        }
        P_WR(args[3], &need, 4U);
        a32_ios_bsd_ok(ctx, 0U, 0U, false);
        return 0;
    }
    e = sysc_by_oid(mib[0], mib[1]);
    if (e == NULL) { P_DIE(D_ENOENT); }
    if (args[4] != 0U) { P_DIE(D_EPERM); }
    return sysc_emit(ctx, e, args[2], args[3]);
}

static int do_sysctlbyname(a32_ctx_t *ctx, const uint32_t *args)
{
    return sysc_named(ctx, args[0], args[1], args[2], args[3], args[4]);
}

/* ---- XNU probes & randomness ---- */

static int do_csops(a32_ctx_t *ctx, const uint32_t *args)
{
    /* CS_OPS_STATUS (0) reports CS_VALID (4) for translated images. */
    if (args[1] == 0U && args[2] != 0U) {
        int32_t f = 4;
        P_WR(args[2], &f, sizeof(f));
    }
    a32_ios_bsd_ok(ctx, 0U, 0U, false);
    return 0;
}

static int do_proc_info(a32_ctx_t *ctx, const uint32_t *args)
{
    const char *p = (ctx->exec_path != NULL) ? ctx->exec_path : "";
    size_t n = strlen(p) + 1U;
    uint32_t k;
    if (args[0] != 2U || args[2] != 11U) { P_DIE(D_EINVAL); } /* PROC_PIDPATHINFO only */
    if (args[4] == 0U || args[5] == 0U) { P_DIE(D_EFAULT); }
    k = (n < (size_t)args[5]) ? (uint32_t)n : args[5];
    P_WR(args[4], p, k);
    a32_ios_bsd_ok(ctx, k, 0U, false);
    return 0;
}

static int do_getentropy(a32_ctx_t *ctx, const uint32_t *args)
{
    uint32_t n = args[1];
    uint32_t got = 0U;
    int fd;
    if (n > 256U) {
        a32_ios_bsd_err(ctx, 5); /* Darwin EIO */
        return 0;
    }
    if (n == 0U) {
        a32_ios_bsd_ok(ctx, 0U, 0U, false);
        return 0;
    }
    if (!a32_guest_range_ok(ctx, args[0], n, (uint32_t)PROT_WRITE)) {
        a32_ios_bsd_err(ctx, D_EFAULT);
        return 0;
    }
    fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return host_fail(ctx);
    }
    while (got < n) {
        ssize_t r = read(fd, (char *)a32_guest_ptr(ctx, args[0] + got),
            (size_t)(n - got));
        if (r <= 0) {
            close(fd);
            return host_fail(ctx);
        }
        got += (uint32_t)r;
    }
    close(fd);
    a32_vm32_copyout_invalidate_exec(ctx, args[0], n);
    a32_ios_bsd_ok(ctx, 0U, 0U, false);
    return 0;
}

static int do_abort_payload(a32_ctx_t *ctx, const uint32_t *args)
{
    (void)ctx;
    (void)args;
    fflush(NULL);
    _exit(134);
    return 0;
}

/* ---- Public BSD process/network dispatcher ---- */

int a32_ios_dispatch_bsd_proc_net(a32_ctx_t *ctx, uint32_t bsd_nr,
    const uint32_t *args, bool *out_handled)
{
    if (out_handled != NULL) {
        *out_handled = true;
    }
    switch (bsd_nr) {
    case 1U: return do_exit(ctx, args);
    case 2U: case 66U: return do_fork(ctx);
    case 7U: case 400U: return do_wait4(ctx, args);
    case 59U: return do_execve(ctx, args);
    case 20U: a32_ios_bsd_ok(ctx, (uint32_t)getpid(), 0U, false); return 0;
    case 39U: a32_ios_bsd_ok(ctx, (uint32_t)getppid(), 0U, false); return 0;
    case 23U: return ok_or_fail(ctx, setuid((uid_t)args[0]), 0U);
    case 24U: a32_ios_bsd_ok(ctx, (uint32_t)getuid(), 0U, false); return 0;
    case 25U: a32_ios_bsd_ok(ctx, (uint32_t)geteuid(), 0U, false); return 0;
    case 43U: a32_ios_bsd_ok(ctx, (uint32_t)getegid(), 0U, false); return 0;
    case 47U: a32_ios_bsd_ok(ctx, (uint32_t)getgid(), 0U, false); return 0;
    case 181U: return ok_or_fail(ctx, setgid((gid_t)args[0]), 0U);
    case 79U: return do_getgroups(ctx, args);
    case 80U: return do_setgroups(ctx, args);
    case 81U: a32_ios_bsd_ok(ctx, (uint32_t)getpgrp(), 0U, false); return 0;
    case 82U: return ok_or_fail(ctx, setpgid((pid_t)(int32_t)args[0], (pid_t)(int32_t)args[1]), 0U);
    case 147U: return ok_pid(ctx, setsid());
    case 151U: return ok_pid(ctx, getpgid((pid_t)(int32_t)args[0]));
    case 310U: return ok_pid(ctx, getsid((pid_t)(int32_t)args[0]));
    case 327U: a32_ios_bsd_ok(ctx, 0U, 0U, false); return 0;
    case 117U: return do_getrusage(ctx, args);
    case 194U: return do_getrlimit(ctx, args);
    case 195U: return do_setrlimit(ctx, args);
    case 116U: return do_gettimeofday(ctx, args);
    case 93U: case 407U: return do_select(ctx, args);
    case 230U: case 417U: return do_poll(ctx, args);
    case 202U: return do_sysctl(ctx, args);
    case 274U: return do_sysctlbyname(ctx, args);
    case 169U: case 170U: return do_csops(ctx, args);
    case 294U: case 439U: case 442U: case 443U: case 444U: case 445U: case 446U: case 494U:
        a32_ios_bsd_err(ctx, D_EINVAL);
        return 0;
    case 336U: return do_proc_info(ctx, args);
    case 367U: case 368U: case 489U: a32_ios_bsd_ok(ctx, 0U, 0U, false); return 0;
    case 500U: return do_getentropy(ctx, args);
    case 520U: case 521U: return do_abort_payload(ctx, args);
    default: return a32_ios_dispatch_bsd_sock(ctx, bsd_nr, args, out_handled);
    }
}
