/*
 * tests/test_m1_part2.c — M1 ISA & FS tests (part 2)
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_m1_common.h"

static uint32_t __attribute__((noinline)) arm_lsl_reg(uint32_t val, uint32_t sh)
{
    uint32_t out;
    __asm__ volatile("lsl %0, %1, %2" : "=r"(out) : "r"(val), "r"(sh));
    return out;
}

static uint32_t __attribute__((noinline)) arm_lsr_reg(uint32_t val, uint32_t sh)
{
    uint32_t out;
    __asm__ volatile("lsr %0, %1, %2" : "=r"(out) : "r"(val), "r"(sh));
    return out;
}

static uint32_t __attribute__((noinline)) arm_asr_reg(uint32_t val, uint32_t sh)
{
    uint32_t out;
    __asm__ volatile("asr %0, %1, %2" : "=r"(out) : "r"(val), "r"(sh));
    return out;
}

static uint32_t __attribute__((noinline))
arm_lsls_reg_c(uint32_t val, uint32_t sh, uint32_t c_in, uint32_t *out_c)
{
    uint32_t out = 0U;
    uint32_t fl  = 0U;
    __asm__ volatile(
        "subs %[fl], %[cin], #1\n\t"
        "lsls %[out], %[val], %[sh]\n\t"
        "mrs  %[fl], apsr\n\t"
        : [out] "=&r"(out), [fl] "=&r"(fl)
        : [val] "r"(val), [sh] "r"(sh), [cin] "r"(c_in)
        : "cc"
    );
    *out_c = (fl >> 29) & 1U;
    return out;
}

static uint32_t __attribute__((noinline))
arm_lsrs_reg_c(uint32_t val, uint32_t sh, uint32_t c_in, uint32_t *out_c)
{
    uint32_t out = 0U;
    uint32_t fl  = 0U;
    __asm__ volatile(
        "subs %[fl], %[cin], #1\n\t"
        "lsrs %[out], %[val], %[sh]\n\t"
        "mrs  %[fl], apsr\n\t"
        : [out] "=&r"(out), [fl] "=&r"(fl)
        : [val] "r"(val), [sh] "r"(sh), [cin] "r"(c_in)
        : "cc"
    );
    *out_c = (fl >> 29) & 1U;
    return out;
}

static uint32_t __attribute__((noinline))
arm_asrs_reg_c(uint32_t val, uint32_t sh, uint32_t c_in, uint32_t *out_c)
{
    uint32_t out = 0U;
    uint32_t fl  = 0U;
    __asm__ volatile(
        "subs %[fl], %[cin], #1\n\t"
        "asrs %[out], %[val], %[sh]\n\t"
        "mrs  %[fl], apsr\n\t"
        : [out] "=&r"(out), [fl] "=&r"(fl)
        : [val] "r"(val), [sh] "r"(sh), [cin] "r"(c_in)
        : "cc"
    );
    *out_c = (fl >> 29) & 1U;
    return out;
}

static uint32_t __attribute__((noinline))
arm_rors_reg_c(uint32_t val, uint32_t sh, uint32_t c_in, uint32_t *out_c)
{
    uint32_t out = 0U;
    uint32_t fl  = 0U;
    __asm__ volatile(
        "subs %[fl], %[cin], #1\n\t"
        "rors %[out], %[val], %[sh]\n\t"
        "mrs  %[fl], apsr\n\t"
        : [out] "=&r"(out), [fl] "=&r"(fl)
        : [val] "r"(val), [sh] "r"(sh), [cin] "r"(c_in)
        : "cc"
    );
    *out_c = (fl >> 29) & 1U;
    return out;
}

#if defined(__thumb__)
static uint32_t __attribute__((noinline))
t16_lsls_reg_c(uint32_t val, uint32_t sh, uint32_t c_in, uint32_t *out_c)
{
    uint32_t io = val;
    uint32_t fl = 0U;
    __asm__ volatile(
        "subs %[fl], %[cin], #1\n\t"
        "lsls %[io], %[sh]\n\t"
        "mrs  %[fl], apsr\n\t"
        : [io] "+l"(io), [fl] "=&l"(fl)
        : [sh] "l"(sh), [cin] "l"(c_in)
        : "cc"
    );
    *out_c = (fl >> 29) & 1U;
    return io;
}

static uint32_t __attribute__((noinline))
t16_lsrs_reg_c(uint32_t val, uint32_t sh, uint32_t c_in, uint32_t *out_c)
{
    uint32_t io = val;
    uint32_t fl = 0U;
    __asm__ volatile(
        "subs %[fl], %[cin], #1\n\t"
        "lsrs %[io], %[sh]\n\t"
        "mrs  %[fl], apsr\n\t"
        : [io] "+l"(io), [fl] "=&l"(fl)
        : [sh] "l"(sh), [cin] "l"(c_in)
        : "cc"
    );
    *out_c = (fl >> 29) & 1U;
    return io;
}

static uint32_t __attribute__((noinline))
t16_asrs_reg_c(uint32_t val, uint32_t sh, uint32_t c_in, uint32_t *out_c)
{
    uint32_t io = val;
    uint32_t fl = 0U;
    __asm__ volatile(
        "subs %[fl], %[cin], #1\n\t"
        "asrs %[io], %[sh]\n\t"
        "mrs  %[fl], apsr\n\t"
        : [io] "+l"(io), [fl] "=&l"(fl)
        : [sh] "l"(sh), [cin] "l"(c_in)
        : "cc"
    );
    *out_c = (fl >> 29) & 1U;
    return io;
}

static uint32_t __attribute__((noinline))
t16_rors_reg_c(uint32_t val, uint32_t sh, uint32_t c_in, uint32_t *out_c)
{
    uint32_t io = val;
    uint32_t fl = 0U;
    __asm__ volatile(
        "subs %[fl], %[cin], #1\n\t"
        "rors %[io], %[sh]\n\t"
        "mrs  %[fl], apsr\n\t"
        : [io] "+l"(io), [fl] "=&l"(fl)
        : [sh] "l"(sh), [cin] "l"(c_in)
        : "cc"
    );
    *out_c = (fl >> 29) & 1U;
    return io;
}
#endif


int run_test_m1_part2(void)
{
    /*
     * 14. Register-specified shifts with Rs[7:0] >= 32 (LSL, LSR, ASR):
     * Test shift amounts 0, 31, 32, 33, 64, 255 (and 256 where Rs[7:0] == 0).
     */
    {
        uint32_t val = 0x87654321U;
        if (arm_lsl_reg(val, 0U) != val ||
            arm_lsl_reg(val, 31U) != 0x80000000U ||
            arm_lsl_reg(val, 32U) != 0U ||
            arm_lsl_reg(val, 33U) != 0U ||
            arm_lsl_reg(val, 64U) != 0U ||
            arm_lsl_reg(val, 255U) != 0U ||
            arm_lsl_reg(val, 256U) != val) {
            return 67;
        }
        if (arm_lsr_reg(val, 0U) != val ||
            arm_lsr_reg(val, 31U) != 1U ||
            arm_lsr_reg(val, 32U) != 0U ||
            arm_lsr_reg(val, 33U) != 0U ||
            arm_lsr_reg(val, 64U) != 0U ||
            arm_lsr_reg(val, 255U) != 0U ||
            arm_lsr_reg(val, 256U) != val) {
            return 68;
        }
        uint32_t neg_val = 0x80001234U;
        uint32_t pos_val = 0x7FFF1234U;
        if (arm_asr_reg(neg_val, 0U) != neg_val ||
            arm_asr_reg(neg_val, 31U) != 0xFFFFFFFFU ||
            arm_asr_reg(neg_val, 32U) != 0xFFFFFFFFU ||
            arm_asr_reg(neg_val, 33U) != 0xFFFFFFFFU ||
            arm_asr_reg(neg_val, 64U) != 0xFFFFFFFFU ||
            arm_asr_reg(neg_val, 255U) != 0xFFFFFFFFU ||
            arm_asr_reg(neg_val, 256U) != neg_val) {
            return 69;
        }
        if (arm_asr_reg(pos_val, 0U) != pos_val ||
            arm_asr_reg(pos_val, 31U) != 0U ||
            arm_asr_reg(pos_val, 32U) != 0U ||
            arm_asr_reg(pos_val, 33U) != 0U ||
            arm_asr_reg(pos_val, 64U) != 0U ||
            arm_asr_reg(pos_val, 255U) != 0U ||
            arm_asr_reg(pos_val, 256U) != pos_val) {
            return 70;
        }
    }

