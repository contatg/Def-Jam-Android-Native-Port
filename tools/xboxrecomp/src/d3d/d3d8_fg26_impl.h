/*
 * Experimental image-based optical-flow interpolation.
 * One producer (render owner), one presenter. Six immutable snapshots.
 * Queue submission is serialized by d3d8_fg26_queue.h; GPU work remains async.
 * Included ONCE by d3d8_vk.c, after Vulkan entry-point wrappers.
 */
#ifndef DEFJAM_FG26_IMPL_H
#define DEFJAM_FG26_IMPL_H
#include <time.h>
#include <errno.h>
#include <stdint.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include "d3d8_fg26_policy.h"
#include "d3d8_fg26_spirv.h"
#include "lsfg27/lsfg27.h"
#include "fg34/fg34.h"
#if defined(__ANDROID__)
#include "platform/android_thread_policy.h"
#endif
#define FG26_SLOTS 6
#define FG26_SWAP_MAX 8
typedef struct {VkImage image;VkDeviceMemory memory;VkImageView view;VkFramebuffer fb;int initialized;} FG26Image;
typedef struct {FG26Image gpu;int state;uint64_t stamp,serial;} FG26Slot;
typedef struct {
    VkInstance instance;VkPhysicalDevice physical;VkDevice device;VkQueue queue;
    PFN_vkGetInstanceProcAddr get_instance_proc;
    int lsfg27,fp16;unsigned multiplier,refresh;float flow_scale;unsigned gpu_budget_us;
    const char *shader_directory;
    uint32_t family,width,height,swap_count,swap_width,swap_height;
    VkSwapchainKHR swap;VkImage swap_images[FG26_SWAP_MAX];
    VkOffset3D rectangle[2];unsigned timestamp_bits;float timestamp_period;
} FG26Config;
typedef struct {
    FG26Config config;FG26Slot slots[FG26_SLOTS];FG26Image flow[2],output;
    void *lsfg;
    VkRenderPass flow_pass,output_pass;VkPipeline flow_pipeline,warp_pipeline;
    VkPipelineLayout layout;VkDescriptorSetLayout descriptor_layout;
    VkDescriptorPool descriptor_pool;VkDescriptorSet descriptors[3];VkSampler sampler;
    VkShaderModule vertex,flow_shader,warp_shader;
    VkCommandPool command_pool;VkCommandBuffer command;VkFence fence;
    VkSemaphore acquire,done[FG26_SWAP_MAX];VkQueryPool queries;
    pthread_t thread;int started,pending,pending_generated;double present_us;unsigned over_budget,gpu_samples;double gpu_average;_Atomic int stop,error;
    int budget_paused; unsigned budget_good_probes; uint64_t budget_next_probe;
    uint64_t serial;_Atomic uint64_t input_count,dropped_count;
} FG26Context;
static FG26Context fg26;
static int fg27_device_ready,fg27_fp16_enabled;
static int fg27_requested(void) {
    static int engine = -1;
    if (engine < 0) {
        const char *setting = getenv("DEFJAM_FRAMEGEN39");
        engine = setting && setting[0] == '2' ? 2 : setting && setting[0] == '1' ? 1 : 0;
    }
    return engine;
}
static int fg27_setting(const char *name,int def,int lo,int hi) {
    const char *s=getenv(name);if(!s||!*s)return def;
    char *end=NULL;long n=strtol(s,&end,10);return end&&!*end&&n>=lo&&n<=hi?(int)n:def;
}
/* Compatibility with the player's saved FG guards. Legacy generator retains
 * its original limits. Framegen stays off by default. */
