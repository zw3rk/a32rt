/*
 * ios_bind.c — 32-bit iOS Mach-O rebase and bind interpreter
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

#define IOS_REBASE_DONE 0x00u
#define IOS_REBASE_SET_TYPE_IMM 0x10u
#define IOS_REBASE_SET_SEG_OFF_ULEB 0x20u
#define IOS_REBASE_ADD_ADDR_ULEB 0x30u
#define IOS_REBASE_ADD_ADDR_IMM_SCALED 0x40u
#define IOS_REBASE_DO_TIMES 0x50u
#define IOS_REBASE_DO_ULEB_TIMES 0x60u
#define IOS_REBASE_DO_ADD_ADDR_ULEB 0x70u
#define IOS_REBASE_DO_TIMES_SKIPPING 0x80u

#define IOS_BIND_DONE 0x00u
#define IOS_BIND_SET_ORD_IMM 0x10u
#define IOS_BIND_SET_ORD_ULEB 0x20u
#define IOS_BIND_SET_SPECIAL_IMM 0x30u
#define IOS_BIND_SET_SYM_TRAILING 0x40u
#define IOS_BIND_SET_TYPE_IMM 0x50u
#define IOS_BIND_SET_ADDEND_SLEB 0x60u
#define IOS_BIND_SET_SEG_OFF_ULEB 0x70u
#define IOS_BIND_ADD_ADDR_ULEB 0x80u
#define IOS_BIND_DO_BIND 0x90u
#define IOS_BIND_DO_ADD_ADDR_ULEB 0xA0u
#define IOS_BIND_DO_ADD_ADDR_IMM 0xB0u
#define IOS_BIND_DO_TIMES_SKIPPING 0xC0u

#define IOS_INDIRECT_ABS 0x40000000u
#define IOS_INDIRECT_LOCAL 0x80000000u
#define IOS_BIND_FLAG_WEAK_IMPORT 0x01u
#define IOS_BIND_FLAG_NON_WEAK_DEF 0x08u

static bool ios_addr_ok(const a32_ios_image_t *img, uint32_t va)
{
    size_t k;
    for (k = 0; k < img->seg_count; k++) {
        uint32_t start;
        if (img->segs[k].is_pagezero || img->segs[k].vmsize == 0u) {
            continue;
        }
        start = a32_page_down(img->segs[k].vmaddr);
        if (va >= start && va + 4u > va &&
            va + 4u <= start + img->segs[k].vmsize) {
            return true;
        }
    }
    return false;
}

static bool ios_addr_ok_any(const a32_ios_image_t *images, size_t count,
                            uint32_t va)
{
    size_t k;
    for (k = 0; k < count; k++) {
        if (!images[k].is_synthetic && ios_addr_ok(&images[k], va)) {
            return true;
        }
    }
    return false;
}

/* Guest slot pointer for segs[seg_idx].vmaddr + off; NULL if out of range. */
static uint32_t *ios_slot_ptr(const a32_ctx_t *ctx, const a32_ios_image_t *img,
                              uint32_t seg_idx, uint64_t seg_off)
{
    uint32_t start;
    uint64_t end;
    uint64_t va;
    if (seg_idx >= (uint32_t)img->seg_count) {
        return NULL;
    }
    if (img->segs[seg_idx].is_pagezero || img->segs[seg_idx].vmsize == 0u) {
        return NULL;
    }
    if (seg_off + 4u > (uint64_t)img->segs[seg_idx].vmsize) {
        return NULL;
    }
    start = a32_page_down(img->segs[seg_idx].vmaddr);
    end = (uint64_t)start + (uint64_t)img->segs[seg_idx].vmsize;
    va = (uint64_t)start + seg_off;
    if (va < (uint64_t)start || va + 4u > end) {
        return NULL;
    }
    return (uint32_t *)a32_guest_ptr(ctx, (uint32_t)va);
}

/* ---- Rebase ---- */

