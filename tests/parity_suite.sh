#!/bin/sh
# tests/parity_suite.sh — Wave C1 catalogue parity gate for a32rt.
#
# Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
# SPDX-License-Identifier: Apache-2.0
#
# Guest parity across armhf (A32) and thumb (Thumb-2) ABI rows on the
# darwin host translator (_build/a32rt-darwin) and, when HL_PARITY=1 or
# PARITY_TIER=qualify, the hyper-linux leg (../hyper-linux/_build/hl +
# _build/a32rt). Every (guest, engine) pair is compared against committed
# golden files under tests/golden/ (stdout, normalized stderr, exit
# code). AOT round-trip reload and the P3 cached-GPR/FPR signal-frame
# check are explicit rows. Finishes with parity_summary (0 FAIL, 0 GAP).
#
# Usage:
#   sh tests/parity_suite.sh
#   PARITY_UPDATE_GOLDEN=1 sh tests/parity_suite.sh   # (re)generate goldens
#   HL_PARITY=1 sh tests/parity_suite.sh              # + hl legs
#   PARITY_TIER=qualify sh tests/parity_suite.sh      # + hl legs
set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CONTRACT="$(cd "$(dirname -- "$0")/../../rt-series/tools" && pwd)/parity_contract.sh"
# shellcheck disable=SC1090
. "$CONTRACT"
parity_init "a32rt"

RT_DARWIN="$ROOT/_build/a32rt-darwin"
RT_LINUX="$ROOT/_build/a32rt"
HL_BIN="$ROOT/../hyper-linux/_build/hl"
GOLDEN_DIR="$ROOT/tests/golden"
BENCH_ARMHF="$ROOT/../rt-series/_build/bench/bench-armhf"

mkdir -p "$GOLDEN_DIR"

# Whether to run the hyper-linux legs in addition to the darwin legs.
DO_HL=0
if [ "${HL_PARITY:-0}" = "1" ] || [ "${PARITY_TIER:-smoke}" = "qualify" ]; then
    DO_HL=1
fi

# Normalize bench timing (us/ops_sec/total_us vary run to run); CRCs stay.
norm_bench() {
    sed -E -e 's/us=[0-9]+/us=N/g' -e 's/ops_sec=[0-9]+/ops_sec=N/g' \
        -e 's/total_us=[0-9]+/total_us=N/g' "$1" > "$2"
}

# Normalize difftest step counts (hello lockstep length varies 396/405
# with stack placement); the faulting PC and exit reasons are exact.
norm_difftest_err() {
    sed -E -e 's/step=[0-9]+/step=N/g' "$1" > "$2"
}

