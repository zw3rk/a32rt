/*
 * src/vm.c — Guest 4 GiB window [G, G+4GiB) reservation and guest VMA manager
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#define _GNU_SOURCE
#include "a32rt.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#define A32_PAGE_SIZE      4096U
#define A32_PAGE_MASK      (~(A32_PAGE_SIZE - 1U))
#define A32_WINDOW_SIZE    0x100000000ULL /* 4 GiB */
#define A32_LOW_GUARD_SIZE 0x10000U       /* 64 KiB low guard at guest VA 0 */
#define A32_MMAP_BASE_32   0x40000000U    /* Guest non-fixed mmap region base (1 GiB) */
#define A32_MMAP_TOP_32    0xDF000000U    /* Below stack (0xE0000000) and kuser (0xFFFF0000) */

static uint32_t page_align_down(uint32_t v)
{
    return v & A32_PAGE_MASK;
}

static uint32_t page_align_up(uint32_t v)
{
    return (v + (A32_PAGE_SIZE - 1U)) & A32_PAGE_MASK;
}

static int try_reserve_window(uint64_t base)
{
    uint64_t map_start = (base == 0ULL) ? (uint64_t)A32_LOW_GUARD_SIZE : base;
    uint64_t map_len   = (base == 0ULL) ? (A32_WINDOW_SIZE - (uint64_t)A32_LOW_GUARD_SIZE)
                                        : A32_WINDOW_SIZE;

    void *res = mmap((void *)(uintptr_t)map_start, (size_t)map_len, PROT_NONE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (res == MAP_FAILED) {
        return -1;
    }
    if ((uint64_t)(uintptr_t)res != map_start) {
        munmap(res, (size_t)map_len);
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
            unsigned long long val = strtoull(env_win, &endp, 0);
            if (endp != env_win && *endp == '\0') {
                base = (uint64_t)val;
                has_explicit = true;
            }
        }
    }

    if (!has_explicit && ctx->has_host_info && ctx->host_info.window_hint != 0ULL) {
        base = ctx->host_info.window_hint;
        has_explicit = true;
    }

    if (has_explicit) {
        if (try_reserve_window(base) != 0) {
            fprintf(stderr, "a32rt: failed to reserve guest window at 0x%llx\n",
                    (unsigned long long)base);
            return -1;
        }
        ctx->guest_base = base;
    } else {
        /* Try G = 0 first; if occupied, scan 4 GiB-aligned windows [4G, 64G). */
        if (try_reserve_window(0ULL) == 0) {
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
    }

    ctx->mmap_cursor = A32_MMAP_BASE_32;
    ctx->region_count = 0;
    return 0;
}

int a32_vm_record_region(a32_ctx_t *ctx, uint32_t start, uint32_t end,
                         uint32_t prot, uint32_t flags, const char *name)
{
    if (!ctx || start >= end) {
        return -1;
    }
    if (ctx->region_count >= A32_MAX_REGIONS) {
        return -1;
    }
    a32_region_t *r = &ctx->regions[ctx->region_count++];
    r->start = start;
    r->end   = end;
    r->prot  = prot;
    r->flags = flags;
    if (name) {
        snprintf(r->name, sizeof(r->name), "%s", name);
    } else {
        r->name[0] = '\0';
    }
    return 0;
}

uint32_t a32_vm_brk(a32_ctx_t *ctx, uint32_t new_brk)
{
    if (!ctx) {
        return 0U;
    }
    if (new_brk == 0U || new_brk < ctx->brk_base) {
        return ctx->brk_curr;
    }

    uint32_t old_page = page_align_up(ctx->brk_curr);
    uint32_t new_page = page_align_up(new_brk);

    if (new_page > old_page) {
        uint32_t len = new_page - old_page;
        void *host_addr = a32_guest_ptr(ctx, old_page);
        void *res = mmap(host_addr, len, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (res == MAP_FAILED) {
            return ctx->brk_curr;
        }
    } else if (new_page < old_page) {
        uint32_t len = old_page - new_page;
        void *host_addr = a32_guest_ptr(ctx, new_page);
        /* Re-reserve shrunk brk pages with PROT_NONE inside the guest window */
        mmap(host_addr, len, PROT_NONE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    }

    ctx->brk_curr = new_brk;
    return ctx->brk_curr;
}

uint32_t a32_vm_mmap2(a32_ctx_t *ctx, uint32_t addr, uint32_t len, uint32_t prot,
                      uint32_t flags, int fd, uint32_t pgoffset)
{
    if (!ctx || len == 0U) {
        return (uint32_t)(-EINVAL);
    }

    uint32_t aligned_len = page_align_up(len);
    if (aligned_len < len) {
        return (uint32_t)(-ENOMEM);
    }

    /* Translate guest flags (ARM MAP_FIXED = 0x10, MAP_ANONYMOUS = 0x20, MAP_PRIVATE = 0x02) */
    int host_flags = 0;
    if (flags & 0x01U) {
        host_flags |= MAP_SHARED;
    }
    if (flags & 0x02U) {
        host_flags |= MAP_PRIVATE;
    }
    if (flags & 0x20U) {
        host_flags |= MAP_ANONYMOUS;
    }

    uint32_t target_va = 0U;
    if (flags & 0x10U /* MAP_FIXED */) {
        if ((addr & (A32_PAGE_SIZE - 1U)) != 0U || addr < A32_LOW_GUARD_SIZE) {
            return (uint32_t)(-EINVAL);
        }
        target_va = addr;
    } else {
        target_va = page_align_up(ctx->mmap_cursor);
        if (target_va + aligned_len < target_va || target_va + aligned_len > A32_MMAP_TOP_32) {
            return (uint32_t)(-ENOMEM);
        }
        ctx->mmap_cursor = target_va + aligned_len;
    }

    int host_prot = 0;
    if (prot & 0x1U) {
        host_prot |= PROT_READ;
    }
    if (prot & 0x2U) {
        host_prot |= PROT_WRITE;
    }
    if (prot & 0x4U) {
        host_prot |= PROT_EXEC;
    }

    void *host_target = a32_guest_ptr(ctx, target_va);
    off_t host_off = (off_t)pgoffset * (off_t)A32_PAGE_SIZE;
    void *res = mmap(host_target, aligned_len, host_prot,
                     host_flags | MAP_FIXED, (host_flags & MAP_ANONYMOUS) ? -1 : fd,
                     (host_flags & MAP_ANONYMOUS) ? 0 : host_off);
    if (res == MAP_FAILED) {
        return (uint32_t)(-errno);
    }

    a32_vm_record_region(ctx, target_va, target_va + aligned_len, prot, flags, "[mmap]");
    return target_va;
}

int a32_vm_mprotect(a32_ctx_t *ctx, uint32_t addr, uint32_t len, uint32_t prot)
{
    if (!ctx || (addr & (A32_PAGE_SIZE - 1U)) != 0U) {
        return -EINVAL;
    }
    if (len == 0U) {
        return 0;
    }
    uint32_t aligned_len = page_align_up(len);
    int host_prot = 0;
    if (prot & 0x1U) {
        host_prot |= PROT_READ;
    }
    if (prot & 0x2U) {
        host_prot |= PROT_WRITE;
    }
    if (prot & 0x4U) {
        host_prot |= PROT_EXEC;
    }

    if (mprotect(a32_guest_ptr(ctx, addr), aligned_len, host_prot) != 0) {
        return -errno;
    }
    return 0;
}

int a32_vm_munmap(a32_ctx_t *ctx, uint32_t addr, uint32_t len)
{
    if (!ctx || (addr & (A32_PAGE_SIZE - 1U)) != 0U) {
        return -EINVAL;
    }
    if (len == 0U) {
        return -EINVAL;
    }
    uint32_t aligned_len = page_align_up(len);
    uint32_t start = page_align_down(addr);
    /* Keep the [G, G+4GiB) reservation intact by replacing unmapped pages with PROT_NONE */
    void *res = mmap(a32_guest_ptr(ctx, start), aligned_len, PROT_NONE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (res == MAP_FAILED) {
        return -errno;
    }
    return 0;
}