static int ios_run_rebase(a32_ctx_t *ctx, a32_ios_image_t *img)
{
    uint8_t *blob;
    size_t pos = 0;
    size_t size;
    uint32_t seg_idx = 0;
    uint64_t seg_off = 0;
    if (img->slide == 0u) {
        return 0;
    }
    blob = a32_ios_read_blob(img, img->dyld_info.rebase_off,
                         img->dyld_info.rebase_size);
    if (blob == NULL) {
        fprintf(stderr, "a32rt: %s: cannot read rebase opcodes\n", img->path);
        return -1;
    }
    size = img->dyld_info.rebase_size;
    while (pos < size) {
        uint8_t op = blob[pos++];
        uint8_t code = (uint8_t)(op & 0xF0u);
        uint8_t imm = (uint8_t)(op & 0x0Fu);
        uint64_t u = 0;
        switch (code) {
        case IOS_REBASE_DONE:
            pos = size;
            break;
        case IOS_REBASE_SET_TYPE_IMM:
            break;
        case IOS_REBASE_SET_SEG_OFF_ULEB:
            seg_idx = imm;
            if (!a32_ios_rd_uleb(blob, size, &pos, &seg_off)) {
                goto fail;
            }
            break;
        case IOS_REBASE_ADD_ADDR_ULEB:
            if (!a32_ios_rd_uleb(blob, size, &pos, &u)) {
                goto fail;
            }
            seg_off += u;
            break;
        case IOS_REBASE_ADD_ADDR_IMM_SCALED:
            seg_off += (uint64_t)imm * 4u;
            break;
        case IOS_REBASE_DO_TIMES:
            for (u = 0; u < imm; u++) {
                uint32_t *slot = ios_slot_ptr(ctx, img, seg_idx, seg_off);
                uint32_t v;
                if (slot == NULL) {
                    goto fail;
                }
                memcpy(&v, slot, 4);
                v += img->slide;
                memcpy(slot, &v, 4);
                seg_off += 4u;
            }
            break;
        case IOS_REBASE_DO_ULEB_TIMES:
        case IOS_REBASE_DO_ADD_ADDR_ULEB:
        case IOS_REBASE_DO_TIMES_SKIPPING: {
            uint64_t count = 0;
            uint64_t skip = 0;
            uint64_t k;
            if (code == IOS_REBASE_DO_ULEB_TIMES) {
                if (!a32_ios_rd_uleb(blob, size, &pos, &count)) {
                    goto fail;
                }
            } else if (code == IOS_REBASE_DO_ADD_ADDR_ULEB) {
                count = 1;
            } else {
                if (!a32_ios_rd_uleb(blob, size, &pos, &count) ||
                    !a32_ios_rd_uleb(blob, size, &pos, &skip) ||
                    count > 0x1000000ULL || skip > 0xFFFFFFFFULL) {
                    goto fail;
                }
            }
            for (k = 0; k < count; k++) {
                uint32_t *slot = ios_slot_ptr(ctx, img, seg_idx, seg_off);
                uint32_t v;
                if (slot == NULL) {
                    goto fail;
                }
                memcpy(&v, slot, 4);
                v += img->slide;
                memcpy(slot, &v, 4);
                seg_off += 4u;
                if (code == IOS_REBASE_DO_TIMES_SKIPPING) {
                    seg_off += skip;
                }
            }
            if (code == IOS_REBASE_DO_ADD_ADDR_ULEB) {
                if (!a32_ios_rd_uleb(blob, size, &pos, &u)) {
                    goto fail;
                }
                seg_off += u;
            }
            break;
        }
        default:
            goto fail;
        }
    }
    free(blob);
    return 0;
fail:
    fprintf(stderr, "a32rt: %s: bad rebase opcodes\n", img->path);
    free(blob);
    return -1;
}

