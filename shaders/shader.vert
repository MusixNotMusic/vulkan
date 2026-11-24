#version 450
// Gerstner 波顶点位移 + 解析法线
// 参考: GPU Gems Ch.1 / docs/gta6-water-research.md
// 公式: P += (Q·A·D·cos(φ), A·sin(φ), Q·A·D·cos(φ))
//       N += (−D.x·WA·cos(φ), −Q·WA·sin(φ), −D.z·WA·cos(φ))
// 约束: Σ Q·A·k ≤ 1.0（防止波面翻卷）

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

layout(location = 0) out vec2 fragUV;
layout(location = 1) out vec3 fragColor;
layout(location = 2) out vec3 worldPos;
layout(location = 3) out vec3 worldNormal;

// ------------------------------------------------------------------
// 波参数: 方向(归一化), 振幅, 波长, 陡度Q, 初相
// Σ Q·A·k ≈ 0.48 < 1.0  安全
// ------------------------------------------------------------------
struct Wave {
    vec2 dir;
    float amplitude;
    float wavelength;
    float steepness;
    float phase;
};

const Wave WAVES[6] = Wave[6](
    // ---- 大波：主运动 ----
    Wave(vec2( 0.94,  0.34), 0.50, 24.0, 0.45, 0.0),
    Wave(vec2(-0.70,  0.72), 0.30, 14.0, 0.50, 1.3),
    // ---- 中波 ----
    Wave(vec2( 0.30, -0.95), 0.20,  8.0, 0.55, 2.1),
    Wave(vec2(-0.95, -0.30), 0.12,  5.0, 0.60, 0.7),
    // ---- 小波：涟漪细节 ----
    Wave(vec2( 0.60,  0.80), 0.08,  3.5, 0.65, 3.4),
    Wave(vec2(-0.20,  0.98), 0.05,  2.5, 0.70, 4.5)
);

void gerstner(vec2 p0, Wave w, float t,
              out vec3 disp, out vec3 nAcc) {
    float k     = 6.2831853 / w.wavelength;   // 波数
    float omega = sqrt(9.81 * k);             // 色散关系 ω=√(g·k)
    float phi   = k * dot(w.dir, p0) - omega * t + w.phase;
    float QA    = w.steepness * w.amplitude;  // 水平位移幅度
    float WA    = w.amplitude * k;            // 法线幅度

    float c = cos(phi);
    float s = sin(phi);

    disp = vec3(w.dir.x * QA * c,
                w.amplitude * s,
                w.dir.y * QA * c);
    nAcc = vec3(-w.dir.x * WA * c,
                -w.steepness * WA * s,
                -w.dir.y * WA * c);
}

void main() {
    vec2 p0 = inPosition.xz;      // 未变形的原始网格坐标（所有波以此为准）
    vec3 P  = inPosition;
    vec3 N  = vec3(0.0, 1.0, 0.0);

    for (int i = 0; i < 6; ++i) {
        vec3 disp, nAcc;
        gerstner(p0, WAVES[i], time, disp, nAcc);
        P += disp;
        N += nAcc;
    }
    N = normalize(N);

    // 世界空间（model 为单位阵，但保持通用性）
    vec4 wp = model * vec4(P, 1.0);
    worldPos    = wp.xyz;
    worldNormal = mat3(model) * N;
    fragUV      = inUV;
    fragColor   = inColor;

    gl_Position = proj * view * wp;
}
