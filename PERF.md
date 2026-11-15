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

---

## 4. Round 20: Backend Codegen, Regime A2 Soft-TLB, VFP/NEON & macOS AOT Waves (`master` @ `cc3d3aa` -> `100af25`, `16 / 16` Resolved)

### 4.1 Unthrottled Single-Core Boost Pre-Wave Profile (`cc3d3aa`, `bench_suite.c --full`, Best of 5 Idle AC Trials)

| Target / Guest Binary | `coremark` (`µs`) | `chacha20` (`µs`) | `vfp_simd` (`µs`) | `calls_ibtc` (`µs`) | `syscalls` (`µs`) | `total_us` (`µs`) | vs. Native `arm64` |
|---|---:|---:|---:|---:|---:|---:|---:|
| **Native `arm64-apple-darwin`** | `624` | `959` | `371` | `1,774` | `5` | **`3,735`** | `1.00×` |
| **Darwin DBT — iOS `armv7` (Thumb-2)** | `1,250` | `1,445` | `593` | `2,349` | `148` | **`5,785`** | `1.55×` |
| **Darwin DBT — ELF32 `thumb` (`-mthumb`)** | `1,281` | `1,491` | `582` | `2,407` | `116` | **`5,877`** | `1.57×` |
| **Darwin DBT — ELF32 `armhf` (`-marm`)** | `2,009` | `1,839` | `702` | `2,513` | `113` | **`7,176`** | `1.92×` |
| **Darwin DBT — iOS `armv7s` (mixed NEON+VFP)** | `1,274` | `1,212` | **`5,590`** | `2,351` | `144` | **`10,571`** | `2.83×` |

### 4.2 Round 20 Optimization Checklist (`Step 0` + `Waves A–E`, `16 / 16` Resolved in `100af25`)

