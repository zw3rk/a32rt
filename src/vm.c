/*
 * src/vm.c — Guest 4 GiB window [G, G+4GiB) reservation and guest VMA manager
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#define _GNU_SOURCE
#include "a32rt.h"
#include "os/backend.h"
#include "vm/vm32.h"
#include "vm/vm_priv.h"

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

#define A32_WINDOW_SIZE         0x100000000ULL

static _Atomic uint32_t g_vm_lock = 0U;
static a32_ctx_t       *g_vm_root_ctx = NULL;
static uint32_t         g_vm_brk_base = 0U;
static uint32_t         g_vm_brk_curr = 0U;
static uint32_t         g_vm_mmap_cursor = A32_MMAP_BASE_32;
static a32_region_t     g_vm_regions[A32_MAX_REGIONS];
static size_t           g_vm_region_count = 0U;

void a32_vm_lock_acquire(void)
{
    a32_spin_lock(&g_vm_lock);
}

void a32_vm_lock_release(void)
{
    a32_spin_unlock(&g_vm_lock);
}

/* Fork-child reset (A32-R3-4): never inherit a VM lock held by a thread
 * that did not survive fork(). */
void a32_vm_fork_child_reset(void)
{
    atomic_store_explicit(&g_vm_lock, 0U, memory_order_release);
}

static int vm_reserve_prot_none(a32_ctx_t *ctx, uint32_t start, uint32_t len)
{
    void *out = NULL;
    return a32_backend_map_anon(a32_guest_ptr(ctx, start), (size_t)len,
        0, 1, 1, 0, &out);
}

void a32_vm_invalidate_jit(a32_ctx_t *ctx, uint32_t start, uint32_t end)
{
    uint32_t inv_start = (start >= 4U) ? (start - 4U) : 0U;
    (void)a32_jit_invalidate_range(ctx, inv_start, end);
}

void a32_vm_note_grown(uint32_t old_addr, uint32_t ext_end)
{
    if (g_vm_mmap_cursor >= old_addr && g_vm_mmap_cursor < ext_end &&
        ext_end <= A32_MMAP_TOP_32) {
        g_vm_mmap_cursor = ext_end;
    }
}

void a32_vm_sync_from_ctx(a32_ctx_t *ctx)
{
    if (ctx->brk_base != 0U && g_vm_brk_base != ctx->brk_base) {
        g_vm_brk_base = ctx->brk_base;
        g_vm_brk_curr = ctx->brk_curr;
    }
}

static void vm_sync_brk_to_ctx(a32_ctx_t *ctx)
{
    if (g_vm_brk_base != 0U) {
        ctx->brk_base = g_vm_brk_base;
        ctx->brk_curr = g_vm_brk_curr;
        if (g_vm_root_ctx != NULL && g_vm_root_ctx != ctx) {
            g_vm_root_ctx->brk_base = g_vm_brk_base;
            g_vm_root_ctx->brk_curr = g_vm_brk_curr;
        }
    }
}

static void vm_copy_state_to_ctx(a32_ctx_t *dst)
{
    if (g_vm_brk_base != 0U) {
        dst->brk_base = g_vm_brk_base;
        dst->brk_curr = g_vm_brk_curr;
    }
    size_t count = (g_vm_region_count <= A32_MAX_REGIONS) ? g_vm_region_count : A32_MAX_REGIONS;
    dst->mmap_cursor  = g_vm_mmap_cursor;
    dst->region_count = count;
    if (count > 0U) {
        memcpy(dst->regions, g_vm_regions, count * sizeof(a32_region_t));
    }
}

