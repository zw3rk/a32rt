# `a32rt` — AArch32 (`armhf` Linux ELF & 32-Bit iOS `armv7`/`armv7s` Mach-O) → AArch64 Binary Translator

`a32rt` is a standalone, zero-dependency, Rosetta-style user-mode dynamic (JIT), reference-interpreter, and ahead-of-time (AOT) binary translator that runs **both** 32-bit ARM Linux ELF binaries (`armv7l` / `armhf` / `armel`) **and** 32-bit iOS Mach-O binaries (`armv7` / `armv7s`, thin `MH_MAGIC` and universal `FAT_MAGIC`) on 64-bit AArch64 hosts whose CPUs lack hardware 32-bit EL0 execution support (Apple Silicon M1–M5 on macOS and Linux, and modern AArch64 server processors).

It builds from a single Curated-C11 and AArch64-assembly tree into two standalone zero-dependency host binaries:
- `_build/a32rt`: Static-PIE `aarch64-linux` ELF executable (for native AArch64 Linux and [`hyper-linux`](../hyper-linux) on Apple Silicon).
- `_build/a32rt-darwin`: Native `arm64-apple-darwin` Mach-O executable (runs directly on macOS Apple Silicon with `MAP_JIT` APRR W^X and 7-instruction 4 KiB Soft-TLB over 16 KiB host pages, no VM required).

At startup `a32rt` inspects the guest binary's magic bytes and automatically selects either the **Linux ARM EABI (`ELFCLASS32` `EM_ARM`)** personality or the **32-bit iOS Darwin/XNU (`MH_MAGIC` / `FAT_MAGIC` `CPU_TYPE_ARM`)** personality inside a reserved 4 GiB guest virtual address window `[G, G + 4 GiB)`.

For the full architectural specification, register allocation, and design rationale, see [`A32RT.md`](A32RT.md) and [`docs/HOST-CONTRACT.md`](docs/HOST-CONTRACT.md).

---

## Key Features

- **Zero External Dependencies & Dual Host Backends**: Written in strict Curated C11 (`-std=c11 -Werror -Werror=vla -pedantic -Wall -Wextra`, `<= 900` lines per file) and AArch64 assembly (`src/trampoline.S`), with `src/os/backend_linux.c` (`aarch64-linux` / `hyper-linux`) and `src/os/backend_darwin.c` (standalone macOS `arm64`).
- **Dual Execution Engines (`--interp` + `--dbt`), Lockstep `--difftest` & Persistent `--aot`**:
  - **Portable Reference Interpreter (`src/interp/`)**: Complete Curated-C11 oracle covering A32, 16-bit Thumb (`T16`), 32-bit Thumb-2 (`T32`), VFPv4-D32, and NEON ASIMD (`--interp`), plus instruction-by-instruction lockstep verification against the AArch64 JIT (`--difftest`).
  - **AArch64 DBT (`src/dbt/`) & Persistent `.a32cache` (`--aot`)**: Direct 32-bit AArch64 emitter (`src/core/`), 8-slot in-cache chaining, IBTC, RAS, and persistent content-hashed `.a32cache` artifacts supporting both ELF32 (`PT_LOAD` / `.so`) and 32-bit iOS Mach-O (`LC_SEGMENT`, thin and universal `FAT_MAGIC` slice offset) binaries.
- **Parametric 4 GiB Guest Window (`[G, G + 4 GiB)`) & Soft-TLB**:
  - **Regime A1 (`H == 4096`)**: On 4 KiB AArch64 Linux (`G = 0`) and under [`hyper-linux`](../hyper-linux) (`G = 0x1_0000_0000`), `a32rt` holds `G` in reserved register `x20` (`xG`) and lowers all guest memory accesses via `[xG, wAddr, uxtw]`, preserving exact 32-bit address wrap-around (`mod 2^32`).
  - **Regime A2 (`H > 4096`, e.g. 16 KiB macOS or 16/64 KiB Linux)**: Uses a 7-instruction 4 KiB Soft-TLB (`x20 = &ctx->tlb[0]`) with 2-instruction miss stubs and 24-byte `dbt_mem_site_t` fault/preemption recovery over in-place `Provider A` (`src/vm/vm_file.c`) COW arenas.
- **Complete ARMv7-A + ARMv8-A AArch32 User-Mode ISA + VFPv4-D32 + NEON (ASIMD) + Crypto**:
  - **A32 & Thumb-2 (`T16`/`T32`)**: Data processing, barrel shifter, multiply/accumulate, hardware divide (`SDIV`/`UDIV`), saturating/DSP/parallel byte & halfword media instructions, bitfield ops, load/store word/byte/halfword/doubleword/multiple (`LDM`/`STM`, `PUSH`/`POP`), `IT` blocks, table branches (`TBB`/`TBH`), barriers (`DMB`/`DSB`/`ISB`), hardware CRC-32 (`CRC32{B,H,W}` and Castagnoli `CRC32C{B,H,W}`), and exclusives (`LDREX`/`STREX{B,H,D}` lowered directly to 8/16/32/64-bit `LDXR`/`STXR` pairs).
  - **VFPv4-D32, NEON, ARMv8-A Directed-Rounding FP & Crypto**: Full `s0`–`s31` (32 single-precision), `d0`–`d31` (32 double-precision), and `q0`–`q15` (16 quadword SIMD) register file, including `VLD1`–`VLD4`/`VST1`–`VST4` multi-structure loads/stores, `VTBL`/`VTBX`, polynomial arithmetic (`VMULL.P8` and 64×64→128 carryless `VMULL.P64`), saturating/rounding/widening/narrowing SIMD, `VCMP` + `VMRS APSR_nzcv`, `FPSCR` ↔ `FPCR`/`FPSR` synchronization, ARMv8-A floating-point extensions (`VSEL{EQ,VS,GE,GT}.F32/.F64`, scalar & vector `VMAXNM`/`VMINNM`, `VRINT{A,N,P,M,Z,R,X}.F32/.F64`, `VCVT{A,N,P,M}.{S32,U32}.{F32,F64}`), and hardware Cryptography Extensions (`AESE.8`, `AESD.8`, `AESMC.8`, `AESIMC.8`, `SHA1{C,P,M,H,SU0,SU1}.32`, `SHA256{H,H2,SU0,SU1}.32`) advertised via `AT_HWCAP2 = 0x1f`.
