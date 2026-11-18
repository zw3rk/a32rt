# `a32rt` Defect Ledger (`DEFECTS.md`)

## Round 22 — Regime A2 Grouped Soft-TLB Cold-Path & Pre-Indexed Writeback Fixes (`master` @ `a53ee27` -> Remediated in `5959599`, `3 / 3` Resolved, 2026-11-19)

Identified in Regime A2 `grp_emit_grouped` word load/store grouping (`test-m8-glibc-armhf` `ld-linux-armhf.so.3` and `test-m7-armhf --dbt` `ld-musl-armhf.so.1` failures), remediated in `5959599`, and re-verified with `45 / 45` parity restored.

- [x] **A32-R22-D1 (`DBT/Soft-TLB`, `src/core/core_emit_mem.c:467-493`)** — `grp_emit_grouped` Regime A2 cross-4KiB-page cold slow path omitted an unconditional `B next_op` jump after `per_op_mem` (`LDR`/`STR`), falling straight through into `cold_emit_fault` whenever a grouped word load/store crossed a 4 KiB page boundary (causing `ld-linux-armhf.so.3` in `test-m8-glibc-armhf` and `ld-musl-armhf.so.1` in `test-m7-armhf` to abort at `0xdfff...` with `rc=139`). Fixed by emitting `b_next` (`B next_op`) and bumping `grp_try_word` `cold_reserve` budget (`42U -> 43U`).
- [x] **A32-R22-D2 (`DBT/Addr`, `src/core/core_emit_mem.c:429`)** — `grp_emit_grouped` hot path left `w22 = rn + min_off` instead of restoring `w22 = rn + offs[n - 1U]` before `hot_join`, causing pre-indexed writeback instructions (`ldr r1, [r4, #4]!` -> `mov w4, w22`) merged at the end of a group to write `rn + min_off` back to `rn` and spin forever in `ld-musl` (`test-m7-armhf --dbt`). Fixed by adding `(offs[n - 1U] - min_off)` to `w22` when `offs[n - 1U] != min_off`.
- [x] **A32-R22-D3 (`BUILD`, `Makefile`)** — Added `-MMD -MP` auto-generated `.d` header dependency tracking across `OBJ_DIR` (`obj`, `darwin-obj`, `obj-slim`, `darwin-slim-obj`) and `g02-obj` so `src/*.h` struct layout changes always recompile affected objects.

---

## Round 21 — A32H_ABI 1.2 `G02` External Darwin Bridge & `a32rt-slim` Audit (`master` @ `6bf6d76` -> Remediated in `a53ee27`, `5 / 5` Resolved, 2026-11-17)

Introduced the A32H_ABI 1.2 `G02` external Darwin host bridge (`71a1cd7`, plus `369bcf6` frozen `a32host.h` sync) with `a32rt-slim` build decoupling, audited across RC4 (`a001b01`, `0387553`) plus store-classifier follow-ups (`2e7699e`, `7acddf0`, `5013c52`, `9f17ff5`), and merged in `a53ee27`.

- [x] **A32-R21-D1 (`G02/CPU`, `src/os/darwin_host_cpu.c`)** — Thumb-2/ARM store-fault opcode classifier missed `PUSH {.., lr}` (`0xB5xx`), `VSTn`/`VLDn` (L at bit 21, not 20), `STREXH`/`STREXD` and SIMD store fault masks, plus conditional ARM VFP stores and media ops. Tightened across `2e7699e`, `7acddf0`, `5013c52`, and `0387553`.
- [x] **A32-R21-D2 (`G02/VM`, `src/os/darwin_host_bridge.c`, `src/vm/vm_window.c`)** — Soft-TLB region sync left provider-populated pages clobbered or mis-protected on registration, and unmap-vs-`mprotect` replies were misclassified. Fixed via stage-then-`register_range` mapping and `is_prot` classification (`a001b01`).
- [x] **A32-R21-D3 (`G02/Threads`, `src/os/darwin_host_bridge.c`)** — Spawned worker threads inherited the spawner's `host_tid`, and queued async signals re-polled every block boundary instead of being consumed atomically. Fixed by populating `host_tid` on the worker pthread (`child->host_tid = 0` at spawn) and `fetch_and`-consuming unmasked `pending_signals` before the `A32H_DARWIN_TRAP` poll (`0387553`).
- [x] **A32-R21-D4 (`G02/AOT+CLI`, `src/dbt/dbt_aot_cache.c`, `src/os/darwin_host_bridge.c`, `src/main.c`)** — Host-loaded (`A32H_DARWIN_BOOT`) AOT cache key hashed only the personality tag plus the first 4 KiB at `entry_pc`, so distinct Mach-O binaries collided; `--personality` accepted unknown values and `--engine`/`--cache` option arguments confused bridge detection. Fixed with region-spanning executable hash (excluding `darwin-commpage`), `personality_known` CLI validation (`darwin-host|hl-darwin|linux|ios`), and option-argument skipping (`a001b01`, `0387553`).
- [x] **A32-R21-D5 (`G02/Tests`, `tests/test_g02_darwin_bridge.c`)** — Thumb test-vector length was hardcoded instead of derived from the first halfword, miscounting 32-bit Thumb-2 instructions. Fixed in `9f17ff5`.

