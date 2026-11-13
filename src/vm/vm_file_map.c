/*
 * src/vm/vm_file_map.c -- Host-mapping install + populate for file backing
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Split out of src/vm/vm_file.c (A32-R3): the host-side install path
 * (staged A2 anonymous reservation, shared-anon memfd, native file mmap),
 * the MAP_PRIVATE populate wrapper, and (A32-R4) the private-lookup,
 * span/coalesce, and fork-reset helpers. The tracking tables live in
 * vm_file.c and are shared via src/vm/vm_priv.h. Curated C11, no
 * inline asm.
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

/* File-backed prefix length of a fresh mapping, page-aligned and clamped
 * to len: bytes below round_up(file_size) come from the file, the tail is
 * wholly beyond EOF. Shared with vm_file.c's EOF enforcement. */
uint32_t a32_vmfile_eof_keep(uint32_t len, uint64_t file_offset,
    uint64_t file_size)
{
    uint64_t eof_round = (file_size + 0xFFFULL) & ~0xFFFULL;
    uint64_t keep = 0ULL;
    if (eof_round > file_offset) {
        keep = eof_round - file_offset;
        if (keep > (uint64_t)len) {
            keep = (uint64_t)len;
        }
        keep &= ~0xFFFULL;
    }
    return (uint32_t)keep;
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
        /* Commit the final protection only over the file-backed prefix so
         * the beyond-EOF cut installed above survives publication. */
        struct stat st;
        uint32_t ulen = (len > 0xFFFFFFFFULL) ? 0xFFFFFFFFU : (uint32_t)len;
        size_t keep = len;
        if (fstat(host_fd, &st) == 0 &&
            (uint64_t)st.st_size < file_offset + (uint64_t)ulen) {
            keep = (size_t)a32_vmfile_eof_keep(ulen, file_offset,
                (uint64_t)st.st_size);
        }
        if (keep > 0U) {
            rc = a32_backend_protect(hva, keep, final_prot);
        }
    }
    if (rc != 0) {
        void *out = NULL;
        (void)a32_backend_map_anon(hva, len, 0, 1, 1, 0, &out);
        /* The host range no longer carries the file: retire the tracking
         * just recorded so later lookups cannot mistake it for backing. */
        uintptr_t arena = a32_vm32_arena_base();
        if (arena != 0U && (uintptr_t)hva >= arena &&
            ((uintptr_t)hva - arena) + len <= A32_VM32_WINDOW_SIZE &&
            len <= 0xFFFFFFFFULL) {
            uint32_t sgva = (uint32_t)((uintptr_t)hva - arena);
            /* The retired range is exactly the record just created, so
             * no middle split can demand a slot: infallible here. */
            (void)a32_vmfile_unmap_shared(sgva, sgva + (uint32_t)len);
        }
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
                ((uintptr_t)res - arena) + len <= A32_VM32_WINDOW_SIZE &&
                len <= 0xFFFFFFFFULL) {
                uint32_t sgva = (uint32_t)((uintptr_t)res - arena);
                map_rc = a32_vmfile_record_shared(sgva,
                    sgva + (uint32_t)len, sh_fd, 0U);
                if (map_rc != 0) {
                    void *out = NULL;
                    (void)a32_backend_map_anon(res, len, 0, 1, 1, 0, &out);
                }
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

/* Fork-child reset (A32-R3-4): the surviving thread must not inherit a
 * mutex held by a vanished thread. */
void a32_vmfile_fork_child_reset(void)
{
    (void)pthread_mutex_destroy(&g_vmfile_lock);
    (void)pthread_mutex_init(&g_vmfile_lock, NULL);
}

/* Spanning private lookup for mremap (A32-R4-1): resolves backing
 * across contiguous compatible records. After an in-place mremap
 * extension the enlarged range is covered by two adjacent records (the
 * original plus the extension); a single-record lookup would report
 * "absent" and the caller would silently fall back to anonymous
 * backing, losing file identity on the next grow or move. Every
 * visited record must share (dev, ino) with the first and carry a
 * contiguous file offset; the returned descriptor is an owned duplicate
 * of the first record's fd. Returns 1 when [start_gva, end_gva) is
 * fully covered, 0 when no private backing exists there, or a negative
 * errno when a descriptor cannot be duplicated (the caller must
 * propagate, never treat as anonymous). */
int a32_vmfile_lookup_private_span(uint32_t start_gva, uint32_t end_gva,
    uint64_t *out_dev, uint64_t *out_ino, uint64_t *out_file_offset,
    int *out_dup_fd)
{
    uint64_t dev = 0ULL;
    uint64_t ino = 0ULL;
    uint64_t first_off = 0ULL;
    uint32_t cur = start_gva;
    bool have_first = false;
    int dup_fd = -1;
    int rc = 0;
    if (end_gva <= start_gva) {
        return -EINVAL;
    }
    pthread_mutex_lock(&g_vmfile_lock);
    while (cur < end_gva) {
        bool advanced = false;
        for (unsigned i = 0U; i < A32_VMFILE_SLOTS; i++) {
            if (!g_fmaps[i].used || g_fmaps[i].dup_fd < 0 ||
                cur < g_fmaps[i].start_gva || cur >= g_fmaps[i].end_gva) {
                continue;
            }
            if (!have_first) {
                int fd = fcntl(g_fmaps[i].dup_fd, F_DUPFD_CLOEXEC, 3);
                if (fd < 0) {
                    rc = -errno;
                    break;
                }
                dev = g_fmaps[i].dev;
                ino = g_fmaps[i].ino;
                dup_fd = fd;
                first_off = g_fmaps[i].file_offset +
                    (uint64_t)(cur - g_fmaps[i].start_gva);
                have_first = true;
            } else {
                uint64_t expect = first_off + (uint64_t)(cur - start_gva);
                uint64_t got = g_fmaps[i].file_offset +
                    (uint64_t)(cur - g_fmaps[i].start_gva);
                if (g_fmaps[i].dev != dev || g_fmaps[i].ino != ino ||
                    got != expect) {
                    break;
                }
            }
            if (g_fmaps[i].end_gva <= cur) {
                break;
            }
            cur = g_fmaps[i].end_gva;
            advanced = true;
            break;
        }
        if (rc != 0 || !advanced) {
            break;
        }
    }
    pthread_mutex_unlock(&g_vmfile_lock);
    if (rc != 0) {
        if (dup_fd >= 0) {
            close(dup_fd);
        }
        if (out_dup_fd != NULL) {
            *out_dup_fd = -1;
        }
        return rc;
    }
    if (!have_first || cur < end_gva) {
        /* Absent (or only partially covered): no private backing. Any
         * duplicate taken above belongs to a prefix that cannot serve
         * the whole span, so release it. */
        if (dup_fd >= 0) {
            close(dup_fd);
        }
        if (out_dup_fd != NULL) {
            *out_dup_fd = -1;
        }
        return 0;
    }
    if (out_dev != NULL) {
        *out_dev = dev;
    }
    if (out_ino != NULL) {
        *out_ino = ino;
    }
    if (out_file_offset != NULL) {
        *out_file_offset = first_off;
    }
    if (out_dup_fd != NULL) {
        *out_dup_fd = dup_fd;
    } else {
        close(dup_fd);
    }
    return 1;
}

/* Merge contiguous compatible private records inside [start_gva,
 * end_gva) back into one entry (A32-R4-1): repeated in-place mremap
 * growth otherwise strands one record per extension, and the next span
 * lookup degrades (or a future table-exhaustion path fails). Records
 * merge only when adjacent with matching (dev, ino) and a contiguous
 * file offset; the survivor keeps the lowest slot's descriptor.
 * Returns 0 on success (including nothing to merge). */
int a32_vmfile_coalesce_private(uint32_t start_gva, uint32_t end_gva)
{
    bool merged;
    if (end_gva <= start_gva) {
        return -EINVAL;
    }
    pthread_mutex_lock(&g_vmfile_lock);
    do {
        merged = false;
        for (unsigned i = 0U; i < A32_VMFILE_SLOTS && !merged; i++) {
            if (!g_fmaps[i].used || g_fmaps[i].dup_fd < 0 ||
                g_fmaps[i].start_gva < start_gva ||
                g_fmaps[i].end_gva > end_gva) {
                continue;
            }
            for (unsigned j = 0U; j < A32_VMFILE_SLOTS; j++) {
                if (i == j || !g_fmaps[j].used ||
                    g_fmaps[j].start_gva != g_fmaps[i].end_gva ||
                    g_fmaps[j].start_gva < start_gva ||
                    g_fmaps[j].end_gva > end_gva) {
                    continue;
                }
                if (g_fmaps[j].dev != g_fmaps[i].dev ||
                    g_fmaps[j].ino != g_fmaps[i].ino ||
                    g_fmaps[j].file_offset != g_fmaps[i].file_offset +
                        (uint64_t)(g_fmaps[i].end_gva - g_fmaps[i].start_gva)) {
                    continue;
                }
                g_fmaps[i].end_gva = g_fmaps[j].end_gva;
                if (g_fmaps[j].dup_fd >= 0) {
                    close(g_fmaps[j].dup_fd);
                }
                g_fmaps[j].used = false;
                merged = true;
                break;
            }
        }
    } while (merged);
    pthread_mutex_unlock(&g_vmfile_lock);
    return 0;
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
