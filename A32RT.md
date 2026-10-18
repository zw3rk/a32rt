# Rosetta-style AArch32 → AArch64 user-mode translator — specification & architecture (`a32rt`)

Status: specification & architecture, rev 4, 2026-10-18. Rev 2 added §5, integration with `zw3rk/hyper-linux` (`hl`), and reworked §4, §6 and §8 around a host-agnostic `a32rt`. Rev 3 aligned with `hyper-linux` `HEAD` (`d76cd75`), resolved `[M]` items against primary sources, and locked in the implementation choices: **Curated C11 + minimal AArch64 assembly**, **zero external dependencies** (bespoke A32/T32 → A64 translator and 32-bit A64 emitter from `M0`, no Dynarmic/oaknut/CMake), and **`M0-hl` primary verification** with `limactl` (`lima-aarch64`) as the ground-truth Linux oracle. **Rev 4** records the completed `M0`–`M5` (`M0-hl`–`M5-hl`) implementation and specifies **Milestone `M6` / `M6-hl` (Next-Horizon Enhancements & Hardening)**: direct in-cache slot chaining, 24-slot `v8`–`v31` lazy-write-back VFP & 128-bit NEON (`s0`–`s31`, `d0`–`d31`, `q0`–`q15`) register caching, base-address-independent relocatable `.so` AOT caches (`A32_AOT_VERSION 5`), ARMv8-A AArch32 ISA extensions (`CRC32`, Crypto, Directed-Rounding FP), `VFP_MAGIC` signal frames, and multi-library real-world dynamic linking.

Tags: **[V]** = checked against a primary source that loaded during research; **[M]** = from memory / not re-verified, check before relying on it; **[hl: file:function]** = grounded in the hyper-linux source at `../hyper-linux` (commit `d76cd75`). All external links returned HTTP 200 at time of writing.

---

## 1. Executive summary

