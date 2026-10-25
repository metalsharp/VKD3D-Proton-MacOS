#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#define WINE_VK_HOST
#include "wine/vulkan.h"
typedef VkResult (VKAPI_PTR *pfn_create)(const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*);
#define P(s, f) printf("%-48s %llu\n", #s "." #f, (unsigned long long)(s).f)
int main(void) {
    HMODULE vk = LoadLibraryA("vulkan-1.dll");
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)(void*)GetProcAddress(vk, "vkGetInstanceProcAddr");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "probe", 1, "probe", 1, VK_MAKE_API_VERSION(0, 1, 4, 0)};
    VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    VkInstance inst; VkPhysicalDevice dev; uint32_t n = 1;
    ((pfn_create)(void*)gipa(NULL, "vkCreateInstance"))(&ci, NULL, &inst);
    ((PFN_vkEnumeratePhysicalDevices)(void*)gipa(inst, "vkEnumeratePhysicalDevices"))(inst, &n, &dev);
    PFN_vkGetPhysicalDeviceFeatures2 feat2 = (PFN_vkGetPhysicalDeviceFeatures2)(void*)gipa(inst, "vkGetPhysicalDeviceFeatures2");
    PFN_vkGetPhysicalDeviceProperties2 props2 = (PFN_vkGetPhysicalDeviceProperties2)(void*)gipa(inst, "vkGetPhysicalDeviceProperties2");

    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR};
    VkPhysicalDeviceAccelerationStructureFeaturesKHR asf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, &rtf};
    VkPhysicalDeviceRayQueryFeaturesKHR rqf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR, &asf};
    VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT ilf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT, &rqf};
    VkPhysicalDeviceMeshShaderFeaturesEXT msf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT, &ilf};
    VkPhysicalDeviceVulkan12Features v12f = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &msf};
    VkPhysicalDeviceFeatures2 f = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v12f};
    feat2(dev, &f);

    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR};
    VkPhysicalDeviceConservativeRasterizationPropertiesEXT crp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONSERVATIVE_RASTERIZATION_PROPERTIES_EXT, &rtp};
    VkPhysicalDeviceProperties2 p = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &crp};
    props2(dev, &p);

    P(rtf, rayTracingPipeline); P(rtf, rayTraversalPrimitiveCulling); P(rtf, rayTracingPipelineTraceRaysIndirect);
    P(asf, accelerationStructure); P(rqf, rayQuery);
    P(rtp, maxRayHitAttributeSize); P(rtp, shaderGroupHandleSize); P(rtp, shaderGroupBaseAlignment); P(rtp, shaderGroupHandleAlignment); P(rtp, maxRayRecursionDepth);
    P(f.features, sparseBinding); P(f.features, sparseResidencyBuffer); P(f.features, sparseResidencyImage2D); P(f.features, sparseResidencyImage3D);
    P(f.features, sparseResidencyAliased); P(f.features, shaderResourceResidency); P(f.features, shaderResourceMinLod);
    P(p.properties.sparseProperties, residencyNonResidentStrict); P(p.properties.sparseProperties, residencyStandard2DBlockShape); P(p.properties.sparseProperties, residencyStandard3DBlockShape);
    P(crp, primitiveOverestimationSize); P(crp, primitiveUnderestimation); P(crp, conservativePointAndLineRasterization);
    P(crp, degenerateTrianglesRasterized); P(crp, fullyCoveredFragmentShaderInputVariable); P(crp, conservativeRasterizationPostDepthCoverage);
    P(ilf, fragmentShaderPixelInterlock); P(ilf, fragmentShaderSampleInterlock);
    P(msf, meshShader); P(msf, taskShader);
    P(f.features, shaderInt64); P(f.features, vertexPipelineStoresAndAtomics); P(v12f, shaderOutputLayer); P(v12f, shaderOutputViewportIndex);
    P(f.features, logicOp);
    P(f.features, geometryShader); P(f.features, tessellationShader); P(f.features, dualSrcBlend); P(f.features, depthClamp); P(f.features, depthBounds);
    P(f.features, shaderStorageImageMultisample); P(f.features, shaderStorageImageReadWithoutFormat); P(f.features, multiViewport);
    P(v12f, shaderBufferInt64Atomics); P(v12f, shaderInt8); P(v12f, shaderFloat16);
    {
        VkPhysicalDeviceVulkan12Properties v12p = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
        VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &v12p};
        props2(dev, &p2);
        P(v12p, denormBehaviorIndependence); P(v12p, shaderDenormFlushToZeroFloat32); P(v12p, shaderDenormPreserveFloat32);
        P(v12p, driverID);
    }
    return 0;
}
