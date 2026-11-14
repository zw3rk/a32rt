/*
 * src/ios/ios_svc_bsd_sock.c — 32-bit iOS XNU BSD sockets + VM advice
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Curated C11, host-portable (macOS arm64 AND aarch64-linux-musl).
 * BSD sockets (+ SCM_RIGHTS marshaling) and the munmap / mprotect /
 * madvise / mincore / msync VM helpers; kqueue lives in
 * ios_svc_bsd_kevent.c.
 */

#if !defined(__APPLE__)
#define _GNU_SOURCE
#else
#define _DARWIN_C_SOURCE
#endif

#include "a32rt.h"
#include "ios/ios.h"
#include "os/backend.h"
#include "vm/vm32.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

/* Darwin ABI constants (stable XNU ABI, valid on both hosts). */
#define D_AF_UNIX 1U
#define D_AF_INET 2U
#define D_AF_INET6 30U
#define D_SOL_SOCKET 0xFFFFU
#define D_SO_REUSEADDR 0x0004U
#define D_SO_KEEPALIVE 0x0008U
#define D_SO_BROADCAST 0x0020U
#define D_SO_LINGER 0x0080U
#define D_SO_REUSEPORT 0x0200U
#define D_SO_SNDBUF 0x1001U
#define D_SO_RCVBUF 0x1002U
#define D_SO_ERROR 0x1007U
#define D_SO_TYPE 0x1008U
#define D_SO_NOSIGPIPE 0x1022U
#define D_MSG_OOB 0x0001U
#define D_MSG_PEEK 0x0002U
#define D_MSG_DONTROUTE 0x0004U
#define D_MSG_EOR 0x0008U
#define D_MSG_TRUNC 0x0010U
#define D_MSG_CTRUNC 0x0020U
#define D_MSG_WAITALL 0x0040U
#define D_MSG_DONTWAIT 0x0080U
#define D_MSG_NOSIGNAL 0x80000U
#define D_SCM_RIGHTS 0x01U
#define D_EBADF 9
#define D_EFAULT 14
#define D_EINVAL 22
#define D_ENOMEM 12
#define D_EAFNOSUPPORT 47
#define D_ENOPROTOOPT 42
#define SK_IOV_MAX 64U
#define SK_BUF_MAX 128U
#define SK_CTL_MAX 256U

static bool a32_vm32_access_ok(a32_ctx_t *ctx, uint32_t va, uint32_t len,
    uint32_t prot)
{
    return len == 0U || a32_guest_range_ok(ctx, va, len, prot);
}

static void sk_fail(a32_ctx_t *ctx)
{
    a32_ios_bsd_err(ctx, a32_ios_host_to_darwin_errno(errno));
}

static void sk_st32(a32_ctx_t *ctx, uint32_t va, uint32_t v)
{
    memcpy(a32_guest_ptr(ctx, va), &v, 4);
    a32_vm32_copyout_invalidate_exec(ctx, va, 4);
}

static int sk_dom_g2h(uint32_t d) /* Darwin AF -> host AF */
{
    if (d == D_AF_UNIX) { return AF_UNIX; }
    if (d == D_AF_INET) { return AF_INET; }
    return d == D_AF_INET6 ? AF_INET6 : -1; /* 30 Darwin, 10 Linux */
}

static uint32_t sk_fam_h2g(int f) /* host AF -> Darwin AF */
{
    if (f == AF_UNIX) { return D_AF_UNIX; }
    if (f == AF_INET) { return D_AF_INET; }
    return f == AF_INET6 ? D_AF_INET6 : (uint32_t)f;
}

static int sk_opt_g2h(uint32_t level, uint32_t name, bool *noop)
{
    *noop = false;
    if (level != D_SOL_SOCKET) { return (int)name; } /* TCP etc. match */
    switch (name) {
    case D_SO_REUSEADDR: return SO_REUSEADDR;
    case D_SO_KEEPALIVE: return SO_KEEPALIVE;
    case D_SO_BROADCAST: return SO_BROADCAST;
    case D_SO_LINGER: return SO_LINGER;
    case D_SO_REUSEPORT: return SO_REUSEPORT;
    case D_SO_SNDBUF: return SO_SNDBUF;
    case D_SO_RCVBUF: return SO_RCVBUF;
    case D_SO_ERROR: return SO_ERROR;
    case D_SO_TYPE: return SO_TYPE;
    case D_SO_NOSIGPIPE: *noop = true; return 0;
    default: return -1;
    }
}

/* Darwin msg flags -> host. Darwin MSG_DONTWAIT is 0x80 (Linux: 0x40)
 * and Darwin MSG_NOSIGNAL is 0x80000; map every bit explicitly. */
