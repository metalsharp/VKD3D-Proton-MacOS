#!/bin/bash
# Builds tests/memory/tess_stress.exe (x86_64, so MetalSharp Wine runs it on its VKD3D-Proton lane). The shaders are
# compiled with dxc (DXC=path to dxc, run under WINE when it is a Windows build).
#   LLVM_MINGW=... DXC=... [WINE=...] tests/memory/build.sh
set -euo pipefail
WS="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$WS/tests/memory"
OUT="$WS/artifacts/build/memory"
mkdir -p "$OUT"
: > "$OUT/tess_stress_shaders.h"
for s in vs hs ds ps; do
    S=$(echo $s | tr a-z A-Z)
    if [ -n "${WINE:-}" ]; then
        cp "$SRC/tess_stress.hlsl" "$OUT/tmp.hlsl"
        (cd "$OUT" && "$WINE" "$DXC" -nologo -T ${s}_6_0 -E main -D $S=1 -Fo $s.dxil tmp.hlsl)
    else
        "$DXC" -nologo -T ${s}_6_0 -E main -D $S=1 -Fo "$OUT/$s.dxil" "$SRC/tess_stress.hlsl"
    fi
    (cd "$OUT" && xxd -i -n ${s}_dxil $s.dxil >> tess_stress_shaders.h)
done
"$LLVM_MINGW/bin/x86_64-w64-mingw32-clang" -O1 -Wall -I "$OUT" -o "$OUT/tess_stress.exe" "$SRC/tess_stress.c" -ld3d12
echo "built $OUT/tess_stress.exe"
