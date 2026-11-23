#!/bin/bash
# smoke.sh — DXVK-macOS smoke harness (P0.9)
# Builds Tier-1 smoke EXEs (mingw), then runs them under MetalSharp Wine 11.17
# with the built DXVK PE DLLs + bundled MoltenVK. All artifacts stay on the
# external scratch drive; nothing is written to /tmp or the internal disk.
#
# Usage:
#   tools/smoke.sh build                 # build smoke EXEs + DXVK lanes (if needed)
#   tools/smoke.sh run  [lane] [frames]  # lane: d3d9|d3d8|d3d11 (default all), default 300 frames
#   tools/smoke.sh soak  [lane] [minutes]
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
SCRATCH="${DXVK_SMOKE_SCRATCH:-/Volumes/AverySSD/scratch-p012}"
BUILD="$SCRATCH/build"
RUN="$SCRATCH/smoke-runs"
SMOKE="$REPO/tools/smoke"
WINE_BIN="${METALSHARP_WINE:-/Volumes/AverySSD/rainworld-mono-patch/runtime-bundle-original/runtime/wine/bin/metalsharp-wine}"
WINE_LIBS="$(dirname "$WINE_BIN")/../lib"
RUNTIME_ROOT="$(cd "$(dirname "$WINE_BIN")/.." && pwd)"
mkdir -p "$BUILD" "$RUN"

log() { echo "[smoke] $*"; }
die() { echo "[smoke] FATAL: $*" >&2; exit 1; }

build_exes() {
  log "building Tier-1 smoke EXEs with mingw..."
  local cc64=x86_64-w64-mingw32-gcc cc32=i686-w64-mingw32-gcc
  mkdir -p "$BUILD/smoke"
  # d3d9
  $cc64 -O2 "$SMOKE/d3d9_smoke.c" -o "$BUILD/smoke/d3d9_smoke.exe" -ld3d9 -lgdi32 -luser32
  $cc32 -O2 "$SMOKE/d3d9_smoke.c" -o "$BUILD/smoke/d3d9_smoke32.exe" -ld3d9 -lgdi32 -luser32
  # d3d11
  $cc64 -O2 "$SMOKE/d3d11_smoke.c" -o "$BUILD/smoke/d3d11_smoke.exe" -ld3d11 -ldxgi -ld3dcompiler_47 -luuid -lgdi32 -luser32
  $cc32 -O2 "$SMOKE/d3d11_smoke.c" -o "$BUILD/smoke/d3d11_smoke32.exe" -ld3d11 -ldxgi -ld3dcompiler_47 -luuid -lgdi32 -luser32
  # d3d8
  if echo '#include <d3d8.h>' | $cc64 -x c - -fsyntax-only -ld3d8 2>/dev/null; then
    $cc64 -O2 "$SMOKE/d3d8_smoke.c" -o "$BUILD/smoke/d3d8_smoke.exe" -lgdi32 -luser32
    $cc32 -O2 "$SMOKE/d3d8_smoke.c" -o "$BUILD/smoke/d3d8_smoke32.exe" -lgdi32 -luser32
    log "d3d8 smoke built"
  else
    log "NOTE: d3d8 headers not available in mingw — d3d8 lane skipped"
  fi
  log "smoke EXEs ready in $BUILD/smoke"
}

stage_dlls() {
  # stage DXVK PE DLLs next to the EXEs
  local dll64
  dll64="$(ls "$BUILD"/stage/x86_64/bin/*.dll 2>/dev/null | head -1 || true)"
  local dll64_dir
  dll64_dir="$(dirname "${dll64:-none}")"
  [ "$dll64_dir" != "none" ] || die "no built DLLs under $BUILD/stage — build DXVK first (Phase 1 build step)"
  cp "$dll64_dir"/*.dll "$BUILD/smoke/" 2>/dev/null || true
}

run_one() {
  local exe="$1" frames="$2"
  local name; name="$(basename "$exe" .exe)"
  local out="$RUN/${name}-$(date +%Y%m%d-%H%M%S)"
  mkdir -p "$out"
  log "running $name ($frames frames) -> $out"
  export WINEPREFIX="$SCRATCH/wineprefix"
  export WINEDEBUG=-all
  export WINEDLLOVERRIDES="d3d8,d3d9,d3d10core,d3d11,dxgi=n,b"
  export VK_ICD_FILENAMES="$RUNTIME_ROOT/wine/etc/vulkan/icd.d/MoltenVK_icd.json"
  export DYLD_LIBRARY_PATH="$RUNTIME_ROOT/wine/lib/moltenvk-vkmt:$RUNTIME_ROOT/wine/lib"
  export DXVK_LOG_PATH="$out"
  export DXVK_LOG_LEVEL=info
  local start_ms end_ms
  start_ms=$(python3 -c 'import time;print(int(time.time()*1000))')
  if ! ( cd "$BUILD/smoke" && timeout 300 "$WINE_BIN" "$exe" "$frames" ) >"$out/stdout.log" 2>"$out/stderr.log"; then
    log "RESULT $name: FAIL (nonzero exit or timeout)"
    return 1
  fi
  end_ms=$(python3 -c 'import time;print(int(time.time()*1000))')
  if grep -qE "ERR>" "$out"/*.log 2>/dev/null; then
    log "RESULT $name: FAIL (DXVK ERR lines present)"
    return 1
  fi
  log "RESULT $name: PASS ($((end_ms-start_ms)) ms wall)"
}

mode="${1:-run}"
case "$mode" in
  build) build_exes ;;
  run)
    build_exes
    stage_dlls || true
    frames="${3:-300}"
    lanes="${2:-all}"
    rc=0
    for exe in "$BUILD/smoke"/*.exe; do
      base="$(basename "$exe")"
      if [ "$lanes" != "all" ] && [[ "$base" != d3d*_smoke* ]]; then continue; fi
      if [ "$lanes" != "all" ] && [[ "$base" != ${lanes}_smoke* ]]; then continue; fi
      run_one "$base" "$frames" || rc=1
    done
    exit $rc ;;
  soak)
    log "soak mode: repeat run until minutes elapsed"
    build_exes; stage_dlls || true
    minutes="${3:-30}"; lane="${2:-d3d9}"
    end=$(( $(date +%s) + minutes*60 ))
    fails=0; runs=0
    while [ "$(date +%s)" -lt "$end" ]; do
      for exe in "$BUILD/smoke/${lane}"_smoke*.exe; do
        runs=$((runs+1)); run_one "$(basename "$exe")" 5000 || fails=$((fails+1))
      done
    done
    log "SOAK DONE: $runs runs, $fails failures"
    [ "$fails" -eq 0 ] ;;
  *) die "unknown mode: $mode (use build|run|soak)" ;;
esac
