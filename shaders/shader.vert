#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec3 inColor;

layout(binding = 0) uniform UniformData {
    mat4 model;
    mat4 view;
    mat4 proj;
    float time;
};

layout(location = 0) out vec2 fragUV;
layout(location = 1) out vec3 fragColor;

void main() {
    gl_Position = proj * view * model * vec4(inPosition, 1.0);
    fragUV = inUV;
    fragColor = inColor;
}