#if !defined(__thumb__)
    /*
     * 15. 64-bit multiply with flags (UMULLS, SMULLS) in A32:
     * Verify Z=1 on 0 64-bit result, Z=0 when only RdLo is 0, and N=1 on negative 64-bit result.
     */
    {
        uint32_t rlo = 0U, rhi = 0U, flags = 0U;
        uint32_t c1 = 10U, c2 = 20U;

        /* 15a: UMULLS 0 * 0x12345678 = 0 -> Z=1, N=0 */
        uint32_t u_zero = 0U, u_val = 0x12345678U;
        __asm__ volatile(
            "cmp %[c1], %[c2]\n\t"
            "umulls %[lo], %[hi], %[a], %[b]\n\t"
            "mrs %[fl], apsr\n\t"
            : [lo] "=&r"(rlo), [hi] "=&r"(rhi), [fl] "=&r"(flags)
            : [c1] "r"(c1), [c2] "r"(c2), [a] "r"(u_zero), [b] "r"(u_val)
            : "cc"
        );
        if (rlo != 0U || rhi != 0U || (flags & 0xC0000000U) != 0x40000000U) {
            return 71;
        }

        /* 15b: UMULLS 0x80000000 * 2 = 0x00000001_00000000 (lo=0, hi=1) -> Z=0, N=0 */
        uint32_t u_half = 0x80000000U, u_two = 2U;
        __asm__ volatile(
            "cmp %[c1], %[c1]\n\t"
            "umulls %[lo], %[hi], %[a], %[b]\n\t"
            "mrs %[fl], apsr\n\t"
            : [lo] "=&r"(rlo), [hi] "=&r"(rhi), [fl] "=&r"(flags)
            : [c1] "r"(c1), [a] "r"(u_half), [b] "r"(u_two)
            : "cc"
        );
        if (rlo != 0U || rhi != 1U || (flags & 0xC0000000U) != 0x00000000U) {
            return 72;
        }

        /* 15c: UMULLS 0xFFFFFFFF * 0xFFFFFFFF = 0xFFFFFFFE_00000001 -> N=1, Z=0 */
        uint32_t u_max = 0xFFFFFFFFU;
        __asm__ volatile(
            "cmp %[c1], %[c1]\n\t"
            "umulls %[lo], %[hi], %[a], %[b]\n\t"
            "mrs %[fl], apsr\n\t"
            : [lo] "=&r"(rlo), [hi] "=&r"(rhi), [fl] "=&r"(flags)
            : [c1] "r"(c1), [a] "r"(u_max), [b] "r"(u_max)
            : "cc"
        );
        if (rlo != 1U || rhi != 0xFFFFFFFEU || (flags & 0xC0000000U) != 0x80000000U) {
            return 73;
        }

        /* 15d: SMULLS 0 * -42 = 0 -> Z=1, N=0 */
        int32_t s_zero = 0, s_neg = -42;
        __asm__ volatile(
            "cmp %[c1], %[c2]\n\t"
            "smulls %[lo], %[hi], %[a], %[b]\n\t"
            "mrs %[fl], apsr\n\t"
            : [lo] "=&r"(rlo), [hi] "=&r"(rhi), [fl] "=&r"(flags)
            : [c1] "r"(c1), [c2] "r"(c2), [a] "r"(s_zero), [b] "r"(s_neg)
            : "cc"
        );
        if (rlo != 0U || rhi != 0U || (flags & 0xC0000000U) != 0x40000000U) {
            return 74;
        }

        /* 15e: SMULLS -3 * 5 = -15 (0xFFFFFFFF_FFFFFFF1) -> N=1, Z=0 */
        int32_t s_m3 = -3, s_p5 = 5;
        __asm__ volatile(
            "cmp %[c1], %[c1]\n\t"
            "smulls %[lo], %[hi], %[a], %[b]\n\t"
            "mrs %[fl], apsr\n\t"
            : [lo] "=&r"(rlo), [hi] "=&r"(rhi), [fl] "=&r"(flags)
            : [c1] "r"(c1), [a] "r"(s_m3), [b] "r"(s_p5)
            : "cc"
        );
        if (rlo != 0xFFFFFFF1U || rhi != 0xFFFFFFFFU || (flags & 0xC0000000U) != 0x80000000U) {
            return 75;
        }

        /* 15f: SMULLS -3 * -5 = +15 -> N=0, Z=0 */
        int32_t s_m5 = -5;
        __asm__ volatile(
            "cmp %[c1], %[c2]\n\t"
            "smulls %[lo], %[hi], %[a], %[b]\n\t"
            "mrs %[fl], apsr\n\t"
            : [lo] "=&r"(rlo), [hi] "=&r"(rhi), [fl] "=&r"(flags)
            : [c1] "r"(c1), [c2] "r"(c2), [a] "r"(s_m3), [b] "r"(s_m5)
            : "cc"
        );
        if (rlo != 15U || rhi != 0U || (flags & 0xC0000000U) != 0x00000000U) {
            return 76;
        }
    }
