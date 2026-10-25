# Validation and regression

## Runtime environment

The probes run under a compatible Wine installation on the target macOS
host, with the candidate files staged beside the probe executable. For the
compatibility claim, the host must be macOS 14 / Metal 3; running on a newer
host is only a newer-host smoke test.

```bash
source scripts/env.sh
export WINEPREFIX="$WS/artifacts/prefix"
export WINE_BIN="${WINE_BIN:-$(command -v wine)}"
export WINEDLLOVERRIDES="d3d12,d3d12core,dxgi=n,b"
export VK_ICD_FILENAMES="$WS/artifacts/stage-dxr/MoltenVK_icd.json"
export DYLD_LIBRARY_PATH="$WS/artifacts/stage-dxr${WINE_UNIX_LIB:+:$WINE_UNIX_LIB}"
export DYLD_FALLBACK_LIBRARY_PATH="$DYLD_LIBRARY_PATH"
```

The exact local runner may add `VKMT_ALLOW_NON_SINGLE_TEXEL_ALIGNMENT=1`,
`MVK_PRESENT_MODE=1`, cache paths, and `WINEDEBUG=-all`. Keep those settings
in the runner rather than changing the probes.

## DXGI-1 adapter gate

The pinned DXVK macOS DXGI provider and adapter identity gate run with:

```bash
make dxgi-test
```

This proves native `dxgi.dll` loading, DXGI factory creation, hardware adapter
enumeration, stable DXGI/D3D12 LUID matching, D3D12Core loadability and exports,
Vulkan/MoltenVK vendor/device identity, ten repeatable runs, invalid-input
handling, and the complete regression suite. It does not create a swapchain;
presentation begins in DXGI-2.

## DXGI-2 windowed presentation gate

Run the complete stage 2 gate with:

```bash
make dxgi-present-test
```

The validator requires a native x86_64 DXVK `dxgi.dll` built from the pinned
base plus the checked-in D3D12 bridge patch, a real Win32 window, and a
DXGI/D3D12 LUID match. It runs two deterministic passes of all four accepted
windowed combinations:

- `CreateSwapChain` with flip-discard and flip-sequential, followed by `Present`;
- `CreateSwapChainForHwnd` with flip-discard and flip-sequential, followed by
  `Present1`.

Every accepted mode renders a deterministic clear and triangle, checks
backbuffer transitions and CPU readback, runs 1,000 frames, alternates sync
intervals 0/1, requests `DXGI_PRESENT_ALLOW_TEARING` only when reported,
executes `DXGI_PRESENT_TEST`, and checks frame statistics and last-present
count. Invalid descriptors and post-release presentation are negative tests.
The validator then reruns the six-probe regression suite and records module
paths, SHA-256 hashes, source revisions, and Wine/DXVK/vkd3d/MoltenVK output in
`artifacts/evidence/dxgi-2-presentation.md` and its adjacent logs.

stage 2 does not cover resize, minimize, fullscreen, recovery stress, or broad
gameplay stability. Those claims remain blocked until later stage gates pass.

## DXGI-3 lifecycle gate

Run the lifecycle probe and its full preservation gates with:

```bash
make dxgi-lifecycle-test
```

The validator requires the clean pinned DXVK base plus the checked-in bridge
patch, matched x86_64 `dxgi.dll`/D3D12 modules, and the staged MoltenVK ICD.
It runs two deterministic passes of a real Win32 window using the stage-1
adapter. Each pass verifies normal multi-size `ResizeBuffers`, RTV and
backbuffer reacquisition, deterministic RGB-triangle/clear readback and state
transitions, minimize/restore and occlusion, destruction/recreation, and
fullscreen query/target/windowed-fallback behavior. Zero-size and exclusive
fullscreen results are accepted only when explicitly reported supported or
unsupported.

The negative matrix covers invalid dimensions, outstanding backbuffer
references, invalid `ResizeTarget`/fullscreen parameters, and presentation
after window destruction. The stress section performs 100 create/resize/destroy
cycles and checks ordered GPU-idle/resource/swapchain/queue/device/adapter/
factory/window shutdown. The validator then reruns the DXGI-1 and DXGI-2 gates
and all six existing regression probes. Evidence is written to
`artifacts/evidence/dxgi-3-lifecycle.md` and the adjacent `dxgi-3-*` logs.

DXGI-3 proves this tested lifecycle lane only. It does not claim broad gameplay
stability, later format/HDR coverage, or final package promotion.

## DXGI-4 format and color-policy gate

Run the format/color/HDR matrix with:

```bash
make dxgi-formats-test
```

