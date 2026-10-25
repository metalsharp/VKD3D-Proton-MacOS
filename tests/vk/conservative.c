/* GPU test: VK_EXT_conservative_rasterization overestimation against a CPU triangle/pixel-square
 * intersection reference, for direct and indexed draws (both windings), including degenerate
 * triangles, and FullyCoveredEXT (inner coverage) against a pixel-inside-triangle reference. */
#include "vkcommon.h"
#include <math.h>

#define W 32

/* Separating axis test: does triangle t (pixel coordinates) intersect the square [x0,x1]x[y0,y1]? */
static int tri_box(const float t[3][2], float x0, float y0, float x1, float y1)
{
    float minx = fminf(t[0][0], fminf(t[1][0], t[2][0])), maxx = fmaxf(t[0][0], fmaxf(t[1][0], t[2][0]));
    float miny = fminf(t[0][1], fminf(t[1][1], t[2][1])), maxy = fmaxf(t[0][1], fmaxf(t[1][1], t[2][1]));
    if (maxx < x0 || minx > x1 || maxy < y0 || miny > y1)
        return 0;
    for (int e = 0; e < 3; e++)
    {
        const float *a = t[e], *b = t[(e + 1) % 3];
        float nx = b[1] - a[1], ny = a[0] - b[0];
        float tmin = INFINITY, tmax = -INFINITY, bmin = INFINITY, bmax = -INFINITY;
        for (int k = 0; k < 3; k++)
        {
            float d = nx * t[k][0] + ny * t[k][1];
            tmin = fminf(tmin, d); tmax = fmaxf(tmax, d);
        }
        float cx[4] = { x0, x1, x0, x1 }, cy[4] = { y0, y0, y1, y1 };
        for (int k = 0; k < 4; k++)
        {
            float d = nx * cx[k] + ny * cy[k];
            bmin = fminf(bmin, d); bmax = fmaxf(bmax, d);
        }
        if (tmax < bmin || bmax < tmin)
            return 0;
    }
    return 1;
}

/* Is the square [x0,x1]x[y0,y1] inside triangle t (never for degenerate triangles)? */
static int box_in_tri(const float t[3][2], float x0, float y0, float x1, float y1)
{
    float area = (t[1][0] - t[0][0]) * (t[2][1] - t[0][1]) - (t[1][1] - t[0][1]) * (t[2][0] - t[0][0]);
    if (fabsf(area) < 1e-6f)
        return 0;
    float cx[4] = { x0, x1, x0, x1 }, cy[4] = { y0, y0, y1, y1 };
    for (int e = 0; e < 3; e++)
    {
        const float *a = t[e], *b = t[(e + 1) % 3];
        for (int k = 0; k < 4; k++)
            if (((b[0] - a[0]) * (cy[k] - a[1]) - (b[1] - a[1]) * (cx[k] - a[0])) * area < 0)
                return 0;
    }
    return 1;
}

