#!/bin/bash
# Build tests/fsr/fsr4probe.exe (x86_64) against the FSR SDK's FFX API headers (scripts/fetch-fsr-sdk.sh), and
# put it in <out-dir> with the SDK's DX12 loader and upscaler DLLs, unmodified, which it loads from beside itself.
#   LLVM_MINGW=<llvm-mingw> scripts/build-fsr-probe.sh [out-dir]
set -euo pipefail
WS="$(cd "$(dirname "$0")/.." && pwd)"
LLVM_MINGW="${LLVM_MINGW:?LLVM_MINGW is not set}"
SDK="$WS/sources/fsr-sdk/Kits/FidelityFX"
OUT="${1:-$WS/artifacts/build/fsr-probe}"
[ -d "$SDK/api/include" ] || { echo "FSR SDK missing (run scripts/fetch-fsr-sdk.sh)" >&2; exit 1; }
# macOS compiler flags would reach the PE cross compiler
unset CFLAGS CXXFLAGS OBJCFLAGS LDFLAGS CPPFLAGS CC CXX OBJC
mkdir -p "$OUT"
"$LLVM_MINGW/bin/x86_64-w64-mingw32-clang++" -std=c++20 -O2 -Wall -Wno-missing-field-initializers \
    -I"$SDK/api/include" -I"$SDK/upscalers/include" -I"$SDK/framegeneration/include" \
    "$WS/tests/fsr/fsr4probe.cpp" -o "$OUT/fsr4probe.exe" -static -ld3d12 -ldxgi
cp "$SDK/signedbin/amd_fidelityfx_loader_dx12.dll" "$SDK/signedbin/amd_fidelityfx_upscaler_dx12.dll" \
    "$SDK/signedbin/amd_fidelityfx_framegeneration_dx12.dll" "$OUT/"
cp "$SDK/docs/license.md" "$OUT/LICENSE.fsr-sdk.md"
ls -la "$OUT"
