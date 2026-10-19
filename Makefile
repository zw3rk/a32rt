# Self-documenting Makefile for a32rt — AArch32 -> AArch64 user-mode translator
#
# Copyright 2026 Moritz Angermann <moritz@zw3rk.com>, zw3rk pte. ltd.
# SPDX-License-Identifier: Apache-2.0

.DEFAULT_GOAL := help
.PHONY: help all build tests check-headers \
        test-hello-hl test-hello-lima \
        test-m1-hl test-m1-lima \
        test-m2-hl test-m2-lima \
        test-m3-hl test-m3-lima \
        test-m4-hl test-m4-lima \
        test-m5-hl test-m5-lima \
        test-m6-hl test-m6-lima \
        test-m7-hl test-m7-lima \
        test-m8-hl test-m8-lima \
        test-all-hl test-all-lima \
        clean

BUILD_DIR := _build
HL_DIR    ?= ../hyper-linux
HL_BIN    ?= $(HL_DIR)/_build/hl
LIMA_VM   ?= hl-rosetta

# Locate aarch64-unknown-linux-musl-gcc (from PATH or cached /nix/store wrapper)
A64_CC_CACHED := /nix/store/h4svaqanhy15x4c3mwx5rp6kcjm4d1hp-aarch64-unknown-linux-musl-gcc-wrapper-15.3.0/bin/aarch64-unknown-linux-musl-gcc
A64_CC_PATH   := $(shell command -v aarch64-unknown-linux-musl-gcc 2>/dev/null || ([ -x $(A64_CC_CACHED) ] && echo $(A64_CC_CACHED)))
ifneq ($(origin CC),command line)
  CC := $(A64_CC_PATH)
endif

# Locate unwrapped clang / clang++ + ld.lld for fast local armv7 ELF32 fixture builds
CLANG_CACHED   := /nix/store/15n3slfdsd5zhyxrvaa7vm36f1jydwqp-clang-21.1.8/bin/clang
CLANGXX_CACHED := /nix/store/15n3slfdsd5zhyxrvaa7vm36f1jydwqp-clang-21.1.8/bin/clang++
LLD_CACHED     := /nix/store/smnrx7kvgcxbl6s2scgwk6zp0np6yw7q-lld-21.1.8/bin/ld.lld
CLANG_RAW      := $(shell [ -x $(CLANG_CACHED) ] && echo $(CLANG_CACHED) || command -v clang 2>/dev/null)
CLANGXX_RAW    := $(shell [ -x $(CLANGXX_CACHED) ] && echo $(CLANGXX_CACHED) || command -v clang++ 2>/dev/null)
LLD_RAW        := $(shell [ -x $(LLD_CACHED) ] && echo $(LLD_CACHED) || command -v ld.lld 2>/dev/null)
LIMACTL_CACHED := /nix/store/piidmzyl8dvi91i5y6hs8kaclbalk7bf-lima-2.1.4/bin/limactl
LIMACTL        ?= $(shell command -v limactl 2>/dev/null || ([ -x $(LIMACTL_CACHED) ] && echo $(LIMACTL_CACHED)))

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
             $(BUILD_DIR)/test-m5-armhf \
             $(BUILD_DIR)/test-m7-armhf \
             $(BUILD_DIR)/test-m7-thumb \
             $(BUILD_DIR)/test-m8-glibc-armhf \
             $(BUILD_DIR)/test-m8-glibc-thumb \
             $(BUILD_DIR)/test-m8-cpp-armhf \
             $(BUILD_DIR)/test-m8-cpp-thumb

ARM32_FREESTANDING_CFLAGS := --target=armv7a-unknown-linux-eabihf -march=armv7ve -mfpu=neon-vfpv4 \
                             -mfloat-abi=hard -std=c11 -Werror=vla -pedantic -Wall -Wextra -O2 \
                             -ffreestanding -fno-stack-protector -nostdlib -DA32RT_FREESTANDING_TEST

help: ## Show available build and verification targets
	@awk 'BEGIN {FS = ":.*##"; printf "\nUsage:\n  make \033[36m<target>\033[0m\n\nTargets:\n"} \
		/^[a-zA-Z0-9_-]+:.*?##/ { printf "  \033[36m%-20s\033[0m %s\n", $$1, $$2 }' $(MAKEFILE_LIST)

all: build tests ## Build a32rt and all arm32 M0-M8 test fixtures

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

tests: $(TEST_BINS) ## Build arm32 (A32 and Thumb-2) test fixtures for M0..M8

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
SYSROOT_GLIBC := $(abspath $(BUILD_DIR)/sysroot-glibc-armhf)

$(BUILD_DIR)/sysroot-armv7/bin/busybox: | $(BUILD_DIR)
	@printf "\033[32m▸ Fetching Alpine armv7 musl/busybox sysroot\033[0m\n"
	@mkdir -p $(BUILD_DIR)/sysroot-armv7
	@curl -fsSL https://dl-cdn.alpinelinux.org/alpine/v3.21/releases/armv7/alpine-minirootfs-3.21.8-armv7.tar.gz \
		| tar -xz -C $(BUILD_DIR)/sysroot-armv7

