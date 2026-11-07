// ROV (rasterizer ordered views) execution probe: verifies that
// fragment-shader interlock (Metal raster-order-groups via MVK) serializes
// UAV writes per-pixel. Two overlapping triangles write to the same UAV
// location in a non-deterministic order; with the interlock the final value
// must be the LAST triangle's (deterministic), proving ordered execution.
// Usage: vk-rov-probe <libMoltenVK.dylib>
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


int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: %s <libMoltenVK.dylib>\n", argv[0]); return 1; }
    setvbuf(stdout, NULL, _IONBF, 0);
    void* h = dlopen(argv[1], RTLD_NOW|RTLD_LOCAL);
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(h,"vkGetInstanceProcAddr");
    VkInstance inst; VkInstanceCreateInfo ici={VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    const char* instExts[] = { VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME };
    ici.enabledExtensionCount = 1; ici.ppEnabledExtensionNames = instExts;
    PFN_vkCreateInstance vkci = (PFN_vkCreateInstance)gipa(NULL,"vkCreateInstance");
    if (vkci(&ici,NULL,&inst)!=VK_SUCCESS){printf("inst fail\n");return 1;}
    PFN_vkEnumeratePhysicalDevices epd=(PFN_vkEnumeratePhysicalDevices)gipa(inst,"vkEnumeratePhysicalDevices");
    uint32_t n=0; epd(inst,&n,NULL); VkPhysicalDevice p; epd(inst,&n,&p);

    // interlock feature check
    PFN_vkGetPhysicalDeviceFeatures2 gf2=(PFN_vkGetPhysicalDeviceFeatures2)gipa(inst,"vkGetPhysicalDeviceFeatures2");
    if (!gf2) gf2=(PFN_vkGetPhysicalDeviceFeatures2)gipa(inst,"vkGetPhysicalDeviceFeatures2KHR");
    VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT il={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 f2={VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext=&il;
    gf2(p,&f2);
    printf("pixelInterlock=%d sampleInterlock=%d\n", (int)il.fragmentShaderPixelInterlock, (int)il.fragmentShaderSampleInterlock);

    // device with the interlock extension + feature
    const char* devExts[] = { VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME };
    float qprio = 1.0f;
    VkDeviceQueueCreateInfo dqc={VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; dqc.queueFamilyIndex=0; dqc.queueCount=1; dqc.pQueuePriorities=&qprio;
    VkDeviceCreateInfo dci={VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.queueCreateInfoCount=1; dci.pQueueCreateInfos=&dqc;
    dci.enabledExtensionCount=1; dci.ppEnabledExtensionNames=devExts;
    il.fragmentShaderPixelInterlock = VK_TRUE;   // enable the feature
    dci.pNext = &il;
    PFN_vkCreateDevice vkcdev=(PFN_vkCreateDevice)gipa(inst,"vkCreateDevice");
    VkDevice dev; VkResult vr = vkcdev(p,&dci,NULL,&dev);
    printf("device create: %s (%d)\n", vr==VK_SUCCESS?"OK":"FAIL",(int)vr);
    if (vr != VK_SUCCESS) { printf("RESULT: DEVICE CREATE FAILED\n"); return 0; }
    VkQueue q; ((PFN_vkGetDeviceQueue)gipa(inst,"vkGetDeviceQueue"))(dev,0,0,&q);

    // shaders: interlocked UAV write. Two triangles drawn in order; each
    // writes its id to the UAV with a per-pixel interlock; the final value
    // must be the LAST triangle's (2) if the writes are ordered.
    static const char* vsSrc = "#version 450\nlayout(location=0) in vec2 pos;\nvoid main(){ gl_Position = vec4(pos, 0.5, 1.0); }\n";
    static const char* fsSrc =
        "#version 450\n"
        "#extension GL_EXT_fragment_shader_interlock : require\n"
        "layout(location=0) out vec4 color;\n"
        "layout(set=0, binding=0, rgba32ui) uniform uimage2D uav;\n"
        "layout(push_constant) uniform PC { int id; } pc;\n"
        "void main() {\n"
        "    beginInvocationInterlockARB();\n"
        "    imageStore(uav, ivec2(gl_FragCoord.xy), uvec4(pc.id));\n"
        "    endInvocationInterlockARB();\n"
        "    color = vec4(1,0,0,1);\n"
        "}\n";
    FILE* f = create_owner_file("/tmp/vk-rov-vs.vert"); fputs(vsSrc,f); fclose(f);
    f = create_owner_file("/tmp/vk-rov-fs.frag"); fputs(fsSrc,f); fclose(f);
    system("glslangValidator -V /tmp/vk-rov-vs.vert -o /tmp/vk-rov-vert.spv >/dev/null 2>&1");
    system("glslangValidator -V /tmp/vk-rov-fs.frag -o /tmp/vk-rov-frag.spv >/dev/null 2>&1");
    FILE* vf = fopen("/tmp/vk-rov-vert.spv","rb"); FILE* ff = fopen("/tmp/vk-rov-frag-1.spv","rb");
    if (!vf || !ff) { printf("shader compile fail\n"); return 1; }
    fseek(vf,0,SEEK_END); long vsz=ftell(vf); fseek(vf,0,SEEK_SET);
    fseek(ff,0,SEEK_END); long fsz=ftell(ff); fseek(ff,0,SEEK_SET);
    uint32_t* vcode = malloc(vsz); uint32_t* fcode = malloc(fsz);
    fread(vcode,1,vsz,vf); fread(fcode,1,fsz,ff); fclose(vf); fclose(ff);
    VkShaderModuleCreateInfo smc={VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smc.codeSize=vsz; smc.pCode=vcode; VkShaderModule vsm, fsm;
    PFN_vkCreateShaderModule vkcsm=(PFN_vkCreateShaderModule)gipa(inst,"vkCreateShaderModule");
    vkcsm(dev,&smc,NULL,&vsm); smc.codeSize=fsz; smc.pCode=fcode; vkcsm(dev,&smc,NULL,&fsm);
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
    VkPipelineMultisampleStateCreateInfo ms={VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,NULL,0,VK_SAMPLE_COUNT_1_BIT,VK_FALSE,0,NULL,VK_FALSE,VK_FALSE};
    VkPipelineColorBlendAttachmentState cba={VK_FALSE}; cba.colorWriteMask=0xf;
    VkPipelineColorBlendStateCreateInfo cbs={VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,NULL,0,VK_FALSE,VK_LOGIC_OP_COPY,1,&cba,{0,0,0,0}};
    VkDescriptorSetLayoutBinding dslb={0,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_FRAGMENT_BIT,NULL};
    VkDescriptorSetLayoutCreateInfo dsli={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,NULL,0,1,&dslb};
    PFN_vkCreateDescriptorSetLayout vkcdsl=(PFN_vkCreateDescriptorSetLayout)gipa(inst,"vkCreateDescriptorSetLayout");
    VkDescriptorSetLayout dsl; vkcdsl(dev,&dsli,NULL,&dsl);
    VkPipelineLayoutCreateInfo plc={VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plc.setLayoutCount=1; plc.pSetLayouts=&dsl;
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
    VkPipeline pipe;
    vr = vkcgp(dev,VK_NULL_HANDLE,1,&gpc,NULL,&pipe);
    printf("interlock pipeline create: %s (%d)\n", vr==VK_SUCCESS?"OK":"FAIL",(int)vr);
    if (vr != VK_SUCCESS) { printf("RESULT: INTERLOCK PIPELINE CREATE FAILED\n"); return 0; }
    // second pipeline with id=2
    VkShaderModule fsm2;
    FILE* ff2 = fopen("/tmp/vk-rov-frag-2.spv","rb");
    fseek(ff2,0,SEEK_END); long fsz2=ftell(ff2); fseek(ff2,0,SEEK_SET);
    uint32_t* fcode2 = malloc(fsz2);
    fread(fcode2,1,fsz2,ff2); fclose(ff2);
    smc.codeSize=fsz2; smc.pCode=fcode2; vkcsm(dev,&smc,NULL,&fsm2);
    VkPipelineShaderStageCreateInfo stages2[2] = {stages[0], stages[1]};
    stages2[1].module = fsm2;
    VkGraphicsPipelineCreateInfo gpc2 = gpc;
    gpc2.stageCount=2; gpc2.pStages=stages2;
    VkPipeline pipe2;
    vr = vkcgp(dev,VK_NULL_HANDLE,1,&gpc2,NULL,&pipe2);
    printf("pipeline2 create: %s (%d)\n", vr==VK_SUCCESS?"OK":"FAIL",(int)vr);

    // UAV image (R32G32B32A32_UINT) 64x64
    VkImageCreateInfo ic={VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ic.imageType=VK_IMAGE_TYPE_2D; ic.format=VK_FORMAT_R32G32B32A32_UINT; ic.extent=(VkExtent3D){64,64,1};
    ic.mipLevels=1; ic.arrayLayers=1; ic.samples=VK_SAMPLE_COUNT_1_BIT; ic.tiling=VK_IMAGE_TILING_OPTIMAL;
    ic.usage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT; ic.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    PFN_vkCreateImage vkci2=(PFN_vkCreateImage)gipa(inst,"vkCreateImage");
    VkImage uav; vkci2(dev,&ic,NULL,&uav);
    VkMemoryRequirements mr; ((PFN_vkGetImageMemoryRequirements)gipa(inst,"vkGetImageMemoryRequirements"))(dev,uav,&mr);
    VkMemoryAllocateInfo mai=(VkMemoryAllocateInfo){VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,NULL,mr.size,0};
    VkDeviceMemory umem; PFN_vkAllocateMemory vkam=(PFN_vkAllocateMemory)gipa(inst,"vkAllocateMemory"); vkam(dev,&mai,NULL,&umem);
    ((PFN_vkBindImageMemory)gipa(inst,"vkBindImageMemory"))(dev,uav,umem,0);
    PFN_vkCreateImageView vkciv=(PFN_vkCreateImageView)gipa(inst,"vkCreateImageView");
    VkImageViewCreateInfo ivc={VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ivc.image=uav; ivc.viewType=VK_IMAGE_VIEW_TYPE_2D; ivc.format=VK_FORMAT_R32G32B32A32_UINT;
    ivc.subresourceRange=(VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VkImageView uavView; vkciv(dev,&ivc,NULL,&uavView);
    VkDescriptorSetAllocateInfo dsai={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    VkDescriptorPoolSize dps={VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1};
    VkDescriptorPoolCreateInfo dpci={VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,NULL,0,1,1,&dps};
    PFN_vkCreateDescriptorPool vkcdp=(PFN_vkCreateDescriptorPool)gipa(inst,"vkCreateDescriptorPool");
    VkDescriptorPool dp; vkcdp(dev,&dpci,NULL,&dp);
    dsai.descriptorPool=dp; dsai.descriptorSetCount=1; dsai.pSetLayouts=&dsl;
    VkDescriptorSet ds; PFN_vkAllocateDescriptorSets vkads=(PFN_vkAllocateDescriptorSets)gipa(inst,"vkAllocateDescriptorSets"); vkads(dev,&dsai,&ds);
    VkDescriptorImageInfo dii={VK_NULL_HANDLE,uavView,VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wds={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wds.dstSet=ds; wds.dstBinding=0; wds.descriptorCount=1; wds.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; wds.pImageInfo=&dii;
    PFN_vkUpdateDescriptorSets vkuds=(PFN_vkUpdateDescriptorSets)gipa(inst,"vkUpdateDescriptorSets"); vkuds(dev,1,&wds,0,NULL);

    // RT + framebuffer
    VkImageCreateInfo ic2=ic; ic2.format=VK_FORMAT_R8G8B8A8_UNORM; ic2.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VkImage rt; vkci2(dev,&ic2,NULL,&rt);
    VkMemoryRequirements mr2; ((PFN_vkGetImageMemoryRequirements)gipa(inst,"vkGetImageMemoryRequirements"))(dev,rt,&mr2);
    mai=(VkMemoryAllocateInfo){VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,NULL,mr2.size,0};
    VkDeviceMemory rmem; vkam(dev,&mai,NULL,&rmem);
    ((PFN_vkBindImageMemory)gipa(inst,"vkBindImageMemory"))(dev,rt,rmem,0);
    VkImageViewCreateInfo ivc2=ivc; ivc2.image=rt; ivc2.format=VK_FORMAT_R8G8B8A8_UNORM;
    VkImageView rtView; vkciv(dev,&ivc2,NULL,&rtView);
    VkFramebufferCreateInfo fbc={VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,NULL,0,rp,1,&rtView,64,64,1};
    PFN_vkCreateFramebuffer vkcfb=(PFN_vkCreateFramebuffer)gipa(inst,"vkCreateFramebuffer");
    VkFramebuffer fb; vkcfb(dev,&fbc,NULL,&fb);

    // vertex buffer: one fullscreen triangle drawn twice (id 1 then id 2)
    float vdata[12] = {
        -1,-1, 3,-1, -1,3,  // fullscreen (covers the whole viewport)
        -1,-1, 3,-1, -1,3   // same again
    };
    VkBufferCreateInfo bc={VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bc.size=12*4; bc.usage=VK_BUFFER_USAGE_VERTEX_BUFFER_BIT; bc.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    PFN_vkCreateBuffer vkcb=(PFN_vkCreateBuffer)gipa(inst,"vkCreateBuffer");
    VkBuffer vb; vkcb(dev,&bc,NULL,&vb);
    ((PFN_vkGetBufferMemoryRequirements)gipa(inst,"vkGetBufferMemoryRequirements"))(dev,vb,&mr);
    mai=(VkMemoryAllocateInfo){VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,NULL,mr.size,1};
    VkDeviceMemory vmem; vkam(dev,&mai,NULL,&vmem);
    ((PFN_vkBindBufferMemory)gipa(inst,"vkBindBufferMemory"))(dev,vb,vmem,0);
    void* data; ((PFN_vkMapMemory)gipa(inst,"vkMapMemory"))(dev,vmem,0,VK_WHOLE_SIZE,0,&data);
    memcpy(data, vdata, 48);
    ((PFN_vkUnmapMemory)gipa(inst,"vkUnmapMemory"))(dev,vmem);
    // readback buffer
    bc.size=64*64*16; bc.usage=VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer rb; vkcb(dev,&bc,NULL,&rb);
    ((PFN_vkGetBufferMemoryRequirements)gipa(inst,"vkGetBufferMemoryRequirements"))(dev,rb,&mr);
    mai=(VkMemoryAllocateInfo){VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,NULL,mr.size,1};
    VkDeviceMemory rbMem; vkam(dev,&mai,NULL,&rbMem);
    ((PFN_vkBindBufferMemory)gipa(inst,"vkBindBufferMemory"))(dev,rb,rbMem,0);

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
    // UAV to GENERAL
    VkImageMemoryBarrier2 imb={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    imb.srcStageMask=VK_PIPELINE_STAGE_2_NONE; imb.srcAccessMask=0;
    imb.dstStageMask=VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT; imb.dstAccessMask=VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    imb.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; imb.newLayout=VK_IMAGE_LAYOUT_GENERAL;
    imb.image=uav; imb.subresourceRange=(VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VkDependencyInfo depi={VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; depi.imageMemoryBarrierCount=1; depi.pImageMemoryBarriers=&imb;
    vkcpb2(cb,&depi);
    VkRenderPassBeginInfo rpbi={VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rpbi.renderPass=rp; rpbi.framebuffer=fb; rpbi.renderArea=(VkRect2D){{0,0},{64,64}};
    VkClearValue cv=(VkClearValue){{0,0,0,0}}; rpbi.clearValueCount=1; rpbi.pClearValues=&cv;
    PFN_vkCmdBeginRenderPass vkcbp=(PFN_vkCmdBeginRenderPass)gipa(inst,"vkCmdBeginRenderPass");
    vkcbp(cb,&rpbi,VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp0={0,0,64,64,0,1}; VkRect2D sc=(VkRect2D){{0,0},{64,64}};
    PFN_vkCmdSetViewport vkcsv=(PFN_vkCmdSetViewport)gipa(inst,"vkCmdSetViewport");
    PFN_vkCmdSetScissor vkcss=(PFN_vkCmdSetScissor)gipa(inst,"vkCmdSetScissor");
    vkcsv(cb,0,1,&vp0); vkcss(cb,0,1,&sc);
    PFN_vkCmdBindPipeline vkcbp2=(PFN_vkCmdBindPipeline)gipa(inst,"vkCmdBindPipeline");
    vkcbp2(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,pipe);
    VkDeviceSize off=0;
    PFN_vkCmdBindVertexBuffers vkcbvb=(PFN_vkCmdBindVertexBuffers)gipa(inst,"vkCmdBindVertexBuffers");
    vkcbvb(cb,0,1,&vb,&off);
    PFN_vkCmdBindDescriptorSets vkcbds=(PFN_vkCmdBindDescriptorSets)gipa(inst,"vkCmdBindDescriptorSets");
    vkcbds(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,pl,0,1,&ds,0,NULL);
    PFN_vkCmdDraw vkcd=(PFN_vkCmdDraw)gipa(inst,"vkCmdDraw");
    vkcd(cb,3,1,0,0);                       // pipeline 1: id 1 (tri 1)
    if (!getenv("FIRST_ONLY")) {
        vkcbp2(cb,VK_PIPELINE_BIND_POINT_GRAPHICS,pipe2);
        vkcd(cb,3,1,0,0);                       // pipeline 2: id 2 (same tri)
    }
    PFN_vkCmdEndRenderPass vkcerp=(PFN_vkCmdEndRenderPass)gipa(inst,"vkCmdEndRenderPass");
    vkcerp(cb);
    // UAV to TRANSFER_SRC
    imb.srcStageMask=VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT; imb.srcAccessMask=VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    imb.dstStageMask=VK_PIPELINE_STAGE_2_TRANSFER_BIT; imb.dstAccessMask=VK_ACCESS_2_TRANSFER_READ_BIT;
    imb.oldLayout=VK_IMAGE_LAYOUT_GENERAL; imb.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkcpb2(cb,&depi);
    VkBufferImageCopy bic=(VkBufferImageCopy){0,0,0,(VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},{0,0,0},(VkExtent3D){64,64,1}};
    PFN_vkCmdCopyImageToBuffer vkccb=(PFN_vkCmdCopyImageToBuffer)gipa(inst,"vkCmdCopyImageToBuffer");
    vkccb(cb,uav,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,rb,1,&bic);
    vkecb(cb);
    VkSubmitInfo si={VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount=1; si.pCommandBuffers=&cb;
    vr = vkqs(q,1,&si,VK_NULL_HANDLE);
    vkdwi(dev);

    void* rdata; ((PFN_vkMapMemory)gipa(inst,"vkMapMemory"))(dev,rbMem,0,VK_WHOLE_SIZE,0,&rdata);
    uint32_t* ux = (uint32_t*)rdata;
    int ordered = 1, mism = 0, ones = 0, twos = 0, zeros = 0;
    for (int i = 0; i < 64*64; i++) {
        uint32_t v = ux[i*4];  // the id written
        if (v == 1) ones++;
        if (v == 2) twos++;
        if (v == 0) zeros++;
        if (v != 2) { ordered = 0; if (mism < 5) printf("pixel %d: value %u (expect 2)\n", i, v); mism++; }
    }
    printf("ones=%d twos=%d zeros=%d\n", ones, twos, zeros);

    printf("mismatches: %d\n", mism);
    printf("RESULT: %s\n", ordered ? "ROV INTERLOCK ORDERING WORKS" : "ROV ORDERING FAILED");
    return 0;
}
