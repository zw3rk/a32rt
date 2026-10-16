/*
 * tests/bench_coremark.c — M2 / M2-hl CoreMark-style benchmark fixture
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stddef.h>

#define COREMARK_ITERATIONS 2000U
#define EXPECTED_CRC 0x41ddU

struct arm_timespec64 {
    int64_t tv_sec;
    int64_t tv_nsec;
};

struct arm_timespec32 {
    int32_t tv_sec;
    int32_t tv_nsec;
};

typedef struct list_node {
    struct list_node *next;
    uint32_t val;
} list_node_t;

#if defined(__aarch64__)
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
#else
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
#endif

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

static void get_monotonic_time(struct arm_timespec64 *ts)
{
#if defined(__aarch64__)
    sys2(113U /* SYS_clock_gettime */, 1U /* CLOCK_MONOTONIC */, (uintptr_t)ts);
#else
    int32_t r = sys2(403U /* clock_gettime64 */, 1U /* CLOCK_MONOTONIC */, (uint32_t)(uintptr_t)ts);
    if (r != 0) {
        struct arm_timespec32 ts32;
        ts32.tv_sec = 0;
        ts32.tv_nsec = 0;
        sys2(263U /* clock_gettime32 */, 1U, (uint32_t)(uintptr_t)&ts32);
        ts->tv_sec = (int64_t)ts32.tv_sec;
        ts->tv_nsec = (int64_t)ts32.tv_nsec;
    }
#endif
}

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

static int run_coremark(void)
{
    struct arm_timespec64 ts_start, ts_end;
    get_monotonic_time(&ts_start);

    uint16_t crc = 0xFFFFU;

    for (uint32_t it = 0U; it < COREMARK_ITERATIONS; it++) {
        /* 1. 4x4 integer matrix multiply */
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

        /* 2. Linked-list search & sort (16 elements) */
        list_node_t nodes[16];
        for (uint32_t k = 0U; k < 16U; k++) {
            nodes[k].next = (k + 1U < 16U) ? &nodes[k + 1U] : NULL;
            nodes[k].val  = (it * 31U + k * 19U + 7U) & 0xFFU;
        }
        for (int pass = 0; pass < 16; pass++) {
            list_node_t *curr = &nodes[0];
            while (curr != NULL && curr->next != NULL) {
                if (curr->val > curr->next->val) {
                    uint32_t tmp = curr->val;
                    curr->val = curr->next->val;
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

        /* 3. State machine tokenization */
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

        /* 4. Update CRC-16 with iteration results */
        crc = crc16_update(crc, (uint8_t)(c_sum & 0xFFU));
        crc = crc16_update(crc, (uint8_t)((c_sum >> 8) & 0xFFU));
        crc = crc16_update(crc, (uint8_t)((c_sum >> 16) & 0xFFU));
        crc = crc16_update(crc, (uint8_t)((c_sum >> 24) & 0xFFU));

        crc = crc16_update(crc, (uint8_t)(l_sum & 0xFFU));
        crc = crc16_update(crc, (uint8_t)((l_sum >> 8) & 0xFFU));

        crc = crc16_update(crc, (uint8_t)(tokens & 0xFFU));
    }

    get_monotonic_time(&ts_end);

    if (crc != EXPECTED_CRC) {
        return 1;
    }

    uint32_t sec_diff = (uint32_t)(ts_end.tv_sec - ts_start.tv_sec);
    int32_t nsec_diff = (int32_t)(ts_end.tv_nsec - ts_start.tv_nsec);
    if (nsec_diff < 0) {
        sec_diff--;
        nsec_diff += 1000000000;
    }
    uint32_t elapsed_us = sec_diff * 1000000U + ((uint32_t)nsec_diff / 1000U);
    if (elapsed_us == 0U) {
        elapsed_us = 1U;
    }
    uint32_t iter_per_sec = (COREMARK_ITERATIONS * 1000000U) / elapsed_us;

    char msg[96];
    size_t pos = 0U;
    pos = append_str(msg, pos, "CoreMark CRC: PASS (iters=");
    pos = append_u32(msg, pos, COREMARK_ITERATIONS);
    pos = append_str(msg, pos, " elapsed_us=");
    pos = append_u32(msg, pos, elapsed_us);
    pos = append_str(msg, pos, " iter_per_sec=");
    pos = append_u32(msg, pos, iter_per_sec);
    pos = append_str(msg, pos, ")\n");

#if defined(__aarch64__)
    sys3(64U /* SYS_write */, 1U, (uintptr_t)msg, pos);
#else
    sys3(4U /* write */, 1U, (uint32_t)(uintptr_t)msg, (uint32_t)pos);
#endif
    return 0;
}

void __attribute__((noreturn)) _start_c(void)
{
    int rc = run_coremark();
#if defined(__aarch64__)
    sys1(94U /* SYS_exit_group */, (uint64_t)rc);
#else
    sys1(248U /* exit_group */, (uint32_t)rc);
#endif
    __builtin_unreachable();
}

#if defined(__aarch64__)
__asm__(
    ".text\n"
    ".globl _start\n"
    ".type _start, %function\n"
    "_start:\n"
    "    mov x0, sp\n"
    "    and sp, x0, #~15\n"
    "    bl _start_c\n"
);
#else
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
