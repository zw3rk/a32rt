#!/bin/bash
# tests/test_m7_ecosystem.sh — M7 real-world armv7 application ecosystem suite
# Runs inside the armv7 Alpine sysroot under /bin/bash via a32rt.
#
# Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

export PATH="/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"
export HOME="/tmp"

WORK="/tmp/a32rt_m7_eco_$$"
rm -rf "$WORK"
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT

# 1. bash & dash
dash_out="$(dash -c 'x=17; y=25; echo $((x + y))')"
[ "$dash_out" = "42" ] || { echo "FAIL: dash ($dash_out)"; exit 1; }

# 2. uutils-coreutils, grep, sed, gawk, find, diff, tree
printf "gamma\nalpha\nbeta\nalpha\n" | sort | uniq -c | sed -E 's/^[[:space:]]+//' > "$WORK/sorted.txt"
grep -q "^2 alpha$" "$WORK/sorted.txt" || { echo "FAIL: sort/uniq/sed/grep"; exit 1; }

awk_out="$(printf "a 10.5\nb 20.25\na 4.25\n" | gawk '{ sum[$1] += $2 } END { printf "%.2f:%.2f", sum["a"], sum["b"] }')"
[ "$awk_out" = "14.75:20.25" ] || { echo "FAIL: gawk ($awk_out)"; exit 1; }

mkdir -p "$WORK/tree_dir/sub1" "$WORK/tree_dir/sub2"
echo "one" > "$WORK/tree_dir/sub1/f1.txt"
echo "two" > "$WORK/tree_dir/sub2/f2.txt"
found_cnt="$(find "$WORK/tree_dir" -name '*.txt' | wc -l | tr -d ' ')"
[ "$found_cnt" = "2" ] || { echo "FAIL: find ($found_cnt)"; exit 1; }

tree_out="$(tree -f -i --noreport "$WORK/tree_dir")"
echo "$tree_out" | grep -q "f1.txt" || { echo "FAIL: tree"; exit 1; }

if diff -u "$WORK/tree_dir/sub1/f1.txt" "$WORK/tree_dir/sub2/f2.txt" > "$WORK/diff.patch"; then
    echo "FAIL: diff should return non-zero on differing files"
    exit 1
fi
grep -q "^-one$" "$WORK/diff.patch" && grep -q "^+two$" "$WORK/diff.patch" || { echo "FAIL: diff content"; exit 1; }

# 3. jq
jq_out="$(printf '{"items":[{"k":"a","v":3},{"k":"b","v":7},{"k":"c","v":5}]}' | jq -r '[.items[] | select(.v >= 5) | .k] | join(",")')"
[ "$jq_out" = "b,c" ] || { echo "FAIL: jq ($jq_out)"; exit 1; }

# 4. sqlite3
sql_out="$(sqlite3 "$WORK/test.db" "
CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT, score REAL);
INSERT INTO t (name, score) VALUES ('alice', 91.5), ('bob', 84.0), ('carol', 96.5);
SELECT name, CAST(ROUND(AVG(score) OVER ()) AS INTEGER) FROM t WHERE score > 90 ORDER BY score DESC;
PRAGMA integrity_check;
")"
printf "%s\n" "$sql_out" | grep -q "^carol|94$" || { echo "FAIL: sqlite3 ($sql_out)"; exit 1; }
printf "%s\n" "$sql_out" | grep -q "^ok$" || { echo "FAIL: sqlite3 integrity_check"; exit 1; }

# 5. lua5.4
lua_out="$(lua5.4 -e '
local co = coroutine.create(function()
    local s = 0
    for i = 1, 100 do s = s + i end
    coroutine.yield(s)
end)
local ok, res = coroutine.resume(co)
assert(ok and res == 5050)
print("LUA54_OK:" .. tostring(res))
')"
[ "$lua_out" = "LUA54_OK:5050" ] || { echo "FAIL: lua5.4 ($lua_out)"; exit 1; }

# 6. luajit (with ARM32 hardware JIT enabled)
luajit_out="$(luajit -e '
assert(jit and jit.status() == true, "LuaJIT JIT must be enabled")
local acc = 0
for i = 1, 100000 do
    acc = bit.bxor(acc + i, bit.lshift(i, 1))
