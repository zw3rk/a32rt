/*
 * src/ios/ios_stubs_time.c -- Zero-exit time & PID leaf stubs
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Native Thumb-2 leaf implementations for `_getpid`, `_mach_absolute_time`
 * and `_mach_timebase_info` emitted into the RX leaf window. The DBT
 * translates the leaf bodies inline, so each call runs with zero SVC exits.
 * `_getpid` reads the runtime PID slot (deterministic guest RAM, safe in
 * every mode including --difftest). The timer leaves lower `mrrc p15,1`
 * (CNTVCT) inline and are enabled only outside difftest mode on hosts
 * where the startup self-test proves `mach_absolute_time` tracks CNTVCT.
 * Curated C11, no VLAs.
 */

#if !defined(__APPLE__)
#define _GNU_SOURCE
#else
#define _DARWIN_C_SOURCE
#endif

#include "a32rt.h"
#include "ios/ios.h"
#include "vm/vm32.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#if defined(__APPLE__) && defined(__aarch64__)
#include <mach/mach_time.h>
#endif

#define PID_SLOT_OFF   0x0CU
#define TB_NUMER_OFF   0x10U
#define TB_DENOM_OFF   0x14U

static uint32_t g_pid_va = 0U;
static uint32_t g_abs_va = 0U;
static uint32_t g_tb_va = 0U;

void a32_ios_refresh_runtime_pid(a32_ctx_t *ctx)
{
    uint32_t pid;
    if (ctx == NULL || !ctx->is_ios) {
        return;
    }
    pid = (uint32_t)getpid();
    if (!a32_guest_range_ok(ctx, A32_IOS_RUNTIME_DATA_VA + PID_SLOT_OFF, 4U,
            (uint32_t)(PROT_READ | PROT_WRITE))) {
        return;
    }
    memcpy(a32_guest_ptr(ctx, A32_IOS_RUNTIME_DATA_VA + PID_SLOT_OFF), &pid, 4U);
}

/* Thumb-2 MOVW/MOVT emitters (same encoding as ios_commpage_stack.c). */
static void t_movw(uint16_t *out, size_t *pos, uint32_t rd, uint32_t imm16)
{
    uint32_t i = (imm16 >> 11) & 1U;
    uint32_t imm4 = (imm16 >> 12) & 0xFU;
    uint32_t imm3 = (imm16 >> 8) & 7U;
    uint32_t imm8 = imm16 & 0xFFU;
    out[(*pos)++] = (uint16_t)(0xF240U | (i << 10) | imm4);
    out[(*pos)++] = (uint16_t)((imm3 << 12) | (rd << 8) | imm8);
}

static void t_movt(uint16_t *out, size_t *pos, uint32_t rd, uint32_t imm16)
{
    uint32_t i = (imm16 >> 11) & 1U;
    uint32_t imm4 = (imm16 >> 12) & 0xFU;
    uint32_t imm3 = (imm16 >> 8) & 7U;
    uint32_t imm8 = imm16 & 0xFFU;
    out[(*pos)++] = (uint16_t)(0xF2C0U | (i << 10) | imm4);
    out[(*pos)++] = (uint16_t)((imm3 << 12) | (rd << 8) | imm8);
}

static bool t_emit(a32_ctx_t *ctx, uint32_t va, const uint16_t *halves, size_t n)
{
    size_t bytes = n * 2U;
    if (ctx == NULL || halves == NULL || n == 0U) {
        return false;
    }
    /* The leaf window lives inside the tramp mapping, which is RW at emit
     * time but only recorded on seal; copy directly without a range check. */
    memcpy(a32_guest_ptr(ctx, va), halves, bytes);
    return true;
}

#if defined(__APPLE__) && defined(__aarch64__)
static bool time_selftest_ok(uint32_t *out_numer, uint32_t *out_denom)
{
    mach_timebase_info_data_t tb;
    uint64_t c0 = 0ULL;
    uint64_t c1 = 0ULL;
    uint64_t t1 = 0ULL;
    if (out_numer == NULL || out_denom == NULL) {
        return false;
    }
    if (mach_timebase_info(&tb) != 0) {
        return false;
    }
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(c0));
    t1 = mach_absolute_time();
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(c1));
    if (!(c0 <= t1 && t1 <= c1)) {
        return false;
    }
    *out_numer = tb.numer;
    *out_denom = tb.denom;
    return true;
}
#endif

