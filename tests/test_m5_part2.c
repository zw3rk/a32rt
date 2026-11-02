/*
 * tests/test_m5_part2.c — M5 LTP compatibility tests (part 2: tests 14..19)
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "test_m5_common.h"

static int test14_rwx_mmap_mprotect(void)
{
    /* Part A: mmap2 directly with PROT_READ|PROT_WRITE|PROT_EXEC (7U) */
    uint32_t rwx_page = (uint32_t)sys6(192U /* mmap2 */, 0U, 4096U,
                                       7U /* PROT_READ|PROT_WRITE|PROT_EXEC */,
                                       0x22U /* MAP_PRIVATE|MAP_ANONYMOUS */,
                                       0xFFFFFFFFU, 0U);
    if (rwx_page > 0xFFFFF000U) {
        return 115;
    }

    volatile uint32_t *code = (volatile uint32_t *)(uintptr_t)rwx_page;
    code[0] = 0xE3A0003AU; /* mov r0, #58 */
    code[1] = 0xE12FFF1EU; /* bx lr */
    sys3(0x0F0002U /* cacheflush */, rwx_page, rwx_page + 16U, 0U);

    typedef uint32_t (*rwx_fn_t)(void);
    rwx_fn_t fn = (rwx_fn_t)(uintptr_t)rwx_page;
    if (fn() != 58U) {
        sys2(91U /* munmap */, rwx_page, 4096U);
        return 116;
    }

    /* Modify in place WITHOUT calling mprotect (proves guest RWX remains writable) */
    code[0] = 0xE3A0005BU; /* mov r0, #91 */
    sys3(0x0F0002U /* cacheflush */, rwx_page, rwx_page + 16U, 0U);
    if (fn() != 91U) {
        sys2(91U /* munmap */, rwx_page, 4096U);
        return 117;
    }
    sys2(91U /* munmap */, rwx_page, 4096U);

    /* Part B: mmap2 PROT_READ then mprotect to PROT_READ|PROT_WRITE|PROT_EXEC (7U) */
    uint32_t p2 = (uint32_t)sys6(192U /* mmap2 */, 0U, 4096U,
                                 1U /* PROT_READ */,
                                 0x22U /* MAP_PRIVATE|MAP_ANONYMOUS */,
                                 0xFFFFFFFFU, 0U);
    if (p2 > 0xFFFFF000U) {
        return 118;
    }
    if (sys3(125U /* mprotect */, p2, 4096U, 7U /* PROT_READ|PROT_WRITE|PROT_EXEC */) != 0) {
        sys2(91U /* munmap */, p2, 4096U);
        return 119;
    }

    volatile uint32_t *code2 = (volatile uint32_t *)(uintptr_t)p2;
    code2[0] = 0xE3A0004CU; /* mov r0, #76 */
    code2[1] = 0xE12FFF1EU; /* bx lr */
    sys3(0x0F0002U /* cacheflush */, p2, p2 + 16U, 0U);

    rwx_fn_t fn2 = (rwx_fn_t)(uintptr_t)p2;
    if (fn2() != 76U) {
        sys2(91U /* munmap */, p2, 4096U);
        return 120;
    }

    code2[0] = 0xE3A00063U; /* mov r0, #99 */
    sys3(0x0F0002U /* cacheflush */, p2, p2 + 16U, 0U);
    if (fn2() != 99U) {
        sys2(91U /* munmap */, p2, 4096U);
        return 121;
    }

    sys2(91U /* munmap */, p2, 4096U);
    return 0;
}

/* --------------------------------------------------------------------------
 * Test 15: ppoll (336) sigsetsize validation (positive == 8, negative != 8)
 * -------------------------------------------------------------------------- */
