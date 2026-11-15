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

/* Soft-TLB arena: 32-entry PIC helper table immediately below tlb[0]
 * so [x25, #-8*(i+1)] stays reserved for PIC helpers. tlb[0] sits at
 * &g_tlb_arena.tlb[0]; 16 B stride per entry. */
typedef struct {
    uint64_t helpers[32];
    a32_vm32_tlb_entry_t tlb[A32_VM32_TLB_SIZE];
} a32_vm32_tlb_arena_t;

static a32_vm32_tlb_arena_t g_tlb_arena;
static bool g_tlb_ever_used = false;

/* Per-4 KiB guest permission ground truth (bit0=R, bit1=W, bit2=X),
 * maintained by the sync hooks alongside the TLB. The A2 union mprotect
 * reads neighbour permissions from here. */
static uint8_t g_prot[A32_VM32_TLB_SIZE];

/* Soft-TLB base for the A2 emitter/trampoline x25 (see vm32.h). */
uintptr_t a32_vm32_tlb_base = 0U;

typedef struct {
    _Atomic uint64_t epoch;
    _Atomic(const a32_ctx_t *) owner;
    _Atomic uint32_t addr;
    _Atomic uint32_t size;
    _Atomic uint64_t val;
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
    if (g_regime == A32_VM32_REGIME_A2_SOFT_TLB) {
        if (!g_tlb_ever_used) {
            g_tlb_arena.tlb[0].read_ntag =
                A32_VM32_TLB_PAGE0_INVALID;
            g_tlb_arena.tlb[0].write_ntag =
                A32_VM32_TLB_PAGE0_INVALID;
            g_tlb_ever_used = true;
        } else {
            a32_vm32_tlb_invalidate_all();
        }
    }
    memset(g_prot, 0, sizeof(g_prot));
    memset(g_ll, 0, sizeof(g_ll));
    a32_vm32_tlb_base = (g_regime == A32_VM32_REGIME_A2_SOFT_TLB)
        ? (uintptr_t)&g_tlb_arena.tlb[0] : 0U;
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
    return __atomic_load_n(&g_arena_base, __ATOMIC_ACQUIRE);
}

/* Rebase the arena (RC-U3-r4 #2, A32-R20-D3): negated tags are
 * G-independent (0ULL - (vpn << 12)), so rebasing only publishes the new
 * base; warm entries stay canonical with no per-entry fixup. A no-op
 * when base is unchanged. */
