/*
 * src/ios/ios_svc_bsd.c — 32-bit iOS XNU BSD syscalls (svc #0x80, r12 >= 0)
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Curated C11, host-portable (macOS arm64 AND aarch64-linux-musl).
 * File-descriptor / IO / VM helpers plus the top-level BSD dispatcher;
 * filesystem / directory syscalls live in ios_svc_bsd_fs.c and the
 * remaining units declared in ios/ios.h.
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
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

/* Darwin open-flag constants (stable XNU ABI, valid on both hosts). */
#define IOS_O_NONBLOCK 0x0004U
#define IOS_O_APPEND 0x0008U
#define IOS_O_NOFOLLOW 0x0100U
#define IOS_O_CREAT 0x0200U
#define IOS_O_TRUNC 0x0400U
#define IOS_O_EXCL 0x0800U
#define IOS_O_DIRECTORY 0x100000U
#define IOS_O_SYMLINK 0x200000U
#define IOS_O_CLOEXEC 0x1000000U

/* Darwin ioctl requests handled here (numerically host-identical). */
#define IOS_TIOCGWINSZ 0x40087468U
#define IOS_FIONREAD 0x4004667FU
#define IOS_FIONBIO 0x8004667EU
#define IOS_FIOCLEX 0x20006601U
#define IOS_FIONCLEX 0x20006602U

/* Darwin fcntl commands handled here. */
#define IOS_F_DUPFD 0U
#define IOS_F_GETFD 1U
#define IOS_F_SETFD 2U
#define IOS_F_GETFL 3U
#define IOS_F_SETFL 4U
#define IOS_F_GETPATH 50U
#define IOS_F_DUPFD_CLOEXEC 67U

#define IOS_BSD_PATH_MAX 4096U
#define IOS_IOV_MAX 64U

int a32_ios_host_to_darwin_errno(int host_err)
{
#if defined(__APPLE__)
    return host_err > 0 ? host_err : 22;
#else
    /* Linux host errno -> Darwin errno. Identity values fall through. */
    switch (host_err) {
    case 11: return 35; /* EAGAIN -> EAGAIN */
    case 35: return 11; /* EDEADLK -> EDEADLK */
    case 115: return 36; /* EINPROGRESS */
    case 114: return 37; /* EALREADY */
    case 88: return 38; /* ENOTSOCK */
    case 89: return 39; /* EDESTADDRREQ */
    case 90: return 40; /* EMSGSIZE */
    case 91: return 41; /* EPROTOTYPE */
    case 92: return 42; /* ENOPROTOOPT */
    case 93: return 43; /* EPROTONOSUPPORT */
    case 95: return 45; /* EOPNOTSUPP -> EOPNOTSUPP */
    case 97: return 47; /* EAFNOSUPPORT */
    case 98: return 48; /* EADDRINUSE */
    case 99: return 49; /* EADDRNOTAVAIL */
    case 100: return 50; /* ENETDOWN */
    case 101: return 51; /* ENETUNREACH */
    case 103: return 53; /* ECONNABORTED */
    case 104: return 54; /* ECONNRESET */
    case 105: return 55; /* ENOBUFS */
    case 106: return 56; /* EISCONN */
    case 107: return 57; /* ENOTCONN */
    case 110: return 60; /* ETIMEDOUT */
    case 111: return 61; /* ECONNREFUSED */
    case 40: return 62; /* ELOOP */
    case 36: return 63; /* ENAMETOOLONG */
    case 113: return 65; /* EHOSTUNREACH */
    case 39: return 66; /* ENOTEMPTY */
    case 38: return 78; /* ENOSYS */
    case 75: return 84; /* EOVERFLOW */
    case 125: return 89; /* ECANCELED */
    case 84: return 92; /* EILSEQ */
    default: return host_err > 0 ? host_err : 22;
    }
#endif
}

void a32_ios_bsd_ok(a32_ctx_t *ctx, uint32_t r0, uint32_t r1, bool dual_ret)
{
    if (ctx == NULL) {
        return;
    }
    ctx->cpsr &= ~(uint32_t)(1U << 29);
    ctx->regs[0] = r0;
    if (dual_ret) {
        ctx->regs[1] = r1;
    }
}