# run_guest <abi> <host> <case> <engine-flag> <engine-name> <guest> <golden-base> [guest-arg]
# Host-specific goldens: hl legs use `<base>.hl.<engine>.*` (the hl
# filesystem makes test-m1 PASS where darwin returns 21), darwin legs
# use `<base>.<engine>.*`.
run_guest() {
    _abi="$1"; _host="$2"; _case="$3"; _engflag="$4"; _eng="$5"; _guest="$6"; _gold="$7"
    _garg="${8:-}"
    if [ "$_host" = "hl" ]; then
        _gold="$_gold.hl"
    fi
    _gold_out="$GOLDEN_DIR/$_gold.$_eng.out"
    _gold_code="$GOLDEN_DIR/$_gold.$_eng.code"
    _gold_err="$GOLDEN_DIR/$_gold.$_eng.err"
    _tmp_out=$(mktemp /tmp/a32parity_out.XXXXXX)
    _tmp_err=$(mktemp /tmp/a32parity_err.XXXXXX)
    _tmp_nout=$(mktemp /tmp/a32parity_nout.XXXXXX)
    _tmp_nerr=$(mktemp /tmp/a32parity_nerr.XXXXXX)
    _tmp_code=$(mktemp /tmp/a32parity_code.XXXXXX)
    _t0=$(parity_now_ms)
    _rc=0
    if [ "$_host" = "hl" ]; then
        if [ -n "$_garg" ]; then
            # shellcheck disable=SC2086
            perl -e 'alarm 15; exec @ARGV' "$HL_BIN" "$RT_LINUX" "$_engflag" -- "$_guest" "$_garg" >"$_tmp_out" 2>"$_tmp_err" || _rc=$?
        else
            perl -e 'alarm 15; exec @ARGV' "$HL_BIN" "$RT_LINUX" "$_engflag" -- "$_guest" >"$_tmp_out" 2>"$_tmp_err" || _rc=$?
        fi
    else
        if [ -n "$_garg" ]; then
            # shellcheck disable=SC2086
            perl -e 'alarm 15; exec @ARGV' "$RT_DARWIN" "$_engflag" -- "$_guest" "$_garg" >"$_tmp_out" 2>"$_tmp_err" || _rc=$?
        else
            perl -e 'alarm 15; exec @ARGV' "$RT_DARWIN" "$_engflag" -- "$_guest" >"$_tmp_out" 2>"$_tmp_err" || _rc=$?
        fi
    fi
    _t1=$(parity_now_ms)
    _ms=0
    if [ "$_t0" != "0" ] && [ "$_t1" != "0" ]; then
        _ms=$((_t1 - _t0))
    fi
    # Normalize stdout for bench (timing) and stderr for hl harness + difftest steps.
    case "$_case" in
        bench-*) norm_bench "$_tmp_out" "$_tmp_nout" ;;
        *) cp "$_tmp_out" "$_tmp_nout" ;;
    esac
    parity_norm_stderr "$_tmp_err" "$_tmp_nerr.tmp"
    case "$_eng" in
        difftest) norm_difftest_err "$_tmp_nerr.tmp" "$_tmp_nerr" ;;
        *) cp "$_tmp_nerr.tmp" "$_tmp_nerr" ;;
    esac
    rm -f "$_tmp_nerr.tmp"
    printf '%s' "$_rc" > "$_tmp_code"
    _st="PASS"
    _det=""
    parity_check_golden "$_gold_out" "$_tmp_nout"; _rc_cmp=$?
    if [ "$_rc_cmp" -eq 2 ]; then
        _st="GAP"; _det="missing golden $_gold.$_eng.out"
    elif [ "$_rc_cmp" -ne 0 ]; then
        _st="FAIL"; _det="stdout differs"
    fi
    if [ "$_st" = "PASS" ]; then
        parity_check_golden "$_gold_code" "$_tmp_code"; _rc_cmp=$?
        if [ "$_rc_cmp" -eq 2 ]; then
            _st="GAP"; _det="missing golden $_gold.$_eng.code"
        elif [ "$_rc_cmp" -ne 0 ]; then
            _st="FAIL"; _det="exit $_rc differs"
        fi
    fi
    if [ "$_st" = "PASS" ]; then
        parity_check_golden "$_gold_err" "$_tmp_nerr"; _rc_cmp=$?
        if [ "$_rc_cmp" -eq 2 ]; then
            _st="GAP"; _det="missing golden $_gold.$_eng.err"
        elif [ "$_rc_cmp" -ne 0 ]; then
            _st="FAIL"; _det="stderr differs"
        fi
    fi
    parity_row "$_abi" "$_host" "$_case" "$_eng" "$_st" "$_ms" "$_det"
    rm -f "$_tmp_out" "$_tmp_err" "$_tmp_nout" "$_tmp_nerr" "$_tmp_code"
}

# --- structural gate: make check (line limits, x18/push-pop/VLA bans) ---
_t0=$(parity_now_ms)
if make -C "$ROOT" --no-print-directory check >/tmp/a32_check.log 2>&1; then
    _st="PASS"; _det=""
else
    _st="FAIL"; _det="make check failed"
fi
_t1=$(parity_now_ms)
_ms=0
if [ "$_t0" != "0" ] && [ "$_t1" != "0" ]; then
    _ms=$((_t1 - _t0))
fi
parity_row "host" "darwin" "make-check" "check" "$_st" "$_ms" "$_det"

