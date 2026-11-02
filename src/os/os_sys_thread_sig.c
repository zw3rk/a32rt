/*
 * src/os/os_sys_thread_sig.c - Thread/process/memory/stat/ioctl syscalls and top-level dispatcher
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "os/sys_priv.h"

static void thread_exit_cleanup(a32_ctx_t *ctx)
{
    a32_cleanup_robust_list(ctx);
    uint32_t zero = 0U;
    if (guest_write(ctx, ctx->clear_child_tid, &zero, 4U)) {
#if defined(__linux__)
        syscall(SYS_futex, a32_guest_ptr(ctx, ctx->clear_child_tid),
                1 /* FUTEX_WAKE */, 1, NULL, NULL, 0);
#endif
    }
}

void *guest_thread_start(void *arg)
{
    child_thread_args_t *args = (child_thread_args_t *)arg;
    a32_ctx_t *ctx    = args->child_ctx;
    uint32_t flags    = args->flags;
    uint32_t ptid_va  = args->ptid_va;
    uint32_t ctid_va  = args->ctid_va;

    a32_signals_set_thread_ctx(ctx);
    ctx->host_tid        = 0;
    ctx->host_tid        = get_host_tid(ctx);
    ctx->is_thread_clone = true;
    args->child_tid      = ctx->host_tid;

    uint32_t tid32 = (uint32_t)ctx->host_tid;
    if (flags & CLONE_PARENT_SETTID) {
        (void)guest_write(ctx, ptid_va, &tid32, 4U);
    }
    if (flags & CLONE_CHILD_SETTID) {
        (void)guest_write(ctx, ctid_va, &tid32, 4U);
    }

    __atomic_store_n(&args->started, 1, __ATOMIC_SEQ_CST);

    a32_jit_run(ctx);

    thread_exit_cleanup(ctx);
    a32_signals_set_thread_ctx(NULL);
    free(ctx);
    return NULL;
}

static void print_stats_if_enabled(const a32_ctx_t *ctx)
{
    if (!ctx->stats_enabled) {
        return;
    }
    fprintf(stderr,
            "[a32rt stats] blocks_compiled=%llu blocks_executed=%llu chain_hits=%llu "
            "ibtc_hits=%llu ras_hits=%llu flags_elided=%llu syscalls=%llu "
            "icache_flushes=%llu wx_transitions=%llu window=0x%llx\n",
            (unsigned long long)ctx->blocks_compiled,
            (unsigned long long)ctx->blocks_executed,
            (unsigned long long)ctx->chain_hits,
            (unsigned long long)ctx->ibtc_hits,
            (unsigned long long)ctx->ras_hits,
            (unsigned long long)ctx->flags_elided,
            (unsigned long long)ctx->syscalls_executed,
            (unsigned long long)ctx->icache_flushes,
            (unsigned long long)ctx->wx_transitions,
            (unsigned long long)ctx->guest_base);
}

void check_pwait_signals(a32_ctx_t *ctx, uint64_t temp_mask)
{
    ctx->sig_mask = temp_mask;
    a32_signals_check_pending(ctx);
}

void restore_pwait_sigmask(a32_ctx_t *ctx, uint64_t saved_sig_mask)
{
    if (ctx->exit_reason == A32_EXIT_BRANCH) {
        uint8_t *fp = (uint8_t *)a32_guest_ptr(ctx, ctx->regs[13]);
        uint32_t oldmask32 = (uint32_t)saved_sig_mask;
        memcpy(fp + 156U, &oldmask32, sizeof(oldmask32));
        memcpy(fp + 232U, &saved_sig_mask, sizeof(saved_sig_mask));
        memcpy(fp + 968U, &saved_sig_mask, sizeof(saved_sig_mask));
    } else {
        ctx->sig_mask = saved_sig_mask;
    }
}

int pwait_sigmask_enter(a32_ctx_t *ctx, uint32_t mask_va, uint32_t mask_sz,
                        bool *has_temp_mask, uint64_t *saved_sig_mask,
                        uint64_t *temp_mask)
{
    *has_temp_mask  = false;
    *saved_sig_mask = ctx->sig_mask;
    *temp_mask      = 0ULL;
    if (mask_va == 0U) {
        return 0;
    }
    if (!guest_read(ctx, temp_mask, mask_va, 8U)) {
        return -EFAULT;
    }
    if (mask_sz != 8U) {
        return -EINVAL;
    }
    *temp_mask &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
    *has_temp_mask = true;
    ctx->sig_mask  = *temp_mask;
    if ((__atomic_load_n(&ctx->pending_signals, __ATOMIC_SEQ_CST) & ~(*temp_mask)) != 0ULL) {
        uint32_t orig_r0 = ctx->regs[0];
        ctx->regs[0] = (uint32_t)(-EINTR);
        check_pwait_signals(ctx, *temp_mask);
        if (ctx->exit_reason == A32_EXIT_BRANCH) {
            restore_pwait_sigmask(ctx, *saved_sig_mask);
            return 1;
        }
        ctx->sig_mask = *temp_mask;
        ctx->regs[0]  = orig_r0;
    }
    return 0;
}

