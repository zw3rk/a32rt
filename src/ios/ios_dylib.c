/*
 * ios_dylib.c — 32-bit iOS standalone dynamic linker: dylib resolution,
 * init collection, protections, dyld handoff, and image loading
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
#define IOS_MAX_CMDSZ (256u * 1024u)
#define IOS_MAX_NCMDS 512u
#define IOS_PATH_MAX 1024u

static uint32_t ios_rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
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

static bool ios_join3(char *dst, size_t dstsz, const char *a, const char *b,
                      const char *c)
{
    size_t na = strlen(a);
    size_t nb = strlen(b);
    size_t nc = strlen(c);
    if (na + nb + nc + 1u > dstsz) {
        return false;
    }
    memcpy(dst, a, na);
    memcpy(dst + na, b, nb);
    memcpy(dst + na + nb, c, nc + 1u);
    return true;
}

static void ios_dirname(const char *p, char *out, size_t outsz)
{
    const char *s = strrchr(p, '/');
    if (s == NULL) {
        ios_copy_str(out, outsz, ".");
    } else if (s == p) {
        ios_copy_str(out, outsz, "/");
    } else {
        size_t n = (size_t)(s - p);
        if (n >= outsz) {
            n = outsz - 1u;
        }
        memcpy(out, p, n);
        out[n] = '\0';
    }
}

/* ---- Dependency resolution (standalone dyld mode) ---- */

static bool ios_is_system_dylib(const char *dep)
{
    static const char *const exact[] = {
        "/usr/lib/libSystem.B.dylib",
        "/usr/lib/libSystem.dylib",
        "/usr/lib/libgcc_s.1.dylib",
        "/usr/lib/libcompiler_rt.dylib",
        "/usr/lib/dyld",
    };
    static const char sys_prefix[] = "/usr/lib/system/lib";
    static const char dylib_suffix[] = ".dylib";
    size_t k;
    for (k = 0; k < sizeof exact / sizeof exact[0]; k++) {
        if (strcmp(dep, exact[k]) == 0) {
            return true;
        }
    }
    if (strncmp(dep, sys_prefix, sizeof sys_prefix - 1u) == 0) {
        size_t n = strlen(dep);
        size_t s = sizeof dylib_suffix - 1u;
        if (n >= s && strcmp(dep + n - s, dylib_suffix) == 0) {
            return true;
        }
    }
    return false;
}

static const char *ios_host_env(const a32_ctx_t *ctx, const char *name)
{
    size_t nl;
    size_t i;
    if (ctx == NULL || ctx->host_envp == NULL || name == NULL) {
        return NULL;
    }
    nl = strlen(name);
    for (i = 0; ctx->host_envp[i] != NULL; i++) {
        if (strncmp(ctx->host_envp[i], name, nl) == 0 &&
            ctx->host_envp[i][nl] == '=') {
            return ctx->host_envp[i] + nl + 1u;
        }
    }
    return NULL;
}

static const char *ios_dep_basename(const char *dep)
{
    const char *s = strrchr(dep, '/');
    return (s != NULL) ? (s + 1u) : dep;
}

