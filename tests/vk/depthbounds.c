/* GPU test: the depth bounds test (emulated on GPUs without Metal's), against the stored depth: static and
 * dynamic bounds, and a depth write between tests (the snapshot must follow it). */
#include "vkcommon.h"

#define W 16
#define H 4

static VkPipeline make_pipeline(struct vkctx *c, VkPipelineLayout layout, VkShaderModule vs, VkShaderModule fs, int write_depth,
                                int bounds_test, int dynamic_bounds, float minb, float maxb)
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
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = write_depth;
    ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    ds.depthBoundsTestEnable = bounds_test;
    ds.minDepthBounds = minb;
    ds.maxDepthBounds = maxb;
    VkPipelineColorBlendAttachmentState ba = { 0 };
    ba.colorWriteMask = write_depth ? 0 : 0xf;
    VkPipelineColorBlendStateCreateInfo blend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, 0, 1, &ba };
    VkDynamicState dyn[3] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BOUNDS };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, dynamic_bounds ? 3 : 2, dyn };
    VkFormat color = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo ri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, 1, &color, VK_FORMAT_D32_SFLOAT };
    VkGraphicsPipelineCreateInfo gpi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri, 0, 2, stages, &vi, &ia, NULL, &vp, &rs, &ms, &ds,
                                         &blend, &dy, layout };
    VkPipeline p;
    CHECK(vkCreateGraphicsPipelines(c->device, VK_NULL_HANDLE, 1, &gpi, NULL, &p));
    return p;
}

static void rect(VkCommandBuffer cb, VkPipelineLayout pl, float x0, float x1, float z)
{
    float pc[3] = { x0, x1, z };
    vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    char path[1024];
    struct vkctx c;
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    f2.features.depthBounds = VK_TRUE;
    static const char *const de[] = { "VK_KHR_portability_subset" };
    vkctx_init(&c, &f2, de, 1);

    VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT, 0, 12 };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 0, NULL, 1, &pcr };
    VkPipelineLayout pl;
    CHECK(vkCreatePipelineLayout(c.device, &pli, NULL, &pl));
    snprintf(path, sizeof(path), "%s/depthbounds_vert.spv", dir);
    VkShaderModule vs = vkctx_shader(&c, path);
    snprintf(path, sizeof(path), "%s/depthbounds_frag.spv", dir);
    VkShaderModule fs = vkctx_shader(&c, path);
    VkPipeline writer = make_pipeline(&c, pl, vs, fs, 1, 0, 0, 0, 1);
    VkPipeline bounds_static = make_pipeline(&c, pl, vs, fs, 0, 1, 0, 0.5f, 1.0f);
    VkPipeline bounds_dynamic = make_pipeline(&c, pl, vs, fs, 0, 1, 1, 0.0f, 1.0f);

    struct vkimg color = vkctx_image(&c, VK_FORMAT_R8G8B8A8_UNORM, W, H,
                                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
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

    /* Each case: the depth is 0.1 on the left half and 0.6 on the right, then a fullscreen rectangle at
     * z = 0.9 passes the bounds test where the stored depth is inside the bounds. */
    struct { const char *name; float minb, maxb; int dynamic; int rewrite; const char *want; } cases[] = {
        { "static [0.5, 1]", 0.5f, 1.0f, 0, 0, "........RRRRRRRR" },
        { "dynamic [0, 0.2]", 0.0f, 0.2f, 1, 0, "RRRRRRRR........" },
        { "dynamic [0.25, 0.35] after rewriting the right half to 0.3", 0.25f, 0.35f, 1, 1, "........RRRRRRRR" },
        { "dynamic [0, 1]", 0.0f, 1.0f, 1, 0, "RRRRRRRRRRRRRRRR" },
    };
    for (unsigned k = 0; k < sizeof(cases) / sizeof(cases[0]); k++)
    {
        VkCommandBuffer cb = vkctx_begin(&c);
        vkctx_barrier(cb, color.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkImageMemoryBarrier db = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                                    VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
                                    VK_QUEUE_FAMILY_IGNORED, depth, { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 } };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, NULL, 0, NULL, 1, &db);
        VkRenderingAttachmentInfo catt = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, color.view, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        catt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; catt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingAttachmentInfo datt = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, dview, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL };
        datt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; datt.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        datt.clearValue.depthStencil.depth = 1.0f;
        VkRenderingInfo rinfo = { VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, { { 0, 0 }, { W, H } }, 1, 0, 1, &catt, &datt };
        vkCmdBeginRendering(cb, &rinfo);
        VkViewport v = { 0, 0, W, H, 0, 1 };
        VkRect2D s = { { 0, 0 }, { W, H } };
        vkCmdSetViewport(cb, 0, 1, &v);
        vkCmdSetScissor(cb, 0, 1, &s);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, writer);
        rect(cb, pl, -1.0f, 0.0f, 0.1f);
        rect(cb, pl, 0.0f, 1.0f, 0.6f);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, cases[k].dynamic ? bounds_dynamic : bounds_static);
        if (cases[k].rewrite)
        {
            /* A test that snapshots the current depth, then a depth write, then the test that must see it. */
            vkCmdSetDepthBounds(cb, 0.0f, 1.0f);
            rect(cb, pl, -1.0f, -0.99f, 0.9f);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, writer);
            rect(cb, pl, 0.0f, 1.0f, 0.3f);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, bounds_dynamic);
        }
        if (cases[k].dynamic)
            vkCmdSetDepthBounds(cb, cases[k].minb, cases[k].maxb);
        rect(cb, pl, -1.0f, 1.0f, 0.9f);
        vkCmdEndRendering(cb);
        vkctx_barrier(cb, color.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        struct vkbuf rb = vkctx_readback(&c, cb, &color, 4);
        vkctx_submit(&c, cb);
        char got[W + 1];
        const uint8_t *px = rb.map;
        for (int x = 0; x < W; x++)
            got[x] = px[(1 * W + x) * 4] > 127 ? 'R' : '.';
        got[W] = 0;
        /* The rewrite case's first, 1-pixel-wide test draw lands in column 0; ignore it. */
        if (cases[k].rewrite)
            got[0] = cases[k].want[0];
        printf("%-60s %s (want %s)\n", cases[k].name, got, cases[k].want);
        EXPECT_EQ(cases[k].name, strcmp(got, cases[k].want) == 0, 1);
    }
    printf("%s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
