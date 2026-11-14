/*
 * tests/ios/ios_guest.h — freestanding 32-bit iOS (armv7/armv7s) guest ABI.
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zero host-SDK dependencies: only <stdint.h>/<stddef.h> (compiler-provided).
 * Declares the exact 32-bit iOS POSIX/Mach/signal/pthread types and prototypes
 * used by the a32rt iOS fixtures so every test compiles with
 * -std=c11 -Werror -Werror=vla -pedantic -Wall -Wextra -O2 -fno-stack-protector
 * with no host-SDK warnings and fully controlled 32-bit types.
 */

#ifndef IOS_GUEST_H
#define IOS_GUEST_H

#include <stdint.h>
#include <stddef.h>

/* ---- _COMM_PAGE (XNU 32-bit) ---- */
#define IOS_COMMPAGE_BASE 0xFFFF4000U
#define IOS_COMMPAGE_SIG "commpage 32-bit"
#define IOS_COMMPAGE_SIG_LEN 15U
#define IOS_COMMPAGE_NCPUS_OFF 0x22U

/* ---- BSD syscall numbers (XNU, shared with Darwin) ---- */
#define IOS_SYS_exit 1
#define IOS_SYS_fork 2
#define IOS_SYS_read 3
#define IOS_SYS_write 4
#define IOS_SYS_open 5
#define IOS_SYS_close 6
#define IOS_SYS_wait4 7
#define IOS_SYS_unlink 10
#define IOS_SYS_lseek 19
#define IOS_SYS_getpid 20
#define IOS_SYS_pipe 42
#define IOS_SYS_sigaltstack 53
#define IOS_SYS_execve 59
#define IOS_SYS_socketpair 135
#define IOS_SYS_poll 230
#define IOS_SYS_kqueue 362
#define IOS_SYS_kevent 363

/* ---- Mach / ARM trap numbers (r12 for svc #0x80) ---- */
#define IOS_TRAP_MACH_VM_ALLOCATE 0xFFFFFFF6U  /* -10 */
#define IOS_TRAP_MACH_VM_DEALLOCATE 0xFFFFFFF4U /* -12 */
#define IOS_TRAP_MACH_VM_PROTECT 0xFFFFFFF2U   /* -14 */
#define IOS_TRAP_MACH_REPLY_PORT 0xFFFFFFE6U   /* -26 */
#define IOS_TRAP_MACH_THREAD_SELF 0xFFFFFFE5U  /* -27 */
#define IOS_TRAP_MACH_TASK_SELF 0xFFFFFFE4U    /* -28 */
#define IOS_TRAP_MACH_HOST_SELF 0xFFFFFFE3U    /* -29 */
#define IOS_TRAP_MACH_MSG 0xFFFFFFE1U          /* -31 */
#define IOS_TRAP_MACH_TIMEBASE_INFO 0xFFFFFFA7U /* -89 */
#define IOS_TRAP_ARM_ICACHE_INVALIDATE 0x80000000U

/* ---- errno / signal numbers ---- */
#define IOS_ENOENT 2
#define IOS_SIGSEGV 11
#define IOS_SIGUSR1 30
#define IOS_SA_ONSTACK 0x0001
#define IOS_SA_RESTART 0x0002
#define IOS_SA_RESETHAND 0x0004
#define IOS_SA_NODEFER 0x0010
#define IOS_SA_SIGINFO 0x0040

/* ---- sigaltstack / poll / kqueue constants ---- */
#define IOS_SS_ONSTACK 1
#define IOS_SS_DISABLE 4
#define IOS_POLLIN 0x0001
#define IOS_POLLOUT 0x0004
#define IOS_EVFILT_READ (-1)
#define IOS_EV_ADD 0x0001
#define IOS_EV_ONESHOT 0x0010

