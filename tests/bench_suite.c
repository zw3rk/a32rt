/*
 * tests/bench_suite.c — Cross-architecture multi-workload benchmark suite
 *
 * Compiles freestanding (-ffreestanding -nostdlib) for:
 *   - armv7a-unknown-linux-eabihf (-marm A32 and -mthumb Thumb-2)
 *   - aarch64-unknown-linux-gnu   (native 64-bit AArch64 baseline)
 *   - x86_64-unknown-linux-gnu    (64-bit x86-64 for RosettaLinux & qemu-x86_64)
 *   - i686-unknown-linux-gnu      (32-bit x86 for qemu-i386)
 *
 * Workloads:
 *   1. coremark   : 2,000 iters of 4x4 matmul, 16-node list sort, state machine, CRC-16
 *   2. chacha20   : 8,000 blocks (512 KiB) of 20-round ChaCha20 ARX stream cipher
 *   3. vfp_simd   : 40,000 iters of 4x4 FP32 matmul + FP64 Mandelbrot kernel
 *   4. calls_ibtc : Recursive fib(28) + 200,000 indirect vtable calls
 *   5. syscalls   : 20,000 getpid() syscalls measuring raw SVC/syscall shim latency
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct bench_timespec64 {
    int64_t tv_sec;
    int64_t tv_nsec;
};

struct bench_timespec32 {
    int32_t tv_sec;
    int32_t tv_nsec;
};

#if defined(__aarch64__)
static inline int64_t sys0(uint64_t nr)
{
    register uint64_t x8 __asm__("x8") = nr;
    register uint64_t x0 __asm__("x0") = 0U;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory", "cc");
    return (int64_t)x0;
}

static inline int64_t sys1(uint64_t nr, uint64_t a0)
{
    register uint64_t x8 __asm__("x8") = nr;
    register uint64_t x0 __asm__("x0") = a0;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory", "cc");
    return (int64_t)x0;
}

static inline int64_t sys2(uint64_t nr, uint64_t a0, uint64_t a1)
{
    register uint64_t x8 __asm__("x8") = nr;
    register uint64_t x0 __asm__("x0") = a0;
    register uint64_t x1 __asm__("x1") = a1;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1) : "memory", "cc");
    return (int64_t)x0;
}

static inline int64_t sys3(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2)
{
    register uint64_t x8 __asm__("x8") = nr;
    register uint64_t x0 __asm__("x0") = a0;
    register uint64_t x1 __asm__("x1") = a1;
    register uint64_t x2 __asm__("x2") = a2;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory", "cc");
    return (int64_t)x0;
}

static void get_monotonic_time(struct bench_timespec64 *ts)
{
    sys2(113U /* SYS_clock_gettime */, 1U /* CLOCK_MONOTONIC */, (uintptr_t)ts);
}

static int32_t do_getpid(void)
{
    return (int32_t)sys0(172U /* SYS_getpid */);
}

static void do_write_stdout(const char *buf, size_t len)
{
    sys3(64U /* SYS_write */, 1U, (uintptr_t)buf, len);
}

static void __attribute__((noreturn)) do_exit_group(int rc)
{
    sys1(94U /* SYS_exit_group */, (uint64_t)rc);
    __builtin_unreachable();
}

__asm__(
    ".text\n"
    ".globl _start\n"
    ".type _start, %function\n"
    "_start:\n"
    "    mov x0, sp\n"
    "    and sp, x0, #~15\n"
    "    bl _start_c\n"
);

#elif defined(__x86_64__)
static inline int64_t sys0(uint64_t nr)
{
    int64_t ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(nr) : "rcx", "r11", "memory", "cc");
    return ret;
}

static inline int64_t sys1(uint64_t nr, uint64_t a0)
{
    int64_t ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(nr), "D"(a0) : "rcx", "r11", "memory", "cc");
    return ret;
}

static inline int64_t sys2(uint64_t nr, uint64_t a0, uint64_t a1)
{
    int64_t ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a0), "S"(a1)
                     : "rcx", "r11", "memory", "cc");
    return ret;
}

static inline int64_t sys3(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2)
{
    int64_t ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(nr), "D"(a0), "S"(a1), "d"(a2)
                     : "rcx", "r11", "memory", "cc");
    return ret;
}

