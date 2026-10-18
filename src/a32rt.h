/*
 * src/a32rt.h — Core context, constants, and module interfaces for a32rt
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef A32RT_H
#define A32RT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "a32host.h"

/* CPSR bit definitions */
#define A32_CPSR_N (1U << 31)
#define A32_CPSR_Z (1U << 30)
#define A32_CPSR_C (1U << 29)
#define A32_CPSR_V (1U << 28)
#define A32_CPSR_Q (1U << 27)
#define A32_CPSR_T (1U << 5)
#define A32_CPSR_NZCV_MASK 0xF0000000U
#define A32_CPSR_GE_MASK   0x000F0000U

/* Block exit reasons returned by a32_jit_run_block */
#define A32_EXIT_NONE      0U
#define A32_EXIT_BRANCH    1U /* Non-interworking branch; preserve CPSR.T */
#define A32_EXIT_INTERWORK 2U /* Interworking branch; bit 0 of next_pc sets CPSR.T */
#define A32_EXIT_SVC       3U /* Supervisor call (SVC #imm) */
#define A32_EXIT_UNDEF     4U /* Undefined / unhandled instruction */
#define A32_EXIT_FAULT     5U /* Guest memory fault */
#define A32_EXIT_BKPT      6U /* Breakpoint / GDB trap */

/* Maximum number of tracked guest VMA regions */
#define A32_MAX_REGIONS    512U
#define A32_RAS_SIZE       64U
#define A32_IBTC_SIZE      1024U
#define A32_PAGE_SIZE      4096U
#define A32_LOW_GUARD_SIZE 0x00010000U
#define A32_KUSER_BASE_32  0xFFFF0000U

typedef struct a32_region {
    uint32_t start;
    uint32_t end;
    uint32_t prot;
    uint32_t flags;
    char     name[64];
} a32_region_t;

typedef struct a32_ibtc_entry {
    uint32_t    key;
    uint32_t    epoch;
    const void *host_code;
    void       *entry;
    uint64_t    pad;
} a32_ibtc_entry_t;

_Static_assert(sizeof(a32_ibtc_entry_t) == 32, "a32_ibtc_entry_t must be 32 bytes");

typedef struct a32_pc_map_stage {
    uint32_t host_word_idx;
    uint32_t guest_pc;
    uint32_t itstate;
} a32_pc_map_stage_t;

/*
 * Architectural + runtime state for a 32-bit ARM guest thread.
 * Field offsets of the first 35248 bytes are ABI-locked with `src/trampoline.S`
 * and `src/a64_emit.h`.
 */
