#!/bin/zsh
# d3d12test.sh <name> <dll dir|runtime> <arch: aarch64|x86_64> [test names...]
# Runs tests/d3d12's d3d12test with the given d3d12.dll + d3d12core.dll (or the runtime's VKD3D-Proton lane), DXVK's
# DXGI from the runtime's lane, and the runtime's MoltenVK, in a directory of its own; output in <name>.log there.
name=$1; dlls=$2; arch=$3; shift 3
WS=/Volumes/AverySSD/MetalSharp-Wine-Arm64-FEX/src/vkd3d-proton
R=$HOME/.metalsharp/runtime/MetalSharp-Wine.app/Contents
LANE=$R/lib/vkd3d-proton/x86_64-windows
D=$WS/artifacts/run/$name
rm -rf $D; mkdir -p $D
cp $WS/artifacts/build/d3d12test/d3d12test-$arch.exe $D/
ov="d3d12,d3d12core=n"
# DXVK's DXGI is x86_64 only: native arm64 programs use Wine's.
if [ $arch = x86_64 ]; then cp $LANE/dxgi.dll $LANE/d3d11.dll $LANE/d3d10core.dll $D/; ov="d3d12,d3d12core,dxgi,d3d11,d3d10core=n"; fi
if [ $arch = x86_64 ]; then
  # x86_64 programs run the lane as MetalSharp Wine does (GRAPHICS_BACKEND=vkd3d): the given DLLs go into it.
  rm -f $D/dxgi.dll $D/d3d11.dll $D/d3d10core.dll; ov=""; export GRAPHICS_BACKEND=vkd3d
  if [ "$dlls" != runtime ]; then
    cp $dlls/d3d12.dll $dlls/d3d12core.dll $LANE/
    # The loader's builtin path wants Wine's builtin signature (MetalSharp-Wine scripts/stage-vkd3d-proton.sh).
    for f in $LANE/d3d12.dll $LANE/d3d12core.dll; do
      python3 -c 'import sys; f=open(sys.argv[1],"r+b"); f.seek(64); f.write(b"Wine builtin DLL".ljust(32,b"\0"))' $f
    done
  fi
elif [ "$dlls" = runtime ]; then cp $LANE/d3d12.dll $LANE/d3d12core.dll $D/; else cp $dlls/d3d12.dll $dlls/d3d12core.dll $D/; fi
cd $D
WINEPREFIX=${WINEPREFIX:-/Volumes/AverySSD/UnrealEngine/test-prefix} WINEDEBUG=${WINEDEBUG:--all} WINEDLLOVERRIDES="${ov:+$ov;}winedbg.exe=d" \
  VKD3D_SHADER_CACHE_PATH=0 timeout ${TIMEOUT:-900} $R/bin/wine d3d12test-$arch.exe "$@" > $name.log 2>&1
echo "rc=$? $(tail -1 $name.log) -> $D/$name.log"