static int sk_flags_g2h(uint32_t f)
{
    int h = 0;
    if ((f & D_MSG_OOB) != 0U) { h |= MSG_OOB; }
    if ((f & D_MSG_PEEK) != 0U) { h |= MSG_PEEK; }
    if ((f & D_MSG_DONTROUTE) != 0U) { h |= MSG_DONTROUTE; }
#ifdef MSG_EOR
    if ((f & D_MSG_EOR) != 0U) { h |= MSG_EOR; }
#endif
#ifdef MSG_TRUNC
    if ((f & D_MSG_TRUNC) != 0U) { h |= MSG_TRUNC; }
#endif
#ifdef MSG_CTRUNC
    if ((f & D_MSG_CTRUNC) != 0U) { h |= MSG_CTRUNC; }
#endif
#ifdef MSG_WAITALL
    if ((f & D_MSG_WAITALL) != 0U) { h |= MSG_WAITALL; }
#endif
#ifdef MSG_DONTWAIT
    if ((f & D_MSG_DONTWAIT) != 0U) { h |= MSG_DONTWAIT; }
#endif
#ifdef MSG_NOSIGNAL
    if ((f & D_MSG_NOSIGNAL) != 0U) { h |= MSG_NOSIGNAL; }
#endif
    return h;
}

/* Host msg_flags -> Darwin (for recvmsg msg_flags writeback). */
static uint32_t sk_flags_h2g(int f)
{
    uint32_t d = 0U;
    if ((f & MSG_OOB) != 0) { d |= D_MSG_OOB; }
    if ((f & MSG_PEEK) != 0) { d |= D_MSG_PEEK; }
    if ((f & MSG_DONTROUTE) != 0) { d |= D_MSG_DONTROUTE; }
#ifdef MSG_EOR
    if ((f & MSG_EOR) != 0) { d |= D_MSG_EOR; }
#endif
#ifdef MSG_TRUNC
    if ((f & MSG_TRUNC) != 0) { d |= D_MSG_TRUNC; }
#endif
#ifdef MSG_CTRUNC
    if ((f & MSG_CTRUNC) != 0) { d |= D_MSG_CTRUNC; }
#endif
#ifdef MSG_WAITALL
    if ((f & MSG_WAITALL) != 0) { d |= D_MSG_WAITALL; }
#endif
#ifdef MSG_DONTWAIT
    if ((f & MSG_DONTWAIT) != 0) { d |= D_MSG_DONTWAIT; }
#endif
    return d;
}

/* BSD sockaddr {u8 len, u8 family, ...} -> Linux {u16 family, ...}. */
static int sk_sa_g2h(a32_ctx_t *ctx, uint32_t gva, uint32_t glen,
    struct sockaddr_storage *out, uint32_t *out_len)
{
    uint8_t hdr[2], *hp;
    uint32_t blen;
    int hf;
    if (glen < 2U || glen > SK_BUF_MAX) { return -D_EINVAL; }
    if (!a32_vm32_access_ok(ctx, gva, glen, (uint32_t)PROT_READ)) { return -D_EFAULT; }
    memcpy(hdr, a32_guest_ptr(ctx, gva), 2);
    blen = hdr[0];
    if (blen < 2U || blen > glen) { blen = glen; }
    hf = sk_dom_g2h(hdr[1]);
    if (hf < 0) { return -D_EAFNOSUPPORT; }
    memset(out, 0, sizeof(*out));
    hp = (uint8_t *)out;
    hp[0] = (uint8_t)(hf & 0xFF);
    hp[1] = (uint8_t)((hf >> 8) & 0xFF); /* little-endian u16 family */
    memcpy(hp + 2, (const uint8_t *)a32_guest_ptr(ctx, gva) + 2, (size_t)(blen - 2U));
    *out_len = blen;
    return 0;
}

/* Linux sockaddr -> BSD {u8 len, u8 family, ...}; returns full bsd length. */
static int sk_sa_h2g(a32_ctx_t *ctx, const struct sockaddr *sa, uint32_t slen,
    uint32_t gva, uint32_t gmax)
{
    const uint8_t *hp;
    uint8_t *gp;
    uint32_t n;
    uint16_t hf;
    if (slen < 2U || slen > SK_BUF_MAX) { return -D_EINVAL; }
    if (gmax == 0U) { return (int)slen; }
    if (!a32_vm32_access_ok(ctx, gva, gmax, (uint32_t)PROT_WRITE)) { return -D_EFAULT; }
    hp = (const uint8_t *)sa;
    gp = (uint8_t *)a32_guest_ptr(ctx, gva);
    hf = (uint16_t)((uint16_t)hp[0] | ((uint16_t)hp[1] << 8));
    n = slen < gmax ? slen : gmax;
    gp[0] = (uint8_t)slen;
    if (n > 1U) { gp[1] = (uint8_t)sk_fam_h2g((int)hf); }
    if (n > 2U) { memcpy(gp + 2, hp + 2, (size_t)(n - 2U)); }
    a32_vm32_copyout_invalidate_exec(ctx, gva, n);
    return (int)slen;
}

static bool sk_ld_len(a32_ctx_t *ctx, uint32_t va, uint32_t *out)
{
    uint32_t v;
    if (!a32_vm32_access_ok(ctx, va, 4U, (uint32_t)PROT_READ)) { return false; }
    memcpy(&v, a32_guest_ptr(ctx, va), 4);
    *out = v > SK_BUF_MAX ? SK_BUF_MAX : v;
    return true;
}