static int ios_run_local_relocs(a32_ctx_t *ctx, a32_ios_image_t *img)
{
    uint8_t *blob;
    uint32_t i;
    uint64_t loc_bytes;
    if (img->slide == 0u) {
        return 0;
    }
    loc_bytes = (uint64_t)img->dysymtab.nlocrel * 8ULL;
    if (loc_bytes > img->slice_size || loc_bytes > 0x10000000ULL) {
        fprintf(stderr, "a32rt: %s: bad local reloc table\n", img->path);
        return -1;
    }
    blob = a32_ios_read_blob(img, img->dysymtab.locreloff, (uint32_t)loc_bytes);
    if (blob == NULL) {
        fprintf(stderr, "a32rt: %s: cannot read local relocs\n", img->path);
        return -1;
    }
    for (i = 0; i < img->dysymtab.nlocrel; i++) {
        const uint8_t *e = blob + (size_t)i * 8u;
        uint32_t addr;
        uint32_t va;
        uint32_t v;
        memcpy(&addr, e, 4);
        if ((addr & 0x80000000u) != 0u) {
            continue;
        }
        va = img->mh_va + addr;
        if (!ios_addr_ok(img, va)) {
            free(blob);
            fprintf(stderr, "a32rt: %s: local reloc out of range\n", img->path);
            return -1;
        }
        memcpy(&v, a32_guest_ptr(ctx, va), 4);
        v += img->slide;
        memcpy(a32_guest_ptr(ctx, va), &v, 4);
    }
    free(blob);
    return 0;
}

/* ---- Bind ---- */

typedef struct ios_bind_state {
    uint32_t seg_idx;
    uint64_t seg_off;
    int64_t ordinal;
    bool ordinal_is_special;
    bool have_sym;
    char sym[512];
    int64_t addend;
    uint32_t sym_flags;
} ios_bind_state_t;

/* Resolve the bind target. -1 = weak-missing (write 0), -2 = fatal,
 * -4 = flat lookup, -5 = weak flat lookup, else image index. */
static int ios_bind_target(const a32_ios_image_t *img, size_t img_idx,
                           const ios_bind_state_t *st, size_t image_count)
{
    (void)image_count;
    if (st->ordinal_is_special) {
        if (st->ordinal == 0) {
            return (int)img_idx;
        }
        if (st->ordinal == -1) {
            return 0;
        }
        if (st->ordinal == -2) {
            return -4;
        }
        if (st->ordinal == -3) {
            return -5;
        }
        return -2;
    }
    if (st->ordinal == 0) {
        return (int)img_idx;
    }
    if (st->ordinal < 0 ||
        st->ordinal > (int64_t)A32_IOS_MAX_DEPS ||
        (uint64_t)st->ordinal > (uint64_t)img->dep_count) {
        return -2;
    }
    return img->dep_image_idx[st->ordinal - 1];
}

static int ios_bind_once(a32_ctx_t *ctx, a32_ios_image_t *images,
    size_t image_count, size_t img_idx, const ios_bind_state_t *st, bool is_weak)
{
    a32_ios_image_t *img = &images[img_idx];
    int target;
    uint32_t sym_addr = 0;
    bool found = false;
    bool weak;
    uint32_t *slot;
    uint32_t val = 0;
    slot = ios_slot_ptr(ctx, img, st->seg_idx, st->seg_off);
    if (slot == NULL) {
        fprintf(stderr, "a32rt: %s: bind slot out of range\n", img->path);
        return -1;
    }
    if (!st->have_sym || st->sym[0] == '\0') {
        fprintf(stderr, "a32rt: %s: bind without symbol\n", img->path);
        return -1;
    }
    if (is_weak && (st->sym_flags & IOS_BIND_FLAG_NON_WEAK_DEF) != 0u) {
        return 0;
    }
    weak = is_weak || ((st->sym_flags & IOS_BIND_FLAG_WEAK_IMPORT) != 0u);
    target = ios_bind_target(img, img_idx, st, image_count);
    if (target == -5) {
        weak = true;
        target = -4;
    }
    if (target == -1) {
        weak = true;
    }
    if (!weak && !st->ordinal_is_special && target >= 0 &&
        st->ordinal > 0 && (uint64_t)st->ordinal <= (uint64_t)img->dep_count &&
        img->dep_weak[st->ordinal - 1] &&
        img->dep_image_idx[st->ordinal - 1] < 0) {
        weak = true;
    }
    if (target == -1) {
        val = 0;
        memcpy(slot, &val, 4);
        return 0;
    }
    if (target == -4) {
        size_t k;
        for (k = 0; k < image_count; k++) {
            sym_addr = a32_ios_resolve_in(ctx, images, image_count, k,
                st->sym, 0u, &found);
            if (found) {
                break;
            }
        }
        if (!found) {
            sym_addr = a32_ios_lookup_synth_sym(ctx, st->sym, &found);
        }
    } else {
        if (target < 0 || (size_t)target >= image_count) {
            fprintf(stderr, "a32rt: %s: bad bind ordinal for '%s'\n",
                    img->path, st->sym);
            return -1;
        }
        if (is_weak) {
            size_t k;
            for (k = 0; k < image_count; k++) {
                sym_addr = a32_ios_resolve_in(ctx, images, image_count, k,
                    st->sym, 0u, &found);
                if (found) {
                    break;
                }
            }
        } else {
            sym_addr = a32_ios_resolve_in(ctx, images, image_count,
                (size_t)target, st->sym, 0u, &found);
        }
    }
    if (!found) {
        if (weak) {
            val = 0;
            memcpy(slot, &val, 4);
            return 0;
        }
        fprintf(stderr, "a32rt: %s: cannot resolve '%s'\n", img->path, st->sym);
        return -1;
    }
    val = sym_addr + (uint32_t)(int32_t)st->addend;
    memcpy(slot, &val, 4);
    return 0;
}

