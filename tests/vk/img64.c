/* GPU test: 64-bit image atomics (VK_EXT_shader_image_atomic_int64), emulated in SPIRV-Cross. The OR
 * target is an R64_UINT view of a mutable R32G32_UINT image, the way VKD3D-Proton stores sampler feedback. */
#include "vkcommon.h"

static VkImage make_image(struct vkctx *c, VkFormat format, uint32_t w, uint32_t layers, VkImageCreateFlags flags, VkDeviceMemory *mem)
{
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, flags, VK_IMAGE_TYPE_2D, format, { w, 2, 1 }, 1, layers,
                              VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                              VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                              VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage image;
    CHECK(vkCreateImage(c->device, &ici, NULL, &image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(c->device, image, &req);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size,
                                 vkctx_memory_type(c, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    CHECK(vkAllocateMemory(c->device, &mai, NULL, mem));
    CHECK(vkBindImageMemory(c->device, image, *mem, 0));
    return image;
}

static VkImageView make_view(struct vkctx *c, VkImage image, VkFormat format, uint32_t layers)
{
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, image,
                                  layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D, format, { 0 },
                                  { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers } };
    VkImageView view;
    CHECK(vkCreateImageView(c->device, &vci, NULL, &view));
    return view;
}

int main(int argc, char **argv)
{
    const char *spv = argc > 1 ? argv[1] : "img64_comp.spv";
    enum { N = 4096 };
    struct vkctx c;
    VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT img64 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_IMAGE_ATOMIC_INT64_FEATURES_EXT };
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &img64 };
    img64.shaderImageInt64Atomics = VK_TRUE;
    f2.features.shaderInt64 = VK_TRUE;
    static const char *const de[] = { "VK_KHR_portability_subset", "VK_EXT_shader_image_atomic_int64" };
    vkctx_init(&c, &f2, de, 2);

    VkDeviceMemory m0, m1, m2;
    VkImage img = make_image(&c, VK_FORMAT_R32G32_UINT, 8, 1, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT, &m0);
    VkImage arr = make_image(&c, VK_FORMAT_R64_UINT, 4, 3, 0, &m1);
    VkImage simg = make_image(&c, VK_FORMAT_R64_SINT, 1, 1, 0, &m2);
    VkImageView views[3] = { make_view(&c, img, VK_FORMAT_R64_UINT, 1), make_view(&c, arr, VK_FORMAT_R64_UINT, 3),
                             make_view(&c, simg, VK_FORMAT_R64_SINT, 1) };
    struct vkbuf out = vkctx_buffer(&c, N * 8, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

    VkDescriptorSetLayoutBinding b[4] = {
        { 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
        { 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
        { 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
    };
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 4, b };
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
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 } };
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 2, ps };
    VkDescriptorPool dp;
    CHECK(vkCreateDescriptorPool(c.device, &dpi, NULL, &dp));
    VkDescriptorSetAllocateInfo dsa = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dp, 1, &dsl };
    VkDescriptorSet set;
    CHECK(vkAllocateDescriptorSets(c.device, &dsa, &set));
    VkDescriptorImageInfo ii[3];
    VkWriteDescriptorSet w[4];
    for (int k = 0; k < 3; k++)
    {
        ii[k] = (VkDescriptorImageInfo){ VK_NULL_HANDLE, views[k], VK_IMAGE_LAYOUT_GENERAL };
        w[k] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, k, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ii[k], NULL, NULL };
    }
    VkDescriptorBufferInfo oi = { out.buffer, 0, VK_WHOLE_SIZE };
    w[3] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 3, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &oi, NULL };
    vkUpdateDescriptorSets(c.device, 4, w, 0, NULL);

    VkCommandBuffer cb = vkctx_begin(&c);
    VkImage images[3] = { img, arr, simg };
    uint32_t layer_counts[3] = { 1, 3, 1 };
    for (int k = 0; k < 3; k++)
    {
        VkImageMemoryBarrier ib = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                    VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, images[k],
                                    { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layer_counts[k] } };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
        VkClearColorValue zero = { { 0 } };
        VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layer_counts[k] };
        vkCmdClearColorImage(cb, images[k], VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &r);
    }
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &set, 0, NULL);
    vkCmdDispatch(cb, N / 64, 1, 1);
    mb = (VkMemoryBarrier){ VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    struct vkbuf rb[3];
    uint32_t widths[3] = { 8, 4, 1 };
    for (int k = 0; k < 3; k++)
    {
        rb[k] = vkctx_buffer(&c, widths[k] * 2 * layer_counts[k] * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        VkBufferImageCopy r = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layer_counts[k] }, { 0, 0, 0 }, { widths[k], 2, 1 } };
        vkCmdCopyImageToBuffer(cb, images[k], VK_IMAGE_LAYOUT_GENERAL, rb[k].buffer, 1, &r);
    }
    vkctx_submit(&c, cb);

    const uint64_t *v = rb[0].map;
    for (uint32_t x = 0; x < 8; x++)
    {
        uint64_t want = 0;
        for (uint32_t bit = x; bit < 64; bit += 8)
            want |= 1ull << bit;
        char what[32];
        snprintf(what, sizeof(what), "or texel %u", x);
        EXPECT_EQ(what, v[x], want);
    }
    v = rb[1].map;   /* layer 2, row 1 */
    for (uint32_t x = 0; x < 4; x++)
    {
        char what[32];
        snprintf(what, sizeof(what), "add texel %u", x);
        EXPECT_EQ(what, v[2 * 8 + 4 + x], (uint64_t)(N / 4) * 0x100000001ull);
    }
    static unsigned char seen[4][N / 4];
    uint32_t bad = 0;
    const uint64_t *ret = out.map;
    for (uint32_t i = 0; i < N; i++)
    {
        uint64_t k = ret[i] / 0x100000001ull;
        if (ret[i] % 0x100000001ull || k >= N / 4 || seen[i & 3][k]++)
            bad++;
    }
    EXPECT_EQ("add returned values", bad, 0);
    EXPECT_EQ("signed min", ((const int64_t *)rb[2].map)[0], -32);
    printf("%s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