- **Fast JIT & Persistent `.a32cache` AOT Engine**:
  - **Strict W^X Discipline**: Code is emitted into a staging buffer and transitioned to `PROT_READ | PROT_EXEC` once per basic-block batch. Under `hyper-linux` (`A32H_CAP_ICACHE_TRAP_IALLU`), a single `IC IVAU + ISB` flushes the entire translation cache without per-cacheline VM exits.
  - **Direct In-Cache 8-Slot Chaining (`br x17`), IBTC & In-Trampoline 64-Entry RAS**: Static direct branch/call exits (`a64_emit_exit_branch`) emit a 12-word inline check of `[x19, #368]` (`pending_signals | flags | itstate`), `cur_entry->slot_entry[s]`, and `g_cache_epoch` (`ldar w21, [x28]`), jumping directly to the target block's `host_code` via `br x17` inside the code cache without returning to `trampoline.S` (`99.99%+` in-cache chain rate on hot loops); polymorphic indirect branches and function returns dispatch in-trampoline through a 64-entry per-thread Return Address Stack (`RAS`, `ctx->ras`) and a 1024-entry lock-free Indirect Branch Target Cache (`IBTC`, `ctx->ibtc`) without exiting to C.
  - **Intra-Block Optimizations**: 128-instruction basic block window; backward dead-NZCV liveness analysis elides redundant `MRS/MSR NZCV` instructions across consecutive flag-setting operations; 24-slot (`v8`–`v31`) lazy-write-back VFP & NEON register cache (`s0`–`s31`, `d0`–`d31`, and 128-bit `q0`–`q15` with full cross-width aliasing) pins scalar and vector accumulators in host SIMD registers across basic blocks.
  - **Base-Address-Independent (Relocatable) Shared-Library & Multi-Region AOT Cache (`A32_AOT_VERSION 8`)**: `a32rt --aot <elf32-or-so>` pre-translates static executables, `PT_INTERP` dynamic linkers (`ld-musl-armhf.so.1` + main executable), and `ET_DYN` shared libraries (`.so`, including `e_entry == 0` with automatic Thumb-2 vs. A32 prologue detection) into content-hashed `.a32cache` artifacts (`$A32RT_CACHE`, `$XDG_CACHE_HOME`, or `/tmp/a32rt-cache-*`). Guest-PC-derived immediates use a self-identifying `MOVZ Wd, #hi16, LSL #16` + `MOVK Wd, #lo16, LSL #0` pair (`a64_emit_mov_pc32`), enabling `a32_aot_load_fd` to relocate pre-translated `.so` blocks by `delta = load_bias - hdr.load_bias` at runtime when mapped at arbitrary ASLR base addresses via `mmap2(..., PROT_EXEC)`.
- **Linux ARM EABI (`armhf`) Runtime Compatibility**:
  - **Multi-Library Dynamic Linking & Sysroot Overlay**: Loads both static (`ET_EXEC`) and PIE/dynamically linked (`ET_DYN` + `PT_INTERP`) ARM32 ELF binaries (verified with Alpine `armv7` `ld-musl-armhf.so.1`, `busybox`, `apk` with `libapk.so` + `libcrypto.so.3` + `libssl.so.3` + `libz.so.1`, `scanelf`, and `getconf`), translating sysroot-prefixed paths transparently via `--sysroot` / `A32RT_SYSROOT`.
  - **Synthetic `0xffff0000` `[vectors]` kuser Page**: Populates real A32 instruction sequences at `0xffff0f60` (`__kuser_cmpxchg64`), `0xffff0fa0` (`__kuser_memory_barrier`), `0xffff0fc0` (`__kuser_cmpxchg`), `0xffff0fe0` (`__kuser_get_tls`), and helper version `5` at `0xffff0ffc`.
  - **ARM EABI Syscall & Struct Shim**: Translates 240+ ARM EABI (`r7`, `r0`–`r5`) syscalls and private ARM syscalls (`cacheflush`, `set_tls`, `get_tls`) to `aarch64-linux` (`x8`, `x0`–`x5`), shimming 32-to-64 structures (`stat64`, `statx`, `statfs64`, `sysinfo`, `rusage`, `iovec`, `msghdr`/`cmsghdr`, `epoll_event`, `flock64`, `timespec`/`timeval`/`itimerspec` time32 & time64 variants, TTY/block/network `ioctl` structs `termios`, `winsize`, `ifreq`, `ifconf`, `BLKGETSIZE*`, `unshare`, `faccessat2`, `fallocate`, `splice`, `sync_file_range2`, `tee`, `timerfd_create`, `signalfd4`, `inotify_init1`, `preadv`/`pwritev`, `setns`, `seccomp`, `memfd_create`, `copy_file_range`, `rseq`, `close_range`), `/proc/self/{exe,maps,auxv,cmdline,stat,status}`, `/proc/cpuinfo`, and `execve` re-wrapping.
  - **Threads, Futexes, Self-Modifying Code & Signals**: Supports `clone` (`CLONE_VM | CLONE_THREAD | CLONE_SETTLS`) with isolated per-thread JIT state, 3-class signal routing (`SIGSEGV`/`SIGBUS`/`SIGILL`/`SIGFPE` side-table PC recovery, guest write-protect SMC invalidation, and asynchronous RT signal delivery), and authentic 32-bit ARM `rt_sigframe` / `sigframe` including the `VFP_MAGIC` (`0x56465001`) coprocessor frame (`d0`–`d31`, `fpscr`, `fpexc`) in `ucontext_t.uc_regspace` across `rt_sigreturn` (`r7 = 173`) / `sigreturn` (`r7 = 119`).
