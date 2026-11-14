# `a32rt` Defect Ledger (`DEFECTS.md`)

## Round 19 — 32-Bit iOS (`armv7`/`armv7s`) & Thumb-2/DBT Audit (`master` @ `8d87576`, 2026-11-14)

Audited across 6 parallel `claude-fable-5-1` subsystem deep-dives (`../rt-series/lode/reviews/convergence/2026-11-14-a32rt-ios-lane{1..6}-fable.md`) and cross-verified against C/ASM sources and `clang -target armv7-apple-ios9.0 -O2 -S` APCS-GNU ABI output. See [`PERF.md`](PERF.md) for the companion performance optimization ledger (`A32-IOS-P1`..`A32-IOS-P9`).

### 1. Security, Memory-Safety & Concurrency (`P1`)

- [ ] **A32-IOS-D1 (`SECURITY`, `src/ios/ios_stubs.c:363-385, 645-694`)** — Host stack out-of-bounds read in synthetic `printf` (`pfmt1` bounds copy by untruncated `snprintf` return `w` instead of `sizeof(tmp) - 1`) and `snprintf`/`vsnprintf` (`cp` not clamped to `sizeof(buf) - 1`).
- [ ] **A32-IOS-D2 (`SECURITY`, `src/ios/ios_svc_bsd.c:635-647, 685-694`)** — Unchecked guest VA range in `madvise` (75) and `msync` (65) + Darwin `MADV_FREE_REUSE (8)` / `MADV_CAN_REUSE (9)` passed directly to Linux host `MADV_FREE (8)` / `MADV_REMOVE (9)`.
- [ ] **A32-IOS-D3 (`SECURITY`, `src/dbt/dbt_aot_cache.c:151, 520-532`)** — Missing `st.st_uid == geteuid()` ownership check in `load_cache_from_path` when loading `/tmp/a32rt-cache-*.bin` AOT caches.
- [ ] **A32-IOS-D4 (`CONCURRENCY`, `src/core/core_emit_mem.c:27-74`, `src/dbt/dbt_jit_query.c:131, 183-194`)** — Data race on non-TLS `g_site_stage[]` during unlocked multi-threaded JIT block translation + stale pre-SMC block installation when `cur_epoch != use_epoch`.

### 2. XNU 32-Bit iOS Functional & ABI Bugs (`P1`)

- [ ] **A32-IOS-D5 (`LOADER`, `src/ios/ios_bind.c:45-46, 333-339, 384-405`)** — Weak imports (`BIND_SYMBOL_FLAGS_WEAK_IMPORT` / `N_WEAK_REF`) abort Mach-O load instead of binding `0`; `INDIRECT_SYMBOL_LOCAL`/`ABS` masks set to `0xFFFFFFFF`/`0xFFFFFFFE` instead of `0x80000000`/`0x40000000`.
- [ ] **A32-IOS-D6 (`THREADS`, `src/ios/ios_threads.c:76, 438-518, 612, 648-700`)** — Static `PTHREAD_MUTEX_INITIALIZER` (`0x4D555458`) / `PTHREAD_COND_INITIALIZER` (`0x434F4E44`) fail with `EINVAL`; `psynch`/`ulock` 64-bucket condvars lose wakeups on collisions; `pthread_join` lacks self/main guards; TSD destructors skipped on thread exit.
- [ ] **A32-IOS-D7 (`SIGNALS`, `src/ios/ios_signals.c:146, 641-696`, `src/ios/ios_svc_bsd.c:853-855`)** — Self-delivered signals (`sigsuspend`, `kill(self)`, `pthread_kill(self)`) clobber `_sigtramp` `r0` handler pointer or save pre-syscall `r0` in `mcontext`; `g_ios_pending_info[32]` is non-TLS.
- [ ] **A32-IOS-D8 (`STUBS`, `src/ios/ios_stubs.c:617-625`)** — Synthetic `fprintf` (`id 49`) shifts `r1 = r2; r2 = r3;` and passes `start = 1U`, skipping the first format argument (`r2`).
- [ ] **A32-IOS-D9 (`MACH`, `src/ios/ios_svc_mach.c:440, 722`)** — 8th Mach trap argument (`cur_protection` in `vm_map` trap 15, `rcv_msg` in `mach_msg_overwrite_trap` trap 32) read from `[sp+16]` instead of `r8` (`osfmk/mach/arm/syscall_sw.h` `_kernel_trap_6to9`).
- [ ] **A32-IOS-D10 (`BSD-ABI`, `src/ios/ios_svc_bsd*.c`)** — `getdirentries64` `d_name` offset (`24` vs `21`) & `basep` seek; `statfs64` `f_bsize` layout & `AT_SYMLINK_FOLLOW` (`0x20` vs `0x40`); `openat` ignoring `dirfd` & `F_GETPATH` requiring 4096B instead of `1024B`; `rl_host` permuting `RLIMIT_*` on macOS & `sysctl` truncation errno; macOS 12-byte `cmsghdr` (`SCM_RIGHTS`) & `MSG_DONTWAIT`; `kevent` `POLLIN|POLLOUT` spin, timer deadlines & `kevent_qos` (`374`).
- [ ] **A32-IOS-D11 (`OS`, `src/os/os_sys_thread_sig.c:233-274`, `src/os/backend_darwin.c`)** — Darwin-host Linux `futex` (`FUTEX_WAIT`) lacks `a32_guest_range_ok` and ignores `FUTEX_WAKE`.

