#version 450
// FFT 海洋顶点位移 (Tessendorf) — 采样 IFFT 结果纹理
// 高度 h 与水平位移 Dx/Dz 均为物理量纲（米），来自 compute 频域计算
// 参考: docs/gta6-water-research.md（工业级 FFT 路线）

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec3 inColor;

layout(binding = 0) uniform UniformData {
    mat4 model;
    mat4 view;
    mat4 proj;
    float time;
    vec3 cameraPos;
    vec3 sunDir;
};

layout(binding = 2) uniform sampler2D texHeightDx;  // r=高度 h, b=水平位移 Dx
layout(binding = 3) uniform sampler2D texDzSx;      // r=水平位移 Dz, b=斜率 Sx

layout(location = 0) out vec2 fragUV;
layout(location = 1) out vec3 fragColor;
layout(location = 2) out vec3 worldPos;
layout(location = 3) out vec3 worldNormal;

// Choppy 缩放: FFT 水平位移偏保守，适当增强波峰尖锐感
const float CHOPPY = 0.85;

void main() {
    vec4 hd = texture(texHeightDx, inUV);   // r=h, b=Dx
    vec4 ds = texture(texDzSx,     inUV);   // r=Dz

    vec3 P = inPosition + vec3(hd.b * CHOPPY, hd.r, ds.r * CHOPPY);

    // 世界空间（model 为单位阵，但保持通用性）
    vec4 wp = model * vec4(P, 1.0);
    worldPos    = wp.xyz;
    worldNormal = vec3(0.0, 1.0, 0.0);   // 法线在片段着色器由斜率纹理重建
    fragUV      = inUV;
    fragColor   = inColor;

    gl_Position = proj * view * wp;
}
