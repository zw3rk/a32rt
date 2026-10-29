/*
 * src/vm/vm32.h -- Two-regime Class-A 32-bit VM interface (Wave U3 A32-4)
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * UNIFICATION-PLAN.md Rev 4, sections 4.3-4.6 and 5.1: every Class-A
 * translator reserves the 4 GiB guest window [G, G + 4 GiB) in one of two
 * regimes selected by the dynamic host page size H (never hardcoded):
 *
 *   Regime A1 (H == 4096: native 4K Linux kernels and every hyper-linux
 *   run, where Stage-1 EL1 tables stay 4 KiB): the MAP_PRIVATE anonymous
 *   window owned by a32_vm_init with x20 = G; DBT emits 1-instruction
 *   [x20, wAddr, uxtw] accesses; guest fork() uses native MAP_PRIVATE COW.
 *
 *   Regime A2 (H > 4096: standalone 16 KiB macOS Mach-O backend_darwin.c
 *   and 16K/64K Linux without /dev/a32host): the same 4 GiB span stays a
 *   single MAP_PRIVATE arena whose active host pages carry the union of
 *   their 4 KiB sub-page protections (PROT_READ | PROT_WRITE, never
 *   PROT_EXEC); x20 = &tlb[0] and every DBT access goes through the
 *   7-instruction Soft-TLB fast path (section 4.3) with the 32-bit guest
 *   address zero-extended into w1 first (mov w1, wAddr).
 *
 * a32_vm32_init runs after a32_vm_init and records the regime plus the
 * arena base (== ctx->guest_base in both regimes). It never reserves a
 * second window: there is exactly one [G, G + 4 GiB) mapping per process.
 *
 * Curated C11 without VLAs. No inline asm and no platform-register use
 * anywhere in src/vm/.
 */

#ifndef A32RT_VM_VM32_H
#define A32RT_VM_VM32_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "a32rt.h"

/* Guest ABI page granule (a32) and window size. */
#define A32_VM32_GUEST_PAGE_SIZE 4096U
#define A32_VM32_GUEST_PAGE_MASK 4095U
#define A32_VM32_WINDOW_SIZE 0x100000000ULL

typedef enum {
    A32_VM32_REGIME_UNSET = 0,
    A32_VM32_REGIME_A1_DIRECT = 1, /* H == 4096: [x20, wAddr, uxtw] */
    A32_VM32_REGIME_A2_SOFT_TLB = 2 /* H > 4096: 7-insn Soft-TLB */
} a32_vm32_regime_t;

/* ---- Regime selection and lifetime (src/vm/vm32.c) ---- */

a32_vm32_regime_t a32_vm32_select_regime(size_t host_page_size);
a32_vm32_regime_t a32_vm32_current_regime(void);

/* Record the regime for the window already reserved by a32_vm_init and
 * publish the arena base. Returns 0 on success, negative errno on
 * failure (unknown host page size). Never reserves or remaps anything. */
int a32_vm32_init(a32_ctx_t *ctx);

/* Canonical host address of a guest VA. Fixed for the mapping lifetime:
 * in-place COW promotion (PAGE_COW_CLEAN -> PAGE_COW_DIRTY) never moves
 * HVA, so sibling CLONE_VM threads and futex keys stay coherent. Both
 * regimes share one coherent address: HVA = G + gva. */
void *a32_vm32_hva(const a32_ctx_t *ctx, uint32_t gva);
void a32_vm32_set_arena_base(uintptr_t base);

/* Arena base backing [G, G + 4 GiB) (== G in both regimes). */
uintptr_t a32_vm32_arena_base(void);

/* ---- 7-instruction Soft-TLB (UNIFICATION-PLAN section 4.3) ---- */

/* Full-coverage table: one entry per 4 KiB guest page in the 4 GiB window
 * (1M entries x 32 B = 32 MiB BSS, demand-zero). Direct-mapped aliasing is
 * unacceptable for the A2 fast path: a miss exits the block with a guest
 * fault, and an eviction-induced miss on a mapped page would deliver a
 * bogus fault with no mid-block resume machinery. Full coverage makes
 * every tag miss a genuine unmapped-or-wrong-permission fault. */
