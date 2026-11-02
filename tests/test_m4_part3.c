/*
 * tests/test_m4_part3.c — M4 VFPv4-D32 & NEON tests (part 3: sections 28..31)
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_m4_common.h"

int run_vfp_neon_part3(void)
{
    /*
     * 28. Round 6 regressions & positive tests:
     *     - 28a: VNMLA.F32 / VNMLA.F64 (and VNMLS.F32 / VNMLS.F64) signed-zero
     *            and negation-before-addition order:
     *            For d = +1.0, n = -1.0, m = +1.0:
     *            VNMLA computes (-d) + (-(n * m)) = (-1.0) + (+1.0) = +0.0
     *            (sign bit == 0), whereas -(d + n*m) = -(+0.0) = -0.0 would
     *            produce sign bit == 1!
     *     - 28b: NEON VMLA.F32 and VMLS.F32 (64-bit D and 128-bit Q vector forms,
     *            plus by-scalar d0, d1, d2[0] / d2[1] and q0, q2, d2[0]).
     */
    {
        /* 28a-1: VNMLA.F32 signed-zero check (d = +1.0f, n = -1.0f, m = +1.0f -> +0.0f) */
        float f_pos1 = 1.0f;
        float f_neg1 = -1.0f;
        uint32_t vnmla_f32_zero_bits = 0xFFFFFFFFU;
        uint32_t vnmls_f32_zero_bits = 0xFFFFFFFFU;
        __asm__ volatile(
            "vldr s0, %2\n"
            "vldr s1, %3\n"
            "vldr s2, %2\n"
            "vnmla.f32 s0, s1, s2\n"
            "vmov %0, s0\n"
            "vldr s0, %2\n"
            "vldr s1, %2\n"
            "vldr s2, %2\n"
            "vnmls.f32 s0, s1, s2\n"
            "vmov %1, s0\n"
            : "=&r"(vnmla_f32_zero_bits), "=r"(vnmls_f32_zero_bits)
            : "m"(f_pos1), "m"(f_neg1)
            : "s0", "s1", "s2"
        );
        if (vnmla_f32_zero_bits != 0x00000000U ||
            vnmls_f32_zero_bits != 0x00000000U) {
            return 77;
        }

        /* 28a-2: VNMLA.F64 signed-zero check (d = +1.0, n = -1.0, m = +1.0 -> +0.0) */
        double d_pos1 = 1.0;
        double d_neg1 = -1.0;
        uint64_t vnmla_f64_zero_bits = 0xFFFFFFFFFFFFFFFFULL;
        uint64_t vnmls_f64_zero_bits = 0xFFFFFFFFFFFFFFFFULL;
        __asm__ volatile(
            "vldr d0, %2\n"
            "vldr d1, %3\n"
            "vldr d2, %2\n"
            "vnmla.f64 d0, d1, d2\n"
            "vstr d0, %0\n"
            "vldr d0, %2\n"
            "vldr d1, %2\n"
            "vldr d2, %2\n"
            "vnmls.f64 d0, d1, d2\n"
            "vstr d0, %1\n"
            : "=m"(vnmla_f64_zero_bits), "=m"(vnmls_f64_zero_bits)
            : "m"(d_pos1), "m"(d_neg1)
            : "d0", "d1", "d2", "memory"
        );
        if (vnmla_f64_zero_bits != 0x0000000000000000ULL ||
            vnmls_f64_zero_bits != 0x0000000000000000ULL) {
            return 78;
        }

        /* 28a-3: Non-zero VNMLA / VNMLS values (d = 2.0, n = 3.0, m = 4.0) */
        float f_d = 2.0f, f_n = 3.0f, f_m = 4.0f;
        float vnmla_f32_val = 0.0f, vnmls_f32_val = 0.0f;
        double d_d = 2.0, d_n = 3.0, d_m = 4.0;
        double vnmla_f64_val = 0.0, vnmls_f64_val = 0.0;
        __asm__ volatile(
            "vldr s0, %2\n"
            "vldr s1, %3\n"
            "vldr s2, %4\n"
            "vnmla.f32 s0, s1, s2\n"
            "vstr s0, %0\n"
            "vldr s0, %2\n"
            "vnmls.f32 s0, s1, s2\n"
            "vstr s0, %1\n"
            : "=m"(vnmla_f32_val), "=m"(vnmls_f32_val)
            : "m"(f_d), "m"(f_n), "m"(f_m)
            : "s0", "s1", "s2", "memory"
        );
        __asm__ volatile(
            "vldr d0, %2\n"
            "vldr d1, %3\n"
            "vldr d2, %4\n"
            "vnmla.f64 d0, d1, d2\n"
            "vstr d0, %0\n"
            "vldr d0, %2\n"
            "vnmls.f64 d0, d1, d2\n"
            "vstr d0, %1\n"
            : "=m"(vnmla_f64_val), "=m"(vnmls_f64_val)
            : "m"(d_d), "m"(d_n), "m"(d_m)
            : "d0", "d1", "d2", "memory"
        );
        if (vnmla_f32_val != -14.0f || vnmls_f32_val != 10.0f ||
            vnmla_f64_val != -14.0  || vnmls_f64_val != 10.0) {
            return 79;
        }

        /* 28b-1: NEON 64-bit vector VMLA.F32 d0, d1, d2 and VMLS.F32 d0, d1, d2 */
        float d_acc_init[2] = { 10.0f, 20.0f };
        float d_src1[2]     = { 2.5f, -4.0f };
        float d_src2[2]     = { 4.0f,  3.0f };
        float d_vmla_out[2] = { 0.0f, 0.0f };
        float d_vmls_out[2] = { 0.0f, 0.0f };
        __asm__ volatile(
            "vld1.32 {d0}, [%2]\n"
            "vld1.32 {d1}, [%3]\n"
            "vld1.32 {d2}, [%4]\n"
            "vmla.f32 d0, d1, d2\n"
            "vst1.32 {d0}, [%0]\n"
            "vld1.32 {d0}, [%2]\n"
            "vmls.f32 d0, d1, d2\n"
            "vst1.32 {d0}, [%1]\n"
            :
            : "r"(d_vmla_out), "r"(d_vmls_out),
              "r"(d_acc_init), "r"(d_src1), "r"(d_src2)
            : "d0", "d1", "d2", "memory"
        );
        if (d_vmla_out[0] != 20.0f || d_vmla_out[1] != 8.0f ||
            d_vmls_out[0] != 0.0f  || d_vmls_out[1] != 32.0f) {
            return 80;
        }

        /* 28b-2: NEON 128-bit vector VMLA.F32 q0, q1, q2 and VMLS.F32 q0, q1, q2 */
        float q_acc_init[4] = { 10.0f, 20.0f, -5.0f, 100.0f };
        float q_src1[4]     = { 2.0f,  -3.0f,  4.0f, 5.0f };
        float q_src2[4]     = { 3.0f,   5.0f, -2.5f, 8.0f };
        float q_vmla_out[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        float q_vmls_out[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        __asm__ volatile(
            "vld1.32 {d0-d1}, [%2]\n"
            "vld1.32 {d2-d3}, [%3]\n"
            "vld1.32 {d4-d5}, [%4]\n"
            "vmla.f32 q0, q1, q2\n"
            "vst1.32 {d0-d1}, [%0]\n"
            "vld1.32 {d0-d1}, [%2]\n"
            "vmls.f32 q0, q1, q2\n"
            "vst1.32 {d0-d1}, [%1]\n"
            :
            : "r"(q_vmla_out), "r"(q_vmls_out),
              "r"(q_acc_init), "r"(q_src1), "r"(q_src2)
            : "q0", "q1", "q2", "memory"
        );
        if (q_vmla_out[0] != 16.0f || q_vmla_out[1] != 5.0f ||
            q_vmla_out[2] != -15.0f || q_vmla_out[3] != 140.0f ||
            q_vmls_out[0] != 4.0f  || q_vmls_out[1] != 35.0f ||
            q_vmls_out[2] != 5.0f  || q_vmls_out[3] != 60.0f) {
            return 81;
        }

        /*
         * 28b-3: NEON by-scalar VMLA.F32 d0, d1, d2[0], VMLS.F32 d0, d1, d2[0],
         *        VMLS.F32 d0, d1, d2[1], and VMLS.F32 q0, q2, d2[0]
         */
        float sc_d2[2]      = { 3.0f, -2.0f };
        float sc_d0_mla[2]  = { 0.0f, 0.0f };
        float sc_d0_mls0[2] = { 0.0f, 0.0f };
        float sc_d0_mls1[2] = { 0.0f, 0.0f };
        float sc_q0_mls0[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        __asm__ volatile(
            "vld1.32 {d0}, [%4]\n"
            "vld1.32 {d1}, [%5]\n"
            "vld1.32 {d2}, [%6]\n"
            "vmla.f32 d0, d1, d2[0]\n"
            "vst1.32 {d0}, [%0]\n"
            "vld1.32 {d0}, [%4]\n"
            "vmls.f32 d0, d1, d2[0]\n"
            "vst1.32 {d0}, [%1]\n"
            "vld1.32 {d0}, [%4]\n"
            "vmls.f32 d0, d1, d2[1]\n"
            "vst1.32 {d0}, [%2]\n"
            "vld1.32 {d0-d1}, [%7]\n"
            "vld1.32 {d2}, [%6]\n"
            "vld1.32 {d4-d5}, [%8]\n"
            "vmls.f32 q0, q2, d2[0]\n"
            "vst1.32 {d0-d1}, [%3]\n"
            :
            : "r"(sc_d0_mla), "r"(sc_d0_mls0), "r"(sc_d0_mls1), "r"(sc_q0_mls0),
              "r"(d_acc_init), "r"(d_src1), "r"(sc_d2),
              "r"(q_acc_init), "r"(q_src1)
            : "q0", "d2", "q2", "memory"
        );
        /*
         * d_acc_init = {10.0, 20.0}, d_src1 = {2.5, -4.0}, sc_d2 = {3.0, -2.0}:
         *   vmla d0, d1, d2[0]: {10 + 2.5*3, 20 + (-4)*3}   = {17.5, 8.0}
         *   vmls d0, d1, d2[0]: {10 - 2.5*3, 20 - (-4)*3}   = {2.5, 32.0}
         *   vmls d0, d1, d2[1]: {10 - 2.5*(-2), 20 - (-4)*(-2)} = {15.0, 12.0}
         *   vmls q0, q2, d2[0]: {10 - 2*3, 20 - (-3)*3, -5 - 4*3, 100 - 5*3} = {4.0, 29.0, -17.0, 85.0}
         */
        if (sc_d0_mla[0] != 17.5f  || sc_d0_mla[1] != 8.0f   ||
            sc_d0_mls0[0] != 2.5f  || sc_d0_mls0[1] != 32.0f ||
            sc_d0_mls1[0] != 15.0f || sc_d0_mls1[1] != 12.0f ||
            sc_q0_mls0[0] != 4.0f  || sc_q0_mls0[1] != 29.0f ||
            sc_q0_mls0[2] != -17.0f || sc_q0_mls0[3] != 85.0f) {
            return 82;
        }
    }

    /*
     * 29. Round 7 regressions for scalar VMLS.F32 / VMLS.F64 and
     *     VNMLS.F32 / VNMLS.F64:
     *     - 29a: VNMLS.F32 / VNMLS.F64 signed-zero (+0.0 when d=+1.0, n=+1.0, m=+1.0
     *            -> (-1.0) + (+1.0) = +0.0 with sign bit 0; and -0.0 when
     *            d=+0.0, n=-1.0, m=+0.0 -> (-0.0) + (-0.0) = -0.0 with sign bit 1)
     *            and VMLS.F32 / VMLS.F64 signed-zero (+0.0 when d=+1.0, n=+1.0, m=+1.0
     *            -> (+1.0) + (-1.0) = +0.0 with sign bit 0).
     *     - 29b: VMLS.F32 / VMLS.F64 and VNMLS.F32 / VNMLS.F64 with finite values
     *            and destination-source register aliasing (Sd == Sn, Sd == Sm,
     *            Dd == Dn, Dd == Dm).
     *     - 29c: QNaN propagation in VMLS.F32 / VMLS.F64 and VNMLS.F32 / VNMLS.F64
     *            (when accumulator d is QNaN or multiplier operand n/m is QNaN).
     */
    {
        /* 29a: Signed-zero checks for VMLS and VNMLS (F32 & F64) */
        float f_one = 1.0f;
        float f_neg_one = -1.0f;
        float f_pos_zero = 0.0f;
        uint32_t vmls_f32_pzero = 0xFFFFFFFFU;
        uint32_t vnmls_f32_pzero = 0xFFFFFFFFU;
        uint32_t vnmls_f32_nzero = 0U;
        __asm__ volatile(
            "vldr s1, %3\n"
            "vldr s3, %3\n"
            "vldr s5, %3\n"
            "vmls.f32 s1, s3, s5\n"
            "vmov %0, s1\n"
            "vldr s1, %3\n"
            "vnmls.f32 s1, s3, s5\n"
            "vmov %1, s1\n"
            "vldr s1, %5\n"
            "vldr s3, %4\n"
            "vldr s5, %5\n"
            "vnmls.f32 s1, s3, s5\n"
            "vmov %2, s1\n"
            : "=&r"(vmls_f32_pzero), "=&r"(vnmls_f32_pzero), "=r"(vnmls_f32_nzero)
            : "m"(f_one), "m"(f_neg_one), "m"(f_pos_zero)
            : "s1", "s3", "s5"
        );
        if (vmls_f32_pzero != 0x00000000U ||
            vnmls_f32_pzero != 0x00000000U ||
            vnmls_f32_nzero != 0x80000000U) {
            return 83;
        }

        double d_one = 1.0;
        double d_neg_one = -1.0;
        double d_pos_zero = 0.0;
        uint64_t vmls_f64_pzero = 0xFFFFFFFFFFFFFFFFULL;
        uint64_t vnmls_f64_pzero = 0xFFFFFFFFFFFFFFFFULL;
        uint64_t vnmls_f64_nzero = 0ULL;
        __asm__ volatile(
            "vldr d16, %3\n"
            "vldr d17, %3\n"
            "vldr d18, %3\n"
            "vmls.f64 d16, d17, d18\n"
            "vstr d16, %0\n"
            "vldr d16, %3\n"
            "vnmls.f64 d16, d17, d18\n"
            "vstr d16, %1\n"
            "vldr d16, %5\n"
            "vldr d17, %4\n"
            "vldr d18, %5\n"
            "vnmls.f64 d16, d17, d18\n"
            "vstr d16, %2\n"
            : "=m"(vmls_f64_pzero), "=m"(vnmls_f64_pzero), "=m"(vnmls_f64_nzero)
            : "m"(d_one), "m"(d_neg_one), "m"(d_pos_zero)
            : "d16", "d17", "d18", "memory"
        );
        if (vmls_f64_pzero != 0x0000000000000000ULL ||
            vnmls_f64_pzero != 0x0000000000000000ULL ||
            vnmls_f64_nzero != 0x8000000000000000ULL) {
            return 84;
        }

        /* 29b: Finite values and Sd == Sn / Sd == Sm / Dd == Dn / Dd == Dm aliasing */
        float f_20_5 = 20.5f, f_3_5 = 3.5f, f_4_0 = 4.0f;
        float vmls_f32_fin = 0.0f, vmls_f32_alias_n = 0.0f, vnmls_f32_alias_m = 0.0f;
        __asm__ volatile(
            "vldr s1, %3\n"
            "vldr s2, %4\n"
            "vldr s3, %5\n"
            "vmls.f32 s1, s2, s3\n"
            "vstr s1, %0\n"
            "vldr s2, %4\n"
            "vmls.f32 s2, s2, s3\n"
            "vstr s2, %1\n"
            "vldr s2, %4\n"
            "vldr s3, %5\n"
            "vnmls.f32 s3, s2, s3\n"
            "vstr s3, %2\n"
            : "=m"(vmls_f32_fin), "=m"(vmls_f32_alias_n), "=m"(vnmls_f32_alias_m)
            : "m"(f_20_5), "m"(f_3_5), "m"(f_4_0)
            : "s1", "s2", "s3", "memory"
        );
        /*
         * vmls_f32_fin:     20.5 - (3.5 * 4.0)  = 6.5
         * vmls_f32_alias_n: 3.5  - (3.5 * 4.0)  = -10.5
         * vnmls_f32_alias_m: -4.0 + (3.5 * 4.0) = 10.0
         */
        if (vmls_f32_fin != 6.5f ||
            vmls_f32_alias_n != -10.5f ||
            vnmls_f32_alias_m != 10.0f) {
            return 85;
        }

        double d_20_5 = 20.5, d_3_5 = 3.5, d_4_0 = 4.0;
        double vmls_f64_fin = 0.0, vmls_f64_alias_n = 0.0, vnmls_f64_alias_m = 0.0;
        __asm__ volatile(
            "vldr d1, %3\n"
            "vldr d2, %4\n"
            "vldr d3, %5\n"
            "vmls.f64 d1, d2, d3\n"
            "vstr d1, %0\n"
            "vldr d2, %4\n"
            "vmls.f64 d2, d2, d3\n"
            "vstr d2, %1\n"
            "vldr d2, %4\n"
            "vldr d3, %5\n"
            "vnmls.f64 d3, d2, d3\n"
            "vstr d3, %2\n"
            : "=m"(vmls_f64_fin), "=m"(vmls_f64_alias_n), "=m"(vnmls_f64_alias_m)
            : "m"(d_20_5), "m"(d_3_5), "m"(d_4_0)
            : "d1", "d2", "d3", "memory"
        );
        if (vmls_f64_fin != 6.5 ||
            vmls_f64_alias_n != -10.5 ||
            vnmls_f64_alias_m != 10.0) {
            return 86;
        }

        /* 29c: QNaN propagation in VMLS.F32 / VMLS.F64 and VNMLS.F32 / VNMLS.F64 */
        uint32_t qnan32_in = 0x7FC01234U;
        uint32_t vmls32_nan_d = 0U, vmls32_nan_n = 0U, vnmls32_nan_d = 0U;
        __asm__ volatile(
            "vmov s0, %3\n"
            "vldr s1, %4\n"
            "vldr s2, %5\n"
            "vmls.f32 s0, s1, s2\n"
            "vmov %0, s0\n"
            "vldr s0, %4\n"
            "vmov s1, %3\n"
            "vmls.f32 s0, s1, s2\n"
            "vmov %1, s0\n"
            "vmov s0, %3\n"
            "vldr s1, %4\n"
            "vnmls.f32 s0, s1, s2\n"
            "vmov %2, s0\n"
            : "=&r"(vmls32_nan_d), "=&r"(vmls32_nan_n), "=r"(vnmls32_nan_d)
            : "r"(qnan32_in), "m"(f_3_5), "m"(f_4_0)
            : "s0", "s1", "s2"
        );
        if ((vmls32_nan_d & 0x7FC00000U) != 0x7FC00000U ||
            (vmls32_nan_n & 0x7FC00000U) != 0x7FC00000U ||
            (vnmls32_nan_d & 0x7FC00000U) != 0x7FC00000U) {
            return 87;
        }

        uint64_t qnan64_in = 0x7FF8000012345678ULL;
        uint64_t vmls64_nan_d = 0ULL, vmls64_nan_n = 0ULL, vnmls64_nan_d = 0ULL;
        __asm__ volatile(
            "vldr d0, %3\n"
            "vldr d1, %4\n"
            "vldr d2, %5\n"
            "vmls.f64 d0, d1, d2\n"
            "vstr d0, %0\n"
            "vldr d0, %4\n"
            "vldr d1, %3\n"
            "vmls.f64 d0, d1, d2\n"
            "vstr d0, %1\n"
            "vldr d0, %3\n"
            "vldr d1, %4\n"
            "vnmls.f64 d0, d1, d2\n"
            "vstr d0, %2\n"
            : "=m"(vmls64_nan_d), "=m"(vmls64_nan_n), "=m"(vnmls64_nan_d)
            : "m"(qnan64_in), "m"(d_3_5), "m"(d_4_0)
            : "d0", "d1", "d2", "memory"
        );
        if ((vmls64_nan_d & 0x7FF8000000000000ULL) != 0x7FF8000000000000ULL ||
            (vmls64_nan_n & 0x7FF8000000000000ULL) != 0x7FF8000000000000ULL ||
            (vnmls64_nan_d & 0x7FF8000000000000ULL) != 0x7FF8000000000000ULL) {
            return 88;
        }
    }

    /*
     * 30. Round 8 regressions for NEON VMLS.F32 (64-bit D vector, 128-bit Q
     *     vector, and by-scalar d0, d1, d2[0] / q0, q1, d4[1]):
     *     - 30a: Signed-zero (+1.0 - (+1.0 * +1.0) = +0.0 with sign bit 0, and
     *            -0.0 - (+1.0 * +0.0) = -0.0 with sign bit 1)
     *     - 30b: Finite values and register aliasing (Dd == Dn, Dd == Dm,
     *            Qd == Qn, Qd == Qm, and by-scalar Dd == Dn == Dm)
     *     - 30c: Per-lane QNaN propagation alongside finite lanes
     */
    {
        /* 30a: Signed-zero checks across vector and by-scalar NEON VMLS.F32 */
        static const uint32_t d_sz_acc[2] = { 0x3F800000U /* +1.0f */, 0x80000000U /* -0.0f */ };
        static const uint32_t d_sz_n[2]   = { 0x3F800000U /* +1.0f */, 0x3F800000U /* +1.0f */ };
        static const uint32_t d_sz_m[2]   = { 0x3F800000U /* +1.0f */, 0x00000000U /* +0.0f */ };
        static uint32_t d_sz_out[2];
        static uint32_t d_sc_out[2];

        static const uint32_t q_sz_acc[4] = {
            0x3F800000U /* +1.0f */, 0x40200000U /* +2.5f */,
            0x80000000U /* -0.0f */, 0xC0600000U /* -3.5f */
        };
        static const uint32_t q_sz_n[4] = {
            0x3F800000U /* +1.0f */, 0x40200000U /* +2.5f */,
            0x3F800000U /* +1.0f */, 0xC0600000U /* -3.5f */
        };
        static const uint32_t q_sz_m[4] = {
            0x3F800000U /* +1.0f */, 0x3F800000U /* +1.0f */,
            0x00000000U /* +0.0f */, 0x3F800000U /* +1.0f */
        };
        static uint32_t q_sz_out[4];

        static const uint32_t q_sc_acc[4] = {
            0x40000000U /* +2.0f */, 0xC0800000U /* -4.0f */,
            0x40C00000U /* +6.0f */, 0x80000000U /* -0.0f */
        };
        static const uint32_t q_sc_n[4] = {
            0x3F800000U /* +1.0f */, 0xC0000000U /* -2.0f */,
            0x40400000U /* +3.0f */, 0x00000000U /* +0.0f */
        };
        static const uint32_t d4_sc[2] = {
            0x40A00000U /* +5.0f */, 0x40000000U /* +2.0f */
        };
        static uint32_t q_sc_out[4];

        __asm__ volatile(
            "vld1.32 {d0}, [%2]\n"
            "vld1.32 {d1}, [%3]\n"
            "vld1.32 {d2}, [%4]\n"
            "vmls.f32 d0, d1, d2\n"
            "vst1.32 {d0}, [%0]\n"
            "vld1.32 {d0}, [%3]\n"
            "vmls.f32 d0, d1, d2[0]\n"
            "vst1.32 {d0}, [%1]\n"
            :
            : "r"(d_sz_out), "r"(d_sc_out),
              "r"(d_sz_acc), "r"(d_sz_n), "r"(d_sz_m)
            : "d0", "d1", "d2", "memory"
        );
        if (d_sz_out[0] != 0x00000000U || d_sz_out[1] != 0x80000000U ||
            d_sc_out[0] != 0x00000000U || d_sc_out[1] != 0x00000000U) {
            return 89;
        }

        __asm__ volatile(
            "vld1.32 {d0-d1}, [%2]\n"
            "vld1.32 {d2-d3}, [%3]\n"
            "vld1.32 {d4-d5}, [%4]\n"
            "vmls.f32 q0, q1, q2\n"
            "vst1.32 {d0-d1}, [%0]\n"
            "vld1.32 {d0-d1}, [%5]\n"
            "vld1.32 {d2-d3}, [%6]\n"
            "vld1.32 {d4}, [%7]\n"
            "vmls.f32 q0, q1, d4[1]\n"
            "vst1.32 {d0-d1}, [%1]\n"
            :
            : "r"(q_sz_out), "r"(q_sc_out),
              "r"(q_sz_acc), "r"(q_sz_n), "r"(q_sz_m),
              "r"(q_sc_acc), "r"(q_sc_n), "r"(d4_sc)
            : "q0", "q1", "q2", "memory"
        );
        if (q_sz_out[0] != 0x00000000U || q_sz_out[1] != 0x00000000U ||
            q_sz_out[2] != 0x80000000U || q_sz_out[3] != 0x00000000U ||
            q_sc_out[0] != 0x00000000U || q_sc_out[1] != 0x00000000U ||
            q_sc_out[2] != 0x00000000U || q_sc_out[3] != 0x80000000U) {
            return 90;
        }

        /* 30b: Finite values, by-scalar d4[1], and destination/source aliasing */
        static const float d_al_init0[2] = { 12.0f, -6.0f };
        static const float d_al_init1[2] = { 3.0f,  4.0f };
        static const float d_al_init2[2] = { 5.0f,  -2.0f };
        static float d_al_n_out[2];
        static float d_al_m_out[2];
        static float d_al_sc_out[2];

        __asm__ volatile(
            "vld1.32 {d0}, [%3]\n"
            "vld1.32 {d1}, [%4]\n"
            "vmls.f32 d0, d0, d1\n"
            "vst1.32 {d0}, [%0]\n"
            "vld1.32 {d0}, [%5]\n"
            "vld1.32 {d1}, [%4]\n"
            "vmls.f32 d1, d0, d1\n"
            "vst1.32 {d1}, [%1]\n"
            "vld1.32 {d2}, [%4]\n"
            "vmls.f32 d2, d2, d2[0]\n"
            "vst1.32 {d2}, [%2]\n"
            :
            : "r"(d_al_n_out), "r"(d_al_m_out), "r"(d_al_sc_out),
              "r"(d_al_init0), "r"(d_al_init1), "r"(d_al_init2)
            : "d0", "d1", "d2", "memory"
        );
        if (d_al_n_out[0] != -24.0f || d_al_n_out[1] != 18.0f ||
            d_al_m_out[0] != -12.0f || d_al_m_out[1] != 12.0f ||
            d_al_sc_out[0] != -6.0f || d_al_sc_out[1] != -8.0f) {
            return 91;
        }

        static const float q_fin_acc[4] = { 20.0f, 10.0f, -5.0f, 100.0f };
        static const float q_fin_n[4]   = { 1.5f,  -2.0f,  3.0f,  12.5f };
        static const float d4_fin_sc[2] = { 7.0f,   4.0f };
        static float q_fin_sc_out[4];
        static float q_al_n_out[4];
        static float q_al_m_out[4];
        __asm__ volatile(
            "vld1.32 {d0-d1}, [%3]\n"
            "vld1.32 {d2-d3}, [%4]\n"
            "vld1.32 {d4}, [%5]\n"
            "vmls.f32 q0, q1, d4[1]\n"
            "vst1.32 {d0-d1}, [%0]\n"
            "vld1.32 {d0-d1}, [%3]\n"
            "vld1.32 {d2-d3}, [%4]\n"
            "vmls.f32 q0, q0, q1\n"
            "vst1.32 {d0-d1}, [%1]\n"
            "vld1.32 {d0-d1}, [%3]\n"
            "vld1.32 {d2-d3}, [%4]\n"
            "vmls.f32 q1, q0, q1\n"
            "vst1.32 {d2-d3}, [%2]\n"
            :
            : "r"(q_fin_sc_out), "r"(q_al_n_out), "r"(q_al_m_out),
              "r"(q_fin_acc), "r"(q_fin_n), "r"(d4_fin_sc)
            : "q0", "q1", "d4", "memory"
        );
        if (q_fin_sc_out[0] != 14.0f  || q_fin_sc_out[1] != 18.0f   ||
            q_fin_sc_out[2] != -17.0f || q_fin_sc_out[3] != 50.0f   ||
            q_al_n_out[0] != -10.0f   || q_al_n_out[1] != 30.0f     ||
            q_al_n_out[2] != 10.0f    || q_al_n_out[3] != -1150.0f  ||
            q_al_m_out[0] != -28.5f   || q_al_m_out[1] != 18.0f     ||
            q_al_m_out[2] != 18.0f    || q_al_m_out[3] != -1237.5f) {
            return 92;
        }

        /* 30c: Per-lane QNaN propagation in vector and by-scalar NEON VMLS.F32 */
        static const uint32_t q_nan_acc[4] = {
            0x7FC01234U /* QNaN */,  0x41200000U /* 10.0f */,
            0x41A00000U /* 20.0f */, 0x42C80000U /* 100.0f */
        };
        static const uint32_t q_nan_n[4] = {
            0x40000000U /* 2.0f */,  0x7FC05678U /* QNaN */,
            0x40800000U /* 4.0f */,  0x40A00000U /* 5.0f */
        };
        static const uint32_t q_nan_m[4] = {
            0x40400000U /* 3.0f */,  0x40A00000U /* 5.0f */,
            0x7FC09ABCU /* QNaN */,  0x41000000U /* 8.0f */
        };
        static const uint32_t d4_nan_sc[2] = {
            0x40400000U /* 3.0f */,  0x7FC0DEF0U /* QNaN */
        };
        static uint32_t d_nan_out[2];
        static uint32_t q_nan_out[4];
        static uint32_t q_sc0_nan_out[4];
        static uint32_t q_sc1_nan_out[4];

        __asm__ volatile(
            "vld1.32 {d0}, [%4]\n"
            "vld1.32 {d1}, [%5]\n"
            "vld1.32 {d2}, [%6]\n"
            "vmls.f32 d0, d1, d2\n"
            "vst1.32 {d0}, [%0]\n"
            "vld1.32 {d0-d1}, [%4]\n"
            "vld1.32 {d2-d3}, [%5]\n"
            "vld1.32 {d4-d5}, [%6]\n"
            "vmls.f32 q0, q1, q2\n"
            "vst1.32 {d0-d1}, [%1]\n"
            "vld1.32 {d0-d1}, [%4]\n"
            "vld1.32 {d2-d3}, [%5]\n"
            "vld1.32 {d4}, [%7]\n"
            "vmls.f32 q0, q1, d4[0]\n"
            "vst1.32 {d0-d1}, [%2]\n"
            "vld1.32 {d0-d1}, [%4]\n"
            "vld1.32 {d2-d3}, [%5]\n"
            "vld1.32 {d4}, [%7]\n"
            "vmls.f32 q0, q1, d4[1]\n"
            "vst1.32 {d0-d1}, [%3]\n"
            :
            : "r"(d_nan_out), "r"(q_nan_out),
              "r"(q_sc0_nan_out), "r"(q_sc1_nan_out),
              "r"(q_nan_acc), "r"(q_nan_n), "r"(q_nan_m), "r"(d4_nan_sc)
            : "q0", "q1", "q2", "memory"
        );
        if ((d_nan_out[0] & 0x7FC00000U) != 0x7FC00000U ||
            (d_nan_out[1] & 0x7FC00000U) != 0x7FC00000U) {
            return 93;
        }
        /*
         * In q_nan_out: lanes 0..2 have QNaN in acc/n/m; lane 3 is 100.0 - 5.0*8.0 = 60.0f (0x42700000)
         * In q_sc0_nan_out (d4[0] = 3.0f): lanes 0..1 are QNaN; lane 2 = 20 - 4*3 = 8.0f (0x41000000);
         *   lane 3 = 100 - 5*3 = 85.0f (0x42AA0000)
         * In q_sc1_nan_out (d4[1] = QNaN): all 4 lanes are QNaN
         */
        if ((q_nan_out[0] & 0x7FC00000U) != 0x7FC00000U ||
            (q_nan_out[1] & 0x7FC00000U) != 0x7FC00000U ||
            (q_nan_out[2] & 0x7FC00000U) != 0x7FC00000U ||
            q_nan_out[3] != 0x42700000U /* 60.0f */ ||
            (q_sc0_nan_out[0] & 0x7FC00000U) != 0x7FC00000U ||
            (q_sc0_nan_out[1] & 0x7FC00000U) != 0x7FC00000U ||
            q_sc0_nan_out[2] != 0x41000000U /* 8.0f */ ||
            q_sc0_nan_out[3] != 0x42AA0000U /* 85.0f */ ||
            (q_sc1_nan_out[0] & 0x7FC00000U) != 0x7FC00000U ||
            (q_sc1_nan_out[1] & 0x7FC00000U) != 0x7FC00000U ||
            (q_sc1_nan_out[2] & 0x7FC00000U) != 0x7FC00000U ||
            (q_sc1_nan_out[3] & 0x7FC00000U) != 0x7FC00000U) {
            return 94;
        }
    }

    /*
     * 31. ARMv8-A AArch32 CRC32 (CRC32B/H/W, CRC32CB/H/W) and Crypto
     *     (AESE.8, AESD.8, AESMC.8, AESIMC.8, VMULL.P64, SHA1*, SHA256*)
     */
    {
#ifdef __thumb__
#define INSN_CRC32B_R0_R1_R2       ".inst.w 0xfac1f082\n"
#define INSN_CRC32H_R0_R1_R2       ".inst.w 0xfac1f092\n"
#define INSN_CRC32W_R0_R1_R2       ".inst.w 0xfac1f0a2\n"
#define INSN_CRC32CB_R0_R1_R2      ".inst.w 0xfad1f082\n"
#define INSN_CRC32CH_R0_R1_R2      ".inst.w 0xfad1f092\n"
#define INSN_CRC32CW_R0_R1_R2      ".inst.w 0xfad1f0a2\n"
#define INSN_AESE_Q0_Q1            ".inst.w 0xffb00302\n"
#define INSN_AESD_Q0_Q1            ".inst.w 0xffb00342\n"
#define INSN_AESMC_Q0_Q1           ".inst.w 0xffb00382\n"
#define INSN_AESIMC_Q0_Q1          ".inst.w 0xffb003c2\n"
#define INSN_SHA1H_Q0_Q1           ".inst.w 0xffb902c2\n"
#define INSN_SHA1SU1_Q0_Q1         ".inst.w 0xffba0382\n"
#define INSN_SHA256SU0_Q0_Q1       ".inst.w 0xffba03c2\n"
#define INSN_SHA1C_Q0_Q1_Q2        ".inst.w 0xef020c44\n"
#define INSN_SHA1P_Q0_Q1_Q2        ".inst.w 0xef120c44\n"
#define INSN_SHA1M_Q0_Q1_Q2        ".inst.w 0xef220c44\n"
#define INSN_SHA1SU0_Q0_Q1_Q2      ".inst.w 0xef320c44\n"
#define INSN_SHA256H_Q0_Q1_Q2      ".inst.w 0xff020c44\n"
#define INSN_SHA256H2_Q0_Q1_Q2     ".inst.w 0xff120c44\n"
#define INSN_SHA256SU1_Q0_Q1_Q2    ".inst.w 0xff220c44\n"
#define INSN_VMULL_P64_Q0_D2_D4    ".inst.w 0xefa20e04\n"
#else
#define INSN_CRC32B_R0_R1_R2       ".inst 0xe1010042\n"
#define INSN_CRC32H_R0_R1_R2       ".inst 0xe1210042\n"
#define INSN_CRC32W_R0_R1_R2       ".inst 0xe1410042\n"
#define INSN_CRC32CB_R0_R1_R2      ".inst 0xe1010242\n"
#define INSN_CRC32CH_R0_R1_R2      ".inst 0xe1210242\n"
#define INSN_CRC32CW_R0_R1_R2      ".inst 0xe1410242\n"
#define INSN_AESE_Q0_Q1            ".inst 0xf3b00302\n"
#define INSN_AESD_Q0_Q1            ".inst 0xf3b00342\n"
#define INSN_AESMC_Q0_Q1           ".inst 0xf3b00382\n"
#define INSN_AESIMC_Q0_Q1          ".inst 0xf3b003c2\n"
#define INSN_SHA1H_Q0_Q1           ".inst 0xf3b902c2\n"
#define INSN_SHA1SU1_Q0_Q1         ".inst 0xf3ba0382\n"
#define INSN_SHA256SU0_Q0_Q1       ".inst 0xf3ba03c2\n"
#define INSN_SHA1C_Q0_Q1_Q2        ".inst 0xf2020c44\n"
#define INSN_SHA1P_Q0_Q1_Q2        ".inst 0xf2120c44\n"
#define INSN_SHA1M_Q0_Q1_Q2        ".inst 0xf2220c44\n"
#define INSN_SHA1SU0_Q0_Q1_Q2      ".inst 0xf2320c44\n"
#define INSN_SHA256H_Q0_Q1_Q2      ".inst 0xf3020c44\n"
#define INSN_SHA256H2_Q0_Q1_Q2     ".inst 0xf3120c44\n"
#define INSN_SHA256SU1_Q0_Q1_Q2    ".inst 0xf3220c44\n"
#define INSN_VMULL_P64_Q0_D2_D4    ".inst 0xf2a20e04\n"
#endif

        /* 31a: IEEE 802.3 CRC-32 and Castagnoli CRC-32C over "123456789" */
        uint32_t crc_out = 0U, crcc_out = 0U;
        __asm__ volatile(
            "mov r1, #-1\n"
            "movw r2, #0x3231\n"
            "movt r2, #0x3433\n"
            INSN_CRC32W_R0_R1_R2
            "mov r1, r0\n"
            "movw r2, #0x3635\n"
            INSN_CRC32H_R0_R1_R2
            "mov r1, r0\n"
            "mov r2, #0x37\n"
            INSN_CRC32B_R0_R1_R2
            "mov r1, r0\n"
            "movw r2, #0x3938\n"
            INSN_CRC32H_R0_R1_R2
            "mvn %0, r0\n"
            "mov r1, #-1\n"
            "movw r2, #0x3231\n"
            "movt r2, #0x3433\n"
            INSN_CRC32CW_R0_R1_R2
            "mov r1, r0\n"
            "movw r2, #0x3635\n"
            INSN_CRC32CH_R0_R1_R2
            "mov r1, r0\n"
            "mov r2, #0x37\n"
            INSN_CRC32CB_R0_R1_R2
            "mov r1, r0\n"
            "movw r2, #0x3938\n"
            INSN_CRC32CH_R0_R1_R2
            "mvn %1, r0\n"
            : "=&r"(crc_out), "=r"(crcc_out)
            :
            : "r0", "r1", "r2"
        );
        if (crc_out != 0xCBF43926U || crcc_out != 0xE3069283U) {
            return 95;
        }

        /* 31b: AES roundtrip: AESD(AESE(state, key), zero) ^ key == state & AESIMC(AESMC(state)) == state */
        static const uint32_t aes_state[4] = {
            0x3243F6A8U, 0x885A308DU, 0x313198A2U, 0xE0370734U
        };
        static const uint32_t aes_key[4] = {
            0x2B7E1516U, 0x28AED2A6U, 0xABF71588U, 0x09CF4F3CU
        };
        static const uint32_t aes_zero[4] = { 0U, 0U, 0U, 0U };
        static uint32_t aes_enc[4];
        static uint32_t aes_rt1[4];
        static uint32_t aes_rt2[4];
        __asm__ volatile(
            /* q0 = AESE(state, key); store encrypted state, then AESD(q0, zero) ^ key -> aes_rt1 */
            "vld1.32 {d0-d1}, [%3]\n"
            "vld1.32 {d2-d3}, [%4]\n"
            INSN_AESE_Q0_Q1
            "vst1.32 {d0-d1}, [%0]\n"
            "vld1.32 {d2-d3}, [%5]\n"
            INSN_AESD_Q0_Q1
            "vld1.32 {d2-d3}, [%4]\n"
            "veor q0, q0, q1\n"
            "vst1.32 {d0-d1}, [%1]\n"
            /* AESIMC(AESMC(state)) -> aes_rt2 */
            "vld1.32 {d2-d3}, [%3]\n"
            INSN_AESMC_Q0_Q1
            "vmov q1, q0\n"
            INSN_AESIMC_Q0_Q1
            "vst1.32 {d0-d1}, [%2]\n"
            :
            : "r"(aes_enc), "r"(aes_rt1), "r"(aes_rt2),
              "r"(aes_state), "r"(aes_key), "r"(aes_zero)
            : "q0", "q1", "memory"
        );
        if (aes_enc[0] == aes_state[0] ||
            aes_rt1[0] != aes_state[0] || aes_rt1[1] != aes_state[1] ||
            aes_rt1[2] != aes_state[2] || aes_rt1[3] != aes_state[3] ||
            aes_rt2[0] != aes_state[0] || aes_rt2[1] != aes_state[1] ||
            aes_rt2[2] != aes_state[2] || aes_rt2[3] != aes_state[3]) {
            return 96;
        }

        /* 31c: VMULL.P64 64x64->128 polynomial multiplication */
        static const uint64_t pmull_a = 0x0123456789ABCDEFULL;
        static const uint64_t pmull_b = 0xFEDCBA9876543210ULL;
        static uint64_t pmull_out[2];
        __asm__ volatile(
            "vldr d2, [%1]\n"
            "vldr d4, [%2]\n"
            INSN_VMULL_P64_Q0_D2_D4
            "vst1.64 {d0-d1}, [%0]\n"
            :
            : "r"(pmull_out), "r"(&pmull_a), "r"(&pmull_b)
            : "q0", "d2", "d4", "memory"
        );
        if (pmull_out[0] != 0x40A0789828C810F0ULL ||
            pmull_out[1] != 0x00E038D8688850B0ULL) {
            return 97;
        }

        /* 31d: SHA-1 instructions (SHA1H.32, SHA1C.32, SHA1P.32, SHA1M.32, SHA1SU0.32, SHA1SU1.32) */
        static const uint32_t sha_in0[4] = {
            0x67452301U, 0xEFCDAB89U, 0x98BADCFEU, 0x10325476U
        };
        static const uint32_t sha_in1[4] = {
            0xC3D2E1F0U, 0x11223344U, 0x55667788U, 0x99AABBCCU
        };
        static const uint32_t sha_in2[4] = {
            0x5A827999U, 0x6ED9EBA1U, 0x8F1BBCDCU, 0xCA62C1D6U
        };
        static uint32_t sha1h_out[4];
        static uint32_t sha1c_out[4];
        static uint32_t sha1p_out[4];
        static uint32_t sha1m_out[4];
        static uint32_t sha1su0_out[4];
        static uint32_t sha1su1_out[4];

        __asm__ volatile(
            "vld1.32 {d0-d1}, [%6]\n"
            "vld1.32 {d2-d3}, [%7]\n"
            INSN_SHA1H_Q0_Q1
            "vst1.32 {d0-d1}, [%0]\n"

            "vld1.32 {d0-d1}, [%6]\n"
            "vld1.32 {d2-d3}, [%7]\n"
            "vld1.32 {d4-d5}, [%8]\n"
            INSN_SHA1C_Q0_Q1_Q2
            "vst1.32 {d0-d1}, [%1]\n"

            "vld1.32 {d0-d1}, [%6]\n"
            INSN_SHA1P_Q0_Q1_Q2
            "vst1.32 {d0-d1}, [%2]\n"

            "vld1.32 {d0-d1}, [%6]\n"
            INSN_SHA1M_Q0_Q1_Q2
            "vst1.32 {d0-d1}, [%3]\n"

            "vld1.32 {d0-d1}, [%6]\n"
            INSN_SHA1SU0_Q0_Q1_Q2
            "vst1.32 {d0-d1}, [%4]\n"

            "vld1.32 {d0-d1}, [%6]\n"
            INSN_SHA1SU1_Q0_Q1
            "vst1.32 {d0-d1}, [%5]\n"
            :
            : "r"(sha1h_out), "r"(sha1c_out), "r"(sha1p_out),
              "r"(sha1m_out), "r"(sha1su0_out), "r"(sha1su1_out),
              "r"(sha_in0), "r"(sha_in1), "r"(sha_in2)
            : "q0", "q1", "q2", "memory"
        );
        if (sha1h_out[0] != 0x30F4B87CU || sha1h_out[1] != 0U ||
            sha1h_out[2] != 0U || sha1h_out[3] != 0U ||
            sha1c_out[0] != 0x52B5EE1AU || sha1c_out[1] != 0xD514B5A8U ||
            sha1c_out[2] != 0x1C66D562U || sha1c_out[3] != 0xE7ED262CU ||
            sha1p_out[0] != 0xA023551EU || sha1p_out[1] != 0xD28916A7U ||
            sha1p_out[2] != 0x72BD2B00U || sha1p_out[3] != 0x9B8FB7ADU ||
            sha1m_out[0] != 0xFC29787AU || sha1m_out[1] != 0xB620105AU ||
            sha1m_out[2] != 0x3C64B05BU || sha1m_out[3] != 0xE7ED262CU ||
            sha1su0_out[0] != 0xA57D8666U || sha1su0_out[1] != 0x9126145EU ||
            sha1su0_out[2] != 0xD47381D2U || sha1su0_out[3] != 0xCB72A6E4U ||
            sha1su1_out[0] != 0xECCE208AU || sha1su1_out[1] != 0x7557B803U ||
            sha1su1_out[2] != 0x0220CE64U || sha1su1_out[3] != 0xF9F8E9F9U) {
            return 98;
        }

        /* 31e: SHA-256 instructions (SHA256H.32, SHA256H2.32, SHA256SU0.32, SHA256SU1.32) */
        static uint32_t sha256h_out[4];
        static uint32_t sha256h2_out[4];
        static uint32_t sha256su0_out[4];
        static uint32_t sha256su1_out[4];

        __asm__ volatile(
            "vld1.32 {d0-d1}, [%4]\n"
            "vld1.32 {d2-d3}, [%5]\n"
            "vld1.32 {d4-d5}, [%6]\n"
            INSN_SHA256H_Q0_Q1_Q2
            "vst1.32 {d0-d1}, [%0]\n"

            "vld1.32 {d0-d1}, [%4]\n"
            INSN_SHA256H2_Q0_Q1_Q2
            "vst1.32 {d0-d1}, [%1]\n"

            "vld1.32 {d0-d1}, [%4]\n"
            INSN_SHA256SU0_Q0_Q1
            "vst1.32 {d0-d1}, [%2]\n"

            "vld1.32 {d0-d1}, [%4]\n"
            INSN_SHA256SU1_Q0_Q1_Q2
            "vst1.32 {d0-d1}, [%3]\n"
            :
            : "r"(sha256h_out), "r"(sha256h2_out),
              "r"(sha256su0_out), "r"(sha256su1_out),
              "r"(sha_in0), "r"(sha_in1), "r"(sha_in2)
            : "q0", "q1", "q2", "memory"
        );
        if (sha256h_out[0] != 0x56E908D8U || sha256h_out[1] != 0x9DCC7EDDU ||
            sha256h_out[2] != 0xC698F62EU || sha256h_out[3] != 0x62DF5964U ||
            sha256h2_out[0] != 0xCA8E8AD1U || sha256h2_out[1] != 0x050B3C00U ||
            sha256h2_out[2] != 0x9DDD36ACU || sha256h2_out[3] != 0xCB800675U ||
            sha256su0_out[0] != 0xCC0978D6U || sha256su0_out[1] != 0x48E73391U ||
            sha256su0_out[2] != 0x13F68728U || sha256su0_out[3] != 0x51B41D7FU ||
            sha256su1_out[0] != 0x223D66C6U || sha256su1_out[1] != 0x7E1747DEU ||
            sha256su1_out[2] != 0x5218F2CAU || sha256su1_out[3] != 0xB5C04327U) {
            return 99;
        }
    }

    return 0;
}
