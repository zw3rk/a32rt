/*
 * tests/test_g02_darwin_bridge.c — G02 external Darwin bridge qualification
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * In-process qualification of src/os/darwin_host_bridge.c against a mock
 * A32H_DARWIN_BOOT / A32H_DARWIN_TRAP handler. Linked against the slim
 * A32RT_EMBED_IOS=0 object set (no src/ios dependency). Covers Thumb-2 +
 * ARM interworking, BSD svc C-bit protocol, Mach r12 traps, BKPT, mapped
 * vs. protection faults with SYNC recovery, SMC FLUSH rebind, DELIVER
 * VFP restore, SPAWN LDREX/STREX atomics + TLS, and interp-vs-dbt
 * lockstep with zero divergences.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "a32rt.h"
#include "os/darwin_host_bridge.h"

#define G_ARM   0x00100000U
#define G_THUMB 0x00200000U
#define G_WORK  0x00300000U
#define G_DATA  0x00400000U
#define G_STACK 0x00500000U
#define G_STACK_TOP 0x00504000U
#define G_FAULT 0x00600000U
#define G_RO    0x00700000U
#define G_SMC   0x00800000U

#define SVC_OK    0x1000U
#define SVC_ERR   0x1001U
#define SVC_EXIT  0x1FFFU
#define SVC_SPAWN 0x2000U
#define SVC_WAIT  0x2001U
#define SVC_FPU   0x2002U
#define SVC_FLUSH 0x2004U

#define PERSON_HASH 0x12345678ABCDEF01ULL
#define D0_PATTERN  0x1122334455667788ULL
#define FPSCR_PATTERN 0x03000000U
#define WORKER_TLS  0xDEADBEEFU
#define WORKER_R0   0xA5A5U

static const uint32_t g_arm_entry[] = {
    0xe3003001U, 0xe3403020U, 0xe12fff33U, 0xe3004000U, 0xe3404040U,
    0xe5840000U, 0xe3a0c000U, 0xe3017000U, 0xef000080U, 0xe10f5000U,
    0xe5845004U, 0xe5840008U, 0xe3a0c000U, 0xe3017001U, 0xef000080U,
    0xe10f5000U, 0xe584500cU, 0xe5840010U, 0xe3a0c000U, 0xe3017000U,
    0xef000081U, 0xe5840014U, 0xe3e0c01bU, 0xe3a07000U, 0xef000080U,
    0xe5840018U, 0xe7ffdefeU, 0xe3001000U, 0xe3401060U, 0xe5910000U,
    0xe584001cU, 0xe3001000U, 0xe3401070U, 0xe3a00042U, 0xe5810000U,
    0xe5912000U, 0xe5842020U, 0xe3003000U, 0xe3403080U, 0xe12fff33U,
    0xe5840024U, 0xe3001000U, 0xe3401080U, 0xe3000002U, 0xe34e03a0U,
    0xe5810000U, 0xe30f0f1eU, 0xe34e012fU, 0xe5810004U, 0xe1a00001U,
    0xe3a01008U, 0xe3a0c000U, 0xe3027004U, 0xef000080U, 0xe3003000U,
    0xe3403080U, 0xe12fff33U, 0xe5840028U, 0xe3a0c000U, 0xe3027002U,
    0xef000080U, 0xec510b10U, 0xe584002cU, 0xe5841030U, 0xe3a0c000U,
    0xe3027000U, 0xef000080U, 0xe3027001U, 0xef000080U, 0xe3a0002aU,
    0xe3a0c000U, 0xe3017fffU, 0xef000080U,
};

static const uint32_t g_thumb_fn[] = {
    0x20aaf64aU, 0x0c00f04fU, 0x0700f241U, 0xf240df80U, 0xf2c00400U,
    0x65200440U, 0xf241defeU, 0xf2c00100U, 0x68080160U, 0xf64a6560U,
    0x477020aaU,
};

static const uint32_t g_worker_fn[] = {
    0xee1d2f70U, 0xe3004000U, 0xe3404040U, 0xe5842078U, 0xe584007cU,
    0xe3001080U, 0xe3401040U, 0xe3a03064U, 0xe1912f9fU, 0xe2822001U,
    0xe1815f92U, 0xe3550000U, 0x1afffffaU, 0xe2533001U, 0x1afffff8U,
    0xe3a02001U, 0xe3001084U, 0xe3401040U, 0xe5812000U, 0xe12fff1eU,
};

static const uint32_t g_smc_v1[] = { 0xe3a00001U, 0xe12fff1eU };

static int g_failures = 0;

#define CHECK(cond, ...)                                                  \
    do {                                                                  \
        if (!(cond)) {                                                    \
            g_failures++;                                                 \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);          \
            fprintf(stderr, __VA_ARGS__);                                 \
            fprintf(stderr, "\n");                                        \
        }                                                                 \
    } while (0)

typedef struct {
    uint64_t window_base;
    uint64_t log[4096];
    uint32_t log_reason[4096];
    uint32_t log_pc[4096];
    size_t nlog;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int saw_thread_exit;
    int exit_status;
    int saw_exit;
    unsigned svc_ok;
    unsigned svc_err;
    unsigned svc_mach;
    unsigned svc_flush;
    unsigned svc_fpu;
    unsigned svc_spawn;
    unsigned svc_wait;
    unsigned bkpt;
    unsigned fault_unmapped;
    unsigned fault_prot;
    unsigned full_seen;
} mock_t;

static uint64_t mock_hash_trap(const a32h_darwin_trap_t *t,
    const a32h_darwin_cpu_state_t *cpu)
{
    uint64_t h = 0xCBF29CE484222325ULL;
    h ^= (uint64_t)t->exit_reason + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    h ^= (uint64_t)t->trap_imm + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    h ^= (uint64_t)t->fault_va + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    h ^= (uint64_t)t->fault_kind + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    h ^= (uint64_t)t->fault_is_prot + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    h ^= (uint64_t)cpu->pc + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    h ^= (uint64_t)cpu->cpsr_or_cr + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    for (unsigned i = 0U; i < 8U; i++) {
        h ^= (uint64_t)cpu->gpr[i] + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    }
    return h;
}

static void mock_map_fill(uint64_t base, uint32_t va, const uint32_t *words,
    size_t nwords)
{
    void *dst = (void *)(uintptr_t)(base + (uint64_t)va);
    void *out = mmap(dst, 0x1000U, PROT_READ | PROT_WRITE,
        MAP_FIXED | MAP_ANON | MAP_PRIVATE, -1, 0);
    if (out == MAP_FAILED || out != dst) {
        fprintf(stderr, "mock: mmap fixed failed\n");
        exit(1);
    }
    if (words != NULL && nwords > 0U) {
        memcpy(dst, words, nwords * sizeof(*words));
    }
}

static void mock_set_vmap(a32h_darwin_boot_t *b, unsigned idx, uint32_t va,
    uint32_t len, uint32_t prot)
{
    a32h_darwin_vmap_t *v = (a32h_darwin_vmap_t *)(uintptr_t)b->vmap_ptr;
    v[idx].va = va;
    v[idx].len = len;
    v[idx].prot = prot;
    v[idx].flags = 0U;
}

static int mock_boot(mock_t *m, a32h_darwin_boot_t *b)
{
    unsigned n = 0U;
    (void)b->abi_minor;
    m->window_base = b->window_base;
    mock_map_fill(b->window_base, G_ARM, g_arm_entry,
        sizeof(g_arm_entry) / sizeof(g_arm_entry[0]));
    mock_map_fill(b->window_base, G_THUMB, g_thumb_fn,
        sizeof(g_thumb_fn) / sizeof(g_thumb_fn[0]));
    mock_map_fill(b->window_base, G_WORK, g_worker_fn,
        sizeof(g_worker_fn) / sizeof(g_worker_fn[0]));
    mock_map_fill(b->window_base, G_SMC, g_smc_v1,
        sizeof(g_smc_v1) / sizeof(g_smc_v1[0]));
    mock_map_fill(b->window_base, G_DATA, NULL, 0U);
    mock_map_fill(b->window_base, G_STACK, NULL, 0U);
    mock_map_fill(b->window_base, G_RO, NULL, 0U);
    mock_set_vmap(b, n++, G_ARM, 0x1000U, PROT_READ | PROT_EXEC);
    mock_set_vmap(b, n++, G_THUMB, 0x1000U, PROT_READ | PROT_EXEC);
    mock_set_vmap(b, n++, G_WORK, 0x1000U, PROT_READ | PROT_EXEC);
    mock_set_vmap(b, n++, G_SMC, 0x1000U, PROT_READ | PROT_EXEC);
    mock_set_vmap(b, n++, G_DATA, 0x1000U, PROT_READ | PROT_WRITE);
    mock_set_vmap(b, n++, G_STACK, 0x4000U, PROT_READ | PROT_WRITE);
    mock_set_vmap(b, n++, G_RO, 0x1000U, PROT_READ);
    b->vmap_count = n;
    b->entry_pc = G_ARM;
    b->entry_sp = G_STACK_TOP;
    b->entry_r0 = 0U;
    b->entry_cpsr_or_msr = 0U;
    b->initial_tid = 1U;
    b->aot_personality_hash = PERSON_HASH;
    return 0;
}

static void mock_log(mock_t *m, const a32h_darwin_trap_t *t,
    const a32h_darwin_cpu_state_t *cpu)
{
    if (m->nlog < sizeof(m->log) / sizeof(m->log[0])) {
        m->log[m->nlog] = mock_hash_trap(t, cpu);
        m->log_reason[m->nlog] = t->exit_reason;
        m->log_pc[m->nlog] = cpu->pc;
        m->nlog++;
    }
}

static void mock_dump(const mock_t *m, const char *label)
{
    static const char *names[] = {
        "NONE", "SVC", "FAULT", "BKPT", "ILLEGAL", "THREAD_EXIT",
    };
    fprintf(stderr, "mock trace %s (%zu traps):\n", label, m->nlog);
    for (size_t i = 0U; i < m->nlog; i++) {
        uint32_t r = m->log_reason[i];
        fprintf(stderr, "  #%zu %s pc=0x%08x\n", i,
            r < 6U ? names[r] : "?", m->log_pc[i]);
    }
}

static int mock_trap_svc(mock_t *m, a32h_darwin_trap_t *t,
    a32h_darwin_cpu_state_t *cpu)
{
    uint32_t r7;
    if (t->trap_imm != 0x80U && t->trap_imm != 0x81U) {
        fprintf(stderr, "mock: bad svc imm 0x%x\n", t->trap_imm);
        return 1;
    }
    if ((int32_t)cpu->gpr[12] < 0) {
        cpu->gpr[0] = 0x12345678U; /* Mach trap: negative r12 */
        cpu->cpsr_or_cr &= ~A32_CPSR_C;
        m->svc_mach++;
        return 0;
    }
    r7 = cpu->gpr[7];
    switch (r7) {
    case SVC_OK:
        cpu->gpr[0] = 0x11111111U;
        cpu->cpsr_or_cr &= ~A32_CPSR_C;
        m->svc_ok++;
        return 0;
    case SVC_ERR:
        cpu->gpr[0] = 2U;
        cpu->cpsr_or_cr |= A32_CPSR_C;
        m->svc_err++;
        return 0;
    case SVC_EXIT:
        t->reply_action |= A32H_DACT_EXIT_PROCESS;
        t->exit_status = (int32_t)cpu->gpr[0];
        m->exit_status = (int)cpu->gpr[0];
        m->saw_exit = 1;
        return 0;
    case SVC_SPAWN:
        t->reply_action |= A32H_DACT_SPAWN_THREAD;
        t->spawn_tid = 7U;
        t->spawn_pc = G_WORK;
        t->spawn_sp = G_STACK_TOP - 0x800U;
        t->spawn_r0 = WORKER_R0;
        t->spawn_tls = WORKER_TLS;
        m->svc_spawn++;
        return 0;
    case SVC_WAIT: {
        struct timespec ts;
        int rc = 0;
        m->svc_wait++;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 10;
        while (!m->saw_thread_exit && rc == 0) {
            rc = pthread_cond_timedwait(&m->cv, &m->mu, &ts);
        }
        if (!m->saw_thread_exit) {
            fprintf(stderr, "mock: worker wait timed out\n");
            return 1;
        }
        return 0;
    }
    case SVC_FPU:
        if ((cpu->state_flags & A32H_DSTATE_FP_VALID) == 0U) {
            cpu->state_flags |= A32H_DSTATE_NEED_FULL;
            return 0;
        }
        if (cpu->fpr[0] != 0ULL) {
            fprintf(stderr, "mock: expected zero d0 snapshot, got 0x%llx\n",
                (unsigned long long)cpu->fpr[0]);
            return 1;
        }
        m->full_seen++;
        cpu->state_flags |= A32H_DSTATE_FP_DIRTY;
        for (unsigned i = 0U; i < 32U; i++) {
            cpu->fpr[i] = D0_PATTERN + (uint64_t)i;
        }
        cpu->fpscr = FPSCR_PATTERN;
        t->reply_action |= A32H_DACT_DELIVER_SIGNAL;
        m->svc_fpu++;
        return 0;
    case SVC_FLUSH:
        t->reply_action |= A32H_DACT_FLUSH_ICACHE;
        t->flush_va = cpu->gpr[0];
        t->flush_len = cpu->gpr[1];
        /* Patched code pages go back to RX so fetch faults clear. */
        t->reply_action |= A32H_DACT_SYNC_VM_PROT;
        t->prot_va = cpu->gpr[0] & ~0xFFFU;
        t->prot_len = 0x1000U;
        t->prot_flags = PROT_READ | PROT_EXEC;
        m->svc_flush++;
        return 0;
    default:
        fprintf(stderr, "mock: unknown svc r7=0x%x\n", r7);
        return 1;
    }
}

