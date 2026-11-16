# Extracting `a32rt/src/ios/` into `hyper-linux` (`docs/plans/ios-hl-extraction.md`)

**Status**: Planned architectural refactoring (paired with `../rt-series/docs/plans/darwin-personality-boundary.md` and `../hyper-linux/docs/plans/ppc-darwin/02-cross-repo-execution-contract.goal`).

## 1. Objective

Keep `a32rt` (like `ppc32rt`) **small and focused** on:
1. AArch32 (`ARMv7-A` / `Thumb-2` / `VFPv4` / `NEON`) CPU state, reference interpreter (`src/interp/`), AArch64 DBT + persistent AOT (`src/dbt/`, `src/core/`), and 4 GiB guest memory window / 16-byte Soft-TLB (`src/vm/`);
2. Standalone 32-bit Linux ELF userspace (`src/elf32.c`, `src/stack32.c`, `src/signals.c`, `src/os/`);
3. A **narrow, versioned external guest-execution & trap contract** (matching `ppc-darwin` Goal `G02`) so `hyper-linux` (`hl`) drives non-Linux OS personalities externally.

All 32-bit Darwin/iOS OS personality modules currently under `a32rt/src/ios/` (`16` files: Mach-O/FAT loader, dylib/export-trie/bind resolver, commpage/stack setup, BSD syscalls, `kqueue`/`kevent`, Mach traps/ports/MIG, Darwin signals, `pthread`/`psynch`/`ulock`, and synthetic `libSystem` stubs) migrate into **`../hyper-linux`** alongside `hl`'s existing `ios/*` workstreams and the incoming 32-bit PowerPC Darwin (`ppc-darwin` + `ppc-darwin-ui`) personality.

## 2. Module Migration Map (`a32rt/src/ios/` -> `hyper-linux`)

| Current `a32rt` Source | Destination Role in `hyper-linux` (`hl`) | Shared vs. Arch-Specific |
|---|---|---|
| `src/ios/ios_macho.c`, `ios_dylib.c`, `ios_export.c`, `ios_bind.c` | Unified 32-bit Mach-O / FAT admission, `LC_LOAD_DYLIB`, export-trie, and rebase/bind engine (endian-parameterized for LE `CPU_TYPE_ARM` and BE `CPU_TYPE_POWERPC`). | Shared Mach-O core + arch relocation/stub hooks |
| `src/ios/ios_commpage_stack.c` | Arch-specific commpage + initial Mach-O stack/applev builder (`0xFFFF4000` iOS ARMv7 vs. `0xFFFF8000` Mac OS X PPC32). | Arch-specific layout |
| `src/ios/ios_svc_bsd*.c` (`bsd`, `fs`, `proc`, `sock`, `kevent`) | XNU BSD syscall personality (`svc #0x80` on ARMv7, `sc` `r0 >= 0` on PPC32), reusing validated guest-memory copyin/copyout helpers. | Mostly shared XNU 32-bit BSD layer (endian-aware structs) |
| `src/ios/ios_svc_mach.c` | Mach port namespace, `mach_msg` / MIG descriptors, semaphore/clock/VM traps (`r12 < 0` / `svc #0x80` on ARMv7, `r0 < 0` / `sc` on PPC32). | Shared Mach IPC core + arch trap ABI |
| `src/ios/ios_threads.c`, `ios_signals.c` | XNU `bsdthread` / `psynch` / `ulock` + `_sigtramp` / `mcontext` frame builder driving narrow `a32rt` / `ppc32rt` thread-context & register snapshot APIs. | Shared lifecycle + arch `mcontext` |
| `src/ios/ios_stubs.c`, `ios_stubs_ext.c`, `ios_stubs_time.c` | `hl` Wine-style synthetic `libSystem` compatibility mode (paired with `hl`'s native system-bundle mode and UI presentation services). | Shared stub dispatch + small arch trampoline bytes |

## 3. Phased Cutover Rules (Zero Regression)

1. **Freeze `G02` Cross-Repo Contract First**: Define the versioned execution, guest-VA memory copy/map/protect, trap (`SVC` / `BKPT` / `FAULT`), thread-context, and AOT cache-namespace ABI across `rt-series`, `a32rt`, `ppc32rt`, and `hyper-linux`.
2. **Stand Up & Qualify `hl` iOS Path Before Deleting `a32rt/src/ios/`**: Keep `make test-ios` (`61 / 61` PASS) green in `a32rt` until `hl` passes the full 61-test 32-bit iOS suite through the narrow `G02` contract with `--difftest` (`0` divergences) and warm AOT parity.
3. **Retire `a32rt/src/ios/` Cleanly**: Once `hl` owns and qualifies the 32-bit iOS personality, remove `src/ios/*` (`~6,500` LOC) from `a32rt`, leaving only the narrow `G02` host-driven personality hook (`<= 300` LOC) in `a32rt`.
