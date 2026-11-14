/*
 * src/ios/ios_svc_bsd_fs.c — 32-bit iOS XNU BSD filesystem syscalls
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Curated C11, host-portable (macOS arm64 AND aarch64-linux-musl).
 * stat / statfs / dir / link / rename / chmod / chown helpers plus
 * a32_ios_dispatch_bsd_fs (via a32_ios_handle_svc in ios_svc_bsd.c).
 */

#if !defined(__APPLE__)
#define _GNU_SOURCE
#else
#define _DARWIN_C_SOURCE
#endif

#include "a32rt.h"
#include "ios/ios.h"
#include "vm/vm32.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/statfs.h>
#elif defined(__APPLE__)
#include <sys/mount.h>
#endif

/* Darwin AT flag constants (stable XNU ABI, valid on both hosts). */
#define IOS_AT_REMOVEDIR 0x0080U
#define IOS_AT_SYMLINK_NOFOLLOW 0x0020U
#define IOS_AT_SYMLINK_FOLLOW 0x0040U
#define IOS_AT_EACCESS 0x0010U

#define IOS_BSD_PATH_MAX 4096U
#define IOS_DIR_BUF 4096U

/* ---- Small guest-memory helpers (negative Darwin errno on failure) ---- */

static bool ios_wr_ok(a32_ctx_t *ctx, uint32_t va, uint32_t len)
{
    return len == 0U || a32_guest_range_ok(ctx, va, len, (uint32_t)PROT_WRITE);
}

