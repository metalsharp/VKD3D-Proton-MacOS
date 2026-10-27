// End-to-end test of MoltenVK's Metal Shader Converter bridge (MetalSharp docs/MSC-BRIDGE-CONTRACT.md).
//   msctest <libMoltenVK.dylib> <cs1.dxil> <cs2.dxil> <cs3.dxil> <vs.dxil> <ps.dxil> [cs5 cs6 cs7 cs8 .dxil, or a dumped .bin blob as the 10th]
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("FAIL %s = %d (line %d)\n", #x, r_, __LINE__); exit(1); } } while (0)

static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetDeviceProcAddr gdpa;
static VkInstance inst;
static VkDevice dev;
static VkPhysicalDevice phys;
static VkQueue queue;
static uint32_t qfam;
static int failures;

#define IFN(name) PFN_##name name = (PFN_##name)gipa(inst, #name)
#define DFN(name) PFN_##name name = (PFN_##name)gdpa(dev, #name)

static void* readFile(const char* path, size_t* size) {
    FILE* f = fopen(path, "rb");
    if (!f) { printf("FAIL can't open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END); *size = ftell(f); fseek(f, 0, SEEK_SET);
    void* p = malloc(*size); fread(p, 1, *size, f); fclose(f);
    return p;
}

// --- the blob (section 1) ---
typedef struct { uint32_t type, visibility, a, b, c, r0, r1; } MSCRootParam;
typedef struct { uint32_t rangeType, numDescriptors, baseShaderRegister, registerSpace, flags, offset; } MSCRange;

#define NO_HEAPS 0xFFFFFFFFu
static uint32_t g_rsFlags, g_heapsOffset = NO_HEAPS, g_stage;	// version 2 header fields for the next blob
typedef struct { uint32_t location; char name[60]; } VtxInput;
static const VtxInput* g_vtxInputs; static uint32_t g_vtxInputCount;

static uint32_t* buildBlob(const void* dxil, size_t dxilSize, const MSCRootParam* params, uint32_t nParams,
                           const MSCRange* ranges, uint32_t nRanges, size_t* blobSize) {
    uint32_t rsSize = 16 + nParams * sizeof(MSCRootParam) + nRanges * sizeof(MSCRange);
    uint32_t dxilOff = 40, dxilPadded = (uint32_t)((dxilSize + 3) & ~3u);
    uint32_t rsOff = dxilOff + dxilPadded;
    uint32_t viOff = rsOff + rsSize;
    *blobSize = viOff + g_vtxInputCount * sizeof(VtxInput);
    uint8_t* b = calloc(1, *blobSize);
    uint32_t hdr[10] = { 0x4D534358, 2, g_stage, dxilOff, (uint32_t)dxilSize, rsOff, rsSize, g_heapsOffset,
                         g_vtxInputCount ? viOff : 0, g_vtxInputCount };
    if (g_vtxInputCount) memcpy(b + viOff, g_vtxInputs, g_vtxInputCount * sizeof(VtxInput));
    memcpy(b, hdr, sizeof(hdr));
    memcpy(b + dxilOff, dxil, dxilSize);
    uint32_t rsHdr[4] = { nParams, nRanges, 0, g_rsFlags };
    memcpy(b + rsOff, rsHdr, 16);
    memcpy(b + rsOff + 16, params, nParams * sizeof(MSCRootParam));
    memcpy(b + rsOff + 16 + nParams * sizeof(MSCRootParam), ranges, nRanges * sizeof(MSCRange));
    return (uint32_t*)b;
}

// --- buffers ---
typedef struct { VkBuffer buf; VkDeviceMemory mem; void* map; VkDeviceAddress va; VkDeviceSize size; } Buf;

static uint32_t findMemType(uint32_t bits, VkMemoryPropertyFlags want) {
    IFN(vkGetPhysicalDeviceMemoryProperties);
    VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    printf("FAIL no memory type\n"); exit(1);
}

static Buf makeBuf(VkDeviceSize size, VkBufferUsageFlags usage) {
    DFN(vkCreateBuffer); DFN(vkGetBufferMemoryRequirements); DFN(vkAllocateMemory); DFN(vkBindBufferMemory);
    DFN(vkMapMemory); DFN(vkGetBufferDeviceAddress);
    Buf b = { .size = size };
    VkBufferCreateInfo ci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
        .usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
    CHECK(vkCreateBuffer(dev, &ci, NULL, &b.buf));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, b.buf, &mr);
    VkMemoryAllocateFlagsInfo fi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT };
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &fi, mr.size,
        findMemType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    CHECK(vkAllocateMemory(dev, &ai, NULL, &b.mem));
    CHECK(vkBindBufferMemory(dev, b.buf, b.mem, 0));
    CHECK(vkMapMemory(dev, b.mem, 0, VK_WHOLE_SIZE, 0, &b.map));
    memset(b.map, 0xEE, size);
    VkBufferDeviceAddressInfo bi = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = b.buf };
    b.va = vkGetBufferDeviceAddress(dev, &bi);
    return b;
}

static VkPipeline makePipeline(const char* dxilPath, const MSCRootParam* params, uint32_t nParams,
                               const MSCRange* ranges, uint32_t nRanges, uint32_t pcSize, VkPipelineLayout* outLayout,
                               VkResult* outResult) {
    DFN(vkCreateShaderModule); DFN(vkCreatePipelineLayout); DFN(vkCreateComputePipelines);
    size_t dxilSize, blobSize;
    void* dxil = readFile(dxilPath, &dxilSize);
    uint32_t* blob;
    if (strstr(dxilPath, ".bin")) { blob = dxil; blobSize = dxilSize; dxil = malloc(1); }	// a blob as VKD3D-Proton wrote it
    else blob = buildBlob(dxil, dxilSize, params, nParams, ranges, nRanges, &blobSize);
    VkShaderModuleCreateInfo smci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = blobSize, .pCode = blob };
    VkShaderModule sm;
    CHECK(vkCreateShaderModule(dev, &smci, NULL, &sm));
    memset(blob, 0xCD, blobSize);	// the caller owns pCode: MoltenVK must have copied it
    free(blob); free(dxil);
    VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, pcSize };
    VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
    CHECK(vkCreatePipelineLayout(dev, &plci, NULL, outLayout));
    VkComputePipelineCreateInfo cpci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main" },
        .layout = *outLayout };
    VkPipeline pl = VK_NULL_HANDLE;
    *outResult = vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pl);
    return pl;
}

