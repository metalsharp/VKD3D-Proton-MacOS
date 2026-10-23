/* Prints the Vulkan inputs to VKD3D-Proton's feature level 12_2 gates that are not yet met. */
#include "vkcommon.h"
int main(void)
{
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "q", 1, "q", 1, VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL };
    VkInstance inst; VkPhysicalDevice pd; uint32_t n = 1;
    CHECK(vkCreateInstance(&ici, NULL, &inst));
    CHECK(vkEnumeratePhysicalDevices(inst, &n, &pd));
    VkPhysicalDeviceFragmentShadingRateFeaturesKHR fsr = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR };
    VkPhysicalDeviceRayQueryFeaturesKHR rq = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR, &fsr };
    VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR, &rq };
    VkPhysicalDeviceRayTracingMaintenance1FeaturesKHR rtm = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_MAINTENANCE_1_FEATURES_KHR, &rtp };
    VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT img64 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_ATOMIC_INT64_FEATURES_EXT, &rtm };
    VkPhysicalDeviceFeatures2 f = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &img64 };
    vkGetPhysicalDeviceFeatures2(pd, &f);
    VkPhysicalDeviceFragmentShadingRatePropertiesKHR fsrp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR };
    VkPhysicalDeviceProperties2 p = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &fsrp };
    vkGetPhysicalDeviceProperties2(pd, &p);
#define P(s, m) printf("%-44s %llu\n", #m, (unsigned long long)(s).m)
    P(f.features, depthBounds);
    P(rtp, rayTracingPipeline); P(rtp, rayTraversalPrimitiveCulling); P(rtp, rayTracingPipelineTraceRaysIndirect);
    P(rq, rayQuery); P(rtm, rayTracingMaintenance1); P(rtm, rayTracingPipelineTraceRaysIndirect2);
    P(fsr, pipelineFragmentShadingRate); P(fsr, primitiveFragmentShadingRate); P(fsr, attachmentFragmentShadingRate);
    P(img64, shaderImageInt64Atomics); P(img64, sparseImageInt64Atomics);
    static const VkFormat fmts[] = { VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16_UNORM, VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                                     VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R8G8_SNORM };
    for (unsigned i = 0; i < sizeof(fmts) / sizeof(fmts[0]); i++)
    {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(pd, fmts[i], &fp);
        printf("AS vertex format %-4d %d\n", fmts[i], !!(fp.bufferFeatures & VK_FORMAT_FEATURE_ACCELERATION_STRUCTURE_VERTEX_BUFFER_BIT_KHR));
    }
    uint32_t extn = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &extn, NULL);
    VkExtensionProperties *ext = calloc(extn, sizeof(*ext));
    vkEnumerateDeviceExtensionProperties(pd, NULL, &extn, ext);
    for (uint32_t i = 0; i < extn; i++)
        if (strstr(ext[i].extensionName, "ray") || strstr(ext[i].extensionName, "shading_rate") || strstr(ext[i].extensionName, "acceleration"))
            printf("ext %s\n", ext[i].extensionName);
    return 0;
}