- **32-Bit iOS (`armv7` / `armv7s`) Mach-O & Darwin/XNU Runtime Personality (`src/ios/`)**:
  - **Thin & Universal Fat Mach-O Loader (`ios_macho.c`)**: Auto-detects thin 32-bit `MH_MAGIC` (`0xfeedface`) and universal `FAT_MAGIC` / `FAT_CIGAM` (`0xcafebabe` / `0xbebafeca`) binaries, selects the best `CPU_TYPE_ARM (12)` slice (`armv7s` → `armv7` → `armv6` → `CPU_SUBTYPE_ARM_ALL`), maps `LC_SEGMENT` (`__PAGEZERO` guard, `__TEXT`, `__DATA`, `__LINKEDIT`) with PIE slide when `MH_PIE` is set, and initializes entry state from `LC_UNIXTHREAD` or `LC_MAIN` (`CPSR.T` derived from `entry & 1`).
  - **Dual Dynamic-Linking Modes (`ios_dylib.c`, `ios_bind.c`, `ios_export.c`, `ios_stubs.c`)**: When `--sysroot` provides an extracted iOS SDK/rootfs containing `/usr/lib/dyld`, maps `dyld` and hands off on an authentic XNU `mh_execute_header` + `apple[]` initial stack; otherwise runs a built-in recursive `LC_LOAD_DYLIB` / `LC_LOAD_WEAK_DYLIB` / `LC_REEXPORT_DYLIB` loader (`@executable_path`, `@loader_path`, and `@rpath` via `LC_RPATH`) with `LC_DYLD_INFO[_ONLY]` rebase/bind/weak-bind/lazy-bind bytecode execution, classic `LC_SYMTAB` + `LC_DYSYMTAB` indirect-symbol binding, Mach-O export-trie resolution (including re-exports and resolver thunks), topological `__mod_init_func` constructors, and a synthetic `libSystem.B.dylib` `svc #0x81` stub island (`malloc`/`free`/`realloc`/`posix_memalign`, `printf`/`snprintf`, string/memory/ctype/`qsort`/`bsearch`/`arc4random`, `dlopen`/`dlsym`, `setjmp`/`longjmp`, `pthread_*`, `dispatch_once`/`dispatch_sync_f`, and minimal Objective-C selector/class hooks).
  - **XNU `_COMM_PAGE`, `TPIDRURO`, `svc #0x80` BSD/Mach/ARM Traps, Threads & Signals**: Populates the 32-bit ARM Darwin `_COMM_PAGE` at `0xFFFF4000` (mirrored at `0xFFFF0000`), maintains per-thread `TPIDRURO` (`mrc p15, 0, Rt, c13, c0, 3` with low 2 bits cleared per Darwin `cthread` ABI), routes `svc #0x80` across BSD syscalls (`r12 > 0` and indirect `syscall()` `r12 == 0` with `r0:r1` 64-bit returns and `CPSR.C` carry-flag error polarity, including `kqueue`/`kevent`, sockets, `sysctl`, `mmap`/`mincore`, `fork`/`wait4`, and `--image-fd` `execve`), Mach traps (`r12 < 0`: `mach_absolute_time`, `mach_timebase_info`, `mach_msg_trap`, `mach_vm_*`, `semaphore_*`), and ARM fast traps (`r12 = 0x80000000..`: `thread_set_tsd_base`, `icache_invalidate`, `dcache_flush`), plus `bsdthread`/`pthread`/`ulock_wait`/`ulock_wake` synchronization and 32-bit Darwin signal frames (`__darwin_arm_ucontext`, `sigreturn #184`, `sigaltstack`, `sigsuspend`).
- **Built-In GDB RSP Stub**: Launch with `--gdb <port>` (or `A32RT_GDB=<port>`) to debug guest `arm32` binaries with `gdb-multiarch` or `lldb` (inspect `r0`–`r15`/`cpsr`, read/write guest memory, single-step basic blocks, and set software breakpoints).

---

## Quick Start

### 1. Build `a32rt` (Linux & macOS) and Test Fixtures

Inside the Nix development environment (or with `aarch64-unknown-linux-musl-gcc` and `clang`/`ld.lld` installed):

```sh
nix develop -c make all
make build-darwin
```

This produces:
- `_build/a32rt`: Static-PIE `aarch64-linux` binary translator (zero runtime dependencies).
- `_build/a32rt-darwin`: Standalone native `arm64-apple-darwin` Mach-O binary translator (runs directly on macOS Apple Silicon without a VM).
- `_build/a32rt-binfmt-p`: Symlink enabling `binfmt_misc` `P` (preserve `argv[0]`) mode automatically.
- `_build/hello-*`, `_build/test-m1-*` .. `_build/test-m5-*`, `_build/bench-coremark-*`, and `_build/ios/*`: Freestanding Linux ELF32 and 32-bit iOS (`armv7`/`armv7s`/universal fat) verification binaries.