---

## Round 20 — Backend Dispatch, Soft-TLB, VFP/NEON & AOT Audit (`master` @ `cc3d3aa` -> Remediated in `100af25`, `6 / 6` Resolved + Pre-Merge RC Hardening, 2026-11-15)

Identified during the 6-subsystem backend performance & precision deep-dive (`A32-R20-D1`..`A32-R20-D4`) plus lock-step `--difftest` verification (`A32-R20-D5`..`A32-R20-D6`), remediated across isolated worker waves (`b4cee33..100af25`), and re-verified across 6 post-wave code-review slices (`RC-1`..`RC-4`) with `make check check-headers build-darwin build`, `./tools/run_ios_suite.sh` (`61 / 61` PASS), Regime A1 & Regime A2 `--auto-aot` warm reload (`blocks_compiled=0`), `test-m4` DBT PASS, and lock-step `--difftest` (`0` divergences). Paired with [`PERF.md`](PERF.md) (`A32-R20-P0`..`A32-R20-P15`, `16 / 16` resolved).

- [x] **A32-R20-D1 (`DBT/RAS`, `src/dbt/dbt_jit_run.c`, `src/core/core_emit_exit.c`, `src/core/core_emit_shared.c`)** — `cur_entry->ret_entry` was never wired on the common path (`ras_hits = 0` permanently): on a first `BL callee`, the return block (`LR`) was not yet compiled (`found_e == NULL`), so `set_ht_ret_slot_locked` was skipped while `try_chain_entries` chained the caller directly to `callee`; `POP {.., pc}` / `LDR pc, [sp], #4` omitted return hints, and inline RAS/IBTC hits only bumped `chain_hits`. Fixed via unresolved-caller RAS back-patching on first return, `b->is_return` gating across `BX LR` / `MOV PC, LR` / `POP {.., pc}` / `LDR PC, [SP], #4`, and dedicated `ras_hits`/`ibtc_hits` telemetry under `--stats` (`ras_hits = 3,571,175` on `bench_suite.c --full`).
- [x] **A32-R20-D2 (`AOT`, `src/dbt/dbt_aot_cache.c`, `src/dbt/aot/aot_sites.c`)** — `write_block_cb` silently saved blocks without fault-recovery site records when `a32_aot_site_fetch` returned `-EAGAIN` on `trylock` contention; `compute_elf_hash` folded `ctx->guest_base` into the cache key even though emitted A64 code is parametric in `x20`; stale pre-`A32-IOS-P4` Regime A2 guards disabled `--aot` / `--auto-aot` on macOS despite position-independent Soft-TLB addressing. Fixed via bounded spin-acquire in `site_lock_acquire`, `guest_base`-independent cache hashing with PIE `load_bias` site rebasing, fail-closed `MOVZ/MOVK hw>=2` verification, and inline `.bin` fault-site serialization (`blocks_compiled=0` on Regime A2 warm reload).
- [x] **A32-R20-D3 (`VM32/SMC`, `src/vm/vm32.c`, `src/vm/vm32.h`, `src/vm/vm_jit_mem.c`)** — `a32_vm32_tlb_fill` set `write_tag` solely from mapping `writable` without clearing it on `A32_PROT_WATCH` translated code pages; `a32_vm32_tlb_invalidate_all()` unconditionally dirtied all 32 MiB of Soft-TLB BSS at startup (including on Regime A1); `a32_vm32_tlb_fill` ran `vm32_bump_spanned` (64 atomic RMWs) per page inside `sync_region` loops and `vm32_sync_host_pages_a2` issued uncoalesced per-page `mprotect` calls. Fixed via 16-byte `ntag = ~page_tag` Soft-TLB entries honoring `A32_PROT_WATCH`, lazy reservation / zero-RSS `tlb[0]` init (`ns_vm_init`: `6.23 ms -> 94–199 µs`), range-level `vm32_bump_spanned`, and coalesced `mprotect` + APRR flushes (`ns_wx_flush`: `126 µs -> 9–18 µs`).
- [x] **A32-R20-D4 (`VFP/DBT`, `src/interp/interp_vfp_cache.c`, `src/core/core_emit_alu.c`)** — Any `S_k` / `D_{k>>1}` / `Q_{k>>2}` register overlap set `vfp_preheader_ineligible = true` for the entire block, disabling loop-preheader VFP register promotion for all remaining non-overlapping `D8..D31` registers in mixed-width loops (`armv7s` `vfp_simd`). Replaced with the 22-slot (`v8..v29`) VFP register cache with exact `S`/`D`/`Q` alias invalidation and `a32_vfp_record_preheader` loop-pinning.
- [x] **A32-R20-D5 (`INTERP/NEON`, `src/interp/interp_vfp.c:340, 388`, fixed in `348b299`)** — `vfp_mcr_mrc` checked `((opc1 & 2U) == 0U)` instead of `(opc1 == 0U || opc1 == 1U)`, hijacking `VDUP.32 Dd/Qd, Rt` (`opc1 == 4, 5`) as single-lane `VMOV` before `i_vfp_neon_int` could execute, causing `--difftest` divergence against the DBT.
- [x] **A32-R20-D6 (`INTERP/VFP`, `src/interp/interp_vfp.c:789, 820`, fixed in `9274aff`)** — Host Clang `-O2` algebraically folded `(-d + p)` into `(p - d)` (`FSUB`) in scalar `VNMLS.F32`/`.F64`, failing to negate the sign bit of a quiet NaN accumulator `d` (`0x7FC01234 -> 0xFFC01234` per ARMv7-A `FPAdd(FPNeg(Vd), prod)`). Fixed by materializing `volatile` `-d` prior to addition so `--difftest` matches hardware/DBT `VNMLS` QNaN propagation bit-for-bit.
- **Pre-merge review hardening (`RC-1`..`RC-4`, remediated in `026566a..100af25`)**: Fixed cross-page Soft-TLB `queue_cold_spanN` page-2 branch target (`f2 = addr_w + span - 1` instead of `f1`), dynamic grouped `cold_words` reservation + inline VFP flush fallback (`core_emit_mem.c`), `_Static_assert` struct-offset guards (`core_emit_exit.c`), `trampoline.S` `ldar w21, [x28]` acquire ordering, `PROT_READ|PROT_WRITE` validation on iOS timer data pointers (`ios_stubs_time.c`), A64 `ADD`/`SUB` `ROR` & `imm5 == 0` predication guards + `MULS`/`MLAS` flag-write exclusion + logical-immediate `S=1` fallback + post-indexed `Rm == Rt` shift preservation + 4-bit NZCV liveness across `CMP.W`/`TST.W` vs `LDRH`/`LDRD`/branches (`interp_pred_imm.c`, `interp_a32_dp.c`, `interp_a32_ctl.c`, `interp_t16.c`, `interp_t32_dp.c`), and `a64_vfp_inout_*` clean-slot eviction ordering (`interp_neon_alu.c`).

