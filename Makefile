# Self-documenting Makefile for a32rt — AArch32 -> AArch64 user-mode translator
#
# Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
# SPDX-License-Identifier: Apache-2.0

.DEFAULT_GOAL := help
.PHONY: help all build tests \
        test-hello-hl test-hello-lima \
        test-m1-hl test-m1-lima \
        test-m2-hl test-m2-lima \
        test-m3-hl test-m3-lima \
        test-m4-hl test-m4-lima \
        test-m5-hl test-m5-lima \
        test-all-hl test-all-lima \
        clean

BUILD_DIR := _build
HL_DIR    ?= ../hyper-linux
HL_BIN    ?= $(HL_DIR)/_build/hl
LIMA_VM   ?= hl-rosetta

# Locate aarch64-unknown-linux-musl-gcc (from PATH or cached /nix/store wrapper)
A64_CC_PATH := $(shell command -v aarch64-unknown-linux-musl-gcc 2>/dev/null)
ifeq ($(A64_CC_PATH),)
  A64_CC_PATH := $(shell ls -d /nix/store/*-aarch64-unknown-linux-musl-gcc-wrapper-15.3*/bin/aarch64-unknown-linux-musl-gcc 2>/dev/null | head -n 1)
endif
ifneq ($(origin CC),command line)
  CC := $(A64_CC_PATH)
endif

# Locate unwrapped clang + ld.lld for fast local armv7 ELF32 fixture builds
ARM32_CC_PATH := $(shell command -v armv7l-unknown-linux-musleabihf-gcc 2>/dev/null)
CLANG_RAW     := $(shell ls -d /nix/store/*-clang-21.*/bin/clang /nix/store/*-clang-19.*/bin/clang 2>/dev/null | tail -n 1)
ifeq ($(CLANG_RAW),)
  CLANG_RAW := clang
endif
LLD_RAW       := $(shell ls -d /nix/store/*-lld-21.*/bin/ld.lld /nix/store/*-lld-19.*/bin/ld.lld 2>/dev/null | tail -n 1)
ifeq ($(LLD_RAW),)
  LLD_RAW := ld.lld
endif
LIMACTL       ?= $(shell command -v limactl 2>/dev/null || ls -d /nix/store/*-lima-*/bin/limactl 2>/dev/null | tail -n 1)

CFLAGS := -std=c11 -Werror -Werror=vla -pedantic -Wall -Wextra -O2 -fPIE \
          -fno-stack-protector -Iinclude -Isrc
LDFLAGS := -static-pie -Wl,-z,max-page-size=4096

SRCS := src/main.c \
        src/host.c \
        src/vm.c \
        src/elf32.c \
        src/stack32.c \
        src/a64_emit.c \
        src/decode_a32.c \
        src/decode_t32.c \
        src/vfp_neon.c \
        src/jit.c \
        src/aot.c \
        src/syscalls.c \
        src/signals.c \
        src/gdb.c
ASM_SRCS := src/trampoline.S
HDRS := include/a32host.h src/a32rt.h src/a64_emit.h

A32RT_BIN := $(BUILD_DIR)/a32rt
TEST_BINS := $(BUILD_DIR)/hello-armhf-static \
             $(BUILD_DIR)/hello-thumb-static \
             $(BUILD_DIR)/test-m1-armhf \
             $(BUILD_DIR)/test-m1-thumb \
             $(BUILD_DIR)/bench-coremark-armhf \
             $(BUILD_DIR)/bench-coremark-thumb \
             $(BUILD_DIR)/test-m3-armhf \
             $(BUILD_DIR)/test-m4-armhf \
             $(BUILD_DIR)/test-m4-thumb \
             $(BUILD_DIR)/test-m5-armhf

ARM32_FREESTANDING_CFLAGS := --target=armv7a-unknown-linux-eabihf -march=armv7ve -mfpu=neon-vfpv4 \
                             -mfloat-abi=hard -std=c11 -Werror=vla -pedantic -Wall -Wextra -O2 \
                             -ffreestanding -fno-stack-protector -nostdlib -DA32RT_FREESTANDING_TEST