### 2. Run 32-Bit iOS (`armv7` / `armv7s`) Mach-O Binaries (macOS or Linux)

```sh
# Run thin armv7/armv7s or universal fat iOS Mach-O binaries directly on macOS:
./_build/a32rt-darwin ./_build/ios/test-ios-fat
./_build/a32rt-darwin --difftest ./_build/ios/test-ios-dylib

# Run or AOT-compile 32-bit iOS Mach-O binaries on AArch64 Linux:
./_build/a32rt --aot ./_build/ios/test-ios-fat
./_build/a32rt ./_build/ios/test-ios-fat

# Run the full 61-leg 32-bit iOS qualification suite across macOS & Linux:
make test-ios
```

### 3. Run Linux ELF32 Binaries on Standalone macOS or Plain AArch64 Linux

```sh
# Run directly on macOS Apple Silicon (no VM):
./_build/a32rt-darwin ./_build/hello-armhf-static

# Run a static armhf binary on AArch64 Linux:
./_build/a32rt ./_build/hello-armhf-static

# Run a dynamically linked musl armv7 binary (e.g. busybox or apk) with a sysroot:
./_build/a32rt --sysroot ./_build/sysroot-armv7 ./_build/sysroot-armv7/bin/busybox sh -c 'ls / && echo ok'
./_build/a32rt --sysroot ./_build/sysroot-armv7 ./_build/sysroot-armv7/sbin/apk --version

# Pre-translate a shared library (.so) and executable into the relocatable persistent AOT cache:
./_build/a32rt --aot ./_build/sysroot-armv7/usr/lib/libz.so.1.3.2
./_build/a32rt --aot --sysroot ./_build/sysroot-armv7 ./_build/sysroot-armv7/sbin/apk
./_build/a32rt --stats --sysroot ./_build/sysroot-armv7 ./_build/sysroot-armv7/sbin/apk --version
```

### 4. Run under `hyper-linux` (`hl`) on macOS Apple Silicon

```sh
# Direct invocation via hyper-linux:
../hyper-linux/_build/hl ./_build/a32rt ./_build/hello-armhf-static

# Or let hyper-linux auto-invoke a32rt on any ELFCLASS32 EM_ARM binary:
../hyper-linux/_build/hl --a32rt ./_build/a32rt --sysroot ./_build/sysroot-armv7 \
  ./_build/sysroot-armv7/bin/busybox sh -c 'uname -a && echo ok'
```

### 5. Register with Linux `binfmt_misc`

On any AArch64 Linux host, register `_build/a32rt` (with `OCF` flags) or `_build/a32rt-binfmt-p` (with `OCFP` flags) to execute `arm32` ELF binaries transparently (`AT_EXECFD` is supported natively):

```sh
echo ':arm32:M::\x7fELF\x01\x01\x01\x00\x00\x00\x00\x00\x00\x00\x00\x00\x02\x00\x28\x00:\xff\xff\xff\xff\xff\xff\xff\x00\xff\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff\xff:/path/to/a32rt:OCF' \
  | sudo tee /proc/sys/fs/binfmt_misc/register
```

---

## CLI Options & Environment Variables

```text
Usage: a32rt [options] [--] <arm32-elf-or-ios-macho> [guest-args...]
```

| CLI Flag | Environment Variable | Description |
| :--- | :--- | :--- |
| `--interp` | `A32RT_ENGINE=interp` | Execute using the portable Curated-C11 A32/Thumb-2/VFP/NEON reference interpreter. |
| `--dbt` | `A32RT_ENGINE=dbt` | Execute using the AArch64 dynamic binary translator (default). |
| `--difftest` | `A32RT_ENGINE=difftest` | Run `--interp` and `--dbt` in instruction-by-instruction lockstep and verify zero state divergence. |
| `--window <hex>` | `A32RT_WINDOW=<hex>` | Guest 4 GiB window base `G` (e.g. `0x0` or `0x100000000`). Probed via `/dev/a32host` or address-space availability if unset. |
| `--sysroot <path>` | `A32RT_SYSROOT=<path>` | Guest `armhf` Linux rootfs (`PT_INTERP`) or extracted 32-bit iOS SDK/rootfs (`/usr/lib/dyld` and dylibs). |
| `--cache <dir>` | `A32RT_CACHE=<dir>` | Directory for persistent `.a32cache` AOT translation artifacts (default: `$XDG_CACHE_HOME` or `/tmp`). |
| `--aot` | `A32RT_AOT=1` | Pre-translate executable segments (`PT_LOAD` ELF32 / `.so` or `LC_SEGMENT` thin/fat iOS Mach-O) into `<cache>` and exit `0`. |
| `--auto-aot` | `A32RT_AUTO_AOT=1` | Automatically save compiled JIT blocks into the persistent `.a32cache` on exit when no warm cache was loaded at startup, and propagate across `execve`. |
| `--execfn <path>` | — | Override resolved guest executable path (`AT_EXECFN` / `apple[0]`) across descriptor-backed `execve`. |
| `--image-fd <fd>` | — | Load the guest ELF or Mach-O image directly from open file descriptor `<fd>` (eliminating path TOCTOU). |
| `--no-host` | — | Bypass `/dev/a32host` probing and use standalone user-space `Provider A` VM management. |
| `--stats` | `A32RT_STATS=1` | Print JIT/AOT telemetry (blocks, emitted bytes, W^X toggles, chain/IBTC/RAS hits, cache flushes) to `stderr` on exit. |
| `--trace` | `A32RT_TRACE=1` | Trace translated basic block entries and guest syscalls to `stderr`. |
| `--gdb <port>` | `A32RT_GDB=<port>` | Start the GDB Remote Serial Protocol (RSP) server on `127.0.0.1:<port>` and wait for debugger attach before guest entry. |
| *(argv[0] `*-binfmt-p`)* | `A32RT_BINFMT_P=1` | Enable Linux `binfmt_misc` `P`-flag argument parsing (`<interp> <full-path> <orig-argv0> [args...]`). |
| `-h`, `--help` | — | Print CLI usage summary and exit. |

