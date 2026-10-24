/* GPU test: VK_EXT_dynamic_rendering_unused_attachments. Pipelines whose attachment formats differ from the
 * render pass's: a pipeline format with a null attachment, a pipeline without the format of a bound attachment
 * (color and depth, also with the depth test enabled), and a pipeline depth test without a depth attachment. */
#include "vkcommon.h"

#define W 4
#define H 4

static VkPipeline make_pipeline(struct vkctx *c, VkPipelineLayout layout, VkShaderModule vs, VkShaderModule fs,
                                uint32_t color_count, const VkFormat *colors, VkFormat depth, int depth_test)
{
    VkPipelineShaderStageCreateInfo stages[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL },
    };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT };
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable = depth_test;
    ds.depthWriteEnable = depth_test;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState ba[2] = { { 0 }, { 0 } };
    ba[0].colorWriteMask = ba[1].colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo blend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, 0, color_count, ba };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, dyn };
    VkPipelineRenderingCreateInfo ri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, color_count, colors, depth };
    VkGraphicsPipelineCreateInfo gpi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri, 0, 2, stages, &vi, &ia, NULL, &vp, &rs, &ms, &ds,
                                         &blend, &dy, layout };
    VkPipeline p;
    CHECK(vkCreateGraphicsPipelines(c->device, VK_NULL_HANDLE, 1, &gpi, NULL, &p));
    return p;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    char path[1024];
    struct vkctx c;
    VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT uf = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_FEATURES_EXT };
    VkPhysicalDeviceFeatures2 query = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &uf };
    vkctx_init(&c, NULL, NULL, 0);
    vkGetPhysicalDeviceFeatures2(c.physical, &query);
    printf("dynamicRenderingUnusedAttachments %u\n", uf.dynamicRenderingUnusedAttachments);
    EXPECT_EQ("dynamicRenderingUnusedAttachments", uf.dynamicRenderingUnusedAttachments, 1);

    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineLayout pl;
    CHECK(vkCreatePipelineLayout(c.device, &pli, NULL, &pl));
    snprintf(path, sizeof(path), "%s/fullscreen_vert.spv", dir);
    VkShaderModule vs = vkctx_shader(&c, path);
    snprintf(path, sizeof(path), "%s/unused_frag.spv", dir);
    VkShaderModule fs = vkctx_shader(&c, path);

    struct vkimg a = vkctx_image(&c, VK_FORMAT_R8G8B8A8_UNORM, W, H, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    struct vkimg b = vkctx_image(&c, VK_FORMAT_R8G8B8A8_UNORM, W, H, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkImageCreateInfo dci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_D32_SFLOAT, { W, H, 1 }, 1, 1,
                              VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                              VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage depth;
    CHECK(vkCreateImage(c.device, &dci, NULL, &depth));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(c.device, depth, &req);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size,
                                 vkctx_memory_type(&c, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    VkDeviceMemory dmem;
    CHECK(vkAllocateMemory(c.device, &mai, NULL, &dmem));
    CHECK(vkBindImageMemory(c.device, depth, dmem, 0));
    VkImageViewCreateInfo dvi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, depth, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_D32_SFLOAT, { 0 },
                                  { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 } };
    VkImageView dview;
    CHECK(vkCreateImageView(c.device, &dvi, NULL, &dview));

    VkFormat rgba = VK_FORMAT_R8G8B8A8_UNORM;
    VkFormat both[2] = { rgba, rgba }, second[2] = { VK_FORMAT_UNDEFINED, rgba };
    /* name, pipeline colors, depth format and test, pass attachments (a, b, depth), wanted a and b red */
    struct { const char *name; uint32_t count; const VkFormat *colors; VkFormat depth; int test; int pass_a, pass_b, pass_depth;
             int a_red, b_red; } cases[] = {
        { "pipeline color 1 with a null attachment", 2, both, VK_FORMAT_UNDEFINED, 0, 1, 0, 0, 1, 0 },
        { "pipeline without color 0's format", 2, second, VK_FORMAT_UNDEFINED, 0, 1, 1, 0, 0, 1 },
        { "pipeline with fewer color attachments", 1, both, VK_FORMAT_UNDEFINED, 0, 1, 1, 0, 1, 0 },
        { "pipeline without the depth format", 1, both, VK_FORMAT_UNDEFINED, 0, 1, 0, 1, 1, 0 },
        { "pipeline depth test without the depth format", 1, both, VK_FORMAT_UNDEFINED, 1, 1, 0, 1, 1, 0 },
        { "pipeline depth test without a depth attachment", 1, both, VK_FORMAT_D32_SFLOAT, 1, 1, 0, 0, 1, 0 },
    };
    for (unsigned k = 0; k < sizeof(cases) / sizeof(cases[0]); k++)
    {
        VkPipeline p = make_pipeline(&c, pl, vs, fs, cases[k].count, cases[k].colors, cases[k].depth, cases[k].test);
        VkCommandBuffer cb = vkctx_begin(&c);
        vkctx_barrier(cb, a.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        vkctx_barrier(cb, b.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkImageMemoryBarrier db = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
                                    VK_QUEUE_FAMILY_IGNORED, depth, { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 } };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, NULL, 0, NULL, 1, &db);
        VkRenderingAttachmentInfo att[2] = {
            { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, cases[k].pass_a ? a.view : VK_NULL_HANDLE, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
            { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, cases[k].pass_b ? b.view : VK_NULL_HANDLE, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL },
        };
        for (int i = 0; i < 2; i++)
        {
            att[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; att[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            att[i].clearValue.color.float32[2] = att[i].clearValue.color.float32[3] = 1.0f;
        }
        VkRenderingAttachmentInfo datt = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, dview, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL };
        datt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; datt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        datt.clearValue.depthStencil.depth = 0.0f;  /* A depth test against it would fail everywhere. */
        VkRenderingInfo rinfo = { VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, { { 0, 0 }, { W, H } }, 1, 0, 2, att,
                                  cases[k].pass_depth ? &datt : NULL };
        vkCmdBeginRendering(cb, &rinfo);
        VkViewport v = { 0, 0, W, H, 0, 1 };
        VkRect2D s = { { 0, 0 }, { W, H } };
        vkCmdSetViewport(cb, 0, 1, &v);
        vkCmdSetScissor(cb, 0, 1, &s);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
        vkCmdDraw(cb, 3, 1, 0, 0);
        vkCmdEndRendering(cb);
        vkctx_barrier(cb, a.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vkctx_barrier(cb, b.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        struct vkbuf ra = vkctx_readback(&c, cb, &a, 4), rb = vkctx_readback(&c, cb, &b, 4);
        vkctx_submit(&c, cb);
        const uint8_t *pa = ra.map, *pb = rb.map;
        int a_red = pa[0] == 255 && pa[2] == 0, b_red = pb[0] == 255 && pb[2] == 0;
        printf("%-50s a %u %u %u %u, b %u %u %u %u\n", cases[k].name, pa[0], pa[1], pa[2], pa[3], pb[0], pb[1], pb[2], pb[3]);
        char name[128];
        if (cases[k].pass_a)
        {
            snprintf(name, sizeof(name), "%s: attachment a", cases[k].name);
            EXPECT_EQ(name, a_red, cases[k].a_red);
        }
        if (cases[k].pass_b)
        {
            snprintf(name, sizeof(name), "%s: attachment b", cases[k].name);
            EXPECT_EQ(name, b_red, cases[k].b_red);
        }
    }
    printf("%s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
