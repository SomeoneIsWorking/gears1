#version 450
// Native pass: the startup movie's YUV -> RGB composite.
//
// Written from the title's own microcode, which is 33 dwords and fully decoded:
//   tfetch2D r2.x, r0.xy, tf0      // Y plane
//   tfetch2D r2.y, r0.xy, tf1      // U plane
//   tfetch2D r2.z, r0.xy, tf2      // V plane
//   dp3 r1.x, r2.zxy, c0.zxy   +   const offset from c3.x * c1.w  etc
//   dp3 r1.y, r2.zxy, c1.zxy
//   dp3 r1.z, r2.zxy, c2.zxy
//   add oC0.xyz, r1.xyz, r0.xyz    // the per-channel offsets
//   maxs oC0.w, c3.ww
//
// The matrix and the offsets are the GUEST'S, read from its float-constant UBO --
// this pass reproduces the arithmetic, not the numbers. That is the whole point of
// a native pass: the operation comes from the engine, the values come from the game.
layout(set = 3, binding = 0) uniform texture2D Tex0;
layout(set = 3, binding = 1) uniform texture2D Tex1;
layout(set = 3, binding = 2) uniform texture2D Tex2;
layout(set = 3, binding = 3) uniform sampler Samp0;
layout(set = 3, binding = 4) uniform sampler Samp1;
layout(set = 3, binding = 5) uniform sampler Samp2;

layout(set = 1, binding = 2) uniform PixelConstants { vec4 c[4]; } Consts;

layout(location = 0) in vec2 InUV;
layout(location = 0) out vec4 OutColor;

void main()
{
    float y = texture(sampler2D(Tex0, Samp0), InUV).x;
    float u = texture(sampler2D(Tex1, Samp1), InUV).x;
    float v = texture(sampler2D(Tex2, Samp2), InUV).x;

    // The microcode's dp3 operands are r2.zxy = (v, y, u), against c0/c1/c2.zxy.
    vec3 yuv = vec3(v, y, u);
    vec3 rgb;
    rgb.x = dot(yuv, Consts.c[0].zxy);
    rgb.y = dot(yuv, Consts.c[1].zxy);
    rgb.z = dot(yuv, Consts.c[2].zxy);

    // The offsets the microcode adds: c1.w * c3.x on green, c3.x on red and blue,
    // each already scaled by the guest.
    vec3 offset = vec3(Consts.c[0].w, Consts.c[1].w, Consts.c[2].w) * Consts.c[3].x;
    OutColor = vec4(rgb + offset, Consts.c[3].w);
}