static int ios_run_bind_stream(a32_ctx_t *ctx, a32_ios_image_t *images,
                               size_t image_count, size_t img_idx,
                               const uint8_t *buf, uint32_t size, bool is_lazy,
                               bool is_weak)
{
    ios_bind_state_t st;
    size_t pos = 0;
    memset(&st, 0, sizeof st);
    while (pos < (size_t)size) {
        uint8_t op = buf[pos++];
        uint8_t code = (uint8_t)(op & 0xF0u);
        uint8_t imm = (uint8_t)(op & 0x0Fu);
        uint64_t u = 0;
        int64_t s = 0;
        switch (code) {
        case IOS_BIND_DONE:
            if (is_lazy) {
                break;
            }
            return 0;
        case IOS_BIND_SET_ORD_IMM:
            st.ordinal = (int64_t)imm;
            st.ordinal_is_special = false;
            break;
        case IOS_BIND_SET_ORD_ULEB:
            if (!a32_ios_rd_uleb(buf, size, &pos, &u)) {
                goto fail;
            }
            st.ordinal = (int64_t)u;
            st.ordinal_is_special = false;
            break;
        case IOS_BIND_SET_SPECIAL_IMM:
            if (imm == 0u) {
                st.ordinal = 0;
            } else {
                st.ordinal = (int64_t)(int8_t)(uint8_t)(0xF0u | imm);
            }
            st.ordinal_is_special = true;
            break;
        case IOS_BIND_SET_SYM_TRAILING: {
            size_t len = 0;
            st.sym_flags = imm;
            st.have_sym = false;
            while (pos < (size_t)size && buf[pos] != 0u) {
                if (len + 1u < sizeof st.sym) {
                    st.sym[len] = (char)buf[pos];
                }
                len++;
                pos++;
            }
            if (pos >= (size_t)size) {
                goto fail;
            }
            pos++;
            if (len >= sizeof st.sym) {
                goto fail;
            }
            st.sym[len] = '\0';
            st.have_sym = true;
            break;
        }
        case IOS_BIND_SET_TYPE_IMM:
            break;
        case IOS_BIND_SET_ADDEND_SLEB:
            if (!a32_ios_rd_sleb(buf, size, &pos, &s)) {
                goto fail;
            }
            st.addend = s;
            break;
        case IOS_BIND_SET_SEG_OFF_ULEB:
            st.seg_idx = imm;
            if (!a32_ios_rd_uleb(buf, size, &pos, &u)) {
                goto fail;
            }
            st.seg_off = u;
            break;
        case IOS_BIND_ADD_ADDR_ULEB:
            if (!a32_ios_rd_uleb(buf, size, &pos, &u)) {
                goto fail;
            }
            st.seg_off += u;
            break;
        case IOS_BIND_DO_BIND:
            if (ios_bind_once(ctx, images, image_count, img_idx, &st,
                              is_weak) != 0) {
                return -1;
            }
            st.seg_off += 4u;
            break;
        case IOS_BIND_DO_ADD_ADDR_ULEB:
            if (ios_bind_once(ctx, images, image_count, img_idx, &st,
                              is_weak) != 0) {
                return -1;
            }
            if (!a32_ios_rd_uleb(buf, size, &pos, &u)) {
                goto fail;
            }
            st.seg_off += 4u + u;
            break;
        case IOS_BIND_DO_ADD_ADDR_IMM:
            if (ios_bind_once(ctx, images, image_count, img_idx, &st,
                              is_weak) != 0) {
                return -1;
            }
            st.seg_off += 4u + (uint64_t)imm * 4u;
            break;
        case IOS_BIND_DO_TIMES_SKIPPING: {
            uint64_t count = 0;
            uint64_t skip = 0;
            uint64_t k;
            if (!a32_ios_rd_uleb(buf, size, &pos, &count) ||
                !a32_ios_rd_uleb(buf, size, &pos, &skip)) {
                goto fail;
            }
            if (count > 0x1000000ULL) {
                goto fail;
            }
            for (k = 0; k < count; k++) {
                if (ios_bind_once(ctx, images, image_count, img_idx, &st,
                                  is_weak) != 0) {
                    return -1;
                }
                st.seg_off += 4u + skip;
            }
            break;
        }
        default:
            goto fail;
        }
    }
    return 0;
fail:
    fprintf(stderr, "a32rt: %s: bad bind opcodes\n", images[img_idx].path);
    return -1;
}

