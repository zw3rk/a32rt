/*
 * src/aot.c — Ahead-Of-Time (AOT) pre-translator and disk cache (M2)
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#define _GNU_SOURCE
#include "a32rt.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define A32_AOT_MAGIC      0x41333243U /* "A32C" */
#define A32_AOT_VERSION    2U
#define A32_MAX_AOT_SCAN   256U
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

static uint64_t compute_elf_hash(const char *elf_path)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    if (elf_path != NULL) {
        h = fnv1a_64_step(h, elf_path, strlen(elf_path));
        struct stat st;
        if (stat(elf_path, &st) == 0) {
            uint64_t sz = (uint64_t)st.st_size;
            uint64_t mt = (uint64_t)st.st_mtime;
            h = fnv1a_64_step(h, &sz, sizeof(sz));
            h = fnv1a_64_step(h, &mt, sizeof(mt));
        }
        FILE *f = fopen(elf_path, "rb");
        if (f != NULL) {
            uint8_t buf[4096];
            size_t n = fread(buf, 1U, sizeof(buf), f);
            if (n > 0U) {
                h = fnv1a_64_step(h, buf, n);
            }
            fclose(f);
        }
    }
    return h;
}

static void resolve_cache_path(const a32_ctx_t *ctx, const char *elf_path,
                               char *out_path, size_t out_len)
{
    uint64_t h = compute_elf_hash(elf_path);
    const char *dir = ctx->cache_dir;
    if (dir == NULL || dir[0] == '\0') {
        dir = getenv("A32RT_CACHE");
    }
    if (dir == NULL || dir[0] == '\0') {
        dir = getenv("XDG_CACHE_HOME");
    }
    if (dir != NULL && dir[0] != '\0') {
        (void)mkdir(dir, 0755);
        snprintf(out_path, out_len, "%s/a32rt-cache-%016llx.bin", dir, (unsigned long long)h);
    } else {
        snprintf(out_path, out_len, "/tmp/a32rt-cache-%016llx.bin", (unsigned long long)h);
    }
}

typedef struct aot_save_state {
    FILE    *f;
    uint32_t count;
    bool     error;
} aot_save_state_t;

static void count_block_cb(uint32_t guest_pc, uint32_t guest_end_pc, bool is_thumb,
                           const uint32_t *words, size_t word_count,
                           const a32_pc_map_stage_t *pc_map, size_t pc_map_count,
                           void *arg)
{
    (void)guest_pc;
    (void)guest_end_pc;
    (void)is_thumb;
    (void)words;
    (void)word_count;
    (void)pc_map;
    (void)pc_map_count;
    aot_save_state_t *st = (aot_save_state_t *)arg;
    st->count++;
}

static void write_block_cb(uint32_t guest_pc, uint32_t guest_end_pc, bool is_thumb,
                           const uint32_t *words, size_t word_count,
                           const a32_pc_map_stage_t *pc_map, size_t pc_map_count,
                           void *arg)
{
    aot_save_state_t *st = (aot_save_state_t *)arg;
    if (st->error) {
        return;
    }
    size_t map_cnt = (pc_map != NULL && pc_map_count <= A32_AOT_MAX_PC_MAP) ? pc_map_count : 0U;

    a32_aot_block_header_t bh;
    bh.guest_pc     = guest_pc;
    bh.guest_end_pc = guest_end_pc;
    bh.is_thumb     = is_thumb ? 1U : 0U;
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
}

int a32_aot_save_cache(a32_ctx_t *ctx, const char *elf_path)
{
    if (elf_path == NULL) {
        return -1;
    }
    char cache_path[512];
    resolve_cache_path(ctx, elf_path, cache_path, sizeof(cache_path));

    char tmp_path[540];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", cache_path, (int)getpid());

    FILE *f = fopen(tmp_path, "wb");
    if (f == NULL) {
        return -1;
    }

    aot_save_state_t count_st = { .f = NULL, .count = 0U, .error = false };
    a32_jit_iter_blocks(count_block_cb, &count_st);

    a32_aot_header_t hdr;
    hdr.magic      = A32_AOT_MAGIC;
    hdr.version    = A32_AOT_VERSION;
    hdr.num_blocks = count_st.count;
    hdr.reserved   = 0U;

    if (fwrite(&hdr, sizeof(hdr), 1U, f) != 1U) {
        fclose(f);
        unlink(tmp_path);
        return -1;
    }

    aot_save_state_t write_st = { .f = f, .count = 0U, .error = false };
    a32_jit_iter_blocks(write_block_cb, &write_st);

    if (write_st.error) {
        fclose(f);
        unlink(tmp_path);
        return -1;
    }

    fclose(f);
    if (rename(tmp_path, cache_path) != 0) {
        unlink(tmp_path);
        return -1;
    }
    return 0;
}

