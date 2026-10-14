{ pkgs ? import <nixpkgs> {} }:

let
  llvm18 = pkgs.llvmPackages_18;
  llvm21 = pkgs.llvmPackages_21;
  # Unwrapped Cross Drivers
  clang21u = pkgs.llvmPackages_21.clang-unwrapped;
  clang18u = pkgs.llvmPackages_18.clang-unwrapped;

  # Clang 21 resource dir (builtin headers + compiler-rt, as Ubuntu has them)
  clang21ResourceDir = pkgs.runCommand "eden-clang-21-resource-dir" { } ''
    mkdir -p $out/lib
    ln -s ${clang21u.lib}/lib/clang/21/include $out/include
    ln -s ${pkgs.llvmPackages_21.compiler-rt}/lib/linux $out/lib/linux
  '';

  python = pkgs.python3.withPackages (ps: [
    ps.mako ps.pyyaml ps.ply ps.packaging
    # tools/launcher/bake-loading-text.py (the loading screen's and the overlay's baked font).
    ps.numpy ps.pillow
  ]);

  # Merged LLVM 21 libdir (libLLVM-21.so, libclang.so, libclang-cpp.so in one dir)
  libdirs21 = pkgs.lib.concatMapStringsSep " " (d:
    pkgs.lib.concatMapStringsSep " " (o:
      if o == "out" then "${d}/lib"
      else if d ? "${o}" then "${d.${o}}/lib"
      else "") (d.outputs or [ "out" ]))
    [ llvm21.llvm llvm21.clang llvm21.libclang ];

  shimLib = pkgs.runCommand "eden-llvm21-libdir" { } ''
    mkdir -p $out/lib
    for p in ${libdirs21}; do
      [ -d "$p" ] || continue
      for base in libLLVM-21 libclang libclang-cpp; do
        for f in "$p"/$base.so*; do
          [ -e "$f" ] || continue
          ln -sf "$f" "$out/lib/$(basename "$f")"
          ln -sf "$(basename "$f")" "$out/lib/$base.so"
        done
      done
    done
  '';

  realLlvmCfg21 = "${llvm21.llvm.dev}/bin/llvm-config";
  realLlvmCfg18 = "${llvm18.llvm.dev}/bin/llvm-config";

  # Toolchain Bin Shims
  shimBin = pkgs.runCommand "eden-toolchain-bin" { } ''
    mkdir -p $out/bin
    # Unversioned clang* (Payload SDK driver), reporting clang21ResourceDir
    for tool in clang clang++ clang-cpp; do
      cat > $out/bin/$tool <<CLANGPRINT
#!/bin/sh
for a in "\$@"; do
  case "\$a" in
    --print-resource-dir) echo ${clang21ResourceDir}; exit 0 ;;
  esac
done
exec ${clang21u}/bin/$tool "\$@"
CLANGPRINT
      chmod +x $out/bin/$tool
    done
    # clang-18/++-18: cross calls use the unwrapped driver, host calls wrapped
    for tool in clang clang++; do
      cat > $out/bin/$tool-18 <<CLANGSHIM
#!/bin/sh
for a in "\$@"; do
  case "\$a" in
    # Cross: unwrapped driver with builtin headers from the -lib output
    --target=*|-target=*|--target|-target)
      exec ${clang18u}/bin/$tool -resource-dir ${clang18u.lib}/lib/clang/${pkgs.lib.versions.major clang18u.version} "\$@" ;;
  esac
done
exec ${llvm18.clang}/bin/$tool "\$@"
CLANGSHIM
      chmod +x $out/bin/$tool-18
    done
    ln -sf ${clang21u}/bin/clang      $out/bin/clang-21
    ln -sf ${clang21u}/bin/clang++    $out/bin/clang++-21
    ln -sf ${llvm18.lld}/bin/ld.lld    $out/bin/ld.lld-18
    ln -sf ${llvm21.lld}/bin/ld.lld    $out/bin/ld.lld-21
    # Unversioned ld.lld (Payload SDK resolver); re-add -z nodynamic-undefined-weak for
    # the PS5 eboot link (only caller of --no-dynamic-linker)
    cat > $out/bin/ld.lld <<LDLLDSHIM
#!/bin/sh
extra=
for a in "\$@"; do
  case "\$a" in --no-dynamic-linker) extra="-z nodynamic-undefined-weak" ;; esac
done
exec ${llvm21.lld}/bin/ld.lld \$extra "\$@"
LDLLDSHIM
    chmod +x $out/bin/ld.lld
    for n in llvm-ar llvm-ranlib llvm-nm llvm-readobj llvm-objcopy llvm-objdump llvm-symbolizer; do
      ln -sf ${llvm18.llvm}/bin/$n     $out/bin/$n-18
      ln -sf ${llvm21.llvm}/bin/$n     $out/bin/$n-21
    done
    ln -sf llvm-symbolizer-18          $out/bin/llvm-symbolizer
    ln -sf ${realLlvmCfg18}            $out/bin/llvm-config-18
    ln -sf ${realLlvmCfg21}            $out/bin/llvm-config-21
    # Ubuntu-style llvm-* names the PS5 payload SDK wrappers resolve (LLVM 21)
    ln -sf clang     $out/bin/llvm-clang
    ln -sf clang++   $out/bin/llvm-clang++
    ln -sf clang-cpp $out/bin/llvm-clang-cpp
    ln -sf ${llvm21.lld}/bin/ld.lld      $out/bin/llvm-ld.lld
    ln -sf ${llvm21.llvm}/bin/llvm-ar    $out/bin/llvm-ar
    ln -sf ${llvm21.llvm}/bin/llvm-nm    $out/bin/llvm-nm
    ln -sf ${llvm21.llvm}/bin/llvm-objcopy $out/bin/llvm-objcopy
    ln -sf ${llvm21.llvm}/bin/llvm-ranlib  $out/bin/llvm-ranlib
    if [ -e ${llvm21.llvm}/bin/llvm-strip ]; then
      ln -sf ${llvm21.llvm}/bin/llvm-strip $out/bin/llvm-strip
    else
      ln -sf ${llvm21.llvm}/bin/llvm-objcopy $out/bin/llvm-strip
    fi
    cat > $out/bin/llvm-config <<LLVMSHIM
