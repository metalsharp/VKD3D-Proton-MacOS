#!/bin/bash
# Build the custom vkd3d-proton PE pair (d3d12.dll forwarder + d3d12core.dll impl)
# with llvm-mingw + ninja + meson, targeting x86_64-windows.
set -e
WS="$(cd "$(dirname "$0")/.." && pwd)"
source "$WS/scripts/env.sh"
SRC="$WS/sources/vkd3d-proton"
OUT="$WS/artifacts/build/vkd3d-proton-build"
DXGI_LIFECYCLE_PATCH="$WS/patches/vkd3d-proton-dxgi-lifecycle.patch"

[ -d "$SRC/libs/vkd3d" ] || { echo "vkd3d-proton source missing"; exit 1; }
[ -s "$DXGI_LIFECYCLE_PATCH" ] || { echo "vkd3d lifecycle patch missing"; exit 1; }

# Keep the downloaded source checkout at its pinned revision.  The lifecycle
# guard is a small source patch applied only for this build and then reverted,
# just like the checked-in DXVK bridge lane.
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
git -C "$SRC" apply --check "$DXGI_LIFECYCLE_PATCH"
git -C "$SRC" apply "$DXGI_LIFECYCLE_PATCH"
patch_applied=1

cat > "$WS/artifacts/vkd3d-cross-x86_64.txt" <<'EOF'
[binaries]
c = 'x86_64-w64-mingw32-clang'
cpp = 'x86_64-w64-mingw32-clang++'
ar = 'llvm-ar'
strip = 'llvm-strip'
windres = 'llvm-windres'
widl = 'x86_64-w64-mingw32-widl'
[host_machine]
system = 'windows'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
[properties]
needs_exe_wrapper = true
EOF

cd "$SRC"
meson setup --cross-file "$WS/artifacts/vkd3d-cross-x86_64.txt" \
    --buildtype release --strip \
    "$OUT" . || { echo "meson setup failed (check meson_options.txt for current flags)"; exit 1; }
ninja -C "$OUT"

mkdir -p "$WS/artifacts/build/vkd3d-proton/x86_64-windows"
# locate the built DLLs wherever meson placed them
find "$OUT" -name 'd3d12.dll'   -exec cp {} "$WS/artifacts/build/vkd3d-proton/x86_64-windows/" \; -print
find "$OUT" -name 'd3d12core.dll' -exec cp {} "$WS/artifacts/build/vkd3d-proton/x86_64-windows/" \; -print

echo "== built =="
file "$WS"/artifacts/build/vkd3d-proton/x86_64-windows/*.dll
shasum -a 256 "$WS"/artifacts/build/vkd3d-proton/x86_64-windows/*.dll
