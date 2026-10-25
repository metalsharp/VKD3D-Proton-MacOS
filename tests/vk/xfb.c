/* GPU test: transform feedback. Captures of draws and indexed draws with instancing into two buffers with
 * a gap between their bindings, resuming from counter buffers, overflow of the smaller buffer (a primitive
 * goes to every buffer or none), the primitives-written query, counter buffers given to End that differ from
 * Begin's (also with nothing drawn), draws outside transform feedback, rasterization of the captures, and
 * a triangle strip drawn with a triangle list pipeline through dynamic primitive topology, indexed draws with
 * a first index and vertex offset, indirect draws (two draws each, indexed and not), and a draw with its vertex
 * count from a counter buffer (vkCmdDrawIndirectByteCountEXT). */
#include "vkcommon.h"

#define W 8
#define H 8
#define SENTINEL 0xdeadbeefu

static const float pos[3][2] = { { -1, -1 }, { 3, -1 }, { -1, 3 } };

static VkPipeline make_pipeline(struct vkctx *c, VkPipelineLayout layout, VkShaderModule vs, VkShaderModule fs, int dynamic_topology)
{
    VkPipelineShaderStageCreateInfo stages[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL },
    };
    VkVertexInputBindingDescription vb = { 0, 16, VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va = { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0 };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0, 1, &vb, 1, &va };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT };
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState ba = { 0 };
    ba.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo blend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, 0, 1, &ba };
    VkDynamicState dyn[3] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, dynamic_topology ? 3 : 2, dyn };
    VkFormat color = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo ri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, 1, &color };
    VkGraphicsPipelineCreateInfo gpi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri, 0, 2, stages, &vi, &ia, NULL, &vp, &rs, &ms, &ds,
                                         &blend, &dy, layout };
    VkPipeline p;
    CHECK(vkCreateGraphicsPipelines(c->device, VK_NULL_HANDLE, 1, &gpi, NULL, &p));
    return p;
}

static void begin_rendering(VkCommandBuffer cb, struct vkimg *img)
{
    VkRenderingAttachmentInfo att = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, img->view, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, { { 0, 0 }, { W, H } }, 1, 0, 1, &att };
    vkCmdBeginRendering(cb, &ri);
    VkViewport v = { 0, 0, W, H, 0, 1 };
    VkRect2D s = { { 0, 0 }, { W, H } };
    vkCmdSetViewport(cb, 0, 1, &v);
    vkCmdSetScissor(cb, 0, 1, &s);
}

/* Checks a captured vertex of buffer 0 (the attribute of vertex v, instance i). */
static void expect_o0(const char *what, const float *o, unsigned v, unsigned i)
{
    char name[128];
    snprintf(name, sizeof(name), "%s: o0 of vertex %u instance %u", what, v, i);
    int ok = o[0] == pos[v % 3][0] && o[1] == pos[v % 3][1] && o[2] == (float)v && o[3] == 1.0f + 100.0f * i;
    if (!ok)
        printf("  %s = (%g %g %g %g)\n", name, o[0], o[1], o[2], o[3]);
    EXPECT_EQ(name, ok, 1);
}