static void get_monotonic_time(struct bench_timespec64 *ts)
{
    sys2(228U /* SYS_clock_gettime */, 4U /* CLOCK_MONOTONIC_RAW */, (uintptr_t)ts);
}

static int32_t do_getpid(void)
{
    return (int32_t)sys0(39U /* SYS_getpid */);
}

static void do_write_stdout(const char *buf, size_t len)
{
    sys3(1U /* SYS_write */, 1U, (uintptr_t)buf, len);
}

static void __attribute__((noreturn)) do_exit_group(int rc)
{
    sys1(231U /* SYS_exit_group */, (uint64_t)rc);
    __builtin_unreachable();
}

__asm__(
    ".text\n"
    ".globl _start\n"
    ".type _start, @function\n"
    "_start:\n"
    "    andq $-16, %rsp\n"
    "    call _start_c\n"
);

#elif defined(__i386__)
static inline int32_t sys0(uint32_t nr)
{
    int32_t ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(nr) : "memory", "cc");
    return ret;
}

static inline int32_t sys1(uint32_t nr, uint32_t a0)
{
    int32_t ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(nr), "b"(a0) : "memory", "cc");
    return ret;
}

static inline int32_t sys2(uint32_t nr, uint32_t a0, uint32_t a1)
{
    int32_t ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(nr), "b"(a0), "c"(a1) : "memory", "cc");
    return ret;
}

static inline int32_t sys3(uint32_t nr, uint32_t a0, uint32_t a1, uint32_t a2)
{
    int32_t ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(nr), "b"(a0), "c"(a1), "d"(a2)
                     : "memory", "cc");
    return ret;
}

static void get_monotonic_time(struct bench_timespec64 *ts)
{
    int32_t r = sys2(403U /* clock_gettime64 */, 1U, (uint32_t)(uintptr_t)ts);
    if (r != 0) {
        struct bench_timespec32 ts32 = { 0, 0 };
        sys2(265U /* clock_gettime32 */, 1U, (uint32_t)(uintptr_t)&ts32);
        ts->tv_sec  = (int64_t)ts32.tv_sec;
        ts->tv_nsec = (int64_t)ts32.tv_nsec;
    }
}

static int32_t do_getpid(void)
{
    return sys0(20U /* SYS_getpid */);
}

static void do_write_stdout(const char *buf, size_t len)
{
    sys3(4U /* SYS_write */, 1U, (uint32_t)(uintptr_t)buf, (uint32_t)len);
}

static void __attribute__((noreturn)) do_exit_group(int rc)
{
    sys1(252U /* SYS_exit_group */, (uint32_t)rc);
    __builtin_unreachable();
}

__asm__(
    ".text\n"
    ".globl _start\n"
    ".type _start, @function\n"
    "_start:\n"
    "    andl $-16, %esp\n"
    "    call _start_c\n"
);

#else /* __arm__ (A32 / Thumb-2) */
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

static inline int32_t sys0(uint32_t nr)
{
    register uint32_t r7 __asm__("r7") = nr;
    register uint32_t r0 __asm__("r0") = 0U;
    __asm__ volatile("svc #0" : "+r"(r0) : "r"(r7) : "memory", "cc");
    return (int32_t)r0;
}

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

static void get_monotonic_time(struct bench_timespec64 *ts)
{
    int32_t r = sys2(403U /* clock_gettime64 */, 1U, (uint32_t)(uintptr_t)ts);
    if (r != 0) {
        struct bench_timespec32 ts32 = { 0, 0 };
        sys2(263U /* clock_gettime32 */, 1U, (uint32_t)(uintptr_t)&ts32);
        ts->tv_sec  = (int64_t)ts32.tv_sec;
        ts->tv_nsec = (int64_t)ts32.tv_nsec;
    }
}

static int32_t do_getpid(void)
{
    return sys0(20U /* SYS_getpid */);
}

static void do_write_stdout(const char *buf, size_t len)
{
    sys3(4U /* SYS_write */, 1U, (uint32_t)(uintptr_t)buf, (uint32_t)len);
}

