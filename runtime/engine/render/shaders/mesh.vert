#version 450

// Row-vector engine matrices uploaded row-major read as their transposes, so
// a column-vector product here applies them in the engine's order.
layout(set = 1, binding = 0) uniform Frame
{
    mat4 view_projection;
} frame;

layout(push_constant) uniform Instance
{
    mat4 world;
    vec4 material;
    vec4 light_scales[3];
} instance;

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec2 in_light_uv;

layout(location = 0) out vec3 out_normal;
layout(location = 1) out vec2 out_uv;
layout(location = 2) out vec2 out_light_uv;

void main()
{
    vec4 world_position = instance.world * vec4(in_position, 1.0);
    out_normal = normalize(mat3(instance.world) * in_normal);
    out_uv = in_uv;
    out_light_uv = in_light_uv;
    gl_Position = frame.view_projection * world_position;
}
