/* GPU test: SM 6.6 compute-shader derivatives and (emulated) 64-bit buffer atomics. */
#include "vkcommon.h"

static VkPipeline compute_pipeline(struct vkctx *c, VkPipelineLayout pl, const char *spv)
{
    VkComputePipelineCreateInfo cpi = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    cpi.stage = (VkPipelineShaderStageCreateInfo){ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT,
                                                   vkctx_shader(c, spv), "main", NULL };
    cpi.layout = pl;
    VkPipeline pipe;
    CHECK(vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe));
    return pipe;
}

static void test_derivatives(struct vkctx *c, const char *spv)
{
    enum { W = 8, LEVELS = 3 };
    float texels[W * W + (W / 2) * (W / 2) + (W / 4) * (W / 4)];
    struct vkimg img = vkctx_image(c, VK_FORMAT_R32_SFLOAT, W, W, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    vkDestroyImageView(c->device, img.view, NULL);
    vkDestroyImage(c->device, img.image, NULL);
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R32_SFLOAT, { W, W, 1 }, LEVELS, 1,
                              VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                              VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED };
    CHECK(vkCreateImage(c->device, &ici, NULL, &img.image));
    CHECK(vkBindImageMemory(c->device, img.image, img.memory, 0));
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img.image, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R32_SFLOAT,
                                  { 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, LEVELS, 0, 1 } };
    CHECK(vkCreateImageView(c->device, &vci, NULL, &img.view));

    /* Each mip holds its own level number, so the sampled value is the LOD picked. */
    struct vkbuf staging = vkctx_buffer(c, sizeof(texels), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    float *t = staging.map;
    VkDeviceSize offsets[LEVELS];
    for (uint32_t l = 0, o = 0; l < LEVELS; l++)
    {
        offsets[l] = o * 4;
        for (uint32_t n = 0; n < (W >> l) * (W >> l); n++)
            t[o++] = (float)l;
    }
    VkCommandBuffer cb = vkctx_begin(c);
    VkImageMemoryBarrier ib = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img.image,
                                { VK_IMAGE_ASPECT_COLOR_BIT, 0, LEVELS, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    for (uint32_t l = 0; l < LEVELS; l++)
    {
        VkBufferImageCopy r = { offsets[l], 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1 }, { 0, 0, 0 }, { W >> l, W >> l, 1 } };
        vkCmdCopyBufferToImage(cb, staging.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    }
    ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; ib.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    vkctx_submit(c, cb);

    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 16;
    VkSampler sampler;
    CHECK(vkCreateSampler(c->device, &sci, NULL, &sampler));

    VkDescriptorSetLayoutBinding b[2] = {
        { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
    };
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 2, b };
    VkDescriptorSetLayout dsl;
    CHECK(vkCreateDescriptorSetLayout(c->device, &dli, NULL, &dsl));
    VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 4 };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl, 1, &pcr };
    VkPipelineLayout pl;
    CHECK(vkCreatePipelineLayout(c->device, &pli, NULL, &pl));
    VkPipeline pipe = compute_pipeline(c, pl, spv);

    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 } };
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 2, ps };
    VkDescriptorPool dp;
    CHECK(vkCreateDescriptorPool(c->device, &dpi, NULL, &dp));
    VkDescriptorSetAllocateInfo dsa = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dp, 1, &dsl };
    VkDescriptorSet set;
    CHECK(vkAllocateDescriptorSets(c->device, &dsa, &set));
    struct vkbuf out = vkctx_buffer(c, 64 * 2 * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    VkDescriptorImageInfo ii = { sampler, img.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorBufferInfo oi = { out.buffer, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w[2] = {
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii, NULL, NULL },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &oi, NULL },
    };
    vkUpdateDescriptorSets(c->device, 2, w, 0, NULL);

    for (int k = 0; k < LEVELS; k++)
    {
        float kf = (float)k;
        cb = vkctx_begin(c);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &set, 0, NULL);
        vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &kf);
        vkCmdDispatch(cb, 1, 1, 1);
        vkctx_submit(c, cb);

        const float *v = out.map;
        for (uint32_t i = 0; i < 64; i++)
        {
            float x = (float)(i & 1), y = (float)((i >> 1) & 1), quad = (float)(i >> 2);
            float want[8] = { 3 + 7 * quad, 5, 2, 0, y ? 13 : 2, x ? 11 : 0, (3 + 7 * quad) + 5, kf };
            for (int n = 0; n < 8; n++)
            {
                char what[64];
                snprintf(what, sizeof(what), "lod %d lane %u value %d: %.3f", k, i, n, v[i * 8 + n]);
                EXPECT_EQ(what, v[i * 8 + n] == want[n], 1);
            }
        }
    }
}

