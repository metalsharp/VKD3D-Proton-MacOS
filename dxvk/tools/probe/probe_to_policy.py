#!/usr/bin/env python3
"""
probe_to_policy.py — convert a capability-probe JSON report into seeded
docs/gap-policy.md rows, with automatic status derivation per capability.

Usage: python3 tools/probe/probe_to_policy.py <probe.json> [> docs/gap-policy.md]
"""
import json, sys, datetime

def load(path):
    with open(path) as f:
        return json.load(f)

def dev0(d):
    return d["device0"]

REQUIRED = [f"core.{n}" for n in "depthBiasClamp depthClamp dualSrcBlend fillModeNonSolid fullDrawIndexUint32 imageCubeArray "
    "independentBlend multiViewport occlusionQueryPrecise sampleRateShading samplerAnisotropy shaderClipDistance shaderInt64 "
    "textureCompressionBC".split()] + \
    [f"coreRequired.{n}" for n in "robustBufferAccess fragmentStoresAndAtomics multiDrawIndirect shaderImageGatherExtended "
    "shaderInt16 shaderSampledImageArrayDynamicIndexing".split()] + \
    ["vulkan11.shaderDrawParameters", "vulkan11.storageBuffer16BitAccess"] + \
    [f"vulkan12.{n}" for n in "bufferDeviceAddress descriptorIndexing descriptorBindingSampledImageUpdateAfterBind "
    "descriptorBindingUpdateUnusedWhilePending descriptorBindingPartiallyBound hostQueryReset runtimeDescriptorArray "
    "samplerMirrorClampToEdge scalarBlockLayout timelineSemaphore uniformBufferStandardLayout vulkanMemoryModel".split()] + \
    ["vulkan12Required.storageBuffer8BitAccess", "vulkan12Required.shaderInt8"] + \
    [f"vulkan13.{n}" for n in "inlineUniformBlock dynamicRendering maintenance4 shaderDemoteToHelperInvocation "
    "shaderZeroInitializeWorkgroupMemory subgroupSizeControl synchronization2".split()] + \
    ["vulkan13Required.computeFullSubgroups"]
REQUIRED_EXTENSIONS = ["VK_KHR_load_store_op_none", "VK_KHR_maintenance5", "VK_KHR_maintenance6", "VK_KHR_swapchain"]

def lookup(dev, path):
    node = dev
    for key in path.split("."):
        if key not in node:
            return None  # a report from the older probe, which did not read this field
        node = node[key]
    return node