# --- guest matrix (darwin legs always; hl legs when DO_HL=1) ---
# Map fixture names to ABI rows. test_m1_isa_fs -> test-m1-armhf (A32);
# test_m4_vfp_neon -> test-m4-armhf (A32) + test-m4-thumb (Thumb-2).
GUESTS_ARMHF="hello-armhf-static test-m1-armhf test-m4-armhf"
GUESTS_THUMB="hello-thumb-static test-m4-thumb"
# armel (soft-float ABI): integer-only fixtures; VFP/NEON (m4) stays
# hard-float-only (needs compiler __aeabi_fp helpers — see Makefile).
GUESTS_ARMEL="hello-armel-static test-m1-armel"
# Thumb interp pins: every Thumb guest starts with bic.w in _start, which
# the T32 data-processing unit does not implement yet, so interp UNDEFs
# (132, A32_EXIT_UNDEF=4 in difftest lines) at step 2 while dbt — whose
# own T32 DP lowering handles bic.w — proceeds.  Before the
# hw2[15:14]==10 branch-guard fix these rows pinned 139 wild-branch
# faults at the same PCs; the goldens now pin the clean UNDEF instead.
# test-m1-thumb is a second Thumb-2 row for the m1 ISA suite.
if [ -x "$ROOT/_build/test-m1-thumb" ]; then
    GUESTS_THUMB="$GUESTS_THUMB test-m1-thumb"
fi

for _g in $GUESTS_ARMHF; do
    if [ ! -x "$ROOT/_build/$_g" ]; then
        parity_row "armhf" "darwin" "$_g" "dbt" "GAP" "0" "missing guest $_g"
        continue
    fi
    for _e in "--dbt:dbt" "--interp:interp" "--difftest:difftest"; do
        _flag="${_e%%:*}"; _name="${_e##*:}"
        run_guest "armhf" "darwin" "$_g" "$_flag" "$_name" "$ROOT/_build/$_g" "$_g"
    done
done
# test-m3-armhf (p4 threads/signals): interp+dbt goldens only.  The
# lockstep difftest cannot gate it (threads + async signals do not
# compose with 1:1 lockstep: signal delivery races the stepper), so no
# difftest row — cf. the catalogue's "where single-thread/supported".
for _g in test-m3-armhf; do
    if [ ! -x "$ROOT/_build/$_g" ]; then
        parity_row "armhf" "darwin" "$_g" "dbt" "GAP" "0" "missing guest $_g"
        continue
    fi
    for _e in "--dbt:dbt" "--interp:interp"; do
        _flag="${_e%%:*}"; _name="${_e##*:}"
        run_guest "armhf" "darwin" "$_g" "$_flag" "$_name" "$ROOT/_build/$_g" "$_g"
    done
done
# Uniform libc guests (A32 static musl, `make guests-libc`): p3_torture +
# tcg smoke from ../rt-series/tests.  Interp dies in musl startup (A32
# startup-code gap, silent UNDEF/132).  DBT survives startup but faults in
# signal test 1 (wild PC into rodata — open signal-path gap, native
# ALL_OK).  tcg-smoke hangs past the banner under dbt (bounded below with
# a 10 s alarm; native finishes in 0.01 s).  No difftest rows:
# threaded+signals do not compose with lockstep (same rationale as m3).
if [ -x "$ROOT/_build/p3-torture-a32" ]; then
    run_guest "armhf" "darwin" "p3-torture-a32" "--dbt" "dbt" "$ROOT/_build/p3-torture-a32" "p3-torture-a32"
    run_guest "armhf" "darwin" "p3-torture-a32" "--interp" "interp" "$ROOT/_build/p3-torture-a32" "p3-torture-a32"
else
    parity_row "armhf" "darwin" "p3-torture-a32" "dbt" "GAP" "0" "missing guest (make guests-libc)"