help: ## Show available build and verification targets
	@awk 'BEGIN {FS = ":.*##"; printf "\nUsage:\n  make \033[36m<target>\033[0m\n\nTargets:\n"} \
		/^[a-zA-Z0-9_-]+:.*?##/ { printf "  \033[36m%-20s\033[0m %s\n", $$1, $$2 }' $(MAKEFILE_LIST)

all: build tests ## Build a32rt and all arm32 M0-M5 test fixtures

OBJ_DIR  := $(BUILD_DIR)/obj
C_OBJS   := $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(SRCS))
ASM_OBJS := $(patsubst src/%.S,$(OBJ_DIR)/%.o,$(ASM_SRCS))
OBJS     := $(C_OBJS) $(ASM_OBJS)

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

$(OBJ_DIR):
	@mkdir -p $(OBJ_DIR)

$(OBJ_DIR)/%.o: src/%.c $(HDRS) | $(OBJ_DIR)
	@$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: src/%.S $(HDRS) | $(OBJ_DIR)
	@$(CC) $(CFLAGS) -c $< -o $@

build: $(A32RT_BIN) ## Build static-PIE aarch64-linux a32rt binary

$(A32RT_BIN): $(OBJS) | $(BUILD_DIR)
	@printf "\033[32m▸ Linking\033[0m $@\n"
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) -o $@
	@ln -sf a32rt $(BUILD_DIR)/a32rt-binfmt-p

tests: $(TEST_BINS) ## Build arm32 (A32 and Thumb-2) test fixtures for M0..M5

define BUILD_ARM32_FIXTURE
$(BUILD_DIR)/$(1): tests/$(2) | $(BUILD_DIR)
	@printf "\033[32m▸ Building\033[0m $$@ ($(3))\n"
	$(CLANG_RAW) $(ARM32_FREESTANDING_CFLAGS) $(3) -c $$< -o $(BUILD_DIR)/$(1).o
	$(LLD_RAW) -m armelf_linux_eabi -Bstatic --no-dynamic-linker \
		--image-base=0x00010000 -e _start $(BUILD_DIR)/$(1).o -o $$@
endef

$(eval $(call BUILD_ARM32_FIXTURE,hello-armhf-static,hello.c,-marm))
$(eval $(call BUILD_ARM32_FIXTURE,hello-thumb-static,hello.c,-mthumb))
$(eval $(call BUILD_ARM32_FIXTURE,test-m1-armhf,test_m1_isa_fs.c,-marm))
$(eval $(call BUILD_ARM32_FIXTURE,test-m1-thumb,test_m1_isa_fs.c,-mthumb))
$(eval $(call BUILD_ARM32_FIXTURE,bench-coremark-armhf,bench_coremark.c,-marm))
$(eval $(call BUILD_ARM32_FIXTURE,bench-coremark-thumb,bench_coremark.c,-mthumb))
$(eval $(call BUILD_ARM32_FIXTURE,test-m3-armhf,test_m3_threads_signals.c,-marm))
$(eval $(call BUILD_ARM32_FIXTURE,test-m4-armhf,test_m4_vfp_neon.c,-marm))
$(eval $(call BUILD_ARM32_FIXTURE,test-m4-thumb,test_m4_vfp_neon.c,-mthumb))
$(eval $(call BUILD_ARM32_FIXTURE,test-m5-armhf,test_m5_ltp_gdb.c,-marm))

TIMEOUT_RUN ?= perl -e 'alarm 15; exec @ARGV'
SYSROOT_ARMV7 := $(abspath $(BUILD_DIR)/sysroot-armv7)

$(BUILD_DIR)/sysroot-armv7/bin/busybox: | $(BUILD_DIR)
	@printf "\033[32m▸ Fetching Alpine armv7 musl/busybox sysroot\033[0m\n"
	@mkdir -p $(BUILD_DIR)/sysroot-armv7
	@curl -fsSL https://dl-cdn.alpinelinux.org/alpine/v3.21/releases/armv7/alpine-minirootfs-3.21.8-armv7.tar.gz \
		| tar -xz -C $(BUILD_DIR)/sysroot-armv7

