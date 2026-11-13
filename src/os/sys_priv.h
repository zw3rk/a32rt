/*
 * src/os/sys_priv.h - Private declarations and ABI tables for ARM EABI syscalls
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef A32_SYS_PRIV_H
#define A32_SYS_PRIV_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif

#include "a32rt.h"
#include "vm/vm32.h"
#include "vm/vm_priv.h"

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
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/sendfile.h>
#include <sys/statfs.h>
#include <sys/sysinfo.h>
#include <sys/timerfd.h>
#elif defined(__APPLE__)
#include <sys/mount.h>
#include <sys/param.h>
#undef ifc_buf
#endif

#ifndef CMSG_ALIGN
#define CMSG_ALIGN(len) (((len) + sizeof(size_t) - 1U) & ~(sizeof(size_t) - 1U))
#endif

#ifndef CLONE_VM
#define CLONE_VM             0x00000100U
#define CLONE_FS             0x00000200U
#define CLONE_FILES          0x00000400U
#define CLONE_SIGHAND        0x00000800U
#define CLONE_VFORK          0x00004000U
#define CLONE_PARENT         0x00008000U
#define CLONE_THREAD         0x00010000U
#define CLONE_SYSVSEM        0x00040000U
#define CLONE_SETTLS         0x00080000U
#define CLONE_PARENT_SETTID  0x00100000U
#define CLONE_CHILD_CLEARTID 0x00200000U
#define CLONE_DETACHED       0x00400000U
#define CLONE_CHILD_SETTID   0x01000000U
#endif
#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
#endif

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
#define ARM_NR_times             43U
#define ARM_NR_getpriority       96U
#define ARM_NR_setpriority       97U
#define ARM_NR_personality       136U
#define ARM_NR_flock             143U
#define ARM_NR_mlock             150U
#define ARM_NR_munlock           151U
#define ARM_NR_arm_fadvise64_64  270U
#define ARM_NR_inotify_add_watch 317U
#define ARM_NR_inotify_rm_watch  318U
#define ARM_NR_membarrier        389U
#define ARM_NR_mlock2            390U
#define ARM_NR_close_range       436U
#define ARM_NR_faccessat2        439U
#define ARM_NR_setxattr          226U
#define ARM_NR_lsetxattr         227U
#define ARM_NR_fsetxattr         228U
#define ARM_NR_getxattr          229U
#define ARM_NR_lgetxattr         230U
#define ARM_NR_fgetxattr         231U
#define ARM_NR_listxattr         232U
#define ARM_NR_llistxattr        233U
#define ARM_NR_flistxattr        234U
#define ARM_NR_removexattr       235U
#define ARM_NR_lremovexattr      236U
#define ARM_NR_fremovexattr      237U

/* ARM private syscalls (0x0f0000+) */
#define ARM_NR_breakpoint        0x0F0001U
#define ARM_NR_cacheflush        0x0F0002U
#define ARM_NR_set_tls           0x0F0005U
#define ARM_NR_get_tls           0x0F0006U

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
#ifndef SYS_fadvise64
#define SYS_fadvise64 223
#endif
#ifndef SYS_membarrier
#define SYS_membarrier 283
#endif
#ifndef SYS_inotify_add_watch
#define SYS_inotify_add_watch 27
#endif
#ifndef SYS_inotify_rm_watch
#define SYS_inotify_rm_watch 28
#endif
#ifndef SYS_mlock2
#define SYS_mlock2 284
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

typedef struct arm_timeval64 {
    int64_t tv_sec;
    int64_t tv_usec;
} arm_timeval64_t;

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

static inline void ifr32_to_64(a64_ifreq_t *dst, const arm_ifreq_t *src)
{
    memset(dst, 0, sizeof(*dst));
    memcpy(dst->ifr_name, src->ifr_name, sizeof(src->ifr_name));
    memcpy(dst->ifr_ifru.raw, src->ifr_ifru, sizeof(src->ifr_ifru));
}

static inline void ifr64_to_32(arm_ifreq_t *dst, const a64_ifreq_t *src)
{
    memset(dst, 0, sizeof(*dst));
    memcpy(dst->ifr_name, src->ifr_name, sizeof(dst->ifr_name));
    memcpy(dst->ifr_ifru, src->ifr_ifru.raw, sizeof(dst->ifr_ifru));
}

#define MAX_DIR_COOKIES 4096U
#define DIR_COOKIE_BASE 0x70000000LL
#define VIRTUAL_PROC_NOT_MATCHED (-9999)

typedef struct dir_cookie_entry {
    int      fd;
    uint64_t off64;
} dir_cookie_entry_t;

extern dir_cookie_entry_t g_dir_cookies[MAX_DIR_COOKIES];
extern size_t             g_dir_cookie_count;

