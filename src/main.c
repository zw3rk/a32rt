/*
 * src/main.c — CLI parsing, AT_EXECFD, host probing, and startup orchestration
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#define _GNU_SOURCE
#include "a32rt.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/syscall.h>
#include <unistd.h>

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

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s [--window <hex>] [--sysroot <path>] [--cache <dir>] [--stats] [--trace] [--aot] [--gdb <port>] -- <PATH> <ARGV0> [ARGS...]\n"
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
    ctx.host_tid = (int)syscall(SYS_gettid);

    uint64_t requested_window = UINT64_MAX;
    const char *env_sysroot = getenv("A32RT_SYSROOT");
    if (env_sysroot && env_sysroot[0] != '\0') {
        ctx.sysroot = env_sysroot;
    }
    const char *env_stats = getenv("A32RT_STATS");
    if (env_stats && strcmp(env_stats, "1") == 0) {
        ctx.stats_enabled = true;
    }
    const char *env_trace = getenv("A32RT_TRACE");
    if (env_trace && strcmp(env_trace, "1") == 0) {
        ctx.trace_enabled = true;
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
    const char *env_aot = getenv("A32RT_AOT");
    if (env_aot && strcmp(env_aot, "1") == 0) {
        ctx.aot_mode = true;
    }

    const char *exec_path = NULL;
    int         guest_argc = 0;
    char      **guest_argv = NULL;

    bool is_binfmt_p = ends_with(argv[0], "a32rt-binfmt-p");
    const char *env_binfmt_p = getenv("A32RT_BINFMT_P");
    if (env_binfmt_p && strcmp(env_binfmt_p, "1") == 0) {
        is_binfmt_p = true;
    }

    int i = 1;
    bool explicit_sep = false;
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
        } else if (strcmp(argv[i], "--sysroot") == 0) {
            if (i + 1 >= argc || argv[i + 1][0] == '\0') {
                usage(argv[0]);
                return 1;
            }
            ctx.sysroot = argv[i + 1];
            i += 2;
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
        } else if (strcmp(argv[i], "--stats") == 0) {
            ctx.stats_enabled = true;
            i++;
        } else if (strcmp(argv[i], "--trace") == 0) {
            ctx.trace_enabled = true;
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
        static char sysroot_abs[4096];
        if (realpath(ctx.sysroot, sysroot_abs) != NULL) {
            ctx.sysroot = sysroot_abs;
        }
    }

    if (explicit_sep) {
        if (i >= argc) {
            usage(argv[0]);
            return 1;
        }
        exec_path = argv[i];
        if (i + 1 < argc) {
            guest_argc = argc - (i + 1);
            guest_argv = &argv[i + 1];
        } else {
            guest_argc = 1;
            guest_argv = &argv[i];
        }
    } else if (is_binfmt_p) {
        if (i + 1 >= argc) {
            usage(argv[0]);
            return 1;
        }
        exec_path  = argv[i];
        guest_argc = argc - (i + 1);
        guest_argv = &argv[i + 1];
    } else {
        if (i >= argc) {
            usage(argv[0]);
            return 1;
        }
        exec_path  = argv[i];
        guest_argc = argc - i;
        guest_argv = &argv[i];
    }

    if (ctx.sysroot != NULL && ctx.sysroot[0] != '\0') {
        const char *stripped = strip_sysroot_prefix(exec_path, ctx.sysroot);
        if (stripped == NULL && raw_sysroot != ctx.sysroot) {
            stripped = strip_sysroot_prefix(exec_path, raw_sysroot);
        }
        if (stripped != NULL) {
            exec_path = stripped;
        }
        while (exec_path[0] == '/' && exec_path[1] == '/') {
            exec_path++;
        }
    }

    ctx.exec_path = exec_path;

    a32_host_probe(&ctx);

    if (a32_vm_init(&ctx, requested_window) != 0) {
        return 1;
    }

    int exec_fd = -1;
    unsigned long aux_execfd = getauxval(AT_EXECFD);
    if (aux_execfd != 0UL && aux_execfd <= (unsigned long)INT_MAX) {
        exec_fd = (int)aux_execfd;
    }

    a32_elf_info_t elf_info;
    if (a32_elf_load(&ctx, exec_fd, exec_path, &elf_info) != 0) {
        return 1;
    }

    if (a32_kuser_setup(&ctx) != 0) {
        return 1;
    }

    uint32_t initial_sp = 0U;
    if (a32_stack_setup(&ctx, &elf_info, guest_argc, guest_argv, envp, &initial_sp) != 0) {
        return 1;
    }

    a32_signals_set_thread_ctx(&ctx);
    if (a32_signals_init(&ctx) != 0 || a32_jit_init(&ctx) != 0) {
        return 1;
    }

    if (ctx.aot_mode) {
        return a32_aot_generate(&ctx, &elf_info, exec_path);
    }
    a32_aot_load_cache(&ctx, exec_path);

    if (ctx.gdb_port > 0) {
        if (a32_gdb_init(&ctx, ctx.gdb_port) != 0) {
            ctx.gdb_enabled = false;
            ctx.single_step = false;
            fprintf(stderr, "a32rt: warning: failed to initialize GDB on port %d\n", ctx.gdb_port);
        }
    }

    ctx.regs[13] = initial_sp;
    a32_set_pc_interwork(&ctx, ctx.elf_entry);

    return a32_jit_run(&ctx);
}
