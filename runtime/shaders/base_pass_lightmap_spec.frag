#version 450
// Native pass: UE3's base pass, directional-lightmap material with a specular
// EXPONENT (pixel shader 0xd99a15450a08043a).
//
// THE SECOND MATERIAL OF THE SAME FAMILY as runtime/shaders/base_pass_lightmap.frag,
// and worth having both: it is what says which parts of that shader were UE3's
// base pass and which were one material's own choices. 60 draws across three
// captures, third-largest base-pass shader in an Act 1 frame.
//
// SAME SKELETON, THREE DIFFERENCES. Read against base_pass_lightmap.frag:
//
//   - **The reflection weights are raised to a power, not squared.** The other
//     material squares `saturate(R . basis)`; this one does
//     `exp2(c255.x * log2(saturate(R . basis)))` -- a specular exponent the guest
//     supplies. The diffuse weights are still squared in both.
//   - **There is no specular map.** The albedo (tf1) scaled by `c255.w` serves as
//     both, so the specular terms multiply the albedo where the other material
//     multiplied a separate texture.
//   - **The normal comes from ONE texture**, scale-and-biased by `c253.w`/`c252.z`,
//     with no detail normal map added on top.
//
// The basis vectors are packed differently again -- two of them differ in their
// SECOND coefficient here (`c254.y` against `c254.w`) where the other material's
// differed in the first, and the third is a two-term dot against `c253.xy`. Same
// shape, different register packing: that is a material's parameter layout, not a
// change in what UE3 is doing.
//
//   N   = normalize(normal map * c253.w + c252.z)              tf0
//   V   = normalize(interpolator 3)      eye vector, tangent space
//   L   = normalize(interpolator 4)      light vector, tangent space
//   R   = 2*(N.V)*N - V
//   A'  = albedo * c255.w                                      tf1
//   LMi = LightMapScale[i] * lightmap_texture_i                 tf2, tf3, tf4
//
//   colour = A'*D * (LM0*bC(N)^2 + LM1*bA(N)^2 + LM2*bB(N)^2)
//          + A'    * (LM0*bC(R)^p + LM1*bA(R)^p + LM2*bB(R)^p)
//          + A'*D  * (c1*wrapA^2 + c2*wrapB^2)
//          + c0
//   colour *= c253.z            D = c252.x - c0,  p = c255.x
//
// The reduction is `tools/ucode_reduce.py` on the title's own microcode; the
// accumulation ORDER below is the sequencer's and must not be rearranged, because
// it rounds after every step. The interface is read off the module the runtime
// builds (GEARS_DRAW_SPV_DUMP): texture bindings are numbered by ORDER OF FIRST
// USE, which here fetches tf1 first, so the albedo lands on binding 0 and the
// normal map -- fetch constant 0 -- lands on binding 4.
layout(set = 3, binding = 0) uniform texture2DArray TexAlbedo;      // tf1
layout(set = 3, binding = 2) uniform texture2DArray TexLightMap2;   // tf4
layout(set = 3, binding = 4) uniform texture2DArray TexNormal;      // tf0
layout(set = 3, binding = 6) uniform texture2DArray TexLightMap1;   // tf3
layout(set = 3, binding = 8) uniform texture2DArray TexLightMap0;   // tf2
layout(set = 3, binding = 10) uniform sampler SmpAlbedo;
layout(set = 3, binding = 11) uniform sampler SmpLightMap2;
layout(set = 3, binding = 12) uniform sampler SmpNormal;
layout(set = 3, binding = 13) uniform sampler SmpLightMap1;
layout(set = 3, binding = 14) uniform sampler SmpLightMap0;

layout(set = 1, binding = 0) uniform XeSystemConstants {
    layout(offset = 0)   uint  flags;
    layout(offset = 64)  uvec4 texture_swizzled_signs[2];
    layout(offset = 192) vec4  color_exp_bias;
    layout(offset = 400) uvec4 texture_integer_scale_bits[8];
} Sys;

layout(set = 1, binding = 4) uniform XeFetchConstants { uvec4 f[48]; } Fetch_;

