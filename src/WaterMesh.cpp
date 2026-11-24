#include "vulkan/WaterMesh.h"

#include <cmath>
#include <algorithm>
#include <random>

namespace vkb {

// ============================================================================
// 水面网格（XZ 平面上的 Quad Grid，三角带）
// ============================================================================

void generateWaterPlane(
    std::vector<WaterVertex>& vertices,
    std::vector<uint32_t>& indices,
    float size,
    uint32_t resolution)
{
    vertices.clear();
    indices.clear();

    if (resolution < 2) resolution = 2;
    uint32_t N = resolution;
    float halfSize = size * 0.5f;
    float step = size / (N - 1);

    vertices.reserve(N * N);
    for (uint32_t row = 0; row < N; ++row) {
        for (uint32_t col = 0; col < N; ++col) {
            WaterVertex v{};
            v.x = -halfSize + col * step;  // X 轴
            v.y = 0.0f;                    // 水面高度
            v.z = -halfSize + row * step;  // Z 轴
            v.u = static_cast<float>(col) / (N - 1);
            v.v = static_cast<float>(row) / (N - 1);
            v.r = 0.0f;
            v.g = 0.4f;
            v.b = 0.7f;
            vertices.push_back(v);
        }
    }

    // 三角带索引：沿行 zig-zag 走过
    indices.reserve((N - 1) * (2 * N));
    for (uint32_t row = 0; row < N - 1; ++row) {
        if (row & 1) {
            // 反向
            for (int32_t col = N - 1; col >= 0; --col) {
                indices.push_back(row * N + col);
                indices.push_back((row + 1) * N + col);
            }
        } else {
            for (uint32_t col = 0; col < N; ++col) {
                indices.push_back(row * N + col);
                indices.push_back((row + 1) * N + col);
            }
        }
    }
}

// ============================================================================
// 简化 Perlin 噪声（2D）
// ============================================================================

namespace {

// 快速伪随机 hash
inline float hash21(float x, float y) {
    float s = std::sin(x * 127.1f + y * 311.7f) * 43758.5453f;
    return s - std::floor(s);
}

inline float smoothstep(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

inline float smoothstep3(float edge0, float edge1, float x) {
    float t = (x - edge0) / (edge1 - edge0);
    return smoothstep(t);
}

inline float perlin2D(float x, float y) {
    int xi = static_cast<int>(std::floor(x));
    int yi = static_cast<int>(std::floor(y));
    float xf = x - xi;
    float yf = y - yi;

    float tl = hash21(xi, yi);
    float tr = hash21(xi + 1, yi);
    float bl = hash21(xi, yi + 1);
    float br = hash21(xi + 1, yi + 1);

    float u = smoothstep(xf);
    float v = smoothstep(yf);

    float top = tl + u * (tr - tl);
    float bot = bl + u * (br - bl);
    return top + v * (bot - top);
}

// 多 Octave fbm 噪声
inline float fbm2D(float x, float y, int octaves = 4) {
    float value = 0.0f;
    float amp = 0.5f;
    float freq = 1.0f;
    for (int i = 0; i < octaves; ++i) {
        value += amp * perlin2D(x * freq, y * freq);
        freq *= 2.0f;
        amp *= 0.5f;
    }
    return value;
}

} // namespace

// ============================================================================
// 程序化水纹理
// ============================================================================

std::vector<uint8_t> generateWaterTexture(uint32_t width, uint32_t height) {
    std::vector<uint8_t> pixels(width * height * 4);

    // 调色板：浅蓝 → 深蓝 渐变
    auto lerp = [](float a, float b, float t) { return a + t * (b - a); };

    // 基础颜色（深、浅两种蓝色）
    float deepBlue[3]   = { 0.01f, 0.08f, 0.25f };   // 深海蓝
    float midBlue[3]    = { 0.05f, 0.35f, 0.65f };   // 中蓝
    float shallowBlue[3]= { 0.20f, 0.65f, 0.85f };   // 浅海蓝

    // 高光斑点（泡沫色）
    float foam[3] = { 0.95f, 0.98f, 1.0f };

    // 随机种子
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> highlightRand(0.0f, 1.0f);

    for (uint32_t y = 0; y < height; ++y) {
        float v = static_cast<float>(y) / height;
        for (uint32_t x = 0; x < width; ++x) {
            float u = static_cast<float>(x) / width;

            // 坐标（scaled 让噪声有合理的平铺密度）
            float nx = static_cast<float>(x) / 32.0f;
            float ny = static_cast<float>(y) / 32.0f;

            // FBM 噪声（细节纹理）
            float n1 = fbm2D(nx, ny, 4);       // 大尺度
            float n2 = fbm2D(nx * 2.1f + 5.0f, ny * 2.1f + 3.0f, 3); // 小尺度
            float noise = n1 * 0.7f + n2 * 0.3f;

            // 渐变深度（y 从下到上逐渐变浅，模拟浅水到深水）
            float depth = 1.0f - v;
            float depthFactor = smoothstep3(depth - 0.3f, depth + 0.3f, noise);
            depthFactor = std::clamp(depthFactor, 0.0f, 1.0f);

            // 底色：深 ↔ 中 ↔ 浅
            float r = lerp(deepBlue[0], midBlue[0], depthFactor);
            float g = lerp(deepBlue[1], midBlue[1], depthFactor);
            float b = lerp(deepBlue[2], midBlue[2], depthFactor);

            // 高光（小噪声值对应的高光斑点）
            float highlight = smoothstep3(0.75f, 0.95f, noise);
            highlight *= highlightRand(rng);
            highlight = std::min(highlight * 3.0f, 1.0f);

            r = lerp(r, foam[0], highlight);
            g = lerp(g, foam[1], highlight);
            b = lerp(b, foam[2], highlight);

            // 写入 RGBA
            uint32_t idx = (y * width + x) * 4;
            pixels[idx + 0] = static_cast<uint8_t>(std::min(r, 1.0f) * 255);
            pixels[idx + 1] = static_cast<uint8_t>(std::min(g, 1.0f) * 255);
            pixels[idx + 2] = static_cast<uint8_t>(std::min(b, 1.0f) * 255);
            pixels[idx + 3] = 255; // alpha 全不透明
        }
    }

    return pixels;
}

} // namespace vkb
