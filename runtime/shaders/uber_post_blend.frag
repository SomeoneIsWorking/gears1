#version 450
// Native pass: UE3's uber post-process blend -- depth-of-field composite, then
// the scene colour transform, then the output gamma.
//
// WHAT THIS PASS IS. It is the last thing that touches the frame's colour before
// motion blur, and it is where every "the picture is the wrong colour" question
// in this project actually lives. `tools/pass_structure.py` puts it at the end of
// the post chain in every gameplay capture (courtyard draw 716, act1_v2 draw 76),
// a two-primitive full-screen draw with the depth test off, sampling the resolved
// scene colour, a blurred copy of it, and the resolved scene depth.
//
// THE TITLE'S OWN MICROCODE, decoded in full (scratch/shaders/bound_out/
// ps_9610bf8038af9aaf.ucode.txt). Instruction numbers are the sequencer's:
//
//    4  tfetch2D r2,      r0.xy, tf2   // scene colour, all four channels
//    5  tfetch2D r3.xyz_, r1.xy, tf0   // the blurred copy
//    6  tfetch2D r0.x___, r1.xy, tf1   // scene depth, as the guest stored it
//    7  mad   r0.x, r0.x, c0.z, -c0.w
//    8  rcp   r0.x, r0.x                     // UE3 ConvertFromDeviceZ
//    9  add   r0.x, r0.x, -c1.x              // SceneDepth - FocusDistance
//   10  cndgt r0.y, -r0.x, c2.x, c2.y        // in front? MaxNear : MaxFar
//   11  mul_sat r0.x, |r0.x|, c1.y           // * InverseFocusRadius
//   12  log   r0.x, r0.x
//   13  mulsc r0.x, c1.z, r0.x               // ^ FocusExponent
//   14  exp   r0.x, r0.x
//   15  min   r0.y, r0.y, r0.x               // clamp to the max blur
//   16  subsc_sat r0.w, c255.x, r0.y         // 1 - blur  (c255.x is 1.0)
//   17  mul   r0.xyz, r0.w, r3.xyz
//   18  add   r1, r0.zyxw, r2.zyxw
//   19  rcp   r0.x, r1.w
//   20  mad_sat r0.xyz, r1.yzxx, r0.xxxx, -c3.yxzz
//   21  mul   r1.xyz, r0.zxyy, c4.zyxx
//   22  log   r0.x, |r1.x|      (and .y, .z from |r1.y|, |r1.z|)
//   25  mul   r0.xyz, r0.xyzz, c5.zyxx
//   26  exp   r1.x, r0.z        (and .y, .z from r0.y, r0.x)
//   29  mad   r0._yzw, r1.xxyz, c3.wwww, c8.xxyz
//   30  dp3   r0.x, r1.zxyy, c6.zxyy
//   31  add   r0.xyz, r0.zyww, r0.xxxx
//   32  mul_sat r1.xyz, r0.yxzz, c7.xyzz
//   33  log   r0.xyz  (no abs -- the value is already saturated)
//   36  mul   r0.xyz, r0.xyzz, c7.wwww
//   37  exp   oC0.xyz            + max oC0._000  // alpha is 0
//
// THE SWIZZLES CANCEL, EVERY TIME, and saying so is the reading. Instruction 18
// writes `(fog + scene).zyx`; 20 reads `r1.yzx` and subtracts `c3.yxz`; 21 reads
// `r0.zxy` and multiplies `c4.zyx`; 26 reads the three logs back in reverse. Each
// pair composes to the identity, so the whole chain is per-channel arithmetic in
// the original channel order. Transcribing the permutations would be correct and
// unreadable; reducing them is the understanding, and the A/B gate against the
// translated shader is what makes the reduction safe to trust.
//
// WHAT IT IS, IN UE3's VOCABULARY. Instructions 7-16 are `CalcUnfocusedPercent`:
// device Z to scene depth, distance from the focus plane, raised to a falloff
// exponent, clamped to a near or far maximum depending on which side of focus the
// pixel is. 17-19 blend the blurred copy against the sharp one and divide by the
// accumulated weight the sharp target carries in its alpha. 20-32 are the scene
// colour transform: subtract shadows, scale by highlights, raise to midtones,
// then a desaturation written as a luminance dot product plus a scaled original.
// 33-37 are the final gamma. The names are UE3's; the numbers are all the guest's,
// read from its own float-constant UBO -- a native pass reproduces the OPERATION.
//
// THE BINDINGS ARE NOT A CHOICE, and here they are NOT in fetch-constant order.
// The translator numbers texture bindings by ORDER OF FIRST USE, and this shader
// fetches tf2 first. Read off the real translated module for this draw
// (GEARS_DRAW_SPV_DUMP, then spirv-dis):
//   set 3: 0/1 = texture2 unsigned/signed, 2/3 = texture0, 4/5 = texture1,
//          6 = sampler2, 7 = sampler0, 8 = sampler1.
// Assuming tf0 lands on binding 0 -- which is what the movie pass's layout looks
// like at a glance -- samples the wrong image and still draws a plausible picture.
//
// AND THEY ARE 2D ARRAYS, not 2D. The translator declares
// `OpTypeImage %float 2D 0 1 0 1` -- Arrayed = 1 -- and samples with a
// three-component coordinate whose layer is 0. The pipeline's descriptor layout
// is built from that module, so a `texture2D` here is the wrong view type for
// the descriptor it is handed.
layout(set = 3, binding = 0) uniform texture2DArray SceneColorTex;   // tf2
layout(set = 3, binding = 2) uniform texture2DArray BlurredTex;      // tf0
layout(set = 3, binding = 4) uniform texture2DArray SceneDepthTex;   // tf1
layout(set = 3, binding = 6) uniform sampler SceneColorSamp;
layout(set = 3, binding = 7) uniform sampler BlurredSamp;
layout(set = 3, binding = 8) uniform sampler SceneDepthSamp;