static void expect_o1(const char *what, const uint32_t *o, unsigned v, unsigned i)
{
    char name[128];
    snprintf(name, sizeof(name), "%s: o1.x", what);
    EXPECT_EQ(name, o[0], v);
    snprintf(name, sizeof(name), "%s: o1.y", what);
    EXPECT_EQ(name, o[1], i);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    char path[1024];
    struct vkctx c;
    VkPhysicalDeviceTransformFeedbackFeaturesEXT xf = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT };
    xf.transformFeedback = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &xf };
    static const char *const de[] = { "VK_KHR_portability_subset", "VK_EXT_transform_feedback" };
    vkctx_init(&c, &f2, de, 2);

    VkPhysicalDeviceTransformFeedbackPropertiesEXT xp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_PROPERTIES_EXT };
    VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &xp };
    vkGetPhysicalDeviceProperties2(c.physical, &p2);
    printf("maxTransformFeedbackBuffers %u, queries %u, draw %u\n", xp.maxTransformFeedbackBuffers, xp.transformFeedbackQueries,
           xp.transformFeedbackDraw);
    EXPECT_EQ("maxTransformFeedbackBuffers", xp.maxTransformFeedbackBuffers >= 4, 1);
    EXPECT_EQ("transformFeedbackQueries", xp.transformFeedbackQueries, 1);
    EXPECT_EQ("transformFeedbackDraw", xp.transformFeedbackDraw, 1);

    PFN_vkCmdBindTransformFeedbackBuffersEXT bind = (void *)vkGetDeviceProcAddr(c.device, "vkCmdBindTransformFeedbackBuffersEXT");
    PFN_vkCmdBeginTransformFeedbackEXT begin = (void *)vkGetDeviceProcAddr(c.device, "vkCmdBeginTransformFeedbackEXT");
    PFN_vkCmdEndTransformFeedbackEXT end = (void *)vkGetDeviceProcAddr(c.device, "vkCmdEndTransformFeedbackEXT");
    PFN_vkCmdBeginQueryIndexedEXT begin_query = (void *)vkGetDeviceProcAddr(c.device, "vkCmdBeginQueryIndexedEXT");
    PFN_vkCmdEndQueryIndexedEXT end_query = (void *)vkGetDeviceProcAddr(c.device, "vkCmdEndQueryIndexedEXT");
    PFN_vkCmdDrawIndirectByteCountEXT draw_byte_count = (void *)vkGetDeviceProcAddr(c.device, "vkCmdDrawIndirectByteCountEXT");

    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineLayout pl;
    CHECK(vkCreatePipelineLayout(c.device, &pli, NULL, &pl));
    snprintf(path, sizeof(path), "%s/xfb_vert.spv", dir);
    VkShaderModule vs = vkctx_shader(&c, path);
    snprintf(path, sizeof(path), "%s/xfb_frag.spv", dir);
    VkShaderModule fs = vkctx_shader(&c, path);
    VkPipeline pipeline = make_pipeline(&c, pl, vs, fs, 0);
    VkPipeline dynamic_pipeline = make_pipeline(&c, pl, vs, fs, 1);

    struct vkbuf vtx = vkctx_buffer(&c, 12 * 16, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    for (unsigned v = 0; v < 12; v++)
    {
        float *a = (float *)vtx.map + 4 * v;
        a[0] = pos[v % 3][0]; a[1] = pos[v % 3][1]; a[2] = (float)v; a[3] = 1.0f;
    }
    struct vkbuf idx = vkctx_buffer(&c, 6 * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    static const uint32_t indices[6] = { 0, 1, 2, 6, 7, 8 };
    memcpy(idx.map, indices, sizeof(indices));
    VkBufferUsageFlags xfb_usage = VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT | VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT;
    struct vkbuf b0 = vkctx_buffer(&c, 1024, xfb_usage);
    struct vkbuf b2 = vkctx_buffer(&c, 256, xfb_usage);
    struct vkbuf b2b = vkctx_buffer(&c, 256, xfb_usage);
    struct vkbuf cnt = vkctx_buffer(&c, 64, xfb_usage);
    for (unsigned i = 0; i < 256; i++)
        ((uint32_t *)b0.map)[i] = SENTINEL;
    for (unsigned i = 0; i < 64; i++)
        ((uint32_t *)b2.map)[i] = ((uint32_t *)b2b.map)[i] = SENTINEL;
    for (unsigned i = 0; i < 16; i++)
        ((uint32_t *)cnt.map)[i] = SENTINEL;
    ((uint32_t *)cnt.map)[0] = 16;  /* Buffer 0 resumes after one vertex. */
    ((uint32_t *)cnt.map)[2] = 0;   /* Buffer 2 starts at its beginning. */

    VkQueryPoolCreateInfo qpi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TRANSFORM_FEEDBACK_STREAM_EXT, 2 };
    VkQueryPool qp;
    CHECK(vkCreateQueryPool(c.device, &qpi, NULL, &qp));

    struct vkimg color = vkctx_image(&c, VK_FORMAT_R8G8B8A8_UNORM, W, H,
                                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    VkDeviceSize zero = 0;

    /* 1: a draw of 2 instances and an indexed draw of 2 triangles; buffer 2 is bound with room for 3 triangles,
     * so the indexed draw's second triangle is not captured. */
    VkCommandBuffer cb = vkctx_begin(&c);
    vkCmdResetQueryPool(cb, qp, 0, 2);
    vkctx_barrier(cb, color.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    begin_rendering(cb, &color);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindVertexBuffers(cb, 0, 1, &vtx.buffer, &zero);
    vkCmdBindIndexBuffer(cb, idx.buffer, 0, VK_INDEX_TYPE_UINT32);
    VkDeviceSize size0 = VK_WHOLE_SIZE, size2 = 9 * 8;
    bind(cb, 0, 1, &b0.buffer, &zero, &size0);
    bind(cb, 2, 1, &b2.buffer, &zero, &size2);
    VkBuffer counters[3] = { cnt.buffer, VK_NULL_HANDLE, cnt.buffer };
    VkDeviceSize counter_offsets[3] = { 0, 0, 8 };
    begin_query(cb, qp, 0, 0, 0);
    begin(cb, 0, 3, counters, counter_offsets);
    vkCmdDraw(cb, 3, 2, 3, 0);
    vkCmdDrawIndexed(cb, 6, 1, 0, 0, 0);
    end(cb, 0, 3, counters, counter_offsets);
    end_query(cb, qp, 0, 0);
    /* Not captured. */
    vkCmdDraw(cb, 3, 1, 9, 0);
    vkCmdEndRendering(cb);
    vkctx_barrier(cb, color.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    struct vkbuf rb = vkctx_readback(&c, cb, &color, 4);
    vkctx_submit(&c, cb);

    const uint32_t *counter = cnt.map;
    EXPECT_EQ("1: counter 0", counter[0], 16 + 9 * 16);
    EXPECT_EQ("1: counter 2", counter[2], 9 * 8);
    EXPECT_EQ("1: counter between", counter[1], SENTINEL);
    EXPECT_EQ("1: b0 before the resume offset", ((uint32_t *)b0.map)[0], SENTINEL);
    const float *o0 = (const float *)((char *)b0.map + 16);
    const uint32_t *o1 = b2.map;
    for (unsigned i = 0; i < 2; i++)
        for (unsigned v = 0; v < 3; v++, o0 += 4, o1 += 2)
        {
            expect_o0("1: draw", o0, 3 + v, i);
            expect_o1("1: draw", o1, 3 + v, i);
        }
    for (unsigned v = 0; v < 3; v++, o0 += 4, o1 += 2)
    {
        expect_o0("1: indexed draw", o0, v, 0);
        expect_o1("1: indexed draw", o1, v, 0);
    }
    EXPECT_EQ("1: b0 after the captures", ((uint32_t *)o0)[0], SENTINEL);
    EXPECT_EQ("1: b2 after the captures", o1[0], SENTINEL);
    uint64_t q[2] = { 0 };
    CHECK(vkGetQueryPoolResults(c.device, qp, 0, 1, sizeof(q), q, sizeof(q), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
    printf("query: %llu written, %llu needed\n", (unsigned long long)q[0], (unsigned long long)q[1]);
    EXPECT_EQ("1: primitives written", q[0], 3);
    EXPECT_EQ("1: primitives needed", q[1], 4);
    const uint8_t *px = rb.map;
    EXPECT_EQ("1: rasterized", px[(H / 2 * W + W / 2) * 4], 255);

    /* 2: resume buffer 0 from the counter, with a new buffer 2 from its start, and End's counter elsewhere. */
    cb = vkctx_begin(&c);
    vkCmdResetQueryPool(cb, qp, 1, 1);
    begin_rendering(cb, &color);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindVertexBuffers(cb, 0, 1, &vtx.buffer, &zero);
    bind(cb, 0, 1, &b0.buffer, &zero, NULL);
    bind(cb, 2, 1, &b2b.buffer, &zero, NULL);
    VkDeviceSize end_offset = 16;
    begin_query(cb, qp, 1, 0, 0);
    begin(cb, 0, 1, &cnt.buffer, &zero);
    vkCmdDraw(cb, 3, 1, 0, 0);
    end(cb, 0, 1, &cnt.buffer, &end_offset);
    end_query(cb, qp, 1, 0);
    /* 3: nothing drawn: End stores the offset Begin loaded. */
    VkDeviceSize begin_offset = 16, end_offset2 = 24;
    begin(cb, 0, 1, &cnt.buffer, &begin_offset);
    end(cb, 0, 1, &cnt.buffer, &end_offset2);
    vkCmdEndRendering(cb);
    vkctx_submit(&c, cb);
    EXPECT_EQ("2: End's counter", counter[4], 16 + 12 * 16);
    o0 = (const float *)((char *)b0.map + 16 + 9 * 16);
    for (unsigned v = 0; v < 3; v++, o0 += 4)
        expect_o0("2: resumed draw", o0, v, 0);
    EXPECT_EQ("2: b0 after the captures", ((uint32_t *)o0)[0], SENTINEL);
    o1 = b2b.map;
    for (unsigned v = 0; v < 3; v++, o1 += 2)
        expect_o1("2: new buffer 2", o1, v, 0);
    CHECK(vkGetQueryPoolResults(c.device, qp, 1, 1, sizeof(q), q, sizeof(q), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
    EXPECT_EQ("2: primitives written", q[0], 1);
    EXPECT_EQ("2: primitives needed", q[1], 1);
    EXPECT_EQ("3: End's counter with nothing drawn", counter[6], 16 + 12 * 16);

    /* 4: a 4-vertex triangle strip through dynamic topology: triangles (0 1 2) and (1 3 2), each possibly
     * rotated (the provoking vertex need not be preserved). */
    struct vkbuf s0 = vkctx_buffer(&c, 256, xfb_usage), s2 = vkctx_buffer(&c, 256, xfb_usage);
    for (unsigned i = 0; i < 64; i++)
        ((uint32_t *)s0.map)[i] = ((uint32_t *)s2.map)[i] = SENTINEL;
    cb = vkctx_begin(&c);
    begin_rendering(cb, &color);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, dynamic_pipeline);
    vkCmdSetPrimitiveTopology(cb, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
    vkCmdBindVertexBuffers(cb, 0, 1, &vtx.buffer, &zero);
    bind(cb, 0, 1, &s0.buffer, &zero, NULL);
    bind(cb, 2, 1, &s2.buffer, &zero, NULL);
    begin(cb, 0, 0, NULL, NULL);
    vkCmdDraw(cb, 4, 1, 0, 0);
    end(cb, 0, 0, NULL, NULL);
    vkCmdEndRendering(cb);
    vkctx_submit(&c, cb);
    static const unsigned strip[2][3] = { { 0, 1, 2 }, { 1, 3, 2 } };
    for (unsigned t = 0; t < 2; t++)
    {
        const float *o = (const float *)s0.map + 12 * t;
        unsigned got[3] = { (unsigned)o[2], (unsigned)o[6], (unsigned)o[10] };
        int ok = 0;
        for (unsigned r = 0; r < 3; r++)
            ok |= got[r] == strip[t][0] && got[(r + 1) % 3] == strip[t][1] && got[(r + 2) % 3] == strip[t][2];
        printf("strip triangle %u: %u %u %u\n", t, got[0], got[1], got[2]);
        EXPECT_EQ("4: strip triangle", ok, 1);
    }
    EXPECT_EQ("4: b0 after the strip", ((uint32_t *)s0.map)[24], SENTINEL);

    /* 5: an indexed draw with a first index and a vertex offset: indices 6 7 8 from index 3, offset 1. */
    for (unsigned i = 0; i < 64; i++)
        ((uint32_t *)s0.map)[i] = ((uint32_t *)s2.map)[i] = SENTINEL;
    cb = vkctx_begin(&c);
    begin_rendering(cb, &color);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindVertexBuffers(cb, 0, 1, &vtx.buffer, &zero);
    vkCmdBindIndexBuffer(cb, idx.buffer, 0, VK_INDEX_TYPE_UINT32);
    bind(cb, 0, 1, &s0.buffer, &zero, NULL);
    bind(cb, 2, 1, &s2.buffer, &zero, NULL);
    begin(cb, 0, 0, NULL, NULL);
    vkCmdDrawIndexed(cb, 3, 1, 3, 1, 0);
    end(cb, 0, 0, NULL, NULL);
    vkCmdEndRendering(cb);
    vkctx_submit(&c, cb);
    o0 = s0.map;
    o1 = s2.map;
    for (unsigned v = 0; v < 3; v++, o0 += 4, o1 += 2)
    {
        expect_o0("5: indexed draw with offsets", o0, 7 + v, 0);
        expect_o1("5: indexed draw with offsets", o1, 7 + v, 0);
    }

    /* 6: indirect draws: { 3 vertices from 3, 2 instances from 1 } and { 3 from 6 }, then indexed
     * { indexes 3..5 (6 7 8), vertex offset 1 } and { indexes 0..2, instance 2 }. */
    struct vkbuf ind = vkctx_buffer(&c, 256, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
    VkDrawIndirectCommand *dc = ind.map;
    dc[0] = (VkDrawIndirectCommand){ 3, 2, 3, 1 };
    dc[1] = (VkDrawIndirectCommand){ 3, 1, 6, 0 };
    VkDrawIndexedIndirectCommand *dic = (VkDrawIndexedIndirectCommand *)((char *)ind.map + 128);
    dic[0] = (VkDrawIndexedIndirectCommand){ 3, 1, 3, 1, 0 };
    dic[1] = (VkDrawIndexedIndirectCommand){ 3, 1, 0, 0, 2 };
    for (unsigned i = 0; i < 64; i++)
        ((uint32_t *)s0.map)[i] = ((uint32_t *)s2.map)[i] = SENTINEL;
    cb = vkctx_begin(&c);
    vkCmdResetQueryPool(cb, qp, 0, 1);
    begin_rendering(cb, &color);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindVertexBuffers(cb, 0, 1, &vtx.buffer, &zero);
    vkCmdBindIndexBuffer(cb, idx.buffer, 0, VK_INDEX_TYPE_UINT32);
    bind(cb, 0, 1, &s0.buffer, &zero, NULL);
    bind(cb, 2, 1, &s2.buffer, &zero, NULL);
    begin_query(cb, qp, 0, 0, 0);
    begin(cb, 0, 0, NULL, NULL);
    vkCmdDrawIndirect(cb, ind.buffer, 0, 2, sizeof(VkDrawIndirectCommand));
    vkCmdDrawIndexedIndirect(cb, ind.buffer, 128, 2, sizeof(VkDrawIndexedIndirectCommand));
    end(cb, 0, 0, NULL, NULL);
    end_query(cb, qp, 0, 0);
    vkCmdEndRendering(cb);
    vkctx_submit(&c, cb);
    static const unsigned ind_v[5] = { 3, 3, 6, 7, 0 }, ind_i[5] = { 1, 2, 0, 0, 2 };
    o0 = s0.map;
    o1 = s2.map;
    for (unsigned t = 0; t < 5; t++)
        for (unsigned v = 0; v < 3; v++, o0 += 4, o1 += 2)
        {
            expect_o0("6: indirect draw", o0, ind_v[t] + v, ind_i[t]);
            expect_o1("6: indirect draw", o1, ind_v[t] + v, ind_i[t]);
        }
    CHECK(vkGetQueryPoolResults(c.device, qp, 0, 1, sizeof(q), q, sizeof(q), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
    EXPECT_EQ("6: primitives written", q[0], 5);

    /* 7: the counter at offset 0 holds 208 bytes: past a counter offset of 16, 12 vertices of 16 bytes (instance 1). */
    for (unsigned i = 0; i < 64; i++)
        ((uint32_t *)s0.map)[i] = ((uint32_t *)s2.map)[i] = SENTINEL;
    struct vkbuf big0 = vkctx_buffer(&c, 1024, xfb_usage), big2 = vkctx_buffer(&c, 1024, xfb_usage);
    cb = vkctx_begin(&c);
    begin_rendering(cb, &color);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    vkCmdBindVertexBuffers(cb, 0, 1, &vtx.buffer, &zero);
    bind(cb, 0, 1, &big0.buffer, &zero, NULL);
    bind(cb, 2, 1, &big2.buffer, &zero, NULL);
    begin(cb, 0, 0, NULL, NULL);
    draw_byte_count(cb, 1, 1, cnt.buffer, 0, 16, 16);
    end(cb, 0, 0, NULL, NULL);
    vkCmdEndRendering(cb);
    vkctx_submit(&c, cb);
    o0 = big0.map;
    for (unsigned v = 0; v < 12; v++, o0 += 4)
        expect_o0("7: byte count draw", o0, v, 1);

    printf("%s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