static int mock_in_range(uint32_t va, uint32_t base, uint32_t len)
{
    return va >= base && (uint64_t)va < (uint64_t)base + (uint64_t)len;
}

static int mock_trap_fault(mock_t *m, a32h_darwin_trap_t *t)
{
    if (mock_in_range(t->fault_va, G_FAULT, 0x2000U)) {
        if (t->fault_kind != A32H_DFAULT_READ || t->fault_is_prot != 0U) {
            fprintf(stderr, "mock: unmapped fault kind=%u is_prot=%u\n",
                t->fault_kind, t->fault_is_prot);
            return 1;
        }
        t->reply_action |= A32H_DACT_SYNC_VM_PROT;
        t->prot_va = t->fault_va & ~0xFFFU;
        t->prot_len = 0x1000U;
        t->prot_flags = PROT_READ | PROT_WRITE;
        m->fault_unmapped++;
        return 0;
    }
    if (mock_in_range(t->fault_va, G_RO, 0x1000U) ||
        mock_in_range(t->fault_va, G_SMC, 0x1000U)) {
        if (t->fault_kind != A32H_DFAULT_WRITE || t->fault_is_prot != 1U) {
            fprintf(stderr, "mock: prot fault kind=%u is_prot=%u va=0x%x\n",
                t->fault_kind, t->fault_is_prot, t->fault_va);
            return 1;
        }
        t->reply_action |= A32H_DACT_SYNC_VM_PROT;
        t->prot_va = t->fault_va & ~0xFFFU;
        t->prot_len = 0x1000U;
        t->prot_flags = PROT_READ | PROT_WRITE;
        m->fault_prot++;
        return 0;
    }
    fprintf(stderr, "mock: unexpected fault va=0x%x kind=%u prot=%u\n",
        t->fault_va, t->fault_kind, t->fault_is_prot);
    return 1;
}

