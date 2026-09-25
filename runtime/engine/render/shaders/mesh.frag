#version 450

layout(set = 0, binding = 0) uniform sampler2D base_color;
layout(set = 0, binding = 1) uniform sampler2D opacity;

// material: x = opacity use (0 none, 1 alpha test, 2 blend), y = alpha-test
// clip value, z = opacity channel (0 red .. 3 alpha), w = 1 when lit.
layout(push_constant) uniform Instance
{
    mat4 world;
    mat4 view_projection;
    vec4 material;
} instance;

layout(location = 0) in vec3 in_normal;
layout(location = 1) in vec2 in_uv;

layout(location = 0) out vec4 out_color;

void main()
{
    float alpha = 1.0;
    if (instance.material.x > 0.5)
    {
        alpha = texture(opacity, in_uv)[int(instance.material.z)];
        if (instance.material.x < 1.5 && alpha < instance.material.y)
        {
            discard;
        }
    }
    vec3 color = texture(base_color, in_uv).rgb;
    if (instance.material.w > 0.5)
    {
        vec3 normal = normalize(in_normal);
        vec3 sun = normalize(vec3(0.4, 0.3, 0.85));
        color *= 0.35 + 0.65 * abs(dot(normal, sun));
    }
    out_color = vec4(color, instance.material.x > 1.5 ? alpha : 1.0);
}
