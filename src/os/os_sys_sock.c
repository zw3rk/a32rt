/*
 * src/os/os_sys_sock.c - File I/O, directory, socket, select/poll, and robust-list syscalls
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "os/sys_priv.h"

#define SIGFD_MAX 32
static struct { int fd; uint64_t mask; int flags; bool used; } g_sigfd[SIGFD_MAX];
void a32_sigfd_track(int fd, uint64_t mask, int flags)
{
    for (int i = 0; i < SIGFD_MAX; i++) {
        if (g_sigfd[i].used && g_sigfd[i].fd == fd) {
            g_sigfd[i].mask = mask; g_sigfd[i].flags = flags; return;
        }
    }
    for (int i = 0; i < SIGFD_MAX; i++) {
        if (!g_sigfd[i].used) { g_sigfd[i].used = true; g_sigfd[i].fd = fd; g_sigfd[i].mask = mask; g_sigfd[i].flags = flags; return; }
    }
}
void a32_sigfd_untrack(int fd)
{
    for (int i = 0; i < SIGFD_MAX; i++) {
        if (g_sigfd[i].used && g_sigfd[i].fd == fd) g_sigfd[i].used = false;
    }
}
bool a32_sigfd_lookup(int fd, uint64_t *mask_out, int *flags_out)
{
    for (int i = 0; i < SIGFD_MAX; i++) {
        if (g_sigfd[i].used && g_sigfd[i].fd == fd) {
            if (mask_out) *mask_out = g_sigfd[i].mask;
            if (flags_out) *flags_out = g_sigfd[i].flags;
            return true;
        }
    }
    return false;
}

int a32_sys_dispatch_sock(a32_ctx_t *ctx)
{
    uint32_t nr = ctx->regs[7];
    uint32_t a0 = ctx->regs[0];
    uint32_t a1 = ctx->regs[1];
    uint32_t a2 = ctx->regs[2];
    uint32_t a3 = ctx->regs[3];
    uint32_t a4 = ctx->regs[4];
    uint32_t a5 = ctx->regs[5];
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;

    switch (nr) {
    case ARM_NR_set_robust_list:
        FAIL_EINVAL_IF(a1 != 12U);
        ctx->robust_list_head = a0;
        ctx->robust_list_len  = a1;
        ctx->regs[0] = 0U;
        break;

    case ARM_NR_get_robust_list: {
        FAIL_EINVAL_IF((int32_t)a0 < 0);
        FAIL_EFAULT_IF(!a32_guest_range_ok(ctx, a1, 4U, PROT_WRITE) ||
                       !a32_guest_range_ok(ctx, a2, 4U, PROT_WRITE));
        int self_tid = (ctx->host_tid != 0) ? ctx->host_tid : (int)getpid();
        if (a0 != 0U && (int)a0 != self_tid) {
            FAIL_ERR((kill((pid_t)a0, 0) < 0 && errno == ESRCH) ? ESRCH : EPERM);
        }
        uint32_t h = ctx->robust_list_head;
        uint32_t l = (ctx->robust_list_len != 0U) ? ctx->robust_list_len : 12U;
        (void)guest_write(ctx, a1, &h, sizeof(h));
        (void)guest_write(ctx, a2, &l, sizeof(l));
        ctx->regs[0] = 0U;
        break;
    }

    case ARM_NR_pread64:
    case ARM_NR_pwrite64: {
        FAIL_EFAULT_IF(a2 > 0U && a1 < 0x1000U);
        off_t pos = (off_t)u64_from_u32(a4, a5);
        ssize_t r = (nr == ARM_NR_pread64)
                    ? pread((int)a0, a32_guest_ptr(ctx, a1), (size_t)a2, pos)
                    : pwrite((int)a0, a32_guest_ptr(ctx, a1), (size_t)a2, pos);
        if (nr == ARM_NR_pwrite64 && r > 0) {
            (void)a32_vmfile_sync_fd((int)a0);
        } else if (nr == ARM_NR_pread64 && r > 0) {
            a32_vm32_copyout_invalidate_exec(ctx, a1, (uint32_t)r);
        }
        ctx->regs[0] = host_ret(r);
        break;
    }

    case ARM_NR_truncate:
    case ARM_NR_truncate64: {
        char resolved[PATH_MAX];
        RESOLVE_AT(AT_FDCWD, a0, resolved, true);
        if (nr == ARM_NR_truncate) {
            FAIL_EINVAL_IF((int32_t)a1 < 0);
        }
        off_t len = (nr == ARM_NR_truncate64)
                    ? (off_t)u64_from_u32(a2, a3)
                    : (off_t)(int32_t)a1;
        {
            int tr = truncate(resolved, len);
            if (tr == 0) {
                (void)a32_vmfile_sync_path(resolved);
            }
            ctx->regs[0] = host_ret(tr);
        }
        break;
    }

    case ARM_NR_ftruncate:
    case ARM_NR_ftruncate64: {
        if (nr == ARM_NR_ftruncate) {
            FAIL_EINVAL_IF((int32_t)a1 < 0);
        }
        off_t len = (nr == ARM_NR_ftruncate64)
                    ? (off_t)u64_from_u32(a2, a3)
                    : (off_t)(int32_t)a1;
        {
            int fr = ftruncate((int)a0, len);
            if (fr == 0) {
                (void)a32_vmfile_sync_fd((int)a0);
            }
            ctx->regs[0] = host_ret(fr);
        }
        break;
    }

    case ARM_NR_sched_setaffinity:
        FAIL_EFAULT_IF(a2 < 0x1000U);
#if defined(__linux__)
        ctx->regs[0] = host_ret(syscall(SYS_sched_setaffinity, (pid_t)a0,
                                        (unsigned int)a1, a32_guest_ptr(ctx, a2)));
#else
        ctx->regs[0] = 0U;
#endif
        break;

    case ARM_NR_sched_getaffinity: {
        FAIL_EFAULT_IF(a2 < 0x1000U);
        FAIL_EINVAL_IF((a1 & 3U) != 0U || a1 == 0U);
#if defined(__linux__)
        if ((a1 & 7U) == 4U && a1 <= 1020U) {
            uint64_t kbuf[128] = { 0 };
            long r = syscall(SYS_sched_getaffinity, (pid_t)a0,
                             (unsigned int)(a1 + 4U), kbuf);
            if (r < 0) {
                FAIL_ERR(errno);
            }
            uint32_t out_len = ((uint32_t)r > a1) ? a1 : (uint32_t)r;
            memcpy(a32_guest_ptr(ctx, a2), kbuf, out_len);
            ctx->regs[0] = out_len;
            break;
        }
        ctx->regs[0] = host_ret(syscall(SYS_sched_getaffinity, (pid_t)a0,
                                        (unsigned int)a1, a32_guest_ptr(ctx, a2)));
#else
        uint32_t mask = 1U;
        memcpy(a32_guest_ptr(ctx, a2), &mask, 4U);
        ctx->regs[0] = 4U;
#endif
        break;
    }

    case ARM_NR_sendfile64:
        FAIL_EFAULT_IF(a2 != 0U && a2 < 0x1000U);
#if defined(__linux__)
        ctx->regs[0] = host_ret(sendfile((int)a0, (int)a1,
                                         a2 ? (off_t *)a32_guest_ptr(ctx, a2) : NULL,
                                         (size_t)a3));
#else
        FAIL_ERR(ENOSYS);
#endif
        break;

    case ARM_NR_statx: {
#if defined(__linux__)
        char resolved[PATH_MAX];
        bool follow = ((a2 & AT_SYMLINK_NOFOLLOW) == 0U);
        int dirfd = translate_at_fd(a0);
        FAIL_EFAULT_IF(!a32_guest_range_ok(ctx, a4, 256U, PROT_WRITE));
        RESOLVE_AT(dirfd, a1, resolved, follow);
        int flags = (strcmp(resolved, "/tmp") == 0) ? (int)(a2 & ~(uint32_t)AT_SYMLINK_NOFOLLOW) : (int)a2;
        long r = syscall(SYS_statx, dirfd, resolved, flags, (unsigned int)a3, a32_guest_ptr(ctx, a4));
        if (r == 0 && ctx->has_host_info) {
            uint32_t *stx32 = (uint32_t *)a32_guest_ptr(ctx, a4);
            if (stx32[5] != 0U) {
                stx32[5] = (uint32_t)geteuid();
                stx32[6] = (uint32_t)getegid();
            }
        }
        ctx->regs[0] = host_ret(r);
#else
        /* Darwin statx fallback via fstatat (unblocks dynamic linking). */
        char resolved[PATH_MAX];
        bool follow = ((a2 & AT_SYMLINK_NOFOLLOW) == 0U);
        int dirfd = translate_at_fd(a0);
        FAIL_EFAULT_IF(!a32_guest_range_ok(ctx, a4, 256U, PROT_WRITE));
        FAIL_EFAULT_IF(a1 == 0U);
        struct stat st;
        char first = 0;
        FAIL_EFAULT_IF(!guest_read(ctx, &first, a1, 1U));
        if (first == '\0' && (((uint32_t)a2) & 0x1000U) != 0U) {
            if (dirfd == AT_FDCWD) {
                if (stat(".", &st) != 0) FAIL_ERR(errno);
            } else if (fstat(dirfd, &st) != 0) {
                FAIL_ERR(errno);
            }
        } else {
            RESOLVE_AT(dirfd, a1, resolved, follow);
            int stflags = follow ? 0 : AT_SYMLINK_NOFOLLOW;
            if (fstatat(dirfd, resolved, &st, stflags) != 0) {
                FAIL_ERR(errno);
            }
        }
        uint8_t *stxb = (uint8_t *)a32_guest_ptr(ctx, a4);
        memset(stxb, 0, 256U);
        /* Linux statx (LE): atime@64 btime@80 ctime@96 mtime@112. */
        uint32_t stx_mask = ((uint32_t)a3) & 0xFFFU; /* BASIC_STATS|BTIME */
        uint32_t stx_uid = (uint32_t)st.st_uid;
        uint32_t stx_gid = (uint32_t)st.st_gid;
        if (ctx->has_host_info) {
            stx_uid = (uint32_t)geteuid();
            stx_gid = (uint32_t)getegid();
        }
        uint16_t stx_mode = (uint16_t)st.st_mode;
        uint64_t stx_ino = (uint64_t)st.st_ino;
        uint64_t stx_size = (uint64_t)st.st_size;
        uint64_t stx_blocks = (uint64_t)st.st_blocks;
        uint32_t stx_blksize = (uint32_t)st.st_blksize;
        uint32_t stx_nlink = (uint32_t)st.st_nlink;
        memcpy(stxb + 0, &stx_mask, 4U);
        memcpy(stxb + 4, &stx_blksize, 4U);
        memcpy(stxb + 16, &stx_nlink, 4U);
        memcpy(stxb + 20, &stx_uid, 4U);
        memcpy(stxb + 24, &stx_gid, 4U);
        memcpy(stxb + 28, &stx_mode, 2U);
        memcpy(stxb + 32, &stx_ino, 8U);
        memcpy(stxb + 40, &stx_size, 8U);
        memcpy(stxb + 48, &stx_blocks, 8U);
        int64_t secs[4] = { (int64_t)st.st_atimespec.tv_sec, (int64_t)st.st_birthtimespec.tv_sec,
                            (int64_t)st.st_ctimespec.tv_sec, (int64_t)st.st_mtimespec.tv_sec };
        uint32_t nsecs[4] = { (uint32_t)st.st_atimespec.tv_nsec, (uint32_t)st.st_birthtimespec.tv_nsec,
                              (uint32_t)st.st_ctimespec.tv_nsec, (uint32_t)st.st_mtimespec.tv_nsec };
        for (unsigned ti = 0U; ti < 4U; ti++) {
            memcpy(stxb + 64 + ti * 16, &secs[ti], 8U);
            memcpy(stxb + 72 + ti * 16, &nsecs[ti], 4U);
        }
        /* Darwin dev_t: major bits[31:24], minor bits[23:0]. */
        uint32_t rdev_major = (uint32_t)(((uint32_t)st.st_rdev >> 24) & 0xFFU);
        uint32_t rdev_minor = (uint32_t)((uint32_t)st.st_rdev & 0xFFFFFFU);
        uint32_t dev_major = (uint32_t)(((uint32_t)st.st_dev >> 24) & 0xFFU);
        uint32_t dev_minor = (uint32_t)((uint32_t)st.st_dev & 0xFFFFFFU);
        memcpy(stxb + 128, &rdev_major, 4U);
        memcpy(stxb + 132, &rdev_minor, 4U);
        memcpy(stxb + 136, &dev_major, 4U);
        memcpy(stxb + 140, &dev_minor, 4U);
        ctx->regs[0] = 0U;
