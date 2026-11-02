/*
 * tests/test_m3_common.h — Shared definitions for M3 threads & signals tests
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef A32_TEST_M3_COMMON_H
#define A32_TEST_M3_COMMON_H

#include <stdint.h>
#include <stddef.h>

struct sigaction32 {
    uint32_t sa_handler;
    uint32_t sa_flags;
    uint32_t sa_restorer;
    uint32_t sa_mask[2];
};

struct siginfo32 {
    int32_t  si_signo;
    int32_t  si_errno;
    int32_t  si_code;
    void    *si_addr;
    uint32_t _pad[28];
};

_Static_assert(offsetof(struct siginfo32, si_addr) == 12,
               "si_addr must be at byte offset 12 in 32-bit siginfo_t");


struct timespec32_m3 {
    int32_t tv_sec;
    int32_t tv_nsec;
};


struct stack32_m3 {
    uint32_t ss_sp;
    int32_t  ss_flags;
    uint32_t ss_size;
};


struct siginfo32_kill {
    int32_t  si_signo;
    int32_t  si_errno;
    int32_t  si_code;
    int32_t  si_pid;
    uint32_t si_uid;
    uint32_t _pad[27];
};

_Static_assert(offsetof(struct siginfo32_kill, si_pid) == 12,
               "si_pid must be at byte offset 12 in 32-bit siginfo_t");


struct timespec64_m3 {
    int64_t tv_sec;
    int64_t tv_nsec;
};

_Static_assert(sizeof(struct timespec64_m3) == 16U, "timespec64_m3 must be 16 bytes");


struct sigcontext32_m3 {
    uint32_t trap_no;
    uint32_t error_code;
    uint32_t oldmask;
    uint32_t arm_r0;
    uint32_t arm_r1;
    uint32_t arm_r2;
    uint32_t arm_r3;
    uint32_t arm_r4;
    uint32_t arm_r5;
    uint32_t arm_r6;
    uint32_t arm_r7;
    uint32_t arm_r8;
    uint32_t arm_r9;
    uint32_t arm_r10;
    uint32_t arm_fp;
    uint32_t arm_ip;
    uint32_t arm_sp;
    uint32_t arm_lr;
    uint32_t arm_pc;
    uint32_t arm_cpsr;
    uint32_t fault_address;
};

struct ucontext32_m3 {
    uint32_t               uc_flags;
    uint32_t               uc_link;
    struct stack32_m3      uc_stack;
    struct sigcontext32_m3 uc_mcontext;
    uint64_t               uc_sigmask;
};

_Static_assert(offsetof(struct ucontext32_m3, uc_mcontext.arm_lr) == 88U,
               "arm_lr must be at byte offset 88 in ucontext32_m3");
_Static_assert(offsetof(struct ucontext32_m3, uc_mcontext.arm_pc) == 92U,
               "arm_pc must be at byte offset 92 in ucontext32_m3");

struct siginfo32_rt_m3 {
    int32_t  si_signo;
    int32_t  si_errno;
    int32_t  si_code;
    int32_t  si_pid;
    uint32_t si_uid;
    uint32_t si_value;
    uint32_t _pad[26];
};

_Static_assert(sizeof(struct siginfo32_rt_m3) == 128U,
               "siginfo32_rt_m3 must be 128 bytes");
_Static_assert(offsetof(struct siginfo32_rt_m3, si_pid) == 12U,
               "si_pid must be at offset 12 in siginfo32_rt_m3");
_Static_assert(offsetof(struct siginfo32_rt_m3, si_value) == 20U,
               "si_value must be at offset 20 in siginfo32_rt_m3");


int arm_clone_trampoline(uint32_t flags, void *child_stack, uint32_t *ptid,
                         void *tls, uint32_t *ctid, void (*fn)(void));
void my_rt_restorer(void);
void resethand_asm_handler(int sig, void *info, void *ucontext);

static inline int32_t sys1(uint32_t nr, uint32_t a0)
{
    register uint32_t r7 __asm__("r7") = nr;
    register uint32_t r0 __asm__("r0") = a0;
    __asm__ volatile("svc #0" : "+r"(r0) : "r"(r7) : "memory", "cc");
    return (int32_t)r0;
}

static inline int32_t sys2(uint32_t nr, uint32_t a0, uint32_t a1)
{
    register uint32_t r7 __asm__("r7") = nr;
    register uint32_t r0 __asm__("r0") = a0;
    register uint32_t r1 __asm__("r1") = a1;
    __asm__ volatile("svc #0" : "+r"(r0) : "r"(r7), "r"(r1) : "memory", "cc");
    return (int32_t)r0;
}

static inline int32_t sys3(uint32_t nr, uint32_t a0, uint32_t a1, uint32_t a2)
{
    register uint32_t r7 __asm__("r7") = nr;
    register uint32_t r0 __asm__("r0") = a0;
    register uint32_t r1 __asm__("r1") = a1;
    register uint32_t r2 __asm__("r2") = a2;
    __asm__ volatile("svc #0"
                     : "+r"(r0)
                     : "r"(r7), "r"(r1), "r"(r2)
                     : "memory", "cc");
    return (int32_t)r0;
}

static inline int32_t sys4(uint32_t nr, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
    register uint32_t r7 __asm__("r7") = nr;
    register uint32_t r0 __asm__("r0") = a0;
    register uint32_t r1 __asm__("r1") = a1;
    register uint32_t r2 __asm__("r2") = a2;
    register uint32_t r3 __asm__("r3") = a3;
    __asm__ volatile("svc #0"
                     : "+r"(r0)
                     : "r"(r7), "r"(r1), "r"(r2), "r"(r3)
                     : "memory", "cc");
    return (int32_t)r0;
}

static inline int32_t sys6(uint32_t nr, uint32_t a0, uint32_t a1, uint32_t a2,
                           uint32_t a3, uint32_t a4, uint32_t a5)
{
    register uint32_t r7 __asm__("r7") = nr;
    register uint32_t r0 __asm__("r0") = a0;
    register uint32_t r1 __asm__("r1") = a1;
    register uint32_t r2 __asm__("r2") = a2;
    register uint32_t r3 __asm__("r3") = a3;
    register uint32_t r4 __asm__("r4") = a4;
    register uint32_t r5 __asm__("r5") = a5;
    __asm__ volatile("svc #0"
                     : "+r"(r0)
                     : "r"(r7), "r"(r1), "r"(r2), "r"(r3), "r"(r4), "r"(r5)
                     : "memory", "cc");
    return (int32_t)r0;
}


extern volatile uint32_t g_altstack_base;
extern volatile uint32_t g_altstack_size;
extern volatile uint32_t g_r8_usr1_hits;
void r8_usr1_handler(int sig, void *info, void *ucontext);

int run_test_m3_part2(void);
int run_test_m3_part3(void);
int run_test_m3_part4(void);

#endif /* A32_TEST_M3_COMMON_H */