test-hello-hl: $(A32RT_BIN) $(BUILD_DIR)/hello-armhf-static $(BUILD_DIR)/hello-thumb-static ## Run M0-hl smoke test under hyper-linux (primary target)
	@printf "\033[32m▸ Verifying M0-hl (A32)\033[0m under $(HL_BIN)\n"
	@out="$$($(TIMEOUT_RUN) $(HL_BIN) $(abspath $(A32RT_BIN)) $(abspath $(BUILD_DIR)/hello-armhf-static))"; \
	if [ "$$out" != "hello" ]; then \
		printf "\033[31mFAIL\033[0m: expected 'hello', got '%s'\n" "$$out"; exit 1; \
	fi
	@printf "\033[32m▸ Verifying M0-hl (Thumb-2)\033[0m under $(HL_BIN)\n"
	@out="$$($(TIMEOUT_RUN) $(HL_BIN) $(abspath $(A32RT_BIN)) $(abspath $(BUILD_DIR)/hello-thumb-static))"; \
	if [ "$$out" != "hello" ]; then \
		printf "\033[31mFAIL\033[0m: expected 'hello', got '%s'\n" "$$out"; exit 1; \
	fi
	@printf "\033[32mPASS\033[0m: M0-hl (A32 + Thumb-2)\n"

test-hello-lima: $(A32RT_BIN) $(BUILD_DIR)/hello-armhf-static $(BUILD_DIR)/hello-thumb-static ## Run M0 smoke test inside limactl Linux VM (ground-truth oracle)
	@printf "\033[32m▸ Verifying M0\033[0m inside Lima VM ($(LIMA_VM))\n"
	@out="$$($(TIMEOUT_RUN) $(LIMACTL) shell --workdir $(abspath .) $(LIMA_VM) -- $(abspath $(A32RT_BIN)) -- $(abspath $(BUILD_DIR)/hello-armhf-static) hello)"; \
	if [ "$$out" != "hello" ]; then \
		printf "\033[31mFAIL\033[0m: expected 'hello', got '%s'\n" "$$out"; exit 1; \
	fi
	@out="$$($(TIMEOUT_RUN) $(LIMACTL) shell --workdir $(abspath .) $(LIMA_VM) -- $(abspath $(A32RT_BIN)) -- $(abspath $(BUILD_DIR)/hello-thumb-static) hello)"; \
	if [ "$$out" != "hello" ]; then \
		printf "\033[31mFAIL\033[0m: expected 'hello', got '%s'\n" "$$out"; exit 1; \
	fi
	@printf "\033[32mPASS\033[0m: M0 on Lima ($(LIMA_VM))\n"

test-m1-hl: $(A32RT_BIN) $(BUILD_DIR)/test-m1-armhf $(BUILD_DIR)/test-m1-thumb $(BUILD_DIR)/sysroot-armv7/bin/busybox ## Run M1-hl ISA, procfs, getdents64, fork/exec & musl busybox under hl
	@printf "\033[32m▸ Verifying M1-hl (A32 + Thumb-2 + musl busybox)\033[0m under $(HL_BIN)\n"
	@$(TIMEOUT_RUN) $(HL_BIN) $(abspath $(A32RT_BIN)) $(abspath $(BUILD_DIR)/test-m1-armhf)
	@$(TIMEOUT_RUN) $(HL_BIN) $(abspath $(A32RT_BIN)) $(abspath $(BUILD_DIR)/test-m1-thumb)
	@out="$$($(TIMEOUT_RUN) $(HL_BIN) $(abspath $(A32RT_BIN)) --sysroot $(SYSROOT_ARMV7) $(SYSROOT_ARMV7)/bin/busybox sh -c 'ls / && echo ok')"; \
	echo "$$out" | grep -q "^ok$$" || { printf "\033[31mFAIL\033[0m: busybox sh failed:\n%s\n" "$$out"; exit 1; }
	@printf "M1-hl musl ld-musl-armhf.so.1 + busybox sh (ls / && echo ok): PASS\n"
	@printf "\033[32mPASS\033[0m: M1-hl\n"

