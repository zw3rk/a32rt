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

#define A32_VMFILE_SLOTS 64U

typedef struct {
    bool used;
    uint64_t dev;
    uint64_t ino;
    uint64_t file_size;
} a32_vmfile_inode_t;

typedef struct {
    bool used;
    uint64_t dev;
    uint64_t ino;
    uint32_t start_gva;
    uint32_t end_gva;
    uint64_t file_offset;
    int dup_fd;
} a32_vmfile_map_t;

static a32_vmfile_inode_t g_inodes[A32_VMFILE_SLOTS];
static a32_vmfile_map_t g_fmaps[A32_VMFILE_SLOTS];
static pthread_mutex_t g_vmfile_lock = PTHREAD_MUTEX_INITIALIZER;

static void fmap_record_locked(uint64_t dev, uint64_t ino, uint32_t start_gva,
    uint32_t end_gva, uint64_t file_offset, int host_fd)
{
    unsigned slot = A32_VMFILE_SLOTS;
    for (unsigned i = 0U; i < A32_VMFILE_SLOTS; i++) {
        if (g_fmaps[i].used && g_fmaps[i].start_gva == start_gva) {
            if (g_fmaps[i].dup_fd >= 0) {
                close(g_fmaps[i].dup_fd);
                g_fmaps[i].dup_fd = -1;
            }
            slot = i;
            break;
        }
        if (!g_fmaps[i].used && slot == A32_VMFILE_SLOTS) {
            slot = i;
        }
    }
    if (slot < A32_VMFILE_SLOTS) {
        g_fmaps[slot].used = true;
        g_fmaps[slot].dev = dev;
        g_fmaps[slot].ino = ino;
        g_fmaps[slot].start_gva = start_gva;
        g_fmaps[slot].end_gva = end_gva;
        g_fmaps[slot].file_offset = file_offset;
        g_fmaps[slot].dup_fd = (host_fd >= 0)
            ? fcntl(host_fd, F_DUPFD_CLOEXEC, 3) : -1;
    }
}

static a32_vmfile_inode_t *inode_intern_locked(uint64_t dev, uint64_t ino)
{
    unsigned i;
    unsigned free_slot = A32_VMFILE_SLOTS;
    for (i = 0U; i < A32_VMFILE_SLOTS; i++) {
        if (g_inodes[i].used && g_inodes[i].dev == dev &&
            g_inodes[i].ino == ino) {
            return &g_inodes[i];
        }
        if (!g_inodes[i].used && free_slot == A32_VMFILE_SLOTS) {
            free_slot = i;
        }
    }
    if (free_slot == A32_VMFILE_SLOTS) {
        return NULL;
    }
    g_inodes[free_slot].used = true;
    g_inodes[free_slot].dev = dev;
    g_inodes[free_slot].ino = ino;
    g_inodes[free_slot].file_size = 0U;
    return &g_inodes[free_slot];
}

/* Populate a fresh MAP_PRIVATE file mapping: pread the file bytes while
 * the range is temporarily RW, propagate errors, zero-fill the EOF tail
 * only inside the mapped range, then apply the final guest prot. On
 * failure the range is left PROT_NONE. Non-private mappings return 0. */
int a32_vmfile_populate_if_private(void *hva, int host_fd,
    uint64_t file_offset, size_t len, int host_flags, int final_prot)
{
    bool rw_stage;
    int rc;
    if ((host_flags & (MAP_ANONYMOUS | MAP_SHARED)) != 0) {
        return 0;
    }
    if (hva == NULL || host_fd < 0 || len == 0U) {
        return -EINVAL;
    }
    rw_stage = ((final_prot & (PROT_READ | PROT_WRITE)) !=
        (PROT_READ | PROT_WRITE));
    if (rw_stage) {
        rc = a32_backend_protect(hva, len, PROT_READ | PROT_WRITE);
        if (rc != 0) {
            return rc;
        }
    }
    rc = a32_vmfile_populate_private(hva, host_fd, file_offset, len);
    if (rc == 0 && rw_stage) {
        rc = a32_backend_protect(hva, len, final_prot);
    }
    if (rc != 0) {
        void *out = NULL;
        (void)a32_backend_map_anon(hva, len, 0, 1, 1, 0, &out);
    }
    return rc;
}

