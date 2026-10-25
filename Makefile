SHELL := /bin/bash

ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
LLVM_MINGW ?= $(ROOT)/toolchain/llvm-mingw-20260616-ucrt-macos-universal
BUILD_DIR ?= $(ROOT)/artifacts/build
STAGE_DIR ?= $(ROOT)/artifacts/stage-dxr
WINE_RUNNER ?= /tmp/run-probe.sh
PACKAGE ?= $(ROOT)/vkd3d-proton-macos.tar.zst
XCODE16_DEVELOPER_DIR ?= /Users/averyfelts/Downloads/Xcode.app/Contents/Developer
DXVK_SRC ?= $(ROOT)/sources/dxvk-macos
DXVK_COMMIT ?= 8f1e28deed3ad30802f7e1bdff428ec14e6e7817

.PHONY: help tools docs-check env vkd3d moltenvk metal3 build flprobe stage ladder \
	dxgi dxgi-probe dxgi-test dxgi-present-probe dxgi-present-test \
	dxgi-lifecycle-probe dxgi-lifecycle-test dxgi-formats-probe \
	dxgi-formats-test dxgi-sync-probe dxgi-sync-test package test clean

help:
	@printf '%s\n' \
	  'make tools       verify required host tools and source clones' \
	  'make docs-check  validate documentation, links, and whitespace' \
	  'make vkd3d       build the x86_64 D3D12 DLL pair' \
	  'make dxgi        build the pinned DXVK macOS DXGI provider' \
	  'make dxgi-probe  compile the DXGI adapter identity probe' \
	  'make dxgi-test   run DXGI-1 identity, repeatability, and regression gates' \
	  'make dxgi-present-probe  build the DXGI-2 windowed presentation probe' \
	  'make dxgi-present-test   run DXGI-2 swapchain/present validation' \
	  'make dxgi-lifecycle-probe build the DXGI-3 window lifecycle probe' \
	  'make dxgi-lifecycle-test  run DXGI-3 lifecycle validation and regressions' \
	  'make dxgi-formats-probe   build the DXGI-4 format/color probe' \
	  'make dxgi-formats-test    run DXGI-4 format/color/HDR validation' \
	  'make dxgi-sync-probe      build the DXGI-5 synchronization probe' \
	  'make dxgi-sync-test       run DXGI-5 pacing/recovery stress validation' \
	  'make moltenvk    build the universal MoltenVK dylib (staged, not promoted)' \
	  'make metal3      build MoltenVK with macOS 14 / Metal 3 compatibility target' \
	  'make build       run both vkd3d and MoltenVK builds' \
	  'make flprobe     compile the D3D12 feature-level probe' \
	  'make stage       stage the matched D3D12 pair and MoltenVK ICD' \
	  'make ladder      run flprobe through WINE_RUNNER' \
	  'make package     create the release runtime tarball' \
	  'make test        run docs-check and the available local checks' \
	  'make clean       remove generated build/staging outputs only'

tools:
	@bash scripts/validate-toolchain.sh

docs-check:
	@test -f README.md
	@test -f LICENSE
	@test -f Makefile
	@test -f CONTRIBUTING.md
	@test -f SECURITY.md
	@test -f CODE_OF_CONDUCT.md
	@for f in docs/README.md docs/Final.md docs/requirements.md docs/Development.md docs/features.md docs/validation.md docs/release.md docs/install.md; do \
		test -f "$$f" || { echo "missing $$f"; exit 1; }; \
	done
	@grep -q 'github.com/metalsharp/vkd3d-proton-macos' README.md
	@grep -q 'License-AGPL' README.md
	@grep -q 'Build' README.md
	@git diff --check
	@echo 'documentation checks: PASS'

env:
	@source scripts/env.sh

vkd3d:
	@bash scripts/build-vkd3d-proton.sh

dxgi:
	@DXVK_SRC="$(DXVK_SRC)" DXVK_COMMIT="$(DXVK_COMMIT)" bash scripts/build-dxgi.sh

dxgi-probe: dxgi
	@mkdir -p "$(STAGE_DIR)"
	@"$(LLVM_MINGW)/bin/x86_64-w64-mingw32-clang" -O2 scripts/probes/dxgi/dxgi_probe.c \
		-o "$(STAGE_DIR)/dxgi_probe.exe" -ldxgi -ld3d12
	@file "$(STAGE_DIR)/dxgi_probe.exe"

dxgi-test: stage dxgi-probe
	@bash scripts/validate-dxgi-adapter.sh

dxgi-present-probe: dxgi
	@DXGI_PRESENT_SHADER_DIR="$(STAGE_DIR)/dxgi-present" \
		bash scripts/build-dxgi-present-shaders.sh
	@mkdir -p "$(STAGE_DIR)"
	@"$(LLVM_MINGW)/bin/x86_64-w64-mingw32-clang" -O2 scripts/probes/dxgi-present/dxgi_present_probe.c \
		-o "$(STAGE_DIR)/dxgi_present_probe.exe" -ldxgi -ld3d12 -luser32 -lgdi32
	@file "$(STAGE_DIR)/dxgi_present_probe.exe"

dxgi-present-test: stage dxgi-present-probe
	@bash scripts/validate-dxgi-presentation.sh

