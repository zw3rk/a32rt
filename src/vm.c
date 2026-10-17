/*
 * src/vm.c — Guest 4 GiB window [G, G+4GiB) reservation and guest VMA manager
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#define _GNU_SOURCE
#include "a32rt.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
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
#define A32_KUSER_BASE_32  0xFFFF0000U    /* Architectural kuser helper page */

static _Atomic uint32_t g_vm_lock = 0U;
static a32_ctx_t       *g_vm_root_ctx = NULL;
static uint32_t         g_vm_brk_base = 0U;
static uint32_t         g_vm_brk_curr = 0U;
static uint32_t         g_vm_mmap_cursor = A32_MMAP_BASE_32;
static a32_region_t     g_vm_regions[A32_MAX_REGIONS + 4U];
static size_t           g_vm_region_count = 0U;

static inline void vm_lock_acquire(void)
{
    while (atomic_exchange_explicit(&g_vm_lock, 1U, memory_order_acquire) != 0U) {
#if defined(__aarch64__)
        __asm__ volatile("yield");
#endif
    }
}

static inline void vm_lock_release(void)
{
    atomic_store_explicit(&g_vm_lock, 0U, memory_order_release);
}

static uint32_t page_align_down(uint32_t v)
{
    return v & A32_PAGE_MASK;
}

static uint64_t page_align_up(uint64_t v)
{
    return (v + 0xFFFULL) & ~0xFFFULL;
}

static void vm_sync_from_ctx(a32_ctx_t *ctx)
{
    if (ctx->brk_base != 0U && g_vm_brk_base != ctx->brk_base) {
        g_vm_brk_base = ctx->brk_base;
        g_vm_brk_curr = ctx->brk_curr;
    }
}

static void vm_sync_to_ctx(a32_ctx_t *ctx)
{
    if (g_vm_brk_base != 0U) {
        ctx->brk_base = g_vm_brk_base;
        ctx->brk_curr = g_vm_brk_curr;
    }
    size_t count = g_vm_region_count;
    if (count > A32_MAX_REGIONS) {
        count = A32_MAX_REGIONS;
    }
    ctx->mmap_cursor  = g_vm_mmap_cursor;
    ctx->region_count = count;
    if (count > 0U) {
        memcpy(ctx->regions, g_vm_regions, count * sizeof(a32_region_t));
    }
    if (g_vm_root_ctx && g_vm_root_ctx != ctx) {
        if (g_vm_brk_base != 0U) {
            g_vm_root_ctx->brk_base = g_vm_brk_base;
            g_vm_root_ctx->brk_curr = g_vm_brk_curr;
        }
        g_vm_root_ctx->mmap_cursor  = g_vm_mmap_cursor;
        g_vm_root_ctx->region_count = count;
        if (count > 0U) {
            memcpy(g_vm_root_ctx->regions, g_vm_regions,
                   count * sizeof(a32_region_t));
        }
    }
}

static void vm_remove_region_at(size_t idx)
{
    if (idx >= g_vm_region_count) {
        return;
    }
    if (idx + 1U < g_vm_region_count) {
        memmove(&g_vm_regions[idx], &g_vm_regions[idx + 1U],
                (g_vm_region_count - idx - 1U) * sizeof(a32_region_t));
    }
    g_vm_region_count--;
}

