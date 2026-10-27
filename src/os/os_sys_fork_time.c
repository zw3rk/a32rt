/*
 * src/os/os_sys_fork_time.c - split-TU part 3/7 of src/syscalls.c (lines 1107-1609).
 * Compiled only via the src/syscalls.c wrapper; do not add to Makefile SRCS.
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

static void fill_arm_stat64(const a32_ctx_t *ctx, arm_stat64_t *dst, const struct stat *src)
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

static pid_t fork_with_guest_snapshot(a32_ctx_t *ctx, uint32_t child_sp)
{
    if (!ctx || !ctx->has_host_info) {
        return fork();
    }

    a32_vm_sync_ctx(ctx);
    if (ctx->region_count == 0U) {
        return fork();
    }

    fork_subregion_t *r_list   = (fork_subregion_t *)calloc(ctx->region_count, sizeof(fork_subregion_t));
    fork_cluster_t   *clusters = (fork_cluster_t *)calloc(ctx->region_count, sizeof(fork_cluster_t));
    if (!r_list || !clusters) {
        free(clusters);
        free(r_list);
        return fork();
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
        return fork();
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
        return fork();
    }

    for (size_t i = 0U; i < r_count; i++) {
        void    *src   = a32_guest_ptr(ctx, r_list[i].start);
        uint32_t s_len = r_list[i].end - r_list[i].start;
        int      hprot = a32_host_prot(ctx, r_list[i].prot);
        if ((hprot & PROT_READ) == 0) {
            (void)mprotect(src, s_len, hprot | PROT_READ);
        }
        memcpy(snap_buf + r_list[i].buf_off, src, s_len);
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

    if (sp_low > ctx->stack_base) {
        (void)mprotect(a32_guest_ptr(ctx, ctx->stack_base),
                       sp_low - ctx->stack_base, PROT_READ | PROT_WRITE);
    }
    if (pid == 0) {
        for (size_t c = 0U; c < c_count; c++) {
            (void)mprotect(a32_guest_ptr(ctx, clusters[c].start),
                           clusters[c].end - clusters[c].start,
                           PROT_READ | PROT_WRITE);
        }
        for (size_t i = 0U; i < r_count; i++) {
            memcpy(a32_guest_ptr(ctx, r_list[i].start),
                   snap_buf + r_list[i].buf_off,
                   r_list[i].end - r_list[i].start);
            int hprot = a32_host_prot(ctx, r_list[i].prot);
            if (hprot != (PROT_READ | PROT_WRITE)) {
                (void)mprotect(a32_guest_ptr(ctx, r_list[i].start),
                               r_list[i].end - r_list[i].start,
                               hprot);
            }
        }
    } else {
        for (size_t i = 0U; i < r_count; i++) {
            (void)mprotect(a32_guest_ptr(ctx, r_list[i].start),
                           r_list[i].end - r_list[i].start,
                           a32_host_prot(ctx, r_list[i].prot));
        }
    }

    (void)munmap(snap_buf, total_bytes);
    free(clusters);
    free(r_list);
    errno = saved_errno;
    return pid;
}

static void fill_arm_rusage32(arm_rusage32_t *dst, const struct rusage *src)
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

static void fill_arm_statfs64(arm_statfs64_t *dst, const struct statfs *src)
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

static bool is_arm32_elf_file(const char *path)
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

static bool guest_range_mapped(const a32_ctx_t *ctx, uint32_t va, uint32_t len, uint32_t req_prot)
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
    if (guest_range_mapped(ctx, va, len, req_prot)) {
        return true;
    }
    a32_vm_sync_ctx(ctx);
    return guest_range_mapped(ctx, va, len, req_prot);
}

static inline bool guest_read(a32_ctx_t *ctx, void *dst, uint32_t va, uint32_t len)
{
    if (!a32_guest_range_ok(ctx, va, len, PROT_READ)) {
        return false;
    }
    memcpy(dst, a32_guest_ptr(ctx, va), len);
    return true;
}

static inline bool guest_write(a32_ctx_t *ctx, uint32_t va, const void *src, uint32_t len)
{
    if (!a32_guest_range_ok(ctx, va, len, PROT_WRITE)) {
        return false;
    }
    memcpy(a32_guest_ptr(ctx, va), src, len);
    return true;
}

static int translate_linux_clockid(uint32_t linux_clk, clockid_t *out_clk)
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

static bool read_guest_timespec(a32_ctx_t *ctx, uint32_t va, bool time64, struct timespec *out)
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

static bool write_guest_timespec(a32_ctx_t *ctx, uint32_t va, bool time64, const struct timespec *in)
{
    if (time64) {
        arm_timespec64_t ts64 = { (int64_t)in->tv_sec, (int64_t)in->tv_nsec };
        return guest_write(ctx, va, &ts64, sizeof(ts64));
    }
    arm_timespec32_t ts32 = { (int32_t)in->tv_sec, (int32_t)in->tv_nsec };
    return guest_write(ctx, va, &ts32, sizeof(ts32));
}

#if defined(__linux__)
static bool read_guest_itimerspec(a32_ctx_t *ctx, uint32_t va, bool time64, struct itimerspec *out)
{
    uint32_t step = time64 ? 16U : 8U;
    return read_guest_timespec(ctx, va, time64, &out->it_interval) &&
           read_guest_timespec(ctx, va + step, time64, &out->it_value);
}

static bool write_guest_itimerspec(a32_ctx_t *ctx, uint32_t va, bool time64, const struct itimerspec *in)
{
    uint32_t step = time64 ? 16U : 8U;
    return write_guest_timespec(ctx, va, time64, &in->it_interval) &&
           write_guest_timespec(ctx, va + step, time64, &in->it_value);
}
#endif

static bool read_guest_timeval(a32_ctx_t *ctx, uint32_t va, bool time64, struct timeval *out)
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

static bool write_guest_timeval(a32_ctx_t *ctx, uint32_t va, bool time64, const struct timeval *in)
{
    if (time64) {
        arm_timeval64_t tv64 = { (int64_t)in->tv_sec, (int64_t)in->tv_usec };
        return guest_write(ctx, va, &tv64, sizeof(tv64));
    }
    arm_timeval32_t tv32 = { (int32_t)in->tv_sec, (int32_t)in->tv_usec };
    return guest_write(ctx, va, &tv32, sizeof(tv32));
}

static bool read_guest_itimerval(a32_ctx_t *ctx, uint32_t va, struct itimerval *out)
{
    return read_guest_timeval(ctx, va, false, &out->it_interval) &&
           read_guest_timeval(ctx, va + (uint32_t)sizeof(arm_timeval32_t), false, &out->it_value);
}

static bool write_guest_itimerval(a32_ctx_t *ctx, uint32_t va, const struct itimerval *in)
{
    return write_guest_timeval(ctx, va, false, &in->it_interval) &&
           write_guest_timeval(ctx, va + (uint32_t)sizeof(arm_timeval32_t), false, &in->it_value);
}

static int handle_fcntl_flock(a32_ctx_t *ctx, int fd, int cmd, uint32_t arg, bool is64)
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
