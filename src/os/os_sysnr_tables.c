/*
 * src/os/os_sysnr_tables.c - split-TU part 1/7 of src/syscalls.c (lines 1-539).
 * Compiled only via the src/syscalls.c wrapper; do not add to Makefile SRCS.
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * src/syscalls.c — ARM EABI (r7, r0..r5) -> aarch64-linux syscall & struct shim
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#define _GNU_SOURCE
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
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

typedef struct dir_cookie_entry {
    int      fd;
    uint64_t off64;
} dir_cookie_entry_t;

static dir_cookie_entry_t g_dir_cookies[MAX_DIR_COOKIES];
static size_t             g_dir_cookie_count = 0U;
