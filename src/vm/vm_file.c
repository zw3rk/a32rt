/*
 * src/vm/vm_file.c -- Provider A in-place file backing (Wave U3 A32-4)
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * UNIFICATION-PLAN.md Rev 4, section 5.1: every guest page stays in place
 * at its canonical host address HVA = arena_base + gva for its whole
 * lifetime. MAP_PRIVATE file pages are populated with pread at mmap time
 * (PAGE_COW_CLEAN, write_tag invalid); the first write promotes the page
 * PAGE_COW_CLEAN -> PAGE_COW_DIRTY in place at the same HVA, so sibling
 * CLONE_VM threads, futex keys, and contiguous spans stay coherent and
 * host fork() COW-isolates the MAP_PRIVATE arena natively.
 *
 * The inode table below is the family-shared size/sequence sidecar: the
 * populate path interns (st_dev, st_ino), truncate/write hooks update
 * size and code generation and discard wholly-removed MAP_PRIVATE tails
 * from the Soft-TLB. Curated C11, no inline asm.
 */

#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#else
#define _GNU_SOURCE
#endif
#include "vm/vm32.h"
#include "vm/vm_priv.h"
#include "os/backend.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define A32_VMFILE_INODES 512U

typedef struct {
    bool used;
    uint64_t dev;
    uint64_t ino;
    uint64_t file_size;
} a32_vmfile_inode_t;

static a32_vmfile_inode_t g_inodes[A32_VMFILE_INODES];

/* Tracking-table storage shared with src/vm/vm_file_map.c (see
 * src/vm/vm_priv.h); the inode size sidecar stays private to this file. */
a32_vmfile_map_t g_fmaps[A32_VMFILE_SLOTS];
a32_vmfile_shared_t g_shared[A32_VMFILE_SLOTS];
pthread_mutex_t g_vmfile_lock = PTHREAD_MUTEX_INITIALIZER;

/* Slot selection without mutation: same-start overwrite reuses its slot,
 * otherwise a free slot is required. Returns the slot index or -1 when
 * tracking cannot be maintained (A32-R3-5: reserve before mutation). */
static int fmap_slot_for_locked(uint32_t start_gva)
{
    int free_slot = -1;
    for (unsigned i = 0U; i < A32_VMFILE_SLOTS; i++) {
        if (g_fmaps[i].used && g_fmaps[i].start_gva == start_gva) {
            return (int)i;
        }
        if (!g_fmaps[i].used && free_slot < 0) {
            free_slot = (int)i;
        }
    }
    return free_slot;
}

static int shared_slot_for_locked(uint32_t start_gva)
{
    int free_slot = -1;
    for (unsigned i = 0U; i < A32_VMFILE_SLOTS; i++) {
        if (g_shared[i].used && g_shared[i].start_gva == start_gva) {
            return (int)i;
        }
        if (!g_shared[i].used && free_slot < 0) {
            free_slot = (int)i;
        }
    }
    return free_slot;
}

/* Pre-flight capacity check used before mutating host mappings: 0 when a
 * later record for [start_gva, end_gva) is guaranteed a table slot,
 * -ENOMEM otherwise. Inode capacity is verified at record time. */
int a32_vmfile_check_record(uint32_t start_gva, bool shared)
{
    int rc = 0;
    pthread_mutex_lock(&g_vmfile_lock);
    if (fmap_slot_for_locked(start_gva) < 0 ||
        (shared && shared_slot_for_locked(start_gva) < 0)) {
        rc = -ENOMEM;
    }
    pthread_mutex_unlock(&g_vmfile_lock);
    return rc;
}

/* MAP_FIXED replacement pre-flight (A32-R3-1 follow-up 2, A32-R4-5):
 * capacity check that accounts for slots the retire step is about to
 * free. Only a fully covered record frees its slot for reuse: a
 * same-start record that survives as a suffix ([start, end) covers just
 * its prefix) keeps its slot, so the replacement record still needs a
 * fresh one. A middle split (an old entry spanning beyond both sides)
 * consumes one extra slot for its right half. Returns 0 when the
 * replacement record is guaranteed a slot after the retire, -ENOMEM
 * otherwise. Call BEFORE the retire step drops the old range. */
