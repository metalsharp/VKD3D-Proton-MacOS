/* GPU test: VkPipelineColorBlendStateCreateInfo logic ops (D3D12 OutputMergerLogicOp) on UNORM8 and UINT attachments. */
#include "vkcommon.h"

static uint32_t apply(VkLogicOp op, uint32_t s, uint32_t d)
{
    switch (op)
    {
    case VK_LOGIC_OP_CLEAR: return 0;
    case VK_LOGIC_OP_AND: return s & d;
    case VK_LOGIC_OP_AND_REVERSE: return s & ~d;
    case VK_LOGIC_OP_COPY: return s;
    case VK_LOGIC_OP_AND_INVERTED: return ~s & d;
    case VK_LOGIC_OP_NO_OP: return d;
    case VK_LOGIC_OP_XOR: return s ^ d;
    case VK_LOGIC_OP_OR: return s | d;
    case VK_LOGIC_OP_NOR: return ~(s | d);
    case VK_LOGIC_OP_EQUIVALENT: return ~(s ^ d);
    case VK_LOGIC_OP_INVERT: return ~d;
    case VK_LOGIC_OP_OR_REVERSE: return s | ~d;
    case VK_LOGIC_OP_COPY_INVERTED: return ~s;
    case VK_LOGIC_OP_OR_INVERTED: return ~s | d;
    case VK_LOGIC_OP_NAND: return ~(s & d);
    default: return ~0u;
    }
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    char path[1024];
    struct vkctx c;
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    VkPhysicalDeviceVulkan13Features v13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    v13.dynamicRendering = VK_TRUE;
    f2.pNext = &v13;
    f2.features.logicOp = VK_TRUE;
    static const char *const de[] = { "VK_KHR_portability_subset" };
    vkctx_init(&c, &f2, de, 1);
    VkPhysicalDeviceFeatures supported;
    vkGetPhysicalDeviceFeatures(c.physical, &supported);
    printf("logicOp=%u\n", supported.logicOp);
    if (!supported.logicOp)
        return 1;

    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineLayout pl;
    CHECK(vkCreatePipelineLayout(c.device, &pli, NULL, &pl));
    snprintf(path, sizeof(path), "%s/fullscreen_vert.spv", dir);
    VkShaderModule vs = vkctx_shader(&c, path);

    /* texel: bytes per pixel; bits: per channel. */
    struct { VkFormat format; const char *fs; uint32_t texel, bits; uint32_t src[4]; const char *name; } kinds[] = {
        { VK_FORMAT_R8G8B8A8_UNORM, "logicop_unorm_frag.spv", 4, 8, { 0x19, 0x5a, 0xf0, 0x0f }, "unorm8" },
        { VK_FORMAT_R8G8B8A8_UINT, "logicop_uint_frag.spv", 4, 8, { 0x12345678u, 0xf0f0f0f0u, 0x0u, 0xffffffffu }, "uint8" },
        { VK_FORMAT_R16G16B16A16_UINT, "logicop_uint_frag.spv", 8, 16, { 0x12345678u, 0xf0f0f0f0u, 0x0u, 0xffffffffu }, "uint16" },
        { VK_FORMAT_R32G32B32A32_UINT, "logicop_uint_frag.spv", 16, 32, { 0x12345678u, 0xf0f0f0f0u, 0x0u, 0xffffffffu }, "uint32" },
    };
    for (int k = 0; k < 4; k++)
    {
        snprintf(path, sizeof(path), "%s/%s", dir, kinds[k].fs);
        VkShaderModule fs = vkctx_shader(&c, path);
        for (VkLogicOp op = VK_LOGIC_OP_CLEAR; op <= VK_LOGIC_OP_SET; op++)
        {
            VkPipelineColorBlendAttachmentState att = { 0 };
            att.colorWriteMask = 0xf;
            VkPipelineColorBlendStateCreateInfo blend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_TRUE, op, 1, &att };
            VkPipeline pipe = vkctx_graphics_pipeline(&c, pl, vs, fs, kinds[k].format, &blend, NULL);
            struct vkimg img = vkctx_image(&c, kinds[k].format, 16, 16, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
            uint32_t mask = kinds[k].bits == 32 ? ~0u : (1u << kinds[k].bits) - 1;
            uint32_t dst[4] = { 0xff00ffaau, 0x0ff00f3cu, 0x13579b0fu, 0x000000ffu };
            VkClearColorValue clear;
            for (int i = 0; i < 4; i++)
            {
                dst[i] &= mask;
                clear.uint32[i] = dst[i];
                if (k == 0) clear.float32[i] = dst[i] / 255.0f;
            }
            VkCommandBuffer cb = vkctx_begin(&c);
            vkctx_barrier(cb, img.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdClearColorImage(cb, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
            vkctx_barrier(cb, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            vkctx_begin_rendering(cb, &img);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            vkCmdDraw(cb, 3, 1, 0, 0);
            vkCmdEndRendering(cb);
            vkctx_barrier(cb, img.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            struct vkbuf rb = vkctx_readback(&c, cb, &img, kinds[k].texel);
            vkctx_submit(&c, cb);
            char what[96];
            for (int i = 0; i < 4; i++)
            {
                size_t texel = (8 * 16 + 8) * 4 + i;
                uint32_t got = kinds[k].bits == 8 ? ((uint8_t *)rb.map)[texel] : kinds[k].bits == 16 ? ((uint16_t *)rb.map)[texel] : ((uint32_t *)rb.map)[texel];
                /* Integer formats store the low bits of the shader output. */
                uint32_t want = apply(op, kinds[k].src[i] & mask, dst[i]) & mask;
                snprintf(what, sizeof(what), "%s op %d channel %d", kinds[k].name, op, i);
                EXPECT_EQ(what, got, want);
            }
        }
    }
    printf("%s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