int a32_ios_emit_time_pid_leaf_stubs(a32_ctx_t *ctx, uint32_t *io_cur_va)
{
    uint32_t cur;
    uint16_t pid_code[8];
    size_t n = 0U;
    uint32_t pid_addr = A32_IOS_RUNTIME_DATA_VA + PID_SLOT_OFF;
    bool dry_run = (ctx == NULL);
#if defined(__APPLE__) && defined(__aarch64__)
    static bool s_mach_checked = false;
    static bool s_mach_ok = false;
    static uint32_t s_numer = 0U;
    static uint32_t s_denom = 0U;
    uint16_t abs_code[4];
    uint16_t tb_code[16];
    size_t tn = 0U;
#endif
    if (io_cur_va == NULL) {
        return -1;
    }
    if (!dry_run && !ctx->is_ios) {
        return 0;
    }
    cur = *io_cur_va & ~1U;
    if (cur < A32_IOS_LEAF_STUB_VA ||
        cur + 12U > A32_IOS_LEAF_STUB_VA + A32_IOS_LEAF_STUB_SIZE) {
        return -1;
    }
    t_movw(pid_code, &n, 0U, pid_addr & 0xFFFFU);
    t_movt(pid_code, &n, 0U, (pid_addr >> 16) & 0xFFFFU);
    pid_code[n++] = 0x6800U;
    pid_code[n++] = 0x4770U;
    if (!dry_run && !a32_guest_range_ok(ctx, pid_addr, 4U, (uint32_t)(PROT_READ | PROT_WRITE))) {
        g_pid_va = 0U;
        return -1;
    }
    if (!dry_run && !t_emit(ctx, cur, pid_code, n)) {
        return -1;
    }
    g_pid_va = cur | 1U;
    cur += (uint32_t)(n * 2U);
#if defined(__APPLE__) && defined(__aarch64__)
    if (!s_mach_checked) {
        s_mach_ok = time_selftest_ok(&s_numer, &s_denom);
        s_mach_checked = true;
    }
    if (s_mach_ok) {
        if (!dry_run &&
            !a32_guest_range_ok(ctx, A32_IOS_RUNTIME_DATA_VA + TB_NUMER_OFF, 8U,
                (uint32_t)(PROT_READ | PROT_WRITE))) {
            g_abs_va = 0U;
            g_tb_va = 0U;
            *io_cur_va = cur;
            return 0;
        }
        if (!dry_run &&
            a32_guest_range_ok(ctx, A32_IOS_RUNTIME_DATA_VA + TB_NUMER_OFF, 8U,
                (uint32_t)(PROT_READ | PROT_WRITE))) {
            memcpy(a32_guest_ptr(ctx, A32_IOS_RUNTIME_DATA_VA + TB_NUMER_OFF),
                &s_numer, 4U);
            memcpy(a32_guest_ptr(ctx, A32_IOS_RUNTIME_DATA_VA + TB_DENOM_OFF),
                &s_denom, 4U);
        }
        if (cur + 6U > A32_IOS_LEAF_STUB_VA + A32_IOS_LEAF_STUB_SIZE) {
            *io_cur_va = cur;
            return 0;
        }
        abs_code[0] = 0xEC51U;
        abs_code[1] = 0x0F1EU;
        abs_code[2] = 0x4770U;
        if (!dry_run && !t_emit(ctx, cur, abs_code, 3U)) {
            *io_cur_va = cur;
            return 0;
        }
        g_abs_va = cur | 1U;
        cur += 6U;
        if (cur + 28U <= A32_IOS_LEAF_STUB_VA + A32_IOS_LEAF_STUB_SIZE) {
            uint32_t tb_addr = A32_IOS_RUNTIME_DATA_VA + TB_NUMER_OFF;
            tn = 0U;
            tb_code[tn++] = 0x2800U;
            tb_code[tn++] = 0xD009U;
            t_movw(tb_code, &tn, 2U, tb_addr & 0xFFFFU);
            t_movt(tb_code, &tn, 2U, (tb_addr >> 16) & 0xFFFFU);
            tb_code[tn++] = 0x6811U;
            tb_code[tn++] = 0x6001U;
            tb_code[tn++] = 0x6851U;
            tb_code[tn++] = 0x6041U;
            tb_code[tn++] = 0x2000U;
            tb_code[tn++] = 0x4770U;
            tb_code[tn++] = 0x2004U;
            tb_code[tn++] = 0x4770U;
            if (dry_run || t_emit(ctx, cur, tb_code, tn)) {
                g_tb_va = cur | 1U;
                cur += (uint32_t)(tn * 2U);
            }
        }
    } else {
        g_abs_va = 0U;
        g_tb_va = 0U;
    }
#else
    (void)cur;
#endif
    *io_cur_va = cur;
    return 0;
}

uint32_t a32_ios_lookup_time_pid_leaf(const char *sym_name, bool *out_found)
{
    if (out_found != NULL) {
        *out_found = false;
    }
    if (sym_name == NULL) {
        return 0U;
    }
    if (strcmp(sym_name, "_getpid") == 0 || strcmp(sym_name, "___getpid") == 0) {
        if (g_pid_va != 0U) {
            if (out_found != NULL) {
                *out_found = true;
            }
            return g_pid_va;
        }
        return 0U;
    }
    if (strcmp(sym_name, "_mach_absolute_time") == 0) {
        if (g_abs_va != 0U) {
            if (out_found != NULL) {
                *out_found = true;
            }
            return g_abs_va;
        }
        return 0U;
    }
    if (strcmp(sym_name, "_mach_timebase_info") == 0) {
        if (g_tb_va != 0U) {
            if (out_found != NULL) {
                *out_found = true;
            }
            return g_tb_va;
        }
        return 0U;
    }
    return 0U;
}
