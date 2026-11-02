/*
 * tests/test_m4_common.h — Shared helpers for M4 VFPv4-D32 & NEON tests
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef A32_TEST_M4_COMMON_H
#define A32_TEST_M4_COMMON_H

#include <stdint.h>
#include <stddef.h>


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


int run_vfp_neon_part2(void);
int run_vfp_neon_part3(void);
int run_section32_armv8_vfp_tests(void);
int run_m6_neon_aliasing_tests(void);

#endif /* A32_TEST_M4_COMMON_H */
