/* GPU test: 3D sparse images (Metal's tiles are one slice deep, a nonstandard block shape). */
#include "vkcommon.h"

int main(int argc, char **argv)
{
    const char *spv = argc > 1 ? argv[1] : "sparse3d_comp.spv";
    struct vkctx c;
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    f2.features.sparseBinding = f2.features.sparseResidencyImage3D = f2.features.shaderResourceResidency = VK_TRUE;
    static const char *const de[] = { "VK_KHR_portability_subset" };
    vkctx_init(&c, &f2, de, 1);

    VkSparseImageFormatProperties sp;
    uint32_t n = 1;
    vkGetPhysicalDeviceSparseImageFormatProperties(c.physical, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TYPE_3D, VK_SAMPLE_COUNT_1_BIT,
                                                   VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_TILING_OPTIMAL, &n, &sp);
    printf("3D RGBA8: %u entries, granularity %ux%ux%u, flags 0x%x\n", n, sp.imageGranularity.width, sp.imageGranularity.height,
           sp.imageGranularity.depth, n ? sp.flags : 0);
    EXPECT_EQ("3D sparse format", n, 1);
    if (!n)
        return 1;
    VkExtent3D g = sp.imageGranularity;

    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT,
                              VK_IMAGE_TYPE_3D, VK_FORMAT_R8G8B8A8_UNORM, { 2 * g.width, 2 * g.height, 4 }, 1, 1, VK_SAMPLE_COUNT_1_BIT,
                              VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE,
                              0, NULL, VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage image;
    CHECK(vkCreateImage(c.device, &ici, NULL, &image));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(c.device, image, &mr);
    printf("image %ux%ux4: size %llu (%llu blocks)\n", 2 * g.width, 2 * g.height, (unsigned long long)mr.size,
           (unsigned long long)(mr.size / 65536));
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, 65536,
                                 vkctx_memory_type(&c, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    VkDeviceMemory pool;
    CHECK(vkAllocateMemory(c.device, &mai, NULL, &pool));
    /* Bind the tile at x = 1 tile, y = 0, slice 2. */
    VkSparseImageMemoryBind tile = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 }, { (int32_t)g.width, 0, 2 }, g, pool, 0, 0 };
    VkSparseImageMemoryBindInfo tileInfo = { image, 1, &tile };
    VkBindSparseInfo bind = { VK_STRUCTURE_TYPE_BIND_SPARSE_INFO };
    bind.imageBindCount = 1; bind.pImageBinds = &tileInfo;
    CHECK(vkQueueBindSparse(c.queue, 1, &bind, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(c.queue));

    VkSamplerCreateInfo smp = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    VkSampler sampler;
    CHECK(vkCreateSampler(c.device, &smp, NULL, &sampler));
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, image, VK_IMAGE_VIEW_TYPE_3D, VK_FORMAT_R8G8B8A8_UNORM,
                                  { 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VkImageView view;
    CHECK(vkCreateImageView(c.device, &vci, NULL, &view));
    VkDescriptorSetLayoutBinding b[2] = {
        { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
    };
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 2, b };
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
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 } };
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 2, ps };
    VkDescriptorPool dp;
    CHECK(vkCreateDescriptorPool(c.device, &dpi, NULL, &dp));
    VkDescriptorSetAllocateInfo dsa = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dp, 1, &dsl };
    VkDescriptorSet set;
    CHECK(vkAllocateDescriptorSets(c.device, &dsa, &set));
    struct vkbuf out = vkctx_buffer(&c, 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    VkDescriptorImageInfo ii = { sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorBufferInfo oi = { out.buffer, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w[2] = {
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii, NULL, NULL },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &oi, NULL },
    };
    vkUpdateDescriptorSets(c.device, 2, w, 0, NULL);

    VkCommandBuffer cb = vkctx_begin(&c);
    VkImageMemoryBarrier ib = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image,
                                { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    VkClearColorValue clear = { { 0.25f, 0.5f, 0.75f, 1.0f } };
    vkCmdClearColorImage(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &ib.subresourceRange);
    ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; ib.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &set, 0, NULL);
    vkCmdDispatch(cb, 1, 1, 1);
    vkctx_submit(&c, cb);

    const float *col = out.map;
    const uint32_t *res = (const uint32_t *)out.map + 16;
    static const char *names[4] = { "bound tile", "same xy, slice 1", "slice 2, tile 0", "slice 2, bound tile far corner" };
    static const int want[4] = { 1, 0, 0, 1 };
    for (int i = 0; i < 4; i++)
    {
        printf("%-32s resident %u value %.2f\n", names[i], res[i], col[i * 4]);
        EXPECT_EQ(names[i], res[i], (uint32_t)want[i]);
        EXPECT_EQ(names[i], (int)(col[i * 4] * 100 + 0.5f), want[i] ? 25 : 0);
    }
    printf("%s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
