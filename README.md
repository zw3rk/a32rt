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
- **Complete ARMv7-A User-Mode ISA + VFPv4-D32 + NEON (ASIMD)**:
  - **A32 & Thumb-2 (`T16`/`T32`)**: Data processing, barrel shifter, multiply/accumulate, hardware divide (`SDIV`/`UDIV`), saturating/DSP/parallel byte & halfword media instructions, bitfield ops, load/store word/byte/halfword/doubleword/multiple (`LDM`/`STM`, `PUSH`/`POP`), `IT` blocks, table branches (`TBB`/`TBH`), barriers (`DMB`/`DSB`/`ISB`), and exclusives (`LDREX`/`STREX{B,H,D}` lowered directly to 8/16/32/64-bit `LDXR`/`STXR` pairs).
  - **VFPv4-D32 & NEON**: Full `s0`–`s31` (32 single-precision), `d0`–`d31` (32 double-precision), and `q0`–`q15` (16 quadword SIMD) register file, including `VLD1`–`VLD4`/`VST1`–`VST4` multi-structure loads/stores, `VTBL`/`VTBX`, polynomial arithmetic, saturating/rounding/widening/narrowing SIMD, `VCMP` + `VMRS APSR_nzcv`, and `FPSCR` ↔ `FPCR`/`FPSR` synchronization.
- **Fast JIT & Persistent `.a32cache` AOT Engine**:
  - **Strict W^X Discipline**: Code is emitted into a staging buffer and transitioned to `PROT_READ | PROT_EXEC` once per basic-block batch. Under `hyper-linux` (`A32H_CAP_ICACHE_TRAP_IALLU`), a single `IC IVAU + ISB` flushes the entire translation cache without per-cacheline VM exits.
  - **Direct Block Chaining, IBTC & RAS**: Conditional/unconditional direct branches patch RW chain slots (`a32_chain_slot`); indirect branches dispatch through a 4096-entry lock-free Indirect Branch Target Cache (`IBTC`, `x22`) and a 16-entry per-thread Return Address Stack (`RAS`) for `BX lr` / `POP {..., pc}`.
  - **Intra-Block Optimizations**: Backward dead-NZCV liveness analysis elides redundant `MRS/MSR NZCV` instructions across consecutive flag-setting operations; single-precision `s0`–`s15` block view caching avoids repeated `s`/`d` register unpacking/repacking.
  - **Disk-Backed AOT Cache**: `a32rt --aot <elf32>` pre-translates static `PT_LOAD` executable segments into a SHA-256-keyed `.a32cache` artifact (`$A32RT_CACHE` or `/tmp/a32rt-cache`) that is mapped directly at startup.
- **Linux ARM EABI (`armhf`) Runtime Compatibility**:
  - **Dynamic Linking & Sysroot Overlay**: Loads both static (`ET_EXEC`) and PIE/dynamically linked (`ET_DYN` + `PT_INTERP`) ARM32 ELF binaries (verified with Alpine `armv7` `ld-musl-armhf.so.1` and `busybox`), translating sysroot-prefixed paths transparently via `--sysroot` / `A32RT_SYSROOT`.
  - **Synthetic `0xffff0000` `[vectors]` kuser Page**: Populates real A32 instruction sequences at `0xffff0f60` (`__kuser_cmpxchg64`), `0xffff0fa0` (`__kuser_memory_barrier`), `0xffff0fc0` (`__kuser_cmpxchg`), `0xffff0fe0` (`__kuser_get_tls`), and helper version `5` at `0xffff0ffc`.
  - **ARM EABI Syscall & Struct Shim**: Translates 220+ ARM EABI (`r7`, `r0`–`r5`) syscalls and private ARM syscalls (`cacheflush`, `set_tls`, `get_tls`) to `aarch64-linux` (`x8`, `x0`–`x5`), shimming 32-to-64 structures (`stat64`, `statfs64`, `sysinfo`, `rusage`, `iovec`, `msghdr`/`cmsghdr`, `epoll_event`, `flock64`, `timespec`/`timeval`/`itimerspec` time32 & time64 variants), `/proc/self/{exe,maps,auxv,cmdline,stat,status}`, and `execve` re-wrapping.
  - **Threads, Futexes, Self-Modifying Code & Signals**: Supports `clone` (`CLONE_VM | CLONE_THREAD | CLONE_SETTLS`) with isolated per-thread JIT state, 3-class signal routing (`SIGSEGV`/`SIGBUS`/`SIGILL`/`SIGFPE` side-table PC recovery, guest write-protect SMC invalidation, and asynchronous RT signal delivery), and authentic 32-bit ARM `rt_sigframe` + `rt_sigreturn` (`r7 = 173`) / `sigreturn` (`r7 = 119`).
- **Built-In GDB RSP Stub**: Launch with `--gdb <port>` (or `A32RT_GDB=<port>`) to debug guest `arm32` binaries with `gdb-multiarch` or `lldb` (inspect `r0`–`r15`/`cpsr`, read/write guest memory, single-step basic blocks, and set software breakpoints).

---

## Quick Start

### 1. Build `a32rt` and Test Fixtures

Inside the Nix development environment (or with `aarch64-unknown-linux-musl-gcc` and `clang`/`ld.lld` installed):

```sh
nix develop -c make all
```

This produces:
- `_build/a32rt`: Static-PIE `aarch64-linux` binary translator (`~225 KiB`, zero runtime dependencies).
- `_build/a32rt-binfmt-p`: Symlink enabling `binfmt_misc` `P` (preserve `argv[0]`) mode automatically.
- `_build/hello-*`, `_build/test-m1-*` .. `_build/test-m5-*`, `_build/bench-coremark-*`: Freestanding A32 and Thumb-2 verification binaries.

