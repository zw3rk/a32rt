/*
 * src/gdb.c — GDB Remote Serial Protocol (RSP) stub
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#define _GNU_SOURCE
#include "a32rt.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#define GDB_MAX_BREAKPOINTS 64U
#define GDB_PKT_BUF_SIZE    4096U

typedef struct gdb_breakpoint {
    uint32_t addr;
    bool     active;
} gdb_breakpoint_t;

static int              g_server_sock = -1;
static int              g_client_sock = -1;
static gdb_breakpoint_t g_breakpoints[GDB_MAX_BREAKPOINTS];

static const char g_gdb_target_xml[] =
    "<?xml version=\"1.0\"?>\n"
    "<!DOCTYPE target SYSTEM \"gdb-target.dtd\">\n"
    "<target version=\"1.0\">\n"
    "  <architecture>arm</architecture>\n"
    "  <feature name=\"org.gnu.gdb.arm.core\">\n"
    "    <reg name=\"r0\" bitsize=\"32\" type=\"uint32\" regnum=\"0\"/>\n"
    "    <reg name=\"r1\" bitsize=\"32\" type=\"uint32\" regnum=\"1\"/>\n"
    "    <reg name=\"r2\" bitsize=\"32\" type=\"uint32\" regnum=\"2\"/>\n"
    "    <reg name=\"r3\" bitsize=\"32\" type=\"uint32\" regnum=\"3\"/>\n"
    "    <reg name=\"r4\" bitsize=\"32\" type=\"uint32\" regnum=\"4\"/>\n"
    "    <reg name=\"r5\" bitsize=\"32\" type=\"uint32\" regnum=\"5\"/>\n"
    "    <reg name=\"r6\" bitsize=\"32\" type=\"uint32\" regnum=\"6\"/>\n"
    "    <reg name=\"r7\" bitsize=\"32\" type=\"uint32\" regnum=\"7\"/>\n"
    "    <reg name=\"r8\" bitsize=\"32\" type=\"uint32\" regnum=\"8\"/>\n"
    "    <reg name=\"r9\" bitsize=\"32\" type=\"uint32\" regnum=\"9\"/>\n"
    "    <reg name=\"r10\" bitsize=\"32\" type=\"uint32\" regnum=\"10\"/>\n"
    "    <reg name=\"r11\" bitsize=\"32\" type=\"uint32\" regnum=\"11\"/>\n"
    "    <reg name=\"r12\" bitsize=\"32\" type=\"uint32\" regnum=\"12\"/>\n"
    "    <reg name=\"sp\" bitsize=\"32\" type=\"data_ptr\" regnum=\"13\"/>\n"
    "    <reg name=\"lr\" bitsize=\"32\" type=\"int\" regnum=\"14\"/>\n"
    "    <reg name=\"pc\" bitsize=\"32\" type=\"code_ptr\" regnum=\"15\"/>\n"
    "    <reg name=\"cpsr\" bitsize=\"32\" regnum=\"25\"/>\n"
    "  </feature>\n"
    "  <feature name=\"org.gnu.gdb.arm.vfp\">\n"
    "    <reg name=\"d0\" bitsize=\"64\" type=\"ieee_double\" regnum=\"26\"/>\n"
    "    <reg name=\"d1\" bitsize=\"64\" type=\"ieee_double\" regnum=\"27\"/>\n"
    "    <reg name=\"d2\" bitsize=\"64\" type=\"ieee_double\" regnum=\"28\"/>\n"
    "    <reg name=\"d3\" bitsize=\"64\" type=\"ieee_double\" regnum=\"29\"/>\n"
    "    <reg name=\"d4\" bitsize=\"64\" type=\"ieee_double\" regnum=\"30\"/>\n"
    "    <reg name=\"d5\" bitsize=\"64\" type=\"ieee_double\" regnum=\"31\"/>\n"
    "    <reg name=\"d6\" bitsize=\"64\" type=\"ieee_double\" regnum=\"32\"/>\n"
    "    <reg name=\"d7\" bitsize=\"64\" type=\"ieee_double\" regnum=\"33\"/>\n"
    "    <reg name=\"d8\" bitsize=\"64\" type=\"ieee_double\" regnum=\"34\"/>\n"
    "    <reg name=\"d9\" bitsize=\"64\" type=\"ieee_double\" regnum=\"35\"/>\n"
    "    <reg name=\"d10\" bitsize=\"64\" type=\"ieee_double\" regnum=\"36\"/>\n"
    "    <reg name=\"d11\" bitsize=\"64\" type=\"ieee_double\" regnum=\"37\"/>\n"
    "    <reg name=\"d12\" bitsize=\"64\" type=\"ieee_double\" regnum=\"38\"/>\n"
    "    <reg name=\"d13\" bitsize=\"64\" type=\"ieee_double\" regnum=\"39\"/>\n"
    "    <reg name=\"d14\" bitsize=\"64\" type=\"ieee_double\" regnum=\"40\"/>\n"
    "    <reg name=\"d15\" bitsize=\"64\" type=\"ieee_double\" regnum=\"41\"/>\n"
    "    <reg name=\"d16\" bitsize=\"64\" type=\"ieee_double\" regnum=\"42\"/>\n"
    "    <reg name=\"d17\" bitsize=\"64\" type=\"ieee_double\" regnum=\"43\"/>\n"
    "    <reg name=\"d18\" bitsize=\"64\" type=\"ieee_double\" regnum=\"44\"/>\n"
    "    <reg name=\"d19\" bitsize=\"64\" type=\"ieee_double\" regnum=\"45\"/>\n"
    "    <reg name=\"d20\" bitsize=\"64\" type=\"ieee_double\" regnum=\"46\"/>\n"
    "    <reg name=\"d21\" bitsize=\"64\" type=\"ieee_double\" regnum=\"47\"/>\n"
    "    <reg name=\"d22\" bitsize=\"64\" type=\"ieee_double\" regnum=\"48\"/>\n"
    "    <reg name=\"d23\" bitsize=\"64\" type=\"ieee_double\" regnum=\"49\"/>\n"
    "    <reg name=\"d24\" bitsize=\"64\" type=\"ieee_double\" regnum=\"50\"/>\n"
    "    <reg name=\"d25\" bitsize=\"64\" type=\"ieee_double\" regnum=\"51\"/>\n"
    "    <reg name=\"d26\" bitsize=\"64\" type=\"ieee_double\" regnum=\"52\"/>\n"
    "    <reg name=\"d27\" bitsize=\"64\" type=\"ieee_double\" regnum=\"53\"/>\n"
    "    <reg name=\"d28\" bitsize=\"64\" type=\"ieee_double\" regnum=\"54\"/>\n"
    "    <reg name=\"d29\" bitsize=\"64\" type=\"ieee_double\" regnum=\"55\"/>\n"
    "    <reg name=\"d30\" bitsize=\"64\" type=\"ieee_double\" regnum=\"56\"/>\n"
    "    <reg name=\"d31\" bitsize=\"64\" type=\"ieee_double\" regnum=\"57\"/>\n"
    "    <reg name=\"fpscr\" bitsize=\"32\" type=\"int\" regnum=\"58\"/>\n"
    "  </feature>\n"
    "</target>\n";

static inline uint8_t hex_val(char c)
{
    if (c >= '0' && c <= '9') {
        return (uint8_t)(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return (uint8_t)(c - 'a' + 10);
    }
    if (c >= 'A' && c <= 'F') {
        return (uint8_t)(c - 'A' + 10);
    }
    return 0U;
}

static void hex_encode_bytes(char *out, const void *src_ptr, size_t len)
{
    static const char hex[] = "0123456789abcdef";
    const uint8_t *src = (const uint8_t *)src_ptr;
    for (size_t i = 0U; i < len; i++) {
        out[i * 2U]      = hex[(src[i] >> 4U) & 0xFU];
        out[i * 2U + 1U] = hex[src[i] & 0xFU];
    }
}

static void hex_decode_bytes(void *dst_ptr, const char *in, size_t len)
{
    uint8_t *dst = (uint8_t *)dst_ptr;
    for (size_t i = 0U; i < len; i++) {
        if (in[i * 2U] == '\0' || in[i * 2U + 1U] == '\0') {
            break;
        }
        dst[i] = (uint8_t)((hex_val(in[i * 2U]) << 4U) | hex_val(in[i * 2U + 1U]));
    }
}

static void encode_u32_le(char *out, uint32_t val)
{
    hex_encode_bytes(out, &val, sizeof(val));
}

static uint32_t decode_u32_le(const char *in)
{
    uint32_t val = 0U;
    hex_decode_bytes(&val, in, sizeof(val));
    return val;
}

static void encode_u64_le(char *out, uint64_t val)
{
    hex_encode_bytes(out, &val, sizeof(val));
}

static uint64_t decode_u64_le(const char *in)
{
    uint64_t val = 0ULL;
    hex_decode_bytes(&val, in, sizeof(val));
    return val;
}

static ssize_t gdb_read_byte(int fd, char *out)
{
    for (;;) {
        ssize_t r = read(fd, out, 1U);
        if (r < 0 && errno == EINTR) {
            continue;
        }
        return r;
    }
}

static void send_ack(int fd)
{
    const char ack = '+';
    for (;;) {
        ssize_t res = write(fd, &ack, 1U);
        if (res < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
}

static void send_packet(int fd, const char *payload)
{
    if (fd < 0) {
        return;
    }
    size_t len = strlen(payload);
    uint8_t csum = 0U;
    for (size_t i = 0U; i < len; i++) {
        csum = (uint8_t)(csum + (uint8_t)payload[i]);
    }
    char buf[GDB_PKT_BUF_SIZE];
    int n = snprintf(buf, sizeof(buf), "$%s#%02x", payload, (unsigned)csum);
    if (n > 0) {
        size_t total = (size_t)n;
        size_t written = 0U;
        while (written < total) {
            ssize_t w = write(fd, buf + written, total - written);
            if (w < 0 && errno == EINTR) {
                continue;
            }
            if (w <= 0) {
                break;
            }
            written += (size_t)w;
        }
    }
}

static int recv_packet(int fd, char *buf, size_t buf_cap)
{
    size_t pos = 0U;
    bool in_pkt = false;

    for (;;) {
        char ch = 0;
        ssize_t r = gdb_read_byte(fd, &ch);
        if (r <= 0) {
            return -1;
        }
        if (ch == 0x03) {
            buf[0] = 0x03;
            buf[1] = '\0';
            return 1;
        }
        if (!in_pkt) {
            if (ch == '$') {
                in_pkt = true;
                pos = 0U;
            }
            continue;
        }
        if (ch == '#') {
            char csum[2] = {0, 0};
            if (gdb_read_byte(fd, &csum[0]) <= 0 || gdb_read_byte(fd, &csum[1]) <= 0) {
                return -1;
            }
            buf[pos] = '\0';
            send_ack(fd);
            return (int)pos;
        }
        if (pos + 1U < buf_cap) {
            buf[pos++] = ch;
        }
    }
}

static bool g_initial_stop = true;

int a32_gdb_init(a32_ctx_t *ctx, int port)
{
    if (port <= 0) {
        port = 1234;
    }

    ctx->gdb_enabled = false;
    ctx->gdb_port    = port;
    ctx->single_step = false;
    g_initial_stop   = true;

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        fprintf(stderr, "a32rt: failed to create GDB socket: %s\n", strerror(errno));
        return -1;
    }
    (void)fcntl(s, F_SETFD, FD_CLOEXEC);

    int opt = 1;
    if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) != 0) {
        fprintf(stderr, "a32rt: setsockopt SO_REUSEADDR failed: %s\n", strerror(errno));
    }

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family      = AF_INET;
    sin.sin_port        = htons((uint16_t)port);
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(s, (struct sockaddr *)&sin, sizeof(sin)) != 0) {
        fprintf(stderr, "a32rt: failed to bind GDB port %d: %s\n", port, strerror(errno));
        close(s);
        return -1;
    }
    if (listen(s, 1) != 0) {
        fprintf(stderr, "a32rt: failed to listen on GDB socket: %s\n", strerror(errno));
        close(s);
        return -1;
    }

    g_server_sock = s;
    fprintf(stderr, "[a32rt] GDB RSP server listening on 127.0.0.1:%d, waiting for connection...\n", port);

    int client;
    do {
        client = accept(s, NULL, NULL);
    } while (client < 0 && errno == EINTR);
    if (client < 0) {
        fprintf(stderr, "a32rt: GDB accept failed: %s\n", strerror(errno));
        close(s);
        g_server_sock = -1;
        return -1;
    }
    (void)fcntl(client, F_SETFD, FD_CLOEXEC);

    int nodelay = 1;
    setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    g_client_sock    = client;
    ctx->gdb_enabled = true;
    ctx->single_step = true;
    fprintf(stderr, "[a32rt] GDB client connected on 127.0.0.1:%d\n", port);
    return 0;
}

bool a32_gdb_has_breakpoint(uint32_t guest_pc)
{
    uint32_t check_pc = guest_pc & ~1U;
    for (size_t i = 0U; i < GDB_MAX_BREAKPOINTS; i++) {
        if (g_breakpoints[i].active && (g_breakpoints[i].addr & ~1U) == check_pc) {
            return true;
        }
    }
    return false;
}

static bool gdb_range_mapped(const a32_ctx_t *ctx, uint32_t addr, uint32_t len)
{
    if (!ctx || addr < A32_LOW_GUARD_SIZE) {
        return false;
    }
    a32_vm_sync_ctx((a32_ctx_t *)ctx);
    uint64_t end = (uint64_t)addr + (uint64_t)(len == 0U ? 1U : len);
    if (end > 0x100000000ULL) {
        return false;
    }
    uint64_t cur = (uint64_t)addr;
    size_t idx = 0U;
    while (cur < end) {
        if (cur >= (uint64_t)A32_KUSER_BASE_32 &&
            cur < (uint64_t)A32_KUSER_BASE_32 + A32_PAGE_SIZE) {
            cur = (uint64_t)A32_KUSER_BASE_32 + A32_PAGE_SIZE;
            continue;
        }
        while (idx < ctx->region_count && (uint64_t)ctx->regions[idx].end <= cur) {
            idx++;
        }
        if (idx >= ctx->region_count) {
            return false;
        }
        const a32_region_t *r = &ctx->regions[idx];
        if ((r->prot & 0x7U) == 0U || (uint64_t)r->start > cur) {
            return false;
        }
        cur = (uint64_t)r->end;
    }
    return true;
}

static void gdb_disconnect_client(a32_ctx_t *ctx)
{
    if (g_client_sock >= 0) {
        close(g_client_sock);
        g_client_sock = -1;
    }
    ctx->gdb_enabled = false;
    ctx->single_step = false;
    memset(g_breakpoints, 0, sizeof(g_breakpoints));
}

int a32_gdb_handle_stop(a32_ctx_t *ctx, int sig)
{
    if (g_client_sock < 0) {
        return 0;
    }

    char stop_pkt[32];
    snprintf(stop_pkt, sizeof(stop_pkt), "S%02x", (unsigned)sig);
    if (!g_initial_stop) {
        send_packet(g_client_sock, stop_pkt);
    }
    g_initial_stop = false;

    char cmd[GDB_PKT_BUF_SIZE];
    for (;;) {
        int n = recv_packet(g_client_sock, cmd, sizeof(cmd));
        if (n <= 0) {
            gdb_disconnect_client(ctx);
            return 0;
        }

        if (cmd[0] == 0x03) {
            send_packet(g_client_sock, "S02");
            continue;
        }

        if (cmd[0] == '?') {
            send_packet(g_client_sock, stop_pkt);
        } else if (cmd[0] == 'g') {
            /* 16 32-bit GPRs (r0..r15) + cpsr in little-endian hex */
            char resp[17U * 8U + 1U];
            hex_encode_bytes(resp, ctx->regs, sizeof(ctx->regs));
            encode_u32_le(&resp[16U * 8U], ctx->cpsr);
            resp[17U * 8U] = '\0';
            send_packet(g_client_sock, resp);
        } else if (cmd[0] == 'G') {
            /* Write GPRs + cpsr */
            if (strlen(cmd + 1) < 16U * 8U) {
                send_packet(g_client_sock, "E01");
                continue;
            }
            const char *p = cmd + 1;
            hex_decode_bytes(ctx->regs, p, sizeof(ctx->regs));
            p += 16U * 8U;
            if (strlen(p) >= 8U) {
                ctx->cpsr = decode_u32_le(p);
            }
            ctx->itstate = 0U;
            a32_normalize_pc(ctx);
            send_packet(g_client_sock, "OK");
        } else if (cmd[0] == 'p') {
            /* Read single register: 0..15, 25 (0x19) = cpsr, 26..57 = d0..d31, 58 = fpscr */
            char *endp = NULL;
            unsigned reg = (unsigned)strtoul(cmd + 1, &endp, 16);
            if (reg < 16U) {
                char resp[9];
                encode_u32_le(resp, ctx->regs[reg]);
                resp[8] = '\0';
                send_packet(g_client_sock, resp);
            } else if (reg == 25U) {
                char resp[9];
                encode_u32_le(resp, ctx->cpsr);
                resp[8] = '\0';
                send_packet(g_client_sock, resp);
            } else if (reg >= 26U && reg < 58U) {
                char resp[17];
                encode_u64_le(resp, ctx->vfp_d[reg - 26U]);
                resp[16] = '\0';
                send_packet(g_client_sock, resp);
            } else if (reg == 58U) {
                char resp[9];
                encode_u32_le(resp, ctx->fpscr);
                resp[8] = '\0';
                send_packet(g_client_sock, resp);
            } else {
                send_packet(g_client_sock, "00000000");
            }
        } else if (cmd[0] == 'P') {
            /* Write single register: P<n>=<val> */
            char *eq = strchr(cmd + 1, '=');
            if (eq != NULL) {
                *eq = '\0';
                unsigned reg = (unsigned)strtoul(cmd + 1, NULL, 16);
                const char *val_str = eq + 1;
                size_t val_len = strlen(val_str);
                if (reg < 16U && val_len >= 8U) {
                    ctx->regs[reg] = decode_u32_le(val_str);
                    if (reg == 15U) {
                        ctx->itstate = 0U;
                        a32_normalize_pc(ctx);
                    }
                    send_packet(g_client_sock, "OK");
                } else if (reg == 25U && val_len >= 8U) {
                    ctx->cpsr = decode_u32_le(val_str);
                    a32_normalize_pc(ctx);
                    send_packet(g_client_sock, "OK");
                } else if (reg >= 26U && reg < 58U && val_len >= 16U) {
                    ctx->vfp_d[reg - 26U] = decode_u64_le(val_str);
                    send_packet(g_client_sock, "OK");
                } else if (reg == 58U && val_len >= 8U) {
                    ctx->fpscr = decode_u32_le(val_str);
                    send_packet(g_client_sock, "OK");
                } else {
                    send_packet(g_client_sock, "E01");
                }
            } else {
                send_packet(g_client_sock, "E01");
            }
        } else if (cmd[0] == 'm') {
            /* Read guest memory: m<addr>,<len> */
            char *comma = strchr(cmd + 1, ',');
            if (comma != NULL) {
                *comma = '\0';
                unsigned long raw_addr = strtoul(cmd + 1, NULL, 16);
                if (raw_addr > 0xFFFFFFFFUL) {
                    send_packet(g_client_sock, "E14");
                    continue;
                }
                uint32_t addr = (uint32_t)raw_addr;
                size_t len    = (size_t)strtoul(comma + 1, NULL, 16);
                if (len > 1024U) {
                    len = 1024U;
                }
                if (!gdb_range_mapped(ctx, addr, (uint32_t)len)) {
                    send_packet(g_client_sock, "E14");
                    continue;
                }
                char resp[2049];
                hex_encode_bytes(resp, a32_guest_ptr(ctx, addr), len);
                resp[len * 2U] = '\0';
                send_packet(g_client_sock, resp);
            } else {
                send_packet(g_client_sock, "E01");
            }
        } else if (cmd[0] == 'M') {
            /* Write guest memory: M<addr>,<len>:<hex> */
            char *comma = strchr(cmd + 1, ',');
            char *colon = strchr(cmd + 1, ':');
            if (comma != NULL && colon != NULL && comma < colon) {
                *comma = '\0';
                *colon = '\0';
                unsigned long raw_addr = strtoul(cmd + 1, NULL, 16);
                if (raw_addr > 0xFFFFFFFFUL) {
                    send_packet(g_client_sock, "E14");
                    continue;
                }
                uint32_t addr = (uint32_t)raw_addr;
                size_t len    = (size_t)strtoul(comma + 1, NULL, 16);
                if (len > 1024U) {
                    len = 1024U;
                }
                if (strlen(colon + 1) < len * 2U) {
                    send_packet(g_client_sock, "E01");
                    continue;
                }
                if (!gdb_range_mapped(ctx, addr, (uint32_t)len) ||
                    (uint64_t)addr + (uint64_t)len > (uint64_t)A32_KUSER_BASE_32) {
                    send_packet(g_client_sock, "E14");
                    continue;
                }
                if (len > 0U) {
                    uint32_t page_start = a32_page_down(addr);
                    uint32_t page_end   = (uint32_t)a32_page_up((uint64_t)addr + (uint64_t)len);
                    if (mprotect(a32_guest_ptr(ctx, page_start),
                                 (size_t)(page_end - page_start),
                                 PROT_READ | PROT_WRITE) != 0) {
                        send_packet(g_client_sock, "E14");
                        continue;
                    }
                    hex_decode_bytes(a32_guest_ptr(ctx, addr), colon + 1, len);
                    a32_jit_invalidate_range((a32_ctx_t *)ctx, addr, addr + (uint32_t)len);
                    for (uint32_t pg = page_start; pg < page_end; pg += A32_PAGE_SIZE) {
                        int restore_prot = PROT_READ | PROT_WRITE;
                        for (size_t i = 0U; i < ctx->region_count; i++) {
                            const a32_region_t *r = &ctx->regions[i];
                            if (pg >= r->start && pg < r->end) {
                                restore_prot = a32_host_prot(ctx, r->prot);
                                break;
                            }
                        }
                        (void)mprotect(a32_guest_ptr(ctx, pg), A32_PAGE_SIZE, restore_prot);
                    }
                }
                send_packet(g_client_sock, "OK");
            } else {
                send_packet(g_client_sock, "E01");
            }
        } else if ((cmd[0] == 'Z' || cmd[0] == 'z') &&
                   (cmd[1] == '0' || cmd[1] == '1') && cmd[2] == ',') {
            /* Insert (Z) or remove (z) breakpoint: Z0,<addr>,<kind> */
            char *comma1 = strchr(cmd + 3, ',');
            uint32_t len = 4U;
            if (comma1 != NULL) {
                *comma1 = '\0';
                len = (uint32_t)strtoul(comma1 + 1, NULL, 16);
                if (len == 0U) {
                    len = 4U;
                }
            }
            uint32_t addr = (uint32_t)strtoul(cmd + 3, NULL, 16) & ~1U;
            bool ok = true;
            if (cmd[0] == 'Z') {
                ok = a32_gdb_has_breakpoint(addr);
                if (!ok) {
                    for (size_t i = 0U; i < GDB_MAX_BREAKPOINTS; i++) {
                        if (!g_breakpoints[i].active) {
                            g_breakpoints[i].addr   = addr;
                            g_breakpoints[i].active = true;
                            ok = true;
                            break;
                        }
                    }
                }
            } else {
                for (size_t i = 0U; i < GDB_MAX_BREAKPOINTS; i++) {
                    if (g_breakpoints[i].active && (g_breakpoints[i].addr & ~1U) == addr) {
                        g_breakpoints[i].active = false;
                        break;
                    }
                }
            }
            a32_jit_invalidate_range(ctx, addr, addr + len);
            send_packet(g_client_sock, ok ? "OK" : "E01");
        } else if (cmd[0] == 's' || cmd[0] == 'S' || cmd[0] == 'c' || cmd[0] == 'C') {
            /* Step (s/S) or continue (c/C): s[addr], c[addr], Ssig[;addr], Csig[;addr] */
            const char *addr_str = NULL;
            if ((cmd[0] == 's' || cmd[0] == 'c') && cmd[1] != '\0') {
                addr_str = cmd + 1;
            } else if (cmd[0] == 'S' || cmd[0] == 'C') {
                const char *semi = strchr(cmd + 1, ';');
                if (semi != NULL && semi[1] != '\0') {
                    addr_str = semi + 1;
                }
            }
            if (addr_str != NULL) {
                ctx->regs[15] = (uint32_t)strtoul(addr_str, NULL, 16);
                ctx->itstate  = 0U;
                a32_normalize_pc(ctx);
            }
            ctx->single_step = (cmd[0] == 's' || cmd[0] == 'S');
            break;
        } else if (strncmp(cmd, "vCont?", 6) == 0) {
            send_packet(g_client_sock, "vCont;c;C;s;S");
        } else if (strncmp(cmd, "vCont;", 6) == 0) {
            if (strstr(cmd, ";s") != NULL || strstr(cmd, ";S") != NULL) {
                ctx->single_step = true;
                break;
            }
            if (strstr(cmd, ";c") != NULL || strstr(cmd, ";C") != NULL) {
                ctx->single_step = false;
                break;
            }
            send_packet(g_client_sock, "");
        } else if (strncmp(cmd, "qSupported", 10) == 0) {
            send_packet(g_client_sock, "PacketSize=4000;swbreak+;qXfer:features:read+");
        } else if (strncmp(cmd, "qXfer:features:read:target.xml:", 31) == 0) {
            char *comma = strchr(cmd + 31, ',');
            if (comma != NULL) {
                size_t offset = (size_t)strtoul(cmd + 31, NULL, 16);
                size_t len    = (size_t)strtoul(comma + 1, NULL, 16);
                size_t xml_len = sizeof(g_gdb_target_xml) - 1U;
                if (offset >= xml_len || len == 0U) {
                    send_packet(g_client_sock, "l");
                } else {
                    size_t chunk = xml_len - offset;
                    if (chunk > len) {
                        chunk = len;
                    }
                    if (chunk > GDB_PKT_BUF_SIZE - 16U) {
                        chunk = GDB_PKT_BUF_SIZE - 16U;
                    }
                    char resp[GDB_PKT_BUF_SIZE];
                    resp[0] = (offset + chunk >= xml_len) ? 'l' : 'm';
                    memcpy(resp + 1, g_gdb_target_xml + offset, chunk);
                    resp[1U + chunk] = '\0';
                    send_packet(g_client_sock, resp);
                }
            } else {
                send_packet(g_client_sock, "E01");
            }
        } else if (cmd[0] == 'D') {
            /* Detach */
            send_packet(g_client_sock, "OK");
            gdb_disconnect_client(ctx);
            break;
        } else if (cmd[0] == 'k') {
            /* Kill */
            send_packet(g_client_sock, "OK");
            gdb_disconnect_client(ctx);
            if (g_server_sock >= 0) {
                close(g_server_sock);
                g_server_sock = -1;
            }
            exit(0);
        } else {
            /* Unsupported command */
            send_packet(g_client_sock, "");
        }
    }

    return 0;
}
