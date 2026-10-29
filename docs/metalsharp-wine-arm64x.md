# VKD3D-Proton on MetalSharp Wine (ARM64X) and a rebuilt custom MoltenVK

The v1.0 runtime's sources were never published: its `d3d12core.dll` reports
`vkd3d-proton - build: 5d24bc718560d60+` (a local commit that is not in upstream), and the
custom ("VKMT") MoltenVK exists only as `libMoltenVK.dylib`. This document records how both are
being rebuilt from source so that they cannot be lost again, what each D3D12 capability needs
from Vulkan, and what the hardware actually does.

## Sources

| Tree | Base | Branch | Contents |
|---|---|---|---|
| `sources/vkd3d-proton` | upstream master `ef9a02e8` (2026-10-20, reports 3.1.0) | `metalsharp-moltenvk` | exported as `patches/vkd3d-proton-moltenvk.patch` |
| `sources/MoltenVK` | KhronosGroup/MoltenVK main `5189f8de` (2026-10-13) | `metalsharp-vkmt` | dttdrv's `macgaming/ray-query-pr`, `mesh/native`, `fix/specialization-array-sizes`, `fix/tessellation-patch-counts`, then the work below |

SPIRV-Cross comes from dttdrv/SPIRV-Cross `c1af5739` (MSL ray tracing), pinned by the
ray-query branch. `scripts/build-vkd3d-proton-arm64x.sh` builds the D3D12 pair as ARM64X (one DLL
with ARM64 and ARM64EC code), which MetalSharp Wine runs natively in ARM64 and x86_64 programs.

## What the v1.0 runtime really did

The v1.0 VKD3D-Proton changed only two requirement checks (both tagged "(VKMT)" in its strings):
transform feedback became optional (stream output disabled), and
`VKMT_ALLOW_NON_SINGLE_TEXEL_ALIGNMENT` waived single-texel alignment. Its "12_2" came from an
older upstream with fewer checks: on the same VKMT MoltenVK, current upstream measures much
less, because VKMT does not provide what the levels need:

| Capability | VKMT MoltenVK reports | v1.0 claimed |
|---|---|---|
| DXR 1.0/1.1 | `rayTracingPipeline = 0` (ray query only), 64-byte group handles | tier 1.1 |
| Tiled resources 2+ | sparse yes, `shaderResourceResidency = 0` | tier 4 |
| Conservative rasterization 3 | extension present, every property 0 | tier 3 |
| VRS 2 | no `VK_KHR_fragment_shading_rate` | tier 2 |
| SM 6.2+ | `shaderDenormPreserveFloat32 = 0` | SM 6.5 |

(The `flprobe.c` "official"/"mingw" labels are swapped: Microsoft's values are
`SHADER_MODEL = 7`, `OPTIONS5 = 27`, `OPTIONS6 = 30`, `OPTIONS7 = 32`, which both builds answer.)

## Measured on Apple M4 (macOS 27.2)

Out-of-bounds buffer access (`tests/metal/robust.m`):

| Binding | OOB read | OOB write |
|---|---|---|
| whole small buffer bound directly | 0 | dropped |
| GPU address in an argument buffer | garbage | — |
| range at an offset in a larger buffer | neighbouring data | lands |

So `robustBufferAccess2` is not provided by the hardware for D3D12's suballocated descriptors;
it needs bounds checks in the generated MSL.

## Requirements and status

Feature level and shader model gates are in `libs/vkd3d/device.c`
(`d3d12_device_caps_init_feature_level`, the SM ladder around `D3D_SHADER_MODEL_6_x`).

| Need | For | Rebuilt MoltenVK | Work |
|---|---|---|---|
| robustness2 `robustBufferAccess2` | device creation | no | bounds checks in SPIRV-Cross MSL for descriptor buffers |
| robustness2 `nullDescriptor` | device creation | no | dummy zero resources for null descriptors |
| `depthBounds` | FL 12_1+ (OPTIONS2) | no (VKMT emulated) | fragment-shader emulation |
| `logicOp` | FL 11_1 | no (VKMT emulated) | framebuffer-fetch emulation |
| conservative rasterization | FL 12_1 / 12_2 tier 3 | no (VKMT emulated) | port and finish the emulation |
| 64-bit image atomics | sampler feedback, SM 6.6 | no (VKMT patched SPIRV-Cross) | port `scripts/patch-spirv-cross.sh` |
| sparse binding/residency | tiled tier 2/3 | no | Metal sparse textures and placement heaps |
| `shaderResourceResidency` | tiled tier 2 | no | residency feedback |
| geometry shaders | D3D 11_0 content | no | dttdrv `geometry-shaders`, PR #2786 |
| ray tracing pipelines | DXR 1.0/1.1 | yes (on by default; `MVK_CONFIG_ENABLE_EXPERIMENTAL_RAY_TRACING=0` turns it off), 32-byte handles | validate |
| fp32 denorm preserve/flush | SM 6.2 | no | hardware test, then float controls |
| compute derivatives | SM 6.6 | no | quad shuffles in compute |
| 64-bit buffer atomics | SM 6.6 | no (Metal has 64-bit min/max only) | emulation |
| `shaderQuadControl` + maximal reconvergence | SM 6.7 | yes | validate |
| fragment shading rate | VRS tier 2 | no | rasterization rate maps |
| D3D11on12 | D3D11 apps on D3D12 | yes (`shaderCullDistance` as clip distances, `VK_KHR_pipeline_library`) | metalsharp/DXVK-MacOS (DXVK 3.1.1) `dxgi`/`d3d11` as ARM64X (`scripts/build-dxvk-arm64x.sh`), D3D12 test `d3d11on12` |
