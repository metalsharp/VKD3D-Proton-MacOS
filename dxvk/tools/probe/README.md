# Vulkan capability probe (tools/probe)

Loads a Vulkan ICD (MoltenVK) **directly via dlopen** — no Vulkan Loader/SDK
required — and reports every device property, feature bit, and extension that
DXVK 3.x depends on, as JSON.

Output feeds `docs/gap-policy.md` (see `probe_to_policy.py`) and serves as the
per-machine evidence artifact required by roadmap Phases 0–4.

## Build
```bash
./build.sh            # clang, vendored headers in include/ (KhronosGroup/Vulkan-Headers)
```

## Run
```bash
# auto-discovers MoltenVK (env var, well-known MetalSharp runtime paths, brew):
MOLTENVK_LIB=/path/to/libMoltenVK.dylib ./bin/probe > report.json

# pretty view:
python3 -m json.tool report.json
```

## Fields that matter most (see docs 05/06 of the audit)
- `apiVersion` — DXVK 3.x hard-requires ≥ 1.3
- `core.geometryShader`, `vulkan13.dynamicRendering`, `ext.transformFeedback`,
  `ext.robustBufferAccess2`, `ext.graphicsPipelineLibrary`,
  `vulkan14.dynamicRenderingLocalRead`, `ext.presentTiming`
- `extensions[]` — availability of VK_EXT_robustness2 (or VK_KHR_robustness2),
  VK_EXT_depth_clip_enable/control, VK_EXT_present_timing, VK_KHR_present_wait

Note: MoltenVK exposes the robustness2 feature struct under either the
`VK_EXT_robustness2` or `VK_KHR_robustness2` name depending on version; the
probe reports the feature bits regardless of which name is advertised.
