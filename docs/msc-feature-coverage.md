# Metal Shader Converter: shader feature coverage

What Apple's Metal Shader Converter (4.0b3, `IR_VERSION` 4.0.3) does with each shader feature, from
`tests/msc-bridge/featuresweep.py`. That script compiles one HLSL snippet per feature with DXC and converts it with
`tests/msc-bridge/mscconv` (bounds checks on, NaN and Inf kept, as MoltenVK converts). Re-run it on a new converter
release.

- **ok:** converts, and the output was checked where noted.
- **WARN:** converts, but the converter drops the feature with a warning. That is an accuracy risk: the bridge has to
  keep such shaders on SPIR-V, unless dropping the feature changes nothing on this platform.
- **FAILED:** conversion fails. VKD3D-Proton creates the pipeline through SPIR-V instead (compute and graphics both
  fall back). Features that dxil-spirv's meta flags name are rejected before conversion is tried.

## Dropped with a warning

| Feature | Converter | Bridge |
| --- | --- | --- |
| `SV_CullDistance` (vertex and mesh outputs) | `Unsupported: culldistance`; primitives aren't culled | Stays on SPIR-V: MoltenVK passes cull distances on as clip distances. Test `cull_clip_distance_dxil` and `_dxbc`. |
| `SV_ShadingRate` (vertex, mesh primitive output, pixel input) | `Unsupported: shadingrate` | Converts. VRS tier 2 is emulated, and SPIR-V doesn't apply a per-primitive rate either. |

## Fails to convert

| DXIL operation | HLSL | Bridge |
| --- | --- | --- |
| `viewID` | `SV_ViewID` | View instancing PSOs stay on SPIR-V. |
| `innerCoverage` | `SV_InnerCoverage` | Falls back on failure. |
| `attributeAtVertex` | `GetAttributeAtVertex` | Barycentrics meta flag. |
| `textureStoreSample` | SM 6.7 `RWTexture2DMS` writes | Falls back on failure. |
| `splitDouble` | `asuint(double, lo, hi)`, also from DXC's double lowering | Falls back on failure. |
| `atomicBinOp.i64` | 64-bit atomics on buffers (`InterlockedAdd64`) | 64-bit atomics meta flag. |
| `textureGatherRaw` | SM 6.7 `GatherRaw` | Falls back on failure. |
| `writeSamplerFeedbackLevel` | Sampler feedback | Falls back on failure. |

The converter also rejects some features the sweep doesn't cover: ray queries (no acceleration-structure binding,
kept on SPIR-V before conversion) and 64-bit wave ops on some types (`Unsupported 64bit wave op`).

## Converts

- **Vertex:** `SV_ClipDistance`; `SV_RenderTargetArrayIndex` and `SV_ViewportArrayIndex` outputs; SM 6.8
  `SV_StartVertexLocation` and `SV_StartInstanceLocation` (through the draw parameters MoltenVK binds).
- **Pixel:**
  - `SV_StencilRef`; `SV_Coverage` in and out
  - `SV_Depth`; `SV_DepthGreaterEqual` and `SV_DepthLessEqual` (Metal `depth(greater)` and `depth(less)`)
  - `SV_SampleIndex`, `SV_IsFrontFace`, `SV_PrimitiveID`, `SV_RenderTargetArrayIndex` input; `SV_Barycentrics`
  - `discard`; coarse and fine derivatives; `EvaluateAttributeAtSample` and `EvaluateAttributeSnapped`
  - sampling:
    - `SampleCmp`, `SampleCmpLevelZero`, SM 6.7 `SampleCmpLevel`
    - `CalculateLevelOfDetail` (and `Unclamped`)
    - bias, gradient, offsets; LOD clamp
    - gathers: by component, compare gathers, programmable offsets
  - `Texture2DMS` sample positions; quad reads, SM 6.7 `QuadAny` and `QuadAll`, SM 6.6 `IsHelperLane`
  - UAV writes and rasterizer-ordered views