/* Classic indirect-symbol binding for images without LC_DYLD_INFO. */
static int ios_run_classic_bind(a32_ctx_t *ctx, a32_ios_image_t *images,
                                size_t image_count, size_t img_idx)
{
    a32_ios_image_t *img = &images[img_idx];
    uint8_t *syms = NULL;
    uint8_t *strs = NULL;
    uint8_t *ind = NULL;
    uint8_t *cmds = NULL;
    ios_mach_header_t mh;
    size_t pos = 0;
    uint32_t i;
    int rc = -1;
    if (!img->has_symtab || !img->has_dysymtab) {
        return 0;
    }
    if (img->symtab.nsyms == 0u || img->symtab.strsize == 0u ||
        img->dysymtab.nindirectsyms == 0u) {
        return 0;
    }
    {
        uint64_t sym_bytes =
            (uint64_t)img->symtab.nsyms * (uint64_t)sizeof(ios_nlist32_t);
        uint64_t ind_bytes = (uint64_t)img->dysymtab.nindirectsyms * 4ULL;
        if (sym_bytes > img->slice_size || sym_bytes > 0x10000000ULL ||
            ind_bytes > img->slice_size || ind_bytes > 0x10000000ULL) {
            goto out;
        }
        syms = a32_ios_read_blob(img, img->symtab.symoff, (uint32_t)sym_bytes);
        strs = a32_ios_read_blob(img, img->symtab.stroff, img->symtab.strsize);
        ind = a32_ios_read_blob(img, img->dysymtab.indirectsymoff,
                                (uint32_t)ind_bytes);
    }
    if (syms == NULL || strs == NULL || ind == NULL) {
        goto out;
    }
    if (a32_ios_pread_full(img->fd, &mh, sizeof mh, img->slice_off) != 0) {
        goto out;
    }
    if (mh.magic != IOS_MH_MAGIC || mh.sizeofcmds > 256u * 1024u) {
        goto out;
    }
    cmds = malloc(mh.sizeofcmds);
    if (cmds == NULL) {
        goto out;
    }
    if (a32_ios_pread_full(img->fd, cmds, mh.sizeofcmds,
                       img->slice_off + 28u) != 0) {
        goto out;
    }
    for (i = 0; i < mh.ncmds; i++) {
        uint32_t cmd;
        uint32_t csz;
        uint32_t nsects;
        uint32_t t;
        if (pos + 8u > mh.sizeofcmds) {
            break;
        }
        cmd = (uint32_t)cmds[pos] | ((uint32_t)cmds[pos + 1] << 8) |
              ((uint32_t)cmds[pos + 2] << 16) | ((uint32_t)cmds[pos + 3] << 24);
        csz = (uint32_t)cmds[pos + 4] | ((uint32_t)cmds[pos + 5] << 8) |
              ((uint32_t)cmds[pos + 6] << 16) | ((uint32_t)cmds[pos + 7] << 24);
        if (csz < 8u || (uint64_t)pos + (uint64_t)csz > mh.sizeofcmds) {
            break;
        }
        if (cmd != IOS_LC_SEGMENT || csz < 56u + 4u) {
            pos += (size_t)csz;
            continue;
        }
        nsects = (uint32_t)cmds[pos + 48] | ((uint32_t)cmds[pos + 49] << 8) |
                 ((uint32_t)cmds[pos + 50] << 16) |
                 ((uint32_t)cmds[pos + 51] << 24);
        for (t = 0; t < nsects; t++) {
            size_t sp = pos + 56u + (size_t)t * 68u;
            uint32_t flags;
            uint32_t addr;
            uint32_t size;
            uint32_t base;
            uint64_t w64;
            uint64_t idx64;
            if (sp + 68u > pos + (size_t)csz) {
                break;
            }
            flags = (uint32_t)cmds[sp + 56] | ((uint32_t)cmds[sp + 57] << 8) |
                    ((uint32_t)cmds[sp + 58] << 16) |
                    ((uint32_t)cmds[sp + 59] << 24);
            if ((flags & 0xFFu) != IOS_S_LAZY_SYMBOL_PTRS &&
                (flags & 0xFFu) != IOS_S_NON_LAZY_SYMBOL_PTRS) {
                continue;
            }
            addr = (uint32_t)cmds[sp + 32] | ((uint32_t)cmds[sp + 33] << 8) |
                   ((uint32_t)cmds[sp + 34] << 16) |
                   ((uint32_t)cmds[sp + 35] << 24);
            size = (uint32_t)cmds[sp + 36] | ((uint32_t)cmds[sp + 37] << 8) |
                   ((uint32_t)cmds[sp + 38] << 16) |
                   ((uint32_t)cmds[sp + 39] << 24);
            base = (uint32_t)cmds[sp + 60] | ((uint32_t)cmds[sp + 61] << 8) |
                   ((uint32_t)cmds[sp + 62] << 16) |
                   ((uint32_t)cmds[sp + 63] << 24);
            if ((uint64_t)addr + (uint64_t)size > 0x100000000ULL ||
                ((uint64_t)size >> 2) > (uint64_t)img->dysymtab.nindirectsyms) {
                goto out;
            }
            for (w64 = 0; w64 + 4ULL <= (uint64_t)size; w64 += 4ULL) {
                uint32_t tab_idx;
                uint32_t strx;
                const char *nm;
                uint32_t sym_addr;
                bool found = false;
                uint32_t va =
                    (uint32_t)((uint64_t)addr + (uint64_t)img->slide + w64);
                size_t k;
                idx64 = (uint64_t)base + (w64 >> 2);
                if (idx64 >= (uint64_t)img->dysymtab.nindirectsyms) {
                    continue;
                }
                memcpy(&tab_idx, ind + (size_t)idx64 * 4u, 4);
                if ((tab_idx & IOS_INDIRECT_LOCAL) != 0u) {
                    if ((tab_idx & IOS_INDIRECT_ABS) == 0u) {
                        uint32_t cur;
                        if (!ios_addr_ok(img, va) &&
                            !ios_addr_ok_any(images, image_count, va)) {
                            goto out;
                        }
                        memcpy(&cur, a32_guest_ptr(ctx, va), 4);
                        cur += img->slide;
                        memcpy(a32_guest_ptr(ctx, va), &cur, 4);
                    }
                    continue;
                }
                if (tab_idx == IOS_INDIRECT_ABS) {
                    continue;
                }
                if (tab_idx >= img->symtab.nsyms) {
                    continue;
                }
                memcpy(&strx,
                       syms + (size_t)tab_idx * sizeof(ios_nlist32_t), 4);
                if (strx >= img->symtab.strsize) {
                    continue;
                }
                nm = (const char *)(strs + strx);
                if (memchr(nm, '\0', (size_t)(img->symtab.strsize - strx)) == NULL) {
                    goto out;
                }
                sym_addr = a32_ios_resolve_in(ctx, images, image_count, img_idx,
                                          nm, 0u, &found);
                if (!found) {
                    for (k = 0; k < image_count && !found; k++) {
                        if (k == img_idx) {
                            continue;
                        }
                        sym_addr = a32_ios_resolve_in(ctx, images, image_count, k,
                                                  nm, 0u, &found);
                    }
                }
                if (!found) {
                    continue;
                }
                if (!ios_addr_ok(img, va) && !ios_addr_ok_any(images,
                                                             image_count,
                                                             va)) {
                    goto out;
                }
                memcpy(a32_guest_ptr(ctx, va), &sym_addr, 4);
            }
        }
        pos += (size_t)csz;
    }
    rc = 0;
out:
    free(syms);
    free(strs);
    free(ind);
    free(cmds);
    return rc;
}

