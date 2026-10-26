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

int a32_aot_site_save(const char *dir, uint32_t guest_pc_base,
    const a32_vm32_mem_site_t *sites, uint32_t count)
{
    char path[512];
    uint8_t *buf = NULL;
    size_t len = 0U;
    int fd = -1;
    size_t done = 0U;
    int rc;
    if (dir == NULL || dir[0] == '\0') {
        return -EINVAL;
    }
    rc = a32_aot_site_encode(sites, count, &buf, &len);
    if (rc != 0) {
        return rc;
    }
    site_path(path, sizeof(path), dir, guest_pc_base);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
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
    free(buf);
    return rc;
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
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -errno;
    }
    if (fstat(fd, &st) != 0 || st.st_size <= 0 ||
        (uint64_t)st.st_size > 16U * 1024U * 1024U) {
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
