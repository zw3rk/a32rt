/*
 * src/dbt/aot/aot_sites.c -- AOT memory-site table codec (Wave U3 A32-4)
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Relative a32_vm32_mem_site_t records (24 bytes each) with a magic,
 * version, count, and FNV-1a checksum envelope. All multi-byte header
 * fields are little-endian on disk; hosts are LE-only (see vm32.h).
 * Curated C11, no inline asm.
 */

#include "dbt/aot/aot.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define A32_AOT_SITE_HDR_LEN 16U

static void put_u32le(uint8_t *p, uint32_t v)
{
    p[0U] = (uint8_t)v;
    p[1U] = (uint8_t)(v >> 8U);
    p[2U] = (uint8_t)(v >> 16U);
    p[3U] = (uint8_t)(v >> 24U);
}

static uint32_t get_u32le(const uint8_t *p)
{
    return (uint32_t)p[0U] | ((uint32_t)p[1U] << 8U) |
        ((uint32_t)p[2U] << 16U) | ((uint32_t)p[3U] << 24U);
}

static uint32_t fnv1a(const uint8_t *p, size_t len)
{
    uint32_t h = 2166136261U;
    size_t i;
    for (i = 0U; i < len; i++) {
        h ^= p[i];
        h *= 16777619U;
    }
    return h;
}

int a32_aot_site_encode(const a32_vm32_mem_site_t *sites, uint32_t count,
    uint8_t **out_buf, size_t *out_len)
{
    size_t len;
    uint8_t *buf;
    if (sites == NULL || count == 0U || out_buf == NULL || out_len == NULL) {
        return -EINVAL;
    }
    len = (size_t)A32_AOT_SITE_HDR_LEN + (size_t)count * 24U;
    if (len < (size_t)A32_AOT_SITE_HDR_LEN ||
        (len - (size_t)A32_AOT_SITE_HDR_LEN) / 24U != (size_t)count) {
        return -E2BIG;
    }
    buf = (uint8_t *)malloc(len);
    if (buf == NULL) {
        return -ENOMEM;
    }
    put_u32le(buf, A32_AOT_SITE_MAGIC);
    put_u32le(buf + 4U, A32_AOT_SITE_VERSION);
    put_u32le(buf + 8U, count);
    memcpy(buf + 12U, sites, (size_t)count * 24U);
    put_u32le(buf + 12U + (size_t)count * 24U,
        fnv1a(buf + 12U, (size_t)count * 24U));
    *out_buf = buf;
    *out_len = len;
    return 0;
}

int a32_aot_site_decode(const uint8_t *buf, size_t len,
    a32_vm32_mem_site_t *sites, uint32_t cap, uint32_t *out_count)
{
    uint32_t count;
    size_t want;
    if (out_count != NULL) {
        *out_count = 0U;
    }
    if (buf == NULL || sites == NULL || cap == 0U || out_count == NULL) {
        return -EINVAL;
    }
    if (len < (size_t)A32_AOT_SITE_HDR_LEN + 4U) {
        return -EBADMSG;
    }
    if (get_u32le(buf) != A32_AOT_SITE_MAGIC) {
        return -EBADMSG;
    }
    if (get_u32le(buf + 4U) != A32_AOT_SITE_VERSION) {
        return -EBADMSG;
    }
    count = get_u32le(buf + 8U);
    if (count == 0U || count > cap) {
        return -EBADMSG;
    }
    want = (size_t)A32_AOT_SITE_HDR_LEN + (size_t)count * 24U;
    if (len < want) {
        return -EBADMSG;
    }
    if (get_u32le(buf + 12U + (size_t)count * 24U) !=
        fnv1a(buf + 12U, (size_t)count * 24U)) {
        return -EBADMSG;
    }
    memcpy(sites, buf + 12U, (size_t)count * 24U);
    *out_count = count;
    return 0;
}

static void site_path(char *dst, size_t dst_len, const char *dir,
    uint32_t guest_pc_base)
{
    snprintf(dst, dst_len, "%s/a3ts-%08x.bin", dir, guest_pc_base);
}

/* Monotonic temp-name counter for atomic site saves (A32-IOS-D3). */
static _Atomic unsigned g_site_save_seq = 0U;

int a32_aot_site_save(const char *dir, uint32_t guest_pc_base,
    const a32_vm32_mem_site_t *sites, uint32_t count)
{
    char path[512];
    char tmp[576];
    uint8_t *buf = NULL;
    size_t len = 0U;
    int fd = -1;
    size_t done = 0U;
    int rc;
    int n;
    if (dir == NULL || dir[0] == '\0') {
        return -EINVAL;
    }
    rc = a32_aot_site_encode(sites, count, &buf, &len);
    if (rc != 0) {
        return rc;
    }
    site_path(path, sizeof(path), dir, guest_pc_base);
    /* Unique temp name per save; O_EXCL|O_NOFOLLOW refuses symlinks and
     * pre-existing files instead of following them. */
    n = snprintf(tmp, sizeof(tmp), "%s.tmp.%d.%u", path, (int)getpid(),
        atomic_fetch_add_explicit(&g_site_save_seq, 1U, memory_order_relaxed));
    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        free(buf);
        return -ENAMETOOLONG;
    }
    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        rc = -errno;
        free(buf);
        return rc;
    }
    while (done < len) {
        ssize_t w = write(fd, buf + done, len - done);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            rc = -errno;
            break;
        }
        done += (size_t)w;
    }
    close(fd);
    if (rc == 0 && rename(tmp, path) != 0) {
        rc = -errno;
    }
    if (rc != 0) {
        (void)unlink(tmp);
    }
    free(buf);
    return rc;
}

