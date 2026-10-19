# AGENTS.md — `a32rt`

`a32rt` is a standalone, zero-dependency, Rosetta-style AArch32 (`arm32` / `armhf`) → AArch64 (`aarch64-linux`) user-mode dynamic and AOT binary translator. It builds as a static-PIE `aarch64-linux` ELF that loads an `arm32` ELF into a 4 GiB guest window `[G, G + 4 GiB)` (`G = 0` on plain Linux when free; `G = 0x1_0000_0000` in offset mode under `hyper-linux`) and issues only standard `aarch64-linux` syscalls.

Read [`A32RT.md`](A32RT.md) for the full technical design, host contract (`A32H_ABI 1.0`), and milestone plan.

---

## 1. Ground Rules & Inheritance

This repository follows:
- Global instructions in `~/.config/ai/AGENTS.md` (`ITERATE_FAST` / `BUILD` vs `HARDEN` session modes, performance instrumentation loop, Makefile conventions, commit hygiene).
- Curated C11 guidelines in `~/.config/ai/C_GUIDELINES.md`.
- Sibling boundary rules in `../hyper-linux/AGENTS.md` and `../hyper-linux/docs/SIBLING-REPOS.md`.

### Key Invariants
1. **Language & Toolchain**: **Curated C11** (`-std=c11 -Werror=vla -pedantic -Wall -Wextra`) + minimal AArch64 `.S` assembly (`trampoline.S`, `start.S`).
   - Fixed-width integers (`uint8_t`, `uint16_t`, `uint32_t`, `uint64_t`, `size_t`, `uintptr_t`) everywhere; `unsigned` for bitwise/shifts.
   - Forbidden: VLAs, Annex K (`*_s`), `<threads.h>`, `_Generic`, `gets`/`sprintf`/`strcpy`/`strcat`/`atoi` (use `snprintf` and `strtol`/`strtoul`).
2. **Zero External Dependencies**: No Dynarmic, no oaknut, no C++, no CMake. `a32rt` uses its own bespoke A32/T32 decoder, 32-bit AArch64 instruction encoder (`a64_emit`), runtime code cache, and EABI syscall/signal layer from `M0`.
3. **Host-Agnostic Contract**: Zero `#include` or build-time dependencies on `hyper-linux`. All host hints are negotiated at runtime via standard `auxv`, environment variables (`A32RT_WINDOW`, `A32RT_SYSROOT`, `A32RT_CACHE`), or the optional `/dev/a32host` ioctl (`include/a32host.h`, `A32H_ABI 1.0`).
4. **Build Driver**: Self-documenting root `Makefile` (`make help` default goal, `##` target comments, `.PHONY` declarations) driven inside `nix develop -c make <target>`.
5. **Verification Targets**:
   - **Primary target**: **`M0-hl`..`M7-hl`** running under `hl` (`../hyper-linux/_build/hl --a32rt ...`) on macOS Apple Silicon.
   - **Ground-truth Linux oracle**: **`limactl`** (`lima-aarch64` VM) running the exact same static-PIE `a32rt` binary and `armhf` test fixtures on real AArch64 Linux.
6. **License & Author**: `Apache-2.0`, `Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.` Never add `Co-Authored-By:` or AI attribution trailers to commits.

---

## 2. Repository Layout