#!/bin/sh
for a in "\$@"; do last="\$a"; done
case "\$last" in
--libdir) echo "${shimLib}/lib" ;;
--bindir) echo "$out/bin" ;;
*) exec ${realLlvmCfg21} "\$@" ;;
esac
LLVMSHIM
    chmod +x $out/bin/llvm-config
  '';

  # SPIRV-LLVM-Translator (libLLVMSPIRVLib) for Mesa's clc host tools
  llvmspirv = pkgs.stdenv.mkDerivation {
    pname = "llvm-spirv";
    version = "21.1.8";
    src = pkgs.fetchurl {
      url = "https://github.com/KhronosGroup/SPIRV-LLVM-Translator/archive/v21.1.8.tar.gz";
      sha256 = "sha256-PFcg03p2eTPM+vUh/iTkmDabqv04S1mbECvGmAggxyI=";
    };
    nativeBuildInputs = [ pkgs.cmake pkgs.ninja ];
    buildInputs = [ llvm21.llvm llvm21.libclang ];
    configurePhase = ''
      if [ -f "${llvm21.llvm}/lib/cmake/llvm/LLVMConfig.cmake" ]; then
        llvm_cmake_dir="${llvm21.llvm}/lib/cmake/llvm"
      elif [ -f "${llvm21.llvm.dev}/lib/cmake/llvm/LLVMConfig.cmake" ]; then
        llvm_cmake_dir="${llvm21.llvm.dev}/lib/cmake/llvm"
      else
        echo "ERROR: could not locate LLVMConfig.cmake for LLVM 21" >&2
        exit 1
      fi
      cmake -S . -B . -G Ninja \
        -DLLVM_DIR="$llvm_cmake_dir" \
        -DLLVM_INCLUDE_TESTS=OFF \
        -DLLVM_SPIRV_INCLUDE_TESTS=OFF \
        -DLLVM_EXTERNAL_SPIRV_HEADERS_SOURCE_DIR=${pkgs.spirv-headers} \
        -DBUILD_SHARED_LIBS=ON \
        -DCMAKE_INSTALL_PREFIX=$out \
        -DCMAKE_BUILD_TYPE=Release
      cmake --build . -j$NIX_BUILD_CORES
      cmake --install .
    '';
  };
  # BASH_ENV: tools/*.sh reset PATH; this re-prepends the nix PATH in every bash
  bashEnv = pkgs.writeText "eden-bash-env" ''
    if [ -n "''${EDEN_NIX_PREFIX:-}" ]; then
      case "$PATH" in "''${EDEN_NIX_PREFIX}"*) ;; *) PATH="''${EDEN_NIX_PREFIX}$PATH"; export PATH ;; esac
      trap 'case "$PATH" in "''${EDEN_NIX_PREFIX}"*) ;; *) PATH="''${EDEN_NIX_PREFIX}$PATH"; export PATH ;; esac' DEBUG
    fi
  '';
in

pkgs.mkShell {
  name = "prospero-eden";

  nativeBuildInputs = [
    shimBin
    pkgs.cmake
    pkgs.ninja
    pkgs.meson
    pkgs.gnumake
    pkgs.ccache
    pkgs.nasm
    pkgs.bison
    pkgs.flex
    pkgs.pkg-config
    pkgs.util-linux
    pkgs.curl
    pkgs.wget
    pkgs.git
    pkgs.rsync
    pkgs.unzip
    pkgs.file
    pkgs.patch
    pkgs.glslang
    pkgs.spirv-tools
    pkgs.spirv-headers
    pkgs.xxd
    pkgs.which
    llvm21.clang
    llvm21.llvm
    llvm21.lld
    llvm21.libclang
    llvm18.clang
    llvm18.llvm
    llvm18.lld
    llvmspirv
    python
  ];

  env = {
    # sourced by every non-interactive bash; restores our PATH after scripts reset it
    BASH_ENV = "${bashEnv}";
    # Payload SDK wrappers select the LLVM install; the shim reports our bindir/libdir
    LLVM_CONFIG = "${shimBin}/bin/llvm-config";
    # shimLib/llvmspirv for LLVM 21 host tools; gcc runtime for -fuse-ld=lld links
    LD_LIBRARY_PATH = "${shimLib}/lib:${llvmspirv}/lib:${pkgs.stdenv.cc.cc.lib}/lib";
    # nixpkgs' cc-wrapper hardening (FORTIFY, stack protector) differs from Ubuntu's toolchain
    NIX_HARDENING_ENABLE = "";
  };

  shellHook = ''
    export EDEN_NIX_PREFIX="$PATH:"
    echo "ProsperoEden shell: LLVM 18.1 + LLVM 21.1, llvm-spirv @21"
    echo "  clang-18=$(clang-18 --version 2>/dev/null | head -1)"
    echo "  clang-21=$(clang-21 --version 2>/dev/null | head -1)"
    echo "  ld.lld-21=$(ld.lld-21 --version 2>/dev/null)"
    echo "  llvm-config=$(llvm-config --version 2>/dev/null)"
  '';
}