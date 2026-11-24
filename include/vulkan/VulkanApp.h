#pragma once

#include <vulkan/vulkan.h>
#include <GLFW/glfw3.h>

#include <string>
#include <vector>
#include <optional>
#include <memory>
#include <cstdint>
#include <chrono>

#include "vulkan/ShaderWatcher.h"
#include "vulkan/WaterMesh.h"
#include "vulkan/VulkanResources.h"

namespace vkb {

struct QueueFamilyIndices {
    std::optional<uint32_t> graphicsFamily;
    std::optional<uint32_t> presentFamily;
    bool isComplete() const {
        return graphicsFamily.has_value() && presentFamily.has_value();
    }
};

struct SwapchainSupportDetails {
    VkSurfaceCapabilitiesKHR capabilities;
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
};

// MVP + Time Uniform Buffer (std140 布局)
struct WaterUniformData {
    float model[16];
    float view[16];
    float proj[16];
    float time;
    float pad[3]; // 对齐 16 字节
};

class VulkanApp {
public:
    VulkanApp(uint32_t width = 800, uint32_t height = 600, const std::string& title = "Vulkan Water");
    ~VulkanApp();

    void run();

private:
    // ---------- GLFW ----------
    uint32_t m_width;
    uint32_t m_height;
    std::string m_title;
    GLFWwindow* m_window = nullptr;

    // ---------- Vulkan Core ----------
    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;

    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_graphicsQueue = VK_NULL_HANDLE;
    VkQueue m_presentQueue = VK_NULL_HANDLE;

    // ---------- Swapchain ----------
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    VkFormat m_swapchainImageFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D m_swapchainExtent{};
    std::vector<VkImage> m_swapchainImages;
    std::vector<VkImageView> m_swapchainImageViews;
    std::vector<VkFramebuffer> m_swapchainFramebuffers;

    // ---------- Water Texture ----------
    VkImage m_waterTexture = VK_NULL_HANDLE;
    VkDeviceMemory m_waterTextureMemory = VK_NULL_HANDLE;
    VkImageView m_waterTextureImageView = VK_NULL_HANDLE;
    VkSampler m_waterTextureSampler = VK_NULL_HANDLE;
    uint32_t m_waterTexWidth = 0;
    uint32_t m_waterTexHeight = 0;

    // ---------- Water Mesh ----------
    VkBuffer m_vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_vertexBufferMemory = VK_NULL_HANDLE;
    VkBuffer m_indexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_indexBufferMemory = VK_NULL_HANDLE;
    uint32_t m_vertexCount = 0;
    uint32_t m_indexCount = 0;

    // ---------- Uniform Buffer ----------
    std::vector<VkBuffer> m_uniformBuffers;
    std::vector<VkDeviceMemory> m_uniformBufferMemories;
    std::vector<void*> m_uniformBufferMapped;

    // ---------- Descriptors ----------
    VkDescriptorSetLayout m_descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> m_descriptorSets;

    // ---------- Rendering ----------
    VkRenderPass m_renderPass = VK_NULL_HANDLE;
    VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_graphicsPipeline = VK_NULL_HANDLE;

    VkCommandPool m_commandPool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> m_commandBuffers;

    // ---------- Sync ----------
    int MAX_FRAMES_IN_FLIGHT = 3; // 初始值，createSwapchain 后会调整
    std::vector<VkSemaphore> m_imageAvailableSemaphores;
    std::vector<VkSemaphore> m_renderFinishedSemaphores;
    std::vector<VkFence> m_inFlightFences;
    size_t m_currentFrame = 0;

    // ---------- Time ----------
    std::chrono::high_resolution_clock::time_point m_startTime;

    // ---------- Initialization ----------
    void initWindow();
    void initVulkan();
    void createInstance();
    void setupDebugMessenger();
    void createSurface();
    void pickPhysicalDevice();
    void createLogicalDevice();
    void createSwapchain();
    void createImageViews();
    void createRenderPass();
    void createGraphicsPipeline();
    void createFramebuffers();
    void createCommandPool();
    void createCommandBuffers();
    void recordCommandBuffer(uint32_t index);
    void createSyncObjects();

    void createWaterTexture();
    void createWaterMesh();
    void createUniformBuffers();
    void createDescriptorSetLayout();
    void createDescriptorPool();
    void createDescriptorSets();
    void updateUniformBuffers();

    // ---------- Render Loop ----------
    void mainLoop();
    void drawFrame();
    void cleanupSwapchain();
    void recreateSwapchain();

    // ---------- Shader Hot Reload ----------
    ShaderWatcher m_shaderWatcher;
    std::string m_exeDir;
    std::string m_shaderSourceDir = "shaders";
    std::string m_shaderBinDir = "shaders";
    void startShaderWatcher();
    void tryHotReload();
    bool recompileShader(const std::string& srcFile, const std::string& spvOut);
    void destroyGraphicsPipelineOnly();
    void recreateGraphicsPipeline();

    // ---------- Helpers ----------
    std::vector<const char*> getRequiredExtensions() const;
    bool checkValidationLayerSupport() const;
    QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device) const;
    bool isDeviceSuitable(VkPhysicalDevice device) const;
    SwapchainSupportDetails querySwapchainSupport(VkPhysicalDevice device) const;
    VkSurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats) const;
    VkPresentModeKHR chooseSwapPresentMode(const std::vector<VkPresentModeKHR>& availableModes) const;
    VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities) const;
    VkShaderModule createShaderModule(const std::vector<char>& code) const;

    // ---------- Debug ----------
    VKAPI_ATTR static VkBool32 VKAPI_CALL debugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT type,
        const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
        void* pUserData);

    // ---------- Cleanup ----------
    void cleanup();
    void cleanupWaterTexture();
    void cleanupWaterMesh();
    void cleanupUniformBuffers();
    void cleanupDescriptors();

    // ---------- Validation ----------
    bool m_enableValidationLayers = true;
    const std::vector<const char*> m_validationLayers = {
        "VK_LAYER_KHRONOS_validation"
    };
};

} // namespace vkb
