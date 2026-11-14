/*
 * tests/ios/libcalc_ios.c — @executable_path/libcalc_ios.dylib fixture.
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Exercises LC_DYLD_INFO_ONLY rebase (g_lib_ptr), constructor ordering,
 * and Thumb/A32 interworking across the dylib boundary via the export trie.
 */

#include "ios_guest.h"

int g_lib_val = 100;
int *g_lib_ptr = &g_lib_val;

static int ctor_log[4];
static int ctor_n = 0;

static void lib_init(void) __attribute__((constructor));
static void lib_init(void)
{
    if (ctor_n < 4) {
        ctor_log[ctor_n] = 1;
    }
    ctor_n++;
    g_lib_val = 200;
}

int calc_thumb_add(int a, int b)
{
    return a + b;
}

__attribute__((target("arm"))) int calc_arm_mul(int a, int b)
{
    return a * b;
}
