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
 */

#define _GNU_SOURCE
#include "os/darwin_host_bridge.h"

#include "dbt/difftest.h"
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

#define DARWIN_VMAP_CAP 64U
#define DARWIN_RETRAP_SAME_MAX 256U

static a32h_darwin_handler_fn g_fn = NULL;
static void *g_opaque = NULL;
static bool g_use_dbt = false;

static int host_ioctl(unsigned long req, void *arg)
{
    int fd = open(A32H_DEV_PATH, O_RDWR | O_CLOEXEC);
    int rc, e;
    if (fd < 0) {
        return -ENODEV;
    }
#if defined(__linux__)
    rc = (int)syscall(SYS_ioctl, fd, req, arg);
#else
    rc = ioctl(fd, req, arg);
#endif
    e = errno;
    close(fd);
    return (rc != 0) ? ((e != 0) ? -e : -EIO) : 0;
}

static int call_trap(a32h_darwin_handler_fn fn, void *opaque,
    unsigned long req, void *arg)
{
    return (fn != NULL) ? fn(opaque, req, arg) : host_ioctl(req, arg);
}

bool a32_darwin_cli_wants_bridge(int argc, char **argv, bool *out_darwin_boot)
{
    bool boot = false, want = false;
    const char *e;
    for (int i = 1; i < argc; i++) {
        const char *s = argv[i];
        if (s == NULL) {
            continue;
        } else if (strcmp(s, "--darwin-boot") == 0) {
            boot = true;
            want = true;
        } else if (strcmp(s, "--personality") == 0 && i + 1 < argc &&
                   argv[i + 1] != NULL && strcmp(argv[i + 1], "hl-darwin") == 0) {
            want = true;
        } else if (strncmp(s, "--personality=", 14) == 0 &&
                   strcmp(s + 14, "hl-darwin") == 0) {
            want = true;
        }
    }
    e = getenv("A32RT_PERSONALITY");
    if (e != NULL && strcmp(e, "hl-darwin") == 0) {
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

/* r0..r15, pc, cpsr (N/Z/C/V + T), fpscr, tls_tp; d0..d31 only when full. */
static void fill_cpu(const a32_ctx_t *ctx, a32h_darwin_cpu_state_t *cpu,
    bool full)
{
    memset(cpu, 0, sizeof(*cpu));
    for (unsigned i = 0U; i < 16U; i++) {
        cpu->gpr[i] = ctx->regs[i];
    }
    cpu->pc = ctx->regs[15];
    cpu->next_pc = ctx->regs[15] + ((ctx->cpsr & A32_CPSR_T) != 0U ? 2U : 4U);
    cpu->lr = ctx->regs[14];
    cpu->cpsr_or_cr = ctx->cpsr;
    cpu->fpscr = ctx->fpscr;
    cpu->tls_tp = ctx->tls_tp;
    if (full) {
        cpu->state_flags = A32H_DSTATE_FP_VALID;
        for (unsigned i = 0U; i < 32U; i++) {
            cpu->fpr[i] = ctx->vfp_d[i];
        }
    }
}

static void apply_cpu(a32_ctx_t *ctx, const a32h_darwin_cpu_state_t *cpu)
{
    bool t = ((cpu->pc & 1U) != 0U) || ((cpu->cpsr_or_cr & A32_CPSR_T) != 0U);
    for (unsigned i = 0U; i < 16U; i++) {
        ctx->regs[i] = cpu->gpr[i];
    }
    ctx->regs[14] = cpu->lr;
    ctx->cpsr = (cpu->cpsr_or_cr & ~A32_CPSR_T) | (t ? A32_CPSR_T : 0U);
    ctx->regs[15] = t ? (cpu->pc & ~1U) : (cpu->pc & ~3U);
    ctx->tls_tp = cpu->tls_tp;
    ctx->fpscr = cpu->fpscr;
    if ((cpu->state_flags & A32H_DSTATE_FP_DIRTY) != 0U) {
        for (unsigned i = 0U; i < 32U; i++) {
            ctx->vfp_d[i] = cpu->fpr[i];
        }
    }
}

/* DEXIT_FAULT classifier: exec when VA is the PC, else load vs. store. */
static uint32_t fault_kind_for(const a32_ctx_t *ctx)
{
    uint32_t pc = ctx->regs[15];
    uint32_t w = 0U;
    uint16_t hw = 0U;
    if (ctx->fault_addr == (pc & ~1U)) {
        return A32H_DFAULT_EXEC;
    }
    if ((ctx->cpsr & A32_CPSR_T) != 0U) {
        if (!a32_guest_range_ok((a32_ctx_t *)ctx, pc & ~1U, 2U,
                (uint32_t)PROT_READ)) {
            return A32H_DFAULT_READ;
        }
        memcpy(&hw, a32_guest_ptr(ctx, pc & ~1U), 2U);
        if ((hw & 0xF800U) == 0x6000U) {
            return ((hw & 0x0800U) != 0U) ? A32H_DFAULT_READ : A32H_DFAULT_WRITE;
        }
        if ((hw & 0xFF00U) == 0xB400U || (hw & 0xF800U) == 0xC000U) {
            return A32H_DFAULT_WRITE;
        }
        return A32H_DFAULT_READ;
    }
    if (!a32_guest_range_ok((a32_ctx_t *)ctx, pc & ~3U, 4U,
            (uint32_t)PROT_READ)) {
        return A32H_DFAULT_READ;
    }
    memcpy(&w, a32_guest_ptr(ctx, pc & ~3U), 4U);
    if ((w & 0x0FF000F0U) == 0x01800090U ||
        (((w & 0x0C000000U) == 0x04000000U ||
          (w & 0x0E000000U) == 0x08000000U) &&
         (((w >> 20) & 1U) == 0U))) {
        return A32H_DFAULT_WRITE;
    }
    return A32H_DFAULT_READ;
}

static int register_range(a32_ctx_t *ctx, uint32_t s, uint32_t e, uint32_t gp,
    const char *name);

static int do_boot(a32_ctx_t *ctx, a32h_darwin_handler_fn fn, void *opaque)
{
    a32h_darwin_vmap_t vmaps[DARWIN_VMAP_CAP];
    a32h_darwin_boot_t boot;
    bool t;
    memset(vmaps, 0, sizeof(vmaps));
    memset(&boot, 0, sizeof(boot));
    boot.arch = A32H_ARCH_ARM32;
    boot.abi_minor = A32H_ABI_MINOR_DARWIN;
    boot.window_base = ctx->guest_base;
    boot.vmap_ptr = (uint64_t)(uintptr_t)vmaps;
    boot.vmap_cap = DARWIN_VMAP_CAP;
    if (call_trap(fn, opaque, A32H_DARWIN_BOOT, &boot) != 0 ||
        boot.arch != A32H_ARCH_ARM32 || boot.entry_sp == 0U ||
        boot.vmap_count > DARWIN_VMAP_CAP) {
        fprintf(stderr, "a32rt: A32H_DARWIN_BOOT failed\n");
        return -1;
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
        end = (end64 >= 0x100000000ULL) ? 0xFFFFFFFFU
                                        : (((uint32_t)end64 + 0xFFFU) & ~0xFFFU);
        if (end <= start) {
            continue;
        }
        gp = vmaps[i].prot & 0x7U;
        if (register_range(ctx, start, end, gp, "darwin-boot") != 0) {
            return -1;
        }
    }
    ctx->darwin_personality_hash = boot.aot_personality_hash;
    ctx->darwin_tid = boot.initial_tid;
    ctx->is_ios = true; /* 0xDEFE -> BKPT, CP15 TPIDRURO/CNTVCT live */
    ctx->darwin_external = true;
    ctx->regs[15] = boot.entry_pc & ~1U;
    t = ((boot.entry_pc & 1U) != 0U) ||
        ((boot.entry_cpsr_or_msr & A32_CPSR_T) != 0U);
    ctx->cpsr = (boot.entry_cpsr_or_msr & A32_CPSR_NZCV_MASK) |
                (t ? A32_CPSR_T : 0U);
    ctx->regs[13] = boot.entry_sp;
    ctx->regs[0] = boot.entry_r0;
    return 0;
}

static void build_trap(const a32_ctx_t *ctx, uint32_t tid, uint32_t dexit,
    a32h_darwin_cpu_state_t *cpu, a32h_darwin_trap_t *trap)
{
    memset(cpu, 0, sizeof(*cpu));
    memset(trap, 0, sizeof(*trap));
    trap->arch = A32H_ARCH_ARM32;
    trap->tid = tid;
    trap->exit_reason = dexit;
    trap->cpu_state_ptr = (uint64_t)(uintptr_t)cpu;
    if (dexit == A32H_DEXIT_SVC_TRAP) {
        trap->trap_imm = ctx->svc_imm;
    } else if (dexit == A32H_DEXIT_FAULT) {
        trap->fault_va = ctx->fault_addr;
        trap->fault_kind = fault_kind_for(ctx);
        trap->fault_is_prot =
            a32_guest_range_ok((a32_ctx_t *)ctx, ctx->fault_addr, 1U, 0U) ? 1U
                                                                         : 0U;
    }
    fill_cpu(ctx, cpu, false);
}

/* 0 ok, -1 fatal; refills with full FP and resends on NEED_FULL. */
static int send_trap(a32_ctx_t *ctx, a32h_darwin_handler_fn fn, void *opaque,
    a32h_darwin_cpu_state_t *cpu, a32h_darwin_trap_t *trap)
{
    if (call_trap(fn, opaque, A32H_DARWIN_TRAP, trap) != 0) {
        return -1;
    }
    if (((cpu->state_flags & A32H_DSTATE_NEED_FULL) != 0U) &&
        ((cpu->state_flags & A32H_DSTATE_FP_VALID) == 0U)) {
        fill_cpu(ctx, cpu, true);
        if (call_trap(fn, opaque, A32H_DARWIN_TRAP, trap) != 0) {
            return -1;
        }
    }
    return 0;
}

/* Map [s, e) RW (or reuse the provider's pre-mapped pages); keep contents. */
static int stage_map(a32_ctx_t *ctx, uint32_t s, uint32_t e)
{
    void *hva = (void *)(uintptr_t)(ctx->guest_base + (uint64_t)s);
    void *out = NULL;
    size_t mlen = (size_t)(e - s);
    if ((a32_backend_map_anon(hva, mlen, PROT_READ | PROT_WRITE, 1, 0, 0,
             &out) != 0 ||
         out != hva) &&
        a32_backend_protect(hva, mlen, PROT_READ | PROT_WRITE) != 0) {
        return -1;
    }
    return 0;
}

/* Stage, record, and protect [s, e) with guest prot gp (contents kept). */
static int register_range(a32_ctx_t *ctx, uint32_t s, uint32_t e, uint32_t gp,
    const char *name)
{
    if (stage_map(ctx, s, e) != 0 ||
        a32_vm_record_region(ctx, s, e, gp, 0U, name) != 0 ||
        a32_vm32_protect(ctx, s, (size_t)(e - s), gp) != 0) {
        return -1;
    }
    return 0;
}

static int run_loop(a32_ctx_t *ctx, a32h_darwin_handler_fn fn, void *opaque,
    bool use_dbt, bool difftest, uint32_t tid, bool is_worker);

static void send_thread_exit(a32_ctx_t *ctx, a32h_darwin_handler_fn fn,
    void *opaque, uint32_t tid)
{
    a32h_darwin_cpu_state_t cpu;
    a32h_darwin_trap_t trap;
    build_trap(ctx, tid, A32H_DEXIT_THREAD_EXIT, &cpu, &trap);
    if (call_trap(fn, opaque, A32H_DARWIN_TRAP, &trap) == 0) {
        apply_cpu(ctx, &cpu);
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

/* 0 resume, 1 exit requested (status in trap->exit_status), -1 fatal. */
static int apply_reply(a32_ctx_t *ctx, uint32_t tid,
    a32h_darwin_cpu_state_t *cpu, a32h_darwin_trap_t *trap)
{
    uint32_t act = trap->reply_action;
    (void)tid;
    apply_cpu(ctx, cpu);
    if ((act & A32H_DACT_FLUSH_ICACHE) != 0U && trap->flush_len != 0U) {
        a32_vm32_copyout_invalidate_exec(ctx, trap->flush_va, trap->flush_len);
    }
    if ((act & A32H_DACT_SYNC_VM_PROT) != 0U && trap->prot_len != 0U) {
        uint64_t end64 = (uint64_t)trap->prot_va + (uint64_t)trap->prot_len;
        uint32_t s = trap->prot_va & ~0xFFFU;
        uint32_t gp = trap->prot_flags & 0x7U;
        uint32_t e;
        if (end64 > 0x100000000ULL) {
            end64 = 0x100000000ULL;
        }
        e = (end64 >= 0x100000000ULL)
                ? 0xFFFFFFFFU
                : (((uint32_t)end64 + 0xFFFU) & ~0xFFFU);
        if (e > s) {
            if (!a32_guest_range_ok(ctx, s, e - s, 0U)) {
                if (register_range(ctx, s, e, gp, "darwin-prot") != 0) {
                    return -1;
                }
            } else if (a32_vm_mprotect(ctx, s, e - s, gp) != 0) {
                return -1;
            }
            (void)a32_vm32_sync_region(s, e, (gp & 0x1U) != 0U,
                (gp & 0x2U) != 0U, (gp & 0x4U) != 0U);
        }
    }
    if ((act & A32H_DACT_SPAWN_THREAD) == 0U) {
        return ((act & A32H_DACT_EXIT_PROCESS) != 0U) ? 1 : 0;
    }
    {
        a32_ctx_t *child = (a32_ctx_t *)malloc(sizeof(a32_ctx_t));
        pthread_t th;
        bool t;
        if (child == NULL) {
            return -1;
        }
        memcpy(child, ctx, sizeof(*child));
        t = (trap->spawn_pc & 1U) != 0U;
        child->regs[15] = trap->spawn_pc & ~1U;
        child->cpsr = (child->cpsr & ~A32_CPSR_T) | (t ? A32_CPSR_T : 0U);
        child->regs[13] = trap->spawn_sp;
        child->regs[0] = trap->spawn_r0;
        child->regs[14] = 0U; /* return lands on VA 0 -> thread exit */
        child->tls_tp = trap->spawn_tls;
        child->darwin_tid = trap->spawn_tid;
        child->is_thread_clone = true;
        child->last_exec_entry = NULL;
        child->exit_reason = A32_EXIT_NONE;
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
    if (rc == A32_EXIT_UNDEF && a32_interp_step(ctx) != A32_EXIT_UNDEF) {
        return ctx->exit_reason;
    }
    return rc;
}

static bool rc_continue(uint32_t rc)
{
    return rc == A32_EXIT_NONE || rc == A32_EXIT_BRANCH ||
           rc == A32_EXIT_INTERWORK;
}

static int run_loop(a32_ctx_t *ctx, a32h_darwin_handler_fn fn, void *opaque,
    bool use_dbt, bool difftest, uint32_t tid, bool is_worker)
{
    static a32_ctx_t dut; /* ~80 KiB: keep off pthread stacks */
    bool have_dut = difftest && !is_worker;
    uint64_t compared = 0ULL;
    uint32_t last_pc = 0U, last_dexit = 0U, same_count = 0U;
    int exit_status = 0;
    if (have_dut) {
        memcpy(&dut, ctx, sizeof(dut));
    }
    for (;;) {
        uint32_t pc = ctx->regs[15];
        uint32_t rc, dexit = A32H_DEXIT_NONE;
        a32h_darwin_cpu_state_t cpu;
        a32h_darwin_trap_t trap;
        int ar;
        if (pc == 0U && is_worker) { /* lr=0 return: thread exit */
            send_thread_exit(ctx, fn, opaque, tid);
            return 0;
        }
        /* Async host signals stay queued in pending_signals; the host owns
         * the external lifecycle and drains them via DELIVER_SIGNAL traps. */
        if (have_dut) {
            memcpy(&dut, ctx, sizeof(dut));
            memset(dut.ibtc, 0, sizeof(dut.ibtc));
            memset(dut.ras, 0, sizeof(dut.ras));
            dut.ras_top = 0U;
        }
        rc = step_once(ctx, use_dbt);
        ctx->blocks_executed++;
        if (have_dut) {
            uint32_t d_rc = step_once(&dut, true);
            if (rc_continue(rc) != rc_continue(d_rc) ||
                (!rc_continue(rc) && rc != d_rc) ||
                !a32_interp_state_equal(ctx, &dut) ||
                ctx->itstate != dut.itstate || ctx->tls_tp != dut.tls_tp ||
                ctx->fault_addr != dut.fault_addr ||
                ctx->svc_imm != dut.svc_imm) {
                fprintf(stderr,
                    "[darwin-difftest] DIVERGENCE at 0x%08x rc=%u d=%u\n", pc,
                    rc, d_rc);
                return 1;
            }
        }
        if (rc_continue(rc)) {
            compared++;
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
            return 1;
        }
        if (dexit == last_dexit && pc == last_pc) {
            if (++same_count > DARWIN_RETRAP_SAME_MAX) {
                fprintf(stderr, "a32rt: darwin bridge: trap loop 0x%08x\n",
                    pc);
                return 1;
            }
        } else {
            last_dexit = dexit;
            last_pc = pc;
            same_count = 0U;
        }
        build_trap(ctx, tid, dexit, &cpu, &trap);
        if (send_trap(ctx, fn, opaque, &cpu, &trap) != 0) {
            return 1;
        }
        ar = apply_reply(ctx, tid, &cpu, &trap);
        if (ar < 0) {
            return 1;
        }
        if (ar > 0) {
            exit_status = trap.exit_status;
            break;
        }
        if (have_dut) {
            memcpy(&dut, ctx, sizeof(dut));
        }
        compared++;
    }
    if (is_worker) {
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
    build_trap(ctx, ctx->darwin_tid, A32H_DEXIT_SVC_TRAP, &cpu, &trap);
    if (send_trap(ctx, NULL, NULL, &cpu, &trap) != 0) {
        return -1;
    }
    ar = apply_reply(ctx, ctx->darwin_tid, &cpu, &trap);
    if (ar != 0) {
        ctx->regs[0] = (ar > 0) ? (uint32_t)trap.exit_status : 1U;
        return 1;
    }
    return 0;
}

void a32_darwin_enqueue_signal(a32_ctx_t *ctx, int sig)
{
    if (ctx == NULL || sig < 1 || sig > 64) {
        return;
    }
    __atomic_fetch_or(&ctx->pending_signals, 1ULL << (sig - 1),
        __ATOMIC_ACQ_REL);
}

/* Sync-fault exit for external guests: SEGV/BUS become DEXIT_FAULT through
 * the trap loop; anything else is fatal (no Linux/iOS frame to deliver). */
uint32_t a32_darwin_sync_exit(int sig)
{
    if (sig != SIGSEGV && sig != SIGBUS) {
        _exit(128 + sig);
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
