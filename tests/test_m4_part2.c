/*
 * tests/test_m4_part2.c — M4 VFPv4-D32 & NEON tests (part 2: sections 21..27)
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_m4_common.h"

int run_vfp_neon_part2(void)
{
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
            : "=&r"(fp16_packed), "=&r"(fp16_out_b), "=r"(fp16_out_t)
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
            : "=&r"(fxp_s32), "=r"(fxp_f32)
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

    /*
     * 25. FPSCR <-> hardware FPCR/FPSR synchronization (Round 4 regression):
     *     - 25a: FPSCR.RMode (bits [23:22]) rounding modes with VCVTR.S32.F32:
     *            0 = RN (Nearest), 1 = RP (+Inf), 2 = RM (-Inf), 3 = RZ (Zero)
     *     - 25b: Sticky FPSCR.QC (bit 27) readback after NEON saturating ops
     *            (negative non-saturating check + positive saturating check)
     */
    {
        uint32_t saved_fpscr = 0U;
        __asm__ volatile("vmrs %0, fpscr" : "=r"(saved_fpscr));

        float pos_v = 1.25f;
        float neg_v = -1.25f;
        int32_t r_pos = 0;
        int32_t r_neg = 0;

        /* RMode = 1 (0x00400000: Round towards +Inf / RP): 1.25 -> +2, -1.25 -> -1 */
        uint32_t fpscr_rp = (saved_fpscr & ~0x00C00000U) | 0x00400000U;
        __asm__ volatile(
            "vmsr fpscr, %2\n"
            "vldr s0, %3\n"
            "vldr s1, %4\n"
            "vcvtr.s32.f32 s2, s0\n"
            "vcvtr.s32.f32 s3, s1\n"
            "vmov %0, s2\n"
            "vmov %1, s3\n"
            : "=r"(r_pos), "=r"(r_neg)
            : "r"(fpscr_rp), "m"(pos_v), "m"(neg_v)
            : "s0", "s1", "s2", "s3"
        );
        if (r_pos != 2 || r_neg != -1) {
            __asm__ volatile("vmsr fpscr, %0" : : "r"(saved_fpscr));
            return 62;
        }

        /* RMode = 2 (0x00800000: Round towards -Inf / RM): 1.25 -> +1, -1.25 -> -2 */
        uint32_t fpscr_rm = (saved_fpscr & ~0x00C00000U) | 0x00800000U;
        __asm__ volatile(
            "vmsr fpscr, %2\n"
            "vldr s0, %3\n"
            "vldr s1, %4\n"
            "vcvtr.s32.f32 s2, s0\n"
            "vcvtr.s32.f32 s3, s1\n"
            "vmov %0, s2\n"
            "vmov %1, s3\n"
            : "=r"(r_pos), "=r"(r_neg)
            : "r"(fpscr_rm), "m"(pos_v), "m"(neg_v)
            : "s0", "s1", "s2", "s3"
        );
        if (r_pos != 1 || r_neg != -2) {
            __asm__ volatile("vmsr fpscr, %0" : : "r"(saved_fpscr));
            return 63;
        }

        /* RMode = 3 (0x00C00000: Round towards Zero / RZ): 1.75 -> +1, -1.75 -> -1 */
        float pos_v2 = 1.75f;
        float neg_v2 = -1.75f;
        uint32_t fpscr_rz = (saved_fpscr & ~0x00C00000U) | 0x00C00000U;
        __asm__ volatile(
            "vmsr fpscr, %2\n"
            "vldr s0, %3\n"
            "vldr s1, %4\n"
            "vcvtr.s32.f32 s2, s0\n"
            "vcvtr.s32.f32 s3, s1\n"
            "vmov %0, s2\n"
            "vmov %1, s3\n"
            : "=r"(r_pos), "=r"(r_neg)
            : "r"(fpscr_rz), "m"(pos_v2), "m"(neg_v2)
            : "s0", "s1", "s2", "s3"
        );
        if (r_pos != 1 || r_neg != -1) {
            __asm__ volatile("vmsr fpscr, %0" : : "r"(saved_fpscr));
            return 64;
        }

        /* RMode = 0 (0x00000000: Round to Nearest / RN): 1.75 -> +2, -1.75 -> -2 */
        uint32_t fpscr_rn = (saved_fpscr & ~0x00C00000U);
        __asm__ volatile(
            "vmsr fpscr, %2\n"
            "vldr s0, %3\n"
            "vldr s1, %4\n"
            "vcvtr.s32.f32 s2, s0\n"
            "vcvtr.s32.f32 s3, s1\n"
            "vmov %0, s2\n"
            "vmov %1, s3\n"
            : "=r"(r_pos), "=r"(r_neg)
            : "r"(fpscr_rn), "m"(pos_v2), "m"(neg_v2)
            : "s0", "s1", "s2", "s3"
        );
        if (r_pos != 2 || r_neg != -2) {
            __asm__ volatile("vmsr fpscr, %0" : : "r"(saved_fpscr));
            return 65;
        }

        /*
         * 25b: FPSCR.QC (bit 27) sticky saturation flag:
         *      - Clear QC via VMSR, run non-saturating VQADD.S16 -> QC stays 0
         *      - Run saturating VQADD.S16 -> QC becomes 1 and stays 1 across
         *        subsequent non-saturating VQADD.S16
         */
        int16_t ns_a[4]  = { 10, -20, 30, -40 };
        int16_t ns_b[4]  = { 5,  -10, 15, -20 };
        int16_t sat_a[4] = { 30000, -30000, 100, -100 };
        int16_t sat_b[4] = { 10000, -10000, 200, -200 };
        int16_t q_out[4] = { 0, 0, 0, 0 };
        uint32_t qc_clear = saved_fpscr & ~(1U << 27);
        uint32_t fpscr_after_ns = 0U;
        uint32_t fpscr_after_sat = 0U;
        uint32_t fpscr_after_sticky = 0U;

        __asm__ volatile(
            "vmsr fpscr, %3\n"
            "vld1.16 {d0}, [%4]\n"
            "vld1.16 {d1}, [%5]\n"
            "vqadd.s16 d2, d0, d1\n"
            "vmrs %0, fpscr\n"
            "vld1.16 {d0}, [%6]\n"
            "vld1.16 {d1}, [%7]\n"
            "vqadd.s16 d2, d0, d1\n"
            "vst1.16 {d2}, [%2]\n"
            "vmrs %1, fpscr\n"
            : "=&r"(fpscr_after_ns), "=r"(fpscr_after_sat)
            : "r"(q_out), "r"(qc_clear), "r"(ns_a), "r"(ns_b), "r"(sat_a), "r"(sat_b)
            : "d0", "d1", "d2", "memory"
        );
        if ((fpscr_after_ns & (1U << 27)) != 0U) {
            __asm__ volatile("vmsr fpscr, %0" : : "r"(saved_fpscr));
            return 66;
        }
        if ((fpscr_after_sat & (1U << 27)) == 0U ||
            q_out[0] != 32767 || q_out[1] != -32768 ||
            q_out[2] != 300   || q_out[3] != -300) {
            __asm__ volatile("vmsr fpscr, %0" : : "r"(saved_fpscr));
            return 67;
        }

        /* Verify QC stays sticky after a non-saturating VQADD.S16, then clears via VMSR */
        uint32_t fpscr_after_clear = 0U;
        __asm__ volatile(
            "vld1.16 {d0}, [%2]\n"
            "vld1.16 {d1}, [%3]\n"
            "vqadd.s16 d2, d0, d1\n"
            "vmrs %0, fpscr\n"
            "vmsr fpscr, %4\n"
            "vmrs %1, fpscr\n"
            : "=&r"(fpscr_after_sticky), "=r"(fpscr_after_clear)
            : "r"(ns_a), "r"(ns_b), "r"(qc_clear)
            : "d0", "d1", "d2"
        );
        __asm__ volatile("vmsr fpscr, %0" : : "r"(saved_fpscr));
        if ((fpscr_after_sticky & (1U << 27)) == 0U ||
            (fpscr_after_clear & (1U << 27)) != 0U) {
            return 68;
        }
    }

    /*
     * 26. VFPv4 NEON half-precision vector VCVT.F16.F32 / VCVT.F32.F16,
     *     polynomial multiply VMUL.P8 / VMULL.P8, and 16-bit fixed-point
     *     VCVT.S16.F32 / VCVT.U16.F32 saturation (Round 4 regression):
     */
    {
        /* 26a: NEON vector VCVT.F16.F32 Dd, Qm & VCVT.F32.F16 Qd, Dm */
        float f32_in[4]     = { 1.0f, -2.5f, 0.5f, 6.25f };
        uint16_t f16_mid[4] = { 0U, 0U, 0U, 0U };
        float f32_out[4]    = { 0.0f, 0.0f, 0.0f, 0.0f };
        __asm__ volatile(
            "vld1.32 {d2, d3}, [%2]\n"
            "vcvt.f16.f32 d0, q1\n"
            "vst1.16 {d0}, [%0]\n"
            "vcvt.f32.f16 q2, d0\n"
            "vst1.32 {d4, d5}, [%1]\n"
            :
            : "r"(f16_mid), "r"(f32_out), "r"(f32_in)
            : "d0", "d2", "d3", "d4", "d5", "memory"
        );
        if (f16_mid[0] != 0x3C00U || f16_mid[1] != 0xC100U ||
            f16_mid[2] != 0x3800U || f16_mid[3] != 0x4640U ||
            f32_out[0] != 1.0f    || f32_out[1] != -2.5f   ||
            f32_out[2] != 0.5f    || f32_out[3] != 6.25f) {
            return 69;
        }

        /*
         * 26b: Polynomial multiply over GF(2)[x]: VMUL.P8 (64-bit D form)
         *      and widening VMULL.P8 Qd, Dn, Dm
         *      - 0x03 (x + 1) * 0x03 (x + 1) = x^2 + 1 = 0x05
         *      - 0x07 (x^2 + x + 1) * 0x03 (x + 1) = x^3 + 1 = 0x09
         *      - 0xFF * 0x02 = 0x1FE (low byte 0xFE in VMUL.P8, 0x01FE in VMULL.P8)
         *      - 0x80 * 0x80 = 0x4000 (low byte 0x00 in VMUL.P8, 0x4000 in VMULL.P8)
         */
        uint8_t p_a[8]      = { 0x03U, 0x07U, 0xFFU, 0x80U, 0x05U, 0x00U, 0x01U, 0x0FU };
        uint8_t p_b[8]      = { 0x03U, 0x03U, 0x02U, 0x80U, 0x05U, 0xFFU, 0xABU, 0x0FU };
        uint8_t pmul_out[8] = { 0U };
        uint16_t pmull_out[8] = { 0U };
        __asm__ volatile(
            "vld1.8 {d0}, [%2]\n"
            "vld1.8 {d1}, [%3]\n"
            "vmul.p8 d2, d0, d1\n"
            "vst1.8 {d2}, [%0]\n"
            "vmull.p8 q2, d0, d1\n"
            "vst1.16 {d4, d5}, [%1]\n"
            :
            : "r"(pmul_out), "r"(pmull_out), "r"(p_a), "r"(p_b)
            : "d0", "d1", "d2", "d4", "d5", "memory"
        );
        if (pmul_out[0] != 0x05U || pmul_out[1] != 0x09U ||
            pmul_out[2] != 0xFEU || pmul_out[3] != 0x00U ||
            pmul_out[4] != 0x11U || pmul_out[5] != 0x00U ||
            pmul_out[6] != 0xABU || pmul_out[7] != 0x55U) {
            return 70;
        }
        if (pmull_out[0] != 0x0005U || pmull_out[1] != 0x0009U ||
            pmull_out[2] != 0x01FEU || pmull_out[3] != 0x4000U ||
            pmull_out[4] != 0x0011U || pmull_out[5] != 0x0000U ||
            pmull_out[6] != 0x00ABU || pmull_out[7] != 0x0055U) {
            return 71;
        }

        /*
         * 26c: 16-bit fixed-point VCVT.S16.F32 / VCVT.F32.S16 and
         *      VCVT.U16.F32 / VCVT.F32.U16 (including saturation to
         *      [-32768, 32767] and [0, 65535])
         */
        float s16_in      = 12.5f;   /* 12.5 * 16 = 200 */
        float s16_pos_sat = 3000.0f; /* 3000 * 16 = 48000 -> saturates to 32767 */
        float s16_neg_sat = -3000.0f;/* -3000 * 16 = -48000 -> saturates to -32768 */
        int32_t s16_fxp   = 0;
        int32_t s16_hi    = 0;
        int32_t s16_lo    = 0;
        float s16_rt      = 0.0f;
        __asm__ volatile(
            "vldr s0, %4\n"
            "vcvt.s16.f32 s0, s0, #4\n"
            "vmov %0, s0\n"
            "vcvt.f32.s16 s0, s0, #4\n"
            "vstr s0, %3\n"
            "vldr s1, %5\n"
            "vcvt.s16.f32 s1, s1, #4\n"
            "vmov %1, s1\n"
            "vldr s2, %6\n"
            "vcvt.s16.f32 s2, s2, #4\n"
            "vmov %2, s2\n"
            : "=&r"(s16_fxp), "=&r"(s16_hi), "=&r"(s16_lo), "=m"(s16_rt)
            : "m"(s16_in), "m"(s16_pos_sat), "m"(s16_neg_sat)
            : "s0", "s1", "s2"
        );
        if (s16_fxp != 200 || s16_rt != 12.5f ||
            s16_hi != 32767 || s16_lo != -32768) {
            return 72;
        }

        float u16_in      = 25.25f;  /* 25.25 * 16 = 404 */
        float u16_pos_sat = 5000.0f; /* 5000 * 16 = 80000 -> saturates to 65535 */
        float u16_neg_sat = -5.0f;   /* negative -> saturates to 0 */
        uint32_t u16_fxp  = 0U;
        uint32_t u16_hi   = 0U;
        uint32_t u16_lo   = 1U;
        float u16_rt      = 0.0f;
        __asm__ volatile(
            "vldr s0, %4\n"
            "vcvt.u16.f32 s0, s0, #4\n"
            "vmov %0, s0\n"
            "vcvt.f32.u16 s0, s0, #4\n"
            "vstr s0, %3\n"
            "vldr s1, %5\n"
            "vcvt.u16.f32 s1, s1, #4\n"
            "vmov %1, s1\n"
            "vldr s2, %6\n"
            "vcvt.u16.f32 s2, s2, #4\n"
            "vmov %2, s2\n"
            : "=&r"(u16_fxp), "=&r"(u16_hi), "=&r"(u16_lo), "=m"(u16_rt)
            : "m"(u16_in), "m"(u16_pos_sat), "m"(u16_neg_sat)
            : "s0", "s1", "s2"
        );
        if (u16_fxp != 404U || u16_rt != 25.25f ||
            u16_hi != 65535U || u16_lo != 0U) {
            return 73;
        }
    }

    /*
     * 27. Double-precision fixed-point VCVT.<S32/S16/U32/U16>.F64 64-bit Dd
     *     sign/zero-extension and VLD4.32 {d0[], d1[], d2[], d3[]}, [r0:128]
     *     (Round 5 regression):
     */
    {
        /*
         * 27a: vcvt.s32.f64 d0, d0, #8 with -2.5  -> -640 (0xFFFFFFFFFFFFFD80ULL)
         *      vcvt.s16.f64 d1, d1, #4 with -12.5 -> -200 (0xFFFFFFFFFFFFFF38ULL)
         *      vcvt.u32.f64 d2, d2, #8 with +2.5  -> +640 (0x0000000000000280ULL)
         *      vcvt.u16.f64 d3, d3, #4 with +12.5 -> +200 (0x00000000000000C8ULL)
         */
        double d_neg_2_5  = -2.5;
        double d_neg_12_5 = -12.5;
        double d_pos_2_5  = 2.5;
        double d_pos_12_5 = 12.5;
        uint64_t raw_s32_d0 = 0ULL;
        uint64_t raw_s16_d1 = 0ULL;
        uint64_t raw_u32_d2 = 0xFFFFFFFFFFFFFFFFULL;
        uint64_t raw_u16_d3 = 0xFFFFFFFFFFFFFFFFULL;
        double rt_s32 = 0.0;
        double rt_s16 = 0.0;

        __asm__ volatile(
            "vldr d0, %6\n"
            "vldr d1, %7\n"
            "vldr d2, %8\n"
            "vldr d3, %9\n"
            "vcvt.s32.f64 d0, d0, #8\n"
            "vcvt.s16.f64 d1, d1, #4\n"
            "vcvt.u32.f64 d2, d2, #8\n"
            "vcvt.u16.f64 d3, d3, #4\n"
            "vstr d0, %0\n"
            "vstr d1, %1\n"
            "vstr d2, %2\n"
            "vstr d3, %3\n"
            "vcvt.f64.s32 d0, d0, #8\n"
            "vcvt.f64.s16 d1, d1, #4\n"
            "vstr d0, %4\n"
            "vstr d1, %5\n"
            : "=m"(raw_s32_d0), "=m"(raw_s16_d1),
              "=m"(raw_u32_d2), "=m"(raw_u16_d3),
              "=m"(rt_s32), "=m"(rt_s16)
            : "m"(d_neg_2_5), "m"(d_neg_12_5),
              "m"(d_pos_2_5), "m"(d_pos_12_5)
            : "d0", "d1", "d2", "d3", "memory"
        );
        if (raw_s32_d0 != 0xFFFFFFFFFFFFFD80ULL ||
            (uint32_t)(raw_s32_d0 >> 32) != 0xFFFFFFFFU ||
            raw_s16_d1 != 0xFFFFFFFFFFFFFF38ULL ||
            (uint32_t)(raw_s16_d1 >> 32) != 0xFFFFFFFFU) {
            return 74;
        }
        if (raw_u32_d2 != 0x0000000000000280ULL ||
            (uint32_t)(raw_u32_d2 >> 32) != 0x00000000U ||
            raw_u16_d3 != 0x00000000000000C8ULL ||
            (uint32_t)(raw_u16_d3 >> 32) != 0x00000000U ||
            rt_s32 != -2.5 || rt_s16 != -12.5) {
            return 75;
        }

        /*
         * 27b: Positive test for vld4.32 {d0[], d1[], d2[], d3[]}, [r0:128]
         *      (all-lanes 4-element 32-bit structure load with 128-bit alignment hint)
         */
        static const _Alignas(16) uint32_t vld4_in[4] = {
            0x11223344U, 0x55667788U, 0x99AABBCCU, 0xDDEEFF00U
        };
        uint64_t vld4_out[4] = { 0ULL, 0ULL, 0ULL, 0ULL };
        __asm__ volatile(
            "vld4.32 {d0[], d1[], d2[], d3[]}, [%1:128]\n"
            "vstmia %0, {d0-d3}\n"
            :
            : "r"(vld4_out), "r"(vld4_in)
            : "d0", "d1", "d2", "d3", "memory"
        );
        if (vld4_out[0] != 0x1122334411223344ULL ||
            vld4_out[1] != 0x5566778855667788ULL ||
            vld4_out[2] != 0x99AABBCC99AABBCCULL ||
            vld4_out[3] != 0xDDEEFF00DDEEFF00ULL) {
            return 76;
        }
    }

    return 0;
}
