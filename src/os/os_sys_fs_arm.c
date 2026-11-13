/*
 * src/os/os_sys_fs_arm.c - Epoll, timerfd, VFS/xattr, and ARM private syscalls
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "os/sys_priv.h"

#if defined(__linux__)
/* Full-extent writability check for the epoll_wait/epoll_pwait result
 * buffer (A32-R4-9): validating only the first event lets the kernel
 * write later events across a guest protection boundary on hosts whose
 * page granule is coarser than the guest's 4 KiB. Linux-only: the
 * epoll dispatch below is likewise guarded, and struct epoll_event is
 * incomplete on Darwin. */
static bool epoll_out_ok(a32_ctx_t *ctx, uint32_t base_va, uint32_t maxevents)
{
    uint64_t bytes =
        (uint64_t)maxevents * (uint64_t)sizeof(struct epoll_event);
    if (bytes == 0ULL || bytes > 0xFFFFFFFFULL) {
        return false;
    }
    return a32_guest_range_ok(ctx, base_va, (uint32_t)bytes,
        (uint32_t)PROT_WRITE);
}
#endif

int a32_sys_dispatch_fs_arm(a32_ctx_t *ctx)
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
    case ARM_NR_ppoll:
    case ARM_NR_ppoll_time64: {
        bool time64 = (nr == ARM_NR_ppoll_time64);
        FAIL_EFAULT_IF(a1 > 0U && !a32_guest_range_ok(ctx, a0, 8U, PROT_READ | PROT_WRITE));
        struct timespec host_ts;
        struct timespec *ts_ptr = NULL;
        if (a2 != 0U) {
            FAIL_EFAULT_IF(!read_guest_timespec(ctx, a2, time64, &host_ts));
            FAIL_EINVAL_IF(host_ts.tv_sec < 0 || host_ts.tv_nsec < 0 || host_ts.tv_nsec >= 1000000000L);
            ts_ptr = &host_ts;
        }
        bool     has_temp_mask  = false;
        uint64_t saved_sig_mask = ctx->sig_mask;
        uint64_t temp_mask      = 0ULL;
        int sm_rc = pwait_sigmask_enter(ctx, a3, a4, &has_temp_mask, &saved_sig_mask, &temp_mask);
        if (sm_rc != 0) {
            if (sm_rc < 0) {
                ctx->regs[0] = (uint32_t)sm_rc;
            }
            break;
        }
        struct pollfd *fds = (a1 > 0U) ? (struct pollfd *)a32_guest_ptr(ctx, a0) : NULL;
#if defined(__linux__)
        long r = syscall(SYS_ppoll, fds, (nfds_t)a1, ts_ptr,
                         has_temp_mask ? &temp_mask : NULL, (size_t)8);
#else
        int t_ms = ts_ptr ? (int)(ts_ptr->tv_sec * 1000 + ts_ptr->tv_nsec / 1000000) : -1;
        long r = poll(fds, (nfds_t)a1, t_ms);
#endif
        int saved_errno = errno;
        if (a2 != 0U && (r >= 0 || saved_errno == EINTR)) {
            uint32_t ts_sz = time64 ? (uint32_t)sizeof(arm_timespec64_t)
                                    : (uint32_t)sizeof(arm_timespec32_t);
            a32_vm_sync_ctx(ctx);
            if (guest_range_mapped(ctx, a2, ts_sz, PROT_READ | PROT_WRITE)) {
                (void)write_guest_timespec(ctx, a2, time64, &host_ts);
            }
        }
        errno = saved_errno;
        ctx->regs[0] = host_ret(r);
        pwait_sigmask_leave(ctx, has_temp_mask, saved_sig_mask, temp_mask);
        break;
    }

    case ARM_NR_epoll_create:
    case ARM_NR_epoll_create1:
    case ARM_NR_epoll_ctl:
    case ARM_NR_epoll_wait:
    case ARM_NR_epoll_pwait:
    case ARM_NR_eventfd:
    case ARM_NR_eventfd2:
    case ARM_NR_timerfd_create:
    case ARM_NR_timerfd_settime:
    case ARM_NR_timerfd_settime64:
    case ARM_NR_timerfd_gettime:
    case ARM_NR_timerfd_gettime64: {
#if defined(__linux__)
        if (nr == ARM_NR_epoll_create) {
            ctx->regs[0] = host_ret(epoll_create((int)a0));
        } else if (nr == ARM_NR_epoll_create1) {
            ctx->regs[0] = host_ret(epoll_create1((int)a0));
        } else if (nr == ARM_NR_epoll_ctl) {
            FAIL_EFAULT_IF(a3 != 0U && !a32_guest_range_ok(ctx, a3, (uint32_t)sizeof(struct epoll_event), PROT_READ));
            ctx->regs[0] = host_ret(epoll_ctl((int)a0, (int)a1, (int)a2,
                                              a3 ? (struct epoll_event *)a32_guest_ptr(ctx, a3) : NULL));
        } else if (nr == ARM_NR_epoll_wait) {
            FAIL_EINVAL_IF((int32_t)a2 <= 0 || a2 > (uint32_t)(INT_MAX / sizeof(struct epoll_event)));
            FAIL_EFAULT_IF(!epoll_out_ok(ctx, a1, a2));
            int nev = epoll_wait((int)a0, (struct epoll_event *)a32_guest_ptr(ctx, a1),
                                 (int)a2, (int)a3);
            if (nev > 0) {
                a32_vm32_copyout_invalidate_exec(ctx, a1,
                    (uint32_t)nev * (uint32_t)sizeof(struct epoll_event));
            }
            ctx->regs[0] = host_ret(nev);
        } else if (nr == ARM_NR_epoll_pwait) {
            FAIL_EINVAL_IF((int32_t)a2 <= 0 || a2 > (uint32_t)(INT_MAX / sizeof(struct epoll_event)));
            FAIL_EFAULT_IF(!epoll_out_ok(ctx, a1, a2));
            bool     has_temp_mask  = false;
            uint64_t saved_sig_mask = ctx->sig_mask;
            uint64_t temp_mask      = 0ULL;
            int sm_rc = pwait_sigmask_enter(ctx, a4, a5, &has_temp_mask, &saved_sig_mask, &temp_mask);
            if (sm_rc != 0) {
                if (sm_rc < 0) {
                    ctx->regs[0] = (uint32_t)sm_rc;
                }
                break;
            }
            long r = syscall(SYS_epoll_pwait, (int)a0,
                             (struct epoll_event *)a32_guest_ptr(ctx, a1), (int)a2, (int)a3,
                             has_temp_mask ? &temp_mask : NULL, (size_t)8);
            if (r > 0) {
                a32_vm32_copyout_invalidate_exec(ctx, a1,
                    (uint32_t)r * (uint32_t)sizeof(struct epoll_event));
            }
            ctx->regs[0] = host_ret(r);
            pwait_sigmask_leave(ctx, has_temp_mask, saved_sig_mask, temp_mask);
        } else if (nr == ARM_NR_eventfd || nr == ARM_NR_eventfd2) {
            ctx->regs[0] = host_ret(eventfd((unsigned int)a0, (nr == ARM_NR_eventfd2) ? (int)a1 : 0));
        } else if (nr == ARM_NR_timerfd_create) {
            ctx->regs[0] = host_ret(timerfd_create((int)a0, (int)a1));
        } else if (nr == ARM_NR_timerfd_settime || nr == ARM_NR_timerfd_settime64) {
            bool time64 = (nr == ARM_NR_timerfd_settime64);
            uint32_t it_sz = time64 ? 32U : 16U;
            struct itimerspec h_in, h_out = { 0 };
            FAIL_EFAULT_IF(!read_guest_itimerspec(ctx, a2, time64, &h_in) ||
                           (a3 != 0U && !a32_guest_range_ok(ctx, a3, it_sz, PROT_WRITE)));
            int r = timerfd_settime((int)a0, (int)a1, &h_in, (a3 != 0U) ? &h_out : NULL);
            if (r == 0 && a3 != 0U) {
                (void)write_guest_itimerspec(ctx, a3, time64, &h_out);
            }
            ctx->regs[0] = host_ret(r);
        } else {
            bool time64 = (nr == ARM_NR_timerfd_gettime64);
            uint32_t it_sz = time64 ? 32U : 16U;
            FAIL_EFAULT_IF(!a32_guest_range_ok(ctx, a1, it_sz, PROT_WRITE));
            struct itimerspec h_out = { 0 };
            int r = timerfd_gettime((int)a0, &h_out);
            if (r == 0) {
                (void)write_guest_itimerspec(ctx, a1, time64, &h_out);
            }
            ctx->regs[0] = host_ret(r);
        }
#else
        FAIL_ERR(ENOSYS);
#endif
        break;
    }

    /* Filesystem & descriptor management */
    case ARM_NR_dup:
        ctx->regs[0] = host_ret(dup((int)a0));
        break;

    case ARM_NR_dup2:
        ctx->regs[0] = host_ret(dup2((int)a0, (int)a1));
        break;

    case ARM_NR_dup3:
#if defined(__linux__)
        ctx->regs[0] = host_ret(dup3((int)a0, (int)a1, (int)a2));
#else
        if (a0 == a1) {
            FAIL_ERR(EINVAL);
        }
        ctx->regs[0] = host_ret(dup2((int)a0, (int)a1));
#endif
        break;

    case ARM_NR_pipe:
    case ARM_NR_pipe2: {
        FAIL_EFAULT_IF(!a32_guest_range_ok(ctx, a0, 8U, PROT_WRITE));
        int fds[2];
#if defined(__linux__)
        int r = (nr == ARM_NR_pipe2) ? pipe2(fds, (int)a1) : pipe(fds);
#else
        int r = pipe(fds);
#endif
        if (r < 0) {
            FAIL_ERR(errno);
        }
        memcpy(a32_guest_ptr(ctx, a0), fds, sizeof(fds));
        ctx->regs[0] = 0U;
        break;
    }

    case ARM_NR_chdir: {
        char resolved[PATH_MAX];
        RESOLVE_AT(AT_FDCWD, a0, resolved, true);
        ctx->regs[0] = host_ret(chdir(resolved));
        break;
    }

    case ARM_NR_fchdir:
        ctx->regs[0] = host_ret(fchdir((int)a0));
        break;

    case ARM_NR_mkdir:
    case ARM_NR_mkdirat: {
        char resolved[PATH_MAX];
        int dirfd       = (nr == ARM_NR_mkdirat) ? translate_at_fd(a0) : AT_FDCWD;
        uint32_t p_va   = (nr == ARM_NR_mkdirat) ? a1 : a0;
        mode_t mode     = (nr == ARM_NR_mkdirat) ? (mode_t)a2 : (mode_t)a1;
        RESOLVE_AT(dirfd, p_va, resolved, false);
        ctx->regs[0] = host_ret(mkdirat(dirfd, resolved, mode));
        break;
    }

    case ARM_NR_rmdir:
    case ARM_NR_unlink:
    case ARM_NR_unlinkat: {
        char resolved[PATH_MAX];
        int dirfd     = (nr == ARM_NR_unlinkat) ? translate_at_fd(a0) : AT_FDCWD;
        uint32_t p_va = (nr == ARM_NR_unlinkat) ? a1 : a0;
        int u_flags   = (nr == ARM_NR_unlinkat) ? ((a2 & 0x200U) ? AT_REMOVEDIR : 0)
                      : (nr == ARM_NR_rmdir)    ? AT_REMOVEDIR : 0;
        RESOLVE_AT(dirfd, p_va, resolved, false);
        ctx->regs[0] = host_ret(unlinkat(dirfd, resolved, u_flags));
        break;
    }

    case ARM_NR_rename:
    case ARM_NR_renameat:
    case ARM_NR_renameat2: {
        char r_old[PATH_MAX], r_new[PATH_MAX];
        bool is_at      = (nr != ARM_NR_rename);
        int oldfd       = is_at ? translate_at_fd(a0) : AT_FDCWD;
        uint32_t old_va = is_at ? a1 : a0;
        int newfd       = is_at ? translate_at_fd(a2) : AT_FDCWD;
        uint32_t new_va = is_at ? a3 : a1;
        unsigned int fl = (nr == ARM_NR_renameat2) ? (unsigned int)a4 : 0U;
        RESOLVE_AT(oldfd, old_va, r_old, false);
        RESOLVE_AT(newfd, new_va, r_new, false);
#if defined(__linux__)
        ctx->regs[0] = host_ret(syscall(SYS_renameat2, oldfd, r_old, newfd, r_new, fl));
#else
        (void)fl;
        ctx->regs[0] = host_ret(renameat(oldfd, r_old, newfd, r_new));
#endif
        break;
    }

    case ARM_NR_link:
    case ARM_NR_linkat: {
        char r_old[PATH_MAX], r_new[PATH_MAX];
        bool is_at      = (nr == ARM_NR_linkat);
        int oldfd       = is_at ? translate_at_fd(a0) : AT_FDCWD;
        uint32_t old_va = is_at ? a1 : a0;
        int newfd       = is_at ? translate_at_fd(a2) : AT_FDCWD;
        uint32_t new_va = is_at ? a3 : a1;
        int l_flags     = (is_at && (a4 & 0x400U)) ? AT_SYMLINK_FOLLOW : 0;
        bool follow_old = ((l_flags & AT_SYMLINK_FOLLOW) != 0);
        RESOLVE_AT(oldfd, old_va, r_old, follow_old);
        RESOLVE_AT(newfd, new_va, r_new, false);
        ctx->regs[0] = host_ret(linkat(oldfd, r_old, newfd, r_new, l_flags));
        break;
    }

    case ARM_NR_symlink:
    case ARM_NR_symlinkat: {
        char target[PATH_MAX];
        char r_link[PATH_MAX];
        int newfd       = (nr == ARM_NR_symlinkat) ? translate_at_fd(a1) : AT_FDCWD;
        uint32_t lnk_va = (nr == ARM_NR_symlinkat) ? a2 : a1;
        int trc = read_guest_cstr(ctx, a0, target, sizeof(target));
        if (trc != 0) {
            ctx->regs[0] = (uint32_t)trc;
            break;
        }
        RESOLVE_AT(newfd, lnk_va, r_link, false);
        ctx->regs[0] = host_ret(symlinkat(target, newfd, r_link));
        break;
    }

    case ARM_NR_readlink:
    case ARM_NR_readlinkat: {
        bool is_at       = (nr == ARM_NR_readlinkat);
        int dirfd        = is_at ? translate_at_fd(a0) : AT_FDCWD;
        uint32_t path_va = is_at ? a1 : a0;
        uint32_t buf_va  = is_at ? a2 : a1;
        uint32_t bufsiz  = is_at ? a3 : a2;
        FAIL_EINVAL_IF((int32_t)bufsiz <= 0);
        FAIL_EFAULT_IF(!a32_guest_range_ok(ctx, path_va, 1U, PROT_READ) ||
                       !a32_guest_range_ok(ctx, buf_va, 1U, PROT_WRITE));
        char resolved[PATH_MAX];
        RESOLVE_AT(dirfd, path_va, resolved, false);
        const char *guest_path = (const char *)a32_guest_ptr(ctx, path_va);
        if (is_proc_self_exe(guest_path, resolved)) {
            const char *ep = get_guest_exe_path(ctx);
            size_t elen = strlen(ep);
            size_t copylen = (elen < (size_t)bufsiz) ? elen : (size_t)bufsiz;
            FAIL_EFAULT_IF(!guest_write(ctx, buf_va, ep, (uint32_t)copylen));
            ctx->regs[0] = (uint32_t)copylen;
            break;
        }
        /* A32-R13-3: synthesized entries are handled above; anything else
         * under host /proc or /sys is refused here. */
        if (a32_sys_path_denied(ctx, resolved)) FAIL_ERR(ENOENT);
        ctx->regs[0] = host_ret(readlinkat(dirfd, resolved,
                                           (char *)a32_guest_ptr(ctx, buf_va), (size_t)bufsiz));
        break;
    }

    case ARM_NR_chmod:
    case ARM_NR_fchmodat: {
        char resolved[PATH_MAX];
        int dirfd     = (nr == ARM_NR_fchmodat) ? translate_at_fd(a0) : AT_FDCWD;
        uint32_t p_va = (nr == ARM_NR_fchmodat) ? a1 : a0;
        mode_t mode   = (nr == ARM_NR_fchmodat) ? (mode_t)a2 : (mode_t)a1;
        RESOLVE_AT(dirfd, p_va, resolved, true);
        ctx->regs[0] = host_ret(fchmodat(dirfd, resolved, mode, 0));
        break;
    }

    case ARM_NR_fchmod:
        ctx->regs[0] = host_ret(fchmod((int)a0, (mode_t)a1));
        break;

    case ARM_NR_chown:
    case ARM_NR_lchown:
    case ARM_NR_chown32:
    case ARM_NR_lchown32:
    case ARM_NR_fchownat: {
        char resolved[PATH_MAX];
        bool is_at  = (nr == ARM_NR_fchownat);
        bool is_16  = (nr == ARM_NR_chown || nr == ARM_NR_lchown);
        int dirfd   = is_at ? translate_at_fd(a0) : AT_FDCWD;
        uint32_t va = is_at ? a1 : a0;
        uid_t uid   = is_at ? (uid_t)a2 : (uid_t)(is_16 ? id16_to_32(a1) : a1);
        gid_t gid   = is_at ? (gid_t)a3 : (gid_t)(is_16 ? id16_to_32(a2) : a2);
        int c_flags = (is_at ? ((a4 & 0x100U) != 0U)
                             : (nr == ARM_NR_lchown || nr == ARM_NR_lchown32))
                      ? AT_SYMLINK_NOFOLLOW : 0;
        bool follow = ((c_flags & AT_SYMLINK_NOFOLLOW) == 0);
        RESOLVE_AT(dirfd, va, resolved, follow);
        ctx->regs[0] = host_ret(fchownat(dirfd, resolved, uid, gid, c_flags));
        break;
    }

    case ARM_NR_fchown:
    case ARM_NR_fchown32: {
        bool is_16 = (nr == ARM_NR_fchown);
        uid_t uid  = (uid_t)(is_16 ? id16_to_32(a1) : a1);
        gid_t gid  = (gid_t)(is_16 ? id16_to_32(a2) : a2);
        ctx->regs[0] = host_ret(fchown((int)a0, uid, gid));
        break;
    }

    case ARM_NR_umask:
        ctx->regs[0] = (uint32_t)umask((mode_t)a0);
        break;

    case ARM_NR_fsync:
        ctx->regs[0] = host_ret(fsync((int)a0));
        break;

    case ARM_NR_fdatasync:
#if defined(__linux__)
        ctx->regs[0] = host_ret(fdatasync((int)a0));
#else
        ctx->regs[0] = host_ret(fsync((int)a0));
#endif
        break;

    case ARM_NR_utimensat:
    case ARM_NR_utimensat_time64: {
        bool time64 = (nr == ARM_NR_utimensat_time64);
        char resolved[PATH_MAX];
        const char *path = NULL;
        int dirfd = translate_at_fd(a0);
        if (a1 != 0U) {
            bool follow = ((a3 & 0x100U) == 0U);
            RESOLVE_AT(dirfd, a1, resolved, follow);
            path = resolved;
        }
        struct timespec host_ts[2];
        struct timespec *ts_ptr = NULL;
        if (a2 != 0U) {
            uint32_t ts_sz = time64 ? 16U : 8U;
            FAIL_EFAULT_IF(!read_guest_timespec(ctx, a2, time64, &host_ts[0]) ||
                           !read_guest_timespec(ctx, a2 + ts_sz, time64, &host_ts[1]));
            ts_ptr = host_ts;
        }
#if defined(__linux__)
        ctx->regs[0] = host_ret(syscall(SYS_utimensat, dirfd, path, ts_ptr, (int)a3));
#else
        ctx->regs[0] = host_ret((path == NULL) ? futimens(dirfd, ts_ptr)
                                               : utimensat(dirfd, path, ts_ptr, (a3 & 0x100U) ? AT_SYMLINK_NOFOLLOW : 0));
#endif
        break;
    }

    case ARM_NR_nanosleep:
    case ARM_NR_clock_nanosleep:
    case ARM_NR_clock_nanosleep64: {
        bool is_clock = (nr != ARM_NR_nanosleep);
        bool time64   = (nr == ARM_NR_clock_nanosleep64);
        clockid_t clk = is_clock ? (clockid_t)a0 : CLOCK_REALTIME;
        int flags     = is_clock ? (int)a1 : 0;
        uint32_t req_va = is_clock ? a2 : a0;
        uint32_t rem_va = is_clock ? a3 : a1;
        bool abstime  = ((flags & 1) != 0);
        uint32_t ts_sz = time64 ? 16U : 8U;
        struct timespec host_req, host_rem = { 0, 0 };
        FAIL_EFAULT_IF(!read_guest_timespec(ctx, req_va, time64, &host_req) ||
                       (!abstime && rem_va != 0U && !a32_guest_range_ok(ctx, rem_va, ts_sz, PROT_WRITE)));
        int err;
#if defined(__linux__)
        if (is_clock) {
            err = clock_nanosleep(clk, flags, &host_req,
                                  (!abstime && rem_va != 0U) ? &host_rem : NULL);
        } else {
            err = (nanosleep(&host_req, (rem_va != 0U) ? &host_rem : NULL) < 0) ? errno : 0;
        }
#else
        (void)clk;
        err = (nanosleep(&host_req, (!abstime && rem_va != 0U) ? &host_rem : NULL) < 0) ? errno : 0;
#endif
        if (err != 0) {
            ctx->regs[0] = (uint32_t)(-err);
            if (err == EINTR && !abstime && rem_va != 0U) {
                (void)write_guest_timespec(ctx, rem_va, time64, &host_rem);
            }
        } else {
            ctx->regs[0] = 0U;
        }
        break;
    }

    case ARM_NR_sched_yield:
        ctx->regs[0] = host_ret(sched_yield());
        break;

    case ARM_NR_prctl: {
        static _Thread_local char tls_comm[16] = "a32rt";
        if (a0 == 15U /* PR_SET_NAME */) {
            char kbuf[16];
            size_t n = 0U;
            bool ok = false;
            for (; n < 15U; n++) {
                if (!guest_read(ctx, &kbuf[n], a1 + (uint32_t)n, 1U)) {
                    break;
                }
                if (kbuf[n] == '\0') {
                    ok = true;
                    break;
                }
            }
            if (!ok && n == 15U) {
                kbuf[15] = '\0';
                n = 16U;
                ok = true;
            }
            FAIL_EFAULT_IF(!ok);
            memcpy(tls_comm, kbuf, n);
            memset(tls_comm + n, 0, 16U - n);
#if defined(__linux__)
            ctx->regs[0] = host_ret(prctl(PR_SET_NAME, (unsigned long)(uintptr_t)tls_comm,
                                          (unsigned long)a2, (unsigned long)a3,
                                          (unsigned long)a4));
#else
            ctx->regs[0] = 0U;
#endif
            break;
        }
        if (a0 == 16U /* PR_GET_NAME */) {
            FAIL_EFAULT_IF(!guest_write(ctx, a1, tls_comm, 16U));
#if defined(__linux__)
            ctx->regs[0] = host_ret(prctl(PR_GET_NAME,
                                          (unsigned long)(uintptr_t)a32_guest_ptr(ctx, a1),
                                          (unsigned long)a2, (unsigned long)a3,
                                          (unsigned long)a4));
#else
            ctx->regs[0] = 0U;
#endif
            break;
        }
        if (a0 == 40U /* PR_GET_TID_ADDRESS */) {
            FAIL_EINVAL_IF(a2 != 0U || a3 != 0U || a4 != 0U);
            FAIL_EFAULT_IF(!guest_write(ctx, a1, &ctx->clear_child_tid, 4U));
            ctx->regs[0] = 0U;
            break;
        }
#if defined(__linux__)
        unsigned long arg2 = (unsigned long)a1;
        if (a0 == PR_GET_PDEATHSIG) {
            FAIL_EFAULT_IF(!a32_guest_range_ok(ctx, a1, 4U, PROT_WRITE));
            arg2 = (unsigned long)(uintptr_t)a32_guest_ptr(ctx, a1);
        }
        ctx->regs[0] = host_ret(prctl((int)a0, arg2, (unsigned long)a2,
                                      (unsigned long)a3, (unsigned long)a4));
#else
        ctx->regs[0] = 0U;
#endif
        break;
    }

    case ARM_NR_madvise: {
        uint64_t aligned_len = ((uint64_t)a1 + 0xFFFULL) & ~0xFFFULL;
        FAIL_EINVAL_IF((a0 & 0xFFFU) != 0U || a0 < 0x1000U ||
                       (uint64_t)a0 + (uint64_t)a1 > 0xFFFF0000ULL ||
                       (uint64_t)a0 + aligned_len > 0xFFFF0000ULL);
#if defined(__linux__)
        if (a1 > 0U) {
            /* Validate the VMA range first, then issue one madvise (A32-R9-1). */
            a32_vm_sync_ctx(ctx);
            if (!guest_range_mapped(ctx, a0, (uint32_t)aligned_len, PROT_NONE)) {
                FAIL_ERR(ENOMEM);
            }
            if (a2 == MADV_DONTNEED) {
                a32_jit_invalidate_range(ctx, a0, a0 + (uint32_t)aligned_len);
            }
        }
        ctx->regs[0] = host_ret(madvise(a32_guest_ptr(ctx, a0), (size_t)a1, (int)a2));
#else
        ctx->regs[0] = 0U;
#endif
        break;
    }

    case ARM_NR_kill:
