/*
 * src/vm/vm_priv.h -- Internal VM operations shared by src/vm.c and
 * src/vm/vm_mremap.c (A32-R3-1 region/backing split, A32-R3-3 mremap).
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * All functions below assume the VM lock is held. Curated C11.
 */

#ifndef A32RT_VM_PRIV_H
#define A32RT_VM_PRIV_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "a32rt.h"

#define A32_MMAP_BASE_32        0x40000000U
#define A32_MMAP_TOP_32         0xDF000000U
#define A32_MAP_FIXED           0x10U
#define A32_MAP_ANONYMOUS       0x20U
#define A32_MAP_FIXED_NOREPLACE 0x100000U
#define A32_MREMAP_MAYMOVE      1U
#define A32_MREMAP_FIXED        2U

void a32_vm_lock_acquire(void);
void a32_vm_lock_release(void);
void a32_vm_sync_from_ctx(a32_ctx_t *ctx);
void a32_vm_sync_to_ctx(a32_ctx_t *ctx);

static inline uint64_t a32_vm_page_align_up(uint64_t v)
{
    return (v + 0xFFFULL) & ~0xFFFULL;
}

/* ---- Provider A file-backing tables (A32-R4 file split) ----
 * Storage lives in src/vm/vm_file.c; src/vm/vm_file_map.c shares it for
 * the lookup/coalesce/retire helpers moved there. */
#define A32_VMFILE_SLOTS 1024U

typedef struct {
    bool used;
    uint64_t dev;
    uint64_t ino;
    uint32_t start_gva;
    uint32_t end_gva;
    uint64_t file_offset;
    int dup_fd;
} a32_vmfile_map_t;

typedef struct {
    bool used;
    uint32_t start_gva;
    uint32_t end_gva;
    int dup_fd;
    uint32_t pgoffset;
} a32_vmfile_shared_t;

extern a32_vmfile_map_t g_fmaps[A32_VMFILE_SLOTS];
extern a32_vmfile_shared_t g_shared[A32_VMFILE_SLOTS];
extern pthread_mutex_t g_vmfile_lock;

bool a32_vm_range_overlaps(uint32_t start, uint32_t end);
bool a32_vm_mapped_range(uint32_t start, uint32_t end, uint32_t *out_prot,
    uint32_t *out_flags, char *out_name, size_t name_sz);
uint32_t a32_vm_mmap_locked(a32_ctx_t *ctx, uint32_t addr, uint32_t len,
    uint32_t prot, uint32_t flags, int fd, uint32_t pgoffset,
    const char *name);
int a32_vm_unmap_locked(a32_ctx_t *ctx, uint32_t addr, uint32_t len);
int a32_vm_protect_locked(a32_ctx_t *ctx, uint32_t addr, uint32_t len,
    uint32_t prot);
int a32_vm_record_region_locked(uint32_t start, uint32_t end, uint32_t prot,
    uint32_t flags, const char *name);
void a32_vm_invalidate_jit(a32_ctx_t *ctx, uint32_t start, uint32_t end);
/* Cursor follow-up after an in-place mremap extension. */
void a32_vm_note_grown(uint32_t old_addr, uint32_t ext_end);

#endif /* A32RT_VM_PRIV_H */
