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

struct siginfo32 {
    int32_t  si_signo;
    int32_t  si_errno;
    int32_t  si_code;
    void    *si_addr;
    uint32_t _pad[28];
};

_Static_assert(offsetof(struct siginfo32, si_addr) == 12,
               "si_addr must be at byte offset 12 in 32-bit siginfo_t");

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
    ".globl resethand_asm_handler\n"
    ".type resethand_asm_handler, %function\n"
    "resethand_asm_handler:\n"
    "    ldr r3, [sp, #0]\n"
    "    b   resethand_c_handler\n"
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
 * Test 3: Synchronous SIGSEGV Recovery, si_addr & VFP d0-d31 Preservation
 * -------------------------------------------------------------------------- */
static volatile uint32_t g_sigsegv_handled = 0U;
static volatile uint32_t g_sigsegv_si_addr_ok = 0U;
static volatile uint32_t g_fault_page_addr = 0U;
static uint64_t g_vfp_init[32];
static uint64_t g_vfp_clobber[32];
static uint64_t g_vfp_after[32];

static void sigsegv_handler(int sig, void *info, void *uctx_ptr)
{
    uint8_t *uc = (uint8_t *)uctx_ptr;
    uint32_t arm_pc = *(uint32_t *)(uc + 92);
    uint32_t fault_addr = *(uint32_t *)(uc + 100);

    /* Always advance past the 4-byte faulting ldr so we never infinite-loop */
    *(uint32_t *)(uc + 92) = arm_pc + 4U;

    const struct siginfo32 *si = (const struct siginfo32 *)info;
    if (sig == 11 && si != NULL && si->si_signo == 11 &&
        (uint32_t)(uintptr_t)si->si_addr == g_fault_page_addr &&
        fault_addr == g_fault_page_addr) {
        g_sigsegv_si_addr_ok = 1U;
    }

    /* Clobber all 32 VFP double registers d0-d31 inside the signal handler */
    const uint64_t *clob_lo = &g_vfp_clobber[0];
    const uint64_t *clob_hi = &g_vfp_clobber[16];
    __asm__ volatile(
        "vldmia %0, {d0-d15}\n"
        "vldmia %1, {d16-d31}\n"
        :
        : "r"(clob_lo), "r"(clob_hi)
        : "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7",
          "d8", "d9", "d10", "d11", "d12", "d13", "d14", "d15",
          "d16", "d17", "d18", "d19", "d20", "d21", "d22", "d23",
          "d24", "d25", "d26", "d27", "d28", "d29", "d30", "d31",
          "memory"
    );

    g_sigsegv_handled = 1U;
}