void a32_vm_sync_to_ctx(a32_ctx_t *ctx)
{
    vm_copy_state_to_ctx(ctx);
    if (g_vm_root_ctx != NULL && g_vm_root_ctx != ctx) {
        vm_copy_state_to_ctx(g_vm_root_ctx);
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
    if (start >= end || g_vm_region_count >= A32_MAX_REGIONS) {
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
    if (g_vm_region_count < 2U) {
        return;
    }
    size_t dst = 0U;
    for (size_t src = 1U; src < g_vm_region_count; src++) {
        a32_region_t *prev = &g_vm_regions[dst];
        const a32_region_t *curr = &g_vm_regions[src];
        if (prev->end == curr->start && prev->prot == curr->prot &&
            prev->flags == curr->flags && strcmp(prev->name, curr->name) == 0 &&
            a32_vmfile_can_coalesce(prev->end, curr->start)) {
            prev->end = curr->end;
        } else {
            dst++;
            if (dst != src) {
                g_vm_regions[dst] = *curr;
            }
        }
    }
    g_vm_region_count = dst + 1U;
}

/* Retire the previous mapping over [unmap_start, unmap_end): VMA
 * regions, Soft-TLB translations, and file tracking (A32-R3-1, A32-R4-6).
 * The file-tracking retire runs FIRST and is transactional: exhaustion
 * or descriptor failure returns an error with no VMA, host, or tracking
 * state changed, so the caller can abort before mutating anything. */
static int vm_unmap_regions_locked(uint32_t unmap_start, uint32_t unmap_end)
{
    int rc;
    size_t i;
    if (unmap_end <= unmap_start) {
        return -EINVAL;
    }
    rc = a32_vmfile_preflight_unmap(unmap_start, unmap_end);
    if (rc != 0) {
        return rc;
    }
    rc = a32_vmfile_unmap_shared(unmap_start, unmap_end);
    if (rc != 0) {
        return rc;
    }
    i = 0U;
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
            if (g_vm_region_count + 1U >= A32_MAX_REGIONS) {
                size_t before = g_vm_region_count;
                vm_coalesce_regions_locked();
                if (g_vm_region_count != before) {
                    i = 0U;
                    continue;
                }
            }
            a32_region_t orig = *r;
            if (vm_insert_region_raw(unmap_end, orig.end,
                                     orig.prot, orig.flags, orig.name) == 0) {
                g_vm_regions[i].end = unmap_start;
                i += 2U;
            } else {
                i++;
            }
            continue;
        }
        if (r->start < unmap_start) {
            r->end = unmap_start;
        } else {
            r->start = unmap_end;
        }
        i++;
    }
    /* Wave U3 A32-4: drop Soft-TLB translations for the unmapped span
     * (file tracking retired above, before any VMA mutation). */
    a32_vm32_sync_unmap(unmap_start, unmap_end);
    return 0;
}

/* Pure insertion (A32-R3-1): retiring the previous mapping — regions,
 * Soft-TLB translations, and file tracking — is the caller's job and
 * happens BEFORE the new host mapping is installed, so registration can
 * never delete backing metadata it just created. */
int a32_vm_record_region_locked(uint32_t start, uint32_t end,
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
    uint32_t clean_flags = flags & ~(A32_MAP_FIXED | A32_MAP_FIXED_NOREPLACE);
    int rc = vm_insert_region_raw(start, end, prot, clean_flags, name_copy);
    if (rc < 0) {
        vm_coalesce_regions_locked();
        rc = vm_insert_region_raw(start, end, prot, clean_flags, name_copy);
    }
    vm_coalesce_regions_locked();
    /* Wave U3 A32-4: publish the Soft-TLB translation alongside the region
     * record so Regime A2 stays coherent without a second walk. */
    if (rc == 0) {
        a32_vm32_sync_region(start, end, (prot & 0x1U) != 0U,
            (prot & 0x2U) != 0U, (prot & 0x4U) != 0U);
    }
    return rc;
}

/* Protection-change pre-flight (A32-R4-8): count the VMA entries a
 * partial mprotect over [mp_start, mp_end) must allocate (a middle
 * split needs two, an edge split one, a fully covered region none)
 * and refuse with -ENOMEM when the region table cannot represent the
 * result. Call BEFORE changing any host permission so a capacity
 * failure leaves every table and mapping untouched. */
