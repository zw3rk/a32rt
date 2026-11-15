/*
 * src/ios/ios.h — 32-bit iOS (armv7 / armv7s) Mach-O, CommPage, and XNU ABI
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Frozen module interface shared by `src/main.c`, `src/signals.c`,
 * `src/os/os_sys_thread_sig.c`, and the `src/ios/` implementation units.
 * Curated C11, <= 900 lines.
 */

#ifndef A32RT_IOS_H
#define A32RT_IOS_H

#include "a32rt.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- Mach-O 32-bit & Universal (Fat) wire constants ---- */

#define IOS_MH_MAGIC                 0xFEEDFACEU
#define IOS_MH_CIGAM                 0xCEFAEDFEU
#define IOS_FAT_MAGIC                0xCAFEBABEU
#define IOS_FAT_CIGAM                0xBEBAFECAUL

#define IOS_CPU_TYPE_ARM             12
#define IOS_CPU_SUBTYPE_MASK         0x00FFFFFFU
#define IOS_CPU_SUBTYPE_ARM_ALL      0U
#define IOS_CPU_SUBTYPE_ARM_V6       6U
#define IOS_CPU_SUBTYPE_ARM_V7       9U
#define IOS_CPU_SUBTYPE_ARM_V7F      10U
#define IOS_CPU_SUBTYPE_ARM_V7S      11U
#define IOS_CPU_SUBTYPE_ARM_V7K      12U
#define IOS_CPU_SUBTYPE_ARM_V8       13U

#define IOS_MH_EXECUTE               0x2U
#define IOS_MH_DYLIB                 0x6U
#define IOS_MH_DYLINKER              0x7U
#define IOS_MH_PIE                   0x00200000U

#define IOS_LC_REQ_DYLD              0x80000000U
#define IOS_LC_SEGMENT               0x00000001U
#define IOS_LC_SYMTAB                0x00000002U
#define IOS_LC_UNIXTHREAD            0x00000005U
#define IOS_LC_DYSYMTAB              0x0000000BU
#define IOS_LC_LOAD_DYLIB            0x0000000CU
#define IOS_LC_ID_DYLIB              0x0000000DU
#define IOS_LC_LOAD_DYLINKER         0x0000000EU
#define IOS_LC_ID_DYLINKER           0x0000000FU
#define IOS_LC_ROUTINES              0x00000011U
#define IOS_LC_LOAD_WEAK_DYLIB       (0x00000018U | IOS_LC_REQ_DYLD)
#define IOS_LC_UUID                  0x0000001BU
#define IOS_LC_RPATH                 (0x0000001CU | IOS_LC_REQ_DYLD)
#define IOS_LC_CODE_SIGNATURE        0x0000001DU
#define IOS_LC_REEXPORT_DYLIB        (0x0000001FU | IOS_LC_REQ_DYLD)
#define IOS_LC_LAZY_LOAD_DYLIB       0x00000020U
#define IOS_LC_ENCRYPTION_INFO       0x00000021U
#define IOS_LC_DYLD_INFO             0x00000022U
#define IOS_LC_DYLD_INFO_ONLY        (0x00000022U | IOS_LC_REQ_DYLD)
#define IOS_LC_LOAD_UPWARD_DYLIB     (0x00000023U | IOS_LC_REQ_DYLD)
#define IOS_LC_FUNCTION_STARTS       0x00000026U
#define IOS_LC_MAIN                  (0x00000028U | IOS_LC_REQ_DYLD)
#define IOS_LC_DATA_IN_CODE          0x00000029U

#define IOS_SECTION_TYPE_MASK        0x000000FFU
#define IOS_S_NON_LAZY_SYMBOL_PTRS   0x06U
#define IOS_S_LAZY_SYMBOL_PTRS       0x07U
#define IOS_S_MOD_INIT_FUNC_PTRS     0x09U

#define IOS_N_STAB                   0xE0U
#define IOS_N_TYPE                   0x0EU
#define IOS_N_EXT                    0x01U
#define IOS_N_SECT                   0x0EU
#define IOS_N_ABS                    0x02U
#define IOS_N_ARM_THUMB_DEF          0x0008U

/* ---- Guest virtual address layout inside [G, G + 4 GiB) ---- */