#endif

    /*
     * 16. LDRH / LDRSB / LDRSH post-index & pre-index writeback aliasing (rn == rt):
     * Verify loaded value in Rt is not overwritten by base register writeback when Rn == Rt.
     * Note: raw instruction encodings (.word for A32, .hword for T32) are used for the
     * Rn == Rt writeback cases because assemblers reject Rn == Rt writeback mnemonics.
     */
    {
        uint16_t hbuf[4] = { 0xBEEFU, 0xCAFEU, 0x1234U, 0x8001U };
        int8_t   bbuf[4] = { -42, -100, 55, -1 };

        /* 16a: LDRH post-index r0, [r0], #2 */
        {
            register uint32_t r0 __asm__("r0") = (uint32_t)(uintptr_t)&hbuf[0];
#if defined(__thumb__)
            __asm__ volatile(".hword 0xF830, 0x0B02" : "+r"(r0) : : "memory");
#else
            __asm__ volatile(".word 0xE0D000B2" : "+r"(r0) : : "memory");
#endif
            if (r0 != 0x0000BEEFU) {
                return 77;
            }
        }

        /* 16b: LDRH pre-index r0, [r0, #2]! */
        {
            register uint32_t r0 __asm__("r0") = (uint32_t)(uintptr_t)&hbuf[0];
#if defined(__thumb__)
            __asm__ volatile(".hword 0xF830, 0x0F02" : "+r"(r0) : : "memory");
#else
            __asm__ volatile(".word 0xE1F000B2" : "+r"(r0) : : "memory");
#endif
            if (r0 != 0x0000CAFEU) {
                return 78;
            }
        }

        /* 16c: LDRSB post-index r0, [r0], #1 */
        {
            register uint32_t r0 __asm__("r0") = (uint32_t)(uintptr_t)&bbuf[0];
#if defined(__thumb__)
            __asm__ volatile(".hword 0xF910, 0x0B01" : "+r"(r0) : : "memory");
#else
            __asm__ volatile(".word 0xE0D000D1" : "+r"(r0) : : "memory");
#endif
            if (r0 != (uint32_t)(int32_t)(-42)) {
                return 79;
            }
        }

        /* 16d: LDRSB pre-index r0, [r0, #1]! */
        {
            register uint32_t r0 __asm__("r0") = (uint32_t)(uintptr_t)&bbuf[0];
#if defined(__thumb__)
            __asm__ volatile(".hword 0xF910, 0x0F01" : "+r"(r0) : : "memory");
#else
            __asm__ volatile(".word 0xE1F000D1" : "+r"(r0) : : "memory");
#endif
            if (r0 != (uint32_t)(int32_t)(-100)) {
                return 80;
            }
        }

        /* 16e: LDRSH post-index r0, [r0], #2 */
        {
            register uint32_t r0 __asm__("r0") = (uint32_t)(uintptr_t)&hbuf[0];
#if defined(__thumb__)
            __asm__ volatile(".hword 0xF930, 0x0B02" : "+r"(r0) : : "memory");
#else
            __asm__ volatile(".word 0xE0D000F2" : "+r"(r0) : : "memory");
#endif
            if (r0 != 0xFFFFBEEFU) {
                return 81;
            }
        }

        /* 16f: LDRSH pre-index r0, [r0, #2]! */
        {
            register uint32_t r0 __asm__("r0") = (uint32_t)(uintptr_t)&hbuf[0];
#if defined(__thumb__)
            __asm__ volatile(".hword 0xF930, 0x0F02" : "+r"(r0) : : "memory");
#else
            __asm__ volatile(".word 0xE1F000F2" : "+r"(r0) : : "memory");
#endif
            if (r0 != 0xFFFFCAFEU) {
                return 82;
            }
        }

        /* 16g: Positive check when rn != rt: verify both loaded value and base writeback */
        {
            const uint16_t *p = &hbuf[0];
            uint32_t val_h = 0U;
            __asm__ volatile("ldrh %[v], [%[p]], #2"
                             : [v] "=&r"(val_h), [p] "+r"(p)
                             :
                             : "memory");
            if (val_h != 0x0000BEEFU || p != &hbuf[1]) {
                return 83;
            }
            __asm__ volatile("ldrsh %[v], [%[p], #2]!"
                             : [v] "=&r"(val_h), [p] "+r"(p)
                             :
                             : "memory");
            if (val_h != 0x00001234U || p != &hbuf[2]) {
                return 84;
            }
        }
    }

    /*
     * 17. UXTAB / UXTAH / SXTAB / SXTAH when Rd == Rn (and Rd == Rm):
     * Regression test verifying Rd is not overwritten before adding Rn when Rd == Rn.
     */
    {
        /* 17a: uxtab r0, r0, r1 in a 4-iteration loop */
        uint32_t acc = 1000U;
        uint32_t addend_u8 = 0x12345680U; /* low byte 0x80 = 128 */
        for (int i = 0; i < 4; i++) {
            register uint32_t r0 __asm__("r0") = acc;
            register uint32_t r1 __asm__("r1") = addend_u8;
            __asm__ volatile("uxtab r0, r0, r1" : "+r"(r0) : "r"(r1));
            acc = r0;
        }
        if (acc != 1512U) {
            return 85;
        }

        /* 17b: sxtah r0, r0, r1 in a 4-iteration loop */
        acc = 2000U;
        uint32_t addend_s16 = 0x1234FFCEU; /* low halfword 0xFFCE = -50 */
        for (int i = 0; i < 4; i++) {
            register uint32_t r0 __asm__("r0") = acc;
            register uint32_t r1 __asm__("r1") = addend_s16;
            __asm__ volatile("sxtah r0, r0, r1" : "+r"(r0) : "r"(r1));
            acc = r0;
        }
        if (acc != 1800U) {
            return 86;
        }

        /* 17c: uxtah r0, r0, r1, ror #8 and sxtab r0, r0, r1, ror #8 when Rd == Rn */
        {
            register uint32_t r0 __asm__("r0") = 5000U;
            register uint32_t r1 __asm__("r1") = 0x11223344U; /* (r1 ror 8)[15:0] = 0x2233 = 8755 */
            __asm__ volatile("uxtah r0, r0, r1, ror #8" : "+r"(r0) : "r"(r1));
            if (r0 != 13755U) {
                return 87;
            }
        }
        {
            register uint32_t r0 __asm__("r0") = 500U;
            register uint32_t r1 __asm__("r1") = 0x1122CE44U; /* (r1 ror 8)[7:0] = 0xCE = -50 */
            __asm__ volatile("sxtab r0, r0, r1, ror #8" : "+r"(r0) : "r"(r1));
            if (r0 != 450U) {
                return 88;
            }
        }
    }

    /*
     * 18. SMULWB / SMULWT and SMLAWB / SMLAWT with distinct top/bottom halfwords in Rm:
     * Verify B selects Rm[15:0] and T selects Rm[31:16].
     */
    {
        int32_t  rn32  = 0x00030000;      /* 3 * 65536 */
        uint32_t rm_bt = 0xFFF90005U;     /* top = -7 (0xFFF9), bottom = +5 (0x0005) */
        int32_t  ra32  = 100;
        int32_t  out_wb = 0, out_wt = 0;

        __asm__ volatile("smulwb %0, %1, %2" : "=r"(out_wb) : "r"(rn32), "r"(rm_bt));
        __asm__ volatile("smulwt %0, %1, %2" : "=r"(out_wt) : "r"(rn32), "r"(rm_bt));
        if (out_wb != 15 || out_wt != -21) {
            return 89;
        }

        __asm__ volatile("smlawb %0, %1, %2, %3" : "=r"(out_wb) : "r"(rn32), "r"(rm_bt), "r"(ra32));
        __asm__ volatile("smlawt %0, %1, %2, %3" : "=r"(out_wt) : "r"(rn32), "r"(rm_bt), "r"(ra32));
        if (out_wb != 115 || out_wt != 79) {
            return 90;
        }

        int32_t  rn_neg = -0x00028000;    /* -2.5 * 65536 = -163840 */
        uint32_t rm_bt2 = 0x0006FFFCU;    /* top = +6, bottom = -4 */
        int32_t  ra_neg = -50;
        __asm__ volatile("smulwb %0, %1, %2" : "=r"(out_wb) : "r"(rn_neg), "r"(rm_bt2));
        __asm__ volatile("smulwt %0, %1, %2" : "=r"(out_wt) : "r"(rn_neg), "r"(rm_bt2));
        if (out_wb != 10 || out_wt != -15) {
            return 91;
        }

        __asm__ volatile("smlawb %0, %1, %2, %3" : "=r"(out_wb) : "r"(rn_neg), "r"(rm_bt2), "r"(ra_neg));
        __asm__ volatile("smlawt %0, %1, %2, %3" : "=r"(out_wt) : "r"(rn_neg), "r"(rm_bt2), "r"(ra_neg));
        if (out_wb != -40 || out_wt != -65) {
            return 92;
        }
    }

    /*
     * 19. Extended multiply, dual-halfword, dual-extend, SAD, and saturating arithmetic:
     * SMMUL, SMMULR, SMMLA, SMMLS, UMAAL, SMUAD, SMUSD, SMLAD, SMLSD, SMLALD,
     * UXTB16, UXTAB16, SXTB16, SXTAB16, USAD8, USADA8, QADD, QSUB, QDADD, QDSUB.
     */
    {
        /* 19a: SMMUL, SMMULR, SMMLA, SMMLS */
        int32_t sm_a = 0x40000000; /* 2^30 */
        int32_t sm_b = 6;          /* product = 0x1_80000000 (bit 31 = 1) */
        int32_t sm_neg = -0x40000000;
        int32_t sm_acc = 100;
        int32_t sm_res = 0, sm_res_r = 0;

        __asm__ volatile("smmul  %0, %1, %2" : "=r"(sm_res)   : "r"(sm_a), "r"(sm_b));
        __asm__ volatile("smmulr %0, %1, %2" : "=r"(sm_res_r) : "r"(sm_a), "r"(sm_b));
        if (sm_res != 1 || sm_res_r != 2) {
            return 93;
        }

        __asm__ volatile("smmul  %0, %1, %2" : "=r"(sm_res)   : "r"(sm_neg), "r"(sm_b));
        __asm__ volatile("smmulr %0, %1, %2" : "=r"(sm_res_r) : "r"(sm_neg), "r"(sm_b));
        if (sm_res != -2 || sm_res_r != -1) {
            return 94;
        }

        __asm__ volatile("smmla %0, %1, %2, %3" : "=r"(sm_res) : "r"(sm_a), "r"(sm_b), "r"(sm_acc));
        if (sm_res != 101) {
            return 95;
        }
        __asm__ volatile("smmla %0, %1, %2, %3" : "=r"(sm_res) : "r"(sm_neg), "r"(sm_b), "r"(sm_acc));
        if (sm_res != 98) {
            return 96;
        }

        int32_t sm_b12 = 12; /* 12 * 2^30 = 3 * 2^32 */
        int32_t sm_ra50 = 50;
        __asm__ volatile("smmls %0, %1, %2, %3" : "=r"(sm_res) : "r"(sm_a), "r"(sm_b12), "r"(sm_ra50));
        if (sm_res != 47) {
            return 97;
        }
        __asm__ volatile("smmls %0, %1, %2, %3" : "=r"(sm_res) : "r"(sm_neg), "r"(sm_b12), "r"(sm_ra50));
        if (sm_res != 53) {
            return 98;
        }

        /* 19b: UMAAL */
        {
            register uint32_t r0 __asm__("r0") = 100U;
            register uint32_t r1 __asm__("r1") = 200U;
            register uint32_t r2 __asm__("r2") = 0x80000000U;
            register uint32_t r3 __asm__("r3") = 4U;
            __asm__ volatile("umaal r0, r1, r2, r3" : "+r"(r0), "+r"(r1) : "r"(r2), "r"(r3));
            if (r0 != 300U || r1 != 2U) {
                return 99;
            }
        }
        {
            register uint32_t r0 __asm__("r0") = 0xFFFFFFFFU;
            register uint32_t r1 __asm__("r1") = 0xFFFFFFFFU;
            register uint32_t r2 __asm__("r2") = 0xFFFFFFFFU;
            register uint32_t r3 __asm__("r3") = 0xFFFFFFFFU;
            __asm__ volatile("umaal r0, r1, r2, r3" : "+r"(r0), "+r"(r1) : "r"(r2), "r"(r3));
            if (r0 != 0xFFFFFFFFU || r1 != 0xFFFFFFFFU) {
                return 100;
            }
        }

        /* 19c: SMUAD, SMUSD, SMLAD, SMLSD, SMLALD */
        uint32_t d_rn = ((uint32_t)(uint16_t)(-3) << 16) | 4U; /* top = -3, bot = +4 */
        uint32_t d_rm = ((uint32_t)(uint16_t)5 << 16) | 6U;    /* top = +5, bot = +6 */
        int32_t  d_ra = 100;
        int32_t  d_out = 0;

        /* p1 = 4*6 = 24, p2 = -3*5 = -15 -> p1 + p2 = 9, p1 - p2 = 39 */
        __asm__ volatile("smuad %0, %1, %2" : "=r"(d_out) : "r"(d_rn), "r"(d_rm));
        if (d_out != 9) {
            return 101;
        }
        __asm__ volatile("smusd %0, %1, %2" : "=r"(d_out) : "r"(d_rn), "r"(d_rm));
        if (d_out != 39) {
            return 102;
        }
        __asm__ volatile("smlad %0, %1, %2, %3" : "=r"(d_out) : "r"(d_rn), "r"(d_rm), "r"(d_ra));
        if (d_out != 109) {
            return 103;
        }
        __asm__ volatile("smlsd %0, %1, %2, %3" : "=r"(d_out) : "r"(d_rn), "r"(d_rm), "r"(d_ra));
        if (d_out != 139) {
            return 104;
        }
        {
            register uint32_t r0 __asm__("r0") = 0xFFFFFFFCU;
            register uint32_t r1 __asm__("r1") = 0U;
            register uint32_t r2 __asm__("r2") = d_rn;
            register uint32_t r3 __asm__("r3") = d_rm;
            __asm__ volatile("smlald r0, r1, r2, r3" : "+r"(r0), "+r"(r1) : "r"(r2), "r"(r3));
            if (r0 != 5U || r1 != 1U) {
                return 105;
            }
        }

        /* 19d: UXTB16, UXTAB16, SXTB16, SXTAB16 */
        uint32_t x16_rm = 0x128456F3U;
        uint32_t x16_out = 0U;
        __asm__ volatile("uxtb16 %0, %1" : "=r"(x16_out) : "r"(x16_rm));
        if (x16_out != 0x008400F3U) {
            return 106;
        }
        __asm__ volatile("uxtb16 %0, %1, ror #8" : "=r"(x16_out) : "r"(x16_rm));
        if (x16_out != 0x00120056U) {
            return 107;
        }

        uint32_t ux16_rn = 0x0010FFFEU;
        uint32_t ux16_rm = 0x99058804U; /* [23:16]=5, [7:0]=4 -> lo wraps without carry into hi */
        __asm__ volatile("uxtab16 %0, %1, %2" : "=r"(x16_out) : "r"(ux16_rn), "r"(ux16_rm));
        if (x16_out != 0x00150002U) {
            return 108;
        }

        uint32_t sx16_rm = 0x1280567FU; /* [23:16]=0x80 (-128), [7:0]=0x7F (+127) */
        __asm__ volatile("sxtb16 %0, %1" : "=r"(x16_out) : "r"(sx16_rm));
        if (x16_out != 0xFF80007FU) {
            return 109;
        }

        uint32_t sxa16_rn = 0x00640064U; /* 100 in both halfwords */
        uint32_t sxa16_rm = 0x99F68814U; /* [23:16]=0xF6 (-10), [7:0]=0x14 (+20) */
        __asm__ volatile("sxtab16 %0, %1, %2" : "=r"(x16_out) : "r"(sxa16_rn), "r"(sxa16_rm));
        if (x16_out != 0x005A0078U) {
            return 110;
        }

        /* 19e: USAD8 and USADA8 */
        uint32_t sad_a = 0x1050A0FFU; /* 16, 80, 160, 255 */
        uint32_t sad_b = 0x2030B0F0U; /* 32, 48, 176, 240 -> |16-32|+|80-48|+|160-176|+|255-240| = 79 */
        uint32_t sad_acc = 1000U;
        uint32_t sad_out = 0U;
        __asm__ volatile("usad8  %0, %1, %2" : "=&r"(sad_out) : "r"(sad_a), "r"(sad_b));
        if (sad_out != 79U) {
            return 111;
        }
        __asm__ volatile("usada8 %0, %1, %2, %3" : "=&r"(sad_out) : "r"(sad_a), "r"(sad_b), "r"(sad_acc));
        if (sad_out != 1079U) {
            return 112;
        }

        /* 19f: QADD, QSUB, QDADD, QDSUB (both non-saturating and saturating) */
        int32_t q_out = 0;
        int32_t q_100 = 100, q_200 = 200, q_300 = 300, q_50 = 50, q_m100 = -100;
        int32_t q_pos_near = 0x7FFFFFF0;
        int32_t q_neg_near = (int32_t)0x80000010U;
        int32_t q_big_pos  = 0x50000000;

        __asm__ volatile("qadd %0, %1, %2" : "=r"(q_out) : "r"(q_100), "r"(q_200));
        if (q_out != 300) {
            return 113;
        }
        __asm__ volatile("qadd %0, %1, %2" : "=r"(q_out) : "r"(q_pos_near), "r"(q_100));
        if (q_out != (int32_t)0x7FFFFFFFU) {
            return 114;
        }
        __asm__ volatile("qadd %0, %1, %2" : "=r"(q_out) : "r"(q_neg_near), "r"(q_m100));
        if (q_out != (int32_t)0x80000000U) {
            return 115;
        }

        __asm__ volatile("qsub %0, %1, %2" : "=r"(q_out) : "r"(q_300), "r"(q_100));
        if (q_out != 200) {
            return 116;
        }
        __asm__ volatile("qsub %0, %1, %2" : "=r"(q_out) : "r"(q_pos_near), "r"(q_m100));
        if (q_out != (int32_t)0x7FFFFFFFU) {
            return 117;
        }
        __asm__ volatile("qsub %0, %1, %2" : "=r"(q_out) : "r"(q_neg_near), "r"(q_100));
        if (q_out != (int32_t)0x80000000U) {
            return 118;
        }

        __asm__ volatile("qdadd %0, %1, %2" : "=r"(q_out) : "r"(q_100), "r"(q_50));
        if (q_out != 200) {
            return 119;
        }
        __asm__ volatile("qdadd %0, %1, %2" : "=r"(q_out) : "r"(q_100), "r"(q_big_pos));
        if (q_out != (int32_t)0x7FFFFFFFU) {
            return 120;
        }

        __asm__ volatile("qdsub %0, %1, %2" : "=r"(q_out) : "r"(q_300), "r"(q_50));
        if (q_out != 200) {
            return 121;
        }
        int32_t q_zero = 0;
        __asm__ volatile("qdsub %0, %1, %2" : "=r"(q_out) : "r"(q_zero), "r"(q_big_pos));
        if (q_out != (int32_t)0x80000001U) {
            return 122;
        }
    }

