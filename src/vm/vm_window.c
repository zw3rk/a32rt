/*
 * src/vm/vm_window.c -- Guest 4 GiB window [G, G+4GiB) reservation
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Split from src/vm.c (A32-R13-16): window probing and reservation live
 * here so src/vm.c stays under the 900-line limit. Curated C11.
 */

#include "a32rt.h"
#include "os/backend.h"
#include "vm/vm_priv.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define A32_WINDOW_SIZE 0x100000000ULL

static int try_reserve_window(uint64_t base)
{
    if ((base & (A32_PAGE_SIZE - 1ULL)) != 0ULL || base > UINT64_MAX - A32_WINDOW_SIZE) {
        return -1;
    }
    uint64_t map_start = (base == 0ULL) ? (uint64_t)A32_LOW_GUARD_SIZE : base;
    uint64_t map_len   = (base == 0ULL) ? (A32_WINDOW_SIZE - (uint64_t)A32_LOW_GUARD_SIZE) : A32_WINDOW_SIZE;
    void *out = NULL;
    if (a32_backend_map_anon((void *)(uintptr_t)map_start, (size_t)map_len, 0, 1, 0, 0, &out) != 0 ||
        (uint64_t)(uintptr_t)out != map_start) {
        if (out != NULL && (uint64_t)(uintptr_t)out != map_start) {
            (void)a32_backend_unmap(out, (size_t)map_len);
        }
        return -1;
    }
    return 0;
}

int a32_vm_init(a32_ctx_t *ctx, uint64_t requested_window)
{
    if (!ctx) {
        return -1;
    }
    uint64_t base = requested_window;
    bool     has_explicit = (requested_window != UINT64_MAX);
    if (!has_explicit) {
        const char *env_win = getenv("A32RT_WINDOW");
        if (env_win && env_win[0] != '\0') {
            char *endp = NULL;
            errno = 0;
            unsigned long long val = strtoull(env_win, &endp, 0);
            if (strchr(env_win, '-') != NULL || errno == ERANGE || !endp ||
                endp == env_win || *endp != '\0' || (uint64_t)val == UINT64_MAX) {
                fprintf(stderr, "a32rt: invalid A32RT_WINDOW '%s'\n", env_win);
                return -1;
            }
            base = (uint64_t)val;
            has_explicit = true;
        }
    }
    if (has_explicit) {
        if (try_reserve_window(base) != 0) {
            fprintf(stderr, "a32rt: failed to reserve guest window at 0x%llx\n", (unsigned long long)base);
            return -1;
        }
        ctx->guest_base = base;
    } else if (ctx->has_host_info && ctx->host_info.window_hint != 0ULL &&
               try_reserve_window(ctx->host_info.window_hint) == 0) {
        ctx->guest_base = ctx->host_info.window_hint;
    } else if (!ctx->is_ios && try_reserve_window(0ULL) == 0) {
        ctx->guest_base = 0ULL;
    } else {
        bool found = false;
        for (uint64_t cand = 0x100000000ULL; cand < 0x1000000000ULL; cand += 0x100000000ULL) {
            if (try_reserve_window(cand) == 0) {
                ctx->guest_base = cand;
                found = true;
                break;
            }
        }
        if (!found) {
            fprintf(stderr, "a32rt: unable to find free 4 GiB guest window\n");
            return -1;
        }
    }
    a32_vm_lock_acquire();
    g_vm_root_ctx     = ctx;
    g_vm_brk_base     = 0U;
    g_vm_brk_curr     = 0U;
    g_vm_mmap_cursor  = A32_MMAP_BASE_32;
    g_vm_region_count = 0U;
    ctx->mmap_cursor  = A32_MMAP_BASE_32;
    ctx->region_count = 0U;
    a32_vm_lock_release();
    return 0;
}