---

## Round 19 — 32-Bit iOS (`armv7`/`armv7s`) & Thumb-2/DBT Audit (`master` @ `8d87576` -> Remediated in `f5e0d35`, `19 / 19` Resolved, 2026-11-14)

Audited across 6 parallel subsystem deep-dives (`../rt-series/lode/reviews/convergence/2026-11-14-a32rt-ios-lane{1..6}-fable.md`), remediated across isolated worker lanes (`8d87576..f5e0d35`), and independently re-verified across 8 post-remediation tracks (`../rt-series/lode/reviews/convergence/2026-11-14-a32rt-ios-verify-{a1,a2a1,a2a2,a2b,b,c1,c2a,c2b}-fable.md`) with `make check check-headers test-ios` (`61 / 61` PASS), ELF32 `M1..M7` regressions PASS, and lock-step `--difftest` (`0` divergences). See [`PERF.md`](PERF.md) for the companion performance optimization ledger (`A32-IOS-P1`..`A32-IOS-P9`, `9 / 9` resolved).

### 1. Security, Memory-Safety & Concurrency (`P1`)

- [x] **A32-IOS-D1 (`SECURITY`, `src/ios/ios_stubs.c:363-385, 645-694`)** — Host stack out-of-bounds read in synthetic `printf` (`pfmt1` bounds copy by untruncated `snprintf` return `w` instead of `sizeof(tmp) - 1`) and `snprintf`/`vsnprintf` (`cp` not clamped to `sizeof(buf) - 1`).
- [x] **A32-IOS-D2 (`SECURITY`, `src/ios/ios_svc_bsd.c:635-647, 685-694`)** — Unchecked guest VA range in `madvise` (75) and `msync` (65) + Darwin `MADV_FREE_REUSE (8)` / `MADV_CAN_REUSE (9)` passed directly to Linux host `MADV_FREE (8)` / `MADV_REMOVE (9)`.
- [x] **A32-IOS-D3 (`SECURITY`, `src/dbt/dbt_aot_cache.c:151, 520-532`)** — Missing `st.st_uid == geteuid()` ownership check in `load_cache_from_path` when loading `/tmp/a32rt-cache-*.bin` AOT caches.
- [x] **A32-IOS-D4 (`CONCURRENCY`, `src/core/core_emit_mem.c:27-74`, `src/dbt/dbt_jit_query.c:131, 183-194`)** — Data race on non-TLS `g_site_stage[]` during unlocked multi-threaded JIT block translation + stale pre-SMC block installation when `cur_epoch != use_epoch`.