static int mock_trap(mock_t *m, a32h_darwin_trap_t *t)
{
    a32h_darwin_cpu_state_t *cpu;
    int rc;
    if (t->arch != A32H_ARCH_ARM32) {
        fprintf(stderr, "mock: bad arch %u\n", t->arch);
        return 1;
    }
    cpu = (a32h_darwin_cpu_state_t *)(uintptr_t)t->cpu_state_ptr;
    if (cpu == NULL) {
        return 1;
    }
    switch (t->exit_reason) {
    case A32H_DEXIT_SVC_TRAP:
        rc = mock_trap_svc(m, t, cpu);
        break;
    case A32H_DEXIT_FAULT:
        rc = mock_trap_fault(m, t);
        break;
    case A32H_DEXIT_BKPT: {
        bool thumb = (cpu->cpsr_or_cr & A32_CPSR_T) != 0U;
        cpu->pc += thumb ? 2U : 4U;
        m->bkpt++;
        rc = 0;
        break;
    }
    case A32H_DEXIT_THREAD_EXIT:
        m->saw_thread_exit = 1;
        pthread_cond_signal(&m->cv);
        rc = 0;
        break;
    default:
        fprintf(stderr, "mock: unexpected exit %u\n", t->exit_reason);
        return 1;
    }
    if (rc == 0) {
        mock_log(m, t, cpu);
    }
    return rc;
}

