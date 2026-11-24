#version 450

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec3 fragColor;

layout(binding = 0) uniform UniformData {
    mat4 model;
    mat4 view;
    mat4 proj;
    float time;
};

layout(binding = 1) uniform sampler2D waterTex;

layout(location = 0) out vec4 outColor;

// 简化 hash 噪声
float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

// fbm 噪声
float fbm(vec2 p) {
    float value = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 4; i++) {
        value += amp * hash21(p);
        p *= 2.0;
        amp *= 0.5;
    }
    return value;
}

void main() {
    // 时间驱动的 UV 扰动 —— 模拟水流动 + 波纹
    vec2 uv = fragUV * 6.0; // 平铺 6 次，让网格上每个 tile 有纹理

    // 大尺度流动：两层正弦波叠加
    float wave1 = sin(uv.x * 1.5 + time * 0.8) * 0.02;
    float wave2 = cos(uv.y * 2.0 + time * 0.6 + 1.5) * 0.02;

    // 小尺度高光：噪声驱动
    float n1 = fbm(uv * 3.0 + vec2(time * 0.2, time * 0.1));
    float n2 = fbm(uv * 5.0 - vec2(time * 0.15, -time * 0.25));

    vec2 displacedUV = uv + vec2(wave1, wave2) + vec2(n1 - 0.5, n2 - 0.5) * 0.08;

    // 主颜色（采样水纹理 + 顶点色混合）
    vec3 waterColor = texture(waterTex, displacedUV).rgb;

    // 高光斑点：小噪声值 → 高光
    float highlight = smoothstep(0.82, 0.95, n2);
    waterColor = mix(waterColor, vec3(1.0), highlight * 0.35);

    // 深色边缘（近地平线处更深）
    float darkenY = smoothstep(0.0, 0.15, fragUV.y) *
                    smoothstep(1.0, 0.85, fragUV.y);
    waterColor *= mix(0.75, 1.0, darkenY);

    // 轻微叠加顶点色
    waterColor = mix(waterColor, fragColor, 0.3);

    outColor = vec4(waterColor, 1.0);
}
