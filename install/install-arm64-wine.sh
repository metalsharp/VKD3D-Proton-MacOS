#!/bin/bash
# Installs VKD3D-Proton macOS into a prefix of an arm64 Wine with ARM64EC (MetalSharp Wine, or another arm64 build
# that runs x86_64 programs with FEX): the ARM64X d3d12.dll and d3d12core.dll, which arm64 programs and x86_64
# programs (as ARM64EC) both load, metalsharp/DXVK-MacOS's dxgi.dll, d3d11.dll and d3d10core.dll, also ARM64X, which
# x86_64 programs present VKD3D-Proton's swap chains with, and MoltenVK. This is MetalSharp Wine's VKD3D-Proton lane:
# for x86_64 programs.
#   install-arm64-wine.sh [--prefix DIR] [--wine PATH] [--release TAG | --packages DIR] [--uninstall]
# (install-common.sh describes the options)
LANE=arm64
source "$(cd "$(dirname "$0")" && pwd)/install-common.sh"
