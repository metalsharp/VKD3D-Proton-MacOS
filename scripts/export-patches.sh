#!/bin/bash
# Export sources/MoltenVK and its External/SPIRV-Cross work into patches/ (see fetch-sources.sh).
set -e
WS="$(cd "$(dirname "$0")/.." && pwd)"
cd "$WS/sources/MoltenVK"
git diff 5189f8de..HEAD -- . ':!External' > "$WS/patches/moltenvk/metalsharp-vkmt.diff"
git log --format='%h %s' 5189f8de..HEAD > "$WS/patches/moltenvk/metalsharp-vkmt.log"
cd External/SPIRV-Cross
rm -f "$WS"/patches/spirv-cross/*.patch
git format-patch -q c1af5739..HEAD -o "$WS/patches/spirv-cross"
ls "$WS/patches/spirv-cross"
