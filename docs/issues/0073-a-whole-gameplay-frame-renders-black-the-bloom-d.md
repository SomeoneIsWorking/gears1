---
id: 73
title: A whole gameplay frame renders black: the guest hands the post-process blend a zero colour scale and a NaN offset
status: open
symptom: a captured gameplay frame renders completely black (0 of 921600 px non-black) although every draw issues and the scene colour target is full of content; other captures of the same scene render fine
tags: gpu,draw,post,black,frame,constants,guest,native-renderer
created: 2026-08-05
updated: 2026-08-05
---

## Symptom

`scratch/frames/play_v2.gfr` renders **completely black** -- 0 of 921,600 px
non-black, while 921,600 px were written (so it is not an unwritten target). 656
of 868 draws issue, 0 skipped. `courtyard.gfr` and `bright.gfr`, the same game
area, render fine.

It has been black since before any of this session's changes; it is the capture
`tools/verify_native_pass.sh` has been calling INCONCLUSIVE as a negative control.

## Localised to one draw

Checkpoints (`GEARS_DRAW_FRAME_STEP`) put the transition at issued draw 630 =
**draw 840, pixel shader 0x9610bf8038af9aaf** -- UE3's uber post-process blend
(DOF/bloom composite + colour transform).

## The cause, measured

The pixel shader's own float constants, as the guest programmed them
(`GEARS_DRAW_FRAME_LIST=1 GEARS_DRAW_PS_CONSTS=9610bf8038af9aaf`). Every capture
agrees on c0..c6 and c9; **only c7 and c8 differ**:

| capture | c7 | c8 |
|---|---|---|
| courtyard | (1, 1, 1, 0.5) | (0, 0, 0, 0) |
| bright | (1, 1, 1, 0.5) | (0, 0, 0, 0) |
| act1_v2 | (1, 1, 1, 0.5) | (0, 0, 0, 0) |
| **play_v2** | **(0, 0, 0, 0.5)** | **(-nan, -nan, -nan, 0)**, bits `ffc00000` |

c7.xyz is the shader's output colour scale (instruction 32, `mul_sat r1.xyz,
r0, c7.xyz`) and c8.xyz is an additive term (instruction 29). A zero scale takes
every pixel to zero on its own; a NaN offset poisons every pixel on its own.

**Proved with a control arm**, not argued. `GEARS_DRAW_PS_CONST_SET` substitutes
a working capture's numbers into this one:

    GEARS_DRAW_PS_CONST_SET='9610bf8038af9aaf:7=1,1,1,0.5;9610bf8038af9aaf:8=0,0,0,0'
      -> 916139/921600 px non-black (99.4%)   [the frame appears]
    ...:7=1,1,1,0.5  alone   -> 0.0%          [the NaN still kills it]
    ...:8=0,0,0,0    alone   -> 0.0%          [the zero scale still kills it]

Both constants are independently fatal, and together they are the whole cause.

`ffc00000` is the negative quiet NaN an invalid operation produces (0/0). It is
not a value any post-process setting is authored with, so **at least c8 is a
guest-side defect** -- the renderer is faithfully drawing what the command
stream told it to. c7.xyz = 0 is what UE3 writes for a camera fade to black, so
on its own it could be legitimate; arriving alongside a NaN in the adjacent
constant, it is more likely that one guest-side computation produced both.

With both substituted, the frame is a dark interior with correct geometry --
very dark, but that is a separate question from black.

## RETRACTED: the earlier explanation on this page was WRONG

This entry previously said the frame was black because the bloom/DOF buffer at
`0x6e4000` resolved empty and the blend's `W = sharpWeight + tf2.a` therefore
divided by zero. Both halves are false, and the constants above refute them:

- **There is no divide by zero.** c2 = (0, 0.4, 0, 0): MaxNearBlur is 0. c1.x =
  1200 is the focus distance and the frame's depths run 62..350, so every pixel
  is IN FRONT of focus, takes the near maximum, and gets `blurAmount = 0` --
  hence `sharpWeight = 1` and `W = 1`. It was never near zero.
- **The empty bloom buffer is CORRECT for this frame.** The downsample
  (`a146058ecfeb9122`) weights each tap by the DOF unfocused percent plus a
  bloom term thresholded at c255.x = 1.0. With MaxNearBlur = 0 the DOF term is
  zero everywhere, and the scene colour maxes out at exactly 1.0, which `sgt`
  does not pass. Zero out is the right answer. Measured: `GEARS_DRAW_ONLY=627`
  (that draw alone) leaves `0x6e4000` at 0.0%, and the checkpoint on surface
  `0x5a0` right after it is 0 px non-black -- the pass shades zero, the resolve
  is innocent.
- **The 328x184-into-352x182 resolve size mismatch is not implicated.** It was
  the first thing the old entry told the next session to check. It is real and
  harmless here.

The measurement that produced the wrong story was a per-input probe of the blend
pass. It was accurate -- tf2 really is 0.0% -- and the inference from it was not.
A probe of the INPUTS could never see that the fault was in the CONSTANTS.

## Next

Find what the guest computes into these two constants. They are written into the
pixel float constant file before draw 840, so the values are already in the
command stream: this is CPU-side. In UE3 terms c7 looks like
`GammaColorScaleAndInverse` (ColorScale.rgb, 1/Gamma) and c8 an additive colour
term, both fed from the view's post-process settings. A 0/0 reaching a
render-thread constant points at a recompiled-code defect or an uninitialised
post-process setting, not at the renderer.

## Instruments added or fixed on the way

- `GEARS_DRAW_PS_CONST_SET=<pshash>:<i>=<x>,<y>,<z>,<w>` (new, control arm) --
  substitute one packed float constant. This is what turned "the constants
  differ" into "the constants are the cause".
- `GEARS_DRAW_PS_CONSTS` now prints **raw bits** next to each float. `-nan` as a
  word says nothing; `ffc00000` (invalid operation) versus `ffffffff`
  (uninitialised memory) point at completely different bugs.
- `GEARS_DRAW_FRAME_STEP` **silently dropped every checkpoint that followed a
  resolve** -- it read `openTarget`, which `endPass()` nulls -- and those are
  exactly the checkpoints that say whether a pass's output survived to its
  resolve. It now falls back to the last opened surface, and when it still
  cannot take one it says so instead of printing nothing.