### 2. XNU 32-Bit iOS Functional & ABI Bugs (`P1`)

- [x] **A32-IOS-D5 (`LOADER`, `src/ios/ios_bind.c:45-46, 333-339, 384-405`)** — Weak imports (`BIND_SYMBOL_FLAGS_WEAK_IMPORT` / `N_WEAK_REF`) abort Mach-O load instead of binding `0`; `INDIRECT_SYMBOL_LOCAL`/`ABS` masks set to `0xFFFFFFFF`/`0xFFFFFFFE` instead of `0x80000000`/`0x40000000`.
- [x] **A32-IOS-D6 (`THREADS`, `src/ios/ios_threads.c:76, 438-518, 612, 648-700`)** — Static `PTHREAD_MUTEX_INITIALIZER` (`0x4D555458`) / `PTHREAD_COND_INITIALIZER` (`0x434F4E44`) fail with `EINVAL`; `psynch`/`ulock` 64-bucket condvars lose wakeups on collisions; `pthread_join` lacks self/main guards; TSD destructors skipped on thread exit.
- [x] **A32-IOS-D7 (`SIGNALS`, `src/ios/ios_signals.c:146, 641-696`, `src/ios/ios_svc_bsd.c:853-855`)** — Self-delivered signals (`sigsuspend`, `kill(self)`, `pthread_kill(self)`) clobber `_sigtramp` `r0` handler pointer or save pre-syscall `r0` in `mcontext`; `g_ios_pending_info[32]` is non-TLS.
- [x] **A32-IOS-D8 (`STUBS`, `src/ios/ios_stubs.c:617-625`)** — Synthetic `fprintf` (`id 49`) shifts `r1 = r2; r2 = r3;` and passes `start = 1U`, skipping the first format argument (`r2`).
- [x] **A32-IOS-D9 (`MACH`, `src/ios/ios_svc_mach.c:440, 722`)** — 8th Mach trap argument (`cur_protection` in `vm_map` trap 15, `rcv_msg` in `mach_msg_overwrite_trap` trap 32) read from `[sp+16]` instead of `r8` (`osfmk/mach/arm/syscall_sw.h` `_kernel_trap_6to9`).
- [x] **A32-IOS-D10 (`BSD-ABI`, `src/ios/ios_svc_bsd*.c`)** — `getdirentries64` `d_name` offset (`24` vs `21`) & `basep` seek; `statfs64` `f_bsize` layout & `AT_SYMLINK_FOLLOW` (`0x20` vs `0x40`); `openat` ignoring `dirfd` & `F_GETPATH` requiring 4096B instead of `1024B`; `rl_host` permuting `RLIMIT_*` on macOS & `sysctl` truncation errno; macOS 12-byte `cmsghdr` (`SCM_RIGHTS`) & `MSG_DONTWAIT`; `kevent` `POLLIN|POLLOUT` spin, timer deadlines & `kevent_qos` (`374`).
- [x] **A32-IOS-D11 (`OS`, `src/os/os_sys_thread_sig.c:233-274`, `src/os/backend_darwin.c`)** — Darwin-host Linux `futex` (`FUTEX_WAIT`) lacks `a32_guest_range_ok` and ignores `FUTEX_WAKE`.

### 3. Loader, Mach, Stubs, Interpreter & DBT Completeness (`P2`)

