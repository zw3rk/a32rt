/*
 * ios_macho.c — 32-bit iOS Mach-O probe, slice picker, and segment mapper
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#define _GNU_SOURCE
#include "a32rt.h"
#include "ios/ios.h"
#include "os/backend.h"
#include "vm/vm32.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define IOS_CMD_HDR_SIZE 8u
#define IOS_MACH_HDR_SIZE 28u
#define IOS_SEG_CMD_SIZE 56u
#define IOS_COPY_CHUNK 8192u
#define IOS_MAX_CMDSZ (256u * 1024u)
#define IOS_MAX_NCMDS 512u
#define IOS_PATH_MAX 1024u

static uint32_t ios_rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t ios_rd_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

int a32_ios_pread_full(int fd, void *buf, size_t count, uint64_t offset)
{
    uint8_t *p = (uint8_t *)buf;
    size_t done = 0;
    while (done < count) {
        ssize_t n = pread(fd, p + done, count - done, (off_t)(offset + done));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

static void ios_copy_str(char *dst, size_t dstsz, const char *src)
{
    size_t n = strlen(src);
    if (n >= dstsz) {
        n = dstsz - 1u;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}




/* ---- Mach-O / Fat detection ---- */

static bool ios_probe_fd(int fd)
{
    uint8_t h[4];
    if (a32_ios_pread_full(fd, h, sizeof h, 0) != 0) {
        return false;
    }
    if (ios_rd_le32(h) == IOS_MH_MAGIC) {
        ios_mach_header_t mh;
        if (a32_ios_pread_full(fd, &mh, sizeof mh, 0) != 0) {
            return false;
        }
        return mh.cputype == IOS_CPU_TYPE_ARM &&
               (mh.filetype == IOS_MH_EXECUTE ||
                mh.filetype == IOS_MH_DYLINKER ||
                mh.filetype == IOS_MH_DYLIB);
    }
    if (h[0] == 0xCAu && h[1] == 0xFEu && h[2] == 0xBAu && h[3] == 0xBEu) {
        uint8_t nb[4];
        uint32_t i;
        uint32_t n;
        if (a32_ios_pread_full(fd, nb, sizeof nb, 4) != 0) {
            return false;
        }
        n = ios_rd_be32(nb);
        if (n == 0u || n > 32u) {
            return false;
        }
        for (i = 0; i < n; i++) {
            uint8_t e[20];
            uint8_t m[4];
            uint32_t off;
            if (a32_ios_pread_full(fd, e, sizeof e, 8u + (uint64_t)i * 20u) != 0) {
                return false;
            }
            if ((int32_t)ios_rd_be32(e) != IOS_CPU_TYPE_ARM) {
                continue;
            }
            off = ios_rd_be32(e + 8);
            if (a32_ios_pread_full(fd, m, sizeof m, off) != 0) {
                continue;
            }
            if (ios_rd_le32(m) == IOS_MH_MAGIC) {
                return true;
            }
        }
    }
    return false;
}

bool a32_ios_is_macho_fd(int fd, const char *path, const a32_ctx_t *ctx)
{
    int use = fd;
    int owned = -1;
    bool res;
    if (use < 0) {
        if (path == NULL) {
            return false;
        }
        use = a32_open_guest_ro(ctx, path);
        if (use < 0) {
            return false;
        }
        owned = use;
    }
    res = ios_probe_fd(use);
    if (owned >= 0) {
        close(owned);
    }
    return res;
}

static uint32_t ios_slice_score(uint32_t subtype)
{
    switch (subtype) {
    case IOS_CPU_SUBTYPE_ARM_V7S:
        return 50u;
    case IOS_CPU_SUBTYPE_ARM_V7:
        return 40u;
    case IOS_CPU_SUBTYPE_ARM_V7F:
    case IOS_CPU_SUBTYPE_ARM_V7K:
    case IOS_CPU_SUBTYPE_ARM_V8:
        return 30u;
    case IOS_CPU_SUBTYPE_ARM_V6:
        return 20u;
    default:
        return 10u;
    }
}

