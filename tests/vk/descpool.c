/* GPU test: a descriptor pool sized for exactly one set of large sampler arrays (as DXVK's global sampler heap,
 * 2048 samplers, update-after-bind) allocates it: samplers take aux buffer space when sampler reduction is emulated. */
#include "vkcommon.h"

int main(void)
{
    VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    f12.descriptorBindingSampledImageUpdateAfterBind = f12.descriptorBindingPartiallyBound = f12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12 };
    struct vkctx c;
    vkctx_init(&c, &f2, NULL, 0);
    const uint32_t counts[3] = { 16, 1024, 2048 };
    for (uint32_t i = 0; i < 3; i++)
        for (int uab = 0; uab < 2; uab++)
        {
            uint32_t n = counts[i];
            VkDescriptorSetLayoutBinding binding = { 0, VK_DESCRIPTOR_TYPE_SAMPLER, n, VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT };
            VkDescriptorBindingFlags bf = uab ? VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT |
                                                VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT : 0;
            VkDescriptorSetLayoutBindingFlagsCreateInfo lf = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, NULL, 1, &bf };
            VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &lf,
                                                   uab ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT : 0, 1, &binding };
            VkDescriptorSetLayout l;
            CHECK(vkCreateDescriptorSetLayout(c.device, &li, NULL, &l));
            VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_SAMPLER, n };
            VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL,
                                              uab ? VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT : 0, 1, 1, &ps };
            VkDescriptorPool p;
            CHECK(vkCreateDescriptorPool(c.device, &pi, NULL, &p));
            VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, p, 1, &l };
            VkDescriptorSet s;
            char what[64];
            snprintf(what, sizeof(what), "%u samplers%s", n, uab ? ", update after bind" : "");
            EXPECT_EQ(what, vkAllocateDescriptorSets(c.device, &ai, &s), VK_SUCCESS);
            vkDestroyDescriptorPool(c.device, p, NULL);
            vkDestroyDescriptorSetLayout(c.device, l, NULL);
        }
    printf("%s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
