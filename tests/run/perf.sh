#!/bin/zsh
# perf.sh <label> <base|new> [perf_probe.py run options...]: stage the baseline (the runtime's VKD3D-Proton lane and
# MoltenVK as they were before this branch, from /Volumes/AverySSD/UnrealEngine/backup) or this build into the runtime,
# then run tests/perf's probe through the lane (x86_64 under FEX). JSON in artifacts/perf/<label>.json.
label=$1; which=$2; shift 2
WS=/Volumes/AverySSD/MetalSharp-Wine-Arm64-FEX/src/vkd3d-proton
R=$HOME/.metalsharp/runtime/MetalSharp-Wine.app/Contents
LANE=$R/lib/vkd3d-proton/x86_64-windows
B=/Volumes/AverySSD/UnrealEngine/backup
if [ $which = base ]; then
  cp $B/vkd3d-lane-pre-mscnative/d3d12.dll $B/vkd3d-lane-pre-mscnative/d3d12core.dll $LANE/
  cp $B/libMoltenVK.runtime-pre-mscnative.dylib $R/lib/libMoltenVK.dylib
else
  cp $WS/artifacts/build/vkd3d-proton/aarch64-windows/d3d12.dll $WS/artifacts/build/vkd3d-proton/aarch64-windows/d3d12core.dll $LANE/
  for f in $LANE/d3d12.dll $LANE/d3d12core.dll; do
    python3 -c 'import sys; f=open(sys.argv[1],"r+b"); f.seek(64); f.write(b"Wine builtin DLL".ljust(32,b"\0"))' $f
  done
  cp $WS/artifacts/mvk-new/libMoltenVK.dylib $R/lib/libMoltenVK.dylib
  cp $WS/artifacts/dxbcconverter/libdxbcconverter.dylib $R/lib/
fi
mkdir -p $WS/artifacts/perf
WINEPREFIX=${WINEPREFIX:-/Volumes/AverySSD/UnrealEngine/test-prefix} WINEDEBUG=-all \
  python3 $WS/tests/perf/perf_probe.py run --label $label --wine $R/bin/wine --out $WS/artifacts/perf/$label.json "$@"