#define A32_IOS_PAGEZERO_DEFAULT     0x00004000U
#define A32_IOS_PIE_BASE             0x00004000U
#define A32_IOS_DYLIB_BASE           0x10000000U
#define A32_IOS_DYLD_BASE            0x2FE00000U
#define A32_IOS_HIGH_MIN_VA          0xFFFF0000U
#define A32_IOS_HIGH_MAX_VA          0xFFFF8000U

#define A32_IOS_COMMPAGE_LEGACY_VA   0xFFFF0000U /* 4 KiB RO mirror */
#define A32_IOS_TRAMP_BASE_VA        0xFFFF1000U /* 12 KiB RX helper pages */
#define A32_IOS_TRAMP_SIZE           0x00003000U
#define A32_IOS_CRT0_VA              0xFFFF1000U /* Thumb-2 LC_MAIN + init runner */
#define A32_IOS_SIGTRAMP_VA          0xFFFF1800U /* Thumb-2 __sigtramp -> SYS_sigreturn */
#define A32_IOS_THRTRAMP_VA          0xFFFF1900U /* Thumb-2 pthread start wrapper */
#define A32_IOS_STUB_BASE_VA         0xFFFF2000U /* 256 * 8B Thumb-2 svc #0x81 stubs */
#define A32_IOS_MAX_STUBS            256U
#define A32_IOS_LEAF_STUB_VA         0xFFFF2800U /* 6 KiB native Thumb-2 leaf stubs */
#define A32_IOS_LEAF_STUB_SIZE       0x00001800U
#define A32_IOS_C_EXIT_STUB_VA       ((A32_IOS_STUB_BASE_VA + 5U * 8U) | 1U)
#define A32_IOS_COMMPAGE_VA          0xFFFF4000U /* 4 KiB RO XNU 32-bit _COMM_PAGE */
#define A32_IOS_RUNTIME_DATA_VA      0xFFFF5000U /* 4 KiB RW TSD/pthread/errno/stdio */

/* Offsets inside A32_IOS_RUNTIME_DATA_VA (0xFFFF5000) */
#define A32_IOS_TSD_OFF              0x000U      /* TPIDRURO base: slot[0]=self */
#define A32_IOS_MAIN_PTHREAD_OFF     0x400U      /* struct _opaque_pthread_t */
#define A32_IOS_MAIN_ERRNO_OFF       0x500U      /* int errno */
#define A32_IOS_STACK_GUARD_OFF      0x510U      /* ___stack_chk_guard */
#define A32_IOS_ENVIRON_PTR_OFF      0x520U      /* _environ (char ***) */
#define A32_IOS_STDINP_OFF           0x530U      /* ___stdinp */
#define A32_IOS_STDOUTP_OFF          0x534U      /* ___stdoutp */
#define A32_IOS_STDERRP_OFF          0x538U      /* ___stderrp */
#define A32_IOS_OPTIND_OFF           0x540U      /* _optind */
#define A32_IOS_OPTARG_OFF           0x544U      /* _optarg */
#define A32_IOS_OPTERR_OFF           0x548U      /* _opterr */
#define A32_IOS_OPTOPT_OFF           0x54CU      /* _optopt */
#define A32_IOS_STDIO_FILES_OFF      0x600U      /* 3 synthetic FILE objects */

/* ---- Mach-O wire structs (little-endian ARM32; Fat header is big-endian) ---- */

typedef struct ios_mach_header {
    uint32_t magic;
    int32_t  cputype;
    int32_t  cpusubtype;
    uint32_t filetype;
    uint32_t ncmds;
    uint32_t sizeofcmds;
    uint32_t flags;
} ios_mach_header_t;

typedef struct ios_load_command {
    uint32_t cmd;
    uint32_t cmdsize;
} ios_load_command_t;

typedef struct ios_segment_command {
    uint32_t cmd;
    uint32_t cmdsize;
    char     segname[16];
    uint32_t vmaddr;
    uint32_t vmsize;
    uint32_t fileoff;
    uint32_t filesize;
    int32_t  maxprot;
    int32_t  initprot;
    uint32_t nsects;
    uint32_t flags;
} ios_segment_command_t;

typedef struct ios_section {
    char     sectname[16];
    char     segname[16];
    uint32_t addr;
    uint32_t size;
    uint32_t offset;
    uint32_t align;
    uint32_t reloff;
    uint32_t nreloc;
    uint32_t flags;
    uint32_t reserved1;
    uint32_t reserved2;
} ios_section_t;

