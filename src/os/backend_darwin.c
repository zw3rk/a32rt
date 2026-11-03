/*
 * src/os/backend_darwin.c -- macOS host backend (Wave U3 A32-4, Backend 2)
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Standalone macOS arm64 Mach-O (zero Hypervisor.framework dependency):
 * libSystem mmap, per-thread MAP_JIT APRR W^X publication, pthread_kill
 * preemption for quiescence. Apple-only paths sit behind __APPLE__
 * guards with -ENOSYS fallbacks so this TU stays syntax-clean under
 * make check on Linux. Curated C11.
 */

#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
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

#if defined(__APPLE__)
#include <libkern/OSCacheControl.h>
#include <mach/vm_inherit.h>
#endif

a32_backend_kind_t a32_backend_kind(void)
{
#if defined(__APPLE__)
    return A32_BACKEND_DARWIN;
#else
    return A32_BACKEND_LINUX;
#endif
}

size_t a32_backend_page_size(void)
{
    long h = sysconf(_SC_PAGESIZE);
    if (h != 4096L && h != 16384L && h != 65536L) {
        return 16384U;
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
        p |= PROT_READ;
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
    if (replace && hint_addr != NULL) {
        /* A32-R4-2: install genuinely fresh anonymous backing instead
         * of patching the previous mapping in place. Zeroing in place
         * writes through a shared-file mapping into the file itself
         * and leaves the range attached to the old object; skipping
         * the zeroing for PROT_NONE leaves stale bytes that a later
         * mprotect re-exposes. A fresh MAP_FIXED anonymous mapping
         * detaches both cases. */
        size_t hp = a32_backend_page_size();
        uintptr_t start = (uintptr_t)hint_addr & ~(uintptr_t)(hp - 1U);
        uintptr_t end = ((uintptr_t)hint_addr + len + hp - 1U) &
            ~(uintptr_t)(hp - 1U);
        if (shared != 0 && hp > 4096U &&
            (((uintptr_t)hint_addr & (hp - 1U)) != 0U)) {
            return -EINVAL;
        }
        if (start == (uintptr_t)hint_addr && end == (uintptr_t)hint_addr + len) {
            res = mmap(hint_addr, len, host_prot(linux_prot),
                flags | MAP_FIXED, -1, 0);
            if (res == MAP_FAILED) {
                return -errno;
            }
#if defined(__APPLE__)
            if (minherit(hint_addr, len,
                    shared != 0 ? VM_INHERIT_SHARE : VM_INHERIT_COPY) != 0) {
                return -errno;
            }
#endif
            *out_host_addr = hint_addr;
            return 0;
        }
        /* Sub-page replacement (a 4 KiB guest range inside a larger
         * host page): replacing the whole host page would detach the
         * unaffected 4 KiB siblings from their backing, so snapshot
         * the host page, install fresh storage, restore the siblings
         * around the zeroed target, and keep the new pages private
         * (shared replacements are always host-page-aligned by the
         * gap rules in src/vm.c, so COPY is correct here). */
        {
            size_t hlen = end - start;
            uintptr_t off = (uintptr_t)hint_addr - start;
            uint8_t *saved;
            if (shared != 0) {
                return -EINVAL;
            }
            saved = (uint8_t *)malloc(hlen);
            if (saved == NULL) {
                return -ENOMEM;
            }
            if (mprotect((void *)start, hlen, PROT_READ | PROT_WRITE) != 0) {
                int merr = errno;
                free(saved);
                return -merr;
            }
            memcpy(saved, (void *)start, hlen);
            res = mmap((void *)start, hlen, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
            if (res == MAP_FAILED) {
                int merr = errno;
                free(saved);
                return -merr;
            }
            if (off > 0U) {
                memcpy((void *)start, saved, off);
            }
            if (off + len < hlen) {
                memcpy((void *)(start + off + len), saved + off + len,
                    hlen - off - len);
            }
            memset((void *)(start + off), 0, len);
            free(saved);
#if defined(__APPLE__)
            if (minherit((void *)start, hlen, VM_INHERIT_COPY) != 0) {
                return -errno;
            }
#endif
            /* Large-page hosts keep guest pages RW at host level (see
             * a32_backend_protect); per-4 KiB guest protection stays a
             * Soft-TLB matter. */
            *out_host_addr = hint_addr;
            return 0;
        }
    }
    if (replace) {
        flags |= MAP_FIXED;
    } else if (fixed) {
#if defined(MAP_FIXED_NOREPLACE)
        flags |= MAP_FIXED_NOREPLACE;
#endif
    }
    res = mmap(hint_addr, len, host_prot(linux_prot), flags, -1, 0);
    if (res == MAP_FAILED) {
        return -errno;
    }
    if (!replace && fixed && hint_addr != NULL && res != hint_addr) {
        munmap(res, len);
        return -EEXIST;
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
    if ((replace || fixed) && hint_addr != NULL) {
        size_t hp = a32_backend_page_size();
        if ((((uintptr_t)hint_addr & (hp - 1U)) != 0U) ||
            ((aligned_file_offset & (uint64_t)(hp - 1U)) != 0ULL)) {
            return -EINVAL;
        }
    }
    if (replace) {
        flags |= MAP_FIXED;
    } else if (fixed) {
#if defined(MAP_FIXED_NOREPLACE)
        flags |= MAP_FIXED_NOREPLACE;
#endif
    }
    res = mmap(hint_addr, len, host_prot(linux_prot), flags, host_fd,
        (off_t)aligned_file_offset);
    if (res == MAP_FAILED) {
        return -errno;
    }
    if (!replace && fixed && hint_addr != NULL && res != hint_addr) {
        munmap(res, len);
        return -EEXIST;
    }
#if defined(__APPLE__)
    if (shared == 0) {
        size_t hp = a32_backend_page_size();
        uintptr_t start = (uintptr_t)res & ~(uintptr_t)(hp - 1U);
        uintptr_t end = ((uintptr_t)res + len + hp - 1U) &
            ~(uintptr_t)(hp - 1U);
        (void)minherit((void *)start, end - start, VM_INHERIT_COPY);
    }
#endif
    *out_host_addr = res;
    return 0;
}

int a32_backend_protect(void *host_addr, size_t len, int linux_prot)
{
    size_t hp;
    uintptr_t start;
    uintptr_t end;
    if (host_addr == NULL || len == 0U) {
        return -EINVAL;
    }
    hp = a32_backend_page_size();
    start = (uintptr_t)host_addr & ~(uintptr_t)(hp - 1U);
    end = ((uintptr_t)host_addr + len + hp - 1U) & ~(uintptr_t)(hp - 1U);
    if (hp > 4096U) {
        /* On 16 KiB Darwin hosts, 4 KiB guest page permissions are enforced
         * by the Soft-TLB (vm32); keep shared 16 KiB host pages RW so a
         * sub-page mprotect never faults sibling 4 KiB guest pages. */
        (void)linux_prot;
        if (mprotect((void *)start, end - start,
                PROT_READ | PROT_WRITE) != 0) {
            return -errno;
        }
        return 0;
    }
    if (mprotect((void *)start, end - start, host_prot(linux_prot)) != 0) {
        return -errno;
    }
    return 0;
}

int a32_backend_unmap(void *host_addr, size_t len)
{
    size_t hp;
    if (host_addr == NULL || len == 0U) {
        return -EINVAL;
    }
    hp = a32_backend_page_size();
    if (hp > 4096U &&
        (((uintptr_t)host_addr & (hp - 1U)) != 0U || (len & (hp - 1U)) != 0U)) {
        return 0;
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
#if defined(__APPLE__)
    res = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_JIT, -1, 0);
#else
    res = mmap(NULL, size, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
    if (res == MAP_FAILED) {
        return NULL;
    }
    return res;
}

void a32_backend_jit_thread_init(void)
{
#if defined(__APPLE__)
    pthread_jit_write_protect_np(1);
#endif
}

void a32_backend_jit_begin_write(void *buf, size_t size)
{
    (void)size;
#if defined(__APPLE__)
    if (buf == NULL) {
        return;
    }
    pthread_jit_write_protect_np(0);
#else
    (void)buf;
#endif
}

int a32_backend_jit_seal_rx(void *buf, size_t size)
{
    if (buf == NULL || size == 0U) {
        return -EINVAL;
    }
#if defined(__APPLE__)
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(buf, size);
#else
    if (mprotect(buf, size, PROT_READ | PROT_EXEC) != 0) {
        return -errno;
    }
    __builtin___clear_cache((char *)buf, (char *)buf + size);
#endif
    return 0;
}

void a32_backend_jit_free(void *buf, size_t size)
{
    if (buf == NULL || size == 0U) {
        return;
    }
    munmap(buf, size);
}

int a32_backend_worker_sync_init(a32_worker_sync_t *ws)
{
    int fds[2];
    if (ws == NULL) {
        return -EINVAL;
    }
    memset(ws, 0, sizeof(*ws));
    ws->wake_pipe_rd = -1;
    ws->wake_pipe_wr = -1;
    if (pipe(fds) != 0) {
        return -errno;
    }
    if (fcntl(fds[0], F_SETFL, O_NONBLOCK) != 0 ||
        fcntl(fds[1], F_SETFL, O_NONBLOCK) != 0) {
        int e = errno;
        close(fds[0]);
        close(fds[1]);
        return -e;
    }
    ws->wake_pipe_rd = fds[0];
    ws->wake_pipe_wr = fds[1];
    return 0;
}

void a32_backend_worker_sync_destroy(a32_worker_sync_t *ws)
{
    if (ws == NULL) {
        return;
    }
    if (ws->wake_pipe_rd >= 0) {
        close(ws->wake_pipe_rd);
    }
    if (ws->wake_pipe_wr >= 0) {
        close(ws->wake_pipe_wr);
    }
    ws->wake_pipe_rd = -1;
    ws->wake_pipe_wr = -1;
}

void a32_backend_worker_notify_signal(a32_worker_sync_t *ws)
{
    const char b = 0;
    if (ws == NULL || ws->wake_pipe_wr < 0) {
        return;
    }
    atomic_fetch_add(&ws->sig_seq, 1U);
    (void)write(ws->wake_pipe_wr, &b, 1U);
}

int a32_backend_preempt_worker(uint64_t host_tid)
{
#if defined(__APPLE__)
    if (host_tid == 0U) {
        return -EINVAL;
    }
    if (pthread_kill((pthread_t)host_tid, SIGUSR2) != 0) {
        return -errno;
    }
    return 0;
#else
    (void)host_tid;
    return -ENOSYS;
#endif
}
