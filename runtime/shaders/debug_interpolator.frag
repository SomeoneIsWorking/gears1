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
// WHAT EACH CHANNEL SHOWS (this build): o2 RAW, remapped so 0.5 is zero and
// 1.0 is +1 -- R = o2.x, G = o2.y, B = o2.z. The render target is float, so
// values outside [0,1] are NOT clipped and read back as-is; un-remap with
// (v - 0.5) * 2. Measured on the character: (-23.47, +13.96, +25.48), |o2| in
// 34..65.
//
// UE3 settles what o2 SHOULD be, and it matches: GpuSkinVertexFactory.usf:244
// (the skinned factory this mesh uses) computes TangentCameraVector as
// tangent-space (CameraPosition - WorldPosition) -- toward the camera, hence
// positive z facing the viewer, and UNNORMALISED. So o2 is correct and the
// material's saturate(0.3 - normalize(o2).z) is MEANT to be ~0 facing the
// camera: draw 460 is a rim term, not the character's diffuse.
//
// Earlier builds of this shader emitted other things; two are worth keeping in
// mind because each cost several notes on catalog #77:
//   * it once computed the gate as saturate(1 - normalize(o2).z), DROPPING the
//     + c254.x that ucode_reduce's full reduction shows (t42 = t11 + c254.x).
//     That read 0.32-0.42 -- "open" -- when the real gate is exactly 0. Read the
//     REDUCTION to its end, never the listing.
//   * it once replicated the tf1 coordinate chain to test a sign flip. That
//     experiment was sound and its conclusion was not: the ramp is downstream of
//     the gate, so lighting it proves nothing about the cause.
//
// Edit the body to emit whatever the current question needs, and edit THIS
// BLOCK in the same change -- a diagnostic whose comment describes different
// channels than it writes is the worst instrument in the tree.

layout(set = 3, binding = 0) uniform texture2DArray Tf0Tex;   // the normal map
layout(set = 3, binding = 2) uniform texture2DArray Tf1Tex;   // the k_8 ramp
layout(set = 3, binding = 6) uniform sampler       Tf0Samp;
layout(set = 3, binding = 7) uniform sampler       Tf1Samp;

layout(location = 0) in vec4 InR0;   // r0: .xy is the material coordinate
layout(location = 2) in vec4 InR2;   // r2: the vector the gate is built from
layout(location = 4) in vec4 InR4;   // r4: what the tf1 COORDINATE is built from
layout(location = 0) out vec4 OutColor;

void main() {
    vec3 r2 = InR2.xyz;
    float len = length(r2);
    // Guard the divide rather than emitting NaN: a NaN paints whatever the
    // blend does with it and reads as an answer. Zero length is a real possible
    // finding, so it gets a defined output instead of being hidden.
    float nz = len > 0.0 ? (r2.z / len) : 0.0;
    // THE REAL GATE. An earlier build of this shader computed
    // saturate(1 - nz) and measured it "open" at 0.32-0.42 -- WRONG, and it
    // sent three commits after the coordinate instead. ucode_reduce's full
    // reduction has t42 = nz + c254.x and t70 = saturate(c254.y - t42), i.e.
    //     gate = saturate(c254.y - c254.x - nz) = saturate(0.3 - nz)
    // with c254 = (0.7, 1, 0.8, 8). The +0.7 was dropped by reading the listing
    // instead of the reduction. Both forms are emitted below so the mistake
    // cannot be repeated silently.
    float gateWrong = clamp(1.0 - nz, 0.0, 1.0);
    float gate      = clamp(0.3 - nz, 0.0, 1.0);

    // The layer is 0: the translated shader declares its 2D textures as arrays,
    // so a plain 2D sample here would not match the descriptor and the pipeline
    // would be rejected rather than quietly reading the wrong thing.
    // THE COORDINATE'S ACTUAL INPUT. The gate above is built from interpolator
    // o2, but PS r2 is OVERWRITTEN at instruction 10 before the tf1 coordinate
    // is built at 11-17 -- that chain starts from o4, via
    // `mad r5.xyz, r4.xyzz, c253.xxxx, c253.yyyy` with c253 = (2, -1), i.e. the
    // usual [0,1] -> [-1,1] unpack, then a normalise. So o4 is the input the
    // failing lookup actually depends on, and o2's length says nothing about it.
    // THE COORDINATE, replicated EXACTLY from tools/ucode_reduce.py's straight-
    // line reduction of this shader (t13..t40), not from reading the listing:
    //   n   = normalize(o4*2-1), permuted   t13=n.z t14=n.x t15=n.y
    //   t7  = tf0.z*2-1
    //   t40 = t18*c5.x - t19*c3.x - t17*c4.x   with c0,c1,c2 collapsed
    //   coord.x = t40 + c254.y      (c254.y = 1)
    // Constants from GEARS_DRAW_PS_CONSTS on this draw.
    float tf0z = texture(sampler2DArray(Tf0Tex, Tf0Samp), vec3(InR0.xy, 0.0)).z;
    vec3 n = normalize(InR4.xyz * 2.0 - 1.0);
    float t7 = tf0z * 2.0 - 1.0;
    float t17 = n.y * t7, t18 = n.z * t7, t19 = n.x * t7;
    float t40 = t18 * (-0.0826) + t19 * 0.9913 - t17 * 0.1024;
    float coordX      = t40 + 1.0;
    float coordFlipped = -t40 + 1.0;   // the same chain with n.x's sign flipped

    float rampAsIs    = texture(sampler2DArray(Tf1Tex, Tf1Samp), vec3(coordX, 0.5, 0.0)).x;
    float rampFlipped = texture(sampler2DArray(Tf1Tex, Tf1Samp), vec3(coordFlipped, 0.5, 0.0)).x;

    // R = the ramp AS WE COMPUTE IT (the material's actual lookup)
    // G = the ramp with the sign flipped (what it would read if o4.x were negative)
    // B = coordX/2, so the coordinate itself is legible: >0.5 here means >1.0,
    //     i.e. clamped past the end of the ramp.
    // R = the REAL gate, saturate(0.3 - normalize(o2).z). Zero here IS the bug.
    // G = normalize(o2).z remapped (0.5 = zero), the value the gate turns on.
    // B = the old, wrong gate, kept so a run cannot be confused with the
    //     earlier builds that measured it.
    // o2 RAW, remapped so 0.5 is zero and 1.0 is +1: the gate needs
    // normalize(o2).z < 0.3 and ours measures 0.31-0.69, so the question is what
    // o2 actually is. Its LENGTH is also reported because a tangent-space vector
    // should be about 1 and this one measured >= 4.
    OutColor = vec4(r2.x * 0.5 + 0.5, r2.y * 0.5 + 0.5, r2.z * 0.5 + 0.5, 1.0);
}
