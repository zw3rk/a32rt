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
 */

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
    printf("ARCH_TAG=%d\n", IOS_ARCH_TAG);
    return 0;
}