void pwait_sigmask_leave(a32_ctx_t *ctx, bool has_temp_mask,
                         uint64_t saved_sig_mask, uint64_t temp_mask)
{
    if (has_temp_mask) {
        check_pwait_signals(ctx, temp_mask);
        restore_pwait_sigmask(ctx, saved_sig_mask);
    }
}

int translate_guest_iov(a32_ctx_t *ctx, uint32_t iov_va, uint32_t iovcnt,
                        struct iovec *stack_iov, uint32_t stack_cap,
                        struct iovec **out_iov)
{
    if ((int32_t)iovcnt < 0 || iovcnt > 1024U) {
        return -EINVAL;
    }
    if (iovcnt == 0U) {
        *out_iov = stack_iov;
        return 0;
    }
    if (!a32_guest_range_ok(ctx, iov_va, iovcnt * 8U, PROT_READ)) {
        return -EFAULT;
    }
    struct iovec *hiov = stack_iov;
    if (iovcnt > stack_cap) {
        hiov = (struct iovec *)calloc(iovcnt, sizeof(struct iovec));
        if (!hiov) {
            return -ENOMEM;
        }
    }
    const arm_iovec32_t *giov = (const arm_iovec32_t *)a32_guest_ptr(ctx, iov_va);
    for (uint32_t i = 0U; i < iovcnt; i++) {
        if (giov[i].iov_len > 0U && giov[i].iov_base < 0x1000U) {
            if (hiov != stack_iov) {
                free(hiov);
            }
            return -EFAULT;
        }
        hiov[i].iov_base = (giov[i].iov_len > 0U) ? a32_guest_ptr(ctx, giov[i].iov_base) : NULL;
        hiov[i].iov_len  = (size_t)giov[i].iov_len;
    }
    *out_iov = hiov;
    return 0;
}

bool read_guest_strv(a32_ctx_t *ctx, uint32_t vec_va, char **out, int max_count, int *out_count)
{
    int count = 0;
    if (vec_va != 0U) {
        for (;;) {
            uint32_t slot_va = vec_va + (uint32_t)count * 4U;
            uint32_t str_va = 0U;
            if (!guest_read(ctx, &str_va, slot_va, 4U)) {
                return false;
            }
            if (str_va == 0U) {
                break;
            }
            if (count >= max_count) {
                return false;
            }
            /* Verify string is NUL-terminated within readable guest pages */
            uint32_t cur = str_va;
            for (;;) {
                uint32_t page_rem = A32_PAGE_SIZE - (cur & (A32_PAGE_SIZE - 1U));
                if (!a32_guest_range_ok(ctx, cur, 1U, PROT_READ)) {
                    return false;
                }
                const char *p = (const char *)a32_guest_ptr(ctx, cur);
                const void *nul = memchr(p, '\0', page_rem);
                if (nul != NULL) {
                    break;
                }
                if (cur + page_rem < cur) {
                    return false;
                }
                cur += page_rem;
            }
            out[count] = (char *)a32_guest_ptr(ctx, str_va);
            count++;
        }
    }
    out[count] = NULL;
    if (out_count) {
        *out_count = count;
    }
    return true;
}

/* Darwin futex WAIT emulation (no kernel futex waits on macOS).  Returns
 * true when the op is fully handled (regs[0] set); false falls through
 * to regs[0] = 0.  Guest (Linux) errnos throughout: EAGAIN 11 (Darwin
 * host 35), ETIMEDOUT 110, EINVAL 22 (same both sides).  No wait queue
 * yet: matched waits with a timeout sleep it out, then time out. */
