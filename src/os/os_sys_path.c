/*
 * src/os/os_sys_path.c - split-TU part 2/7 of src/syscalls.c (lines 540-1106).
 * Compiled only via the src/syscalls.c wrapper; do not add to Makefile SRCS.
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

static int64_t map_dir_cookie(int fd, uint64_t host_off)
{
    if (host_off < (uint64_t)DIR_COOKIE_BASE) {
        return (int64_t)host_off;
    }
    for (size_t i = 0U; i < g_dir_cookie_count; i++) {
        if (g_dir_cookies[i].fd == fd && g_dir_cookies[i].off64 == host_off) {
            return DIR_COOKIE_BASE + (int64_t)i;
        }
    }
    size_t idx;
    if (g_dir_cookie_count < MAX_DIR_COOKIES) {
        idx = g_dir_cookie_count++;
    } else {
        idx = (size_t)(host_off % MAX_DIR_COOKIES);
    }
    g_dir_cookies[idx].fd    = fd;
    g_dir_cookies[idx].off64 = host_off;
    return DIR_COOKIE_BASE + (int64_t)idx;
}

static bool unmap_dir_cookie(int fd, int64_t guest_off, off_t *out_host_off)
{
    if (guest_off >= DIR_COOKIE_BASE &&
        (uint64_t)(guest_off - DIR_COOKIE_BASE) < g_dir_cookie_count) {
        size_t idx = (size_t)(guest_off - DIR_COOKIE_BASE);
        if (g_dir_cookies[idx].fd == fd) {
            *out_host_off = (off_t)g_dir_cookies[idx].off64;
            return true;
        }
    }
    return false;
}

static uint32_t host_ret(ssize_t r)
{
    if (r < 0) {
        return (uint32_t)(-errno);
    }
    return (uint32_t)r;
}

static inline uint64_t u64_from_u32(uint32_t lo, uint32_t hi)
{
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static int translate_at_fd(uint32_t guest_dfd)
{
    if (guest_dfd == 0xFFFFFF9CU || guest_dfd == (uint32_t)AT_FDCWD) {
        return AT_FDCWD;
    }
    return (int)(int32_t)guest_dfd;
}

static int translate_open_flags(uint32_t guest_flags)
{
#if defined(__APPLE__)
    int f = (int)(guest_flags & 3U);
    if (guest_flags & 0x00000040U) f |= O_CREAT;
    if (guest_flags & 0x00000080U) f |= O_EXCL;
    if (guest_flags & 0x00000100U) f |= O_NOCTTY;
    if (guest_flags & 0x00000200U) f |= O_TRUNC;
    if (guest_flags & 0x00000400U) f |= O_APPEND;
    if (guest_flags & 0x00000800U) f |= O_NONBLOCK;
    if (guest_flags & 0x00001000U) f |= O_DSYNC;
    if (guest_flags & (0x00004000U | 0x00010000U)) f |= O_DIRECTORY;
    if (guest_flags & (0x00008000U | 0x00020000U)) f |= O_NOFOLLOW;
    if (guest_flags & 0x00080000U) f |= O_CLOEXEC;
    return f;
#else
    return (int)guest_flags;
#endif
}

static inline uint32_t id16_to_32(uint32_t id)
{
    return ((id & 0xFFFFU) == 0xFFFFU) ? (uint32_t)-1 : (id & 0xFFFFU);
}

static inline int get_host_tid(a32_ctx_t *ctx)
{
    if (ctx->host_tid == 0) {
#if defined(__linux__)
        ctx->host_tid = (int)syscall(SYS_gettid);
#elif defined(__APPLE__)
        uint64_t tid64 = 0ULL;
        (void)pthread_threadid_np(NULL, &tid64);
        ctx->host_tid = (tid64 != 0ULL) ? (int)(tid64 & 0x3FFFFFFFULL) : (int)getpid();
#else
        ctx->host_tid = (int)getpid();
#endif
    }
    return ctx->host_tid;
}

static size_t sysroot_prefix_len(const a32_ctx_t *ctx)
{
    if (!ctx || !ctx->sysroot || ctx->sysroot[0] == '\0') {
        return 0U;
    }
    size_t len = strlen(ctx->sysroot);
    while (len > 1U && ctx->sysroot[len - 1U] == '/') {
        len--;
    }
    return (len == 1U && ctx->sysroot[0] == '/') ? 0U : len;
}

static void vpath_pop_dir(char *vpath)
{
    char *last_slash = strrchr(vpath, '/');
    if (!last_slash || last_slash == vpath) {
        vpath[0] = '/';
        vpath[1] = '\0';
    } else {
        *last_slash = '\0';
    }
}

static bool is_passthrough_vpath(const char *sysroot_clean, const char *vpath)
{
    if (strcmp(vpath, "/tmp") == 0       ||
        strncmp(vpath, "/proc/", 6) == 0 ||
        strncmp(vpath, "/sys/", 5) == 0  ||
        strncmp(vpath, "/dev/", 5) == 0) {
        return true;
    }
    if (strcmp(vpath, "/proc") == 0 ||
        strcmp(vpath, "/sys") == 0  ||
        strcmp(vpath, "/dev") == 0  ||
        strncmp(vpath, "/tmp/", 5) == 0) {
        char sr_cand[PATH_MAX];
        int n = snprintf(sr_cand, sizeof(sr_cand), "%s%s", sysroot_clean, vpath);
        struct stat st;
        if (n > 0 && (size_t)n < sizeof(sr_cand)) {
            if (lstat(sr_cand, &st) == 0) {
                return false;
            }
            if (strncmp(vpath, "/tmp/", 5) == 0 && lstat(vpath, &st) != 0) {
                char *slash = strrchr(sr_cand, '/');
                if (slash != NULL && (size_t)(slash - sr_cand) > strlen(sysroot_clean) + 4U) {
                    *slash = '\0';
                    if (lstat(sr_cand, &st) == 0) {
                        return false;
                    }
                }
            }
        }
        return true;
    }
    return false;
}

static int resolve_guest_path_str_at(const a32_ctx_t *ctx, int dirfd, const char *guest_path,
                                     char *buf, size_t size, bool follow_final)
{
    if (!ctx || !guest_path || !buf || size == 0U) {
        return -1;
    }
    if (guest_path[0] == '\0') {
        buf[0] = '\0';
        return 0;
    }
    size_t sr_len = sysroot_prefix_len(ctx);
    if (sr_len == 0U) {
        int n = snprintf(buf, size, "%s", guest_path);
        return (n >= 0 && (size_t)n < size) ? 0 : -1;
    }
    if (sr_len >= PATH_MAX) {
        return -1;
    }
    char sysroot_clean[PATH_MAX];
    memcpy(sysroot_clean, ctx->sysroot, sr_len);
    sysroot_clean[sr_len] = '\0';

    char vpath[PATH_MAX];
    char rem[PATH_MAX];
    bool in_sysroot = false;

    if (guest_path[0] == '/') {
        if (strncmp(guest_path, sysroot_clean, sr_len) == 0 &&
            (guest_path[sr_len] == '/' || guest_path[sr_len] == '\0')) {
            guest_path = (guest_path[sr_len] == '\0') ? "/" : (guest_path + sr_len);
        }
        in_sysroot = true;
        vpath[0]   = '/';
        vpath[1]   = '\0';
        int n = snprintf(rem, sizeof(rem), "%s", guest_path);
        if (n < 0 || (size_t)n >= sizeof(rem)) {
            return -1;
        }
    } else {
        char base_dir[PATH_MAX];
        base_dir[0] = '\0';
        if (dirfd == AT_FDCWD) {
            if (getcwd(base_dir, sizeof(base_dir)) == NULL) {
                base_dir[0] = '\0';
            }
        } else if (dirfd >= 0) {
            char fd_link[64];
            snprintf(fd_link, sizeof(fd_link), "/proc/self/fd/%d", dirfd);
            ssize_t ln = readlink(fd_link, base_dir, sizeof(base_dir) - 1U);
            if (ln > 0) {
                base_dir[ln] = '\0';
            }
        }

        if (base_dir[0] != '\0' && strncmp(base_dir, sysroot_clean, sr_len) == 0 &&
            (base_dir[sr_len] == '/' || base_dir[sr_len] == '\0')) {
            in_sysroot = true;
            const char *vbase = (base_dir[sr_len] == '\0') ? "/" : (base_dir + sr_len);
            snprintf(vpath, sizeof(vpath), "%s", vbase);
            int n = snprintf(rem, sizeof(rem), "%s", guest_path);
            if (n < 0 || (size_t)n >= sizeof(rem)) {
                return -1;
            }
        } else if (strcmp(base_dir, "/tmp") == 0 || strncmp(base_dir, "/tmp/", 5) == 0 ||
                   strcmp(base_dir, "/private/tmp") == 0 || strncmp(base_dir, "/private/tmp/", 13) == 0) {
            in_sysroot = true;
            const char *tbase = (strncmp(base_dir, "/private/tmp", 12) == 0) ? (base_dir + 8) : base_dir;
            snprintf(vpath, sizeof(vpath), "%s", tbase);
            int n = snprintf(rem, sizeof(rem), "%s", guest_path);
            if (n < 0 || (size_t)n >= sizeof(rem)) {
                return -1;
            }
        } else {
            /* dirfd is outside sysroot; check if relative path (or intermediate component) is a symlink into sysroot */
            char rel_cur[PATH_MAX];
            int nr = snprintf(rel_cur, sizeof(rel_cur), "%s", guest_path);
            if (nr < 0 || (size_t)nr >= sizeof(rel_cur)) {
                return -1;
            }
            for (int depth = 0; depth < 8; depth++) {
                /* Check intermediate prefixes and (if follow_final) final path */
                bool hopped = false;
                size_t rlen = strlen(rel_cur);
                size_t scan = 0U;
                while (scan < rlen) {
                    while (scan < rlen && rel_cur[scan] == '/') {
                        scan++;
                    }
                    if (scan >= rlen) {
                        break;
                    }
                    while (scan < rlen && rel_cur[scan] != '/') {
                        scan++;
                    }
                    size_t after_pos = scan;
                    while (after_pos < rlen && rel_cur[after_pos] == '/') {
                        after_pos++;
                    }
                    bool is_last_comp = (after_pos >= rlen);
                    if (is_last_comp && !follow_final) {
                        break;
                    }
                    char prefix[PATH_MAX];
                    if (scan >= sizeof(prefix)) {
                        break;
                    }
                    memcpy(prefix, rel_cur, scan);
                    prefix[scan] = '\0';

                    char link_target[PATH_MAX];
                    ssize_t ln = readlinkat(dirfd, prefix, link_target, sizeof(link_target) - 1U);
                    if (ln > 0) {
                        link_target[ln] = '\0';
                        const char *rest = rel_cur + after_pos;
                        if (link_target[0] == '/') {
                            in_sysroot = true;
                            vpath[0]   = '/';
                            vpath[1]   = '\0';
                            int nm = (*rest != '\0')
                                     ? snprintf(rem, sizeof(rem), "%s/%s", link_target, rest)
                                     : snprintf(rem, sizeof(rem), "%s", link_target);
                            if (nm < 0 || (size_t)nm >= sizeof(rem)) {
                                return -1;
                            }
                        } else {
                            char *last_sl = strrchr(prefix, '/');
                            char next_rel[PATH_MAX];
                            int nm;
                            if (last_sl != NULL) {
                                *last_sl = '\0';
                                nm = (*rest != '\0')
                                     ? snprintf(next_rel, sizeof(next_rel), "%s/%s/%s", prefix, link_target, rest)
                                     : snprintf(next_rel, sizeof(next_rel), "%s/%s", prefix, link_target);
                            } else {
                                nm = (*rest != '\0')
                                     ? snprintf(next_rel, sizeof(next_rel), "%s/%s", link_target, rest)
                                     : snprintf(next_rel, sizeof(next_rel), "%s", link_target);
                            }
                            if (nm > 0 && (size_t)nm < sizeof(next_rel)) {
                                snprintf(rel_cur, sizeof(rel_cur), "%s", next_rel);
                                hopped = true;
                            }
                        }
                        break;
                    }
                }
                if (in_sysroot || !hopped) {
                    break;
                }
            }
            if (!in_sysroot) {
                int n = snprintf(buf, size, "%s", rel_cur);
                return (n >= 0 && (size_t)n < size) ? 0 : -1;
            }
        }
    }

    const char *p = rem;
    int symlink_loops = 0;
    while (*p != '\0') {
        while (*p == '/') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        const char *slash = strchr(p, '/');
        size_t comp_len = slash ? (size_t)(slash - p) : strlen(p);
        const char *after = slash ? slash : (p + comp_len);
        while (*after == '/') {
            after++;
        }
        bool is_last = (*after == '\0');

        if (comp_len == 1U && p[0] == '.') {
            p = after;
            continue;
        }
        if (comp_len == 2U && p[0] == '.' && p[1] == '.') {
            if (strcmp(vpath, "/") != 0) {
                vpath_pop_dir(vpath);
            }
            p = after;
            continue;
        }

        size_t vlen = strlen(vpath);
        if (vlen + 1U + comp_len + 1U > sizeof(vpath)) {
            return -1;
        }
        if (strcmp(vpath, "/") == 0) {
            memcpy(vpath + 1U, p, comp_len);
            vpath[1U + comp_len] = '\0';
        } else {
            vpath[vlen] = '/';
            memcpy(vpath + vlen + 1U, p, comp_len);
            vpath[vlen + 1U + comp_len] = '\0';
        }

        if (strncmp(vpath, "/proc/", 6) == 0 ||
            strncmp(vpath, "/sys/", 5) == 0  ||
            strncmp(vpath, "/dev/", 5) == 0) {
            int np = is_last ? snprintf(buf, size, "%s", vpath)
                             : snprintf(buf, size, "%s/%s", vpath, after);
            return (np >= 0 && (size_t)np < size) ? 0 : -1;
        }

        if ((!is_last || follow_final) && strcmp(vpath, "/tmp") != 0) {
            char host_cand[PATH_MAX];
            int nh = is_passthrough_vpath(sysroot_clean, vpath)
                     ? snprintf(host_cand, sizeof(host_cand), "%s", vpath)
                     : snprintf(host_cand, sizeof(host_cand), "%s%s", sysroot_clean, vpath);
            if (nh > 0 && (size_t)nh < sizeof(host_cand)) {
                char link_target[PATH_MAX];
                ssize_t ln = readlink(host_cand, link_target, sizeof(link_target) - 1U);
                if (ln > 0 && ++symlink_loops <= 32) {
                    link_target[ln] = '\0';
                    char next_rem[PATH_MAX];
                    int nr = is_last ? snprintf(next_rem, sizeof(next_rem), "%s", link_target)
                                     : snprintf(next_rem, sizeof(next_rem), "%s/%s", link_target, after);
                    if (nr < 0 || (size_t)nr >= sizeof(next_rem)) {
                        return -1;
                    }
                    if (link_target[0] == '/') {
                        vpath[0] = '/';
                        vpath[1] = '\0';
                    } else {
                        vpath_pop_dir(vpath);
                    }
                    snprintf(rem, sizeof(rem), "%s", next_rem);
                    p = rem;
                    continue;
                }
            }
        }
        p = after;
    }

    if (is_passthrough_vpath(sysroot_clean, vpath)) {
        int np = snprintf(buf, size, "%s", vpath);
        return (np >= 0 && (size_t)np < size) ? 0 : -1;
    }
    int nf = (strcmp(vpath, "/") == 0)
             ? snprintf(buf, size, "%s", sysroot_clean)
             : snprintf(buf, size, "%s%s", sysroot_clean, vpath);
    return (nf >= 0 && (size_t)nf < size) ? 0 : -1;
}