- [x] **A32-IOS-D12 (`LOADER`, `src/ios/ios_dylib.c:116-120, 271-276, 369-370`, `src/ios/ios_macho.c:617`)** — `__mod_init_func` DFS post-order, `ios_apply_prot` unaligned page rounding (`start + vmsize`), `(st_dev, st_ino)`/`install_name` dylib dedup, and fixed-segment overlap check.
- [x] **A32-IOS-D13 (`EXPORT`, `src/ios/ios_export.c:125-130, 218-222, 343`)** — Export-trie >255B edge truncation and non-`libSystem` two-level ordinal fallback.
- [x] **A32-IOS-D14 (`CRT0/AOT`, `src/ios/ios_commpage_stack.c:174-177`, `src/ios/ios_stubs.c:859`, `src/ios/ios_svc_bsd_proc.c:130`)** — CRT0 return via `exit()` running LIFO `__cxa_atexit`/`atexit` callbacks, plus `--stats` and `--auto-aot` persistence on iOS process exit.
- [x] **A32-IOS-D15 (`MACH`, `src/ios/ios_svc_mach.c:250-256, 692-706, 821`, `src/ios/ios_stubs.c:834`)** — Complex 40B MIG port descriptor reply in `task_get_special_port`, `mach_port_destruct` cleanup, `MACH_SEND_NO_BUFFER` on full queue, and real Mach port from `_mach_thread_self`.
- [x] **A32-IOS-D16 (`STUBS`, `src/ios/ios_stubs.c:297, 418, 788, 806, 857`, `tests/ios/libSystem.tbd`)** — `strtol`/`strtoul` `endptr`, `sysctl` stub errno sign, `nanosleep` `EINTR`, `abort` `SIGABRT`, synthetic `FILE*` zero/flags init, and `libcompiler_rt` `___divdi3`/`___udivdi3`/`___moddi3`/`___umoddi3` exports.
- [x] **A32-IOS-D17 (`INTERP`, `src/interp/interp_vfp_neon*.c`)** — Full ARMv7/VFPv4 NEON reference interpreter coverage (`VMUL.I*/P8`, `VMLA/VMLS`, long/wide/shifts, `.F32`/`.F16`/fixed-point ops with cumulative `FPSCR` flags under Standard FPSCR, `VLD2..4`/`VST2..4`, `VREV16/32`, `VDUP`/`VEXT`/`VTBL`/`VTRN`/`VUZP`/`VZIP`) + pre-flight memory range checks.
- [x] **A32-IOS-D18 (`INTERP/DBT`, `src/interp/interp_thumb.c:82`, `interp_a32.c`, `interp_t16.c`, `interp_a32_ctl.c`)** — Darwin `__builtin_trap()` (`0xDEFE` Thumb / `0xE7FFDEFE` ARM) raises `SIGTRAP` (`A32_EXIT_BKPT`) when `ctx->is_ios`.
- [x] **A32-IOS-D19 (`DBT`, `src/dbt/dbt_jit_run.c:147, 502-532, 592`)** — iOS protection-fault `SIGBUS` vs `SIGSEGV`, UNDEF-fallback `A32_EXIT_BKPT` `SIGTRAP`, host `fpcr` reset on guest fault recovery, and `pthread_jit_write_protect_np(1)` in `a32_dbt_single_step`.

---

## Historical Archive — Pre-Round-13 Audit Notes (`d84feb31`, Resolved in `25ef1aa`)

Reviewed `master` `d84feb311a0a4676d56e81590b7bd442f57f4a17`.
The only local branch is `master`, and it is the only worktree (`/Users/angerman/Projects/zw3rk/a32rt`).
Each entry was checked by a second read that was given the location and a question, not the first conclusion. Second-read notes are under the review scratch `blind/` directory.
In-source comment edits are in the working tree and are not committed. Project documents are recorded and left unchanged.

## D1. comment

- Repo: `a32rt`
- Ref: `master`
- Commit: `d84feb311a0a4676d56e81590b7bd442f57f4a17`
- Path: `tests/test_m5_ltp_gdb.c`
- Lines: 2193 and 2292 (before the comment edit)
- Symbol: `test23_r10_efault_high_unmapped_and_setresuid_rt34`

Both section comments said `setresuid32 (164)` and `setresgid32 (170)`.
ARM EABI `__NR_setresuid` is 164 and `__NR_setresgid` is 170. `__NR_setresuid32` is 208 and `__NR_setresgid32` is 210.
The same function's calls already use those numbers: `sys3(164U /* setresuid */)`, `sys3(170U /* setresgid */)`, `sys3(208U /* setresuid32 */)`, `sys3(210U /* setresgid32 */)`.
`src/syscalls.c` defines `ARM_NR_setresuid` 164, `ARM_NR_setresgid` 170, `ARM_NR_setresuid32` 208, and `ARM_NR_setresgid32` 210.
The comments named the 32-bit syscalls with the 16-bit numbers.
The comments now name all four calls with those numbers. The calls were not changed.
Second read: `blind/blind-read-setresuid-comments.md`.

