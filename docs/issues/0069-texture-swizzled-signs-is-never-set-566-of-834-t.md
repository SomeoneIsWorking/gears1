---
id: 69
title: texture_swizzled_signs is never set: 566 of 834 texture bindings ask for GAMMA and are read as linear
status: open
symptom: scene colour and contrast look wrong in a way no single pass explains; textures read without their sRGB-to-linear decode
tags: gpu,textures,colour,gamma,system-constants,open
created: 2026-08-05
updated: 2026-08-05
---

## The gap, measured

Every translated texture fetch branches on
`xe_uniform_system_constants.texture_swizzled_signs` to choose between the
unsigned and signed views of a texture and whether to apply a sign remap. The remap
for `TextureSign::kGamma` is the piecewise sRGB-to-linear decode.

**This renderer never sets that constant.** It is zero, so every fetch takes the
unsigned, undecoded path. Counted per frame by the new 'frame texture signs' line:

    act1_v2 (menu)     2 sign patterns among   70 bindings: [0x3f x48] [0x55 x10]
    act1 (gameplay)    2 sign patterns among  834 bindings: [0x3f x566] [0x55 x12]
    play_v2            2 sign patterns among 1064 bindings: [0x3f x688] [0x55 x14]
    boot150 (movie)    0 of 6 bindings -- correct there, and the line says so

0x3f is TextureSign::kGamma on R, G and B; 0x55 is kSigned on all four. So on a
gameplay frame **566 of 834 bindings are gamma textures being read as linear**.

## The fix that was written, and why it is NOT shipped

Xenia's own algorithm ports in a dozen lines -- `texture_util::SwizzleSigns` on
each fetch constant, packed 8 bits per slot into `texture_swizzled_signs[fc >> 2]`.
Register base 0x4800 and the 6-dword stride were checked against Xenia's
`RegisterFile::GetTextureFetch`. The signs must be POST-SWIZZLE, which is why the
port calls SwizzleSigns rather than reading the raw fields.

Applying it makes the image visibly WORSE:

    act1 gameplay   mean R 15.94 G 22.66 B 22.95  ->  R 5.40 G 8.58 B 8.69
    act1_v2 menu    mean R 17.72 G 20.89 B 36.36  ->  R 11.31 G 14.70 B 37.26

and the menu goes blotchy with crushed blacks and oversaturated blue patches.

**It is not simply a missing output encode.** If the only missing piece were the
matching linear-to-gamma encode on the way out, re-encoding the decoded frame would
land back on the current image. It does not: re-encoding at 1/2.2 moves it FURTHER
away (mean difference 44.0, against 10.5 for the decoded frame itself). Something
else in the chain is missing, and it is not a global tone curve.

Consistent with that: the gamma-WRITE flag (kSysFlag_ConvertColor0ToGamma) is set
only for k_8_8_8_8_GAMMA render targets, matching Xenia exactly, and this title's
surface 0x2d0 never uses that format -- it uses plain k_8_8_8_8. The title does its
own encode inside the composite pass instead (ps 0x501ac5d8692bf7b6 raises the
colour to c0.x, and some draws pass c0.x = 0.5), so the encode is per-pass and
conditional rather than a property of the render target.

## Where to pick this up

The two halves have to land together. What is known:

- the decode side is a dozen lines and is already written and reverted in this
  commit's parent -- see the comment block in DeriveSystemConstants;
- the encode side is NOT the system-constant flag, because the render target format
  is not gamma; it is inside the title's own composite, which is already
  implemented natively (runtime/shaders/scene_gamma.frag) and bit-exact;
- so the question to answer first is what the composite's c0.x is across a whole
  frame, and whether every path to the front buffer passes through a pass that
  encodes. GEARS_DRAW_PS_CONSTS=501ac5d8692bf7b6 prints it per draw.

Do NOT ship the decode alone. Half a matched pair is worse than neither, which is
what the numbers above show.