void a32_ios_bsd_err(a32_ctx_t *ctx, int darwin_errno)
{
    int e;
    if (ctx == NULL) {
        return;
    }
    e = darwin_errno >= 0 ? darwin_errno : -darwin_errno;
    if (e <= 0) {
        e = 22;
    }
    ctx->cpsr |= (uint32_t)(1U << 29);
    ctx->regs[0] = (uint32_t)e;
}

/* ---- Small guest-memory helpers (negative Darwin errno on failure) ---- */

static bool ios_rd_ok(a32_ctx_t *ctx, uint32_t va, uint32_t len)
{
    return len == 0U ||
        a32_guest_range_ok(ctx, va, len, (uint32_t)PROT_READ);
}

static bool ios_wr_ok(a32_ctx_t *ctx, uint32_t va, uint32_t len)
{
    return len == 0U ||
        a32_guest_range_ok(ctx, va, len, (uint32_t)PROT_WRITE);
}

static uint32_t ios_ld32(a32_ctx_t *ctx, uint32_t va)
{
    uint32_t v = 0U;
    memcpy(&v, a32_guest_ptr(ctx, va), 4);
    return v;
}

static int ios_cstr(a32_ctx_t *ctx, uint32_t gva, char *out, size_t cap)
{
    size_t n = 0U;
    if (cap == 0U) {
        return -14;
    }
    while (n + 1U < cap) {
        const char *hva;
        if (!a32_guest_range_ok(ctx, gva + (uint32_t)n, 1U,
                (uint32_t)PROT_READ)) {
            return -14;
        }
        hva = (const char *)a32_guest_ptr(ctx, gva);
        out[n] = hva[n];
        if (hva[n] == '\0') {
            return 0;
        }
        n++;
    }
    out[cap - 1U] = '\0';
    return -63;
}

/* Resolve a guest path VA to an absolute host path (negative Darwin errno). */
static int ios_resolve(a32_ctx_t *ctx, uint32_t gva, char *host, size_t hcap)
{
    char tmp[IOS_BSD_PATH_MAX];
    int rc;
    if (ios_cstr(ctx, gva, tmp, sizeof(tmp)) != 0) {
        return -14;
    }
    rc = a32_resolve_guest_path(ctx, tmp, host, hcap);
    return rc == 0 ? 0 : -2;
}

static int ios_xlate_oflags(uint32_t gf)
{
    int h = (int)(gf & 3U);
    if ((gf & IOS_O_NONBLOCK) != 0U) {
        h |= O_NONBLOCK;
    }
    if ((gf & IOS_O_APPEND) != 0U) {
        h |= O_APPEND;
    }
    if ((gf & IOS_O_CREAT) != 0U) {
        h |= O_CREAT;
    }
    if ((gf & IOS_O_TRUNC) != 0U) {
        h |= O_TRUNC;
    }
    if ((gf & IOS_O_EXCL) != 0U) {
        h |= O_EXCL;
    }
    if ((gf & IOS_O_NOFOLLOW) != 0U) {
        h |= O_NOFOLLOW;
    }
#ifdef O_DIRECTORY
    if ((gf & IOS_O_DIRECTORY) != 0U) {
        h |= O_DIRECTORY;
    }
#endif
#ifdef O_CLOEXEC
    if ((gf & IOS_O_CLOEXEC) != 0U) {
        h |= O_CLOEXEC;
    }
#endif
    return h;
}

/* Read one 4-byte stack slot safely (0 when unmapped). */
static uint32_t ios_stk(a32_ctx_t *ctx, uint32_t sp, uint32_t idx)
{
    uint32_t va = sp + idx * 4U;
    if (va < sp || !ios_rd_ok(ctx, va, 4U)) {
        return 0U;
    }
    return ios_ld32(ctx, va);
}

/* Host errno -> BSD error return. */
static void ios_fail(a32_ctx_t *ctx)
{
    a32_ios_bsd_err(ctx, a32_ios_host_to_darwin_errno(errno));
}

