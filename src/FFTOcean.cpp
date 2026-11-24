// ============================================================================
// FFT 海洋实现 — Tessendorf 频域法
// 详见 include/vulkan/FFTOcean.h 顶部注释
// ============================================================================

#include "vulkan/FFTOcean.h"
#include "vulkan/VulkanResources.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <random>
#include <stdexcept>

namespace vkb {

namespace {

// 图像布局屏障辅助
void imageBarrier(VkCommandBuffer cmd, VkImage image,
                  VkImageLayout oldLayout, VkImageLayout newLayout,
                  VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                  VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = oldLayout;
    b.newLayout = newLayout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.baseMipLevel = 0;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.baseArrayLayer = 0;
    b.subresourceRange.layerCount = 1;
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

// 提交一次性 command buffer
void submitOneTime(VkDevice device, VkCommandPool pool, VkQueue queue,
                   const std::function<void(VkCommandBuffer)>& record) {
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = pool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device, &allocInfo, &cmd);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd, &beginInfo);

    record(cmd);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);

    vkFreeCommandBuffers(device, pool, 1, &cmd);
}

} // anon namespace

// ============================================================================
// 构造 / 析构
// ============================================================================

FFTOcean::FFTOcean(VkPhysicalDevice physicalDevice, VkDevice device,
                   VkCommandPool commandPool, VkQueue graphicsQueue,
                   const std::string& shaderDir)
    : m_physicalDevice(physicalDevice)
    , m_device(device)
    , m_commandPool(commandPool)
    , m_queue(graphicsQueue)
    , m_shaderDir(shaderDir) {
    createDataTextures();
    uploadInitialData();
    m_sampler = createSampler(m_device);
    createDescriptorResources();
    createPipelines();

    std::cout << "[FFTOcean] N=" << N << ", patch=" << PATCH_SIZE
              << "m, dispatches/frame=" << (1 + 3 * LOG_N * 2) << std::endl;
}

