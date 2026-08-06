#version 450
// A DIAGNOSTIC substitution, not a native pass. It renders no UE3 pass and must
// never appear in the native-pass roster: it exists to answer one question that
// nothing else in this project can see.
//
// THE QUESTION (catalog #77). The character renders pure black. Its base pass,
// ps 0xf662d670789bfac0, emits
//
//     oC0.xyz = ((r5 * gate) * r4.w) * c254.w        c254.w = 8
//     gate    = saturate(1 - normalize(r2.xyz).z)    r2 = interpolator o2
//     r4.w    = tf0.z * 2 - 1                        tf0 = the NORMAL MAP
//
// so with c254.w a bright multiplier, only `gate`, `r4.w` or `r5` can zero it.
// Eight candidate causes have been eliminated by measurement (catalog #77), and
// the first version of THIS shader eliminated the gate: it reads 0.32-0.42 on
// the character, i.e. wide open. So the remaining multipliers are r4.w and r5.
//
// r5 is `albedo * tf1`, where tf1's sample coordinate is computed by twelve
// instructions from c0..c5. Replicating those here would risk a confidently
// wrong answer, so this shader does NOT attempt it -- it reads the two things it
// can read exactly, and the tf1 lead is chased separately.
//
// Interpolator location N is register rN, exactly as the translated shader
// receives them (runtime/shaders/base_pass_lightmap.frag reads r0..r4 the same
// way), and texture bindings follow FETCH ISSUE ORDER within set 3, textures on
// even bindings and their samplers from 6 -- the convention
// runtime/shaders/uber_post_blend.frag documents. This shader's issue order is
// tf0 (instruction 3), tf1 (18), tf2 (19), so tf0 is binding 0 / sampler 6.
//
// WHAT EACH CHANNEL SHOWS:
// BOTH multipliers have now been read and both are healthy -- r4.w is 0.92-1.0
// and the gate is 0.32-0.42, so gate*r4.w*c254.w is about 2.6, a BRIGHT
// multiplier. Since the material's output is exactly zero, arithmetic forces
// r5 = albedo * tf1 to be zero, and the albedo decodes to real character art.
// So tf1's sample must be zero, and this build asks the next question: is that
// the BINDING or the COORDINATE?
//
//   R = tf1 sampled at a FIXED (0.5, 0.5). This is a BINDING test and owes
//       nothing to the shader's computed coordinate. tf1 (0x32eb000) is a
//       256x256 k_8 ramp measured 50.8% zero, 30.8% at 255, mean 99.5/255 =
//       0.39 -- so a healthy binding reads SOMETHING here on most of the mesh,
//       and a flat 0 means the fetch itself is broken (format, swizzle XXX1 on
//       a single-channel texture, or the descriptor).
//   G = tf1 sampled at the material coordinate r0.xy. NOT the shader's real
//       coordinate (that is twelve instructions of c0..c5 this refuses to
//       replicate) -- it is a second, independent probe of the same binding.
//   B = the gate, kept so one image still carries a known-good reference and a
//       run cannot be confused with the previous build's output.

layout(set = 3, binding = 0) uniform texture2DArray Tf0Tex;   // the normal map
layout(set = 3, binding = 2) uniform texture2DArray Tf1Tex;   // the k_8 ramp
layout(set = 3, binding = 6) uniform sampler       Tf0Samp;
layout(set = 3, binding = 7) uniform sampler       Tf1Samp;

layout(location = 0) in vec4 InR0;   // r0: .xy is the material coordinate
layout(location = 2) in vec4 InR2;   // r2: the vector the gate is built from
layout(location = 0) out vec4 OutColor;

void main() {
    vec3 r2 = InR2.xyz;
    float len = length(r2);
    // Guard the divide rather than emitting NaN: a NaN paints whatever the
    // blend does with it and reads as an answer. Zero length is a real possible
    // finding, so it gets a defined output instead of being hidden.
    float nz = len > 0.0 ? (r2.z / len) : 0.0;
    float gate = clamp(1.0 - nz, 0.0, 1.0);

    // The layer is 0: the translated shader declares its 2D textures as arrays,
    // so a plain 2D sample here would not match the descriptor and the pipeline
    // would be rejected rather than quietly reading the wrong thing.
    float tf1_fixed = texture(sampler2DArray(Tf1Tex, Tf1Samp), vec3(0.5, 0.5, 0.0)).x;
    float tf1_uv    = texture(sampler2DArray(Tf1Tex, Tf1Samp), vec3(InR0.xy, 0.0)).x;

    OutColor = vec4(tf1_fixed, tf1_uv, gate, 1.0);
}
