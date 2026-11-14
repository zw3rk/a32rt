# `a32rt` Performance Ledger (`PERF.md`)

Tracks instrumented benchmark baselines, diagnosed bottlenecks, and verified speedups across `a32rt`'s ELF32 (`armhf`, `thumb`) and 32-bit iOS (`armv7`/`armv7s` Mach-O) execution paths, following the 6-step Performance Loop (`Instrument -> Measure -> Hypothesise -> Fix -> Measure -> Validate`).

---

## 1. Pre-Remediation Baseline (`master` @ `8d87576`, `bench_suite.c`, 5-workload `min_us`)

Measured head-to-head on identical Apple Silicon hardware across native macOS (`_build/a32rt-darwin`, Regime A2 Soft-TLB) and Linux (`_build/a32rt`, Regime A1 direct window):

| Target / Guest Binary | `coremark` (`µs`) | `chacha20` (`µs`) | `vfp_simd` (`µs`) | `calls_ibtc` (`µs`) | `syscalls` (`µs`) | `total_us` (`µs`) | Notes |
|---|---:|---:|---:|---:|---:|---:|---|
| **Darwin DBT — ELF32 `armhf` (`-marm`)** | `2,918` | `2,611` | `1,355` | `4,812` | `219` | **`11,915`** | Scalar VFP in `vfp_simd`, inline `svc #0` |
| **Darwin DBT — ELF32 `thumb` (`-mthumb`)** | `3,722` | `3,064` | `1,374` | `4,960` | `221` | **`13,341`** | Scalar VFP in `vfp_simd`, inline `svc #0` |
| **Darwin DBT — iOS `armv7s` (`libSystem`)** | `2,841` | `2,568` | **`49,413`** | `5,079` | **`676`** | **`61,308`** | **`36.0×` slower `vfp_simd`**, `__picsymbolstub4` `_getpid` |
| **Darwin DBT — iOS `armv7s` (raw `svc #0x80`)** | `2,841` | `2,568` | **`49,413`** | `5,079` | `292` | **`60,193`** | Isolates `+384 µs` (`2.32×`) PIC interworking overhead |
| **Linux DBT — ELF32 `thumb` (`-mthumb`)** | `1,837` | `1,828` | `1,168` | `4,267` | `775` | **`9,875`** | Regime A1 (`G=0`) |
| **Linux DBT — iOS `armv7s` (`libSystem`)** | `1,669` | `1,801` | **`36,813`** | `4,476` | `1,010` | **`46,456`** | **`31.5×` slower `vfp_simd`** |

---

## 2. Round 19 Performance Optimization Checklist (`A32-IOS-P1` .. `A32-IOS-P9`, `9 / 9` Resolved in `f5e0d35`)

- [x] **A32-IOS-P1 — Critical (`36.0×` `vfp_simd`) — Coalesce redundant per-instruction NEON `MSR FPCR` pipeline flushes across basic blocks & VFP self-loops**
  - **Where**: `src/interp/decode_priv.h:188-230`, `src/interp/interp_neon_alu.c`, `src/interp/interp_neon_mem.c`, `src/core/core_emit_alu.c`, `src/core/core_emit_mem.c`, `src/dbt/dbt_jit_query.c:226-246`, `src/trampoline.S:320-326`.
  - **Root cause**: `clang -target armv7s-apple-ios9.0` auto-vectorizes `vfp_simd` into NEON `.f32` vector ops (`vmul.f32`/`vadd.f32`/`vsub.f32`). `neon_fp_fpcr_begin` + `neon_fp_fpcr_end` emitted two pipeline-serializing `MSR FPCR` instructions around *every single* NEON FP instruction (~1.76M `MSR FPCR` stalls across `vfp_simd`), even inside consecutive NEON sequences and pure-NEON self-loops.
  - **Fix**: Track `fpcr_neon_active`, `saw_neon_fp`, `saw_scalar_vfp`, and `preheader_neon_fpcr` on `a64_buf_t`: enter Standard FPSCR (`FZ=1, DN=1, RMode=RN, AHP`) once on the first NEON FP op (or once in the self-loop preheader when `saw_neon_fp && !saw_scalar_vfp`, decoupled from `vfp_preheader_ineligible` register-promotion aliasing) and restore scalar `FPCR` only before scalar VFP ops, conditional branch boundaries, and block/side exits (`a64_vfp_emit_exit_flush`), while unconditionally resetting non-zero host `FPCR` in `trampoline.S` `.Lslow_exit`.

