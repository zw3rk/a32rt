/*
 * src/host.c — Optional /dev/a32host handshake & capability detection
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#define _GNU_SOURCE
#include "a32rt.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

uint64_t a32_host_monotonic_ns(void)
{
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

void a32_print_stats(const a32_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    fprintf(stderr,
        "[a32rt stats] blocks_compiled=%llu blocks_executed=%llu chain_hits=%llu "
        "ibtc_hits=%llu ras_hits=%llu flags_elided=%llu syscalls=%llu "
        "icache_flushes=%llu wx_transitions=%llu window=0x%llx\n",
        (unsigned long long)ctx->blocks_compiled,
        (unsigned long long)ctx->blocks_executed,
        (unsigned long long)ctx->chain_hits,
        (unsigned long long)ctx->ibtc_hits,
        (unsigned long long)ctx->ras_hits,
        (unsigned long long)ctx->flags_elided,
        (unsigned long long)ctx->syscalls_executed,
        (unsigned long long)ctx->icache_flushes,
        (unsigned long long)ctx->wx_transitions,
        (unsigned long long)ctx->guest_base);
    fprintf(stderr,
        "[a32rt stats-ext] ns_vm_init=%llu ns_translate=%llu ns_wx_flush=%llu "
        "aprr_toggles=%llu tlb_checks=%llu tlb_groups=%llu tlb_slow=%llu "
        "vfp_copies=%llu fpcr_sw=%llu fpcr_slow=%llu aot_abs64=%llu "
        "exits=%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
        (unsigned long long)ctx->ns_vm_init,
        (unsigned long long)ctx->ns_translate,
        (unsigned long long)ctx->ns_wx_flush,
        (unsigned long long)ctx->aprr_toggles,
        (unsigned long long)ctx->tlb_checks_emitted,
        (unsigned long long)ctx->tlb_group_guards,
        (unsigned long long)ctx->tlb_slow_hits,
        (unsigned long long)ctx->vfp_cache_copies_emitted,
        (unsigned long long)ctx->fpcr_switches_emitted,
        (unsigned long long)ctx->fpcr_guard_slow_hits,
        (unsigned long long)ctx->aot_abs64_words,
        (unsigned long long)ctx->exits_by_reason[1],
        (unsigned long long)ctx->exits_by_reason[2],
        (unsigned long long)ctx->exits_by_reason[3],
        (unsigned long long)ctx->exits_by_reason[4],
        (unsigned long long)ctx->exits_by_reason[5],
        (unsigned long long)ctx->exits_by_reason[6],
        (unsigned long long)ctx->exits_by_reason[7]);
}

void a32_host_probe(a32_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }
    memset(&ctx->host_info, 0, sizeof(ctx->host_info));
    ctx->has_host_info = false;

    const char *host_env = getenv("A32RT_HOST");
    if (host_env && strcmp(host_env, "none") == 0) {
        return;
    }

    int fd = open(A32H_DEV_PATH, O_RDWR | O_CLOEXEC);
    if (fd < 0 && (errno == EACCES || errno == EPERM || errno == EROFS)) {
        fd = open(A32H_DEV_PATH, O_RDONLY | O_CLOEXEC);
    }
    if (fd < 0) {
        return;
    }

    a32h_info_t info = {0};
#if defined(__linux__)
    if (syscall(SYS_ioctl, fd, (unsigned long)A32H_GET_INFO, &info) == 0 &&
#else
    if (ioctl(fd, (unsigned long)A32H_GET_INFO, &info) == 0 &&
#endif
        info.abi_major == A32H_ABI_MAJOR) {
        ctx->host_info = info;
        ctx->has_host_info = true;
    }

    close(fd);
}