int a32_aot_load_cache(a32_ctx_t *ctx, const char *elf_path)
{
    if (elf_path == NULL) {
        return -1;
    }
    char cache_path[512];
    resolve_cache_path(ctx, elf_path, cache_path, sizeof(cache_path));

    FILE *f = fopen(cache_path, "rb");
    if (f == NULL) {
        return -1;
    }

    a32_aot_header_t hdr;
    if (fread(&hdr, sizeof(hdr), 1U, f) != 1U) {
        fclose(f);
        return -1;
    }
    if (hdr.magic != A32_AOT_MAGIC || hdr.version != A32_AOT_VERSION ||
        hdr.num_blocks > A32_AOT_MAX_BLOCKS) {
        fclose(f);
        return -1;
    }

    static uint32_t           words_buf[A32_AOT_MAX_WORDS];
    static a32_pc_map_stage_t pc_map_buf[A32_AOT_MAX_PC_MAP];
    for (uint32_t i = 0U; i < hdr.num_blocks; i++) {
        a32_aot_block_header_t bh;
        if (fread(&bh, sizeof(bh), 1U, f) != 1U) {
            break;
        }
        if (bh.word_count == 0U || bh.word_count > A32_AOT_MAX_WORDS ||
            bh.pc_map_count > A32_AOT_MAX_PC_MAP) {
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
        (void)a32_jit_install_aot_block(ctx, bh.guest_pc, bh.guest_end_pc,
                                        bh.is_thumb != 0U,
                                        words_buf, (size_t)bh.word_count,
                                        bh.pc_map_count > 0U ? pc_map_buf : NULL,
                                        (size_t)bh.pc_map_count);
    }

    fclose(f);
    (void)a32_jit_flush_batch(ctx);
    return 0;
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

int a32_aot_generate(a32_ctx_t *ctx, const a32_elf_info_t *elf_info, const char *elf_path)
{
    if (ctx == NULL || elf_info == NULL || elf_path == NULL) {
        return -1;
    }

    bool     entry_thumb = (elf_info->entry & 1U) != 0U;
    uint32_t entry_pc    = elf_info->entry & (entry_thumb ? ~1U : ~3U);

    (void)a32_jit_compile_block(ctx, entry_pc, entry_thumb, NULL);

    uint32_t disc_pcs[A32_MAX_AOT_SCAN];
    bool     disc_thumb[A32_MAX_AOT_SCAN];
    size_t   disc_count = 0U;

    if (elf_info->text_start > 0U && elf_info->text_end > elf_info->text_start) {
        if (entry_thumb) {
            for (uint32_t pc = elf_info->text_start;
                 pc + 2U <= elf_info->text_end && disc_count < A32_MAX_AOT_SCAN;
                 pc += 2U) {
                const uint16_t *hw_ptr = (const uint16_t *)a32_guest_ptr(ctx, pc);
                uint16_t hw = *hw_ptr;

                if ((hw >> 11U) >= 0x1DU) {
                    /* 32-bit Thumb-2 instruction: advance past second halfword */
                    pc += 2U;
                    continue;
                }

                /* Function prologues: PUSH {..., lr} or SUB sp, #imm */
                if ((hw & 0xFF00U) == 0xB500U || (hw & 0xFF80U) == 0xB080U) {
                    add_target(pc, true, disc_pcs, disc_thumb, &disc_count);
                }
                /* Branch cond: B<c> */
                else if ((hw & 0xF000U) == 0xD000U && (hw & 0x0E00U) != 0x0E00U) {
                    int8_t imm8 = (int8_t)(hw & 0xFFU);
                    uint32_t tgt = (uint32_t)((int32_t)pc + 4 + ((int32_t)imm8 << 1));
                    if (tgt >= elf_info->text_start && tgt < elf_info->text_end) {
                        add_target(tgt, true, disc_pcs, disc_thumb, &disc_count);
                    }
                }
                /* Branch uncond: B */
                else if ((hw & 0xF800U) == 0xE000U) {
                    int32_t imm11 = (int32_t)(hw & 0x7FFU);
                    if ((imm11 & 0x400) != 0) {
                        imm11 |= ~0x7FF;
                    }
                    uint32_t tgt = (uint32_t)((int32_t)pc + 4 + (imm11 << 1));
                    if (tgt >= elf_info->text_start && tgt < elf_info->text_end) {
                        add_target(tgt, true, disc_pcs, disc_thumb, &disc_count);
                    }
                }
            }
        } else {
            for (uint32_t pc = elf_info->text_start;
                 pc + 4U <= elf_info->text_end && disc_count < A32_MAX_AOT_SCAN;
                 pc += 4U) {
                const uint32_t *insn_ptr = (const uint32_t *)a32_guest_ptr(ctx, pc);
                uint32_t insn = *insn_ptr;

                /* Function prologues: STMDB sp!, {..., lr} or SUB sp, sp, #imm */
                if ((insn & 0x0FFF4000U) == 0x092D4000U ||
                    (insn & 0x0FFF0000U) == 0x024D0000U ||
                    insn == 0xE52DE004U) {
                    add_target(pc, false, disc_pcs, disc_thumb, &disc_count);
                }
                /* Branch / call: B / BL */
                else if ((insn & 0x0E000000U) == 0x0A000000U) {
                    int32_t imm24 = (int32_t)(insn & 0x00FFFFFFU);
                    if ((imm24 & 0x00800000) != 0) {
                        imm24 |= (int32_t)0xFF000000U;
                    }
                    uint32_t tgt = (uint32_t)((int32_t)pc + 8 + (imm24 << 2));
                    if (tgt >= elf_info->text_start && tgt < elf_info->text_end) {
                        add_target(tgt, false, disc_pcs, disc_thumb, &disc_count);
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
                    if (tgt >= elf_info->text_start && tgt < elf_info->text_end) {
                        add_target(tgt, true, disc_pcs, disc_thumb, &disc_count);
                    }
                }
            }
        }
    }

    for (size_t i = 0U; i < disc_count; i++) {
        (void)a32_jit_compile_block(ctx, disc_pcs[i], disc_thumb[i], NULL);
    }

    (void)a32_jit_flush_batch(ctx);
    return a32_aot_save_cache(ctx, elf_path);
}
