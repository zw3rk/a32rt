/*
 * src/os/darwin_host_bridge.c — Narrow A32H_ABI 1.2 (G02) external Darwin bridge
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * A32H_ARCH_ARM32 client of A32H_DARWIN_BOOT / A32H_DARWIN_TRAP. The host
 * owns Mach-O/dyld, BSD/Mach traps, threads, and signals; this file only
 * moves register, VMA, and exit state across the contract. Provider rule:
 * BOOT populates guest bytes in [window_base+va, +len) (mapping pages
 * itself when needed); the bridge registers ranges, syncs the vm32
 * Soft-TLB, and enforces final protections (contents kept). Exclusives run
 * on spawned worker threads outside difftest lockstep (shared reservation
 * table is keyed by ctx, so a dut shadow would steal ref reservations).
 *
 * Threading model: the VMA region table (src/vm.c globals, a32_vm_lock)
 * and the JIT code cache (jit_lock) are shared across all bridge workers;
 * only regs/cpsr/FP/TLS/pending_signals are per-thread. Region and
 * code-cache mutations always go through those locks.
 */

#define _GNU_SOURCE
#include "os/darwin_host_bridge.h"

#include "dbt/difftest.h"
#include "interp/interp.h"
#include "os/backend.h"
#include "vm/vm32.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#include <unistd.h>

#define DARWIN_VMAP_STACK_CAP 64U
/* Unrecovered identical *fault* (pc, fault_va, regs, cpsr, kind, prot)
 * trap detector: consecutive identical fault traps with zero guest
 * progress indicate the host is not resolving the fault. SVC traps from
 * one call site are legitimate (read() loops) and never counted. */
#define DARWIN_FAULT_SAME_MAX 256U

static a32h_darwin_handler_fn g_fn = NULL;
static void *g_opaque = NULL;
static bool g_use_dbt = false;

/* Cached /dev/a32host fd shared by every BOOT/TRAP round-trip (one open
 * per process instead of open+ioctl+close per svc). ioctl on a shared fd
 * is thread-safe; EINTR retries, and a stale fd resets the cache. */
static pthread_mutex_t g_host_fd_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_host_fd = -1;

static int host_ioctl(unsigned long req, void *arg)
{
    int fd, rc, e;
    pthread_mutex_lock(&g_host_fd_mu);
    if (g_host_fd < 0) {
        g_host_fd = open(A32H_DEV_PATH, O_RDWR | O_CLOEXEC);
        if (g_host_fd < 0) {
            pthread_mutex_unlock(&g_host_fd_mu);
            return -ENODEV;
        }
    }
    fd = g_host_fd;
    pthread_mutex_unlock(&g_host_fd_mu);
    for (;;) {
#if defined(__linux__)
        rc = (int)syscall(SYS_ioctl, fd, req, arg);
#else
        rc = ioctl(fd, req, arg);
#endif
        if (rc == 0) {
            return 0;
        }
        e = errno;
        if (e == EINTR) {
            continue;
        }
        if (e == EBADF) {
            pthread_mutex_lock(&g_host_fd_mu);
            if (g_host_fd == fd) {
                close(g_host_fd);
                g_host_fd = -1;
            }
            pthread_mutex_unlock(&g_host_fd_mu);
        }
        return (e != 0) ? -e : -EIO;
    }
}

static int call_trap(a32h_darwin_handler_fn fn, void *opaque,
    unsigned long req, void *arg)
{
    return (fn != NULL) ? fn(opaque, req, arg) : host_ioctl(req, arg);
}

static bool is_bridge_personality(const char *p)
{
    return p != NULL && (strcmp(p, "darwin-host") == 0 ||
                         strcmp(p, "hl-darwin") == 0);
}

bool a32_darwin_cli_wants_bridge(int argc, char **argv, bool *out_darwin_boot)
{
    bool boot = false, want = false;
    for (int i = 1; i < argc; i++) {
        const char *s = argv[i];
        if (s == NULL || strcmp(s, "--") == 0 || s[0] != '-') {
            break; /* `--` or first positional <PATH>: guest argv follows */
        } else if (strcmp(s, "--darwin-boot") == 0) {
            boot = true;
            want = true;
        } else if (strcmp(s, "--personality") == 0 && i + 1 < argc &&
                   is_bridge_personality(argv[i + 1])) {
            want = true;
        } else if (strncmp(s, "--personality=", 14) == 0 &&
                   is_bridge_personality(s + 14)) {
            want = true;
        }
    }
    if (is_bridge_personality(getenv("A32RT_PERSONALITY"))) {
        want = true;
    }
    if (out_darwin_boot != NULL) {
        *out_darwin_boot = boot;
    }
    return want;
}

