# Build VKD3D-Proton macOS

The [Build workflow](../.github/workflows/build.yml) is the best reference
for the portable repository checks. The root `Makefile` is the source of
truth for local build commands:

```bash
make help
```

This document is for developers who are comfortable with macOS toolchains,
cross-compilers, Meson/Ninja, Xcode, Wine, and GPU debugging. It is not a
beginner's introduction to those tools.

## Prerequisites

- Apple Silicon Mac with a Metal 3-capable GPU for the compatibility lane.
- macOS with Xcode and Command Line Tools installed.
- Xcode 16+ with the Metal toolchain. Xcode 16 is the reference macOS 14
  compatibility toolchain; newer Xcode versions may target macOS 14 too.
- `llvm-mingw-20260616-ucrt-macos-universal` with clang 22.1.8.
- Meson 1.x, Ninja, CMake, Python 3, Git, `zstd`, and `pkg-config`.
- DirectX Shader Compiler (`dxc`) for SM 6.x HLSL/DXIL probe shaders.
- A compatible Wine installation for Wine-side probes.
- `gh` authenticated with repository permissions for publishing releases.

Run the repository checks before building:

```bash
make docs-check
make tools
```

`make tools` checks the selected Xcode and source-clone paths. On another
machine, set `XCODE_DEVELOPER_DIR` and `WINE_BIN` locally; do not commit
machine-specific paths.

## Source setup

The public repository keeps large source and build trees out of Git. Create
fresh working trees at these paths:

```text
sources/vkd3d-proton/
sources/MoltenVK/
sources/SPIRV-Cross/
```

`make vkd3d` requires the vkd3d-proton tree. `make moltenvk` requires
MoltenVK and its fetched dependencies. Keep the SPIRV-Cross revision used by
MoltenVK aligned with `scripts/patch-spirv-cross.sh`.

## Setup the environment

```bash
source scripts/env.sh
```

The environment script configures the llvm-mingw path, Xcode developer
directory, generic Wine launcher, macOS deployment target, and workspace
output locations. Set
`WINE_BIN` to a specific Wine executable when it is not on `PATH`; set
`WINE_UNIX_LIB` when that Wine build needs an explicit Unix-library path. The
runtime target is x86_64 PE under Wine/Rosetta; MoltenVK is built universal
(x86_64 + arm64).

## Build vkd3d-proton

Build the matched D3D12 forwarder/implementation pair:

```bash
make vkd3d
```

This runs Meson with the generated
`artifacts/vkd3d-cross-x86_64.txt` cross file and Ninja. Outputs are:

```text
artifacts/build/vkd3d-proton/x86_64-windows/d3d12.dll
artifacts/build/vkd3d-proton/x86_64-windows/d3d12core.dll
```

Both files must be PE32+ x86-64 and must come from the same build. Never mix
DLLs from different lanes or build timestamps.

## Build DXVK macOS DXGI

stage DXGI-1 uses the pinned DXVK macOS source at commit
`8f1e28deed3ad30802f7e1bdff428ec14e6e7817`:

```bash
git clone https://github.com/Gcenx/DXVK-macOS.git sources/dxvk-macos
git -C sources/dxvk-macos checkout 8f1e28deed3ad30802f7e1bdff428ec14e6e7817
make dxgi
make dxgi-probe
```

The provider is built as an x86_64 PE DLL at
`artifacts/build/dxvk-macos/x86_64-windows/dxgi.dll`. Stage it with the
matched D3D12 pair using `make stage`. The complete stage 1 gate is:

```bash
make dxgi-test
```

That runs the adapter/factory probe ten times, checks stable DXGI/D3D12 LUID
identity against the MoltenVK GPU log, loads and checks the D3D12Core exports,
runs invalid-input tests, and reruns the existing six-probe regression suite.
DXGI-1 does not yet test windowed swapchains or gameplay presentation; those
are DXGI-2 and later.

## Build and validate DXGI-2 presentation

The DXGI-2 lane applies the checked-in
`patches/dxvk-macos-d3d12-dxgi.patch` to the clean pinned DXVK base while
building. The source checkout is restored to the pinned commit after the
build, so verify it remains clean:

```bash
make dxgi-present-probe
make dxgi-present-test
```

`dxgi-present-test` stages the native x86_64 `dxgi.dll` beside the matched
`d3d12.dll`/`d3d12core.dll`, builds the DXIL triangle shaders, and runs the
real-Win32-window probe under the configured `WINE_RUNNER`. It accepts only
the four windowed combinations supported by this lane:

```text
CreateSwapChain           + flip-discard     + Present
CreateSwapChain           + flip-sequential  + Present
CreateSwapChainForHwnd    + flip-discard     + Present1
CreateSwapChainForHwnd    + flip-sequential  + Present1
```

Each mode must complete 1,000 frames with GPU readback matching the clear and
triangle reference. The gate also checks adapter LUID identity, sync intervals
0/1, `DXGI_PRESENT_TEST`, tearing reporting, frame statistics, last-present
count, invalid descriptors, and presentation after releasing the caller's
swapchain reference. It leaves evidence and full Wine/vkd3d/MoltenVK/DXVK
logs under `artifacts/evidence/`.

