/*
 * src/syscalls.c — ARM EABI (r7, r0..r5) -> aarch64-linux syscall & struct shim
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#define _GNU_SOURCE
#include "a32rt.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

/* ARM EABI syscall numbers */
#define ARM_NR_restart_syscall   0U
#define ARM_NR_exit              1U
#define ARM_NR_fork              2U
#define ARM_NR_read              3U
#define ARM_NR_write             4U
#define ARM_NR_open              5U
#define ARM_NR_close             6U
#define ARM_NR_link              9U
#define ARM_NR_unlink            10U
#define ARM_NR_execve            11U
#define ARM_NR_chdir             12U
#define ARM_NR_chmod             15U
#define ARM_NR_lchown            16U
#define ARM_NR_lseek             19U
#define ARM_NR_getpid            20U
#define ARM_NR_access            33U
#define ARM_NR_kill              37U
#define ARM_NR_rename            38U
#define ARM_NR_mkdir             39U
#define ARM_NR_rmdir             40U
#define ARM_NR_dup               41U
#define ARM_NR_pipe              42U
#define ARM_NR_brk               45U
#define ARM_NR_ioctl             54U
#define ARM_NR_fcntl             55U
#define ARM_NR_setpgid           57U
#define ARM_NR_umask             60U
#define ARM_NR_dup2              63U
#define ARM_NR_getppid           64U
#define ARM_NR_getpgrp           65U
#define ARM_NR_setsid            66U
#define ARM_NR_getrusage         77U
#define ARM_NR_gettimeofday      78U
#define ARM_NR_symlink           83U
#define ARM_NR_readlink          85U
#define ARM_NR_munmap            91U
#define ARM_NR_fchmod            94U
#define ARM_NR_fchown            95U
#define ARM_NR_wait4             114U
#define ARM_NR_sysinfo           116U
#define ARM_NR_fsync             118U
#define ARM_NR_sigreturn         119U
#define ARM_NR_clone             120U
#define ARM_NR_uname             122U
#define ARM_NR_mprotect          125U
#define ARM_NR_getpgid           132U
#define ARM_NR_fchdir            133U
#define ARM_NR__llseek           140U
#define ARM_NR_getdents          141U
#define ARM_NR_readv             145U
#define ARM_NR_writev            146U
#define ARM_NR_getsid            147U
#define ARM_NR_fdatasync         148U
#define ARM_NR_sched_yield       158U
#define ARM_NR_nanosleep         162U
#define ARM_NR_poll              168U
#define ARM_NR_prctl             172U
#define ARM_NR_rt_sigreturn      173U
#define ARM_NR_rt_sigaction      174U
#define ARM_NR_rt_sigprocmask    175U
#define ARM_NR_pread64           180U
#define ARM_NR_pwrite64          181U
#define ARM_NR_chown             182U
#define ARM_NR_getcwd            183U
#define ARM_NR_sigaltstack       186U
#define ARM_NR_vfork             190U
#define ARM_NR_ugetrlimit        191U
#define ARM_NR_mmap2             192U
#define ARM_NR_truncate64        193U
#define ARM_NR_ftruncate64       194U
#define ARM_NR_stat64            195U
#define ARM_NR_lstat64           196U
#define ARM_NR_fstat64           197U
#define ARM_NR_getuid32          199U
#define ARM_NR_getgid32          200U
#define ARM_NR_geteuid32         201U
#define ARM_NR_getegid32         202U
#define ARM_NR_setreuid32        203U
#define ARM_NR_setregid32        204U
#define ARM_NR_getgroups32       205U
#define ARM_NR_setgroups32       206U
#define ARM_NR_fchown32          207U
#define ARM_NR_setresuid32       208U
#define ARM_NR_getresuid32       209U
#define ARM_NR_setresgid32       210U
#define ARM_NR_getresgid32       211U
#define ARM_NR_chown32           212U
#define ARM_NR_setuid32          213U
#define ARM_NR_setgid32          214U
#define ARM_NR_getdents64        217U
#define ARM_NR_madvise           220U
#define ARM_NR_fcntl64           221U
#define ARM_NR_gettid            224U
#define ARM_NR_tkill             238U
#define ARM_NR_sendfile64        239U
#define ARM_NR_futex             240U
#define ARM_NR_exit_group        248U
#define ARM_NR_epoll_create      250U
#define ARM_NR_epoll_ctl         251U
#define ARM_NR_epoll_wait        252U
#define ARM_NR_set_tid_address   256U
#define ARM_NR_clock_gettime32   263U
#define ARM_NR_clock_getres32    264U
#define ARM_NR_statfs64          266U
#define ARM_NR_fstatfs64         267U
#define ARM_NR_tgkill            268U
#define ARM_NR_waitid            280U
#define ARM_NR_socket            281U
#define ARM_NR_bind              282U
#define ARM_NR_connect           283U
#define ARM_NR_listen            284U
#define ARM_NR_accept            285U
#define ARM_NR_getsockname       286U
#define ARM_NR_getpeername       287U
#define ARM_NR_socketpair        288U
#define ARM_NR_send              289U
#define ARM_NR_sendto            290U
#define ARM_NR_recv              291U
#define ARM_NR_recvfrom          292U
#define ARM_NR_shutdown          293U
#define ARM_NR_setsockopt        294U
#define ARM_NR_getsockopt        295U
#define ARM_NR_sendmsg           296U
#define ARM_NR_recvmsg           297U
#define ARM_NR_openat            322U
#define ARM_NR_mkdirat           323U
#define ARM_NR_fstatat64         327U
#define ARM_NR_unlinkat          328U
#define ARM_NR_renameat          329U
#define ARM_NR_linkat            330U
#define ARM_NR_symlinkat         331U
#define ARM_NR_readlinkat        332U
#define ARM_NR_fchmodat          333U
#define ARM_NR_faccessat         334U
#define ARM_NR_fchownat          335U
#define ARM_NR_ppoll             336U
#define ARM_NR_set_robust_list   338U
#define ARM_NR_get_robust_list   339U
#define ARM_NR_epoll_pwait       346U
#define ARM_NR_utimensat         348U
#define ARM_NR_timerfd_create    350U
#define ARM_NR_eventfd           351U
#define ARM_NR_timerfd_settime   353U
#define ARM_NR_timerfd_gettime   354U
#define ARM_NR_eventfd2          356U
#define ARM_NR_epoll_create1     357U
#define ARM_NR_dup3              358U
#define ARM_NR_pipe2             359U
#define ARM_NR_accept4           366U
#define ARM_NR_prlimit64         369U
#define ARM_NR_renameat2         382U
#define ARM_NR_getrandom         384U
#define ARM_NR_statx             397U
#define ARM_NR_clock_gettime64   403U
#define ARM_NR_clock_getres64    406U
#define ARM_NR_clock_nanosleep64 407U
#define ARM_NR_utimensat_time64  412U
#define ARM_NR_ppoll_time64      414U
#define ARM_NR_futex_time64      422U
#define ARM_NR_faccessat2        439U

/* ARM private syscalls (0x0f0000+) */
#define ARM_NR_breakpoint        0x0F0001U
#define ARM_NR_cacheflush        0x0F0002U
#define ARM_NR_set_tls           0x0F0005U
#define ARM_NR_get_tls           0x0F0800U

#ifndef SYS_memfd_create
#define SYS_memfd_create 279
#endif

typedef struct arm_iovec32 {
    uint32_t iov_base;
    uint32_t iov_len;
} arm_iovec32_t;

typedef struct arm_msghdr32 {
    uint32_t msg_name;
    uint32_t msg_namelen;
    uint32_t msg_iov;
    uint32_t msg_iovlen;
    uint32_t msg_control;
    uint32_t msg_controllen;
    uint32_t msg_flags;
} arm_msghdr32_t;

typedef struct arm_timespec64 {
    int64_t tv_sec;
    int64_t tv_nsec;
} arm_timespec64_t;

typedef struct arm_timespec32 {
    int32_t tv_sec;
    int32_t tv_nsec;
} arm_timespec32_t;

typedef struct arm_stat64 {
    uint64_t st_dev;
    uint8_t  __pad0[4];
    uint32_t __st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint8_t  __pad3[4];
    int64_t  st_size;
    uint32_t st_blksize;
    uint64_t st_blocks;
    uint32_t st_atime_sec;
    uint32_t st_atime_nsec;
    uint32_t st_mtime_sec;
    uint32_t st_mtime_nsec;
    uint32_t st_ctime_sec;
    uint32_t st_ctime_nsec;
    uint64_t st_ino;
} arm_stat64_t;

typedef struct arm_statfs64 {
    uint32_t f_type;
    uint32_t f_bsize;
    uint64_t f_blocks;
    uint64_t f_bfree;
    uint64_t f_bavail;
    uint64_t f_files;
    uint64_t f_ffree;
    struct { int32_t val[2]; } f_fsid;
    uint32_t f_namelen;
    uint32_t f_frsize;
    uint32_t f_flags;
    uint32_t f_spare[4];
} arm_statfs64_t;

typedef struct arm_rusage32 {
    struct { int32_t tv_sec; int32_t tv_usec; } ru_utime;
    struct { int32_t tv_sec; int32_t tv_usec; } ru_stime;
    int32_t ru_maxrss;
    int32_t ru_ixrss;
    int32_t ru_idrss;
    int32_t ru_isrss;
    int32_t ru_minflt;
    int32_t ru_majflt;
    int32_t ru_nswap;
    int32_t ru_inblock;
    int32_t ru_oublock;
    int32_t ru_msgsnd;
    int32_t ru_msgrcv;
    int32_t ru_nsignals;
    int32_t ru_nvcsw;
    int32_t ru_nivcsw;
} arm_rusage32_t;

typedef struct arm_sysinfo32 {
    int32_t  uptime;
    uint32_t loads[3];
    uint32_t totalram;
    uint32_t freeram;
    uint32_t sharedram;
    uint32_t bufferram;
    uint32_t totalswap;
    uint32_t freeswap;
    uint16_t procs;
    uint16_t pad;
    uint32_t totalhigh;
    uint32_t freehigh;
    uint32_t mem_unit;
    char     _f[20 - 2 * sizeof(uint32_t) - sizeof(uint32_t)];
} arm_sysinfo32_t;

typedef struct arm_rlimit32 {
    uint32_t rlim_cur;
    uint32_t rlim_max;
} arm_rlimit32_t;