static int vm_mprotect_preflight_locked(uint32_t mp_start, uint32_t mp_end)
{
    size_t need = 0U;
    if (mp_end <= mp_start) {
        return -EINVAL;
    }
    for (size_t i = 0U; i < g_vm_region_count; i++) {
        const a32_region_t *r = &g_vm_regions[i];
        if (r->end <= mp_start || r->start >= mp_end) {
            continue;
        }
        if (r->start >= mp_start && r->end <= mp_end) {
            continue;
        }
        if (r->start < mp_start && r->end > mp_end) {
            need += 2U;
        } else {
            need += 1U;
        }
    }
    if ((uint64_t)g_vm_region_count + (uint64_t)need > (uint64_t)A32_MAX_REGIONS) {
        return -ENOMEM;
    }
    return 0;
}

static int vm_mprotect_regions_locked(uint32_t mp_start, uint32_t mp_end, uint32_t prot)
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
        a32_region_t orig = *r;
        if (orig.start < mp_start && orig.end > mp_end) {
            /* Pre-flight guaranteed two free entries; a failure here
             * leaves the region untouched rather than widening the
             * metadata beyond the requested range (A32-R4-8). */
            r->end = mp_start;
            if (vm_insert_region_raw(mp_start, mp_end, prot, orig.flags, orig.name) != 0 ||
                vm_insert_region_raw(mp_end, orig.end, orig.prot, orig.flags, orig.name) != 0) {
                *r = orig;
                return -ENOMEM;
            }
            i += 3U;
        } else if (orig.start < mp_start) {
            if (vm_insert_region_raw(mp_start, orig.end, prot, orig.flags, orig.name) == 0) {
                g_vm_regions[i].end = mp_start;
                i += 2U;
            } else {
                return -ENOMEM;
            }
        } else {
            r->start = mp_end;
            if (vm_insert_region_raw(orig.start, mp_end, prot, orig.flags, orig.name) == 0) {
                i += 2U;
            } else {
                r->start = orig.start;
                return -ENOMEM;
            }
        }
    }
    vm_coalesce_regions_locked();
    /* Wave U3 A32-4: re-publish the Soft-TLB translation under the new
     * protections so Regime A2 faults match the guest view. */
    a32_vm32_sync_region(mp_start, mp_end, (prot & 0x1U) != 0U,
        (prot & 0x2U) != 0U, (prot & 0x4U) != 0U);
    return 0;
}

bool a32_vm_range_overlaps(uint32_t start, uint32_t end)
{
    size_t hp = a32_backend_page_size();
    for (size_t i = 0U; i < g_vm_region_count && g_vm_regions[i].start < end; i++) {
        uint64_t re = (uint64_t)g_vm_regions[i].end;
        if (hp > 4096U && (g_vm_regions[i].flags & 0x01U) != 0U) { re = (re + hp - 1U) & ~(hp - 1U); }
        if (re > (uint64_t)start) { return true; }
    }
    return false;
}

bool a32_vm_mapped_range(uint32_t start, uint32_t end,
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
        } else if ((out_prot != NULL && r->prot != first_prot) ||
                   (out_flags != NULL && (r->flags & 0x23U) != first_flags)) {
            return false;
        }
        if (r->end >= end) {
            return true;
        }
        cur = r->end;
    }
    return false;
}

