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
  - `SV_Coverage` in and out
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
- `SV_StencilRef` is marked `air.StencilRef` in the converted function, where Metal's attribute is `air.stencil`, so
  Metal ignores it and stencil REPLACE writes the API reference. Pixel shaders writing it stay on SPIR-V (matched by
  system value in DXIL signatures and by name in DXBC ones, where FXC writes 0). Test `ps_depth_stencil_out` (and
  `_dxbc`). The other system values' AIR attributes match what Metal's compiler emits.
- Min/max sampler reduction reads as linear filtering on GPUs before Apple10 with macOS 26. There, PSOs with such
  static samplers use SPIR-V. Once the application makes a min/max sampler, PSOs that sample from tables or the heap
  use SPIR-V too: later ones aren't bridged, and earlier ones draw and dispatch with their SPIR-V pipelines (test
  `minmax_heap_late`). Where Metal samplers reduce natively (Apple10 with macOS 26, this M6), they stay on the bridge.
  VKD3D-Proton learns which from `VK_KHR_portability_subset`'s `samplerMipLodBias`, which MoltenVK sets under the same
  condition.
- Sampler `MipLODBias`: Metal samplers have a LOD bias only on Apple10 GPUs with macOS 26. Before this branch the
  bias was ignored elsewhere (M1 to M4, and macOS 14 and 15) on both paths. Now the bridge puts the bias in each sampler
  descriptor entry's metadata, as `IRDescriptorTableSetSampler` does. With `IRCompatibilityFlagSamplerLODBias`, the
  converter adds it to `Sample`, `SampleLevel` and `SampleBias` (checked in the AIR: it loads the entry's third word
  and adds it to the bias or level). It doesn't add it to `SampleGrad`. So shaders with gradient samples (dxil-spirv
  0007) stay on SPIR-V where a biased static sampler, or once a biased heap sampler exists, can reach them. On SPIR-V,
  SPIRV-Cross adds the bias from the sampler's emulated word (bits 16 to 31, signed 8.8): to `level()` and `bias()`, as
  `bias()` for implicit LODs, and as an `exp2()` scale of gradients. Tests `sampler_lod_bias`, `_dxbc` and `_late`
  (static and heap samplers, biases 2, 3 and -1) pass on every lane, with `METALSHARP_METAL_VERSION=3` standing in
  for an M1 to M4. Every SPIR-V shader carries the emulation, so samplers made after a pipeline apply too. It measured
  within noise in the probe's sampling scenes (`MVK_CONFIG_EMULATE_SAMPLER_LOD_BIAS=0` turns it off).
- SRV `ResourceMinLODClamp`: MoltenVK had no `VK_EXT_image_view_min_lod`, so VKD3D-Proton moved the view's first mip to
  `floor(clamp)`. That was wrong on both paths: every LOD read `floor(clamp) - MostDetailedMip` mips too far, and
  `GetDimensions` reported the moved view. A streaming engine clamping to the first resident mip got every texture that
  many mips blurrier. MoltenVK now has the extension, so views keep their mips. Shaders clamp to the view's minimum LOD:
  - SPIR-V shaders read it from a word MoltenVK keeps next to each texture (SPIRV-Cross `emulate_image_view_min_lod`),
    as `level(max())` and `min_lod_clamp()`. Always on: it measured within noise.
  - Converted shaders take it from the texture entry's metadata (`IRCompatibilityFlagTextureMinLODClamp`), counted from
    the view's first mip. The converter clamps `Sample`, `SampleGrad` and `SampleCmp`, but not `SampleLevel`,
    `SampleCmpLevelZero` or `SampleCmpLevel`, which Vulkan's extension (made to match D3D12) clamps. Shaders with such
    explicit LOD samples (dxil-spirv 0008) use SPIR-V once the application has made a clamped view.
  - Comparisons: SPIR-V shaders clamp them too (`sample_compare` takes `min_lod_clamp()` and `level()`).
  - Tests `srv_min_lod_clamp` (and `_dxbc`): MostDetailedMip 1, clamp 2.5, linear mip filtering;
    `srv_min_lod_clamp_cmp` (and `_dxbc`): `SampleCmp`, `SampleCmpLevelZero` and `SampleCmpLevel` at clamp 2. Every lane.
  - D3D12 defines the clamp for sample LODs, so `Load` reads any mip. Gathers aren't clamped on either path; what
    hardware does there is unverified.
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
- `SV_Barycentrics` (perspective) reads wrong values when it's the fragment function's only stage input (Apple M6,
  macOS 27.2: `(l0, l1 / 2, l2 + l1 / 2)` for a triangle with w = 1, 2 and 4), and the converter passes Metal's
  `barycentric_coord` straight through. Barycentric shaders stay on SPIR-V (meta flag). There, SPIRV-Cross gives such a
  fragment function a `[[position]]` input, which makes them right. Test `barycentrics`.

