/*
 * src/syscalls.c - ARM EABI (r7, r0..r5) -> aarch64-linux syscall & struct shim
 * (split-TU wrapper; implementation lives in src/os/, included below in order).
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "os/os_sysnr_tables.c"
#include "os/os_sys_path.c"
#include "os/os_sys_fork_time.c"
#include "os/os_sys_thread_sig.c"
#include "os/os_sys_sig_time.c"
#include "os/os_sys_sock.c"
#include "os/os_sys_fs_arm.c"
