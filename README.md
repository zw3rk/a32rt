# `a32rt` — AArch32 (`arm32` / `armhf`) → AArch64 (`aarch64-linux`) User-Mode Binary Translator

`a32rt` is a standalone, zero-dependency, Rosetta-style user-mode dynamic (JIT) and ahead-of-time (AOT) binary translator that runs 32-bit ARM (`armv7l` / `armhf`, A32 and Thumb-2, VFPv4-D32, and NEON ASIMD) Linux ELF binaries on 64-bit AArch64 Linux systems whose CPUs lack hardware 32-bit EL0 execution support (such as Apple Silicon M1–M4 and modern AArch64 server processors).

It builds as a single static-PIE `aarch64-linux` ELF executable (`_build/a32rt`) written in Curated C11 and minimal AArch64 assembly, loads an `arm32` ELF and its optional `PT_INTERP` dynamic linker (`ld-musl-armhf.so.1` or `ld-linux-armhf.so.3`) into a reserved 4 GiB guest virtual address window `[G, G + 4 GiB)`, and issues only standard `aarch64-linux` syscalls.

For the full architectural specification, register allocation, and design rationale, see [`A32RT.md`](A32RT.md) and [`docs/HOST-CONTRACT.md`](docs/HOST-CONTRACT.md).

---

## Key Features

- **Zero External Dependencies**: Written in strict C11 (`-std=c11 -Werror -Werror=vla -pedantic -Wall -Wextra`) and AArch64 assembly (`src/trampoline.S`). Uses a bespoke A32/T32 decoder, direct 32-bit AArch64 instruction encoder (`src/a64_emit.c`), runtime code cache, and EABI syscall/signal layer.
- **Parametric 4 GiB Guest Window (`[G, G + 4 GiB)`)**:
  - **Identity mode (`G = 0`)**: On plain AArch64 Linux when `[0x00010000, 0xffff0000)` is free, guest addresses equal host addresses.
  - **Offset mode (`G = 0x1_0000_0000`)**: Under [`hyper-linux`](../hyper-linux) on macOS Apple Silicon (where `[0, 4 GiB)` is reserved by macOS `PAGEZERO`), `a32rt` holds `G` in reserved host register `x20` (`xG`) and lowers all guest memory accesses via `[xG, wAddr, uxtw]`, preserving exact 32-bit address wrap-around (`mod 2^32`).
- **Complete ARMv7-A + ARMv8-A AArch32 User-Mode ISA + VFPv4-D32 + NEON (ASIMD) + Crypto**:
  - **A32 & Thumb-2 (`T16`/`T32`)**: Data processing, barrel shifter, multiply/accumulate, hardware divide (`SDIV`/`UDIV`), saturating/DSP/parallel byte & halfword media instructions, bitfield ops, load/store word/byte/halfword/doubleword/multiple (`LDM`/`STM`, `PUSH`/`POP`), `IT` blocks, table branches (`TBB`/`TBH`), barriers (`DMB`/`DSB`/`ISB`), hardware CRC-32 (`CRC32{B,H,W}` and Castagnoli `CRC32C{B,H,W}`), and exclusives (`LDREX`/`STREX{B,H,D}` lowered directly to 8/16/32/64-bit `LDXR`/`STXR` pairs).
  - **VFPv4-D32, NEON, ARMv8-A Directed-Rounding FP & Crypto**: Full `s0`–`s31` (32 single-precision), `d0`–`d31` (32 double-precision), and `q0`–`q15` (16 quadword SIMD) register file, including `VLD1`–`VLD4`/`VST1`–`VST4` multi-structure loads/stores, `VTBL`/`VTBX`, polynomial arithmetic (`VMULL.P8` and 64×64→128 carryless `VMULL.P64`), saturating/rounding/widening/narrowing SIMD, `VCMP` + `VMRS APSR_nzcv`, `FPSCR` ↔ `FPCR`/`FPSR` synchronization, ARMv8-A floating-point extensions (`VSEL{EQ,VS,GE,GT}.F32/.F64`, scalar & vector `VMAXNM`/`VMINNM`, `VRINT{A,N,P,M,Z,R,X}.F32/.F64`, `VCVT{A,N,P,M}.{S32,U32}.{F32,F64}`), and hardware Cryptography Extensions (`AESE.8`, `AESD.8`, `AESMC.8`, `AESIMC.8`, `SHA1{C,P,M,H,SU0,SU1}.32`, `SHA256{H,H2,SU0,SU1}.32`) advertised via `AT_HWCAP2 = 0x1f`.