test-m1-lima: $(A32RT_BIN) $(BUILD_DIR)/test-m1-armhf $(BUILD_DIR)/test-m1-thumb $(BUILD_DIR)/sysroot-armv7/bin/busybox ## Run M1 ISA, procfs, getdents64, fork/exec & musl busybox inside Lima VM
	@printf "\033[32m▸ Verifying M1 (A32 + Thumb-2 + musl busybox)\033[0m inside Lima VM ($(LIMA_VM))\n"
	@$(TIMEOUT_RUN) $(LIMACTL) shell --workdir $(abspath .) $(LIMA_VM) -- $(abspath $(A32RT_BIN)) $(abspath $(BUILD_DIR)/test-m1-armhf)
	@$(TIMEOUT_RUN) $(LIMACTL) shell --workdir $(abspath .) $(LIMA_VM) -- $(abspath $(A32RT_BIN)) $(abspath $(BUILD_DIR)/test-m1-thumb)
	@out="$$($(TIMEOUT_RUN) $(LIMACTL) shell --workdir $(abspath .) $(LIMA_VM) -- $(abspath $(A32RT_BIN)) --sysroot $(SYSROOT_ARMV7) $(SYSROOT_ARMV7)/bin/busybox sh -c 'ls / && echo ok')"; \
	echo "$$out" | grep -q "^ok$$" || { printf "\033[31mFAIL\033[0m: busybox sh failed:\n%s\n" "$$out"; exit 1; }
	@printf "M1 musl ld-musl-armhf.so.1 + busybox sh (ls / && echo ok): PASS\n"
	@printf "\033[32mPASS\033[0m: M1 on Lima\n"

test-m2-hl: $(A32RT_BIN) $(BUILD_DIR)/bench-coremark-armhf $(BUILD_DIR)/bench-coremark-thumb $(BUILD_DIR)/sysroot-armv7/bin/busybox ## Run M2-hl CoreMark CRC & AOT cache verification under hl
	@printf "\033[32m▸ Verifying M2-hl (CoreMark A32 + Thumb-2 + AOT)\033[0m under $(HL_BIN)\n"
	@A32RT_STATS=1 $(TIMEOUT_RUN) $(HL_BIN) $(abspath $(A32RT_BIN)) $(abspath $(BUILD_DIR)/bench-coremark-armhf)
	@A32RT_STATS=1 $(TIMEOUT_RUN) $(HL_BIN) $(abspath $(A32RT_BIN)) $(abspath $(BUILD_DIR)/bench-coremark-thumb)
	@$(TIMEOUT_RUN) $(HL_BIN) $(abspath $(A32RT_BIN)) --cache $(abspath $(BUILD_DIR)) --aot $(abspath $(SYSROOT_ARMV7)/lib/ld-musl-armhf.so.1)
	@$(TIMEOUT_RUN) $(HL_BIN) $(abspath $(A32RT_BIN)) --cache $(abspath $(BUILD_DIR)) --aot --sysroot $(abspath $(SYSROOT_ARMV7)) $(abspath $(SYSROOT_ARMV7)/bin/busybox)
	@out="$$($(TIMEOUT_RUN) $(HL_BIN) $(abspath $(A32RT_BIN)) --cache $(abspath $(BUILD_DIR)) --sysroot $(abspath $(SYSROOT_ARMV7)) $(abspath $(SYSROOT_ARMV7)/bin/busybox) echo aot_interp_ok)"; \
	rm -f $(BUILD_DIR)/a32rt-*cache-*.bin; \
	[ "$$out" = "aot_interp_ok" ] || { printf "\033[31mFAIL\033[0m: busybox AOT failed: %s\n" "$$out"; exit 1; }
	@printf "\033[32mPASS\033[0m: M2-hl\n"