// Packed in ascending register order: c0,c1,c2,c3,c4,c5,c252,c253,c254,c255.
layout(set = 1, binding = 2) uniform XeFloatConstants { vec4 c[10]; } K;
#define kAmbient        K.c[0].xyz  // c0
#define kLightA         K.c[1].xyz  // c1
#define kLightB         K.c[2].xyz  // c2
#define kLightMapScale0 K.c[3].xyz  // c3
#define kLightMapScale1 K.c[4].xyz  // c4
#define kLightMapScale2 K.c[5].xyz  // c5
#define kOne            K.c[6].x    // c252.x -- forms the diffuse scale with c0
#define kDotAddend      K.c[6].y    // c252.y -- the dp2add addend of basis C
#define kNormalBias     K.c[6].z    // c252.z
#define kBasisCy        K.c[7].x    // c253.x
#define kBasisCz        K.c[7].y    // c253.y
#define kOutputScale    K.c[7].z    // c253.z
#define kNormalScale    K.c[7].w    // c253.w
#define kBasisZ         K.c[8].x    // c254.x -- shared by basis A and basis B
#define kBasisAx        K.c[8].y    // c254.y
#define kBasisY         K.c[8].z    // c254.z -- shared
#define kBasisBx        K.c[8].w    // c254.w
#define kSpecExponent   K.c[9].x    // c255.x
#define kWrapA          K.c[9].y    // c255.y -- both the scale and the offset
#define kWrapB          K.c[9].z    // c255.z
#define kAlbedoScale    K.c[9].w    // c255.w

layout(location = 0) in vec4 InLightMapUV;   // r0
layout(location = 1) in vec4 InTexCoord;     // r1
layout(location = 2) in vec4 InAlpha;        // r2: only .w is read
layout(location = 3) in vec4 InEyeVector;    // r3
layout(location = 4) in vec4 InLightVector;  // r4
layout(location = 0) out vec4 OutColor;

const uint kConvertColor0ToGamma = 0x4000u;
const float kTexelRoundingOffset = 0.75 / 512.0;

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

float ApplyTextureSign(float v, uint mode)
{
    if (mode == 2u) return v * 2.0 - 1.0;
    if (mode == 3u) return DecodeTextureGamma(v);
    return v;
}

float ApplyIntegerScale(float v, uint bits, uint comp)
{
    if (bits == 0u)
        return v;
    const uint field = bitfieldExtract(bits, int(5u * comp), 5);
    const uint n = (field & 15u) + 1u - bitfieldExtract(field, 4, 1);
    return v * float((1u << n) - 1u);
}

vec2 FetchSize(uint k)
{
    const uint d = 6u * k + 2u;
    const uint w = Fetch_.f[d >> 2][d & 3u];
    return vec2(float(bitfieldExtract(w, 0, 13) + 1u),
                float(bitfieldExtract(w, 13, 13) + 1u));
}

float FetchLodScale(uint k)
{
    const uint d = 6u * k + 4u;
    return exp2(float(bitfieldExtract(int(Fetch_.f[d >> 2][d & 3u]), 12, 10))
                * 0.03125);
}

vec4 Fetch(texture2DArray tex, sampler smp, vec2 uv, uint k)
{
    const vec2 co = uv + kTexelRoundingOffset / FetchSize(k);
    const float s = FetchLodScale(k);
    vec4 v = textureGrad(sampler2DArray(tex, smp), vec3(co, 0.0),
                         dFdxCoarse(co) * s, dFdyCoarse(co) * s);
    const uint u = k >> 2;
    const uint signs = (Sys.texture_swizzled_signs[u >> 2][u & 3u] >> (8u * (k & 3u)))
                       & 0xFFu;
    const uint scaleBits = Sys.texture_integer_scale_bits[k >> 2][k & 3u];
    for (uint i = 0u; i < 4u; ++i)
    {
        v[i] = ApplyTextureSign(v[i], (signs >> (2u * i)) & 3u);
        v[i] = ApplyIntegerScale(v[i], scaleBits, i);
    }
    return v;
}