typedef struct a32_ctx {
    uint32_t regs[16];          /* 0x00: r0..r15 (r13=sp, r14=lr, r15=pc) */
    uint32_t cpsr;              /* 0x40: NZCV(31:28), Q(27), GE(19:16), IT, T(5) */
    uint32_t tls_tp;            /* 0x44: CP15 c13 TPIDRURO */
    uint64_t guest_base;        /* 0x48: Host base address G of [G, G+4GiB) window */
    uint32_t exit_reason;       /* 0x50: A32_EXIT_* */
    uint32_t svc_imm;           /* 0x54: SVC immediate */
    uint32_t pad_it;            /* 0x58: Reserved alignment slot */
    uint32_t fault_addr;        /* 0x5c: Faulting guest address */
    uint64_t vfp_d[32];         /* 0x60: VFPv4-D32 / NEON registers (D0..D31 / S0..S31 / Q0..Q15) */
    uint32_t fpscr;             /* 0x160: VFP/NEON Floating-Point Status and Control Register */
    uint32_t pad0;              /* 0x164: Alignment padding */

    /* ABI-locked fast-dispatch fields for `src/trampoline.S` (offsets 360..35248) */
    uint64_t          sig_mask;           /* 0x168 (360) */
    volatile uint64_t pending_signals;    /* 0x170 (368) */
    bool              trace_enabled;      /* 0x178 (376) */
    bool              gdb_enabled;        /* 0x179 (377) */
    bool              single_step;        /* 0x17a (378) */
    volatile bool     sync_sig_delivered; /* 0x17b (379) */
    uint32_t          itstate;            /* 0x17c (380): Thumb IT block state (packed in 128b load at 368) */
    uint64_t          blocks_executed;    /* 0x180 (384) */
    uint64_t          chain_hits;         /* 0x188 (392) */
    void             *last_exec_entry;    /* 0x190 (400) */
    uint64_t          ibtc_hits;          /* 0x198 (408) */
    uint64_t          ras_hits;           /* 0x1a0 (416) */
    uint32_t          ras_top;            /* 0x1a8 (424) */
    uint32_t          pad2;               /* 0x1ac (428) */
    a32_ibtc_entry_t  ras[A32_RAS_SIZE];  /* 0x1b0 (432) */
    a32_ibtc_entry_t  ibtc[A32_IBTC_SIZE];/* 0x9b0 (2480) */

    /* Guest memory layout & allocator inside [G, G + 4GiB) */
    uint32_t brk_base;
    uint32_t brk_curr;
    uint32_t mmap_cursor;       /* Bottom-up allocation cursor for guest mmap2 */
    uint32_t interp_base;
    uint32_t elf_base;
    uint32_t elf_entry;
    uint32_t stack_top;
    uint32_t stack_base;

    /* Host capabilities from /dev/a32host */
    a32h_info_t host_info;
    bool        has_host_info;

    /* Tracked guest regions (sorted by start) for /proc/self/maps and gap finding */
    a32_region_t regions[A32_MAX_REGIONS];
    size_t       region_count;

    /* Thread, futex, and signal state */
    uint32_t          clear_child_tid;
    uint32_t          robust_list_head;
    uint32_t          robust_list_len;
    uint32_t          altstack_sp;
    uint32_t          altstack_size;
    uint32_t          altstack_flags;
    int               host_tid;
    bool              is_thread_clone;

    /* Guest startup metadata for /proc/self/{cmdline,auxv,exe} */
    int      guest_argc;
    char   **guest_argv;
    char   **host_envp;
    uint32_t auxv_data[64];
    size_t   auxv_bytes;

    /* Configuration, AOT, GDB & instrumentation */
    const char *sysroot;
    const char *exec_path;
    const char *cache_dir;
    bool        stats_enabled;
    bool        aot_mode;
    int         gdb_port;
    uint64_t    blocks_compiled;
    uint64_t    syscalls_executed;
    uint64_t    icache_flushes;
    uint64_t    wx_transitions;
    uint64_t    flags_elided;
} a32_ctx_t;

#define A32_CTX_VFP_OFFSET   0x60U
#define A32_CTX_FPSCR_OFFSET 0x160U

_Static_assert(offsetof(a32_ctx_t, regs) == 0, "regs must be at offset 0");
_Static_assert(offsetof(a32_ctx_t, cpsr) == 64, "cpsr must be at offset 64");
_Static_assert(offsetof(a32_ctx_t, tls_tp) == 68, "tls_tp must be at offset 68");
_Static_assert(offsetof(a32_ctx_t, guest_base) == 72, "guest_base must be at offset 72");
_Static_assert(offsetof(a32_ctx_t, exit_reason) == 80, "exit_reason must be at offset 80");
_Static_assert(offsetof(a32_ctx_t, svc_imm) == 84, "svc_imm must be at offset 84");
_Static_assert(offsetof(a32_ctx_t, vfp_d) == 96, "vfp_d must be at offset 96 (0x60)");
_Static_assert(offsetof(a32_ctx_t, fpscr) == 352, "fpscr must be at offset 352 (0x160)");
_Static_assert(offsetof(a32_ctx_t, sig_mask) == 360, "sig_mask must be at offset 360");
_Static_assert(offsetof(a32_ctx_t, pending_signals) == 368, "pending_signals must be at offset 368");
_Static_assert(offsetof(a32_ctx_t, trace_enabled) == 376, "trace_enabled must be at offset 376");
_Static_assert(offsetof(a32_ctx_t, gdb_enabled) == 377, "gdb_enabled must be at offset 377");
_Static_assert(offsetof(a32_ctx_t, single_step) == 378, "single_step must be at offset 378");
_Static_assert(offsetof(a32_ctx_t, sync_sig_delivered) == 379, "sync_sig_delivered must be at offset 379");
_Static_assert(offsetof(a32_ctx_t, itstate) == 380, "itstate must be at offset 380");
_Static_assert(offsetof(a32_ctx_t, blocks_executed) == 384, "blocks_executed must be at offset 384");
_Static_assert(offsetof(a32_ctx_t, chain_hits) == 392, "chain_hits must be at offset 392");
_Static_assert(offsetof(a32_ctx_t, last_exec_entry) == 400, "last_exec_entry must be at offset 400");
_Static_assert(offsetof(a32_ctx_t, ibtc_hits) == 408, "ibtc_hits must be at offset 408");
_Static_assert(offsetof(a32_ctx_t, ras_hits) == 416, "ras_hits must be at offset 416 (0x1a0)");
_Static_assert(offsetof(a32_ctx_t, ras_top) == 424, "ras_top must be at offset 424 (0x1a8)");
_Static_assert(offsetof(a32_ctx_t, ras) == 432, "ras must be at offset 432 (0x1b0)");
_Static_assert(offsetof(a32_ctx_t, ibtc) == 2480, "ibtc must be at offset 2480 (0x9b0)");

