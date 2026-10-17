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
#define ARM_NR__newselect        142U
#define ARM_NR_readv             145U
#define ARM_NR_writev            146U
#define ARM_NR_getsid            147U
#define ARM_NR_fdatasync         148U
#define ARM_NR_sched_yield       158U
#define ARM_NR_nanosleep         162U
#define ARM_NR_mremap            163U
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
#define ARM_NR_execveat          387U
#define ARM_NR_statx             397U
#define ARM_NR_clock_gettime64   403U
#define ARM_NR_clock_getres64    406U
#define ARM_NR_clock_nanosleep64 407U
#define ARM_NR_timerfd_gettime64 410U
#define ARM_NR_timerfd_settime64 411U
#define ARM_NR_utimensat_time64  412U
#define ARM_NR_pselect6_time64   413U
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

typedef struct arm_timeval32 {
    int32_t tv_sec;
    int32_t tv_usec;
} arm_timeval32_t;

typedef struct arm_itimerspec32 {
    arm_timespec32_t it_interval;
    arm_timespec32_t it_value;
} arm_itimerspec32_t;

typedef struct arm_itimerspec64 {
    arm_timespec64_t it_interval;
    arm_timespec64_t it_value;
} arm_itimerspec64_t;

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
    if (!ctx->sysroot || ctx->sysroot[0] == '\0') {
        int n = snprintf(buf, size, "%s", guest_path);
        return (n >= 0 && (size_t)n < size) ? 0 : -1;
    }

    char sysroot_clean[PATH_MAX];
    size_t sr_len = strlen(ctx->sysroot);
    if (sr_len >= sizeof(sysroot_clean)) {
        return -1;
    }
    memcpy(sysroot_clean, ctx->sysroot, sr_len + 1U);
    while (sr_len > 1U && sysroot_clean[sr_len - 1U] == '/') {
        sysroot_clean[--sr_len] = '\0';
    }
    if (strcmp(sysroot_clean, "/") == 0) {
        int n = snprintf(buf, size, "%s", guest_path);
        return (n >= 0 && (size_t)n < size) ? 0 : -1;
    }

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
                char *last_slash = strrchr(vpath, '/');
                if (!last_slash || last_slash == vpath) {
                    vpath[0] = '/';
                    vpath[1] = '\0';
                } else {
                    *last_slash = '\0';
                }
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
                        char *last_slash = strrchr(vpath, '/');
                        if (!last_slash || last_slash == vpath) {
                            vpath[0] = '/';
                            vpath[1] = '\0';
                        } else {
                            *last_slash = '\0';
                        }
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
    if (!ctx || guest_addr < 0x1000U || !buf || size == 0U) {
        return -1;
    }
    const char *guest_path = (const char *)a32_guest_ptr(ctx, guest_addr);
    return resolve_guest_path_str_at(ctx, dirfd, guest_path, buf, size, follow_final);
}

