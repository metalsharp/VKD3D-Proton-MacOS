#!/bin/bash
# Fetch Apple's Metal Shader Converter runtime library into artifacts/metal-shaderconverter/, for the MoltenVK
# package. MoltenVK loads it (from next to libMoltenVK.dylib) to convert DXIL compute shaders for VKD3D-Proton
# (METALSHARP_MSC=1). Apple's own download needs an Apple ID, so this repository hosts the unmodified,
# Apple-signed files as the release "deps-metal-shaderconverter-4.0b3", which Apple's license allows
# (section 2B: "install, use, test, and distribute the Dynamic Libraries for the sole purpose of shader
# conversion"). Each file is checked against its SHA-256 below.
#   scripts/fetch-metal-shaderconverter.sh
set -euo pipefail
WS="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$WS/artifacts/metal-shaderconverter"
URL="https://github.com/metalsharp/VKD3D-Proton-MacOS/releases/download/deps-metal-shaderconverter-4.0b3"
FILES=(
    "libmetalirconverter.dylib a41296b0da21704678511a6e7fdf8aeae1cb732391eaa001e7433cfebee79f8a"
    "LICENSE.metal-shaderconverter.rtf 20108dfe30663a2c6c717d9c5ca92486d9a4682c85c8ab344b20470feac8d0c3"
    "ACKNOWLEDGEMENTS.metal-shaderconverter.rtf 01ed5de80de6e1ca7c074ef602d867ef0bdbf0be3abc97adb7199c1ad380c3ba"
)
mkdir -p "$OUT"
for entry in "${FILES[@]}"; do
    name="${entry%% *}"
    sum="${entry##* }"
    if [ ! -f "$OUT/$name" ] || [ "$(shasum -a 256 "$OUT/$name" | cut -d' ' -f1)" != "$sum" ]; then
        curl -sfL --retry 3 -o "$OUT/$name.tmp" "$URL/$name"
        mv "$OUT/$name.tmp" "$OUT/$name"
    fi
    got="$(shasum -a 256 "$OUT/$name" | cut -d' ' -f1)"
    [ "$got" = "$sum" ] || { echo "$name: SHA-256 $got, expected $sum" >&2; exit 1; }
done
# Shipped as Apple signed it: its license doesn't allow modifying it, so it is never re-signed.
signature="$(codesign -dv --verbose=2 "$OUT/libmetalirconverter.dylib" 2>&1 || true)"
grep -qx 'Authority=Software Signing' <<<"$signature" ||
    { echo "libmetalirconverter.dylib is not signed by Apple" >&2; exit 1; }
echo "metal-shaderconverter: $OUT"
