/*
 * src/elf32.c — ELFCLASS32 (EM_ARM) loader and kuser helper page setup
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#define _GNU_SOURCE
#include "a32rt.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define EI_NIDENT   16U
#define ELFCLASS32  1U
#define ELFDATA2LSB 1U
#define ET_EXEC     2U
#define ET_DYN      3U
#define EM_ARM      40U

#define PT_LOAD   1U
#define PT_INTERP 3U
#define PT_PHDR   6U

#define PF_X 0x1U
#define PF_W 0x2U
#define PF_R 0x4U

#define A32_PAGE_SIZE     4096U
#define A32_KUSER_BASE_32 0xFFFF0000U

typedef struct elf32_ehdr {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} elf32_ehdr_t;

typedef struct elf32_phdr {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} elf32_phdr_t;

static uint32_t page_down(uint32_t v)
{
    return v & ~0xFFFU;
}

static uint32_t page_up(uint32_t v)
{
    return (v + 0xFFFU) & ~0xFFFU;
}

static int pread_full(int fd, void *buf, size_t count, off_t offset)
{
    uint8_t *p = (uint8_t *)buf;
    size_t total = 0;
    while (total < count) {
        ssize_t n = pread(fd, p + total, count - total, offset + (off_t)total);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        total += (size_t)n;
    }
    return 0;
}

static int load_elf32_image(a32_ctx_t *ctx, int fd, uint32_t default_pie_base,
                            const char *label, a32_elf_info_t *out)
{
    elf32_ehdr_t ehdr;
    if (pread_full(fd, &ehdr, sizeof(ehdr), 0) != 0) {
        fprintf(stderr, "a32rt: %s: failed to read ELF header\n", label);
        return -1;
    }

    if (ehdr.e_ident[0] != 0x7fU || ehdr.e_ident[1] != 'E' ||
        ehdr.e_ident[2] != 'L'  || ehdr.e_ident[3] != 'F') {
        fprintf(stderr, "a32rt: %s: not an ELF file\n", label);
        return -1;
    }
    if (ehdr.e_ident[4] != ELFCLASS32 || ehdr.e_ident[5] != ELFDATA2LSB ||
        ehdr.e_machine != EM_ARM) {
        fprintf(stderr, "a32rt: %s: not a 32-bit little-endian ARM ELF\n", label);
        return -1;
    }
    if (ehdr.e_type != ET_EXEC && ehdr.e_type != ET_DYN) {
        fprintf(stderr, "a32rt: %s: unsupported ELF type %u\n", label, ehdr.e_type);
        return -1;
    }
    if (ehdr.e_phentsize != sizeof(elf32_phdr_t) || ehdr.e_phnum == 0U || ehdr.e_phnum > 64U) {
        fprintf(stderr, "a32rt: %s: invalid program header table\n", label);
        return -1;
    }

    elf32_phdr_t phdrs[64];
    size_t phdr_bytes = (size_t)ehdr.e_phnum * sizeof(elf32_phdr_t);
    if (pread_full(fd, phdrs, phdr_bytes, (off_t)ehdr.e_phoff) != 0) {
        fprintf(stderr, "a32rt: %s: failed to read program headers\n", label);
        return -1;
    }

    uint32_t min_vaddr = UINT32_MAX;
    for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
        if (phdrs[i].p_type == PT_LOAD && phdrs[i].p_memsz > 0U) {
            if (phdrs[i].p_vaddr < min_vaddr) {
                min_vaddr = phdrs[i].p_vaddr;
            }
        }
    }
    if (min_vaddr == UINT32_MAX) {
        fprintf(stderr, "a32rt: %s: no PT_LOAD segments\n", label);
        return -1;
    }

    uint32_t load_bias = 0U;
    if (ehdr.e_type == ET_DYN && min_vaddr < 0x10000U) {
        load_bias = default_pie_base;
    }

    if (ehdr.e_entry == 0U) {
        if (!(ehdr.e_type == ET_DYN && ctx->aot_mode)) {
            fprintf(stderr, "a32rt: %s: missing entry point (e_entry == 0)\n", label);
            return -1;
        }
    }

    uint64_t entry64 = 0ULL;
    if (ehdr.e_entry != 0U) {
        entry64 = (uint64_t)ehdr.e_entry + (uint64_t)load_bias;
        if (entry64 < 0x10000ULL || entry64 >= (uint64_t)A32_KUSER_BASE_32) {
            fprintf(stderr, "a32rt: %s: entry point 0x%llx out of guest bounds\n",
                    label, (unsigned long long)entry64);
            return -1;
        }
    }

    memset(out, 0, sizeof(*out));
    out->load_bias = load_bias + page_down(min_vaddr);
    out->entry     = (uint32_t)entry64;
    out->phent     = ehdr.e_phentsize;
    out->phnum     = ehdr.e_phnum;
    if (out->entry != 0U && (out->entry & 3U) == 2U) {
        fprintf(stderr, "a32rt: %s: misaligned A32 entry point 0x%x\n", label, out->entry);
        return -1;
    }

    for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
        const elf32_phdr_t *ph = &phdrs[i];
        if (ph->p_type == PT_PHDR) {
            uint64_t phdr_va64 = (uint64_t)load_bias + (uint64_t)ph->p_vaddr;
            if (phdr_va64 + phdr_bytes <= (uint64_t)A32_KUSER_BASE_32) {
                out->phdr_vaddr = (uint32_t)phdr_va64;
            }
        } else if (ph->p_type == PT_INTERP) {
            if (ph->p_filesz == 0U || ph->p_filesz >= sizeof(out->interp_path)) {
                fprintf(stderr, "a32rt: %s: invalid PT_INTERP filesz %u\n", label, ph->p_filesz);
                return -1;
            }
            if (pread_full(fd, out->interp_path, ph->p_filesz, (off_t)ph->p_offset) != 0) {
                return -1;
            }
            out->interp_path[ph->p_filesz] = '\0';
            if (out->interp_path[0] == '\0') {
                return -1;
            }
        }
    }

    uint32_t highest_mapped_end  = 0U;
    uint32_t highest_mapped_prot = 0U;
    uint32_t prev_seg_end        = 0U;
    uint32_t fallback_phdr_vaddr = 0U;
    bool     entry_in_load       = false;
    bool     phdr_in_load        = false;

    for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
        const elf32_phdr_t *ph = &phdrs[i];
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0U) {
            continue;
        }
        if (ph->p_filesz > ph->p_memsz) {
            fprintf(stderr, "a32rt: %s: invalid segment filesz (0x%x) > memsz (0x%x)\n",
                    label, ph->p_filesz, ph->p_memsz);
            return -1;
        }

        uint64_t seg_vaddr64 = (uint64_t)load_bias + (uint64_t)ph->p_vaddr;
        uint64_t memsz       = (uint64_t)ph->p_memsz;
        if (seg_vaddr64 >= (uint64_t)A32_KUSER_BASE_32 ||
            seg_vaddr64 + memsz > (uint64_t)A32_KUSER_BASE_32) {
            fprintf(stderr, "a32rt: %s: segment [0x%llx, 0x%llx) out of guest bounds\n",
                    label, (unsigned long long)seg_vaddr64,
                    (unsigned long long)(seg_vaddr64 + memsz));
            return -1;
        }

        uint32_t seg_vaddr  = (uint32_t)seg_vaddr64;
        uint64_t seg_end64  = seg_vaddr64 + memsz;
        if (seg_vaddr < prev_seg_end) {
            fprintf(stderr, "a32rt: %s: overlapping or out-of-order PT_LOAD segment at 0x%x\n",
                    label, seg_vaddr);
            return -1;
        }
        prev_seg_end = (uint32_t)seg_end64;

        if (out->entry != 0U) {
            uint32_t entry_pc      = (out->entry & 1U) ? (out->entry & ~1U) : (out->entry & ~3U);
            uint32_t entry_min_len = (out->entry & 1U) ? 2U : 4U;
            if ((ph->p_flags & PF_X) != 0U &&
                entry_pc >= seg_vaddr &&
                (uint64_t)entry_pc + entry_min_len <= seg_vaddr64 + memsz) {
                entry_in_load = true;
            }
        }
        uint32_t page_start = page_down(seg_vaddr);
        uint64_t map_end64  = (seg_end64 + 0xFFFULL) & ~0xFFFULL;
        if (page_start < 0x10000U || map_end64 > (uint64_t)A32_KUSER_BASE_32) {
            fprintf(stderr, "a32rt: %s: segment [0x%x, 0x%llx) out of guest bounds\n",
                    label, page_start, (unsigned long long)map_end64);
            return -1;
        }
        uint32_t map_end   = (uint32_t)map_end64;
        uint32_t map_start = (page_start < highest_mapped_end) ? highest_mapped_end : page_start;
        for (size_t r = 0U; r < ctx->region_count; r++) {
            if (map_start < ctx->regions[r].end && map_end > ctx->regions[r].start) {
                fprintf(stderr, "a32rt: %s: segment [0x%x, 0x%x) overlaps existing region\n",
                        label, map_start, map_end);
                return -1;
            }
        }

        int host_prot = 0;
        if (ph->p_flags & PF_R) {
            host_prot |= PROT_READ;
        }
        if (ph->p_flags & PF_W) {
            host_prot |= PROT_WRITE;
        }
        if (ph->p_flags & PF_X) {
            host_prot |= PROT_EXEC;
        }

        uint32_t read_file_off = ph->p_offset;
        uint32_t read_file_len = ph->p_filesz;

        if (page_start < map_start) {
            uint32_t shared_end  = (map_end < map_start) ? map_end : map_start;
            uint32_t shared_len  = shared_end - page_start;
            uint32_t merged_prot = highest_mapped_prot | (uint32_t)host_prot;

            if (mprotect(a32_guest_ptr(ctx, page_start), shared_len, PROT_READ | PROT_WRITE) != 0) {
                fprintf(stderr, "a32rt: %s: mprotect RW failed on shared page [0x%x, 0x%x): %s\n",
                        label, page_start, shared_end, strerror(errno));
                return -1;
            }

            if (map_end > map_start) {
                uint32_t new_len = map_end - map_start;
                void *res = mmap(a32_guest_ptr(ctx, map_start), new_len,
                                 PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
                if (res == MAP_FAILED) {
                    fprintf(stderr, "a32rt: %s: failed to map segment [0x%x, 0x%x): %s\n",
                            label, map_start, map_end, strerror(errno));
                    return -1;
                }
            }

            if (ph->p_filesz > 0U) {
                void *host_seg = a32_guest_ptr(ctx, seg_vaddr);
                if (pread_full(fd, host_seg, ph->p_filesz, (off_t)ph->p_offset) != 0) {
                    fprintf(stderr, "a32rt: %s: failed to read segment payload\n", label);
                    return -1;
                }
            }

            if (ph->p_memsz > ph->p_filesz) {
                memset(a32_guest_ptr(ctx, seg_vaddr + ph->p_filesz), 0,
                       (size_t)(ph->p_memsz - ph->p_filesz));
            }

            uint32_t restore_prot = merged_prot;
            if ((restore_prot & (uint32_t)PROT_EXEC) != 0U) {
                restore_prot |= (uint32_t)PROT_READ;
            }
            if ((ctx->host_info.caps & A32H_CAP_WX_ENFORCED) != 0ULL &&
                (restore_prot & (uint32_t)(PROT_WRITE | PROT_EXEC)) ==
                    (uint32_t)(PROT_WRITE | PROT_EXEC)) {
                restore_prot = (uint32_t)(PROT_READ | PROT_WRITE);
            }

            if (mprotect(a32_guest_ptr(ctx, page_start), shared_len, (int)restore_prot) != 0) {
                fprintf(stderr, "a32rt: %s: mprotect failed on shared page [0x%x, 0x%x): %s\n",
                        label, page_start, shared_end, strerror(errno));
                return -1;
            }
            a32_vm_record_region(ctx, page_start, shared_end, merged_prot, 0x12U, label);

            if (map_end > map_start) {
                uint32_t new_len  = map_end - map_start;
                uint32_t new_prot = (uint32_t)host_prot;
                if ((new_prot & (uint32_t)PROT_EXEC) != 0U) {
                    new_prot |= (uint32_t)PROT_READ;
                }
                if ((ctx->host_info.caps & A32H_CAP_WX_ENFORCED) != 0ULL &&
                    (new_prot & (uint32_t)(PROT_WRITE | PROT_EXEC)) ==
                        (uint32_t)(PROT_WRITE | PROT_EXEC)) {
                    new_prot = (uint32_t)(PROT_READ | PROT_WRITE);
                }
                if (mprotect(a32_guest_ptr(ctx, map_start), new_len, (int)new_prot) != 0) {
                    fprintf(stderr, "a32rt: %s: mprotect failed on [0x%x, 0x%x): %s\n",
                            label, map_start, map_end, strerror(errno));
                    return -1;
                }
                a32_vm_record_region(ctx, map_start, map_end, (uint32_t)host_prot, 0x12U, label);
                highest_mapped_end  = map_end;
                highest_mapped_prot = (uint32_t)host_prot;
            } else {
                highest_mapped_prot = merged_prot;
            }
        } else {
            uint32_t map_len = map_end - map_start;
            void *host_map = a32_guest_ptr(ctx, map_start);
            void *res = mmap(host_map, map_len, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
            if (res == MAP_FAILED) {
                fprintf(stderr, "a32rt: %s: failed to map segment [0x%x, 0x%x): %s\n",
                        label, map_start, map_end, strerror(errno));
                return -1;
            }

            if (ph->p_filesz > 0U) {
                uint32_t page_off = ph->p_vaddr & 0xFFFU;
                if (page_off != 0U && ph->p_offset >= page_off) {
                    read_file_off = ph->p_offset - page_off;
                    read_file_len = ph->p_filesz + page_off;
                    if (pread_full(fd, host_map, read_file_len, (off_t)read_file_off) != 0) {
                        fprintf(stderr, "a32rt: %s: failed to read segment payload\n", label);
                        return -1;
                    }
                } else {
                    void *host_seg = a32_guest_ptr(ctx, seg_vaddr);
                    if (pread_full(fd, host_seg, ph->p_filesz, (off_t)ph->p_offset) != 0) {
                        fprintf(stderr, "a32rt: %s: failed to read segment payload\n", label);
                        return -1;
                    }
                }
            }

            if (ph->p_memsz > ph->p_filesz) {
                memset(a32_guest_ptr(ctx, seg_vaddr + ph->p_filesz), 0,
                       (size_t)(ph->p_memsz - ph->p_filesz));
            }

            uint32_t apply_prot = (uint32_t)host_prot;
            if ((apply_prot & (uint32_t)PROT_EXEC) != 0U) {
                apply_prot |= (uint32_t)PROT_READ;
            }
            if ((ctx->host_info.caps & A32H_CAP_WX_ENFORCED) != 0ULL &&
                (apply_prot & (uint32_t)(PROT_WRITE | PROT_EXEC)) ==
                    (uint32_t)(PROT_WRITE | PROT_EXEC)) {
                apply_prot = (uint32_t)(PROT_READ | PROT_WRITE);
            }

            if (mprotect(host_map, map_len, (int)apply_prot) != 0) {
                fprintf(stderr, "a32rt: %s: mprotect failed on [0x%x, 0x%x): %s\n",
                        label, map_start, map_end, strerror(errno));
                return -1;
            }

            a32_vm_record_region(ctx, map_start, map_end, (uint32_t)host_prot, 0x12U, label);
            highest_mapped_end  = map_end;
            highest_mapped_prot = (uint32_t)host_prot;
        }

        if (ph->p_flags & PF_X) {
            if (out->text_start == 0U || seg_vaddr < out->text_start) {
                out->text_start = seg_vaddr;
            }
            if (seg_vaddr + ph->p_memsz > out->text_end) {
                out->text_end = seg_vaddr + ph->p_memsz;
            }
        }

        uint32_t base_va = (read_file_off < ph->p_offset) ? page_start : seg_vaddr;
        if (out->phdr_vaddr != 0U &&
            out->phdr_vaddr >= base_va &&
            (uint64_t)out->phdr_vaddr + phdr_bytes <= (uint64_t)base_va + (uint64_t)read_file_len) {
            phdr_in_load = true;
        }
        if (fallback_phdr_vaddr == 0U && read_file_off <= ehdr.e_phoff &&
            (uint64_t)ehdr.e_phoff + phdr_bytes <= (uint64_t)read_file_off + (uint64_t)read_file_len) {
            fallback_phdr_vaddr = base_va + (ehdr.e_phoff - read_file_off);
        }

        if (map_end > out->max_vaddr) {
            out->max_vaddr = map_end;
        }
    }

    if (!phdr_in_load) {
        out->phdr_vaddr = 0U;
    }
    if (out->phdr_vaddr == 0U) {
        out->phdr_vaddr = fallback_phdr_vaddr;
    }

    if (out->entry == 0U && out->text_start >= 0x10000U && out->text_end > out->text_start) {
        out->entry    = out->text_start;
        entry_in_load = true;
    }

    if (!entry_in_load) {
        fprintf(stderr, "a32rt: %s: entry point 0x%x is not within any PT_LOAD segment\n",
                label, out->entry);
        return -1;
    }

    return 0;
}

int a32_elf_load(a32_ctx_t *ctx, int exec_fd, const char *path, a32_elf_info_t *out_info)
{
    if (!ctx || !out_info) {
        return -1;
    }

    int fd = exec_fd;
    if (fd < 0) {
        if (!path) {
            return -1;
        }
        char resolved_exec[PATH_MAX];
        if (a32_resolve_guest_path(ctx, path, resolved_exec, sizeof(resolved_exec)) == 0) {
            fd = open(resolved_exec, O_RDONLY | O_CLOEXEC);
        }
        if (fd < 0) {
            fd = open(path, O_RDONLY | O_CLOEXEC);
        }
        if (fd < 0) {
            fprintf(stderr, "a32rt: cannot open '%s': %s\n", path, strerror(errno));
            return -1;
        }
    }

    int rc = load_elf32_image(ctx, fd, 0x00400000U, path ? path : "[guest]", out_info);
    close(fd);
    if (rc != 0) {
        return -1;
    }

    ctx->elf_base  = out_info->load_bias;
    ctx->elf_entry = out_info->entry;
    ctx->brk_base  = page_up(out_info->max_vaddr);
    ctx->brk_curr  = ctx->brk_base;

    /* Load PT_INTERP if present */
    if (out_info->interp_path[0] != '\0') {
        char resolved_interp[PATH_MAX];
        int ifd = -1;
        if (a32_resolve_guest_path(ctx, out_info->interp_path,
                                   resolved_interp, sizeof(resolved_interp)) == 0) {
            ifd = open(resolved_interp, O_RDONLY | O_CLOEXEC);
        } else {
            snprintf(resolved_interp, sizeof(resolved_interp), "%s", out_info->interp_path);
        }
        if (ifd < 0 && strcmp(resolved_interp, out_info->interp_path) != 0) {
            ifd = open(out_info->interp_path, O_RDONLY | O_CLOEXEC);
        }
        if (ifd < 0) {
            fprintf(stderr, "a32rt: cannot open interpreter '%s': %s\n",
                    resolved_interp, strerror(errno));
            return -1;
        }

        a32_elf_info_t interp_info;
        uint32_t interp_base = (ctx->brk_base > 0x3F000000U) ? ctx->brk_base : 0x3F000000U;
        int irc = load_elf32_image(ctx, ifd, interp_base, out_info->interp_path, &interp_info);
        close(ifd);
        if (irc != 0) {
            return -1;
        }
        out_info->interp_base = interp_info.load_bias;
        ctx->interp_base      = interp_info.load_bias;
        ctx->elf_entry        = interp_info.entry;
        uint32_t interp_end   = page_up(interp_info.max_vaddr);
        if (ctx->brk_base >= interp_info.load_bias && ctx->brk_base < interp_end) {
            ctx->brk_base = interp_end;
            ctx->brk_curr = ctx->brk_base;
        }
    }

    return 0;
}