static inline void *a32_guest_ptr(const a32_ctx_t *ctx, uint32_t guest_va)
{
    return (void *)(uintptr_t)(ctx->guest_base + (uint64_t)guest_va);
}

static inline uint32_t a32_page_down(uint32_t x)
{
    return x & ~(A32_PAGE_SIZE - 1U);
}

static inline uint32_t a32_page_up(uint32_t x)
{
    return (x + (A32_PAGE_SIZE - 1U)) & ~(A32_PAGE_SIZE - 1U);
}

static inline void a32_set_pc_interwork(a32_ctx_t *ctx, uint32_t target)
{
    if ((target & 1U) != 0U) {
        ctx->cpsr |= A32_CPSR_T;
        ctx->regs[15] = target & ~1U;
    } else {
        ctx->cpsr &= ~A32_CPSR_T;
        ctx->regs[15] = target & ~3U;
    }
}

static inline void a32_normalize_pc(a32_ctx_t *ctx)
{
    if ((ctx->regs[15] & 1U) != 0U) {
        ctx->cpsr |= A32_CPSR_T;
        ctx->regs[15] &= ~1U;
    } else {
        ctx->regs[15] &= (ctx->cpsr & A32_CPSR_T) ? ~1U : ~3U;
    }
}

static inline int a32_host_prot(const a32_ctx_t *ctx, uint32_t guest_prot)
{
    uint32_t p = guest_prot & 0x7U;
    if ((p & 0x4U) != 0U) {
        p |= 0x1U; /* PROT_EXEC implies PROT_READ on host */
        if (ctx != NULL && (ctx->host_info.caps & A32H_CAP_WX_ENFORCED) != 0ULL &&
            (p & 0x2U) != 0U) {
            p &= ~0x4U;
        }
    }
    return (int)p;
}

/* Host handshake (`src/host.c`) */
void a32_host_probe(a32_ctx_t *ctx);

/* Guest window & memory management (`src/vm.c`) */
int      a32_vm_init(a32_ctx_t *ctx, uint64_t requested_window);
int      a32_vm_record_region(a32_ctx_t *ctx, uint32_t start, uint32_t end,
                              uint32_t prot, uint32_t flags, const char *name);
uint32_t a32_vm_brk(a32_ctx_t *ctx, uint32_t new_brk);
uint32_t a32_vm_mmap2(a32_ctx_t *ctx, uint32_t addr, uint32_t len, uint32_t prot,
                      uint32_t flags, int fd, uint32_t pgoffset);
int      a32_vm_mprotect(a32_ctx_t *ctx, uint32_t addr, uint32_t len, uint32_t prot);
int      a32_vm_munmap(a32_ctx_t *ctx, uint32_t addr, uint32_t len);
uint32_t a32_vm_mremap(a32_ctx_t *ctx, uint32_t old_addr, uint32_t old_len,
                       uint32_t new_len, uint32_t flags, uint32_t new_addr);
void     a32_vm_sync_ctx(a32_ctx_t *ctx);

/* ELF32 loader & initial stack (`src/elf32.c`, `src/stack32.c`) */
typedef struct a32_elf_info {
    uint32_t entry;
    uint32_t phdr_vaddr;
    uint32_t phent;
    uint32_t phnum;
    uint32_t load_bias;
    uint32_t max_vaddr;
    uint32_t interp_base;
    uint32_t text_start;
    uint32_t text_end;
    char     interp_path[256];
} a32_elf_info_t;

int a32_open_guest_ro(const a32_ctx_t *ctx, const char *guest_path);
int a32_elf_load(a32_ctx_t *ctx, int exec_fd, const char *path, a32_elf_info_t *out_info);
int a32_kuser_setup(a32_ctx_t *ctx);
int a32_stack_setup(a32_ctx_t *ctx, const a32_elf_info_t *elf_info,
                    int guest_argc, char **guest_argv, char **host_envp,
                    uint32_t *out_sp);