typedef struct ios_dyld_info_command {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t rebase_off;
    uint32_t rebase_size;
    uint32_t bind_off;
    uint32_t bind_size;
    uint32_t weak_bind_off;
    uint32_t weak_bind_size;
    uint32_t lazy_bind_off;
    uint32_t lazy_bind_size;
    uint32_t export_off;
    uint32_t export_size;
} ios_dyld_info_command_t;

typedef struct ios_symtab_command {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t symoff;
    uint32_t nsyms;
    uint32_t stroff;
    uint32_t strsize;
} ios_symtab_command_t;

typedef struct ios_linkedit_data_command {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t dataoff;
    uint32_t datasize;
} ios_linkedit_data_command_t;

typedef struct ios_dysymtab_command {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t ilocalsym, nlocalsym;
    uint32_t iextdefsym, nextdefsym;
    uint32_t iundefsym, nundefsym;
    uint32_t tocoff, ntoc;
    uint32_t modtaboff, nmodtab;
    uint32_t extrefsymoff, nextrefsyms;
    uint32_t indirectsymoff, nindirectsyms;
    uint32_t extreloff, nextrel;
    uint32_t locreloff, nlocrel;
} ios_dysymtab_command_t;

typedef struct ios_nlist32 {
    uint32_t n_strx;
    uint8_t  n_type;
    uint8_t  n_sect;
    uint16_t n_desc;
    uint32_t n_value;
} ios_nlist32_t;

/* ---- Loaded image metadata shared between ios_macho.c and ios_bind.c ---- */

#define A32_IOS_MAX_SEGMENTS 16U
#define A32_IOS_MAX_DEPS     64U
#define A32_IOS_MAX_RPATHS   16U
#define A32_IOS_MAX_IMAGES   64U

typedef struct a32_ios_seg_map {
    char     segname[17];
    uint32_t vmaddr;     /* Slid guest VA */
    uint32_t vmsize;     /* Page-aligned size */
    uint32_t fileoff;
    uint32_t filesize;
    uint32_t initprot;
    uint32_t maxprot;
    bool     is_pagezero;
} a32_ios_seg_map_t;

typedef struct a32_ios_image {
    char                    path[512];
    char                    install_name[256];
    int                     fd;           /* Owned fd (>= 0 while loading/binding, -1 if synthetic) */
    uint64_t                slice_off;    /* File offset of thin ARM32 Mach-O slice */
    uint64_t                slice_size;   /* Byte length of slice */
    uint32_t                mh_va;        /* Guest VA of mach_header */
    uint32_t                slide;        /* VM slide applied */
    uint32_t                filetype;
    bool                    is_synthetic; /* True for built-in synthetic libSystem */
    size_t                  seg_count;
    a32_ios_seg_map_t       segs[A32_IOS_MAX_SEGMENTS];
    bool                    has_dyld_info;
    ios_dyld_info_command_t dyld_info;
    bool                    has_symtab;
    ios_symtab_command_t    symtab;
    bool                    has_dysymtab;
    ios_dysymtab_command_t  dysymtab;
    size_t                  dep_count;
    int                     dep_image_idx[A32_IOS_MAX_DEPS]; /* Index in images[], or -1 weak-missing */
    bool                    dep_weak[A32_IOS_MAX_DEPS];
    size_t                  rpath_count;
    char                    rpaths[A32_IOS_MAX_RPATHS][256];
    uint64_t                st_dev;
    uint64_t                st_ino;
    bool                    has_fn_starts;
    ios_linkedit_data_command_t fn_starts;
    uint8_t                *cached_export;
    uint8_t                *cached_symtab;
    uint8_t                *cached_strtab;
} a32_ios_image_t;

/* ---- Compiler-verified 32-bit iOS (APCS-GNU 4B-aligned) kernel structs ---- */