bool a32_darwin_is_macho_fd(int fd)
{
    uint8_t magic[4];
    uint32_t le;
    if (fd < 0 || pread(fd, magic, sizeof(magic), 0) != 4) {
        return false;
    }
    le = (uint32_t)magic[0] | ((uint32_t)magic[1] << 8) |
         ((uint32_t)magic[2] << 16) | ((uint32_t)magic[3] << 24);
    return le == 0xFEEDFACEU || le == 0xCEFAEDFEU || le == 0xCAFEBABEU ||
           le == 0xBEBAFECAU;
}

bool a32_darwin_auto_bridge(const a32_ctx_t *ctx, bool target_is_macho,
    bool darwin_boot)
{
    return ctx != NULL && ctx->has_host_info &&
           (ctx->host_info.caps & A32H_CAP_DARWIN_PERSONALITY) != 0ULL &&
           (target_is_macho || darwin_boot);
}

/* FNV-1a mix of the host personality tag into the AOT cache key. */
uint64_t a32_darwin_mix_hash(uint64_t h, uint64_t person)
{
    for (unsigned k = 0U; k < 8U; k++) {
        h ^= (person >> (k * 8U)) & 0xFFULL;
        h *= 0x100000001b3ULL;
    }
    return (h != 0ULL) ? h : 1ULL;
}

/* CPU + fault marshaling lives in src/os/darwin_host_cpu.c
 * (a32_darwin_fill_cpu / a32_darwin_apply_cpu / a32_darwin_esr_wnr /
 * a32_darwin_fault_kind_for / a32_darwin_bkpt_imm_for). */

/* (classifiers moved to src/os/darwin_host_cpu.c) */

/* Exclusive end for a page-aligned [va, end64) range clamped to 4 GiB.
 * A range reaching exactly 4 GiB is recorded as end 0xFFFFFFFF (the
 * top-of-4GiB sentinel): guest_range_mapped treats a region ending at
 * 0xFFFFFFFF as covering through the last byte, and per-page loops use
 * `page < end` so the top page is still visited. */
static uint32_t page_end32(uint64_t end64)
{
    if (end64 >= 0x100000000ULL) {
        return 0xFFFFFFFFU;
    }
    return (((uint32_t)end64 + 0xFFFU) & ~0xFFFU);
}

static int register_range(a32_ctx_t *ctx, uint32_t s, uint32_t e, uint32_t gp,
    const char *name);

static int do_boot(a32_ctx_t *ctx, a32h_darwin_handler_fn fn, void *opaque)
{
    a32h_darwin_vmap_t stack_vmaps[DARWIN_VMAP_STACK_CAP];
    a32h_darwin_vmap_t *vmaps = stack_vmaps;
    a32h_darwin_boot_t boot;
    bool t, heap_vmaps = false;
    int rc = -1;
    memset(stack_vmaps, 0, sizeof(stack_vmaps));
    memset(&boot, 0, sizeof(boot));
    boot.arch = A32H_ARCH_ARM32;
    boot.abi_minor = A32H_ABI_MINOR_DARWIN;
    boot.window_base = ctx->guest_base;
    boot.vmap_ptr = (uint64_t)(uintptr_t)vmaps;
    boot.vmap_cap = DARWIN_VMAP_STACK_CAP;
    if (call_trap(fn, opaque, A32H_DARWIN_BOOT, &boot) != 0 ||
        boot.arch != A32H_ARCH_ARM32 || boot.entry_sp == 0U) {
        fprintf(stderr, "a32rt: A32H_DARWIN_BOOT failed\n");
        return -1;
    }
    if (boot.vmap_count > DARWIN_VMAP_STACK_CAP) {
        /* Dyld images exceed the stack cap: allocate exactly vmap_count
         * entries and re-issue BOOT so no VMA is dropped. */
        uint32_t want = boot.vmap_count;
        vmaps = (a32h_darwin_vmap_t *)calloc((size_t)want, sizeof(*vmaps));
        if (vmaps == NULL) {
            fprintf(stderr, "a32rt: A32H_DARWIN_BOOT vmap alloc failed\n");
            return -1;
        }
        heap_vmaps = true;
        memset(&boot, 0, sizeof(boot));
        boot.arch = A32H_ARCH_ARM32;
        boot.abi_minor = A32H_ABI_MINOR_DARWIN;
        boot.window_base = ctx->guest_base;
        boot.vmap_ptr = (uint64_t)(uintptr_t)vmaps;
        boot.vmap_cap = want;
        if (call_trap(fn, opaque, A32H_DARWIN_BOOT, &boot) != 0 ||
            boot.arch != A32H_ARCH_ARM32 || boot.entry_sp == 0U ||
            boot.vmap_count > want) {
            fprintf(stderr, "a32rt: A32H_DARWIN_BOOT retry failed\n");
            free(vmaps);
            return -1;
        }
    }
    for (uint32_t i = 0U; i < boot.vmap_count; i++) {
        uint64_t end64 = (uint64_t)vmaps[i].va + (uint64_t)vmaps[i].len;
        uint32_t start = vmaps[i].va & ~0xFFFU;
        uint32_t end, gp;
        if (vmaps[i].len == 0U) {
            continue;
        }
        if (end64 > 0x100000000ULL) {
            end64 = 0x100000000ULL;
        }
        end = page_end32(end64);
        if (end <= start) {
            continue;
        }
        gp = vmaps[i].prot & 0x7U;
        if (register_range(ctx, start, end, gp, "darwin-boot") != 0) {
            goto out;
        }
    }
    if (boot.commpage_va != 0U && boot.commpage_size != 0U) {
        uint64_t cend = (uint64_t)boot.commpage_va +
            (uint64_t)boot.commpage_size;
        uint32_t cs = boot.commpage_va & ~0xFFFU;
        uint32_t ce;
        uint64_t clen;
        if (cend > 0x100000000ULL) {
            cend = 0x100000000ULL;
        }
        ce = page_end32(cend);
        clen = cend - (uint64_t)boot.commpage_va;
        if (ce > cs && clen <= 0xFFFFFFFFU &&
            !a32_guest_range_ok(ctx, boot.commpage_va, (uint32_t)clen, 0U)) {
            /* CommPage data (timebase words, no code): readable; RX so a
             * stray fetch faults cleanly instead of executing zeros. */
            if (register_range(ctx, cs, ce, (uint32_t)PROT_READ |
                    (uint32_t)PROT_EXEC, "darwin-commpage") != 0) {
                goto out;
            }
        }
    }
    ctx->darwin_personality_hash = boot.aot_personality_hash;
    ctx->darwin_tid = boot.initial_tid;
    ctx->is_ios = true; /* 0xDEFE -> BKPT, CP15 TPIDRURO/CNTVCT live */
    ctx->darwin_external = true;
    ctx->regs[15] = boot.entry_pc & ~1U;
    ctx->elf_entry = boot.entry_pc & ~1U; /* stable AOT image anchor */
    t = ((boot.entry_pc & 1U) != 0U) ||
        ((boot.entry_cpsr_or_msr & A32_CPSR_T) != 0U);
    /* Boot CPSR keeps NZCV + T only (user mode, no stale mode/E bits). */
    ctx->cpsr = (boot.entry_cpsr_or_msr & A32_CPSR_NZCV_MASK) |
                (t ? A32_CPSR_T : 0U);
    ctx->regs[13] = boot.entry_sp;
    ctx->regs[0] = boot.entry_r0;
    rc = 0;
out:
    if (heap_vmaps) {
        free(vmaps);
    }
    return rc;
}

