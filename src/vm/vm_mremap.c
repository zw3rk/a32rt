/*
 * src/vm/vm_mremap.c -- Guest mremap with file-backing preservation
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Split out of src/vm.c (A32-R3): shared mappings keep their descriptor
 * and offset across growth and relocation, and private file mappings
 * carry their backing identity too (A32-R3-3) — growth populates newly
 * exposed file pages while existing private modifications stay in place,
 * and relocation re-maps from the same file offset before copying the
 * old (possibly modified) bytes over. EOF cuts travel with the mapping:
 * the copy step skips pages the source already retired beyond EOF.
 * Curated C11.
 */

#define _GNU_SOURCE
#include "a32rt.h"
#include "os/backend.h"
#include "vm/vm32.h"
#include "vm/vm_priv.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* Private backing carried across the operation: owned duplicate
 * descriptor, file offset of old_addr, and the tracked entry bounds. */
typedef struct {
    bool     present;
    int      fd;
    uint32_t pgoff_at_old;
} mremap_priv_t;

static void mremap_priv_release(mremap_priv_t *p)
{
    if (p->present && p->fd >= 0) {
        close(p->fd);
    }
    p->present = false;
    p->fd = -1;
}

static mremap_priv_t mremap_priv_lookup(uint32_t old_addr, uint32_t old_end,
    bool is_sh_file, uint32_t keep_flags)
{
    mremap_priv_t p;
    uint32_t map_start = 0U;
    uint32_t map_end = 0U;
    uint64_t file_offset = 0ULL;
    int dup_fd = -1;
    p.present = false;
    p.fd = -1;
    p.pgoff_at_old = 0U;
    if (is_sh_file || (keep_flags & A32_MAP_ANONYMOUS) != 0U) {
        return p;
    }
    if (!a32_vmfile_lookup_private(old_addr, NULL, NULL, &file_offset,
            &dup_fd, &map_start, &map_end)) {
        return p;
    }
    if (map_start > old_addr || map_end < old_end || dup_fd < 0) {
        if (dup_fd >= 0) {
            close(dup_fd);
        }
        return p;
    }
    p.present = true;
    p.fd = dup_fd;
    p.pgoff_at_old =
        (uint32_t)((file_offset + (uint64_t)(old_addr - map_start)) / 4096ULL);
    return p;
}

/* Copy the surviving prefix after a move, skipping source pages the EOF
 * cut already retired: those read back as zeros, which the fresh file
 * population of the destination already provides. */
static void mremap_copy_prefix(a32_ctx_t *ctx, uint32_t new_va,
    uint32_t old_addr, uint32_t copy_len)
{
    uint32_t off = 0U;
    while (off < copy_len) {
        uint32_t chunk = copy_len - off;
        if (chunk > 0x1000U) {
            chunk = 0x1000U;
        }
        if (a32_vm32_range_prot_ok(old_addr + off, chunk,
                (uint32_t)PROT_READ)) {
            memcpy(a32_guest_ptr(ctx, new_va + off),
                a32_guest_ptr(ctx, old_addr + off), chunk);
        }
        off += chunk;
    }
}