static int mock_handler(void *opaque, unsigned long req, void *arg)
{
    mock_t *m = (mock_t *)opaque;
    int rc;
    pthread_mutex_lock(&m->mu);
    if (req == (unsigned long)A32H_DARWIN_BOOT) {
        rc = mock_boot(m, (a32h_darwin_boot_t *)arg);
    } else if (req == (unsigned long)A32H_DARWIN_TRAP) {
        rc = mock_trap(m, (a32h_darwin_trap_t *)arg);
    } else {
        fprintf(stderr, "mock: unknown req 0x%lx\n", req);
        rc = 1;
    }
    pthread_mutex_unlock(&m->mu);
    return rc;
}

static uint32_t mock_data(mock_t *m, uint32_t off)
{
    uint32_t v = 0U;
    memcpy(&v, (const void *)(uintptr_t)(m->window_base + (uint64_t)G_DATA + (uint64_t)off), 4U);
    return v;
}

static void mock_init(mock_t *m)
{
    memset(m, 0, sizeof(*m));
    pthread_mutex_init(&m->mu, NULL);
    pthread_cond_init(&m->cv, NULL);
}

static void mock_fini(mock_t *m)
{
    pthread_mutex_destroy(&m->mu);
    pthread_cond_destroy(&m->cv);
}