/* -2 = not found here (try next candidate), -1 = fatal, else image index. */
static int ios_try_load_candidate(a32_ctx_t *ctx, a32_ios_image_t *images,
                                  a32_ios_parsed_t *parsed, size_t *count,
                                  uint32_t *dylib_bump, const char *cand)
{
    struct stat st;
    size_t ni;
    a32_ios_image_t *img;
    uint64_t file_size;
    ios_mach_header_t mh;
    int fd;
    size_t i;
    for (i = 0; i < *count; i++) {
        if (images[i].is_synthetic) {
            continue;
        }
        if (strcmp(images[i].path, cand) == 0) {
            return (int)i;
        }
        if (images[i].install_name[0] != '\0' &&
            strcmp(images[i].install_name, cand) == 0) {
            return (int)i;
        }
    }
    if (*count >= A32_IOS_MAX_IMAGES) {
        fprintf(stderr, "a32rt: too many Mach-O images\n");
        return -1;
    }
    fd = a32_open_guest_ro(ctx, cand);
    if (fd < 0) {
        fd = open(cand, O_RDONLY | O_CLOEXEC);
    }
    if (fd < 0) {
        return -2;
    }
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return -2;
    }
    if (st.st_ino != 0u) {
        for (i = 0; i < *count; i++) {
            if (images[i].is_synthetic || images[i].st_ino == 0u) {
                continue;
            }
            if (images[i].st_dev == (uint64_t)st.st_dev &&
                images[i].st_ino == (uint64_t)st.st_ino) {
                close(fd);
                return (int)i;
            }
        }
    }
    file_size = (uint64_t)st.st_size;
    ni = *count;
    img = &images[ni];
    memset(img, 0, sizeof *img);
    img->fd = -1;
    if (a32_ios_pick_slice(fd, file_size, &img->slice_off, &img->slice_size) != 0) {
        close(fd);
        return -2;
    }
    if (a32_ios_pread_full(fd, &mh, sizeof mh, img->slice_off) != 0 ||
        mh.magic != IOS_MH_MAGIC || mh.cputype != IOS_CPU_TYPE_ARM ||
        mh.filetype != IOS_MH_DYLIB) {
        close(fd);
        return -2;
    }
    img->fd = fd;
    img->st_dev = (uint64_t)st.st_dev;
    img->st_ino = (uint64_t)st.st_ino;
    ios_copy_str(img->path, sizeof img->path, cand);
    if (a32_ios_parse_map(ctx, img, &parsed[ni], dylib_bump) != 0) {
        close(fd);
        img->fd = -1;
        return -1;
    }
    *count = ni + 1u;
    return (int)ni;
}

static int ios_try_candidate(a32_ctx_t *ctx, a32_ios_image_t *images,
                             a32_ios_parsed_t *parsed, size_t *count,
                             uint32_t *dylib_bump, const char *cand)
{
    if (cand[0] == '\0') {
        return -2;
    }
    return ios_try_load_candidate(ctx, images, parsed, count, dylib_bump, cand);
}

static int ios_try_path_list(a32_ctx_t *ctx, a32_ios_image_t *images,
    a32_ios_parsed_t *parsed, size_t *count, uint32_t *dylib_bump,
    const char *list, const char *base)
{
    char dirs[IOS_PATH_MAX];
    size_t dl, p = 0;
    if (list == NULL || list[0] == '\0' || base[0] == '\0') {
        return -2;
    }
    dl = strlen(list);
    if (dl >= sizeof dirs) {
        dl = sizeof dirs - 1u;
    }
    memcpy(dirs, list, dl);
    dirs[dl] = '\0';
    while (p <= dl) {
        size_t q = p, dn, bn;
        char cand[IOS_PATH_MAX];
        int r;
        while (q < dl && dirs[q] != ':') {
            q++;
        }
        dirs[q] = '\0';
        dn = strlen(dirs + p);
        bn = strlen(base);
        if (dn != 0u && dn + 1u + bn + 1u <= sizeof cand) {
            memcpy(cand, dirs + p, dn);
            cand[dn] = '/';
            memcpy(cand + dn + 1u, base, bn + 1u);
            r = ios_try_candidate(ctx, images, parsed, count, dylib_bump, cand);
            if (r != -2) {
                return r;
            }
        }
        p = q + 1u;
    }
    return -2;
}