/* ---- open / mmap / prot flags ---- */
#define IOS_O_RDONLY 0
#define IOS_O_WRONLY 1
#define IOS_O_RDWR 2
#define IOS_O_CREAT 0x200
#define IOS_O_TRUNC 0x400
#define IOS_SEEK_SET 0
#define IOS_PROT_NONE 0
#define IOS_PROT_READ 1
#define IOS_PROT_WRITE 2
#define IOS_PROT_EXEC 4
#define IOS_MAP_PRIVATE 2
#define IOS_MAP_ANON 0x1000
#define IOS_MAP_FAILED ((void *)0xFFFFFFFFU)

/* ---- 108-byte iOS stat64 (APCS-GNU 4-byte aligned; must match a32rt) ---- */
typedef struct __attribute__((packed, aligned(4))) ios_guest_stat {
    int32_t st_dev;
    uint16_t st_mode;
    uint16_t st_nlink;
    uint64_t st_ino;
    uint32_t st_uid;
    uint32_t st_gid;
    int32_t st_rdev;
    int32_t st_atime_sec;
    int32_t st_atime_nsec;
    int32_t st_mtime_sec;
    int32_t st_mtime_nsec;
    int32_t st_ctime_sec;
    int32_t st_ctime_nsec;
    int32_t st_btime_sec;
    int32_t st_btime_nsec;
    int64_t st_size;
    int64_t st_blocks;
    int32_t st_blksize;
    uint32_t st_flags;
    uint32_t st_gen;
    int32_t st_lspare;
    int64_t st_qspare[2];
} ios_guest_stat_t;

_Static_assert(sizeof(ios_guest_stat_t) == 108, "ios_guest_stat_t must be 108 bytes");

/* ---- sigaltstack / poll / kevent / timespec (32-bit Darwin) ---- */
typedef struct __attribute__((packed, aligned(4))) ios_stack32 {
    uint32_t ss_sp;
    uint32_t ss_size;
    int32_t ss_flags;
} ios_stack32_t;

_Static_assert(sizeof(ios_stack32_t) == 12, "ios_stack32_t must be 12 bytes");

typedef struct __attribute__((packed, aligned(4))) ios_pollfd {
    int32_t fd;
    int16_t events;
    int16_t revents;
} ios_pollfd_t;

_Static_assert(sizeof(ios_pollfd_t) == 8, "ios_pollfd_t must be 8 bytes");

typedef struct __attribute__((packed, aligned(4))) ios_kevent32 {
    uint32_t ident;
    int16_t filter;
    uint16_t flags;
    uint32_t fflags;
    int32_t data;
    uint32_t udata;
} ios_kevent32_t;

_Static_assert(sizeof(ios_kevent32_t) == 20, "ios_kevent32_t must be 20 bytes");

typedef struct __attribute__((packed, aligned(4))) ios_timespec32 {
    int32_t tv_sec;
    int32_t tv_nsec;
} ios_timespec32_t;

_Static_assert(sizeof(ios_timespec32_t) == 8, "ios_timespec32_t must be 8 bytes");

/* ---- signal / ucontext (32-bit Darwin) ---- */
typedef void (*ios_sighandler_fn_t)(int);
typedef void (*ios_sigaction_fn_t)(int, void *, void *);

typedef struct ios_sigaction {
    ios_sigaction_fn_t sa_sigaction;
    uint32_t sa_mask;
    int sa_flags;
} ios_sigaction_t;

typedef struct ios_arm_exception_state32 {
    uint32_t exception;
    uint32_t fsr;
    uint32_t far;
} ios_arm_exception_state32_t;

typedef struct ios_arm_thread_state32 {
    uint32_t r[13];
    uint32_t sp;
    uint32_t lr;
    uint32_t pc;
    uint32_t cpsr;
} ios_arm_thread_state32_t;

typedef struct ios_arm_vfp_state32 {
    uint32_t r[64];
    uint32_t fpscr;
    uint32_t pad[8];
} ios_arm_vfp_state32_t;

typedef struct ios_mcontext32 {
    ios_arm_exception_state32_t es;
    ios_arm_thread_state32_t ss;
    ios_arm_vfp_state32_t fs;
} ios_mcontext32_t;