The validator runs two deterministic passes using the real Win32 window and
stage-1 adapter. It requires exact GPU readback for BGRA8 UNORM, BGRA8 sRGB,
and RGBA8 UNORM, including channel order, alpha, linear values, and sRGB
conversion. It validates D3D12 format-support results, render-target resource
and RTV creation, dimensions, barriers, an MSAA `ResolveSubresource`, backbuffer
acquisition, Present, and tearing policy. R10G10B10A2 is classified separately: the current lane reports
D3D12 support and resource/RTV creation but rejects the swapchain and does not
perform unsafe GPU conversion readback.

D24/D32 depth-stencil resources and DSVs are created, cleared, transitioned,
and depth-read back. Stencil clear/DSV behavior is exercised, while the
backend's missing stencil copy plane is recorded as unsupported. The policy
matrix checks alpha mode, `CheckColorSpaceSupport`, `SetColorSpace1`, HDR
metadata, invalid color-space/alpha/format/descriptor inputs, incompatible
SDR/HDR combinations, and tearing flags. DXGI 1.4/1.5 expose Check/Set color
space methods but no `GetColorSpace1`; the probe records that API as not
exposed. A successful HDR metadata setter is never used as HDR evidence.

On the current configured runner, SDR P709 is supported while scRGB, HDR10 PQ,
and extended P2020 are accurately reported unsupported. The TV is not used as
HDR evidence; a display's capability alone does not override the actual DXGI
and GPU result. Evidence is stored in
`artifacts/evidence/dxgi-4-formats.md` and adjacent `dxgi-4-*` logs. The format
does not claim broad gameplay stability or begin synchronization/recovery work.

## DXGI-5 synchronization and pacing gate

Run the synchronization and recovery-classification gate with:

```bash
make dxgi-sync-test
```

The validator requires the pinned DXVK source and matched staged modules, then
runs independent short passes with two, three, and four frames in flight. It
repeats the three-frame pass for determinism before running a 100,000-frame
stress pass. Each pass uses a real Win32 window and checks adapter identity,
fence signal/completion, bounded `SetEventOnCompletion` waits, cross-queue
signal/wait, frame-latency waitable objects, sync intervals 0/1,
`DXGI_PRESENT_TEST`, occlusion, deterministic RGB-triangle readback, and
ordered GPU-idle shutdown.

The stress pass performs periodic resource and graphics-pipeline churn and
records process working-set samples. `GetDeviceRemovedReason` is checked after
normal operation and shutdown. `IDXGIDevice3::Trim` and unsafe forced
out-of-order/resource-early-release negatives are reported from the actual
backend boundary; the latter are not submitted because this backend asserts
instead of returning a safe HRESULT. Unsupported behavior is not promoted to
success.

Evidence is consolidated in
`artifacts/evidence/dxgi-5-synchronization.md`, with short-run, long-stress,
stage-gate, module, and backend logs in adjacent `dxgi-5-*` files. DXGI-5 is
synthetic synchronization evidence only and does not claim device-loss
recovery or broad gameplay stability.

## Probe gate

The release gate is green only when all of these pass:

| Probe | Required result |
|---|---|
| `flprobe.exe` | feature levels 11_0 to 12_1 create, and 12_2 with MoltenVK's experimental ray tracing |
| `cr_inner_probe.exe` | CR tier-3 InnerCoverage result matches reference |
| `feedback_probe.exe` | sampler feedback matches CPU reference exactly |
| `mesh_probe.exe` | mesh dispatch is pixel-exact |
| `corpus_gs.exe` | geometry-shader corpus works |

Run an individual staged probe with the established runner:

```bash
cd artifacts/stage-dxr
/tmp/run-probe.sh flprobe.exe
/tmp/run-probe.sh feedback_probe.exe
/tmp/run-probe.sh mesh_probe.exe
```

## Evidence requirements

Each promoted runtime must record:

1. `d3d12.dll`, `d3d12core.dll`, and `libMoltenVK.dylib` SHA-256 hashes;
2. the source commit IDs and compiler versions;
3. the loaded module paths from the probe;
4. the full ladder and regression output; and
5. MoltenVK architecture and code-signature checks.

The authoritative release ladder evidence is
`artifacts/evidence/rung-ladder-2026-08-16.txt`. The release archive repeats
its own SHA-256 checksums so a downloaded runtime can be checked independently.

## Failure rules

- A stale staged DLL invalidates the run; rebuild and restage both DLLs.
- A mixed-architecture or mixed-build D3D12 pair is invalid.
- Extension advertisement or a successful shader translation is not GPU
  acceptance evidence without deterministic readback.
- Do not use `VKD3D_FEATURE_LEVEL` or option-bit forcing to claim support.