def rows(dev, meta):
    api = tuple(int(x) for x in dev["apiVersion"].split("."))
    exts = set(dev["extensions"])
    R = []

    def add(cap, status, evidence, action):
        R.append((cap, status, evidence, action))

    api_ok = api >= (1, 3, 0)
    add("Vulkan API ≥ 1.3 (DXVK 3.x hard gate)",
        "supported" if api_ok else "BLOCKER",
        f"apiVersion={dev['apiVersion']}", "none" if api_ok else "upgrade MoltenVK ≥1.3.0")

    missing = [path for path in REQUIRED if lookup(dev, path) is False]
    missing += [e for e in REQUIRED_EXTENSIONS if e not in exts]
    unread = [path for path in REQUIRED if lookup(dev, path) is None]
    add("DXVK 3.1.1 required features and extensions",
        "BLOCKER" if missing else ("audit" if unread else "supported"),
        "missing: " + ", ".join(missing) if missing else
        f"{len(unread)} not in this report (older probe): rerun tools/probe" if unread else
        f"all {len(REQUIRED) + len(REQUIRED_EXTENSIONS)} present",
        "upgrade MoltenVK" if missing else "none")

    # A geometry shader stage needs output limits as well as the feature bit: D3D11 allows 1024 vertices
    gs = dev["core"]["geometryShader"]
    gs_vertices = dev.get("limits", {}).get("maxGeometryOutputVertices")
    add("core.geometryShader",
        "hard-gap" if not gs else ("partial" if gs_vertices == 0 else "supported"),
        f"geometryShader={gs}, maxGeometryOutputVertices={gs_vertices}",
        "graceful per-call errors + single log line (P3.4/P3.5); GS content unsupported" if not gs else
        "MoltenVK reports the feature with zero geometry limits: fix the limits in MoltenVK, then test GS draws (P3.4)"
        if gs_vertices == 0 else "test GS draws (P3.4)")

    add("core.tessellationShader", "supported" if dev["core"]["tessellationShader"] else "hard-gap",
        f"tessellationShader={dev['core']['tessellationShader']}", "FL11 pipeline test (P3.7)")

    tf = dev["ext"]["transformFeedback"] or "VK_EXT_transform_feedback" in exts
    streams = dev.get("tfFeatures", {}).get("geometryStreams")
    add("VK_EXT_transform_feedback (stream-out)", "hard-gap" if not tf else ("partial" if not streams else "supported"),
        f"transformFeedback={dev['ext']['transformFeedback']}, geometryStreams={streams}, "
        f"maxTransformFeedbackStreams={dev['tfProperties']['maxTransformFeedbackStreams']}",
        "upstream SO creation returns DXGI_ERROR_INVALID_CALL — acceptable graceful-fail (P3.6)" if not tf else
        "stream 0 only: test SO draws; multi-stream SO stays unsupported (P3.6)")

    r2 = dev["ext"]["robustBufferAccess2"]
    add("VK_KHR/EXT_robustness2 (robustBufferAccess2)",
        "supported" if r2 else "hard-gap",
        f"robustBufferAccess2={r2}",
        "if supported: REMOVE the force-off in dxvk_device_info.cpp (P3.3); else keep force-off")

    dce = dev["ext"]["depthClipEnable"] in (True,) or "VK_EXT_depth_clip_enable" in exts
    dcc = dev["ext"]["depthClipControl"]
    add("Depth clip enable (VK_EXT_depth_clip_enable)",
        "supported" if dce else "hard-gap",
        f"depthClipEnable={dev['ext']['depthClipEnable']}, depthClipControl={dcc}",
        "none: 3.1.1 chains VkPipelineRasterizationDepthClipStateCreateInfoEXT" if dce else
        "restore the 1.10 fallback depthClampEnable = !depthClipEnable (P3.3); depth_clip_control cannot replace it")

    gpl = dev["ext"]["graphicsPipelineLibrary"]
    add("VK_EXT_graphics_pipeline_library (compile stutter)",
        "hard-gap" if not gpl else "supported",
        f"graphicsPipelineLibrary={gpl}",
        "port DXVK-async pipe compiler (P4.1–P4.5)")

    pt = dev["ext"]["presentTiming"] or "VK_EXT_present_timing" in exts
    pw = "VK_KHR_present_wait" in exts
    add("Present timing / present wait (upstream 44-commit tail)",
        "supported" if pt else ("partial" if pw else "graceful-fail"),
        f"presentTiming={dev['ext']['presentTiming']}, VK_KHR_present_wait={pw}",
        "verify opt-in degrade path (P1.6)")

    sparse = dev["core"]["sparseBinding"] and dev["core"]["sparseResidencyImage2D"]
    add("Sparse/tiled resources (D3D11)", "supported" if sparse else "hard-gap",
        f"sparseBinding={dev['core']['sparseBinding']}, sparseResidencyImage2D={dev['core']['sparseResidencyImage2D']}",
        "if supported: test a tiled-resource app; else documented-unsupported (P3.10)")

    add("Pipeline statistics queries (D3D11)",
        "hard-gap" if not dev["core"]["pipelineStatisticsQuery"] else "supported",
        f"pipelineStatisticsQuery={dev['core']['pipelineStatisticsQuery']}",
        "graceful-zero + single log line (P3.10): vkCreateQueryPool fails and DxvkGpuQueryAllocator::allocQuery "
        "dereferences a null pool, so D3D11 pipeline-statistics queries crash until this lands"
        if not dev["core"]["pipelineStatisticsQuery"] else "none")

    add("Custom border colors (D3D11 sampler)",
        "supported" if dev["ext"]["customBorderColors"] else "graceful-fail",
        f"customBorderColors={dev['ext']['customBorderColors']}",
        "DXVK gates on feature; keep default off (P2.1 option audit)")

    add("Sampler binding aliasing (d3d9)",
        "shim",
        "MVK advertises no aliasing primitive; deAliasedSamplers shim in fork",
        "auto-enable when unsupported (P2.2)")

    db = dev["ext"]["descriptorBuffer"]
    add("3.x descriptor-heap path",
        "supported" if db else "fallback",
        f"descriptorBuffer={db}",
        "verify non-heap fallback engages; bench it (P3.8)")

    add("Conservative rasterization (D3D11)",
        "best-effort" if "VK_EXT_conservative_rasterization" in exts else "hard-gap",
        f"overestimationSize={dev['conservativeRaster']['primitiveOverestimationSize']}",
        "approximate per MVK; verify visually (P3.10)")

    add("Dual-source blending / logicOp / independentBlend", "supported",
        f"dualSrcBlend={dev['core']['dualSrcBlend']}, logicOp={dev['core']['logicOp']}",
        "none")

    return R

def main():
    d = load(sys.argv[1])
    dev = dev0(d)
    meta = sys.argv[1]
    print("# Gap Policy (auto-seeded from capability probe)")
    print()
    print(f"*Machine: {dev['deviceName']} · MoltenVK {dev['driverInfo']} · Vulkan {dev['apiVersion']} · generated {datetime.date.today().isoformat()} from `{meta}`*")
    print()
    print("| Capability | Status | Evidence (probe) | Action / Phase |")
    print("|---|---|---|---|")
    for cap, status, ev, act in rows(dev, meta):
        print(f"| {cap} | **{status}** | `{ev}` | {act} |")
    print()
    print("*(statuses: supported · shim · partial · fallback · best-effort · hard-gap · graceful-fail · BLOCKER)*")

if __name__ == "__main__":
    main()
