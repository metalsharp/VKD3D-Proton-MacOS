#!/bin/bash
# Each DLL comes from the package's directory for it: dll_dir() below.
# Installs VKD3D-Proton macOS into a Wine prefix: the lane's DLLs into system32 (the ones they replace kept for
# --uninstall), native DLL overrides for them, and the universal MoltenVK with a launcher that points Wine's Vulkan
# loader at it. Sourced by install-x86_64-wine.sh and install-arm64-wine.sh, which set LANE.
#
#   install-<lane>-wine.sh [--prefix DIR] [--wine PATH] [--release TAG | --packages DIR] [--uninstall]
#
#   --prefix DIR     the Wine prefix (default: $WINEPREFIX, else ~/.wine)
#   --wine PATH      the wine binary that runs the prefix (default: $WINE, else wine on the PATH)
#   --release TAG    the release to download (default: the latest), from github.com/metalsharp/VKD3D-Proton-MacOS
#   --packages DIR   use packages already downloaded there (.tar.zst or extracted) instead of downloading
#   --uninstall      put back the DLLs the install replaced and remove its overrides and files
#
# Afterwards, run programs with <prefix>/vkd3d-proton-macos/wine (or source .../env.sh first), so Vulkan finds this
# MoltenVK. A wine that is a wrapper clearing Vulkan's variables (MetalSharp Wine's bin/wine) needs the binary it
# wraps instead: MetalSharp-Wine.app/Contents/MacOS/wine.
set -euo pipefail

REPO="metalsharp/VKD3D-Proton-MacOS"
case "$LANE" in
    x86_64) DLL_DIR=x86_64-windows; MACHINE="x86-64"; DLLS=(d3d12 d3d12core dxgi d3d11 d3d10core) ;;
    arm64)  DLL_DIR=aarch64-windows; MACHINE="Aarch64"; DLLS=(d3d12 d3d12core dxgi d3d11 d3d10core) ;;
    *) echo "unknown lane $LANE" >&2; exit 1 ;;
esac

PREFIX="${WINEPREFIX:-$HOME/.wine}"
WINE_BIN="${WINE:-$(command -v wine 2>/dev/null || true)}"
TAG=""
PACKAGES=""
UNINSTALL=0
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

usage() { sed -n '5,17p' "$SCRIPT_DIR/install-common.sh" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }
while [ $# -gt 0 ]; do
    case "$1" in
        --prefix) PREFIX="$2"; shift 2 ;;
        --wine) WINE_BIN="$2"; shift 2 ;;
        --release) TAG="$2"; shift 2 ;;
        --packages) PACKAGES="$2"; shift 2 ;;
        --uninstall) UNINSTALL=1; shift ;;
        -h|--help) usage 0 ;;
        *) echo "unknown option $1" >&2; usage 1 ;;
    esac
done

die() { echo "error: $*" >&2; exit 1; }
# The arm64 lane's D3D12 pair is in aarch64-windows and its DXVK DXGI and D3D10/11 in x86_64-windows, where MetalSharp
# Wine's VKD3D-Proton lane has them; all five are ARM64X, which x86_64 programs load as ARM64EC under FEX.
dll_dir() { case "$LANE:$1" in arm64:d3d12|arm64:d3d12core) echo aarch64-windows ;; *) echo x86_64-windows ;; esac; }
SYS32="$PREFIX/drive_c/windows/system32"
HOME_DIR="$PREFIX/vkd3d-proton-macos"
BACKUP="$HOME_DIR/backup"
[ -d "$SYS32" ] || die "$PREFIX is not a Wine prefix (no drive_c/windows/system32); run wineboot there first"
[ -n "$WINE_BIN" ] && [ -x "$WINE_BIN" ] || die "no wine binary: pass --wine PATH"
export WINEPREFIX="$PREFIX" WINEDEBUG=-all

# The prefix's own DLLs say which Wine made it: an x86_64 prefix needs the x86_64 lane, an arm64 one the ARM64X lane.
kind="$(file -b "$SYS32/kernel32.dll" 2>/dev/null || true)"
case "$kind" in
    *"$MACHINE"*) ;;
    *) die "$PREFIX holds ${kind:-no kernel32.dll}, not $MACHINE DLLs: use the other install script" ;;
esac

overrides() {   # overrides add|delete
    for dll in "${DLLS[@]}"; do
        if [ "$1" = add ]; then
            "$WINE_BIN" reg add 'HKEY_CURRENT_USER\Software\Wine\DllOverrides' /v "$dll" /d native /f >/dev/null
        else
            "$WINE_BIN" reg delete 'HKEY_CURRENT_USER\Software\Wine\DllOverrides' /v "$dll" /f >/dev/null 2>&1 || true
        fi
    done
}

if [ "$UNINSTALL" = 1 ]; then
    [ -d "$HOME_DIR" ] || die "VKD3D-Proton macOS is not installed in $PREFIX"
    for dll in "${DLLS[@]}"; do
        if [ -f "$BACKUP/$dll.dll" ]; then mv -f "$BACKUP/$dll.dll" "$SYS32/$dll.dll"; else rm -f "$SYS32/$dll.dll"; fi
    done
    overrides delete
    "$WINE_BIN" wineboot -u >/dev/null 2>&1 || true   # puts Wine's builtin DLLs back where none were kept
    rm -rf "$HOME_DIR"
    echo "removed VKD3D-Proton macOS from $PREFIX"
    exit 0
fi