uint32_t a32_vm_mremap(a32_ctx_t *ctx, uint32_t old_addr, uint32_t old_len,
                       uint32_t new_len, uint32_t flags, uint32_t new_addr)
{
    if (!ctx || (old_addr & (A32_PAGE_SIZE - 1U)) != 0U ||
        old_len == 0U || new_len == 0U) {
        return (uint32_t)(-EINVAL);
    }
    if ((flags & ~(A32_MREMAP_MAYMOVE | A32_MREMAP_FIXED)) != 0U ||
        ((flags & A32_MREMAP_FIXED) != 0U && (flags & A32_MREMAP_MAYMOVE) == 0U)) {
        return (uint32_t)(-EINVAL);
    }

    uint64_t old_aligned = a32_vm_page_align_up((uint64_t)old_len);
    uint64_t new_aligned = a32_vm_page_align_up((uint64_t)new_len);
    if (old_addr < A32_LOW_GUARD_SIZE ||
        (uint64_t)old_addr + old_aligned > (uint64_t)A32_KUSER_BASE_32) {
        return (uint32_t)(-EINVAL);
    }
    if (new_aligned == 0ULL || new_aligned > (uint64_t)A32_KUSER_BASE_32) {
        return (uint32_t)(-ENOMEM);
    }

    a32_vm_lock_acquire();
    a32_vm_sync_from_ctx(ctx);

    uint32_t prot       = 0x3U;
    uint32_t orig_flags = 0x22U;
    char     orig_name[64];
    orig_name[0] = '\0';
    if (!a32_vm_mapped_range(old_addr, (uint32_t)((uint64_t)old_addr + old_aligned),
                                      &prot, &orig_flags, orig_name, sizeof(orig_name))) {
        a32_vm_sync_to_ctx(ctx);
        a32_vm_lock_release();
        return (uint32_t)(-EFAULT);
    }
    uint32_t    keep_flags  = orig_flags & 0x23U;
    uint32_t    sh_flags    = keep_flags & ~A32_MAP_ANONYMOUS;
    uint32_t    map_flags   = (((keep_flags & 0x01U) != 0U) ? 0x01U : 0x02U) | A32_MAP_ANONYMOUS;
    int         sh_fd       = -1;
    uint32_t    sh_pgoff    = 0U;
    bool        is_sh_file  = ((keep_flags & 0x01U) != 0U) &&
                              a32_vmfile_lookup_shared(old_addr, &sh_fd, &sh_pgoff);
    const char *region_name = (orig_name[0] != '\0') ? orig_name : "[mmap]";
    mremap_priv_t priv = mremap_priv_lookup(old_addr,
        (uint32_t)((uint64_t)old_addr + old_aligned), is_sh_file, keep_flags);
    if (priv.present) {
        map_flags = 0x02U;
    }
    if (is_sh_file && (keep_flags & A32_MAP_ANONYMOUS) != 0U && new_aligned > old_aligned) {
        (void)a32_vmfile_grow_shared(sh_fd, sh_pgoff, new_aligned);
    }

    if ((flags & A32_MREMAP_FIXED) == 0U) {
        if (new_aligned == old_aligned) {
            mremap_priv_release(&priv);
            a32_vm_sync_to_ctx(ctx);
            a32_vm_lock_release();
            return old_addr;
        }
        if (new_aligned < old_aligned) {
            uint32_t unmap_addr = old_addr + (uint32_t)new_aligned;
            uint32_t unmap_len  = (uint32_t)(old_aligned - new_aligned);
            int rc = a32_vm_unmap_locked(ctx, unmap_addr, unmap_len);
            mremap_priv_release(&priv);
            a32_vm_sync_to_ctx(ctx);
            a32_vm_lock_release();
            if (rc != 0) {
                return (uint32_t)rc;
            }
            a32_vm_invalidate_jit(ctx, unmap_addr, unmap_addr + unmap_len);
            return old_addr;
        }

        uint32_t old_end = (uint32_t)((uint64_t)old_addr + old_aligned);
        uint64_t new_end = (uint64_t)old_addr + new_aligned;
        if (new_end <= (uint64_t)A32_KUSER_BASE_32 &&
            !a32_vm_range_overlaps(old_end, (uint32_t)new_end)) {
            uint32_t ext_flags = (is_sh_file ? sh_flags :
                (priv.present ? 0x02U : map_flags)) | A32_MAP_FIXED;
            uint32_t ext_pgoff = is_sh_file ? (sh_pgoff + (uint32_t)(old_aligned / A32_PAGE_SIZE)) :
                (priv.present ? (priv.pgoff_at_old + (uint32_t)(old_aligned / A32_PAGE_SIZE)) : 0U);
            int ext_fd = is_sh_file ? sh_fd : (priv.present ? priv.fd : -1);
            uint32_t ext = a32_vm_mmap_locked(ctx, old_end, (uint32_t)(new_aligned - old_aligned),
                                           prot, ext_flags, ext_fd, ext_pgoff,
                                           region_name);
            if (ext <= 0xFFFFF000U) {
                if (is_sh_file) {
                    /* Merge the extension record back into one entry;
                     * a failure only leaves the range split in two
                     * tracked entries, never untracked. */
                    (void)a32_vmfile_record_shared(old_addr,
                        (uint32_t)new_end, sh_fd, sh_pgoff);
                }
                uint32_t ext_end = (uint32_t)new_end;
                a32_vm_note_grown(old_addr, ext_end);
                mremap_priv_release(&priv);
                a32_vm_sync_to_ctx(ctx);
                a32_vm_lock_release();
                if ((prot & (uint32_t)PROT_EXEC) != 0U) {
                    a32_vm_invalidate_jit(ctx, old_end, ext_end);
                }
                return old_addr;
            }
        }

        if ((flags & A32_MREMAP_MAYMOVE) == 0U) {
            mremap_priv_release(&priv);
            a32_vm_sync_to_ctx(ctx);
            a32_vm_lock_release();
            return (uint32_t)(-ENOMEM);
        }
    } else {
        if ((new_addr & (A32_PAGE_SIZE - 1U)) != 0U || new_addr < A32_LOW_GUARD_SIZE ||
            (uint64_t)new_addr + new_aligned > (uint64_t)A32_KUSER_BASE_32 ||
            ((uint64_t)new_addr < (uint64_t)old_addr + old_aligned &&
             (uint64_t)old_addr < (uint64_t)new_addr + new_aligned)) {
            mremap_priv_release(&priv);
            a32_vm_sync_to_ctx(ctx);
            a32_vm_lock_release();
            return (uint32_t)(-EINVAL);
        }
    }

    bool     is_fixed = (flags & A32_MREMAP_FIXED) != 0U;
    uint32_t rw_prot  = is_sh_file ? prot : ((prot & ~((uint32_t)PROT_EXEC)) | (uint32_t)(PROT_READ | PROT_WRITE));
    uint32_t mv_flags = (is_sh_file ? sh_flags :
        (priv.present ? 0x02U : map_flags)) | (is_fixed ? A32_MAP_FIXED : 0U);
    int mv_fd = is_sh_file ? sh_fd : (priv.present ? priv.fd : -1);
    uint32_t mv_pgoff = is_sh_file ? sh_pgoff : (priv.present ? priv.pgoff_at_old : 0U);
    uint32_t new_va   = a32_vm_mmap_locked(ctx, is_fixed ? new_addr : 0U, (uint32_t)new_aligned,
                                        rw_prot, mv_flags, mv_fd, mv_pgoff,
                                        region_name);
    if (new_va > 0xFFFFF000U) {
        mremap_priv_release(&priv);
        a32_vm_sync_to_ctx(ctx);
        a32_vm_lock_release();
        return new_va;
    }

    if (!is_sh_file) {
        if ((prot & 0x1U) == 0U) {
            (void)a32_backend_protect(a32_guest_ptr(ctx, old_addr), (size_t)old_aligned, PROT_READ);
        }
        uint32_t copy_len = (uint32_t)((old_aligned < new_aligned) ? old_aligned : new_aligned);
        mremap_copy_prefix(ctx, new_va, old_addr, copy_len);
        if (prot != 0x3U) {
            (void)a32_vm_protect_locked(ctx, new_va, (uint32_t)new_aligned, prot);
        }
    }
    mremap_priv_release(&priv);
    (void)a32_vm_unmap_locked(ctx, old_addr, (uint32_t)old_aligned);
    a32_vm_sync_to_ctx(ctx);
    a32_vm_lock_release();

    a32_vm_invalidate_jit(ctx, old_addr, (uint32_t)((uint64_t)old_addr + old_aligned));
    if (is_fixed || (prot & (uint32_t)PROT_EXEC) != 0U) {
        a32_vm_invalidate_jit(ctx, new_va, (uint32_t)((uint64_t)new_va + new_aligned));
    }
    return new_va;
}