int a32_resolve_guest_path(const a32_ctx_t *ctx, const char *guest_path,
                           char *buf, size_t size)
{
    return resolve_guest_path_str_at(ctx, AT_FDCWD, guest_path, buf, size, true);
}

static int resolve_guest_path_at(const a32_ctx_t *ctx, int dirfd, uint32_t guest_addr,
                                 char *buf, size_t size, bool follow_final)
{
    if (!ctx || guest_addr < 0x1000U || guest_addr >= 0xFFFF0000U || !buf || size == 0U ||
        !a32_guest_range_ok((a32_ctx_t *)ctx, guest_addr, 1U, PROT_READ)) {
        return -EFAULT;
    }
    const char *gpath = (const char *)a32_guest_ptr(ctx, guest_addr);
    if (strnlen(gpath, PATH_MAX) >= PATH_MAX) {
        return -ENAMETOOLONG;
    }
    if (resolve_guest_path_str_at(ctx, dirfd, gpath, buf, size, follow_final) < 0) {
        return -ENAMETOOLONG;
    }
    return 0;
}

static int create_anon_file(const char *name, const void *data, size_t len)
{
    int fd = -1;
#if defined(__linux__)
    fd = (int)syscall(SYS_memfd_create, name, 0x0001U /* MFD_CLOEXEC */);
#else
    (void)name;
#endif
    if (fd < 0) {
        char tmpl[] = "/tmp/a32rt_proc_XXXXXX";
        fd = mkstemp(tmpl);
        if (fd >= 0) {
            unlink(tmpl);
        }
    }
    if (fd >= 0 && data && len > 0) {
        ssize_t written = write(fd, data, len);
        (void)written;
        lseek(fd, 0, SEEK_SET);
    }
    return fd;
}

