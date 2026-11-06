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
- Min/max sampler reduction reads as linear filtering on GPUs before Apple10 with macOS 26 (SPIR-V for PSOs with
  such samplers).
- Typed SRVs of three 32-bit components have no Metal texel format (DXIL: dxil-spirv's meta flag; DXBC: the
  reflection check).
- `StructuredBuffer<float4>` accesses become four 4-byte loads or stores even with `rawBufferLoad` alignment 16: about
  1.5x the GPU time of SPIR-V on streaming kernels. It isn't fixable from the DXIL.
- Metal `sin` and `cos` return 0 for infinite or NaN arguments, on either path.
- `SV_Barycentrics` (perspective) reads wrong values when it's the fragment function's only stage input (Apple M4,
  macOS 27.2: `(l0, l1 / 2, l2 + l1 / 2)` for a triangle with w = 1, 2 and 4), and the converter passes Metal's
  `barycentric_coord` straight through. Barycentric shaders stay on SPIR-V (meta flag). There, SPIRV-Cross gives such a
  fragment function a `[[position]]` input, which makes them right. Test `barycentrics`.

## SPIR-V path limits found on the way

- One pixel shader reading both perspective and noperspective `SV_Barycentrics` needs MSL 4.0. Before it, Metal takes
  one `barycentric_coord` per fragment function, so on macOS 14 and 15 (and `METALSHARP_METAL_VERSION=3`) the pipeline
  fails. SPIRV-Cross refused it on MSL 4 too, until this branch.