---

## Verification & Milestone Suite (`M0`–`M8`, `iOS`, & `Parity`)

`a32rt` is continuously verified across **three host environments** (`_build/a32rt-darwin` on standalone macOS Apple Silicon, `_build/a32rt` under [`hyper-linux`](../hyper-linux) in offset mode `G = 0x1_0000_0000`, and `_build/a32rt` on AArch64 Linux):

```sh
# Run the 61-leg 32-bit iOS Mach-O qualification suite (Darwin + Linux aarch64):
make test-ios

# Run the 66-row multi-engine ELF32 parity gate (Darwin + hyper-linux, interp/dbt/difftest/AOT):
HL_PARITY=1 sh tests/parity_suite.sh

# Run all M0..M8 Linux suites under hyper-linux or inside the Lima AArch64 Linux VM:
make test-all-hl
make test-all-lima
```

| Milestone | Target (`Darwin` / `hl` / `Linux`) | Coverage Verified |
| :--- | :--- | :--- |
| **iOS** | `make test-ios` | **61/61 PASS** across standalone macOS (`_build/a32rt-darwin`) and AArch64 Linux (`_build/a32rt`) in `--interp`, `--dbt`, `--difftest` (`0` divergences), `--sysroot`, and `--aot`: thin `armv7` & `armv7s` `MH_MAGIC`, universal `FAT_MAGIC`, `LC_DYLD_INFO_ONLY` + `@executable_path` + `@rpath` (`LC_RPATH`) dylib binding & `__mod_init_func`, `_COMM_PAGE`, `TPIDRURO`, BSD/Mach/ARM `svc #0x80` traps (`kqueue`/`kevent`, `socketpair`, `poll`, `sysctl`, `mach_msg_trap`, `mach_vm_*`, `semaphore_*`, `fork`/`wait4`/`execve`), `pthread`/`ulock` synchronization, and Darwin 32-bit signal frames (`sigaltstack`, `SA_ONSTACK`, `SA_RESETHAND`, `sigreturn #184`). |
| **Parity** | `HL_PARITY=1 sh tests/parity_suite.sh` | **66/66 PASS (`0 fail, 0 gap`)** across A32, Thumb-2 (`T16`/`T32`), and `armel` fixtures in `--interp`, `--dbt`, `--difftest`, and `--aot` on standalone macOS and `hyper-linux`. |
| **M0** | `make test-hello-hl`<br>`make test-hello-lima` | Static `ET_EXEC` ELF32 loader, `[G, G+4GiB)` reservation, `0xffff0000` kuser page, A32 & Thumb-2 baseline decoder, trampoline context switch, `write` + `exit_group`. |
| **M1** | `make test-m1-hl`<br>`make test-m1-lima` | Complete ARMv7-A integer/branch/bitfield/DSP/media ISA (A32 & Thumb-2), `PT_INTERP` dynamic linking with real Alpine `ld-musl-armhf.so.1` + `busybox sh -c 'ls / && echo ok'`, `/proc/self/{exe,maps,auxv,cmdline,stat,status}`, `getdents64`, `fork` + `wait4` + `execve`, file-backed `mmap2(PROT_EXEC)` code execution. |
| **M2** | `make test-m2-hl`<br>`make test-m2-lima` | Direct block chaining, 1024-entry lock-free IBTC, 64-entry in-trampoline RAS, dead-NZCV elision, single-batch W^X + `IC IVAU`, multi-region & shared-library `.a32cache` AOT pre-translation (`busybox` + `ld-musl-armhf.so.1`), CoreMark-style CRC benchmark (`0x41dd` across 2,000 iterations in A32 & Thumb-2). |
| **M3** | `make test-m3-hl`<br>`make test-m3-lima` | `clone` threads (`CLONE_VM \| CLONE_THREAD \| CLONE_SETTLS`), `LDREX`/`STREX{B,H,D}` + kuser `__kuser_cmpxchg` / `__kuser_cmpxchg64` + `futex`, 3-class signal router (`SIGSEGV` fault recovery with exact guest `PC`, `SIGUSR1` + `SA_SIGINFO` `ucontext_t` + `VFP_MAGIC` `uc_regspace` `d0`–`d31` mutation, `sigaltstack`, real `rt_sigreturn`), and self-modifying code (`SMC`) via both `cacheflush` and write-protected page faults. |
| **M4** | `make test-m4-hl`<br>`make test-m4-lima` | VFPv4-D32 (`s0`–`s31`, `d0`–`d31`, scalar arithmetic, `VFMA`/`VFMS`, `VSQRT`, `VCVT`, `VCMP` + `VMRS APSR_nzcv`, `FPSCR` rounding modes), NEON ASIMD (`q0`–`q15`, integer/float SIMD, `VLD1`–`VLD4`/`VST1`–`VST4`, `VTBL`/`VTBX`, saturating/widening/narrowing ops, auto-vectorized loops), ARMv8-A Directed-Rounding FP (`VSEL{EQ,VS,GE,GT}`, scalar & vector `VMAXNM`/`VMINNM`, `VRINT{A,N,P,M,Z,R,X}`, `VCVT{A,N,P,M}`), and ARMv8-A `CRC32`/`CRC32C` + Crypto (`AESE`/`AESD`/`AESMC`/`AESIMC`, `VMULL.P64`, `SHA1`, `SHA256`) in both A32 and Thumb-2. |
| **M5** | `make test-m5-hl`<br>`make test-m5-lima` | LTP-style 32-to-64 syscall & struct conformance (`stat64`/`fstat64`/`fstatat64`, `statfs64`, `sysinfo`, `rusage`, `uname` `"armv7l"`, `fcntl64`, `epoll`, `socketpair` + `sendmsg`/`recvmsg` `SCM_RIGHTS` `cmsghdr` translation, `SIOCGIF*`/`SIOCGIFCONF`/`BLKGETSIZE*` `ioctl` shims, `/proc/self/auxv` `AT_HWCAP2`, `poll`/`ppoll`/`pselect6`, `clock_gettime64`, `prlimit64`, negative `EFAULT` checks) + GDB RSP stub (`?`, `g`/`G`, `m`/`M`, `s`, `c`, `Z0`/`z0`). |
| **M6** | `make test-m6-hl`<br>`make test-m6-lima` | Direct In-Cache 8-Slot Chaining (`slot_key[0..7]` / `slot_entry[0..7]`, inline `br x17`), Base-Address-Independent Relocatable `.so` AOT Cache (`A32_AOT_VERSION 8`, `a64_emit_mov_pc32`), 128-Bit NEON `q0`–`q15` (`A64_VFP_KIND_Q`) Intra-Block Register Caching & Cross-Width (`Q` ↔ `D` ↔ `S`) Aliasing, and Multi-Library Dynamic Linking (`apk --version` linking `libapk.so.2.14.0`, `libcrypto.so.3`, `libssl.so.3`, `libz.so.1`, `scanelf -V`, `getconf LONG_BIT`, plus relocatable `libz.so.1.3.2` `.a32cache` loading). |
| **M7** | `make test-m7-hl`<br>`make test-m7-lima` | IEEE-754 `libm` & `fenv` rounding/exception conformance, `setjmp`/`longjmp` VFP `d8`–`d15` preservation, POSIX `regcomp`/`regexec`, `pthread` mutex/condvar/TLS (`tests/test_m7_libc_math.c` in A32 & Thumb-2), and the 19-package Alpine v3.21 `armv7` ecosystem suite (`bash`, `dash`, `coreutils`, `grep`, `sed`, `gawk`, `find`, `diff`, `tree`, `jq`, `sqlite3`, `lua5.4`, `luajit` with ARM32 JIT, `openssl`, `zstd`, `xz`, `git`, `python3.12`, and self-hosted `gcc 14.2.0` + `binutils` compiling, linking, inspecting, and running an `armhf` ELF binary). |
| **M8** | `make test-m8-hl`<br>`make test-m8-lima` | Debian 12 (`bookworm`) `armhf` `glibc 2.36` conformance (`PT_INTERP = /lib/ld-linux-armhf.so.3`, `AT_PLATFORM = "v8l"`, `dlopen`/`dlsym`, NPTL `pthread_create` + `__thread` TLS, `qsort`, `libm`, and `/bin/dash`), C++17 & ARM EHABI `.ARM.exidx` zero-cost exception unwinding across 6 stack frames restoring callee-saved GPRs (`r4`–`r9`) and VFP (`d8`–`d15`) on both Alpine `musl` (`test-m8-cpp-armhf` / `test-m8-cpp-thumb`) and Debian `bookworm` `glibc 2.36` (`test-m8-cpp-glibc-armhf` / `test-m8-cpp-glibc-thumb` with `libstdc++.so.6` + `libgcc_s.so.1`), RAII destructors, RTTI (`dynamic_cast`/`typeid`), STL (`std::vector`, `std::map`, `std::sort`, `std::accumulate`), `std::thread`/`std::future` cross-thread exception propagation, self-hosted `armhf` `g++ 14.2.0` compilation, and `--auto-aot` (`A32RT_AUTO_AOT=1`) transparent cache persistence. |

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
| **Native `aarch64-linux`** | `aarch64` | 764 | 1,019 | 466 | 2,076 | 638 | **4,963** | 8.66× |
| **Apple `RosettaLinux`** | `x86_64` | 901 | 1,707 | 823 | 3,494 | 759 | **7,684** | 5.59× |
| **`a32rt` (Warm `.a32cache`)** | **`armhf` (Thumb-2)** | **2,180** | **1,717** | **1,608** | **4,706** | **799** | **11,010** | **3.90× faster** |
| **`a32rt` (Warm `.a32cache`)** | **`armhf` (A32)** | **2,295** | **1,567** | **1,380** | **4,735** | **1,647** | **11,624** | **3.88× faster** |
| **`qemu-arm` 10.1.0 (TCG)** | `armhf` (Thumb-2) | 5,351 | 3,065 | 12,578 | 20,642 | 1,330 | **42,966** | 1.00× |
| **`qemu-arm` 10.1.0 (TCG)** | `armhf` (A32) | 5,672 | 5,290 | 12,743 | 19,364 | 2,080 | **45,149** | 0.95× |
| **`qemu-x86_64` 10.1.0 (TCG)** | `x86_64` | 20,606 | 4,267 | 17,495 | 24,211 | 1,305 | **67,884** | 0.63× |
| **`qemu-i386` 10.1.0 (TCG)** | `i386` (32-bit x86) | 10,779 | 14,099 | 17,951 | 28,566 | 1,388 | **72,783** | 0.59× |

