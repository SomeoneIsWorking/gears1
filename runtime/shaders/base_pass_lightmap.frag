#version 450
// Native pass: UE3's base pass, directional-lightmap material (pixel shader
// 0x1f1a3f779667a02a).
//
// THE FIRST PASS THAT DRAWS THE WORLD. Everything native before this was
// full-screen post: this one is 174 draws of the Act 1 courtyard's geometry, per
// EDRAM tile, the hottest of the 44 base-pass materials in that frame
// (`tools/pass_structure.py --draws BASEPASS`). It is UE3's
// `FBasePassDrawingPolicy` shading a texture-lightmapped surface --
// `BasePassPixelShader.usf` plus the material's own generated expression chain.
//
// WHAT IT COMPUTES, in UE3's vocabulary. Gears' static lighting is a DIRECTIONAL
// lightmap: three coefficient textures holding the incoming radiance projected
// onto three fixed basis vectors, and a surface samples them weighted by how much
// its normal faces each basis. This shader does that TWICE -- once with the
// shading normal, giving diffuse, and once with the reflection of the eye vector
// about that normal, giving specular from the same lightmap. Then two dynamic
// directional terms, then a constant.
//
//   N   = normalize(base normal map + detail normal map)   tf0, tf1
//   V   = normalize(interpolator 3)      the eye vector, tangent space
//   L   = normalize(interpolator 4)      a light vector, tangent space
//   R   = 2*(N.V)*N - V
//   bN  = saturate(N . LightMapBasis[i])^2      i = 0,1,2
//   bR  = saturate(R . LightMapBasis[i])^2
//   LMi = LightMapScale[i] * lightmap_texture_i                tf4, tf5, tf6
//
//   colour = Diffuse * Albedo * (LM0*bN2 + LM1*bN1 + LM2*bN0)
//          +           Specular * (LM0*bR2 + LM1*bR1 + LM2*bR0)
//          + Diffuse * Albedo * (c1*wrapA^2 + c2*wrapB^2)
//          + c0
//   colour *= c252.y
//
// The basis vectors are the guest's constants, not ours: two of them differ only
// in their first component (`c255.x` against `c255.w`) and the third has a zero
// component, which is exactly the shape of UE3's `LightMapBasis` -- two mirrored
// vectors and one in the plane between them.
//
// HOW THE SWIZZLES WERE REDUCED. The microcode's 49 ALU instructions rotate
// channels on almost every line: ten consecutive `mad r0.xyz_, ..., r0.zxyy`
// accumulations each rotate by one, and the lightmap constants are read as
// `c3.zyx` against a texture fetched with destination swizzle `zxy_`. Composed,
// every one of those permutations cancels: the whole shader is per-channel
// arithmetic in the original channel order. That was established by symbolically
// simulating the register file rather than by reading swizzles off the page, and
// the A/B gate against the translated shader is what makes the reduction safe to
// trust. The one thing that does NOT cancel is the ORDER of the accumulation, and
// that is preserved literally below -- the sequencer rounds after every step.
//
// THE INTERFACE, read off the module the runtime actually builds
// (GEARS_DRAW_SPV_DUMP + spirv-dis), not guessed. Texture bindings are numbered
// by ORDER OF FIRST USE in the shader, which here fetches tf3 first:
//   set 3: 0/1 = texture3, 2/3 = texture1, 4/5 = texture0, 6/7 = texture4,
//          8/9 = texture5, 10/11 = texture6, 12/13 = texture2,
//          samplers 14..20 in that same order.
// Five interpolators, locations 0..4. Ten float constants, packed in ascending
// register order: c0,c1,c2,c3,c4,c5,c252,c253,c254,c255 -> indices 0..9.
layout(set = 3, binding = 0)  uniform texture2DArray TexSpecular;   // tf3
layout(set = 3, binding = 2)  uniform texture2DArray TexDetailNrm;  // tf1
layout(set = 3, binding = 4)  uniform texture2DArray TexNormal;     // tf0
layout(set = 3, binding = 6)  uniform texture2DArray TexLightMap0;  // tf4
layout(set = 3, binding = 8)  uniform texture2DArray TexLightMap1;  // tf5
layout(set = 3, binding = 10) uniform texture2DArray TexLightMap2;  // tf6
layout(set = 3, binding = 12) uniform texture2DArray TexAlbedo;     // tf2
layout(set = 3, binding = 14) uniform sampler SmpSpecular;
layout(set = 3, binding = 15) uniform sampler SmpDetailNrm;
layout(set = 3, binding = 16) uniform sampler SmpNormal;
layout(set = 3, binding = 17) uniform sampler SmpLightMap0;
layout(set = 3, binding = 18) uniform sampler SmpLightMap1;
layout(set = 3, binding = 19) uniform sampler SmpLightMap2;
layout(set = 3, binding = 20) uniform sampler SmpAlbedo;