int a32_vmfile_check_replace(uint32_t start_gva, uint32_t end_gva,
    bool shared)
{
    unsigned free_fmap = 0U;
    unsigned free_shared = 0U;
    unsigned freed_fmap = 0U;
    unsigned freed_shared = 0U;
    unsigned extra_fmap = 0U;
    unsigned extra_shared = 0U;
    unsigned need_fmap;
    unsigned need_shared;
    unsigned i;
    if (end_gva <= start_gva) {
        return -EINVAL;
    }
    pthread_mutex_lock(&g_vmfile_lock);
    for (i = 0U; i < A32_VMFILE_SLOTS; i++) {
        if (!g_fmaps[i].used) {
            free_fmap++;
        } else if (g_fmaps[i].start_gva >= start_gva &&
                   g_fmaps[i].end_gva <= end_gva) {
            /* Fully covered (same-start or not): the retire drops this
             * entry, freeing its slot for the replacement record. A
             * same-start prefix survivor is NOT counted here. */
            freed_fmap++;
        } else if (g_fmaps[i].start_gva < start_gva &&
                   g_fmaps[i].end_gva > end_gva) {
            extra_fmap++;
        }
        if (!g_shared[i].used) {
            free_shared++;
        } else if (g_shared[i].start_gva >= start_gva &&
                   g_shared[i].end_gva <= end_gva) {
            freed_shared++;
        } else if (g_shared[i].start_gva < start_gva &&
                   g_shared[i].end_gva > end_gva) {
            extra_shared++;
        }
    }
    pthread_mutex_unlock(&g_vmfile_lock);
    /* The replacement record always needs exactly one slot after the
     * retire (a same-start survivor keeps its own slot in place); each
     * middle split additionally consumes one slot for the right half
     * the retire step creates. */
    need_fmap = 1U + extra_fmap;
    if (free_fmap + freed_fmap < need_fmap) {
        return -ENOMEM;
    }
    if (shared) {
        need_shared = 1U + extra_shared;
        if (free_shared + freed_shared < need_shared) {
            return -ENOMEM;
        }
    }
    return 0;
}

/* Unmap retire pre-flight (A32-R4-6): 0 when the retire of
 * [unmap_start, unmap_end) cannot run out of split slots. Fully covered
 * and edge-truncated records free or reuse slots in place; only middle
 * splits (an old entry spanning beyond both sides) consume one fresh
 * slot each. Call BEFORE any host or VMA mutation. */
int a32_vmfile_preflight_unmap(uint32_t unmap_start, uint32_t unmap_end)
{
    unsigned free_fmap = 0U;
    unsigned free_shared = 0U;
    unsigned need_fmap = 0U;
    unsigned need_shared = 0U;
    unsigned i;
    if (unmap_end <= unmap_start) {
        return -EINVAL;
    }
    pthread_mutex_lock(&g_vmfile_lock);
    for (i = 0U; i < A32_VMFILE_SLOTS; i++) {
        if (!g_fmaps[i].used) {
            free_fmap++;
        } else if (g_fmaps[i].start_gva < unmap_start &&
                   g_fmaps[i].end_gva > unmap_end) {
            need_fmap++;
        }
        if (!g_shared[i].used) {
            free_shared++;
        } else if (g_shared[i].start_gva < unmap_start &&
                   g_shared[i].end_gva > unmap_end) {
            need_shared++;
        }
    }
    pthread_mutex_unlock(&g_vmfile_lock);
    if (free_fmap < need_fmap || free_shared < need_shared) {
        return -ENOMEM;
    }
    return 0;
}

/* Transactional record: the slot is secured before any mutation, so a
 * full table reports -ENOMEM without disturbing existing entries. */
