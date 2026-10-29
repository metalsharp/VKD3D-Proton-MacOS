#!/bin/bash
# Recreate sources/ from upstream and this repository's patches:
#   sources/vkd3d-proton  upstream VKD3D-Proton + patches/vkd3d-proton-moltenvk.patch
#   sources/MoltenVK      upstream MoltenVK + patches/moltenvk/metalsharp-vkmt.diff
#                         (dttdrv's ray tracing, mesh shader and fix branches, then MetalSharp's work),
#                         with External/SPIRV-Cross at the dttdrv revision + patches/spirv-cross/metalsharp.diff
#   sources/dxvk          metalsharp/DXVK-MacOS v3.1.1-macos.1 (DXVK 3.1.1 on MoltenVK) with its dxbc-spirv patch
#                         applied, for both lanes: built for x86_64 (scripts/build-dxvk.sh) and as ARM64X
#                         (scripts/build-dxvk-arm64x.sh)
#   scripts/fetch-sources.sh [vkd3d-proton] [moltenvk] [dxvk]   (default: all three)
set -e
WS="$(cd "$(dirname "$0")/.." && pwd)"
VKD3D_REV=ef9a02e873715c9e2da0d6db8a236aca8380b303
MOLTENVK_REV=5189f8de
DXVK_REV=2c81c3c6cead6cc016dae804739b77740418bcaa   # metalsharp/DXVK-MacOS v3.1.1-macos.1
mkdir -p "$WS/sources"
WANT=" ${*:-vkd3d-proton moltenvk dxvk} "
WANT="${WANT/ dxvk-arm64x / dxvk }"   # the arm64 lane's old name for the same source
want() { [[ "$WANT" == *" $1 "* ]]; }

if want vkd3d-proton && [ ! -d "$WS/sources/vkd3d-proton/.git" ]; then
    git clone https://github.com/HansKristian-Work/vkd3d-proton.git "$WS/sources/vkd3d-proton"
    git -C "$WS/sources/vkd3d-proton" checkout -b metalsharp-moltenvk "$VKD3D_REV"
    git -C "$WS/sources/vkd3d-proton" submodule update --init --recursive
    git -C "$WS/sources/vkd3d-proton" am "$WS/patches/vkd3d-proton-moltenvk.patch"
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
    git clone https://github.com/metalsharp/DXVK-MacOS.git "$WS/sources/dxvk"
    git -C "$WS/sources/dxvk" checkout -b metalsharp-dxvk "$DXVK_REV"
    git -C "$WS/sources/dxvk" submodule update --init --recursive
    git -C "$WS/sources/dxvk/subprojects/dxbc-spirv" apply ../../patches/dxbc-spirv-moltenvk.patch
    # DXVK's version is `git describe --dirty=+`: the patched submodule would make it v3.1.1-macos.1+
    git -C "$WS/sources/dxvk" config submodule.subprojects/dxbc-spirv.ignore all
fi
echo "sources ready; build MoltenVK with scripts/build-moltenvk.sh, DXVK with scripts/build-dxvk.sh (x86_64 lane)"
echo "and scripts/build-dxvk-arm64x.sh (arm64 lane)"
