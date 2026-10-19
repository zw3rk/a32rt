/*
 * src/aot.c — Ahead-Of-Time (AOT) pre-translator and disk cache
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
#define A32_AOT_VERSION    7U
#define A32_MAX_AOT_SCAN   4096U
#define A32_AOT_MAX_BLOCKS 16384U
#define A32_AOT_MAX_WORDS  2048U
#define A32_AOT_MAX_PC_MAP 256U

typedef struct a32_aot_header {
    uint32_t magic;
    uint32_t version;
    uint32_t num_blocks;
    uint32_t load_bias;
} a32_aot_header_t;

static inline int32_t sign_extend_24(uint32_t v)
{
    return (v & 0x00800000U) ? (int32_t)(v | 0xFF000000U) : (int32_t)(v & 0x00FFFFFFU);
}

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

static uint64_t hash_fd_contents(uint64_t h, int fd)
{
    if (fd < 0) {
        return 0ULL;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        return 0ULL;
    }
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
            if (n < 0 && errno == EINTR) {
                continue;
            }
            return 0ULL;
        }
        h = fnv1a_64_step(h, buf, (size_t)n);
        offset += (off_t)n;
    }
    return (h != 0ULL) ? h : 1ULL;
}

static uint64_t compute_fd_hash(int fd)
{
    return hash_fd_contents(0xcbf29ce484222325ULL, fd);
}

static uint64_t compute_elf_hash(const a32_ctx_t *ctx, const char *elf_path,
                                 uint64_t *out_fd_hash)
{
    if (elf_path == NULL) {
        return 0ULL;
    }
    int fd = a32_open_guest_ro(ctx, elf_path);
    if (fd < 0) {
        return 0ULL;
    }
    uint64_t fd_hash = compute_fd_hash(fd);
    close(fd);
    if (fd_hash == 0ULL) {
        return 0ULL;
    }
    if (out_fd_hash != NULL) {
        *out_fd_hash = fd_hash;
    }
    uint64_t h = fd_hash;
    if (ctx != NULL) {
        h = fnv1a_64_step(h, &ctx->elf_base, sizeof(ctx->elf_base));
        h = fnv1a_64_step(h, &ctx->interp_base, sizeof(ctx->interp_base));
        h = fnv1a_64_step(h, &ctx->elf_entry, sizeof(ctx->elf_entry));
        if (ctx->sysroot != NULL && ctx->sysroot[0] != '\0') {
            h = fnv1a_64_step(h, ctx->sysroot, strlen(ctx->sysroot));
        }
    }
    h = fnv1a_64_step(h, elf_path, strlen(elf_path));
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
                              char *out_path, size_t out_size, bool create_dir,
                              uint64_t *out_fd_hash)
{
    uint64_t h = compute_elf_hash(ctx, elf_path, out_fd_hash);
    if (h == 0ULL) {
        return -1;
    }
    return format_cache_path(ctx, "a32rt-cache", h, out_path, out_size, create_dir);
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
    return a32_jit_range_executable(ctx, guest_pc, guest_end_pc - guest_pc);
}

static void count_block_cb(uint32_t guest_pc, uint32_t guest_end_pc,
                           bool is_thumb, bool is_call,
                           const uint32_t *words, size_t word_count,
                           const a32_pc_map_stage_t *pc_map, size_t pc_map_count,
                           void *arg)
{
    (void)is_call;
    aot_save_state_t *st = (aot_save_state_t *)arg;
    if (aot_block_valid(st->ctx, guest_pc, guest_end_pc, is_thumb,
                        words, word_count, pc_map, pc_map_count)) {
        st->count++;
    }
}

static void write_block_cb(uint32_t guest_pc, uint32_t guest_end_pc,
                           bool is_thumb, bool is_call,
                           const uint32_t *words, size_t word_count,
                           const a32_pc_map_stage_t *pc_map, size_t pc_map_count,
                           void *arg)
{
    aot_save_state_t *st = (aot_save_state_t *)arg;
    if (st->error ||
        !aot_block_valid(st->ctx, guest_pc, guest_end_pc, is_thumb,
                         words, word_count, pc_map, pc_map_count)) {
        return;
    }

    a32_aot_block_header_t bh = {
        .guest_pc     = guest_pc,
        .guest_end_pc = guest_end_pc,
        .is_thumb     = (is_thumb ? 1U : 0U) | (is_call ? 2U : 0U),
        .word_count   = (uint32_t)word_count,
        .pc_map_count = (uint32_t)pc_map_count,
    };

    if (fwrite(&bh, sizeof(bh), 1U, st->f) != 1U ||
        fwrite(words, sizeof(uint32_t), word_count, st->f) != word_count ||
        fwrite(pc_map, sizeof(a32_pc_map_stage_t), pc_map_count, st->f) != pc_map_count) {
        st->error = true;
        return;
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
    char     cache_path[PATH_MAX];
    uint64_t fd_hash = 0ULL;
    if (resolve_cache_path(ctx, elf_path, cache_path, sizeof(cache_path), true, &fd_hash) != 0) {
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

    a32_aot_header_t hdr = {
        .magic      = A32_AOT_MAGIC,
        .version    = A32_AOT_VERSION,
        .num_blocks = count_st.count,
        .load_bias  = (ctx != NULL) ? ctx->elf_base : 0U,
    };

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

    if (fclose(f) != 0 || rename(tmp_path, cache_path) != 0) {
        unlink(tmp_path);
        return -1;
    }

    if (ctx != NULL && ctx->aot_mode && fd_hash != 0ULL) {
        char fd_cache_path[PATH_MAX];
        if (format_cache_path(ctx, "a32rt-fdcache", fd_hash,
                              fd_cache_path, sizeof(fd_cache_path), true) == 0) {
            (void)unlink(fd_cache_path);
            if (link(cache_path, fd_cache_path) != 0) {
                copy_file(cache_path, fd_cache_path);
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
    if (!allow_reloc && hdr.load_bias != load_bias) {
        return -1;
    }

    uint32_t delta = allow_reloc ? (load_bias - hdr.load_bias) : 0U;

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
        if (bh.pc_map_count > 0U &&
            fread(pc_map_buf, sizeof(a32_pc_map_stage_t), bh.pc_map_count, f) != bh.pc_map_count) {
            break;
        }
        if (delta != 0U && bh.guest_pc < A32_KUSER_BASE_32) {
            bh.guest_pc     += delta;
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
                    if (orig_pc >= A32_LOW_GUARD_SIZE && orig_pc < A32_KUSER_BASE_32) {
                        uint32_t reloc_pc = orig_pc + delta;
                        words_buf[w]      = (w0 & ~(0xFFFFU << 5U)) | (((reloc_pc >> 16U) & 0xFFFFU) << 5U);
                        words_buf[w + 1U] = (w1 & ~(0xFFFFU << 5U)) | ((reloc_pc & 0xFFFFU) << 5U);
                    }
                    w++;
                }
            }
        }
        if (!a32_jit_range_executable(ctx, bh.guest_pc, bh.guest_end_pc - bh.guest_pc)) {
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

static int load_cache_from_path(a32_ctx_t *ctx, const char *cache_path,
                                uint32_t load_bias, bool allow_reloc)
{
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
    int rc = deserialize_aot_cache_stream(ctx, f, load_bias, allow_reloc, cache_path);
    fclose(f);
    return rc;
}

int a32_aot_load_cache(a32_ctx_t *ctx, const char *elf_path)
{
    if (ctx == NULL || elf_path == NULL) {
        return -1;
    }
    char cache_path[PATH_MAX];
    if (resolve_cache_path(ctx, elf_path, cache_path, sizeof(cache_path), false, NULL) != 0) {
        return -1;
    }
    return load_cache_from_path(ctx, cache_path, ctx->elf_base, false);
}

int a32_aot_load_fd(a32_ctx_t *ctx, int fd, uint32_t load_bias)
{
    if (ctx == NULL || fd < 0) {
        return -1;
    }
    uint64_t fd_hash = compute_fd_hash(fd);
    if (fd_hash == 0ULL) {
        return -1;
    }
    char cache_path[PATH_MAX];
    if (format_cache_path(ctx, "a32rt-fdcache", fd_hash, cache_path, sizeof(cache_path), false) != 0) {
        return -1;
    }
    return load_cache_from_path(ctx, cache_path, load_bias, true);
}

typedef struct aot_scan_buf {
    const a32_ctx_t *ctx;
    uint32_t        *pcs;
    bool            *thumbs;
    uint32_t        *seen_keys;
    size_t           count;
    size_t           limit;
} aot_scan_buf_t;

static void add_target(aot_scan_buf_t *sb, uint32_t pc, bool thumb)
{
    if (sb->count >= sb->limit) {
        return;
    }
    if (sb->seen_keys != NULL) {
        uint32_t key = a32_make_key(pc, thumb);
        if (key == 0U) {
            key = 0xFFFFFFFFU;
        }
        uint32_t idx = (key * 2654435761U) & 65535U;
        while (sb->seen_keys[idx] != 0U) {
            if (sb->seen_keys[idx] == key) {
                return;
            }
            idx = (idx + 1U) & 65535U;
        }
        sb->seen_keys[idx] = key;
    } else {
        for (size_t i = 0U; i < sb->count; i++) {
            if (sb->pcs[i] == pc && sb->thumbs[i] == thumb) {
                return;
            }
        }
    }
    sb->pcs[sb->count]    = pc;
    sb->thumbs[sb->count] = thumb;
    sb->count++;
}

static void aot_try_add_target(aot_scan_buf_t *sb, uint32_t tgt, bool thumb, uint32_t min_len)
{
    if (tgt >= A32_LOW_GUARD_SIZE && tgt <= A32_KUSER_BASE_32 - min_len &&
        a32_jit_range_executable(sb->ctx, tgt, min_len)) {
        add_target(sb, tgt, thumb);
    }
}

static void seed_elf_symbols(aot_scan_buf_t *sb, const a32_ctx_t *ctx,
                             const char *elf_path, bool *io_prefer_thumb)
{
    int fd = a32_open_guest_ro(ctx, elf_path);
    if (fd < 0) {
        return;
    }
    uint8_t ehdr[52];
    if (pread(fd, ehdr, sizeof(ehdr), 0) != (ssize_t)sizeof(ehdr) ||
        memcmp(ehdr, "\x7f""ELF\x01\x01\x01", 7U) != 0) {
        close(fd);
        return;
    }
    uint32_t e_shoff;
    uint16_t e_shentsize, e_shnum;
    memcpy(&e_shoff, ehdr + 32, 4U);
    memcpy(&e_shentsize, ehdr + 46, 2U);
    memcpy(&e_shnum, ehdr + 48, 2U);
    if (e_shoff == 0U || e_shentsize != 40U || e_shnum == 0U || e_shnum > 1024U) {
        close(fd);
        return;
    }

    size_t thumb_syms = 0U;
    size_t arm_syms   = 0U;
    for (uint16_t i = 0U; i < e_shnum; i++) {
        uint8_t sh[40];
        if (pread(fd, sh, sizeof(sh), (off_t)e_shoff + (off_t)i * 40) != (ssize_t)sizeof(sh)) {
            break;
        }
        uint32_t sh_type, sh_offset, sh_size, sh_entsize;
        memcpy(&sh_type, sh + 4, 4U);
        memcpy(&sh_offset, sh + 16, 4U);
        memcpy(&sh_size, sh + 20, 4U);
        memcpy(&sh_entsize, sh + 36, 4U);
        if ((sh_type != 2U && sh_type != 11U) || sh_entsize != 16U ||
            sh_size == 0U || sh_size > (16U * 1024U * 1024U)) {
            continue;
        }
        uint8_t sym_buf[4096];
        for (uint32_t off = 0U; off + 16U <= sh_size; off += (uint32_t)sizeof(sym_buf)) {
            size_t chunk = sh_size - off;
            if (chunk > sizeof(sym_buf)) {
                chunk = sizeof(sym_buf);
            }
            chunk &= ~(size_t)15U;
            if (pread(fd, sym_buf, chunk, (off_t)sh_offset + (off_t)off) != (ssize_t)chunk) {
                break;
            }
            for (size_t s = 0U; s < chunk; s += 16U) {
                uint32_t st_value;
                uint8_t  st_info = sym_buf[s + 12U];
                uint16_t st_shndx;
                memcpy(&st_value, sym_buf + s + 4U, 4U);
                memcpy(&st_shndx, sym_buf + s + 14U, 2U);
                if ((st_info & 0xFU) != 2U || st_shndx == 0U || st_value == 0U) {
                    continue;
                }
                bool     is_t   = (st_value & 1U) != 0U;
                uint32_t sym_pc = st_value & (is_t ? ~1U : ~3U);
                if (ctx->elf_base != 0U && sym_pc < ctx->elf_base) {
                    sym_pc += ctx->elf_base;
                }
                if (sb->count < 1024U) {
                    aot_try_add_target(sb, sym_pc, is_t, is_t ? 2U : 4U);
                }
                if (is_t) {
                    thumb_syms++;
                } else {
                    arm_syms++;
                }
            }
        }
    }
    close(fd);
    if (io_prefer_thumb != NULL && (thumb_syms + arm_syms) > 0U) {
        *io_prefer_thumb = (thumb_syms >= arm_syms);
    }
}

static void scan_text_range(aot_scan_buf_t *sb, uint32_t r_start, uint32_t r_end, bool prefer_thumb)
{
    if (r_start == 0U || r_end <= r_start ||
        !a32_jit_range_executable(sb->ctx, r_start, r_end - r_start)) {
        return;
    }
    const a32_ctx_t *ctx = sb->ctx;

    if (prefer_thumb) {
        uint32_t scan_start = (r_start + 1U) & ~1U;
        for (uint32_t pc = scan_start;
             pc + 2U <= r_end && sb->count < sb->limit;
             pc += 2U) {
            const uint16_t *hw_ptr = (const uint16_t *)a32_guest_ptr(ctx, pc);
            uint16_t hw = *hw_ptr;

            if ((hw >> 11U) >= 0x1DU) {
                if (pc + 4U > r_end) {
                    break;
                }
                uint16_t hw1 = hw;
                uint16_t hw2 = hw_ptr[1];

                /* 32-bit prologue: PUSH.W {..., lr} (STMDB sp!, {..., lr}) */
                if (hw1 == 0xE92DU && (hw2 & 0x4000U) != 0U) {
                    add_target(sb, pc, true);
                } else if (hw1 == 0xE8BDU && (hw2 & 0x8000U) != 0U) {
                    /* 32-bit epilogue: POP.W {..., pc} -> next function entry */
                    for (uint32_t next_fn = pc + 4U;
                         next_fn + 2U <= r_end && next_fn <= pc + 8U;
                         next_fn += 2U) {
                        add_target(sb, next_fn, true);
                        if (*(const uint16_t *)a32_guest_ptr(ctx, next_fn) != 0xBF00U) {
                            break;
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
                        aot_try_add_target(sb, tgt, !is_blx, 4U);
                        if (is_link && pc <= 0xFFFFFFF7U && pc + 8U <= r_end) {
                            add_target(sb, pc + 4U, true);
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
                            aot_try_add_target(sb, pc + 4U + (uint32_t)offset, true, 4U);
                        }
                    }
                }

                /* 32-bit Thumb-2 instruction: advance past second halfword */
                pc += 2U;
                continue;
            }

            /* Function prologues: PUSH {..., lr} or SUB sp, #imm */
            if (((hw & 0xFF00U) == 0xB500U || (hw & 0xFF80U) == 0xB080U) &&
                pc <= 0xFFFFFFFCU && pc + 4U <= r_end) {
                add_target(sb, pc, true);
            }
            /* Epilogues: BX lr or POP {..., pc} -> next function entry */
            else if (hw == 0x4770U || (hw & 0xFF00U) == 0xBD00U) {
                for (uint32_t next_fn = pc + 2U;
                     next_fn + 2U <= r_end && next_fn <= pc + 6U;
                     next_fn += 2U) {
                    add_target(sb, next_fn, true);
                    if (*(const uint16_t *)a32_guest_ptr(ctx, next_fn) != 0xBF00U) {
                        break;
                    }
                }
            }
            /* CBZ / CBNZ Rn, label */
            else if ((hw & 0xF500U) == 0xB100U) {
                uint32_t offset = (((hw >> 9U) & 1U) << 6U) | (((hw >> 3U) & 31U) << 1U);
                aot_try_add_target(sb, pc + 4U + offset, true, 2U);
            }
            /* SVC #imm8 -> fallthrough block at pc + 2 */
            else if ((hw & 0xFF00U) == 0xDF00U) {
                if (pc + 4U <= r_end) {
                    add_target(sb, pc + 2U, true);
                }
            }
            /* Branch cond: B<c> */
            else if ((hw & 0xF000U) == 0xD000U && (hw & 0x0E00U) != 0x0E00U) {
                int8_t imm8 = (int8_t)(hw & 0xFFU);
                uint32_t tgt = (uint32_t)((int32_t)pc + 4 + ((int32_t)imm8 << 1));
                aot_try_add_target(sb, tgt, true, 4U);
            }
            /* Branch uncond: B */
            else if ((hw & 0xF800U) == 0xE000U) {
                int32_t imm11 = (int32_t)(hw & 0x7FFU);
                if ((imm11 & 0x400) != 0) {
                    imm11 |= ~0x7FF;
                }
                uint32_t tgt = (uint32_t)((int32_t)pc + 4 + (imm11 << 1));
                aot_try_add_target(sb, tgt, true, 4U);
            }
        }
    } else {
        uint32_t scan_start = (r_start + 3U) & ~3U;
        for (uint32_t pc = scan_start;
             pc + 4U <= r_end && sb->count < sb->limit;
             pc += 4U) {
            uint32_t insn = *(const uint32_t *)a32_guest_ptr(ctx, pc);

            /* Function prologues: STMDB sp!, {..., lr} or SUB sp, sp, #imm */
            if ((insn >> 28U) != 0xFU &&
                ((insn & 0x0FFF4000U) == 0x092D4000U ||
                 (insn & 0x0FFF0000U) == 0x024D0000U ||
                 insn == 0xE52DE004U)) {
                add_target(sb, pc, false);
            }
            /* SVC #imm24 or return (BX lr / POP {..., pc}) -> fallthrough / next fn at pc + 4 */
            else if ((insn >> 28U) != 0xFU &&
                     ((insn & 0x0F000000U) == 0x0F000000U ||
                      (insn & 0x0FFFFFFFU) == 0x012FFF1EU ||
                      (insn & 0x0E108000U) == 0x08108000U)) {
                if (pc + 8U <= r_end) {
                    add_target(sb, pc + 4U, false);
                }
            }
            /* Branch / call: B / BL */
            else if ((insn & 0x0E000000U) == 0x0A000000U) {
                int32_t imm24 = sign_extend_24(insn);
                uint32_t tgt = (uint32_t)((int32_t)pc + 8 + (imm24 << 2));
                aot_try_add_target(sb, tgt, false, 4U);
                if (((insn >> 24U) & 1U) != 0U && pc <= 0xFFFFFFF7U && pc + 8U <= r_end) {
                    add_target(sb, pc + 4U, false);
                }
            }
            /* Branch with Link and Exchange: BLX (imm) */
            else if ((insn & 0xFE000000U) == 0xFA000000U) {
                int32_t imm24 = sign_extend_24(insn);
                uint32_t h   = (insn >> 24U) & 1U;
                uint32_t tgt = (uint32_t)((int32_t)pc + 8 + (imm24 << 2) + (int32_t)(h << 1U));
                aot_try_add_target(sb, tgt, true, 4U);
                if (pc <= 0xFFFFFFF7U && pc + 8U <= r_end) {
                    add_target(sb, pc + 4U, false);
                }
            }
        }
    }
}