/* NOTE: the bridge zeroes ctx and runs boot itself; the caller only
 * inspects the filled ctx afterwards. */
static int run_case(mock_t *m, a32_ctx_t *ctx, bool use_dbt, bool difftest,
    const char *label)
{
    int rc;
    memset(ctx, 0, sizeof(*ctx));
    rc = a32_run_darwin_ctx_with_handler(ctx, mock_handler, m, use_dbt,
        difftest);
    CHECK(rc == 42, "%s: exit status %d, want 42", label, rc);
    CHECK(m->saw_exit && m->exit_status == 42, "%s: no clean EXIT", label);
    CHECK(ctx->is_ios && ctx->darwin_external, "%s: personality flags", label);
    CHECK(ctx->darwin_tid == 1U, "%s: tid %u", label, ctx->darwin_tid);
    CHECK(ctx->darwin_personality_hash == PERSON_HASH, "%s: person hash",
        label);
    CHECK(m->bkpt == 2U, "%s: bkpt %u, want 2", label, m->bkpt);
    CHECK(m->svc_ok == 3U, "%s: svc_ok %u, want 3", label, m->svc_ok);
    CHECK(m->svc_err == 1U, "%s: svc_err %u", label, m->svc_err);
    CHECK(m->svc_mach == 1U, "%s: mach %u", label, m->svc_mach);
    CHECK(m->svc_flush == 1U, "%s: flush %u", label, m->svc_flush);
    CHECK(m->svc_fpu == 1U, "%s: fpu %u", label, m->svc_fpu);
    CHECK(m->svc_spawn == 1U, "%s: spawn %u", label, m->svc_spawn);
    CHECK(m->svc_wait == 1U, "%s: wait %u", label, m->svc_wait);
    CHECK(m->fault_unmapped == 2U, "%s: unmapped %u, want 2", label,
        m->fault_unmapped);
    CHECK(m->fault_prot == 2U, "%s: prot %u, want 2", label, m->fault_prot);
    CHECK(m->full_seen == 1U, "%s: full %u", label, m->full_seen);
    CHECK(m->saw_thread_exit, "%s: no THREAD_EXIT", label);
    CHECK(mock_data(m, 0U) == 0xAAAAU, "%s: interwork 0x%x", label,
        mock_data(m, 0U));
    CHECK((mock_data(m, 4U) & A32_CPSR_C) == 0U, "%s: BSD ok C set", label);
    CHECK(mock_data(m, 8U) == 0x11111111U, "%s: BSD ok r0", label);
    CHECK((mock_data(m, 12U) & A32_CPSR_C) != 0U, "%s: BSD err C clear",
        label);
    CHECK(mock_data(m, 16U) == 2U, "%s: BSD err r0", label);
    CHECK(mock_data(m, 20U) == 0x11111111U, "%s: svc81 r0", label);
    CHECK(mock_data(m, 24U) == 0x12345678U, "%s: mach r0", label);
    CHECK(mock_data(m, 28U) == 0U, "%s: recovered read", label);
    CHECK(mock_data(m, 32U) == 0x42U, "%s: recovered write", label);
    CHECK(mock_data(m, 36U) == 1U, "%s: smc v1", label);
    CHECK(mock_data(m, 40U) == 2U, "%s: smc v2", label);
    CHECK(mock_data(m, 44U) == (uint32_t)(D0_PATTERN & 0xFFFFFFFFULL),
        "%s: d0 lo", label);
    CHECK(mock_data(m, 48U) == (uint32_t)(D0_PATTERN >> 32), "%s: d0 hi",
        label);
    CHECK(mock_data(m, 80U) == 0x11111111U, "%s: thumb svc", label);
    CHECK(mock_data(m, 84U) == 0U, "%s: thumb read", label);
    CHECK(mock_data(m, 120U) == WORKER_TLS, "%s: tls 0x%x", label,
        mock_data(m, 120U));
    CHECK(mock_data(m, 124U) == WORKER_R0, "%s: cookie", label);
    CHECK(mock_data(m, 128U) == 100U, "%s: counter %u", label,
        mock_data(m, 128U));
    CHECK(mock_data(m, 132U) == 1U, "%s: done flag", label);
    CHECK(ctx->fpscr == FPSCR_PATTERN, "%s: fpscr 0x%x", label, ctx->fpscr);
    CHECK(ctx->vfp_d[1] == D0_PATTERN + 1ULL, "%s: d1 restore", label);
    CHECK(ctx->vfp_d[31] == D0_PATTERN + 31ULL, "%s: d31 restore", label);
    if (use_dbt && !difftest) {
        CHECK(ctx->blocks_compiled > 0U, "%s: dbt compiled nothing", label);
    }
    if (rc != 42) {
        mock_dump(m, label);
    }
    printf("PASS g02 %s (exit=%d traps=%zu)\n", label, rc, m->nlog);
    return rc;
}

