#!/bin/bash
# Build the native MoltenVK GPU tests.
#   tests/vk/build.sh <out-dir>
# Run one with the MoltenVK under test:
#   DYLD_LIBRARY_PATH=<dir with libMoltenVK.dylib> <out-dir>/robust <out-dir>/robust.spv
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
WS="$(cd "$HERE/../.." && pwd)"
OUT="${1:?usage: build.sh <out-dir>}"
MVK="${MVK:-$WS/artifacts/mvk-new}"
HEADERS="$WS/sources/MoltenVK/External/Vulkan-Headers/include"
mkdir -p "$OUT"
for src in "$HERE"/*.c; do
    name="$(basename "$src" .c)"
    clang -O1 -g -I"$HEADERS" "$src" -L"$MVK" -lMoltenVK -Wl,-rpath,"$MVK" -o "$OUT/$name"
done
for shader in "$HERE"/*.comp "$HERE"/*.vert "$HERE"/*.frag; do
    [ -e "$shader" ] || continue
    glslangValidator -V --target-env vulkan1.3 "$shader" -o "$OUT/$(basename "$shader" | tr . _).spv" >/dev/null
done
ls "$OUT"
