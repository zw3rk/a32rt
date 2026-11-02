/*
 * tests/test_m4_part4.c — M4 ARMv8 VFP and M6 NEON cross-width aliasing tests
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_m4_common.h"

#if defined(__thumb__) || defined(__thumb2__)
#define A32_V8_INST(w) ".inst.w " #w "\n"
#define INSN_VMAXNM_Q0_Q1_Q2 ".inst.w 0xff020f54\n"
#define INSN_VMINNM_Q0_Q1_Q2 ".inst.w 0xff220f54\n"
#else
#define A32_V8_INST(w) ".inst " #w "\n"
#define INSN_VMAXNM_Q0_Q1_Q2 ".inst 0xf3020f54\n"
#define INSN_VMINNM_Q0_Q1_Q2 ".inst 0xf3220f54\n"
#endif

int run_section32_armv8_vfp_tests(void)
{
    /* 32a: VSEL{EQ,VS,GE,GT}.F32 and .F64 */
    {
        float s2 = 10.5f, s4 = 20.5f, out_s = 0.0f;
        double d2 = 100.5, d4 = 200.5, out_d = 0.0;
        int32_t val0 = 0, val1 = 1, val3 = 3, val5 = 5;
        int32_t max_int = 0x7fffffff, zero_int = 0;

        /* VSELEQ.F32: condition true */
        __asm__ volatile(
            "vldr s2, %1\n"
            "vldr s4, %2\n"
            "cmp %3, %3\n"
            A32_V8_INST(0xfe010a02) /* vseleq.f32 s0, s2, s4 */
            "vstr s0, %0\n"
            : "=m"(out_s) : "m"(s2), "m"(s4), "r"(val0) : "s0", "s2", "s4", "cc", "memory"
        );
        if (out_s != 10.5f) {
            return 101;
        }

        /* VSELEQ.F32: condition false */
        __asm__ volatile(
            "vldr s2, %1\n"
            "vldr s4, %2\n"
            "cmp %3, %4\n"
            A32_V8_INST(0xfe010a02) /* vseleq.f32 s0, s2, s4 */
            "vstr s0, %0\n"
            : "=m"(out_s) : "m"(s2), "m"(s4), "r"(val0), "r"(val1) : "s0", "s2", "s4", "cc", "memory"
        );
        if (out_s != 20.5f) {
            return 102;
        }

        /* VSELGT.F32: condition true */
        __asm__ volatile(
            "vldr s2, %1\n"
            "vldr s4, %2\n"
            "cmp %3, %4\n"
            A32_V8_INST(0xfe310a02) /* vselgt.f32 s0, s2, s4 */
            "vstr s0, %0\n"
            : "=m"(out_s) : "m"(s2), "m"(s4), "r"(val5), "r"(val3) : "s0", "s2", "s4", "cc", "memory"
        );
        if (out_s != 10.5f) {
            return 103;
        }

        /* VSELGT.F32: condition false */
        __asm__ volatile(
            "vldr s2, %1\n"
            "vldr s4, %2\n"
            "cmp %3, %4\n"
            A32_V8_INST(0xfe310a02) /* vselgt.f32 s0, s2, s4 */
            "vstr s0, %0\n"
            : "=m"(out_s) : "m"(s2), "m"(s4), "r"(val3), "r"(val5) : "s0", "s2", "s4", "cc", "memory"
        );
        if (out_s != 20.5f) {
            return 104;
        }

        /* VSELGE.F64: condition true */
        __asm__ volatile(
            "vldr d2, %1\n"
            "vldr d4, %2\n"
            "cmp %3, %3\n"
            A32_V8_INST(0xfe220b04) /* vselge.f64 d0, d2, d4 */
            "vstr d0, %0\n"
            : "=m"(out_d) : "m"(d2), "m"(d4), "r"(val5) : "d0", "d2", "d4", "cc", "memory"
        );
        if (out_d != 100.5) {
            return 105;
        }

        /* VSELGE.F64: condition false */
        __asm__ volatile(
            "vldr d2, %1\n"
            "vldr d4, %2\n"
            "cmp %3, %4\n"
            A32_V8_INST(0xfe220b04) /* vselge.f64 d0, d2, d4 */
            "vstr d0, %0\n"
            : "=m"(out_d) : "m"(d2), "m"(d4), "r"(val3), "r"(val5) : "d0", "d2", "d4", "cc", "memory"
        );
        if (out_d != 200.5) {
            return 106;
        }

        /* VSELVS.F64: condition true (overflow) */
        int32_t tmp_ovf = max_int;
        __asm__ volatile(
            "vldr d2, %2\n"
            "vldr d4, %3\n"
            "adds %1, %1, #1\n"
            A32_V8_INST(0xfe120b04) /* vselvs.f64 d0, d2, d4 */
            "vstr d0, %0\n"
            : "=m"(out_d), "+r"(tmp_ovf) : "m"(d2), "m"(d4) : "d0", "d2", "d4", "cc", "memory"
        );
        if (out_d != 100.5) {
            return 107;
        }

        /* VSELVS.F64: condition false (no overflow) */
        int32_t tmp_no_ovf = zero_int;
        __asm__ volatile(
            "vldr d2, %2\n"
            "vldr d4, %3\n"
            "adds %1, %1, #0\n"
            A32_V8_INST(0xfe120b04) /* vselvs.f64 d0, d2, d4 */
            "vstr d0, %0\n"
            : "=m"(out_d), "+r"(tmp_no_ovf) : "m"(d2), "m"(d4) : "d0", "d2", "d4", "cc", "memory"
        );
        if (out_d != 200.5) {
            return 108;
        }
    }

    /* 32b: VMAXNM / VMINNM (.F32 and .F64, regular numbers and NaN vs 42.5) */
    {
        float s2 = 10.0f, s4 = 20.0f, out_s = 0.0f;
        float nan_f = __builtin_nanf("");
        float val_f = 42.5f;

        /* VMAXNM.F32 regular */
        __asm__ volatile(
            "vldr s2, %1\n"
            "vldr s4, %2\n"
            A32_V8_INST(0xfe810a02) /* vmaxnm.f32 s0, s2, s4 */
            "vstr s0, %0\n"
            : "=m"(out_s) : "m"(s2), "m"(s4) : "s0", "s2", "s4", "memory"
        );
        if (out_s != 20.0f) {
            return 110;
        }

        /* VMAXNM.F32 NaN vs 42.5f -> 42.5f */
        __asm__ volatile(
            "vldr s2, %1\n"
            "vldr s4, %2\n"
            A32_V8_INST(0xfe810a02) /* vmaxnm.f32 s0, s2, s4 */
            "vstr s0, %0\n"
            : "=m"(out_s) : "m"(nan_f), "m"(val_f) : "s0", "s2", "s4", "memory"
        );
        if (out_s != 42.5f) {
            return 111;
        }

        /* VMAXNM.F32 42.5f vs NaN -> 42.5f */
        __asm__ volatile(
            "vldr s2, %1\n"
            "vldr s4, %2\n"
            A32_V8_INST(0xfe810a02) /* vmaxnm.f32 s0, s2, s4 */
            "vstr s0, %0\n"
            : "=m"(out_s) : "m"(val_f), "m"(nan_f) : "s0", "s2", "s4", "memory"
        );
        if (out_s != 42.5f) {
            return 112;
        }

        /* VMINNM.F32 regular */
        __asm__ volatile(
            "vldr s2, %1\n"
            "vldr s4, %2\n"
            A32_V8_INST(0xfe810a42) /* vminnm.f32 s0, s2, s4 */
            "vstr s0, %0\n"
            : "=m"(out_s) : "m"(s2), "m"(s4) : "s0", "s2", "s4", "memory"
        );
        if (out_s != 10.0f) {
            return 113;
        }

        /* VMINNM.F32 NaN vs 42.5f -> 42.5f */
        __asm__ volatile(
            "vldr s2, %1\n"
            "vldr s4, %2\n"
            A32_V8_INST(0xfe810a42) /* vminnm.f32 s0, s2, s4 */
            "vstr s0, %0\n"
            : "=m"(out_s) : "m"(nan_f), "m"(val_f) : "s0", "s2", "s4", "memory"
        );
        if (out_s != 42.5f) {
            return 114;
        }

        double d2 = 10.0, d4 = 20.0, out_d = 0.0;
        double nan_d = __builtin_nan("");
        double val_d = 42.5;

        /* VMAXNM.F64 regular */
        __asm__ volatile(
            "vldr d2, %1\n"
            "vldr d4, %2\n"
            A32_V8_INST(0xfe820b04) /* vmaxnm.f64 d0, d2, d4 */
            "vstr d0, %0\n"
            : "=m"(out_d) : "m"(d2), "m"(d4) : "d0", "d2", "d4", "memory"
        );
        if (out_d != 20.0) {
            return 115;
        }

        /* VMAXNM.F64 NaN vs 42.5 -> 42.5 */
        __asm__ volatile(
            "vldr d2, %1\n"
            "vldr d4, %2\n"
            A32_V8_INST(0xfe820b04) /* vmaxnm.f64 d0, d2, d4 */
            "vstr d0, %0\n"
            : "=m"(out_d) : "m"(nan_d), "m"(val_d) : "d0", "d2", "d4", "memory"
        );
        if (out_d != 42.5) {
            return 116;
        }

        /* VMINNM.F64 regular */
        __asm__ volatile(
            "vldr d2, %1\n"
            "vldr d4, %2\n"
            A32_V8_INST(0xfe820b44) /* vminnm.f64 d0, d2, d4 */
            "vstr d0, %0\n"
            : "=m"(out_d) : "m"(d2), "m"(d4) : "d0", "d2", "d4", "memory"
        );
        if (out_d != 10.0) {
            return 117;
        }

        /* VMINNM.F64 42.5 vs NaN -> 42.5 */
        __asm__ volatile(
            "vldr d2, %1\n"
            "vldr d4, %2\n"
            A32_V8_INST(0xfe820b44) /* vminnm.f64 d0, d2, d4 */
            "vstr d0, %0\n"
            : "=m"(out_d) : "m"(val_d), "m"(nan_d) : "d0", "d2", "d4", "memory"
        );
        if (out_d != 42.5) {
            return 118;
        }
    }

    /* 32c: VRINT{A,N,P,M,Z,R,X}.F32 and .F64 on 2.5, -2.5, 2.25, -2.25 */
    {
        static const float in_f[4] = { 2.5f, -2.5f, 2.25f, -2.25f };
        static const double in_d[4] = { 2.5, -2.5, 2.25, -2.25 };

        static const float exp_a[4] = { 3.0f, -3.0f, 2.0f, -2.0f };
        static const float exp_n[4] = { 2.0f, -2.0f, 2.0f, -2.0f };
        static const float exp_p[4] = { 3.0f, -2.0f, 3.0f, -2.0f };
        static const float exp_m[4] = { 2.0f, -3.0f, 2.0f, -3.0f };
        static const float exp_z[4] = { 2.0f, -2.0f, 2.0f, -2.0f };

        for (int i = 0; i < 4; i++) {
            float out_f = 0.0f;
            double out_d = 0.0;

            /* VRINTA.F32 */
            __asm__ volatile(
                "vldr s2, %1\n"
                A32_V8_INST(0xfeb80a41) /* vrinta.f32 s0, s2 */
                "vstr s0, %0\n"
                : "=m"(out_f) : "m"(in_f[i]) : "s0", "s2", "memory"
            );
            if (out_f != exp_a[i]) {
                return 120 + i;
            }

            /* VRINTA.F64 */
            __asm__ volatile(
                "vldr d2, %1\n"
                A32_V8_INST(0xfeb80b42) /* vrinta.f64 d0, d2 */
                "vstr d0, %0\n"
                : "=m"(out_d) : "m"(in_d[i]) : "d0", "d2", "memory"
            );
            if (out_d != (double)exp_a[i]) {
                return 125 + i;
            }

            /* VRINTN.F32 */
            __asm__ volatile(
                "vldr s2, %1\n"
                A32_V8_INST(0xfeb90a41) /* vrintn.f32 s0, s2 */
                "vstr s0, %0\n"
                : "=m"(out_f) : "m"(in_f[i]) : "s0", "s2", "memory"
            );
            if (out_f != exp_n[i]) {
                return 130 + i;
            }

            /* VRINTN.F64 */
            __asm__ volatile(
                "vldr d2, %1\n"
                A32_V8_INST(0xfeb90b42) /* vrintn.f64 d0, d2 */
                "vstr d0, %0\n"
                : "=m"(out_d) : "m"(in_d[i]) : "d0", "d2", "memory"
            );
            if (out_d != (double)exp_n[i]) {
                return 135 + i;
            }

            /* VRINTP.F32 */
            __asm__ volatile(
                "vldr s2, %1\n"
                A32_V8_INST(0xfeba0a41) /* vrintp.f32 s0, s2 */
                "vstr s0, %0\n"
                : "=m"(out_f) : "m"(in_f[i]) : "s0", "s2", "memory"
            );
            if (out_f != exp_p[i]) {
                return 140 + i;
            }

            /* VRINTP.F64 */
            __asm__ volatile(
                "vldr d2, %1\n"
                A32_V8_INST(0xfeba0b42) /* vrintp.f64 d0, d2 */
                "vstr d0, %0\n"
                : "=m"(out_d) : "m"(in_d[i]) : "d0", "d2", "memory"
            );
            if (out_d != (double)exp_p[i]) {
                return 145 + i;
            }

            /* VRINTM.F32 */
            __asm__ volatile(
                "vldr s2, %1\n"
                A32_V8_INST(0xfebb0a41) /* vrintm.f32 s0, s2 */
                "vstr s0, %0\n"
                : "=m"(out_f) : "m"(in_f[i]) : "s0", "s2", "memory"
            );
            if (out_f != exp_m[i]) {
                return 150 + i;
            }

            /* VRINTM.F64 */
            __asm__ volatile(
                "vldr d2, %1\n"
                A32_V8_INST(0xfebb0b42) /* vrintm.f64 d0, d2 */
                "vstr d0, %0\n"
                : "=m"(out_d) : "m"(in_d[i]) : "d0", "d2", "memory"
            );
            if (out_d != (double)exp_m[i]) {
                return 155 + i;
            }

            /* VRINTZ.F32 */
            __asm__ volatile(
                "vldr s2, %1\n"
                A32_V8_INST(0xeeb60ac1) /* vrintz.f32 s0, s2 */
                "vstr s0, %0\n"
                : "=m"(out_f) : "m"(in_f[i]) : "s0", "s2", "memory"
            );
            if (out_f != exp_z[i]) {
                return 160 + i;
            }

            /* VRINTZ.F64 */
            __asm__ volatile(
                "vldr d2, %1\n"
                A32_V8_INST(0xeeb60bc2) /* vrintz.f64 d0, d2 */
                "vstr d0, %0\n"
                : "=m"(out_d) : "m"(in_d[i]) : "d0", "d2", "memory"
            );
            if (out_d != (double)exp_z[i]) {
                return 165 + i;
            }

            /* VRINTR.F32 */
            __asm__ volatile(
                "vldr s2, %1\n"
                A32_V8_INST(0xeeb60a41) /* vrintr.f32 s0, s2 */
                "vstr s0, %0\n"
                : "=m"(out_f) : "m"(in_f[i]) : "s0", "s2", "memory"
            );
            if (out_f != exp_n[i]) {
                return 170 + i;
            }

            /* VRINTR.F64 */
            __asm__ volatile(
                "vldr d2, %1\n"
                A32_V8_INST(0xeeb60b42) /* vrintr.f64 d0, d2 */
                "vstr d0, %0\n"
                : "=m"(out_d) : "m"(in_d[i]) : "d0", "d2", "memory"
            );
            if (out_d != (double)exp_n[i]) {
                return 175 + i;
            }

            /* VRINTX.F32 */
            __asm__ volatile(
                "vldr s2, %1\n"
                A32_V8_INST(0xeeb70a41) /* vrintx.f32 s0, s2 */
                "vstr s0, %0\n"
                : "=m"(out_f) : "m"(in_f[i]) : "s0", "s2", "memory"
            );
            if (out_f != exp_n[i]) {
                return 180 + i;
            }

            /* VRINTX.F64 */
            __asm__ volatile(
                "vldr d2, %1\n"
                A32_V8_INST(0xeeb70b42) /* vrintx.f64 d0, d2 */
                "vstr d0, %0\n"
                : "=m"(out_d) : "m"(in_d[i]) : "d0", "d2", "memory"
            );
            if (out_d != (double)exp_n[i]) {
                return 185 + i;
            }
        }
    }

    /* 32d: VCVT{A,N,P,M} integer conversions */
    {
        float in_pos_f = 2.5f, in_neg_f = -2.5f;
        double in_pos_d = 2.5, in_neg_d = -2.5;
        int32_t out_s = 0;
        uint32_t out_u = 0;

        /* VCVTA.S32.F32 */
        __asm__ volatile(
            "vldr s2, %1\n"
            A32_V8_INST(0xfebc0ac1) /* vcvta.s32.f32 s0, s2 */
            "vstr s0, %0\n"
            : "=m"(out_s) : "m"(in_pos_f) : "s0", "s2", "memory"
        );
        if (out_s != 3) {
            return 190;
        }
        __asm__ volatile(
            "vldr s2, %1\n"
            A32_V8_INST(0xfebc0ac1) /* vcvta.s32.f32 s0, s2 */
            "vstr s0, %0\n"
            : "=m"(out_s) : "m"(in_neg_f) : "s0", "s2", "memory"
        );
        if (out_s != -3) {
            return 191;
        }

        /* VCVTN.S32.F32 */
        __asm__ volatile(
            "vldr s2, %1\n"
            A32_V8_INST(0xfebd0ac1) /* vcvtn.s32.f32 s0, s2 */
            "vstr s0, %0\n"
            : "=m"(out_s) : "m"(in_pos_f) : "s0", "s2", "memory"
        );
        if (out_s != 2) {
            return 192;
        }
        __asm__ volatile(
            "vldr s2, %1\n"
            A32_V8_INST(0xfebd0ac1) /* vcvtn.s32.f32 s0, s2 */
            "vstr s0, %0\n"
            : "=m"(out_s) : "m"(in_neg_f) : "s0", "s2", "memory"
        );
        if (out_s != -2) {
            return 193;
        }

        /* VCVTP.S32.F32 */
        __asm__ volatile(
            "vldr s2, %1\n"
            A32_V8_INST(0xfebe0ac1) /* vcvtp.s32.f32 s0, s2 */
            "vstr s0, %0\n"
            : "=m"(out_s) : "m"(in_pos_f) : "s0", "s2", "memory"
        );
        if (out_s != 3) {
            return 194;
        }
        __asm__ volatile(
            "vldr s2, %1\n"
            A32_V8_INST(0xfebe0ac1) /* vcvtp.s32.f32 s0, s2 */
            "vstr s0, %0\n"
            : "=m"(out_s) : "m"(in_neg_f) : "s0", "s2", "memory"
        );
        if (out_s != -2) {
            return 195;
        }

        /* VCVTM.S32.F32 */
        __asm__ volatile(
            "vldr s2, %1\n"
            A32_V8_INST(0xfebf0ac1) /* vcvtm.s32.f32 s0, s2 */
            "vstr s0, %0\n"
            : "=m"(out_s) : "m"(in_pos_f) : "s0", "s2", "memory"
        );
        if (out_s != 2) {
            return 196;
        }
        __asm__ volatile(
            "vldr s2, %1\n"
            A32_V8_INST(0xfebf0ac1) /* vcvtm.s32.f32 s0, s2 */
            "vstr s0, %0\n"
            : "=m"(out_s) : "m"(in_neg_f) : "s0", "s2", "memory"
        );
        if (out_s != -3) {
            return 197;
        }

        /* VCVTA.U32.F64 */
        __asm__ volatile(
            "vldr d2, %1\n"
            A32_V8_INST(0xfebc0b42) /* vcvta.u32.f64 s0, d2 */
            "vstr s0, %0\n"
            : "=m"(out_u) : "m"(in_pos_d) : "s0", "d2", "memory"
        );
        if (out_u != 3U) {
            return 198;
        }

        /* VCVTN.S32.F64 */
        __asm__ volatile(
            "vldr d2, %1\n"
            A32_V8_INST(0xfebd0bc2) /* vcvtn.s32.f64 s0, d2 */
            "vstr s0, %0\n"
            : "=m"(out_s) : "m"(in_pos_d) : "s0", "d2", "memory"
        );
        if (out_s != 2) {
            return 199;
        }
        __asm__ volatile(
            "vldr d2, %1\n"
            A32_V8_INST(0xfebd0bc2) /* vcvtn.s32.f64 s0, d2 */
            "vstr s0, %0\n"
            : "=m"(out_s) : "m"(in_neg_d) : "s0", "d2", "memory"
        );
        if (out_s != -2) {
            return 200;
        }

        /* VCVTP.S32.F64 */
        __asm__ volatile(
            "vldr d2, %1\n"
            A32_V8_INST(0xfebe0bc2) /* vcvtp.s32.f64 s0, d2 */
            "vstr s0, %0\n"
            : "=m"(out_s) : "m"(in_pos_d) : "s0", "d2", "memory"
        );
        if (out_s != 3) {
            return 201;
        }
        __asm__ volatile(
            "vldr d2, %1\n"
            A32_V8_INST(0xfebe0bc2) /* vcvtp.s32.f64 s0, d2 */
            "vstr s0, %0\n"
            : "=m"(out_s) : "m"(in_neg_d) : "s0", "d2", "memory"
        );
        if (out_s != -2) {
            return 202;
        }

        /* VCVTM.S32.F64 */
        __asm__ volatile(
            "vldr d2, %1\n"
            A32_V8_INST(0xfebf0bc2) /* vcvtm.s32.f64 s0, d2 */
            "vstr s0, %0\n"
            : "=m"(out_s) : "m"(in_pos_d) : "s0", "d2", "memory"
        );
        if (out_s != 2) {
            return 203;
        }
        __asm__ volatile(
            "vldr d2, %1\n"
            A32_V8_INST(0xfebf0bc2) /* vcvtm.s32.f64 s0, d2 */
            "vstr s0, %0\n"
            : "=m"(out_s) : "m"(in_neg_d) : "s0", "d2", "memory"
        );
        if (out_s != -3) {
            return 204;
        }
    }

    /* 32e: NEON Vector VMAXNM.F32 / VMINNM.F32 */
    {
        float nan_f = __builtin_nanf("");
        float v1[4] = { 10.0f, nan_f,  30.0f, -5.0f };
        float v2[4] = { 20.0f, 42.5f, -10.0f, -50.0f };
        float v_max_out[4] = { 0 };
        float v_min_out[4] = { 0 };

        __asm__ volatile(
            "vld1.32 {d2-d3}, [%1]\n"
            "vld1.32 {d4-d5}, [%2]\n"
            INSN_VMAXNM_Q0_Q1_Q2
            "vst1.32 {d0-d1}, [%0]\n"
            :
            : "r"(v_max_out), "r"(v1), "r"(v2)
            : "q0", "q1", "q2", "memory"
        );
        if (v_max_out[0] != 20.0f || v_max_out[1] != 42.5f ||
            v_max_out[2] != 30.0f || v_max_out[3] != -5.0f) {
            return 210;
        }

        __asm__ volatile(
            "vld1.32 {d2-d3}, [%1]\n"
            "vld1.32 {d4-d5}, [%2]\n"
            INSN_VMINNM_Q0_Q1_Q2
            "vst1.32 {d0-d1}, [%0]\n"
            :
            : "r"(v_min_out), "r"(v1), "r"(v2)
            : "q0", "q1", "q2", "memory"
        );
        if (v_min_out[0] != 10.0f || v_min_out[1] != 42.5f ||
            v_min_out[2] != -10.0f || v_min_out[3] != -50.0f) {
            return 211;
        }
    }

    static const char pass_msg[] = "M4 ARMv8-A Directed-Rounding VFP: PASS\n";
    sys3(4U /* write */, 1U, (uint32_t)(uintptr_t)pass_msg, (uint32_t)(sizeof(pass_msg) - 1U));
    return 0;
}