static int sk_store_addr(a32_ctx_t *ctx, const struct sockaddr *sa,
    uint32_t slen, uint32_t gva, uint32_t capva)
{
    uint32_t cap;
    int rc;
    if (!sk_ld_len(ctx, capva, &cap)) { return -D_EFAULT; }
    rc = sk_sa_h2g(ctx, sa, slen, gva, cap);
    if (rc < 0 || !a32_vm32_access_ok(ctx, capva, 4U, (uint32_t)PROT_WRITE)) {
        return rc < 0 ? rc : -D_EFAULT;
    }
    sk_st32(ctx, capva, (uint32_t)rc);
    return 0;
}

static void sk_do_socket(a32_ctx_t *ctx, const uint32_t *a)
{
    int dom = sk_dom_g2h(a[0]);
    int fd;
    if (dom < 0) { a32_ios_bsd_err(ctx, D_EAFNOSUPPORT); return; }
    fd = socket(dom, (int)(a[1] & 0xFUL), (int)(int32_t)a[2]);
    if (fd < 0) { sk_fail(ctx); return; }
    a32_ios_bsd_ok(ctx, (uint32_t)fd, 0U, false);
}

static void sk_do_addrop(a32_ctx_t *ctx, const uint32_t *a, bool is_bind)
{
    struct sockaddr_storage hs;
    uint32_t hlen = 0U;
    int fd = (int)(int32_t)a[0], rc = sk_sa_g2h(ctx, a[1], a[2], &hs, &hlen);
    if (rc != 0) { a32_ios_bsd_err(ctx, -rc); return; }
    rc = is_bind ? bind(fd, (const struct sockaddr *)&hs, (socklen_t)hlen) :
        connect(fd, (const struct sockaddr *)&hs, (socklen_t)hlen);
    if (rc != 0) { sk_fail(ctx); return; }
    a32_ios_bsd_ok(ctx, 0U, 0U, false);
}

static void sk_do_setsockopt(a32_ctx_t *ctx, const uint32_t *a)
{
    bool noop = false;
    int name = sk_opt_g2h(a[1], a[2], &noop);
    uint8_t buf[SK_BUF_MAX];
    int level;
    if (noop) { a32_ios_bsd_ok(ctx, 0U, 0U, false); return; } /* NOSIGPIPE: MSG_NOSIGNAL covers Linux */
    if (name < 0) { a32_ios_bsd_err(ctx, D_ENOPROTOOPT); return; }
    if (a[4] > SK_BUF_MAX || !a32_vm32_access_ok(ctx, a[3], a[4], (uint32_t)PROT_READ)) {
        a32_ios_bsd_err(ctx, a[4] > SK_BUF_MAX ? D_EINVAL : D_EFAULT);
        return;
    }
    level = (a[1] == D_SOL_SOCKET) ? SOL_SOCKET : (int)a[1];
    memcpy(buf, a32_guest_ptr(ctx, a[3]), a[4]);
    if (setsockopt((int)(int32_t)a[0], level, name, buf, (socklen_t)a[4]) != 0) {
        sk_fail(ctx);
        return;
    }
    a32_ios_bsd_ok(ctx, 0U, 0U, false);
}

static void sk_do_getsockopt(a32_ctx_t *ctx, const uint32_t *a)
{
    bool noop = false;
    int name = sk_opt_g2h(a[1], a[2], &noop);
    uint8_t buf[SK_BUF_MAX];
    uint32_t cap;
    socklen_t hlen;
    if (name < 0 || !sk_ld_len(ctx, a[4], &cap)) {
        a32_ios_bsd_err(ctx, name < 0 ? D_ENOPROTOOPT : D_EFAULT);
        return;
    }
    hlen = (socklen_t)(cap > 4U && noop ? 4U : cap); /* NOSIGPIPE reads as 0 */
    memset(buf, 0, sizeof(buf));
    if (!noop && getsockopt((int)(int32_t)a[0],
            (a[1] == D_SOL_SOCKET) ? SOL_SOCKET : (int)a[1], name, buf, &hlen) != 0) {
        sk_fail(ctx);
        return;
    }
    if (!a32_vm32_access_ok(ctx, a[3], (uint32_t)hlen, (uint32_t)PROT_WRITE)) {
        a32_ios_bsd_err(ctx, D_EFAULT);
        return;
    }
    memcpy(a32_guest_ptr(ctx, a[3]), buf, hlen);
    a32_vm32_copyout_invalidate_exec(ctx, a[3], (uint32_t)hlen);
    sk_st32(ctx, a[4], (uint32_t)hlen);
    a32_ios_bsd_ok(ctx, 0U, 0U, false);
}