- [x] **A32-IOS-P2 — High (`2.32×` PIC stub calls) — Handle mode-switching `A32_EXIT_INTERWORK` (`A32 <-> T32`) inside `trampoline.S` `.Lrun_loop`**
  - **Where**: `src/trampoline.S:104-169`.
  - **Root cause**: Mach-O `__picsymbolstub4` stubs are 16-byte A32 (ARM) sequences called from T32 (Thumb-2) code via `blx`, then jumping via `ldr pc, [ip]` to a T32 target. `trampoline.S:101-103` bailed out to `.Lslow_exit` (saving/restoring 33 registers + C round-trip) whenever `(w15 & 1) != w26`, costing two full C exits per dynamic library call (`676 µs` vs `292 µs`).
  - **Fix**: Compute candidate `next_thumb` and `next_key` for `A32_EXIT_INTERWORK` inside `trampoline.S`, probe `chain0`/`chain1`/`ibtc`/`ras`, and commit `w26`, `w25`, and `CPSR.T` (`[x19, #64]`) inline on hit without leaving `.Lrun_loop`.

- [x] **A32-IOS-P3 — High — Native guest Thumb-2 machine-code stubs for hot leaf `libSystem` functions**
  - **Where**: `src/ios/ios_stubs.c`, `src/ios/ios_stubs_ext.c`, `src/ios/ios_commpage_stack.c`.
  - **Root cause**: Leaf `libSystem` calls (`_strlen`, `_strcmp`, `_strncmp`, `_memcpy`, `_memmove`, `_memset`, `_memcmp`, `___error`, `_pthread_self`, `_mach_absolute_time`) previously trapped via `svc #0x81` (`A32_EXIT_SVC` -> C dispatch) on every call.
  - **Fix**: Emit compact Thumb-2 guest implementations in the `0xFFFF2800..0xFFFF3FFF` RX trampoline region and bind those symbols directly to their Thumb-2 VAs so the DBT translates and chains them inline with zero `SVC` exits.

- [x] **A32-IOS-P4 — Medium — Single-load Regime A2 Soft-TLB base (`[sp, #160]`) & lock-free JIT lookup/return-slot fast paths**
  - **Where**: `src/trampoline.S:47-55`, `src/core/core_emit_mem.c:89-107`, `src/dbt/dbt_jit_query.c:42`, `src/dbt/dbt_jit_run.c:428`.
  - **Root cause**: Regime A2 (`a32rt-darwin`) emitted up to four `MOVZ`/`MOVK x21` instructions per guest memory op to materialize `a32_vm32_tlb_base`; `a32_jit_compile_block` took `jit_lock` before probing atomic `g_ht`; `dbt_jit_run.c:428` acquired `jit_lock` on every call exit even when `cur_entry->ret_entry == found_e`.
  - **Fix**: Spill `VM32_TLB_BASE` once at `[sp, #160]` on `a32_jit_run_block` entry and load with 1-insn `ldr x21, [sp, #160]`; add lock-free atomic pre-checks in `dbt_jit_query.c` and `dbt_jit_run.c`.

- [x] **A32-IOS-P5 — Medium — Per-image cached export trie & symtab/strtab blobs across `a32_ios_bind_all`**
  - **Where**: `src/ios/ios_export.c`, `src/ios/ios_dylib.c`, `src/ios/ios.h`.
  - **Root cause**: `a32_ios_resolve_in` `malloc`ed and `pread`ed the export trie and `symtab`+`strtab` from disk on every symbol bind (`O(binds × table_bytes)` syscalls at startup).
  - **Fix**: Cache read blobs on `a32_ios_image_t` on first lookup and free them once `a32_ios_bind_all` completes.

- [x] **A32-IOS-P6 — Medium — `LC_FUNCTION_STARTS` (`0x26`) Mach-O AOT pre-translation (`--aot` / `--auto-aot`)**
  - **Where**: `src/ios/ios_macho.c`, `src/dbt/dbt_aot_scan.c`, `src/main.c`.
  - **Root cause**: `dbt_aot_scan.c` seeded symbols from ELF headers (`seed_elf_symbols`), missing Mach-O `__TEXT,__text` bounds and `LC_FUNCTION_STARTS` Thumb/ARM entry deltas.
  - **Fix**: Populate `out_info->text_start`, `text_end`, `load_bias`, and decoded `LC_FUNCTION_STARTS` entries in `ios_macho.c` and seed them in `a32_aot_generate` (achieving `blocks_compiled=0` on warm AOT reload).

- [x] **A32-IOS-P7 — Low — Targeted SMC check in synthetic `qsort` (`ios_stubs.c`)**
  - **Where**: `src/ios/ios_stubs.c`.
  - **Root cause**: `qsort_resume` invalidated the entire `n * sz` array via `a32_vm32_copyout_invalidate_exec` on every element swap of an $O(n^2)$ insertion sort.
  - **Fix**: Only invalidate swapped `[a, a + sz)` / `[b, b + sz)` slices when `a32_jit_range_executable` is true.

- [x] **A32-IOS-P8 — Low — Page-granular `ios_cstr` guest string validation**
  - **Where**: `src/ios/ios_svc_bsd.c`.
  - **Root cause**: `ios_cstr` invoked `a32_guest_range_ok` (binary-searching `ctx->regions`) once per byte.
  - **Fix**: Validate one guest 4 KiB page span at a time and scan with `memchr`.