- A Rosetta-style arm32→arm64 translator already exists and ships: **Tango** (Amanieu Systems). It does AOT pre-translation into persistent caches plus a JIT fallback, has a small GPL kernel module, and runs on Xiaomi 14 / OnePlus 12 (Snapdragon 8 Gen 3, no AArch32). It is closed source and commercially licensed. [V]
- Its research ancestor **MAMBO-X64** (PLDI'17) reports a SPEC CPU2006 geomean overhead under 7.5% on Cortex-A53, and a 1% *speedup* over native AArch32 on X-Gene 1. The source does not appear to be public. [V]
- The open-source state of the art is **Dynarmic** (C++20 JIT-only library, A32/T32 guest, AArch64 host, 0BSD) and **qemu-user** (TCG, no persistent cache). Nothing open does AOT plus a JIT plus a complete Linux compat-ABI layer for arm32-on-arm64.
- Proposed component: **`a32rt`**, a standalone, zero-dependency, static-PIE `aarch64-linux` ELF written in **Curated C11 + minimal AArch64 assembly** with the same guest-side contract as Apple's RosettaLinux. It takes an AArch32 ELF, issues **only native aarch64 Linux syscalls**, and does all compat-ABI and ISA translation itself. One binary runs unchanged inside `hl` on macOS, under `binfmt_misc` on AArch64 Linux (`limactl` / native hardware), and in other loaders.
- Host extras are optional and negotiated through a small, documented `/dev/a32host` ioctl handshake (§5.2). Examples: 32-bit-clean directory offsets, a guaranteed 4K page size, an AOT cache service, and a hint for W^X / I-cache behaviour. Without the handshake, a32rt falls back to plain-Linux behaviour.
- Under `hl`, Apple Silicon's lack of AArch32 is sidestepped by running `a32rt` as a normal aarch64 guest. `hl`'s 4 KB stage-1 granule also removes the 16K-page problem that Asahi has. Because `hl`'s low memory is already occupied (`[vvar]`/`[vdso]` at `0xE000`/`0xF000`, `[shim]` at `0x100000`, ELF/stack/RX arena from `0x400000` to `256 MB`, `[shim-data]` at `2 GiB`), `a32rt` supports an **offset-base mode** (`G = 0x1_0000_0000`, i.e. `[4 GiB, 8 GiB)`) from day one (§5.1).
- Plan: build our own bespoke A32/T32 → A64 translator and direct 32-bit A64 instruction emitter in Curated C11 from **M0**, with zero external dependencies. Verify **M0-hl** under `hl` on macOS Apple Silicon as the primary target and **`limactl` (`lima-aarch64`)** as the ground-truth Linux oracle. Then add dynamic linking (`M1`/`M1-hl`), superblock/AOT optimization + CoreMark (`M2`/`M2-hl`), threads/signals/SMC (`M3`), VFPv4/NEON (`M4`), LTP/GDB hardening (`M5`), and next-horizon enhancements (`M6`/`M6-hl`, §4.2).

---

## 2. Prior art

### 2.1 Directly relevant (arm32 guest → arm64 host)

| Name | Links | Mechanism | Status | Numbers stated by source |
|---|---|---|---|---|
| **Tango** (Amanieu Systems) | https://www.amanieusystems.com/ · tech: https://www.amanieusystems.com/technology · perf: https://www.amanieusystems.com/performance · compat: https://www.amanieusystems.com/compatibility · downloads: https://www.amanieusystems.com/download-v2-1-0 · kernel module: https://github.com/AmanieuSystems/tango32-module | User-mode DBT. **Dynamic translator**: fragments go into a per-process code cache shared across threads. **Pre-translator** (AOT): discovers code from ELF entry points, follows branches recursively, builds a CFG for cross-fragment liveness and register-allocation hints, and writes a persistent cache file per ELF. Android: system libraries are pre-translated into the image, and apps are pre-translated at install/dexopt via the package manager. Linux: invoked via binfmt_misc. [V] | v1.0 Jul 2019 → v2.1.0 2024-11-08. Android 12–15. Evaluation images for Pixel 6 (GrapheneOS) and an AWS Ubuntu 22.04 AMI; the evaluation license limits programs to 4 h runtime. [V] | "Usually within 10% of native". The X1/V1 translated code can beat native A32: Geekbench 5 runs 40% faster in A64 than A32 on the Pixel 6 X1. LTP 32-bit syscalls 1006/1015 (99.11%). Top 1000 Android apps pass. 20,000+ h of RISU fuzzing. [V] (the per-benchmark charts are images and were not read) |
| **MAMBO-X64** (d'Antras, Gorgovan, Garside, Luján; Manchester) | paper PDF: https://pure.manchester.ac.uk/ws/portalfiles/portal/56078084/pldi_16.pdf · PLDI'17 page: https://pldi17.sigplan.org/details/pldi-2017-papers/28/Low-Overhead-Dynamic-Binary-Translation-on-ARM | Process-level JIT DBT, one instance per 32-bit process. Contributions: dynamic mapping of AArch32 FP registers onto A64 registers; speculative address generation (assume reg+offset doesn't overflow 32 bits, fix up on fault); return-aware traces that use hardware return prediction; precise, race-free signal delivery; fast atomic hash tables for indirect branches. [V] | Research. The public `beehive-lab/mambo` repo is the **same-ISA** DBI (ARM32, AArch64, RISC-V), **not** MAMBO-X64. [V] | SPEC CPU2006 geomean < 7.5% on A53. +1% on X-Gene 1 (per-benchmark range +38% to −19%). Disabling speculative address generation takes gcc from +0.4% to −11.1%. No comparison against QEMU. [V] |
| **HyperMAMBO-X64** (VEE'17) | cited from the Tango tech page above | Uses ARMv8 virtualization extensions for system-transparent translation. [V for existence; details M] | Research | — |
| **Dynarmic** | https://github.com/azahar-emu/dynarmic · design: https://github.com/azahar-emu/dynarmic/blob/master/docs/Design.md · license: https://github.com/azahar-emu/dynarmic/blob/master/LICENSE.txt | C++20 JIT library. Guests: ARMv3 to 32-bit v8 plus A64. Hosts: x86-64 and AArch64. You bring your own memory system (callbacks, page table, or fastmem base). It has an interpreter-fallback hook. There is no OS layer and no AOT. [V] | Mature, from the Citra/yuzu lineage. The old upstream `github.com/merryhime/dynarmic` now returns **404**; the Azahar fork is live. License: "dynarmic is under a 0BSD license" (README) [V], with separate bundled C++ dependencies (`fmt`, `mcl`, `oaknut`, `tsl-robin-map`). Not used by `a32rt` (which is zero-dependency C11). | None stated |
| **touchHLE** | https://touchhle.org/ · https://github.com/touchHLE/touchHLE | HLE emulator for early iPhone OS apps. It reimplements Foundation, UIKit, GLES, OpenAL and others, and runs only the app binary plus a few dylibs on Dynarmic. [V] | Active, multiple contributors. Official builds: x64 Windows, x64 macOS, AArch64 Android. AArch64 macOS/Linux "probably works, build yourself". [V] | — |
| **qemu-user + binfmt_misc** | TCG docs: https://www.qemu.org/docs/master/devel/tcg.html · binfmt: https://www.kernel.org/doc/html/latest/admin-guide/binfmt-misc.html | Block-level JIT through the generic TCG IR, with an in-memory TB cache and TB chaining. No persistent/AOT cache. Syscalls are translated in userspace (linux-user). [V for TCG design; specifics M] | Mature, the de facto baseline | No arm32→arm64 vs native numbers found from a primary source |

### 2.2 Adjacent (useful techniques, wrong guest/host pair)

- **rev.ng**: https://github.com/revng/revng. Uses QEMU's TCG front end ("libtinycode") to lift ARM and other binaries into LLVM IR. Static recompilation was tested on statically linked uClibc/musl ARM ELFs. A research/decompiler tool, not a runtime. [V]
- **Remill**: https://github.com/lifting-bits/remill. Lifts machine code to LLVM bitcode. The README says "AArch32 (32-bit ARMv8 / ARMv7) support is underway"; AArch64, x86/amd64 and SPARC are supported. [V] McSema (built on Remill) is deprecated [M].
- **HQEMU**: https://homepage.iis.sinica.edu.tw/papers/dyhong/18239-F.pdf. TCG for fast first translation, plus LLVM optimization of hot traces on other cores. About 2.4–2.5× faster than QEMU on SPEC int, measured with an **ARM guest on an x86-64 host**. [V via search summary of the paper; re-read before citing numbers]
- **Android Native Bridge**: https://android.googlesource.com/platform/art/+/refs/heads/main/libnativebridge/README.md. ART's hook for loading foreign-ISA `.so`s. AOSP "does not provide an actual native bridge". [V]
- **Berberis**: https://android.googlesource.com/platform/frameworks/libs/binary_translation/+/refs/heads/main/README.md. Google's Native Bridge implementation, **riscv64 guest → x86_64 host** only. [V] Intel **Houdini**: ARM→x86, proprietary [M].
- **FEX-Emu**: https://github.com/FEX-Emu/FEX. x86/x86-64 on ARM64 only. [V] **Box64**: https://github.com/ptitSeb/box64. x86-64 guest only; 32-bit x86 goes through Box86/Box32. [V] FEX and Box64 are good references for 16K-page hosts, thunking host libraries, and AOT caches [M].
- **MAMBO (same-ISA DBI)**: https://github.com/beehive-lab/mambo. A clean C codebase (`pie/` decoder generator) for A32/T32 decode and scanner tables. [V]

### 2.3 Platform context

- **Linux asymmetric AArch32** (5.15, Will Deacon, commit 2122a833316f): https://docs.kernel.org/arch/arm64/asymmetric-32bit.html. Opt in with `allow_mismatched_32bit_el0`. Capable CPUs are listed in `/sys/devices/system/cpu/aarch32_el0`, and 32-bit tasks are affinity-restricted to them. KVM refuses AArch32 EL0 on asymmetric systems. On a CPU with **no** AArch32, `execve` of a 32-bit ELF gets `-ENOEXEC`, so binfmt_misc can route it to the translator. [V]
- **Android**: AOSP has a `core_64_bit_only.mk` product config (`TARGET_SUPPORTS_32_BIT_APPS := false`). Pixel 7 (Oct 2022) disabled 32-bit apps in software even though Tensor G2 has AArch32-capable cores, and all later Pixels are 64-bit-only, with no translator: https://arstechnica.com/gadgets/2022/10/the-pixel-7-tests-out-64-bit-only-android-cant-run-32-bit-apps/ [V]. Xiaomi 14 and OnePlus 12 (China builds) ship Tango: https://www.androidpolice.com/oneplus-12-workaround-to-run-32-bit-apps/ [V]. No evidence was found of MediaTek, Huawei or Samsung arm32 layers.
- **Windows 11 24H2** removed Arm32 app support, and Prism does not emulate Arm32: https://learn.microsoft.com/en-us/windows/arm/arm32-to-arm64 [V]
- **Apple**: iOS 11 dropped 32-bit apps, and A11+ / M-series cores have no AArch32 [V]. No Apple translator exists.
- **Kernel kuser helpers** (arm32 ABI detail you must emulate): https://docs.kernel.org/arch/arm/kernel_user_helpers.html [V]

---

## 3. Technical problem breakdown

### 3.1 Register and state mapping
- **GPRs**: guest `r0–r14` map to fixed host `W` registers (A64 has `x0–x30`). That leaves 15+ host registers for the guest window base (`xG`), context pointer (`xCtx`), dispatch/IBTC scratch registers, and temporaries. Every 32-bit A64 operation on a `W` register zero-extends to 64 bits, which matches guest 32-bit wrap-around semantics and allows direct `[xG, wN, uxtw]` addressing. [V]
- **PC as GPR**:
  - In **A32**, reading `PC` yields `instr_addr + 8`. [V]
  - In **T32**, `ADR`, `LDR (literal)`, and `VLDR (literal)` use `Align(PC, 4) = (instr_addr + 4) & ~3u`, whereas `MOV Rd, pc` and `ADD Rd, pc, Rm` use unaligned `instr_addr + 4`. [V]
  - All `PC` reads are compile-time constants at translation time.
  - Writes to `PC` (`BX`, `BLX`, `LDR pc`, `POP {..,pc}`, `LDM` with `pc`, and A32 ALU writes without `S`) are indirect branches. Bit 0 selects Thumb (`T=1`) vs ARM (`T=0`) on `BX`, `BLX`, `LDR pc`, `LDM/POP {..,pc}`, and A32 ALU writes to `PC` in ARMv7-A; in T32, `MOV pc, Rm` and `ADD pc, Rm` branch within Thumb without interworking. [V]
- **NZCV** maps 1:1 onto A64 NZCV. Both architectures define `C` for subtraction as NOT borrow, so `ADDS/SUBS/ADCS/SBCS/CMP/CMN` translate directly. The pitfalls:
  - A32 logical ops with `S` (`ANDS/ORRS/EORS/MOVS/BICS/MVNS/TST/TEQ`) set **C from the barrel-shifter carry-out** and leave **V unchanged**. A64 `ANDS` sets `C=0, V=0`. Emit either explicit `N`/`Z` updates plus shifter `C` merge (preserving `V`) or eliminate dead flag writes when the next instruction overwrites `NZCV`. [V]
  - A64 has no `ORRS/EORS`; emit `ORR/EOR` followed by flag updates when flags are live. [V]
  - **Q** (sticky saturation) and **GE[3:0]** (SIMD-within-a-register, `SADD8` etc.) have no host counterpart. Keep them in the context struct (`a32_ctx_t`), updated on saturating/parallel-arithmetic instructions. [V]
- **Shifted operands**:
  - Immediate `LSL/LSR/ASR` map onto A64 shifted-register forms; `ROR` immediate maps to `EXTR` (or logical shifted-register `ROR`). [V]
  - Register-specified shifts (`Rm, LSL Rs`) use the bottom byte `Rs[7:0]` with A32 semantics for shift amounts `>= 32` (`LSL/LSR >= 32` yields `0`, `ASR >= 32` yields sign bit). A64 `LSLV/LSRV/ASRV` takes the shift amount modulo 32 (`Rs[4:0]`), so clamp when `Rs[7:5] != 0`. [V]
  - `RRX` (`(C << 31) | (Rm >> 1)`, carry-out = `Rm[0]`) is synthesized with 2–3 A64 instructions. [V]
  - Rotated A32 immediates and Thumb modified immediates expand to constants at translation time (including shifter carry-out when `S=1`). [V]
- **Conditional execution**: almost every A32 instruction is conditional. Options:
  - Branch around the instruction (`B.!cond skip`) — simplest and general for stores/syscalls/multi-instruction lowerings.
  - `CSEL`/`CSINC`/`CCMP` for pure ALU ops when `S=0` (best for short runs).
  - T32 **IT blocks** (up to 4 instructions) carry an 8-bit `ITSTATE` (`CPSR.IT[7:0]`). Asynchronous signals are delivered at fragment boundaries (`ITSTATE == 0`), while **synchronous faults** (`SIGSEGV`/`SIGBUS`) inside an `IT` block reconstruct `CPSR.IT[7:0]` from the per-fragment fault side table (§3.6). [V]
- **LDM/STM/PUSH/POP**: lower to 32-bit `LDP/STP wA, wB, [x_addr, #imm]` pairs (plus a trailing `LDR/STR` for odd register counts) and base writeback. Handle architectural edge cases: base register in the register list (load overwrites base vs store value), and `PC` in the load list (interworking indirect branch). `LDRD/STRD` map to `LDP/STP w`. [V]
- **Exclusives and atomics**:
  - 8/16/32-bit `LDREX/STREX{B,H}` map 1:1 to `LDXR/STXR{B,H}`. [V]
  - 64-bit `LDREXD Rt, Rt2, [Rn]` / `STREXD Rd, Rt, Rt2, [Rn]` must lower to **64-bit `LDXR x_tmp, [x_addr]` / `STXR w_rd, x_tmp, [x_addr]`** with `BFI`/`UBFX` to pack/unpack `(Rt, Rt2)`, **not** 128-bit `LDXP/STXP` (`LDXP/STXP` is 128-bit and is not single-copy atomic without `FEAT_LSE2`, whereas 64-bit `LDXR/STXR` is single-copy atomic on all ARMv8.0+ cores). [V]
  - The translator must not insert host loads/stores, spills, or cache lookups between `LDREX` and `STREX`, or the exclusive monitor can clear and livelock the guest loop. Keep `LDREX…STREX` basic blocks in a single trace/superblock when possible. [V]
  - `CLREX` → `CLREX`. `DMB/DSB/ISB` → `DMB/DSB/ISB`. [V]
  - `SWP/SWPB` (deprecated in ARMv6+): emulate via `LDXR/STXR` loop. [V]
- **Memory model**: AArch32 and AArch64 share the ARMv8 memory model [V], so no TSO-style fencing is needed (unlike x86→ARM translation).
- **Unaligned access**: ARMv7-A permits unaligned `LDR/STR/LDRH/STRH` (`SCTLR.A=0`), which A64 `LDR/STR` supports natively on Normal memory. [V]
- **Endianness**: support little-endian (`ELFDATA2LSB`) only. Trap `SETEND` if non-LE is requested.

### 3.2 VFP/NEON
- Guest register banks:
  - `S0–S31` alias `D0–D15`: `S(2n)` is the low 32 bits of `Dn` and `S(2n+1)` is the high 32 bits of `Dn`. [V]
  - `D0–D31` (VFPv3-D32 / VFPv4-D32 / NEON); `Qn = D(2n):D(2n+1)` (`Q0–Q15`). [V]
- Host bank: `V0–V31`, 128 bits each.
- Mapping choices:
  - (a) Guest `Qn` → host `Vn` (`V0–V15`). 128-bit `Q` ops and even `D(2n)` ops are native; odd `D(2n+1)` and odd `S` registers use lane instructions (`INS`, `UMOV`, `DUP`) or scratch `V16–V31` unpacking.
  - (b) Guest `Dn` → host `Vn` low 64 bits (`V0–V31`). Scalar `D0–D31` and even `S(2n)` are direct; `Q` ops pack/unpack pairs.
  - (c) MAMBO-X64/Tango style: **dynamic per-fragment view tracking** that avoids redundant pack/unpack across basic blocks. Start with (b) or (a) for initial VFP support, then add per-fragment view tracking in `M4`.
- **FPSCR**: maps onto A64 `FPCR` + `FPSR`. The deprecated VFP short-vector fields (`FPSCR.LEN/STRIDE`) are trapped if set non-zero. [V]
- **Standard FPSCR for Advanced SIMD**: in AArch32, NEON floating-point instructions always use the architectural Standard FPSCR value (Round-to-Nearest, Flush-to-Zero `FZ=1`, Default NaN `DN=1`), whereas VFP scalar instructions use `FPSCR`. On A64, both scalar FP and AdvSIMD read `FPCR`. Toggle or cache `FPCR` mode around NEON float ops vs VFP ops. [V]
- Exception flags: `FPSCR.QC` maps to `FPSR.QC`. [V]

### 3.3 ARM/Thumb interworking
- The instruction set state is selected by bit 0 of interworking branch targets (`T=1` Thumb, `T=0` ARM). Translation-cache keys always include the `T` bit: `key = (guest_pc & ~1u) | T`. [V]
- T32 decode is mixed 16-bit and 32-bit (`halfword[15:11]` in `{0b11101, 0b11110, 0b11111}` indicates a 32-bit Thumb-2 instruction). [V]
- Code discovery for AOT handles mixed ARM and Thumb within a single ELF using `.dynsym`/`.symtab` bit 0, AAELF mapping symbols (`$a`, `$t`, `$d`), and `.ARM.exidx` unwind tables. [V]
- Literal pools (`$d`) sit inline between code; the translator stops linear decode at unconditional branches (`B`, `BX`, `POP {..,pc}`, `LDR pc`) so inline data pools are never decoded as instructions.

### 3.4 Self-modifying code and JITs inside the guest
- arm32 Linux user code invokes the private syscall `__ARM_NR_cacheflush` (`0x0f0002`, `r0 = start`, `r1 = end`, `r2 = flags`) after writing instructions. [V] This is the primary invalidation hook to evict translated fragments intersecting `[start, end)`.
- Robustness (`M3`): also write-protect guest pages that have active translations (`mprotect(PROT_READ)`), catching `SIGSEGV` writes to invalidate stale translations before re-enabling `PROT_WRITE`.

### 3.5 Indirect branches
- Inline / data-page hash table (IBTC) keyed on `(guest_pc | T)` mapping to the translated host code pointer, plus direct-branch chaining via data-page chain slots (`ldr x16, [slot]; br x16`).
- Return prediction: maintain a small **return address stack** (RAS) of `(guest_ret_pc_t, host_ret_addr)` so `BX LR` / `POP {..,PC}` compare against the predicted guest return address and execute a host `RET` on hit. [V]

### 3.6 Signals and precise state
- Host signals fall into three classes (Tango / MAMBO-X64 model [V]):
  - **Internal**: used by `a32rt` itself (e.g., SMC write-protection faults), never visible to the guest.
  - **Synchronous**: `SIGSEGV`/`SIGBUS`/`SIGILL`/`SIGFPE` caused by a translated guest instruction. Map `host_pc → (guest_pc, T, itstate, cond)` via per-fragment side tables, materialize `CPSR` (including `T` bit and `IT[7:0]`), and deliver to the guest signal handler.
  - **Asynchronous**: mark pending in the thread context and deliver at the next safe point (fragment exit).
- Build an arm32 `rt_sigframe` / `ucontext` on the guest stack (`struct sigcontext` with `r0–r15`, `cpsr`, `fault_address`, VFP state in `uc_regspace`), redirect execution to the guest handler with `lr` pointing to a sigreturn trampoline, and handle arm32 `sigreturn` (119) / `rt_sigreturn` (173). [V]

### 3.7 Address space
- The guest lives in a contiguous 4 GiB window `[G, G + 4 GiB)` inside the host process:
  - When `G = 0` (plain Linux with low 4 GiB available above `mmap_min_addr`), guest VA equals host VA.
  - In **offset mode** (`G != 0`, e.g. `G = 0x1_0000_0000` under `hl`), a dedicated host register `xG` holds `G`. Every guest memory access computes `x_addr = xG + uxtw(w_guest_addr)` (`add x_addr, xG, w_guest_addr, uxtw`), so 32-bit guest wrap-around (`reg + imm` overflowing `2^32`) wraps cleanly in 32-bit `W` arithmetic before adding `xG`.
- `a32rt` reserves `[G, G + 4 GiB)` up front with `mmap(..., PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0)` and manages all guest `mmap2`/`mremap`/`munmap`/`brk` allocations inside that window.
- **Page size**: under `hl` (`4 KB` stage-1 granule) and `limactl` (`4 KB` kernel), host page size matches the 4 KB arm32 ABI (`AT_PAGESZ = 4096`).

### 3.8 Syscall ABI
- EABI convention: `svc #0`, syscall number in `r7`, arguments in `r0–r5` (up to 6 register args). 64-bit arguments (`loff_t`, `uint64_t`) are aligned to **even/odd register pairs** (`r0:r1`, `r2:r3`, `r4:r5`), and `fadvise64_64` (270) / `sync_file_range2` (341) reorder arguments so 64-bit pairs fit in `r0–r5` without spilling. [V]
- Private ARM syscalls (`__ARM_NR_BASE = 0x0f0000`) [V]:
  - `__ARM_NR_breakpoint = 0x0f0001`
  - `__ARM_NR_cacheflush = 0x0f0002`
  - `__ARM_NR_set_tls    = 0x0f0005`
  - `__ARM_NR_get_tls    = 0x0f0800`
- Struct layout translations between 32-bit ARM EABI (`ILP32`, 4-byte `long`/pointers, 64-bit `ino_t`/`off64_t` in `*64` structs) and 64-bit `aarch64-linux` (`LP64`):
  - `stat64`, `statx`, `statfs64`, `timespec` (`time32` and `*_time64` syscalls)
  - `iovec`, `msghdr`/`cmsghdr`, `sigaction`/`siginfo_t`/`ucontext_t`, `rlimit`, `epoll_event`, `flock64`
  - `getdents64` (`d_off` 64-bit cookie remapping to 31-bit positive `off_t` cookies when `A32H_CAP_DIROFF32` is not advertised)

### 3.9 TLS, vDSO, kuser helpers, /proc, threads
- **TLS**: guest reads `TPIDRURO` via `MRC p15, 0, Rd, c13, c0, 3` (and `__kuser_get_tls`) and sets it via `__ARM_NR_set_tls` (`0x0f0005`) or `clone(CLONE_SETTLS)`. Store `tls_tp` in `a32_ctx_t` and lower `MRC p15, 0, Rd, c13, c0, 3` to a single `LDR wRd, [xCtx, #offsetof(a32_ctx_t, tls_tp)]`. Never clobber host `TPIDR_EL0`. [V]
- **kuser helpers** at guest `0xffff0000–0xffff0fff` (https://docs.kernel.org/arch/arm/kernel_user_helpers.html [V]):
  - Map a 4 KB page at `G + 0xffff0000` (`PROT_READ | PROT_EXEC`) and populate actual A32 machine code at the architectural entry points:
    - `0xffff0f60` (`__kuser_cmpxchg64`): `ldrexd` / `strexd` loop + `bx lr`
    - `0xffff0fa0` (`__kuser_memory_barrier`): `dmb ish; bx lr`
    - `0xffff0fc0` (`__kuser_cmpxchg`): `ldrex` / `strex` loop + `bx lr`
    - `0xffff0fe0` (`__kuser_get_tls`): `mrc p15, 0, r0, c13, c0, 3; bx lr`
    - `0xffff0ffc` (`__kuser_helper_version`): `uint32_t` word `5`
  - Because real A32 instructions and the version word live at `G + 0xffff0000`, guest data reads of `*(uint32_t *)0xffff0ffc` and direct branches/calls into `0xffff0f60..0xffff0fe0` work seamlessly through normal translation without special-case PC hooks.
- **vDSO & auxv**: omit `AT_SYSINFO_EHDR` initially (libc falls back to syscalls); synthesize arm32 `AT_HWCAP`, `AT_HWCAP2`, `AT_PAGESZ = 4096`, `AT_PLATFORM = "v8l"`, `AT_RANDOM`, `AT_PHDR`, `AT_PHENT`, `AT_PHNUM`, `AT_BASE`, `AT_ENTRY`, `AT_EXECFN`.
- **`/proc`**: intercept `/proc/self/exe`, `/proc/self/maps`, `/proc/self/auxv`, `/proc/self/cmdline`, and `/proc/cpuinfo` in `a32rt`'s `openat`/`readlinkat` shim so the guest sees its own arm32 mappings and identity.

---

## 4. Architecture (`zw3rk/a32rt`)

### 4.0 Component boundary: `a32rt` as a zero-dependency guest-side translator
```
            ┌──────────────── host (any of) ────────────────┐
            │ hl (macOS HVF VM) | Linux kernel + binfmt_misc │
            │ other loader (container shim, Native Bridge)   │
            └───────▲───────────── aarch64 Linux syscall ABI ┘
                    │  (+ optional /dev/a32host handshake, §5.2)
  a32rt (static-PIE aarch64-linux ELF, Curated C11 + minimal A64 .S, zero external deps)
    ├─ loader: guest ELF32 + ld-musl-armhf / ld-linux-armhf into guest window [G, G+4G)
    ├─ JIT: bespoke A32/T32 → A64 translator + direct 32-bit A64 emitter; arbitrary (pc, T) entry
    ├─ AOT cache: lookup by (SHA-256 of ELF, a32rt build-id, CPU feature mask)
    ├─ compat ABI: arm32 EABI syscalls → aarch64 syscalls, struct and ioctl translation
    ├─ signals: aarch64 host frame → arm32 guest frame; precise state via side tables
    └─ emulated in-process: kuser page, TPIDRURO, /proc/self/{exe,maps,auxv,cmdline}, guest brk
a32aot = `a32rt --aot IN OUT` (same binary, offline mode)
```

Design rules (all host-agnostic):
1. **Curated C11 + minimal AArch64 assembly, zero external dependencies.** Same engineering discipline as `hyper-linux` (`-std=c11 -Werror=vla -pedantic -Wall -Wextra`, self-documenting root `Makefile` + `flake.nix`, no C++, no CMake, no Dynarmic, no oaknut).
2. **No host-specific code paths.** Behaviour differences come only from (a) probing standard syscalls, (b) auxv, and (c) the optional `/dev/a32host` handshake.
3. **Static-PIE, no fixed high addresses, no pointer tagging.** The host places `a32rt` anywhere above the guest window.
4. **Guest window `[G, G+4 GiB)`, reserved up front** with `mmap(MAP_FIXED_NOREPLACE, PROT_NONE)`. `G` is resolved from `A32RT_WINDOW` > `/dev/a32host` `window_hint` > `0` (with fallback scan for a free 4 GiB-aligned `[G, G+4 GiB)` window when `G=0` fails). All guest memory accesses add `xG` to zero-extended 32-bit guest addresses (`[xG, wAddr, uxtw]`).
5. **In-process JIT that accepts any `(pc, T)` at any time.** AOT only seeds the cache. A cache miss always translates on demand and never asserts (§5.5).
6. **W^X-agnostic code cache.** Write translated A64 instructions into staging buffers or RW pages in batches, then `mprotect(PROT_READ | PROT_EXEC)` once per batch. Chain links and IBTC tables live in **data** pages.
7. **Batched I-cache maintenance.** Flush once per translated block/batch. When `A32H_CAP_ICACHE_TRAP_IALLU` is advertised by `/dev/a32host` (under `hl`), a single `IC IVAU` + `ISB` suffices for the entire batch because `hl`'s HVC #12 handler already executes `IC IALLU`.
8. **Self-tracked guest mappings and `brk`.** `a32rt` allocates its own memory with `mmap` outside `[G, G+4 GiB)`, emulates guest `brk` and `mmap2` inside `[G, G+4 GiB)`, and synthesizes `/proc/self/maps` from its own region table.

### 4.1 Bespoke A32/T32 → A64 Translator & Direct Emitter
- **Architecture (`src/`)**:
  - `a64_emit.h` / `a64_emit.c`: compact, dependency-free 32-bit AArch64 instruction encoder (`ADD/SUB/AND/ORR/EOR/EXTR`, `LSLV/LSRV/ASRV/RORV`, `MOVZ/MOVK/MOVN`, `CSEL/CSINC/CCMP`, `LDR/STR/LDP/STP`, `LDXR/STXR/CLREX/DMB/DSB/ISB`, `B/BL/B.cond/BR/BLR/RET/SVC`).
  - `decode_a32.c` & `decode_t32.c`: table/bitfield-driven A32 and T32 (16-bit & 32-bit Thumb-2) decoders lowering directly into A64 instructions (or a flat micro-IR for superblock flag liveness in `M2`).
  - `trampoline.S`: host↔guest context switch (`a32_enter_jit`), saving host callee-saved registers (`x19–x29, x30`), loading `xG`, `xCtx`, and guest `r0–r14`/`NZCV` into mapped host registers, jumping to translated code, and returning to the C dispatcher on `SVC`, unhandled indirect branch, or fault.
- **Fixed Register Allocation**:
  - Guest `r0–r14` (`r13 = sp`, `r14 = lr`) live in fixed host `W` registers across basic blocks (`w0–w14`).
  - Host `x19` = `xCtx` (`a32_ctx_t *`), `x20` = `xG` (`guest_base`), `x15–x17` = translator scratch (`w15` effective 32-bit address, `x16/x17` branch/helper scratch).

### 4.2 Next-Horizon Architecture & Hardening (`M6` / `M6-hl`)
1. **Direct In-Cache Slot Chaining (`slot_key[0..1]`, `chain_entry[0..1]`) & In-Trampoline `RAS` / `IBTC`**:
   - Each compiled basic block records its up to two static same-mode direct branch exit keys (`slot_key[0]` for primary/fallthrough/unconditional exits and `slot_key[1]` for taken conditional branch exits).
   - Static direct exits emit a compact position-independent inline dispatch sequence that checks `pending_signals | flags | itstate` (`ldp x17, x23, [x19, #368]`) and `cur_entry->chain_entry[S]` (`[x27, #48 + S*8]`), updating `x27` (`cur_entry`), incrementing `x29` (`chain_hits_delta`), and branching directly (`br x17`) to the target block inside `g_code_cache` without returning to `trampoline.S`.
   - Because each basic block has its own dedicated `br x17` instruction per outgoing edge, the host CPU's Indirect Branch Predictor (BTB) predicts each static edge with zero multi-way aliasing, while keeping code pages strictly `PROT_READ | PROT_EXEC` (W^X).
   - Polymorphic indirect branches (`bx rm`, `pop {.., pc}`, vtables) fall back to the 1024-entry lock-free `IBTC` (`ctx->ibtc`) and 64-entry per-thread `RAS` (`ctx->ras`) inside `trampoline.S`.
2. **24-Slot (`v8`–`v31`) Lazy-Write-Back VFP & 128-Bit NEON Register Cache (`s0`–`s31`, `d0`–`d31`, `q0`–`q15`)**:
   - Host callee-saved `d8`–`d15` are preserved in `a32_jit_run_block` prologue/epilogue, unlocking all 24 host SIMD registers `v8`–`v31` across basic blocks (`128`-instruction window).
   - `a64_emit.c` tracks cached `S` (`A64_VFP_KIND_S`), `D` (`A64_VFP_KIND_D`), and 128-bit `Q` (`A64_VFP_KIND_Q`) registers in `v8`–`v31` with dirty bitmasks (`vfp_slot_dirty`) and clean-first eviction so modified FP/SIMD accumulators stay pinned in hardware registers across inner loops.
   - Cross-width aliasing (`s[2n..2n+1]` ↔ `d[n]` ↔ `q[n>>1]`) automatically flushes and invalidates overlapping views; dirty slots are flushed before guest memory operations (preserving exact `ctx->vfp_d` state on synchronous `SIGSEGV`), conditional branch edges, and block exits.
3. **Base-Address-Independent (Relocatable) Shared-Library `.a32cache` Blocks (`A32_AOT_VERSION 5`)**:
   - Every guest-PC-derived immediate in `decode_a32.c`, `decode_t32.c`, and `vfp_neon.c` is emitted via `a64_emit_mov_pc32`, which emits a `MOVZ`/`MOVK` pair and records its word index in `b->pc_relocs[]`.
   - `.a32cache` stores `hdr.reserved = base_bias` and each block's `pc_relocs[]` table. When a shared library (`ET_DYN` `.so`) or `PT_INTERP` is mapped at an arbitrary ASLR `load_bias` via `mmap2(PROT_EXEC)`, `a32_aot_load_fd` adds `delta = load_bias - hdr.reserved` to `guest_pc`, `guest_end_pc`, `slot_key[0..1]`, `pc_map[]`, and each `MOVZ`/`MOVK` pair in `pc_relocs[]`, installing pre-translated blocks with zero runtime recompilation.
4. **ARMv8-A AArch32 User-Mode ISA Extensions & `VFP_MAGIC` Signal Frame**:
   - Hardware CRC-32 (`CRC32{B,H,W}` and Castagnoli `CRC32C{B,H,W}`), Cryptography Extensions (`AESE.8`, `AESD.8`, `AESMC.8`, `AESIMC.8`, `VMULL.P64`, `SHA1{C,P,M,H,SU0,SU1}.32`, `SHA256{H,H2,SU0,SU1}.32`), and Directed-Rounding Floating-Point (`VSEL{EQ,VS,GE,GT}.F32/.F64`, scalar & vector `VMAXNM`/`VMINNM`, `VRINT{A,N,P,M,Z,R,X}.F32/.F64`, `VCVT{A,N,P,M}.{S32,U32}.{F32,F64}`), advertised via `AT_HWCAP2 = 0x1f` and `/proc/cpuinfo`.
   - Authentic Linux ARM `VFP_MAGIC` (`0x56465001`, size `0x120`) coprocessor frame in `rt_sigframe.uc_regspace` / `sigframe.uc_regspace` saving and restoring `d0`–`d31` and `fpscr` across `rt_sigreturn` / `sigreturn`.
5. **Real-World Multi-Library Dynamic Linking (`M6` / `M6-hl`)**:
   - Multi-library dynamic linking verified with Alpine `armv7` binaries loading multiple `.so` dependencies (`apk --version` loading `ld-musl-armhf.so.1`, `libapk.so.2.14.0`, `libcrypto.so.3`, `libssl.so.3`, `libz.so.1.3.2`, plus `scanelf -V` and `getconf LONG_BIT`).

---

## 5. Integration with `zw3rk/hyper-linux` (`HEAD` `d76cd75`)

Goal: make `arm32-linux` binaries run under `hl` on macOS **without coupling `a32rt` to `hl`**. `hl` treats `a32rt` like RosettaLinux: an external static `aarch64-linux` ELF that `hl` loads above 4 GiB and hands the `arm32` binary to on `AT_EXECFD = 3`.

### 5.1 Relevant `hl` facts (verified against `hyper-linux` `d76cd75`)

| Fact | Where (`hyper-linux` `d76cd75`) | Consequence for `a32rt` & `hl` |
|---|---|---|
| Stage-A P2 added `elf_probe_file(path, &probe)` and `hl_identity_resolve()`, called in `main()` and `sys_execve()` before `elf_load()`. Both `elf_probe_file` and `elf_load` currently reject `EI_CLASS != ELFCLASS64` | `elf.c:elf_probe_file`, `elf.c:elf_load`, `guest_identity.h:hl_identity_resolve` | Extend `elf_probe_file` to parse `elf32_ehdr_t`/`elf32_phdr_t` when `EI_CLASS == ELFCLASS32 && e_machine == EM_ARM (40)`, populating `PT_INTERP` and `PT_NOTE` for identity resolution while leaving `elf_load` 64-bit-only (§5.4) |
| `x86_64` is detected in `main` (`need_rosetta = e_machine == EM_X86_64`) and uses a two-phase setup (`rosetta_prepare` before page tables, `rosetta_finalize` after vCPU creation) | `hl.c:main`, `rosetta.c:rosetta_prepare`, `rosetta.c:rosetta_finalize` | Template for `a32_prepare` / `a32_finalize` (`src/a32.c`), without Rosetta's VA alias or TTBR1 `kbuf` |
| Stage-1 uses the **4 KB granule**; `AT_PAGESZ = 4096` always | `hl.c:main`; `stack.c:build_linux_stack` | **No 16K-page problem under `hl`**. Advertise `A32H_CAP_PAGE4K` |
| Low guest VA layout: `[vvar]` `0xE000`, `[vdso]` `0xF000`; `[shim]` code `0x100000` (`SHIM_BASE`), `HL_LOW_PROTECT_END = 0x200000`; ELF/PIE base `0x400000`; `brk >= 16 MB`; stack top and RX arena from `256 MB`; **`[shim-data]` at `0x80000000` (`2 GiB`, `[2 GiB, 2 GiB + 2 MiB)`, EL1-only `MEM_PERM_NONE`)**; RW `MMAP_BASE = 8 GiB`. Both `[shim]` and `[shim-data]` are tracked in `g->regions[]` | `guest.h:48-133`; `hl.c:1094-1099` | **`G = 0` is occupied, and `2 GiB` has `SHIM_DATA_BASE`.** Offset mode with **`G = 4 GiB` (`[0x1_0000_0000, 0x2_0000_0000)`)** sits cleanly above `SHIM_DATA_BASE` (`2 GiB`) and below `MMAP_BASE` (`8 GiB`) |
| Sub-2MB `MAP_FIXED` / `mprotect` over `PROT_NONE`: when `a32rt` reserves `[G, G+4G)` with `PROT_NONE` (no L2 blocks created) and then maps a 4 KB/64 KB segment at `G + 0x10000`, `sys_mmap` calls `guest_extend_page_tables(..., page_perms)` which creates the entire 2 MiB block `[G, G+2M)` with `page_perms`, and `guest_update_perms` skips splitting because `old_perms == page_perms` — leaving `G + 0` (the guest NULL page!) mapped | `syscall.c:1586`, `syscall.c:2098`, `guest.c:2427` | **Fix in `hl`**: in `sys_mmap` (`is_fixed && !is_prot_none`) and `sys_mprotect`, call `guest_extend_page_tables(..., MEM_PERM_NONE)` first, then `guest_update_perms(..., page_perms)` so partial 2 MiB blocks split into L3 pages and keep unmapped neighbours `MEM_PERM_NONE` |
| Fork: generation-1 fork uses COW `shm_fd` when `!g->is_rosetta`, but `guest_init_from_shm` sets `g->shm_fd = -1` in the child and `guest_reset` sets `shm_fd = -1` on `execve`. Thus **generation-2 (grandchild) forks and post-`execve` forks** use `guest_get_used_regions()`, which currently skips `MAP_FIXED` regions in `[4 GiB, 8 GiB)` | `fork_ipc.c:sys_clone_fork`, `guest.c:350`, `guest.c:1223`, `syscall_exec.c:694` | Keep `is_rosetta = 0` for generation-1 COW fork, **and** update `guest_get_used_regions()` to emit non-`PROT_NONE` regions in `[4 GiB, MMAP_BASE)` plus the `a32rt` image range (`g->a32rt_guest_base..+g->a32rt_size`) |
| Default VFS mode is **host passthrough (`HL_FS_LEGACY`)** unless `--sysroot`, `--fs-mode=rooted`, or `--bind` is passed | `hl.c:98-105` | Local file AOT caching under `$A32RT_CACHE` or `$XDG_CACHE_HOME/a32rt` persists on the host in both default passthrough mode and rooted mode |
| HVF enforces W^X (HVC #9 toggles 4 KB pages RW↔RX) and traps EL0 `IC IVAU`/`DC CVAU` (HVC #12 performs `IC IALLU`) | `shim.S`; `syscall_proc.c:vcpu_run_loop` cases 9 & 12 | `a32rt` batches `mprotect(RX)` and uses a single `IC IVAU` per batch when `A32H_CAP_ICACHE_TRAP_IALLU` is set |
| `/dev` is a registry (`hl_device_register`), and `test/test-docs-counts.c` pins `179 syscall dispatch cases` in `syscall.c` | `device.c`; `test/test-docs-counts.c`; `AGENTS.md:169` | Register `/dev/a32host` via `hl_device_register()` and add **zero** new cases to `syscall_dispatch` |

### 5.2 Host contract (`docs/HOST-CONTRACT.md`, `include/a32host.h`, `A32H_ABI 1.0`)

1. **Invocation**:
   - `a32rt [a32rt-opts] -- PATH ARGV0 [ARGS…]`: explicit form used by `hl` and other loaders.
   - binfmt **P** form `argv = [a32rt, PATH, ARGV0, ARGS…]` (when `argv[0]` ends with `a32rt-binfmt-p` or `A32RT_BINFMT_P=1`).
   - Legacy non-P form `[a32rt, PATH, ARGS…]` (`ARGV0 := PATH`).
   - If `AT_EXECFD` is present in host `auxv`, `a32rt` loads the target binary from that fd instead of `openat(PATH)`.
2. **Syscalls**: standard `aarch64-linux` syscalls only.
3. **Address space**: a 4 GiB-aligned `[G, G+4 GiB)` window below `2^47`, reservable with `PROT_NONE`.
4. **Optional `/dev/a32host` handshake**:
   ```c
   struct a32h_info {            /* all fields little-endian, 256 bytes total */
     uint16_t abi_major, abi_minor;  /* 1, 0 */
     uint32_t flags;                 /* reserved, 0 */
     uint64_t caps;                  /* A32H_CAP_* bitmask */
     uint64_t window_hint;           /* preferred G (0x100000000 on hl), 0 = none */
     uint32_t page_size;             /* guaranteed mmap granularity (4096) */
     uint32_t _pad;
     char     aot_socket[108];       /* AF_UNIX path, "" = none */
     uint8_t  _reserved[116];        /* pad to 256 bytes */
   };
   ```
   Graceful fallback when `/dev/a32host` returns `ENOENT`/`ENODEV`/`EACCES`/`ENOTTY` or when `A32RT_HOST=none`.

### 5.3 Repo layout
- **Separate repository `zw3rk/a32rt`** with its own `flake.nix` and self-documenting root `Makefile`, matching `hyper-linux/docs/SIBLING-REPOS.md`.
- `hl` consumes `a32rt` at runtime via `--a32rt PATH` > `$HL_A32RT` > default package path, and vendors `include/a32host.h` (`src/a32host.h`).
- Cross-building armhf musl test fixtures in Nix uses:
  ```nix
  armv7lMuslPkgs = import nixpkgs {
    localSystem = "x86_64-linux";
    crossSystem = { config = "armv7l-unknown-linux-musleabihf"; };
  };
  ```
  (Verified `[V]`: `pkgsCross.armv7l-hf-multiplatform` is glibc `gnueabihf`, `.pkgsMusl` fails in nixpkgs for 32-bit ARM, and `pkgsCross.muslpi` is `armv6l`.)

### 5.4 `hl`-side changes (`M0-hl` & `M1-hl`)
1. **Detection (`src/elf.h`, `src/elf.c`)**: extend `elf_probe_file()` to accept `ELFCLASS32 && ELFDATA2LSB && EM_ARM (40)`, parsing `elf32_ehdr_t` and `elf32_phdr_t` so `hl_identity_resolve()` receives `PT_INTERP` and `PT_NOTE` while `elf_load()` stays 64-bit-only.
2. **`src/a32.h` / `src/a32.c`**:
   - `a32_prepare()`: resolve `a32rt` path (`--a32rt` > `$HL_A32RT` > default), `elf_load(a32rt_path)`, map it 2 MB-aligned just below `g->interp_base`, record `g->a32rt_guest_base` and `g->a32rt_size`.
   - `a32_finalize()`: build vDSO, pre-open the `arm32` ELF at guest fd 3 (`AT_EXECFD = 3`), construct `argv = [a32rt, "--", path, argv[0], argv[1]...]`, pass `A32RT_WINDOW=0x100000000` (and `A32RT_SYSROOT` when `--sysroot` is set), and call `proc_set_elf_path(path)` / `proc_set_cmdline(original_argv)`.
3. **Sub-2MB `MAP_FIXED` / `mprotect` and Fork Fixes (`src/syscall.c`, `src/guest.c`, `src/syscall_exec.c`)**:
   - In `sys_mmap` (`is_fixed && !is_prot_none`) and `sys_mprotect`, extend unmapped L2 blocks with `MEM_PERM_NONE` before `guest_update_perms()`.
   - In `guest_get_used_regions()`, include `g->a32rt_guest_base..+g->a32rt_size` and non-`PROT_NONE` regions in `[4 GiB, MMAP_BASE)`.
   - Wire `need_a32` into `src/hl.c:main` and `src/syscall_exec.c:sys_execve`; set process title to `"<name> (armv7-linux)"`; allow `--gdb`.
4. **`/dev/a32host` (`src/device.c`)**: register `/dev/a32host` via `hl_device_register()` returning `A32H_CAP_PAGE4K | A32H_CAP_WX_ENFORCED | A32H_CAP_ICACHE_TRAP_IALLU`, `window_hint = 0x100000000ULL`, `page_size = 4096`.

### 5.5 Lessons from `lode/rosetta.md` applied to `a32rt`
- **Static CFG recovery is incomplete**: the AOT cache is a *seed*; any unknown `(pc, T)` at runtime translates on demand via the JIT.
- **No pending-fixup state across blocks**: every translated block is self-contained, exiting via data-page chain slots or the dispatcher.
- **Single-process execution**: no two-process `ptrace` split.
- **Documented, optional `/dev/a32host` handshake**: no secret signatures; graceful fallback on `ENOENT`/`ENOTTY`.
- **Plain user VA only**: static-PIE, no pointer tagging, no TTBR1 kernel-VA mmaps.
- **Self-tracked `/proc/self/maps`**: `a32rt` tracks its own guest mappings rather than parsing host `/proc/self/maps`.

---

## 6. Phased plan

Explore first (`ITERATE_FAST` / `BUILD` mode): break internal APIs freely, no premature hardening, **one runnable vertical slice and one acceptance check per milestone**. Primary verification target is **`M0-hl` under `hl` on macOS Apple Silicon**, with **`limactl` (`lima-aarch64`)** as the ground-truth Linux oracle. Harden at `M5`.

| Milestone | Scope | Exit criterion (one check) |
|---|---|---|
| **M0** (portable `a32rt`) | `zw3rk/a32rt` repo in Curated C11 + minimal A64 `.S`; zero external deps; `flake.nix` + self-documenting `Makefile`; bespoke A32/T32 → A64 translator + direct 32-bit A64 emitter covering static-hello integer/branch/load-store/exclusive/CP15 subset; parametric `[G, G+4G)` window reservation (`G=0` default, `A32RT_WINDOW` / `/dev/a32host` override); static `arm32` ELF loader + initial stack/auxv + kuser page (`0xffff0000`); EABI syscall shim (`write, writev, read, openat, close, fstat64, exit, exit_group, brk, mmap2, munmap, mprotect, uname, ioctl, set_tls, get_tls, cacheflush, set_tid_address, getrandom, clock_gettime64, rt_sigaction/rt_sigprocmask` stubs) | Under `limactl` (`lima-aarch64`, ground-truth Linux oracle): `./a32rt -- ./hello-armhf-static hello` prints `hello` and exits `0` |
| **M0-hl** (primary target) | `hl` integration per §5.4 (`elf_probe_file` ELF32 support, `src/a32.c`, sub-2MB `MAP_FIXED`/`mprotect` L2 split fix, `guest_get_used_regions` window support, `/dev/a32host` device, `execve` branch). `a32rt` runs in **offset mode** (`G = 4 GiB`) | On macOS Apple Silicon: `make test-a32-hello` (`hl --a32rt <path>/a32rt ./hello-armhf-static`) prints `hello` and exits `0` |
| **M1** | Full A32/T32 integer ISA coverage in the bespoke translator; dynamic linking (`ld-musl-armhf` / `ld-linux-armhf` via `PT_INTERP` + `A32RT_SYSROOT`); file/stat/dir/fork/exec/wait syscalls (`getdents64` cookie remap, `/proc/self/{exe,maps,auxv,cmdline}`, self re-exec for `arm32→arm32` `execve`) | Under `limactl` (`lima-aarch64`): dynamically linked armhf `busybox sh -c 'ls / && echo ok'` prints listing + `ok` and exits `0` |
| **M1-hl** | `--sysroot` wired to `a32rt` under `hl`; single-instruction `IC IVAU` batch flush via `A32H_CAP_ICACHE_TRAP_IALLU`; generation-2 fork verification (`busybox sh -c 'ls / && echo ok'`) | On macOS: `hl --sysroot <armhf> busybox sh -c 'ls / && echo ok'` prints listing + `ok` and exits `0` |
| **M2** | Default-off `--stats` instrumentation; dead-NZCV flag liveness over blocks/superblocks; `CSEL`/`CSINC`/`CCMP` conditional lowering; RAS return prediction; data-page IBTC + direct-branch chaining; `a32rt --aot` pre-translator + local file cache (`$XDG_CACHE_HOME/a32rt`) | Warm-cache `coremark` (`armhf -O2`) passes CRC under `limactl` (`lima-aarch64`); report score vs `qemu-arm` and native AArch64 |
| **M2-hl** | Warm-cache `coremark` under `hl`; measure `wxcount_to_rx/rw` and HVC #12 exits (`hl -v`) | `coremark` CRC passes under `hl`; report score, W^X toggle count, and HVC #12 exit count |
| **M3** | Threads (`clone` with `CLONE_VM|CLONE_THREAD|CLONE_SETTLS`, `futex`, shared code cache with epoch invalidation, userspace 32-bit robust lists), precise synchronous + asynchronous signals (32-bit `rt_sigframe` + `itstate` side tables), `cacheflush` + `mprotect` SMC detection | Threaded + signal smoke test (LTP `futex*`/`sigaction*` subset) passes under `hl` and `limactl` |
| **M4** | VFPv4-D32 and Advanced SIMD (NEON) in the bespoke translator; dynamic per-fragment FP view tracking; Standard-FPSCR handling for NEON float ops | Embench / FP+SIMD benchmark subset produces exact outputs under `hl` and `limactl` |
| **M5** (`HARDEN`) | LTP 32-bit syscall suite; compat ioctls; guest-level GDB RSP stub (`A32RT_GDB_PORT`); `hl` matrix target `make test-a32-*` | LTP 32-bit syscall pass-rate report + `hl` test matrix green |
| **M6** (`NEXT-HORIZON`) | Direct in-cache slot chaining (`slot_key[0..1]`, inline `br x17`), 64-entry in-trampoline `RAS`; 24-slot (`v8`–`v31`) lazy-write-back VFP + 128-bit NEON (`s0`–`s31`, `d0`–`d31`, `q0`–`q15`) register cache; relocatable `.so` AOT cache (`A32_AOT_VERSION 5` `pc_relocs[]`); ARMv8-A AArch32 `CRC32`, Crypto, Directed-Rounding FP (`AT_HWCAP2 = 0x1f`); `VFP_MAGIC` (`0x56465001`) `uc_regspace` signal frame; multi-library dynamic linking (`apk`, `libcrypto.so.3`, `libssl.so.3`, `libz.so.1`, `scanelf`, `getconf`) | `make test-all-lima` + `make bench-lima` green; relocatable `.so` AOT cache + multi-library dynamic linking verified |
| **M6-hl** (`NEXT-HORIZON`) | Full `M6` feature set verified under `hl` (`G = 4 GiB` offset mode) + 6-way `make bench-hl` comparison (`aarch64` native, `RosettaLinux`, `a32rt` A32 & Thumb-2 Warm AOT & Cold JIT) | `make test-all-hl` + `make bench-hl` green (`blocks_compiled = 0` on warm AOT) |

---

## 7. Open questions and risks
- **Licensing**: `zw3rk/a32rt` is authored from scratch in Curated C11 under `Apache-2.0` (`Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.`) with zero third-party translator dependencies. Avoid copying GPLv2 code from `qemu-user` or `tango32-module`.
- **Patents and IP**: Amanieu Systems is commercial, and the MAMBO-X64 techniques are published. Do a freedom-to-operate check before commercial deployment.
- **Offset-mode codegen cost under `hl`**: `G = 4 GiB` requires `add x_addr, xG, w_guest, uxtw` before memory accesses (`[xG, w_guest, uxtw]` handles register+0 in a single instruction; `reg + imm` uses an extra `add`). Measure in `M2-hl` before considering any `LOW4G_FREE` layout change in `hl`.
- **W^X and I-cache traps under `hl`**: `a32rt` emits code in batches, calls `mprotect(PROT_READ | PROT_EXEC)` once per batch, and keeps chain slots / IBTC entries in RW data pages to avoid HVC #9 RW↔RX ping-pong.
- **Contract drift**: keep `include/a32host.h` ABI-versioned (`A32H_ABI 1.0`) and owned by `a32rt`, with a vendored copy in `hyper-linux/src/a32host.h`.

---

## 8. Agent briefs

### 8.1 Brief A: `a32rt` M0 (standalone Curated C11 repo, zero dependencies)
```
Goal: M0 of "a32rt", a Rosetta-style AArch32→AArch64 Linux user-mode translator,
built as a standalone component (repo: zw3rk/a32rt). It must have ZERO external
dependencies and ZERO dependencies on hyper-linux: written in Curated C11
(~/.config/ai/C_GUIDELINES.md) + minimal AArch64 assembly, built via a
self-documenting root Makefile and flake.nix.
Phase: ITERATE_FAST (BUILD). Break internal APIs freely, no premature hardening.

Context: read A32RT.md §3, §4, §5.2 (host contract), §6, and AGENTS.md.

Scope (M0 only):
- Repo with flake.nix (packages.aarch64-linux.a32rt static-PIE musl + cross-built
  armv7l-unknown-linux-musleabihf hello fixture) and self-documenting Makefile.
- Bespoke A32/T32 → A64 translator and direct 32-bit A64 instruction emitter in C11
  (src/a64_emit.c, src/decode_a32.c, src/decode_t32.c, src/jit.c, src/trampoline.S).
- Guest window [G, G+4GiB): reserve with mmap(MAP_FIXED_NOREPLACE, PROT_NONE).
  Resolve G from A32RT_WINDOW > /dev/a32host window_hint > 0 (fallback to 4GiB scan).
- Loader: static arm32 ELF (ELFCLASS32, EM_ARM=40) into the window; arm32 initial stack
  (argc/argv/envp/auxv: AT_PAGESZ=4096, AT_HWCAP, AT_HWCAP2, AT_RANDOM, AT_PHDR/PHENT/PHNUM/ENTRY,
  AT_PLATFORM="v8l").
- Invocation: support `a32rt [opts] -- PATH ARGV0 [ARGS]`, binfmt P-form, and legacy form.
  Use AT_EXECFD if present.
- Syscall shim on SVC #0 (nr in r7, args r0–r5): write, writev, read, openat, close,
  fstat64, exit, exit_group, brk (emulated inside window), mmap2 (4K units, inside window),
  munmap, mprotect, uname, ioctl, set_tls (0x0f0005), get_tls (0x0f0800),
  cacheflush (0x0f0002), set_tid_address, rt_sigprocmask/rt_sigaction (stub 0),
  getrandom, clock_gettime64.
- kuser page at G+0xffff0000 populated with real A32 helper instructions and version 5 at 0xffff0ffc.
- Probe `/dev/a32host` (`A32H_GET_INFO`); fall back cleanly on ENOENT/ENOTTY.

Verification:
- Primary target (with Brief B): `make test-a32-hello` under `hl` on macOS Apple Silicon.
- Ground-truth Linux oracle: `./a32rt -- ./hello-armhf-static hello` inside `limactl` (`lima-aarch64`).
```

### 8.2 Brief B: `hyper-linux` integration (`M0-hl`)
```
Goal: let `hl` run arm32-linux ELFs by delegating to an external `a32rt` binary, the way
it delegates x86_64 to RosettaLinux. `hl` must not link or include `a32rt` internal code
(only the shared `a32host.h` ioctl header).
Phase: ITERATE_FAST (BUILD). Minimal diff, one acceptance check.

Context: A32RT.md §5.1–§5.4 (`hyper-linux` `d76cd75`). Follow `hyper-linux/AGENTS.md`
(Curated C11, HVC protocol, W^X, 179 syscall dispatch cases invariant).

Changes:
1. src/elf.h & src/elf.c: extend `elf_probe_file()` to accept `ELFCLASS32 && ELFDATA2LSB && EM_ARM (40)`,
   parsing `elf32_ehdr_t` and `elf32_phdr_t` for `PT_INTERP` and `PT_NOTE`. Keep `elf_load()` 64-bit-only.
2. src/a32.h & src/a32.c: implement `a32_prepare()` and `a32_finalize()`:
   - resolve `a32rt` path via `--a32rt PATH` > `$HL_A32RT` > default;
   - load `a32rt` 2MB-aligned just below `g->interp_base`;
   - pre-open arm32 ELF at fd 3 (`AT_EXECFD=3`), set `argv = [a32rt, "--", path, argv0, ...]`,
     inject `A32RT_WINDOW=0x100000000`, call `proc_set_elf_path(path)` and `proc_set_cmdline(original_argv)`.
3. src/syscall.c & src/guest.c:
   - in `sys_mmap` (`is_fixed && !is_prot_none`) and `sys_mprotect`, call
     `guest_extend_page_tables(..., MEM_PERM_NONE)` before `guest_update_perms(..., page_perms)`
     so sub-2MB mappings split L2 blocks cleanly without mapping the guest NULL page;
   - in `guest_get_used_regions()`, include `g->a32rt_guest_base..+g->a32rt_size` and
     non-`PROT_NONE` regions in `[4 GiB, MMAP_BASE)`.
4. src/device.c: register `/dev/a32host` via `hl_device_register()` (`A32H_GET_INFO`).
5. src/hl.c & src/syscall_exec.c: wire `need_a32`, keep `is_rosetta = 0`, set title `"<name> (armv7-linux)"`,
   allow `--gdb`.

Verification:
- `make test-a32-hello` (`_build/hl --a32rt <path>/a32rt ./hello-armhf-static` printing `hello`, exit 0).
```

---

## 9. M2 / M2-hl Benchmark Results & Verification Summary

Measured on Apple Silicon (`macOS` host running `../hyper-linux/_build/hl` and `limactl` `hl-rosetta` AArch64 Linux VM) using `tests/bench_coremark.c` (`2,000` CoreMark-style iterations: 4×4 matrix multiply, 16-node linked-list sort, state-machine tokenizer, and CRC-16 validation `0x41dd`, compiled with Clang 21 `-O2` and NEON loop/SLP auto-vectorization enabled):

| Target / Mode | Guest Window `G` | Runtime `blocks_compiled` | `icache_flushes` (W^X batches) | `flags_elided` | `HVC #12` Hot-Loop Exits | Elapsed Time (`2,000` iters) | Throughput (`iter/sec`) |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| **Native `aarch64-linux` (`hl`)** | N/A (64-bit) | — | — | — | `0` | `1.23 ms` | `1,628,664` |
| **Native `aarch64-linux` (`limactl`)** | N/A (64-bit) | — | — | — | N/A | `0.98 ms` | `2,036,659` |
| **`a32rt` A32 Cold JIT (`hl`)** | `0x1_0000_0000` | `38` | `38` | `1` | `0` | `31.53 ms` | `63,437` |
| **`a32rt` A32 Warm `.a32cache` (`hl`)** | `0x1_0000_0000` | `13` (`-65.8%`) | `14` (`-63.2%`) | `0` (AOT) | `0` | `8.45 ms` (`3.73x` faster) | `236,770` |
| **`a32rt` T32 Warm `.a32cache` (`hl`)** | `0x1_0000_0000` | `20` (`-48.7%`) | `21` (`-46.2%`) | `73` | `0` | `10.32 ms` | `193,723` |
| **`a32rt` A32 Cold JIT (`limactl`)** | `0x0` | `38` | `38` | `1` | N/A | `10.20 ms` | `196,001` |
| **`a32rt` A32 Warm `.a32cache` (`limactl`)** | `0x0` | `13` (`-65.8%`) | `14` (`-63.2%`) | `0` (AOT) | N/A | `8.86 ms` | `225,682` |
| **`a32rt` T32 Cold JIT (`limactl`)** | `0x0` | `39` | `39` | `88` | N/A | `9.27 ms` | `215,773` |
| **`a32rt` T32 Warm `.a32cache` (`limactl`)** | `0x0` | `20` (`-48.7%`) | `21` (`-46.2%`) | `73` | N/A | `10.72 ms` | `186,532` |

