#version 450

// Native implementation of the observed full-screen scene composite.
//
// The descriptor contract is the translated shader's public host interface:
// fetch constants are six dwords per fetch, system constants use Xenia's
// std140 layout represented as uvec4 words, and the two image bindings are
// the unsigned and signed views of fetch constant zero.  This source is
// independently authored from that observable contract; it contains no title
// instructions or generated microcode.

layout(location = 0) in vec4 in_interpolator_0;
layout(location = 1) in vec4 in_interpolator_1;
layout(location = 0) out vec4 out_fragment;

layout(set = 1, binding = 0, std140) uniform SystemConstants
{
    uvec4 words[33];
} system_constants;

layout(set = 1, binding = 2, std140) uniform FloatConstants
{
    vec4 values[3];
} float_constants;

layout(set = 1, binding = 4, std140) uniform FetchConstants
{
    uvec4 words[48];
} fetch_constants;

layout(set = 3, binding = 0) uniform texture2DArray texture_unsigned;
layout(set = 3, binding = 1) uniform texture2DArray texture_signed;
layout(set = 3, binding = 2) uniform sampler texture_sampler;

float IntegerTextureScale(uint field)
{
    uint mantissa = field & 15u;
    uint exponent = mantissa + 1u - ((field >> 4u) & 1u);
    return float((1u << exponent) - 1u);
}

float GammaToLinear(float unsigned_value)
{
    float value = clamp(unsigned_value, 0.0, 1.0);
    float high_step = value >= 0.752941191 ? 0.0078125 : 0.00390625;
    float high_offset = value >= 0.752941191 ? -1024.0 : -256.0;
    float mid_step = value >= 0.250980407 ? 0.001953125 : 0.0009765625;
    float mid_offset = value >= 0.250980407 ? -64.0 : 0.0;
    float step = value >= 0.376470596 ? high_step : mid_step;
    float offset = value >= 0.376470596 ? high_offset : mid_offset;
    float quantized = value * 261120.0 * step + offset;
    quantized += trunc(quantized * step);
    return quantized * 0.000977517106;
}

float SelectTextureChannel(uint sign_mode, uint channel,
                           vec4 unsigned_sample, vec4 signed_sample)
{
    float unsigned_value = unsigned_sample[channel];
    float signed_value = signed_sample[channel];
    if (sign_mode == 1u)
        return signed_value;
    if (sign_mode == 2u)
        return unsigned_value * 2.0 - 1.0;
    if (sign_mode == 3u)
        return GammaToLinear(unsigned_value);
    return unsigned_value;
}

vec3 EncodeFixedRgb(vec3 value)
{
    value = clamp(value, 0.0, 1.0);
    bvec3 high = greaterThanEqual(value, vec3(0.500488758));
    vec3 high_step = mix(vec3(255.75), vec3(127.875), vec3(high));
    vec3 high_offset = mix(vec3(0.250980407), vec3(0.501960814), vec3(high));
    bvec3 low = greaterThanEqual(value, vec3(0.12512219));
    vec3 low_step = mix(vec3(1023.0), vec3(511.5), vec3(low));
    vec3 low_offset = mix(vec3(0.0), vec3(0.125490203), vec3(low));
    bvec3 upper = greaterThanEqual(value, vec3(0.376470596));
    vec3 step = mix(low_step, high_step, vec3(upper));
    vec3 offset = mix(low_offset, high_offset, vec3(upper));
    return trunc(value * step) * (1.0 / 255.0) + offset;
}

void main()
{
    uvec4 fetch0 = fetch_constants.words[0];
    uint packed_dimensions = fetch0.z;
    vec2 texture_size = vec2(float((packed_dimensions & 8191u) + 1u),
                             float(((packed_dimensions >> 13u) & 8191u) + 1u));
    vec2 coordinate = in_interpolator_0.xy + vec2(0.00146484375) / texture_size;

    int sample_exponent = int(fetch0.w << 10u) >> 22;
    float lod_scale = exp2(float(sample_exponent) * 0.03125);
    vec2 gradient_x = dFdxCoarse(coordinate) * lod_scale;
    vec2 gradient_y = dFdyCoarse(coordinate) * lod_scale;
    vec3 sample_coordinate = vec3(coordinate, 0.0);
    vec4 unsigned_sample = textureGrad(sampler2DArray(texture_unsigned, texture_sampler),
                                        sample_coordinate, gradient_x, gradient_y);
    vec4 signed_sample = textureGrad(sampler2DArray(texture_signed, texture_sampler),
                                      sample_coordinate, gradient_x, gradient_y);

    // The translated shader consumes signs in output order.  The fetch
    // swizzle is already composed into the Vulkan image view by the host
    // texture uploader, so applying system word 6 here would swizzle twice.
    // Conflating that view-owned mapping with the sign byte was the root cause
    // of the rejected native candidate's kGamma divergence.
    uint sign_word = system_constants.words[4].x;
    vec4 colour;
    for (uint channel = 0u; channel < 4u; ++channel)
    {
        uint sign_mode = (sign_word >> (channel * 2u)) & 3u;
        colour[channel] = SelectTextureChannel(sign_mode, channel, unsigned_sample,
                                                signed_sample);
    }

    uint integer_scales = system_constants.words[25].x;
    if (integer_scales != 0u)
    {
        colour *= vec4(IntegerTextureScale(integer_scales),
                       IntegerTextureScale(integer_scales >> 5u),
                       IntegerTextureScale(integer_scales >> 10u),
                       IntegerTextureScale(integer_scales >> 15u));
    }
    int colour_exponent = int(fetch0.w << 13u) >> 26;
    colour *= exp2(float(colour_exponent));
    colour *= in_interpolator_1;

    if (float_constants.values[0].x != float_constants.values[2].x)
    {
        vec3 reordered = clamp(colour.rgb, 0.0, 1.0).zyx;
        colour.rgb = exp2(log2(abs(reordered)) * float_constants.values[0].x).zyx;
    }
    colour.rgb *= float_constants.values[1].x;
    colour *= uintBitsToFloat(system_constants.words[12].x);
    if ((system_constants.words[0].x & 16384u) != 0u)
        colour.rgb = EncodeFixedRgb(colour.rgb);
    out_fragment = colour;
}