## Converter compatibility flags

MoltenVK sets `BoundsCheck` (with robustness), `DisableNanInfOptimization` (unless fast math is always on),
`SamplerLODBias` (where Metal samplers have no LOD bias) and `TextureMinLODClamp` (with `VK_EXT_image_view_min_lod`).
The others, checked on an Apple M6 against D3D's rules on both paths (DXIL and DXBC, Metal 4 and 3):

- `PositionInvariance` strips fast-math flags from position math. Without it, a bridge depth pre-pass and a SPIR-V main
  pass with the same vertex shader (transcendentals, `normalize`, `pow`, `rsqrt`) still match to the bit
  (`depth_equal_cross_path`). Not set.
- `SampleNanToZero`: NaN sample coordinates already read as 0 (`sample_nan_coords`). Not set.
- `TexWriteRoundingRTZ`: render target and typed UAV writes already round toward zero into float16, float11 and
  float10 (70000 stores the largest finite value) and to nearest into UNORM (`rt_float_conversion`,
  `uav_float_conversion`). Not set.

## Second accuracy round: tessellation, geometry, mesh, rasterizer depth, heaps

Each test checks D3D's rules directly rather than comparing paths. All pass on every lane (Metal 4 and 3, bridge on
and off), except as noted.

- `tess_domains` (DXIL, DXBC). For each domain, partitioning and factor set, the domain shader records its points and
  the pixel shader counts coverage. Checked: distinct points, points per edge (which also checks the order of the edge
  factors), exactly-once coverage, the rounding of integer, pow2 and fractional factors, the clamp at 64, culled
  patches, and windings. pow2 tessellated as integer, because SPIR-V has no pow2 spacing. The hull shader now rounds
  its factors up to a power of 2 as it stores them (dxil-spirv 0009, with the DXBC declaration found in its tokens).
  Isolines make no pipeline: Metal's tessellator has triangle and quad domains only. MoltenVK skips draws with
  tessellation followed by a geometry shader.
- `gs_features` (DXIL, DXBC): adjacency input order, `SV_PrimitiveID`, `[instance(3)]` with `SV_GSInstanceID`,
  point-to-quad strips with `RestartStrip`, and `SV_RenderTargetArrayIndex` and `SV_ViewportArrayIndex` from a GS.
- `mesh_features` (on the bridge): amplification payloads launching 0 to 2 mesh groups, per-primitive attributes,
  `SV_CullPrimitive`, `SetMeshOutputCounts(0, 0)`, and a group at the 256-primitive limit.
- `raster_depth`:
  - Checked: depth bias on D32 (`2^(e - 23)` units), D24 (`1 / (2^24 - 1)`) and D16 (`1 / 65535`), slope-scaled and
    clamped bias, the viewport depth range, and depth clip on and off.
  - D24 is 32-bit float depth on Apple GPUs, so its bias came out in float units: an eighth of it at depth 0.1, which
    shows as shadow acne. VKD3D-Proton now applies the constant part as a viewport depth offset in D24 steps.
    MoltenVK advertises `VK_EXT_depth_range_unrestricted`, so a viewport may move past 1; depth stays clamped to 1.
  - Depth clip on and off were wrong in front of the near plane while MoltenVK kept every render pass in Metal's
    private OpenGL mode (z clipped against -w, clamped to 0 instead of MinDepth); MoltenVK now uses that mode only for
    draws that need it. `raster_clip` and `ia_restart` cover it.
