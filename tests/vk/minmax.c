/* GPU test: VK_SAMPLER_REDUCTION_MODE_MIN/MAX (D3D12 MINIMUM/MAXIMUM filters) against a CPU reference. */
#include "vkcommon.h"
#include <math.h>

#define W 8
static float texels[3][W * W];

static float fetch(int lvl, int x, int y)
{
    int w = W >> lvl;
    x = x < 0 ? 0 : x >= w ? w - 1 : x;   /* clamp to edge */
    y = y < 0 ? 0 : y >= w ? w - 1 : y;
    return texels[lvl][y * w + x];
}

static float reduce_level(int lvl, float u, float v, int mode)
{
    int w = W >> lvl;
    float px = u * w - 0.5f, py = v * w - 0.5f;
    int ix = (int)floorf(px), iy = (int)floorf(py);
    float fx = px - ix, fy = py - iy;
    float r = fetch(lvl, ix, iy);
#define RED(a, b) (mode == 1 ? fminf(a, b) : fmaxf(a, b))
    if (fx > 0) r = RED(r, fetch(lvl, ix + 1, iy));
    if (fy > 0) r = RED(r, fetch(lvl, ix, iy + 1));
    if (fx > 0 && fy > 0) r = RED(r, fetch(lvl, ix + 1, iy + 1));
    return r;
}

static float reference(float u, float v, float lod, int mode)
{
    float l = fminf(fmaxf(lod, 0.0f), 2.0f);
    int l0 = (int)floorf(l);
    float r = reduce_level(l0, u, v, mode);
    if (l > l0 && l0 + 1 <= 2) r = RED(r, reduce_level(l0 + 1, u, v, mode));
    return r;
}