static void run(VkPipeline pl, VkPipelineLayout layout, const void* pc, uint32_t pcSize, uint32_t gx, uint32_t gy,
                void (*extra)(VkCommandBuffer)) {
    DFN(vkCreateCommandPool); DFN(vkAllocateCommandBuffers); DFN(vkBeginCommandBuffer); DFN(vkCmdBindPipeline);
    DFN(vkCmdPushConstants); DFN(vkCmdDispatch); DFN(vkEndCommandBuffer); DFN(vkQueueSubmit); DFN(vkQueueWaitIdle);
    DFN(vkCmdPipelineBarrier);
    VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qfam };
    VkCommandPool pool; CHECK(vkCreateCommandPool(dev, &cpi, NULL, &pool));
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb; CHECK(vkAllocateCommandBuffers(dev, &cai, &cb));
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    CHECK(vkBeginCommandBuffer(cb, &bi));
    if (extra) extra(cb);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl);
    vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, pcSize, pc);
    vkCmdDispatch(cb, gx, gy, 1);
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    CHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(queue));
}

static int expectEq(const char* what, uint32_t i, uint32_t got, uint32_t want) {
    if (got != want) { if (failures++ < 10) printf("  MISMATCH %s[%u] = %u, expected %u\n", what, i, got, want); return 0; }
    return 1;
}

static VkImage g_image, g_image9;
static void transitionImage9(VkCommandBuffer cb);
static uint64_t g_ivHandle, g_smpHandle;
static void transitionImage(VkCommandBuffer cb) {
    DFN(vkCmdPipelineBarrier);
    VkImageMemoryBarrier ib = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT, .oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = g_image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
}

static void transitionImage9(VkCommandBuffer cb) {
    DFN(vkCmdPipelineBarrier);
    VkImageMemoryBarrier ib = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT, .oldLayout = VK_IMAGE_LAYOUT_PREINITIALIZED,
        .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = g_image9, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
}

