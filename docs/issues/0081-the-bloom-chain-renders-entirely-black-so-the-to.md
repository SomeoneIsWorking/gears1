---
id: 81
title: The bloom chain renders entirely black, so the tonemap composites with no highlights
status: open
symptom: surface 0x5a0 has 0 non-black pixels after every one of its draws, and its resolve target is 0 of 192192 components non-zero
tags: gpu,draw,bloom,post,tonemap,colour,act1
created: 2026-08-06
updated: 2026-08-06
---

Found while chasing #62's narrow output range, on `scratch/frames/act1.gfr`
(the capture that reproduces it offline in 550 ms).

## Measured, two independent ways

The resolve dump: bloom's destination is empty.

    resolve target 0x6e0000 (352x182)  range 0.000000 .. 0.000000
                                       0 of 192192 components non-zero [0.0%]

And the checkpoint probe, per draw, on the surface that feeds it:

    checkpoint after 492 draws on surface 0x2d0: 825578 px non-black
    checkpoint after 493 draws on surface 0x5a0:      0 px non-black
    checkpoint after 494 draws on surface 0x5a0:      0 px non-black
    checkpoint after 495 draws on surface 0x5a0:      0 px non-black
    checkpoint after 496 draws on surface 0x5a0:      0 px non-black
    checkpoint after 497 draws on surface 0x5a0:      0 px non-black
    checkpoint after 498 draws on surface 0x2d0: 854590 px non-black

A pixel trace on 0x5a0 agrees: (100,50) is (0,0,0,0) for all 5 samples and
never changes.

## The draws are NOT being dropped -- they run and write black

From the diag table, guest draws 699/701/703 (issued #495/496/497):

    prim triangle_list, 6 verts, 2 prims after clip, **57600 fragment
    invocations**, verdict `shaded`, colour mask 15, blend off,
    viewport and scissor 322x182, surface 0x5a0 (k_16_16_16_16_FLOAT)

57600 of the 58604 pixels in a 322x182 rectangle. So the geometry, the raster
state and the colour mask are all fine, the pixel shader runs on essentially
every pixel of the target, and what it writes is zero.

That rules out the whole family of "the draw died at some stage" explanations
that catalog #30 and #31 covered -- this is a shader producing black, not a
draw that never happened.

## Why it matters for #62

Draw 707 (`ps 629226076307234e`) is the final full-screen pass into 0x2d0 before
the front-buffer resolve, and bloom is what puts the bright halo back into a
tonemapped image. With the bloom term identically zero, the top of the range has
nothing to come from -- which is exactly #62's symptom (our presented frame tops
out at 0.30-0.42 where the oracle reaches 1.0).

NOT YET ESTABLISHED: that fixing bloom fixes #62. Bloom being black is a defect
on its own terms and is stated as that.

## Next

`ps a146058ecfeb9122` is the bright pass: 161600 bytes of SPIR-V, 5 float
constants, 4 textures, 2 samplers, and the frame report shows it sampling the
DEPTH resolve destination 0xba40000. The question is what its four bindings
actually resolve to and which of them is black -- one of the frame's resolve
targets (0xcb81000) is legitimately all-zero because it resolves a
just-cleared surface, and if the bright pass reads that, it would produce
exactly this.

`GEARS_DRAW_PS_CONSTS=a146058ecfeb9122` printed nothing on this capture; that
needs looking at before it can be used to rule the constants in or out.
