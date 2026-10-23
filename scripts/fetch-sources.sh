#!/bin/bash
# Recreate sources/ from upstream and this repository's patches:
#   sources/vkd3d-proton  upstream VKD3D-Proton + patches/vkd3d-proton-moltenvk.patch
#   sources/MoltenVK      upstream MoltenVK + patches/moltenvk/metalsharp-vkmt.diff
#                         (dttdrv's ray tracing, mesh shader and fix branches, then MetalSharp's work),
#                         with External/SPIRV-Cross at the dttdrv revision + patches/spirv-cross/*.patch
#   scripts/fetch-sources.sh
set -e
WS="$(cd "$(dirname "$0")/.." && pwd)"
VKD3D_REV=ef9a02e873715c9e2da0d6db8a236aca8380b303
MOLTENVK_REV=5189f8de
mkdir -p "$WS/sources"

if [ ! -d "$WS/sources/vkd3d-proton/.git" ]; then
    git clone https://github.com/HansKristian-Work/vkd3d-proton.git "$WS/sources/vkd3d-proton"
    git -C "$WS/sources/vkd3d-proton" checkout -b metalsharp-moltenvk "$VKD3D_REV"
    git -C "$WS/sources/vkd3d-proton" submodule update --init --recursive
    git -C "$WS/sources/vkd3d-proton" am "$WS/patches/vkd3d-proton-moltenvk.patch"
fi

if [ ! -d "$WS/sources/MoltenVK/.git" ]; then
    git clone https://github.com/KhronosGroup/MoltenVK.git "$WS/sources/MoltenVK"
    git -C "$WS/sources/MoltenVK" checkout -b metalsharp-vkmt "$MOLTENVK_REV"
    git -C "$WS/sources/MoltenVK" apply --index "$WS/patches/moltenvk/metalsharp-vkmt.diff"
    git -C "$WS/sources/MoltenVK" -c user.name=MetalSharp -c user.email=metalsharp@users.noreply.github.com \
        commit -q -m "MetalSharp MoltenVK (patches/moltenvk/metalsharp-vkmt.diff)"
    (cd "$WS/sources/MoltenVK" && ./fetchDependencies --none)
    git -C "$WS/sources/MoltenVK/External/SPIRV-Cross" checkout -b metalsharp-robust
    git -C "$WS/sources/MoltenVK/External/SPIRV-Cross" -c user.name=MetalSharp -c user.email=metalsharp@users.noreply.github.com \
        am "$WS"/patches/spirv-cross/*.patch
fi
echo "sources ready; build MoltenVK with scripts/build-moltenvk.sh"