typedef struct __attribute__((packed, aligned(4))) ios_stat64 {
    int32_t  st_dev;            /* 0 */
    uint16_t st_mode;           /* 4 */
    uint16_t st_nlink;          /* 6 */
    uint64_t st_ino;            /* 8 */
    uint32_t st_uid;            /* 16 */
    uint32_t st_gid;            /* 20 */
    int32_t  st_rdev;           /* 24 */
    int32_t  st_atime_sec;      /* 28 */
    int32_t  st_atime_nsec;     /* 32 */
    int32_t  st_mtime_sec;      /* 36 */
    int32_t  st_mtime_nsec;     /* 40 */
    int32_t  st_ctime_sec;      /* 44 */
    int32_t  st_ctime_nsec;     /* 48 */
    int32_t  st_btime_sec;      /* 52 */
    int32_t  st_btime_nsec;     /* 56 */
    int64_t  st_size;           /* 60 (4-byte aligned on APCS-GNU!) */
    int64_t  st_blocks;         /* 68 */
    int32_t  st_blksize;        /* 76 */
    uint32_t st_flags;          /* 80 */
    uint32_t st_gen;            /* 84 */
    int32_t  st_lspare;         /* 88 */
    int64_t  st_qspare[2];      /* 92..108 */
} ios_stat64_t;

_Static_assert(sizeof(ios_stat64_t) == 108, "ios_stat64_t must be 108 bytes");

typedef struct __attribute__((packed, aligned(4))) ios_ostat32 {
    int32_t  st_dev;            /* 0 */
    uint32_t st_ino;            /* 4 */
    uint16_t st_mode;           /* 8 */
    uint16_t st_nlink;          /* 10 */
    uint32_t st_uid;            /* 12 */
    uint32_t st_gid;            /* 16 */
    int32_t  st_rdev;           /* 20 */
    int32_t  st_atime_sec;      /* 24 */
    int32_t  st_atime_nsec;     /* 28 */
    int32_t  st_mtime_sec;      /* 32 */
    int32_t  st_mtime_nsec;     /* 36 */
    int32_t  st_ctime_sec;      /* 40 */
    int32_t  st_ctime_nsec;     /* 44 */
    int64_t  st_size;           /* 48 */
    int64_t  st_blocks;         /* 56 */
    int32_t  st_blksize;        /* 64 */
    uint32_t st_flags;          /* 68 */
    uint32_t st_gen;            /* 72 */
    int32_t  st_lspare;         /* 76 */
    int64_t  st_qspare[2];      /* 80..96 */
} ios_ostat32_t;

_Static_assert(sizeof(ios_ostat32_t) == 96, "ios_ostat32_t must be 96 bytes");

/* ---- Public & cross-unit iOS personality API ---- */

/* Loader & dynamic binder (ios_macho.c, ios_dylib.c, ios_export.c, ios_bind.c) */
typedef struct a32_ios_parsed {
    char     dylinker[256];
    bool     has_main;
    uint32_t main_entryoff;
    bool     has_thread;
    uint32_t tstate[16];
    uint32_t tcpsr;
    size_t   ndeps;
    char     deps[A32_IOS_MAX_DEPS][256];
    bool     depweak[A32_IOS_MAX_DEPS];
} a32_ios_parsed_t;

bool     a32_ios_is_macho_fd(int fd, const char *path, const a32_ctx_t *ctx);
int      a32_ios_pread_full(int fd, void *buf, size_t count, uint64_t offset);
int      a32_ios_pick_slice(int fd, uint64_t file_size, uint64_t *out_off, uint64_t *out_size);
int      a32_ios_parse_map(a32_ctx_t *ctx, a32_ios_image_t *img, a32_ios_parsed_t *par,
                           uint32_t *dylib_bump);
int      a32_ios_load(a32_ctx_t *ctx, int exec_fd, const char *path, a32_elf_info_t *out_info);
uint8_t *a32_ios_read_blob(const a32_ios_image_t *img, uint32_t off, uint32_t sz);
bool     a32_ios_rd_uleb(const uint8_t *buf, size_t size, size_t *pos, uint64_t *out);
bool     a32_ios_rd_sleb(const uint8_t *buf, size_t size, size_t *pos, int64_t *out);
uint32_t a32_ios_resolve_in(a32_ctx_t *ctx, a32_ios_image_t *images,
                            size_t image_count, size_t img_idx,
                            const char *sym_name, unsigned depth, bool *out_found);