#endif
        break;
    }

    case ARM_NR_unshare:
#if defined(__linux__)
        ctx->regs[0] = (a0 == 0U) ? 0U : host_ret(syscall(SYS_unshare, (int)a0));
#else
        FAIL_ERR(ENOSYS);
#endif
        break;

    case ARM_NR_setns:
#if defined(__linux__)
        ctx->regs[0] = host_ret(syscall(SYS_setns, (int)a0, (int)a1));
#else
        FAIL_ERR(ENOSYS);
#endif
        break;

    case ARM_NR_seccomp:
    case ARM_NR_rseq:
        FAIL_ERR(ENOSYS);

    case ARM_NR_close_range:
        FAIL_EINVAL_IF(a0 > a1 || (a2 & ~6U /* CLOSE_RANGE_UNSHARE(2) | CLOSE_RANGE_CLOEXEC(4) */) != 0U);
#if defined(__linux__)
        ctx->regs[0] = host_ret(syscall(SYS_close_range, (unsigned int)a0, (unsigned int)a1, (unsigned int)a2));
#else
        FAIL_ERR(ENOSYS);
#endif
        break;

    case ARM_NR_memfd_create: {
#if defined(__linux__)
        FAIL_EINVAL_IF((a1 & ~0xFC00001FU) != 0U || ((a1 & 4U) == 0U && (a1 & ~0x1FU) != 0U));
        char mname[250];
        uint32_t mn = 0U;
        for (; mn < 250U; mn++) {
            char c = 0;
            if (!guest_read(ctx, &c, a0 + mn, 1U)) FAIL_ERR(EFAULT);
            mname[mn] = c;
            if (c == '\0') break;
        }
        FAIL_EINVAL_IF(mn >= 250U);
        ctx->regs[0] = host_ret(syscall(SYS_memfd_create, mname, (unsigned int)a1));
#else
        FAIL_ERR(ENOSYS);
#endif
        break;
    }

    case ARM_NR_inotify_init1:
#if defined(__linux__)
        ctx->regs[0] = host_ret(syscall(SYS_inotify_init1, (int)a0));
#else
        FAIL_ERR(ENOSYS);
#endif
        break;

    case ARM_NR_signalfd4: {
        uint64_t gmask = 0ULL;
        FAIL_EINVAL_IF(a2 != 8U);
        FAIL_EFAULT_IF(!guest_read(ctx, &gmask, a1, sizeof(gmask)));
        for (int s = 1; s <= 64; s++) {
            if ((gmask >> (s - 1)) & 1ULL) a32_signals_ensure_host_handler(s);
        }
#if defined(__linux__)
        if ((int)a0 == -1) {
            sigset_t hmask;
            memset(&hmask, 0, sizeof(hmask));
            memcpy(&hmask, &gmask, sizeof(uint64_t));
            int hfd = (int)syscall(SYS_signalfd4, -1, &hmask, (size_t)8, (int)a3);
            if (hfd < 0) FAIL_ERR(errno);
            a32_sigfd_track(hfd, gmask, (int)a3);
            ctx->regs[0] = (uint32_t)hfd;
        } else {
            sigset_t hmask;
            memset(&hmask, 0, sizeof(hmask));
            memcpy(&hmask, &gmask, sizeof(uint64_t));
            int hfd = (int)syscall(SYS_signalfd4, (int)a0, &hmask, (size_t)8, (int)a3);
            if (hfd < 0) FAIL_ERR(errno);
            a32_sigfd_track(hfd, gmask, (int)a3);
            ctx->regs[0] = (uint32_t)hfd;
        }
#else
        int hfd = (int)a0;
        if ((int)a0 == -1) {
            hfd = open("/dev/null", O_RDONLY);
            if (hfd < 0) FAIL_ERR(errno);
            if ((a3 & 0x800U) != 0U) (void)fcntl(hfd, F_SETFL, O_NONBLOCK);
        }
        a32_sigfd_track(hfd, gmask, (int)a3);
        ctx->regs[0] = (uint32_t)hfd;
#endif
        break;
    }

    case ARM_NR_splice:
    case ARM_NR_copy_file_range: {
#if defined(__linux__)
        FAIL_EFAULT_IF((a1 != 0U && !a32_guest_range_ok(ctx, a1, 8U, PROT_READ | PROT_WRITE)) ||
                       (a3 != 0U && !a32_guest_range_ok(ctx, a3, 8U, PROT_READ | PROT_WRITE)));
        loff_t *p_off_in  = a1 ? (loff_t *)a32_guest_ptr(ctx, a1) : NULL;
        loff_t *p_off_out = a3 ? (loff_t *)a32_guest_ptr(ctx, a3) : NULL;
        long h_nr = (nr == ARM_NR_splice) ? SYS_splice : SYS_copy_file_range;
        ctx->regs[0] = host_ret(syscall(h_nr, (int)a0, p_off_in,
                                        (int)a2, p_off_out, (size_t)a4, (unsigned int)a5));
#else
        FAIL_ERR(ENOSYS);
#endif
        break;
    }

    case ARM_NR_sync_file_range2: {
#if defined(__linux__)
        off_t offset = (off_t)u64_from_u32(a2, a3);
        off_t nbytes = (off_t)u64_from_u32(a4, a5);
        ctx->regs[0] = host_ret(syscall(SYS_sync_file_range, (int)a0, offset, nbytes, (unsigned int)a1));
#else
        ctx->regs[0] = 0U;
#endif
        break;
    }

    case ARM_NR_tee:
#if defined(__linux__)
        ctx->regs[0] = host_ret(syscall(SYS_tee, (int)a0, (int)a1, (size_t)a2, (unsigned int)a3));
#else
        FAIL_ERR(ENOSYS);
#endif
        break;

    case ARM_NR_fallocate: {
#if defined(__linux__)
        off_t offset = (off_t)u64_from_u32(a2, a3);
        off_t len    = (off_t)u64_from_u32(a4, a5);
        ctx->regs[0] = host_ret(syscall(SYS_fallocate, (int)a0, (int)a1, offset, len));
#else
        FAIL_ERR(ENOSYS);
#endif
        break;
    }

    /* Socket syscalls */
    case ARM_NR_socket:
        ctx->regs[0] = host_ret(socket((int)a0, (int)a1, (int)a2));
        break;

    case ARM_NR_bind:
        ctx->regs[0] = host_ret(bind((int)a0, a32_guest_ptr(ctx, a1), (socklen_t)a2));
        break;

    case ARM_NR_connect:
        ctx->regs[0] = host_ret(connect((int)a0, a32_guest_ptr(ctx, a1), (socklen_t)a2));
        break;

    case ARM_NR_listen:
        ctx->regs[0] = host_ret(listen((int)a0, (int)a1));
        break;

    case ARM_NR_accept:
    case ARM_NR_accept4: {
        struct sockaddr *sa = a1 ? a32_guest_ptr(ctx, a1) : NULL;
        socklen_t *salen    = a2 ? (socklen_t *)a32_guest_ptr(ctx, a2) : NULL;
#if defined(__linux__)
        int r = (nr == ARM_NR_accept4) ? accept4((int)a0, sa, salen, (int)a3)
                                       : accept((int)a0, sa, salen);
#else
        int r = accept((int)a0, sa, salen);
#endif
        ctx->regs[0] = host_ret(r);
        break;
    }

    case ARM_NR_getsockname:
        ctx->regs[0] = host_ret(getsockname((int)a0, a32_guest_ptr(ctx, a1),
                                            (socklen_t *)a32_guest_ptr(ctx, a2)));
        break;

    case ARM_NR_getpeername:
        ctx->regs[0] = host_ret(getpeername((int)a0, a32_guest_ptr(ctx, a1),
                                            (socklen_t *)a32_guest_ptr(ctx, a2)));
        break;

    case ARM_NR_socketpair:
        FAIL_EFAULT_IF(a3 < 0x1000U);
        ctx->regs[0] = host_ret(socketpair((int)a0, (int)a1, (int)a2,
                                           (int *)a32_guest_ptr(ctx, a3)));
        break;

    case ARM_NR_send:
    case ARM_NR_sendto: {
        /* A32-R13-4: buffer and address must sit in the guest window. */
        FAIL_EFAULT_IF(a2 != 0U && !a32_guest_range_ok(ctx, a1, a2, PROT_READ));
        const struct sockaddr *sa = NULL;
        socklen_t salen           = 0U;
        if (nr == ARM_NR_sendto && a4 != 0U) {
            FAIL_EINVAL_IF((int32_t)a5 < 0);
            FAIL_EFAULT_IF(a5 != 0U && !a32_guest_range_ok(ctx, a4, a5, PROT_READ));
            sa    = a32_guest_ptr(ctx, a4);
            salen = (socklen_t)a5;
        }
        ctx->regs[0] = host_ret(sendto((int)a0, a2 ? a32_guest_ptr(ctx, a1) : NULL,
                                       (size_t)a2, (int)a3, sa, salen));
        break;
    }

    case ARM_NR_recv:
    case ARM_NR_recvfrom: {
        /* A32-R13-4: buffer, address and length cell stay in-window. */
        FAIL_EFAULT_IF(a2 != 0U && !a32_guest_range_ok(ctx, a1, a2, PROT_WRITE));
        struct sockaddr *sa = NULL;
        socklen_t *sp = NULL, hlen = 0U;
        if (nr == ARM_NR_recvfrom && a4 != 0U) {
            socklen_t glen = 0U;
            FAIL_EFAULT_IF(a5 == 0U ||
                           !a32_guest_range_ok(ctx, a5, sizeof(socklen_t),
                                               PROT_READ | PROT_WRITE));
            memcpy(&glen, a32_guest_ptr(ctx, a5), sizeof(glen));
            FAIL_EINVAL_IF((int32_t)glen < 0);
            FAIL_EFAULT_IF(glen != 0U &&
                           !a32_guest_range_ok(ctx, a4, (uint32_t)glen, PROT_WRITE));
            sa   = a32_guest_ptr(ctx, a4);
            hlen = glen;
            sp   = &hlen;
        }
        ssize_t r = recvfrom((int)a0, a2 ? a32_guest_ptr(ctx, a1) : NULL,
                             (size_t)a2, (int)a3, sa, sp);
        if (r >= 0 && sp != NULL) {
            socklen_t back = hlen;
            memcpy(a32_guest_ptr(ctx, a5), &back, sizeof(back));
        }
        if (r > 0) a32_vm32_copyout_invalidate_exec(ctx, a1, (uint32_t)r);
        ctx->regs[0] = host_ret(r);
        break;
    }

    case ARM_NR_shutdown:
        ctx->regs[0] = host_ret(shutdown((int)a0, (int)a1));
        break;

    case ARM_NR_setsockopt: {
        FAIL_EFAULT_IF(a3 != 0U && a3 < 0x1000U);
        if (a1 == 1U /* SOL_SOCKET */ &&
            (a2 == 20U /* SO_RCVTIMEO_OLD */ || a2 == 21U /* SO_SNDTIMEO_OLD */ ||
             a2 == 66U /* SO_RCVTIMEO_NEW */ || a2 == 67U /* SO_SNDTIMEO_NEW */)) {
            bool time64 = (a2 >= 66U);
            uint32_t min_len = time64 ? 16U : 8U;
            FAIL_EINVAL_IF((int32_t)a4 < (int32_t)min_len);
            struct timeval tv64;
            FAIL_EFAULT_IF(!read_guest_timeval(ctx, a3, time64, &tv64));
            if (tv64.tv_usec < 0 || tv64.tv_usec >= 1000000) {
                FAIL_ERR(EDOM);
            }
            int host_opt = (a2 == 20U || a2 == 66U) ? 20 : 21;
            ctx->regs[0] = host_ret(setsockopt((int)a0, 1, host_opt,
                                               &tv64, (socklen_t)sizeof(tv64)));
            break;
        }
        ctx->regs[0] = host_ret(setsockopt((int)a0, (int)a1, (int)a2,
                                           a3 ? a32_guest_ptr(ctx, a3) : NULL, (socklen_t)a4));
        break;
    }

    case ARM_NR_getsockopt: {
        FAIL_EFAULT_IF((a3 != 0U && a3 < 0x1000U) || (a4 != 0U && a4 < 0x1000U));
        if (a1 == 1U /* SOL_SOCKET */ &&
            (a2 == 20U /* SO_RCVTIMEO_OLD */ || a2 == 21U /* SO_SNDTIMEO_OLD */ ||
             a2 == 66U /* SO_RCVTIMEO_NEW */ || a2 == 67U /* SO_SNDTIMEO_NEW */) &&
            a3 != 0U && a4 != 0U) {
            bool time64 = (a2 >= 66U);
            FAIL_EFAULT_IF(!a32_guest_range_ok(ctx, a4, 4U, PROT_READ | PROT_WRITE));
            socklen_t guest_len = *(socklen_t *)a32_guest_ptr(ctx, a4);
            FAIL_EINVAL_IF((int32_t)guest_len < 0);
            struct timeval tv64 = { 0 };
            socklen_t host_len = (socklen_t)sizeof(tv64);
            int host_opt = (a2 == 20U || a2 == 66U) ? 20 : 21;
            int r = getsockopt((int)a0, 1, host_opt, &tv64, &host_len);
            if (r == 0) {
                uint8_t out_buf[16];
                socklen_t full_len = time64 ? 16U : 8U;
                if (time64) {
                    arm_timeval64_t tv = { (int64_t)tv64.tv_sec, (int64_t)tv64.tv_usec };
                    memcpy(out_buf, &tv, sizeof(tv));
                } else {
                    arm_timeval32_t tv = { (int32_t)tv64.tv_sec, (int32_t)tv64.tv_usec };
                    memcpy(out_buf, &tv, sizeof(tv));
                }
                socklen_t copy_len = (guest_len < full_len) ? guest_len : full_len;
                FAIL_EFAULT_IF(copy_len > 0U && !a32_guest_range_ok(ctx, a3, (uint32_t)copy_len, PROT_WRITE));
                memcpy(a32_guest_ptr(ctx, a3), out_buf, (size_t)copy_len);
                *(socklen_t *)a32_guest_ptr(ctx, a4) = copy_len;
            }
            ctx->regs[0] = host_ret(r);
            break;
        }
        ctx->regs[0] = host_ret(getsockopt((int)a0, (int)a1, (int)a2,
                                           a3 ? a32_guest_ptr(ctx, a3) : NULL,
                                           a4 ? (socklen_t *)a32_guest_ptr(ctx, a4) : NULL));
        break;
    }

    case ARM_NR_sendmsg:
    case ARM_NR_recvmsg: {
        FAIL_EFAULT_IF(!a32_guest_range_ok(ctx, a1, (uint32_t)sizeof(arm_msghdr32_t),
                                           (nr == ARM_NR_recvmsg) ? (PROT_READ | PROT_WRITE) : PROT_READ));
        arm_msghdr32_t *gm = (arm_msghdr32_t *)a32_guest_ptr(ctx, a1);
        arm_msghdr32_t hdr = *gm;
        if (hdr.msg_iovlen > 1024U) {
            FAIL_ERR(EMSGSIZE);
        }
        FAIL_EINVAL_IF(nr == ARM_NR_sendmsg && hdr.msg_controllen > 0U && hdr.msg_controllen < 12U);
        FAIL_EFAULT_IF((hdr.msg_name != 0U && hdr.msg_name < 0x1000U) ||
                       (hdr.msg_controllen != 0U &&
                        !a32_guest_range_ok(ctx, hdr.msg_control, hdr.msg_controllen,
                                            (nr == ARM_NR_recvmsg) ? PROT_WRITE : PROT_READ)));
        struct msghdr hm = {
            .msg_name    = (hdr.msg_name != 0U) ? a32_guest_ptr(ctx, hdr.msg_name) : NULL,
            .msg_namelen = (socklen_t)hdr.msg_namelen,
            .msg_flags   = (int)hdr.msg_flags,
        };

        _Alignas(8) uint8_t cbuf_st[1024] = { 0 };
        uint8_t *cbuf64 = cbuf_st;
        size_t cbuf_cap = sizeof(cbuf_st);
        uint8_t *cbuf_dyn = NULL;
        uint8_t *csnap = NULL;
        uint32_t ctl_len = hdr.msg_controllen;
        if (hdr.msg_control != 0U && ctl_len > 0U) {
            if (nr == ARM_NR_sendmsg) {
                csnap = (uint8_t *)malloc(ctl_len);
                if (csnap == NULL) FAIL_ERR(ENOBUFS);
                memcpy(csnap, a32_guest_ptr(ctx, hdr.msg_control), ctl_len);
                const uint8_t *gctl = csnap;
                uint32_t t32 = 0U;
                size_t need64 = 0U;
                while (t32 + 12U <= ctl_len) {
                    uint32_t l32;
                    memcpy(&l32, gctl + t32, 4);
                    if (l32 < 12U || l32 > ctl_len - t32) { free(csnap); FAIL_ERR(EINVAL); }
                    need64 += CMSG_SPACE((size_t)(l32 - 12U));
                    t32 += (l32 + 3U) & ~3U;
                }
                if (need64 > (64U * 1024U)) { free(csnap); FAIL_ERR(ENOBUFS); }
                if (need64 > cbuf_cap) {
                    cbuf_dyn = (uint8_t *)malloc(need64);
                    if (cbuf_dyn == NULL) { free(csnap); FAIL_ERR(ENOBUFS); }
                    cbuf64 = cbuf_dyn;
                    cbuf_cap = need64;
                }
                uint32_t off32 = 0U;
                size_t off64 = 0U;
                while (off32 + 12U <= ctl_len) {
                    uint32_t len32;
                    int32_t level32, type32;
                    memcpy(&len32, gctl + off32, 4);
                    if (len32 < 12U || len32 > ctl_len - off32) { free(csnap); FAIL_ERR(EINVAL); }
                    memcpy(&level32, gctl + off32 + 4U, 4);
                    memcpy(&type32, gctl + off32 + 8U, 4);
                    size_t payload_len = (size_t)(len32 - 12U);
                    size_t space64 = CMSG_SPACE(payload_len);
                    if (off64 + space64 > cbuf_cap) { free(csnap); FAIL_ERR(ENOBUFS); }
                    struct cmsghdr *cm64 = (struct cmsghdr *)(void *)(cbuf64 + off64);
                    cm64->cmsg_len   = CMSG_LEN(payload_len);
                    cm64->cmsg_level = (int)level32;
                    cm64->cmsg_type  = (int)type32;
                    memcpy(CMSG_DATA(cm64), gctl + off32 + 12U, payload_len);
                    off64 += space64;
                    off32 += (len32 + 3U) & ~3U;
                }
                hm.msg_control    = (off64 > 0U) ? cbuf64 : NULL;
                hm.msg_controllen = off64;
            } else {
                size_t cap64 = (size_t)hdr.msg_controllen * 2U;
                hm.msg_control    = cbuf64;
                hm.msg_controllen = (cap64 > cbuf_cap) ? cbuf_cap : cap64;
            }
        }

        struct iovec host_iov[16];
        struct iovec *hiov = NULL;
        uint32_t msg_prot = (nr == ARM_NR_recvmsg)
            ? (uint32_t)PROT_WRITE : (uint32_t)PROT_READ;
        int iov_rc = translate_guest_iov(ctx, hdr.msg_iov, (int)hdr.msg_iovlen,
                                         host_iov, 16U, &hiov, msg_prot);
        if (iov_rc < 0) {
            free(cbuf_dyn);
            free(csnap);
            ctx->regs[0] = (uint32_t)iov_rc;
            break;
        }
        hm.msg_iov    = (hdr.msg_iovlen > 0U) ? hiov : NULL;
        hm.msg_iovlen = (size_t)hdr.msg_iovlen;

        ssize_t r = (nr == ARM_NR_sendmsg) ? sendmsg((int)a0, &hm, (int)a2)
                                           : recvmsg((int)a0, &hm, (int)a2);
        if (r >= 0 && nr == ARM_NR_recvmsg) {
            guest_iov_invalidate(ctx, hiov, hdr.msg_iovlen);
            (void)a32_vm32_copyout_invalidate_exec(ctx, a1,
                (uint32_t)sizeof(arm_msghdr32_t));
            gm->msg_namelen = (uint32_t)hm.msg_namelen;
            gm->msg_flags   = (uint32_t)hm.msg_flags;
            if (hdr.msg_control != 0U) {
                uint8_t *gctl_out = (uint8_t *)a32_guest_ptr(ctx, hdr.msg_control);
                uint32_t off32 = 0U;
                size_t off64 = 0U;
                bool ctrunc = false;
                while (off64 + sizeof(struct cmsghdr) <= hm.msg_controllen) {
                    const struct cmsghdr *cm64 = (const struct cmsghdr *)(const void *)(cbuf64 + off64);
                    if (cm64->cmsg_len < sizeof(struct cmsghdr) ||
                        off64 + cm64->cmsg_len > hm.msg_controllen) {
                        break;
                    }
                    size_t payload_len = cm64->cmsg_len - sizeof(struct cmsghdr);
                    int32_t tv32[2];
                    const void *payload_src = CMSG_DATA(cm64);
                    if (cm64->cmsg_level == SOL_SOCKET &&
                        cm64->cmsg_type == 29 /* SCM_TIMESTAMP (time32) */ &&
                        payload_len == 16U) {
                        int64_t tv64[2];
                        memcpy(tv64, CMSG_DATA(cm64), 16U);
                        tv32[0] = (int32_t)tv64[0];
                        tv32[1] = (int32_t)tv64[1];
                        payload_src = tv32;
                        payload_len = 8U;
                    }
                    uint32_t len32 = 12U + (uint32_t)payload_len;
                    uint32_t space32 = (len32 + 3U) & ~3U;
                    if (!ctrunc && off32 + len32 <= hdr.msg_controllen) {
                        int32_t level32 = (int32_t)cm64->cmsg_level;
                        int32_t type32  = (int32_t)cm64->cmsg_type;
                        memcpy(gctl_out + off32, &len32, 4);
                        memcpy(gctl_out + off32 + 4U, &level32, 4);
                        memcpy(gctl_out + off32 + 8U, &type32, 4);
                        memcpy(gctl_out + off32 + 12U, payload_src, payload_len);
                        off32 += (off32 + space32 <= hdr.msg_controllen) ? space32 : len32;
                    } else {
                        ctrunc = true;
                        if (cm64->cmsg_level == SOL_SOCKET &&
                            cm64->cmsg_type == SCM_RIGHTS) {
                            size_t nfds = payload_len / sizeof(int);
                            const int *fds = (const int *)(const void *)CMSG_DATA(cm64);
                            for (size_t fi = 0U; fi < nfds; fi++) {
                                close(fds[fi]);
                            }
                        }
                        gm->msg_flags |= MSG_CTRUNC;
                    }
                    off64 += CMSG_ALIGN(cm64->cmsg_len);
                }
                if (off32 > 0U) {
                    (void)a32_vm32_copyout_invalidate_exec(ctx, hdr.msg_control, off32);
                }
                gm->msg_controllen = off32;
            }
        }
        if (hiov != host_iov) {
            free(hiov);
        }
        free(cbuf_dyn);
        free(csnap);
        ctx->regs[0] = host_ret(r);
        break;
    }

    /* Select, polling, epoll, eventfd, timerfd */
    case ARM_NR__newselect:
    case ARM_NR_pselect6:
    case ARM_NR_pselect6_time64: {
        int nfds = (int)a0;
        FAIL_EINVAL_IF(nfds < 0);
        size_t fd_bytes32 = (((size_t)nfds + 31U) / 32U) * 4U;
        uint32_t fd_chk = (fd_bytes32 > 0x1000000U) ? 0U : (uint32_t)fd_bytes32;
        FAIL_EINVAL_IF(fd_bytes32 > 0x1000000U);
        FAIL_EFAULT_IF((a1 != 0U && fd_bytes32 > 0U && !a32_guest_range_ok(ctx, a1, fd_chk, PROT_READ | PROT_WRITE)) ||
                       (a2 != 0U && fd_bytes32 > 0U && !a32_guest_range_ok(ctx, a2, fd_chk, PROT_READ | PROT_WRITE)) ||
                       (a3 != 0U && fd_bytes32 > 0U && !a32_guest_range_ok(ctx, a3, fd_chk, PROT_READ | PROT_WRITE)));
        struct timespec host_ts;
        struct timespec *ts_ptr = NULL;
        bool time64 = (nr == ARM_NR_pselect6_time64);
        if (a4 != 0U) {
            if (nr == ARM_NR__newselect) {
                struct timeval tv32;
                FAIL_EFAULT_IF(!read_guest_timeval(ctx, a4, false, &tv32));
                FAIL_EINVAL_IF(tv32.tv_sec < 0 || tv32.tv_usec < 0 || tv32.tv_usec >= 1000000);
                host_ts.tv_sec  = tv32.tv_sec;
                host_ts.tv_nsec = (long)tv32.tv_usec * 1000L;
            } else {
                FAIL_EFAULT_IF(!read_guest_timespec(ctx, a4, time64, &host_ts));
                FAIL_EINVAL_IF(host_ts.tv_sec < 0 || host_ts.tv_nsec < 0 || host_ts.tv_nsec >= 1000000000L);
            }
            ts_ptr = &host_ts;
        }

        struct {
            const void *ss;
            size_t      ss_len;
        } host_sig_pack;
        void    *sig_arg        = NULL;
        bool     has_temp_mask  = false;
        uint64_t saved_sig_mask = ctx->sig_mask;
        uint64_t temp_mask      = 0ULL;
        if (nr != ARM_NR__newselect && a5 != 0U) {
            uint32_t g_pack[2];
            FAIL_EFAULT_IF(!guest_read(ctx, g_pack, a5, sizeof(g_pack)));
            int sm_rc = pwait_sigmask_enter(ctx, g_pack[0], g_pack[1],
                                            &has_temp_mask, &saved_sig_mask, &temp_mask);
            if (sm_rc != 0) {
                if (sm_rc < 0) {
                    ctx->regs[0] = (uint32_t)sm_rc;
                }
                break;
            }
            host_sig_pack.ss     = has_temp_mask ? (const sigset_t *)&temp_mask : NULL;
            host_sig_pack.ss_len = (size_t)g_pack[1];
            sig_arg = &host_sig_pack;
        }

        uint64_t rset[16], wset[16], eset[16];
        void *r_ptr = NULL;
        void *w_ptr = NULL;
        void *e_ptr = NULL;
        uint8_t *r_dyn = NULL;
        uint8_t *w_dyn = NULL;
        uint8_t *e_dyn = NULL;
        size_t host_bytes = (((size_t)nfds + 63U) / 64U) * 8U;
        if (nfds <= 1024) {
            if (a1 != 0U) {
                memset(rset, 0, sizeof(rset));
                memcpy(rset, a32_guest_ptr(ctx, a1), fd_bytes32);
                r_ptr = rset;
            }
            if (a2 != 0U) {
                memset(wset, 0, sizeof(wset));
                memcpy(wset, a32_guest_ptr(ctx, a2), fd_bytes32);
                w_ptr = wset;
            }
            if (a3 != 0U) {
                memset(eset, 0, sizeof(eset));
                memcpy(eset, a32_guest_ptr(ctx, a3), fd_bytes32);
                e_ptr = eset;
            }
        } else {
            if (a1 != 0U) {
                r_dyn = (uint8_t *)malloc(host_bytes);
                if (r_dyn == NULL) {
                    FAIL_ERR(ENOMEM);
                }
                memset(r_dyn, 0, host_bytes);
                memcpy(r_dyn, a32_guest_ptr(ctx, a1), fd_bytes32);
                r_ptr = r_dyn;
            }
            if (a2 != 0U) {
                w_dyn = (uint8_t *)malloc(host_bytes);
                if (w_dyn == NULL) {
                    free(r_dyn);
                    FAIL_ERR(ENOMEM);
                }
                memset(w_dyn, 0, host_bytes);
                memcpy(w_dyn, a32_guest_ptr(ctx, a2), fd_bytes32);
                w_ptr = w_dyn;
            }
            if (a3 != 0U) {
                e_dyn = (uint8_t *)malloc(host_bytes);
                if (e_dyn == NULL) {
                    free(r_dyn);
                    free(w_dyn);
                    FAIL_ERR(ENOMEM);
                }
                memset(e_dyn, 0, host_bytes);
                memcpy(e_dyn, a32_guest_ptr(ctx, a3), fd_bytes32);
                e_ptr = e_dyn;
            }
        }

#if defined(__linux__)
        long r = syscall(SYS_pselect6, nfds, r_ptr, w_ptr, e_ptr, ts_ptr, sig_arg);
#else
        (void)sig_arg;
        long r = pselect(nfds, (fd_set *)r_ptr, (fd_set *)w_ptr, (fd_set *)e_ptr, ts_ptr, NULL);
#endif
        int saved_errno = errno;
        if (r >= 0) {
            if (nfds <= 1024) {
                if (a1 != 0U) {
                    memcpy(a32_guest_ptr(ctx, a1), rset, fd_bytes32);
                }
                if (a2 != 0U) {
                    memcpy(a32_guest_ptr(ctx, a2), wset, fd_bytes32);
                }
                if (a3 != 0U) {
                    memcpy(a32_guest_ptr(ctx, a3), eset, fd_bytes32);
                }
            } else {
                if (a1 != 0U) {
                    memcpy(a32_guest_ptr(ctx, a1), r_dyn, fd_bytes32);
                }
                if (a2 != 0U) {
                    memcpy(a32_guest_ptr(ctx, a2), w_dyn, fd_bytes32);
                }
                if (a3 != 0U) {
                    memcpy(a32_guest_ptr(ctx, a3), e_dyn, fd_bytes32);
                }
            }
        }
        free(r_dyn);
        free(w_dyn);
        free(e_dyn);
        if (a4 != 0U && (r >= 0 || saved_errno == EINTR)) {
            uint32_t ts_sz = time64 ? (uint32_t)sizeof(arm_timespec64_t)
                                    : (uint32_t)sizeof(arm_timespec32_t);
            a32_vm_sync_ctx(ctx);
            if (guest_range_mapped(ctx, a4, ts_sz, PROT_READ | PROT_WRITE)) {
                if (nr == ARM_NR__newselect) {
                    struct timeval tv;
                    tv.tv_sec  = host_ts.tv_sec;
                    tv.tv_usec = (suseconds_t)(host_ts.tv_nsec / 1000L);
                    (void)write_guest_timeval(ctx, a4, false, &tv);
                } else {
                    (void)write_guest_timespec(ctx, a4, time64, &host_ts);
                }
            }
        }
        errno = saved_errno;
        ctx->regs[0] = host_ret(r);
        pwait_sigmask_leave(ctx, has_temp_mask, saved_sig_mask, temp_mask);
        break;
    }

    case ARM_NR_poll:
        FAIL_EFAULT_IF(a1 > 0U && !a32_guest_range_ok(ctx, a0, 8U, PROT_READ | PROT_WRITE));
        ctx->regs[0] = host_ret(poll((a1 > 0U) ? (struct pollfd *)a32_guest_ptr(ctx, a0) : NULL,
                                     (nfds_t)a1, (int)a2));
        break;

    default:
        return a32_sys_dispatch_fs_arm(ctx);
    }
    return 0;
}