_Static_assert(sizeof(ios_mcontext32_t) == 372, "ios_mcontext32_t must be 372 bytes");

typedef struct ios_ucontext32 {
    int32_t uc_onstack;
    uint32_t uc_sigmask;
    uint32_t uc_stack_ss_sp;
    uint32_t uc_stack_ss_size;
    int32_t uc_stack_ss_flags;
    struct ios_ucontext32 *uc_link;
    uint32_t uc_mcsize;
    ios_mcontext32_t *uc_mcontext;
} ios_ucontext32_t;

_Static_assert(sizeof(ios_ucontext32_t) == 32, "ios_ucontext32_t must be 32 bytes");

/* ---- Mach types ---- */
typedef uint32_t ios_mach_port_t;
typedef int32_t ios_kern_return_t;

typedef struct ios_mach_msg_header {
    uint32_t msgh_bits;
    uint32_t msgh_size;
    uint32_t msgh_remote_port;
    uint32_t msgh_local_port;
    uint32_t msgh_voucher_port;
    uint32_t msgh_id;
} ios_mach_msg_header_t;

typedef struct ios_mach_timebase_info {
    uint32_t numer;
    uint32_t denom;
} ios_mach_timebase_info_t;

/* ---- pthread (opaque 32-bit Darwin) ---- */
typedef uint32_t ios_pthread_t;
typedef uint32_t ios_pthread_key_t;
typedef struct ios_pthread_mutex {
    uint32_t __opaque[16];
} ios_pthread_mutex_t;
typedef void *(*ios_pthread_start_fn_t)(void *);

/* ---- FILE (opaque) ---- */
typedef struct ios_FILE ios_FILE_t;
extern ios_FILE_t *__stdoutp;
extern ios_FILE_t *__stderrp;
extern ios_FILE_t *__stdinp;
extern char **environ;

/* ---- libc / BSD prototypes (C names map to TBD _-prefixed symbols) ---- */
extern void exit(int __status) __attribute__((noreturn));
extern void _exit(int __status) __attribute__((noreturn));
extern void abort(void) __attribute__((noreturn));
extern int *__error(void);
extern int open(const char *__path, int __flags, ...);
extern int close(int __fd);
extern int unlink(const char *__path);
extern int read(int __fd, void *__buf, size_t __n);
extern int write(int __fd, const void *__buf, size_t __n);
extern int64_t lseek(int __fd, int64_t __off, int __whence);
extern int stat(const char *__path, ios_guest_stat_t *__buf);
extern int fstat(int __fd, ios_guest_stat_t *__buf);
extern void *mmap(void *__addr, size_t __len, int __prot, int __flags, int __fd,
    int64_t __off);
extern int munmap(void *__addr, size_t __len);
extern int mprotect(void *__addr, size_t __len, int __prot);
extern int getpid(void);
extern int gettimeofday(void *__tv, void *__tz);
extern int sysctlbyname(const char *__name, void *__oldp, size_t *__oldlenp,
    void *__newp, size_t __newlen);
extern int getentropy(void *__buf, size_t __len);
extern void *malloc(size_t __size);
extern void *calloc(size_t __n, size_t __size);
extern void *realloc(void *__ptr, size_t __size);
extern void free(void *__ptr);
extern size_t strlen(const char *__s);
extern int strcmp(const char *__a, const char *__b);
extern int strncmp(const char *__a, const char *__b, size_t __n);
extern void *memcpy(void *__dst, const void *__src, size_t __n);
extern void *memmove(void *__dst, const void *__src, size_t __n);
extern void *memset(void *__dst, int __c, size_t __n);
extern int memcmp(const void *__a, const void *__b, size_t __n);
extern int puts(const char *__s);
extern int printf(const char *__fmt, ...);
extern int snprintf(char *__s, size_t __n, const char *__fmt, ...);
extern uint64_t mach_absolute_time(void);
extern ios_kern_return_t mach_timebase_info(ios_mach_timebase_info_t *__info);
extern ios_mach_port_t mach_task_self(void);
extern ios_mach_port_t mach_host_self(void);
extern ios_mach_port_t mach_thread_self(void);
extern void sys_icache_invalidate(void *__addr, size_t __len);
extern int sigaction(int __sig, const ios_sigaction_t *__act,
    ios_sigaction_t *__oact);