## D2. other

- Repo: `a32rt`
- Ref: `master`
- Commit: `d84feb311a0a4676d56e81590b7bd442f57f4a17`
- Path: `AGENTS.md`
- Line: 17
- Symbol: toolchain sentence

`AGENTS.md` says the assembly sources are `trampoline.S` and `start.S`.
`git ls-files '*.S'` at this commit lists only `src/trampoline.S`. There is no `start.S`.
This is a project document, not an in-source comment, so the file was not edited.
Second read: `blind/blind-read-a32rt-docs-facts.md`.

## D3. other

- Repo: `a32rt`
- Ref: `master`
- Commit: `d84feb311a0a4676d56e81590b7bd442f57f4a17`
- Path: `README.md` line 24, `A32RT.md` lines 204 and 308
- Symbol: `A32_AOT_VERSION`

`src/aot.c:23` is `#define A32_AOT_VERSION 8U`.
`README.md:24` says `A32_AOT_VERSION 5`. `A32RT.md:204` and `A32RT.md:308` say `A32_AOT_VERSION 6`.
`README.md:135`, `README.md:204`, and `A32RT.md:418` already say 8.
A reader of the version-5 or version-6 lines will not accept the header the translator writes.
Documents were not edited.
Second read: `blind/blind-read-a32rt-docs-facts.md`.

## D4. other

- Repo: `a32rt`
- Ref: `master`
- Commit: `d84feb311a0a4676d56e81590b7bd442f57f4a17`
- Path: `README.md` lines 137 and 232, `A32RT.md` lines 311 and 429
- Symbol: `AT_PLATFORM`

`src/stack32.c:129` pushes `"v8l"` as `AT_PLATFORM`.
Those four lines say `AT_PLATFORM` is `"v7l"`. `A32RT.md:150` and `A32RT.md:346` already say `"v8l"`.
`armv7l` in the triple and `uname` text is a different string and is not this defect.
Documents were not edited.
Second read: `blind/blind-read-a32rt-docs-facts.md`.

## D5. other

- Repo: `a32rt`
- Ref: `master`
- Commit: `d84feb311a0a4676d56e81590b7bd442f57f4a17`
- Path: `A32RT.md` line 167, `AGENTS.md` line 56, `README.md` line 238
- Symbol: `compute_elf_hash`

The documents say the AOT cache is keyed by SHA-256 of the ELF, and `A32RT.md` also says a build-id and a CPU feature mask.
`src/aot.c` `compute_elf_hash` steps `fnv1a_64_step` over the file size, mtime, bytes, `elf_base`, `interp_base`, `elf_entry`, sysroot, and path. There is no SHA-256, build-id, or CPU feature mask in that function.
Documents were not edited.
Second read: `blind/blind-read-a32rt-docs-facts.md`.

## D6. other

- Repo: `a32rt`
- Ref: `master`
- Commit: `d84feb311a0a4676d56e81590b7bd442f57f4a17`
- Path: `README.md` line 24, `A32RT.md` line 206
- Symbol: `a32_aot_header_t`

Both lines say relocation uses `delta = load_bias - hdr.reserved`.
`a32_aot_header_t` in `src/aot.c` is `magic`, `version`, `num_blocks`, and `load_bias`. There is no `reserved` field.
`src/aot.c:378` computes `delta` as `load_bias - hdr.load_bias`.
Documents were not edited.
Second read: `blind/blind-read-a32rt-docs-facts.md`.

## D7. other

- Repo: `a32rt`
- Ref: `master`
- Commit: `d84feb311a0a4676d56e81590b7bd442f57f4a17`
- Path: `README.md` line 97, `A32RT.md` lines 232 and 303
- Symbol: `format_cache_path`

`README.md:97` says the default cache is the directory `/tmp/a32rt-cache`.
`A32RT.md` says the cache lives at `$XDG_CACHE_HOME/a32rt`.
`format_cache_path` writes `<dir>/a32rt-cache-<16 hex>.bin`, or `/tmp/a32rt-cache-<16 hex>.bin` when `A32RT_CACHE` and `XDG_CACHE_HOME` are unset. The directory is `XDG_CACHE_HOME` itself, not a subdirectory named `a32rt`.
Documents were not edited.
Second read: `blind/blind-read-a32rt-docs-facts.md`.

