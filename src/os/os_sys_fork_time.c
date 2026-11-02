/*
 * src/os/os_sys_fork_time.c - Stat/time struct shims, fork snapshot, and robust futex cleanup
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "os/sys_priv.h"
#include "dbt/jit_priv.h"

void fill_arm_stat64(const a32_ctx_t *ctx, arm_stat64_t *dst, const struct stat *src)
{
    memset(dst, 0, sizeof(*dst));
    dst->st_dev        = (uint64_t)src->st_dev;
    dst->__st_ino      = (uint32_t)src->st_ino;
    dst->st_mode       = (uint32_t)src->st_mode;
    dst->st_nlink      = (uint32_t)src->st_nlink;
    if (ctx && ctx->has_host_info && src->st_uid != 0U) {
        dst->st_uid    = (uint32_t)geteuid();
        dst->st_gid    = (uint32_t)getegid();
    } else {
        dst->st_uid    = (uint32_t)src->st_uid;
        dst->st_gid    = (uint32_t)src->st_gid;
    }
    dst->st_rdev       = (uint64_t)src->st_rdev;
    dst->st_size       = (int64_t)src->st_size;
    dst->st_blksize    = (uint32_t)src->st_blksize;
    dst->st_blocks     = (uint64_t)src->st_blocks;
#if defined(__APPLE__)
    dst->st_atime_sec  = (uint32_t)src->st_atimespec.tv_sec;
    dst->st_atime_nsec = (uint32_t)src->st_atimespec.tv_nsec;
    dst->st_mtime_sec  = (uint32_t)src->st_mtimespec.tv_sec;
    dst->st_mtime_nsec = (uint32_t)src->st_mtimespec.tv_nsec;
    dst->st_ctime_sec  = (uint32_t)src->st_ctimespec.tv_sec;
    dst->st_ctime_nsec = (uint32_t)src->st_ctimespec.tv_nsec;
#else
    dst->st_atime_sec  = (uint32_t)src->st_atim.tv_sec;
    dst->st_atime_nsec = (uint32_t)src->st_atim.tv_nsec;
    dst->st_mtime_sec  = (uint32_t)src->st_mtim.tv_sec;
    dst->st_mtime_nsec = (uint32_t)src->st_mtim.tv_nsec;
    dst->st_ctime_sec  = (uint32_t)src->st_ctim.tv_sec;
    dst->st_ctime_nsec = (uint32_t)src->st_ctim.tv_nsec;
#endif
    dst->st_ino        = (uint64_t)src->st_ino;
}

typedef struct fork_subregion {
    uint32_t start;
    uint32_t end;
    uint32_t prot;
    size_t   buf_off;
} fork_subregion_t;

typedef struct fork_cluster {
    uint32_t start;
    uint32_t end;
    size_t   first_idx;
    size_t   count;
} fork_cluster_t;

/* The fork child starts with exactly one thread: drop every lock and
 * lease a vanished thread may have held. */
static void fork_child_reset(void)
{
    a32_jit_fork_child();
    a32_vm_fork_child_reset();
    a32_vmfile_fork_child_reset();
}

/* Reconcile one fork() return issued after a32_jit_fork_prepare: the
 * child resets, the parent (or a failed fork, which only has a parent)
 * resumes execution entrants. */
static pid_t fork_reconciled(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        fork_child_reset();
    } else {
        a32_jit_fork_parent();
    }
    return pid;
}