#if !defined(__thumb__)
    /*
     * 20. Regression test for A32 post-indexed register load when Rt == Rm:
     * Verify `ldr r0, [r1], r0` and `ldrh r0, [r1], r0` compute base writeback
     * using the original offset in r0 rather than the newly loaded value.
     */
    {
        uint32_t warr[4] = { 0xCAFEBABEU, 0x11223344U, 0x55667788U, 0x99AABBCCU };
        {
            register uint32_t r0 __asm__("r0") = 8U; /* 2 words */
            register uint32_t r1 __asm__("r1") = (uint32_t)(uintptr_t)&warr[0];
            /* A32: ldr r0, [r1], r0 */
            __asm__ volatile(".word 0xE6910000" : "+r"(r0), "+r"(r1) : : "memory");
            if (r0 != 0xCAFEBABEU || r1 != (uint32_t)(uintptr_t)&warr[2]) {
                return 123;
            }
        }

        uint16_t harr[4] = { 0xBEEFU, 0x1111U, 0x2222U, 0x3333U };
        {
            register uint32_t r0 __asm__("r0") = 4U; /* 2 halfwords */
            register uint32_t r1 __asm__("r1") = (uint32_t)(uintptr_t)&harr[0];
            /* A32: ldrh r0, [r1], r0 */
            __asm__ volatile(".word 0xE09100B0" : "+r"(r0), "+r"(r1) : : "memory");
            if (r0 != 0x0000BEEFU || r1 != (uint32_t)(uintptr_t)&harr[2]) {
                return 124;
            }
        }
    }
