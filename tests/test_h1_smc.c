/*
 * tests/test_h1_smc.c — H1 self-modifying-code fixture (A32).
 *
 * Patches smc_target (mov r0, #1 -> mov r0, #2) after mprotecting its
 * page writable, issues __ARM_NR_cacheflush, then executes the patched
 * code.  The DBT must invalidate the stale translation (cacheflush ->
 * a32_jit_invalidate_range); the interpreter executes the new bytes
 * directly.  Prints H1_SMC lines; exit 0 iff both calls observe the
 * expected values.
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stddef.h>

#ifdef A32RT_FREESTANDING_TEST

static inline int32_t sys1(uint32_t nr, uint32_t a0)
{
    register uint32_t r7 __asm__("r7") = nr;
    register uint32_t r0 __asm__("r0") = a0;
    __asm__ volatile("svc #0" : "+r"(r0) : "r"(r7) : "memory", "cc");
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

__asm__(
    ".text\n"
    ".globl smc_target\n"
    ".type smc_target, %function\n"
    "smc_target:\n"
    "    mov r0, #1\n"
    "    bx lr\n");

extern uint32_t smc_target(void);

static int g_fail = 0;

static void puts_(const char *s)
{
    const char *p = s;
    while (*p) {
        p++;
    }
    sys3(4U /* write */, 1U, (uint32_t)s, (uint32_t)(p - s));
}

static void report(const char *name, int ok)
{
    puts_(name);
    puts_(ok ? " ok\n" : " FAIL\n");
    if (!ok) {
        g_fail++;
    }
}

int cmain(void)
{
    uint32_t (*volatile call_target)(void) = smc_target;
    uint32_t v0 = call_target();
    report("H1_SMC exec-before-patch", v0 == 1U);

    uint32_t page = ((uint32_t)smc_target) & ~0xFFFU;
    int32_t mpr = sys3(125U /* mprotect */, page, 4096U, 7U /* RWX */);
    report("H1_SMC mprotect-rwx", mpr == 0);

    volatile uint32_t *slot = (volatile uint32_t *)smc_target;
    *slot = 0xE3A00002U; /* mov r0, #2 */
    sys3(0x0F0002U /* cacheflush */, (uint32_t)smc_target,
         (uint32_t)smc_target + 8U, 0U);

    uint32_t v1 = call_target();
    report("H1_SMC exec-after-patch", v1 == 2U);

    puts_(g_fail == 0 ? "H1_SMC ALL_OK\n" : "H1_SMC FAIL\n");
    return g_fail == 0 ? 0 : 1;
}

void __attribute__((noreturn)) _start_c(uint32_t *sp)
{
    (void)sp;
    int rc = cmain();
    sys1(248U /* exit_group */, (uint32_t)rc);
    while (1) {
    }
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
    "    mov r7, #1\n"
    "    svc #0\n");

#else

int main(void)
{
    return 0;
}

#endif
