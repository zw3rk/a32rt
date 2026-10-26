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
#include "os/backend.h"

#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static a32_vm32_tlb_entry_t g_tlb[A32_VM32_TLB_SIZE];

/* Per-4 KiB guest permission ground truth (bit0=R, bit1=W, bit2=X),
 * maintained by the sync hooks alongside the TLB. The A2 union mprotect
 * reads neighbour permissions from here. */
static uint8_t g_prot[A32_VM32_TLB_SIZE];

/* Soft-TLB base for the A2 emitter/trampoline x21 (see vm32.h). */
uintptr_t a32_vm32_tlb_base = 0U;

typedef struct {
    _Atomic uint64_t epoch;
    _Atomic(const a32_ctx_t *) owner;
    _Atomic uint32_t addr;
    _Atomic uint32_t size;
    _Atomic uint32_t val;
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
    memset(g_prot, 0, sizeof(g_prot));
    memset(g_ll, 0, sizeof(g_ll));
    a32_vm32_tlb_base = (g_regime == A32_VM32_REGIME_A2_SOFT_TLB)
        ? (uintptr_t)&g_tlb[0] : 0U;
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

/* Reservation-table slot: every 64-byte line hashed into 1024 entries.
 * Never tlb_index() (4096 entries): that overruns g_ll whenever G != 0. */
static inline uint32_t ll_index(uint32_t gva_aligned)
{
    return (gva_aligned >> A32_VM32_LL_LINE_SHIFT) & (A32_VM32_LL_LINES - 1U);
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
    atomic_fetch_add(&g_ll[ll_index(gva_page & ~0xFFFU)].epoch, 1ULL);
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
    bool readable, bool writable, bool executable)
{
    uint32_t page;
    uint8_t bits;
    if (g_arena_base == 0U) {
        return;
    }
    bits = (uint8_t)(((readable ? 1U : 0U) | (writable ? 2U : 0U)) |
        (executable ? 4U : 0U));
    for (page = start_gva & ~0xFFFU; page < end_gva; page += 0x1000U) {
        a32_vm32_tlb_fill(page, g_arena_base + (uintptr_t)page,
            readable, writable);
        g_prot[page >> 12U] = bits;
        if (page + 0x1000U < page) {
            break;
        }
    }
}

/* Drop translations for [start_gva, end_gva) after munmap/mremap. */
void a32_vm32_sync_unmap(uint32_t start_gva, uint32_t end_gva)
{
    uint32_t start = start_gva & ~0xFFFU;
    uint32_t end = (end_gva + 0xFFFU) & ~0xFFFU;
    a32_vm32_tlb_invalidate_range(start_gva, end_gva);
    for (; start < end; start += 0x1000U) {
        g_prot[start >> 12U] = 0U;
        if (start + 0x1000U < start) {
            break;
        }
    }
}

/* Regime-aware mprotect (see vm32.h). A1 forwards guest-sized ranges
 * directly; A2 (>4 KiB host pages) protects each covering host page with
 * the union of its 4 KiB sub-pages. Target sub-pages use guest_prot;
 * neighbours keep their recorded g_prot permissions so a 4 KiB guest
 * mprotect never silently widens or narrows its host page siblings. X is
 * unioned faithfully like R/W (A1 maps guest-X with host PROT_EXEC;
 * never invent or drop it here). */
int a32_vm32_protect(a32_ctx_t *ctx, uint32_t addr, size_t len,
    uint32_t guest_prot)
{
    size_t host_page;
    uint32_t hp_start;
    uint32_t hp_end;
    if (ctx == NULL || len == 0U) {
        return -EINVAL;
    }
    if (g_regime != A32_VM32_REGIME_A2_SOFT_TLB) {
        return a32_backend_protect(a32_vm32_hva(ctx, addr), len,
            a32_host_prot(ctx, guest_prot));
    }
    host_page = a32_backend_page_size();
    if (host_page != 16384U && host_page != 65536U) {
        host_page = 16384U;
    }
    hp_start = (uint32_t)((uint64_t)addr & ~(uint64_t)(host_page - 1U));
    hp_end = (uint32_t)(((uint64_t)addr + (uint64_t)len +
        (uint64_t)(host_page - 1U)) & ~(uint64_t)(host_page - 1U));
    for (uint32_t hp = hp_start; hp < hp_end; hp += (uint32_t)host_page) {
        uint32_t union_prot = 0U;
        uint32_t hp_page_end = hp + (uint32_t)host_page;
        if (hp_page_end < hp) {
            hp_page_end = 0xFFFFFFFFU;
        }
        for (uint32_t sp = hp; sp < hp_page_end; sp += 0x1000U) {
            if (sp >= addr && (uint64_t)sp - (uint64_t)addr < (uint64_t)len) {
                union_prot |= guest_prot & 0x7U;
            } else {
                union_prot |= g_prot[sp >> 12U];
            }
            if (sp + 0x1000U < sp) {
                break;
            }
        }
        void *hp_addr = (void *)((uintptr_t)ctx->guest_base + (uintptr_t)hp);
        int rc = a32_backend_protect(hp_addr, host_page,
            a32_host_prot(ctx, union_prot));
        if (rc != 0) {
            return rc;
        }
        if (hp + (uint32_t)host_page < hp) {
            break;
        }
    }
    return 0;
}

/* ---- Shared LDREX/STREX reservation table ---- */

void a32_vm32_ll_record(a32_ctx_t *ctx, uint32_t gva, unsigned size,
    uint32_t val)
{
    uint32_t h = ll_index(gva & ~3U);
    atomic_store(&g_ll[h].owner, ctx);
    atomic_store(&g_ll[h].addr, gva);
    atomic_store(&g_ll[h].size, (uint32_t)size);
    atomic_store(&g_ll[h].val, val);
    if (ctx != NULL) {
        ctx->ll_epoch_snap = atomic_load(&g_ll[h].epoch);
    }
}

void a32_vm32_reservation_bump(uint32_t gva_aligned)
{
    uint32_t h = ll_index(gva_aligned);
    atomic_fetch_add(&g_ll[h].epoch, 1ULL);
    atomic_store(&g_ll[h].owner, NULL);
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
    if (atomic_load(&g_ll[h].owner) != ctx ||
        atomic_load(&g_ll[h].addr) != gva ||
        atomic_load(&g_ll[h].size) != size) {
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
    if (cur != atomic_load(&g_ll[h].val) ||
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
    atomic_store(&g_ll[h].owner, NULL);
    return ok;
}
