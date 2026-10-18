/*
 * src/aot.c — Ahead-Of-Time (AOT) pre-translator and disk cache (M2)
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#define _GNU_SOURCE
#include "a32rt.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define A32_AOT_MAGIC      0x41333243U /* "A32C" */
#define A32_AOT_VERSION    5U
#define A32_MAX_AOT_SCAN   4096U
#define A32_AOT_MAX_BLOCKS 16384U
#define A32_AOT_MAX_WORDS  2048U
#define A32_AOT_MAX_PC_MAP 256U

typedef struct a32_aot_header {
    uint32_t magic;
    uint32_t version;
    uint32_t num_blocks;
    uint32_t reserved;
} a32_aot_header_t;

typedef struct a32_aot_block_header {
    uint32_t guest_pc;
    uint32_t guest_end_pc;
    uint32_t is_thumb;
    uint32_t word_count;
    uint32_t pc_map_count;
} a32_aot_block_header_t;

static uint64_t fnv1a_64_step(uint64_t hash, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0U; i < len; i++) {
        hash ^= (uint64_t)p[i];
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

static uint64_t compute_elf_hash(const a32_ctx_t *ctx, const char *elf_path)
{
    if (elf_path == NULL) {
        return 0ULL;
    }
    uint64_t h = 0xcbf29ce484222325ULL;
    if (ctx != NULL) {
        h = fnv1a_64_step(h, &ctx->elf_base, sizeof(ctx->elf_base));
        h = fnv1a_64_step(h, &ctx->interp_base, sizeof(ctx->interp_base));
        h = fnv1a_64_step(h, &ctx->elf_entry, sizeof(ctx->elf_entry));
        if (ctx->sysroot != NULL && ctx->sysroot[0] != '\0') {
            h = fnv1a_64_step(h, ctx->sysroot, strlen(ctx->sysroot));
        }
    }
    h = fnv1a_64_step(h, elf_path, strlen(elf_path));
    char resolved_path[PATH_MAX];
    const char *host_path = elf_path;
    if (ctx != NULL &&
        a32_resolve_guest_path(ctx, elf_path, resolved_path, sizeof(resolved_path)) == 0) {
        host_path = resolved_path;
    }
    struct stat st;
    if (stat(host_path, &st) != 0) {
        if (host_path == elf_path || stat(elf_path, &st) != 0) {
            return 0ULL;
        }
        host_path = elf_path;
    }
    uint64_t sz  = (uint64_t)st.st_size;
    uint64_t mt  = (uint64_t)st.st_mtime;
    uint64_t mtn = (uint64_t)st.st_mtim.tv_nsec;
    h = fnv1a_64_step(h, &sz, sizeof(sz));
    h = fnv1a_64_step(h, &mt, sizeof(mt));
    h = fnv1a_64_step(h, &mtn, sizeof(mtn));

    FILE *f = fopen(host_path, "rb");
    if (f == NULL && host_path != elf_path) {
        f = fopen(elf_path, "rb");
    }
    if (f == NULL) {
        return 0ULL;
    }
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1U, sizeof(buf), f)) > 0U) {
        h = fnv1a_64_step(h, buf, n);
    }
    bool read_err = (ferror(f) != 0);
    fclose(f);
    if (read_err) {
        return 0ULL;
    }
    return (h != 0ULL) ? h : 1ULL;
}

static int format_cache_path(const a32_ctx_t *ctx, const char *prefix, uint64_t hash,
                             char *out, size_t out_sz, bool create_dir)
{
    if (out == NULL || out_sz == 0U || hash == 0ULL || prefix == NULL) {
        return -1;
    }
    const char *dir = (ctx != NULL) ? ctx->cache_dir : NULL;
    if (dir == NULL || dir[0] == '\0') {
        dir = getenv("A32RT_CACHE");
    }
    if (dir == NULL || dir[0] == '\0') {
        dir = getenv("XDG_CACHE_HOME");
    }
    int n;
    if (dir != NULL && dir[0] != '\0') {
        if (create_dir) {
            (void)mkdir(dir, 0755);
        }
        n = snprintf(out, out_sz, "%s/%s-%016llx.bin", dir, prefix, (unsigned long long)hash);
    } else {
        n = snprintf(out, out_sz, "/tmp/%s-%016llx.bin", prefix, (unsigned long long)hash);
    }
    if (n < 0 || (size_t)n >= out_sz) {
        out[0] = '\0';
        return -1;
    }
    return 0;
}

static int resolve_cache_path(const a32_ctx_t *ctx, const char *elf_path,
                              char *out_path, size_t out_size, bool create_dir)
{
    if (elf_path == NULL) {
        return -1;
    }
    uint64_t h = compute_elf_hash(ctx, elf_path);
    if (h == 0ULL) {
        return -1;
    }
    return format_cache_path(ctx, "a32rt-cache", h, out_path, out_size, create_dir);
}