- **Compute:**
  - wave ops:
    - basics, prefix ops and bit ops; `WaveReadLaneAt`
    - SM 6.5 `WaveMatch` and `WaveMultiPrefix*`
    - SM 6.6 `WaveSize`, SM 6.8 wave size ranges
    - 64-bit integer wave sums
  - arithmetic:
    - int64 and double arithmetic; 16-bit types
    - SM 6.4 `dot4add`, `dot2add`; SM 6.6 `pack`/`unpack`
    - bit ops, `msad4`, half conversions
  - atomics: groupshared (32- and SM 6.6 64-bit), SM 6.6 float compare-exchange, typed UAV
  - resources:
    - SM 6.6 `ResourceDescriptorHeap`/`SamplerDescriptorHeap`; `NonUniformResourceIndex`
    - SM 6.6 compute derivatives; `GetDimensions`
  - SM 6.8 `Barrier`
- **Mesh:** basic output, `SV_CullPrimitive`, per-primitive `SV_RenderTargetArrayIndex` and `SV_ViewportArrayIndex`.

## Converts, but wrong or slow

These convert cleanly, but tests found them wrong or slow, so the bridge treats them specially:

- `CheckAccessFullyMapped` reads false for mapped tiles (residency meta flag: SPIR-V).
- Min/max sampler reduction reads as linear filtering on GPUs before Apple10 with macOS 26. PSOs with such static
  samplers use SPIR-V. Once the application makes a min/max sampler, PSOs that sample from tables or the heap use
  SPIR-V too: later ones aren't bridged, and earlier ones draw and dispatch with their SPIR-V pipelines (test
  `minmax_heap_late`).
