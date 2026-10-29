#!/bin/bash
# Build DXVK for the arm64 lane (MetalSharp Wine's VKD3D-Proton lane) from sources/dxvk, metalsharp/DXVK-MacOS
# (DXVK 3.1.1 with its MoltenVK work, see fetch-sources.sh): dxgi.dll presents VKD3D-Proton's swap chains, d3d11.dll and
# d3d10core.dll run D3D11 and D3D10 programs and provide D3D11On12CreateDevice on VKD3D-Proton devices, d3d9.dll and
# d3d8.dll run D3D9 and D3D8 ones. ARM64X, like
# VKD3D-Proton's arm64 build: x86_64 programs load the DLLs as ARM64EC under FEX, so DXVK is not emulated.
# The output folder is x86_64-windows because MetalSharp Wine loads the lane's DLLs for x86_64 programs from there.
#   LLVM_MINGW=<llvm-mingw> scripts/build-dxvk-arm64x.sh [out-dir]
set -euo pipefail
WS="$(cd "$(dirname "$0")/.." && pwd)"
LLVM_MINGW="${LLVM_MINGW:?LLVM_MINGW is not set}"
OUT="${1:-$WS/artifacts/build/dxvk-arm64x}"
SRC="$WS/sources/dxvk"
[ -f "$SRC/build-arm64x.txt" ] || { echo "DXVK source missing: $SRC (run scripts/fetch-sources.sh dxvk)" >&2; exit 1; }
export PATH="$LLVM_MINGW/bin:$PATH"
command -v arm64ec-w64-mingw32-clang >/dev/null || { echo "arm64ec-w64-mingw32-clang not in $LLVM_MINGW/bin" >&2; exit 1; }
# macOS compiler flags would reach the PE cross compilers
unset CFLAGS CXXFLAGS OBJCFLAGS LDFLAGS CPPFLAGS CC CXX OBJC
mkdir -p "$OUT"
[ -f "$OUT/build/build.ninja" ] || meson setup --cross-file "$SRC/build-arm64x.txt" --buildtype release \
    -Db_ndebug=if-release "$OUT/build" "$SRC" >/dev/null
ninja -C "$OUT/build" >/dev/null
mkdir -p "$OUT/x86_64-windows"
for dll in dxgi/dxgi d3d11/d3d11 d3d10/d3d10core d3d9/d3d9 d3d8/d3d8; do
    llvm-strip -o "$OUT/x86_64-windows/${dll##*/}.dll" "$OUT/build/src/$dll.dll"
done
ls -la "$OUT/x86_64-windows"
