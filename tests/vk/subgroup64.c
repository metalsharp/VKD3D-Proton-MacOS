/* GPU test: 64-bit subgroup operations (shaderSubgroupExtendedTypes), lowered by SPIRV-Cross to 32-bit
 * halves or a walk over the active lanes. Assumes 32-wide subgroups (Apple GPUs). */
#include "vkcommon.h"

int main(int argc, char **argv)
{
    const char *spv = argc > 1 ? argv[1] : "subgroup64_comp.spv";
    enum { N = 64 };
    struct vkctx c;
    VkPhysicalDeviceVulkan12Features v12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v12 };
    v12.shaderSubgroupExtendedTypes = VK_TRUE;
    f2.features.shaderInt64 = VK_TRUE;
    static const char *const de[] = { "VK_KHR_portability_subset" };
    vkctx_init(&c, &f2, de, 1);

    VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &b };
    VkDescriptorSetLayout dsl;
    CHECK(vkCreateDescriptorSetLayout(c.device, &dli, NULL, &dsl));
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl, 0, NULL };
    VkPipelineLayout pl;
    CHECK(vkCreatePipelineLayout(c.device, &pli, NULL, &pl));
    VkComputePipelineCreateInfo cpi = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    cpi.stage = (VkPipelineShaderStageCreateInfo){ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT,
                                                   vkctx_shader(&c, spv), "main", NULL };
    cpi.layout = pl;
    VkPipeline pipe;
    CHECK(vkCreateComputePipelines(c.device, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe));
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 1, &ps };
    VkDescriptorPool dp;
    CHECK(vkCreateDescriptorPool(c.device, &dpi, NULL, &dp));
    VkDescriptorSetAllocateInfo dsa = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dp, 1, &dsl };
    VkDescriptorSet set;
    CHECK(vkAllocateDescriptorSets(c.device, &dsa, &set));
    struct vkbuf out = vkctx_buffer(&c, N * 8 * 8, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    VkDescriptorBufferInfo bi = { out.buffer, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi, NULL };
    vkUpdateDescriptorSets(c.device, 1, &w, 0, NULL);

    VkCommandBuffer cb = vkctx_begin(&c);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &set, 0, NULL);
    vkCmdDispatch(cb, 1, 1, 1);
    vkctx_submit(&c, cb);

    const uint64_t *o = out.map;
#define V(i) (((uint64_t)(i) << 33) | (uint64_t)(i))
    for (uint32_t i = 0; i < N; i++)
    {
        uint32_t base = i & ~31u;
        uint64_t or_ = 0, sum = 0, incl = 0, excl = 0;
        for (uint32_t l = base; l < base + 32; l++)
        {
            or_ |= V(l);
            sum += V(l);
            if (l <= i) incl += V(l);
            if (l < i) excl += V(l);
        }
        uint64_t want[8] = { or_, sum, incl, excl, V(base + 31), V(base), V(i ^ 1), (uint64_t)((int64_t)V(base) - 100) };
        static const char *names[8] = { "or", "add", "inclusive add", "exclusive add", "max", "broadcast first", "shuffle xor", "signed min" };
        for (int k = 0; k < 8; k++)
        {
            char what[48];
            snprintf(what, sizeof(what), "lane %u %s", i, names[k]);
            EXPECT_EQ(what, o[i * 8 + k], want[k]);
        }
    }
    printf("%s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