fi
# run_alarm_guest: run_guest shape for guests that hang under an engine.
# Bounds the run with a perl alarm; the golden pins the partial output
# and the alarm rc (142).  Args: abi host case secs path golden [args...].
run_alarm_guest() {
    _abi="$1"; _host="$2"; _case="$3"; _secs="$4"; _path="$5"; _gold="$6"
    shift 6
    _t0=$(parity_now_ms)
    _tmp_out=$(mktemp /tmp/a32alm.XXXXXX); _tmp_err=$(mktemp /tmp/a32alm.XXXXXX); _tmp_code=$(mktemp /tmp/a32alm.XXXXXX)
    perl -e 'alarm $ARGV[0]; exec @ARGV[1..$#ARGV]' "$_secs" "$RT_DARWIN" --dbt -- "$_path" "$@" >"$_tmp_out" 2>"$_tmp_err"; _rc=$?
    printf '%s' "$_rc" > "$_tmp_code"
    _t1=$(parity_now_ms)
    _ms=0
    if [ "$_t0" != "0" ] && [ "$_t1" != "0" ]; then
        _ms=$((_t1 - _t0))
    fi
    _st="PASS"; _det=""
    parity_check_golden "$GOLDEN_DIR/$_gold.dbt.out" "$_tmp_out"; _rc_cmp=$?
    if [ "$_rc_cmp" -eq 2 ]; then
        _st="GAP"; _det="missing golden $_gold.dbt.out"
    elif [ "$_rc_cmp" -ne 0 ]; then
        _st="FAIL"; _det="stdout differs"
    fi
    if [ "$_st" = "PASS" ]; then
        parity_check_golden "$GOLDEN_DIR/$_gold.dbt.code" "$_tmp_code"; _rc_cmp=$?
        if [ "$_rc_cmp" -eq 2 ]; then
            _st="GAP"; _det="missing golden $_gold.dbt.code"
        elif [ "$_rc_cmp" -ne 0 ]; then
            _st="FAIL"; _det="exit $_rc differs"
        fi
    fi
    if [ "$_st" = "PASS" ]; then
        uniq "$GOLDEN_DIR/$_gold.dbt.err" > "$_tmp_err.gold" 2>/dev/null || true
        uniq "$_tmp_err" > "$_tmp_err.act" 2>/dev/null || true
        parity_check_golden "$_tmp_err.gold" "$_tmp_err.act"; _rc_cmp=$?
        rm -f "$_tmp_err.gold" "$_tmp_err.act"
        if [ "$_rc_cmp" -eq 2 ]; then
            _st="GAP"; _det="missing golden $_gold.dbt.err"
        elif [ "$_rc_cmp" -ne 0 ]; then
            _st="FAIL"; _det="stderr differs"
        fi
    fi
    parity_row "$_abi" "$_host" "$_case" "dbt" "$_st" "$_ms" "$_det"
    rm -f "$_tmp_out" "$_tmp_err" "$_tmp_code"
}
if [ -x "$ROOT/_build/tcg-smoke-a32" ]; then
    run_guest "armhf" "darwin" "tcg-smoke-a32" "--interp" "interp" "$ROOT/_build/tcg-smoke-a32" "tcg-smoke-a32" "4096"
    # tcg-smoke dbt hangs: bound with a 2 s alarm, golden pins the
    # partial (banner-only) output + alarm rc.
    run_alarm_guest "armhf" "darwin" "tcg-smoke-a32" "2" "$ROOT/_build/tcg-smoke-a32" "tcg-smoke-a32" "4096"
else
    parity_row "armhf" "darwin" "tcg-smoke-a32" "dbt" "GAP" "0" "missing guest (make guests-libc)"
fi
# h1_smc: freestanding SMC (mprotect + patch + cacheflush + exec).  All
# engines agree, so the lockstep difftest row is included.
if [ -x "$ROOT/_build/test-h1-armhf" ]; then
    for _e in "--dbt:dbt" "--interp:interp" "--difftest:difftest"; do
        _flag="${_e%%:*}"; _name="${_e##*:}"
        run_guest "armhf" "darwin" "h1_smc" "$_flag" "$_name" "$ROOT/_build/test-h1-armhf" "h1_smc"
    done
else
    parity_row "armhf" "darwin" "h1_smc" "dbt" "GAP" "0" "missing guest test-h1-armhf"
fi
# p2_libc dynamic leg (m7, musl) via A32RT_SYSROOT: interp golden pins the
# ldso-startup UNDEF (132); dbt hangs after musl-thread-startup faults
# (open thread-startup gap) — 2 s alarm row.  No difftest (threads).
if [ -x "$ROOT/_build/test-m7-armhf" ]; then
    A32RT_SYSROOT="$ROOT/_build/sysroot-armv7" run_guest "armhf" "darwin" "test-m7-armhf" "--interp" "interp" "$ROOT/_build/test-m7-armhf" "test-m7-armhf"
    A32RT_SYSROOT="$ROOT/_build/sysroot-armv7" run_alarm_guest "armhf" "darwin" "test-m7-armhf" "2" "$ROOT/_build/test-m7-armhf" "test-m7-armhf"
