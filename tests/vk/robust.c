/* GPU test: robustBufferAccess2 and nullDescriptor for storage and uniform buffers on MoltenVK,
 * through a variable-count update-after-bind storage buffer array (the vkd3d-proton heap layout). */
#include "vkcommon.h"

#define HEAP_COUNT 8

int main(int argc, char **argv)
{
    const char *spv = argc > 1 ? argv[1] : "robust.spv";
    struct vkctx c;

    VkPhysicalDeviceRobustness2FeaturesEXT r2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT };
    VkPhysicalDeviceVulkan12Features v12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &r2 };
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v12 };
    {
        /* Query on a throwaway instance first so the test reports what is advertised. */
        VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "q", 1, "q", 1, VK_API_VERSION_1_3 };
        VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL };
        VkInstance inst; VkPhysicalDevice pd; uint32_t n = 1;
        CHECK(vkCreateInstance(&ici, NULL, &inst));
        CHECK(vkEnumeratePhysicalDevices(inst, &n, &pd));
        vkGetPhysicalDeviceFeatures2(pd, &f2);
        vkDestroyInstance(inst, NULL);
    }
    printf("robustBufferAccess2=%u robustImageAccess2=%u nullDescriptor=%u\n", r2.robustBufferAccess2, r2.robustImageAccess2, r2.nullDescriptor);
    if (!r2.robustBufferAccess2 || !r2.nullDescriptor)
        return 1;

    /* Chained like vkd3d-proton: other feature structures around robustness2. */
    VkPhysicalDeviceRobustness2FeaturesEXT r2e = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, NULL, VK_TRUE, VK_TRUE, VK_TRUE };
    VkPhysicalDeviceTransformFeedbackFeaturesEXT xfb = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT, &r2e, VK_TRUE, VK_FALSE };
    VkPhysicalDeviceVulkan12Features v12e = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, getenv("WITH_XFB") ? (void *)&xfb : (void *)&r2e };
    v12e.descriptorIndexing = VK_TRUE;
    v12e.runtimeDescriptorArray = VK_TRUE;
    v12e.descriptorBindingVariableDescriptorCount = VK_TRUE;
    v12e.descriptorBindingPartiallyBound = VK_TRUE;
    v12e.descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
    v12e.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2e = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &v12e };
    f2e.features.robustBufferAccess = VK_TRUE;
    static const char *const de[] = { VK_EXT_ROBUSTNESS_2_EXTENSION_NAME, "VK_KHR_portability_subset", "VK_EXT_transform_feedback" };
    vkctx_init(&c, &f2e, de, getenv("WITH_XFB") ? 3 : 2);

    /* Heap set: variable-count storage buffer array. */
    VkDescriptorBindingFlags heap_flags = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                          VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo hfi = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, NULL, 1, &heap_flags };
    VkDescriptorSetLayoutBinding hb = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u << 16, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo hli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &hfi,
                                            VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT, 1, &hb };
    VkDescriptorSetLayout heap_layout, io_layout;
    CHECK(vkCreateDescriptorSetLayout(c.device, &hli, NULL, &heap_layout));

    VkDescriptorSetLayoutBinding iob[3] = {
        { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
        { 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
        { 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
    };
    VkDescriptorSetLayoutCreateInfo ioli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 3, iob };
    CHECK(vkCreateDescriptorSetLayout(c.device, &ioli, NULL, &io_layout));

    VkDescriptorSetLayout layouts[2] = { heap_layout, io_layout };
    VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 12 };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 2, layouts, 1, &pcr };
    VkPipelineLayout pl;
    CHECK(vkCreatePipelineLayout(c.device, &pli, NULL, &pl));

    VkComputePipelineCreateInfo cpi = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    cpi.stage = (VkPipelineShaderStageCreateInfo){ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
                                                   VK_SHADER_STAGE_COMPUTE_BIT, vkctx_shader(&c, spv), "main", NULL };
    cpi.layout = pl;
    VkPipeline pipe;
    CHECK(vkCreateComputePipelines(c.device, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe));

    VkDescriptorPoolSize sizes[2] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, HEAP_COUNT + 1 }, { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 } };
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT, 2, 2, sizes };
    VkDescriptorPool dp;
    CHECK(vkCreateDescriptorPool(c.device, &dpi, NULL, &dp));
    uint32_t variable_count = HEAP_COUNT;
    VkDescriptorSetVariableDescriptorCountAllocateInfo vci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO, NULL, 1, &variable_count };
    VkDescriptorSetAllocateInfo dsa = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, &vci, dp, 1, &heap_layout };
    VkDescriptorSet sets[2];
    CHECK(vkAllocateDescriptorSets(c.device, &dsa, &sets[0]));
    VkDescriptorSetAllocateInfo dsb = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dp, 1, &io_layout };
    CHECK(vkAllocateDescriptorSets(c.device, &dsb, &sets[1]));

    struct vkbuf heap = vkctx_buffer(&c, 65536, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    struct vkbuf out = vkctx_buffer(&c, 4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    struct vkbuf ubo = vkctx_buffer(&c, 65536, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    uint32_t *h = heap.map, *o = out.map, *u = ubo.map;
    for (uint32_t i = 0; i < 65536 / 4; i++)
    {
        h[i] = 0x11000000u | i;
        u[i] = 0x22000000u | i;
    }
    memset(o, 0xcc, 4096);

    /* Heap: [2] = 64 bytes at 256 (in), [5] = null, [6] = 32 bytes at 1024 (out); the rest null. */
    VkDescriptorBufferInfo hinfo[HEAP_COUNT];
    for (uint32_t i = 0; i < HEAP_COUNT; i++)
        hinfo[i] = (VkDescriptorBufferInfo){ VK_NULL_HANDLE, 0, VK_WHOLE_SIZE };
    hinfo[2] = (VkDescriptorBufferInfo){ heap.buffer, 256, 64 };
    hinfo[6] = (VkDescriptorBufferInfo){ heap.buffer, 1024, 32 };
    VkDescriptorBufferInfo oinfo = { out.buffer, 0, VK_WHOLE_SIZE };
    VkDescriptorBufferInfo uinfo = { ubo.buffer, 4096, 64 };
    VkDescriptorBufferInfo ninfo = { VK_NULL_HANDLE, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w[4] = {
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, sets[0], 0, 0, HEAP_COUNT, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, hinfo, NULL },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, sets[1], 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &oinfo, NULL },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, sets[1], 1, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, NULL, &uinfo, NULL },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, sets[1], 2, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, NULL, &ninfo, NULL },
    };
    vkUpdateDescriptorSets(c.device, 4, w, 0, NULL);

    VkCommandBuffer cb = vkctx_begin(&c);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 2, sets, 0, NULL);
    uint32_t pc[3] = { 2, 5, 6 };
    vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
    vkCmdDispatch(cb, 1, 1, 1);
    vkctx_submit(&c, cb);

    char what[64];
    for (uint32_t t = 0; t < 64; t++)
    {
        snprintf(what, sizeof(what), "read heap[2][%u]", t);
        EXPECT_EQ(what, o[t], t < 16 ? (0x11000000u | (64 + t)) : 0);
        snprintf(what, sizeof(what), "read null[%u]", t);
        EXPECT_EQ(what, o[64 + t], 0);
        snprintf(what, sizeof(what), "atomic oob[%u]", t);
        EXPECT_EQ(what, o[128 + t], 0);
        snprintf(what, sizeof(what), "ubo[%u].x", t);
        EXPECT_EQ(what, o[192 + t], t < 4 ? (0x22000000u | (1024 + t * 4)) : 0);
        snprintf(what, sizeof(what), "null ubo[%u].y", t);
        EXPECT_EQ(what, o[256 + t], 0);
    }
    for (uint32_t i = 0; i < 128; i++)
    {
        /* Words 256..319 are heap[6]'s 8 words, then neighbours that must be untouched. */
        uint32_t word = 256 + i;
        snprintf(what, sizeof(what), "heap word %u", word);
        EXPECT_EQ(what, h[word], i < 8 ? (0xA0000000u | i) : (0x11000000u | word));
    }
    for (uint32_t i = 0; i < 256 - 80; i++)
    {
        uint32_t word = 64 + 16 + i; /* after heap[2]'s range: atomics must not land */
        snprintf(what, sizeof(what), "heap word %u", word);
        EXPECT_EQ(what, h[word], 0x11000000u | word);
    }
    for (uint32_t i = 0; i < 16; i++)
    {
        snprintf(what, sizeof(what), "heap word %u (null write)", i);
        EXPECT_EQ(what, h[i], 0x11000000u | i);
    }

    printf("%s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