/* ---- read / write / readv / writev / pread / pwrite ---- */

static void ios_do_read(a32_ctx_t *ctx, uint32_t fd, uint32_t buf_va,
    uint32_t n)
{
    ssize_t r;
    if (n == 0U) {
        a32_ios_bsd_ok(ctx, 0U, 0U, false);
        return;
    }
    if (!ios_wr_ok(ctx, buf_va, n)) {
        a32_ios_bsd_err(ctx, 14);
        return;
    }
    r = read((int)(int32_t)fd, a32_guest_ptr(ctx, buf_va), (size_t)n);
    if (r < 0) {
        ios_fail(ctx);
        return;
    }
    a32_vm32_copyout_invalidate_exec(ctx, buf_va, (uint32_t)r);
    a32_ios_bsd_ok(ctx, (uint32_t)r, 0U, false);
}

static void ios_do_write(a32_ctx_t *ctx, uint32_t fd, uint32_t buf_va,
    uint32_t n)
{
    ssize_t r;
    if (n == 0U) {
        a32_ios_bsd_ok(ctx, 0U, 0U, false);
        return;
    }
    if (!ios_rd_ok(ctx, buf_va, n)) {
        a32_ios_bsd_err(ctx, 14);
        return;
    }
    r = write((int)(int32_t)fd, a32_guest_ptr(ctx, buf_va), (size_t)n);
    if (r < 0) {
        ios_fail(ctx);
        return;
    }
    a32_ios_bsd_ok(ctx, (uint32_t)r, 0U, false);
}

/* Gather guest iovec array into host iovecs; for_read selects PROT_WRITE. */
static int ios_gather_iov(a32_ctx_t *ctx, uint32_t vec_va, uint32_t cnt,
    struct iovec *hv, uint32_t *gb, uint32_t *gl, bool for_read)
{
    uint32_t i;
    uint32_t need = for_read ? (uint32_t)PROT_WRITE : (uint32_t)PROT_READ;
    if (cnt == 0U || cnt > IOS_IOV_MAX) {
        return -22;
    }
    if (!ios_rd_ok(ctx, vec_va, cnt * 8U)) {
        return -14;
    }
    for (i = 0U; i < cnt; i++) {
        uint32_t b = ios_ld32(ctx, vec_va + i * 8U);
        uint32_t l = ios_ld32(ctx, vec_va + i * 8U + 4U);
        if (l > 0U && !a32_guest_range_ok(ctx, b, l, need)) {
            return -14;
        }
        gb[i] = b;
        gl[i] = l;
        hv[i].iov_base = l == 0U ? NULL : a32_guest_ptr(ctx, b);
        hv[i].iov_len = (size_t)l;
    }
    return 0;
}

static void ios_do_readv(a32_ctx_t *ctx, uint32_t fd, uint32_t vec_va,
    uint32_t cnt)
{
    struct iovec hv[IOS_IOV_MAX];
    uint32_t gb[IOS_IOV_MAX];
    uint32_t gl[IOS_IOV_MAX];
    uint32_t i;
    ssize_t r;
    int rc = ios_gather_iov(ctx, vec_va, cnt, hv, gb, gl, true);
    if (rc != 0) {
        a32_ios_bsd_err(ctx, -rc);
        return;
    }
    r = readv((int)(int32_t)fd, hv, (int)cnt);
    if (r < 0) {
        ios_fail(ctx);
        return;
    }
    for (i = 0U; i < cnt; i++) {
        if (gl[i] > 0U) {
            a32_vm32_copyout_invalidate_exec(ctx, gb[i], gl[i]);
        }
    }
    a32_ios_bsd_ok(ctx, (uint32_t)r, 0U, false);
}

static void ios_do_writev(a32_ctx_t *ctx, uint32_t fd, uint32_t vec_va,
    uint32_t cnt)
{
    struct iovec hv[IOS_IOV_MAX];
    uint32_t gb[IOS_IOV_MAX];
    uint32_t gl[IOS_IOV_MAX];
    ssize_t r;
    int rc = ios_gather_iov(ctx, vec_va, cnt, hv, gb, gl, false);
    (void)gb;
    (void)gl;
    if (rc != 0) {
        a32_ios_bsd_err(ctx, -rc);
        return;
    }
    r = writev((int)(int32_t)fd, hv, (int)cnt);
    if (r < 0) {
        ios_fail(ctx);
        return;
    }
    a32_ios_bsd_ok(ctx, (uint32_t)r, 0U, false);
}