```
a32rt/
├── A32RT.md             # Architecture, prior art, hl integration & milestone spec
├── AGENTS.md            # Repo-specific agent instructions
├── LICENSE              # Apache-2.0
├── Makefile             # Self-documenting top-level build & test driver
├── flake.nix            # Nix builds: static-PIE aarch64-linux a32rt + armhf test fixtures
├── docs/
│   └── HOST-CONTRACT.md # Versioned host contract & /dev/a32host ABI (A32H_ABI 1.0)
├── include/
│   └── a32host.h        # Shared C11 header for /dev/a32host ioctls (vendored by hl)
├── tools/
│   └── setup_m7_sysroot.py # Alpine v3.21 armv7 package sysroot provisioner for M7
├── src/
│   ├── main.c           # CLI parsing, AT_EXECFD, host probing, startup orchestration
│   ├── host.c           # /dev/a32host handshake & capability detection
│   ├── vm.c             # [G, G+4GiB) guest window reservation, guest mmap2/mprotect/brk
│   ├── elf32.c          # ELFCLASS32 EM_ARM loader, PT_INTERP, kuser page setup
│   ├── stack32.c        # 32-bit initial stack, argv/envp/auxv builder
│   ├── a64_emit.h/.c    # Direct 32-bit AArch64 instruction encoder
│   ├── decode_a32.c     # A32 instruction decoder & A64 lowering
│   ├── decode_t32.c     # T32 (Thumb-1 / Thumb-2) decoder & A64 lowering
│   ├── vfp_neon.c       # VFPv4-D32 & NEON (ASIMD) decoder & A64 lowering
│   ├── jit.c            # Code cache, RW chain slots, IBTC, W^X batch mprotect, dispatcher
│   ├── aot.c            # Persistent disk-backed AOT cache (.a32cache, SHA-256 keyed)
│   ├── trampoline.S     # Host <-> guest context switch, GPR + VFP d0-d31 save/restore
│   ├── syscalls.c       # ARM EABI (r7, r0-r5) -> aarch64-linux syscall & struct shim
│   ├── signals.c        # 3-class signal routing, side-table recovery & arm32 rt_sigframe
│   └── gdb.c            # GDB Remote Serial Protocol (RSP) stub (--gdb / A32RT_GDB)
└── tests/
    ├── hello.c                    # M0 / M0-hl A32 & Thumb-2 smoke test
    ├── test_m1_isa_fs.c           # M1 / M1-hl full A32/T32 ISA, /proc/self/*, getdents64, fork/wait4
    ├── bench_coremark.c           # M2 / M2-hl CoreMark-style CRC & AOT cache benchmark
    ├── test_m3_threads_signals.c  # M3 / M3-hl clone threads, futex, RT signals, SIGSEGV & SMC
    ├── test_m4_vfp_neon.c         # M4 / M4-hl VFPv4-D32 (s0-s31, d0-d31) & NEON SIMD suite
    ├── test_m5_ltp_gdb.c          # M5 / M5-hl LTP compatibility & 32-to-64 struct conformance suite
    ├── test_m7_libc_math.c        # M7 / M7-hl IEEE-754 libm, fenv, regex, setjmp & pthread conformance
    └── test_m7_ecosystem.sh       # M7 / M7-hl real-world Alpine armv7 apps & self-hosted gcc suite
```

---

## 3. Architecture & Codegen Rules

- **Parametric Guest Window (`[G, G + 4 GiB)`)**:
  - Never hardcode `G = 0`. Keep `G` in reserved host register `x20` (`xG`) and compute guest memory addresses via `[xG, wAddr, uxtw]` or `add xTmp, xG, wAddr, uxtw` so 32-bit address wrap-around (`reg + imm` mod `2^32`) is preserved in offset mode (`G = 0x1_0000_0000` under `hl`).
- **W^X & I-Cache Discipline**:
  - Emit A64 instructions into a staging buffer or RW page batch, transition pages to `PROT_READ | PROT_EXEC` once per batch, and place all mutable chain slots and IBTC tables in RW data pages.
  - When `A32H_CAP_ICACHE_TRAP_IALLU` is present (under `hl`), execute a single `IC IVAU` + `ISB` per batch instead of per-cacheline loops (avoiding one HVC #12 VM exit per cache line).
- **Exclusives (`LDREX` / `STREX`)**:
  - Lower 8/16/32-bit exclusives to `LDXR`/`STXR{B,H}` and 64-bit `LDREXD`/`STREXD` to **64-bit `LDXR xTmp, [xAddr]` / `STXR wRd, xTmp, [xAddr]`** (never 128-bit `LDXP/STXP`). Never insert host loads, stores, or spills between `LDREX` and `STREX`.
- **kuser Page (`0xffff0000`)**:
  - Map `G + 0xffff0000` (`PROT_READ | PROT_EXEC`) and populate real A32 instruction sequences at `0xffff0f60` (`__kuser_cmpxchg64`), `0xffff0fa0` (`__kuser_memory_barrier`), `0xffff0fc0` (`__kuser_cmpxchg`), `0xffff0fe0` (`__kuser_get_tls`), and `uint32_t` version `5` at `0xffff0ffc`.
- **Performance Loop**:
  - Before optimizing translation quality or runtime overhead (`M2`+), follow the 6-step loop in `~/.config/ai/AGENTS.md`: 1. Instrument (`--stats` / `A32RT_STATS=1`) → 2. Measure → 3. Hypothesise → 4. Fix → 5. Measure again → 6. Validate or retract (`git checkout --`).
