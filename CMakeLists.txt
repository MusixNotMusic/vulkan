cmake_minimum_required(VERSION 3.20)
project(VulkanBase LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------- Dependencies ----------
find_package(Vulkan REQUIRED)
find_package(glfw3 CONFIG REQUIRED)

# ---------- Shader compilation ----------
find_program(GLSLANG_VALIDATOR "glslangValidator" PATHS "$ENV{VULKAN_SDK}/Bin" NO_DEFAULT_PATH)
find_program(GLSLC "glslc" PATHS "$ENV{VULKAN_SDK}/Bin" NO_DEFAULT_PATH)

set(SHADER_SRC_DIR "${CMAKE_SOURCE_DIR}/shaders")
set(SHADER_OUT_DIR "${CMAKE_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY ${SHADER_OUT_DIR})

file(GLOB SHADER_SOURCES "${SHADER_SRC_DIR}/*.vert" "${SHADER_SRC_DIR}/*.frag"
                          "${SHADER_SRC_DIR}/*.comp" "${SHADER_SRC_DIR}/*.geom"
                          "${SHADER_SRC_DIR}/*.tesc" "${SHADER_SRC_DIR}/*.tese")

foreach(SHADER_SRC ${SHADER_SOURCES})
    get_filename_component(SHADER_NAME ${SHADER_SRC} NAME)
    set(SHADER_SPIRV "${SHADER_OUT_DIR}/${SHADER_NAME}.spv")
    add_custom_command(
        OUTPUT ${SHADER_SPIRV}
        COMMAND ${GLSLC} ${SHADER_SRC} -o ${SHADER_SPIRV}
        DEPENDS ${SHADER_SRC}
        COMMENT "Compiling shader ${SHADER_NAME}"
    )
    list(APPEND SPIRV_SHADERS ${SHADER_SPIRV})
endforeach()

add_custom_target(Shaders ALL DEPENDS ${SPIRV_SHADERS})

# ---------- Executable ----------
file(GLOB_RECURSE SOURCES "src/*.cpp")
file(GLOB_RECURSE HEADERS "include/*.h")

add_executable(${PROJECT_NAME} ${SOURCES} ${HEADERS})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    $ENV{VULKAN_SDK}/Include
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Vulkan::Vulkan
    glfw
)

if(WIN32)
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        VK_USE_PLATFORM_WIN32_KHR
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
    target_link_libraries(${PROJECT_NAME} PRIVATE dwmapi)
endif()

add_dependencies(${PROJECT_NAME} Shaders)

# Copy shaders to runtime location
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${SHADER_OUT_DIR}
        $<TARGET_FILE_DIR:${PROJECT_NAME}>/shaders
)