- **Fast JIT & Persistent `.a32cache` AOT Engine**:
  - **Strict W^X Discipline**: Code is emitted into a staging buffer and transitioned to `PROT_READ | PROT_EXEC` once per basic-block batch. Under `hyper-linux` (`A32H_CAP_ICACHE_TRAP_IALLU`), a single `IC IVAU + ISB` flushes the entire translation cache without per-cacheline VM exits.
  - **Direct In-Cache 8-Slot Chaining (`br x17`), IBTC & In-Trampoline 64-Entry RAS**: Static direct branch/call exits (`a64_emit_exit_branch`) emit a 12-word inline check of `[x19, #368]` (`pending_signals | flags | itstate`), `cur_entry->slot_entry[s]`, and `g_cache_epoch` (`ldar w21, [x28]`), jumping directly to the target block's `host_code` via `br x17` inside the code cache without returning to `trampoline.S` (`99.99%+` in-cache chain rate on hot loops); polymorphic indirect branches and function returns dispatch in-trampoline through a 64-entry per-thread Return Address Stack (`RAS`, `ctx->ras`) and a 1024-entry lock-free Indirect Branch Target Cache (`IBTC`, `ctx->ibtc`) without exiting to C.
  - **Intra-Block Optimizations**: 128-instruction basic block window; backward dead-NZCV liveness analysis elides redundant `MRS/MSR NZCV` instructions across consecutive flag-setting operations; 24-slot (`v8`–`v31`) lazy-write-back VFP & NEON register cache (`s0`–`s31`, `d0`–`d31`, and 128-bit `q0`–`q15` with full cross-width aliasing) pins scalar and vector accumulators in host SIMD registers across basic blocks.
  - **Base-Address-Independent (Relocatable) Shared-Library & Multi-Region AOT Cache (`A32_AOT_VERSION 5`)**: `a32rt --aot <elf32-or-so>` pre-translates static executables, `PT_INTERP` dynamic linkers (`ld-musl-armhf.so.1` + main executable), and `ET_DYN` shared libraries (`.so`, including `e_entry == 0` with automatic Thumb-2 vs. A32 prologue detection) into content-hashed `.a32cache` artifacts (`$A32RT_CACHE` or `/tmp/a32rt-cache-*`). Guest-PC-derived immediates use a self-identifying `MOVZ Wd, #hi16, LSL #16` + `MOVK Wd, #lo16, LSL #0` pair (`a64_emit_mov_pc32`), enabling `a32_aot_load_fd` to relocate pre-translated `.so` blocks by `delta = load_bias - hdr.reserved` at runtime when mapped at arbitrary ASLR base addresses via `mmap2(..., PROT_EXEC)`.
