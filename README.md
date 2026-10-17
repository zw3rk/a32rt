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
  - **Direct Block Chaining, IBTC & In-Trampoline 64-Entry RAS**: Conditional/unconditional direct branches patch 2-way RW chain slots (`chain_key[0..1]` / `chain_code[0..1]`); polymorphic indirect branches and function returns dispatch in-trampoline through a 64-entry per-thread Return Address Stack (`RAS`, `ctx->ras`) and a 1024-entry lock-free Indirect Branch Target Cache (`IBTC`, `ctx->ibtc`) without exiting to C.
  - **Intra-Block Optimizations**: Backward dead-NZCV liveness analysis elides redundant `MRS/MSR NZCV` instructions across consecutive flag-setting operations; single-precision `s0`–`s15` block view caching avoids repeated `s`/`d` register unpacking/repacking.
  - **Multi-Region & Shared-Library Disk-Backed AOT Cache**: `a32rt --aot <elf32-or-so>` pre-translates static executables, `PT_INTERP` dynamic linkers (`ld-musl-armhf.so.1` + main executable), and `ET_DYN` shared libraries (`.so`, including `e_entry == 0`) into content-hashed `.a32cache` artifacts (`$A32RT_CACHE` or `/tmp/a32rt-cache-*`) that are loaded automatically at startup and on file-backed `mmap2(..., PROT_EXEC)` mappings.
- **Linux ARM EABI (`armhf`) Runtime Compatibility**:
  - **Dynamic Linking & Sysroot Overlay**: Loads both static (`ET_EXEC`) and PIE/dynamically linked (`ET_DYN` + `PT_INTERP`) ARM32 ELF binaries (verified with Alpine `armv7` `ld-musl-armhf.so.1` and `busybox`), translating sysroot-prefixed paths transparently via `--sysroot` / `A32RT_SYSROOT`.
  - **Synthetic `0xffff0000` `[vectors]` kuser Page**: Populates real A32 instruction sequences at `0xffff0f60` (`__kuser_cmpxchg64`), `0xffff0fa0` (`__kuser_memory_barrier`), `0xffff0fc0` (`__kuser_cmpxchg`), `0xffff0fe0` (`__kuser_get_tls`), and helper version `5` at `0xffff0ffc`.
  - **ARM EABI Syscall & Struct Shim**: Translates 220+ ARM EABI (`r7`, `r0`–`r5`) syscalls and private ARM syscalls (`cacheflush`, `set_tls`, `get_tls`) to `aarch64-linux` (`x8`, `x0`–`x5`), shimming 32-to-64 structures (`stat64`, `statfs64`, `sysinfo`, `rusage`, `iovec`, `msghdr`/`cmsghdr`, `epoll_event`, `flock64`, `timespec`/`timeval`/`itimerspec` time32 & time64 variants, TTY/block/network `ioctl` structs `termios`, `winsize`, `ifreq`, `ifconf`, `BLKGETSIZE*`), `/proc/self/{exe,maps,auxv,cmdline,stat,status}`, `/proc/cpuinfo`, and `execve` re-wrapping.
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
- `_build/a32rt`: Static-PIE `aarch64-linux` binary translator (`~235 KiB`, zero runtime dependencies).
- `_build/a32rt-binfmt-p`: Symlink enabling `binfmt_misc` `P` (preserve `argv[0]`) mode automatically.
- `_build/hello-*`, `_build/test-m1-*` .. `_build/test-m5-*`, `_build/bench-coremark-*`: Freestanding A32 and Thumb-2 verification binaries.

### 2. Run on Plain AArch64 Linux (or inside Lima VM)

```sh
# Run a static armhf binary directly:
./_build/a32rt ./_build/hello-armhf-static

# Run a dynamically linked musl armv7 binary (e.g. busybox) with a sysroot:
./_build/a32rt --sysroot ./_build/sysroot-armv7 ./_build/sysroot-armv7/bin/busybox sh -c 'ls / && echo ok'

# Pre-translate an armhf binary (or shared library / PT_INTERP binary) into the persistent AOT cache:
./_build/a32rt --aot ./_build/bench-coremark-armhf
./_build/a32rt --stats ./_build/bench-coremark-armhf
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
| `--aot` | `A32RT_AOT=1` | Pre-translate `PT_LOAD` executable segments (including `PT_INTERP` and `.so` libraries) into `<cache>` and exit `0`. |
| `--stats` | `A32RT_STATS=1` | Print JIT/AOT telemetry (blocks, emitted bytes, W^X toggles, IBTC hit rate, RAS hits, cache flushes) to `stderr` on exit. |
| `--trace` | `A32RT_TRACE=1` | Trace translated basic block entries and guest syscalls to `stderr`. |
| `--gdb <port>` | `A32RT_GDB=<port>` | Start the GDB Remote Serial Protocol (RSP) server on `127.0.0.1:<port>` and wait for debugger attach before guest entry. |
| *(argv[0] `*-binfmt-p`)* | `A32RT_BINFMT_P=1` | Enable Linux `binfmt_misc` `P`-flag argument parsing (`<interp> <full-path> <orig-argv0> [args...]`). |
| `-h`, `--help` | — | Print CLI usage summary and exit. |

---

## Verification & Milestone Suite (`M0`–`M5`)

`a32rt` is continuously verified against two targets:
1. **Primary Target (`*-hl`)**: Running inside [`hyper-linux`](../hyper-linux) (`../hyper-linux/_build/hl`) on macOS Apple Silicon in offset mode (`G = 0x1_0000_0000`) with `/dev/a32host` (`A32H_ABI 1.0`).
2. **Ground-Truth Linux Oracle (`*-lima`)**: Running the exact same static-PIE `_build/a32rt` binary on real AArch64 Linux inside Lima (`limactl`, `hl-rosetta` VM).

Run the complete test suite on either target via `Makefile`:

```sh
# Run all M0..M5 suites under hyper-linux (macOS Apple Silicon):
make test-all-hl