static void test_units(void)
{
    bool boot = false;
    char *argv1[] = { (char *)"a32rt", (char *)"--personality", (char *)"hl-darwin", (char *)"x" };
    char *argv2[] = { (char *)"a32rt", (char *)"--darwin-boot", (char *)"x" };
    char *argv3[] = { (char *)"a32rt", (char *)"--personality=hl-darwin" };
    char *argv4[] = { (char *)"a32rt", (char *)"x" };
    uint64_t h = 0xCBF29CE484222325ULL;
    a32_ctx_t c;
    CHECK(a32_darwin_cli_wants_bridge(4, argv1, &boot) && !boot, "cli hl-darwin");
    CHECK(a32_darwin_cli_wants_bridge(3, argv2, &boot) && boot, "cli darwin-boot");
    CHECK(a32_darwin_cli_wants_bridge(2, argv3, NULL), "cli personality=");
    CHECK(!a32_darwin_cli_wants_bridge(2, argv4, NULL), "cli negative");
    CHECK(!a32_darwin_is_macho_fd(-1), "macho fd -1");
    CHECK(!a32_darwin_is_macho_fd(999999), "macho fd bad");
    CHECK(a32_darwin_mix_hash(h, PERSON_HASH) != h, "mix changes key");
    CHECK(a32_darwin_mix_hash(h, PERSON_HASH) ==
        a32_darwin_mix_hash(h, PERSON_HASH), "mix stable");
    CHECK(a32_darwin_mix_hash(h, PERSON_HASH) != a32_darwin_mix_hash(h, 2ULL),
        "mix isolates personalities");
    memset(&c, 0, sizeof(c));
    CHECK(!a32_darwin_auto_bridge(&c, true, false), "auto without host");
    c.has_host_info = true;
    c.host_info.abi_major = A32H_ABI_MAJOR;
    c.host_info.caps = A32H_CAP_DARWIN_PERSONALITY;
    CHECK(a32_darwin_auto_bridge(&c, true, false), "auto macho");
    CHECK(a32_darwin_auto_bridge(&c, false, true), "auto boot flag");
    CHECK(!a32_darwin_auto_bridge(&c, false, false), "auto negative");
    printf("PASS g02 units\n");
}