void     a32_ios_free_image_caches(a32_ios_image_t *images, size_t image_count);
int      a32_ios_bind_all(a32_ctx_t *ctx, a32_ios_image_t *images, size_t image_count);
uint32_t a32_ios_lookup_synth_sym(a32_ctx_t *ctx, const char *sym_name, bool *out_found);
uint32_t a32_ios_lookup_synth_ext(a32_ctx_t *ctx, const char *sym_name, bool *out_found);
int      a32_ios_emit_native_leaf_stubs(a32_ctx_t *ctx);
int      a32_ios_handle_stub_ext(a32_ctx_t *ctx, uint32_t stub_id, bool *out_handled);
void     a32_ios_exit_teardown(a32_ctx_t *ctx, int code);
void     a32_ios_refresh_runtime_pid(a32_ctx_t *ctx);
int      a32_ios_emit_time_pid_leaf_stubs(a32_ctx_t *ctx, uint32_t *io_cur_va);
uint32_t a32_ios_lookup_time_pid_leaf(const char *sym_name, bool *out_found);

/* CommPage, trampolines, and Darwin stack setup (ios_commpage_stack.c) */
int      a32_ios_commpage_setup(a32_ctx_t *ctx);
int      a32_ios_stack_setup(a32_ctx_t *ctx, const a32_elf_info_t *info,
                             int guest_argc, char **guest_argv, char **host_envp,
                             uint32_t *out_sp);

/* BSD syscalls, Mach/ARM traps, threads & synthetic libSystem stubs */
int      a32_ios_host_to_darwin_errno(int host_err);
void     a32_ios_bsd_ok(a32_ctx_t *ctx, uint32_t r0, uint32_t r1, bool dual_ret);
void     a32_ios_bsd_err(a32_ctx_t *ctx, int darwin_errno);
int      a32_ios_handle_svc(a32_ctx_t *ctx);
int      a32_ios_dispatch_bsd_fs(a32_ctx_t *ctx, uint32_t bsd_nr,
                                 const uint32_t *args, bool *out_handled);
int      a32_ios_dispatch_bsd_proc_net(a32_ctx_t *ctx, uint32_t bsd_nr,
                                       const uint32_t *args, bool *out_handled);
int      a32_ios_dispatch_bsd_sock(a32_ctx_t *ctx, uint32_t bsd_nr,
                                   const uint32_t *args, bool *out_handled);
int      a32_ios_dispatch_bsd_kevent(a32_ctx_t *ctx, uint32_t bsd_nr,
                                     const uint32_t *args, bool *out_handled);
void     a32_ios_kq_close(int fd);
void     a32_ios_kq_atfork_reset(void);
int      a32_ios_dispatch_mach_or_arm_trap(a32_ctx_t *ctx, uint32_t raw_r12);
uint32_t a32_ios_mach_thread_self_port(a32_ctx_t *ctx);
int      a32_ios_dispatch_thread_sync_bsd(a32_ctx_t *ctx, uint32_t bsd_nr,
                                          const uint32_t *args, bool *out_handled);
int      a32_ios_thread_kill_target(a32_ctx_t *caller, uint32_t target_id, int host_sig);
int      a32_ios_stub_pthread(a32_ctx_t *ctx, uint32_t stub_id, bool *out_handled);
int      a32_ios_handle_stub_svc(a32_ctx_t *ctx);

/* Signals (ios_signals.c) */
int      a32_ios_host_to_darwin_sig(int host_sig);
int      a32_ios_darwin_to_host_sig(int darwin_sig);
void     a32_ios_signals_enqueue_host(a32_ctx_t *ctx, int host_sig, const void *host_siginfo);
void     a32_ios_signals_check_pending(a32_ctx_t *ctx);
bool     a32_ios_signals_deliver_sync(a32_ctx_t *ctx, int host_sig, uint32_t fault_addr);
int      a32_ios_sys_sigaction(a32_ctx_t *ctx, int dsig, uint32_t nsv_va, uint32_t osv_va);
int      a32_ios_sys_sigprocmask(a32_ctx_t *ctx, int how, uint32_t set_va, uint32_t oset_va);
int      a32_ios_sys_sigpending(a32_ctx_t *ctx, uint32_t osv_va);
int      a32_ios_sys_sigaltstack(a32_ctx_t *ctx, uint32_t nss_va, uint32_t oss_va);
int      a32_ios_sys_sigsuspend(a32_ctx_t *ctx, uint32_t mask);
int      a32_ios_sys_kill(a32_ctx_t *ctx, int pid, int dsig, int posix);
int      a32_ios_sys_pthread_kill(a32_ctx_t *ctx, uint32_t port_or_tid, int dsig);
int      a32_ios_sys_sigreturn(a32_ctx_t *ctx, uint32_t uctx_va, int infostyle);

#endif /* A32RT_IOS_H */
