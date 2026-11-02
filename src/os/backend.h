/*
 * src/os/backend.h -- Host OS backend interface (Wave U3 A32-4)
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * UNIFICATION-PLAN.md Rev 4, section 6 (U3 increment): the memory, JIT,
 * and worker primitives consumed by src/vm/vm32.c and the DBT. Two
 * implementations ship from this tree:
 *
 *   Backend 1 (src/os/backend_linux.c): static-PIE aarch64-linux
 *   (native Linux or hyper-linux); raw mmap/mprotect, tgkill preempt.
 *
 *   Backend 2 (src/os/backend_darwin.c): standalone macOS arm64 Mach-O
 *   (libSystem, MAP_JIT thread-local APRR W^X, pthread_kill preempt).
 *
 * Linux-only paths sit behind __linux__ guards with -ENOSYS fallbacks so
 * every TU stays syntax-clean under make check on either host. Curated
 * C11 without VLAs. Never touches the platform
 * register (no inline asm anywhere in src/os/).
 */

#ifndef A32RT_OS_BACKEND_H
#define A32RT_OS_BACKEND_H

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "a32rt.h"

/* ---- 1. Backend identity & dynamic host page size ---- */

typedef enum {
    A32_BACKEND_LINUX = 1,
    A32_BACKEND_DARWIN = 2
} a32_backend_kind_t;

a32_backend_kind_t a32_backend_kind(void);
size_t a32_backend_page_size(void); /* 4096, 16384, or 65536, never fixed */

/* ---- 2. Host memory primitives consumed by src/vm/ ---- */

/* fixed places at hint_addr (MAP_FIXED_NOREPLACE: fail if occupied);
 * replace additionally allows clobbering (plain MAP_FIXED) for the
 * munmap/brk/mmap paths that intentionally replace live pages. */
int a32_backend_map_anon(void *hint_addr, size_t len, int linux_prot,
    int fixed, int replace, int shared, void **out_host_addr);
int a32_backend_map_file(void *hint_addr, size_t len, int linux_prot,
    int shared, int fixed, int replace, int host_fd,
    uint64_t aligned_file_offset, void **out_host_addr);
int a32_backend_protect(void *host_addr, size_t len, int linux_prot);
int a32_backend_unmap(void *host_addr, size_t len);
int a32_backend_msync(void *host_addr, size_t len);

/* ---- 3. JIT W^X code-cache allocation ---- */

void *a32_backend_jit_alloc(size_t size);
void a32_backend_jit_thread_init(void);
void a32_backend_jit_begin_write(void *buf, size_t size);
int a32_backend_jit_seal_rx(void *buf, size_t size);
void a32_backend_jit_free(void *buf, size_t size);

/* ---- 4. Worker sync & preemption (quiescence, section 4.5) ---- */

typedef struct {
    _Atomic uint32_t sig_seq;
    int wake_pipe_rd;
    int wake_pipe_wr;
    uint64_t host_tid;
} a32_worker_sync_t;

int a32_backend_worker_sync_init(a32_worker_sync_t *ws);
void a32_backend_worker_sync_destroy(a32_worker_sync_t *ws);
void a32_backend_worker_notify_signal(a32_worker_sync_t *ws);
/* Reserved preemption signal (SIGRTMIN+1 on Linux, SIGUSR2 on Darwin). */
int a32_backend_preempt_worker(uint64_t host_tid);

#endif /* A32RT_OS_BACKEND_H */
