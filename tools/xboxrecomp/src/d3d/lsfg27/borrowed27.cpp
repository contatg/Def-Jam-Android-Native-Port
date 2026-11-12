/* Native adapter for the MIT lsfg-vk compute backend. No DLL execution. */
#include <volk.h>
#include "lsfg27.h"
#include "core/device.hpp"
#include "core/image.hpp"
#include "core/commandbuffer.hpp"
#include "common/utils.hpp"
#include "v3_1p/shaders/mipmaps.hpp"
#include "v3_1p/shaders/alpha.hpp"
#include "v3_1p/shaders/beta.hpp"
#include "v3_1p/shaders/gamma.hpp"
#include "v3_1p/shaders/delta.hpp"
#include "v3_1p/shaders/generate.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace LSFG::Core {
Device::Device(VkPhysicalDevice physical,VkDevice borrowed,VkQueue queue,uint32_t family)
    : device(std::make_shared<VkDevice>(borrowed)),physicalDevice(physical),
      computeFamilyIdx(family),computeQueue(queue) {
    fallbackDescriptorImage=std::make_shared<Image>(*this,VkExtent2D{1,1},
        VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_USAGE_SAMPLED_BIT|
        VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT);
}
const Image& Device::getFallbackDescriptorImage() const {return *fallbackDescriptorImage;}
CommandBuffer::CommandBuffer(VkCommandBuffer borrowed)
    : state(std::make_shared<CommandBufferState>(CommandBufferState::Recording)),
      commandBuffer(std::make_shared<VkCommandBuffer>(borrowed)) {}
}

