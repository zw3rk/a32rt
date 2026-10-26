/*
 * src/jit.c - Code cache, RW chain slots, IBTC, W^X batch mprotect, dispatcher
 * (split-TU wrapper; implementation lives in src/vm/, included below in order).
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "vm/vm_jit_mem.c"
#include "vm/vm_jit_state.c"
#include "dbt/dbt_jit_query.c"
#include "dbt/dbt_jit_run.c"
