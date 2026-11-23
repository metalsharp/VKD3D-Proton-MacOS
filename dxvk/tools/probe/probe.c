/*
 * probe.c — DXVK-macOS Vulkan capability probe
 *
 * Loads a Vulkan ICD (MoltenVK) directly via dlopen (no Vulkan loader required),
 * queries every device property/feature/extension DXVK 3.x cares about, and
 * emits a machine-readable JSON report used as evidence for docs/gap-policy.md.
 *
 * Build:  ./build.sh   (clang, vendored headers in include/)
 * Run:    MOLTENVK_LIB=/path/libMoltenVK.dylib ./probe > report.json
 *         (falls back to a list of well-known MoltenVK locations)
 *
 * SPDX-License-Identifier: Apache-2.0  (headers: KhronosGroup/Vulkan-Headers)
 */
#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- function pointers ---- */
static PFN_vkGetInstanceProcAddr p_vkGetInstanceProcAddr;
static PFN_vkEnumerateInstanceExtensionProperties p_vkEnumerateInstanceExtensionProperties;
static PFN_vkEnumerateInstanceLayerProperties p_vkEnumerateInstanceLayerProperties;
static PFN_vkCreateInstance p_vkCreateInstance;

#define INST_FN(name) static PFN_##name name;
INST_FN(vkEnumeratePhysicalDevices)
INST_FN(vkGetPhysicalDeviceProperties)
INST_FN(vkGetPhysicalDeviceProperties2)
INST_FN(vkGetPhysicalDeviceFeatures2)
INST_FN(vkGetPhysicalDeviceQueueFamilyProperties)
INST_FN(vkGetPhysicalDeviceMemoryProperties)
INST_FN(vkGetPhysicalDeviceProperties)
INST_FN(vkEnumerateDeviceExtensionProperties)
INST_FN(vkDestroyInstance)

static void *load_icd(const char *env) {
    const char *candidates[8];
    int n = 0;
    if (env && *env) candidates[n++] = env;
    candidates[n++] = "/Volumes/AverySSD/rainworld-mono-patch/runtime-bundle-original/runtime/wine/lib/moltenvk-vkmt/libMoltenVK.dylib";
    candidates[n++] = "libMoltenVK.dylib";
    candidates[n++] = "/usr/local/lib/libMoltenVK.dylib";
    candidates[n++] = "/opt/homebrew/lib/libMoltenVK.dylib";
    for (int i = 0; i < n; i++) {
        void *h = dlopen(candidates[i], RTLD_NOW | RTLD_LOCAL);
        if (h) { fprintf(stderr, "[probe] ICD: %s\n", candidates[i]); return h; }
    }
    return NULL;
}

static void *getfn(VkInstance inst, const char *name) {
    void *p = (void *)p_vkGetInstanceProcAddr(inst, name);
    if (!p) p = dlsym(RTLD_DEFAULT, name);
    return p;
}

static void json_str(const char *s) {
    putchar('"');
    for (const char *c = s; *c; c++) {
        if (*c == '"' || *c == '\\') putchar('\\');
        putchar(*c);
    }
    putchar('"');
}