/* JIT & execution engine (`src/jit.c`, `src/trampoline.S`) */
typedef void (*a32_block_iter_fn)(uint32_t guest_pc, uint32_t guest_end_pc,
                                  bool is_thumb, bool is_call,
                                  const uint32_t *words, size_t word_count,
                                  const a32_pc_map_stage_t *pc_map, size_t pc_map_count,
                                  void *arg);

int         a32_jit_init(a32_ctx_t *ctx);
bool        a32_jit_range_executable(const a32_ctx_t *ctx, uint32_t addr, uint32_t len);
int         a32_jit_invalidate_range(a32_ctx_t *ctx, uint32_t start, uint32_t end);
int         a32_jit_run(a32_ctx_t *ctx);
uint32_t    a32_jit_run_block(a32_ctx_t *ctx, const void *code_ptr,
                              void *cur_entry, const void *epoch_ptr);
void        a32_jit_record_pc_map(uint32_t host_word_idx, uint32_t guest_pc, uint32_t itstate);
bool        a32_jit_lookup_host_pc(uintptr_t host_pc, uint32_t *out_guest_pc, uint32_t *out_itstate);
const void *a32_jit_compile_block(a32_ctx_t *ctx, uint32_t pc, bool is_thumb, uint32_t *out_code_bytes);
int         a32_jit_install_aot_block(a32_ctx_t *ctx, uint32_t guest_pc, uint32_t guest_end_pc,
                                      bool is_thumb, bool is_call,
                                      const uint32_t *words, size_t word_count,
                                      const a32_pc_map_stage_t *pc_map, size_t pc_map_count);
void        a32_jit_iter_blocks(a32_block_iter_fn fn, void *arg);
int         a32_jit_flush_batch(a32_ctx_t *ctx);

/* AOT pre-translator & disk cache (`src/aot.c`) */
int a32_aot_generate(a32_ctx_t *ctx, const a32_elf_info_t *elf_info, const char *elf_path);
int a32_aot_load_cache(a32_ctx_t *ctx, const char *elf_path);
int a32_aot_save_cache(a32_ctx_t *ctx, const char *elf_path);
int a32_aot_load_fd(a32_ctx_t *ctx, int fd, uint32_t load_bias);

/* Syscalls & signals (`src/syscalls.c`, `src/signals.c`) */
bool       a32_guest_range_ok(a32_ctx_t *ctx, uint32_t va, uint32_t len, uint32_t req_prot);
int        a32_resolve_guest_path(const a32_ctx_t *ctx, const char *guest_path,
                                  char *buf, size_t size);
int        a32_handle_syscall(a32_ctx_t *ctx);
void       a32_cleanup_robust_list(a32_ctx_t *ctx);
int        a32_signals_init(a32_ctx_t *ctx);
void       a32_signals_set_thread_ctx(a32_ctx_t *ctx);
a32_ctx_t *a32_signals_get_thread_ctx(void);
void       a32_signals_check_pending(a32_ctx_t *ctx);
bool       a32_signals_deliver_sync(a32_ctx_t *ctx, int sig, uint32_t fault_addr);
void       a32_signals_ensure_host_handler(int sig);
void       a32_signals_preset_siginfo(int sig, const void *host_si);
void       a32_signals_consume_siginfo(int sig, void *guest_si32);
int        a32_sys_rt_sigaction(a32_ctx_t *ctx, int signum, uint32_t act_va,
                                uint32_t oldact_va, uint32_t sigsetsize);
int        a32_sys_rt_sigprocmask(a32_ctx_t *ctx, int how, uint32_t set_va,
                                  uint32_t oldset_va, uint32_t sigsetsize);
int        a32_sys_rt_sigreturn(a32_ctx_t *ctx);
int        a32_sys_sigaltstack(a32_ctx_t *ctx, uint32_t uss_va, uint32_t uoss_va);

/* GDB RSP stub (`src/gdb.c`) */
int  a32_gdb_init(a32_ctx_t *ctx, int port);
bool a32_gdb_has_breakpoint(uint32_t guest_pc);
int  a32_gdb_handle_stop(a32_ctx_t *ctx, int sig);

#endif /* A32RT_H */
