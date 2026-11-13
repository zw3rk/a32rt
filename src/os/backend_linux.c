/*
 * src/os/backend_linux.c -- Linux host backend (Wave U3 A32-4, Backend 1)
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Static-PIE aarch64-linux (native Linux or hyper-linux): raw
 * mmap/mprotect/munmap, plain RW->RX flip with I-cache maintenance done
 * by the caller, tgkill preemption for quiescence. Linux-only paths sit
 * behind __linux__ guards with -ENOSYS fallbacks so this TU stays
 * syntax-clean under make check on macOS. Curated C11.
 */

#define _GNU_SOURCE

#include "os/backend.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/syscall.h>
#endif

a32_backend_kind_t a32_backend_kind(void)
{
#if defined(__linux__)
    return A32_BACKEND_LINUX;
#else
    return A32_BACKEND_DARWIN;
#endif
}

size_t a32_backend_page_size(void)
{
    long h = sysconf(_SC_PAGESIZE);
    if (h != 4096L && h != 16384L && h != 65536L) {
        return 4096U;
    }
    return (size_t)h;
}

static int host_prot(int linux_prot)
{
    int p = 0;
    if ((linux_prot & 0x1) != 0) {
        p |= PROT_READ;
    }
    if ((linux_prot & 0x2) != 0) {
        p |= PROT_WRITE;
    }
    if ((linux_prot & 0x4) != 0) {
        p |= PROT_EXEC;
    }
    return p;
}

int a32_backend_map_anon(void *hint_addr, size_t len, int linux_prot,
    int fixed, int replace, int shared, void **out_host_addr)
{
    int flags = (shared ? MAP_SHARED : MAP_PRIVATE) | MAP_ANONYMOUS;
    void *res;
    if (len == 0U || out_host_addr == NULL) {
        return -EINVAL;
    }
    if (replace) {
        flags |= MAP_FIXED;
    } else if (fixed) {
#if defined(MAP_FIXED_NOREPLACE)
        flags |= MAP_FIXED_NOREPLACE;
#else
        flags |= MAP_FIXED;
#endif
    }
    res = mmap(hint_addr, len, host_prot(linux_prot), flags, -1, 0);
    if (res == MAP_FAILED) {
        return -errno;
    }
    *out_host_addr = res;
    return 0;
}

int a32_backend_map_file(void *hint_addr, size_t len, int linux_prot,
    int shared, int fixed, int replace, int host_fd,
    uint64_t aligned_file_offset, void **out_host_addr)
{
    int flags = shared ? MAP_SHARED : MAP_PRIVATE;
    void *res;
    if (len == 0U || host_fd < 0 || out_host_addr == NULL) {
        return -EINVAL;
    }
    if (replace) {
        flags |= MAP_FIXED;
    } else if (fixed) {
#if defined(MAP_FIXED_NOREPLACE)
        flags |= MAP_FIXED_NOREPLACE;
#else
        flags |= MAP_FIXED;
#endif
    }
    res = mmap(hint_addr, len, host_prot(linux_prot), flags, host_fd,
        (off_t)aligned_file_offset);
    if (res == MAP_FAILED) {
        return -errno;
    }
    *out_host_addr = res;
    return 0;
}

int a32_backend_protect(void *host_addr, size_t len, int linux_prot)
{
    if (host_addr == NULL || len == 0U) {
        return -EINVAL;
    }
    if (mprotect(host_addr, len, host_prot(linux_prot)) != 0) {
        return -errno;
    }
    return 0;
}

int a32_backend_unmap(void *host_addr, size_t len)
{
    if (host_addr == NULL || len == 0U) {
        return -EINVAL;
    }
    if (munmap(host_addr, len) != 0) {
        return -errno;
    }
    return 0;
}

int a32_backend_msync(void *host_addr, size_t len)
{
    if (host_addr == NULL || len == 0U) {
        return -EINVAL;
    }
    if (msync(host_addr, len, MS_SYNC) != 0) {
        return -errno;
    }
    return 0;
}

void *a32_backend_jit_alloc(size_t size)
{
    void *res;
    if (size == 0U) {
        return NULL;
    }
    res = mmap(NULL, size, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (res == MAP_FAILED) {
        return NULL;
    }
    return res;
}

void a32_backend_jit_thread_init(void)
{
}

void a32_backend_jit_begin_write(void *buf, size_t size)
{
    (void)buf;
    (void)size;
}

int a32_backend_jit_seal_rx(void *buf, size_t size)
{
    if (buf == NULL || size == 0U) {
        return -EINVAL;
    }
    if (mprotect(buf, size, PROT_READ | PROT_EXEC) != 0) {
        return -errno;
    }
    __builtin___clear_cache((char *)buf, (char *)buf + size);
    return 0;
}

void a32_backend_jit_free(void *buf, size_t size)
{
    if (buf == NULL || size == 0U) {
        return;
    }
    munmap(buf, size);
}

/* Worker wake-pipe helpers live in src/os/backend_sync.c (A32-R13-11). */
