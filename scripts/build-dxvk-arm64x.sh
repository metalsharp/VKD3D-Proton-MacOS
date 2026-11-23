#!/bin/bash
# DXVK's ARM64X build: scripts/build-dxvk.sh arm64x (kept for the scripts that call it by this name).
#   LLVM_MINGW=<llvm-mingw> scripts/build-dxvk-arm64x.sh [out-dir]
exec bash "$(dirname "$0")/build-dxvk.sh" arm64x "$@"