test-m2-lima: $(A32RT_BIN) $(BUILD_DIR)/bench-coremark-armhf $(BUILD_DIR)/bench-coremark-thumb $(BUILD_DIR)/sysroot-armv7/bin/busybox ## Run M2 CoreMark CRC & AOT cache verification inside Lima VM
	@printf "\033[32m▸ Verifying M2 (CoreMark A32 + Thumb-2 + AOT)\033[0m inside Lima VM ($(LIMA_VM))\n"
	@$(TIMEOUT_RUN) $(LIMACTL) shell --workdir $(abspath .) $(LIMA_VM) -- $(abspath $(A32RT_BIN)) --aot $(abspath $(BUILD_DIR)/bench-coremark-armhf)
	@$(TIMEOUT_RUN) $(LIMACTL) shell --workdir $(abspath .) $(LIMA_VM) -- $(abspath $(A32RT_BIN)) --stats $(abspath $(BUILD_DIR)/bench-coremark-armhf)
	@$(TIMEOUT_RUN) $(LIMACTL) shell --workdir $(abspath .) $(LIMA_VM) -- $(abspath $(A32RT_BIN)) --stats $(abspath $(BUILD_DIR)/bench-coremark-thumb)
	@$(TIMEOUT_RUN) $(LIMACTL) shell --workdir $(abspath .) $(LIMA_VM) -- $(abspath $(A32RT_BIN)) --aot $(abspath $(SYSROOT_ARMV7)/lib/ld-musl-armhf.so.1)
	@$(TIMEOUT_RUN) $(LIMACTL) shell --workdir $(abspath .) $(LIMA_VM) -- $(abspath $(A32RT_BIN)) --aot --sysroot $(abspath $(SYSROOT_ARMV7)) $(abspath $(SYSROOT_ARMV7)/bin/busybox)
	@out="$$($(TIMEOUT_RUN) $(LIMACTL) shell --workdir $(abspath .) $(LIMA_VM) -- $(abspath $(A32RT_BIN)) --sysroot $(abspath $(SYSROOT_ARMV7)) $(abspath $(SYSROOT_ARMV7)/bin/busybox) echo aot_interp_ok)"; \
	[ "$$out" = "aot_interp_ok" ] || { printf "\033[31mFAIL\033[0m: busybox AOT failed: %s\n" "$$out"; exit 1; }
	@printf "\033[32mPASS\033[0m: M2 on Lima\n"

test-m3-hl: $(A32RT_BIN) $(BUILD_DIR)/test-m3-armhf ## Run M3 threads, futex, signals, and SMC verification under hl
	@printf "\033[32m▸ Verifying M3-hl (threads, futex, signals, SMC)\033[0m under $(HL_BIN)\n"
	@$(TIMEOUT_RUN) $(HL_BIN) $(abspath $(A32RT_BIN)) $(abspath $(BUILD_DIR)/test-m3-armhf)
	@printf "\033[32mPASS\033[0m: M3-hl\n"

test-m3-lima: $(A32RT_BIN) $(BUILD_DIR)/test-m3-armhf ## Run M3 threads, futex, signals, and SMC verification inside Lima VM
	@printf "\033[32m▸ Verifying M3 (threads, futex, signals, SMC)\033[0m inside Lima VM ($(LIMA_VM))\n"
	@$(TIMEOUT_RUN) $(LIMACTL) shell --workdir $(abspath .) $(LIMA_VM) -- $(abspath $(A32RT_BIN)) $(abspath $(BUILD_DIR)/test-m3-armhf)
	@printf "\033[32mPASS\033[0m: M3 on Lima\n"

test-m4-hl: $(A32RT_BIN) $(BUILD_DIR)/test-m4-armhf $(BUILD_DIR)/test-m4-thumb ## Run M4 VFPv4-D32 and NEON verification under hl
	@printf "\033[32m▸ Verifying M4-hl (VFPv4-D32 + NEON, A32 + Thumb-2)\033[0m under $(HL_BIN)\n"
	@$(TIMEOUT_RUN) $(HL_BIN) $(abspath $(A32RT_BIN)) $(abspath $(BUILD_DIR)/test-m4-armhf)
	@$(TIMEOUT_RUN) $(HL_BIN) $(abspath $(A32RT_BIN)) $(abspath $(BUILD_DIR)/test-m4-thumb)
	@printf "\033[32mPASS\033[0m: M4-hl\n"