static void sk_do_socketpair(a32_ctx_t *ctx, const uint32_t *a)
{
    int dom = sk_dom_g2h(a[0]);
    int f[2];
    int32_t g[2];
    if (dom < 0) { a32_ios_bsd_err(ctx, D_EAFNOSUPPORT); return; }
    if (socketpair(dom, (int)(a[1] & 0xFUL), (int)(int32_t)a[2], f) != 0) {
        sk_fail(ctx);
        return;
    }
    if (!a32_vm32_access_ok(ctx, a[3], 8U, (uint32_t)PROT_WRITE)) {
        (void)close(f[0]);
        (void)close(f[1]);
        a32_ios_bsd_err(ctx, D_EFAULT);
        return;
    }
    g[0] = (int32_t)f[0];
    g[1] = (int32_t)f[1]; /* two int32 fds out */
    memcpy(a32_guest_ptr(ctx, a[3]), g, 8);
    a32_vm32_copyout_invalidate_exec(ctx, a[3], 8);
    a32_ios_bsd_ok(ctx, 0U, 0U, false);
}

/* 32-bit Darwin msghdr (28 bytes); cmsg {u32 len, i32 level, i32 type}. */
typedef struct __attribute__((packed, aligned(4))) sk_msg32 {
    uint32_t msg_name;
    uint32_t msg_namelen;
    uint32_t msg_iov;
    int32_t msg_iovlen;
    uint32_t msg_control;
    uint32_t msg_controllen;
    int32_t msg_flags;
} sk_msg32_t;

/* Guest (12B, 4-byte aligned cmsghdr) -> host control buffer.
 * The host cmsghdr is 12B on macOS but 16B on Linux, so the host side
 * is built only with CMSG_SPACE / CMSG_LEN / CMSG_FIRSTHDR /
 * CMSG_NXTHDR / CMSG_DATA. `out` must be CMSG-aligned (callers pass a
 * union with struct cmsghdr). */
static int sk_ctl_g2h(a32_ctx_t *ctx, uint32_t gva, uint32_t glen,
    void *out, uint32_t *out_len)
{
    struct msghdr mhdr;
    struct cmsghdr *hc;
    uint32_t gp = gva, end = gva + glen;
    size_t used = 0U;
    if (glen > SK_CTL_MAX ||
            !a32_vm32_access_ok(ctx, gva, glen, (uint32_t)PROT_READ)) {
        return glen > SK_CTL_MAX ? -D_EINVAL : -D_EFAULT;
    }
    memset(&mhdr, 0, sizeof(mhdr));
    mhdr.msg_control = out;
    mhdr.msg_controllen = SK_CTL_MAX;
    hc = CMSG_FIRSTHDR(&mhdr);
    if (hc == NULL) {
        return -D_EINVAL;
    }
    while (gp + 12U <= end) {
        uint32_t cl;
        int32_t lv, tp;
        uint32_t dlen;
        memcpy(&cl, a32_guest_ptr(ctx, gp), 4);
        memcpy(&lv, a32_guest_ptr(ctx, gp + 4U), 4);
        memcpy(&tp, a32_guest_ptr(ctx, gp + 8U), 4);
        if (cl < 12U || gp + cl > end) { break; }
        dlen = cl - 12U;
        if (used + CMSG_SPACE(dlen) > SK_CTL_MAX) { break; }
        lv = (lv == (int32_t)D_SOL_SOCKET) ? SOL_SOCKET : lv;
        tp = (lv == SOL_SOCKET && tp == (int32_t)D_SCM_RIGHTS) ? SCM_RIGHTS : tp;
        hc->cmsg_len = CMSG_LEN(dlen);
        hc->cmsg_level = lv;
        hc->cmsg_type = tp;
        if (dlen > 0U) {
            memcpy(CMSG_DATA(hc), a32_guest_ptr(ctx, gp + 12U), dlen);
        }
        used = (size_t)((uint8_t *)hc - (uint8_t *)out) + CMSG_SPACE(dlen);
        gp += (cl + 3U) & ~3U;
        if (gp + 12U <= end) {
            hc = CMSG_NXTHDR(&mhdr, hc);
            if (hc == NULL) { break; }
        }
    }
    *out_len = (uint32_t)used;
    return 0;
}

/* Host control buffer -> guest (12B cmsghdr, 4-byte aligned). Iterates
 * with CMSG_FIRSTHDR / CMSG_NXTHDR so both the 12B macOS and the 16B
 * Linux host cmsghdr are decoded correctly. */