- [x] **A32-R20-P0 (`Step 0 — Attribution`, `b4cee33`, `ff468fe`)** — Added `--stats` / `[a32rt stats-ext]` phase timers (`ns_vm_init`, `ns_translate`, `ns_wx_flush`), real APRR toggle count (`aprr_toggles`), inline RAS/IBTC hit discrimination, CP15 Generic Timer (`CNTFRQ`/`CNTVCT`), split `src/core/core_emit_exit.c`, freed `x25` (`A64_XTLB`), and added codegen/runtime counters (`tlb_checks`, `tlb_groups`, `tlb_slow`, `vfp_copies`, `fpcr_sw`, `fpcr_slow`, `aot_abs64`, `exits`).
- [x] **A32-R20-P1 (`Wave A1 — RAS Wiring & Return Gating`, `453088f`)** — Recorded unresolved `caller` in `ctx->ras[top]`, wired `caller->ret_entry` when the return block is compiled on first return (`A32-R20-D1`), and gated inline 64-entry RAS pop on `b->is_return` (`BX LR`, `MOV PC, LR`, `POP {.., pc}`, `LDR PC, [SP], #4`) ahead of inline 1,024-entry IBTC lookup (`ras_hits`: `0 -> 3,571,175`).
- [x] **A32-R20-P2 (`Wave A2 — Forward Chain Signal-Poll Elision`, `453088f`)** — Skipped `emit_exit_sig_check` on forward direct chains (`target_pc > cur_insn_pc`), keeping bounded loop-iteration signal delivery via backward edges, indirects, and C exits.
- [x] **A32-R20-P3 (`Wave A3 — Exit Epoch & LDM Probe Tightening`, `453088f`, `026566a`)** — Streamlined inline exit dispatch (`core_emit_exit.c`) and skipped redundant 2-reg `LDM` pre-probes (`core_emit_shared.c`), while preserving `ldar w21, [x28]` acquire ordering in `trampoline.S` `.Lrun_loop` paired with release cache updates.
- [x] **A32-R20-P4 (`Wave B1 — 1-Load 16B Soft-TLB & Out-of-Line Cold Stubs`, `caaee20`, `026566a`)** — Pinned `x25` (`A64_XTLB`) to `a32_vm32_tlb_base` across `.Lrun_loop`, shrunk `a32_vm32_tlb_entry_t` to 16-byte `{read_ntag, write_ntag}` negated-page-tag entries (`CMN` + `CSEL`, `32 MiB -> 16 MiB`), dropped the redundant `addend` load (`[x20, wA, uxtw]`), honored `A32_PROT_WATCH` on translated code pages (`A32-R20-D3`), and moved miss/cross-page fault stubs out-of-line at block tail with exact page-2 `f2 = addr_w + span - 1` fault addressing.
- [x] **A32-R20-P5 (`Wave B2 — Base+Offset Soft-TLB Check Grouping`, `caaee20`, `100af25`)** — Coalesced runs of same-base `[rn, #imm]` memory accesses in `src/core/core_emit_mem.c` under a single span range guard (`tlb_groups = 15..84`, `tlb_slow = 0`) with dynamic cold-word capacity checks and inline VFP-flush fallback.
- [x] **A32-R20-P6 (`Wave B3 — Superblock / Fallthrough Chaining & Batching`, `caaee20`, `98f333e`)** — Tightened straight-line fallthrough compilation and batched JIT code-cache flushes across multi-block compilation bursts (`dbt_jit_query.c`, `vm_jit_mem.c`).
- [x] **A32-R20-P7 (`Wave C1 — A32/T32 Immediate Folding`, `b670cd7`, `0d8f290`, `100af25`)** — Folded A32/T32 `ADD`/`SUB`/`CMP`/`CMN` immediates, `RSB #0`, `MOVZ`/`MOVN`, `MOVW`+`MOVT` pairs, and 32-bit bitmask logical immediates (`AND`/`ORR`/`EOR`, with exact `S=1` carry/overflow preservation) directly into A64 ALU instructions (`src/interp/interp_pred_imm.c`, `interp_a32_dp.c`, `interp_t32_dp.c`).
- [x] **A32-R20-P8 (`Wave C2 — Barrel-Shifter Fusion`, `b670cd7`, `0d8f290`)** — Fused immediate `LSL`/`LSR`/`ASR`/`ROR` operands into A64 shifted-register ALU ops (rejecting unsupported `ROR` on A64 `ADD`/`SUB` and `imm5 == 0` `LSR/ASR #32` / `RRX` special forms, and preserving post-indexed `Rm == Rt` shifts).
- [x] **A32-R20-P9 (`Wave C3 — Branchless CSEL Predication & Skip Merging`, `b670cd7`, `0d8f290`, `8e22e24`)** — Lowered `S=0` predicated A32 (`cond < 14`) and Thumb `IT` ALU instructions (including `MOV`/`MVN` with `Rn == 15`) to branchless `CSEL`/`CSINC`/`CSINV`/`CSNEG` and merged consecutive same-condition non-flag-writing instructions.
- [x] **A32-R20-P10 (`Wave C4 — 4-Bit NZCV Liveness & PC-Relative Constant Folding`, `b670cd7`, `0d8f290`, `100af25`)** — Added 4-bit backward `N|Z|C|V` liveness across A32 and Thumb-2 (`CMP.W`/`CMN.W`/`TST.W`/`TEQ.W` flag-kill tracking with conservative `0xF` preservation across `LDRH`/`LDRD`/mid-block branches) plus A32 `rn == 15` PC-relative constant folding.
- [x] **A32-R20-P11 (`Wave D1 — Direct v8..v29 VFP/NEON Register Cache`, `eddead7`, `8582770`)** — Added the 22-slot (`v8..v29`) VFP/NEON register cache (`src/interp/interp_vfp_cache.c`) resolving cached `S`/`D`/`Q` operands directly in host vector registers without scratch `FMOV`/`ORR` copies, with clean-slot eviction ordering in `a64_vfp_inout_*`.
- [x] **A32-R20-P12 (`Wave D2 — Single-Probe VLDM/VSTM & NEON Load/Store Integration`, `73a0e05`, `eddead7`)** — Collapsed contiguous `VLDM`/`VSTM`/`VPUSH`/`VPOP` transfers and wired `interp_neon_mem.c` directly into the `S`/`D`/`Q` register cache (`vfp_simd` on `armhf`/`thumb`/`armv7` cut **`2.0×–2.56×`** to `527–690 µs`).
- [x] **A32-R20-P13 (`Wave D3 — Alias-Safe VFP Preheader Pinning`, `eddead7`, `8582770`)** — Replaced block-wide `vfp_preheader_ineligible` (`A32-R20-D4`) with exact `S`/`D`/`Q` alias flushing plus `a32_vfp_record_preheader` wiring inside `cache_src`/`cache_dst` so self-loops pin non-aliased `S`/`D`/`Q` registers across back-edges.
- [x] **A32-R20-P14 (`Wave D4 — Range-Guarded Mixed NEON+Scalar Self-Loop FPCR Elision`, `eddead7`, `8582770`)** — Added range-guarded (`[-0.0, +0.0]` and `[2^-61, 2^62)`, checked via `UMAXP` vector reduction) mixed NEON+scalar self-loop `FPCR == 0` elision with `VMSR` invalidation (`a64_vfp_note_vmsr`), cutting `iOS armv7s` `vfp_simd` another **`2.34×–2.40×`** (`10,580 -> 4,516 µs` on Darwin, **`10.94×` cumulative** vs. Round 19 pre-fix `49,413 µs`).
- [x] **A32-R20-P15 (`Wave E1–E3 — macOS Regime A2 AOT, Lazy VM32 Init & Zero-Exit Timer/PID Stubs`, `98f333e`, `026566a`)** — Enabled position-independent Regime A2 `--aot`/`--auto-aot` with inline `.bin` fault sites (`A32-R20-D2`, `blocks_compiled=0`), lazy reservation / zero-RSS `tlb[0]` init + coalesced `mprotect`/APRR (`A32-R20-D3`), and native Thumb-2 leaf stubs for `_getpid`, `_mach_absolute_time`, and `_mach_timebase_info` (`src/ios/ios_stubs_time.c`; `syscalls` traps dropped `20,062 -> 57`, `syscalls` bench **`325 µs -> 39 µs`**, **`8.33×` faster**).

