#!/bin/bash
# Build the D3D12 functional tests for x86_64 (FEX) and native arm64.
#   LLVM_MINGW=<llvm-mingw> tests/d3d12/build.sh <out-dir>
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:?usage: build.sh <out-dir>}"
LLVM_MINGW=${LLVM_MINGW:?set LLVM_MINGW to an llvm-mingw toolchain}
mkdir -p "$OUT"
for arch in x86_64 aarch64; do
    "$LLVM_MINGW/bin/$arch-w64-mingw32-clang++" -O1 -g -std=c++17 "$HERE/d3d12test.cpp" -o "$OUT/d3d12test-$arch.exe" \
        -ld3d12 -ldxgi -ld3dcompiler_47 -ldxguid -luuid -static
done
ls "$OUT"