// THE FETCH CONSTANTS, because the texture's size is the GUEST's number and not
// the host image's. The translator reads width and height out of fetch constant
// dword 2 (13 bits each, stored as size-1) and uses THAT to scale the texel
// rounding offset. `textureSize()` returns the host image's extent instead, and
// the two agree only as long as nothing pads or re-rounds an allocation --
// which is not a property this shader gets to assume.
//
// Xenos fetch constants are SIX dwords each, packed here as a uvec4 array, so
// constant k's dword d lives at [(6k+d)/4][(6k+d)%4]:
//   size      = dword 6k+2   -> tf0 [0][2], tf1 [2][0], tf2 [3][2]
//   lod bias  = dword 6k+4, bits 12..21, signed, in 1/32 steps
layout(set = 1, binding = 4) uniform XeFetchConstants { uvec4 f[48]; } Fetch_;

layout(set = 1, binding = 0) uniform XeSystemConstants {
    layout(offset = 0)   uint  flags;
    // Post-swizzle TextureSign, two bits per component, eight bits per fetch
    // constant, four fetch constants per uint. This pass samples fetch constants
    // 0, 1 and 2, so all three live in element 0's x -- at bit 0, bit 8 and bit
    // 16. Verified against the translated module, which extracts exactly those
    // (OpBitFieldUExtract at 16/18/20/22 for tf2 and 0/2/4 for tf0).
    layout(offset = 64)  uvec4 texture_swizzled_signs[2];
    layout(offset = 192) vec4  color_exp_bias;
} Sys;

// PACKED, NOT INDEXED BY REGISTER NUMBER. The microcode names c0..c8 and c255;
// the translator emits a TEN-entry block holding exactly those in ascending
// register order, so c255 is at index 9. Confirmed: the module declares
// `%_arr_v4float_uint_10`. Indexing c[255] reads 4 KiB past a 160-byte buffer.
layout(set = 1, binding = 2) uniform XeFloatConstants { vec4 c[10]; } K;
#define kDepthScale     K.c[0].z   // c0.z  ConvertFromDeviceZ numerator terms
#define kDepthBias      K.c[0].w   // c0.w
#define kFocusDistance  K.c[1].x   // c1.x
#define kInvFocusRadius K.c[1].y   // c1.y
#define kFocusExponent  K.c[1].z   // c1.z
#define kMaxNearBlur    K.c[2].x   // c2.x
#define kMaxFarBlur     K.c[2].y   // c2.y
#define kShadows        K.c[3].xyz // c3.xyz
#define kOriginalWeight K.c[3].w   // c3.w   (the un-desaturated share)
#define kHighlights     K.c[4].xyz // c4.xyz
#define kMidTones       K.c[5].xyz // c5.xyz
#define kLuminance      K.c[6].xyz // c6.xyz (already scaled by 1 - desaturation)
#define kOutputScale    K.c[7].xyz // c7.xyz
#define kOutputGamma    K.c[7].w   // c7.w
#define kOffset         K.c[8].xyz // c8.xyz
#define kOne            K.c[9].x   // c255.x

