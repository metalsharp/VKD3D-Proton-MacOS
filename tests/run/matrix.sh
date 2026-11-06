#!/bin/zsh
# matrix.sh <tag> <dll dir>: the d3d12 suite on Metal 4 and Metal 3 (METALSHARP_METAL_VERSION=3), with the Metal Shader
# Converter bridge and without (METALSHARP_MSC=0), native arm64 and x86_64 (FEX, bridge on); one summary line each.
tag=$1; dlls=$2
WS=/Volumes/AverySSD/MetalSharp-Wine-Arm64-FEX/src/vkd3d-proton
export D3D12TEST_CACHE='Z:\Volumes\AverySSD\MetalSharp-Wine-Arm64-FEX\src\vkd3d-proton\artifacts\run\dxilcache'
for mv in 4 3; do
  for m in 1 0; do
    n=$tag-a64-m$mv-msc$m
    METALSHARP_METAL_VERSION=$mv METALSHARP_MSC=$m zsh $WS/tests/run/d3d12test.sh $n $dlls aarch64 >/dev/null
    echo "arm64  metal $mv msc $m: $(tail -1 $WS/artifacts/run/$n/$n.log) | $(grep '\] FAIL' $WS/artifacts/run/$n/$n.log | tr '\n' ' ')"
  done
  n=$tag-x64-m$mv-msc1
  METALSHARP_METAL_VERSION=$mv TIMEOUT=2400 zsh $WS/tests/run/d3d12test.sh $n $dlls x86_64 >/dev/null
  echo "x86_64 metal $mv msc 1: $(tail -1 $WS/artifacts/run/$n/$n.log) | $(grep '\] FAIL' $WS/artifacts/run/$n/$n.log | tr '\n' ' ')"
done
