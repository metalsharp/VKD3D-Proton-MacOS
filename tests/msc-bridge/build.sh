#!/bin/bash
# Builds the Metal Shader Converter bridge tests (docs/MSC-BRIDGE-CONTRACT.md in MetalSharp-Wine):
#  - msctest: native, drives libMoltenVK.dylib directly with blobs as VKD3D-Proton writes them (compute with root
#    descriptors, tables, typed buffers, heaps; vertex + pixel shaders; non-uniform indexing; typed offsets).
#      artifacts/build/msc-bridge/msctest artifacts/mvk-new/libMoltenVK.dylib cs1.dxil cs2.dxil cs3.dxil vs.dxil ps.dxil \
#          [cs5.dxil cs6.dxil cs7.dxil cs8.dxil]   (MSCTEST_ROBUST=1: with robustness, as VKD3D-Proton enables it)
#    libmetalirconverter.dylib must sit next to the libMoltenVK.dylib it loads.
#    MSCTEST_SPIRV=artifacts/build/msc-bridge adds test 10: SPIR-V and bridge draws in one render pass.
#  - mscprobe: converts dumped DXIL with a dumped root signature (METALSHARP_TRACE=shader writes <hash>.dxil and
#    <hash>.rs) as MoltenVK does, and prints the converter's reflection next to VKD3D-Proton's argument layout.
#      DYLD_LIBRARY_PATH=artifacts/mvk-new artifacts/build/msc-bridge/mscprobe <root signature .rs> <shader .dxil>...
#  - eitest.exe: a D3D12 program (aarch64, run under Wine with the VKD3D-Proton build): plain and ExecuteIndirect
#    dispatches and draws with root constant changes, on bridge PSOs.
#  - wave16test.exe, wave16btest.exe: D3D12 programs (x86_64, so MetalSharp Wine runs them on its VKD3D-Proton lane):
#    wave operations on 16-bit values, checked against the CPU; and 16-bit values through groupshared memory, raw
#    buffers and a half-float UAV over several waves, written to a file to compare with a METALSHARP_MSC=0 run.
# The shaders are compiled with dxc and glslangValidator (DXC=path to dxc, run under WINE when it is a Windows build).
#   LLVM_MINGW=... DXC=... [WINE=...] tests/msc-bridge/build.sh
set -euo pipefail
WS="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$WS/tests/msc-bridge"
OUT="$WS/artifacts/build/msc-bridge"
mkdir -p "$OUT"
dxc() { # dxc <profile> <hlsl> <out> [dxc flags...]
    local profile="$1" hlsl="$2" out="$3"
    shift 3
    if [ -n "${WINE:-}" ]; then
        cp "$hlsl" "$OUT/tmp.hlsl"
        (cd "$OUT" && "$WINE" "$DXC" -nologo -T "$profile" -E main "$@" -Fo "$(basename "$out")" tmp.hlsl)
    else
        "$DXC" -nologo -T "$profile" -E main "$@" -Fo "$out" "$hlsl"
    fi
}
for s in cs1:cs_6_0 cs2:cs_6_0 cs3:cs_6_6 cs5:cs_6_0 cs6:cs_6_0 cs7:cs_6_0 cs8:cs_6_0 vs:vs_6_0 ps:ps_6_0 \
         ei_cs:cs_6_0 ei_vs:vs_6_0 ei_ps:ps_6_0; do
    dxc "${s##*:}" "$SRC/hlsl/${s%%:*}.hlsl" "$OUT/${s%%:*}.dxil"
done
for s in wave16 wave16b; do
    dxc cs_6_2 "$SRC/hlsl/$s.hlsl" "$OUT/$s.dxil" -enable-16bit-types
done
clang -O1 -I "$WS/sources/MoltenVK/External/Vulkan-Headers/include" -o "$OUT/msctest" "$SRC/msctest.c"
glslangValidator -V -o "$OUT/il_vs.spv" "$SRC/glsl/il.vert"
glslangValidator -V -o "$OUT/il_fs.spv" "$SRC/glsl/il.frag"
MSC_HEADERS="$WS/sources/MoltenVK/MoltenVK/MoltenVK/MetalShaderConverter/metal_irconverter"
clang -O1 -I "$MSC_HEADERS" -o "$OUT/mscprobe" "$SRC/mscprobe.c" -L "$WS/artifacts/mvk-new" -lmetalirconverter
for n in cs vs ps; do (cd "$OUT" && xxd -i -n ${n}_dxil ei_$n.dxil > $n.h); done
"$LLVM_MINGW/bin/aarch64-w64-mingw32-clang" -O1 -I "$OUT" -o "$OUT/eitest.exe" "$SRC/eitest.c" -ld3d12
for s in wave16 wave16b; do
    (cd "$OUT" && xxd -i -n ${s}_dxil $s.dxil > $s.h)
    "$LLVM_MINGW/bin/x86_64-w64-mingw32-clang" -O1 -I "$OUT" -o "$OUT/${s}test.exe" "$SRC/${s}test.c" -ld3d12
done
echo "msc-bridge tests: $OUT"