static int fmap_record_locked(uint64_t dev, uint64_t ino, uint32_t start_gva,
    uint32_t end_gva, uint64_t file_offset, int host_fd)
{
    int slot = fmap_slot_for_locked(start_gva);
    int dup_fd = -1;
    if (slot < 0) {
        return -ENOMEM;
    }
    if (host_fd >= 0) {
        dup_fd = fcntl(host_fd, F_DUPFD_CLOEXEC, 3);
        if (dup_fd < 0) {
            return -errno;
        }
    }
    if (g_fmaps[slot].used && g_fmaps[slot].dup_fd >= 0) {
        close(g_fmaps[slot].dup_fd);
    }
    g_fmaps[slot].used = true;
    g_fmaps[slot].dev = dev;
    g_fmaps[slot].ino = ino;
    g_fmaps[slot].start_gva = start_gva;
    g_fmaps[slot].end_gva = end_gva;
    g_fmaps[slot].file_offset = file_offset;
    g_fmaps[slot].dup_fd = dup_fd;
    return 0;
}

static a32_vmfile_inode_t *inode_intern_locked(uint64_t dev, uint64_t ino)
{
    unsigned i;
    unsigned free_slot = A32_VMFILE_INODES;
    for (i = 0U; i < A32_VMFILE_INODES; i++) {
        if (g_inodes[i].used && g_inodes[i].dev == dev &&
            g_inodes[i].ino == ino) {
            return &g_inodes[i];
        }
        if (!g_inodes[i].used && free_slot == A32_VMFILE_INODES) {
            free_slot = i;
        }
    }
    if (free_slot == A32_VMFILE_INODES) {
        return NULL;
    }
    g_inodes[free_slot].used = true;
    g_inodes[free_slot].dev = dev;
    g_inodes[free_slot].ino = ino;
    g_inodes[free_slot].file_size = 0U;
    return &g_inodes[free_slot];
}

/* Enforce the EOF boundary for one fresh mapping: pages wholly beyond
 * round_up(file_size) become inaccessible (A32-R3-2). File-backed pages
 * are left alone here; the caller publishes them through the normal
 * region path afterward. Deliberately never clears: fresh pages carry no
 * stale cuts (the retire step dropped them), and clearing would PROT_NONE
 * pages the populate step is about to fill. */
static void vmfile_enforce_eof_locked(uint32_t start_gva, uint32_t len,
    uint64_t file_offset, uint64_t file_size)
{
    uint32_t keep = a32_vmfile_eof_keep(len, file_offset, file_size);
    if ((uint64_t)keep < (uint64_t)len) {
        a32_vm32_set_eof_cut(start_gva + keep, len - keep, true);
    }
}

int a32_vmfile_populate_private(void *hva, int host_fd, uint64_t file_offset,
    size_t len)
{
    struct stat st;
    size_t done = 0U;
    int rec_rc = 0;
    if (hva == NULL || host_fd < 0 || len == 0U) {
        return -EINVAL;
    }
    if (fstat(host_fd, &st) != 0) {
        return -errno;
    }
    {
        uintptr_t arena = a32_vm32_arena_base();
        pthread_mutex_lock(&g_vmfile_lock);
        a32_vmfile_inode_t *e =
            inode_intern_locked((uint64_t)st.st_dev, (uint64_t)st.st_ino);
        if (e == NULL) {
            pthread_mutex_unlock(&g_vmfile_lock);
            return -ENOMEM;
        }
        /* Track the actual file size, never the mapping extent: extent
         * would corrupt the growth/shrink baseline and hide beyond-EOF
         * pages that must stay inaccessible (A32-R3-2). */
        e->file_size = (uint64_t)st.st_size;
        if ((uintptr_t)hva >= arena &&
            ((uintptr_t)hva - arena) + len <= A32_VM32_WINDOW_SIZE) {
            uint32_t sgva = (uint32_t)((uintptr_t)hva - arena);
            rec_rc = fmap_record_locked((uint64_t)st.st_dev,
                (uint64_t)st.st_ino, sgva, sgva + (uint32_t)len,
                file_offset, host_fd);
            if (rec_rc == 0) {
                vmfile_enforce_eof_locked(sgva, (uint32_t)len, file_offset,
                    (uint64_t)st.st_size);
            }
        }
        pthread_mutex_unlock(&g_vmfile_lock);
        if (rec_rc != 0) {
            return rec_rc;
        }
    }
    while (done < len) {
        ssize_t r = pread(host_fd, (char *)hva + done, len - done,
            (off_t)(file_offset + (uint64_t)done));
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -errno;
        }
        if (r == 0) {
            /* RC-U3-r3 #5: zero-fill only the partial page tail up to
             * align_up(eof, 4096). Pages wholly beyond EOF stay
             * untouched: on A2 a file-backed touch there can raise
             * SIGBUS, and anonymous staging is already zeroed. */
            if (st.st_size >= 0) {
                uint64_t fill_start = file_offset + (uint64_t)done;
                uint64_t fill_cap = file_offset + (uint64_t)len;
                uint64_t fill_end =
                    (((uint64_t)st.st_size + 4095ULL) & ~4095ULL);
                if (fill_end > fill_cap) {
                    fill_end = fill_cap;
                }
                if (fill_end > fill_start) {
                    memset((char *)hva + done, 0,
                        (size_t)(fill_end - fill_start));
                }
            }
            break;
        }
        done += (size_t)r;
    }
    return 0;
}

