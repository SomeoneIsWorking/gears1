#version 450
// Native pass: the full-screen scene composite -- gamma, then exposure scale.
//
// This is the pass every frame ends on: it samples the resolved scene colour,
// modulates it by an interpolated colour, raises it to a gamma the game supplies
// as a float constant, and scales the result. The title's microcode, decoded:
//
//   /*  3 */ tfetch2D r0, r0.xy, tf0            // scene colour, all four channels
//   /*  4 */ sne r2.x___, c0.xxxx, c255.xxxx    // is a gamma actually configured?
//   /*  5 */ mul r0, r0, r1                     // interpolator 1 modulates it
//   /*  6 */ max_sat r1.xyz_, r0.yxzz, r0.yxzz  + setp_ne r0._, r2.x
//   /*  7 */ (p0) log r0.x___, r_abs[1].z
//   /*  8 */ (p0) log r0._y__, r_abs[1].x
//   /*  9 */ (p0) log r0.__z_, r_abs[1].y
//   /* 10 */ (p0) mul r1.xyz_, r0.xyzz, c0.xxxx
//   /* 11 */ (p0) exp r0.x___, r1.z
//   /* 12 */ (p0) exp r0._y__, r1.y
//   /* 13 */ (p0) exp r0.__z_, r1.x
//   /* 14 */ mul r0.xyz_, r0.xyzz, c1.xxxx
//   /* 15 */ max oC0, r0, r0
//
// THE TWO SWIZZLES CANCEL, and working that through is the only subtle part. The
// saturate stores (s.y, s.x, s.z); the three logs read r1.z, r1.x, r1.y into
// x, y, z; the three exps read r1.z, r1.y, r1.x back. Composing the three
// permutations gives the identity, so the whole predicated block is exactly
//   rgb = pow(saturate(rgb), c0.x)
// and the swizzles are the compiler's register allocation, not a channel swap.
// Writing it out per channel would be transcription; writing it as one line is
// the reading, and the A/B against the translated shader is what settles which
// of the two is right.
//
// THE SATURATE ONLY HAPPENS ON THE GAMMA PATH. When no gamma is configured the
// colour passes through unclamped and can exceed 1 -- so the branch is not an
// optimisation that could be flattened away, it changes the result.
layout(set = 3, binding = 0) uniform texture2D SceneColor;   // unsigned view
layout(set = 3, binding = 2) uniform sampler SceneSampler;

layout(set = 1, binding = 0) uniform XeSystemConstants {
    layout(offset = 0)   uint flags;
    layout(offset = 192) vec4 color_exp_bias;
} Sys;

// PACKED, NOT INDEXED BY REGISTER NUMBER. The microcode names c0, c1 and c255,
// and the translator emits a three-entry block holding exactly those, in
// ascending register order. So c255 is at index 2 here. The packing is derived
// from the microcode, which never changes for a given shader hash, so this
// mapping is fixed -- but it is a mapping, and reading c[255] would be past the
// end of a 48-byte buffer.
layout(set = 1, binding = 2) uniform XeFloatConstants { vec4 c[3]; } Consts;
#define kGamma        Consts.c[0].x   // guest c0.x
#define kExposure     Consts.c[1].x   // guest c1.x
#define kGammaDisabled Consts.c[2].x  // guest c255.x -- gamma applies unless equal

layout(location = 0) in vec4 InTexCoord;     // r0: .xy is the scene coordinate
layout(location = 1) in vec4 InModulate;     // r1: multiplies the sampled colour
layout(location = 0) out vec4 OutColor;

const uint kConvertColor0ToGamma = 0x4000u;
const float kTexelRoundingOffset = 0.75 / 512.0;

vec3 EncodePwlGamma(vec3 linear)
{
    vec3 v = clamp(linear, 0.0, 1.0);
    bvec3 hi    = greaterThanEqual(v, vec3(0.500488758));
    bvec3 mid   = greaterThanEqual(v, vec3(0.0625610948));
    bvec3 upper = greaterThanEqual(v, vec3(0.12512219));
    vec3 scale  = mix(mix(vec3(1023.0), vec3(511.5), vec3(mid)),
                      mix(vec3(255.75), vec3(127.875), vec3(hi)), vec3(upper));
    vec3 offset = mix(mix(vec3(0.0), vec3(0.125490203), vec3(mid)),
                      mix(vec3(0.250980407), vec3(0.501960814), vec3(hi)), vec3(upper));
    return trunc(v * scale) * (1.0 / 255.0) + offset;
}

void main()
{
    vec2 uv = InTexCoord.xy
            + kTexelRoundingOffset / vec2(textureSize(sampler2D(SceneColor, SceneSampler), 0));
    precise vec4 texel = texture(sampler2D(SceneColor, SceneSampler), uv) * InModulate;

    vec3 rgb = texel.xyz;
    if (kGamma != kGammaDisabled)
        // exp2(g * log2(|x|)), not pow(): the sequencer's log and exp are separate
        // instructions and pow() is free to be a different approximation.
        rgb = exp2(kGamma * log2(abs(clamp(rgb, 0.0, 1.0))));

    precise vec3 scaled = rgb * kExposure;
    float alpha = texel.w;

    scaled *= Sys.color_exp_bias.x;
    alpha  *= Sys.color_exp_bias.x;
    if ((Sys.flags & kConvertColor0ToGamma) != 0u)
        scaled = EncodePwlGamma(scaled);

    OutColor = vec4(scaled, alpha);
}
