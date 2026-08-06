#version 450
// A DIAGNOSTIC substitution, not a native pass. It renders no UE3 pass and must
// never appear in the native-pass roster: it exists to answer one question that
// nothing else in this project can see.
//
// ============================ HOW TO USE THIS ============================
//
//   1. edit the body,
//   2. RUN tools/gen_native_spv.sh runtime/shaders/debug_interpolator.frag \
//          runtime/debug_interp_spv.h DebugInterpolatorSpirv
//   3. rebuild, then GEARS_DRAW_DEBUG_INTERP=f662d670789bfac0.
//
// STEP 2 IS NOT OPTIONAL AND HAS BEEN SKIPPED BEFORE. The runtime compiles
// runtime/debug_interp_spv.h, a GENERATED blob -- NOT this file. Four
// measurements in one session were taken against a blob built from a different
// body, and read as answers (catalog #77). If a run's numbers are identical to
// the previous run's after you changed the body, that is the symptom.
//
// Read it with GEARS_DRAW_SURFACE_DUMP=<diag>, which copies the whole surface
// in its own float format immediately after the draw. Do NOT read it out of the
// presented screenshot: that is post-resolve and post-tonemap, and reading it
// there produced two retracted measurements.
//
// ======================= WHAT THIS BUILD EMITS ===========================
//
//   R = gate     saturate(0.3 - normalize(o2).z)   instruction 25
//   G = ramp     the tf1 env-ramp fetch             instruction 18
//   B = -7       THE COVERAGE SENTINEL (see below)
//   A = 1
//
// The material is, at the export,
//
//     oC0.xyz = (albedo' * ramp * (0.8,0.8,1.0) * gate * T7 + c6.xyz) * c254.w
//
// with c6.xyz = (0,0,0) and c254.w = 8. So those four ARE the multiplicands: if
// the character is black, at least one of them is zero on its pixels, and this
// build reads all four at once on the draw's own fragments.
//
// ==================== WHY THIS IS A PORT, NOT A MODEL ====================
//
// Earlier builds REPLICATED parts of the material -- the gate from the
// instruction listing (which drops the `+ c254.x` and made it read "open" when
// it is shut), and the ramp coordinate as a 1D lookup (it is 2D: instruction 18
// fetches at r4.zy). Both produced retracted findings.
//
// This build instead ports instructions 3..25 one for one, and reads the
// constants from the SAME UBO the translated shader is handed
// (set 1 binding 2, ascending storage index) rather than pasting measured
// numbers in. There is nothing left to get wrong about a constant, and the
// rotating swizzles -- which this material uses on nearly every instruction --
// are transcribed rather than simplified.
//
// Packed constant order for this shader, 10 vec4s, ascending storage index:
//   K.c[0..5] = c0..c5   K.c[6] = c6   K.c[7] = c253   K.c[8] = c254   K.c[9] = c255
//
// SENTINEL MODE. Define DEBUG_SENTINEL to emit a constant vec4(-7) instead.
// Nothing else in a frame writes -7, so the dumped surface's -7 pixels are
// EXACTLY this draw's coverage -- which is how a reading gets a denominator
// instead of a hand-drawn rectangle. Averaging a rectangle over the screenshot
// is what contaminated three earlier attempts: the box was mostly wall.

layout(set = 3, binding = 0) uniform texture2DArray Tf0Tex;   // the normal map
layout(set = 3, binding = 2) uniform texture2DArray Tf1Tex;   // the env ramp
layout(set = 3, binding = 4) uniform texture2DArray Tf2Tex;   // the diffuse
layout(set = 3, binding = 6) uniform sampler       Tf0Samp;
layout(set = 3, binding = 7) uniform sampler       Tf1Samp;
layout(set = 3, binding = 8) uniform sampler       Tf2Samp;

layout(set = 1, binding = 2) uniform XeFloatConstants { vec4 c[10]; } K;

layout(location = 0) in vec4 InR0;   // r0: .xy is the material coordinate
layout(location = 2) in vec4 InR2;   // r2: the vector the gate is built from
layout(location = 4) in vec4 InR4;   // r4: the tangent-space normal, packed

layout(location = 0) out vec4 OutColor;

