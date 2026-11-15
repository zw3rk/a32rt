/*
 * src/main.c — CLI parsing, AT_EXECFD, host probing, and startup orchestration
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#define _GNU_SOURCE
#include "a32rt.h"
#include "dbt/difftest.h"
#include "ios/ios.h"
#include "vm/vm32.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__linux__)
#include <sys/auxv.h>
#endif
#include <sys/syscall.h>
#include <unistd.h>

uint64_t a32_host_monotonic_ns(void)
{
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

void a32_print_stats(const a32_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    fprintf(stderr,
        "[a32rt stats] blocks_compiled=%llu blocks_executed=%llu chain_hits=%llu "
        "ibtc_hits=%llu ras_hits=%llu flags_elided=%llu syscalls=%llu "
        "icache_flushes=%llu wx_transitions=%llu\n",
        (unsigned long long)ctx->blocks_compiled,
        (unsigned long long)ctx->blocks_executed,
        (unsigned long long)ctx->chain_hits,
        (unsigned long long)ctx->ibtc_hits,
        (unsigned long long)ctx->ras_hits,
        (unsigned long long)ctx->flags_elided,
        (unsigned long long)ctx->syscalls_executed,
        (unsigned long long)ctx->icache_flushes,
        (unsigned long long)ctx->wx_transitions);
    fprintf(stderr,
        "[a32rt stats-ext] ns_vm_init=%llu ns_translate=%llu ns_wx_flush=%llu "
        "aprr_toggles=%llu tlb_checks=%llu tlb_groups=%llu tlb_slow=%llu "
        "vfp_copies=%llu fpcr_sw=%llu fpcr_slow=%llu aot_abs64=%llu "
        "exits=%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
        (unsigned long long)ctx->ns_vm_init,
        (unsigned long long)ctx->ns_translate,
        (unsigned long long)ctx->ns_wx_flush,
        (unsigned long long)ctx->aprr_toggles,
        (unsigned long long)ctx->tlb_checks_emitted,
        (unsigned long long)ctx->tlb_group_guards,
        (unsigned long long)ctx->tlb_slow_hits,
        (unsigned long long)ctx->vfp_cache_copies_emitted,
        (unsigned long long)ctx->fpcr_switches_emitted,
        (unsigned long long)ctx->fpcr_guard_slow_hits,
        (unsigned long long)ctx->aot_abs64_words,
        (unsigned long long)ctx->exits_by_reason[1],
        (unsigned long long)ctx->exits_by_reason[2],
        (unsigned long long)ctx->exits_by_reason[3],
        (unsigned long long)ctx->exits_by_reason[4],
        (unsigned long long)ctx->exits_by_reason[5],
        (unsigned long long)ctx->exits_by_reason[6],
        (unsigned long long)ctx->exits_by_reason[7]);
}

static bool getenv_bool(const char *name)
{
    const char *s = getenv(name);
    return s != NULL && strcmp(s, "1") == 0;
}

static bool ends_with(const char *str, const char *suffix)
{
    size_t slen = strlen(str);
    size_t xlen = strlen(suffix);
    if (slen < xlen) {
        return false;
    }
    return strcmp(str + (slen - xlen), suffix) == 0;
}

static bool parse_port(const char *s, int *out_port)
{
    if (s == NULL || s[0] == '\0') {
        return false;
    }
    char *endp = NULL;
    long port = strtol(s, &endp, 10);
    if (endp == NULL || endp == s || *endp != '\0' || port <= 0 || port > 65535) {
        return false;
    }
    *out_port = (int)port;
    return true;
}

static const char *strip_sysroot_prefix(const char *path, const char *prefix)
{
    if (prefix == NULL || prefix[0] == '\0') {
        return NULL;
    }
    size_t len = strlen(prefix);
    while (len > 1U && prefix[len - 1U] == '/') {
        len--;
    }
    if (len > 1U && strncmp(path, prefix, len) == 0 && path[len] == '/') {
        return path + len;
    }
    return NULL;
}

static uint64_t getenv_hex(const char *name)
{
    const char *s = getenv(name);
    char *endp = NULL;
    unsigned long long v;
    if (s == NULL || s[0] == '\0') {
        return 0ULL;
    }
    errno = 0;
    v = strtoull(s, &endp, 16);
    if (errno != 0 || endp == NULL || endp == s || *endp != '\0') {
        return 0ULL;
    }
    return (uint64_t)v;
}

/* Transport keys for the execve signal-state carry (see the execve
 * path in src/os/os_sys_sig_time.c). */
