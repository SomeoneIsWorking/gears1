#version 450

layout(set = 0, binding = 0) uniform sampler2D base_color;

layout(location = 0) in vec3 in_normal;
layout(location = 1) in vec2 in_uv;

layout(location = 0) out vec4 out_color;

void main()
{
    vec3 normal = normalize(in_normal);
    vec3 sun = normalize(vec3(0.4, 0.3, 0.85));
    float lit = 0.35 + 0.65 * abs(dot(normal, sun));
    out_color = vec4(texture(base_color, in_uv).rgb * lit, 1.0);
}
