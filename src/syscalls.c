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
#define ARM_NR_setrlimit         75U
#define ARM_NR_getrlimit         76U
#define ARM_NR_getrusage         77U
#define ARM_NR_gettimeofday      78U
#define ARM_NR_symlink           83U
#define ARM_NR_readlink          85U
#define ARM_NR_munmap            91U
#define ARM_NR_truncate          92U
#define ARM_NR_ftruncate         93U
#define ARM_NR_fchmod            94U
#define ARM_NR_fchown            95U
#define ARM_NR_setitimer         104U
#define ARM_NR_getitimer         105U
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
#define ARM_NR__newselect        142U
#define ARM_NR_readv             145U
#define ARM_NR_writev            146U
#define ARM_NR_getsid            147U
#define ARM_NR_fdatasync         148U
#define ARM_NR_sched_yield       158U
#define ARM_NR_nanosleep         162U
#define ARM_NR_mremap            163U
#define ARM_NR_setresuid         164U
#define ARM_NR_poll              168U
#define ARM_NR_setresgid         170U
#define ARM_NR_prctl             172U
#define ARM_NR_rt_sigreturn      173U
#define ARM_NR_rt_sigaction      174U
#define ARM_NR_rt_sigprocmask    175U
#define ARM_NR_rt_sigpending     176U
#define ARM_NR_rt_sigtimedwait   177U
#define ARM_NR_rt_sigqueueinfo   178U
#define ARM_NR_rt_sigsuspend     179U
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
#define ARM_NR_lchown32          198U
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
#define ARM_NR_sched_setaffinity 241U
#define ARM_NR_sched_getaffinity 242U
#define ARM_NR_exit_group        248U
#define ARM_NR_epoll_create      250U
#define ARM_NR_epoll_ctl         251U
#define ARM_NR_epoll_wait        252U
#define ARM_NR_set_tid_address   256U
#define ARM_NR_clock_gettime32   263U
#define ARM_NR_clock_getres32    264U
#define ARM_NR_clock_nanosleep   265U
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
#define ARM_NR_fchownat          325U
#define ARM_NR_fstatat64         327U
#define ARM_NR_unlinkat          328U
#define ARM_NR_renameat          329U
#define ARM_NR_linkat            330U
#define ARM_NR_symlinkat         331U
#define ARM_NR_readlinkat        332U
#define ARM_NR_fchmodat          333U
#define ARM_NR_faccessat         334U
#define ARM_NR_pselect6          335U
#define ARM_NR_ppoll             336U
#define ARM_NR_unshare           337U
#define ARM_NR_set_robust_list   338U
#define ARM_NR_get_robust_list   339U
#define ARM_NR_splice            340U
#define ARM_NR_sync_file_range2  341U
#define ARM_NR_tee               342U
#define ARM_NR_epoll_pwait       346U
#define ARM_NR_utimensat         348U
#define ARM_NR_timerfd_create    350U
#define ARM_NR_eventfd           351U
#define ARM_NR_fallocate         352U
#define ARM_NR_timerfd_settime   353U
#define ARM_NR_timerfd_gettime   354U
#define ARM_NR_signalfd4         355U
#define ARM_NR_eventfd2          356U
#define ARM_NR_epoll_create1     357U
#define ARM_NR_dup3              358U
#define ARM_NR_pipe2             359U
#define ARM_NR_inotify_init1     360U
#define ARM_NR_preadv            361U
#define ARM_NR_pwritev           362U
#define ARM_NR_rt_tgsigqueueinfo 363U
#define ARM_NR_accept4           366U
#define ARM_NR_prlimit64         369U
#define ARM_NR_setns             375U
#define ARM_NR_renameat2         382U
#define ARM_NR_seccomp           383U
#define ARM_NR_getrandom         384U
#define ARM_NR_memfd_create      385U
#define ARM_NR_execveat          387U
#define ARM_NR_copy_file_range   391U
#define ARM_NR_statx             397U
#define ARM_NR_rseq              398U
#define ARM_NR_clock_gettime64   403U
#define ARM_NR_clock_getres64    406U
#define ARM_NR_clock_nanosleep64 407U
#define ARM_NR_timerfd_gettime64 410U
#define ARM_NR_timerfd_settime64 411U
#define ARM_NR_utimensat_time64  412U
#define ARM_NR_pselect6_time64   413U
#define ARM_NR_ppoll_time64      414U
#define ARM_NR_rt_sigtimedwait_time64 421U
#define ARM_NR_futex_time64      422U
#define ARM_NR_close_range       436U
#define ARM_NR_faccessat2        439U

/* ARM private syscalls (0x0f0000+) */
#define ARM_NR_breakpoint        0x0F0001U
#define ARM_NR_cacheflush        0x0F0002U
#define ARM_NR_set_tls           0x0F0005U
#define ARM_NR_get_tls           0x0F0800U

/* ARM32-specific open flag */
#define ARM_O_NOFOLLOW           0100000U

#ifndef SYS_unshare
#define SYS_unshare 97
#endif
#ifndef SYS_splice
#define SYS_splice 76
#endif
#ifndef SYS_sync_file_range
#define SYS_sync_file_range 84
#endif
#ifndef SYS_tee
#define SYS_tee 77
#endif
#ifndef SYS_fallocate
#define SYS_fallocate 47
#endif
#ifndef SYS_timerfd_create
#define SYS_timerfd_create 85
#endif
#ifndef SYS_signalfd4
#define SYS_signalfd4 74
#endif
#ifndef SYS_inotify_init1
#define SYS_inotify_init1 26
#endif
#ifndef SYS_preadv
#define SYS_preadv 69
#endif
#ifndef SYS_pwritev
#define SYS_pwritev 70
#endif
#ifndef SYS_setns
#define SYS_setns 268
#endif
#ifndef SYS_memfd_create
#define SYS_memfd_create 279
#endif
#ifndef SYS_copy_file_range
#define SYS_copy_file_range 285
#endif
#ifndef SYS_close_range
#define SYS_close_range 436
#endif
#ifndef SYS_faccessat2
#define SYS_faccessat2 439
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

typedef struct arm_timeval32 {
    int32_t tv_sec;
    int32_t tv_usec;
} arm_timeval32_t;

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

