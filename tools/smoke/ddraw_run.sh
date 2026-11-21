#!/bin/bash
# ddraw_run.sh — runs tools/smoke/ddraw_draw.c (D3D7/6/5/3 through d7vk's ddraw.dll) under a MetalSharp Wine.
#
#   tools/smoke/ddraw_run.sh <wine app Contents dir> <ddraw.dll dir> [test|all] [dxvk|wine|routed|default]
#
# DDRAW_ARCH=i386 (default, as the games are) or x86_64 picks the test program's architecture; the DLL dir must
# hold the matching ddraw.dll from this repo: i386, or for x86_64 the x86_64 build on the x86_64 Wine and the
# ARM64X build on MetalSharp Wine (arm64). Wine's own ddraw.dll of that architecture is staged next to it as
# ddraw_.dll, which d7vk proxies DirectDraw to. "wine" runs the same test on Wine's ddraw alone, as a baseline.
# "routed" copies nothing and sets GRAPHICS_BACKEND=dxvk, for a MetalSharp Wine whose loader gives programs its
# lib/dxvk lane's ddraw.dll (and Wine's own as ddraw_.dll); "default" sets nothing.
# Everything is written under DDRAW_SMOKE_SCRATCH (default: the external drive).
set -euo pipefail

CONTENTS="$1"; DLLDIR="$2"; TEST="${3:-all}"; MODE="${4:-dxvk}"
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
SCRATCH="${DDRAW_SMOKE_SCRATCH:-/Volumes/AverySSD/d7vk-port-scope/run}"
ARCH="${DDRAW_ARCH:-i386}"
case "$ARCH" in
  i386)   CC="${CC32:-i686-w64-mingw32-clang}"; WINEDIR=i386-windows ;;
  x86_64) CC="${CC64:-x86_64-w64-mingw32-clang}"; WINEDIR=x86_64-windows ;;
  *) echo "DDRAW_ARCH is i386 or x86_64" >&2; exit 2 ;;
esac
WINE="$CONTENTS/MacOS/wine"
LANE="$(basename "$(dirname "$CONTENTS")" .app)"
STAGE="$SCRATCH/stage-$LANE-$ARCH-$MODE"
OUT="$SCRATCH/logs-$LANE-$ARCH-$MODE-$(date +%Y%m%d-%H%M%S)"

[ -x "$WINE" ] || { echo "no Wine at $WINE" >&2; exit 2; }
# MetalSharp Wine (arm64) runs x86_64 programs with its ARM64X builtins
[ -d "$CONTENTS/lib/wine/$WINEDIR" ] || WINEDIR=aarch64-windows
mkdir -p "$STAGE" "$OUT"
"$CC" -O2 "$REPO/tools/smoke/ddraw_draw.c" -o "$STAGE/ddraw_draw.exe" -lddraw -ldxguid -luuid -luser32
rm -f "$STAGE/ddraw.dll" "$STAGE/ddraw_.dll"
unset DYLD_LIBRARY_PATH DYLD_FALLBACK_LIBRARY_PATH WINEDLLPATH WINELOADER VK_ICD_FILENAMES VK_DRIVER_FILES \
  GRAPHICS_BACKEND WINEDLLOVERRIDES
case "$MODE" in
  dxvk)      # this repo's ddraw.dll next to the program, Wine's as ddraw_.dll
    cp "$DLLDIR/ddraw.dll" "$STAGE/ddraw.dll"
    cp "$CONTENTS/lib/wine/$WINEDIR/ddraw.dll" "$STAGE/ddraw_.dll"
    export WINEDLLOVERRIDES="ddraw=n,b" ;;
  wine)      export WINEDLLOVERRIDES="ddraw=b" ;;
  routed)    export GRAPHICS_BACKEND=dxvk ;;   # the Wine's lib/dxvk lane, found by its loader; DLLDIR is unused
  default) ;;                                  # whatever the Wine picks by itself
  *) echo "mode is dxvk, wine, routed or default" >&2; exit 2 ;;
esac
export WINEPREFIX="$SCRATCH/prefix-$LANE"
export WINESERVER="$CONTENTS/MacOS/wineserver"
export WINEDEBUG="${WINEDEBUG:--all}"
export DXVK_LOG_PATH="$OUT"
export DXVK_LOG_LEVEL="${DXVK_LOG_LEVEL:-info}"
[ -d "$WINEPREFIX" ] || "$WINE" wineboot -u >/dev/null 2>&1 || true

set +e
(cd "$STAGE" && timeout 180 "$WINE" ddraw_draw.exe "$TEST") >"$OUT/stdout.log" 2>"$OUT/stderr.log"
rc=$?
set -e
"$WINESERVER" -k >/dev/null 2>&1 || true
tr -d '\r' < "$OUT/stdout.log"
echo "exit $rc, logs in $OUT"
exit $rc
