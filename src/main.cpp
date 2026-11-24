#include "vulkan/VulkanApp.h"
#include <iostream>

int main() {
    vkb::VulkanApp app(800, 600, "Vulkan Base Framework");

    try {
        app.run();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
