/*
 * ios_export.c — 32-bit iOS Mach-O export trie and symbol resolution
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#define _GNU_SOURCE
#include "a32rt.h"
#include "ios/ios.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define IOS_EXPORT_REEXPORT 0x08u
#define IOS_EXPORT_STUB_RESOLVER 0x10u

uint8_t *a32_ios_read_blob(const a32_ios_image_t *img, uint32_t off,
                              uint32_t size)
{
    uint32_t sz = size;
    uint8_t *buf;
    if (sz == 0u || sz > 0x10000000U) {
        return NULL;
    }
    if ((uint64_t)off + (uint64_t)sz > img->slice_size) {
        return NULL;
    }
    buf = malloc((size_t)sz + 1U);
    if (buf == NULL) {
        return NULL;
    }
    if (a32_ios_pread_full(img->fd, buf, sz, img->slice_off + off) != 0) {
        free(buf);
        return NULL;
    }
    buf[sz] = 0;
    return buf;
}

bool a32_ios_rd_uleb(const uint8_t *buf, size_t size, size_t *pos,
                        uint64_t *out)
{
    uint64_t v = 0;
    unsigned shift = 0;
    while (*pos < size) {
        uint8_t b = buf[(*pos)++];
        if (shift >= 64u && (b & 0x7Fu) != 0u) {
            return false;
        }
        v |= ((uint64_t)(b & 0x7Fu)) << shift;
        shift += 7u;
        if ((b & 0x80u) == 0u) {
            *out = v;
            return true;
        }
        if (shift >= 70u) {
            return false;
        }
    }
    return false;
}

bool a32_ios_rd_sleb(const uint8_t *buf, size_t size, size_t *pos,
                        int64_t *out)
{
    uint64_t u = 0;
    int64_t v;
    unsigned shift = 0;
    uint8_t b = 0;
    while (*pos < size) {
        b = buf[(*pos)++];
        if (shift >= 64u && (b & 0x7Fu) != 0u) {
            return false;
        }
        u |= ((uint64_t)(b & 0x7Fu)) << shift;
        shift += 7u;
        if ((b & 0x80u) == 0u) {
            break;
        }
        if (shift >= 70u) {
            return false;
        }
    }
    if (*pos > size || shift == 0u) {
        return false;
    }
    if (shift < 64u && (b & 0x40u) != 0u) {
        u |= (~0ULL << shift);
    }
    v = (int64_t)u;
    *out = v;
    return true;
}

/* ---- Export-trie lookup ---- */

typedef struct ios_trie_hit {
    bool hit;
    uint32_t flags;
    uint64_t arg0;
    uint64_t arg1;
    char import_name[512];
} ios_trie_hit_t;

static bool ios_trie_str(const uint8_t *buf, size_t size, size_t *pos,
                         const char **out_edge, size_t *out_len)
{
    size_t start = *pos;
    size_t len;
    while (*pos < size && buf[*pos] != 0u) {
        (*pos)++;
    }
    if (*pos >= size) {
        return false;
    }
    len = *pos - start;
    *out_edge = (const char *)(buf + start);
    *out_len = len;
    (*pos)++;
    return true;
}