/* -2 = missing, -1 = fatal, else image index. */
static int ios_resolve_dep(a32_ctx_t *ctx, a32_ios_image_t *images,
                           a32_ios_parsed_t *parsed, size_t *count,
                           uint32_t *dylib_bump, const char *exec_dir,
                           size_t cur, const char *dep)
{
    static const char exe_prefix[] = "@executable_path/";
    static const char ld_prefix[] = "@loader_path/";
    static const char rp_prefix[] = "@rpath/";
    char cand[IOS_PATH_MAX];
    char loader_dir[IOS_PATH_MAX];
    const char *dyld_path;
    int dlr;
    dyld_path = ios_host_env(ctx, "DYLD_LIBRARY_PATH");
    if (dyld_path != NULL) {
        dlr = ios_try_path_list(ctx, images, parsed, count, dylib_bump,
                                dyld_path, ios_dep_basename(dep));
        if (dlr != -2) {
            return dlr;
        }
    }
    if (strncmp(dep, exe_prefix, sizeof exe_prefix - 1u) == 0) {
        const char *rest = dep + sizeof exe_prefix - 1u;
        int r;
        if (!ios_join3(cand, sizeof cand, exec_dir, "/", rest)) {
            return -2;
        }
        r = ios_try_candidate(ctx, images, parsed, count, dylib_bump, cand);
        return r;
    }
    if (strncmp(dep, ld_prefix, sizeof ld_prefix - 1u) == 0) {
        const char *rest = dep + sizeof ld_prefix - 1u;
        int r;
        ios_dirname(images[cur].path, loader_dir, sizeof loader_dir);
        if (!ios_join3(cand, sizeof cand, loader_dir, "/", rest)) {
            return -2;
        }
        r = ios_try_candidate(ctx, images, parsed, count, dylib_bump, cand);
        return r;
    }
    if (strncmp(dep, rp_prefix, sizeof rp_prefix - 1u) == 0) {
        const char *rest = dep + sizeof rp_prefix - 1u;
        size_t pass;
        for (pass = 0; pass < 2u; pass++) {
            const a32_ios_image_t *rpimg = (pass == 0u) ? &images[cur] : &images[0];
            size_t r;
            for (r = 0; r < rpimg->rpath_count; r++) {
                const char *rp = rpimg->rpaths[r];
                char expanded[IOS_PATH_MAX];
                int lr;
                if (strcmp(rp, "@executable_path") == 0) {
                    ios_copy_str(expanded, sizeof expanded, exec_dir);
                } else if (strncmp(rp, exe_prefix, sizeof exe_prefix - 1u) == 0) {
                    if (!ios_join3(expanded, sizeof expanded, exec_dir, "/",
                                   rp + sizeof exe_prefix - 1u)) {
                        continue;
                    }
                } else if (strcmp(rp, "@loader_path") == 0) {
                    ios_dirname(rpimg->path, expanded, sizeof expanded);
                } else if (strncmp(rp, ld_prefix, sizeof ld_prefix - 1u) == 0) {
                    ios_dirname(rpimg->path, loader_dir, sizeof loader_dir);
                    if (!ios_join3(expanded, sizeof expanded, loader_dir, "/",
                                   rp + sizeof ld_prefix - 1u)) {
                        continue;
                    }
                } else {
                    ios_copy_str(expanded, sizeof expanded, rp);
                }
                if (!ios_join3(cand, sizeof cand, expanded, "/", rest)) {
                    continue;
                }
                lr = ios_try_candidate(ctx, images, parsed, count, dylib_bump,
                                       cand);
                if (lr != -2) {
                    return lr;
                }
            }
        }
        {
            const char *fb = ios_host_env(ctx, "DYLD_FALLBACK_LIBRARY_PATH");
            if (fb != NULL) {
                int fr = ios_try_path_list(ctx, images, parsed, count,
                                           dylib_bump, fb,
                                           ios_dep_basename(dep));
                if (fr != -2) {
                    return fr;
                }
            }
        }
        return -2;
    }
    {
        int r = ios_try_candidate(ctx, images, parsed, count, dylib_bump, dep);
        if (r != -2) {
            return r;
        }
        {
            const char *fb = ios_host_env(ctx, "DYLD_FALLBACK_LIBRARY_PATH");
            if (fb != NULL) {
                return ios_try_path_list(ctx, images, parsed, count,
                                         dylib_bump, fb,
                                         ios_dep_basename(dep));
            }
        }
        return -2;
    }
}

