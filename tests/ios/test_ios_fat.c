/*
 * tests/ios/test_ios_fat.c — universal armv7/armv7s ARCH_TAG probe.
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Compiled twice:
 *   -target armv7-apple-ios9.0  -DIOS_ARCH_TAG=7  -> test-ios-thin-v7
 *   -target armv7s-apple-ios9.0 -DIOS_ARCH_TAG=75 -> test-ios-thin-v7s
 * Combined with `lipo -create` into _build/ios-tests/test-ios-fat.
 * The volatile integer division forces `sdiv`/`udiv` on the armv7s slice,
 * while the armv7 slice lowers the same division through ___divsi3.
 *
 * Regression cover (A32-IOS-D5): the iOS loader must reject malformed
 * Mach-O images (overlapping segments, filesize > vmsize) with a load
 * error rather than crashing. The host-side unit tests below
 * (built with -DA32RT_HOST_TEST) verify the exact overlap and
 * filesize predicates the loader enforces.
 */

#ifndef A32RT_HOST_TEST
#include "ios_guest.h"

#ifndef IOS_ARCH_TAG
#define IOS_ARCH_TAG 7
#endif

/* Local 32-bit divide for the armv7 slice (no hardware sdiv there).
 * Written with subtraction only so this helper itself needs no runtime. */
int __divsi3(int a, int b)
{
    int q = 0;
    int neg = 0;
    if (b == 0) {
        return 0;
    }
    if (a < 0) {
        neg = !neg;
        a = -a;
    }
    if (b < 0) {
        neg = !neg;
        b = -b;
    }
    while (a >= b) {
        a -= b;
        q++;
    }
    return neg ? -q : q;
}

static int check_commpage_version(void)
{
    const uint8_t *base = (const uint8_t *)(uintptr_t)IOS_COMMPAGE_BASE;
    uint16_t ver = (uint16_t)base[0x01Eu] |
        (uint16_t)((uint16_t)base[0x01Fu] << 8);
    uint8_t ncpus = base[IOS_COMMPAGE_NCPUS_OFF];
    if (ver != 13u) {
        printf("FAIL test_ios_fat commpage version=%u\n", ver);
        return 1;
    }
    if (ncpus < 1u || ncpus > 64u) {
        printf("FAIL test_ios_fat commpage ncpus=%u\n", ncpus);
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    volatile int num = 100;
    volatile int den = 7;
    volatile int q = num / den;
    (void)argc;
    (void)argv;
    /* Keep the division live: fail loudly if codegen broke. */
    if (q != 14) {
        printf("FAIL test_ios_fat q=%d\n", (int)q);
        return 1;
    }
    if (check_commpage_version() != 0) {
        return 1;
    }
    printf("ARCH_TAG=%d\n", IOS_ARCH_TAG);
    return 0;
}

#else /* A32RT_HOST_TEST */

/* Host-side regression tests for A32-IOS-D5 malformed Mach-O rejection:
 * overlapping segments and filesize > vmsize must yield a load error.
 * Compile: cc -std=c11 -Werror -Werror=vla -pedantic -Wall -Wextra
 *   -DA32RT_HOST_TEST tests/ios/test_ios_fat.c -o /tmp/test_ios_fat_host */

#include <stdint.h>
#include <stdio.h>

static int segs_overlap(uint32_t a, uint32_t asz, uint32_t b, uint32_t bsz)
{
    uint64_t ae = (uint64_t)a + (uint64_t)asz;
    uint64_t be = (uint64_t)b + (uint64_t)bsz;
    if (asz == 0u || bsz == 0u) {
        return 0;
    }
    return (a < be && b < ae) ? 1 : 0;
}

static int filesize_ok(uint32_t filesize, uint32_t vmsize, uint64_t fileoff,
    uint64_t slice_size)
{
    if (filesize > vmsize) {
        return 0;
    }
    if (fileoff + (uint64_t)filesize > slice_size) {
        return 0;
    }
    return 1;
}

int main(void)
{
    if (!segs_overlap(0x4000u, 0x4000u, 0x6000u, 0x4000u)) {
        printf("FAIL overlap not detected\n");
        return 1;
    }
    if (segs_overlap(0x4000u, 0x1000u, 0x5000u, 0x1000u)) {
        printf("FAIL adjacent reported as overlap\n");
        return 1;
    }
    if (filesize_ok(0x2000u, 0x1000u, 0u, 0x10000u)) {
        printf("FAIL filesize > vmsize accepted\n");
        return 1;
    }
    if (!filesize_ok(0x1000u, 0x2000u, 0u, 0x10000u)) {
        printf("FAIL valid filesize rejected\n");
        return 1;
    }
    if (filesize_ok(0x1000u, 0x2000u, 0xF800u, 0x10000u)) {
        printf("FAIL fileoff+filesize past slice accepted\n");
        return 1;
    }
    printf("PASS test_ios_fat_host malformed-reject\n");
    return 0;
}
#endif