typedef struct __attribute__((packed, aligned(4))) arm_statfs64 {
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

_Static_assert(sizeof(arm_statfs64_t) == 84, "arm_statfs64_t must be 84 bytes");

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

typedef struct arm_ifreq {
    char    ifr_name[16];
    uint8_t ifr_ifru[16];
} arm_ifreq_t;

_Static_assert(sizeof(arm_ifreq_t) == 32, "arm_ifreq_t must be 32 bytes");

typedef struct a64_ifreq {
    char     ifr_name[16];
    union {
        uint8_t  raw[24];
        uint64_t align64;
    } ifr_ifru;
} a64_ifreq_t;

_Static_assert(sizeof(a64_ifreq_t) == 40, "a64_ifreq_t must be 40 bytes");

typedef struct arm_ifconf {
    int32_t  ifc_len;
    uint32_t ifc_buf;
} arm_ifconf_t;

_Static_assert(sizeof(arm_ifconf_t) == 8, "arm_ifconf_t must be 8 bytes");

typedef struct a64_ifconf {
    int   ifc_len;
    int   __pad;
    void *ifc_buf;
} a64_ifconf_t;

_Static_assert(sizeof(a64_ifconf_t) == 16, "a64_ifconf_t must be 16 bytes");

#define MAX_DIR_COOKIES 4096U
#define DIR_COOKIE_BASE 0x70000000LL

typedef struct dir_cookie_entry {
    int      fd;
    uint64_t off64;
} dir_cookie_entry_t;

static dir_cookie_entry_t g_dir_cookies[MAX_DIR_COOKIES];
static size_t             g_dir_cookie_count = 0U;

static int64_t map_dir_cookie(int fd, uint64_t host_off)
{
    if (host_off < (uint64_t)DIR_COOKIE_BASE) {
        return (int64_t)host_off;
    }
    for (size_t i = 0U; i < g_dir_cookie_count; i++) {
        if (g_dir_cookies[i].fd == fd && g_dir_cookies[i].off64 == host_off) {
            return DIR_COOKIE_BASE + (int64_t)i;
        }
    }
    size_t idx;
    if (g_dir_cookie_count < MAX_DIR_COOKIES) {
        idx = g_dir_cookie_count++;
    } else {
        idx = (size_t)(host_off % MAX_DIR_COOKIES);
    }
    g_dir_cookies[idx].fd    = fd;
    g_dir_cookies[idx].off64 = host_off;
    return DIR_COOKIE_BASE + (int64_t)idx;
}

static bool unmap_dir_cookie(int fd, int64_t guest_off, off_t *out_host_off)
{
    if (guest_off >= DIR_COOKIE_BASE &&
        (uint64_t)(guest_off - DIR_COOKIE_BASE) < g_dir_cookie_count) {
        size_t idx = (size_t)(guest_off - DIR_COOKIE_BASE);
        if (g_dir_cookies[idx].fd == fd) {
            *out_host_off = (off_t)g_dir_cookies[idx].off64;
            return true;
        }
    }
    return false;
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

static inline uint32_t id16_to_32(uint32_t id)
{
    return ((id & 0xFFFFU) == 0xFFFFU) ? (uint32_t)-1 : (id & 0xFFFFU);
}

static inline int get_host_tid(a32_ctx_t *ctx)
{
    if (ctx->host_tid == 0) {
        ctx->host_tid = (int)syscall(SYS_gettid);
    }
    return ctx->host_tid;
}

static size_t sysroot_prefix_len(const a32_ctx_t *ctx)
{
    if (!ctx || !ctx->sysroot || ctx->sysroot[0] == '\0') {
        return 0U;
    }
    size_t len = strlen(ctx->sysroot);
    while (len > 1U && ctx->sysroot[len - 1U] == '/') {
        len--;
    }
    return (len == 1U && ctx->sysroot[0] == '/') ? 0U : len;
}

static void vpath_pop_dir(char *vpath)
{
    char *last_slash = strrchr(vpath, '/');
    if (!last_slash || last_slash == vpath) {
        vpath[0] = '/';
        vpath[1] = '\0';
    } else {
        *last_slash = '\0';
    }
}

static bool is_passthrough_vpath(const char *sysroot_clean, const char *vpath)
{
    if (strncmp(vpath, "/proc/", 6) == 0 ||
        strncmp(vpath, "/sys/", 5) == 0  ||
        strncmp(vpath, "/dev/", 5) == 0) {
        return true;
    }
    if (strcmp(vpath, "/proc") == 0 ||
        strcmp(vpath, "/sys") == 0  ||
        strcmp(vpath, "/dev") == 0  ||
        strcmp(vpath, "/tmp") == 0  ||
        strncmp(vpath, "/tmp/", 5) == 0) {
        char sr_cand[PATH_MAX];
        int n = snprintf(sr_cand, sizeof(sr_cand), "%s%s", sysroot_clean, vpath);
        struct stat st;
        if (n > 0 && (size_t)n < sizeof(sr_cand) && lstat(sr_cand, &st) == 0) {
            return false;
        }
        return true;
    }
    return false;
}

static int resolve_guest_path_str_at(const a32_ctx_t *ctx, int dirfd, const char *guest_path,
                                     char *buf, size_t size, bool follow_final)
{
    if (!ctx || !guest_path || !buf || size == 0U) {
        return -1;
    }
    if (guest_path[0] == '\0') {
        buf[0] = '\0';
        return 0;
    }
    size_t sr_len = sysroot_prefix_len(ctx);
    if (sr_len == 0U) {
        int n = snprintf(buf, size, "%s", guest_path);
        return (n >= 0 && (size_t)n < size) ? 0 : -1;
    }
    if (sr_len >= PATH_MAX) {
        return -1;
    }
    char sysroot_clean[PATH_MAX];
    memcpy(sysroot_clean, ctx->sysroot, sr_len);
    sysroot_clean[sr_len] = '\0';

    char vpath[PATH_MAX];
    char rem[PATH_MAX];
    bool in_sysroot = false;

    if (guest_path[0] == '/') {
        in_sysroot = true;
        vpath[0]   = '/';
        vpath[1]   = '\0';
        int n = snprintf(rem, sizeof(rem), "%s", guest_path);
        if (n < 0 || (size_t)n >= sizeof(rem)) {
            return -1;
        }
    } else {
        char base_dir[PATH_MAX];
        base_dir[0] = '\0';
        if (dirfd == AT_FDCWD) {
            if (syscall(SYS_getcwd, base_dir, sizeof(base_dir)) < 0) {
                base_dir[0] = '\0';
            }
        } else if (dirfd >= 0) {
            char fd_link[64];
            snprintf(fd_link, sizeof(fd_link), "/proc/self/fd/%d", dirfd);
            ssize_t ln = readlink(fd_link, base_dir, sizeof(base_dir) - 1U);
            if (ln > 0) {
                base_dir[ln] = '\0';
            }
        }

        if (base_dir[0] != '\0' && strncmp(base_dir, sysroot_clean, sr_len) == 0 &&
            (base_dir[sr_len] == '/' || base_dir[sr_len] == '\0')) {
            in_sysroot = true;
            const char *vbase = (base_dir[sr_len] == '\0') ? "/" : (base_dir + sr_len);
            snprintf(vpath, sizeof(vpath), "%s", vbase);
            int n = snprintf(rem, sizeof(rem), "%s", guest_path);
            if (n < 0 || (size_t)n >= sizeof(rem)) {
                return -1;
            }
        } else if (strcmp(base_dir, "/tmp") == 0 || strncmp(base_dir, "/tmp/", 5) == 0) {
            in_sysroot = true;
            snprintf(vpath, sizeof(vpath), "%s", base_dir);
            int n = snprintf(rem, sizeof(rem), "%s", guest_path);
            if (n < 0 || (size_t)n >= sizeof(rem)) {
                return -1;
            }
        } else {
            /* dirfd is outside sysroot; check if relative path (or intermediate component) is a symlink into sysroot */
            char rel_cur[PATH_MAX];
            int nr = snprintf(rel_cur, sizeof(rel_cur), "%s", guest_path);
            if (nr < 0 || (size_t)nr >= sizeof(rel_cur)) {
                return -1;
            }
            for (int depth = 0; depth < 8; depth++) {
                /* Check intermediate prefixes and (if follow_final) final path */
                bool hopped = false;
                size_t rlen = strlen(rel_cur);
                size_t scan = 0U;
                while (scan < rlen) {
                    while (scan < rlen && rel_cur[scan] == '/') {
                        scan++;
                    }
                    if (scan >= rlen) {
                        break;
                    }
                    while (scan < rlen && rel_cur[scan] != '/') {
                        scan++;
                    }
                    size_t after_pos = scan;
                    while (after_pos < rlen && rel_cur[after_pos] == '/') {
                        after_pos++;
                    }
                    bool is_last_comp = (after_pos >= rlen);
                    if (is_last_comp && !follow_final) {
                        break;
                    }
                    char prefix[PATH_MAX];
                    if (scan >= sizeof(prefix)) {
                        break;
                    }
                    memcpy(prefix, rel_cur, scan);
                    prefix[scan] = '\0';

                    char link_target[PATH_MAX];
                    ssize_t ln = readlinkat(dirfd, prefix, link_target, sizeof(link_target) - 1U);
                    if (ln > 0) {
                        link_target[ln] = '\0';
                        const char *rest = rel_cur + after_pos;
                        if (link_target[0] == '/') {
                            in_sysroot = true;
                            vpath[0]   = '/';
                            vpath[1]   = '\0';
                            int nm = (*rest != '\0')
                                     ? snprintf(rem, sizeof(rem), "%s/%s", link_target, rest)
                                     : snprintf(rem, sizeof(rem), "%s", link_target);
                            if (nm < 0 || (size_t)nm >= sizeof(rem)) {
                                return -1;
                            }
                        } else {
                            char *last_sl = strrchr(prefix, '/');
                            char next_rel[PATH_MAX];
                            int nm;
                            if (last_sl != NULL) {
                                *last_sl = '\0';
                                nm = (*rest != '\0')
                                     ? snprintf(next_rel, sizeof(next_rel), "%s/%s/%s", prefix, link_target, rest)
                                     : snprintf(next_rel, sizeof(next_rel), "%s/%s", prefix, link_target);
                            } else {
                                nm = (*rest != '\0')
                                     ? snprintf(next_rel, sizeof(next_rel), "%s/%s", link_target, rest)
                                     : snprintf(next_rel, sizeof(next_rel), "%s", link_target);
                            }
                            if (nm > 0 && (size_t)nm < sizeof(next_rel)) {
                                snprintf(rel_cur, sizeof(rel_cur), "%s", next_rel);
                                hopped = true;
                            }
                        }
                        break;
                    }
                }
                if (in_sysroot || !hopped) {
                    break;
                }
            }
            if (!in_sysroot) {
                int n = snprintf(buf, size, "%s", rel_cur);
                return (n >= 0 && (size_t)n < size) ? 0 : -1;
            }
        }
    }

    const char *p = rem;
    int symlink_loops = 0;
    while (*p != '\0') {
        while (*p == '/') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        const char *slash = strchr(p, '/');
        size_t comp_len = slash ? (size_t)(slash - p) : strlen(p);
        const char *after = slash ? slash : (p + comp_len);
        while (*after == '/') {
            after++;
        }
        bool is_last = (*after == '\0');

        if (comp_len == 1U && p[0] == '.') {
            p = after;
            continue;
        }
        if (comp_len == 2U && p[0] == '.' && p[1] == '.') {
            if (strcmp(vpath, "/") != 0) {
                vpath_pop_dir(vpath);
            }
            p = after;
            continue;
        }

        size_t vlen = strlen(vpath);
        if (vlen + 1U + comp_len + 1U > sizeof(vpath)) {
            return -1;
        }
        if (strcmp(vpath, "/") == 0) {
            memcpy(vpath + 1U, p, comp_len);
            vpath[1U + comp_len] = '\0';
        } else {
            vpath[vlen] = '/';
            memcpy(vpath + vlen + 1U, p, comp_len);
            vpath[vlen + 1U + comp_len] = '\0';
        }

        if (strncmp(vpath, "/proc/", 6) == 0 ||
            strncmp(vpath, "/sys/", 5) == 0  ||
            strncmp(vpath, "/dev/", 5) == 0) {
            int np = is_last ? snprintf(buf, size, "%s", vpath)
                             : snprintf(buf, size, "%s/%s", vpath, after);
            return (np >= 0 && (size_t)np < size) ? 0 : -1;
        }

        if (!is_last || follow_final) {
            char host_cand[PATH_MAX];
            int nh = is_passthrough_vpath(sysroot_clean, vpath)
                     ? snprintf(host_cand, sizeof(host_cand), "%s", vpath)
                     : snprintf(host_cand, sizeof(host_cand), "%s%s", sysroot_clean, vpath);
            if (nh > 0 && (size_t)nh < sizeof(host_cand)) {
                char link_target[PATH_MAX];
                ssize_t ln = readlink(host_cand, link_target, sizeof(link_target) - 1U);
                if (ln > 0 && ++symlink_loops <= 32) {
                    link_target[ln] = '\0';
                    char next_rem[PATH_MAX];
                    int nr = is_last ? snprintf(next_rem, sizeof(next_rem), "%s", link_target)
                                     : snprintf(next_rem, sizeof(next_rem), "%s/%s", link_target, after);
                    if (nr < 0 || (size_t)nr >= sizeof(next_rem)) {
                        return -1;
                    }
                    if (link_target[0] == '/') {
                        vpath[0] = '/';
                        vpath[1] = '\0';
                    } else {
                        vpath_pop_dir(vpath);
                    }
                    snprintf(rem, sizeof(rem), "%s", next_rem);
                    p = rem;
                    continue;
                }
            }
        }
        p = after;
    }

    if (is_passthrough_vpath(sysroot_clean, vpath)) {
        int np = snprintf(buf, size, "%s", vpath);
        return (np >= 0 && (size_t)np < size) ? 0 : -1;
    }
    int nf = (strcmp(vpath, "/") == 0)
             ? snprintf(buf, size, "%s", sysroot_clean)
             : snprintf(buf, size, "%s%s", sysroot_clean, vpath);
    return (nf >= 0 && (size_t)nf < size) ? 0 : -1;
}

int a32_resolve_guest_path(const a32_ctx_t *ctx, const char *guest_path,
                           char *buf, size_t size)
{
    return resolve_guest_path_str_at(ctx, AT_FDCWD, guest_path, buf, size, true);
}

static int resolve_guest_path_at(const a32_ctx_t *ctx, int dirfd, uint32_t guest_addr,
                                 char *buf, size_t size, bool follow_final)
{
    if (!ctx || guest_addr < 0x1000U || guest_addr >= 0xFFFF0000U || !buf || size == 0U ||
        !a32_guest_range_ok((a32_ctx_t *)ctx, guest_addr, 1U, PROT_READ)) {
        return -EFAULT;
    }
    const char *gpath = (const char *)a32_guest_ptr(ctx, guest_addr);
    if (strnlen(gpath, PATH_MAX) >= PATH_MAX) {
        return -ENAMETOOLONG;
    }
    if (resolve_guest_path_str_at(ctx, dirfd, gpath, buf, size, follow_final) < 0) {
        return -ENAMETOOLONG;
    }
    return 0;
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
    a32_vm_sync_ctx((a32_ctx_t *)ctx);
    char buf[16384];
    size_t off = 0;

    for (size_t i = 0; i < ctx->region_count && off + 256 < sizeof(buf); i++) {
        const a32_region_t *r = &ctx->regions[i];
        char prot_r = (r->prot & PROT_READ) ? 'r' : '-';
        char prot_w = (r->prot & PROT_WRITE) ? 'w' : '-';
        char prot_x = (r->prot & PROT_EXEC) ? 'x' : '-';
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
    uint32_t auxv_copy[64];
    size_t bytes = ctx->auxv_bytes;
    if (bytes > sizeof(auxv_copy)) {
        bytes = sizeof(auxv_copy);
    }
    memcpy(auxv_copy, ctx->auxv_data, bytes);
    size_t words = bytes / sizeof(uint32_t);
    for (size_t i = 0U; i + 1U < words; i += 2U) {
        if (auxv_copy[i] == 0U /* AT_NULL */) {
            break;
        }
        if (auxv_copy[i] == 26U /* AT_HWCAP2 */) {
            auxv_copy[i + 1U] = 0x1FU;
        }
    }
    return create_anon_file("proc_auxv", auxv_copy, bytes);
}

static int create_proc_cpuinfo(void)
{
    static const char cpuinfo_str[] =
        "processor\t: 0\n"
        "model name\t: ARMv8 Processor rev 0 (v8l)\n"
        "BogoMIPS\t: 48.00\n"
        "Features\t: half thumb fastmult vfp edsp neon vfpv3 tls vfpv4 idiva idivt vfpd32 lpae evtstrm aes pmull sha1 sha2 crc32\n"
        "CPU implementer\t: 0x41\n"
        "CPU architecture: 8\n"
        "CPU variant\t: 0x0\n"
        "CPU part\t: 0xd08\n"
        "CPU revision\t: 0\n\n";
    return create_anon_file("proc_cpuinfo", cpuinfo_str, sizeof(cpuinfo_str) - 1U);
}

static const char *skip_proc_prefix(const char *path, bool self_sub)
{
    if (!path || path[0] != '/') {
        return NULL;
    }
    const char *p = path;
    while (*p == '/') {
        p++;
    }
    if (strncmp(p, "proc/", 5) != 0) {
        return NULL;
    }
    p += 5;
    while (*p == '/') {
        p++;
    }
    if (self_sub) {
        if (strncmp(p, "self/", 5) != 0) {
            return NULL;
        }
        p += 5;
        while (*p == '/') {
            p++;
        }
    }
    return p;
}

static bool match_proc_leaf(const char *gpath, const char *resolved, bool self_sub, const char *leaf)
{
    const char *p1 = skip_proc_prefix(gpath, self_sub);
    if (p1 && strcmp(p1, leaf) == 0) {
        return true;
    }
    const char *p2 = skip_proc_prefix(resolved, self_sub);
    return p2 != NULL && strcmp(p2, leaf) == 0;
}

static const char *get_guest_exe_path(const a32_ctx_t *ctx)
{
    const char *ep = (ctx->exec_path && ctx->exec_path[0])
                     ? ctx->exec_path
                     : (ctx->guest_argv ? ctx->guest_argv[0] : NULL);
    if (!ep) {
        ep = "";
    }
    size_t sr_len = sysroot_prefix_len(ctx);
    if (sr_len > 0U && strncmp(ep, ctx->sysroot, sr_len) == 0 && ep[sr_len] == '/') {
        ep += sr_len;
    }
    return ep;
}

#define VIRTUAL_PROC_NOT_MATCHED (-9999)

static int try_open_virtual_proc(a32_ctx_t *ctx, const char *gpath, const char *resolved)
{
    if (match_proc_leaf(gpath, resolved, true, "maps")) {
        return create_proc_maps(ctx);
    }
    if (match_proc_leaf(gpath, resolved, true, "cmdline")) {
        return create_proc_cmdline(ctx);
    }
    if (match_proc_leaf(gpath, resolved, true, "auxv")) {
        return create_proc_auxv(ctx);
    }
    if (match_proc_leaf(gpath, resolved, false, "cpuinfo")) {
        return create_proc_cpuinfo();
    }
    if (match_proc_leaf(gpath, resolved, true, "exe")) {
        const char *ep = get_guest_exe_path(ctx);
        char exe_res[PATH_MAX];
        if (a32_resolve_guest_path(ctx, ep, exe_res, sizeof(exe_res)) < 0) {
            return -ENOENT;
        }
        int fd = open(exe_res, O_RDONLY | O_CLOEXEC);
        return (fd >= 0) ? fd : -errno;
    }
    return VIRTUAL_PROC_NOT_MATCHED;
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
            syscall(SYS_futex, futex_ptr, 1 /* FUTEX_WAKE */, 1, NULL, NULL, 0);
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

static void thread_exit_cleanup(a32_ctx_t *ctx)
{
    a32_cleanup_robust_list(ctx);
    uint32_t zero = 0U;
    if (guest_write(ctx, ctx->clear_child_tid, &zero, 4U)) {
        syscall(SYS_futex, a32_guest_ptr(ctx, ctx->clear_child_tid),
                1 /* FUTEX_WAKE */, 1, NULL, NULL, 0);
    }
}

typedef struct child_thread_args {
    a32_ctx_t   *child_ctx;
    uint32_t     flags;
    uint32_t     ptid_va;
    uint32_t     ctid_va;
    int          child_tid;
    volatile int started;
} child_thread_args_t;

static void *guest_thread_start(void *arg)
{
    child_thread_args_t *args = (child_thread_args_t *)arg;
    a32_ctx_t *ctx    = args->child_ctx;
    uint32_t flags    = args->flags;
    uint32_t ptid_va  = args->ptid_va;
    uint32_t ctid_va  = args->ctid_va;

    a32_signals_set_thread_ctx(ctx);
    ctx->host_tid        = (int)syscall(SYS_gettid);
    ctx->is_thread_clone = true;
    args->child_tid      = ctx->host_tid;

    uint32_t tid32 = (uint32_t)ctx->host_tid;
    if (flags & 0x00100000U /* CLONE_PARENT_SETTID */) {
        (void)guest_write(ctx, ptid_va, &tid32, 4U);
    }
    if (flags & 0x01000000U /* CLONE_CHILD_SETTID */) {
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

static void check_pwait_signals(a32_ctx_t *ctx, uint64_t temp_mask)
{
    ctx->sig_mask = temp_mask;
    a32_signals_check_pending(ctx);
}

static void restore_pwait_sigmask(a32_ctx_t *ctx, uint64_t saved_sig_mask, uint64_t temp_mask)
{
    (void)temp_mask;
    if (ctx->exit_reason == A32_EXIT_BRANCH) {
        uint8_t *fp = (uint8_t *)a32_guest_ptr(ctx, ctx->regs[13]);
        uint32_t oldmask32 = (uint32_t)saved_sig_mask;
        memcpy(fp + 156U, &oldmask32, sizeof(oldmask32));
        memcpy(fp + 232U, &saved_sig_mask, sizeof(saved_sig_mask));
        memcpy(fp + 856U, &saved_sig_mask, sizeof(saved_sig_mask));
    } else {
        ctx->sig_mask = saved_sig_mask;
    }
}

static int pwait_sigmask_enter(a32_ctx_t *ctx, uint32_t mask_va, uint32_t mask_sz,
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
            restore_pwait_sigmask(ctx, *saved_sig_mask, *temp_mask);
            return 1;
        }
        ctx->sig_mask = *temp_mask;
        ctx->regs[0]  = orig_r0;
    }
    return 0;
}

static void pwait_sigmask_leave(a32_ctx_t *ctx, bool has_temp_mask,
                                uint64_t saved_sig_mask, uint64_t temp_mask)
{
    if (has_temp_mask) {
        check_pwait_signals(ctx, temp_mask);
        restore_pwait_sigmask(ctx, saved_sig_mask, temp_mask);
    }
}

static int translate_guest_iov(a32_ctx_t *ctx, uint32_t iov_va, uint32_t iovcnt,
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

static bool read_guest_strv(a32_ctx_t *ctx, uint32_t vec_va, char **out, int max_count, int *out_count)
{
    int count = 0;
    if (vec_va != 0U) {
        if (!a32_guest_range_ok(ctx, vec_va, 4U, PROT_READ)) {
            return false;
        }
        const uint32_t *g_vec = (const uint32_t *)a32_guest_ptr(ctx, vec_va);
        while (g_vec[count] != 0U && count < max_count) {
            if (!a32_guest_range_ok(ctx, g_vec[count], 1U, PROT_READ)) {
                return false;
            }
            out[count] = (char *)a32_guest_ptr(ctx, g_vec[count]);
            count++;
        }
    }
    out[count] = NULL;
    if (out_count) {
        *out_count = count;
    }
    return true;
}

#define FAIL_ERR(err)        do { ctx->regs[0] = (uint32_t)(-(err)); goto out; } while (0)
#define FAIL_EFAULT_IF(cond) do { if (cond) FAIL_ERR(EFAULT); } while (0)
#define FAIL_EINVAL_IF(cond) do { if (cond) FAIL_ERR(EINVAL); } while (0)
#define RESOLVE_AT(dirfd, va, buf, follow) do { \
    int _rc = resolve_guest_path_at(ctx, (dirfd), (va), (buf), sizeof(buf), (follow)); \
    if (_rc < 0) { ctx->regs[0] = (uint32_t)_rc; goto out; } \
} while (0)

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
    case ARM_NR_exit_group:
        thread_exit_cleanup(ctx);
        if (nr == ARM_NR_exit && ctx->is_thread_clone) {
            a32_signals_set_thread_ctx(NULL);
            free(ctx);
            pthread_exit(NULL);
        }
        print_stats_if_enabled(ctx);
        syscall(SYS_exit_group, (int)a0);
        return 1;

    case ARM_NR_read:
        FAIL_EFAULT_IF(a2 > 0U && a1 < 0x1000U);
        ctx->regs[0] = host_ret(read((int)a0, a32_guest_ptr(ctx, a1), (size_t)a2));
        break;

    case ARM_NR_write:
        FAIL_EFAULT_IF(a2 > 0U && a1 < 0x1000U);
        ctx->regs[0] = host_ret(write((int)a0, a32_guest_ptr(ctx, a1), (size_t)a2));
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
            off_t pos = (off_t)(((uint64_t)a4 << 32) | (uint64_t)a3);
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
        ctx->regs[0] = host_ret(openat(dirfd, resolved, (int)flags, mode));
        break;
    }

    case ARM_NR_close:
        for (size_t i = 0U; i < g_dir_cookie_count; i++) {
            if (g_dir_cookies[i].fd == (int)a0) {
                g_dir_cookies[i].fd = -1;
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
        off_t off = (off_t)(((uint64_t)a1 << 32) | (uint64_t)a2);
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
            memset(&ifr64, 0, sizeof(ifr64));
            memcpy(ifr64.ifr_name, ifr32.ifr_name, 16U);
            memcpy(ifr64.ifr_ifru.raw, ifr32.ifr_ifru, 16U);
            int r = ioctl((int)a0, (int)cmd, &ifr64);
            if (r == 0) {
                memcpy(ifr32.ifr_name, ifr64.ifr_name, 16U);
                memcpy(ifr32.ifr_ifru, ifr64.ifr_ifru.raw, 16U);
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
                    memcpy(entry32.ifr_name, buf64[i].ifr_name, 16U);
                    memcpy(entry32.ifr_ifru, buf64[i].ifr_ifru.raw, 16U);
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
            FAIL_EFAULT_IF(!a32_guest_range_ok(ctx, a2, sizeof(struct flock_arm32),
                                               (cmd == 5) ? (PROT_READ | PROT_WRITE) : PROT_READ));
            struct flock_arm32 *gfl = (struct flock_arm32 *)a32_guest_ptr(ctx, a2);
            struct flock fl;
            memset(&fl, 0, sizeof(fl));
            fl.l_type   = gfl->l_type;
            fl.l_whence = gfl->l_whence;
            fl.l_start  = (off_t)gfl->l_start;
            fl.l_len    = (off_t)gfl->l_len;
            fl.l_pid    = (pid_t)gfl->l_pid;
            if (fcntl((int)a0, cmd, &fl) < 0) {
                FAIL_ERR(errno);
            }
            if (cmd == 5) {
                gfl->l_type   = fl.l_type;
                gfl->l_whence = fl.l_whence;
                gfl->l_start  = (int32_t)fl.l_start;
                gfl->l_len    = (int32_t)fl.l_len;
                gfl->l_pid    = (int32_t)fl.l_pid;
            }
            ctx->regs[0] = 0U;
            break;
        }
        if (cmd == 12 /* F_GETLK64 */  || cmd == 13 /* F_SETLK64 */  || cmd == 14 /* F_SETLKW64 */ ||
            cmd == 36 /* F_OFD_GETLK */ || cmd == 37 /* F_OFD_SETLK */ || cmd == 38 /* F_OFD_SETLKW */) {
            FAIL_EFAULT_IF(!a32_guest_range_ok(ctx, a2, sizeof(struct flock64_arm),
                                               (cmd == 12 || cmd == 36) ? (PROT_READ | PROT_WRITE) : PROT_READ));
            struct flock64_arm *gfl = (struct flock64_arm *)a32_guest_ptr(ctx, a2);
            struct flock fl;
            memset(&fl, 0, sizeof(fl));
            fl.l_type   = gfl->l_type;
            fl.l_whence = gfl->l_whence;
            fl.l_start  = (off_t)gfl->l_start;
            fl.l_len    = (off_t)gfl->l_len;
            fl.l_pid    = (pid_t)gfl->l_pid;
            int host_cmd = (cmd == 12) ? F_GETLK : (cmd == 13 ? F_SETLK : (cmd == 14 ? F_SETLKW : cmd));
            if (fcntl((int)a0, host_cmd, &fl) < 0) {
                FAIL_ERR(errno);
            }
            if (cmd == 12 || cmd == 36) {
                gfl->l_type   = fl.l_type;
                gfl->l_whence = fl.l_whence;
                gfl->l_start  = (int64_t)fl.l_start;
                gfl->l_len    = (int64_t)fl.l_len;
                gfl->l_pid    = (int32_t)fl.l_pid;
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
        fill_arm_stat64((arm_stat64_t *)a32_guest_ptr(ctx, a1), &st);
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
        int flags        = is_at ? (int)a3 : ((nr == ARM_NR_lstat64) ? AT_SYMLINK_NOFOLLOW : 0);
        FAIL_EFAULT_IF(!a32_guest_range_ok(ctx, buf_va, sizeof(arm_stat64_t), PROT_WRITE));
        char resolved[PATH_MAX];
        RESOLVE_AT(dirfd, path_va, resolved, (flags & AT_SYMLINK_NOFOLLOW) == 0);
        struct stat st;
        if (fstatat(dirfd, resolved, &st, flags) != 0) {
            FAIL_ERR(errno);
        }
        fill_arm_stat64((arm_stat64_t *)a32_guest_ptr(ctx, buf_va), &st);
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
        uint32_t flags   = (nr == ARM_NR_faccessat2) ? a3 : 0U;
        FAIL_EINVAL_IF((is_at && (mode & ~7U) != 0U) ||
                       (flags & ~(0x100U | 0x200U | 0x1000U)) != 0U);
        char resolved[PATH_MAX];
        RESOLVE_AT(dirfd, path_va, resolved, (flags & AT_SYMLINK_NOFOLLOW) == 0U);
        if (nr == ARM_NR_faccessat2) {
            long rc = syscall(SYS_faccessat2, dirfd, resolved, (int)mode, (int)flags);
            if (rc < 0 && errno == ENOSYS) {
                rc = faccessat(dirfd, resolved, (int)mode, (int)(flags & (0x100U | 0x200U)));
            }
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
        if (sr_len > 0U) {
            char host_cwd[PATH_MAX];
            if (syscall(SYS_getcwd, host_cwd, sizeof(host_cwd)) < 0) {
                FAIL_ERR(errno);
            }
            const char *guest_cwd = host_cwd;
            if (strncmp(host_cwd, ctx->sysroot, sr_len) == 0 &&
                (host_cwd[sr_len] == '/' || host_cwd[sr_len] == '\0')) {
                guest_cwd = (host_cwd[sr_len] == '\0') ? "/" : (host_cwd + sr_len);
            }
            size_t len = strlen(guest_cwd);
            if ((size_t)a1 < len + 1U) {
                FAIL_ERR(ERANGE);
            }
            memcpy(buf, guest_cwd, len + 1U);
            ctx->regs[0] = (uint32_t)(len + 1U);
            break;
        }
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
        ctx->regs[0] = host_ret(syscall(SYS_setuid, (uid_t)a0));
        break;

    case ARM_NR_setgid32:
        ctx->regs[0] = host_ret(syscall(SYS_setgid, (gid_t)a0));
        break;

    case ARM_NR_setreuid32:
        ctx->regs[0] = host_ret(syscall(SYS_setreuid, (uid_t)a0, (uid_t)a1));
        break;

    case ARM_NR_setregid32:
        ctx->regs[0] = host_ret(syscall(SYS_setregid, (gid_t)a0, (gid_t)a1));
        break;

    case ARM_NR_setresuid:
    case ARM_NR_setresuid32: {
        bool is16 = (nr == ARM_NR_setresuid);
        uid_t r = (uid_t)(is16 ? id16_to_32(a0) : a0);
        uid_t e = (uid_t)(is16 ? id16_to_32(a1) : a1);
        uid_t s = (uid_t)(is16 ? id16_to_32(a2) : a2);
        ctx->regs[0] = host_ret(syscall(SYS_setresuid, r, e, s));
        break;
    }

    case ARM_NR_setresgid:
    case ARM_NR_setresgid32: {
        bool is16 = (nr == ARM_NR_setresgid);
        gid_t r = (gid_t)(is16 ? id16_to_32(a0) : a0);
        gid_t e = (gid_t)(is16 ? id16_to_32(a1) : a1);
        gid_t s = (gid_t)(is16 ? id16_to_32(a2) : a2);
        ctx->regs[0] = host_ret(syscall(SYS_setresgid, r, e, s));
        break;
    }

    case ARM_NR_getresuid32:
    case ARM_NR_getresgid32: {
        FAIL_EFAULT_IF((a0 != 0U && !a32_guest_range_ok(ctx, a0, 4U, PROT_WRITE)) ||
                       (a1 != 0U && !a32_guest_range_ok(ctx, a1, 4U, PROT_WRITE)) ||
                       (a2 != 0U && !a32_guest_range_ok(ctx, a2, 4U, PROT_WRITE)));
        uint32_t r = 0U, e = 0U, s = 0U;
        int rc = (nr == ARM_NR_getresuid32)
                 ? getresuid((uid_t *)&r, (uid_t *)&e, (uid_t *)&s)
                 : getresgid((gid_t *)&r, (gid_t *)&e, (gid_t *)&s);
        if (rc != 0) {
            FAIL_ERR(errno);
        }
        if (a0) *(uint32_t *)a32_guest_ptr(ctx, a0) = r;
        if (a1) *(uint32_t *)a32_guest_ptr(ctx, a1) = e;
        if (a2) *(uint32_t *)a32_guest_ptr(ctx, a2) = s;
        ctx->regs[0] = 0U;
        break;
    }

    case ARM_NR_getgroups32:
        FAIL_EINVAL_IF((int32_t)a0 < 0);
        FAIL_EFAULT_IF(a0 > 0U && a1 < 0x1000U);
        ctx->regs[0] = host_ret(syscall(SYS_getgroups, (int)a0, a0 > 0U ? (gid_t *)a32_guest_ptr(ctx, a1) : NULL));
        break;

    case ARM_NR_setgroups32:
        FAIL_EINVAL_IF((int32_t)a0 < 0 || a0 > 65536U);
        FAIL_EFAULT_IF(a0 > 0U && a1 < 0x1000U);
        ctx->regs[0] = host_ret(syscall(SYS_setgroups, (size_t)a0, a0 > 0U ? (const gid_t *)a32_guest_ptr(ctx, a1) : NULL));
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
                for (int s = 1; s <= 64; s++) {
                    uint64_t bit = 1ULL << (s - 1);
                    if ((ready & bit) != 0ULL) {
                        uint64_t prev = __atomic_fetch_and(&ctx->pending_signals, ~bit, __ATOMIC_SEQ_CST);
                        if ((prev & bit) != 0ULL) {
                            matched_sig = s;
                            break;
                        }
                    }
                }
                if (matched_sig != 0) {
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
            (void)syscall(SYS_ppoll, NULL, 0, &poll_ts, &temp_mask, (size_t)8);
        }
        restore_pwait_sigmask(ctx, saved_sig_mask, temp_mask);
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
        ctx->regs[0] = host_ret(syscall(SYS_prlimit64, (pid_t)a0, (int)a1,
                                        a2 ? a32_guest_ptr(ctx, a2) : NULL,
                                        a3 ? a32_guest_ptr(ctx, a3) : NULL));
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
        struct sysinfo si;
        if (sysinfo(&si) != 0) {
            FAIL_ERR(errno);
        }
        uint64_t mem_unit = (si.mem_unit > 0U) ? (uint64_t)si.mem_unit : 1ULL;
        while ((((uint64_t)si.totalram | (uint64_t)si.totalswap) >> 32) != 0ULL &&
               (mem_unit << 1) <= 0xFFFFFFFFULL) {
            mem_unit <<= 1;
            si.totalram  >>= 1;
            si.freeram   >>= 1;
            si.sharedram >>= 1;
            si.bufferram >>= 1;
            si.totalswap >>= 1;
            si.freeswap  >>= 1;
            si.totalhigh >>= 1;
            si.freehigh  >>= 1;
        }
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
        si32.mem_unit  = (uint32_t)mem_unit;
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
        ctx->regs[0] = host_ret(getrandom(a32_guest_ptr(ctx, a0), (size_t)a1, (unsigned int)a2));
        break;

    case ARM_NR_clock_gettime32:
    case ARM_NR_clock_gettime64: {
        bool time64 = (nr == ARM_NR_clock_gettime64);
        FAIL_EFAULT_IF(!a32_guest_range_ok(ctx, a1, time64 ? 16U : 8U, PROT_WRITE));
        struct timespec ts;
        if (clock_gettime((clockid_t)a0, &ts) != 0) {
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
        if (clock_getres((clockid_t)a0, &ts) != 0) {
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
        int r = (a1 == 0U) ? gettimeofday(&tv, NULL)
                           : (int)syscall(SYS_gettimeofday, &tv, &tz);
        if (r != 0) {
            FAIL_ERR(errno);
        }
        if (a0 != 0U) {
            arm_timeval32_t tv32 = { (int32_t)tv.tv_sec, (int32_t)tv.tv_usec };
            memcpy(a32_guest_ptr(ctx, a0), &tv32, sizeof(tv32));
        }
        if (a1 != 0U) {
            int32_t tz32[2] = { (int32_t)tz.tz_minuteswest, (int32_t)tz.tz_dsttime };
            memcpy(a32_guest_ptr(ctx, a1), tz32, sizeof(tz32));
        }
        ctx->regs[0] = 0U;
        break;
    }

    case ARM_NR_getitimer: {
        FAIL_EFAULT_IF(!a32_guest_range_ok(ctx, a1, 16U, PROT_WRITE));
        struct itimerval itv;
        memset(&itv, 0, sizeof(itv));
        if (getitimer((int)a0, &itv) != 0) {
            FAIL_ERR(errno);
        }
        if (itv.it_value.tv_sec == 0 && itv.it_value.tv_usec == 0) {
            itv.it_interval.tv_sec  = 0;
            itv.it_interval.tv_usec = 0;
        }
        int32_t out[4] = {
            (int32_t)itv.it_interval.tv_sec,
            (int32_t)itv.it_interval.tv_usec,
            (int32_t)itv.it_value.tv_sec,
            (int32_t)itv.it_value.tv_usec
        };
        memcpy(a32_guest_ptr(ctx, a1), out, sizeof(out));
        ctx->regs[0] = 0U;
        break;
    }

    case ARM_NR_setitimer: {
        FAIL_EFAULT_IF((a1 != 0U && !a32_guest_range_ok(ctx, a1, 16U, PROT_READ)) ||
                       (a2 != 0U && !a32_guest_range_ok(ctx, a2, 16U, PROT_WRITE)));
        struct itimerval new_itv, old_itv;
        memset(&new_itv, 0, sizeof(new_itv));
        memset(&old_itv, 0, sizeof(old_itv));
        if (a1 != 0U) {
            int32_t in[4];
            memcpy(in, a32_guest_ptr(ctx, a1), sizeof(in));
            FAIL_EINVAL_IF(in[1] < 0 || in[1] > 999999 || in[3] < 0 || in[3] > 999999);
            new_itv.it_interval.tv_sec  = (time_t)in[0];
            new_itv.it_interval.tv_usec = (suseconds_t)in[1];
            new_itv.it_value.tv_sec     = (time_t)in[2];
            new_itv.it_value.tv_usec    = (suseconds_t)in[3];
        }
        if (setitimer((int)a0, (a1 != 0U) ? &new_itv : NULL, (a2 != 0U) ? &old_itv : NULL) != 0) {
            FAIL_ERR(errno);
        }
        if (a2 != 0U) {
            int32_t out[4] = {
                (int32_t)old_itv.it_interval.tv_sec,
                (int32_t)old_itv.it_interval.tv_usec,
                (int32_t)old_itv.it_value.tv_sec,
                (int32_t)old_itv.it_value.tv_usec
            };
            memcpy(a32_guest_ptr(ctx, a2), out, sizeof(out));
        }
        ctx->regs[0] = 0U;
        break;
    }

    case ARM_NR_getdents64: {
        FAIL_EFAULT_IF(a1 < 0x1000U);
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
        break;
    }

    case ARM_NR_getdents: {
        FAIL_EFAULT_IF(a1 < 0x1000U);
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
        break;
    }

    case ARM_NR_fork:
    case ARM_NR_vfork: {
        pid_t pid = fork();
        if (pid < 0) {
            FAIL_ERR(errno);
        } else if (pid == 0) {
            ctx->host_tid = (int)syscall(SYS_gettid);
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
        break;
    }

    case ARM_NR_clone: {
        uint32_t flags = a0;
        FAIL_EINVAL_IF(((flags & 0x00000800U /* CLONE_SIGHAND */) != 0U &&
                        (flags & 0x00000100U /* CLONE_VM */) == 0U) ||
                       ((flags & 0x00010000U /* CLONE_THREAD */) != 0U &&
                        (flags & 0x00000800U /* CLONE_SIGHAND */) == 0U));
        FAIL_EFAULT_IF(((flags & 0x00100000U /* CLONE_PARENT_SETTID */) != 0U &&
                        !a32_guest_range_ok(ctx, a2, 4U, 2U)) ||
                       ((flags & 0x01000000U /* CLONE_CHILD_SETTID */) != 0U &&
                        !a32_guest_range_ok(ctx, a4, 4U, 2U)));
        bool is_thread = ((flags & 0x00000100U /* CLONE_VM */) != 0U &&
                          (flags & 0x00004000U /* CLONE_VFORK */) == 0U);
        if (is_thread) {
            a32_ctx_t *child_ctx = (a32_ctx_t *)calloc(1, sizeof(a32_ctx_t));
            if (!child_ctx) {
                FAIL_ERR(ENOMEM);
            }
            a32_vm_sync_ctx(ctx);
            memcpy(child_ctx, ctx, sizeof(a32_ctx_t));
            child_ctx->regs[0]          = 0U;
            child_ctx->regs[13]         = a1 ? a1 : ctx->regs[13];
            child_ctx->clear_child_tid  = (flags & 0x00200000U /* CLONE_CHILD_CLEARTID */) ? a4 : 0U;
            child_ctx->robust_list_head = 0U;
            child_ctx->robust_list_len  = 0U;
            child_ctx->pending_signals  = 0ULL;
            child_ctx->altstack_sp      = 0U;
            child_ctx->altstack_size    = 0U;
            child_ctx->altstack_flags   = 2U; /* SS_DISABLE */
            child_ctx->ras_top          = 0U;
            memset(child_ctx->ras, 0, sizeof(child_ctx->ras));
            memset(child_ctx->ibtc, 0, sizeof(child_ctx->ibtc));
            if (flags & 0x00080000U /* CLONE_SETTLS */) {
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
            break;
        }

        /* Non-thread clone: process fork */
        pid_t pid = fork();
        if (pid < 0) {
            FAIL_ERR(errno);
        } else if (pid == 0) {
            ctx->host_tid         = (int)syscall(SYS_gettid);
            ctx->is_thread_clone  = false;
            ctx->robust_list_head = 0U;
            ctx->robust_list_len  = 0U;
            ctx->pending_signals  = 0ULL;
            a32_signals_set_thread_ctx(ctx);
            if (a1 != 0U) {
                ctx->regs[13] = a1;
            }
            if (flags & 0x00080000U /* CLONE_SETTLS */) {
                ctx->tls_tp = a3;
            }
            if ((flags & 0x00100000U /* CLONE_PARENT_SETTID */) &&
                a32_guest_range_ok(ctx, a2, 4U, 2U)) {
                *(uint32_t *)a32_guest_ptr(ctx, a2) = (uint32_t)ctx->host_tid;
            }
            if ((flags & 0x01000000U /* CLONE_CHILD_SETTID */) &&
                a32_guest_range_ok(ctx, a4, 4U, 2U)) {
                *(uint32_t *)a32_guest_ptr(ctx, a4) = (uint32_t)ctx->host_tid;
            }
            ctx->clear_child_tid = (flags & 0x00200000U /* CLONE_CHILD_CLEARTID */) ? a4 : 0U;
            ctx->regs[0] = 0U;
        } else {
            if ((flags & 0x00100000U /* CLONE_PARENT_SETTID */) &&
                a32_guest_range_ok(ctx, a2, 4U, 2U)) {
                *(uint32_t *)a32_guest_ptr(ctx, a2) = (uint32_t)pid;
            }
            ctx->regs[0] = (uint32_t)pid;
        }
        break;
    }

    case ARM_NR_execve:
    case ARM_NR_execveat: {
        int dirfd         = (nr == ARM_NR_execveat) ? translate_at_fd(a0) : AT_FDCWD;
        uint32_t path_va  = (nr == ARM_NR_execveat) ? a1 : a0;
        uint32_t argv_va  = (nr == ARM_NR_execveat) ? a2 : a1;
        uint32_t envp_va  = (nr == ARM_NR_execveat) ? a3 : a2;
        uint32_t ex_flags = (nr == ARM_NR_execveat) ? a4 : 0U;

        FAIL_EFAULT_IF(!a32_guest_range_ok(ctx, path_va, 1U, 1U));

        const char *guest_orig_path = (const char *)a32_guest_ptr(ctx, path_va);
        bool follow = ((ex_flags & 0x100U /* AT_SYMLINK_NOFOLLOW */) == 0U);
        bool is_empty_path_fd = false;
        char resolved[PATH_MAX];
        if ((ex_flags & 0x1000U /* AT_EMPTY_PATH */) != 0U &&
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

        if (is_arm32_elf_file(path)) {
            char self_exe[PATH_MAX];
            ssize_t sn = readlink("/proc/self/exe", self_exe, sizeof(self_exe) - 1);
            if (sn <= 0) {
                snprintf(self_exe, sizeof(self_exe), "/proc/self/exe");
            } else {
                self_exe[sn] = '\0';
            }

            char win_buf[32];
            char *re_argv[524];
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
            re_argv[rpos++] = (char *)"--";
            re_argv[rpos++] = (char *)path;
            for (int k = 0; k < argc_count && rpos < 523; k++) {
                re_argv[rpos++] = new_argv[k];
            }
            re_argv[rpos] = NULL;
            execve(self_exe, re_argv, target_envp);
        } else {
            execve(path, new_argv, target_envp);
        }
        if (saved_fd_flags >= 0 && (saved_fd_flags & FD_CLOEXEC) != 0) {
            int saved_err = errno;
            (void)fcntl(dirfd, F_SETFD, saved_fd_flags);
            errno = saved_err;
        }
        FAIL_ERR(errno);
    }

    case ARM_NR_wait4: {
        FAIL_EFAULT_IF((a1 != 0U && !a32_guest_range_ok(ctx, a1, 4U, 2U)) ||
                       (a3 != 0U && !a32_guest_range_ok(ctx, a3, (uint32_t)sizeof(arm_rusage32_t), 2U)));
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
        FAIL_EFAULT_IF((a2 != 0U && !a32_guest_range_ok(ctx, a2, 128U, 2U)) ||
                       (a4 != 0U && !a32_guest_range_ok(ctx, a4, (uint32_t)sizeof(arm_rusage32_t), 2U)));
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
            FAIL_EFAULT_IF(!a32_guest_range_ok(ctx, a4, 4U, 1U));
            uaddr2 = a32_guest_ptr(ctx, a4);
        } else {
            uaddr2 = (void *)(uintptr_t)a4;
        }
        ctx->regs[0] = host_ret(syscall(SYS_futex, a32_guest_ptr(ctx, a0),
                                        (int)a1, a2, arg4, uaddr2, a5));
        break;
    }

    case ARM_NR_set_robust_list:
        FAIL_EINVAL_IF(a1 != 12U);
        ctx->robust_list_head = a0;
        ctx->robust_list_len  = a1;
        ctx->regs[0] = 0U;
        break;

    case ARM_NR_get_robust_list: {
        FAIL_EINVAL_IF((int32_t)a0 < 0);
        FAIL_EFAULT_IF(!a32_guest_range_ok(ctx, a1, 4U, 2U) ||
                       !a32_guest_range_ok(ctx, a2, 4U, 2U));
        int self_tid = (ctx->host_tid != 0) ? ctx->host_tid : (int)getpid();
        if (a0 != 0U && (int)a0 != self_tid) {
            FAIL_ERR((kill((pid_t)a0, 0) < 0 && errno == ESRCH) ? ESRCH : EPERM);
        }
        uint32_t h = ctx->robust_list_head;
        uint32_t l = (ctx->robust_list_len != 0U) ? ctx->robust_list_len : 12U;
        memcpy(a32_guest_ptr(ctx, a1), &h, sizeof(h));
        memcpy(a32_guest_ptr(ctx, a2), &l, sizeof(l));
        ctx->regs[0] = 0U;
        break;
    }

    case ARM_NR_pread64:
    case ARM_NR_pwrite64: {
        FAIL_EFAULT_IF(a2 > 0U && a1 < 0x1000U);
        off_t pos = (off_t)(((uint64_t)a5 << 32) | (uint64_t)a4);
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
                    ? (off_t)(((uint64_t)a3 << 32) | (uint64_t)a2)
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
                    ? (off_t)(((uint64_t)a3 << 32) | (uint64_t)a2)
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
        bool follow = ((a2 & 0x100U /* AT_SYMLINK_NOFOLLOW */) == 0U);
        int dirfd = translate_at_fd(a0);
        FAIL_EFAULT_IF(!a32_guest_range_ok(ctx, a4, 256U, 2U));
        RESOLVE_AT(dirfd, a1, resolved, follow);
        ctx->regs[0] = host_ret(syscall(SYS_statx, dirfd, resolved,
                                        (int)a2, (unsigned int)a3,
                                        a32_guest_ptr(ctx, a4)));
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
        FAIL_EFAULT_IF(!a32_guest_range_ok(ctx, a0, 1U, 1U));
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
        sigemptyset(&hmask);
        for (int s = 1; s <= 64; s++) {
            if ((gmask & (1ULL << (s - 1))) != 0ULL) {
                sigaddset(&hmask, s);
            }
        }
        ctx->regs[0] = host_ret(syscall(SYS_signalfd4, (int)a0, &hmask, sizeof(hmask), (int)a3));
        break;
    }

    case ARM_NR_splice:
    case ARM_NR_copy_file_range: {
        FAIL_EFAULT_IF((a1 != 0U && !a32_guest_range_ok(ctx, a1, 8U, 3U)) ||
                       (a3 != 0U && !a32_guest_range_ok(ctx, a3, 8U, 3U)));
        loff_t *p_off_in  = a1 ? (loff_t *)a32_guest_ptr(ctx, a1) : NULL;
        loff_t *p_off_out = a3 ? (loff_t *)a32_guest_ptr(ctx, a3) : NULL;
        long h_nr = (nr == ARM_NR_splice) ? SYS_splice : SYS_copy_file_range;
        ctx->regs[0] = host_ret(syscall(h_nr, (int)a0, p_off_in,
                                        (int)a2, p_off_out, (size_t)a4, (unsigned int)a5));
        break;
    }

    case ARM_NR_sync_file_range2: {
        off_t offset = (off_t)(((uint64_t)a3 << 32) | (uint64_t)a2);
        off_t nbytes = (off_t)(((uint64_t)a5 << 32) | (uint64_t)a4);
        ctx->regs[0] = host_ret(syscall(SYS_sync_file_range, (int)a0, offset, nbytes, (unsigned int)a1));
        break;
    }

    case ARM_NR_tee:
        ctx->regs[0] = host_ret(syscall(SYS_tee, (int)a0, (int)a1, (size_t)a2, (unsigned int)a3));
        break;

    case ARM_NR_fallocate: {
        off_t offset = (off_t)(((uint64_t)a3 << 32) | (uint64_t)a2);
        off_t len    = (off_t)(((uint64_t)a5 << 32) | (uint64_t)a4);
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
            FAIL_EFAULT_IF(!a32_guest_range_ok(ctx, a3, min_len, 1U));
            int64_t sec, usec;
            if (time64) {
                int64_t tv64_in[2];
                memcpy(tv64_in, a32_guest_ptr(ctx, a3), sizeof(tv64_in));
                sec  = tv64_in[0];
                usec = tv64_in[1];
            } else {
                arm_timeval32_t tv32;
                memcpy(&tv32, a32_guest_ptr(ctx, a3), sizeof(tv32));
                sec  = tv32.tv_sec;
                usec = tv32.tv_usec;
            }
            if (usec < 0 || usec >= 1000000) {
                FAIL_ERR(EDOM);
            }
            struct timeval tv64 = { (time_t)sec, (suseconds_t)usec };
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
            FAIL_EFAULT_IF(!a32_guest_range_ok(ctx, a4, 4U, 3U));
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
                    int64_t tv64_out[2] = { (int64_t)tv64.tv_sec, (int64_t)tv64.tv_usec };
                    memcpy(out_buf, tv64_out, 16);
                } else {
                    arm_timeval32_t tv32 = { (int32_t)tv64.tv_sec, (int32_t)tv64.tv_usec };
                    memcpy(out_buf, &tv32, 8);
                }
                socklen_t copy_len = (guest_len < full_len) ? guest_len : full_len;
                FAIL_EFAULT_IF(copy_len > 0U && !a32_guest_range_ok(ctx, a3, (uint32_t)copy_len, 2U));
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
                                           (nr == ARM_NR_recvmsg) ? 3U : 1U));
        arm_msghdr32_t *gm = (arm_msghdr32_t *)a32_guest_ptr(ctx, a1);
        if (gm->msg_iovlen > 1024U) {
            FAIL_ERR(EMSGSIZE);
        }
        FAIL_EINVAL_IF(nr == ARM_NR_sendmsg && gm->msg_controllen > 0U && gm->msg_controllen < 12U);
        FAIL_EFAULT_IF((gm->msg_name != 0U && gm->msg_name < 0x1000U) ||
                       (gm->msg_controllen != 0U &&
                        !a32_guest_range_ok(ctx, gm->msg_control, gm->msg_controllen,
                                            (nr == ARM_NR_recvmsg) ? 2U : 1U)));
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
                        if (cm64->cmsg_level == 1 /* SOL_SOCKET */ &&
                            cm64->cmsg_type == 1 /* SCM_RIGHTS */) {
                            size_t nfds = payload_len / sizeof(int);
                            const int *fds = (const int *)(const void *)CMSG_DATA(cm64);
                            for (size_t fi = 0U; fi < nfds; fi++) {
                                close(fds[fi]);
                            }
                        }
                        gm->msg_flags |= 0x08U /* MSG_CTRUNC */;
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
        FAIL_EFAULT_IF((a1 != 0U && !a32_guest_range_ok(ctx, a1, chk_bytes, 3U)) ||
                       (a2 != 0U && !a32_guest_range_ok(ctx, a2, chk_bytes, 3U)) ||
                       (a3 != 0U && !a32_guest_range_ok(ctx, a3, chk_bytes, 3U)));
        struct timespec host_ts;
        struct timespec *ts_ptr = NULL;
        bool time64 = (nr == ARM_NR_pselect6_time64);
        if (a4 != 0U) {
            if (nr == ARM_NR__newselect) {
                arm_timeval32_t tv32;
                FAIL_EFAULT_IF(!guest_read(ctx, &tv32, a4, sizeof(tv32)));
                FAIL_EINVAL_IF(tv32.tv_sec < 0 || tv32.tv_usec < 0 || tv32.tv_usec >= 1000000);
                host_ts.tv_sec  = (time_t)tv32.tv_sec;
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
            if (guest_range_mapped(ctx, a4, ts_sz, 3U)) {
                if (nr == ARM_NR__newselect) {
                    arm_timeval32_t *tv32 = (arm_timeval32_t *)a32_guest_ptr(ctx, a4);
                    tv32->tv_sec  = (int32_t)host_ts.tv_sec;
                    tv32->tv_usec = (int32_t)(host_ts.tv_nsec / 1000L);
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
        FAIL_EFAULT_IF(a1 > 0U && !a32_guest_range_ok(ctx, a0, 8U, 3U));
        ctx->regs[0] = host_ret(poll((a1 > 0U) ? (struct pollfd *)a32_guest_ptr(ctx, a0) : NULL,
                                     (nfds_t)a1, (int)a2));
        break;

    case ARM_NR_ppoll:
    case ARM_NR_ppoll_time64: {
        bool time64 = (nr == ARM_NR_ppoll_time64);
        FAIL_EFAULT_IF(a1 > 0U && !a32_guest_range_ok(ctx, a0, 8U, 3U));
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
        long r = syscall(SYS_ppoll, fds, (nfds_t)a1, ts_ptr,
                         has_temp_mask ? &temp_mask : NULL, (size_t)8);
        int saved_errno = errno;
        if (a2 != 0U && (r >= 0 || saved_errno == EINTR)) {
            uint32_t ts_sz = time64 ? (uint32_t)sizeof(arm_timespec64_t)
                                    : (uint32_t)sizeof(arm_timespec32_t);
            a32_vm_sync_ctx(ctx);
            if (guest_range_mapped(ctx, a2, ts_sz, 3U)) {
                (void)write_guest_timespec(ctx, a2, time64, &host_ts);
            }
        }
        errno = saved_errno;
        ctx->regs[0] = host_ret(r);
        pwait_sigmask_leave(ctx, has_temp_mask, saved_sig_mask, temp_mask);
        break;
    }

    case ARM_NR_epoll_create:
        ctx->regs[0] = host_ret(epoll_create((int)a0));
        break;

    case ARM_NR_epoll_create1:
        ctx->regs[0] = host_ret(epoll_create1((int)a0));
        break;

    case ARM_NR_epoll_ctl:
        FAIL_EFAULT_IF(a3 != 0U && !a32_guest_range_ok(ctx, a3, (uint32_t)sizeof(struct epoll_event), 1U));
        ctx->regs[0] = host_ret(epoll_ctl((int)a0, (int)a1, (int)a2,
                                          a3 ? (struct epoll_event *)a32_guest_ptr(ctx, a3) : NULL));
        break;

    case ARM_NR_epoll_wait:
        FAIL_EINVAL_IF((int32_t)a2 <= 0 || a2 > (uint32_t)(INT_MAX / sizeof(struct epoll_event)));
        FAIL_EFAULT_IF(!a32_guest_range_ok(ctx, a1, (uint32_t)sizeof(struct epoll_event), 2U));
        ctx->regs[0] = host_ret(epoll_wait((int)a0, (struct epoll_event *)a32_guest_ptr(ctx, a1),
                                           (int)a2, (int)a3));
        break;

    case ARM_NR_epoll_pwait: {
        FAIL_EINVAL_IF((int32_t)a2 <= 0 || a2 > (uint32_t)(INT_MAX / sizeof(struct epoll_event)));
        FAIL_EFAULT_IF(!a32_guest_range_ok(ctx, a1, (uint32_t)sizeof(struct epoll_event), 2U));
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
        ctx->regs[0] = host_ret(r);
        pwait_sigmask_leave(ctx, has_temp_mask, saved_sig_mask, temp_mask);
        break;
    }

    case ARM_NR_eventfd:
    case ARM_NR_eventfd2:
        ctx->regs[0] = host_ret(eventfd((unsigned int)a0, (nr == ARM_NR_eventfd2) ? (int)a1 : 0));
        break;

    case ARM_NR_timerfd_create:
        ctx->regs[0] = host_ret(timerfd_create((int)a0, (int)a1));
        break;

    case ARM_NR_timerfd_settime:
    case ARM_NR_timerfd_settime64: {
        bool time64 = (nr == ARM_NR_timerfd_settime64);
        uint32_t it_sz = time64 ? 32U : 16U;
        struct itimerspec h_in, h_out = { 0 };
        FAIL_EFAULT_IF(!read_guest_itimerspec(ctx, a2, time64, &h_in) ||
                       (a3 != 0U && !a32_guest_range_ok(ctx, a3, it_sz, 2U)));
        int r = timerfd_settime((int)a0, (int)a1, &h_in, (a3 != 0U) ? &h_out : NULL);
        if (r == 0 && a3 != 0U) {
            (void)write_guest_itimerspec(ctx, a3, time64, &h_out);
        }
        ctx->regs[0] = host_ret(r);
        break;
    }

    case ARM_NR_timerfd_gettime:
    case ARM_NR_timerfd_gettime64: {
        bool time64 = (nr == ARM_NR_timerfd_gettime64);
        uint32_t it_sz = time64 ? 32U : 16U;
        FAIL_EFAULT_IF(!a32_guest_range_ok(ctx, a1, it_sz, 2U));
        struct itimerspec h_out = { 0 };
        int r = timerfd_gettime((int)a0, &h_out);
        if (r == 0) {
            (void)write_guest_itimerspec(ctx, a1, time64, &h_out);
        }
        ctx->regs[0] = host_ret(r);
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
        ctx->regs[0] = host_ret(dup3((int)a0, (int)a1, (int)a2));
        break;

    case ARM_NR_pipe:
    case ARM_NR_pipe2: {
        FAIL_EFAULT_IF(!a32_guest_range_ok(ctx, a0, 8U, 2U));
        int fds[2];
        int r = (nr == ARM_NR_pipe2) ? pipe2(fds, (int)a1) : pipe(fds);
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
        int u_flags   = (nr == ARM_NR_unlinkat) ? (int)a2
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
        ctx->regs[0] = host_ret(syscall(SYS_renameat2, oldfd, r_old, newfd, r_new, fl));
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
        int l_flags     = is_at ? (int)a4 : 0;
        bool follow_old = ((l_flags & 0x400 /* AT_SYMLINK_FOLLOW */) != 0);
        RESOLVE_AT(oldfd, old_va, r_old, follow_old);
        RESOLVE_AT(newfd, new_va, r_new, false);
        ctx->regs[0] = host_ret(linkat(oldfd, r_old, newfd, r_new, l_flags));
        break;
    }

    case ARM_NR_symlink:
    case ARM_NR_symlinkat: {
        char r_link[PATH_MAX];
        int newfd       = (nr == ARM_NR_symlinkat) ? translate_at_fd(a1) : AT_FDCWD;
        uint32_t lnk_va = (nr == ARM_NR_symlinkat) ? a2 : a1;
        FAIL_EFAULT_IF(!a32_guest_range_ok(ctx, a0, 1U, 1U));
        const char *target = (const char *)a32_guest_ptr(ctx, a0);
        if (strnlen(target, PATH_MAX) >= PATH_MAX) {
            FAIL_ERR(ENAMETOOLONG);
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
        FAIL_EFAULT_IF(!a32_guest_range_ok(ctx, path_va, 1U, 1U) ||
                       !a32_guest_range_ok(ctx, buf_va, 1U, 2U));
        char resolved[PATH_MAX];
        RESOLVE_AT(dirfd, path_va, resolved, false);
        const char *guest_path = (const char *)a32_guest_ptr(ctx, path_va);
        if (match_proc_leaf(guest_path, resolved, true, "exe")) {
            const char *ep = get_guest_exe_path(ctx);
            size_t elen = strlen(ep);
            size_t copylen = (elen < (size_t)bufsiz) ? elen : (size_t)bufsiz;
            FAIL_EFAULT_IF(!guest_write(ctx, buf_va, ep, (uint32_t)copylen));
            ctx->regs[0] = (uint32_t)copylen;
            break;
        }
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
        int c_flags = is_at ? (int)a4
                    : (nr == ARM_NR_lchown || nr == ARM_NR_lchown32) ? 0x100 /* AT_SYMLINK_NOFOLLOW */ : 0;
        bool follow = ((c_flags & 0x100 /* AT_SYMLINK_NOFOLLOW */) == 0);
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
        ctx->regs[0] = host_ret(fdatasync((int)a0));
        break;

    case ARM_NR_utimensat:
    case ARM_NR_utimensat_time64: {
        bool time64 = (nr == ARM_NR_utimensat_time64);
        char resolved[PATH_MAX];
        const char *path = NULL;
        int dirfd = translate_at_fd(a0);
        if (a1 != 0U) {
            bool follow = ((a3 & 0x100U /* AT_SYMLINK_NOFOLLOW */) == 0U);
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
        ctx->regs[0] = host_ret(syscall(SYS_utimensat, dirfd, path, ts_ptr, (int)a3));
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
        bool abstime  = ((flags & TIMER_ABSTIME) != 0);
        uint32_t ts_sz = time64 ? 16U : 8U;
        struct timespec host_req, host_rem = { 0, 0 };
        FAIL_EFAULT_IF(!read_guest_timespec(ctx, req_va, time64, &host_req) ||
                       (!abstime && rem_va != 0U && !a32_guest_range_ok(ctx, rem_va, ts_sz, 2U)));
        int err;
        if (is_clock) {
            err = clock_nanosleep(clk, flags, &host_req,
                                  (!abstime && rem_va != 0U) ? &host_rem : NULL);
        } else {
            err = (nanosleep(&host_req, (rem_va != 0U) ? &host_rem : NULL) < 0) ? errno : 0;
        }
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
        unsigned long arg2 = (unsigned long)a1;
        if (a0 == 15U /* PR_SET_NAME */) {
            FAIL_EFAULT_IF(!a32_guest_range_ok(ctx, a1, 1U, 1U));
            const char *src = (const char *)a32_guest_ptr(ctx, a1);
            size_t n = strnlen(src, 15U);
            memcpy(tls_comm, src, n);
            memset(tls_comm + n, 0, 16U - n);
            arg2 = (unsigned long)(uintptr_t)src;
        } else if (a0 == 16U /* PR_GET_NAME */) {
            FAIL_EFAULT_IF(!guest_write(ctx, a1, tls_comm, 16U));
            arg2 = (unsigned long)(uintptr_t)a32_guest_ptr(ctx, a1);
        } else if (a0 == 2U /* PR_GET_PDEATHSIG */) {
            FAIL_EFAULT_IF(!a32_guest_range_ok(ctx, a1, 4U, 2U));
            arg2 = (unsigned long)(uintptr_t)a32_guest_ptr(ctx, a1);
        }
        ctx->regs[0] = host_ret(prctl((int)a0, arg2, (unsigned long)a2,
                                      (unsigned long)a3, (unsigned long)a4));
        break;
    }

    case ARM_NR_madvise: {
        uint64_t aligned_len = ((uint64_t)a1 + 0xFFFULL) & ~0xFFFULL;
        FAIL_EINVAL_IF((a0 & 0xFFFU) != 0U || a0 < 0x1000U ||
                       (uint64_t)a0 + (uint64_t)a1 > 0xFFFF0000ULL ||
                       (uint64_t)a0 + aligned_len > 0xFFFF0000ULL);
        if (a1 > 0U) {
            if (madvise(a32_guest_ptr(ctx, a0), 0U, (int)a2) != 0) {
                FAIL_ERR(errno);
            }
            a32_vm_sync_ctx(ctx);
            if (!guest_range_mapped(ctx, a0, (uint32_t)aligned_len, 0U)) {
                FAIL_ERR(ENOMEM);
            }
            if (a2 == 4U /* MADV_DONTNEED */) {
                a32_jit_invalidate_range(ctx, a0, a0 + (uint32_t)aligned_len);
            }
        }
        ctx->regs[0] = host_ret(madvise(a32_guest_ptr(ctx, a0), (size_t)a1, (int)a2));
        break;
    }

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

out:
    #undef RESOLVE_AT
    #undef FAIL_EINVAL_IF
    #undef FAIL_EFAULT_IF
    #undef FAIL_ERR

    if ((ctx->exit_reason != A32_EXIT_BRANCH ||
         nr == ARM_NR_sigreturn ||
         nr == ARM_NR_rt_sigreturn) &&
        __builtin_expect((__atomic_load_n(&ctx->pending_signals, __ATOMIC_ACQUIRE) & ~ctx->sig_mask) != 0ULL, 0)) {
        a32_signals_check_pending(ctx);
    }
    return 0;
}
