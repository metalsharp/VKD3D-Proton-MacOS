#!/bin/bash
# Build amdxc64.dll (amdxc64/), the stand-in for AMD's D3D12 driver DLL that lets AMD FSR offer FSR Upscaling 4 on
# any GPU when METALSHARP_FSR4=1 (docs/fsr4.md): x86_64 for the x86_64 lane, and ARM64X for the arm64 lane, which
# x86_64 programs load as ARM64EC under FEX. Output: <out-dir>/x86_64-windows and <out-dir>/arm64x.
#   LLVM_MINGW=<llvm-mingw> scripts/build-amdxc64.sh [out-dir]
set -euo pipefail
WS="$(cd "$(dirname "$0")/.." && pwd)"
LLVM_MINGW="${LLVM_MINGW:?LLVM_MINGW is not set}"
OUT="${1:-$WS/artifacts/build/amdxc64}"
# macOS compiler flags would reach the PE cross compilers
unset CFLAGS CXXFLAGS OBJCFLAGS LDFLAGS CPPFLAGS CC CXX OBJC
mkdir -p "$OUT/x86_64-windows" "$OUT/arm64x"
FLAGS=(-O2 -Wall -Wno-unused-function -shared -static-libgcc "$WS/amdxc64/amdxc64.c" "$WS/amdxc64/amdxc64.def")
"$LLVM_MINGW/bin/x86_64-w64-mingw32-clang" "${FLAGS[@]}" -o "$OUT/x86_64-windows/amdxc64.dll"
"$LLVM_MINGW/bin/arm64ec-w64-mingw32-clang" -marm64x "${FLAGS[@]}" -o "$OUT/arm64x/amdxc64.dll"
"$LLVM_MINGW/bin/llvm-strip" "$OUT/x86_64-windows/amdxc64.dll" "$OUT/arm64x/amdxc64.dll"
ls -la "$OUT"/*/amdxc64.dll