static int sk_ctl_h2g(a32_ctx_t *ctx, const void *in, uint32_t ilen,
    uint32_t gva, uint32_t gmax)
{
    struct msghdr mhdr;
    struct cmsghdr *hc;
    const uint8_t *base = (const uint8_t *)in;
    uint32_t gp = 0U;
    if (!a32_vm32_access_ok(ctx, gva, gmax, (uint32_t)PROT_WRITE)) {
        return -D_EFAULT;
    }
    memset(&mhdr, 0, sizeof(mhdr));
    mhdr.msg_control = (void *)base;
    mhdr.msg_controllen = ilen;
    for (hc = CMSG_FIRSTHDR(&mhdr); hc != NULL;
            hc = CMSG_NXTHDR(&mhdr, hc)) {
        size_t hlen = hc->cmsg_len;
        uint32_t dlen;
        int32_t lv, tp;
        uint32_t gl;
        uint8_t *gd;
        if (hlen < CMSG_LEN(0U) ||
                (const uint8_t *)hc + hlen > base + ilen) {
            break;
        }
        dlen = (uint32_t)(hlen - CMSG_LEN(0U));
        lv = hc->cmsg_level;
        tp = hc->cmsg_type;
        gl = 12U + dlen;
        if (gp + gl > gmax) { break; } /* MSG_CTRUNC */
        gd = (uint8_t *)a32_guest_ptr(ctx, gva + gp);
        lv = (lv == SOL_SOCKET) ? (int32_t)D_SOL_SOCKET : lv;
        tp = (lv == (int32_t)D_SOL_SOCKET && tp == SCM_RIGHTS) ?
            (int32_t)D_SCM_RIGHTS : tp;
        memcpy(gd, &gl, 4);
        memcpy(gd + 4U, &lv, 4);
        memcpy(gd + 8U, &tp, 4);
        if (dlen > 0U) {
            memcpy(gd + 12U, CMSG_DATA(hc), dlen);
        }
        gp += (gl + 3U) & ~3U;
    }
    if (gp > 0U) { a32_vm32_copyout_invalidate_exec(ctx, gva, gp); }
    return (int)gp;
}

static int sk_gather_iov(a32_ctx_t *ctx, uint32_t gva, int32_t cnt,
    struct iovec *hv, uint32_t *gb, uint32_t *gl, bool for_read)
{
    int32_t i;
    uint32_t need = for_read ? (uint32_t)PROT_WRITE : (uint32_t)PROT_READ, b, l;
    if (cnt <= 0 || cnt > (int32_t)SK_IOV_MAX ||
            !a32_vm32_access_ok(ctx, gva, (uint32_t)cnt * 8U, (uint32_t)PROT_READ)) {
        return -D_EFAULT;
    }
    for (i = 0; i < cnt; i++) {
        memcpy(&b, a32_guest_ptr(ctx, gva + (uint32_t)i * 8U), 4);
        memcpy(&l, a32_guest_ptr(ctx, gva + (uint32_t)i * 8U + 4U), 4);
        if (l > 0U && !a32_vm32_access_ok(ctx, b, l, need)) { return -D_EFAULT; }
        gb[i] = b;
        gl[i] = l;
        hv[i].iov_base = l == 0U ? NULL : a32_guest_ptr(ctx, b);
        hv[i].iov_len = (size_t)l;
    }
    return cnt;
}

/* Unified sendto/sendmsg: sendto is folded into msg form, one sendmsg. */
static void sk_do_send(a32_ctx_t *ctx, const uint32_t *a, bool is_msg)
{
    sk_msg32_t m;
    struct sockaddr_storage hs;
    struct msghdr mh;
    struct iovec hv[SK_IOV_MAX];
    union {
        struct cmsghdr align;
        uint8_t raw[SK_CTL_MAX];
    } ctl_u;
    uint32_t gb[SK_IOV_MAX], gl[SK_IOV_MAX];
    uint32_t hlen = 0U, ctllen = 0U;
    int flags, n = 1, rc;
    ssize_t r;
    if (is_msg) {
        if (!a32_vm32_access_ok(ctx, a[1], (uint32_t)sizeof(m), (uint32_t)PROT_READ)) {
            a32_ios_bsd_err(ctx, D_EFAULT);
            return;
        }
        memcpy(&m, a32_guest_ptr(ctx, a[1]), sizeof(m));
        n = sk_gather_iov(ctx, m.msg_iov, m.msg_iovlen, hv, gb, gl, false);
        if (n < 0) { a32_ios_bsd_err(ctx, -n); return; }
        flags = sk_flags_g2h((uint32_t)a[2]);
    } else {
        if (!a32_vm32_access_ok(ctx, a[1], a[2], (uint32_t)PROT_READ)) {
            a32_ios_bsd_err(ctx, D_EFAULT);
            return;
        }
        m.msg_name = a[4];
        m.msg_namelen = a[5];
        m.msg_control = m.msg_controllen = 0U;
        hv[0].iov_base = a32_guest_ptr(ctx, a[1]);
        hv[0].iov_len = (size_t)a[2];
        flags = sk_flags_g2h(a[3]);
    }
    if (m.msg_name != 0U && m.msg_namelen != 0U) {
        rc = sk_sa_g2h(ctx, m.msg_name, m.msg_namelen, &hs, &hlen);
        if (rc != 0) { a32_ios_bsd_err(ctx, -rc); return; }
    }
    if (m.msg_control != 0U && m.msg_controllen != 0U) {
        rc = sk_ctl_g2h(ctx, m.msg_control, m.msg_controllen, ctl_u.raw,
            &ctllen);
        if (rc != 0) { a32_ios_bsd_err(ctx, -rc); return; }
    }
#if defined(__linux__)
    flags |= MSG_NOSIGNAL; /* Darwin SO_NOSIGPIPE equivalent */
#endif
    memset(&mh, 0, sizeof(mh));
    mh.msg_name = hlen == 0U ? NULL : (void *)&hs;
    mh.msg_namelen = (socklen_t)hlen;
    mh.msg_iov = hv;
    mh.msg_iovlen = (size_t)n;
    mh.msg_control = ctllen == 0U ? NULL : (void *)ctl_u.raw;
    mh.msg_controllen = (size_t)ctllen;
    r = sendmsg((int)(int32_t)a[0], &mh, flags);
    if (r < 0) { sk_fail(ctx); return; }
    a32_ios_bsd_ok(ctx, (uint32_t)r, 0U, false);
}