void a32_vmfile_promote_cow(uint32_t gva_page, bool readable, bool writable)
{
    uintptr_t base = a32_vm32_arena_base();
    uint32_t page = gva_page & ~0xFFFU;
    if (base == 0U) {
        return;
    }
    /* In place at the same HVA: only the translation changes from
     * PAGE_COW_CLEAN (write_tag invalid) to PAGE_COW_DIRTY. */
    a32_vm32_tlb_fill(page, base + (uintptr_t)page, readable, writable);
}

/* Promote every 4 KiB page in [start_gva, end_gva) after the host
 * mapping gains PROT_WRITE (called by src/vm.c mprotect wiring). */
void a32_vmfile_promote_range(uint32_t start_gva, uint32_t end_gva,
    bool readable, bool writable)
{
    uint64_t end = ((uint64_t)end_gva + 0xFFFU) & ~0xFFFU;
    uint64_t p = (uint64_t)(start_gva & ~0xFFFU);
    for (; p < end; p += 0x1000U) {
        a32_vmfile_promote_cow((uint32_t)p, readable, writable);
    }
}

/* Identity-aware resize sync shared by the ftruncate/truncate/write
 * hooks below. Only the interned (dev, ino) entry matching the file is
 * touched; unrelated mappings keep their state. On shrink, pages wholly
 * beyond the new EOF are marked with A32_PROT_EOF_CUT and dropped from
 * the Soft-TLB so both interpreter and DBT accesses fault; on regrowth,
 * the EOF cut is cleared and staged mappings repopulate from dup_fd. */