static bool ios_trie_walk(const uint8_t *b, size_t n, const char *name,
                          ios_trie_hit_t *hit)
{
    size_t pos = 0;
    const char *rem = name;
    unsigned hops = 0;
    hit->hit = false;
    if (n == 0u) {
        return false;
    }
    for (;;) {
        uint64_t tsize = 0;
        uint64_t nchild = 0;
        uint64_t i;
        size_t term_start;
        size_t rem_len;
        bool advanced = false;
        if (++hops > 512U) {
            return false;
        }
        if (!a32_ios_rd_uleb(b, n, &pos, &tsize)) {
            return false;
        }
        term_start = pos;
        rem_len = strlen(rem);
        if (tsize > 0u && rem_len == 0u) {
            uint64_t flags = 0;
            if (!a32_ios_rd_uleb(b, n, &pos, &flags)) {
                return false;
            }
            hit->flags = (uint32_t)flags;
            if ((flags & IOS_EXPORT_REEXPORT) != 0u) {
                uint64_t ord = 0;
                const char *edge = NULL;
                size_t ilen = 0;
                if (!a32_ios_rd_uleb(b, n, &pos, &ord)) {
                    return false;
                }
                if (!ios_trie_str(b, n, &pos, &edge, &ilen)) {
                    return false;
                }
                if (ilen >= sizeof hit->import_name) {
                    return false;
                }
                memcpy(hit->import_name, edge, ilen);
                hit->import_name[ilen] = '\0';
                hit->arg0 = ord;
                hit->hit = true;
                return true;
            }
            if ((flags & IOS_EXPORT_STUB_RESOLVER) != 0u) {
                uint64_t stub = 0;
                uint64_t res = 0;
                if (!a32_ios_rd_uleb(b, n, &pos, &stub) ||
                    !a32_ios_rd_uleb(b, n, &pos, &res)) {
                    return false;
                }
                hit->arg0 = stub;
                hit->arg1 = res;
                hit->hit = true;
                return true;
            }
            if (!a32_ios_rd_uleb(b, n, &pos, &hit->arg0)) {
                return false;
            }
            hit->hit = true;
            return true;
        }
        pos = term_start + (size_t)tsize;
        if (pos > n) {
            return false;
        }
        if (!a32_ios_rd_uleb(b, n, &pos, &nchild)) {
            return false;
        }
        for (i = 0; i < nchild; i++) {
            size_t elen = 0;
            const char *edge = NULL;
            uint64_t coff = 0;
            if (!ios_trie_str(b, n, &pos, &edge, &elen)) {
                return false;
            }
            if (elen == 0u) {
                return false;
            }
            if (!a32_ios_rd_uleb(b, n, &pos, &coff)) {
                return false;
            }
            if (!advanced && elen <= rem_len &&
                strncmp(rem, edge, elen) == 0 && coff < n) {
                rem += elen;
                pos = (size_t)coff;
                advanced = true;
            }
        }
        if (!advanced) {
            return false;
        }
    }
}

/* ---- Per-image cached blobs (A32-IOS-P5) ---- */

static uint8_t *ios_get_export(a32_ios_image_t *img)
{
    if (img->cached_export != NULL) {
        return img->cached_export;
    }
    if (!img->has_dyld_info || img->dyld_info.export_size == 0u) {
        return NULL;
    }
    img->cached_export = a32_ios_read_blob(img, img->dyld_info.export_off,
        img->dyld_info.export_size);
    return img->cached_export;
}

static uint8_t *ios_get_symtab(a32_ios_image_t *img)
{
    uint64_t sym_bytes;
    if (img->cached_symtab != NULL) {
        return img->cached_symtab;
    }
    if (!img->has_symtab || img->symtab.nsyms == 0u) {
        return NULL;
    }
    sym_bytes = (uint64_t)img->symtab.nsyms * (uint64_t)sizeof(ios_nlist32_t);
    if (sym_bytes > img->slice_size || sym_bytes > 0x10000000ULL ||
        sym_bytes > 0xFFFFFFFFULL) {
        return NULL;
    }
    img->cached_symtab = a32_ios_read_blob(img, img->symtab.symoff,
        (uint32_t)sym_bytes);
    return img->cached_symtab;
}

static uint8_t *ios_get_strtab(a32_ios_image_t *img)
{
    if (img->cached_strtab != NULL) {
        return img->cached_strtab;
    }
    if (!img->has_symtab || img->symtab.strsize == 0u) {
        return NULL;
    }
    img->cached_strtab = a32_ios_read_blob(img, img->symtab.stroff,
        img->symtab.strsize);
    return img->cached_strtab;
}

void a32_ios_free_image_caches(a32_ios_image_t *images, size_t image_count)
{
    size_t i;
    if (images == NULL) {
        return;
    }
    for (i = 0; i < image_count; i++) {
        free(images[i].cached_export);
        images[i].cached_export = NULL;
        free(images[i].cached_symtab);
        images[i].cached_symtab = NULL;
        free(images[i].cached_strtab);
        images[i].cached_strtab = NULL;
    }
}