$(BUILD_DIR)/sysroot-armv7/usr/bin/gcc $(BUILD_DIR)/sysroot-glibc-armhf/lib/ld-linux-armhf.so.3: $(BUILD_DIR)/sysroot-armv7/bin/busybox tools/setup_m7_sysroot.py
	@printf "\033[32m▸ Populating Alpine armv7 & Debian bookworm glibc 2.36 sysroots\033[0m\n"
	@python3 tools/setup_m7_sysroot.py $(BUILD_DIR)/apk-cache $(BUILD_DIR)/sysroot-armv7 --with-gcc --glibc-sysroot $(BUILD_DIR)/sysroot-glibc-armhf
	@touch $(BUILD_DIR)/sysroot-armv7/usr/bin/gcc

define BUILD_ARM32_MUSL_FIXTURE
$(BUILD_DIR)/$(1): tests/$(2) $(BUILD_DIR)/sysroot-armv7/usr/bin/gcc | $(BUILD_DIR)
	@printf "\033[32m▸ Building\033[0m $$@ ($(3), musl dynamic)\n"
	$(CLANG_RAW) --target=armv7a-unknown-linux-musleabihf -march=armv7ve -mfpu=neon-vfpv4 \
		-mfloat-abi=hard $(3) -std=c11 -Werror=vla -Wall -Wextra -O2 -fPIE \
		-isystem $(SYSROOT_ARMV7)/usr/include -c $$< -o $(BUILD_DIR)/$(1).o
	$(LLD_RAW) -m armelf_linux_eabi -pie -dynamic-linker /lib/ld-musl-armhf.so.1 \
		$(SYSROOT_ARMV7)/usr/lib/Scrt1.o $(SYSROOT_ARMV7)/usr/lib/crti.o \
		$(SYSROOT_ARMV7)/usr/lib/gcc/armv7-alpine-linux-musleabihf/14.2.0/crtbeginS.o \
		$(BUILD_DIR)/$(1).o -L$(SYSROOT_ARMV7)/usr/lib -L$(SYSROOT_ARMV7)/lib -lc \
		$(SYSROOT_ARMV7)/usr/lib/gcc/armv7-alpine-linux-musleabihf/14.2.0/libgcc.a \
		$(SYSROOT_ARMV7)/usr/lib/gcc/armv7-alpine-linux-musleabihf/14.2.0/crtendS.o \
		$(SYSROOT_ARMV7)/usr/lib/crtn.o -o $$@
endef

$(eval $(call BUILD_ARM32_MUSL_FIXTURE,test-m7-armhf,test_m7_libc_math.c,-marm))
$(eval $(call BUILD_ARM32_MUSL_FIXTURE,test-m7-thumb,test_m7_libc_math.c,-mthumb))

define BUILD_ARM32_GLIBC_FIXTURE
$(BUILD_DIR)/$(1): tests/$(2) $(BUILD_DIR)/sysroot-glibc-armhf/lib/ld-linux-armhf.so.3 $(BUILD_DIR)/sysroot-armv7/usr/bin/gcc | $(BUILD_DIR)
	@printf "\033[32m▸ Building\033[0m $$@ ($(3), glibc 2.36 dynamic)\n"
	$(CLANG_RAW) --target=armv7a-unknown-linux-gnueabihf -march=armv7ve -mfpu=neon-vfpv4 \
		-mfloat-abi=hard $(3) -std=c11 -Werror=vla -pedantic -Wall -Wextra -O2 -fPIE \
		-isystem $(SYSROOT_GLIBC)/usr/include/arm-linux-gnueabihf -isystem $(SYSROOT_GLIBC)/usr/include \
		-c $$< -o $(BUILD_DIR)/$(1).o
	$(LLD_RAW) -m armelf_linux_eabi -pie --sysroot=$(SYSROOT_GLIBC) --dynamic-linker=/lib/ld-linux-armhf.so.3 \
		$(SYSROOT_GLIBC)/usr/lib/arm-linux-gnueabihf/Scrt1.o $(SYSROOT_GLIBC)/usr/lib/arm-linux-gnueabihf/crti.o \
		$(SYSROOT_ARMV7)/usr/lib/gcc/armv7-alpine-linux-musleabihf/14.2.0/crtbeginS.o \
		$(BUILD_DIR)/$(1).o -L$(SYSROOT_GLIBC)/usr/lib/arm-linux-gnueabihf -L$(SYSROOT_GLIBC)/lib/arm-linux-gnueabihf \
		-lc -lm $(SYSROOT_GLIBC)/lib/arm-linux-gnueabihf/libpthread.so.0 $(SYSROOT_GLIBC)/lib/arm-linux-gnueabihf/libdl.so.2 \
		$(SYSROOT_ARMV7)/usr/lib/gcc/armv7-alpine-linux-musleabihf/14.2.0/libgcc.a \
		$(SYSROOT_ARMV7)/usr/lib/gcc/armv7-alpine-linux-musleabihf/14.2.0/crtendS.o \
		$(SYSROOT_GLIBC)/usr/lib/arm-linux-gnueabihf/crtn.o -o $$@
endef

$(eval $(call BUILD_ARM32_GLIBC_FIXTURE,test-m8-glibc-armhf,test_m8_glibc.c,-marm))
$(eval $(call BUILD_ARM32_GLIBC_FIXTURE,test-m8-glibc-thumb,test_m8_glibc.c,-mthumb))