static bool is_exec_sig_key(const char *s)
{
    return strncmp(s, "A32RT_EXEC_SIGMASK=", 19) == 0 ||
           strncmp(s, "A32RT_EXEC_SIGIGN=", 18) == 0 ||
           strncmp(s, "A32RT_EXEC_SIGPEND=", 19) == 0;
}

/* Filtered process-lifetime copy of envp without transport entries
 * (A32-R4-7): main's envp points at the initial stack image, which
 * unsetenv() does not rewrite, so the guest stack builder must receive
 * a copy with stale entries removed — otherwise the guest inherits
 * them verbatim and the next execve would carry duplicates. */
static char **filter_exec_sig_env(char **envp)
{
    size_t n = 0U;
    size_t w = 0U;
    char **out;
    while (envp != NULL && envp[n] != NULL) {
        n++;
    }
    out = (char **)malloc((n + 1U) * sizeof(char *));
    if (out == NULL) {
        return envp;
    }
    for (size_t r = 0U; r < n; r++) {
        if (is_exec_sig_key(envp[r])) {
            continue;
        }
        out[w++] = envp[r];
    }
    out[w] = NULL;
    return out;
}

/* Cross-RT execve class: a re-exec replaces the image, so the blocked
 * mask, ignored dispositions, and pending signals carried through the
 * environment are re-applied here — Linux preserves all three while
 * caught handlers revert to default. */
static void exec_signal_restore(a32_ctx_t *ctx)
{
    uint64_t keep = ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));
    uint64_t mask = getenv_hex("A32RT_EXEC_SIGMASK") & keep;
    uint64_t ign = getenv_hex("A32RT_EXEC_SIGIGN") & keep;
    uint64_t pend = getenv_hex("A32RT_EXEC_SIGPEND") & keep;
    /* The carried state has been consumed here: drop it first so a
     * later native child process never inherits stale execve-class
     * signal state, even when the carried values are all zero
     * (A32-R3-7 follow-up 5). */
    unsetenv("A32RT_EXEC_SIGMASK");
    unsetenv("A32RT_EXEC_SIGIGN");
    unsetenv("A32RT_EXEC_SIGPEND");
    if ((mask | ign | pend) == 0ULL) {
        return;
    }
    ctx->sig_mask = mask;
    ctx->pending_signals = pend;
    a32_signals_mark_ignored(ign);
    while (mask != 0ULL) {
        int s = __builtin_ctzll(mask) + 1;
        mask &= mask - 1ULL;
        a32_signals_ensure_host_handler(s);
    }
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s [--window <hex>] [--sysroot <path>] [--cache <dir>] [--execfn <path>] [--image-fd <fd>] [--no-host] [--stats] [--trace] [--aot] [--auto-aot] [--gdb <port>] [--interp | --dbt | --difftest] -- <PATH> [<ARGV0> [ARGS...]]\n"
            "  %s <PATH> [ARGS...]\n",
            prog, prog);
}

int main(int argc, char **argv, char **envp)
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    static a32_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
#if defined(__linux__)
    ctx.host_tid = (int)syscall(SYS_gettid);
#else
    ctx.host_tid = (int)getpid();