void main() {
    // Constants, named as the microcode names them.
    vec4 c0 = K.c[0], c1 = K.c[1], c2 = K.c[2];
    vec4 c3 = K.c[3], c4 = K.c[4], c5 = K.c[5];
    vec4 c253 = K.c[7], c254 = K.c[8], c255 = K.c[9];

    vec4 r0 = InR0, r2 = InR2, r4 = InR4, r5 = vec4(0.0);

    // 3:  tfetch2D r0.__z_, r0.xy, tf0
    r0.z = texture(sampler2DArray(Tf0Tex, Tf0Samp), vec3(r0.xy, 0.0)).z;
    // 4:  dp3 r5.___w, r2.zxyy, r2.zxyy
    r5.w = dot(r2.zxy, r2.zxy);
    // 5:  mad r5.xyz_, r4.xyzz, c253.xxxx, c253.yyyy
    r5.xyz = r4.xyz * c253.x + c253.y;
    // 6:  mad r4.___w, r0.zzzz, c253.xxxx, c253.yyyy      <- T7
    r4.w = r0.z * c253.x + c253.y;
    // 7:  dp3 r4._y__, r5.zxyy, r5.zxyy  +  rsq r4.x___, r_abs[5].w
    //     Both slots read the PRE-instruction registers, so r4.y is computed
    //     from r5.xyz and r4.x from r5.w in the same step.
    float i7y = dot(r5.zxy, r5.zxy);
    float i7x = inversesqrt(max(abs(r5.w), 1e-30));
    r4.y = i7y; r4.x = i7x;
    // 8:  mul r4.x___, r4.xxxx, r2.zzzz  +  rsq r4._y__, r_abs[4].y
    float i8x = r4.x * r2.z;                              // normalize(o2).z
    float i8y = inversesqrt(max(abs(r4.y), 1e-30));
    r4.x = i8x; r4.y = i8y;
    // 9:  mul r5.xyz_, r5.zxyy, r4.yyyy      -> normalize(o4*2-1), rotated
    r5.xyz = r5.zxy * r4.y;
    // 10: mul r2.xyz_, r5.zxyy, r4.wwww
    r2.xyz = r5.zxy * r4.w;
    // 11: mul r5.xyz_, r2.xxxx, c1.zyxx
    r5.xyz = r2.x * vec3(c1.z, c1.y, c1.x);
    // 12: mad r5.xyz_, r2.zzzz, c0.xzyy, r5.zxyy
    r5.xyz = r2.z * vec3(c0.x, c0.z, c0.y) + r5.zxy;
    // 13: mad r5.xyz_, r2.yyyy, c2.yxzz, r5.zxyy
    r5.xyz = r2.y * vec3(c2.y, c2.x, c2.z) + r5.zxy;
    // 14: mul r4._yz_, r5.xxxx, c4.yyxx
    float i14y = r5.x * c4.y, i14z = r5.x * c4.x;
    r4.y = i14y; r4.z = i14z;
    // 15: mad r4._yz_, r5.yyyy, c3.xxyy, r4.zzyy
    float i15y = r5.y * c3.x + r4.z, i15z = r5.y * c3.y + r4.y;
    r4.y = i15y; r4.z = i15z;
    // 16: mad r4._yz_, r5.zzzz, c5.yyxx, r4.zzyy
    float i16y = r5.z * c5.y + r4.z, i16z = r5.z * c5.x + r4.y;
    r4.y = i16y; r4.z = i16z;
    // 17: add r4.xyz_, r4.xyzz, c254.xyyy
    r4.xyz = r4.xyz + vec3(c254.x, c254.y, c254.y);
    float coordU = r4.z, coordV = r4.y;   // the fetch coordinate itself
    // 18: tfetch2D r5.xyz_, r4.zy, tf1     <- the env ramp, a 2D fetch
    r5.xyz = texture(sampler2DArray(Tf1Tex, Tf1Samp), vec3(r4.z, r4.y, 0.0)).xyz;
    // 19: tfetch2D r0.xyz_, r0.xy, tf2     <- the diffuse
    r0.xyz = texture(sampler2DArray(Tf2Tex, Tf2Samp), vec3(r0.xy, 0.0)).xyz;
    // 20: dp3 r5.___w, r0.zxyy, c255.xyzz  <- luminance of the diffuse
    r5.w = dot(r0.zxy, c255.xyz);
    // 25: subsc_sat r4.x___, c254.y, r4.x  <- THE GATE
    float gate = clamp(c254.y - r4.x, 0.0, 1.0);

#ifdef DEBUG_SENTINEL
    // A value nothing else in the frame writes, so the dump's -7 pixels are
    // exactly this draw's coverage and the reading below gets a denominator.
    OutColor = vec4(-7.0);
#else
    // BLUE IS THE COVERAGE SENTINEL. Nothing else in a frame writes -7, so the
    // dump's B == -7 pixels are EXACTLY this draw's fragments, in the SAME run
    // that carries the values -- every reading gets its own denominator, which
    // is what three retracted measurements lacked (they averaged a hand-drawn
    // rectangle of the screenshot that was mostly wall).
    //
    // NOT alpha, which would be the natural place: this title's colour surfaces
    // are k_2_10_10_10 variants whose guest clamp is ALPHA-ONLY, so a sentinel
    // written there comes back as 0 and the mask silently reads empty. Measured,
    // not assumed -- the alpha version of this shader dumped 0 covered pixels.
    OutColor = vec4(gate, r5.x, -7.0, 1.0);
#endif
}