static int create_proc_maps(const a32_ctx_t *ctx)
{
    a32_vm_sync_ctx((a32_ctx_t *)ctx);
    char buf[16384];
    size_t off = 0;

    for (size_t i = 0; i < ctx->region_count && off + 256 < sizeof(buf); i++) {
        const a32_region_t *r = &ctx->regions[i];
        char prot_r = (r->prot & PROT_READ) ? 'r' : '-';
        char prot_w = (r->prot & PROT_WRITE) ? 'w' : '-';
        char prot_x = (r->prot & PROT_EXEC) ? 'x' : '-';
        char prot_s = (r->flags & 0x01) ? 's' : 'p';
        int n = snprintf(buf + off, sizeof(buf) - off,
                         "%08x-%08x %c%c%c%c 00000000 00:00 0 %s%s\n",
                         r->start, r->end, prot_r, prot_w, prot_x, prot_s,
                         (r->name[0] && r->name[0] != '[') ? "         " : " ",
                         r->name[0] ? r->name : "");
        if (n > 0) {
            off += (size_t)n;
        }
    }
    return create_anon_file("proc_maps", buf, off);
}

static int create_proc_cmdline(const a32_ctx_t *ctx)
{
    char buf[4096];
    size_t off = 0;
    if (ctx->guest_argv) {
        for (int i = 0; i < ctx->guest_argc && off + 256 < sizeof(buf); i++) {
            if (ctx->guest_argv[i]) {
                size_t len = strlen(ctx->guest_argv[i]) + 1;
                if (off + len < sizeof(buf)) {
                    memcpy(buf + off, ctx->guest_argv[i], len);
                    off += len;
                }
            }
        }
    }
    return create_anon_file("proc_cmdline", buf, off);
}

