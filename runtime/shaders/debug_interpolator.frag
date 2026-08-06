#version 450
// A DIAGNOSTIC substitution, not a native pass. It renders no UE3 pass and must
// never appear in the native-pass roster: it exists to answer one question that
// nothing else in this project can see.
//
// THE QUESTION (catalog #77). The character renders pure black. Its base pass,
// ps 0xf662d670789bfac0, multiplies everything it emits by
//
//     saturate(1 - normalize(r2.xyz).z)
//
// built in the microcode from `dp3 r5.w, r2.zxy, r2.zxy`, `rsq`, `mul ..., r2.z`
// and `subsc_sat r4.x, c254.y, r4.x` with c254.y = 1.0. r2 is interpolator o2,
// computed by 440 vertex-shader instructions with control flow and an
// address-register bone-palette lookup -- beyond what `tools/ucode_reduce.py`
// models, and invisible to every probe the renderer has. Seven candidate causes
// for the black character have been eliminated by measurement (see #77); all of
// them were guesses at this value.
//
// Interpolator location N is register rN, exactly as the translated shader
// receives them (see runtime/shaders/base_pass_lightmap.frag, which reads r0..r4
// the same way), so this reads the SAME data the title's shader reads.
//
// WHAT EACH CHANNEL SHOWS, chosen so the failure is legible rather than merely
// visible:
//   R = the gate itself, saturate(1 - normalize(r2).z). BLACK here IS the bug.
//   G = normalize(r2).z mapped to [0,1] (0.5 = zero), so the sign is readable.
//   B = length(r2) / 4. A tangent-space vector should be near 1 -> ~0.25.
//       0 means the interpolator is dead; a large value means it is unnormalised.
//
// So: a red image means the gate is open and the character should be lit; a
// black image with mid-grey green means normalize(r2).z is pinned at +1; a black
// image with black blue means o2 arrives as the zero vector.

layout(location = 2) in vec4 InR2;
layout(location = 0) out vec4 OutColor;

void main() {
    vec3 r2 = InR2.xyz;
    float len = length(r2);
    // Guard the divide rather than letting it produce NaN: a NaN would paint
    // whatever the blend does with it and read as an answer. Zero length is a
    // real possible finding, so it gets a defined output (green 0.5, blue 0)
    // rather than being hidden.
    float nz = len > 0.0 ? (r2.z / len) : 0.0;
    float gate = clamp(1.0 - nz, 0.0, 1.0);
    OutColor = vec4(gate, nz * 0.5 + 0.5, min(len * 0.25, 1.0), 1.0);
}