typedef struct child_thread_args {
    a32_ctx_t   *child_ctx;
    uint32_t     flags;
    uint32_t     ptid_va;
    uint32_t     ctid_va;
    int          child_tid;
    volatile int started;
} child_thread_args_t;

static inline int host_errno_to_linux(int err)
{
#if !defined(__APPLE__)
    return err;
#else
    if (err <= 0) {
        return err;
    }
    switch (err) {
    case 11: return 35; case 35: return 11; case 36: return 115;
    case 37: return 114; case 38: return 88; case 39: return 89;
    case 40: return 90; case 41: return 91; case 42: return 92;
    case 43: return 93; case 44: return 94; case 45: return 95;
    case 46: return 96; case 47: return 97; case 48: return 98;
    case 49: return 99; case 50: return 100; case 51: return 101;
    case 52: return 102; case 53: return 103; case 54: return 104;
    case 55: return 105; case 56: return 106; case 57: return 107;
    case 58: return 108; case 59: return 109; case 60: return 110;
    case 61: return 111; case 62: return 40; case 63: return 36;
    case 64: return 112; case 65: return 113; case 66: return 39;
    case 68: return 87; case 69: return 122; case 70: return 116;
    case 71: return 66; case 77: return 37; case 78: return 38;
    case 84: return 75; case 89: return 125; case 90: return 43;
    case 91: return 42; case 92: return 84; case 93: return 61; case 94: return 74;
    case 95: return 72; case 96: return 61; case 97: return 67;
    case 98: return 63; case 99: return 60; case 100: return 71;
    case 101: return 62; case 102: return 95; case 104: return 131;
    case 105: return 130; default: return err;
    }
#endif
}

static inline uint32_t host_ret(ssize_t r)
{
    if (r < 0) {
        return (uint32_t)(-host_errno_to_linux(errno));
    }
    return (uint32_t)r;
}

