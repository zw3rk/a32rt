/*
 * tests/test_m1_part3.c — M1 ISA & FS tests (part 3)
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_m1_common.h"

int run_test_m1_part3(void)
{
    /*
     * 23. Regression tests for MSR APSR_nzcvq, Rm, MSR APSR_g, Rm (GE[3:0] bits [19:16]
     *     read back via SEL and MRS), and MSR APSR_nzcvqg, Rm.
     */
    {
        uint32_t sel_a = 0x11223344U;
        uint32_t sel_b = 0xAABBCCDDU;
        uint32_t sel_out = 0U;
        uint32_t flags = 0U;
        uint32_t c1 = 10U, c2 = 20U;

        /* 23a: MSR APSR_g, Rm with GE = 0b0101 (0x00050000) preserving Z=1, C=1 (0x60000000) */
        uint32_t ge_0101 = 0x00050000U;
        __asm__ volatile(
            "cmp %[c1], %[c1]\n\t"
            "msr apsr_g, %[ge]\n\t"
            "mrs %[fl], apsr\n\t"
            "sel %[out], %[sa], %[sb]\n\t"
            : [out] "=&r"(sel_out), [fl] "=&r"(flags)
            : [c1] "r"(c1), [ge] "r"(ge_0101), [sa] "r"(sel_a), [sb] "r"(sel_b)
            : "cc"
        );
        if (sel_out != 0xAA22CC44U || (flags & 0xF00F0000U) != 0x60050000U) {
            return 131;
        }

        /* 23b: MSR APSR_g, Rm with GE = 0b1010 (0x000A0000) preserving N=1 (0x80000000) */
        uint32_t ge_1010 = 0x000A0000U;
        __asm__ volatile(
            "cmp %[c1], %[c2]\n\t"
            "msr apsr_g, %[ge]\n\t"
            "mrs %[fl], apsr\n\t"
            "sel %[out], %[sa], %[sb]\n\t"
            : [out] "=&r"(sel_out), [fl] "=&r"(flags)
            : [c1] "r"(c1), [c2] "r"(c2), [ge] "r"(ge_1010), [sa] "r"(sel_a), [sb] "r"(sel_b)
            : "cc"
        );
        if (sel_out != 0x11BB33DDU || (flags & 0xF00F0000U) != 0x800A0000U) {
            return 132;
        }

        /* 23c: MSR APSR_nzcvq, Rm updates NZCV without clobbering GE[3:0] */
        uint32_t nzcv_a = 0xA0000000U; /* N=1, Z=0, C=1, V=0 */
        uint32_t nzcv_5 = 0x50000000U; /* N=0, Z=1, C=0, V=1 */
        __asm__ volatile(
            "msr apsr_g, %[ge]\n\t"
            "msr apsr_nzcvq, %[nzcv]\n\t"
            "mrs %[fl], apsr\n\t"
            "sel %[out], %[sa], %[sb]\n\t"
            : [out] "=&r"(sel_out), [fl] "=&r"(flags)
            : [ge] "r"(ge_0101), [nzcv] "r"(nzcv_a), [sa] "r"(sel_a), [sb] "r"(sel_b)
            : "cc"
        );
        if (sel_out != 0xAA22CC44U || (flags & 0xF00F0000U) != 0xA0050000U) {
            return 133;
        }
        __asm__ volatile(
            "msr apsr_nzcvq, %[nzcv]\n\t"
            "mrs %[fl], apsr\n\t"
            : [fl] "=&r"(flags)
            : [nzcv] "r"(nzcv_5)
            : "cc"
        );
        if ((flags & 0xF00F0000U) != 0x50050000U) {
            return 134;
        }

        /* 23d: MSR APSR_nzcvqg, Rm updates both NZCV and GE[3:0] */
        uint32_t both_9c = 0x900C0000U; /* N=1, Z=0, C=0, V=1, GE=0b1100 */
        __asm__ volatile(
            "msr apsr_nzcvqg, %[both]\n\t"
            "mrs %[fl], apsr\n\t"
            "sel %[out], %[sa], %[sb]\n\t"
            : [out] "=&r"(sel_out), [fl] "=&r"(flags)
            : [both] "r"(both_9c), [sa] "r"(sel_a), [sb] "r"(sel_b)
            : "cc"
        );
        if (sel_out != 0x1122CCDDU || (flags & 0xF00F0000U) != 0x900C0000U) {
            return 135;
        }
    }

    /*
     * 24. Regression tests for SSAT16, USAT16, and UADD8 / USUB8 + SEL:
     *     Verify dual 16-bit signed/unsigned saturation, parallel bytewise
     *     addition/subtraction GE[3:0] flags via SEL, and NZCV preservation.
     */
    {
        uint32_t res = 0U, flags = 0U;
        uint32_t c1 = 10U, c2 = 20U;

        /* 24a: SSAT16 #8 (signed range [-128, +127] = [0xFF80, 0x007F]) */
        uint32_t s16_in1 = 0x012CFED4U; /* hi = +300 -> +127 (0x007F), lo = -300 -> -128 (0xFF80) */
        __asm__ volatile(
            "cmp %[c1], %[c1]\n\t"
            "ssat16 %[res], #8, %[in]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [c1] "r"(c1), [in] "r"(s16_in1)
            : "cc"
        );
        if (res != 0x007FFF80U || (flags & 0xF0000000U) != 0x60000000U) {
            return 136;
        }

        uint32_t s16_in2 = 0xFFD600C8U; /* hi = -42 (in-range), lo = +200 -> +127 (0x007F) */
        __asm__ volatile(
            "cmp %[c1], %[c2]\n\t"
            "ssat16 %[res], #8, %[in]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [c1] "r"(c1), [c2] "r"(c2), [in] "r"(s16_in2)
            : "cc"
        );
        if (res != 0xFFD6007FU || (flags & 0xF0000000U) != 0x80000000U) {
            return 137;
        }

        /* 24b: USAT16 #8 (unsigned range [0, 255] = [0x0000, 0x00FF]) */
        uint32_t u16_in1 = 0xFFCE01F4U; /* hi = -50 -> 0, lo = +500 -> 255 (0x00FF) */
        __asm__ volatile(
            "cmp %[c1], %[c1]\n\t"
            "usat16 %[res], #8, %[in]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [c1] "r"(c1), [in] "r"(u16_in1)
            : "cc"
        );
        if (res != 0x000000FFU || (flags & 0xF0000000U) != 0x60000000U) {
            return 138;
        }

        uint32_t u16_in2 = 0x03E8002AU; /* hi = +1000 -> 255 (0x00FF), lo = +42 (in-range) */
        __asm__ volatile(
            "cmp %[c1], %[c2]\n\t"
            "usat16 %[res], #8, %[in]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [c1] "r"(c1), [c2] "r"(c2), [in] "r"(u16_in2)
            : "cc"
        );
        if (res != 0x00FF002AU || (flags & 0xF0000000U) != 0x80000000U) {
            return 139;
        }

        /* 24c: UADD8 + SEL (both A32 and T32) */
        uint32_t ua_rn = 0xF0208010U;
        uint32_t ua_rm = 0x203080F0U;
        uint32_t sel_a = 0x11223344U;
        uint32_t sel_b = 0xAABBCCDDU;
        uint32_t sel_out = 0U;
        __asm__ volatile(
            "cmp %[c1], %[c1]\n\t"
            "uadd8 %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            "sel %[so], %[sa], %[sb]\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags), [so] "=&r"(sel_out)
            : [c1] "r"(c1), [rn] "r"(ua_rn), [rm] "r"(ua_rm),
              [sa] "r"(sel_a), [sb] "r"(sel_b)
            : "cc"
        );
        if (res != 0x10500000U || sel_out != 0x11BB3344U ||
            (flags & 0xF00F0000U) != 0x600B0000U) {
            return 140;
        }

        /* 24d: USUB8 + SEL (both A32 and T32) */
        uint32_t us_rn = 0x10508020U;
        uint32_t us_rm = 0x20509010U;
        __asm__ volatile(
            "cmp %[c1], %[c2]\n\t"
            "usub8 %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            "sel %[so], %[sa], %[sb]\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags), [so] "=&r"(sel_out)
            : [c1] "r"(c1), [c2] "r"(c2), [rn] "r"(us_rn), [rm] "r"(us_rm),
              [sa] "r"(sel_a), [sb] "r"(sel_b)
            : "cc"
        );
        if (res != 0xF000F010U || sel_out != 0xAA22CC44U ||
            (flags & 0xF00F0000U) != 0x80050000U) {
            return 141;
        }
    }

    /*
     * 25. Regression tests for APSR.Q (sticky saturation bit 27):
     *     - Clear Q via `msr APSR_nzcvq, r0`, verify `mrs r1, apsr` has bit 27 == 0.
     *     - Verify non-saturating operations do not set Q (negative tests).
     *     - Trigger saturation in SSAT, USAT, SSAT16, USAT16, QADD, QSUB, QDADD, QDSUB,
     *       verifying each sets APSR.Q (bit 27 == 1) and that Q remains sticky across
     *       non-saturating instructions until cleared by `msr APSR_nzcvq`.
     */
    {
        const uint32_t q_bit = 1U << 27;
        uint32_t zero = 0U;
        uint32_t res = 0U;
        uint32_t flags = 0U;

        /* 25a: Direct clear and set of Q via MSR APSR_nzcvq */
        __asm__ volatile(
            "msr apsr_nzcvq, %[z]\n\t"
            "mrs %[fl], apsr\n\t"
            : [fl] "=&r"(flags)
            : [z] "r"(zero)
            : "cc"
        );
        if ((flags & q_bit) != 0U) {
            return 142;
        }
        __asm__ volatile(
            "msr apsr_nzcvq, %[qb]\n\t"
            "mrs %[fl], apsr\n\t"
            : [fl] "=&r"(flags)
            : [qb] "r"(q_bit)
            : "cc"
        );
        if ((flags & q_bit) != q_bit) {
            return 143;
        }

        /* 25b: SSAT (negative non-saturating + positive saturating + stickiness) */
        int32_t ssat_ok = 42;
        int32_t ssat_sat = 500;
        __asm__ volatile(
            "msr apsr_nzcvq, %[z]\n\t"
            "ssat %[res], #8, %[ok]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [z] "r"(zero), [ok] "r"(ssat_ok)
            : "cc"
        );
        if (res != 42U || (flags & q_bit) != 0U) {
            return 144;
        }
        __asm__ volatile(
            "msr apsr_nzcvq, %[z]\n\t"
            "ssat %[res], #8, %[sat]\n\t"
            "ssat %[res], #8, %[ok]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [z] "r"(zero), [sat] "r"(ssat_sat), [ok] "r"(ssat_ok)
            : "cc"
        );
        if (res != 42U || (flags & q_bit) != q_bit) {
            return 145;
        }

        /* 25c: USAT (negative non-saturating + positive saturating + negative input saturating) */
        int32_t usat_ok = 100;
        int32_t usat_sat = 500;
        int32_t usat_neg = -10;
        __asm__ volatile(
            "msr apsr_nzcvq, %[z]\n\t"
            "usat %[res], #8, %[ok]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [z] "r"(zero), [ok] "r"(usat_ok)
            : "cc"
        );
        if (res != 100U || (flags & q_bit) != 0U) {
            return 146;
        }
        __asm__ volatile(
            "msr apsr_nzcvq, %[z]\n\t"
            "usat %[res], #8, %[sat]\n\t"
            "usat %[res], #8, %[ok]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [z] "r"(zero), [sat] "r"(usat_sat), [ok] "r"(usat_ok)
            : "cc"
        );
        if (res != 100U || (flags & q_bit) != q_bit) {
            return 147;
        }
        __asm__ volatile(
            "msr apsr_nzcvq, %[z]\n\t"
            "usat %[res], #8, %[neg]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [z] "r"(zero), [neg] "r"(usat_neg)
            : "cc"
        );
        if (res != 0U || (flags & q_bit) != q_bit) {
            return 148;
        }

        /* 25d: SSAT16 (non-saturating vs saturating + sticky) */
        uint32_t ssat16_ok = 0x0020FFE0U;  /* hi = +32, lo = -32 (in [-128, 127]) */
        uint32_t ssat16_sat = 0x012C0010U; /* hi = +300 -> +127, lo = +16 */
        __asm__ volatile(
            "msr apsr_nzcvq, %[z]\n\t"
            "ssat16 %[res], #8, %[ok]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [z] "r"(zero), [ok] "r"(ssat16_ok)
            : "cc"
        );
        if (res != 0x0020FFE0U || (flags & q_bit) != 0U) {
            return 149;
        }
        __asm__ volatile(
            "msr apsr_nzcvq, %[z]\n\t"
            "ssat16 %[res], #8, %[sat]\n\t"
            "ssat16 %[res], #8, %[ok]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [z] "r"(zero), [sat] "r"(ssat16_sat), [ok] "r"(ssat16_ok)
            : "cc"
        );
        if (res != 0x0020FFE0U || (flags & q_bit) != q_bit) {
            return 150;
        }

        /* 25e: USAT16 (non-saturating vs saturating + sticky) */
        uint32_t usat16_ok = 0x00800040U;  /* hi = 128, lo = 64 (in [0, 255]) */
        uint32_t usat16_sat = 0x0010FFFEU; /* hi = 16, lo = -2 -> 0 */
        __asm__ volatile(
            "msr apsr_nzcvq, %[z]\n\t"
            "usat16 %[res], #8, %[ok]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [z] "r"(zero), [ok] "r"(usat16_ok)
            : "cc"
        );
        if (res != 0x00800040U || (flags & q_bit) != 0U) {
            return 151;
        }
        __asm__ volatile(
            "msr apsr_nzcvq, %[z]\n\t"
            "usat16 %[res], #8, %[sat]\n\t"
            "usat16 %[res], #8, %[ok]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [z] "r"(zero), [sat] "r"(usat16_sat), [ok] "r"(usat16_ok)
            : "cc"
        );
        if (res != 0x00800040U || (flags & q_bit) != q_bit) {
            return 152;
        }

        /* 25f: QADD, QSUB, QDADD, QDSUB setting APSR.Q and remaining sticky */
        int32_t q_100 = 100, q_200 = 200, q_50 = 50;
        int32_t q_pos_near = 0x7FFFFFF0;
        int32_t q_neg_near = (int32_t)0x80000010U;
        int32_t q_big_pos  = 0x50000000;

        /* QADD */
        __asm__ volatile(
            "msr apsr_nzcvq, %[z]\n\t"
            "qadd %[res], %[a], %[b]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [z] "r"(zero), [a] "r"(q_100), [b] "r"(q_200)
            : "cc"
        );
        if (res != 300U || (flags & q_bit) != 0U) {
            return 153;
        }
        __asm__ volatile(
            "msr apsr_nzcvq, %[z]\n\t"
            "qadd %[res], %[sat], %[a]\n\t"
            "qadd %[res], %[a], %[b]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [z] "r"(zero), [sat] "r"(q_pos_near), [a] "r"(q_100), [b] "r"(q_200)
            : "cc"
        );
        if (res != 300U || (flags & q_bit) != q_bit) {
            return 154;
        }

        /* QSUB */
        __asm__ volatile(
            "msr apsr_nzcvq, %[z]\n\t"
            "qsub %[res], %[b], %[a]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [z] "r"(zero), [a] "r"(q_100), [b] "r"(q_200)
            : "cc"
        );
        if (res != 100U || (flags & q_bit) != 0U) {
            return 155;
        }
        __asm__ volatile(
            "msr apsr_nzcvq, %[z]\n\t"
            "qsub %[res], %[neg], %[a]\n\t"
            "qsub %[res], %[b], %[a]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [z] "r"(zero), [neg] "r"(q_neg_near), [a] "r"(q_100), [b] "r"(q_200)
            : "cc"
        );
        if (res != 100U || (flags & q_bit) != q_bit) {
            return 156;
        }

        /* QDADD */
        __asm__ volatile(
            "msr apsr_nzcvq, %[z]\n\t"
            "qdadd %[res], %[a], %[b]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [z] "r"(zero), [a] "r"(q_100), [b] "r"(q_50)
            : "cc"
        );
        if (res != 200U || (flags & q_bit) != 0U) {
            return 157;
        }
        __asm__ volatile(
            "msr apsr_nzcvq, %[z]\n\t"
            "qdadd %[res], %[a], %[big]\n\t"
            "qdadd %[res], %[a], %[b]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [z] "r"(zero), [a] "r"(q_100), [b] "r"(q_50), [big] "r"(q_big_pos)
            : "cc"
        );
        if (res != 200U || (flags & q_bit) != q_bit) {
            return 158;
        }

        /* QDSUB */
        __asm__ volatile(
            "msr apsr_nzcvq, %[z]\n\t"
            "qdsub %[res], %[a], %[b]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [z] "r"(zero), [a] "r"(q_200), [b] "r"(q_50)
            : "cc"
        );
        if (res != 100U || (flags & q_bit) != 0U) {
            return 159;
        }
        __asm__ volatile(
            "msr apsr_nzcvq, %[z]\n\t"
            "qdsub %[res], %[z], %[big]\n\t"
            "qdsub %[res], %[a], %[b]\n\t"
            "mrs %[fl], apsr\n\t"
            "msr apsr_nzcvq, %[z]\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [z] "r"(zero), [a] "r"(q_200), [b] "r"(q_50), [big] "r"(q_big_pos)
            : "cc"
        );
        if (res != 100U || (flags & q_bit) != q_bit) {
            return 160;
        }
    }

    /*
     * 26. Regression tests for UADD16 and USUB16 + SEL:
     *     Verify parallel 16-bit unsigned addition and subtraction setting
     *     GE[1:0] and GE[3:2] (read back via MRS and SEL) while preserving NZCV.
     */
    {
        uint32_t res = 0U, flags = 0U, sel_out = 0U;
        uint32_t c1 = 10U, c2 = 20U;
        uint32_t sel_a = 0x11223344U;
        uint32_t sel_b = 0xAABBCCDDU;

        /* 26a: UADD16 with hi carry, lo no carry -> GE = 0b1100 (0xC) */
        uint32_t ua16_rn1 = 0xF0001234U;
        uint32_t ua16_rm1 = 0x20002345U;
        __asm__ volatile(
            "cmp %[c1], %[c1]\n\t"
            "uadd16 %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            "sel %[so], %[sa], %[sb]\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags), [so] "=&r"(sel_out)
            : [c1] "r"(c1), [rn] "r"(ua16_rn1), [rm] "r"(ua16_rm1),
              [sa] "r"(sel_a), [sb] "r"(sel_b)
            : "cc"
        );
        if (res != 0x10003579U || sel_out != 0x1122CCDDU ||
            (flags & 0xF00F0000U) != 0x600C0000U) {
            return 161;
        }

        /* 26b: UADD16 with hi no carry, lo carry -> GE = 0b0011 (0x3) */
        uint32_t ua16_rn2 = 0x10008000U;
        uint32_t ua16_rm2 = 0x20009000U;
        __asm__ volatile(
            "cmp %[c1], %[c2]\n\t"
            "uadd16 %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            "sel %[so], %[sa], %[sb]\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags), [so] "=&r"(sel_out)
            : [c1] "r"(c1), [c2] "r"(c2), [rn] "r"(ua16_rn2), [rm] "r"(ua16_rm2),
              [sa] "r"(sel_a), [sb] "r"(sel_b)
            : "cc"
        );
        if (res != 0x30001000U || sel_out != 0xAABB3344U ||
            (flags & 0xF00F0000U) != 0x80030000U) {
            return 162;
        }

        /* 26c: USUB16 with hi borrow, lo no borrow -> GE = 0b0011 (0x3) */
        uint32_t us16_rn1 = 0x10005000U;
        uint32_t us16_rm1 = 0x20003000U;
        __asm__ volatile(
            "cmp %[c1], %[c1]\n\t"
            "usub16 %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            "sel %[so], %[sa], %[sb]\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags), [so] "=&r"(sel_out)
            : [c1] "r"(c1), [rn] "r"(us16_rn1), [rm] "r"(us16_rm1),
              [sa] "r"(sel_a), [sb] "r"(sel_b)
            : "cc"
        );
        if (res != 0xF0002000U || sel_out != 0xAABB3344U ||
            (flags & 0xF00F0000U) != 0x60030000U) {
            return 163;
        }

        /* 26d: USUB16 with hi equal (no borrow), lo borrow -> GE = 0b1100 (0xC) */
        uint32_t us16_rn2 = 0x40000001U;
        uint32_t us16_rm2 = 0x40000002U;
        __asm__ volatile(
            "cmp %[c1], %[c2]\n\t"
            "usub16 %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            "sel %[so], %[sa], %[sb]\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags), [so] "=&r"(sel_out)
            : [c1] "r"(c1), [c2] "r"(c2), [rn] "r"(us16_rn2), [rm] "r"(us16_rm2),
              [sa] "r"(sel_a), [sb] "r"(sel_b)
            : "cc"
        );
        if (res != 0x0000FFFFU || sel_out != 0x1122CCDDU ||
            (flags & 0xF00F0000U) != 0x800C0000U) {
            return 164;
        }
    }

    /*
     * 27. Regression test for LDRD r0, r1, [r0] and LDM r0, {r0, r1} where
     *     base register rn is overwritten by the first loaded word.
     */
    {
        _Alignas(8) uint32_t pair[2] = { 0xDEADBEEFU, 0xCAFEBABEU };

        /* 27a: LDRD r0, r1, [r0] */
        {
            register uint32_t r0 __asm__("r0") = (uint32_t)(uintptr_t)&pair[0];
            register uint32_t r1 __asm__("r1") = 0U;
            __asm__ volatile("ldrd r0, r1, [r0]" : "+r"(r0), "=r"(r1) : : "memory");
            if (r0 != 0xDEADBEEFU || r1 != 0xCAFEBABEU) {
                return 165;
            }
        }

        /* 27b: LDM r0, {r0, r1} */
        pair[0] = 0x11223344U;
        pair[1] = 0x55667788U;
        {
            register uint32_t r0 __asm__("r0") = (uint32_t)(uintptr_t)&pair[0];
            register uint32_t r1 __asm__("r1") = 0U;
            __asm__ volatile("ldm r0, {r0, r1}" : "+r"(r0), "=r"(r1) : : "memory");
            if (r0 != 0x11223344U || r1 != 0x55667788U) {
                return 166;
            }
        }

#if defined(__thumb__)
        /* 27c: Thumb-2 32-bit LDM.W r0, {r0, r1} */
        pair[0] = 0x87654321U;
        pair[1] = 0xFEDCBA98U;
        {
            register uint32_t r0 __asm__("r0") = (uint32_t)(uintptr_t)&pair[0];
            register uint32_t r1 __asm__("r1") = 0U;
            __asm__ volatile("ldm.w r0, {r0, r1}" : "+r"(r0), "=r"(r1) : : "memory");
            if (r0 != 0x87654321U || r1 != 0xFEDCBA98U) {
                return 167;
            }
        }
#endif
    }

    /*
     * 28. Regression tests for SADD8, SSUB8, SADD16, SSUB16 + SEL (Round 5):
     *     Verify signed parallel byte and halfword addition/subtraction setting
     *     CPSR.GE[3:0] based on signed >= 0 lane results (including lanes where
     *     8-bit/16-bit signed overflow wraps the truncated lane bit 7/15),
     *     while preserving CPSR.NZCV.
     */
    {
        uint32_t res = 0U, flags = 0U, sel_out = 0U;
        uint32_t c1 = 10U, c2 = 20U;
        uint32_t sel_a = 0x11223344U;
        uint32_t sel_b = 0xAABBCCDDU;

        /*
         * 28a: SADD8 + SEL:
         *   lane 3: -112 (0x90) + 32 (0x20)  = -80  (0xB0) <  0 -> GE[3] = 0
         *   lane 2: +32  (0x20) + -32 (0xE0) = 0    (0x00) >= 0 -> GE[2] = 1
         *   lane 1: +112 (0x70) + 32 (0x20)  = +144 (0x90) >= 0 -> GE[1] = 1
         *   lane 0: +16  (0x10) + -32 (0xE0) = -16  (0xF0) <  0 -> GE[0] = 0
         *   -> res = 0xB00090F0U, GE[3:0] = 0b0110 (0x6), sel_out = 0xAA2233DDU
         */
        uint32_t sa8_rn = 0x90207010U;
        uint32_t sa8_rm = 0x20E020E0U;
        __asm__ volatile(
            "cmp %[c1], %[c1]\n\t"
            "sadd8 %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            "sel %[so], %[sa], %[sb]\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags), [so] "=&r"(sel_out)
            : [c1] "r"(c1), [rn] "r"(sa8_rn), [rm] "r"(sa8_rm),
              [sa] "r"(sel_a), [sb] "r"(sel_b)
            : "cc"
        );
        if (res != 0xB00090F0U || sel_out != 0xAA2233DDU ||
            (flags & 0xF00F0000U) != 0x60060000U) {
            return 168;
        }

        /*
         * 28b: SSUB8 + SEL:
         *   lane 3: +16  (0x10) - -32 (0xE0) = +48  (0x30) >= 0 -> GE[3] = 1
         *   lane 2: -112 (0x90) - +32 (0x20) = -144 (0x70) <  0 -> GE[2] = 0
         *   lane 1: +32  (0x20) - +48 (0x30) = -16  (0xF0) <  0 -> GE[1] = 0
         *   lane 0: +80  (0x50) - +80 (0x50) = 0    (0x00) >= 0 -> GE[0] = 1
         *   -> res = 0x3070F000U, GE[3:0] = 0b1001 (0x9), sel_out = 0x11BBCC44U
         */
        uint32_t ss8_rn = 0x10902050U;
        uint32_t ss8_rm = 0xE0203050U;
        __asm__ volatile(
            "cmp %[c1], %[c2]\n\t"
            "ssub8 %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            "sel %[so], %[sa], %[sb]\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags), [so] "=&r"(sel_out)
            : [c1] "r"(c1), [c2] "r"(c2), [rn] "r"(ss8_rn), [rm] "r"(ss8_rm),
              [sa] "r"(sel_a), [sb] "r"(sel_b)
            : "cc"
        );
        if (res != 0x3070F000U || sel_out != 0x11BBCC44U ||
            (flags & 0xF00F0000U) != 0x80090000U) {
            return 169;
        }

        /*
         * 28c: SADD16 + SEL:
         *   hi: +28672 (0x7000) + +8192 (0x2000) = +36864 (0x9000) >= 0 -> GE[3:2] = 0b11
         *   lo: -4096  (0xF000) + +2048 (0x0800) = -2048  (0xF800) <  0 -> GE[1:0] = 0b00
         *   -> res = 0x9000F800U, GE[3:0] = 0b1100 (0xC), sel_out = 0x1122CCDDU
         */
        uint32_t sa16_rn = 0x7000F000U;
        uint32_t sa16_rm = 0x20000800U;
        __asm__ volatile(
            "cmp %[c1], %[c1]\n\t"
            "sadd16 %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            "sel %[so], %[sa], %[sb]\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags), [so] "=&r"(sel_out)
            : [c1] "r"(c1), [rn] "r"(sa16_rn), [rm] "r"(sa16_rm),
              [sa] "r"(sel_a), [sb] "r"(sel_b)
            : "cc"
        );
        if (res != 0x9000F800U || sel_out != 0x1122CCDDU ||
            (flags & 0xF00F0000U) != 0x600C0000U) {
            return 170;
        }

        /*
         * 28d: SSUB16 + SEL:
         *   hi: -28672 (0x9000) - +8192 (0x2000) = -36864 (0x7000) <  0 -> GE[3:2] = 0b00
         *   lo: +4096  (0x1000) - -4096 (0xF000) = +8192  (0x2000) >= 0 -> GE[1:0] = 0b11
         *   -> res = 0x70002000U, GE[3:0] = 0b0011 (0x3), sel_out = 0xAABB3344U
         */
        uint32_t ss16_rn = 0x90001000U;
        uint32_t ss16_rm = 0x2000F000U;
        __asm__ volatile(
            "cmp %[c1], %[c2]\n\t"
            "ssub16 %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            "sel %[so], %[sa], %[sb]\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags), [so] "=&r"(sel_out)
            : [c1] "r"(c1), [c2] "r"(c2), [rn] "r"(ss16_rn), [rm] "r"(ss16_rm),
              [sa] "r"(sel_a), [sb] "r"(sel_b)
            : "cc"
        );
        if (res != 0x70002000U || sel_out != 0xAABB3344U ||
            (flags & 0xF00F0000U) != 0x80030000U) {
            return 171;
        }
    }

    return 0;
}