static void ios_do_pread(a32_ctx_t *ctx, uint32_t fd, uint32_t buf_va,
    uint32_t n, uint64_t off, bool is_pwrite)
{
    ssize_t r;
    if (n == 0U) {
        a32_ios_bsd_ok(ctx, 0U, 0U, false);
        return;
    }
    if (is_pwrite ? !ios_rd_ok(ctx, buf_va, n) : !ios_wr_ok(ctx, buf_va, n)) {
        a32_ios_bsd_err(ctx, 14);
        return;
    }
    if (is_pwrite) {
        r = pwrite((int)(int32_t)fd, a32_guest_ptr(ctx, buf_va), (size_t)n,
            (off_t)off);
    } else {
        r = pread((int)(int32_t)fd, a32_guest_ptr(ctx, buf_va), (size_t)n,
            (off_t)off);
    }
    if (r < 0) {
        ios_fail(ctx);
        return;
    }
    if (!is_pwrite) {
        a32_vm32_copyout_invalidate_exec(ctx, buf_va, (uint32_t)r);
    }
    a32_ios_bsd_ok(ctx, (uint32_t)r, 0U, false);
}

/* ---- open / openat / close / dup / dup2 / pipe ---- */

static void ios_do_open(a32_ctx_t *ctx, uint32_t path_va, uint32_t flags,
    uint32_t mode)
{
    char host[IOS_BSD_PATH_MAX];
    int rc;
    int fd;
    rc = ios_resolve(ctx, path_va, host, sizeof(host));
    if (rc != 0) {
        a32_ios_bsd_err(ctx, -rc);
        return;
    }
    fd = open(host, ios_xlate_oflags(flags), (mode_t)mode);
    if (fd < 0) {
        ios_fail(ctx);
        return;
    }
    a32_ios_bsd_ok(ctx, (uint32_t)fd, 0U, false);
}

/* Resolved host paths are absolute, so the host dirfd is redundant. */
static void ios_do_openat(a32_ctx_t *ctx, uint32_t path_va, uint32_t flags,
    uint32_t mode)
{
    ios_do_open(ctx, path_va, flags, mode);
}

static void ios_do_lseek(a32_ctx_t *ctx, uint32_t fd, uint32_t lo,
    uint32_t hi, uint32_t whence)
{
    uint64_t uo = (uint64_t)lo | ((uint64_t)hi << 32);
    off_t r = lseek((int)(int32_t)fd, (off_t)uo, (int)(int32_t)whence);
    if (r < 0) {
        ios_fail(ctx);
        return;
    }
    a32_ios_bsd_ok(ctx, (uint32_t)(uint64_t)r,
        (uint32_t)((uint64_t)r >> 32), true);
}

/* ---- ioctl / fcntl ---- */

static void ios_do_ioctl(a32_ctx_t *ctx, uint32_t fd, uint32_t req,
    uint32_t arg_va)
{
    int r;
    switch (req) {
    case IOS_TIOCGWINSZ:
        if (!ios_wr_ok(ctx, arg_va, 8U)) {
            a32_ios_bsd_err(ctx, 14);
            return;
        }
        r = ioctl((int)(int32_t)fd, TIOCGWINSZ, a32_guest_ptr(ctx, arg_va));
        break;
    case IOS_FIONREAD:
        if (!ios_wr_ok(ctx, arg_va, 4U)) {
            a32_ios_bsd_err(ctx, 14);
            return;
        }
        r = ioctl((int)(int32_t)fd, FIONREAD, a32_guest_ptr(ctx, arg_va));
        break;
    case IOS_FIONBIO:
        if (!ios_rd_ok(ctx, arg_va, 4U)) {
            a32_ios_bsd_err(ctx, 14);
            return;
        }
        r = ioctl((int)(int32_t)fd, FIONBIO, a32_guest_ptr(ctx, arg_va));
        break;
    case IOS_FIOCLEX:
        r = ioctl((int)(int32_t)fd, FIOCLEX);
        break;
    case IOS_FIONCLEX:
        r = ioctl((int)(int32_t)fd, FIONCLEX);
        break;
    default:
        a32_ios_bsd_err(ctx, 25);
        return;
    }
    if (r != 0) {
        ios_fail(ctx);
        return;
    }
    if (req == IOS_TIOCGWINSZ) {
        a32_vm32_copyout_invalidate_exec(ctx, arg_va, 8U);
    } else if (req == IOS_FIONREAD) {
        a32_vm32_copyout_invalidate_exec(ctx, arg_va, 4U);
    }
    a32_ios_bsd_ok(ctx, 0U, 0U, false);
}

