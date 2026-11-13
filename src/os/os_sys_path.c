/*
 * src/os/os_sys_path.c - Path resolution and virtual /proc synthesis
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "os/sys_priv.h"

dir_cookie_entry_t g_dir_cookies[MAX_DIR_COOKIES];
size_t             g_dir_cookie_count = 0U;

int64_t map_dir_cookie(int fd, uint64_t host_off)
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

bool unmap_dir_cookie(int fd, int64_t guest_off, off_t *out_host_off)
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

size_t sysroot_prefix_len(const a32_ctx_t *ctx)
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
    /* A32-R13-3: only /tmp may escape the sysroot. /proc, /sys and /dev
     * always resolve under sysroot_clean when a sysroot is set, so guest
     * /proc, /sys and /dev paths can never name raw host paths. */
    if (strcmp(vpath, "/tmp") == 0) {
        return true;
    }
    if (strncmp(vpath, "/tmp/", 5) == 0) {
        char sr_cand[PATH_MAX];
        int n = snprintf(sr_cand, sizeof(sr_cand), "%s%s", sysroot_clean, vpath);
        struct stat st;
        if (n > 0 && (size_t)n < sizeof(sr_cand)) {
            if (lstat(sr_cand, &st) == 0) {
                return false;
            }
            if (lstat(vpath, &st) != 0) {
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

int resolve_guest_path_str_at(const a32_ctx_t *ctx, int dirfd, const char *guest_path,
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

int read_guest_cstr(const a32_ctx_t *ctx, uint32_t guest_addr,
                    char *out, size_t max_len)
{
    if (!ctx || guest_addr < 0x1000U || guest_addr >= 0xFFFF0000U || !out || max_len == 0U) {
        return -EFAULT;
    }
    size_t pos = 0U;
    while (pos < max_len) {
        uint32_t va = guest_addr + (uint32_t)pos;
        if (va < guest_addr || va >= 0xFFFF0000U) {
            return -EFAULT;
        }
        uint32_t page_rem = A32_PAGE_SIZE - (va & (A32_PAGE_SIZE - 1U));
        size_t chunk = max_len - pos;
        if (chunk > (size_t)page_rem) {
            chunk = (size_t)page_rem;
        }
        if (!a32_guest_range_ok((a32_ctx_t *)ctx, va, (uint32_t)chunk, PROT_READ)) {
            return -EFAULT;
        }
        const char *p = (const char *)a32_guest_ptr(ctx, va);
        const void *nul = memchr(p, '\0', chunk);
        if (nul != NULL) {
            size_t copy_len = (size_t)((const char *)nul - p) + 1U;
            memcpy(out + pos, p, copy_len);
            return 0;
        }
        memcpy(out + pos, p, chunk);
        pos += chunk;
    }
    return -ENAMETOOLONG;
}

int resolve_guest_path_at(const a32_ctx_t *ctx, int dirfd, uint32_t guest_addr,
                          char *buf, size_t size, bool follow_final)
{
    char gpath[PATH_MAX];
    if (!buf || size == 0U) {
        return -EFAULT;
    }
    int rc = read_guest_cstr(ctx, guest_addr, gpath, sizeof(gpath));
    if (rc != 0) {
        return rc;
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

static int clean_guest_vpath(const char *path, char *out, size_t size);

bool is_proc_self_exe(const char *gpath, const char *resolved)
{
    const char *leaf = skip_proc_prefix(gpath);
    if (!leaf) {
        leaf = skip_proc_prefix(resolved);
    }
    if (!leaf || strncmp(leaf, "self/", 5) != 0) {
        /* Canonical spellings (A32-R13-3): /proc/./self/exe, //proc//... */
        char clean[PATH_MAX];
        if (gpath != NULL && clean_guest_vpath(gpath, clean, sizeof(clean)) == 0 &&
            strcmp(clean, "/proc/self/exe") == 0) {
            return true;
        }
        if (resolved != NULL &&
            clean_guest_vpath(resolved, clean, sizeof(clean)) == 0 &&
            strcmp(clean, "/proc/self/exe") == 0) {
            return true;
        }
        return false;
    }
    const char *sub = leaf + 5;
    while (*sub == '/') {
        sub++;
    }
    return strcmp(sub, "exe") == 0;
}

const char *get_guest_exe_path(const a32_ctx_t *ctx)
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

/* Canonicalize an absolute guest path for host-special checks (A32-R13-3):
 * collapse runs of '/', drop '/.', pop for '/..'. Relative paths and
 * truncation fail. */
static int clean_guest_vpath(const char *path, char *out, size_t size)
{
    size_t len = 0U;
    size_t r = 0U;
    size_t w = 0U;
    if (path == NULL || path[0] != '/' || out == NULL || size == 0U) {
        return -1;
    }
    while (len < PATH_MAX && path[len] != '\0') {
        len++;
    }
    while (r < len) {
        size_t start;
        size_t clen;
        while (r < len && path[r] == '/') {
            r++;
        }
        if (r >= len) {
            break;
        }
        start = r;
        while (r < len && path[r] != '/') {
            r++;
        }
        clen = r - start;
        if (clen == 1U && path[start] == '.') {
            continue;
        }
        if (clen == 2U && path[start] == '.' && path[start + 1U] == '.') {
            while (w > 0U && out[w - 1U] != '/') {
                w--;
            }
            if (w > 1U) {
                w--;
            }
            continue;
        }
        if (w + 1U + clen >= size) {
            return -1;
        }
        out[w] = '/';
        memcpy(out + w + 1U, path + start, clen);
        w += 1U + clen;
    }
    if (w == 0U) {
        out[w++] = '/';
    }
    out[w] = '\0';
    return 0;
}

/* True when a resolved host path sits under the explicit sysroot. */
static bool under_sysroot_clean(const a32_ctx_t *ctx, const char *resolved)
{
    size_t sr_len = sysroot_prefix_len(ctx);
    return sr_len > 0U && strncmp(resolved, ctx->sysroot, sr_len) == 0 &&
        (resolved[sr_len] == '/' || resolved[sr_len] == '\0');
}

static bool special_host_prefix(const char *clean)
{
    return strcmp(clean, "/proc") == 0 || strncmp(clean, "/proc/", 6) == 0 ||
        strcmp(clean, "/sys") == 0 || strncmp(clean, "/sys/", 5) == 0;
}

int a32_sys_path_denied(const a32_ctx_t *ctx, const char *resolved)
{
    /* A32-R13-3: a resolved host path naming /proc or /sys outside the
     * explicit sysroot must never reach the host. */
    char clean[PATH_MAX];
    if (resolved == NULL || resolved[0] == '\0') {
        return 0;
    }
    if (under_sysroot_clean(ctx, resolved)) {
        return 0;
    }
    if (clean_guest_vpath(resolved, clean, sizeof(clean)) != 0) {
        return 0;
    }
    return special_host_prefix(clean) ? -ENOENT : 0;
}

int try_open_virtual_proc(a32_ctx_t *ctx, const char *gpath, const char *resolved)
{
    /* Match synthesized entries on the canonical guest path (A32-R13-3)
     * so spellings like /proc/./self/maps cannot slip to the host, and
     * never hand raw host /proc or /sys to the guest. */
    char clean[PATH_MAX];
    if (clean_guest_vpath(gpath, clean, sizeof(clean)) == 0) {
        if (strcmp(clean, "/proc/cpuinfo") == 0) {
            return create_proc_cpuinfo();
        }
        if (strncmp(clean, "/proc/self/", 11) == 0) {
            const char *sub = clean + 11;
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
        if (special_host_prefix(clean)) {
            /* Unknown /proc (or bare /proc, /sys): only an explicit
             * sysroot file may back it; otherwise there is no file. */
            return under_sysroot_clean(ctx, resolved) ? VIRTUAL_PROC_NOT_MATCHED : -ENOENT;
        }
        return VIRTUAL_PROC_NOT_MATCHED;
    }
    /* Legacy relative forms (e.g. openat into /proc/self). */
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
