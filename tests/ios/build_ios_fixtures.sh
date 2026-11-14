#!/bin/sh
# tests/ios/build_ios_fixtures.sh — build genuine 32-bit iOS fixtures.
#
# Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
# SPDX-License-Identifier: Apache-2.0
#
# Builds all fixtures cleanly into _build/ios-tests/:
#   test-ios-static, test-ios-thin-v7, test-ios-thin-v7s, test-ios-fat,
#   test-ios-bsd-mach, libcalc_ios.dylib, test-ios-dylib,
#   test-ios-signals-threads, rpath_sub/librpath_ios.dylib, test-ios-proc-net
#
# Toolchain (macOS):
#   clang -target armv7-apple-ios9.0 -Wno-incompatible-sysroot
#   ld -arch armv7 -static -e _start | -pie -ios_version_min 9.0 | -dylib
#   lipo -create
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
OUT="$ROOT/_build/ios-tests"
SRC="$ROOT/tests/ios"
TBD="$SRC/libSystem.tbd"

if [ "$(uname)" != "Darwin" ]; then
    echo "build_ios_fixtures.sh: requires Darwin (got $(uname))" >&2
    exit 1
fi
command -v clang >/dev/null 2>&1 || { echo "missing clang" >&2; exit 1; }
command -v ld >/dev/null 2>&1 || { echo "missing ld" >&2; exit 1; }
command -v lipo >/dev/null 2>&1 || { echo "missing lipo" >&2; exit 1; }

mkdir -p "$OUT"

CFLAGS="-Wno-incompatible-sysroot -std=c11 -Werror -Werror=vla -pedantic -Wall -Wextra -O2 -fno-stack-protector -ffreestanding -nostdlib"

echo "▸ test-ios-static (LC_UNIXTHREAD MH_EXECUTE)"
# shellcheck disable=SC2086
clang -target armv7-apple-ios9.0 $CFLAGS -c "$SRC/test_ios_static.c" -o "$OUT/.test-ios-static.o"
ld -arch armv7 -static -e _start "$OUT/.test-ios-static.o" -o "$OUT/test-ios-static"

echo "▸ test-ios-thin-v7 (LC_MAIN MH_EXECUTE armv7)"
# shellcheck disable=SC2086
clang -target armv7-apple-ios9.0 $CFLAGS -DIOS_ARCH_TAG=7 -c "$SRC/test_ios_fat.c" -o "$OUT/.thin-v7.o"
ld -arch armv7 -pie -ios_version_min 9.0 "$OUT/.thin-v7.o" "$TBD" -o "$OUT/test-ios-thin-v7"

echo "▸ test-ios-thin-v7s (LC_MAIN MH_EXECUTE armv7s)"
# shellcheck disable=SC2086
clang -target armv7s-apple-ios9.0 $CFLAGS -DIOS_ARCH_TAG=75 -c "$SRC/test_ios_fat.c" -o "$OUT/.thin-v7s.o"
ld -arch armv7s -pie -ios_version_min 9.0 "$OUT/.thin-v7s.o" "$TBD" -o "$OUT/test-ios-thin-v7s"

echo "▸ test-ios-fat (universal FAT_MAGIC)"
lipo -create "$OUT/test-ios-thin-v7" "$OUT/test-ios-thin-v7s" -output "$OUT/test-ios-fat"

echo "▸ test-ios-bsd-mach (LC_MAIN MH_EXECUTE)"
# shellcheck disable=SC2086
clang -target armv7-apple-ios9.0 $CFLAGS -c "$SRC/test_ios_bsd_mach.c" -o "$OUT/.bsd-mach.o"
ld -arch armv7 -pie -ios_version_min 9.0 "$OUT/.bsd-mach.o" "$TBD" -o "$OUT/test-ios-bsd-mach"

echo "▸ libcalc_ios.dylib (MH_DYLIB)"
# shellcheck disable=SC2086
clang -target armv7-apple-ios9.0 $CFLAGS -c "$SRC/libcalc_ios.c" -o "$OUT/.libcalc.o"
ld -arch armv7 -dylib -ios_version_min 9.0 -install_name @executable_path/libcalc_ios.dylib "$OUT/.libcalc.o" "$TBD" -o "$OUT/libcalc_ios.dylib"

echo "▸ test-ios-dylib (LC_MAIN MH_EXECUTE + libcalc)"
# shellcheck disable=SC2086
clang -target armv7-apple-ios9.0 $CFLAGS -c "$SRC/test_ios_dylib.c" -o "$OUT/.dylib-exe.o"
ld -arch armv7 -pie -ios_version_min 9.0 "$OUT/.dylib-exe.o" "$OUT/libcalc_ios.dylib" "$TBD" -o "$OUT/test-ios-dylib"

echo "▸ test-ios-signals-threads (LC_MAIN MH_EXECUTE)"
# shellcheck disable=SC2086
clang -target armv7-apple-ios9.0 $CFLAGS -c "$SRC/test_ios_signals_threads.c" -o "$OUT/.sigthr.o"
ld -arch armv7 -pie -ios_version_min 9.0 "$OUT/.sigthr.o" "$TBD" -o "$OUT/test-ios-signals-threads"

echo "▸ rpath_sub/librpath_ios.dylib (MH_DYLIB @rpath)"
mkdir -p "$OUT/rpath_sub"
# shellcheck disable=SC2086
clang -target armv7-apple-ios9.0 $CFLAGS -c "$SRC/librpath_ios.c" -o "$OUT/.rpath.o"
ld -arch armv7 -dylib -ios_version_min 9.0 -install_name @rpath/librpath_ios.dylib "$OUT/.rpath.o" "$TBD" -o "$OUT/rpath_sub/librpath_ios.dylib"

echo "▸ test-ios-proc-net (LC_MAIN MH_EXECUTE + rpath)"
# shellcheck disable=SC2086
clang -target armv7-apple-ios9.0 $CFLAGS -c "$SRC/test_ios_proc_net.c" -o "$OUT/.proc-net.o"
ld -arch armv7 -pie -ios_version_min 9.0 -rpath @executable_path/rpath_sub "$OUT/.proc-net.o" "$OUT/rpath_sub/librpath_ios.dylib" "$TBD" -o "$OUT/test-ios-proc-net"

rm -f "$OUT"/.thin-v7.o "$OUT"/.thin-v7s.o "$OUT"/.bsd-mach.o \
    "$OUT"/.libcalc.o "$OUT"/.dylib-exe.o "$OUT"/.sigthr.o \
    "$OUT"/.test-ios-static.o "$OUT"/.rpath.o "$OUT"/.proc-net.o

echo "ok: $OUT"
file "$OUT"/test-ios-static "$OUT"/test-ios-thin-v7 "$OUT"/test-ios-thin-v7s \
    "$OUT"/test-ios-fat "$OUT"/test-ios-bsd-mach "$OUT"/libcalc_ios.dylib \
    "$OUT"/test-ios-dylib "$OUT"/test-ios-signals-threads \
    "$OUT"/rpath_sub/librpath_ios.dylib "$OUT"/test-ios-proc-net
