/*
 * tests/test_m1_part5.c — M1 ISA & FS tests (part 5)
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_m1_common.h"

int run_test_m1_part5(void)
{
    /*
     * 31. Round 7 positive & regression tests for all 12 parallel 16-bit
     *     exchange instructions:
     *     - 31a: SASX, SSAX, UASX, USAX + SEL (verifying cross-lane
     *            Rn[31:16] +/- Rm[15:0] and Rn[15:0] -/+ Rm[31:16] results,
     *            CPSR.GE[3:0] updates including signed 16-bit overflow, and
     *            CPSR.NZCV preservation).
     *     - 31b: SHASX, SHSAX, UHASX, UHSAX (verifying signed and unsigned
     *            halving cross-lane results including unsigned a < b lanes and
     *            preservation of CPSR.GE, CPSR.Q, and CPSR.NZCV).
     *     - 31c: QASX, QSAX, UQASX, UQSAX (verifying signed and unsigned
     *            saturating cross-lane results and preservation of CPSR.GE,
     *            CPSR.Q, and CPSR.NZCV).
     */
    {
        uint32_t res = 0U, flags = 0U, sel_out = 0U;
        uint32_t c1 = 10U, c2 = 20U;
        uint32_t sel_a = 0x11223344U;
        uint32_t sel_b = 0xAABBCCDDU;
        const uint32_t psr_mask = 0xF80F0000U; /* NZCV (31:28), Q (27), GE (19:16) */
        uint32_t psr_a = 0x60050000U;          /* N=0,Z=1,C=1,V=0, Q=0, GE=0b0101 */
        uint32_t psr_b = 0x980A0000U;          /* N=1,Z=0,C=0,V=1, Q=1, GE=0b1010 */
        uint32_t zero_psr = 0U;

        /*
         * 31a-1: SASX + SEL:
         *   case 1: Rn = 0x70001000 (+28672, +4096), Rm = 0x20002000 (+8192, +8192)
         *     hi = Rn[31:16] + Rm[15:0] = +28672 + +8192 = +36864 (0x9000) >= 0 -> GE[3:2] = 11
         *     lo = Rn[15:0] - Rm[31:16] = +4096  - +8192 = -4096  (0xF000) <  0 -> GE[1:0] = 00
         *   case 2: Rn = 0xF0002000 (-4096, +8192), Rm = 0xF0000800 (-4096, +2048)
         *     hi = -4096 + +2048  = -2048  (0xF800) <  0 -> GE[3:2] = 00
         *     lo = +8192 - -4096  = +12288 (0x3000) >= 0 -> GE[1:0] = 11
         */
        uint32_t sasx_rn1 = 0x70001000U, sasx_rm1 = 0x20002000U;
        uint32_t sasx_rn2 = 0xF0002000U, sasx_rm2 = 0xF0000800U;
        __asm__ volatile(
            "cmp %[c1], %[c1]\n\t"
            "sasx %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            "sel %[so], %[sa], %[sb]\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags), [so] "=&r"(sel_out)
            : [c1] "r"(c1), [rn] "r"(sasx_rn1), [rm] "r"(sasx_rm1),
              [sa] "r"(sel_a), [sb] "r"(sel_b)
            : "cc"
        );
        if (res != 0x9000F000U || sel_out != 0x1122CCDDU ||
            (flags & 0xF00F0000U) != 0x600C0000U) {
            return 202;
        }
        __asm__ volatile(
            "cmp %[c1], %[c2]\n\t"
            "sasx %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            "sel %[so], %[sa], %[sb]\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags), [so] "=&r"(sel_out)
            : [c1] "r"(c1), [c2] "r"(c2), [rn] "r"(sasx_rn2), [rm] "r"(sasx_rm2),
              [sa] "r"(sel_a), [sb] "r"(sel_b)
            : "cc"
        );
        if (res != 0xF8003000U || sel_out != 0xAABB3344U ||
            (flags & 0xF00F0000U) != 0x80030000U) {
            return 203;
        }

        /*
         * 31a-2: SSAX + SEL:
         *   case 1: Rn = 0x90007000 (-28672, +28672), Rm = 0x20002000 (+8192, +8192)
         *     hi = Rn[31:16] - Rm[15:0] = -28672 - +8192 = -36864 (0x7000) <  0 -> GE[3:2] = 00
         *     lo = Rn[15:0] + Rm[31:16] = +28672 + +8192 = +36864 (0x9000) >= 0 -> GE[1:0] = 11
         *   case 2: Rn = 0x3000F000 (+12288, -4096), Rm = 0x08001000 (+2048, +4096)
         *     hi = +12288 - +4096 = +8192 (0x2000) >= 0 -> GE[3:2] = 11
         *     lo = -4096  + +2048 = -2048 (0xF800) <  0 -> GE[1:0] = 00
         */
        uint32_t ssax_rn1 = 0x90007000U, ssax_rm1 = 0x20002000U;
        uint32_t ssax_rn2 = 0x3000F000U, ssax_rm2 = 0x08001000U;
        __asm__ volatile(
            "cmp %[c1], %[c1]\n\t"
            "ssax %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            "sel %[so], %[sa], %[sb]\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags), [so] "=&r"(sel_out)
            : [c1] "r"(c1), [rn] "r"(ssax_rn1), [rm] "r"(ssax_rm1),
              [sa] "r"(sel_a), [sb] "r"(sel_b)
            : "cc"
        );
        if (res != 0x70009000U || sel_out != 0xAABB3344U ||
            (flags & 0xF00F0000U) != 0x60030000U) {
            return 204;
        }
        __asm__ volatile(
            "cmp %[c1], %[c2]\n\t"
            "ssax %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            "sel %[so], %[sa], %[sb]\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags), [so] "=&r"(sel_out)
            : [c1] "r"(c1), [c2] "r"(c2), [rn] "r"(ssax_rn2), [rm] "r"(ssax_rm2),
              [sa] "r"(sel_a), [sb] "r"(sel_b)
            : "cc"
        );
        if (res != 0x2000F800U || sel_out != 0x1122CCDDU ||
            (flags & 0xF00F0000U) != 0x800C0000U) {
            return 205;
        }

        /*
         * 31a-3: UASX + SEL:
         *   case 1: Rn = 0xF0001000, Rm = 0x30002000
         *     hi = 0xF000 + 0x2000 = 0x11000 >= 0x10000 -> 0x1000, GE[3:2] = 11
         *     lo = 0x1000 - 0x3000 = -0x2000 <  0       -> 0xE000, GE[1:0] = 00
         *   case 2: Rn = 0x10005000, Rm = 0x20003000
         *     hi = 0x1000 + 0x3000 = 0x04000 <  0x10000 -> 0x4000, GE[3:2] = 00
         *     lo = 0x5000 - 0x2000 = +0x3000 >= 0       -> 0x3000, GE[1:0] = 11
         */
        uint32_t uasx_rn1 = 0xF0001000U, uasx_rm1 = 0x30002000U;
        uint32_t uasx_rn2 = 0x10005000U, uasx_rm2 = 0x20003000U;
        __asm__ volatile(
            "cmp %[c1], %[c1]\n\t"
            "uasx %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            "sel %[so], %[sa], %[sb]\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags), [so] "=&r"(sel_out)
            : [c1] "r"(c1), [rn] "r"(uasx_rn1), [rm] "r"(uasx_rm1),
              [sa] "r"(sel_a), [sb] "r"(sel_b)
            : "cc"
        );
        if (res != 0x1000E000U || sel_out != 0x1122CCDDU ||
            (flags & 0xF00F0000U) != 0x600C0000U) {
            return 206;
        }
        __asm__ volatile(
            "cmp %[c1], %[c2]\n\t"
            "uasx %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            "sel %[so], %[sa], %[sb]\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags), [so] "=&r"(sel_out)
            : [c1] "r"(c1), [c2] "r"(c2), [rn] "r"(uasx_rn2), [rm] "r"(uasx_rm2),
              [sa] "r"(sel_a), [sb] "r"(sel_b)
            : "cc"
        );
        if (res != 0x40003000U || sel_out != 0xAABB3344U ||
            (flags & 0xF00F0000U) != 0x80030000U) {
            return 207;
        }

        /*
         * 31a-4: USAX + SEL:
         *   case 1: Rn = 0x1000F000, Rm = 0x20003000
         *     hi = 0x1000 - 0x3000 = -0x2000 <  0       -> 0xE000, GE[3:2] = 00
         *     lo = 0xF000 + 0x2000 = 0x11000 >= 0x10000 -> 0x1000, GE[1:0] = 11
         *   case 2: Rn = 0x50001000, Rm = 0x30002000
         *     hi = 0x5000 - 0x2000 = +0x3000 >= 0       -> 0x3000, GE[3:2] = 11
         *     lo = 0x1000 + 0x3000 = 0x04000 <  0x10000 -> 0x4000, GE[1:0] = 00
         */
        uint32_t usax_rn1 = 0x1000F000U, usax_rm1 = 0x20003000U;
        uint32_t usax_rn2 = 0x50001000U, usax_rm2 = 0x30002000U;
        __asm__ volatile(
            "cmp %[c1], %[c1]\n\t"
            "usax %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            "sel %[so], %[sa], %[sb]\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags), [so] "=&r"(sel_out)
            : [c1] "r"(c1), [rn] "r"(usax_rn1), [rm] "r"(usax_rm1),
              [sa] "r"(sel_a), [sb] "r"(sel_b)
            : "cc"
        );
        if (res != 0xE0001000U || sel_out != 0xAABB3344U ||
            (flags & 0xF00F0000U) != 0x60030000U) {
            return 208;
        }
        __asm__ volatile(
            "cmp %[c1], %[c2]\n\t"
            "usax %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            "sel %[so], %[sa], %[sb]\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags), [so] "=&r"(sel_out)
            : [c1] "r"(c1), [c2] "r"(c2), [rn] "r"(usax_rn2), [rm] "r"(usax_rm2),
              [sa] "r"(sel_a), [sb] "r"(sel_b)
            : "cc"
        );
        if (res != 0x30004000U || sel_out != 0x1122CCDDU ||
            (flags & 0xF00F0000U) != 0x800C0000U) {
            return 209;
        }

        /*
         * 31b: Halving parallel 16-bit exchange (SHASX, SHSAX, UHASX, UHSAX):
         *      Verify cross-lane halving results and preservation of NZCV, Q, GE.
         */
        /* 31b-1: SHASX */
        uint32_t shasx_rn = 0x4E20B1E0U; /* hi = +20000, lo = -20000 */
        uint32_t shasx_rm = 0x4E214E20U; /* hi = +20001, lo = +20000 -> (+20000, -20001) */
        __asm__ volatile(
            "msr apsr_nzcvqg, %[pa]\n\t"
            "shasx %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [pa] "r"(psr_a), [rn] "r"(shasx_rn), [rm] "r"(shasx_rm)
            : "cc"
        );
        if (res != 0x4E20B1DFU || (flags & psr_mask) != psr_a) {
            return 210;
        }

        /* 31b-2: SHSAX */
        uint32_t shsax_rn = 0x4E20B1E0U; /* hi = +20000, lo = -20000 */
        uint32_t shsax_rm = 0xB1DFB1E0U; /* hi = -20001, lo = -20000 -> (+20000, -20001) */
        __asm__ volatile(
            "msr apsr_nzcvqg, %[pb]\n\t"
            "shsax %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [pb] "r"(psr_b), [rn] "r"(shsax_rn), [rm] "r"(shsax_rm)
            : "cc"
        );
        if (res != 0x4E20B1DFU || (flags & psr_mask) != psr_b) {
            return 211;
        }

        /* 31b-3: UHASX (both lo < Rm[31:16] negative diff and lo >= Rm[31:16]) */
        uint32_t uhasx_rn1 = 0xC3500064U; /* hi = 50000, lo = 100 */
        uint32_t uhasx_rm1 = 0x012CC350U; /* hi = 300,   lo = 50000 -> hi=50000 (0xC350), lo=(100-300)>>1=-100 (0xFF9C) */
        uint32_t uhasx_rn2 = 0xFFFFC350U; /* hi = 65535, lo = 50000 */
        uint32_t uhasx_rm2 = 0x27100001U; /* hi = 10000, lo = 1     -> hi=32768 (0x8000), lo=20000 (0x4E20) */
        __asm__ volatile(
            "msr apsr_nzcvqg, %[pa]\n\t"
            "uhasx %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [pa] "r"(psr_a), [rn] "r"(uhasx_rn1), [rm] "r"(uhasx_rm1)
            : "cc"
        );
        if (res != 0xC350FF9CU || (flags & psr_mask) != psr_a) {
            return 212;
        }
        __asm__ volatile(
            "msr apsr_nzcvqg, %[pb]\n\t"
            "uhasx %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [pb] "r"(psr_b), [rn] "r"(uhasx_rn2), [rm] "r"(uhasx_rm2)
            : "cc"
        );
        if (res != 0x80004E20U || (flags & psr_mask) != psr_b) {
            return 213;
        }

        /* 31b-4: UHSAX (both hi < Rm[15:0] negative diff and hi >= Rm[15:0]) */
        uint32_t uhsax_rn1 = 0x0064C350U; /* hi = 100,   lo = 50000 */
        uint32_t uhsax_rm1 = 0xC350012CU; /* hi = 50000, lo = 300   -> hi=-100 (0xFF9C), lo=50000 (0xC350) */
        uint32_t uhsax_rn2 = 0xC350FFFFU; /* hi = 50000, lo = 65535 */
        uint32_t uhsax_rm2 = 0x00012710U; /* hi = 1,     lo = 10000 -> hi=20000 (0x4E20), lo=32768 (0x8000) */
        __asm__ volatile(
            "msr apsr_nzcvqg, %[pa]\n\t"
            "uhsax %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [pa] "r"(psr_a), [rn] "r"(uhsax_rn1), [rm] "r"(uhsax_rm1)
            : "cc"
        );
        if (res != 0xFF9CC350U || (flags & psr_mask) != psr_a) {
            return 214;
        }
        __asm__ volatile(
            "msr apsr_nzcvqg, %[pb]\n\t"
            "uhsax %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [pb] "r"(psr_b), [rn] "r"(uhsax_rn2), [rm] "r"(uhsax_rm2)
            : "cc"
        );
        if (res != 0x4E208000U || (flags & psr_mask) != psr_b) {
            return 215;
        }

        /*
         * 31c: Saturating parallel 16-bit exchange (QASX, QSAX, UQASX, UQSAX):
         *      Verify saturating and non-saturating cross-lane results and
         *      preservation of NZCV, Q, GE.
         */
        /* 31c-1: QASX */
        uint32_t qasx_rn1 = 0x75308AD0U; /* hi = +30000, lo = -30000 */
        uint32_t qasx_rm1 = 0x27102710U; /* hi = +10000, lo = +10000 -> +32767 (0x7FFF), -32768 (0x8000) */
        uint32_t qasx_rn2 = 0x03E807D0U; /* hi = +1000,  lo = +2000 */
        uint32_t qasx_rm2 = 0x01F407D0U; /* hi = +500,   lo = +2000  -> +3000 (0x0BB8), +1500 (0x05DC) */
        __asm__ volatile(
            "msr apsr_nzcvqg, %[pa]\n\t"
            "qasx %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [pa] "r"(psr_a), [rn] "r"(qasx_rn1), [rm] "r"(qasx_rm1)
            : "cc"
        );
        if (res != 0x7FFF8000U || (flags & psr_mask) != psr_a) {
            return 216;
        }
        __asm__ volatile(
            "msr apsr_nzcvqg, %[pb]\n\t"
            "qasx %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [pb] "r"(psr_b), [rn] "r"(qasx_rn2), [rm] "r"(qasx_rm2)
            : "cc"
        );
        if (res != 0x0BB805DCU || (flags & psr_mask) != psr_b) {
            return 217;
        }

        /* 31c-2: QSAX */
        uint32_t qsax_rn1 = 0x75308AD0U; /* hi = +30000, lo = -30000 */
        uint32_t qsax_rm1 = 0xD8F0D8F0U; /* hi = -10000, lo = -10000 -> +32767 (0x7FFF), -32768 (0x8000) */
        uint32_t qsax_rn2 = 0x07D0FC18U; /* hi = +2000,  lo = -1000 */
        uint32_t qsax_rm2 = 0x019001F4U; /* hi = +400,   lo = +500   -> +1500 (0x05DC), -600 (0xFDA8) */
        __asm__ volatile(
            "msr apsr_nzcvqg, %[pa]\n\t"
            "qsax %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [pa] "r"(psr_a), [rn] "r"(qsax_rn1), [rm] "r"(qsax_rm1)
            : "cc"
        );
        if (res != 0x7FFF8000U || (flags & psr_mask) != psr_a) {
            return 218;
        }
        __asm__ volatile(
            "msr apsr_nzcvqg, %[pb]\n\t"
            "qsax %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [pb] "r"(psr_b), [rn] "r"(qsax_rn2), [rm] "r"(qsax_rm2)
            : "cc"
        );
        if (res != 0x05DCFDA8U || (flags & psr_mask) != psr_b) {
            return 219;
        }

        /* 31c-3: UQASX */
        uint32_t uqasx_rn1 = 0xC3502710U; /* hi = 50000, lo = 10000 */
        uint32_t uqasx_rm1 = 0x75307530U; /* hi = 30000, lo = 30000 -> 65535 (0xFFFF), 0 (0x0000) */
        uint32_t uqasx_rn2 = 0x2710C350U; /* hi = 10000, lo = 50000 */
        uint32_t uqasx_rm2 = 0x3A984E20U; /* hi = 15000, lo = 20000 -> 30000 (0x7530), 35000 (0x88B8) */
        __asm__ volatile(
            "msr apsr_nzcvqg, %[pa]\n\t"
            "uqasx %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [pa] "r"(psr_a), [rn] "r"(uqasx_rn1), [rm] "r"(uqasx_rm1)
            : "cc"
        );
        if (res != 0xFFFF0000U || (flags & psr_mask) != psr_a) {
            return 220;
        }
        __asm__ volatile(
            "msr apsr_nzcvqg, %[pb]\n\t"
            "uqasx %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [pb] "r"(psr_b), [rn] "r"(uqasx_rn2), [rm] "r"(uqasx_rm2)
            : "cc"
        );
        if (res != 0x753088B8U || (flags & psr_mask) != psr_b) {
            return 221;
        }

        /* 31c-4: UQSAX */
        uint32_t uqsax_rn1 = 0x2710C350U; /* hi = 10000, lo = 50000 */
        uint32_t uqsax_rm1 = 0x75307530U; /* hi = 30000, lo = 30000 -> 0 (0x0000), 65535 (0xFFFF) */
        uint32_t uqsax_rn2 = 0xC3502710U; /* hi = 50000, lo = 10000 */
        uint32_t uqsax_rm2 = 0x4E203A98U; /* hi = 20000, lo = 15000 -> 35000 (0x88B8), 30000 (0x7530) */
        __asm__ volatile(
            "msr apsr_nzcvqg, %[pa]\n\t"
            "uqsax %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [pa] "r"(psr_a), [rn] "r"(uqsax_rn1), [rm] "r"(uqsax_rm1)
            : "cc"
        );
        if (res != 0x0000FFFFU || (flags & psr_mask) != psr_a) {
            return 222;
        }
        __asm__ volatile(
            "msr apsr_nzcvqg, %[pb]\n\t"
            "uqsax %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            "msr apsr_nzcvqg, %[z]\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [pb] "r"(psr_b), [rn] "r"(uqsax_rn2), [rm] "r"(uqsax_rm2), [z] "r"(zero_psr)
            : "cc"
        );
        if (res != 0x88B87530U || (flags & psr_mask) != psr_b) {
            return 223;
        }
    }