define BUILD_ARM32_CPP_FIXTURE
$(BUILD_DIR)/$(1): tests/$(2) $(BUILD_DIR)/sysroot-armv7/usr/bin/gcc | $(BUILD_DIR)
	@printf "\033[32m▸ Building\033[0m $$@ ($(3), C++17 & ARM EHABI)\n"
	$(CLANGXX_RAW) --target=armv7a-unknown-linux-musleabihf -march=armv7ve -mfpu=neon-vfpv4 \
		-mfloat-abi=hard $(3) -std=c++17 -Wall -Wextra -O2 -fPIE -fexceptions -frtti \
		-isystem $(SYSROOT_ARMV7)/usr/include/c++/14.2.0 \
		-isystem $(SYSROOT_ARMV7)/usr/include/c++/14.2.0/armv7-alpine-linux-musleabihf \
		-isystem $(SYSROOT_ARMV7)/usr/include -c $$< -o $(BUILD_DIR)/$(1).o
	$(LLD_RAW) -m armelf_linux_eabi -pie -dynamic-linker /lib/ld-musl-armhf.so.1 \
		$(SYSROOT_ARMV7)/usr/lib/Scrt1.o $(SYSROOT_ARMV7)/usr/lib/crti.o \
		$(SYSROOT_ARMV7)/usr/lib/gcc/armv7-alpine-linux-musleabihf/14.2.0/crtbeginS.o \
		$(BUILD_DIR)/$(1).o -L$(SYSROOT_ARMV7)/usr/lib/gcc/armv7-alpine-linux-musleabihf/14.2.0 \
		-L$(SYSROOT_ARMV7)/usr/lib -L$(SYSROOT_ARMV7)/lib -lstdc++ -lm -lgcc_s -lc -lgcc \
		$(SYSROOT_ARMV7)/usr/lib/gcc/armv7-alpine-linux-musleabihf/14.2.0/crtendS.o \
		$(SYSROOT_ARMV7)/usr/lib/crtn.o -o $$@
endef

$(eval $(call BUILD_ARM32_CPP_FIXTURE,test-m8-cpp-armhf,test_m8_cpp_ehabi.cpp,-marm))
$(eval $(call BUILD_ARM32_CPP_FIXTURE,test-m8-cpp-thumb,test_m8_cpp_ehabi.cpp,-mthumb))

check-headers: ## Verify include/a32host.h matches $(HL_DIR)/src/a32host.h
	@cmp -s include/a32host.h $(HL_DIR)/src/a32host.h || (echo "ERROR: include/a32host.h out of sync with $(HL_DIR)/src/a32host.h" && exit 1)
	@echo "=== check-headers: PASS ==="

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
	@$(TIMEOUT_RUN) $(HL_BIN) $(abspath $(A32RT_BIN)) --cache $(abspath $(BUILD_DIR)) --aot $(abspath $(BUILD_DIR)/bench-coremark-armhf)
	@A32RT_STATS=1 $(TIMEOUT_RUN) $(HL_BIN) $(abspath $(A32RT_BIN)) --cache $(abspath $(BUILD_DIR)) $(abspath $(BUILD_DIR)/bench-coremark-armhf)
	@A32RT_STATS=1 $(TIMEOUT_RUN) $(HL_BIN) $(abspath $(A32RT_BIN)) --cache $(abspath $(BUILD_DIR)) $(abspath $(BUILD_DIR)/bench-coremark-thumb)
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

test-m5-hl: $(A32RT_BIN) $(BUILD_DIR)/test-m5-armhf $(BUILD_DIR)/hello-armhf-static ## Run M5 LTP syscall conformance and GDB RSP verification under hl
	@printf "\033[32m▸ Verifying M5-hl (LTP syscall conformance + compat shims)\033[0m under $(HL_BIN)\n"
	@$(TIMEOUT_RUN) $(HL_BIN) $(abspath $(A32RT_BIN)) $(abspath $(BUILD_DIR)/test-m5-armhf)
	@$(TIMEOUT_RUN) python3 -c 'import socket, subprocess, time; p = subprocess.Popen(["$(HL_BIN)", "$(abspath $(A32RT_BIN))", "--gdb", "14322", "$(abspath $(BUILD_DIR)/hello-armhf-static)"], stdout=subprocess.PIPE, stderr=subprocess.PIPE); exec("for _ in range(50):\n try:\n  s = socket.create_connection((\"127.0.0.1\", 14322), timeout=3); break\n except OSError:\n  time.sleep(0.05)"); rpkt = lambda: (lambda b=b"": [b := b + s.recv(1024) for _ in range(16) if b"#" not in b or len(b.split(b"#")[-1]) < 2] and b.decode("latin1"))(); send = lambda cmd: (s.sendall(b"$$" + cmd.encode("latin1") + b"#" + ("%02x" % (sum(cmd.encode("latin1")) & 0xff)).encode("latin1")), rpkt())[1]; r1 = send("?"); r2 = send("g"); r_sup = send("qSupported"); assert "qXfer:features:read+" in r_sup, r_sup; r_xml_part = send("qXfer:features:read:target.xml:0,40"); assert "$$m<?xml" in r_xml_part, r_xml_part; r_xml = send("qXfer:features:read:target.xml:0,fff"); assert "$$l" in r_xml and "org.gnu.gdb.arm.vfp" in r_xml, r_xml; r_xml_eof = send("qXfer:features:read:target.xml:ffff,40"); assert "$$l#" in r_xml_eof, r_xml_eof; assert "OK" in send("P1a=0123456789abcdef"); assert "0123456789abcdef" in send("p1a"); assert "OK" in send("P3a=12345678"); assert "12345678" in send("p3a"); r3 = send("s"); send("c"); out, _ = p.communicate(timeout=3); assert "S05" in r1 and len(r2) >= 128 and "S05" in r3 and out == b"hello\n", (r1, r2, r3, out); print("M5-hl GDB RSP Stub: PASS")'
	@printf "\033[32mPASS\033[0m: M5-hl\n"