int main(int argc, char **argv) {
    const char *env = getenv("MOLTENVK_LIB");
    void *icd = load_icd(env);
    if (!icd) { fprintf(stderr, "[probe] FATAL: no MoltenVK ICD found\n"); return 2; }

    p_vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(icd, "vkGetInstanceProcAddr");
    p_vkEnumerateInstanceExtensionProperties = (PFN_vkEnumerateInstanceExtensionProperties)dlsym(icd, "vkEnumerateInstanceExtensionProperties");
    p_vkCreateInstance = (PFN_vkCreateInstance)dlsym(icd, "vkCreateInstance");
    if (!p_vkGetInstanceProcAddr || !p_vkCreateInstance || !p_vkEnumerateInstanceExtensionProperties) {
        fprintf(stderr, "[probe] FATAL: ICD entry points missing\n"); return 2;
    }

    /* instance extensions present? */
    uint32_t iext_count = 0;
    p_vkEnumerateInstanceExtensionProperties(NULL, &iext_count, NULL);
    VkExtensionProperties iexts[64];
    if (iext_count > 64) iext_count = 64;
    p_vkEnumerateInstanceExtensionProperties(NULL, &iext_count, iexts);
    int have_portability = 0;
    for (uint32_t i = 0; i < iext_count; i++)
        if (!strcmp(iexts[i].extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) have_portability = 1;

    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "dxvk-macos-probe";
    app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app.pEngineName = "dxvk-macos-audit";
    app.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    app.apiVersion = VK_MAKE_API_VERSION(0, 1, 4, 0);

    const char *ext_names[4];
    uint32_t next = 0;
    if (have_portability) ext_names[next++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;

    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = next;
    ici.ppEnabledExtensionNames = ext_names;
    if (have_portability) ici.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;

    VkInstance inst = VK_NULL_HANDLE;
    VkResult r = p_vkCreateInstance(&ici, NULL, &inst);
    if (r != VK_SUCCESS) { fprintf(stderr, "[probe] FATAL: vkCreateInstance=%d\n", r); return 2; }

#define L(name) name = (PFN_##name)getfn(inst, #name)
    L(vkEnumeratePhysicalDevices); L(vkGetPhysicalDeviceProperties);
    L(vkGetPhysicalDeviceProperties2); L(vkGetPhysicalDeviceFeatures2);
    L(vkGetPhysicalDeviceQueueFamilyProperties); L(vkGetPhysicalDeviceMemoryProperties);
    L(vkEnumerateDeviceExtensionProperties); L(vkDestroyInstance);
#undef L

    uint32_t ndev = 0;
    vkEnumeratePhysicalDevices(inst, &ndev, NULL);
    VkPhysicalDevice devs[8];
    if (ndev > 8) ndev = 8;
    vkEnumeratePhysicalDevices(inst, &ndev, devs);

    printf("{\n  \"icd\": "); json_str(env ? env : "auto");
    printf(",\n  \"instance_portability_enumeration\": %s", have_portability ? "true" : "false");
    printf(",\n  \"device_count\": %u", ndev);

    for (uint32_t d = 0; d < ndev; d++) {
        VkPhysicalDevice pd = devs[d];
        printf(",\n  \"device%d\": {\n", d);

        /* ---- properties2 chain: driver, TF props, minmax props, conservative props ---- */
        VkPhysicalDeviceDriverProperties driver = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
        VkPhysicalDeviceTransformFeedbackPropertiesEXT tfp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_PROPERTIES_EXT };
        VkPhysicalDeviceSamplerFilterMinmaxProperties minmax = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_FILTER_MINMAX_PROPERTIES };
        VkPhysicalDeviceConservativeRasterizationPropertiesEXT cons = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONSERVATIVE_RASTERIZATION_PROPERTIES_EXT };
        VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        p2.pNext = &driver;
        driver.pNext = &tfp;
        tfp.pNext = &minmax;
        minmax.pNext = &cons;
        vkGetPhysicalDeviceProperties2(pd, &p2);
        const VkPhysicalDeviceProperties *pp = &p2.properties;

        printf("    \"deviceName\": "); json_str(pp->deviceName); printf(",\n");
        printf("    \"apiVersion\": \"%u.%u.%u\",\n", VK_API_VERSION_MAJOR(pp->apiVersion), VK_API_VERSION_MINOR(pp->apiVersion), VK_API_VERSION_PATCH(pp->apiVersion));
        printf("    \"driverVersionRaw\": %u,\n", pp->driverVersion);
        printf("    \"vendorID\": %u, \"deviceID\": %u,\n", pp->vendorID, pp->deviceID);
        printf("    \"driverID\": %d, \"driverName\": ", (int)driver.driverID); json_str(driver.driverName);
        printf(", \"driverInfo\": "); json_str(driver.driverInfo); printf(",\n");

        /* ---- features2 chain ---- */
        VkPhysicalDeviceVulkan11Features f11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
        VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
        VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        VkPhysicalDeviceVulkan14Features f14 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES };
        VkPhysicalDeviceTransformFeedbackFeaturesEXT ftf = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT };
        VkPhysicalDeviceRobustness2FeaturesKHR fr2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_KHR };
        VkPhysicalDeviceDepthClipEnableFeaturesEXT fdce = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT };
        VkPhysicalDeviceDepthClipControlFeaturesEXT fdcc = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT };
        VkPhysicalDeviceCustomBorderColorFeaturesEXT fcbc = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT };
        VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT fgpl = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT };
        VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT fdiv = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_KHR };
        VkPhysicalDeviceMeshShaderFeaturesEXT fmsh = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT };
        VkPhysicalDevicePresentTimingFeaturesEXT fpt = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_TIMING_FEATURES_EXT };
        VkPhysicalDeviceDescriptorBufferFeaturesEXT fdb = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT };

        VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        f2.pNext = &f11;
        f11.pNext = &f12; f12.pNext = &f13; f13.pNext = &f14; f14.pNext = &ftf;
        ftf.pNext = &fr2; fr2.pNext = &fdce; fdce.pNext = &fdcc; fdcc.pNext = &fcbc;
        fcbc.pNext = &fgpl; fgpl.pNext = &fdiv; fdiv.pNext = &fmsh; fmsh.pNext = &fpt;
        fpt.pNext = &fdb;
        vkGetPhysicalDeviceFeatures2(pd, &f2);

        VkPhysicalDeviceFeatures *c = &f2.features;
