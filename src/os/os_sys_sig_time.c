/*
 * src/os/os_sys_sig_time.c - Signal, clone/fork/execve, wait, timer, and futex syscalls
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "os/sys_priv.h"

/* Transport keys for the execve signal-state carry. The guest stack
 * starts clean (see filter_exec_sig_env in src/main.c), but a guest
 * that re-execs with an explicit envp can still pass stale triples;
 * both exec branches below filter them (A32-R4-7). */
static bool is_exec_sig_transport(const char *s)
{
    return strncmp(s, "A32RT_EXEC_SIGMASK=", 19) == 0 ||
           strncmp(s, "A32RT_EXEC_SIGIGN=", 18) == 0 ||
           strncmp(s, "A32RT_EXEC_SIGPEND=", 19) == 0;
}

int a32_sys_dispatch_sig_time(a32_ctx_t *ctx)
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
    case ARM_NR_rt_sigpending: {
        FAIL_EINVAL_IF(a1 != 8U);
        uint64_t pend = __atomic_load_n(&ctx->pending_signals, __ATOMIC_SEQ_CST);
        FAIL_EFAULT_IF(!guest_write(ctx, a0, &pend, sizeof(pend)));
        ctx->regs[0] = 0U;
        break;
    }

    case ARM_NR_rt_sigtimedwait:
    case ARM_NR_rt_sigtimedwait_time64: {
        bool time64 = (nr == ARM_NR_rt_sigtimedwait_time64);
        FAIL_EINVAL_IF(a3 != 8U);
        uint64_t wait_set = 0ULL;
        FAIL_EFAULT_IF(!guest_read(ctx, &wait_set, a0, 8U) ||
                       (a1 != 0U && !a32_guest_range_ok(ctx, a1, 128U, PROT_WRITE)));
        int64_t rem_ns = -1;
        if (a2 != 0U) {
            struct timespec ts;
            FAIL_EFAULT_IF(!read_guest_timespec(ctx, a2, time64, &ts));
            FAIL_EINVAL_IF(ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L);
            int64_t t_sec = (ts.tv_sec > 3600) ? 3600 : (int64_t)ts.tv_sec;
            rem_ns = t_sec * 1000000000LL + (int64_t)ts.tv_nsec;
        }
        wait_set &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
        for (int s = 1; s <= 64; s++) {
            if ((wait_set & (1ULL << (s - 1))) != 0ULL) {
                a32_signals_ensure_host_handler(s);
            }
        }

        int matched_sig = 0;
        for (;;) {
            uint64_t pend = __atomic_load_n(&ctx->pending_signals, __ATOMIC_SEQ_CST);
            uint64_t ready = pend & wait_set;
            if (ready != 0ULL) {
                int s = __builtin_ctzll(ready) + 1;
                uint64_t bit = 1ULL << (s - 1);
                uint64_t prev = __atomic_fetch_and(&ctx->pending_signals, ~bit, __ATOMIC_SEQ_CST);
                if ((prev & bit) != 0ULL) {
                    matched_sig = s;
                    break;
                }
            }
            if ((pend & ~ctx->sig_mask & ~wait_set) != 0ULL) {
                ctx->regs[0] = (uint32_t)(-EINTR);
                break;
            }
            if (rem_ns == 0) {
                ctx->regs[0] = (uint32_t)(-EAGAIN);
                break;
            }
            int64_t slice_ns = 1000000LL;
            if (rem_ns > 0 && rem_ns < slice_ns) {
                slice_ns = rem_ns;
            }
            struct timespec sl_req = { 0, (long)slice_ns };
            struct timespec sl_rem = { 0, 0 };
            int sr = nanosleep(&sl_req, &sl_rem);
            if (sr < 0 && errno == EINTR) {
                if (rem_ns > 0) {
                    int64_t left = (int64_t)sl_rem.tv_sec * 1000000000LL + (int64_t)sl_rem.tv_nsec;
                    int64_t elapsed = slice_ns - left;
                    if (elapsed < 0) {
                        elapsed = 0;
                    }
                    rem_ns = (rem_ns > elapsed) ? (rem_ns - elapsed) : 0;
                }
                continue;
            }
            if (rem_ns > 0) {
                rem_ns = (rem_ns > slice_ns) ? (rem_ns - slice_ns) : 0;
            }
        }
        if (matched_sig != 0) {
            a32_signals_consume_siginfo(matched_sig, (a1 != 0U) ? a32_guest_ptr(ctx, a1) : NULL);
            ctx->regs[0] = (uint32_t)matched_sig;
        }
        break;
    }

    case ARM_NR_rt_sigsuspend: {
        FAIL_EINVAL_IF(a1 != 8U);
        uint64_t saved_sig_mask = ctx->sig_mask;
        uint64_t temp_mask      = 0ULL;
        FAIL_EFAULT_IF(!guest_read(ctx, &temp_mask, a0, sizeof(temp_mask)));
        temp_mask &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
        ctx->regs[0] = (uint32_t)(-EINTR);
        for (;;) {
            check_pwait_signals(ctx, temp_mask);
            if (ctx->exit_reason == A32_EXIT_BRANCH) {
                break;
            }
            struct timespec poll_ts = { 0, 1000000L };
#if defined(__linux__)
            (void)syscall(SYS_ppoll, NULL, 0, &poll_ts, &temp_mask, (size_t)8);
#else
            (void)nanosleep(&poll_ts, NULL);
#endif
        }
        restore_pwait_sigmask(ctx, saved_sig_mask);
        break;
    }

    case ARM_NR_rt_sigqueueinfo:
    case ARM_NR_rt_tgsigqueueinfo: {
        bool is_tg     = (nr == ARM_NR_rt_tgsigqueueinfo);
        int sig        = (int)(is_tg ? a2 : a1);
        uint32_t si_va = is_tg ? a3 : a2;
        uint32_t gsi[6];
        FAIL_EFAULT_IF(!guest_read(ctx, gsi, si_va, sizeof(gsi)));
        siginfo_t si;
        memset(&si, 0, sizeof(si));
        si.si_signo           = (int)gsi[0];
        si.si_errno           = (int)gsi[1];
        si.si_code            = (int)gsi[2];
        si.si_pid             = (pid_t)gsi[3];
        si.si_uid             = (uid_t)gsi[4];
        si.si_value.sival_int = (int)gsi[5];
#if defined(__linux__)
        long rc = is_tg ? syscall(SYS_rt_tgsigqueueinfo, (pid_t)a0, (pid_t)a1, sig, &si)
                        : syscall(SYS_rt_sigqueueinfo, (pid_t)a0, sig, &si);
        if (rc < 0 && errno == ENOSYS) {
            FAIL_EINVAL_IF(sig < 0 || sig > 64 || (is_tg && ((int)a0 <= 0 || (int)a1 <= 0)));
            if (sig != 0 && (si.si_code >= 0 || si.si_code == -6 /* SI_TKILL */) &&
                si.si_pid != getpid()) {
                FAIL_ERR(EPERM);
            }
            a32_signals_preset_siginfo(sig, &si);
            rc = is_tg ? syscall(SYS_tgkill, (pid_t)a0, (pid_t)a1, sig)
                       : syscall(SYS_kill, (pid_t)a0, sig);
        }
#else
        a32_signals_preset_siginfo(sig, &si);
        long rc = kill((pid_t)a0, sig);
#endif
        ctx->regs[0] = host_ret(rc);
        break;
    }

    case ARM_NR_sigaltstack:
        ctx->regs[0] = (uint32_t)a32_sys_sigaltstack(ctx, a0, a1);
        break;

    case ARM_NR_sigreturn:
    case ARM_NR_rt_sigreturn: {
        int rc = a32_sys_rt_sigreturn(ctx);
        if (rc < 0) {
            ctx->regs[0] = (uint32_t)rc;
        }
        break;
    }

    case ARM_NR_getrlimit:
    case ARM_NR_ugetrlimit: {
        FAIL_EFAULT_IF(!a32_guest_range_ok(ctx, a1, 8U, PROT_WRITE));
        struct rlimit rl;
        if (getrlimit((int)a0, &rl) != 0) {
            FAIL_ERR(errno);
        }
        arm_rlimit32_t rl32 = {
            (rl.rlim_cur > 0xFFFFFFFFUL) ? 0xFFFFFFFFU : (uint32_t)rl.rlim_cur,
            (rl.rlim_max > 0xFFFFFFFFUL) ? 0xFFFFFFFFU : (uint32_t)rl.rlim_max
        };
        memcpy(a32_guest_ptr(ctx, a1), &rl32, sizeof(rl32));
        ctx->regs[0] = 0U;
        break;
    }

    case ARM_NR_setrlimit: {
        arm_rlimit32_t rl32;
        FAIL_EFAULT_IF(!guest_read(ctx, &rl32, a1, sizeof(rl32)));
        struct rlimit rl = {
            (rl32.rlim_cur == 0xFFFFFFFFU) ? RLIM_INFINITY : (rlim_t)rl32.rlim_cur,
            (rl32.rlim_max == 0xFFFFFFFFU) ? RLIM_INFINITY : (rlim_t)rl32.rlim_max
        };
        ctx->regs[0] = host_ret(setrlimit((int)a0, &rl));
        break;
    }

    case ARM_NR_prlimit64:
        FAIL_EFAULT_IF((a2 != 0U && a2 < 0x1000U) || (a3 != 0U && a3 < 0x1000U));