static int ios_cstr(a32_ctx_t *ctx, uint32_t gva, char *out, size_t cap)
{
    size_t n = 0U;
    uint32_t cur = gva;
    if (cap == 0U) {
        return -14;
    }
    for (;;) {
        uint32_t rem = 0x1000U - (cur & 0xFFFU);
        const char *blk;
        size_t want;
        const char *hit;
        if (n + 1U >= cap) {
            out[cap - 1U] = '\0';
            return -63;
        }
        if (!a32_guest_range_ok(ctx, cur, rem, (uint32_t)PROT_READ)) {
            return -14;
        }
        blk = (const char *)a32_guest_ptr(ctx, cur);
        want = cap - n - 1U;
        if (want > rem) {
            want = rem;
        }
        hit = (const char *)memchr(blk, 0, want);
        if (hit != NULL) {
            memcpy(out + n, blk, (size_t)(hit - blk) + 1U);
            return 0;
        }
        if (n + rem >= cap) {
            out[cap - 1U] = '\0';
            return -63;
        }
        memcpy(out + n, blk, rem);
        n += rem;
        cur += rem;
    }
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

/* Host errno -> BSD error return. */
static void ios_fail(a32_ctx_t *ctx)
{
    a32_ios_bsd_err(ctx, a32_ios_host_to_darwin_errno(errno));
}

/* ---- stat family (ios_ostat32_t 96B / ios_stat64_t 108B) ---- */

static void ios_fill_ostat(const struct stat *hs, ios_ostat32_t *o)
{
    memset(o, 0, sizeof(*o));
    o->st_dev = (int32_t)hs->st_dev;
    o->st_ino = (uint32_t)hs->st_ino;
    o->st_mode = (uint16_t)hs->st_mode;
    o->st_nlink = (uint16_t)hs->st_nlink;
    o->st_uid = (uint32_t)hs->st_uid;
    o->st_gid = (uint32_t)hs->st_gid;
    o->st_rdev = (int32_t)hs->st_rdev;
#if defined(__APPLE__)
    o->st_atime_sec = hs->st_atimespec.tv_sec;
    o->st_atime_nsec = (int32_t)hs->st_atimespec.tv_nsec;
    o->st_mtime_sec = hs->st_mtimespec.tv_sec;
    o->st_mtime_nsec = (int32_t)hs->st_mtimespec.tv_nsec;
    o->st_ctime_sec = hs->st_ctimespec.tv_sec;
    o->st_ctime_nsec = (int32_t)hs->st_ctimespec.tv_nsec;
    o->st_flags = hs->st_flags;
    o->st_gen = (uint32_t)hs->st_gen;
#else
    o->st_atime_sec = (int32_t)hs->st_atim.tv_sec;
    o->st_atime_nsec = (int32_t)hs->st_atim.tv_nsec;
    o->st_mtime_sec = (int32_t)hs->st_mtim.tv_sec;
    o->st_mtime_nsec = (int32_t)hs->st_mtim.tv_nsec;
    o->st_ctime_sec = (int32_t)hs->st_ctim.tv_sec;
    o->st_ctime_nsec = (int32_t)hs->st_ctim.tv_nsec;
#endif
    o->st_size = (int64_t)hs->st_size;
    o->st_blocks = (int64_t)hs->st_blocks;
    o->st_blksize = (int32_t)hs->st_blksize;
}

static void ios_fill_stat64(const struct stat *hs, ios_stat64_t *o)
{
    memset(o, 0, sizeof(*o));
    o->st_dev = (int32_t)hs->st_dev;
    o->st_mode = (uint16_t)hs->st_mode;
    o->st_nlink = (uint16_t)hs->st_nlink;
    o->st_ino = (uint64_t)hs->st_ino;
    o->st_uid = (uint32_t)hs->st_uid;
    o->st_gid = (uint32_t)hs->st_gid;
    o->st_rdev = (int32_t)hs->st_rdev;
#if defined(__APPLE__)
    o->st_atime_sec = hs->st_atimespec.tv_sec;
    o->st_atime_nsec = (int32_t)hs->st_atimespec.tv_nsec;
    o->st_mtime_sec = hs->st_mtimespec.tv_sec;
    o->st_mtime_nsec = (int32_t)hs->st_mtimespec.tv_nsec;
    o->st_ctime_sec = hs->st_ctimespec.tv_sec;
    o->st_ctime_nsec = (int32_t)hs->st_ctimespec.tv_nsec;
    o->st_btime_sec = hs->st_birthtimespec.tv_sec;
    o->st_btime_nsec = (int32_t)hs->st_birthtimespec.tv_nsec;
    o->st_flags = hs->st_flags;
    o->st_gen = (uint32_t)hs->st_gen;
#else
    o->st_atime_sec = (int32_t)hs->st_atim.tv_sec;
    o->st_atime_nsec = (int32_t)hs->st_atim.tv_nsec;
    o->st_mtime_sec = (int32_t)hs->st_mtim.tv_sec;
    o->st_mtime_nsec = (int32_t)hs->st_mtim.tv_nsec;
    o->st_ctime_sec = (int32_t)hs->st_ctim.tv_sec;
    o->st_ctime_nsec = (int32_t)hs->st_ctim.tv_nsec;
#endif
    o->st_size = (int64_t)hs->st_size;
    o->st_blocks = (int64_t)hs->st_blocks;
    o->st_blksize = (int32_t)hs->st_blksize;
}

/* Write one filled stat buffer to guest memory. */
static void ios_emit_stat(a32_ctx_t *ctx, const struct stat *hs,
    uint32_t buf_va, bool is64)
{
    if (is64) {
        ios_stat64_t o;
        ios_fill_stat64(hs, &o);
        memcpy(a32_guest_ptr(ctx, buf_va), &o, sizeof(o));
        a32_vm32_copyout_invalidate_exec(ctx, buf_va, (uint32_t)sizeof(o));
    } else {
        ios_ostat32_t o;
        ios_fill_ostat(hs, &o);
        memcpy(a32_guest_ptr(ctx, buf_va), &o, sizeof(o));
        a32_vm32_copyout_invalidate_exec(ctx, buf_va, (uint32_t)sizeof(o));
    }
    a32_ios_bsd_ok(ctx, 0U, 0U, false);
}

/* Path / fd / at stat core: kind 0 = stat, 1 = lstat. */
static void ios_do_stat(a32_ctx_t *ctx, uint32_t path_va, uint32_t buf_va,
    bool is64, bool nofollow)
{
    char host[IOS_BSD_PATH_MAX];
    struct stat hs;
    int rc;
    if (!ios_wr_ok(ctx, buf_va, is64 ? 108U : 96U)) {
        a32_ios_bsd_err(ctx, 14);
        return;
    }
    rc = ios_resolve(ctx, path_va, host, sizeof(host));
    if (rc != 0) {
        a32_ios_bsd_err(ctx, -rc);
        return;
    }
    rc = nofollow ? lstat(host, &hs) : stat(host, &hs);
    if (rc != 0) {
        ios_fail(ctx);
        return;
    }
    ios_emit_stat(ctx, &hs, buf_va, is64);
}

static void ios_do_fstat(a32_ctx_t *ctx, uint32_t fd, uint32_t buf_va,
    bool is64)
{
    struct stat hs;
    if (!ios_wr_ok(ctx, buf_va, is64 ? 108U : 96U)) {
        a32_ios_bsd_err(ctx, 14);
        return;
    }
    if (fstat((int)(int32_t)fd, &hs) != 0) {
        ios_fail(ctx);
        return;
    }
    ios_emit_stat(ctx, &hs, buf_va, is64);
}

/* ---- statfs64 / fstatfs64 / getfsstat64 (exact 2168-byte XNU layout) ---- */

typedef struct __attribute__((packed, aligned(4))) ios_statfs64 {
    uint32_t f_bsize;         /* 0 */
    int32_t  f_iosize;        /* 4 */
    uint64_t f_blocks;        /* 8 */
    uint64_t f_bfree;         /* 16 */
    uint64_t f_bavail;        /* 24 */
    uint64_t f_files;         /* 32 */
    uint64_t f_ffree;         /* 40 */
    uint32_t f_fsid[2];       /* 48 */
    uint32_t f_owner;         /* 56 */
    uint32_t f_type;          /* 60 */
    uint32_t f_flags;         /* 64 */
    uint32_t f_fssubtype;     /* 68 */
    char     f_fstypename[16];    /* 72 */
    char     f_mntonname[1024];   /* 88 */
    char     f_mntfromname[1024]; /* 1112 */
    uint32_t f_reserved[8];   /* 2136 */
} ios_statfs64_t;

_Static_assert(sizeof(ios_statfs64_t) == 2168,
    "ios_statfs64_t must be 2168 bytes");

/* Fill one statfs64 entry from a path (NULL when is_fd) or an open fd. */
static void ios_statfs_one(a32_ctx_t *ctx, uint32_t buf_va,
    bool is_fd, int fd, const char *path, uint32_t ok_rv)
{
    ios_statfs64_t o;
#if defined(__linux__)
    struct statfs hs;
    int rc = is_fd ? fstatfs(fd, &hs) : statfs(path, &hs);
    if (rc != 0) {
        ios_fail(ctx);
        return;
    }
    memset(&o, 0, sizeof(o));
    o.f_bsize = (uint32_t)hs.f_bsize;
    o.f_iosize = (int32_t)hs.f_frsize;
    o.f_blocks = (uint64_t)hs.f_blocks;
    o.f_bfree = (uint64_t)hs.f_bfree;
    o.f_bavail = (uint64_t)hs.f_bavail;
    o.f_files = (uint64_t)hs.f_files;
    o.f_ffree = (uint64_t)hs.f_ffree;
    memcpy(o.f_fsid, &hs.f_fsid, sizeof(o.f_fsid));
    /* Linux ST_* -> Darwin MNT_*: RDONLY, SYNCHRONOUS, NOEXEC, NOSUID,
     * NODEV, NOATIME; the host fs is always guest-local (MNT_LOCAL). */
    o.f_flags = (uint32_t)((hs.f_flags & 1UL ? 0x1U : 0U) | (hs.f_flags & 16UL ? 0x2U : 0U) |
        (hs.f_flags & 8UL ? 0x4U : 0U) | (hs.f_flags & 2UL ? 0x8U : 0U) |
        (hs.f_flags & 4UL ? 0x10U : 0U) | (hs.f_flags & 1024UL ? 0x10000000U : 0U) | 0x1000U);
#else
    struct statfs hs;
    int rc = is_fd ? fstatfs(fd, &hs) : statfs(path, &hs);
    if (rc != 0) {
        ios_fail(ctx);
        return;
    }
    memset(&o, 0, sizeof(o));
    o.f_bsize = (uint32_t)hs.f_bsize;
    o.f_iosize = (int32_t)hs.f_iosize;
    o.f_blocks = (uint64_t)hs.f_blocks;
    o.f_bfree = (uint64_t)hs.f_bfree;
    o.f_bavail = (uint64_t)hs.f_bavail;
    o.f_files = (uint64_t)hs.f_files;
    o.f_ffree = (uint64_t)hs.f_ffree;
    o.f_fsid[0] = (uint32_t)hs.f_fsid.val[0];
    o.f_fsid[1] = (uint32_t)hs.f_fsid.val[1];
    o.f_owner = (uint32_t)hs.f_owner;
    o.f_type = (uint32_t)hs.f_type;
    o.f_flags = (uint32_t)hs.f_flags;
    o.f_fssubtype = (uint32_t)hs.f_fssubtype;
    memcpy(o.f_fstypename, hs.f_fstypename, sizeof(o.f_fstypename));
    memcpy(o.f_mntonname, hs.f_mntonname, sizeof(o.f_mntonname));
    memcpy(o.f_mntfromname, hs.f_mntfromname, sizeof(o.f_mntfromname));
#endif
    memcpy(a32_guest_ptr(ctx, buf_va), &o, sizeof(o));
    a32_vm32_copyout_invalidate_exec(ctx, buf_va, (uint32_t)sizeof(o));
    a32_ios_bsd_ok(ctx, ok_rv, 0U, false);
}

static void ios_do_statfs(a32_ctx_t *ctx, uint32_t path_va, uint32_t buf_va,
    bool is_fd, int fd)
{
    char host[IOS_BSD_PATH_MAX];
    if (!ios_wr_ok(ctx, buf_va, (uint32_t)sizeof(ios_statfs64_t))) {
        a32_ios_bsd_err(ctx, 14);
        return;
    }
    if (!is_fd) {
        int rr = ios_resolve(ctx, path_va, host, sizeof(host));
        if (rr != 0) {
            a32_ios_bsd_err(ctx, -rr);
            return;
        }
    } else {
        host[0] = '\0';
    }
    ios_statfs_one(ctx, buf_va, is_fd, fd, host, 0U);
}

/* getfsstat64: single-entry table for the sysroot; count query when buf == 0. */
static void ios_do_getfsstat(a32_ctx_t *ctx, uint32_t buf_va, uint32_t size)
{
    char host[IOS_BSD_PATH_MAX];
    if (buf_va == 0U) {
        a32_ios_bsd_ok(ctx, 1U, 0U, false);
        return;
    }
    if (size < (uint32_t)sizeof(ios_statfs64_t)) {
        a32_ios_bsd_err(ctx, 22);
        return;
    }
    if (!ios_wr_ok(ctx, buf_va, (uint32_t)sizeof(ios_statfs64_t))) {
        a32_ios_bsd_err(ctx, 14);
        return;
    }
    if (a32_resolve_guest_path(ctx, "/", host, sizeof(host)) != 0) {
        snprintf(host, sizeof(host), "/");
    }
    ios_statfs_one(ctx, buf_va, false, 0, host, 1U);
}

/* ---- *at plumbing (Darwin -> host flag/fd translation) ---- */

static int ios_atfd(uint32_t gfd)
{
    /* Darwin AT_FDCWD is -2; host AT_FDCWD comes from <fcntl.h>. */
    return (int32_t)gfd == -2 ? AT_FDCWD : (int)(int32_t)gfd;
}

static int ios_resolve2(a32_ctx_t *ctx, uint32_t a_va, uint32_t b_va,
    char *ha, char *hb)
{
    int r = ios_resolve(ctx, a_va, ha, IOS_BSD_PATH_MAX);
    return (r != 0) ? r : ios_resolve(ctx, b_va, hb, IOS_BSD_PATH_MAX);
}

/* ---- fstatat64 (469): fd/path/buf/flag, always 64-bit layout ---- */

static void ios_do_fstatat(a32_ctx_t *ctx, uint32_t fd, uint32_t path_va,
    uint32_t buf_va, uint32_t flag)
{
    char host[IOS_BSD_PATH_MAX];
    struct stat hs;
    int rc;
    int hf = (flag & IOS_AT_SYMLINK_NOFOLLOW) != 0U ? AT_SYMLINK_NOFOLLOW : 0;
    if (!ios_wr_ok(ctx, buf_va, 108U)) {
        a32_ios_bsd_err(ctx, 14);
        return;
    }
    rc = ios_resolve(ctx, path_va, host, sizeof(host));
    if (rc != 0) {
        a32_ios_bsd_err(ctx, -rc);
        return;
    }
    if (fstatat(ios_atfd(fd), host, &hs, hf) != 0) {
        ios_fail(ctx);
        return;
    }
    ios_emit_stat(ctx, &hs, buf_va, true);
}

/* ---- getdirentries64 (344): 21B header + name padded to 4B ----
 *
 * 32-bit Darwin struct dirent: d_ino (8B @0), d_seekoff (8B @8),
 * d_reclen (u16 @16), d_namlen (u16 @18), d_type (u8 @20),
 * d_name[] @21. basep is an *output*: the kernel writes the updated
 * directory offset there; the fd's own offset selects the start. */

static void ios_do_getdirentries64(a32_ctx_t *ctx, uint32_t fd,
    uint32_t buf_va, uint32_t nbytes, uint32_t pos_va)
{
    uint8_t *dst;
    uint32_t off = 0U;
    int dfd;
    DIR *dp;
    long cur;
    uint64_t npos;
    if (nbytes > 0U && !ios_wr_ok(ctx, buf_va, nbytes)) {
        a32_ios_bsd_err(ctx, 14);
        return;
    }
    if (pos_va != 0U && !ios_wr_ok(ctx, pos_va, 8U)) {
        a32_ios_bsd_err(ctx, 14);
        return;
    }
    cur = lseek((int)(int32_t)fd, 0, SEEK_CUR);
    if (cur < 0) {
        ios_fail(ctx);
        return;
    }
    dfd = dup((int)(int32_t)fd);
    if (dfd < 0) {
        ios_fail(ctx);
        return;
    }
    dp = fdopendir(dfd);
    if (dp == NULL) {
        close(dfd);
        ios_fail(ctx);
        return;
    }
    dst = nbytes == 0U ? NULL : (uint8_t *)a32_guest_ptr(ctx, buf_va);
    for (;;) {
        struct dirent *e;
        size_t nl;
        uint32_t rn;
        long prev = telldir(dp);
        uint64_t v64;
        uint16_t v16;
        if ((e = readdir(dp)) == NULL) {
            break;
        }
        nl = strlen(e->d_name);
        rn = (uint32_t)((21U + nl + 1U + 3U) & ~3U);
        if (rn > nbytes - off) {
            seekdir(dp, prev);
            break;
        }
        v64 = (uint64_t)e->d_ino;
        memcpy(dst + off, &v64, 8U);
        v64 = (uint64_t)telldir(dp);
        memcpy(dst + off + 8U, &v64, 8U);
        v16 = (uint16_t)rn;
        memcpy(dst + off + 16U, &v16, 2U);
        v16 = (uint16_t)nl;
        memcpy(dst + off + 18U, &v16, 2U);
        dst[off + 20U] = (uint8_t)e->d_type;
        memcpy(dst + off + 21U, e->d_name, nl + 1U);
        memset(dst + off + 21U + nl + 1U, 0, rn - 21U - (uint32_t)nl - 1U);
        off += rn;
    }
    npos = (uint64_t)telldir(dp);
    closedir(dp);
    (void)lseek((int)(int32_t)fd, (off_t)npos, SEEK_SET);
    if (pos_va != 0U) {
        memcpy(a32_guest_ptr(ctx, pos_va), &npos, 8U);
        a32_vm32_copyout_invalidate_exec(ctx, pos_va, 8U);
    }
    if (off > 0U) {
        a32_vm32_copyout_invalidate_exec(ctx, buf_va, off);
    }
    a32_ios_bsd_ok(ctx, off, 0U, false);
}

/* ---- link / unlink / rename ---- */

static void ios_do_link(a32_ctx_t *ctx, uint32_t o_va, uint32_t n_va)
{
    char ho[IOS_BSD_PATH_MAX], hn[IOS_BSD_PATH_MAX];
    int rc = ios_resolve2(ctx, o_va, n_va, ho, hn);
    if (rc != 0) {
        a32_ios_bsd_err(ctx, -rc);
        return;
    }
    if (link(ho, hn) != 0) {
        ios_fail(ctx);
        return;
    }
    a32_ios_bsd_ok(ctx, 0U, 0U, false);
}

static void ios_do_linkat(a32_ctx_t *ctx, const uint32_t *a)
{
    char ho[IOS_BSD_PATH_MAX], hn[IOS_BSD_PATH_MAX];
    int rc;
    int hf = (a[4] & IOS_AT_SYMLINK_FOLLOW) != 0U ? AT_SYMLINK_FOLLOW : 0;
    rc = ios_resolve2(ctx, a[1], a[3], ho, hn);
    if (rc != 0) {
        a32_ios_bsd_err(ctx, -rc);
        return;
    }
    if (linkat(ios_atfd(a[0]), ho, ios_atfd(a[2]), hn, hf) != 0) {
        ios_fail(ctx);
        return;
    }
    a32_ios_bsd_ok(ctx, 0U, 0U, false);
}

static void ios_do_unlink(a32_ctx_t *ctx, uint32_t p_va)
{
    char host[IOS_BSD_PATH_MAX];
    int rc = ios_resolve(ctx, p_va, host, sizeof(host));
    if (rc != 0) {
        a32_ios_bsd_err(ctx, -rc);
        return;
    }
    if (unlink(host) != 0) {
        ios_fail(ctx);
        return;
    }
    a32_ios_bsd_ok(ctx, 0U, 0U, false);
}

static void ios_do_unlinkat(a32_ctx_t *ctx, uint32_t fd, uint32_t p_va,
    uint32_t flag)
{
    char host[IOS_BSD_PATH_MAX];
    int rc;
    int hf = (flag & IOS_AT_REMOVEDIR) != 0U ? AT_REMOVEDIR : 0;
    rc = ios_resolve(ctx, p_va, host, sizeof(host));
    if (rc != 0) {
        a32_ios_bsd_err(ctx, -rc);
        return;
    }
    if (unlinkat(ios_atfd(fd), host, hf) != 0) {
        ios_fail(ctx);
        return;
    }
    a32_ios_bsd_ok(ctx, 0U, 0U, false);
}

static void ios_do_rmdir(a32_ctx_t *ctx, uint32_t p_va)
{
    char host[IOS_BSD_PATH_MAX];
    int rc = ios_resolve(ctx, p_va, host, sizeof(host));
    if (rc != 0) {
        a32_ios_bsd_err(ctx, -rc);
        return;
    }
    if (rmdir(host) != 0) {
        ios_fail(ctx);
        return;
    }
    a32_ios_bsd_ok(ctx, 0U, 0U, false);
}

static void ios_do_rename(a32_ctx_t *ctx, uint32_t o_va, uint32_t n_va)
{
    char ho[IOS_BSD_PATH_MAX], hn[IOS_BSD_PATH_MAX];
    int rc = ios_resolve2(ctx, o_va, n_va, ho, hn);
    if (rc != 0) {
        a32_ios_bsd_err(ctx, -rc);
        return;
    }
    if (rename(ho, hn) != 0) {
        ios_fail(ctx);
        return;
    }
    a32_ios_bsd_ok(ctx, 0U, 0U, false);
}

static void ios_do_renameat(a32_ctx_t *ctx, const uint32_t *a)
{
    char ho[IOS_BSD_PATH_MAX], hn[IOS_BSD_PATH_MAX];
    int rc = ios_resolve2(ctx, a[1], a[3], ho, hn);
    if (rc != 0) {
        a32_ios_bsd_err(ctx, -rc);
        return;
    }
    if (renameat(ios_atfd(a[0]), ho, ios_atfd(a[2]), hn) != 0) {
        ios_fail(ctx);
        return;
    }
    a32_ios_bsd_ok(ctx, 0U, 0U, false);
}

/* ---- chdir / mkdir ---- */

static void ios_do_chdir(a32_ctx_t *ctx, uint32_t p_va)
{
    char host[IOS_BSD_PATH_MAX];
    int rc = ios_resolve(ctx, p_va, host, sizeof(host));
    if (rc != 0) {
        a32_ios_bsd_err(ctx, -rc);
        return;
    }
    if (chdir(host) != 0) {
        ios_fail(ctx);
        return;
    }
    a32_ios_bsd_ok(ctx, 0U, 0U, false);
}

static void ios_do_mkdir(a32_ctx_t *ctx, uint32_t p_va, uint32_t mode)
{
    char host[IOS_BSD_PATH_MAX];
    int rc = ios_resolve(ctx, p_va, host, sizeof(host));
    if (rc != 0) {
        a32_ios_bsd_err(ctx, -rc);
        return;
    }
    if (mkdir(host, (mode_t)mode) != 0) {
        ios_fail(ctx);
        return;
    }
    a32_ios_bsd_ok(ctx, 0U, 0U, false);
}

static void ios_do_mkdirat(a32_ctx_t *ctx, uint32_t fd, uint32_t p_va,
    uint32_t mode)
{
    char host[IOS_BSD_PATH_MAX];
    int rc = ios_resolve(ctx, p_va, host, sizeof(host));
    if (rc != 0) {
        a32_ios_bsd_err(ctx, -rc);
        return;
    }
    if (mkdirat(ios_atfd(fd), host, (mode_t)mode) != 0) {
        ios_fail(ctx);
        return;
    }
    a32_ios_bsd_ok(ctx, 0U, 0U, false);
}

/* ---- chmod / chown / access ---- */

static void ios_do_chmod(a32_ctx_t *ctx, uint32_t p_va, uint32_t mode)
{
    char host[IOS_BSD_PATH_MAX];
    int rc = ios_resolve(ctx, p_va, host, sizeof(host));
    if (rc != 0) {
        a32_ios_bsd_err(ctx, -rc);
        return;
    }
    if (chmod(host, (mode_t)mode) != 0) {
        ios_fail(ctx);
        return;
    }
    a32_ios_bsd_ok(ctx, 0U, 0U, false);
}

static void ios_do_chown(a32_ctx_t *ctx, uint32_t p_va, uint32_t uid,
    uint32_t gid)
{
    char host[IOS_BSD_PATH_MAX];
    int rc = ios_resolve(ctx, p_va, host, sizeof(host));
    if (rc != 0) {
        a32_ios_bsd_err(ctx, -rc);
        return;
    }
    if (chown(host, (uid_t)uid, (gid_t)gid) != 0) {
        ios_fail(ctx);
        return;
    }
    a32_ios_bsd_ok(ctx, 0U, 0U, false);
}

static void ios_do_access(a32_ctx_t *ctx, uint32_t p_va, uint32_t mode)
{
    char host[IOS_BSD_PATH_MAX];
    int rc = ios_resolve(ctx, p_va, host, sizeof(host));
    if (rc != 0) {
        a32_ios_bsd_err(ctx, -rc);
        return;
    }
    if (access(host, (int)mode) != 0) {
        ios_fail(ctx);
        return;
    }
    a32_ios_bsd_ok(ctx, 0U, 0U, false);
}

static void ios_do_faccessat(a32_ctx_t *ctx, uint32_t fd, uint32_t p_va,
    uint32_t mode, uint32_t flag)
{
    char host[IOS_BSD_PATH_MAX];
    int rc;
    int hf = ((flag & IOS_AT_SYMLINK_NOFOLLOW) != 0U ? AT_SYMLINK_NOFOLLOW : 0) |
        ((flag & IOS_AT_EACCESS) != 0U ? AT_EACCESS : 0);
    rc = ios_resolve(ctx, p_va, host, sizeof(host));
    if (rc != 0) {
        a32_ios_bsd_err(ctx, -rc);
        return;
    }
    if (faccessat(ios_atfd(fd), host, (int)mode, hf) != 0) {
        ios_fail(ctx);
        return;
    }
    a32_ios_bsd_ok(ctx, 0U, 0U, false);
}

/* ---- symlink / readlink (link target stored verbatim) ---- */

static void ios_do_symlink(a32_ctx_t *ctx, uint32_t t_va, uint32_t p_va)
{
    char tgt[IOS_BSD_PATH_MAX], host[IOS_BSD_PATH_MAX];
    int rc = ios_cstr(ctx, t_va, tgt, sizeof(tgt));
    if (rc != 0) {
        a32_ios_bsd_err(ctx, -rc);
        return;
    }
    rc = ios_resolve(ctx, p_va, host, sizeof(host));
    if (rc != 0) {
        a32_ios_bsd_err(ctx, -rc);
        return;
    }
    if (symlink(tgt, host) != 0) {
        ios_fail(ctx);
        return;
    }
    a32_ios_bsd_ok(ctx, 0U, 0U, false);
}

static void ios_do_symlinkat(a32_ctx_t *ctx, uint32_t t_va, uint32_t fd,
    uint32_t p_va)
{
    char tgt[IOS_BSD_PATH_MAX], host[IOS_BSD_PATH_MAX];
    int rc = ios_cstr(ctx, t_va, tgt, sizeof(tgt));
    if (rc != 0) {
        a32_ios_bsd_err(ctx, -rc);
        return;
    }
    rc = ios_resolve(ctx, p_va, host, sizeof(host));
    if (rc != 0) {
        a32_ios_bsd_err(ctx, -rc);
        return;
    }
    if (symlinkat(tgt, ios_atfd(fd), host) != 0) {
        ios_fail(ctx);
        return;
    }
    a32_ios_bsd_ok(ctx, 0U, 0U, false);
}

static void ios_do_readlink(a32_ctx_t *ctx, uint32_t p_va, uint32_t buf_va,
    uint32_t sz, bool is_at, int atfd)
{
    char host[IOS_BSD_PATH_MAX];
    ssize_t n;
    int rc = ios_resolve(ctx, p_va, host, sizeof(host));
    if (rc != 0) {
        a32_ios_bsd_err(ctx, -rc);
        return;
    }
    if (!ios_wr_ok(ctx, buf_va, sz)) {
        a32_ios_bsd_err(ctx, 14);
        return;
    }
    n = is_at ? readlinkat(atfd, host, a32_guest_ptr(ctx, buf_va), (size_t)sz) : readlink(host, a32_guest_ptr(ctx, buf_va), (size_t)sz);
    if (n < 0) {
        ios_fail(ctx);
        return;
    }
    a32_vm32_copyout_invalidate_exec(ctx, buf_va, (uint32_t)n);
    a32_ios_bsd_ok(ctx, (uint32_t)n, 0U, false);
}

/* ---- pathconf / truncate ---- */

static void ios_do_pathconf(a32_ctx_t *ctx, uint32_t p_va, uint32_t name)
{
    char host[IOS_BSD_PATH_MAX];
    long r;
    int rc = ios_resolve(ctx, p_va, host, sizeof(host));
    if (rc != 0) {
        a32_ios_bsd_err(ctx, -rc);
        return;
    }
    errno = 0;
    r = pathconf(host, (int)name);
    if (r < 0 && errno != 0) {
        ios_fail(ctx);
        return;
    }
    a32_ios_bsd_ok(ctx, (uint32_t)r, 0U, false);
}

static void ios_do_truncate(a32_ctx_t *ctx, uint32_t p_va, uint32_t lo,
    uint32_t hi)
{
    char host[IOS_BSD_PATH_MAX];
    off_t len = (off_t)((uint64_t)lo | ((uint64_t)hi << 32));
    int rc = ios_resolve(ctx, p_va, host, sizeof(host));
    if (rc != 0) {
        a32_ios_bsd_err(ctx, -rc);
        return;
    }
    if (truncate(host, len) != 0) {
        ios_fail(ctx);
        return;
    }
    a32_ios_bsd_ok(ctx, 0U, 0U, false);
}

/* ---- Public FS dispatcher (304 lives in ios_threads.c: psynch_cvsignal) ---- */

int a32_ios_dispatch_bsd_fs(a32_ctx_t *ctx, uint32_t bsd_nr,
    const uint32_t *args, bool *out_handled)
{
    off_t len;
    if (out_handled != NULL) {
        *out_handled = false;
    }
    if (ctx == NULL || args == NULL) {
        return 0;
    }
    switch (bsd_nr) {
    case 188U: ios_do_stat(ctx, args[0], args[1], false, false); break;
    case 189U: ios_do_fstat(ctx, args[0], args[1], false); break;
    case 190U: ios_do_stat(ctx, args[0], args[1], false, true); break;
    case 338U: ios_do_stat(ctx, args[0], args[1], true, false); break;
    case 339U: ios_do_fstat(ctx, args[0], args[1], true); break;
    case 340U: ios_do_stat(ctx, args[0], args[1], true, true); break;
    case 469U: ios_do_fstatat(ctx, args[0], args[1], args[2], args[3]); break;
    case 345U: ios_do_statfs(ctx, args[0], args[1], false, 0); break;
    case 346U: ios_do_statfs(ctx, 0U, args[1], true, (int)(int32_t)args[0]); break;
    case 347U: ios_do_getfsstat(ctx, args[0], args[1]); break;
    case 344U: ios_do_getdirentries64(ctx, args[0], args[1], args[2], args[3]); break;
    case 9U: ios_do_link(ctx, args[0], args[1]); break;
    case 471U: ios_do_linkat(ctx, args); break;
    case 10U: ios_do_unlink(ctx, args[0]); break;
    case 472U: ios_do_unlinkat(ctx, args[0], args[1], args[2]); break;
    case 12U: ios_do_chdir(ctx, args[0]); break;
    case 13U:
        if (fchdir((int)(int32_t)args[0]) != 0) { ios_fail(ctx); }
        else { a32_ios_bsd_ok(ctx, 0U, 0U, false); }
        break;
    case 15U: ios_do_chmod(ctx, args[0], args[1]); break;
    case 124U:
        if (fchmod((int)(int32_t)args[0], (mode_t)args[1]) != 0) { ios_fail(ctx); }
        else { a32_ios_bsd_ok(ctx, 0U, 0U, false); }
        break;
    case 16U: ios_do_chown(ctx, args[0], args[1], args[2]); break;
    case 123U:
        if (fchown((int)(int32_t)args[0], (uid_t)args[1], (gid_t)args[2]) != 0) { ios_fail(ctx); }
        else { a32_ios_bsd_ok(ctx, 0U, 0U, false); }
        break;
    case 33U: ios_do_access(ctx, args[0], args[1]); break;
    case 466U: ios_do_faccessat(ctx, args[0], args[1], args[2], args[3]); break;
    case 57U: ios_do_symlink(ctx, args[0], args[1]); break;
    case 474U: ios_do_symlinkat(ctx, args[0], args[1], args[2]); break;
    case 58U: ios_do_readlink(ctx, args[0], args[1], args[2], false, 0); break;
    case 473U: ios_do_readlink(ctx, args[1], args[2], args[3], true, ios_atfd(args[0])); break;
    case 60U: a32_ios_bsd_ok(ctx, (uint32_t)umask((mode_t)(args[0] & 0777U)), 0U, false); break;
    case 95U: case 408U:
        if (fsync((int)(int32_t)args[0]) != 0) { ios_fail(ctx); }
        else { a32_ios_bsd_ok(ctx, 0U, 0U, false); }
        break;
    case 128U: ios_do_rename(ctx, args[0], args[1]); break;
    case 465U: ios_do_renameat(ctx, args); break;
    case 136U: ios_do_mkdir(ctx, args[0], args[1]); break;
    case 475U: ios_do_mkdirat(ctx, args[0], args[1], args[2]); break;
    case 137U: ios_do_rmdir(ctx, args[0]); break;
    case 191U: ios_do_pathconf(ctx, args[0], args[1]); break;
    case 192U: {
        long r;
        errno = 0; r = fpathconf((int)(int32_t)args[0], (int)args[1]);
        if (r < 0 && errno != 0) { ios_fail(ctx); }
        else { a32_ios_bsd_ok(ctx, (uint32_t)r, 0U, false); }
        break;
    }
    case 200U: ios_do_truncate(ctx, args[0], args[1], args[2]); break;
    case 201U:
        len = (off_t)((uint64_t)args[1] | ((uint64_t)args[2] << 32));
        if (ftruncate((int)(int32_t)args[0], len) != 0) { ios_fail(ctx); }
        else { a32_ios_bsd_ok(ctx, 0U, 0U, false); }
        break;
    default:
        return 0;
    }
    if (out_handled != NULL) {
        *out_handled = true;
    }
    return 0;
}
