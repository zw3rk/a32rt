{
  description = "a32rt — Rosetta-style AArch32 (arm32/armhf) to AArch64 Linux user-mode binary translator";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      darwinSystem = "aarch64-darwin";
      darwinPkgs   = nixpkgs.legacyPackages.${darwinSystem};
      linuxSystem  = "x86_64-linux";
      linuxPkgs    = nixpkgs.legacyPackages.${linuxSystem};
      aarch64LinuxPkgs = nixpkgs.legacyPackages.aarch64-linux;

      # Cross-compilers for aarch64-unknown-linux-musl (a32rt itself)
      darwinA64Musl = darwinPkgs.pkgsCross.aarch64-multiplatform-musl;
      linuxA64Musl  = linuxPkgs.pkgsCross.aarch64-multiplatform-musl;

      # Cross-compilers for armv7l-unknown-linux-musleabihf (32-bit ARM guest fixtures)
      armv7lMuslPkgs = import nixpkgs {
        localSystem = linuxSystem;
        crossSystem = { config = "armv7l-unknown-linux-musleabihf"; };
      };

      mkA32rt = crossPkgs: crossPkgs.stdenv.mkDerivation {
        pname = "a32rt";
        version = "0.1.0";
        src = ./.;

        dontStrip = true;

        buildPhase = ''
          make build CC="${crossPkgs.stdenv.cc.targetPrefix}gcc"
        '';

        installPhase = ''
          mkdir -p $out/bin $out/include
          cp _build/a32rt $out/bin/a32rt
          ln -sf a32rt $out/bin/a32rt-binfmt-p
          cp include/a32host.h $out/include/a32host.h
        '';

        meta = with crossPkgs.lib; {
          description = "Standalone static-PIE AArch32 -> AArch64 Linux user-mode translator";
          mainProgram = "a32rt";
          platforms = [ "aarch64-linux" ];
          license = licenses.asl20;
        };
      };

      mkA32rtTests = armPkgs: armPkgs.stdenv.mkDerivation {
        pname = "a32rt-tests";
        version = "0.1.0";
        src = ./.;

        buildPhase = ''
          mkdir -p _build
          CC="${armPkgs.stdenv.cc.targetPrefix}gcc"
          FLAGS="-std=c11 -O2 -static -nostdlib -ffreestanding -fno-stack-protector -DA32RT_FREESTANDING_TEST -march=armv7ve -mfpu=neon-vfpv4 -mfloat-abi=hard"
          $CC $FLAGS -marm   -o _build/hello-armhf-static   tests/hello.c
          $CC $FLAGS -mthumb -o _build/hello-thumb-static   tests/hello.c
          $CC $FLAGS -marm   -o _build/test-m1-armhf        tests/test_m1_isa_fs.c
          $CC $FLAGS -mthumb -o _build/test-m1-thumb        tests/test_m1_isa_fs.c
          $CC $FLAGS -marm   -o _build/bench-coremark-armhf tests/bench_coremark.c
          $CC $FLAGS -mthumb -o _build/bench-coremark-thumb tests/bench_coremark.c
          $CC $FLAGS -marm   -o _build/test-m3-armhf        tests/test_m3_threads_signals.c
          $CC $FLAGS -marm   -o _build/test-m4-armhf        tests/test_m4_vfp_neon.c
          $CC $FLAGS -mthumb -o _build/test-m4-thumb        tests/test_m4_vfp_neon.c
          $CC $FLAGS -marm   -o _build/test-m5-armhf        tests/test_m5_ltp_gdb.c
          $CC $FLAGS -marm   -o _build/bench-suite-armhf    tests/bench_suite.c
          $CC $FLAGS -mthumb -o _build/bench-suite-thumb    tests/bench_suite.c
        '';

        installPhase = ''
          mkdir -p $out/bin
          cp _build/* $out/bin/
        '';
      };

      mkDarwinA32rtTests = darwinPkgs.stdenvNoCC.mkDerivation {
        pname = "a32rt-tests";
        version = "0.1.0";
        src = ./.;
        nativeBuildInputs = [ darwinPkgs.llvmPackages.clang-unwrapped darwinPkgs.lld ];
        dontStrip = true;
        dontFixup = true;

        buildPhase = ''
          mkdir -p _build
          CFLAGS="--target=armv7a-unknown-linux-eabihf -march=armv7ve -mfpu=neon-vfpv4 -mfloat-abi=hard -std=c11 -Werror=vla -pedantic -Wall -Wextra -O2 -ffreestanding -fno-stack-protector -nostdlib -DA32RT_FREESTANDING_TEST"
          build_one() {
            clang $CFLAGS "$3" -c "tests/$2" -o "_build/$1.o"
            ld.lld -m armelf_linux_eabi -Bstatic --no-dynamic-linker --image-base=0x00010000 -e _start "_build/$1.o" -o "_build/$1"
            rm -f "_build/$1.o"
          }
          build_one hello-armhf-static   hello.c                 -marm
          build_one hello-thumb-static   hello.c                 -mthumb
          build_one test-m1-armhf        test_m1_isa_fs.c        -marm
          build_one test-m1-thumb        test_m1_isa_fs.c        -mthumb
          build_one bench-coremark-armhf bench_coremark.c        -marm
          build_one bench-coremark-thumb bench_coremark.c        -mthumb
          build_one test-m3-armhf        test_m3_threads_signals.c -marm
          build_one test-m4-armhf        test_m4_vfp_neon.c      -marm
          build_one test-m4-thumb        test_m4_vfp_neon.c      -mthumb
          build_one test-m5-armhf        test_m5_ltp_gdb.c       -marm
          build_one bench-suite-armhf    bench_suite.c           -marm
          build_one bench-suite-thumb    bench_suite.c           -mthumb
        '';

        installPhase = ''
          mkdir -p $out/bin
          cp _build/* $out/bin/
        '';
      };

      busyboxArmhfStatic = armv7lMuslPkgs.busybox.override {
        enableStatic = true;
      };

    in {
      packages.${darwinSystem} = {
        a32rt       = mkA32rt darwinA64Musl;
        a32rt-tests = mkDarwinA32rtTests;
        default     = self.packages.${darwinSystem}.a32rt;
      };

      checks.${darwinSystem} = {
        a32rt       = self.packages.${darwinSystem}.a32rt;
        a32rt-tests = self.packages.${darwinSystem}.a32rt-tests;
      };

      packages.${linuxSystem} = {
        a32rt                = mkA32rt linuxA64Musl;
        a32rt-tests          = mkA32rtTests armv7lMuslPkgs;
        busybox-armhf-static = busyboxArmhfStatic;
        default              = self.packages.${linuxSystem}.a32rt;
      };

      packages.aarch64-linux = {
        a32rt                = mkA32rt aarch64LinuxPkgs.pkgsMusl;
        a32rt-tests          = mkA32rtTests armv7lMuslPkgs;
        busybox-armhf-static = busyboxArmhfStatic;
        default              = self.packages.aarch64-linux.a32rt;
      };

      devShells.${darwinSystem}.default = darwinPkgs.mkShell {
        name = "a32rt-dev";
        buildInputs = [
          darwinPkgs.gnumake
          darwinPkgs.clang
          darwinPkgs.lld
          darwinPkgs.binutils
          darwinPkgs.python3
          darwinPkgs.lima
          darwinA64Musl.stdenv.cc
        ];
        LIMACTL = "${darwinPkgs.lima}/bin/limactl";
      };
    };
}