end
print("LUAJIT_OK:" .. tostring(acc))
')"
[ "$luajit_out" = "LUAJIT_OK:675631440" ] || { echo "FAIL: luajit ($luajit_out)"; exit 1; }

# 7. openssl, zstd, xz
printf "The quick brown fox jumps over the lazy dog 0123456789\n" > "$WORK/plain.txt"
openssl enc -aes-256-cbc -pbkdf2 -iter 1000 -pass pass:a32rt_secret_key -in "$WORK/plain.txt" -out "$WORK/cipher.bin"
openssl enc -d -aes-256-cbc -pbkdf2 -iter 1000 -pass pass:a32rt_secret_key -in "$WORK/cipher.bin" -out "$WORK/dec.txt"
cmp -s "$WORK/plain.txt" "$WORK/dec.txt" || { echo "FAIL: openssl aes-256-cbc roundtrip"; exit 1; }

zstd -T1 -q -f "$WORK/plain.txt" -o "$WORK/plain.txt.zst"
zstd -T1 -d -q -f "$WORK/plain.txt.zst" -o "$WORK/plain.unzst"
cmp -s "$WORK/plain.txt" "$WORK/plain.unzst" || { echo "FAIL: zstd roundtrip"; exit 1; }

xz -T1 -z -k -f "$WORK/plain.txt"
xz -T1 -d -c "$WORK/plain.txt.xz" > "$WORK/plain.unxz"
cmp -s "$WORK/plain.txt" "$WORK/plain.unxz" || { echo "FAIL: xz roundtrip"; exit 1; }

# 8. git
mkdir -p "$WORK/repo"
(
    cd "$WORK/repo"
    git init -q -b main
    git config user.email "test@zw3rk.com"
    git config user.name "a32rt test"
    echo "hello git" > README.md
    git add README.md
    git commit -q -m "Initial commit"
    log_msg="$(git log -1 --pretty=%s)"
    [ "$log_msg" = "Initial commit" ] || { echo "FAIL: git log ($log_msg)"; exit 1; }
)

# 9. python3.12
py_out="$(python3 -B -c '
import hashlib, json, math, re, zlib
data = {"pi": round(math.pi, 6), "fib": [1, 1, 2, 3, 5, 8, 13, 21]}
raw = json.dumps(data, sort_keys=True).encode("utf-8")
comp = zlib.compress(raw, level=6)
assert zlib.decompress(comp) == raw
h = hashlib.sha256(raw).hexdigest()[:16]
assert re.match(r"^[0-9a-f]{16}$", h)
print(f"PYTHON312_OK:{h}")
')"
[ "$py_out" = "PYTHON312_OK:b5abbd2c04884798" ] || { echo "FAIL: python3 ($py_out)"; exit 1; }

# 10. Self-hosted gcc + binutils (cc1, as, collect2, ld, readelf, nm, strip)
cat << 'EOF' > "$WORK/selfhost.c"
#include <stdio.h>
#include <math.h>

static int fib(int n) {
    int a = 0, b = 1;
    for (int i = 0; i < n; i++) {
        int t = a + b;
        a = b;
        b = t;
    }
    return a;
}

int main(void) {
    printf("GCC_SELFHOST_OK:%d:%.3f\n", fib(20), sqrt(2.0));
    return 0;
}
EOF

gcc -O2 "$WORK/selfhost.c" -lm -o "$WORK/selfhost.elf"
readelf -h "$WORK/selfhost.elf" | grep -q "Machine:.*ARM" || { echo "FAIL: readelf"; exit 1; }
nm -D "$WORK/selfhost.elf" | grep -q "printf" || { echo "FAIL: nm"; exit 1; }
strip "$WORK/selfhost.elf"
self_out="$("$WORK/selfhost.elf")"
[ "$self_out" = "GCC_SELFHOST_OK:6765:1.414" ] || { echo "FAIL: selfhost ($self_out)"; exit 1; }

echo "M7 Alpine armv7 Ecosystem (bash, dash, coreutils, grep, sed, gawk, find, diff, tree, jq, sqlite3, lua5.4, luajit, openssl, zstd, xz, git, python3.12, self-hosted gcc+binutils): PASS"