static void build_trap(const a32_ctx_t *ctx, uint32_t tid, uint32_t dexit,
    bool full, a32h_darwin_cpu_state_t *cpu, a32h_darwin_trap_t *trap)
{
    memset(cpu, 0, sizeof(*cpu));
    memset(trap, 0, sizeof(*trap));
    trap->arch = A32H_ARCH_ARM32;
    trap->tid = tid;
    trap->exit_reason = dexit;
    trap->cpu_state_ptr = (uint64_t)(uintptr_t)cpu;
    a32_darwin_fill_cpu(ctx, cpu, full);
    if (dexit == A32H_DEXIT_SVC_TRAP) {
        trap->trap_imm = ctx->svc_imm;
        a32_darwin_fill_svc_pc(ctx, cpu);
    } else if (dexit == A32H_DEXIT_FAULT) {
        trap->fault_va = ctx->fault_addr;
        trap->fault_kind = a32_darwin_fault_kind_for(ctx);
        trap->fault_is_prot =
            a32_guest_range_ok((a32_ctx_t *)ctx, ctx->fault_addr, 1U, 0U) ? 1U
                                                                         : 0U;
    } else if (dexit == A32H_DEXIT_BKPT) {
        trap->trap_imm = a32_darwin_bkpt_imm_for(ctx);
    }
}

/* 0 ok, -1 fatal; clears reply/outputs and resends with full FP on
 * NEED_FULL (the host must treat a NEED_FULL retry idempotently). */
static int send_trap(a32_ctx_t *ctx, a32h_darwin_handler_fn fn, void *opaque,
    a32h_darwin_cpu_state_t *cpu, a32h_darwin_trap_t *trap, uint32_t tid,
    uint32_t dexit)
{
    if (call_trap(fn, opaque, A32H_DARWIN_TRAP, trap) != 0) {
        return -1;
    }
    if (((cpu->state_flags & A32H_DSTATE_NEED_FULL) != 0U) &&
        ((cpu->state_flags & A32H_DSTATE_FP_VALID) == 0U)) {
        build_trap(ctx, tid, dexit, true, cpu, trap);
        if (call_trap(fn, opaque, A32H_DARWIN_TRAP, trap) != 0) {
            return -1;
        }
    }
    return 0;
}