void a32_vm32_set_arena_base(uintptr_t base)
{
    uintptr_t old = __atomic_load_n(&g_arena_base, __ATOMIC_ACQUIRE);
    if (base == old) {
        return;
    }
    __atomic_store_n(&g_arena_base, base, __ATOMIC_RELEASE);
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

/* Bump every reservation-bucket epoch spanned by [start_gva, start_gva+len):
 * unmap/remap/protect/EOF-cut recycling must fail a stale STREX (A32-R9-7).
 * A span covering the full 64 KiB hash stride bumps all buckets once.
 * Epoch-only and lock-free: a mismatched snapshot already fails STREX. */
static void vm32_bump_spanned(uint32_t start_gva, uint64_t len)
{
    uint64_t s;
    uint64_t e;
    if (len == 0ULL) {
        return;
    }
    s = (uint64_t)(start_gva & ~63U);
    e = (uint64_t)start_gva + len;
    if (e > 0x100000000ULL) {
        e = 0x100000000ULL;
    }
    if (e <= s) {
        return;
    }
    if (e - s >= ((uint64_t)A32_VM32_LL_LINES << A32_VM32_LL_LINE_SHIFT)) {
        for (uint32_t h = 0U; h < A32_VM32_LL_LINES; h++) {
            atomic_fetch_add(&g_ll[h].epoch, 1ULL);
        }
        return;
    }
    for (uint64_t line = s; line < e; line += 64ULL) {
        atomic_fetch_add(&g_ll[ll_index((uint32_t)line)].epoch, 1ULL);
    }
}

static void vm32_tlb_fill_no_bump(uint32_t gva_page,
    uintptr_t host_page_base, bool readable, bool writable)
{
    uint32_t idx;
    uint8_t pbits;
    bool cut;
    bool r;
    bool w;
    (void)host_page_base;
    idx = tlb_index(gva_page);
    pbits = __atomic_load_n(&g_prot[idx], __ATOMIC_RELAXED);
    cut = (pbits & A32_PROT_EOF_CUT) != 0U;
    r = readable;
    w = writable;
    if (g_regime == A32_VM32_REGIME_A2_SOFT_TLB && w && !r) {
        r = true;
    }
    __atomic_store_n(&g_tlb_arena.tlb[idx].read_ntag,
        (!cut && r) ? vm32_valid_ntag(idx) : vm32_invalid_ntag(idx),
        __ATOMIC_RELEASE);
    __atomic_store_n(&g_tlb_arena.tlb[idx].write_ntag,
        (!cut && w) ? vm32_valid_ntag(idx) : vm32_invalid_ntag(idx),
        __ATOMIC_RELEASE);
}

void a32_vm32_tlb_fill(uint32_t gva_page, uintptr_t host_page_base,
    bool readable, bool writable)
{
    vm32_tlb_fill_no_bump(gva_page, host_page_base, readable, writable);
    /* A remap breaks every LDREX reservation on this page (A32-R9-7). */
    vm32_bump_spanned(gva_page & ~0xFFFU, 0x1000ULL);
}

void a32_vm32_tlb_invalidate_range(uint32_t start_gva, uint32_t end_gva)
{
    uint64_t start = (uint64_t)(start_gva & ~0xFFFU);
    uint64_t end = ((uint64_t)end_gva + 0xFFFULL) & ~0xFFFULL;
    uint64_t pages = (end > start) ? ((end - start) >> 12U) : 0ULL;
    uint64_t i;
    if (pages >= A32_VM32_TLB_SIZE) {
        a32_vm32_tlb_invalidate_all();
        return;
    }
    for (i = 0ULL; i < pages; i++) {
        uint32_t idx = tlb_index((uint32_t)(start + (i << 12U)));
        __atomic_store_n(&g_tlb_arena.tlb[idx].read_ntag,
            vm32_invalid_ntag(idx), __ATOMIC_RELEASE);
        __atomic_store_n(&g_tlb_arena.tlb[idx].write_ntag,
            vm32_invalid_ntag(idx), __ATOMIC_RELEASE);
    }
}

void a32_vm32_tlb_invalidate_all(void)
{
    uint32_t i;
    for (i = 0U; i < A32_VM32_TLB_SIZE; i++) {
        __atomic_store_n(&g_tlb_arena.tlb[i].read_ntag,
            vm32_invalid_ntag(i), __ATOMIC_RELEASE);
        __atomic_store_n(&g_tlb_arena.tlb[i].write_ntag,
            vm32_invalid_ntag(i), __ATOMIC_RELEASE);
    }
}

static int vm32_sync_host_pages_a2(uint32_t start_gva, uint32_t end_gva)
{
    size_t host_page;
    uint32_t hp_start;
    uint32_t hp_end;
    uintptr_t base = __atomic_load_n(&g_arena_base, __ATOMIC_ACQUIRE);
    bool have_run = false;
    uintptr_t run_start = 0U;
    size_t run_len = 0U;
    int run_host_prot = 0;
    if (g_regime != A32_VM32_REGIME_A2_SOFT_TLB || base == 0U ||
        end_gva <= start_gva) {
        return 0;
    }
    host_page = a32_backend_page_size();
    if (host_page != 16384U && host_page != 65536U) {
        host_page = 16384U;
    }
    hp_start = (uint32_t)((uint64_t)start_gva & ~(uint64_t)(host_page - 1U));
    hp_end = (uint32_t)(((uint64_t)end_gva + (uint64_t)(host_page - 1U)) &
        ~(uint64_t)(host_page - 1U));
    for (uint32_t hp = hp_start; hp < hp_end; hp += (uint32_t)host_page) {
        uint32_t union_prot = 0U;
        uint32_t hp_page_end = hp + (uint32_t)host_page;
        int host_prot;
        if (hp_page_end < hp) {
            hp_page_end = 0xFFFFFFFFU;
        }
        for (uint32_t sp = hp; sp < hp_page_end; sp += 0x1000U) {
            union_prot |= (uint32_t)(__atomic_load_n(&g_prot[sp >> 12U],
                __ATOMIC_RELAXED) & 0x7U);
            if (sp + 0x1000U < sp) {
                break;
            }
        }
        host_prot = a32_host_prot(NULL, union_prot);
        if (!have_run) {
            run_start = base + (uintptr_t)hp;
            run_len = host_page;
            run_host_prot = host_prot;
            have_run = true;
        } else if (host_prot == run_host_prot &&
            run_start + run_len == base + (uintptr_t)hp) {
            run_len += host_page;
        } else {
            int run_rc = a32_backend_protect((void *)run_start, run_len,
                run_host_prot);
            if (run_rc != 0) {
                return run_rc;
            }
            run_start = base + (uintptr_t)hp;
            run_len = host_page;
            run_host_prot = host_prot;
        }
        if (hp + (uint32_t)host_page < hp) {
            break;
        }
    }
    if (have_run) {
        int run_rc = a32_backend_protect((void *)run_start, run_len,
            run_host_prot);
        if (run_rc != 0) {
            return run_rc;
        }
    }
    return 0;
}

/* Publish translations for [start_gva, end_gva) after a successful
 * mmap/mprotect (host base == arena base + page in both regimes). */
int a32_vm32_sync_region(uint32_t start_gva, uint32_t end_gva,
    bool readable, bool writable, bool executable)
{
    uint32_t page;
    uint8_t bits;
    bool eff_readable = readable;
    bool eff_writable = writable;
    uintptr_t base = __atomic_load_n(&g_arena_base, __ATOMIC_ACQUIRE);
    if (base == 0U) {
        return 0;
    }
    if (g_regime == A32_VM32_REGIME_A2_SOFT_TLB && eff_writable &&
        !eff_readable) {
        eff_readable = true;
    }
    bits = (uint8_t)(((eff_readable ? 1U : 0U) | (eff_writable ? 2U : 0U)) |
        (executable ? 4U : 0U));
    for (page = start_gva & ~0xFFFU; page < end_gva; page += 0x1000U) {
        uint8_t old_b = __atomic_load_n(&g_prot[page >> 12U], __ATOMIC_RELAXED);
        __atomic_store_n(&g_prot[page >> 12U],
            (uint8_t)((old_b & A32_PROT_EOF_CUT) | bits), __ATOMIC_RELEASE);
        vm32_tlb_fill_no_bump(page, base + (uintptr_t)page, eff_readable,
            eff_writable);
        if (page + 0x1000U < page) {
            break;
        }
    }
    if (end_gva > start_gva) {
        vm32_bump_spanned(start_gva, (uint64_t)end_gva - (uint64_t)start_gva);
    }
    return vm32_sync_host_pages_a2(start_gva & ~0xFFFU, (end_gva + 0xFFFU) & ~0xFFFU);
}

/* Drop translations for [start_gva, end_gva) after munmap/mremap. */
int a32_vm32_sync_unmap(uint32_t start_gva, uint32_t end_gva)
{
    uint32_t start = start_gva & ~0xFFFU;
    uint32_t end = (end_gva + 0xFFFU) & ~0xFFFU;
    for (uint32_t p = start; p < end; p += 0x1000U) {
        uint32_t idx = tlb_index(p);
        uint64_t valid = vm32_valid_ntag(idx);
        if (__atomic_load_n(&g_tlb_arena.tlb[idx].read_ntag,
                __ATOMIC_RELAXED) == valid ||
            __atomic_load_n(&g_tlb_arena.tlb[idx].write_ntag,
                __ATOMIC_RELAXED) == valid) {
            __atomic_store_n(&g_tlb_arena.tlb[idx].read_ntag,
                vm32_invalid_ntag(idx), __ATOMIC_RELEASE);
            __atomic_store_n(&g_tlb_arena.tlb[idx].write_ntag,
                vm32_invalid_ntag(idx), __ATOMIC_RELEASE);
        }
        __atomic_store_n(&g_prot[p >> 12U], 0U, __ATOMIC_RELEASE);
        if (p + 0x1000U < p) {
            break;
        }
    }
    /* Dropping translations recycles the range: fail stale STREX (A32-R9-7). */
    vm32_bump_spanned(start, (uint64_t)end - (uint64_t)start);
    return vm32_sync_host_pages_a2(start, end);
}

bool a32_vm32_range_prot_ok(uint32_t va, uint32_t len, uint32_t req_prot)
{
    if (len == 0U) {
        return true;
    }
    if (__atomic_load_n(&g_arena_base, __ATOMIC_ACQUIRE) == 0U) {
        return false;
    }
    uint64_t end = (uint64_t)va + (uint64_t)len;
    if (end > 0x100000000ULL) {
        return false;
    }
    uint8_t req = (uint8_t)(req_prot & 0x7U);
    for (uint64_t p = (uint64_t)(va & ~0xFFFU); p < end; p += 0x1000ULL) {
        uint8_t pr = __atomic_load_n(&g_prot[p >> 12U], __ATOMIC_ACQUIRE);
        if ((pr & A32_PROT_EOF_CUT) != 0U || (pr & req) != req) {
            return false;
        }
    }
    return true;
}

bool a32_vm32_page_eof_cut(uint32_t gva)
{
    if (__atomic_load_n(&g_arena_base, __ATOMIC_ACQUIRE) == 0U) {
        return false;
    }
    return (__atomic_load_n(&g_prot[gva >> 12U],
        __ATOMIC_ACQUIRE) & A32_PROT_EOF_CUT) != 0U;
}

int a32_vm32_set_eof_cut(uint32_t gva, uint32_t len, bool cut)
{
    uintptr_t base = __atomic_load_n(&g_arena_base, __ATOMIC_ACQUIRE);
    if (base == 0U || len == 0U) {
        return 0;
    }
    uint64_t s = (uint64_t)(gva & ~0xFFFU);
    uint64_t e = ((uint64_t)gva + (uint64_t)len + 0xFFFULL) & ~0xFFFULL;
    if (e > 0x100000000ULL) {
        e = 0x100000000ULL;
    }
    /* EOF-cut transitions recycle pages: fail stale STREX (A32-R9-7). */
    vm32_bump_spanned((uint32_t)s, e - s);
    for (uint64_t p = s; p < e; p += 0x1000ULL) {
        uint32_t page = (uint32_t)p;
        uint32_t idx = page >> 12U;
        uint8_t old_b = __atomic_load_n(&g_prot[idx], __ATOMIC_RELAXED);
        uint8_t eff = (uint8_t)(old_b & 0x7U);
        uint8_t new_b = cut ? (uint8_t)(eff | A32_PROT_EOF_CUT) : eff;
        __atomic_store_n(&g_prot[idx], new_b, __ATOMIC_RELEASE);
        if (g_regime == A32_VM32_REGIME_A1_DIRECT) {
            int want = cut ? 0 : a32_host_prot(NULL, eff);
            int rc = a32_backend_protect((void *)(base + (uintptr_t)page),
                0x1000U, want);
            if (rc != 0) {
                /* A32-R13-7: restore this page's metadata (the TLB was
                 * not touched yet) so no page keeps cut metadata with
                 * stale host permissions. */
                __atomic_store_n(&g_prot[idx], old_b, __ATOMIC_RELEASE);
                return rc;
            }
        }
        if (cut) {
            __atomic_store_n(&g_tlb_arena.tlb[idx].read_ntag,
                vm32_invalid_ntag(idx), __ATOMIC_RELEASE);
            __atomic_store_n(&g_tlb_arena.tlb[idx].write_ntag,
                vm32_invalid_ntag(idx), __ATOMIC_RELEASE);
        } else {
            vm32_tlb_fill_no_bump(page, base + (uintptr_t)page,
                (eff & 1U) != 0U, (eff & 2U) != 0U);
        }
    }
    return 0;
}

/* Regime-aware mprotect (see vm32.h). A1 forwards guest-sized ranges
 * directly; A2 (>4 KiB host pages) computes the union of its 4 KiB
 * sub-pages and passes it to a32_backend_protect, which on Darwin keeps
 * any active host page mapped PROT_READ | PROT_WRITE (never PROT_EXEC)
 * while the Soft-TLB and g_prot[] enforce 4 KiB R/W/X in software. */
int a32_vm32_protect(a32_ctx_t *ctx, uint32_t addr, size_t len,
    uint32_t guest_prot)
{
    size_t host_page;
    uint32_t hp_start;
    uint32_t hp_end;
    bool have_run = false;
    uintptr_t run_start = 0U;
    size_t run_len = 0U;
    int run_host_prot = 0;
    if (ctx == NULL || len == 0U) {
        return -EINVAL;
    }
    if (g_regime != A32_VM32_REGIME_A2_SOFT_TLB) {
        return a32_backend_protect(a32_vm32_hva(ctx, addr), len,
            a32_host_prot(ctx, guest_prot));
    }
    vm32_bump_spanned(addr, (uint64_t)len);
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
        int host_prot;
        if (hp_page_end < hp) {
            hp_page_end = 0xFFFFFFFFU;
        }
        for (uint32_t sp = hp; sp < hp_page_end; sp += 0x1000U) {
            if (sp >= addr && (uint64_t)sp - (uint64_t)addr < (uint64_t)len) {
                union_prot |= guest_prot & 0x7U;
            } else {
                union_prot |= (uint32_t)(__atomic_load_n(&g_prot[sp >> 12U],
                    __ATOMIC_RELAXED) & 0x7U);
            }
            if (sp + 0x1000U < sp) {
                break;
            }
        }
        host_prot = a32_host_prot(ctx, union_prot);
        if (!have_run) {
            run_start = (uintptr_t)ctx->guest_base + (uintptr_t)hp;
            run_len = host_page;
            run_host_prot = host_prot;
            have_run = true;
        } else if (host_prot == run_host_prot &&
            run_start + run_len ==
                (uintptr_t)ctx->guest_base + (uintptr_t)hp) {
            run_len += host_page;
        } else {
            int run_rc = a32_backend_protect((void *)run_start, run_len,
                run_host_prot);
            if (run_rc != 0) {
                return run_rc;
            }
            run_start = (uintptr_t)ctx->guest_base + (uintptr_t)hp;
            run_len = host_page;
            run_host_prot = host_prot;
        }
        if (hp + (uint32_t)host_page < hp) {
            break;
        }
    }
    if (have_run) {
        int run_rc = a32_backend_protect((void *)run_start, run_len,
            run_host_prot);
        if (run_rc != 0) {
            return run_rc;
        }
    }
    return 0;
}