/* ---- Init-pointer collection, protections, and entry setup ---- */

static bool ios_seg_has(const a32_ios_image_t *img, uint32_t va, uint32_t sz,
                        uint32_t need_prot, bool single_range)
{
    size_t k;
    uint64_t end = (uint64_t)va + (uint64_t)sz;
    uint32_t key = va & ~1U;
    if (sz == 0u || end > 0x100000000ULL) {
        return false;
    }
    for (k = 0; k < img->seg_count; k++) {
        uint32_t start, send;
        if (img->segs[k].is_pagezero || img->segs[k].vmsize == 0u) {
            continue;
        }
        if ((img->segs[k].initprot & need_prot) == 0U) {
            continue;
        }
        start = a32_page_down(img->segs[k].vmaddr);
        send = start + img->segs[k].vmsize;
        if (single_range) {
            if (va >= start && end <= (uint64_t)send) {
                return true;
            }
        } else if (key >= start && key < send) {
            return true;
        }
    }
    return false;
}
static bool ios_range_in_readable(const a32_ios_image_t *img, uint32_t va,
    uint32_t sz)
{
    return ios_seg_has(img, va, sz, 1U, true);
}
static bool ios_exec_contains(const a32_ios_image_t *img, uint32_t va)
{
    return ios_seg_has(img, va, 4U, 4U, false);
}
static int ios_collect_inits(a32_ctx_t *ctx, a32_ios_image_t *images,
                             size_t count)
{
    size_t order[A32_IOS_MAX_IMAGES];
    size_t norder = 0;
    size_t oi;
    size_t idx;
    ctx->ios_init_count = 0;
    for (idx = count; idx > 1u; idx--) {
        order[norder++] = idx - 1u;
    }
    if (count > 0u) {
        order[norder++] = 0;
    }
    for (oi = 0; oi < norder; oi++) {
        a32_ios_image_t *img = &images[order[oi]];
        ios_mach_header_t mh;
        uint8_t *cmds;
        size_t pos = 0;
        uint32_t i;
        if (img->is_synthetic) {
            continue;
        }
        if (ctx->ios_init_count >= 64u) {
            return -1;
        }
        if (a32_ios_pread_full(img->fd, &mh, sizeof mh, img->slice_off) != 0) {
            return -1;
        }
        if (mh.magic != IOS_MH_MAGIC || mh.ncmds > IOS_MAX_NCMDS ||
            mh.sizeofcmds == 0u || mh.sizeofcmds > IOS_MAX_CMDSZ) {
            return -1;
        }
        cmds = malloc(mh.sizeofcmds);
        if (cmds == NULL) {
            return -1;
        }
        if (a32_ios_pread_full(img->fd, cmds, mh.sizeofcmds,
                           img->slice_off + IOS_MACH_HDR_SIZE) != 0) {
            free(cmds);
            return -1;
        }
        for (i = 0; i < mh.ncmds; i++) {
            uint32_t cmd;
            uint32_t csz;
            uint32_t nsects;
            uint32_t t;
            if (pos + IOS_CMD_HDR_SIZE > mh.sizeofcmds) {
                break;
            }
            cmd = ios_rd_le32(cmds + pos);
            csz = ios_rd_le32(cmds + pos + 4);
            if (csz < IOS_CMD_HDR_SIZE ||
                (uint64_t)pos + (uint64_t)csz > mh.sizeofcmds) {
                break;
            }
            if (cmd == IOS_LC_SEGMENT && csz >= IOS_SEG_CMD_SIZE + 4u) {
                nsects = ios_rd_le32(cmds + pos + 48);
                for (t = 0; t < nsects; t++) {
                    size_t sp = pos + IOS_SEG_CMD_SIZE + (size_t)t * 68u;
                    uint32_t flags;
                    uint32_t addr;
                    uint32_t size;
                    uint32_t w;
                    if (sp + 68u > pos + (size_t)csz) {
                        break;
                    }
                    flags = ios_rd_le32(cmds + sp + 56);
                    if ((flags & IOS_SECTION_TYPE_MASK) != IOS_S_MOD_INIT_FUNC_PTRS) {
                        continue;
                    }
                    addr = ios_rd_le32(cmds + sp + 32);
                    size = ios_rd_le32(cmds + sp + 36);
                    if ((addr & 3U) != 0U) {
                        free(cmds);
                        return -1;
                    }
                    if (size > 0u) {
                        uint32_t slid_va = addr + img->slide;
                        if (!ios_range_in_readable(img, slid_va, size)) {
                            free(cmds);
                            return -1;
                        }
                        if ((uint64_t)ctx->ios_init_count +
                            (uint64_t)(size / 4u) > 64u) {
                            free(cmds);
                            return -1;
                        }
                    }
                    for (w = 0; w + 4u <= size; w += 4u) {
                        uint32_t va = addr + img->slide + w;
                        uint32_t val;
                        if (ctx->ios_init_count >= 64u) {
                            free(cmds);
                            return -1;
                        }
                        memcpy(&val, a32_guest_ptr(ctx, va), 4);
                        ctx->ios_init_va[ctx->ios_init_count++] = val;
                    }
                }
            }
            pos += (size_t)csz;
        }
        free(cmds);
    }
    return 0;
}