layout(set = 1, binding = 0) uniform XeSystemConstants {
    layout(offset = 0)   uint  flags;
    // Two bits per component, eight bits per fetch constant, four fetch constants
    // per uint. Fetch constant k is uint k/4 -- element (k/4)/4, component
    // (k/4)%4 -- at bit 8*(k%4).
    layout(offset = 64)  uvec4 texture_swizzled_signs[2];
    layout(offset = 192) vec4  color_exp_bias;
    layout(offset = 400) uvec4 texture_integer_scale_bits[8];
} Sys;

// Xenos fetch constants: six dwords each, packed as uvec4[48]. Constant k's dword
// d is at [(6k+d)/4][(6k+d)%4]. Size is dword 6k+2 (two 13-bit fields holding
// size-1); the LOD bias is dword 6k+4, bits 12..21, signed, in 1/32 steps.
layout(set = 1, binding = 4) uniform XeFetchConstants { uvec4 f[48]; } Fetch_;

layout(set = 1, binding = 2) uniform XeFloatConstants { vec4 c[10]; } K;
#define kAmbient      K.c[0].xyz  // c0.xyz  -- added at the end, and subtracted
                                  // from c255.z to form the diffuse scale
#define kLightA       K.c[1].xyz  // c1
#define kLightB       K.c[2].xyz  // c2
#define kLightMapScale0 K.c[3].xyz // c3
#define kLightMapScale1 K.c[4].xyz // c4
#define kLightMapScale2 K.c[5].xyz // c5
#define kDetailUvScale  K.c[6].x   // c252.x
#define kOutputScale    K.c[6].y   // c252.y
#define kZero           K.c[6].z   // c252.z -- the dot products' addend
// c254.x does DOUBLE DUTY in this material: it scales the base normal map's z
// channel, and it is both the scale and the offset of the first dynamic term's
// wrap. That is the guest packing two unrelated parameters into one register, not
// a transcription slip -- named once here so the two uses stay traceable.
#define kNormalZScale   K.c[8].x   // c254.x
#define kWrapScaleB     K.c[8].y   // c254.y
#define kBasis2Y        K.c[8].z   // c254.z
#define kBasis2Z        K.c[8].w   // c254.w
#define kOne            K.c[9].z   // c255.z

layout(location = 0) in vec4 InLightMapUV;   // r0: .xy is the lightmap coordinate
layout(location = 1) in vec4 InTexCoord;     // r1: .xy is the material coordinate
layout(location = 2) in vec4 InAlpha;        // r2: only .w is read, straight to alpha
layout(location = 3) in vec4 InEyeVector;    // r3: tangent-space eye vector
layout(location = 4) in vec4 InLightVector;  // r4: tangent-space light vector
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
    if (mode == 2u) return v * 2.0 - 1.0;          // kBiased
    if (mode == 3u) return DecodeTextureGamma(v);  // kGamma
    return v;                                      // kUnsigned, kSigned
}

// The integer-scale step the translator emits after the sign decode. Guarded on
// the whole word being non-zero exactly as the translated shader guards it, so it
// is inert when the guest programs no scale -- which is the case on every draw
// measured so far, and is precisely why it must be written from the module rather
// than from observed behaviour.
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

// dp3 in the Xenos's association: (a0*b0 + a1*b1) + a2*b2, in the operand order
// the microcode names, with no contraction. `dot()` is free to reassociate.
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