static inline uint64_t u64_from_u32(uint32_t lo, uint32_t hi)
{
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static inline int translate_at_fd(uint32_t guest_dfd)
{
    if (guest_dfd == 0xFFFFFF9CU || guest_dfd == (uint32_t)AT_FDCWD) {
        return AT_FDCWD;
    }
    return (int)(int32_t)guest_dfd;
}

static inline int translate_open_flags(uint32_t guest_flags)
{
#if defined(__APPLE__)
    int f = (int)(guest_flags & 3U);
    if (guest_flags & 0x00000040U) f |= O_CREAT;
    if (guest_flags & 0x00000080U) f |= O_EXCL;
    if (guest_flags & 0x00000100U) f |= O_NOCTTY;
    if (guest_flags & 0x00000200U) f |= O_TRUNC;
    if (guest_flags & 0x00000400U) f |= O_APPEND;
    if (guest_flags & 0x00000800U) f |= O_NONBLOCK;
    if (guest_flags & 0x00001000U) f |= O_DSYNC;
    if (guest_flags & 0x00004000U) f |= O_DIRECTORY;
    if (guest_flags & 0x00008000U) f |= O_NOFOLLOW;
    if (guest_flags & 0x00080000U) f |= O_CLOEXEC;
    return f;
#else
    return (int)guest_flags;
#endif
}

static inline uint32_t id16_to_32(uint32_t id)
{
    return ((id & 0xFFFFU) == 0xFFFFU) ? (uint32_t)-1 : (id & 0xFFFFU);
}

static inline int get_host_tid(a32_ctx_t *ctx)
{
    if (ctx->host_tid == 0) {
#if defined(__linux__)
        ctx->host_tid = (int)syscall(SYS_gettid);
#elif defined(__APPLE__)
        uint64_t tid64 = 0ULL;
        (void)pthread_threadid_np(NULL, &tid64);
        ctx->host_tid = (tid64 != 0ULL) ? (int)(tid64 & 0x3FFFFFFFULL) : (int)getpid();
#else
        ctx->host_tid = (int)getpid();
#endif
    }
    return ctx->host_tid;
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
    /* Every emulator-side store breaks reservations on all spanned
     * 64-byte buckets (cross-RT LL/SC class) and flushes JIT code when
     * any written page is guest-EXEC (A32-R9-6). */
    a32_vm32_copyout_invalidate_exec(ctx, va, len);
    return true;
}

/* Post-syscall invalidation for host-side stores into guest iov buffers
 * (readv/recvmsg class, A32-R9-6): reservation bump plus JIT flush when
 * any touched page is guest-EXEC. HVA is canonical (G + gva) in both
 * regimes, so the guest address is recovered by subtracting the base. */
static inline void guest_iov_invalidate(a32_ctx_t *ctx, const struct iovec *hiov,
    uint32_t iovcnt)
{
    uint32_t i;
    if (ctx == NULL || hiov == NULL) {
        return;
    }
    for (i = 0U; i < iovcnt; i++) {
        if (hiov[i].iov_base == NULL || hiov[i].iov_len == 0U) {
            continue;
        }
        uint32_t gva =
            (uint32_t)((uintptr_t)hiov[i].iov_base - (uintptr_t)ctx->guest_base);
        a32_vm32_copyout_invalidate_exec(ctx, gva, hiov[i].iov_len);
    }
}

/* Shared helpers across os_sys_*.c */
int64_t     a32_sys_map_dir_cookie(int fd, uint64_t host_off);
bool        a32_sys_unmap_dir_cookie(int fd, int64_t guest_off, off_t *out_host_off);
size_t      a32_sys_sysroot_prefix_len(const a32_ctx_t *ctx);
int         a32_sys_read_guest_cstr(const a32_ctx_t *ctx, uint32_t guest_addr, char *out, size_t max_len);
int         a32_sys_resolve_guest_path_str_at(const a32_ctx_t *ctx, int dirfd, const char *guest_path,
                                              char *buf, size_t size, bool follow_final);
int         a32_sys_resolve_guest_path_at(const a32_ctx_t *ctx, int dirfd, uint32_t guest_addr,
                                          char *buf, size_t size, bool follow_final);
bool        a32_sys_is_proc_self_exe(const char *gpath, const char *resolved);
const char *a32_sys_get_guest_exe_path(const a32_ctx_t *ctx);
int         a32_sys_try_open_virtual_proc(a32_ctx_t *ctx, const char *gpath, const char *resolved);
int         a32_sys_path_denied(const a32_ctx_t *ctx, const char *resolved);

void  a32_sys_fill_arm_stat64(const a32_ctx_t *ctx, arm_stat64_t *dst, const struct stat *src);
pid_t a32_sys_fork_with_guest_snapshot(a32_ctx_t *ctx, uint32_t child_sp);
void  a32_sys_fill_arm_rusage32(arm_rusage32_t *dst, const struct rusage *src);
void  a32_sys_fill_arm_statfs64(arm_statfs64_t *dst, const struct statfs *src);
bool  a32_sys_is_arm32_elf_file(const char *path);
bool  a32_sys_guest_range_mapped(const a32_ctx_t *ctx, uint32_t va, uint32_t len, uint32_t req_prot);
int   a32_sys_translate_linux_clockid(uint32_t linux_clk, clockid_t *out_clk);
bool  a32_sys_read_guest_timespec(a32_ctx_t *ctx, uint32_t va, bool time64, struct timespec *out);
bool  a32_sys_write_guest_timespec(a32_ctx_t *ctx, uint32_t va, bool time64, const struct timespec *in);
#if defined(__linux__)
bool  a32_sys_read_guest_itimerspec(a32_ctx_t *ctx, uint32_t va, bool time64, struct itimerspec *out);
bool  a32_sys_write_guest_itimerspec(a32_ctx_t *ctx, uint32_t va, bool time64, const struct itimerspec *in);
#endif
bool  a32_sys_read_guest_timeval(a32_ctx_t *ctx, uint32_t va, bool time64, struct timeval *out);
bool  a32_sys_write_guest_timeval(a32_ctx_t *ctx, uint32_t va, bool time64, const struct timeval *in);
bool  a32_sys_read_guest_itimerval(a32_ctx_t *ctx, uint32_t va, struct itimerval *out);
bool  a32_sys_write_guest_itimerval(a32_ctx_t *ctx, uint32_t va, const struct itimerval *in);
int   a32_sys_handle_fcntl_flock(a32_ctx_t *ctx, int fd, int cmd, uint32_t arg, bool is64);

void *a32_sys_guest_thread_start(void *arg);
int   a32_sys_handle_fork_clone(a32_ctx_t *ctx, uint32_t nr, uint32_t a0,
                                uint32_t a1, uint32_t a2, uint32_t a3,
                                uint32_t a4);
void  a32_sys_check_pwait_signals(a32_ctx_t *ctx, uint64_t temp_mask);
void  a32_sys_restore_pwait_sigmask(a32_ctx_t *ctx, uint64_t saved_sig_mask);
int   a32_sys_pwait_sigmask_enter(a32_ctx_t *ctx, uint32_t mask_va, uint32_t mask_sz,
                                  bool *has_temp_mask, uint64_t *saved_sig_mask,
                                  uint64_t *temp_mask);
void  a32_sys_pwait_sigmask_leave(a32_ctx_t *ctx, bool has_temp_mask,
                                  uint64_t saved_sig_mask, uint64_t temp_mask);
int   a32_sys_translate_guest_iov(a32_ctx_t *ctx, uint32_t iov_va, uint32_t iovcnt,
                                  struct iovec *stack_iov, uint32_t stack_cap,
                                  struct iovec **out_iov, uint32_t req_prot);
bool  a32_sys_read_guest_strv(a32_ctx_t *ctx, uint32_t vec_va, char **out, int max_count, int *out_count);
#if !defined(__linux__)
bool  a32_sys_futex_darwin_wait(a32_ctx_t *ctx, uint32_t a0, uint32_t op,
                                uint32_t a2, uint32_t a3, uint32_t a5, bool time64);
#endif

int a32_sys_dispatch_sig_time(a32_ctx_t *ctx);
int a32_sys_dispatch_sock(a32_ctx_t *ctx);
int a32_sys_dispatch_fs_arm(a32_ctx_t *ctx);
void a32_signals_ensure_host_handler(int sig);
void a32_sigfd_track(int fd, uint64_t mask, int flags);
void a32_sigfd_untrack(int fd);
bool a32_sigfd_lookup(int fd, uint64_t *mask_out, int *flags_out);

#define map_dir_cookie             a32_sys_map_dir_cookie
#define unmap_dir_cookie           a32_sys_unmap_dir_cookie
#define sysroot_prefix_len         a32_sys_sysroot_prefix_len
#define read_guest_cstr            a32_sys_read_guest_cstr
#define resolve_guest_path_str_at  a32_sys_resolve_guest_path_str_at
#define resolve_guest_path_at      a32_sys_resolve_guest_path_at
#define is_proc_self_exe           a32_sys_is_proc_self_exe
#define get_guest_exe_path         a32_sys_get_guest_exe_path
#define try_open_virtual_proc      a32_sys_try_open_virtual_proc
#define fill_arm_stat64            a32_sys_fill_arm_stat64
#define fork_with_guest_snapshot   a32_sys_fork_with_guest_snapshot
#define fill_arm_rusage32          a32_sys_fill_arm_rusage32
#define fill_arm_statfs64          a32_sys_fill_arm_statfs64
#define is_arm32_elf_file          a32_sys_is_arm32_elf_file
#define guest_range_mapped         a32_sys_guest_range_mapped
#define translate_linux_clockid    a32_sys_translate_linux_clockid
#define read_guest_timespec        a32_sys_read_guest_timespec
#define write_guest_timespec       a32_sys_write_guest_timespec
#define read_guest_itimerspec      a32_sys_read_guest_itimerspec
#define write_guest_itimerspec     a32_sys_write_guest_itimerspec
#define read_guest_timeval         a32_sys_read_guest_timeval
#define write_guest_timeval        a32_sys_write_guest_timeval
#define read_guest_itimerval       a32_sys_read_guest_itimerval
#define write_guest_itimerval      a32_sys_write_guest_itimerval
#define handle_fcntl_flock         a32_sys_handle_fcntl_flock
#define guest_thread_start         a32_sys_guest_thread_start
#define check_pwait_signals        a32_sys_check_pwait_signals
#define restore_pwait_sigmask      a32_sys_restore_pwait_sigmask
#define pwait_sigmask_enter        a32_sys_pwait_sigmask_enter
#define pwait_sigmask_leave        a32_sys_pwait_sigmask_leave
#define translate_guest_iov        a32_sys_translate_guest_iov
#define read_guest_strv            a32_sys_read_guest_strv
#define futex_darwin_wait          a32_sys_futex_darwin_wait

#define FAIL_ERR(err) do { int _e = (int)(err); ctx->regs[0] = (_e < 0) ? (uint32_t)_e : (uint32_t)(-host_errno_to_linux(_e)); return 0; } while (0)
#define FAIL_EFAULT_IF(cond) do { if (cond) FAIL_ERR(EFAULT); } while (0)
#define FAIL_EINVAL_IF(cond) do { if (cond) FAIL_ERR(EINVAL); } while (0)
#define RESOLVE_AT(dirfd, va, buf, follow) do { \
    int _rc = resolve_guest_path_at(ctx, (dirfd), (va), (buf), sizeof(buf), (follow)); \
    if (_rc < 0) { ctx->regs[0] = (uint32_t)_rc; return 0; } \
} while (0)
/* A32-R13-3: same resolution, then refuse a raw host /proc or /sys path
 * that is not confined under the explicit sysroot. */
#define RESOLVE_AT_CONFINED(dirfd, va, buf, follow) do { \
    RESOLVE_AT(dirfd, va, buf, follow); \
    if (a32_sys_path_denied(ctx, buf)) FAIL_ERR(ENOENT); \
} while (0)

#endif /* A32_SYS_PRIV_H */