static int ios_apply_prot(a32_ctx_t *ctx, a32_ios_image_t *images, size_t count)
{
    size_t i;
    size_t k;
    for (i = 0; i < count; i++) {
        const a32_ios_image_t *img = &images[i];
        if (img->is_synthetic) {
            continue;
        }
        for (k = 0; k < img->seg_count; k++) {
            uint32_t start;
            uint32_t end;
            if (img->segs[k].is_pagezero || img->segs[k].vmsize == 0u) {
                continue;
            }
            start = a32_page_down(img->segs[k].vmaddr);
            end = a32_page_up(img->segs[k].vmaddr + img->segs[k].vmsize);
            if (a32_vm_mprotect(ctx, start, end - start,
                                img->segs[k].initprot & 0x7u) != 0) {
                fprintf(stderr, "a32rt: %s: failed to seal %.16s: %s\n", img->path,
                        img->segs[k].segname, strerror(errno));
                return -1;
            }
        }
    }
    return 0;
}

static void ios_fill_info(const a32_ios_image_t *images, a32_elf_info_t *out)
{
    size_t k;
    uint32_t exec_max = 0;
    for (k = 0; k < images[0].seg_count; k++) {
        uint32_t start;
        uint32_t end;
        if (images[0].segs[k].is_pagezero || images[0].segs[k].vmsize == 0u) {
            continue;
        }
        start = a32_page_down(images[0].segs[k].vmaddr);
        end = start + images[0].segs[k].vmsize;
        if (strncmp(images[0].segs[k].segname, "__TEXT", 16) == 0) {
            if (out->text_start == 0u || start < out->text_start) {
                out->text_start = start;
            }
            if (end > out->text_end) {
                out->text_end = end;
            }
        }
        if (end > exec_max) {
            exec_max = end;
        }
    }
    out->load_bias = images[0].slide;
    out->max_vaddr = exec_max;
}