#define A32_VM32_TLB_BITS 20U
#define A32_VM32_TLB_SIZE (1U << A32_VM32_TLB_BITS) /* 1048576 entries */
#define A32_VM32_TLB_INVALID_TAG (~UINT64_C(0))

typedef struct {
    uint64_t read_tag; /* Offset 0: (gva & ~0xFFF) if readable, else ~0 */
    uintptr_t addend; /* Offset 8: host_page_base - (gva & ~0xFFF) */
    uint64_t write_tag; /* Offset 16: (gva & ~0xFFF) iff writable &&
                         * !cow_clean && !has_code && !has_reservation */
    uint64_t flags_reserved; /* Offset 24: zero, keeps 32 B alignment */
} a32_vm32_tlb_entry_t;

_Static_assert(sizeof(a32_vm32_tlb_entry_t) == 32,
    "Soft-TLB entry must be 32 bytes");

/* Fast-path probe: true on tag hit with N-byte alignment, addend out.
 * Instructions 1-6 of the emitted sequence have no PSTATE or x0/x1 side
 * effects, so a miss cleanly reaches the 2-insn per-site stub. */
bool a32_vm32_tlb_probe(uint32_t gva, bool is_store, uint32_t width,
    uintptr_t *addend_out);

/* Slow path shared by every .L_tlb_miss_site_k stub (ctx, x1 = gva,
 * x0 = value, block id, 16-bit site index intact). Returns 0 and fills
 * hva_out on success, negative guest-signal code on fault. */
int a32_vm32_tlb_miss(a32_ctx_t *ctx, uint32_t gva, bool is_store,
    void **hva_out);

/* Populate one 4 KiB translation after a mapping change. */
void a32_vm32_tlb_fill(uint32_t gva_page, uintptr_t host_page_base,
    bool readable, bool writable);

/* Range-selective invalidation (< 512 pages) else full clear. */
void a32_vm32_tlb_invalidate_range(uint32_t start_gva, uint32_t end_gva);
void a32_vm32_tlb_invalidate_all(void);

/* Mapping-layer hooks: publish translations for [start, end) after a
 * successful mmap/mprotect (host base == arena base + page in both
 * regimes), or drop them after munmap/mremap. Called by src/vm.c.
 * The per-4 KiB guest permission record (R/W/X ground truth for the A2
 * host-page union in a32_vm32_protect) is maintained alongside. */
void a32_vm32_sync_region(uint32_t start_gva, uint32_t end_gva,
    bool readable, bool writable, bool executable);
void a32_vm32_sync_unmap(uint32_t start_gva, uint32_t end_gva);

/* Soft-TLB base address for the A2 emitter/trampoline (x21). Set by
 * a32_vm32_init: &g_tlb[0] when regime == A2_SOFT_TLB, 0 (unused) in A1. */
extern uintptr_t a32_vm32_tlb_base;

/* Regime-aware mprotect. Outside A2 this forwards [addr, addr+len)
 * directly; under A2 it computes the union of every 4 KiB sub-page
 * permission (target sub-pages contribute guest_prot, neighbours their
 * recorded permissions) within each host page and protects each host
 * page once via a32_backend_protect. Returns 0, negative errno else. */
int a32_vm32_protect(a32_ctx_t *ctx, uint32_t addr, size_t len,
    uint32_t guest_prot);

/* ---- LDREX/STREX reservation + true atomic RMW (section 4.4) ----
 *
 * The single shared reservation table for the DBT slow path and the
 * reference interpreter. Every 64-byte line hashed into 1024 entries
 * tracks a SEQ_CST epoch plus the owner ctx, address, and value snapshot
 * taken at LDREX. Any store to a reserved line bumps the epoch, so a
 * stale STREX fails instead of silently winning.
 *
 * Both hosts are little-endian (aarch64 Linux and arm64 Darwin), matching
 * the LE a32 guest: no byte-swap on the atomic path.
 */