test-m4-lima: $(A32RT_BIN) $(BUILD_DIR)/test-m4-armhf $(BUILD_DIR)/test-m4-thumb ## Run M4 VFPv4-D32 and NEON verification inside Lima VM
	@printf "\033[32m▸ Verifying M4 (VFPv4-D32 + NEON, A32 + Thumb-2)\033[0m inside Lima VM ($(LIMA_VM))\n"
	@$(TIMEOUT_RUN) $(LIMACTL) shell --workdir $(abspath .) $(LIMA_VM) -- $(abspath $(A32RT_BIN)) $(abspath $(BUILD_DIR)/test-m4-armhf)
	@$(TIMEOUT_RUN) $(LIMACTL) shell --workdir $(abspath .) $(LIMA_VM) -- $(abspath $(A32RT_BIN)) $(abspath $(BUILD_DIR)/test-m4-thumb)
	@printf "\033[32mPASS\033[0m: M4 on Lima\n"

test-m5-hl: $(A32RT_BIN) $(BUILD_DIR)/test-m5-armhf ## Run M5 LTP syscall conformance and GDB RSP verification under hl
	@printf "\033[32m▸ Verifying M5-hl (LTP syscall conformance + compat shims)\033[0m under $(HL_BIN)\n"
	@$(TIMEOUT_RUN) $(HL_BIN) $(abspath $(A32RT_BIN)) $(abspath $(BUILD_DIR)/test-m5-armhf)
	@printf "\033[32mPASS\033[0m: M5-hl\n"

test-m5-lima: $(A32RT_BIN) $(BUILD_DIR)/test-m5-armhf $(BUILD_DIR)/hello-armhf-static ## Run M5 LTP syscall conformance and GDB RSP verification inside Lima VM
	@printf "\033[32m▸ Verifying M5 (LTP syscall conformance + GDB RSP)\033[0m inside Lima VM ($(LIMA_VM))\n"
	@$(TIMEOUT_RUN) $(LIMACTL) shell --workdir $(abspath .) $(LIMA_VM) -- $(abspath $(A32RT_BIN)) $(abspath $(BUILD_DIR)/test-m5-armhf)
	@$(TIMEOUT_RUN) $(LIMACTL) shell --workdir $(abspath .) $(LIMA_VM) -- python3 -c 'import socket, subprocess, time; p = subprocess.Popen(["./_build/a32rt", "--gdb", "14321", "./_build/hello-armhf-static"], stdout=subprocess.PIPE, stderr=subprocess.PIPE); time.sleep(0.2); s = socket.create_connection(("127.0.0.1", 14321), timeout=3); rpkt = lambda: (lambda b=b"": [b := b + s.recv(1024) for _ in range(16) if b"#" not in b or len(b.split(b"#")[-1]) < 2] and b.decode())(); s.sendall(b"$$?#3f"); r1 = rpkt(); s.sendall(b"$$g#67"); r2 = rpkt(); s.sendall(b"$$s#73"); r3 = rpkt(); s.sendall(b"$$c#63"); out, _ = p.communicate(timeout=3); assert "S05" in r1 and len(r2) >= 128 and "S05" in r3 and out == b"hello\n", (r1, r2, r3, out); print("M5 GDB RSP Stub: PASS")'
	@printf "\033[32mPASS\033[0m: M5 on Lima\n"

test-all-hl: test-hello-hl test-m1-hl test-m2-hl test-m3-hl test-m4-hl test-m5-hl ## Run complete M0..M5 suite under hyper-linux

test-all-lima: test-hello-lima test-m1-lima test-m2-lima test-m3-lima test-m4-lima test-m5-lima ## Run complete M0..M5 suite inside Lima VM

BENCH_BINS := $(BUILD_DIR)/bench-suite-armhf \
              $(BUILD_DIR)/bench-suite-thumb \
              $(BUILD_DIR)/bench-suite-aarch64 \
              $(BUILD_DIR)/bench-suite-x86_64 \
              $(BUILD_DIR)/bench-suite-i386

$(eval $(call BUILD_ARM32_FIXTURE,bench-suite-armhf,bench_suite.c,-marm))
$(eval $(call BUILD_ARM32_FIXTURE,bench-suite-thumb,bench_suite.c,-mthumb))