#if !defined(__linux__)
bool futex_darwin_wait(a32_ctx_t *ctx, uint32_t a0, uint32_t basic_op,
                       uint32_t a2, uint32_t a3, uint32_t a5, bool time64)
{
    uint32_t *uaddr = (uint32_t *)a32_guest_ptr(ctx, a0);
    if (basic_op == 9U && a5 == 0U) {
        ctx->regs[0] = (uint32_t)-EINVAL;
        return true;
    }
    if (basic_op == 4U && __atomic_load_n(uaddr, __ATOMIC_SEQ_CST) != a5) {
        ctx->regs[0] = (uint32_t)-11;
        return true;
    }
    if (basic_op != 0U && basic_op != 9U) {
        return false;
    }
    if (__atomic_load_n(uaddr, __ATOMIC_SEQ_CST) != a2) {
        ctx->regs[0] = (uint32_t)-11;
        return true;
    }
    if (a3 == 0U) {
        return false;
    }
    {
        struct timespec end_m;
        struct timespec now_m;
        long ds;
        long dn;
        if (read_guest_timespec(ctx, a3, time64, &end_m)) {
            struct timespec rem;
            (void)clock_gettime(CLOCK_MONOTONIC, &now_m);
            ds = (long)(end_m.tv_sec - now_m.tv_sec);
            dn = (long)(end_m.tv_nsec - now_m.tv_nsec);
            if (dn < 0L) {
                ds -= 1L;
                dn += 1000000000L;
            }
            if (ds >= 0L) {
                rem.tv_sec = (time_t)ds;
                rem.tv_nsec = dn;
                (void)nanosleep(&rem, NULL);
            }
        }
    }
    ctx->regs[0] = (uint32_t)-110;
    return true;
}
#endif /* !defined(__linux__) */

static int dispatch_thread_sig(a32_ctx_t *ctx)
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
    case ARM_NR_restart_syscall:
        ctx->regs[0] = 0U;
        break;

    case ARM_NR_exit:
    case ARM_NR_exit_group:
        thread_exit_cleanup(ctx);
        if (nr == ARM_NR_exit && ctx->is_thread_clone) {
            a32_signals_set_thread_ctx(NULL);
            free(ctx);
            pthread_exit(NULL);
        }
        if (ctx->auto_aot && !ctx->aot_loaded && ctx->blocks_compiled > 0U &&
            ctx->exec_path != NULL && ctx->exec_path[0] != '\0') {
            (void)a32_aot_save_cache(ctx, ctx->exec_path);
        }
        print_stats_if_enabled(ctx);
#if defined(__linux__)
        syscall(SYS_exit_group, (int)a0);
#else
        _exit((int)a0);