static VkPipeline make_pipeline(struct vkctx *c, VkPipelineLayout layout, VkShaderModule vs, VkShaderModule fs, int conservative,
                                int dynamic_stride)
{
    VkPipelineShaderStageCreateInfo stages[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL },
    };
    VkVertexInputBindingDescription vb = { 0, dynamic_stride ? 0 : 8, VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va = { 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0, 1, &vb, 1, &va };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL };
    VkPipelineRasterizationConservativeStateCreateInfoEXT cs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT };
    cs.conservativeRasterizationMode = VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, conservative ? &cs : NULL };
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT };
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState ba = { 0 };
    ba.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo blend = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, 0, 1, &ba };
    VkDynamicState dyn[3] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, dynamic_stride ? 3 : 2, dyn };
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo ri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, 1, &format };
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
    static const char *const de[] = { "VK_KHR_portability_subset", "VK_EXT_conservative_rasterization" };
    vkctx_init(&c, NULL, de, 2);

    VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &b };
    VkDescriptorSetLayout dsl;
    CHECK(vkCreateDescriptorSetLayout(c.device, &dli, NULL, &dsl));
    VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT, 0, 8 };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl, 1, &pcr };
    VkPipelineLayout pl;
    CHECK(vkCreatePipelineLayout(c.device, &pli, NULL, &pl));
    snprintf(path, sizeof(path), "%s/conservative_vert.spv", dir);
    VkShaderModule vs = vkctx_shader(&c, path);
    snprintf(path, sizeof(path), "%s/conservative_frag.spv", dir);
    VkShaderModule fs = vkctx_shader(&c, path);
    VkPipeline normal = make_pipeline(&c, pl, vs, fs, 0, 0), conservative = make_pipeline(&c, pl, vs, fs, 1, 0);
    VkPipeline conservative_dynamic = make_pipeline(&c, pl, vs, fs, 1, 1);

    struct vkbuf ubo = vkctx_buffer(&c, 16, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    float xform[4] = { 0.5f, 0.5f, 0.25f, -0.25f };
    memcpy(ubo.map, xform, sizeof(xform));
    float bias[2] = { -0.25f, 0.25f };
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 };
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 1, &ps };
    VkDescriptorPool dp;
    CHECK(vkCreateDescriptorPool(c.device, &dpi, NULL, &dp));
    VkDescriptorSetAllocateInfo dsa = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dp, 1, &dsl };
    VkDescriptorSet set;
    CHECK(vkAllocateDescriptorSets(c.device, &dsa, &set));
    VkDescriptorBufferInfo bi = { ubo.buffer, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, NULL, &bi, NULL };
    vkUpdateDescriptorSets(c.device, 1, &w, 0, NULL);

    /* Triangles in pixel coordinates: small, thin slivers and an acute needle. */
    static const float tris[][3][2] = {
        { { 3.3f, 4.2f }, { 9.7f, 5.1f }, { 5.4f, 11.6f } },
        { { 1.2f, 1.5f }, { 30.6f, 3.1f }, { 1.4f, 2.0f } },
        { { 16.5f, 2.2f }, { 17.1f, 29.8f }, { 16.6f, 29.9f } },
        { { 20.2f, 20.3f }, { 20.6f, 20.5f }, { 20.4f, 20.9f } },
        { { 5.0f, 25.0f }, { 28.0f, 30.0f }, { 6.0f, 25.2f } },
        { { 2.2f, 10.3f }, { 8.7f, 13.1f }, { 5.45f, 11.7f } },     /* degenerate: a segment */
        { { 25.5f, 12.25f }, { 25.5f, 12.25f }, { 25.5f, 12.25f } }, /* degenerate: a point */
        { { 2.0f, 14.0f }, { 30.0f, 15.0f }, { 12.0f, 31.0f } },    /* large, for inner coverage */
    };
    enum { TRIS = sizeof(tris) / sizeof(tris[0]) };
    struct vkbuf vbuf = vkctx_buffer(&c, TRIS * 3 * 8, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    float *vp = vbuf.map;
    for (int t = 0; t < TRIS; t++)
        for (int k = 0; k < 3; k++)
        {
            /* pixel -> NDC, then undo pos * 0.5 + (0.25, -0.25) + (-0.25, 0.25). */
            vp[(t * 3 + k) * 2 + 0] = (tris[t][k][0] / 16.0f - 1.0f) * 2.0f;
            vp[(t * 3 + k) * 2 + 1] = (tris[t][k][1] / 16.0f - 1.0f) * 2.0f;
        }
    struct vkbuf ibuf = vkctx_buffer(&c, 3 * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    uint32_t idx[3] = { 0, 2, 1 };   /* reversed winding */
    memcpy(ibuf.map, idx, sizeof(idx));

    struct vkimg img = vkctx_image(&c, VK_FORMAT_R8G8B8A8_UNORM, W, W,
                                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    for (int t = 0; t < TRIS; t++)
    {
        /* 0: normal, 1: conservative, 2: conservative indexed, 3: conservative with a dynamic stride */
        for (int mode = 0; mode < 4; mode++)
        {
            VkCommandBuffer cb = vkctx_begin(&c);
            vkctx_barrier(cb, img.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkClearColorValue zero = { { 0 } };
            VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdClearColorImage(cb, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
            vkctx_barrier(cb, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            vkctx_begin_rendering(cb, &img);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, mode == 3 ? conservative_dynamic : mode ? conservative : normal);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &set, 0, NULL);
            vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_VERTEX_BIT, 0, 8, bias);
            VkDeviceSize off = (VkDeviceSize)t * 3 * 8;
            VkDeviceSize stride = 8;
            if (mode == 3)
                vkCmdBindVertexBuffers2(cb, 0, 1, &vbuf.buffer, &off, NULL, &stride);
            else
                vkCmdBindVertexBuffers(cb, 0, 1, &vbuf.buffer, &off);
            if (mode == 2)
            {
                vkCmdBindIndexBuffer(cb, ibuf.buffer, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(cb, 3, 1, 0, 0, 0);
            }
            else
                vkCmdDraw(cb, 3, 1, 0, 0);
            vkCmdEndRendering(cb);
            vkctx_barrier(cb, img.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            struct vkbuf rb = vkctx_readback(&c, cb, &img, 4);
            vkctx_submit(&c, cb);

            const uint8_t *px = rb.map;
            int missing = 0, extra = 0, covered = 0, ref_count = 0, inner = 0, inner_missing = 0, inner_extra = 0;
            for (int y = 0; y < W; y++)
                for (int x = 0; x < W; x++)
                {
                    int got = px[(y * W + x) * 4] > 127;
                    int ref = tri_box(tris[t], x, y, x + 1, y + 1);
                    int near = tri_box(tris[t], x - 0.02f, y - 0.02f, x + 1.02f, y + 1.02f);
                    covered += got;
                    ref_count += ref;
                    if (mode ? (ref && !got) : (got && !near))
                    {
                        missing++;
                        if (getenv("VERBOSE"))
                            printf("  missing pixel (%d,%d)\n", x, y);
                    }
                    if (mode && got && !near)
                        extra++;
                    int got_inner = px[(y * W + x) * 4 + 1] > 127;
                    inner += got_inner;
                    if (mode && !got_inner && box_in_tri(tris[t], x - 0.02f, y - 0.02f, x + 1.02f, y + 1.02f))
                        inner_missing++;
                    if (got_inner && !box_in_tri(tris[t], x + 0.02f, y + 0.02f, x + 0.98f, y + 0.98f))
                        inner_extra++;
                }
            char what[160];
            snprintf(what, sizeof(what), "triangle %d mode %d: %d covered, %d reference, %d missing, %d extra; inner %d (%d missing, %d extra)",
                     t, mode, covered, ref_count, missing, extra, inner, inner_missing, inner_extra);
            printf("%s\n", what);
            EXPECT_EQ(what, missing + extra + inner_missing + inner_extra, 0);
        }
    }
    printf("%s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
