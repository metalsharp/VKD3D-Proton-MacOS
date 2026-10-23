/* GPU test: sparse images and buffers (vkQueueBindSparse, residency feedback) on MoltenVK. */
#include "vkcommon.h"

#define BLOCK 65536u

static void bind_sparse(struct vkctx *c, const VkBindSparseInfo *info, VkSemaphore signal, uint64_t value)
{
    VkTimelineSemaphoreSubmitInfo ts = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, NULL, 0, NULL, 1, &value };
    VkBindSparseInfo bi = *info;
    bi.pNext = &ts;
    bi.signalSemaphoreCount = 1;
    bi.pSignalSemaphores = &signal;
    CHECK(vkQueueBindSparse(c->queue, 1, &bi, VK_NULL_HANDLE));
}

static void submit_waiting(struct vkctx *c, VkCommandBuffer cb, VkSemaphore wait, uint64_t value)
{
    CHECK(vkEndCommandBuffer(cb));
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkTimelineSemaphoreSubmitInfo ts = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, NULL, 1, &value, 0, NULL };
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, &ts, 1, &wait, &stage, 1, &cb, 0, NULL };
    CHECK(vkQueueSubmit(c->queue, 1, &si, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(c->queue));
    vkFreeCommandBuffers(c->device, c->pool, 1, &cb);
}