#if defined(__linux__)
        ctx->regs[0] = host_ret(syscall(SYS_prlimit64, (pid_t)a0, (int)a1,
                                        a2 ? a32_guest_ptr(ctx, a2) : NULL,
                                        a3 ? a32_guest_ptr(ctx, a3) : NULL));
#else
        FAIL_ERR(ENOSYS);
#endif
        break;

    case ARM_NR_getrusage: {
        FAIL_EFAULT_IF(!a32_guest_range_ok(ctx, a1, sizeof(arm_rusage32_t), PROT_WRITE));
        struct rusage ru;
        if (getrusage((int)a0, &ru) != 0) {
            FAIL_ERR(errno);
        }
        fill_arm_rusage32((arm_rusage32_t *)a32_guest_ptr(ctx, a1), &ru);
        ctx->regs[0] = 0U;
        break;
    }

    case ARM_NR_sysinfo: {
        FAIL_EFAULT_IF(!a32_guest_range_ok(ctx, a0, sizeof(arm_sysinfo32_t), PROT_WRITE));
        arm_sysinfo32_t si32;
        memset(&si32, 0, sizeof(si32));
#if defined(__linux__)
        struct sysinfo si;
        if (sysinfo(&si) != 0) {
            FAIL_ERR(errno);
        }
        uint64_t mem_unit = (si.mem_unit > 0U) ? (uint64_t)si.mem_unit : 1ULL;
        while ((((uint64_t)si.totalram | (uint64_t)si.totalswap) >> 32) != 0ULL &&
               (mem_unit << 1) <= 0xFFFFFFFFULL) {
            mem_unit <<= 1;
            si.totalram >>= 1; si.freeram >>= 1; si.sharedram >>= 1; si.bufferram >>= 1;
            si.totalswap >>= 1; si.freeswap >>= 1; si.totalhigh >>= 1; si.freehigh >>= 1;
        }
        si32.uptime    = (int32_t)si.uptime;
        si32.loads[0]  = (uint32_t)si.loads[0];
        si32.loads[1]  = (uint32_t)si.loads[1];
        si32.loads[2]  = (uint32_t)si.loads[2];
        si32.totalram  = (uint32_t)si.totalram;  si32.freeram   = (uint32_t)si.freeram;
        si32.sharedram = (uint32_t)si.sharedram; si32.bufferram = (uint32_t)si.bufferram;
        si32.totalswap = (uint32_t)si.totalswap; si32.freeswap  = (uint32_t)si.freeswap;
        si32.procs     = (uint16_t)si.procs;
        si32.totalhigh = (uint32_t)si.totalhigh; si32.freehigh  = (uint32_t)si.freehigh;
        si32.mem_unit  = (uint32_t)mem_unit;
#else
        si32.uptime = 1000; si32.totalram = 262144U; si32.freeram = 131072U;
        si32.procs  = 64U;  si32.mem_unit = 4096U;
#endif
        memcpy(a32_guest_ptr(ctx, a0), &si32, sizeof(si32));
        ctx->regs[0] = 0U;
        break;
    }

    case ARM_NR_statfs64:
    case ARM_NR_fstatfs64: {
        FAIL_EINVAL_IF(a1 != sizeof(arm_statfs64_t) && a1 != 88U);
        FAIL_EFAULT_IF(!a32_guest_range_ok(ctx, a2, sizeof(arm_statfs64_t), PROT_WRITE));
        struct statfs sfs;
        int r;
        if (nr == ARM_NR_statfs64) {
            char resolved[PATH_MAX];
            RESOLVE_AT(AT_FDCWD, a0, resolved, true);
            r = statfs(resolved, &sfs);
        } else {
            r = fstatfs((int)a0, &sfs);
        }
        if (r != 0) {
            FAIL_ERR(errno);
        }
        fill_arm_statfs64((arm_statfs64_t *)a32_guest_ptr(ctx, a2), &sfs);
        ctx->regs[0] = 0U;
        break;
    }

    case ARM_NR_getrandom:
        FAIL_EFAULT_IF(a1 > 0U && a0 < 0x1000U);
