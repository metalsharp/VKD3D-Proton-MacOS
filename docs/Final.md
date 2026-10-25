# VKD3D-Proton macOS — Final State

**Status:** shipped and publicly released as [`v1.0`](https://github.com/metalsharp/vkd3d-proton-macos/releases/tag/v1.0).

> This records the v1.0 release. Current releases are built by CI with separate x86_64, arm64 and MoltenVK
> packages: see [release.md](release.md) and [install.md](install.md).

This is the consolidated final-state record for the Apple Silicon D3D12
runtime. It replaces the numbered planning/evidence documents that were used
during development.

## Runtime route

```text
D3D12 application
        │
        ▼
Compatible Wine / Rosetta x86_64
        │
        ▼
vkd3d-proton: d3d12.dll + d3d12core.dll
        │
        ▼
Vulkan / winevulkan
        │
        ▼
custom MoltenVK: universal x86_64 + arm64
        │
        ▼
Apple Metal 3 / macOS 14 compatibility target
```

The runtime is intentionally x86_64 on the Wine/D3D12 side and universal on
the MoltenVK side. Keep the ICD manifest beside the dylib and pin it with
`VK_ICD_FILENAMES`.

## Released package

Download [`vkd3d-proton-macos.tar.zst`](https://github.com/metalsharp/vkd3d-proton-macos/releases/tag/v1.0).
The archive contains:

```text
vkd3d-proton-macos/
├── dxgi.dll
├── d3d12.dll
├── d3d12core.dll
├── libMoltenVK.dylib
├── MoltenVK_icd.json
├── README.md
└── SHA256SUMS
```

Release artifact hashes:

| Artifact | SHA-256 |
|---|---|
| `dxgi.dll` | `943dc921530aeba8bc5add09f5a3c5fac7da50e90a84ca2f41f1b87ba532846e` |
| `d3d12.dll` | `ac2b8674798bdbdd21ce1aa48daf1e2657813ecc878b80e2641bf0d2c3f2a43e` |
| `d3d12core.dll` | `78ab917a20dbc050ba3d0def8c0241e53c90ded0a036462955108e0ef78022a8` |
| `libMoltenVK.dylib` | `38e0a7c3839390d524a3bb4b1165d13e96a2c3e771a14df2510c1ad5ab598bde` |
| `MoltenVK_icd.json` | `578ff08cd0d8734619357541771a5abc9c3470ca300030219a971a9e9dbbe466` |

`libMoltenVK.dylib` is a universal x86_64/arm64 Mach-O with a valid adhoc
signature for the isolated runtime override path.

## DXGI-2 windowed presentation validation

The v1.0 runtime bundle now includes the native x86_64 DXVK-macOS `dxgi.dll`.
After the v1.0 runtime release, the pinned DXVK macOS lane was extended with
the checked-in `patches/dxvk-macos-d3d12-dxgi.patch`. This bridge lets the
native DXVK `dxgi.dll` consume vkd3d-proton's Vulkan-backed D3D12 swapchain
frontend instead of reporting an unsupported device type. The complete gate is
`make dxgi-present-test`, with evidence in
[`dxgi-2-presentation.md`](../artifacts/evidence/dxgi-2-presentation.md).

Two repeatable runs passed all four windowed combinations:

```text
CreateSwapChain        + flip-discard    + Present
CreateSwapChain        + flip-sequential + Present
CreateSwapChainForHwnd + flip-discard    + Present1
CreateSwapChainForHwnd + flip-sequential + Present1
```

Each mode rendered a deterministic clear and triangle with GPU readback for
1,000 frames, checked transitions, sync intervals, tearing, test presents,
statistics, last-present count, and negative release/descriptor cases. The
existing six-probe regression suite remained green. This is a basic windowed
presentation gate, not a claim of resize, fullscreen, recovery, or broad
gameplay stability, and the v1.0 archive remains the separately documented
runtime release until a later package promotion.

## DXGI-3 lifecycle validation

The follow-on lifecycle gate is now green with `make dxgi-lifecycle-test`.
Evidence is consolidated in
[`dxgi-3-lifecycle.md`](../artifacts/evidence/dxgi-3-lifecycle.md). Two
repeatable runs used a real Win32 window and the stage-1 adapter and passed:

- normal 800x600, 320x240, and 1024x512 resizes with RTV recreation and
  pixel-correct RGB-triangle/clear readback;
- minimize/restore, zero-size behavior, occlusion/test Present, fullscreen
  query, `ResizeTarget`, and safe windowed fallback;
- destruction/recreation, invalid dimensions and target parameters,
  outstanding-backbuffer-reference handling, and post-destruction Present;
- 100 create/resize/destroy cycles and ordered GPU-idle through window
  shutdown; and
- the DXGI-1, DXGI-2, and existing six-probe regression gates.

The lifecycle dimension guard is reproduced by
`patches/vkd3d-proton-dxgi-lifecycle.patch`, applied transiently by
`scripts/build-vkd3d-proton.sh`. Zero-size drawable and exclusive fullscreen
are reported according to the host's actual HRESULTs. This is a validated
lifecycle lane, not a claim of broad gameplay stability or a replacement for
the later format, pacing, recovery, and real-game stages.

## DXGI-4 format and color policy validation

The format/color gate is green with `make dxgi-formats-test`; complete evidence
is in [`dxgi-4-formats.md`](../artifacts/evidence/dxgi-4-formats.md). Two runs
passed exact GPU readback and presentation for BGRA8 UNORM, BGRA8 sRGB, and
RGBA8 UNORM. The matrix also covers format support, RTV/resource creation,
backbuffer descriptors, barriers, MSAA resolve/readback, alpha reporting, SDR color space, tearing,
depth/DSV behavior, invalid descriptors, invalid alpha modes, incompatible
HDR color space, and HDR metadata error handling.

R10G10B10A2 is reported as D3D12/resource-supported but unsupported for this
native DXGI swapchain/readback lane. D24/D32 depth planes pass deterministic
readback; stencil clear and DSV behavior execute, while the backend's stencil
copy plane is explicitly unsupported. The configured DXGI path reports SDR
P709 supported and scRGB/HDR10/P2020 unsupported. The HDR metadata setter's
HRESULT is not treated as HDR support, and no HDR claim is made for either the
TV or the MacBook display without a supported DXGI color-space result and GPU
readback. DXGI-1/2/3 and the six-probe regression suite remain green.

This is format and presentation-policy validation only; it does not claim
broad gameplay stability or cover the later synchronization/recovery stage.

## DXGI-5 synchronization and pacing validation

The synthetic synchronization gate is green with `make dxgi-sync-test`.
Evidence is consolidated in
[`dxgi-5-synchronization.md`](../artifacts/evidence/dxgi-5-synchronization.md).
It passed repeatable short runs with two, three, and four frames in flight and
a 100,000-frame stress run using the real Win32 window, stage-1 adapter, and
DXGI-4 swapchain.

The gate checks D3D12 fence signals and event completion, bounded CPU waits,
cross-queue waits, frame-latency waitable objects, sync intervals 0/1,
`DXGI_PRESENT_TEST`, occlusion, exact RGB-triangle readback, periodic resource
and pipeline churn, working-set samples, device-removal queries, and ordered
GPU-idle shutdown. The configured backend reports `IDXGIDevice3::Trim` as
unavailable. Forced out-of-order fence and resource-release-before-completion
negatives are explicitly reported unsupported because this backend asserts when
those unsafe operations are submitted; they are not treated as passes.

DXGI-1 through DXGI-4 and the six-probe regression suite remained green. This
is synthetic synchronization/pacing evidence only; it does not claim
device-loss recovery, real-game compatibility, or broad gameplay stability.

## Feature-level ladder

The final `flprobe.exe` run on Apple GPU returned `S_OK` and `dev=CREATED` for
every requested minimum level:

| Minimum feature level | Result |
|---|---|
| 12_2 | created |
| 12_1 | created |
| 12_0 | created |
| 11_1 | created |
| 11_0 | created |
| CORE_1_0 | created |

`CheckFeatureSupport(FEATURE_LEVELS)` reports **12_2** as the maximum. The
probe handles the known mingw-w64 renumbering of several feature-query IDs by
trying the official Microsoft IDs and then the ABI-compatible mingw IDs.
That fallback is not feature forcing.

## Validated capability matrix

| Capability | Final reported result | Acceptance evidence |
|---|---:|---|
| Shader Model | 6.5 | feature ladder |
| DXR / ray tracing | tier 1.1 (`RaytracingTier=11`) | inline ray-query path and ladder |
| Variable-rate shading | tier 2 | feature ladder |
| Mesh shaders | tier 1 (`MeshShaderTier=10`) | pixel-exact mesh dispatch |
| Sampler feedback | tier 0_9 (`SamplerFeedbackTier=90`) | exact CPU-reference readback |
| Tiled resources | tier 4 | sparse execution evidence and ladder |
| Conservative rasterization | tier 3 | InnerCoverage/CR probe |
| Rasterizer-ordered views | supported | ordering probe |
| Output-merger logical operations | supported | pixel-exact logic-op matrix |
| Depth bounds | supported | feature ladder |
| Copy-queue timestamps | supported | feature ladder |
| Fully typed casting | supported | feature ladder |
| Barycentrics | supported | feature ladder |

Capability reporting is distinguished from full API-surface claims. In
particular, the full TraceRay/RTPSO path and the optional task/object mesh
amplification variant remain separate follow-up work; the shipped acceptance
claims are the inline ray-query and mesh-only paths listed above.

## macOS 14 / Metal 3 compatibility status

The compatibility build profile is now reproducible with the current Xcode
beta:

```bash
make metal3
```

The resulting universal `libMoltenVK.dylib` compiled successfully with
`MACOSX_DEPLOYMENT_TARGET=14.0`; its Mach-O `LC_BUILD_VERSION` reports
`minos 14.0` for both arm64 and x86_64. The build used the Xcode 27 SDK while
targeting macOS 14, and the source runtime already selects Metal 3 language
and feature paths through OS/GPU availability checks.

The downloaded Xcode 16 profile is ready but its license must be accepted
locally before it can run:

```bash
sudo xcodebuild -license accept
XCODE16_DEVELOPER_DIR=/Users/averyfelts/Downloads/Xcode.app/Contents/Developer make metal3
```

Compilation against Xcode 16 and functional execution on an actual macOS 14
Metal 3 host remain separate validation gates. A newer Metal 4 host can prove
the macOS 14 deployment target but cannot prove runtime behavior on Metal 3.

## Regression gate

The final companion gate passed:

- `cr_inner_probe.exe` — CR tier-3 InnerCoverage matches the reference;
- `feedback_probe.exe` — sampler feedback matches the CPU reference exactly;
- `mesh_probe.exe` — mesh dispatch is pixel-exact;
- `corpus.exe` — CORE_1_0 compute corpus works;
- `corpus_gs.exe` — geometry-shader corpus works; and
- `compute_matrix.exe` — CORE_1_0 compute matrix works.

## Delivered implementation

- vkd3d-proton MS/AS graphics pipeline state layout and extraction were
  restored and kept ABI-consistent with the generated D3D12 headers.
- MoltenVK gained the mesh-stage plumbing, mesh draw dispatch path, and the
  required null guard in reserved vertex-attribute setup.
- SPIRV-Cross gained sampler-feedback 64-bit image-atomic lowering and the
  storage-image access fix needed for descriptor-array/function-parameter
  paths.
- Conservative rasterization, logic-op emulation, ROV ordering, sparse
  resources, ray-query, CORE_1_0, mesh, and sampler-feedback probes provide
  deterministic GPU/readback evidence for the promoted capabilities.
- The release process produces a small runtime archive rather than placing
  generated binary bundles in Git history.

## Reproduce the build

Install the tools in [requirements.md](requirements.md), place fresh source
trees under `sources/`, and use the root Makefile:

```bash
make docs-check
make tools
make vkd3d
make moltenvk
make flprobe
make stage
make ladder WINE_RUNNER=/tmp/run-probe.sh
make package PACKAGE=./vkd3d-proton-macos.tar.zst
```

The complete developer workflow, debugging variables, and release procedure
are in [Development.md](Development.md). Validation rules are in
[validation.md](validation.md), and runtime installation is in
[release.md](release.md).

## Repository policies

- [Contributing](../CONTRIBUTING.md)
- [Security](../SECURITY.md)
- [Code of Conduct](../CODE_OF_CONDUCT.md)

## Final provenance

- Public repository: <https://github.com/metalsharp/vkd3d-proton-macos>
- Release tag: `v1.0`
- Final documentation/policy state is checked by `make docs-check`.
- Generated source clones, build directories, and release archives remain
  outside Git; the release asset is the distribution artifact.