/* Stage [s, e) for registration. Pages the provider already populated
 * (real /dev/a32host, or the mock handler's own maps) are left untouched
 * (contents kept): MAP_FIXED_NOREPLACE fails with EEXIST when occupied.
 * Only genuinely absent pages get fresh anonymous backing. Never forces
 * PROT_READ|PROT_WRITE: host-populated MAP_SHARED read-only provider
 * pages would fail mprotect(RW) and MAP_FIXED anon would zero loaded
 * Mach-O bytes. Final protections are applied by register_range. */
static int stage_map(a32_ctx_t *ctx, uint32_t s, uint32_t e)
{
    void *hva = (void *)(uintptr_t)(ctx->guest_base + (uint64_t)s);
    void *out = NULL;
    size_t mlen = (size_t)(e - s);
    int rc = a32_backend_map_anon(hva, mlen, PROT_READ | PROT_WRITE,
        1, 0, 0, &out);
    if (rc == 0 && out == hva) {
        return 0; /* staged fresh anonymous pages */
    }
    if (rc == -EEXIST || (rc == 0 && out != hva)) {
        return 0; /* host-populated: keep provider bytes as-is */
    }
    return -1;
}

/* Stage, record, and protect [s, e) with guest prot gp (contents kept). */
static int register_range(a32_ctx_t *ctx, uint32_t s, uint32_t e, uint32_t gp,
    const char *name)
{
    if (stage_map(ctx, s, e) != 0 ||
        a32_vm_record_region(ctx, s, e, gp, 0U, name) != 0 ||
        a32_vm32_sync_region(s, e, (gp & 0x1U) != 0U, (gp & 0x2U) != 0U,
            (gp & 0x4U) != 0U) != 0 ||
        a32_vm32_protect(ctx, s, (size_t)(e - s), gp) != 0) {
        return -1;
    }
    return 0;
}

static int run_loop(a32_ctx_t *ctx, a32h_darwin_handler_fn fn, void *opaque,
    bool use_dbt, bool difftest, uint32_t tid, bool is_worker);

/* Whole-process exit from any thread: flush reporting, then terminate.
 * EXIT_PROCESS is never swallowed as a thread exit. */
static void worker_exit_process(a32_ctx_t *ctx, int status)
{
    if (ctx->stats_enabled) {
        a32_print_stats(ctx);
    }
    if (ctx->auto_aot && !ctx->aot_loaded && ctx->blocks_compiled > 0U &&
        ctx->exec_path != NULL) {
        (void)a32_aot_save_cache(ctx, ctx->exec_path);
    }
    _exit(status);
}

static void send_thread_exit(a32_ctx_t *ctx, a32h_darwin_handler_fn fn,
    void *opaque, uint32_t tid)
{
    a32h_darwin_cpu_state_t cpu;
    a32h_darwin_trap_t trap;
    build_trap(ctx, tid, A32H_DEXIT_THREAD_EXIT, false, &cpu, &trap);
    if (call_trap(fn, opaque, A32H_DARWIN_TRAP, &trap) == 0) {
        a32_darwin_apply_cpu(ctx, &cpu);
    }
}

/* Detached: the provider rendezvous on DEXIT_THREAD_EXIT before EXIT_PROCESS,
 * so no join is needed (process exit reaps any straggler). */
static void *worker_main(void *arg)
{
    a32_ctx_t *c = (a32_ctx_t *)arg;
    a32_signals_set_thread_ctx(c);
    (void)run_loop(c, g_fn, g_opaque, g_use_dbt, false, c->darwin_tid, true);
    a32_signals_set_thread_ctx(NULL);
    free(c);
    return NULL;
}

/* Fold a host reply into guest state.
 * 0 resume, 1 exit-process (status in trap->exit_status),
 * 2 exit-thread, -1 fatal. */
