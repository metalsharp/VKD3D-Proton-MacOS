/* GPU test: FP16 denormals are preserved (arithmetic, conversion and storage), as reported by
 * shaderDenormPreserveFloat16. */
#include "vkcommon.h"
#include <math.h>

static uint16_t f2h_bits(float f)
{
    /* Only for the exact values used here. */
    if (f == ldexpf(1, -14)) return 0x0400;
    if (f == ldexpf(1, -10)) return 0x1400;
    return 0;
}

int main(int argc, char **argv)
{
    struct vkctx c;
    VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    f12.shaderFloat16 = VK_TRUE;
    VkPhysicalDeviceVulkan11Features f11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12 };
    f11.storageBuffer16BitAccess = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11 };
    f2.features.shaderInt16 = VK_TRUE;
    vkctx_init(&c, &f2, NULL, 0);
    VkPhysicalDeviceVulkan12Properties p12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES };
    VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &p12 };
    vkGetPhysicalDeviceProperties2(c.physical, &p2);
    printf("shaderDenormPreserveFloat16 %u denormBehaviorIndependence %u\n", p12.shaderDenormPreserveFloat16, p12.denormBehaviorIndependence);
    EXPECT_EQ("shaderDenormPreserveFloat16", p12.shaderDenormPreserveFloat16, 1);
    EXPECT_EQ("denormBehaviorIndependence is not NONE", p12.denormBehaviorIndependence != VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE, 1);

    VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &b };
    VkDescriptorSetLayout dsl;
    CHECK(vkCreateDescriptorSetLayout(c.device, &dli, NULL, &dsl));
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl };
    VkPipelineLayout pl;
    CHECK(vkCreatePipelineLayout(c.device, &pli, NULL, &pl));
    VkShaderModule cs = vkctx_shader(&c, argv[1]);
    VkComputePipelineCreateInfo cpi = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs, "main", NULL }, pl };
    VkPipeline p;
    CHECK(vkCreateComputePipelines(c.device, VK_NULL_HANDLE, 1, &cpi, NULL, &p));
    struct vkbuf buf = vkctx_buffer(&c, 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    memset(buf.map, 0, 256);
    float *in_f = buf.map;
    uint16_t *in_h = (uint16_t *)((char *)buf.map + 16);
    in_f[0] = ldexpf(1, -20);
    in_h[0] = f2h_bits(ldexpf(1, -14));
    in_h[1] = f2h_bits(ldexpf(1, -10));
    in_h[2] = 0x0003;  /* 3 * 2^-24 */
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 1, &ps };
    VkDescriptorPool dp;
    CHECK(vkCreateDescriptorPool(c.device, &dpi, NULL, &dp));
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dp, 1, &dsl };
    VkDescriptorSet ds;
    CHECK(vkAllocateDescriptorSets(c.device, &dai, &ds));
    VkDescriptorBufferInfo bi = { buf.buffer, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ds, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi };
    vkUpdateDescriptorSets(c.device, 1, &w, 0, NULL);
    VkCommandBuffer cb = vkctx_begin(&c);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    vkCmdDispatch(cb, 1, 1, 1);
    vkctx_submit(&c, cb);
    const float *out_f = (const float *)((char *)buf.map + 24);
    const uint16_t *out_h = (const uint16_t *)((char *)buf.map + 56);
    printf("2^-24 %g, 2^-23 %g, 6 * 2^-24 %g, convert 2^-20 %g; stored %#x %#x\n", out_f[0], out_f[1], out_f[2], out_f[3], out_h[0], out_h[1]);
    EXPECT_EQ("denormal product", out_f[0] == ldexpf(1, -24), 1);
    EXPECT_EQ("denormal arithmetic", out_f[1] == ldexpf(1, -23), 1);
    EXPECT_EQ("stored denormal arithmetic", out_f[2] == 6 * ldexpf(1, -24), 1);
    EXPECT_EQ("FP32 to FP16 denormal", out_f[3] == ldexpf(1, -20), 1);
    EXPECT_EQ("denormal stored", out_h[0], 0x0001);
    EXPECT_EQ("denormal through storage", out_h[1], 0x0003);
    printf("%s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