/* ---- Symbol resolution ---- */

static uint32_t ios_symtab_lookup(a32_ios_image_t *img,
                                  const char *sym_name, bool *found)
{
    uint8_t *syms;
    uint8_t *strs;
    uint32_t i;
    *found = false;
    if (!img->has_symtab || img->symtab.nsyms == 0u) {
        return 0;
    }
    syms = ios_get_symtab(img);
    strs = ios_get_strtab(img);
    if (syms == NULL || strs == NULL) {
        return 0;
    }
    for (i = 0; i < img->symtab.nsyms; i++) {
        const uint8_t *e = syms + (size_t)i * sizeof(ios_nlist32_t);
        uint32_t strx;
        uint8_t n_type;
        uint16_t n_desc;
        uint32_t n_value;
        memcpy(&strx, e, 4);
        n_type = e[4];
        n_desc = (uint16_t)e[6] | ((uint16_t)e[7] << 8);
        memcpy(&n_value, e + 8, 4);
        if ((n_type & IOS_N_STAB) != 0u || (n_type & IOS_N_EXT) == 0u ||
            (n_type & IOS_N_TYPE) != IOS_N_SECT) {
            continue;
        }
        if (strx >= img->symtab.strsize) {
            continue;
        }
        if (strcmp((const char *)(strs + strx), sym_name) != 0) {
            continue;
        }
        *found = true;
        return (n_value + img->slide) |
               (((n_desc & IOS_N_ARM_THUMB_DEF) != 0u) ? 1u : 0u);
    }
    return 0;
}

uint32_t a32_ios_resolve_in(a32_ctx_t *ctx, a32_ios_image_t *images,
                               size_t image_count, size_t img_idx,
                               const char *sym_name, unsigned depth,
                               bool *out_found)
{
    a32_ios_image_t *img;
    uint8_t *blob = NULL;
    ios_trie_hit_t hit;
    uint32_t addr;
    *out_found = false;
    if (depth > 8u || img_idx >= image_count || sym_name[0] == '\0') {
        return 0;
    }
    img = &images[img_idx];
    if (img->is_synthetic) {
        return a32_ios_lookup_synth_sym(ctx, sym_name, out_found);
    }
    if (img->has_dyld_info && img->dyld_info.export_size > 0u) {
        blob = ios_get_export(img);
        if (blob != NULL) {
            bool ok = ios_trie_walk(blob, img->dyld_info.export_size, sym_name,
                                    &hit);
            if (ok && hit.hit) {
                if ((hit.flags & IOS_EXPORT_REEXPORT) != 0u) {
                    uint64_t ord = hit.arg0;
                    const char *next = hit.import_name[0] != '\0'
                                           ? hit.import_name
                                           : sym_name;
                    int dep_idx = -1;
                    if (ord != 0u && ord <= (uint64_t)img->dep_count) {
                        dep_idx = img->dep_image_idx[ord - 1u];
                    }
                    if (dep_idx < 0) {
                        return a32_ios_lookup_synth_sym(ctx, next, out_found);
                    }
                    return a32_ios_resolve_in(ctx, images, image_count,
                        (size_t)dep_idx, next, depth + 1u, out_found);
                }
                if ((hit.flags & 0x3u) == 0x2u) {
                    addr = (uint32_t)hit.arg0;
                } else if ((hit.flags & IOS_EXPORT_STUB_RESOLVER) != 0u) {
                    addr = img->mh_va + (uint32_t)hit.arg0;
                } else {
                    addr = img->mh_va + (uint32_t)hit.arg0;
                }
                *out_found = true;
                return addr;
            }
        }
    }
    addr = ios_symtab_lookup(img, sym_name, out_found);
    if (*out_found) {
        return addr;
    }
    return a32_ios_lookup_synth_sym(ctx, sym_name, out_found);
}