int main(int argc, char **argv)
{
    const char *spv = argc > 1 ? argv[1] : "sparse_comp.spv";
    struct vkctx c;

    VkPhysicalDeviceVulkan12Features v12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    v12.timelineSemaphore = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v12 };
    f2.features.sparseBinding = f2.features.sparseResidencyBuffer = f2.features.sparseResidencyImage2D = VK_TRUE;
    f2.features.shaderResourceResidency = f2.features.shaderResourceMinLod = VK_TRUE;
    static const char *const de[] = { "VK_KHR_portability_subset" };
    vkctx_init(&c, &f2, de, 1);

    VkPhysicalDeviceFeatures feats;
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceFeatures(c.physical, &feats);
    vkGetPhysicalDeviceProperties(c.physical, &props);
    VkQueueFamilyProperties qf[8];
    uint32_t qfn = 8;
    vkGetPhysicalDeviceQueueFamilyProperties(c.physical, &qfn, qf);
    printf("sparseBinding %u residencyBuffer %u residencyImage2D %u residencyImage3D %u aliased %u shaderResourceResidency %u minLod %u\n",
           feats.sparseBinding, feats.sparseResidencyBuffer, feats.sparseResidencyImage2D, feats.sparseResidencyImage3D,
           feats.sparseResidencyAliased, feats.shaderResourceResidency, feats.shaderResourceMinLod);
    printf("standard2D %u standard3D %u alignedMipSize %u nonResidentStrict %u queue0 sparse %u addressSpace %llu\n",
           props.sparseProperties.residencyStandard2DBlockShape, props.sparseProperties.residencyStandard3DBlockShape,
           props.sparseProperties.residencyAlignedMipSize, props.sparseProperties.residencyNonResidentStrict,
           !!(qf[0].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT), (unsigned long long)props.limits.sparseAddressSpaceSize);
    if (!feats.sparseResidencyImage2D)
        return 1;

    VkFormat formats[] = { VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R16G16B16A16_SFLOAT,
                           VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC7_UNORM_BLOCK, VK_FORMAT_D32_SFLOAT };
    for (unsigned i = 0; i < sizeof(formats) / sizeof(formats[0]); i++)
    {
        VkSparseImageFormatProperties sp;
        uint32_t n = 1;
        vkGetPhysicalDeviceSparseImageFormatProperties(c.physical, formats[i], VK_IMAGE_TYPE_2D, VK_SAMPLE_COUNT_1_BIT,
                                                       VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_TILING_OPTIMAL, &n, &sp);
        printf("format %d: %u entries granularity %ux%ux%u flags 0x%x\n", formats[i], n, sp.imageGranularity.width,
               sp.imageGranularity.height, sp.imageGranularity.depth, n ? sp.flags : 0);
        EXPECT_EQ("standard block shape", n ? sp.flags & VK_SPARSE_IMAGE_FORMAT_NONSTANDARD_BLOCK_SIZE_BIT : 1, 0);
    }

    VkSemaphoreTypeCreateInfo stci = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, NULL, VK_SEMAPHORE_TYPE_TIMELINE, 0 };
    VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &stci };
    VkSemaphore timeline;
    CHECK(vkCreateSemaphore(c.device, &sci, NULL, &timeline));
    uint64_t tv = 0;

    /* Sparse image with a full mip chain. */
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT,
                              VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, { 512, 512, 1 }, 10, 1, VK_SAMPLE_COUNT_1_BIT,
                              VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE,
                              0, NULL, VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage image;
    CHECK(vkCreateImage(c.device, &ici, NULL, &image));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(c.device, image, &mr);
    VkSparseImageMemoryRequirements sr;
    uint32_t srn = 1;
    vkGetImageSparseMemoryRequirements(c.device, image, &srn, &sr);
    printf("image: size %llu align %llu types 0x%x; granularity %ux%u tailFirstLod %u tailSize %llu tailOffset %llu stride %llu\n",
           (unsigned long long)mr.size, (unsigned long long)mr.alignment, mr.memoryTypeBits, sr.formatProperties.imageGranularity.width,
           sr.formatProperties.imageGranularity.height, sr.imageMipTailFirstLod, (unsigned long long)sr.imageMipTailSize,
           (unsigned long long)sr.imageMipTailOffset, (unsigned long long)sr.imageMipTailStride);
    EXPECT_EQ("granularity", sr.formatProperties.imageGranularity.width, 128);

    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, 8 * BLOCK,
                                 vkctx_memory_type(&c, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    VkDeviceMemory pool;
    CHECK(vkAllocateMemory(c.device, &mai, NULL, &pool));

    VkSparseImageMemoryBind tile = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 }, { 0, 0, 0 }, { 128, 128, 1 }, pool, 0, 0 };
    VkSparseImageMemoryBindInfo tileInfo = { image, 1, &tile };
    VkSparseMemoryBind tail = { sr.imageMipTailOffset, sr.imageMipTailSize, pool, BLOCK, 0 };
    VkSparseImageOpaqueMemoryBindInfo tailInfo = { image, 1, &tail };
    VkBindSparseInfo bind = { VK_STRUCTURE_TYPE_BIND_SPARSE_INFO };
    bind.imageOpaqueBindCount = 1; bind.pImageOpaqueBinds = &tailInfo;
    bind.imageBindCount = 1; bind.pImageBinds = &tileInfo;

    /* Sparse buffer of 4 blocks, block 1 bound. */
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT,
                               4 * BLOCK, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    VkBuffer sbuf;
    CHECK(vkCreateBuffer(c.device, &bci, NULL, &sbuf));
    VkSparseMemoryBind bblock = { BLOCK, BLOCK, pool, 4 * BLOCK, 0 };
    VkSparseBufferMemoryBindInfo bbind = { sbuf, 1, &bblock };
    bind.bufferBindCount = 1; bind.pBufferBinds = &bbind;
    bind_sparse(&c, &bind, timeline, ++tv);

    /* Pipeline. */
    VkSamplerCreateInfo smp = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    smp.magFilter = smp.minFilter = VK_FILTER_NEAREST; smp.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST; smp.maxLod = 16;
    VkSampler sampler;
    CHECK(vkCreateSampler(c.device, &smp, NULL, &sampler));
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, image, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM,
                                  { 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 10, 0, 1 } };
    VkImageView view;
    CHECK(vkCreateImageView(c.device, &vci, NULL, &view));
    VkDescriptorSetLayoutBinding b[3] = {
        { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
        { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
    };
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 3, b };
    VkDescriptorSetLayout dsl;
    CHECK(vkCreateDescriptorSetLayout(c.device, &dli, NULL, &dsl));
    VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 4 };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl, 1, &pcr };
    VkPipelineLayout pl;
    CHECK(vkCreatePipelineLayout(c.device, &pli, NULL, &pl));
    VkComputePipelineCreateInfo cpi = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    cpi.stage = (VkPipelineShaderStageCreateInfo){ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT,
                                                   vkctx_shader(&c, spv), "main", NULL };
    cpi.layout = pl;
    VkPipeline pipe;
    CHECK(vkCreateComputePipelines(c.device, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe));
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 } };
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 2, ps };
    VkDescriptorPool dp;
    CHECK(vkCreateDescriptorPool(c.device, &dpi, NULL, &dp));
    VkDescriptorSetAllocateInfo dsa = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dp, 1, &dsl };
    VkDescriptorSet set;
    CHECK(vkAllocateDescriptorSets(c.device, &dsa, &set));
    struct vkbuf out = vkctx_buffer(&c, 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    VkDescriptorImageInfo ii = { sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorBufferInfo oi = { out.buffer, 0, VK_WHOLE_SIZE }, si = { sbuf, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w[3] = {
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii, NULL, NULL },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &oi, NULL },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &si, NULL },
    };
    vkUpdateDescriptorSets(c.device, 3, w, 0, NULL);

    /* Clear every mip (writes to unbound tiles are dropped), then sample and write the sparse buffer. */
    VkCommandBuffer cb = vkctx_begin(&c);
    VkImageMemoryBarrier ib = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image,
                                { VK_IMAGE_ASPECT_COLOR_BIT, 0, 10, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    VkClearColorValue clear = { { 0.25f, 0.5f, 0.75f, 1.0f } };
    vkCmdClearColorImage(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &ib.subresourceRange);
    ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; ib.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &set, 0, NULL);
    uint32_t write_buffer = 1;
    vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &write_buffer);
    vkCmdDispatch(cb, 1, 1, 1);
    submit_waiting(&c, cb, timeline, tv);

    float *col = (float *)out.map;
    uint32_t *res = (uint32_t *)out.map + 16;
    printf("mip0 tile(0,0): %.2f resident %u | tile(1,0): %.2f resident %u | mip5 (tail): %.2f resident %u\n",
           col[0], res[0], col[4], res[1], col[8], res[2]);
    EXPECT_EQ("mapped tile resident", res[0], 1);
    EXPECT_EQ("mapped tile value", (int)(col[0] * 100 + 0.5f), 25);
    EXPECT_EQ("unmapped tile not resident", res[1], 0);
    EXPECT_EQ("unmapped tile reads 0", col[4] == 0.0f, 1);
    EXPECT_EQ("mip tail resident", res[2], 1);
    EXPECT_EQ("mip tail value", (int)(col[9] * 100 + 0.5f), 50);

    /* Read the sparse buffer back in a later submission. */
    struct vkbuf rb = vkctx_buffer(&c, 4 * BLOCK, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    cb = vkctx_begin(&c);
    VkBufferCopy copy = { 0, 0, 4 * BLOCK };
    vkCmdCopyBuffer(cb, sbuf, rb.buffer, 1, &copy);
    vkctx_submit(&c, cb);
    uint32_t *words = rb.map;
    for (uint32_t blk = 0; blk < 4; blk++)
    {
        uint32_t wi = blk * (BLOCK / 4) + 7;
        char what[64];
        snprintf(what, sizeof(what), "sparse buffer block %u", blk);
        EXPECT_EQ(what, words[wi], blk == 1 ? (0xA0000000u | wi) : 0);
    }

    /* Unbind tile (0,0): it becomes non-resident. */
    tile.memory = VK_NULL_HANDLE;
    VkBindSparseInfo unbind = { VK_STRUCTURE_TYPE_BIND_SPARSE_INFO };
    unbind.imageBindCount = 1; unbind.pImageBinds = &tileInfo;
    bind_sparse(&c, &unbind, timeline, ++tv);
    cb = vkctx_begin(&c);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &set, 0, NULL);
    write_buffer = 0;
    vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &write_buffer);
    vkCmdDispatch(cb, 1, 1, 1);
    submit_waiting(&c, cb, timeline, tv);
    printf("after unbind: tile(0,0) %.2f resident %u\n", col[0], res[0]);
    EXPECT_EQ("unbound tile not resident", res[0], 0);

    printf("%s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
