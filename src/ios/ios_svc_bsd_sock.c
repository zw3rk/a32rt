/*
 * src/ios/ios_svc_bsd_sock.c — 32-bit iOS XNU BSD sockets + kqueue
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Curated C11, host-portable (macOS arm64 AND aarch64-linux-musl).
 * BSD sockets + portable user-space kqueue (real host pipe fds,
 * poll() for READ/WRITE, CLOCK_MONOTONIC for TIMER).
 */

#if !defined(__APPLE__)
#define _GNU_SOURCE
#else
#define _DARWIN_C_SOURCE
#endif

#include "a32rt.h"
#include "ios/ios.h"
#include "vm/vm32.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
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
#define D_EV_READ -1
#define D_EV_WRITE -2
#define D_EV_TIMER -7
#define D_EV_ADD 0x0001U
#define D_EV_DELETE 0x0002U
#define D_EV_DISABLE 0x0008U
#define D_EV_ONESHOT 0x0010U
#define D_EV_ERROR 0x4000U
#define D_EV_EOF 0x8000U
#define D_EBADF 9
#define D_EFAULT 14
#define D_EINVAL 22
#define D_ENOMEM 12
#define D_EAFNOSUPPORT 47
#define D_ENOPROTOOPT 42
#define SK_IOV_MAX 64U
#define SK_BUF_MAX 128U
#define SK_CTL_MAX 256U
#define SK_KQ_MAX 8U
#define SK_KNOTE_MAX 32U
#define SK_EV_CAP 256U

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

