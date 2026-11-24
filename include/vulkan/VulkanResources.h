#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <string>

namespace vkb {

// CPU → GPU Buffer 上传工具
VkBuffer createBuffer(
    VkDevice device,
    VkDeviceSize size,
    VkBufferUsageFlags usage);

VkDeviceMemory allocateBufferMemory(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VkBuffer buffer,
    VkMemoryPropertyFlags properties);

void copyBuffer(
    VkDevice device,
    VkBuffer src,
    VkBuffer dst,
    VkDeviceSize size,
    VkCommandPool commandPool,
    VkQueue graphicsQueue);

// 直接用 staging buffer 上传数据到 GPU buffer
void uploadToDeviceBuffer(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VkBuffer dstBuffer,
    const void* data,
    VkDeviceSize size,
    VkCommandPool commandPool,
    VkQueue graphicsQueue);

// CPU → GPU Image 上传工具
VkImage createImage(
    VkDevice device,
    uint32_t width,
    uint32_t height,
    VkFormat format,
    VkImageUsageFlags usage);

VkDeviceMemory allocateImageMemory(
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    VkImage image,
    VkMemoryPropertyFlags properties);

void transitionImageLayout(
    VkDevice device,
    VkImage image,
    VkFormat format,
    VkImageLayout oldLayout,
    VkImageLayout newLayout,
    VkCommandPool commandPool,
    VkQueue graphicsQueue);

void copyBufferToImage(
    VkDevice device,
    VkBuffer buffer,
    VkImage image,
    uint32_t width,
    uint32_t height,
    VkCommandPool commandPool,
    VkQueue graphicsQueue);

VkImageView createImageView(
    VkDevice device,
    VkImage image,
    VkFormat format,
    VkImageAspectFlags aspectMask = VK_IMAGE_ASPECT_COLOR_BIT);

VkSampler createSampler(VkDevice device);

// 内存查找辅助
uint32_t findMemoryType(
    VkPhysicalDevice physicalDevice,
    uint32_t typeFilter,
    VkMemoryPropertyFlags properties);

} // namespace vkb