int a32_ios_pick_slice(int fd, uint64_t file_size, uint64_t *out_off,
                          uint64_t *out_size)
{
    uint8_t m[4];
    uint8_t nb[4];
    uint32_t n;
    uint32_t i;
    bool have = false;
    uint32_t best = 0;
    uint64_t boff = 0;
    uint64_t bsz = 0;
    if (a32_ios_pread_full(fd, m, sizeof m, 0) != 0) {
        return -1;
    }
    if (ios_rd_le32(m) == IOS_MH_MAGIC) {
        *out_off = 0;
        *out_size = file_size;
        return 0;
    }
    if (!(m[0] == 0xCAu && m[1] == 0xFEu && m[2] == 0xBAu && m[3] == 0xBEu)) {
        return -1;
    }
    if (a32_ios_pread_full(fd, nb, sizeof nb, 4) != 0) {
        return -1;
    }
    n = ios_rd_be32(nb);
    if (n == 0u || n > 32u) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        uint8_t e[20];
        uint32_t sub;
        uint64_t off;
        uint64_t sz;
        uint32_t score;
        if (a32_ios_pread_full(fd, e, sizeof e, 8u + (uint64_t)i * 20u) != 0) {
            return -1;
        }
        if ((int32_t)ios_rd_be32(e) != IOS_CPU_TYPE_ARM) {
            continue;
        }
        sub = ios_rd_be32(e + 4) & IOS_CPU_SUBTYPE_MASK;
        off = ios_rd_be32(e + 8);
        sz = ios_rd_be32(e + 12);
        score = ios_slice_score(sub);
        if (!have || score > best) {
            have = true;
            best = score;
            boff = off;
            bsz = sz;
        }
    }
    if (!have) {
        return -1;
    }
    if (boff + bsz < boff || boff + bsz > file_size) {
        return -1;
    }
    if (bsz < (uint64_t)sizeof(ios_mach_header_t)) {
        return -1;
    }
    *out_off = boff;
    *out_size = bsz;
    return 0;
}

/* Parsed load-command extras use a32_a32_ios_parsed_t from ios/ios.h. */

static bool ios_parse_thread(const uint8_t *b, uint32_t csz, a32_ios_parsed_t *par)
{
    size_t p = 8;
    size_t end = csz;
    int k;
    while (p + 8u <= end) {
        uint32_t fl = ios_rd_le32(b + p);
        uint32_t ct = ios_rd_le32(b + p + 4);
        if (ct > 256u || p + 8u + (size_t)ct * 4u > end) {
            return false;
        }
        if (fl == 1u && ct >= 17u && !par->has_thread) {
            for (k = 0; k < 16; k++) {
                par->tstate[k] = ios_rd_le32(b + p + 8u + (size_t)k * 4u);
            }
            par->tcpsr = ios_rd_le32(b + p + 8u + 64u);
            par->has_thread = true;
        }
        p += 8u + (size_t)ct * 4u;
    }
    return p == end;
}

/* Copy a load-command-embedded NUL-terminated string; false if unbounded. */
static bool ios_lc_str(const uint8_t *b, uint32_t csz, uint32_t noff,
                       char *dst, size_t dstsz)
{
    size_t avail;
    size_t len;
    if (noff >= csz) {
        return false;
    }
    avail = (size_t)csz - (size_t)noff;
    len = strnlen((const char *)(b + noff), avail);
    if (len >= avail) {
        return false;
    }
    ios_copy_str(dst, dstsz, (const char *)(b + noff));
    return true;
}

static bool ios_seg_is_pagezero(const ios_segment_command_t *sg)
{
    return strncmp(sg->segname, "__PAGEZERO", 16) == 0 ||
           (sg->initprot == 0 && sg->maxprot == 0 && sg->filesize == 0u);
}

static int ios_copy_payload(a32_ctx_t *ctx, int fd, uint64_t slice_off,
                            uint64_t slice_size,
                            const ios_segment_command_t *sg, uint32_t slid_va,
                            const char *label)
{
    uint32_t copy_len = sg->filesize < sg->vmsize ? sg->filesize : sg->vmsize;
    uint64_t base;
    uint32_t done = 0;
    if (copy_len == 0u) {
        return 0;
    }
    if ((uint64_t)sg->fileoff + (uint64_t)copy_len > 0xFFFFFFFFULL) {
        return -1;
    }
    if (sg->filesize > 0u &&
        (uint64_t)sg->fileoff + (uint64_t)sg->filesize > slice_size) {
        return -1;
    }
    if ((uint64_t)sg->fileoff + (uint64_t)copy_len > slice_size) {
        return -1;
    }
    base = slice_off + (uint64_t)sg->fileoff;
    while (done < copy_len) {
        uint8_t chunk[IOS_COPY_CHUNK];
        uint32_t want = copy_len - done;
        if (want > (uint32_t)sizeof chunk) {
            want = (uint32_t)sizeof chunk;
        }
        if (a32_ios_pread_full(fd, chunk, want, base + done) != 0) {
            fprintf(stderr, "a32rt: %s: short read of segment payload\n", label);
            return -1;
        }
        memcpy(a32_guest_ptr(ctx, slid_va + done), chunk, want);
        done += want;
    }
    return 0;
}