static int sk_flags_g2h(uint32_t f) /* Darwin send/recv flags -> host */
{
    int h = (int)(f & 0x7U); /* OOB / PEEK / DONTROUTE match */
#ifdef MSG_EOR
    if ((f & 0x08U) != 0U) { h |= MSG_EOR; }
#endif
#ifdef MSG_TRUNC
    if ((f & 0x10U) != 0U) { h |= MSG_TRUNC; }
#endif
#ifdef MSG_CTRUNC
    if ((f & 0x20U) != 0U) { h |= MSG_CTRUNC; }
#endif
#ifdef MSG_WAITALL
    if ((f & 0x40U) != 0U) { h |= MSG_WAITALL; }
#endif
    return h;
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

static int sk_ctl_g2h(a32_ctx_t *ctx, uint32_t gva, uint32_t glen,
    uint8_t *out, uint32_t *out_len)
{
    uint32_t gp = gva, end = gva + glen, hp = 0U, cl;
    int32_t lv, tp;
    uint64_t hlen;
    if (glen > SK_CTL_MAX || !a32_vm32_access_ok(ctx, gva, glen, (uint32_t)PROT_READ)) {
        return glen > SK_CTL_MAX ? -D_EINVAL : -D_EFAULT;
    }
    while (gp + 12U <= end) {
        memcpy(&cl, a32_guest_ptr(ctx, gp), 4);
        memcpy(&lv, a32_guest_ptr(ctx, gp + 4U), 4);
        memcpy(&tp, a32_guest_ptr(ctx, gp + 8U), 4);
        if (cl < 12U || gp + cl > end) { break; }
        hlen = (uint64_t)((16U + (cl - 12U) + 7U) & ~7U); /* cmsg 8-align */
        if (hp + hlen > SK_CTL_MAX) { break; }
        lv = (lv == (int32_t)D_SOL_SOCKET) ? SOL_SOCKET : lv;
        memcpy(out + hp, &hlen, 8);
        memcpy(out + hp + 8U, &lv, 4);
        memcpy(out + hp + 12U, &tp, 4);
        memcpy(out + hp + 16U, a32_guest_ptr(ctx, gp + 12U), cl - 12U);
        hp += (uint32_t)hlen;
        gp += (cl + 3U) & ~3U;
    }
    *out_len = hp;
    return 0;
}

static int sk_ctl_h2g(a32_ctx_t *ctx, const uint8_t *in, uint32_t ilen,
    uint32_t gva, uint32_t gmax)
{
    uint32_t ip = 0U, gp = 0U, gl;
    int32_t lv, tp;
    uint64_t hl;
    uint8_t *gd;
    if (!a32_vm32_access_ok(ctx, gva, gmax, (uint32_t)PROT_WRITE)) { return -D_EFAULT; }
    while (ip + 16U <= ilen) {
        memcpy(&hl, in + ip, 8);
        memcpy(&lv, in + ip + 8U, 4);
        memcpy(&tp, in + ip + 12U, 4);
        if (hl < 16U || ip + hl > ilen) { break; }
        gl = 12U + (uint32_t)hl - 16U;
        if (gp + gl > gmax) { break; } /* MSG_CTRUNC */
        gd = (uint8_t *)a32_guest_ptr(ctx, gva + gp);
        lv = (lv == SOL_SOCKET) ? (int32_t)D_SOL_SOCKET : lv;
        memcpy(gd, &gl, 4);
        memcpy(gd + 4U, &lv, 4);
        memcpy(gd + 8U, &tp, 4);
        memcpy(gd + 12U, in + ip + 16U, gl - 12U);
        gp += (gl + 3U) & ~3U;
        ip += (((uint32_t)hl + 7U) & ~7U); /* cmsg 8-align */
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
    uint8_t ctl[SK_CTL_MAX];
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
        rc = sk_ctl_g2h(ctx, m.msg_control, m.msg_controllen, ctl, &ctllen);
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
    mh.msg_control = ctllen == 0U ? NULL : (void *)ctl;
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
    uint8_t ctl[SK_CTL_MAX];
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
    mh.msg_control = (m.msg_control == 0U || m.msg_controllen == 0U) ? NULL : (void *)ctl;
    mh.msg_controllen = (m.msg_control == 0U || m.msg_controllen == 0U) ? 0 : sizeof(ctl);
    r = recvmsg((int)(int32_t)a[0], &mh, flags);
    if (r < 0) { sk_fail(ctx); return; }
    for (i = 0; i < n; i++) {
        a32_vm32_copyout_invalidate_exec(ctx, gb[(uint32_t)i], gl[(uint32_t)i]);
    }
    if (is_msg) {
        if (m.msg_name != 0U && mh.msg_name != NULL) {
            rc = sk_sa_h2g(ctx, (const struct sockaddr *)&hs, (uint32_t)mh.msg_namelen,
                m.msg_name, m.msg_namelen);
            if (rc >= 0 && a32_vm32_access_ok(ctx, a[1] + 4U, 4U, (uint32_t)PROT_WRITE)) {
                sk_st32(ctx, a[1] + 4U, (uint32_t)rc);
            }
        }
        if (m.msg_control != 0U && mh.msg_control != NULL) {
            rc = sk_ctl_h2g(ctx, ctl, (uint32_t)mh.msg_controllen, m.msg_control,
                m.msg_controllen);
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

/* Portable kqueue: 8 kqueues x 32 knotes, real host pipe fds. */
typedef struct {
    bool used; bool oneshot; bool disabled;
    int16_t filter; /* D_EV_READ / D_EV_WRITE / D_EV_TIMER */
    uint32_t ident; /* fd for READ/WRITE, timer id for TIMER */
    uint32_t fflags; uint64_t udata; uint64_t deadline_ns; uint64_t period_ns;
} sk_knote_t;
typedef struct {
    bool used; int rfd; int wfd; /* rfd is the kqueue fd itself */
    sk_knote_t notes[SK_KNOTE_MAX];
} sk_kq_t;

static sk_kq_t g_kq[SK_KQ_MAX]; /* +1 pipe-backed fd per kqueue */
static pthread_mutex_t g_kq_mu = PTHREAD_MUTEX_INITIALIZER;

static uint64_t sk_now_ns(void)
{
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static sk_kq_t *sk_kq_alloc(void) /* reclaims pipes closed behind us */
{
    uint32_t i;
    for (i = 0U; i < SK_KQ_MAX; i++) {
        if (!g_kq[i].used) { return &g_kq[i]; }
        if (fcntl(g_kq[i].rfd, F_GETFD) < 0 && errno == EBADF) {
            (void)close(g_kq[i].wfd);
            memset(&g_kq[i], 0, sizeof(g_kq[i]));
            return &g_kq[i];
        }
    }
    return NULL;
}

void a32_ios_kq_close(int fd)
{
    uint32_t i;
    if (fd < 0) {
        return;
    }
    (void)pthread_mutex_lock(&g_kq_mu);
    for (i = 0U; i < SK_KQ_MAX; i++) {
        if (g_kq[i].used && g_kq[i].rfd == fd) {
            if (g_kq[i].wfd >= 0 && g_kq[i].wfd != fd) {
                (void)close(g_kq[i].wfd);
            }
            memset(&g_kq[i], 0, sizeof(g_kq[i]));
        }
    }
    (void)pthread_mutex_unlock(&g_kq_mu);
}

static void sk_knote_change(sk_kq_t *kq, int16_t filter, uint16_t flags,
    uint32_t fflags, int32_t data, uint32_t ident, uint64_t udata)
{
    sk_knote_t *kn = NULL;
    sk_knote_t *slot = NULL;
    uint32_t i;
    uint64_t period;
    if (filter != (int16_t)D_EV_READ && filter != (int16_t)D_EV_WRITE &&
            filter != (int16_t)D_EV_TIMER) { return; }
    for (i = 0U; i < SK_KNOTE_MAX; i++) {
        if (kq->notes[i].used) {
            if (kq->notes[i].filter == filter && kq->notes[i].ident == ident) {
                kn = &kq->notes[i];
                break;
            }
        } else if (slot == NULL) {
            slot = &kq->notes[i];
        }
    }
    if ((flags & D_EV_DELETE) != 0U) {
        if (kn != NULL) { memset(kn, 0, sizeof(*kn)); }
        return;
    }
    if ((flags & D_EV_ADD) == 0U) {
        if (kn != NULL) { kn->disabled = (flags & D_EV_DISABLE) != 0U; }
        return;
    }
    if (kn == NULL) {
        if (slot == NULL) { return; }
        kn = slot;
        memset(kn, 0, sizeof(*kn));
        kn->used = true;
        kn->filter = filter;
        kn->ident = ident;
    }
    kn->fflags = fflags;
    kn->udata = udata;
    kn->disabled = (flags & D_EV_DISABLE) != 0U;
    kn->oneshot = (flags & D_EV_ONESHOT) != 0U; /* one-shot or periodic */
    if (filter != (int16_t)D_EV_TIMER) { return; }
    period = data <= 0 ? 0U : (uint64_t)data;
    period *= ((fflags & 1U) != 0U) ? 1000000000ULL : 1000000ULL; /* s vs ms */
    kn->period_ns = ((flags & D_EV_ONESHOT) != 0U) ? 0U : period;
    kn->deadline_ns = sk_now_ns() + period;
}

static bool sk_knote_ready(const sk_knote_t *kn, int32_t *data, bool *eof)
{
    struct pollfd pfd = { (int)kn->ident,
        (short)(kn->filter == (int16_t)D_EV_READ ? POLLIN : POLLOUT), 0 };
    *eof = false;
    *data = 1;
    if (kn->filter == (int16_t)D_EV_TIMER) { return sk_now_ns() >= kn->deadline_ns; }
    if (poll(&pfd, 1, 0) < 0 || (pfd.revents & (POLLNVAL | POLLHUP | POLLERR)) != 0) {
        *eof = true;
        return true;
    }
    if ((pfd.revents & pfd.events) == 0) { return false; }
    if (kn->filter == (int16_t)D_EV_READ) {
        int n = 0;
        if (ioctl((int)kn->ident, FIONREAD, &n) == 0 && n > 0) { *data = (int32_t)n; }
    }
    return true;
}

/* 20-byte kevent / 48-byte kevent64 (4-byte aligned, packed). */
typedef struct __attribute__((packed, aligned(4))) {
    uint32_t ident; int16_t filter; uint16_t flags; uint32_t fflags;
    int32_t data; uint32_t udata;
} sk_ev32_t;
typedef struct __attribute__((packed, aligned(4))) {
    uint64_t ident; int16_t filter; uint16_t flags; uint32_t fflags;
    int64_t data; uint64_t udata; uint64_t ext[2];
} sk_ev64_t;

_Static_assert(sizeof(sk_ev32_t) == 20, "kevent must be 20 bytes");
_Static_assert(sizeof(sk_ev64_t) == 48, "kevent64 must be 48 bytes");

static void sk_emit(a32_ctx_t *ctx, uint32_t base, uint32_t idx, bool is64,
    uint64_t ident, int16_t filter, uint16_t flags, uint32_t fflags,
    int64_t data, uint64_t udata)
{
    if (!is64) {
        sk_ev32_t v = { (uint32_t)ident, filter, flags, fflags, (int32_t)data,
            (uint32_t)udata };
        memcpy(a32_guest_ptr(ctx, base + idx * 20U), &v, 20);
        return;
    }
    sk_ev64_t v = { ident, filter, flags, fflags, data, udata, { 0U, 0U } };
    memcpy(a32_guest_ptr(ctx, base + idx * 48U), &v, 48);
}

/* Shared kevent/kevent64 scan; is64 selects the 48-byte struct. */
static void sk_do_kevent(a32_ctx_t *ctx, int kd, uint32_t chl, int nch,
    uint32_t evl, int nev, uint64_t timeout_ns, bool is64)
{
    sk_kq_t *kq = NULL;
    uint32_t i, nout = 0U, sz = is64 ? 48U : 20U;
    uint32_t o_ff = is64 ? 12U : 8U;
    struct pollfd pfds[SK_KNOTE_MAX];
    struct timespec ts;
    nfds_t nfds = 0U;
    int wait_ms;
    (void)pthread_mutex_lock(&g_kq_mu);
    for (i = 0U; i < SK_KQ_MAX; i++) {
        if (g_kq[i].used && g_kq[i].rfd == kd) { kq = &g_kq[i]; break; }
    }
    if (kq == NULL) { (void)pthread_mutex_unlock(&g_kq_mu); a32_ios_bsd_err(ctx, D_EBADF); return; }
    if (nch < 0 || nev < 0 || (uint32_t)nch > SK_EV_CAP || (uint32_t)nev > SK_EV_CAP) {
        (void)pthread_mutex_unlock(&g_kq_mu);
        a32_ios_bsd_err(ctx, D_EINVAL);
        return;
    }
    if ((nch > 0 && !a32_vm32_access_ok(ctx, chl, (uint32_t)nch * sz, (uint32_t)PROT_READ)) ||
            (nev > 0 && !a32_vm32_access_ok(ctx, evl, (uint32_t)nev * sz, (uint32_t)PROT_WRITE))) {
        (void)pthread_mutex_unlock(&g_kq_mu);
        a32_ios_bsd_err(ctx, D_EFAULT);
        return;
    }
    for (i = 0U; i < (uint32_t)nch; i++) {
        const uint8_t *e = (const uint8_t *)a32_guest_ptr(ctx, chl + i * sz);
        uint64_t ident, udata;
        int64_t data;
        int16_t filter;
        uint16_t flags;
        uint32_t fflags, v32;
        int32_t d32;
        if (is64) {
            memcpy(&ident, e, 8);
            memcpy(&filter, e + 8U, 2);
            memcpy(&flags, e + 10U, 2);
            memcpy(&fflags, e + o_ff, 4);
            memcpy(&data, e + 16U, 8);
            memcpy(&udata, e + 24U, 8);
        } else {
            memcpy(&v32, e, 4);
            ident = v32;
            memcpy(&filter, e + 4U, 2);
            memcpy(&flags, e + 6U, 2);
            memcpy(&fflags, e + o_ff, 4);
            memcpy(&d32, e + 12U, 4);
            data = d32;
            memcpy(&v32, e + 16U, 4);
            udata = v32;
        }
        sk_knote_change(kq, filter, flags, fflags,
            data > 0x7FFFFFFFLL ? 0x7FFFFFFF : (int32_t)data, (uint32_t)ident, udata);
    }
    if (nev == 0) { (void)pthread_mutex_unlock(&g_kq_mu); a32_ios_bsd_ok(ctx, 0U, 0U, false); return; }
    for (i = 0U; i < SK_KNOTE_MAX; i++) {
        if (kq->notes[i].used && !kq->notes[i].disabled &&
                kq->notes[i].filter != (int16_t)D_EV_TIMER) {
            pfds[nfds].fd = (int)kq->notes[i].ident;
            pfds[nfds].events = POLLIN | POLLOUT;
            pfds[nfds].revents = 0;
            nfds++;
        }
    }
    wait_ms = timeout_ns == 0U ? 0 : timeout_ns >= 60000000000ULL ? 60000 :
        (int)(timeout_ns / 1000000ULL);
    (void)pthread_mutex_unlock(&g_kq_mu);
    if (nfds > 0U) { (void)poll(pfds, nfds, wait_ms); }
    else if (timeout_ns != 0U) {
        ts.tv_sec = (time_t)(timeout_ns / 1000000000ULL);
        ts.tv_nsec = (long)(timeout_ns % 1000000000ULL);
        (void)nanosleep(&ts, NULL);
    }
    (void)pthread_mutex_lock(&g_kq_mu);
    kq = NULL;
    for (i = 0U; i < SK_KQ_MAX; i++) {
        if (g_kq[i].used && g_kq[i].rfd == kd) { kq = &g_kq[i]; break; }
    }
    if (kq == NULL) {
        (void)pthread_mutex_unlock(&g_kq_mu);
        a32_ios_bsd_err(ctx, D_EBADF);
        return;
    }
    for (i = 0U; i < SK_KNOTE_MAX && nout < (uint32_t)nev; i++) {
        sk_knote_t *kn = &kq->notes[i];
        sk_knote_t ev;
        int32_t data;
        bool eof;
        if (!kn->used || kn->disabled) { continue; }
        if (!sk_knote_ready(kn, &data, &eof)) { continue; }
        ev = *kn;
        if (ev.filter == (int16_t)D_EV_TIMER) {
            kn->deadline_ns = sk_now_ns() + ev.period_ns;
            if (ev.period_ns == 0U) { memset(kn, 0, sizeof(*kn)); }
        } else if (ev.oneshot) {
            memset(kn, 0, sizeof(*kn));
        }
        sk_emit(ctx, evl, nout, is64, (uint64_t)ev.ident, ev.filter,
            eof ? (uint16_t)(D_EV_EOF | D_EV_ERROR) : 0U, ev.fflags,
            eof ? D_EBADF : (int64_t)data, ev.udata);
        nout++;
    }
    a32_vm32_copyout_invalidate_exec(ctx, evl, nout * sz);
    (void)pthread_mutex_unlock(&g_kq_mu);
    a32_ios_bsd_ok(ctx, nout, 0U, false);
}

static void sk_do_kqueue(a32_ctx_t *ctx)
{
    sk_kq_t *kq;
    int fds[2];
    (void)pthread_mutex_lock(&g_kq_mu);
    kq = sk_kq_alloc();
    if (kq == NULL) { (void)pthread_mutex_unlock(&g_kq_mu); a32_ios_bsd_err(ctx, D_ENOMEM); return; }
    if (pipe(fds) != 0) { (void)pthread_mutex_unlock(&g_kq_mu); sk_fail(ctx); return; }
    memset(kq, 0, sizeof(*kq));
    kq->used = true;
    kq->rfd = fds[0];
    kq->wfd = fds[1];
    (void)pthread_mutex_unlock(&g_kq_mu);
    a32_ios_bsd_ok(ctx, (uint32_t)fds[0], 0U, false);
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
    case 362U: sk_do_kqueue(ctx); break;
    case 363U: case 374U: {
        int32_t sec = 0, nsec = 0;
        uint64_t tns;
        if (args[5] != 0U) {
            if (!a32_vm32_access_ok(ctx, args[5], 8U, (uint32_t)PROT_READ)) {
                a32_ios_bsd_err(ctx, D_EFAULT);
                break;
            }
            memcpy(&sec, a32_guest_ptr(ctx, args[5]), 4);
            memcpy(&nsec, a32_guest_ptr(ctx, args[5] + 4U), 4);
            if (sec < 0 || nsec < 0 || nsec >= 1000000000) {
                a32_ios_bsd_err(ctx, D_EINVAL);
                break;
            }
        }
        tns = args[5] == 0U ? 60000000000ULL : /* NULL: cap a 60 s wait */
            (uint64_t)sec * 1000000000ULL + (uint64_t)nsec;
        sk_do_kevent(ctx, (int)(int32_t)args[0], args[1], (int)(int32_t)args[2],
            args[3], (int)(int32_t)args[4], tns, bsd_nr == 374U);
        break;
    }
    default: return 0;
    }
    if (out_handled != NULL) { *out_handled = true; }
    return 0;
}