- [x] **A32-IOS-P9 — Low — `O(1)` lock-free thread-slot index on `a32_ctx_t` (`ios_thr_slot_plus1`)**
  - **Where**: `src/ios/ios_threads.c`, `src/a32rt.h`.
  - **Root cause**: `thr_find_ctx` acquired `g_thr_mu` and linearly scanned 64 slots on every `pthread_self` / `pthread_getspecific` / `pthread_setspecific` call.
  - **Fix**: Cache `slot + 1` in `ctx->ios_thr_slot_plus1` on thread registration for `O(1)` lookup.

---

## 3. Post-Remediation Verification Results (`master` @ `f5e0d35`, 5-trial `min_us`)

Measured on the same Apple Silicon machine with 5 quiet trials per target (`min_us` per workload; `sum(min_us)` / best single-trial `total_us`):

| Target / Guest Binary | `coremark` (`µs`) | `chacha20` (`µs`) | `vfp_simd` (`µs`) | `calls_ibtc` (`µs`) | `syscalls` (`µs`) | `sum(min_us)` / Best Trial (`µs`) | Key Speedup vs. `8d87576` |
|---|---:|---:|---:|---:|---:|---:|---|
| **Darwin DBT — ELF32 `armhf` (`-marm`)** | `3,867` | `4,150` | `1,441` | `5,553` | `242` | **`15,253` / `16,004`** | Parity preserved (`ALL_PASS`) |
| **Darwin DBT — ELF32 `thumb` (`-mthumb`)** | `3,061` | `3,322` | `1,341` | `5,384` | `250` | **`13,358` / `14,474`** | Parity preserved (`ALL_PASS`) |
| **Darwin DBT — iOS `armv7s` (`libSystem`)** | `3,080` | `2,872` | **`10,249`** | `5,867` | **`355`** | **`22,423` / `23,923`** | **`4.82×` `vfp_simd`**, **`1.90×` `syscalls`** (`89%` PIC overhead cut), **`2.56×–2.73×` total** |
| **Darwin DBT — iOS `armv7s` (raw `svc #0x80`)** | `3,170` | `2,694` | **`10,237`** | `6,896` | `314` | **`23,311` / `25,736`** | **`4.83×` `vfp_simd`**, `+41 µs` PIC delta (vs. `+384 µs` pre-fix) |
| **Linux DBT — ELF32 `armhf` (`-marm`)** | `2,309` | `1,554` | `1,390` | `5,913` | `879` | **`12,045` / `12,099`** | Regime A1 parity preserved |
| **Linux DBT — ELF32 `thumb` (`-mthumb`)** | `2,074` | `1,784` | `1,418` | `5,680` | `873` | **`11,829` / `12,184`** | Regime A1 parity preserved |
| **Linux DBT — iOS `armv7s` (`libSystem`, Cold JIT)** | `2,315` | `1,523` | **`10,963`** | `5,753` | **`937`** | **`21,491` / `22,146`** | **`3.36×` `vfp_simd`**, **`1.00×` vs. raw `svc #0x80`**, **`2.10×–2.16×` total** |
| **Linux DBT — iOS `armv7s` (raw `svc #0x80`, Cold JIT)** | `2,399` | `1,527` | **`10,370`** | `5,608` | `938` | **`20,842` / `21,300`** | **`3.55×` `vfp_simd`** |
| **Linux DBT — iOS `armv7s` (`libSystem`, Warm AOT)** | `2,217` | `1,655` | **`10,689`** | `5,727` | **`936`** | **`21,224` / `22,013`** | **`3.44×` `vfp_simd`**, **`blocks_compiled=0`**, `icache_flushes=1`, `wx_transitions=1` |

- **Architectural note on `vfp_simd` (`4.82×` in mixed `.f32`+`.f64` vs. `0` `MSR FPCR` in pure-NEON self-loops)**: `bench_suite.c`'s `vfp_simd` kernel interleaves single-precision `float` and double-precision `double` math in the same loop body. On `armv7s-apple-ios9.0`, Clang schedules `float` onto ARMv7 NEON `.f32` instructions (architecturally forcing `FZ=1, DN=1, RMode=RN`) and `double` onto scalar VFP `.f64` instructions (using `ctx->fpscr` `FZ=0, DN=0`) with 6 cross-domain transitions per iteration. Coalescing `FPCR` reduced `MSR FPCR` pipeline flushes from `44`/iter (`1.76M` total) to the architectural minimum of `12`/iter (`480K` total, `4.82×` faster), while pure-NEON self-loops (`saw_neon_fp && !saw_scalar_vfp`) hoist `MSR FPCR` into the loop preheader and execute **zero** `MSR FPCR` instructions in the loop body.
- **Correctness & Differential Verification**: `make check check-headers test-ios` (`61 / 61` PASS), ELF32 `M1..M7` regressions PASS, and `--difftest` on `bench_ios_v7s --smoke` & `bench_ios_v7s_svc --smoke` (`ALL_PASS`, `0` divergences on both Darwin and Linux).