static int fg27_guard_budget=1,fg27_guard_slow=1,fg27_budget_retry_s=45;
static unsigned fg27_warmup=60;
static void fg27_guards_init(int enabled) {
    fg27_guard_budget=1;
    fg27_warmup=(unsigned)fg27_setting("DEFJAM_LSFG27_WARMUP",60,0,600);
    fg27_budget_retry_s=fg27_setting("DEFJAM_LSFG27_BUDGET_RETRY",45,0,600);
    fg27_guard_slow=1;
    fg26_policy_max_dt=enabled&&!fg27_guard_slow?100000000ull:50000000ull;
    fg26_policy_max_delay=enabled&&!fg27_guard_slow?140000000ull:60000000ull;
    fprintf(stderr,"[LSFG27] guards budget=%d warmup=%u retry=%d slow=%d\n",fg27_guard_budget,fg27_warmup,fg27_budget_retry_s,fg27_guard_slow);
}
static pthread_mutex_t fg26_guard=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fg26_wake;
static pthread_once_t fg26_once=PTHREAD_ONCE_INIT;
static int fg26_wake_ok;
static _Atomic int fg26_active,fg26_paused,fg26_latched,fg30_slow_paused;
static _Atomic int fg26_real_milli,fg26_output_milli,fg26_generated_milli,fg26_gpu_us;
static uint64_t fg26_now(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;}
static void fg26_once_init(void){
    pthread_condattr_t a;
    if(!pthread_condattr_init(&a)){
        if(!pthread_condattr_setclock(&a,CLOCK_MONOTONIC)&&!pthread_cond_init(&fg26_wake,&a))fg26_wake_ok=1;
        pthread_condattr_destroy(&a);
    }
    atexit(fg26_stop);
}
void defjam_framegen26_stats(float out[4]) {
    out[0]=(float)atomic_load(&fg26_active);out[1]=atomic_load(&fg26_real_milli)/1000.f;
    out[2]=atomic_load(&fg26_output_milli)/1000.f;out[3]=atomic_load(&fg26_generated_milli)/1000.f;
}
int defjam_framegen26_ui(void){return atomic_load(&fg26_active)?(1<<20)|(1<<21)|2|((fg26.config.refresh?fg26.config.refresh:60)<<8):0;}
void defjam_framegen26_pause(int paused) {
    atomic_store(&fg26_paused,!!paused);pthread_once(&fg26_once,fg26_once_init);
    if(!fg26_wake_ok)return;
    pthread_mutex_lock(&fg26_guard);pthread_cond_broadcast(&fg26_wake);pthread_mutex_unlock(&fg26_guard);
}
static int fg26_memory(uint32_t bits,uint32_t *index) {
    VkPhysicalDeviceMemoryProperties p;vkGetPhysicalDeviceMemoryProperties(fg26.config.physical,&p);
    for(uint32_t i=0;i<p.memoryTypeCount;i++)if((bits&(1u<<i))&&(p.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)){*index=i;return 1;}
    return 0;
}
static int fg26_image(FG26Image *o,uint32_t w,uint32_t h,VkFormat format,VkImageUsageFlags usage) {
    VkImageCreateInfo ci={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=format,
        .extent={w,h,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,.usage=usage};
    VkMemoryRequirements r;VkMemoryAllocateInfo a={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    VkImageViewCreateInfo v={.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=format,
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}};
    if(vkCreateImage(fg26.config.device,&ci,NULL,&o->image)!=VK_SUCCESS)return 0;
    vkGetImageMemoryRequirements(fg26.config.device,o->image,&r);a.allocationSize=r.size;
    if(!fg26_memory(r.memoryTypeBits,&a.memoryTypeIndex)||vkAllocateMemory(fg26.config.device,&a,NULL,&o->memory)!=VK_SUCCESS)return 0;
    if(vkBindImageMemory(fg26.config.device,o->image,o->memory,0)!=VK_SUCCESS)return 0;
    v.image=o->image;return vkCreateImageView(fg26.config.device,&v,NULL,&o->view)==VK_SUCCESS;
}
static void fg26_barrier(VkCommandBuffer cb,VkImage image,VkImageLayout from,VkImageLayout to) {
    VkImageMemoryBarrier b={.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,.oldLayout=from,.newLayout=to,
        .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.image=image,
        .subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1},
        .srcAccessMask=from==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,NULL,0,NULL,1,&b);
}
static int fg26_pass(VkFormat format,VkImageLayout final,VkRenderPass *out) {
    VkAttachmentDescription a={.format=format,.samples=VK_SAMPLE_COUNT_1_BIT,.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .storeOp=VK_ATTACHMENT_STORE_OP_STORE,.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,.finalLayout=final};
    VkAttachmentReference ref={0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub={.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.colorAttachmentCount=1,.pColorAttachments=&ref};
    VkSubpassDependency dep[2]={
        {.srcSubpass=VK_SUBPASS_EXTERNAL,.dstSubpass=0,.srcStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
         .dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,.srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT},
        {.srcSubpass=0,.dstSubpass=VK_SUBPASS_EXTERNAL,.srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
         .dstStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT}};
    VkRenderPassCreateInfo ci={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=1,.pAttachments=&a,
        .subpassCount=1,.pSubpasses=&sub,.dependencyCount=2,.pDependencies=dep};
    return vkCreateRenderPass(fg26.config.device,&ci,NULL,out)==VK_SUCCESS;
}
static int fg26_framebuffer(FG26Image *o,VkRenderPass pass,uint32_t w,uint32_t h) {
    VkFramebufferCreateInfo ci={.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=pass,.attachmentCount=1,
        .pAttachments=&o->view,.width=w,.height=h,.layers=1};
    return vkCreateFramebuffer(fg26.config.device,&ci,NULL,&o->fb)==VK_SUCCESS;
}
static int fg26_shader(const uint32_t *code,size_t size,VkShaderModule *out) {
    VkShaderModuleCreateInfo ci={.sType=VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,.codeSize=size,.pCode=code};
    return vkCreateShaderModule(fg26.config.device,&ci,NULL,out)==VK_SUCCESS;
}
static int fg26_pipeline(VkShaderModule fragment,VkRenderPass pass,VkPipeline *out) {
    VkPipelineShaderStageCreateInfo stages[2]={
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_VERTEX_BIT,.module=fg26.vertex,.pName="main"},
        {.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,.stage=VK_SHADER_STAGE_FRAGMENT_BIT,.module=fragment,.pName="main"}};
    VkPipelineVertexInputStateCreateInfo vi={.sType=VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia={.sType=VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp={.sType=VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,.viewportCount=1,.scissorCount=1};
    VkPipelineRasterizationStateCreateInfo rs={.sType=VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode=VK_POLYGON_MODE_FILL,.cullMode=VK_CULL_MODE_NONE,.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE,.lineWidth=1.f};
    VkPipelineMultisampleStateCreateInfo ms={.sType=VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState blend={.colorWriteMask=0xf};
    VkPipelineColorBlendStateCreateInfo cb={.sType=VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,.attachmentCount=1,.pAttachments=&blend};
    VkDynamicState dynamic[2]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy={.sType=VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,.dynamicStateCount=2,.pDynamicStates=dynamic};
    VkGraphicsPipelineCreateInfo ci={.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,.stageCount=2,.pStages=stages,
        .pVertexInputState=&vi,.pInputAssemblyState=&ia,.pViewportState=&vp,.pRasterizationState=&rs,
        .pMultisampleState=&ms,.pColorBlendState=&cb,.pDynamicState=&dy,.layout=fg26.layout,.renderPass=pass};
    return vkCreateGraphicsPipelines(fg26.config.device,VK_NULL_HANDLE,1,&ci,NULL,out)==VK_SUCCESS;
}
static int fg26_resources(void) {
    VkDevice d=fg26.config.device;uint32_t w=fg26.config.width,h=fg26.config.height,fw=(w+15)/16,fh=(h+15)/16;
    VkFormatProperties props;vkGetPhysicalDeviceFormatProperties(fg26.config.physical,VK_FORMAT_R16G16B16A16_SFLOAT,&props);
    VkFormatFeatureFlags need=(fg26.config.lsfg27?VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT:VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if((props.optimalTilingFeatures&need)!=need)return 0;
    vkGetPhysicalDeviceFormatProperties(fg26.config.physical,VK_FORMAT_R8G8B8A8_UNORM,&props);
    need|=VK_FORMAT_FEATURE_BLIT_DST_BIT|VK_FORMAT_FEATURE_BLIT_SRC_BIT;
    if((props.optimalTilingFeatures&need)!=need)return 0;
    for(unsigned i=0;i<FG26_SLOTS;i++)if(!fg26_image(&fg26.slots[i].gpu,w,h,VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_SAMPLED_BIT))return 0;
    if(!fg26.config.lsfg27) {
    if(!fg26_pass(VK_FORMAT_R16G16B16A16_SFLOAT,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,&fg26.flow_pass)||
       !fg26_pass(VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,&fg26.output_pass))return 0;
    for(unsigned i=0;i<2;i++)if(!fg26_image(&fg26.flow[i],fw,fh,VK_FORMAT_R16G16B16A16_SFLOAT,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT)||
       !fg26_framebuffer(&fg26.flow[i],fg26.flow_pass,fw,fh))return 0;
    if(!fg26_image(&fg26.output,w,h,VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT)||
       !fg26_framebuffer(&fg26.output,fg26.output_pass,w,h))return 0;
    VkSamplerCreateInfo sampler={.sType=VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,.magFilter=VK_FILTER_LINEAR,.minFilter=VK_FILTER_LINEAR,
        .mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST,.addressModeU=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,.maxLod=0.f};
    if(vkCreateSampler(d,&sampler,NULL,&fg26.sampler)!=VK_SUCCESS)return 0;
    VkDescriptorSetLayoutBinding bindings[4]={0};
    for(unsigned i=0;i<4;i++){bindings[i].binding=i;bindings[i].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount=1;bindings[i].stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;}
    VkDescriptorSetLayoutCreateInfo dl={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,.bindingCount=4,.pBindings=bindings};
    if(vkCreateDescriptorSetLayout(d,&dl,NULL,&fg26.descriptor_layout)!=VK_SUCCESS)return 0;
    VkPushConstantRange push={VK_SHADER_STAGE_FRAGMENT_BIT,0,16};
    VkPipelineLayoutCreateInfo pl={.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,.setLayoutCount=1,
        .pSetLayouts=&fg26.descriptor_layout,.pushConstantRangeCount=1,.pPushConstantRanges=&push};
    if(vkCreatePipelineLayout(d,&pl,NULL,&fg26.layout)!=VK_SUCCESS)return 0;
    VkDescriptorPoolSize size={VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,12};
    VkDescriptorPoolCreateInfo pool={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,.maxSets=3,.poolSizeCount=1,.pPoolSizes=&size};
    if(vkCreateDescriptorPool(d,&pool,NULL,&fg26.descriptor_pool)!=VK_SUCCESS)return 0;
    VkDescriptorSetLayout layouts[3]={fg26.descriptor_layout,fg26.descriptor_layout,fg26.descriptor_layout};
    VkDescriptorSetAllocateInfo ai={.sType=VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,.descriptorPool=fg26.descriptor_pool,.descriptorSetCount=3,.pSetLayouts=layouts};
    if(vkAllocateDescriptorSets(d,&ai,fg26.descriptors)!=VK_SUCCESS)return 0;
    if(!fg26_shader(fg26_fullscreen_spv,sizeof fg26_fullscreen_spv,&fg26.vertex)||
       !fg26_shader(fg26_flow_spv,sizeof fg26_flow_spv,&fg26.flow_shader)||
       !fg26_shader(fg26_warp_spv,sizeof fg26_warp_spv,&fg26.warp_shader)||
       !fg26_pipeline(fg26.flow_shader,fg26.flow_pass,&fg26.flow_pipeline)||
       !fg26_pipeline(fg26.warp_shader,fg26.output_pass,&fg26.warp_pipeline))return 0;
    }
    VkCommandPoolCreateInfo cp={.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,.queueFamilyIndex=fg26.config.family};
    if(vkCreateCommandPool(d,&cp,NULL,&fg26.command_pool)!=VK_SUCCESS)return 0;
    VkCommandBufferAllocateInfo cmd={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=fg26.command_pool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1};
    if(vkAllocateCommandBuffers(d,&cmd,&fg26.command)!=VK_SUCCESS)return 0;
    VkFenceCreateInfo fence={.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,.flags=VK_FENCE_CREATE_SIGNALED_BIT};
    VkSemaphoreCreateInfo sem={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if(vkCreateFence(d,&fence,NULL,&fg26.fence)!=VK_SUCCESS||vkCreateSemaphore(d,&sem,NULL,&fg26.acquire)!=VK_SUCCESS)return 0;
    for(uint32_t i=0;i<fg26.config.swap_count;i++)if(vkCreateSemaphore(d,&sem,NULL,&fg26.done[i])!=VK_SUCCESS)return 0;
    if(fg26.config.timestamp_bits) {
        VkQueryPoolCreateInfo q={.sType=VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,.queryType=VK_QUERY_TYPE_TIMESTAMP,.queryCount=4};
        if(vkCreateQueryPool(d,&q,NULL,&fg26.queries)!=VK_SUCCESS)return 0;
    }
    if(fg26.config.lsfg27) {
        vkGetPhysicalDeviceFormatProperties(fg26.config.physical,VK_FORMAT_R8_UNORM,&props);
        need=VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        if((props.optimalTilingFeatures&need)!=need){fprintf(stderr,"[LSFG27] R8_UNORM lacks storage, sampling or linear filter support\n");return 0;}
        if(fg26.config.lsfg27==1){
        vkGetPhysicalDeviceFormatProperties(fg26.config.physical,VK_FORMAT_R32_SFLOAT,&props);
        if((props.optimalTilingFeatures&need)!=need){
            fprintf(stderr,"[FG34] R32_SFLOAT lacks storage, sampling or linear filter support\n");return 0;
        }
        FG34Info info={.abi=2,.size=sizeof info,.get_instance_proc=fg26.config.get_instance_proc,
            .instance=fg26.config.instance,.physical=fg26.config.physical,.device=d,.queue=fg26.config.queue,
            .command=fg26.command,.family=fg26.config.family,.width=w,.height=h,.flow_scale=fg26.config.flow_scale,
            .shader_directory=fg26.config.shader_directory?fg26.config.shader_directory:"/storage/emulated/0/DefJamFFNY/lsfg27",.fp16=fg26.config.fp16,.multiplier=fg26.config.multiplier};
        char error[384]={0};fg26.lsfg=defjam_fg34_create(&info,error,sizeof error);
        if(!fg26.lsfg){fprintf(stderr,"[FG34] initialize failed: %s; real presentation retained\n",error);return 0;}
        }else{
        LSFG27Info info={.abi=2,.size=sizeof info,.get_instance_proc=fg26.config.get_instance_proc,
            .instance=fg26.config.instance,.physical=fg26.config.physical,.device=d,.queue=fg26.config.queue,
            .command=fg26.command,.family=fg26.config.family,.width=w,.height=h,.flow_scale=fg26.config.flow_scale,
            .shader_directory=fg26.config.shader_directory?fg26.config.shader_directory:"/storage/emulated/0/DefJamFFNY/lsfg27",.fp16=fg26.config.fp16,.multiplier=fg26.config.multiplier};
        char error[384]={0};fg26.lsfg=defjam_lsfg27_create(&info,error,sizeof error);
        if(!fg26.lsfg){fprintf(stderr,"[LSFG27] initialize failed: %s; real presentation retained\n",error);return 0;}
        }
    }
    return 1;
}
static void fg26_descriptor(unsigned set,int left,int right) {
    VkDescriptorImageInfo images[4]={
        {fg26.sampler,fg26.slots[left].gpu.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {fg26.sampler,fg26.slots[right].gpu.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {fg26.sampler,fg26.flow[0].view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {fg26.sampler,fg26.flow[1].view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet writes[4]={0};unsigned n=set==2?4:2;
    for(unsigned i=0;i<n;i++){writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;writes[i].dstSet=fg26.descriptors[set];
        writes[i].dstBinding=i;writes[i].descriptorCount=1;writes[i].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;writes[i].pImageInfo=&images[i];}
    vkUpdateDescriptorSets(fg26.config.device,n,writes,0,NULL);
}
static void fg26_draw(FG26Image *target,VkRenderPass pass,VkPipeline pipe,unsigned set,uint32_t w,uint32_t h,float alpha) {
    VkRenderPassBeginInfo begin={.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=pass,.framebuffer=target->fb,.renderArea={{0,0},{w,h}}};
    VkViewport vp={0.f,0.f,(float)w,(float)h,0.f,1.f};VkRect2D sc={{0,0},{w,h}};
    float params[4]={(float)fg26.config.width,(float)fg26.config.height,alpha,0.f};
    vkCmdBeginRenderPass(fg26.command,&begin,VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(fg26.command,0,1,&vp);vkCmdSetScissor(fg26.command,0,1,&sc);
    vkCmdBindPipeline(fg26.command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipe);
    vkCmdBindDescriptorSets(fg26.command,VK_PIPELINE_BIND_POINT_GRAPHICS,fg26.layout,0,1,&fg26.descriptors[set],0,NULL);
    vkCmdPushConstants(fg26.command,fg26.layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof params,params);
    vkCmdDraw(fg26.command,3,1,0,0);vkCmdEndRenderPass(fg26.command);
}
static int fg26_wait_gpu(void) {
    if(!fg26.pending)return 1;
    VkResult r;
    do {r=vkWaitForFences(fg26.config.device,1,&fg26.fence,VK_TRUE,100000000ull);}
    while(r==VK_TIMEOUT&&!atomic_load(&fg26.stop));
    if(r!=VK_SUCCESS){atomic_store(&fg26.error,r==VK_TIMEOUT?VK_TIMEOUT:r);return 0;}
    fg26.pending=0;
    if(fg26.queries){
        uint64_t ticks[4];VkResult q=vkGetQueryPoolResults(fg26.config.device,fg26.queries,0,4,sizeof ticks,ticks,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT);
        if(q!=VK_SUCCESS){fprintf(stderr,"[FG26] GPU timestamp unavailable after fence=%d; returning to original renderer\n",q);atomic_store(&fg26.error,q==VK_NOT_READY?VK_ERROR_UNKNOWN:q);return 0;}
        if(q==VK_SUCCESS){uint64_t mask=fg26.config.timestamp_bits>=64?UINT64_MAX:(1ull<<fg26.config.timestamp_bits)-1;
            fg26.present_us=((ticks[3]-ticks[0])&mask)*(double)fg26.config.timestamp_period/1000.0;
            if(!fg26.pending_generated)return 1;
            uint64_t delta=(ticks[2]-ticks[1])&mask;double us=delta*(double)fg26.config.timestamp_period/1000.0;
            atomic_store(&fg26_gpu_us,us<1000000.0?(int)us:-1);
            if(fg26.budget_paused){
                double budget=fg26.config.gpu_budget_us?fg26.config.gpu_budget_us:4000u;
                fg26.budget_good_probes=us<budget*.85?fg26.budget_good_probes+1:0;
                fprintf(stderr,"[LSFG27] budget probe=%.3fms limit=%.3fms good=%u/3\n",us/1000.,budget/1000.,fg26.budget_good_probes);
                if(fg26.budget_good_probes>=3){
                    fg26.budget_paused=0;fg26.over_budget=0;fg26.gpu_samples=5+fg27_warmup;fg26.gpu_average=us;
                    fprintf(stderr,"[LSFG27] generation resumed after three affordable probes\n");
                }else fg26.budget_next_probe=fg26_now()+(uint64_t)(us>=budget*.85?fg27_budget_retry_s:3)*1000000000ull;
                return 1;
            }
            if(++fg26.gpu_samples>4+(fg26.config.lsfg27?fg27_warmup:0)){
                fg26.gpu_average=fg26.gpu_samples==5+(fg26.config.lsfg27?fg27_warmup:0)?us:fg26.gpu_average*.875+us*.125;
                fg26.over_budget=fg26.gpu_average>(fg26.config.gpu_budget_us?fg26.config.gpu_budget_us:4000u)?fg26.over_budget+1:0;
                if(fg26.over_budget>=12&&(!fg26.config.lsfg27||fg27_guard_budget)){
                    if(fg26.config.lsfg27&&fg27_budget_retry_s>0){
                        fg26.budget_paused=1;fg26.budget_good_probes=0;fg26.budget_next_probe=fg26_now()+(uint64_t)fg27_budget_retry_s*1000000000ull;
                        fprintf(stderr,"[LSFG27] generation paused: mean=%.3fms exceeds %.3fms; game frames continue; retry scheduled\n",fg26.gpu_average/1000.,(fg26.config.gpu_budget_us?fg26.config.gpu_budget_us:4000u)/1000.);
                    }else{fprintf(stderr,"[FG32] legacy generator auto-off over budget\n");atomic_store(&fg26.error,VK_ERROR_UNKNOWN);return 0;}
                }
            }}
    }
    return 1;
}
static VkResult fg26_present(int left,int right,float alpha,int *did_interpolate,uint64_t *pair_a,uint64_t *pair_b) {
    VkDevice d=fg26.config.device;uint32_t image=0;
    int backend_failed=0;
    VkResult r=vkAcquireNextImageKHR(d,fg26.config.swap,2000000ull,fg26.acquire,VK_NULL_HANDLE,&image);
    if(r!=VK_SUCCESS&&r!=VK_SUBOPTIMAL_KHR)return r;
    VkCommandBufferBeginInfo begin={.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    if(vkResetCommandPool(d,fg26.command_pool,0)!=VK_SUCCESS||vkBeginCommandBuffer(fg26.command,&begin)!=VK_SUCCESS)return VK_ERROR_DEVICE_LOST;
    if(fg26.queries){vkCmdResetQueryPool(fg26.command,fg26.queries,0,4);vkCmdWriteTimestamp(fg26.command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,fg26.queries,0);}
    /* TRANSFER is also the acquire semaphore wait stage. Do not count
     * that WSI wait as interpolation work. Only fresh generated phases
     * contribute to the generator budget after the submission completes. */
    if(fg26.queries)vkCmdWriteTimestamp(fg26.command,VK_PIPELINE_STAGE_TRANSFER_BIT,fg26.queries,1);
    *did_interpolate=alpha>=0.f?3:0;
    VkImage source;
    if(*did_interpolate&&fg26.config.lsfg27) {
        int unique=0;char error[384]={0};
        int (*record)(void *,VkImage,VkImage,uint64_t,uint64_t,unsigned,VkImage *,int *,char *,size_t) =
            fg26.config.lsfg27==1 ? defjam_fg34_record : defjam_lsfg27_record;
        if(!record(fg26.lsfg,fg26.slots[left].gpu.image,fg26.slots[right].gpu.image,
            fg26.slots[left].serial,fg26.slots[right].serial,(unsigned)(alpha*fg26.config.multiplier+.5f),&source,&unique,error,sizeof error)) {
            fprintf(stderr,"[LSFG27] record failed: %s; presenting acquired image with real frame before fallback\n",error);
            /* Discard the incomplete recording, but return the acquired WSI
             * image through a normal real-frame present before stopping. */
            if(vkResetCommandPool(d,fg26.command_pool,0)!=VK_SUCCESS||vkBeginCommandBuffer(fg26.command,&begin)!=VK_SUCCESS)return VK_ERROR_DEVICE_LOST;
            if(fg26.queries){vkCmdResetQueryPool(fg26.command,fg26.queries,0,4);vkCmdWriteTimestamp(fg26.command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,fg26.queries,0);vkCmdWriteTimestamp(fg26.command,VK_PIPELINE_STAGE_TRANSFER_BIT,fg26.queries,1);}
            backend_failed=1;*did_interpolate=0;source=fg26.slots[right].gpu.image;
            fg26_barrier(fg26.command,source,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        }else{
            *did_interpolate=unique?3:1;
        }
    }else if(*did_interpolate){
        if(*pair_a!=fg26.slots[left].serial||*pair_b!=fg26.slots[right].serial){
            fg26_descriptor(0,left,right);fg26_descriptor(1,right,left);
            fg26_draw(&fg26.flow[0],fg26.flow_pass,fg26.flow_pipeline,0,(fg26.config.width+15)/16,(fg26.config.height+15)/16,0.f);
            fg26_draw(&fg26.flow[1],fg26.flow_pass,fg26.flow_pipeline,1,(fg26.config.width+15)/16,(fg26.config.height+15)/16,0.f);
            *pair_a=fg26.slots[left].serial;*pair_b=fg26.slots[right].serial;
        }
        fg26_descriptor(2,left,right);
        fg26_draw(&fg26.output,fg26.output_pass,fg26.warp_pipeline,2,fg26.config.width,fg26.config.height,alpha);
        source=fg26.output.image;
    }else {
        source=fg26.slots[right].gpu.image;
        fg26_barrier(fg26.command,source,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    }
    if(fg26.queries)vkCmdWriteTimestamp(fg26.command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,fg26.queries,2);
    fg26.pending_generated=*did_interpolate==3;
    fg26_barrier(fg26.command,fg26.config.swap_images[image],VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageSubresourceRange range={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};VkClearColorValue black={{0.f,0.f,0.f,1.f}};
    vkCmdClearColorImage(fg26.command,fg26.config.swap_images[image],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&black,1,&range);
    fg26_barrier(fg26.command,fg26.config.swap_images[image],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit blit={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.srcOffsets={{0,0,0},{(int32_t)fg26.config.width,(int32_t)fg26.config.height,1}},
        .dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}};
    memcpy(blit.dstOffsets,fg26.config.rectangle,sizeof blit.dstOffsets);
    vkCmdBlitImage(fg26.command,source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,fg26.config.swap_images[image],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,VK_FILTER_LINEAR);
    if(!*did_interpolate)fg26_barrier(fg26.command,source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    fg26_barrier(fg26.command,fg26.config.swap_images[image],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    if(fg26.queries)vkCmdWriteTimestamp(fg26.command,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,fg26.queries,3);
    if(vkEndCommandBuffer(fg26.command)!=VK_SUCCESS||vkResetFences(d,1,&fg26.fence)!=VK_SUCCESS)return VK_ERROR_DEVICE_LOST;
    VkPipelineStageFlags wait=VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.waitSemaphoreCount=1,.pWaitSemaphores=&fg26.acquire,.pWaitDstStageMask=&wait,
        .commandBufferCount=1,.pCommandBuffers=&fg26.command,.signalSemaphoreCount=1,.pSignalSemaphores=&fg26.done[image]};
    r=vkQueueSubmit(fg26.config.queue,1,&submit,fg26.fence);
    if(r!=VK_SUCCESS)return r;
    fg26.pending=1;
    VkPresentInfoKHR present={.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,.waitSemaphoreCount=1,.pWaitSemaphores=&fg26.done[image],
        .swapchainCount=1,.pSwapchains=&fg26.config.swap,.pImageIndices=&image};
    r=vkQueuePresentKHR(fg26.config.queue,&present);
    return backend_failed&&(r==VK_SUCCESS||r==VK_SUBOPTIMAL_KHR)?VK_ERROR_UNKNOWN:r;
}
static void *fg26_run(void *unused) {
    (void)unused;
#if defined(__ANDROID__)
    /* Use the existing host-task budget, not a pinned render CPU inherited at creation. */
    defjam_android_native_thread_enter("framegen",__func__,0);
#endif
#if defined(__linux__)
    pthread_setname_np(pthread_self(),"dj-framegen");
#endif
    int timeline[FG26_SLOTS],n=0;FG26Timeline policy={0};
    uint64_t deadline=0,report=fg26_now(),outputs=0,generated=0,repeated=0,last_serial=0,pair_a=0,pair_b=0;
    uint64_t step=1000000000ull/(fg26.config.refresh?fg26.config.refresh:60);
    while(!atomic_load(&fg26.stop)) {
        if(!fg26_wait_gpu())break;
        pthread_mutex_lock(&fg26_guard);
        if(atomic_load(&fg26_paused)) {
            policy.stable=0;deadline=0;
            while(!atomic_load(&fg26.stop)&&atomic_load(&fg26_paused))pthread_cond_wait(&fg26_wake,&fg26_guard);
        }
        if(atomic_load(&fg26.stop)){pthread_mutex_unlock(&fg26_guard);break;}
        /* READY snapshots have already been submitted to the same queue. */
        for(;;){
            int next=-1;
            for(unsigned i=0;i<FG26_SLOTS;i++)if(fg26.slots[i].state==2&&(next<0||fg26.slots[i].serial<fg26.slots[next].serial))next=(int)i;
            if(next<0||n==FG26_SLOTS)break;
            fg26.slots[next].state=3;timeline[n++]=next;fg26_timeline_note(&policy,fg26.slots[next].stamp);
        }
        uint64_t now=fg26_now();
        if(fg26.config.lsfg27&&policy.n) {
            uint64_t mean=0;for(unsigned k=0;k<policy.n;k++)mean+=policy.intervals[k];mean/=policy.n;
            uint64_t wanted=mean/fg26.config.multiplier,ceiling=1000000000ull/fg26.config.refresh;
            step=wanted>ceiling?wanted:ceiling;
        }
        if(!n||now>fg26.slots[timeline[n-1]].stamp+250000000ull){
            /* No new game frames: sleep, do not redraw a paused/stalled game forever. */
            deadline=0;policy.stable=0;
            while(n>1){fg26.slots[timeline[0]].state=0;memmove(timeline,timeline+1,(size_t)(--n)*sizeof *timeline);}
            pthread_cond_wait(&fg26_wake,&fg26_guard);pthread_mutex_unlock(&fg26_guard);continue;
        }
        if(deadline&&now<deadline){
            struct timespec until={(time_t)(deadline/1000000000ull),(long)(deadline%1000000000ull)};
            pthread_cond_timedwait(&fg26_wake,&fg26_guard,&until);pthread_mutex_unlock(&fg26_guard);continue;
        }
        if(!deadline||now>deadline+step*2)deadline=now;
        deadline+=step;
        uint64_t target=now>policy.delay_ns?now-policy.delay_ns:0;
        while(n>2&&fg26.slots[timeline[1]].stamp<=target){
            fg26.slots[timeline[0]].state=0;memmove(timeline,timeline+1,(size_t)(--n)*sizeof *timeline);
        }
        int left=timeline[0],right=left;
        for(int i=1;i<n;i++)if(fg26.slots[timeline[i]].stamp>=target){left=timeline[i-1];right=timeline[i];break;}
        if(target>=fg26.slots[timeline[n-1]].stamp)left=right=timeline[n-1];
        float alpha=left!=right?fg26_alpha(fg26.slots[left].stamp,fg26.slots[right].stamp,target,fg26.config.lsfg27&&policy.n>=2?4:policy.stable):-1.f;
        if(alpha<0.f&&left!=right&&(target<=fg26.slots[left].stamp||target-fg26.slots[left].stamp<(fg26.slots[right].stamp-fg26.slots[left].stamp)/2))right=left;
        if(fg26.config.lsfg27&&alpha>=0.f) {
            unsigned multiplier=fg26.config.multiplier;
            unsigned phase=(unsigned)(alpha*multiplier+.5f);
            if(!phase){right=left;alpha=-1.f;}
            else if(phase>=multiplier){left=right;alpha=-1.f;}
            else alpha=(float)phase/multiplier;
        }
        if(fg26.budget_paused&&now<fg26.budget_next_probe&&alpha>=0.f){
            if(alpha<.5f)right=left;
            alpha=-1.f;
        }
        uint64_t serial=fg26.slots[right].serial;
        pthread_mutex_unlock(&fg26_guard);
        if(policy.slow>=10&&(!fg26.config.lsfg27||fg27_guard_slow)){
            if(fg26.config.lsfg27){fprintf(stderr,"[FG30] suspended: source below20FPS for10 intervals; retry after recovery above24FPS for3s\n");atomic_store(&fg30_slow_paused,1);atomic_store(&fg26.error,VK_NOT_READY);}
            else {fprintf(stderr,"[FG26] auto-off: source below20FPS for10 intervals; real renderer restored\n");atomic_store(&fg26.error,VK_ERROR_UNKNOWN);}
            break;
        }
        int interpolated=0;VkResult result=fg26_present(left,right,alpha,&interpolated,&pair_a,&pair_b);
        if(result==VK_SUCCESS||result==VK_SUBOPTIMAL_KHR){outputs++;generated+=(interpolated>>1)&1;repeated+=interpolated==1||(!interpolated&&serial==last_serial);last_serial=serial;}
        else if(result!=VK_TIMEOUT&&result!=VK_NOT_READY){atomic_store(&fg26.error,result);break;}
        int gpu=atomic_load(&fg26_gpu_us);
        now=fg26_now();
        if(now-report>=2000000000ull){
            fprintf(stderr,"[LSFG27] generated_gpu_last_ms=%.3f generated_gpu_ewma_ms=%.3f generated_samples=%u presentation_elapsed_ms=%.3f budget_paused=%d; presentation includes WSI waits/copy and does not charge the generator budget\n",gpu<0?-1.:gpu/1000.,fg26.gpu_average/1000.,fg26.gpu_samples,fg26.present_us/1000.,fg26.budget_paused);
            double seconds=(now-report)/1e9;uint64_t inputs=atomic_exchange(&fg26.input_count,0),dropped=atomic_exchange(&fg26.dropped_count,0);
            atomic_store(&fg26_real_milli,(int)(inputs*1000.0/seconds));atomic_store(&fg26_output_milli,(int)(outputs*1000.0/seconds));atomic_store(&fg26_generated_milli,(int)(generated*1000.0/seconds));
            fprintf(stderr,"[FG26] real_source_fps=%.2f accepted_output_fps=%.2f generated_unique_fps=%.2f repeated_images=%"PRIu64" capture_drops=%"PRIu64" gpu_last_ms=%.3f gpu_ewma_ms=%.3f delay_ms=%.2f stable=%u; output is queue acceptance, not verified scanout; no extra simulation steps\n",
                inputs/seconds,outputs/seconds,generated/seconds,repeated,dropped,gpu<0?-1.:gpu/1000.,fg26.queries?fg26.gpu_average/1000.:-1.,policy.delay_ns/1e6,policy.stable);
            report=now;outputs=generated=repeated=0;
        }
    }
    return NULL;
}
static void fg26_image_free(FG26Image *o) {
    VkDevice d=fg26.config.device;
    if(o->fb)vkDestroyFramebuffer(d,o->fb,NULL);
    if(o->view)vkDestroyImageView(d,o->view,NULL);
    if(o->image)vkDestroyImage(d,o->image,NULL);
    if(o->memory)vkFreeMemory(d,o->memory,NULL);
}
static void fg26_stop(void) {
    if(!fg26.config.device)return;
    atomic_store(&fg26.stop,1);pthread_once(&fg26_once,fg26_once_init);
    if(fg26_wake_ok){pthread_mutex_lock(&fg26_guard);pthread_cond_broadcast(&fg26_wake);pthread_mutex_unlock(&fg26_guard);}
    if(fg26.started)pthread_join(fg26.thread,NULL);
    /* Also covers producer copies and WSI's pending semaphore waits. */
    vkDeviceWaitIdle(fg26.config.device);
    VkDevice d=fg26.config.device;
    if(fg26.lsfg){
        if(fg26.config.lsfg27==1)defjam_fg34_destroy(fg26.lsfg);
        else defjam_lsfg27_destroy(fg26.lsfg);
        fg26.lsfg=NULL;
    }
    for(unsigned i=0;i<FG26_SLOTS;i++)fg26_image_free(&fg26.slots[i].gpu);
    for(unsigned i=0;i<2;i++)fg26_image_free(&fg26.flow[i]);
    fg26_image_free(&fg26.output);
    if(fg26.flow_pipeline)vkDestroyPipeline(d,fg26.flow_pipeline,NULL);
    if(fg26.warp_pipeline)vkDestroyPipeline(d,fg26.warp_pipeline,NULL);
    if(fg26.descriptor_pool)vkDestroyDescriptorPool(d,fg26.descriptor_pool,NULL);
    if(fg26.layout)vkDestroyPipelineLayout(d,fg26.layout,NULL);
    if(fg26.descriptor_layout)vkDestroyDescriptorSetLayout(d,fg26.descriptor_layout,NULL);
    if(fg26.flow_pass)vkDestroyRenderPass(d,fg26.flow_pass,NULL);
    if(fg26.output_pass)vkDestroyRenderPass(d,fg26.output_pass,NULL);
    if(fg26.sampler)vkDestroySampler(d,fg26.sampler,NULL);
    if(fg26.vertex)vkDestroyShaderModule(d,fg26.vertex,NULL);
    if(fg26.flow_shader)vkDestroyShaderModule(d,fg26.flow_shader,NULL);
    if(fg26.warp_shader)vkDestroyShaderModule(d,fg26.warp_shader,NULL);
    if(fg26.command_pool)vkDestroyCommandPool(d,fg26.command_pool,NULL);
    if(fg26.fence)vkDestroyFence(d,fg26.fence,NULL);
    if(fg26.acquire)vkDestroySemaphore(d,fg26.acquire,NULL);
    for(unsigned i=0;i<FG26_SWAP_MAX;i++)if(fg26.done[i])vkDestroySemaphore(d,fg26.done[i],NULL);
    if(fg26.queries)vkDestroyQueryPool(d,fg26.queries,NULL);
    atomic_store_explicit(&fg26_active,0,memory_order_release);
    atomic_store_explicit(&fg26_queue_shared,0,memory_order_release);
    if(fg26.started)fprintf(stderr,"[FG26] stopped; private presentation resources released, original renderer retained\n");
    memset(&fg26,0,sizeof fg26);
}
static int fg26_start(const FG26Config *config) {
    if(atomic_load(&fg26_latched))return 0;
    if(atomic_load(&fg26_active))return !atomic_load(&fg26.error);
    if(!config->swap||config->swap_count>FG26_SWAP_MAX||!config->width||!config->height||config->width>1280||config->height>960){
        fprintf(stderr,"[FG26] unavailable dimensions=%ux%u swap_images=%u; bounded experiment supports up to1280x960 and8 images\n",config->width,config->height,config->swap_count);
        atomic_store(&fg26_latched,1);return 0;
    }
    pthread_once(&fg26_once,fg26_once_init);
    if(!fg26_wake_ok){fprintf(stderr,"[FG26] monotonic condition variable unavailable; original presentation retained\n");atomic_store(&fg26_latched,1);return 0;}
    fg26.config=*config;
    fg27_guards_init(config->lsfg27);
    if(!fg26_resources()){fprintf(stderr,"[FG26] resources/features unavailable; original presentation retained\n");atomic_store(&fg26_latched,1);fg26_stop();return 0;}
    atomic_store(&fg26_gpu_us,-1);atomic_store(&fg26_real_milli,0);atomic_store(&fg26_output_milli,0);atomic_store(&fg26_generated_milli,0);
    atomic_store_explicit(&fg26_queue_shared,1,memory_order_release);
    if(pthread_create(&fg26.thread,NULL,fg26_run,NULL)){atomic_store(&fg26_latched,1);fg26_stop();return 0;}
    fg26.started=1;atomic_store_explicit(&fg26_active,1,memory_order_release);
    if(config->lsfg27)fprintf(stderr,"[FG39] enabled selected backend x2/x3/x4 native snapshots=6 internal=%ux%u flow_scale=%.2f precision=%s gpu_budget_ms=%.3f output_request=configured; same Turnip/device/queue, no guest steps; extra video buffering20-60ms\n",
        config->width,config->height,config->flow_scale,config->fp16?"FP16":"FP32",config->gpu_budget_us/1000.);
    else fprintf(stderr,"[FG26] enabled optical-flow experiment, snapshots=6 internal=%ux%u flow=%ux%u output_request=60Hz; auto delay20-60ms, no guest clock/audio/control edits, HUD within game may distort\n",config->width,config->height,(config->width+15)/16,(config->height+15)/16);
    return 1;
}
static int fg26_reserve(void) {
    pthread_mutex_lock(&fg26_guard);int found=-1;
    for(unsigned i=0;i<FG26_SLOTS;i++)if(!fg26.slots[i].state){fg26.slots[i].state=1;found=(int)i;break;}
    pthread_mutex_unlock(&fg26_guard);return found;
}
static void fg26_capture(VkCommandBuffer command,int index,VkImage from,uint32_t w,uint32_t h) {
    if(index<0)return;
    FG26Image *o=&fg26.slots[index].gpu;
    fg26_barrier(command,o->image,o->initialized?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit blit={.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.srcOffsets={{0,0,0},{(int32_t)w,(int32_t)h,1}},
        .dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1},.dstOffsets={{0,0,0},{(int32_t)fg26.config.width,(int32_t)fg26.config.height,1}}};
    vkCmdBlitImage(command,from,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,o->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,VK_FILTER_LINEAR);
    fg26_barrier(command,o->image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);o->initialized=1;
}
static void fg26_publish(int index,int submitted) {
    if(submitted)atomic_fetch_add(&fg26.input_count,1);
    pthread_mutex_lock(&fg26_guard);
    if(index>=0) {
        fg26.slots[index].stamp=fg26_now();fg26.slots[index].serial=++fg26.serial;
        fg26.slots[index].state=submitted?2:0;
    }else if(submitted)atomic_fetch_add(&fg26.dropped_count,1);
    pthread_cond_signal(&fg26_wake);pthread_mutex_unlock(&fg26_guard);
}
#endif