extern int sigprocmask(int __how, const uint32_t *__set, uint32_t *__oset);
extern int kill(int __pid, int __sig);
extern int raise(int __sig);
extern ios_pthread_t pthread_self(void);
extern int pthread_create(ios_pthread_t *__t, const void *__attr,
    ios_pthread_start_fn_t __fn, void *__arg);
extern int pthread_join(ios_pthread_t __t, void **__ret);
extern int pthread_mutex_init(ios_pthread_mutex_t *__m, const void *__attr);
extern int pthread_mutex_lock(ios_pthread_mutex_t *__m);
extern int pthread_mutex_unlock(ios_pthread_mutex_t *__m);
extern int pthread_mutex_destroy(ios_pthread_mutex_t *__m);
extern int pthread_key_create(ios_pthread_key_t *__k, void (*__dtor)(void *));
extern int pthread_setspecific(ios_pthread_key_t __k, const void *__v);
extern void *pthread_getspecific(ios_pthread_key_t __k);

/* ---- raw svc #0x80 helpers (BSD convention: r12 = nr, CPSR.C = error) ---- */
static inline __attribute__((unused)) int32_t
ios_bsd0(uint32_t nr, uint32_t *carry_out)
{
    register uint32_t r0 __asm__("r0") = 0U;
    register uint32_t r12 __asm__("r12") = nr;
    uint32_t cpsr = 0U;
    __asm__ __volatile__("svc #0x80\n\tmrs %1, apsr"
                         : "+r"(r0), "=r"(cpsr)
                         : "r"(r12)
                         : "memory", "cc");
    if (carry_out != 0) {
        *carry_out = (cpsr >> 29U) & 1U;
    }
    return (int32_t)r0;
}

/* Dual-return BSD helper: fork (r0=pid parent / r0=0,r1=1 child),
 * pipe (r0=rfd, r1=wfd). XNU ARM32 ABI: r0..r1 dual return, r12 = nr. */
static inline __attribute__((unused)) int32_t
ios_bsd0_r01(uint32_t nr, uint32_t *r1_out, uint32_t *carry_out)
{
    register uint32_t r0 __asm__("r0") = 0U;
    register uint32_t r1 __asm__("r1") = 0U;
    register uint32_t r12 __asm__("r12") = nr;
    uint32_t cpsr = 0U;
    __asm__ __volatile__("svc #0x80\n\tmrs %1, apsr"
                         : "+r"(r0), "=r"(cpsr), "+r"(r1)
                         : "r"(r12)
                         : "memory", "cc");
    if (r1_out != 0) {
        *r1_out = r1;
    }
    if (carry_out != 0) {
        *carry_out = (cpsr >> 29U) & 1U;
    }
    return (int32_t)r0;
}

static inline __attribute__((unused)) int32_t
ios_bsd3(uint32_t nr, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t *carry_out)
{
    register uint32_t r0 __asm__("r0") = a0;
    register uint32_t r1 __asm__("r1") = a1;
    register uint32_t r2 __asm__("r2") = a2;
    register uint32_t r12 __asm__("r12") = nr;
    uint32_t cpsr = 0U;
    __asm__ __volatile__("svc #0x80\n\tmrs %1, apsr"
                         : "+r"(r0), "=r"(cpsr)
                         : "r"(r1), "r"(r2), "r"(r12)
                         : "memory", "cc");
    if (carry_out != 0) {
        *carry_out = (cpsr >> 29U) & 1U;
    }
    return (int32_t)r0;
}

