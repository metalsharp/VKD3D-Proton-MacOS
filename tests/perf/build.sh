#!/bin/bash
# Build the D3D12 performance probe for x86_64 (games' path: FEX + ARM64EC VKD3D-Proton) and arm64.
#   LLVM_MINGW=<llvm-mingw> tests/perf/build.sh [out-dir]   (default artifacts/build/perf)
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$HERE/../../artifacts/build/perf}"
LLVM_MINGW=${LLVM_MINGW:?set LLVM_MINGW to an llvm-mingw toolchain}
mkdir -p "$OUT"
for arch in x86_64 aarch64; do
    "$LLVM_MINGW/bin/$arch-w64-mingw32-clang++" -O2 -std=c++17 "$HERE/d3d12_perf_probe.cpp" -o "$OUT/d3d12_perf_probe-$arch.exe" \
        -ld3d12 -ldxgi -ld3dcompiler_47 -luser32 -static
done
ls "$OUT"
