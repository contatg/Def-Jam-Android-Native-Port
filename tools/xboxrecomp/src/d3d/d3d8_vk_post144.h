/* Presentation only. AMD FSR1 EASU + RCAS; optional MostWanted-inspired filters.
 * One Vulkan owner thread. No guest memory, simulation or audio changes. */
#include "d3d8_fsr144_shaders.h"
#include "d3d8_spatial34_shader.h"

typedef struct {
    VkImage img; VkDeviceMemory mem; VkImageView view; VkFramebuffer fb;
    VkPipeline pipe; VkDescriptorSet set;
} PostTarget144;
static struct {
    VkRenderPass pass;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout pl;
    VkDescriptorPool pool;
    VkSampler sampler;
    PostTarget144 target[2];
    Ring ubo;
    uint32_t w,h,source_w,source_h,vs,fs[2],plain;
    float gamma[256][4]; int gamma_on, ready, failed, two;
    uint64_t frames,record_us,last;
} post144;

static int option144(const char *name,int fallback,int lo,int hi)
{
    const char *s=getenv(name); char *end; long n;
    if(!s||!*s)return fallback;
    n=strtol(s,&end,10);return *end||n<lo||n>hi?fallback:(int)n;
}
static void post_drop144(void)
{
    /* Caller drained submitted commands before any destruction. */
    for(unsigned i=0;i<2;i++) {
        PostTarget144 *t=&post144.target[i];
        if(t->pipe)vkDestroyPipeline(vk.dev,t->pipe,NULL);
        if(t->fb)vkDestroyFramebuffer(vk.dev,t->fb,NULL);
        if(t->view)vkDestroyImageView(vk.dev,t->view,NULL);
        if(t->img)vkDestroyImage(vk.dev,t->img,NULL);
        if(t->mem)vkFreeMemory(vk.dev,t->mem,NULL);
        memset(t,0,sizeof *t);
    }
    if(post144.pool)vkDestroyDescriptorPool(vk.dev,post144.pool,NULL);
    if(post144.pl)vkDestroyPipelineLayout(vk.dev,post144.pl,NULL);
    if(post144.dsl)vkDestroyDescriptorSetLayout(vk.dev,post144.dsl,NULL);
    if(post144.pass)vkDestroyRenderPass(vk.dev,post144.pass,NULL);
    if(post144.sampler)vkDestroySampler(vk.dev,post144.sampler,NULL);
    if(post144.ubo.map)vkUnmapMemory(vk.dev,post144.ubo.mem);
    if(post144.ubo.buf)vkDestroyBuffer(vk.dev,post144.ubo.buf,NULL);
    if(post144.ubo.mem)vkFreeMemory(vk.dev,post144.ubo.mem,NULL);
    memset(&post144.ubo,0,sizeof post144.ubo);
    post144.pool=VK_NULL_HANDLE;post144.pl=VK_NULL_HANDLE;post144.dsl=VK_NULL_HANDLE;
    post144.pass=VK_NULL_HANDLE;post144.sampler=VK_NULL_HANDLE;post144.ready=0;
}
static uint32_t post_shader144(int easu,int rcas)
{
    const char *define=easu?"#define FSR_EASU_F 1\n":rcas?"#define FSR_RCAS_F 1\n":"";
    const char *end=easu?s_post144_easu:s_post144_finish;
    size_t size=strlen(define)+strlen(s_post144_common)+strlen(s_post144_base)+strlen(end)+1;
    char *text=malloc(size);uint32_t shader=0;
    if(text){snprintf(text,size,"%s%s%s%s",define,s_post144_common,s_post144_base,end);
        shader=shader_make(text,0,easu?"fsr1_easu144":rcas?"fsr1_rcas144":"screen_filter144");free(text);}
    return shader;
}
static uint32_t post_spatial_shader34(void)
{
    const char *define=s_spatial34==2?"#define SPATIAL_LANCZOS 1\n":"";
    size_t n=strlen(define)+strlen(s_post144_base)+strlen(s_spatial34_shader)+1;
    char *text=malloc(n);uint32_t shader=0;
    if(text){snprintf(text,n,"%s%s%s",define,s_post144_base,s_spatial34_shader);
        shader=shader_make(text,0,s_spatial34==2?"spatial_lanczos34":"spatial_bicubic34");free(text);}
    return shader;
}
static void post_prepare144(void)
{
    if(!s_upscale144&&!s_filter144&&!s_quality_fxaa145&&!s_spatial34)return;
    uint64_t start=usec_now();
    post144.vs=shader_make(s_gamma_vs,1,"post_vs144");
    post144.plain=post_shader144(0,0);
    post144.fs[0]=s_upscale144?post_shader144(1,0):s_spatial34?post_spatial_shader34():post144.plain;
    if(s_upscale144)post144.fs[1]=post_shader144(0,1);
    post144.failed=!post144.vs||!post144.plain||!post144.fs[0]||(s_upscale144&&!post144.fs[1]);
    LOG("[POST144] shaders prepared in %.2f ms; FSR1=%d filter=%d; image quality costs GPU time%s",
        (usec_now()-start)/1000.0,s_upscale144,s_filter144,post144.failed?"; failed, original presentation retained":"");
}
static int post_make144(uint32_t w,uint32_t h,int two)
{
    VkFormatProperties fp;VkPhysicalDeviceProperties props;
    VkAttachmentDescription att={0};VkAttachmentReference ref={0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub={0};VkSubpassDependency dep[2]={{0}};
    VkRenderPassCreateInfo rp={VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    VkDescriptorSetLayoutBinding bindings[3]={{0}};
    VkDescriptorSetLayoutCreateInfo dl={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    VkPipelineLayoutCreateInfo pl={VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkDescriptorPoolSize sizes[3]={{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,2},{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,2},{VK_DESCRIPTOR_TYPE_SAMPLER,2}};
    VkDescriptorPoolCreateInfo pool={VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    VkSamplerCreateInfo sampler={VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    const VkDeviceSize stride=sizeof s_gamma+8*sizeof(float);
    vkGetPhysicalDeviceProperties(vk.phys,&props);
    if(!w||!h||w>props.limits.maxImageDimension2D||h>props.limits.maxImageDimension2D||w>4096||h>4096){post144.failed=1;LOG("[POST144] output extent unsupported; original presentation retained");return 0;}
    vkGetPhysicalDeviceFormatProperties(vk.phys,VK_FORMAT_R8G8B8A8_UNORM,&fp);
    VkFormatFeatureFlags need=VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if((fp.optimalTilingFeatures&need)!=need){post144.failed=1;LOG("[POST144] RGBA8 sampled/linear/attachment features unavailable; original presentation retained");return 0;}
    post144.w=w;post144.h=h;post144.two=two;
    att.format=VK_FORMAT_R8G8B8A8_UNORM;att.samples=VK_SAMPLE_COUNT_1_BIT;
    att.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;att.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;att.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;att.finalLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=1;sub.pColorAttachments=&ref;
    /* Also cover previous frame's shader/transfer readers before reusing the attachments. */
    dep[0].srcSubpass=VK_SUBPASS_EXTERNAL;dep[0].dstSubpass=0;
    dep[0].srcStageMask=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT;
    dep[0].srcAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_TRANSFER_READ_BIT;
    dep[0].dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;dep[0].dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[1].srcSubpass=0;dep[1].dstSubpass=VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;dep[1].srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[1].dstStageMask=VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT|VK_PIPELINE_STAGE_TRANSFER_BIT;
    dep[1].dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_TRANSFER_READ_BIT;
    rp.attachmentCount=1;rp.pAttachments=&att;rp.subpassCount=1;rp.pSubpasses=&sub;rp.dependencyCount=2;rp.pDependencies=dep;
    if(vkCreateRenderPass(vk.dev,&rp,NULL,&post144.pass)!=VK_SUCCESS)goto fail;
    bindings[0]=(VkDescriptorSetLayoutBinding){0,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_FRAGMENT_BIT,NULL};
    bindings[1]=(VkDescriptorSetLayoutBinding){8,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_FRAGMENT_BIT,NULL};
    bindings[2]=(VkDescriptorSetLayoutBinding){16,VK_DESCRIPTOR_TYPE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,NULL};
    dl.bindingCount=3;dl.pBindings=bindings;
    if(vkCreateDescriptorSetLayout(vk.dev,&dl,NULL,&post144.dsl)!=VK_SUCCESS)goto fail;
    pl.setLayoutCount=1;pl.pSetLayouts=&post144.dsl;
    if(vkCreatePipelineLayout(vk.dev,&pl,NULL,&post144.pl)!=VK_SUCCESS)goto fail;
    pool.maxSets=2;pool.poolSizeCount=3;pool.pPoolSizes=sizes;
    if(vkCreateDescriptorPool(vk.dev,&pool,NULL,&post144.pool)!=VK_SUCCESS)goto fail;
    sampler.minFilter=sampler.magFilter=VK_FILTER_LINEAR;
    sampler.addressModeU=sampler.addressModeV=sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if(vkCreateSampler(vk.dev,&sampler,NULL,&post144.sampler)!=VK_SUCCESS)goto fail;
    VkDeviceSize aligned=(stride+props.limits.minUniformBufferOffsetAlignment-1)/props.limits.minUniformBufferOffsetAlignment*props.limits.minUniformBufferOffsetAlignment;
    if(!ring_make(&post144.ubo,aligned*2,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT))goto fail;
    for(unsigned i=0;i<(two==1?2u:1u);i++) {
        PostTarget144 *t=&post144.target[i];
        VkFramebufferCreateInfo fb={VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        VkDescriptorSetAllocateInfo ai={VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        VkWriteDescriptorSet writes[3]={{0}};
        VkDescriptorBufferInfo buffer={post144.ubo.buf,aligned*i,stride};
        VkDescriptorImageInfo image={VK_NULL_HANDLE,i?post144.target[0].view:vk.color_view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo samp={post144.sampler,VK_NULL_HANDLE,VK_IMAGE_LAYOUT_UNDEFINED};
        VkGraphicsPipelineCreateInfo ci={VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        VkPipelineShaderStageCreateInfo stages[2]={{0}};
        VkPipelineVertexInputStateCreateInfo vi={VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia={VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        VkPipelineViewportStateCreateInfo vp={VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        VkPipelineRasterizationStateCreateInfo rs={VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        VkPipelineMultisampleStateCreateInfo ms={VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        VkPipelineColorBlendAttachmentState blend={0};
        VkPipelineColorBlendStateCreateInfo cb={VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        const VkDynamicState dynamic[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dy={VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        if(!image_make(w,h,VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT,&t->img,&t->mem,&t->view))goto fail;
        fb.renderPass=post144.pass;fb.attachmentCount=1;fb.pAttachments=&t->view;fb.width=w;fb.height=h;fb.layers=1;
        if(vkCreateFramebuffer(vk.dev,&fb,NULL,&t->fb)!=VK_SUCCESS)goto fail;
        ai.descriptorPool=post144.pool;ai.descriptorSetCount=1;ai.pSetLayouts=&post144.dsl;
        if(vkAllocateDescriptorSets(vk.dev,&ai,&t->set)!=VK_SUCCESS)goto fail;
        for(unsigned j=0;j<3;j++){writes[j].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;writes[j].dstSet=t->set;writes[j].descriptorCount=1;}
        writes[0].dstBinding=0;writes[0].descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;writes[0].pBufferInfo=&buffer;
        writes[1].dstBinding=8;writes[1].descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;writes[1].pImageInfo=&image;
        writes[2].dstBinding=16;writes[2].descriptorType=VK_DESCRIPTOR_TYPE_SAMPLER;writes[2].pImageInfo=&samp;
        vkUpdateDescriptorSets(vk.dev,3,writes,0,NULL);
        for(unsigned j=0;j<2;j++){stages[j].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;stages[j].pName="main";}
        stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=vk.shaders[post144.vs-1];
        stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=vk.shaders[(two?post144.fs[i]:post144.plain)-1];
        ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;vp.viewportCount=vp.scissorCount=1;
        rs.polygonMode=VK_POLYGON_MODE_FILL;rs.cullMode=VK_CULL_MODE_NONE;rs.lineWidth=1;
        ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;blend.colorWriteMask=15;cb.attachmentCount=1;cb.pAttachments=&blend;
        dy.dynamicStateCount=2;dy.pDynamicStates=dynamic;
        ci.stageCount=2;ci.pStages=stages;ci.pVertexInputState=&vi;ci.pInputAssemblyState=&ia;ci.pViewportState=&vp;
        ci.pRasterizationState=&rs;ci.pMultisampleState=&ms;ci.pColorBlendState=&cb;ci.pDynamicState=&dy;
        ci.layout=post144.pl;ci.renderPass=post144.pass;
        if(vkCreateGraphicsPipelines(vk.dev,vk.pcache,1,&ci,NULL,&t->pipe)!=VK_SUCCESS)goto fail;
        uint8_t *out=post144.ubo.map+aligned*i;
        memcpy(out,s_gamma,sizeof s_gamma);
        const float dims[4]={i?(float)w:(float)vk.tw,i?(float)h:(float)vk.th,(float)w,(float)h};
        const float control[4]={s_gamma_on?1.f:0.f,(float)s_filter144,s_sharpness144,s_quality_fxaa145?1.f:0.f};
        memcpy(out+sizeof s_gamma,dims,sizeof dims);memcpy(out+sizeof s_gamma+sizeof dims,control,sizeof control);
    }
    memcpy(post144.gamma,s_gamma,sizeof s_gamma);post144.gamma_on=s_gamma_on;post144.source_w=vk.tw;post144.source_h=vk.th;
    post144.ready=1;LOG("[POST144] %ux%u -> %ux%u; %s; filter=%d; FSR supersedes FXAA when enabled",
        vk.tw,vk.th,w,h,two==1?"AMD FSR1 EASU+RCAS (FP32)":two==2?(s_spatial34==2?"Lanczos-2":"Bicubic"):"screen filter",s_filter144);return 1;
fail:
    post_drop144();post144.failed=1;LOG("[POST144] allocation/pipeline failed; original presentation retained");return 0;
}
static int post_draw144(VkImage *image,uint32_t *width,uint32_t *height)
{
    if((!s_upscale144&&!s_filter144&&!s_quality_fxaa145&&!s_spatial34)||post144.failed||!post144.vs)return 0;
    VkOffset3D rect[2];present_rect(vk.swap_ext.width,vk.swap_ext.height,rect);
    uint32_t w=vk.tw,h=vk.th;int two=0;
    /* EASU only enlarges: never silently downsample or run FSR at equal size. */
    if(s_upscale144&&(uint32_t)(rect[1].x-rect[0].x)>w&&(uint32_t)(rect[1].y-rect[0].y)>h) {
        w=(uint32_t)(rect[1].x-rect[0].x);h=(uint32_t)(rect[1].y-rect[0].y);two=1;
    } else if(s_upscale144){
        static uint32_t bypass_w144,bypass_h144;
        uint32_t dw=(uint32_t)(rect[1].x-rect[0].x),dh=(uint32_t)(rect[1].y-rect[0].y);
        if(dw!=bypass_w144||dh!=bypass_h144){LOG("[POST144] FSR bypassed: output %ux%u is not larger than input %ux%u in both dimensions",dw,dh,vk.tw,vk.th);bypass_w144=dw;bypass_h144=dh;}
        if(!s_filter144&&!s_quality_fxaa145)return 0;
    } else if(s_spatial34){
        uint32_t dw=(uint32_t)(rect[1].x-rect[0].x),dh=(uint32_t)(rect[1].y-rect[0].y);
        if(dw>w&&dh>h){w=dw;h=dh;two=2;}
        else if(!s_filter144&&!s_quality_fxaa145)return 0;
    }
    if(!post144.ready||w!=post144.w||h!=post144.h||two!=post144.two) {
        if(!frame_drain_pending())return 0;
        post_drop144();
        if(!post_make144(w,h,two))return 0;
    }
    if(post144.gamma_on!=s_gamma_on||memcmp(post144.gamma,s_gamma,sizeof s_gamma)) {
        if(!frame_drain_pending())return 0;
        VkPhysicalDeviceProperties props;vkGetPhysicalDeviceProperties(vk.phys,&props);
        size_t stride=sizeof s_gamma+8*sizeof(float),a=props.limits.minUniformBufferOffsetAlignment;
        stride=(stride+a-1)/a*a;
        for(unsigned i=0;i<(two==1?2u:1u);i++){
            memcpy(post144.ubo.map+stride*i,s_gamma,sizeof s_gamma);
            float on=s_gamma_on?1.f:0.f;memcpy(post144.ubo.map+stride*i+sizeof s_gamma+4*sizeof(float),&on,sizeof on);
        }
        memcpy(post144.gamma,s_gamma,sizeof s_gamma);post144.gamma_on=s_gamma_on;
    }
    uint64_t start=usec_now();
    image_layout(vk.cmd,vk.color,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    for(unsigned i=0;i<(two==1?2u:1u);i++){
        VkRenderPassBeginInfo rp={VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        VkViewport vp={0,0,(float)w,(float)h,0,1};VkRect2D sc={{0,0},{w,h}};
        rp.renderPass=post144.pass;rp.framebuffer=post144.target[i].fb;rp.renderArea=sc;
        vkCmdBeginRenderPass(vk.cmd,&rp,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdSetViewport(vk.cmd,0,1,&vp);vkCmdSetScissor(vk.cmd,0,1,&sc);
        vkCmdBindPipeline(vk.cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,post144.target[i].pipe);
        vkCmdBindDescriptorSets(vk.cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,post144.pl,0,1,&post144.target[i].set,0,NULL);
        vkCmdDraw(vk.cmd,3,1,0,0);vkCmdEndRenderPass(vk.cmd);
    }
    *image=post144.target[two==1?1:0].img;*width=w;*height=h;
    image_layout(vk.cmd,*image,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    image_layout(vk.cmd,vk.color,VK_IMAGE_ASPECT_COLOR_BIT,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    post144.record_us+=usec_now()-start;post144.frames++;
    uint64_t now=usec_now();if(!post144.last)post144.last=now;
    if(now-post144.last>=10000000){LOG("[POST144-10S] frames=%llu CPU_record=%.3f ms/frame; GPU cost not included; output=%ux%u",
        (unsigned long long)post144.frames,post144.frames?post144.record_us/(1000.0*post144.frames):0.0,w,h);
        post144.frames=post144.record_us=0;post144.last=now;}
    return 1;
}

/* Called on the Vulkan owner before this frame records any presentation pass.
 * Draws already recorded in the current frame reference no post144 resources.
 * Keep old resources on a fence failure; consume the request only on success. */
static void post_apply145(void)
{
    unsigned bits=atomic_load_explicit(&s_quality_pending145,memory_order_acquire);
    if(!bits)return;
    if(!frame_drain_pending())return;
    bits=atomic_exchange_explicit(&s_quality_pending145,0,memory_order_acq_rel);
    if(!bits)return;
    post_drop144();post144.failed=0;
    s_upscale144=(bits&1)!=0;s_quality_fxaa145=(bits&2)!=0;
    s_filter144=(int)((bits>>2)&7);s_sharpness144=((bits>>5)&255)/100.f;
    post_prepare144();
    LOG("[GRAPHICS145] presentation applied at frame boundary: fsr=%d fxaa=%d filter=%d",s_upscale144,s_quality_fxaa145,s_filter144);
}