static int vmfile_sync_size(uint64_t dev, uint64_t ino, uint64_t new_size)
{
    uint64_t cut_off = (new_size + 0xFFFULL) & ~0xFFFULL;
    uint64_t old_size = new_size;
    unsigned i;
    bool shrink = false;
    bool grow = false;
    uintptr_t arena = a32_vm32_arena_base();

    pthread_mutex_lock(&g_vmfile_lock);
    for (i = 0U; i < A32_VMFILE_INODES; i++) {
        if (!g_inodes[i].used || g_inodes[i].dev != dev ||
            g_inodes[i].ino != ino) {
            continue;
        }
        old_size = g_inodes[i].file_size;
        if (new_size < old_size) {
            shrink = true;
            g_inodes[i].file_size = new_size;
        } else if (new_size > old_size) {
            grow = true;
            g_inodes[i].file_size = new_size;
        }
    }
    if (shrink) {
        for (i = 0U; i < A32_VMFILE_SLOTS; i++) {
            if (!g_fmaps[i].used || g_fmaps[i].dev != dev ||
                g_fmaps[i].ino != ino) {
                continue;
            }
            uint64_t map_len = (uint64_t)(g_fmaps[i].end_gva - g_fmaps[i].start_gva);
            uint64_t map_end_off = g_fmaps[i].file_offset + map_len;
            if (arena != 0U && new_size < cut_off &&
                cut_off > g_fmaps[i].file_offset && new_size < map_end_off) {
                uint64_t z_start = (new_size > g_fmaps[i].file_offset)
                    ? (new_size - g_fmaps[i].file_offset) : 0ULL;
                uint64_t z_end = ((cut_off < map_end_off) ? cut_off : map_end_off) -
                    g_fmaps[i].file_offset;
                if (z_end > z_start && g_fmaps[i].dup_fd >= 0) {
                    uint32_t zgva = g_fmaps[i].start_gva + (uint32_t)z_start;
                    if (a32_vm32_range_prot_ok(zgva, (uint32_t)(z_end - z_start), PROT_WRITE)) {
                        memset((void *)(arena + (uintptr_t)zgva), 0, (size_t)(z_end - z_start));
                    }
                }
            }
            if (map_end_off > cut_off) {
                uint64_t inv_off = (cut_off > g_fmaps[i].file_offset)
                    ? (cut_off - g_fmaps[i].file_offset) : 0ULL;
                uint32_t inv_start = g_fmaps[i].start_gva + (uint32_t)inv_off;
                if (g_fmaps[i].end_gva > inv_start) {
                    a32_vm32_set_eof_cut(inv_start,
                        g_fmaps[i].end_gva - inv_start, true);
                }
            }
        }
    } else if (grow) {
        uint64_t old_cut = (old_size + 0xFFFULL) & ~0xFFFULL;
        for (i = 0U; i < A32_VMFILE_SLOTS; i++) {
            if (!g_fmaps[i].used || g_fmaps[i].dev != dev ||
                g_fmaps[i].ino != ino) {
                continue;
            }
            uint64_t map_len = (uint64_t)(g_fmaps[i].end_gva - g_fmaps[i].start_gva);
            uint64_t map_end_off = g_fmaps[i].file_offset + map_len;
            if (cut_off > g_fmaps[i].file_offset && old_cut < map_end_off) {
                uint64_t u_start_off = (old_cut > g_fmaps[i].file_offset)
                    ? (old_cut - g_fmaps[i].file_offset) : 0ULL;
                uint64_t u_end_off = ((cut_off < map_end_off) ? cut_off : map_end_off) -
                    g_fmaps[i].file_offset;
                if (u_end_off > u_start_off) {
                    uint32_t ugva = g_fmaps[i].start_gva + (uint32_t)u_start_off;
                    uint32_t ulen = (uint32_t)(u_end_off - u_start_off);
                    a32_vm32_set_eof_cut(ugva, ulen, false);
                    if (arena != 0U && g_fmaps[i].dup_fd >= 0 &&
                        a32_vm32_current_regime() == A32_VM32_REGIME_A2_SOFT_TLB) {
                        memset((void *)(arena + (uintptr_t)ugva), 0, ulen);
                        (void)pread(g_fmaps[i].dup_fd,
                            (void *)(arena + (uintptr_t)ugva), ulen,
                            (off_t)(g_fmaps[i].file_offset + u_start_off));
                    }
                }
            }
        }
    }
    pthread_mutex_unlock(&g_vmfile_lock);
    return 0;
}

int a32_vmfile_sync_fd(int host_fd)
{
    struct stat st;
    if (host_fd < 0) {
        return -EINVAL;
    }
    if (fstat(host_fd, &st) != 0) {
        return -errno;
    }
    return vmfile_sync_size((uint64_t)st.st_dev, (uint64_t)st.st_ino,
        (uint64_t)st.st_size);
}

int a32_vmfile_sync_path(const char *host_path)
{
    struct stat st;
    if (host_path == NULL || host_path[0] == '\0') {
        return -EINVAL;
    }
    if (stat(host_path, &st) != 0) {
        return -errno;
    }
    return vmfile_sync_size((uint64_t)st.st_dev, (uint64_t)st.st_ino,
        (uint64_t)st.st_size);
}

/* Transactional shared record (A32-R3-5): the descriptor is duplicated
 * before locking, both table slots are secured before any mutation, and
 * the inode entry is interned before anything is published. Any failure
 * reports a negative errno with all tables untouched. */