int main(int argc, char **argv)
{
    const char *spv = argc > 1 ? argv[1] : "minmax_comp.spv";
    struct vkctx c;
    VkPhysicalDeviceVulkan12Features v12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    v12.samplerFilterMinmax = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v12 };
    static const char *const de[] = { "VK_KHR_portability_subset" };

    VkPhysicalDeviceVulkan12Features q12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    VkPhysicalDeviceFeatures2 qf = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &q12 };
    VkPhysicalDeviceVulkan12Properties p12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES };
    VkPhysicalDeviceProperties2 qp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &p12 };
    {
        VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "q", 1, "q", 1, VK_API_VERSION_1_3 };
        VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL };
        VkInstance inst; VkPhysicalDevice pd; uint32_t n = 1;
        CHECK(vkCreateInstance(&ici, NULL, &inst));
        CHECK(vkEnumeratePhysicalDevices(inst, &n, &pd));
        vkGetPhysicalDeviceFeatures2(pd, &qf);
        vkGetPhysicalDeviceProperties2(pd, &qp);
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(pd, VK_FORMAT_R32_SFLOAT, &fp);
        printf("samplerFilterMinmax %u singleComponentFormats %u imageComponentMapping %u R32F minmax %u\n", q12.samplerFilterMinmax,
               p12.filterMinmaxSingleComponentFormats, p12.filterMinmaxImageComponentMapping,
               !!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_MINMAX_BIT));
        vkDestroyInstance(inst, NULL);
    }
    if (!q12.samplerFilterMinmax)
        return 1;
    vkctx_init(&c, &f2, de, 1);

    srand(7);
    for (int l = 0; l < 3; l++)
        for (int i = 0; i < (W >> l) * (W >> l); i++)
            texels[l][i] = (float)(rand() % 1000) / 1000.0f;

    struct vkimg img = vkctx_image(&c, VK_FORMAT_R32_SFLOAT, W, W, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    /* vkctx_image makes one mip; recreate with three. */
    vkDestroyImageView(c.device, img.view, NULL);
    vkDestroyImage(c.device, img.image, NULL);
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R32_SFLOAT, { W, W, 1 }, 3, 1,
                              VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                              VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED };
    CHECK(vkCreateImage(c.device, &ici, NULL, &img.image));
    CHECK(vkBindImageMemory(c.device, img.image, img.memory, 0));
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img.image, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R32_SFLOAT,
                                  { 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 3, 0, 1 } };
    CHECK(vkCreateImageView(c.device, &vci, NULL, &img.view));

    struct vkbuf staging = vkctx_buffer(&c, sizeof(texels), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    memcpy(staging.map, texels, sizeof(texels));
    VkCommandBuffer cb = vkctx_begin(&c);
    VkImageMemoryBarrier ib = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img.image,
                                { VK_IMAGE_ASPECT_COLOR_BIT, 0, 3, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    for (uint32_t l = 0; l < 3; l++)
    {
        VkBufferImageCopy r = { (VkDeviceSize)l * W * W * 4, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1 }, { 0, 0, 0 }, { W >> l, W >> l, 1 } };
        vkCmdCopyBufferToImage(cb, staging.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    }
    ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; ib.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    vkctx_submit(&c, cb);

    VkSampler samplers[3];
    for (int s = 0; s < 3; s++)
    {
        VkSamplerReductionModeCreateInfo rm = { VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO, NULL,
                                                s == 0 ? VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE : s == 1 ? VK_SAMPLER_REDUCTION_MODE_MIN : VK_SAMPLER_REDUCTION_MODE_MAX };
        VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, &rm };
        sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.maxLod = 16;
        CHECK(vkCreateSampler(c.device, &sci, NULL, &samplers[s]));
    }

    VkDescriptorSetLayoutBinding b[4] = {
        { 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
        { 1, VK_DESCRIPTOR_TYPE_SAMPLER, 3, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
        { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
        { 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
    };
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 4, b };
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

    VkDescriptorPoolSize ps[3] = { { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1 }, { VK_DESCRIPTOR_TYPE_SAMPLER, 3 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 } };
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 3, ps };
    VkDescriptorPool dp;
    CHECK(vkCreateDescriptorPool(c.device, &dpi, NULL, &dp));
    VkDescriptorSetAllocateInfo dsa = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dp, 1, &dsl };
    VkDescriptorSet set;
    CHECK(vkAllocateDescriptorSets(c.device, &dsa, &set));

    enum { N = 24 };
    struct vkbuf out = vkctx_buffer(&c, N * 3 * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    struct vkbuf in = vkctx_buffer(&c, N * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    float *q = in.map;
    for (int i = 0; i < N; i++)
    {
        q[i * 4 + 0] = (float)(rand() % 1000) / 1000.0f;
        q[i * 4 + 1] = (float)(rand() % 1000) / 1000.0f;
        q[i * 4 + 2] = (i % 4) * 0.5f;   /* lod 0, 0.5, 1, 1.5 */
        q[i * 4 + 3] = 0;
    }
    VkDescriptorImageInfo ii = { VK_NULL_HANDLE, img.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo si[3] = { { samplers[0] }, { samplers[1] }, { samplers[2] } };
    VkDescriptorBufferInfo oi = { out.buffer, 0, VK_WHOLE_SIZE }, qi = { in.buffer, 0, VK_WHOLE_SIZE };
    /* Write the samplers one at a time, like vkd3d-proton fills its heap. */
    VkWriteDescriptorSet w[6] = {
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &ii, NULL, NULL },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 1, 2, 1, VK_DESCRIPTOR_TYPE_SAMPLER, &si[2], NULL, NULL },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER, &si[0], NULL, NULL },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 1, 1, 1, VK_DESCRIPTOR_TYPE_SAMPLER, &si[1], NULL, NULL },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &oi, NULL },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 3, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &qi, NULL },
    };
    vkUpdateDescriptorSets(c.device, 6, w, 0, NULL);

    cb = vkctx_begin(&c);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &set, 0, NULL);
    uint32_t count = N;
    vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &count);
    vkCmdDispatch(cb, 1, 1, 1);
    vkctx_submit(&c, cb);

    float *v = out.map;
    for (int i = 0; i < N; i++)
    {
        float u = q[i * 4], vv = q[i * 4 + 1], lod = q[i * 4 + 2];
        float mn = reference(u, vv, lod, 1), mx = reference(u, vv, lod, 2);
        char what[96];
        snprintf(what, sizeof(what), "min uv (%.3f,%.3f) lod %.1f: %.4f vs %.4f", u, vv, lod, v[i * 3 + 1], mn);
        EXPECT_EQ(what, fabsf(v[i * 3 + 1] - mn) < 1e-4f, 1);
        snprintf(what, sizeof(what), "max uv (%.3f,%.3f) lod %.1f: %.4f vs %.4f", u, vv, lod, v[i * 3 + 2], mx);
        EXPECT_EQ(what, fabsf(v[i * 3 + 2] - mx) < 1e-4f, 1);
        snprintf(what, sizeof(what), "average within min..max at %d", i);
        EXPECT_EQ(what, v[i * 3] >= mn - 1e-4f && v[i * 3] <= mx + 1e-4f, 1);
    }
    printf("%s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
