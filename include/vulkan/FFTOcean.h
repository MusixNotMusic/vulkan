#pragma once

// ============================================================================
// FFT 海洋 — Tessendorf 频域法（compute shader 实现，RDR2/GTA6 同类路线）
//
// 每帧流程（recordCompute，录入图形 command buffer，render pass 之前）:
//   1) spectrum.comp : h(k,t) = h0(k)e^{iωt} + h0*(-k)e^{-iωt}
//                      同时算 Dx/Dz（水平位移）与 Sx/Sz（斜率）
//   2) fft.comp      : 3 组纹理 × 横向8 pass + 纵向8 pass 的 2D IFFT
//                      蝴蝶查找表（bit-reversal + twiddle）预生成一次
// 输出 3 张 RGBA32F 纹理（图形管线 LINEAR 采样）:
//   texA0: r=高度 h, b=水平位移 Dx
//   texA1: r=位移 Dz,  b=斜率 Sx=dH/dx
//   texA2: r=斜率 Sz=dH/dz
// ============================================================================

#include <vulkan/vulkan.h>
#include <string>
#include <vector>

namespace vkb {

class FFTOcean {
public:
    static constexpr uint32_t N = 256;          // 频域网格分辨率
    static constexpr uint32_t LOG_N = 8;        // log2(N)
    static constexpr float PATCH_SIZE = 60.0f;  // 世界空间补丁尺寸（米），与水面网格一致

    FFTOcean(VkPhysicalDevice physicalDevice, VkDevice device,
             VkCommandPool commandPool, VkQueue graphicsQueue,
             const std::string& shaderDir);
    ~FFTOcean();

    FFTOcean(const FFTOcean&) = delete;
    FFTOcean& operator=(const FFTOcean&) = delete;

    // 录入 compute 指令（必须在 vkCmdBeginRenderPass 之前调用）
    void recordCompute(VkCommandBuffer cmd, float time);

    // 图形管线采样接口（图像保持 GENERAL layout）
    VkImageView heightDxView() const { return m_viewsA[0]; }  // .r=高度, .b=位移Dx
    VkImageView dzSxView()     const { return m_viewsA[1]; }  // .r=位移Dz, .b=斜率Sx
    VkImageView szView()       const { return m_viewsA[2]; }  // .r=斜率Sz
    VkSampler  sampler()       const { return m_sampler; }

private:
    void createDataTextures();
    void uploadInitialData();
    void createDescriptorResources();
    void createPipelines();

    void generateInitialSpectrum(std::vector<float>& texel) const; // h0 频谱 (N*N*4 floats)
    void generateButterfly(std::vector<float>& texel) const;       // 蝴蝶表 (N*logN*4 floats)

    VkShaderModule createShaderModule(const std::vector<char>& code) const;
    std::vector<char> readFile(const std::string& path) const;

    VkPhysicalDevice m_physicalDevice;
    VkDevice m_device;
    VkCommandPool m_commandPool;
    VkQueue m_queue;
    std::string m_shaderDir;

    // 静态数据纹理（上传一次）
    VkImage m_h0Image = VK_NULL_HANDLE;
    VkDeviceMemory m_h0Memory = VK_NULL_HANDLE;
    VkImageView m_h0View = VK_NULL_HANDLE;

    VkImage m_butterflyImage = VK_NULL_HANDLE;
    VkDeviceMemory m_butterflyMemory = VK_NULL_HANDLE;
    VkImageView m_butterflyView = VK_NULL_HANDLE;

    // 3 组 ping-pong 纹理对（每个 RGBA 装 2 个复数信号）
    VkImage m_imagesA[3] = {};
    VkDeviceMemory m_memoriesA[3] = {};
    VkImageView m_viewsA[3] = {};
    VkImage m_imagesB[3] = {};
    VkDeviceMemory m_memoriesB[3] = {};
    VkImageView m_viewsB[3] = {};

    VkSampler m_sampler = VK_NULL_HANDLE;

    // compute 资源
    VkDescriptorSetLayout m_spectrumLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_fftLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet m_spectrumSet = VK_NULL_HANDLE;
    VkDescriptorSet m_fftSet = VK_NULL_HANDLE;
    VkPipelineLayout m_spectrumPipelineLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_fftPipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_spectrumPipeline = VK_NULL_HANDLE;
    VkPipeline m_fftPipeline = VK_NULL_HANDLE;
};

} // namespace vkb
