/*
 * src/os/os_sys_sock.c - split-TU part 6/7 of src/syscalls.c (lines 3179-3697).
 * Compiled only via the src/syscalls.c wrapper; do not add to Makefile SRCS.
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

    case ARM_NR_pread64:
    case ARM_NR_pwrite64: {
        FAIL_EFAULT_IF(a2 > 0U && a1 < 0x1000U);
        off_t pos = (off_t)u64_from_u32(a4, a5);
        ssize_t r = (nr == ARM_NR_pread64)
                    ? pread((int)a0, a32_guest_ptr(ctx, a1), (size_t)a2, pos)
                    : pwrite((int)a0, a32_guest_ptr(ctx, a1), (size_t)a2, pos);
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
        ctx->regs[0] = host_ret(truncate(resolved, len));
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
        ctx->regs[0] = host_ret(ftruncate((int)a0, len));
        break;
    }

    case ARM_NR_sched_setaffinity:
        FAIL_EFAULT_IF(a2 < 0x1000U);
        ctx->regs[0] = host_ret(syscall(SYS_sched_setaffinity, (pid_t)a0,
                                        (unsigned int)a1, a32_guest_ptr(ctx, a2)));
        break;

    case ARM_NR_sched_getaffinity: {
        FAIL_EFAULT_IF(a2 < 0x1000U);
        FAIL_EINVAL_IF((a1 & 3U) != 0U || a1 == 0U);
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
        break;
    }

    case ARM_NR_sendfile64:
        FAIL_EFAULT_IF(a2 != 0U && a2 < 0x1000U);
        ctx->regs[0] = host_ret(sendfile((int)a0, (int)a1,
                                         a2 ? (off_t *)a32_guest_ptr(ctx, a2) : NULL,
                                         (size_t)a3));
        break;

    case ARM_NR_statx: {
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
        break;
    }

    case ARM_NR_unshare:
        ctx->regs[0] = (a0 == 0U) ? 0U : host_ret(syscall(SYS_unshare, (int)a0));
        break;

    case ARM_NR_setns:
        ctx->regs[0] = host_ret(syscall(SYS_setns, (int)a0, (int)a1));
        break;

    case ARM_NR_seccomp:
    case ARM_NR_rseq:
        FAIL_ERR(ENOSYS);

    case ARM_NR_close_range:
        FAIL_EINVAL_IF(a0 > a1 || (a2 & ~6U /* CLOSE_RANGE_UNSHARE(2) | CLOSE_RANGE_CLOEXEC(4) */) != 0U);
        ctx->regs[0] = host_ret(syscall(SYS_close_range, (unsigned int)a0, (unsigned int)a1, (unsigned int)a2));
        break;

    case ARM_NR_memfd_create: {
        FAIL_EFAULT_IF(!a32_guest_range_ok(ctx, a0, 1U, PROT_READ));
        FAIL_EINVAL_IF((a1 & ~0xFC00001FU) != 0U || ((a1 & 4U) == 0U && (a1 & ~0x1FU) != 0U));
        const char *uname_str = (const char *)a32_guest_ptr(ctx, a0);
        FAIL_EINVAL_IF(strnlen(uname_str, 250U) >= 250U);
        ctx->regs[0] = host_ret(syscall(SYS_memfd_create, uname_str, (unsigned int)a1));
        break;
    }

    case ARM_NR_inotify_init1:
        ctx->regs[0] = host_ret(syscall(SYS_inotify_init1, (int)a0));
        break;

    case ARM_NR_signalfd4: {
        uint64_t gmask = 0ULL;
        FAIL_EFAULT_IF(a2 != 8U || !guest_read(ctx, &gmask, a1, sizeof(gmask)));
        sigset_t hmask;
        memset(&hmask, 0, sizeof(hmask));
        memcpy(&hmask, &gmask, sizeof(uint64_t));
        ctx->regs[0] = host_ret(syscall(SYS_signalfd4, (int)a0, &hmask, sizeof(hmask), (int)a3));
        break;
    }

    case ARM_NR_splice:
    case ARM_NR_copy_file_range: {
        FAIL_EFAULT_IF((a1 != 0U && !a32_guest_range_ok(ctx, a1, 8U, PROT_READ | PROT_WRITE)) ||
                       (a3 != 0U && !a32_guest_range_ok(ctx, a3, 8U, PROT_READ | PROT_WRITE)));
        loff_t *p_off_in  = a1 ? (loff_t *)a32_guest_ptr(ctx, a1) : NULL;
        loff_t *p_off_out = a3 ? (loff_t *)a32_guest_ptr(ctx, a3) : NULL;
        long h_nr = (nr == ARM_NR_splice) ? SYS_splice : SYS_copy_file_range;
        ctx->regs[0] = host_ret(syscall(h_nr, (int)a0, p_off_in,
                                        (int)a2, p_off_out, (size_t)a4, (unsigned int)a5));
        break;
    }

    case ARM_NR_sync_file_range2: {
        off_t offset = (off_t)u64_from_u32(a2, a3);
        off_t nbytes = (off_t)u64_from_u32(a4, a5);
        ctx->regs[0] = host_ret(syscall(SYS_sync_file_range, (int)a0, offset, nbytes, (unsigned int)a1));
        break;
    }

    case ARM_NR_tee:
        ctx->regs[0] = host_ret(syscall(SYS_tee, (int)a0, (int)a1, (size_t)a2, (unsigned int)a3));
        break;

    case ARM_NR_fallocate: {
        off_t offset = (off_t)u64_from_u32(a2, a3);
        off_t len    = (off_t)u64_from_u32(a4, a5);
        ctx->regs[0] = host_ret(syscall(SYS_fallocate, (int)a0, (int)a1, offset, len));
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
        int r = (nr == ARM_NR_accept4) ? accept4((int)a0, sa, salen, (int)a3)
                                       : accept((int)a0, sa, salen);
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
        const struct sockaddr *sa = (nr == ARM_NR_sendto && a4) ? a32_guest_ptr(ctx, a4) : NULL;
        socklen_t salen           = (nr == ARM_NR_sendto) ? (socklen_t)a5 : 0U;
        ctx->regs[0] = host_ret(sendto((int)a0, a32_guest_ptr(ctx, a1), (size_t)a2, (int)a3,
                                       sa, salen));
        break;
    }

    case ARM_NR_recv:
    case ARM_NR_recvfrom: {
        struct sockaddr *sa = (nr == ARM_NR_recvfrom && a4) ? a32_guest_ptr(ctx, a4) : NULL;
        socklen_t *salen    = (nr == ARM_NR_recvfrom && a5) ? (socklen_t *)a32_guest_ptr(ctx, a5) : NULL;
        ctx->regs[0] = host_ret(recvfrom((int)a0, a32_guest_ptr(ctx, a1), (size_t)a2, (int)a3,
                                         sa, salen));
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
        if (gm->msg_iovlen > 1024U) {
            FAIL_ERR(EMSGSIZE);
        }
        FAIL_EINVAL_IF(nr == ARM_NR_sendmsg && gm->msg_controllen > 0U && gm->msg_controllen < 12U);
        FAIL_EFAULT_IF((gm->msg_name != 0U && gm->msg_name < 0x1000U) ||
                       (gm->msg_controllen != 0U &&
                        !a32_guest_range_ok(ctx, gm->msg_control, gm->msg_controllen,
                                            (nr == ARM_NR_recvmsg) ? PROT_WRITE : PROT_READ)));
        struct msghdr hm = {
            .msg_name    = (gm->msg_name != 0U) ? a32_guest_ptr(ctx, gm->msg_name) : NULL,
            .msg_namelen = (socklen_t)gm->msg_namelen,
            .msg_flags   = (int)gm->msg_flags,
        };

        _Alignas(8) uint8_t cbuf64[1024] = { 0 };
        if (gm->msg_control != 0U && gm->msg_controllen > 0U) {
            if (nr == ARM_NR_sendmsg) {
                const uint8_t *gctl = (const uint8_t *)a32_guest_ptr(ctx, gm->msg_control);
                uint32_t off32 = 0U;
                size_t off64 = 0U;
                while (off32 + 12U <= gm->msg_controllen) {
                    uint32_t len32;
                    int32_t level32, type32;
                    memcpy(&len32, gctl + off32, 4);
                    memcpy(&level32, gctl + off32 + 4U, 4);
                    memcpy(&type32, gctl + off32 + 8U, 4);
                    FAIL_EINVAL_IF(len32 < 12U || len32 > gm->msg_controllen - off32);
                    size_t payload_len = (size_t)(len32 - 12U);
                    size_t space64 = CMSG_SPACE(payload_len);
                    if (off64 + space64 > sizeof(cbuf64)) {
                        break;
                    }
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
                size_t cap64 = (size_t)gm->msg_controllen * 2U;
                hm.msg_control    = cbuf64;
                hm.msg_controllen = (cap64 > sizeof(cbuf64)) ? sizeof(cbuf64) : cap64;
            }
        }

        struct iovec host_iov[16];
        struct iovec *hiov = NULL;
        int iov_rc = translate_guest_iov(ctx, gm->msg_iov, (int)gm->msg_iovlen,
                                         host_iov, 16U, &hiov);
        if (iov_rc < 0) {
            ctx->regs[0] = (uint32_t)iov_rc;
            break;
        }
        hm.msg_iov    = (gm->msg_iovlen > 0U) ? hiov : NULL;
        hm.msg_iovlen = (size_t)gm->msg_iovlen;

        ssize_t r = (nr == ARM_NR_sendmsg) ? sendmsg((int)a0, &hm, (int)a2)
                                           : recvmsg((int)a0, &hm, (int)a2);
        if (r >= 0) {
            gm->msg_namelen = (uint32_t)hm.msg_namelen;
            gm->msg_flags   = (uint32_t)hm.msg_flags;
            if (nr == ARM_NR_recvmsg && gm->msg_control != 0U) {
                uint8_t *gctl_out = (uint8_t *)a32_guest_ptr(ctx, gm->msg_control);
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
                    uint32_t len32 = 12U + (uint32_t)payload_len;
                    uint32_t space32 = (len32 + 3U) & ~3U;
                    if (!ctrunc && off32 + len32 <= gm->msg_controllen) {
                        int32_t level32 = (int32_t)cm64->cmsg_level;
                        int32_t type32  = (int32_t)cm64->cmsg_type;
                        memcpy(gctl_out + off32, &len32, 4);
                        memcpy(gctl_out + off32 + 4U, &level32, 4);
                        memcpy(gctl_out + off32 + 8U, &type32, 4);
                        memcpy(gctl_out + off32 + 12U, CMSG_DATA(cm64), payload_len);
                        off32 += (off32 + space32 <= gm->msg_controllen) ? space32 : len32;
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
                gm->msg_controllen = off32;
            }
        }
        if (hiov != host_iov) {
            free(hiov);
        }
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
        uint32_t chk_bytes = (fd_bytes32 > 0U && fd_bytes32 <= 128U) ? (uint32_t)fd_bytes32 : 4U;
        FAIL_EFAULT_IF((a1 != 0U && !a32_guest_range_ok(ctx, a1, chk_bytes, PROT_READ | PROT_WRITE)) ||
                       (a2 != 0U && !a32_guest_range_ok(ctx, a2, chk_bytes, PROT_READ | PROT_WRITE)) ||
                       (a3 != 0U && !a32_guest_range_ok(ctx, a3, chk_bytes, PROT_READ | PROT_WRITE)));
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
            r_ptr = (a1 != 0U) ? a32_guest_ptr(ctx, a1) : NULL;
            w_ptr = (a2 != 0U) ? a32_guest_ptr(ctx, a2) : NULL;
            e_ptr = (a3 != 0U) ? a32_guest_ptr(ctx, a3) : NULL;
        }

        long r = syscall(SYS_pselect6, nfds, r_ptr, w_ptr, e_ptr, ts_ptr, sig_arg);
        int saved_errno = errno;
        if (r >= 0 && nfds <= 1024) {
            if (a1 != 0U) {
                memcpy(a32_guest_ptr(ctx, a1), rset, fd_bytes32);
            }
            if (a2 != 0U) {
                memcpy(a32_guest_ptr(ctx, a2), wset, fd_bytes32);
            }
            if (a3 != 0U) {
                memcpy(a32_guest_ptr(ctx, a3), eset, fd_bytes32);
            }
        }
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
