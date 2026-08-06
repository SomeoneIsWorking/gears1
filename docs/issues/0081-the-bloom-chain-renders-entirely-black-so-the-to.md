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

### Note (2026-08-06)
## WHY it is black, end to end, from the shader's own disassembly (2026-08-06)

The bright pass is `ps a146058ecfeb9122`, 459 dwords. Its shape, from
`xenos_translate --raw`:

    tfetch2D r2/r11/r9/r6/r8/r3/r4/r1.xyz_, r0.xy, tf0, Offset{X,Y}=...
    tfetch2D r0/r5/r12.x, r0.xy, tf1, Offset...
    sgt  r14.xyz_, r1.xyzz, c255.xxxx
    sgt  r10.x_zw, r4.xyyz, c255.xxxx
    sgt  r17/r18/r13/r7/r5/r19 ..., c255.xxxx
    max4 ...

It samples a 4x2 neighbourhood of tf0 (colour) and tf1 (depth), and every one
of those samples is put through `sgt <sample>, c255.x` -- set-greater-than
against a single scalar. That is the bloom THRESHOLD, and everything after it is
a max4 reduction of the results.

**c255.x = 1.0** (0x3f800000, measured on this draw).

**Its input tops out at 0.125.** fc0 binds resolve target 0xbde0000 with
`exp_adjust +0` -- so no sampling-side exponent compensation is asked for -- and
that target's measured range is 0.000000 .. 0.125000.

So every `sgt` compares something <= 0.125 against 1.0, every one yields zero,
and the pass writes black over its whole 322x182 target. Nothing is wrong with
the draw, the bindings, the raster state or the constants: the shader is doing
exactly what it was written to do with the numbers it was given.

## So this entry is a SYMPTOM, and the cause is upstream

Bloom being black is not a bloom defect. It is the first place where the scene
being too dark becomes VISIBLE as a hard zero rather than as a dim image, which
is why it showed up as "0 of 192192 components non-zero".

The question it hands upstream is sharp and quantitative: **the threshold is
1.0, and the input reaches it only if the surface behind it exceeds 8.0** (the
last resolve into 0xbde0000 is draw 670, from surface 0x2d0, with
copy_dest_exp_bias -3, i.e. x0.125). Our surface 0x2d0 maxes at exactly 1.0.
For this title's bloom to do anything on hardware, that surface must carry
values of at least 8, and ours carries 1.

Not yet known: whether 0x2d0 is capped at 1.0 by something we do, or is simply
receiving a scene that never gets bright. 46 of the draws feeding it are
colour_fmt 12 (k_2_10_10_10_FLOAT_AS_16_16_16_16), whose guest clamp is
alpha-only, so those draws are NOT clamped by us and could write above 1.0.

## Missing instrument

Every range measured in this investigation has come from a resolve DESTINATION,
because that is the only thing the renderer reports a range for. The question
above is about a SURFACE, and there is no probe that reports one's min/max. That
is the next thing to build, and it is why this stops here rather than guessing.

## New knob: GEARS_DRAW_TEX_BINDS=<ps hash>

What a named pixel shader actually samples, one line per binding: fetch
constant, base address, dimension, `exp_adjust` (with the multiplier it means),
and which of the three sources served it -- this frame's resolve target, a guest
texture, or a stub. The frame report only ever counted bindings by kind across
the whole frame, and three separate investigations have had to infer a single
pass's inputs from those aggregates.

### Note (2026-08-06)
Third instrument agrees the bloom surface is empty: GEARS_DRAW_SURFACE_RANGE reports surface 0x5a0 at 0.0000..0.0000 on every channel. And the upstream question this entry handed over now has a measured answer -- surface 0x400, the HDR scene, peaks at 2.19 with 0.19% of pixels above 1.0, where bloom's threshold implies it must reach 8.0. So the scene is real HDR but about 3.7x too dim, which matches the ~3.4x shortfall #62 measures at the other end of the pipeline.

### Note (2026-08-06)
## CORRECTION: bloom is NOT universally black -- it is act1 that has none

Ran the surface probe over every capture instead of just the one this entry was
opened on:

    capture      surface 0x5a0 (bloom) max R/G/B
    act1         0.0000 / 0.0000 / 0.0000        <- this entry
    act1_now     2.5488 / 0.6196 / 0.1220
    act1_v2      1.5840 / 0.4075 / 0.0751
    black        0.0268 / 0.0275 / 0.0258
    courtyard    0.0500 / 0.0500 / 0.0500

Bloom produces output in three of five captures, and above 1.0 in two of them.
So the chain works, and "the bloom chain renders entirely black" is wrong as a
general claim -- it is true of act1 only.

Everything measured about act1 stands: the bright pass thresholds with
`sgt <sample>, c255.x` at c255.x = 1.0, its input tops out at 0.125, so it
writes zero. What changes is the reading: on act1 that is bloom CORRECTLY
finding nothing above the threshold in a dark moment, not a defect.

This entry was opened on a single capture and generalised from it. The
discriminator existed the whole time -- run the same probe on a capture that
should bloom -- and running it is what settles it.

Status: not a defect on the evidence available. Left open only because it is
unknown whether act1's moment SHOULD have bloom; the oracle could answer that
and has not been asked.