layout(location = 0) in vec4 InInterp0;   // r0: .xy is the scene-colour coordinate
layout(location = 1) in vec4 InInterp1;   // r1: .xy is the blur/depth coordinate
layout(location = 0) out vec4 OutColor;

const uint kConvertColor0ToGamma = 0x4000u;
const float kTexelRoundingOffset = 0.75 / 512.0;  // 0.00146484375, the translator's own literal

// TextureSign::kGamma -- the 360's piecewise gamma-to-LINEAR decode.
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

// Mode 1 (kSigned) is a pass-through here for the same reason it is in
// scene_gamma.frag: this renderer binds the unsigned view to both slots, so the
// translated shader's signed branch samples the same image ours does. Reading the
// constant is not optional -- the translated shader branches on it, so a native
// pass that ignores it diverges silently the moment it stops being zero.
float ApplyTextureSign(float v, uint mode)
{
    if (mode == 2u) return v * 2.0 - 1.0;          // kBiased
    if (mode == 3u) return DecodeTextureGamma(v);  // kGamma
    return v;                                      // kUnsigned, kSigned
}

vec4 ApplySigns(vec4 v, uint signs)
{
    return vec4(ApplyTextureSign(v.x, (signs >> 0) & 3u),
                ApplyTextureSign(v.y, (signs >> 2) & 3u),
                ApplyTextureSign(v.z, (signs >> 4) & 3u),
                ApplyTextureSign(v.w, (signs >> 6) & 3u));
}

// Width and height of fetch constant `k`, as the guest programmed them.
vec2 FetchSize(uint k)
{
    const uint dword = 6u * k + 2u;
    const uint packed = Fetch_.f[dword >> 2][dword & 3u];
    return vec2(float(bitfieldExtract(packed, 0, 13) + 1u),
                float(bitfieldExtract(packed, 13, 13) + 1u));
}

// The fetch constant's LOD bias, as the factor the translator scales the
// derivatives by: exp2(bias / 32).
float FetchLodScale(uint k)
{
    const uint dword = 6u * k + 4u;
    const int packed = int(Fetch_.f[dword >> 2][dword & 3u]);
    return exp2(float(bitfieldExtract(packed, 12, 10)) * 0.03125);
}

vec4 Fetch(texture2DArray tex, sampler samp, vec2 uv, uint k, uint signs)
{
    // The Xenos samples at texel centres; the translator adds this fraction of a
    // texel, sized from the fetch constant.
    const vec2 c = uv + kTexelRoundingOffset / FetchSize(k);
    // EXPLICIT gradients, COARSE, scaled by the fetch constant's LOD bias -- what
    // the translator emits (OpDPdxCoarse / OpDPdyCoarse feeding
    // OpImageSampleExplicitLod ... Grad). An implicit-LOD `texture()` lets the
    // driver pick its own derivative precision, which is not the same function.
    const float lodScale = FetchLodScale(k);
    const vec2 ddx = dFdxCoarse(c) * lodScale;
    const vec2 ddy = dFdyCoarse(c) * lodScale;
    const vec4 v = textureGrad(sampler2DArray(tex, samp), vec3(c, 0.0), ddx, ddy);
    return ApplySigns(v, signs);
}

// The sequencer's log/exp pair, not pow(). They are separate instructions with
// their own approximations, and pow() is free to be a third one.
vec3 PowAbs(vec3 v, vec3 e) { return exp2(e * log2(abs(v))); }
vec3 PowSat(vec3 v, float e) { return exp2(e * log2(v)); }