#endif
        return 1;

    case ARM_NR_read:
        FAIL_EFAULT_IF(a2 > 0U && a1 < 0x1000U);
        ctx->regs[0] = host_ret(read((int)a0, a32_guest_ptr(ctx, a1), (size_t)a2));
        break;

    case ARM_NR_write:
        FAIL_EFAULT_IF(a2 > 0U && a1 < 0x1000U);
        {
            ssize_t wr = write((int)a0, a32_guest_ptr(ctx, a1), (size_t)a2);
            if (wr > 0) {
                (void)a32_vmfile_sync_fd((int)a0);
            }
            ctx->regs[0] = host_ret(wr);
        }
        break;

    case ARM_NR_readv:
    case ARM_NR_writev:
    case ARM_NR_preadv:
    case ARM_NR_pwritev: {
        struct iovec host_iov[64];
        struct iovec *iov_ptr = NULL;
        int irc = translate_guest_iov(ctx, a1, a2, host_iov, 64U, &iov_ptr);
        if (irc < 0) {
            FAIL_ERR(-irc);
        }
        ssize_t r;
        if (nr == ARM_NR_preadv || nr == ARM_NR_pwritev) {
            off_t pos = (off_t)u64_from_u32(a3, a4);
            if (pos < 0) {
                if (iov_ptr != host_iov) {
                    free(iov_ptr);
                }
                FAIL_ERR(EINVAL);
            }
            r = (nr == ARM_NR_pwritev) ? pwritev((int)a0, iov_ptr, (int)a2, pos)
                                       : preadv((int)a0, iov_ptr, (int)a2, pos);
        } else {
            r = (nr == ARM_NR_writev) ? writev((int)a0, iov_ptr, (int)a2)
                                      : readv((int)a0, iov_ptr, (int)a2);
        }
        if ((nr == ARM_NR_writev || nr == ARM_NR_pwritev) && r > 0) {
            (void)a32_vmfile_sync_fd((int)a0);
        }
        if (iov_ptr != host_iov) {
            free(iov_ptr);
        }
        ctx->regs[0] = host_ret(r);
        break;
    }

    case ARM_NR_open:
    case ARM_NR_openat: {
        bool is_at       = (nr == ARM_NR_openat);
        int dirfd        = is_at ? translate_at_fd(a0) : AT_FDCWD;
        uint32_t path_va = is_at ? a1 : a0;
        uint32_t flags   = is_at ? a2 : a1;
        mode_t mode      = (mode_t)(is_at ? a3 : a2);
        char resolved[PATH_MAX];
        RESOLVE_AT(dirfd, path_va, resolved, (flags & ARM_O_NOFOLLOW) == 0U);
        const char *guest_path = (const char *)a32_guest_ptr(ctx, path_va);
        int vfd = try_open_virtual_proc(ctx, guest_path, resolved);
        if (vfd != VIRTUAL_PROC_NOT_MATCHED) {
            ctx->regs[0] = (uint32_t)vfd;
            break;
        }
        ctx->regs[0] = host_ret(openat(dirfd, resolved, translate_open_flags(flags), mode));
        break;
    }

    case ARM_NR_close:
        if (g_dir_cookie_count > 0U) {
            for (size_t i = 0U; i < g_dir_cookie_count; i++) {
                if (g_dir_cookies[i].fd == (int)a0) {
                    g_dir_cookies[i].fd = -1;
                }
            }
        }
        ctx->regs[0] = host_ret(close((int)a0));
        break;

    case ARM_NR_lseek: {
        off_t off = (off_t)(int32_t)a1;
        if (a2 == SEEK_SET) {
            (void)unmap_dir_cookie((int)a0, (int64_t)off, &off);
        }
        off_t res = lseek((int)a0, off, (int)a2);
        if (res < 0) {
            FAIL_ERR(errno);
        } else if ((uint64_t)res >= (uint64_t)DIR_COOKIE_BASE) {
            struct stat st;
            if (fstat((int)a0, &st) == 0 && S_ISDIR(st.st_mode)) {
                ctx->regs[0] = (uint32_t)map_dir_cookie((int)a0, (uint64_t)res);
            } else if (res > 0x7FFFFFFFL) {
                FAIL_ERR(EOVERFLOW);
            } else {
                ctx->regs[0] = (uint32_t)res;
            }
        } else {
            ctx->regs[0] = (uint32_t)res;
        }
        break;
    }

    case ARM_NR__llseek: {
        FAIL_EFAULT_IF(!a32_guest_range_ok(ctx, a3, 8U, PROT_WRITE));
        off_t off = (off_t)u64_from_u32(a2, a1);
        if (a4 == SEEK_SET) {
            (void)unmap_dir_cookie((int)a0, (int64_t)off, &off);
        }
        off_t res = lseek((int)a0, off, (int)a4);
        if (res < 0) {
            FAIL_ERR(errno);
        }
        uint64_t ures = (uint64_t)res;
        if (ures >= (uint64_t)DIR_COOKIE_BASE) {
            struct stat st;
            if (fstat((int)a0, &st) == 0 && S_ISDIR(st.st_mode)) {
                ures = (uint64_t)map_dir_cookie((int)a0, ures);
            }
        }
        memcpy(a32_guest_ptr(ctx, a3), &ures, sizeof(ures));
        ctx->regs[0] = 0U;
        break;
    }

    case ARM_NR_brk:
        ctx->regs[0] = a32_vm_brk(ctx, a0);
        break;

    case ARM_NR_mmap2:
        ctx->regs[0] = a32_vm_mmap2(ctx, a0, a1, a2, a3, (int)(int32_t)a4, a5);
        break;

    case ARM_NR_mprotect:
        ctx->regs[0] = (uint32_t)a32_vm_mprotect(ctx, a0, a1, a2);
        break;

    case ARM_NR_munmap:
        ctx->regs[0] = (uint32_t)a32_vm_munmap(ctx, a0, a1);
        break;

    case ARM_NR_mremap:
        ctx->regs[0] = a32_vm_mremap(ctx, a0, a1, a2, a3, a4);
        break;

    case ARM_NR_ioctl: {
        uint32_t cmd = a1;
        /* Block device ioctls */
        if (cmd == 0x1260U /* BLKGETSIZE */) {
            FAIL_EFAULT_IF(!a32_guest_range_ok(ctx, a2, 4U, PROT_WRITE));
            unsigned long sz64 = 0UL;
            int r = ioctl((int)a0, (int)0x1260U, &sz64);
            if (r == 0) {
                uint32_t sz32 = (uint32_t)sz64;
                memcpy(a32_guest_ptr(ctx, a2), &sz32, 4U);
            }
            ctx->regs[0] = host_ret(r);
            break;
        }
        if (cmd == 0x1268U /* BLKSSZGET */ || cmd == 0x127bU /* BLKPBSZGET */) {
            FAIL_EFAULT_IF(!a32_guest_range_ok(ctx, a2, 4U, PROT_WRITE));
            int val = 0;
            int r = ioctl((int)a0, (int)cmd, &val);
            if (r == 0) {
                memcpy(a32_guest_ptr(ctx, a2), &val, 4U);
            }
            ctx->regs[0] = host_ret(r);
            break;
        }
        if (cmd == 0x80041272U /* BLKGETSIZE64 (arm32) */ ||
            cmd == 0x80081272U /* BLKGETSIZE64 (aarch64) */) {
            FAIL_EFAULT_IF(!a32_guest_range_ok(ctx, a2, 8U, PROT_WRITE));
            uint64_t bsz64 = 0ULL;
            int r = ioctl((int)a0, (int)0x80081272U, &bsz64);
            if (r == 0) {
                memcpy(a32_guest_ptr(ctx, a2), &bsz64, 8U);
            }
            ctx->regs[0] = host_ret(r);
            break;
        }
        /* Network interface ioctls: struct ifreq (32B arm32 <-> 40B aarch64) */
        if (cmd == 0x8910U /* SIOCGIFNAME */ ||
            cmd == 0x8913U /* SIOCGIFFLAGS */ ||
            cmd == 0x8915U /* SIOCGIFADDR */ ||
            cmd == 0x8917U /* SIOCGIFDSTADDR */ ||
            cmd == 0x8919U /* SIOCGIFBRDADDR */ ||
            cmd == 0x891bU /* SIOCGIFNETMASK */ ||
            cmd == 0x891dU /* SIOCGIFMETRIC */ ||
            cmd == 0x8921U /* SIOCGIFMTU */ ||
            cmd == 0x8927U /* SIOCGIFHWADDR */ ||
            cmd == 0x8933U /* SIOCGIFINDEX */ ||
            cmd == 0x8942U /* SIOCGIFTXQLEN */) {
            FAIL_EFAULT_IF(!a32_guest_range_ok(ctx, a2, sizeof(arm_ifreq_t), PROT_READ | PROT_WRITE));
            arm_ifreq_t ifr32;
            memcpy(&ifr32, a32_guest_ptr(ctx, a2), sizeof(ifr32));
            a64_ifreq_t ifr64;
            ifr32_to_64(&ifr64, &ifr32);
            int r = ioctl((int)a0, (int)cmd, &ifr64);
            if (r == 0) {
                ifr64_to_32(&ifr32, &ifr64);
                memcpy(a32_guest_ptr(ctx, a2), &ifr32, sizeof(ifr32));
            }
            ctx->regs[0] = host_ret(r);
            break;
        }
        /* Network interface list: struct ifconf (8B arm32 <-> 16B aarch64) */
        if (cmd == 0x8912U /* SIOCGIFCONF */) {
            FAIL_EFAULT_IF(!a32_guest_range_ok(ctx, a2, sizeof(arm_ifconf_t), PROT_READ | PROT_WRITE));
            arm_ifconf_t ifc32;
            memcpy(&ifc32, a32_guest_ptr(ctx, a2), sizeof(ifc32));
            if (ifc32.ifc_buf == 0U) {
                a64_ifconf_t ifc64;
                memset(&ifc64, 0, sizeof(ifc64));
                int r = ioctl((int)a0, (int)0x8912U, &ifc64);
                if (r == 0) {
                    int n = ifc64.ifc_len / (int)sizeof(a64_ifreq_t);
                    ifc32.ifc_len = n * (int32_t)sizeof(arm_ifreq_t);
                    memcpy(a32_guest_ptr(ctx, a2), &ifc32, sizeof(ifc32));
                }
                ctx->regs[0] = host_ret(r);
                break;
            }
            FAIL_EINVAL_IF(ifc32.ifc_len < 0);
            FAIL_EFAULT_IF(ifc32.ifc_len > 0 &&
                           !a32_guest_range_ok(ctx, ifc32.ifc_buf, (uint32_t)ifc32.ifc_len, PROT_WRITE));
            int max_entries = ifc32.ifc_len / (int)sizeof(arm_ifreq_t);
            if (max_entries > 64) {
                max_entries = 64;
            }
            a64_ifreq_t buf64[64];
            memset(buf64, 0, sizeof(buf64));
            a64_ifconf_t ifc64;
            memset(&ifc64, 0, sizeof(ifc64));
            ifc64.ifc_len = max_entries * (int)sizeof(a64_ifreq_t);
            ifc64.ifc_buf = (max_entries > 0) ? (void *)buf64 : NULL;
            int r = (max_entries > 0) ? ioctl((int)a0, (int)0x8912U, &ifc64) : 0;
            if (r == 0) {
                int n = (max_entries > 0) ? (ifc64.ifc_len / (int)sizeof(a64_ifreq_t)) : 0;
                uint8_t *dst = (uint8_t *)a32_guest_ptr(ctx, ifc32.ifc_buf);
                for (int i = 0; i < n; i++) {
                    arm_ifreq_t entry32;
                    ifr64_to_32(&entry32, &buf64[i]);
                    memcpy(dst + (size_t)i * sizeof(arm_ifreq_t), &entry32, sizeof(arm_ifreq_t));
                }
                ifc32.ifc_len = n * (int32_t)sizeof(arm_ifreq_t);
                memcpy(a32_guest_ptr(ctx, a2), &ifc32, sizeof(ifc32));
            }
            ctx->regs[0] = host_ret(r);
            break;
        }
        if (cmd != 0x5450U /* FIONCLEX */ && cmd != 0x5451U /* FIOCLEX */ &&
            cmd != 0x540EU /* TIOCSCTTY */) {
            FAIL_EFAULT_IF(a2 != 0U && !a32_guest_range_ok(ctx, a2, 1U, PROT_READ));
        }
        void *arg_ptr = (a2 != 0U) ? a32_guest_ptr(ctx, a2) : NULL;
        ctx->regs[0] = host_ret(ioctl((int)a0, (int)a1, arg_ptr));
        break;
    }

    case ARM_NR_fcntl:
    case ARM_NR_fcntl64: {
        int cmd = (int)a1;
        if (cmd == 5 /* F_GETLK */ || cmd == 6 /* F_SETLK */ || cmd == 7 /* F_SETLKW */) {
            int r = handle_fcntl_flock(ctx, (int)a0, cmd, a2, false);
            if (r < 0) {
                FAIL_ERR(-r);
            }
            ctx->regs[0] = 0U;
            break;
        }
        if (cmd == 12 /* F_GETLK64 */  || cmd == 13 /* F_SETLK64 */  || cmd == 14 /* F_SETLKW64 */ ||
            cmd == 36 /* F_OFD_GETLK */ || cmd == 37 /* F_OFD_SETLK */ || cmd == 38 /* F_OFD_SETLKW */) {
            int r = handle_fcntl_flock(ctx, (int)a0, cmd, a2, true);
            if (r < 0) {
                FAIL_ERR(-r);
            }
            ctx->regs[0] = 0U;
            break;
        }
        if (cmd == 15 /* F_SETOWN_EX */ || cmd == 16 /* F_GETOWN_EX */) {
            FAIL_EFAULT_IF(a2 < 0x1000U);
            ctx->regs[0] = host_ret(fcntl((int)a0, cmd, a32_guest_ptr(ctx, a2)));
            break;
        }
        ctx->regs[0] = host_ret(fcntl((int)a0, cmd, (unsigned long)a2));
        break;
    }

    case ARM_NR_uname: {
        FAIL_EFAULT_IF(!a32_guest_range_ok(ctx, a0, sizeof(struct utsname), PROT_WRITE));
        struct utsname u;
        if (uname(&u) != 0) {
            FAIL_ERR(errno);
        }
        snprintf(u.machine, sizeof(u.machine), "armv8l");
        memcpy(a32_guest_ptr(ctx, a0), &u, sizeof(u));
        ctx->regs[0] = 0U;
        break;
    }

    case ARM_NR_fstat64: {
        FAIL_EFAULT_IF(!a32_guest_range_ok(ctx, a1, sizeof(arm_stat64_t), PROT_WRITE));
        struct stat st;
        if (fstat((int)a0, &st) != 0) {
            FAIL_ERR(errno);
        }
        fill_arm_stat64(ctx, (arm_stat64_t *)a32_guest_ptr(ctx, a1), &st);
        ctx->regs[0] = 0U;
        break;
    }

    case ARM_NR_stat64:
    case ARM_NR_lstat64:
    case ARM_NR_fstatat64: {
        bool is_at       = (nr == ARM_NR_fstatat64);
        int dirfd        = is_at ? translate_at_fd(a0) : AT_FDCWD;
        uint32_t path_va = is_at ? a1 : a0;
        uint32_t buf_va  = is_at ? a2 : a1;
        uint32_t gflags  = is_at ? a3 : ((nr == ARM_NR_lstat64) ? 0x100U : 0U);
        int flags        = (gflags & 0x100U) ? AT_SYMLINK_NOFOLLOW : 0;
        FAIL_EFAULT_IF(!a32_guest_range_ok(ctx, buf_va, sizeof(arm_stat64_t), PROT_WRITE));
        char resolved[PATH_MAX];
        RESOLVE_AT(dirfd, path_va, resolved, (flags & AT_SYMLINK_NOFOLLOW) == 0);
        if (strcmp(resolved, "/tmp") == 0) {
            flags &= ~AT_SYMLINK_NOFOLLOW;
        }
        struct stat st;
        if (fstatat(dirfd, resolved, &st, flags) != 0) {
            FAIL_ERR(errno);
        }
        fill_arm_stat64(ctx, (arm_stat64_t *)a32_guest_ptr(ctx, buf_va), &st);
        ctx->regs[0] = 0U;
        break;
    }

    case ARM_NR_access:
    case ARM_NR_faccessat:
    case ARM_NR_faccessat2: {
        bool is_at       = (nr != ARM_NR_access);
        int dirfd        = is_at ? translate_at_fd(a0) : AT_FDCWD;
        uint32_t path_va = is_at ? a1 : a0;
        uint32_t mode    = is_at ? a2 : a1;
        uint32_t gflags  = (nr == ARM_NR_faccessat2) ? a3 : 0U;
        FAIL_EINVAL_IF((is_at && (mode & ~7U) != 0U) ||
                       (gflags & ~(0x100U | 0x200U | 0x1000U)) != 0U);
        int hflags       = ((gflags & 0x100U) ? AT_SYMLINK_NOFOLLOW : 0) |
                           ((gflags & 0x200U) ? AT_EACCESS : 0);
        char resolved[PATH_MAX];
        RESOLVE_AT(dirfd, path_va, resolved, (hflags & AT_SYMLINK_NOFOLLOW) == 0);
        if (nr == ARM_NR_faccessat2) {
#if defined(__linux__)
            long rc = syscall(SYS_faccessat2, dirfd, resolved, (int)mode, (int)gflags);
            if (rc < 0 && errno == ENOSYS) {
                rc = faccessat(dirfd, resolved, (int)mode, hflags);
            }
#else
            long rc = faccessat(dirfd, resolved, (int)mode, hflags);
#endif
            ctx->regs[0] = host_ret(rc);
        } else {
            ctx->regs[0] = host_ret(faccessat(dirfd, resolved, (int)mode, 0));
        }
        break;
    }

    case ARM_NR_getcwd: {
        FAIL_EFAULT_IF(a0 < 0x1000U);
        char *buf = (char *)a32_guest_ptr(ctx, a0);
        size_t sr_len = sysroot_prefix_len(ctx);
        char host_cwd[PATH_MAX];
        if (getcwd(host_cwd, sizeof(host_cwd)) == NULL) {
            FAIL_ERR(errno);
        }
        const char *guest_cwd = host_cwd;
        if (sr_len > 0U) {
            if (strncmp(host_cwd, ctx->sysroot, sr_len) == 0 &&
                (host_cwd[sr_len] == '/' || host_cwd[sr_len] == '\0')) {
                guest_cwd = (host_cwd[sr_len] == '\0') ? "/" : (host_cwd + sr_len);
            } else if (strcmp(host_cwd, "/private/tmp") == 0 ||
                       strncmp(host_cwd, "/private/tmp/", 13) == 0) {
                guest_cwd = host_cwd + 8;
            }
        }
        size_t len = strlen(guest_cwd);
        if ((size_t)a1 < len + 1U) {
            FAIL_ERR(ERANGE);
        }
        memcpy(buf, guest_cwd, len + 1U);
        ctx->regs[0] = (uint32_t)(len + 1U);
        break;
    }

    case ARM_NR_getpid:
        ctx->regs[0] = (uint32_t)getpid();
        break;

    case ARM_NR_getppid:
        ctx->regs[0] = (uint32_t)getppid();
        break;

    case ARM_NR_getpgrp:
        ctx->regs[0] = (uint32_t)getpgrp();
        break;

    case ARM_NR_gettid:
        ctx->regs[0] = (uint32_t)get_host_tid(ctx);
        break;

    case ARM_NR_getuid32:
        ctx->regs[0] = (uint32_t)getuid();
        break;

    case ARM_NR_geteuid32:
        ctx->regs[0] = (uint32_t)geteuid();
        break;

    case ARM_NR_getgid32:
        ctx->regs[0] = (uint32_t)getgid();
        break;

    case ARM_NR_getegid32:
        ctx->regs[0] = (uint32_t)getegid();
        break;

    case ARM_NR_setuid32:
        ctx->regs[0] = host_ret(setuid((uid_t)a0));
        break;

    case ARM_NR_setgid32:
        ctx->regs[0] = host_ret(setgid((gid_t)a0));
        break;

    case ARM_NR_setreuid32:
        ctx->regs[0] = host_ret(setreuid((uid_t)a0, (uid_t)a1));
        break;

    case ARM_NR_setregid32:
        ctx->regs[0] = host_ret(setregid((gid_t)a0, (gid_t)a1));
        break;

    case ARM_NR_setresuid:
    case ARM_NR_setresuid32: {
        bool is16 = (nr == ARM_NR_setresuid);
        uid_t r = (uid_t)(is16 ? id16_to_32(a0) : a0);
        uid_t e = (uid_t)(is16 ? id16_to_32(a1) : a1);
        uid_t s = (uid_t)(is16 ? id16_to_32(a2) : a2);
#if defined(__linux__)
        ctx->regs[0] = host_ret(syscall(SYS_setresuid, r, e, s));
#else
        (void)s;
        ctx->regs[0] = host_ret(setreuid(r, e));
#endif
        break;
    }

    case ARM_NR_setresgid:
    case ARM_NR_setresgid32: {
        bool is16 = (nr == ARM_NR_setresgid);
        gid_t r = (gid_t)(is16 ? id16_to_32(a0) : a0);
        gid_t e = (gid_t)(is16 ? id16_to_32(a1) : a1);
        gid_t s = (gid_t)(is16 ? id16_to_32(a2) : a2);
#if defined(__linux__)
        ctx->regs[0] = host_ret(syscall(SYS_setresgid, r, e, s));
#else
        (void)s;
        ctx->regs[0] = host_ret(setregid(r, e));
#endif
        break;
    }

    case ARM_NR_getresuid32:
    case ARM_NR_getresgid32: {
        FAIL_EFAULT_IF((a0 != 0U && !a32_guest_range_ok(ctx, a0, 4U, PROT_WRITE)) ||
                       (a1 != 0U && !a32_guest_range_ok(ctx, a1, 4U, PROT_WRITE)) ||
                       (a2 != 0U && !a32_guest_range_ok(ctx, a2, 4U, PROT_WRITE)));
        uint32_t r = 0U, e = 0U, s = 0U;
#if defined(__linux__)
        int rc = (nr == ARM_NR_getresuid32)
                 ? getresuid((uid_t *)&r, (uid_t *)&e, (uid_t *)&s)
                 : getresgid((gid_t *)&r, (gid_t *)&e, (gid_t *)&s);
        if (rc != 0) {
            FAIL_ERR(errno);
        }
#else
        r = (nr == ARM_NR_getresuid32) ? (uint32_t)getuid() : (uint32_t)getgid();
        e = (nr == ARM_NR_getresuid32) ? (uint32_t)geteuid() : (uint32_t)getegid();
        s = e;
#endif
        if (a0) *(uint32_t *)a32_guest_ptr(ctx, a0) = r;
        if (a1) *(uint32_t *)a32_guest_ptr(ctx, a1) = e;
        if (a2) *(uint32_t *)a32_guest_ptr(ctx, a2) = s;
        ctx->regs[0] = 0U;
        break;
    }

    case ARM_NR_getgroups32:
        FAIL_EINVAL_IF((int32_t)a0 < 0);
        FAIL_EFAULT_IF(a0 > 0U && a1 < 0x1000U);
        ctx->regs[0] = host_ret(getgroups((int)a0, a0 > 0U ? (gid_t *)a32_guest_ptr(ctx, a1) : NULL));
        break;

    case ARM_NR_setgroups32:
        FAIL_EINVAL_IF((int32_t)a0 < 0 || a0 > 65536U);
        FAIL_EFAULT_IF(a0 > 0U && a1 < 0x1000U);
        ctx->regs[0] = host_ret(setgroups((int)a0, a0 > 0U ? (const gid_t *)a32_guest_ptr(ctx, a1) : NULL));
        break;

    case ARM_NR_set_tid_address:
        ctx->clear_child_tid = a0;
        ctx->regs[0] = (uint32_t)get_host_tid(ctx);
        break;

    case ARM_NR_rt_sigaction:
        ctx->regs[0] = (uint32_t)a32_sys_rt_sigaction(ctx, (int)a0, a1, a2, a3);
        break;

    case ARM_NR_rt_sigprocmask:
        ctx->regs[0] = (uint32_t)a32_sys_rt_sigprocmask(ctx, (int)a0, a1, a2, a3);
        break;

    default:
        return a32_sys_dispatch_sig_time(ctx);
    }
    return 0;
}

int a32_handle_syscall(a32_ctx_t *ctx)
{
    uint32_t nr = ctx->regs[7];
    if (ctx->trace_enabled) {
        fprintf(stderr, "[a32rt] syscall nr=%u (0x%x) args=(0x%x, 0x%x, 0x%x, 0x%x)\n",
                nr, nr, ctx->regs[0], ctx->regs[1], ctx->regs[2], ctx->regs[3]);
    }
    int rc = dispatch_thread_sig(ctx);
    if (rc != 0) {
        return rc;
    }
    if ((ctx->exit_reason != A32_EXIT_BRANCH ||
         nr == ARM_NR_sigreturn ||
         nr == ARM_NR_rt_sigreturn) &&
        __builtin_expect((__atomic_load_n(&ctx->pending_signals, __ATOMIC_ACQUIRE) & ~ctx->sig_mask) != 0ULL, 0)) {
        a32_signals_check_pending(ctx);
    }
    return 0;
}