static inline __attribute__((unused)) int32_t
ios_bsd6(uint32_t nr, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
    uint32_t a4, uint32_t a5, uint32_t *carry_out)
{
    register uint32_t r0 __asm__("r0") = a0;
    register uint32_t r1 __asm__("r1") = a1;
    register uint32_t r2 __asm__("r2") = a2;
    register uint32_t r3 __asm__("r3") = a3;
    register uint32_t r4 __asm__("r4") = a4;
    register uint32_t r5 __asm__("r5") = a5;
    register uint32_t r12 __asm__("r12") = nr;
    uint32_t cpsr = 0U;
    __asm__ __volatile__("svc #0x80\n\tmrs %1, apsr"
                         : "+r"(r0), "=r"(cpsr)
                         : "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5),
                           "r"(r12)
                         : "memory", "cc");
    if (carry_out != 0) {
        *carry_out = (cpsr >> 29U) & 1U;
    }
    return (int32_t)r0;
}

/* Mach/ARM traps use the same svc #0x80 with negative / high r12. */
static inline __attribute__((unused)) uint32_t
ios_trap0(uint32_t trap)
{
    register uint32_t r0 __asm__("r0") = 0U;
    register uint32_t r12 __asm__("r12") = trap;
    __asm__ __volatile__("svc #0x80" : "+r"(r0) : "r"(r12) : "memory", "cc");
    return r0;
}

static inline __attribute__((unused)) uint32_t
ios_trap3(uint32_t trap, uint32_t a0, uint32_t a1, uint32_t a2)
{
    register uint32_t r0 __asm__("r0") = a0;
    register uint32_t r1 __asm__("r1") = a1;
    register uint32_t r2 __asm__("r2") = a2;
    register uint32_t r12 __asm__("r12") = trap;
    __asm__ __volatile__("svc #0x80"
                         : "+r"(r0)
                         : "r"(r1), "r"(r2), "r"(r12)
                         : "memory", "cc");
    return r0;
}

static inline __attribute__((unused)) uint32_t
ios_trap4(uint32_t trap, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
    register uint32_t r0 __asm__("r0") = a0;
    register uint32_t r1 __asm__("r1") = a1;
    register uint32_t r2 __asm__("r2") = a2;
    register uint32_t r3 __asm__("r3") = a3;
    register uint32_t r12 __asm__("r12") = trap;
    __asm__ __volatile__("svc #0x80"
                         : "+r"(r0)
                         : "r"(r1), "r"(r2), "r"(r3), "r"(r12)
                         : "memory", "cc");
    return r0;
}

/* Raw BSD write/exit used by the static fixture (no libc). */
static inline __attribute__((unused)) int32_t
ios_raw_write(int fd, const void *buf, uint32_t n)
{
    register uint32_t r0 __asm__("r0") = (uint32_t)fd;
    register uint32_t r1 __asm__("r1") = (uint32_t)(uintptr_t)buf;
    register uint32_t r2 __asm__("r2") = n;
    register uint32_t r12 __asm__("r12") = (uint32_t)IOS_SYS_write;
    register int32_t out __asm__("r0");
    __asm__ __volatile__("svc #0x80"
                         : "=r"(out)
                         : "r"(r0), "r"(r1), "r"(r2), "r"(r12)
                         : "memory", "cc");
    return out;
}

static inline __attribute__((unused)) void
ios_raw_exit(int code)
{
    register uint32_t r0 __asm__("r0") = (uint32_t)code;
    register uint32_t r12 __asm__("r12") = (uint32_t)IOS_SYS_exit;
    __asm__ __volatile__("svc #0x80" : : "r"(r0), "r"(r12) : "memory");
    while (1) {
    }
}

static inline __attribute__((unused)) uint32_t
ios_read_tpidruro(void)
{
    uint32_t v = 0U;
    __asm__ __volatile__("mrc p15, 0, %0, c13, c0, 3" : "=r"(v));
    return v;
}

#endif /* IOS_GUEST_H */
