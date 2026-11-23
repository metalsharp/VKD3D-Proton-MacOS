#!/bin/bash
# Build the DXVK-macOS capability probe (native macOS, vendored Vulkan headers).
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p bin
clang -O2 -Wall -Wextra -Wno-unused-parameter -Iinclude probe.c -o bin/probe
echo "[build] tools/probe/bin/probe ready"
