#!/bin/bash
# Build DXVK (dxvk/: DXVK 3.1 on MoltenVK, with d7vk's ddraw) for one lane:
#   x86_64  x86_64 Windows programs, x86_64-windows        (artifacts/build/dxvk)
#   i386    32-bit programs, i386-windows                   (artifacts/build/dxvk-i386)
#   arm64x  ARM64X, for an arm64 Wine that runs x86_64 programs with FEX (MetalSharp Wine): one DLL with ARM64 code
#           and ARM64EC code, which x86_64 programs load, so DXVK is not emulated; x86_64-windows, where that Wine
#           loads its DXVK lanes from                       (artifacts/build/dxvk-arm64x)
# Each holds ddraw.dll (D3D7 to 3), d3d8.dll, d3d9.dll, d3d10core.dll, d3d11.dll and dxgi.dll. The VKD3D lane takes
# its dxgi.dll (VKD3D-Proton's swap chains), d3d11.dll and d3d10core.dll (D3D11On12) from the same build.
# dxvk/'s submodules are checked out, and its dxbc-spirv patch applied, when they are not yet.
#   LLVM_MINGW=<llvm-mingw> scripts/build-dxvk.sh [x86_64|i386|arm64x] [out-dir]
set -euo pipefail
WS="$(cd "$(dirname "$0")/.." && pwd)"
LLVM_MINGW="${LLVM_MINGW:?LLVM_MINGW is not set}"
LANE="${1:-x86_64}"
SRC="$WS/dxvk"
case "$LANE" in
    x86_64) OUT="${2:-$WS/artifacts/build/dxvk}";       DIR=x86_64-windows; TRIPLE=x86_64-w64-mingw32 ;;
    i386)   OUT="${2:-$WS/artifacts/build/dxvk-i386}";  DIR=i386-windows;   TRIPLE=i686-w64-mingw32 ;;
    arm64x) OUT="${2:-$WS/artifacts/build/dxvk-arm64x}"; DIR=x86_64-windows; TRIPLE=arm64ec-w64-mingw32 ;;
    *) echo "usage: build-dxvk.sh [x86_64|i386|arm64x] [out-dir]" >&2; exit 2 ;;
esac
export PATH="$LLVM_MINGW/bin:$PATH"
command -v "$TRIPLE-clang" >/dev/null || { echo "$TRIPLE-clang not in $LLVM_MINGW/bin" >&2; exit 1; }
# macOS compiler flags would reach the PE cross compilers
unset CFLAGS CXXFLAGS OBJCFLAGS LDFLAGS CPPFLAGS CC CXX OBJC

bash "$WS/scripts/fetch-sources.sh" dxvk

mkdir -p "$OUT"
CROSS="$OUT/cross-$LANE.txt"
if [ "$LANE" = arm64x ]; then
    cp "$SRC/build-arm64x.txt" "$CROSS"
else
    # dxvk/build-win64.txt and build-win32.txt name GCC's MinGW; llvm-mingw's clang builds the same
    cpu_family=$([ "$LANE" = i386 ] && echo x86 || echo x86_64)
    cpu=$([ "$LANE" = i386 ] && echo i686 || echo x86_64)
    cat > "$CROSS" <<CROSS
[binaries]
c = '$TRIPLE-clang'
cpp = '$TRIPLE-clang++'
ar = 'llvm-ar'
strip = 'llvm-strip'
windres = '$TRIPLE-windres'
[host_machine]
system = 'windows'
cpu_family = '$cpu_family'
cpu = '$cpu'
endian = 'little'
[properties]
needs_exe_wrapper = true
CROSS
fi
[ -f "$OUT/build/build.ninja" ] || meson setup --cross-file "$CROSS" --buildtype release -Db_ndebug=if-release \
    "$OUT/build" "$SRC" >/dev/null
ninja -C "$OUT/build" >/dev/null
mkdir -p "$OUT/$DIR"
for dll in ddraw/ddraw d3d8/d3d8 d3d9/d3d9 d3d10/d3d10core d3d11/d3d11 dxgi/dxgi; do
    llvm-strip -o "$OUT/$DIR/${dll##*/}.dll" "$OUT/build/src/$dll.dll"
done
ls -la "$OUT/$DIR"