struct linux_dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[];
};

struct flock_arm32 {
    int16_t  l_type;
    int16_t  l_whence;
    int32_t  l_start;
    int32_t  l_len;
    int32_t  l_pid;
};

struct flock64_arm {
    int16_t  l_type;
    int16_t  l_whence;
    uint8_t  __pad[4];
    int64_t  l_start;
    int64_t  l_len;
    int32_t  l_pid;
};

#define MAX_DIR_COOKIES 4096U
static uint64_t g_dir_cookies[MAX_DIR_COOKIES];
static size_t   g_dir_cookie_count = 0U;

static int64_t map_dir_cookie(uint64_t host_off)
{
    if (host_off <= 0x7FFFFFFFULL) {
        return (int64_t)host_off;
    }
    for (size_t i = 0; i < g_dir_cookie_count; i++) {
        if (g_dir_cookies[i] == host_off) {
            return (int64_t)(i + 1);
        }
    }
    if (g_dir_cookie_count < MAX_DIR_COOKIES) {
        size_t idx = g_dir_cookie_count++;
        g_dir_cookies[idx] = host_off;
        return (int64_t)(idx + 1);
    }
    return (int64_t)(host_off & 0x7FFFFFFFULL);
}

static uint64_t unmap_dir_cookie(int64_t guest_off)
{
    if (guest_off > 0 && (size_t)guest_off <= g_dir_cookie_count) {
        return g_dir_cookies[guest_off - 1];
    }
    return (uint64_t)guest_off;
}

static uint32_t host_ret(ssize_t r)
{
    if (r < 0) {
        return (uint32_t)(-errno);
    }
    return (uint32_t)r;
}

static int translate_at_fd(uint32_t guest_dfd)
{
    if (guest_dfd == 0xFFFFFF9CU /* AT_FDCWD (-100) */) {
        return AT_FDCWD;
    }
    return (int)(int32_t)guest_dfd;
}

static const char *resolve_guest_path(const a32_ctx_t *ctx, const char *guest_path,
                                     char *buf, size_t buf_size)
{
    if (!guest_path) {
        return NULL;
    }
    if (ctx->sysroot && ctx->sysroot[0] != '\0' && guest_path[0] == '/') {
        if ((strncmp(guest_path, "/proc/", 6) == 0 ||
             strncmp(guest_path, "/sys/", 5) == 0 ||
             strncmp(guest_path, "/dev/", 5) == 0) &&
            access(guest_path, F_OK) == 0) {
            return guest_path;
        }
        int n = snprintf(buf, buf_size, "%s%s", ctx->sysroot, guest_path);
        if (n > 0 && (size_t)n < buf_size) {
            for (int depth = 0; depth < 8; depth++) {
                char link_target[PATH_MAX];
                ssize_t ln = readlink(buf, link_target, sizeof(link_target) - 1U);
                if (ln <= 0) {
                    break;
                }
                link_target[ln] = '\0';
                if (link_target[0] != '/') {
                    break;
                }
                int n2 = snprintf(buf, buf_size, "%s%s", ctx->sysroot, link_target);
                if (n2 <= 0 || (size_t)n2 >= buf_size) {
                    break;
                }
            }
            if (access(buf, F_OK) == 0) {
                return buf;
            }
            if (strncmp(guest_path, "/proc", 5) == 0 ||
                strncmp(guest_path, "/sys", 4) == 0 ||
                strncmp(guest_path, "/dev", 4) == 0) {
                return guest_path;
            }
            return buf;
        }
    }
    return guest_path;
}

static int create_anon_file(const char *name, const void *data, size_t len)
{
    int fd = (int)syscall(SYS_memfd_create, name, 0x0001U /* MFD_CLOEXEC */);
    if (fd < 0) {
        char tmpl[] = "/tmp/a32rt_proc_XXXXXX";
        fd = mkstemp(tmpl);
        if (fd >= 0) {
            unlink(tmpl);
        }
    }
    if (fd >= 0 && data && len > 0) {
        ssize_t written = write(fd, data, len);
        (void)written;
        lseek(fd, 0, SEEK_SET);
    }
    return fd;
}

static int create_proc_maps(const a32_ctx_t *ctx)
{
    char buf[16384];
    size_t off = 0;

    for (size_t i = 0; i < ctx->region_count && off + 256 < sizeof(buf); i++) {
        const a32_region_t *r = &ctx->regions[i];
        char prot_r = (r->prot & 0x4) ? 'r' : '-';
        char prot_w = (r->prot & 0x2) ? 'w' : '-';
        char prot_x = (r->prot & 0x1) ? 'x' : '-';
        char prot_s = (r->flags & 0x01) ? 's' : 'p';
        int n = snprintf(buf + off, sizeof(buf) - off,
                         "%08x-%08x %c%c%c%c 00000000 00:00 0 %s%s\n",
                         r->start, r->end, prot_r, prot_w, prot_x, prot_s,
                         (r->name[0] && r->name[0] != '[') ? "         " : " ",
                         r->name[0] ? r->name : "");
        if (n > 0) {
            off += (size_t)n;
        }
    }
    return create_anon_file("proc_maps", buf, off);
}

static int create_proc_cmdline(const a32_ctx_t *ctx)
{
    char buf[4096];
    size_t off = 0;
    if (ctx->guest_argv) {
        for (int i = 0; i < ctx->guest_argc && off + 256 < sizeof(buf); i++) {
            if (ctx->guest_argv[i]) {
                size_t len = strlen(ctx->guest_argv[i]) + 1;
                if (off + len < sizeof(buf)) {
                    memcpy(buf + off, ctx->guest_argv[i], len);
                    off += len;
                }
            }
        }
    }
    return create_anon_file("proc_cmdline", buf, off);
}

static int create_proc_auxv(const a32_ctx_t *ctx)
{
    return create_anon_file("proc_auxv", ctx->auxv_data, ctx->auxv_bytes);
}

static void fill_arm_stat64(arm_stat64_t *dst, const struct stat *src)
{
    memset(dst, 0, sizeof(*dst));
    dst->st_dev        = (uint64_t)src->st_dev;
    dst->__st_ino      = (uint32_t)src->st_ino;
    dst->st_mode       = (uint32_t)src->st_mode;
    dst->st_nlink      = (uint32_t)src->st_nlink;
    dst->st_uid        = (uint32_t)src->st_uid;
    dst->st_gid        = (uint32_t)src->st_gid;
    dst->st_rdev       = (uint64_t)src->st_rdev;
    dst->st_size       = (int64_t)src->st_size;
    dst->st_blksize    = (uint32_t)src->st_blksize;
    dst->st_blocks     = (uint64_t)src->st_blocks;
    dst->st_atime_sec  = (uint32_t)src->st_atim.tv_sec;
    dst->st_atime_nsec = (uint32_t)src->st_atim.tv_nsec;
    dst->st_mtime_sec  = (uint32_t)src->st_mtim.tv_sec;
    dst->st_mtime_nsec = (uint32_t)src->st_mtim.tv_nsec;
    dst->st_ctime_sec  = (uint32_t)src->st_ctim.tv_sec;
    dst->st_ctime_nsec = (uint32_t)src->st_ctim.tv_nsec;
    dst->st_ino        = (uint64_t)src->st_ino;
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
    dst->f_fsid.val[0] = (int32_t)src->f_fsid.__val[0];
    dst->f_fsid.val[1] = (int32_t)src->f_fsid.__val[1];
    dst->f_namelen   = (uint32_t)src->f_namelen;
    dst->f_frsize    = (uint32_t)src->f_frsize;
    dst->f_flags     = (uint32_t)src->f_flags;
}

static bool is_arm32_elf_file(const char *path)
{
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
    if (hdr[0] == 0x7f && hdr[1] == 'E' && hdr[2] == 'L' && hdr[3] == 'F' &&
        hdr[4] == 1 /* ELFCLASS32 */ && hdr[5] == 1 /* ELFDATA2LSB */ &&
        hdr[18] == 40 && hdr[19] == 0 /* EM_ARM */) {
        return true;
    }
    return false;
}

void a32_cleanup_robust_list(a32_ctx_t *ctx)
{
    if (!ctx || ctx->robust_list_head == 0U) {
        return;
    }
    uint32_t head_va = ctx->robust_list_head;
    uint32_t *head_ptr = (uint32_t *)a32_guest_ptr(ctx, head_va);
    int32_t futex_offset = (int32_t)head_ptr[1];
    uint32_t curr = head_ptr[0];

    size_t limit = 1024U;
    while (curr != 0U && curr != head_va && limit-- > 0U) {
        uint32_t futex_va = curr + (uint32_t)futex_offset;
        uint32_t *futex_ptr = (uint32_t *)a32_guest_ptr(ctx, futex_va);
        uint32_t val = *futex_ptr;
        if ((val & 0x0FFFFFFFU) == (uint32_t)ctx->host_tid) {
            *futex_ptr |= 0x40000000U /* FUTEX_OWNER_DIED */;
            syscall(SYS_futex, futex_ptr, 1 /* FUTEX_WAKE */, 1, NULL, NULL, 0);
        }
        uint32_t *node = (uint32_t *)a32_guest_ptr(ctx, curr);
        curr = node[0];
    }
    ctx->robust_list_head = 0U;
}

typedef struct child_thread_args {
    a32_ctx_t   *child_ctx;
    uint32_t     flags;
    volatile int started;
} child_thread_args_t;

static void *child_thread_fn(void *arg)
{
    child_thread_args_t *args = (child_thread_args_t *)arg;
    a32_ctx_t *child_ctx = args->child_ctx;
    uint32_t flags = args->flags;

    a32_signals_set_thread_ctx(child_ctx);
    child_ctx->host_tid = (int)syscall(SYS_gettid);
    child_ctx->is_thread_clone = true;

    if (flags & (0x01000000U /* CLONE_CHILD_SETTID */ | 0x00200000U /* CLONE_CHILD_CLEARTID */)) {
        if (child_ctx->clear_child_tid != 0U) {
            *(uint32_t *)a32_guest_ptr(child_ctx, child_ctx->clear_child_tid) =
                (uint32_t)child_ctx->host_tid;
        }
    }

    args->started = 1;

    a32_jit_run(child_ctx);

    a32_cleanup_robust_list(child_ctx);
    if (child_ctx->clear_child_tid != 0U) {
        uint32_t *ctid = (uint32_t *)a32_guest_ptr(child_ctx, child_ctx->clear_child_tid);
        *ctid = 0U;
        syscall(SYS_futex, ctid, 1 /* FUTEX_WAKE */, 1, NULL, NULL, 0);
    }
    free(child_ctx);
    return NULL;
}