static int apply_reply(a32_ctx_t *ctx, uint32_t tid,
    a32h_darwin_cpu_state_t *cpu, a32h_darwin_trap_t *trap)
{
    uint32_t act = trap->reply_action;
    uint64_t end64;
    uint32_t s, e;
    size_t plen;
    (void)tid;
    a32_darwin_apply_cpu(ctx, cpu);
    if (((act & A32H_DACT_FLUSH_ICACHE) != 0U) && trap->flush_len != 0U) {
        end64 = (uint64_t)trap->flush_va + (uint64_t)trap->flush_len;
        if (end64 > 0x100000000ULL) {
            end64 = 0x100000000ULL;
        }
        if (end64 > (uint64_t)trap->flush_va) {
            e = page_end32(end64);
            plen = (size_t)(end64 - (uint64_t)trap->flush_va);
            a32_vm32_copyout_invalidate_exec(ctx, trap->flush_va, plen);
            (void)a32_jit_invalidate_range(ctx, trap->flush_va & ~3U, e);
        }
    }
    if (((act & A32H_DACT_SYNC_VM_PROT) != 0U) && trap->prot_len != 0U) {
        uint32_t gp = trap->prot_flags & 0x7U;
        end64 = (uint64_t)trap->prot_va + (uint64_t)trap->prot_len;
        if (end64 > 0x100000000ULL) {
            end64 = 0x100000000ULL;
        }
        s = trap->prot_va & ~0xFFFU;
        e = page_end32(end64);
        plen = (size_t)(end64 - (uint64_t)s);
        if (e > s && plen > 0U) {
            /* plen <= 4 GiB by clamping; a full-window sync truncates
             * to 0xFFFFFFFF (munmap then takes the EINVAL fallback). */
            uint32_t llen = (plen >= 0x100000000ULL) ? 0xFFFFFFFFU
                                                    : (uint32_t)plen;
            if (gp == 0U) {
                /* Unmap: retire the VMA so later accesses report
                 * unmapped (fault_is_prot = 0 -> SIGSEGV) and drop
                 * JIT/Soft-TLB state across the range. */
                if (llen == 0U || a32_vm_munmap(ctx, s, llen) != 0) {
                    /* High-VA ranges reject munmap: at least drop the
                     * translations so stale code cannot run. */
                    (void)a32_vm32_sync_unmap(s, e);
                } else {
                    (void)a32_vm32_sync_unmap(s, e);
                }
                a32_vm32_tlb_invalidate_range(s, e);
                (void)a32_jit_invalidate_range(ctx, s, e);
            } else if (!a32_guest_range_ok(ctx, s, llen, 0U)) {
                if (register_range(ctx, s, e, gp, "darwin-prot") != 0) {
                    return -1;
                }
            } else if (a32_vm_mprotect(ctx, s, llen, gp) != 0) {
                return -1;
            }
            if (gp != 0U) {
                /* Publish updated A2 Soft-TLB tags and flush JIT across range. */
                (void)a32_vm32_sync_region(s, e, (gp & 0x1U) != 0U,
                    (gp & 0x2U) != 0U, (gp & 0x4U) != 0U);
                (void)a32_jit_invalidate_range(ctx, s, e);
            }
        }
    }
    if ((act & A32H_DACT_EXIT_THREAD) != 0U) {
        return 2;
    }
    if ((act & A32H_DACT_SPAWN_THREAD) != 0U) {
        a32_ctx_t *child = (a32_ctx_t *)malloc(sizeof(a32_ctx_t));
        pthread_t th;
        bool t;
        if (child == NULL) {
            return -1;
        }
        memcpy(child, ctx, sizeof(*child));
        /* The child shares the VMA table and JIT cache (both guarded by
         * their own locks); only thread-local state is reset here. */
        child->pending_signals = 0U;
        child->itstate = 0U;
        child->sync_sig_delivered = false;
        child->exit_reason = A32_EXIT_NONE;
        child->fault_host_valid = 0U;
        child->fpscr = 0U; /* Darwin default FPSCR for new threads */
        memset(child->ibtc, 0, sizeof(child->ibtc));
        memset(child->ras, 0, sizeof(child->ras));
        child->ras_top = 0U;
        t = (trap->spawn_pc & 1U) != 0U;
        child->regs[15] = trap->spawn_pc & ~1U;
        child->cpsr = (child->cpsr & ~A32_CPSR_T) | (t ? A32_CPSR_T : 0U);
        child->regs[13] = trap->spawn_sp;
        child->regs[0] = trap->spawn_r0;
        child->regs[14] = 0U; /* return lands on VA 0 -> thread exit */
        child->tls_tp = trap->spawn_tls;
        child->darwin_tid = trap->spawn_tid;
#if defined(__linux__)
        child->host_tid = (int)syscall(SYS_gettid);
#else
        child->host_tid = (int)getpid();
#endif
        child->is_thread_clone = true;
        child->last_exec_entry = NULL;
        if (pthread_create(&th, NULL, worker_main, child) != 0) {
            free(child);
            return -1;
        }
        pthread_detach(th);
    }
    return ((act & A32H_DACT_EXIT_PROCESS) != 0U) ? 1 : 0;
}

/* The host exclusive monitor does not survive the block boundary (any
 * intervening exception clears it, and single-step seals one insn per
 * block), so LDREX/STREX run through the reservation-table interpreter
 * path instead of native LDXR/STXR. Pure bridge policy; the translator's
 * native lowering is untouched. */
static bool exclusive_at(const a32_ctx_t *ctx, uint32_t pc)
{
    if ((ctx->cpsr & A32_CPSR_T) != 0U) {
        uint16_t hw = 0U;
        if (!a32_guest_range_ok((a32_ctx_t *)ctx, pc & ~1U, 2U,
                (uint32_t)PROT_READ)) {
            return false;
        }
        memcpy(&hw, a32_guest_ptr(ctx, pc & ~1U), 2U);
        return ((hw & 0xFFF0U) == 0xE840U) || ((hw & 0xFFF0U) == 0xE850U);
    }
    {
        uint32_t w = 0U;
        if (!a32_guest_range_ok((a32_ctx_t *)ctx, pc & ~3U, 4U,
                (uint32_t)PROT_READ)) {
            return false;
        }
        memcpy(&w, a32_guest_ptr(ctx, pc & ~3U), 4U);
        return (w & 0x0F8000F0U) == 0x01800090U;
    }
}