- **Linux ARM EABI (`armhf`) Runtime Compatibility**:
  - **Multi-Library Dynamic Linking & Sysroot Overlay**: Loads both static (`ET_EXEC`) and PIE/dynamically linked (`ET_DYN` + `PT_INTERP`) ARM32 ELF binaries (verified with Alpine `armv7` `ld-musl-armhf.so.1`, `busybox`, `apk` with `libapk.so` + `libcrypto.so.3` + `libssl.so.3` + `libz.so.1`, `scanelf`, and `getconf`), translating sysroot-prefixed paths transparently via `--sysroot` / `A32RT_SYSROOT`.
  - **Synthetic `0xffff0000` `[vectors]` kuser Page**: Populates real A32 instruction sequences at `0xffff0f60` (`__kuser_cmpxchg64`), `0xffff0fa0` (`__kuser_memory_barrier`), `0xffff0fc0` (`__kuser_cmpxchg`), `0xffff0fe0` (`__kuser_get_tls`), and helper version `5` at `0xffff0ffc`.
  - **ARM EABI Syscall & Struct Shim**: Translates 240+ ARM EABI (`r7`, `r0`–`r5`) syscalls and private ARM syscalls (`cacheflush`, `set_tls`, `get_tls`) to `aarch64-linux` (`x8`, `x0`–`x5`), shimming 32-to-64 structures (`stat64`, `statx`, `statfs64`, `sysinfo`, `rusage`, `iovec`, `msghdr`/`cmsghdr`, `epoll_event`, `flock64`, `timespec`/`timeval`/`itimerspec` time32 & time64 variants, TTY/block/network `ioctl` structs `termios`, `winsize`, `ifreq`, `ifconf`, `BLKGETSIZE*`, `unshare`, `faccessat2`, `fallocate`, `splice`, `sync_file_range2`, `tee`, `timerfd_create`, `signalfd4`, `inotify_init1`, `preadv`/`pwritev`, `setns`, `seccomp`, `memfd_create`, `copy_file_range`, `rseq`, `close_range`), `/proc/self/{exe,maps,auxv,cmdline,stat,status}`, `/proc/cpuinfo`, and `execve` re-wrapping.
  - **Threads, Futexes, Self-Modifying Code & Signals**: Supports `clone` (`CLONE_VM | CLONE_THREAD | CLONE_SETTLS`) with isolated per-thread JIT state, 3-class signal routing (`SIGSEGV`/`SIGBUS`/`SIGILL`/`SIGFPE` side-table PC recovery, guest write-protect SMC invalidation, and asynchronous RT signal delivery), and authentic 32-bit ARM `rt_sigframe` / `sigframe` including the `VFP_MAGIC` (`0x56465001`) coprocessor frame (`d0`–`d31`, `fpscr`, `fpexc`) in `ucontext_t.uc_regspace` across `rt_sigreturn` (`r7 = 173`) / `sigreturn` (`r7 = 119`).
- **Built-In GDB RSP Stub**: Launch with `--gdb <port>` (or `A32RT_GDB=<port>`) to debug guest `arm32` binaries with `gdb-multiarch` or `lldb` (inspect `r0`–`r15`/`cpsr`, read/write guest memory, single-step basic blocks, and set software breakpoints).

---

## Quick Start

### 1. Build `a32rt` and Test Fixtures

Inside the Nix development environment (or with `aarch64-unknown-linux-musl-gcc` and `clang`/`ld.lld` installed):

```sh
nix develop -c make all
```

This produces:
- `_build/a32rt`: Static-PIE `aarch64-linux` binary translator (`~240 KiB`, zero runtime dependencies).
- `_build/a32rt-binfmt-p`: Symlink enabling `binfmt_misc` `P` (preserve `argv[0]`) mode automatically.
- `_build/hello-*`, `_build/test-m1-*` .. `_build/test-m5-*`, `_build/bench-coremark-*`: Freestanding A32 and Thumb-2 verification binaries.

### 2. Run on Plain AArch64 Linux (or inside Lima VM)

```sh
# Run a static armhf binary directly:
./_build/a32rt ./_build/hello-armhf-static

# Run a dynamically linked musl armv7 binary (e.g. busybox or apk) with a sysroot:
./_build/a32rt --sysroot ./_build/sysroot-armv7 ./_build/sysroot-armv7/bin/busybox sh -c 'ls / && echo ok'
./_build/a32rt --sysroot ./_build/sysroot-armv7 ./_build/sysroot-armv7/sbin/apk --version

# Pre-translate a shared library (.so) and executable into the relocatable persistent AOT cache:
./_build/a32rt --aot ./_build/sysroot-armv7/usr/lib/libz.so.1.3.2
./_build/a32rt --aot --sysroot ./_build/sysroot-armv7 ./_build/sysroot-armv7/sbin/apk
./_build/a32rt --stats --sysroot ./_build/sysroot-armv7 ./_build/sysroot-armv7/sbin/apk --version
```

### 3. Run under `hyper-linux` (`hl`) on macOS Apple Silicon