void main()
{
    const vec2 uv = InTexCoord.xy;
    const vec2 lmUv = InLightMapUV.xy;

    const vec3 T3 = Fetch(TexSpecular,  SmpSpecular,  uv, 3u).xyz;
    const vec2 T1 = Fetch(TexDetailNrm, SmpDetailNrm, uv * kDetailUvScale, 1u).xy;
    const vec3 T0 = Fetch(TexNormal,    SmpNormal,    uv, 0u).xyz;
    const vec3 T4 = Fetch(TexLightMap0, SmpLightMap0, lmUv, 4u).xyz;
    const vec3 T5 = Fetch(TexLightMap1, SmpLightMap1, lmUv, 5u).xyz;
    const vec3 T6 = Fetch(TexLightMap2, SmpLightMap2, lmUv, 6u).xyz;
    const vec3 T2 = Fetch(TexAlbedo,    SmpAlbedo,    uv, 2u).xyz;

    // Tangent-space frame. Both are normalised with the Xenos's `rsq` of the
    // dp3-of-itself, associated (z*z + x*x) + y*y as the microcode writes it.
    precise float eyeLenSq = Dp3(InEyeVector.zxy, InEyeVector.zxy);
    precise float lightLenSq = Dp3(InLightVector.zxy, InLightVector.zxy);
    const vec3 V = inversesqrt(abs(eyeLenSq)) * InEyeVector.xyz;
    const vec3 L = inversesqrt(abs(lightLenSq)) * InLightVector.xyz;

    // The shading normal: the base normal map plus the detail normal map, each
    // biased by a constant the guest supplies, with the z channel taken only from
    // the base map and scaled. Written in the microcode's own order -- the
    // detail map's two channels are doubled first, biased, then the base map's
    // channel is added TWICE (`add` then `add` on the same operand), which is not
    // the same rounding as multiplying by two.
    precise float nx = ((T1.x + T1.x) + K.c[7].y) + T0.x;
    nx = nx + T0.x;
    precise float ny = ((T1.y + T1.y) + K.c[7].y) + T0.y;
    ny = ny + T0.y;
    precise float nz = kNormalZScale * ((T0.z + T0.z) + K.c[7].x);

    precise float nLenSq = (nx * nx + ny * ny) + kZero;
    nLenSq = nz * nz + nLenSq;
    const float nInv = inversesqrt(abs(nLenSq));
    const vec3 N = vec3(nx, ny, nz) * nInv;

    // The reflection of the eye vector about the normal, as three `dp2add`s:
    // 2*(N.V)*N - V, with the doubling written as x*y + x*y because that is the
    // instruction the sequencer has.
    precise float NdotV = Dp3(vec3(N.z, N.x, N.y), vec3(V.z, V.x, V.y));
    precise vec3 R = vec3((N.x * NdotV + N.x * NdotV) - V.x,
                          (N.y * NdotV + N.y * NdotV) - V.y,
                          (N.z * NdotV + N.z * NdotV) - V.z);

    // The three lightmap basis weights, squared. Basis 0 and 1 differ only in
    // their first coefficient; basis 2 is the one with a zero component, so it is
    // a two-term dot rather than three.
    const vec3 Nb = vec3(N.x * K.c[7].w, N.y * K.c[7].w, N.z * K.c[7].z);
    const vec3 Rb = vec3(R.x * K.c[7].w, R.y * K.c[7].w, R.z * K.c[7].z);
    const vec3 basis0 = vec3(K.c[9].x, K.c[9].y, K.c[9].z);
    const vec3 basis1 = vec3(K.c[9].w, K.c[9].y, K.c[9].z);

    float bN0 = clamp(Dp3(Nb, basis0), 0.0, 1.0);
    float bN1 = clamp(Dp3(Nb, basis1), 0.0, 1.0);
    precise float bN2raw = (N.y * kBasis2Y + N.z * kBasis2Z) + kZero;
    float bN2 = clamp(bN2raw, 0.0, 1.0);
    float bR0 = clamp(Dp3(Rb, basis0), 0.0, 1.0);
    float bR1 = clamp(Dp3(Rb, basis1), 0.0, 1.0);
    precise float bR2raw = (R.y * kBasis2Y + R.z * kBasis2Z) + kZero;
    float bR2 = clamp(bR2raw, 0.0, 1.0);

    bN0 *= bN0; bN1 *= bN1; bN2 *= bN2;
    bR0 *= bR0; bR1 *= bR1; bR2 *= bR2;

    // The lightmap coefficients, scaled. Every swizzle in these six instructions
    // cancels -- LightMapScale[i] multiplies its texture component for component.
    const vec3 LM0 = kLightMapScale0 * T4;
    const vec3 LM1 = kLightMapScale1 * T5;
    const vec3 LM2 = kLightMapScale2 * T6;

    // The material's diffuse scale, and the albedo it multiplies.
    const vec3 D = vec3(kOne) - kAmbient;
    const vec3 DA = D * T2;

    // The two dynamic directional terms. Neither is saturated before squaring:
    // the microcode squares `N.L * scale + offset` directly.
    precise float NdotL = Dp3(vec3(L.z, L.x, L.y), vec3(N.z, N.x, N.y));
    precise float wrapA = NdotL * kNormalZScale + kNormalZScale;
    precise float wrapB = NdotL * kWrapScaleB + kNormalZScale;
    const float wA = wrapA * wrapA;
    const float wB = wrapB * wrapB;

    // THE ACCUMULATION ORDER IS THE SEQUENCER'S. Six alternating steps, diffuse
    // then specular, basis 2 then 1 then 0; then the dynamic terms; then the
    // ambient constant. Reordering any of them changes the rounding.
    precise vec3 acc = (LM0 * bN2) * T2 * D + (LM0 * bR2) * T3;
    acc = ((LM1 * bN1) * T2) * D + acc;
    acc = (LM1 * bR1) * T3 + acc;
    acc = ((LM2 * bN0) * T2) * D + acc;
    acc = (LM2 * bR0) * T3 + acc;

    precise vec3 dynamic = (DA * wA) * kLightA + (DA * wB) * kLightB;
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
