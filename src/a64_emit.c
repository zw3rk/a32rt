/*
 * src/a64_emit.c - Direct 32-bit AArch64 instruction encoder implementation
 * (split-TU wrapper; implementation lives in src/core/, included below in order).
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "core/core_emit_alu.c"
#include "core/core_emit_mem.c"
#include "core/core_emit_shared.c"
