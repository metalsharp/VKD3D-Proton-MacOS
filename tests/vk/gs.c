/* GPU test: geometry shaders (run as Metal object and mesh stages): point to quad expansion, instancing,
 * triangle strip output winding (with front faces culled), the primitive ID and input vertex order for
 * triangle lists, strips and fans, indexed draws, line lists and strips with point output, and lines and
 * triangles with adjacency (lists and a triangle strip), and indirect (and indirect count) draws. */
#include "vkcommon.h"

#define W 16
#define H 16

struct gsctx { struct vkctx c; VkPipelineLayout layout; VkShaderModule vs, fs; struct vkimg rt; const char *dir; };

static VkPipeline make_pipeline(struct gsctx *g, const char *geom, VkPrimitiveTopology topology, VkCullModeFlags cull)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s_geom.spv", g->dir, geom);
    VkShaderModule gs = vkctx_shader(&g->c, path);
    VkPipelineShaderStageCreateInfo stages[3] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, g->vs, "main", NULL },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_GEOMETRY_BIT, gs, "main", NULL },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, g->fs, "main", NULL },
    };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, topology };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = cull; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT };
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState ba = { 0 };
    ba.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo blend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, 0, 1, &ba };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, dyn };
    VkFormat color = VK_FORMAT_R8G8B8A8_UINT;
    VkPipelineRenderingCreateInfo ri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, 1, &color };
    VkGraphicsPipelineCreateInfo gpi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri, 0, 3, stages, &vi, &ia, NULL, &vp, &rs, &ms, &ds,
                                         &blend, &dy, g->layout };
    VkPipeline p;
    CHECK(vkCreateGraphicsPipelines(g->c.device, VK_NULL_HANDLE, 1, &gpi, NULL, &p));
    return p;
}

/* Draws and returns the render target's texels (RGBA8 UINT). */
enum { DIRECT, INDIRECT, INDIRECT_COUNT };

/* Draws the indirect commands in args (indexed if indices), and returns the render target's texels. */
static const uint8_t *draw_args(struct gsctx *g, VkPipeline p, uint32_t table, int mode, const void *args, uint32_t count,
                                uint32_t drawn, const uint32_t *indices, uint32_t index_count);

