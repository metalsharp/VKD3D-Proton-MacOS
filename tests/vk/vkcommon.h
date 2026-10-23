/* Minimal native Vulkan helpers for MoltenVK GPU tests (linked directly against libMoltenVK.dylib).
 * Build: tests/vk/build.sh; run with DYLD_LIBRARY_PATH pointing at the MoltenVK under test. */
#pragma once
#define VK_ENABLE_BETA_EXTENSIONS
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s:%d: %s = %d\n", __FILE__, __LINE__, #x, r_); exit(2); } } while (0)

struct vkctx {
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    VkCommandPool pool;
    VkPhysicalDeviceMemoryProperties memory;
};

static void vkctx_init(struct vkctx *c, const void *features_chain, const char *const *extensions, uint32_t extension_count)
{
    /* No loader: MoltenVK is the instance, so no portability enumeration. */
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "mvktest", 1, "mvktest", 1, VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL };
    CHECK(vkCreateInstance(&ici, NULL, &c->instance));
    uint32_t n = 1;
    CHECK(vkEnumeratePhysicalDevices(c->instance, &n, &c->physical));
    vkGetPhysicalDeviceMemoryProperties(c->physical, &c->memory);
    c->queue_family = 0;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, c->queue_family, 1, &prio };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, features_chain, 0, 1, &qci, 0, NULL, extension_count, extensions, NULL };
    CHECK(vkCreateDevice(c->physical, &dci, NULL, &c->device));
    vkGetDeviceQueue(c->device, c->queue_family, 0, &c->queue);
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, c->queue_family };
    CHECK(vkCreateCommandPool(c->device, &pci, NULL, &c->pool));
}

static uint32_t vkctx_memory_type(struct vkctx *c, uint32_t bits, VkMemoryPropertyFlags flags)
{
    for (uint32_t i = 0; i < c->memory.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (c->memory.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    fprintf(stderr, "no memory type\n");
    exit(2);
}

struct vkbuf { VkBuffer buffer; VkDeviceMemory memory; void *map; VkDeviceSize size; };

static struct vkbuf vkctx_buffer(struct vkctx *c, VkDeviceSize size, VkBufferUsageFlags usage)
{
    struct vkbuf b = { 0 };
    b.size = size;
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    CHECK(vkCreateBuffer(c->device, &bci, NULL, &b.buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(c->device, b.buffer, &req);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size,
                                 vkctx_memory_type(c, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    CHECK(vkAllocateMemory(c->device, &mai, NULL, &b.memory));
    CHECK(vkBindBufferMemory(c->device, b.buffer, b.memory, 0));
    CHECK(vkMapMemory(c->device, b.memory, 0, VK_WHOLE_SIZE, 0, &b.map));
    return b;
}

static VkShaderModule vkctx_shader(struct vkctx *c, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t *code = malloc(size);
    if (fread(code, 1, size, f) != (size_t)size) { perror(path); exit(2); }
    fclose(f);
    VkShaderModuleCreateInfo smci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, (size_t)size, code };
    VkShaderModule m;
    CHECK(vkCreateShaderModule(c->device, &smci, NULL, &m));
    free(code);
    return m;
}

static VkCommandBuffer vkctx_begin(struct vkctx *c)
{
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, c->pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    VkCommandBuffer cb;
    CHECK(vkAllocateCommandBuffers(c->device, &cai, &cb));
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
    CHECK(vkBeginCommandBuffer(cb, &bi));
    return cb;
}

static void vkctx_submit(struct vkctx *c, VkCommandBuffer cb)
{
    CHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL };
    CHECK(vkQueueSubmit(c->queue, 1, &si, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(c->queue));
    vkFreeCommandBuffers(c->device, c->pool, 1, &cb);
}

static int failures;
#define EXPECT_EQ(what, got, want) do { unsigned long long g_ = (got), w_ = (want); \
    if (g_ != w_) { if (failures++ < 200) printf("FAIL %s: got 0x%llx want 0x%llx\n", what, g_, w_); } } while (0)

struct vkimg { VkImage image; VkDeviceMemory memory; VkImageView view; VkFormat format; uint32_t width, height; };

static struct vkimg vkctx_image(struct vkctx *c, VkFormat format, uint32_t width, uint32_t height, VkImageUsageFlags usage)
{
    struct vkimg i = { 0 };
    i.format = format; i.width = width; i.height = height;
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, format, { width, height, 1 }, 1, 1,
                              VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
                              VK_IMAGE_LAYOUT_UNDEFINED };
    CHECK(vkCreateImage(c->device, &ici, NULL, &i.image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(c->device, i.image, &req);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size,
                                 vkctx_memory_type(c, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    CHECK(vkAllocateMemory(c->device, &mai, NULL, &i.memory));
    CHECK(vkBindImageMemory(c->device, i.image, i.memory, 0));
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, i.image, VK_IMAGE_VIEW_TYPE_2D, format, { 0 },
                                  { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    CHECK(vkCreateImageView(c->device, &vci, NULL, &i.view));
    return i;
}

static void vkctx_barrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT,
                               VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, from, to,
                               VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, image, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

/* Copies a color image (in TRANSFER_SRC_OPTIMAL) into a new host buffer. */
static struct vkbuf vkctx_readback(struct vkctx *c, VkCommandBuffer cb, struct vkimg *img, uint32_t texel_size)
{
    struct vkbuf b = vkctx_buffer(c, (VkDeviceSize)img->width * img->height * texel_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkBufferImageCopy r = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { img->width, img->height, 1 } };
    vkCmdCopyImageToBuffer(cb, img->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b.buffer, 1, &r);
    return b;
}

/* A graphics pipeline for dynamic rendering into one color attachment, no vertex input. */
static VkPipeline vkctx_graphics_pipeline(struct vkctx *c, VkPipelineLayout layout, VkShaderModule vs, VkShaderModule fs, VkFormat format,
                                          const VkPipelineColorBlendStateCreateInfo *blend, const void *rasterization_next)
{
    VkPipelineShaderStageCreateInfo stages[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL },
    };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, rasterization_next };
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT };
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, dyn };
    VkPipelineRenderingCreateInfo ri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, 1, &format };
    VkGraphicsPipelineCreateInfo gpi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri, 0, 2, stages, &vi, &ia, NULL, &vp, &rs, &ms, &ds,
                                         blend, &dy, layout };
    VkPipeline p;
    CHECK(vkCreateGraphicsPipelines(c->device, VK_NULL_HANDLE, 1, &gpi, NULL, &p));
    return p;
}

/* Begins dynamic rendering into one color attachment (loaded, not cleared) with a full viewport. */
static void vkctx_begin_rendering(VkCommandBuffer cb, struct vkimg *img)
{
    VkRenderingAttachmentInfo att = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, img->view, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, { { 0, 0 }, { img->width, img->height } }, 1, 0, 1, &att };
    vkCmdBeginRendering(cb, &ri);
    VkViewport v = { 0, 0, (float)img->width, (float)img->height, 0, 1 };
    VkRect2D s = { { 0, 0 }, { img->width, img->height } };
    vkCmdSetViewport(cb, 0, 1, &v);
    vkCmdSetScissor(cb, 0, 1, &s);
}