## D8. other

- Repo: `a32rt`
- Ref: `master`
- Commit: `d84feb311a0a4676d56e81590b7bd442f57f4a17`
- Path: `A32RT.md` lines 198 and 199
- Symbol: `emit_ras_push_inline`, `a64_emit_exit_branch_reg`

Line 198 says a direct call pushes `cur_entry->ret_key`, `ret_code`, and `ret_entry` (`[x27, #80..#103]`) onto the RAS.
`src/a64_emit.c:1124` loads only `[x27, #96]` (`ret_entry` into `x22`) and `src/a64_emit.c:1138` stores that pointer at `ras[top].entry`.
Line 199 says the inline RAS pop jumps with `br x17`.
`src/a64_emit.c:1299` emits `br x22` (`0xD61F02C0`).
`A32RT.md` was not edited.
Second read: `blind/blind-read-a32rt-ras-emit.md`.

## D9. comment

- Repo: `a32rt`
- Ref: `master`
- Commit: `d84feb311a0a4676d56e81590b7bd442f57f4a17`
- Path: `tests/test_m3_threads_signals.c`
- Lines: 1225 and 1293
- Symbol: `thumb_it_udf_helper`

The helper assembles `.short 0xbf01`. Thumb IT `0xbf01` is `itttt eq`: condition EQ and mask `0001`, so the next four Thumb instructions are in the block (`moveq r0, #10`, `udf #0`, `moveq r0, #20`, `addeq r0, #5`). `ittt eq` is mask `0010` (`0xbf02`) and covers only three following instructions, which would leave `addeq` outside the block.
The comments said `ittt eq` while still counting `addeq` as inside the block. They now say `itttt eq`. The encoding byte was not changed.
Second read: `blind/blind-read-test-m3-comments.md`.

## D10. comment

- Repo: `a32rt`
- Ref: `master`
- Commit: `d84feb311a0a4676d56e81590b7bd442f57f4a17`
- Path: `tests/test_m3_threads_signals.c`
- Lines: 2940 and 2941
- Symbol: `test23_m6_adversarial_regressions`

The summary named `VDUP.32 D0, D16[0]` as `0xf3b40c20` and `VDUP.32 Q0` as `0xf3f41c00`.
The instructions that follow are `.inst 0xf3b80c00` and `.inst 0xf3f41c40`, and the comments on those lines call them an invalid VDUP scalar with imm4=8 and an invalid VDUP.32 Q with an odd Qd.
The summary now cites those two words. The `.inst` lines were not changed.
Second read: `blind/blind-read-test-m3-comments.md`.

## D11. comment

- Repo: `a32rt`
- Ref: `master`
- Commit: `d84feb311a0a4676d56e81590b7bd442f57f4a17`
- Path: `tests/test_m3_threads_signals.c`
- Lines: 3026 and 3027
- Symbol: `test23_m6_adversarial_regressions`

The block comment said `code[7]` is `mov r1, #77` (`0xe3a0104d`) and `code[8]` is an `orr` that builds that mov.
The stores that run are `code[7] = 0xE59F1018` (`ldr r1, [pc, #24]`, loading the literal `mov r0, #77` at `code[15]`) and `code[8] = 0xE58E1000` (`str r1, [lr]`).
The comment now names those two instructions. The stores were not changed.
Second read: `blind/blind-read-test-m3-comments.md`.

## D12. comment

- Repo: `a32rt`
- Ref: `master`
- Commit: `d84feb311a0a4676d56e81590b7bd442f57f4a17`
- Path: `tests/test_m4_vfp_neon.c`
- Line: 430
- Symbol: odd single-precision VMOV block

The comment said the block tests high D register `d17` (`M=1`).
The asm is `vmov s1, s2, r0, r1` and back, and the clobber list is `d0`, `d1`, and `d16`. It does not read or write `d17`.
The comment now names `s1, s2` only. The asm was not changed.
Second read: `blind/blind-read-vfp-and-l1c-comments.md`.

## D13. comment

- Repo: `a32rt`
- Ref: `master`
- Commit: `d84feb311a0a4676d56e81590b7bd442f57f4a17`
- Path: `tests/test_m4_vfp_neon.c`
- Lines: 1583 and 1726
- Symbol: section 28b by-scalar VMLS

Both comments named `VMLS.F32 q0, q1, d2[0]`.
The instruction in that block is `vmls.f32 q0, q2, d2[0]`.
The comments now say `q2`. The instruction was not changed.
Second read: `blind/blind-read-vfp-and-l1c-comments.md`.

