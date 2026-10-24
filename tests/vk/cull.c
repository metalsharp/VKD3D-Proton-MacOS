/* GPU test: cull distances (shaderCullDistance, written as clip distances): a primitive whose vertices all have
 * a negative cull distance is culled, one with only some negative ones is drawn whole, through vertex, geometry
 * and mesh shaders; and clip and cull distances together. */
#include "vkcommon.h"

#define W 12
#define H 4

enum { VERTEX, GEOMETRY, MESH, CLIP_AND_CULL, KIND_COUNT };
static const char *kind_names[KIND_COUNT] = { "vertex", "geometry", "mesh", "clip and cull" };

static VkPipeline make_pipeline(struct vkctx *c, VkPipelineLayout layout, const char *dir, int kind)
{
    static const char *const names[KIND_COUNT][2] = {
        { "cull_vert", NULL }, { "cull_vert", "cull_geom" }, { "cull_mesh", NULL }, { "cullclip_vert", NULL },
    };
    static const VkShaderStageFlagBits bits[KIND_COUNT][2] = {
        { VK_SHADER_STAGE_VERTEX_BIT }, { VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_GEOMETRY_BIT },
        { VK_SHADER_STAGE_MESH_BIT_EXT }, { VK_SHADER_STAGE_VERTEX_BIT },
    };
    VkPipelineShaderStageCreateInfo stages[3];
    uint32_t n = 0;
    char path[1024];
    for (; n < 2 && names[kind][n]; n++)
    {
        snprintf(path, sizeof(path), "%s/%s.spv", dir, names[kind][n]);
        stages[n] = (VkPipelineShaderStageCreateInfo){ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, bits[kind][n],
                                                        vkctx_shader(c, path), "main", NULL };
    }
    snprintf(path, sizeof(path), "%s/cull_frag.spv", dir);
    stages[n++] = (VkPipelineShaderStageCreateInfo){ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
                                                      vkctx_shader(c, path), "main", NULL };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0,
                                                  VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT };
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState ba = { 0 };
    ba.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo blend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, 0, 1, &ba };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, dyn };
    VkFormat color = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo ri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, 1, &color };
    VkGraphicsPipelineCreateInfo gpi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri, 0, n, stages,
                                         kind == MESH ? NULL : &vi, kind == MESH ? NULL : &ia, NULL,
                                         &vp, &rs, &ms, &ds, &blend, &dy, layout };
    VkPipeline p;
    CHECK(vkCreateGraphicsPipelines(c->device, VK_NULL_HANDLE, 1, &gpi, NULL, &p));
    return p;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    VkPhysicalDeviceMeshShaderFeaturesEXT mesh = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT };
    mesh.meshShader = VK_TRUE;
    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &mesh };
    f13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f13 };
    f2.features.geometryShader = f2.features.shaderClipDistance = f2.features.shaderCullDistance = VK_TRUE;
    const char *ext[] = { VK_EXT_MESH_SHADER_EXTENSION_NAME };
    struct vkctx c;
    vkctx_init(&c, &f2, ext, 1);
    struct vkimg rt = vkctx_image(&c, VK_FORMAT_R8G8B8A8_UNORM, W, H, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineLayout layout;
    CHECK(vkCreatePipelineLayout(c.device, &plci, NULL, &layout));
    PFN_vkCmdDrawMeshTasksEXT draw_mesh = (void *)vkGetDeviceProcAddr(c.device, "vkCmdDrawMeshTasksEXT");

    for (int kind = 0; kind < KIND_COUNT; kind++)
    {
        VkPipeline p = make_pipeline(&c, layout, dir, kind);
        VkCommandBuffer cb = vkctx_begin(&c);
        vkctx_barrier(cb, rt.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo att = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, rt.view, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo rinfo = { VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, { { 0, 0 }, { W, H } }, 1, 0, 1, &att };
        vkCmdBeginRendering(cb, &rinfo);
        VkViewport v = { 0, 0, W, H, 0, 1 };
        VkRect2D s = { { 0, 0 }, { W, H } };
        vkCmdSetViewport(cb, 0, 1, &v);
        vkCmdSetScissor(cb, 0, 1, &s);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
        if (kind == MESH)
            draw_mesh(cb, 3, 1, 1);
        else
            vkCmdDraw(cb, 6, kind == CLIP_AND_CULL ? 1 : 3, 0, 0);
        vkCmdEndRendering(cb);
        vkctx_barrier(cb, rt.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        struct vkbuf rb = vkctx_readback(&c, cb, &rt, 4);
        vkctx_submit(&c, cb);
        const uint32_t *px = rb.map;
        char what[128];
        if (kind == CLIP_AND_CULL)
        {
            /* The bottom half (gl_ClipDistance >= 0 at y = -1, the top of the target) is drawn, also at the left. */
            for (uint32_t x = 0; x < W; x += W - 1)
            {
                snprintf(what, sizeof(what), "%s: column %u, top row", kind_names[kind], x);
                EXPECT_EQ(what, px[x], 0xff0000ff);
                snprintf(what, sizeof(what), "%s: column %u, bottom row", kind_names[kind], x);
                EXPECT_EQ(what, px[(H - 1) * W + x], 0);
            }
        }
        else
            for (uint32_t x = 0; x < W; x++)
                for (uint32_t y = 0; y < H; y++)
                {
                    snprintf(what, sizeof(what), "%s: pixel (%u, %u)", kind_names[kind], x, y);
                    EXPECT_EQ(what, px[y * W + x], x < W / 3 ? 0 : 0xff0000ff);
                }
        vkDestroyPipeline(c.device, p, NULL);
    }
    printf("%s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
