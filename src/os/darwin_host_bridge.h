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

/* Forward-compatible worker thread-exit reply bit. The frozen
 * include/a32host.h contract (A32H_ABI 1.2) defines DACT bits 0..4; bit 5
 * is reserved there. Hosts that implement __bsdthread_terminate replies
 * set this bit to ask the bridge to exit only the calling worker thread
 * (DEXIT_THREAD_EXIT rendezvous); older hosts never set it, so testing
 * the bit is a no-op against them. */
#ifndef A32H_DACT_EXIT_THREAD
#define A32H_DACT_EXIT_THREAD (1U << 5)
#endif

/* CLI/env activation: --personality darwin-host (canonical G02/hl flag)
 * or hl-darwin (alias), --darwin-boot, A32RT_PERSONALITY. Stops option
 * scanning at `--` or the first positional <PATH> so guest argv never
 * hijacks the launcher. Sets *out_darwin_boot for --darwin-boot. */
bool a32_darwin_cli_wants_bridge(int argc, char **argv, bool *out_darwin_boot);

/* Minimal Mach-O magic check for slim builds (no src/ios/ dependency). */
bool a32_darwin_is_macho_fd(int fd);

/* Auto-activation when the host advertises A32H_CAP_DARWIN_PERSONALITY. */
bool a32_darwin_auto_bridge(const a32_ctx_t *ctx, bool target_is_macho,
    bool darwin_boot);

/* FNV-1a mix of the host personality tag into the AOT cache key. */
uint64_t a32_darwin_mix_hash(uint64_t h, uint64_t person);

/* AArch64 ESR Write-not-Read flag for a host sync-trap ucontext
 * (bit 6 of the ISS). uctx is a ucontext_t *; returns 1 (store), 0
 * (load), or -1 when no ESR is available on this host. */
int a32_darwin_esr_wnr(void *uctx);

/* Register / CPSR / IT-state marshaling (src/os/darwin_host_cpu.c). */
void a32_darwin_fill_cpu(const a32_ctx_t *ctx, a32h_darwin_cpu_state_t *cpu,
    bool full);
void a32_darwin_fill_svc_pc(const a32_ctx_t *ctx, a32h_darwin_cpu_state_t *cpu);
void a32_darwin_apply_cpu(a32_ctx_t *ctx, const a32h_darwin_cpu_state_t *cpu);

/* Bridge fault classifier (ARM32 + Thumb-1/Thumb-2 stores, STREX of every
 * size, VSTR/VSTM/VST1): A32H_DFAULT_WRITE iff the faulting instruction
 * is a store, else A32H_DFAULT_READ (EXEC is decided by the caller).
 * Public for tests/test_g02_darwin_bridge.c; prefers ctx->fault_is_write
 * recorded from the host ESR when ctx->fault_host_valid is set. */
uint32_t a32_darwin_fault_kind_for(const a32_ctx_t *ctx);

/* BKPT #imm at the faulting pc (src/os/darwin_host_cpu.c). */
uint32_t a32_darwin_bkpt_imm_for(const a32_ctx_t *ctx);

/* Host-ioctl path used by src/main.c (cached /dev/a32host fd). */
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
uint32_t a32_darwin_sync_exit(a32_ctx_t *ctx, int sig);

#endif /* A32RT_OS_DARWIN_HOST_BRIDGE_H */
