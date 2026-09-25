#version 450

layout(location = 0) in vec3 in_normal;
layout(location = 1) in vec2 in_uv;

layout(location = 0) out vec4 out_color;

void main()
{
    vec3 normal = normalize(in_normal);
    vec3 sun = normalize(vec3(0.4, 0.3, 0.85));
    float lit = 0.25 + 0.75 * max(dot(normal, sun), 0.0);
    vec3 albedo = vec3(0.72, 0.70, 0.66);
    out_color = vec4(albedo * lit, 1.0);
}