/* Unified recvfrom/recvmsg: recvfrom is folded into msg form, one recvmsg. */
static void sk_do_recv(a32_ctx_t *ctx, const uint32_t *a, bool is_msg)
{
    sk_msg32_t m;
    struct sockaddr_storage hs;
    struct msghdr mh;
    struct iovec hv[SK_IOV_MAX];
    union {
        struct cmsghdr align;
        uint8_t raw[SK_CTL_MAX];
    } ctl_u;
    uint32_t gb[SK_IOV_MAX], gl[SK_IOV_MAX];
    int flags, n = 1, rc;
    int32_t i;
    ssize_t r;
    if (is_msg) {
        if (!a32_vm32_access_ok(ctx, a[1], (uint32_t)sizeof(m), (uint32_t)PROT_READ)) {
            a32_ios_bsd_err(ctx, D_EFAULT);
            return;
        }
        memcpy(&m, a32_guest_ptr(ctx, a[1]), sizeof(m));
        n = sk_gather_iov(ctx, m.msg_iov, m.msg_iovlen, hv, gb, gl, true);
        if (n < 0) { a32_ios_bsd_err(ctx, -n); return; }
        flags = sk_flags_g2h((uint32_t)a[2]);
    } else {
        uint32_t cap = 0U;
        if (!a32_vm32_access_ok(ctx, a[1], a[2], (uint32_t)PROT_WRITE) ||
                ((a[4] != 0U && a[5] != 0U) && !sk_ld_len(ctx, a[5], &cap))) {
            a32_ios_bsd_err(ctx, D_EFAULT);
            return;
        }
        m.msg_name = (a[4] != 0U && a[5] != 0U) ? a[4] : 0U;
        m.msg_namelen = (a[4] != 0U && a[5] != 0U) ? cap : 0U;
        m.msg_control = m.msg_controllen = 0U;
        hv[0].iov_base = a32_guest_ptr(ctx, a[1]);
        hv[0].iov_len = (size_t)a[2];
        gb[0] = a[1];
        gl[0] = a[2];
        flags = sk_flags_g2h(a[3]);
    }
    memset(&mh, 0, sizeof(mh));
    memset(&hs, 0, sizeof(hs));
    mh.msg_name = m.msg_name == 0U ? NULL : (void *)&hs;
    mh.msg_namelen = m.msg_name == 0U ? 0 : (socklen_t)sizeof(hs);
    mh.msg_iov = hv;
    mh.msg_iovlen = (size_t)n;
    mh.msg_control = (m.msg_control == 0U || m.msg_controllen == 0U) ? NULL :
        (void *)ctl_u.raw;
    mh.msg_controllen = (m.msg_control == 0U || m.msg_controllen == 0U) ? 0 :
        sizeof(ctl_u.raw);
    r = recvmsg((int)(int32_t)a[0], &mh, flags);
    if (r < 0) { sk_fail(ctx); return; }
    for (i = 0; i < n; i++) {
        a32_vm32_copyout_invalidate_exec(ctx, gb[(uint32_t)i], gl[(uint32_t)i]);
    }
    if (is_msg) {
        if (a32_vm32_access_ok(ctx, a[1] + 24U, 4U, (uint32_t)PROT_WRITE)) {
            uint32_t df = sk_flags_h2g(mh.msg_flags);
            memcpy(a32_guest_ptr(ctx, a[1] + 24U), &df, 4);
            a32_vm32_copyout_invalidate_exec(ctx, a[1] + 24U, 4U);
        }
        if (m.msg_name != 0U && mh.msg_name != NULL) {
            rc = sk_sa_h2g(ctx, (const struct sockaddr *)&hs, (uint32_t)mh.msg_namelen,
                m.msg_name, m.msg_namelen);
            if (rc >= 0 && a32_vm32_access_ok(ctx, a[1] + 4U, 4U, (uint32_t)PROT_WRITE)) {
                sk_st32(ctx, a[1] + 4U, (uint32_t)rc);
            }
        }
        if (m.msg_control != 0U && mh.msg_control != NULL) {
            rc = sk_ctl_h2g(ctx, ctl_u.raw, (uint32_t)mh.msg_controllen,
                m.msg_control, m.msg_controllen);
            if (rc >= 0 && a32_vm32_access_ok(ctx, a[1] + 20U, 4U, (uint32_t)PROT_WRITE)) {
                sk_st32(ctx, a[1] + 20U, (uint32_t)rc);
            }
        }
    } else if (m.msg_name != 0U && mh.msg_name != NULL) {
        rc = sk_store_addr(ctx, (const struct sockaddr *)&hs, (uint32_t)mh.msg_namelen,
            m.msg_name, a[5]);
        if (rc != 0) { a32_ios_bsd_err(ctx, -rc); return; }
    }
    a32_ios_bsd_ok(ctx, (uint32_t)r, 0U, false);
}