$(BUILD_DIR)/bench-suite-aarch64: tests/bench_suite.c | $(BUILD_DIR)
	@printf "\033[32m▸ Building\033[0m $@ (aarch64)\n"
	$(CLANG_RAW) --target=aarch64-unknown-linux-gnu -std=c11 -Werror=vla -pedantic -Wall -Wextra -O2 \
		-ffreestanding -fno-stack-protector -nostdlib -c $< -o $@.o
	$(LLD_RAW) -m aarch64linux -Bstatic --no-dynamic-linker --image-base=0x00400000 -e _start $@.o -o $@

$(BUILD_DIR)/bench-suite-x86_64: tests/bench_suite.c | $(BUILD_DIR)
	@printf "\033[32m▸ Building\033[0m $@ (x86_64)\n"
	$(CLANG_RAW) --target=x86_64-unknown-linux-gnu -std=c11 -Werror=vla -pedantic -Wall -Wextra -O2 \
		-ffreestanding -fno-stack-protector -nostdlib -c $< -o $@.o
	$(LLD_RAW) -m elf_x86_64 -Bstatic --no-dynamic-linker --image-base=0x00400000 -e _start $@.o -o $@

$(BUILD_DIR)/bench-suite-i386: tests/bench_suite.c | $(BUILD_DIR)
	@printf "\033[32m▸ Building\033[0m $@ (i386)\n"
	$(CLANG_RAW) --target=i686-unknown-linux-gnu -std=c11 -Werror=vla -pedantic -Wall -Wextra -O2 \
		-ffreestanding -fno-stack-protector -nostdlib -c $< -o $@.o
	$(LLD_RAW) -m elf_i386 -Bstatic --no-dynamic-linker -e _start $@.o -o $@

bench-bins: $(BENCH_BINS) ## Build cross-architecture benchmark binaries (armhf, thumb, aarch64, x86_64, i386)

bench-hl: $(A32RT_BIN) $(BENCH_BINS) ## Run multi-workload benchmark suite under hyper-linux (native, a32rt Cold JIT & Warm AOT, RosettaLinux)
	@printf "\033[36m=== [hyper-linux] 1. Native aarch64-linux ===\033[0m\n"
	@$(TIMEOUT_RUN) $(HL_BIN) $(abspath $(BUILD_DIR)/bench-suite-aarch64)
	@printf "\033[36m=== [hyper-linux] 2. a32rt A32 (Cold JIT, G=4GiB) ===\033[0m\n"
	@rm -f $(BUILD_DIR)/a32rt-*cache-*.bin
	@$(TIMEOUT_RUN) $(HL_BIN) $(abspath $(A32RT_BIN)) --stats $(abspath $(BUILD_DIR)/bench-suite-armhf)
	@printf "\033[36m=== [hyper-linux] 3. a32rt A32 (Warm .a32cache AOT, G=4GiB) ===\033[0m\n"
	@$(TIMEOUT_RUN) $(HL_BIN) $(abspath $(A32RT_BIN)) --cache $(abspath $(BUILD_DIR)) --aot $(abspath $(BUILD_DIR)/bench-suite-armhf)
	@$(TIMEOUT_RUN) $(HL_BIN) $(abspath $(A32RT_BIN)) --cache $(abspath $(BUILD_DIR)) --stats $(abspath $(BUILD_DIR)/bench-suite-armhf)
	@printf "\033[36m=== [hyper-linux] 4. a32rt Thumb-2 (Cold JIT, G=4GiB) ===\033[0m\n"
	@rm -f $(BUILD_DIR)/a32rt-*cache-*.bin
	@$(TIMEOUT_RUN) $(HL_BIN) $(abspath $(A32RT_BIN)) --stats $(abspath $(BUILD_DIR)/bench-suite-thumb)
	@printf "\033[36m=== [hyper-linux] 5. a32rt Thumb-2 (Warm .a32cache AOT, G=4GiB) ===\033[0m\n"
	@$(TIMEOUT_RUN) $(HL_BIN) $(abspath $(A32RT_BIN)) --cache $(abspath $(BUILD_DIR)) --aot $(abspath $(BUILD_DIR)/bench-suite-thumb)
	@$(TIMEOUT_RUN) $(HL_BIN) $(abspath $(A32RT_BIN)) --cache $(abspath $(BUILD_DIR)) --stats $(abspath $(BUILD_DIR)/bench-suite-thumb)
	@rm -f $(BUILD_DIR)/a32rt-*cache-*.bin
	@printf "\033[36m=== [hyper-linux] 6. Apple RosettaLinux (x86_64 -> aarch64) ===\033[0m\n"
	@$(TIMEOUT_RUN) $(HL_BIN) $(abspath $(BUILD_DIR)/bench-suite-x86_64)