```sh
# Direct invocation via hyper-linux:
../hyper-linux/_build/hl ./_build/a32rt ./_build/hello-armhf-static

# Or let hyper-linux auto-invoke a32rt on any ELFCLASS32 EM_ARM binary:
../hyper-linux/_build/hl --a32rt ./_build/a32rt --sysroot ./_build/sysroot-armv7 \
  ./_build/sysroot-armv7/bin/busybox sh -c 'uname -a && echo ok'
```

### 4. Register with Linux `binfmt_misc`

On any AArch64 Linux host, register `_build/a32rt` (with `OCF` flags) or `_build/a32rt-binfmt-p` (with `OCFP` flags) to execute `arm32` ELF binaries transparently (`AT_EXECFD` is supported natively):

```sh
echo ':arm32:M::\x7fELF\x01\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\x28\x00:\xff\xff\xff\xff\xff\xff\xff\x00\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff\xff:/path/to/a32rt:OCF' \
  | sudo tee /proc/sys/fs/binfmt_misc/register
```

---

## CLI Options & Environment Variables

```text
Usage: a32rt [options] [--] <arm32-elf> [guest-args...]
```

| CLI Flag | Environment Variable | Description |
| :--- | :--- | :--- |
| `--window <hex>` | `A32RT_WINDOW=<hex>` | Guest 4 GiB window base `G` (e.g. `0x0` or `0x100000000`). Probed via `/dev/a32host` or address-space availability if unset. |
| `--sysroot <path>` | `A32RT_SYSROOT=<path>` | Guest `armhf` sysroot directory for `PT_INTERP` (`ld-musl-armhf.so.1` / `ld-linux-armhf.so.3`) and absolute file paths. |
| `--cache <dir>` | `A32RT_CACHE=<dir>` | Directory for persistent `.a32cache` AOT translation artifacts (default: `/tmp/a32rt-cache`). |
| `--aot` | `A32RT_AOT=1` | Pre-translate `PT_LOAD` executable segments (including `PT_INTERP` and relocatable `.so` libraries) into `<cache>` and exit `0`. |
| `--stats` | `A32RT_STATS=1` | Print JIT/AOT telemetry (blocks, emitted bytes, W^X toggles, chain/IBTC/RAS hits, cache flushes) to `stderr` on exit. |
| `--trace` | `A32RT_TRACE=1` | Trace translated basic block entries and guest syscalls to `stderr`. |
| `--gdb <port>` | `A32RT_GDB=<port>` | Start the GDB Remote Serial Protocol (RSP) server on `127.0.0.1:<port>` and wait for debugger attach before guest entry. |
| *(argv[0] `*-binfmt-p`)* | `A32RT_BINFMT_P=1` | Enable Linux `binfmt_misc` `P`-flag argument parsing (`<interp> <full-path> <orig-argv0> [args...]`). |
| `-h`, `--help` | — | Print CLI usage summary and exit. |

---

## Verification & Milestone Suite (`M0`–`M6`)

`a32rt` is continuously verified against two targets:
1. **Primary Target (`*-hl`)**: Running inside [`hyper-linux`](../hyper-linux) (`../hyper-linux/_build/hl`) on macOS Apple Silicon in offset mode (`G = 0x1_0000_0000`) with `/dev/a32host` (`A32H_ABI 1.0`).
2. **Ground-Truth Linux Oracle (`*-lima`)**: Running the exact same static-PIE `_build/a32rt` binary on real AArch64 Linux inside Lima (`limactl`, `hl-rosetta` VM).

Run the complete test suite on either target via `Makefile`:

```sh
# Run all M0..M6 suites under hyper-linux (macOS Apple Silicon):
make test-all-hl

# Run all M0..M6 suites inside the Lima AArch64 Linux VM:
make test-all-lima

# Show all individual Makefile targets:
make help
```