static int create_proc_auxv(const a32_ctx_t *ctx)
{
    return create_anon_file("a32rt-auxv", ctx->auxv_data, ctx->auxv_bytes);
}

static int create_proc_cpuinfo(void)
{
    static const char cpuinfo_str[] =
        "processor\t: 0\n"
        "model name\t: ARMv8 Processor rev 0 (v8l)\n"
        "BogoMIPS\t: 48.00\n"
        "Features\t: half thumb fastmult vfp edsp neon vfpv3 tls vfpv4 idiva idivt vfpd32 lpae evtstrm aes pmull sha1 sha2 crc32\n"
        "CPU implementer\t: 0x41\n"
        "CPU architecture: 8\n"
        "CPU variant\t: 0x0\n"
        "CPU part\t: 0xd08\n"
        "CPU revision\t: 0\n\n";
    return create_anon_file("proc_cpuinfo", cpuinfo_str, sizeof(cpuinfo_str) - 1U);
}

static const char *skip_proc_prefix(const char *path)
{
    if (!path || path[0] != '/') {
        return NULL;
    }
    const char *p = path;
    while (*p == '/') {
        p++;
    }
    if (strncmp(p, "proc/", 5) != 0) {
        return NULL;
    }
    p += 5;
    while (*p == '/') {
        p++;
    }
    return p;
}