int a32_vmfile_record_shared(uint32_t start_gva, uint32_t end_gva, int fd,
    uint32_t pgoffset)
{
    struct stat st;
    bool have_st = false;
    int dup_fd;
    int fmap_slot;
    int sh_slot;
    int frc;
    if (fd < 0 || start_gva >= end_gva) {
        return -EINVAL;
    }
    dup_fd = fcntl(fd, F_DUPFD_CLOEXEC, 3);
    if (dup_fd < 0) {
        return -errno;
    }
    have_st = (fstat(dup_fd, &st) == 0);
    pthread_mutex_lock(&g_vmfile_lock);
    fmap_slot = fmap_slot_for_locked(start_gva);
    sh_slot = shared_slot_for_locked(start_gva);
    if (fmap_slot < 0 || sh_slot < 0) {
        pthread_mutex_unlock(&g_vmfile_lock);
        close(dup_fd);
        return -ENOMEM;
    }
    if (have_st) {
        a32_vmfile_inode_t *e =
            inode_intern_locked((uint64_t)st.st_dev, (uint64_t)st.st_ino);
        if (e == NULL) {
            pthread_mutex_unlock(&g_vmfile_lock);
            close(dup_fd);
            return -ENOMEM;
        }
        e->file_size = (uint64_t)st.st_size;
    }
    if (have_st) {
        frc = fmap_record_locked((uint64_t)st.st_dev, (uint64_t)st.st_ino,
            start_gva, end_gva, (uint64_t)pgoffset * 4096ULL, -1);
        if (frc == 0) {
            vmfile_enforce_eof_locked(start_gva, end_gva - start_gva,
                (uint64_t)pgoffset * 4096ULL, (uint64_t)st.st_size);
        }
    } else {
        frc = 0;
    }
    if (frc == 0) {
        if (g_shared[sh_slot].used && g_shared[sh_slot].dup_fd >= 0) {
            close(g_shared[sh_slot].dup_fd);
        }
        g_shared[sh_slot].used = true;
        g_shared[sh_slot].start_gva = start_gva;
        g_shared[sh_slot].end_gva = end_gva;
        g_shared[sh_slot].dup_fd = dup_fd;
        g_shared[sh_slot].pgoffset = pgoffset;
        dup_fd = -1;
    }
    pthread_mutex_unlock(&g_vmfile_lock);
    if (dup_fd >= 0) {
        close(dup_fd);
    }
    return frc;
}

bool a32_vmfile_lookup_shared(uint32_t start_gva, int *out_fd,
    uint32_t *out_pgoffset)
{
    bool found = false;
    pthread_mutex_lock(&g_vmfile_lock);
    for (unsigned i = 0U; i < A32_VMFILE_SLOTS; i++) {
        if (g_shared[i].used && start_gva >= g_shared[i].start_gva &&
            start_gva < g_shared[i].end_gva) {
            uint32_t delta = (start_gva - g_shared[i].start_gva) / 4096U;
            if (out_fd) {
                *out_fd = g_shared[i].dup_fd;
            }
            if (out_pgoffset) {
                *out_pgoffset = g_shared[i].pgoffset + delta;
            }
            found = true;
            break;
        }
    }
    pthread_mutex_unlock(&g_vmfile_lock);
    return found;
}

int a32_vmfile_grow_shared(int sh_fd, uint32_t sh_pgoff, uint64_t new_len)
{
    struct stat st;
    if (sh_fd < 0) {
        return -EINVAL;
    }
    if (fstat(sh_fd, &st) != 0) {
        return -errno;
    }
    uint64_t hp = (uint64_t)a32_backend_page_size();
    uint64_t need = ((uint64_t)sh_pgoff * 4096ULL + new_len + hp - 1ULL) &
        ~(hp - 1ULL);
    if ((uint64_t)st.st_size < need) {
        if (ftruncate(sh_fd, (off_t)need) != 0) {
            return -errno;
        }
    }
    return 0;
}

/* Free-slot probe for a spanning split (A32-R3-5): secures the table
 * slot before the caller duplicates any descriptor or truncates the
 * original entry. Returns the slot, or -1 when the split cannot be
 * tracked — the caller must then leave the original entry untouched. */
static int vmfile_free_slot_locked(bool is_shared)
{
    for (unsigned j = 0U; j < A32_VMFILE_SLOTS; j++) {
        bool taken = is_shared ? g_shared[j].used : g_fmaps[j].used;
        if (!taken) {
            return (int)j;
        }
    }
    return -1;
}