FFTOcean::~FFTOcean() {
    if (m_device == VK_NULL_HANDLE) return;

    if (m_spectrumPipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, m_spectrumPipeline, nullptr);
    if (m_fftPipeline != VK_NULL_HANDLE) vkDestroyPipeline(m_device, m_fftPipeline, nullptr);
    if (m_spectrumPipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_device, m_spectrumPipelineLayout, nullptr);
    if (m_fftPipelineLayout != VK_NULL_HANDLE) vkDestroyPipelineLayout(m_device, m_fftPipelineLayout, nullptr);
    if (m_descriptorPool != VK_NULL_HANDLE) vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
    if (m_spectrumLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(m_device, m_spectrumLayout, nullptr);
    if (m_fftLayout != VK_NULL_HANDLE) vkDestroyDescriptorSetLayout(m_device, m_fftLayout, nullptr);

    if (m_sampler != VK_NULL_HANDLE) vkDestroySampler(m_device, m_sampler, nullptr);

    auto destroyImg = [&](VkImageView& view, VkImage& image, VkDeviceMemory& mem) {
        if (view != VK_NULL_HANDLE) vkDestroyImageView(m_device, view, nullptr);
        if (image != VK_NULL_HANDLE) vkDestroyImage(m_device, image, nullptr);
        if (mem != VK_NULL_HANDLE) vkFreeMemory(m_device, mem, nullptr);
        view = VK_NULL_HANDLE; image = VK_NULL_HANDLE; mem = VK_NULL_HANDLE;
    };
    destroyImg(m_h0View, m_h0Image, m_h0Memory);
    destroyImg(m_butterflyView, m_butterflyImage, m_butterflyMemory);
    for (int i = 0; i < 3; ++i) {
        destroyImg(m_viewsA[i], m_imagesA[i], m_memoriesA[i]);
        destroyImg(m_viewsB[i], m_imagesB[i], m_memoriesB[i]);
    }
}

// ============================================================================
// 数据纹理创建（全部保持 GENERAL layout）
// ============================================================================

void FFTOcean::createDataTextures() {
    const VkImageUsageFlags staticUsage =
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    const VkImageUsageFlags waveUsage =
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    // h0 频谱纹理
    m_h0Image = createImage(m_device, N, N, VK_FORMAT_R32G32B32A32_SFLOAT, staticUsage);
    m_h0Memory = allocateImageMemory(m_physicalDevice, m_device, m_h0Image,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    m_h0View = createImageView(m_device, m_h0Image, VK_FORMAT_R32G32B32A32_SFLOAT);

    // 蝴蝶查找表 (N × log2N)
    m_butterflyImage = createImage(m_device, N, LOG_N, VK_FORMAT_R32G32B32A32_SFLOAT, staticUsage);
    m_butterflyMemory = allocateImageMemory(m_physicalDevice, m_device, m_butterflyImage,
                                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    m_butterflyView = createImageView(m_device, m_butterflyImage, VK_FORMAT_R32G32B32A32_SFLOAT);

    // 3 组 ping-pong 纹理对
    for (int i = 0; i < 3; ++i) {
        m_imagesA[i] = createImage(m_device, N, N, VK_FORMAT_R32G32B32A32_SFLOAT, waveUsage);
        m_memoriesA[i] = allocateImageMemory(m_physicalDevice, m_device, m_imagesA[i],
                                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        m_viewsA[i] = createImageView(m_device, m_imagesA[i], VK_FORMAT_R32G32B32A32_SFLOAT);

        m_imagesB[i] = createImage(m_device, N, N, VK_FORMAT_R32G32B32A32_SFLOAT, waveUsage);
        m_memoriesB[i] = allocateImageMemory(m_physicalDevice, m_device, m_imagesB[i],
                                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        m_viewsB[i] = createImageView(m_device, m_imagesB[i], VK_FORMAT_R32G32B32A32_SFLOAT);
    }
}

// ============================================================================
// CPU 生成 h0 频谱（Phillips 谱 + Box-Muller/正态高斯 + 方向扩散 + 小波抑制）
// ============================================================================

void FFTOcean::generateInitialSpectrum(std::vector<float>& texel) const {
    std::vector<float> h0rg(size_t(N) * N * 2, 0.0f);

    const float g = 9.81f;
    const float windSpeed = 10.0f;                  // 风速 (m/s)
    const float Lp = windSpeed * windSpeed / g;     // Phillips 最大波长参数
    const float smallSuppress = 0.05f;              // 小波抑制因子 l
    const float wx = 1.0f, wz = 0.35f;              // 风向（内部归一化）
    const float wLen = std::sqrt(wx * wx + wz * wz);
    const float PI = 3.14159265358979f;

    std::mt19937 rng(20261124u);
    std::normal_distribution<float> gauss(0.0f, 1.0f);

    double sumP = 0.0;
    for (uint32_t m = 0; m < N; ++m) {
        for (uint32_t n = 0; n < N; ++n) {
            float kn = static_cast<float>(static_cast<int>(n) - static_cast<int>(N / 2));
            float km = static_cast<float>(static_cast<int>(m) - static_cast<int>(N / 2));
            float kx = 2.0f * PI * kn / PATCH_SIZE;
            float kz = 2.0f * PI * km / PATCH_SIZE;
            float k = std::sqrt(kx * kx + kz * kz);

            size_t i2 = (size_t(m) * N + n) * 2;
            if (k < 1e-6f) { h0rg[i2] = h0rg[i2 + 1] = 0.0f; continue; }

            float dx = kx / k, dz = kz / k;
            float d = dx * (wx / wLen) + dz * (wz / wLen);   // k̂·ŵ

            // Phillips 谱: A·e^{-1/(kLp)²}/k⁴·|k̂·ŵ|²·e^{-(kl)²}（A 归一化吸收）
            float kl = k * Lp;
            float p = std::exp(-1.0f / (kl * kl)) / (k * k * k * k)
                      * (d * d) * std::exp(-(k * smallSuppress) * (k * smallSuppress));
            // 逆风小波保留少量（避免海面过于"单方向"）
            if (d < 0.0f) p *= 0.10f;

            // h0(k) = ξ1·√(P/2) + i·ξ2·√(P/2)
            float amp = std::sqrt(p * 0.5f);
            h0rg[i2]     = gauss(rng) * amp;
            h0rg[i2 + 1] = gauss(rng) * amp;
            sumP += p;
        }
    }

    // 归一化: 波高 RMS → 0.45m；同时乘 1/N² 抵消 IFFT 无缩放
    float scale = static_cast<float>(0.45 / std::sqrt(sumP)) / float(N * N);
    for (size_t i = 0; i < size_t(N) * N * 2; ++i) h0rg[i] *= scale;

    // 打包 RGBA: rg = h0(k), ba = conj(h0(-k))（-k 在纹理 (N-n, N-m) 处）
    texel.assign(size_t(N) * N * 4, 0.0f);
    for (uint32_t m = 0; m < N; ++m) {
        for (uint32_t n = 0; n < N; ++n) {
            uint32_t n2 = (N - n) % N;
            uint32_t m2 = (N - m) % N;
            size_t i4 = (size_t(m) * N + n) * 4;
            size_t j2 = (size_t(m2) * N + n2) * 2;
            texel[i4 + 0] = h0rg[(size_t(m) * N + n) * 2 + 0];
            texel[i4 + 1] = h0rg[(size_t(m) * N + n) * 2 + 1];
            texel[i4 + 2] = h0rg[j2 + 0];
            texel[i4 + 3] = -h0rg[j2 + 1];  // 共轭
        }
    }
}

// ============================================================================
// CPU 生成蝴蝶查找表 (Cooley-Tukey DIT, stage 0 隐式位反转)
// texel = (idxUpper, idxLower, twiddle.r, twiddle.i)
// ============================================================================

void FFTOcean::generateButterfly(std::vector<float>& texel) const {
    texel.assign(size_t(N) * LOG_N * 4, 0.0f);

    auto bitReverse = [](uint32_t x, uint32_t bits) {
        uint32_t r = 0;
        for (uint32_t i = 0; i < bits; ++i) { r = (r << 1) | (x & 1u); x >>= 1; }
        return r;
    };

    const float PI = 3.14159265358979f;
    for (uint32_t stage = 0; stage < LOG_N; ++stage) {
        uint32_t span = 1u << stage;
        uint32_t period = span << 1;
        for (uint32_t x = 0; x < N; ++x) {
            bool lower = (x & span) != 0;
            uint32_t offs = x & (span - 1);   // 上下两半的 twiddle 指数一致

            uint32_t idxU = lower ? x - span : x;
            uint32_t idxL = lower ? x : x + span;
            if (stage == 0) {  // 首段读取位反转下标 → 输入无需真正重排
                idxU = bitReverse(idxU, LOG_N);
                idxL = bitReverse(idxL, LOG_N);
            }

            // 逆变换 (IFFT): twiddle = e^{+2πi·offs/period}，下半带负号
            float angle = 2.0f * PI * float(offs) / float(period);
            float twR = std::cos(angle), twI = std::sin(angle);
            if (lower) { twR = -twR; twI = -twI; }

            size_t i4 = (size_t(stage) * N + x) * 4;
            texel[i4 + 0] = float(idxU);
            texel[i4 + 1] = float(idxL);
            texel[i4 + 2] = twR;
            texel[i4 + 3] = twI;
        }
    }
}

// ============================================================================
// 初始上传（staging → 纹理，全部转入 GENERAL layout）
// ============================================================================

void FFTOcean::uploadInitialData() {
    std::vector<float> h0Texel, butterflyTexel;
    generateInitialSpectrum(h0Texel);
    generateButterfly(butterflyTexel);

    VkDeviceSize h0Size = VkDeviceSize(h0Texel.size() * sizeof(float));
    VkDeviceSize bfSize = VkDeviceSize(butterflyTexel.size() * sizeof(float));

    auto makeStaging = [&](VkDeviceSize size, const void* data,
                           VkBuffer& buf, VkDeviceMemory& mem) {
        buf = createBuffer(m_device, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        mem = allocateBufferMemory(m_physicalDevice, m_device, buf,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        void* mapped;
        vkMapMemory(m_device, mem, 0, size, 0, &mapped);
        memcpy(mapped, data, size);
        vkUnmapMemory(m_device, mem);
    };

    VkBuffer h0Staging, bfStaging;
    VkDeviceMemory h0StagingMem, bfStagingMem;
    makeStaging(h0Size, h0Texel.data(), h0Staging, h0StagingMem);
    makeStaging(bfSize, butterflyTexel.data(), bfStaging, bfStagingMem);

    // 1) h0/蝴蝶 → TRANSFER_DST；A/B → GENERAL（后续 compute 直接读写）
    submitOneTime(m_device, m_commandPool, m_queue, [&](VkCommandBuffer cmd) {
        imageBarrier(cmd, m_h0Image,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        imageBarrier(cmd, m_butterflyImage,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        for (int i = 0; i < 3; ++i) {
            imageBarrier(cmd, m_imagesA[i],
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                0, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
            imageBarrier(cmd, m_imagesB[i],
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                0, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        }
    });

    // 2) 上传内容
    copyBufferToImage(m_device, h0Staging, m_h0Image, N, N, m_commandPool, m_queue);
    copyBufferToImage(m_device, bfStaging, m_butterflyImage, N, LOG_N, m_commandPool, m_queue);

    // 3) h0/蝴蝶 → GENERAL
    submitOneTime(m_device, m_commandPool, m_queue, [&](VkCommandBuffer cmd) {
        imageBarrier(cmd, m_h0Image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        imageBarrier(cmd, m_butterflyImage,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    });

    vkDestroyBuffer(m_device, h0Staging, nullptr);
    vkFreeMemory(m_device, h0StagingMem, nullptr);
    vkDestroyBuffer(m_device, bfStaging, nullptr);
    vkFreeMemory(m_device, bfStagingMem, nullptr);
}

// ============================================================================
// Descriptor
// ============================================================================

void FFTOcean::createDescriptorResources() {
    // spectrum: b0=h0(ro), b1=A[3] 输出
    VkDescriptorSetLayoutBinding spectrumBindings[2] = {};
    spectrumBindings[0] = { 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    spectrumBindings[1] = { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };

    // fft: b0=A[3], b1=B[3], b2=蝴蝶表(ro)
    VkDescriptorSetLayoutBinding fftBindings[3] = {};
    fftBindings[0] = { 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    fftBindings[1] = { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
    fftBindings[2] = { 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;

    layoutInfo.bindingCount = 2;
    layoutInfo.pBindings = spectrumBindings;
    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_spectrumLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create FFT spectrum descriptor layout!");
    }

    layoutInfo.bindingCount = 3;
    layoutInfo.pBindings = fftBindings;
    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_fftLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create FFT descriptor layout!");
    }

    VkDescriptorPoolSize poolSize{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 16 };
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 2;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create FFT descriptor pool!");
    }

    VkDescriptorSetLayout layouts[2] = { m_spectrumLayout, m_fftLayout };
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = m_descriptorPool;
    allocInfo.descriptorSetCount = 2;
    allocInfo.pSetLayouts = layouts;
    VkDescriptorSet sets[2] = {};
    if (vkAllocateDescriptorSets(m_device, &allocInfo, sets) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate FFT descriptor sets!");
    }
    m_spectrumSet = sets[0];
    m_fftSet = sets[1];

    VkDescriptorImageInfo h0Info{ m_sampler, m_h0View, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorImageInfo aInfo[3], bInfo[3], bfInfo{ m_sampler, m_butterflyView, VK_IMAGE_LAYOUT_GENERAL };
    for (int i = 0; i < 3; ++i) {
        aInfo[i] = { m_sampler, m_viewsA[i], VK_IMAGE_LAYOUT_GENERAL };
        bInfo[i] = { m_sampler, m_viewsB[i], VK_IMAGE_LAYOUT_GENERAL };
    }

    VkWriteDescriptorSet writes[5] = {};
    writes[0].dstSet = m_spectrumSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[0].pImageInfo = &h0Info;
    writes[1].dstSet = m_spectrumSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 3;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].pImageInfo = aInfo;
    writes[2].dstSet = m_fftSet;
    writes[2].dstBinding = 0;
    writes[2].descriptorCount = 3;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[2].pImageInfo = aInfo;
    writes[3].dstSet = m_fftSet;
    writes[3].dstBinding = 1;
    writes[3].descriptorCount = 3;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[3].pImageInfo = bInfo;
    writes[4].dstSet = m_fftSet;
    writes[4].dstBinding = 2;
    writes[4].descriptorCount = 1;
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[4].pImageInfo = &bfInfo;
    for (int i = 0; i < 5; ++i) writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;

    vkUpdateDescriptorSets(m_device, 5, writes, 0, nullptr);
}

// ============================================================================
// Compute Pipelines
// ============================================================================

VkShaderModule FFTOcean::createShaderModule(const std::vector<char>& code) const {
    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = code.size();
    info.pCode = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule module;
    if (vkCreateShaderModule(m_device, &info, nullptr, &module) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create FFT shader module!");
    }
    return module;
}

std::vector<char> FFTOcean::readFile(const std::string& path) const {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Failed to open FFT shader: " + path);
    }
    size_t size = size_t(file.tellg());
    std::vector<char> buffer(size);
    file.seekg(0);
    file.read(buffer.data(), size);
    return buffer;
}

void FFTOcean::createPipelines() {
    const VkPushConstantRange pcRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, 16 };

    auto build = [&](VkDescriptorSetLayout layout, const std::string& spv,
                     VkPipelineLayout& pipeLayout, VkPipeline& pipe) {
        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &layout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &pcRange;
        if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &pipeLayout) != VK_SUCCESS) {
            throw std::runtime_error("Failed to create FFT pipeline layout!");
        }

        auto code = readFile(m_shaderDir + "\\" + spv);
        VkShaderModule module = createShaderModule(code);
        VkPipelineShaderStageCreateInfo stage{};
        stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = module;
        stage.pName = "main";

        VkComputePipelineCreateInfo pipeInfo{};
        pipeInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipeInfo.stage = stage;
        pipeInfo.layout = pipeLayout;
        if (vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pipeInfo, nullptr, &pipe) != VK_SUCCESS) {
            vkDestroyShaderModule(m_device, module, nullptr);
            throw std::runtime_error("Failed to create FFT compute pipeline: " + spv);
        }
        vkDestroyShaderModule(m_device, module, nullptr);
    };

    build(m_spectrumLayout, "spectrum.comp.spv", m_spectrumPipelineLayout, m_spectrumPipeline);
    build(m_fftLayout, "fft.comp.spv", m_fftPipelineLayout, m_fftPipeline);
}

// ============================================================================
// 每帧 compute 录制
// ============================================================================

void FFTOcean::recordCompute(VkCommandBuffer cmd, float time) {
    // ---- pass 1: 频谱更新 h(k,t) + Dx/Dz/Sx/Sz ----
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_spectrumPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        m_spectrumPipelineLayout, 0, 1, &m_spectrumSet, 0, nullptr);
    float pc[4] = { time, 0.0f, 0.0f, 0.0f };
    vkCmdPushConstants(cmd, m_spectrumPipelineLayout,
        VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
    vkCmdDispatch(cmd, N / 8, N / 8, 1);

    // storage write → read 全屏障
    VkMemoryBarrier memBarrier{};
    memBarrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    memBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    memBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memBarrier, 0, nullptr, 0, nullptr);

    // ---- pass 2: 3 组纹理 × (横向8 + 纵向8) IFFT，ping-pong ----
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_fftPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
        m_fftPipelineLayout, 0, 1, &m_fftSet, 0, nullptr);

    for (uint32_t t = 0; t < 3; ++t) {
        for (uint32_t dir = 0; dir < 2; ++dir) {  // 0=横向, 1=纵向
            for (uint32_t stage = 0; stage < LOG_N; ++stage) {
                float pc2[4] = { float(stage), dir == 0 ? 1.0f : 0.0f, float(t), 0.0f };
                vkCmdPushConstants(cmd, m_fftPipelineLayout,
                    VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc2), pc2);
                vkCmdDispatch(cmd, N / 8, N / 8, 1);
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memBarrier, 0, nullptr, 0, nullptr);
            }
        }
    }

    // ---- compute → graphics: 顶点/片段着色器采样 IFFT 结果 ----
    memBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    memBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 1, &memBarrier, 0, nullptr, 0, nullptr);
}

} // namespace vkb