static void ios_refine_text_and_fn_starts(a32_ctx_t *ctx,
                                          a32_ios_image_t *images,
                                          a32_elf_info_t *out)
{
    size_t k;
    bool have_exec_text = false;
    uint32_t ex_start = 0;
    uint32_t ex_end = 0;
    for (k = 0; k < images[0].seg_count; k++) {
        if (images[0].segs[k].is_pagezero ||
            images[0].segs[k].vmsize == 0u) {
            continue;
        }
        if (strncmp(images[0].segs[k].segname, "__TEXT", 16) != 0) {
            continue;
        }
        if ((images[0].segs[k].initprot & 4U) == 0U) {
            continue;
        }
        if (!have_exec_text) {
            ex_start = images[0].segs[k].vmaddr;
            ex_end = images[0].segs[k].vmaddr + images[0].segs[k].vmsize;
            have_exec_text = true;
        } else {
            uint32_t s = images[0].segs[k].vmaddr;
            uint32_t e = s + images[0].segs[k].vmsize;
            if (s < ex_start) {
                ex_start = s;
            }
            if (e > ex_end) {
                ex_end = e;
            }
        }
    }
    if (have_exec_text) {
        out->text_start = ex_start;
        out->text_end = ex_end;
    }
    if (images[0].has_fn_starts && images[0].fn_starts.datasize > 0u) {
        uint8_t *blob = a32_ios_read_blob(&images[0],
            images[0].fn_starts.dataoff, images[0].fn_starts.datasize);
        size_t pos = 0, size = images[0].fn_starts.datasize, count = 0;
        uint32_t addr = images[0].mh_va;
        uint32_t *starts;
        if (blob == NULL) {
            return;
        }
        starts = malloc(65536u * sizeof *starts);
        if (starts == NULL) {
            free(blob);
            return;
        }
        while (pos < size && count < 65536u) {
            uint64_t delta = 0;
            uint32_t key;
            if (!a32_ios_rd_uleb(blob, size, &pos, &delta)) {
                break;
            }
            if (delta == 0u) {
                continue;
            }
            addr += (uint32_t)delta;
            key = addr & ~1U;
            if (key < out->text_start || key >= out->text_end) {
                continue;
            }
            starts[count++] = addr;
        }
        free(blob);
        if (count == 0u) {
            free(starts);
            return;
        }
        free(ctx->ios_fn_starts);
        ctx->ios_fn_starts = starts;
        ctx->ios_fn_starts_count = (uint32_t)count;
    }
}

static uint32_t ios_thread_entry(uint32_t pc, uint32_t cpsr)
{
    if (((cpsr & 0x20u) != 0u) || ((pc & 1u) != 0u)) {
        return pc | 1u;
    }
    return pc;
}

static int ios_peek_dylinker(int fd, uint64_t *slice_off, uint64_t *slice_size,
                             uint64_t file_size)
{
    ios_mach_header_t mh;
    if (a32_ios_pick_slice(fd, file_size, slice_off, slice_size) != 0) {
        return -1;
    }
    if (a32_ios_pread_full(fd, &mh, sizeof mh, *slice_off) != 0) {
        return -1;
    }
    if (mh.magic != IOS_MH_MAGIC || mh.cputype != IOS_CPU_TYPE_ARM ||
        mh.filetype != IOS_MH_DYLINKER) {
        return -1;
    }
    return 0;
}