/* ---- Per-block registry (emitters -> disk cache bridge) ---- */

typedef struct {
    bool used;
    uint32_t base;
    uint32_t count;
    a32_vm32_mem_site_t sites[A32_AOT_SITE_PER_BLOCK];
} a32_aot_site_slot_t;

/* Leaf guard for g_site_slots: track runs unlocked during translation
 * while fetch drains on save/load paths, so concurrent translations must
 * not corrupt each other's staged records (A32-IOS-D4). fetch uses
 * trylock and reports -EAGAIN when busy; neither save nor load treats a
 * missed fetch as fatal. */
static pthread_mutex_t g_site_mu = PTHREAD_MUTEX_INITIALIZER;
static a32_aot_site_slot_t g_site_slots[A32_AOT_SITE_TRACK_MAX];

int a32_aot_site_track(uint32_t guest_pc_base,
    const a32_vm32_mem_site_t *sites, uint32_t count)
{
    uint32_t i;
    uint32_t free_slot = A32_AOT_SITE_TRACK_MAX;
    int rc = -ENOSPC;
    if (sites == NULL || count == 0U || count > A32_AOT_SITE_PER_BLOCK) {
        return -EINVAL;
    }
    pthread_mutex_lock(&g_site_mu);
    for (i = 0U; i < A32_AOT_SITE_TRACK_MAX; i++) {
        if (g_site_slots[i].used && g_site_slots[i].base == guest_pc_base) {
            memcpy(g_site_slots[i].sites, sites,
                (size_t)count * sizeof(sites[0]));
            g_site_slots[i].count = count;
            rc = 0;
            goto out;
        }
        if (!g_site_slots[i].used && free_slot == A32_AOT_SITE_TRACK_MAX) {
            free_slot = i;
        }
    }
    if (free_slot != A32_AOT_SITE_TRACK_MAX) {
        g_site_slots[free_slot].used = true;
        g_site_slots[free_slot].base = guest_pc_base;
        g_site_slots[free_slot].count = count;
        memcpy(g_site_slots[free_slot].sites, sites,
            (size_t)count * sizeof(sites[0]));
        rc = 0;
    }
out:
    pthread_mutex_unlock(&g_site_mu);
    return rc;
}

int a32_aot_site_fetch(uint32_t guest_pc_base,
    a32_vm32_mem_site_t *sites, uint32_t cap, uint32_t *out_count)
{
    uint32_t i;
    int rc = -ENOENT;
    if (out_count != NULL) {
        *out_count = 0U;
    }
    if (sites == NULL || cap == 0U || out_count == NULL) {
        return -EINVAL;
    }
    if (pthread_mutex_trylock(&g_site_mu) != 0) {
        return -EAGAIN;
    }
    for (i = 0U; i < A32_AOT_SITE_TRACK_MAX; i++) {
        if (g_site_slots[i].used && g_site_slots[i].base == guest_pc_base) {
            if (g_site_slots[i].count > cap) {
                rc = -E2BIG;
                break;
            }
            memcpy(sites, g_site_slots[i].sites,
                (size_t)g_site_slots[i].count * sizeof(sites[0]));
            *out_count = g_site_slots[i].count;
            rc = 0;
            break;
        }
    }
    pthread_mutex_unlock(&g_site_mu);
    return rc;
}

void a32_aot_site_clear(void)
{
    pthread_mutex_lock(&g_site_mu);
    memset(g_site_slots, 0, sizeof(g_site_slots));
    pthread_mutex_unlock(&g_site_mu);
}

int a32_aot_site_load(const char *dir, uint32_t guest_pc_base,
    a32_vm32_mem_site_t *sites, uint32_t cap, uint32_t *out_count)
{
    char path[512];
    struct stat st;
    uint8_t *buf = NULL;
    int fd = -1;
    size_t done = 0U;
    size_t len;
    int rc;
    if (out_count != NULL) {
        *out_count = 0U;
    }
    if (dir == NULL || dir[0] == '\0' || sites == NULL || cap == 0U ||
        out_count == NULL) {
        return -EINVAL;
    }
    site_path(path, sizeof(path), dir, guest_pc_base);
    fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        return -errno;
    }
    /* Refuse symlinks (O_NOFOLLOW above), non-regular, foreign-owned, or
     * group/world-writable payloads before trusting a single byte. */
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_uid != geteuid() || (st.st_mode & 0022) != 0 ||
        st.st_size <= 0 || (uint64_t)st.st_size > 16U * 1024U * 1024U) {
        rc = -EBADMSG;
        goto out;
    }
    len = (size_t)st.st_size;
    buf = (uint8_t *)malloc(len);
    if (buf == NULL) {
        rc = -ENOMEM;
        goto out;
    }
    while (done < len) {
        ssize_t r = read(fd, buf + done, len - done);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            rc = -errno;
            goto out;
        }
        if (r == 0) {
            break;
        }
        done += (size_t)r;
    }
    if (done != len) {
        rc = -EBADMSG;
        goto out;
    }
    rc = a32_aot_site_decode(buf, len, sites, cap, out_count);
out:
    if (fd >= 0) {
        close(fd);
    }
    free(buf);
    return rc;
}
