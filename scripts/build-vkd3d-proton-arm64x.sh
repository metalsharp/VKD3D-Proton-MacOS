#!/bin/bash
# Build the vkd3d-proton PE pair (d3d12.dll forwarder + d3d12core.dll impl) as ARM64X for
# MetalSharp Wine (native arm64 Wine with FEX): one DLL with ARM64 and ARM64EC code, so native
# ARM64 programs and x86_64 programs (ARM64EC under FEX) both run it without emulation.
# Uses llvm-mingw's arm64ec-w64-mingw32 toolchain with -marm64x, as MetalSharp Wine's Mullion does.
#   LLVM_MINGW=<llvm-mingw> build-vkd3d-proton-arm64x.sh
set -e
WS="$(cd "$(dirname "$0")/.." && pwd)"
LLVM_MINGW=${LLVM_MINGW:?set LLVM_MINGW to an llvm-mingw with arm64ec support}
SRC="$WS/sources/vkd3d-proton"
OUT="$WS/artifacts/build/vkd3d-proton-arm64x-build"
STAGE="$WS/artifacts/build/vkd3d-proton/aarch64-windows"
CROSS="$WS/artifacts/vkd3d-cross-arm64x.txt"
DXGI_LIFECYCLE_PATCH="$WS/patches/vkd3d-proton-dxgi-lifecycle.patch"
# The MoltenVK changes (patches/vkd3d-proton-moltenvk.patch, from the metalsharp-moltenvk branch)
MOLTENVK_PATCH="$WS/patches/vkd3d-proton-moltenvk.patch"
export PATH="$LLVM_MINGW/bin:$PATH"

[ -d "$SRC/libs/vkd3d" ] || { echo "vkd3d-proton source missing"; exit 1; }
command -v arm64ec-w64-mingw32-clang >/dev/null || { echo "arm64ec-w64-mingw32-clang not in $LLVM_MINGW/bin"; exit 1; }
command -v glslangValidator >/dev/null || { echo "glslangValidator is required"; exit 1; }

# Same lifecycle guard as the x86_64 lane, applied for this build and then reverted.
patch_applied=0
cleanup() {
    if [ "$patch_applied" -eq 1 ]; then
        git -C "$SRC" apply -R "$DXGI_LIFECYCLE_PATCH" || true
    fi
}
trap cleanup EXIT
git -C "$SRC" diff --quiet -- libs/vkd3d/swapchain.c || {
    echo "swapchain.c has uncommitted changes; refusing to apply lifecycle patch" >&2
    exit 1
}
# A plain upstream checkout gets the MoltenVK patch committed on top; the metalsharp-moltenvk branch has it.
if ! git -C "$SRC" grep -q "aligned through the typed offset buffer" -- libs/vkd3d/device.c; then
    git -C "$SRC" am -q "$MOLTENVK_PATCH"
fi
git -C "$SRC" apply --check "$DXGI_LIFECYCLE_PATCH"
git -C "$SRC" apply "$DXGI_LIFECYCLE_PATCH"
patch_applied=1

mkdir -p "$(dirname "$CROSS")"
cat > "$CROSS" <<'EOF'
[binaries]
c = 'arm64ec-w64-mingw32-clang'
cpp = 'arm64ec-w64-mingw32-clang++'
ar = 'llvm-ar'
strip = 'llvm-strip'
windres = 'llvm-windres'
widl = 'arm64ec-w64-mingw32-widl'

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
EOF

rm -rf "$OUT"
meson setup --cross-file "$CROSS" --buildtype release --strip "$OUT" "$SRC"
ninja -C "$OUT"

mkdir -p "$STAGE"
find "$OUT" -name 'd3d12.dll' -exec cp {} "$STAGE/" \;
find "$OUT" -name 'd3d12core.dll' -exec cp {} "$STAGE/" \;
echo "== built =="
file "$STAGE"/*.dll
