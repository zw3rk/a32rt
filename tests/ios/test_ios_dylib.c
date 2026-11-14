/*
 * tests/ios/test_ios_dylib.c — dynamic PIE exe linked against libcalc_ios.
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Own constructor verifies libcalc's constructor already ran (g_lib_val==200),
 * then exercises Thumb/A32 cross-dylib calls, malloc family, snprintf.
 */

#include "ios_guest.h"

extern int g_lib_val;
extern int *g_lib_ptr;
extern int calc_thumb_add(int a, int b);
extern int calc_arm_mul(int a, int b);

static int exe_ctor_seen = 0;

static void exe_init(void) __attribute__((constructor));
static void exe_init(void)
{
    /* libcalc's constructor must have run first (dyld dependency order). */
    if (g_lib_val == 200 && g_lib_ptr != 0 && *g_lib_ptr == 200) {
        exe_ctor_seen = 1;
    }
}

int main(int argc, char **argv)
{
    int s = 0;
    int p = 0;
    char buf[64];
    char *m = 0;
    char *c = 0;
    char *r = 0;
    (void)argc;
    (void)argv;
    if (!exe_ctor_seen) {
        printf("FAIL dylib ctor order val=%d\n", g_lib_val);
        return 1;
    }
    s = calc_thumb_add(40, 2);
    p = calc_arm_mul(6, 7);
    if (s != 42 || p != 42) {
        printf("FAIL dylib call add=%d mul=%d\n", s, p);
        return 1;
    }
    m = (char *)malloc(32);
    c = (char *)calloc(4, 8);
    if (m == 0 || c == 0) {
        printf("FAIL dylib malloc\n");
        return 1;
    }
    memset(m, 0x41, 32);
    memset(c, 0x42, 32);
    r = (char *)realloc(m, 64);
    if (r == 0) {
        printf("FAIL dylib realloc\n");
        free(c);
        return 1;
    }
    if (r[0] != (char)0x41) {
        printf("FAIL dylib realloc content\n");
        free(r);
        free(c);
        return 1;
    }
    snprintf(buf, sizeof(buf), "add=%d mul=%d", s, p);
    if (strcmp(buf, "add=42 mul=42") != 0) {
        printf("FAIL dylib snprintf %s\n", buf);
        free(r);
        free(c);
        return 1;
    }
    free(r);
    free(c);
    printf("PASS test_ios_dylib\n");
    return 0;
}