#ifndef __thumb__
    /*
     * 32. Round 8 regression test for false-condition A32 LDM / STM with
     *     empty register list (reg_mask == 0):
     *     Verify that in A32 mode a false-condition LDMNE/STMNE/LDMEQ/STMEQ
     *     with register_list == 0 (e.g. .inst 0x18900000 when Z == 1 after
     *     cmp r0, r0) is skipped as a no-op without raising SIGILL, without
     *     performing any memory access or base writeback, and while preserving
     *     CPSR.NZCV.
     */
    {
        /* 32a: Z == 1 (cmp r0, r0): LDMIANE r0, {} (0x18900000) & LDMIANE r0!, {} (0x18B00000) */
        register uint32_t r0 __asm__("r0") = 0x12345678U;
        uint32_t flags = 0U;
        __asm__ volatile(
            "cmp r0, r0\n\t"
            ".inst 0x18900000\n\t" /* ldmiane r0, {} */
            ".inst 0x18B00000\n\t" /* ldmiane r0!, {} */
            ".inst 0x18800000\n\t" /* stmiane r0, {} */
            "mrs %[fl], apsr\n\t"
            : "+r"(r0), [fl] "=&r"(flags)
            :
            : "cc"
        );
        if (r0 != 0x12345678U || (flags & 0xF0000000U) != 0x60000000U) {
            return 224;
        }

        /* 32b: Z == 0 (cmp r0, r1 with 10 < 20): LDMIAEQ r0!, {} (0x08B00000) & STMIAEQ r0!, {} (0x08A00000) */
        r0 = 10U;
        register uint32_t r1 __asm__("r1") = 20U;
        flags = 0U;
        __asm__ volatile(
            "cmp r0, r1\n\t"
            ".inst 0x08B00000\n\t" /* ldmiaeq r0!, {} */
            ".inst 0x08A00000\n\t" /* stmiaeq r0!, {} */
            "mrs %[fl], apsr\n\t"
            : "+r"(r0), [fl] "=&r"(flags)
            : "r"(r1)
            : "cc"
        );
        if (r0 != 10U || (flags & 0xF0000000U) != 0x80000000U) {
            return 225;
        }
    }
