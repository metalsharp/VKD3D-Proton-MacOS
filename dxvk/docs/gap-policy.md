# Gap Policy (auto-seeded from capability probe)

*Machine: Apple M4 · MoltenVK 1.4.3 · Vulkan 1.4.357 · generated 2026-10-28 from `docs/baselines/baseline-M4-macOS27-metalsharp-wine-4.0.json`*

| Capability | Status | Evidence (probe) | Action / Phase |
|---|---|---|---|
| Vulkan API ≥ 1.3 (DXVK 3.x hard gate) | **supported** | `apiVersion=1.4.357` | none |
| DXVK 3.1.1 required features and extensions | **supported** | `all 48 present` | none |
| core.geometryShader | **partial** | `geometryShader=True, maxGeometryOutputVertices=0` | MoltenVK reports the feature with zero geometry limits: fix the limits in MoltenVK, then test GS draws (P3.4) |
| core.tessellationShader | **supported** | `tessellationShader=True` | FL11 pipeline test (P3.7) |
| VK_EXT_transform_feedback (stream-out) | **partial** | `transformFeedback=True, geometryStreams=False, maxTransformFeedbackStreams=1` | stream 0 only: test SO draws; multi-stream SO stays unsupported (P3.6) |
| VK_KHR/EXT_robustness2 (robustBufferAccess2) | **supported** | `robustBufferAccess2=True` | if supported: REMOVE the force-off in dxvk_device_info.cpp (P3.3); else keep force-off |
| Depth clip enable (VK_EXT_depth_clip_enable) | **supported** | `depthClipEnable=True, depthClipControl=True` | none: 3.1.1 chains VkPipelineRasterizationDepthClipStateCreateInfoEXT |
| VK_EXT_graphics_pipeline_library (compile stutter) | **hard-gap** | `graphicsPipelineLibrary=False` | port DXVK-async pipe compiler (P4.1–P4.5) |
| Present timing / present wait (upstream 44-commit tail) | **partial** | `presentTiming=False, VK_KHR_present_wait=True` | verify opt-in degrade path (P1.6) |
| Sparse/tiled resources (D3D11) | **supported** | `sparseBinding=True, sparseResidencyImage2D=True` | if supported: test a tiled-resource app; else documented-unsupported (P3.10) |
| Pipeline statistics queries (D3D11) | **hard-gap** | `pipelineStatisticsQuery=False` | graceful-zero + single log line (P3.10): vkCreateQueryPool fails and DxvkGpuQueryAllocator::allocQuery dereferences a null pool, so D3D11 pipeline-statistics queries crash until this lands |
| Custom border colors (D3D11 sampler) | **graceful-fail** | `customBorderColors=False` | DXVK gates on feature; keep default off (P2.1 option audit) |
| Sampler binding aliasing (d3d9) | **shim** | `MVK advertises no aliasing primitive; deAliasedSamplers shim in fork` | auto-enable when unsupported (P2.2) |
| 3.x descriptor-heap path | **fallback** | `descriptorBuffer=False` | verify non-heap fallback engages; bench it (P3.8) |
| Conservative rasterization (D3D11) | **best-effort** | `overestimationSize=0.004` | approximate per MVK; verify visually (P3.10) |
| Dual-source blending / logicOp / independentBlend | **supported** | `dualSrcBlend=True, logicOp=True` | none |

*(statuses: supported · shim · partial · fallback · best-effort · hard-gap · graceful-fail · BLOCKER)*

## Probe notes (2026-10-28)

- **Which MoltenVK:** the one MetalSharp Wine 4.0 ships (`MetalSharp-Wine.app/Contents/lib/libMoltenVK.dylib`, the
  metalsharp-vkmt fork at d1070b55), the MoltenVK both the DXVK lane and the VKD3D-Proton lane load. The earlier
  baseline (`baseline-M4-macOS27.json`, 2026-09-21) read an older VKMT build from a test runtime, which had no geometry
  shaders, transform feedback, depth clip enable or cull distances; it is kept for comparison only.
- **Same results with the lane's settings:** the six `MVK_*`/`VKMT_*` variables MetalSharp sets for its Vulkan lanes
  (private API, synchronous submits and so on) change no feature, limit or extension.
- **Ray tracing extensions** (`VK_KHR_acceleration_structure`, `ray_query`, `ray_tracing_pipeline`) appear only with
  `MVK_CONFIG_ENABLE_EXPERIMENTAL_RAY_TRACING=1`. DXVK does not use them.
- **Geometry shaders** run as Metal mesh pipelines in this MoltenVK, but it reports every `maxGeometry*` limit as 0, so
  any GS pipeline exceeds the reported limits. That needs fixing on the MoltenVK side before GS draws are tested.
- **Pipeline statistics:** MoltenVK fails `vkCreateQueryPool` for `VK_QUERY_TYPE_PIPELINE_STATISTICS`
  (`MVKQueryPool.mm`), so the null-pool crash in `DxvkGpuQueryAllocator::allocQuery` is reachable on MoltenVK.
- **Probe fix:** the probe's Vulkan 1.2 line printed a `shaderFloatControls` field with no argument, which shifted
  every later value on that line by one (`samplerFilterMinmax` onward). Earlier reports' `vulkan12` values after
  `samplerMirrorClampToEdge` are unreliable.

---

## Runtime validation addendum (Phase 2, P2.3–P2.8) — device-level verdicts

*Under MetalSharp wine-11.17 + bundled MoltenVK, DXVK `v3.1.1-macos.0` PE DLLs:*

| Lane | Device create | Clear/Present ×600 | 30-min soak | Verdict (device-level) |
|---|---|---|---|---|
| d3d9 (x64+i386) | ✅ | ✅ | ✅ 28 runs / 0 fail | **supported** |
| d3d8 (x64+i386) | ✅ | ✅ | rides d3d9 | **supported** (rides d3d9 lane) |
| d3d11+dxgi (x64+i386) | ✅ | ✅ | not soaked this phase | **supported** (device-level; soak in Phase 3) |

Full-content rendering (fixed-function variety, samplers edge cases) remains
Tier-2 pending real apps — see docs/TEST-APPS.md slots. Evidence:
`/Volumes/AverySSD/scratch-p012/smoke-runs/` (20260921-*).