Key takeaways:
- **`a32rt` vs. `qemu-arm` 10.1.0 (TCG)**: With Direct In-Cache 8-Slot Chaining (`br x17`), Direct PC-Relative Self-Loop Back-Edges (`b loop_body_offset`), Self-Loop VFP/NEON Preheader Promotion (`v8`–`v31`), 64-bit `LDP`/`STP` Multi-Word Pairing (`LDM`/`STM`/`PUSH`/`POP` and `LDRD`/`STRD`), and 8-word Inline `RAS` Push + 13-word Inline `RAS` Pop + 1-instruction `ubfx` `IBTC` Dispatch, `a32rt` is **3.88×–3.90× faster** than `qemu-arm` across both A32 (`11,624 µs` vs. `45,149 µs`) and Thumb-2 (`11,010 µs` vs. `42,966 µs`), winning on **every single workload** and achieving **2.22×–2.34×** of native `aarch64-linux`.
- **A32 vs. Thumb-2 Parity**: `calls_ibtc` executes in **`4,735 µs` on A32** and **`4,706 µs` on Thumb-2** (within `0.6%`), with `14,686,789` / `14,706,855` (`99.86%`) block transitions staying completely inside `g_code_cache`.
- **`a32rt` vs. `qemu-x86_64` / `qemu-i386`**: `a32rt` is **5.84×–6.61× faster** than `qemu-x86_64` and `qemu-i386`.

