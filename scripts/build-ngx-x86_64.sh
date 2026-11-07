#!/bin/sh
# scripts/build-ngx-x86_64.sh: VKD3D-Proton-MacOS's NGX core and NVAPI (ngx/) as x86_64 PE, for an x86_64 Wine (Rosetta), into
# artifacts/build/ngx/x86_64-windows. Needs LLVM_MINGW (an llvm-mingw) and sources/vkd3d-proton and
# sources/nvapi (scripts/fetch-sources.sh).
set -e
WS="$(cd "$(dirname "$0")/.." && pwd)"
LLVM_MINGW=${LLVM_MINGW:?set LLVM_MINGW to an llvm-mingw}
OUT="$WS/artifacts/build/ngx-x86_64-build"
STAGE="$WS/artifacts/build/ngx/x86_64-windows"
CROSS="$WS/artifacts/ngx-cross-x86_64.txt"
export PATH="$LLVM_MINGW/bin:$PATH"

[ -f "$WS/sources/nvapi/nvapi.h" ] || { echo "sources/nvapi missing (scripts/fetch-sources.sh nvapi)"; exit 1; }
mkdir -p "$(dirname "$CROSS")"
cat > "$CROSS" <<'CROSSFILE'
[binaries]
cpp = 'x86_64-w64-mingw32-clang++'
ar = 'llvm-ar'
strip = 'llvm-strip'


[host_machine]
system = 'windows'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'

[properties]
needs_exe_wrapper = true
CROSSFILE

rm -rf "$OUT"
meson setup --cross-file "$CROSS" --buildtype release --strip "$OUT" "$WS/ngx"
ninja -C "$OUT"
mkdir -p "$STAGE"
cp "$OUT/nvngx.dll" "$OUT/nvapi64.dll" "$STAGE/"

# The x86_64 MetalSharp Wine loads both as Wine builtins (lib/wine/x86_64-windows, from which wineboot puts nvngx.dll in
# every prefix's System32), so they carry Wine's builtin marker. It is in the signed bytes, so it goes in first.
python3 -I - "$STAGE/nvngx.dll" "$STAGE/nvapi64.dll" <<'PY'
import struct, sys
for path in sys.argv[1:]:
    with open(path, "r+b") as f:
        header = f.read(64)
        assert header[:2] == b"MZ" and struct.unpack_from("<I", header, 60)[0] >= 96, path
        f.seek(64)
        f.write(b"Wine builtin DLL".ljust(32, b"\0"))
PY

# NGX loads only a core whose Authenticode signature chains to a trusted root: nvngx.dll is signed with the Mullion
# NGX key, whose certificate (ngx/signing/mullion-ngx.pem) the lane's DXGI trusts in the prefix (ngx/signing/README.md).
# The key: NGX_SIGNING_KEY_FILE, else ~/.metalsharp/ngx-signing/key.pem; without one nvngx.dll stays unsigned.
KEY="${NGX_SIGNING_KEY_FILE:-$HOME/.metalsharp/ngx-signing/key.pem}"
CERT="$WS/ngx/signing/mullion-ngx.pem"
if [ -f "$KEY" ] && command -v osslsigncode >/dev/null; then
    osslsigncode sign -certs "$CERT" -key "$KEY" -n "Mullion NGX" -h sha256 \
        -in "$STAGE/nvngx.dll" -out "$STAGE/nvngx.dll.signed"
    mv "$STAGE/nvngx.dll.signed" "$STAGE/nvngx.dll"
    osslsigncode verify -CAfile "$CERT" -in "$STAGE/nvngx.dll" >/dev/null
    echo "signed nvngx.dll"
else
    echo "warning: no NGX signing key (or osslsigncode): nvngx.dll is unsigned, and NGX will not load it" >&2
fi
file "$STAGE"/*.dll