int a32_ios_parse_map(a32_ctx_t *ctx, a32_ios_image_t *img, a32_ios_parsed_t *par,
                         uint32_t *dylib_bump)
{
    ios_mach_header_t mh;
    uint8_t *cmds = NULL;
    ios_segment_command_t tsegs[A32_IOS_MAX_SEGMENTS];
    size_t nsegs = 0;
    uint32_t min_va = UINT32_MAX;
    uint64_t max_end = 0;
    uint32_t slide = 0;
    uint32_t prev_end = 0;
    size_t pos = 0;
    uint32_t i;
    size_t s;
    bool bad = false;
    if (a32_ios_pread_full(img->fd, &mh, sizeof mh, img->slice_off) != 0) {
        fprintf(stderr, "a32rt: %s: cannot read mach header\n", img->path);
        return -1;
    }
    if (mh.magic != IOS_MH_MAGIC || mh.cputype != IOS_CPU_TYPE_ARM) {
        fprintf(stderr, "a32rt: %s: not an ARM32 Mach-O slice\n", img->path);
        return -1;
    }
    if (mh.filetype != IOS_MH_EXECUTE && mh.filetype != IOS_MH_DYLIB &&
        mh.filetype != IOS_MH_DYLINKER) {
        fprintf(stderr, "a32rt: %s: unsupported Mach-O filetype %u\n", img->path,
                mh.filetype);
        return -1;
    }
    if (mh.ncmds == 0u || mh.ncmds > IOS_MAX_NCMDS || mh.sizeofcmds == 0u ||
        mh.sizeofcmds > IOS_MAX_CMDSZ) {
        fprintf(stderr, "a32rt: %s: bad load command table\n", img->path);
        return -1;
    }
    if ((uint64_t)mh.sizeofcmds + IOS_MACH_HDR_SIZE > img->slice_size) {
        fprintf(stderr, "a32rt: %s: load commands exceed slice\n", img->path);
        return -1;
    }
    cmds = malloc(mh.sizeofcmds);
    if (cmds == NULL) {
        return -1;
    }
    if (a32_ios_pread_full(img->fd, cmds, mh.sizeofcmds,
                       img->slice_off + IOS_MACH_HDR_SIZE) != 0) {
        fprintf(stderr, "a32rt: %s: cannot read load commands\n", img->path);
        free(cmds);
        return -1;
    }
    for (i = 0; i < mh.ncmds && !bad; i++) {
        uint32_t cmd;
        uint32_t csz;
        const uint8_t *b;
        if (pos + IOS_CMD_HDR_SIZE > mh.sizeofcmds) {
            bad = true;
            break;
        }
        cmd = ios_rd_le32(cmds + pos);
        csz = ios_rd_le32(cmds + pos + 4);
        if (csz < IOS_CMD_HDR_SIZE || (uint64_t)pos + (uint64_t)csz > mh.sizeofcmds) {
            bad = true;
            break;
        }
        b = cmds + pos;
        switch (cmd) {
        case IOS_LC_SEGMENT: {
            ios_segment_command_t sg;
            if (csz < IOS_SEG_CMD_SIZE || nsegs >= A32_IOS_MAX_SEGMENTS) {
                bad = true;
                break;
            }
            memcpy(&sg, b, sizeof sg);
            tsegs[nsegs++] = sg;
            if (!ios_seg_is_pagezero(&sg) && sg.vmsize > 0u) {
                uint64_t e = (uint64_t)sg.vmaddr + (uint64_t)sg.vmsize;
                if (sg.vmaddr < min_va) {
                    min_va = sg.vmaddr;
                }
                if (e > max_end) {
                    max_end = e;
                }
            }
            break;
        }
        case IOS_LC_ENCRYPTION_INFO: {
            if (csz < 20u) {
                bad = true;
                break;
            }
            if (ios_rd_le32(b + 16) != 0u) {
                fprintf(stderr, "a32rt: %s: encrypted binary not supported\n",
                        img->path);
                free(cmds);
                return -1;
            }
            break;
        }
        case IOS_LC_MAIN: {
            if (csz < 24u) {
                bad = true;
                break;
            }
            par->has_main = true;
            par->main_entryoff = ios_rd_le32(b + 8);
            break;
        }
        case IOS_LC_UNIXTHREAD: {
            if (!ios_parse_thread(b, csz, par)) {
                bad = true;
            }
            break;
        }
        case IOS_LC_LOAD_DYLINKER: {
            char tmp[256];
            uint32_t noff;
            if (csz < 12u) {
                bad = true;
                break;
            }
            noff = ios_rd_le32(b + 8);
            if (!ios_lc_str(b, csz, noff, tmp, sizeof tmp)) {
                bad = true;
                break;
            }
            ios_copy_str(par->dylinker, sizeof par->dylinker, tmp);
            break;
        }
        case IOS_LC_LOAD_DYLIB:
        case IOS_LC_LOAD_WEAK_DYLIB:
        case IOS_LC_REEXPORT_DYLIB:
        case IOS_LC_LAZY_LOAD_DYLIB:
        case IOS_LC_LOAD_UPWARD_DYLIB: {
            char tmp[256];
            uint32_t noff;
            if (csz < 12u) {
                bad = true;
                break;
            }
            noff = ios_rd_le32(b + 8);
            if (par->ndeps >= A32_IOS_MAX_DEPS ||
                !ios_lc_str(b, csz, noff, tmp, sizeof tmp)) {
                bad = true;
                break;
            }
            ios_copy_str(par->deps[par->ndeps], sizeof par->deps[0], tmp);
            par->depweak[par->ndeps] = (cmd == IOS_LC_LOAD_WEAK_DYLIB);
            par->ndeps++;
            break;
        }
        case IOS_LC_ID_DYLIB: {
            char tmp[256];
            uint32_t noff;
            if (csz < 12u) {
                bad = true;
                break;
            }
            noff = ios_rd_le32(b + 8);
            if (!ios_lc_str(b, csz, noff, tmp, sizeof tmp)) {
                bad = true;
                break;
            }
            ios_copy_str(img->install_name, sizeof img->install_name, tmp);
            break;
        }
        case IOS_LC_RPATH: {
            char tmp[256];
            uint32_t noff;
            if (csz < 12u) {
                bad = true;
                break;
            }
            noff = ios_rd_le32(b + 8);
            if (!ios_lc_str(b, csz, noff, tmp, sizeof tmp)) {
                bad = true;
                break;
            }
            if (img->rpath_count < A32_IOS_MAX_RPATHS) {
                ios_copy_str(img->rpaths[img->rpath_count],
                             sizeof img->rpaths[0], tmp);
                img->rpath_count++;
            }
            break;
        }
        case IOS_LC_DYLD_INFO:
        case IOS_LC_DYLD_INFO_ONLY: {
            if (csz < sizeof(ios_dyld_info_command_t) || img->has_dyld_info) {
                bad = true;
                break;
            }
            memcpy(&img->dyld_info, b, sizeof img->dyld_info);
            img->has_dyld_info = true;
            break;
        }
        case IOS_LC_SYMTAB: {
            if (csz < sizeof(ios_symtab_command_t) || img->has_symtab) {
                bad = true;
                break;
            }
            memcpy(&img->symtab, b, sizeof img->symtab);
            img->has_symtab = true;
            break;
        }
        case IOS_LC_DYSYMTAB: {
            if (csz < sizeof(ios_dysymtab_command_t) || img->has_dysymtab) {
                bad = true;
                break;
            }
            memcpy(&img->dysymtab, b, sizeof img->dysymtab);
            img->has_dysymtab = true;
            break;
        }
        default:
            break;
        }
        pos += (size_t)csz;
    }
    if (bad || min_va == UINT32_MAX || max_end == 0u ||
        max_end > 0x100000000ULL) {
        fprintf(stderr, "a32rt: %s: no mappable segments\n", img->path);
        free(cmds);
        return -1;
    }
    if (mh.filetype == IOS_MH_EXECUTE) {
        if (((mh.flags & IOS_MH_PIE) != 0u) && min_va == 0u) {
            slide = A32_IOS_PIE_BASE;
        } else if (min_va < 0x1000u) {
            slide = A32_IOS_PIE_BASE - min_va;
        } else {
            slide = 0u;
        }
    } else if (mh.filetype == IOS_MH_DYLIB) {
        uint64_t span = (uint64_t)a32_page_up((uint32_t)(max_end - min_va));
        uint64_t nb;
        if (*dylib_bump < min_va) {
            fprintf(stderr, "a32rt: %s: dylib base collision\n", img->path);
            free(cmds);
            return -1;
        }
        slide = *dylib_bump - min_va;
        nb = (uint64_t)*dylib_bump + span + 0x10000u;
        if (nb > (uint64_t)A32_KUSER_BASE_32) {
            fprintf(stderr, "a32rt: %s: out of dylib address space\n", img->path);
            free(cmds);
            return -1;
        }
        *dylib_bump = (uint32_t)nb;
    } else {
        slide = (min_va == 0u) ? A32_IOS_DYLD_BASE : 0u;
    }
    for (s = 0; s < nsegs; s++) {
        const ios_segment_command_t *sg = &tsegs[s];
        a32_ios_seg_map_t *dst;
        uint64_t slid;
        uint32_t seg_start;
        uint32_t seg_end;
        uint32_t map_start;
        void *res = NULL;
        if (img->seg_count >= A32_IOS_MAX_SEGMENTS) {
            fprintf(stderr, "a32rt: %s: too many segments\n", img->path);
            free(cmds);
            return -1;
        }
        dst = &img->segs[img->seg_count];
        memset(dst, 0, sizeof *dst);
        memcpy(dst->segname, sg->segname, 16);
        dst->segname[16] = '\0';
        dst->is_pagezero = ios_seg_is_pagezero(sg);
        if (dst->is_pagezero || sg->vmsize == 0u) {
            img->seg_count++;
            continue;
        }
        slid = (uint64_t)sg->vmaddr + (uint64_t)slide;
        if (slid + (uint64_t)sg->vmsize > (uint64_t)A32_KUSER_BASE_32) {
            fprintf(stderr, "a32rt: %s: segment out of guest bounds\n", img->path);
            free(cmds);
            return -1;
        }
        seg_start = a32_page_down((uint32_t)slid);
        seg_end = a32_page_up((uint32_t)(slid + (uint64_t)sg->vmsize));
        if (seg_start < 0x1000u || seg_end <= seg_start) {
            fprintf(stderr, "a32rt: %s: bad segment range\n", img->path);
            free(cmds);
            return -1;
        }
        map_start = seg_start;
        if (seg_start < prev_end) {
            if (seg_start < a32_page_down(prev_end)) {
                fprintf(stderr, "a32rt: %s: overlapping segments\n", img->path);
                free(cmds);
                return -1;
            }
            map_start = prev_end;
        }
        if (seg_end > map_start) {
            if (a32_backend_map_anon(a32_guest_ptr(ctx, map_start),
                                    (size_t)(seg_end - map_start),
                                    PROT_READ | PROT_WRITE, 1, 1, 0,
                                    &res) != 0) {
                fprintf(stderr, "a32rt: %s: failed to map segment: %s\n", img->path,
                        strerror(errno));
                free(cmds);
                return -1;
            }
            if (a32_vm_record_region(ctx, map_start, seg_end, 0x3u, 0x12u,
                                     img->path) != 0) {
                fprintf(stderr, "a32rt: %s: failed to record segment\n", img->path);
                free(cmds);
                return -1;
            }
        }
        if (sg->filesize > 0u &&
            (uint64_t)sg->fileoff + (uint64_t)sg->filesize > img->slice_size) {
            fprintf(stderr, "a32rt: %s: segment outside slice\n", img->path);
            free(cmds);
            return -1;
        }
        if (ios_copy_payload(ctx, img->fd, img->slice_off, img->slice_size, sg,
                             (uint32_t)slid, img->path) != 0) {
            free(cmds);
            return -1;
        }
        {
            uint32_t copy_len = sg->filesize < sg->vmsize ? sg->filesize : sg->vmsize;
            if (sg->vmsize > copy_len) {
                memset(a32_guest_ptr(ctx, (uint32_t)slid + copy_len), 0,
                       (size_t)(sg->vmsize - copy_len));
            }
        }
        if (sg->fileoff == 0u && sg->filesize >= IOS_MACH_HDR_SIZE &&
            img->mh_va == 0u) {
            img->mh_va = (uint32_t)slid;
        }
        dst->vmaddr = (uint32_t)slid;
        dst->vmsize = seg_end - seg_start;
        dst->fileoff = sg->fileoff;
        dst->filesize = sg->filesize;
        dst->initprot = (uint32_t)sg->initprot;
        dst->maxprot = (uint32_t)sg->maxprot;
        img->seg_count++;
        if (seg_end > prev_end) {
            prev_end = seg_end;
        }
    }
    img->filetype = mh.filetype;
    img->slide = slide;
    free(cmds);
    return 0;
}

