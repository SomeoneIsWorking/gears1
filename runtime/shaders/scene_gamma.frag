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
// A 2D ARRAY, not a 2D image. The translator declares
// `OpTypeImage %float 2D 0 1 0 1` (Arrayed = 1) and this renderer binds guest
// textures as VK_IMAGE_VIEW_TYPE_2D_ARRAY, so `texture2D` here was the wrong view
// type for the descriptor it was handed. It produced bit-identical pixels anyway
// -- the driver tolerates it -- which is exactly why the A/B gate could not see
// it and Vulkan validation could: "VkImageViewType is VK_IMAGE_VIEW_TYPE_2D_ARRAY
// but the OpTypeImage has (Dim = 2D) and (Arrayed = 0)".
layout(set = 3, binding = 0) uniform texture2DArray SceneColor;   // unsigned view
layout(set = 3, binding = 2) uniform sampler SceneSampler;

// The fetch constants, for the texture size the GUEST programmed. The translator
// scales the texel rounding offset by that, not by the host image's extent.
// Constant k's dword d is at [(6k+d)/4][(6k+d)%4]; the size is dword 6k+2, two
// 13-bit fields holding (size - 1). This pass samples fetch constant 0.
layout(set = 1, binding = 4) uniform XeFetchConstants { uvec4 f[48]; } Fetch_;

layout(set = 1, binding = 0) uniform XeSystemConstants {
    layout(offset = 0)   uint  flags;
    // Post-swizzle TextureSign per component, 8 bits per fetch constant, four
    // fetch constants per uint. This pass samples fetch constant 0, so its byte is
    // the low 8 bits of element 0. Reading it is not optional: the translated
    // shader branches on it, so a native pass that ignores it silently diverges
    // the moment the constant stops being zero -- which is exactly what the A/B
    // gate caught when it did.
    layout(offset = 64)  uvec4 texture_swizzled_signs[2];
    layout(offset = 192) vec4  color_exp_bias;
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

// TextureSign::kGamma -- the 360's piecewise gamma-to-LINEAR decode, the inverse of
// the encode this same pass applies on the way out. Evaluating it: 1.0 -> 1.0 and
// 0.5 -> 0.248, i.e. gamma 2.0, against the composite's own exponent of 0.5.
float DecodeTextureGamma(float v)
{
    v = clamp(v, 0.0, 1.0);
    bool hi  = v >= 0.752941191;
    bool mid = v >= 0.250980407;
    bool up  = v >= 0.376470596;
    float scale  = up ? (hi ? 0.0078125   : 0.00390625)
                      : (mid ? 0.001953125 : 0.0009765625);
    float offset = up ? (hi ? -1024.0 : -256.0)
                      : (mid ? -64.0  : 0.0);
    float a = v * 261120.0 * scale + offset;
    return (a + trunc(a * scale)) * 0.000977517106;
}

// The fetch epilogue the translator emits, for one component. Mode 1 (kSigned)
// samples the signed view; this renderer binds the unsigned view to both, so it is
// a pass-through here exactly as it is in the translated shader under the same
// binding -- and that equivalence is what the A/B gate is checking.
float ApplyTextureSign(float v, uint mode)
{
    if (mode == 2u) return v * 2.0 - 1.0;          // kBiased
    if (mode == 3u) return DecodeTextureGamma(v);  // kGamma
    return v;                                       // kUnsigned, kSigned
}

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
    const uint sizeWord = Fetch_.f[0][2];
    const vec2 texSize = vec2(float(bitfieldExtract(sizeWord, 0, 13) + 1u),
                              float(bitfieldExtract(sizeWord, 13, 13) + 1u));
    vec2 uv = InTexCoord.xy + kTexelRoundingOffset / texSize;
    // Explicit COARSE gradients scaled by the fetch constant's LOD bias, which is
    // what the translator emits (OpDPdxCoarse/OpDPdyCoarse into
    // OpImageSampleExplicitLod ... Grad). An implicit-LOD `texture()` lets the
    // driver choose its own derivative precision -- a different function, and the
    // one that cost the uber-post pass 20 off-by-one samples before it was fixed.
    const float lodScale = exp2(float(bitfieldExtract(int(Fetch_.f[1][0]), 12, 10))
                                * 0.03125);
    vec4 sampled = textureGrad(sampler2DArray(SceneColor, SceneSampler),
                               vec3(uv, 0.0),
                               dFdxCoarse(uv) * lodScale, dFdyCoarse(uv) * lodScale);
    // Fetch constant 0's signs: byte 0 of element 0, two bits per component.
    const uint signs = Sys.texture_swizzled_signs[0].x & 0xFFu;
    sampled.x = ApplyTextureSign(sampled.x, (signs >> 0) & 3u);
    sampled.y = ApplyTextureSign(sampled.y, (signs >> 2) & 3u);
    sampled.z = ApplyTextureSign(sampled.z, (signs >> 4) & 3u);
    sampled.w = ApplyTextureSign(sampled.w, (signs >> 6) & 3u);
    precise vec4 texel = sampled * InModulate;

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