#if defined(__linux__)
        ctx->regs[0] = host_ret(getrandom(a32_guest_ptr(ctx, a0), (size_t)a1, (unsigned int)a2));
#else
        (void)a2;
        arc4random_buf(a32_guest_ptr(ctx, a0), (size_t)a1);
        ctx->regs[0] = a1;
#endif
        break;

    case ARM_NR_clock_gettime32:
    case ARM_NR_clock_gettime64: {
        bool time64 = (nr == ARM_NR_clock_gettime64);
        FAIL_EFAULT_IF(!a32_guest_range_ok(ctx, a1, time64 ? 16U : 8U, PROT_WRITE));
        struct timespec ts;
        clockid_t clk = CLOCK_REALTIME;
        if (translate_linux_clockid(a0, &clk) != 0 || clock_gettime(clk, &ts) != 0) {
            FAIL_ERR(errno);
        }
        (void)write_guest_timespec(ctx, a1, time64, &ts);
        ctx->regs[0] = 0U;
        break;
    }

    case ARM_NR_clock_getres32:
    case ARM_NR_clock_getres64: {
        bool time64 = (nr == ARM_NR_clock_getres64);
        FAIL_EFAULT_IF(a1 != 0U && !a32_guest_range_ok(ctx, a1, time64 ? 16U : 8U, PROT_WRITE));
        struct timespec ts;
        clockid_t clk = CLOCK_REALTIME;
        if (translate_linux_clockid(a0, &clk) != 0 || clock_getres(clk, &ts) != 0) {
            FAIL_ERR(errno);
        }
        if (a1 != 0U) {
            (void)write_guest_timespec(ctx, a1, time64, &ts);
        }
        ctx->regs[0] = 0U;
        break;
    }

    case ARM_NR_gettimeofday: {
        FAIL_EFAULT_IF((a0 != 0U && !a32_guest_range_ok(ctx, a0, 8U, PROT_WRITE)) ||
                       (a1 != 0U && !a32_guest_range_ok(ctx, a1, 8U, PROT_WRITE)));
        struct timeval tv;
        struct timezone tz = { 0, 0 };
        int r = gettimeofday(&tv, (a1 != 0U) ? &tz : NULL);
        if (r != 0) {
            FAIL_ERR(errno);
        }
        if (a0 != 0U) {
            (void)write_guest_timeval(ctx, a0, false, &tv);
        }
        if (a1 != 0U) {
            int32_t tz32[2] = { (int32_t)tz.tz_minuteswest, (int32_t)tz.tz_dsttime };
            memcpy(a32_guest_ptr(ctx, a1), tz32, sizeof(tz32));
        }
        ctx->regs[0] = 0U;
        break;
    }

    case ARM_NR_getitimer: {
        struct itimerval itv;
        memset(&itv, 0, sizeof(itv));
        if (getitimer((int)a0, &itv) != 0) {
            FAIL_ERR(errno);
        }
        if (itv.it_value.tv_sec == 0 && itv.it_value.tv_usec == 0) {
            itv.it_interval.tv_sec  = 0;
            itv.it_interval.tv_usec = 0;
        }
        FAIL_EFAULT_IF(!write_guest_itimerval(ctx, a1, &itv));
        ctx->regs[0] = 0U;
        break;
    }

    case ARM_NR_setitimer: {
        struct itimerval new_itv, old_itv;
        memset(&new_itv, 0, sizeof(new_itv));
        memset(&old_itv, 0, sizeof(old_itv));
        if (a1 != 0U) {
            FAIL_EFAULT_IF(!read_guest_itimerval(ctx, a1, &new_itv));
            FAIL_EINVAL_IF(new_itv.it_interval.tv_usec < 0 || new_itv.it_interval.tv_usec > 999999 ||
                           new_itv.it_value.tv_usec < 0 || new_itv.it_value.tv_usec > 999999);
        }
        if (a2 != 0U) {
            FAIL_EFAULT_IF(!a32_guest_range_ok(ctx, a2, 16U, PROT_WRITE));
        }
        if (setitimer((int)a0, (a1 != 0U) ? &new_itv : NULL, (a2 != 0U) ? &old_itv : NULL) != 0) {
            FAIL_ERR(errno);
        }
        if (a2 != 0U) {
            (void)write_guest_itimerval(ctx, a2, &old_itv);
        }
        ctx->regs[0] = 0U;
        break;
    }

    case ARM_NR_getdents64: {
        FAIL_EFAULT_IF(a1 < 0x1000U);
#if defined(__linux__)
        long r = syscall(SYS_getdents64, (int)a0, a32_guest_ptr(ctx, a1), (size_t)a2);
        if (r < 0) {
            FAIL_ERR(errno);
        }
        if (!(ctx->has_host_info && (ctx->host_info.caps & A32H_CAP_DIROFF32))) {
            uint8_t *p = (uint8_t *)a32_guest_ptr(ctx, a1);
            size_t pos = 0;
            while (pos < (size_t)r) {
                struct linux_dirent64 *d = (struct linux_dirent64 *)(p + pos);
                if (d->d_reclen == 0) {
                    break;
                }
                d->d_off = map_dir_cookie((int)a0, (uint64_t)d->d_off);
                pos += d->d_reclen;
            }
        }
        ctx->regs[0] = (uint32_t)r;
#else
        FAIL_ERR(ENOSYS);
#endif
        break;
    }

    case ARM_NR_getdents: {
        FAIL_EFAULT_IF(a1 < 0x1000U);
#if defined(__linux__)
        size_t host_buf_size = (a2 > 32768U) ? 65536U : (size_t)a2 * 2U;
        if (host_buf_size < 1024U) {
            host_buf_size = 1024U;
        }
        uint8_t *host_buf = (uint8_t *)malloc(host_buf_size);
        if (!host_buf) {
            FAIL_ERR(ENOMEM);
        }
        off_t last_off = lseek((int)a0, 0, SEEK_CUR);
        long nread = syscall(SYS_getdents64, (int)a0, host_buf, host_buf_size);
        if (nread < 0) {
            free(host_buf);
            FAIL_ERR(errno);
        }
        uint8_t *out = (uint8_t *)a32_guest_ptr(ctx, a1);
        size_t in_pos = 0U;
        size_t out_pos = 0U;
        bool too_small = false;
        while (in_pos < (size_t)nread) {
            struct linux_dirent64 *d64 = (struct linux_dirent64 *)(host_buf + in_pos);
            if (d64->d_reclen == 0U) {
                break;
            }
            size_t namelen = strlen(d64->d_name);
            size_t reclen32 = (10U + namelen + 1U + 1U + 3U) & ~3U;
            if (out_pos + reclen32 > a2) {
                (void)lseek((int)a0, last_off, SEEK_SET);
                if (out_pos == 0U) {
                    too_small = true;
                }
                break;
            }
            uint32_t *d32_words = (uint32_t *)(out + out_pos);
            d32_words[0] = (uint32_t)d64->d_ino;
            d32_words[1] = (uint32_t)(map_dir_cookie((int)a0, (uint64_t)d64->d_off) & 0x7FFFFFFFU);
            *(uint16_t *)(out + out_pos + 8) = (uint16_t)reclen32;
            memcpy(out + out_pos + 10, d64->d_name, namelen + 1U);
            out[out_pos + reclen32 - 1U] = d64->d_type;
            last_off = (off_t)d64->d_off;
            out_pos += reclen32;
            in_pos += d64->d_reclen;
        }
        free(host_buf);
        ctx->regs[0] = too_small ? (uint32_t)(-EINVAL) : (uint32_t)out_pos;
#else
        FAIL_ERR(ENOSYS);
#endif
        break;
    }

    case ARM_NR_fork:
    case ARM_NR_vfork:
    case ARM_NR_clone: {
        return a32_sys_handle_fork_clone(ctx, nr, a0, a1, a2, a3, a4);
    }

    case ARM_NR_execve:
    case ARM_NR_execveat: {
        int dirfd         = (nr == ARM_NR_execveat) ? translate_at_fd(a0) : AT_FDCWD;
        uint32_t path_va  = (nr == ARM_NR_execveat) ? a1 : a0;
        uint32_t argv_va  = (nr == ARM_NR_execveat) ? a2 : a1;
        uint32_t envp_va  = (nr == ARM_NR_execveat) ? a3 : a2;
        uint32_t ex_flags = (nr == ARM_NR_execveat) ? a4 : 0U;

        FAIL_EFAULT_IF(!a32_guest_range_ok(ctx, path_va, 1U, PROT_READ));

        const char *guest_orig_path = (const char *)a32_guest_ptr(ctx, path_va);
        bool follow = ((ex_flags & AT_SYMLINK_NOFOLLOW) == 0U);
        bool is_empty_path_fd = false;
        char resolved[PATH_MAX];
        if ((ex_flags & AT_EMPTY_PATH) != 0U &&
            guest_orig_path[0] == '\0' && dirfd >= 0) {
            if (fcntl(dirfd, F_GETFD) < 0) {
                FAIL_ERR(EBADF);
            }
            is_empty_path_fd = true;
            snprintf(resolved, sizeof(resolved), "/proc/self/fd/%d", dirfd);
        } else {
            int path_rc = resolve_guest_path_at(ctx, dirfd, path_va, resolved, sizeof(resolved), follow);
            if (path_rc < 0) {
                ctx->regs[0] = (uint32_t)path_rc;
                break;
            }
            if (!follow) {
                struct stat st_l;
                if (lstat(resolved, &st_l) == 0 && S_ISLNK(st_l.st_mode)) {
                    FAIL_ERR(ELOOP);
                }
            }
        }
        const char *path = resolved;

        char *new_argv[512];
        char *new_envp[512];
        int argc_count = 0;
        FAIL_EFAULT_IF(!read_guest_strv(ctx, argv_va, new_argv, 500, &argc_count) ||
                       !read_guest_strv(ctx, envp_va, new_envp, 510, NULL));
        char **target_envp = (envp_va != 0U) ? new_envp : (ctx->host_envp ? ctx->host_envp : environ);

        /* Shebang (#!) script support */
        char interp_resolved[PATH_MAX];
        char interp_argv0[PATH_MAX];
        char shebang_line[256];
        if (!is_arm32_elf_file(path) && access(path, X_OK) == 0) {
            int sfd = open(path, O_RDONLY | O_CLOEXEC);
            if (sfd >= 0) {
                ssize_t nread = read(sfd, shebang_line, sizeof(shebang_line) - 1U);
                close(sfd);
                if (nread >= 2 && shebang_line[0] == '#' && shebang_line[1] == '!') {
                    shebang_line[nread] = '\0';
                    char *nl = strpbrk(shebang_line, "\r\n");
                    if (nl != NULL) {
                        *nl = '\0';
                    }
                    char *ip = shebang_line + 2;
                    while (*ip == ' ' || *ip == '\t') {
                        ip++;
                    }
                    if (*ip != '\0') {
                        char *interp_start = ip;
                        while (*ip != '\0' && *ip != ' ' && *ip != '\t') {
                            ip++;
                        }
                        char *opt_arg = NULL;
                        if (*ip != '\0') {
                            *ip++ = '\0';
                            while (*ip == ' ' || *ip == '\t') {
                                ip++;
                            }
                            if (*ip != '\0') {
                                opt_arg = ip;
                                char *endp = opt_arg + strlen(opt_arg);
                                while (endp > opt_arg && (endp[-1] == ' ' || endp[-1] == '\t')) {
                                    *--endp = '\0';
                                }
                            }
                        }
                        if (resolve_guest_path_str_at(ctx, AT_FDCWD, interp_start,
                                                      interp_resolved, sizeof(interp_resolved), true) == 0) {
                            snprintf(interp_argv0, sizeof(interp_argv0), "%s", interp_start);
                            char *rewritten_argv[512];
                            int rw_count = 0;
                            rewritten_argv[rw_count++] = interp_argv0;
                            if (opt_arg != NULL && opt_arg[0] != '\0') {
                                rewritten_argv[rw_count++] = opt_arg;
                            }
                            rewritten_argv[rw_count++] = (is_arm32_elf_file(interp_resolved) && !is_empty_path_fd)
                                                         ? (char *)guest_orig_path
                                                         : resolved;
                            for (int k = 1; k < argc_count && rw_count < 511; k++) {
                                rewritten_argv[rw_count++] = new_argv[k];
                            }
                            rewritten_argv[rw_count] = NULL;
                            for (int k = 0; k <= rw_count; k++) {
                                new_argv[k] = rewritten_argv[k];
                            }
                            argc_count = rw_count;
                            path = interp_resolved;
                        }
                    }
                }
            }
        }

        int saved_fd_flags = -1;
        if (is_empty_path_fd && dirfd >= 0) {
            saved_fd_flags = fcntl(dirfd, F_GETFD);
            if (saved_fd_flags >= 0 && (saved_fd_flags & FD_CLOEXEC) != 0) {
                (void)fcntl(dirfd, F_SETFD, saved_fd_flags & ~FD_CLOEXEC);
            }
        }

        char sysroot_env[PATH_MAX + 32];
        char cache_env[PATH_MAX + 32];
        char *hl_envp[520];
        if (ctx->has_host_info &&
            ((ctx->sysroot && ctx->sysroot[0]) || (ctx->cache_dir && ctx->cache_dir[0]) || ctx->auto_aot)) {
            int epos = 0;
            bool has_sr = false;
            bool has_ca = false;
            bool has_aa = false;
            if (target_envp != NULL) {
                for (; target_envp[epos] != NULL && epos < 516; epos++) {
                    if (strncmp(target_envp[epos], "A32RT_SYSROOT=", 14) == 0) {
                        has_sr = true;
                    } else if (strncmp(target_envp[epos], "A32RT_CACHE=", 12) == 0) {
                        has_ca = true;
                    } else if (strncmp(target_envp[epos], "A32RT_AUTO_AOT=", 15) == 0) {
                        has_aa = true;
                    }
                    hl_envp[epos] = target_envp[epos];
                }
            }
            if (!has_sr && ctx->sysroot && ctx->sysroot[0]) {
                snprintf(sysroot_env, sizeof(sysroot_env), "A32RT_SYSROOT=%s", ctx->sysroot);
                hl_envp[epos++] = sysroot_env;
            }
            if (!has_ca && ctx->cache_dir && ctx->cache_dir[0]) {
                snprintf(cache_env, sizeof(cache_env), "A32RT_CACHE=%s", ctx->cache_dir);
                hl_envp[epos++] = cache_env;
            }
            if (!has_aa && ctx->auto_aot) {
                hl_envp[epos++] = (char *)"A32RT_AUTO_AOT=1";
            }
            hl_envp[epos] = NULL;
            target_envp = hl_envp;
        }

        if (!ctx->has_host_info && is_arm32_elf_file(path)) {
            char self_exe[PATH_MAX];
            ssize_t sn = readlink("/proc/self/exe", self_exe, sizeof(self_exe) - 1);
            if (sn <= 0) {
                snprintf(self_exe, sizeof(self_exe), "/proc/self/exe");
            } else {
                self_exe[sn] = '\0';
            }

            char win_buf[32];
            char *re_argv[525];
            int rpos = 0;
            re_argv[rpos++] = self_exe;
            if (ctx->guest_base != 0ULL) {
                snprintf(win_buf, sizeof(win_buf), "0x%llx", (unsigned long long)ctx->guest_base);
                re_argv[rpos++] = (char *)"--window";
                re_argv[rpos++] = win_buf;
            }
            if (ctx->sysroot && ctx->sysroot[0]) {
                re_argv[rpos++] = (char *)"--sysroot";
                re_argv[rpos++] = (char *)ctx->sysroot;
            }
            if (ctx->cache_dir && ctx->cache_dir[0]) {
                re_argv[rpos++] = (char *)"--cache";
                re_argv[rpos++] = (char *)ctx->cache_dir;
            }
            if (ctx->auto_aot) {
                re_argv[rpos++] = (char *)"--auto-aot";
            }
            re_argv[rpos++] = (char *)"--";
            re_argv[rpos++] = (char *)path;
            for (int k = 0; k < argc_count && rpos < 524; k++) {
                re_argv[rpos++] = new_argv[k];
            }
            re_argv[rpos] = NULL;
            /* Cross-RT execve class: a re-exec replaces the image, so the
             * blocked mask, ignored dispositions, and pending signals must
             * travel in the environment — Linux keeps the mask and the
             * ignored set across exec, and caught handlers revert to
             * default (nothing caught survives here by construction). */
            char exec_mask_env[64];
            char exec_ign_env[64];
            char exec_pend_env[64];
            char *re_envp[520];
            int repos = 0;
            uint64_t nochld = ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
            uint64_t exec_ign = a32_signals_ignored_mask() & nochld;
            uint64_t exec_pend =
                __atomic_load_n(&ctx->pending_signals, __ATOMIC_SEQ_CST) & nochld;
            snprintf(exec_mask_env, sizeof(exec_mask_env), "A32RT_EXEC_SIGMASK=%llx",
                (unsigned long long)(ctx->sig_mask & nochld));
            snprintf(exec_ign_env, sizeof(exec_ign_env), "A32RT_EXEC_SIGIGN=%llx",
                (unsigned long long)exec_ign);
            snprintf(exec_pend_env, sizeof(exec_pend_env), "A32RT_EXEC_SIGPEND=%llx",
                (unsigned long long)exec_pend);
            if (target_envp != NULL) {
                for (int tpos = 0; target_envp[tpos] != NULL && repos < 516; tpos++) {
                    /* A32-R4-7: drop stale transport entries carried in
                     * the guest-supplied environment so exactly one
                     * current value per key is appended below. */
                    if (is_exec_sig_transport(target_envp[tpos])) {
                        continue;
                    }
                    re_envp[repos++] = target_envp[tpos];
                }
            }
            re_envp[repos++] = exec_mask_env;
            re_envp[repos++] = exec_ign_env;
            re_envp[repos++] = exec_pend_env;
            re_envp[repos] = NULL;
            execve(self_exe, re_argv, re_envp);
        } else {
            /* Native child: strip transport entries instead of leaking
             * stale execve-class state into a non-translator image.
             * A32-R4-7: size native_envp dynamically from the
             * environ count so large environments are never
             * truncated by a fixed array. */
            size_t nenv = 0U;
            char **native_envp = NULL;
            size_t npos = 0U;
            if (target_envp != NULL) {
                while (target_envp[nenv] != NULL) {
                    nenv++;
                }
            }
            native_envp = (char **)malloc((nenv + 1U) * sizeof(char *));
            if (native_envp == NULL) {
                if (saved_fd_flags >= 0 && (saved_fd_flags & FD_CLOEXEC) != 0) {
                    int saved_err = ENOMEM;
                    (void)fcntl(dirfd, F_SETFD, saved_fd_flags);
                    errno = saved_err;
                } else {
                    errno = ENOMEM;
                }
                FAIL_ERR(errno);
            }
            if (target_envp != NULL) {
                for (size_t tpos = 0U; target_envp[tpos] != NULL; tpos++) {
                    if (is_exec_sig_transport(target_envp[tpos])) {
                        continue;
                    }
                    native_envp[npos++] = target_envp[tpos];
                }
            }
            native_envp[npos] = NULL;
            execve(path, new_argv, native_envp);
            free(native_envp);
        }
        if (saved_fd_flags >= 0 && (saved_fd_flags & FD_CLOEXEC) != 0) {
            int saved_err = errno;
            (void)fcntl(dirfd, F_SETFD, saved_fd_flags);
            errno = saved_err;
        }
        FAIL_ERR(errno);
    }

    case ARM_NR_wait4: {
        FAIL_EFAULT_IF((a1 != 0U && !a32_guest_range_ok(ctx, a1, 4U, PROT_WRITE)) ||
                       (a3 != 0U && !a32_guest_range_ok(ctx, a3, (uint32_t)sizeof(arm_rusage32_t), PROT_WRITE)));
        int status = 0;
        struct rusage ru;
        pid_t r = wait4((pid_t)a0, &status, (int)a2, a3 ? &ru : NULL);
        if (r < 0) {
            FAIL_ERR(errno);
        }
        if (a1 != 0U) {
            *(int32_t *)a32_guest_ptr(ctx, a1) = status;
        }
        if (a3 != 0U) {
            fill_arm_rusage32((arm_rusage32_t *)a32_guest_ptr(ctx, a3), &ru);
        }
        ctx->regs[0] = (uint32_t)r;
        break;
    }

    case ARM_NR_waitid: {
        FAIL_EFAULT_IF((a2 != 0U && !a32_guest_range_ok(ctx, a2, 128U, PROT_WRITE)) ||
                       (a4 != 0U && !a32_guest_range_ok(ctx, a4, (uint32_t)sizeof(arm_rusage32_t), PROT_WRITE)));
#if defined(__linux__)
        siginfo_t si = { 0 };
        struct rusage ru = { 0 };
        long r = syscall(SYS_waitid, (int)a0, (id_t)a1,
                         (a2 != 0U) ? &si : NULL, (int)a3,
                         (a4 != 0U) ? &ru : NULL);
        if (r < 0) {
            FAIL_ERR(errno);
        }
        if (a2 != 0U) {
            uint32_t *p = (uint32_t *)a32_guest_ptr(ctx, a2);
            memset(p, 0, 128);
            p[0] = (uint32_t)si.si_signo;
            p[1] = (uint32_t)si.si_errno;
            p[2] = (uint32_t)si.si_code;
            p[3] = (uint32_t)si.si_pid;
            p[4] = (uint32_t)si.si_uid;
            p[5] = (uint32_t)si.si_status;
        }
        if (a4 != 0U) {
            fill_arm_rusage32((arm_rusage32_t *)a32_guest_ptr(ctx, a4), &ru);
        }
        ctx->regs[0] = 0U;
#else
        FAIL_ERR(ENOSYS);
#endif
        break;
    }

    case ARM_NR_futex:
    case ARM_NR_futex_time64: {
        FAIL_EFAULT_IF(a0 < 0x1000U);
        bool time64 = (nr == ARM_NR_futex_time64);
        uint32_t basic_op = a1 & 0x7FU;
        bool has_timeout = (basic_op == 0U  /* FUTEX_WAIT */ ||
                            basic_op == 6U  /* FUTEX_LOCK_PI */ ||
                            basic_op == 9U  /* FUTEX_WAIT_BITSET */ ||
                            basic_op == 11U /* FUTEX_WAIT_REQUEUE_PI */ ||
                            basic_op == 13U /* FUTEX_LOCK_PI2 */);
        struct timespec host_ts;
        const void *arg4 = NULL;

        if (has_timeout) {
            if (a3 != 0U) {
                FAIL_EFAULT_IF(!read_guest_timespec(ctx, a3, time64, &host_ts));
                arg4 = &host_ts;
            }
        } else {
            arg4 = (const void *)(uintptr_t)a3;
        }

        bool takes_uaddr2 = (basic_op == 3U  /* FUTEX_REQUEUE */ ||
                             basic_op == 4U  /* FUTEX_CMP_REQUEUE */ ||
                             basic_op == 5U  /* FUTEX_WAKE_OP */ ||
                             basic_op == 11U /* FUTEX_WAIT_REQUEUE_PI */ ||
                             basic_op == 12U /* FUTEX_CMP_REQUEUE_PI */);
        void *uaddr2 = NULL;
        if (takes_uaddr2) {
            FAIL_EFAULT_IF(!a32_guest_range_ok(ctx, a4, 4U, PROT_READ));
            uaddr2 = a32_guest_ptr(ctx, a4);
        } else {
            uaddr2 = (void *)(uintptr_t)a4;
        }
#if defined(__linux__)
        ctx->regs[0] = host_ret(syscall(SYS_futex, a32_guest_ptr(ctx, a0),
                                        (int)a1, a2, arg4, uaddr2, a5));
#else
        (void)arg4; (void)uaddr2;
        if (!futex_darwin_wait(ctx, a0, basic_op, a2, a3, a5, time64)) {
            ctx->regs[0] = 0U;
        }
#endif
        break;
    }

    default:
        return a32_sys_dispatch_sock(ctx);
    }
    return 0;
}
