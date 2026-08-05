#version 450
// Native pass: the startup movie's YUV -> RGB composite.
//
// THE OPERATION IS THE TITLE'S, THE VALUES ARE THE GAME'S. Written from the
// title's own microcode (33 dwords, fully decoded below) rather than translated
// from it. Every constant this shader multiplies by is read from the guest's own
// float-constant UBO -- a native pass reproduces the arithmetic, not the numbers.
//
//   /*  2 */ tfetch2D r2.x___, r0.xy, tf0          // Y plane
//   /*  3 */ tfetch2D r2._x__, r0.xy, tf1          // U plane
//   /*  4 */ tfetch2D r2.__x_, r0.xy, tf2          // V plane
//   /*  5 */ mul  r0._y__, c1.wwww, c3.xxxx    + maxs r0._,      c0.ww
//   /*  6 */ dp3  r1.x___, r2.zxyy, c0.zxyy    + muls_prev r0.x, c3.x
//   /*  7 */ dp3  r1._y__, r2.zxyy, c1.zxyy    + maxs r0._,      c2.ww
//   /*  8 */ dp3  r1.__z_, r2.zxyy, c2.zxyy    + muls_prev r0.z, c3.x
//   /*  9 */ add  oC0.xyz_, r1.xyzz, r0.xyzz   + maxs oC0.___w,  c3.ww
//
// The scalar pipe is a chain through the previous-scalar register: `maxs c0.ww`
// leaves c0.w in PS, the next `muls_prev c3.x` multiplies it, so the three colour
// offsets are (c0.w, c1.w, c2.w) * c3.x -- r0.y is computed by the vector pipe and
// r0.x/r0.z by the scalar pipe, which is why they look asymmetric in the listing.
//
// THE BINDINGS ARE NOT A CHOICE. They are the layout the translator emits for a
// shader with three texture fetches, and this module is substituted into a
// pipeline built for that layout, so it must match exactly:
//   set 3: binding 0/1 = texture0 unsigned/signed, 2/3 = texture1, 4/5 = texture2,
//          bindings 6,7,8 = the three samplers.
//   set 1: binding 0 = system constants, binding 2 = pixel float constants.
// Getting this wrong is not a compile error -- it samples a different image and
// still draws a plausible picture, which is exactly how the first version of this
// shader shipped with the right geometry and the wrong colours.
//
// AND THEY ARE 2D ARRAYS. The translator declares `OpTypeImage %float 2D 0 1 0 1`
// (Arrayed = 1) and this renderer binds guest textures as
// VK_IMAGE_VIEW_TYPE_2D_ARRAY. `texture2D` drew identical pixels and was still the
// wrong view type, which only Vulkan validation could see.
layout(set = 3, binding = 0) uniform texture2DArray TexY;
layout(set = 3, binding = 2) uniform texture2DArray TexU;
layout(set = 3, binding = 4) uniform texture2DArray TexV;
layout(set = 3, binding = 6) uniform sampler SampY;
layout(set = 3, binding = 7) uniform sampler SampU;
layout(set = 3, binding = 8) uniform sampler SampV;

// The system constants, declared by explicit offset rather than by repeating all
// 32 members: only the two the epilogue needs are named, and an offset that ever
// stops matching the translator's block is a mismatch the A/B comparison catches.
layout(set = 1, binding = 0) uniform XeSystemConstants {
    layout(offset = 0)   uint flags;
    layout(offset = 192) vec4 color_exp_bias;   // one per render target; .x is colour 0
} Sys;

// Sized to FOUR, not 256: the translator emits exactly the constants the shader
// touches (c0..c3 here), so the buffer really is 64 bytes and a larger array would
// read past it.
layout(set = 1, binding = 2) uniform XeFloatConstants { vec4 c[4]; } Consts;

// The fetch constants, for the texture size the GUEST programmed rather than the
// host image's extent. Constant k's dword d is at [(6k+d)/4][(6k+d)%4]: the size
// is dword 6k+2 (two 13-bit fields holding size-1) and the LOD bias is dword
// 6k+4, bits 12..21, signed, in 1/32 steps.
layout(set = 1, binding = 4) uniform XeFetchConstants { uvec4 f[48]; } Fetch_;

// Interpolator 0. The guest's vertex shader puts the movie's texture coordinate in
// r0.xy of the pixel shader, which is interpolator 0's xy.
layout(location = 0) in vec4 InInterpolator0;
layout(location = 0) out vec4 OutColor;

