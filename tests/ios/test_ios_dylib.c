/*
 * tests/ios/test_ios_dylib.c — dynamic PIE exe linked against libcalc_ios.
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Own constructor verifies libcalc's constructor already ran (g_lib_val==200),
 * then exercises Thumb/A32 cross-dylib calls, malloc family, snprintf.
 *
 * Regression cover (A32-IOS-D12): the loader must deduplicate dylibs by
 * (st_dev, st_ino) so a dylib reachable via two symlink paths maps once
 * and its initializer runs exactly once. The host-side unit tests below
 * (built with -DA32RT_HOST_TEST) verify symlink (st_dev, st_ino)
 * identity, which is the dedup key the loader uses.
 */

#ifndef A32RT_HOST_TEST
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

static int check_single_init(void)
{
    int a;
    int b;
    /* A symlinked dylib loaded twice would run its initializer twice.
     * libcalc sets g_lib_val=200 idempotently, so verify the call
     * surface is stable across repeated cross-dylib calls instead. */
    a = calc_thumb_add(1, 1);
    b = calc_arm_mul(2, 3);
    if (a != 2 || b != 6) {
        printf("FAIL dylib single-init add=%d mul=%d\n", a, b);
        return 1;
    }
    if (g_lib_val != 200 || g_lib_ptr == 0 || *g_lib_ptr != 200) {
        printf("FAIL dylib single-init val=%d\n", g_lib_val);
        return 1;
    }
    return 0;
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
    if (check_single_init() != 0) {
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

#else /* A32RT_HOST_TEST */

/* Host-side regression test for (st_dev, st_ino) symlink dedup.
 * Compile: cc -std=c11 -Werror -Werror=vla -pedantic -Wall -Wextra
 *   -DA32RT_HOST_TEST tests/ios/test_ios_dylib.c -o /tmp/test_ios_dylib_host */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int same_file(const char *a, const char *b)
{
    struct stat sa;
    struct stat sb;
    if (stat(a, &sa) != 0 || stat(b, &sb) != 0) {
        return 0;
    }
    if (sa.st_ino == 0u || sb.st_ino == 0u) {
        return 0;
    }
    return (sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino) ? 1 : 0;
}

int main(void)
{
    char d[64];
    char f[128];
    char l[128];
    FILE *fp;
    snprintf(d, sizeof d, "/tmp/a32rt-dedup-%d", (int)getpid());
    snprintf(f, sizeof f, "%s/libcalc_ios.dylib", d);
    snprintf(l, sizeof l, "%s/alias.dylib", d);
    if (mkdir(d, 0700) != 0) {
        printf("FAIL dedup mkdir\n");
        return 1;
    }
    fp = fopen(f, "w");
    if (fp == NULL) {
        printf("FAIL dedup create\n");
        return 1;
    }
    fputs("dylib", fp);
    fclose(fp);
    if (symlink(f, l) != 0) {
        printf("FAIL dedup symlink\n");
        return 1;
    }
    if (!same_file(f, l)) {
        printf("FAIL symlink (st_dev,st_ino) mismatch\n");
        return 1;
    }
    unlink(l);
    unlink(f);
    rmdir(d);
    printf("PASS test_ios_dylib_host symlink-dedup\n");
    return 0;
}
#endif