struct m6_neon_aliasing_data {
    int32_t init_q0[4];
    int32_t init_q1[4];
    float init_q4[4];
    float init_q5[4];
    int32_t out_q2[4];
    int32_t out_q3[4];
    float out_q4[4];
    uint32_t out_s[4];
    uint32_t out_d0[2];
    uint32_t out_d1[2];
    uint32_t new_s1;
    int32_t final_q0[4];
};

int __attribute__((noinline)) run_m6_neon_aliasing_tests(void)
{
    /* 128-Bit NEON q0–q15 Intra-Block Register Caching & Cross-Width (Q <-> D <-> S) Aliasing */
    struct m6_neon_aliasing_data data;
    data.init_q0[0] = 10; data.init_q0[1] = 20; data.init_q0[2] = 30; data.init_q0[3] = 40;
    data.init_q1[0] = 1;  data.init_q1[1] = 2;  data.init_q1[2] = 3;  data.init_q1[3] = 4;
    data.init_q4[0] = 2.0f; data.init_q4[1] = 3.0f; data.init_q4[2] = 4.0f; data.init_q4[3] = 5.0f;
    data.init_q5[0] = 1.5f; data.init_q5[1] = 2.5f; data.init_q5[2] = 0.5f; data.init_q5[3] = 10.0f;
    data.new_s1 = 9999;

    __asm__ volatile(
        "mov r1, %0\n"
        "vld1.32 {d0-d1}, [r1]!\n"    /* init_q0 */
        "vld1.32 {d2-d3}, [r1]!\n"    /* init_q1 */
        "vld1.32 {d8-d9}, [r1]!\n"    /* init_q4 */
        "vld1.32 {d10-d11}, [r1]!\n"  /* init_q5 */
        /* Tight back-to-back sequence of 128-bit vector instructions */
        "vadd.i32 q0, q0, q1\n"
        "vsub.i32 q2, q0, q1\n"
        "veor q3, q2, q0\n"
        "vmul.f32 q4, q4, q5\n"
        /* Store out_q2, out_q3, out_q4 */
        "vst1.32 {d4-d5}, [r1]!\n"    /* out_q2 */
        "vst1.32 {d6-d7}, [r1]!\n"    /* out_q3 */
        "vst1.32 {d8-d9}, [r1]!\n"    /* out_q4 */
        /* Read s0, s1, s2, s3 */
        "vstr s0, [r1, #0]\n"
        "vstr s1, [r1, #4]\n"
        "vstr s2, [r1, #8]\n"
        "vstr s3, [r1, #12]\n"
        "add r1, r1, #16\n"
        /* Read d0, d1 */
        "vst1.32 {d0-d1}, [r1]!\n"    /* out_d0, out_d1 */
        /* Modify s1 directly from new_s1 */
        "vldr s1, [r1, #0]\n"
        "add r1, r1, #4\n"
        /* Read q0 again after modifying s1 */
        "vst1.32 {d0-d1}, [r1]!\n"    /* final_q0 */
        :
        : "r"(&data)
        : "r1", "q0", "q1", "q2", "q3", "q4", "q5", "memory"
    );

    /* Assert exact bit-for-bit results */
    if (data.out_q2[0] != 10 || data.out_q2[1] != 20 || data.out_q2[2] != 30 || data.out_q2[3] != 40) {
        return 301;
    }
    if (data.out_q3[0] != 1 || data.out_q3[1] != 2 || data.out_q3[2] != 63 || data.out_q3[3] != 4) {
        return 302;
    }
    if (data.out_q4[0] != 3.0f || data.out_q4[1] != 7.5f || data.out_q4[2] != 2.0f || data.out_q4[3] != 50.0f) {
        return 303;
    }
    if (data.out_s[0] != 11U || data.out_s[1] != 22U || data.out_s[2] != 33U || data.out_s[3] != 44U) {
        return 304;
    }
    if (data.out_d0[0] != 11U || data.out_d0[1] != 22U || data.out_d1[0] != 33U || data.out_d1[1] != 44U) {
        return 305;
    }
    if (data.final_q0[0] != 11 || data.final_q0[1] != 9999 || data.final_q0[2] != 33 || data.final_q0[3] != 44) {
        return 306;
    }

    /* 2. Self-Loop VFP/NEON Preheader Promotion + Register-Cached VMOV/VCVT + Early Exit Flush */
    {
        double loop_d0 = 1.5;
        double loop_d1 = 0.5;
        int32_t gpr_sum = 0;
        int32_t final_s4 = 0;
        /* Single-block self-loop with early conditional break at iter 20 (out of 50):
         * Verifies both normal loop iterations (with d0, d1, s4 promoted into v8..v31)
         * and dirty VFP write-back on early mid-block conditional exit! */
        __asm__ volatile(
            "vldr d0, %[d0]\n\t"
            "vldr d1, %[d1]\n\t"
            "mov r2, #0\n\t"
            "mov %[sum], #0\n"
            "1:\n\t"
            "vadd.f64 d0, d0, d1\n\t"
            "vcvt.s32.f64 s4, d0\n\t"
            "vmov r3, s4\n\t"
            "add %[sum], %[sum], r3\n\t"
            "add r2, r2, #1\n\t"
            "cmp r2, #20\n\t"
            "beq 2f\n\t"
            "cmp r2, #50\n\t"
            "blt 1b\n"
            "2:\n\t"
            "vstr d0, %[d0]\n\t"
            "vmov %[s4], s4\n\t"
            : [d0] "+m"(loop_d0), [sum] "=&r"(gpr_sum), [s4] "=&r"(final_s4)
            : [d1] "m"(loop_d1)
            : "r2", "r3", "d0", "d1", "d2", "memory", "cc"
        );
        /* After 20 iterations: loop_d0 = 1.5 + 20 * 0.5 = 11.5, final_s4 = 11,
         * gpr_sum = sum_{k=1..20} trunc(1.5 + 0.5*k) = 2+2+3+3+...+11+11 = 130 */
        if (loop_d0 != 11.5 || final_s4 != 11 || gpr_sum != 130) {
            return 307;
        }
    }

    /* 3. Self-Loop with Cross-Width S/D/Q Aliasing (vfp_preheader_ineligible fallback) */
    {
        uint32_t q_buf[4] = { 1U, 2U, 3U, 4U };
        __asm__ volatile(
            "mov r1, %[buf]\n\t"
            "vld1.32 {d0-d1}, [r1]\n\t"
            "mov r2, #10\n"
            "1:\n\t"
            "vadd.i32 q0, q0, q0\n\t"
            "vmov r3, s1\n\t"
            "add r3, r3, #1\n\t"
            "vmov s1, r3\n\t"
            "subs r2, r2, #1\n\t"
            "bne 1b\n\t"
            "vst1.32 {d0-d1}, [r1]\n\t"
            :
            : [buf] "r"(q_buf)
            : "r1", "r2", "r3", "q0", "memory", "cc"
        );
        /* Lane 0: 1 << 10 = 1024; Lane 1 (s1): ((2*2+1)*2+1)... = 2*1024 + 1023 = 3071;
         * Lane 2: 3 << 10 = 3072; Lane 3: 4 << 10 = 4096 */
        if (q_buf[0] != 1024U || q_buf[1] != 3071U || q_buf[2] != 3072U || q_buf[3] != 4096U) {
            return 308;
        }
    }

    static const char pass_msg[] = "M6 NEON Q-D-S Cross-Width Aliasing: PASS\n";
    sys3(4U /* write */, 1U, (uint32_t)(uintptr_t)pass_msg, (uint32_t)(sizeof(pass_msg) - 1U));
    return 0;
}