bench-lima: $(A32RT_BIN) $(BENCH_BINS) ## Run multi-workload benchmark suite inside Lima VM (native, a32rt, qemu-arm, RosettaLinux, qemu-x86_64, qemu-i386)
	@printf "\033[36m=== [Lima Linux VM] 1. Native aarch64-linux ===\033[0m\n"
	@$(TIMEOUT_RUN) $(LIMACTL) shell --workdir $(abspath .) $(LIMA_VM) -- $(abspath $(BUILD_DIR)/bench-suite-aarch64)
	@printf "\033[36m=== [Lima Linux VM] 2. a32rt A32 (Warm .a32cache AOT, G=0) ===\033[0m\n"
	@$(TIMEOUT_RUN) $(LIMACTL) shell --workdir $(abspath .) $(LIMA_VM) -- $(abspath $(A32RT_BIN)) --aot $(abspath $(BUILD_DIR)/bench-suite-armhf)
	@$(TIMEOUT_RUN) $(LIMACTL) shell --workdir $(abspath .) $(LIMA_VM) -- $(abspath $(A32RT_BIN)) --stats $(abspath $(BUILD_DIR)/bench-suite-armhf)
	@printf "\033[36m=== [Lima Linux VM] 3. a32rt Thumb-2 (Warm .a32cache AOT, G=0) ===\033[0m\n"
	@$(TIMEOUT_RUN) $(LIMACTL) shell --workdir $(abspath .) $(LIMA_VM) -- $(abspath $(A32RT_BIN)) --aot $(abspath $(BUILD_DIR)/bench-suite-thumb)
	@$(TIMEOUT_RUN) $(LIMACTL) shell --workdir $(abspath .) $(LIMA_VM) -- $(abspath $(A32RT_BIN)) --stats $(abspath $(BUILD_DIR)/bench-suite-thumb)
	@printf "\033[36m=== [Lima Linux VM] 4. qemu-arm 10.1.0 TCG (A32) ===\033[0m\n"
	@$(TIMEOUT_RUN) $(LIMACTL) shell --workdir $(abspath .) $(LIMA_VM) -- qemu-arm $(abspath $(BUILD_DIR)/bench-suite-armhf)
	@printf "\033[36m=== [Lima Linux VM] 5. qemu-arm 10.1.0 TCG (Thumb-2) ===\033[0m\n"
	@$(TIMEOUT_RUN) $(LIMACTL) shell --workdir $(abspath .) $(LIMA_VM) -- qemu-arm $(abspath $(BUILD_DIR)/bench-suite-thumb)
	@printf "\033[36m=== [Lima Linux VM] 6. Apple RosettaLinux (x86_64 -> aarch64) ===\033[0m\n"
	@$(TIMEOUT_RUN) $(LIMACTL) shell --workdir $(abspath .) $(LIMA_VM) -- /mnt/lima-rosetta/rosetta $(abspath $(BUILD_DIR)/bench-suite-x86_64)
	@printf "\033[36m=== [Lima Linux VM] 7. qemu-x86_64 10.1.0 TCG (x86_64) ===\033[0m\n"
	@$(TIMEOUT_RUN) $(LIMACTL) shell --workdir $(abspath .) $(LIMA_VM) -- qemu-x86_64 $(abspath $(BUILD_DIR)/bench-suite-x86_64)
	@printf "\033[36m=== [Lima Linux VM] 8. qemu-i386 10.1.0 TCG (32-bit x86) ===\033[0m\n"
	@$(TIMEOUT_RUN) $(LIMACTL) shell --workdir $(abspath .) $(LIMA_VM) -- qemu-i386 $(abspath $(BUILD_DIR)/bench-suite-i386)

bench: bench-hl bench-lima ## Run complete benchmark suite across hyper-linux and Lima VM

clean: ## Remove build artifacts
	rm -rf $(BUILD_DIR)
