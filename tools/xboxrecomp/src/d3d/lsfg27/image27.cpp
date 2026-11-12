/* Private images for native LSFG. MIT lsfg-vk interface, no external memory. */
#include <volk.h>
#include "core/image.hpp"
#include "common/exception.hpp"
#include <algorithm>
#include <stdexcept>
using namespace LSFG::Core;
Image::Image(const Device &device,VkExtent2D requested,VkFormat fmt,
        VkImageUsageFlags usage,VkImageAspectFlags aspect)
    : extent{std::max(1u,requested.width),std::max(1u,requested.height)},
      format(fmt),aspectFlags(aspect) {
    VkImage rawImage{};VkDeviceMemory rawMemory{};VkImageView rawView{};
    VkDevice dev=device.handle();
    try {
        VkImageCreateInfo c{};c.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        c.imageType=VK_IMAGE_TYPE_2D;c.format=fmt;c.extent={extent.width,extent.height,1};
        c.mipLevels=c.arrayLayers=1;c.samples=VK_SAMPLE_COUNT_1_BIT;c.usage=usage;
        auto r=vkCreateImage(dev,&c,nullptr,&rawImage);
        if(r!=VK_SUCCESS)throw LSFG::vulkan_error(r,"LSFG private image creation failed");
        VkMemoryRequirements req{};vkGetImageMemoryRequirements(dev,rawImage,&req);
        VkPhysicalDeviceMemoryProperties props{};vkGetPhysicalDeviceMemoryProperties(device.getPhysicalDevice(),&props);
        uint32_t type=props.memoryTypeCount;
        for(uint32_t j=0;j<props.memoryTypeCount;j++)if((req.memoryTypeBits&(1u<<j))&&
            (props.memoryTypes[j].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)){type=j;break;}
        if(type==props.memoryTypeCount)throw std::runtime_error("LSFG image has no compatible device memory");
        VkMemoryAllocateInfo alloc{};alloc.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.allocationSize=req.size;alloc.memoryTypeIndex=type;
        r=vkAllocateMemory(dev,&alloc,nullptr,&rawMemory);
        if(r!=VK_SUCCESS)throw LSFG::vulkan_error(r,"LSFG private image memory failed");
        r=vkBindImageMemory(dev,rawImage,rawMemory,0);
        if(r!=VK_SUCCESS)throw LSFG::vulkan_error(r,"LSFG private image bind failed");
        VkImageViewCreateInfo viewInfo{};viewInfo.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image=rawImage;viewInfo.viewType=VK_IMAGE_VIEW_TYPE_2D;viewInfo.format=fmt;
        viewInfo.subresourceRange={aspect,0,1,0,1};
        r=vkCreateImageView(dev,&viewInfo,nullptr,&rawView);
        if(r!=VK_SUCCESS)throw LSFG::vulkan_error(r,"LSFG image view failed");
    }catch(...) {
        if(rawView)vkDestroyImageView(dev,rawView,nullptr);
        if(rawImage)vkDestroyImage(dev,rawImage,nullptr);
        if(rawMemory)vkFreeMemory(dev,rawMemory,nullptr);
        throw;
    }
    // Dependencies keep view -> image -> allocation alive across wrapper copies.
    layout=std::make_shared<VkImageLayout>(VK_IMAGE_LAYOUT_UNDEFINED);
    memory=std::shared_ptr<VkDeviceMemory>(new VkDeviceMemory(rawMemory),[dev](auto *p){vkFreeMemory(dev,*p,nullptr);delete p;});
    image=std::shared_ptr<VkImage>(new VkImage(rawImage),[dev,backing=memory](auto *p){vkDestroyImage(dev,*p,nullptr);delete p;});
    view=std::shared_ptr<VkImageView>(new VkImageView(rawView),[dev,owner=image](auto *p){vkDestroyImageView(dev,*p,nullptr);delete p;});
}
Image::Image(const Device&,VkExtent2D,VkFormat,VkImageUsageFlags,VkImageAspectFlags,int) {
    throw std::runtime_error("Native LSFG adapter does not import FD images");
}
#ifdef __ANDROID__
Image::Image(const Device&,VkExtent2D,VkFormat,VkImageUsageFlags,VkImageAspectFlags,AHardwareBuffer*) {
    throw std::runtime_error("Native LSFG adapter does not import AHB images");
}
#endif