int main(int argc, char** argv) {
    if (argc < 7) { printf("usage: msctest libMoltenVK.dylib cs1.dxil cs2.dxil cs3.dxil vs.dxil ps.dxil\n"); return 2; }
    void* mvk = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!mvk) { printf("FAIL dlopen: %s\n", dlerror()); return 1; }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(mvk, "vkGetInstanceProcAddr");
    PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "msctest", .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    CHECK(vkCreateInstance(&ici, NULL, &inst));
    IFN(vkEnumeratePhysicalDevices); IFN(vkGetPhysicalDeviceQueueFamilyProperties); IFN(vkCreateDevice);
    IFN(vkEnumerateDeviceExtensionProperties); IFN(vkGetPhysicalDeviceProperties);
    gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
    uint32_t n = 1; CHECK(vkEnumeratePhysicalDevices(inst, &n, &phys) < 0 ? VK_ERROR_UNKNOWN : VK_SUCCESS);
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(phys, &props);
    printf("device: %s\n", props.deviceName);

    uint32_t extCount = 0; vkEnumerateDeviceExtensionProperties(phys, NULL, &extCount, NULL);
    VkExtensionProperties* exts = calloc(extCount, sizeof(*exts));
    vkEnumerateDeviceExtensionProperties(phys, NULL, &extCount, exts);
    int hasNVX = 0;
    for (uint32_t i = 0; i < extCount; i++)
        if (!strcmp(exts[i].extensionName, VK_NVX_IMAGE_VIEW_HANDLE_EXTENSION_NAME)) { hasNVX = 1; printf("VK_NVX_image_view_handle advertised, spec version %u\n", exts[i].specVersion); }
    if (!hasNVX) { printf("FAIL VK_NVX_image_view_handle not advertised\n"); return 1; }

    VkQueueFamilyProperties qf[8]; uint32_t nq = 8; vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qf);
    for (qfam = 0; qfam < nq && !(qf[qfam].queueFlags & VK_QUEUE_COMPUTE_BIT); qfam++);
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qfam, .queueCount = 1, .pQueuePriorities = &prio };
    VkPhysicalDeviceRobustness2FeaturesEXT rb2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,
        .robustBufferAccess2 = getenv("MSCTEST_ROBUST") != NULL, .nullDescriptor = getenv("MSCTEST_ROBUST") != NULL };
    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &rb2, .dynamicRendering = VK_TRUE };
    VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13, .bufferDeviceAddress = VK_TRUE };
    const char* devExts[] = { VK_NVX_IMAGE_VIEW_HANDLE_EXTENSION_NAME, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f12, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
        .enabledExtensionCount = getenv("MSCTEST_ROBUST") ? 2 : 1, .ppEnabledExtensionNames = devExts };
    CHECK(vkCreateDevice(phys, &dci, NULL, &dev));
    DFN(vkGetDeviceQueue); vkGetDeviceQueue(dev, qfam, 0, &queue);

    DFN(vkGetImageViewHandleNVX); DFN(vkGetImageViewHandle64NVX); DFN(vkGetImageViewAddressNVX);
    printf("vkGetDeviceProcAddr: vkGetImageViewHandleNVX=%p vkGetImageViewHandle64NVX=%p vkGetImageViewAddressNVX=%p\n",
           (void*)vkGetImageViewHandleNVX, (void*)vkGetImageViewHandle64NVX, (void*)vkGetImageViewAddressNVX);
    if (!vkGetImageViewHandleNVX || !vkGetImageViewHandle64NVX || !vkGetImageViewAddressNVX) { printf("FAIL NVX entry points missing\n"); return 1; }

    // ---------------- Test 1: RootConstants(b0, 2), UAV(u0), DescriptorTable(UAV(u1)) ----------------
    printf("\n== Test 1: %s ==\n", argv[2]);
    {
        MSCRootParam params[3] = {
            { 1, 0, 0, 0, 2 },		// 32-bit constants: b0, space 0, 2 values
            { 4, 0, 0, 0, 0 },		// root UAV: u0
            { 0, 0, 0, 1, 0 },		// table: ranges[0], 1 range
        };
        MSCRange ranges[1] = { { 1, 1, 1, 0, 0, 0 } };	// UAV, 1 descriptor, u1
        uint32_t pcSize = 24;		// constants 8 + root UAV 8 + table 8
        VkPipelineLayout layout; VkResult res;
        VkPipeline pl = makePipeline(argv[2], params, 3, ranges, 1, pcSize, &layout, &res);
        printf("vkCreateComputePipelines = %d\n", res);
        if (res != VK_SUCCESS) { printf("FAIL pipeline creation\n"); return 1; }

        const uint32_t N = 64;
        Buf out0 = makeBuf(N * 4, 0), out1 = makeBuf(N * 4, 0), heap = makeBuf(4 * 24, 0);
        uint64_t* h = heap.map;
        memset(h, 0, heap.size);
        uint32_t slot = 2;					// the table starts at heap index 2
        h[slot * 3 + 0] = out1.va;			// gpuVA
        h[slot * 3 + 1] = 0;				// textureViewID
        h[slot * 3 + 2] = N * 4;			// metadata: range
        uint8_t top[24] = {0};
        uint32_t a = 7, b = 1000;
        memcpy(top + 0, &a, 4); memcpy(top + 4, &b, 4);
        uint64_t va0 = out0.va, tbl = heap.va + slot * 24;
        memcpy(top + 8, &va0, 8); memcpy(top + 16, &tbl, 8);
        run(pl, layout, top, pcSize, 1, 1, NULL);
        uint32_t ok = 0, *o0 = out0.map, *o1 = out1.map;
        for (uint32_t i = 0; i < N; i++) { ok += expectEq("u0", i, o0[i], a * i + b); ok += expectEq("u1", i, o1[i], (a ^ i) + b * 3); }
        printf("test 1: %u/%u values correct (u0[0..3] = %u %u %u %u, u1[0..3] = %u %u %u %u)\n", ok, 2 * N,
               o0[0], o0[1], o0[2], o0[3], o1[0], o1[1], o1[2], o1[3]);
    }

    // ---------------- Test 2: typed UAV via a buffer view, texture SRV, sampler; NVX handles ----------------
    printf("\n== Test 2: %s ==\n", argv[3]);
    {
        DFN(vkCreateBufferView); DFN(vkCreateImage); DFN(vkGetImageMemoryRequirements); DFN(vkAllocateMemory);
        DFN(vkBindImageMemory); DFN(vkMapMemory); DFN(vkGetImageSubresourceLayout); DFN(vkCreateImageView); DFN(vkCreateSampler);
        MSCRootParam params[5] = {
            { 1, 0, 0, 0, 2 },		// constants b0 x2
            { 4, 0, 0, 0, 0 },		// root UAV u0
            { 0, 0, 0, 1, 0 },		// table: UAV u1-u2
            { 0, 0, 1, 1, 0 },		// table: SRV t0
            { 0, 0, 2, 1, 0 },		// table: sampler s0
        };
        MSCRange ranges[3] = { { 1, 2, 1, 0, 0, 0 }, { 0, 1, 0, 0, 0, 0 }, { 3, 1, 0, 0, 0, 0 } };
        uint32_t pcSize = 40;
        VkPipelineLayout layout; VkResult res;
        VkPipeline pl = makePipeline(argv[3], params, 5, ranges, 3, pcSize, &layout, &res);
        printf("vkCreateComputePipelines = %d\n", res);
        if (res != VK_SUCCESS) { printf("FAIL pipeline creation\n"); return 1; }

        const uint32_t N = 64;
        Buf out0 = makeBuf(N * 4 + 16, 0), out1 = makeBuf(N * 4, 0);
        Buf typed = makeBuf(N * 4, VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT);
        Buf heap = makeBuf(8 * 24, 0), sheap = makeBuf(2 * 24, 0);
        VkBufferViewCreateInfo bvci = { VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, .buffer = typed.buf, .format = VK_FORMAT_R32_UINT, .range = N * 4 };
        VkBufferView bv; CHECK(vkCreateBufferView(dev, &bvci, NULL, &bv));

        VkImageCreateInfo imci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
            .extent = { 2, 1, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_LINEAR,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT, .initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED };
        CHECK(vkCreateImage(dev, &imci, NULL, &g_image));
        VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, g_image, &mr);
        VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size,
            findMemType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
        VkDeviceMemory imem; CHECK(vkAllocateMemory(dev, &ai, NULL, &imem));
        CHECK(vkBindImageMemory(dev, g_image, imem, 0));
        uint8_t* ip; CHECK(vkMapMemory(dev, imem, 0, VK_WHOLE_SIZE, 0, (void**)&ip));
        VkImageSubresource isr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 }; VkSubresourceLayout sl;
        vkGetImageSubresourceLayout(dev, g_image, &isr, &sl);
        uint8_t texels[8] = { 0, 200, 0, 255, 100, 50, 0, 255 };	// texel 0 = (0, 200), texel 1 = (100, 50)
        memcpy(ip + sl.offset, texels, 8);
        VkImageViewCreateInfo ivci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g_image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_UNORM, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        VkImageView iv; CHECK(vkCreateImageView(dev, &ivci, NULL, &iv));
        VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0.0f };
        VkSampler smp; CHECK(vkCreateSampler(dev, &sci, NULL, &smp));

        // NVX handle queries (section 4).
        VkImageViewHandleInfoNVX hi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_HANDLE_INFO_NVX, .imageView = iv, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE };
        uint64_t ivHandle = vkGetImageViewHandle64NVX(dev, &hi);
        uint32_t ivHandle32 = vkGetImageViewHandleNVX(dev, &hi);
        hi.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        uint64_t ivHandleStorage = vkGetImageViewHandle64NVX(dev, &hi);
        VkImageViewHandleInfoNVX si = { VK_STRUCTURE_TYPE_IMAGE_VIEW_HANDLE_INFO_NVX, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .sampler = smp };
        uint64_t smpHandle = vkGetImageViewHandle64NVX(dev, &si);
        VkImageViewHandleInfoNVX bi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_HANDLE_INFO_NVX, .imageView = (VkImageView)bv, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER };
        uint64_t bvHandle = vkGetImageViewHandle64NVX(dev, &bi);
        VkImageViewHandleInfoNVX ni = { VK_STRUCTURE_TYPE_IMAGE_VIEW_HANDLE_INFO_NVX, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE };
        uint64_t nullHandle = vkGetImageViewHandle64NVX(dev, &ni);
        ni.imageView = iv; ni.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        uint64_t otherHandle = vkGetImageViewHandle64NVX(dev, &ni);
        VkImageViewAddressPropertiesNVX ap = { VK_STRUCTURE_TYPE_IMAGE_VIEW_ADDRESS_PROPERTIES_NVX };
        VkResult addrRes = vkGetImageViewAddressNVX(dev, iv, &ap);
        printf("NVX: image view 0x%llx (32-bit 0x%x, storage 0x%llx), sampler 0x%llx, texel buffer view 0x%llx, null view %llu, storage-buffer type %llu, address query %d\n",
               (unsigned long long)ivHandle, ivHandle32, (unsigned long long)ivHandleStorage, (unsigned long long)smpHandle,
               (unsigned long long)bvHandle, (unsigned long long)nullHandle, (unsigned long long)otherHandle, addrRes);
        if (!ivHandle || !smpHandle || !bvHandle || ivHandle32 != (uint32_t)ivHandle || nullHandle || otherHandle || addrRes != VK_ERROR_OUT_OF_HOST_MEMORY) {
            printf("FAIL NVX handle checks\n"); failures++;
        } else {
            printf("NVX handle checks passed\n");
        }

        g_ivHandle = ivHandle; g_smpHandle = smpHandle;
        uint32_t a = 3, b = 500;
        for (int variant = 0; variant < 2; variant++) {
            memset(out0.map, 0xEE, out0.size); memset(out1.map, 0xEE, out1.size); memset(typed.map, 0xEE, typed.size);
            uint64_t* h = heap.map; memset(h, 0, heap.size);
            // CBV_SRV_UAV shadow heap: u1 at 3, u2 at 4 (table at 3), t0 at 6 (table at 6).
            h[3 * 3 + 0] = out1.va; h[3 * 3 + 1] = 0; h[3 * 3 + 2] = N * 4;
            h[4 * 3 + 0] = typed.va; h[4 * 3 + 1] = bvHandle; h[4 * 3 + 2] = (uint64_t)(N * 4) | (1ull << 63);
            h[6 * 3 + 0] = 0; h[6 * 3 + 1] = ivHandle; h[6 * 3 + 2] = 0;
            // Sampler shadow heap, index 1. Variant 0: the contract's layout (handle in textureViewID);
            // variant 1: the converter runtime's IRDescriptorTableSetSampler (handle in gpuVA).
            uint64_t* sh = sheap.map; memset(sh, 0, sheap.size);
            if (variant == 0) { sh[1 * 3 + 1] = smpHandle; } else { sh[1 * 3 + 0] = smpHandle; }
            uint8_t top[40] = {0};
            memcpy(top + 0, &a, 4); memcpy(top + 4, &b, 4);
            uint64_t v;
            v = out0.va;               memcpy(top + 8, &v, 8);
            v = heap.va + 3 * 24;      memcpy(top + 16, &v, 8);
            v = heap.va + 6 * 24;      memcpy(top + 24, &v, 8);
            v = sheap.va + 1 * 24;     memcpy(top + 32, &v, 8);
            run(pl, layout, top, pcSize, 1, 2, variant == 0 ? transitionImage : NULL);
            uint32_t ok = 0, *o0 = out0.map, *o1 = out1.map, *ty = typed.map;
            int f0 = failures;
            for (uint32_t i = 0; i < N; i++) {
                ok += expectEq("u0", i, o0[i], a * i + b);
                ok += expectEq("u1", i, o1[i], (a ^ i) + b * 3);
                ok += expectEq("u2 (typed)", i, ty[i], a + i * 7);
            }
            failures = f0;	// the sampled values are judged below
            printf("test 2 variant %d (%s): %u/%u buffer values correct; sampled texel (r, g) = (%u, %u): %s\n", variant,
                   variant == 0 ? "sampler handle in textureViewID, as the contract says" : "sampler handle in gpuVA, as IRDescriptorTableSetSampler does",
                   ok, 3 * N, o0[64], o0[65],
                   (o0[64] == 50 && o0[65] == 125) ? "linear filtering (sampler used)" :
                   (o0[64] == 100 && o0[65] == 50) || (o0[64] == 0 && o0[65] == 200) ? "nearest filtering (sampler NOT used)" : "unexpected");
            if (ok != 3 * N) failures++;
        }
    }

    // ---------------- Test 3: SM 6.6 heap indexing; heaps bound from the addresses after the top-level buffer ----------------
    printf("\n== Test 3: %s ==\n", argv[4]);
    {
        MSCRootParam params[1] = { { 1, 0, 0, 0, 4 } };	// constants b0 x4
        g_rsFlags = 0x400 | 0x800;		// CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED
        g_heapsOffset = 16;				// after the 16-byte top-level buffer
        uint32_t pcSize = 32;
        VkPipelineLayout layout; VkResult res;
        VkPipeline pl = makePipeline(argv[4], params, 1, NULL, 0, pcSize, &layout, &res);
        g_rsFlags = 0; g_heapsOffset = NO_HEAPS;
        printf("vkCreateComputePipelines = %d\n", res);
        if (res != VK_SUCCESS) { printf("FAIL pipeline creation\n"); return 1; }

        const uint32_t N = 64;
        // Offset the heaps inside larger buffers, so a binding that ignores the offset reads the wrong entries.
        Buf out = makeBuf((N + 2) * 4, 0), heap = makeBuf(64 + 16 * 24, 0), sheap = makeBuf(64 + 8 * 24, 0);
        uint64_t* h = (uint64_t*)((uint8_t*)heap.map + 64); memset(heap.map, 0, heap.size);
        uint64_t* sh = (uint64_t*)((uint8_t*)sheap.map + 64); memset(sheap.map, 0, sheap.size);
        h[5 * 3 + 0] = out.va; h[5 * 3 + 2] = (N + 2) * 4;		// RWStructuredBuffer at heap index 5
        h[9 * 3 + 1] = g_ivHandle;								// Texture2D at heap index 9
        sh[3 * 3 + 0] = g_smpHandle;							// sampler at sampler heap index 3 (handle in gpuVA)
        uint8_t pc[32] = {0};
        uint32_t consts[4] = { 5, 9, 3, 11 };
        memcpy(pc, consts, 16);
        uint64_t v = heap.va + 64; memcpy(pc + 16, &v, 8);
        v = sheap.va + 64;         memcpy(pc + 24, &v, 8);
        run(pl, layout, pc, pcSize, 1, 1, NULL);
        uint32_t ok = 0, *o = out.map;
        for (uint32_t i = 0; i < N; i++) ok += expectEq("heap UAV", i, o[i], i * 11 + 5);
        printf("test 3: %u/%u heap-indexed values correct; sampled texel (r, g) = (%u, %u): %s\n", ok, N, o[64], o[65],
               (o[64] == 50 && o[65] == 125) ? "linear filtering (heap texture and heap sampler used)" : "WRONG");
        if (!(o[64] == 50 && o[65] == 125)) failures++;
    }

    // ---------------- Test 5: typed buffer element offsets (argv[7]) ----------------
    if (argc > 7) {
        printf("\n== Test 5: %s ==\n", argv[7]);
        DFN(vkCreateBufferView);
        MSCRootParam params[2] = { { 0, 0, 0, 1, 0 }, { 4, 0, 1, 0, 0 } };	// table UAV u0; root UAV u1
        MSCRange ranges[1] = { { 1, 1, 0, 0, 0, 0 } };
        VkPipelineLayout layout; VkResult res;
        VkPipeline pl = makePipeline(argv[7], params, 2, ranges, 1, 16, &layout, &res);
        if (res != VK_SUCCESS) { printf("FAIL pipeline creation\n"); return 1; }
        Buf data = makeBuf(256 * 4, VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT), out = makeBuf(16, 0), heap = makeBuf(24, 0);
        DFN(vkGetImageViewHandle64NVX);
        uint32_t firsts[] = { 0, 1, 3, 5, 9, 20, 100 };
        for (int mode = 0; mode < 3; mode++) {
            for (unsigned f = 0; f < sizeof(firsts) / sizeof(firsts[0]); f++) {
                uint32_t first = firsts[f];
                // mode 0: texel view of the whole buffer, offset = first; mode 1: view from the 16-byte boundary below.
                // mode 2: a view of exactly the D3D view, at its first element.
                uint32_t viewStart = mode == 2 ? first * 4 : mode ? (first * 4) & ~15u : 0, off = mode == 2 ? 0 : first - viewStart / 4;
                VkBufferViewCreateInfo bvci = { VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, .buffer = data.buf,
                    .format = VK_FORMAT_R32_UINT, .offset = viewStart, .range = mode == 2 ? 16 : 256 * 4 - viewStart };
                VkBufferView bv; CHECK(vkCreateBufferView(dev, &bvci, NULL, &bv));
                VkImageViewHandleInfoNVX hi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_HANDLE_INFO_NVX, .imageView = (VkImageView)bv,
                    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER };
                uint64_t* h = heap.map;
                h[0] = data.va + first * 4; h[1] = vkGetImageViewHandle64NVX(dev, &hi);
                // MSCTEST_MD: 0 = off<<32 | typed, 1 = off<<32, 2 = bytes<<32 | typed, 3 = off<<48 | typed, 4 = size incl. padding
                int md = getenv("MSCTEST_MD") ? atoi(getenv("MSCTEST_MD")) : 0;
                uint64_t sz = 4 * 4 + (md == 4 ? off * 4 : 0);
                uint64_t o = md == 2 ? off * 4 : off;
                h[2] = sz | (o << (md == 3 ? 48 : 32)) | (md == 1 || md == 5 ? 0 : (1ull << 63));
                if (md == 5) { h[1] = 0; h[2] = sz; }
                memset(data.map, 0, data.size); memset(out.map, 0xEE, out.size);
                uint8_t top[16]; uint64_t v = heap.va; memcpy(top, &v, 8); v = out.va; memcpy(top + 8, &v, 8);
                run(pl, layout, top, 16, 1, 1, NULL);
                uint32_t* d = data.map; int at = -1;
                for (int i = 0; i < 256; i++) if (d[i] == 100) { at = i; break; }
                int good = at == (int)first && d[first + 1] == 101 && d[first + 3] == 103 && ((uint32_t*)out.map)[0] == 4;
                printf("  mode %d (%s) first %3u: view offset %3u, element 100 written at %3d, dims %u: %s\n", mode,
                       mode == 2 ? "exact view" : mode ? "view at 16-byte boundary" : "view at buffer start", first, off, at, ((uint32_t*)out.map)[0], good ? "ok" : "WRONG");
                // Only views starting 16-byte aligned can be expressed (the converter ignores the offset); the rest is
                // why VKD3D-Proton keeps typed buffers off the bridge.
                if (mode == 2 && !good && (first * 4) % 16 == 0) failures++;
            }
        }
    }

    // ---------------- Test 6: typed SRV element offsets (argv[8]) ----------------
    if (argc > 8) {
        printf("\n== Test 6: %s ==\n", argv[8]);
        DFN(vkCreateBufferView); DFN(vkGetImageViewHandle64NVX);
        MSCRootParam params[2] = { { 0, 0, 0, 1, 0 }, { 4, 0, 1, 0, 0 } };	// table SRV t0; root UAV u1
        MSCRange ranges[1] = { { 0, 1, 0, 0, 0, 0 } };
        VkPipelineLayout layout; VkResult res;
        VkPipeline pl = makePipeline(argv[8], params, 2, ranges, 1, 16, &layout, &res);
        if (res != VK_SUCCESS) { printf("FAIL pipeline creation\n"); return 1; }
        Buf data = makeBuf(256 * 4, VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT), out = makeBuf(32, 0), heap = makeBuf(24, 0);
        for (int i = 0; i < 256; i++) ((uint32_t*)data.map)[i] = 1000 + i;
        uint32_t firsts[] = { 1, 5, 9 };
        for (unsigned f = 0; f < 3; f++) {
            uint32_t first = firsts[f], viewStart = (first * 4) & ~15u, off = first - viewStart / 4;
            VkBufferViewCreateInfo bvci = { VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, .buffer = data.buf,
                .format = VK_FORMAT_R32_UINT, .offset = viewStart, .range = 256 * 4 - viewStart };
            VkBufferView bv; CHECK(vkCreateBufferView(dev, &bvci, NULL, &bv));
            VkImageViewHandleInfoNVX hi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_HANDLE_INFO_NVX, .imageView = (VkImageView)bv,
                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER };
            uint64_t* h = heap.map;
            h[0] = data.va + first * 4; h[1] = vkGetImageViewHandle64NVX(dev, &hi);
            h[2] = (uint64_t)(4 * 4) | ((uint64_t)off << 32) | (1ull << 63);
            memset(out.map, 0xEE, out.size);
            uint8_t top[16]; uint64_t v = heap.va; memcpy(top, &v, 8); v = out.va; memcpy(top + 8, &v, 8);
            run(pl, layout, top, 16, 1, 1, NULL);
            uint32_t* o = out.map;
            printf("  SRV first %u (view offset %u): read %u %u %u %u (want %u..), dims %u: %s\n", first, off, o[1], o[2], o[3], o[4],
                   1000 + first, o[0], o[1] == 1000 + first && o[0] == 4 ? "ok" : "WRONG");
        }
    }

    // ---------------- Test 7: non-uniform indexing (argv[9]) ----------------
    if (argc > 9) {
        printf("\n== Test 7: %s ==\n", argv[9]);
        MSCRootParam params[2] = { { 0, 0, 0, 1, 0 }, { 4, 0, 1, 0, 0 } };
        MSCRange ranges[1] = { { 0, 64, 0, 0, 0, 0 } };
        VkPipelineLayout layout; VkResult res;
        VkPipeline pl = makePipeline(argv[9], params, 2, ranges, 1, 16, &layout, &res);
        if (res != VK_SUCCESS) { printf("FAIL pipeline creation\n"); return 1; }
        Buf data = makeBuf(64 * 16, 0), out = makeBuf(64 * 4, 0), heap = makeBuf(64 * 24, 0);
        uint64_t* h = heap.map;
        for (int i = 0; i < 64; i++) { ((uint32_t*)data.map)[i * 4] = 500 + i; h[i * 3] = data.va + i * 16; h[i * 3 + 1] = 0; h[i * 3 + 2] = 16; }
        for (int rep = 0; rep < 3; rep++) {
            memset(out.map, 0xEE, out.size);
            uint8_t top[16]; uint64_t v = heap.va; memcpy(top, &v, 8); v = out.va; memcpy(top + 8, &v, 8);
            run(pl, layout, top, 16, 1, 1, NULL);
            uint32_t* o = out.map; int bad = 0;
            for (int i = 0; i < 64; i++) if (o[i] != 500u + i) { if (bad++ < 4) printf("  lane %d read %u (want %u)\n", i, o[i], 500 + i); }
            printf("test 7 run %d: %d/64 lanes wrong\n", rep, bad);
            if (bad) failures++;
        }
    }

    // ---------------- Test 8: non-uniform sampler indexing (argv[10]) ----------------
    if (argc > 10) {
        printf("\n== Test 8: %s ==\n", argv[10]);
        DFN(vkCreateSampler); DFN(vkGetImageViewHandle64NVX);
        MSCRootParam params[3] = { { 0, 0, 0, 1, 0 }, { 0, 0, 1, 1, 0 }, { 4, 0, 1, 0, 0 } };
        MSCRange ranges[2] = { { 0, 1, 0, 0, 0, 0 }, { 3, 64, 0, 0, 0, 0 } };
        VkPipelineLayout layout; VkResult res;
        VkPipeline pl = makePipeline(argv[10], params, 3, ranges, 2, 24, &layout, &res);
        if (res != VK_SUCCESS) { printf("FAIL pipeline creation\n"); return 1; }
        VkSampler smp[2];
        for (int i = 0; i < 2; i++) {
            VkSamplerAddressMode m = i ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
            VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
                .addressModeU = m, .addressModeV = m, .addressModeW = m, .maxLod = 0.0f };
            CHECK(vkCreateSampler(dev, &sci, NULL, &smp[i]));
        }
        uint64_t sh[2];
        for (int i = 0; i < 2; i++) {
            VkImageViewHandleInfoNVX hi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_HANDLE_INFO_NVX, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .sampler = smp[i] };
            sh[i] = vkGetImageViewHandle64NVX(dev, &hi);
        }
        printf("  sampler handles: wrap 0x%llx, clamp 0x%llx\n", (unsigned long long)sh[0], (unsigned long long)sh[1]);
        Buf out = makeBuf(64 * 4, 0), heap = makeBuf(24, 0), sheap = makeBuf(64 * 24, 0);
        uint64_t* h = heap.map; h[0] = 0; h[1] = g_ivHandle; h[2] = 0;
        uint64_t* s = sheap.map; for (int i = 0; i < 64; i++) { s[i * 3] = sh[i & 1]; s[i * 3 + 1] = 0; s[i * 3 + 2] = 0; }
        for (int rep = 0; rep < 3; rep++) {
            memset(out.map, 0xEE, out.size);
            uint8_t top[24]; uint64_t v = heap.va; memcpy(top, &v, 8); v = sheap.va; memcpy(top + 8, &v, 8); v = out.va; memcpy(top + 16, &v, 8);
            run(pl, layout, top, 24, 1, 1, NULL);
            uint32_t* o = out.map; int bad = 0;
            for (int i = 0; i < 64; i++) { uint32_t want = (i & 1) ? 100 : 0; if (o[i] != want) { if (bad++ < 6) printf("  lane %d read %u (want %u)\n", i, o[i], want); } }
            printf("test 8 run %d: %d/64 lanes wrong\n", rep, bad);
            if (bad) failures++;
        }
    }

    // ---------------- Test 9: VKD3D-Proton's bindless_samplers DXIL (argv[11]) ----------------
    if (argc > 11) {
        printf("\n== Test 9: %s ==\n", argv[11]);
        DFN(vkCreateSampler); DFN(vkGetImageViewHandle64NVX); DFN(vkCreateImage); DFN(vkGetImageMemoryRequirements);
        DFN(vkAllocateMemory); DFN(vkBindImageMemory); DFN(vkMapMemory); DFN(vkGetImageSubresourceLayout); DFN(vkCreateImageView);
        // MSCTEST_FLAGS: the 1.1 flags VKD3D-Proton gives a 1.0 root signature (volatile descriptors and data).
        int vol = getenv("MSCTEST_FLAGS") != NULL;
        MSCRootParam params[3] = { { 0, 0, 0, 1, 0 }, { 0, 0, 1, 1, 0 }, { 4, 0, 0, 0, vol ? 2 : 0 } };
        MSCRange ranges[2] = { { 0, 1, 0, 0, vol ? 3 : 0, 0 }, { 3, 1024, 0, 0, vol ? (getenv("MSCTEST_SMPDATA") ? 3 : 1) : 0, 0 } };
        VkPipelineLayout layout; VkResult res;
        VkPipeline pl = makePipeline(argv[11], params, 3, ranges, 2, 24, &layout, &res);
        if (res != VK_SUCCESS) { printf("FAIL pipeline creation\n"); return 1; }
        VkImageCreateInfo imci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R32_SFLOAT,
            .extent = { 2, 2, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_LINEAR,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT, .initialLayout = VK_IMAGE_LAYOUT_PREINITIALIZED };
        VkImage img; CHECK(vkCreateImage(dev, &imci, NULL, &img)); g_image9 = img;
        VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, img, &mr);
        VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size,
            findMemType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
        VkDeviceMemory imem; CHECK(vkAllocateMemory(dev, &ai, NULL, &imem)); CHECK(vkBindImageMemory(dev, img, imem, 0));
        uint8_t* ip; CHECK(vkMapMemory(dev, imem, 0, VK_WHOLE_SIZE, 0, (void**)&ip));
        VkImageSubresource isr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 }; VkSubresourceLayout sl; vkGetImageSubresourceLayout(dev, img, &isr, &sl);
        float row0[2] = { 10, 100 }, row1[2] = { 100, 100 };
        memcpy(ip + sl.offset, row0, 8); memcpy(ip + sl.offset + sl.rowPitch, row1, 8);
        VkImageViewCreateInfo ivci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = VK_FORMAT_R32_SFLOAT, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        VkImageView iv; CHECK(vkCreateImageView(dev, &ivci, NULL, &iv));
        VkImageViewHandleInfoNVX ihi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_HANDLE_INFO_NVX, .imageView = iv, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE };
        uint64_t ivh = vkGetImageViewHandle64NVX(dev, &ihi);
        uint64_t sh[2];
        int swap = getenv("MSCTEST_SWAP") != NULL;	// create the clamp sampler first, so its handle is the lower one
        for (int k = 0; k < 2; k++) {
            int i = swap ? 1 - k : k;
            VkSamplerAddressMode m = i ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
            VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
                .addressModeU = m, .addressModeV = m, .addressModeW = m, .maxLod = 0.0f, .minLod = swap ? 0.0f : 0.0f,
                .borderColor = swap ? VK_BORDER_COLOR_INT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK };
            VkSampler smp; CHECK(vkCreateSampler(dev, &sci, NULL, &smp));
            VkImageViewHandleInfoNVX hi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_HANDLE_INFO_NVX, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .sampler = smp };
            sh[i] = vkGetImageViewHandle64NVX(dev, &hi);
        }
        printf("  sampler handles: wrap %llu, clamp %llu\n", (unsigned long long)sh[0], (unsigned long long)sh[1]);
        Buf out = makeBuf(64 * 4, 0), heap = makeBuf(24, 0), sheap = makeBuf(1024 * 24, 0);
        uint64_t* h = heap.map; h[0] = 0; h[1] = ivh; h[2] = 0;
        uint64_t* sp = sheap.map; for (int i = 0; i < 1024; i++) { sp[i * 3] = sh[i & 1]; sp[i * 3 + 1] = 0; sp[i * 3 + 2] = 0; }
        for (int first = 0; first < 2; first++) {
            memset(out.map, 0xEE, out.size);
            uint8_t top[24]; uint64_t v = heap.va; memcpy(top, &v, 8); v = sheap.va; memcpy(top + 8, &v, 8); v = out.va; memcpy(top + 16, &v, 8);
            run(pl, layout, top, 24, 1, 1, first ? NULL : transitionImage9);
            uint32_t* o = out.map; int bad = 0;
            for (int i = 0; i < 64; i++) { uint32_t want = (i & 1) ? 100 : 10; if (o[i] != want) { if (bad++ < 6) printf("  lane %d read %u (want %u)\n", i, o[i], want); } }
            printf("test 9 run %d: %d/64 lanes wrong\n", first, bad);
            if (bad) failures++;
        }
    }

    // ---------------- Test 4: vertex + pixel shaders; stage-in by semantic, viewport Y as VKD3D-Proton sets it ----------------
    printf("\n== Test 4: %s + %s ==\n", argv[5], argv[6]);
    {
        DFN(vkCreateShaderModule); DFN(vkCreatePipelineLayout); DFN(vkCreateGraphicsPipelines); DFN(vkCreateImage);
        DFN(vkGetImageMemoryRequirements); DFN(vkAllocateMemory); DFN(vkBindImageMemory); DFN(vkCreateImageView);
        DFN(vkCreateCommandPool); DFN(vkAllocateCommandBuffers); DFN(vkBeginCommandBuffer); DFN(vkCmdBindPipeline);
        DFN(vkCmdPushConstants); DFN(vkCmdDraw); DFN(vkEndCommandBuffer); DFN(vkQueueSubmit); DFN(vkQueueWaitIdle);
        DFN(vkCmdPipelineBarrier); DFN(vkCmdBeginRendering); DFN(vkCmdEndRendering); DFN(vkCmdSetViewport);
        DFN(vkCmdSetScissor); DFN(vkCmdBindVertexBuffers); DFN(vkCmdCopyImageToBuffer);
        const uint32_t W = 8, H = 8;
        MSCRootParam params[1] = { { 1, 0, 0, 0, 4 } };	// constants b0 x4, visible to all
        g_rsFlags = 0x1;									// ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
        // Locations on purpose not in the converter's order: POSITION at 3, COLOR at 0.
        static const VtxInput inputs[2] = { { 3, "POSITION0" }, { 0, "COLOR0" } };
        VkShaderModule modules[2];
        const char* paths[2] = { argv[5], argv[6] };
        for (int st = 0; st < 2; st++) {
            g_stage = 1 + st; g_vtxInputs = st ? NULL : inputs; g_vtxInputCount = st ? 0 : 2;
            size_t dxilSize, blobSize; void* dxil = readFile(paths[st], &dxilSize);
            uint32_t* blob = buildBlob(dxil, dxilSize, params, 1, NULL, 0, &blobSize);
            VkShaderModuleCreateInfo smci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = blobSize, .pCode = blob };
            CHECK(vkCreateShaderModule(dev, &smci, NULL, &modules[st]));
            free(blob); free(dxil);
        }
        g_stage = 0; g_vtxInputs = NULL; g_vtxInputCount = 0; g_rsFlags = 0;
        VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 };
        VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
        VkPipelineLayout layout; CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &layout));
        VkPipelineShaderStageCreateInfo stages[2] = {
            { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = modules[0], .pName = "main" },
            { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = modules[1], .pName = "main" } };
        VkVertexInputBindingDescription vb = { 0, 24, VK_VERTEX_INPUT_RATE_VERTEX };
        VkVertexInputAttributeDescription va[2] = { { 3, 0, VK_FORMAT_R32G32_SFLOAT, 0 }, { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 8 } };
        VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .vertexBindingDescriptionCount = 1,
            .pVertexBindingDescriptions = &vb, .vertexAttributeDescriptionCount = 2, .pVertexAttributeDescriptions = va };
        VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
        VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1 };
        VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL,
            .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_CLOCKWISE, .lineWidth = 1.0f };
        VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
        VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xF };
        VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1, .pAttachments = &cba };
        VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2, .pDynamicStates = dyn };
        VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
        VkPipelineRenderingCreateInfo ri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1, .pColorAttachmentFormats = &fmt };
        VkGraphicsPipelineCreateInfo gpci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri, .stageCount = 2, .pStages = stages,
            .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
            .pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = &ds, .layout = layout };
        VkPipeline pl = VK_NULL_HANDLE;
        VkResult res = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pl);
        printf("vkCreateGraphicsPipelines = %d\n", res);
        if (res != VK_SUCCESS) { printf("FAIL pipeline creation\n"); return 1; }

        VkImageCreateInfo imci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt, .extent = { W, H, 1 },
            .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT };
        VkImage img; CHECK(vkCreateImage(dev, &imci, NULL, &img));
        VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, img, &mr);
        VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, findMemType(mr.memoryTypeBits, 0) };
        VkDeviceMemory imem; CHECK(vkAllocateMemory(dev, &ai, NULL, &imem)); CHECK(vkBindImageMemory(dev, img, imem, 0));
        VkImageViewCreateInfo ivci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        VkImageView iv; CHECK(vkCreateImageView(dev, &ivci, NULL, &iv));
        // A triangle over all of the top half in D3D clip space (y from 0 to 1 is the top), red, alpha 1.
        float verts[3][6] = { { -1, 0, 1, 0, 0, 1 }, { 3, 0, 1, 0, 0, 1 }, { -1, 4, 1, 0, 0, 1 } };
        Buf vbuf = makeBuf(sizeof(verts), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT); memcpy(vbuf.map, verts, sizeof(verts));
        Buf rb = makeBuf(W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);

        VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qfam };
        VkCommandPool pool; CHECK(vkCreateCommandPool(dev, &cpi, NULL, &pool));
        VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
        VkCommandBuffer cbuf; CHECK(vkAllocateCommandBuffers(dev, &cai, &cbuf));
        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        CHECK(vkBeginCommandBuffer(cbuf, &bi));
        VkImageMemoryBarrier ib = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        vkCmdPipelineBarrier(cbuf, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
        VkRenderingAttachmentInfo att = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = iv, .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
        VkRenderingInfo rinfo = { VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = { { 0, 0 }, { W, H } }, .layerCount = 1,
            .colorAttachmentCount = 1, .pColorAttachments = &att };
        vkCmdBeginRendering(cbuf, &rinfo);
        vkCmdBindPipeline(cbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, pl);
        VkViewport vpt = { 0, (float)H, (float)W, -(float)H, 0, 1 };	// VKD3D-Proton's D3D viewport: negative height
        vkCmdSetViewport(cbuf, 0, 1, &vpt);
        VkRect2D sc = { { 0, 0 }, { W, H } }; vkCmdSetScissor(cbuf, 0, 1, &sc);
        float offs[4] = { 0, 0, 0, 1 };
        vkCmdPushConstants(cbuf, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, offs);
        VkDeviceSize vbo = 0; vkCmdBindVertexBuffers(cbuf, 0, 1, &vbuf.buf, &vbo);
        vkCmdDraw(cbuf, 3, 1, 0, 0);
        vkCmdEndRendering(cbuf);
        ib.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; ib.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        ib.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        vkCmdPipelineBarrier(cbuf, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
        VkBufferImageCopy bic = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, H, 1 } };
        vkCmdCopyImageToBuffer(cbuf, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buf, 1, &bic);
        VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
        vkCmdPipelineBarrier(cbuf, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        CHECK(vkEndCommandBuffer(cbuf));
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cbuf };
        CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
        uint32_t* px = rb.map; uint32_t ok = 0;
        for (uint32_t y = 0; y < H; y++) {
            char row[16] = {0};
            for (uint32_t x = 0; x < W; x++) {
                uint32_t want = y < H / 2 ? 0xFF0000FFu : 0x00000000u;	// RGBA8 red (little-endian) in the top half
                row[x] = px[y * W + x] == 0xFF0000FFu ? 'R' : px[y * W + x] == 0 ? '.' : '?';
                ok += px[y * W + x] == want;
            }
            printf("  row %u: %s\n", y, row);
        }
        printf("test 4: %u/%u pixels as expected (red top half: Y the way up D3D has it)\n", ok, W * H);
        if (ok != W * H) failures++;
    }

    printf("\n%s\n", failures ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED");
    return failures ? 1 : 0;
}