### 2. `hyper-linux` (`hl` on macOS Apple M5, Offset Mode `G = 4 GiB`)

| Translator / Target (`hl`) | Guest ISA | `coremark` (`us`) | `chacha20` (`us`) | `vfp_simd` (`us`) | `calls_ibtc` (`us`) | `syscalls` (`us`) | **Total (`us`)** | Runtime `blocks_compiled` |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| **`a32rt` (Cold JIT)** | **`armhf` (Thumb-2)** | **1,173** | **2,535** | **767** | **3,480** | **6,852** | **14,807** | `136` |
| **Native `aarch64-linux`** | `aarch64` | 1,001 | 1,372 | 553 | 2,075 | 13,907 | **18,908** | — |
| **Apple `RosettaLinux`** | `x86_64` | 1,141 | 2,426 | 747 | 3,178 | 13,611 | **21,103** | — (AOT) |
| **`a32rt` (Warm `.a32cache`)** | **`armhf` (A32)** | **1,791** | **1,284** | **1,342** | **4,724** | **12,952** | **22,093** | **`0`** |
| **`a32rt` (Warm `.a32cache`)** | **`armhf` (Thumb-2)** | **1,595** | **1,190** | **1,393** | **4,873** | **16,818** | **25,869** | **`0`** |
| **`a32rt` (Cold JIT)** | **`armhf` (A32)** | 2,572 | 2,123 | 1,631 | 6,409 | 19,277 | **32,012** | `137` |

---

## Completed Architecture & Optimization Features (`M0`–`M8`)

All `M0`–`M8` and `M0-hl`–`M8-hl` milestones specified in [`A32RT.md`](A32RT.md) are complete and verified:
- **Direct In-Cache 8-Slot Chaining (`br x17`), Direct Self-Loop Back-Edges (`b loop_body_offset`) & 8-Word Inline `RAS` / 1-Instruction `ubfx` `IBTC` Dispatch**: Static branches, `BL` / `BLX` calls, `BX LR` / `POP {..., PC}` returns, and indirect branches transition directly between compiled blocks inside `g_code_cache` (`99.86%+` in-cache chain hit rate across `14.7M` executed blocks in `bench-suite`).
- **64-Bit `LDP`/`STP` Multi-Word Pairing & Self-Loop Conditional Back-Edge Termination**: `LDM`/`STM`/`PUSH`/`POP` and `LDRD`/`STRD` pair consecutive 32-bit guest registers into 64-bit host `LDP`/`STP` operations after a single `[xG, wAddr, uxtw]` address calculation, and conditional backward branches targeting `start_pc` terminate basic blocks so post-loop epilogue instructions never disqualify loop VFP promotion.
- **24-Slot (`v8`–`v31`) Lazy-Write-Back VFP/NEON Register Cache & Self-Loop Preheader Promotion**: Caches `s0`–`s31`, `d0`–`d31`, and 128-bit `q0`–`q15` in AArch64 hardware SIMD registers, keeps `VMOV`/`VCVT` in-register via `FMOV`, and hoists loop-invariant VFP/NEON loads into a loop preheader while deferring dirty write-backs to loop exits.
- **Base-Address-Independent Relocatable `.so` AOT Cache (`A32_AOT_VERSION 8`) & `--auto-aot` (`A32RT_AUTO_AOT=1`)**: Multi-region (`PT_INTERP` + `.so`) pre-translation with self-identifying `a64_emit_mov_pc32` immediate relocation, heap-allocated discovery tables, and automatic on-exit `.a32cache` persistence.
- **Full `musl` & `glibc` 2.36 + C++17 / ARM EHABI Ecosystem Support (`M7`–`M8`)**: Verified on both Alpine v3.21 `armv7` (`ld-musl-armhf.so.1`, 19-package ecosystem including `LuaJIT`, `Python 3.12`, `git`, `sqlite3`, `openssl`, and self-hosted `gcc`/`g++ 14.2.0`) and Debian 12 `bookworm` `armhf` (`ld-linux-armhf.so.3` `glibc 2.36`, NPTL pthreads, `dlopen`, and `libstdc++.so.6` + `libgcc_s.so.1` C++17 `.ARM.exidx` zero-cost exception unwinding in both A32 and Thumb-2).
- **GDB RSP `qXfer:features:read:target.xml` & `VFP_MAGIC` (`0x56465001`) Signal Frame**: Full ARMv7 + VFPv3-D32 (`d0`–`d31`, `fpscr`) XML target description for `gdb-multiarch`/`lldb` and `rt_sigframe.uc_regspace` coprocessor state preservation.

