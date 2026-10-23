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
    if (g_ != w_) { if (failures++ < 20) printf("FAIL %s: got 0x%llx want 0x%llx\n", what, g_, w_); } } while (0)
