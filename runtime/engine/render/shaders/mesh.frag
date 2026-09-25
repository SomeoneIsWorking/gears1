#version 450

layout(set = 0, binding = 0) uniform sampler2D base_color;
layout(set = 0, binding = 1) uniform sampler2D opacity;
layout(set = 0, binding = 2) uniform sampler2D light_map[3];

// material: x = opacity use (0 none, 1 alpha test, 2 blend), y = alpha-test
// clip value, z = opacity channel (0 red .. 3 alpha), w = lighting (0 unlit,
// 1 fixed sun, 2 light map). light_scales: each light-map coefficient's RGB
// scale.
layout(push_constant) uniform Instance
{
    mat4 world;
    vec4 material;
    vec4 light_scales[3];
} instance;

layout(location = 0) in vec3 in_normal;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec2 in_light_uv;

layout(location = 0) out vec4 out_color;

// Each coefficient's basis direction has a tangent-space Z of 1/sqrt(3), so
// an unperturbed surface normal receives that share of every coefficient.
const float kFlatNormalWeight = 0.57735027;

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
    if (instance.material.w > 1.5)
    {
        vec3 light = vec3(0.0);
        for (int i = 0; i < 3; ++i)
        {
            light += texture(light_map[i], in_light_uv).rgb * instance.light_scales[i].rgb;
        }
        color *= light * kFlatNormalWeight;
    }
    else if (instance.material.w > 0.5)
    {
        vec3 normal = normalize(in_normal);
        vec3 sun = normalize(vec3(0.4, 0.3, 0.85));
        color *= 0.35 + 0.65 * abs(dot(normal, sun));
    }
    out_color = vec4(color, instance.material.x > 1.5 ? alpha : 1.0);
}