---

## Repository Layout

```text
a32rt/
├── A32RT.md             # Full architecture, register convention & milestone specification
├── AGENTS.md            # Repository engineering guidelines & invariants
├── LICENSE              # Apache-2.0
├── Makefile             # Self-documenting top-level build & verification driver
├── README.md            # Overview, usage, benchmarks & verification guide
├── flake.nix            # Nix flake for static-PIE aarch64-linux & darwin a32rt, armhf toolchains & hydraJobs
├── docs/
│   └── HOST-CONTRACT.md # Versioned host contract & /dev/a32host ABI (A32H_ABI 1.0/1.1)
├── include/
│   └── a32host.h        # Shared C11 header for /dev/a32host ioctls (vendored by hyper-linux)
├── tools/
│   ├── run_ios_suite.sh    # 61-leg macOS & Linux 32-bit iOS qualification runner (interp/dbt/difftest/AOT)
│   └── setup_m7_sysroot.py # Alpine v3.21 armv7 & Debian bookworm armhf glibc 2.36 sysroot provisioner
├── src/
│   ├── a32rt.h          # Internal structures, CPU state (a32_ctx_t), and module APIs
│   ├── a64_emit.h       # Direct 32-bit AArch64 instruction encoder interface
│   ├── main.c           # CLI parsing, ELF32 vs. Mach-O dispatch, AT_EXECFD & startup orchestration
│   ├── host.c           # /dev/a32host handshake & runtime capability detection
│   ├── elf32.c          # ELFCLASS32 EM_ARM loader, PT_INTERP, and 0xffff0000 kuser page setup
│   ├── stack32.c        # 32-bit Linux initial stack, argv/envp/auxv builder & AT_PLATFORM ("v8l")
│   ├── signals.c        # 3-class signal router, PC side-table recovery & arm32 rt_sigframe
│   ├── gdb.c            # GDB Remote Serial Protocol (RSP) stub (--gdb / A32RT_GDB)
│   ├── trampoline.S     # Host <-> guest context switch & in-trampoline chain/IBTC/Soft-TLB dispatch
│   ├── core/            # AArch64 scalar, FP/ASIMD & Soft-TLB emitters (core_emit_*.c)
│   ├── interp/          # Portable Curated-C11 A32, T16/T32 Thumb-2 & VFP/NEON reference interpreter
│   ├── dbt/             # AArch64 JIT cache, chaining, IBTC, RAS, lockstep difftest & .a32cache AOT
│   ├── vm/              # Class-A 4 GiB window, Soft-TLB, and Provider A in-place file COW (vm32, vm_file)
│   ├── os/              # Linux ARM EABI syscall/struct shims plus backend_linux.c & backend_darwin.c
│   └── ios/             # 32-bit iOS (armv7/armv7s) Mach-O/FAT loader, dyld binder, _COMM_PAGE, BSD/Mach/ARM traps, signals, threads & libSystem stub island
└── tests/
    ├── parity_suite.sh            # 66-row multi-engine ELF32 parity gate (golden outputs in tests/golden/)
    ├── ios/                       # 32-bit iOS Mach-O/FAT fixtures, @rpath dylibs & build_ios_fixtures.sh
    ├── hello.c                    # M0 / M0-hl A32 & Thumb-2 smoke test
    ├── test_m1_isa_fs.c           # M1 / M1-hl full A32/T32 ISA, /proc/self/*, getdents64, fork/wait4
    ├── bench_coremark.c           # M2 / M2-hl CoreMark-style CRC & AOT cache benchmark
    ├── bench_suite.c              # Multi-workload cross-architecture benchmark suite
    ├── test_m3_threads_signals.c  # M3 / M3-hl clone threads, futex, RT signals, SIGSEGV & SMC
    ├── test_m4_vfp_neon.c         # M4 / M4-hl VFPv4-D32 (s0-s31, d0-d31) & NEON SIMD suite
    ├── test_m5_ltp_gdb.c          # M5 / M5-hl LTP compatibility & 32-to-64 struct conformance suite
    ├── test_m7_libc_math.c        # M7 / M7-hl IEEE-754 libm, fenv, regex, setjmp & pthread conformance
    ├── test_m7_ecosystem.sh       # M7 / M7-hl real-world Alpine armv7 apps & self-hosted gcc suite
    ├── test_m8_glibc.c            # M8 / M8-hl Debian bookworm armhf glibc 2.36 (ld-linux-armhf.so.3, NPTL, dlopen)
    └── test_m8_cpp_ehabi.cpp      # M8 / M8-hl C++17 & ARM EHABI (.ARM.exidx unwind, RTTI, RAII, STL, std::future)
```

---

## License

Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.

Licensed under the Apache License, Version 2.0 (see [`LICENSE`](LICENSE)).
