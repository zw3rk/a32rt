# `a32rt` Host Contract (`A32H_ABI 1.0`)

`a32rt` is a host-agnostic, static-PIE `aarch64-linux` user-mode translator for 32-bit ARM (`ELFCLASS32`, `EM_ARM = 40`, EABI hard-float and soft-float) binaries. It runs unmodified on plain AArch64 Linux kernels (`binfmt_misc` or direct invocation) and inside `hyper-linux` (`hl`) on macOS Apple Silicon.

---

## 1. Invocation Forms

`a32rt` accepts three command-line invocation forms:

1. **Explicit loader form** (used by `hl` and CLI users):
   ```
   a32rt [a32rt-options] -- <PATH> <ARGV0> [ARGS...]
   ```
   Options before `--`:
   - `--window <hex>`: override the 4 GiB guest window base `G` (same as `A32RT_WINDOW=<hex>`).
   - `--sysroot <path>`: prefix path for `PT_INTERP` and absolute library lookups (same as `A32RT_SYSROOT=<path>`).
   - `--stats`: print translation and execution statistics on exit (same as `A32RT_STATS=1`).
   - `--trace`: trace guest basic blocks and syscalls to stderr (same as `A32RT_TRACE=1`).

2. **`binfmt_misc` `P`-flag form** (when `argv[0]` ends with `a32rt-binfmt-p` or `A32RT_BINFMT_P=1`):
   ```
   a32rt-binfmt-p <PATH> <ARGV0> [ARGS...]
   ```

3. **Legacy non-`P` form**:
   ```
   a32rt <PATH> [ARGS...]
   ```
   Here guest `argv[0]` defaults to `<PATH>`.

### Pre-Opened Binary (`AT_EXECFD`)
If `AT_EXECFD` is present and non-zero in the host `auxv`, `a32rt` reads the target 32-bit ELF from that file descriptor instead of opening `<PATH>`.

---

## 2. Host Syscall Floor (`M0`)

`a32rt` issues only standard `aarch64-linux` syscalls:
- Memory management: `mmap` (222), `munmap` (215), `mprotect` (226)
- File I/O: `openat` (56), `close` (57), `read` (63), `write` (64), `readv` (65), `writev` (66), `lseek` (62), `newfstatat` (79), `fstat` (80), `ioctl` (29)
- Process & identity: `exit` (93), `exit_group` (94), `set_tid_address` (96), `uname` (160), `getuid` (174), `geteuid` (175), `getgid` (176), `getegid` (177)
- Signals: `rt_sigaction` (134), `rt_sigprocmask` (135), `sigaltstack` (132)
- Clocks & entropy: `clock_gettime` (113), `getrandom` (278)

---

## 3. Address Space & Guest Window `[G, G + 4 GiB)`

`a32rt` reserves a contiguous 4 GiB window `[G, G + 4 GiB)` below `2^47` using `mmap(..., PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0)`.

Resolution order for `G`:
1. `--window <hex>` or `A32RT_WINDOW=<hex>` environment variable.
2. `info.window_hint` from `/dev/a32host` (`A32H_GET_INFO`), if non-zero.
3. `G = 0` (reserving `[mmap_min_addr, 4 GiB)`), falling back to scanning `4 GiB`-aligned bases (`0x1_0000_0000`, `0x2_0000_0000`, ...) if `G = 0` is already occupied.

---

## 4. Optional `/dev/a32host` Handshake (`include/a32host.h`)

At startup (unless `A32RT_HOST=none`), `a32rt` opens `/dev/a32host` (`O_RDWR | O_CLOEXEC`) and issues `ioctl(fd, A32H_GET_INFO, &info)` (`0x81006101UL`). If `openat` or `ioctl` returns an error (`ENOENT`, `ENODEV`, `EACCES`, `ENOTTY`), `a32rt` closes the descriptor (if open) and proceeds with standard Linux defaults.
