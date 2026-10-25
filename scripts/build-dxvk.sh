#!/bin/bash
# Build DXVK 2.7.1 (sources/dxvk, see fetch-sources.sh) for the VKD3D lane: its dxgi.dll presents VKD3D-Proton's
# swap chains, and its d3d11.dll provides D3D11On12CreateDevice on VKD3D-Proton devices.
#   LLVM_MINGW=<llvm-mingw> scripts/build-dxvk.sh [out-dir]
set -euo pipefail
WS="$(cd "$(dirname "$0")/.." && pwd)"
LLVM_MINGW="${LLVM_MINGW:?LLVM_MINGW is not set}"
OUT="${1:-$WS/artifacts/build/dxvk}"
SRC="$WS/sources/dxvk"
[ -f "$SRC/meson.build" ] || { echo "DXVK source missing: $SRC (run scripts/fetch-sources.sh)" >&2; exit 1; }
export PATH="$LLVM_MINGW/bin:$PATH"
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
cp "$OUT/x86_64/src/dxgi/dxgi.dll" "$OUT/x86_64/src/d3d11/d3d11.dll" "$OUT/x86_64/src/d3d10/d3d10core.dll" "$OUT/x86_64-windows/"
ls -la "$OUT/x86_64-windows"