test-m5-lima: $(A32RT_BIN) $(BUILD_DIR)/test-m5-armhf $(BUILD_DIR)/hello-armhf-static ## Run M5 LTP syscall conformance and GDB RSP verification inside Lima VM
	@printf "\033[32m▸ Verifying M5 (LTP syscall conformance + GDB RSP)\033[0m inside Lima VM ($(LIMA_VM))\n"
	@$(TIMEOUT_RUN) $(LIMACTL) shell --workdir $(abspath .) $(LIMA_VM) -- $(abspath $(A32RT_BIN)) $(abspath $(BUILD_DIR)/test-m5-armhf)
	@$(TIMEOUT_RUN) $(LIMACTL) shell --workdir $(abspath .) $(LIMA_VM) -- python3 -c 'import socket, subprocess, time; p = subprocess.Popen(["./_build/a32rt", "--gdb", "14321", "./_build/hello-armhf-static"], stdout=subprocess.PIPE, stderr=subprocess.PIPE); exec("for _ in range(50):\n try:\n  s = socket.create_connection((\"127.0.0.1\", 14321), timeout=3); break\n except OSError:\n  time.sleep(0.05)"); rpkt = lambda: (lambda b=b"": [b := b + s.recv(1024) for _ in range(16) if b"#" not in b or len(b.split(b"#")[-1]) < 2] and b.decode("latin1"))(); send = lambda cmd: (s.sendall(b"$$" + cmd.encode("latin1") + b"#" + ("%02x" % (sum(cmd.encode("latin1")) & 0xff)).encode("latin1")), rpkt())[1]; r1 = send("?"); r2 = send("g"); r_sup = send("qSupported"); assert "qXfer:features:read+" in r_sup, r_sup; r_xml_part = send("qXfer:features:read:target.xml:0,40"); assert "$$m<?xml" in r_xml_part, r_xml_part; r_xml = send("qXfer:features:read:target.xml:0,fff"); assert "$$l" in r_xml and "org.gnu.gdb.arm.vfp" in r_xml, r_xml; r_xml_eof = send("qXfer:features:read:target.xml:ffff,40"); assert "$$l#" in r_xml_eof, r_xml_eof; assert "OK" in send("P1a=0123456789abcdef"); assert "0123456789abcdef" in send("p1a"); assert "OK" in send("P3a=12345678"); assert "12345678" in send("p3a"); r3 = send("s"); send("c"); out, _ = p.communicate(timeout=3); assert "S05" in r1 and len(r2) >= 128 and "S05" in r3 and out == b"hello\n", (r1, r2, r3, out); print("M5 GDB RSP Stub: PASS")'
	@printf "\033[32mPASS\033[0m: M5 on Lima\n"

test-m6-hl: $(A32RT_BIN) $(BUILD_DIR)/sysroot-armv7/bin/busybox ## Run M6 real-world multi-library dynamic linking & relocatable .so AOT cache under hl
	@printf "\033[32m▸ Verifying M6-hl (apk, scanelf, getconf dynamic linking)\033[0m under $(HL_BIN)\n"
	@out="$$($(TIMEOUT_RUN) $(HL_BIN) $(abspath $(A32RT_BIN)) --sysroot $(SYSROOT_ARMV7) $(SYSROOT_ARMV7)/sbin/apk --version)"; \
	echo "$$out" | grep -q "apk-tools" || { printf "\033[31mFAIL\033[0m: apk --version failed:\n%s\n" "$$out"; exit 1; }; \
	printf "M6-hl apk --version: PASS (%s)\n" "$$out"
	@out="$$($(TIMEOUT_RUN) $(HL_BIN) $(abspath $(A32RT_BIN)) --sysroot $(SYSROOT_ARMV7) $(SYSROOT_ARMV7)/usr/bin/scanelf -V)"; \
	echo "$$out" | grep -q "scanelf" || { printf "\033[31mFAIL\033[0m: scanelf -V failed:\n%s\n" "$$out"; exit 1; }; \
	printf "M6-hl scanelf -V: PASS (%s)\n" "$$out"
	@out="$$($(TIMEOUT_RUN) $(HL_BIN) $(abspath $(A32RT_BIN)) --sysroot $(SYSROOT_ARMV7) $(SYSROOT_ARMV7)/usr/bin/getconf LONG_BIT)"; \
	[ "$$out" = "32" ] || { printf "\033[31mFAIL\033[0m: getconf LONG_BIT failed: expected 32, got '%s'\n" "$$out"; exit 1; }; \
	printf "M6-hl getconf LONG_BIT: PASS (32)\n"
	@printf "\033[32m▸ Verifying M6-hl (Relocatable Shared-Library .so AOT Cache)\033[0m\n"
	@rm -f $(BUILD_DIR)/a32rt-*cache-*.bin
	@$(TIMEOUT_RUN) $(HL_BIN) $(abspath $(A32RT_BIN)) --cache $(abspath $(BUILD_DIR)) --aot $(SYSROOT_ARMV7)/usr/lib/libz.so.1.3.2
	@$(TIMEOUT_RUN) $(HL_BIN) $(abspath $(A32RT_BIN)) --cache $(abspath $(BUILD_DIR)) --aot --sysroot $(SYSROOT_ARMV7) $(SYSROOT_ARMV7)/sbin/apk
	@A32RT_STATS=1 $(TIMEOUT_RUN) $(HL_BIN) $(abspath $(A32RT_BIN)) --cache $(abspath $(BUILD_DIR)) --stats --sysroot $(SYSROOT_ARMV7) $(SYSROOT_ARMV7)/sbin/apk --version
	@rm -f $(BUILD_DIR)/a32rt-*cache-*.bin
	@printf "\033[32mPASS\033[0m: M6-hl\n"

