#!/bin/bash
# Build DXVK for the x86_64 lane from sources/dxvk, metalsharp/DXVK-MacOS (DXVK 3.1.1 with its MoltenVK work, see
# fetch-sources.sh): dxgi.dll presents VKD3D-Proton's swap chains, d3d11.dll and d3d10core.dll run D3D11 and D3D10
# programs and provide D3D11On12CreateDevice on VKD3D-Proton devices, d3d9.dll and d3d8.dll run D3D9 and D3D8 ones. The arm64 lane builds the same source as ARM64X
# (build-dxvk-arm64x.sh).
#   LLVM_MINGW=<llvm-mingw> scripts/build-dxvk.sh [out-dir]
set -euo pipefail
WS="$(cd "$(dirname "$0")/.." && pwd)"
LLVM_MINGW="${LLVM_MINGW:?LLVM_MINGW is not set}"
OUT="${1:-$WS/artifacts/build/dxvk}"
SRC="$WS/sources/dxvk"
[ -f "$SRC/meson.build" ] || { echo "DXVK source missing: $SRC (run scripts/fetch-sources.sh)" >&2; exit 1; }
export PATH="$LLVM_MINGW/bin:$PATH"
# macOS compiler flags would reach the PE cross compilers
unset CFLAGS CXXFLAGS OBJCFLAGS LDFLAGS CPPFLAGS CC CXX OBJC
mkdir -p "$OUT"
cat > "$OUT/cross-x86_64.txt" <<CROSS
[binaries]
c = 'x86_64-w64-mingw32-clang'
cpp = 'x86_64-w64-mingw32-clang++'
ar = 'llvm-ar'
strip = 'llvm-strip'
windres = 'x86_64-w64-mingw32-windres'
[host_machine]
system = 'windows'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
[properties]
needs_exe_wrapper = true
CROSS
[ -f "$OUT/x86_64/build.ninja" ] || meson setup --cross-file "$OUT/cross-x86_64.txt" --buildtype release \
    -Db_ndebug=if-release "$OUT/x86_64" "$SRC" >/dev/null
ninja -C "$OUT/x86_64" >/dev/null
mkdir -p "$OUT/x86_64-windows"
for dll in dxgi/dxgi d3d11/d3d11 d3d10/d3d10core d3d9/d3d9 d3d8/d3d8; do
    llvm-strip -o "$OUT/x86_64-windows/${dll##*/}.dll" "$OUT/x86_64/src/$dll.dll"
done
ls -la "$OUT/x86_64-windows"
