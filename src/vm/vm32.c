/*
 * src/vm/vm32.c -- Two-regime Class-A 32-bit VM core (Wave U3 A32-4)
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regime selection, canonical HVA, 7-instruction Soft-TLB data side, and
 * the shared LDREX/STREX reservation table. Both hosts are little-endian
 * (aarch64 Linux and arm64 Darwin), matching the LE a32 guest, so the
 * atomic path needs no byte-swap. Curated C11, no inline asm.
 */

#include "vm/vm32.h"

#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static a32_vm32_tlb_entry_t g_tlb[A32_VM32_TLB_SIZE];

typedef struct {
    _Atomic uint64_t epoch;
    const a32_ctx_t *owner;
    uint32_t addr;
    uint32_t size;
    uint32_t val;
} a32_ll_line_t;

static a32_ll_line_t g_ll[A32_VM32_LL_LINES];
static a32_vm32_regime_t g_regime = A32_VM32_REGIME_UNSET;
static uintptr_t g_arena_base = 0U;

a32_vm32_regime_t a32_vm32_select_regime(size_t host_page_size)
{
    if (host_page_size == 4096U) {
        return A32_VM32_REGIME_A1_DIRECT;
    }
    if (host_page_size == 16384U || host_page_size == 65536U) {
        return A32_VM32_REGIME_A2_SOFT_TLB;
    }
    return A32_VM32_REGIME_UNSET;
}

a32_vm32_regime_t a32_vm32_current_regime(void)
{
    return g_regime;
}

int a32_vm32_init(a32_ctx_t *ctx)
{
    long h;
    if (ctx == NULL) {
        return -EINVAL;
    }
    h = sysconf(_SC_PAGESIZE);
    if (h != 4096L && h != 16384L && h != 65536L) {
        return -ENOSYS;
    }
    g_regime = a32_vm32_select_regime((size_t)h);
    g_arena_base = (uintptr_t)ctx->guest_base;
    a32_vm32_tlb_invalidate_all();
    memset(g_ll, 0, sizeof(g_ll));
    return 0;
}

void *a32_vm32_hva(const a32_ctx_t *ctx, uint32_t gva)
{
    uintptr_t base;
    if (ctx == NULL) {
        base = g_arena_base;
    } else {
        base = (uintptr_t)ctx->guest_base;
    }
    return (void *)(base + (uintptr_t)gva);
}

uintptr_t a32_vm32_arena_base(void)
{
    return g_arena_base;
}

/* ---- Soft-TLB data side ---- */

static inline uint32_t tlb_index(uint32_t gva)
{
    return (gva >> 12U) & (A32_VM32_TLB_SIZE - 1U);
}

bool a32_vm32_tlb_probe(uint32_t gva, bool is_store, uint32_t width,
    uintptr_t *addend_out)
{
    uint32_t idx;
    uint64_t tag;
    if (width != 1U && width != 2U && width != 4U && width != 8U &&
        width != 16U) {
        return false;
    }
    if ((gva & (width - 1U)) != 0U) {
        return false;
    }
    idx = tlb_index(gva);
    tag = is_store ? g_tlb[idx].write_tag : g_tlb[idx].read_tag;
    if (tag != (uint64_t)(gva & ~0xFFFU)) {
        return false;
    }
    if (width > 1U && ((gva & 0xFFFU) + width - 1U) >> 12U != 0U) {
        uint32_t idx2 = (idx + 1U) & (A32_VM32_TLB_SIZE - 1U);
        uint64_t tag2 = is_store ? g_tlb[idx2].write_tag : g_tlb[idx2].read_tag;
        if (tag2 != (uint64_t)(((gva & ~0xFFFU) + 0x1000U) & 0xFFFFF000U)) {
            return false;
        }
    }
    if (addend_out != NULL) {
        *addend_out = g_tlb[idx].addend;
    }
    return true;
}

void a32_vm32_tlb_fill(uint32_t gva_page, uintptr_t host_page_base,
    bool readable, bool writable)
{
    uint32_t idx = tlb_index(gva_page);
    uint64_t vpn = (uint64_t)(gva_page & ~0xFFFU);
    g_tlb[idx].addend = host_page_base - (uintptr_t)vpn;
    g_tlb[idx].read_tag = readable ? vpn : A32_VM32_TLB_INVALID_TAG;
    g_tlb[idx].write_tag = writable ? vpn : A32_VM32_TLB_INVALID_TAG;
    g_tlb[idx].flags_reserved = 0U;
    /* A remap breaks any LDREX reservation on this page. */
    atomic_fetch_add(&g_ll[tlb_index(gva_page)].epoch, 1ULL);
}

void a32_vm32_tlb_invalidate_range(uint32_t start_gva, uint32_t end_gva)
{
    uint32_t start = start_gva & ~0xFFFU;
    uint32_t end = (end_gva + 0xFFFU) & ~0xFFFU;
    uint32_t pages = (end > start) ? (end - start) >> 12U : 0U;
    uint32_t i;
    if (pages >= 512U) {
        a32_vm32_tlb_invalidate_all();
        return;
    }
    for (i = 0U; i < pages; i++) {
        uint32_t idx = tlb_index(start + (i << 12U));
        g_tlb[idx].read_tag = A32_VM32_TLB_INVALID_TAG;
        g_tlb[idx].write_tag = A32_VM32_TLB_INVALID_TAG;
        g_tlb[idx].flags_reserved = 0U;
    }
}

