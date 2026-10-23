/* Prints the Vulkan inputs to VKD3D-Proton's shader model ladder (SM 6.0-6.7). */
#include "vkcommon.h"
int main(void)
{
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "q", 1, "q", 1, VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL };
    VkInstance inst; VkPhysicalDevice pd; uint32_t n = 1;
    CHECK(vkCreateInstance(&ici, NULL, &inst));
    CHECK(vkEnumeratePhysicalDevices(inst, &n, &pd));
    VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR deriv = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR };
    VkPhysicalDeviceShaderQuadControlFeaturesKHR quad = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_QUAD_CONTROL_FEATURES_KHR, &deriv };
    VkPhysicalDeviceShaderMaximalReconvergenceFeaturesKHR recon = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MAXIMAL_RECONVERGENCE_FEATURES_KHR, &quad };
    VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT img64 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_ATOMIC_INT64_FEATURES_EXT, &recon };
    VkPhysicalDeviceVulkan13Features v13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &img64 };
    VkPhysicalDeviceVulkan12Features v12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &v13 };
    VkPhysicalDeviceFeatures2 f = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v12 };
    vkGetPhysicalDeviceFeatures2(pd, &f);
    VkPhysicalDeviceVulkan13Properties p13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES };
    VkPhysicalDeviceVulkan12Properties p12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES, &p13 };
    VkPhysicalDeviceProperties2 p = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &p12 };
    vkGetPhysicalDeviceProperties2(pd, &p);
#define P(s, m) printf("%-44s %llu\n", #m, (unsigned long long)(s).m)
    P(p12, denormBehaviorIndependence); P(p12, shaderDenormFlushToZeroFloat32); P(p12, shaderDenormPreserveFloat32);
    P(p12, shaderDenormPreserveFloat16); P(p12, shaderDenormFlushToZeroFloat16);
    P(v12, shaderInt8); P(v12, shaderBufferInt64Atomics); P(v12, shaderSharedInt64Atomics); P(f.features, shaderInt64);
    P(img64, shaderImageInt64Atomics);
    P(v13, subgroupSizeControl); P(v13, computeFullSubgroups); P(p13, minSubgroupSize); P(p13, maxSubgroupSize);
    P(p13, requiredSubgroupSizeStages);
    P(deriv, computeDerivativeGroupLinear); P(deriv, computeDerivativeGroupQuads);
    P(recon, shaderMaximalReconvergence); P(quad, shaderQuadControl);
    return 0;
}