- `heap_placement`: aliased placed buffers, overlapping buffers, GPU addresses that differ by the heap offsets, a
  placed texture, and placed upload buffers.

## Third accuracy round: pixel outputs, interpolation, vertex input

- `ps_depth_stencil_out`: `SV_Depth`, `SV_DepthGreaterEqual` and `SV_DepthLessEqual`, discard with a depth output,
  `SV_StencilRef` (converter bug above: SPIR-V).
- `interp_samples`: on a 4x target with the standard pattern, `sample` inputs and `EvaluateAttributeAtSample` read the
  sample positions, `GetRenderTargetSamplePosition` returns them (the converter can't convert it: such shaders fail
  over to SPIR-V), and `EvaluateAttributeSnapped(f, (4, -4))` reads center + (1/4, -1/4). On SPIR-V it read 1/16 low:
  Metal's `interpolate_at_offset` is relative to the pixel's corner, and SPIRV-Cross added 1/2 - 1/16 (an Intel
  correction); `interpolate_at_offset_exact` adds 1/2 on Apple GPUs. FXC 10.1 miscompiles `EvaluateAttributeSnapped`
  (it writes all four components of a register it still needs and stores `.zw`), so `_dxbc` doesn't check it.
- Centroid inputs read the center on fully covered pixels. Metal clips primitives to the viewport rather than
  using a guard band, so pixels on the inner edges of a clipped primitive are partly covered by each piece, and
  centroid moves there. A pixel shader that runs per sample evaluates centroid inputs at its sample on some pixels
  and at the center on others; D3D leaves that to the implementation.
- `instance_step_rate`: per-instance data at step rates 0 to 3, start instances, DrawIndexed with a base vertex and
  ExecuteIndirect, on both paths.
- `dxbc_stripped`: DXBC without reflection is bridged (the instruction scanner took `imad` for `customdata`).

## SPIR-V path limits found on the way

- One pixel shader reading both perspective and noperspective `SV_Barycentrics` needs MSL 4.0. Before it, Metal takes
  one `barycentric_coord` per fragment function, so on macOS 14 and 15 (and `METALSHARP_METAL_VERSION=3`) the pipeline
  fails. SPIRV-Cross refused it on MSL 4 too, until this branch.

## Op-level differential test

`d3d12test opdiff` (opt-in: `D3D12TEST_OPDIFF=1`; writes `opdiff-dxil.bin` and `opdiff-dxbc.bin` in the run directory) runs 67 single-operation compute shaders as DXIL
and 64 as DXBC on every pair of about 40 special inputs (zeros, infinities, NaNs, denormals, integer limits) and on
random bits. `tests/run/opdiff.py` compares a run with the bridge against one without. Results on an Apple M6, macOS
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
  formats read by a vertex shader from a per-vertex and a per-instance stream (step rate 2). All 29 formats read the
  same on both paths, and the decodes checked against a CPU reference are exact, with missing components reading 0, 0,
  1. The formats checked are R32, R32G32, RGBA8 SNORM, R8, BGRA8, R10G10B10A2_UNORM and R11G11B10.
  `R10G10B10A2_UINT` has no Metal vertex format (only the normalized 10:10:10:2 one). As Mullion does, the attribute
  is fetched as one R32_UINT word and the vertex shader unpacks it (`(word >> 10 * c) & (c == 3 ? 3 : 0x3ff)`,
  dxil-spirv patch 0006). Such pipelines use SPIR-V, since a converted vertex function reads its attributes as they
  are. Test `vertex_rgb10a2_uint` checks every component against a CPU unpack for DXIL `uint4`, `int4`, `uint3` and
  `uint16_t4`, and DXBC `uint4` and `int2`, per vertex and per instance, next to an R32_UINT attribute in the same
  stream. The format also reports `IA_VERTEX_BUFFER` support. A pipeline with no pixel shader, render target or depth
  target is made with rasterizer discard, and a converted vertex function returns its outputs, which Metal rejects
  without rasterization: MoltenVK rasterizes such bridge pipelines, which draw nothing (test `vs_uav_no_attachments`).