pid_t fork_with_guest_snapshot(a32_ctx_t *ctx, uint32_t child_sp)
{
    sigset_t block_all;
    sigset_t saved_set;
    bool masked = false;
    if (!ctx || !ctx->has_host_info) {
        /* Plain-Linux fork (ground-truth oracle under lima): still
         * order the JIT lock against fork() so a sibling thread
         * mid-compile cannot leave the child with a half-mutated
         * cache and a zeroed lock (A32-R3-4 follow-up 4). No guest
         * snapshot is needed outside hl; the quiesce alone suffices. */
        pid_t pid;
        sigfillset(&block_all);
        masked = (pthread_sigmask(SIG_BLOCK, &block_all, &saved_set) == 0);
        a32_jit_fork_prepare();
        pid = fork_reconciled();
        if (masked) {
            (void)pthread_sigmask(SIG_SETMASK, &saved_set, NULL);
        }
        return pid;
    }

    /* A32-R3-4: quiesce JIT execution/mutation before the snapshot so the
     * child inherits no leases of vanished threads and no partially
     * mutated cache state. Signals stay blocked until both sides have
     * reconciled, so no handler runs inside the PROT_NONE window.
     * Lock order throughout fork/truncate/futex is JIT -> VM -> vmfile
     * (never inverted): this path holds the JIT lock across fork(),
     * takes the VM lock only briefly via a32_vm_sync_ctx, and consults
     * file/EOF state through lock-free atomics; truncate takes only
     * the vmfile lock and futex takes none. */
    sigfillset(&block_all);
    masked = (pthread_sigmask(SIG_BLOCK, &block_all, &saved_set) == 0);
    a32_jit_fork_prepare();

    a32_vm_sync_ctx(ctx);
    if (ctx->region_count == 0U) {
        pid_t pid = fork_reconciled();
        if (masked) {
            (void)pthread_sigmask(SIG_SETMASK, &saved_set, NULL);
        }
        return pid;
    }

    fork_subregion_t *r_list   = (fork_subregion_t *)calloc(ctx->region_count, sizeof(fork_subregion_t));
    fork_cluster_t   *clusters = (fork_cluster_t *)calloc(ctx->region_count, sizeof(fork_cluster_t));
    if (!r_list || !clusters) {
        free(clusters);
        free(r_list);
        pid_t pid = fork_reconciled();
        if (masked) {
            (void)pthread_sigmask(SIG_SETMASK, &saved_set, NULL);
        }
        return pid;
    }

    size_t   r_count     = 0U;
    size_t   total_bytes = 0U;
    uint32_t sp_low      = ctx->stack_base;
    for (size_t i = 0U; i < ctx->region_count; i++) {
        const a32_region_t *r = &ctx->regions[i];
        if (r->end <= r->start || (r->prot & (PROT_READ | PROT_WRITE | PROT_EXEC)) == 0U) {
            continue;
        }
        uint32_t s_start = r->start;
        uint32_t s_end   = r->end;
        if (s_start == ctx->stack_base && s_end == ctx->stack_top) {
            uint32_t sp_min = ctx->regs[13];
            if (child_sp >= ctx->stack_base && child_sp < sp_min) {
                sp_min = child_sp;
            }
            if (sp_min >= ctx->stack_base && sp_min < ctx->stack_top) {
                sp_low = (sp_min > ctx->stack_base + 65536U)
                         ? ((sp_min - 65536U) & ~4095U)
                         : ctx->stack_base;
                s_start = sp_low;
            }
        }
        r_list[r_count].start   = s_start;
        r_list[r_count].end     = s_end;
        r_list[r_count].prot    = r->prot;
        r_list[r_count].buf_off = total_bytes;
        total_bytes += (size_t)(s_end - s_start);
        r_count++;
    }

    if (r_count == 0U || total_bytes == 0U) {
        free(clusters);
        free(r_list);
        pid_t pid = fork_reconciled();
        if (masked) {
            (void)pthread_sigmask(SIG_SETMASK, &saved_set, NULL);
        }
        return pid;
    }

    size_t c_count = 0U;
    for (size_t i = 0U; i < r_count; i++) {
        if (c_count > 0U && r_list[i].start == clusters[c_count - 1U].end) {
            clusters[c_count - 1U].end = r_list[i].end;
            clusters[c_count - 1U].count++;
        } else {
            clusters[c_count].start     = r_list[i].start;
            clusters[c_count].end       = r_list[i].end;
            clusters[c_count].first_idx = i;
            clusters[c_count].count     = 1U;
            c_count++;
        }
    }

    uint8_t *snap_buf = (uint8_t *)mmap(NULL, total_bytes, PROT_READ | PROT_WRITE,
                                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (snap_buf == MAP_FAILED) {
        free(clusters);
        free(r_list);
        pid_t pid = fork_reconciled();
        if (masked) {
            (void)pthread_sigmask(SIG_SETMASK, &saved_set, NULL);
        }
        return pid;
    }

    for (size_t i = 0U; i < r_count; i++) {
        uint32_t rs = r_list[i].start;
        uint32_t re = r_list[i].end;
        int hprot = a32_host_prot(ctx, r_list[i].prot);
        /* A32-R4-4: file mappings keep their VMA permissions even when
         * truncation retired pages past EOF (A32_PROT_EOF_CUT). Copy
         * page by page and skip cut pages: touching them here would
         * fault while signals are blocked. */
        for (uint32_t p = rs; p < re; p += 0x1000U) {
            uint32_t chunk = (re - p > 0x1000U) ? 0x1000U : (re - p);
            void *src;
            if (a32_vm32_page_eof_cut(p)) {
                continue;
            }
            src = a32_guest_ptr(ctx, p);
            if ((hprot & PROT_READ) == 0) {
                (void)mprotect(src, chunk, hprot | PROT_READ);
            }
            memcpy(snap_buf + r_list[i].buf_off + (p - rs), src, chunk);
        }
    }

    for (size_t c = 0U; c < c_count; c++) {
        (void)mprotect(a32_guest_ptr(ctx, clusters[c].start),
                       clusters[c].end - clusters[c].start,
                       PROT_NONE);
    }
    if (sp_low > ctx->stack_base) {
        (void)mprotect(a32_guest_ptr(ctx, ctx->stack_base),
                       sp_low - ctx->stack_base, PROT_NONE);
    }

    pid_t pid = fork();
    int saved_errno = errno;
    if (pid == 0) {
        /* Child: drop locks held by vanished threads, then rebuild
         * writable working copies from the snapshot. EOF-cut pages
         * are neither copied (the snapshot holds no bytes for them)
         * nor widened: they stay PROT_NONE like the post-truncate
         * mapping they came from. */
        fork_child_reset();
        for (size_t c = 0U; c < c_count; c++) {
            (void)mprotect(a32_guest_ptr(ctx, clusters[c].start),
                           clusters[c].end - clusters[c].start,
                           PROT_READ | PROT_WRITE);
        }
        if (sp_low > ctx->stack_base) {
            (void)mprotect(a32_guest_ptr(ctx, ctx->stack_base),
                           sp_low - ctx->stack_base, PROT_READ | PROT_WRITE);
        }
        for (size_t i = 0U; i < r_count; i++) {
            uint32_t rs = r_list[i].start;
            uint32_t re = r_list[i].end;
            int hprot = a32_host_prot(ctx, r_list[i].prot);
            uint32_t p = rs;
            while (p < re) {
                /* One mprotect per contiguous run with identical
                 * treatment, so cut-free regions keep the previous
                 * single-call shape. */
                uint32_t q = p + 0x1000U;
                if (a32_vm32_page_eof_cut(p)) {
                    while (q < re && a32_vm32_page_eof_cut(q)) {
                        q += 0x1000U;
                    }
                    (void)mprotect(a32_guest_ptr(ctx, p), q - p,
                        PROT_NONE);
                } else {
                    while (q < re && !a32_vm32_page_eof_cut(q)) {
                        q += 0x1000U;
                    }
                    memcpy(a32_guest_ptr(ctx, p),
                           snap_buf + r_list[i].buf_off + (p - rs),
                           q - p);
                    if (hprot != (PROT_READ | PROT_WRITE)) {
                        (void)mprotect(a32_guest_ptr(ctx, p), q - p,
                            hprot);
                    }
                }
                p = q;
            }
        }
    } else {
        /* Parent, including the failed-fork path: restore guest
         * protections BEFORE reopening JIT execution (A32-R4-3), so no
         * resumed guest thread can run inside the PROT_NONE window and
         * fault on valid memory. Cut pages return to PROT_NONE rather
         * than the wider VMA permissions (A32-R4-4). */
        int restore_err = 0;
        if (sp_low > ctx->stack_base) {
            if (mprotect(a32_guest_ptr(ctx, ctx->stack_base),
                    sp_low - ctx->stack_base,
                    PROT_READ | PROT_WRITE) != 0 && restore_err == 0) {
                restore_err = errno;
            }
        }
        for (size_t i = 0U; i < r_count; i++) {
            uint32_t rs = r_list[i].start;
            uint32_t re = r_list[i].end;
            int hprot = a32_host_prot(ctx, r_list[i].prot);
            uint32_t p = rs;
            while (p < re) {
                uint32_t q = p + 0x1000U;
                int want;
                if (a32_vm32_page_eof_cut(p)) {
                    while (q < re && a32_vm32_page_eof_cut(q)) {
                        q += 0x1000U;
                    }
                    want = PROT_NONE;
                } else {
                    while (q < re && !a32_vm32_page_eof_cut(q)) {
                        q += 0x1000U;
                    }
                    want = hprot;
                }
                if (mprotect(a32_guest_ptr(ctx, p), q - p, want) != 0 &&
                    restore_err == 0) {
                    restore_err = errno;
                }
                p = q;
            }
        }
        /* The execution barrier lifts only after every protection is
         * back (checked above); a failed restore with a live child is
         * reported but cannot stop the reopening without deadlocking
         * every future JIT user. */
        a32_jit_fork_parent();
        if (restore_err != 0 && pid >= 0) {
            static const char msg[] =
                "a32rt: fork parent protection restore failed\n";
            (void)write(STDERR_FILENO, msg, sizeof(msg) - 1U);
        }
    }

    (void)munmap(snap_buf, total_bytes);
    free(clusters);
    free(r_list);
    if (masked) {
        (void)pthread_sigmask(SIG_SETMASK, &saved_set, NULL);
    }
    errno = saved_errno;
    return pid;
}

/* fork/vfork/clone dispatch moved here from os_sys_sig_time.c (A32-R3):
 * process forking shares this file's snapshot + JIT/VM coordination, and
 * the move keeps os_sys_sig_time.c under the 900-line limit. */
int a32_sys_handle_fork_clone(a32_ctx_t *ctx, uint32_t nr, uint32_t a0,
    uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4)
{
    if (nr == ARM_NR_fork || nr == ARM_NR_vfork) {
        pid_t pid = fork_with_guest_snapshot(ctx, 0U);
        if (pid < 0) {
            FAIL_ERR(errno);
        } else if (pid == 0) {
            ctx->host_tid = 0;
            ctx->host_tid = get_host_tid(ctx);
            ctx->is_thread_clone = false;
            ctx->clear_child_tid = 0U;
            ctx->robust_list_head = 0U;
            ctx->robust_list_len = 0U;
            ctx->pending_signals = 0ULL;
            a32_signals_set_thread_ctx(ctx);
            ctx->regs[0] = 0U;
        } else {
            ctx->regs[0] = (uint32_t)pid;
        }
        return 0;
    }
    if (nr != ARM_NR_clone) {
        FAIL_ERR(ENOSYS);
    }
    {
        uint32_t flags = a0;
        FAIL_EINVAL_IF(((flags & CLONE_SIGHAND) != 0U &&
                        (flags & CLONE_VM) == 0U) ||
                       ((flags & CLONE_THREAD) != 0U &&
                        (flags & CLONE_SIGHAND) == 0U));
        FAIL_EFAULT_IF(((flags & CLONE_PARENT_SETTID) != 0U &&
                        !a32_guest_range_ok(ctx, a2, 4U, PROT_WRITE)) ||
                       ((flags & CLONE_CHILD_SETTID) != 0U &&
                        !a32_guest_range_ok(ctx, a4, 4U, PROT_WRITE)));
        bool is_thread = ((flags & CLONE_VM) != 0U &&
                          (flags & CLONE_VFORK) == 0U);
        if (is_thread) {
            a32_ctx_t *child_ctx = (a32_ctx_t *)calloc(1, sizeof(a32_ctx_t));
            if (!child_ctx) {
                FAIL_ERR(ENOMEM);
            }
            a32_vm_sync_ctx(ctx);
            memcpy(child_ctx, ctx, sizeof(a32_ctx_t));
            child_ctx->regs[0]          = 0U;
            child_ctx->regs[13]         = a1 ? a1 : ctx->regs[13];
            child_ctx->clear_child_tid  = (flags & CLONE_CHILD_CLEARTID) ? a4 : 0U;
            child_ctx->robust_list_head = 0U;
            child_ctx->robust_list_len  = 0U;
            child_ctx->pending_signals  = 0ULL;
            child_ctx->altstack_sp      = 0U;
            child_ctx->altstack_size    = 0U;
            child_ctx->altstack_flags   = SS_DISABLE;
            child_ctx->ras_top          = 0U;
            memset(child_ctx->ras, 0, sizeof(child_ctx->ras));
            memset(child_ctx->ibtc, 0, sizeof(child_ctx->ibtc));
            if (flags & CLONE_SETTLS) {
                child_ctx->tls_tp = a3;
            }

            child_thread_args_t targs = {
                .child_ctx = child_ctx,
                .flags     = flags,
                .ptid_va   = a2,
                .ctid_va   = a4,
                .child_tid = 0,
                .started   = 0,
            };

            (void)a32_jit_init(child_ctx);

            pthread_t pt;
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

            int prc = pthread_create(&pt, &attr, guest_thread_start, &targs);
            pthread_attr_destroy(&attr);
            if (prc != 0) {
                free(child_ctx);
                FAIL_ERR(prc);
            }

            while (!__atomic_load_n(&targs.started, __ATOMIC_SEQ_CST)) {
                sched_yield();
            }

            ctx->regs[0] = (uint32_t)targs.child_tid;
            return 0;
        }

        /* Non-thread clone: process fork */
        pid_t pid = fork_with_guest_snapshot(ctx, a1);
        if (pid < 0) {
            FAIL_ERR(errno);
        } else if (pid == 0) {
            ctx->host_tid         = 0;
            ctx->host_tid         = get_host_tid(ctx);
            ctx->is_thread_clone  = false;
            ctx->robust_list_head = 0U;
            ctx->robust_list_len  = 0U;
            ctx->pending_signals  = 0ULL;
            a32_signals_set_thread_ctx(ctx);
            if (a1 != 0U) {
                ctx->regs[13] = a1;
            }
            if (flags & CLONE_SETTLS) {
                ctx->tls_tp = a3;
            }
            if ((flags & CLONE_PARENT_SETTID) &&
                a32_guest_range_ok(ctx, a2, 4U, PROT_WRITE)) {
                *(uint32_t *)a32_guest_ptr(ctx, a2) = (uint32_t)ctx->host_tid;
            }
            if ((flags & CLONE_CHILD_SETTID) &&
                a32_guest_range_ok(ctx, a4, 4U, PROT_WRITE)) {
                *(uint32_t *)a32_guest_ptr(ctx, a4) = (uint32_t)ctx->host_tid;
            }
            ctx->clear_child_tid = (flags & CLONE_CHILD_CLEARTID) ? a4 : 0U;
            ctx->regs[0] = 0U;
        } else {
            if ((flags & CLONE_PARENT_SETTID) &&
                a32_guest_range_ok(ctx, a2, 4U, PROT_WRITE)) {
                *(uint32_t *)a32_guest_ptr(ctx, a2) = (uint32_t)pid;
            }
            ctx->regs[0] = (uint32_t)pid;
        }
        return 0;
    }
}

void fill_arm_rusage32(arm_rusage32_t *dst, const struct rusage *src)
{
    memset(dst, 0, sizeof(*dst));
    dst->ru_utime.tv_sec  = (int32_t)src->ru_utime.tv_sec;
    dst->ru_utime.tv_usec = (int32_t)src->ru_utime.tv_usec;
    dst->ru_stime.tv_sec  = (int32_t)src->ru_stime.tv_sec;
    dst->ru_stime.tv_usec = (int32_t)src->ru_stime.tv_usec;
    dst->ru_maxrss   = (int32_t)src->ru_maxrss;
    dst->ru_ixrss    = (int32_t)src->ru_ixrss;
    dst->ru_idrss    = (int32_t)src->ru_idrss;
    dst->ru_isrss    = (int32_t)src->ru_isrss;
    dst->ru_minflt   = (int32_t)src->ru_minflt;
    dst->ru_majflt   = (int32_t)src->ru_majflt;
    dst->ru_nswap    = (int32_t)src->ru_nswap;
    dst->ru_inblock  = (int32_t)src->ru_inblock;
    dst->ru_oublock  = (int32_t)src->ru_oublock;
    dst->ru_msgsnd   = (int32_t)src->ru_msgsnd;
    dst->ru_msgrcv   = (int32_t)src->ru_msgrcv;
    dst->ru_nsignals = (int32_t)src->ru_nsignals;
    dst->ru_nvcsw    = (int32_t)src->ru_nvcsw;
    dst->ru_nivcsw   = (int32_t)src->ru_nivcsw;
}

void fill_arm_statfs64(arm_statfs64_t *dst, const struct statfs *src)
{
    memset(dst, 0, sizeof(*dst));
    dst->f_type      = (uint32_t)src->f_type;
    dst->f_bsize     = (uint32_t)src->f_bsize;
    dst->f_blocks    = (uint64_t)src->f_blocks;
    dst->f_bfree     = (uint64_t)src->f_bfree;
    dst->f_bavail    = (uint64_t)src->f_bavail;
    dst->f_files     = (uint64_t)src->f_files;
    dst->f_ffree     = (uint64_t)src->f_ffree;
#if defined(__APPLE__)
    dst->f_fsid.val[0] = (int32_t)src->f_fsid.val[0];
    dst->f_fsid.val[1] = (int32_t)src->f_fsid.val[1];
    dst->f_namelen   = 255U;
    dst->f_frsize    = (uint32_t)src->f_bsize;
#else
    dst->f_fsid.val[0] = (int32_t)src->f_fsid.__val[0];
    dst->f_fsid.val[1] = (int32_t)src->f_fsid.__val[1];
    dst->f_namelen   = (uint32_t)src->f_namelen;
    dst->f_frsize    = (uint32_t)src->f_frsize;
#endif
    dst->f_flags     = (uint32_t)src->f_flags;
}

bool is_arm32_elf_file(const char *path)
{
    if (access(path, X_OK) != 0) {
        return false;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    uint8_t hdr[20];
    ssize_t n = read(fd, hdr, sizeof(hdr));
    close(fd);
    if (n < 20) {
        return false;
    }
    uint16_t e_type = (uint16_t)hdr[16] | (uint16_t)((uint16_t)hdr[17] << 8);
    if (hdr[0] == 0x7f && hdr[1] == 'E' && hdr[2] == 'L' && hdr[3] == 'F' &&
        hdr[4] == 1 /* ELFCLASS32 */ && hdr[5] == 1 /* ELFDATA2LSB */ &&
        (e_type == 2U /* ET_EXEC */ || e_type == 3U /* ET_DYN */) &&
        hdr[18] == 40 && hdr[19] == 0 /* EM_ARM */) {
        return true;
    }
    return false;
}

bool guest_range_mapped(const a32_ctx_t *ctx, uint32_t va, uint32_t len, uint32_t req_prot)
{
    uint64_t end = (uint64_t)va + (uint64_t)len;
    uint64_t cur = (uint64_t)va;
    size_t lo = 0U;
    size_t hi = ctx->region_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2U;
        if ((uint64_t)ctx->regions[mid].end <= cur) {
            lo = mid + 1U;
        } else {
            hi = mid;
        }
    }
    for (size_t i = lo; i < ctx->region_count; i++) {
        const a32_region_t *r = &ctx->regions[i];
        if ((uint64_t)r->end <= cur) {
            continue;
        }
        if ((uint64_t)r->start > cur || (r->prot & req_prot) != req_prot) {
            return false;
        }
        if ((uint64_t)r->end >= end) {
            return true;
        }
        cur = (uint64_t)r->end;
    }
    return false;
}

bool a32_guest_range_ok(a32_ctx_t *ctx, uint32_t va, uint32_t len, uint32_t req_prot)
{
    if (!ctx || va < 0x1000U || len == 0U) {
        return false;
    }
    uint64_t end = (uint64_t)va + (uint64_t)len;
    uint64_t max_end = (req_prot == (uint32_t)PROT_READ) ? 0x100000000ULL : 0xFFFF0000ULL;
    if (end > max_end) {
        return false;
    }
    if (!guest_range_mapped(ctx, va, len, req_prot)) {
        a32_vm_sync_ctx(ctx);
        if (!guest_range_mapped(ctx, va, len, req_prot)) {
            return false;
        }
    }
    if (a32_vm32_arena_base() != 0U && !a32_vm32_range_prot_ok(va, len, req_prot)) {
        return false;
    }
    return true;
}

int translate_linux_clockid(uint32_t linux_clk, clockid_t *out_clk)
{
#if defined(__APPLE__)
    switch (linux_clk) {
    case 0U: case 5U: *out_clk = CLOCK_REALTIME; return 0;
    case 1U: case 6U: case 7U: *out_clk = CLOCK_MONOTONIC; return 0;
    case 2U: *out_clk = CLOCK_PROCESS_CPUTIME_ID; return 0;
    case 3U: *out_clk = CLOCK_THREAD_CPUTIME_ID; return 0;
    case 4U: *out_clk = CLOCK_MONOTONIC_RAW; return 0;
    default: errno = EINVAL; return -1;
    }
#else
    *out_clk = (clockid_t)linux_clk;
    return 0;
#endif
}

bool read_guest_timespec(a32_ctx_t *ctx, uint32_t va, bool time64, struct timespec *out)
{
    if (time64) {
        arm_timespec64_t ts64;
        if (!guest_read(ctx, &ts64, va, sizeof(ts64))) {
            return false;
        }
        out->tv_sec  = (time_t)ts64.tv_sec;
        out->tv_nsec = (long)ts64.tv_nsec;
    } else {
        arm_timespec32_t ts32;
        if (!guest_read(ctx, &ts32, va, sizeof(ts32))) {
            return false;
        }
        out->tv_sec  = (time_t)ts32.tv_sec;
        out->tv_nsec = (long)ts32.tv_nsec;
    }
    return true;
}

bool write_guest_timespec(a32_ctx_t *ctx, uint32_t va, bool time64, const struct timespec *in)
{
    if (time64) {
        arm_timespec64_t ts64 = { (int64_t)in->tv_sec, (int64_t)in->tv_nsec };
        return guest_write(ctx, va, &ts64, sizeof(ts64));
    }
    arm_timespec32_t ts32 = { (int32_t)in->tv_sec, (int32_t)in->tv_nsec };
    return guest_write(ctx, va, &ts32, sizeof(ts32));
}

#if defined(__linux__)
bool read_guest_itimerspec(a32_ctx_t *ctx, uint32_t va, bool time64, struct itimerspec *out)
{
    uint32_t step = time64 ? 16U : 8U;
    return read_guest_timespec(ctx, va, time64, &out->it_interval) &&
           read_guest_timespec(ctx, va + step, time64, &out->it_value);
}

bool write_guest_itimerspec(a32_ctx_t *ctx, uint32_t va, bool time64, const struct itimerspec *in)
{
    uint32_t step = time64 ? 16U : 8U;
    return write_guest_timespec(ctx, va, time64, &in->it_interval) &&
           write_guest_timespec(ctx, va + step, time64, &in->it_value);
}
#endif

bool read_guest_timeval(a32_ctx_t *ctx, uint32_t va, bool time64, struct timeval *out)
{
    if (time64) {
        arm_timeval64_t tv64;
        if (!guest_read(ctx, &tv64, va, sizeof(tv64))) {
            return false;
        }
        out->tv_sec  = (time_t)tv64.tv_sec;
        out->tv_usec = (suseconds_t)tv64.tv_usec;
    } else {
        arm_timeval32_t tv32;
        if (!guest_read(ctx, &tv32, va, sizeof(tv32))) {
            return false;
        }
        out->tv_sec  = (time_t)tv32.tv_sec;
        out->tv_usec = (suseconds_t)tv32.tv_usec;
    }
    return true;
}

bool write_guest_timeval(a32_ctx_t *ctx, uint32_t va, bool time64, const struct timeval *in)
{
    if (time64) {
        arm_timeval64_t tv64 = { (int64_t)in->tv_sec, (int64_t)in->tv_usec };
        return guest_write(ctx, va, &tv64, sizeof(tv64));
    }
    arm_timeval32_t tv32 = { (int32_t)in->tv_sec, (int32_t)in->tv_usec };
    return guest_write(ctx, va, &tv32, sizeof(tv32));
}

bool read_guest_itimerval(a32_ctx_t *ctx, uint32_t va, struct itimerval *out)
{
    return read_guest_timeval(ctx, va, false, &out->it_interval) &&
           read_guest_timeval(ctx, va + (uint32_t)sizeof(arm_timeval32_t), false, &out->it_value);
}

bool write_guest_itimerval(a32_ctx_t *ctx, uint32_t va, const struct itimerval *in)
{
    return write_guest_timeval(ctx, va, false, &in->it_interval) &&
           write_guest_timeval(ctx, va + (uint32_t)sizeof(arm_timeval32_t), false, &in->it_value);
}

int handle_fcntl_flock(a32_ctx_t *ctx, int fd, int cmd, uint32_t arg, bool is64)
{
    bool is_get = is64 ? (cmd == 12 /* F_GETLK64 */ || cmd == 36 /* F_OFD_GETLK */)
                       : (cmd == 5 /* F_GETLK */);
    uint32_t sz = is64 ? (uint32_t)sizeof(struct flock64_arm) : (uint32_t)sizeof(struct flock_arm32);
    if (!a32_guest_range_ok(ctx, arg, sz, is_get ? (PROT_READ | PROT_WRITE) : PROT_READ)) {
        return -EFAULT;
    }
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    int host_cmd;
    if (is64) {
        struct flock64_arm *gfl = (struct flock64_arm *)a32_guest_ptr(ctx, arg);
        fl.l_type   = gfl->l_type;
        fl.l_whence = gfl->l_whence;
        fl.l_start  = (off_t)gfl->l_start;
        fl.l_len    = (off_t)gfl->l_len;
        fl.l_pid    = (pid_t)gfl->l_pid;
        host_cmd    = (cmd == 12) ? F_GETLK : (cmd == 13 ? F_SETLK : (cmd == 14 ? F_SETLKW : cmd));
    } else {
        struct flock_arm32 *gfl = (struct flock_arm32 *)a32_guest_ptr(ctx, arg);
        fl.l_type   = gfl->l_type;
        fl.l_whence = gfl->l_whence;
        fl.l_start  = (off_t)gfl->l_start;
        fl.l_len    = (off_t)gfl->l_len;
        fl.l_pid    = (pid_t)gfl->l_pid;
        host_cmd    = cmd;
    }

    if (fcntl(fd, host_cmd, &fl) < 0) {
        return -errno;
    }

    if (is_get) {
        if (is64) {
            struct flock64_arm *gfl = (struct flock64_arm *)a32_guest_ptr(ctx, arg);
            gfl->l_type   = fl.l_type;
            gfl->l_whence = fl.l_whence;
            gfl->l_start  = (int64_t)fl.l_start;
            gfl->l_len    = (int64_t)fl.l_len;
            gfl->l_pid    = (int32_t)fl.l_pid;
        } else {
            struct flock_arm32 *gfl = (struct flock_arm32 *)a32_guest_ptr(ctx, arg);
            gfl->l_type   = fl.l_type;
            gfl->l_whence = fl.l_whence;
            gfl->l_start  = (int32_t)fl.l_start;
            gfl->l_len    = (int32_t)fl.l_len;
            gfl->l_pid    = (int32_t)fl.l_pid;
        }
    }
    return 0;
}

static void wake_robust_futex(a32_ctx_t *ctx, uint32_t node_va, int32_t futex_offset, uint32_t self_tid)
{
    uint32_t futex_va = node_va + (uint32_t)futex_offset;
    if (futex_va < 0x1000U || futex_va > 0xFFFF0000U - 4U ||
        (futex_va & 3U) != 0U ||
        !guest_range_mapped(ctx, futex_va, 4U, PROT_READ | PROT_WRITE)) {
        return;
    }
    uint32_t *futex_ptr = (uint32_t *)a32_guest_ptr(ctx, futex_va);
    uint32_t val = __atomic_load_n(futex_ptr, __ATOMIC_SEQ_CST);
    while ((val & 0x3FFFFFFFU) == self_tid) {
        uint32_t new_val = (val & 0x80000000U) | 0x40000000U /* FUTEX_OWNER_DIED */;
        if (__atomic_compare_exchange_n(futex_ptr, &val, new_val, false,
                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
#if defined(__linux__)
            syscall(SYS_futex, futex_ptr, 1 /* FUTEX_WAKE */, 1, NULL, NULL, 0);
#endif
            break;
        }
    }
}

void a32_cleanup_robust_list(a32_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }
    uint32_t head_va = ctx->robust_list_head;
    ctx->robust_list_head = 0U;
    if (head_va < 0x1000U || head_va > 0xFFFF0000U - 12U || (head_va & 3U) != 0U) {
        return;
    }
    a32_vm_sync_ctx(ctx);
    if (!guest_range_mapped(ctx, head_va, 12U, PROT_READ)) {
        return;
    }
    uint32_t self_tid = (uint32_t)get_host_tid(ctx);
    uint32_t *head_ptr = (uint32_t *)a32_guest_ptr(ctx, head_va);
    int32_t futex_offset = (int32_t)head_ptr[1];
    uint32_t curr = head_ptr[0];
    uint32_t pending = head_ptr[2];

    size_t limit = 1024U;
    while (curr >= 0x1000U && curr <= 0xFFFF0000U - 4U &&
           (curr & 3U) == 0U && curr != head_va && limit-- > 0U &&
           guest_range_mapped(ctx, curr, 4U, PROT_READ)) {
        uint32_t *node = (uint32_t *)a32_guest_ptr(ctx, curr);
        uint32_t next = node[0];
        if (curr != pending) {
            wake_robust_futex(ctx, curr, futex_offset, self_tid);
        }
        curr = next;
    }
    if (pending >= 0x1000U && pending <= 0xFFFF0000U - 4U &&
        (pending & 3U) == 0U && pending != head_va) {
        wake_robust_futex(ctx, pending, futex_offset, self_tid);
    }
}