/* One step; DBT UNDEF falls back to interp exactly like a32_jit_run. */
static uint32_t step_once(a32_ctx_t *ctx, bool use_dbt)
{
    uint32_t rc;
    if (!use_dbt || exclusive_at(ctx, ctx->regs[15])) {
        return a32_interp_step(ctx);
    }
    rc = a32_dbt_single_step(ctx);
    if (rc == A32_EXIT_UNDEF) {
        uint32_t irc = a32_interp_step(ctx);
        if (irc != A32_EXIT_UNDEF) {
            return irc;
        }
    }
    return rc;
}

static bool rc_continue(uint32_t rc)
{
    return rc == A32_EXIT_NONE || rc == A32_EXIT_BRANCH ||
           rc == A32_EXIT_INTERWORK;
}

/* Best-effort thread-exit rendezvous before a worker reports fatal: the
 * host may be blocked waiting for DEXIT_THREAD_EXIT. */
static int worker_bail(a32_ctx_t *ctx, a32h_darwin_handler_fn fn,
    void *opaque, uint32_t tid)
{
    send_thread_exit(ctx, fn, opaque, tid);
    return 1;
}

/* Maps apply_reply codes to run_loop control flow:
 * 0 continue, 1 stop with *status, 2 thread exit (status 0 for workers). */
static int apply_action(a32_ctx_t *ctx, a32h_darwin_handler_fn fn,
    void *opaque, int ar, const a32h_darwin_trap_t *trap, uint32_t tid,
    bool is_worker, int *out_status)
{
    if (ar < 0) {
        return -1;
    }
    if (ar == 2) {
        send_thread_exit(ctx, fn, opaque, tid);
        *out_status = trap->exit_status;
        return 2;
    }
    if (ar > 0) {
        *out_status = trap->exit_status;
        if (is_worker) {
            worker_exit_process(ctx, trap->exit_status); /* no return */
        }
        return 1;
    }
    return 0;
}