- Typed SRVs of three 32-bit components have no Metal texel format. Shaders declaring one use SPIR-V (DXIL:
  dxil-spirv's meta flag; DXBC: the reflection check). A shader declaring `Buffer<float4>` (or any 32-bit typed SRV)
  may still be given such a view. Once the application makes one, PSOs reading typed buffer SRVs use SPIR-V the same
  way (test `typedbuffer_rgb32_float4`).
- Out-of-bounds buffer loads are slow: the converter loads through a null pointer (offset by the index) instead of
  branching. With half its loads out of bounds, a streaming kernel takes about 1.5x the GPU time of SPIR-V (probe
  scenes `structured_buffer`, `robust_oob`, `null_descriptors`). In bounds it takes the same time
  (`structured_inbounds`, `particles`). `StructuredBuffer<float4>` accesses also become four 4-byte loads or stores,
  even with `rawBufferLoad` alignment 16, but that measured no slower.
- Metal `sin` and `cos` return 0 for infinite or NaN arguments, on either path.
- `SV_Barycentrics` (perspective) reads wrong values when it's the fragment function's only stage input (Apple M4,
  macOS 27.2: `(l0, l1 / 2, l2 + l1 / 2)` for a triangle with w = 1, 2 and 4), and the converter passes Metal's
  `barycentric_coord` straight through. Barycentric shaders stay on SPIR-V (meta flag). There, SPIRV-Cross gives such a
  fragment function a `[[position]]` input, which makes them right. Test `barycentrics`.

## SPIR-V path limits found on the way

- One pixel shader reading both perspective and noperspective `SV_Barycentrics` needs MSL 4.0. Before it, Metal takes
  one `barycentric_coord` per fragment function, so on macOS 14 and 15 (and `METALSHARP_METAL_VERSION=3`) the pipeline
  fails. SPIRV-Cross refused it on MSL 4 too, until this branch.

## Op-level differential test

`d3d12test opdiff` (opt-in: `D3D12TEST_OPDIFF=1`; writes `opdiff-dxil.bin` and `opdiff-dxbc.bin` in the run directory) runs 67 single-operation compute shaders as DXIL
and 64 as DXBC on every pair of about 40 special inputs (zeros, infinities, NaNs, denormals, integer limits) and on
random bits. `tests/run/opdiff.py` compares a run with the bridge against one without. Results on an Apple M4, macOS
27.2, with the D3D11.3 functional spec's rules:

| Operation | Bridge (converter) | SPIR-V | Spec |
| --- | --- | --- | --- |
| `f32tof16` | round toward zero; float16 denormal results rounded to nearest (44 of 4096 inputs) | round to nearest even, and +inf from 65520 up, until this branch; now round toward zero, exact on every input | round toward zero; finite overflow gives the largest finite value (3.2.2) |
| `frc(±inf)` | NaN | NaN (DXBC was 0 until this branch: SPIRV-Cross now selects NaN for MSL `fract`) | NaN |
| `sincos(±inf)`, `sincos(NaN)` | 0, NaN with `-Gis` | DXIL 0; DXBC NaN | NaN; but DXC's default (no `-Gis`) allows the fast form, which the converter (`air.fast_sin`) and SPIR-V both use for DXIL |
| `dp2` of `0 * -0 + -0 * 0` | -0 | DXBC +0 | IEEE: -0 |

Every other operation gives the same bits on both paths: float arithmetic, min/max, rounding, sqrt, rsqrt, rcp, exp2,
log2, saturate, sign, lerp, clamp, NaN/INF tests and compares; integer arithmetic, division, shifts, bit counts, bit
reversal, min/max/abs and compares; float/integer conversions; `f16tof32`; 64-bit integer multiply, shift and
conversion (DXIL).

## Sampler and typed buffer format differential tests

- `d3d12test samplediff` (opt-in: `D3D12TEST_SAMPLEDIFF=1`, writes `samplediff.bin` in the run directory; compare with `tests/run/samplediff.py`): 25
  sampler descriptors from the sampler heap, covering:
  - point, linear and mixed filters; anisotropic 4x and 16x
  - wrap, mirror, clamp, border with three border colors, and mirror once
  - LOD bias, minimum and maximum LOD
  - comparison functions

  Each is used by `SampleLevel`, `SampleGrad`, `Gather` and `SampleCmpLevelZero` at 256 coordinates inside and outside
  [0, 1]. The converter and SPIR-V give the same results to 1e-4, on Metal 4 and on Metal 3.
- `d3d12test formatdiff` (opt-in: `D3D12TEST_FORMATDIFF=1`, writes `formatdiff.bin` in the run directory; compare with `tests/run/formatdiff.py`): 39
  typed buffer formats, read from random bytes through a typed SRV and written through a typed UAV. Both paths load and
  store the same values. The decodes of these formats match a CPU reference exactly:
  - RGBA16F
  - R10G10B10A2_UNORM, R11G11B10_FLOAT
  - RGBA8 UNORM and SNORM, BGRA8
  - RG16 SNORM

  SNORM maps -128 (and -32768) to -1, as D3D requires.
- `d3d12test texdiff` (opt-in: `D3D12TEST_TEXDIFF=1`, writes `texdiff.bin` in the run directory; compare with `tests/run/texdiff.py`): 29 texture
  formats read texel by texel with `Load` through an SRV:
  - sRGB RGBA8, BGRA8 and BC1/BC7
  - BGRX8, RGB9E5, B5G6R5, B5G5R5A1, B4G4R4A4, A8
  - R10G10B10A2, R11G11B10, RG8 SNORM, BC1 to BC7
  - D32, D16, and the depth and stencil planes of D24S8 and D32S8

  Both paths read the same values. Against a CPU reference, the sRGB, 16-bit packed, depth and stencil reads are
  exact, including D24S8, which Apple GPUs keep as D32S8. Stencil arrives in `.g`.
- `d3d12test vtxdiff` (opt-in: `D3D12TEST_VTXDIFF=1`, writes `vtxdiff.bin` in the run directory; compare with `tests/run/vtxdiff.py`): 29 vertex
  formats read by a vertex shader from a per-vertex and a per-instance stream (step rate 2). All 28 formats with a
  Metal vertex format read the same on both paths, and the decodes checked against a CPU reference are exact, with
  missing components reading 0, 0, 1. The formats checked are R32, R32G32, RGBA8 SNORM, R8, BGRA8, R10G10B10A2_UNORM
  and R11G11B10. `R10G10B10A2_UINT` makes no pipeline on either path: Metal only has a normalized 10:10:10:2 vertex
  format. A pipeline with no pixel shader, render target or depth target is made with rasterizer discard, and a converted
  vertex function returns its outputs, which Metal rejects without rasterization: MoltenVK rasterizes such bridge
  pipelines, which draw nothing (test `vs_uav_no_attachments`). An unnormalized 10:10:10:2 vertex format would need
  the vertex shader to fetch its attributes itself, which MoltenVK does only for geometry shaders and conservative
  rasterization; it remains unsupported.