### 2. Run on Plain AArch64 Linux (or inside Lima VM)

```sh
# Run a static armhf binary directly:
./_build/a32rt ./_build/hello-armhf-static

# Run a dynamically linked musl armv7 binary (e.g. busybox) with a sysroot:
./_build/a32rt --sysroot ./_build/sysroot-armv7 ./_build/sysroot-armv7/bin/busybox sh -c 'ls / && echo ok'

# Pre-translate an armhf binary into the persistent AOT cache and run with telemetry:
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
| `--aot` | `A32RT_AOT=1` | Pre-translate `PT_LOAD` executable segments into `<cache>/<sha256>.a32cache` and exit `0`. |
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
| **M1** | `make test-m1-hl`<br>`make test-m1-lima` | Complete ARMv7-A integer/branch/bitfield/DSP/media ISA (A32 & Thumb-2), `PT_INTERP` dynamic linking with real Alpine `ld-musl-armhf.so.1` + `busybox sh -c 'ls / && echo ok'`, `/proc/self/{exe,maps,auxv,cmdline,stat,status}`, `getdents64`, `fork` + `wait4` + `execve`. |
| **M2** | `make test-m2-hl`<br>`make test-m2-lima` | Direct block chaining, 4096-entry lock-free IBTC, 16-entry RAS, dead-NZCV elision, single-batch W^X + `IC IVAU`, SHA-256 `.a32cache` AOT pre-translation, CoreMark-style CRC benchmark (`0x8e3a` across 2,500 iterations in A32 & Thumb-2). |
| **M3** | `make test-m3-hl`<br>`make test-m3-lima` | `clone` threads (`CLONE_VM \| CLONE_THREAD \| CLONE_SETTLS`), `LDREX`/`STREX{B,H,D}` + kuser `__kuser_cmpxchg` / `__kuser_cmpxchg64` + `futex`, 3-class signal router (`SIGSEGV` fault recovery with exact guest `PC`, `SIGUSR1` + `SA_SIGINFO` `ucontext_t` mutation, `sigaltstack`, real `rt_sigreturn`), and self-modifying code (`SMC`) via both `cacheflush` and write-protected page faults. |
| **M4** | `make test-m4-hl`<br>`make test-m4-lima` | VFPv4-D32 (`s0`–`s31`, `d0`–`d31`, scalar arithmetic, `VFMA`/`VFMS`, `VSQRT`, `VCVT`, `VCMP` + `VMRS APSR_nzcv`, `FPSCR` rounding modes) and NEON ASIMD (`q0`–`q15`, integer/float SIMD, `VLD1`–`VLD4`/`VST1`–`VST4`, `VTBL`/`VTBX`, saturating/widening/narrowing ops, auto-vectorized loops) in both A32 and Thumb-2. |
| **M5** | `make test-m5-hl`<br>`make test-m5-lima` | LTP-style 32-to-64 syscall & struct conformance (`stat64`/`fstat64`/`fstatat64`, `statfs64`, `sysinfo`, `rusage`, `uname` `"armv7l"`, `fcntl64`, `epoll`, `socketpair` + `sendmsg`/`recvmsg` `SCM_RIGHTS` `cmsghdr` translation, `poll`/`ppoll`/`pselect6`, `clock_gettime64`, `prlimit64`, negative `EFAULT` checks) + GDB RSP stub (`?`, `g`/`G`, `m`/`M`, `s`, `c`, `Z0`/`z0`). |

---

## Repository Layout

```text
a32rt/
├── A32RT.md             # Full architecture, register convention & milestone specification
├── AGENTS.md            # Repository engineering guidelines & invariants
├── LICENSE              # Apache-2.0
├── Makefile             # Self-documenting top-level build & verification driver
├── README.md            # Overview, usage & verification guide
├── flake.nix            # Nix flake for static-PIE aarch64-linux a32rt + armhf toolchains
├── docs/
│   └── HOST-CONTRACT.md # Versioned host contract & /dev/a32host ABI (A32H_ABI 1.0)
├── include/
│   └── a32host.h        # Shared C11 header for /dev/a32host ioctls (vendored by hyper-linux)
├── src/
│   ├── a32rt.h          # Internal structures, CPU state (a32_cpu_state), and module APIs
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
│   ├── trampoline.S     # Host <-> guest context switch (GPRs + VFP d0-d31 save/restore)
│   ├── syscalls.c       # ARM EABI (r7, r0-r5) -> aarch64-linux syscall & struct shim
│   ├── signals.c        # 3-class signal router, PC side-table recovery & arm32 rt_sigframe
│   └── gdb.c            # GDB Remote Serial Protocol (RSP) stub (--gdb / A32RT_GDB)
└── tests/
    ├── hello.c                    # M0 / M0-hl A32 & Thumb-2 smoke test
    ├── test_m1_isa_fs.c           # M1 / M1-hl full A32/T32 ISA, /proc/self/*, getdents64, fork/wait4
    ├── bench_coremark.c           # M2 / M2-hl CoreMark-style CRC & AOT cache benchmark
    ├── test_m3_threads_signals.c  # M3 / M3-hl clone threads, futex, RT signals, SIGSEGV & SMC
    ├── test_m4_vfp_neon.c         # M4 / M4-hl VFPv4-D32 (s0-s31, d0-d31) & NEON SIMD suite
    └── test_m5_ltp_gdb.c          # M5 / M5-hl LTP compatibility & 32-to-64 struct conformance suite
```

---

## License

Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.

Licensed under the Apache License, Version 2.0 (see [`LICENSE`](LICENSE)).