#if defined(__APPLE__)
        if ((int)a1 >= 1 && (int)a1 <= 64) {
            siginfo_t ksi;
            memset(&ksi, 0, sizeof(ksi));
            ksi.si_signo = (int)a1;
            ksi.si_code  = 0;
            ksi.si_pid   = getpid();
            ksi.si_uid   = getuid();
            a32_signals_preset_siginfo((int)a1, &ksi);
        }
#endif
        ctx->regs[0] = host_ret(kill((pid_t)a0, (int)a1));
        break;

    case ARM_NR_tkill:
#if defined(__linux__)
        ctx->regs[0] = host_ret(syscall(SYS_tkill, (pid_t)a0, (int)a1));
#else
        if ((int)a1 >= 1 && (int)a1 <= 64) {
            siginfo_t ksi;
            memset(&ksi, 0, sizeof(ksi));
            ksi.si_signo = (int)a1;
            ksi.si_code  = -6 /* SI_TKILL */;
            ksi.si_pid   = getpid();
            ksi.si_uid   = getuid();
            a32_signals_preset_siginfo((int)a1, &ksi);
        }
        ctx->regs[0] = host_ret(kill(getpid(), (int)a1));
#endif
        break;

    case ARM_NR_tgkill:
#if defined(__linux__)
        ctx->regs[0] = host_ret(syscall(SYS_tgkill, (pid_t)a0, (pid_t)a1, (int)a2));