else
    parity_row "armhf" "darwin" "test-m7-armhf" "dbt" "GAP" "0" "missing guest test-m7-armhf"
fi
# p5_dyn (m8 glibc 2.36): dbt PASS via the sysroot; interp 132
# (ldso-startup gap).  No difftest (NPTL threads + dlopen).
if [ -x "$ROOT/_build/test-m8-glibc-armhf" ]; then
    A32RT_SYSROOT="$ROOT/_build/sysroot-glibc-armhf" run_guest "armhf" "darwin" "test-m8-glibc-armhf" "--dbt" "dbt" "$ROOT/_build/test-m8-glibc-armhf" "test-m8-glibc-armhf"
    A32RT_SYSROOT="$ROOT/_build/sysroot-glibc-armhf" run_guest "armhf" "darwin" "test-m8-glibc-armhf" "--interp" "interp" "$ROOT/_build/test-m8-glibc-armhf" "test-m8-glibc-armhf"
else
    parity_row "armhf" "darwin" "test-m8-glibc-armhf" "dbt" "GAP" "0" "missing guest test-m8-glibc-armhf"
fi
# h4_cxx (m8 C++17 ARM EHABI, musl): dbt faults in runtime init (open
# libstdc++-init gap, native PASS); interp 132 (ldso-startup gap).
# No difftest (std::thread).
if [ -x "$ROOT/_build/test-m8-cpp-armhf" ]; then
    A32RT_SYSROOT="$ROOT/_build/sysroot-armv7" run_guest "armhf" "darwin" "test-m8-cpp-armhf" "--dbt" "dbt" "$ROOT/_build/test-m8-cpp-armhf" "test-m8-cpp-armhf"
    A32RT_SYSROOT="$ROOT/_build/sysroot-armv7" run_guest "armhf" "darwin" "test-m8-cpp-armhf" "--interp" "interp" "$ROOT/_build/test-m8-cpp-armhf" "test-m8-cpp-armhf"
else
    parity_row "armhf" "darwin" "test-m8-cpp-armhf" "dbt" "GAP" "0" "missing guest test-m8-cpp-armhf"
fi
for _g in $GUESTS_THUMB; do
    if [ ! -x "$ROOT/_build/$_g" ]; then
        parity_row "thumb" "darwin" "$_g" "dbt" "GAP" "0" "missing guest $_g"
        continue
    fi
    for _e in "--dbt:dbt" "--interp:interp" "--difftest:difftest"; do
        _flag="${_e%%:*}"; _name="${_e##*:}"
        run_guest "thumb" "darwin" "$_g" "$_flag" "$_name" "$ROOT/_build/$_g" "$_g"
    done
done
for _g in $GUESTS_ARMEL; do
    if [ ! -x "$ROOT/_build/$_g" ]; then
        parity_row "armel" "darwin" "$_g" "dbt" "GAP" "0" "missing guest $_g"
        continue
    fi
    for _e in "--dbt:dbt" "--interp:interp" "--difftest:difftest"; do
        _flag="${_e%%:*}"; _name="${_e##*:}"
        run_guest "armel" "darwin" "$_g" "$_flag" "$_name" "$ROOT/_build/$_g" "$_g"
    done
done
if [ -x "$BENCH_ARMHF" ]; then
    for _e in "--dbt:dbt" "--interp:interp" "--difftest:difftest"; do
        _flag="${_e%%:*}"; _name="${_e##*:}"
        run_guest "armhf" "darwin" "bench-armhf-smoke" "$_flag" "$_name" "$BENCH_ARMHF" "bench-armhf-smoke" "--smoke"
    done
fi

