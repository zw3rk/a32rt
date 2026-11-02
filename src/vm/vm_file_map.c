/*
 * src/vm/vm_file_map.c -- Host-mapping install + populate for file backing
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Split out of src/vm/vm_file.c (A32-R3): the host-side install path
 * (staged A2 anonymous reservation, shared-anon memfd, native file mmap)
 * and the MAP_PRIVATE populate wrapper. Uses only public vm32/backend
 * APIs plus the published record/retire entry points; the tracking
 * tables themselves stay in vm_file.c. Curated C11, no inline asm.
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
            a32_vmfile_unmap_shared(sgva, sgva + (uint32_t)len);
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