#else
        if ((int)a2 >= 1 && (int)a2 <= 64) {
            siginfo_t ksi;
            memset(&ksi, 0, sizeof(ksi));
            ksi.si_signo = (int)a2;
            ksi.si_code  = -6 /* SI_TKILL */;
            ksi.si_pid   = getpid();
            ksi.si_uid   = getuid();
            a32_signals_preset_siginfo((int)a2, &ksi);
        }
        ctx->regs[0] = host_ret(kill((pid_t)a0, (int)a2));
#endif
        break;

    case ARM_NR_setpgid:
        ctx->regs[0] = host_ret(setpgid((pid_t)a0, (pid_t)a1));
        break;

    case ARM_NR_getpgid:
        ctx->regs[0] = host_ret(getpgid((pid_t)a0));
        break;

    case ARM_NR_setsid:
        ctx->regs[0] = host_ret(setsid());
        break;

    case ARM_NR_getsid:
        ctx->regs[0] = host_ret(getsid((pid_t)a0));
        break;

    case ARM_NR_arm_fadvise64_64: {
#if defined(__linux__)
        int64_t offset = (int64_t)u64_from_u32(a2, a3);
        int64_t len    = (int64_t)u64_from_u32(a4, a5);
        long r = syscall(SYS_fadvise64, (int)a0, offset, len, (int)a1);
        ctx->regs[0] = (r < 0 && errno == ENOSYS) ? 0U : host_ret(r);
#else
        ctx->regs[0] = 0U;
#endif
        break;
    }

    case ARM_NR_membarrier: {
#if defined(__linux__)
        long r = syscall(SYS_membarrier, (int)a0, (unsigned int)a1, (int)a2);
        if (r < 0 && errno == ENOSYS && a0 == 0U /* MEMBARRIER_CMD_QUERY */) {
            ctx->regs[0] = 0U;
        } else {
            ctx->regs[0] = host_ret(r);
        }
#else
        ctx->regs[0] = 0U;
#endif
        break;
    }

    case ARM_NR_flock:
        ctx->regs[0] = host_ret(flock((int)a0, (int)a1));
        break;

    case ARM_NR_inotify_add_watch: {
#if defined(__linux__)
        char resolved[PATH_MAX];
        RESOLVE_AT(AT_FDCWD, a1, resolved, true);
        ctx->regs[0] = host_ret(syscall(SYS_inotify_add_watch, (int)a0, resolved, a2));
#else
        FAIL_ERR(ENOSYS);
#endif
        break;
    }

    case ARM_NR_inotify_rm_watch:
