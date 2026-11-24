#pragma once

#include <vector>
#include <cstdint>

namespace vkb {

// 顶点布局：Position(xyz) + UV(uv) + Color(rgb)
struct WaterVertex {
    float x, y, z;
    float u, v;
    float r, g, b;
};

// 生成一个 XZ 平面上的水面网格（三角形带）
// resolution: 网格分辨率 N x N
void generateWaterPlane(
    std::vector<WaterVertex>& vertices,
    std::vector<uint32_t>& indices,
    float size = 40.0f,
    uint32_t resolution = 64);

// CPU 端程序化生成水纹理（蓝色渐变 + Perlin 噪声 + 高光斑点）
// 返回 RGBA 字节数组，格式从上到下
std::vector<uint8_t> generateWaterTexture(
    uint32_t width = 512,
    uint32_t height = 512);

} // namespace vkb
