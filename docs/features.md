# Feature levels and capabilities

## D3D12 ladder

`scripts/flprobe.c` against VKD3D-Proton and this MoltenVK on an Apple M4 (macOS 27.2):

| Minimum level | Result |
|---|---|
| D3D_FEATURE_LEVEL_12_2 | device created with `MVK_CONFIG_ENABLE_EXPERIMENTAL_RAY_TRACING=1`; without it, `E_INVALIDARG` (no ray tracing) |
| D3D_FEATURE_LEVEL_12_1 | `S_OK`, device created |
| D3D_FEATURE_LEVEL_12_0 | `S_OK`, device created |
| D3D_FEATURE_LEVEL_11_1 | `S_OK`, device created |
| D3D_FEATURE_LEVEL_11_0 | `S_OK`, device created |

`CheckFeatureSupport(FEATURE_LEVELS)` reports **12_1** as the maximum, and **12_2** with ray tracing on. The highest
shader model is **6.8**.
The complete captured result is
[`artifacts/evidence/rung-ladder-2026-08-16.txt`](../artifacts/evidence/rung-ladder-2026-08-16.txt).

## Reported capability matrix

| Capability | Reported result | Acceptance |
|---|---:|---|
| Shader model | SM 6.5 | ladder |
| DXR / ray tracing | tier 1.1 (`RaytracingTier=11`) | ladder + ray-query feasibility |
| Variable-rate shading | tier 2 | ladder |
| Mesh shaders | tier 1 (`MeshShaderTier=10`) | pixel-exact mesh dispatch |
| Sampler feedback | tier 0_9 (`SamplerFeedbackTier=90`) | exact CPU-reference readback |
| Tiled resources | tier 4 | ladder/sparse evidence |
| Conservative rasterization | tier 3 | CR probe |
| ROVs | supported | ROV ordering probe |
| Depth bounds | supported | ladder |
| Copy-queue timestamps | supported | ladder |
| Fully typed casting | supported | ladder |
| Barycentrics | supported | ladder |
| Output-merger logic ops | supported | pixel-exact logic-op evidence |

The mesh acceptance probe covers the mesh-only dispatch path. The optional
task/object amplification variant remains a separate follow-up and is not
claimed as an release acceptance requirement.

## What the numbers mean

The mingw-w64 headers used by llvm-mingw renumber several `D3D12_FEATURE`
values. `flprobe.c` first queries the official Microsoft IDs and then uses the
known mingw IDs for the affected options. The fallback is ABI handling, not a
feature override. No environment option is used to manufacture the 12_2
result.