/* ---- Shared LDREX/STREX reservation table ---- */
static atomic_flag g_res_lock = ATOMIC_FLAG_INIT;
static void res_lock(void)
{
    while (atomic_flag_test_and_set_explicit(&g_res_lock,
        memory_order_acquire)) {
    }
}
static void res_unlock(void)
{
    atomic_flag_clear_explicit(&g_res_lock, memory_order_release);
}

uint64_t a32_vm32_ll_epoch(uint32_t gva)
{
    return atomic_load(&g_ll[ll_index(gva & ~3U)].epoch);
}

void a32_vm32_ll_record(a32_ctx_t *ctx, uint32_t gva, unsigned size,
    uint32_t val)
{
    uint32_t h = ll_index(gva & ~3U);
    res_lock();
    atomic_store(&g_ll[h].owner, ctx);
    atomic_store(&g_ll[h].addr, gva);
    atomic_store(&g_ll[h].size, (uint32_t)size);
    atomic_store(&g_ll[h].val, (uint64_t)val);
    if (ctx != NULL) {
        ctx->ll_epoch_snap = atomic_load(&g_ll[h].epoch);
    }
    res_unlock();
}

void a32_vm32_ll_record64(a32_ctx_t *ctx, uint32_t gva, uint64_t val)
{
    uint32_t h = ll_index(gva & ~3U);
    res_lock();
    atomic_store(&g_ll[h].owner, ctx);
    atomic_store(&g_ll[h].addr, gva);
    atomic_store(&g_ll[h].size, 8U);
    atomic_store(&g_ll[h].val, val);
    if (ctx != NULL) {
        ctx->ll_epoch_snap = atomic_load(&g_ll[h].epoch);
    }
    res_unlock();
}