| Milestone | Target (`hl` / `Lima`) | Coverage Verified |
| :--- | :--- | :--- |
| **M0** | `make test-hello-hl`<br>`make test-hello-lima` | Static `ET_EXEC` ELF32 loader, `[G, G+4GiB)` reservation, `0xffff0000` kuser page, A32 & Thumb-2 baseline decoder, trampoline context switch, `write` + `exit_group`. |
| **M1** | `make test-m1-hl`<br>`make test-m1-lima` | Complete ARMv7-A integer/branch/bitfield/DSP/media ISA (A32 & Thumb-2), `PT_INTERP` dynamic linking with real Alpine `ld-musl-armhf.so.1` + `busybox sh -c 'ls / && echo ok'`, `/proc/self/{exe,maps,auxv,cmdline,stat,status}`, `getdents64`, `fork` + `wait4` + `execve`, file-backed `mmap2(PROT_EXEC)` code execution. |
| **M2** | `make test-m2-hl`<br>`make test-m2-lima` | Direct block chaining, 1024-entry lock-free IBTC, 64-entry in-trampoline RAS, dead-NZCV elision, single-batch W^X + `IC IVAU`, multi-region & shared-library `.a32cache` AOT pre-translation (`busybox` + `ld-musl-armhf.so.1`), CoreMark-style CRC benchmark (`0x41dd` across 2,000 iterations in A32 & Thumb-2). |
| **M3** | `make test-m3-hl`<br>`make test-m3-lima` | `clone` threads (`CLONE_VM \| CLONE_THREAD \| CLONE_SETTLS`), `LDREX`/`STREX{B,H,D}` + kuser `__kuser_cmpxchg` / `__kuser_cmpxchg64` + `futex`, 3-class signal router (`SIGSEGV` fault recovery with exact guest `PC`, `SIGUSR1` + `SA_SIGINFO` `ucontext_t` + `VFP_MAGIC` `uc_regspace` `d0`–`d31` mutation, `sigaltstack`, real `rt_sigreturn`), and self-modifying code (`SMC`) via both `cacheflush` and write-protected page faults. |
| **M4** | `make test-m4-hl`<br>`make test-m4-lima` | VFPv4-D32 (`s0`–`s31`, `d0`–`d31`, scalar arithmetic, `VFMA`/`VFMS`, `VSQRT`, `VCVT`, `VCMP` + `VMRS APSR_nzcv`, `FPSCR` rounding modes), NEON ASIMD (`q0`–`q15`, integer/float SIMD, `VLD1`–`VLD4`/`VST1`–`VST4`, `VTBL`/`VTBX`, saturating/widening/narrowing ops, auto-vectorized loops), ARMv8-A Directed-Rounding FP (`VSEL{EQ,VS,GE,GT}`, scalar & vector `VMAXNM`/`VMINNM`, `VRINT{A,N,P,M,Z,R,X}`, `VCVT{A,N,P,M}`), and ARMv8-A `CRC32`/`CRC32C` + Crypto (`AESE`/`AESD`/`AESMC`/`AESIMC`, `VMULL.P64`, `SHA1`, `SHA256`) in both A32 and Thumb-2. |
| **M5** | `make test-m5-hl`<br>`make test-m5-lima` | LTP-style 32-to-64 syscall & struct conformance (`stat64`/`fstat64`/`fstatat64`, `statfs64`, `sysinfo`, `rusage`, `uname` `"armv7l"`, `fcntl64`, `epoll`, `socketpair` + `sendmsg`/`recvmsg` `SCM_RIGHTS` `cmsghdr` translation, `SIOCGIF*`/`SIOCGIFCONF`/`BLKGETSIZE*` `ioctl` shims, `/proc/self/auxv` `AT_HWCAP2`, `poll`/`ppoll`/`pselect6`, `clock_gettime64`, `prlimit64`, negative `EFAULT` checks) + GDB RSP stub (`?`, `g`/`G`, `m`/`M`, `s`, `c`, `Z0`/`z0`). |
| **M6** | `make test-m6-hl`<br>`make test-m6-lima` | Direct In-Cache 8-Slot Chaining (`slot_key[0..7]` / `slot_entry[0..7]`, inline `br x17`), Base-Address-Independent Relocatable `.so` AOT Cache (`A32_AOT_VERSION 5`, `a64_emit_mov_pc32`), 128-Bit NEON `q0`–`q15` (`A64_VFP_KIND_Q`) Intra-Block Register Caching & Cross-Width (`Q` ↔ `D` ↔ `S`) Aliasing, and Multi-Library Dynamic Linking (`apk --version` linking `libapk.so.2.14.0`, `libcrypto.so.3`, `libssl.so.3`, `libz.so.1`, `scanelf -V`, `getconf LONG_BIT`, plus relocatable `libz.so.1.3.2` `.a32cache` loading). |