static void print_stats_if_enabled(const a32_ctx_t *ctx)
{
    if (!ctx->stats_enabled) {
        return;
    }
    fprintf(stderr,
            "[a32rt stats] blocks_compiled=%llu blocks_executed=%llu ras_hits=%llu "
            "flags_elided=%llu syscalls=%llu icache_flushes=%llu window=0x%llx\n",
            (unsigned long long)ctx->blocks_compiled,
            (unsigned long long)ctx->blocks_executed,
            (unsigned long long)ctx->ras_hits,
            (unsigned long long)ctx->flags_elided,
            (unsigned long long)ctx->syscalls_executed,
            (unsigned long long)ctx->icache_flushes,
            (unsigned long long)ctx->guest_base);
}

int a32_handle_syscall(a32_ctx_t *ctx)
{
    uint32_t nr = ctx->regs[7];
    uint32_t a0 = ctx->regs[0];
    uint32_t a1 = ctx->regs[1];
    uint32_t a2 = ctx->regs[2];
    uint32_t a3 = ctx->regs[3];
    uint32_t a4 = ctx->regs[4];
    uint32_t a5 = ctx->regs[5];

    if (ctx->trace_enabled) {
        fprintf(stderr, "[a32rt] syscall nr=%u (0x%x) args=(0x%x, 0x%x, 0x%x, 0x%x)\n",
                nr, nr, a0, a1, a2, a3);
    }

    switch (nr) {
    case ARM_NR_restart_syscall:
        ctx->regs[0] = 0U;
        break;

    case ARM_NR_exit:
        if (ctx->is_thread_clone) {
            a32_cleanup_robust_list(ctx);
            if (ctx->clear_child_tid != 0U) {
                uint32_t *ctid = (uint32_t *)a32_guest_ptr(ctx, ctx->clear_child_tid);
                *ctid = 0U;
                syscall(SYS_futex, ctid, 1 /* FUTEX_WAKE */, 1, NULL, NULL, 0);
            }
            pthread_exit(NULL);
        }
        print_stats_if_enabled(ctx);
        syscall(SYS_exit_group, (int)a0);
        return 1;

    case ARM_NR_exit_group:
        print_stats_if_enabled(ctx);
        syscall(SYS_exit_group, (int)a0);
        return 1;

    case ARM_NR_read:
        ctx->regs[0] = host_ret(read((int)a0, a32_guest_ptr(ctx, a1), (size_t)a2));
        break;

    case ARM_NR_write:
        ctx->regs[0] = host_ret(write((int)a0, a32_guest_ptr(ctx, a1), (size_t)a2));
        break;

    case ARM_NR_readv:
    case ARM_NR_writev: {
        if (a2 > 1024U) {
            ctx->regs[0] = (uint32_t)(-EINVAL);
            break;
        }
        struct iovec host_iov[64];
        struct iovec *iov_ptr = host_iov;
        if (a2 > 64U) {
            iov_ptr = (struct iovec *)calloc(a2, sizeof(struct iovec));
            if (!iov_ptr) {
                ctx->regs[0] = (uint32_t)(-ENOMEM);
                break;
            }
        }
        const arm_iovec32_t *guest_iov = (const arm_iovec32_t *)a32_guest_ptr(ctx, a1);
        for (uint32_t i = 0U; i < a2; i++) {
            iov_ptr[i].iov_base = a32_guest_ptr(ctx, guest_iov[i].iov_base);
            iov_ptr[i].iov_len  = (size_t)guest_iov[i].iov_len;
        }
        ssize_t r = (nr == ARM_NR_writev) ? writev((int)a0, iov_ptr, (int)a2)
                                          : readv((int)a0, iov_ptr, (int)a2);
        ctx->regs[0] = host_ret(r);
        if (iov_ptr != host_iov) {
            free(iov_ptr);
        }
        break;
    }

    case ARM_NR_open: {
        const char *guest_path = (const char *)a32_guest_ptr(ctx, a0);
        if (strcmp(guest_path, "/proc/self/maps") == 0) {
            ctx->regs[0] = (uint32_t)create_proc_maps(ctx);
            break;
        }
        if (strcmp(guest_path, "/proc/self/cmdline") == 0) {
            ctx->regs[0] = (uint32_t)create_proc_cmdline(ctx);
            break;
        }
        if (strcmp(guest_path, "/proc/self/auxv") == 0) {
            ctx->regs[0] = (uint32_t)create_proc_auxv(ctx);
            break;
        }
        char resolved[PATH_MAX];
        const char *path = resolve_guest_path(ctx, guest_path, resolved, sizeof(resolved));
        ctx->regs[0] = host_ret(openat(AT_FDCWD, path, (int)a1, (mode_t)a2));
        break;
    }

    case ARM_NR_openat: {
        const char *guest_path = (const char *)a32_guest_ptr(ctx, a1);
        if (strcmp(guest_path, "/proc/self/maps") == 0) {
            ctx->regs[0] = (uint32_t)create_proc_maps(ctx);
            break;
        }
        if (strcmp(guest_path, "/proc/self/cmdline") == 0) {
            ctx->regs[0] = (uint32_t)create_proc_cmdline(ctx);
            break;
        }
        if (strcmp(guest_path, "/proc/self/auxv") == 0) {
            ctx->regs[0] = (uint32_t)create_proc_auxv(ctx);
            break;
        }
        char resolved[PATH_MAX];
        const char *path = resolve_guest_path(ctx, guest_path, resolved, sizeof(resolved));
        ctx->regs[0] = host_ret(openat(translate_at_fd(a0), path, (int)a2, (mode_t)a3));
        break;
    }

    case ARM_NR_close:
        ctx->regs[0] = host_ret(close((int)a0));
        break;

    case ARM_NR_lseek: {
        off_t off = (off_t)(int32_t)a1;
        if (a2 == SEEK_SET && off > 0 && (size_t)off <= g_dir_cookie_count) {
            off = (off_t)unmap_dir_cookie((int64_t)off);
        }
        ctx->regs[0] = host_ret((ssize_t)lseek((int)a0, off, (int)a2));
        break;
    }

    case ARM_NR__llseek: {
        off_t off = (off_t)(((uint64_t)a1 << 32) | (uint64_t)a2);
        if (a4 == SEEK_SET && off > 0 && (size_t)off <= g_dir_cookie_count) {
            off = (off_t)unmap_dir_cookie((int64_t)off);
        }
        off_t res = lseek((int)a0, off, (int)a4);
        if (res < 0) {
            ctx->regs[0] = (uint32_t)(-errno);
        } else {
            uint64_t ures = (uint64_t)res;
            memcpy(a32_guest_ptr(ctx, a3), &ures, sizeof(ures));
            ctx->regs[0] = 0U;
        }
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

    case ARM_NR_ioctl: {
        void *arg_ptr = (a2 != 0U) ? a32_guest_ptr(ctx, a2) : NULL;
        ctx->regs[0] = host_ret(ioctl((int)a0, (unsigned long)a1, arg_ptr));
        break;
    }

    case ARM_NR_fcntl:
    case ARM_NR_fcntl64: {
        int cmd = (int)a1;
        if (cmd == 5 /* F_GETLK */ || cmd == 6 /* F_SETLK */ || cmd == 7 /* F_SETLKW */) {
            struct flock_arm32 *gfl = (struct flock_arm32 *)a32_guest_ptr(ctx, a2);
            struct flock fl;
            memset(&fl, 0, sizeof(fl));
            fl.l_type   = gfl->l_type;
            fl.l_whence = gfl->l_whence;
            fl.l_start  = (off_t)gfl->l_start;
            fl.l_len    = (off_t)gfl->l_len;
            fl.l_pid    = (pid_t)gfl->l_pid;
            int r = fcntl((int)a0, cmd, &fl);
            if (r < 0) {
                ctx->regs[0] = (uint32_t)(-errno);
            } else {
                if (cmd == 5) {
                    gfl->l_type   = fl.l_type;
                    gfl->l_whence = fl.l_whence;
                    gfl->l_start  = (int32_t)fl.l_start;
                    gfl->l_len    = (int32_t)fl.l_len;
                    gfl->l_pid    = (int32_t)fl.l_pid;
                }
                ctx->regs[0] = 0U;
            }
            break;
        }
        if (cmd == 12 /* F_GETLK64 */ || cmd == 13 /* F_SETLK64 */ || cmd == 14 /* F_SETLKW64 */) {
            struct flock64_arm *gfl = (struct flock64_arm *)a32_guest_ptr(ctx, a2);
            struct flock fl;
            memset(&fl, 0, sizeof(fl));
            fl.l_type   = gfl->l_type;
            fl.l_whence = gfl->l_whence;
            fl.l_start  = (off_t)gfl->l_start;
            fl.l_len    = (off_t)gfl->l_len;
            fl.l_pid    = (pid_t)gfl->l_pid;
            int host_cmd = (cmd == 12) ? F_GETLK : (cmd == 13 ? F_SETLK : F_SETLKW);
            int r = fcntl((int)a0, host_cmd, &fl);
            if (r < 0) {
                ctx->regs[0] = (uint32_t)(-errno);
            } else {
                if (cmd == 12) {
                    gfl->l_type   = fl.l_type;
                    gfl->l_whence = fl.l_whence;
                    gfl->l_start  = (int64_t)fl.l_start;
                    gfl->l_len    = (int64_t)fl.l_len;
                    gfl->l_pid    = (int32_t)fl.l_pid;
                }
                ctx->regs[0] = 0U;
            }
            break;
        }
        ctx->regs[0] = host_ret(fcntl((int)a0, cmd, (unsigned long)a2));
        break;
    }

    case ARM_NR_uname: {
        struct utsname u;
        if (uname(&u) != 0) {
            ctx->regs[0] = (uint32_t)(-errno);
        } else {
            snprintf(u.machine, sizeof(u.machine), "armv8l");
            memcpy(a32_guest_ptr(ctx, a0), &u, sizeof(u));
            ctx->regs[0] = 0U;
        }
        break;
    }

    case ARM_NR_fstat64: {
        struct stat st;
        if (fstat((int)a0, &st) != 0) {
            ctx->regs[0] = (uint32_t)(-errno);
        } else {
            fill_arm_stat64((arm_stat64_t *)a32_guest_ptr(ctx, a1), &st);
            ctx->regs[0] = 0U;
        }
        break;
    }

    case ARM_NR_stat64:
    case ARM_NR_lstat64: {
        struct stat st;
        const char *guest_path = (const char *)a32_guest_ptr(ctx, a0);
        char resolved[PATH_MAX];
        const char *path = resolve_guest_path(ctx, guest_path, resolved, sizeof(resolved));
        int flags = (nr == ARM_NR_lstat64) ? AT_SYMLINK_NOFOLLOW : 0;
        if (fstatat(AT_FDCWD, path, &st, flags) != 0) {
            ctx->regs[0] = (uint32_t)(-errno);
        } else {
            fill_arm_stat64((arm_stat64_t *)a32_guest_ptr(ctx, a1), &st);
            ctx->regs[0] = 0U;
        }
        break;
    }

    case ARM_NR_fstatat64: {
        struct stat st;
        const char *guest_path = (const char *)a32_guest_ptr(ctx, a1);
        char resolved[PATH_MAX];
        const char *path = resolve_guest_path(ctx, guest_path, resolved, sizeof(resolved));
        if (fstatat(translate_at_fd(a0), path, &st, (int)a3) != 0) {
            ctx->regs[0] = (uint32_t)(-errno);
        } else {
            fill_arm_stat64((arm_stat64_t *)a32_guest_ptr(ctx, a2), &st);
            ctx->regs[0] = 0U;
        }
        break;
    }

    case ARM_NR_access: {
        const char *guest_path = (const char *)a32_guest_ptr(ctx, a0);
        char resolved[PATH_MAX];
        const char *path = resolve_guest_path(ctx, guest_path, resolved, sizeof(resolved));
        ctx->regs[0] = host_ret(faccessat(AT_FDCWD, path, (int)a1, 0));
        break;
    }

    case ARM_NR_faccessat:
    case ARM_NR_faccessat2: {
        const char *guest_path = (const char *)a32_guest_ptr(ctx, a1);
        char resolved[PATH_MAX];
        const char *path = resolve_guest_path(ctx, guest_path, resolved, sizeof(resolved));
        ctx->regs[0] = host_ret(faccessat(translate_at_fd(a0), path, (int)a2, (int)a3));
        break;
    }

    case ARM_NR_getcwd: {
        char *buf = (char *)a32_guest_ptr(ctx, a0);
        long r = syscall(SYS_getcwd, buf, (size_t)a1);
        ctx->regs[0] = (r < 0) ? (uint32_t)(-errno) : (uint32_t)r;
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
        ctx->regs[0] = (uint32_t)syscall(SYS_gettid);
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

    case ARM_NR_setresuid32:
        ctx->regs[0] = host_ret(setresuid((uid_t)a0, (uid_t)a1, (uid_t)a2));
        break;

    case ARM_NR_getresuid32: {
        uid_t ruid = 0, euid = 0, suid = 0;
        if (getresuid(&ruid, &euid, &suid) != 0) {
            ctx->regs[0] = (uint32_t)(-errno);
        } else {
            if (a0) *(uint32_t *)a32_guest_ptr(ctx, a0) = (uint32_t)ruid;
            if (a1) *(uint32_t *)a32_guest_ptr(ctx, a1) = (uint32_t)euid;
            if (a2) *(uint32_t *)a32_guest_ptr(ctx, a2) = (uint32_t)suid;
            ctx->regs[0] = 0U;
        }
        break;
    }

    case ARM_NR_setresgid32:
        ctx->regs[0] = host_ret(setresgid((gid_t)a0, (gid_t)a1, (gid_t)a2));
        break;

    case ARM_NR_getresgid32: {
        gid_t rgid = 0, egid = 0, sgid = 0;
        if (getresgid(&rgid, &egid, &sgid) != 0) {
            ctx->regs[0] = (uint32_t)(-errno);
        } else {
            if (a0) *(uint32_t *)a32_guest_ptr(ctx, a0) = (uint32_t)rgid;
            if (a1) *(uint32_t *)a32_guest_ptr(ctx, a1) = (uint32_t)egid;
            if (a2) *(uint32_t *)a32_guest_ptr(ctx, a2) = (uint32_t)sgid;
            ctx->regs[0] = 0U;
        }
        break;
    }

    case ARM_NR_fchown32:
        ctx->regs[0] = host_ret(fchown((int)a0, (uid_t)a1, (gid_t)a2));
        break;

    case ARM_NR_chown32: {
        const char *guest_path = (const char *)a32_guest_ptr(ctx, a0);
        char resolved[PATH_MAX];
        const char *path = resolve_guest_path(ctx, guest_path, resolved, sizeof(resolved));
        ctx->regs[0] = host_ret(fchownat(AT_FDCWD, path, (uid_t)a1, (gid_t)a2, 0));
        break;
    }

    case ARM_NR_getgroups32:
        ctx->regs[0] = host_ret(getgroups((int)a0, (gid_t *)a32_guest_ptr(ctx, a1)));
        break;

    case ARM_NR_setgroups32:
        ctx->regs[0] = host_ret(setgroups((size_t)a0, (const gid_t *)a32_guest_ptr(ctx, a1)));
        break;

    case ARM_NR_set_tid_address:
        ctx->clear_child_tid = a0;
        ctx->regs[0] = (uint32_t)syscall(SYS_set_tid_address, a32_guest_ptr(ctx, a0));
        break;

    case ARM_NR_rt_sigaction:
        ctx->regs[0] = (uint32_t)a32_sys_rt_sigaction(ctx, (int)a0, a1, a2, a3);
        break;

    case ARM_NR_rt_sigprocmask:
        ctx->regs[0] = (uint32_t)a32_sys_rt_sigprocmask(ctx, (int)a0, a1, a2, a3);
        break;

    case ARM_NR_sigaltstack:
        ctx->regs[0] = (uint32_t)a32_sys_sigaltstack(ctx, a0, a1);
        break;

    case ARM_NR_sigreturn:
    case ARM_NR_rt_sigreturn:
        (void)a32_sys_rt_sigreturn(ctx);
        break;

    case ARM_NR_ugetrlimit: {
        struct rlimit rl;
        if (getrlimit((int)a0, &rl) != 0) {
            ctx->regs[0] = (uint32_t)(-errno);
        } else {
            arm_rlimit32_t rl32;
            rl32.rlim_cur = (rl.rlim_cur > 0xFFFFFFFFUL) ? 0xFFFFFFFFU : (uint32_t)rl.rlim_cur;
            rl32.rlim_max = (rl.rlim_max > 0xFFFFFFFFUL) ? 0xFFFFFFFFU : (uint32_t)rl.rlim_max;
            memcpy(a32_guest_ptr(ctx, a1), &rl32, sizeof(rl32));
            ctx->regs[0] = 0U;
        }
        break;
    }

    case ARM_NR_prlimit64:
        ctx->regs[0] = host_ret(syscall(SYS_prlimit64, (pid_t)a0, (int)a1,
                                        a2 ? a32_guest_ptr(ctx, a2) : NULL,
                                        a3 ? a32_guest_ptr(ctx, a3) : NULL));
        break;

    case ARM_NR_getrusage: {
        struct rusage ru;
        if (getrusage((int)a0, &ru) != 0) {
            ctx->regs[0] = (uint32_t)(-errno);
        } else {
            fill_arm_rusage32((arm_rusage32_t *)a32_guest_ptr(ctx, a1), &ru);
            ctx->regs[0] = 0U;
        }
        break;
    }

    case ARM_NR_sysinfo: {
        struct sysinfo si;
        if (sysinfo(&si) != 0) {
            ctx->regs[0] = (uint32_t)(-errno);
        } else {
            arm_sysinfo32_t si32;
            memset(&si32, 0, sizeof(si32));
            si32.uptime    = (int32_t)si.uptime;
            si32.loads[0]  = (uint32_t)si.loads[0];
            si32.loads[1]  = (uint32_t)si.loads[1];
            si32.loads[2]  = (uint32_t)si.loads[2];
            si32.totalram  = (uint32_t)si.totalram;
            si32.freeram   = (uint32_t)si.freeram;
            si32.sharedram = (uint32_t)si.sharedram;
            si32.bufferram = (uint32_t)si.bufferram;
            si32.totalswap = (uint32_t)si.totalswap;
            si32.freeswap  = (uint32_t)si.freeswap;
            si32.procs     = (uint16_t)si.procs;
            si32.totalhigh = (uint32_t)si.totalhigh;
            si32.freehigh  = (uint32_t)si.freehigh;
            si32.mem_unit  = (uint32_t)si.mem_unit;
            memcpy(a32_guest_ptr(ctx, a0), &si32, sizeof(si32));
            ctx->regs[0] = 0U;
        }
        break;
    }

    case ARM_NR_statfs64: {
        const char *guest_path = (const char *)a32_guest_ptr(ctx, a0);
        char resolved[PATH_MAX];
        const char *path = resolve_guest_path(ctx, guest_path, resolved, sizeof(resolved));
        struct statfs sfs;
        if (statfs(path, &sfs) != 0) {
            ctx->regs[0] = (uint32_t)(-errno);
        } else {
            fill_arm_statfs64((arm_statfs64_t *)a32_guest_ptr(ctx, a2), &sfs);
            ctx->regs[0] = 0U;
        }
        break;
    }

    case ARM_NR_fstatfs64: {
        struct statfs sfs;
        if (fstatfs((int)a0, &sfs) != 0) {
            ctx->regs[0] = (uint32_t)(-errno);
        } else {
            fill_arm_statfs64((arm_statfs64_t *)a32_guest_ptr(ctx, a2), &sfs);
            ctx->regs[0] = 0U;
        }
        break;
    }

    case ARM_NR_getrandom:
        ctx->regs[0] = host_ret(getrandom(a32_guest_ptr(ctx, a0), (size_t)a1, (unsigned int)a2));
        break;

    case ARM_NR_clock_gettime32: {
        struct timespec ts;
        if (clock_gettime((clockid_t)a0, &ts) != 0) {
            ctx->regs[0] = (uint32_t)(-errno);
        } else {
            arm_timespec32_t ts32;
            ts32.tv_sec  = (int32_t)ts.tv_sec;
            ts32.tv_nsec = (int32_t)ts.tv_nsec;
            memcpy(a32_guest_ptr(ctx, a1), &ts32, sizeof(ts32));
            ctx->regs[0] = 0U;
        }
        break;
    }

    case ARM_NR_clock_gettime64: {
        struct timespec ts;
        if (clock_gettime((clockid_t)a0, &ts) != 0) {
            ctx->regs[0] = (uint32_t)(-errno);
        } else {
            arm_timespec64_t ts64;
            ts64.tv_sec  = (int64_t)ts.tv_sec;
            ts64.tv_nsec = (int64_t)ts.tv_nsec;
            memcpy(a32_guest_ptr(ctx, a1), &ts64, sizeof(ts64));
            ctx->regs[0] = 0U;
        }
        break;
    }

    case ARM_NR_clock_getres32: {
        struct timespec ts;
        if (clock_getres((clockid_t)a0, &ts) != 0) {
            ctx->regs[0] = (uint32_t)(-errno);
        } else {
            if (a1 != 0U) {
                arm_timespec32_t ts32;
                ts32.tv_sec  = (int32_t)ts.tv_sec;
                ts32.tv_nsec = (int32_t)ts.tv_nsec;
                memcpy(a32_guest_ptr(ctx, a1), &ts32, sizeof(ts32));
            }
            ctx->regs[0] = 0U;
        }
        break;
    }

    case ARM_NR_clock_getres64: {
        struct timespec ts;
        if (clock_getres((clockid_t)a0, &ts) != 0) {
            ctx->regs[0] = (uint32_t)(-errno);
        } else {
            if (a1 != 0U) {
                arm_timespec64_t ts64;
                ts64.tv_sec  = (int64_t)ts.tv_sec;
                ts64.tv_nsec = (int64_t)ts.tv_nsec;
                memcpy(a32_guest_ptr(ctx, a1), &ts64, sizeof(ts64));
            }
            ctx->regs[0] = 0U;
        }
        break;
    }

    case ARM_NR_gettimeofday: {
        struct timeval tv;
        if (gettimeofday(&tv, NULL) != 0) {
            ctx->regs[0] = (uint32_t)(-errno);
        } else {
            if (a0 != 0U) {
                uint32_t *p = (uint32_t *)a32_guest_ptr(ctx, a0);
                p[0] = (uint32_t)tv.tv_sec;
                p[1] = (uint32_t)tv.tv_usec;
            }
            ctx->regs[0] = 0U;
        }
        break;
    }

    case ARM_NR_getdents64: {
        long r = syscall(SYS_getdents64, (int)a0, a32_guest_ptr(ctx, a1), (size_t)a2);
        if (r < 0) {
            ctx->regs[0] = (uint32_t)(-errno);
            break;
        }
        if (!(ctx->has_host_info && (ctx->host_info.caps & A32H_CAP_DIROFF32))) {
            uint8_t *p = (uint8_t *)a32_guest_ptr(ctx, a1);
            size_t pos = 0;
            while (pos < (size_t)r) {
                struct linux_dirent64 *d = (struct linux_dirent64 *)(p + pos);
                if (d->d_reclen == 0) {
                    break;
                }
                d->d_off = map_dir_cookie((uint64_t)d->d_off);
                pos += d->d_reclen;
            }
        }
        ctx->regs[0] = (uint32_t)r;
        break;
    }

    case ARM_NR_getdents: {
        size_t host_buf_size = a2 * 2;
        if (host_buf_size < 1024) {
            host_buf_size = 1024;
        }
        uint8_t *host_buf = (uint8_t *)malloc(host_buf_size);
        if (!host_buf) {
            ctx->regs[0] = (uint32_t)(-ENOMEM);
            break;
        }
        long nread = syscall(SYS_getdents64, (int)a0, host_buf, host_buf_size);
        if (nread < 0) {
            free(host_buf);
            ctx->regs[0] = (uint32_t)(-errno);
            break;
        }
        uint8_t *out = (uint8_t *)a32_guest_ptr(ctx, a1);
        size_t in_pos = 0;
        size_t out_pos = 0;
        while (in_pos < (size_t)nread) {
            struct linux_dirent64 *d64 = (struct linux_dirent64 *)(host_buf + in_pos);
            if (d64->d_reclen == 0) {
                break;
            }
            size_t namelen = strlen(d64->d_name);
            size_t reclen32 = (10 + namelen + 1 + 1 + 3) & ~3U;
            if (out_pos + reclen32 > a2) {
                if (out_pos == 0) {
                    free(host_buf);
                    ctx->regs[0] = (uint32_t)(-EINVAL);
                    break;
                }
                lseek((int)a0, (off_t)d64->d_off, SEEK_SET);
                break;
            }
            uint32_t *d32_words = (uint32_t *)(out + out_pos);
            d32_words[0] = (uint32_t)d64->d_ino;
            d32_words[1] = (uint32_t)(map_dir_cookie((uint64_t)d64->d_off) & 0x7FFFFFFFU);
            *(uint16_t *)(out + out_pos + 8) = (uint16_t)reclen32;
            memcpy(out + out_pos + 10, d64->d_name, namelen + 1);
            out[out_pos + reclen32 - 1] = d64->d_type;
            out_pos += reclen32;
            in_pos += d64->d_reclen;
        }
        free(host_buf);
        ctx->regs[0] = (uint32_t)out_pos;
        break;
    }

    case ARM_NR_fork:
    case ARM_NR_vfork: {
        pid_t pid = fork();
        if (pid < 0) {
            ctx->regs[0] = (uint32_t)(-errno);
        } else if (pid == 0) {
            ctx->host_tid = (int)syscall(SYS_gettid);
            ctx->is_thread_clone = false;
            a32_signals_set_thread_ctx(ctx);
            ctx->regs[0] = 0U;
        } else {
            ctx->regs[0] = (uint32_t)pid;
        }
        break;
    }

    case ARM_NR_clone: {
        uint32_t flags = a0;
        if ((flags & 0x00000100U /* CLONE_VM */) != 0U && (flags & 0x00004000U /* CLONE_VFORK */) == 0U) {
            a32_ctx_t *child_ctx = (a32_ctx_t *)calloc(1, sizeof(a32_ctx_t));
            if (!child_ctx) {
                ctx->regs[0] = (uint32_t)(-ENOMEM);
                break;
            }
            memcpy(child_ctx, ctx, sizeof(a32_ctx_t));
            child_ctx->regs[0]  = 0U;
            child_ctx->regs[13] = a1 ? a1 : ctx->regs[13];
            if (flags & 0x00080000U /* CLONE_SETTLS */) {
                child_ctx->tls_tp = a3;
            }
            if (flags & 0x00200000U /* CLONE_CHILD_CLEARTID */) {
                child_ctx->clear_child_tid = a4;
            }

            child_thread_args_t targs;
            targs.child_ctx = child_ctx;
            targs.flags     = flags;
            targs.started   = 0;

            pthread_t pt;
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

            int prc = pthread_create(&pt, &attr, child_thread_fn, &targs);
            pthread_attr_destroy(&attr);
            if (prc != 0) {
                free(child_ctx);
                ctx->regs[0] = (uint32_t)(-prc);
                break;
            }

            while (!targs.started) {
                sched_yield();
            }

            if (flags & 0x00100000U /* CLONE_PARENT_SETTID */) {
                if (a2 != 0U) {
                    *(uint32_t *)a32_guest_ptr(ctx, a2) = (uint32_t)child_ctx->host_tid;
                }
            }

            ctx->regs[0] = (uint32_t)child_ctx->host_tid;
            break;
        }

        /* Non-thread clone: process fork */
        pid_t pid = fork();
        if (pid < 0) {
            ctx->regs[0] = (uint32_t)(-errno);
        } else if (pid == 0) {
            ctx->host_tid = (int)syscall(SYS_gettid);
            ctx->is_thread_clone = false;
            a32_signals_set_thread_ctx(ctx);
            if (a1 != 0U) {
                ctx->regs[13] = a1;
            }
            ctx->regs[0] = 0U;
        } else {
            ctx->regs[0] = (uint32_t)pid;
        }
        break;
    }

    case ARM_NR_execve: {
        const char *guest_path = (const char *)a32_guest_ptr(ctx, a0);
        char resolved[PATH_MAX];
        const char *path = resolve_guest_path(ctx, guest_path, resolved, sizeof(resolved));

        char *new_argv[512];
        int argc_count = 0;
        if (a1 != 0U) {
            uint32_t *g_argv = (uint32_t *)a32_guest_ptr(ctx, a1);
            while (g_argv[argc_count] != 0U && argc_count < 500) {
                new_argv[argc_count] = (char *)a32_guest_ptr(ctx, g_argv[argc_count]);
                argc_count++;
            }
        }
        new_argv[argc_count] = NULL;

        char *new_envp[512];
        int envc_count = 0;
        if (a2 != 0U) {
            uint32_t *g_envp = (uint32_t *)a32_guest_ptr(ctx, a2);
            while (g_envp[envc_count] != 0U && envc_count < 510) {
                new_envp[envc_count] = (char *)a32_guest_ptr(ctx, g_envp[envc_count]);
                envc_count++;
            }
        }
        new_envp[envc_count] = NULL;

        if (is_arm32_elf_file(path)) {
            char self_exe[PATH_MAX];
            ssize_t sn = readlink("/proc/self/exe", self_exe, sizeof(self_exe) - 1);
            if (sn <= 0) {
                snprintf(self_exe, sizeof(self_exe), "/proc/self/exe");
            } else {
                self_exe[sn] = '\0';
            }

            char win_buf[32];
            char *re_argv[518];
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
            re_argv[rpos++] = (char *)"--";
            re_argv[rpos++] = (char *)path;
            for (int k = 0; k < argc_count && rpos < 517; k++) {
                re_argv[rpos++] = new_argv[k];
            }
            re_argv[rpos] = NULL;
            execve(self_exe, re_argv, new_envp[0] ? new_envp : environ);
        } else {
            execve(path, new_argv, new_envp[0] ? new_envp : environ);
        }
        ctx->regs[0] = (uint32_t)(-errno);
        break;
    }

    case ARM_NR_wait4: {
        int status = 0;
        struct rusage ru;
        pid_t r = wait4((pid_t)a0, &status, (int)a2, a3 ? &ru : NULL);
        if (r < 0) {
            ctx->regs[0] = (uint32_t)(-errno);
        } else {
            if (a1 != 0U) {
                *(int32_t *)a32_guest_ptr(ctx, a1) = status;
            }
            if (a3 != 0U) {
                fill_arm_rusage32((arm_rusage32_t *)a32_guest_ptr(ctx, a3), &ru);
            }
            ctx->regs[0] = (uint32_t)r;
        }
        break;
    }

    case ARM_NR_waitid: {
        siginfo_t si;
        memset(&si, 0, sizeof(si));
        int r = waitid((idtype_t)a0, (id_t)a1, &si, (int)a3);
        if (r != 0) {
            ctx->regs[0] = (uint32_t)(-errno);
        } else {
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
            ctx->regs[0] = 0U;
        }
        break;
    }

    case ARM_NR_futex: {
        void *uaddr = a32_guest_ptr(ctx, a0);
        int op = (int)a1;
        uint32_t val = a2;
        int basic_op = op & 0x7F;
        struct timespec host_ts;
        struct timespec *ts_ptr = NULL;

        if (a3 != 0U && (basic_op == 0 /* FUTEX_WAIT */ || basic_op == 11 /* FUTEX_WAIT_BITSET */)) {
            const arm_timespec32_t *ts32 = (const arm_timespec32_t *)a32_guest_ptr(ctx, a3);
            host_ts.tv_sec  = (time_t)ts32->tv_sec;
            host_ts.tv_nsec = (long)ts32->tv_nsec;
            ts_ptr = &host_ts;
        }

        void *uaddr2 = (a4 != 0U) ? a32_guest_ptr(ctx, a4) : NULL;
        long r = syscall(SYS_futex, uaddr, op, val, ts_ptr, uaddr2, a5);
        ctx->regs[0] = host_ret(r);
        break;
    }

    case ARM_NR_futex_time64: {
        void *uaddr = a32_guest_ptr(ctx, a0);
        int op = (int)a1;
        uint32_t val = a2;
        int basic_op = op & 0x7F;
        struct timespec host_ts;
        struct timespec *ts_ptr = NULL;

        if (a3 != 0U && (basic_op == 0 /* FUTEX_WAIT */ || basic_op == 11 /* FUTEX_WAIT_BITSET */)) {
            const arm_timespec64_t *ts64 = (const arm_timespec64_t *)a32_guest_ptr(ctx, a3);
            host_ts.tv_sec  = (time_t)ts64->tv_sec;
            host_ts.tv_nsec = (long)ts64->tv_nsec;
            ts_ptr = &host_ts;
        }

        void *uaddr2 = (a4 != 0U) ? a32_guest_ptr(ctx, a4) : NULL;
        long r = syscall(SYS_futex, uaddr, op, val, ts_ptr, uaddr2, a5);
        ctx->regs[0] = host_ret(r);
        break;
    }

    case ARM_NR_set_robust_list:
        ctx->robust_list_head = a0;
        ctx->robust_list_len  = a1;
        ctx->regs[0] = 0U;
        break;

    case ARM_NR_get_robust_list:
        if (a0 == 0U) {
            if (a1 != 0U) {
                uint32_t h = ctx->robust_list_head;
                memcpy(a32_guest_ptr(ctx, a1), &h, sizeof(h));
            }
            if (a2 != 0U) {
                uint32_t l = ctx->robust_list_len;
                memcpy(a32_guest_ptr(ctx, a2), &l, sizeof(l));
            }
            ctx->regs[0] = 0U;
        } else {
            ctx->regs[0] = (uint32_t)(-EPERM);
        }
        break;

    case ARM_NR_pread64: {
        off_t pos = (off_t)(((uint64_t)a5 << 32) | (uint64_t)a4);
        ctx->regs[0] = host_ret(pread((int)a0, a32_guest_ptr(ctx, a1), (size_t)a2, pos));
        break;
    }

    case ARM_NR_pwrite64: {
        off_t pos = (off_t)(((uint64_t)a5 << 32) | (uint64_t)a4);
        ctx->regs[0] = host_ret(pwrite((int)a0, a32_guest_ptr(ctx, a1), (size_t)a2, pos));
        break;
    }

    case ARM_NR_truncate64: {
        const char *guest_path = (const char *)a32_guest_ptr(ctx, a0);
        char resolved[PATH_MAX];
        const char *path = resolve_guest_path(ctx, guest_path, resolved, sizeof(resolved));
        off_t len = (off_t)(((uint64_t)a3 << 32) | (uint64_t)a2);
        ctx->regs[0] = host_ret(truncate(path, len));
        break;
    }

    case ARM_NR_ftruncate64: {
        off_t len = (off_t)(((uint64_t)a3 << 32) | (uint64_t)a2);
        ctx->regs[0] = host_ret(ftruncate((int)a0, len));
        break;
    }

    case ARM_NR_sendfile64: {
        off_t *off_ptr = a2 ? (off_t *)a32_guest_ptr(ctx, a2) : NULL;
        ctx->regs[0] = host_ret(sendfile((int)a0, (int)a1, off_ptr, (size_t)a3));
        break;
    }

    case ARM_NR_statx: {
        const char *guest_path = (const char *)a32_guest_ptr(ctx, a1);
        char resolved[PATH_MAX];
        const char *path = resolve_guest_path(ctx, guest_path, resolved, sizeof(resolved));
        ctx->regs[0] = host_ret(syscall(SYS_statx, translate_at_fd(a0), path,
                                        (int)a2, (unsigned int)a3,
                                        a32_guest_ptr(ctx, a4)));
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
        ctx->regs[0] = host_ret(accept((int)a0,
                                      a1 ? a32_guest_ptr(ctx, a1) : NULL,
                                      a2 ? (socklen_t *)a32_guest_ptr(ctx, a2) : NULL));
        break;

    case ARM_NR_accept4:
        ctx->regs[0] = host_ret(accept4((int)a0,
                                       a1 ? a32_guest_ptr(ctx, a1) : NULL,
                                       a2 ? (socklen_t *)a32_guest_ptr(ctx, a2) : NULL,
                                       (int)a3));
        break;

    case ARM_NR_getsockname:
        ctx->regs[0] = host_ret(getsockname((int)a0, a32_guest_ptr(ctx, a1),
                                           (socklen_t *)a32_guest_ptr(ctx, a2)));
        break;

    case ARM_NR_getpeername:
        ctx->regs[0] = host_ret(getpeername((int)a0, a32_guest_ptr(ctx, a1),
                                           (socklen_t *)a32_guest_ptr(ctx, a2)));
        break;

    case ARM_NR_socketpair:
        ctx->regs[0] = host_ret(socketpair((int)a0, (int)a1, (int)a2,
                                          (int *)a32_guest_ptr(ctx, a3)));
        break;

    case ARM_NR_send:
        ctx->regs[0] = host_ret(send((int)a0, a32_guest_ptr(ctx, a1), (size_t)a2, (int)a3));
        break;

    case ARM_NR_sendto:
        ctx->regs[0] = host_ret(sendto((int)a0, a32_guest_ptr(ctx, a1), (size_t)a2, (int)a3,
                                      a4 ? a32_guest_ptr(ctx, a4) : NULL, (socklen_t)a5));
        break;

    case ARM_NR_recv:
        ctx->regs[0] = host_ret(recv((int)a0, a32_guest_ptr(ctx, a1), (size_t)a2, (int)a3));
        break;

    case ARM_NR_recvfrom:
        ctx->regs[0] = host_ret(recvfrom((int)a0, a32_guest_ptr(ctx, a1), (size_t)a2, (int)a3,
                                        a4 ? a32_guest_ptr(ctx, a4) : NULL,
                                        a5 ? (socklen_t *)a32_guest_ptr(ctx, a5) : NULL));
        break;

    case ARM_NR_shutdown:
        ctx->regs[0] = host_ret(shutdown((int)a0, (int)a1));
        break;

    case ARM_NR_setsockopt:
        ctx->regs[0] = host_ret(setsockopt((int)a0, (int)a1, (int)a2,
                                          a3 ? a32_guest_ptr(ctx, a3) : NULL, (socklen_t)a4));
        break;

    case ARM_NR_getsockopt:
        ctx->regs[0] = host_ret(getsockopt((int)a0, (int)a1, (int)a2,
                                          a3 ? a32_guest_ptr(ctx, a3) : NULL,
                                          a4 ? (socklen_t *)a32_guest_ptr(ctx, a4) : NULL));
        break;

    case ARM_NR_sendmsg:
    case ARM_NR_recvmsg: {
        arm_msghdr32_t *gm = (arm_msghdr32_t *)a32_guest_ptr(ctx, a1);
        struct msghdr hm;
        memset(&hm, 0, sizeof(hm));
        hm.msg_name    = (gm->msg_name != 0U) ? a32_guest_ptr(ctx, gm->msg_name) : NULL;
        hm.msg_namelen = (socklen_t)gm->msg_namelen;
        hm.msg_flags   = (int)gm->msg_flags;

        _Alignas(8) uint8_t cbuf64[1024];
        memset(cbuf64, 0, sizeof(cbuf64));
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
                    if (len32 < 12U || off32 + len32 > gm->msg_controllen) {
                        break;
                    }
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
                if (cap64 > sizeof(cbuf64)) {
                    cap64 = sizeof(cbuf64);
                }
                hm.msg_control    = cbuf64;
                hm.msg_controllen = cap64;
            }
        }

        struct iovec host_iov[16];
        struct iovec *hiov = host_iov;
        if (gm->msg_iovlen > 16U) {
            hiov = (struct iovec *)calloc(gm->msg_iovlen, sizeof(struct iovec));
            if (!hiov) {
                ctx->regs[0] = (uint32_t)(-ENOMEM);
                break;
            }
        }
        const arm_iovec32_t *giov = (const arm_iovec32_t *)a32_guest_ptr(ctx, gm->msg_iov);
        for (uint32_t i = 0U; i < gm->msg_iovlen; i++) {
            hiov[i].iov_base = a32_guest_ptr(ctx, giov[i].iov_base);
            hiov[i].iov_len  = (size_t)giov[i].iov_len;
        }
        hm.msg_iov    = hiov;
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
                while (off64 + sizeof(struct cmsghdr) <= hm.msg_controllen) {
                    const struct cmsghdr *cm64 = (const struct cmsghdr *)(const void *)(cbuf64 + off64);
                    if (cm64->cmsg_len < sizeof(struct cmsghdr) ||
                        off64 + cm64->cmsg_len > hm.msg_controllen) {
                        break;
                    }
                    size_t payload_len = cm64->cmsg_len - sizeof(struct cmsghdr);
                    uint32_t len32 = 12U + (uint32_t)payload_len;
                    uint32_t space32 = (len32 + 3U) & ~3U;
                    if (off32 + len32 <= gm->msg_controllen) {
                        int32_t level32 = (int32_t)cm64->cmsg_level;
                        int32_t type32  = (int32_t)cm64->cmsg_type;
                        memcpy(gctl_out + off32, &len32, 4);
                        memcpy(gctl_out + off32 + 4U, &level32, 4);
                        memcpy(gctl_out + off32 + 8U, &type32, 4);
                        memcpy(gctl_out + off32 + 12U, CMSG_DATA(cm64), payload_len);
                        off32 += (off32 + space32 <= gm->msg_controllen) ? space32 : len32;
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

    /* Polling, epoll, eventfd, timerfd */
    case ARM_NR_poll:
        ctx->regs[0] = host_ret(poll((struct pollfd *)a32_guest_ptr(ctx, a0),
                                    (nfds_t)a1, (int)a2));
        break;

    case ARM_NR_ppoll: {
        struct timespec host_ts;
        struct timespec *ts_ptr = NULL;
        if (a2 != 0U) {
            const arm_timespec32_t *ts32 = (const arm_timespec32_t *)a32_guest_ptr(ctx, a2);
            host_ts.tv_sec  = (time_t)ts32->tv_sec;
            host_ts.tv_nsec = (long)ts32->tv_nsec;
            ts_ptr = &host_ts;
        }
        sigset_t *sig_ptr = a3 ? (sigset_t *)a32_guest_ptr(ctx, a3) : NULL;
        ctx->regs[0] = host_ret(ppoll((struct pollfd *)a32_guest_ptr(ctx, a0),
                                     (nfds_t)a1, ts_ptr, sig_ptr));
        break;
    }

    case ARM_NR_ppoll_time64: {
        struct timespec host_ts;
        struct timespec *ts_ptr = NULL;
        if (a2 != 0U) {
            const arm_timespec64_t *ts64 = (const arm_timespec64_t *)a32_guest_ptr(ctx, a2);
            host_ts.tv_sec  = (time_t)ts64->tv_sec;
            host_ts.tv_nsec = (long)ts64->tv_nsec;
            ts_ptr = &host_ts;
        }
        sigset_t *sig_ptr = a3 ? (sigset_t *)a32_guest_ptr(ctx, a3) : NULL;
        ctx->regs[0] = host_ret(ppoll((struct pollfd *)a32_guest_ptr(ctx, a0),
                                     (nfds_t)a1, ts_ptr, sig_ptr));
        break;
    }

    case ARM_NR_epoll_create:
        ctx->regs[0] = host_ret(epoll_create((int)a0));
        break;

    case ARM_NR_epoll_create1:
        ctx->regs[0] = host_ret(epoll_create1((int)a0));
        break;

    case ARM_NR_epoll_ctl:
        ctx->regs[0] = host_ret(epoll_ctl((int)a0, (int)a1, (int)a2,
                                         (struct epoll_event *)a32_guest_ptr(ctx, a3)));
        break;

    case ARM_NR_epoll_wait:
        ctx->regs[0] = host_ret(epoll_wait((int)a0, (struct epoll_event *)a32_guest_ptr(ctx, a1),
                                          (int)a2, (int)a3));
        break;

    case ARM_NR_epoll_pwait:
        ctx->regs[0] = host_ret(epoll_pwait((int)a0, (struct epoll_event *)a32_guest_ptr(ctx, a1),
                                           (int)a2, (int)a3,
                                           a4 ? (const sigset_t *)a32_guest_ptr(ctx, a4) : NULL));
        break;

    case ARM_NR_eventfd:
        ctx->regs[0] = host_ret(eventfd((unsigned int)a0, 0));
        break;

    case ARM_NR_eventfd2:
        ctx->regs[0] = host_ret(eventfd((unsigned int)a0, (int)a1));
        break;

    case ARM_NR_timerfd_create:
        ctx->regs[0] = host_ret(timerfd_create((int)a0, (int)a1));
        break;

    case ARM_NR_timerfd_settime:
        ctx->regs[0] = host_ret(timerfd_settime((int)a0, (int)a1,
                                               (const struct itimerspec *)a32_guest_ptr(ctx, a2),
                                               a3 ? (struct itimerspec *)a32_guest_ptr(ctx, a3) : NULL));
        break;

    case ARM_NR_timerfd_gettime:
        ctx->regs[0] = host_ret(timerfd_gettime((int)a0, (struct itimerspec *)a32_guest_ptr(ctx, a1)));
        break;

    /* Filesystem & descriptor management */
    case ARM_NR_dup:
        ctx->regs[0] = host_ret(dup((int)a0));
        break;

    case ARM_NR_dup2:
        ctx->regs[0] = host_ret(dup2((int)a0, (int)a1));
        break;

    case ARM_NR_dup3:
        ctx->regs[0] = host_ret(dup3((int)a0, (int)a1, (int)a2));
        break;

    case ARM_NR_pipe: {
        int fds[2];
        int r = pipe(fds);
        if (r < 0) {
            ctx->regs[0] = (uint32_t)(-errno);
        } else {
            memcpy(a32_guest_ptr(ctx, a0), fds, sizeof(fds));
            ctx->regs[0] = 0U;
        }
        break;
    }

    case ARM_NR_pipe2: {
        int fds[2];
        int r = pipe2(fds, (int)a1);
        if (r < 0) {
            ctx->regs[0] = (uint32_t)(-errno);
        } else {
            memcpy(a32_guest_ptr(ctx, a0), fds, sizeof(fds));
            ctx->regs[0] = 0U;
        }
        break;
    }

    case ARM_NR_chdir: {
        const char *guest_path = (const char *)a32_guest_ptr(ctx, a0);
        char resolved[PATH_MAX];
        const char *path = resolve_guest_path(ctx, guest_path, resolved, sizeof(resolved));
        ctx->regs[0] = host_ret(chdir(path));
        break;
    }

    case ARM_NR_fchdir:
        ctx->regs[0] = host_ret(fchdir((int)a0));
        break;

    case ARM_NR_mkdir: {
        const char *guest_path = (const char *)a32_guest_ptr(ctx, a0);
        char resolved[PATH_MAX];
        const char *path = resolve_guest_path(ctx, guest_path, resolved, sizeof(resolved));
        ctx->regs[0] = host_ret(mkdir(path, (mode_t)a1));
        break;
    }

    case ARM_NR_mkdirat: {
        const char *guest_path = (const char *)a32_guest_ptr(ctx, a1);
        char resolved[PATH_MAX];
        const char *path = resolve_guest_path(ctx, guest_path, resolved, sizeof(resolved));
        ctx->regs[0] = host_ret(mkdirat(translate_at_fd(a0), path, (mode_t)a2));
        break;
    }

    case ARM_NR_rmdir: {
        const char *guest_path = (const char *)a32_guest_ptr(ctx, a0);
        char resolved[PATH_MAX];
        const char *path = resolve_guest_path(ctx, guest_path, resolved, sizeof(resolved));
        ctx->regs[0] = host_ret(rmdir(path));
        break;
    }

    case ARM_NR_unlink: {
        const char *guest_path = (const char *)a32_guest_ptr(ctx, a0);
        char resolved[PATH_MAX];
        const char *path = resolve_guest_path(ctx, guest_path, resolved, sizeof(resolved));
        ctx->regs[0] = host_ret(unlink(path));
        break;
    }

    case ARM_NR_unlinkat: {
        const char *guest_path = (const char *)a32_guest_ptr(ctx, a1);
        char resolved[PATH_MAX];
        const char *path = resolve_guest_path(ctx, guest_path, resolved, sizeof(resolved));
        ctx->regs[0] = host_ret(unlinkat(translate_at_fd(a0), path, (int)a2));
        break;
    }

    case ARM_NR_rename: {
        const char *g_old = (const char *)a32_guest_ptr(ctx, a0);
        const char *g_new = (const char *)a32_guest_ptr(ctx, a1);
        char r_old[PATH_MAX], r_new[PATH_MAX];
        const char *p_old = resolve_guest_path(ctx, g_old, r_old, sizeof(r_old));
        const char *p_new = resolve_guest_path(ctx, g_new, r_new, sizeof(r_new));
        ctx->regs[0] = host_ret(rename(p_old, p_new));
        break;
    }

    case ARM_NR_renameat: {
        const char *g_old = (const char *)a32_guest_ptr(ctx, a1);
        const char *g_new = (const char *)a32_guest_ptr(ctx, a3);
        char r_old[PATH_MAX], r_new[PATH_MAX];
        const char *p_old = resolve_guest_path(ctx, g_old, r_old, sizeof(r_old));
        const char *p_new = resolve_guest_path(ctx, g_new, r_new, sizeof(r_new));
        ctx->regs[0] = host_ret(renameat(translate_at_fd(a0), p_old,
                                        translate_at_fd(a2), p_new));
        break;
    }

    case ARM_NR_renameat2: {
        const char *g_old = (const char *)a32_guest_ptr(ctx, a1);
        const char *g_new = (const char *)a32_guest_ptr(ctx, a3);
        char r_old[PATH_MAX], r_new[PATH_MAX];
        const char *p_old = resolve_guest_path(ctx, g_old, r_old, sizeof(r_old));
        const char *p_new = resolve_guest_path(ctx, g_new, r_new, sizeof(r_new));
        ctx->regs[0] = host_ret(syscall(SYS_renameat2, translate_at_fd(a0), p_old,
                                        translate_at_fd(a2), p_new, (unsigned int)a4));
        break;
    }

    case ARM_NR_link: {
        const char *g_old = (const char *)a32_guest_ptr(ctx, a0);
        const char *g_new = (const char *)a32_guest_ptr(ctx, a1);
        char r_old[PATH_MAX], r_new[PATH_MAX];
        const char *p_old = resolve_guest_path(ctx, g_old, r_old, sizeof(r_old));
        const char *p_new = resolve_guest_path(ctx, g_new, r_new, sizeof(r_new));
        ctx->regs[0] = host_ret(link(p_old, p_new));
        break;
    }

    case ARM_NR_linkat: {
        const char *g_old = (const char *)a32_guest_ptr(ctx, a1);
        const char *g_new = (const char *)a32_guest_ptr(ctx, a3);
        char r_old[PATH_MAX], r_new[PATH_MAX];
        const char *p_old = resolve_guest_path(ctx, g_old, r_old, sizeof(r_old));
        const char *p_new = resolve_guest_path(ctx, g_new, r_new, sizeof(r_new));
        ctx->regs[0] = host_ret(linkat(translate_at_fd(a0), p_old,
                                      translate_at_fd(a2), p_new, (int)a4));
        break;
    }

    case ARM_NR_symlink: {
        const char *target = (const char *)a32_guest_ptr(ctx, a0);
        const char *linkpath = (const char *)a32_guest_ptr(ctx, a1);
        char r_link[PATH_MAX];
        const char *p_link = resolve_guest_path(ctx, linkpath, r_link, sizeof(r_link));
        ctx->regs[0] = host_ret(symlink(target, p_link));
        break;
    }

    case ARM_NR_symlinkat: {
        const char *target = (const char *)a32_guest_ptr(ctx, a0);
        const char *linkpath = (const char *)a32_guest_ptr(ctx, a2);
        char r_link[PATH_MAX];
        const char *p_link = resolve_guest_path(ctx, linkpath, r_link, sizeof(r_link));
        ctx->regs[0] = host_ret(symlinkat(target, translate_at_fd(a1), p_link));
        break;
    }

    case ARM_NR_readlink: {
        const char *guest_path = (const char *)a32_guest_ptr(ctx, a0);
        if (strcmp(guest_path, "/proc/self/exe") == 0) {
            const char *ep = (ctx->exec_path && ctx->exec_path[0]) ? ctx->exec_path : ctx->guest_argv[0];
            size_t elen = strlen(ep);
            size_t copylen = (elen < (size_t)a2) ? elen : (size_t)a2;
            memcpy(a32_guest_ptr(ctx, a1), ep, copylen);
            ctx->regs[0] = (uint32_t)copylen;
            break;
        }
        char resolved[PATH_MAX];
        const char *path = resolve_guest_path(ctx, guest_path, resolved, sizeof(resolved));
        ctx->regs[0] = host_ret(readlink(path, (char *)a32_guest_ptr(ctx, a1), (size_t)a2));
        break;
    }

    case ARM_NR_readlinkat: {
        const char *guest_path = (const char *)a32_guest_ptr(ctx, a1);
        if (strcmp(guest_path, "/proc/self/exe") == 0) {
            const char *ep = (ctx->exec_path && ctx->exec_path[0]) ? ctx->exec_path : ctx->guest_argv[0];
            size_t elen = strlen(ep);
            size_t copylen = (elen < (size_t)a3) ? elen : (size_t)a3;
            memcpy(a32_guest_ptr(ctx, a2), ep, copylen);
            ctx->regs[0] = (uint32_t)copylen;
            break;
        }
        char resolved[PATH_MAX];
        const char *path = resolve_guest_path(ctx, guest_path, resolved, sizeof(resolved));
        ctx->regs[0] = host_ret(readlinkat(translate_at_fd(a0), path,
                                          (char *)a32_guest_ptr(ctx, a2), (size_t)a3));
        break;
    }

    case ARM_NR_chmod: {
        const char *guest_path = (const char *)a32_guest_ptr(ctx, a0);
        char resolved[PATH_MAX];
        const char *path = resolve_guest_path(ctx, guest_path, resolved, sizeof(resolved));
        ctx->regs[0] = host_ret(chmod(path, (mode_t)a1));
        break;
    }

    case ARM_NR_fchmod:
        ctx->regs[0] = host_ret(fchmod((int)a0, (mode_t)a1));
        break;

    case ARM_NR_fchmodat: {
        const char *guest_path = (const char *)a32_guest_ptr(ctx, a1);
        char resolved[PATH_MAX];
        const char *path = resolve_guest_path(ctx, guest_path, resolved, sizeof(resolved));
        ctx->regs[0] = host_ret(fchmodat(translate_at_fd(a0), path, (mode_t)a2, 0));
        break;
    }

    case ARM_NR_chown:
    case ARM_NR_lchown: {
        const char *guest_path = (const char *)a32_guest_ptr(ctx, a0);
        char resolved[PATH_MAX];
        const char *path = resolve_guest_path(ctx, guest_path, resolved, sizeof(resolved));
        int r = (nr == ARM_NR_lchown) ? lchown(path, (uid_t)a1, (gid_t)a2)
                                      : chown(path, (uid_t)a1, (gid_t)a2);
        ctx->regs[0] = host_ret(r);
        break;
    }

    case ARM_NR_fchown:
        ctx->regs[0] = host_ret(fchown((int)a0, (uid_t)a1, (gid_t)a2));
        break;

    case ARM_NR_fchownat: {
        const char *guest_path = (const char *)a32_guest_ptr(ctx, a1);
        char resolved[PATH_MAX];
        const char *path = resolve_guest_path(ctx, guest_path, resolved, sizeof(resolved));
        ctx->regs[0] = host_ret(fchownat(translate_at_fd(a0), path, (uid_t)a2, (gid_t)a3, (int)a4));
        break;
    }

    case ARM_NR_umask:
        ctx->regs[0] = (uint32_t)umask((mode_t)a0);
        break;

    case ARM_NR_fsync:
        ctx->regs[0] = host_ret(fsync((int)a0));
        break;

    case ARM_NR_fdatasync:
        ctx->regs[0] = host_ret(fdatasync((int)a0));
        break;

    case ARM_NR_utimensat: {
        const char *guest_path = (const char *)a32_guest_ptr(ctx, a1);
        char resolved[PATH_MAX];
        const char *path = resolve_guest_path(ctx, guest_path, resolved, sizeof(resolved));
        struct timespec host_ts[2];
        struct timespec *ts_ptr = NULL;
        if (a2 != 0U) {
            const arm_timespec32_t *gts = (const arm_timespec32_t *)a32_guest_ptr(ctx, a2);
            host_ts[0].tv_sec  = (time_t)gts[0].tv_sec;
            host_ts[0].tv_nsec = (long)gts[0].tv_nsec;
            host_ts[1].tv_sec  = (time_t)gts[1].tv_sec;
            host_ts[1].tv_nsec = (long)gts[1].tv_nsec;
            ts_ptr = host_ts;
        }
        ctx->regs[0] = host_ret(utimensat(translate_at_fd(a0), path, ts_ptr, (int)a3));
        break;
    }

    case ARM_NR_utimensat_time64: {
        const char *guest_path = (const char *)a32_guest_ptr(ctx, a1);
        char resolved[PATH_MAX];
        const char *path = resolve_guest_path(ctx, guest_path, resolved, sizeof(resolved));
        struct timespec host_ts[2];
        struct timespec *ts_ptr = NULL;
        if (a2 != 0U) {
            const arm_timespec64_t *gts = (const arm_timespec64_t *)a32_guest_ptr(ctx, a2);
            host_ts[0].tv_sec  = (time_t)gts[0].tv_sec;
            host_ts[0].tv_nsec = (long)gts[0].tv_nsec;
            host_ts[1].tv_sec  = (time_t)gts[1].tv_sec;
            host_ts[1].tv_nsec = (long)gts[1].tv_nsec;
            ts_ptr = host_ts;
        }
        ctx->regs[0] = host_ret(utimensat(translate_at_fd(a0), path, ts_ptr, (int)a3));
        break;
    }

    case ARM_NR_nanosleep: {
        const arm_timespec32_t *req = (const arm_timespec32_t *)a32_guest_ptr(ctx, a0);
        struct timespec host_req = { (time_t)req->tv_sec, (long)req->tv_nsec };
        struct timespec host_rem;
        int r = nanosleep(&host_req, a1 ? &host_rem : NULL);
        if (r < 0) {
            ctx->regs[0] = (uint32_t)(-errno);
            if (a1 != 0U) {
                arm_timespec32_t *rem = (arm_timespec32_t *)a32_guest_ptr(ctx, a1);
                rem->tv_sec  = (int32_t)host_rem.tv_sec;
                rem->tv_nsec = (int32_t)host_rem.tv_nsec;
            }
        } else {
            ctx->regs[0] = 0U;
        }
        break;
    }

    case ARM_NR_clock_nanosleep64: {
        const arm_timespec64_t *req = (const arm_timespec64_t *)a32_guest_ptr(ctx, a2);
        struct timespec host_req = { (time_t)req->tv_sec, (long)req->tv_nsec };
        struct timespec host_rem;
        int r = clock_nanosleep((clockid_t)a0, (int)a1, &host_req, a3 ? &host_rem : NULL);
        if (r != 0) {
            ctx->regs[0] = (uint32_t)(-r);
            if (a3 != 0U && (int)a1 == 0) {
                arm_timespec64_t *rem = (arm_timespec64_t *)a32_guest_ptr(ctx, a3);
                rem->tv_sec  = (int64_t)host_rem.tv_sec;
                rem->tv_nsec = (int64_t)host_rem.tv_nsec;
            }
        } else {
            ctx->regs[0] = 0U;
        }
        break;
    }

    case ARM_NR_sched_yield:
        ctx->regs[0] = host_ret(sched_yield());
        break;

    case ARM_NR_prctl:
        ctx->regs[0] = host_ret(prctl((int)a0, (unsigned long)a1, (unsigned long)a2,
                                     (unsigned long)a3, (unsigned long)a4));
        break;

    case ARM_NR_madvise:
        ctx->regs[0] = host_ret(madvise(a32_guest_ptr(ctx, a0), (size_t)a1, (int)a2));
        break;

    case ARM_NR_kill:
        ctx->regs[0] = host_ret(kill((pid_t)a0, (int)a1));
        break;

    case ARM_NR_tkill:
        ctx->regs[0] = host_ret(syscall(SYS_tkill, (pid_t)a0, (int)a1));
        break;

    case ARM_NR_tgkill:
        ctx->regs[0] = host_ret(syscall(SYS_tgkill, (pid_t)a0, (pid_t)a1, (int)a2));
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

    a32_signals_check_pending(ctx);
    return 0;
}