int a32_kuser_setup(a32_ctx_t *ctx)
{
    if (!ctx) {
        return -1;
    }

    const uint32_t kuser_va = 0xFFFF0000U;
    void *host_page = a32_guest_ptr(ctx, kuser_va);
    void *res = mmap(host_page, 4096U, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (res == MAP_FAILED) {
        fprintf(stderr, "a32rt: failed to map kuser page at 0xffff0000: %s\n",
                strerror(errno));
        return -1;
    }

    memset(host_page, 0, 4096U);

    /*
     * 0xffff0f60: __kuser_cmpxchg64
     *   push    {r4, r5, r6, r7}
     *   ldrd    r4, r5, [r0]
     *   ldrd    r6, r7, [r1]
     * 1:ldrexd  r0, r1, [r2]
     *   eors    r3, r0, r4
     *   eoreqs  r3, r1, r5
     *   strexdeq r3, r6, r7, [r2]
     *   teqeq   r3, #1
     *   beq     1b
     *   rsbs    r0, r3, #0
     *   dmb     ish
     *   pop     {r4, r5, r6, r7}
     *   bx      lr
     */
    static const uint32_t kuser_cmpxchg64[] = {
        0xe92d00f0U,
        0xe1c040d0U,
        0xe1c160d0U,
        0xe1b20f9fU,
        0xe0303004U,
        0x00313005U,
        0x01a23f96U,
        0x03330001U,
        0x0afffff9U,
        0xe2730000U,
        0xf57ff05bU,
        0xe8bd00f0U,
        0xe12fff1eU,
    };
    memcpy((uint8_t *)host_page + 0x0F60U, kuser_cmpxchg64, sizeof(kuser_cmpxchg64));

    /*
     * 0xffff0fa0: __kuser_memory_barrier
     *   dmb     ish
     *   bx      lr
     */
    static const uint32_t kuser_dmb[] = {
        0xf57ff05bU,
        0xe12fff1eU,
    };
    memcpy((uint8_t *)host_page + 0x0FA0U, kuser_dmb, sizeof(kuser_dmb));

    /*
     * 0xffff0fa8: rt_sigreturn restorer
     *   mov r7, #173
     *   svc #0
     */
    static const uint32_t kuser_rt_sigreturn[] = {
        0xe3a070adU,
        0xef000000U,
    };
    memcpy((uint8_t *)host_page + 0x0FA8U, kuser_rt_sigreturn, sizeof(kuser_rt_sigreturn));

    /*
     * 0xffff0fb0: sigreturn restorer
     *   mov r7, #119
     *   svc #0
     */
    static const uint32_t kuser_sigreturn[] = {
        0xe3a07077U,
        0xef000000U,
    };
    memcpy((uint8_t *)host_page + 0x0FB0U, kuser_sigreturn, sizeof(kuser_sigreturn));

    /*
     * 0xffff0fc0: __kuser_cmpxchg
     * 1:ldrex   r3, [r2]
     *   subs    r3, r3, r0
     *   strexeq r3, r1, [r2]
     *   teqeq   r3, #1
     *   beq     1b
     *   rsbs    r0, r3, #0
     *   dmb     ish
     *   bx      lr
     */
    static const uint32_t kuser_cmpxchg[] = {
        0xe1923f9fU,
        0xe0533000U,
        0x01823f91U,
        0x03330001U,
        0x0afffffaU,
        0xe2730000U,
        0xf57ff05bU,
        0xe12fff1eU,
    };
    memcpy((uint8_t *)host_page + 0x0FC0U, kuser_cmpxchg, sizeof(kuser_cmpxchg));

    /*
     * 0xffff0fe0: __kuser_get_tls
     *   mrc     p15, 0, r0, c13, c0, 3
     *   bx      lr
     */
    static const uint32_t kuser_get_tls[] = {
        0xee1d0f70U,
        0xe12fff1eU,
    };
    memcpy((uint8_t *)host_page + 0x0FE0U, kuser_get_tls, sizeof(kuser_get_tls));

    /* 0xffff0ffc: __kuser_helper_version = 5 */
    uint32_t kuser_ver = 5U;
    memcpy((uint8_t *)host_page + 0x0FFCU, &kuser_ver, sizeof(kuser_ver));

    if (mprotect(host_page, 4096U, PROT_READ | PROT_EXEC) != 0) {
        fprintf(stderr, "a32rt: failed to mprotect kuser page: %s\n", strerror(errno));
        return -1;
    }

    a32_vm_record_region(ctx, kuser_va, kuser_va + 4096U,
                         (uint32_t)(PROT_READ | PROT_EXEC), 0x12U, "[vectors]");
    return 0;
}