### 4.3 Post-Remediation Head-to-Head Verification (`cc3d3aa` vs. `100af25`, 5-Trial `min_us`)

Measured via interleaved 5-trial A/B rounds on Apple Silicon across both **Darwin DBT** (`_build/a32rt-darwin`, Regime A2 Soft-TLB, matching Section 3's `sum(min_us)` / best single-trial `total_us` methodology) and **Linux DBT** (`_build/a32rt`, Regime A1 direct window):

| Host / Guest Target | `coremark` (`µs`) | `chacha20` (`µs`) | `vfp_simd` (`µs`) | `calls_ibtc` (`µs`) | `syscalls` (`µs`) | `sum(min_us)` / Best Trial (`µs`) | Speedup vs. R19 (`cc3d3aa`) |
|---|---:|---:|---:|---:|---:|---:|---|
| **Native `arm64-apple-darwin` (interleaved ref.)** | `729` | `958` | `380` | `1,782` | `6` | **`3,855` / `3,906`** | `1.00×` host reference (`2,206 µs` peak boost) |
| **Darwin DBT — iOS `armv7` (R19 Cold JIT)** | `3,077` | `3,268` | `1,378` | `5,837` | `325` | **`13,885` / `15,035`** | R19 baseline (`--auto-aot` disabled on A2) |
| **Darwin DBT — iOS `armv7` (R20 Cold JIT)** | `2,516` | `2,790` | **`587`** | `5,642` | **`39`** | **`11,574` / `11,959`** | **`2.35×` `vfp_simd`**, **`8.33×` `syscalls`**, **`1.26×` total** (`6,922 µs` peak boost) |
| **Darwin DBT — iOS `armv7` (R20 Warm AOT)** | `2,490` | `2,829` | **`587`** | `5,116` | **`39`** | **`11,061` / `11,474`** | **`1.31×` total**, **`blocks_compiled=0`**, `icache_flushes=1` |
| **Darwin DBT — iOS `armv7s` (R19 Cold JIT)** | `3,625` | `2,699` | `10,580` | `5,856` | `330` | **`23,090` / `23,411`** | R19 baseline (`49,413 µs` `vfp_simd` pre-R19) |
| **Darwin DBT — iOS `armv7s` (R20 Cold JIT)** | `2,542` | `2,223` | **`4,516`** | `4,757` | **`47`** | **`14,085` / `14,307`** | **`2.34×` `vfp_simd`** (**`10.94×` vs. `8d87576`**), **`7.02×` `syscalls`**, **`1.64×` total** (**`4.29×` vs. `8d87576`**) |
| **Darwin DBT — iOS `armv7s` (R20 Warm AOT)** | `2,558` | `2,187` | **`4,617`** | `5,180` | **`47`** | **`14,589` / `14,679`** | **`1.60×` total**, **`blocks_compiled=0`**, `icache_flushes=1` |
| **Darwin DBT — ELF32 `armhf` (R19 Cold JIT)** | `3,697` | `3,984` | `1,341` | `5,820` | `242` | **`15,084` / `15,811`** | R19 baseline |
| **Darwin DBT — ELF32 `armhf` (R20 Cold JIT)** | `2,514` | `2,812` | **`686`** | `5,371` | `250` | **`11,633` / `11,634`** | **`1.47×` `coremark`**, **`1.95×` `vfp_simd`**, **`1.36×` total** |
| **Darwin DBT — ELF32 `armhf` (R20 Warm AOT)** | `2,176` | `2,819` | **`686`** | `5,147` | `252` | **`11,080` / `11,090`** | **`1.70×` `coremark`**, **`1.43×` total**, **`blocks_compiled=0`** |
| **Darwin DBT — ELF32 `thumb` (R19 Cold JIT)** | `3,150` | `3,321` | `1,349` | `5,463` | `249` | **`13,532` / `13,634`** | R19 baseline |
| **Darwin DBT — ELF32 `thumb` (R20 Cold JIT)** | `2,496` | `2,875` | **`683`** | `4,720` | `255` | **`11,029` / `11,076`** | **`1.26×` `coremark`**, **`1.98×` `vfp_simd`**, **`1.23×` total** |
| **Darwin DBT — ELF32 `thumb` (R20 Warm AOT)** | **`1,858`** | **`2,212`** | **`527`** | **`3,631`** | **`202`** | **`8,430` / `8,430`** | **`1.70×` `coremark`**, **`2.56×` `vfp_simd`**, **`1.62×` total** (`2.16×` vs. native) |
| **Linux DBT — iOS `armv7` (R20 Cold / Warm AOT)** | `2,068` / `1,955` | `1,541` / `1,538` | **`586` / `587`** | `5,364` / `5,283` | **`33` / `35`** | **`9,592` / `9,398`** | **`2.33×` `vfp_simd`**, **`28.8×` `syscalls`**, **`1.26×` total** vs. R19 (`12,111 µs`) |
| **Linux DBT — iOS `armv7s` (R20 Cold / Warm AOT)** | `1,931` / `1,961` | `1,431` / `1,424` | **`4,574` / `4,588`** | `5,413` / `5,475` | **`34` / `35`** | **`13,383` / `13,483`** | **`2.40×` `vfp_simd`** (**`8.05×` vs. `8d87576`**), **`27.6×` `syscalls`**, **`1.63×` total** |
| **Linux DBT — ELF32 `armhf` (R20 Cold / Warm AOT)** | **`1,629` / `1,655`** | `1,538` / `1,491` | **`689` / `689`** | `5,291` / `5,429` | `869` / `881` | **`10,016` / `10,145`** | **`1.42×` `coremark`**, **`2.02×` `vfp_simd`**, **`1.20×` total** vs. R19 (`12,045 µs`) |
| **Linux DBT — ELF32 `thumb` (R20 Cold / Warm AOT)** | `1,904` / `1,905` | `1,532` / `1,531` | **`690` / `690`** | `5,386` / `5,354` | `895` / `894` | **`10,407` / `10,374`** | **`2.06×` `vfp_simd`**, **`1.14×` total** vs. R19 (`11,829 µs`) |

### 4.4 Key `--stats` / `[a32rt stats-ext]` Telemetry Shifts (`cc3d3aa -> 100af25`)

| Metric (`bench_suite.c --full` on Darwin Regime A2) | R19 (`cc3d3aa`) | R20 Cold JIT (`100af25`) | R20 Warm `--auto-aot` (`100af25`) | Impact |
|---|---:|---:|---:|---|
| **`ras_hits` (inline 64-entry RAS pop)** | `0` | **`3,571,175`** (`armv7`/`armhf`/`thumb`) | Folded into `chain_hits` (`14.57M`) | `100%` of recursive & leaf returns stay in `.Lrun_loop` |
| **`ibtc_hits` (inline 1,024-entry IBTC)** | `20,080` | **`1,020,107`** | Folded / `76`–`20,047` | Jump tables & vtable calls bypass `trampoline.S` |
| **`syscalls` (`iOS armv7` / `armv7s`)** | `20,062` | **`57`** | **`57`** | **`351×` trap reduction** (`_getpid` + Mach timer leaf stubs) |
| **`ns_vm_init` (startup VM32 & Soft-TLB)** | `~6,230,000 ns` | **`94,000–199,000 ns`** (`19,125 ns` Linux) | **`117,000–171,000 ns`** | **`31×–66×` faster** on Darwin (`>300×` on Linux) |
| **`ns_wx_flush` (`W^X` / APRR + I-cache)** | `~126,000 ns` | **`9,000–18,000 ns`** | **`4,000–10,000 ns`** (`wx_transitions=1`) | **`7×–14×` faster** JIT flush; single transition on AOT load |
| **`blocks_compiled` / `ns_translate`** | `136–187` / `~0.6 ms` | `136–186` / `497–626 µs` | **`0` / `0 ns`** (`aot_abs64=0`) | Zero runtime compilation on Regime A1 & Regime A2 warm start |
| **`tlb_groups` / `tlb_slow`** | `0` / n/a | **`15–84` / `0`** | `0` (pre-emitted) / **`0`** | Grouped Soft-TLB span checks with zero slow-path faults |
