/*
 * tests/test_m1_isa_fs.c — M1 / M1-hl A32 & Thumb-2 ISA and filesystem test
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stddef.h>

struct linux_dirent64 {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[];
};

__asm__(
    ".text\n"
    ".globl __aeabi_uidiv\n"
    ".type __aeabi_uidiv, %function\n"
    "__aeabi_uidiv:\n"
    "    udiv r0, r0, r1\n"
    "    bx lr\n"
    ".globl __aeabi_idiv\n"
    ".type __aeabi_idiv, %function\n"
    "__aeabi_idiv:\n"
    "    sdiv r0, r0, r1\n"
    "    bx lr\n"
    ".globl __aeabi_uidivmod\n"
    ".type __aeabi_uidivmod, %function\n"
    "__aeabi_uidivmod:\n"
    "    udiv r2, r0, r1\n"
    "    mls r1, r2, r1, r0\n"
    "    mov r0, r2\n"
    "    bx lr\n"
    ".globl __aeabi_idivmod\n"
    ".type __aeabi_idivmod, %function\n"
    "__aeabi_idivmod:\n"
    "    sdiv r2, r0, r1\n"
    "    mls r1, r2, r1, r0\n"
    "    mov r0, r2\n"
    "    bx lr\n"
);

static inline int32_t sys1(uint32_t nr, uint32_t a0)
{
    register uint32_t r7 __asm__("r7") = nr;
    register uint32_t r0 __asm__("r0") = a0;
    __asm__ volatile("svc #0" : "+r"(r0) : "r"(r7) : "memory", "cc");
    return (int32_t)r0;
}

static inline int32_t sys2(uint32_t nr, uint32_t a0, uint32_t a1)
{
    register uint32_t r7 __asm__("r7") = nr;
    register uint32_t r0 __asm__("r0") = a0;
    register uint32_t r1 __asm__("r1") = a1;
    __asm__ volatile("svc #0" : "+r"(r0) : "r"(r7), "r"(r1) : "memory", "cc");
    return (int32_t)r0;
}

static inline int32_t sys3(uint32_t nr, uint32_t a0, uint32_t a1, uint32_t a2)
{
    register uint32_t r7 __asm__("r7") = nr;
    register uint32_t r0 __asm__("r0") = a0;
    register uint32_t r1 __asm__("r1") = a1;
    register uint32_t r2 __asm__("r2") = a2;
    __asm__ volatile("svc #0"
                     : "+r"(r0)
                     : "r"(r7), "r"(r1), "r"(r2)
                     : "memory", "cc");
    return (int32_t)r0;
}

static inline int32_t sys4(uint32_t nr, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3)
{
    register uint32_t r7 __asm__("r7") = nr;
    register uint32_t r0 __asm__("r0") = a0;
    register uint32_t r1 __asm__("r1") = a1;
    register uint32_t r2 __asm__("r2") = a2;
    register uint32_t r3 __asm__("r3") = a3;
    __asm__ volatile("svc #0"
                     : "+r"(r0)
                     : "r"(r7), "r"(r1), "r"(r2), "r"(r3)
                     : "memory", "cc");
    return (int32_t)r0;
}

static void my_memset(void *dst, int c, size_t n)
{
    uint8_t *p = (uint8_t *)dst;
    for (size_t i = 0; i < n; i++) {
        p[i] = (uint8_t)c;
    }
}

static int my_memcmp(const void *s1, const void *s2, size_t n)
{
    const uint8_t *p1 = (const uint8_t *)s1;
    const uint8_t *p2 = (const uint8_t *)s2;
    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i]) {
            return (int)p1[i] - (int)p2[i];
        }
    }
    return 0;
}

static const char *my_strstr(const char *haystack, const char *needle)
{
    if (!*needle) {
        return haystack;
    }
    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n && (*h == *n)) {
            h++;
            n++;
        }
        if (!*n) {
            return haystack;
        }
    }
    return NULL;
}

static int run_test(void)
{
    /* 1. Hardware division udiv / sdiv */
    volatile uint32_t u_a = 987654321U;
    volatile uint32_t u_b = 12345U;
    volatile uint32_t u_q = u_a / u_b;
    volatile uint32_t u_rem = u_a - u_q * u_b;
    if (u_q != 80004U || u_rem != 4941U) {
        return 1;
    }

    volatile int32_t s_a = -987654321;
    volatile int32_t s_b = 12345;
    volatile int32_t s_q = s_a / s_b;
    volatile int32_t s_rem = s_a - s_q * s_b;
    if (s_q != -80004 || s_rem != -4941) {
        return 2;
    }

    /* 2. Bitfields & byte-swaps */
    if (__builtin_clz(0x00100000U) != 11) {
        return 3;
    }
    if (__builtin_clz(0x80000000U) != 0) {
        return 4;
    }
    if (__builtin_clz(0x00000001U) != 31) {
        return 5;
    }
    if (__builtin_bswap32(0x12345678U) != 0x78563412U) {
        return 6;
    }
    if (__builtin_bswap16(0x1234U) != 0x3412U) {
        return 7;
    }

    /* 3. Shifts & rotates */
    volatile uint32_t v = 0xa55a1234U;
    if ((v >> 4) != 0x0a55a123U) {
        return 8;
    }
    if ((v << 4) != 0x55a12340U) {
        return 9;
    }
    volatile int32_t sv = (int32_t)0x80000000U;
    if ((sv >> 1) != (int32_t)0xc0000000U) {
        return 10;
    }
    uint32_t ror_val;
    __asm__ volatile("ror %0, %1, #8" : "=r"(ror_val) : "r"(v));
    if (ror_val != 0x34a55a12U) {
        return 11;
    }

    /* 4. 64-bit uint64_t multiply-accumulate umull / umlal */
    uint32_t lo = 0U, hi = 0U;
    uint32_t m1 = 0x80000001U, m2 = 3U;
    __asm__ volatile("umull %0, %1, %2, %3" : "=r"(lo), "=r"(hi) : "r"(m1), "r"(m2));
    if (hi != 1U || lo != 0x80000003U) {
        return 12;
    }
    __asm__ volatile("umlal %0, %1, %2, %3" : "+r"(lo), "+r"(hi) : "r"(m1), "r"(m2));
    if (hi != 3U || lo != 0x00000006U) {
        return 13;
    }

    /* 5. Scaled register offset loads/stores */
    uint32_t arr[8] = { 101U, 202U, 303U, 404U, 505U, 606U, 707U, 808U };
    uint32_t idx = 4U;
    uint32_t loaded = 0U;
    __asm__ volatile("ldr %0, [%1, %2, lsl #2]" : "=r"(loaded) : "r"(arr), "r"(idx));
    if (loaded != 505U) {
        return 14;
    }
    uint32_t stored = 999U;
    __asm__ volatile("str %0, [%1, %2, lsl #2]" : : "r"(stored), "r"(arr), "r"(idx) : "memory");
    if (arr[4] != 999U) {
        return 15;
    }

    /* 6. readlinkat / readlink on /proc/self/exe */
    char exe_path[256];
    my_memset(exe_path, 0, sizeof(exe_path));
    int32_t len = sys4(328U /* readlinkat */, 0xFFFFFF9CU /* AT_FDCWD (-100) */,
                       (uint32_t)(uintptr_t)"/proc/self/exe",
                       (uint32_t)(uintptr_t)exe_path, (uint32_t)(sizeof(exe_path) - 1U));
    if (len <= 0) {
        len = sys3(85U /* readlink */, (uint32_t)(uintptr_t)"/proc/self/exe",
                   (uint32_t)(uintptr_t)exe_path, (uint32_t)(sizeof(exe_path) - 1U));
    }
    if (len <= 0) {
        return 16;
    }

    /* 7. openat + read on /proc/self/maps */
    int32_t mfd = sys4(322U /* openat */, 0xFFFFFF9CU,
                       (uint32_t)(uintptr_t)"/proc/self/maps", 0U, 0U);
    if (mfd < 0) {
        mfd = sys3(5U /* open */, (uint32_t)(uintptr_t)"/proc/self/maps", 0U, 0U);
    }
    if (mfd < 0) {
        return 17;
    }
    char maps_buf[4096];
    my_memset(maps_buf, 0, sizeof(maps_buf));
    int32_t mread = 0;
    while (mread < (int32_t)(sizeof(maps_buf) - 1U)) {
        int32_t nr = sys3(3U /* read */, (uint32_t)mfd,
                          (uint32_t)(uintptr_t)(maps_buf + mread),
                          (uint32_t)(sizeof(maps_buf) - 1U - (uint32_t)mread));
        if (nr <= 0) {
            break;
        }
        mread += nr;
    }
    sys1(6U /* close */, (uint32_t)mfd);
    if (mread <= 0) {
        return 18;
    }
    maps_buf[mread] = '\0';
    if (!my_strstr(maps_buf, "ffff0000-ffff1000") && !my_strstr(maps_buf, "[vectors]")) {
        return 19;
    }

    /* 8. openat + getdents64 (217) on /tmp or . */
    int32_t dfd = sys4(322U /* openat */, 0xFFFFFF9CU,
                       (uint32_t)(uintptr_t)"/tmp", 0U, 0U);
    if (dfd < 0) {
        dfd = sys4(322U, 0xFFFFFF9CU, (uint32_t)(uintptr_t)".", 0U, 0U);
    }
    if (dfd < 0) {
        return 20;
    }
    char d_buf[1024];
    int32_t d_read = sys3(217U /* getdents64 */, (uint32_t)dfd,
                          (uint32_t)(uintptr_t)d_buf, (uint32_t)sizeof(d_buf));
    sys1(6U /* close */, (uint32_t)dfd);
    if (d_read <= 0) {
        return 21;
    }
    int d_pos = 0;
    int entries_found = 0;
    while (d_pos < d_read) {
        const struct linux_dirent64 *d = (const struct linux_dirent64 *)(const void *)(d_buf + d_pos);
        if (d->d_reclen == 0U || d->d_reclen > (uint16_t)(d_read - d_pos)) {
            return 22;
        }
        d_pos += (int)d->d_reclen;
        entries_found++;
    }
    if (entries_found == 0) {
        return 23;
    }

    /* 9. pipe2 (359) + fork (2) + write/read + wait4 (114) */
    int pfd[2];
    int32_t pr = sys2(359U /* pipe2 */, (uint32_t)(uintptr_t)pfd, 0U);
    if (pr != 0) {
        return 24;
    }
    int32_t pid = sys1(2U /* fork */, 0U);
    if (pid < 0) {
        return 25;
    }
    if (pid == 0) {
        sys1(6U /* close */, (uint32_t)pfd[0]);
        static const char msg[] = "m1-fork-ok";
        sys3(4U /* write */, (uint32_t)pfd[1], (uint32_t)(uintptr_t)msg, (uint32_t)(sizeof(msg) - 1U));
        sys1(6U /* close */, (uint32_t)pfd[1]);
        sys1(1U /* exit */, 42U);
        __builtin_unreachable();
    }
    sys1(6U /* close */, (uint32_t)pfd[1]);
    char pipe_in[16];
    my_memset(pipe_in, 0, sizeof(pipe_in));
    int32_t r_bytes = sys3(3U /* read */, (uint32_t)pfd[0],
                           (uint32_t)(uintptr_t)pipe_in, (uint32_t)(sizeof(pipe_in) - 1U));
    sys1(6U /* close */, (uint32_t)pfd[0]);
    if (r_bytes != 10) {
        return 26;
    }
    if (my_memcmp(pipe_in, "m1-fork-ok", 10U) != 0) {
        return 27;
    }

    int32_t wait_status = 0;
    int32_t wr = sys4(114U /* wait4 */, (uint32_t)pid,
                      (uint32_t)(uintptr_t)&wait_status, 0U, 0U);
    if (wr != pid) {
        return 28;
    }
    if ((wait_status & 0x7F) != 0) {
        return 29;
    }
    if (((wait_status >> 8) & 0xFF) != 42) {
        return 30;
    }

    static const char pass_msg[] = "M1 ISA & FS: PASS\n";
    sys3(4U /* write */, 1U, (uint32_t)(uintptr_t)pass_msg, (uint32_t)(sizeof(pass_msg) - 1U));
    return 0;
}

void __attribute__((noreturn)) _start_c(void)
{
    int rc = run_test();
    sys1(248U /* exit_group */, (uint32_t)rc);
    __builtin_unreachable();
}

__asm__(
    ".text\n"
    ".globl _start\n"
    ".type _start, %function\n"
    "_start:\n"
    "    mov r0, sp\n"
    "    bic r1, r0, #7\n"
    "    mov sp, r1\n"
    "    bl _start_c\n"
);