test-m6-lima: $(A32RT_BIN) $(BUILD_DIR)/sysroot-armv7/bin/busybox ## Run M6 real-world multi-library dynamic linking & relocatable .so AOT cache inside Lima VM
	@printf "\033[32m▸ Verifying M6 (apk, scanelf, getconf dynamic linking)\033[0m inside Lima VM ($(LIMA_VM))\n"
	@out="$$($(TIMEOUT_RUN) $(LIMACTL) shell --workdir $(abspath .) $(LIMA_VM) -- $(abspath $(A32RT_BIN)) --sysroot $(SYSROOT_ARMV7) $(SYSROOT_ARMV7)/sbin/apk --version)"; \
	echo "$$out" | grep -q "apk-tools" || { printf "\033[31mFAIL\033[0m: apk --version failed:\n%s\n" "$$out"; exit 1; }; \
	printf "M6 apk --version: PASS (%s)\n" "$$out"
	@out="$$($(TIMEOUT_RUN) $(LIMACTL) shell --workdir $(abspath .) $(LIMA_VM) -- $(abspath $(A32RT_BIN)) --sysroot $(SYSROOT_ARMV7) $(SYSROOT_ARMV7)/usr/bin/scanelf -V)"; \
	echo "$$out" | grep -q "scanelf" || { printf "\033[31mFAIL\033[0m: scanelf -V failed:\n%s\n" "$$out"; exit 1; }; \
	printf "M6 scanelf -V: PASS (%s)\n" "$$out"
	@out="$$($(TIMEOUT_RUN) $(LIMACTL) shell --workdir $(abspath .) $(LIMA_VM) -- $(abspath $(A32RT_BIN)) --sysroot $(SYSROOT_ARMV7) $(SYSROOT_ARMV7)/usr/bin/getconf LONG_BIT)"; \
	[ "$$out" = "32" ] || { printf "\033[31mFAIL\033[0m: getconf LONG_BIT failed: expected 32, got '%s'\n" "$$out"; exit 1; }; \
	printf "M6 getconf LONG_BIT: PASS (32)\n"
	@printf "\033[32m▸ Verifying M6 (Relocatable Shared-Library .so AOT Cache)\033[0m inside Lima VM ($(LIMA_VM))\n"
	@rm -f $(BUILD_DIR)/a32rt-*cache-*.bin
	@$(TIMEOUT_RUN) $(LIMACTL) shell --workdir $(abspath .) $(LIMA_VM) -- $(abspath $(A32RT_BIN)) --cache $(abspath $(BUILD_DIR)) --aot $(SYSROOT_ARMV7)/usr/lib/libz.so.1.3.2
	@$(TIMEOUT_RUN) $(LIMACTL) shell --workdir $(abspath .) $(LIMA_VM) -- $(abspath $(A32RT_BIN)) --cache $(abspath $(BUILD_DIR)) --aot --sysroot $(SYSROOT_ARMV7) $(SYSROOT_ARMV7)/sbin/apk
	@$(TIMEOUT_RUN) $(LIMACTL) shell --workdir $(abspath .) $(LIMA_VM) -- $(abspath $(A32RT_BIN)) --cache $(abspath $(BUILD_DIR)) --stats --sysroot $(SYSROOT_ARMV7) $(SYSROOT_ARMV7)/sbin/apk --version
	@rm -f $(BUILD_DIR)/a32rt-*cache-*.bin
	@printf "\033[32mPASS\033[0m: M6 on Lima\n"

test-m7-hl: $(A32RT_BIN) $(BUILD_DIR)/test-m7-armhf $(BUILD_DIR)/test-m7-thumb $(BUILD_DIR)/sysroot-armv7/usr/bin/gcc tests/test_m7_ecosystem.sh ## Run M7 libc-math conformance & Alpine armv7 application ecosystem (incl. self-hosted gcc) under hl
	@printf "\033[32m▸ Verifying M7-hl (libc/math conformance A32 + Thumb-2)\033[0m under $(HL_BIN)\n"
	@$(TIMEOUT_RUN) env HL_A32RT=$(abspath $(A32RT_BIN)) A32RT_SYSROOT=$(SYSROOT_ARMV7) $(HL_BIN) --a32rt $(abspath $(A32RT_BIN)) -- $(abspath $(BUILD_DIR)/test-m7-armhf)
	@$(TIMEOUT_RUN) env HL_A32RT=$(abspath $(A32RT_BIN)) A32RT_SYSROOT=$(SYSROOT_ARMV7) $(HL_BIN) --a32rt $(abspath $(A32RT_BIN)) -- $(abspath $(BUILD_DIR)/test-m7-thumb)
	@printf "\033[32m▸ Verifying M7-hl (Alpine armv7 ecosystem + LuaJIT + Python 3.12 + self-hosted GCC)\033[0m under $(HL_BIN)\n"
	@cp tests/test_m7_ecosystem.sh /tmp/test_m7_ecosystem.sh
	@perl -e 'alarm 180; exec @ARGV' env HL_A32RT=$(abspath $(A32RT_BIN)) A32RT_SYSROOT=$(SYSROOT_ARMV7) $(HL_BIN) --a32rt $(abspath $(A32RT_BIN)) -- $(SYSROOT_ARMV7)/bin/bash /tmp/test_m7_ecosystem.sh
	@rm -f /tmp/test_m7_ecosystem.sh
	@printf "\033[32mPASS\033[0m: M7-hl\n"