---

## Benchmarks vs. Existing Solutions

[`tests/bench_suite.c`](tests/bench_suite.c) is a deterministic, checksum-verified 5-workload benchmark suite that compiles from identical C11 source (`-O2 -ffreestanding -nostdlib`) across five target ISAs (`armhf` A32, `armhf` Thumb-2, `aarch64`, `x86_64`, and `i386`). Each workload executes **1 warmup pass + 4 timed trials** and reports the minimum elapsed time (`us`):

1. **`coremark`**: 2,000 iterations of 4×4 integer matrix multiplication, 16-node linked-list insertion sort, numeric state-machine parser, and CRC-16 (`crc=0x000041dd`).
2. **`chacha20`**: 8,000 blocks (512 KiB) of 20-round ChaCha20 ARX stream cipher encryption (`crc=0x065fe64f`).
3. **`vfp_simd`**: 40,000 iterations of 4×4 single-precision FP32 matrix-vector multiply + 4-step double-precision FP64 Mandelbrot orbit (`crc=0xfffdaf18`).
4. **`calls_ibtc`**: Recursive `fib(28)` (`1,028,457` calls/returns) + `200,000` indirect vtable calls (`crc=0xed9c5a0a`).
5. **`syscalls`**: 4,000 `getpid()` syscalls measuring guest `SVC #0` / `syscall` / `int $0x80` trap + ABI shim latency (`crc=0x00000fa0`).

### Measurement Host & Environment

- **Host Hardware**: Apple MacBook Air (**Apple M5**, `T8142`, 10 cores: 4 performance + 6 efficiency, 32 GiB unified memory, macOS Darwin 25.5.0).
- **Linux VM (`bench-lima`)**: Lima `hl-rosetta` (`Linux 6.17.0-41-generic aarch64`, Ubuntu, Apple Virtualization.framework, 4 vCPUs, 3.8 GiB RAM, idle host/guest load `< 0.15`).
- **`hyper-linux` (`bench-hl`)**: Direct macOS Hypervisor.framework user-space VMM (`../hyper-linux/_build/hl`) running on the Apple M5 host in offset mode (`G = 0x1_0000_0000`).

Reproduce the benchmarks with:

```sh
make bench-lima   # 8-way comparison inside AArch64 Linux VM (G = 0)
make bench-hl     # 6-way comparison under hyper-linux on macOS Apple Silicon (G = 4 GiB)
make bench        # Run both suites
```

### 1. AArch64 Linux (`limactl` `hl-rosetta` VM on Apple M5, `G = 0`)

All times in microseconds (`us`, lower is better):

| Translator / Target | Guest ISA | `coremark` (`us`) | `chacha20` (`us`) | `vfp_simd` (`us`) | `calls_ibtc` (`us`) | `syscalls` (`us`) | **Total (`us`)** | **vs. `qemu-arm`** |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| **Native `aarch64-linux`** | `aarch64` | 791 | 947 | 440 | 2,195 | 719 | **5,092** | 14.03× |
| **Apple `RosettaLinux`** | `x86_64` | 1,023 | 2,044 | 705 | 3,128 | 1,137 | **8,037** | 8.89× |
| **`a32rt` (Warm `.a32cache`)** | **`armhf` (Thumb-2)** | **2,239** | **2,831** | **2,026** | **5,851** | **939** | **13,886** | **5.15× faster** |
| **`a32rt` (Warm `.a32cache`)** | **`armhf` (A32)** | **2,312** | **2,432** | **2,091** | **6,396** | **917** | **14,148** | **5.05× faster** |
| **`qemu-arm` 10.1.0 (TCG)** | `armhf` (Thumb-2) | 5,730 | 3,642 | 15,914 | 44,779 | 1,379 | **71,444** | 1.00× |
| **`qemu-arm` 10.1.0 (TCG)** | `armhf` (A32) | 6,679 | 5,538 | 29,577 | 46,090 | 1,748 | **89,632** | 0.80× |
| **`qemu-i386` 10.1.0 (TCG)** | `i386` (32-bit x86) | 7,092 | 12,622 | 24,687 | 54,489 | 2,715 | **101,605** | 0.70× |
| **`qemu-x86_64` 10.1.0 (TCG)** | `x86_64` | 22,496 | 4,945 | 29,133 | 44,783 | 1,305 | **102,662** | 0.70× |