static int vm_find_free_gap_locked(uint64_t aligned_len, uint64_t align, uint32_t *out_va)
{
    if (aligned_len == 0ULL || aligned_len > (uint64_t)(A32_MMAP_TOP_32 - A32_MMAP_BASE_32)) {
        return -1;
    }
    uint64_t cursor_cand = (a32_vm_page_align_up((uint64_t)g_vm_mmap_cursor) + align - 1ULL) & ~(align - 1ULL);
    if (cursor_cand < (uint64_t)A32_MMAP_BASE_32) {
        cursor_cand = (uint64_t)A32_MMAP_BASE_32;
    }
    if (cursor_cand + aligned_len <= (uint64_t)A32_MMAP_TOP_32 &&
        !a32_vm_range_overlaps((uint32_t)cursor_cand, (uint32_t)(cursor_cand + aligned_len))) {
        *out_va = (uint32_t)cursor_cand;
        return 0;
    }
    uint64_t cand = (uint64_t)A32_MMAP_BASE_32;
    for (size_t i = 0U; i < g_vm_region_count; i++) {
        uint64_t r_align = ((g_vm_regions[i].flags & 0x01U) != 0U)
            ? (uint64_t)a32_backend_page_size() : align;
        uint64_t r_start = (uint64_t)g_vm_regions[i].start;
        uint64_t r_end   = (a32_vm_page_align_up((uint64_t)g_vm_regions[i].end) + r_align - 1ULL) & ~(r_align - 1ULL);
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
    } else if (try_reserve_window(0ULL) == 0) {
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

int a32_vm_record_region(a32_ctx_t *ctx, uint32_t start, uint32_t end,
                         uint32_t prot, uint32_t flags, const char *name)
{
    if (!ctx || start >= end) {
        return -1;
    }
    a32_vm_lock_acquire();
    a32_vm_sync_from_ctx(ctx);
    int rc = a32_vm_record_region_locked(start, end, prot, flags, name);
    a32_vm_sync_to_ctx(ctx);
    a32_vm_lock_release();
    return rc;
}

uint32_t a32_vm_brk(a32_ctx_t *ctx, uint32_t new_brk)
{
    if (!ctx) {
        return 0U;
    }
    a32_vm_lock_acquire();
    a32_vm_sync_from_ctx(ctx);

    uint32_t brk_limit = (g_vm_brk_base < A32_MMAP_BASE_32) ? A32_MMAP_BASE_32 : A32_MMAP_TOP_32;
    if (new_brk == 0U || new_brk < g_vm_brk_base || new_brk > brk_limit) {
        uint32_t curr = g_vm_brk_curr;
        vm_sync_brk_to_ctx(ctx);
        a32_vm_lock_release();
        return curr;
    }

    uint64_t old_page64 = a32_vm_page_align_up((uint64_t)g_vm_brk_curr);
    uint64_t new_page64 = a32_vm_page_align_up((uint64_t)new_brk);
    if (new_page64 > (uint64_t)brk_limit) {
        uint32_t curr = g_vm_brk_curr;
        vm_sync_brk_to_ctx(ctx);
        a32_vm_lock_release();
        return curr;
    }

    uint32_t old_page = (uint32_t)old_page64;
    uint32_t new_page = (uint32_t)new_page64;

    if (new_page > old_page) {
        if (a32_vm_range_overlaps(old_page, new_page)) {
            uint32_t curr = g_vm_brk_curr;
            vm_sync_brk_to_ctx(ctx);
            a32_vm_lock_release();
            return curr;
        }
        uint32_t len = new_page - old_page;
        void *host_addr = a32_guest_ptr(ctx, old_page);
        void *res = NULL;
        if (a32_backend_map_anon(host_addr, len, PROT_READ | PROT_WRITE,
                1, 1, 0, &res) != 0 || res != host_addr) {
            uint32_t curr = g_vm_brk_curr;
            vm_sync_brk_to_ctx(ctx);
            a32_vm_lock_release();
            return curr;
        }
        if (a32_vm_record_region_locked(old_page, new_page,
                (uint32_t)(PROT_READ | PROT_WRITE), 0x22U, "[heap]") != 0) {
            /* Region table full: release the pages again rather than
             * leaving mapped memory untracked. */
            (void)a32_vm_unmap_locked(ctx, old_page, len);
            uint32_t curr = g_vm_brk_curr;
            vm_sync_brk_to_ctx(ctx);
            a32_vm_lock_release();
            return curr;
        }
    } else if (new_page < old_page) {
        uint32_t len = old_page - new_page;
        /* A32-R4-6: retire tracking/VMA first so a capacity failure
         * keeps brk intact; the PROT_NONE re-reserve below then only
         * preserves the window reservation. */
        if (vm_unmap_regions_locked(new_page, old_page) != 0) {
            uint32_t curr = g_vm_brk_curr;
            vm_sync_brk_to_ctx(ctx);
            a32_vm_lock_release();
            return curr;
        }
        /* Re-reserve shrunk brk pages with PROT_NONE inside the guest window */
        (void)vm_reserve_prot_none(ctx, new_page, len);
    }

    g_vm_brk_curr = new_brk;
    if (new_page != old_page) {
        a32_vm_sync_to_ctx(ctx);
    } else {
        vm_sync_brk_to_ctx(ctx);
    }
    a32_vm_lock_release();

    if (new_page < old_page) {
        a32_vm_invalidate_jit(ctx, new_page, old_page);
    }
    return new_brk;
}

uint32_t a32_vm_mmap_locked(a32_ctx_t *ctx, uint32_t addr, uint32_t len, uint32_t prot,
                                uint32_t flags, int fd, uint32_t pgoffset,
                                const char *name)
{
    if (!ctx || len == 0U || (flags & 0x03U) == 0U || (flags & 0x03U) == 0x03U) {
        return (uint32_t)(-EINVAL);
    }

    uint64_t aligned_len = a32_vm_page_align_up((uint64_t)len);
    if (aligned_len == 0ULL || aligned_len > (uint64_t)A32_KUSER_BASE_32) {
        return (uint32_t)(-ENOMEM);
    }

    int host_flags = ((flags & 0x01U) ? MAP_SHARED : 0) |
                     ((flags & 0x02U) ? MAP_PRIVATE : 0) |
                     ((flags & A32_MAP_ANONYMOUS) ? MAP_ANONYMOUS : 0);
    uint64_t gap_align = ((host_flags & MAP_SHARED) != 0)
        ? (uint64_t)a32_backend_page_size() : (uint64_t)A32_PAGE_SIZE;
    uint64_t gap_len = (aligned_len + gap_align - 1ULL) & ~(gap_align - 1ULL);
    uint32_t target_va     = 0U;
    bool     update_cursor = false;
    if ((flags & (A32_MAP_FIXED | A32_MAP_FIXED_NOREPLACE)) != 0U) {
        if ((addr & (A32_PAGE_SIZE - 1U)) != 0U || addr < A32_LOW_GUARD_SIZE ||
            ((uint64_t)addr & (gap_align - 1ULL)) != 0ULL ||
            ((host_flags & (MAP_SHARED | MAP_ANONYMOUS)) == MAP_SHARED &&
             (((uint64_t)pgoffset * A32_PAGE_SIZE) & (gap_align - 1ULL)) != 0ULL)) {
            return (uint32_t)(-EINVAL);
        }
        if ((uint64_t)addr + gap_len > (uint64_t)A32_KUSER_BASE_32) {
            return (uint32_t)(-ENOMEM);
        }
        if ((flags & A32_MAP_FIXED_NOREPLACE) != 0U &&
            a32_vm_range_overlaps(addr, (uint32_t)((uint64_t)addr + gap_len))) {
            return (uint32_t)(-EEXIST);
        }
        if (gap_len > aligned_len &&
            a32_vm_range_overlaps(addr + (uint32_t)aligned_len,
                                     (uint32_t)((uint64_t)addr + gap_len))) {
            return (uint32_t)(-EINVAL);
        }
        target_va = addr;
    } else {
        uint32_t hint_va = (uint32_t)((uint64_t)a32_page_down(addr) & ~(gap_align - 1ULL));
        if (addr != 0U && hint_va >= A32_MMAP_BASE_32 &&
            (uint64_t)hint_va + gap_len <= (uint64_t)A32_MMAP_TOP_32 &&
            !a32_vm_range_overlaps(hint_va, (uint32_t)((uint64_t)hint_va + gap_len))) {
            target_va = hint_va;
        } else if (vm_find_free_gap_locked(gap_len, gap_align, &target_va) != 0) {
            return (uint32_t)(-ENOMEM);
        } else {
            update_cursor = true;
        }
    }

    bool is_file = ((host_flags & MAP_ANONYMOUS) == 0 && fd >= 0);
    bool is_shared = ((host_flags & MAP_SHARED) != 0);
    uint32_t target_end = (uint32_t)((uint64_t)target_va + aligned_len);
    /* MAP_FIXED replacement pre-flight (A32-R3-1 follow-up 2): when the
     * new mapping needs file tracking, verify BEFORE retiring that the
     * retire frees enough slots for the replacement record, so a failed
     * capacity check cannot leave the old range mapped but untracked. */
    if (is_file &&
        a32_vmfile_check_replace(target_va, target_end, is_shared) != 0) {
        return (uint32_t)(-ENOMEM);
    }
    /* A32-R3-1: retire the previous mapping (regions, Soft-TLB, file
     * tracking) BEFORE installing the new host mapping, then publish the
     * new region and its backing together. Retiring first also frees the
     * old slots, mirroring MAP_FIXED replace semantics. The retire is
     * transactional (A32-R4-6): failure leaves every table and the host
     * mapping untouched, so the error propagates before anything is
     * installed. */
    {
        int retire_rc = vm_unmap_regions_locked(target_va, target_end);
        if (retire_rc != 0) {
            return (uint32_t)retire_rc;
        }
    }
    /* A32-R3-5: refuse before installing the new host mapping when file
     * tracking cannot be maintained. */
    if (is_file &&
        a32_vmfile_check_record(target_va, is_shared) != 0) {
        return (uint32_t)(-ENOMEM);
    }
    int   host_prot   = a32_host_prot(ctx, prot);
    void *host_target = a32_guest_ptr(ctx, target_va);
    off_t host_off    = (off_t)pgoffset * (off_t)A32_PAGE_SIZE;
    void *res = NULL;
    bool a2_stage = a32_vm32_current_regime() == A32_VM32_REGIME_A2_SOFT_TLB &&
        (host_flags & MAP_ANONYMOUS) == 0 && (host_flags & MAP_SHARED) == 0;
    int map_rc = a32_vmfile_map_commit(host_target, (size_t)aligned_len, host_prot,
        fd, (uint64_t)host_off, host_flags, a2_stage, &res);
    if (map_rc != 0) {
        return (uint32_t)map_rc;
    }
    if ((flags & (0x01U | A32_MAP_ANONYMOUS)) == 0x01U && fd >= 0) {
        int sh_rc = a32_vmfile_record_shared(target_va, target_end, fd,
            pgoffset);
        if (sh_rc != 0) {
            (void)a32_vm_unmap_locked(ctx, target_va, (uint32_t)aligned_len);
            return (uint32_t)sh_rc;
        }
    }
    if (a32_vm_record_region_locked(target_va, target_end,
            prot & 0x7U, flags,
            (name != NULL && name[0] != '\0') ? name : "[mmap]") != 0) {
        (void)a32_vm_unmap_locked(ctx, target_va, (uint32_t)aligned_len);
        return (uint32_t)(-ENOMEM);
    }
    if (update_cursor) {
        g_vm_mmap_cursor = (uint32_t)((uint64_t)target_va + gap_len);
    }
    return target_va;
}

uint32_t a32_vm_mmap2(a32_ctx_t *ctx, uint32_t addr, uint32_t len, uint32_t prot,
                      uint32_t flags, int fd, uint32_t pgoffset)
{
    if (!ctx) {
        return (uint32_t)(-EINVAL);
    }

    a32_vm_lock_acquire();
    a32_vm_sync_from_ctx(ctx);
    uint32_t res = a32_vm_mmap_locked(ctx, addr, len, prot, flags, fd,
        pgoffset, "[mmap]");
    a32_vm_sync_to_ctx(ctx);
    a32_vm_lock_release();

    if (res <= 0xFFFFF000U &&
        ((flags & (A32_MAP_FIXED | A32_MAP_FIXED_NOREPLACE)) != 0U ||
         (prot & (uint32_t)PROT_EXEC) != 0U)) {
        a32_vm_invalidate_jit(ctx, res, (uint32_t)((uint64_t)res + a32_vm_page_align_up((uint64_t)len)));
    }

    if (res <= 0xFFFFF000U &&
        (prot & (uint32_t)PROT_EXEC) != 0U &&
        (flags & A32_MAP_ANONYMOUS) == 0U &&
        fd >= 0) {
        uint32_t load_bias = res - (pgoffset * A32_PAGE_SIZE);
        (void)a32_aot_load_fd(ctx, fd, load_bias);
    }

    return res;
}

int a32_vm_protect_locked(a32_ctx_t *ctx, uint32_t addr, uint32_t len, uint32_t prot)
{
    if (!ctx || (addr & (A32_PAGE_SIZE - 1U)) != 0U || addr < 0x1000U || (prot & ~0x7U) != 0U) {
        return -EINVAL;
    }
    if (len == 0U) {
        return 0;
    }
    uint64_t aligned_len = a32_vm_page_align_up((uint64_t)len);
    uint64_t end64       = (uint64_t)addr + aligned_len;
    if (addr < A32_LOW_GUARD_SIZE ||
        aligned_len == 0ULL ||
        end64 > (uint64_t)A32_KUSER_BASE_32 ||
        !a32_vm_mapped_range(addr, (uint32_t)end64, NULL, NULL, NULL, 0U)) {
        return -ENOMEM;
    }

    /* A32-R4-8: verify the region table can represent the split
     * protection change BEFORE touching host permissions, so -ENOMEM
     * leaves every table, translation, and host page untouched. */
    int pre_rc = vm_mprotect_preflight_locked(addr, (uint32_t)end64);
    if (pre_rc != 0) {
        return pre_rc;
    }
    int prot_rc = a32_vm32_protect(ctx, addr, (size_t)aligned_len, prot);
    if (prot_rc != 0) {
        return prot_rc;
    }
    if ((prot & (uint32_t)PROT_WRITE) != 0U) {
        a32_vmfile_promote_range(addr, (uint32_t)end64, (prot & 0x1U) != 0U, true);
    }

    return vm_mprotect_regions_locked(addr, (uint32_t)end64, prot & 0x7U);
}

int a32_vm_mprotect(a32_ctx_t *ctx, uint32_t addr, uint32_t len, uint32_t prot)
{
    if (!ctx) {
        return -EINVAL;
    }

    a32_vm_lock_acquire();
    a32_vm_sync_from_ctx(ctx);
    int rc = a32_vm_protect_locked(ctx, addr, len, prot);
    a32_vm_sync_to_ctx(ctx);
    a32_vm_lock_release();

    if (rc == 0 && len > 0U) {
        a32_vm_invalidate_jit(ctx, addr, (uint32_t)((uint64_t)addr + a32_vm_page_align_up((uint64_t)len)));
    }
    return rc;
}

int a32_vm_unmap_locked(a32_ctx_t *ctx, uint32_t addr, uint32_t len)
{
    if (!ctx || (addr & (A32_PAGE_SIZE - 1U)) != 0U || len == 0U) {
        return -EINVAL;
    }
    uint64_t aligned_len = a32_vm_page_align_up((uint64_t)len);
    if (addr < A32_LOW_GUARD_SIZE ||
        (uint64_t)addr + aligned_len > (uint64_t)A32_KUSER_BASE_32) {
        return -EINVAL;
    }

    uint32_t start = a32_page_down(addr);
    uint32_t end = (uint32_t)((uint64_t)start + aligned_len);
    /* A32-R4-6: retire tracking/VMA state BEFORE the host mapping
     * changes, so exhaustion or descriptor failure aborts with the
     * mapping (and its backing records) intact. */
    int rc = vm_unmap_regions_locked(start, end);
    if (rc != 0) {
        return rc;
    }
    /* Keep the [G, G+4GiB) reservation intact by replacing unmapped pages with PROT_NONE */
    rc = vm_reserve_prot_none(ctx, start, (uint32_t)aligned_len);
    if (rc != 0) {
        return rc;
    }

    if (g_vm_mmap_cursor == end && start >= A32_MMAP_BASE_32) {
        g_vm_mmap_cursor = start;
    }
    return 0;
}

int a32_vm_munmap(a32_ctx_t *ctx, uint32_t addr, uint32_t len)
{
    if (!ctx) {
        return -EINVAL;
    }

    a32_vm_lock_acquire();
    a32_vm_sync_from_ctx(ctx);
    int rc = a32_vm_unmap_locked(ctx, addr, len);
    a32_vm_sync_to_ctx(ctx);
    a32_vm_lock_release();

    if (rc == 0) {
        a32_vm_invalidate_jit(ctx, addr, (uint32_t)((uint64_t)addr + a32_vm_page_align_up((uint64_t)len)));
    }
    return rc;
}


void a32_vm_sync_ctx(a32_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }
    a32_vm_lock_acquire();
    a32_vm_sync_from_ctx(ctx);
    a32_vm_sync_to_ctx(ctx);
    a32_vm_lock_release();
}
