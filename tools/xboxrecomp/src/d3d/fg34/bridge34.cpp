/* win-fg compositor bridge for DefJam. Vulkan handles are borrowed from the
 * game's existing loader; neither the device nor queue is created here. */
#include "fg34.h"
#include "vendor/src/framegen.hpp"
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace {
struct Output { VkImage image{}; VkDeviceMemory memory{}; VkImageView view{}; bool initialized{}; };
void message(char *out, size_t size, const char *value) { if (out && size) std::snprintf(out, size, "%s", value); }

struct Session {
    FG34Info info;
    winfg::InstanceDispatch id{};
    winfg::DeviceDispatch dd{};
    winfg::FrameGen engine;
    std::unordered_map<VkImage, VkImageView> views;
    std::vector<Output> outputs;
    uint64_t pairA{}, pairB{};
    unsigned computed{};
    bool engineStarted{};

    explicit Session(const FG34Info &i) : info(i) {}
    ~Session() {
        if (engineStarted) engine.destroy();
        for (auto &it : views) if (it.second) dd.DestroyImageView(info.device, it.second, nullptr);
        for (auto &o : outputs) {
            if (o.view) dd.DestroyImageView(info.device, o.view, nullptr);
            if (o.image) dd.DestroyImage(info.device, o.image, nullptr);
            if (o.memory) dd.FreeMemory(info.device, o.memory, nullptr);
        }
    }
    void init() {
        auto gp=info.get_instance_proc;
        id.GetPhysicalDeviceMemoryProperties=(PFN_vkGetPhysicalDeviceMemoryProperties)gp(info.instance,"vkGetPhysicalDeviceMemoryProperties");
        auto get=(PFN_vkGetDeviceProcAddr)gp(info.instance,"vkGetDeviceProcAddr");
        if(!id.GetPhysicalDeviceMemoryProperties||!get) throw std::runtime_error("Vulkan loader missing required entry points");
#define LOAD(n) do { dd.n=(PFN_vk##n)get(info.device,"vk" #n); if(!dd.n) throw std::runtime_error("missing vk" #n); } while(0)
        LOAD(AllocateDescriptorSets); LOAD(AllocateMemory); LOAD(BindBufferMemory); LOAD(BindImageMemory);
        LOAD(CmdBindDescriptorSets); LOAD(CmdBindPipeline); LOAD(CmdClearColorImage); LOAD(CmdCopyImage);
        LOAD(CmdDispatch); LOAD(CmdPipelineBarrier); LOAD(CreateBuffer); LOAD(CreateComputePipelines);
        LOAD(CreateDescriptorPool); LOAD(CreateDescriptorSetLayout); LOAD(CreateImage); LOAD(CreateImageView);
        LOAD(CreatePipelineLayout); LOAD(CreateSampler); LOAD(CreateShaderModule); LOAD(DestroyBuffer);
        LOAD(DestroyDescriptorPool); LOAD(DestroyDescriptorSetLayout); LOAD(DestroyImage); LOAD(DestroyImageView);
        LOAD(DestroyPipeline); LOAD(DestroyPipelineLayout); LOAD(DestroySampler); LOAD(DestroyShaderModule);
        LOAD(DeviceWaitIdle); LOAD(FreeMemory); LOAD(GetBufferMemoryRequirements); LOAD(GetImageMemoryRequirements);
        LOAD(MapMemory); LOAD(ResetDescriptorPool); LOAD(UnmapMemory); LOAD(UpdateDescriptorSets);
#undef LOAD
        engineStarted=true;
        if(!engine.init(&dd,&id,info.physical,info.device,info.family,info.queue)) throw std::runtime_error("frame generator init failed");
        winfg::Config cfg{};
        // The previous slider was a quality knob: do not use it to scale motion vectors.
        cfg.flowScale=1.0f; cfg.perfPreset=info.flow_scale<0.5f?2:info.flow_scale<0.8f?1:0;
        cfg.model=cfg.perfPreset==0?4:3; cfg.gmMode=2; cfg.frMode=2; cfg.frIters=0;
        engine.configure(cfg);
        if(!engine.onResize({info.width,info.height}, VK_FORMAT_R8G8B8A8_UNORM)) throw std::runtime_error("frame generator image allocation failed");
        outputs.resize(info.multiplier-1);
        VkPhysicalDeviceMemoryProperties mem{}; id.GetPhysicalDeviceMemoryProperties(info.physical,&mem);
        for(auto &o : outputs) {
            VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            ci.imageType=VK_IMAGE_TYPE_2D;ci.format=VK_FORMAT_R8G8B8A8_UNORM;
            ci.extent={info.width,info.height,1};ci.mipLevels=1;ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;
            ci.tiling=VK_IMAGE_TILING_OPTIMAL;ci.usage=VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            if(dd.CreateImage(info.device,&ci,nullptr,&o.image)!=VK_SUCCESS)throw std::runtime_error("FG output image failed");
            VkMemoryRequirements req{};dd.GetImageMemoryRequirements(info.device,o.image,&req);
            uint32_t index=UINT32_MAX;for(uint32_t k=0;k<mem.memoryTypeCount;k++)if((req.memoryTypeBits&(1u<<k))&&(mem.memoryTypes[k].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)){index=k;break;}
            if(index==UINT32_MAX)throw std::runtime_error("FG output memory unavailable");
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=index;
            if(dd.AllocateMemory(info.device,&ai,nullptr,&o.memory)!=VK_SUCCESS||dd.BindImageMemory(info.device,o.image,o.memory,0)!=VK_SUCCESS)throw std::runtime_error("FG output memory failed");
            o.view=createView(o.image);
        }
        std::fprintf(stderr,"[FG34] open optical flow (FSR3-derived) + win-fg synth, preset=%d, %ux%u; no DLL; experimental\n",cfg.perfPreset,info.width,info.height);
    }
    VkImageView createView(VkImage image) {
        VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};v.image=image;v.viewType=VK_IMAGE_VIEW_TYPE_2D;
        v.format=VK_FORMAT_R8G8B8A8_UNORM;v.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        VkImageView out{};if(dd.CreateImageView(info.device,&v,nullptr,&out)!=VK_SUCCESS)throw std::runtime_error("FG input view failed");return out;
    }
    VkImageView view(VkImage image) {
        auto it=views.find(image);if(it!=views.end())return it->second;
        VkImageView v=createView(image);views.emplace(image,v);return v;
    }
    void barrier(VkImage image,VkImageLayout oldLayout,VkImageLayout newLayout) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=image;b.oldLayout=oldLayout;b.newLayout=newLayout;
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        b.srcAccessMask=oldLayout==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
        dd.CmdPipelineBarrier(info.command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);
    }
};
}

