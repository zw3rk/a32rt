/*
 * src/dbt/aot/aot.h -- AOT memory-site table API (Wave U3 A32-4)
 *
 * Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
 * SPDX-License-Identifier: Apache-2.0
 *
 * UNIFICATION-PLAN.md Rev 4, sections 4.3.3 and 7: the DBT serializes its
 * relative a32_vm32_mem_site_t recovery table into the AOT block payload
 * so a warm cache keeps precise fault recovery after process restart and
 * rebasing. The on-disk record is 100% position-independent (block-base
 * relative offsets only, magic + version + checksum; corrupt, truncated,
 * or foreign-version payloads are rejected, never partially applied).
 *
 * This is the site-table layer only; block code/relocation persistence
 * stays in src/aot.c (a32_aot_*). No symbol overlap by construction:
 * every symbol here carries the a32_aot_site_ prefix. Curated C11.
 */

#ifndef A32RT_DBT_AOT_H
#define A32RT_DBT_AOT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vm/vm32.h"

#define A32_AOT_SITE_MAGIC 0x41335453U /* "A3TS" */
#define A32_AOT_SITE_VERSION 1U

/* Serialize count sites into a freshly allocated buffer (magic +
 * version + count + records + checksum). Returns 0 on success.
 * The caller frees *out_buf. Rejects count == 0 and NULL sites. */
int a32_aot_site_encode(const a32_vm32_mem_site_t *sites, uint32_t count,
    uint8_t **out_buf, size_t *out_len);

/* Validate and decode a buffer produced by a32_aot_site_encode into the
 * caller-supplied sites array (cap entries). On success returns 0 with
 * *out_count records; on magic/version/checksum/truncation mismatch
 * returns -1 with *out_count == 0 and no partial output. */
int a32_aot_site_decode(const uint8_t *buf, size_t len,
    a32_vm32_mem_site_t *sites, uint32_t cap, uint32_t *out_count);

/* Persist/restore one block payload keyed by guest pc range. Restore
 * rejects foreign payloads the same way decode does. */
int a32_aot_site_save(const char *dir, uint32_t guest_pc_base,
    const a32_vm32_mem_site_t *sites, uint32_t count);
int a32_aot_site_load(const char *dir, uint32_t guest_pc_base,
    a32_vm32_mem_site_t *sites, uint32_t cap, uint32_t *out_count);

#endif /* A32RT_DBT_AOT_H */
