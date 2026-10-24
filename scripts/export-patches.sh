#!/bin/bash
# Export sources/MoltenVK and its External/SPIRV-Cross work, and sources/dxvk's, into patches/ (see fetch-sources.sh).
set -e
WS="$(cd "$(dirname "$0")/.." && pwd)"
cd "$WS/sources/MoltenVK"
git diff 5189f8de..HEAD -- . ':!External' > "$WS/patches/moltenvk/metalsharp-vkmt.diff"
git log --format='%h %s' 5189f8de..HEAD > "$WS/patches/moltenvk/metalsharp-vkmt.log"
cd External/SPIRV-Cross
rm -f "$WS"/patches/spirv-cross/*.patch
git format-patch -q c1af5739..HEAD -o "$WS/patches/spirv-cross"
ls "$WS/patches/spirv-cross"
cd "$WS/sources/dxvk"
mkdir -p "$WS/patches/dxvk"
rm -f "$WS"/patches/dxvk/*.patch
git format-patch -q v2.7.1..HEAD -o "$WS/patches/dxvk"
ls "$WS/patches/dxvk"