static void ios_do_fcntl(a32_ctx_t *ctx, uint32_t fd, uint32_t cmd,
    uint32_t arg)
{
    int r;
    switch (cmd) {
    case IOS_F_DUPFD:
        r = fcntl((int)(int32_t)fd, F_DUPFD, (int)(int32_t)arg);
        break;
    case IOS_F_DUPFD_CLOEXEC:
        r = fcntl((int)(int32_t)fd, F_DUPFD_CLOEXEC, (int)(int32_t)arg);
        break;
    case IOS_F_GETFD:
        r = fcntl((int)(int32_t)fd, F_GETFD);
        break;
    case IOS_F_SETFD:
        r = fcntl((int)(int32_t)fd, F_SETFD, (int)arg);
        break;
    case IOS_F_GETFL: {
        int h;
        int d = 0;
        r = fcntl((int)(int32_t)fd, F_GETFL);
        if (r < 0) {
            ios_fail(ctx);
            return;
        }
        h = r;
        d |= h & 3;
        if ((h & O_APPEND) != 0) {
            d |= (int)IOS_O_APPEND;
        }
        if ((h & O_NONBLOCK) != 0) {
            d |= (int)IOS_O_NONBLOCK;
        }
        a32_ios_bsd_ok(ctx, (uint32_t)d, 0U, false);
        return;
    }
    case IOS_F_SETFL: {
        int h = (int)(arg & 3U);
        if ((arg & IOS_O_APPEND) != 0U) {
            h |= O_APPEND;
        }
        if ((arg & IOS_O_NONBLOCK) != 0U) {
            h |= O_NONBLOCK;
        }
        r = fcntl((int)(int32_t)fd, F_SETFL, h);
        break;
    }
    case IOS_F_GETPATH: {
#if defined(__APPLE__)
        if (!ios_wr_ok(ctx, arg, IOS_BSD_PATH_MAX)) {
            a32_ios_bsd_err(ctx, 14);
            return;
        }
        r = fcntl((int)(int32_t)fd, F_GETPATH, a32_guest_ptr(ctx, arg));
        if (r != 0) {
            ios_fail(ctx);
            return;
        }
        a32_vm32_copyout_invalidate_exec(ctx, arg, IOS_BSD_PATH_MAX);
        a32_ios_bsd_ok(ctx, 0U, 0U, false);
        return;
#else
        char proc[64];
        char tmp[IOS_BSD_PATH_MAX];
        ssize_t n;
        if (!ios_wr_ok(ctx, arg, IOS_BSD_PATH_MAX)) {
            a32_ios_bsd_err(ctx, 14);
            return;
        }
        snprintf(proc, sizeof(proc), "/proc/self/fd/%d", (int)(int32_t)fd);
        n = readlink(proc, tmp, sizeof(tmp) - 1U);
        if (n < 0) {
            ios_fail(ctx);
            return;
        }
        tmp[n] = '\0';
        {
            size_t k = (size_t)n + 1U;
            if (k > IOS_BSD_PATH_MAX) {
                k = IOS_BSD_PATH_MAX;
            }
            memcpy(a32_guest_ptr(ctx, arg), tmp, k);
            a32_vm32_copyout_invalidate_exec(ctx, arg, (uint32_t)k);
        }
        a32_ios_bsd_ok(ctx, 0U, 0U, false);
        return;
#endif
    }
    default:
        a32_ios_bsd_err(ctx, 22);
        return;
    }
    if (r < 0) {
        ios_fail(ctx);
        return;
    }
    a32_ios_bsd_ok(ctx, (uint32_t)r, 0U, false);
}

