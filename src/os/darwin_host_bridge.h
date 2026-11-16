/*
 * src/os/darwin_host_bridge.h — Narrow A32H_ABI 1.2 (G02) external Darwin bridge
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * A32H_ARCH_ARM32 client of A32H_DARWIN_BOOT / A32H_DARWIN_TRAP over
 * /dev/a32host, plus the in-process test entry point used by
 * tests/test_g02_darwin_bridge.c. Keeps a32rt small: Mach-O/dyld, BSD/Mach
 * traps, threads, and signals stay in hyper-linux (hl).
 */

#ifndef A32RT_OS_DARWIN_HOST_BRIDGE_H
#define A32RT_OS_DARWIN_HOST_BRIDGE_H

#include "a32rt.h"

#include <stdbool.h>
#include <stdint.h>

/* CLI/env activation: --personality hl-darwin, --darwin-boot,
 * A32RT_PERSONALITY=hl-darwin. Sets *out_darwin_boot for --darwin-boot. */
bool a32_darwin_cli_wants_bridge(int argc, char **argv, bool *out_darwin_boot);

/* Minimal Mach-O magic check for slim builds (no src/ios/ dependency). */
bool a32_darwin_is_macho_fd(int fd);

/* Auto-activation when the host advertises A32H_CAP_DARWIN_PERSONALITY. */
bool a32_darwin_auto_bridge(const a32_ctx_t *ctx, bool target_is_macho,
    bool darwin_boot);

/* FNV-1a mix of the host personality tag into the AOT cache key. */
uint64_t a32_darwin_mix_hash(uint64_t h, uint64_t person);

/* Host-ioctl path used by src/main.c (opens /dev/a32host per round-trip). */
int a32_darwin_boot_and_run(a32_ctx_t *ctx, bool use_dbt, bool difftest);

/* In-process path: fn == NULL falls back to host ioctl. The 4-arg form
 * owns its context; the ctx form runs inside the caller's context so
 * tests can inspect blocks_compiled / regions / personality hash. */
int a32_run_darwin_with_handler(a32h_darwin_handler_fn fn, void *opaque,
    bool use_dbt, bool difftest);
int a32_run_darwin_ctx_with_handler(a32_ctx_t *ctx, a32h_darwin_handler_fn fn,
    void *opaque, bool use_dbt, bool difftest);

/* Dispatch hooks for the slim build (called with darwin_external set). */
int a32_darwin_trap_once(a32_ctx_t *ctx);
void a32_darwin_enqueue_signal(a32_ctx_t *ctx, int sig);
uint32_t a32_darwin_sync_exit(int sig);

#endif /* A32RT_OS_DARWIN_HOST_BRIDGE_H */