#if defined(__linux__)
        ctx->regs[0] = host_ret(syscall(SYS_inotify_rm_watch, (int)a0, (int)a1));
#else
        FAIL_ERR(ENOSYS);
#endif
        break;

    case ARM_NR_mlock:
        FAIL_EFAULT_IF(a1 > 0U && !a32_guest_range_ok(ctx, a0, a1, PROT_NONE));
        ctx->regs[0] = host_ret(mlock(a32_guest_ptr(ctx, a0), (size_t)a1));
        break;

    case ARM_NR_munlock:
        FAIL_EFAULT_IF(a1 > 0U && !a32_guest_range_ok(ctx, a0, a1, PROT_NONE));
        ctx->regs[0] = host_ret(munlock(a32_guest_ptr(ctx, a0), (size_t)a1));
        break;

    case ARM_NR_mlock2:
        FAIL_EFAULT_IF(a1 > 0U && !a32_guest_range_ok(ctx, a0, a1, PROT_NONE));
#if defined(__linux__)
        ctx->regs[0] = host_ret(syscall(SYS_mlock2, a32_guest_ptr(ctx, a0), (size_t)a1, (unsigned int)a2));
#else
        FAIL_ERR(ENOSYS);
#endif
        break;

    case ARM_NR_getpriority:
#if defined(__linux__)
        ctx->regs[0] = host_ret(syscall(SYS_getpriority, (int)a0, (id_t)a1));
#else
        {
            errno = 0;
            int p = getpriority((int)a0, (id_t)a1);
            if (p == -1 && errno != 0) FAIL_ERR(errno);
            ctx->regs[0] = (uint32_t)(20 - p);
        }
#endif
        break;

    case ARM_NR_setpriority:
        ctx->regs[0] = host_ret(setpriority((int)a0, (id_t)a1, (int)a2));
        break;

    case ARM_NR_personality:
        ctx->regs[0] = (a0 == 0xFFFFFFFFU || a0 == 0U) ? 0U : (uint32_t)(-EINVAL);
        break;

    case ARM_NR_times:
    case ARM_NR_setxattr:
    case ARM_NR_lsetxattr:
    case ARM_NR_fsetxattr:
    case ARM_NR_getxattr:
    case ARM_NR_lgetxattr:
    case ARM_NR_fgetxattr:
    case ARM_NR_listxattr:
    case ARM_NR_llistxattr:
    case ARM_NR_flistxattr:
    case ARM_NR_removexattr:
    case ARM_NR_lremovexattr:
    case ARM_NR_fremovexattr: {
#if defined(__linux__)
        if (nr == ARM_NR_times) {
            if (a0 != 0U) {
                FAIL_EFAULT_IF(!a32_guest_range_ok(ctx, a0, 16U, PROT_WRITE));
                int64_t h_tms[4] = { 0, 0, 0, 0 };
                long r = syscall(SYS_times, h_tms);
                if (r < 0) {
                    FAIL_ERR(errno);
                }
                uint32_t g_tms[4] = {
                    (uint32_t)h_tms[0], (uint32_t)h_tms[1],
                    (uint32_t)h_tms[2], (uint32_t)h_tms[3],
                };
                (void)guest_write(ctx, a0, g_tms, sizeof(g_tms));
                ctx->regs[0] = (uint32_t)r;
            } else {
                ctx->regs[0] = host_ret(syscall(SYS_times, NULL));
            }
        } else if (nr == ARM_NR_setxattr || nr == ARM_NR_lsetxattr || nr == ARM_NR_fsetxattr) {
            FAIL_EFAULT_IF(!a32_guest_range_ok(ctx, a1, 1U, PROT_READ) ||
                           (a3 > 0U && !a32_guest_range_ok(ctx, a2, a3, PROT_READ)));
            const char *name = (const char *)a32_guest_ptr(ctx, a1);
            const void *val  = (a2 != 0U) ? a32_guest_ptr(ctx, a2) : NULL;
            if (nr == ARM_NR_fsetxattr) {
                ctx->regs[0] = host_ret(syscall(SYS_fsetxattr, (int)a0, name, val, (size_t)a3, (int)a4));
            } else {
                char resolved[PATH_MAX];
                RESOLVE_AT(AT_FDCWD, a0, resolved, nr == ARM_NR_setxattr);
                long sysno = (nr == ARM_NR_setxattr) ? SYS_setxattr : SYS_lsetxattr;
                ctx->regs[0] = host_ret(syscall(sysno, resolved, name, val, (size_t)a3, (int)a4));
            }
        } else if (nr == ARM_NR_getxattr || nr == ARM_NR_lgetxattr || nr == ARM_NR_fgetxattr) {
            FAIL_EFAULT_IF(!a32_guest_range_ok(ctx, a1, 1U, PROT_READ) ||
                           (a3 > 0U && !a32_guest_range_ok(ctx, a2, a3, PROT_WRITE)));
            const char *name = (const char *)a32_guest_ptr(ctx, a1);
            void       *val  = (a2 != 0U) ? a32_guest_ptr(ctx, a2) : NULL;
            if (nr == ARM_NR_fgetxattr) {
                ctx->regs[0] = host_ret(syscall(SYS_fgetxattr, (int)a0, name, val, (size_t)a3));
            } else {
                char resolved[PATH_MAX];
                RESOLVE_AT(AT_FDCWD, a0, resolved, nr == ARM_NR_getxattr);
                long sysno = (nr == ARM_NR_getxattr) ? SYS_getxattr : SYS_lgetxattr;
                ctx->regs[0] = host_ret(syscall(sysno, resolved, name, val, (size_t)a3));
            }
        } else if (nr == ARM_NR_listxattr || nr == ARM_NR_llistxattr || nr == ARM_NR_flistxattr) {
            FAIL_EFAULT_IF(a2 > 0U && !a32_guest_range_ok(ctx, a1, a2, PROT_WRITE));
            void *list = (a1 != 0U) ? a32_guest_ptr(ctx, a1) : NULL;
            if (nr == ARM_NR_flistxattr) {
                ctx->regs[0] = host_ret(syscall(SYS_flistxattr, (int)a0, list, (size_t)a2));
            } else {
                char resolved[PATH_MAX];
                RESOLVE_AT(AT_FDCWD, a0, resolved, nr == ARM_NR_listxattr);
                long sysno = (nr == ARM_NR_listxattr) ? SYS_listxattr : SYS_llistxattr;
                ctx->regs[0] = host_ret(syscall(sysno, resolved, list, (size_t)a2));
            }
        } else {
            FAIL_EFAULT_IF(!a32_guest_range_ok(ctx, a1, 1U, PROT_READ));
            const char *name = (const char *)a32_guest_ptr(ctx, a1);
            if (nr == ARM_NR_fremovexattr) {
                ctx->regs[0] = host_ret(syscall(SYS_fremovexattr, (int)a0, name));
            } else {
                char resolved[PATH_MAX];
                RESOLVE_AT(AT_FDCWD, a0, resolved, nr == ARM_NR_removexattr);
                long sysno = (nr == ARM_NR_removexattr) ? SYS_removexattr : SYS_lremovexattr;
                ctx->regs[0] = host_ret(syscall(sysno, resolved, name));
            }
        }
#else
        FAIL_ERR(ENOSYS);
#endif
        break;
    }

    /* ARM private syscalls */
    case ARM_NR_set_tls:
        ctx->tls_tp  = a0;
        ctx->regs[0] = 0U;
        break;

    case ARM_NR_get_tls:
        ctx->regs[0] = ctx->tls_tp;
        break;

    case ARM_NR_cacheflush:
        a32_jit_invalidate_range(ctx, a0, a1);
        ctx->regs[0] = 0U;
        break;

    case ARM_NR_breakpoint:
        if (ctx->gdb_enabled) {
            a32_gdb_handle_stop(ctx, SIGTRAP);
        } else {
            fprintf(stderr, "a32rt: ARM breakpoint syscall executed at pc=0x%08x\n", ctx->regs[15]);
            exit(133);
        }
        break;

    default:
        fprintf(stderr, "a32rt: unimplemented ARM EABI syscall %u (0x%x) at pc=0x%08x\n",
                nr, nr, ctx->regs[15]);
        ctx->regs[0] = (uint32_t)(-ENOSYS);
        break;
    }

    return 0;
}
