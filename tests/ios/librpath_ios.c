/*
 * tests/ios/librpath_ios.c — @rpath/libc dylib fixture with __mod_init_func.
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Verifies @rpath resolution and constructor ordering: rpath_init() must
 * run before the executable's entry so rpath_ctor_ran() reports 77.
 */

#include "ios_guest.h"

static int g_rpath_inited = 0;

static void rpath_init(void) __attribute__((constructor));
static void rpath_init(void)
{
    g_rpath_inited = 77;
}

__attribute__((visibility("default"))) int rpath_ctor_ran(void)
{
    return g_rpath_inited;
}

__attribute__((visibility("default"))) int rpath_combine(int a, int b)
{
    return a * 100 + b + g_rpath_inited;
}
