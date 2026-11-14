/*
 * tests/ios/test_ios_static.c — static LC_UNIXTHREAD Thumb-2 smoke test.
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Linked with: ld -arch armv7 -static -e _start
 * Entry _start is Thumb-2 and verifies _COMM_PAGE, TPIDRURO, VFPv4/NEON,
 * then issues raw svc #0x80 SYS_write / SYS_exit.
 */

#include "ios_guest.h"

/* Thumb-2 entry trampoline: tail-call into C. Keeps `-e _start` working
 * (a C `void _start(void)` would mangle to `__start`). */
__asm__(".thumb\n"
        ".syntax unified\n"
        ".text\n"
        ".globl _start\n"
        ".thumb_func _start\n"
        "_start:\n"
        "  bl _ios_static_main\n"
        "  b .\n");

typedef float ios_v2f_t __attribute__((vector_size(8)));

static int check_commpage(void)
{
    const char *base = (const char *)(uintptr_t)IOS_COMMPAGE_BASE;
    const char *want = IOS_COMMPAGE_SIG;
    uint32_t i = 0U;
    for (i = 0U; i < IOS_COMMPAGE_SIG_LEN; i++) {
        if (base[i] != want[i]) {
            return 1;
        }
    }
    /* ncpus lives at offset 0x22; require at least one CPU. */
    if (*(const volatile uint8_t *)(uintptr_t)(IOS_COMMPAGE_BASE +
            IOS_COMMPAGE_NCPUS_OFF) < (uint8_t)1) {
        return 2;
    }
    return 0;
}

static int check_tls(void)
{
    uint32_t tp = ios_read_tpidruro();
    const volatile uint32_t *slot = 0;
    if (tp == 0U) {
        return 1;
    }
    slot = (const volatile uint32_t *)(uintptr_t)tp;
    if (*slot == 0U) {
        return 2;
    }
    return 0;
}

static int check_fp(void)
{
    /* Double FMA: 1.5 * 2.25 + 0.625 = 4.0 (VFPv4 vmla.f64). */
    volatile double a = 1.5;
    volatile double b = 2.25;
    volatile double c = 0.625;
    volatile double r = a * b + c;
    ios_v2f_t x = { 1.0f, 2.0f };
    ios_v2f_t y = { 3.0f, 4.0f };
    ios_v2f_t z = x + y;
    if (r < 3.999999 || r > 4.000001) {
        return 1;
    }
    if (z[0] < 3.999f || z[0] > 4.001f || z[1] < 5.999f || z[1] > 6.001f) {
        return 2;
    }
    return 0;
}

void ios_static_main(void)
{
    static const char pass[] = "PASS test_ios_static\n";
    int rc = 0;
    rc = check_commpage();
    if (rc != 0) {
        ios_raw_exit(10 + rc);
    }
    rc = check_tls();
    if (rc != 0) {
        ios_raw_exit(20 + rc);
    }
    rc = check_fp();
    if (rc != 0) {
        ios_raw_exit(30 + rc);
    }
    ios_raw_write(1, pass, (uint32_t)(sizeof(pass) - 1U));
    ios_raw_exit(0);
}
