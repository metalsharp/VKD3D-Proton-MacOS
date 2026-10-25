#!/bin/bash
# VKD3D-Proton-MacOS workspace environment.
# Usage: source scripts/env.sh
WS="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export WS
# An llvm-mingw set by the caller (CI downloads one) is kept.
export LLVM_MINGW="${LLVM_MINGW:-$WS/toolchain/llvm-mingw-20260616-ucrt-macos-universal}"
export PATH="$LLVM_MINGW/bin:/opt/homebrew/bin:$PATH"
# Xcode/Metal toolchain. Override XCODE_DEVELOPER_DIR for another install.
# The default is only taken where it exists, so another machine keeps its selected Xcode.
export XCODE_DEVELOPER_DIR="${XCODE_DEVELOPER_DIR:-/Users/averyfelts/Downloads/Xcode-beta.app/Contents/Developer}"
if [ -z "${DEVELOPER_DIR:-}" ] && [ -d "$XCODE_DEVELOPER_DIR" ]; then
    export DEVELOPER_DIR="$XCODE_DEVELOPER_DIR"
fi
# macOS 14 is the compatibility floor; Metal 3 is selected at runtime on it.
export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-14.0}"
# Generic Wine launcher. Override WINE_BIN for a custom Wine build.
export WINE_BIN="${WINE_BIN:-$(command -v wine 2>/dev/null || true)}"
export WINEPREFIX="${WINEPREFIX:-$WS/artifacts/prefix}"
# Optional directory containing Wine's x86_64 Unix libraries.
export WINE_UNIX_LIB="${WINE_UNIX_LIB:-}"
# Build outputs
export VKD3D_BUILD="$WS/artifacts/build/vkd3d-proton-build"
export MVK_PACKAGE="$WS/sources/MoltenVK/Package/Release/MoltenVK/dynamic/dylib/macOS"
echo "[env] WS=$WS"
echo "[env] llvm-mingw clang: $($LLVM_MINGW/bin/x86_64-w64-mingw32-clang --version 2>/dev/null | head -1)"
echo "[env] DEVELOPER_DIR=${DEVELOPER_DIR:-(selected Xcode)}"
echo "[env] MACOSX_DEPLOYMENT_TARGET=$MACOSX_DEPLOYMENT_TARGET"