void a32_vm32_copyout_invalidate(uint32_t gva, size_t len)
{
    uint64_t end;
    uint64_t line;
    bool locked;
    if (len == 0U) {
        return;
    }
    end = (uint64_t)gva + (uint64_t)len;
    if (end > 0x100000000ULL) {
        end = 0x100000000ULL;
    }
    /* A32-R5-6: signal-handler reentrancy — never spin when this thread */
    /* already holds g_res_lock; the table entries are _Atomic anyway. */
    locked = !atomic_flag_test_and_set_explicit(&g_res_lock,
        memory_order_acquire);
    /* A32-R13-9: the 1024-entry table repeats every 1024 lines, so a span
     * covering the full 64 KiB hash stride bumps each bucket once instead
     * of rescanning the same buckets up to 67M times. */
    if (((end - (uint64_t)(gva & ~63U)) + 63ULL) >> 6U >= A32_VM32_LL_LINES) {
        for (uint32_t h = 0U; h < A32_VM32_LL_LINES; h++) {
            atomic_fetch_add(&g_ll[h].epoch, 1ULL);
            atomic_store(&g_ll[h].owner, NULL);
        }
    } else {
        for (line = (uint64_t)(gva & ~63U); line < end; line += 64U) {
            uint32_t h = ll_index((uint32_t)line);
            atomic_fetch_add(&g_ll[h].epoch, 1ULL);
            atomic_store(&g_ll[h].owner, NULL);
        }
    }
    if (locked) {
        res_unlock();
    }
}