float Dp3(vec3 a, vec3 b)
{
    precise vec3 p = a * b;
    precise float s = p.x + p.y;
    precise float t = s + p.z;
    return t;
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

// The three lightmap basis weights for a direction. Basis A and B differ only in
// their x coefficient; basis C is the one with a zero component, so the sequencer
// issues it as a two-term `dp2add` rather than a `dp3`.
vec3 BasisWeights(vec3 d)
{
    precise float a = Dp3(vec3(d.z, d.x, d.y), vec3(kBasisZ, kBasisAx, kBasisY));
    precise float b = Dp3(vec3(d.z, d.x, d.y), vec3(kBasisZ, kBasisBx, kBasisY));
    precise float c = (d.y * kBasisCy + d.z * kBasisCz) + kDotAddend;
    return clamp(vec3(a, b, c), 0.0, 1.0);
}

void main()
{
    const vec2 uv = InTexCoord.xy;
    const vec2 lmUv = InLightMapUV.xy;

    const vec3 albedo = Fetch(TexAlbedo,    SmpAlbedo,    uv,   1u).xyz;
    const vec3 T4     = Fetch(TexLightMap2, SmpLightMap2, lmUv, 4u).xyz;
    const vec3 nrm    = Fetch(TexNormal,    SmpNormal,    uv,   0u).xyz;
    const vec3 T3     = Fetch(TexLightMap1, SmpLightMap1, lmUv, 3u).xyz;
    const vec3 T2     = Fetch(TexLightMap0, SmpLightMap0, lmUv, 2u).xyz;

    // Tangent-space frame, normalised with the Xenos's `rsq` of a dp3 associated
    // (z*z + x*x) + y*y, which is the operand order the microcode names.
    const vec3 V = inversesqrt(abs(Dp3(InEyeVector.zxy, InEyeVector.zxy)))
                 * InEyeVector.xyz;
    const vec3 L = inversesqrt(abs(Dp3(InLightVector.zxy, InLightVector.zxy)))
                 * InLightVector.xyz;

    // One normal map, scaled and biased. No detail map here, unlike the other
    // material in this family.
    precise vec3 n = nrm * kNormalScale + kNormalBias;
    const vec3 N = n * inversesqrt(abs(Dp3(n.zxy, n.zxy)));

    precise float NdotV = Dp3(vec3(N.z, N.x, N.y), vec3(V.z, V.x, V.y));
    precise vec3 R = vec3((N.x * NdotV + N.x * NdotV) - V.x,
                          (N.y * NdotV + N.y * NdotV) - V.y,
                          (N.z * NdotV + N.z * NdotV) - V.z);

    const vec3 bN = BasisWeights(N);
    const vec3 bR = BasisWeights(R);
    // Diffuse squares the weight; specular raises it to the guest's exponent.
    // exp2(p * log2(x)), not pow(): the sequencer's log and exp are separate
    // instructions and pow() is free to be a different approximation.
    const vec3 diffW = bN * bN;
    const vec3 specW = exp2(vec3(kSpecExponent) * log2(bR));

    const vec3 LM0 = kLightMapScale0 * T2;
    const vec3 LM1 = kLightMapScale1 * T3;
    const vec3 LM2 = kLightMapScale2 * T4;

    // The albedo doubles as the specular colour; only the diffuse path is scaled
    // by (c252.x - c0).
    const vec3 A = albedo * kAlbedoScale;
    const vec3 D = vec3(kOne) - kAmbient;
    const vec3 DA = A * D;

    // Basis C pairs with lightmap 0, basis A with lightmap 1, basis B with
    // lightmap 2. That pairing is the guest's and differs from the other
    // material's, so it is read off the microcode rather than assumed.
    precise vec3 acc = (LM0 * diffW.z) * DA + (LM0 * specW.z) * A;
    acc = (LM1 * diffW.x) * DA + acc;
    acc = (LM1 * specW.x) * A + acc;
    acc = (LM2 * diffW.y) * DA + acc;
    acc = (LM2 * specW.y) * A + acc;

    // Two dynamic directional terms. c255.y is both the scale and the offset of
    // the first wrap; neither is saturated before squaring.
    precise float NdotL = Dp3(vec3(L.z, L.x, L.y), vec3(N.z, N.x, N.y));
    precise float wrapA = NdotL * kWrapA + kWrapA;
    precise float wrapB = NdotL * kWrapB + kWrapA;
    precise vec3 dynamic = (DA * (wrapA * wrapA)) * kLightA
                         + (DA * (wrapB * wrapB)) * kLightB;
    acc = dynamic + acc;
    acc = acc + kAmbient;

    vec3 rgb = acc * kOutputScale;
    float alpha = InAlpha.w;

    rgb   *= Sys.color_exp_bias.x;
    alpha *= Sys.color_exp_bias.x;
    if ((Sys.flags & kConvertColor0ToGamma) != 0u)
        rgb = EncodePwlGamma(rgb);

    OutColor = vec4(rgb, alpha);
}