static int ios_load_inner(a32_ctx_t *ctx, int exec_fd, const char *path,
                          a32_elf_info_t *out_info, a32_ios_image_t *images,
                          a32_ios_parsed_t *parsed)
{
    struct stat st;
    uint64_t file_size;
    uint32_t dylib_bump = A32_IOS_DYLIB_BASE;
    size_t image_count = 0;
    char exec_dir[IOS_PATH_MAX];
    size_t idx;
    size_t j;
    int fd = exec_fd;
    if (fd < 0) {
        fd = a32_open_guest_ro(ctx, path);
        if (fd < 0) {
            fprintf(stderr, "a32rt: cannot open '%s': %s\n", path,
                    strerror(errno));
            return -1;
        }
    }
    images[0].fd = fd;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        fprintf(stderr, "a32rt: cannot stat '%s': %s\n", path, strerror(errno));
        return -1;
    }
    images[0].st_dev = (uint64_t)st.st_dev;
    images[0].st_ino = (uint64_t)st.st_ino;
    file_size = (uint64_t)st.st_size;
    ios_copy_str(images[0].path, sizeof images[0].path, path);
    if (a32_ios_pick_slice(fd, file_size, &images[0].slice_off,
                       &images[0].slice_size) != 0) {
        fprintf(stderr, "a32rt: '%s' is not an ARM32 Mach-O\n", path);
        return -1;
    }
    if (a32_ios_parse_map(ctx, &images[0], &parsed[0], &dylib_bump) != 0) {
        return -1;
    }
    image_count = 1;
    /* Real dyld handoff: /usr/lib/dyld present under the sysroot. */
    if (parsed[0].dylinker[0] != '\0' && ctx->sysroot != NULL &&
        ctx->sysroot[0] != '\0') {
        int dfd = a32_open_guest_ro(ctx, parsed[0].dylinker);
        if (dfd >= 0) {
            struct stat dst;
            uint64_t dsize = 0;
            bool ok = false;
            if (fstat(dfd, &dst) == 0 && dst.st_size > 0) {
                dsize = (uint64_t)dst.st_size;
                ok = true;
            }
            if (ok && ios_peek_dylinker(dfd, &images[1].slice_off,
                                        &images[1].slice_size, dsize) == 0) {
                uint32_t dyld_entry;
                images[1].fd = dfd;
                images[1].st_dev = (uint64_t)dst.st_dev;
                images[1].st_ino = (uint64_t)dst.st_ino;
                ios_copy_str(images[1].path, sizeof images[1].path,
                             parsed[0].dylinker);
                if (a32_ios_parse_map(ctx, &images[1], &parsed[1], &dylib_bump) != 0) {
                    return -1;
                }
                if (!parsed[1].has_thread) {
                    fprintf(stderr, "a32rt: dyld has no entry thread state\n");
                    return -1;
                }
                dyld_entry = ios_thread_entry(parsed[1].tstate[15],
                                              parsed[1].tcpsr);
                for (j = 0; j < 13u; j++) {
                    ctx->regs[j] = parsed[1].tstate[j];
                }
                ctx->regs[14] = parsed[1].tstate[14];
                if (ios_apply_prot(ctx, images, 2) != 0) {
                    return -1;
                }
                ctx->ios_uses_dyld = true;
                ctx->ios_has_crt0 = false;
                ctx->ios_init_count = 0;
                ctx->ios_mh_va = images[0].mh_va;
                ctx->ios_main_va = 0;
                ctx->elf_base = images[0].mh_va;
                ctx->elf_entry = dyld_entry;
                out_info->entry = dyld_entry;
                ios_fill_info(images, out_info);
                ios_refine_text_and_fn_starts(ctx, images, out_info);
                return 0;
            }
            close(dfd);
        }
    }
    /* Standalone dynamic linker mode. */
    memset(&images[1], 0, sizeof images[1]);
    images[1].is_synthetic = true;
    images[1].fd = -1;
    ios_copy_str(images[1].path, sizeof images[1].path,
                 "/usr/lib/libSystem.B.dylib");
    ios_copy_str(images[1].install_name, sizeof images[1].install_name,
                 "/usr/lib/libSystem.B.dylib");
    image_count = 2;
    ios_dirname(path, exec_dir, sizeof exec_dir);
    if (exec_dir[0] == '\0') {
        ios_copy_str(exec_dir, sizeof exec_dir, ".");
    }
    for (idx = 0; idx < image_count; idx++) {
        if (images[idx].is_synthetic) {
            continue;
        }
        images[idx].dep_count = parsed[idx].ndeps;
        for (j = 0; j < parsed[idx].ndeps; j++) {
            int r = ios_resolve_dep(ctx, images, parsed, &image_count,
                                    &dylib_bump, exec_dir, idx,
                                    parsed[idx].deps[j]);
            images[idx].dep_weak[j] = parsed[idx].depweak[j];
            if (r >= 0) {
                images[idx].dep_image_idx[j] = r;
            } else if (r == -1) {
                return -1;
            } else if (ios_is_system_dylib(parsed[idx].deps[j])) {
                images[idx].dep_image_idx[j] = 1;
            } else if (parsed[idx].depweak[j]) {
                images[idx].dep_image_idx[j] = -1;
            } else {
                fprintf(stderr, "a32rt: cannot find required dylib '%s' for '%s'\n",
                        parsed[idx].deps[j], images[idx].path);
                return -1;
            }
        }
    }
    if (a32_ios_bind_all(ctx, images, image_count) != 0) {
        a32_ios_free_image_caches(images, image_count);
        return -1;
    }
    a32_ios_free_image_caches(images, image_count);
    if (ios_collect_inits(ctx, images, image_count) != 0) {
        return -1;
    }
    if (ios_apply_prot(ctx, images, image_count) != 0) {
        return -1;
    }
    ios_fill_info(images, out_info);
    ios_refine_text_and_fn_starts(ctx, images, out_info);
    ctx->ios_uses_dyld = false;
    ctx->ios_mh_va = images[0].mh_va;
    ctx->brk_base = a32_page_up(out_info->max_vaddr);
    ctx->brk_curr = ctx->brk_base;
    ctx->elf_base = images[0].mh_va;
    if (parsed[0].has_main) {
        uint32_t eva;
        if ((uint64_t)parsed[0].main_entryoff >=
            (uint64_t)images[0].slice_size) {
            return -1;
        }
        eva = images[0].mh_va + parsed[0].main_entryoff;
        if (!ios_exec_contains(&images[0], eva)) {
            return -1;
        }
        ctx->ios_main_va = eva;
        ctx->ios_has_crt0 = true;
        ctx->elf_entry = A32_IOS_CRT0_VA | 1u;
        out_info->entry = ctx->ios_main_va;
    } else if (parsed[0].has_thread) {
        uint32_t entry;
        uint32_t slid_pc;
        for (j = 0; j < 13u; j++) {
            ctx->regs[j] = parsed[0].tstate[j];
        }
        ctx->regs[14] = parsed[0].tstate[14];
        slid_pc = parsed[0].tstate[15] + images[0].slide;
        if (!ios_exec_contains(&images[0], slid_pc)) {
            return -1;
        }
        entry = ios_thread_entry(parsed[0].tstate[15], parsed[0].tcpsr);
        ctx->ios_main_va = entry;
        if (ctx->ios_init_count > 0u) {
            ctx->ios_has_crt0 = true;
            ctx->elf_entry = A32_IOS_CRT0_VA | 1u;
        } else {
            ctx->ios_has_crt0 = false;
            ctx->elf_entry = entry;
            out_info->entry = entry;
        }
    } else {
        ctx->ios_main_va = 0;
        ctx->ios_has_crt0 = false;
        ctx->elf_entry = 0;
        out_info->entry = 0;
    }
    return 0;
}

int a32_ios_load(a32_ctx_t *ctx, int exec_fd, const char *path,
                 a32_elf_info_t *out_info)
{
    a32_ios_image_t *images;
    a32_ios_parsed_t *parsed;
    int rc;
    size_t i;
    if (ctx == NULL || out_info == NULL || path == NULL) {
        return -1;
    }
    memset(out_info, 0, sizeof *out_info);
    images = calloc(A32_IOS_MAX_IMAGES, sizeof *images);
    parsed = calloc(A32_IOS_MAX_IMAGES, sizeof *parsed);
    if (images == NULL || parsed == NULL) {
        free(images);
        free(parsed);
        return -1;
    }
    for (i = 0; i < A32_IOS_MAX_IMAGES; i++) {
        images[i].fd = -1;
    }
    rc = ios_load_inner(ctx, exec_fd, path, out_info, images, parsed);
    a32_ios_free_image_caches(images, A32_IOS_MAX_IMAGES);
    for (i = 0; i < A32_IOS_MAX_IMAGES; i++) {
        if (!images[i].is_synthetic && images[i].fd >= 0) {
            close(images[i].fd);
            images[i].fd = -1;
        }
    }
    free(images);
    free(parsed);
    return rc;
}