/* Store-path invalidation with SMC flush (A32-R9-6): reservation bump
 * plus a JIT flush when any written page is guest-EXEC. */
void a32_vm32_copyout_invalidate_exec(a32_ctx_t *ctx, uint32_t gva, size_t len)
{
    uint64_t end;
    uint32_t inv_end;
    a32_vm32_copyout_invalidate(gva, len);
    if (ctx == NULL || len == 0U) {
        return;
    }
    end = (uint64_t)gva + (uint64_t)len;
    if (end > 0x100000000ULL) {
        end = 0x100000000ULL;
    }
    inv_end = (end >= 0x100000000ULL) ? 0xFFFFFFFFU : (uint32_t)end;
    for (uint64_t p = (uint64_t)(gva & ~0xFFFU); p < end; p += 0x1000ULL) {
        if (a32_vm32_range_prot_ok((uint32_t)p, 1U, (uint32_t)PROT_EXEC)) {
            (void)a32_jit_invalidate_range(ctx, gva, inv_end);
            return;
        }
    }
}

/* Fork/clone-child reset (A32-R9-10): drop reservations owned by the
 * vanished parent context so the child's first STREX starts clean. */
void a32_vm32_ll_clear_owner(const a32_ctx_t *ctx)
{
    for (uint32_t h = 0U; h < A32_VM32_LL_LINES; h++) {
        if (atomic_load(&g_ll[h].owner) == ctx) {
            atomic_fetch_add(&g_ll[h].epoch, 1ULL);
            atomic_store(&g_ll[h].owner, NULL);
        }
    }
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
    res_lock();
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
    res_unlock();
    return ok;
}

