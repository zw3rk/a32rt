/*
 * src/dbt/dbt_aot_cache.c — Ahead-Of-Time (AOT) disk cache serialization and loading
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#define _GNU_SOURCE
#include "a32rt.h"

#include "dbt/aot/aot.h"
#include "os/darwin_host_bridge.h"

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
#define A32_AOT_VERSION    10U
#define A32_AOT_MAX_BLOCKS 16384U
#define A32_AOT_MAX_WORDS  4096U
#define A32_AOT_MAX_PC_MAP 256U

typedef struct a32_aot_header {
    uint32_t magic;
    uint32_t version;
    uint32_t num_blocks;
    uint32_t load_bias;
    uint32_t ios_flags;  /* bit 0: ctx->is_ios at save time */
    uint32_t regime;     /* a32_vm32_regime_t at save time */
} a32_aot_header_t;

typedef struct a32_aot_block_header {
    uint32_t guest_pc;
    uint32_t guest_end_pc;
    uint32_t is_thumb;
    uint32_t word_count;
    uint32_t pc_map_count;
    uint32_t site_count;
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
#if defined(__APPLE__)
    uint64_t mtn = (uint64_t)st.st_mtimespec.tv_nsec;
#else
    uint64_t mtn = (uint64_t)st.st_mtim.tv_nsec;
#endif
    h = fnv1a_64_step(h, &sz, sizeof(sz));
    h = fnv1a_64_step(h, &mt, sizeof(mt));
    h = fnv1a_64_step(h, &mtn, sizeof(mtn));

    uint8_t buf[16384];
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
        size_t done = 0U;
        size_t len = (size_t)n;
        while (done + 8U <= len) {
            uint64_t w = 0ULL;
            memcpy(&w, buf + done, sizeof(w));
            for (unsigned k = 0U; k < 8U; k++) {
                h ^= (uint64_t)((w >> (k * 8U)) & 0xFFULL);
                h *= 0x100000001b3ULL;
            }
            done += 8U;
        }
        while (done < len) {
            h ^= (uint64_t)buf[done];
            h *= 0x100000001b3ULL;
            done++;
        }
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
    uint64_t fd_hash;
    if (fd < 0) {
        /* G02 bridge has no ELF file: the host personality tag seeds the
         * hash instead so Darwin caches never collide with Linux ones. */
        if (ctx == NULL || ctx->darwin_personality_hash == 0ULL) {
            return 0ULL;
        }
        fd_hash = ctx->darwin_personality_hash;
    } else {
        fd_hash = compute_fd_hash(fd);
        close(fd);
        if (fd_hash == 0ULL) {
            return 0ULL;
        }
    }
    if (out_fd_hash != NULL) {
        *out_fd_hash = fd_hash;
    }
    uint64_t h = fd_hash;
    if (ctx != NULL) {
        if (ctx->darwin_personality_hash != 0ULL) {
            h = a32_darwin_mix_hash(h, ctx->darwin_personality_hash);
        }
        h = fnv1a_64_step(h, &ctx->elf_base, sizeof(ctx->elf_base));
        h = fnv1a_64_step(h, &ctx->interp_base, sizeof(ctx->interp_base));
        h = fnv1a_64_step(h, &ctx->elf_entry, sizeof(ctx->elf_entry));
        /* A32-IOS-D3: the hash covers the raw executable bytes (format
         * agnostic: ELF32 and Mach-O/Fat hash identically cleanly), plus
         * the personality and window regime so Regime A1 vs A2 and ELF
         * vs iOS caches never collide. */
        uint32_t ios_flag = ctx->is_ios ? 1U : 0U;
        h = fnv1a_64_step(h, &ios_flag, sizeof(ios_flag));
        uint32_t regime = (uint32_t)a32_vm32_current_regime();
        h = fnv1a_64_step(h, &regime, sizeof(regime));
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
    int src_fd = open(src_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (src_fd < 0) {
        return;
    }
    char tmp_dst[PATH_MAX + 64];
    int n = snprintf(tmp_dst, sizeof(tmp_dst), "%s.tmp.%d", dst_path, (int)getpid());
    if (n < 0 || (size_t)n >= sizeof(tmp_dst)) {
        close(src_fd);
        return;
    }
    /* Same atomic-replace discipline as a32_aot_save_cache: drop any stale
     * temp, then create exclusively without following symlinks. */
    (void)unlink(tmp_dst);
    int dst_fd =
        open(tmp_dst, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
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
        guest_end_pc <= guest_pc || (guest_end_pc - guest_pc) > 4096U) {
        return false;
    }
    for (size_t i = 0U; i < word_count; i++) {
        uint32_t w = words[i];
        if (((w & 0xFFE00000U) == 0xD2C00000U || (w & 0xFFE00000U) == 0xD2E00000U ||
             (w & 0xFFE00000U) == 0xF2C00000U || (w & 0xFFE00000U) == 0xF2E00000U) &&
            ((w >> 5) & 0xFFFFU) != 0U) {
            if (ctx != NULL) {
                ((a32_ctx_t *)ctx)->aot_abs64_words++;
            }
            return false;
        }
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

    a32_vm32_mem_site_t site_buf[A32_AOT_SITE_PER_BLOCK];
    uint32_t n_sites = 0U;
    (void)a32_aot_site_fetch(guest_pc, site_buf, A32_AOT_SITE_PER_BLOCK, &n_sites);

    a32_aot_block_header_t bh = {
        .guest_pc     = guest_pc,
        .guest_end_pc = guest_end_pc,
        .is_thumb     = (is_thumb ? 1U : 0U) | (is_call ? 2U : 0U),
        .word_count   = (uint32_t)word_count,
        .pc_map_count = (uint32_t)pc_map_count,
        .site_count   = n_sites,
    };

    if (fwrite(&bh, sizeof(bh), 1U, st->f) != 1U ||
        fwrite(words, sizeof(uint32_t), word_count, st->f) != word_count ||
        fwrite(pc_map, sizeof(a32_pc_map_stage_t), pc_map_count, st->f) != pc_map_count ||
        (n_sites > 0U &&
         fwrite(site_buf, sizeof(a32_vm32_mem_site_t), n_sites, st->f) != n_sites)) {
        st->error = true;
        return;
    }
    st->count++;
}

/* Staged per-block site records from the DBT emitters (src/core/). */
extern void a32_emit_sites_flush(void);

int a32_aot_save_cache(a32_ctx_t *ctx, const char *elf_path)
{
    if (elf_path == NULL) {
        return -1;
    }
    /* Drain the emitter-staged site batch for the final block first, so
     * the per-block fetch below observes every translated block. */
    a32_emit_sites_flush();
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
        .ios_flags  = (ctx != NULL && ctx->is_ios) ? 1U : 0U,
        .regime     = (uint32_t)a32_vm32_current_regime(),
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
    /* A32-IOS-D3: Regime A1 vs A2 and ELF vs iOS caches never collide. */
    if (((hdr.ios_flags & 1U) != (ctx->is_ios ? 1U : 0U)) ||
        hdr.regime != (uint32_t)a32_vm32_current_regime()) {
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

    a32_vm32_mem_site_t site_buf[A32_AOT_SITE_PER_BLOCK];
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
            bh.guest_end_pc - bh.guest_pc > 4096U ||
            bh.word_count == 0U || bh.word_count > A32_AOT_MAX_WORDS ||
            bh.pc_map_count >= A32_AOT_MAX_PC_MAP ||
            bh.site_count > A32_AOT_SITE_PER_BLOCK) {
            break;
        }
        if (fread(words_buf, sizeof(uint32_t), bh.word_count, f) != bh.word_count) {
            break;
        }
        if (bh.pc_map_count > 0U &&
            fread(pc_map_buf, sizeof(a32_pc_map_stage_t), bh.pc_map_count, f) != bh.pc_map_count) {
            break;
        }
        if (bh.site_count > 0U &&
            fread(site_buf, sizeof(a32_vm32_mem_site_t), bh.site_count, f) != bh.site_count) {
            break;
        }
        if (delta != 0U && bh.guest_pc < A32_KUSER_BASE_32) {
            bh.guest_pc     += delta;
            bh.guest_end_pc += delta;
            for (uint32_t m = 0U; m < bh.pc_map_count; m++) {
                pc_map_buf[m].guest_pc += delta;
            }
            for (uint32_t s = 0U; s < bh.site_count; s++) {
                /* R1: site_buf[s].guest_pc += delta is a no-op here: records
                 * hold relative guest_pc_delta, rebased via track() below. */
                (void)site_buf[s].guest_pc_delta;
            }
            for (uint32_t w = 0U; w + 1U < bh.word_count; w++) {
                uint32_t w0 = words_buf[w];
                uint32_t w1 = words_buf[w + 1U];
                if ((w0 & 0xFFE00000U) == 0x52A00000U &&
                    (w1 & 0xFFE00000U) == 0x72800000U &&
                    (w0 & 31U) == (w1 & 31U)) {
                    uint32_t orig_pc = (((w0 >> 5U) & 0xFFFFU) << 16U) | ((w1 >> 5U) & 0xFFFFU);
                    if (orig_pc >= a32_min_va(ctx) && orig_pc < A32_KUSER_BASE_32) {
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
        if (bh.site_count > 0U) {
            (void)a32_aot_site_track(bh.guest_pc, site_buf, bh.site_count);
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
    /* A32-IOS-D3: refuse non-regular, foreign-owned, or group/world
     * writable cache files before mapping any bytes. */
    struct stat st;
    if (!(fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
          st.st_uid == geteuid() && (st.st_mode & 0022) == 0)) {
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