int main(void)
{
    static a32_ctx_t ctx_interp;
    static a32_ctx_t ctx_dbt;
    static a32_ctx_t ctx_diff;
    mock_t mi;
    mock_t md;
    mock_t mf;
    int rc_interp, rc_dbt, rc_diff;
    int fails_before;
    test_units();
    mock_init(&mi);
    rc_interp = run_case(&mi, &ctx_interp, false, false, "interp");
    mock_init(&md);
    rc_dbt = run_case(&md, &ctx_dbt, true, false, "dbt");
    fails_before = g_failures;
    CHECK(rc_interp == 42 && rc_dbt == 42, "run rc");
    CHECK(mi.nlog == md.nlog && mi.nlog > 0U, "trap counts %zu vs %zu",
        mi.nlog, md.nlog);
    if (mi.nlog == md.nlog && mi.nlog > 0U) {
        CHECK(memcmp(mi.log, md.log, mi.nlog * sizeof(mi.log[0])) == 0,
            "interp vs dbt trap log diverges");
    }
    if (g_failures == fails_before) {
        printf("PASS g02 interp==dbt (%zu traps)\n", mi.nlog);
    }
    mock_init(&mf);
    rc_diff = run_case(&mf, &ctx_diff, true, true, "difftest");
    CHECK(rc_diff == 42, "difftest rc");
    if (g_failures == 0) {
        printf("PASS g02 darwin bridge: 0 divergences\n");
    }
    mock_fini(&mi);
    mock_fini(&md);
    mock_fini(&mf);
    if (g_failures != 0) {
        fprintf(stderr, "FAIL g02: %d checks failed\n", g_failures);
        return 1;
    }
    printf("PASS g02 all\n");
    return 0;
}