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

#include "vm/vm32.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
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
    uint64_t resize_seq;
    uint64_t code_generation;
} a32_vmfile_inode_t;

static a32_vmfile_inode_t g_inodes[A32_VMFILE_SLOTS];
static pthread_mutex_t g_vmfile_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_code_generation = 1U;

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
    g_inodes[free_slot].resize_seq = 0U;
    g_inodes[free_slot].code_generation = g_code_generation;
    return &g_inodes[free_slot];
}

int a32_vmfile_populate_private(void *hva, int host_fd, uint64_t file_offset,
    size_t len)
{
    struct stat st;
    size_t done = 0U;
    if (hva == NULL || host_fd < 0 || len == 0U) {
        return -EINVAL;
    }
    if (fstat(host_fd, &st) == 0) {
        pthread_mutex_lock(&g_vmfile_lock);
        a32_vmfile_inode_t *e =
            inode_intern_locked((uint64_t)st.st_dev, (uint64_t)st.st_ino);
        if (e != NULL) {
            uint64_t end = file_offset + (uint64_t)len;
            if (end > e->file_size) {
                e->file_size = end;
            }
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
            memset((char *)hva + done, 0, len - done);
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

int a32_vmfile_on_truncate(uint64_t new_size)
{
    uint32_t cut = (uint32_t)(((new_size + 0xFFFU) & ~0xFFFU) & 0xFFFFFFFFU);
    unsigned i;
    pthread_mutex_lock(&g_vmfile_lock);
    g_code_generation++;
    for (i = 0U; i < A32_VMFILE_SLOTS; i++) {
        if (!g_inodes[i].used) {
            continue;
        }
        g_inodes[i].resize_seq++;
        g_inodes[i].code_generation = g_code_generation;
        if ((uint64_t)cut < g_inodes[i].file_size) {
            /* Discard wholly-removed MAP_PRIVATE tails so a later regrow
             * observes clean file bytes, not stale dirty pages. */
            a32_vm32_tlb_invalidate_range(cut, 0U);
            g_inodes[i].file_size = (uint64_t)cut;
        }
    }
    pthread_mutex_unlock(&g_vmfile_lock);
    return 0;
}

int a32_vmfile_hook_write(uint64_t end_offset)
{
    unsigned i;
    pthread_mutex_lock(&g_vmfile_lock);
    for (i = 0U; i < A32_VMFILE_SLOTS; i++) {
        if (!g_inodes[i].used) {
            continue;
        }
        if (end_offset > g_inodes[i].file_size) {
            g_inodes[i].file_size = end_offset;
            g_inodes[i].resize_seq++;
        }
    }
    pthread_mutex_unlock(&g_vmfile_lock);
    return 0;
}