/* Transactional retire for munmap/MAP_FIXED (A32-R4-6): all split
 * slots and duplicated descriptors are reserved BEFORE any mutation,
 * so exhaustion or EMFILE reports an error with every table untouched.
 * Returns 0 on success, -ENOMEM when a middle split cannot be tracked,
 * -errno when a descriptor cannot be duplicated, -EINVAL on an empty
 * range. Callers must check BEFORE mutating host or VMA state. */
int a32_vmfile_unmap_shared(uint32_t unmap_start, uint32_t unmap_end)
{
    unsigned need_shared = 0U;
    unsigned need_fmap = 0U;
    unsigned free_shared = 0U;
    unsigned free_fmap = 0U;
    unsigned i;
    unsigned si;
    unsigned fi;
    int *sh_dup = NULL;
    int *fm_dup = NULL;
    int rc = 0;
    if (unmap_end <= unmap_start) {
        return -EINVAL;
    }
    pthread_mutex_lock(&g_vmfile_lock);
    for (i = 0U; i < A32_VMFILE_SLOTS; i++) {
        if (g_shared[i].used) {
            if (g_shared[i].start_gva < unmap_start &&
                g_shared[i].end_gva > unmap_end) {
                need_shared++;
            }
        } else {
            free_shared++;
        }
        if (g_fmaps[i].used) {
            if (g_fmaps[i].start_gva < unmap_start &&
                g_fmaps[i].end_gva > unmap_end) {
                need_fmap++;
            }
        } else {
            free_fmap++;
        }
    }
    if (free_shared < need_shared || free_fmap < need_fmap) {
        pthread_mutex_unlock(&g_vmfile_lock);
        return -ENOMEM;
    }
    if (need_shared > 0U) {
        sh_dup = (int *)calloc(need_shared, sizeof(int));
        if (sh_dup == NULL) {
            pthread_mutex_unlock(&g_vmfile_lock);
            return -ENOMEM;
        }
        for (i = 0U; i < need_shared; i++) {
            sh_dup[i] = -1;
        }
    }
    if (need_fmap > 0U) {
        fm_dup = (int *)calloc(need_fmap, sizeof(int));
        if (fm_dup == NULL) {
            free(sh_dup);
            pthread_mutex_unlock(&g_vmfile_lock);
            return -ENOMEM;
        }
        for (i = 0U; i < need_fmap; i++) {
            fm_dup[i] = -1;
        }
    }
    /* Reserve every descriptor the splits need. No table has been
     * touched yet, so any failure below leaves all state intact. */
    si = 0U;
    fi = 0U;
    for (i = 0U; i < A32_VMFILE_SLOTS; i++) {
        if (g_shared[i].used && g_shared[i].start_gva < unmap_start &&
            g_shared[i].end_gva > unmap_end) {
            int fd = fcntl(g_shared[i].dup_fd, F_DUPFD_CLOEXEC, 3);
            if (fd < 0) {
                rc = -errno;
                break;
            }
            sh_dup[si] = fd;
            si++;
        }
        if (g_fmaps[i].used && g_fmaps[i].start_gva < unmap_start &&
            g_fmaps[i].end_gva > unmap_end && g_fmaps[i].dup_fd >= 0) {
            int fd = fcntl(g_fmaps[i].dup_fd, F_DUPFD_CLOEXEC, 3);
            if (fd < 0) {
                rc = -errno;
                break;
            }
            fm_dup[fi] = fd;
            fi++;
        }
    }
    if (rc != 0) {
        for (i = 0U; i < si; i++) {
            close(sh_dup[i]);
        }
        for (i = 0U; i < fi; i++) {
            close(fm_dup[i]);
        }
        free(sh_dup);
        free(fm_dup);
        pthread_mutex_unlock(&g_vmfile_lock);
        return rc;
    }
    /* All resources are secured: the mutation below cannot fail. Middle
     * splits consume the reserved slots/descriptors in record order. */
    si = 0U;
    fi = 0U;
    for (i = 0U; i < A32_VMFILE_SLOTS; i++) {
        if (g_shared[i].used) {
            uint32_t s = g_shared[i].start_gva;
            uint32_t e = g_shared[i].end_gva;
            if (e > unmap_start && s < unmap_end) {
                if (s >= unmap_start && e <= unmap_end) {
                    /* Fully covered: drop tracking only. The host range
                     * stays whatever the caller installed (PROT_NONE on
                     * munmap, the new mapping on MAP_FIXED retire) —
                     * never resurrected as anonymous memory (A32-R3-1). */
                    close(g_shared[i].dup_fd);
                    g_shared[i].used = false;
                } else if (s < unmap_start && e <= unmap_end) {
                    g_shared[i].end_gva = unmap_start;
                } else if (s >= unmap_start && e > unmap_end) {
                    g_shared[i].pgoffset += (unmap_end - s) / 4096U;
                    g_shared[i].start_gva = unmap_end;
                } else {
                    int r_slot = vmfile_free_slot_locked(true);
                    /* r_slot is guaranteed: preflight secured one free
                     * slot per middle split, and only middle splits
                     * consume slots here. */
                    g_shared[i].end_gva = unmap_start;
                    g_shared[r_slot].used = true;
                    g_shared[r_slot].start_gva = unmap_end;
                    g_shared[r_slot].end_gva = e;
                    g_shared[r_slot].dup_fd = sh_dup[si];
                    g_shared[r_slot].pgoffset =
                        g_shared[i].pgoffset + (unmap_end - s) / 4096U;
                    sh_dup[si] = -1;
                    si++;
                }
            }
        }
        if (g_fmaps[i].used) {
            uint32_t s = g_fmaps[i].start_gva;
            uint32_t e = g_fmaps[i].end_gva;
            if (e > unmap_start && s < unmap_end) {
                if (s >= unmap_start && e <= unmap_end) {
                    if (g_fmaps[i].dup_fd >= 0) {
                        close(g_fmaps[i].dup_fd);
                        g_fmaps[i].dup_fd = -1;
                    }
                    g_fmaps[i].used = false;
                } else if (s < unmap_start && e <= unmap_end) {
                    g_fmaps[i].end_gva = unmap_start;
                } else if (s >= unmap_start && e > unmap_end) {
                    g_fmaps[i].file_offset += (uint64_t)(unmap_end - s);
                    g_fmaps[i].start_gva = unmap_end;
                } else {
                    int r_slot = vmfile_free_slot_locked(false);
                    int r_fd = -1;
                    if (g_fmaps[i].dup_fd >= 0) {
                        /* Pre-duplicated above in record order; the
                         * fi-th anonymous-backed middle split takes
                         * the fi-th reserved descriptor. */
                        r_fd = fm_dup[fi];
                        fm_dup[fi] = -1;
                        fi++;
                    }
                    g_fmaps[i].end_gva = unmap_start;
                    g_fmaps[r_slot].used = true;
                    g_fmaps[r_slot].dev = g_fmaps[i].dev;
                    g_fmaps[r_slot].ino = g_fmaps[i].ino;
                    g_fmaps[r_slot].start_gva = unmap_end;
                    g_fmaps[r_slot].end_gva = e;
                    g_fmaps[r_slot].file_offset =
                        g_fmaps[i].file_offset + (uint64_t)(unmap_end - s);
                    g_fmaps[r_slot].dup_fd = r_fd;
                }
            }
        }
    }
    /* Reclaim inode entries whose last mapping just disappeared so the
     * inode table cannot leak over the process lifetime (A32-R3-5).
     * Re-interning later restores the size from fstat. */
    for (unsigned i = 0U; i < A32_VMFILE_INODES; i++) {
        bool live = false;
        if (!g_inodes[i].used) {
            continue;
        }
        for (unsigned j = 0U; j < A32_VMFILE_SLOTS; j++) {
            if (g_fmaps[j].used && g_fmaps[j].dev == g_inodes[i].dev &&
                g_fmaps[j].ino == g_inodes[i].ino) {
                live = true;
                break;
            }
        }
        if (!live) {
            g_inodes[i].used = false;
        }
    }
    free(sh_dup);
    free(fm_dup);
    pthread_mutex_unlock(&g_vmfile_lock);
    return 0;
}