static int resolve_guest_path_ex(const a32_ctx_t *ctx, uint32_t guest_addr,
                                 char *buf, size_t size, bool follow_final)
{
    return resolve_guest_path_at(ctx, AT_FDCWD, guest_addr, buf, size, follow_final);
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
        char prot_r = (r->prot & 1U) ? 'r' : '-';
        char prot_w = (r->prot & 2U) ? 'w' : '-';
        char prot_x = (r->prot & 4U) ? 'x' : '-';
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
    if (!ctx || ctx->robust_list_head < 0x1000U) {
        return;
    }
    uint32_t head_va = ctx->robust_list_head;
    uint32_t *head_ptr = (uint32_t *)a32_guest_ptr(ctx, head_va);
    int32_t futex_offset = (int32_t)head_ptr[1];
    uint32_t curr = head_ptr[0];

    size_t limit = 1024U;
    while (curr >= 0x1000U && curr != head_va && limit-- > 0U) {
        uint32_t futex_va = curr + (uint32_t)futex_offset;
        if (futex_va >= 0x1000U) {
            uint32_t *futex_ptr = (uint32_t *)a32_guest_ptr(ctx, futex_va);
            uint32_t val = *futex_ptr;
            if ((val & 0x0FFFFFFFU) == (uint32_t)ctx->host_tid) {
                *futex_ptr |= 0x40000000U /* FUTEX_OWNER_DIED */;
                syscall(SYS_futex, futex_ptr, 1 /* FUTEX_WAKE */, 1, NULL, NULL, 0);
            }
        }
        uint32_t *node = (uint32_t *)a32_guest_ptr(ctx, curr);
        curr = node[0];
    }
    ctx->robust_list_head = 0U;
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

    if ((flags & 0x00100000U /* CLONE_PARENT_SETTID */) && ptid_va != 0U) {
        *(uint32_t *)a32_guest_ptr(ctx, ptid_va) = (uint32_t)ctx->host_tid;
    }
    if ((flags & 0x01000000U /* CLONE_CHILD_SETTID */) && ctid_va != 0U) {
        uint32_t *ctid = (uint32_t *)a32_guest_ptr(ctx, ctid_va);
        *ctid = (uint32_t)ctx->host_tid;
    }

    __atomic_store_n(&args->started, 1, __ATOMIC_SEQ_CST);

    a32_jit_run(ctx);

    a32_cleanup_robust_list(ctx);
    if (ctx->clear_child_tid != 0U) {
        uint32_t *ctid = (uint32_t *)a32_guest_ptr(ctx, ctx->clear_child_tid);
        *ctid = 0U;
        syscall(SYS_futex, ctid, 1 /* FUTEX_WAKE */, 1, NULL, NULL, 0);
    }
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

static void check_pwait_signals(a32_ctx_t *ctx, uint64_t temp_mask)
{
    uint64_t pending = __atomic_load_n(&ctx->pending_signals, __ATOMIC_SEQ_CST);
    if ((pending & ~temp_mask) == 0ULL) {
        return;
    }
    uint64_t blocked = pending & temp_mask;
    if (blocked != 0ULL) {
        __atomic_fetch_and(&ctx->pending_signals, ~blocked, __ATOMIC_SEQ_CST);
    }
    ctx->sig_mask = 0ULL;
    a32_signals_check_pending(ctx);
    if (blocked != 0ULL) {
        __atomic_fetch_or(&ctx->pending_signals, blocked, __ATOMIC_SEQ_CST);
    }
}

static void restore_pwait_sigmask(a32_ctx_t *ctx, uint64_t saved_sig_mask, uint64_t temp_mask)
{
    (void)temp_mask;
    if (ctx->exit_reason == A32_EXIT_BRANCH) {
        uint8_t *fp = (uint8_t *)a32_guest_ptr(ctx, ctx->regs[13]);
        uint32_t oldmask32 = (uint32_t)saved_sig_mask;
        memcpy(fp + 156U, &oldmask32, sizeof(oldmask32));
        memcpy(fp + 232U, &saved_sig_mask, sizeof(saved_sig_mask));
        memcpy(fp + 464U, &saved_sig_mask, sizeof(saved_sig_mask));
        ctx->sig_mask |= saved_sig_mask;
    } else {
        ctx->sig_mask = saved_sig_mask;
    }
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
            a32_signals_set_thread_ctx(NULL);
            free(ctx);
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
        if (a2 > 0U && a1 < 0x1000U) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        ctx->regs[0] = host_ret(read((int)a0, a32_guest_ptr(ctx, a1), (size_t)a2));
        break;

    case ARM_NR_write:
        if (a2 > 0U && a1 < 0x1000U) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        ctx->regs[0] = host_ret(write((int)a0, a32_guest_ptr(ctx, a1), (size_t)a2));
        break;

    case ARM_NR_readv:
    case ARM_NR_writev: {
        if ((int32_t)a2 < 0 || a2 > 1024U) {
            ctx->regs[0] = (uint32_t)(-EINVAL);
            break;
        }
        if (a2 > 0U && a1 < 0x1000U) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
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
        bool bad_iov = false;
        for (uint32_t i = 0U; i < a2; i++) {
            if (guest_iov[i].iov_len > 0U && guest_iov[i].iov_base < 0x1000U) {
                bad_iov = true;
                break;
            }
            iov_ptr[i].iov_base = a32_guest_ptr(ctx, guest_iov[i].iov_base);
            iov_ptr[i].iov_len  = (size_t)guest_iov[i].iov_len;
        }
        if (bad_iov) {
            if (iov_ptr != host_iov) {
                free(iov_ptr);
            }
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
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
        if (a0 < 0x1000U) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
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
        bool follow = ((a1 & 0100000U /* O_NOFOLLOW */) == 0U);
        if (resolve_guest_path_ex(ctx, a0, resolved, sizeof(resolved), follow) < 0) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        ctx->regs[0] = host_ret(openat(AT_FDCWD, resolved, (int)a1, (mode_t)a2));
        break;
    }

    case ARM_NR_openat: {
        if (a1 < 0x1000U) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
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
        int dirfd = translate_at_fd(a0);
        char resolved[PATH_MAX];
        bool follow = ((a2 & 0100000U /* O_NOFOLLOW */) == 0U);
        if (resolve_guest_path_at(ctx, dirfd, a1, resolved, sizeof(resolved), follow) < 0) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        ctx->regs[0] = host_ret(openat(dirfd, resolved, (int)a2, (mode_t)a3));
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
            ctx->regs[0] = (uint32_t)(-errno);
        } else if ((uint64_t)res >= (uint64_t)DIR_COOKIE_BASE) {
            ctx->regs[0] = (uint32_t)map_dir_cookie((int)a0, (uint64_t)res);
        } else {
            ctx->regs[0] = (uint32_t)res;
        }
        break;
    }

    case ARM_NR__llseek: {
        if (a3 < 0x1000U) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        off_t off = (off_t)(((uint64_t)a1 << 32) | (uint64_t)a2);
        if (a4 == SEEK_SET) {
            (void)unmap_dir_cookie((int)a0, (int64_t)off, &off);
        }
        off_t res = lseek((int)a0, off, (int)a4);
        if (res < 0) {
            ctx->regs[0] = (uint32_t)(-errno);
        } else {
            uint64_t ures = (uint64_t)res;
            if (ures >= (uint64_t)DIR_COOKIE_BASE) {
                ures = (uint64_t)map_dir_cookie((int)a0, ures);
            }
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

    case ARM_NR_mremap:
        ctx->regs[0] = a32_vm_mremap(ctx, a0, a1, a2, a3, a4);
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
            if (a2 < 0x1000U) {
                ctx->regs[0] = (uint32_t)(-EFAULT);
                break;
            }
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
        if (cmd == 12 /* F_GETLK64 */  || cmd == 13 /* F_SETLK64 */  || cmd == 14 /* F_SETLKW64 */ ||
            cmd == 36 /* F_OFD_GETLK */ || cmd == 37 /* F_OFD_SETLK */ || cmd == 38 /* F_OFD_SETLKW */) {
            if (a2 < 0x1000U) {
                ctx->regs[0] = (uint32_t)(-EFAULT);
                break;
            }
            struct flock64_arm *gfl = (struct flock64_arm *)a32_guest_ptr(ctx, a2);
            struct flock fl;
            memset(&fl, 0, sizeof(fl));
            fl.l_type   = gfl->l_type;
            fl.l_whence = gfl->l_whence;
            fl.l_start  = (off_t)gfl->l_start;
            fl.l_len    = (off_t)gfl->l_len;
            fl.l_pid    = (pid_t)gfl->l_pid;
            int host_cmd = (cmd == 12) ? F_GETLK : (cmd == 13 ? F_SETLK : (cmd == 14 ? F_SETLKW : cmd));
            int r = fcntl((int)a0, host_cmd, &fl);
            if (r < 0) {
                ctx->regs[0] = (uint32_t)(-errno);
            } else {
                if (cmd == 12 || cmd == 36) {
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
        if (cmd == 15 /* F_SETOWN_EX */ || cmd == 16 /* F_GETOWN_EX */) {
            if (a2 < 0x1000U) {
                ctx->regs[0] = (uint32_t)(-EFAULT);
                break;
            }
            ctx->regs[0] = host_ret(fcntl((int)a0, cmd, a32_guest_ptr(ctx, a2)));
            break;
        }
        ctx->regs[0] = host_ret(fcntl((int)a0, cmd, (unsigned long)a2));
        break;
    }

    case ARM_NR_uname: {
        if (a0 < 0x1000U) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
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
        if (a1 < 0x1000U) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
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
        if (a1 < 0x1000U) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        struct stat st;
        char resolved[PATH_MAX];
        bool follow = (nr != ARM_NR_lstat64);
        if (resolve_guest_path_ex(ctx, a0, resolved, sizeof(resolved), follow) < 0) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        int flags = (nr == ARM_NR_lstat64) ? AT_SYMLINK_NOFOLLOW : 0;
        if (fstatat(AT_FDCWD, resolved, &st, flags) != 0) {
            ctx->regs[0] = (uint32_t)(-errno);
        } else {
            fill_arm_stat64((arm_stat64_t *)a32_guest_ptr(ctx, a1), &st);
            ctx->regs[0] = 0U;
        }
        break;
    }

    case ARM_NR_fstatat64: {
        if (a2 < 0x1000U) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        int dirfd = translate_at_fd(a0);
        struct stat st;
        char resolved[PATH_MAX];
        bool follow = ((a3 & 0x100U /* AT_SYMLINK_NOFOLLOW */) == 0U);
        if (resolve_guest_path_at(ctx, dirfd, a1, resolved, sizeof(resolved), follow) < 0) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        if (fstatat(dirfd, resolved, &st, (int)a3) != 0) {
            ctx->regs[0] = (uint32_t)(-errno);
        } else {
            fill_arm_stat64((arm_stat64_t *)a32_guest_ptr(ctx, a2), &st);
            ctx->regs[0] = 0U;
        }
        break;
    }

    case ARM_NR_access: {
        char resolved[PATH_MAX];
        if (resolve_guest_path_ex(ctx, a0, resolved, sizeof(resolved), true) < 0) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        ctx->regs[0] = host_ret(faccessat(AT_FDCWD, resolved, (int)a1, 0));
        break;
    }

    case ARM_NR_faccessat:
    case ARM_NR_faccessat2: {
        int dirfd = translate_at_fd(a0);
        char resolved[PATH_MAX];
        bool follow = (nr == ARM_NR_faccessat) || ((a3 & 0x100U /* AT_SYMLINK_NOFOLLOW */) == 0U);
        if (resolve_guest_path_at(ctx, dirfd, a1, resolved, sizeof(resolved), follow) < 0) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        int fa_flags = (nr == ARM_NR_faccessat2) ? (int)(a3 & 0x100U) : 0;
        ctx->regs[0] = host_ret(faccessat(dirfd, resolved, (int)a2, fa_flags));
        break;
    }

    case ARM_NR_getcwd: {
        if (a0 < 0x1000U) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        char *buf = (char *)a32_guest_ptr(ctx, a0);
        if (ctx->sysroot != NULL && ctx->sysroot[0] != '\0') {
            char host_cwd[PATH_MAX];
            long r = syscall(SYS_getcwd, host_cwd, sizeof(host_cwd));
            if (r < 0) {
                ctx->regs[0] = (uint32_t)(-errno);
                break;
            }
            size_t sr_len = strlen(ctx->sysroot);
            while (sr_len > 1U && ctx->sysroot[sr_len - 1U] == '/') {
                sr_len--;
            }
            const char *guest_cwd = host_cwd;
            if (sr_len > 1U && strncmp(host_cwd, ctx->sysroot, sr_len) == 0 &&
                (host_cwd[sr_len] == '/' || host_cwd[sr_len] == '\0')) {
                guest_cwd = (host_cwd[sr_len] == '\0') ? "/" : (host_cwd + sr_len);
            }
            size_t len = strlen(guest_cwd);
            if ((size_t)a1 < len + 1U) {
                ctx->regs[0] = (uint32_t)(-ERANGE);
                break;
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
        if ((a0 != 0U && a0 < 0x1000U) ||
            (a1 != 0U && a1 < 0x1000U) ||
            (a2 != 0U && a2 < 0x1000U)) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
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
        if ((a0 != 0U && a0 < 0x1000U) ||
            (a1 != 0U && a1 < 0x1000U) ||
            (a2 != 0U && a2 < 0x1000U)) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
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
        char resolved[PATH_MAX];
        if (resolve_guest_path_ex(ctx, a0, resolved, sizeof(resolved), true) < 0) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        ctx->regs[0] = host_ret(fchownat(AT_FDCWD, resolved, (uid_t)a1, (gid_t)a2, 0));
        break;
    }

    case ARM_NR_lchown32: {
        char resolved[PATH_MAX];
        if (resolve_guest_path_ex(ctx, a0, resolved, sizeof(resolved), false) < 0) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        ctx->regs[0] = host_ret(fchownat(AT_FDCWD, resolved, (uid_t)a1, (gid_t)a2, AT_SYMLINK_NOFOLLOW));
        break;
    }

    case ARM_NR_getgroups32:
        if ((int32_t)a0 < 0) {
            ctx->regs[0] = (uint32_t)(-EINVAL);
            break;
        }
        if (a0 > 0U && a1 < 0x1000U) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        ctx->regs[0] = host_ret(getgroups((int)a0, (gid_t *)a32_guest_ptr(ctx, a1)));
        break;

    case ARM_NR_setgroups32:
        if (a0 > 0U && a1 < 0x1000U) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        ctx->regs[0] = host_ret(setgroups((size_t)a0, (const gid_t *)a32_guest_ptr(ctx, a1)));
        break;

    case ARM_NR_set_tid_address:
        ctx->clear_child_tid = a0;
        ctx->regs[0] = (uint32_t)syscall(SYS_gettid);
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
    case ARM_NR_rt_sigreturn: {
        int rc = a32_sys_rt_sigreturn(ctx);
        if (rc < 0) {
            ctx->regs[0] = (uint32_t)rc;
        }
        break;
    }

    case ARM_NR_getrlimit:
    case ARM_NR_ugetrlimit: {
        if (a1 < 0x1000U) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
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

    case ARM_NR_setrlimit: {
        if (a1 < 0x1000U) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        arm_rlimit32_t rl32;
        memcpy(&rl32, a32_guest_ptr(ctx, a1), sizeof(rl32));
        struct rlimit rl;
        rl.rlim_cur = (rl32.rlim_cur == 0xFFFFFFFFU) ? RLIM_INFINITY : (rlim_t)rl32.rlim_cur;
        rl.rlim_max = (rl32.rlim_max == 0xFFFFFFFFU) ? RLIM_INFINITY : (rlim_t)rl32.rlim_max;
        ctx->regs[0] = host_ret(setrlimit((int)a0, &rl));
        break;
    }

    case ARM_NR_prlimit64:
        if ((a2 != 0U && a2 < 0x1000U) || (a3 != 0U && a3 < 0x1000U)) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        ctx->regs[0] = host_ret(syscall(SYS_prlimit64, (pid_t)a0, (int)a1,
                                        a2 ? a32_guest_ptr(ctx, a2) : NULL,
                                        a3 ? a32_guest_ptr(ctx, a3) : NULL));
        break;

    case ARM_NR_getrusage: {
        if (a1 < 0x1000U) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
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
        if (a0 < 0x1000U) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
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
        if (a1 != sizeof(arm_statfs64_t) && a1 != 88U) {
            ctx->regs[0] = (uint32_t)(-EINVAL);
            break;
        }
        if (a2 < 0x1000U) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        char resolved[PATH_MAX];
        if (resolve_guest_path_ex(ctx, a0, resolved, sizeof(resolved), true) < 0) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        struct statfs sfs;
        if (statfs(resolved, &sfs) != 0) {
            ctx->regs[0] = (uint32_t)(-errno);
        } else {
            fill_arm_statfs64((arm_statfs64_t *)a32_guest_ptr(ctx, a2), &sfs);
            ctx->regs[0] = 0U;
        }
        break;
    }

    case ARM_NR_fstatfs64: {
        if (a1 != sizeof(arm_statfs64_t) && a1 != 88U) {
            ctx->regs[0] = (uint32_t)(-EINVAL);
            break;
        }
        if (a2 < 0x1000U) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
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
        if (a1 > 0U && a0 < 0x1000U) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        ctx->regs[0] = host_ret(getrandom(a32_guest_ptr(ctx, a0), (size_t)a1, (unsigned int)a2));
        break;

    case ARM_NR_clock_gettime32: {
        if (a1 < 0x1000U) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
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
        if (a1 < 0x1000U) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
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
        if (a1 != 0U && a1 < 0x1000U) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
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
        if (a1 != 0U && a1 < 0x1000U) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
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
        if ((a0 != 0U && a0 < 0x1000U) || (a1 != 0U && a1 < 0x1000U)) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
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
        if (a1 < 0x1000U) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
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
                d->d_off = map_dir_cookie((int)a0, (uint64_t)d->d_off);
                pos += d->d_reclen;
            }
        }
        ctx->regs[0] = (uint32_t)r;
        break;
    }

    case ARM_NR_getdents: {
        if (a1 < 0x1000U) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        size_t host_buf_size = (a2 > 32768U) ? 65536U : (size_t)a2 * 2U;
        if (host_buf_size < 1024U) {
            host_buf_size = 1024U;
        }
        uint8_t *host_buf = (uint8_t *)malloc(host_buf_size);
        if (!host_buf) {
            ctx->regs[0] = (uint32_t)(-ENOMEM);
            break;
        }
        off_t last_off = lseek((int)a0, 0, SEEK_CUR);
        long nread = syscall(SYS_getdents64, (int)a0, host_buf, host_buf_size);
        if (nread < 0) {
            free(host_buf);
            ctx->regs[0] = (uint32_t)(-errno);
            break;
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
            ctx->regs[0] = (uint32_t)(-errno);
        } else if (pid == 0) {
            ctx->host_tid = (int)syscall(SYS_gettid);
            ctx->is_thread_clone = false;
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
        bool is_thread = ((flags & 0x00000100U /* CLONE_VM */) != 0U &&
                          (flags & 0x00004000U /* CLONE_VFORK */) == 0U);
        if (is_thread) {
            a32_ctx_t *child_ctx = (a32_ctx_t *)calloc(1, sizeof(a32_ctx_t));
            if (!child_ctx) {
                ctx->regs[0] = (uint32_t)(-ENOMEM);
                break;
            }
            a32_vm_sync_ctx(ctx);
            memcpy(child_ctx, ctx, sizeof(a32_ctx_t));
            child_ctx->regs[0]          = 0U;
            child_ctx->regs[13]         = a1 ? a1 : ctx->regs[13];
            child_ctx->clear_child_tid  = 0U;
            child_ctx->robust_list_head = 0U;
            child_ctx->robust_list_len  = 0U;
            child_ctx->pending_signals  = 0ULL;
            child_ctx->altstack_sp      = 0U;
            child_ctx->altstack_size    = 0U;
            child_ctx->altstack_flags   = 2U; /* SS_DISABLE */
            child_ctx->ras_top          = 0U;
            memset(child_ctx->ras_key, 0, sizeof(child_ctx->ras_key));
            memset(child_ctx->ibtc, 0, sizeof(child_ctx->ibtc));

            if (flags & 0x00080000U /* CLONE_SETTLS */) {
                child_ctx->tls_tp = a3;
            }
            if (flags & 0x00200000U /* CLONE_CHILD_CLEARTID */) {
                child_ctx->clear_child_tid = a4;
            }

            child_thread_args_t targs;
            targs.child_ctx = child_ctx;
            targs.flags     = flags;
            targs.ptid_va   = a2;
            targs.ctid_va   = a4;
            targs.child_tid = 0;
            targs.started   = 0;

            pthread_t pt;
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

            int prc = pthread_create(&pt, &attr, guest_thread_start, &targs);
            pthread_attr_destroy(&attr);
            if (prc != 0) {
                free(child_ctx);
                ctx->regs[0] = (uint32_t)(-prc);
                break;
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
            ctx->regs[0] = (uint32_t)(-errno);
        } else if (pid == 0) {
            ctx->host_tid = (int)syscall(SYS_gettid);
            ctx->is_thread_clone = false;
            ctx->pending_signals = 0ULL;
            a32_signals_set_thread_ctx(ctx);
            if (a1 != 0U) {
                ctx->regs[13] = a1;
            }
            if (flags & 0x00080000U /* CLONE_SETTLS */) {
                ctx->tls_tp = a3;
            }
            if ((flags & 0x00100000U /* CLONE_PARENT_SETTID */) && a2 >= 0x1000U) {
                *(uint32_t *)a32_guest_ptr(ctx, a2) = (uint32_t)ctx->host_tid;
            }
            if ((flags & 0x01000000U /* CLONE_CHILD_SETTID */) && a4 >= 0x1000U) {
                *(uint32_t *)a32_guest_ptr(ctx, a4) = (uint32_t)ctx->host_tid;
            }
            if (flags & 0x00200000U /* CLONE_CHILD_CLEARTID */) {
                ctx->clear_child_tid = a4;
            } else {
                ctx->clear_child_tid = 0U;
            }
            ctx->regs[0] = 0U;
        } else {
            if ((flags & 0x00100000U /* CLONE_PARENT_SETTID */) && a2 >= 0x1000U) {
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

        if (path_va < 0x1000U ||
            (argv_va != 0U && argv_va < 0x1000U) ||
            (envp_va != 0U && envp_va < 0x1000U)) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }

        const char *guest_orig_path = (const char *)a32_guest_ptr(ctx, path_va);
        bool follow = ((ex_flags & 0x100U /* AT_SYMLINK_NOFOLLOW */) == 0U);
        bool is_empty_path_fd = false;
        char resolved[PATH_MAX];
        if ((ex_flags & 0x1000U /* AT_EMPTY_PATH */) != 0U &&
            guest_orig_path[0] == '\0' && dirfd >= 0) {
            if (fcntl(dirfd, F_GETFD) < 0) {
                ctx->regs[0] = (uint32_t)(-EBADF);
                break;
            }
            is_empty_path_fd = true;
            snprintf(resolved, sizeof(resolved), "/proc/self/fd/%d", dirfd);
        } else {
            if (resolve_guest_path_at(ctx, dirfd, path_va, resolved, sizeof(resolved), follow) < 0) {
                ctx->regs[0] = (uint32_t)(-EFAULT);
                break;
            }
            if (!follow) {
                struct stat st_l;
                if (lstat(resolved, &st_l) == 0 && S_ISLNK(st_l.st_mode)) {
                    ctx->regs[0] = (uint32_t)(-ELOOP);
                    break;
                }
            }
        }
        const char *path = resolved;

        char *new_argv[512];
        int argc_count = 0;
        bool bad_ptr = false;
        if (argv_va != 0U) {
            uint32_t *g_argv = (uint32_t *)a32_guest_ptr(ctx, argv_va);
            while (g_argv[argc_count] != 0U && argc_count < 500) {
                if (g_argv[argc_count] < 0x1000U) {
                    bad_ptr = true;
                    break;
                }
                new_argv[argc_count] = (char *)a32_guest_ptr(ctx, g_argv[argc_count]);
                argc_count++;
            }
        }
        if (bad_ptr) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        new_argv[argc_count] = NULL;

        char *new_envp[512];
        int envc_count = 0;
        if (envp_va != 0U) {
            uint32_t *g_envp = (uint32_t *)a32_guest_ptr(ctx, envp_va);
            while (g_envp[envc_count] != 0U && envc_count < 510) {
                if (g_envp[envc_count] < 0x1000U) {
                    bad_ptr = true;
                    break;
                }
                new_envp[envc_count] = (char *)a32_guest_ptr(ctx, g_envp[envc_count]);
                envc_count++;
            }
        }
        if (bad_ptr) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        new_envp[envc_count] = NULL;
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
        ctx->regs[0] = (uint32_t)(-errno);
        break;
    }

    case ARM_NR_wait4: {
        if ((a1 != 0U && a1 < 0x1000U) || (a3 != 0U && a3 < 0x1000U)) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
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
        if ((a2 != 0U && a2 < 0x1000U) || (a4 != 0U && a4 < 0x1000U)) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        siginfo_t si;
        struct rusage ru;
        memset(&si, 0, sizeof(si));
        memset(&ru, 0, sizeof(ru));
        long r = syscall(SYS_waitid, (int)a0, (id_t)a1,
                         (a2 != 0U) ? &si : NULL, (int)a3,
                         (a4 != 0U) ? &ru : NULL);
        if (r < 0) {
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
            if (a4 != 0U) {
                fill_arm_rusage32((arm_rusage32_t *)a32_guest_ptr(ctx, a4), &ru);
            }
            ctx->regs[0] = 0U;
        }
        break;
    }

    case ARM_NR_futex: {
        if (a0 < 0x1000U) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        void *uaddr = a32_guest_ptr(ctx, a0);
        int op = (int)a1;
        uint32_t val = a2;
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
                if (a3 < 0x1000U) {
                    ctx->regs[0] = (uint32_t)(-EFAULT);
                    break;
                }
                const arm_timespec32_t *ts32 = (const arm_timespec32_t *)a32_guest_ptr(ctx, a3);
                host_ts.tv_sec  = (time_t)ts32->tv_sec;
                host_ts.tv_nsec = (long)ts32->tv_nsec;
                arg4 = &host_ts;
            }
        } else {
            arg4 = (const void *)(uintptr_t)a3;
        }

        void *uaddr2 = (a4 != 0U) ? a32_guest_ptr(ctx, a4) : NULL;
        long r = syscall(SYS_futex, uaddr, op, val, arg4, uaddr2, a5);
        ctx->regs[0] = host_ret(r);
        break;
    }

    case ARM_NR_futex_time64: {
        if (a0 < 0x1000U) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        void *uaddr = a32_guest_ptr(ctx, a0);
        int op = (int)a1;
        uint32_t val = a2;
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
                if (a3 < 0x1000U) {
                    ctx->regs[0] = (uint32_t)(-EFAULT);
                    break;
                }
                const arm_timespec64_t *ts64 = (const arm_timespec64_t *)a32_guest_ptr(ctx, a3);
                host_ts.tv_sec  = (time_t)ts64->tv_sec;
                host_ts.tv_nsec = (long)ts64->tv_nsec;
                arg4 = &host_ts;
            }
        } else {
            arg4 = (const void *)(uintptr_t)a3;
        }

        void *uaddr2 = (a4 != 0U) ? a32_guest_ptr(ctx, a4) : NULL;
        long r = syscall(SYS_futex, uaddr, op, val, arg4, uaddr2, a5);
        ctx->regs[0] = host_ret(r);
        break;
    }

    case ARM_NR_set_robust_list:
        if (a1 != 12U) {
            ctx->regs[0] = (uint32_t)(-EINVAL);
            break;
        }
        ctx->robust_list_head = a0;
        ctx->robust_list_len  = a1;
        ctx->regs[0] = 0U;
        break;

    case ARM_NR_get_robust_list:
        if (a1 < 0x1000U || a2 < 0x1000U) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
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
        if (a2 > 0U && a1 < 0x1000U) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        off_t pos = (off_t)(((uint64_t)a5 << 32) | (uint64_t)a4);
        ctx->regs[0] = host_ret(pread((int)a0, a32_guest_ptr(ctx, a1), (size_t)a2, pos));
        break;
    }

    case ARM_NR_pwrite64: {
        if (a2 > 0U && a1 < 0x1000U) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        off_t pos = (off_t)(((uint64_t)a5 << 32) | (uint64_t)a4);
        ctx->regs[0] = host_ret(pwrite((int)a0, a32_guest_ptr(ctx, a1), (size_t)a2, pos));
        break;
    }

    case ARM_NR_truncate64: {
        char resolved[PATH_MAX];
        if (resolve_guest_path_ex(ctx, a0, resolved, sizeof(resolved), true) < 0) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        off_t len = (off_t)(((uint64_t)a3 << 32) | (uint64_t)a2);
        ctx->regs[0] = host_ret(truncate(resolved, len));
        break;
    }

    case ARM_NR_ftruncate64: {
        off_t len = (off_t)(((uint64_t)a3 << 32) | (uint64_t)a2);
        ctx->regs[0] = host_ret(ftruncate((int)a0, len));
        break;
    }

    case ARM_NR_sendfile64: {
        if (a2 != 0U && a2 < 0x1000U) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        off_t *off_ptr = a2 ? (off_t *)a32_guest_ptr(ctx, a2) : NULL;
        ctx->regs[0] = host_ret(sendfile((int)a0, (int)a1, off_ptr, (size_t)a3));
        break;
    }

    case ARM_NR_statx: {
        char resolved[PATH_MAX];
        bool follow = ((a2 & 0x100U /* AT_SYMLINK_NOFOLLOW */) == 0U);
        int dirfd = translate_at_fd(a0);
        if (a4 < 0x1000U ||
            resolve_guest_path_at(ctx, dirfd, a1, resolved, sizeof(resolved), follow) < 0) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        ctx->regs[0] = host_ret(syscall(SYS_statx, dirfd, resolved,
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
        if (a3 < 0x1000U) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
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

    case ARM_NR_setsockopt: {
        if (a3 != 0U && a3 < 0x1000U) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        if (a1 == 1U /* SOL_SOCKET */ &&
            (a2 == 20U /* SO_RCVTIMEO_OLD */ || a2 == 21U /* SO_SNDTIMEO_OLD */) &&
            a3 != 0U && a4 == sizeof(arm_timeval32_t)) {
            arm_timeval32_t tv32;
            memcpy(&tv32, a32_guest_ptr(ctx, a3), sizeof(tv32));
            struct timeval tv64;
            tv64.tv_sec  = (time_t)tv32.tv_sec;
            tv64.tv_usec = (suseconds_t)tv32.tv_usec;
            ctx->regs[0] = host_ret(setsockopt((int)a0, (int)a1, (int)a2,
                                               &tv64, (socklen_t)sizeof(tv64)));
            break;
        }
        ctx->regs[0] = host_ret(setsockopt((int)a0, (int)a1, (int)a2,
                                          a3 ? a32_guest_ptr(ctx, a3) : NULL, (socklen_t)a4));
        break;
    }

    case ARM_NR_getsockopt: {
        if ((a3 != 0U && a3 < 0x1000U) || (a4 != 0U && a4 < 0x1000U)) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        if (a1 == 1U /* SOL_SOCKET */ &&
            (a2 == 20U /* SO_RCVTIMEO_OLD */ || a2 == 21U /* SO_SNDTIMEO_OLD */) &&
            a3 != 0U && a4 != 0U) {
            socklen_t guest_len = *(socklen_t *)a32_guest_ptr(ctx, a4);
            if (guest_len == sizeof(arm_timeval32_t)) {
                struct timeval tv64;
                memset(&tv64, 0, sizeof(tv64));
                socklen_t host_len = (socklen_t)sizeof(tv64);
                int r = getsockopt((int)a0, (int)a1, (int)a2, &tv64, &host_len);
                if (r == 0) {
                    arm_timeval32_t tv32;
                    tv32.tv_sec  = (int32_t)tv64.tv_sec;
                    tv32.tv_usec = (int32_t)tv64.tv_usec;
                    memcpy(a32_guest_ptr(ctx, a3), &tv32, sizeof(tv32));
                    *(socklen_t *)a32_guest_ptr(ctx, a4) = (socklen_t)sizeof(tv32);
                }
                ctx->regs[0] = host_ret(r);
                break;
            }
        }
        ctx->regs[0] = host_ret(getsockopt((int)a0, (int)a1, (int)a2,
                                          a3 ? a32_guest_ptr(ctx, a3) : NULL,
                                          a4 ? (socklen_t *)a32_guest_ptr(ctx, a4) : NULL));
        break;
    }

    case ARM_NR_sendmsg:
    case ARM_NR_recvmsg: {
        if (a1 < 0x1000U) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        arm_msghdr32_t *gm = (arm_msghdr32_t *)a32_guest_ptr(ctx, a1);
        struct msghdr hm;
        memset(&hm, 0, sizeof(hm));
        hm.msg_name    = (gm->msg_name != 0U) ? a32_guest_ptr(ctx, gm->msg_name) : NULL;
        hm.msg_namelen = (socklen_t)gm->msg_namelen;
        hm.msg_flags   = (int)gm->msg_flags;

        _Alignas(8) uint8_t cbuf64[1024];
        memset(cbuf64, 0, sizeof(cbuf64));
        if (gm->msg_control != 0U && gm->msg_controllen > 0U) {
            if (gm->msg_control < 0x1000U) {
                ctx->regs[0] = (uint32_t)(-EFAULT);
                break;
            }
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

        if (gm->msg_iovlen > 0U && gm->msg_iov < 0x1000U) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
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

    /* Select, polling, epoll, eventfd, timerfd */
    case ARM_NR__newselect:
    case ARM_NR_pselect6:
    case ARM_NR_pselect6_time64: {
        int nfds = (int)a0;
        if (nfds < 0) {
            ctx->regs[0] = (uint32_t)(-EINVAL);
            break;
        }
        if ((a1 != 0U && a1 < 0x1000U) ||
            (a2 != 0U && a2 < 0x1000U) ||
            (a3 != 0U && a3 < 0x1000U)) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        struct timespec host_ts;
        struct timespec *ts_ptr = NULL;
        if (a4 != 0U) {
            if (a4 < 0x1000U) {
                ctx->regs[0] = (uint32_t)(-EFAULT);
                break;
            }
            if (nr == ARM_NR__newselect) {
                const arm_timeval32_t *tv32 = (const arm_timeval32_t *)a32_guest_ptr(ctx, a4);
                if (tv32->tv_sec < 0 || tv32->tv_usec < 0 || tv32->tv_usec >= 1000000) {
                    ctx->regs[0] = (uint32_t)(-EINVAL);
                    break;
                }
                host_ts.tv_sec  = (time_t)tv32->tv_sec;
                host_ts.tv_nsec = (long)tv32->tv_usec * 1000L;
            } else if (nr == ARM_NR_pselect6) {
                const arm_timespec32_t *ts32 = (const arm_timespec32_t *)a32_guest_ptr(ctx, a4);
                host_ts.tv_sec  = (time_t)ts32->tv_sec;
                host_ts.tv_nsec = (long)ts32->tv_nsec;
            } else {
                const arm_timespec64_t *ts64 = (const arm_timespec64_t *)a32_guest_ptr(ctx, a4);
                host_ts.tv_sec  = (time_t)ts64->tv_sec;
                host_ts.tv_nsec = (long)ts64->tv_nsec;
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
            if (a5 < 0x1000U) {
                ctx->regs[0] = (uint32_t)(-EFAULT);
                break;
            }
            const uint32_t *g_pack = (const uint32_t *)a32_guest_ptr(ctx, a5);
            if (g_pack[0] != 0U && g_pack[0] < 0x1000U) {
                ctx->regs[0] = (uint32_t)(-EFAULT);
                break;
            }
            if (g_pack[0] != 0U && g_pack[1] != 8U) {
                ctx->regs[0] = (uint32_t)(-EINVAL);
                break;
            }
            if (g_pack[0] != 0U) {
                memcpy(&temp_mask, a32_guest_ptr(ctx, g_pack[0]), sizeof(temp_mask));
                temp_mask &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
                has_temp_mask = true;
                ctx->sig_mask = temp_mask;
            }
            host_sig_pack.ss     = (g_pack[0] != 0U) ? a32_guest_ptr(ctx, g_pack[0]) : NULL;
            host_sig_pack.ss_len = (size_t)g_pack[1];
            sig_arg = &host_sig_pack;
        }

        if (has_temp_mask &&
            (__atomic_load_n(&ctx->pending_signals, __ATOMIC_SEQ_CST) & ~temp_mask) != 0ULL) {
            uint32_t orig_r0 = ctx->regs[0];
            ctx->regs[0] = (uint32_t)(-EINTR);
            check_pwait_signals(ctx, temp_mask);
            if (ctx->exit_reason == A32_EXIT_BRANCH) {
                restore_pwait_sigmask(ctx, saved_sig_mask, temp_mask);
                break;
            }
            ctx->sig_mask = temp_mask;
            ctx->regs[0]  = orig_r0;
        }

        uint64_t rset[16], wset[16], eset[16];
        void *r_ptr = NULL;
        void *w_ptr = NULL;
        void *e_ptr = NULL;
        size_t fd_bytes32 = (((size_t)nfds + 31U) / 32U) * 4U;
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
        if (a4 != 0U && (r >= 0 || errno == EINTR)) {
            if (nr == ARM_NR__newselect) {
                arm_timeval32_t *tv32 = (arm_timeval32_t *)a32_guest_ptr(ctx, a4);
                tv32->tv_sec  = (int32_t)host_ts.tv_sec;
                tv32->tv_usec = (int32_t)(host_ts.tv_nsec / 1000L);
            } else if (nr == ARM_NR_pselect6) {
                arm_timespec32_t *ts32 = (arm_timespec32_t *)a32_guest_ptr(ctx, a4);
                ts32->tv_sec  = (int32_t)host_ts.tv_sec;
                ts32->tv_nsec = (int32_t)host_ts.tv_nsec;
            } else {
                arm_timespec64_t *ts64 = (arm_timespec64_t *)a32_guest_ptr(ctx, a4);
                ts64->tv_sec  = (int64_t)host_ts.tv_sec;
                ts64->tv_nsec = (int64_t)host_ts.tv_nsec;
            }
        }
        ctx->regs[0] = host_ret(r);
        if (has_temp_mask) {
            check_pwait_signals(ctx, temp_mask);
            restore_pwait_sigmask(ctx, saved_sig_mask, temp_mask);
        }
        break;
    }

    case ARM_NR_poll:
        if (a1 > 0U && a0 < 0x1000U) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        ctx->regs[0] = host_ret(poll((a1 > 0U) ? (struct pollfd *)a32_guest_ptr(ctx, a0) : NULL,
                                    (nfds_t)a1, (int)a2));
        break;

    case ARM_NR_ppoll: {
        if ((a1 > 0U && a0 < 0x1000U) ||
            (a2 != 0U && a2 < 0x1000U) ||
            (a3 != 0U && a3 < 0x1000U)) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        if (a3 != 0U && a4 != 8U) {
            ctx->regs[0] = (uint32_t)(-EINVAL);
            break;
        }
        struct timespec host_ts;
        struct timespec *ts_ptr = NULL;
        if (a2 != 0U) {
            const arm_timespec32_t *ts32 = (const arm_timespec32_t *)a32_guest_ptr(ctx, a2);
            host_ts.tv_sec  = (time_t)ts32->tv_sec;
            host_ts.tv_nsec = (long)ts32->tv_nsec;
            ts_ptr = &host_ts;
        }
        bool     has_temp_mask  = false;
        uint64_t saved_sig_mask = ctx->sig_mask;
        uint64_t temp_mask      = 0ULL;
        if (a3 != 0U) {
            memcpy(&temp_mask, a32_guest_ptr(ctx, a3), sizeof(temp_mask));
            temp_mask &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
            has_temp_mask = true;
            ctx->sig_mask = temp_mask;
        }
        if (has_temp_mask &&
            (__atomic_load_n(&ctx->pending_signals, __ATOMIC_SEQ_CST) & ~temp_mask) != 0ULL) {
            uint32_t orig_r0 = ctx->regs[0];
            ctx->regs[0] = (uint32_t)(-EINTR);
            check_pwait_signals(ctx, temp_mask);
            if (ctx->exit_reason == A32_EXIT_BRANCH) {
                restore_pwait_sigmask(ctx, saved_sig_mask, temp_mask);
                break;
            }
            ctx->sig_mask = temp_mask;
            ctx->regs[0]  = orig_r0;
        }
        sigset_t *sig_ptr = a3 ? (sigset_t *)a32_guest_ptr(ctx, a3) : NULL;
        ctx->regs[0] = host_ret(ppoll((a1 > 0U) ? (struct pollfd *)a32_guest_ptr(ctx, a0) : NULL,
                                     (nfds_t)a1, ts_ptr, sig_ptr));
        if (has_temp_mask) {
            check_pwait_signals(ctx, temp_mask);
            restore_pwait_sigmask(ctx, saved_sig_mask, temp_mask);
        }
        break;
    }

    case ARM_NR_ppoll_time64: {
        if ((a1 > 0U && a0 < 0x1000U) ||
            (a2 != 0U && a2 < 0x1000U) ||
            (a3 != 0U && a3 < 0x1000U)) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        if (a3 != 0U && a4 != 8U) {
            ctx->regs[0] = (uint32_t)(-EINVAL);
            break;
        }
        struct timespec host_ts;
        struct timespec *ts_ptr = NULL;
        if (a2 != 0U) {
            const arm_timespec64_t *ts64 = (const arm_timespec64_t *)a32_guest_ptr(ctx, a2);
            host_ts.tv_sec  = (time_t)ts64->tv_sec;
            host_ts.tv_nsec = (long)ts64->tv_nsec;
            ts_ptr = &host_ts;
        }
        bool     has_temp_mask  = false;
        uint64_t saved_sig_mask = ctx->sig_mask;
        uint64_t temp_mask      = 0ULL;
        if (a3 != 0U) {
            memcpy(&temp_mask, a32_guest_ptr(ctx, a3), sizeof(temp_mask));
            temp_mask &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
            has_temp_mask = true;
            ctx->sig_mask = temp_mask;
        }
        if (has_temp_mask &&
            (__atomic_load_n(&ctx->pending_signals, __ATOMIC_SEQ_CST) & ~temp_mask) != 0ULL) {
            uint32_t orig_r0 = ctx->regs[0];
            ctx->regs[0] = (uint32_t)(-EINTR);
            check_pwait_signals(ctx, temp_mask);
            if (ctx->exit_reason == A32_EXIT_BRANCH) {
                restore_pwait_sigmask(ctx, saved_sig_mask, temp_mask);
                break;
            }
            ctx->sig_mask = temp_mask;
            ctx->regs[0]  = orig_r0;
        }
        sigset_t *sig_ptr = a3 ? (sigset_t *)a32_guest_ptr(ctx, a3) : NULL;
        ctx->regs[0] = host_ret(ppoll((a1 > 0U) ? (struct pollfd *)a32_guest_ptr(ctx, a0) : NULL,
                                     (nfds_t)a1, ts_ptr, sig_ptr));
        if (has_temp_mask) {
            check_pwait_signals(ctx, temp_mask);
            restore_pwait_sigmask(ctx, saved_sig_mask, temp_mask);
        }
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
                                         a3 ? (struct epoll_event *)a32_guest_ptr(ctx, a3) : NULL));
        break;

    case ARM_NR_epoll_wait:
        if (a1 < 0x1000U) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        ctx->regs[0] = host_ret(epoll_wait((int)a0, (struct epoll_event *)a32_guest_ptr(ctx, a1),
                                          (int)a2, (int)a3));
        break;

    case ARM_NR_epoll_pwait: {
        if (a1 < 0x1000U || (a4 != 0U && a4 < 0x1000U)) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        if (a4 != 0U && a5 != 8U) {
            ctx->regs[0] = (uint32_t)(-EINVAL);
            break;
        }
        bool     has_temp_mask  = false;
        uint64_t saved_sig_mask = ctx->sig_mask;
        uint64_t temp_mask      = 0ULL;
        if (a4 != 0U) {
            memcpy(&temp_mask, a32_guest_ptr(ctx, a4), sizeof(temp_mask));
            temp_mask &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
            has_temp_mask = true;
            ctx->sig_mask = temp_mask;
        }
        if (has_temp_mask &&
            (__atomic_load_n(&ctx->pending_signals, __ATOMIC_SEQ_CST) & ~temp_mask) != 0ULL) {
            uint32_t orig_r0 = ctx->regs[0];
            ctx->regs[0] = (uint32_t)(-EINTR);
            check_pwait_signals(ctx, temp_mask);
            if (ctx->exit_reason == A32_EXIT_BRANCH) {
                restore_pwait_sigmask(ctx, saved_sig_mask, temp_mask);
                break;
            }
            ctx->sig_mask = temp_mask;
            ctx->regs[0]  = orig_r0;
        }
        ctx->regs[0] = host_ret(epoll_pwait((int)a0, (struct epoll_event *)a32_guest_ptr(ctx, a1),
                                           (int)a2, (int)a3,
                                           a4 ? (const sigset_t *)a32_guest_ptr(ctx, a4) : NULL));
        if (has_temp_mask) {
            check_pwait_signals(ctx, temp_mask);
            restore_pwait_sigmask(ctx, saved_sig_mask, temp_mask);
        }
        break;
    }

    case ARM_NR_eventfd:
        ctx->regs[0] = host_ret(eventfd((unsigned int)a0, 0));
        break;

    case ARM_NR_eventfd2:
        ctx->regs[0] = host_ret(eventfd((unsigned int)a0, (int)a1));
        break;

    case ARM_NR_timerfd_create:
        ctx->regs[0] = host_ret(timerfd_create((int)a0, (int)a1));
        break;

    case ARM_NR_timerfd_settime: {
        if (a2 < 0x1000U || (a3 != 0U && a3 < 0x1000U)) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        arm_itimerspec32_t g_in;
        memcpy(&g_in, a32_guest_ptr(ctx, a2), sizeof(g_in));
        struct itimerspec h_in, h_out;
        memset(&h_out, 0, sizeof(h_out));
        h_in.it_interval.tv_sec  = (time_t)g_in.it_interval.tv_sec;
        h_in.it_interval.tv_nsec = (long)g_in.it_interval.tv_nsec;
        h_in.it_value.tv_sec     = (time_t)g_in.it_value.tv_sec;
        h_in.it_value.tv_nsec    = (long)g_in.it_value.tv_nsec;
        int r = timerfd_settime((int)a0, (int)a1, &h_in, (a3 != 0U) ? &h_out : NULL);
        if (r == 0 && a3 != 0U) {
            arm_itimerspec32_t g_out;
            g_out.it_interval.tv_sec  = (int32_t)h_out.it_interval.tv_sec;
            g_out.it_interval.tv_nsec = (int32_t)h_out.it_interval.tv_nsec;
            g_out.it_value.tv_sec     = (int32_t)h_out.it_value.tv_sec;
            g_out.it_value.tv_nsec    = (int32_t)h_out.it_value.tv_nsec;
            memcpy(a32_guest_ptr(ctx, a3), &g_out, sizeof(g_out));
        }
        ctx->regs[0] = host_ret(r);
        break;
    }

    case ARM_NR_timerfd_settime64: {
        if (a2 < 0x1000U || (a3 != 0U && a3 < 0x1000U)) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        arm_itimerspec64_t g_in;
        memcpy(&g_in, a32_guest_ptr(ctx, a2), sizeof(g_in));
        struct itimerspec h_in, h_out;
        memset(&h_out, 0, sizeof(h_out));
        h_in.it_interval.tv_sec  = (time_t)g_in.it_interval.tv_sec;
        h_in.it_interval.tv_nsec = (long)g_in.it_interval.tv_nsec;
        h_in.it_value.tv_sec     = (time_t)g_in.it_value.tv_sec;
        h_in.it_value.tv_nsec    = (long)g_in.it_value.tv_nsec;
        int r = timerfd_settime((int)a0, (int)a1, &h_in, (a3 != 0U) ? &h_out : NULL);
        if (r == 0 && a3 != 0U) {
            arm_itimerspec64_t g_out;
            g_out.it_interval.tv_sec  = (int64_t)h_out.it_interval.tv_sec;
            g_out.it_interval.tv_nsec = (int64_t)h_out.it_interval.tv_nsec;
            g_out.it_value.tv_sec     = (int64_t)h_out.it_value.tv_sec;
            g_out.it_value.tv_nsec    = (int64_t)h_out.it_value.tv_nsec;
            memcpy(a32_guest_ptr(ctx, a3), &g_out, sizeof(g_out));
        }
        ctx->regs[0] = host_ret(r);
        break;
    }

    case ARM_NR_timerfd_gettime: {
        if (a1 < 0x1000U) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        struct itimerspec h_out;
        memset(&h_out, 0, sizeof(h_out));
        int r = timerfd_gettime((int)a0, &h_out);
        if (r == 0) {
            arm_itimerspec32_t g_out;
            g_out.it_interval.tv_sec  = (int32_t)h_out.it_interval.tv_sec;
            g_out.it_interval.tv_nsec = (int32_t)h_out.it_interval.tv_nsec;
            g_out.it_value.tv_sec     = (int32_t)h_out.it_value.tv_sec;
            g_out.it_value.tv_nsec    = (int32_t)h_out.it_value.tv_nsec;
            memcpy(a32_guest_ptr(ctx, a1), &g_out, sizeof(g_out));
        }
        ctx->regs[0] = host_ret(r);
        break;
    }

    case ARM_NR_timerfd_gettime64: {
        if (a1 < 0x1000U) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        struct itimerspec h_out;
        memset(&h_out, 0, sizeof(h_out));
        int r = timerfd_gettime((int)a0, &h_out);
        if (r == 0) {
            arm_itimerspec64_t g_out;
            g_out.it_interval.tv_sec  = (int64_t)h_out.it_interval.tv_sec;
            g_out.it_interval.tv_nsec = (int64_t)h_out.it_interval.tv_nsec;
            g_out.it_value.tv_sec     = (int64_t)h_out.it_value.tv_sec;
            g_out.it_value.tv_nsec    = (int64_t)h_out.it_value.tv_nsec;
            memcpy(a32_guest_ptr(ctx, a1), &g_out, sizeof(g_out));
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

    case ARM_NR_pipe: {
        if (a0 < 0x1000U) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
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
        if (a0 < 0x1000U) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
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
        char resolved[PATH_MAX];
        if (resolve_guest_path_ex(ctx, a0, resolved, sizeof(resolved), true) < 0) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        ctx->regs[0] = host_ret(chdir(resolved));
        break;
    }

    case ARM_NR_fchdir:
        ctx->regs[0] = host_ret(fchdir((int)a0));
        break;

    case ARM_NR_mkdir: {
        char resolved[PATH_MAX];
        if (resolve_guest_path_ex(ctx, a0, resolved, sizeof(resolved), false) < 0) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        ctx->regs[0] = host_ret(mkdir(resolved, (mode_t)a1));
        break;
    }

    case ARM_NR_mkdirat: {
        char resolved[PATH_MAX];
        int dirfd = translate_at_fd(a0);
        if (resolve_guest_path_at(ctx, dirfd, a1, resolved, sizeof(resolved), false) < 0) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        ctx->regs[0] = host_ret(mkdirat(dirfd, resolved, (mode_t)a2));
        break;
    }

    case ARM_NR_rmdir: {
        char resolved[PATH_MAX];
        if (resolve_guest_path_ex(ctx, a0, resolved, sizeof(resolved), false) < 0) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        ctx->regs[0] = host_ret(rmdir(resolved));
        break;
    }

    case ARM_NR_unlink: {
        char resolved[PATH_MAX];
        if (resolve_guest_path_ex(ctx, a0, resolved, sizeof(resolved), false) < 0) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        ctx->regs[0] = host_ret(unlink(resolved));
        break;
    }

    case ARM_NR_unlinkat: {
        char resolved[PATH_MAX];
        int dirfd = translate_at_fd(a0);
        if (resolve_guest_path_at(ctx, dirfd, a1, resolved, sizeof(resolved), false) < 0) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        ctx->regs[0] = host_ret(unlinkat(dirfd, resolved, (int)a2));
        break;
    }

    case ARM_NR_rename: {
        char r_old[PATH_MAX], r_new[PATH_MAX];
        if (resolve_guest_path_ex(ctx, a0, r_old, sizeof(r_old), false) < 0 ||
            resolve_guest_path_ex(ctx, a1, r_new, sizeof(r_new), false) < 0) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        ctx->regs[0] = host_ret(rename(r_old, r_new));
        break;
    }

    case ARM_NR_renameat: {
        char r_old[PATH_MAX], r_new[PATH_MAX];
        int oldfd = translate_at_fd(a0);
        int newfd = translate_at_fd(a2);
        if (resolve_guest_path_at(ctx, oldfd, a1, r_old, sizeof(r_old), false) < 0 ||
            resolve_guest_path_at(ctx, newfd, a3, r_new, sizeof(r_new), false) < 0) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        ctx->regs[0] = host_ret(renameat(oldfd, r_old, newfd, r_new));
        break;
    }

    case ARM_NR_renameat2: {
        char r_old[PATH_MAX], r_new[PATH_MAX];
        int oldfd = translate_at_fd(a0);
        int newfd = translate_at_fd(a2);
        if (resolve_guest_path_at(ctx, oldfd, a1, r_old, sizeof(r_old), false) < 0 ||
            resolve_guest_path_at(ctx, newfd, a3, r_new, sizeof(r_new), false) < 0) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        ctx->regs[0] = host_ret(syscall(SYS_renameat2, oldfd, r_old,
                                        newfd, r_new, (unsigned int)a4));
        break;
    }

    case ARM_NR_link: {
        char r_old[PATH_MAX], r_new[PATH_MAX];
        if (resolve_guest_path_ex(ctx, a0, r_old, sizeof(r_old), false) < 0 ||
            resolve_guest_path_ex(ctx, a1, r_new, sizeof(r_new), false) < 0) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        ctx->regs[0] = host_ret(link(r_old, r_new));
        break;
    }

    case ARM_NR_linkat: {
        char r_old[PATH_MAX], r_new[PATH_MAX];
        int oldfd = translate_at_fd(a0);
        int newfd = translate_at_fd(a2);
        bool follow_old = ((a4 & 0x400U /* AT_SYMLINK_FOLLOW */) != 0U);
        if (resolve_guest_path_at(ctx, oldfd, a1, r_old, sizeof(r_old), follow_old) < 0 ||
            resolve_guest_path_at(ctx, newfd, a3, r_new, sizeof(r_new), false) < 0) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        ctx->regs[0] = host_ret(linkat(oldfd, r_old, newfd, r_new, (int)a4));
        break;
    }

    case ARM_NR_symlink: {
        char r_link[PATH_MAX];
        if (a0 < 0x1000U ||
            resolve_guest_path_ex(ctx, a1, r_link, sizeof(r_link), false) < 0) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        const char *target = (const char *)a32_guest_ptr(ctx, a0);
        ctx->regs[0] = host_ret(symlink(target, r_link));
        break;
    }

    case ARM_NR_symlinkat: {
        char r_link[PATH_MAX];
        int newfd = translate_at_fd(a1);
        if (a0 < 0x1000U ||
            resolve_guest_path_at(ctx, newfd, a2, r_link, sizeof(r_link), false) < 0) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        const char *target = (const char *)a32_guest_ptr(ctx, a0);
        ctx->regs[0] = host_ret(symlinkat(target, newfd, r_link));
        break;
    }

    case ARM_NR_readlink: {
        if ((int32_t)a2 <= 0) {
            ctx->regs[0] = (uint32_t)(-EINVAL);
            break;
        }
        if (a0 < 0x1000U || a1 < 0x1000U) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        const char *guest_path = (const char *)a32_guest_ptr(ctx, a0);
        if (strcmp(guest_path, "/proc/self/exe") == 0) {
            const char *ep = (ctx->exec_path && ctx->exec_path[0]) ? ctx->exec_path : ctx->guest_argv[0];
            if (ctx->sysroot && ctx->sysroot[0]) {
                size_t sr_len = strlen(ctx->sysroot);
                while (sr_len > 1U && ctx->sysroot[sr_len - 1U] == '/') {
                    sr_len--;
                }
                if (sr_len > 1U && strncmp(ep, ctx->sysroot, sr_len) == 0 && ep[sr_len] == '/') {
                    ep += sr_len;
                }
            }
            size_t elen = strlen(ep);
            size_t copylen = (elen < (size_t)a2) ? elen : (size_t)a2;
            memcpy(a32_guest_ptr(ctx, a1), ep, copylen);
            ctx->regs[0] = (uint32_t)copylen;
            break;
        }
        char resolved[PATH_MAX];
        if (resolve_guest_path_ex(ctx, a0, resolved, sizeof(resolved), false) < 0) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        ctx->regs[0] = host_ret(readlink(resolved, (char *)a32_guest_ptr(ctx, a1), (size_t)a2));
        break;
    }

    case ARM_NR_readlinkat: {
        if ((int32_t)a3 <= 0) {
            ctx->regs[0] = (uint32_t)(-EINVAL);
            break;
        }
        if (a1 < 0x1000U || a2 < 0x1000U) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        const char *guest_path = (const char *)a32_guest_ptr(ctx, a1);
        if (strcmp(guest_path, "/proc/self/exe") == 0) {
            const char *ep = (ctx->exec_path && ctx->exec_path[0]) ? ctx->exec_path : ctx->guest_argv[0];
            if (ctx->sysroot && ctx->sysroot[0]) {
                size_t sr_len = strlen(ctx->sysroot);
                while (sr_len > 1U && ctx->sysroot[sr_len - 1U] == '/') {
                    sr_len--;
                }
                if (sr_len > 1U && strncmp(ep, ctx->sysroot, sr_len) == 0 && ep[sr_len] == '/') {
                    ep += sr_len;
                }
            }
            size_t elen = strlen(ep);
            size_t copylen = (elen < (size_t)a3) ? elen : (size_t)a3;
            memcpy(a32_guest_ptr(ctx, a2), ep, copylen);
            ctx->regs[0] = (uint32_t)copylen;
            break;
        }
        char resolved[PATH_MAX];
        int dirfd = translate_at_fd(a0);
        if (resolve_guest_path_at(ctx, dirfd, a1, resolved, sizeof(resolved), false) < 0) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        ctx->regs[0] = host_ret(readlinkat(dirfd, resolved,
                                          (char *)a32_guest_ptr(ctx, a2), (size_t)a3));
        break;
    }

    case ARM_NR_chmod: {
        char resolved[PATH_MAX];
        if (resolve_guest_path_ex(ctx, a0, resolved, sizeof(resolved), true) < 0) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        ctx->regs[0] = host_ret(chmod(resolved, (mode_t)a1));
        break;
    }

    case ARM_NR_fchmod:
        ctx->regs[0] = host_ret(fchmod((int)a0, (mode_t)a1));
        break;

    case ARM_NR_fchmodat: {
        char resolved[PATH_MAX];
        int dirfd = translate_at_fd(a0);
        if (resolve_guest_path_at(ctx, dirfd, a1, resolved, sizeof(resolved), true) < 0) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        ctx->regs[0] = host_ret(fchmodat(dirfd, resolved, (mode_t)a2, 0));
        break;
    }

    case ARM_NR_chown:
    case ARM_NR_lchown: {
        char resolved[PATH_MAX];
        bool follow = (nr != ARM_NR_lchown);
        if (resolve_guest_path_ex(ctx, a0, resolved, sizeof(resolved), follow) < 0) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        uid_t uid = (a1 == 0xFFFFU) ? (uid_t)-1 : (uid_t)a1;
        gid_t gid = (a2 == 0xFFFFU) ? (gid_t)-1 : (gid_t)a2;
        int r = (nr == ARM_NR_lchown) ? lchown(resolved, uid, gid)
                                      : chown(resolved, uid, gid);
        ctx->regs[0] = host_ret(r);
        break;
    }

    case ARM_NR_fchown: {
        uid_t uid = (a1 == 0xFFFFU) ? (uid_t)-1 : (uid_t)a1;
        gid_t gid = (a2 == 0xFFFFU) ? (gid_t)-1 : (gid_t)a2;
        ctx->regs[0] = host_ret(fchown((int)a0, uid, gid));
        break;
    }

    case ARM_NR_fchownat: {
        char resolved[PATH_MAX];
        int dirfd = translate_at_fd(a0);
        bool follow = ((a4 & 0x100U /* AT_SYMLINK_NOFOLLOW */) == 0U);
        if (resolve_guest_path_at(ctx, dirfd, a1, resolved, sizeof(resolved), follow) < 0) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        ctx->regs[0] = host_ret(fchownat(dirfd, resolved, (uid_t)a2, (gid_t)a3, (int)a4));
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
        char resolved[PATH_MAX];
        const char *path = NULL;
        int dirfd = translate_at_fd(a0);
        if (a1 != 0U) {
            bool follow = ((a3 & 0x100U /* AT_SYMLINK_NOFOLLOW */) == 0U);
            if (resolve_guest_path_at(ctx, dirfd, a1, resolved, sizeof(resolved), follow) < 0) {
                ctx->regs[0] = (uint32_t)(-EFAULT);
                break;
            }
            path = resolved;
        }
        struct timespec host_ts[2];
        struct timespec *ts_ptr = NULL;
        if (a2 != 0U) {
            if (a2 < 0x1000U) {
                ctx->regs[0] = (uint32_t)(-EFAULT);
                break;
            }
            const arm_timespec32_t *gts = (const arm_timespec32_t *)a32_guest_ptr(ctx, a2);
            host_ts[0].tv_sec  = (time_t)gts[0].tv_sec;
            host_ts[0].tv_nsec = (long)gts[0].tv_nsec;
            host_ts[1].tv_sec  = (time_t)gts[1].tv_sec;
            host_ts[1].tv_nsec = (long)gts[1].tv_nsec;
            ts_ptr = host_ts;
        }
        ctx->regs[0] = host_ret(syscall(SYS_utimensat, dirfd, path, ts_ptr, (int)a3));
        break;
    }

    case ARM_NR_utimensat_time64: {
        char resolved[PATH_MAX];
        const char *path = NULL;
        int dirfd = translate_at_fd(a0);
        if (a1 != 0U) {
            bool follow = ((a3 & 0x100U /* AT_SYMLINK_NOFOLLOW */) == 0U);
            if (resolve_guest_path_at(ctx, dirfd, a1, resolved, sizeof(resolved), follow) < 0) {
                ctx->regs[0] = (uint32_t)(-EFAULT);
                break;
            }
            path = resolved;
        }
        struct timespec host_ts[2];
        struct timespec *ts_ptr = NULL;
        if (a2 != 0U) {
            if (a2 < 0x1000U) {
                ctx->regs[0] = (uint32_t)(-EFAULT);
                break;
            }
            const arm_timespec64_t *gts = (const arm_timespec64_t *)a32_guest_ptr(ctx, a2);
            host_ts[0].tv_sec  = (time_t)gts[0].tv_sec;
            host_ts[0].tv_nsec = (long)gts[0].tv_nsec;
            host_ts[1].tv_sec  = (time_t)gts[1].tv_sec;
            host_ts[1].tv_nsec = (long)gts[1].tv_nsec;
            ts_ptr = host_ts;
        }
        ctx->regs[0] = host_ret(syscall(SYS_utimensat, dirfd, path, ts_ptr, (int)a3));
        break;
    }

    case ARM_NR_nanosleep: {
        if (a0 < 0x1000U || (a1 != 0U && a1 < 0x1000U)) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
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

    case ARM_NR_clock_nanosleep: {
        if (a2 < 0x1000U || (a3 != 0U && a3 < 0x1000U)) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
        const arm_timespec32_t *req = (const arm_timespec32_t *)a32_guest_ptr(ctx, a2);
        struct timespec host_req = { (time_t)req->tv_sec, (long)req->tv_nsec };
        struct timespec host_rem;
        int r = clock_nanosleep((clockid_t)a0, (int)a1, &host_req, a3 ? &host_rem : NULL);
        if (r != 0) {
            ctx->regs[0] = (uint32_t)(-r);
            if (a3 != 0U && (int)a1 == 0) {
                arm_timespec32_t *rem = (arm_timespec32_t *)a32_guest_ptr(ctx, a3);
                rem->tv_sec  = (int32_t)host_rem.tv_sec;
                rem->tv_nsec = (int32_t)host_rem.tv_nsec;
            }
        } else {
            ctx->regs[0] = 0U;
        }
        break;
    }

    case ARM_NR_clock_nanosleep64: {
        if (a2 < 0x1000U || (a3 != 0U && a3 < 0x1000U)) {
            ctx->regs[0] = (uint32_t)(-EFAULT);
            break;
        }
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

    case ARM_NR_prctl: {
        static _Thread_local char tls_comm[16] = "a32rt";
        unsigned long arg2 = (unsigned long)a1;
        if (a0 == 15U /* PR_SET_NAME */ ||
            a0 == 16U /* PR_GET_NAME */ ||
            a0 == 2U  /* PR_GET_PDEATHSIG */) {
            if (a1 < 0x1000U) {
                ctx->regs[0] = (uint32_t)(-EFAULT);
                break;
            }
            arg2 = (unsigned long)(uintptr_t)a32_guest_ptr(ctx, a1);
        }
        if (a0 == 15U /* PR_SET_NAME */) {
            const char *src = (const char *)arg2;
            size_t n = 0U;
            while (n < 15U && src[n] != '\0') {
                tls_comm[n] = src[n];
                n++;
            }
            while (n < 16U) {
                tls_comm[n++] = '\0';
            }
        } else if (a0 == 16U /* PR_GET_NAME */) {
            memcpy((void *)arg2, tls_comm, 16U);
        }
        ctx->regs[0] = host_ret(prctl((int)a0, arg2, (unsigned long)a2,
                                      (unsigned long)a3, (unsigned long)a4));
        break;
    }

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

    if (ctx->exit_reason != A32_EXIT_BRANCH ||
        nr == ARM_NR_sigreturn ||
        nr == ARM_NR_rt_sigreturn) {
        a32_signals_check_pending(ctx);
    }
    return 0;
}