test-m7-lima: $(A32RT_BIN) $(BUILD_DIR)/test-m7-armhf $(BUILD_DIR)/test-m7-thumb $(BUILD_DIR)/sysroot-armv7/usr/bin/gcc tests/test_m7_ecosystem.sh ## Run M7 libc-math conformance & Alpine armv7 application ecosystem inside Lima VM
	@printf "\033[32m▸ Verifying M7 (libc/math conformance A32 + Thumb-2)\033[0m inside Lima VM ($(LIMA_VM))\n"
	@$(TIMEOUT_RUN) $(LIMACTL) shell --workdir $(abspath .) $(LIMA_VM) -- $(abspath $(A32RT_BIN)) --sysroot $(SYSROOT_ARMV7) $(abspath $(BUILD_DIR)/test-m7-armhf)
	@$(TIMEOUT_RUN) $(LIMACTL) shell --workdir $(abspath .) $(LIMA_VM) -- $(abspath $(A32RT_BIN)) --sysroot $(SYSROOT_ARMV7) $(abspath $(BUILD_DIR)/test-m7-thumb)
	@printf "\033[32m▸ Verifying M7 (Alpine armv7 ecosystem + LuaJIT + Python 3.12 + self-hosted GCC)\033[0m inside Lima VM ($(LIMA_VM))\n"
	@perl -e 'alarm 180; exec @ARGV' $(LIMACTL) shell --workdir $(abspath .) $(LIMA_VM) -- sh -c 'cp tests/test_m7_ecosystem.sh /tmp/test_m7_ecosystem.sh && $(abspath $(A32RT_BIN)) --sysroot $(SYSROOT_ARMV7) $(SYSROOT_ARMV7)/bin/bash /tmp/test_m7_ecosystem.sh && rm -f /tmp/test_m7_ecosystem.sh'
	@printf "\033[32mPASS\033[0m: M7 on Lima\n"

test-m8-hl: $(A32RT_BIN) $(BUILD_DIR)/hello-armhf-static $(BUILD_DIR)/test-m8-glibc-armhf $(BUILD_DIR)/test-m8-glibc-thumb $(BUILD_DIR)/test-m8-cpp-armhf $(BUILD_DIR)/test-m8-cpp-thumb $(BUILD_DIR)/sysroot-glibc-armhf/lib/ld-linux-armhf.so.3 $(BUILD_DIR)/sysroot-armv7/usr/bin/gcc ## Run M8 Debian bookworm glibc 2.36, C++17 ARM EHABI, self-hosted g++ & --auto-aot under hl
	@printf "\033[32m▸ Verifying M8-hl (Debian bookworm glibc 2.36 A32 + Thumb-2 + dash)\033[0m under $(HL_BIN)\n"
	@$(TIMEOUT_RUN) $(HL_BIN) $(abspath $(A32RT_BIN)) --sysroot $(SYSROOT_GLIBC) $(abspath $(BUILD_DIR)/test-m8-glibc-armhf)
	@$(TIMEOUT_RUN) $(HL_BIN) $(abspath $(A32RT_BIN)) --sysroot $(SYSROOT_GLIBC) $(abspath $(BUILD_DIR)/test-m8-glibc-thumb)
	@out="$$($(TIMEOUT_RUN) $(HL_BIN) $(abspath $(A32RT_BIN)) --sysroot $(SYSROOT_GLIBC) $(SYSROOT_GLIBC)/bin/dash -c 'echo GLIBC_DASH_OK')"; \
	[ "$$out" = "GLIBC_DASH_OK" ] || { printf "\033[31mFAIL\033[0m: glibc dash failed: %s\n" "$$out"; exit 1; }; \
	printf "M8-hl Debian armhf glibc 2.36 /bin/dash: PASS (%s)\n" "$$out"
	@printf "\033[32m▸ Verifying M8-hl (C++17 & ARM EHABI .ARM.exidx unwinding A32 + Thumb-2 + self-hosted g++ 14.2.0)\033[0m under $(HL_BIN)\n"
	@$(TIMEOUT_RUN) $(HL_BIN) $(abspath $(A32RT_BIN)) --sysroot $(SYSROOT_ARMV7) $(abspath $(BUILD_DIR)/test-m8-cpp-armhf)
	@$(TIMEOUT_RUN) $(HL_BIN) $(abspath $(A32RT_BIN)) --sysroot $(SYSROOT_ARMV7) $(abspath $(BUILD_DIR)/test-m8-cpp-thumb)
	@cp tests/test_m8_cpp_ehabi.cpp /tmp/test_m8_cpp_ehabi.cpp
	@perl -e 'alarm 120; exec @ARGV' env PATH="/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin" HL_A32RT=$(abspath $(A32RT_BIN)) A32RT_SYSROOT=$(SYSROOT_ARMV7) $(HL_BIN) --a32rt $(abspath $(A32RT_BIN)) -- $(SYSROOT_ARMV7)/usr/bin/g++ -O1 -std=c++17 -DA32RT_SELFHOST_FAST /tmp/test_m8_cpp_ehabi.cpp -o /tmp/test_m8_cpp_ehabi_self
	@$(TIMEOUT_RUN) $(HL_BIN) $(abspath $(A32RT_BIN)) --sysroot $(SYSROOT_ARMV7) /tmp/test_m8_cpp_ehabi_self
	@rm -f /tmp/test_m8_cpp_ehabi.cpp /tmp/test_m8_cpp_ehabi_self
	@printf "\033[32m▸ Verifying M8-hl (--auto-aot transparent cache persistence)\033[0m under $(HL_BIN)\n"
	@rm -f $(BUILD_DIR)/a32rt-*cache-*.bin
	@$(TIMEOUT_RUN) $(HL_BIN) $(abspath $(A32RT_BIN)) --cache $(abspath $(BUILD_DIR)) --auto-aot --stats $(abspath $(BUILD_DIR)/hello-armhf-static) >/dev/null 2>&1
	@out2="$$($(TIMEOUT_RUN) $(HL_BIN) $(abspath $(A32RT_BIN)) --cache $(abspath $(BUILD_DIR)) --auto-aot --stats $(abspath $(BUILD_DIR)/hello-armhf-static) 2>&1)"; \
	rm -f $(BUILD_DIR)/a32rt-*cache-*.bin; \
	echo "$$out2" | grep -q "blocks_compiled=0" || { printf "\033[31mFAIL\033[0m: expected blocks_compiled=0 on second --auto-aot run:\n%s\n" "$$out2"; exit 1; }; \
	printf "M8-hl --auto-aot (blocks_compiled=0 on warm run): PASS\n"
	@printf "\033[32mPASS\033[0m: M8-hl\n"

