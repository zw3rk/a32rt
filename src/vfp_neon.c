/*
 * src/vfp_neon.c - VFPv4-D32 & NEON (ASIMD) decoder & A64 lowering
 * (split-TU wrapper; implementation lives in src/interp/, included below in order).
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "interp/interp_vfp_core.c"
#include "interp/interp_neon_mem.c"
#include "interp/interp_neon_alu.c"