# Run all M0..M5 suites inside the Lima AArch64 Linux VM:
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
| **Native `aarch64-linux`** | `aarch64` | 812 | 1,161 | 431 | 2,132 | 1,419 | **5,955** | 8.54× |
| **Apple `RosettaLinux`** | `x86_64` | 973 | 2,071 | 733 | 3,714 | 775 | **8,266** | 6.15× |
| **`a32rt` (Warm `.a32cache`)** | **`armhf` (Thumb-2)** | **5,224** | **2,652** | **2,896** | **15,650** | **915** | **27,337** | **1.86× faster** |
| **`a32rt` (Warm `.a32cache`)** | **`armhf` (A32)** | **5,463** | **2,630** | **3,122** | **15,667** | **924** | **27,806** | **1.83× faster** |
| **`qemu-arm` 10.1.0 (TCG)** | `armhf` (Thumb-2) | 6,206 | 3,676 | 14,726 | 24,781 | 1,277 | **50,666** | 1.00× |
| **`qemu-arm` 10.1.0 (TCG)** | `armhf` (A32) | 6,827 | 5,680 | 15,183 | 21,568 | 1,593 | **50,851** | 1.00× (baseline) |
| **`qemu-i386` 10.1.0 (TCG)** | `i386` (32-bit x86) | 5,889 | 12,086 | 15,680 | 29,731 | 2,471 | **65,857** | 0.77× |
| **`qemu-x86_64` 10.1.0 (TCG)** | `x86_64` | 23,292 | 4,727 | 16,834 | 23,924 | 2,373 | **71,150** | 0.71× |

Key takeaways:
- **`a32rt` vs. `qemu-arm` 10.1.0 (TCG)**: `a32rt` beats `qemu-arm` on **every single workload**: **1.19×–1.25× faster** on `coremark`, **1.39×–2.16× faster** on `chacha20`, **4.86×–5.09× faster** on `vfp_simd` (where `a32rt` lowers VFPv4-D32 / NEON directly to hardware AArch64 FP/ASIMD instructions rather than softfloat helpers), **1.38×–1.58× faster** on `calls_ibtc` (with **`3,571,150` in-trampoline 64-entry `RAS` hits** and zero C dispatcher fallbacks), and **1.40×–1.72× faster** on `syscalls`.
- **`a32rt` vs. `qemu-x86_64` / `qemu-i386`**: `a32rt` is **2.6× faster** than `qemu-x86_64` and **2.4× faster** than `qemu-i386`.

### 2. `hyper-linux` (`hl` on macOS Apple M5, Offset Mode `G = 4 GiB`)

| Translator / Target (`hl`) | Guest ISA | `coremark` (`us`) | `chacha20` (`us`) | `vfp_simd` (`us`) | `calls_ibtc` (`us`) | `syscalls` (`us`) | **Total (`us`)** | Runtime `blocks_compiled` / `ras_hits` |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| **Native `aarch64-linux`** | `aarch64` | 798 | 956 | 461 | 2,031 | 10,640 | **14,886** | — |
| **Apple `RosettaLinux`** | `x86_64` | 882 | 1,687 | 736 | 3,167 | 11,799 | **18,271** | — (AOT) |
| **`a32rt` (Warm `.a32cache`)** | **`armhf` (A32)** | **5,142** | **2,313** | **2,514** | **15,033** | **11,387** | **36,389** | **`1` / `3,571,150`** |
| **`a32rt` (Cold JIT)** | **`armhf` (Thumb-2)** | 6,148 | 2,284 | 2,390 | 14,448 | 11,142 | **36,412** | `122` / `3,571,150` |
| **`a32rt` (Warm `.a32cache`)** | **`armhf` (Thumb-2)** | **5,928** | **2,283** | **2,510** | **13,870** | **13,489** | **38,080** | **`2` / `3,571,150`** |
| **`a32rt` (Cold JIT)** | **`armhf` (A32)** | 6,007 | 2,301 | 2,283 | 16,361 | 16,044 | **42,996** | `117` / `3,571,136` |

---

## Future Optimization Opportunities

All `M0`–`M5` and `M0-hl`–`M5-hl` milestones specified in [`A32RT.md`](A32RT.md) — as well as the 64-entry in-trampoline `RAS`, full ARMv8-A AArch32 user-mode ISA (`VSEL`, `VMAXNM`/`VMINNM`, `VRINT*`, `VCVT{A,N,P,M}`, `CRC32`/`CRC32C`, and `AES`/`PMULL`/`SHA1`/`SHA256` Crypto), multi-region `PT_INTERP`/`.so` `.a32cache` AOT pre-translation, `VFP_MAGIC` (`0x56465001`) signal frame `uc_regspace` support, and block/network `ioctl` translation — are complete and verified. Potential future enhancements include:

- **Cross-Block Superblock Register Allocation for `d0`–`d31`**: Keeping guest VFP/NEON `d0`–`d31` resident in host `v0`–`v31` across entire `a32_jit_run_block` dispatch chains rather than loading/storing `d` registers at basic-block boundaries.
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
