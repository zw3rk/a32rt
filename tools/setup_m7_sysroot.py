#!/usr/bin/env python3
# tools/setup_m7_sysroot.py — Deterministic Alpine v3.21 armv7 & Debian armhf sysroot builder
#
# Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
# SPDX-License-Identifier: Apache-2.0

import io
import lzma
import os
import shutil
import subprocess
import sys
import tarfile
import urllib.request

ALPINE_MIRROR = "https://dl-cdn.alpinelinux.org/alpine/v3.21"
REPOS = ["main", "community"]

DEBIAN_MIRROR = "http://deb.debian.org/debian"
DEBIAN_GLIBC_PKGS = [
    "libc6",
    "libc6-dev",
    "linux-libc-dev",
    "gcc-12-base",
    "libgcc-s1",
    "libgcc-12-dev",
    "libstdc++6",
    "libstdc++-12-dev",
    "dash",
]

WANTED_PACKAGES = [
    # Toolchain & C/C++ development headers/static libs for local clang & self-hosted g++
    "musl-dev",
    "linux-headers",
    "libgcc",
    "libstdc++",
    "libstdc++-dev",
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

DEFAULT_PKGS = WANTED_PACKAGES

OPTIONAL_HEAVY_PKGS = [
    "gcc",
    "g++",
    "libstdc++-dev",
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


def extract_deb_pure_python(deb_path, dest_dir):
    with open(deb_path, "rb") as f:
        data = f.read()
    if not data.startswith(b"!<arch>\n"):
        raise ValueError(f"{deb_path}: invalid ar magic")
    pos = 8
    n = len(data)
    data_tar_bytes = None
    data_tar_name = None
    while pos + 60 <= n:
        hdr = data[pos : pos + 60]
        name = hdr[0:16].decode("ascii", errors="replace").strip().rstrip("/")
        size_str = hdr[48:58].decode("ascii", errors="replace").strip()
        size = int(size_str)
        payload_start = pos + 60
        payload_end = payload_start + size
        if name.startswith("data.tar"):
            data_tar_name = name
            data_tar_bytes = data[payload_start:payload_end]
            break
        pos = payload_end + (size & 1)

    if data_tar_bytes is None:
        raise ValueError(f"{deb_path}: no data.tar.* member found")

    if data_tar_name.endswith(".xz"):
        mode = "r:xz"
    elif data_tar_name.endswith(".gz"):
        mode = "r:gz"
    elif data_tar_name.endswith(".bz2"):
        mode = "r:bz2"
    else:
        mode = "r:"

    hardlinks = []
    with tarfile.open(fileobj=io.BytesIO(data_tar_bytes), mode=mode) as tf:
        for member in tf.getmembers():
            rel = member.name.lstrip("./")
            if not rel:
                continue
            target_path = os.path.join(dest_dir, rel)
            if member.isdir():
                if os.path.islink(target_path):
                    os.unlink(target_path)
                os.makedirs(target_path, exist_ok=True)
            elif member.isfile():
                os.makedirs(os.path.dirname(target_path), exist_ok=True)
                if os.path.islink(target_path) or (os.path.lexists(target_path) and not os.path.isdir(target_path)):
                    os.unlink(target_path)
                src_f = tf.extractfile(member)
                if src_f is not None:
                    with open(target_path, "wb") as out_f:
                        shutil.copyfileobj(src_f, out_f)
                    os.chmod(target_path, member.mode & 0o777)
            elif member.issym():
                os.makedirs(os.path.dirname(target_path), exist_ok=True)
                if os.path.islink(target_path) or (os.path.lexists(target_path) and not os.path.isdir(target_path)):
                    os.unlink(target_path)
                elif os.path.isdir(target_path):
                    shutil.rmtree(target_path)
                os.symlink(member.linkname, target_path)
            elif member.islnk():
                hardlinks.append((member, target_path))

    for member, target_path in hardlinks:
        link_src = os.path.join(dest_dir, member.linkname.lstrip("./"))
        os.makedirs(os.path.dirname(target_path), exist_ok=True)
        if os.path.islink(target_path) or (os.path.lexists(target_path) and not os.path.isdir(target_path)):
            os.unlink(target_path)
        if os.path.exists(link_src):
            shutil.copy2(link_src, target_path)


def setup_glibc_sysroot(glibc_sysroot_dir, cache_dir=None):
    libc_so = os.path.join(glibc_sysroot_dir, "lib", "arm-linux-gnueabihf", "libc.so.6")
    dash_bin = os.path.join(glibc_sysroot_dir, "bin", "dash")
    ld_interp = os.path.join(glibc_sysroot_dir, "lib", "ld-linux-armhf.so.3")
    cpp_iostream = os.path.join(glibc_sysroot_dir, "usr", "include", "c++", "12", "iostream")
    gcc_crtbegin = os.path.join(glibc_sysroot_dir, "usr", "lib", "gcc", "arm-linux-gnueabihf", "12", "crtbeginS.o")
    if (
        os.path.exists(libc_so)
        and os.path.exists(dash_bin)
        and os.path.exists(ld_interp)
        and os.path.exists(cpp_iostream)
        and os.path.exists(gcc_crtbegin)
    ):
        return

    if cache_dir is None:
        cache_dir = os.path.join(os.path.dirname(os.path.abspath(glibc_sysroot_dir)), "apk-cache")
    os.makedirs(cache_dir, exist_ok=True)
    os.makedirs(glibc_sysroot_dir, exist_ok=True)

    pkg_xz_cache = os.path.join(cache_dir, "debian-bookworm-armhf-Packages.xz")
    if not os.path.exists(pkg_xz_cache):
        url = f"{DEBIAN_MIRROR}/dists/bookworm/main/binary-armhf/Packages.xz"
        print("  [fetch] debian-bookworm-armhf-Packages.xz")
        req = urllib.request.urlopen(url, timeout=60)
        with open(pkg_xz_cache, "wb") as f:
            f.write(req.read())

    with open(pkg_xz_cache, "rb") as f:
        pkg_text = lzma.decompress(f.read()).decode("utf-8", errors="replace")

    wanted_set = set(DEBIAN_GLIBC_PKGS)
    pkg_filenames = {}
    for stanza in pkg_text.split("\n\n"):
        pkg_name = None
        filename = None
        for line in stanza.splitlines():
            if line.startswith("Package: "):
                pkg_name = line[len("Package: ") :].strip()
            elif line.startswith("Filename: "):
                filename = line[len("Filename: ") :].strip()
        if pkg_name in wanted_set and filename:
            pkg_filenames[pkg_name] = filename

    print(f"▸ Installing {len(DEBIAN_GLIBC_PKGS)} Debian bookworm armhf glibc 2.36 packages into {glibc_sysroot_dir}")
    for pkg_name in DEBIAN_GLIBC_PKGS:
        rel_url = pkg_filenames.get(pkg_name)
        if not rel_url:
            raise RuntimeError(f"Debian package {pkg_name} not found in Packages.xz")
        deb_fname = os.path.basename(rel_url)
        deb_cache = os.path.join(cache_dir, deb_fname)
        if not os.path.exists(deb_cache):
            url = f"{DEBIAN_MIRROR}/{rel_url}"
            print(f"  [fetch] {deb_fname}")
            req = urllib.request.urlopen(url, timeout=60)
            with open(deb_cache, "wb") as f:
                f.write(req.read())
        extract_deb_pure_python(deb_cache, glibc_sysroot_dir)

    # Ensure /lib/ld-linux-armhf.so.3 resolves cleanly inside the sysroot
    lib_dir = os.path.join(glibc_sysroot_dir, "lib")
    os.makedirs(lib_dir, exist_ok=True)
    if os.path.lexists(ld_interp):
        os.unlink(ld_interp)
    os.symlink("arm-linux-gnueabihf/ld-linux-armhf.so.3", ld_interp)

    # Ensure /bin/sh -> dash exists
    bin_sh = os.path.join(glibc_sysroot_dir, "bin", "sh")
    if not os.path.lexists(bin_sh):
        os.symlink("dash", bin_sh)

    # Convert any absolute symlinks in usr/lib/arm-linux-gnueabihf and usr/lib/gcc/arm-linux-gnueabihf/12
    # to relative symlinks so host ld.lld resolves them inside glibc_sysroot_dir cleanly.
    for lib_subdir in [
        os.path.join(glibc_sysroot_dir, "usr", "lib", "arm-linux-gnueabihf"),
        os.path.join(glibc_sysroot_dir, "usr", "lib", "gcc", "arm-linux-gnueabihf", "12"),
    ]:
        if os.path.isdir(lib_subdir):
            for entry in os.listdir(lib_subdir):
                full_p = os.path.join(lib_subdir, entry)
                if os.path.islink(full_p):
                    target = os.readlink(full_p)
                    if target.startswith("/"):
                        target_in_sysroot = os.path.join(glibc_sysroot_dir, target.lstrip("/"))
                        rel_target = os.path.relpath(target_in_sysroot, lib_subdir)
                        os.unlink(full_p)
                        os.symlink(rel_target, full_p)

    os.makedirs(os.path.join(glibc_sysroot_dir, "tmp"), exist_ok=True)
    print("▸ Debian bookworm armhf glibc 2.36 sysroot ready")


def main():
    args = sys.argv[1:]
    with_gcc = False
    glibc_sysroot_dir = None

    positional = []
    i = 0
    while i < len(args):
        if args[i] == "--with-gcc":
            with_gcc = True
            i += 1
        elif args[i] == "--glibc-sysroot" and i + 1 < len(args):
            glibc_sysroot_dir = args[i + 1]
            i += 2
        else:
            positional.append(args[i])
            i += 1

    if len(positional) == 1:
        sysroot_dir = positional[0]
        cache_dir = os.path.join(os.path.dirname(os.path.abspath(sysroot_dir)), "apk-cache")
        with_gcc = True
    elif len(positional) >= 2:
        cache_dir = positional[0]
        sysroot_dir = positional[1]
    else:
        print(
            f"Usage: {sys.argv[0]} [<cache-dir>] <sysroot-dir> [--with-gcc] [--glibc-sysroot <dir>]",
            file=sys.stderr,
        )
        sys.exit(1)

    if glibc_sysroot_dir is None:
        glibc_sysroot_dir = os.path.join(
            os.path.dirname(os.path.abspath(sysroot_dir)), "sysroot-glibc-armhf"
        )

    os.makedirs(cache_dir, exist_ok=True)
    os.makedirs(sysroot_dir, exist_ok=True)

    pkgs, provides = load_apkindex(cache_dir)
    wanted = list(WANTED_PACKAGES)
    if with_gcc:
        for hp in OPTIONAL_HEAVY_PKGS:
            if hp not in wanted:
                wanted.append(hp)
    closure = resolve_closure(wanted, pkgs, provides)
    print(f"▸ Installing {len(closure)} Alpine v3.21 armv7 packages into {sysroot_dir}")
    download_and_extract(closure, cache_dir, sysroot_dir)

    # Ensure /tmp exists and /bin/sh points to /bin/busybox
    os.makedirs(os.path.join(sysroot_dir, "tmp"), exist_ok=True)
    bin_sh = os.path.join(sysroot_dir, "bin", "sh")
    if os.path.lexists(bin_sh):
        os.unlink(bin_sh)
    os.symlink("/bin/busybox", bin_sh)
    print("▸ Alpine armv7 M7/M8 sysroot ready")

    setup_glibc_sysroot(glibc_sysroot_dir, cache_dir)


if __name__ == "__main__":
    main()