static const uint8_t *draw_indexed(struct gsctx *g, VkPipeline p, uint32_t table, uint32_t instanced, uint32_t vertices,
                                   const uint32_t *indices)
{
    struct vkbuf ib = { 0 };
    if (indices)
    {
        ib = vkctx_buffer(&g->c, vertices * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        memcpy(ib.map, indices, vertices * 4);
    }
    VkCommandBuffer cb = vkctx_begin(&g->c);
    vkctx_barrier(cb, g->rt.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo att = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, g->rt.view, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rinfo = { VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, { { 0, 0 }, { W, H } }, 1, 0, 1, &att };
    vkCmdBeginRendering(cb, &rinfo);
    VkViewport v = { 0, 0, W, H, 0, 1 };
    VkRect2D s = { { 0, 0 }, { W, H } };
    vkCmdSetViewport(cb, 0, 1, &v);
    vkCmdSetScissor(cb, 0, 1, &s);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    uint32_t pc[2] = { table, instanced };
    vkCmdPushConstants(cb, g->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT, 0, sizeof(pc), pc);
    if (indices)
    {
        vkCmdBindIndexBuffer(cb, ib.buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cb, vertices, 1, 0, 0, 0);
    }
    else
        vkCmdDraw(cb, vertices, 1, 0, 0);
    vkCmdEndRendering(cb);
    vkctx_barrier(cb, g->rt.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    struct vkbuf rb = vkctx_readback(&g->c, cb, &g->rt, 4);
    vkctx_submit(&g->c, cb);
    return rb.map;
}

static const uint8_t *draw(struct gsctx *g, VkPipeline p, uint32_t table, uint32_t instanced, uint32_t vertices)
{
    return draw_indexed(g, p, table, instanced, vertices, NULL);
}

static const uint8_t *draw_args(struct gsctx *g, VkPipeline p, uint32_t table, int mode, const void *args, uint32_t count,
                                uint32_t drawn, const uint32_t *indices, uint32_t index_count)
{
    PFN_vkCmdDrawIndirectCount draw_count = (void *)vkGetDeviceProcAddr(g->c.device, "vkCmdDrawIndirectCount");
    PFN_vkCmdDrawIndexedIndirectCount draw_indexed_count = (void *)vkGetDeviceProcAddr(g->c.device, "vkCmdDrawIndexedIndirectCount");
    uint32_t stride = indices ? sizeof(VkDrawIndexedIndirectCommand) : sizeof(VkDrawIndirectCommand);
    struct vkbuf ab = vkctx_buffer(&g->c, 256, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
    memcpy(ab.map, args, count * stride);
    ((uint32_t *)ab.map)[60] = drawn;
    struct vkbuf ib = { 0 };
    if (indices)
    {
        ib = vkctx_buffer(&g->c, index_count * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        memcpy(ib.map, indices, index_count * 4);
    }
    VkCommandBuffer cb = vkctx_begin(&g->c);
    vkctx_barrier(cb, g->rt.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo att = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, g->rt.view, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rinfo = { VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, { { 0, 0 }, { W, H } }, 1, 0, 1, &att };
    vkCmdBeginRendering(cb, &rinfo);
    VkViewport v = { 0, 0, W, H, 0, 1 };
    VkRect2D s = { { 0, 0 }, { W, H } };
    vkCmdSetViewport(cb, 0, 1, &v);
    vkCmdSetScissor(cb, 0, 1, &s);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    uint32_t pc[2] = { table, 0 };
    vkCmdPushConstants(cb, g->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT, 0, sizeof(pc), pc);
    if (indices)
    {
        vkCmdBindIndexBuffer(cb, ib.buffer, 0, VK_INDEX_TYPE_UINT32);
        if (mode == INDIRECT_COUNT)
            draw_indexed_count(cb, ab.buffer, 0, ab.buffer, 240, count, stride);
        else
            vkCmdDrawIndexedIndirect(cb, ab.buffer, 0, count, stride);
    }
    else if (mode == INDIRECT_COUNT)
        draw_count(cb, ab.buffer, 0, ab.buffer, 240, count, stride);
    else
        vkCmdDrawIndirect(cb, ab.buffer, 0, count, stride);
    vkCmdEndRendering(cb);
    vkctx_barrier(cb, g->rt.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    struct vkbuf rb = vkctx_readback(&g->c, cb, &g->rt, 4);
    vkctx_submit(&g->c, cb);
    return rb.map;
}

static void expect_px(const char *what, const uint8_t *px, int x, int y, uint8_t r, uint8_t gg, uint8_t b, uint8_t a)
{
    const uint8_t *t = px + (y * W + x) * 4;
    char name[160];
    snprintf(name, sizeof(name), "%s at (%d, %d): (%u %u %u %u)", what, x, y, t[0], t[1], t[2], t[3]);
    EXPECT_EQ(name, t[0] == r && t[1] == gg && t[2] == b && t[3] == a, 1);
}

int main(int argc, char **argv)
{
    struct gsctx g = { 0 };
    g.dir = argc > 1 ? argv[1] : ".";
    char path[1024];
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    f2.features.geometryShader = VK_TRUE;
    static const char *const de[] = { "VK_KHR_portability_subset" };
    vkctx_init(&g.c, &f2, de, 1);
    VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT, 0, 8 };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 0, NULL, 1, &pcr };
    CHECK(vkCreatePipelineLayout(g.c.device, &pli, NULL, &g.layout));
    snprintf(path, sizeof(path), "%s/gs_vert.spv", g.dir);
    g.vs = vkctx_shader(&g.c, path);
    snprintf(path, sizeof(path), "%s/gs_frag.spv", g.dir);
    g.fs = vkctx_shader(&g.c, path);
    g.rt = vkctx_image(&g.c, VK_FORMAT_R8G8B8A8_UINT, W, H, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);

    /* Points at (4, 4), (12, 4), (4, 12), (12, 12) to 4x4 quads; the second invocation 4 pixels right. */
    VkPipeline quads = make_pipeline(&g, "gs_quads", VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_CULL_MODE_NONE);
    const uint8_t *px = draw(&g, quads, 0, 0, 4);
    expect_px("quads", px, 4, 4, 1, 1, 0, 1);
    expect_px("quads", px, 12, 4, 2, 1, 0, 1);
    expect_px("quads", px, 4, 12, 3, 1, 0, 1);
    expect_px("quads", px, 12, 12, 4, 1, 0, 1);
    expect_px("quads", px, 0, 0, 0, 0, 0, 0);
    expect_px("quads", px, 8, 4, 0, 0, 0, 0);
    px = draw(&g, quads, 0, 1, 4);
    expect_px("instanced quads", px, 4, 4, 1, 1, 0, 1);
    expect_px("instanced quads", px, 8, 4, 1, 2, 0, 1);
    expect_px("instanced quads", px, 8, 12, 3, 2, 0, 1);

    /* A fullscreen quad as a triangle strip, then with front faces culled: both triangles are clockwise
     * (back faces), the second only if the strip keeps the winding. */
    VkPipeline quad_nocull = make_pipeline(&g, "gs_quad", VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_CULL_MODE_NONE);
    px = draw(&g, quad_nocull, 1, 0, 3);
    expect_px("strip quad, first triangle", px, 2, 2, 1, 1, 1, 1);
    expect_px("strip quad, second triangle", px, 13, 13, 1, 1, 1, 1);
    VkPipeline quad = make_pipeline(&g, "gs_quad", VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_CULL_MODE_FRONT_BIT);
    px = draw(&g, quad, 1, 0, 3);
    expect_px("strip winding, first triangle", px, 2, 2, 1, 1, 1, 1);
    expect_px("strip winding, second triangle", px, 13, 13, 1, 1, 1, 1);

    /* Two triangles (left and right halves): primitive IDs 0 and 1, input vertexes in order. */
    VkPipeline tri = make_pipeline(&g, "gs_tri", VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_CULL_MODE_NONE);
    px = draw(&g, tri, 1, 0, 6);
    expect_px("triangle list", px, 1, 4, 1, 1, 2, 3);
    expect_px("triangle list", px, 9, 2, 2, 4, 5, 6);

    /* A 4-vertex strip: the second triangle's input vertexes are 1, 3, 2. */
    VkPipeline tristrip = make_pipeline(&g, "gs_tri", VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, VK_CULL_MODE_NONE);
    px = draw(&g, tristrip, 2, 0, 4);
    expect_px("triangle strip", px, 2, 2, 1, 1, 2, 3);
    expect_px("triangle strip", px, 13, 13, 2, 2, 4, 3);

    /* Two lines between the points of table 0, to their end points (2x2 points). */
    VkPipeline lines = make_pipeline(&g, "gs_lines", VK_PRIMITIVE_TOPOLOGY_LINE_LIST, VK_CULL_MODE_NONE);
    px = draw(&g, lines, 0, 0, 4);
    expect_px("line ends", px, 4, 4, 1, 0, 0, 1);
    expect_px("line ends", px, 12, 4, 2, 0, 0, 1);
    expect_px("line ends", px, 4, 12, 3, 0, 0, 1);
    expect_px("line ends", px, 12, 12, 4, 0, 0, 1);
    expect_px("line ends", px, 8, 8, 0, 0, 0, 0);

    /* Indexed: the right half's triangle (3 4 5), then the left half's. */
    static const uint32_t indices[6] = { 3, 4, 5, 0, 1, 2 };
    px = draw_indexed(&g, tri, 1, 0, 6, indices);
    expect_px("indexed triangle list", px, 9, 2, 1, 4, 5, 6);
    expect_px("indexed triangle list", px, 1, 4, 2, 1, 2, 3);

    /* A fan of the strip's 4 vertexes: triangles (1 2 0) and (2 3 0). */
    VkPipeline fan = make_pipeline(&g, "gs_tri", VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN, VK_CULL_MODE_NONE);
    px = draw(&g, fan, 2, 0, 4);
    expect_px("triangle fan", px, 2, 2, 1, 2, 3, 1);
    expect_px("triangle fan", px, 3, 14, 2, 3, 4, 1);

    /* A line strip of 3 points: lines (0 1) and (1 2). */
    VkPipeline linestrip = make_pipeline(&g, "gs_lines", VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, VK_CULL_MODE_NONE);
    px = draw(&g, linestrip, 0, 0, 3);
    expect_px("line strip ends", px, 4, 4, 1, 0, 0, 1);
    expect_px("line strip ends", px, 12, 4, 2, 0, 0, 1);
    expect_px("line strip ends", px, 4, 12, 3, 0, 0, 1);
    expect_px("line strip ends", px, 12, 12, 0, 0, 0, 0);

    /* A line with adjacency (4 points): its points are input vertexes 1 and 2. */
    VkPipeline ladj = make_pipeline(&g, "gs_ladj", VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY, VK_CULL_MODE_NONE);
    px = draw(&g, ladj, 0, 0, 4);
    expect_px("line with adjacency", px, 12, 4, 1, 2, 3, 4);
    expect_px("line with adjacency", px, 4, 12, 1, 2, 3, 4);
    expect_px("line with adjacency", px, 4, 4, 0, 0, 0, 0);

    /* A triangle with adjacency (6 vertexes), and a strip with adjacency of 8: triangles
     * { 0 1 2 6 4 3 } and { 4 0 2 5 6 7 } (colored by input vertexes 0, 1, 3 and 5). */
    VkPipeline tadj = make_pipeline(&g, "gs_tadj", VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY, VK_CULL_MODE_NONE);
    px = draw(&g, tadj, 3, 0, 6);
    expect_px("triangle with adjacency", px, 2, 2, 1, 2, 4, 6);
    VkPipeline tsadj = make_pipeline(&g, "gs_tadj", VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY, VK_CULL_MODE_NONE);
    px = draw(&g, tsadj, 3, 0, 8);
    expect_px("triangle strip with adjacency", px, 2, 2, 1, 2, 7, 4);
    expect_px("triangle strip with adjacency", px, 13, 13, 5, 1, 6, 8);

    /* Indirect: the right half's triangle (vertexes 3 to 5), then the left half's; primitive IDs restart per draw. */
    VkDrawIndirectCommand dc[2] = { { 3, 1, 3, 0 }, { 3, 1, 0, 0 } };
    px = draw_args(&g, tri, 1, INDIRECT, dc, 2, 2, NULL, 0);
    expect_px("indirect", px, 9, 2, 1, 4, 5, 6);
    expect_px("indirect", px, 1, 4, 1, 1, 2, 3);
    px = draw_args(&g, tri, 1, INDIRECT_COUNT, dc, 2, 1, NULL, 0);
    expect_px("indirect count (1 of 2)", px, 9, 2, 1, 4, 5, 6);
    expect_px("indirect count (1 of 2)", px, 1, 4, 0, 0, 0, 0);
    /* Indexed indirect: indexes 3 to 5 of { 3 4 5 0 1 2 } (the left half), then 0 to 2 (the right half). */
    VkDrawIndexedIndirectCommand dic[2] = { { 3, 1, 3, 0, 0 }, { 3, 1, 0, 0, 0 } };
    px = draw_args(&g, tri, 1, INDIRECT, dic, 2, 2, indices, 6);
    expect_px("indexed indirect", px, 1, 4, 1, 1, 2, 3);
    expect_px("indexed indirect", px, 9, 2, 1, 4, 5, 6);
    px = draw_args(&g, tri, 1, INDIRECT_COUNT, dic, 2, 1, indices, 6);
    expect_px("indexed indirect count (1 of 2)", px, 1, 4, 1, 1, 2, 3);
    expect_px("indexed indirect count (1 of 2)", px, 9, 2, 0, 0, 0, 0);

    printf("%s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
