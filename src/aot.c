/*
 * src/aot.c - Persistent disk-backed AOT cache (.a32cache, SHA-256 keyed)
 * (split-TU wrapper; implementation lives in src/dbt/, included below in order).
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "dbt/dbt_aot_cache.c"
#include "dbt/dbt_aot_scan.c"
