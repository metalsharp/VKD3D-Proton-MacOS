#!/bin/bash
# Builds the Metal Shader Converter bridge tests (docs/MSC-BRIDGE-CONTRACT.md in MetalSharp-Wine):
#  - msctest: native, drives libMoltenVK.dylib directly with blobs as VKD3D-Proton writes them (compute with root
#    descriptors, tables, typed buffers, heaps; vertex + pixel shaders; non-uniform indexing; typed offsets).
#      artifacts/build/msc-bridge/msctest artifacts/mvk-new/libMoltenVK.dylib cs1.dxil cs2.dxil cs3.dxil vs.dxil ps.dxil \
#          [cs5.dxil cs6.dxil cs7.dxil cs8.dxil]   (MSCTEST_ROBUST=1: with robustness, as VKD3D-Proton enables it)
#    libmetalirconverter.dylib must sit next to the libMoltenVK.dylib it loads.
#  - eitest.exe: a D3D12 program (aarch64, run under Wine with the VKD3D-Proton build): plain and ExecuteIndirect
#    dispatches and draws with root constant changes, on bridge PSOs.
# The shaders are compiled with dxc (DXC=path to dxc, run under WINE when it is a Windows build).
#   LLVM_MINGW=... DXC=... [WINE=...] tests/msc-bridge/build.sh
set -euo pipefail
WS="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$WS/tests/msc-bridge"
OUT="$WS/artifacts/build/msc-bridge"
mkdir -p "$OUT"
dxc() { # dxc <profile> <hlsl> <out>
    if [ -n "${WINE:-}" ]; then
        cp "$2" "$OUT/tmp.hlsl"
        (cd "$OUT" && "$WINE" "$DXC" -nologo -T "$1" -E main -Fo "$(basename "$3")" tmp.hlsl)
    else
        "$DXC" -nologo -T "$1" -E main -Fo "$3" "$2"
    fi
}
for s in cs1:cs_6_0 cs2:cs_6_0 cs3:cs_6_6 cs5:cs_6_0 cs6:cs_6_0 cs7:cs_6_0 cs8:cs_6_0 vs:vs_6_0 ps:ps_6_0 \
         ei_cs:cs_6_0 ei_vs:vs_6_0 ei_ps:ps_6_0; do
    dxc "${s##*:}" "$SRC/hlsl/${s%%:*}.hlsl" "$OUT/${s%%:*}.dxil"
done
clang -O1 -I "$WS/sources/MoltenVK/External/Vulkan-Headers/include" -o "$OUT/msctest" "$SRC/msctest.c"
for n in cs vs ps; do (cd "$OUT" && xxd -i -n ${n}_dxil ei_$n.dxil > $n.h); done
"$LLVM_MINGW/bin/aarch64-w64-mingw32-clang" -O1 -I "$OUT" -o "$OUT/eitest.exe" "$SRC/eitest.c" -ld3d12
echo "msc-bridge tests: $OUT"
