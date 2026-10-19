#!/usr/bin/env python3
# tools/setup_m7_sysroot.py — Deterministic Alpine v3.21 armv7 & Debian armhf sysroot builder
#
# Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
# SPDX-License-Identifier: Apache-2.0

import io
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request

ALPINE_MIRROR = "https://dl-cdn.alpinelinux.org/alpine/v3.21"
REPOS = ["main", "community"]

DEFAULT_PKGS = [
    # Toolchain & C development headers/static libs for local clang cross-compilation
    "musl-dev",
    "linux-headers",
    "libgcc",
    "libstdc++",
    # Core shells & CLI utilities (hyper-linux test-static-bins / test-coreutils parity)
    "bash",
    "dash",
    "coreutils",
    "grep",
    "sed",
    "gawk",
    "findutils",
    "diffutils",
    "tree",
    "jq",
    "sqlite",
    # Scripting, JIT & language runtimes
    "lua5.4",
    "luajit",
    "python3",
    # Crypto, compression & version control
    "openssl",
    "zstd",
    "xz",
    "git",
]

OPTIONAL_HEAVY_PKGS = [
    "gcc",
    "binutils",
]


def load_apkindex(cache_dir):
    pkgs = {}
    provides = {}
    for repo in REPOS:
        idx_cache = os.path.join(cache_dir, f"APKINDEX-{repo}.tar.gz")
        if not os.path.exists(idx_cache):
            url = f"{ALPINE_MIRROR}/{repo}/armv7/APKINDEX.tar.gz"
            req = urllib.request.urlopen(url, timeout=30)
            data = req.read()
            with open(idx_cache, "wb") as f:
                f.write(data)
        with tarfile.open(idx_cache, mode="r:gz") as tf:
            content = tf.extractfile("APKINDEX").read().decode("utf-8", errors="replace")
        for blk in content.split("\n\n"):
            fields = {}
            for line in blk.splitlines():
                if ":" in line:
                    k, v = line.split(":", 1)
                    fields[k] = v
            if "P" not in fields or "V" not in fields:
                continue
            name = fields["P"]
            deps = fields.get("D", "").split()
            provs = fields.get("p", "").split()
            pkgs[name] = {
                "name": name,
                "version": fields["V"],
                "repo": repo,
                "deps": deps,
            }
            provides[name] = name
            for p in provs:
                pname = p.split("=")[0].split("<")[0].split(">")[0]
                provides.setdefault(pname, name)
    return pkgs, provides


def resolve_closure(root_names, pkgs, provides):
    resolved = []
    seen = set()
    queue = list(root_names)
    while queue:
        tok = queue.pop(0)
        if tok.startswith("!"):
            continue
        dep_key = tok.split("=")[0].split("<")[0].split(">")[0].split("~")[0]
        pkg_name = provides.get(dep_key, dep_key)
        if pkg_name in seen or pkg_name not in pkgs:
            continue
        seen.add(pkg_name)
        resolved.append(pkgs[pkg_name])
        for d in pkgs[pkg_name]["deps"]:
            queue.append(d)
    return resolved


def download_and_extract(pkg_list, cache_dir, sysroot_dir):
    marker_dir = os.path.join(sysroot_dir, ".m7-installed")
    os.makedirs(marker_dir, exist_ok=True)
    for pkg in pkg_list:
        fname = f"{pkg['name']}-{pkg['version']}.apk"
        marker = os.path.join(marker_dir, fname)
        if os.path.exists(marker):
            continue
        apk_path = os.path.join(cache_dir, fname)
        if not os.path.exists(apk_path):
            url = f"{ALPINE_MIRROR}/{pkg['repo']}/armv7/{fname}"
            print(f"  [fetch] {fname}")
            req = urllib.request.urlopen(url, timeout=60)
            with open(apk_path, "wb") as f:
                f.write(req.read())
        subprocess.run(
            ["tar", "-xzf", apk_path, "-C", sysroot_dir],
            check=True,
            stderr=subprocess.DEVNULL,
        )
        with open(marker, "w") as f:
            f.write("ok\n")


def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <cache-dir> <sysroot-dir> [--with-gcc]", file=sys.stderr)
        sys.exit(1)
    cache_dir = sys.argv[1]
    sysroot_dir = sys.argv[2]
    with_gcc = "--with-gcc" in sys.argv[3:]

    os.makedirs(cache_dir, exist_ok=True)
    os.makedirs(sysroot_dir, exist_ok=True)

    pkgs, provides = load_apkindex(cache_dir)
    wanted = list(DEFAULT_PKGS)
    if with_gcc:
        wanted.extend(OPTIONAL_HEAVY_PKGS)
    closure = resolve_closure(wanted, pkgs, provides)
    print(f"▸ Installing {len(closure)} Alpine v3.21 armv7 packages into {sysroot_dir}")
    download_and_extract(closure, cache_dir, sysroot_dir)

    # Ensure /tmp exists and /bin/sh points to /bin/busybox
    os.makedirs(os.path.join(sysroot_dir, "tmp"), exist_ok=True)
    bin_sh = os.path.join(sysroot_dir, "bin", "sh")
    if os.path.lexists(bin_sh):
        os.unlink(bin_sh)
    os.symlink("/bin/busybox", bin_sh)
    print("▸ Alpine armv7 M7 sysroot ready")


if __name__ == "__main__":
    main()