static uint64_t compute_fd_hash(int fd)
{
    if (fd < 0) {
        return 0ULL;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        return 0ULL;
    }
    uint64_t h = 0xcbf29ce484222325ULL;
    uint64_t sz  = (uint64_t)st.st_size;
    uint64_t mt  = (uint64_t)st.st_mtime;
    uint64_t mtn = (uint64_t)st.st_mtim.tv_nsec;
    h = fnv1a_64_step(h, &sz, sizeof(sz));
    h = fnv1a_64_step(h, &mt, sizeof(mt));
    h = fnv1a_64_step(h, &mtn, sizeof(mtn));

    uint8_t buf[4096];
    off_t offset = 0;
    while (offset < st.st_size) {
        size_t to_read = sizeof(buf);
        if ((uint64_t)(st.st_size - offset) < to_read) {
            to_read = (size_t)(st.st_size - offset);
        }
        ssize_t n = pread(fd, buf, to_read, offset);
        if (n <= 0) {
            break;
        }
        h = fnv1a_64_step(h, buf, (size_t)n);
        offset += (off_t)n;
    }
    return (h != 0ULL) ? h : 1ULL;
}

static int resolve_fd_cache_path(const a32_ctx_t *ctx, uint64_t fd_hash,
                                 char *out_path, size_t out_size, bool create_dir)
{
    return format_cache_path(ctx, "a32rt-fdcache", fd_hash, out_path, out_size, create_dir);
}