/* which: 0 = accept, 1 = getpeername, 2 = getsockname. */
static void sk_do_getaddr(a32_ctx_t *ctx, const uint32_t *a, unsigned which)
{
    struct sockaddr_storage hs;
    socklen_t hlen = (socklen_t)sizeof(hs);
    int fd = (int)(int32_t)a[0], rc;
    bool have = a[1] != 0U && a[2] != 0U;
    memset(&hs, 0, sizeof(hs));
    if (which == 0U) {
        fd = accept(fd, have ? (struct sockaddr *)&hs : NULL, have ? &hlen : NULL);
        if (fd < 0) { sk_fail(ctx); return; }
        if (have) {
            rc = sk_store_addr(ctx, (const struct sockaddr *)&hs, (uint32_t)hlen, a[1], a[2]);
            if (rc != 0) { (void)close(fd); a32_ios_bsd_err(ctx, -rc); return; }
        }
        a32_ios_bsd_ok(ctx, (uint32_t)fd, 0U, false);
        return;
    }
    rc = which == 1U ? getpeername(fd, (struct sockaddr *)&hs, &hlen) :
        getsockname(fd, (struct sockaddr *)&hs, &hlen);
    if (rc != 0) { sk_fail(ctx); return; }
    if (!have) { a32_ios_bsd_ok(ctx, 0U, 0U, false); return; }
    rc = sk_store_addr(ctx, (const struct sockaddr *)&hs, (uint32_t)hlen, a[1], a[2]);
    if (rc != 0) { a32_ios_bsd_err(ctx, -rc); return; }
    a32_ios_bsd_ok(ctx, 0U, 0U, false);
}

/* ---- munmap / mprotect / madvise / mincore / msync (BSD 73/74/75/78/65) ----
 *
 * Called from ios_svc_bsd.c. Guest ranges are validated before any host
 * address derived from ctx->guest_base is passed to the host. */

static bool ios_vm_range_ok(a32_ctx_t *ctx, uint32_t addr, uint32_t len,
    int *err)
{
    if (len == 0U) {
        return true;
    }
    if (addr < a32_min_va(ctx) ||
            (uint64_t)addr + (uint64_t)len > (uint64_t)A32_IOS_HIGH_MIN_VA) {
        *err = 22;
        return false;
    }
    if (!a32_guest_range_ok(ctx, addr, len, (uint32_t)PROT_READ)) {
        *err = 12;
        return false;
    }
    return true;
}

