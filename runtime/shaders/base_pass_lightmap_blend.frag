#version 450
// Native pass: UE3's base pass, directional-lightmap material with a two-layer
// diffuse blend (pixel shader 0xffdafff8542ddcd6).
//
// THE THIRD MATERIAL OF THE FAMILY, and the biggest so far: nine texture fetches,
// eleven constant registers, 184 expressions after reduction. 60 draws across
// three captures.
//
// The skeleton is the one the other two established and it survives again: three
// lightmap textures scaled by LightMapScale[i], three basis weights for the
// shading normal (squared) and three for the reflection (raised to an exponent),
// a six-step alternating diffuse/specular accumulation in a fixed order, two
// dynamic wrap terms, an ambient constant, an output scale.
//
// WHAT THIS MATERIAL ADDS:
//
//   - **A two-layer diffuse.** The albedo is blended against a second colour map
//     through a single-channel mask: `lerp(albedo*c253.x, blend, mask*c253.z)`,
//     written by the sequencer as `mask*(blend - albedo') + albedo'`.
//   - **A specular colour map** (tf5) that the specular terms multiply, where the
//     first material used tf3 and the second re-used its albedo.
//   - **Two different specular exponents.** Basis A and basis C are raised to
//     `c254.x`; basis B is raised to `c253.w`. That asymmetry is what the
//     microcode does -- instruction 45 multiplies basis B's log by `c253.w` while
//     46 multiplies basis A's by `c254.x` -- and it is transcribed rather than
//     tidied, because a native pass that "fixes" an asymmetry is guessing.
//
// And the pairing changed AGAIN: LM(c3) with basis C, LM(c4) with basis B, LM(c5)
// with basis A. Three materials, three different pairings. It is register
// packing chosen per material and it is read off the microcode every time.
//
//   N   = normalize(2*normal + detail*c252.z + bias)     tf0, tf1 (detail at uv*c252.w)
//   V, L = normalize(interpolators 3, 4)
//   R   = 2*(N.V)*N - V
//   C   = lerp(albedo*c253.x, blend, mask*c253.z)        tf2, tf3, tf4
//   D   = c255.z - c0
//   colour = C*D * (LM3*bC(N)^2 + LM4*bB(N)^2 + LM5*bA(N)^2)
//          + spec * (LM3*bC(R)^e + LM4*bB(R)^f + LM5*bA(R)^e)   tf5, e=c254.x f=c253.w
//          + C*D  * (c1*wrapA^2 + c2*wrapB^2)
//          + c0
//   colour *= c252.y
//
// Interface off the module the runtime builds (GEARS_DRAW_SPV_DUMP): bindings are
// numbered by ORDER OF FIRST USE, which here fetches tf3 first, so the BLEND map
// is binding 0 and the normal map -- fetch constant 0 -- is binding 12. Eleven
// float constants packed ascending: c0..c5 then c251,c252,c253,c254,c255.
layout(set = 3, binding = 0)  uniform texture2DArray TexBlend;     // tf3
layout(set = 3, binding = 2)  uniform texture2DArray TexAlbedo;    // tf2
layout(set = 3, binding = 4)  uniform texture2DArray TexSpecular;  // tf5
layout(set = 3, binding = 6)  uniform texture2DArray TexMask;      // tf4
layout(set = 3, binding = 8)  uniform texture2DArray TexLightMapC; // tf6, scaled by c3
layout(set = 3, binding = 10) uniform texture2DArray TexDetailNrm; // tf1
layout(set = 3, binding = 12) uniform texture2DArray TexNormal;    // tf0
layout(set = 3, binding = 14) uniform texture2DArray TexLightMapA; // tf8, scaled by c5
layout(set = 3, binding = 16) uniform texture2DArray TexLightMapB; // tf7, scaled by c4
layout(set = 3, binding = 18) uniform sampler SmpBlend;
layout(set = 3, binding = 19) uniform sampler SmpAlbedo;
layout(set = 3, binding = 20) uniform sampler SmpSpecular;
layout(set = 3, binding = 21) uniform sampler SmpMask;
layout(set = 3, binding = 22) uniform sampler SmpLightMapC;
layout(set = 3, binding = 23) uniform sampler SmpDetailNrm;
layout(set = 3, binding = 24) uniform sampler SmpNormal;
layout(set = 3, binding = 25) uniform sampler SmpLightMapA;
layout(set = 3, binding = 26) uniform sampler SmpLightMapB;