#endif

    /*
     * 33. Round 10 positive & regression tests:
     *     - 33a: Expansion-heavy basic block splitting: execute a single
     *            straight-line sequence of 48 uadd8 / qadd8 instructions
     *            (24 uadd8 + 24 qadd8 interleaved) in inline assembly without
     *            any intermediate branches, verifying it translates and
     *            computes the exact expected parallel & saturated result
     *            without overflowing the JIT staging buffer.
     *     - 33b: qadd8 and uqadd8 preserving CPSR.NZCV across execution
     *            (tested across multiple NZCV combinations and verified via
     *            both MRS APSR and subsequent conditional instructions).
     */
    {
        /*
         * 33a: 48 straight-line uadd8 / qadd8 instructions in one basic block:
         *   u starts at 0x10203040, ustep = 0x0102030A (1, 2, 3, 10)
         *   After 24 uadd8 instructions:
         *     lane 3: 16 + 24 * 1  = 40  (0x28)
         *     lane 2: 32 + 24 * 2  = 80  (0x50)
         *     lane 1: 48 + 24 * 3  = 120 (0x78)
         *     lane 0: (64 + 24 * 10) & 0xFF = 304 & 0xFF = 48 (0x30)
         *     -> u == 0x28507830U
         *   q starts at 0x10EC00FA (+16, -20, 0, -6), qstep = 0x08F802FF (+8, -8, +2, -1)
         *   After 24 qadd8 instructions:
         *     lane 3: +16 + 24 * (+8) = +208 -> saturates to +127 (0x7F)
         *     lane 2: -20 + 24 * (-8) = -212 -> saturates to -128 (0x80)
         *     lane 1:   0 + 24 * (+2) =  +48 (0x30)
         *     lane 0:  -6 + 24 * (-1) =  -30 (0xE2)
         *     -> q == 0x7F8030E2U
         */
        uint32_t u_acc  = 0x10203040U;
        uint32_t u_step = 0x0102030AU;
        uint32_t q_acc  = 0x10EC00FAU;
        uint32_t q_step = 0x08F802FFU;
        uint32_t flags  = 0U;
        uint32_t psr_in = 0x60000000U; /* N=0, Z=1, C=1, V=0 */

#define R10_PAIR_UADD8_QADD8 \
        "uadd8 %[u], %[u], %[us]\n\t" \
        "qadd8 %[q], %[q], %[qs]\n\t"
#define R10_8_UADD8_QADD8 \
        R10_PAIR_UADD8_QADD8 \
        R10_PAIR_UADD8_QADD8 \
        R10_PAIR_UADD8_QADD8 \
        R10_PAIR_UADD8_QADD8
#define R10_48_UADD8_QADD8 \
        R10_8_UADD8_QADD8 \
        R10_8_UADD8_QADD8 \
        R10_8_UADD8_QADD8 \
        R10_8_UADD8_QADD8 \
        R10_8_UADD8_QADD8 \
        R10_8_UADD8_QADD8

        __asm__ volatile(
            "msr apsr_nzcvq, %[pin]\n\t"
            R10_48_UADD8_QADD8
            "mrs %[fl], apsr\n\t"
            : [u] "+r"(u_acc), [q] "+r"(q_acc), [fl] "=&r"(flags)
            : [us] "r"(u_step), [qs] "r"(q_step), [pin] "r"(psr_in)
            : "cc"
        );

#undef R10_48_UADD8_QADD8
#undef R10_8_UADD8_QADD8
#undef R10_PAIR_UADD8_QADD8

        if (u_acc != 0x28507830U || q_acc != 0x7F8030E2U ||
            (flags & 0xF0000000U) != 0x60000000U) {
            return 226;
        }

        /*
         * 33b: Positive regression test for qadd8 and uqadd8 preserving NZCV
         *      across all flag states (N, Z, C, V) and conditional execution.
         */
        static const uint32_t nzcv_patterns[5] = {
            0x00000000U, /* N=0, Z=0, C=0, V=0 */
            0x60000000U, /* N=0, Z=1, C=1, V=0 */
            0x80000000U, /* N=1, Z=0, C=0, V=0 */
            0x30000000U, /* N=0, Z=0, C=1, V=1 */
            0x90000000U  /* N=1, Z=0, C=0, V=1 */
        };
        uint32_t qa_rn  = 0x708510F0U; /* +112, -123, +16, -16 */
        uint32_t qa_rm  = 0x20E01506U; /*  +32,  -32, +21,  +6 -> +127 (0x7F), -128 (0x80), +37 (0x25), -10 (0xF6) */
        uint32_t uqa_rn = 0xF0801005U; /*  240,  128,  16,   5 */
        uint32_t uqa_rm = 0x3090200AU; /*   48,  144,  32,  10 ->  255 (0xFF),  255 (0xFF),  48 (0x30),  15 (0x0F) */

        for (uint32_t i = 0U; i < 5U; i++) {
            uint32_t expected_nzcv = nzcv_patterns[i];
            uint32_t q_res   = 0U;
            uint32_t uq_res  = 0U;
            uint32_t fl_q    = 0U;
            uint32_t fl_uq   = 0U;

            __asm__ volatile(
                "msr apsr_nzcvq, %[pin]\n\t"
                "qadd8 %[qr], %[qrn], %[qrm]\n\t"
                "mrs %[flq], apsr\n\t"
                "uqadd8 %[uqr], %[uqrn], %[uqrm]\n\t"
                "mrs %[fluq], apsr\n\t"
                : [qr] "=&r"(q_res), [uqr] "=&r"(uq_res),
                  [flq] "=&r"(fl_q), [fluq] "=&r"(fl_uq)
                : [pin] "r"(expected_nzcv),
                  [qrn] "r"(qa_rn), [qrm] "r"(qa_rm),
                  [uqrn] "r"(uqa_rn), [uqrm] "r"(uqa_rm)
                : "cc"
            );

            if (q_res != 0x7F8025F6U || uq_res != 0xFFFF300FU ||
                (fl_q & 0xF0000000U) != expected_nzcv ||
                (fl_uq & 0xF0000000U) != expected_nzcv) {
                return 227;
            }
        }

        /* Also verify CMP -> QADD8 -> UQADD8 -> conditional move uses CMP's NZCV */
        uint32_t c_a = 42U, c_b = 42U;
        uint32_t q_out = 0U, uq_out = 0U, cond_check = 0U;
        __asm__ volatile(
            "cmp %[ca], %[cb]\n\t"
            "qadd8 %[qo], %[qrn], %[qrm]\n\t"
            "uqadd8 %[uqo], %[uqrn], %[uqrm]\n\t"
#if defined(__thumb__)
            "ite eq\n\t"
#endif
            "moveq %[cc], #111\n\t"
            "movne %[cc], #222\n\t"
            : [qo] "=&r"(q_out), [uqo] "=&r"(uq_out), [cc] "=&r"(cond_check)
            : [ca] "r"(c_a), [cb] "r"(c_b),
              [qrn] "r"(qa_rn), [qrm] "r"(qa_rm),
              [uqrn] "r"(uqa_rn), [uqrm] "r"(uqa_rm)
            : "cc"
        );
        if (q_out != 0x7F8025F6U || uq_out != 0xFFFF300FU || cond_check != 111U) {
            return 228;
        }
    }

    /* 34: File-backed PROT_EXEC mmap2 execution test */
    {
        static const char tmp_path[] = "/tmp/a32rt_test_mmap_exec.bin";
        static uint32_t page_buf[1024];
        my_memset(page_buf, 0, sizeof(page_buf));
        page_buf[0] = 0xE30C00DEU; /* movw r0, #0xc0de */
        page_buf[1] = 0xE3410337U; /* movt r0, #0x1337 */
        page_buf[2] = 0xE12FFF1EU; /* bx lr */

        (void)sys1(10U /* unlink */, (uint32_t)(uintptr_t)tmp_path);

        /* O_RDWR(2) | O_CREAT(0x40) | O_TRUNC(0x200) = 0x242 */
        int32_t fd = sys3(5U /* open */, (uint32_t)(uintptr_t)tmp_path, 0x242U, 0700U);
        if (fd < 0) {
            return 229;
        }

        int32_t wr = sys3(4U /* write */, (uint32_t)fd, (uint32_t)(uintptr_t)page_buf, sizeof(page_buf));
        if (wr != (int32_t)sizeof(page_buf)) {
            sys1(6U /* close */, (uint32_t)fd);
            sys1(10U /* unlink */, (uint32_t)(uintptr_t)tmp_path);
            return 230;
        }

        /* mmap2(0, 4096, PROT_READ | PROT_EXEC (5), MAP_PRIVATE (2), fd, 0) */
        int32_t mres = sys6(192U /* mmap2 */, 0U, 4096U, 5U /* PROT_READ|PROT_EXEC */,
                            2U /* MAP_PRIVATE */, (uint32_t)fd, 0U);
        if (mres <= 0 || (uint32_t)mres > 0xFFFFF000U) {
            sys1(6U /* close */, (uint32_t)fd);
            sys1(10U /* unlink */, (uint32_t)(uintptr_t)tmp_path);
            return 231;
        }

        uint32_t (*fn)(void) = (uint32_t (*)(void))(uintptr_t)mres;
        uint32_t val = fn();

        (void)sys2(91U /* munmap */, (uint32_t)mres, 4096U);
        sys1(6U /* close */, (uint32_t)fd);
        (void)sys1(10U /* unlink */, (uint32_t)(uintptr_t)tmp_path);

        if (val != 0x1337C0DEU) {
            return 232;
        }
    }

    /* M6: Extended syscall verification (memfd_create, faccessat2, close_range, unshare) */
    {
        /* 1. memfd_create */
        static const char mfd_name[] = "m6_memfd";
        int32_t mfd = sys2(385U /* memfd_create */, (uint32_t)(uintptr_t)mfd_name, 1U /* MFD_CLOEXEC */);
        if (mfd < 0) {
            return 240;
        }
        uint32_t test_val = 0xDEADBEEFU;
        int32_t wr = sys3(4U /* write */, (uint32_t)mfd, (uint32_t)(uintptr_t)&test_val, 4U);
        if (wr != 4) {
            sys1(6U /* close */, (uint32_t)mfd);
            return 241;
        }
        uint64_t seek_res = 0;
        sys5_llseek((uint32_t)mfd, 0, 0, &seek_res, 0 /* SEEK_SET */);
        uint32_t read_val = 0;
        int32_t rd = sys3(3U /* read */, (uint32_t)mfd, (uint32_t)(uintptr_t)&read_val, 4U);
        sys1(6U /* close */, (uint32_t)mfd);
        if (rd != 4 || read_val != 0xDEADBEEFU) {
            return 242;
        }

        /* 2. faccessat2 */
        static const char proc_maps[] = "/proc/self/maps";
        int32_t acc = sys4(439U /* faccessat2 */, (uint32_t)-100 /* AT_FDCWD */,
                           (uint32_t)(uintptr_t)proc_maps, 4U /* R_OK */, 0U /* flags */);
        if (acc != 0) {
            return 243;
        }

        /* 3. close_range */
        int32_t cr = sys3(436U /* close_range */, 500U, 505U, 0U);
        if (cr != 0) {
            return 244;
        }

        /* 4. unshare */
        int32_t un = sys1(337U /* unshare */, 0U);
        if (un != 0) {
            return 245;
        }

        /* 5. Negative (-EFAULT / -EINVAL / -ENOENT) checks for M6 syscalls */
        if (sys2(385U /* memfd_create */, 0x100U /* unmapped */, 0U) != -14 /* -EFAULT */) {
            return 246;
        }
        if (sys2(385U /* memfd_create */, (uint32_t)(uintptr_t)mfd_name, 0x80000000U /* invalid flags */) != -22 /* -EINVAL */) {
            return 247;
        }
        if (sys4(439U /* faccessat2 */, (uint32_t)-100, 0x100U /* unmapped */, 0U, 0U) != -14 /* -EFAULT */) {
            return 248;
        }
        static const char missing_path[] = "/tmp/a32rt_nonexistent_m6_file_12345";
        if (sys4(439U /* faccessat2 */, (uint32_t)-100, (uint32_t)(uintptr_t)missing_path, 0U, 0U) != -2 /* -ENOENT */) {
            return 249;
        }
        if (sys4(439U /* faccessat2 */, (uint32_t)-100, (uint32_t)(uintptr_t)proc_maps, 0U, 0x12340000U) != -22 /* -EINVAL */) {
            return 250;
        }
        if (sys3(436U /* close_range */, 510U, 500U /* first > last */, 0U) != -22 /* -EINVAL */) {
            return 251;
        }
        if (sys3(436U /* close_range */, 500U, 505U, 0x80000000U /* bad flags */) != -22 /* -EINVAL */) {
            return 252;
        }

        /* 6. preadv (361) / pwritev (362) positive & negative (-EFAULT / -EINVAL) checks */
        int32_t pv_fd = sys2(385U /* memfd_create */, (uint32_t)(uintptr_t)mfd_name, 1U);
        if (pv_fd < 0) {
            return 253;
        }
        struct {
            uint32_t iov_base;
            uint32_t iov_len;
        } iov[2];
        uint32_t wpart0 = 0x11223344U, wpart1 = 0x55667788U;
        iov[0].iov_base = (uint32_t)(uintptr_t)&wpart0;
        iov[0].iov_len  = 4U;
        iov[1].iov_base = (uint32_t)(uintptr_t)&wpart1;
        iov[1].iov_len  = 4U;
        if (sys6(362U /* pwritev */, (uint32_t)pv_fd, (uint32_t)(uintptr_t)iov, 2U, 8U, 0U, 0U) != 8) {
            sys1(6U, (uint32_t)pv_fd);
            return 254;
        }
        uint32_t rpart0 = 0U, rpart1 = 0U;
        iov[0].iov_base = (uint32_t)(uintptr_t)&rpart0;
        iov[1].iov_base = (uint32_t)(uintptr_t)&rpart1;
        if (sys6(361U /* preadv */, (uint32_t)pv_fd, (uint32_t)(uintptr_t)iov, 2U, 8U, 0U, 0U) != 8 ||
            rpart0 != 0x11223344U || rpart1 != 0x55667788U) {
            sys1(6U, (uint32_t)pv_fd);
            return 255;
        }
        if (sys6(361U /* preadv */, (uint32_t)pv_fd, 0x100U /* unmapped iov */, 2U, 0U, 0U, 0U) != -14 /* -EFAULT */ ||
            sys6(361U /* preadv */, (uint32_t)pv_fd, (uint32_t)(uintptr_t)iov, (uint32_t)-1, 0U, 0U, 0U) != -22 /* -EINVAL */) {
            sys1(6U, (uint32_t)pv_fd);
            return 256;
        }
        sys1(6U, (uint32_t)pv_fd);

        /* 7. Deep RAS wrap (> 64 call levels) & non-local RAS mismatch fallback regression */
        uint32_t ras_acc = 0U;
        __asm__ volatile(
            "mov r0, #100\n\t"
            "mov r1, #0\n\t"
            "bl 1f\n\t"
            "b 4f\n"
            "1:\n\t"
            "push {lr}\n\t"
            "add r1, r1, r0\n\t"
            "subs r0, r0, #1\n\t"
            "beq 2f\n\t"
            "bl 1b\n"
            "2:\n\t"
            "pop {pc}\n"
            "4:\n\t"
            /* Now test non-local return (skipping a call frame so RAS top mismatches) */
            "bl 5f\n\t"
            "add r1, r1, #1000\n\t"
            "b 7f\n"
            "5:\n\t"
            "mov r2, lr\n\t"
            "bl 6f\n\t"
            "add r1, r1, #255\n\t"  /* skipped by non-local return! */
            "bx r2\n"
            "6:\n\t"
            "bx r2\n"               /* returns directly to 5f's caller (4f+4), mismatching RAS top (5f+8)! */
            "7:\n\t"
            "mov %[out], r1\n\t"
            : [out] "=r"(ras_acc)
            :
            : "r0", "r1", "r2", "lr", "memory", "cc"
        );
        /* Sum(1..100) = 5050, plus 1000 from non-local return = 6050 */
        if (ras_acc != 6050U) {
            return 257;
        }
    }

    return 0;
}
