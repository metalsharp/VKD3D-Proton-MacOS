// Vulkan-level conservative rasterization emulation probe.
// Creates a pipeline with VK_EXT_conservative_rasterization OVERESTIMATE,
// renders random triangles (non-indexed), reads back, and compares the
// coverage against a CPU reference implementing D3D12 tier-1 post-snap
// semantics - pixel-exact expected.
// Usage: vk-cr-probe <libMoltenVK.dylib> [seed]
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

/* Creates (or truncates) a file for writing, owner-only (0600) and not through a symlink. */
static FILE* create_owner_file(const char* path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    return fd < 0 ? NULL : fdopen(fd, "w");
}


static const int W = 64, H = 64;
typedef struct { float x, y; } V2;
typedef struct { V2 a, b, c; } Tri;

static int refCovered(Tri t, int px, int py) {
    V2 sa = {roundf(t.a.x), roundf(t.a.y)};
    V2 sb = {roundf(t.b.x), roundf(t.b.y)};
    V2 sc = {roundf(t.c.x), roundf(t.c.y)};
    float cx = px + 0.5f, cy = py + 0.5f;
    float a2 = (sb.x-sa.x)*(sc.y-sa.y) - (sb.y-sa.y)*(sc.x-sa.x);
    int ccw = a2 >= 0;
    float e1 = (sb.x-sa.x)*(cy-sa.y) - (sb.y-sa.y)*(cx-sa.x);
    float e2 = (sc.x-sb.x)*(cy-sb.y) - (sc.y-sb.y)*(cx-sb.x);
    float e3 = (sa.x-sc.x)*(cy-sc.y) - (sa.y-sc.y)*(cx-sc.x);
    if (ccw) return e1 >= -1e-6f && e2 >= -1e-6f && e3 >= -1e-6f;
    return e1 <= 1e-6f && e2 <= 1e-6f && e3 <= 1e-6f;
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (argc < 2) { printf("usage: %s <libMoltenVK.dylib> [seed]\n", argv[0]); return 1; }
    int seed = argc > 2 ? atoi(argv[2]) : 7;
    srand(seed);

    Tri tris[16];
    for (int i = 0; i < 16; i++) {
        tris[i].a.x = 2 + rand() % 60; tris[i].a.y = 2 + rand() % 60;
        tris[i].b.x = 2 + rand() % 60; tris[i].b.y = 2 + rand() % 60;
        tris[i].c.x = 2 + rand() % 60; tris[i].c.y = 2 + rand() % 60;
    }

    void* h = dlopen(argv[1], RTLD_NOW|RTLD_LOCAL);
    if (!h) { printf("dlopen fail\n"); return 1; }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(h,"vkGetInstanceProcAddr");
    VkInstance inst; VkInstanceCreateInfo ici={VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    const char* instExts[] = { VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME };
    ici.enabledExtensionCount = 1; ici.ppEnabledExtensionNames = instExts;
    PFN_vkCreateInstance vkci = (PFN_vkCreateInstance)gipa(NULL,"vkCreateInstance");
    if (vkci(&ici,NULL,&inst)!=VK_SUCCESS){printf("inst fail\n");return 1;}
    PFN_vkEnumeratePhysicalDevices epd=(PFN_vkEnumeratePhysicalDevices)gipa(inst,"vkEnumeratePhysicalDevices");
    uint32_t n=0; epd(inst,&n,NULL); VkPhysicalDevice p; epd(inst,&n,&p);

    // Check the extension is advertised
    PFN_vkEnumerateDeviceExtensionProperties edep=(PFN_vkEnumerateDeviceExtensionProperties)gipa(inst,"vkEnumerateDeviceExtensionProperties");
    uint32_t extN = 0; edep(p,NULL,&extN,NULL);
    VkExtensionProperties* exts = malloc(extN*sizeof(VkExtensionProperties));
    edep(p,NULL,&extN,exts);
    int hasCR = 0;
    for (uint32_t i = 0; i < extN; i++) if (!strcmp(exts[i].extensionName, VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME)) hasCR = 1;
    printf("EXT_conservative_rasterization advertised: %s\n", hasCR ? "YES" : "NO");
    free(exts);
    if (!hasCR) { printf("RESULT: EXTENSION NOT ADVERTISED\n"); return 0; }

    // Properties
    PFN_vkGetPhysicalDeviceProperties2 gp2=(PFN_vkGetPhysicalDeviceProperties2)gipa(inst,"vkGetPhysicalDeviceProperties2");
    if (!gp2) { gp2=(PFN_vkGetPhysicalDeviceProperties2)gipa(inst,"vkGetPhysicalDeviceProperties2KHR"); }
    if (!gp2) { printf("no props2\n"); return 1; }
    VkPhysicalDeviceConservativeRasterizationPropertiesEXT crProps={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONSERVATIVE_RASTERIZATION_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 p2={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext=&crProps;
    gp2(p,&p2);
    printf("overestimateSize=%.3f degenerateTriangles=%d fullyCovered=%d\n",
           crProps.primitiveOverestimationSize, crProps.degenerateTrianglesRasterized,
           crProps.fullyCoveredFragmentShaderInputVariable);

    // Device with the extension
    const char* devExts[] = { VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME };
    float qprio = 1.0f;
    VkDeviceQueueCreateInfo dqc={VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; dqc.queueFamilyIndex=0; dqc.queueCount=1; dqc.pQueuePriorities=&qprio;
    VkDeviceCreateInfo dci={VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.queueCreateInfoCount=1; dci.pQueueCreateInfos=&dqc;
    dci.enabledExtensionCount=1; dci.ppEnabledExtensionNames=devExts;
    PFN_vkCreateDevice vkcdev=(PFN_vkCreateDevice)gipa(inst,"vkCreateDevice");
    VkDevice dev; 
    VkResult vr = vkcdev(p,&dci,NULL,&dev);
    printf("device create: %s (%d)\n", vr==VK_SUCCESS?"OK":"FAIL",(int)vr);
    if (vr != VK_SUCCESS) { printf("RESULT: DEVICE CREATE FAILED\n"); return 0; }
    VkQueue q; ((PFN_vkGetDeviceQueue)gipa(inst,"vkGetDeviceQueue"))(dev,0,0,&q);

    // Shaders
    static const char* vsSrc = "#version 450\nlayout(location=0) in vec2 pos;\nvoid main(){ gl_Position = vec4(pos, 0.5, 1.0); }\n";
    static const char* fsSrc = "#version 450\nlayout(location=0) out vec4 color;\nvoid main(){ color = vec4(1,0,0,1); }\n";
    FILE* f = create_owner_file("/tmp/vk-cr-vs.vert"); fputs(vsSrc,f); fclose(f);
    f = create_owner_file("/tmp/vk-cr-fs.frag"); fputs(fsSrc,f); fclose(f);
    system("glslangValidator -V /tmp/vk-cr-vs.vert -o /tmp/vk-cr-vert.spv >/dev/null 2>&1");
    system("glslangValidator -V /tmp/vk-cr-fs.frag -o /tmp/vk-cr-frag.spv >/dev/null 2>&1");
    FILE* vf = fopen("/tmp/vk-cr-vert.spv","rb"); FILE* ff = fopen("/tmp/vk-cr-frag.spv","rb");
    fseek(vf,0,SEEK_END); long vsz=ftell(vf); fseek(vf,0,SEEK_SET);
    fseek(ff,0,SEEK_END); long fsz=ftell(ff); fseek(ff,0,SEEK_SET);
    uint32_t* vcode = malloc(vsz); uint32_t* fcode = malloc(fsz);
    fread(vcode,1,vsz,vf); fread(fcode,1,fsz,ff); fclose(vf); fclose(ff);

    VkShaderModuleCreateInfo smc={VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smc.codeSize=vsz; smc.pCode=vcode; VkShaderModule vsm, fsm;
    PFN_vkCreateShaderModule vkcsm=(PFN_vkCreateShaderModule)gipa(inst,"vkCreateShaderModule");
    printf("shader modules...\n");
    vkcsm(dev,&smc,NULL,&vsm); smc.codeSize=fsz; smc.pCode=fcode; vkcsm(dev,&smc,NULL,&fsm);
    printf("shader modules done\n");
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,NULL,0,VK_SHADER_STAGE_VERTEX_BIT,vsm,"main",NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,NULL,0,VK_SHADER_STAGE_FRAGMENT_BIT,fsm,"main",NULL}};

    // Vertex input: one float2 attribute
    VkVertexInputBindingDescription vbind={0,8,VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription vattr={0,0,VK_FORMAT_R32G32_SFLOAT,0};
    VkPipelineVertexInputStateCreateInfo vis={VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,NULL,0,1,&vbind,1,&vattr};
    VkPipelineInputAssemblyStateCreateInfo ias={VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,NULL,0,VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,VK_FALSE};
    VkPipelineViewportStateCreateInfo vps={VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,NULL,0,1,NULL,1,NULL};
    VkDynamicState dynStates[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynState={VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,NULL,0,2,dynStates};
    VkPipelineRasterizationStateCreateInfo rs={VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode=VK_POLYGON_MODE_FILL; rs.cullMode=VK_CULL_MODE_NONE; rs.lineWidth=1.0f;
    VkPipelineRasterizationConservativeStateCreateInfoEXT crInfo={VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_CONSERVATIVE_STATE_CREATE_INFO_EXT};
    crInfo.conservativeRasterizationMode = VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT;
    rs.pNext = &crInfo;
    VkPipelineMultisampleStateCreateInfo ms={VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,NULL,0,VK_SAMPLE_COUNT_1_BIT,VK_FALSE,0,NULL,VK_FALSE,VK_FALSE};
    VkPipelineColorBlendAttachmentState cba={VK_FALSE};
    cba.colorWriteMask=0xf;
    VkPipelineColorBlendStateCreateInfo cbs={VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,NULL,0,VK_FALSE,VK_LOGIC_OP_COPY,1,&cba,{0,0,0,0}};
    VkPipelineLayoutCreateInfo plc={VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    PFN_vkCreatePipelineLayout vkcpl=(PFN_vkCreatePipelineLayout)gipa(inst,"vkCreatePipelineLayout");
    printf("pipeline layout...\n");
    VkPipelineLayout pl; vkcpl(dev,&plc,NULL,&pl);
    printf("pipeline layout done\n");

    VkAttachmentDescription att={0,VK_FORMAT_R8G8B8A8_UNORM,VK_SAMPLE_COUNT_1_BIT,
        VK_ATTACHMENT_LOAD_OP_CLEAR,VK_ATTACHMENT_STORE_OP_STORE,VK_ATTACHMENT_LOAD_OP_DONT_CARE,VK_ATTACHMENT_STORE_OP_DONT_CARE,
        VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference ar={0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp={0,VK_PIPELINE_BIND_POINT_GRAPHICS,0,NULL,1,&ar,NULL,NULL,0,NULL};
    VkSubpassDependency dep={VK_SUBPASS_EXTERNAL,0,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,0};
    VkRenderPassCreateInfo rpc={VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,NULL,0,1,&att,1,&sp,1,&dep};
    PFN_vkCreateRenderPass vkcrp=(PFN_vkCreateRenderPass)gipa(inst,"vkCreateRenderPass");
    printf("render pass...\n");
    VkRenderPass rp; vkcrp(dev,&rpc,NULL,&rp);
    printf("render pass done\n");

    VkGraphicsPipelineCreateInfo gpc={VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpc.stageCount=2; gpc.pStages=stages; gpc.pVertexInputState=&vis; gpc.pInputAssemblyState=&ias;
    gpc.pViewportState=&vps; gpc.pRasterizationState=&rs; gpc.pMultisampleState=&ms; gpc.pColorBlendState=&cbs;
    gpc.layout=pl; gpc.renderPass=rp; gpc.subpass=0; gpc.pDynamicState=&dynState;
    PFN_vkCreateGraphicsPipelines vkcgp=(PFN_vkCreateGraphicsPipelines)gipa(inst,"vkCreateGraphicsPipelines");
    printf("creating CR pipeline...\n");
    VkPipeline pipe;
    vr = vkcgp(dev,VK_NULL_HANDLE,1,&gpc,NULL,&pipe);
    printf("CR pipeline create: %s (%d)\n", vr==VK_SUCCESS?"OK":"FAIL",(int)vr);
    if (vr != VK_SUCCESS) { printf("RESULT: CR PIPELINE CREATE FAILED\n"); return 0; }
    // A second pipeline WITHOUT the conservative state (control render)
    VkPipeline plainPipe;
    if (getenv("PLAIN")) {
        VkGraphicsPipelineCreateInfo gpc2 = gpc;
        VkPipelineRasterizationStateCreateInfo rsPlain = rs;
        rsPlain.pNext = NULL;
        gpc2.pRasterizationState = &rsPlain;
        vr = vkcgp(dev,VK_NULL_HANDLE,1,&gpc2,NULL,&plainPipe);
        printf("plain pipeline create: %s (%d)\n", vr==VK_SUCCESS?"OK":"FAIL",(int)vr);
    }

    // Image + memory
    VkImageCreateInfo ic={VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ic.imageType=VK_IMAGE_TYPE_2D; ic.format=VK_FORMAT_R8G8B8A8_UNORM; ic.extent=(VkExtent3D){W,H,1};
    ic.mipLevels=1; ic.arrayLayers=1; ic.samples=VK_SAMPLE_COUNT_1_BIT; ic.tiling=VK_IMAGE_TILING_OPTIMAL;
    ic.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT; ic.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    PFN_vkCreateImage vkci2=(PFN_vkCreateImage)gipa(inst,"vkCreateImage");
    VkImage img; vkci2(dev,&ic,NULL,&img);
    VkMemoryRequirements mr; ((PFN_vkGetImageMemoryRequirements)gipa(inst,"vkGetImageMemoryRequirements"))(dev,img,&mr);
    VkMemoryAllocateInfo mai=(VkMemoryAllocateInfo){VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,NULL,mr.size,0};
    VkDeviceMemory imem; PFN_vkAllocateMemory vkam=(PFN_vkAllocateMemory)gipa(inst,"vkAllocateMemory"); vkam(dev,&mai,NULL,&imem);
    ((PFN_vkBindImageMemory)gipa(inst,"vkBindImageMemory"))(dev,img,imem,0);
    PFN_vkCreateImageView vkciv=(PFN_vkCreateImageView)gipa(inst,"vkCreateImageView");
    VkImageViewCreateInfo ivc={VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ivc.image=img; ivc.viewType=VK_IMAGE_VIEW_TYPE_2D; ivc.format=VK_FORMAT_R8G8B8A8_UNORM;
    ivc.subresourceRange=(VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VkImageView iv; vkciv(dev,&ivc,NULL,&iv);
    VkFramebufferCreateInfo fbc={VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,NULL,0,rp,1,&iv,W,H,1};
    PFN_vkCreateFramebuffer vkcfb=(PFN_vkCreateFramebuffer)gipa(inst,"vkCreateFramebuffer");
    VkFramebuffer fb; vkcfb(dev,&fbc,NULL,&fb);

    // Vertex buffer (16 triangles)
    VkBufferCreateInfo bc={VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bc.size=16*3*8; bc.usage=VK_BUFFER_USAGE_VERTEX_BUFFER_BIT; bc.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    PFN_vkCreateBuffer vkcb=(PFN_vkCreateBuffer)gipa(inst,"vkCreateBuffer");
    VkBuffer vb; vkcb(dev,&bc,NULL,&vb);
    ((PFN_vkGetBufferMemoryRequirements)gipa(inst,"vkGetBufferMemoryRequirements"))(dev,vb,&mr);
    mai=(VkMemoryAllocateInfo){VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,NULL,mr.size,1};
    VkDeviceMemory vmem; vkam(dev,&mai,NULL,&vmem);
    ((PFN_vkBindBufferMemory)gipa(inst,"vkBindBufferMemory"))(dev,vb,vmem,0);
    void* data; ((PFN_vkMapMemory)gipa(inst,"vkMapMemory"))(dev,vmem,0,VK_WHOLE_SIZE,0,&data);
    float* vp = (float*)data;
    for (int i = 0; i < 16; i++) {
        vp[i*6+0] = tris[i].a.x/(W/2.0f)-1.0f; vp[i*6+1] = tris[i].a.y/(H/2.0f)-1.0f;
        vp[i*6+2] = tris[i].b.x/(W/2.0f)-1.0f; vp[i*6+3] = tris[i].b.y/(H/2.0f)-1.0f;
        vp[i*6+4] = tris[i].c.x/(W/2.0f)-1.0f; vp[i*6+5] = tris[i].c.y/(H/2.0f)-1.0f;
    }
    ((PFN_vkUnmapMemory)gipa(inst,"vkUnmapMemory"))(dev,vmem);
    printf("vb[0..5] = %f %f %f %f %f %f\n", vp[0], vp[1], vp[2], vp[3], vp[4], vp[5]);
    for (int i = 0; i < 16; i++)
        printf("tri%d: (%.0f,%.0f),(%.0f,%.0f),(%.0f,%.0f)\n", i,
               tris[i].a.x, tris[i].a.y, tris[i].b.x, tris[i].b.y, tris[i].c.x, tris[i].c.y);

    // Readback buffer
    bc.size=W*H*4; bc.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer rb; vkcb(dev,&bc,NULL,&rb);
    ((PFN_vkGetBufferMemoryRequirements)gipa(inst,"vkGetBufferMemoryRequirements"))(dev,rb,&mr);
    mai=(VkMemoryAllocateInfo){VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,NULL,mr.size,1};
    VkDeviceMemory rmem; vkam(dev,&mai,NULL,&rmem);
    ((PFN_vkBindBufferMemory)gipa(inst,"vkBindBufferMemory"))(dev,rb,rmem,0);

    PFN_vkCreateCommandPool vkccp=(PFN_vkCreateCommandPool)gipa(inst,"vkCreateCommandPool");
    VkCommandPool cp; VkCommandPoolCreateInfo cpci={VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; cpci.queueFamilyIndex=0;
    vkccp(dev,&cpci,NULL,&cp);
    PFN_vkAllocateCommandBuffers vkacb=(PFN_vkAllocateCommandBuffers)gipa(inst,"vkAllocateCommandBuffers");
    VkCommandBuffer cb; VkCommandBufferAllocateInfo cbai={VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; cbai.commandPool=cp; cbai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbai.commandBufferCount=1;
    vkacb(dev,&cbai,&cb);
    PFN_vkBeginCommandBuffer vkbcb=(PFN_vkBeginCommandBuffer)gipa(inst,"vkBeginCommandBuffer");
    PFN_vkEndCommandBuffer vkecb=(PFN_vkEndCommandBuffer)gipa(inst,"vkEndCommandBuffer");
    PFN_vkQueueSubmit vkqs=(PFN_vkQueueSubmit)gipa(inst,"vkQueueSubmit");
    PFN_vkDeviceWaitIdle vkdwi=(PFN_vkDeviceWaitIdle)gipa(inst,"vkDeviceWaitIdle");
    PFN_vkCmdPipelineBarrier2 vkcpb2=(PFN_vkCmdPipelineBarrier2)gipa(inst,"vkCmdPipelineBarrier2");

    VkCommandBufferBeginInfo cbbi={VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkbcb(cb,&cbbi);
    VkRenderPassBeginInfo rpbi={VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rpbi.renderPass=rp; rpbi.framebuffer=fb; rpbi.renderArea=(VkRect2D){{0,0},{W,H}};
    VkClearValue cv=(VkClearValue){{0,0,0,0}}; rpbi.clearValueCount=1; rpbi.pClearValues=&cv;
    PFN_vkCmdBeginRenderPass vkcbp=(PFN_vkCmdBeginRenderPass)gipa(inst,"vkCmdBeginRenderPass");
    vkcbp(cb,&rpbi,VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp0={0,0,W,H,0,1}; VkRect2D sc=(VkRect2D){{0,0},{W,H}};
    PFN_vkCmdSetViewport vkcsv=(PFN_vkCmdSetViewport)gipa(inst,"vkCmdSetViewport");
    PFN_vkCmdSetScissor vkcss=(PFN_vkCmdSetScissor)gipa(inst,"vkCmdSetScissor");
    vkcsv(cb,0,1,&vp0); vkcss(cb,0,1,&sc);
    PFN_vkCmdBindPipeline vkcbp2=(PFN_vkCmdBindPipeline)gipa(inst,"vkCmdBindPipeline");
    vkcbp2(cb,VK_PIPELINE_BIND_POINT_GRAPHICS, getenv("PLAIN") ? plainPipe : pipe);
    VkDeviceSize off=0;
    PFN_vkCmdBindVertexBuffers vkcbvb=(PFN_vkCmdBindVertexBuffers)gipa(inst,"vkCmdBindVertexBuffers");
    vkcbvb(cb,0,1,&vb,&off);
    PFN_vkCmdDraw vkcd=(PFN_vkCmdDraw)gipa(inst,"vkCmdDraw");
    if (getenv("ONE_TRI")) { vkcd(cb,3,1,0,0); } else { vkcd(cb,48,1,0,0); }
    PFN_vkCmdEndRenderPass vkcerp=(PFN_vkCmdEndRenderPass)gipa(inst,"vkCmdEndRenderPass");
    vkcerp(cb);
    // copy image -> buffer
    VkImageMemoryBarrier2 imb={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    imb.srcStageMask=VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT; imb.srcAccessMask=VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    imb.dstStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT; imb.dstAccessMask=VK_ACCESS_2_TRANSFER_READ_BIT;
    imb.oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; imb.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    imb.image=img; imb.subresourceRange=(VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VkDependencyInfo depi={VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; depi.imageMemoryBarrierCount=1; depi.pImageMemoryBarriers=&imb;
    vkcpb2(cb,&depi);
    VkBufferImageCopy bic=(VkBufferImageCopy){0,0,0,(VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},{0,0,0},(VkExtent3D){W,H,1}};
    PFN_vkCmdCopyImageToBuffer vkccb=(PFN_vkCmdCopyImageToBuffer)gipa(inst,"vkCmdCopyImageToBuffer");
    vkccb(cb,img,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,rb,1,&bic);
    vkecb(cb);
    VkSubmitInfo si={VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount=1; si.pCommandBuffers=&cb;
    vr = vkqs(q,1,&si,VK_NULL_HANDLE);
    if (vr != VK_SUCCESS) { printf("submit fail: %d\n", (int)vr); }
    vkdwi(dev);

    void* rdata; ((PFN_vkMapMemory)gipa(inst,"vkMapMemory"))(dev,rmem,0,VK_WHOLE_SIZE,0,&rdata);
    uint32_t* px = (uint32_t*)rdata;
    int mismatches=0, over=0, under=0, gpuCov=0, refCov=0;
    for (int y=0;y<H;y++) for (int x=0;x<W;x++) {
        int gpu = (px[y*W+x]&0xff)>0;
        int ref = 0;
        for (int i=0;i<16;i++) if (refCovered(tris[i],x,H-2-y)) ref=1;
        if (gpu) gpuCov++;
        if (ref) refCov++;
        if (gpu && !ref) over++;
        if (!gpu && ref) { under++; if (under<6) printf("under at (%d,%d)\n",x,y); }
        if (gpu != ref) mismatches++;
    }
    printf("gpuCov=%d refCov=%d over=%d under=%d\n", gpuCov, refCov, over, under);
    if (getenv("DUMP_PPM")) {
        FILE* f = create_owner_file("/tmp/vkcr-gpu.ppm"); fprintf(f,"P3\n%d %d\n255\n",W,H);
        for (int y=0;y<H;y++) for (int x=0;x<W;x++) {
            uint32_t v=px[y*W+x];
            fprintf(f,"%d %d %d\n", v&0xff, (v>>8)&0xff, (v>>16)&0xff);
        }
        fclose(f);
    }
    printf("RESULT: %s\n", mismatches==0 ? "VULKAN CR EMULATION PIXEL-EXACT" : "MISMATCHES");
    ((PFN_vkUnmapMemory)gipa(inst,"vkUnmapMemory"))(dev,rmem);
    return 0;
}