static void copy_file(const char *src_path, const char *dst_path)
{
    int src_fd = open(src_path, O_RDONLY | O_CLOEXEC);
    if (src_fd < 0) {
        return;
    }
    char tmp_dst[PATH_MAX + 64];
    int n = snprintf(tmp_dst, sizeof(tmp_dst), "%s.tmp.%d", dst_path, (int)getpid());
    if (n < 0 || (size_t)n >= sizeof(tmp_dst)) {
        close(src_fd);
        return;
    }
    int dst_fd = open(tmp_dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (dst_fd < 0) {
        close(src_fd);
        return;
    }
    uint8_t buf[4096];
    ssize_t nr;
    bool err = false;
    while ((nr = read(src_fd, buf, sizeof(buf))) > 0) {
        ssize_t nw = write(dst_fd, buf, (size_t)nr);
        if (nw != nr) {
            err = true;
            break;
        }
    }
    if (nr < 0) {
        err = true;
    }
    close(src_fd);
    close(dst_fd);
    if (!err) {
        (void)rename(tmp_dst, dst_path);
    } else {
        (void)unlink(tmp_dst);
    }
}

static bool aot_range_executable(const a32_ctx_t *ctx, uint32_t addr, uint32_t len)
{
    if (ctx == NULL || len == 0U || addr + len < addr) {
        return false;
    }
    uint32_t cur = addr;
    uint32_t end = addr + len;
    while (cur < end) {
        bool found = false;
        for (size_t i = 0U; i < ctx->region_count; i++) {
            const a32_region_t *r = &ctx->regions[i];
            if ((r->prot & 0x4U) != 0U && cur >= r->start && cur < r->end) {
                cur   = (r->end < end) ? r->end : end;
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

typedef struct aot_save_state {
    const a32_ctx_t *ctx;
    FILE            *f;
    uint32_t         count;
    bool             error;
} aot_save_state_t;

static bool aot_block_valid(const a32_ctx_t *ctx, uint32_t guest_pc, uint32_t guest_end_pc,
                            bool is_thumb, const uint32_t *words, size_t word_count,
                            const a32_pc_map_stage_t *pc_map, size_t pc_map_count)
{
    if (words == NULL || word_count == 0U || word_count > A32_AOT_MAX_WORDS ||
        words[word_count - 1U] != 0xD65F03C0U) {
        return false;
    }
    if (pc_map == NULL || pc_map_count == 0U || pc_map_count >= A32_AOT_MAX_PC_MAP) {
        return false;
    }
    uint32_t align_mask = is_thumb ? 1U : 3U;
    if ((guest_pc & align_mask) != 0U || (guest_end_pc & align_mask) != 0U ||
        guest_end_pc <= guest_pc || (guest_end_pc - guest_pc) > 512U) {
        return false;
    }
    if (!aot_range_executable(ctx, guest_pc, guest_end_pc - guest_pc)) {
        return false;
    }
    return true;
}

static void count_block_cb(uint32_t guest_pc, uint32_t guest_end_pc,
                           bool is_thumb, bool is_call,
                           const uint32_t *words, size_t word_count,
                           const a32_pc_map_stage_t *pc_map, size_t pc_map_count,
                           void *arg)
{
    (void)is_call;
    aot_save_state_t *st = (aot_save_state_t *)arg;
    if (!aot_block_valid(st->ctx, guest_pc, guest_end_pc, is_thumb,
                         words, word_count, pc_map, pc_map_count)) {
        return;
    }
    st->count++;
}

static void write_block_cb(uint32_t guest_pc, uint32_t guest_end_pc,
                           bool is_thumb, bool is_call,
                           const uint32_t *words, size_t word_count,
                           const a32_pc_map_stage_t *pc_map, size_t pc_map_count,
                           void *arg)
{
    aot_save_state_t *st = (aot_save_state_t *)arg;
    if (st->error) {
        return;
    }
    if (!aot_block_valid(st->ctx, guest_pc, guest_end_pc, is_thumb,
                         words, word_count, pc_map, pc_map_count)) {
        return;
    }
    size_t map_cnt = pc_map_count;

    a32_aot_block_header_t bh;
    bh.guest_pc     = guest_pc;
    bh.guest_end_pc = guest_end_pc;
    bh.is_thumb     = (is_thumb ? 1U : 0U) | (is_call ? 2U : 0U);
    bh.word_count   = (uint32_t)word_count;
    bh.pc_map_count = (uint32_t)map_cnt;

    if (fwrite(&bh, sizeof(bh), 1U, st->f) != 1U) {
        st->error = true;
        return;
    }
    if (fwrite(words, sizeof(uint32_t), word_count, st->f) != word_count) {
        st->error = true;
        return;
    }
    if (map_cnt > 0U) {
        if (fwrite(pc_map, sizeof(a32_pc_map_stage_t), map_cnt, st->f) != map_cnt) {
            st->error = true;
            return;
        }
    }
    st->count++;
}

int a32_aot_save_cache(a32_ctx_t *ctx, const char *elf_path)
{
    if (elf_path == NULL) {
        return -1;
    }
    if (ctx != NULL) {
        a32_vm_sync_ctx(ctx);
    }
    char cache_path[PATH_MAX];
    if (resolve_cache_path(ctx, elf_path, cache_path, sizeof(cache_path), true) != 0) {
        return -1;
    }

    char tmp_path[PATH_MAX + 64];
    int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", cache_path, (int)getpid());
    if (n < 0 || (size_t)n >= sizeof(tmp_path)) {
        return -1;
    }

    (void)unlink(tmp_path);
    int fd = open(tmp_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        return -1;
    }
    FILE *f = fdopen(fd, "wb");
    if (f == NULL) {
        close(fd);
        (void)unlink(tmp_path);
        return -1;
    }

    aot_save_state_t count_st = { .ctx = ctx, .f = NULL, .count = 0U, .error = false };
    a32_jit_iter_blocks(count_block_cb, &count_st);

    a32_aot_header_t hdr;
    hdr.magic      = A32_AOT_MAGIC;
    hdr.version    = A32_AOT_VERSION;
    hdr.num_blocks = count_st.count;
    hdr.reserved   = (ctx != NULL) ? ctx->elf_base : 0U;

    if (fwrite(&hdr, sizeof(hdr), 1U, f) != 1U) {
        fclose(f);
        unlink(tmp_path);
        return -1;
    }

    aot_save_state_t write_st = { .ctx = ctx, .f = f, .count = 0U, .error = false };
    a32_jit_iter_blocks(write_block_cb, &write_st);

    if (write_st.error || write_st.count != count_st.count) {
        fclose(f);
        unlink(tmp_path);
        return -1;
    }

    if (fclose(f) != 0) {
        unlink(tmp_path);
        return -1;
    }
    if (rename(tmp_path, cache_path) != 0) {
        unlink(tmp_path);
        return -1;
    }

    if (ctx != NULL && ctx->aot_mode) {
        char resolved_path[PATH_MAX];
        const char *host_path = elf_path;
        if (a32_resolve_guest_path(ctx, elf_path, resolved_path, sizeof(resolved_path)) == 0) {
            host_path = resolved_path;
        }
        int efd = open(host_path, O_RDONLY | O_CLOEXEC);
        if (efd < 0 && host_path != elf_path) {
            efd = open(elf_path, O_RDONLY | O_CLOEXEC);
        }
        if (efd >= 0) {
            uint64_t fdh = compute_fd_hash(efd);
            close(efd);
            if (fdh != 0ULL) {
                char fd_cache_path[PATH_MAX];
                if (resolve_fd_cache_path(ctx, fdh, fd_cache_path, sizeof(fd_cache_path), true) == 0) {
                    (void)unlink(fd_cache_path);
                    if (link(cache_path, fd_cache_path) != 0) {
                        copy_file(cache_path, fd_cache_path);
                    }
                }
            }
        }
    }

    return 0;
}

static int deserialize_aot_cache_stream(a32_ctx_t *ctx, FILE *f, uint32_t load_bias,
                                       bool allow_reloc, const char *cache_path_for_unlink)
{
    if (ctx == NULL || f == NULL) {
        return -1;
    }

    a32_aot_header_t hdr;
    if (fread(&hdr, sizeof(hdr), 1U, f) != 1U) {
        return -1;
    }
    if (hdr.magic != A32_AOT_MAGIC || hdr.version != A32_AOT_VERSION ||
        hdr.num_blocks > A32_AOT_MAX_BLOCKS) {
        if (cache_path_for_unlink != NULL) {
            (void)unlink(cache_path_for_unlink);
        }
        return -1;
    }
    if (!allow_reloc && hdr.reserved != load_bias) {
        return -1;
    }

    uint32_t delta = allow_reloc ? (load_bias - hdr.reserved) : 0U;

    uint32_t *words_buf = (uint32_t *)malloc(sizeof(uint32_t) * A32_AOT_MAX_WORDS);
    a32_pc_map_stage_t *pc_map_buf = (a32_pc_map_stage_t *)malloc(sizeof(a32_pc_map_stage_t) * A32_AOT_MAX_PC_MAP);
    if (words_buf == NULL || pc_map_buf == NULL) {
        free(words_buf);
        free(pc_map_buf);
        return -1;
    }

    uint32_t i = 0U;
    for (; i < hdr.num_blocks; i++) {
        a32_aot_block_header_t bh;
        if (fread(&bh, sizeof(bh), 1U, f) != 1U) {
            break;
        }
        if ((bh.is_thumb & ~3U) != 0U ||
            (bh.guest_pc & ((bh.is_thumb & 1U) ? 1U : 3U)) != 0U ||
            (bh.guest_end_pc & ((bh.is_thumb & 1U) ? 1U : 3U)) != 0U ||
            bh.guest_end_pc <= bh.guest_pc ||
            bh.guest_end_pc - bh.guest_pc > 512U ||
            bh.word_count == 0U || bh.word_count > A32_AOT_MAX_WORDS ||
            bh.pc_map_count >= A32_AOT_MAX_PC_MAP) {
            break;
        }
        if (fread(words_buf, sizeof(uint32_t), bh.word_count, f) != bh.word_count) {
            break;
        }
        if (bh.pc_map_count > 0U) {
            if (fread(pc_map_buf, sizeof(a32_pc_map_stage_t), bh.pc_map_count, f) != bh.pc_map_count) {
                break;
            }
        }
        if (delta != 0U && bh.guest_pc < 0xFFFF0000U) {
            bh.guest_pc += delta;
            bh.guest_end_pc += delta;
            for (uint32_t m = 0U; m < bh.pc_map_count; m++) {
                pc_map_buf[m].guest_pc += delta;
            }
            for (uint32_t w = 0U; w + 1U < bh.word_count; w++) {
                uint32_t w0 = words_buf[w];
                uint32_t w1 = words_buf[w + 1U];
                if ((w0 & 0xFFE00000U) == 0x52A00000U &&
                    (w1 & 0xFFE00000U) == 0x72800000U &&
                    (w0 & 31U) == (w1 & 31U)) {
                    uint32_t orig_pc = (((w0 >> 5U) & 0xFFFFU) << 16U) | ((w1 >> 5U) & 0xFFFFU);
                    if (orig_pc >= 0x10000U && orig_pc < 0xFFFF0000U) {
                        uint32_t reloc_pc = orig_pc + delta;
                        words_buf[w]      = (w0 & ~(0xFFFFU << 5U)) | (((reloc_pc >> 16U) & 0xFFFFU) << 5U);
                        words_buf[w + 1U] = (w1 & ~(0xFFFFU << 5U)) | ((reloc_pc & 0xFFFFU) << 5U);
                    }
                    w++; /* skip second word of pair */
                }
            }
        }
        if (!aot_range_executable(ctx, bh.guest_pc, bh.guest_end_pc - bh.guest_pc)) {
            continue;
        }
        if (a32_jit_install_aot_block(ctx, bh.guest_pc, bh.guest_end_pc,
                                      (bh.is_thumb & 1U) != 0U,
                                      (bh.is_thumb & 2U) != 0U,
                                      words_buf, (size_t)bh.word_count,
                                      bh.pc_map_count > 0U ? pc_map_buf : NULL,
                                      (size_t)bh.pc_map_count) != 0) {
            break;
        }
    }

    bool eof_ok = (i == hdr.num_blocks && fgetc(f) == EOF && ferror(f) == 0);
    free(words_buf);
    free(pc_map_buf);

    if (!eof_ok) {
        (void)a32_jit_flush_batch(ctx);
        if (!allow_reloc) {
            (void)a32_jit_invalidate_range(ctx, 0U, 0xFFFFFFFFU);
        }
        if (cache_path_for_unlink != NULL) {
            (void)unlink(cache_path_for_unlink);
        }
        return -1;
    }
    if (a32_jit_flush_batch(ctx) != 0) {
        if (!allow_reloc) {
            (void)a32_jit_invalidate_range(ctx, 0U, 0xFFFFFFFFU);
        }
        return -1;
    }
    return 0;
}

int a32_aot_load_cache(a32_ctx_t *ctx, const char *elf_path)
{
    if (ctx == NULL || elf_path == NULL) {
        return -1;
    }
    char cache_path[PATH_MAX];
    if (resolve_cache_path(ctx, elf_path, cache_path, sizeof(cache_path), false) != 0) {
        return -1;
    }

    int fd = open(cache_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    struct stat st;
    if (!(fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0022) == 0)) {
        close(fd);
        return -1;
    }
    FILE *f = fdopen(fd, "rb");
    if (f == NULL) {
        close(fd);
        return -1;
    }

    uint32_t load_bias = ctx->elf_base;
    int rc = deserialize_aot_cache_stream(ctx, f, load_bias, false, cache_path);
    fclose(f);
    return rc;
}

int a32_aot_load_fd(a32_ctx_t *ctx, int fd, const char *label, uint32_t load_bias)
{
    (void)label;
    if (ctx == NULL || fd < 0) {
        return -1;
    }
    uint64_t fd_hash = compute_fd_hash(fd);
    if (fd_hash == 0ULL) {
        return -1;
    }
    char cache_path[PATH_MAX];
    if (resolve_fd_cache_path(ctx, fd_hash, cache_path, sizeof(cache_path), false) != 0) {
        return -1;
    }

    int cfd = open(cache_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (cfd < 0) {
        return -1;
    }
    struct stat st;
    if (!(fstat(cfd, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0022) == 0)) {
        close(cfd);
        return -1;
    }
    FILE *f = fdopen(cfd, "rb");
    if (f == NULL) {
        close(cfd);
        return -1;
    }

    int rc = deserialize_aot_cache_stream(ctx, f, load_bias, true, cache_path);
    fclose(f);
    return rc;
}

static void add_target(uint32_t pc, bool thumb,
                       uint32_t *pcs, bool *thumbs, size_t *count)
{
    if (*count >= A32_MAX_AOT_SCAN) {
        return;
    }
    for (size_t i = 0U; i < *count; i++) {
        if (pcs[i] == pc && thumbs[i] == thumb) {
            return;
        }
    }
    pcs[*count]    = pc;
    thumbs[*count] = thumb;
    (*count)++;
}

typedef struct aot_end_scan {
    const a32_ctx_t      *ctx;
    const a32_elf_info_t *elf_info;
    uint32_t             *pcs;
    bool                 *thumbs;
    size_t               *count;
} aot_end_scan_t;

static void scan_text_range(a32_ctx_t *ctx, uint32_t r_start, uint32_t r_end,
                            bool prefer_thumb, uint32_t *disc_pcs, bool *disc_thumb,
                            size_t *disc_count, size_t max_count);

static void collect_end_pc_cb(uint32_t guest_pc, uint32_t guest_end_pc,
                              bool is_thumb, bool is_call,
                              const uint32_t *words, size_t word_count,
                              const a32_pc_map_stage_t *pc_map, size_t pc_map_count,
                              void *arg)
{
    (void)is_call;
    (void)words;
    (void)word_count;
    (void)pc_map;
    (void)pc_map_count;
    aot_end_scan_t *s = (aot_end_scan_t *)arg;
    uint32_t min_len = is_thumb ? 2U : 4U;
    if (guest_end_pc > guest_pc && guest_end_pc - guest_pc <= 512U) {
        scan_text_range((a32_ctx_t *)s->ctx, guest_pc, guest_end_pc,
                        is_thumb, s->pcs, s->thumbs, s->count, A32_MAX_AOT_SCAN);
    }
    if (guest_end_pc >= 0x10000U && guest_end_pc <= 0xFFFF0000U - min_len &&
        aot_range_executable(s->ctx, guest_end_pc, min_len)) {
        add_target(guest_end_pc, is_thumb, s->pcs, s->thumbs, s->count);
    }
}

static void scan_text_range(a32_ctx_t *ctx, uint32_t r_start, uint32_t r_end,
                            bool prefer_thumb, uint32_t *disc_pcs, bool *disc_thumb,
                            size_t *disc_count, size_t max_count)
{
    if (r_start == 0U || r_end <= r_start) {
        return;
    }
    if (max_count > A32_MAX_AOT_SCAN) {
        max_count = A32_MAX_AOT_SCAN;
    }
    if (prefer_thumb) {
        uint32_t scan_start = (r_start + 1U) & ~1U;
        for (uint32_t pc = scan_start;
             pc + 2U <= r_end && *disc_count < max_count;
             pc += 2U) {
            if (!aot_range_executable(ctx, pc, 2U)) {
                continue;
            }
            const uint16_t *hw_ptr = (const uint16_t *)a32_guest_ptr(ctx, pc);
            uint16_t hw = *hw_ptr;

            if ((hw >> 11U) >= 0x1DU) {
                if (pc + 4U > r_end) {
                    break;
                }
                if (!aot_range_executable(ctx, pc + 2U, 2U)) {
                    continue;
                }
                uint16_t hw1 = hw;
                uint16_t hw2 = hw_ptr[1];

                /* 32-bit prologue: PUSH.W {..., lr} (STMDB sp!, {..., lr}) */
                if (hw1 == 0xE92DU && (hw2 & 0x4000U) != 0U) {
                    add_target(pc, true, disc_pcs, disc_thumb, disc_count);
                } else if (hw1 == 0xE8BDU && (hw2 & 0x8000U) != 0U) {
                    /* 32-bit epilogue: POP.W {..., pc} -> next function entry */
                    for (uint32_t next_fn = pc + 4U;
                         next_fn + 2U <= r_end && next_fn <= pc + 8U;
                         next_fn += 2U) {
                        if (aot_range_executable(ctx, next_fn, 2U)) {
                            add_target(next_fn, true, disc_pcs, disc_thumb, disc_count);
                            if (*(const uint16_t *)a32_guest_ptr(ctx, next_fn) != 0xBF00U) {
                                break;
                            }
                        }
                    }
                } else if ((hw1 & 0xF800U) == 0xF000U && (hw2 & 0x8000U) != 0U) {
                    if ((hw2 & 0xD000U) == 0xD000U || (hw2 & 0xD000U) == 0xC000U ||
                        (hw2 & 0xD000U) == 0x9000U) {
                        /* 32-bit BL / BLX / B.W <label> */
                        bool is_blx    = (hw2 & 0xD000U) == 0xC000U;
                        bool is_link   = (hw2 & 0x4000U) != 0U;
                        uint32_t s     = (hw1 >> 10U) & 1U;
                        uint32_t j1    = (hw2 >> 13U) & 1U;
                        uint32_t j2    = (hw2 >> 11U) & 1U;
                        uint32_t i1    = ~(j1 ^ s) & 1U;
                        uint32_t i2    = ~(j2 ^ s) & 1U;
                        uint32_t imm10 = hw1 & 0x3FFU;
                        uint32_t imm11 = hw2 & 0x7FFU;
                        int32_t offset = (int32_t)((s << 24U) | (i1 << 23U) | (i2 << 22U) |
                                                   (imm10 << 12U) | (imm11 << 1U));
                        if (s != 0U) {
                            offset |= (int32_t)0xFE000000U;
                        }
                        uint32_t tgt = is_blx ? ((((pc + 4U) & ~3U) + (uint32_t)offset) & ~3U)
                                              : (pc + 4U + (uint32_t)offset);
                        if (tgt >= 0x10000U && tgt <= 0xFFFFFFFCU && tgt + 4U <= 0xFFFF0000U &&
                            aot_range_executable(ctx, tgt, 4U)) {
                            add_target(tgt, !is_blx, disc_pcs, disc_thumb, disc_count);
                        }
                        if (is_link && pc <= 0xFFFFFFF7U && pc + 8U <= r_end &&
                            aot_range_executable(ctx, pc + 4U, 4U)) {
                            add_target(pc + 4U, true, disc_pcs, disc_thumb, disc_count);
                        }
                    } else if ((hw2 & 0xD000U) == 0x8000U) {
                        unsigned cond = (hw1 >> 6U) & 0xFU;
                        if (cond < 14U) {
                            /* 32-bit B<cond>.W <label> */
                            uint32_t s     = (hw1 >> 10U) & 1U;
                            uint32_t j1    = (hw2 >> 13U) & 1U;
                            uint32_t j2    = (hw2 >> 11U) & 1U;
                            uint32_t imm6  = hw1 & 0x3FU;
                            uint32_t imm11 = hw2 & 0x7FFU;
                            int32_t offset = (int32_t)((s << 20U) | (j2 << 19U) | (j1 << 18U) |
                                                       (imm6 << 12U) | (imm11 << 1U));
                            if (s != 0U) {
                                offset |= (int32_t)0xFFE00000U;
                            }
                            uint32_t tgt = pc + 4U + (uint32_t)offset;
                            if (tgt >= 0x10000U && tgt <= 0xFFFFFFFCU && tgt + 4U <= 0xFFFF0000U &&
                                aot_range_executable(ctx, tgt, 4U)) {
                                add_target(tgt, true, disc_pcs, disc_thumb, disc_count);
                            }
                        }
                    }
                }

                /* 32-bit Thumb-2 instruction: advance past second halfword */
                pc += 2U;
                continue;
            }

            /* Function prologues: PUSH {..., lr} or SUB sp, #imm */
            if (((hw & 0xFF00U) == 0xB500U || (hw & 0xFF80U) == 0xB080U) &&
                pc <= 0xFFFFFFFCU && pc + 4U <= r_end &&
                aot_range_executable(ctx, pc, 4U)) {
                add_target(pc, true, disc_pcs, disc_thumb, disc_count);
            }
            /* Epilogues: BX lr or POP {..., pc} -> next function entry */
            else if (hw == 0x4770U || (hw & 0xFF00U) == 0xBD00U) {
                for (uint32_t next_fn = pc + 2U;
                     next_fn + 2U <= r_end && next_fn <= pc + 6U;
                     next_fn += 2U) {
                    if (aot_range_executable(ctx, next_fn, 2U)) {
                        add_target(next_fn, true, disc_pcs, disc_thumb, disc_count);
                        if (*(const uint16_t *)a32_guest_ptr(ctx, next_fn) != 0xBF00U) {
                            break;
                        }
                    }
                }
            }
            /* CBZ / CBNZ Rn, label */
            else if ((hw & 0xF500U) == 0xB100U) {
                uint32_t offset = (((hw >> 9U) & 1U) << 6U) | (((hw >> 3U) & 31U) << 1U);
                uint32_t tgt = pc + 4U + offset;
                if (tgt >= 0x10000U && tgt + 2U <= 0xFFFF0000U &&
                    aot_range_executable(ctx, tgt, 2U)) {
                    add_target(tgt, true, disc_pcs, disc_thumb, disc_count);
                }
            }
            /* SVC #imm8 -> fallthrough block at pc + 2 */
            else if ((hw & 0xFF00U) == 0xDF00U) {
                if (pc + 4U <= r_end && aot_range_executable(ctx, pc + 2U, 2U)) {
                    add_target(pc + 2U, true, disc_pcs, disc_thumb, disc_count);
                }
            }
            /* Branch cond: B<c> */
            else if ((hw & 0xF000U) == 0xD000U && (hw & 0x0E00U) != 0x0E00U) {
                int8_t imm8 = (int8_t)(hw & 0xFFU);
                uint32_t tgt = (uint32_t)((int32_t)pc + 4 + ((int32_t)imm8 << 1));
                if (tgt >= 0x10000U && tgt <= 0xFFFFFFFCU && tgt + 4U <= 0xFFFF0000U &&
                    aot_range_executable(ctx, tgt, 4U)) {
                    add_target(tgt, true, disc_pcs, disc_thumb, disc_count);
                }
            }
            /* Branch uncond: B */
            else if ((hw & 0xF800U) == 0xE000U) {
                int32_t imm11 = (int32_t)(hw & 0x7FFU);
                if ((imm11 & 0x400) != 0) {
                    imm11 |= ~0x7FF;
                }
                uint32_t tgt = (uint32_t)((int32_t)pc + 4 + (imm11 << 1));
                if (tgt >= 0x10000U && tgt <= 0xFFFFFFFCU && tgt + 4U <= 0xFFFF0000U &&
                    aot_range_executable(ctx, tgt, 4U)) {
                    add_target(tgt, true, disc_pcs, disc_thumb, disc_count);
                }
            }
        }
    } else {
        uint32_t scan_start = (r_start + 3U) & ~3U;
        for (uint32_t pc = scan_start;
             pc + 4U <= r_end && *disc_count < max_count;
             pc += 4U) {
            if (!aot_range_executable(ctx, pc, 4U)) {
                continue;
            }
            const uint32_t *insn_ptr = (const uint32_t *)a32_guest_ptr(ctx, pc);
            uint32_t insn = *insn_ptr;

            /* Function prologues: STMDB sp!, {..., lr} or SUB sp, sp, #imm */
            if ((insn >> 28U) != 0xFU &&
                ((insn & 0x0FFF4000U) == 0x092D4000U ||
                 (insn & 0x0FFF0000U) == 0x024D0000U ||
                 insn == 0xE52DE004U)) {
                add_target(pc, false, disc_pcs, disc_thumb, disc_count);
            }
            /* SVC #imm24 or return (BX lr / POP {..., pc}) -> fallthrough / next fn at pc + 4 */
            else if ((insn >> 28U) != 0xFU &&
                     ((insn & 0x0F000000U) == 0x0F000000U ||
                      (insn & 0x0FFFFFFFU) == 0x012FFF1EU ||
                      (insn & 0x0E108000U) == 0x08108000U)) {
                if (pc + 8U <= r_end && aot_range_executable(ctx, pc + 4U, 4U)) {
                    add_target(pc + 4U, false, disc_pcs, disc_thumb, disc_count);
                }
            }
            /* Branch / call: B / BL */
            else if ((insn & 0x0E000000U) == 0x0A000000U) {
                int32_t imm24 = (int32_t)(insn & 0x00FFFFFFU);
                if ((imm24 & 0x00800000) != 0) {
                    imm24 |= (int32_t)0xFF000000U;
                }
                uint32_t tgt = (uint32_t)((int32_t)pc + 8 + (imm24 << 2));
                if (tgt >= 0x10000U && tgt <= 0xFFFFFFFCU && tgt + 4U <= 0xFFFF0000U &&
                    aot_range_executable(ctx, tgt, 4U)) {
                    add_target(tgt, false, disc_pcs, disc_thumb, disc_count);
                }
                if (((insn >> 24U) & 1U) != 0U &&
                    pc <= 0xFFFFFFF7U && pc + 8U <= r_end &&
                    aot_range_executable(ctx, pc + 4U, 4U)) {
                    add_target(pc + 4U, false, disc_pcs, disc_thumb, disc_count);
                }
            }
            /* Branch with Link and Exchange: BLX (imm) */
            else if ((insn & 0xFE000000U) == 0xFA000000U) {
                int32_t imm24 = (int32_t)(insn & 0x00FFFFFFU);
                if ((imm24 & 0x00800000) != 0) {
                    imm24 |= (int32_t)0xFF000000U;
                }
                uint32_t h = (insn >> 24U) & 1U;
                uint32_t tgt = (uint32_t)((int32_t)pc + 8 + (imm24 << 2) + (int32_t)(h << 1U));
                if (tgt >= 0x10000U && tgt <= 0xFFFFFFFCU && tgt + 4U <= 0xFFFF0000U &&
                    aot_range_executable(ctx, tgt, 4U)) {
                    add_target(tgt, true, disc_pcs, disc_thumb, disc_count);
                }
                if (pc <= 0xFFFFFFF7U && pc + 8U <= r_end &&
                    aot_range_executable(ctx, pc + 4U, 4U)) {
                    add_target(pc + 4U, false, disc_pcs, disc_thumb, disc_count);
                }
            }
        }
    }
}

int a32_aot_generate(a32_ctx_t *ctx, const a32_elf_info_t *elf_info, const char *elf_path)
{
    if (ctx == NULL || elf_info == NULL || elf_path == NULL) {
        return -1;
    }

    bool     entry_thumb = (elf_info->entry & 1U) != 0U;
    uint32_t entry_pc    = elf_info->entry & (entry_thumb ? ~1U : ~3U);

    if (aot_range_executable(ctx, entry_pc, entry_thumb ? 2U : 4U)) {
        (void)a32_jit_compile_block(ctx, entry_pc, entry_thumb, NULL);
    }

    if (ctx->elf_entry != 0U && ctx->elf_entry != elf_info->entry) {
        bool     main_thumb = (ctx->elf_entry & 1U) != 0U;
        uint32_t main_pc    = ctx->elf_entry & (main_thumb ? ~1U : ~3U);
        if (aot_range_executable(ctx, main_pc, main_thumb ? 2U : 4U)) {
            (void)a32_jit_compile_block(ctx, main_pc, main_thumb, NULL);
        }
    }

    static uint32_t disc_pcs[A32_MAX_AOT_SCAN];
    static bool     disc_thumb[A32_MAX_AOT_SCAN];
    size_t          disc_count = 0U;

    bool has_aux_exec = false;
    for (size_t i = 0U; i < ctx->region_count; i++) {
        const a32_region_t *r = &ctx->regions[i];
        if ((r->prot & 0x4U) == 0U || r->start < 0x10000U || r->end > 0xFFFF0000U) {
            continue;
        }
        if (elf_info->text_start > 0U && elf_info->text_end > elf_info->text_start &&
            r->start < elf_info->text_end && r->end > elf_info->text_start) {
            continue;
        }
        has_aux_exec = true;
        bool     r_thumb = entry_thumb;
        uint32_t r_entry = r->start;
        if (ctx->elf_entry != 0U &&
            (ctx->elf_entry & ~3U) >= r->start && (ctx->elf_entry & ~3U) < r->end) {
            r_thumb = (ctx->elf_entry & 1U) != 0U;
            r_entry = ctx->elf_entry & (r_thumb ? ~1U : ~3U);
        }
        size_t aux_budget = disc_count + 1024U;
        if (r_entry > r->start && r_entry < r->end) {
            scan_text_range(ctx, r_entry, r->end, r_thumb, disc_pcs, disc_thumb, &disc_count, aux_budget);
            scan_text_range(ctx, r->start, r_entry, r_thumb, disc_pcs, disc_thumb, &disc_count, aux_budget);
        } else {
            scan_text_range(ctx, r->start, r->end, r_thumb, disc_pcs, disc_thumb, &disc_count, aux_budget);
        }
    }

    if (elf_info->text_start > 0U && elf_info->text_end > elf_info->text_start) {
        size_t main_budget = has_aux_exec ? (disc_count + 1280U) : (A32_MAX_AOT_SCAN - 512U);
        if (entry_pc > elf_info->text_start && entry_pc < elf_info->text_end) {
            scan_text_range(ctx, entry_pc, elf_info->text_end,
                            entry_thumb, disc_pcs, disc_thumb, &disc_count, main_budget);
            scan_text_range(ctx, elf_info->text_start, entry_pc,
                            entry_thumb, disc_pcs, disc_thumb, &disc_count, main_budget);
        } else {
            scan_text_range(ctx, elf_info->text_start, elf_info->text_end,
                            entry_thumb, disc_pcs, disc_thumb, &disc_count, main_budget);
        }
    }

    size_t compiled_idx = 0U;
    for (unsigned pass = 0U; pass < 6U; pass++) {
        size_t limit = disc_count;
        for (; compiled_idx < limit; compiled_idx++) {
            uint32_t min_len = disc_thumb[compiled_idx] ? 2U : 4U;
            if (aot_range_executable(ctx, disc_pcs[compiled_idx], min_len)) {
                (void)a32_jit_compile_block(ctx, disc_pcs[compiled_idx], disc_thumb[compiled_idx], NULL);
            }
        }
        aot_end_scan_t scan = {
            .ctx      = ctx,
            .elf_info = elf_info,
            .pcs      = disc_pcs,
            .thumbs   = disc_thumb,
            .count    = &disc_count,
        };
        a32_jit_iter_blocks(collect_end_pc_cb, &scan);
        if (disc_count == limit) {
            break;
        }
    }

    if (a32_jit_flush_batch(ctx) != 0) {
        return -1;
    }
    return a32_aot_save_cache(ctx, elf_path);
}
