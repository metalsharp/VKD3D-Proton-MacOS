#!/bin/bash
# Prints the key MoltenVK's pipeline caches are kept under (16 hex digits): a hash of the sources that decide what a
# cache holds, so a rebuild that changes none of them keeps the caches games built, where the build UUID changed with
# every build and each launch after one compiled every shader again.
#   scripts/moltenvk-cache-key.sh [MoltenVK source dir]
# The sources: SPIRV-Cross (the MSL it writes), the shader converter (its options and results), the shader libraries
# (MVKShaderModule), and the pipeline cache's stream format and Cereal definitions (that part of MVKPipeline.mm).
# A conversion option is part of each cached library's configuration, so options alone don't need a new key.
set -e
SRC="${1:-$(cd "$(dirname "$0")/.." && pwd)/sources/MoltenVK}"
cd "$SRC"
{
    find External/SPIRV-Cross -maxdepth 1 -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) | LC_ALL=C sort | xargs shasum
    find MoltenVKShaderConverter/MoltenVKShaderConverter MoltenVKShaderConverter/include -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.mm' -o -name '*.def' \) | LC_ALL=C sort | xargs shasum
    shasum MoltenVK/MoltenVK/GPUObjects/MVKShaderModule.h MoltenVK/MoltenVK/GPUObjects/MVKShaderModule.mm
    awk '/^#pragma mark Streaming pipeline cache to and from offline memory/ {on = 1}
         /^#pragma mark MVKRenderPipelineCompiler/ {on = 0}
         on' MoltenVK/MoltenVK/GPUObjects/MVKPipeline.mm | shasum
} | sed 's#  .*/#  #' | shasum | cut -c1-16