// dp3 in the Xenos's association and with no contraction. Instruction 30 reads
// `r1.zxy` against `c6.zxy`, so the sum is (z + x) + y, NOT (x + y) + z. `precise`
// forbids the compiler from fusing or reassociating it back.
float Dp3Zxy(vec3 a, vec3 b)
{
    precise vec3 p = a * b;
    precise float s = p.z + p.x;
    precise float t = s + p.y;
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
    const uint signWord = Sys.texture_swizzled_signs[0].x;
    const vec4 scene   = Fetch(SceneColorTex, SceneColorSamp, InInterp0.xy,
                               2u, (signWord >> 16) & 0xFFu);
    const vec3 blurred = Fetch(BlurredTex, BlurredSamp, InInterp1.xy,
                               0u, (signWord >> 0) & 0xFFu).xyz;
    const float deviceZ = Fetch(SceneDepthTex, SceneDepthSamp, InInterp1.xy,
                                1u, (signWord >> 8) & 0xFFu).x;

    // UE3 ConvertFromDeviceZ: 1 / (DeviceZ * MinZ_MaxZRatio.z - MinZ_MaxZRatio.w).
    precise float sceneDepth = 1.0 / (deviceZ * kDepthScale - kDepthBias);
    precise float distInFront = sceneDepth - kFocusDistance;

    // cndgt: the FIRST operand is -distInFront, so the near maximum is taken when
    // the pixel is IN FRONT of the focus plane. Writing this as
    // `distInFront < 0.0` would invert it at exactly distInFront == 0.
    float maxBlur = (-distInFront > 0.0) ? kMaxNearBlur : kMaxFarBlur;
    float falloff = exp2(kFocusExponent *
                         log2(clamp(abs(distInFront) * kInvFocusRadius, 0.0, 1.0)));
    float blurAmount  = min(maxBlur, falloff);
    float sharpWeight = clamp(kOne - blurAmount, 0.0, 1.0);

    // The blurred copy weighted in, over the sharp target's own accumulated
    // weight in alpha.
    precise vec3 summed = blurred * sharpWeight + scene.xyz;
    precise float weight = sharpWeight + scene.w;

    // The scene colour transform. `mad_sat` saturates, and it saturates BEFORE the
    // highlights multiply -- moving the clamp later changes every pixel above 1.
    precise vec3 normalised = summed * (1.0 / weight);
    vec3 shadowed   = clamp(normalised - kShadows, 0.0, 1.0);
    vec3 highlights = shadowed * kHighlights;
    vec3 graded     = PowAbs(highlights, kMidTones);

    float lum = Dp3Zxy(graded, kLuminance);
    // TWO instructions, and they stay two: 29 is `mad r0._yzw, r1, c3.w, c8` and
    // 31 is `add r0.xyz, ..., r0.x`, and the sequencer rounds between them where
    // a contracted FMA would not. Marked `precise` for that reason and not
    // because it was ever measured to matter -- it was the first suspect for the
    // 20 off-by-one samples this pass shipped with and it was the WRONG one; the
    // cause was the fetch interface above. Kept because it is what the microcode
    // does, and recorded as unmeasured so nobody cites it as a fix.
    precise vec3 weighted = graded * kOriginalWeight + kOffset;
    precise vec3 desaturated = weighted + lum;

    vec3 scaled = clamp(desaturated * kOutputScale, 0.0, 1.0);
    // Instruction 33 has no `r_abs`, unlike 22: the value is already saturated, so
    // the absolute value would be a no-op AND would hide a negative if one ever
    // reached here. Kept faithful rather than defensive.
    vec3 rgb = PowSat(scaled, kOutputGamma);

    // `max oC0._000` writes zero to y, z and w before the three exps overwrite
    // x, y and z -- so alpha leaves this shader as 0, and the epilogue's exponent
    // bias multiplies it as it does the colour.
    float alpha = 0.0;

    rgb   *= Sys.color_exp_bias.x;
    alpha *= Sys.color_exp_bias.x;
    if ((Sys.flags & kConvertColor0ToGamma) != 0u)
        rgb = EncodePwlGamma(rgb);

    OutColor = vec4(rgb, alpha);
}