#define A32_VM32_LL_LINES 1024U
#define A32_VM32_LL_LINE_SHIFT 6U /* 64-byte lines */

/* Record an LDREX reservation: ctx ownership, address, size, and value
 * snapshot, plus the line epoch sampled with SEQ_CST. */
void a32_vm32_ll_record(a32_ctx_t *ctx, uint32_t gva, unsigned size,
    uint32_t val);

/* Attempt STREX: verifies the local monitor (addr/size), the shared
 * owner/addr/epoch/value, then performs the store with a SEQ_CST atomic
 * CAS on HVA. True on success (Rd = 0), false on reservation loss
 * (Rd = 1). Either way the caller's reservation is consumed. Unaligned
 * addresses always fail (caller raises UNDEF). */
bool a32_vm32_sc_attempt(a32_ctx_t *ctx, uint32_t gva, unsigned size,
    uint32_t val);

/* Bump a line epoch when a store lands on a reserved line through any
 * path (DBT slow path, interp, copyout, sigframe). */
void a32_vm32_reservation_bump(uint32_t gva_aligned);

/* ---- Relative DBT memory-site records (section 4.3.3) ---- */

typedef enum {
    A32_VM32_NPC_SEQUENTIAL = 0,
    A32_VM32_NPC_STATIC_TARGET = 1,
    A32_VM32_NPC_DYNAMIC_CTX = 2
} a32_vm32_npc_mode_t;

typedef struct {
    uint16_t seq_start_offset;
    uint16_t host_insn_offset;
    uint16_t cont_insn_offset;
    uint16_t guest_pc_delta;
    int32_t npc_delta;
    uint32_t insn_word;
    int16_t sp_rollback_delta;
    uint8_t npc_mode;
    uint8_t width;
    uint8_t flags;
    uint8_t val_or_dst_reg;
    uint8_t addr_base_reg;
    uint8_t reserved;
} a32_vm32_mem_site_t;

_Static_assert(sizeof(a32_vm32_mem_site_t) == 24,
    "a32_vm32_mem_site_t must be 24 bytes");

#define A32_VM32_SITE_STORE (1U << 0)
#define A32_VM32_SITE_SIGNED (1U << 1)
#define A32_VM32_SITE_PRE_COMMITTED_SP (1U << 2)

/* ---- Provider A file backing (section 5.1, src/vm/vm_file.c) ---- */

int a32_vmfile_map_staged(void *hint_addr, size_t len, int host_fd,
    uint64_t file_offset, void **out);
int a32_vmfile_map_commit(void *host_target, size_t len, int host_prot,
    int host_fd, uint64_t file_offset, int host_flags, bool a2_stage,
    void **out);
int a32_vmfile_populate_private(void *hva, int host_fd, uint64_t file_offset,
    size_t len);
int a32_vmfile_populate_if_private(void *hva, int host_fd,
    uint64_t file_offset, size_t len, int host_flags, int final_prot);
void a32_vmfile_promote_cow(uint32_t gva_page, bool readable, bool writable);
void a32_vmfile_promote_range(uint32_t start_gva, uint32_t end_gva,
    bool readable, bool writable);
int a32_vmfile_on_truncate(uint64_t new_size);
int a32_vmfile_hook_write(uint64_t end_offset);
/* Syscall-hook entry points: refresh the tracked size/sequence for the
 * file behind host_fd (ftruncate/write/pwrite) or host_path (truncate).
 * Entries for other files are untouched. Return 0 (or negative errno
 * when the file cannot be stated); unknown files are ignored. */
int a32_vmfile_sync_fd(int host_fd);
int a32_vmfile_sync_path(const char *host_path);
void a32_vmfile_record_shared(uint32_t start_gva, uint32_t end_gva, int fd,
    uint32_t pgoffset);
bool a32_vmfile_lookup_shared(uint32_t start_gva, int *out_fd,
    uint32_t *out_pgoffset);
void a32_vmfile_unmap_shared(uint32_t unmap_start, uint32_t unmap_end);

#endif /* A32RT_VM_VM32_H */