static int run_loop(a32_ctx_t *ctx, a32h_darwin_handler_fn fn, void *opaque,
    bool use_dbt, bool difftest, uint32_t tid, bool is_worker)
{
    static a32_ctx_t dut; /* ~80 KiB: keep off pthread stacks */
    bool have_dut = difftest && !is_worker;
    /* Unrecovered-fault progress detector: an identical fault (pc,
     * fault_va, regs, cpsr, kind, prot class) with zero guest progress. */
    bool have_last_fault = false;
    uint32_t last_fault_pc = 0U, last_fault_va = 0U;
    uint32_t last_fault_regs[16];
    uint32_t last_fault_cpsr = 0U, last_fault_kind = 0U;
    uint32_t last_fault_prot = 0U, fault_same = 0U;
    uint64_t compared = 0ULL;
    int exit_status = 0;
    memset(last_fault_regs, 0, sizeof(last_fault_regs));
    if (have_dut) {
        memcpy(&dut, ctx, sizeof(dut));
    }
    for (;;) {
        uint32_t pc = ctx->regs[15];
        uint32_t rc, dexit = A32H_DEXIT_NONE;
        a32h_darwin_cpu_state_t cpu;
        a32h_darwin_trap_t trap;
        int ar, aa;
        if (pc == 0U && is_worker) { /* lr=0 return: thread exit */
            send_thread_exit(ctx, fn, opaque, tid);
            return 0;
        }
        /* Queued async signals are drained by the host: at every block
         * boundary with unmasked pending signals, issue a poll trap so
         * the host can answer with DELIVER_SIGNAL. A spinning guest
         * without traps would otherwise never observe SIGINT/SIGALRM. */
        if ((__atomic_load_n(&ctx->pending_signals, __ATOMIC_SEQ_CST) &
                ~ctx->sig_mask) != 0ULL) {
            build_trap(ctx, tid, A32H_DEXIT_NONE, false, &cpu, &trap);
            if (send_trap(ctx, fn, opaque, &cpu, &trap, tid,
                    A32H_DEXIT_NONE) != 0) {
                return is_worker ? worker_bail(ctx, fn, opaque, tid) : 1;
            }
            ar = apply_reply(ctx, tid, &cpu, &trap);
            aa = apply_action(ctx, fn, opaque, ar, &trap, tid, is_worker,
                &exit_status);
            if (aa < 0) {
                return is_worker ? worker_bail(ctx, fn, opaque, tid) : 1;
            }
            if (aa == 2) {
                return is_worker ? 0 : exit_status;
            }
            if (aa > 0) {
                break;
            }
            have_last_fault = false;
            pc = ctx->regs[15];
        }
        if (have_dut) {
            /* Step the shadow under its own thread ctx so a dut fault
             * recovers through dut's side table, then restore. */
            memcpy(&dut, ctx, sizeof(dut));
            memset(dut.ibtc, 0, sizeof(dut.ibtc));
            memset(dut.ras, 0, sizeof(dut.ras));
            dut.ras_top = 0U;
        }
        ctx->fault_host_valid = 0U; /* stale ESR WnR never reused */
        rc = step_once(ctx, use_dbt);
        ctx->blocks_executed++;
        if (have_dut) {
            uint32_t d_rc;
            a32_signals_set_thread_ctx(&dut);
            /* Lockstep reference: the shadow always steps interp while
             * ctx follows use_dbt, so difftest is genuine interp-vs-dbt. */
            d_rc = step_once(&dut, false);
            a32_signals_set_thread_ctx(ctx);
            if (rc_continue(rc) != rc_continue(d_rc) ||
                (!rc_continue(rc) && rc != d_rc) ||
                !a32_interp_state_equal(ctx, &dut) ||
                ctx->itstate != dut.itstate || ctx->tls_tp != dut.tls_tp ||
                ctx->fault_addr != dut.fault_addr ||
                ctx->fault_is_write != dut.fault_is_write ||
                ctx->svc_imm != dut.svc_imm) {
                fprintf(stderr,
                    "[darwin-difftest] DIVERGENCE at 0x%08x rc=%u d=%u\n", pc,
                    rc, d_rc);
                return is_worker ? worker_bail(ctx, fn, opaque, tid) : 1;
            }
        }
        if (rc_continue(rc)) {
            compared++;
            have_last_fault = false;
            continue;
        }
        if (rc == A32_EXIT_SVC) {
            dexit = A32H_DEXIT_SVC_TRAP;
            ctx->syscalls_executed++;
        } else if (rc == A32_EXIT_FAULT) {
            dexit = A32H_DEXIT_FAULT;
        } else if (rc == A32_EXIT_BKPT) {
            dexit = A32H_DEXIT_BKPT;
        } else if (rc == A32_EXIT_UNDEF) {
            dexit = A32H_DEXIT_ILLEGAL;
        } else {
            fprintf(stderr, "a32rt: darwin bridge: rc=%u\n", rc);
            return is_worker ? worker_bail(ctx, fn, opaque, tid) : 1;
        }
        build_trap(ctx, tid, dexit, false, &cpu, &trap);
        if (dexit == A32H_DEXIT_FAULT) {
            /* Unrecovered-fault detector: only an identical fault with
             * zero guest progress (same pc, fault VA, regs, CPSR, kind
             * and prot class) counts. Legitimate same-pc SVC loops and
             * resolving faults reset the counter. */
            if (have_last_fault && trap.fault_va == last_fault_va &&
                pc == last_fault_pc &&
                trap.fault_kind == last_fault_kind &&
                trap.fault_is_prot == last_fault_prot &&
                ctx->cpsr == last_fault_cpsr &&
                memcmp(ctx->regs, last_fault_regs,
                    sizeof(last_fault_regs)) == 0) {
                if (++fault_same > DARWIN_FAULT_SAME_MAX) {
                    fprintf(stderr,
                        "a32rt: darwin bridge: unrecovered fault 0x%08x\n",
                        pc);
                    return is_worker ? worker_bail(ctx, fn, opaque, tid)
                                     : 1;
                }
            } else {
                have_last_fault = true;
                last_fault_pc = pc;
                last_fault_va = trap.fault_va;
                memcpy(last_fault_regs, ctx->regs, sizeof(last_fault_regs));
                last_fault_cpsr = ctx->cpsr;
                last_fault_kind = trap.fault_kind;
                last_fault_prot = trap.fault_is_prot;
                fault_same = 0U;
            }
        } else {
            have_last_fault = false;
        }
        if (send_trap(ctx, fn, opaque, &cpu, &trap, tid, dexit) != 0) {
            return is_worker ? worker_bail(ctx, fn, opaque, tid) : 1;
        }
        ar = apply_reply(ctx, tid, &cpu, &trap);
        if (ar < 0) {
            return is_worker ? worker_bail(ctx, fn, opaque, tid) : 1;
        }
        aa = apply_action(ctx, fn, opaque, ar, &trap, tid, is_worker,
            &exit_status);
        if (aa < 0) {
            return is_worker ? worker_bail(ctx, fn, opaque, tid) : 1;
        }
        if (aa == 2) {
            return is_worker ? 0 : exit_status;
        }
        if (aa > 0) {
            exit_status = trap.exit_status;
            break;
        }
        if (have_dut) {
            memcpy(&dut, ctx, sizeof(dut));
        }
        compared++;
    }
    if (is_worker) {
        /* Unreachable: worker EXIT_PROCESS terminates above. Kept so a
         * future fall-through still rendezvous instead of hanging. */
        send_thread_exit(ctx, fn, opaque, tid);
        return 0;
    }
    if (have_dut && compared == 0ULL) {
        fprintf(stderr, "[darwin-difftest] zero compared instructions\n");
        return 1;
    }
    if (ctx->stats_enabled) {
        a32_print_stats(ctx);
    }
    if (ctx->auto_aot && !ctx->aot_loaded && ctx->blocks_compiled > 0U &&
        ctx->exec_path != NULL) {
        (void)a32_aot_save_cache(ctx, ctx->exec_path);
    }
    return exit_status;
}