/* ---- VM syscalls: mmap / munmap / mprotect / madvise / mincore / msync ---- */

static void ios_do_mmap(a32_ctx_t *ctx, const uint32_t *a)
{
    uint32_t len = a[1];
    uint32_t flags = a[3];
    uint32_t lprot;
    uint32_t lflags;
    int fd;
    uint64_t off;
    uint32_t res;
    if (len == 0U || len > 0x80000000U) {
        a32_ios_bsd_err(ctx, 22);
        return;
    }
    lprot = a[2] & 7U;
    lflags = flags & 0x13U; /* MAP_SHARED / MAP_PRIVATE / MAP_FIXED */
    if ((flags & 0x1000U) != 0U) {
        lflags |= 0x20U; /* Darwin MAP_ANON -> Linux MAP_ANONYMOUS */
    }
    fd = (int)(int32_t)a[4];
    if ((lflags & 0x20U) != 0U) {
        fd = -1;
    }
    off = (uint64_t)a[5] | ((uint64_t)a[6] << 32);
    if ((off & 4095ULL) != 0ULL) {
        a32_ios_bsd_err(ctx, 22);
        return;
    }
    res = a32_vm_mmap2(ctx, a[0], len, lprot, lflags, fd,
        (uint32_t)(off >> 12));
    if (res > 0xFFFFF000U) {
        a32_ios_bsd_err(ctx,
            a32_ios_host_to_darwin_errno(-(int)(int32_t)res));
        return;
    }
    a32_ios_bsd_ok(ctx, res, 0U, false);
}

static void ios_do_vm_simple(a32_ctx_t *ctx, uint32_t bsd, const uint32_t *a)
{
    if (bsd == 73U) {
        int rc = a32_vm_munmap(ctx, a[0], a[1]);
        if (rc != 0) {
            a32_ios_bsd_err(ctx, a32_ios_host_to_darwin_errno(-rc));
            return;
        }
        a32_ios_bsd_ok(ctx, 0U, 0U, false);
    } else if (bsd == 74U) {
        int rc = a32_vm_mprotect(ctx, a[0], a[1], a[2] & 7U);
        if (rc != 0) {
            a32_ios_bsd_err(ctx, a32_ios_host_to_darwin_errno(-rc));
            return;
        }
        a32_ios_bsd_ok(ctx, 0U, 0U, false);
    } else if (bsd == 75U) {
        uint32_t adv = a[3];
#if defined(__linux__)
        if (adv == 5U) {
            adv = 8U; /* Darwin MADV_FREE -> Linux MADV_FREE */
        }
#endif
        if (a[1] == 0U) {
            a32_ios_bsd_ok(ctx, 0U, 0U, false);
            return;
        }
        if (madvise(a32_guest_ptr(ctx, a[0]), (size_t)a[1], (int)adv) != 0) {
            ios_fail(ctx);
            return;
        }
        a32_ios_bsd_ok(ctx, 0U, 0U, false);
    } else if (bsd == 78U) {
        uint64_t v64 = ((uint64_t)a[1] + 4095ULL) / 4096ULL;
        if (a[1] == 0U ||
            (uint64_t)a[0] + (uint64_t)a[1] > 0x100000000ULL ||
            v64 > 0x100000ULL) {
            a32_ios_bsd_err(ctx, 22);
            return;
        }
        if ((a[0] & 4095U) != 0U ||
            !a32_guest_range_ok(ctx, a[0], a[1], (uint32_t)PROT_READ) ||
            !a32_guest_range_ok(ctx, a[2], (uint32_t)v64,
                                (uint32_t)PROT_WRITE)) {
            a32_ios_bsd_err(ctx, 22);
            return;
        }
        if (mincore(a32_guest_ptr(ctx, a[0]), (size_t)a[1],
#if defined(__APPLE__)
                (char *)a32_guest_ptr(ctx, a[2])) != 0) {
#else
                (unsigned char *)a32_guest_ptr(ctx, a[2])) != 0) {
#endif
            ios_fail(ctx);
            return;
        }
        a32_vm32_copyout_invalidate_exec(ctx, a[2], (uint32_t)v64);
        a32_ios_bsd_ok(ctx, 0U, 0U, false);
    } else { /* 65 msync */
        int hf = 0;
        if ((a[2] & 0x1U) != 0U) {
            hf |= MS_ASYNC;
        }
        if ((a[2] & 0x2U) != 0U) {
            hf |= MS_INVALIDATE;
        }
        if ((a[2] & 0x10U) != 0U) {
            hf |= MS_SYNC;
        }
        if (a[1] == 0U) {
            a32_ios_bsd_ok(ctx, 0U, 0U, false);
            return;
        }
        if (msync(a32_guest_ptr(ctx, a[0]), (size_t)a[1], hf) != 0) {
            ios_fail(ctx);
            return;
        }
        a32_ios_bsd_ok(ctx, 0U, 0U, false);
    }
}

