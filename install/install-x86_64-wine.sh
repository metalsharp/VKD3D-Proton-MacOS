#!/bin/bash
# Installs VKD3D-Proton macOS into a prefix of an x86_64 Wine (Intel, or under Rosetta 2: CrossOver, Wine builds for
# macOS): the x86_64 d3d12.dll and d3d12core.dll, DXVK's dxgi.dll, d3d11.dll and d3d10core.dll, and MoltenVK.
#   install-x86_64-wine.sh [--prefix DIR] [--wine PATH] [--release TAG | --packages DIR] [--uninstall]
# (install-common.sh describes the options)
LANE=x86_64
source "$(cd "$(dirname "$0")" && pwd)/install-common.sh"