layout(set = 1, binding = 0) uniform XeSystemConstants {
    layout(offset = 0)   uint  flags;
    layout(offset = 64)  uvec4 texture_swizzled_signs[2];
    layout(offset = 192) vec4  color_exp_bias;
    layout(offset = 400) uvec4 texture_integer_scale_bits[8];
} Sys;

layout(set = 1, binding = 4) uniform XeFetchConstants { uvec4 f[48]; } Fetch_;

layout(set = 1, binding = 2) uniform XeFloatConstants { vec4 c[11]; } K;
#define kAmbient      K.c[0].xyz   // c0
#define kLightA       K.c[1].xyz   // c1
#define kLightB       K.c[2].xyz   // c2
#define kLmScaleC     K.c[3].xyz   // c3 -> tf6
#define kLmScaleB     K.c[4].xyz   // c4 -> tf7
#define kLmScaleA     K.c[5].xyz   // c5 -> tf8
#define kNormalZBias  K.c[6].x     // c251.x
#define kDetailBias   K.c[6].y     // c251.y
#define kZero         K.c[7].x     // c252.x -- the dot products' addend
#define kOutputScale  K.c[7].y     // c252.y
#define kNormalScale  K.c[7].z     // c252.z
#define kDetailUv     K.c[7].w     // c252.w
// c253.x is BOTH the albedo scale and the first wrap's scale and offset. The
// guest packs unrelated parameters into one register; named once so the uses stay
// traceable rather than looking like a transcription slip.
#define kAlbedoScale  K.c[8].x     // c253.x
#define kWrapB        K.c[8].y     // c253.y
#define kMaskScale    K.c[8].z     // c253.z
#define kSpecExpB     K.c[8].w     // c253.w -- basis B only
#define kSpecExpAC    K.c[9].x     // c254.x -- basis A and basis C
#define kBasisZ       K.c[9].y     // c254.y
#define kBasisXY      K.c[9].z     // c254.z
#define kBasisCy      K.c[9].w     // c254.w
#define kBasisAx      K.c[10].x    // c255.x
#define kBasisY       K.c[10].y    // c255.y
#define kOne          K.c[10].z    // c255.z
#define kBasisBx      K.c[10].w    // c255.w

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

// The three basis weights for a direction, before squaring or exponentiation.
// A and B differ only in their x coefficient and run against a PRE-SCALED copy of
// the direction; C is the one with a zero component, so the sequencer issues it as
// a two-term dp2add against the RAW direction. Returned as (A, B, C).
vec3 BasisWeights(vec3 d)
{
    const vec3 pre = vec3(d.x * kBasisXY, d.y * kBasisXY, d.z * kBasisZ);
    precise float a = Dp3(pre, vec3(kBasisAx, kBasisY, kOne));
    precise float b = Dp3(pre, vec3(kBasisBx, kBasisY, kOne));
    precise float c = (d.y * kBasisCy + d.z * kBasisZ) + kZero;
    return clamp(vec3(a, b, c), 0.0, 1.0);
}

