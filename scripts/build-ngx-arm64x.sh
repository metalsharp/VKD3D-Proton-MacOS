#!/bin/sh
# scripts/build-ngx-arm64x.sh: VKD3D-Proton-MacOS's NGX core and NVAPI (ngx/), ARM64X like VKD3D-Proton, into
# artifacts/build/ngx/aarch64-windows. Needs LLVM_MINGW (an llvm-mingw with arm64ec) and sources/vkd3d-proton and
# sources/nvapi (scripts/fetch-sources.sh).
set -e
WS="$(cd "$(dirname "$0")/.." && pwd)"
LLVM_MINGW=${LLVM_MINGW:?set LLVM_MINGW to an llvm-mingw with arm64ec support}
OUT="$WS/artifacts/build/ngx-arm64x-build"
STAGE="$WS/artifacts/build/ngx/aarch64-windows"
CROSS="$WS/artifacts/ngx-cross-arm64x.txt"
export PATH="$LLVM_MINGW/bin:$PATH"

[ -f "$WS/sources/nvapi/nvapi.h" ] || { echo "sources/nvapi missing (scripts/fetch-sources.sh nvapi)"; exit 1; }
mkdir -p "$(dirname "$CROSS")"
cat > "$CROSS" <<'CROSSFILE'
[binaries]
cpp = 'arm64ec-w64-mingw32-clang++'
ar = 'llvm-ar'
strip = 'llvm-strip'

[built-in options]
cpp_args = ['-marm64x']
cpp_link_args = ['-marm64x']

[host_machine]
system = 'windows'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'

[properties]
needs_exe_wrapper = true
CROSSFILE

rm -rf "$OUT"
meson setup --cross-file "$CROSS" --buildtype release --strip "$OUT" "$WS/ngx"
ninja -C "$OUT"
mkdir -p "$STAGE"
cp "$OUT/nvngx.dll" "$OUT/nvapi64.dll" "$STAGE/"
file "$STAGE"/*.dll