void a32_vm32_tlb_invalidate_all(void)
{
    uint32_t i;
    for (i = 0U; i < A32_VM32_TLB_SIZE; i++) {
        g_tlb[i].read_tag = A32_VM32_TLB_INVALID_TAG;
        g_tlb[i].write_tag = A32_VM32_TLB_INVALID_TAG;
        g_tlb[i].addend = 0U;
        g_tlb[i].flags_reserved = 0U;
    }
}

int a32_vm32_tlb_miss(a32_ctx_t *ctx, uint32_t gva, bool is_store,
    void **hva_out)
{
    uint32_t idx;
    uint64_t tag;
    (void)ctx;
    if (hva_out == NULL) {
        return -EINVAL;
    }
    idx = tlb_index(gva);
    tag = is_store ? g_tlb[idx].write_tag : g_tlb[idx].read_tag;
    if (tag != (uint64_t)(gva & ~0xFFFU)) {
        return -EFAULT;
    }
    *hva_out = (void *)(g_tlb[idx].addend + (uintptr_t)gva);
    return 0;
}

/* Publish translations for [start_gva, end_gva) after a successful
 * mmap/mprotect (host base == arena base + page in both regimes). */
void a32_vm32_sync_region(uint32_t start_gva, uint32_t end_gva,
    bool readable, bool writable)
{
    uint32_t page;
    if (g_arena_base == 0U) {
        return;
    }
    for (page = start_gva & ~0xFFFU; page < end_gva; page += 0x1000U) {
        a32_vm32_tlb_fill(page, g_arena_base + (uintptr_t)page,
            readable, writable);
        if (page + 0x1000U < page) {
            break;
        }
    }
}

/* Drop translations for [start_gva, end_gva) after munmap/mremap. */
void a32_vm32_sync_unmap(uint32_t start_gva, uint32_t end_gva)
{
    a32_vm32_tlb_invalidate_range(start_gva, end_gva);
}

/* ---- Shared LDREX/STREX reservation table ---- */

static inline uint32_t ll_index(uint32_t gva_aligned)
{
    return (gva_aligned >> A32_VM32_LL_LINE_SHIFT) & (A32_VM32_LL_LINES - 1U);
}

void a32_vm32_ll_record(a32_ctx_t *ctx, uint32_t gva, unsigned size,
    uint32_t val)
{
    uint32_t h = ll_index(gva & ~3U);
    g_ll[h].owner = ctx;
    g_ll[h].addr = gva;
    g_ll[h].size = (uint32_t)size;
    g_ll[h].val = val;
    if (ctx != NULL) {
        ctx->ll_epoch_snap = atomic_load(&g_ll[h].epoch);
    }
}

void a32_vm32_reservation_bump(uint32_t gva_aligned)
{
    uint32_t h = ll_index(gva_aligned);
    atomic_fetch_add(&g_ll[h].epoch, 1ULL);
    g_ll[h].owner = NULL;
}

bool a32_vm32_sc_attempt(a32_ctx_t *ctx, uint32_t gva, unsigned size,
    uint32_t val)
{
    uint32_t h = ll_index(gva & ~3U);
    uint64_t snap;
    uint32_t cur = 0U;
    void *hva;
    bool ok = false;
    if (ctx == NULL) {
        return false;
    }
    if (size != 1U && size != 2U && size != 4U) {
        return false;
    }
    if ((gva & (size - 1U)) != 0U) {
        return false;
    }
    if (g_ll[h].owner != ctx || g_ll[h].addr != gva || g_ll[h].size != size) {
        goto consume;
    }
    snap = atomic_load(&g_ll[h].epoch);
    if (snap != ctx->ll_epoch_snap) {
        goto consume;
    }
    hva = (void *)((uintptr_t)ctx->guest_base + (uintptr_t)gva);
    if (size == 1U) {
        cur = __atomic_load_n((uint8_t *)hva, __ATOMIC_SEQ_CST);
    } else if (size == 2U) {
        cur = __atomic_load_n((uint16_t *)(void *)hva, __ATOMIC_SEQ_CST);
    } else {
        cur = __atomic_load_n((uint32_t *)(void *)hva, __ATOMIC_SEQ_CST);
    }
    if (cur != g_ll[h].val ||
        snap != atomic_load(&g_ll[h].epoch)) {
        goto consume;
    }
    if (size == 1U) {
        uint8_t e = (uint8_t)cur;
        ok = __atomic_compare_exchange_n((uint8_t *)hva, &e, (uint8_t)val,
            false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    } else if (size == 2U) {
        uint16_t e = (uint16_t)cur;
        ok = __atomic_compare_exchange_n((uint16_t *)(void *)hva, &e,
            (uint16_t)val, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    } else {
        uint32_t e = cur;
        ok = __atomic_compare_exchange_n((uint32_t *)(void *)hva, &e, val,
            false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
consume:
    atomic_fetch_add(&g_ll[h].epoch, 1ULL);
    g_ll[h].owner = NULL;
    return ok;
}