This target intentionally does not test resize, minimize, fullscreen, or
general gameplay stability; those are later DXGI stages.

## Build and validate DXGI-3 lifecycle

The lifecycle lane uses the same pinned DXVK bridge and applies the checked-in
vkd3d guard patch `patches/vkd3d-proton-dxgi-lifecycle.patch` only while
building. `make vkd3d` reverts that temporary source change before returning,
so the downloaded source checkout remains clean apart from the workspace's
pre-existing local vkd3d changes:

```bash
make dxgi-lifecycle-probe
make dxgi-lifecycle-test
```

The probe creates a real Win32 window, selects the stage-1 adapter, and
recreates RTVs/readback resources after each accepted `ResizeBuffers`. It
renders the deterministic RGB triangle and clear color after normal and
minimized/zero-size transitions, checks dimensions, formats, barriers, and
GPU readback, and exercises occlusion, destruction/recreation, fullscreen
queries, `ResizeTarget`, and windowed fallback. Unsupported zero-size or
exclusive-fullscreen behavior must be reported as `UNSUPPORTED`, never
silently treated as success.

The deterministic negatives cover oversized resize dimensions, outstanding
backbuffer references, invalid target/fullscreen parameters, and presentation
after window destruction. The stress loop performs 100 create/resize/destroy
cycles and the probe prints the required shutdown order. The validator runs
two lifecycle passes, reruns DXGI-1 and DXGI-2, reruns the six-probe suite, and
writes complete module/hash/source/log evidence under `artifacts/evidence/`.
DXGI-3 remains a lifecycle validation claim only; it is not a broad gameplay
stability or final-release claim.

## Build and validate DXGI-4 formats and color policy

The DXGI-4 lane applies the checked-in
`patches/dxvk-macos-dxgi-formats.patch` after the D3D12 bridge patch while
building. The stage patch validates swapchain alpha modes and is reverted with
the downloaded DXVK source after the build:

```bash
make dxgi-formats-probe
make dxgi-formats-test
```

The probe runs the real Win32 window and stage-1 adapter through a format
matrix. It performs exact GPU readback for BGRA8 UNORM, BGRA8 sRGB, and RGBA8
UNORM, checking channel ordering, alpha, linear values, and sRGB transfer.
It also resolves a deterministic 4x MSAA BGRA8 target into a single-sample
resource and verifies the resolved clear-color readback.
R10G10B10A2 support and resource/RTV creation are queried separately; the
current native DXGI lane rejects its swapchain and reports GPU readback
unsupported rather than risking a backend hang. D24/D32 DSV creation, clear,
barrier, and depth-plane readback are covered; stencil readback is explicitly
reported unsupported because the backend copy footprint exposes no stencil
plane.

The policy matrix exercises alpha reporting, `CheckColorSpaceSupport`,
`SetColorSpace1`, HDR metadata, invalid color-space/alpha/format/descriptor
inputs, incompatible SDR/HDR combinations, and tearing flags. The Win32 DXGI
interfaces expose Check/Set color-space methods but no `GetColorSpace1`; the
probe records that fact instead of inventing an API result. HDR10/scRGB/P2020
are accepted only when the configured runner reports support and GPU output;
the current run reports them unsupported. The validator runs two format passes,
reruns DXGI-1/2/3 and the six-probe suite, and stores hashes and full logs in
`artifacts/evidence/dxgi-4-*`. This stage makes no gameplay-stability claim.

## Build and validate DXGI-5 synchronization and pacing

The DXGI-5 probe uses the same pinned DXVK bridge and the stage-4 swapchain.
Build and run it with:

```bash
make dxgi-sync-probe
make dxgi-sync-test
```

`dxgi-sync-test` runs short passes with two, three, and four frames in flight,
then repeats the three-frame mode and runs the long gate with 100,000 frames.
The probe creates per-frame command allocators and readback resources, checks
fence signal/completion, bounded event waits, cross-queue waits, frame-latency
waitable objects, sync intervals 0/1, `DXGI_PRESENT_TEST`, occlusion, and
deterministic RGB-triangle readback. It also performs periodic pipeline and
resource churn, records process-memory samples, queries device removal state,
and checks GPU-idle through window shutdown ordering.

`IDXGIDevice3::Trim` is reported as unsupported when the D3D12 device does not
expose that interface. The backend currently asserts instead of returning a
safe result for forced out-of-order fence signals and resource release before
GPU completion, so those unsafe negatives are explicitly classified as
unsupported and are not submitted. This is not a recovery claim. Full module
hashes, runtime logs, fence timelines, memory samples, short-run repeatability,
the 100,000-frame result, and the preserved DXGI-1/2/3/4 and six-probe gates
are stored under `artifacts/evidence/dxgi-5-*`.

This stage proves synthetic synchronization and pacing only. It does not claim
device-loss recovery, real-game compatibility, or broad gameplay stability.

