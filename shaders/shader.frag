#version 450
// 水面光照: Fresnel(Schlick) + 程序化天空反射 + 太阳高光(Blinn-Phong)
//          + 波峰泡沫(Jacobian近似) + 视距深度吸收 + 波峰透光(SSS近似)
// 现有水纹理降级为: 细节法线扰动 + 泡沫噪声源

layout(location = 0) in vec2 fragUV;
layout(location = 1) in vec3 fragColor;
layout(location = 2) in vec3 worldPos;
layout(location = 3) in vec3 worldNormal;

layout(binding = 0) uniform UniformData {
    mat4 model;
    mat4 view;
    mat4 proj;
    float time;
    vec3 cameraPos;
    vec3 sunDir;
};

layout(binding = 1) uniform sampler2D waterTex;

layout(location = 0) out vec4 outColor;

// hash 噪声（泡沫破碎用）
float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float noise2(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash21(i), hash21(i + vec2(1, 0)), u.x),
               mix(hash21(i + vec2(0, 1)), hash21(i + vec2(1, 1)), u.x), u.y);
}

void main() {
    vec3 N = normalize(worldNormal);
    vec3 V = normalize(cameraPos - worldPos);          // 指向相机
    vec3 L = normalize(sunDir);                        // 指向太阳

    // ---- 细节法线扰动（纹理低频噪声 → 微扰动）----
    vec2 detailUV = fragUV * 24.0 + vec2(time * 0.02, time * 0.013);
    vec2 dn = texture(waterTex, detailUV).rg - 0.5;
    N = normalize(N + vec3(dn.x, 0.0, dn.y) * 0.18);

    // ---- Fresnel (Schlick, F0=0.02 水) ----
    float cosTheta = clamp(dot(N, V), 0.0, 1.0);
    float F0 = 0.02;
    float fresnel = F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);

    // ---- 程序化天空反射（近似 GTA6 的环境反射）----
    vec3 R = reflect(-V, N);
    float skyGrad = clamp(R.y * 1.4, 0.0, 1.0);
    vec3 zenith  = vec3(0.22, 0.45, 0.80);   // 天顶蓝
    vec3 horizon = vec3(0.75, 0.85, 0.92);   // 地平线亮色
    vec3 skyColor = mix(horizon, zenith, skyGrad);
    // 反射向量低于水平面时压暗（避免海水下侧反常亮）
    skyColor = mix(skyColor, horizon * 0.35, smoothstep(0.0, -0.15, R.y));

    // ---- 太阳镜面高光 (Blinn-Phong) ----
    vec3 H = normalize(L + V);
    float spec = pow(max(dot(N, H), 0.0), 180.0);
    vec3 sunColor = vec3(1.0, 0.95, 0.85);

    // ---- 水体颜色: 视距深度吸收（近浅远深，模拟 GTA 的 depth absorption）----
    float dist = length(cameraPos - worldPos);
    float clarity = exp(-dist * 0.018);
    vec3 deepColor    = vec3(0.015, 0.08, 0.20);
    vec3 shallowColor = vec3(0.05, 0.32, 0.48);
    vec3 waterColor = mix(deepColor, shallowColor, clarity);

    // ---- 波峰透光 (SSS 近似): 波峰高 + 逆光方向 → 内部散射提亮 ----
    float crestHeight = clamp(worldPos.y * 0.9 + 0.4, 0.0, 1.0);
    float backLight = pow(max(dot(V, -L), 0.0), 3.0);
    waterColor += vec3(0.02, 0.14, 0.10) * crestHeight * backLight;

    // ---- 合成: 反射/折射混合 + 高光 ----
    vec3 color = mix(waterColor, skyColor, fresnel);
    color += sunColor * spec * 1.6;

    // ---- 波峰泡沫 (Jacobian 近似: 法线越倾斜波越陡) ----
    float foamJac = 1.0 - clamp(N.y, 0.0, 1.0);
    float foam = smoothstep(0.28, 0.52, foamJac);
    // 纹理噪声破碎，避免泡沫边缘太"干净"
    float foamNoise = texture(waterTex, fragUV * 12.0 + worldPos.xz * 0.15).r;
    foam *= 0.55 + 0.45 * foamNoise;
    // 波峰高度加权：只有真正高处的浪才起沫
    foam *= clamp(worldPos.y * 1.2 + 0.3, 0.0, 1.0);
    color = mix(color, vec3(0.96, 0.98, 1.0), clamp(foam, 0.0, 1.0) * 0.75);

    outColor = vec4(color, 1.0);
}