## D14. comment

- Repo: `a32rt`
- Ref: `master`
- Commit: `d84feb311a0a4676d56e81590b7bd442f57f4a17`
- Path: `tests/test_m4_vfp_neon.c`
- Lines: 1393–1394
- Symbol: section 26b VMUL.P8

The comment said 26b covers 64-bit and 128-bit `VMUL.P8` plus widening `VMULL.P8`.
The asm is `vmul.p8 d2, d0, d1` and `vmull.p8 q2, d0, d1`. There is no Q-form `vmul.p8`.
The comment now says the 64-bit D form plus `VMULL.P8`. The asm was not changed.
Second read: `blind/blind-read-vfp-and-l1c-comments.md`.

## D15. comment

- Repo: `a32rt`
- Ref: `master`
- Commit: `d84feb311a0a4676d56e81590b7bd442f57f4a17`
- Path: `src/decode_a32.c`
- Line: 283
- Symbol: conditional MOV CSEL fast path

The comment said this path is conditional `MOV Rd, Rm` with `S=0`.
The test is `(insn & 0x0FE00FF0U) == 0x01A00000U`. Bit 20 is clear in that mask, so `S=1` matches too, and the arm returns after `a64_emit_csel_w`.
`a64_emit_csel_w` emits one CSEL (`0x1A800000`) and does not write NZCV.
The comment now says bit 20 is not tested. The mask was not changed. See D16.
Second read: `blind/blind-read-a32-decode-exits.md`.

## D16. correctness

- Repo: `a32rt`
- Ref: `master`
- Commit: `d84feb311a0a4676d56e81590b7bd442f57f4a17`
- Path: `src/decode_a32.c`
- Lines: 284–289
- Symbol: conditional MOV CSEL fast path

`MOVS Rd, Rm` (unshifted, `Rd<15`, `Rm<15`, conditional) takes the CSEL return and does not update N or Z.
A later flag consumer still sees the NZCV from before that `MOVS`.
The mask was not changed.
Second read: `blind/blind-read-a32-decode-exits.md`, plus `a64_emit_csel_w` at `src/a64_emit.c:357`.

## D17. correctness

- Repo: `a32rt`
- Ref: `master`
- Commit: `d84feb311a0a4676d56e81590b7bd442f57f4a17`
- Path: `src/decode_a32.c`
- Line: 1014
- Symbol: A32 data-processing write to PC

`a64_emit_exit_branch_reg(b, A64_TMP0, false)` runs when a handled data-processing op writes `Rd==15` and the opcode is not a logical compare (`opc < 8 || opc >= 0xC`).
`false` selects `A32_EXIT_BRANCH`, whose comment is "Non-interworking branch; preserve CPSR.T" (`src/a32rt.h`).
`A32RT.md` says bit 0 selects Thumb on A32 ALU writes to `PC` in ARMv7-A. An odd target therefore stays in ARM.
The call was not changed. T32 ALUWritePC is a different path.
Second read: `blind/blind-read-a32-decode-exits.md`. `A32RT.md` line 62 was re-read with that note.

## D18. correctness

- Repo: `a32rt`
- Ref: `master`
- Commit: `d84feb311a0a4676d56e81590b7bd442f57f4a17`
- Path: `src/main.c`
- Lines: 205–211
- Symbol: guest argv after `--`

The usage line documents `a32rt ... -- <PATH> <ARGV0>`.
`explicit_sep` selects `guest_argv = &argv[i + 1]` only when `exec_fd < 0`.
`exec_fd` is set from `AT_EXECFD` when that aux value is in `int` range.
When both `--` and `AT_EXECFD` are present, `guest_argv` starts at `PATH` and the documented `ARGV0` is `argv[1]` of the guest.
The condition was not changed.
Second read: `blind/blind-read-a32-vm-and-argv.md`.

## D19. correctness

- Repo: `a32rt`
- Ref: `master`
- Commit: `d84feb311a0a4676d56e81590b7bd442f57f4a17`
- Path: `src/vm.c`
- Lines: 778–779 and 853–854
- Symbol: `a32_vm_mremap`

`map_flags` is `MAP_SHARED` or `MAP_PRIVATE` from the old flags, then always `| A32_MAP_ANONYMOUS` (`0x20`).
Both `vm_mmap2_locked` calls pass fd `-1` and offset `0`, and the bytes are `memcpy`'d.
A `MAP_SHARED` file mapping becomes a private anonymous copy. Later stores do not update the file.
The calls were not changed.
Second read: `blind/blind-read-a32-vm-and-argv.md`.