#endif
    a32_exec_mode_t exec_mode = A32_EXEC_DBT;

    uint64_t requested_window = UINT64_MAX;
    const char *env_sysroot = getenv("A32RT_SYSROOT");
    if (env_sysroot && env_sysroot[0] != '\0') {
        ctx.sysroot = env_sysroot;
    }
    if (getenv_bool("A32RT_STATS")) {
        ctx.stats_enabled = true;
    }
    if (getenv_bool("A32RT_TRACE")) {
        ctx.trace_enabled = true;
    }
    if (getenv_bool("A32RT_AOT")) {
        ctx.aot_mode = true;
    }
    if (getenv_bool("A32RT_AUTO_AOT")) {
        ctx.auto_aot = true;
    }
    if (getenv_bool("A32RT_SINGLE_STEP")) {
        ctx.single_step = true;
    }
    const char *env_cache = getenv("A32RT_CACHE");
    if (env_cache && env_cache[0] != '\0') {
        ctx.cache_dir = env_cache;
    }
    const char *env_gdb = getenv("A32RT_GDB");
    if (!env_gdb || env_gdb[0] == '\0') {
        env_gdb = getenv("A32RT_GDB_PORT");
    }
    (void)parse_port(env_gdb, &ctx.gdb_port);

    const char *exec_path = NULL;
    int         guest_argc = 0;
    char      **guest_argv = NULL;

    bool is_binfmt_p = ends_with(argv[0], "a32rt-binfmt-p") || getenv_bool("A32RT_BINFMT_P");

    int i = 1;
    bool explicit_sep = false;
    const char *execfn_cli = NULL;
    int image_fd_cli = -1;
    while (i < argc && argv[i][0] == '-') {
        if (strcmp(argv[i], "--") == 0) {
            explicit_sep = true;
            i++;
            break;
        } else if (strcmp(argv[i], "--window") == 0) {
            if (i + 1 >= argc || argv[i + 1][0] == '\0' || strchr(argv[i + 1], '-') != NULL) {
                usage(argv[0]);
                return 1;
            }
            char *endp = NULL;
            errno = 0;
            requested_window = (uint64_t)strtoull(argv[i + 1], &endp, 0);
            if (errno == ERANGE || !endp || endp == argv[i + 1] || *endp != '\0' ||
                requested_window == UINT64_MAX) {
                usage(argv[0]);
                return 1;
            }
            i += 2;
        } else if (strcmp(argv[i], "--sysroot") == 0 || strcmp(argv[i], "--rootfs") == 0) {
            if (i + 1 >= argc || argv[i + 1][0] == '\0') {
                usage(argv[0]);
                return 1;
            }
            ctx.sysroot = argv[i + 1];
            i += 2;
        } else if (strcmp(argv[i], "--execfn") == 0) {
            if (i + 1 >= argc || argv[i + 1][0] == '\0') {
                usage(argv[0]);
                return 1;
            }
            execfn_cli = argv[i + 1];
            i += 2;
        } else if (strcmp(argv[i], "--image-fd") == 0) {
            if (i + 1 >= argc || argv[i + 1][0] == '\0' || strchr(argv[i + 1], '-') != NULL) {
                usage(argv[0]);
                return 1;
            }
            char *endp = NULL;
            errno = 0;
            unsigned long fd_val = strtoul(argv[i + 1], &endp, 10);
            if (errno != 0 || !endp || endp == argv[i + 1] || *endp != '\0' ||
                fd_val > (unsigned long)INT_MAX) {
                usage(argv[0]);
                return 1;
            }
            image_fd_cli = (int)fd_val;
            i += 2;
        } else if (strcmp(argv[i], "--no-host") == 0) {
            setenv("A32RT_HOST", "none", 1);
            i++;
        } else if (strcmp(argv[i], "--cache") == 0) {
            if (i + 1 >= argc || argv[i + 1][0] == '\0') {
                usage(argv[0]);
                return 1;
            }
            ctx.cache_dir = argv[i + 1];
            i += 2;
        } else if (strcmp(argv[i], "--gdb") == 0) {
            if (i + 1 >= argc || !parse_port(argv[i + 1], &ctx.gdb_port)) {
                usage(argv[0]);
                return 1;
            }
            i += 2;
        } else if (strcmp(argv[i], "--aot") == 0) {
            ctx.aot_mode = true;
            i++;
        } else if (strcmp(argv[i], "--auto-aot") == 0) {
            ctx.auto_aot = true;
            i++;
        } else if (strcmp(argv[i], "--stats") == 0) {
            ctx.stats_enabled = true;
            i++;
        } else if (strcmp(argv[i], "--trace") == 0) {
            ctx.trace_enabled = true;
            i++;
        } else if (a32_difftest_parse_arg(argv[i], &exec_mode)) {
            i++;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else if (is_binfmt_p && i + 1 < argc) {
            break;
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    const char *raw_sysroot = ctx.sysroot;
    if (ctx.sysroot != NULL && ctx.sysroot[0] != '\0') {
        static char sysroot_abs[PATH_MAX];
        if (realpath(ctx.sysroot, sysroot_abs) != NULL) {
            ctx.sysroot = sysroot_abs;
        }
    }

    if ((execfn_cli == NULL && i >= argc) || (is_binfmt_p && i + 1 >= argc)) {
        usage(argv[0]);
        return 1;
    }

    int exec_fd = image_fd_cli;
#if defined(__linux__)
    if (exec_fd < 0) {
        errno = 0;
        unsigned long aux_execfd = getauxval(AT_EXECFD);
        if (errno == 0 && aux_execfd <= (unsigned long)INT_MAX) {
            exec_fd = (int)aux_execfd;
        }
    }
#endif

    if (execfn_cli != NULL) {
        exec_path = execfn_cli;
        guest_argc = argc - i;
        guest_argv = &argv[i];
    } else {
        exec_path = argv[i];
        if (is_binfmt_p || (explicit_sep && exec_fd < 0 && i + 1 < argc)) {
            guest_argc = argc - (i + 1);
            guest_argv = &argv[i + 1];
        } else {
            guest_argc = argc - i;
            guest_argv = &argv[i];
        }
    }

    bool exec_in_sysroot = false;
    if (ctx.sysroot != NULL && ctx.sysroot[0] != '\0') {
        const char *stripped = strip_sysroot_prefix(exec_path, ctx.sysroot);
        if (stripped == NULL && raw_sysroot != ctx.sysroot) {
            stripped = strip_sysroot_prefix(exec_path, raw_sysroot);
        }
        if (stripped != NULL) {
            exec_path = stripped;
            exec_in_sysroot = true;
        }
        while (exec_path[0] == '/' && exec_path[1] == '/') {
            exec_path++;
        }
        if (guest_argv == &argv[i]) {
            argv[i] = (char *)exec_path;
        }
    }
    if (exec_fd < 0 && !exec_in_sysroot && exec_path[0] == '/') {
        /* A32-R13-2: a host-absolute executable outside the sysroot is
         * operator input, so open it directly instead of forcing it
         * through the confined guest resolver (which has no host
         * fallback). Guest-derived paths below still resolve strictly
         * under the sysroot. */
        exec_fd = open(exec_path, O_RDONLY | O_CLOEXEC);
        if (exec_fd < 0) {
            fprintf(stderr, "a32rt: cannot open '%s': %s\n", exec_path, strerror(errno));
            return 1;
        }
    }

    ctx.exec_path = exec_path;
    ctx.is_ios    = a32_ios_is_macho_fd(exec_fd, exec_path, &ctx);

    a32_host_probe(&ctx);

    /* A32-R4-7: install host handlers BEFORE applying the captured
     * execve-class state so a32_signals_init() does not overwrite
     * restored SIG_IGN dispositions; consume the transport entries
     * BEFORE constructing the guest stack environment and BEFORE
     * loading Mach-O images so DYLD_*_LIBRARY_PATH from host_envp
     * is visible to the iOS loader. */
    a32_signals_set_thread_ctx(&ctx);
    if (a32_signals_init(&ctx) != 0) {
        return 1;
    }
    exec_signal_restore(&ctx);
    char **clean_envp = filter_exec_sig_env(envp);
    ctx.host_envp = clean_envp;

    ctx.difftest_mode = (exec_mode == A32_EXEC_DIFFTEST);
    uint64_t vm_t0 = 0U;
    if (ctx.stats_enabled) {
        vm_t0 = a32_host_monotonic_ns();
    }
    if (a32_vm_init(&ctx, requested_window) != 0) {
        free(ctx.ios_fn_starts);
        return 1;
    }

    /* Wave U3 A32-4: record the Two-Regime selection (A1 direct window on
     * H == 4096, A2 Soft-TLB arena above) for the reserved window. */
    if (a32_vm32_init(&ctx) != 0) {
        free(ctx.ios_fn_starts);
        return 1;
    }
    if (ctx.stats_enabled) {
        ctx.ns_vm_init = a32_host_monotonic_ns() - vm_t0;
    }

    a32_elf_info_t elf_info;
    if (ctx.is_ios) {
        if (a32_ios_load(&ctx, exec_fd, exec_path, &elf_info) != 0 ||
            a32_ios_commpage_setup(&ctx) != 0) {
            free(ctx.ios_fn_starts);
            return 1;
        }
    } else {
        if (a32_elf_load(&ctx, exec_fd, exec_path, &elf_info) != 0 ||
            a32_kuser_setup(&ctx) != 0) {
            return 1;
        }
    }

    uint32_t initial_sp = 0U;
    int stack_rc = ctx.is_ios
        ? a32_ios_stack_setup(&ctx, &elf_info, guest_argc, guest_argv, clean_envp, &initial_sp)
        : a32_stack_setup(&ctx, &elf_info, guest_argc, guest_argv, clean_envp, &initial_sp);
    if (stack_rc != 0) {
        free(ctx.ios_fn_starts);
        return 1;
    }

    if (a32_jit_init(&ctx) != 0) {
        free(ctx.ios_fn_starts);
        return 1;
    }

    if (ctx.aot_mode) {
        int arc = a32_aot_generate(&ctx, &elf_info, exec_path);
        free(ctx.ios_fn_starts);
        return arc;
    }
    ctx.aot_loaded = (a32_aot_load_cache(&ctx, exec_path) == 0);

    if (ctx.gdb_port > 0) {
        if (a32_gdb_init(&ctx, ctx.gdb_port) != 0) {
            ctx.gdb_enabled = false;
            ctx.single_step = false;
            fprintf(stderr, "a32rt: warning: failed to initialize GDB on port %d\n", ctx.gdb_port);
        }
    }

    ctx.regs[13] = initial_sp;
    a32_set_pc_interwork(&ctx, ctx.elf_entry);

    /* RC-U3: --difftest always runs DBT against the reference interpreter. */
    a32_difftest_set_dbt_step(a32_dbt_single_step);

    if (exec_mode == A32_EXEC_INTERP) {
        int irc = a32_difftest_run_interp(&ctx, 0ULL, NULL);
        if (ctx.stats_enabled) {
            a32_print_stats(&ctx);
        }
        free(ctx.ios_fn_starts);
        return irc;
    }
    if (exec_mode == A32_EXEC_DIFFTEST) {
        int drc = a32_difftest_run_compare(&ctx, 0ULL, NULL);
        if (ctx.stats_enabled) {
            a32_print_stats(&ctx);
        }
        free(ctx.ios_fn_starts);
        return drc;
    }
    int rc = a32_difftest_run_dbt(&ctx, NULL);
    if (ctx.stats_enabled) {
        a32_print_stats(&ctx);
    }
    if (ctx.auto_aot && !ctx.aot_loaded && ctx.blocks_compiled > 0U) {
        (void)a32_aot_save_cache(&ctx, exec_path);
    }
    free(ctx.ios_fn_starts);
    return rc;
}