/* Slim-build SVC hook: route one supervisor exit through the host trap. */
int a32_darwin_trap_once(a32_ctx_t *ctx)
{
    a32h_darwin_cpu_state_t cpu;
    a32h_darwin_trap_t trap;
    int ar;
    if (ctx == NULL || ctx->exit_reason != A32_EXIT_SVC) {
        return -1;
    }
    build_trap(ctx, ctx->darwin_tid, A32H_DEXIT_SVC_TRAP, false, &cpu, &trap);
    if (send_trap(ctx, g_fn, g_opaque, &cpu, &trap, ctx->darwin_tid,
            A32H_DEXIT_SVC_TRAP) != 0) {
        return -1;
    }
    ar = apply_reply(ctx, ctx->darwin_tid, &cpu, &trap);
    if (ar < 0) {
        return -1;
    }
    if (ar == 2) {
        /* Thread exit: workers park on VA 0 (run_loop rendezvous);
         * main/last thread ends the process with the host status. */
        if (ctx->is_thread_clone) {
            send_thread_exit(ctx, g_fn, g_opaque, ctx->darwin_tid);
            ctx->regs[15] = 0U;
            ctx->regs[0] = 0U;
            return 0;
        }
        send_thread_exit(ctx, g_fn, g_opaque, ctx->darwin_tid);
        ctx->regs[0] = (uint32_t)trap.exit_status;
        return 1;
    }
    if (ar != 0) {
        ctx->regs[0] = (uint32_t)trap.exit_status;
        return 1;
    }
    return 0;
}

void a32_darwin_enqueue_signal(a32_ctx_t *ctx, int sig)
{
    if (ctx == NULL || sig < 1 || sig > 64) {
        return;
    }
    /* The run_loop signal poll observes every block boundary, so no
     * separate JIT exit-request flag is needed for single-step mode. */
    __atomic_fetch_or(&ctx->pending_signals, 1ULL << (sig - 1),
        __ATOMIC_ACQ_REL);
}

/* Sync-fault exit for external guests. The handler recorded fault_addr,
 * fault_is_write/valid and fault_is_prot before calling; map the host
 * signal to the bridge trap and never raise() or _exit() (a worker must
 * not terminate the process outside EXIT_PROCESS). */
uint32_t a32_darwin_sync_exit(a32_ctx_t *ctx, int sig)
{
    (void)ctx;
    if (sig == SIGTRAP) {
        return A32_EXIT_BKPT;
    }
    if (sig == SIGILL || sig == SIGFPE) {
        return A32_EXIT_UNDEF;
    }
    return A32_EXIT_FAULT;
}

int a32_run_darwin_ctx_with_handler(a32_ctx_t *ctx,
    a32h_darwin_handler_fn fn, void *opaque, bool use_dbt, bool difftest)
{
    if (ctx == NULL) {
        return 1;
    }
#if defined(__linux__)
    ctx->host_tid = (int)syscall(SYS_gettid);
#else
    ctx->host_tid = (int)getpid();
#endif
    g_fn = fn;
    g_opaque = opaque;
    g_use_dbt = use_dbt;
    a32_signals_set_thread_ctx(ctx);
    if (a32_signals_init(ctx) != 0 || a32_vm_init(ctx, UINT64_MAX) != 0 ||
        a32_vm32_init(ctx) != 0 || a32_jit_init(ctx) != 0) {
        return 1;
    }
    a32_difftest_set_dbt_step(a32_dbt_single_step);
    if (do_boot(ctx, fn, opaque) != 0) {
        return 1;
    }
    return run_loop(ctx, fn, opaque, use_dbt, difftest, ctx->darwin_tid,
        false);
}

int a32_run_darwin_with_handler(a32h_darwin_handler_fn fn, void *opaque,
    bool use_dbt, bool difftest)
{
    static a32_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    return a32_run_darwin_ctx_with_handler(&ctx, fn, opaque, use_dbt,
        difftest);
}

int a32_darwin_boot_and_run(a32_ctx_t *ctx, bool use_dbt, bool difftest)
{
    return a32_run_darwin_ctx_with_handler(ctx, NULL, NULL, use_dbt,
        difftest);
}
