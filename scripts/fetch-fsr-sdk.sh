#!/bin/bash
# AMD FSR SDK 2.4.0 ("Redstone", FSR Upscaling 4.1.2) into sources/fsr-sdk: the signed DX12 and Vulkan DLLs, the
# MIT-licensed FFX API headers and the SDK's license. Only the files these need are checked out (sparse), at the
# commit the v2.4.0 tag points to, and each DLL is checked against the SHA-256 recorded here.
#
# The DLLs are AMD's signed binaries. AMD's license allows redistributing them in binary form, unmodified, with its
# notices (sources/fsr-sdk/Kits/FidelityFX/docs/license.md), and forbids reverse engineering, decompiling or
# disassembling them: nothing here modifies, inspects or dumps them, and docs/fsr4.md says how to keep it that way.
#   scripts/fetch-fsr-sdk.sh
set -euo pipefail
WS="$(cd "$(dirname "$0")/.." && pwd)"
SDK="$WS/sources/fsr-sdk"
FSR_SDK_REPO=https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git
FSR_SDK_REV=7539ae8b6568ad29ed78a753404351daf3b5cca7   # v2.4.0
BIN=Kits/FidelityFX/signedbin
DLLS=(
    "e2191d3c0828be45eb8affd4e3ec0378f2293adc345b772a7170edcf3b021ecb  $BIN/amd_fidelityfx_loader_dx12.dll"
    "aad4fccccb29084a655f9a4f9f613bd22c8c03fb4a0f8840148cc6543b4ccace  $BIN/amd_fidelityfx_upscaler_dx12.dll"
    "e6843252f58b0341a0d80f6a20bc0d840643ee95bc48c22cac08745344577bb6  $BIN/amd_fidelityfx_framegeneration_dx12.dll"
    "38a56a01fb3565c0757d9f5c1e4e69de4b65e5f2e5769b323b200dc1f142bab9  $BIN/amd_fidelityfx_loader_vk.dll"
    "4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7  $BIN/amd_fidelityfx_upscaler_vk.dll"
)

if [ ! -d "$SDK/.git" ]; then
    git clone --filter=blob:none --no-checkout "$FSR_SDK_REPO" "$SDK"
    git -C "$SDK" sparse-checkout set --no-cone \
        /Kits/FidelityFX/api/include/ /Kits/FidelityFX/upscalers/include/ /Kits/FidelityFX/framegeneration/include/ \
        "/$BIN/" /Kits/FidelityFX/docs/license.md /Kits/FidelityFX/docs/techniques/super-resolution-ml.md
    git -C "$SDK" checkout -q "$FSR_SDK_REV"
fi
[ "$(git -C "$SDK" rev-parse HEAD)" = "$FSR_SDK_REV" ] || { echo "sources/fsr-sdk is not at $FSR_SDK_REV" >&2; exit 1; }
printf '%s\n' "${DLLS[@]}" | (cd "$SDK" && shasum -a 256 -c --quiet -) || { echo "FSR SDK DLL checksums don't match" >&2; exit 1; }
echo "FSR SDK 2.4.0 ready in $SDK"