extern "C" FG34_API void *defjam_fg34_create(const FG34Info *p,char *error,size_t size) {
    try {
        if(!p||p->abi!=2||p->size!=sizeof(*p)||!p->get_instance_proc||!p->instance||!p->physical||!p->device||!p->queue||!p->command||p->width<128||p->height<128||p->multiplier<2||p->multiplier>4)throw std::runtime_error("invalid FG34 arguments");
        auto s=std::make_unique<Session>(*p);s->init();return s.release();
    }catch(const std::exception &e){message(error,size,e.what());return nullptr;}
     catch(...){message(error,size,"unknown frame generator initialization error");return nullptr;}
}
extern "C" FG34_API int defjam_fg34_record(void *p,VkImage a,VkImage b,uint64_t sa,uint64_t sb,unsigned phase,VkImage *output,int *unique,char *error,size_t size) {
    try {
        if(!p||!a||!b||!sa||sb<=sa||!output||!unique)throw std::runtime_error("invalid frame pair");
        auto &s=*static_cast<Session*>(p);if(phase<1||phase>s.outputs.size())throw std::runtime_error("invalid generation phase");
        if(s.pairA!=sa||s.pairB!=sb){s.pairA=sa;s.pairB=sb;s.computed=0;}
        unsigned bit=1u<<(phase-1);auto &o=s.outputs[phase-1];
        *unique=(s.computed&bit)==0;
        if(*unique) {
            auto va=s.view(a),vb=s.view(b);
            s.barrier(o.image,o.initialized?VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL);
            if(!s.engine.record(s.info.command,va,vb,o.view,(float)phase/s.info.multiplier,0))throw std::runtime_error("FG record failed");
            s.barrier(o.image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            o.initialized=true;s.computed|=bit;
        }
        *output=o.image;return 1;
    }catch(const std::exception &e){message(error,size,e.what());return 0;}
     catch(...){message(error,size,"unknown frame generator error");return 0;}
}
extern "C" FG34_API void defjam_fg34_destroy(void *p){delete static_cast<Session*>(p);}
