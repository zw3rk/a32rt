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

    /* 12. 64-bit D-register (!is_q) integer SIMD:
     *     vadd.i64, vsub.i64, vshl.i64, vshr.u64, vshr.s64, vshl.u64, vshl.s64 */
    uint64_t d64_a = 0x0123456789ABCDEFULL;
    uint64_t d64_b = 0x1000000080000002ULL;
    uint64_t d64_out = 0ULL;
    __asm__ volatile(
        "vldr d1, [%1]\n"
        "vldr d2, [%2]\n"
        "vadd.i64 d0, d1, d2\n"
        "vstr d0, [%0]\n"
        :
        : "r"(&d64_out), "r"(&d64_a), "r"(&d64_b)
        : "d0", "d1", "d2", "memory"
    );
    if (d64_out != (d64_a + d64_b)) {
        return 13;
    }

    __asm__ volatile(
        "vldr d1, [%1]\n"
        "vldr d2, [%2]\n"
        "vsub.i64 d0, d1, d2\n"
        "vstr d0, [%0]\n"
        :
        : "r"(&d64_out), "r"(&d64_a), "r"(&d64_b)
        : "d0", "d1", "d2", "memory"
    );
    if (d64_out != (d64_a - d64_b)) {
        return 14;
    }

    uint64_t sh_in = 0xF102030405060780ULL;
    uint64_t shl_imm_out = 0ULL;
    uint64_t shr_u_out = 0ULL;
    uint64_t shr_s_out = 0ULL;
    __asm__ volatile(
        "vldr d1, [%3]\n"
        "vshl.i64 d0, d1, #7\n"
        "vstr d0, [%0]\n"
        "vshr.u64 d0, d1, #4\n"
        "vstr d0, [%1]\n"
        "vshr.s64 d0, d1, #4\n"
        "vstr d0, [%2]\n"
        :
        : "r"(&shl_imm_out), "r"(&shr_u_out), "r"(&shr_s_out), "r"(&sh_in)
        : "d0", "d1", "memory"
    );
    if (shl_imm_out != (sh_in << 7) ||
        shr_u_out != (sh_in >> 4) ||
        shr_s_out != (uint64_t)((int64_t)sh_in >> 4)) {
        return 15;
    }

    uint64_t sh_cnt_pos = 8ULL;
    uint64_t sh_cnt_neg = (uint64_t)(int64_t)-4;
    uint64_t vshl_u64_pos = 0ULL;
    uint64_t vshl_u64_neg = 0ULL;
    uint64_t vshl_s64_pos = 0ULL;
    uint64_t vshl_s64_neg = 0ULL;
    __asm__ volatile(
        "vldr d1, [%4]\n"
        "vldr d2, [%5]\n"
        "vshl.u64 d0, d1, d2\n"
        "vstr d0, [%0]\n"
        "vshl.s64 d0, d1, d2\n"
        "vstr d0, [%2]\n"
        "vldr d2, [%6]\n"
        "vshl.u64 d0, d1, d2\n"
        "vstr d0, [%1]\n"
        "vshl.s64 d0, d1, d2\n"
        "vstr d0, [%3]\n"
        :
        : "r"(&vshl_u64_pos), "r"(&vshl_u64_neg),
          "r"(&vshl_s64_pos), "r"(&vshl_s64_neg),
          "r"(&sh_in), "r"(&sh_cnt_pos), "r"(&sh_cnt_neg)
        : "d0", "d1", "d2", "memory"
    );
    if (vshl_u64_pos != (sh_in << 8) ||
        vshl_u64_neg != (sh_in >> 4) ||
        vshl_s64_pos != (sh_in << 8) ||
        vshl_s64_neg != (uint64_t)((int64_t)sh_in >> 4)) {
        return 16;
    }

    /* 13. VSHL (register) operand order: Vm shifted by Vn (positive & negative counts) */
    uint32_t vshl_d_val[2] = { 0x00000010U, 0x80000000U };
    int32_t  vshl_d_cnt[2] = { 3, -4 };
    uint32_t vshl_d_out[2] = { 0U, 0U };
    __asm__ volatile(
        "vld1.32 {d1}, [%1]\n"
        "vld1.32 {d2}, [%2]\n"
        "vshl.u32 d0, d1, d2\n"
        "vst1.32 {d0}, [%0]\n"
        :
        : "r"(vshl_d_out), "r"(vshl_d_val), "r"(vshl_d_cnt)
        : "d0", "d1", "d2", "memory"
    );
    if (vshl_d_out[0] != 0x00000080U || vshl_d_out[1] != 0x08000000U) {
        return 17;
    }

    int32_t vshl_q_val[4] = { 5, -64, 0x100, -1024 };
    int32_t vshl_q_cnt[4] = { 4, -2, 3, -5 };
    int32_t vshl_q_out[4] = { 0, 0, 0, 0 };
    __asm__ volatile(
        "vld1.32 {d2, d3}, [%1]\n"
        "vld1.32 {d4, d5}, [%2]\n"
        "vshl.s32 q0, q1, q2\n"
        "vst1.32 {d0, d1}, [%0]\n"
        :
        : "r"(vshl_q_out), "r"(vshl_q_val), "r"(vshl_q_cnt)
        : "d0", "d1", "d2", "d3", "d4", "d5", "memory"
    );
    if (vshl_q_out[0] != 80 || vshl_q_out[1] != -16 ||
        vshl_q_out[2] != 2048 || vshl_q_out[3] != -32) {
        return 18;
    }

    /* 14. VMAX and VMIN on both low (d0-d4, N=0) and high (d16-d20, N=1) registers */
    int32_t s32_in1[2] = { -10, 20 };
    int32_t s32_in2[2] = { -5, 15 };
    int32_t s32_max_lo[2] = { 0, 0 };
    int32_t s32_min_lo[2] = { 0, 0 };
    int32_t s32_max_hi[2] = { 0, 0 };
    int32_t s32_min_hi[2] = { 0, 0 };
    __asm__ volatile(
        "vld1.32 {d1}, [%4]\n"
        "vld1.32 {d2}, [%5]\n"
        "vmax.s32 d0, d1, d2\n"
        "vmin.s32 d3, d1, d2\n"
        "vst1.32 {d0}, [%0]\n"
        "vst1.32 {d3}, [%1]\n"
        "vld1.32 {d16}, [%4]\n"
        "vld1.32 {d17}, [%5]\n"
        "vmax.s32 d18, d16, d17\n"
        "vmin.s32 d19, d16, d17\n"
        "vst1.32 {d18}, [%2]\n"
        "vst1.32 {d19}, [%3]\n"
        :
        : "r"(s32_max_lo), "r"(s32_min_lo), "r"(s32_max_hi), "r"(s32_min_hi),
          "r"(s32_in1), "r"(s32_in2)
        : "d0", "d1", "d2", "d3", "d16", "d17", "d18", "d19", "memory"
    );
    if (s32_max_lo[0] != -5 || s32_max_lo[1] != 20 ||
        s32_min_lo[0] != -10 || s32_min_lo[1] != 15 ||
        s32_max_hi[0] != -5 || s32_max_hi[1] != 20 ||
        s32_min_hi[0] != -10 || s32_min_hi[1] != 15) {
        return 19;
    }

    uint16_t u16_in1[4] = { 0x8000U, 0x0010U, 0xFFFFU, 0x1234U };
    uint16_t u16_in2[4] = { 0x7FFFU, 0x0020U, 0x0001U, 0x5678U };
    uint16_t u16_max_lo[4] = { 0U, 0U, 0U, 0U };
    uint16_t u16_min_lo[4] = { 0U, 0U, 0U, 0U };
    uint16_t u16_max_hi[4] = { 0U, 0U, 0U, 0U };
    uint16_t u16_min_hi[4] = { 0U, 0U, 0U, 0U };
    __asm__ volatile(
        "vld1.16 {d1}, [%4]\n"
        "vld1.16 {d2}, [%5]\n"
        "vmax.u16 d0, d1, d2\n"
        "vmin.u16 d4, d1, d2\n"
        "vst1.16 {d0}, [%0]\n"
        "vst1.16 {d4}, [%1]\n"
        "vld1.16 {d16}, [%4]\n"
        "vld1.16 {d17}, [%5]\n"
        "vmax.u16 d18, d16, d17\n"
        "vmin.u16 d20, d16, d17\n"
        "vst1.16 {d18}, [%2]\n"
        "vst1.16 {d20}, [%3]\n"
        :
        : "r"(u16_max_lo), "r"(u16_min_lo), "r"(u16_max_hi), "r"(u16_min_hi),
          "r"(u16_in1), "r"(u16_in2)
        : "d0", "d1", "d2", "d4", "d16", "d17", "d18", "d20", "memory"
    );
    if (u16_max_lo[0] != 0x8000U || u16_max_lo[1] != 0x0020U ||
        u16_max_lo[2] != 0xFFFFU || u16_max_lo[3] != 0x5678U ||
        u16_min_lo[0] != 0x7FFFU || u16_min_lo[1] != 0x0010U ||
        u16_min_lo[2] != 0x0001U || u16_min_lo[3] != 0x1234U ||
        u16_max_hi[0] != 0x8000U || u16_max_hi[1] != 0x0020U ||
        u16_max_hi[2] != 0xFFFFU || u16_max_hi[3] != 0x5678U ||
        u16_min_hi[0] != 0x7FFFU || u16_min_hi[1] != 0x0010U ||
        u16_min_hi[2] != 0x0001U || u16_min_hi[3] != 0x1234U) {
        return 20;
    }

    /* 15. VMOV Rt, Rt2, Sm, Sm1 vs VMOV Rt, Rt2, Dm and VMOV Sm, Sm1, Rt, Rt2 */
    uint32_t s2_in = 0x3F800000U; /* 1.0f */
    uint32_t s3_in = 0x40000000U; /* 2.0f */
    uint32_t r0_from_s = 0U, r1_from_s = 0U;
    uint32_t r0_from_d = 0U, r1_from_d = 0U;
    uint32_t s4_out = 0U, s5_out = 0U;
    __asm__ volatile(
        "vmov s2, %4\n"
        "vmov s3, %5\n"
        "vmov r0, r1, s2, s3\n"
        "mov %0, r0\n"
        "mov %1, r1\n"
        "vmov s4, s5, r0, r1\n"
        "vmov r0, r1, d2\n"
        "mov %2, r0\n"
        "mov %3, r1\n"
        : "=&r"(r0_from_s), "=&r"(r1_from_s), "=&r"(r0_from_d), "=&r"(r1_from_d)
        : "r"(s2_in), "r"(s3_in)
        : "r0", "r1", "d1", "d2"
    );
    __asm__ volatile(
        "vmov %0, s4\n"
        "vmov %1, s5\n"
        : "=r"(s4_out), "=r"(s5_out)
        :
        : "d2"
    );
    if (r0_from_s != s2_in || r1_from_s != s3_in ||
        r0_from_d != s2_in || r1_from_d != s3_in ||
        s4_out != s2_in || s5_out != s3_in) {
        return 21;
    }

    /* Also test odd single-precision pair (s1, s2 where M=1) vs high D register (d17 where M=1) */
    uint32_t odd_s1_in = 0x40400000U; /* 3.0f */
    uint32_t odd_s2_in = 0x40800000U; /* 4.0f */
    uint32_t odd_r0_out = 0U, odd_r1_out = 0U;
    uint32_t odd_s1_chk = 0U, odd_s2_chk = 0U;
    __asm__ volatile(
        "mov r0, %4\n"
        "mov r1, %5\n"
        "vmov s1, s2, r0, r1\n"
        "vmov %2, s1\n"
        "vmov %3, s2\n"
        "mov r0, #0\n"
        "mov r1, #0\n"
        "vmov r0, r1, s1, s2\n"
        "mov %0, r0\n"
        "mov %1, r1\n"
        : "=&r"(odd_r0_out), "=&r"(odd_r1_out), "=&r"(odd_s1_chk), "=&r"(odd_s2_chk)
        : "r"(odd_s1_in), "r"(odd_s2_in)
        : "r0", "r1", "d0", "d1", "d16"
    );
    if (odd_r0_out != odd_s1_in || odd_r1_out != odd_s2_in ||
        odd_s1_chk != odd_s1_in || odd_s2_chk != odd_s2_in) {
        return 22;
    }

    /* 16. VCMP preserving integer CPSR.NZCV before VMRS, and VCVTR rounding vs VCVT */
    uint32_t cmp_f0_bits = 0x40A00000U; /* 5.0f */
    uint32_t cmp_f1_bits = 0x41200000U; /* 10.0f */
    uint32_t eq_a = 77U;
    uint32_t eq_b = 77U;
    int int_z_preserved = 0;
    int fp_lt_transferred = 0;
    __asm__ volatile(
        "vmov s0, %4\n"
        "vmov s1, %5\n"
        "cmp %2, %3\n"
        "vcmp.f32 s0, s1\n"
        "beq 1f\n"
        "mov %0, #0\n"
        "b 2f\n"
        "1:\n"
        "mov %0, #1\n"
        "2:\n"
        "vmrs APSR_nzcv, fpscr\n"
        "blt 3f\n"
        "mov %1, #0\n"
        "b 4f\n"
        "3:\n"
        "mov %1, #1\n"
        "4:\n"
        : "=&r"(int_z_preserved), "=&r"(fp_lt_transferred)
        : "r"(eq_a), "r"(eq_b), "r"(cmp_f0_bits), "r"(cmp_f1_bits)
        : "d0", "cc"
    );
    if (int_z_preserved != 1 || fp_lt_transferred != 1) {
        return 23;
    }

    float r_pos_f = 2.6f;
    float r_neg_f = -2.6f;
    int32_t vcvtr_pos_s32 = 0, vcvtr_neg_s32 = 0;
    int32_t vcvt_pos_s32 = 0, vcvt_neg_s32 = 0;
    __asm__ volatile(
        "vcvtr.s32.f32 %0, %4\n"
        "vcvtr.s32.f32 %1, %5\n"
        "vcvt.s32.f32  %2, %4\n"
        "vcvt.s32.f32  %3, %5\n"
        : "=&t"(vcvtr_pos_s32), "=&t"(vcvtr_neg_s32),
          "=&t"(vcvt_pos_s32), "=t"(vcvt_neg_s32)
        : "t"(r_pos_f), "t"(r_neg_f)
    );
    if (vcvtr_pos_s32 != 3 || vcvtr_neg_s32 != -3 ||
        vcvt_pos_s32 != 2 || vcvt_neg_s32 != -2) {
        return 24;
    }

    double r_pos_d = 2.6;
    double r_neg_d = -2.6;
    int32_t vcvtr_d_pos = 0, vcvtr_d_neg = 0;
    int32_t vcvt_d_pos = 0, vcvt_d_neg = 0;
    __asm__ volatile(
        "vcvtr.s32.f64 %0, %4\n"
        "vcvtr.s32.f64 %1, %5\n"
        "vcvt.s32.f64  %2, %4\n"
        "vcvt.s32.f64  %3, %5\n"
        : "=&t"(vcvtr_d_pos), "=&t"(vcvtr_d_neg),
          "=&t"(vcvt_d_pos), "=t"(vcvt_d_neg)
        : "w"(r_pos_d), "w"(r_neg_d)
    );
    if (vcvtr_d_pos != 3 || vcvtr_d_neg != -3 ||
        vcvt_d_pos != 2 || vcvt_d_neg != -2) {
        return 25;
    }

    /* 17. NEON modified immediate VMOV.F32 (cmode == 15) and VMVN/VMOV shifting ones (cmode == 13) */
    uint64_t ones64 = 0xFFFFFFFFFFFFFFFFULL;
    uint64_t f32_imm_d0 = 0ULL;
    __asm__ volatile(
        "vldr d0, [%1]\n"
        "vmov.f32 d0, #1.0e+0\n"
        "vstr d0, [%0]\n"
        :
        : "r"(&f32_imm_d0), "r"(&ones64)
        : "d0", "memory"
    );
    if (f32_imm_d0 != 0x3F8000003F800000ULL) {
        return 26;
    }

    uint64_t ones128[2] = { 0xFFFFFFFFFFFFFFFFULL, 0xFFFFFFFFFFFFFFFFULL };
    uint64_t f32_imm_q0[2] = { 0ULL, 0ULL };
    __asm__ volatile(
        "vld1.64 {d0, d1}, [%1]\n"
        "vmov.f32 q0, #1.0e+0\n"
        "vst1.64 {d0, d1}, [%0]\n"
        :
        : "r"(f32_imm_q0), "r"(ones128)
        : "d0", "d1", "memory"
    );
    if (f32_imm_q0[0] != 0x3F8000003F800000ULL ||
        f32_imm_q0[1] != 0x3F8000003F800000ULL) {
        return 27;
    }

    uint64_t cmode13_vmov_d0 = 0ULL;
    uint64_t cmode13_vmvn_d0 = 0ULL;
    uint64_t zeroes64 = 0ULL;
    __asm__ volatile(
        "vldr d0, [%2]\n"
        "vmov.i32 d0, #0xabffff\n"
        "vstr d0, [%0]\n"
        "vldr d0, [%3]\n"
        "vmvn.i32 d0, #0xabffff\n"
        "vstr d0, [%1]\n"
        :
        : "r"(&cmode13_vmov_d0), "r"(&cmode13_vmvn_d0),
          "r"(&ones64), "r"(&zeroes64)
        : "d0", "memory"
    );
    if (cmode13_vmov_d0 != 0x00ABFFFF00ABFFFFULL ||
        cmode13_vmvn_d0 != 0xFF540000FF540000ULL) {
        return 28;
    }

    /* 18. Regression tests for VQMOVN.S16, VQMOVN.U16, and VQMOVUN.S16 (saturation vs truncation) */
    {
        int16_t q_s16[8]   = { -300, -128, -42, 0, 42, 127, 128, 1000 };
        int8_t  out_s8[8]  = { 0, 0, 0, 0, 0, 0, 0, 0 };
        static const int8_t exp_s8[8] = { -128, -128, -42, 0, 42, 127, 127, 127 };
        __asm__ volatile(
            "vld1.16 {d2, d3}, [%1]\n"
            "vqmovn.s16 d0, q1\n"
            "vst1.8 {d0}, [%0]\n"
            :
            : "r"(out_s8), "r"(q_s16)
            : "d0", "d2", "d3", "memory"
        );
        for (int i = 0; i < 8; i++) {
            if (out_s8[i] != exp_s8[i]) {
                return 29;
            }
        }

        uint16_t q_u16[8]  = { 0U, 1U, 127U, 255U, 256U, 0x0102U, 0x7FFFU, 0xFFFFU };
        uint8_t  out_u8[8] = { 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U };
        static const uint8_t exp_u8[8] = { 0U, 1U, 127U, 255U, 255U, 255U, 255U, 255U };
        __asm__ volatile(
            "vld1.16 {d2, d3}, [%1]\n"
            "vqmovn.u16 d0, q1\n"
            "vst1.8 {d0}, [%0]\n"
            :
            : "r"(out_u8), "r"(q_u16)
            : "d0", "d2", "d3", "memory"
        );
        for (int i = 0; i < 8; i++) {
            if (out_u8[i] != exp_u8[i]) {
                return 30;
            }
        }

        int16_t q_su16[8]   = { -500, -1, 0, 42, 127, 200, 255, 256 };
        uint8_t out_su8[8]  = { 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U };
        static const uint8_t exp_su8[8] = { 0U, 0U, 0U, 42U, 127U, 200U, 255U, 255U };
        __asm__ volatile(
            "vld1.16 {d2, d3}, [%1]\n"
            "vqmovun.s16 d0, q1\n"
            "vst1.8 {d0}, [%0]\n"
            :
            : "r"(out_su8), "r"(q_su16)
            : "d0", "d2", "d3", "memory"
        );
        for (int i = 0; i < 8; i++) {
            if (out_su8[i] != exp_su8[i]) {
                return 31;
            }
        }
    }

    /* 19. Regression tests for VSLI.32 vs VSHL.I32, VSRI.32, VSRA.S32, VRSHR.U32, VSHRN.I16 */
    {
        uint32_t d0_init[2]  = { 0x11223344U, 0x55667788U };
        uint32_t d1_in[2]    = { 0xAABBCCDDU, 0xEEFF1234U };
        uint32_t vsli_out[2] = { 0U, 0U };
        uint32_t vshl_out[2] = { 0U, 0U };
        uint32_t vsri_out[2] = { 0U, 0U };
        __asm__ volatile(
            "vld1.32 {d0}, [%3]\n"
            "vld1.32 {d1}, [%4]\n"
            "vsli.32 d0, d1, #8\n"
            "vshl.i32 d2, d1, #8\n"
            "vst1.32 {d0}, [%0]\n"
            "vst1.32 {d2}, [%1]\n"
            "vld1.32 {d0}, [%3]\n"
            "vsri.32 d0, d1, #8\n"
            "vst1.32 {d0}, [%2]\n"
            :
            : "r"(vsli_out), "r"(vshl_out), "r"(vsri_out), "r"(d0_init), "r"(d1_in)
            : "d0", "d1", "d2", "memory"
        );
        if (vsli_out[0] != 0xBBCCDD44U || vsli_out[1] != 0xFF123488U ||
            vshl_out[0] != 0xBBCCDD00U || vshl_out[1] != 0xFF123400U ||
            vsri_out[0] != 0x11AABBCCU || vsri_out[1] != 0x55EEFF12U) {
            return 32;
        }

        int32_t sra_acc[2] = { 100, -50 };
        int32_t sra_in[2]  = { 64, -32 };
        int32_t sra_out[2] = { 0, 0 };
        __asm__ volatile(
            "vld1.32 {d0}, [%1]\n"
            "vld1.32 {d1}, [%2]\n"
            "vsra.s32 d0, d1, #4\n"
            "vst1.32 {d0}, [%0]\n"
            :
            : "r"(sra_out), "r"(sra_acc), "r"(sra_in)
            : "d0", "d1", "memory"
        );
        if (sra_out[0] != 104 || sra_out[1] != -52) {
            return 33;
        }

        uint32_t rshr_in[2]  = { 23U, 24U };
        uint32_t rshr_out[2] = { 0U, 0U };
        __asm__ volatile(
            "vld1.32 {d1}, [%1]\n"
            "vrshr.u32 d0, d1, #4\n"
            "vst1.32 {d0}, [%0]\n"
            :
            : "r"(rshr_out), "r"(rshr_in)
            : "d0", "d1", "memory"
        );
        if (rshr_out[0] != 1U || rshr_out[1] != 2U) {
            return 34;
        }

        uint16_t shrn_in[8] = {
            0x0120U, 0x0340U, 0x0560U, 0x0780U,
            0x09A0U, 0x0BC0U, 0x0DE0U, 0x0F10U
        };
        uint8_t shrn_out[8] = { 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U };
        static const uint8_t shrn_exp[8] = {
            0x12U, 0x34U, 0x56U, 0x78U,
            0x9AU, 0xBCU, 0xDEU, 0xF1U
        };
        __asm__ volatile(
            "vld1.16 {d2, d3}, [%1]\n"
            "vshrn.i16 d0, q1, #4\n"
            "vst1.8 {d0}, [%0]\n"
            :
            : "r"(shrn_out), "r"(shrn_in)
            : "d0", "d2", "d3", "memory"
        );
        for (int i = 0; i < 8; i++) {
            if (shrn_out[i] != shrn_exp[i]) {
                return 35;
            }
        }
    }

    /* 20. Single-lane and all-lane VLD2.8 / VST2.8 */
    {
        uint8_t  dup2_in[2] = { 0xAAU, 0x55U };
        uint64_t dup_d0 = 0ULL, dup_d1 = 0ULL;
        __asm__ volatile(
            "vld2.8 {d0[], d1[]}, [%2]\n"
            "vstr d0, [%0]\n"
            "vstr d1, [%1]\n"
            :
            : "r"(&dup_d0), "r"(&dup_d1), "r"(dup2_in)
            : "d0", "d1", "memory"
        );
        if (dup_d0 != 0xAAAAAAAAAAAAAAAAULL || dup_d1 != 0x5555555555555555ULL) {
            return 36;
        }

        uint64_t init_d0 = 0x0706050403020100ULL;
        uint64_t init_d1 = 0x1716151413121110ULL;
        uint8_t  lane3_in[2] = { 0xCCU, 0xDDU };
        uint64_t after_d0 = 0ULL, after_d1 = 0ULL;
        uint8_t  st_lane3[2] = { 0U, 0U };
        uint8_t  st_lane5[2] = { 0U, 0U };
        __asm__ volatile(
            "vldr d0, [%4]\n"
            "vldr d1, [%5]\n"
            "vld2.8 {d0[3], d1[3]}, [%6]\n"
            "vstr d0, [%0]\n"
            "vstr d1, [%1]\n"
            "vst2.8 {d0[3], d1[3]}, [%2]\n"
            "vst2.8 {d0[5], d1[5]}, [%3]\n"
            :
            : "r"(&after_d0), "r"(&after_d1),
              "r"(st_lane3), "r"(st_lane5),
              "r"(&init_d0), "r"(&init_d1), "r"(lane3_in)
            : "d0", "d1", "memory"
        );
        if (after_d0 != 0x07060504CC020100ULL ||
            after_d1 != 0x17161514DD121110ULL ||
            st_lane3[0] != 0xCCU || st_lane3[1] != 0xDDU ||
            st_lane5[0] != 0x05U || st_lane5[1] != 0x15U) {
            return 37;
        }
    }

    /*
     * 21. VQADD, VQSUB, VHADD, VABD, VABA, VPADD, VCLS, VCLZ, VCNT, VABS, VNEG, VSWP,
     *     and scalar VNMUL.F32 / VNMUL.F64
     */
    {
        /* 21a: VQADD.S16 & VQSUB.S16 */
        int16_t qa_in1[4] = { 30000, -30000, 100, -100 };
        int16_t qa_in2[4] = { 10000, -10000, 200, 50 };
        int16_t qa_out[4] = { 0, 0, 0, 0 };
        int16_t qs_out[4] = { 0, 0, 0, 0 };
        __asm__ volatile(
            "vld1.16 {d1}, [%2]\n"
            "vld1.16 {d2}, [%3]\n"
            "vqadd.s16 d0, d1, d2\n"
            "vqsub.s16 d3, d1, d2\n"
            "vst1.16 {d0}, [%0]\n"
            "vst1.16 {d3}, [%1]\n"
            :
            : "r"(qa_out), "r"(qs_out), "r"(qa_in1), "r"(qa_in2)
            : "d0", "d1", "d2", "d3", "memory"
        );
        if (qa_out[0] != 32767 || qa_out[1] != -32768 || qa_out[2] != 300 || qa_out[3] != -50 ||
            qs_out[0] != 20000 || qs_out[1] != -20000 || qs_out[2] != -100 || qs_out[3] != -150) {
            return 38;
        }

        /* 21b: VHADD.S32 */
        int32_t ha_in1[2] = { 0x7FFFFFF0, -100 };
        int32_t ha_in2[2] = { 0x00000010, -200 };
        int32_t ha_out[2] = { 0, 0 };
        __asm__ volatile(
            "vld1.32 {d1}, [%1]\n"
            "vld1.32 {d2}, [%2]\n"
            "vhadd.s32 d0, d1, d2\n"
            "vst1.32 {d0}, [%0]\n"
            :
            : "r"(ha_out), "r"(ha_in1), "r"(ha_in2)
            : "d0", "d1", "d2", "memory"
        );
        if (ha_out[0] != 0x40000000 || ha_out[1] != -150) {
            return 39;
        }

        /* 21c: VABD.S16 & VABA.S16 */
        int16_t abd_in1[4] = { -10, 50, 100, -5 };
        int16_t abd_in2[4] = { 20, -10, 100, -25 };
        int16_t aba_acc[4] = { 100, 200, 300, 400 };
        int16_t abd_out[4] = { 0, 0, 0, 0 };
        int16_t aba_out[4] = { 0, 0, 0, 0 };
        __asm__ volatile(
            "vld1.16 {d1}, [%2]\n"
            "vld1.16 {d2}, [%3]\n"
            "vld1.16 {d3}, [%4]\n"
            "vabd.s16 d0, d1, d2\n"
            "vaba.s16 d3, d1, d2\n"
            "vst1.16 {d0}, [%0]\n"
            "vst1.16 {d3}, [%1]\n"
            :
            : "r"(abd_out), "r"(aba_out), "r"(abd_in1), "r"(abd_in2), "r"(aba_acc)
            : "d0", "d1", "d2", "d3", "memory"
        );
        if (abd_out[0] != 30 || abd_out[1] != 60 || abd_out[2] != 0 || abd_out[3] != 20 ||
            aba_out[0] != 130 || aba_out[1] != 260 || aba_out[2] != 300 || aba_out[3] != 420) {
            return 40;
        }

        /* 21d: VPADD.I32 & VPADD.F32 */
        int32_t vp_i1[2] = { 10, 20 };
        int32_t vp_i2[2] = { 300, 400 };
        int32_t vp_io[2] = { 0, 0 };
        float   vp_f1[2] = { 1.5f, 2.5f };
        float   vp_f2[2] = { 10.0f, 20.0f };
        float   vp_fo[2] = { 0.0f, 0.0f };
        __asm__ volatile(
            "vld1.32 {d1}, [%2]\n"
            "vld1.32 {d2}, [%3]\n"
            "vpadd.i32 d0, d1, d2\n"
            "vst1.32 {d0}, [%0]\n"
            "vld1.32 {d1}, [%4]\n"
            "vld1.32 {d2}, [%5]\n"
            "vpadd.f32 d0, d1, d2\n"
            "vst1.32 {d0}, [%1]\n"
            :
            : "r"(vp_io), "r"(vp_fo), "r"(vp_i1), "r"(vp_i2), "r"(vp_f1), "r"(vp_f2)
            : "d0", "d1", "d2", "memory"
        );
        if (vp_io[0] != 30 || vp_io[1] != 700 || vp_fo[0] != 4.0f || vp_fo[1] != 30.0f) {
            return 41;
        }

        /* 21e: VCLS.S32, VCLZ.I32, VCNT.8 */
        uint32_t cls_in[2]  = { 0x1FFFFFFFU, 0xC0000000U };
        uint32_t cls_out[2] = { 0U, 0U };
        uint32_t clz_in[2]  = { 0x00080000U, 0x00000001U };
        uint32_t clz_out[2] = { 0U, 0U };
        uint8_t  cnt_in[8]  = { 0x00U, 0x01U, 0x03U, 0x0FU, 0x55U, 0xAAU, 0x7FU, 0xFFU };
        uint8_t  cnt_out[8] = { 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U };
        static const uint8_t cnt_exp[8] = { 0U, 1U, 2U, 4U, 4U, 4U, 7U, 8U };
        __asm__ volatile(
            "vld1.32 {d1}, [%3]\n"
            "vcls.s32 d0, d1\n"
            "vst1.32 {d0}, [%0]\n"
            "vld1.32 {d1}, [%4]\n"
            "vclz.i32 d0, d1\n"
            "vst1.32 {d0}, [%1]\n"
            "vld1.8 {d1}, [%5]\n"
            "vcnt.8 d0, d1\n"
            "vst1.8 {d0}, [%2]\n"
            :
            : "r"(cls_out), "r"(clz_out), "r"(cnt_out),
              "r"(cls_in), "r"(clz_in), "r"(cnt_in)
            : "d0", "d1", "memory"
        );
        if (cls_out[0] != 2U || cls_out[1] != 1U ||
            clz_out[0] != 12U || clz_out[1] != 31U) {
            return 42;
        }
        for (int i = 0; i < 8; i++) {
            if (cnt_out[i] != cnt_exp[i]) {
                return 43;
            }
        }

        /* 21f: VABS.S32, VNEG.S32, VSWP */
        int32_t an_in[2]   = { -42, 99 };
        int32_t abs_out[2] = { 0, 0 };
        int32_t neg_out[2] = { 0, 0 };
        __asm__ volatile(
            "vld1.32 {d2}, [%2]\n"
            "vabs.s32 d0, d2\n"
            "vneg.s32 d1, d2\n"
            "vswp d0, d1\n"
            "vst1.32 {d1}, [%0]\n"
            "vst1.32 {d0}, [%1]\n"
            :
            : "r"(abs_out), "r"(neg_out), "r"(an_in)
            : "d0", "d1", "d2", "memory"
        );
        if (abs_out[0] != 42 || abs_out[1] != 99 ||
            neg_out[0] != 42 || neg_out[1] != -99) {
            return 44;
        }

        /* 21g: Scalar VNMUL.F32 and VNMUL.F64 */
        float nm_f1 = 3.5f, nm_f2 = 2.0f, nm_fo = 0.0f;
        __asm__ volatile("vnmul.f32 %0, %1, %2" : "=t"(nm_fo) : "t"(nm_f1), "t"(nm_f2));
        if (nm_fo != -7.0f) {
            return 45;
        }
        double nm_d1 = -6.25, nm_d2 = 4.0, nm_do = 0.0;
        __asm__ volatile("vnmul.f64 %0, %1, %2" : "=w"(nm_do) : "w"(nm_d1), "w"(nm_d2));
        if (nm_do != 25.0) {
            return 46;
        }
    }

    /* ----------------------------------------------------------------------
     * 22. NEON Long / Narrow / Widening (VABAL, VADDHN/VSUBHN, VADDW/VSUBW)
     * ---------------------------------------------------------------------- */
    {
        /* 22a: VABAL.S16 and VABAL.U16 */
        int32_t abal_s_acc[4] = { 100, 200, 300, 400 };
        int16_t abal_s_a[4]   = { -10, 50, -300, 1000 };
        int16_t abal_s_b[4]   = { 20, -25, -100, 1000 };
        int32_t abal_s_out[4] = { 0, 0, 0, 0 };
        __asm__ volatile(
            "vld1.32 {d0-d1}, [%1]\n"
            "vld1.16 {d2}, [%2]\n"
            "vld1.16 {d3}, [%3]\n"
            "vabal.s16 q0, d2, d3\n"
            "vst1.32 {d0-d1}, [%0]\n"
            :
            : "r"(abal_s_out), "r"(abal_s_acc), "r"(abal_s_a), "r"(abal_s_b)
            : "q0", "d2", "d3", "memory"
        );
        if (abal_s_out[0] != 130 || abal_s_out[1] != 275 ||
            abal_s_out[2] != 500 || abal_s_out[3] != 400) {
            return 47;
        }

        uint32_t abal_u_acc[4] = { 1000U, 2000U, 3000U, 4000U };
        uint16_t abal_u_a[4]   = { 10U, 500U, 60000U, 42U };
        uint16_t abal_u_b[4]   = { 110U, 200U, 10000U, 42U };
        uint32_t abal_u_out[4] = { 0U, 0U, 0U, 0U };
        __asm__ volatile(
            "vld1.32 {d0-d1}, [%1]\n"
            "vld1.16 {d2}, [%2]\n"
            "vld1.16 {d3}, [%3]\n"
            "vabal.u16 q0, d2, d3\n"
            "vst1.32 {d0-d1}, [%0]\n"
            :
            : "r"(abal_u_out), "r"(abal_u_acc), "r"(abal_u_a), "r"(abal_u_b)
            : "q0", "d2", "d3", "memory"
        );
        if (abal_u_out[0] != 1100U || abal_u_out[1] != 2300U ||
            abal_u_out[2] != 53000U || abal_u_out[3] != 4000U) {
            return 48;
        }

        /* 22b: VADDHN.I16 and VSUBHN.I16 */
        uint16_t hn_a[8] = {
            0x0120U, 0x0380U, 0x10FFU, 0x7F01U,
            0x8000U, 0x00FFU, 0x1234U, 0xFF00U
        };
        uint16_t hn_b[8] = {
            0x0240U, 0x0080U, 0x0101U, 0x00FFU,
            0x8000U, 0x0001U, 0x4321U, 0x0100U
        };
        uint8_t addhn_out[8] = { 0 };
        uint8_t subhn_out[8] = { 0 };
        static const uint8_t addhn_exp[8] = {
            0x03U, 0x04U, 0x12U, 0x80U, 0x00U, 0x01U, 0x55U, 0x00U
        };
        static const uint8_t subhn_exp[8] = {
            0xFEU, 0x03U, 0x0FU, 0x7EU, 0x00U, 0x00U, 0xCFU, 0xFEU
        };
        __asm__ volatile(
            "vld1.16 {d2-d3}, [%2]\n"
            "vld1.16 {d4-d5}, [%3]\n"
            "vaddhn.i16 d0, q1, q2\n"
            "vsubhn.i16 d1, q1, q2\n"
            "vst1.8 {d0}, [%0]\n"
            "vst1.8 {d1}, [%1]\n"
            :
            : "r"(addhn_out), "r"(subhn_out), "r"(hn_a), "r"(hn_b)
            : "q0", "q1", "q2", "memory"
        );
        for (int i = 0; i < 8; i++) {
            if (addhn_out[i] != addhn_exp[i]) {
                return 49;
            }
            if (subhn_out[i] != subhn_exp[i]) {
                return 50;
            }
        }

        /* 22c: VADDW.S16 and VSUBW.S16 */
        int32_t w_q1[4]     = { 1000, -2000, 50000, -100000 };
        int16_t w_d4[4]     = { -300, 500, -10000, -25000 };
        int32_t addw_out[4] = { 0, 0, 0, 0 };
        int32_t subw_out[4] = { 0, 0, 0, 0 };
        __asm__ volatile(
            "vld1.32 {d2-d3}, [%2]\n"
            "vld1.16 {d4}, [%3]\n"
            "vaddw.s16 q0, q1, d4\n"
            "vst1.32 {d0-d1}, [%0]\n"
            "vsubw.s16 q0, q1, d4\n"
            "vst1.32 {d0-d1}, [%1]\n"
            :
            : "r"(addw_out), "r"(subw_out), "r"(w_q1), "r"(w_d4)
            : "q0", "q1", "d4", "memory"
        );
        if (addw_out[0] != 700 || addw_out[1] != -1500 ||
            addw_out[2] != 40000 || addw_out[3] != -125000) {
            return 51;
        }
        if (subw_out[0] != 1300 || subw_out[1] != -2500 ||
            subw_out[2] != 60000 || subw_out[3] != -75000) {
            return 52;
        }
    }

    /* ----------------------------------------------------------------------
     * 23. 128-bit VSHL.S64 and Narrowing Rounding Shift Right (VRSHRN.I16)
     * ---------------------------------------------------------------------- */
    {
        /* 23a: 128-bit VSHL.S64 q0, q1, q2 */
        int64_t shl64_in[2]  = { -1024LL, 0x0123456789ABCDEFLL };
        int64_t shl64_cnt[2] = { -3LL, 4LL };
        int64_t shl64_out[2] = { 0LL, 0LL };
        __asm__ volatile(
            "vld1.64 {d2-d3}, [%1]\n"
            "vld1.64 {d4-d5}, [%2]\n"
            "vshl.s64 q0, q1, q2\n"
            "vst1.64 {d0-d1}, [%0]\n"
            :
            : "r"(shl64_out), "r"(shl64_in), "r"(shl64_cnt)
            : "q0", "q1", "q2", "memory"
        );
        if (shl64_out[0] != -128LL || shl64_out[1] != 0x123456789ABCDEF0LL) {
            return 53;
        }

        /* 23b: VRSHRN.I16 d0, q1, #4 */
        uint16_t rshrn_in[8] = {
            0x0017U, 0x0018U, 0x0020U, 0x0029U,
            0x0107U, 0x0108U, 0x0FF0U, 0x0FF8U
        };
        uint8_t rshrn_out[8] = { 0 };
        static const uint8_t rshrn_exp[8] = {
            0x01U, 0x02U, 0x02U, 0x03U, 0x10U, 0x11U, 0xFFU, 0x00U
        };
        __asm__ volatile(
            "vld1.16 {d2-d3}, [%1]\n"
            "vrshrn.i16 d0, q1, #4\n"
            "vst1.8 {d0}, [%0]\n"
            :
            : "r"(rshrn_out), "r"(rshrn_in)
            : "d0", "q1", "memory"
        );
        for (int i = 0; i < 8; i++) {
            if (rshrn_out[i] != rshrn_exp[i]) {
                return 54;
            }
        }
    }

    /* ----------------------------------------------------------------------
     * 24. NEON 2-reg-scalar, FP16 VCVTB/VCVTT, fixed-point VCVT, and VCGT/VCEQ #0
     * ---------------------------------------------------------------------- */
    {
        /* 24a: VMUL.I16 q2, q1, d0[2], VMLA.F32 q2, q1, d0[1], VMULL.S16 q2, d2, d0[1] */
        int16_t sc_d0[4]   = { 2, 3, 7, 11 };
        int16_t sc_q1[8]   = { 1, -2, 3, -4, 5, 10, -20, 100 };
        int16_t sc_mout[8] = { 0 };
        static const int16_t sc_mexp[8] = { 7, -14, 21, -28, 35, 70, -140, 700 };
        __asm__ volatile(
            "vld1.16 {d0}, [%1]\n"
            "vld1.16 {d2-d3}, [%2]\n"
            "vmul.i16 q2, q1, d0[2]\n"
            "vst1.16 {d4-d5}, [%0]\n"
            :
            : "r"(sc_mout), "r"(sc_d0), "r"(sc_q1)
            : "d0", "q1", "q2", "memory"
        );
        for (int i = 0; i < 8; i++) {
            if (sc_mout[i] != sc_mexp[i]) {
                return 55;
            }
        }

        float fsc_d0[2]  = { 1.5f, 2.5f };
        float fsc_q1[4]  = { 2.0f, -4.0f, 6.0f, 8.0f };
        float fsc_acc[4] = { 10.0f, 10.0f, 10.0f, 10.0f };
        float fsc_out[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        __asm__ volatile(
            "vld1.32 {d0}, [%1]\n"
            "vld1.32 {d2-d3}, [%2]\n"
            "vld1.32 {d4-d5}, [%3]\n"
            "vmla.f32 q2, q1, d0[1]\n"
            "vst1.32 {d4-d5}, [%0]\n"
            :
            : "r"(fsc_out), "r"(fsc_d0), "r"(fsc_q1), "r"(fsc_acc)
            : "d0", "q1", "q2", "memory"
        );
        if (fsc_out[0] != 15.0f || fsc_out[1] != 0.0f ||
            fsc_out[2] != 25.0f || fsc_out[3] != 30.0f) {
            return 56;
        }

        int16_t mll_d0[4]  = { 5, -6, 7, 8 };
        int16_t mll_d2[4]  = { 10, -20, 300, -400 };
        int32_t mll_out[4] = { 0, 0, 0, 0 };
        __asm__ volatile(
            "vld1.16 {d0}, [%1]\n"
            "vld1.16 {d2}, [%2]\n"
            "vmull.s16 q2, d2, d0[1]\n"
            "vst1.32 {d4-d5}, [%0]\n"
            :
            : "r"(mll_out), "r"(mll_d0), "r"(mll_d2)
            : "d0", "d2", "q2", "memory"
        );
        if (mll_out[0] != -60 || mll_out[1] != 120 ||
            mll_out[2] != -1800 || mll_out[3] != 2400) {
            return 57;
        }

        /* 24b: Half-precision VCVTB.F16.F32, VCVTT.F16.F32, VCVTB.F32.F16, VCVTT.F32.F16 */
        float fp16_in_b = 1.5f;   /* IEEE 754 binary16: 0x3E00 */
        float fp16_in_t = -2.25f; /* IEEE 754 binary16: 0xC080 */
        uint32_t fp16_packed = 0U;
        float fp16_out_b = 0.0f;
        float fp16_out_t = 0.0f;
        __asm__ volatile(
            "vmov.i32 d0, #0\n"
            "vmov s1, %3\n"
            "vmov s2, %4\n"
            "vcvtb.f16.f32 s0, s1\n"
            "vcvtt.f16.f32 s0, s2\n"
            "vmov %0, s0\n"
            "vcvtb.f32.f16 s3, s0\n"
            "vcvtt.f32.f16 s4, s0\n"
            "vmov %1, s3\n"
            "vmov %2, s4\n"
            : "=r"(fp16_packed), "=r"(fp16_out_b), "=r"(fp16_out_t)
            : "r"(fp16_in_b), "r"(fp16_in_t)
            : "d0", "d1", "d2"
        );
        if (fp16_packed != 0xC0803E00U || fp16_out_b != 1.5f || fp16_out_t != -2.25f) {
            return 58;
        }

        /* 24c: Fixed-point scalar and NEON vector VCVT (#fbits = 8) */
        float fxp_in = 3.25f;
        int32_t fxp_s32 = 0;
        int32_t fxp_sin = -640; /* -640 / 256.0 = -2.5f */
        float fxp_f32 = 0.0f;
        __asm__ volatile(
            "vmov s0, %2\n"
            "vcvt.s32.f32 s0, s0, #8\n"
            "vmov %0, s0\n"
            "vmov s1, %3\n"
            "vcvt.f32.s32 s1, s1, #8\n"
            "vmov %1, s1\n"
            : "=r"(fxp_s32), "=r"(fxp_f32)
            : "r"(fxp_in), "r"(fxp_sin)
            : "d0"
        );
        if (fxp_s32 != 832 || fxp_f32 != -2.5f) {
            return 59;
        }

        float vfxp_in[2]    = { 1.5f, -2.75f };
        int32_t vfxp_mid[2] = { 0, 0 };
        float vfxp_out[2]   = { 0.0f, 0.0f };
        __asm__ volatile(
            "vld1.32 {d1}, [%2]\n"
            "vcvt.s32.f32 d0, d1, #8\n"
            "vst1.32 {d0}, [%0]\n"
            "vcvt.f32.s32 d2, d0, #8\n"
            "vst1.32 {d2}, [%1]\n"
            :
            : "r"(vfxp_mid), "r"(vfxp_out), "r"(vfxp_in)
            : "d0", "d1", "d2", "memory"
        );
        if (vfxp_mid[0] != 384 || vfxp_mid[1] != -704 ||
            vfxp_out[0] != 1.5f || vfxp_out[1] != -2.75f) {
            return 60;
        }

        /* 24d: NEON compare-with-zero (VCGT.S32, VCEQ.I32, VCLT.S32) */
        int32_t cz_in[2]     = { -5, 0 };
        int32_t cz_in2[2]    = { 7, 0 };
        uint32_t cgt_out[2]  = { 0U, 0U };
        uint32_t ceq_out[2]  = { 0U, 0U };
        uint32_t clt_out[2]  = { 0U, 0U };
        __asm__ volatile(
            "vld1.32 {d1}, [%3]\n"
            "vceq.i32 d0, d1, #0\n"
            "vst1.32 {d0}, [%1]\n"
            "vclt.s32 d0, d1, #0\n"
            "vst1.32 {d0}, [%2]\n"
            "vld1.32 {d1}, [%4]\n"
            "vcgt.s32 d0, d1, #0\n"
            "vst1.32 {d0}, [%0]\n"
            :
            : "r"(cgt_out), "r"(ceq_out), "r"(clt_out), "r"(cz_in), "r"(cz_in2)
            : "d0", "d1", "memory"
        );
        if (cgt_out[0] != 0xFFFFFFFFU || cgt_out[1] != 0x00000000U ||
            ceq_out[0] != 0x00000000U || ceq_out[1] != 0xFFFFFFFFU ||
            clt_out[0] != 0xFFFFFFFFU || clt_out[1] != 0x00000000U) {
            return 61;
        }
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