int a32_ios_bind_all(a32_ctx_t *ctx, a32_ios_image_t *images,
                     size_t image_count)
{
    size_t i;
    if (ctx == NULL || images == NULL || image_count == 0u ||
        image_count > A32_IOS_MAX_IMAGES) {
        return -1;
    }
    for (i = 0; i < image_count; i++) {
        a32_ios_image_t *img = &images[i];
        if (img->is_synthetic) {
            continue;
        }
        if (img->has_dyld_info && img->dyld_info.rebase_size > 0u) {
            if (ios_run_rebase(ctx, img) != 0) {
                return -1;
            }
        } else if (img->has_dysymtab && img->dysymtab.nlocrel > 0u &&
                   img->slide != 0u) {
            if (ios_run_local_relocs(ctx, img) != 0) {
                return -1;
            }
        }
    }
    for (i = 0; i < image_count; i++) {
        a32_ios_image_t *img = &images[i];
        uint8_t *blob;
        if (img->is_synthetic || !img->has_dyld_info) {
            continue;
        }
        if (img->dyld_info.bind_size > 0u) {
            blob = a32_ios_read_blob(img, img->dyld_info.bind_off,
                                 img->dyld_info.bind_size);
            if (blob == NULL) {
                fprintf(stderr, "a32rt: %s: cannot read bind opcodes\n",
                        img->path);
                return -1;
            }
            if (ios_run_bind_stream(ctx, images, image_count, i, blob,
                                    img->dyld_info.bind_size, false,
                                    false) != 0) {
                free(blob);
                return -1;
            }
            free(blob);
        }
        if (img->dyld_info.lazy_bind_size > 0u) {
            blob = a32_ios_read_blob(img, img->dyld_info.lazy_bind_off,
                                 img->dyld_info.lazy_bind_size);
            if (blob == NULL) {
                fprintf(stderr, "a32rt: %s: cannot read lazy bind opcodes\n",
                        img->path);
                return -1;
            }
            if (ios_run_bind_stream(ctx, images, image_count, i, blob,
                                    img->dyld_info.lazy_bind_size, true,
                                    false) != 0) {
                free(blob);
                return -1;
            }
            free(blob);
        }
        if (img->dyld_info.weak_bind_size > 0u) {
            blob = a32_ios_read_blob(img, img->dyld_info.weak_bind_off,
                                 img->dyld_info.weak_bind_size);
            if (blob == NULL) {
                fprintf(stderr, "a32rt: %s: cannot read weak bind opcodes\n",
                        img->path);
                return -1;
            }
            if (ios_run_bind_stream(ctx, images, image_count, i, blob,
                                    img->dyld_info.weak_bind_size, false,
                                    true) != 0) {
                free(blob);
                return -1;
            }
            free(blob);
        }
    }
    for (i = 0; i < image_count; i++) {
        if (images[i].is_synthetic || images[i].has_dyld_info) {
            continue;
        }
        if (ios_run_classic_bind(ctx, images, image_count, i) != 0) {
            return -1;
        }
    }
    return 0;
}
