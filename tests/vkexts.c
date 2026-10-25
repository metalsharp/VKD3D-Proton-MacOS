#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#define WINE_VK_HOST
#include "wine/vulkan.h"
typedef VkResult (VKAPI_PTR *pfn_create)(const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*);
int main(void) {
    HMODULE vk = LoadLibraryA("vulkan-1.dll");
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)(void*)GetProcAddress(vk, "vkGetInstanceProcAddr");
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "probe", 1, "probe", 1, VK_MAKE_API_VERSION(0, 1, 4, 0)};
    VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
    VkInstance inst; VkPhysicalDevice dev; uint32_t n = 1, count = 0;
    ((pfn_create)(void*)gipa(NULL, "vkCreateInstance"))(&ci, NULL, &inst);
    ((PFN_vkEnumeratePhysicalDevices)(void*)gipa(inst, "vkEnumeratePhysicalDevices"))(inst, &n, &dev);
    PFN_vkEnumerateDeviceExtensionProperties ext = (PFN_vkEnumerateDeviceExtensionProperties)(void*)gipa(inst, "vkEnumerateDeviceExtensionProperties");
    ext(dev, NULL, &count, NULL);
    VkExtensionProperties *p = calloc(count, sizeof(*p));
    ext(dev, NULL, &count, p);
    for (uint32_t i = 0; i < count; i++) printf("%s %u\n", p[i].extensionName, p[i].specVersion);
    return 0;
}
