/*
 * src/os/backend_sync.c -- Shared worker wake-pipe + signal-sequence helper
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host-independent nonblocking wake-pipe and sig_seq protocol shared by
 * the Linux and Darwin backends (A32-R13-11): one copy so the two
 * backends cannot diverge on quiescence. Curated C11.
 */

#include "os/backend.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

int a32_backend_worker_sync_init(a32_worker_sync_t *ws)
{
    int fds[2];
    if (ws == NULL) {
        return -EINVAL;
    }
    memset(ws, 0, sizeof(*ws));
    ws->wake_pipe_rd = -1;
    ws->wake_pipe_wr = -1;
    if (pipe(fds) != 0) {
        return -errno;
    }
    if (fcntl(fds[0], F_SETFL, O_NONBLOCK) != 0 ||
        fcntl(fds[1], F_SETFL, O_NONBLOCK) != 0) {
        int e = errno;
        close(fds[0]);
        close(fds[1]);
        return -e;
    }
    ws->wake_pipe_rd = fds[0];
    ws->wake_pipe_wr = fds[1];
    return 0;
}

void a32_backend_worker_sync_destroy(a32_worker_sync_t *ws)
{
    if (ws == NULL) {
        return;
    }
    if (ws->wake_pipe_rd >= 0) {
        close(ws->wake_pipe_rd);
    }
    if (ws->wake_pipe_wr >= 0) {
        close(ws->wake_pipe_wr);
    }
    ws->wake_pipe_rd = -1;
    ws->wake_pipe_wr = -1;
}

void a32_backend_worker_notify_signal(a32_worker_sync_t *ws)
{
    const char b = 0;
    if (ws == NULL || ws->wake_pipe_wr < 0) {
        return;
    }
    atomic_fetch_add(&ws->sig_seq, 1U);
    (void)write(ws->wake_pipe_wr, &b, 1U);
}
