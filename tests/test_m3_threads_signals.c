/*
 * tests/test_m3_threads_signals.c — M3 / M3-hl Threads, Signals & SMC fixture
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stddef.h>

struct sigaction32 {
    uint32_t sa_handler;
    uint32_t sa_flags;
    uint32_t sa_restorer;
    uint32_t sa_mask[2];
};

__asm__(
    ".text\n"
    ".globl __aeabi_uidiv\n"
    ".type __aeabi_uidiv, %function\n"
    "__aeabi_uidiv:\n"
    "    udiv r0, r0, r1\n"
    "    bx lr\n"
    ".globl __aeabi_idiv\n"
    ".type __aeabi_idiv, %function\n"
    "__aeabi_idiv:\n"
    "    sdiv r0, r0, r1\n"
    "    bx lr\n"
    ".globl my_rt_restorer\n"
    ".type my_rt_restorer, %function\n"
    "my_rt_restorer:\n"
    "    mov r7, #173\n"  /* SYS_rt_sigreturn */
    "    svc #0\n"
    ".globl arm_clone_trampoline\n"
    ".type arm_clone_trampoline, %function\n"
    "arm_clone_trampoline:\n"
    "    push {r4, r5, r7, lr}\n"
    "    ldr  r4, [sp, #16]\n" /* ctid */
    "    ldr  r5, [sp, #20]\n" /* fn */
    "    bic  r1, r1, #7\n"
    "    sub  r1, r1, #8\n"
    "    str  r5, [r1]\n"
    "    mov  r7, #120\n"      /* SYS_clone */
    "    svc  #0\n"
    "    cmp  r0, #0\n"
    "    beq  .Lchild_entry\n"
    "    pop  {r4, r5, r7, pc}\n"
    ".Lchild_entry:\n"
    "    ldr  r3, [sp]\n"
    "    blx  r3\n"
    "    mov  r7, #1\n"        /* SYS_exit */
    "    mov  r0, #0\n"
    "    svc  #0\n"
);

int arm_clone_trampoline(uint32_t flags, void *child_stack, uint32_t *ptid,
                         void *tls, uint32_t *ctid, void (*fn)(void));
void my_rt_restorer(void);

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

/* --------------------------------------------------------------------------
 * Test 1: Threads + Futex + Atomics
 * -------------------------------------------------------------------------- */
static volatile uint32_t g_shared_counter = 0U;
static volatile uint32_t g_child_futex = 0U;

static void worker_thread(void)
{
    for (int i = 0; i < 100; i++) {
        uint32_t tmp = 0U;
        uint32_t val = 0U;
        __asm__ volatile(
            "1: ldrex %0, [%2]\n"
            "   add   %0, %0, #1\n"
            "   strex %1, %0, [%2]\n"
            "   cmp   %1, #0\n"
            "   bne   1b\n"
            : "=&r"(val), "=&r"(tmp)
            : "r"(&g_shared_counter)
            : "cc", "memory"
        );
    }
    g_child_futex = 1U;
    sys6(240U /* futex */, (uint32_t)(uintptr_t)&g_child_futex, 1U /* FUTEX_WAKE */, 1U, 0U, 0U, 0U);
}

static int test1_threads_futex(void)
{
    uint32_t stack_mem = (uint32_t)sys6(192U /* mmap2 */, 0U, 65536U,
                                        3U /* PROT_READ|PROT_WRITE */,
                                        0x22U /* MAP_PRIVATE|MAP_ANONYMOUS */,
                                        0xFFFFFFFFU, 0U);
    if (stack_mem > 0xFFFFF000U) {
        return 10;
    }
    uint32_t child_sp = (stack_mem + 65536U) & ~7U;

    volatile uint32_t parent_tid = 0U;
    volatile uint32_t child_tid = 1U;
    uint32_t flags = 0x013D0F00U;

    int clone_rc = arm_clone_trampoline(flags, (void *)(uintptr_t)child_sp,
                                        (uint32_t *)(uintptr_t)&parent_tid,
                                        NULL,
                                        (uint32_t *)(uintptr_t)&child_tid,
                                        worker_thread);
    if (clone_rc <= 0) {
        return 11;
    }

    while (child_tid != 0U) {
        sys6(240U /* futex */, (uint32_t)(uintptr_t)&child_tid, 0U /* FUTEX_WAIT */,
             child_tid, 0U, 0U, 0U);
    }

    if (g_shared_counter != 100U) {
        return 12;
    }

    sys2(91U /* munmap */, stack_mem, 65536U);
    return 0;
}

/* --------------------------------------------------------------------------
 * Test 2: Signal Delivery & rt_sigreturn
 * -------------------------------------------------------------------------- */
static volatile uint32_t g_sigusr1_handled = 0U;

static void sigusr1_handler(int sig, void *info, void *ucontext)
{
    (void)sig;
    (void)info;
    (void)ucontext;
    g_sigusr1_handled = 1U;
}

