#!/bin/bash
# Build MetalSharp's MoltenVK (universal dylib) from sources/MoltenVK (scripts/fetch-sources.sh).
#   scripts/build-moltenvk.sh [--externals]
# --externals rebuilds SPIRV-Cross and SPIRV-Tools from sources/MoltenVK/External (needed after
# changing External/SPIRV-Cross). Xcode's derived data lives in $TMPDIR: its build database
# fails with disk I/O errors on some external volumes. Output: artifacts/mvk-new.
set -e
WS="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$WS/sources/MoltenVK"
DD="${TMPDIR:-/tmp}/metalsharp-moltenvk-dd"
OUT="$WS/artifacts/mvk-new"
cd "$SRC"

if [ "$1" = "--externals" ]; then
    xcodebuild -project ExternalDependencies.xcodeproj -scheme ExternalDependencies-macOS \
        -destination "generic/platform=macOS" -configuration Release \
        -enableAddressSanitizer NO -enableThreadSanitizer NO -enableUndefinedBehaviorSanitizer NO \
        -derivedDataPath "$DD-ext" MACOSX_DEPLOYMENT_TARGET=15.0 build
    PROJECT_DIR="." CONFIGURATION=Release SKIP_PACKAGING="" bash -c '
        . ./Scripts/create_ext_lib_xcframeworks.sh
        . ./Scripts/package_ext_libs_finish.sh'
fi

xcodebuild build -project MoltenVKPackaging.xcodeproj -scheme "MoltenVK Package (macOS only)" \
    -destination "generic/platform=macOS" -configuration Release MACOSX_DEPLOYMENT_TARGET=15.0 \
    -derivedDataPath "$DD"
mkdir -p "$OUT"
cp Package/Release/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib "$OUT/"
sed 's#"library_path": *"[^"]*"#"library_path": "./libMoltenVK.dylib"#' \
    Package/Release/MoltenVK/dynamic/dylib/macOS/MoltenVK_icd.json > "$OUT/MoltenVK_icd.json"
shasum -a 256 "$OUT/libMoltenVK.dylib"