dxgi-lifecycle-probe: dxgi
	@DXGI_PRESENT_SHADER_DIR="$(STAGE_DIR)/dxgi-present" \
		bash scripts/build-dxgi-present-shaders.sh
	@mkdir -p "$(STAGE_DIR)"
	@"$(LLVM_MINGW)/bin/x86_64-w64-mingw32-clang" -O2 scripts/probes/dxgi-lifecycle/dxgi_lifecycle_probe.c \
		-o "$(STAGE_DIR)/dxgi_lifecycle_probe.exe" -ldxgi -ld3d12 -luser32 -lgdi32
	@file "$(STAGE_DIR)/dxgi_lifecycle_probe.exe"

dxgi-lifecycle-test: stage dxgi-probe dxgi-present-probe dxgi-lifecycle-probe
	@bash scripts/validate-dxgi-lifecycle.sh

dxgi-formats-probe: dxgi
	@DXGI_PRESENT_SHADER_DIR="$(STAGE_DIR)/dxgi-present" \
		bash scripts/build-dxgi-present-shaders.sh
	@mkdir -p "$(STAGE_DIR)"
	@"$(LLVM_MINGW)/bin/x86_64-w64-mingw32-clang" -O2 scripts/probes/dxgi-formats/dxgi_formats_probe.c \
		-o "$(STAGE_DIR)/dxgi_formats_probe.exe" -ldxgi -ld3d12 -luser32 -lgdi32 -lm
	@file "$(STAGE_DIR)/dxgi_formats_probe.exe"

dxgi-formats-test: stage dxgi-probe dxgi-present-probe dxgi-lifecycle-probe dxgi-formats-probe
	@bash scripts/validate-dxgi-formats.sh

dxgi-sync-probe: dxgi
	@DXGI_PRESENT_SHADER_DIR="$(STAGE_DIR)/dxgi-present" \
		bash scripts/build-dxgi-present-shaders.sh
	@mkdir -p "$(STAGE_DIR)"
	@"$(LLVM_MINGW)/bin/x86_64-w64-mingw32-clang" -O2 scripts/probes/dxgi-sync/dxgi_sync_probe.c \
		-o "$(STAGE_DIR)/dxgi_sync_probe.exe" -ldxgi -ld3d12 -luser32 -lgdi32 -lpsapi
	@file "$(STAGE_DIR)/dxgi_sync_probe.exe"

dxgi-sync-test: stage dxgi-probe dxgi-present-probe dxgi-lifecycle-probe dxgi-formats-probe dxgi-sync-probe
	@bash scripts/validate-dxgi-sync.sh

moltenvk:
	@bash scripts/build-moltenvk.sh

metal3:
	@XCODE_DEVELOPER_DIR="$(XCODE16_DEVELOPER_DIR)" \
		MACOSX_DEPLOYMENT_TARGET=14.0 bash scripts/build-moltenvk.sh

build: vkd3d moltenvk

flprobe:
	@mkdir -p "$(STAGE_DIR)"
	@"$(LLVM_MINGW)/bin/x86_64-w64-mingw32-clang" -O2 scripts/flprobe.c \
		-o "$(STAGE_DIR)/flprobe.exe" -ldxgi -ld3d12
	@file "$(STAGE_DIR)/flprobe.exe"

stage: dxgi
	@test -f "$(BUILD_DIR)/vkd3d-proton/x86_64-windows/d3d12.dll"
	@test -f "$(BUILD_DIR)/vkd3d-proton/x86_64-windows/d3d12core.dll"
	@test -f "$(BUILD_DIR)/dxvk-macos/x86_64-windows/dxgi.dll"
	@test -f "$(BUILD_DIR)/moltenvk-vkmt/MoltenVK_icd.json"
	@test -f "$(BUILD_DIR)/moltenvk-vkmt/libMoltenVK.dylib"
	@mkdir -p "$(STAGE_DIR)"
	@cp "$(BUILD_DIR)/vkd3d-proton/x86_64-windows/d3d12.dll" "$(STAGE_DIR)/"
	@cp "$(BUILD_DIR)/vkd3d-proton/x86_64-windows/d3d12core.dll" "$(STAGE_DIR)/"
	@cp "$(BUILD_DIR)/dxvk-macos/x86_64-windows/dxgi.dll" "$(STAGE_DIR)/"
	@cp "$(BUILD_DIR)/moltenvk-vkmt/"{libMoltenVK.dylib,MoltenVK_icd.json} "$(STAGE_DIR)/"
	@echo "staged runtime in $(STAGE_DIR)"

ladder: flprobe
	@test -x "$(WINE_RUNNER)" || { echo "set WINE_RUNNER=/path/to/run-probe.sh"; exit 1; }
	@cd "$(STAGE_DIR)" && "$(WINE_RUNNER)" flprobe.exe

package:
	@bash scripts/package-runtime.sh "$(PACKAGE)"

test: docs-check
	@echo 'static checks: PASS'
	@echo 'runtime probes: run make ladder and see docs/validation.md'

clean:
	@rm -rf "$(ROOT)/artifacts/build/vkd3d-proton-build" \
		"$(ROOT)/artifacts/build/dxvk-macos" \
		"$(ROOT)/artifacts/build/moltenvk-vkmt.build.new" \
		"$(ROOT)/artifacts/vkd3d-cross-x86_64.txt" \
		"$(ROOT)/artifacts/dxvk-cross-x86_64.txt"
