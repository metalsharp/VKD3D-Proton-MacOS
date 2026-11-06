#!/bin/bash
# Build libdxbcconverter.dylib, DirectXShaderCompiler's DXBC to DXIL converter (dxilconv) as a universal (arm64 +
# x86_64) C library, from sources/DirectXShaderCompiler with patches/dxc/metalsharp-dxilconv.diff
# (scripts/fetch-sources.sh dxc). MoltenVK loads it from next to libMoltenVK.dylib to hand VKD3D-Proton's DXBC
# (SM 4.x/5.x) shaders to Metal Shader Converter, which takes only DXIL.
#   scripts/build-dxbcconverter.sh            Output: artifacts/dxbcconverter/libdxbcconverter.dylib (+ LICENSE.dxc.txt)
set -e
WS="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$WS/sources/DirectXShaderCompiler"
BUILD="${DXC_BUILD_DIR:-$WS/artifacts/build/dxbcconverter-universal}"
OUT="$WS/artifacts/dxbcconverter"
[ -d "$SRC/projects/dxilconv/tools/dxbcconverter-mac" ] || { echo "run scripts/fetch-sources.sh dxc first" >&2; exit 1; }
mkdir -p "$BUILD" "$OUT"
# DXC's predefined options, without SPIR-V code generation (the converter needs none of it)
sed -e 's/set(ENABLE_SPIRV_CODEGEN ON/set(ENABLE_SPIRV_CODEGEN OFF/' -e 's/set(SPIRV_BUILD_TESTS ON/set(SPIRV_BUILD_TESTS OFF/' \
    "$SRC/cmake/caches/PredefinedParams.cmake" > "$BUILD/params.cmake"
cmake -S "$SRC" -B "$BUILD" -G Ninja -C "$BUILD/params.cmake" -DCMAKE_BUILD_TYPE=Release \
    -DHLSL_BUILD_DXILCONV=ON -DHLSL_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_TESTS=OFF \
    -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 >/dev/null
ninja -C "$BUILD" dxbcconverter
cp "$BUILD/lib/libdxbcconverter.dylib" "$OUT/"
install_name_tool -id @rpath/libdxbcconverter.dylib "$OUT/libdxbcconverter.dylib"
strip -x "$OUT/libdxbcconverter.dylib"
codesign -s - -f "$OUT/libdxbcconverter.dylib"
cp "$SRC/LICENSE.TXT" "$OUT/LICENSE.dxc.txt"
lipo -info "$OUT/libdxbcconverter.dylib"