static void __attribute__((noreturn)) do_exit_group(int rc)
{
    sys1(248U /* SYS_exit_group */, (uint32_t)rc);
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
#endif

/* ========================================================================= */
/* Output Formatting Helpers                                                 */
/* ========================================================================= */

static size_t append_str(char *buf, size_t pos, const char *s)
{
    while (*s != '\0') {
        buf[pos++] = *s++;
    }
    return pos;
}

static size_t append_u32(char *buf, size_t pos, uint32_t val)
{
    char tmp[12];
    size_t n = 0U;
    if (val == 0U) {
        buf[pos++] = '0';
        return pos;
    }
    while (val > 0U) {
        tmp[n++] = (char)('0' + (val % 10U));
        val /= 10U;
    }
    while (n > 0U) {
        buf[pos++] = tmp[--n];
    }
    return pos;
}

static size_t append_hex32(char *buf, size_t pos, uint32_t val)
{
    static const char hex[] = "0123456789abcdef";
    buf[pos++] = '0';
    buf[pos++] = 'x';
    for (int i = 7; i >= 0; i--) {
        buf[pos++] = hex[(val >> (i * 4)) & 0xFU];
    }
    return pos;
}

static uint32_t elapsed_us_between(const struct bench_timespec64 *t0,
                                   const struct bench_timespec64 *t1)
{
    uint32_t sec_diff  = (uint32_t)(t1->tv_sec - t0->tv_sec);
    int32_t  nsec_diff = (int32_t)(t1->tv_nsec - t0->tv_nsec);
    if (nsec_diff < 0) {
        sec_diff--;
        nsec_diff += 1000000000;
    }
    uint32_t us = sec_diff * 1000000U + ((uint32_t)nsec_diff / 1000U);
    return (us == 0U) ? 1U : us;
}

static void report_bench(const char *name, uint32_t iters, uint32_t us,
                         uint32_t crc, bool ok)
{
    uint32_t ops_sec;
    if (iters <= 4000U) {
        ops_sec = (iters * 1000000U) / us;
    } else if (us >= 1000U) {
        ops_sec = (iters * 1000U) / (us / 1000U);
    } else {
        ops_sec = (iters * 1000U) / ((us == 0U) ? 1U : us) * 1000U;
    }
    char line[128];
    size_t pos = 0U;
    pos = append_str(line, pos, "BENCH ");
    pos = append_str(line, pos, name);
    pos = append_str(line, pos, " us=");
    pos = append_u32(line, pos, us);
    pos = append_str(line, pos, " ops_sec=");
    pos = append_u32(line, pos, ops_sec);
    pos = append_str(line, pos, " crc=");
    pos = append_hex32(line, pos, crc);
    pos = append_str(line, pos, ok ? " PASS\n" : " FAIL\n");
    do_write_stdout(line, pos);
}

/* ========================================================================= */
/* Workload 1: CoreMark-style Integer Control, List Sort, MatMul & Tokenizer */
/* ========================================================================= */

typedef struct list_node {
    struct list_node *next;
    uint32_t val;
} list_node_t;

static uint16_t crc16_update(uint16_t crc, uint8_t byte)
{
    crc = (uint16_t)(crc ^ (uint16_t)((uint16_t)byte << 8));
    for (int i = 0; i < 8; i++) {
        if ((crc & 0x8000U) != 0U) {
            crc = (uint16_t)((uint16_t)(crc << 1) ^ 0x1021U);
        } else {
            crc = (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static int bench_coremark(uint32_t *out_us)
{
    const uint32_t iters = 2000U;
    uint32_t min_us = 0xFFFFFFFFU;
    uint16_t crc = 0xFFFFU;
    bool all_ok = true;

    for (uint32_t trial = 0U; trial < 5U; trial++) {
        struct bench_timespec64 t0, t1;
        get_monotonic_time(&t0);

        crc = 0xFFFFU;
        for (uint32_t it = 0U; it < iters; it++) {
            int32_t A[4][4];
            int32_t B[4][4];
            for (int i = 0; i < 4; i++) {
                for (int j = 0; j < 4; j++) {
                    A[i][j] = (int32_t)((it * 17U + (uint32_t)i * 5U + (uint32_t)j) & 0xFFU);
                    B[i][j] = (int32_t)((it * 13U + (uint32_t)j * 7U + (uint32_t)i) & 0xFFU);
                }
            }
            uint32_t c_sum = 0U;
            for (int i = 0; i < 4; i++) {
                for (int j = 0; j < 4; j++) {
                    int32_t acc = 0;
                    for (int k = 0; k < 4; k++) {
                        acc += A[i][k] * B[k][j];
                    }
                    c_sum += (uint32_t)acc;
                }
            }

            list_node_t nodes[16];
            for (uint32_t k = 0U; k < 16U; k++) {
                nodes[k].next = (k + 1U < 16U) ? &nodes[k + 1U] : NULL;
                nodes[k].val  = (it * 31U + k * 19U + 7U) & 0xFFU;
            }
            for (int pass = 0; pass < 16; pass++) {
                list_node_t *curr = &nodes[0];
                while (curr != NULL && curr->next != NULL) {
                    if (curr->val > curr->next->val) {
                        uint32_t tmp    = curr->val;
                        curr->val       = curr->next->val;
                        curr->next->val = tmp;
                    }
                    curr = curr->next;
                }
            }
            uint32_t l_sum = 0U;
            list_node_t *p = &nodes[0];
            while (p != NULL) {
                l_sum += p->val;
                p = p->next;
            }

            static const char text[] = "int a = 123 + 456; /* test */ if (a > 0) a = a - 1;";
            uint32_t tokens = 0U;
            int state = 0;
            for (size_t s = 0U; s < sizeof(text) - 1U; s++) {
                char ch = text[s];
                if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
                    state = 0;
                } else if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')) {
                    if (state != 1) {
                        tokens++;
                        state = 1;
                    }
                } else if (ch >= '0' && ch <= '9') {
                    if (state != 2) {
                        tokens++;
                        state = 2;
                    }
                } else {
                    tokens++;
                    state = 3;
                }
            }

            crc = crc16_update(crc, (uint8_t)(c_sum & 0xFFU));
            crc = crc16_update(crc, (uint8_t)((c_sum >> 8) & 0xFFU));
            crc = crc16_update(crc, (uint8_t)((c_sum >> 16) & 0xFFU));
            crc = crc16_update(crc, (uint8_t)((c_sum >> 24) & 0xFFU));
            crc = crc16_update(crc, (uint8_t)(l_sum & 0xFFU));
            crc = crc16_update(crc, (uint8_t)((l_sum >> 8) & 0xFFU));
            crc = crc16_update(crc, (uint8_t)(tokens & 0xFFU));
        }

        get_monotonic_time(&t1);
        if (crc != 0x41ddU) {
            all_ok = false;
        }
        if (trial >= 1U) {
            uint32_t us = elapsed_us_between(&t0, &t1);
            if (us < min_us) {
                min_us = us;
            }
        }
    }

    *out_us = min_us;
    report_bench("coremark", iters, min_us, (uint32_t)crc, all_ok);
    return all_ok ? 0 : 1;
}

/* ========================================================================= */
/* Workload 2: ChaCha20 20-Round ARX Stream Cipher (8,000 blocks = 512 KiB)  */
/* ========================================================================= */

static inline uint32_t rotl32(uint32_t x, unsigned n)
{
    return (x << n) | (x >> (32U - n));
}

#define CHACHA_QR(a, b, c, d)             \
    do {                                  \
        (a) += (b);                       \
        (d) ^= (a);                       \
        (d) = rotl32((d), 16U);           \
        (c) += (d);                       \
        (b) ^= (c);                       \
        (b) = rotl32((b), 12U);           \
        (a) += (b);                       \
        (d) ^= (a);                       \
        (d) = rotl32((d), 8U);            \
        (c) += (d);                       \
        (b) ^= (c);                       \
        (b) = rotl32((b), 7U);            \
    } while (0)

static int bench_chacha20(uint32_t *out_us)
{
    const uint32_t blocks = 8000U;
    uint32_t min_us = 0xFFFFFFFFU;
    uint32_t acc = 0U;
    bool all_ok = true;

    for (uint32_t trial = 0U; trial < 5U; trial++) {
        struct bench_timespec64 t0, t1;
        get_monotonic_time(&t0);

        acc = 0U;
        for (uint32_t blk = 0U; blk < blocks; blk++) {
            uint32_t x0  = 0x61707865U, x1  = 0x3320646eU, x2  = 0x79622d32U, x3  = 0x6b206574U;
            uint32_t x4  = 0x03020100U, x5  = 0x07060504U, x6  = 0x0b0a0908U, x7  = 0x0f0e0d0cU;
            uint32_t x8  = 0x13121110U, x9  = 0x17161514U, x10 = 0x1b1a1918U, x11 = 0x1f1e1d1cU;
            uint32_t x12 = blk ^ acc,   x13 = 0x00000000U, x14 = 0x4a000000U, x15 = 0x00000000U;

            for (int r = 0; r < 10; r++) {
                CHACHA_QR(x0, x4,  x8, x12);
                CHACHA_QR(x1, x5,  x9, x13);
                CHACHA_QR(x2, x6, x10, x14);
                CHACHA_QR(x3, x7, x11, x15);
                CHACHA_QR(x0, x5, x10, x15);
                CHACHA_QR(x1, x6, x11, x12);
                CHACHA_QR(x2, x7,  x8, x13);
                CHACHA_QR(x3, x4,  x9, x14);
            }
            acc ^= (x0 + x4 + x8 + x12) ^ (x3 + x7 + x11 + x15);
        }

        get_monotonic_time(&t1);
        if (acc != 0x065fe64fU) {
            all_ok = false;
        }
        if (trial >= 1U) {
            uint32_t us = elapsed_us_between(&t0, &t1);
            if (us < min_us) {
                min_us = us;
            }
        }
    }

    *out_us = min_us;
    report_bench("chacha20", blocks, min_us, acc, all_ok);
    return all_ok ? 0 : 1;
}

/* ========================================================================= */
/* Workload 3: VFPv4 / NEON Floating-Point & SIMD (40,000 iters)             */
/* ========================================================================= */

#if defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif

static int bench_vfp_simd(uint32_t *out_us)
{
    const uint32_t iters = 40000U;
    uint32_t min_us = 0xFFFFFFFFU;
    uint32_t chk = 0U;
    bool all_ok = true;

    for (uint32_t trial = 0U; trial < 5U; trial++) {
        struct bench_timespec64 t0, t1;
        get_monotonic_time(&t0);

        float  mat_a[16] = {
            1.25f, 0.50f, -0.75f, 2.00f,
            0.25f, 1.50f,  0.50f, -1.00f,
           -0.50f, 0.75f,  1.25f, 0.25f,
            1.00f, -0.25f, 0.50f, 1.75f
        };
        float  vec_x[4] = { 1.0f, 2.0f, 3.0f, 4.0f };
        double m_acc    = 0.0;

        for (uint32_t it = 0U; it < iters; it++) {
            uint32_t step = it;
            __asm__ volatile("" : "+r"(step));
            float bias = (float)(step & 7U) * 0.015625f;
            float out[4];
            for (int r = 0; r < 4; r++) {
                out[r] = mat_a[r * 4 + 0] * vec_x[0] +
                         mat_a[r * 4 + 1] * vec_x[1] +
                         mat_a[r * 4 + 2] * vec_x[2] +
                         mat_a[r * 4 + 3] * vec_x[3] + bias;
            }
            vec_x[0] = out[0] * 0.25f;
            vec_x[1] = out[1] * 0.25f;
            vec_x[2] = out[2] * 0.25f;
            vec_x[3] = out[3] * 0.25f;

            /* 4-iteration FP64 Mandelbrot orbit step */
            double cr = -0.75 + (double)(step & 15U) * 0.00390625;
            double ci =  0.25;
            double zr = cr, zi = ci;
            for (int k = 0; k < 4; k++) {
                double zr2 = zr * zr - zi * zi + cr;
                double zi2 = 2.0 * zr * zi + ci;
                zr = zr2;
                zi = zi2;
            }
            m_acc += zr + zi;
        }

        get_monotonic_time(&t1);
        chk = (uint32_t)(int32_t)(m_acc * 16.0) ^
              (uint32_t)(int32_t)(vec_x[0] * 1024.0f);
        if (chk != 0xfffdaf18U) {
            all_ok = false;
        }
        if (trial >= 1U) {
            uint32_t us = elapsed_us_between(&t0, &t1);
            if (us < min_us) {
                min_us = us;
            }
        }
    }

    *out_us = min_us;
    report_bench("vfp_simd", iters, min_us, chk, all_ok);
    return all_ok ? 0 : 1;
}

/* ========================================================================= */
/* Workload 4: Calls, Recursion & Indirect Branching (fib(28) + vtable)      */
/* ========================================================================= */

static uint32_t __attribute__((noinline)) fib_rec(uint32_t n)
{
    __asm__ volatile("" : "+r"(n));
    if (n < 2U) {
        return n;
    }
    return fib_rec(n - 1U) + fib_rec(n - 2U);
}

typedef uint32_t (*vop_fn)(uint32_t x, uint32_t y);

static uint32_t __attribute__((noinline)) vop_add(uint32_t x, uint32_t y)
{
    return x + (y ^ 0x9e3779b9U);
}

static uint32_t __attribute__((noinline)) vop_xor(uint32_t x, uint32_t y)
{
    return (x ^ y) + 0x7f4a7c15U;
}

static uint32_t __attribute__((noinline)) vop_rol(uint32_t x, uint32_t y)
{
    return rotl32(x + y, 7U);
}

static uint32_t __attribute__((noinline)) vop_mul(uint32_t x, uint32_t y)
{
    return (x * 1664525U) + y + 1013904223U;
}

static int bench_calls_ibtc(uint32_t *out_us)
{
    const uint32_t vtable_iters = 200000U;
    uint32_t min_us = 0xFFFFFFFFU;
    uint32_t state = 0U;
    bool all_ok = true;

    for (uint32_t trial = 0U; trial < 5U; trial++) {
        struct bench_timespec64 t0, t1;
        get_monotonic_time(&t0);

        uint32_t f28 = fib_rec(28U); /* 317811 (1,028,457 recursive calls) */

        vop_fn table[4] = { vop_add, vop_xor, vop_rol, vop_mul };
        __asm__ volatile("" : "+m"(table));

        state = f28;
        for (uint32_t i = 0U; i < vtable_iters; i++) {
            vop_fn fn = table[(state ^ i) & 3U];
            state = fn(state, i);
        }

        get_monotonic_time(&t1);
        if (f28 != 317811U || state != 0xed9c5a0aU) {
            all_ok = false;
        }
        if (trial >= 1U) {
            uint32_t us = elapsed_us_between(&t0, &t1);
            if (us < min_us) {
                min_us = us;
            }
        }
    }

    *out_us = min_us;
    report_bench("calls_ibtc", 1028457U + vtable_iters, min_us, state, all_ok);
    return all_ok ? 0 : 1;
}

/* ========================================================================= */
/* Workload 5: Syscall ABI Shim Throughput (4,000 getpid syscalls)           */
/* ========================================================================= */

static int bench_syscalls(uint32_t *out_us)
{
    const uint32_t iters = 4000U;
    uint32_t min_us = 0xFFFFFFFFU;
    uint32_t acc = 0U;
    bool all_ok = true;

    for (uint32_t trial = 0U; trial < 5U; trial++) {
        struct bench_timespec64 t0, t1;
        get_monotonic_time(&t0);

        acc = 0U;
        int32_t first_pid = do_getpid();
        for (uint32_t i = 0U; i < iters; i++) {
            int32_t p = do_getpid();
            if (p == first_pid) {
                acc++;
            }
        }

        get_monotonic_time(&t1);
        if (acc != iters || first_pid <= 0) {
            all_ok = false;
        }
        if (trial >= 1U) {
            uint32_t us = elapsed_us_between(&t0, &t1);
            if (us < min_us) {
                min_us = us;
            }
        }
    }

    *out_us = min_us;
    report_bench("syscalls", iters, min_us, acc, all_ok);
    return all_ok ? 0 : 1;
}

void __attribute__((noreturn)) _start_c(void)
{
    uint32_t us_cm = 0U, us_cc = 0U, us_fp = 0U, us_cl = 0U, us_sc = 0U;
    int rc = 0;
    rc |= bench_coremark(&us_cm);
    rc |= bench_chacha20(&us_cc);
    rc |= bench_vfp_simd(&us_fp);
    rc |= bench_calls_ibtc(&us_cl);
    rc |= bench_syscalls(&us_sc);

    uint32_t total_us = us_cm + us_cc + us_fp + us_cl + us_sc;
    char msg[96];
    size_t pos = 0U;
    pos = append_str(msg, pos, "BENCH total_us=");
    pos = append_u32(msg, pos, total_us);
    pos = append_str(msg, pos, (rc == 0) ? " ALL_PASS\n" : " FAILED\n");
    do_write_stdout(msg, pos);
    do_exit_group(rc);
}
