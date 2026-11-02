/*
 * tests/test_m1_part4.c — M1 ISA & FS tests (part 4)
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_m1_common.h"

int run_test_m1_part4(void)
{
    /*
     * 29. Regression tests for CPSR.Q (bit 27) sticky overflow flag on signed
     *     multiply-accumulate instructions (Round 5):
     *     SMLABB, SMLAWB, SMUAD (0x80008000 * 0x80008000), SMLAD, SMLSD
     *     (both non-overflowing Q == 0 negative check and overflowing Q == 1
     *     positive + sticky check).
     */
    {
        const uint32_t q_bit = 1U << 27;
        uint32_t zero = 0U;
        uint32_t res = 0U;
        uint32_t flags = 0U;

        /* 29a: SMLABB (non-overflowing Q == 0 vs overflowing Q == 1 + sticky) */
        uint32_t smlabb_rn_ok = 100U, smlabb_rm_ok = 200U, smlabb_ra_ok = 300U;
        uint32_t smlabb_rn_ov = 0x00004000U; /* 16384 */
        uint32_t smlabb_rm_ov = 0x00004000U; /* 16384 -> product = 0x10000000 */
        uint32_t smlabb_ra_ov = 0x7FFFFFF0U; /* 0x10000000 + 0x7FFFFFF0 = 0x8FFFFFF0 (signed overflow) */
        __asm__ volatile(
            "msr apsr_nzcvq, %[z]\n\t"
            "smlabb %[res], %[rn], %[rm], %[ra]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [z] "r"(zero), [rn] "r"(smlabb_rn_ok),
              [rm] "r"(smlabb_rm_ok), [ra] "r"(smlabb_ra_ok)
            : "cc"
        );
        if (res != 20300U || (flags & q_bit) != 0U) {
            return 172;
        }
        __asm__ volatile(
            "msr apsr_nzcvq, %[z]\n\t"
            "smlabb %[res], %[rno], %[rmo], %[rao]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [z] "r"(zero), [rno] "r"(smlabb_rn_ov),
              [rmo] "r"(smlabb_rm_ov), [rao] "r"(smlabb_ra_ov)
            : "cc"
        );
        if (res != 0x8FFFFFF0U || (flags & q_bit) != q_bit) {
            return 173;
        }
        /* Verify Q stays sticky across non-overflowing SMLABB */
        __asm__ volatile(
            "smlabb %[res], %[rn], %[rm], %[ra]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [rn] "r"(smlabb_rn_ok), [rm] "r"(smlabb_rm_ok), [ra] "r"(smlabb_ra_ok)
            : "cc"
        );
        if (res != 20300U || (flags & q_bit) != q_bit) {
            return 174;
        }

        /* 29b: SMLAWB (non-overflowing Q == 0 vs overflowing Q == 1 + sticky) */
        uint32_t smlawb_rn_ok = 0x00020000U; /* 2 * 65536 */
        uint32_t smlawb_rm_ok = 5U;          /* (131072 * 5) >> 16 = 10 */
        uint32_t smlawb_ra_ok = 100U;
        uint32_t smlawb_rn_ov = 0x40000000U; /* 2^30 */
        uint32_t smlawb_rm_ov = 0x00004000U; /* 2^14 -> (2^30 * 2^14) >> 16 = 0x10000000 */
        uint32_t smlawb_ra_ov = 0x7FFFFFF0U;
        __asm__ volatile(
            "msr apsr_nzcvq, %[z]\n\t"
            "smlawb %[res], %[rn], %[rm], %[ra]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [z] "r"(zero), [rn] "r"(smlawb_rn_ok),
              [rm] "r"(smlawb_rm_ok), [ra] "r"(smlawb_ra_ok)
            : "cc"
        );
        if (res != 110U || (flags & q_bit) != 0U) {
            return 175;
        }
        __asm__ volatile(
            "msr apsr_nzcvq, %[z]\n\t"
            "smlawb %[res], %[rno], %[rmo], %[rao]\n\t"
            "smlawb %[res], %[rn], %[rm], %[ra]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [z] "r"(zero), [rno] "r"(smlawb_rn_ov),
              [rmo] "r"(smlawb_rm_ov), [rao] "r"(smlawb_ra_ov),
              [rn] "r"(smlawb_rn_ok), [rm] "r"(smlawb_rm_ok), [ra] "r"(smlawb_ra_ok)
            : "cc"
        );
        if (res != 110U || (flags & q_bit) != q_bit) {
            return 176;
        }

        /*
         * 29c: SMUAD (non-overflowing Q == 0 vs 0x80008000 * 0x80008000 where
         *      (-32768)*(-32768) + (-32768)*(-32768) = +2^31 overflows 32-bit
         *      signed to 0x80000000U and sets Q = 1!)
         */
        uint32_t smuad_rn_ok = 0x00030004U;
        uint32_t smuad_rm_ok = 0x00050006U;
        uint32_t smuad_min   = 0x80008000U;
        __asm__ volatile(
            "msr apsr_nzcvq, %[z]\n\t"
            "smuad %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [z] "r"(zero), [rn] "r"(smuad_rn_ok), [rm] "r"(smuad_rm_ok)
            : "cc"
        );
        if (res != 39U || (flags & q_bit) != 0U) {
            return 177;
        }
        __asm__ volatile(
            "msr apsr_nzcvq, %[z]\n\t"
            "smuad %[res], %[m], %[m]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [z] "r"(zero), [m] "r"(smuad_min)
            : "cc"
        );
        if (res != 0x80000000U || (flags & q_bit) != q_bit) {
            return 178;
        }

        /* 29d: SMLAD (non-overflowing Q == 0 vs overflowing Q == 1 + sticky) */
        uint32_t smlad_ra_ok = 100U;
        uint32_t smlad_rn_ov = 0x40004000U; /* +16384, +16384 */
        uint32_t smlad_rm_ov = 0x40004000U; /* p1 + p2 = 0x20000000 */
        uint32_t smlad_ra_ov = 0x70000000U; /* 0x20000000 + 0x70000000 = 0x90000000 (overflow) */
        __asm__ volatile(
            "msr apsr_nzcvq, %[z]\n\t"
            "smlad %[res], %[rn], %[rm], %[ra]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [z] "r"(zero), [rn] "r"(smuad_rn_ok),
              [rm] "r"(smuad_rm_ok), [ra] "r"(smlad_ra_ok)
            : "cc"
        );
        if (res != 139U || (flags & q_bit) != 0U) {
            return 179;
        }
        __asm__ volatile(
            "msr apsr_nzcvq, %[z]\n\t"
            "smlad %[res], %[rno], %[rmo], %[rao]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [z] "r"(zero), [rno] "r"(smlad_rn_ov),
              [rmo] "r"(smlad_rm_ov), [rao] "r"(smlad_ra_ov)
            : "cc"
        );
        if (res != 0x90000000U || (flags & q_bit) != q_bit) {
            return 180;
        }

        /* 29e: SMLSD (non-overflowing Q == 0 vs overflowing Q == 1 + cleanup) */
        uint32_t smlsd_rn_ov = 0xC0004000U; /* hi = -16384, lo = +16384 */
        uint32_t smlsd_rm_ov = 0x40004000U; /* p1 - p2 = 0x10000000 - (-0x10000000) = 0x20000000 */
        uint32_t smlsd_ra_ov = 0x70000000U; /* (p1 - p2) + ra = 0x90000000 (overflow) */
        __asm__ volatile(
            "msr apsr_nzcvq, %[z]\n\t"
            "smlsd %[res], %[rn], %[rm], %[ra]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [z] "r"(zero), [rn] "r"(smuad_rn_ok),
              [rm] "r"(smuad_rm_ok), [ra] "r"(smlad_ra_ok)
            : "cc"
        );
        if (res != 109U || (flags & q_bit) != 0U) {
            return 181;
        }
        __asm__ volatile(
            "msr apsr_nzcvq, %[z]\n\t"
            "smlsd %[res], %[rno], %[rmo], %[rao]\n\t"
            "mrs %[fl], apsr\n\t"
            "msr apsr_nzcvq, %[z]\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [z] "r"(zero), [rno] "r"(smlsd_rn_ov),
              [rmo] "r"(smlsd_rm_ov), [rao] "r"(smlsd_ra_ov)
            : "cc"
        );
        if (res != 0x90000000U || (flags & q_bit) != q_bit) {
            return 182;
        }
    }

    /*
     * 30. Round 6 positive, negative & regression tests for halving and
     *     saturating parallel arithmetic:
     *     - 30a: Halving parallel arithmetic (SHADD8, SHSUB8, SHADD16, SHSUB16,
     *            UHADD8, UHSUB8, UHADD16, UHSUB16), including UHSUB8/UHSUB16
     *            lanes with a < b (signed 9-bit / 17-bit difference >> 1) and
     *            verification that CPSR.NZCV, CPSR.Q, and CPSR.GE[3:0] are
     *            NOT modified.
     *     - 30b: Saturating parallel arithmetic (QADD8, QSUB8, QADD16, QSUB16,
     *            UQADD8, UQSUB8, UQADD16, UQSUB16), verifying signed/unsigned
     *            lane saturation and that CPSR.NZCV, CPSR.Q, and CPSR.GE[3:0]
     *            are NOT modified.
     */
    {
        uint32_t res = 0U;
        uint32_t flags = 0U;
        const uint32_t psr_mask = 0xF80F0000U; /* NZCV (31:28), Q (27), GE (19:16) */
        uint32_t psr_a = 0x60050000U;          /* N=0,Z=1,C=1,V=0, Q=0, GE=0b0101 */
        uint32_t psr_b = 0x980A0000U;          /* N=1,Z=0,C=0,V=1, Q=1, GE=0b1010 */

        /* 30a-1: SHADD8 */
        uint32_t sha8_rn = 0x649CFB07U; /* +100, -100, -5, +7 */
        uint32_t sha8_rm = 0x649C0204U; /* +100, -100, +2, +4 -> +100, -100, -2, +5 */
        __asm__ volatile(
            "msr apsr_nzcvqg, %[pa]\n\t"
            "shadd8 %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [pa] "r"(psr_a), [rn] "r"(sha8_rn), [rm] "r"(sha8_rm)
            : "cc"
        );
        if (res != 0x649CFE05U || (flags & psr_mask) != psr_a) {
            return 183;
        }

        /* 30a-2: SHSUB8 */
        uint32_t shs8_rn = 0x649C020AU; /* +100, -100, +2, +10 */
        uint32_t shs8_rm = 0x9C640503U; /* -100, +100, +5, +3 -> +100, -100, -2, +3 */
        __asm__ volatile(
            "msr apsr_nzcvqg, %[pb]\n\t"
            "shsub8 %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [pb] "r"(psr_b), [rn] "r"(shs8_rn), [rm] "r"(shs8_rm)
            : "cc"
        );
        if (res != 0x649CFE03U || (flags & psr_mask) != psr_b) {
            return 184;
        }

        /* 30a-3: SHADD16 */
        uint32_t sha16_rn = 0x4E20B1E0U; /* +20000, -20000 */
        uint32_t sha16_rm = 0x4E20B1DFU; /* +20000, -20001 -> +20000, -20001 */
        __asm__ volatile(
            "msr apsr_nzcvqg, %[pa]\n\t"
            "shadd16 %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [pa] "r"(psr_a), [rn] "r"(sha16_rn), [rm] "r"(sha16_rm)
            : "cc"
        );
        if (res != 0x4E20B1DFU || (flags & psr_mask) != psr_a) {
            return 185;
        }

        /* 30a-4: SHSUB16 */
        uint32_t shs16_rn = 0x4E20B1E0U; /* +20000, -20000 */
        uint32_t shs16_rm = 0xB1E04E21U; /* -20000, +20001 -> +20000, -20001 */
        __asm__ volatile(
            "msr apsr_nzcvqg, %[pb]\n\t"
            "shsub16 %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [pb] "r"(psr_b), [rn] "r"(shs16_rn), [rm] "r"(shs16_rm)
            : "cc"
        );
        if (res != 0x4E20B1DFU || (flags & psr_mask) != psr_b) {
            return 186;
        }

        /* 30a-5: UHADD8 */
        uint32_t uha8_rn = 0xC8FFFF0AU; /* 200, 255, 255, 10 */
        uint32_t uha8_rm = 0xC8FF0105U; /* 200, 255, 1, 5 -> 200, 255, 128, 7 */
        __asm__ volatile(
            "msr apsr_nzcvqg, %[pa]\n\t"
            "uhadd8 %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [pa] "r"(psr_a), [rn] "r"(uha8_rn), [rm] "r"(uha8_rm)
            : "cc"
        );
        if (res != 0xC8FF8007U || (flags & psr_mask) != psr_a) {
            return 187;
        }

        /*
         * 30a-6: UHSUB8 (including lanes where a < b:
         *   lane 3: (10 - 20) >> 1 = -5 (0xFB)
         *   lane 2: (0 - 255) >> 1 = -128 (0x80)
         *   lane 1: (200 - 50) >> 1 = 75 (0x4B)
         *   lane 0: (5 - 6) >> 1 = -1 (0xFF))
         */
        uint32_t uhs8_rn = 0x0A00C805U;
        uint32_t uhs8_rm = 0x14FF3206U;
        __asm__ volatile(
            "msr apsr_nzcvqg, %[pb]\n\t"
            "uhsub8 %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [pb] "r"(psr_b), [rn] "r"(uhs8_rn), [rm] "r"(uhs8_rm)
            : "cc"
        );
        if (res != 0xFB804BFFU || (flags & psr_mask) != psr_b) {
            return 188;
        }

        /* 30a-7: UHADD16 */
        uint32_t uha16_rn = 0xC350FFFFU; /* 50000, 65535 */
        uint32_t uha16_rm = 0xC3500001U; /* 50000, 1 -> 50000 (0xC350), 32768 (0x8000) */
        __asm__ volatile(
            "msr apsr_nzcvqg, %[pa]\n\t"
            "uhadd16 %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [pa] "r"(psr_a), [rn] "r"(uha16_rn), [rm] "r"(uha16_rm)
            : "cc"
        );
        if (res != 0xC3508000U || (flags & psr_mask) != psr_a) {
            return 189;
        }

        /*
         * 30a-8: UHSUB16 (including lanes where a < b:
         *   case 1: hi (100 - 300) >> 1 = -100 (0xFF9C), lo (50000 - 10000) >> 1 = 20000 (0x4E20)
         *   case 2: hi (0 - 65535) >> 1 = -32768 (0x8000), lo (10 - 11) >> 1 = -1 (0xFFFF))
         */
        uint32_t uhs16_rn1 = 0x0064C350U;
        uint32_t uhs16_rm1 = 0x012C2710U;
        uint32_t uhs16_rn2 = 0x0000000AU;
        uint32_t uhs16_rm2 = 0xFFFF000BU;
        __asm__ volatile(
            "msr apsr_nzcvqg, %[pb]\n\t"
            "uhsub16 %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [pb] "r"(psr_b), [rn] "r"(uhs16_rn1), [rm] "r"(uhs16_rm1)
            : "cc"
        );
        if (res != 0xFF9C4E20U || (flags & psr_mask) != psr_b) {
            return 190;
        }
        __asm__ volatile(
            "msr apsr_nzcvqg, %[pa]\n\t"
            "uhsub16 %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [pa] "r"(psr_a), [rn] "r"(uhs16_rn2), [rm] "r"(uhs16_rm2)
            : "cc"
        );
        if (res != 0x8000FFFFU || (flags & psr_mask) != psr_a) {
            return 191;
        }

        /*
         * 30b: Saturating parallel arithmetic — verify signed/unsigned saturation
         *      AND verify CPSR.Q (bit 27), CPSR.NZCV, and CPSR.GE[3:0] are NOT modified.
         */
        /* 30b-1: QADD8 */
        uint32_t qa8_rn = 0x649C28E2U; /* +100, -100, +40, -30 */
        uint32_t qa8_rm = 0x32CE140AU; /* +50, -50, +20, +10 -> +127, -128, +60, -20 */
        __asm__ volatile(
            "msr apsr_nzcvqg, %[pa]\n\t"
            "qadd8 %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [pa] "r"(psr_a), [rn] "r"(qa8_rn), [rm] "r"(qa8_rm)
            : "cc"
        );
        if (res != 0x7F803CECU || (flags & psr_mask) != psr_a) {
            return 192;
        }

        /* 30b-2: QSUB8 */
        uint32_t qs8_rn = 0x649C28F6U; /* +100, -100, +40, -10 */
        uint32_t qs8_rm = 0xCE320F14U; /* -50, +50, +15, +20 -> +127, -128, +25, -30 */
        __asm__ volatile(
            "msr apsr_nzcvqg, %[pa]\n\t"
            "qsub8 %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [pa] "r"(psr_a), [rn] "r"(qs8_rn), [rm] "r"(qs8_rm)
            : "cc"
        );
        if (res != 0x7F8019E2U || (flags & psr_mask) != psr_a) {
            return 193;
        }

        /* 30b-3: QADD16 */
        uint32_t qa16_rn1 = 0x75308AD0U; /* +30000, -30000 */
        uint32_t qa16_rm1 = 0x2710D8F0U; /* +10000, -10000 -> +32767 (0x7FFF), -32768 (0x8000) */
        uint32_t qa16_rn2 = 0x03E8FC18U; /* +1000, -1000 */
        uint32_t qa16_rm2 = 0x07D00190U; /* +2000, +400 -> +3000 (0x0BB8), -600 (0xFDA8) */
        __asm__ volatile(
            "msr apsr_nzcvqg, %[pa]\n\t"
            "qadd16 %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [pa] "r"(psr_a), [rn] "r"(qa16_rn1), [rm] "r"(qa16_rm1)
            : "cc"
        );
        if (res != 0x7FFF8000U || (flags & psr_mask) != psr_a) {
            return 194;
        }
        __asm__ volatile(
            "msr apsr_nzcvqg, %[pb]\n\t"
            "qadd16 %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [pb] "r"(psr_b), [rn] "r"(qa16_rn2), [rm] "r"(qa16_rm2)
            : "cc"
        );
        if (res != 0x0BB8FDA8U || (flags & psr_mask) != psr_b) {
            return 195;
        }

        /* 30b-4: QSUB16 */
        uint32_t qs16_rn1 = 0x75308AD0U; /* +30000, -30000 */
        uint32_t qs16_rm1 = 0xD8F02710U; /* -10000, +10000 -> +32767 (0x7FFF), -32768 (0x8000) */
        uint32_t qs16_rn2 = 0x07D0FC18U; /* +2000, -1000 */
        uint32_t qs16_rm2 = 0x01F401F4U; /* +500, +500 -> +1500 (0x05DC), -1500 (0xFA24) */
        __asm__ volatile(
            "msr apsr_nzcvqg, %[pa]\n\t"
            "qsub16 %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [pa] "r"(psr_a), [rn] "r"(qs16_rn1), [rm] "r"(qs16_rm1)
            : "cc"
        );
        if (res != 0x7FFF8000U || (flags & psr_mask) != psr_a) {
            return 196;
        }
        __asm__ volatile(
            "msr apsr_nzcvqg, %[pb]\n\t"
            "qsub16 %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [pb] "r"(psr_b), [rn] "r"(qs16_rn2), [rm] "r"(qs16_rm2)
            : "cc"
        );
        if (res != 0x05DCFA24U || (flags & psr_mask) != psr_b) {
            return 197;
        }

        /* 30b-5: UQADD8 */
        uint32_t uqa8_rn = 0xC8FF6400U; /* 200, 255, 100, 0 */
        uint32_t uqa8_rm = 0x6401322AU; /* 100, 1, 50, 42 -> 255, 255, 150 (0x96), 42 (0x2A) */
        __asm__ volatile(
            "msr apsr_nzcvqg, %[pa]\n\t"
            "uqadd8 %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [pa] "r"(psr_a), [rn] "r"(uqa8_rn), [rm] "r"(uqa8_rm)
            : "cc"
        );
        if (res != 0xFFFF962AU || (flags & psr_mask) != psr_a) {
            return 198;
        }

        /* 30b-6: UQSUB8 */
        uint32_t uqs8_rn = 0x3200C8FFU; /* 50, 0, 200, 255 */
        uint32_t uqs8_rm = 0x64FF32FFU; /* 100, 255, 50, 255 -> 0, 0, 150 (0x96), 0 */
        __asm__ volatile(
            "msr apsr_nzcvqg, %[pa]\n\t"
            "uqsub8 %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [pa] "r"(psr_a), [rn] "r"(uqs8_rn), [rm] "r"(uqs8_rm)
            : "cc"
        );
        if (res != 0x00009600U || (flags & psr_mask) != psr_a) {
            return 199;
        }

        /* 30b-7: UQADD16 */
        uint32_t uqa16_rn = 0xC3502710U; /* 50000, 10000 */
        uint32_t uqa16_rm = 0x75304E20U; /* 30000, 20000 -> 65535 (0xFFFF), 30000 (0x7530) */
        __asm__ volatile(
            "msr apsr_nzcvqg, %[pa]\n\t"
            "uqadd16 %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [pa] "r"(psr_a), [rn] "r"(uqa16_rn), [rm] "r"(uqa16_rm)
            : "cc"
        );
        if (res != 0xFFFF7530U || (flags & psr_mask) != psr_a) {
            return 200;
        }

        /* 30b-8: UQSUB16 */
        uint32_t uqs16_rn = 0x2710C350U; /* 10000, 50000 */
        uint32_t uqs16_rm = 0x75303A98U; /* 30000, 15000 -> 0 (0x0000), 35000 (0x88B8) */
        uint32_t zero_psr = 0U;
        __asm__ volatile(
            "msr apsr_nzcvqg, %[pa]\n\t"
            "uqsub16 %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            "msr apsr_nzcvqg, %[z]\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [pa] "r"(psr_a), [rn] "r"(uqs16_rn), [rm] "r"(uqs16_rm), [z] "r"(zero_psr)
            : "cc"
        );
        if (res != 0x000088B8U || (flags & psr_mask) != psr_a) {
            return 201;
        }
    }

    return 0;
}