#define B(v) ((v) ? "true" : "false")
        printf("    \"core\": { \"geometryShader\": %s, \"tessellationShader\": %s, \"dualSrcBlend\": %s, \"logicOp\": %s, \"multiViewport\": %s, \"samplerAnisotropy\": %s, \"depthBiasClamp\": %s, \"depthClamp\": %s, \"fillModeNonSolid\": %s, \"wideLines\": %s, \"largePoints\": %s, \"occlusionQueryPrecise\": %s, \"pipelineStatisticsQuery\": %s, \"shaderClipDistance\": %s, \"shaderCullDistance\": %s, \"shaderFloat64\": %s, \"shaderInt64\": %s, \"sampleRateShading\": %s, \"sparseBinding\": %s, \"sparseResidencyImage2D\": %s, \"sparseResidencyAliased\": %s, \"variableMultisampleRate\": %s, \"independentBlend\": %s, \"imageCubeArray\": %s, \"fullDrawIndexUint32\": %s, \"textureCompressionBC\": %s },\n",
            B(c->geometryShader), B(c->tessellationShader), B(c->dualSrcBlend), B(c->logicOp), B(c->multiViewport), B(c->samplerAnisotropy), B(c->depthBiasClamp), B(c->depthClamp), B(c->fillModeNonSolid), B(c->wideLines), B(c->largePoints), B(c->occlusionQueryPrecise), B(c->pipelineStatisticsQuery), B(c->shaderClipDistance), B(c->shaderCullDistance), B(c->shaderFloat64), B(c->shaderInt64), B(c->sampleRateShading), B(c->sparseBinding), B(c->sparseResidencyImage2D), B(c->sparseResidencyAliased), B(c->variableMultisampleRate), B(c->independentBlend), B(c->imageCubeArray), B(c->fullDrawIndexUint32), B(c->textureCompressionBC));

        printf("    \"coreRequired\": { \"robustBufferAccess\": %s, \"fragmentStoresAndAtomics\": %s, \"vertexPipelineStoresAndAtomics\": %s, \"multiDrawIndirect\": %s, \"drawIndirectFirstInstance\": %s, \"shaderImageGatherExtended\": %s, \"shaderInt16\": %s, \"shaderSampledImageArrayDynamicIndexing\": %s, \"sparseResidencyBuffer\": %s },\n",
            B(c->robustBufferAccess), B(c->fragmentStoresAndAtomics), B(c->vertexPipelineStoresAndAtomics), B(c->multiDrawIndirect), B(c->drawIndirectFirstInstance), B(c->shaderImageGatherExtended), B(c->shaderInt16), B(c->shaderSampledImageArrayDynamicIndexing), B(c->sparseResidencyBuffer));
        printf("    \"vulkan11\": { \"shaderDrawParameters\": %s, \"storageBuffer16BitAccess\": %s },\n", B(f11.shaderDrawParameters), B(f11.storageBuffer16BitAccess));
        printf("    \"vulkan12Required\": { \"storageBuffer8BitAccess\": %s, \"shaderInt8\": %s },\n", B(f12.storageBuffer8BitAccess), B(f12.shaderInt8));
        printf("    \"vulkan13Required\": { \"computeFullSubgroups\": %s },\n", B(f13.computeFullSubgroups));
        printf("    \"tfFeatures\": { \"geometryStreams\": %s },\n", B(ftf.geometryStreams));
        printf("    \"limits\": { \"maxPushConstantsSize\": %u, \"maxGeometryOutputVertices\": %u, \"maxClipDistances\": %u, \"maxCullDistances\": %u },\n",
            pp->limits.maxPushConstantsSize, pp->limits.maxGeometryOutputVertices, pp->limits.maxClipDistances, pp->limits.maxCullDistances);
        printf("    \"vulkan12\": { \"timelineSemaphore\": %s, \"bufferDeviceAddress\": %s, \"descriptorIndexing\": %s, \"descriptorBindingPartiallyBound\": %s, \"descriptorBindingVariableDescriptorCount\": %s, \"runtimeDescriptorArray\": %s, \"descriptorBindingUpdateUnusedWhilePending\": %s, \"descriptorBindingSampledImageUpdateAfterBind\": %s, \"descriptorBindingStorageBufferUpdateAfterBind\": %s, \"samplerMirrorClampToEdge\": %s, \"samplerFilterMinmax\": %s, \"scalarBlockLayout\": %s, \"hostQueryReset\": %s, \"vulkanMemoryModel\": %s, \"uniformBufferStandardLayout\": %s, \"separateDepthStencilLayouts\": %s, \"imagelessFramebuffer\": %s, \"drawIndirectCount\": %s, \"shaderOutputViewportIndex\": %s, \"shaderOutputLayer\": %s },\n",
            B(f12.timelineSemaphore), B(f12.bufferDeviceAddress), B(f12.descriptorIndexing), B(f12.descriptorBindingPartiallyBound), B(f12.descriptorBindingVariableDescriptorCount), B(f12.runtimeDescriptorArray), B(f12.descriptorBindingUpdateUnusedWhilePending), B(f12.descriptorBindingSampledImageUpdateAfterBind), B(f12.descriptorBindingStorageBufferUpdateAfterBind), B(f12.samplerMirrorClampToEdge), B(f12.samplerFilterMinmax), B(f12.scalarBlockLayout), B(f12.hostQueryReset), B(f12.vulkanMemoryModel), B(f12.uniformBufferStandardLayout), B(f12.separateDepthStencilLayouts), B(f12.imagelessFramebuffer), B(f12.drawIndirectCount), B(f12.shaderOutputViewportIndex), B(f12.shaderOutputLayer));
        printf("    \"vulkan13\": { \"dynamicRendering\": %s, \"synchronization2\": %s, \"shaderDemoteToHelperInvocation\": %s, \"shaderZeroInitializeWorkgroupMemory\": %s, \"inlineUniformBlock\": %s, \"pipelineCreationCacheControl\": %s, \"maintenance4\": %s, \"robustImageAccess\": %s, \"shaderIntegerDotProduct\": %s, \"subgroupSizeControl\": %s },\n",
            B(f13.dynamicRendering), B(f13.synchronization2), B(f13.shaderDemoteToHelperInvocation), B(f13.shaderZeroInitializeWorkgroupMemory), B(f13.inlineUniformBlock), B(f13.pipelineCreationCacheControl), B(f13.maintenance4), B(f13.robustImageAccess), B(f13.shaderIntegerDotProduct), B(f13.subgroupSizeControl));
        printf("    \"vulkan14\": { \"dynamicRenderingLocalRead\": %s, \"pushDescriptor\": %s, \"hostImageCopy\": %s, \"maintenance5\": %s, \"maintenance6\": %s, \"indexTypeUint8\": %s, \"vertexAttributeInstanceRateDivisor\": %s, \"pipelineRobustness\": %s, \"shaderFloatControls2\": %s },\n",
            B(f14.dynamicRenderingLocalRead), B(f14.pushDescriptor), B(f14.hostImageCopy), B(f14.maintenance5), B(f14.maintenance6), B(f14.indexTypeUint8), B(f14.vertexAttributeInstanceRateDivisor), B(f14.pipelineRobustness), B(f14.shaderFloatControls2));
        printf("    \"ext\": { \"transformFeedback\": %s, \"robustBufferAccess2\": %s, \"robustImageAccess2\": %s, \"nullDescriptor\": %s, \"depthClipEnable\": %s, \"depthClipControl\": %s, \"customBorderColors\": %s, \"graphicsPipelineLibrary\": %s, \"vertexAttributeInstanceRateDivisor\": %s, \"taskShader\": %s, \"meshShader\": %s, \"presentTiming\": %s, \"presentAtAbsoluteTime\": %s, \"descriptorBuffer\": %s },\n",
            B(ftf.transformFeedback), B(fr2.robustBufferAccess2), B(fr2.robustImageAccess2), B(fr2.nullDescriptor), B(fdce.depthClipEnable), B(fdcc.depthClipControl), B(fcbc.customBorderColors), B(fgpl.graphicsPipelineLibrary), B(fdiv.vertexAttributeInstanceRateDivisor), B(fmsh.taskShader), B(fmsh.meshShader), B(fpt.presentTiming), B(fpt.presentAtAbsoluteTime), B(fdb.descriptorBuffer));

        printf("    \"tfProperties\": { \"maxTransformFeedbackStreams\": %u, \"transformFeedbackQueries\": %s },\n",
            tfp.maxTransformFeedbackStreams, tfp.transformFeedbackQueries ? "true" : "false");
        printf("    \"conservativeRaster\": { \"primitiveOverestimationSize\": %.3f, \"degenerateTrianglesRasterized\": %s },\n",
            (double)cons.primitiveOverestimationSize, cons.degenerateTrianglesRasterized ? "true" : "false");
        printf("    \"sparseProperties\": { \"residencyStandard2DBlockShape\": %s, \"residencyAlignedMipSize\": %s, \"residencyNonResidentStrict\": %s },\n",
            B(pp->sparseProperties.residencyStandard2DBlockShape), B(pp->sparseProperties.residencyAlignedMipSize), B(pp->sparseProperties.residencyNonResidentStrict));
