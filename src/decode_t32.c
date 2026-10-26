/*
 * src/decode_t32.c - T32 (Thumb-1 / Thumb-2) decoder & A64 lowering
 * (split-TU wrapper; implementation lives in src/interp/, included below in order).
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "interp/interp_t16.c"
#include "interp/interp_t32_dp.c"
#include "interp/interp_t32_mem.c"