/* A2 staged mapping (RC-U3-r3 #5): reserve anonymous RW memory for a
 * private file range. The caller preads the file bytes over it (see
 * a32_vmfile_populate_private) and flips to the committed protection
 * afterward, so no file-backed page can SIGBUS on a beyond-EOF touch. */
int a32_vmfile_map_staged(void *hint_addr, size_t len, int host_fd,
    uint64_t file_offset, void **out)
{
    (void)host_fd;
    (void)file_offset;
    if (hint_addr == NULL || len == 0U || out == NULL) {
        return -EINVAL;
    }
    return a32_backend_map_anon(hint_addr, len, PROT_READ | PROT_WRITE,
        1, 1, 0, out);
}

static int vmfile_create_shared_anon_fd(size_t len)
{
#if defined(__linux__)
    int mfd = memfd_create("a32-shanon", MFD_CLOEXEC);
    if (mfd >= 0) {
        size_t mhp = a32_backend_page_size();
        size_t mfsize = (len + mhp - 1U) & ~(mhp - 1U);
        if (ftruncate(mfd, (off_t)mfsize) != 0) {
            int merr = errno;
            close(mfd);
            return -merr;
        }
        return mfd;
    }
#endif
    const char *dir = getenv("TMPDIR");
    if (dir == NULL || dir[0] == '\0') {
        dir = getenv("A32RT_CACHE");
    }
    if (dir == NULL || dir[0] == '\0') {
        dir = "/tmp";
    }
    char tmpl[256];
    int tlen = snprintf(tmpl, sizeof(tmpl), "%s/a32-shanon-XXXXXX", dir);
    if (tlen <= 0 || (size_t)tlen >= sizeof(tmpl)) {
        return -ENAMETOOLONG;
    }
    int fd = mkstemp(tmpl);
    if (fd < 0) {
        return -errno;
    }
    (void)unlink(tmpl);
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    size_t hp = a32_backend_page_size();
    size_t fsize = (len + hp - 1U) & ~(hp - 1U);
    if (ftruncate(fd, (off_t)fsize) != 0) {
        int err = errno;
        close(fd);
        return -err;
    }
    return fd;
}

/* Reserve the host mapping for one guest range and pread-populate it.
 * When a2_stage is set the range is an A2 private file mapping: stage
 * it in anonymous memory instead of a native file-backed mmap so no
 * file-backed page can SIGBUS on a beyond-EOF touch. The staged range
 * then flows through the same populate + final-protection path. */
int a32_vmfile_map_commit(void *host_target, size_t len, int host_prot,
    int host_fd, uint64_t file_offset, int host_flags, bool a2_stage,
    void **out)
{
    int map_rc;
    int pop_rc;
    void *res = NULL;

    if (host_target == NULL || len == 0U || out == NULL) {
        return -EINVAL;
    }
    if (a2_stage) {
        map_rc = a32_vmfile_map_staged(host_target, len, host_fd,
            file_offset, &res);
    } else if ((host_flags & (MAP_ANONYMOUS | MAP_SHARED)) ==
        (MAP_ANONYMOUS | MAP_SHARED)) {
        int sh_fd = vmfile_create_shared_anon_fd(len);
        if (sh_fd < 0) {
            return sh_fd;
        }
        map_rc = a32_backend_map_file(host_target, len, host_prot,
            1, 1, 1, sh_fd, 0ULL, &res);
        if (map_rc == 0) {
            uintptr_t arena = a32_vm32_arena_base();
            if ((uintptr_t)res >= arena &&
                ((uintptr_t)res - arena) + len <= A32_VM32_WINDOW_SIZE) {
                uint32_t sgva = (uint32_t)((uintptr_t)res - arena);
                a32_vmfile_record_shared(sgva, sgva + (uint32_t)len,
                    sh_fd, 0U);
            }
        }
        close(sh_fd);
    } else if ((host_flags & MAP_ANONYMOUS) != 0) {
        map_rc = a32_backend_map_anon(host_target, len, host_prot,
            1, 1, (host_flags & MAP_SHARED) != 0, &res);
    } else {
        map_rc = a32_backend_map_file(host_target, len, host_prot,
            (host_flags & MAP_SHARED) != 0, 1, 1, host_fd, file_offset,
            &res);
    }
    if (map_rc != 0) {
        return map_rc;
    }
    pop_rc = a32_vmfile_populate_if_private(res, host_fd, file_offset,
        len, host_flags, host_prot);
    if (pop_rc != 0) {
        return pop_rc;
    }
    *out = res;
    return 0;
}