void main()
{
    const vec2 uv = InTexCoord.xy;
    const vec2 lmUv = InLightMapUV.xy;

    const vec3 blendTex = Fetch(TexBlend,     SmpBlend,     uv,   3u).xyz;
    const vec3 albedo   = Fetch(TexAlbedo,    SmpAlbedo,    uv,   2u).xyz;
    const vec3 specTex  = Fetch(TexSpecular,  SmpSpecular,  uv,   5u).xyz;
    const float maskTex = Fetch(TexMask,      SmpMask,      uv,   4u).x;
    const vec3 lmC      = Fetch(TexLightMapC, SmpLightMapC, lmUv, 6u).xyz;
    const vec2 detail   = Fetch(TexDetailNrm, SmpDetailNrm, uv * kDetailUv, 1u).xy;
    const vec3 nrm      = Fetch(TexNormal,    SmpNormal,    uv,   0u).xyz;
    const vec3 lmA      = Fetch(TexLightMapA, SmpLightMapA, lmUv, 8u).xyz;
    const vec3 lmB      = Fetch(TexLightMapB, SmpLightMapB, lmUv, 7u).xyz;

    const vec3 V = inversesqrt(abs(Dp3(InEyeVector.zxy, InEyeVector.zxy)))
                 * InEyeVector.xyz;
    const vec3 L = inversesqrt(abs(Dp3(InLightVector.zxy, InLightVector.zxy)))
                 * InLightVector.xyz;

    // The normal: the base map added TWICE to the biased detail map. Written as
    // two adds because that is what the sequencer does -- doubling with a multiply
    // rounds differently.
    precise float nx = (detail.x * kNormalScale + kDetailBias) + nrm.x;
    nx = nx + nrm.x;
    precise float ny = (detail.y * kNormalScale + kDetailBias) + nrm.y;
    ny = ny + nrm.y;
    precise float nz = nrm.z * kNormalScale + kNormalZBias;
    precise float nLenSq = (nx * nx + ny * ny) + kZero;
    nLenSq = nz * nz + nLenSq;
    const vec3 N = vec3(nx, ny, nz) * inversesqrt(abs(nLenSq));

    precise float NdotV = Dp3(vec3(N.z, N.x, N.y), vec3(V.z, V.x, V.y));
    // TWO steps, as the microcode has them: the doubled term carrying the dot's
    // zero addend, then the subtraction of V.
    precise vec3 twice = vec3((N.x * NdotV + N.x * NdotV) + kZero,
                              (N.y * NdotV + N.y * NdotV) + kZero,
                              (N.z * NdotV + N.z * NdotV) + kZero);
    precise vec3 R = twice - V;

    const vec3 bN = BasisWeights(N);
    const vec3 bR = BasisWeights(R);
    const vec3 diffW = bN * bN;
    // TWO exponents, not one: basis B is raised to c253.w while A and C are raised
    // to c254.x. That asymmetry is the microcode's (instruction 45 scales basis
    // B's log by c253.w, 46 scales A's by c254.x) and is transcribed rather than
    // tidied -- a native pass that "fixes" an asymmetry is guessing.
    const float specA = exp2(kSpecExpAC * log2(bR.x));
    const float specB = exp2(kSpecExpB  * log2(bR.y));
    const float specC = exp2(kSpecExpAC * log2(bR.z));

    const vec3 LMA = kLmScaleA * lmA;
    const vec3 LMB = kLmScaleB * lmB;
    const vec3 LMC = kLmScaleC * lmC;

    // The two-layer diffuse: albedo blended toward a second colour map through a
    // single-channel mask, as `mask*(blend - albedo) + albedo`.
    precise vec3 albedoScaled = albedo * kAlbedoScale;
    const float mask = maskTex * kMaskScale;
    precise vec3 C = mask * (blendTex - albedoScaled) + albedoScaled;
    const vec3 D = vec3(kOne) - kAmbient;

    // GROUPING MATTERS. The diffuse terms are ((LM * weight) * D) * C -- three
    // separate multiplies -- while the dynamic terms below use C*D computed once.
    // Regrouping either changes the rounding.
    precise vec3 acc = ((LMC * diffW.z) * D) * C + (LMC * specC) * specTex;
    acc = ((LMB * diffW.y) * D) * C + acc;
    acc = (LMB * specB) * specTex + acc;
    acc = ((LMA * diffW.x) * D) * C + acc;
    acc = (LMA * specA) * specTex + acc;

    precise float NdotL = Dp3(vec3(L.z, L.x, L.y), vec3(N.z, N.x, N.y));
    precise float wrapA = NdotL * kAlbedoScale + kAlbedoScale;
    precise float wrapB = NdotL * kWrapB + kAlbedoScale;
    precise vec3 DC = C * D;
    precise vec3 dynamic = (DC * (wrapA * wrapA)) * kLightA
                         + (DC * (wrapB * wrapB)) * kLightB;
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
