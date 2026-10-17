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

/* --------------------------------------------------------------------------
 * Test 10: Process-forking clone (without CLONE_VM) with SETTID / SETTLS
 * -------------------------------------------------------------------------- */
static int test10_clone_process_fork(void)
{
    uint32_t saved_tls = 0U;
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(saved_tls));

    uint32_t parent_sentinel_tls = 0x11112220U;
    uint32_t child_sentinel_tls  = 0x22224440U;
    if (sys1(0x0F0005U /* ARM_NR_set_tls */, parent_sentinel_tls) != 0) {
        return 96;
    }

    volatile uint32_t parent_tid = 0U;
    volatile uint32_t child_tid  = 0U;
    /*
     * SIGCHLD (17) | CLONE_PARENT_SETTID (0x00100000) |
     * CLONE_CHILD_SETTID (0x01000000) | CLONE_SETTLS (0x00080000)
     */
    uint32_t clone_flags = 17U | 0x00100000U | 0x01000000U | 0x00080000U;
    int32_t cpid = sys6(120U /* clone */, clone_flags, 0U /* child_stack = NULL */,
                        (uint32_t)(uintptr_t)&parent_tid,
                        child_sentinel_tls,
                        (uint32_t)(uintptr_t)&child_tid, 0U);
    if (cpid < 0) {
        (void)sys1(0x0F0005U /* ARM_NR_set_tls */, saved_tls);
        return 97;
    }

    if (cpid == 0) {
        /* Child process */
        uint32_t child_tls_hw = 0U;
        __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(child_tls_hw));
        typedef uint32_t (*kuser_get_tls_fn)(void);
        kuser_get_tls_fn get_tls = (kuser_get_tls_fn)(uintptr_t)0xFFFF0FE0U;
        uint32_t child_tls_kuser = get_tls();
        uint32_t my_tid = (uint32_t)sys1(224U /* gettid */, 0U);

        if (child_tls_hw != child_sentinel_tls ||
            child_tls_kuser != child_sentinel_tls ||
            child_tid == 0U || child_tid != my_tid) {
            sys1(248U /* exit_group */, 99U);
        }
        sys1(248U /* exit_group */, 55U);
        __builtin_unreachable();
    }

    /* Parent process: verify parent_tid was written and parent's TLS is intact */
    uint32_t after_parent_tls = 0U;
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(after_parent_tls));
    (void)sys1(0x0F0005U /* ARM_NR_set_tls */, saved_tls);

    if (parent_tid != (uint32_t)cpid) {
        return 98;
    }
    if (after_parent_tls != parent_sentinel_tls) {
        return 99;
    }

    uint32_t wstatus = 0U;
    int32_t wrc = sys4(114U /* wait4 */, (uint32_t)cpid,
                       (uint32_t)(uintptr_t)&wstatus, 0U, 0U);
    if (wrc != cpid) {
        return 100;
    }
    if (((wstatus >> 8) & 0xFFU) != 55U || (wstatus & 0x7FU) != 0U) {
        return 101;
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * Test 11: Synchronous SIGILL si_addr and ucontext fault_address == arm_pc
 * -------------------------------------------------------------------------- */
static volatile uint32_t g_sigill_handled = 0U;
static volatile uint32_t g_sigill_addr_ok = 0U;
static volatile uint32_t g_sigill_expected_pc = 0U;

static void sigill_handler(int sig, void *info, void *uctx_ptr)
{
    uint8_t *uc = (uint8_t *)uctx_ptr;
    uint32_t arm_pc     = *(uint32_t *)(uc + 92);
    uint32_t fault_addr = *(uint32_t *)(uc + 100);

    /* Advance past the 4-byte undefined instruction */
    *(uint32_t *)(uc + 92) = arm_pc + 4U;

    const struct siginfo32 *si = (const struct siginfo32 *)info;
    if (sig == 4 && si != NULL && si->si_signo == 4 &&
        (uint32_t)(uintptr_t)si->si_addr == g_sigill_expected_pc &&
        arm_pc == g_sigill_expected_pc &&
        fault_addr == g_sigill_expected_pc) {
        g_sigill_addr_ok = 1U;
    }
    g_sigill_handled = 1U;
}

static int test11_sigill_fault_addr(void)
{
    g_sigill_handled = 0U;
    g_sigill_addr_ok = 0U;
    g_sigill_expected_pc = 0U;

    struct sigaction32 act;
    act.sa_handler  = (uint32_t)(uintptr_t)sigill_handler;
    act.sa_flags    = 0x04000004U; /* SA_SIGINFO | SA_RESTORER */
    act.sa_restorer = (uint32_t)(uintptr_t)my_rt_restorer;
    act.sa_mask[0]  = 0U;
    act.sa_mask[1]  = 0U;

    if (sys4(174U /* rt_sigaction */, 4U /* SIGILL */,
             (uint32_t)(uintptr_t)&act, 0U, 8U) != 0) {
        return 102;
    }

    volatile uint32_t post_udf_reached = 0U;
    __asm__ volatile(
        "adr r0, 1f\n"
        "str r0, [%1]\n"
        "1:\n"
        ".word 0xE7F000F0\n" /* A32 UDF #0 */
        "mov %0, #1\n"
        : "=&r"(post_udf_reached)
        : "r"(&g_sigill_expected_pc)
        : "r0", "memory"
    );

    if (g_sigill_handled != 1U || post_udf_reached != 1U) {
        return 103;
    }
    if (g_sigill_addr_ok != 1U) {
        return 104;
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Test 12: Block chaining across partial cacheflush & targeted invalidation
 * -------------------------------------------------------------------------- */
static int test12_chaining_partial_cacheflush(void)
{
    uint32_t pages = (uint32_t)sys6(192U /* mmap2 */, 0U, 12288U,
                                    3U /* PROT_READ|PROT_WRITE */,
                                    0x22U /* MAP_PRIVATE|MAP_ANONYMOUS */,
                                    0xFFFFFFFFU, 0U);
    if (pages > 0xFFFFF000U) {
        return 105;
    }
    uint32_t page_a = pages;
    uint32_t page_b = pages + 4096U;
    uint32_t page_c = pages + 8192U;

    uint32_t *code_a = (uint32_t *)(uintptr_t)page_a;
    uint32_t *code_b = (uint32_t *)(uintptr_t)page_b;
    uint32_t *code_c = (uint32_t *)(uintptr_t)page_c;

    /* Page A: add r0, r0, #5; b page_b */
    int32_t disp = ((int32_t)page_b - (int32_t)(page_a + 12U)) >> 2;
    code_a[0] = 0xE2800005U; /* add r0, r0, #5 */
    code_a[1] = 0xEA000000U | ((uint32_t)disp & 0x00FFFFFFU); /* b page_b */

    /* Page B: add r0, r0, #10; bx lr */
    code_b[0] = 0xE280000AU; /* add r0, r0, #10 */
    code_b[1] = 0xE12FFF1EU; /* bx lr */

    /* Page C: mov r0, #77; bx lr */
    code_c[0] = 0xE3A0004DU; /* mov r0, #77 */
    code_c[1] = 0xE12FFF1EU; /* bx lr */

    if (sys3(125U /* mprotect */, pages, 12288U, 5U /* PROT_READ|PROT_EXEC */) != 0) {
        sys2(91U /* munmap */, pages, 12288U);
        return 106;
    }
    sys3(0x0F0002U /* cacheflush */, pages, pages + 12288U, 0U);

    typedef uint32_t (*chain_fn_t)(uint32_t);
    chain_fn_t fn_a = (chain_fn_t)(uintptr_t)page_a;

    /* First call translates A & B and patches A->B chain; second runs chained */
    if (fn_a(100U) != 115U || fn_a(100U) != 115U) {
        sys2(91U /* munmap */, pages, 12288U);
        return 107;
    }

    /* Partial cacheflush of unrelated page C must preserve A->B chain */
    sys3(0x0F0002U /* cacheflush */, page_c, page_c + 16U, 0U);
    if (fn_a(100U) != 115U || fn_a(100U) != 115U) {
        sys2(91U /* munmap */, pages, 12288U);
        return 108;
    }

    /*
     * Now patch ONLY page B (add r0, r0, #37) and cacheflush ONLY page B.
     * Invalidation of page B must unchain page A's outgoing chain slot so
     * calling fn_a(100) re-translates page B and returns 100 + 5 + 37 = 142.
     */
    if (sys3(125U /* mprotect */, page_b, 4096U, 3U /* PROT_READ|PROT_WRITE */) != 0) {
        sys2(91U /* munmap */, pages, 12288U);
        return 109;
    }
    code_b[0] = 0xE2800025U; /* add r0, r0, #37 */
    if (sys3(125U /* mprotect */, page_b, 4096U, 5U /* PROT_READ|PROT_EXEC */) != 0) {
        sys2(91U /* munmap */, pages, 12288U);
        return 110;
    }
    sys3(0x0F0002U /* cacheflush */, page_b, page_b + 16U, 0U);

    if (fn_a(100U) != 142U || fn_a(100U) != 142U) {
        sys2(91U /* munmap */, pages, 12288U);
        return 111;
    }

    sys2(91U /* munmap */, pages, 12288U);
    return 0;
}

/* --------------------------------------------------------------------------
 * Test 13: FUTEX_WAIT_BITSET timeout & FUTEX_CMP_REQUEUE val2 regression
 * -------------------------------------------------------------------------- */
struct timespec32_m3 {
    int32_t tv_sec;
    int32_t tv_nsec;
};

static volatile uint32_t g_requeue_f1 = 77U;
static volatile uint32_t g_requeue_f2 = 0U;
static volatile uint32_t g_requeue_entered = 0U;
static volatile uint32_t g_requeue_woken = 0U;

static void requeue_worker_thread(void)
{
    g_requeue_entered = 1U;
    while (g_requeue_f1 == 77U) {
        sys6(240U /* futex */, (uint32_t)(uintptr_t)&g_requeue_f1,
             0U | 128U /* FUTEX_WAIT_PRIVATE */, 77U, 0U, 0U, 0U);
    }
    g_requeue_woken = 1U;
}

static int test13_futex_bitset_requeue(void)
{
    volatile uint32_t fword = 42U;
    struct timespec32_m3 ts;

    /*
     * 13a: Negative — FUTEX_WAIT_BITSET (9 | 128) with bitset == 0 must return
     * -EINVAL (-22), and mismatched val must return -EAGAIN (-11).
     */
    ts.tv_sec = 0;
    ts.tv_nsec = 1000000;
    int32_t rc = sys6(240U /* futex */, (uint32_t)(uintptr_t)&fword,
                      9U | 128U /* FUTEX_WAIT_BITSET_PRIVATE */, 42U,
                      (uint32_t)(uintptr_t)&ts, 0U, 0U /* bitset == 0 */);
    if (rc != -22 /* -EINVAL */) {
        return 112;
    }

    rc = sys6(240U /* futex */, (uint32_t)(uintptr_t)&fword,
              9U | 128U /* FUTEX_WAIT_BITSET_PRIVATE */, 43U /* mismatch */,
              (uint32_t)(uintptr_t)&ts, 0U, 0xFFFFFFFFU);
    if (rc != -11 /* -EAGAIN */) {
        return 113;
    }

    /*
     * 13b: Positive — FUTEX_WAIT_BITSET (9 | 128) with 1 ms timeout must convert
     * the 32-bit struct timespec and return -ETIMEDOUT (-110) rather than
     * passing NULL to the host kernel and blocking forever.
     */
    if (sys2(263U /* clock_gettime */, 1U /* CLOCK_MONOTONIC */,
             (uint32_t)(uintptr_t)&ts) != 0) {
        return 114;
    }
    ts.tv_nsec += 1000000; /* +1 ms */
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000;
    }
    rc = sys6(240U /* futex */, (uint32_t)(uintptr_t)&fword,
              9U | 128U /* FUTEX_WAIT_BITSET_PRIVATE */, 42U,
              (uint32_t)(uintptr_t)&ts, 0U, 0xFFFFFFFFU);
    if (rc != -110 /* -ETIMEDOUT */) {
        return 115;
    }

    /*
     * 13c: Negative — FUTEX_CMP_REQUEUE (4 | 128) with wrong expected val3
     * must return -EAGAIN (-11).
     */
    g_requeue_f1 = 77U;
    g_requeue_f2 = 0U;
    rc = sys6(240U /* futex */, (uint32_t)(uintptr_t)&g_requeue_f1,
              4U | 128U /* FUTEX_CMP_REQUEUE_PRIVATE */,
              1U /* nr_wake */, 2U /* val2 = nr_requeue */,
              (uint32_t)(uintptr_t)&g_requeue_f2, 99U /* wrong val3 */);
    if (rc != -11 /* -EAGAIN */) {
        return 116;
    }

    /*
     * 13d: Positive — FUTEX_CMP_REQUEUE (4 | 128) with nr_wake = 0 and
     * val2 = nr_requeue = 1 on a live waiter thread must requeue 1 waiter from
     * g_requeue_f1 to g_requeue_f2 (returning 1) and allow waking it via
     * g_requeue_f2.
     */
    g_requeue_entered = 0U;
    g_requeue_woken = 0U;
    uint32_t stack_mem = (uint32_t)sys6(192U /* mmap2 */, 0U, 65536U,
                                        3U /* PROT_READ|PROT_WRITE */,
                                        0x22U /* MAP_PRIVATE|MAP_ANONYMOUS */,
                                        0xFFFFFFFFU, 0U);
    if (stack_mem > 0xFFFFF000U) {
        return 117;
    }
    uint32_t child_sp = (stack_mem + 65536U) & ~7U;
    volatile uint32_t parent_tid = 0U;
    volatile uint32_t child_tid = 1U;
    uint32_t flags = 0x013D0F00U;

    int clone_rc = arm_clone_trampoline(flags, (void *)(uintptr_t)child_sp,
                                        (uint32_t *)(uintptr_t)&parent_tid,
                                        NULL,
                                        (uint32_t *)(uintptr_t)&child_tid,
                                        requeue_worker_thread);
    if (clone_rc <= 0) {
        sys2(91U /* munmap */, stack_mem, 65536U);
        return 117;
    }

    while (g_requeue_entered == 0U) {
        sys1(158U /* sched_yield */, 0U);
    }

    int32_t req_rc = 0;
    for (int attempt = 0; attempt < 2000; attempt++) {
        req_rc = sys6(240U /* futex */, (uint32_t)(uintptr_t)&g_requeue_f1,
                      4U | 128U /* FUTEX_CMP_REQUEUE_PRIVATE */,
                      0U /* nr_wake = 0 */, 1U /* val2 = nr_requeue = 1 */,
                      (uint32_t)(uintptr_t)&g_requeue_f2, 77U /* val3 */);
        if (req_rc == 1) {
            break;
        }
        sys1(158U /* sched_yield */, 0U);
    }

    g_requeue_f1 = 88U;
    int32_t wake_rc = sys6(240U /* futex */, (uint32_t)(uintptr_t)&g_requeue_f2,
                           1U | 128U /* FUTEX_WAKE_PRIVATE */, 1U, 0U, 0U, 0U);
    /* Also wake g_requeue_f1 in case requeue failed so worker never hangs */
    sys6(240U /* futex */, (uint32_t)(uintptr_t)&g_requeue_f1,
         1U | 128U /* FUTEX_WAKE_PRIVATE */, 1U, 0U, 0U, 0U);

    while (child_tid != 0U) {
        sys6(240U /* futex */, (uint32_t)(uintptr_t)&child_tid, 0U /* FUTEX_WAIT */,
             child_tid, 0U, 0U, 0U);
    }
    sys2(91U /* munmap */, stack_mem, 65536U);

    if (req_rc != 1 || wake_rc != 1 || g_requeue_woken != 1U) {
        return 118;
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * Test 14: sigaltstack (SS_ONSTACK, EPERM while active, EINVAL flags)
 * -------------------------------------------------------------------------- */
struct stack32_m3 {
    uint32_t ss_sp;
    int32_t  ss_flags;
    uint32_t ss_size;
};

static volatile uint32_t g_altstack_base = 0U;
static volatile uint32_t g_altstack_size = 0U;
static volatile uint32_t g_altstack_result = 0U;

static void altstack_sigusr1_handler(int sig, void *info, void *ucontext)
{
    (void)sig;
    (void)info;
    (void)ucontext;

    uint32_t cur_sp = 0U;
    __asm__ volatile("mov %0, sp" : "=r"(cur_sp));

    if (cur_sp < g_altstack_base || cur_sp >= g_altstack_base + g_altstack_size) {
        g_altstack_result = 1U;
        return;
    }

    struct stack32_m3 oss = { 0U, 0, 0U };
    if (sys2(186U /* sigaltstack */, 0U, (uint32_t)(uintptr_t)&oss) != 0) {
        g_altstack_result = 2U;
        return;
    }
    if ((oss.ss_flags & 1 /* SS_ONSTACK */) == 0 ||
        oss.ss_sp != g_altstack_base ||
        oss.ss_size != g_altstack_size) {
        g_altstack_result = 3U;
        return;
    }

    /* Negative: modifying sigaltstack while executing on it must return -EPERM (-1) */
    struct stack32_m3 try_ss;
    try_ss.ss_sp = g_altstack_base;
    try_ss.ss_flags = 2 /* SS_DISABLE */;
    try_ss.ss_size = 0U;
    if (sys2(186U /* sigaltstack */, (uint32_t)(uintptr_t)&try_ss, 0U) != -1 /* -EPERM */) {
        g_altstack_result = 4U;
        return;
    }

    g_altstack_result = 100U;
}

static int test14_sigaltstack(void)
{
    /* Negative: invalid ss_flags must return -EINVAL (-22) */
    struct stack32_m3 bad_ss;
    bad_ss.ss_sp = 0x10000U;
    bad_ss.ss_flags = 0x1234;
    bad_ss.ss_size = 16384U;
    if (sys2(186U /* sigaltstack */, (uint32_t)(uintptr_t)&bad_ss, 0U) != -22 /* -EINVAL */) {
        return 119;
    }

    uint32_t alt_mem = (uint32_t)sys6(192U /* mmap2 */, 0U, 16384U,
                                      3U /* PROT_READ|PROT_WRITE */,
                                      0x22U /* MAP_PRIVATE|MAP_ANONYMOUS */,
                                      0xFFFFFFFFU, 0U);
    if (alt_mem > 0xFFFFF000U) {
        return 120;
    }

    g_altstack_base = alt_mem;
    g_altstack_size = 16384U;
    g_altstack_result = 0U;

    struct stack32_m3 ss;
    ss.ss_sp = alt_mem;
    ss.ss_flags = 0;
    ss.ss_size = 16384U;
    struct stack32_m3 oss = { 0U, 0, 0U };
    if (sys2(186U /* sigaltstack */, (uint32_t)(uintptr_t)&ss,
             (uint32_t)(uintptr_t)&oss) != 0) {
        sys2(91U /* munmap */, alt_mem, 16384U);
        return 121;
    }

    /* Before signal delivery, SS_ONSTACK (1) must NOT be set */
    if (sys2(186U /* sigaltstack */, 0U, (uint32_t)(uintptr_t)&oss) != 0 ||
        (oss.ss_flags & 1) != 0 || oss.ss_sp != alt_mem || oss.ss_size != 16384U) {
        sys2(91U /* munmap */, alt_mem, 16384U);
        return 122;
    }

    struct sigaction32 act;
    struct sigaction32 old_act;
    act.sa_handler = (uint32_t)(uintptr_t)altstack_sigusr1_handler;
    /* SA_ONSTACK (0x08000000) | SA_RESTORER (0x04000000) | SA_SIGINFO (0x4) */
    act.sa_flags = 0x08000000U | 0x04000000U | 0x00000004U;
    act.sa_restorer = (uint32_t)(uintptr_t)my_rt_restorer;
    act.sa_mask[0] = 0U;
    act.sa_mask[1] = 0U;

    if (sys4(174U /* rt_sigaction */, 10U /* SIGUSR1 */,
             (uint32_t)(uintptr_t)&act, (uint32_t)(uintptr_t)&old_act, 8U) != 0) {
        sys2(91U /* munmap */, alt_mem, 16384U);
        return 123;
    }

    uint32_t tid = (uint32_t)sys1(224U /* gettid */, 0U);
    if (sys2(238U /* tkill */, tid, 10U /* SIGUSR1 */) != 0) {
        sys4(174U /* rt_sigaction */, 10U, (uint32_t)(uintptr_t)&old_act, 0U, 8U);
        sys2(91U /* munmap */, alt_mem, 16384U);
        return 124;
    }

    sys4(174U /* rt_sigaction */, 10U, (uint32_t)(uintptr_t)&old_act, 0U, 8U);

    if (g_altstack_result != 100U) {
        sys2(91U /* munmap */, alt_mem, 16384U);
        return 125;
    }

    /* After rt_sigreturn from alternate stack, SS_ONSTACK (1) must be cleared */
    if (sys2(186U /* sigaltstack */, 0U, (uint32_t)(uintptr_t)&oss) != 0 ||
        (oss.ss_flags & 1) != 0 || oss.ss_sp != alt_mem || oss.ss_size != 16384U) {
        sys2(91U /* munmap */, alt_mem, 16384U);
        return 126;
    }

    /* Disable alternate stack via SS_DISABLE (2) */
    ss.ss_sp = 0U;
    ss.ss_flags = 2 /* SS_DISABLE */;
    ss.ss_size = 0U;
    if (sys2(186U /* sigaltstack */, (uint32_t)(uintptr_t)&ss,
             (uint32_t)(uintptr_t)&oss) != 0) {
        sys2(91U /* munmap */, alt_mem, 16384U);
        return 127;
    }
    if (sys2(186U /* sigaltstack */, 0U, (uint32_t)(uintptr_t)&oss) != 0 ||
        oss.ss_flags != 2 /* SS_DISABLE */) {
        sys2(91U /* munmap */, alt_mem, 16384U);
        return 128;
    }

    sys2(91U /* munmap */, alt_mem, 16384U);
    return 0;
}

/* --------------------------------------------------------------------------
 * Test 15: UDF inside a Thumb IT block (SIGILL si_code, si_addr, arm_pc += 2,
 *          and preserved ITSTATE across rt_sigreturn)
 * -------------------------------------------------------------------------- */
uint32_t thumb_it_udf_helper(uint32_t dummy_r0, uint32_t *out_apsr);

__asm__(
    ".text\n"
    ".balign 4\n"
    ".thumb\n"
    ".thumb_func\n"
    ".globl thumb_it_udf_helper\n"
    ".type thumb_it_udf_helper, %function\n"
    "thumb_it_udf_helper:\n"
    "    .short 0x4280\n"             /* +0:  cmp r0, r0 (Z=1, C=1, N=0, V=0) */
    "    .short 0xbf01\n"             /* +2:  ittt eq */
    "    .short 0x200a\n"             /* +4:  moveq r0, #10 */
    "    .short 0xde00\n"             /* +6:  udf #0 (2-byte T16 undefined) */
    "    .short 0x2014\n"             /* +8:  moveq r0, #20 */
    "    .short 0x3005\n"             /* +10: addeq r0, r0, #5 */
    "    .short 0xf3ef, 0x8200\n"     /* +12: mrs r2, apsr */
    "    .short 0x600a\n"             /* +16: str r2, [r1, #0] */
    "    .short 0x4770\n"             /* +18: bx lr */
    ".arm\n"
);

static volatile uint32_t g_it_udf_handled = 0U;
static volatile uint32_t g_it_udf_ok = 0U;
static volatile uint32_t g_it_udf_expected_pc = 0U;

static void it_udf_sigill_handler(int sig, void *info, void *uctx_ptr)
{
    uint8_t *uc = (uint8_t *)uctx_ptr;
    uint32_t arm_r0     = *(uint32_t *)(uc + 32);
    uint32_t arm_pc     = *(uint32_t *)(uc + 92);
    uint32_t fault_addr = *(uint32_t *)(uc + 100);

    /* Advance past the 2-byte Thumb UDF instruction */
    *(uint32_t *)(uc + 92) = arm_pc + 2U;

    const struct siginfo32 *si = (const struct siginfo32 *)info;
    if (sig == 4 && si != NULL &&
        si->si_signo == 4 &&
        si->si_code == 1 /* ILL_ILLOPC */ &&
        (uint32_t)(uintptr_t)si->si_addr == g_it_udf_expected_pc &&
        arm_pc == g_it_udf_expected_pc &&
        fault_addr == g_it_udf_expected_pc &&
        arm_r0 == 10U) {
        g_it_udf_ok = 1U;
    }
    g_it_udf_handled = 1U;
}

static int test15_thumb_it_udf_sigill(void)
{
    g_it_udf_handled = 0U;
    g_it_udf_ok = 0U;
    uint32_t fn_base = ((uint32_t)(uintptr_t)thumb_it_udf_helper) & ~1U;
    g_it_udf_expected_pc = fn_base + 6U;

    struct sigaction32 act;
    struct sigaction32 old_act;
    act.sa_handler  = (uint32_t)(uintptr_t)it_udf_sigill_handler;
    act.sa_flags    = 0x04000004U; /* SA_SIGINFO | SA_RESTORER */
    act.sa_restorer = (uint32_t)(uintptr_t)my_rt_restorer;
    act.sa_mask[0]  = 0U;
    act.sa_mask[1]  = 0U;

    if (sys4(174U /* rt_sigaction */, 4U /* SIGILL */,
             (uint32_t)(uintptr_t)&act, (uint32_t)(uintptr_t)&old_act, 8U) != 0) {
        return 129;
    }

    uint32_t apsr_after = 0U;
    uint32_t r0_after = thumb_it_udf_helper(0U, &apsr_after);

    sys4(174U /* rt_sigaction */, 4U /* SIGILL */,
         (uint32_t)(uintptr_t)&old_act, 0U, 8U);

    if (g_it_udf_handled != 1U || g_it_udf_ok != 1U) {
        return 130;
    }
    /*
     * Verify the remaining two instructions in `ittt eq` (`moveq r0, #20` and
     * `addeq r0, r0, #5`) executed to produce r0 == 25, AND that ITSTATE was
     * preserved across rt_sigreturn (16-bit moveq/addeq inside an IT block do
     * not modify NZCV, so Z=1 / C=1 == 0x60000000U remains intact).
     */
    if (r0_after != 25U || (apsr_after & 0xF0000000U) != 0x60000000U) {
        return 131;
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * Test 16: Asynchronous kill(getpid(), SIGUSR1) / tgkill SA_SIGINFO si_code & si_pid
 * -------------------------------------------------------------------------- */
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

static volatile uint32_t g_kill_siginfo_calls = 0U;
static volatile int32_t  g_kill_last_signo = 0;
static volatile int32_t  g_kill_last_code = 999;
static volatile int32_t  g_kill_last_pid = -1;

static void kill_siginfo_handler(int sig, void *info, void *ucontext)
{
    (void)ucontext;
    g_kill_siginfo_calls++;
    g_kill_last_signo = sig;
    if (info != NULL) {
        const struct siginfo32_kill *si = (const struct siginfo32_kill *)info;
        g_kill_last_code = si->si_code;
        g_kill_last_pid  = si->si_pid;
    }
}

static int test16_async_kill_tgkill_siginfo(void)
{
    struct sigaction32 act;
    struct sigaction32 old_act;
    act.sa_handler  = (uint32_t)(uintptr_t)kill_siginfo_handler;
    act.sa_flags    = 0x04000004U; /* SA_SIGINFO | SA_RESTORER */
    act.sa_restorer = (uint32_t)(uintptr_t)my_rt_restorer;
    act.sa_mask[0]  = 0U;
    act.sa_mask[1]  = 0U;

    if (sys4(174U /* rt_sigaction */, 10U /* SIGUSR1 */,
             (uint32_t)(uintptr_t)&act, (uint32_t)(uintptr_t)&old_act, 8U) != 0) {
        return 132;
    }

    int32_t my_pid = sys1(20U /* getpid */, 0U);
    int32_t my_tid = sys1(224U /* gettid */, 0U);
    if (my_pid <= 0 || my_tid <= 0) {
        sys4(174U /* rt_sigaction */, 10U, (uint32_t)(uintptr_t)&old_act, 0U, 8U);
        return 133;
    }

    /* 16a: kill(getpid(), SIGUSR1) -> si_code <= 0 (SI_USER == 0) and si_pid == getpid() */
    g_kill_siginfo_calls = 0U;
    g_kill_last_signo = 0;
    g_kill_last_code = 999;
    g_kill_last_pid = -1;

    if (sys2(37U /* kill */, (uint32_t)my_pid, 10U /* SIGUSR1 */) != 0) {
        sys4(174U /* rt_sigaction */, 10U, (uint32_t)(uintptr_t)&old_act, 0U, 8U);
        return 134;
    }
    if (g_kill_siginfo_calls != 1U ||
        g_kill_last_signo != 10 ||
        g_kill_last_code > 0 ||
        (g_kill_last_code != 0 /* SI_USER */ && g_kill_last_code != -6 /* SI_TKILL */) ||
        g_kill_last_pid != my_pid) {
        sys4(174U /* rt_sigaction */, 10U, (uint32_t)(uintptr_t)&old_act, 0U, 8U);
        return 135;
    }

    /* 16b: tgkill(getpid(), gettid(), SIGUSR1) -> si_code <= 0 (SI_TKILL == -6) and si_pid == getpid() */
    g_kill_siginfo_calls = 0U;
    g_kill_last_signo = 0;
    g_kill_last_code = 999;
    g_kill_last_pid = -1;

    if (sys3(268U /* tgkill */, (uint32_t)my_pid, (uint32_t)my_tid, 10U /* SIGUSR1 */) != 0) {
        sys4(174U /* rt_sigaction */, 10U, (uint32_t)(uintptr_t)&old_act, 0U, 8U);
        return 136;
    }
    sys4(174U /* rt_sigaction */, 10U, (uint32_t)(uintptr_t)&old_act, 0U, 8U);

    if (g_kill_siginfo_calls != 1U ||
        g_kill_last_signo != 10 ||
        g_kill_last_code > 0 ||
        (g_kill_last_code != -6 /* SI_TKILL */ && g_kill_last_code != 0 /* SI_USER */) ||
        g_kill_last_pid != my_pid) {
        return 137;
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
    rc = test10_clone_process_fork();
    if (rc != 0) {
        return rc;
    }
    rc = test11_sigill_fault_addr();
    if (rc != 0) {
        return rc;
    }
    rc = test12_chaining_partial_cacheflush();
    if (rc != 0) {
        return rc;
    }
    rc = test13_futex_bitset_requeue();
    if (rc != 0) {
        return rc;
    }
    rc = test14_sigaltstack();
    if (rc != 0) {
        return rc;
    }
    rc = test15_thumb_it_udf_sigill();
    if (rc != 0) {
        return rc;
    }
    rc = test16_async_kill_tgkill_siginfo();
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