#undef B

        /* ---- queue families ---- */
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
        VkQueueFamilyProperties qp[8];
        if (nq > 8) nq = 8;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
        printf("    \"queueFamilies\": [ ");
        for (uint32_t q = 0; q < nq; q++)
            printf("{ \"graphics\": %s, \"compute\": %s, \"transfer\": %s, \"sparse\": %s, \"count\": %u }%s",
                (qp[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) ? "true" : "false",
                (qp[q].queueFlags & VK_QUEUE_COMPUTE_BIT) ? "true" : "false",
                (qp[q].queueFlags & VK_QUEUE_TRANSFER_BIT) ? "true" : "false",
                (qp[q].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT) ? "true" : "false",
                qp[q].queueCount, q + 1 < nq ? ", " : " ");
        printf("],\n");

        /* ---- memory ---- */
        VkPhysicalDeviceMemoryProperties mem;
        vkGetPhysicalDeviceMemoryProperties(pd, &mem);
        printf("    \"memoryHeaps\": [ ");
        for (uint32_t m = 0; m < mem.memoryHeapCount && m < 4; m++)
            printf("{ \"bytes\": %llu, \"deviceLocal\": %s }%s",
                (unsigned long long)mem.memoryHeaps[m].size,
                (mem.memoryHeaps[m].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? "true" : "false",
                m + 1 < mem.memoryHeapCount ? ", " : " ");
        printf("],\n");

        /* ---- extensions ---- */
        uint32_t ne = 0;
        vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
        VkExtensionProperties *ex = (VkExtensionProperties *)malloc(sizeof(VkExtensionProperties) * ne);
        vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ex);
        printf("    \"extensionCount\": %u,\n", ne);
        printf("    \"extensions\": [ ");
        for (uint32_t e = 0; e < ne; e++) {
            json_str(ex[e].extensionName);
            if (e + 1 < ne) printf(", ");
        }
        printf(" ]\n");
        free(ex);

        printf("  }");
    }

    printf("\n}\n");
    vkDestroyInstance(inst, NULL);
    (void)argc; (void)argv;
    return 0;
}