bool a32_vm32_sc_attempt64(a32_ctx_t *ctx, uint32_t gva, uint64_t val)
{
    uint32_t h = ll_index(gva & ~3U);
    uint64_t snap;
    uint64_t cur = 0ULL;
    void *hva;
    bool ok = false;
    if (ctx == NULL) {
        return false;
    }
    if ((gva & 7U) != 0U) {
        return false;
    }
    res_lock();
    if (atomic_load(&g_ll[h].owner) != ctx ||
        atomic_load(&g_ll[h].addr) != gva ||
        atomic_load(&g_ll[h].size) != 8U) {
        goto consume64;
    }
    snap = atomic_load(&g_ll[h].epoch);
    if (snap != ctx->ll_epoch_snap) {
        goto consume64;
    }
    hva = (void *)((uintptr_t)ctx->guest_base + (uintptr_t)gva);
    cur = __atomic_load_n((uint64_t *)(void *)hva, __ATOMIC_SEQ_CST);
    if (cur != atomic_load(&g_ll[h].val) ||
        snap != atomic_load(&g_ll[h].epoch)) {
        goto consume64;
    }
    {
        uint64_t e = cur;
        ok = __atomic_compare_exchange_n((uint64_t *)(void *)hva, &e, val,
            false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
consume64:
    atomic_fetch_add(&g_ll[h].epoch, 1ULL);
    atomic_store(&g_ll[h].owner, NULL);
    res_unlock();
    return ok;
}

void a32_vm32_reservation_bump(uint32_t gva_aligned)
{
    uint32_t h = ll_index(gva_aligned & ~3U);
    atomic_fetch_add(&g_ll[h].epoch, 1ULL);
    atomic_store(&g_ll[h].owner, NULL);
}

void a32_vm32_mark_code_range(uint32_t start_gva, uint32_t end_gva)
{
    (void)start_gva;
    (void)end_gva;
}

void a32_vm32_clear_code_watches(void)
{
}