static void test_atomic64(struct vkctx *c, const char *spv)
{
    enum { N = 4096 };
    VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &b };
    VkDescriptorSetLayout dsl;
    CHECK(vkCreateDescriptorSetLayout(c->device, &dli, NULL, &dsl));
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl, 0, NULL };
    VkPipelineLayout pl;
    CHECK(vkCreatePipelineLayout(c->device, &pli, NULL, &pl));
    VkPipeline pipe = compute_pipeline(c, pl, spv);

    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 1, &ps };
    VkDescriptorPool dp;
    CHECK(vkCreateDescriptorPool(c->device, &dpi, NULL, &dp));
    VkDescriptorSetAllocateInfo dsa = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dp, 1, &dsl };
    VkDescriptorSet set;
    CHECK(vkAllocateDescriptorSets(c->device, &dsa, &set));
    struct vkbuf buf = vkctx_buffer(c, (6 + 2 * N) * 8, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    uint64_t *v = buf.map;
    memset(v, 0, buf.size);
    v[1] = 0x8000000000000000ull;   /* smax starts at INT64_MIN */
    v[2] = ~0ull;                   /* umin */
    v[5] = 0x1234567890ull;         /* exch */
    VkDescriptorBufferInfo bi = { buf.buffer, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi, NULL };
    vkUpdateDescriptorSets(c->device, 1, &w, 0, NULL);

    VkCommandBuffer cb = vkctx_begin(c);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &set, 0, NULL);
    vkCmdDispatch(cb, N / 64, 1, 1);
    vkctx_submit(c, cb);

    EXPECT_EQ("add with carries", v[0], (uint64_t)N * 0x100000001ull);
    EXPECT_EQ("smax", v[1], 2047);
    EXPECT_EQ("umin", v[2], 0xFFFFFFFF00000000ull);
    EXPECT_EQ("xor", v[3], ~0ull);

    /* Compare-exchange: exactly one thread saw 0 and installed its index + 1; the rest saw that. */
    uint64_t *ret = v + 6;
    uint32_t winners = 0, winner = 0;
    for (uint32_t i = 0; i < N; i++)
        if (ret[i * 2] == 0) { winners++; winner = i; }
    EXPECT_EQ("cas winners", winners, 1);
    EXPECT_EQ("cas final", v[4], (uint64_t)winner + 1);
    uint32_t bad = 0;
    for (uint32_t i = 0; i < N; i++)
        if (i != winner && ret[i * 2] != v[4]) bad++;
    EXPECT_EQ("cas losers saw the winner", bad, 0);

    /* Exchange: returned values plus the final value are the initial value plus every thread's value, once each. */
    static unsigned char seen[N + 1];
    bad = 0;
    for (uint32_t i = 0; i <= N; i++)
    {
        uint64_t r = i < N ? ret[i * 2 + 1] : v[5];
        uint32_t slot = r == 0x1234567890ull ? N : (r >> 40) == 1 && (r & 0xFFFFFFFFFFull) < N ? (uint32_t)(r & 0xFFFFFFFFFFull) : N + 1;
        if (slot > N || seen[slot]++)
            bad++;
    }
    EXPECT_EQ("exchange chain", bad, 0);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    char path[1024];
    struct vkctx c;
    VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR deriv = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR };
    VkPhysicalDeviceVulkan12Features v12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &deriv };
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v12 };
    deriv.computeDerivativeGroupLinear = VK_TRUE;
    v12.shaderBufferInt64Atomics = VK_TRUE;
    f2.features.shaderInt64 = VK_TRUE;
    static const char *const de[] = { "VK_KHR_portability_subset", "VK_KHR_compute_shader_derivatives" };
    vkctx_init(&c, &f2, de, 2);

    snprintf(path, sizeof(path), "%s/sm66_deriv_comp.spv", dir);
    test_derivatives(&c, path);
    snprintf(path, sizeof(path), "%s/sm66_atomic64_comp.spv", dir);
    test_atomic64(&c, path);
    printf("%s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
