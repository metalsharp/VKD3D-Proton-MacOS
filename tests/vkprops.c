/* Calls vkGetPhysicalDeviceProperties2 with one extra structure chained (argv[1] picks it). */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WINE_VK_HOST
#include "wine/vulkan.h"
typedef VkResult (VKAPI_PTR *pfn_create)(const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*);
int main(int argc, char** argv) {
    HMODULE vk = LoadLibraryA("vulkan-1.dll");
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)(void*)GetProcAddress(vk, "vkGetInstanceProcAddr");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "probe", 1, "probe", 1, VK_MAKE_API_VERSION(0, 1, 4, 0)};
    VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    VkInstance inst; VkPhysicalDevice dev; uint32_t n = 1;
    if (((pfn_create)(void*)gipa(NULL, "vkCreateInstance"))(&ci, NULL, &inst)) { puts("no instance"); return 1; }
    ((PFN_vkEnumeratePhysicalDevices)(void*)gipa(inst, "vkEnumeratePhysicalDevices"))(inst, &n, &dev);
    PFN_vkGetPhysicalDeviceProperties2 props2 = (PFN_vkGetPhysicalDeviceProperties2)(void*)gipa(inst, "vkGetPhysicalDeviceProperties2");
    VkPhysicalDeviceProperties2 p = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    VkPhysicalDeviceVulkan11Properties v11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
    VkPhysicalDeviceVulkan12Properties v12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDeviceVulkan13Properties v13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES};
    VkPhysicalDeviceVulkan14Properties v14 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_PROPERTIES};
    VkPhysicalDeviceHostImageCopyProperties hic = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_PROPERTIES};
    VkPhysicalDeviceIDProperties id = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceDriverProperties drv = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    /* VKD3D-Proton's maintenance7 chain (libs/vkd3d/device.c) */
    VkPhysicalDeviceMaintenance7PropertiesKHR m7 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_7_PROPERTIES_KHR};
    VkPhysicalDeviceLayeredApiPropertiesKHR layered = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LAYERED_API_PROPERTIES_KHR};
    VkPhysicalDeviceLayeredApiPropertiesListKHR list = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LAYERED_API_PROPERTIES_LIST_KHR};
    VkPhysicalDeviceLayeredApiVulkanPropertiesKHR vk_layered = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LAYERED_API_VULKAN_PROPERTIES_KHR};
    VkPhysicalDeviceDriverProperties real_driver = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    list.layeredApiCount = 1;
    list.pLayeredApis = &layered;
    layered.pNext = &vk_layered;
    vk_layered.properties.pNext = &real_driver;
    VkPhysicalDeviceTransformFeedbackPropertiesEXT xfb = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_PROPERTIES_EXT};
    const char* which = argc > 1 ? argv[1] : "none";
    if (!strcmp(which, "v11")) p.pNext = &v11;
    else if (!strcmp(which, "v12")) p.pNext = &v12;
    else if (!strcmp(which, "v13")) p.pNext = &v13;
    else if (!strcmp(which, "v14")) p.pNext = &v14;
    else if (!strcmp(which, "hic")) p.pNext = &hic;
    else if (!strcmp(which, "id")) p.pNext = &id;
    else if (!strcmp(which, "drv")) p.pNext = &drv;
    else if (!strcmp(which, "m7")) p.pNext = &m7;
    else if (!strcmp(which, "xfb")) p.pNext = &xfb;
    else if (!strcmp(which, "layered")) { p.pNext = &list; }
    else if (!strcmp(which, "layered-nonested")) { layered.pNext = NULL; p.pNext = &list; }
    props2(dev, &p);
    if (!strcmp(which, "v14") || !strcmp(which, "hic")) {
        /* the second call, with arrays for the layouts, as DXVK and VKD3D-Proton make it */
        uint32_t* src_count = !strcmp(which, "v14") ? &v14.copySrcLayoutCount : &hic.copySrcLayoutCount;
        uint32_t* dst_count = !strcmp(which, "v14") ? &v14.copyDstLayoutCount : &hic.copyDstLayoutCount;
        VkImageLayout* src = calloc(*src_count + 1, sizeof(*src));
        VkImageLayout* dst = calloc(*dst_count + 1, sizeof(*dst));
        if (!strcmp(which, "v14")) { v14.pCopySrcLayouts = src; v14.pCopyDstLayouts = dst; }
        else { hic.pCopySrcLayouts = src; hic.pCopyDstLayouts = dst; }
        props2(dev, &p);
        printf("second call ok (src %u, dst %u)\n", *src_count, *dst_count);
    }
    printf("%s: ok (%s)", which, p.properties.deviceName);
    if (!strcmp(which, "v14")) printf(" copySrcLayoutCount=%u", v14.copySrcLayoutCount);
    if (!strcmp(which, "hic")) printf(" copySrcLayoutCount=%u", hic.copySrcLayoutCount);
    if (!strcmp(which, "xfb")) printf(" transformFeedbackQueries=%u streams=%u", xfb.transformFeedbackQueries, xfb.maxTransformFeedbackStreams);
    if (!strcmp(which, "layered")) printf(" layered api=%d driver=%s", layered.layeredAPI, real_driver.driverName);
    puts("");
    return 0;
}
