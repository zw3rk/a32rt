/*
 * src/signals_frame.h — Guest sigframe layout & delivery for arm32 rt_sigframe
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared by src/signals.c (handler, syscalls) and src/signals_frame.c
 * (frame build/restore). Split out so src/signals.c stays under the
 * 900-line limit.
 */

#ifndef A32RT_SIGNALS_FRAME_H
#define A32RT_SIGNALS_FRAME_H

#include "a32rt.h"

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef SI_TKILL
#define SI_TKILL (-6)
#endif
#ifndef SI_KERNEL
#define SI_KERNEL 0x80
#endif

#define A32_SIGFRAME_MAGIC 0x53494746U /* "SIGF" */
#define A32_VFP_MAGIC      0x56465001U /* VFP magic */
#define A32_FPEXC_EN       0x40000000U /* FPEXC_EN */

#define A32_SIG_DFL      0U
#define A32_SIG_IGN      1U

#define A32_SA_NOCLDSTOP 0x00000001U
#define A32_SA_NOCLDWAIT 0x00000002U
#define A32_SA_SIGINFO   0x00000004U
#define A32_SA_RESTORER  0x04000000U
#define A32_SA_ONSTACK   0x08000000U
#define A32_SA_RESTART   0x10000000U
#define A32_SA_NODEFER   0x40000000U
#define A32_SA_RESETHAND 0x80000000U

#define A32_SS_ONSTACK   1U
#define A32_SS_DISABLE   2U

#define A32_UNMASKABLE_SIGNALS ((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)))

typedef struct __attribute__((packed, aligned(4))) guest_sigaction {
    uint32_t handler;
    uint32_t flags;
    uint32_t restorer;
    uint64_t mask;
} guest_sigaction_t;

_Static_assert(sizeof(guest_sigaction_t) == 20, "guest_sigaction_t must be 20 bytes");

typedef struct a32_siginfo {
    int32_t  si_signo;
    int32_t  si_errno;
    int32_t  si_code;
    uint32_t _pad[29];
} a32_siginfo_t;

/* Build the guest siginfo image (shared by deliver and sigwait paths). */
void a32_frame_fill_siginfo(a32_siginfo_t *dst, int sig,
                            const siginfo_t *host_info,
                            uint32_t fault_addr, bool for_sigwait);

/* Build an rt_sigframe on the guest (alt)stack and redirect ctx at handler. */
void a32_frame_deliver(a32_ctx_t *ctx, int sig, guest_sigaction_t *act,
                       const siginfo_t *host_info);

#endif /* A32RT_SIGNALS_FRAME_H */
