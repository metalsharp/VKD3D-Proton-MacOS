/* GPU test: 64-bit atomics on shared memory (shaderSharedInt64Atomics): add, min, max, compare-exchange
 * and exchange from 128 invocations, in 8 workgroups (whose shared variables have the same addresses). */
#include "vkcommon.h"

int main(int argc, char **argv)
{
    struct vkctx c;
    VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    f12.shaderSharedInt64Atomics = VK_TRUE;
    f12.shaderBufferInt64Atomics = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12 };
    f2.features.shaderInt64 = VK_TRUE;
    vkctx_init(&c, &f2, NULL, 0);
    VkPhysicalDeviceVulkan12Features q12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceFeatures2 q2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &q12 };
    vkGetPhysicalDeviceFeatures2(c.physical, &q2);
    EXPECT_EQ("shaderSharedInt64Atomics", q12.shaderSharedInt64Atomics, 1);

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
    const unsigned groups = 8;
    struct vkbuf buf = vkctx_buffer(&c, groups * 5 * 8, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    memset(buf.map, 0, groups * 5 * 8);
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
    vkCmdDispatch(cb, groups, 1, 1);
    vkctx_submit(&c, cb);
    const uint64_t *r = buf.map;
    for (unsigned gi = 0; gi < groups; gi++)
    {
        const uint64_t *g = r + gi * 5;
        uint64_t sum = 0;
        for (unsigned i = 0; i < 128; i++)
            sum += ((uint64_t)(i + 1) << 33) | gi;
        uint64_t lo = (1ull << 33) | gi, hi = (128ull << 33) | gi;
        int xchg_ok = (g[4] & 0x1ffffffffull) == gi && (g[4] >> 33) >= 1 && (g[4] >> 33) <= 128;
        char name[64];
        snprintf(name, sizeof(name), "group %u add", gi); EXPECT_EQ(name, g[0], sum);
        snprintf(name, sizeof(name), "group %u min", gi); EXPECT_EQ(name, g[1], lo);
        snprintf(name, sizeof(name), "group %u max", gi); EXPECT_EQ(name, g[2], hi);
        snprintf(name, sizeof(name), "group %u compare-exchange", gi); EXPECT_EQ(name, g[3], 128 * 0x100000001ull);
        snprintf(name, sizeof(name), "group %u exchange", gi); EXPECT_EQ(name, xchg_ok, 1);
    }
    printf("%s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
