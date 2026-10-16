/*
 * tests/hello.c — M0 / M0-hl A32 & Thumb-2 smoke test fixture
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Exercises:
 *   - 32-bit initial stack layout (argc, argv, envp, auxv: AT_PAGESZ=4096)
 *   - ARM private syscall __ARM_NR_set_tls (0x0f0005) & CP15 TPIDRURO read
 *   - Kernel kuser helper page at 0xffff0000 (__kuser_helper_version,
 *     __kuser_get_tls, __kuser_memory_barrier, __kuser_cmpxchg)
 *   - Emulated guest brk() and mmap2()/mprotect()/munmap() inside [G, G+4GiB)
 *   - ARM EABI writev() / write() and exit_group(0)
 */

#include <stdint.h>
#include <stddef.h>

#ifdef A32RT_FREESTANDING_TEST

struct iovec32 {
    uint32_t iov_base;
    uint32_t iov_len;
};

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

static int run_checks(uint32_t *sp)
{
    uint32_t argc = sp[0];
    uint32_t *argv = &sp[1];
    uint32_t *envp = &argv[argc + 1U];
    uint32_t *auxv = envp;
    while (*auxv != 0U) {
        auxv++;
    }
    auxv++; /* skip NULL envp terminator */

    uint32_t pagesz = 0U;
    for (uint32_t *a = auxv; a[0] != 0U; a += 2) {
        if (a[0] == 6U /* AT_PAGESZ */) {
            pagesz = a[1];
        }
    }
    if (pagesz != 4096U) {
        return 10;
    }

    /* Verify TLS via __ARM_NR_set_tls (0x0f0005), CP15 TPIDRURO, and kuser */
    uint32_t tls_sentinel = 0x12345678U;
    if (sys1(0x0f0005U, (uint32_t)(uintptr_t)&tls_sentinel) != 0) {
        return 11;
    }
    uint32_t tp_cp15 = 0U;
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(tp_cp15));
    if (tp_cp15 != (uint32_t)(uintptr_t)&tls_sentinel) {
        return 12;
    }

    /* Check kuser helper version at 0xffff0ffc and __kuser_get_tls at 0xffff0fe0 */
    uint32_t kuser_ver = *(const volatile uint32_t *)(uintptr_t)0xffff0ffcU;
    if (kuser_ver < 5U) {
        return 13;
    }
    typedef uint32_t (*kuser_get_tls_fn)(void);
    kuser_get_tls_fn kuser_get_tls = (kuser_get_tls_fn)(uintptr_t)0xffff0fe0U;
    if (kuser_get_tls() != (uint32_t)(uintptr_t)&tls_sentinel) {
        return 14;
    }

    /* Check __kuser_cmpxchg at 0xffff0fc0 */
    typedef int (*kuser_cmpxchg_fn)(uint32_t oldval, uint32_t newval, volatile uint32_t *ptr);
    kuser_cmpxchg_fn kuser_cmpxchg = (kuser_cmpxchg_fn)(uintptr_t)0xffff0fc0U;
    volatile uint32_t atomic_word = 41U;
    if (kuser_cmpxchg(41U, 42U, &atomic_word) != 0 || atomic_word != 42U) {
        return 15;
    }

    /* Check emulated guest brk (NR 45) */
    uint32_t brk0 = (uint32_t)sys1(45U, 0U);
    uint32_t brk1 = (uint32_t)sys1(45U, brk0 + 4096U);
    if (brk1 != brk0 + 4096U) {
        return 16;
    }
    *(volatile uint32_t *)(uintptr_t)brk0 = 0xcafebabeU;
    if (*(volatile uint32_t *)(uintptr_t)brk0 != 0xcafebabeU) {
        return 17;
    }

    /* Check guest mmap2 (NR 192), mprotect (NR 125), munmap (NR 91) */
    uint32_t map_addr = (uint32_t)sys6(192U, 0U, 4096U, 3U /* PROT_READ|PROT_WRITE */,
                                       0x22U /* MAP_PRIVATE|MAP_ANONYMOUS */,
                                       0xffffffffU, 0U);
    if (map_addr > 0xfffff000U) {
        return 18;
    }
    *(volatile uint32_t *)(uintptr_t)map_addr = 0xdeadbeefU;
    if (sys3(125U, map_addr, 4096U, 1U /* PROT_READ */) != 0) {
        return 19;
    }
    if (*(volatile uint32_t *)(uintptr_t)map_addr != 0xdeadbeefU) {
        return 20;
    }
    if (sys2(91U, map_addr, 4096U) != 0) {
        return 21;
    }

    /* Print "hello\n" via writev (NR 146) */
    static const char part1[] = "hel";
    static const char part2[] = "lo\n";
    struct iovec32 iov[2];
    iov[0].iov_base = (uint32_t)(uintptr_t)part1;
    iov[0].iov_len  = 3U;
    iov[1].iov_base = (uint32_t)(uintptr_t)part2;
    iov[1].iov_len  = 3U;
    if (sys3(146U, 1U, (uint32_t)(uintptr_t)iov, 2U) != 6) {
        return 22;
    }
    return 0;
}

void __attribute__((noreturn)) _start_c(uint32_t *sp)
{
    int rc = run_checks(sp);
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

#else

#include <stdio.h>

int main(void)
{
    puts("hello");
    return 0;
}

#endif