void a32_ios_bsd_vm_simple(a32_ctx_t *ctx, uint32_t bsd, const uint32_t *a)
{
    if (bsd == 73U) {
        int rc = a32_vm_munmap(ctx, a[0], a[1]);
        if (rc != 0) {
            a32_ios_bsd_err(ctx, a32_ios_host_to_darwin_errno(-rc));
            return;
        }
        a32_ios_bsd_ok(ctx, 0U, 0U, false);
    } else if (bsd == 74U) {
        int rc = a32_vm_mprotect(ctx, a[0], a[1], a[2] & 7U);
        if (rc != 0) {
            a32_ios_bsd_err(ctx, a32_ios_host_to_darwin_errno(-rc));
            return;
        }
        a32_ios_bsd_ok(ctx, 0U, 0U, false);
    } else if (bsd == 75U) {
        uint32_t adv = a[3];
        int verr = 22;
        size_t hp;
        uint32_t lo, hi;
        if (!ios_vm_range_ok(ctx, a[0], a[1], &verr)) {
            a32_ios_bsd_err(ctx, verr);
        } else if (a[1] == 0U) {
            a32_ios_bsd_ok(ctx, 0U, 0U, false);
        } else if (adv == 4U || adv == 5U || adv == 6U || adv == 7U ||
                adv == 8U || adv == 9U) {
            /* Darwin free/discard hints only advise the pager; unlike
             * Linux MADV_DONTNEED they never zero pages. Treat valid
             * ranges as a no-op instead of discarding guest memory. */
            a32_ios_bsd_ok(ctx, 0U, 0U, false);
        } else if (adv > 3U) {
            a32_ios_bsd_err(ctx, 22);
        } else {
            /* Round inward to host pages so 4 KiB guest ranges on
             * 16 KiB hosts never fail with EINVAL. */
            hp = a32_backend_page_size();
            lo = (uint32_t)(((uint64_t)a[0] + (uint64_t)hp - 1ULL) &
                ~((uint64_t)hp - 1ULL));
            hi = (uint32_t)(((uint64_t)a[0] + (uint64_t)a[1]) &
                ~((uint64_t)hp - 1ULL));
            if (lo >= hi) {
                a32_ios_bsd_ok(ctx, 0U, 0U, false);
            } else if (madvise(a32_guest_ptr(ctx, lo), (size_t)(hi - lo),
                    (int)adv) != 0) {
                sk_fail(ctx);
            } else {
                a32_ios_bsd_ok(ctx, 0U, 0U, false);
            }
        }
    } else if (bsd == 78U) {
        uint64_t v64 = ((uint64_t)a[1] + 4095ULL) / 4096ULL;
        int verr = 22;
        if (a[1] == 0U ||
            (uint64_t)a[0] + (uint64_t)a[1] > 0x100000000ULL ||
            v64 > 0x100000ULL || (a[0] & 4095U) != 0U ||
            !ios_vm_range_ok(ctx, a[0], a[1], &verr) ||
            !a32_guest_range_ok(ctx, a[2], (uint32_t)v64,
                                (uint32_t)PROT_WRITE)) {
            a32_ios_bsd_err(ctx, verr);
            return;
        }
        if (mincore(a32_guest_ptr(ctx, a[0]), (size_t)a[1],
#if defined(__APPLE__)
                (char *)a32_guest_ptr(ctx, a[2])) != 0) {
#else
                (unsigned char *)a32_guest_ptr(ctx, a[2])) != 0) {
#endif
            sk_fail(ctx);
            return;
        }
        a32_vm32_copyout_invalidate_exec(ctx, a[2], (uint32_t)v64);
        a32_ios_bsd_ok(ctx, 0U, 0U, false);
    } else { /* 65 msync */
        int hf = 0;
        int verr = 22;
        if ((a[2] & 0x1U) != 0U) {
            hf |= MS_ASYNC;
        }
        if ((a[2] & 0x2U) != 0U) {
            hf |= MS_INVALIDATE;
        }
        if ((a[2] & 0x10U) != 0U) {
            hf |= MS_SYNC;
        }
        if (!ios_vm_range_ok(ctx, a[0], a[1], &verr)) {
            a32_ios_bsd_err(ctx, verr);
        } else if (a[1] == 0U) {
            a32_ios_bsd_ok(ctx, 0U, 0U, false);
        } else {
            size_t hp = a32_backend_page_size();
            uint64_t lo = (uint64_t)a[0] & ~((uint64_t)hp - 1ULL);
            uint64_t hi = ((uint64_t)a[0] + (uint64_t)a[1] +
                (uint64_t)hp - 1ULL) & ~((uint64_t)hp - 1ULL);
            int r;
            if (hi > (uint64_t)A32_IOS_HIGH_MIN_VA) {
                hi = (uint64_t)A32_IOS_HIGH_MIN_VA;
            }
            r = msync(a32_guest_ptr(ctx, (uint32_t)lo), (size_t)(hi - lo),
                hf);
            /* Host-page rounding can cover unmapped slivers next to the
             * verified range; those need no sync. */
            if (r != 0 && errno != ENOMEM) {
                sk_fail(ctx);
            } else {
                a32_ios_bsd_ok(ctx, 0U, 0U, false);
            }
        }
    }
}

int a32_ios_dispatch_bsd_sock(a32_ctx_t *ctx, uint32_t bsd_nr,
    const uint32_t *args, bool *out_handled)
{
    if (out_handled != NULL) { *out_handled = false; }
    if (ctx == NULL || args == NULL) { return 0; }
    switch (bsd_nr) {
    case 97U: sk_do_socket(ctx, args); break;
    case 98U: case 409U: sk_do_addrop(ctx, args, false); break;
    case 104U: sk_do_addrop(ctx, args, true); break;
    case 105U: sk_do_setsockopt(ctx, args); break;
    case 106U:
        if (listen((int)(int32_t)args[0], (int)(int32_t)args[1]) != 0) { sk_fail(ctx); }
        else { a32_ios_bsd_ok(ctx, 0U, 0U, false); }
        break;
    case 118U: sk_do_getsockopt(ctx, args); break;
    case 133U: case 413U: sk_do_send(ctx, args, false); break;
    case 29U: case 403U: sk_do_recv(ctx, args, false); break;
    case 27U: case 401U: sk_do_recv(ctx, args, true); break;
    case 28U: case 402U: sk_do_send(ctx, args, true); break;
    case 30U: case 404U: sk_do_getaddr(ctx, args, 0U); break;
    case 31U: sk_do_getaddr(ctx, args, 1U); break;
    case 32U: sk_do_getaddr(ctx, args, 2U); break;
    case 134U:
        if (shutdown((int)(int32_t)args[0], (int)(int32_t)args[1]) != 0) { sk_fail(ctx); }
        else { a32_ios_bsd_ok(ctx, 0U, 0U, false); }
        break;
    case 135U: sk_do_socketpair(ctx, args); break;
    default: return a32_ios_dispatch_bsd_kevent(ctx, bsd_nr, args,
        out_handled);
    }
    if (out_handled != NULL) { *out_handled = true; }
    return 0;
}