/* ---- Signal-number BSD syscalls (direct ios_signals.c calls) ---- */

static bool ios_try_signal(a32_ctx_t *ctx, uint32_t bsd, const uint32_t *a,
    bool *done)
{
    int rc = 0;
    *done = true;
    switch (bsd) {
    case 46U:
        rc = a32_ios_sys_sigaction(ctx, (int)(int32_t)a[0], a[1], a[2]);
        break;
    case 48U:
    case 329U:
        rc = a32_ios_sys_sigprocmask(ctx, (int)(int32_t)a[0], a[1], a[2]);
        break;
    case 52U:
        rc = a32_ios_sys_sigpending(ctx, a[0]);
        break;
    case 53U:
        rc = a32_ios_sys_sigaltstack(ctx, a[0], a[1]);
        break;
    case 111U:
    case 410U:
        rc = a32_ios_sys_sigsuspend(ctx, a[0]);
        break;
    case 37U:
        rc = a32_ios_sys_kill(ctx, (int)(int32_t)a[0], (int)(int32_t)a[1], 1);
        break;
    case 328U:
        rc = a32_ios_sys_pthread_kill(ctx, a[0], (int)(int32_t)a[1]);
        break;
    case 184U:
        rc = a32_ios_sys_sigreturn(ctx, a[0], (int)(int32_t)a[1]);
        if (rc == 0) {
            return true; /* regs/cpsr already restored; keep them */
        }
        break;
    default:
        *done = false;
        return false;
    }
    if (rc == 0) {
        a32_ios_bsd_ok(ctx, 0U, 0U, false);
    } else {
        a32_ios_bsd_err(ctx, -rc);
    }
    return true;
}

/* ---- Top-level BSD dispatcher ---- */

