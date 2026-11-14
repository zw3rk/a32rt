#!/bin/sh
# tools/run_ios_suite.sh — 32-bit iOS (armv7/armv7s) qualification suite.
#
# Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
# SPDX-License-Identifier: Apache-2.0
#
# Runs tests/ios fixtures on _build/a32rt-darwin (--interp, --dbt, and
# --difftest with 0 divergences) and under the static-PIE Linux _build/a32rt
# in Docker (--interp, --dbt, --difftest with 0 divergences, and --aot +
# warm cached --dbt on 4 KiB-page Regime A1).
set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT="$ROOT/_build/ios-tests"
DARWIN_BIN="$ROOT/_build/a32rt-darwin"
LINUX_BIN="$ROOT/_build/a32rt"

PASS=0
FAIL=0

note_pass() {
    PASS=$((PASS + 1))
    echo "PASS $1"
}

note_fail() {
    FAIL=$((FAIL + 1))
    echo "FAIL $1"
}

ALL_GUESTS="test-ios-static test-ios-thin-v7 test-ios-thin-v7s test-ios-fat test-ios-bsd-mach test-ios-dylib test-ios-signals-threads test-ios-proc-net"
SINGLE_GUESTS="test-ios-static test-ios-thin-v7 test-ios-thin-v7s test-ios-fat test-ios-bsd-mach test-ios-dylib"

# Build fixtures on Darwin when missing or older than sources.
if [ "$(uname)" = "Darwin" ]; then
    NEED_BUILD=0
    for g in $ALL_GUESTS; do
        if [ ! -x "$OUT/$g" ]; then
            NEED_BUILD=1
        fi
    done
    if [ "$NEED_BUILD" = "0" ] && [ -n "$(find "$ROOT/tests/ios" -newer "$OUT/test-ios-static" 2>/dev/null)" ]; then
        NEED_BUILD=1
    fi
    if [ "$NEED_BUILD" = "1" ]; then
        echo "▸ building iOS fixtures"
        sh "$ROOT/tests/ios/build_ios_fixtures.sh" || exit 1
    fi
fi

run_darwin() {
    _mode="$1"
    _guest="$2"
    _label="$3"
    shift 3
    _out=$(mktemp /tmp/a32ios_out.XXXXXX)
    _rc=0
    if [ "${_AOT_CACHE_DIR:-}" != "" ]; then
        timeout --kill-after=30s 60s "$DARWIN_BIN" --cache "$_AOT_CACHE_DIR" "$_mode" -- "$_guest" "$_guest" "$@" >"$_out" 2>&1 || _rc=$?
    else
        timeout --kill-after=30s 60s "$DARWIN_BIN" "$_mode" -- "$_guest" "$_guest" "$@" >"$_out" 2>&1 || _rc=$?
    fi
    if [ "$_rc" -eq 0 ]; then
        note_pass "darwin $_mode $_label"
    else
        note_fail "darwin $_mode $_label (rc=$_rc)"
        head -n 20 "$_out"
    fi
    rm -f "$_out"
}

run_difftest_darwin() {
    _guest="$1"
    _label="$2"
    shift 2
    _out=$(mktemp /tmp/a32ios_diff.XXXXXX)
    _rc=0
    timeout --kill-after=30s 60s "$DARWIN_BIN" --difftest -- "$_guest" "$_guest" "$@" >"$_out" 2>&1 || _rc=$?
    if [ "$_rc" -eq 0 ]; then
        if grep -qiE "divergence[^0]|mismatch|DIFF.*[1-9]" "$_out"; then
            note_fail "darwin difftest $_label (divergences reported)"
            head -n 20 "$_out"
        else
            note_pass "darwin difftest $_label (0 divergences)"
        fi
    else
        note_fail "darwin difftest $_label (rc=$_rc)"
        head -n 20 "$_out"
    fi
    rm -f "$_out"
}

if [ -x "$DARWIN_BIN" ]; then
    echo "▸ darwin legs: $DARWIN_BIN"
    for g in $ALL_GUESTS; do
        run_darwin --interp "$OUT/$g" "$g"
        run_darwin --dbt "$OUT/$g" "$g"
    done
    for g in $SINGLE_GUESTS; do
        run_difftest_darwin "$OUT/$g" "$g"
    done
    run_difftest_darwin "$OUT/test-ios-signals-threads" "test-ios-signals-threads(--no-threads)" --no-threads
    run_difftest_darwin "$OUT/test-ios-proc-net" "test-ios-proc-net(--no-fork)" --no-fork
    _sys_out=$(mktemp /tmp/a32ios_sys.XXXXXX)
    _sys_rc=0
    timeout --kill-after=30s 60s "$DARWIN_BIN" --sysroot "$OUT" --dbt -- "$OUT/test-ios-static" /test-ios-static >"$_sys_out" 2>&1 || _sys_rc=$?
    if [ "$_sys_rc" -eq 0 ] && grep -q "PASS test_ios_static" "$_sys_out"; then
        note_pass "darwin --dbt --sysroot test-ios-static-as-/test-ios-static"
    else
        note_fail "darwin --dbt --sysroot test-ios-static-as-/test-ios-static (rc=$_sys_rc)"
        head -n 20 "$_sys_out"
    fi
    rm -f "$_sys_out"