# The packages: beside this script (it ships in the lane's package), in --packages, or downloaded.
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
find_package() {   # find_package <dir> <subdir that marks it>: prints the package's root
    local dir="$1" mark="$2" f
    for f in "$dir"/*/"$mark" "$dir/$mark"; do [ -e "$f" ] && { dirname "$f"; return 0; }; done
    for f in "$dir"/*.tar.zst; do
        [ -e "$f" ] || continue
        tar -xf "$f" -C "$WORK"
    done
    for f in "$WORK"/*/"$mark"; do [ -e "$f" ] && { dirname "$f"; return 0; }; done
    return 1
}
if [ -z "$PACKAGES" ] && [ ! -d "$SCRIPT_DIR/$DLL_DIR" ]; then
    command -v curl >/dev/null || die "curl is needed to download the release"
    if [ -z "$TAG" ]; then
        TAG="$(curl -fsSL "https://api.github.com/repos/$REPO/releases/latest" | sed -n 's/.*"tag_name": *"\([^"]*\)".*/\1/p' | head -1)"
        [ -n "$TAG" ] || die "can't find the latest release of $REPO"
    fi
    PACKAGES="$WORK/download"
    mkdir -p "$PACKAGES"
    for name in "vkd3d-proton-$TAG-$LANE" "moltenvk-$TAG-universal"; do
        echo "downloading $name.tar.zst ($TAG)"
        curl -fL --progress-bar -o "$PACKAGES/$name.tar.zst" \
            "https://github.com/$REPO/releases/download/$TAG/$name.tar.zst" || die "can't download $name.tar.zst"
    done
fi
if [ -d "$SCRIPT_DIR/$DLL_DIR" ]; then
    DLL_ROOT="$SCRIPT_DIR"
    MVK_ROOT="$(find_package "${PACKAGES:-$SCRIPT_DIR/..}" libMoltenVK.dylib)" ||
        die "no MoltenVK package beside $SCRIPT_DIR: pass --packages DIR with the moltenvk package"
else
    DLL_ROOT="$(find_package "$PACKAGES" "$DLL_DIR")" || die "no $LANE package in $PACKAGES"
    MVK_ROOT="$(find_package "$PACKAGES" libMoltenVK.dylib)" || die "no MoltenVK package in $PACKAGES"
fi
for root in "$DLL_ROOT" "$MVK_ROOT"; do
    [ -f "$root/SHA256SUMS" ] || die "$root has no SHA256SUMS"
    if ! (cd "$root" && shasum -a 256 -c --quiet SHA256SUMS); then die "checksums of $root don't match"; fi
done

mkdir -p "$BACKUP" "$HOME_DIR/moltenvk"
for dll in "${DLLS[@]}"; do
    src="$DLL_ROOT/$(dll_dir "$dll")/$dll.dll"
    [ -f "$src" ] || die "$(dll_dir "$dll")/$dll.dll is missing from the package"
    # what the prefix had first is kept once: a reinstall doesn't keep a previous install's DLL instead
    if [ -f "$SYS32/$dll.dll" ] && [ ! -f "$BACKUP/$dll.dll" ]; then cp -p "$SYS32/$dll.dll" "$BACKUP/$dll.dll"; fi
    cp -f "$src" "$SYS32/$dll.dll"
done
overrides add

cp -f "$MVK_ROOT/libMoltenVK.dylib" "$HOME_DIR/moltenvk/"
# Apple's Metal Shader Converter, which MoltenVK loads from beside itself to convert DXIL compute, vertex and pixel shaders for
# VKD3D-Proton (on by default, METALSHARP_MSC=0 turns it off; packages before it was added don't carry it)
if [ -f "$MVK_ROOT/libmetalirconverter.dylib" ]; then
    cp -f "$MVK_ROOT/libmetalirconverter.dylib" "$HOME_DIR/moltenvk/"
fi
# Wines that load MoltenVK as libvulkan.dylib (no Vulkan loader) find this one first through DYLD_LIBRARY_PATH;
# Wines with a loader find it through VK_DRIVER_FILES
ln -sf libMoltenVK.dylib "$HOME_DIR/moltenvk/libvulkan.dylib"
ln -sf libMoltenVK.dylib "$HOME_DIR/moltenvk/libvulkan.1.dylib"
sed "s#\"library_path\": *\"[^\"]*\"#\"library_path\": \"$HOME_DIR/moltenvk/libMoltenVK.dylib\"#" \
    "$MVK_ROOT/MoltenVK_icd.json" > "$HOME_DIR/moltenvk/MoltenVK_icd.json"

cat > "$HOME_DIR/env.sh" <<EOF
# source this before running $PREFIX's programs with VKD3D-Proton macOS
export WINEPREFIX="$PREFIX"
export VK_DRIVER_FILES="$HOME_DIR/moltenvk/MoltenVK_icd.json"
export VK_ICD_FILENAMES="\$VK_DRIVER_FILES"
export DYLD_LIBRARY_PATH="$HOME_DIR/moltenvk\${DYLD_LIBRARY_PATH:+:\$DYLD_LIBRARY_PATH}"
# With Wine's default debug channels on, VKD3D-Proton reads an unfilled driver ID and refuses the device
# ("Lacking support for single texel alignment"); set WINEDEBUG before sourcing this to debug anyway.
export WINEDEBUG="\${WINEDEBUG:--all}"
EOF
cat > "$HOME_DIR/wine" <<EOF
#!/bin/bash
# $WINE_BIN with VKD3D-Proton macOS's MoltenVK
source "$HOME_DIR/env.sh"
exec "$WINE_BIN" "\$@"
EOF
chmod +x "$HOME_DIR/wine"
echo "$LANE ${TAG:-package}" > "$HOME_DIR/VERSION"

echo "installed VKD3D-Proton macOS ($LANE${TAG:+, $TAG}) into $PREFIX: ${DLLS[*]}"
echo "run programs with: $HOME_DIR/wine program.exe"