static bool is_proc_self_exe(const char *gpath, const char *resolved)
{
    const char *leaf = skip_proc_prefix(gpath);
    if (!leaf) {
        leaf = skip_proc_prefix(resolved);
    }
    if (!leaf || strncmp(leaf, "self/", 5) != 0) {
        return false;
    }
    const char *sub = leaf + 5;
    while (*sub == '/') {
        sub++;
    }
    return strcmp(sub, "exe") == 0;
}

static const char *get_guest_exe_path(const a32_ctx_t *ctx)
{
    const char *ep = (ctx->exec_path && ctx->exec_path[0])
                     ? ctx->exec_path
                     : (ctx->guest_argv ? ctx->guest_argv[0] : NULL);
    if (!ep) {
        ep = "";
    }
    size_t sr_len = sysroot_prefix_len(ctx);
    if (sr_len > 0U && strncmp(ep, ctx->sysroot, sr_len) == 0 && ep[sr_len] == '/') {
        ep += sr_len;
    }
    return ep;
}

#define VIRTUAL_PROC_NOT_MATCHED (-9999)

static int try_open_virtual_proc(a32_ctx_t *ctx, const char *gpath, const char *resolved)
{
    const char *leaf = skip_proc_prefix(gpath);
    if (!leaf) {
        leaf = skip_proc_prefix(resolved);
    }
    if (!leaf) {
        return VIRTUAL_PROC_NOT_MATCHED;
    }
    if (strcmp(leaf, "cpuinfo") == 0) {
        return create_proc_cpuinfo();
    }
    if (strncmp(leaf, "self/", 5) == 0) {
        const char *sub = leaf + 5;
        while (*sub == '/') {
            sub++;
        }
        if (strcmp(sub, "maps") == 0) {
            return create_proc_maps(ctx);
        }
        if (strcmp(sub, "cmdline") == 0) {
            return create_proc_cmdline(ctx);
        }
        if (strcmp(sub, "auxv") == 0) {
            return create_proc_auxv(ctx);
        }
        if (strcmp(sub, "exe") == 0) {
            const char *ep = get_guest_exe_path(ctx);
            char exe_res[PATH_MAX];
            if (a32_resolve_guest_path(ctx, ep, exe_res, sizeof(exe_res)) < 0) {
                return -ENOENT;
            }
            int fd = open(exe_res, O_RDONLY | O_CLOEXEC);
            return (fd >= 0) ? fd : -errno;
        }
    }
    return VIRTUAL_PROC_NOT_MATCHED;
}
