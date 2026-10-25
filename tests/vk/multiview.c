/* GPU test: multiview (VK_KHR_multiview) with every kind of pipeline: vertex and fragment shaders, a geometry shader
 * (multiviewGeometryShader), tessellation (multiviewTessellationShader) and task and mesh shaders (multiviewMeshShader).
 * Each pre-rasterization stage packs its ViewIndex into red (but the task shader: glslang has no gl_ViewIndex in task
 * shaders), the fragment shader its own into green; view masks
 * 0b011 and 0b110 (which doesn't start at view 0) render into a 3-layer image. */
#include "vkcommon.h"

#define W 8
#define H 8
#define LAYERS 3

enum { PLAIN, GEOMETRY, TESSELLATION, MESH, KIND_COUNT };
static const char *kind_names[KIND_COUNT] = { "vertex", "geometry", "tessellation", "mesh" };

static VkPipeline make_pipeline(struct vkctx *c, VkPipelineLayout layout, const char *dir, int kind, uint32_t view_mask)
{
    static const char *const names[KIND_COUNT][4] = {
        { "mv_vert", NULL, NULL, NULL },
        { "mv_vert", "mv_geom", NULL, NULL },
        { "mv_vert", "mv_tesc", "mv_tese", NULL },
        { "mv_task", "mv_mesh", NULL, NULL },
    };
    static const VkShaderStageFlagBits bits[KIND_COUNT][3] = {
        { VK_SHADER_STAGE_VERTEX_BIT },
        { VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_GEOMETRY_BIT },
        { VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT },
        { VK_SHADER_STAGE_TASK_BIT_EXT, VK_SHADER_STAGE_MESH_BIT_EXT },
    };
    VkPipelineShaderStageCreateInfo stages[4];
    uint32_t n = 0;
    char path[1024];
    for (; n < 3 && names[kind][n]; n++)
    {
        snprintf(path, sizeof(path), "%s/%s.spv", dir, names[kind][n]);
        stages[n] = (VkPipelineShaderStageCreateInfo){ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, bits[kind][n],
                                                        vkctx_shader(c, path), "main", NULL };
    }
    snprintf(path, sizeof(path), "%s/mv_frag.spv", dir);
    stages[n++] = (VkPipelineShaderStageCreateInfo){ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
                                                      vkctx_shader(c, path), "main", NULL };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0,
                                                  kind == TESSELLATION ? VK_PRIMITIVE_TOPOLOGY_PATCH_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineTessellationStateCreateInfo ts = { VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO, NULL, 0, 3 };
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
    VkFormat color = VK_FORMAT_R8G8B8A8_UINT;
    VkPipelineRenderingCreateInfo ri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, view_mask, 1, &color };
    VkGraphicsPipelineCreateInfo gpi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri, 0, n, stages,
                                         kind == MESH ? NULL : &vi, kind == MESH ? NULL : &ia, kind == TESSELLATION ? &ts : NULL,
                                         &vp, &rs, &ms, &ds, &blend, &dy, layout };
    VkPipeline p;
    CHECK(vkCreateGraphicsPipelines(c->device, VK_NULL_HANDLE, 1, &gpi, NULL, &p));
    return p;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    VkPhysicalDeviceMeshShaderFeaturesEXT mesh = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT };
    mesh.taskShader = mesh.meshShader = mesh.multiviewMeshShader = VK_TRUE;
    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &mesh };
    f13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceVulkan11Features f11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f13 };
    f11.multiview = f11.multiviewGeometryShader = f11.multiviewTessellationShader = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11 };
    f2.features.geometryShader = f2.features.tessellationShader = VK_TRUE;

    struct vkctx c;
    {
        VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "mvktest", 1, "mvktest", 1, VK_API_VERSION_1_3 };
        VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app };
        CHECK(vkCreateInstance(&ici, NULL, &c.instance));
        uint32_t n = 1;
        CHECK(vkEnumeratePhysicalDevices(c.instance, &n, &c.physical));
        VkPhysicalDeviceMeshShaderFeaturesEXT smesh = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT };
        VkPhysicalDeviceVulkan11Features s11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &smesh };
        VkPhysicalDeviceFeatures2 s2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &s11 };
        vkGetPhysicalDeviceFeatures2(c.physical, &s2);
        printf("multiview %u geometry %u tessellation %u mesh %u\n", s11.multiview, s11.multiviewGeometryShader,
               s11.multiviewTessellationShader, smesh.multiviewMeshShader);
        vkDestroyInstance(c.instance, NULL);
    }
    const char *ext[] = { VK_EXT_MESH_SHADER_EXTENSION_NAME };
    vkctx_init(&c, &f2, ext, 1);

    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UINT, { W, H, 1 }, 1, LAYERS,
                              VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
                              VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage image;
    CHECK(vkCreateImage(c.device, &ici, NULL, &image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(c.device, image, &req);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size,
                                 vkctx_memory_type(&c, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    VkDeviceMemory memory;
    CHECK(vkAllocateMemory(c.device, &mai, NULL, &memory));
    CHECK(vkBindImageMemory(c.device, image, memory, 0));
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, image, VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                                  VK_FORMAT_R8G8B8A8_UINT, { 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, LAYERS } };
    VkImageView view;
    CHECK(vkCreateImageView(c.device, &vci, NULL, &view));
    VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineLayout layout;
    CHECK(vkCreatePipelineLayout(c.device, &plci, NULL, &layout));
    PFN_vkCmdDrawMeshTasksEXT draw_mesh = (void *)vkGetDeviceProcAddr(c.device, "vkCmdDrawMeshTasksEXT");

    uint32_t masks[2] = { 0x3, 0x6 };
    if (getenv("MV_MASK"))
        masks[0] = masks[1] = strtoul(getenv("MV_MASK"), NULL, 0);   /* For debugging. */
    for (int m = 0; m < 2; m++)
        for (int kind = 0; kind < KIND_COUNT; kind++)
        {
            if (getenv("MV_KIND") && atoi(getenv("MV_KIND")) != kind)
                continue;   /* Just one kind, for debugging. */
            VkPipeline p = make_pipeline(&c, layout, dir, kind, masks[m]);
            struct vkbuf rb = vkctx_buffer(&c, W * H * 4 * LAYERS, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            VkCommandBuffer cb = vkctx_begin(&c);
            VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_MEMORY_WRITE_BIT,
                                       VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
                                       VK_QUEUE_FAMILY_IGNORED, image, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, LAYERS } };
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
            VkClearColorValue zero = { 0 };
            vkCmdClearColorImage(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &b.subresourceRange);
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
            VkRenderingAttachmentInfo att = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, view, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
            att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingInfo rinfo = { VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, { { 0, 0 }, { W, H } }, 1, masks[m], 1, &att };
            vkCmdBeginRendering(cb, &rinfo);
            VkViewport v = { 0, 0, W, H, 0, 1 };
            VkRect2D s = { { 0, 0 }, { W, H } };
            vkCmdSetViewport(cb, 0, 1, &v);
            vkCmdSetScissor(cb, 0, 1, &s);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
            if (kind == MESH)
                draw_mesh(cb, 1, 1, 1);
            else
                vkCmdDraw(cb, 3, 1, 0, 0);
            vkCmdEndRendering(cb);
            b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
            VkBufferImageCopy r = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, LAYERS }, { 0, 0, 0 }, { W, H, 1 } };
            vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buffer, 1, &r);
            vkctx_submit(&c, cb);
            const uint8_t *px = rb.map;
            if (getenv("MV_MASK") && !masks[m])
                printf("%s: layer 0 %02x %02x %02x %02x\n", kind_names[kind], px[(H / 2 * W + W / 2) * 4], px[(H / 2 * W + W / 2) * 4 + 1],
                       px[(H / 2 * W + W / 2) * 4 + 2], px[(H / 2 * W + W / 2) * 4 + 3]);
            for (uint32_t layer = 0; layer < LAYERS; layer++)
            {
                const uint8_t *t = px + (layer * W * H + (H / 2) * W + W / 2) * 4;
                int drawn = masks[m] & (1u << layer);
                uint32_t stages = kind == TESSELLATION ? 3 : kind == PLAIN ? 1 : 2;
                uint32_t pre = 0;
                for (uint32_t i = kind == MESH ? 1 : 0; i < stages; i++)
                    pre |= layer << (2 * i);
                char what[128];
                snprintf(what, sizeof(what), "mask 0x%x %s layer %u pre-raster views", masks[m], kind_names[kind], layer);
                EXPECT_EQ(what, t[0], drawn ? pre + 1 : 0);
                snprintf(what, sizeof(what), "mask 0x%x %s layer %u fragment view", masks[m], kind_names[kind], layer);
                EXPECT_EQ(what, t[1], drawn ? layer + 1 : 0);
            }
            vkDestroyPipeline(c.device, p, NULL);
        }
    printf("%s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
