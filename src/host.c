/*
 * src/host.c — Optional /dev/a32host handshake & capability detection
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#define _GNU_SOURCE
#include "a32rt.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

void a32_host_probe(a32_ctx_t *ctx)
{
    if (!ctx) {
        return;
    }
    memset(&ctx->host_info, 0, sizeof(ctx->host_info));
    ctx->has_host_info = false;

    const char *host_env = getenv("A32RT_HOST");
    if (host_env && strcmp(host_env, "none") == 0) {
        return;
    }

    int fd = open(A32H_DEV_PATH, O_RDWR | O_CLOEXEC);
    if (fd < 0 && (errno == EACCES || errno == EPERM || errno == EROFS)) {
        fd = open(A32H_DEV_PATH, O_RDONLY | O_CLOEXEC);
    }
    if (fd < 0) {
        return;
    }

    a32h_info_t info;
    memset(&info, 0, sizeof(info));
    if (syscall(SYS_ioctl, fd, (unsigned long)A32H_GET_INFO, &info) == 0 &&
        info.abi_major == A32H_ABI_MAJOR) {
        ctx->host_info = info;
        ctx->host_info.aot_socket[sizeof(ctx->host_info.aot_socket) - 1U] = '\0';
        ctx->has_host_info = true;
    }

    close(fd);
}