static int test3_sigsegv_recovery(void)
{
    uint32_t page = (uint32_t)sys6(192U /* mmap2 */, 0U, 4096U, 0U /* PROT_NONE */,
                                   0x22U /* MAP_PRIVATE|MAP_ANONYMOUS */, 0xFFFFFFFFU, 0U);
    if (page > 0xFFFFF000U) {
        return 30;
    }
    uint32_t fault_target = page + 0x128U;
    g_fault_page_addr = fault_target;
    g_sigsegv_handled = 0U;
    g_sigsegv_si_addr_ok = 0U;

    for (uint32_t i = 0U; i < 32U; i++) {
        g_vfp_init[i] = 0x3FF0000000000000ULL | ((uint64_t)(i + 1U) << 32) | (uint64_t)(0x1000U + i);
        g_vfp_clobber[i] = 0x7FE00000DEAD0000ULL | (uint64_t)i;
        g_vfp_after[i] = 0ULL;
    }

    struct sigaction32 act;
    act.sa_handler = (uint32_t)(uintptr_t)sigsegv_handler;
    act.sa_flags = 0x04000004U; /* SA_SIGINFO | SA_RESTORER */
    act.sa_restorer = (uint32_t)(uintptr_t)my_rt_restorer;
    act.sa_mask[0] = 0U;
    act.sa_mask[1] = 0U;

    if (sys4(174U /* rt_sigaction */, 11U /* SIGSEGV */, (uint32_t)(uintptr_t)&act, 0U, 8U) != 0) {
        sys2(91U /* munmap */, page, 4096U);
        return 31;
    }

    const uint64_t *init_lo = &g_vfp_init[0];
    const uint64_t *init_hi = &g_vfp_init[16];
    uint64_t *out_lo = &g_vfp_after[0];
    uint64_t *out_hi = &g_vfp_after[16];
    volatile uint32_t post_fault_reached = 0U;
    __asm__ volatile(
        "vldmia %2, {d0-d15}\n"
        "vldmia %3, {d16-d31}\n"
        "ldr r0, [%1]\n"
        "vstmia %4, {d0-d15}\n"
        "vstmia %5, {d16-d31}\n"
        "mov %0, #1\n"
        : "=&r"(post_fault_reached)
        : "r"(fault_target), "r"(init_lo), "r"(init_hi), "r"(out_lo), "r"(out_hi)
        : "r0",
          "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7",
          "d8", "d9", "d10", "d11", "d12", "d13", "d14", "d15",
          "d16", "d17", "d18", "d19", "d20", "d21", "d22", "d23",
          "d24", "d25", "d26", "d27", "d28", "d29", "d30", "d31",
          "memory"
    );

    sys2(91U /* munmap */, page, 4096U);

    if (g_sigsegv_handled != 1U || post_fault_reached != 1U) {
        return 32;
    }
    if (g_sigsegv_si_addr_ok != 1U) {
        return 33;
    }
    for (uint32_t i = 0U; i < 32U; i++) {
        if (g_vfp_after[i] != g_vfp_init[i]) {
            return 34;
        }
    }

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

/* --------------------------------------------------------------------------
 * Test 5: mremap (163) positive (grow/shrink) & negative (unaligned) tests
 * -------------------------------------------------------------------------- */
static int test5_mremap(void)
{
    uint32_t addr = (uint32_t)sys6(192U /* mmap2 */, 0U, 4096U,
                                   3U /* PROT_READ|PROT_WRITE */,
                                   0x22U /* MAP_PRIVATE|MAP_ANONYMOUS */,
                                   0xFFFFFFFFU, 0U);
    if (addr > 0xFFFFF000U) {
        return 50;
    }

    volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)addr;
    p[0] = 0xCAFEBABEU;
    p[1023] = 0xDEADBEEFU;

    /* Grow 4 KiB -> 16 KiB with MREMAP_MAYMOVE (1) */
    uint32_t new_addr = (uint32_t)sys4(163U /* mremap */, addr, 4096U, 16384U,
                                       1U /* MREMAP_MAYMOVE */);
    if (new_addr > 0xFFFFF000U || (new_addr & 0xFFFU) != 0U) {
        sys2(91U /* munmap */, addr, 4096U);
        return 51;
    }

    volatile uint32_t *np = (volatile uint32_t *)(uintptr_t)new_addr;
    if (np[0] != 0xCAFEBABEU || np[1023] != 0xDEADBEEFU) {
        sys2(91U /* munmap */, new_addr, 16384U);
        return 52;
    }

    /* Verify newly grown pages are valid and writable */
    np[1024] = 0x11223344U;
    np[4095] = 0x55667788U;
    if (np[1024] != 0x11223344U || np[4095] != 0x55667788U) {
        sys2(91U /* munmap */, new_addr, 16384U);
        return 53;
    }

    /* Shrink 16 KiB -> 8 KiB in-place (flags = 0) */
    uint32_t shrunk_addr = (uint32_t)sys4(163U /* mremap */, new_addr, 16384U, 8192U, 0U);
    if (shrunk_addr != new_addr) {
        sys2(91U /* munmap */, new_addr, 16384U);
        return 54;
    }

    volatile uint32_t *sp = (volatile uint32_t *)(uintptr_t)shrunk_addr;
    if (sp[0] != 0xCAFEBABEU || sp[1023] != 0xDEADBEEFU || sp[1024] != 0x11223344U) {
        sys2(91U /* munmap */, shrunk_addr, 8192U);
        return 55;
    }

    if (sys2(91U /* munmap */, shrunk_addr, 8192U) != 0) {
        return 56;
    }

    /* Negative: unaligned address (0x12345) must return -EINVAL (-22) */
    int32_t bad_rc = sys4(163U /* mremap */, 0x12345U, 4096U, 8192U, 1U /* MREMAP_MAYMOVE */);
    if (bad_rc != -22 /* -EINVAL */) {
        return 57;
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * Test 6: Negative brk & mmap2 overflow / boundary protection tests
 * -------------------------------------------------------------------------- */
static int test6_vm_boundaries(void)
{
    uint32_t cur_brk = (uint32_t)sys1(45U /* brk */, 0U);
    if (cur_brk == 0U) {
        return 60;
    }

    /* Negative: brk(0xFFFFFFFFU) overflow must return current brk unchanged */
    uint32_t brk_ovf = (uint32_t)sys1(45U /* brk */, 0xFFFFFFFFU);
    if (brk_ovf != cur_brk) {
        return 61;
    }

    /* Negative: brk(0x00000100U) below brk_base must return current brk unchanged */
    uint32_t brk_low = (uint32_t)sys1(45U /* brk */, 0x00000100U);
    if (brk_low != cur_brk) {
        return 62;
    }

    /* Positive: grow brk by 4 KiB, write/read, and shrink back to cur_brk */
    uint32_t grown_brk = (uint32_t)sys1(45U /* brk */, cur_brk + 4096U);
    if (grown_brk != cur_brk + 4096U) {
        return 63;
    }
    volatile uint32_t *bp = (volatile uint32_t *)(uintptr_t)cur_brk;
    bp[0] = 0xA5A55A5AU;
    if (bp[0] != 0xA5A55A5AU) {
        return 64;
    }
    if ((uint32_t)sys1(45U /* brk */, cur_brk) != cur_brk) {
        return 65;
    }

    /* Negative: mmap2 MAP_FIXED at 0x0U (low guard page) must fail with -EINVAL/-ENOMEM */
    int32_t map_zero = sys6(192U /* mmap2 */, 0x0U, 4096U,
                            3U /* PROT_READ|PROT_WRITE */,
                            0x32U /* MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED */,
                            0xFFFFFFFFU, 0U);
    if (map_zero != -22 /* -EINVAL */ && map_zero != -12 /* -ENOMEM */) {
        return 66;
    }

    /* Negative: mmap2 MAP_FIXED at 0xFFFF0000U (kuser page) must fail without clobbering kuser */
    int32_t map_kuser = sys6(192U /* mmap2 */, 0xFFFF0000U, 4096U,
                             3U /* PROT_READ|PROT_WRITE */,
                             0x32U /* MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED */,
                             0xFFFFFFFFU, 0U);
    if (map_kuser != -22 /* -EINVAL */ && map_kuser != -12 /* -ENOMEM */) {
        return 67;
    }

    /* Verify kuser helper version at 0xFFFF0FFCU is still 5 */
    uint32_t kuser_ver = *(const volatile uint32_t *)(uintptr_t)0xFFFF0FFCU;
    if (kuser_ver != 5U) {
        return 68;
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * Test 7: a32_jit_invalidate_range epoch preservation regression test
 * -------------------------------------------------------------------------- */
static uint32_t __attribute__((noinline)) hot_fn_a(uint32_t x)
{
    uint32_t acc = x ^ 0x13579BDFU;
    for (uint32_t i = 0U; i < 8U; i++) {
        if ((acc & 1U) != 0U) {
            acc = (acc >> 1) ^ 0xEDB88320U;
        } else {
            acc >>= 1;
        }
    }
    return acc + x;
}

static uint32_t __attribute__((noinline)) hot_fn_b(uint32_t rounds)
{
    uint32_t (* volatile fn_ptr)(uint32_t) = hot_fn_a;
    uint32_t total = 0x12345678U;
    for (uint32_t r = 0U; r < rounds; r++) {
        total ^= hot_fn_a(total + r);
        total += fn_ptr(r ^ 0x55AA55AAU);
    }
    return total;
}

static int test7_invalidate_range_epoch(void)
{
    _Alignas(64) uint32_t scratch_c[32];
    for (uint32_t i = 0U; i < 32U; i++) {
        scratch_c[i] = i * 3U + 1U;
    }

    /* Warm up and chain hot functions A and B before any range invalidation */
    uint32_t baseline_a = hot_fn_a(42U);
    uint32_t baseline_b = hot_fn_b(16U);

    /*
     * Trigger a32_jit_invalidate_range repeatedly via cacheflush on scratch_c
     * and verify hot_fn_a and hot_fn_b continue to execute and re-chain properly.
     */
    for (uint32_t iter = 0U; iter < 6U; iter++) {
        scratch_c[iter & 31U] ^= 0xA5A5A5A5U;
        sys3(0x0F0002U /* __ARM_NR_cacheflush */,
             (uint32_t)(uintptr_t)&scratch_c[0],
             (uint32_t)(uintptr_t)&scratch_c[32],
             0U);

        for (uint32_t run = 0U; run < 4U; run++) {
            if (hot_fn_a(42U) != baseline_a) {
                return 70;
            }
            if (hot_fn_b(16U) != baseline_b) {
                return 71;
            }
        }
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * Test 8: SA_RESETHAND (0x80000000U) & siginfo at [sp + 0] regression test
 * -------------------------------------------------------------------------- */
static volatile uint32_t g_resethand_calls = 0U;
static volatile int32_t  g_resethand_sp0_signo = 0;
static volatile int32_t  g_resethand_info_signo = 0;

void resethand_c_handler(int sig, void *info, void *ucontext, int32_t sp0_signo)
{
    (void)ucontext;
    g_resethand_calls++;
    g_resethand_sp0_signo = sp0_signo;
    if (info != NULL) {
        const struct siginfo32 *si = (const struct siginfo32 *)info;
        g_resethand_info_signo = si->si_signo;
    } else {
        g_resethand_info_signo = -1;
    }
    if (sig != 12) {
        g_resethand_info_signo = -2;
    }
}

static int test8_sa_resethand(void)
{
    g_resethand_calls = 0U;
    g_resethand_sp0_signo = 0;
    g_resethand_info_signo = 0;

    struct sigaction32 act;
    act.sa_handler  = (uint32_t)(uintptr_t)resethand_asm_handler;
    act.sa_flags    = 0x80000000U | 0x04000000U | 0x00000004U; /* SA_RESETHAND | SA_RESTORER | SA_SIGINFO */
    act.sa_restorer = (uint32_t)(uintptr_t)my_rt_restorer;
    act.sa_mask[0]  = 0U;
    act.sa_mask[1]  = 0U;

    if (sys4(174U /* rt_sigaction */, 12U /* SIGUSR2 */,
             (uint32_t)(uintptr_t)&act, 0U, 8U) != 0) {
        return 80;
    }

    uint32_t tid = (uint32_t)sys1(224U /* gettid */, 0U);
    if (sys2(238U /* tkill */, tid, 12U /* SIGUSR2 */) != 0) {
        return 81;
    }

    if (g_resethand_calls != 1U ||
        g_resethand_sp0_signo != 12 ||
        g_resethand_info_signo != 12) {
        return 82;
    }

    struct sigaction32 oldact;
    oldact.sa_handler  = 0xDEADBEEFU;
    oldact.sa_flags    = 0U;
    oldact.sa_restorer = 0U;
    oldact.sa_mask[0]  = 0U;
    oldact.sa_mask[1]  = 0U;

    if (sys4(174U /* rt_sigaction */, 12U /* SIGUSR2 */,
             0U, (uint32_t)(uintptr_t)&oldact, 8U) != 0) {
        return 83;
    }
    if (oldact.sa_handler != 0U /* SIG_DFL */) {
        return 84;
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * Test 9: Negative (-EFAULT) signal syscall tests
 * -------------------------------------------------------------------------- */
static int test9_negative_signal_syscalls(void)
{
    /* rt_sigaction (174) with invalid act (0x100) and invalid oldact (0x100) -> -EFAULT (-14) */
    if (sys4(174U /* rt_sigaction */, 10U /* SIGUSR1 */, 0x100U, 0U, 8U) != -14) {
        return 90;
    }
    if (sys4(174U /* rt_sigaction */, 10U /* SIGUSR1 */, 0U, 0x100U, 8U) != -14) {
        return 91;
    }

    /* rt_sigprocmask (175) with invalid set (0x100) and invalid oldset (0x100) -> -EFAULT (-14) */
    if (sys4(175U /* rt_sigprocmask */, 0U /* SIG_BLOCK */, 0x100U, 0U, 8U) != -14) {
        return 92;
    }
    if (sys4(175U /* rt_sigprocmask */, 0U /* SIG_BLOCK */, 0U, 0x100U, 8U) != -14) {
        return 93;
    }

    /* sigaltstack (186) with invalid uss (0x100) and invalid uoss (0x100) -> -EFAULT (-14) */
    if (sys2(186U /* sigaltstack */, 0x100U, 0U) != -14) {
        return 94;
    }
    if (sys2(186U /* sigaltstack */, 0U, 0x100U) != -14) {
        return 95;
    }

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
    rc = test5_mremap();
    if (rc != 0) {
        return rc;
    }
    rc = test6_vm_boundaries();
    if (rc != 0) {
        return rc;
    }
    rc = test7_invalidate_range_epoch();
    if (rc != 0) {
        return rc;
    }
    rc = test8_sa_resethand();
    if (rc != 0) {
        return rc;
    }
    rc = test9_negative_signal_syscalls();
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