#endif

#if defined(__thumb__)
    /*
     * 21. Regression test for Thumb-2 `ADD.W pc, pc, r0, lsl #2` jump table:
     * Verify writing an even address to PC via `add.w pc, pc, r0, lsl #2` stays
     * in Thumb state and does not interwork to A32 mode.
     */
    {
        static const uint32_t expected_jt[3] = { 11U, 22U, 33U };
        for (uint32_t idx = 0U; idx < 3U; idx++) {
            uint32_t jt_out = 0U;
            register uint32_t r0 __asm__("r0") = idx;
            __asm__ volatile(
                ".balign 4\n\t"
                ".inst.w 0xEB0F0F80\n\t"
                "b.w 1f\n\t"
                "b.w 2f\n\t"
                "b.w 3f\n"
                "1:\n\t"
                "movs %[out], #11\n\t"
                "b.w 4f\n"
                "2:\n\t"
                "movs %[out], #22\n\t"
                "b.w 4f\n"
                "3:\n\t"
                "movs %[out], #33\n"
                "4:\n\t"
                : [out] "=&r"(jt_out)
                : "r"(r0)
                : "cc"
            );
            if (jt_out != expected_jt[idx]) {
                return 125;
            }
        }
    }
#endif

    /*
     * 22. Barrel-shifter carry-out (C flag) on register-specified shifts
     *     (LSLS, LSRS, ASRS, RORS by register) for amount == 0 (preserving C),
     *     amount == 1..31, amount == 32, and amount > 32.
     */
    {
        uint32_t c_out = 0U;

        /* 22a: LSLS by register */
        if (arm_lsls_reg_c(0x12345678U, 0U, 1U, &c_out) != 0x12345678U || c_out != 1U ||
            arm_lsls_reg_c(0x12345678U, 0U, 0U, &c_out) != 0x12345678U || c_out != 0U ||
            arm_lsls_reg_c(0x12345678U, 256U, 1U, &c_out) != 0x12345678U || c_out != 1U ||
            arm_lsls_reg_c(0x12345678U, 256U, 0U, &c_out) != 0x12345678U || c_out != 0U ||
            arm_lsls_reg_c(0x40000001U, 1U, 1U, &c_out) != 0x80000002U || c_out != 0U ||
            arm_lsls_reg_c(0x40000001U, 2U, 0U, &c_out) != 0x00000004U || c_out != 1U ||
            arm_lsls_reg_c(0x00000003U, 31U, 0U, &c_out) != 0x80000000U || c_out != 1U ||
            arm_lsls_reg_c(0x00000001U, 31U, 1U, &c_out) != 0x80000000U || c_out != 0U ||
            arm_lsls_reg_c(0x12345671U, 32U, 0U, &c_out) != 0U || c_out != 1U ||
            arm_lsls_reg_c(0x12345670U, 32U, 1U, &c_out) != 0U || c_out != 0U ||
            arm_lsls_reg_c(0xFFFFFFFFU, 33U, 1U, &c_out) != 0U || c_out != 0U ||
            arm_lsls_reg_c(0xFFFFFFFFU, 64U, 1U, &c_out) != 0U || c_out != 0U) {
            return 126;
        }

        /* 22b: LSRS by register */
        if (arm_lsrs_reg_c(0x87654321U, 0U, 1U, &c_out) != 0x87654321U || c_out != 1U ||
            arm_lsrs_reg_c(0x87654321U, 0U, 0U, &c_out) != 0x87654321U || c_out != 0U ||
            arm_lsrs_reg_c(0x87654321U, 256U, 1U, &c_out) != 0x87654321U || c_out != 1U ||
            arm_lsrs_reg_c(0x87654321U, 256U, 0U, &c_out) != 0x87654321U || c_out != 0U ||
            arm_lsrs_reg_c(0x80000002U, 1U, 1U, &c_out) != 0x40000001U || c_out != 0U ||
            arm_lsrs_reg_c(0x80000002U, 2U, 0U, &c_out) != 0x20000000U || c_out != 1U ||
            arm_lsrs_reg_c(0x40000000U, 31U, 0U, &c_out) != 0U || c_out != 1U ||
            arm_lsrs_reg_c(0x80000000U, 31U, 1U, &c_out) != 1U || c_out != 0U ||
            arm_lsrs_reg_c(0x80000000U, 32U, 0U, &c_out) != 0U || c_out != 1U ||
            arm_lsrs_reg_c(0x7FFFFFFFU, 32U, 1U, &c_out) != 0U || c_out != 0U ||
            arm_lsrs_reg_c(0xFFFFFFFFU, 33U, 1U, &c_out) != 0U || c_out != 0U ||
            arm_lsrs_reg_c(0xFFFFFFFFU, 64U, 1U, &c_out) != 0U || c_out != 0U) {
            return 127;
        }

        /* 22c: ASRS by register */
        if (arm_asrs_reg_c(0x80000002U, 0U, 1U, &c_out) != 0x80000002U || c_out != 1U ||
            arm_asrs_reg_c(0x80000002U, 0U, 0U, &c_out) != 0x80000002U || c_out != 0U ||
            arm_asrs_reg_c(0x80000002U, 256U, 1U, &c_out) != 0x80000002U || c_out != 1U ||
            arm_asrs_reg_c(0x80000002U, 256U, 0U, &c_out) != 0x80000002U || c_out != 0U ||
            arm_asrs_reg_c(0x80000002U, 1U, 1U, &c_out) != 0xC0000001U || c_out != 0U ||
            arm_asrs_reg_c(0x80000002U, 2U, 0U, &c_out) != 0xE0000000U || c_out != 1U ||
            arm_asrs_reg_c(0x40000000U, 31U, 0U, &c_out) != 0U || c_out != 1U ||
            arm_asrs_reg_c(0x80000000U, 32U, 0U, &c_out) != 0xFFFFFFFFU || c_out != 1U ||
            arm_asrs_reg_c(0x80000000U, 33U, 0U, &c_out) != 0xFFFFFFFFU || c_out != 1U ||
            arm_asrs_reg_c(0x80000000U, 64U, 0U, &c_out) != 0xFFFFFFFFU || c_out != 1U ||
            arm_asrs_reg_c(0x7FFFFFFFU, 32U, 1U, &c_out) != 0U || c_out != 0U ||
            arm_asrs_reg_c(0x7FFFFFFFU, 33U, 1U, &c_out) != 0U || c_out != 0U) {
            return 128;
        }

        /* 22d: RORS by register */
        if (arm_rors_reg_c(0x80000001U, 0U, 1U, &c_out) != 0x80000001U || c_out != 1U ||
            arm_rors_reg_c(0x80000001U, 0U, 0U, &c_out) != 0x80000001U || c_out != 0U ||
            arm_rors_reg_c(0x80000001U, 256U, 1U, &c_out) != 0x80000001U || c_out != 1U ||
            arm_rors_reg_c(0x80000001U, 256U, 0U, &c_out) != 0x80000001U || c_out != 0U ||
            arm_rors_reg_c(0x00000001U, 1U, 0U, &c_out) != 0x80000000U || c_out != 1U ||
            arm_rors_reg_c(0x00000002U, 1U, 1U, &c_out) != 0x00000001U || c_out != 0U ||
            arm_rors_reg_c(0x12345678U, 8U, 1U, &c_out) != 0x78123456U || c_out != 0U ||
            arm_rors_reg_c(0x12345680U, 8U, 0U, &c_out) != 0x80123456U || c_out != 1U ||
            arm_rors_reg_c(0x80000001U, 32U, 0U, &c_out) != 0x80000001U || c_out != 1U ||
            arm_rors_reg_c(0x7FFFFFFFU, 32U, 1U, &c_out) != 0x7FFFFFFFU || c_out != 0U ||
            arm_rors_reg_c(0x00000001U, 33U, 0U, &c_out) != 0x80000000U || c_out != 1U ||
            arm_rors_reg_c(0x00000002U, 33U, 1U, &c_out) != 0x00000001U || c_out != 0U ||
            arm_rors_reg_c(0x80000001U, 64U, 0U, &c_out) != 0x80000001U || c_out != 1U ||
            arm_rors_reg_c(0x7FFFFFFFU, 64U, 1U, &c_out) != 0x7FFFFFFFU || c_out != 0U) {
            return 129;
        }

#if defined(__thumb__)
        /* 22e: T16 2-operand LSLS / LSRS / ASRS / RORS by register */
        if (t16_lsls_reg_c(0x12345671U, 0U, 1U, &c_out) != 0x12345671U || c_out != 1U ||
            t16_lsls_reg_c(0x12345671U, 0U, 0U, &c_out) != 0x12345671U || c_out != 0U ||
            t16_lsls_reg_c(0x40000001U, 2U, 0U, &c_out) != 0x00000004U || c_out != 1U ||
            t16_lsls_reg_c(0x12345671U, 32U, 0U, &c_out) != 0U || c_out != 1U ||
            t16_lsls_reg_c(0xFFFFFFFFU, 33U, 1U, &c_out) != 0U || c_out != 0U ||
            t16_lsrs_reg_c(0x80000002U, 0U, 1U, &c_out) != 0x80000002U || c_out != 1U ||
            t16_lsrs_reg_c(0x80000002U, 2U, 0U, &c_out) != 0x20000000U || c_out != 1U ||
            t16_lsrs_reg_c(0x80000000U, 32U, 0U, &c_out) != 0U || c_out != 1U ||
            t16_lsrs_reg_c(0xFFFFFFFFU, 33U, 1U, &c_out) != 0U || c_out != 0U ||
            t16_asrs_reg_c(0x80000002U, 0U, 0U, &c_out) != 0x80000002U || c_out != 0U ||
            t16_asrs_reg_c(0x80000002U, 2U, 0U, &c_out) != 0xE0000000U || c_out != 1U ||
            t16_asrs_reg_c(0x80000000U, 32U, 0U, &c_out) != 0xFFFFFFFFU || c_out != 1U ||
            t16_asrs_reg_c(0x80000000U, 33U, 0U, &c_out) != 0xFFFFFFFFU || c_out != 1U ||
            t16_rors_reg_c(0x80000001U, 0U, 0U, &c_out) != 0x80000001U || c_out != 0U ||
            t16_rors_reg_c(0x00000001U, 1U, 0U, &c_out) != 0x80000000U || c_out != 1U ||
            t16_rors_reg_c(0x80000001U, 32U, 0U, &c_out) != 0x80000001U || c_out != 1U ||
            t16_rors_reg_c(0x00000001U, 33U, 0U, &c_out) != 0x80000000U || c_out != 1U) {
            return 130;
        }
#endif
    }

    return 0;
}