### 3. Loader, Mach, Stubs, Interpreter & DBT Completeness (`P2`)

- [ ] **A32-IOS-D12 (`LOADER`, `src/ios/ios_dylib.c:116-120, 271-276, 369-370`, `src/ios/ios_macho.c:617`)** — `__mod_init_func` DFS post-order, `ios_apply_prot` unaligned page rounding (`start + vmsize`), `(st_dev, st_ino)`/`install_name` dylib dedup, and fixed-segment overlap check.
- [ ] **A32-IOS-D13 (`EXPORT`, `src/ios/ios_export.c:125-130, 218-222, 343`)** — Export-trie >255B edge truncation and non-`libSystem` two-level ordinal fallback.
- [ ] **A32-IOS-D14 (`CRT0/AOT`, `src/ios/ios_commpage_stack.c:174-177`, `src/ios/ios_stubs.c:859`, `src/ios/ios_svc_bsd_proc.c:130`)** — CRT0 return via `exit()` running LIFO `__cxa_atexit`/`atexit` callbacks, plus `--stats` and `--auto-aot` persistence on iOS process exit.
- [ ] **A32-IOS-D15 (`MACH`, `src/ios/ios_svc_mach.c:250-256, 692-706, 821`, `src/ios/ios_stubs.c:834`)** — Complex 40B MIG port descriptor reply in `task_get_special_port`, `mach_port_destruct` cleanup, `MACH_SEND_NO_BUFFER` on full queue, and real Mach port from `_mach_thread_self`.
- [ ] **A32-IOS-D16 (`STUBS`, `src/ios/ios_stubs.c:297, 418, 788, 806, 857`, `tests/ios/libSystem.tbd`)** — `strtol`/`strtoul` `endptr`, `sysctl` stub errno sign, `nanosleep` `EINTR`, `abort` `SIGABRT`, synthetic `FILE*` zero/flags init, and `libcompiler_rt` `___divdi3`/`___udivdi3`/`___moddi3`/`___umoddi3` exports.
- [ ] **A32-IOS-D17 (`INTERP`, `src/interp/interp_vfp_neon.c`)** — Full ARMv7 NEON reference interpreter coverage (`VMUL.I*`, `VMLA.I*`, `VMLS.I*`, long/wide/shifts, `.F32` vector ops under Standard FPSCR, `VLD2..4`/`VST2..4`, `VREV16/32`, `VDUP`/`VEXT`/`VTBL`/`VTRN`/`VUZP`/`VZIP`) + pre-flight memory range checks.
- [ ] **A32-IOS-D18 (`INTERP/DBT`, `src/interp/interp_thumb.c:82`, `interp_a32.c`, `interp_t16.c`, `interp_a32_ctl.c`)** — Darwin `__builtin_trap()` (`0xDEFE` Thumb / `0xE7FFDEFE` ARM) raises `SIGTRAP` (`A32_EXIT_BKPT`) when `ctx->is_ios`.
- [ ] **A32-IOS-D19 (`DBT`, `src/dbt/dbt_jit_run.c:147, 502-532, 592`)** — iOS protection-fault `SIGBUS` vs `SIGSEGV`, UNDEF-fallback `A32_EXIT_BKPT` `SIGTRAP`, host `fpcr` reset on guest fault recovery, and `pthread_jit_write_protect_np(1)` in `a32_dbt_single_step`.

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
