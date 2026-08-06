---
id: 73
title: A whole gameplay frame renders black: the guest hands the post-process blend a zero colour scale and a NaN offset
status: open
symptom: a captured gameplay frame renders completely black (0 of 921600 px non-black) although every draw issues and the scene colour target is full of content; other captures of the same scene render fine
tags: gpu,draw,post,black,frame,constants,guest,native-renderer
created: 2026-08-05
updated: 2026-08-06
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

### Note (2026-08-05)
## The fatal constants were in GUEST MEMORY, so the renderer is out of it

#73's "Next" asked where the guest computes c7 and c8. A capture stores guest
memory (not the ring), so part of that is answerable offline, and
`tools/find_const_block.py` answers it: search the capture's bytes for the
constants as big-endian dwords.

Run against BOTH classes, which is the only reason the result means anything:

    play_v2.gfr    (black)    c7=(0,0,0,0.5) || c8=(NaN,NaN,NaN,0)   FOUND 7x
                              the working pattern                     found 4x
    courtyard.gfr  (renders)  the play_v2 pattern                     ABSENT
                              the working pattern                     found 13x
    act1_v2.gfr    (renders)  the play_v2 pattern                     ABSENT
                              the working pattern                     found 16x

So the words `ffc00000 ffc00000 ffc00000` were sitting in the guest's own
memory in the frame that renders black, and are nowhere in either frame that
renders. **The renderer's constant packing did not manufacture them** -- it
read what was there. That closes the half of #73 that the renderer could be
blamed for, and it cost no live run.

The seven copies are at guest 0x1d1b0, 0x73aa0, 0xe5830, 0x1576a0, 0x1c9530,
0x23b4a0, 0x2ad430 -- a regular ~0x71e00 stride through low physical memory,
which is the shape of a per-frame buffer reused round-robin. The working
captures' copies sit in the same low region at their own regular stride.

## What is NOT established, so nobody re-derives it as fact

Whether that region is the PM4 ring (values inline in a SET_CONSTANT payload)
or a CPU-side constant array that LOAD_ALU_CONSTANT pulls from. I scanned
backwards from a hit for type-3 packet headers and it produced 37 overlapping
"packets" in 0x800 bytes -- that is what scanning for a bit pattern at
arbitrary alignment produces, not a parse. Discriminating needs a real walk
from the ring base, which the capture does not carry.

## Next

The seven guest addresses are the actionable output: watch one on a live run
and catch the store. `GEARS_WATCH_FREE` is the existing shape for "report the
moment this address is touched, with the caller". Two cautions: the addresses
come from one capture and a fresh run may place the buffer elsewhere, so
re-derive them from that run's own capture rather than trusting these; and
#44 means roughly one live run in three does not reach gameplay.

### Note (2026-08-06)
## A second repro, and one killed hypothesis (2026-08-06)

`scratch/frames/character_auto.gfr` -- the capture the new self-selecting gate
produced (see catalog #77) -- reproduces this **identically**, which doubles the
evidence and gives the entry a repro that contains a character.

    draw 721 (issued 704), ps 0x9610bf8038af9aaf, 921600 fragments
    c7 = (0, 0, 0, 0.5)
    c8 = (-nan, -nan, -nan, 0)   bits ffc00000

`tools/frame_hashes.sh` now tags an all-black render explicitly, and reports
**2 of 16 captures completely black: play_v2.gfr and character_auto.gfr**. They
share a hash (847b7f79e03d5c66) because that is the hash of 921600 black pixels;
the script used to report that as an ordinary match between two captures.

Localisation is unchanged and re-confirmed from the surface trace:
`GEARS_DRAW_TRACE_ALL` on character_auto shows surface 0x2d0 carrying a picture
through issued draw 536 (diag 712, means R 0.070 G 0.073 B 0.085) and reading
zero from the fullscreen draw 721 onward.

### KILLED: "the +inf in c1.y is what produces the NaN in c8"

The same constant block carries `c1 = (1200, inf, 4, 0)`, bits `7f800000`, and
an infinity two constants away from a NaN is an obvious suspect -- inf-inf and
0*inf both produce exactly the `ffc00000` quiet NaN seen in c8.

**It is not the cause.** Run against both classes, the infinity is present in
the captures that render CORRECTLY:

    courtyard        c1=(1200, inf, 4, 0)   c7=(1,1,1,0.5)  c8=(0,0,0,0)   renders
    bright           c1=(1200, inf, 4, 0)   c7=(1,1,1,0.5)  c8=(0,0,0,0)   renders
    black            c1=(1200, inf, 4, 0)   c7=(1,1,1,0.5)  c8=(0,0,0,0)   renders
    act1_v2          c1=(0, 5e-04, 0.6, 0)  c7=(1,1,1,0.5)  c8=(0,0,0,0)   renders
    play_v2          c1=(1200, inf, 4, 0)   c7=(0,0,0,0.5)  c8=(-nan,...)  BLACK
    character_auto   c1=(1200, inf, 4, 0)   c7=(0,0,0,0.5)  c8=(-nan,...)  BLACK

Three of the four working captures carry the same infinity, so it is a normal
value of this constant block and not a signal. Recorded so the next session does
not spend the hour I nearly did chasing it.

The entry's conclusion stands unchanged: c7/c8 are written by the GUEST, and the
cause is on the CPU side.
