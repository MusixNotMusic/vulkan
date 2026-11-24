// 水纹理/Descriptor/Uniform 辅助实现
// 和 VulkanApp.cpp 分离，避免单一文件过大

#include "vulkan/VulkanApp.h"
#include "vulkan/WaterMesh.h"
#include "vulkan/VulkanResources.h"

#include <cmath>
#include <cstring>
#include <stdexcept>
#include <iostream>

namespace vkb {

// ============================================================================
// 矩阵工具（4x4 列主序，符合 GLSL std140）
// ============================================================================

namespace {

struct Mat4 {
    float m[16];
};

Mat4 mat4Identity() {
    Mat4 r{};
    r.m[0]  = 1; r.m[5]  = 1; r.m[10] = 1; r.m[15] = 1;
    return r;
}

Mat4 mat4Perspective(float fovY, float aspect, float nearZ, float farZ) {
    Mat4 r = {};
    float f = 1.0f / std::tan(fovY * 0.5f);
    r.m[0]  = f / aspect;
    r.m[5]  = f;
    r.m[10] = (farZ + nearZ) / (nearZ - farZ);
    r.m[11] = -1.0f;
    r.m[14] = (2.0f * farZ * nearZ) / (nearZ - farZ);
    return r;
}

Mat4 mat4LookAt(float eyeX, float eyeY, float eyeZ,
                float cx,  float cy,  float cz,
                float ux,  float uy,  float uz) {
    // forward = normalize(center - eye)
    float fx = cx - eyeX, fy = cy - eyeY, fz = cz - eyeZ;
    float flen = std::sqrt(fx*fx + fy*fy + fz*fz);
    fx /= flen; fy /= flen; fz /= flen;

    // right = normalize(cross(forward, up))
    float rx = fy*uz - fz*uy;
    float ry = fz*ux - fx*uz;
    float rz = fx*uy - fy*ux;
    float rlen = std::sqrt(rx*rx + ry*ry + rz*rz);
    rx /= rlen; ry /= rlen; rz /= rlen;

    // up = cross(right, forward)
    float uxp = ry*fz - rz*fy;
    float uyp = rz*fx - rx*fz;
    float uzp = rx*fy - ry*fx;

    Mat4 r = {};
    r.m[0]  = rx;
    r.m[4]  = uxp;
    r.m[8]  = -fx;
    r.m[12] = -(rx*eyeX + uxp*eyeY - fx*eyeZ);

    r.m[1]  = ry;
    r.m[5]  = uyp;
    r.m[9]  = -fy;
    r.m[13] = -(ry*eyeX + uyp*eyeY - fy*eyeZ);

    r.m[2]  = rz;
    r.m[6]  = uzp;
    r.m[10] = -fz;
    r.m[14] = -(rz*eyeX + uzp*eyeY - fz*eyeZ);

    r.m[3]  = 0; r.m[7] = 0; r.m[11] = 0; r.m[15] = 1;
    return r;
}

Mat4 mat4RotateY(float radians) {
    Mat4 r = mat4Identity();
    float c = std::cos(radians);
    float s = std::sin(radians);
    r.m[0]  =  c; r.m[2]  =  s;
    r.m[8]  = -s; r.m[10] =  c;
    return r;
}

// 拷贝 raw 到 16 字节对齐缓冲区
void copyMatrix(float* dst, const Mat4& src) {
    memcpy(dst, src.m, 16 * sizeof(float));
}

} // anon namespace

// ============================================================================
// 水纹理创建
// ============================================================================

void VulkanApp::createWaterTexture() {
    m_waterTexWidth = 512;
    m_waterTexHeight = 512;

    auto pixels = generateWaterTexture(m_waterTexWidth, m_waterTexHeight);
    VkDeviceSize size = m_waterTexWidth * m_waterTexHeight * 4;

    // Staging buffer
    VkBuffer stagingBuffer = createBuffer(m_device, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    VkDeviceMemory stagingMem = allocateBufferMemory(
        m_physicalDevice, m_device, stagingBuffer,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    void* mapped;
    vkMapMemory(m_device, stagingMem, 0, size, 0, &mapped);
    memcpy(mapped, pixels.data(), size);
    vkUnmapMemory(m_device, stagingMem);

    // GPU Image
    m_waterTexture = createImage(m_device, m_waterTexWidth, m_waterTexHeight,
        VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    m_waterTextureMemory = allocateImageMemory(
        m_physicalDevice, m_device, m_waterTexture,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    // Transition: UNDEFINED → TRANSFER_DST
    transitionImageLayout(m_device, m_waterTexture, VK_FORMAT_R8G8B8A8_SRGB,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        m_commandPool, m_graphicsQueue);

    // Copy staging buffer → image
    copyBufferToImage(m_device, stagingBuffer, m_waterTexture,
        m_waterTexWidth, m_waterTexHeight, m_commandPool, m_graphicsQueue);

    // Transition: TRANSFER_DST → SHADER_READ
    transitionImageLayout(m_device, m_waterTexture, VK_FORMAT_R8G8B8A8_SRGB,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        m_commandPool, m_graphicsQueue);

    // ImageView + Sampler
    m_waterTextureImageView = createImageView(m_device, m_waterTexture, VK_FORMAT_R8G8B8A8_SRGB);
    m_waterTextureSampler = createSampler(m_device);

    // 清理 staging
    vkDestroyBuffer(m_device, stagingBuffer, nullptr);
    vkFreeMemory(m_device, stagingMem, nullptr);

    std::cout << "[Water] Texture created: " << m_waterTexWidth << "x" << m_waterTexHeight << std::endl;
}

// ============================================================================
// 水面网格
// ============================================================================

void VulkanApp::createWaterMesh() {
    std::vector<WaterVertex> vertices;
    std::vector<uint32_t> indices;
    // 60x60 单位、192x192 分辨率：保证 Gerstner 波有足够顶点密度
    generateWaterPlane(vertices, indices, 60.0f, 192);

    m_vertexCount = static_cast<uint32_t>(vertices.size());
    m_indexCount  = static_cast<uint32_t>(indices.size());

    VkDeviceSize vertexSize = sizeof(WaterVertex) * vertices.size();
    VkDeviceSize indexSize  = sizeof(uint32_t) * indices.size();

    // Vertex Buffer
    m_vertexBuffer = createBuffer(m_device, vertexSize,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    m_vertexBufferMemory = allocateBufferMemory(
        m_physicalDevice, m_device, m_vertexBuffer, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    uploadToDeviceBuffer(m_physicalDevice, m_device, m_vertexBuffer,
        vertices.data(), vertexSize, m_commandPool, m_graphicsQueue);

    // Index Buffer
    m_indexBuffer = createBuffer(m_device, indexSize,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    m_indexBufferMemory = allocateBufferMemory(
        m_physicalDevice, m_device, m_indexBuffer, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    uploadToDeviceBuffer(m_physicalDevice, m_device, m_indexBuffer,
        indices.data(), indexSize, m_commandPool, m_graphicsQueue);

    std::cout << "[Water] Mesh: " << m_vertexCount << " verts, " << m_indexCount << " indices" << std::endl;
}

// ============================================================================
// Uniform Buffers (每帧一份)
// ============================================================================

void VulkanApp::createUniformBuffers() {
    m_uniformBuffers.resize(MAX_FRAMES_IN_FLIGHT);
    m_uniformBufferMemories.resize(MAX_FRAMES_IN_FLIGHT);
    m_uniformBufferMapped.resize(MAX_FRAMES_IN_FLIGHT);

    VkDeviceSize size = sizeof(WaterUniformData);
    // 对齐到 minUniformBufferOffsetAlignment
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(m_physicalDevice, &props);
    size = (size + props.limits.minUniformBufferOffsetAlignment - 1) /
           props.limits.minUniformBufferOffsetAlignment *
           props.limits.minUniformBufferOffsetAlignment;

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        m_uniformBuffers[i] = createBuffer(m_device, size, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        m_uniformBufferMemories[i] = allocateBufferMemory(
            m_physicalDevice, m_device, m_uniformBuffers[i],
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        vkMapMemory(m_device, m_uniformBufferMemories[i], 0, size, 0, &m_uniformBufferMapped[i]);
    }
}

// ============================================================================
// Descriptor Set Layout
// ============================================================================

void VulkanApp::createDescriptorSetLayout() {
    VkDescriptorSetLayoutBinding uboBinding{};
    uboBinding.binding = 0;
    uboBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uboBinding.descriptorCount = 1;
    uboBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    uboBinding.pImmutableSamplers = nullptr;

    VkDescriptorSetLayoutBinding samplerBinding{};
    samplerBinding.binding = 1;
    samplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    samplerBinding.descriptorCount = 1;
    samplerBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    samplerBinding.pImmutableSamplers = nullptr;

    // FFT 海洋输出: 高度+位移 / 位移+斜率 / 斜率（顶点位移 + 片段法线）
    VkDescriptorSetLayoutBinding fftBindings[3];
    for (int i = 0; i < 3; ++i) {
        fftBindings[i].binding = 2 + i;
        fftBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        fftBindings[i].descriptorCount = 1;
        fftBindings[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        fftBindings[i].pImmutableSamplers = nullptr;
    }

    VkDescriptorSetLayoutBinding bindings[] = { uboBinding, samplerBinding,
        fftBindings[0], fftBindings[1], fftBindings[2] };

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 5;
    layoutInfo.pBindings = bindings;

    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_descriptorSetLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create descriptor set layout!");
    }
}

void VulkanApp::createDescriptorPool() {
    VkDescriptorPoolSize poolSizes[] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT) },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT) * 4 },
    };

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = 0;
    poolInfo.maxSets = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);
    poolInfo.poolSizeCount = 2;
    poolInfo.pPoolSizes = poolSizes;

    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create descriptor pool!");
    }
}

void VulkanApp::createDescriptorSets() {
    std::vector<VkDescriptorSetLayout> layouts(MAX_FRAMES_IN_FLIGHT, m_descriptorSetLayout);

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);
    allocInfo.pSetLayouts = layouts.data();

    m_descriptorSets.resize(MAX_FRAMES_IN_FLIGHT);
    if (vkAllocateDescriptorSets(m_device, &allocInfo, m_descriptorSets.data()) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate descriptor sets!");
    }

    // 为每一帧的 DS 写 UBO 和 ImageSampler
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = m_uniformBuffers[i];
        bufferInfo.offset = 0;
        bufferInfo.range = sizeof(WaterUniformData);

        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfo.imageView = m_waterTextureImageView;
        imageInfo.sampler = m_waterTextureSampler;

        // FFT 海洋输出纹理（compute 保持 GENERAL layout）
        VkDescriptorImageInfo fftInfo[3];
        fftInfo[0] = { m_fftOcean->sampler(), m_fftOcean->heightDxView(), VK_IMAGE_LAYOUT_GENERAL };
        fftInfo[1] = { m_fftOcean->sampler(), m_fftOcean->dzSxView(),     VK_IMAGE_LAYOUT_GENERAL };
        fftInfo[2] = { m_fftOcean->sampler(), m_fftOcean->szView(),       VK_IMAGE_LAYOUT_GENERAL };

        std::vector<VkWriteDescriptorSet> descriptorWrites(5);

        descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrites[0].dstSet = m_descriptorSets[i];
        descriptorWrites[0].dstBinding = 0;
        descriptorWrites[0].dstArrayElement = 0;
        descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        descriptorWrites[0].descriptorCount = 1;
        descriptorWrites[0].pBufferInfo = &bufferInfo;

        descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrites[1].dstSet = m_descriptorSets[i];
        descriptorWrites[1].dstBinding = 1;
        descriptorWrites[1].dstArrayElement = 0;
        descriptorWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descriptorWrites[1].descriptorCount = 1;
        descriptorWrites[1].pImageInfo = &imageInfo;

        for (int b = 0; b < 3; ++b) {
            descriptorWrites[2 + b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptorWrites[2 + b].dstSet = m_descriptorSets[i];
            descriptorWrites[2 + b].dstBinding = 2 + b;
            descriptorWrites[2 + b].dstArrayElement = 0;
            descriptorWrites[2 + b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            descriptorWrites[2 + b].descriptorCount = 1;
            descriptorWrites[2 + b].pImageInfo = &fftInfo[b];
        }

        vkUpdateDescriptorSets(m_device, 5, descriptorWrites.data(), 0, nullptr);
    }
}

// ============================================================================
// 每帧更新 Uniform Buffer
// ============================================================================

void VulkanApp::updateUniformBuffers() {
    float time = std::chrono::duration<float>(
        std::chrono::high_resolution_clock::now() - m_startTime).count();

    // 模型固定不旋转（Gerstner 波方向保持世界一致）
    Mat4 model = mat4Identity();

    // 相机：水面上方拉远，展示波面起伏
    const float eyeX = 26.0f, eyeY = 14.0f, eyeZ = 26.0f;
    Mat4 view = mat4LookAt(
        eyeX, eyeY, eyeZ,      // eye
        0.0f, 0.0f, 0.0f,      // center
        0.0f, 1.0f, 0.0f);     // up

    float aspect = static_cast<float>(m_swapchainExtent.width) /
                   static_cast<float>(std::max(1u, m_swapchainExtent.height));
    Mat4 proj = mat4Perspective(60.0f * 3.14159265f / 180.0f, aspect, 0.1f, 500.0f);

    WaterUniformData data{};
    copyMatrix(data.model, model);
    copyMatrix(data.view,  view);
    copyMatrix(data.proj,  proj);
    data.time = time;
    data.cameraPos[0] = eyeX;
    data.cameraPos[1] = eyeY;
    data.cameraPos[2] = eyeZ;
    // 太阳方向（shader 内 normalize）
    data.sunDir[0] = 0.45f;
    data.sunDir[1] = 0.65f;
    data.sunDir[2] = 0.30f;

    memcpy(m_uniformBufferMapped[m_currentFrame], &data, sizeof(data));
}

// ============================================================================
// Cleanup helpers
// ============================================================================

void VulkanApp::cleanupWaterTexture() {
    if (m_waterTextureImageView != VK_NULL_HANDLE) {
        vkDestroyImageView(m_device, m_waterTextureImageView, nullptr);
        m_waterTextureImageView = VK_NULL_HANDLE;
    }
    if (m_waterTextureSampler != VK_NULL_HANDLE) {
        vkDestroySampler(m_device, m_waterTextureSampler, nullptr);
        m_waterTextureSampler = VK_NULL_HANDLE;
    }
    if (m_waterTexture != VK_NULL_HANDLE) {
        vkDestroyImage(m_device, m_waterTexture, nullptr);
        m_waterTexture = VK_NULL_HANDLE;
    }
    if (m_waterTextureMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_waterTextureMemory, nullptr);
        m_waterTextureMemory = VK_NULL_HANDLE;
    }
}

void VulkanApp::cleanupWaterMesh() {
    if (m_vertexBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_device, m_vertexBuffer, nullptr);
        m_vertexBuffer = VK_NULL_HANDLE;
    }
    if (m_vertexBufferMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_vertexBufferMemory, nullptr);
        m_vertexBufferMemory = VK_NULL_HANDLE;
    }
    if (m_indexBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(m_device, m_indexBuffer, nullptr);
        m_indexBuffer = VK_NULL_HANDLE;
    }
    if (m_indexBufferMemory != VK_NULL_HANDLE) {
        vkFreeMemory(m_device, m_indexBufferMemory, nullptr);
        m_indexBufferMemory = VK_NULL_HANDLE;
    }
}

void VulkanApp::cleanupUniformBuffers() {
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        if (m_uniformBufferMapped[i]) {
            vkUnmapMemory(m_device, m_uniformBufferMemories[i]);
        }
        if (m_uniformBuffers[i] != VK_NULL_HANDLE) {
            vkDestroyBuffer(m_device, m_uniformBuffers[i], nullptr);
        }
        if (m_uniformBufferMemories[i] != VK_NULL_HANDLE) {
            vkFreeMemory(m_device, m_uniformBufferMemories[i], nullptr);
        }
    }
    m_uniformBuffers.clear();
    m_uniformBufferMemories.clear();
    m_uniformBufferMapped.clear();
}

void VulkanApp::cleanupDescriptors() {
    if (m_descriptorPool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
        m_descriptorPool = VK_NULL_HANDLE;
    }
    if (m_descriptorSetLayout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(m_device, m_descriptorSetLayout, nullptr);
        m_descriptorSetLayout = VK_NULL_HANDLE;
    }
}

} // namespace vkb
