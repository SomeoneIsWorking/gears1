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

### Note (2026-08-06)
## The bright pass's constants, and an infinity that is the GUEST's (2026-08-06)

`GEARS_DRAW_PS_CONSTS=a146058ecfeb9122` on act1, once the knob was made to work
at all (see below):

    c[0]=(-9990.128, 0.0009999871, 0.1001001, 0.00010009881)
    c[1]=(1200, inf, 4, 0)          [44960000 7f800000 40800000 00000000]
    c[2]=(0, 0.4, 0, 0)
    c[3]=(0.5, 0.5, 0.5, 0.5)
    c[4]=(1, 0.0625, 0, 0)

**c[1].y is +infinity**, raw bits 7f800000. That is the shape of catalog #73 --
a post pass handed a value that makes its output degenerate -- so it was chased
the same way #73 was, and it lands the same way.

### The infinity is in guest memory, so the renderer did not make it

Searching the capture for the exact 16-byte block, big-endian:

    (1200, inf, 4, 0)   30 hits, at real guest addresses --
                        0x22ed0, 0x482b0, 0x4b0b0, 0x686c0, 0x8ae80,
                        0x8b9b0, 0xa8e30, 0xcf2a0, +22 more
    bare +inf dword     673 occurrences

Thirty copies of a well-formed constant block at guest addresses is the title
storing it deliberately, not a value we manufactured on the way in. Exactly the
exoneration #73 got, by the same instrument.

### And the arithmetic that would make it bite is already emulated

The Xenos rule that matters here is that a multiply by zero gives zero even when
the other operand is NaN or infinite, where IEEE and plain SPIR-V give NaN.
Xenia's SPIR-V translator implements it (`spirv_shader_translator_alu.cc`:
"Check if the different components in any of the operands are zero, even if the
other is NaN ... Replace with +0"), and we use that translator, so our shaders
inherit it.

So "the inf poisons the output through a multiply" is NOT available as an
explanation. NOT ruled out: that the shader reaches the infinity through some
other operation, or that it is meant to be consumed by a comparison whose Xenos
semantics differ. The constants are a lead that has been narrowed, not closed.

## Instrument fixed to get here: GEARS_DRAW_PS_CONSTS printed NOTHING

The printing lives inside the per-draw listing, which only runs under
`GEARS_DRAW_FRAME_LIST=1`. So asking for PS_CONSTS on its own produced no
output and no explanation -- the second knob in this session to answer a
question with silence.

It now pulls the listing in for the draws it names (and only those, rather than
a line per draw in the frame), and a hash that matched NO draw says so with the
frame's draw count, because "you asked about a shader this frame never ran" and
"the constants are all zero" both print nothing otherwise.
