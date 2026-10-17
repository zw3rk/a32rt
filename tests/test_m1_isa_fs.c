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

static inline int32_t sys5_llseek(uint32_t fd, uint32_t off_hi, uint32_t off_lo,
                                  uint64_t *res, uint32_t whence)
{
    register uint32_t r7 __asm__("r7") = 140U /* _llseek */;
    register uint32_t r0 __asm__("r0") = fd;
    register uint32_t r1 __asm__("r1") = off_hi;
    register uint32_t r2 __asm__("r2") = off_lo;
    register uint32_t r3 __asm__("r3") = (uint32_t)(uintptr_t)res;
    register uint32_t r4 __asm__("r4") = whence;
    __asm__ volatile("svc #0"
                     : "+r"(r0)
                     : "r"(r7), "r"(r1), "r"(r2), "r"(r3), "r"(r4)
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

static uint32_t __attribute__((noinline)) arm_lsl_reg(uint32_t val, uint32_t sh)
{
    uint32_t out;
    __asm__ volatile("lsl %0, %1, %2" : "=r"(out) : "r"(val), "r"(sh));
    return out;
}

static uint32_t __attribute__((noinline)) arm_lsr_reg(uint32_t val, uint32_t sh)
{
    uint32_t out;
    __asm__ volatile("lsr %0, %1, %2" : "=r"(out) : "r"(val), "r"(sh));
    return out;
}

static uint32_t __attribute__((noinline)) arm_asr_reg(uint32_t val, uint32_t sh)
{
    uint32_t out;
    __asm__ volatile("asr %0, %1, %2" : "=r"(out) : "r"(val), "r"(sh));
    return out;
}

static uint32_t __attribute__((noinline))
arm_lsls_reg_c(uint32_t val, uint32_t sh, uint32_t c_in, uint32_t *out_c)
{
    uint32_t out = 0U;
    uint32_t fl  = 0U;
    __asm__ volatile(
        "subs %[fl], %[cin], #1\n\t"
        "lsls %[out], %[val], %[sh]\n\t"
        "mrs  %[fl], apsr\n\t"
        : [out] "=&r"(out), [fl] "=&r"(fl)
        : [val] "r"(val), [sh] "r"(sh), [cin] "r"(c_in)
        : "cc"
    );
    *out_c = (fl >> 29) & 1U;
    return out;
}

static uint32_t __attribute__((noinline))
arm_lsrs_reg_c(uint32_t val, uint32_t sh, uint32_t c_in, uint32_t *out_c)
{
    uint32_t out = 0U;
    uint32_t fl  = 0U;
    __asm__ volatile(
        "subs %[fl], %[cin], #1\n\t"
        "lsrs %[out], %[val], %[sh]\n\t"
        "mrs  %[fl], apsr\n\t"
        : [out] "=&r"(out), [fl] "=&r"(fl)
        : [val] "r"(val), [sh] "r"(sh), [cin] "r"(c_in)
        : "cc"
    );
    *out_c = (fl >> 29) & 1U;
    return out;
}

static uint32_t __attribute__((noinline))
arm_asrs_reg_c(uint32_t val, uint32_t sh, uint32_t c_in, uint32_t *out_c)
{
    uint32_t out = 0U;
    uint32_t fl  = 0U;
    __asm__ volatile(
        "subs %[fl], %[cin], #1\n\t"
        "asrs %[out], %[val], %[sh]\n\t"
        "mrs  %[fl], apsr\n\t"
        : [out] "=&r"(out), [fl] "=&r"(fl)
        : [val] "r"(val), [sh] "r"(sh), [cin] "r"(c_in)
        : "cc"
    );
    *out_c = (fl >> 29) & 1U;
    return out;
}

static uint32_t __attribute__((noinline))
arm_rors_reg_c(uint32_t val, uint32_t sh, uint32_t c_in, uint32_t *out_c)
{
    uint32_t out = 0U;
    uint32_t fl  = 0U;
    __asm__ volatile(
        "subs %[fl], %[cin], #1\n\t"
        "rors %[out], %[val], %[sh]\n\t"
        "mrs  %[fl], apsr\n\t"
        : [out] "=&r"(out), [fl] "=&r"(fl)
        : [val] "r"(val), [sh] "r"(sh), [cin] "r"(c_in)
        : "cc"
    );
    *out_c = (fl >> 29) & 1U;
    return out;
}

#if defined(__thumb__)
static uint32_t __attribute__((noinline))
t16_lsls_reg_c(uint32_t val, uint32_t sh, uint32_t c_in, uint32_t *out_c)
{
    uint32_t io = val;
    uint32_t fl = 0U;
    __asm__ volatile(
        "subs %[fl], %[cin], #1\n\t"
        "lsls %[io], %[sh]\n\t"
        "mrs  %[fl], apsr\n\t"
        : [io] "+l"(io), [fl] "=&l"(fl)
        : [sh] "l"(sh), [cin] "l"(c_in)
        : "cc"
    );
    *out_c = (fl >> 29) & 1U;
    return io;
}

static uint32_t __attribute__((noinline))
t16_lsrs_reg_c(uint32_t val, uint32_t sh, uint32_t c_in, uint32_t *out_c)
{
    uint32_t io = val;
    uint32_t fl = 0U;
    __asm__ volatile(
        "subs %[fl], %[cin], #1\n\t"
        "lsrs %[io], %[sh]\n\t"
        "mrs  %[fl], apsr\n\t"
        : [io] "+l"(io), [fl] "=&l"(fl)
        : [sh] "l"(sh), [cin] "l"(c_in)
        : "cc"
    );
    *out_c = (fl >> 29) & 1U;
    return io;
}

static uint32_t __attribute__((noinline))
t16_asrs_reg_c(uint32_t val, uint32_t sh, uint32_t c_in, uint32_t *out_c)
{
    uint32_t io = val;
    uint32_t fl = 0U;
    __asm__ volatile(
        "subs %[fl], %[cin], #1\n\t"
        "asrs %[io], %[sh]\n\t"
        "mrs  %[fl], apsr\n\t"
        : [io] "+l"(io), [fl] "=&l"(fl)
        : [sh] "l"(sh), [cin] "l"(c_in)
        : "cc"
    );
    *out_c = (fl >> 29) & 1U;
    return io;
}

static uint32_t __attribute__((noinline))
t16_rors_reg_c(uint32_t val, uint32_t sh, uint32_t c_in, uint32_t *out_c)
{
    uint32_t io = val;
    uint32_t fl = 0U;
    __asm__ volatile(
        "subs %[fl], %[cin], #1\n\t"
        "rors %[io], %[sh]\n\t"
        "mrs  %[fl], apsr\n\t"
        : [io] "+l"(io), [fl] "=&l"(fl)
        : [sh] "l"(sh), [cin] "l"(c_in)
        : "cc"
    );
    *out_c = (fl >> 29) & 1U;
    return io;
}
#endif

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
    int32_t len = sys4(332U /* readlinkat */, 0xFFFFFF9CU /* AT_FDCWD (-100) */,
                       (uint32_t)(uintptr_t)"/proc/self/exe",
                       (uint32_t)(uintptr_t)exe_path, (uint32_t)(sizeof(exe_path) - 1U));
    if (len <= 0) {
        len = sys3(85U /* readlink */, (uint32_t)(uintptr_t)"/proc/self/exe",
                   (uint32_t)(uintptr_t)exe_path, (uint32_t)(sizeof(exe_path) - 1U));
    }
    if (len <= 0) {
        return 16;
    }

    /* 7. openat + read on /proc/self/maps (including rw-p vs -wxp check) */
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
    /* Verify stack / anonymous RW mappings are formatted as rw-p and never -wxp */
    if (!my_strstr(maps_buf, " rw-p ") || my_strstr(maps_buf, "-wxp")) {
        return 31;
    }

    /* 8. openat + getdents64 (217) on /tmp and . + lseek after getdents64 */
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

    /* Also read getdents64 on "." to ensure ext4 64-bit directory cookies are populated */
    int32_t dot_dfd = sys4(322U /* openat */, 0xFFFFFF9CU,
                           (uint32_t)(uintptr_t)".", 0U, 0U);
    if (dot_dfd >= 0) {
        (void)sys3(217U /* getdents64 */, (uint32_t)dot_dfd,
                   (uint32_t)(uintptr_t)d_buf, (uint32_t)sizeof(d_buf));
        sys1(6U /* close */, (uint32_t)dot_dfd);
    }

    /*
     * Regression test: open a regular file after getdents64 and verify
     * lseek(fd, 1, SEEK_SET) and lseek(fd, 2, SEEK_SET) return 1 and 2
     * (and do not corrupt small offsets into 64-bit directory cookies).
     */
    {
        int32_t rfd = sys4(322U /* openat */, 0xFFFFFF9CU,
                           (uint32_t)(uintptr_t)"/proc/self/maps", 0U, 0U);
        if (rfd < 0) {
            return 32;
        }
        char hdr4[4];
        my_memset(hdr4, 0, sizeof(hdr4));
        if (sys3(3U /* read */, (uint32_t)rfd, (uint32_t)(uintptr_t)hdr4, 4U) != 4) {
            sys1(6U /* close */, (uint32_t)rfd);
            return 33;
        }
        int32_t pos1 = sys3(19U /* lseek */, (uint32_t)rfd, 1U, 0U /* SEEK_SET */);
        if (pos1 != 1) {
            sys1(6U /* close */, (uint32_t)rfd);
            return 34;
        }
        char sub3[3];
        my_memset(sub3, 0, sizeof(sub3));
        if (sys3(3U /* read */, (uint32_t)rfd, (uint32_t)(uintptr_t)sub3, 3U) != 3 ||
            my_memcmp(sub3, hdr4 + 1, 3U) != 0) {
            sys1(6U /* close */, (uint32_t)rfd);
            return 35;
        }
        int32_t pos2 = sys3(19U /* lseek */, (uint32_t)rfd, 2U, 0U /* SEEK_SET */);
        if (pos2 != 2) {
            sys1(6U /* close */, (uint32_t)rfd);
            return 36;
        }
        char sub2[2];
        my_memset(sub2, 0, sizeof(sub2));
        if (sys3(3U /* read */, (uint32_t)rfd, (uint32_t)(uintptr_t)sub2, 2U) != 2 ||
            my_memcmp(sub2, hdr4 + 2, 2U) != 0) {
            sys1(6U /* close */, (uint32_t)rfd);
            return 37;
        }
        uint64_t ll_res = 0xDEADBEEFCAFEBABEULL;
        if (sys5_llseek((uint32_t)rfd, 0U, 1U, &ll_res, 0U /* SEEK_SET */) != 0 ||
            ll_res != 1ULL) {
            sys1(6U /* close */, (uint32_t)rfd);
            return 38;
        }
        if (sys5_llseek((uint32_t)rfd, 0U, 2U, &ll_res, 0U /* SEEK_SET */) != 0 ||
            ll_res != 2ULL) {
            sys1(6U /* close */, (uint32_t)rfd);
            return 39;
        }
        sys1(6U /* close */, (uint32_t)rfd);
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

    /*
     * 10. Flag liveness across MOVT and CLZ:
     * Verify SUBS / ADDS setting NZCV followed immediately by MOVT or CLZ
     * does NOT clobber or elide the preceding NZCV flags before a conditional
     * move (moveq/movne/movmi/movpl) or conditional branch (b.cond).
     */
    {
        /* 10a: SUBS sets Z=1 (after prior CMP set Z=0), followed by MOVT, then MOVEQ/MOVNE */
        uint32_t target = 0x00005678U;
        uint32_t cond_out = 0U;
        uint32_t a = 42U, b = 42U;
        __asm__ volatile(
            "cmp %[a], #0\n\t"
            "subs %[a], %[a], %[b]\n\t"
            "movt %[t], #0x1234\n\t"
#if defined(__thumb__)
            "ite eq\n\t"
#endif
            "moveq %[out], #100\n\t"
            "movne %[out], #200\n\t"
            : [t] "+r"(target), [out] "=&r"(cond_out), [a] "+r"(a)
            : [b] "r"(b)
            : "cc"
        );
        if (target != 0x12345678U || cond_out != 100U || a != 0U) {
            return 40;
        }

        /* 10b: SUBS sets Z=0, N=1 (after prior CMP set Z=1, N=0), followed by MOVT, then MOVMI/MOVPL */
        target = 0x0000ABCDU;
        cond_out = 0U;
        a = 10U;
        b = 25U;
        __asm__ volatile(
            "cmp %[a], %[a]\n\t"
            "subs %[a], %[a], %[b]\n\t"
            "movt %[t], #0xDEAD\n\t"
#if defined(__thumb__)
            "ite mi\n\t"
#endif
            "movmi %[out], #77\n\t"
            "movpl %[out], #88\n\t"
            : [t] "+r"(target), [out] "=&r"(cond_out), [a] "+r"(a)
            : [b] "r"(b)
            : "cc"
        );
        if (target != 0xDEADABCDU || cond_out != 77U || a != (uint32_t)(-15)) {
            return 41;
        }

        /* 10c: ADDS sets V=1 (after prior CMP set V=0, Z=1), followed by MOVT, then BVS */
        target = 0x00001111U;
        uint32_t branch_taken = 0U;
        a = 0x7FFFFFFFU;
        b = 1U;
        __asm__ volatile(
            "cmp %[a], %[a]\n\t"
            "adds %[a], %[a], %[b]\n\t"
            "movt %[t], #0x2222\n\t"
            "bvs 1f\n\t"
            "mov %[br], #0\n\t"
            "b 2f\n"
            "1:\n\t"
            "mov %[br], #1\n"
            "2:\n\t"
            : [t] "+r"(target), [br] "=&r"(branch_taken), [a] "+r"(a)
            : [b] "r"(b)
            : "cc"
        );
        if (target != 0x22221111U || branch_taken != 1U || a != 0x80000000U) {
            return 42;
        }

        /* 10d: SUBS sets Z=1 (after prior CMP set Z=0), followed by CLZ, then MOVEQ/MOVNE */
        uint32_t clz_in = 0x00080000U;
        uint32_t clz_out = 0U;
        cond_out = 0U;
        a = 123U;
        b = 123U;
        __asm__ volatile(
            "cmp %[a], #0\n\t"
            "subs %[a], %[a], %[b]\n\t"
            "clz %[co], %[ci]\n\t"
#if defined(__thumb__)
            "ite eq\n\t"
#endif
            "moveq %[out], #55\n\t"
            "movne %[out], #66\n\t"
            : [co] "=&r"(clz_out), [out] "=&r"(cond_out), [a] "+r"(a)
            : [ci] "r"(clz_in), [b] "r"(b)
            : "cc"
        );
        if (clz_out != 12U || cond_out != 55U || a != 0U) {
            return 43;
        }

        /* 10e: SUBS sets Z=0 (after prior CMP set Z=1), followed by CLZ, then MOVNE/MOVEQ */
        clz_in = 0U;
        clz_out = 0U;
        cond_out = 0U;
        a = 50U;
        b = 43U;
        __asm__ volatile(
            "cmp %[a], %[a]\n\t"
            "subs %[a], %[a], %[b]\n\t"
            "clz %[co], %[ci]\n\t"
#if defined(__thumb__)
            "ite ne\n\t"
#endif
            "movne %[out], #99\n\t"
            "moveq %[out], #11\n\t"
            : [co] "=&r"(clz_out), [out] "=&r"(cond_out), [a] "+r"(a)
            : [ci] "r"(clz_in), [b] "r"(b)
            : "cc"
        );
        if (clz_out != 32U || cond_out != 99U || a != 7U) {
            return 44;
        }

        /* 10f: ADDS sets C=1 (after prior CMP set C=0), followed by CLZ, then BCS */
        clz_in = 0x00000001U;
        clz_out = 0U;
        branch_taken = 0U;
        a = 0xFFFFFFFFU;
        b = 2U;
        __asm__ volatile(
            "cmp %[b], %[a]\n\t"
            "adds %[a], %[a], %[b]\n\t"
            "clz %[co], %[ci]\n\t"
            "bcs 1f\n\t"
            "mov %[br], #0\n\t"
            "b 2f\n"
            "1:\n\t"
            "mov %[br], #1\n"
            "2:\n\t"
            : [co] "=&r"(clz_out), [br] "=&r"(branch_taken), [a] "+r"(a)
            : [ci] "r"(clz_in), [b] "r"(b)
            : "cc"
        );
        if (clz_out != 31U || branch_taken != 1U || a != 1U) {
            return 45;
        }
    }

    /*
     * 11. PKHBT and PKHTB (including rd == rn and rd == rm aliasing):
     * PKHBT Rd, Rn, Rm, LSL #imm: Rd[15:0] = Rn[15:0], Rd[31:16] = (Rm << imm)[31:16]
     * PKHTB Rd, Rn, Rm, ASR #imm: Rd[31:16] = Rn[31:16], Rd[15:0] = (Rm ASR imm)[15:0]
     */
    {
        uint32_t rn = 0x12345678U;
        uint32_t rm = 0x9ABCDEF0U;
        uint32_t rd = 0U;

        /* 11a: PKHBT with rd != rn */
        __asm__ volatile("pkhbt %0, %1, %2, lsl #4" : "=r"(rd) : "r"(rn), "r"(rm));
        if (rd != 0xABCD5678U) {
            return 46;
        }
        __asm__ volatile("pkhbt %0, %1, %2" : "=r"(rd) : "r"(rn), "r"(rm));
        if (rd != 0x9ABC5678U) {
            return 47;
        }

        /* 11b: PKHBT with rd == rn aliasing (pkhbt r0, r0, r1, lsl #4) */
        {
            register uint32_t r0 __asm__("r0") = 0x12345678U;
            register uint32_t r1 __asm__("r1") = 0x9ABCDEF0U;
            __asm__ volatile("pkhbt r0, r0, r1, lsl #4" : "+r"(r0) : "r"(r1));
            if (r0 != 0xABCD5678U) {
                return 48;
            }
        }

        /* 11c: PKHBT with rd == rm aliasing (pkhbt r1, r0, r1, lsl #4) */
        {
            register uint32_t r0 __asm__("r0") = 0x12345678U;
            register uint32_t r1 __asm__("r1") = 0x9ABCDEF0U;
            __asm__ volatile("pkhbt r1, r0, r1, lsl #4" : "+r"(r1) : "r"(r0));
            if (r1 != 0xABCD5678U) {
                return 49;
            }
        }

        /* 11d: PKHTB with rd != rn */
        __asm__ volatile("pkhtb %0, %1, %2, asr #4" : "=r"(rd) : "r"(rn), "r"(rm));
        if (rd != 0x1234CDEFU) {
            return 50;
        }

        /* 11e: PKHTB with rd == rn aliasing (pkhtb r0, r0, r1, asr #4) */
        {
            register uint32_t r0 __asm__("r0") = 0x12345678U;
            register uint32_t r1 __asm__("r1") = 0x9ABCDEF0U;
            __asm__ volatile("pkhtb r0, r0, r1, asr #4" : "+r"(r0) : "r"(r1));
            if (r0 != 0x1234CDEFU) {
                return 51;
            }
        }

        /* 11f: PKHTB with rd == rm aliasing (pkhtb r1, r0, r1, asr #4) */
        {
            register uint32_t r0 __asm__("r0") = 0x12345678U;
            register uint32_t r1 __asm__("r1") = 0x9ABCDEF0U;
            __asm__ volatile("pkhtb r1, r0, r1, asr #4" : "+r"(r1) : "r"(r0));
            if (r1 != 0x1234CDEFU) {
                return 52;
            }
        }

        /* 11g: PKHTB with ASR #20 verifying arithmetic sign-extension of negative & positive Rm */
        {
            uint32_t rn2 = 0xCAFEBABEU;
            uint32_t rm_neg = 0x87654321U;
            uint32_t rm_pos = 0x76543210U;
            __asm__ volatile("pkhtb %0, %1, %2, asr #20" : "=r"(rd) : "r"(rn2), "r"(rm_neg));
            if (rd != 0xCAFEF876U) {
                return 53;
            }
            __asm__ volatile("pkhtb %0, %1, %2, asr #20" : "=r"(rd) : "r"(rn2), "r"(rm_pos));
            if (rd != 0xCAFE0765U) {
                return 54;
            }
        }
    }

    /*
     * 12. SSAT, USAT, and SEL preserving NZCV:
     * Set CPSR.NZCV via CMP, execute SSAT, USAT, and SEL, and verify
     * both result correctness and that NZCV bits [31:28] remain untouched.
     */
    {
        uint32_t res = 0U;
        uint32_t flags = 0U;
        uint32_t c1 = 10U, c2 = 20U;

        /* 12a: SSAT positive saturation with Z=1, C=1, N=0, V=0 (0x60000000) */
        int32_t s_in = 500;
        __asm__ volatile(
            "cmp %[c1], %[c1]\n\t"
            "ssat %[res], #8, %[in]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [c1] "r"(c1), [in] "r"(s_in)
            : "cc"
        );
        if (res != 127U || (flags & 0xF0000000U) != 0x60000000U) {
            return 55;
        }

        /* 12b: SSAT negative saturation with N=1, Z=0, C=0, V=0 (0x80000000) */
        s_in = -500;
        __asm__ volatile(
            "cmp %[c1], %[c2]\n\t"
            "ssat %[res], #8, %[in]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [c1] "r"(c1), [c2] "r"(c2), [in] "r"(s_in)
            : "cc"
        );
        if (res != (uint32_t)(-128) || (flags & 0xF0000000U) != 0x80000000U) {
            return 56;
        }

        /* 12c: SSAT non-saturating with V=1, C=1, N=0, Z=0 (0x30000000) */
        s_in = -42;
        uint32_t ov_a = 0x80000000U, ov_b = 1U;
        __asm__ volatile(
            "cmp %[oa], %[ob]\n\t"
            "ssat %[res], #8, %[in]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [oa] "r"(ov_a), [ob] "r"(ov_b), [in] "r"(s_in)
            : "cc"
        );
        if (res != (uint32_t)(-42) || (flags & 0xF0000000U) != 0x30000000U) {
            return 57;
        }

        /* 12d: USAT positive saturation with Z=1, C=1, N=0, V=0 (0x60000000) */
        s_in = 1000;
        __asm__ volatile(
            "cmp %[c1], %[c1]\n\t"
            "usat %[res], #8, %[in]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [c1] "r"(c1), [in] "r"(s_in)
            : "cc"
        );
        if (res != 255U || (flags & 0xF0000000U) != 0x60000000U) {
            return 58;
        }

        /* 12e: USAT negative saturation to 0 with N=1, Z=0, C=0, V=0 (0x80000000) */
        s_in = -50;
        __asm__ volatile(
            "cmp %[c1], %[c2]\n\t"
            "usat %[res], #8, %[in]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [c1] "r"(c1), [c2] "r"(c2), [in] "r"(s_in)
            : "cc"
        );
        if (res != 0U || (flags & 0xF0000000U) != 0x80000000U) {
            return 59;
        }

        /* 12f: USAT non-saturating with V=1, C=1, N=0, Z=0 (0x30000000) */
        s_in = 123;
        __asm__ volatile(
            "cmp %[oa], %[ob]\n\t"
            "usat %[res], #8, %[in]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [oa] "r"(ov_a), [ob] "r"(ov_b), [in] "r"(s_in)
            : "cc"
        );
        if (res != 123U || (flags & 0xF0000000U) != 0x30000000U) {
            return 60;
        }

        /* 12g: SEL preserving NZCV (both 0x60000000 and 0x80000000) */
        uint32_t sel_a = 0x11223344U;
        uint32_t sel_b = 0xAABBCCDDU;
        __asm__ volatile(
            "cmp %[c1], %[c1]\n\t"
            "sel %[res], %[sa], %[sb]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [c1] "r"(c1), [sa] "r"(sel_a), [sb] "r"(sel_b)
            : "cc"
        );
        if ((flags & 0xF0000000U) != 0x60000000U) {
            return 61;
        }
        __asm__ volatile(
            "cmp %[c1], %[c2]\n\t"
            "sel %[res], %[sa], %[sb]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [c1] "r"(c1), [c2] "r"(c2), [sa] "r"(sel_a), [sb] "r"(sel_b)
            : "cc"
        );
        if ((flags & 0xF0000000U) != 0x80000000U) {
            return 62;
        }
    }

#if defined(__thumb__)
    /*
     * 13. T16 CBZ / CBNZ preserving NZCV (when compiled as Thumb):
     * Verify both taken and not-taken paths of cbz and cbnz preserve NZCV.
     */
    {
        uint32_t flags = 0U;
        uint32_t taken = 0U;
        uint32_t c1 = 10U, c2 = 20U;

        /* 13a: CBZ not-taken (r0 != 0) preserves Z=1, C=1, N=0, V=0 (0x60000000) */
        {
            register uint32_t r0 __asm__("r0") = 0x80000001U;
            taken = 0U;
            __asm__ volatile(
                "cmp %[c1], %[c1]\n\t"
                "cbz r0, 1f\n\t"
                "mrs %[fl], apsr\n\t"
                "b 2f\n"
                "1:\n\t"
                "mov %[tk], #1\n"
                "2:\n\t"
                : [fl] "=&r"(flags), [tk] "+r"(taken)
                : "r"(r0), [c1] "r"(c1)
                : "cc"
            );
            if (taken != 0U || (flags & 0xF0000000U) != 0x60000000U) {
                return 63;
            }
        }

        /* 13b: CBZ taken (r0 == 0) preserves N=1, Z=0, C=0, V=0 (0x80000000) */
        {
            register uint32_t r0 __asm__("r0") = 0U;
            taken = 0U;
            __asm__ volatile(
                "cmp %[c1], %[c2]\n\t"
                "cbz r0, 1f\n\t"
                "b 2f\n"
                "1:\n\t"
                "mrs %[fl], apsr\n\t"
                "mov %[tk], #1\n"
                "2:\n\t"
                : [fl] "=&r"(flags), [tk] "+r"(taken)
                : "r"(r0), [c1] "r"(c1), [c2] "r"(c2)
                : "cc"
            );
            if (taken != 1U || (flags & 0xF0000000U) != 0x80000000U) {
                return 64;
            }
        }

        /* 13c: CBNZ not-taken (r0 == 0) preserves N=1, Z=0, C=0, V=0 (0x80000000) */
        {
            register uint32_t r0 __asm__("r0") = 0U;
            taken = 0U;
            __asm__ volatile(
                "cmp %[c1], %[c2]\n\t"
                "cbnz r0, 1f\n\t"
                "mrs %[fl], apsr\n\t"
                "b 2f\n"
                "1:\n\t"
                "mov %[tk], #1\n"
                "2:\n\t"
                : [fl] "=&r"(flags), [tk] "+r"(taken)
                : "r"(r0), [c1] "r"(c1), [c2] "r"(c2)
                : "cc"
            );
            if (taken != 0U || (flags & 0xF0000000U) != 0x80000000U) {
                return 65;
            }
        }

        /* 13d: CBNZ taken (r0 != 0) preserves Z=1, C=1, N=0, V=0 (0x60000000) */
        {
            register uint32_t r0 __asm__("r0") = 0x80000001U;
            taken = 0U;
            __asm__ volatile(
                "cmp %[c1], %[c1]\n\t"
                "cbnz r0, 1f\n\t"
                "b 2f\n"
                "1:\n\t"
                "mrs %[fl], apsr\n\t"
                "mov %[tk], #1\n"
                "2:\n\t"
                : [fl] "=&r"(flags), [tk] "+r"(taken)
                : "r"(r0), [c1] "r"(c1)
                : "cc"
            );
            if (taken != 1U || (flags & 0xF0000000U) != 0x60000000U) {
                return 66;
            }
        }
    }
#endif

    /*
     * 14. Register-specified shifts with Rs[7:0] >= 32 (LSL, LSR, ASR):
     * Test shift amounts 0, 31, 32, 33, 64, 255 (and 256 where Rs[7:0] == 0).
     */
    {
        uint32_t val = 0x87654321U;
        if (arm_lsl_reg(val, 0U) != val ||
            arm_lsl_reg(val, 31U) != 0x80000000U ||
            arm_lsl_reg(val, 32U) != 0U ||
            arm_lsl_reg(val, 33U) != 0U ||
            arm_lsl_reg(val, 64U) != 0U ||
            arm_lsl_reg(val, 255U) != 0U ||
            arm_lsl_reg(val, 256U) != val) {
            return 67;
        }
        if (arm_lsr_reg(val, 0U) != val ||
            arm_lsr_reg(val, 31U) != 1U ||
            arm_lsr_reg(val, 32U) != 0U ||
            arm_lsr_reg(val, 33U) != 0U ||
            arm_lsr_reg(val, 64U) != 0U ||
            arm_lsr_reg(val, 255U) != 0U ||
            arm_lsr_reg(val, 256U) != val) {
            return 68;
        }
        uint32_t neg_val = 0x80001234U;
        uint32_t pos_val = 0x7FFF1234U;
        if (arm_asr_reg(neg_val, 0U) != neg_val ||
            arm_asr_reg(neg_val, 31U) != 0xFFFFFFFFU ||
            arm_asr_reg(neg_val, 32U) != 0xFFFFFFFFU ||
            arm_asr_reg(neg_val, 33U) != 0xFFFFFFFFU ||
            arm_asr_reg(neg_val, 64U) != 0xFFFFFFFFU ||
            arm_asr_reg(neg_val, 255U) != 0xFFFFFFFFU ||
            arm_asr_reg(neg_val, 256U) != neg_val) {
            return 69;
        }
        if (arm_asr_reg(pos_val, 0U) != pos_val ||
            arm_asr_reg(pos_val, 31U) != 0U ||
            arm_asr_reg(pos_val, 32U) != 0U ||
            arm_asr_reg(pos_val, 33U) != 0U ||
            arm_asr_reg(pos_val, 64U) != 0U ||
            arm_asr_reg(pos_val, 255U) != 0U ||
            arm_asr_reg(pos_val, 256U) != pos_val) {
            return 70;
        }
    }

#if !defined(__thumb__)
    /*
     * 15. 64-bit multiply with flags (UMULLS, SMULLS) in A32:
     * Verify Z=1 on 0 64-bit result, Z=0 when only RdLo is 0, and N=1 on negative 64-bit result.
     */
    {
        uint32_t rlo = 0U, rhi = 0U, flags = 0U;
        uint32_t c1 = 10U, c2 = 20U;

        /* 15a: UMULLS 0 * 0x12345678 = 0 -> Z=1, N=0 */
        uint32_t u_zero = 0U, u_val = 0x12345678U;
        __asm__ volatile(
            "cmp %[c1], %[c2]\n\t"
            "umulls %[lo], %[hi], %[a], %[b]\n\t"
            "mrs %[fl], apsr\n\t"
            : [lo] "=&r"(rlo), [hi] "=&r"(rhi), [fl] "=&r"(flags)
            : [c1] "r"(c1), [c2] "r"(c2), [a] "r"(u_zero), [b] "r"(u_val)
            : "cc"
        );
        if (rlo != 0U || rhi != 0U || (flags & 0xC0000000U) != 0x40000000U) {
            return 71;
        }

        /* 15b: UMULLS 0x80000000 * 2 = 0x00000001_00000000 (lo=0, hi=1) -> Z=0, N=0 */
        uint32_t u_half = 0x80000000U, u_two = 2U;
        __asm__ volatile(
            "cmp %[c1], %[c1]\n\t"
            "umulls %[lo], %[hi], %[a], %[b]\n\t"
            "mrs %[fl], apsr\n\t"
            : [lo] "=&r"(rlo), [hi] "=&r"(rhi), [fl] "=&r"(flags)
            : [c1] "r"(c1), [a] "r"(u_half), [b] "r"(u_two)
            : "cc"
        );
        if (rlo != 0U || rhi != 1U || (flags & 0xC0000000U) != 0x00000000U) {
            return 72;
        }

        /* 15c: UMULLS 0xFFFFFFFF * 0xFFFFFFFF = 0xFFFFFFFE_00000001 -> N=1, Z=0 */
        uint32_t u_max = 0xFFFFFFFFU;
        __asm__ volatile(
            "cmp %[c1], %[c1]\n\t"
            "umulls %[lo], %[hi], %[a], %[b]\n\t"
            "mrs %[fl], apsr\n\t"
            : [lo] "=&r"(rlo), [hi] "=&r"(rhi), [fl] "=&r"(flags)
            : [c1] "r"(c1), [a] "r"(u_max), [b] "r"(u_max)
            : "cc"
        );
        if (rlo != 1U || rhi != 0xFFFFFFFEU || (flags & 0xC0000000U) != 0x80000000U) {
            return 73;
        }

        /* 15d: SMULLS 0 * -42 = 0 -> Z=1, N=0 */
        int32_t s_zero = 0, s_neg = -42;
        __asm__ volatile(
            "cmp %[c1], %[c2]\n\t"
            "smulls %[lo], %[hi], %[a], %[b]\n\t"
            "mrs %[fl], apsr\n\t"
            : [lo] "=&r"(rlo), [hi] "=&r"(rhi), [fl] "=&r"(flags)
            : [c1] "r"(c1), [c2] "r"(c2), [a] "r"(s_zero), [b] "r"(s_neg)
            : "cc"
        );
        if (rlo != 0U || rhi != 0U || (flags & 0xC0000000U) != 0x40000000U) {
            return 74;
        }

        /* 15e: SMULLS -3 * 5 = -15 (0xFFFFFFFF_FFFFFFF1) -> N=1, Z=0 */
        int32_t s_m3 = -3, s_p5 = 5;
        __asm__ volatile(
            "cmp %[c1], %[c1]\n\t"
            "smulls %[lo], %[hi], %[a], %[b]\n\t"
            "mrs %[fl], apsr\n\t"
            : [lo] "=&r"(rlo), [hi] "=&r"(rhi), [fl] "=&r"(flags)
            : [c1] "r"(c1), [a] "r"(s_m3), [b] "r"(s_p5)
            : "cc"
        );
        if (rlo != 0xFFFFFFF1U || rhi != 0xFFFFFFFFU || (flags & 0xC0000000U) != 0x80000000U) {
            return 75;
        }

        /* 15f: SMULLS -3 * -5 = +15 -> N=0, Z=0 */
        int32_t s_m5 = -5;
        __asm__ volatile(
            "cmp %[c1], %[c2]\n\t"
            "smulls %[lo], %[hi], %[a], %[b]\n\t"
            "mrs %[fl], apsr\n\t"
            : [lo] "=&r"(rlo), [hi] "=&r"(rhi), [fl] "=&r"(flags)
            : [c1] "r"(c1), [c2] "r"(c2), [a] "r"(s_m3), [b] "r"(s_m5)
            : "cc"
        );
        if (rlo != 15U || rhi != 0U || (flags & 0xC0000000U) != 0x00000000U) {
            return 76;
        }
    }
#endif

    /*
     * 16. LDRH / LDRSB / LDRSH post-index & pre-index writeback aliasing (rn == rt):
     * Verify loaded value in Rt is not overwritten by base register writeback when Rn == Rt.
     * Note: raw instruction encodings (.word for A32, .hword for T32) are used for the
     * Rn == Rt writeback cases because assemblers reject Rn == Rt writeback mnemonics.
     */
    {
        uint16_t hbuf[4] = { 0xBEEFU, 0xCAFEU, 0x1234U, 0x8001U };
        int8_t   bbuf[4] = { -42, -100, 55, -1 };

        /* 16a: LDRH post-index r0, [r0], #2 */
        {
            register uint32_t r0 __asm__("r0") = (uint32_t)(uintptr_t)&hbuf[0];
#if defined(__thumb__)
            __asm__ volatile(".hword 0xF830, 0x0B02" : "+r"(r0) : : "memory");
#else
            __asm__ volatile(".word 0xE0D000B2" : "+r"(r0) : : "memory");
#endif
            if (r0 != 0x0000BEEFU) {
                return 77;
            }
        }

        /* 16b: LDRH pre-index r0, [r0, #2]! */
        {
            register uint32_t r0 __asm__("r0") = (uint32_t)(uintptr_t)&hbuf[0];
#if defined(__thumb__)
            __asm__ volatile(".hword 0xF830, 0x0F02" : "+r"(r0) : : "memory");
#else
            __asm__ volatile(".word 0xE1F000B2" : "+r"(r0) : : "memory");
#endif
            if (r0 != 0x0000CAFEU) {
                return 78;
            }
        }

        /* 16c: LDRSB post-index r0, [r0], #1 */
        {
            register uint32_t r0 __asm__("r0") = (uint32_t)(uintptr_t)&bbuf[0];
#if defined(__thumb__)
            __asm__ volatile(".hword 0xF910, 0x0B01" : "+r"(r0) : : "memory");
#else
            __asm__ volatile(".word 0xE0D000D1" : "+r"(r0) : : "memory");
#endif
            if (r0 != (uint32_t)(int32_t)(-42)) {
                return 79;
            }
        }

        /* 16d: LDRSB pre-index r0, [r0, #1]! */
        {
            register uint32_t r0 __asm__("r0") = (uint32_t)(uintptr_t)&bbuf[0];
#if defined(__thumb__)
            __asm__ volatile(".hword 0xF910, 0x0F01" : "+r"(r0) : : "memory");
#else
            __asm__ volatile(".word 0xE1F000D1" : "+r"(r0) : : "memory");
#endif
            if (r0 != (uint32_t)(int32_t)(-100)) {
                return 80;
            }
        }

        /* 16e: LDRSH post-index r0, [r0], #2 */
        {
            register uint32_t r0 __asm__("r0") = (uint32_t)(uintptr_t)&hbuf[0];
#if defined(__thumb__)
            __asm__ volatile(".hword 0xF930, 0x0B02" : "+r"(r0) : : "memory");
#else
            __asm__ volatile(".word 0xE0D000F2" : "+r"(r0) : : "memory");
#endif
            if (r0 != 0xFFFFBEEFU) {
                return 81;
            }
        }

        /* 16f: LDRSH pre-index r0, [r0, #2]! */
        {
            register uint32_t r0 __asm__("r0") = (uint32_t)(uintptr_t)&hbuf[0];
#if defined(__thumb__)
            __asm__ volatile(".hword 0xF930, 0x0F02" : "+r"(r0) : : "memory");
#else
            __asm__ volatile(".word 0xE1F000F2" : "+r"(r0) : : "memory");
#endif
            if (r0 != 0xFFFFCAFEU) {
                return 82;
            }
        }

        /* 16g: Positive check when rn != rt: verify both loaded value and base writeback */
        {
            const uint16_t *p = &hbuf[0];
            uint32_t val_h = 0U;
            __asm__ volatile("ldrh %[v], [%[p]], #2"
                             : [v] "=&r"(val_h), [p] "+r"(p)
                             :
                             : "memory");
            if (val_h != 0x0000BEEFU || p != &hbuf[1]) {
                return 83;
            }
            __asm__ volatile("ldrsh %[v], [%[p], #2]!"
                             : [v] "=&r"(val_h), [p] "+r"(p)
                             :
                             : "memory");
            if (val_h != 0x00001234U || p != &hbuf[2]) {
                return 84;
            }
        }
    }

    /*
     * 17. UXTAB / UXTAH / SXTAB / SXTAH when Rd == Rn (and Rd == Rm):
     * Regression test verifying Rd is not overwritten before adding Rn when Rd == Rn.
     */
    {
        /* 17a: uxtab r0, r0, r1 in a 4-iteration loop */
        uint32_t acc = 1000U;
        uint32_t addend_u8 = 0x12345680U; /* low byte 0x80 = 128 */
        for (int i = 0; i < 4; i++) {
            register uint32_t r0 __asm__("r0") = acc;
            register uint32_t r1 __asm__("r1") = addend_u8;
            __asm__ volatile("uxtab r0, r0, r1" : "+r"(r0) : "r"(r1));
            acc = r0;
        }
        if (acc != 1512U) {
            return 85;
        }

        /* 17b: sxtah r0, r0, r1 in a 4-iteration loop */
        acc = 2000U;
        uint32_t addend_s16 = 0x1234FFCEU; /* low halfword 0xFFCE = -50 */
        for (int i = 0; i < 4; i++) {
            register uint32_t r0 __asm__("r0") = acc;
            register uint32_t r1 __asm__("r1") = addend_s16;
            __asm__ volatile("sxtah r0, r0, r1" : "+r"(r0) : "r"(r1));
            acc = r0;
        }
        if (acc != 1800U) {
            return 86;
        }

        /* 17c: uxtah r0, r0, r1, ror #8 and sxtab r0, r0, r1, ror #8 when Rd == Rn */
        {
            register uint32_t r0 __asm__("r0") = 5000U;
            register uint32_t r1 __asm__("r1") = 0x11223344U; /* (r1 ror 8)[15:0] = 0x2233 = 8755 */
            __asm__ volatile("uxtah r0, r0, r1, ror #8" : "+r"(r0) : "r"(r1));
            if (r0 != 13755U) {
                return 87;
            }
        }
        {
            register uint32_t r0 __asm__("r0") = 500U;
            register uint32_t r1 __asm__("r1") = 0x1122CE44U; /* (r1 ror 8)[7:0] = 0xCE = -50 */
            __asm__ volatile("sxtab r0, r0, r1, ror #8" : "+r"(r0) : "r"(r1));
            if (r0 != 450U) {
                return 88;
            }
        }
    }

    /*
     * 18. SMULWB / SMULWT and SMLAWB / SMLAWT with distinct top/bottom halfwords in Rm:
     * Verify B selects Rm[15:0] and T selects Rm[31:16].
     */
    {
        int32_t  rn32  = 0x00030000;      /* 3 * 65536 */
        uint32_t rm_bt = 0xFFF90005U;     /* top = -7 (0xFFF9), bottom = +5 (0x0005) */
        int32_t  ra32  = 100;
        int32_t  out_wb = 0, out_wt = 0;

        __asm__ volatile("smulwb %0, %1, %2" : "=r"(out_wb) : "r"(rn32), "r"(rm_bt));
        __asm__ volatile("smulwt %0, %1, %2" : "=r"(out_wt) : "r"(rn32), "r"(rm_bt));
        if (out_wb != 15 || out_wt != -21) {
            return 89;
        }

        __asm__ volatile("smlawb %0, %1, %2, %3" : "=r"(out_wb) : "r"(rn32), "r"(rm_bt), "r"(ra32));
        __asm__ volatile("smlawt %0, %1, %2, %3" : "=r"(out_wt) : "r"(rn32), "r"(rm_bt), "r"(ra32));
        if (out_wb != 115 || out_wt != 79) {
            return 90;
        }

        int32_t  rn_neg = -0x00028000;    /* -2.5 * 65536 = -163840 */
        uint32_t rm_bt2 = 0x0006FFFCU;    /* top = +6, bottom = -4 */
        int32_t  ra_neg = -50;
        __asm__ volatile("smulwb %0, %1, %2" : "=r"(out_wb) : "r"(rn_neg), "r"(rm_bt2));
        __asm__ volatile("smulwt %0, %1, %2" : "=r"(out_wt) : "r"(rn_neg), "r"(rm_bt2));
        if (out_wb != 10 || out_wt != -15) {
            return 91;
        }

        __asm__ volatile("smlawb %0, %1, %2, %3" : "=r"(out_wb) : "r"(rn_neg), "r"(rm_bt2), "r"(ra_neg));
        __asm__ volatile("smlawt %0, %1, %2, %3" : "=r"(out_wt) : "r"(rn_neg), "r"(rm_bt2), "r"(ra_neg));
        if (out_wb != -40 || out_wt != -65) {
            return 92;
        }
    }

    /*
     * 19. Extended multiply, dual-halfword, dual-extend, SAD, and saturating arithmetic:
     * SMMUL, SMMULR, SMMLA, SMMLS, UMAAL, SMUAD, SMUSD, SMLAD, SMLSD, SMLALD,
     * UXTB16, UXTAB16, SXTB16, SXTAB16, USAD8, USADA8, QADD, QSUB, QDADD, QDSUB.
     */
    {
        /* 19a: SMMUL, SMMULR, SMMLA, SMMLS */
        int32_t sm_a = 0x40000000; /* 2^30 */
        int32_t sm_b = 6;          /* product = 0x1_80000000 (bit 31 = 1) */
        int32_t sm_neg = -0x40000000;
        int32_t sm_acc = 100;
        int32_t sm_res = 0, sm_res_r = 0;

        __asm__ volatile("smmul  %0, %1, %2" : "=r"(sm_res)   : "r"(sm_a), "r"(sm_b));
        __asm__ volatile("smmulr %0, %1, %2" : "=r"(sm_res_r) : "r"(sm_a), "r"(sm_b));
        if (sm_res != 1 || sm_res_r != 2) {
            return 93;
        }

        __asm__ volatile("smmul  %0, %1, %2" : "=r"(sm_res)   : "r"(sm_neg), "r"(sm_b));
        __asm__ volatile("smmulr %0, %1, %2" : "=r"(sm_res_r) : "r"(sm_neg), "r"(sm_b));
        if (sm_res != -2 || sm_res_r != -1) {
            return 94;
        }

        __asm__ volatile("smmla %0, %1, %2, %3" : "=r"(sm_res) : "r"(sm_a), "r"(sm_b), "r"(sm_acc));
        if (sm_res != 101) {
            return 95;
        }
        __asm__ volatile("smmla %0, %1, %2, %3" : "=r"(sm_res) : "r"(sm_neg), "r"(sm_b), "r"(sm_acc));
        if (sm_res != 98) {
            return 96;
        }

        int32_t sm_b12 = 12; /* 12 * 2^30 = 3 * 2^32 */
        int32_t sm_ra50 = 50;
        __asm__ volatile("smmls %0, %1, %2, %3" : "=r"(sm_res) : "r"(sm_a), "r"(sm_b12), "r"(sm_ra50));
        if (sm_res != 47) {
            return 97;
        }
        __asm__ volatile("smmls %0, %1, %2, %3" : "=r"(sm_res) : "r"(sm_neg), "r"(sm_b12), "r"(sm_ra50));
        if (sm_res != 53) {
            return 98;
        }

        /* 19b: UMAAL */
        {
            register uint32_t r0 __asm__("r0") = 100U;
            register uint32_t r1 __asm__("r1") = 200U;
            register uint32_t r2 __asm__("r2") = 0x80000000U;
            register uint32_t r3 __asm__("r3") = 4U;
            __asm__ volatile("umaal r0, r1, r2, r3" : "+r"(r0), "+r"(r1) : "r"(r2), "r"(r3));
            if (r0 != 300U || r1 != 2U) {
                return 99;
            }
        }
        {
            register uint32_t r0 __asm__("r0") = 0xFFFFFFFFU;
            register uint32_t r1 __asm__("r1") = 0xFFFFFFFFU;
            register uint32_t r2 __asm__("r2") = 0xFFFFFFFFU;
            register uint32_t r3 __asm__("r3") = 0xFFFFFFFFU;
            __asm__ volatile("umaal r0, r1, r2, r3" : "+r"(r0), "+r"(r1) : "r"(r2), "r"(r3));
            if (r0 != 0xFFFFFFFFU || r1 != 0xFFFFFFFFU) {
                return 100;
            }
        }

        /* 19c: SMUAD, SMUSD, SMLAD, SMLSD, SMLALD */
        uint32_t d_rn = ((uint32_t)(uint16_t)(-3) << 16) | 4U; /* top = -3, bot = +4 */
        uint32_t d_rm = ((uint32_t)(uint16_t)5 << 16) | 6U;    /* top = +5, bot = +6 */
        int32_t  d_ra = 100;
        int32_t  d_out = 0;

        /* p1 = 4*6 = 24, p2 = -3*5 = -15 -> p1 + p2 = 9, p1 - p2 = 39 */
        __asm__ volatile("smuad %0, %1, %2" : "=r"(d_out) : "r"(d_rn), "r"(d_rm));
        if (d_out != 9) {
            return 101;
        }
        __asm__ volatile("smusd %0, %1, %2" : "=r"(d_out) : "r"(d_rn), "r"(d_rm));
        if (d_out != 39) {
            return 102;
        }
        __asm__ volatile("smlad %0, %1, %2, %3" : "=r"(d_out) : "r"(d_rn), "r"(d_rm), "r"(d_ra));
        if (d_out != 109) {
            return 103;
        }
        __asm__ volatile("smlsd %0, %1, %2, %3" : "=r"(d_out) : "r"(d_rn), "r"(d_rm), "r"(d_ra));
        if (d_out != 139) {
            return 104;
        }
        {
            register uint32_t r0 __asm__("r0") = 0xFFFFFFFCU;
            register uint32_t r1 __asm__("r1") = 0U;
            register uint32_t r2 __asm__("r2") = d_rn;
            register uint32_t r3 __asm__("r3") = d_rm;
            __asm__ volatile("smlald r0, r1, r2, r3" : "+r"(r0), "+r"(r1) : "r"(r2), "r"(r3));
            if (r0 != 5U || r1 != 1U) {
                return 105;
            }
        }

        /* 19d: UXTB16, UXTAB16, SXTB16, SXTAB16 */
        uint32_t x16_rm = 0x128456F3U;
        uint32_t x16_out = 0U;
        __asm__ volatile("uxtb16 %0, %1" : "=r"(x16_out) : "r"(x16_rm));
        if (x16_out != 0x008400F3U) {
            return 106;
        }
        __asm__ volatile("uxtb16 %0, %1, ror #8" : "=r"(x16_out) : "r"(x16_rm));
        if (x16_out != 0x00120056U) {
            return 107;
        }

        uint32_t ux16_rn = 0x0010FFFEU;
        uint32_t ux16_rm = 0x99058804U; /* [23:16]=5, [7:0]=4 -> lo wraps without carry into hi */
        __asm__ volatile("uxtab16 %0, %1, %2" : "=r"(x16_out) : "r"(ux16_rn), "r"(ux16_rm));
        if (x16_out != 0x00150002U) {
            return 108;
        }

        uint32_t sx16_rm = 0x1280567FU; /* [23:16]=0x80 (-128), [7:0]=0x7F (+127) */
        __asm__ volatile("sxtb16 %0, %1" : "=r"(x16_out) : "r"(sx16_rm));
        if (x16_out != 0xFF80007FU) {
            return 109;
        }

        uint32_t sxa16_rn = 0x00640064U; /* 100 in both halfwords */
        uint32_t sxa16_rm = 0x99F68814U; /* [23:16]=0xF6 (-10), [7:0]=0x14 (+20) */
        __asm__ volatile("sxtab16 %0, %1, %2" : "=r"(x16_out) : "r"(sxa16_rn), "r"(sxa16_rm));
        if (x16_out != 0x005A0078U) {
            return 110;
        }

        /* 19e: USAD8 and USADA8 */
        uint32_t sad_a = 0x1050A0FFU; /* 16, 80, 160, 255 */
        uint32_t sad_b = 0x2030B0F0U; /* 32, 48, 176, 240 -> |16-32|+|80-48|+|160-176|+|255-240| = 79 */
        uint32_t sad_acc = 1000U;
        uint32_t sad_out = 0U;
        __asm__ volatile("usad8  %0, %1, %2" : "=&r"(sad_out) : "r"(sad_a), "r"(sad_b));
        if (sad_out != 79U) {
            return 111;
        }
        __asm__ volatile("usada8 %0, %1, %2, %3" : "=&r"(sad_out) : "r"(sad_a), "r"(sad_b), "r"(sad_acc));
        if (sad_out != 1079U) {
            return 112;
        }

        /* 19f: QADD, QSUB, QDADD, QDSUB (both non-saturating and saturating) */
        int32_t q_out = 0;
        int32_t q_100 = 100, q_200 = 200, q_300 = 300, q_50 = 50, q_m100 = -100;
        int32_t q_pos_near = 0x7FFFFFF0;
        int32_t q_neg_near = (int32_t)0x80000010U;
        int32_t q_big_pos  = 0x50000000;

        __asm__ volatile("qadd %0, %1, %2" : "=r"(q_out) : "r"(q_100), "r"(q_200));
        if (q_out != 300) {
            return 113;
        }
        __asm__ volatile("qadd %0, %1, %2" : "=r"(q_out) : "r"(q_pos_near), "r"(q_100));
        if (q_out != (int32_t)0x7FFFFFFFU) {
            return 114;
        }
        __asm__ volatile("qadd %0, %1, %2" : "=r"(q_out) : "r"(q_neg_near), "r"(q_m100));
        if (q_out != (int32_t)0x80000000U) {
            return 115;
        }

        __asm__ volatile("qsub %0, %1, %2" : "=r"(q_out) : "r"(q_300), "r"(q_100));
        if (q_out != 200) {
            return 116;
        }
        __asm__ volatile("qsub %0, %1, %2" : "=r"(q_out) : "r"(q_pos_near), "r"(q_m100));
        if (q_out != (int32_t)0x7FFFFFFFU) {
            return 117;
        }
        __asm__ volatile("qsub %0, %1, %2" : "=r"(q_out) : "r"(q_neg_near), "r"(q_100));
        if (q_out != (int32_t)0x80000000U) {
            return 118;
        }

        __asm__ volatile("qdadd %0, %1, %2" : "=r"(q_out) : "r"(q_100), "r"(q_50));
        if (q_out != 200) {
            return 119;
        }
        __asm__ volatile("qdadd %0, %1, %2" : "=r"(q_out) : "r"(q_100), "r"(q_big_pos));
        if (q_out != (int32_t)0x7FFFFFFFU) {
            return 120;
        }

        __asm__ volatile("qdsub %0, %1, %2" : "=r"(q_out) : "r"(q_300), "r"(q_50));
        if (q_out != 200) {
            return 121;
        }
        int32_t q_zero = 0;
        __asm__ volatile("qdsub %0, %1, %2" : "=r"(q_out) : "r"(q_zero), "r"(q_big_pos));
        if (q_out != (int32_t)0x80000001U) {
            return 122;
        }
    }

#if !defined(__thumb__)
    /*
     * 20. Regression test for A32 post-indexed register load when Rt == Rm:
     * Verify `ldr r0, [r1], r0` and `ldrh r0, [r1], r0` compute base writeback
     * using the original offset in r0 rather than the newly loaded value.
     */
    {
        uint32_t warr[4] = { 0xCAFEBABEU, 0x11223344U, 0x55667788U, 0x99AABBCCU };
        {
            register uint32_t r0 __asm__("r0") = 8U; /* 2 words */
            register uint32_t r1 __asm__("r1") = (uint32_t)(uintptr_t)&warr[0];
            /* A32: ldr r0, [r1], r0 */
            __asm__ volatile(".word 0xE6910000" : "+r"(r0), "+r"(r1) : : "memory");
            if (r0 != 0xCAFEBABEU || r1 != (uint32_t)(uintptr_t)&warr[2]) {
                return 123;
            }
        }

        uint16_t harr[4] = { 0xBEEFU, 0x1111U, 0x2222U, 0x3333U };
        {
            register uint32_t r0 __asm__("r0") = 4U; /* 2 halfwords */
            register uint32_t r1 __asm__("r1") = (uint32_t)(uintptr_t)&harr[0];
            /* A32: ldrh r0, [r1], r0 */
            __asm__ volatile(".word 0xE09100B0" : "+r"(r0), "+r"(r1) : : "memory");
            if (r0 != 0x0000BEEFU || r1 != (uint32_t)(uintptr_t)&harr[2]) {
                return 124;
            }
        }
    }
#endif

#if defined(__thumb__)
    /*
     * 21. Regression test for Thumb-2 `ADD.W pc, pc, r0, lsl #2` jump table:
     * Verify writing an even address to PC via `add.w pc, pc, r0, lsl #2` stays
     * in Thumb state and does not interwork to A32 mode.
     */
    {
        static const uint32_t expected_jt[3] = { 11U, 22U, 33U };
        for (uint32_t idx = 0U; idx < 3U; idx++) {
            uint32_t jt_out = 0U;
            register uint32_t r0 __asm__("r0") = idx;
            __asm__ volatile(
                ".balign 4\n\t"
                ".inst.w 0xEB0F0F80\n\t"
                "b.w 1f\n\t"
                "b.w 2f\n\t"
                "b.w 3f\n"
                "1:\n\t"
                "movs %[out], #11\n\t"
                "b.w 4f\n"
                "2:\n\t"
                "movs %[out], #22\n\t"
                "b.w 4f\n"
                "3:\n\t"
                "movs %[out], #33\n"
                "4:\n\t"
                : [out] "=&r"(jt_out)
                : "r"(r0)
                : "cc"
            );
            if (jt_out != expected_jt[idx]) {
                return 125;
            }
        }
    }
#endif

    /*
     * 22. Barrel-shifter carry-out (C flag) on register-specified shifts
     *     (LSLS, LSRS, ASRS, RORS by register) for amount == 0 (preserving C),
     *     amount == 1..31, amount == 32, and amount > 32.
     */
    {
        uint32_t c_out = 0U;

        /* 22a: LSLS by register */
        if (arm_lsls_reg_c(0x12345678U, 0U, 1U, &c_out) != 0x12345678U || c_out != 1U ||
            arm_lsls_reg_c(0x12345678U, 0U, 0U, &c_out) != 0x12345678U || c_out != 0U ||
            arm_lsls_reg_c(0x12345678U, 256U, 1U, &c_out) != 0x12345678U || c_out != 1U ||
            arm_lsls_reg_c(0x12345678U, 256U, 0U, &c_out) != 0x12345678U || c_out != 0U ||
            arm_lsls_reg_c(0x40000001U, 1U, 1U, &c_out) != 0x80000002U || c_out != 0U ||
            arm_lsls_reg_c(0x40000001U, 2U, 0U, &c_out) != 0x00000004U || c_out != 1U ||
            arm_lsls_reg_c(0x00000003U, 31U, 0U, &c_out) != 0x80000000U || c_out != 1U ||
            arm_lsls_reg_c(0x00000001U, 31U, 1U, &c_out) != 0x80000000U || c_out != 0U ||
            arm_lsls_reg_c(0x12345671U, 32U, 0U, &c_out) != 0U || c_out != 1U ||
            arm_lsls_reg_c(0x12345670U, 32U, 1U, &c_out) != 0U || c_out != 0U ||
            arm_lsls_reg_c(0xFFFFFFFFU, 33U, 1U, &c_out) != 0U || c_out != 0U ||
            arm_lsls_reg_c(0xFFFFFFFFU, 64U, 1U, &c_out) != 0U || c_out != 0U) {
            return 126;
        }

        /* 22b: LSRS by register */
        if (arm_lsrs_reg_c(0x87654321U, 0U, 1U, &c_out) != 0x87654321U || c_out != 1U ||
            arm_lsrs_reg_c(0x87654321U, 0U, 0U, &c_out) != 0x87654321U || c_out != 0U ||
            arm_lsrs_reg_c(0x87654321U, 256U, 1U, &c_out) != 0x87654321U || c_out != 1U ||
            arm_lsrs_reg_c(0x87654321U, 256U, 0U, &c_out) != 0x87654321U || c_out != 0U ||
            arm_lsrs_reg_c(0x80000002U, 1U, 1U, &c_out) != 0x40000001U || c_out != 0U ||
            arm_lsrs_reg_c(0x80000002U, 2U, 0U, &c_out) != 0x20000000U || c_out != 1U ||
            arm_lsrs_reg_c(0x40000000U, 31U, 0U, &c_out) != 0U || c_out != 1U ||
            arm_lsrs_reg_c(0x80000000U, 31U, 1U, &c_out) != 1U || c_out != 0U ||
            arm_lsrs_reg_c(0x80000000U, 32U, 0U, &c_out) != 0U || c_out != 1U ||
            arm_lsrs_reg_c(0x7FFFFFFFU, 32U, 1U, &c_out) != 0U || c_out != 0U ||
            arm_lsrs_reg_c(0xFFFFFFFFU, 33U, 1U, &c_out) != 0U || c_out != 0U ||
            arm_lsrs_reg_c(0xFFFFFFFFU, 64U, 1U, &c_out) != 0U || c_out != 0U) {
            return 127;
        }

        /* 22c: ASRS by register */
        if (arm_asrs_reg_c(0x80000002U, 0U, 1U, &c_out) != 0x80000002U || c_out != 1U ||
            arm_asrs_reg_c(0x80000002U, 0U, 0U, &c_out) != 0x80000002U || c_out != 0U ||
            arm_asrs_reg_c(0x80000002U, 256U, 1U, &c_out) != 0x80000002U || c_out != 1U ||
            arm_asrs_reg_c(0x80000002U, 256U, 0U, &c_out) != 0x80000002U || c_out != 0U ||
            arm_asrs_reg_c(0x80000002U, 1U, 1U, &c_out) != 0xC0000001U || c_out != 0U ||
            arm_asrs_reg_c(0x80000002U, 2U, 0U, &c_out) != 0xE0000000U || c_out != 1U ||
            arm_asrs_reg_c(0x40000000U, 31U, 0U, &c_out) != 0U || c_out != 1U ||
            arm_asrs_reg_c(0x80000000U, 32U, 0U, &c_out) != 0xFFFFFFFFU || c_out != 1U ||
            arm_asrs_reg_c(0x80000000U, 33U, 0U, &c_out) != 0xFFFFFFFFU || c_out != 1U ||
            arm_asrs_reg_c(0x80000000U, 64U, 0U, &c_out) != 0xFFFFFFFFU || c_out != 1U ||
            arm_asrs_reg_c(0x7FFFFFFFU, 32U, 1U, &c_out) != 0U || c_out != 0U ||
            arm_asrs_reg_c(0x7FFFFFFFU, 33U, 1U, &c_out) != 0U || c_out != 0U) {
            return 128;
        }

        /* 22d: RORS by register */
        if (arm_rors_reg_c(0x80000001U, 0U, 1U, &c_out) != 0x80000001U || c_out != 1U ||
            arm_rors_reg_c(0x80000001U, 0U, 0U, &c_out) != 0x80000001U || c_out != 0U ||
            arm_rors_reg_c(0x80000001U, 256U, 1U, &c_out) != 0x80000001U || c_out != 1U ||
            arm_rors_reg_c(0x80000001U, 256U, 0U, &c_out) != 0x80000001U || c_out != 0U ||
            arm_rors_reg_c(0x00000001U, 1U, 0U, &c_out) != 0x80000000U || c_out != 1U ||
            arm_rors_reg_c(0x00000002U, 1U, 1U, &c_out) != 0x00000001U || c_out != 0U ||
            arm_rors_reg_c(0x12345678U, 8U, 1U, &c_out) != 0x78123456U || c_out != 0U ||
            arm_rors_reg_c(0x12345680U, 8U, 0U, &c_out) != 0x80123456U || c_out != 1U ||
            arm_rors_reg_c(0x80000001U, 32U, 0U, &c_out) != 0x80000001U || c_out != 1U ||
            arm_rors_reg_c(0x7FFFFFFFU, 32U, 1U, &c_out) != 0x7FFFFFFFU || c_out != 0U ||
            arm_rors_reg_c(0x00000001U, 33U, 0U, &c_out) != 0x80000000U || c_out != 1U ||
            arm_rors_reg_c(0x00000002U, 33U, 1U, &c_out) != 0x00000001U || c_out != 0U ||
            arm_rors_reg_c(0x80000001U, 64U, 0U, &c_out) != 0x80000001U || c_out != 1U ||
            arm_rors_reg_c(0x7FFFFFFFU, 64U, 1U, &c_out) != 0x7FFFFFFFU || c_out != 0U) {
            return 129;
        }

#if defined(__thumb__)
        /* 22e: T16 2-operand LSLS / LSRS / ASRS / RORS by register */
        if (t16_lsls_reg_c(0x12345671U, 0U, 1U, &c_out) != 0x12345671U || c_out != 1U ||
            t16_lsls_reg_c(0x12345671U, 0U, 0U, &c_out) != 0x12345671U || c_out != 0U ||
            t16_lsls_reg_c(0x40000001U, 2U, 0U, &c_out) != 0x00000004U || c_out != 1U ||
            t16_lsls_reg_c(0x12345671U, 32U, 0U, &c_out) != 0U || c_out != 1U ||
            t16_lsls_reg_c(0xFFFFFFFFU, 33U, 1U, &c_out) != 0U || c_out != 0U ||
            t16_lsrs_reg_c(0x80000002U, 0U, 1U, &c_out) != 0x80000002U || c_out != 1U ||
            t16_lsrs_reg_c(0x80000002U, 2U, 0U, &c_out) != 0x20000000U || c_out != 1U ||
            t16_lsrs_reg_c(0x80000000U, 32U, 0U, &c_out) != 0U || c_out != 1U ||
            t16_lsrs_reg_c(0xFFFFFFFFU, 33U, 1U, &c_out) != 0U || c_out != 0U ||
            t16_asrs_reg_c(0x80000002U, 0U, 0U, &c_out) != 0x80000002U || c_out != 0U ||
            t16_asrs_reg_c(0x80000002U, 2U, 0U, &c_out) != 0xE0000000U || c_out != 1U ||
            t16_asrs_reg_c(0x80000000U, 32U, 0U, &c_out) != 0xFFFFFFFFU || c_out != 1U ||
            t16_asrs_reg_c(0x80000000U, 33U, 0U, &c_out) != 0xFFFFFFFFU || c_out != 1U ||
            t16_rors_reg_c(0x80000001U, 0U, 0U, &c_out) != 0x80000001U || c_out != 0U ||
            t16_rors_reg_c(0x00000001U, 1U, 0U, &c_out) != 0x80000000U || c_out != 1U ||
            t16_rors_reg_c(0x80000001U, 32U, 0U, &c_out) != 0x80000001U || c_out != 1U ||
            t16_rors_reg_c(0x00000001U, 33U, 0U, &c_out) != 0x80000000U || c_out != 1U) {
            return 130;
        }
#endif
    }

    /*
     * 23. Regression tests for MSR APSR_nzcvq, Rm, MSR APSR_g, Rm (GE[3:0] bits [19:16]
     *     read back via SEL and MRS), and MSR APSR_nzcvqg, Rm.
     */
    {
        uint32_t sel_a = 0x11223344U;
        uint32_t sel_b = 0xAABBCCDDU;
        uint32_t sel_out = 0U;
        uint32_t flags = 0U;
        uint32_t c1 = 10U, c2 = 20U;

        /* 23a: MSR APSR_g, Rm with GE = 0b0101 (0x00050000) preserving Z=1, C=1 (0x60000000) */
        uint32_t ge_0101 = 0x00050000U;
        __asm__ volatile(
            "cmp %[c1], %[c1]\n\t"
            "msr apsr_g, %[ge]\n\t"
            "mrs %[fl], apsr\n\t"
            "sel %[out], %[sa], %[sb]\n\t"
            : [out] "=&r"(sel_out), [fl] "=&r"(flags)
            : [c1] "r"(c1), [ge] "r"(ge_0101), [sa] "r"(sel_a), [sb] "r"(sel_b)
            : "cc"
        );
        if (sel_out != 0xAA22CC44U || (flags & 0xF00F0000U) != 0x60050000U) {
            return 131;
        }

        /* 23b: MSR APSR_g, Rm with GE = 0b1010 (0x000A0000) preserving N=1 (0x80000000) */
        uint32_t ge_1010 = 0x000A0000U;
        __asm__ volatile(
            "cmp %[c1], %[c2]\n\t"
            "msr apsr_g, %[ge]\n\t"
            "mrs %[fl], apsr\n\t"
            "sel %[out], %[sa], %[sb]\n\t"
            : [out] "=&r"(sel_out), [fl] "=&r"(flags)
            : [c1] "r"(c1), [c2] "r"(c2), [ge] "r"(ge_1010), [sa] "r"(sel_a), [sb] "r"(sel_b)
            : "cc"
        );
        if (sel_out != 0x11BB33DDU || (flags & 0xF00F0000U) != 0x800A0000U) {
            return 132;
        }

        /* 23c: MSR APSR_nzcvq, Rm updates NZCV without clobbering GE[3:0] */
        uint32_t nzcv_a = 0xA0000000U; /* N=1, Z=0, C=1, V=0 */
        uint32_t nzcv_5 = 0x50000000U; /* N=0, Z=1, C=0, V=1 */
        __asm__ volatile(
            "msr apsr_g, %[ge]\n\t"
            "msr apsr_nzcvq, %[nzcv]\n\t"
            "mrs %[fl], apsr\n\t"
            "sel %[out], %[sa], %[sb]\n\t"
            : [out] "=&r"(sel_out), [fl] "=&r"(flags)
            : [ge] "r"(ge_0101), [nzcv] "r"(nzcv_a), [sa] "r"(sel_a), [sb] "r"(sel_b)
            : "cc"
        );
        if (sel_out != 0xAA22CC44U || (flags & 0xF00F0000U) != 0xA0050000U) {
            return 133;
        }
        __asm__ volatile(
            "msr apsr_nzcvq, %[nzcv]\n\t"
            "mrs %[fl], apsr\n\t"
            : [fl] "=&r"(flags)
            : [nzcv] "r"(nzcv_5)
            : "cc"
        );
        if ((flags & 0xF00F0000U) != 0x50050000U) {
            return 134;
        }

        /* 23d: MSR APSR_nzcvqg, Rm updates both NZCV and GE[3:0] */
        uint32_t both_9c = 0x900C0000U; /* N=1, Z=0, C=0, V=1, GE=0b1100 */
        __asm__ volatile(
            "msr apsr_nzcvqg, %[both]\n\t"
            "mrs %[fl], apsr\n\t"
            "sel %[out], %[sa], %[sb]\n\t"
            : [out] "=&r"(sel_out), [fl] "=&r"(flags)
            : [both] "r"(both_9c), [sa] "r"(sel_a), [sb] "r"(sel_b)
            : "cc"
        );
        if (sel_out != 0x1122CCDDU || (flags & 0xF00F0000U) != 0x900C0000U) {
            return 135;
        }
    }

    /*
     * 24. Regression tests for SSAT16, USAT16, and UADD8 / USUB8 + SEL:
     *     Verify dual 16-bit signed/unsigned saturation, parallel bytewise
     *     addition/subtraction GE[3:0] flags via SEL, and NZCV preservation.
     */
    {
        uint32_t res = 0U, flags = 0U;
        uint32_t c1 = 10U, c2 = 20U;

        /* 24a: SSAT16 #8 (signed range [-128, +127] = [0xFF80, 0x007F]) */
        uint32_t s16_in1 = 0x012CFED4U; /* hi = +300 -> +127 (0x007F), lo = -300 -> -128 (0xFF80) */
        __asm__ volatile(
            "cmp %[c1], %[c1]\n\t"
            "ssat16 %[res], #8, %[in]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [c1] "r"(c1), [in] "r"(s16_in1)
            : "cc"
        );
        if (res != 0x007FFF80U || (flags & 0xF0000000U) != 0x60000000U) {
            return 136;
        }

        uint32_t s16_in2 = 0xFFD600C8U; /* hi = -42 (in-range), lo = +200 -> +127 (0x007F) */
        __asm__ volatile(
            "cmp %[c1], %[c2]\n\t"
            "ssat16 %[res], #8, %[in]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [c1] "r"(c1), [c2] "r"(c2), [in] "r"(s16_in2)
            : "cc"
        );
        if (res != 0xFFD6007FU || (flags & 0xF0000000U) != 0x80000000U) {
            return 137;
        }

        /* 24b: USAT16 #8 (unsigned range [0, 255] = [0x0000, 0x00FF]) */
        uint32_t u16_in1 = 0xFFCE01F4U; /* hi = -50 -> 0, lo = +500 -> 255 (0x00FF) */
        __asm__ volatile(
            "cmp %[c1], %[c1]\n\t"
            "usat16 %[res], #8, %[in]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [c1] "r"(c1), [in] "r"(u16_in1)
            : "cc"
        );
        if (res != 0x000000FFU || (flags & 0xF0000000U) != 0x60000000U) {
            return 138;
        }

        uint32_t u16_in2 = 0x03E8002AU; /* hi = +1000 -> 255 (0x00FF), lo = +42 (in-range) */
        __asm__ volatile(
            "cmp %[c1], %[c2]\n\t"
            "usat16 %[res], #8, %[in]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [c1] "r"(c1), [c2] "r"(c2), [in] "r"(u16_in2)
            : "cc"
        );
        if (res != 0x00FF002AU || (flags & 0xF0000000U) != 0x80000000U) {
            return 139;
        }

        /* 24c: UADD8 + SEL (both A32 and T32) */
        uint32_t ua_rn = 0xF0208010U;
        uint32_t ua_rm = 0x203080F0U;
        uint32_t sel_a = 0x11223344U;
        uint32_t sel_b = 0xAABBCCDDU;
        uint32_t sel_out = 0U;
        __asm__ volatile(
            "cmp %[c1], %[c1]\n\t"
            "uadd8 %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            "sel %[so], %[sa], %[sb]\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags), [so] "=&r"(sel_out)
            : [c1] "r"(c1), [rn] "r"(ua_rn), [rm] "r"(ua_rm),
              [sa] "r"(sel_a), [sb] "r"(sel_b)
            : "cc"
        );
        if (res != 0x10500000U || sel_out != 0x11BB3344U ||
            (flags & 0xF00F0000U) != 0x600B0000U) {
            return 140;
        }

        /* 24d: USUB8 + SEL (both A32 and T32) */
        uint32_t us_rn = 0x10508020U;
        uint32_t us_rm = 0x20509010U;
        __asm__ volatile(
            "cmp %[c1], %[c2]\n\t"
            "usub8 %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            "sel %[so], %[sa], %[sb]\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags), [so] "=&r"(sel_out)
            : [c1] "r"(c1), [c2] "r"(c2), [rn] "r"(us_rn), [rm] "r"(us_rm),
              [sa] "r"(sel_a), [sb] "r"(sel_b)
            : "cc"
        );
        if (res != 0xF000F010U || sel_out != 0xAA22CC44U ||
            (flags & 0xF00F0000U) != 0x80050000U) {
            return 141;
        }
    }

    /*
     * 25. Regression tests for APSR.Q (sticky saturation bit 27):
     *     - Clear Q via `msr APSR_nzcvq, r0`, verify `mrs r1, apsr` has bit 27 == 0.
     *     - Verify non-saturating operations do not set Q (negative tests).
     *     - Trigger saturation in SSAT, USAT, SSAT16, USAT16, QADD, QSUB, QDADD, QDSUB,
     *       verifying each sets APSR.Q (bit 27 == 1) and that Q remains sticky across
     *       non-saturating instructions until cleared by `msr APSR_nzcvq`.
     */
    {
        const uint32_t q_bit = 1U << 27;
        uint32_t zero = 0U;
        uint32_t res = 0U;
        uint32_t flags = 0U;

        /* 25a: Direct clear and set of Q via MSR APSR_nzcvq */
        __asm__ volatile(
            "msr apsr_nzcvq, %[z]\n\t"
            "mrs %[fl], apsr\n\t"
            : [fl] "=&r"(flags)
            : [z] "r"(zero)
            : "cc"
        );
        if ((flags & q_bit) != 0U) {
            return 142;
        }
        __asm__ volatile(
            "msr apsr_nzcvq, %[qb]\n\t"
            "mrs %[fl], apsr\n\t"
            : [fl] "=&r"(flags)
            : [qb] "r"(q_bit)
            : "cc"
        );
        if ((flags & q_bit) != q_bit) {
            return 143;
        }

        /* 25b: SSAT (negative non-saturating + positive saturating + stickiness) */
        int32_t ssat_ok = 42;
        int32_t ssat_sat = 500;
        __asm__ volatile(
            "msr apsr_nzcvq, %[z]\n\t"
            "ssat %[res], #8, %[ok]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [z] "r"(zero), [ok] "r"(ssat_ok)
            : "cc"
        );
        if (res != 42U || (flags & q_bit) != 0U) {
            return 144;
        }
        __asm__ volatile(
            "msr apsr_nzcvq, %[z]\n\t"
            "ssat %[res], #8, %[sat]\n\t"
            "ssat %[res], #8, %[ok]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [z] "r"(zero), [sat] "r"(ssat_sat), [ok] "r"(ssat_ok)
            : "cc"
        );
        if (res != 42U || (flags & q_bit) != q_bit) {
            return 145;
        }

        /* 25c: USAT (negative non-saturating + positive saturating + negative input saturating) */
        int32_t usat_ok = 100;
        int32_t usat_sat = 500;
        int32_t usat_neg = -10;
        __asm__ volatile(
            "msr apsr_nzcvq, %[z]\n\t"
            "usat %[res], #8, %[ok]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [z] "r"(zero), [ok] "r"(usat_ok)
            : "cc"
        );
        if (res != 100U || (flags & q_bit) != 0U) {
            return 146;
        }
        __asm__ volatile(
            "msr apsr_nzcvq, %[z]\n\t"
            "usat %[res], #8, %[sat]\n\t"
            "usat %[res], #8, %[ok]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [z] "r"(zero), [sat] "r"(usat_sat), [ok] "r"(usat_ok)
            : "cc"
        );
        if (res != 100U || (flags & q_bit) != q_bit) {
            return 147;
        }
        __asm__ volatile(
            "msr apsr_nzcvq, %[z]\n\t"
            "usat %[res], #8, %[neg]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [z] "r"(zero), [neg] "r"(usat_neg)
            : "cc"
        );
        if (res != 0U || (flags & q_bit) != q_bit) {
            return 148;
        }

        /* 25d: SSAT16 (non-saturating vs saturating + sticky) */
        uint32_t ssat16_ok = 0x0020FFE0U;  /* hi = +32, lo = -32 (in [-128, 127]) */
        uint32_t ssat16_sat = 0x012C0010U; /* hi = +300 -> +127, lo = +16 */
        __asm__ volatile(
            "msr apsr_nzcvq, %[z]\n\t"
            "ssat16 %[res], #8, %[ok]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [z] "r"(zero), [ok] "r"(ssat16_ok)
            : "cc"
        );
        if (res != 0x0020FFE0U || (flags & q_bit) != 0U) {
            return 149;
        }
        __asm__ volatile(
            "msr apsr_nzcvq, %[z]\n\t"
            "ssat16 %[res], #8, %[sat]\n\t"
            "ssat16 %[res], #8, %[ok]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [z] "r"(zero), [sat] "r"(ssat16_sat), [ok] "r"(ssat16_ok)
            : "cc"
        );
        if (res != 0x0020FFE0U || (flags & q_bit) != q_bit) {
            return 150;
        }

        /* 25e: USAT16 (non-saturating vs saturating + sticky) */
        uint32_t usat16_ok = 0x00800040U;  /* hi = 128, lo = 64 (in [0, 255]) */
        uint32_t usat16_sat = 0x0010FFFEU; /* hi = 16, lo = -2 -> 0 */
        __asm__ volatile(
            "msr apsr_nzcvq, %[z]\n\t"
            "usat16 %[res], #8, %[ok]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [z] "r"(zero), [ok] "r"(usat16_ok)
            : "cc"
        );
        if (res != 0x00800040U || (flags & q_bit) != 0U) {
            return 151;
        }
        __asm__ volatile(
            "msr apsr_nzcvq, %[z]\n\t"
            "usat16 %[res], #8, %[sat]\n\t"
            "usat16 %[res], #8, %[ok]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [z] "r"(zero), [sat] "r"(usat16_sat), [ok] "r"(usat16_ok)
            : "cc"
        );
        if (res != 0x00800040U || (flags & q_bit) != q_bit) {
            return 152;
        }

        /* 25f: QADD, QSUB, QDADD, QDSUB setting APSR.Q and remaining sticky */
        int32_t q_100 = 100, q_200 = 200, q_50 = 50;
        int32_t q_pos_near = 0x7FFFFFF0;
        int32_t q_neg_near = (int32_t)0x80000010U;
        int32_t q_big_pos  = 0x50000000;

        /* QADD */
        __asm__ volatile(
            "msr apsr_nzcvq, %[z]\n\t"
            "qadd %[res], %[a], %[b]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [z] "r"(zero), [a] "r"(q_100), [b] "r"(q_200)
            : "cc"
        );
        if (res != 300U || (flags & q_bit) != 0U) {
            return 153;
        }
        __asm__ volatile(
            "msr apsr_nzcvq, %[z]\n\t"
            "qadd %[res], %[sat], %[a]\n\t"
            "qadd %[res], %[a], %[b]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [z] "r"(zero), [sat] "r"(q_pos_near), [a] "r"(q_100), [b] "r"(q_200)
            : "cc"
        );
        if (res != 300U || (flags & q_bit) != q_bit) {
            return 154;
        }

        /* QSUB */
        __asm__ volatile(
            "msr apsr_nzcvq, %[z]\n\t"
            "qsub %[res], %[b], %[a]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [z] "r"(zero), [a] "r"(q_100), [b] "r"(q_200)
            : "cc"
        );
        if (res != 100U || (flags & q_bit) != 0U) {
            return 155;
        }
        __asm__ volatile(
            "msr apsr_nzcvq, %[z]\n\t"
            "qsub %[res], %[neg], %[a]\n\t"
            "qsub %[res], %[b], %[a]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [z] "r"(zero), [neg] "r"(q_neg_near), [a] "r"(q_100), [b] "r"(q_200)
            : "cc"
        );
        if (res != 100U || (flags & q_bit) != q_bit) {
            return 156;
        }

        /* QDADD */
        __asm__ volatile(
            "msr apsr_nzcvq, %[z]\n\t"
            "qdadd %[res], %[a], %[b]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [z] "r"(zero), [a] "r"(q_100), [b] "r"(q_50)
            : "cc"
        );
        if (res != 200U || (flags & q_bit) != 0U) {
            return 157;
        }
        __asm__ volatile(
            "msr apsr_nzcvq, %[z]\n\t"
            "qdadd %[res], %[a], %[big]\n\t"
            "qdadd %[res], %[a], %[b]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [z] "r"(zero), [a] "r"(q_100), [b] "r"(q_50), [big] "r"(q_big_pos)
            : "cc"
        );
        if (res != 200U || (flags & q_bit) != q_bit) {
            return 158;
        }

        /* QDSUB */
        __asm__ volatile(
            "msr apsr_nzcvq, %[z]\n\t"
            "qdsub %[res], %[a], %[b]\n\t"
            "mrs %[fl], apsr\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [z] "r"(zero), [a] "r"(q_200), [b] "r"(q_50)
            : "cc"
        );
        if (res != 100U || (flags & q_bit) != 0U) {
            return 159;
        }
        __asm__ volatile(
            "msr apsr_nzcvq, %[z]\n\t"
            "qdsub %[res], %[z], %[big]\n\t"
            "qdsub %[res], %[a], %[b]\n\t"
            "mrs %[fl], apsr\n\t"
            "msr apsr_nzcvq, %[z]\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags)
            : [z] "r"(zero), [a] "r"(q_200), [b] "r"(q_50), [big] "r"(q_big_pos)
            : "cc"
        );
        if (res != 100U || (flags & q_bit) != q_bit) {
            return 160;
        }
    }

    /*
     * 26. Regression tests for UADD16 and USUB16 + SEL:
     *     Verify parallel 16-bit unsigned addition and subtraction setting
     *     GE[1:0] and GE[3:2] (read back via MRS and SEL) while preserving NZCV.
     */
    {
        uint32_t res = 0U, flags = 0U, sel_out = 0U;
        uint32_t c1 = 10U, c2 = 20U;
        uint32_t sel_a = 0x11223344U;
        uint32_t sel_b = 0xAABBCCDDU;

        /* 26a: UADD16 with hi carry, lo no carry -> GE = 0b1100 (0xC) */
        uint32_t ua16_rn1 = 0xF0001234U;
        uint32_t ua16_rm1 = 0x20002345U;
        __asm__ volatile(
            "cmp %[c1], %[c1]\n\t"
            "uadd16 %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            "sel %[so], %[sa], %[sb]\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags), [so] "=&r"(sel_out)
            : [c1] "r"(c1), [rn] "r"(ua16_rn1), [rm] "r"(ua16_rm1),
              [sa] "r"(sel_a), [sb] "r"(sel_b)
            : "cc"
        );
        if (res != 0x10003579U || sel_out != 0x1122CCDDU ||
            (flags & 0xF00F0000U) != 0x600C0000U) {
            return 161;
        }

        /* 26b: UADD16 with hi no carry, lo carry -> GE = 0b0011 (0x3) */
        uint32_t ua16_rn2 = 0x10008000U;
        uint32_t ua16_rm2 = 0x20009000U;
        __asm__ volatile(
            "cmp %[c1], %[c2]\n\t"
            "uadd16 %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            "sel %[so], %[sa], %[sb]\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags), [so] "=&r"(sel_out)
            : [c1] "r"(c1), [c2] "r"(c2), [rn] "r"(ua16_rn2), [rm] "r"(ua16_rm2),
              [sa] "r"(sel_a), [sb] "r"(sel_b)
            : "cc"
        );
        if (res != 0x30001000U || sel_out != 0xAABB3344U ||
            (flags & 0xF00F0000U) != 0x80030000U) {
            return 162;
        }

        /* 26c: USUB16 with hi borrow, lo no borrow -> GE = 0b0011 (0x3) */
        uint32_t us16_rn1 = 0x10005000U;
        uint32_t us16_rm1 = 0x20003000U;
        __asm__ volatile(
            "cmp %[c1], %[c1]\n\t"
            "usub16 %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            "sel %[so], %[sa], %[sb]\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags), [so] "=&r"(sel_out)
            : [c1] "r"(c1), [rn] "r"(us16_rn1), [rm] "r"(us16_rm1),
              [sa] "r"(sel_a), [sb] "r"(sel_b)
            : "cc"
        );
        if (res != 0xF0002000U || sel_out != 0xAABB3344U ||
            (flags & 0xF00F0000U) != 0x60030000U) {
            return 163;
        }

        /* 26d: USUB16 with hi equal (no borrow), lo borrow -> GE = 0b1100 (0xC) */
        uint32_t us16_rn2 = 0x40000001U;
        uint32_t us16_rm2 = 0x40000002U;
        __asm__ volatile(
            "cmp %[c1], %[c2]\n\t"
            "usub16 %[res], %[rn], %[rm]\n\t"
            "mrs %[fl], apsr\n\t"
            "sel %[so], %[sa], %[sb]\n\t"
            : [res] "=&r"(res), [fl] "=&r"(flags), [so] "=&r"(sel_out)
            : [c1] "r"(c1), [c2] "r"(c2), [rn] "r"(us16_rn2), [rm] "r"(us16_rm2),
              [sa] "r"(sel_a), [sb] "r"(sel_b)
            : "cc"
        );
        if (res != 0x0000FFFFU || sel_out != 0x1122CCDDU ||
            (flags & 0xF00F0000U) != 0x800C0000U) {
            return 164;
        }
    }

    /*
     * 27. Regression test for LDRD r0, r1, [r0] and LDM r0, {r0, r1} where
     *     base register rn is overwritten by the first loaded word.
     */
    {
        _Alignas(8) uint32_t pair[2] = { 0xDEADBEEFU, 0xCAFEBABEU };

        /* 27a: LDRD r0, r1, [r0] */
        {
            register uint32_t r0 __asm__("r0") = (uint32_t)(uintptr_t)&pair[0];
            register uint32_t r1 __asm__("r1") = 0U;
            __asm__ volatile("ldrd r0, r1, [r0]" : "+r"(r0), "=r"(r1) : : "memory");
            if (r0 != 0xDEADBEEFU || r1 != 0xCAFEBABEU) {
                return 165;
            }
        }

        /* 27b: LDM r0, {r0, r1} */
        pair[0] = 0x11223344U;
        pair[1] = 0x55667788U;
        {
            register uint32_t r0 __asm__("r0") = (uint32_t)(uintptr_t)&pair[0];
            register uint32_t r1 __asm__("r1") = 0U;
            __asm__ volatile("ldm r0, {r0, r1}" : "+r"(r0), "=r"(r1) : : "memory");
            if (r0 != 0x11223344U || r1 != 0x55667788U) {
                return 166;
            }
        }

#if defined(__thumb__)
        /* 27c: Thumb-2 32-bit LDM.W r0, {r0, r1} */
        pair[0] = 0x87654321U;
        pair[1] = 0xFEDCBA98U;
        {
            register uint32_t r0 __asm__("r0") = (uint32_t)(uintptr_t)&pair[0];
            register uint32_t r1 __asm__("r1") = 0U;
            __asm__ volatile("ldm.w r0, {r0, r1}" : "+r"(r0), "=r"(r1) : : "memory");
            if (r0 != 0x87654321U || r1 != 0xFEDCBA98U) {
                return 167;
            }
        }
#endif
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