test-m8-lima: $(A32RT_BIN) $(BUILD_DIR)/hello-armhf-static $(BUILD_DIR)/test-m8-glibc-armhf $(BUILD_DIR)/test-m8-glibc-thumb $(BUILD_DIR)/test-m8-cpp-armhf $(BUILD_DIR)/test-m8-cpp-thumb $(BUILD_DIR)/sysroot-glibc-armhf/lib/ld-linux-armhf.so.3 $(BUILD_DIR)/sysroot-armv7/usr/bin/gcc ## Run M8 Debian bookworm glibc 2.36, C++17 ARM EHABI, self-hosted g++ & --auto-aot inside Lima VM
	@printf "\033[32m▸ Verifying M8 (Debian bookworm glibc 2.36 A32 + Thumb-2 + dash)\033[0m inside Lima VM ($(LIMA_VM))\n"
	@$(TIMEOUT_RUN) $(LIMACTL) shell --workdir $(abspath .) $(LIMA_VM) -- $(abspath $(A32RT_BIN)) --sysroot $(SYSROOT_GLIBC) $(abspath $(BUILD_DIR)/test-m8-glibc-armhf)
	@$(TIMEOUT_RUN) $(LIMACTL) shell --workdir $(abspath .) $(LIMA_VM) -- $(abspath $(A32RT_BIN)) --sysroot $(SYSROOT_GLIBC) $(abspath $(BUILD_DIR)/test-m8-glibc-thumb)
	@out="$$($(TIMEOUT_RUN) $(LIMACTL) shell --workdir $(abspath .) $(LIMA_VM) -- $(abspath $(A32RT_BIN)) --sysroot $(SYSROOT_GLIBC) $(SYSROOT_GLIBC)/bin/dash -c 'echo GLIBC_DASH_OK')"; \
	[ "$$out" = "GLIBC_DASH_OK" ] || { printf "\033[31mFAIL\033[0m: glibc dash failed: %s\n" "$$out"; exit 1; }; \
	printf "M8 Debian armhf glibc 2.36 /bin/dash: PASS (%s)\n" "$$out"
	@printf "\033[32m▸ Verifying M8 (C++17 & ARM EHABI .ARM.exidx unwinding A32 + Thumb-2 + self-hosted g++ 14.2.0)\033[0m inside Lima VM ($(LIMA_VM))\n"
	@$(TIMEOUT_RUN) $(LIMACTL) shell --workdir $(abspath .) $(LIMA_VM) -- $(abspath $(A32RT_BIN)) --sysroot $(SYSROOT_ARMV7) $(abspath $(BUILD_DIR)/test-m8-cpp-armhf)
	@$(TIMEOUT_RUN) $(LIMACTL) shell --workdir $(abspath .) $(LIMA_VM) -- $(abspath $(A32RT_BIN)) --sysroot $(SYSROOT_ARMV7) $(abspath $(BUILD_DIR)/test-m8-cpp-thumb)
	@perl -e 'alarm 120; exec @ARGV' $(LIMACTL) shell --workdir $(abspath .) $(LIMA_VM) -- sh -c 'cp tests/test_m8_cpp_ehabi.cpp /tmp/test_m8_cpp_ehabi.cpp && env PATH="/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin" $(abspath $(A32RT_BIN)) --sysroot $(SYSROOT_ARMV7) $(SYSROOT_ARMV7)/usr/bin/g++ -O1 -std=c++17 -DA32RT_SELFHOST_FAST /tmp/test_m8_cpp_ehabi.cpp -o /tmp/test_m8_cpp_ehabi_self && $(abspath $(A32RT_BIN)) --sysroot $(SYSROOT_ARMV7) /tmp/test_m8_cpp_ehabi_self && rm -f /tmp/test_m8_cpp_ehabi.cpp /tmp/test_m8_cpp_ehabi_self'
	@printf "\033[32m▸ Verifying M8 (--auto-aot transparent cache persistence)\033[0m inside Lima VM ($(LIMA_VM))\n"
	@rm -f $(BUILD_DIR)/a32rt-*cache-*.bin
	@$(TIMEOUT_RUN) $(LIMACTL) shell --workdir $(abspath .) $(LIMA_VM) -- $(abspath $(A32RT_BIN)) --cache $(abspath $(BUILD_DIR)) --auto-aot --stats $(abspath $(BUILD_DIR)/hello-armhf-static) >/dev/null 2>&1
	@out2="$$($(TIMEOUT_RUN) $(LIMACTL) shell --workdir $(abspath .) $(LIMA_VM) -- $(abspath $(A32RT_BIN)) --cache $(abspath $(BUILD_DIR)) --auto-aot --stats $(abspath $(BUILD_DIR)/hello-armhf-static) 2>&1)"; \
	rm -f $(BUILD_DIR)/a32rt-*cache-*.bin; \
	echo "$$out2" | grep -q "blocks_compiled=0" || { printf "\033[31mFAIL\033[0m: expected blocks_compiled=0 on second --auto-aot run:\n%s\n" "$$out2"; exit 1; }; \
	printf "M8 --auto-aot (blocks_compiled=0 on warm run): PASS\n"
	@printf "\033[32mPASS\033[0m: M8 on Lima\n"

