# Golden outputs for a32rt catalogue parity (`tests/parity_suite.sh`)
#
# Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
# SPDX-License-Identifier: Apache-2.0
#
# Per-(guest, engine) snapshots captured on darwin-arm64 via
# `PARITY_UPDATE_GOLDEN=1 sh tests/parity_suite.sh`:
#   `<guest>.<engine>.out`  — guest stdout (bench timing normalized to `us=N`)
#   `<guest>.<engine>.code` — guest exit code (`printf '%s' "$rc"`, no newline)
#   `<guest>.<engine>.err`  — normalized stderr (`hl:` lines stripped,
#                              difftest `step=N` normalized)
#
# Guests: `hello-armhf-static` (A32), `hello-thumb-static` (Thumb-2),
# `test-m1-armhf` / `test-m1-thumb` (`test_m1_isa_fs`), `test-m4-armhf` /
# `test-m4-thumb` (`test_m4_vfp_neon`), `bench-armhf-smoke`
# (`../rt-series/_build/bench/bench-armhf --smoke`, timing normalized,
# CRCs exact). Engines: `dbt`, `interp`, `difftest`. HL legs
# (`HL_PARITY=1` / `PARITY_TIER=qualify`) use `<guest>.hl.<engine>.*`
# (the hl filesystem makes `test-m1` PASS where darwin returns 21).
#
# Known engine gaps captured here (not hidden): `--interp` currently
# UNDEFs on `mrc p15, 0` (CP15 TPIDRURO, hello `0xee1d1f70`) and on early
# Thumb-2 / VFP sequences, exiting 132/139 with empty stdout; `--difftest`
# reports the same divergence (`rc_interp=4 rc_dbt=1`). DBT legs carry the
# correct `hello` / M4 PASS output. `test-m1-armhf` exits 21 on darwin
# (getdents64 on the darwin host) identically on all three engines;
# `test-m3-armhf` (P3 row) exits 138 identically on both engines.
# Regenerate with `PARITY_UPDATE_GOLDEN=1`; never hand-edit.
