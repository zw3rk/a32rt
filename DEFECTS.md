# Defects

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