// kSysFlag_ConvertColor0ToGamma. The render target is a gamma-space format, so the
// shader's linear output is encoded on the way out -- with the 360's piecewise
// linear curve, NOT sRGB. Skipping this does not fail; it just makes every pixel
// the wrong brightness, so it belongs in the native pass and not in a later "fix".
const uint kConvertColor0ToGamma = 0x4000u;

vec3 EncodePwlGamma(vec3 linear)
{
    vec3 v = clamp(linear, 0.0, 1.0);
    // Four segments, selected by two comparisons: the same breakpoints and scales
    // the hardware's gamma ramp uses.
    bvec3 hi   = greaterThanEqual(v, vec3(0.500488758));
    bvec3 mid  = greaterThanEqual(v, vec3(0.0625610948));
    bvec3 upper = greaterThanEqual(v, vec3(0.12512219));
    vec3 scaleHi   = mix(vec3(255.75), vec3(127.875), vec3(hi));
    vec3 offsetHi  = mix(vec3(0.250980407), vec3(0.501960814), vec3(hi));
    vec3 scaleLo   = mix(vec3(1023.0), vec3(511.5), vec3(mid));
    vec3 offsetLo  = mix(vec3(0.0), vec3(0.125490203), vec3(mid));
    vec3 scale  = mix(scaleLo, scaleHi, vec3(upper));
    vec3 offset = mix(offsetLo, offsetHi, vec3(upper));
    return trunc(v * scale) * (1.0 / 255.0) + offset;
}

// The Xenos samples at texel centres; a normalised coordinate landing exactly on a
// texel boundary must round the same way the console did, so a fixed fraction of a
// texel is added before sampling. This mirrors what the translator emits (it reads
// the size from the fetch constant; textureSize is the same number). It changed no
// pixel on the capture this pass is verified against, so it is carried for
// faithfulness, not because it was measured to matter.
const float kTexelRoundingOffset = 0.75 / 512.0;

// dp3, in the Xenos's association and with no contraction.
//
// `dot()` would be the obvious spelling and is WRONG here by one part in 2^24: the
// compiler may fuse or reassociate it, the sequencer may not, and the difference
// crosses an 8-bit rounding boundary on a few pixels per frame. Four, measured, on
// the capture this pass is checked against -- small enough to dismiss as noise,
// which is exactly why it is worth spelling out instead. `precise` forbids both
// transforms, so this evaluates as (x*a + y*b) + z*c, the order the microcode does.
float Dp3(vec3 a, vec3 b)
{
    precise vec3 product = a * b;
    precise float sum = (product.x + product.y) + product.z;
    return sum;
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

// Explicit COARSE gradients, as the translator emits them. `texture()` would let
// the driver pick its own derivative precision instead.
float SampleR(texture2DArray tex, sampler samp, vec2 uv, uint k)
{
    const vec2 c = uv + kTexelRoundingOffset / FetchSize(k);
    const float s = FetchLodScale(k);
    return textureGrad(sampler2DArray(tex, samp), vec3(c, 0.0),
                       dFdxCoarse(c) * s, dFdyCoarse(c) * s).x;
}

void main()
{
    vec2 uv = InInterpolator0.xy;

    // ASSUMPTION, STATED SO IT CAN FAIL LOUDLY: the three planes are fetched
    // unsigned, with no exponent bias and no integer scale. The translated shader
    // carries all three as branches on the fetch constants; this pass takes the
    // straight path. If that is wrong for this draw the A/B difference against the
    // translated pass will not reach zero, which is the only reason to believe it.
    float y = SampleR(TexY, SampY, uv, 0u);
    float u = SampleR(TexU, SampU, uv, 1u);
    float v = SampleR(TexV, SampV, uv, 2u);

    // dp3 against r2.zxyy: the operand order is (V, Y, U), matched by c*.zxy.
    vec3 vyu = vec3(v, y, u);
    vec3 rgb = vec3(Dp3(vyu, Consts.c[0].zxy),
                    Dp3(vyu, Consts.c[1].zxy),
                    Dp3(vyu, Consts.c[2].zxy));

    rgb += vec3(Consts.c[0].w, Consts.c[1].w, Consts.c[2].w) * Consts.c[3].x;

    // The epilogue every pixel shader gets: the render target's exponent bias,
    // then the gamma encode when the target is a gamma format.
    float alpha = Consts.c[3].w;
    rgb *= Sys.color_exp_bias.x;
    alpha *= Sys.color_exp_bias.x;
    if ((Sys.flags & kConvertColor0ToGamma) != 0u)
        rgb = EncodePwlGamma(rgb);

    OutColor = vec4(rgb, alpha);
}