test-all-hl: check-headers test-hello-hl test-m1-hl test-m2-hl test-m3-hl test-m4-hl test-m5-hl test-m6-hl test-m7-hl test-m8-hl ## Run complete M0..M8 suite under hyper-linux

test-all-lima: test-hello-lima test-m1-lima test-m2-lima test-m3-lima test-m4-lima test-m5-lima test-m6-lima test-m7-lima test-m8-lima ## Run complete M0..M8 suite inside Lima VM

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
	@$(TIMEOUT_RUN) $(HL_BIN) $(abspath $(A32RT_BIN)) --cache $(abspath $(BUILD_DIR)) --stats $(abspath $(BUILD_DIR)/bench-suite-armhf)
	@printf "\033[36m=== [hyper-linux] 3. a32rt A32 (Warm .a32cache AOT, G=4GiB) ===\033[0m\n"
	@$(TIMEOUT_RUN) $(HL_BIN) $(abspath $(A32RT_BIN)) --cache $(abspath $(BUILD_DIR)) --aot $(abspath $(BUILD_DIR)/bench-suite-armhf)
	@$(TIMEOUT_RUN) $(HL_BIN) $(abspath $(A32RT_BIN)) --cache $(abspath $(BUILD_DIR)) --stats $(abspath $(BUILD_DIR)/bench-suite-armhf)
	@printf "\033[36m=== [hyper-linux] 4. a32rt Thumb-2 (Cold JIT, G=4GiB) ===\033[0m\n"
	@rm -f $(BUILD_DIR)/a32rt-*cache-*.bin
	@$(TIMEOUT_RUN) $(HL_BIN) $(abspath $(A32RT_BIN)) --cache $(abspath $(BUILD_DIR)) --stats $(abspath $(BUILD_DIR)/bench-suite-thumb)
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
	@rm -f $(BUILD_DIR)/a32rt-*cache-*.bin
	@$(TIMEOUT_RUN) $(LIMACTL) shell --workdir $(abspath .) $(LIMA_VM) -- $(abspath $(A32RT_BIN)) --cache $(abspath $(BUILD_DIR)) --aot $(abspath $(BUILD_DIR)/bench-suite-armhf)
	@$(TIMEOUT_RUN) $(LIMACTL) shell --workdir $(abspath .) $(LIMA_VM) -- $(abspath $(A32RT_BIN)) --cache $(abspath $(BUILD_DIR)) --stats $(abspath $(BUILD_DIR)/bench-suite-armhf)
	@printf "\033[36m=== [Lima Linux VM] 3. a32rt Thumb-2 (Warm .a32cache AOT, G=0) ===\033[0m\n"
	@rm -f $(BUILD_DIR)/a32rt-*cache-*.bin
	@$(TIMEOUT_RUN) $(LIMACTL) shell --workdir $(abspath .) $(LIMA_VM) -- $(abspath $(A32RT_BIN)) --cache $(abspath $(BUILD_DIR)) --aot $(abspath $(BUILD_DIR)/bench-suite-thumb)
	@$(TIMEOUT_RUN) $(LIMACTL) shell --workdir $(abspath .) $(LIMA_VM) -- $(abspath $(A32RT_BIN)) --cache $(abspath $(BUILD_DIR)) --stats $(abspath $(BUILD_DIR)/bench-suite-thumb)
	@rm -f $(BUILD_DIR)/a32rt-*cache-*.bin
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
