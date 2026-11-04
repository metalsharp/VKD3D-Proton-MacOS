#!/bin/bash
# Build DXVK 2.7.1 (sources/dxvk, see fetch-sources.sh) for the arm64 lane as ARM64X, like build-vkd3d-proton-arm64x.sh
# builds VKD3D-Proton: one DLL with ARM64 and ARM64EC code, so x86_64 programs (ARM64EC under FEX) and native ARM64
# programs both load it without emulation. dxgi.dll presents VKD3D-Proton's swap chains; d3d11.dll and d3d10core.dll
# provide D3D11On12CreateDevice on VKD3D-Proton devices. The same source and patches as build-dxvk.sh's x86_64 build.
# The output folder is x86_64-windows because MetalSharp Wine loads the lane's DXGI and D3D10/11 from there.
#   LLVM_MINGW=<llvm-mingw> scripts/build-dxvk-arm64x.sh [out-dir]
set -euo pipefail
WS="$(cd "$(dirname "$0")/.." && pwd)"
LLVM_MINGW="${LLVM_MINGW:?LLVM_MINGW is not set}"
OUT="${1:-$WS/artifacts/build/dxvk-arm64x}"
SRC="$WS/sources/dxvk"
[ -f "$SRC/meson.build" ] || { echo "DXVK source missing: $SRC (run scripts/fetch-sources.sh)" >&2; exit 1; }
export PATH="$LLVM_MINGW/bin:$PATH"
command -v arm64ec-w64-mingw32-clang >/dev/null || { echo "arm64ec-w64-mingw32-clang not in $LLVM_MINGW/bin" >&2; exit 1; }
# macOS compiler flags would reach the PE cross compilers
unset CFLAGS CXXFLAGS OBJCFLAGS LDFLAGS CPPFLAGS CC CXX OBJC
mkdir -p "$OUT"
# The target-prefixed windres: llvm-windres alone does not find the target's windows.h for DXVK's version resources.
cat > "$OUT/cross-arm64x.txt" <<'CROSS'
[binaries]
c = 'arm64ec-w64-mingw32-clang'
cpp = 'arm64ec-w64-mingw32-clang++'
ar = 'llvm-ar'
strip = 'llvm-strip'
windres = 'arm64ec-w64-mingw32-windres'

[built-in options]
c_args = ['-marm64x']
cpp_args = ['-marm64x']
c_link_args = ['-marm64x']
cpp_link_args = ['-marm64x']

[host_machine]
system = 'windows'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'

[properties]
needs_exe_wrapper = true
CROSS
[ -f "$OUT/build/build.ninja" ] || meson setup --cross-file "$OUT/cross-arm64x.txt" --buildtype release \
    -Db_ndebug=if-release "$OUT/build" "$SRC" >/dev/null
ninja -C "$OUT/build" >/dev/null
mkdir -p "$OUT/x86_64-windows"
for dll in dxgi/dxgi d3d11/d3d11 d3d10/d3d10core; do
    llvm-strip -o "$OUT/x86_64-windows/${dll##*/}.dll" "$OUT/build/src/$dll.dll"
done
ls -la "$OUT/x86_64-windows"