static int test2_signals(void)
{
    struct sigaction32 act;
    act.sa_handler = (uint32_t)(uintptr_t)sigusr1_handler;
    act.sa_flags = 0x04000004U; /* SA_SIGINFO | SA_RESTORER */
    act.sa_restorer = (uint32_t)(uintptr_t)my_rt_restorer;
    act.sa_mask[0] = 0U;
    act.sa_mask[1] = 0U;

    if (sys4(174U /* rt_sigaction */, 10U /* SIGUSR1 */, (uint32_t)(uintptr_t)&act, 0U, 8U) != 0) {
        return 20;
    }

    uint32_t tid = (uint32_t)sys1(224U /* gettid */, 0U);
    if (sys2(238U /* tkill */, tid, 10U /* SIGUSR1 */) != 0) {
        return 21;
    }

    if (g_sigusr1_handled != 1U) {
        return 22;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Test 3: Synchronous SIGSEGV Recovery via Side Table & ucontext
 * -------------------------------------------------------------------------- */
static volatile uint32_t g_sigsegv_handled = 0U;
static volatile uint32_t g_fault_page_addr = 0U;

static void sigsegv_handler(int sig, void *info, void *uctx_ptr)
{
    (void)sig;
    (void)info;
    g_sigsegv_handled = 1U;
    uint8_t *uc = (uint8_t *)uctx_ptr;
    uint32_t arm_pc = *(uint32_t *)(uc + 92);
    uint32_t fault_addr = *(uint32_t *)(uc + 100);

    if (fault_addr != g_fault_page_addr) {
        return;
    }
    /* Skip the faulting 4-byte instruction */
    *(uint32_t *)(uc + 92) = arm_pc + 4U;
}

static int test3_sigsegv_recovery(void)
{
    uint32_t page = (uint32_t)sys6(192U /* mmap2 */, 0U, 4096U, 0U /* PROT_NONE */,
                                   0x22U /* MAP_PRIVATE|MAP_ANONYMOUS */, 0xFFFFFFFFU, 0U);
    if (page > 0xFFFFF000U) {
        return 30;
    }
    g_fault_page_addr = page;

    struct sigaction32 act;
    act.sa_handler = (uint32_t)(uintptr_t)sigsegv_handler;
    act.sa_flags = 0x04000004U; /* SA_SIGINFO | SA_RESTORER */
    act.sa_restorer = (uint32_t)(uintptr_t)my_rt_restorer;
    act.sa_mask[0] = 0U;
    act.sa_mask[1] = 0U;

    if (sys4(174U /* rt_sigaction */, 11U /* SIGSEGV */, (uint32_t)(uintptr_t)&act, 0U, 8U) != 0) {
        return 31;
    }

    volatile uint32_t post_fault_reached = 0U;
    __asm__ volatile(
        "ldr r0, [%1]\n"
        "mov %0, #1\n"
        : "=r"(post_fault_reached)
        : "r"(page)
        : "r0", "memory"
    );

    if (g_sigsegv_handled != 1U || post_fault_reached != 1U) {
        return 32;
    }

    sys2(91U /* munmap */, page, 4096U);
    return 0;
}

/* --------------------------------------------------------------------------
 * Test 4: Self-Modifying Code & cacheflush
 * -------------------------------------------------------------------------- */
static int test4_smc_cacheflush(void)
{
    uint32_t page = (uint32_t)sys6(192U /* mmap2 */, 0U, 4096U,
                                   3U /* PROT_READ|PROT_WRITE */,
                                   0x22U /* MAP_PRIVATE|MAP_ANONYMOUS */,
                                   0xFFFFFFFFU, 0U);
    if (page > 0xFFFFF000U) {
        return 40;
    }

    uint32_t *code = (uint32_t *)(uintptr_t)page;
    /* A32: mov r0, #11 -> 0xE3A0000B, bx lr -> 0xE12FFF1E */
    code[0] = 0xE3A0000BU;
    code[1] = 0xE12FFF1EU;

    if (sys3(125U /* mprotect */, page, 4096U, 5U /* PROT_READ|PROT_EXEC */) != 0) {
        return 41;
    }
    sys3(0x0F0002U /* cacheflush */, page, page + 16U, 0U);

    typedef uint32_t (*smc_fn_t)(void);
    smc_fn_t fn = (smc_fn_t)(uintptr_t)page;
    uint32_t res1 = fn();
    if (res1 != 11U) {
        return 42;
    }

    /* Reprotect RW and patch immediate to 22 */
    if (sys3(125U /* mprotect */, page, 4096U, 3U /* PROT_READ|PROT_WRITE */) != 0) {
        return 43;
    }
    /* A32: mov r0, #22 -> 0xE3A00016 */
    code[0] = 0xE3A00016U;

    if (sys3(125U /* mprotect */, page, 4096U, 5U /* PROT_READ|PROT_EXEC */) != 0) {
        return 44;
    }
    sys3(0x0F0002U /* cacheflush */, page, page + 16U, 0U);

    uint32_t res2 = fn();
    if (res2 != 22U) {
        return 45;
    }

    sys2(91U /* munmap */, page, 4096U);
    return 0;
}

static int run_all_m3(void)
{
    int rc = test1_threads_futex();
    if (rc != 0) {
        return rc;
    }
    rc = test2_signals();
    if (rc != 0) {
        return rc;
    }
    rc = test3_sigsegv_recovery();
    if (rc != 0) {
        return rc;
    }
    rc = test4_smc_cacheflush();
    if (rc != 0) {
        return rc;
    }

    static const char pass_msg[] = "M3 Threads, Signals & SMC: PASS\n";
    sys3(4U /* write */, 1U, (uint32_t)(uintptr_t)pass_msg, (uint32_t)(sizeof(pass_msg) - 1U));
    return 0;
}

void __attribute__((noreturn)) _start_c(void)
{
    int rc = run_all_m3();
    sys1(248U /* exit_group */, (uint32_t)rc);
    __builtin_unreachable();
}

__asm__(
    ".text\n"
    ".globl _start\n"
    ".type _start, %function\n"
    "_start:\n"
    "    mov r0, sp\n"
    "    bic r1, r0, #7\n"
    "    mov sp, r1\n"
    "    bl _start_c\n"
);