if [ "$DO_HL" -eq 1 ]; then
    if [ ! -x "$HL_BIN" ] || [ ! -x "$RT_LINUX" ]; then
        parity_row "armhf" "hl" "hl-leg" "dbt" "GAP" "0" "missing hl or linux translator"
    else
        for _g in $GUESTS_ARMHF; do
            for _e in "--dbt:dbt" "--interp:interp" "--difftest:difftest"; do
                _flag="${_e%%:*}"; _name="${_e##*:}"
                run_guest "armhf" "hl" "$_g" "$_flag" "$_name" "$ROOT/_build/$_g" "$_g"
            done
        done
        for _g in $GUESTS_THUMB; do
            for _e in "--dbt:dbt" "--interp:interp" "--difftest:difftest"; do
                _flag="${_e%%:*}"; _name="${_e##*:}"
                run_guest "thumb" "hl" "$_g" "$_flag" "$_name" "$ROOT/_build/$_g" "$_g"
            done
        done
        if [ -x "$BENCH_ARMHF" ]; then
            for _e in "--dbt:dbt" "--interp:interp" "--difftest:difftest"; do
                _flag="${_e%%:*}"; _name="${_e##*:}"
                run_guest "armhf" "hl" "bench-armhf-smoke" "$_flag" "$_name" "$BENCH_ARMHF" "bench-armhf-smoke" "--smoke"
            done
        fi
    fi
fi

# --- AOT cache round-trip reload check (DBT, output stability) ---
_t0=$(parity_now_ms)
_AOTDIR=$(mktemp -d /tmp/a32aot_parity.XXXXXX)
_AOT1=$(mktemp /tmp/a32aot1.XXXXXX)
_AOT2=$(mktemp /tmp/a32aot2.XXXXXX)
perl -e 'alarm 15; exec @ARGV' "$RT_DARWIN" --cache "$_AOTDIR" --auto-aot -- "$ROOT/_build/hello-armhf-static" >"$_AOT1" 2>/dev/null
_rc1=$?
perl -e 'alarm 15; exec @ARGV' "$RT_DARWIN" --cache "$_AOTDIR" --auto-aot -- "$ROOT/_build/hello-armhf-static" >"$_AOT2" 2>/dev/null
_rc2=$?
_t1=$(parity_now_ms)
_ms=0
if [ "$_t0" != "0" ] && [ "$_t1" != "0" ]; then
    _ms=$((_t1 - _t0))
fi
if [ "$_rc1" -eq 0 ] && [ "$_rc2" -eq 0 ] && cmp -s "$_AOT1" "$_AOT2" && [ "$(cat "$_AOT1")" = "hello" ]; then
    parity_row "armhf" "darwin" "aot-reload" "dbt" "PASS" "$_ms" ""
else
    parity_row "armhf" "darwin" "aot-reload" "dbt" "FAIL" "$_ms" "rc=$_rc1/$_rc2"
fi
rm -rf "$_AOTDIR" "$_AOT1" "$_AOT2"

# --- P3 cached-GPR/FPR signal-frame check ---
# Structural: side-table recovery (saved_regs/saved_vfp_d) plus VFP
# sigframe (fpregs/fpscr) in src/signals.c.  Behavioral parity lives in
# the test-m3-armhf run_engine interp/dbt rows below (dbt currently
# outruns interp: open futex wait-queues + NEON interp gaps; the lockstep
# difftest cannot gate threaded+signalled guests, so m3 has goldens but
# no difftest row — cf. the catalogue's "where single-thread/supported").
_t0=$(parity_now_ms)
_P3DET=""
if ! grep -q "saved_regs" "$ROOT/src/signals.c"; then
    _P3DET="signals.c lacks saved_regs"
elif ! grep -q "saved_vfp_d" "$ROOT/src/signals.c"; then
    _P3DET="signals.c lacks saved_vfp_d"
elif ! grep -q "fpregs" "$ROOT/src/signals.c"; then
    _P3DET="signals.c lacks VFP fpregs"
fi
_t1=$(parity_now_ms)
_ms=0
if [ "$_t0" != "0" ] && [ "$_t1" != "0" ]; then
    _ms=$((_t1 - _t0))
fi
if [ -z "$_P3DET" ]; then
    parity_row "armhf" "darwin" "p3-signal-frame" "dbt" "PASS" "$_ms" ""
else
    parity_row "armhf" "darwin" "p3-signal-frame" "dbt" "FAIL" "$_ms" "$_P3DET"
fi

parity_summary
