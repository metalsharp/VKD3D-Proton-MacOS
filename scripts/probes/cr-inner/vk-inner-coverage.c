// Slice 4: native-Vulkan InnerCoverage verification probe.
// Renders a fullscreen triangle with CR OVERESTIMATE + a fragment shader reading
// the FullyCoveredEXT input (red when fully covered, blue otherwise). The
// reference: a pixel is fully covered iff all 4 of its corners are inside the
// ORIGINAL (pre-snap) triangle.
// Usage: vk-inner-coverage <libMoltenVK.dylib>
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

/* Creates (or truncates) a file for writing, owner-only (0600) and not through a symlink. */
static FILE* create_owner_file(const char* path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    return fd < 0 ? NULL : fdopen(fd, "w");
}


static const int W = 64, H = 64;

static int fullyCovered(float ax, float ay, float bx, float by, float cx, float cy, int px, int py) {
    float a2 = (bx-ax)*(cy-ay) - (by-ay)*(cx-ax);
    int ccw = a2 >= 0;
    float corners[4][2] = { {px+0.0f,py+0.0f}, {px+1.0f,py+0.0f}, {px+0.0f,py+1.0f}, {px+1.0f,py+1.0f} };
    for (int c = 0; c < 4; c++) {
        float x = corners[c][0], y = corners[c][1];
        float e1 = (bx-ax)*(y-ay) - (by-ay)*(x-ax);
        float e2 = (cx-bx)*(y-by) - (cy-by)*(x-bx);
        float e3 = (ax-cx)*(y-cy) - (ay-cy)*(x-cx);
        if (ccw) { if (!(e1 >= -1e-4 && e2 >= -1e-4 && e3 >= -1e-4)) return 0; }
        else { if (!(e1 <= 1e-4 && e2 <= 1e-4 && e3 <= 1e-4)) return 0; }
    }
    return 1;
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (argc < 2) { printf("usage: %s <libMoltenVK.dylib>\n", argv[0]); return 1; }
    void* h = dlopen(argv[1], RTLD_NOW|RTLD_LOCAL);
    if (!h) { printf("dlopen fail\n"); return 1; }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(h,"vkGetInstanceProcAddr");
    VkInstance inst; VkInstanceCreateInfo ici={VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    PFN_vkCreateInstance vkci = (PFN_vkCreateInstance)gipa(NULL,"vkCreateInstance");
    if (vkci(&ici,NULL,&inst)!=VK_SUCCESS){printf("inst fail\n");return 1;}
    PFN_vkEnumeratePhysicalDevices epd=(PFN_vkEnumeratePhysicalDevices)gipa(inst,"vkEnumeratePhysicalDevices");
    uint32_t n=0; epd(inst,&n,NULL); VkPhysicalDevice p; epd(inst,&n,&p);
    const char* devExts[] = { VK_EXT_CONSERVATIVE_RASTERIZATION_EXTENSION_NAME };
    float qprio = 1.0f;
    VkDeviceQueueCreateInfo dqc={VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; dqc.queueFamilyIndex=0; dqc.queueCount=1; dqc.pQueuePriorities=&qprio;
    VkDeviceCreateInfo dci={VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.queueCreateInfoCount=1; dci.pQueueCreateInfos=&dqc;
    dci.enabledExtensionCount=1; dci.ppEnabledExtensionNames=devExts;
    PFN_vkCreateDevice vkcdev=(PFN_vkCreateDevice)gipa(inst,"vkCreateDevice");
    VkDevice dev; VkResult vr = vkcdev(p,&dci,NULL,&dev);
    printf("device: %s\n", vr==VK_SUCCESS?"OK":"FAIL");
    if (vr != VK_SUCCESS) return 1;
    VkQueue q; ((PFN_vkGetDeviceQueue)gipa(inst,"vkGetDeviceQueue"))(dev,0,0,&q);

    // Shaders: the VS passes the position; the FS reads the FullyCoveredEXT
    static const char* vsSrc = "#version 450\nlayout(location=0) in vec2 pos;\nvoid main(){ gl_Position = vec4(pos, 0.0, 1.0); }\n";
    static const char* fsSrc = "#version 450\n#extension GL_EXT_fragment_fully_covered : enable\nlayout(location=0) out vec4 color;\nvoid main(){ color = vec4(gl_FragCoord.xy / 64.0, gl_FragFullyCoveredEXT ? 1.0 : 0.0, 1.0); }\n";
    FILE* f = create_owner_file("/tmp/vk-ic-vs.vert"); fputs(vsSrc,f); fclose(f);
    f = create_owner_file("/tmp/vk-ic-fs.frag"); fputs(fsSrc,f); fclose(f);
    system("glslangValidator -V /tmp/vk-ic-vs.vert -o /tmp/vk-ic-vert.spv >/dev/null 2>&1");
    system("glslangValidator -V /tmp/vk-ic-fs.frag -o /tmp/vk-ic-frag.spv >/dev/null 2>&1");
    FILE* vf = fopen("/tmp/vk-ic-vert.spv","rb"); FILE* ff = fopen("/tmp/vk-ic-frag.spv","rb");
    if (!vf || !ff) { printf("spv missing\n"); return 1; }
    fseek(vf,0,SEEK_END); long vsz=ftell(vf); fseek(vf,0,SEEK_SET);
    fseek(ff,0,SEEK_END); long fsz=ftell(ff); fseek(ff,0,SEEK_SET);
    uint32_t* vcode = malloc(vsz); uint32_t* fcode = malloc(fsz);
    fread(vcode,1,vsz,vf); fread(fcode,1,fsz,ff); fclose(vf); fclose(ff);
    VkShaderModuleCreateInfo smc={VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    PFN_vkCreateShaderModule vkcsm=(PFN_vkCreateShaderModule)gipa(inst,"vkCreateShaderModule");
    VkShaderModule vsm, fsm;
    smc.codeSize=vsz; smc.pCode=vcode; vkcsm(dev,&smc,NULL,&vsm);
    smc.codeSize=fsz; smc.pCode=fcode; vkcsm(dev,&smc,NULL,&fsm);
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,NULL,0,VK_SHADER_STAGE_VERTEX_BIT,vsm,"main",NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,NULL,0,VK_SHADER_STAGE_FRAGMENT_BIT,fsm,"main",NULL}};
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
    VkPipelineColorBlendAttachmentState cba={VK_FALSE}; cba.colorWriteMask=0xf;
    VkPipelineColorBlendStateCreateInfo cbs={VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,NULL,0,VK_FALSE,VK_LOGIC_OP_COPY,1,&cba,{0,0,0,0}};
    VkPipelineLayoutCreateInfo plc={VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    PFN_vkCreatePipelineLayout vkcpl=(PFN_vkCreatePipelineLayout)gipa(inst,"vkCreatePipelineLayout");
    VkPipelineLayout pl; vkcpl(dev,&plc,NULL,&pl);
    VkAttachmentDescription att={0,VK_FORMAT_R8G8B8A8_UNORM,VK_SAMPLE_COUNT_1_BIT,
        VK_ATTACHMENT_LOAD_OP_CLEAR,VK_ATTACHMENT_STORE_OP_STORE,VK_ATTACHMENT_LOAD_OP_DONT_CARE,VK_ATTACHMENT_STORE_OP_DONT_CARE,
        VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference ar={0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp={0,VK_PIPELINE_BIND_POINT_GRAPHICS,0,NULL,1,&ar,NULL,NULL,0,NULL};
    VkSubpassDependency dep={VK_SUBPASS_EXTERNAL,0,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,0,VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,0};
    VkRenderPassCreateInfo rpc={VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,NULL,0,1,&att,1,&sp,1,&dep};
    PFN_vkCreateRenderPass vkcrp=(PFN_vkCreateRenderPass)gipa(inst,"vkCreateRenderPass");
    VkRenderPass rp; vkcrp(dev,&rpc,NULL,&rp);
    VkGraphicsPipelineCreateInfo gpc={VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpc.stageCount=2; gpc.pStages=stages; gpc.pVertexInputState=&vis; gpc.pInputAssemblyState=&ias;
    gpc.pViewportState=&vps; gpc.pRasterizationState=&rs; gpc.pMultisampleState=&ms; gpc.pColorBlendState=&cbs;
    gpc.layout=pl; gpc.renderPass=rp; gpc.subpass=0; gpc.pDynamicState=&dynState;
    PFN_vkCreateGraphicsPipelines vkcgp=(PFN_vkCreateGraphicsPipelines)gipa(inst,"vkCreateGraphicsPipelines");
    VkPipeline pipe; vr = vkcgp(dev,VK_NULL_HANDLE,1,&gpc,NULL,&pipe);
    printf("pipeline: %s\n", vr==VK_SUCCESS?"OK":"FAIL");
    if (vr != VK_SUCCESS) { printf("RESULT: PIPELINE FAILED (the FullyCoveredEXT input needs the MVK injection)\n"); return 1; }

    // Image + framebuffer
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

    // The fullscreen triangle vertex buffer: (-1,-1),(3,-1),(-1,3) NDC
    VkBufferCreateInfo bc={VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bc.size=3*8; bc.usage=VK_BUFFER_USAGE_VERTEX_BUFFER_BIT; bc.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    PFN_vkCreateBuffer vkcb=(PFN_vkCreateBuffer)gipa(inst,"vkCreateBuffer");
    VkBuffer vb; vkcb(dev,&bc,NULL,&vb);
    ((PFN_vkGetBufferMemoryRequirements)gipa(inst,"vkGetBufferMemoryRequirements"))(dev,vb,&mr);
    mai=(VkMemoryAllocateInfo){VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,NULL,mr.size,1};
    VkDeviceMemory vmem; vkam(dev,&mai,NULL,&vmem);
    ((PFN_vkBindBufferMemory)gipa(inst,"vkBindBufferMemory"))(dev,vb,vmem,0);
    void* data; ((PFN_vkMapMemory)gipa(inst,"vkMapMemory"))(dev,vmem,0,VK_WHOLE_SIZE,0,&data);
    float* vp = (float*)data;
    float vd[6] = { -1,-1, 3,-1, -1,3 };
    memcpy(vp, vd, 24);
    ((PFN_vkUnmapMemory)gipa(inst,"vkUnmapMemory"))(dev,vmem);

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
    VkRenderingAttachmentInfo rai={VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    rai.imageView=iv; rai.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    rai.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; rai.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    rai.clearValue=(VkClearValue){{0,0,0,0}};
    VkRenderingInfo ri={VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea=(VkRect2D){{0,0},{W,H}}; ri.layerCount=1; ri.colorAttachmentCount=1; ri.pColorAttachments=&rai;
    PFN_vkCmdBeginRendering vkcbr=(PFN_vkCmdBeginRendering)gipa(inst,"vkCmdBeginRendering");
    vkcbr(cb,&ri);
    VkViewport vp0={0,H,W,-(float)H,0,1}; VkRect2D sc=(VkRect2D){{0,0},{W,H}};
    PFN_vkCmdSetViewportWithCount vkcsv=(PFN_vkCmdSetViewportWithCount)gipa(inst,"vkCmdSetViewportWithCount");
    PFN_vkCmdSetScissorWithCount vkcss=(PFN_vkCmdSetScissorWithCount)gipa(inst,"vkCmdSetScissorWithCount");
    vkcsv(cb,1,&vp0); vkcss(cb,1,&sc);
    PFN_vkCmdBindPipeline vkcbp2=(PFN_vkCmdBindPipeline)gipa(inst,"vkCmdBindPipeline");
    vkcbp2(cb,VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkDeviceSize off=0;
    VkBuffer vb2=vb;
    PFN_vkCmdBindVertexBuffers2 vkcbvb=(PFN_vkCmdBindVertexBuffers2)gipa(inst,"vkCmdBindVertexBuffers2");
    vkcbvb(cb,0,1,&vb2,&off,NULL,NULL);
    PFN_vkCmdDraw vkcd=(PFN_vkCmdDraw)gipa(inst,"vkCmdDraw");
    vkcd(cb,3,1,0,0);
    PFN_vkCmdEndRendering vkcer=(PFN_vkCmdEndRendering)gipa(inst,"vkCmdEndRendering");
    vkcer(cb);
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
    vkdwi(dev);

    // The reference: the fullscreen triangle in pixel space is (0,0),(2W,0),(0,2H);
    // a pixel (px,py) is fully covered iff ALL 4 corners are inside.
    void* rdata; ((PFN_vkMapMemory)gipa(inst,"vkMapMemory"))(dev,rmem,0,VK_WHOLE_SIZE,0,&rdata);
    uint32_t* px = (uint32_t*)rdata;
    int red = 0, blue = 0, mismatches = 0, refFc = 0;
    for (int y=0;y<H;y++) for (int x=0;x<W;x++) {
        int gpu = (px[y*W+x]&0xff) > 128;
        /* the readback's y is flipped (the Metal's row order) - use y' = H-1-y */
        int ref = fullyCovered(0, 0, 2*W, 0, 0, 2*H, x, H-2-y);
        if (gpu) red++; else blue++;
        if (ref) refFc++;
        if (gpu != ref) mismatches++;
    }
    printf("red=%d blue=%d ref_fully_covered=%d mismatches=%d\n", red, blue, refFc, mismatches);
    /* position map: r=x/64*255 g=y/64*255 - sample some pixels */
    for (int yy = 0; yy < 4; yy++) {
        printf("posrow%d: ", yy);
        for (int x = 0; x < 8; x++) {
            uint32_t v = px[yy*W+x];
            printf("(%d,%d) ", (v&0xff)>>2, ((v>>8)&0xff)>>2);
        }
        printf("\n");
    }
    /* per-pixel: r=x/64*255 g=y/64*255 b=fc a=1. decode position + fc bit */
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint32_t v = px[y*W+x];
            float fx = (v & 0xff) / 255.0f * 64.0f;
            float fy = ((v >> 8) & 0xff) / 255.0f * 64.0f;
            int fcbit = ((v >> 16) & 0xff) > 128;
            printf("P %d %d %.2f %.2f %d\n", x, y, fx, fy, fcbit);
        }
    }
    for (int yy = 0; yy < H; yy++) {
        char line[W+1];
        for (int x = 0; x < W; x++) { line[x] = (px[yy*W+x]&0xff) > 128 ? '#' : '.'; }
        line[W] = 0;
        printf("%s\n", line);
    }
    printf("RESULT: %s\n", mismatches==0 ? "CR TIER 3 INNERCOVERAGE WORKS" : "INNERCOVERAGE MISMATCH");
    return mismatches==0 ? 0 : 2;
}