static int vm_insert_region_raw(uint32_t start, uint32_t end,
                                uint32_t prot, uint32_t flags, const char *name)
{
    if (start >= end || g_vm_region_count >= A32_MAX_REGIONS + 4U) {
        return -1;
    }
    size_t pos = 0U;
    while (pos < g_vm_region_count && g_vm_regions[pos].start < start) {
        pos++;
    }
    if (pos < g_vm_region_count) {
        memmove(&g_vm_regions[pos + 1U], &g_vm_regions[pos],
                (g_vm_region_count - pos) * sizeof(a32_region_t));
    }
    g_vm_region_count++;
    a32_region_t *r = &g_vm_regions[pos];
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

static void vm_coalesce_regions_locked(void)
{
    size_t i = 0U;
    while (i + 1U < g_vm_region_count) {
        a32_region_t *cur  = &g_vm_regions[i];
        a32_region_t *next = &g_vm_regions[i + 1U];
        if (cur->end == next->start &&
            cur->prot == next->prot &&
            (cur->flags & 0x23U) == (next->flags & 0x23U) &&
            strcmp(cur->name, next->name) == 0) {
            cur->end = next->end;
            vm_remove_region_at(i + 1U);
        } else {
            i++;
        }
    }
}

static void vm_unmap_regions_locked(uint32_t unmap_start, uint32_t unmap_end)
{
    size_t i = 0U;
    while (i < g_vm_region_count) {
        a32_region_t *r = &g_vm_regions[i];
        if (r->end <= unmap_start || r->start >= unmap_end) {
            i++;
            continue;
        }
        if (r->start >= unmap_start && r->end <= unmap_end) {
            vm_remove_region_at(i);
            continue;
        }
        if (r->start < unmap_start && r->end > unmap_end) {
            if (g_vm_region_count >= A32_MAX_REGIONS + 3U) {
                size_t before = g_vm_region_count;
                vm_coalesce_regions_locked();
                if (g_vm_region_count != before) {
                    i = 0U;
                    continue;
                }
            }
            uint32_t right_end   = r->end;
            uint32_t right_prot  = r->prot;
            uint32_t right_flags = r->flags;
            char     right_name[64];
            memcpy(right_name, r->name, sizeof(right_name));
            r->end = unmap_start;
            (void)vm_insert_region_raw(unmap_end, right_end, right_prot, right_flags, right_name);
            i += 2U;
            continue;
        }
        if (r->start < unmap_start) {
            r->end = unmap_start;
            i++;
        } else {
            r->start = unmap_end;
            i++;
        }
    }
}

static int vm_record_region_locked(uint32_t start, uint32_t end,
                                   uint32_t prot, uint32_t flags, const char *name)
{
    if (start >= end) {
        return -1;
    }
    char name_copy[64];
    if (name) {
        snprintf(name_copy, sizeof(name_copy), "%s", name);
    } else {
        name_copy[0] = '\0';
    }
    uint32_t clean_flags = flags & ~(0x10U | 0x100000U);
    vm_unmap_regions_locked(start, end);
    int rc = vm_insert_region_raw(start, end, prot, clean_flags, name_copy);
    vm_coalesce_regions_locked();
    return rc;
}

static void vm_mprotect_regions_locked(uint32_t mp_start, uint32_t mp_end, uint32_t prot)
{
    size_t i = 0U;
    while (i < g_vm_region_count) {
        a32_region_t *r = &g_vm_regions[i];
        if (r->end <= mp_start || r->start >= mp_end) {
            i++;
            continue;
        }
        if (r->start >= mp_start && r->end <= mp_end) {
            r->prot = prot;
            i++;
            continue;
        }
        if (r->start < mp_start && r->end > mp_end) {
            if (g_vm_region_count + 2U <= A32_MAX_REGIONS + 4U) {
                uint32_t orig_end   = r->end;
                uint32_t orig_prot  = r->prot;
                uint32_t orig_flags = r->flags;
                char     orig_name[64];
                memcpy(orig_name, r->name, sizeof(orig_name));
                r->end = mp_start;
                (void)vm_insert_region_raw(mp_start, mp_end, prot, orig_flags, orig_name);
                (void)vm_insert_region_raw(mp_end, orig_end, orig_prot, orig_flags, orig_name);
                i += 3U;
            } else {
                r->prot = prot;
                i++;
            }
            continue;
        }
        if (r->start < mp_start) {
            if (g_vm_region_count + 1U <= A32_MAX_REGIONS + 4U) {
                uint32_t orig_end   = r->end;
                uint32_t orig_flags = r->flags;
                char     orig_name[64];
                memcpy(orig_name, r->name, sizeof(orig_name));
                r->end = mp_start;
                (void)vm_insert_region_raw(mp_start, orig_end, prot, orig_flags, orig_name);
                i += 2U;
            } else {
                r->prot = prot;
                i++;
            }
        } else {
            if (g_vm_region_count + 1U <= A32_MAX_REGIONS + 4U) {
                uint32_t orig_start = r->start;
                uint32_t orig_flags = r->flags;
                char     orig_name[64];
                memcpy(orig_name, r->name, sizeof(orig_name));
                r->start = mp_end;
                (void)vm_insert_region_raw(orig_start, mp_end, prot, orig_flags, orig_name);
                i += 2U;
            } else {
                r->prot = prot;
                i++;
            }
        }
    }
    vm_coalesce_regions_locked();
}

static bool vm_range_overlaps_locked(uint32_t start, uint32_t end)
{
    for (size_t i = 0U; i < g_vm_region_count; i++) {
        if (g_vm_regions[i].end > start && g_vm_regions[i].start < end) {
            return true;
        }
    }
    return false;
}

static bool vm_is_range_mapped_locked(uint32_t start, uint32_t end)
{
    if (start >= end) {
        return false;
    }
    uint32_t cur = start;
    for (size_t i = 0U; i < g_vm_region_count; i++) {
        const a32_region_t *r = &g_vm_regions[i];
        if (r->end <= cur) {
            continue;
        }
        if (r->start > cur) {
            return false;
        }
        if (r->end >= end) {
            return true;
        }
        cur = r->end;
    }
    return false;
}

static bool vm_check_mapped_range_locked(uint32_t start, uint32_t end,
                                         uint32_t *out_prot, uint32_t *out_flags,
                                         char *out_name, size_t name_sz)
{
    if (start >= end) {
        return false;
    }
    uint32_t cur = start;
    bool     first = true;
    uint32_t first_prot  = 0U;
    uint32_t first_flags = 0U;
    for (size_t i = 0U; i < g_vm_region_count; i++) {
        const a32_region_t *r = &g_vm_regions[i];
        if (r->end <= cur) {
            continue;
        }
        if (r->start > cur) {
            return false;
        }
        if (first) {
            first_prot  = r->prot;
            first_flags = r->flags & 0x23U;
            if (out_prot) {
                *out_prot = r->prot;
            }
            if (out_flags) {
                *out_flags = r->flags;
            }
            if (out_name && name_sz > 0U) {
                snprintf(out_name, name_sz, "%s", r->name);
            }
            first = false;
        } else {
            if (r->prot != first_prot || (r->flags & 0x23U) != first_flags) {
                return false;
            }
        }
        if (r->end >= end) {
            return true;
        }
        cur = r->end;
    }
    return false;
}

static int vm_find_free_gap_locked(uint64_t aligned_len, uint32_t *out_va)
{
    if (aligned_len == 0ULL ||
        aligned_len > (uint64_t)(A32_MMAP_TOP_32 - A32_MMAP_BASE_32)) {
        return -1;
    }

    uint64_t cursor_cand = page_align_up((uint64_t)g_vm_mmap_cursor);
    if (cursor_cand < (uint64_t)A32_MMAP_BASE_32) {
        cursor_cand = (uint64_t)A32_MMAP_BASE_32;
    }
    if (cursor_cand + aligned_len <= (uint64_t)A32_MMAP_TOP_32 &&
        !vm_range_overlaps_locked((uint32_t)cursor_cand, (uint32_t)(cursor_cand + aligned_len))) {
        *out_va = (uint32_t)cursor_cand;
        return 0;
    }

    uint64_t cand = (uint64_t)A32_MMAP_BASE_32;
    for (size_t i = 0U; i < g_vm_region_count; i++) {
        uint64_t r_start = (uint64_t)g_vm_regions[i].start;
        uint64_t r_end   = page_align_up((uint64_t)g_vm_regions[i].end);
        if (r_end <= cand) {
            continue;
        }
        if (cand + aligned_len <= r_start) {
            break;
        }
        cand = r_end;
    }

    if (cand + aligned_len > (uint64_t)A32_MMAP_TOP_32) {
        return -1;
    }

    *out_va = (uint32_t)cand;
    return 0;
}

static int try_reserve_window(uint64_t base)
{
    if ((base & (A32_PAGE_SIZE - 1ULL)) != 0ULL || base > UINT64_MAX - A32_WINDOW_SIZE) {
        return -1;
    }

    uint64_t map_start = (base == 0ULL) ? (uint64_t)A32_LOW_GUARD_SIZE : base;
    uint64_t map_len   = (base == 0ULL) ? (A32_WINDOW_SIZE - (uint64_t)A32_LOW_GUARD_SIZE)
                                        : A32_WINDOW_SIZE;

    void *res = mmap((void *)(uintptr_t)map_start, (size_t)map_len, PROT_NONE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (res == MAP_FAILED && (errno == EINVAL || errno == EOPNOTSUPP)) {
        res = mmap((void *)(uintptr_t)map_start, (size_t)map_len, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    }
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
            errno = 0;
            unsigned long long val = strtoull(env_win, &endp, 0);
            if (env_win[0] == '-' || errno == ERANGE || !endp ||
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
            fprintf(stderr, "a32rt: failed to reserve guest window at 0x%llx\n",
                    (unsigned long long)base);
            return -1;
        }
        ctx->guest_base = base;
    } else if (ctx->has_host_info && ctx->host_info.window_hint != 0ULL &&
               try_reserve_window(ctx->host_info.window_hint) == 0) {
        ctx->guest_base = ctx->host_info.window_hint;
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

    vm_lock_acquire();
    g_vm_root_ctx     = ctx;
    g_vm_brk_base     = 0U;
    g_vm_brk_curr     = 0U;
    g_vm_mmap_cursor  = A32_MMAP_BASE_32;
    g_vm_region_count = 0U;
    ctx->mmap_cursor  = A32_MMAP_BASE_32;
    ctx->region_count = 0U;
    vm_lock_release();
    return 0;
}

int a32_vm_record_region(a32_ctx_t *ctx, uint32_t start, uint32_t end,
                         uint32_t prot, uint32_t flags, const char *name)
{
    if (!ctx || start >= end) {
        return -1;
    }
    vm_lock_acquire();
    vm_sync_from_ctx(ctx);
    int rc = vm_record_region_locked(start, end, prot, flags, name);
    vm_sync_to_ctx(ctx);
    vm_lock_release();
    return rc;
}

uint32_t a32_vm_brk(a32_ctx_t *ctx, uint32_t new_brk)
{
    if (!ctx) {
        return 0U;
    }
    vm_lock_acquire();
    vm_sync_from_ctx(ctx);

    if (new_brk == 0U || new_brk < g_vm_brk_base || new_brk >= A32_MMAP_BASE_32) {
        uint32_t curr = g_vm_brk_curr;
        vm_sync_to_ctx(ctx);
        vm_lock_release();
        return curr;
    }

    uint64_t old_page64 = page_align_up((uint64_t)g_vm_brk_curr);
    uint64_t new_page64 = page_align_up((uint64_t)new_brk);
    if (new_page64 > (uint64_t)A32_MMAP_BASE_32) {
        uint32_t curr = g_vm_brk_curr;
        vm_sync_to_ctx(ctx);
        vm_lock_release();
        return curr;
    }

    uint32_t old_page = (uint32_t)old_page64;
    uint32_t new_page = (uint32_t)new_page64;

    if (new_page > old_page) {
        if (vm_range_overlaps_locked(old_page, new_page)) {
            uint32_t curr = g_vm_brk_curr;
            vm_sync_to_ctx(ctx);
            vm_lock_release();
            return curr;
        }
        uint32_t len = new_page - old_page;
        void *host_addr = a32_guest_ptr(ctx, old_page);
        void *res = mmap(host_addr, len, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (res == MAP_FAILED) {
            uint32_t curr = g_vm_brk_curr;
            vm_sync_to_ctx(ctx);
            vm_lock_release();
            return curr;
        }
        (void)vm_record_region_locked(old_page, new_page,
                                      (uint32_t)(PROT_READ | PROT_WRITE), 0x22U, "[heap]");
    } else if (new_page < old_page) {
        uint32_t len = old_page - new_page;
        void *host_addr = a32_guest_ptr(ctx, new_page);
        /* Re-reserve shrunk brk pages with PROT_NONE inside the guest window */
        (void)mmap(host_addr, len, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        vm_unmap_regions_locked(new_page, old_page);
    }

    g_vm_brk_curr = new_brk;
    vm_sync_to_ctx(ctx);
    vm_lock_release();

    if (new_page < old_page) {
        a32_jit_invalidate_range(ctx, new_page, old_page);
    }
    return new_brk;
}

static uint32_t vm_mmap2_locked(a32_ctx_t *ctx, uint32_t addr, uint32_t len, uint32_t prot,
                                uint32_t flags, int fd, uint32_t pgoffset)
{
    if (!ctx || len == 0U || (flags & 0x03U) == 0U) {
        return (uint32_t)(-EINVAL);
    }

    uint64_t aligned_len = page_align_up((uint64_t)len);
    if (aligned_len == 0ULL || aligned_len > (uint64_t)A32_KUSER_BASE_32) {
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

    uint32_t target_va     = 0U;
    bool     update_cursor = false;
    if ((flags & 0x100000U /* MAP_FIXED_NOREPLACE */) != 0U) {
        if ((addr & (A32_PAGE_SIZE - 1U)) != 0U || addr < A32_LOW_GUARD_SIZE) {
            return (uint32_t)(-EINVAL);
        }
        if ((uint64_t)addr + aligned_len > (uint64_t)A32_KUSER_BASE_32) {
            return (uint32_t)(-ENOMEM);
        }
        if (vm_range_overlaps_locked(addr, (uint32_t)((uint64_t)addr + aligned_len))) {
            return (uint32_t)(-EEXIST);
        }
        target_va = addr;
    } else if ((flags & 0x10U /* MAP_FIXED */) != 0U) {
        if ((addr & (A32_PAGE_SIZE - 1U)) != 0U || addr < A32_LOW_GUARD_SIZE) {
            return (uint32_t)(-EINVAL);
        }
        if ((uint64_t)addr + aligned_len > (uint64_t)A32_KUSER_BASE_32) {
            return (uint32_t)(-ENOMEM);
        }
        target_va = addr;
    } else {
        uint32_t hint_va = page_align_down(addr);
        if (addr != 0U &&
            hint_va >= A32_MMAP_BASE_32 &&
            (uint64_t)hint_va + aligned_len <= (uint64_t)A32_KUSER_BASE_32 &&
            !vm_range_overlaps_locked(hint_va, (uint32_t)((uint64_t)hint_va + aligned_len))) {
            target_va = hint_va;
        } else if (vm_find_free_gap_locked(aligned_len, &target_va) != 0) {
            return (uint32_t)(-ENOMEM);
        } else {
            update_cursor = true;
        }
    }

    int host_prot = 0;
    if (prot & (0x1U | 0x4U)) {
        host_prot |= PROT_READ;
    }
    if (prot & 0x2U) {
        host_prot |= PROT_WRITE;
    }
    if ((prot & 0x4U) != 0U &&
        !((ctx->host_info.caps & A32H_CAP_WX_ENFORCED) != 0ULL && (prot & 0x2U) != 0U)) {
        host_prot |= PROT_EXEC;
    }

    void *host_target = a32_guest_ptr(ctx, target_va);
    off_t host_off = (off_t)pgoffset * (off_t)A32_PAGE_SIZE;
    void *res = mmap(host_target, (size_t)aligned_len, host_prot,
                     host_flags | MAP_FIXED, (host_flags & MAP_ANONYMOUS) ? -1 : fd,
                     (host_flags & MAP_ANONYMOUS) ? 0 : host_off);
    if (res == MAP_FAILED) {
        return (uint32_t)(-errno);
    }

    if (update_cursor) {
        g_vm_mmap_cursor = (uint32_t)((uint64_t)target_va + aligned_len);
    }
    (void)vm_record_region_locked(target_va, (uint32_t)((uint64_t)target_va + aligned_len),
                                  prot & 0x7U, flags, "[mmap]");
    return target_va;
}

uint32_t a32_vm_mmap2(a32_ctx_t *ctx, uint32_t addr, uint32_t len, uint32_t prot,
                      uint32_t flags, int fd, uint32_t pgoffset)
{
    if (!ctx || len == 0U || (flags & 0x03U) == 0U) {
        return (uint32_t)(-EINVAL);
    }

    vm_lock_acquire();
    vm_sync_from_ctx(ctx);
    uint32_t res = vm_mmap2_locked(ctx, addr, len, prot, flags, fd, pgoffset);
    vm_sync_to_ctx(ctx);
    vm_lock_release();

    if (res <= 0xFFFFF000U && (flags & (0x10U | 0x100000U)) != 0U) {
        uint64_t aligned_len = page_align_up((uint64_t)len);
        a32_jit_invalidate_range(ctx, res, (uint32_t)((uint64_t)res + aligned_len));
    }
    return res;
}

static int vm_mprotect_locked(a32_ctx_t *ctx, uint32_t addr, uint32_t len, uint32_t prot)
{
    if (!ctx || (addr & (A32_PAGE_SIZE - 1U)) != 0U || addr < 0x1000U) {
        return -EINVAL;
    }
    if ((prot & ~0x7U) != 0U) {
        return -EINVAL;
    }
    if (len == 0U) {
        return 0;
    }
    uint64_t aligned_len = page_align_up((uint64_t)len);
    uint64_t end64       = (uint64_t)addr + aligned_len;
    if (addr < A32_LOW_GUARD_SIZE ||
        aligned_len == 0ULL ||
        end64 > (uint64_t)A32_KUSER_BASE_32 ||
        !vm_is_range_mapped_locked(addr, (uint32_t)end64)) {
        return -ENOMEM;
    }

    int host_prot = 0;
    if (prot & (0x1U | 0x4U)) {
        host_prot |= PROT_READ;
    }
    if (prot & 0x2U) {
        host_prot |= PROT_WRITE;
    }
    if ((prot & 0x4U) != 0U &&
        !((ctx->host_info.caps & A32H_CAP_WX_ENFORCED) != 0ULL && (prot & 0x2U) != 0U)) {
        host_prot |= PROT_EXEC;
    }

    if (mprotect(a32_guest_ptr(ctx, addr), (size_t)aligned_len, host_prot) != 0) {
        return -errno;
    }

    vm_mprotect_regions_locked(addr, (uint32_t)end64, prot & 0x7U);
    return 0;
}

int a32_vm_mprotect(a32_ctx_t *ctx, uint32_t addr, uint32_t len, uint32_t prot)
{
    if (!ctx || (addr & (A32_PAGE_SIZE - 1U)) != 0U || addr < 0x1000U) {
        return -EINVAL;
    }
    if ((prot & ~0x7U) != 0U) {
        return -EINVAL;
    }
    if (len == 0U) {
        return 0;
    }

    vm_lock_acquire();
    vm_sync_from_ctx(ctx);
    int rc = vm_mprotect_locked(ctx, addr, len, prot);
    vm_sync_to_ctx(ctx);
    vm_lock_release();

    if (rc == 0) {
        uint64_t aligned_len = page_align_up((uint64_t)len);
        a32_jit_invalidate_range(ctx, addr, (uint32_t)((uint64_t)addr + aligned_len));
    }
    return rc;
}

static int vm_munmap_locked(a32_ctx_t *ctx, uint32_t addr, uint32_t len)
{
    if (!ctx || (addr & (A32_PAGE_SIZE - 1U)) != 0U || len == 0U) {
        return -EINVAL;
    }
    uint64_t aligned_len = page_align_up((uint64_t)len);
    if (addr < A32_LOW_GUARD_SIZE ||
        (uint64_t)addr + aligned_len > (uint64_t)A32_KUSER_BASE_32) {
        return -EINVAL;
    }

    uint32_t start = page_align_down(addr);
    /* Keep the [G, G+4GiB) reservation intact by replacing unmapped pages with PROT_NONE */
    void *res = mmap(a32_guest_ptr(ctx, start), (size_t)aligned_len, PROT_NONE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (res == MAP_FAILED) {
        return -errno;
    }

    uint32_t end = (uint32_t)((uint64_t)start + aligned_len);
    vm_unmap_regions_locked(start, end);
    if (g_vm_mmap_cursor == end && start >= A32_MMAP_BASE_32) {
        g_vm_mmap_cursor = start;
    }
    return 0;
}

int a32_vm_munmap(a32_ctx_t *ctx, uint32_t addr, uint32_t len)
{
    if (!ctx || (addr & (A32_PAGE_SIZE - 1U)) != 0U || len == 0U) {
        return -EINVAL;
    }

    vm_lock_acquire();
    vm_sync_from_ctx(ctx);
    int rc = vm_munmap_locked(ctx, addr, len);
    vm_sync_to_ctx(ctx);
    vm_lock_release();

    if (rc == 0) {
        uint64_t aligned_len = page_align_up((uint64_t)len);
        a32_jit_invalidate_range(ctx, addr, (uint32_t)((uint64_t)addr + aligned_len));
    }
    return rc;
}

uint32_t a32_vm_mremap(a32_ctx_t *ctx, uint32_t old_addr, uint32_t old_len,
                       uint32_t new_len, uint32_t flags, uint32_t new_addr)
{
    if (!ctx || (old_addr & (A32_PAGE_SIZE - 1U)) != 0U ||
        old_len == 0U || new_len == 0U) {
        return (uint32_t)(-EINVAL);
    }
    if ((flags & ~(1U /* MREMAP_MAYMOVE */ | 2U /* MREMAP_FIXED */)) != 0U) {
        return (uint32_t)(-EINVAL);
    }
    if ((flags & 2U) != 0U && (flags & 1U) == 0U) {
        return (uint32_t)(-EINVAL);
    }

    uint64_t old_aligned = page_align_up((uint64_t)old_len);
    uint64_t new_aligned = page_align_up((uint64_t)new_len);
    if (old_addr < A32_LOW_GUARD_SIZE ||
        (uint64_t)old_addr + old_aligned > (uint64_t)A32_KUSER_BASE_32) {
        return (uint32_t)(-EINVAL);
    }
    if (new_aligned == 0ULL || new_aligned > (uint64_t)A32_KUSER_BASE_32) {
        return (uint32_t)(-ENOMEM);
    }

    vm_lock_acquire();
    vm_sync_from_ctx(ctx);

    uint32_t prot = 0x3U; /* Default PROT_READ | PROT_WRITE */
    uint32_t orig_flags = 0x22U;
    char     orig_name[64];
    orig_name[0] = '\0';
    if (!vm_check_mapped_range_locked(old_addr, (uint32_t)((uint64_t)old_addr + old_aligned),
                                      &prot, &orig_flags, orig_name, sizeof(orig_name))) {
        vm_sync_to_ctx(ctx);
        vm_lock_release();
        return (uint32_t)(-EFAULT);
    }
    uint32_t keep_flags = orig_flags & 0x23U;
    uint32_t map_flags  = (((keep_flags & 0x01U) != 0U) ? 0x01U : 0x02U) | 0x20U;
    const char *region_name = (orig_name[0] != '\0') ? orig_name : "[mmap]";

    if ((flags & 2U /* MREMAP_FIXED */) == 0U) {
        if (new_aligned == old_aligned) {
            vm_sync_to_ctx(ctx);
            vm_lock_release();
            return old_addr;
        }
        if (new_aligned < old_aligned) {
            uint32_t unmap_addr = old_addr + (uint32_t)new_aligned;
            uint32_t unmap_len  = (uint32_t)(old_aligned - new_aligned);
            int rc = vm_munmap_locked(ctx, unmap_addr, unmap_len);
            vm_sync_to_ctx(ctx);
            vm_lock_release();
            if (rc != 0) {
                return (uint32_t)rc;
            }
            a32_jit_invalidate_range(ctx, unmap_addr, unmap_addr + unmap_len);
            return old_addr;
        }
    }

    if ((flags & 2U /* MREMAP_FIXED */) != 0U) {
        if ((new_addr & (A32_PAGE_SIZE - 1U)) != 0U || new_addr < A32_LOW_GUARD_SIZE ||
            (uint64_t)new_addr + new_aligned > (uint64_t)A32_KUSER_BASE_32) {
            vm_sync_to_ctx(ctx);
            vm_lock_release();
            return (uint32_t)(-EINVAL);
        }
        if ((uint64_t)new_addr < (uint64_t)old_addr + old_aligned &&
            (uint64_t)old_addr < (uint64_t)new_addr + new_aligned) {
            vm_sync_to_ctx(ctx);
            vm_lock_release();
            return (uint32_t)(-EINVAL);
        }

        uint32_t rw_prot = (prot & ~((uint32_t)PROT_EXEC)) | (uint32_t)(PROT_READ | PROT_WRITE);
        uint32_t mapped = vm_mmap2_locked(ctx, new_addr, (uint32_t)new_aligned,
                                          rw_prot, map_flags | 0x10U,
                                          -1, 0U);
        if (mapped > 0xFFFFF000U) {
            vm_sync_to_ctx(ctx);
            vm_lock_release();
            return mapped;
        }
        if ((prot & 0x1U) == 0U) {
            (void)mprotect(a32_guest_ptr(ctx, old_addr), (size_t)old_aligned, PROT_READ);
        }
        size_t copy_bytes = (size_t)((old_aligned < new_aligned) ? old_aligned : new_aligned);
        memcpy(a32_guest_ptr(ctx, mapped), a32_guest_ptr(ctx, old_addr), copy_bytes);
        if (prot != 0x3U) {
            (void)vm_mprotect_locked(ctx, mapped, (uint32_t)new_aligned, prot);
        }
        (void)vm_record_region_locked(mapped, (uint32_t)((uint64_t)mapped + new_aligned),
                                      prot, keep_flags, region_name);
        (void)vm_munmap_locked(ctx, old_addr, (uint32_t)old_aligned);
        vm_sync_to_ctx(ctx);
        vm_lock_release();

        a32_jit_invalidate_range(ctx, old_addr, (uint32_t)((uint64_t)old_addr + old_aligned));
        a32_jit_invalidate_range(ctx, mapped, (uint32_t)((uint64_t)mapped + new_aligned));
        return mapped;
    }

    /* Growing without MREMAP_FIXED: try in-place first if free */
    uint32_t old_end = (uint32_t)((uint64_t)old_addr + old_aligned);
    uint64_t new_end = (uint64_t)old_addr + new_aligned;
    if (new_end <= (uint64_t)A32_KUSER_BASE_32 &&
        !vm_range_overlaps_locked(old_end, (uint32_t)new_end)) {
        uint32_t ext = vm_mmap2_locked(ctx, old_end, (uint32_t)(new_aligned - old_aligned),
                                       prot, map_flags | 0x10U,
                                       -1, 0U);
        if (ext <= 0xFFFFF000U) {
            (void)vm_record_region_locked(old_end, (uint32_t)new_end,
                                          prot, keep_flags, region_name);
            uint32_t ext_end = (uint32_t)new_end;
            if (ext_end > g_vm_mmap_cursor && ext_end <= A32_MMAP_TOP_32) {
                g_vm_mmap_cursor = ext_end;
            }
            vm_sync_to_ctx(ctx);
            vm_lock_release();
            return old_addr;
        }
    }

    if ((flags & 1U /* MREMAP_MAYMOVE */) == 0U) {
        vm_sync_to_ctx(ctx);
        vm_lock_release();
        return (uint32_t)(-ENOMEM);
    }

    uint32_t rw_prot = (prot & ~((uint32_t)PROT_EXEC)) | (uint32_t)(PROT_READ | PROT_WRITE);
    uint32_t new_va = vm_mmap2_locked(ctx, 0U, (uint32_t)new_aligned,
                                      rw_prot, map_flags,
                                      -1, 0U);
    if (new_va > 0xFFFFF000U) {
        vm_sync_to_ctx(ctx);
        vm_lock_release();
        return new_va;
    }

    if ((prot & 0x1U) == 0U) {
        (void)mprotect(a32_guest_ptr(ctx, old_addr), (size_t)old_aligned, PROT_READ);
    }
    memcpy(a32_guest_ptr(ctx, new_va), a32_guest_ptr(ctx, old_addr), (size_t)old_aligned);
    if (prot != 0x3U) {
        (void)vm_mprotect_locked(ctx, new_va, (uint32_t)new_aligned, prot);
    }
    (void)vm_record_region_locked(new_va, (uint32_t)((uint64_t)new_va + new_aligned),
                                  prot, keep_flags, region_name);
    (void)vm_munmap_locked(ctx, old_addr, (uint32_t)old_aligned);
    vm_sync_to_ctx(ctx);
    vm_lock_release();

    a32_jit_invalidate_range(ctx, old_addr, (uint32_t)((uint64_t)old_addr + old_aligned));
    return new_va;
}

void a32_vm_sync_ctx(a32_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }
    vm_lock_acquire();
    vm_sync_from_ctx(ctx);
    vm_sync_to_ctx(ctx);
    vm_lock_release();
}