## Build MoltenVK

Build the custom universal dylib candidate:

```bash
make moltenvk
```

The build fetches MoltenVK externals, reapplies the local SPIRV-Cross patches,
and writes a candidate to:

```text
artifacts/build/moltenvk-vkmt.build.new/
```

This directory is deliberately staged and not promoted automatically. Run
the validation gate first. Only after the probes pass should the candidate be
copied to `artifacts/build/moltenvk-vkmt/`.

Build both runtime components with:

```bash
make build
```

To build the compatibility profile with the downloaded Xcode 16 and a macOS
14 deployment target:

```bash
make metal3
```

To select the downloaded Xcode 16 explicitly:

```bash
XCODE16_DEVELOPER_DIR=/Users/averyfelts/Downloads/Xcode.app/Contents/Developer make metal3
```

Accept that Xcode installation's license first if `xcodebuild` reports
license error 69.

This verifies compilation against the current Xcode SDK while targeting
macOS 14. The runtime must still be exercised on a macOS 14 host to prove
Metal 3 behavior; a newer host will report and execute its own newer Metal
family.

## Build and stage probes

Compile the D3D12 feature-level probe and stage the canonical runtime:

```bash
make flprobe
make stage
```

`make flprobe` creates `artifacts/stage-dxr/flprobe.exe`. The acceptance
probes that use HLSL require DXIL blobs compiled by `dxc`; see
[validation.md](validation.md) for their expected layout and gate.

## Run validation

The Wine runner is host-specific. The established validation runner is
`/tmp/run-probe.sh`:

```bash
make ladder WINE_RUNNER=/tmp/run-probe.sh
```

The ladder must create devices for 12_1, 12_0, 11_1 and 11_0, with `max=12_1`,
and for 12_2 too, with `max=12_2`, when `MVK_CONFIG_ENABLE_EXPERIMENTAL_RAY_TRACING=1`
turns on MoltenVK's ray tracing pipelines. The full regression gate is documented in
[validation.md](validation.md). Do not accept a result without checking the
loaded module paths and hashes of the staged DLLs.

## Package and publish

Releases are built by CI, not locally: pushing a tag `vX.Y` builds the x86_64 and arm64 VKD3D-Proton packages and
the universal MoltenVK package, and publishes them as one release (`.github/workflows/release.yml`). Pull requests
that change the build build the same packages as artifacts, so a change is checked before it reaches a tag.

```bash
git tag v2.0
git push origin v2.0
```

See [release.md](release.md) for the packages' contents and [install.md](install.md) for installing them. The older
single-archive target, `make package`, still builds the v1.0-style x86_64 archive locally.

## Debugging

Useful diagnostics for the D3D12-on-Metal route include:

```bash
export WINEDEBUG=-all                    # reduce Wine noise
export VKD3D_DEBUG=trace                 # feature/layer diagnostics
export VKD3D_SHADER_DUMP_PATH=/tmp/dxil  # dump converted shaders
export MVK_CONFIG_SHADER_DUMP_DIR=/tmp/msl
export VK_LOADER_DEBUG=error             # Vulkan loader diagnostics
export MTL_DEBUG_LAYER=1                 # Metal API validation
export MTL_SHADER_VALIDATION=1           # Metal shader validation
```

For the validated Wine route, keep the ICD and dynamic-library paths
explicit:

```bash
export WINEDLLOVERRIDES="d3d12,d3d12core,dxgi=n,b"
export VK_ICD_FILENAMES="$WS/artifacts/stage-dxr/MoltenVK_icd.json"
export WINE_BIN="${WINE_BIN:-$(command -v wine)}"
export WINEPREFIX="$WS/artifacts/prefix"
export DYLD_LIBRARY_PATH="$WS/artifacts/stage-dxr${WINE_UNIX_LIB:+:$WINE_UNIX_LIB}"
export DYLD_FALLBACK_LIBRARY_PATH="$DYLD_LIBRARY_PATH"
```

Use `VKMT_ALLOW_NON_SINGLE_TEXEL_ALIGNMENT=1` and `MVK_PRESENT_MODE=1` only
when reproducing the validated launch shape. Feature-level forcing is not
valid acceptance evidence.

## Logs and evidence

Wine-side probes print to stderr/stdout. Save the complete output, not only a
summary line:

```bash
cd artifacts/stage-dxr
/tmp/run-probe.sh flprobe.exe 2>&1 | tee /tmp/flprobe.log
```

Record candidate hashes, source commits, compiler versions, loaded module
paths, and probe output under `artifacts/evidence/`. The current feature
matrix is [features.md](features.md); the current regression rules are in
[validation.md](validation.md).

## Clean and contribute

Remove generated build configuration/output without touching source clones:

```bash
make clean
```

Before opening a change:

```bash
make test
git diff --check
```

Keep generated archives, local source trees, staged DLLs, and machine-specific
paths out of commits. Changes to a promoted runtime require fresh ladder and
regression evidence.