int a32_ios_handle_svc(a32_ctx_t *ctx)
{
    uint32_t raw;
    uint32_t bsd = 0U;
    uint32_t args[8];
    uint32_t sp;
    bool handled = false;
    if (ctx == NULL) {
        return 0;
    }
    if (ctx->svc_imm == 0x81U) {
        return a32_ios_handle_stub_svc(ctx);
    }
    raw = ctx->regs[12];
    if ((int32_t)raw < 0) {
        int rc = a32_ios_dispatch_mach_or_arm_trap(ctx, raw);
        a32_ios_signals_check_pending(ctx);
        return rc;
    }
    sp = ctx->regs[13];
    if (raw == 0U) { /* SYS_syscall: number in r0, args in r1..r3, r4..r6, r8 */
        bsd = ctx->regs[0];
        args[0] = ctx->regs[1];
        args[1] = ctx->regs[2];
        args[2] = ctx->regs[3];
        args[3] = ctx->regs[4];
        args[4] = ctx->regs[5];
        args[5] = ctx->regs[6];
        args[6] = ctx->regs[8];
        args[7] = ios_stk(ctx, sp, 0U);
    } else {
        bsd = raw & 0xFFFFU; /* strip SYSCALL_CLASS_UNIX */
        args[0] = ctx->regs[0];
        args[1] = ctx->regs[1];
        args[2] = ctx->regs[2];
        args[3] = ctx->regs[3];
        args[4] = ctx->regs[4];
        args[5] = ctx->regs[5];
        args[6] = ctx->regs[6];
        args[7] = ctx->regs[8];
    }
    if (ctx->trace_enabled) {
        fprintf(stderr, "[a32rt-ios] bsd=%u args=(%x,%x,%x,%x,%x,%x,%x,%x)\n",
            bsd, args[0], args[1], args[2], args[3],
            args[4], args[5], args[6], args[7]);
    }
    if (ios_try_signal(ctx, bsd, args, &handled) && handled) {
        goto out;
    }
    (void)a32_ios_dispatch_thread_sync_bsd(ctx, bsd, args, &handled);
    if (handled) {
        goto out;
    }
    (void)a32_ios_dispatch_bsd_fs(ctx, bsd, args, &handled);
    if (handled) {
        goto out;
    }
    (void)a32_ios_dispatch_bsd_proc_net(ctx, bsd, args, &handled);
    if (handled) {
        goto out;
    }
    switch (bsd) {
    case 3U: case 396U: ios_do_read(ctx, args[0], args[1], args[2]); break;
    case 4U: case 397U: ios_do_write(ctx, args[0], args[1], args[2]); break;
    case 120U: case 411U: ios_do_readv(ctx, args[0], args[1], args[2]); break;
    case 121U: case 412U: ios_do_writev(ctx, args[0], args[1], args[2]); break;
    case 153U: case 414U:
        ios_do_pread(ctx, args[0], args[1], args[2],
            (uint64_t)args[3] | ((uint64_t)args[4] << 32), false);
        break;
    case 154U: case 415U:
        ios_do_pread(ctx, args[0], args[1], args[2],
            (uint64_t)args[3] | ((uint64_t)args[4] << 32), true);
        break;
    case 5U: case 398U: ios_do_open(ctx, args[0], args[1], args[2]); break;
    case 463U: case 464U: ios_do_openat(ctx, args[1], args[2], args[3]); break;
    case 6U: case 399U:
        a32_ios_kq_close((int)(int32_t)args[0]);
        if (close((int)(int32_t)args[0]) != 0) { ios_fail(ctx); } else { a32_ios_bsd_ok(ctx, 0U, 0U, false); }
        break;
    case 41U: {
        int nd = dup((int)(int32_t)args[0]);
        if (nd < 0) { ios_fail(ctx); } else { a32_ios_bsd_ok(ctx, (uint32_t)nd, 0U, false); }
        break;
    }
    case 90U:
        if (dup2((int)(int32_t)args[0], (int)(int32_t)args[1]) < 0) { ios_fail(ctx); } else { a32_ios_bsd_ok(ctx, args[1], 0U, false); }
        break;
    case 42U: {
        int f[2];
        if (pipe(f) != 0) { ios_fail(ctx); } else { a32_ios_bsd_ok(ctx, (uint32_t)f[0], (uint32_t)f[1], true); }
        break;
    }
    case 54U: ios_do_ioctl(ctx, args[0], args[1], args[2]); break;
    case 92U: case 406U: ios_do_fcntl(ctx, args[0], args[1], args[2]); break;
    case 19U: case 199U: ios_do_lseek(ctx, args[0], args[1], args[2], args[3]); break;
    case 197U: ios_do_mmap(ctx, args); break;
    case 73U: case 74U: case 75U: case 78U: case 65U: ios_do_vm_simple(ctx, bsd, args); break;
    default: a32_ios_bsd_err(ctx, 78); break;
    }
out:
    if (ctx->trace_enabled) {
        fprintf(stderr, "[a32rt-ios] -> r0=%x c=%u\n", ctx->regs[0],
            (ctx->cpsr >> 29U) & 1U);
    }
    if (ctx->exit_reason != (uint32_t)A32_EXIT_BRANCH || bsd == 184U) {
        a32_ios_signals_check_pending(ctx);
    }
    return 0;
}
