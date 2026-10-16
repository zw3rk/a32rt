/*
 * tests/test_m4_vfp_neon.c — M4 / M4-hl VFPv4-D32 & NEON test fixture
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stddef.h>

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
    ".globl __aeabi_uidivmod\n"
    ".type __aeabi_uidivmod, %function\n"
    "__aeabi_uidivmod:\n"
    "    udiv r2, r0, r1\n"
    "    mls r1, r2, r1, r0\n"
    "    mov r0, r2\n"
    "    bx lr\n"
    ".globl __aeabi_idivmod\n"
    ".type __aeabi_idivmod, %function\n"
    "__aeabi_idivmod:\n"
    "    sdiv r2, r0, r1\n"
    "    mls r1, r2, r1, r0\n"
    "    mov r0, r2\n"
    "    bx lr\n"
);

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

static int run_vfp_neon_tests(void)
{
    /* 1. Scalar float arithmetic (s0..s31) */
    volatile float f1 = 12.5f;
    volatile float f2 = 2.5f;
    volatile float f_add = f1 + f2;
    volatile float f_sub = f1 - f2;
    volatile float f_mul = f1 * f2;
    volatile float f_div = f1 / f2;
    if (f_add != 15.0f || f_sub != 10.0f || f_mul != 31.25f || f_div != 5.0f) {
        return 1;
    }

    /* 2. vsqrt.f32 */
    float f_sq_in = 64.0f;
    float f_sq_out = 0.0f;
    __asm__ volatile("vsqrt.f32 %0, %1" : "=t"(f_sq_out) : "t"(f_sq_in));
    if (f_sq_out != 8.0f) {
        return 2;
    }

    /* 3. vfma.f32 */
    float fma_acc = 10.0f;
    float fma_a = 3.0f;
    float fma_b = 4.0f;
    __asm__ volatile("vfma.f32 %0, %1, %2" : "+t"(fma_acc) : "t"(fma_a), "t"(fma_b));
    if (fma_acc != 22.0f) {
        return 3;
    }

    /* 4. Scalar double arithmetic (d0..d15) */
    volatile double d1 = 100.5;
    volatile double d2 = 4.0;
    volatile double d_add = d1 + d2;
    volatile double d_sub = d1 - d2;
    volatile double d_mul = d1 * d2;
    volatile double d_div = d1 / d2;
    if (d_add != 104.5 || d_sub != 96.5 || d_mul != 402.0 || d_div != 25.125) {
        return 4;
    }

    /* 5. vsqrt.f64 */
    double d_sq_in = 144.0;
    double d_sq_out = 0.0;
    __asm__ volatile("vsqrt.f64 %0, %1" : "=w"(d_sq_out) : "w"(d_sq_in));
    if (d_sq_out != 12.0) {
        return 5;
    }

    /* 6. vfma.f64 */
    double dfma_acc = 1.5;
    double dfma_a = 2.5;
    double dfma_b = 4.0;
    __asm__ volatile("vfma.f64 %0, %1, %2" : "+w"(dfma_acc) : "w"(dfma_a), "w"(dfma_b));
    if (dfma_acc != 11.5) {
        return 6;
    }

    /* 7. High registers d16..d31 */
    double high_in = 7.25;
    double high_out = 0.0;
    __asm__ volatile(
        "vmov d16, %1\n"
        "vmov d31, %1\n"
        "vadd.f64 d17, d16, d31\n"
        "vmov %0, d17\n"
        : "=w"(high_out)
        : "w"(high_in)
        : "d16", "d17", "d31"
    );
    if (high_out != 14.5) {
        return 7;
    }

    /* 8. Float <-> Int conversions */
    float conv_f = -42.0f;
    int32_t conv_i = 0;
    __asm__ volatile(
        "vcvt.s32.f32 %0, %1\n"
        : "=t"(conv_i)
        : "t"(conv_f)
    );
    if (conv_i != -42) {
        return 8;
    }

    int32_t to_f_i = 123;
    float back_f = 0.0f;
    __asm__ volatile(
        "vcvt.f32.s32 %0, %1\n"
        : "=t"(back_f)
        : "t"(to_f_i)
    );
    if (back_f != 123.0f) {
        return 9;
    }

    /* 9. vcmp + vmrs APSR_nzcv, fpscr comparison */
    float c_a = 5.0f;
    float c_b = 10.0f;
    int is_lt = 0;
    __asm__ volatile(
        "vcmp.f32 %1, %2\n"
        "vmrs APSR_nzcv, fpscr\n"
        "blt 1f\n"
        "mov %0, #0\n"
        "b 2f\n"
        "1:\n"
        "mov %0, #1\n"
        "2:\n"
        : "=r"(is_lt)
        : "t"(c_a), "t"(c_b)
        : "cc"
    );
    if (is_lt != 1) {
        return 10;
    }

    /* 10. NEON 128-bit vector integer addition (vadd.i32 q0, q1, q2) */
    int32_t in_a[4] = { 10, 20, 30, 40 };
    int32_t in_b[4] = { 1, 2, 3, 4 };
    int32_t out_add[4] = { 0, 0, 0, 0 };
    __asm__ volatile(
        "vld1.32 {d2, d3}, [%1]\n"
        "vld1.32 {d4, d5}, [%2]\n"
        "vadd.i32 q0, q1, q2\n"
        "vst1.32 {d0, d1}, [%0]\n"
        :
        : "r"(out_add), "r"(in_a), "r"(in_b)
        : "d0", "d1", "d2", "d3", "d4", "d5", "memory"
    );
    if (out_add[0] != 11 || out_add[1] != 22 || out_add[2] != 33 || out_add[3] != 44) {
        return 11;
    }

    /* 11. NEON 128-bit vector float multiplication (vmul.f32 q0, q1, q2) */
    float vf_a[4] = { 1.5f, 2.0f, 3.0f, 4.0f };
    float vf_b[4] = { 2.0f, 3.0f, 4.0f, 5.0f };
    float vf_out[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    __asm__ volatile(
        "vld1.32 {d2, d3}, [%1]\n"
        "vld1.32 {d4, d5}, [%2]\n"
        "vmul.f32 q0, q1, q2\n"
        "vst1.32 {d0, d1}, [%0]\n"
        :
        : "r"(vf_out), "r"(vf_a), "r"(vf_b)
        : "d0", "d1", "d2", "d3", "d4", "d5", "memory"
    );
    if (vf_out[0] != 3.0f || vf_out[1] != 6.0f || vf_out[2] != 12.0f || vf_out[3] != 20.0f) {
        return 12;
    }

    static const char pass_msg[] = "M4 VFPv4-D32 & NEON: PASS\n";
    sys3(4U /* write */, 1U, (uint32_t)(uintptr_t)pass_msg, (uint32_t)(sizeof(pass_msg) - 1U));
    return 0;
}

void __attribute__((noreturn)) _start_c(void)
{
    int rc = run_vfp_neon_tests();
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
