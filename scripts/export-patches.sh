#!/bin/bash
# Export sources/vkd3d-proton, sources/MoltenVK and its External/SPIRV-Cross work, and sources/dxvk's, into patches/
# (see fetch-sources.sh).
set -e
WS="$(cd "$(dirname "$0")/.." && pwd)"
# VKD3D-Proton: the commit series on the upstream revision fetch-sources.sh pins, applied there with git am
git -C "$WS/sources/vkd3d-proton" format-patch -q --stdout ef9a02e873715c9e2da0d6db8a236aca8380b303..HEAD \
    > "$WS/patches/vkd3d-proton-moltenvk.patch"
cd "$WS/sources/MoltenVK"
git diff 5189f8de..HEAD -- . ':!External' > "$WS/patches/moltenvk/metalsharp-vkmt.diff"
git log --format='%h %s' 5189f8de..HEAD > "$WS/patches/moltenvk/metalsharp-vkmt.log"
cd External/SPIRV-Cross
# One cumulative diff on the dttdrv revision MoltenVK pins: the history since then has merges,
# which format-patch would flatten into a series that no longer applies in order.
rm -f "$WS"/patches/spirv-cross/*.patch
git diff c1af5739..HEAD > "$WS/patches/spirv-cross/metalsharp.diff"
git log --format='%h %s' c1af5739..HEAD > "$WS/patches/spirv-cross/metalsharp.log"
ls "$WS/patches/spirv-cross"
cd "$WS/sources/dxvk"
mkdir -p "$WS/patches/dxvk"
rm -f "$WS"/patches/dxvk/*.patch
git format-patch -q v2.7.1..HEAD -o "$WS/patches/dxvk"
ls "$WS/patches/dxvk"
