/*
 * src/ios/ios_svc_bsd_kevent.c — iOS BSD kqueue / kevent / kevent64 / kevent_qos
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ios/ios.h"

int a32_ios_dispatch_bsd_kevent(a32_ctx_t *ctx, uint32_t bsd_nr,
    const uint32_t *args, bool *out_handled)
{
    (void)ctx;
    (void)bsd_nr;
    (void)args;
    if (out_handled != NULL) {
        *out_handled = false;
    }
    return 0;
}

void a32_ios_kq_atfork_reset(void)
{
}