namespace {
using namespace LSFG;
using namespace LSFG_3_1P;
void barrier(VkCommandBuffer cmd,VkImage image,VkImageLayout from,VkImageLayout to) {
    VkImageMemoryBarrier b{};b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask=from==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout=from;b.newLayout=to;b.image=image;
    b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);
}
unsigned shader_id(const std::string &name) {
    if(name=="p_mipmaps")return 255;
    if(name=="p_generate")return 256;
    const std::array<unsigned,4> alpha{290,291,292,293};
    const std::array<unsigned,5> beta{298,299,300,301,302},gamma{280,282,283,284,285};
    const std::array<unsigned,10> delta{280,286,287,288,289,281,294,295,296,297};
    for(size_t i=0;i<alpha.size();i++)if(name=="p_alpha["+std::to_string(i)+"]")return alpha[i];
    for(size_t i=0;i<beta.size();i++)if(name=="p_beta["+std::to_string(i)+"]")return beta[i];
    for(size_t i=0;i<gamma.size();i++)if(name=="p_gamma["+std::to_string(i)+"]")return gamma[i];
    for(size_t i=0;i<delta.size();i++)if(name=="p_delta["+std::to_string(i)+"]")return delta[i];
    throw std::runtime_error("Unknown LSFG shader name: "+name);
}
std::vector<uint8_t> load_shader(const std::string &folder,const std::string &name) {
    auto path=folder+"/"+std::to_string(shader_id(name))+".spv";
    std::ifstream f(path,std::ios::binary|std::ios::ate);
    if(!f)throw std::runtime_error("Run Importar-Lossless27.py; shader missing: "+path);
    auto n=f.tellg();
    if(n<20||n>2*1024*1024||static_cast<size_t>(n)%4)throw std::runtime_error("Invalid shader size: "+path);
    std::vector<uint8_t> result(static_cast<size_t>(n));f.seekg(0);
    if(!f.read(reinterpret_cast<char*>(result.data()),n))throw std::runtime_error("Cannot read: "+path);
    uint32_t magic{};std::memcpy(&magic,result.data(),4);
    if(magic!=0x07230203)throw std::runtime_error("Invalid shader magic: "+path);
    return result;
}
struct Session {
    Vulkan vk;
    Core::CommandBuffer command;
    Core::Image inputs[2];std::vector<Core::Image> outputs;unsigned shown=0,computed=0;uint64_t pair_frame=0;
    Shaders::Mipmaps mipmaps;
    std::array<Shaders::Alpha,7> alpha;
    Shaders::Beta beta;
    std::array<Shaders::Gamma,7> gamma;
    std::array<Shaders::Delta,3> delta;
    Shaders::Generate generate;
    uint64_t frame=0,last=0,pair_left=0,pair_right=0;
    bool initialized=false;
    explicit Session(const LSFG27Info &i)
        : vk{.device{ i.physical,i.device,i.queue,i.family },
             .generationCount=i.multiplier-1,.flowScale=1.f/i.flow_scale,.isHdr=false},
          command(i.command) {
        vk.descriptorPool=Core::DescriptorPool(vk.device);
        vk.resources=Pool::ResourcePool(false,vk.flowScale);
        std::string folder=std::string(i.shader_directory)+(i.fp16?"/fp16":"/fp32");
        vk.shaders=Pool::ShaderPool([folder](const std::string &name){return load_shader(folder,name);});
        VkExtent2D extent{i.width,i.height};
        for(auto &img:inputs)img=Core::Image(vk.device,extent,VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        for(unsigned pass=0;pass<i.multiplier-1;pass++)outputs.emplace_back(vk.device,extent,VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        mipmaps=Shaders::Mipmaps(vk,inputs[0],inputs[1]);
        for(size_t n=0;n<7;n++)alpha[n]=Shaders::Alpha(vk,mipmaps.getOutImages()[n]);
        beta=Shaders::Beta(vk,alpha[0].getOutImages());
        for(size_t n=0;n<7;n++) {
            gamma[n]=Shaders::Gamma(vk,alpha[6-n].getOutImages(),beta.getOutImages()[std::min<size_t>(6-n,5)],
                n?std::make_optional(gamma[n-1].getOutImage()):std::nullopt);
            if(n>=4)delta[n-4]=Shaders::Delta(vk,alpha[6-n].getOutImages(),beta.getOutImages()[6-n],
                n==4?std::nullopt:std::make_optional(gamma[n-1].getOutImage()),
                n==4?std::nullopt:std::make_optional(delta[n-5].getOutImage1()),
                n==4?std::nullopt:std::make_optional(delta[n-5].getOutImage2()));
        }
        generate=Shaders::Generate(vk,inputs[0],inputs[1],gamma[6].getOutImage(),
            delta[2].getOutImage1(),delta[2].getOutImage2(),outputs);
    }
    void preprocess(VkImage image,uint64_t count) {
        auto cmd=command.handle();auto &dst=inputs[count%2];auto size=dst.getExtent();
        barrier(cmd,image,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        barrier(cmd,dst.handle(),dst.getLayout(),VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy copy{};copy.srcSubresource=copy.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
        copy.extent={size.width,size.height,1};
        vkCmdCopyImage(cmd,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,dst.handle(),VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
        barrier(cmd,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        barrier(cmd,dst.handle(),VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL);
        dst.setLayout(VK_IMAGE_LAYOUT_GENERAL);
        mipmaps.Dispatch(command,count);
        for(size_t j=0;j<7;j++)alpha[6-j].Dispatch(command,count);
    }
    bool prepare_pair(VkImage previous,VkImage current,uint64_t a,uint64_t b) {
        if(pair_left==a&&pair_right==b)return false;
        if(!initialized) {
            auto fallback=vk.device.getFallbackDescriptorImage();
            barrier(command.handle(),fallback.handle(),VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkClearColorValue black{};VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            vkCmdClearColorImage(command.handle(),fallback.handle(),VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&black,1,&range);
            barrier(command.handle(),fallback.handle(),VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL);
            fallback.setLayout(VK_IMAGE_LAYOUT_GENERAL);initialized=true;
        }
        if(last!=a) {
            // Reset temporal feature history on skipped captures. All three
            // history slots are initialized before the neural chain reads them.
            frame=0;for(unsigned j=0;j<3;j++)preprocess(previous,frame++);
        }
        preprocess(current,frame);
        beta.Dispatch(command,frame);
        pair_frame=frame;shown=computed=0;
        frame++;last=b;pair_left=a;pair_right=b;return true;
    }
    bool record(VkImage previous,VkImage current,uint64_t a,uint64_t b,unsigned phase) {
        prepare_pair(previous,current,a,b);
        unsigned bit=1u<<(phase-1);if(computed&bit)return false;
        size_t pass=phase-1;
        for(size_t j=0;j<7;j++) {
            gamma[j].Dispatch(command,pair_frame,pass);
            if(j>=4)delta[j-4].Dispatch(command,pair_frame,pass);
        }
        generate.Dispatch(command,pair_frame,pass);
        auto &output=outputs[pass];
        barrier(command.handle(),output.handle(),output.getLayout(),VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        output.setLayout(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        computed|=bit;return true;
    }
};
void message(char *out,size_t n,const char *s){if(out&&n)std::snprintf(out,n,"%s",s);}
}

extern "C" LSFG27_API void *defjam_lsfg27_create(const LSFG27Info *i,char *error,size_t n) {
    try {
        if(!i||i->abi!=2||i->size!=sizeof(*i)||!i->get_instance_proc||!i->instance||!i->physical||
           !i->device||!i->command||!i->shader_directory||i->flow_scale<.25f||i->flow_scale>1.f||i->multiplier<2||i->multiplier>4)
            throw std::runtime_error("Invalid native LSFG adapter arguments");
        // Private, hidden Volk dispatch table. Use the game's selected loader.
        volkInitializeCustom(i->get_instance_proc);volkLoadInstance(i->instance);volkLoadDevice(i->device);
        // Host does not enable synchronization2; use the audited sync1 barriers.
        vkCmdPipelineBarrier2=nullptr;vkCmdPipelineBarrier2KHR=nullptr;
        return new Session(*i);
    }catch(const std::exception &e){message(error,n,e.what());return nullptr;}
     catch(...){message(error,n,"Unknown LSFG initialization error");return nullptr;}
}
extern "C" LSFG27_API int defjam_lsfg27_record(void *p,VkImage a,VkImage b,uint64_t sa,uint64_t sb,unsigned phase,
    VkImage *output,int *unique,char *error,size_t n) {
    try {
        if(!p||!a||!b||!sa||sb<=sa||!output||!unique)throw std::runtime_error("Invalid immutable LSFG input pair");
        auto &s=*static_cast<Session*>(p);
        if(phase<1||phase>s.outputs.size())throw std::runtime_error("Invalid LSFG generation phase");
        s.record(a,b,sa,sb,phase);unsigned bit=1u<<(phase-1);
        *unique=!(s.shown&bit);s.shown|=bit;*output=s.outputs[phase-1].handle();return 1;
    }catch(const std::exception &e){message(error,n,e.what());return 0;}
     catch(...){message(error,n,"Unknown LSFG record error");return 0;}
}
extern "C" LSFG27_API void defjam_lsfg27_destroy(void *p) {delete static_cast<Session*>(p);}