static int test15_ppoll_sigsetsize(void)
{
    int32_t efd = sys2(356U /* eventfd2 */, 1U, 0x800U /* EFD_NONBLOCK */);
    if (efd < 0) {
        return 122;
    }

    struct pollfd pfd;
    pfd.fd      = efd;
    pfd.events  = 1; /* POLLIN */
    pfd.revents = 0;

    struct timespec32 ts;
    ts.tv_sec  = 0;
    ts.tv_nsec = 0;

    uint64_t sigmask = 0ULL;

    /* Negative: sigsetsize != 8 (0, 4, 16) must return -EINVAL (-22) */
    if (sys5(336U /* ppoll */, (uint32_t)(uintptr_t)&pfd, 1U,
             (uint32_t)(uintptr_t)&ts, (uint32_t)(uintptr_t)&sigmask, 0U) != -22) {
        sys1(6U /* close */, (uint32_t)efd);
        return 123;
    }
    if (sys5(336U /* ppoll */, (uint32_t)(uintptr_t)&pfd, 1U,
             (uint32_t)(uintptr_t)&ts, (uint32_t)(uintptr_t)&sigmask, 4U) != -22) {
        sys1(6U /* close */, (uint32_t)efd);
        return 124;
    }
    if (sys5(336U /* ppoll */, (uint32_t)(uintptr_t)&pfd, 1U,
             (uint32_t)(uintptr_t)&ts, (uint32_t)(uintptr_t)&sigmask, 16U) != -22) {
        sys1(6U /* close */, (uint32_t)efd);
        return 125;
    }

    /* Positive: sigsetsize == 8 must succeed and report POLLIN */
    int32_t prc = sys5(336U /* ppoll */, (uint32_t)(uintptr_t)&pfd, 1U,
                       (uint32_t)(uintptr_t)&ts, (uint32_t)(uintptr_t)&sigmask, 8U);
    sys1(6U /* close */, (uint32_t)efd);
    if (prc != 1 || (pfd.revents & 1) == 0) {
        return 126;
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * Test 16: 32-bit getdents (141) small-buffer pagination & d_off seeking
 * -------------------------------------------------------------------------- */

static int test16_getdents32_pagination(void)
{
    static const char dir_path[] = "/tmp/a32rt_m5_getdents_dir";
    static const char f_alpha[]  = "/tmp/a32rt_m5_getdents_dir/entry_alpha";
    static const char f_beta[]   = "/tmp/a32rt_m5_getdents_dir/entry_beta";
    static const char f_gamma[]  = "/tmp/a32rt_m5_getdents_dir/entry_gamma";
    static const char f_delta[]  = "/tmp/a32rt_m5_getdents_dir/entry_delta";

    /* Pre-clean in case a previous run left paths behind */
    sys1(10U /* unlink */, (uint32_t)(uintptr_t)f_alpha);
    sys1(10U /* unlink */, (uint32_t)(uintptr_t)f_beta);
    sys1(10U /* unlink */, (uint32_t)(uintptr_t)f_gamma);
    sys1(10U /* unlink */, (uint32_t)(uintptr_t)f_delta);
    sys1(40U /* rmdir */, (uint32_t)(uintptr_t)dir_path);

    if (sys2(39U /* mkdir */, (uint32_t)(uintptr_t)dir_path, 0755U) != 0) {
        return 127;
    }

    const char *files[4] = { f_alpha, f_beta, f_gamma, f_delta };
    for (int i = 0; i < 4; i++) {
        /* O_CREAT | O_WRONLY | O_TRUNC = 01101 = 0x241 */
        int32_t ffd = sys3(5U /* open */, (uint32_t)(uintptr_t)files[i], 0x241U, 0644U);
        if (ffd < 0) {
            return 128;
        }
        sys1(6U /* close */, (uint32_t)ffd);
    }

    /* Open directory O_RDONLY | O_DIRECTORY (ARM O_DIRECTORY = 040000 = 0x4000) */
    int32_t dfd = sys3(5U /* open */, (uint32_t)(uintptr_t)dir_path, 0x4000U, 0U);
    if (dfd < 0) {
        return 129;
    }

    uint8_t buf[48] __attribute__((aligned(4)));

    /*
     * Negative: buffer too small to hold even one linux_dirent32 entry
     * must return -EINVAL (-22), and unmapped buffer must return -EFAULT (-14).
     */
    if (sys3(141U /* getdents */, (uint32_t)dfd, (uint32_t)(uintptr_t)buf, 4U) != -22) {
        sys1(6U /* close */, (uint32_t)dfd);
        return 130;
    }
    if (sys3(141U /* getdents */, (uint32_t)dfd, 0x100U, 48U) != -14) {
        sys1(6U /* close */, (uint32_t)dfd);
        return 131;
    }

    /*
     * Positive: paginate with a small 48-byte buffer (which fits at most 1-2
     * entries per syscall, forcing multiple getdents(141) calls and partial
     * host getdents64 buffer rewinds via lseek(last_d_off)). Every created file
     * must be observed exactly once with none skipped or duplicated.
     */
    uint32_t seen_mask = 0U;
    uint32_t dup_mask  = 0U;
    uint32_t calls     = 0U;

    for (;;) {
        int32_t nread = sys3(141U /* getdents */, (uint32_t)dfd,
                             (uint32_t)(uintptr_t)buf, (uint32_t)sizeof(buf));
        if (nread < 0) {
            sys1(6U /* close */, (uint32_t)dfd);
            return 132;
        }
        if (nread == 0) {
            break;
        }
        calls++;
        int32_t pos = 0;
        while (pos < nread) {
            uint16_t d_reclen = *(const uint16_t *)(const void *)(buf + pos + 8);
            if (d_reclen < 12U || (pos + (int32_t)d_reclen) > nread) {
                sys1(6U /* close */, (uint32_t)dfd);
                return 133;
            }
            const char *d_name = (const char *)(const void *)(buf + pos + 10);
            uint32_t bit = 0U;
            if (m5_streq(d_name, "entry_alpha")) {
                bit = 1U;
            } else if (m5_streq(d_name, "entry_beta")) {
                bit = 2U;
            } else if (m5_streq(d_name, "entry_gamma")) {
                bit = 4U;
            } else if (m5_streq(d_name, "entry_delta")) {
                bit = 8U;
            }
            if (bit != 0U) {
                if ((seen_mask & bit) != 0U) {
                    dup_mask |= bit;
                }
                seen_mask |= bit;
            }
            pos += (int32_t)d_reclen;
        }
        if (calls > 32U) {
            break;
        }
    }

    sys1(6U /* close */, (uint32_t)dfd);
    for (int i = 0; i < 4; i++) {
        sys1(10U /* unlink */, (uint32_t)(uintptr_t)files[i]);
    }
    sys1(40U /* rmdir */, (uint32_t)(uintptr_t)dir_path);

    if (calls < 2U || seen_mask != 0xFU || dup_mask != 0U) {
        return 134;
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * Test 17: prctl pointer translation, getgroups32 & set_robust_list/get_robust_list
 * -------------------------------------------------------------------------- */
struct robust_list_head32_m5 {
    uint32_t list;
    int32_t  futex_offset;
    uint32_t list_op_pending;
};

static int test17_prctl_groups_robust(void)
{
    /*
     * 17a: prctl (172) PR_GET_NAME (16) and PR_SET_NAME (15) guest pointer
     *      translation (+ round-trip, - EFAULT on unmapped/NULL pointer).
     */
    char orig_name[16];
    char new_name[16]  = "a32rt_r4_test";
    char read_name[16];
    for (int i = 0; i < 16; i++) {
        orig_name[i] = '\0';
        read_name[i] = '\0';
    }

    if (sys5(172U /* prctl */, 16U /* PR_GET_NAME */,
             (uint32_t)(uintptr_t)orig_name, 0U, 0U, 0U) != 0) {
        return 135;
    }
    if (sys5(172U /* prctl */, 15U /* PR_SET_NAME */,
             (uint32_t)(uintptr_t)new_name, 0U, 0U, 0U) != 0) {
        return 136;
    }
    if (sys5(172U /* prctl */, 16U /* PR_GET_NAME */,
             (uint32_t)(uintptr_t)read_name, 0U, 0U, 0U) != 0) {
        sys5(172U /* prctl */, 15U /* PR_SET_NAME */,
             (uint32_t)(uintptr_t)orig_name, 0U, 0U, 0U);
        return 137;
    }
    /* Restore original thread name */
    sys5(172U /* prctl */, 15U /* PR_SET_NAME */,
         (uint32_t)(uintptr_t)orig_name, 0U, 0U, 0U);

    if (!m5_streq(read_name, "a32rt_r4_test")) {
        return 138;
    }

    /* Negative: PR_GET_NAME / PR_SET_NAME with NULL (0) or unmapped 0x100 -> -EFAULT (-14) */
    if (sys5(172U /* prctl */, 16U /* PR_GET_NAME */, 0U, 0U, 0U, 0U) != -14 ||
        sys5(172U /* prctl */, 15U /* PR_SET_NAME */, 0x100U, 0U, 0U, 0U) != -14) {
        return 139;
    }

    /*
     * 17b: getgroups32 (205) positive and negative checks:
     *      - Negative: size < 0 returns -EINVAL (-22); unmapped list with size > 0
     *        returns -EFAULT (-14)
     *      - Positive: getgroups32(0, NULL) returns count >= 0, and
     *        getgroups32(16, groups) succeeds
     */
    if (sys2(205U /* getgroups32 */, 0xFFFFFFFFU /* -1 */, 0U) != -22) {
        return 140;
    }
    int32_t ngroups = sys2(205U /* getgroups32 */, 0U, 0U);
    if (ngroups < 0) {
        return 141;
    }
    uint32_t gids[16];
    int32_t ng2 = sys2(205U /* getgroups32 */, 16U, (uint32_t)(uintptr_t)gids);
    if (ng2 != ngroups) {
        return 142;
    }
    if (ngroups > 0 && sys2(205U /* getgroups32 */, 16U, 0x100U) != -14) {
        return 143;
    }

    /*
     * 17c: set_robust_list (338) & get_robust_list (339):
     *      - Negative: set_robust_list(&head, 99) -> -EINVAL (-22)
     *      - Positive: set_robust_list(&head, 12) -> 0, and get_robust_list(0, &hptr, &hlen)
     *        returns head_ptr == &head and len == 12
     */
    static struct robust_list_head32_m5 rhead;
    rhead.list = (uint32_t)(uintptr_t)&rhead;
    rhead.futex_offset = 0;
    rhead.list_op_pending = 0U;

    if (sys2(338U /* set_robust_list */, (uint32_t)(uintptr_t)&rhead, 99U) != -22 ||
        sys2(338U /* set_robust_list */, (uint32_t)(uintptr_t)&rhead, 24U) != -22) {
        return 144;
    }
    if (sys2(338U /* set_robust_list */, (uint32_t)(uintptr_t)&rhead, 12U) != 0) {
        return 145;
    }

    uint32_t out_head = 0U;
    uint32_t out_len  = 0U;
    if (sys3(339U /* get_robust_list */, 0U,
             (uint32_t)(uintptr_t)&out_head,
             (uint32_t)(uintptr_t)&out_len) != 0 ||
        out_head != (uint32_t)(uintptr_t)&rhead ||
        out_len != 12U) {
        return 146;
    }

    /* Negative: get_robust_list with NULL pointers returns -EFAULT (-14) */
    if (sys3(339U /* get_robust_list */, 0U, 0U, (uint32_t)(uintptr_t)&out_len) != -14) {
        return 147;
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * Test 18: Round 5 regressions — mprotect (-ENOMEM/-EINVAL/len=0),
 *          mremap MAP_SHARED|MAP_ANONYMOUS across fork(), and
 *          execve on 0644 ARM32 ELF returning -EACCES in-place
 * -------------------------------------------------------------------------- */
static int test18_mprotect_mremap_shared_execve_noexec(void)
{
    /*
     * 18a: mprotect (125) positive and negative checks:
     *      - Negative: unmapped guest address (0x26000000U) -> -ENOMEM (-12)
     *      - Negative: invalid prot (0x100U) -> -EINVAL (-22)
     *      - Positive: len == 0 on aligned page -> 0
     */
    if (sys3(125U /* mprotect */, 0x26000000U, 4096U, 1U /* PROT_READ */) != -12 /* -ENOMEM */) {
        return 148;
    }
    if (sys3(125U /* mprotect */, 0x26000000U, 4096U, 0x100U /* invalid prot */) != -22 /* -EINVAL */) {
        return 149;
    }
    if (sys3(125U /* mprotect */, 0x26000000U, 0U, 1U /* PROT_READ */) != 0) {
        return 150;
    }

    /*
     * 18b: mremap on MAP_SHARED | MAP_ANONYMOUS (0x21U):
     *      1. Grow a 1-page MAP_SHARED | MAP_ANONYMOUS region in-place to 2 pages,
     *         then force a move to 3 pages with MREMAP_MAYMOVE (1U) by placing a
     *         blocker page immediately after it.
     *      2. Verify page contents are preserved and /proc/self/maps reports
     *         `rw-s` (MAP_SHARED) rather than `rw-p` (MAP_PRIVATE).
     */
    uint32_t sh_base = (uint32_t)sys6(192U /* mmap2 */, 0U, 4096U,
                                      3U /* PROT_READ|PROT_WRITE */,
                                      0x21U /* MAP_SHARED|MAP_ANONYMOUS */,
                                      0xFFFFFFFFU, 0U);
    if (sh_base > 0xFFFFF000U) {
        return 151;
    }

    volatile uint32_t *sh_p1 = (volatile uint32_t *)(uintptr_t)sh_base;
    sh_p1[0] = 0x11112222U;

    uint32_t sh_grown = (uint32_t)sys4(163U /* mremap */, sh_base, 4096U, 8192U,
                                       1U /* MREMAP_MAYMOVE */);
    if (sh_grown > 0xFFFFF000U || (sh_grown & 0xFFFU) != 0U) {
        sys2(91U /* munmap */, sh_base, 4096U);
        return 152;
    }

    volatile uint32_t *grown_p1 = (volatile uint32_t *)(uintptr_t)sh_grown;
    volatile uint32_t *grown_p2 = (volatile uint32_t *)(uintptr_t)(sh_grown + 4096U);
    if (grown_p1[0] != 0x11112222U) {
        sys2(91U /* munmap */, sh_grown, 8192U);
        return 153;
    }
    grown_p2[0] = 0x33334444U;

    /* Map a blocker page at sh_grown + 8192 to force MREMAP_MAYMOVE relocation */
    uint32_t blocker = (uint32_t)sys6(192U /* mmap2 */, sh_grown + 8192U, 4096U,
                                      1U /* PROT_READ */,
                                      0x32U /* MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED */,
                                      0xFFFFFFFFU, 0U);
    if (blocker != sh_grown + 8192U) {
        sys2(91U /* munmap */, sh_grown, 8192U);
        return 154;
    }

    uint32_t sh_moved = (uint32_t)sys4(163U /* mremap */, sh_grown, 8192U, 12288U,
                                       1U /* MREMAP_MAYMOVE */);
    sys2(91U /* munmap */, blocker, 4096U);
    if (sh_moved > 0xFFFFF000U || (sh_moved & 0xFFFU) != 0U || sh_moved == sh_grown) {
        sys2(91U /* munmap */, sh_grown, 8192U);
        return 155;
    }

    volatile uint32_t *moved_p1 = (volatile uint32_t *)(uintptr_t)sh_moved;
    volatile uint32_t *moved_p2 = (volatile uint32_t *)(uintptr_t)(sh_moved + 4096U);
    volatile uint32_t *moved_p3 = (volatile uint32_t *)(uintptr_t)(sh_moved + 8192U);
    moved_p3[0] = 0xCAFEBABEU;

    /* Check /proc/self/maps to verify sh_moved is recorded as rw-s (MAP_SHARED) */
    static const char maps_path[] = "/proc/self/maps";
    int32_t mfd = sys3(5U /* open */, (uint32_t)(uintptr_t)maps_path, 0U /* O_RDONLY */, 0U);
    if (mfd < 0) {
        sys2(91U /* munmap */, sh_moved, 12288U);
        return 156;
    }
    char maps_buf[4096];
    int32_t mn = sys3(3U /* read */, (uint32_t)mfd, (uint32_t)(uintptr_t)maps_buf,
                      (uint32_t)(sizeof(maps_buf) - 1U));
    sys1(6U /* close */, (uint32_t)mfd);
    if (mn <= 0) {
        sys2(91U /* munmap */, sh_moved, 12288U);
        return 156;
    }
    maps_buf[mn] = '\0';

    if (moved_p1[0] != 0x11112222U ||
        moved_p2[0] != 0x33334444U ||
        moved_p3[0] != 0xCAFEBABEU ||
        !my_strstr(maps_buf, "rw-s")) {
        sys2(91U /* munmap */, sh_moved, 12288U);
        return 156;
    }
    sys2(91U /* munmap */, sh_moved, 12288U);

    /*
     * 18c: Negative execve (11) on a 0644 (non-executable) file containing a
     *      valid 32-bit ARM ELF header must return -EACCES (-13) in-place
     *      without replacing the calling process!
     */
    static const char noexec_elf_path[] = "/tmp/a32rt_m5_noexec_arm32_elf";
    sys1(10U /* unlink */, (uint32_t)(uintptr_t)noexec_elf_path);

    int32_t efd = sys3(5U /* open */, (uint32_t)(uintptr_t)noexec_elf_path,
                       0x241U /* O_CREAT | O_TRUNC | O_WRONLY */, 0644U);
    if (efd < 0) {
        return 157;
    }

    uint8_t elf32_hdr[52];
    my_memset(elf32_hdr, 0, sizeof(elf32_hdr));
    elf32_hdr[0]  = 0x7FU;
    elf32_hdr[1]  = (uint8_t)'E';
    elf32_hdr[2]  = (uint8_t)'L';
    elf32_hdr[3]  = (uint8_t)'F';
    elf32_hdr[4]  = 1U;  /* ELFCLASS32 */
    elf32_hdr[5]  = 1U;  /* ELFDATA2LSB */
    elf32_hdr[6]  = 1U;  /* EV_CURRENT */
    elf32_hdr[16] = 2U;  /* ET_EXEC (lo) */
    elf32_hdr[17] = 0U;  /* ET_EXEC (hi) */
    elf32_hdr[18] = 40U; /* EM_ARM (lo) */
    elf32_hdr[19] = 0U;  /* EM_ARM (hi) */
    elf32_hdr[20] = 1U;  /* EV_CURRENT */

    int32_t nw = sys3(4U /* write */, (uint32_t)efd,
                      (uint32_t)(uintptr_t)elf32_hdr, (uint32_t)sizeof(elf32_hdr));
    sys1(6U /* close */, (uint32_t)efd);
    if (nw != (int32_t)sizeof(elf32_hdr)) {
        sys1(10U /* unlink */, (uint32_t)(uintptr_t)noexec_elf_path);
        return 158;
    }
    /* Ensure mode is strictly 0644 (no execute bits) */
    (void)sys2(15U /* chmod */, (uint32_t)(uintptr_t)noexec_elf_path, 0644U);

    uint32_t ex_argv[2];
    ex_argv[0] = (uint32_t)(uintptr_t)noexec_elf_path;
    ex_argv[1] = 0U;
    uint32_t ex_envp[1];
    ex_envp[0] = 0U;

    int32_t ex_rc = sys3(11U /* execve */,
                         (uint32_t)(uintptr_t)noexec_elf_path,
                         (uint32_t)(uintptr_t)ex_argv,
                         (uint32_t)(uintptr_t)ex_envp);
    sys1(10U /* unlink */, (uint32_t)(uintptr_t)noexec_elf_path);

    if (ex_rc != -13 /* -EACCES */) {
        return 159;
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * Test 19: Round 6 regressions —
 *   19a: madvise (220) negative (-ENOMEM on unmapped page, -EINVAL on
 *        unaligned address) & positive (0 on mapped page)
 *   19b: get_robust_list (339) pid validation (pid < 0 -> -EINVAL,
 *        non-existent pid -> -ESRCH, pid == 0 and pid == gettid() -> 0)
 *   19c: setsockopt (294) / getsockopt (295) SO_RCVTIMEO_OLD (20) with
 *        optlen < 8 -> -EINVAL and getsockopt with larger 16-byte optlen
 *        buffer updating optlen to 8
 * -------------------------------------------------------------------------- */
static int test19_r6_madvise_robust_sockopt(void)
{
    /*
     * 19a: madvise (220):
     *   - Negative: unmapped guest page (0x26000000U, 4096, MADV_NORMAL) -> -ENOMEM (-12)
     *   - Negative: unaligned address -> -EINVAL (-22)
     *   - Positive: mapped guest page -> 0
     */
    if (sys3(220U /* madvise */, 0x26000000U, 4096U, 0U /* MADV_NORMAL */) != -12 /* -ENOMEM */) {
        return 160;
    }

    uint32_t mpage = (uint32_t)sys6(192U /* mmap2 */, 0U, 4096U,
                                    3U /* PROT_READ|PROT_WRITE */,
                                    0x22U /* MAP_PRIVATE|MAP_ANONYMOUS */,
                                    0xFFFFFFFFU, 0U);
    if (mpage > 0xFFFFF000U) {
        return 161;
    }

    if (sys3(220U /* madvise */, mpage + 1U, 4096U, 0U /* MADV_NORMAL */) != -22 /* -EINVAL */) {
        sys2(91U /* munmap */, mpage, 4096U);
        return 162;
    }
    if (sys3(220U /* madvise */, mpage, 4096U, 0U /* MADV_NORMAL */) != 0 ||
        sys3(220U /* madvise */, mpage, 4096U, 4U /* MADV_DONTNEED */) != 0) {
        sys2(91U /* munmap */, mpage, 4096U);
        return 163;
    }
    sys2(91U /* munmap */, mpage, 4096U);

    /*
     * 19b: get_robust_list (339):
     *   - Negative: pid < 0 (-1) -> -EINVAL (-22)
     *   - Negative: non-existent pid (999999) -> -ESRCH (-3)
     *   - Positive: pid == 0 and pid == gettid() -> 0
     */
    static struct robust_list_head32_m5 rhead;
    rhead.list = (uint32_t)(uintptr_t)&rhead;
    rhead.futex_offset = -4;
    rhead.list_op_pending = 0U;

    if (sys2(338U /* set_robust_list */, (uint32_t)(uintptr_t)&rhead, 12U) != 0) {
        return 164;
    }

    uint32_t out_head = 0U;
    uint32_t out_len  = 0U;
    if (sys3(339U /* get_robust_list */, 0xFFFFFFFFU /* -1 */,
             (uint32_t)(uintptr_t)&out_head,
             (uint32_t)(uintptr_t)&out_len) != -22 /* -EINVAL */) {
        return 165;
    }
    if (sys3(339U /* get_robust_list */, 999999U,
             (uint32_t)(uintptr_t)&out_head,
             (uint32_t)(uintptr_t)&out_len) != -3 /* -ESRCH */) {
        return 166;
    }

    int32_t my_tid = sys1(224U /* gettid */, 0U);
    if (my_tid <= 0) {
        return 167;
    }

    out_head = 0U;
    out_len  = 0U;
    if (sys3(339U /* get_robust_list */, 0U,
             (uint32_t)(uintptr_t)&out_head,
             (uint32_t)(uintptr_t)&out_len) != 0 ||
        out_head != (uint32_t)(uintptr_t)&rhead ||
        out_len != 12U) {
        return 168;
    }

    out_head = 0U;
    out_len  = 0U;
    if (sys3(339U /* get_robust_list */, (uint32_t)my_tid,
             (uint32_t)(uintptr_t)&out_head,
             (uint32_t)(uintptr_t)&out_len) != 0 ||
        out_head != (uint32_t)(uintptr_t)&rhead ||
        out_len != 12U) {
        return 169;
    }

    /*
     * 19c: setsockopt (294) / getsockopt (295) SO_RCVTIMEO_OLD (20):
     *   - Negative: setsockopt with optlen < 8 (4) -> -EINVAL (-22)
     *   - Positive: setsockopt with optlen == 8 -> 0, and getsockopt with a
     *     larger 16-byte optlen buffer updates optlen to 8 and writes the
     *     expected 32-bit timeval without clobbering bytes [8..15].
     */
    int sv[2];
    sv[0] = -1;
    sv[1] = -1;
    if (sys4(288U /* socketpair */, 1U /* AF_UNIX */, 1U /* SOCK_STREAM */, 0U,
             (uint32_t)(uintptr_t)sv) != 0) {
        return 170;
    }

    struct timeval32 tv_in;
    tv_in.tv_sec  = 3;
    tv_in.tv_usec = 250000;

    if (sys5(294U /* setsockopt */, (uint32_t)sv[0], 1U /* SOL_SOCKET */,
             20U /* SO_RCVTIMEO_OLD */, (uint32_t)(uintptr_t)&tv_in, 4U) != -22 /* -EINVAL */) {
        sys1(6U /* close */, (uint32_t)sv[0]);
        sys1(6U /* close */, (uint32_t)sv[1]);
        return 171;
    }

    if (sys5(294U /* setsockopt */, (uint32_t)sv[0], 1U /* SOL_SOCKET */,
             20U /* SO_RCVTIMEO_OLD */, (uint32_t)(uintptr_t)&tv_in, 8U) != 0) {
        sys1(6U /* close */, (uint32_t)sv[0]);
        sys1(6U /* close */, (uint32_t)sv[1]);
        return 172;
    }

    struct {
        struct timeval32 tv;
        uint32_t         canary[2];
    } opt_buf;
    opt_buf.tv.tv_sec  = 0;
    opt_buf.tv.tv_usec = 0;
    opt_buf.canary[0]  = 0xDEADBEEFU;
    opt_buf.canary[1]  = 0xCAFEBABEU;
    uint32_t optlen = 16U;

    int32_t gso_rc = sys5(295U /* getsockopt */, (uint32_t)sv[0], 1U /* SOL_SOCKET */,
                          20U /* SO_RCVTIMEO_OLD */,
                          (uint32_t)(uintptr_t)&opt_buf,
                          (uint32_t)(uintptr_t)&optlen);
    sys1(6U /* close */, (uint32_t)sv[0]);
    sys1(6U /* close */, (uint32_t)sv[1]);

    if (gso_rc != 0 ||
        optlen != 8U ||
        opt_buf.tv.tv_sec != 3 ||
        opt_buf.tv.tv_usec < 200000 ||
        opt_buf.tv.tv_usec > 300000 ||
        opt_buf.canary[0] != 0xDEADBEEFU ||
        opt_buf.canary[1] != 0xCAFEBABEU) {
        return 173;
    }

    return 0;
}

/* --------------------------------------------------------------------------
 * Test 20: Round 7 regressions —
 *   20a: truncate (92) and ftruncate (93) positive 32-bit file truncation &
 *        extension and negative (-EINVAL on negative length, -EFAULT on
 *        invalid path, -EBADF on invalid fd)
 *   20b: sched_getaffinity (242) positive return > 0 & non-zero CPU mask and
 *        negative (-EFAULT on invalid mask pointer, -ESRCH on bad pid)
 *   20c: gettimeofday (78) with non-NULL tz pointer (+ valid tv & tz and
 *        NULL tv with non-NULL tz, - EFAULT on invalid tv or tz pointer)
 * -------------------------------------------------------------------------- */

int run_test_m5_part2(void)
{
    int rc = test14_rwx_mmap_mprotect();
    if (rc != 0) return rc;
    rc = test15_ppoll_sigsetsize();
    if (rc != 0) return rc;
    rc = test16_getdents32_pagination();
    if (rc != 0) return rc;
    rc = test17_prctl_groups_robust();
    if (rc != 0) return rc;
    rc = test18_mprotect_mremap_shared_execve_noexec();
    if (rc != 0) return rc;
    return test19_r6_madvise_robust_sockopt();
}
