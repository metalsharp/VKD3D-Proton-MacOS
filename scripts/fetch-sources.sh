#!/bin/bash
# Recreate sources/ from upstream and this repository's patches:
#   sources/vkd3d-proton  upstream VKD3D-Proton + patches/vkd3d-proton-moltenvk.patch, and its subprojects/dxil-spirv
#                         + patches/dxil-spirv/*.patch
#   sources/MoltenVK      upstream MoltenVK + patches/moltenvk/metalsharp-vkmt.diff
#                         (dttdrv's ray tracing, mesh shader and fix branches, then MetalSharp's work),
#                         with External/SPIRV-Cross at the dttdrv revision + patches/spirv-cross/metalsharp.diff
#   sources/dxvk          DXVK 2.7.1 + patches/dxvk/*.patch (dxgi and D3D11On12 for the VKD3D lane)
#   sources/DirectXShaderCompiler  DXC + patches/dxc/metalsharp-dxilconv.diff: the DXBC to DXIL converter MoltenVK
#                         loads for Metal Shader Converter (scripts/build-dxbcconverter.sh)
#   scripts/fetch-sources.sh [vkd3d-proton] [moltenvk] [dxvk] [nvapi] [dxc]   (default: all five)
set -e
WS="$(cd "$(dirname "$0")/.." && pwd)"
VKD3D_REV=ef9a02e873715c9e2da0d6db8a236aca8380b303
MOLTENVK_REV=5189f8de
DXVK_TAG=v2.7.1
DXC_TAG=v1.9.2609
mkdir -p "$WS/sources"
WANT=" ${*:-vkd3d-proton moltenvk dxvk nvapi dxc} "
want() { [[ "$WANT" == *" $1 "* ]]; }

if want vkd3d-proton && [ ! -d "$WS/sources/vkd3d-proton/.git" ]; then
    git clone https://github.com/HansKristian-Work/vkd3d-proton.git "$WS/sources/vkd3d-proton"
    git -C "$WS/sources/vkd3d-proton" checkout -b metalsharp-moltenvk "$VKD3D_REV"
    git -C "$WS/sources/vkd3d-proton" submodule update --init --recursive
    git -C "$WS/sources/vkd3d-proton" am "$WS/patches/vkd3d-proton-moltenvk.patch"
    if ls "$WS"/patches/dxil-spirv/*.patch >/dev/null 2>&1; then
        git -C "$WS/sources/vkd3d-proton/subprojects/dxil-spirv" -c user.name=MetalSharp \
            -c user.email=metalsharp@users.noreply.github.com am "$WS"/patches/dxil-spirv/*.patch
    fi
fi

if want moltenvk && [ ! -d "$WS/sources/MoltenVK/.git" ]; then
    git clone https://github.com/KhronosGroup/MoltenVK.git "$WS/sources/MoltenVK"
    git -C "$WS/sources/MoltenVK" checkout -b metalsharp-vkmt "$MOLTENVK_REV"
    git -C "$WS/sources/MoltenVK" apply --index "$WS/patches/moltenvk/metalsharp-vkmt.diff"
    git -C "$WS/sources/MoltenVK" -c user.name=MetalSharp -c user.email=metalsharp@users.noreply.github.com \
        commit -q -m "MetalSharp MoltenVK (patches/moltenvk/metalsharp-vkmt.diff)"
    (cd "$WS/sources/MoltenVK" && ./fetchDependencies --none)
    git -C "$WS/sources/MoltenVK/External/SPIRV-Cross" checkout -b metalsharp-robust
    git -C "$WS/sources/MoltenVK/External/SPIRV-Cross" apply --index "$WS/patches/spirv-cross/metalsharp.diff"
    git -C "$WS/sources/MoltenVK/External/SPIRV-Cross" -c user.name=MetalSharp -c user.email=metalsharp@users.noreply.github.com \
        commit -q -m "MetalSharp SPIRV-Cross (patches/spirv-cross/metalsharp.diff)"
fi
if want dxvk && [ ! -d "$WS/sources/dxvk/.git" ]; then
    git clone --branch "$DXVK_TAG" --depth 1 --recurse-submodules --shallow-submodules \
        https://github.com/doitsujin/dxvk.git "$WS/sources/dxvk"
    git -C "$WS/sources/dxvk" checkout -b metalsharp-dxvk
    git -C "$WS/sources/dxvk" -c user.name=MetalSharp -c user.email=metalsharp@users.noreply.github.com \
        am "$WS"/patches/dxvk/*.patch
fi
# NVIDIA's NVAPI headers (MIT), for the NGX core and NVAPI in ngx/
if want nvapi && [ ! -d "$WS/sources/nvapi/.git" ]; then
    git clone https://github.com/NVIDIA/nvapi.git "$WS/sources/nvapi"
    git -C "$WS/sources/nvapi" checkout -q d08488fcc82eef313b0464db37d2955709691e94
fi
# Microsoft's DirectXShaderCompiler (NCSA): its dxilconv, built as libdxbcconverter.dylib. The SPIR-V submodules are
# only for DXC's configure step; nothing of them is built.
if want dxc && [ ! -d "$WS/sources/DirectXShaderCompiler/.git" ]; then
    git clone --depth 1 --branch "$DXC_TAG" https://github.com/microsoft/DirectXShaderCompiler.git "$WS/sources/DirectXShaderCompiler"
    git -C "$WS/sources/DirectXShaderCompiler" submodule update --init --depth 1 \
        external/DirectX-Headers external/SPIRV-Headers external/SPIRV-Tools
    git -C "$WS/sources/DirectXShaderCompiler" apply "$WS/patches/dxc/metalsharp-dxilconv.diff"
fi
echo "sources ready; build MoltenVK with scripts/build-moltenvk.sh, the DXBC converter with scripts/build-dxbcconverter.sh, DXVK with scripts/build-dxvk.sh, the NGX core and NVAPI with scripts/build-ngx-arm64x.sh"