static void scan_region_from_entry(aot_scan_buf_t *sb, uint32_t start, uint32_t end,
                                   uint32_t entry, bool is_thumb, size_t budget)
{
    sb->limit = (budget < A32_MAX_AOT_SCAN) ? budget : A32_MAX_AOT_SCAN;
    if (entry > start && entry < end) {
        scan_text_range(sb, entry, end, is_thumb);
        scan_text_range(sb, start, entry, is_thumb);
    } else {
        scan_text_range(sb, start, end, is_thumb);
    }
}

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
    aot_scan_buf_t *sb = (aot_scan_buf_t *)arg;
    if (guest_end_pc > guest_pc && guest_end_pc - guest_pc <= 512U) {
        scan_text_range(sb, guest_pc, guest_end_pc, is_thumb);
    }
    aot_try_add_target(sb, guest_end_pc, is_thumb, is_thumb ? 2U : 4U);
}

int a32_aot_generate(a32_ctx_t *ctx, const a32_elf_info_t *elf_info, const char *elf_path)
{
    if (ctx == NULL || elf_info == NULL || elf_path == NULL) {
        return -1;
    }

    bool     entry_thumb = (elf_info->entry & 1U) != 0U;
    uint32_t entry_pc    = elf_info->entry & (entry_thumb ? ~1U : ~3U);

    if (entry_pc != ctx->elf_base &&
        a32_jit_range_executable(ctx, entry_pc, entry_thumb ? 2U : 4U)) {
        (void)a32_jit_compile_block(ctx, entry_pc, entry_thumb, NULL);
    }

    if (ctx->elf_entry != 0U && ctx->elf_entry != elf_info->entry &&
        (ctx->elf_entry & ~3U) != ctx->elf_base) {
        bool     main_thumb = (ctx->elf_entry & 1U) != 0U;
        uint32_t main_pc    = ctx->elf_entry & (main_thumb ? ~1U : ~3U);
        if (a32_jit_range_executable(ctx, main_pc, main_thumb ? 2U : 4U)) {
            (void)a32_jit_compile_block(ctx, main_pc, main_thumb, NULL);
        }
    }

    uint32_t *disc_pcs   = (uint32_t *)calloc(A32_MAX_AOT_SCAN, sizeof(uint32_t));
    bool     *disc_thumb = (bool *)calloc(A32_MAX_AOT_SCAN, sizeof(bool));
    uint32_t *seen_keys  = (uint32_t *)calloc(65536, sizeof(uint32_t));
    if (disc_pcs == NULL || disc_thumb == NULL || seen_keys == NULL) {
        free(disc_pcs);
        free(disc_thumb);
        free(seen_keys);
        return -1;
    }
    aot_scan_buf_t sb = {
        .ctx       = ctx,
        .pcs       = disc_pcs,
        .thumbs    = disc_thumb,
        .seen_keys = seen_keys,
        .count     = 0U,
        .limit     = A32_MAX_AOT_SCAN,
    };

    seed_elf_symbols(&sb, ctx, elf_path, (entry_pc == ctx->elf_base) ? &entry_thumb : NULL);

    bool has_aux_exec = false;
    for (size_t i = 0U; i < ctx->region_count; i++) {
        const a32_region_t *r = &ctx->regions[i];
        if ((r->prot & 0x4U) == 0U || r->start < A32_LOW_GUARD_SIZE || r->end > A32_KUSER_BASE_32) {
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
        scan_region_from_entry(&sb, r->start, r->end, r_entry, r_thumb, sb.count + 1024U);
    }

    if (elf_info->text_start > 0U && elf_info->text_end > elf_info->text_start) {
        size_t main_budget = has_aux_exec ? (sb.count + 1280U) : (A32_MAX_AOT_SCAN - 512U);
        scan_region_from_entry(&sb, elf_info->text_start, elf_info->text_end,
                               entry_pc, entry_thumb, main_budget);
    }

    size_t compiled_idx = 0U;
    sb.limit            = A32_MAX_AOT_SCAN;
    for (unsigned pass = 0U; pass < 6U; pass++) {
        size_t limit = sb.count;
        for (; compiled_idx < limit; compiled_idx++) {
            uint32_t min_len = sb.thumbs[compiled_idx] ? 2U : 4U;
            if (a32_jit_range_executable(ctx, sb.pcs[compiled_idx], min_len)) {
                (void)a32_jit_compile_block(ctx, sb.pcs[compiled_idx], sb.thumbs[compiled_idx], NULL);
            }
        }
        a32_jit_iter_blocks(collect_end_pc_cb, &sb);
        if (sb.count == limit) {
            break;
        }
    }

    int save_rc = (a32_jit_flush_batch(ctx) == 0) ? a32_aot_save_cache(ctx, elf_path) : -1;
    free(disc_pcs);
    free(disc_thumb);
    free(seen_keys);
    return save_rc;
}