Key takeaways:
- **`a32rt` vs. `qemu-arm` 10.1.0 (TCG)**: With `M6` Direct In-Cache 8-Slot Chaining (`br x17`), `a32rt` is **5.05×–6.34× faster overall** than `qemu-arm` and wins on **every single workload**: **2.56×–2.89× faster** on `coremark` (`2,239 us` Thumb-2 / `2,312 us` A32 vs. `5,730 us` / `6,679 us`), **1.29×–2.28× faster** on `chacha20`, **7.85×–14.14× faster** on `vfp_simd` (where `a32rt`'s 24-slot `v8`–`v31` lazy-write-back VFP/NEON register cache and 128-instruction basic block window keep floating-point accumulators in hardware AArch64 SIMD registers across the entire inner loop), **7.21×–7.65× faster** on `calls_ibtc` (`5,851 us` Thumb-2 and `6,396 us` A32 vs. `44,779 us` / `46,090 us`), and **1.47×–1.91× faster** on `syscalls`.
- **`a32rt` vs. `qemu-x86_64` / `qemu-i386`**: `a32rt` is **7.18×–7.39× faster** than `qemu-x86_64` and `qemu-i386`.

### 2. `hyper-linux` (`hl` on macOS Apple M5, Offset Mode `G = 4 GiB`)

| Translator / Target (`hl`) | Guest ISA | `coremark` (`us`) | `chacha20` (`us`) | `vfp_simd` (`us`) | `calls_ibtc` (`us`) | `syscalls` (`us`) | **Total (`us`)** | Runtime `blocks_compiled` |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| **Native `aarch64-linux`** | `aarch64` | 789 | 948 | 436 | 2,550 | 14,110 | **18,833** | — |
| **Apple `RosettaLinux`** | `x86_64` | 1,018 | 2,029 | 732 | 2,889 | 17,507 | **24,175** | — (AOT) |
| **`a32rt` (Warm `.a32cache`)** | **`armhf` (A32)** | **2,300** | **2,398** | **1,736** | **5,906** | **16,685** | **29,025** | **`0`** |
| **`a32rt` (Cold JIT)** | **`armhf` (A32)** | 2,623 | 2,346 | 1,737 | 5,720 | 16,919 | **29,345** | `107` |
| **`a32rt` (Cold JIT)** | **`armhf` (Thumb-2)** | 2,590 | 2,899 | 1,725 | 5,740 | 19,663 | **32,617** | `111` |
| **`a32rt` (Warm `.a32cache`)** | **`armhf` (Thumb-2)** | **2,262** | **2,831** | **1,738** | **5,861** | **19,600** | **32,292** | **`0`** |

---

## Future Optimization Opportunities

All `M0`–`M6` and `M0-hl`–`M6-hl` milestones specified in [`A32RT.md`](A32RT.md) — including Direct In-Cache 8-Slot Chaining (`br x17`), Base-Address-Independent Relocatable `.so` AOT Cache (`A32_AOT_VERSION 5`), 24-slot (`v8`–`v31`) lazy-write-back VFP & 128-bit NEON (`s0`–`s31`, `d0`–`d31`, `q0`–`q15`) register caching, 64-entry in-trampoline `RAS`, 128-instruction basic block window, full ARMv8-A AArch32 user-mode ISA (`VSEL`, `VMAXNM`/`VMINNM`, `VRINT*`, `VCVT{A,N,P,M}`, `CRC32`/`CRC32C`, and `AES`/`PMULL`/`SHA1`/`SHA256` Crypto), multi-library `PT_INTERP`/`.so` dynamic linking (`apk`, `scanelf`, `getconf`), `VFP_MAGIC` (`0x56465001`) signal frame `uc_regspace` support, and block/network `ioctl` translation — are complete and verified. Potential future enhancements include:

- **Multi-Block Trace Superblocks & Cross-Block VFP/NEON Register Allocation**: Extending the intra-block 24-slot `v8`–`v31` VFP/NEON register cache across direct-chained basic-block boundaries (`slot_entry[0..7]`) so hot loops keep floating-point and vector registers live across loop back-edges without basic-block exit VFP flushes.
- **Specialized Multimedia/DRM `ioctl` Device Families**: Extending `ARM_NR_ioctl` struct translation for specialized multimedia or GPU device families (`V4L2`, `DRM`, `ALSA`) if needed by desktop/GUI workloads.

---

## Repository Layout

```text
a32rt/
├── A32RT.md             # Full architecture, register convention & milestone specification
├── AGENTS.md            # Repository engineering guidelines & invariants
├── LICENSE              # Apache-2.0
├── Makefile             # Self-documenting top-level build & verification driver
├── README.md            # Overview, usage, benchmarks & verification guide
├── flake.nix            # Nix flake for static-PIE aarch64-linux a32rt + armhf toolchains
├── docs/
│   └── HOST-CONTRACT.md # Versioned host contract & /dev/a32host ABI (A32H_ABI 1.0)
├── include/
│   └── a32host.h        # Shared C11 header for /dev/a32host ioctls (vendored by hyper-linux)
├── src/
│   ├── a32rt.h          # Internal structures, CPU state (a32_ctx_t), and module APIs
│   ├── main.c           # CLI parsing, AT_EXECFD, host probing & startup orchestration
│   ├── host.c           # /dev/a32host handshake & runtime capability detection
│   ├── vm.c             # [G, G+4GiB) guest window reservation, guest mmap2/mprotect/mremap/brk
│   ├── elf32.c          # ELFCLASS32 EM_ARM loader, PT_INTERP, and 0xffff0000 kuser page setup
│   ├── stack32.c        # 32-bit initial stack, argv/envp/auxv builder & AT_PLATFORM ("v7l")
│   ├── a64_emit.h/.c    # Direct 32-bit AArch64 instruction encoder (scalar, FP & ASIMD)
│   ├── decode_a32.c     # A32 instruction decoder & A64 lowering
│   ├── decode_t32.c     # T32 (Thumb-1 / Thumb-2) instruction decoder & A64 lowering
│   ├── vfp_neon.c       # VFPv4-D32 & NEON (ASIMD) coprocessor decoder & A64 lowering
│   ├── jit.c            # Code cache, RW chain slots, IBTC, RAS, W^X batching & dispatcher
│   ├── aot.c            # Persistent disk-backed AOT cache (.a32cache, SHA-256 keyed)
│   ├── trampoline.S     # Host <-> guest context switch & in-trampoline chain/IBTC dispatch
│   ├── syscalls.c       # ARM EABI (r7, r0-r5) -> aarch64-linux syscall & struct shim
│   ├── signals.c        # 3-class signal router, PC side-table recovery & arm32 rt_sigframe
│   └── gdb.c            # GDB Remote Serial Protocol (RSP) stub (--gdb / A32RT_GDB)
└── tests/
    ├── hello.c                    # M0 / M0-hl A32 & Thumb-2 smoke test
    ├── test_m1_isa_fs.c           # M1 / M1-hl full A32/T32 ISA, /proc/self/*, getdents64, fork/wait4
    ├── bench_coremark.c           # M2 / M2-hl CoreMark-style CRC & AOT cache benchmark
    ├── bench_suite.c              # Multi-workload cross-architecture benchmark suite (armhf/thumb/aarch64/x86_64/i386)
    ├── test_m3_threads_signals.c  # M3 / M3-hl clone threads, futex, RT signals, SIGSEGV & SMC
    ├── test_m4_vfp_neon.c         # M4 / M4-hl VFPv4-D32 (s0-s31, d0-d31) & NEON SIMD suite
    └── test_m5_ltp_gdb.c          # M5 / M5-hl LTP compatibility & 32-to-64 struct conformance suite
```

---

## License

Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.

Licensed under the Apache License, Version 2.0 (see [`LICENSE`](LICENSE)).