else
    echo "skip: $DARWIN_BIN not present" >&2
fi

# Linux Docker leg: same binaries under static-PIE _build/a32rt in alpine:3.20.
DO_DOCKER=0
if [ "${RUN_LINUX_DOCKER:-}" = "1" ]; then
    DO_DOCKER=1
elif command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
    DO_DOCKER=1
fi
if [ "$DO_DOCKER" = "1" ] && [ -x "$LINUX_BIN" ]; then
    echo "▸ linux docker legs: $LINUX_BIN"
    CNAME="a32rt-ios-suite-$$"
    # shellcheck disable=SC2064
    trap "docker rm -f $CNAME >/dev/null 2>&1 || true" EXIT INT TERM
    _DLOG=$(mktemp /tmp/a32ios_dock.XXXXXX)
    _DRC=0
    timeout --kill-after=30s 180s docker run --rm --name "$CNAME" --platform linux/arm64 \
        -v "$ROOT:/work" -w /work alpine:3.20 sh -eu -c '
        BIN=/work/_build/a32rt
        OUT=/work/_build/ios-tests
        CACHE=/tmp/a32ios_aot_cache
        mkdir -p "$CACHE"
        for g in '"$ALL_GUESTS"'; do
            "$BIN" --interp -- "$OUT/$g" >/tmp/out 2>&1
            echo "DOCKER_PASS --interp $g"
            "$BIN" --dbt -- "$OUT/$g" >/tmp/out 2>&1
            echo "DOCKER_PASS --dbt $g"
        done
        for g in '"$SINGLE_GUESTS"'; do
            "$BIN" --difftest -- "$OUT/$g" >/tmp/out 2>&1
            if grep -qiE "divergence[^0]|mismatch|DIFF.*[1-9]" /tmp/out; then
                cat /tmp/out >&2
                exit 1
            fi
            echo "DOCKER_PASS --difftest $g"
        done
        "$BIN" --difftest -- "$OUT/test-ios-signals-threads" "$OUT/test-ios-signals-threads" --no-threads >/tmp/out 2>&1
        if grep -qiE "divergence[^0]|mismatch|DIFF.*[1-9]" /tmp/out; then
            cat /tmp/out >&2
            exit 1
        fi
        echo "DOCKER_PASS --difftest test-ios-signals-threads(--no-threads)"
        "$BIN" --difftest -- "$OUT/test-ios-proc-net" "$OUT/test-ios-proc-net" --no-fork >/tmp/out 2>&1
        if grep -qiE "divergence[^0]|mismatch|DIFF.*[1-9]" /tmp/out; then
            cat /tmp/out >&2
            exit 1
        fi
        echo "DOCKER_PASS --difftest test-ios-proc-net(--no-fork)"
        "$BIN" --sysroot "$OUT" --dbt -- "$OUT/test-ios-static" /test-ios-static >/tmp/out 2>&1
        if ! grep -q "PASS test_ios_static" /tmp/out; then
            cat /tmp/out >&2
            exit 1
        fi
        echo "DOCKER_PASS --dbt --sysroot test-ios-static-as-/test-ios-static"
        PAGESZ=$(getconf PAGESIZE 2>/dev/null || echo 4096)
        if [ "$PAGESZ" = "4096" ]; then
            for g in test-ios-static test-ios-fat test-ios-bsd-mach test-ios-dylib test-ios-proc-net; do
                "$BIN" --cache "$CACHE" --aot -- "$OUT/$g" >/tmp/out 2>&1
                echo "DOCKER_PASS --aot $g"
                "$BIN" --cache "$CACHE" --dbt -- "$OUT/$g" >/tmp/out 2>&1
                echo "DOCKER_PASS --dbt+aot-cache $g"
            done
            ls "$CACHE"/a32rt-cache-*.bin >/dev/null 2>&1
            echo "DOCKER_PASS aot-cache-files-verified"
        else
            echo "DOCKER_SKIP aot-regime-a2 pagesize=$PAGESZ"
        fi
    ' >"$_DLOG" 2>&1 || _DRC=$?
    while IFS= read -r line; do
        case "$line" in
            DOCKER_PASS\ *)
                note_pass "linux-docker ${line#DOCKER_PASS }"
                ;;
            DOCKER_SKIP\ *)
                echo "INFO ${line#DOCKER_SKIP }"
                ;;
        esac
    done <"$_DLOG"
    if [ "$_DRC" -ne 0 ]; then
        note_fail "linux-docker batch (rc=$_DRC)"
        tail -n 30 "$_DLOG"
    fi
    rm -f "$_DLOG"
    trap - EXIT INT TERM
    docker rm -f "$CNAME" >/dev/null 2>&1 || true
elif [ "$DO_DOCKER" = "1" ]; then
    echo "skip: $LINUX_BIN not present" >&2
fi

echo "summary: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