int a32_vmfile_populate_private(void *hva, int host_fd, uint64_t file_offset,
    size_t len)
{
    struct stat st;
    size_t done = 0U;
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
        if (e != NULL) {
            uint64_t end = file_offset + (uint64_t)len;
            if (end > e->file_size) {
                e->file_size = end;
            }
            if ((uint64_t)st.st_size > e->file_size) {
                e->file_size = (uint64_t)st.st_size;
            }
        }
        if ((uintptr_t)hva >= arena &&
            ((uintptr_t)hva - arena) + len <= A32_VM32_WINDOW_SIZE) {
            uint32_t sgva = (uint32_t)((uintptr_t)hva - arena);
            fmap_record_locked((uint64_t)st.st_dev, (uint64_t)st.st_ino,
                sgva, sgva + (uint32_t)len, file_offset, host_fd);
            a32_vm32_set_eof_cut(sgva, (uint32_t)len, false);
        }
        pthread_mutex_unlock(&g_vmfile_lock);
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
    for (i = 0U; i < A32_VMFILE_SLOTS; i++) {
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

typedef struct {
    bool used;
    uint32_t start_gva;
    uint32_t end_gva;
    int dup_fd;
    uint32_t pgoffset;
} a32_vmfile_shared_t;

static a32_vmfile_shared_t g_shared[A32_VMFILE_SLOTS];

void a32_vmfile_record_shared(uint32_t start_gva, uint32_t end_gva, int fd,
    uint32_t pgoffset)
{
    struct stat st;
    if (fd < 0 || start_gva >= end_gva) {
        return;
    }
    int dup_fd = fcntl(fd, F_DUPFD_CLOEXEC, 3);
    if (dup_fd < 0) {
        return;
    }
    bool have_st = (fstat(dup_fd, &st) == 0);
    pthread_mutex_lock(&g_vmfile_lock);
    if (have_st) {
        a32_vmfile_inode_t *e =
            inode_intern_locked((uint64_t)st.st_dev, (uint64_t)st.st_ino);
        if (e != NULL && (uint64_t)st.st_size > e->file_size) {
            e->file_size = (uint64_t)st.st_size;
        }
        fmap_record_locked((uint64_t)st.st_dev, (uint64_t)st.st_ino,
            start_gva, end_gva, (uint64_t)pgoffset * 4096ULL, -1);
        a32_vm32_set_eof_cut(start_gva, end_gva - start_gva, false);
    }
    unsigned slot = A32_VMFILE_SLOTS;
    for (unsigned i = 0U; i < A32_VMFILE_SLOTS; i++) {
        if (g_shared[i].used && g_shared[i].start_gva == start_gva) {
            close(g_shared[i].dup_fd);
            slot = i;
            break;
        }
        if (!g_shared[i].used && slot == A32_VMFILE_SLOTS) {
            slot = i;
        }
    }
    if (slot < A32_VMFILE_SLOTS) {
        g_shared[slot].used = true;
        g_shared[slot].start_gva = start_gva;
        g_shared[slot].end_gva = end_gva;
        g_shared[slot].dup_fd = dup_fd;
        g_shared[slot].pgoffset = pgoffset;
    } else {
        close(dup_fd);
    }
    pthread_mutex_unlock(&g_vmfile_lock);
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

void a32_vmfile_unmap_shared(uint32_t unmap_start, uint32_t unmap_end)
{
    if (unmap_end <= unmap_start) {
        return;
    }
    size_t hp = a32_backend_page_size();
    uintptr_t arena = a32_vm32_arena_base();
    pthread_mutex_lock(&g_vmfile_lock);
    for (unsigned i = 0U; i < A32_VMFILE_SLOTS; i++) {
        if (g_shared[i].used) {
            uint32_t s = g_shared[i].start_gva;
            uint32_t e = g_shared[i].end_gva;
            if (e > unmap_start && s < unmap_end) {
                if (s >= unmap_start && e <= unmap_end) {
                    if (hp > 4096U && arena != 0U && (s & (hp - 1U)) == 0U) {
                        size_t hlen = ((size_t)(e - s) + hp - 1U) & ~(hp - 1U);
                        (void)mmap((void *)(arena + s), hlen,
                            PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
                    }
                    close(g_shared[i].dup_fd);
                    g_shared[i].used = false;
                } else if (s < unmap_start && e <= unmap_end) {
                    g_shared[i].end_gva = unmap_start;
                } else if (s >= unmap_start && e > unmap_end) {
                    g_shared[i].pgoffset += (unmap_end - s) / 4096U;
                    g_shared[i].start_gva = unmap_end;
                } else {
                    g_shared[i].end_gva = unmap_start;
                    int r_fd = fcntl(g_shared[i].dup_fd, F_DUPFD_CLOEXEC, 3);
                    if (r_fd >= 0) {
                        bool placed = false;
                        for (unsigned j = 0U; j < A32_VMFILE_SLOTS; j++) {
                            if (!g_shared[j].used) {
                                g_shared[j].used = true;
                                g_shared[j].start_gva = unmap_end;
                                g_shared[j].end_gva = e;
                                g_shared[j].dup_fd = r_fd;
                                g_shared[j].pgoffset =
                                    g_shared[i].pgoffset + (unmap_end - s) / 4096U;
                                placed = true;
                                break;
                            }
                        }
                        if (!placed) {
                            close(r_fd);
                        }
                    }
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
                    g_fmaps[i].end_gva = unmap_start;
                    int r_fd = (g_fmaps[i].dup_fd >= 0)
                        ? fcntl(g_fmaps[i].dup_fd, F_DUPFD_CLOEXEC, 3) : -1;
                    bool placed = false;
                    for (unsigned j = 0U; j < A32_VMFILE_SLOTS; j++) {
                        if (!g_fmaps[j].used) {
                            g_fmaps[j].used = true;
                            g_fmaps[j].dev = g_fmaps[i].dev;
                            g_fmaps[j].ino = g_fmaps[i].ino;
                            g_fmaps[j].start_gva = unmap_end;
                            g_fmaps[j].end_gva = e;
                            g_fmaps[j].file_offset =
                                g_fmaps[i].file_offset + (uint64_t)(unmap_end - s);
                            g_fmaps[j].dup_fd = r_fd;
                            placed = true;
                            break;
                        }
                    }
                    if (!placed && r_fd >= 0) {
                        close(r_fd);
                    }
                }
            }
        }
    }
    pthread_mutex_unlock(&g_vmfile_lock);
}

bool a32_vmfile_can_coalesce(uint32_t end_a, uint32_t start_b)
{
    if (end_a != start_b) {
        return false;
    }
    bool ok = true;
    pthread_mutex_lock(&g_vmfile_lock);
    const a32_vmfile_shared_t *sh_a = NULL;
    const a32_vmfile_shared_t *sh_b = NULL;
    const a32_vmfile_map_t *fm_a = NULL;
    const a32_vmfile_map_t *fm_b = NULL;
    for (unsigned i = 0U; i < A32_VMFILE_SLOTS; i++) {
        if (g_shared[i].used) {
            if (end_a > g_shared[i].start_gva && end_a <= g_shared[i].end_gva) {
                sh_a = &g_shared[i];
            }
            if (start_b >= g_shared[i].start_gva && start_b < g_shared[i].end_gva) {
                sh_b = &g_shared[i];
            }
        }
        if (g_fmaps[i].used) {
            if (end_a > g_fmaps[i].start_gva && end_a <= g_fmaps[i].end_gva) {
                fm_a = &g_fmaps[i];
            }
            if (start_b >= g_fmaps[i].start_gva && start_b < g_fmaps[i].end_gva) {
                fm_b = &g_fmaps[i];
            }
        }
    }
    if (sh_a != sh_b) {
        ok = false;
    }
    if (fm_a != fm_b) {
        if (fm_a == NULL || fm_b == NULL ||
            fm_a->dev != fm_b->dev || fm_a->ino != fm_b->ino ||
            fm_a->file_offset + (uint64_t)(end_a - fm_a->start_gva) !=
                fm_b->file_offset + (uint64_t)(start_b - fm_b->start_gva)) {
            ok = false;
        }
    }
    pthread_mutex_unlock(&g_vmfile_lock);
    return ok;
}
