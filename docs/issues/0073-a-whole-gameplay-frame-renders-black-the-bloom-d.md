---
id: 73
title: A whole gameplay frame renders black: the guest hands the post-process blend a zero colour scale and a NaN offset
status: open
symptom: a captured gameplay frame renders completely black (0 of 921600 px non-black) although every draw issues and the scene colour target is full of content; other captures of the same scene render fine
tags: gpu,draw,post,black,frame,constants,guest,native-renderer
created: 2026-08-05
updated: 2026-08-07
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

### Note (2026-08-06)
## c7 and c8 NAMED against our own verified reimplementation, and the NaN carries x86's SIGN

Two things this entry had as guesses are now read off code rather than inferred,
and one new discriminator was run against both classes.

### What c7 and c8 ARE

The entry guessed "c7 looks like `GammaColorScaleAndInverse` ... and c8 an
additive colour term". Half right, and the other half matters.
`runtime/shaders/uber_post_blend.frag` is this pass reimplemented and verified
bit-exact against the translated shader (`verify_native_pass.sh`, mean
|difference| 0.0000 over 2,764,800 samples), so it is authority on the mapping:

    c7.xyz = kOutputScale    the FINAL output colour scale
    c7.w   = kOutputGamma    the output gamma exponent
    c8.xyz = kOffset         an offset INSIDE the colour transform

and the tail is

    weighted    = graded * c3.w + kOffset          <- c8 enters HERE
    desaturated = weighted + lum
    scaled      = clamp(desaturated * kOutputScale, 0, 1)   <- c7 multiplies AFTER
    rgb         = PowSat(scaled, kOutputGamma)

**c8 is added BEFORE c7 multiplies.** So "c8 is an overlay colour" is wrong for
this shader, and the entry should not be read that way.

Proved rather than argued, with `GEARS_DRAW_PS_CONST_SET` on play_v2:

    as captured                          0/921600 non-black
    c8 := (0,0,0,0)   neutral offset     0/921600
    c8 := (0.5,0,0,0) a RED overlay      0/921600      <- an overlay would show
    c8 := (0,0.5,0,0) a GREEN overlay    0/921600      <- so would this
    c7 := (1,1,1,0.5) and c8 := 0    911386/921600 (98.9%)

A red or green c8 changes nothing because c7 = 0 multiplies it away. c7.xyz is
the load-bearing constant and nothing downstream can rescue it.

`c7.w = 0.5` is 1/gamma with gamma = 2.0, which is exactly what
`VdGetCurrentDisplayGamma` reports. That is a consistency check on the mapping,
not a coincidence.

### c7 is BINARY across the whole capture set

    (1, 1, 1, 0.5)   act1 act1_now act1_v2 black bright courtyard ingame_v3
                     menu prison_turn swap_v3 walk_gameplay walk_v3 wallcorner
    (0, 0, 0, 0.5)   play_v2   character_auto
    (no such draw)   boot150

Thirteen captures at exactly 1, two at exactly 0, never an intermediate value.
This is `GammaColorScale` -- UE3's `View.ColorScale`, the camera FADE -- either
fully off or fully on, not a fade caught midway.

**That is worth stating plainly and it is NOT yet settled: if these two captures
are of a completed fade, a black frame is CORRECT and they are unusable
captures rather than a rendering defect.** What keeps this entry open is the
NaN, which is a defect on any reading.

### NEW: the fatal NaN has x86's sign, and it exists ONLY in the broken frames

PowerPC's default QNaN for an invalid operation is `7fc00000`; x86 SSE's "real
indefinite" is `ffc00000`. The fatal constant is `ffc00000`. Scanning each
capture's guest memory for aligned dwords of both, which is the discriminator
run against BOTH classes:

    capture          ffc00000 (x86)   7fc00000 (PowerPC)
    play_v2                23                 2          BLACK
    character_auto          8                 2          BLACK
    courtyard               2                 2          renders
    walk_gameplay           2                 2          renders
    bright                  2                 2          renders

Every capture carries a baseline of 2 of each. The broken ones carry 21 and 6
EXTRA x86-signed NaNs and no extra PowerPC-signed ones at all.

And the extra ones are not scattered. Grouped into consecutive runs, play_v2's
23 are:

    7 runs of exactly 3 consecutive dwords   <- seven copies of one all-NaN xyz
    2 isolated dwords                        <- the baseline every capture has

So **21 of the 23 are seven copies of a single all-NaN three-component vector**,
matching the seven copies of the constant block this entry already located at a
regular ~0x71e00 stride. One guest computation produces an entire NaN colour
vector, and it is then copied round-robin.

### What that narrows the hunt to

  * ONE computation, producing all three channels NaN together -- not three
    independent scalar faults, so a single bad scalar or vector operand;
  * it went through HOST floating point (the x86 sign), so it is arithmetic our
    recompiled code executed, not a constant the title stores. That is
    consistent with, and sharper than, this entry's existing finding that the
    bytes were already in guest memory -- they were, and now we know they were
    PUT there by host arithmetic rather than loaded from the title's data;
  * CAUTION, stated because the sign is seductive: an invalid operation gives a
    NaN on BOTH architectures, so the sign proves where this NaN was made, NOT
    that the console avoids one. It does not on its own establish that the
    console renders this moment non-black.

The next step is unchanged in shape but much better aimed: find the single
computation that writes an all-NaN vec3 into that round-robin buffer. It is one
site, not a class of them.

### Note (2026-08-06)
## The renderer now SAYS this, unprompted -- a NaN constant is a perfect predictor of a black frame

This entry cost two sessions largely because nothing in the renderer ever looked
at the VALUES it packs into a constant buffer. The frame reported "0 px
non-black", every draw-level probe reported healthy draws, and the one number
that mattered was only visible to someone who had already guessed the shader
hash and set `GEARS_DRAW_PS_CONSTS` to it.

`UniformCache::CensusConstants` now scans every packed float block on the way
past -- pixel and vertex, on cache rebuilds only, so it is cheap enough to be
unconditional -- and the frame report WARNS when one holds a NaN, naming the
shader, the constant index and the raw bits.

On play_v2, with nothing specified:

    [draw:warn] CONSTANT CENSUS: 4 packed constant vec4(s) contain a NaN, over
      849 repacked block set(s). ...
      vertex shader 0x8354e5cc00c0a98c  c[71]  = [be1aadb6 bc000108 41b67a23 7f8021c6]
      vertex shader 0x8354e5cc00c0a98c  c[161] = [ffff39a6 2904ffa8 fffd41a7 18e35e00]
      vertex shader 0x8354e5cc00c0a98c  c[214] = [2925f8ec 7ffe209f fe0739a6 0c6901ff]
      pixel  shader 0x9610bf8038af9aaf  c[8]   = [ffc00000 ffc00000 ffc00000 00000000]
      Raw bits matter: ffc00000 is x86's default QNaN (host arithmetic made it),
      7fc00000 is PowerPC's, ffffffff is uninitialised memory.

It finds this entry's constant with no prior knowledge of it.

### Run against BOTH classes, over the whole capture set

    capture          NaN vec4s   frame
    act1                 0       91.5% non-black
    act1_now             0       97.8%
    act1_v2              0       97.8%
    black                0       80.6%
    boot150              0       99.6%
    bright               0       83.9%
    character_auto       1        0.0%   <- BLACK
    courtyard            0       99.3%
    ingame_v3            0       97.7%
    menu                 0       99.0%
    play_v2              4        0.0%   <- BLACK
    prison_turn          0       99.9%
    swap_v3              0       96.2%
    walk_gameplay        0       98.8%
    walk_v3              0       99.7%
    wallcorner           0      100.0%

**A NaN constant is present in exactly the two captures that render black and
in none of the fourteen that render.** That is the discriminator this entry
never had, and it is now free on every run.

The negative is designed too: a frame with no NaN says so on the `draw` debug
channel WITH ITS DENOMINATOR -- "no NaN in any float constant, over 726
repacked block set(s) (2 carried an Inf, which is a normal value of this
title's c1)" -- so "clean" cannot be confused with "never scanned".

### A NEW observation, recorded but NOT interpreted

play_v2 also carries three NaN VERTEX constants, all in the skinned character's
`vs 0x8354e5cc00c0a98c` (c[71], c[161], c[214]). They do not have the tidy
`ffc00000` shape -- `ffff39a6`, `7ffe209f`, `7f8021c6` -- which is what packed
non-float data or uninitialised memory read as float looks like, not the result
of one invalid operation. They may be a bone-palette region the shader never
indexes, in which case they are harmless and normal. **This is an observation,
not a finding**: no capture that renders was checked for the same pattern in a
region its shaders do not read, so nothing here says these are abnormal.

### Note (2026-08-07)
## 2026-08-07: this is the LIVE first gameplay render, not one bad capture

Reproduced without a capture file, on the frame selected BY CONTENT (the frame
after the first with >= 400 draws -- see #89 and GEARS_DRAW_FRAME_MIN_DRAWS),
i.e. the first gameplay render of a fresh boot + menu walk. Three independent
runs, three different guest frame indices (2926, 2935, 2942), same result:

    frame 2942, 757 draws:  0 of 921600 px non-black, 921600 px written
    pixel trace (640,360) on surface 0x2d0, one sample after EVERY draw:
      after 502 draws = (0.0273, 0.0273, 0.0332)   <- draw 682 ps 0x63c971f5e9d59913
      after 558 draws = (0, 0, 0)                  <- draw 743 ps 0x9610bf8038af9aaf

So the same shader this page already names -- UE3's uber post-process blend --
takes a lit frame to black, live, on the frame the game starts on. The constant
census on that same frame reports exactly one NaN vec4 in the whole frame, in
that shader, at the same index:

    pixel shader 0x9610bf8038af9aaf  c[8] = [ffc00000 ffc00000 ffc00000 00000000]

The per-resolve dumps of that frame show the scene itself is fine and only the
post chain is not:

    srcC400 -> 0xbdf0000  (HDR scene colour)   mean 0.0378, 98.7% non-zero
    srcC2D0 -> 0xc7f9000  at draw 531          mean 0.0308, 77.7% non-zero
    srcC2D0 -> 0xc7f9000  at draw 665          ZERO
    srcC2D0 -> 0x311000   at draw 756 (front)  ZERO

This does NOT yet say whether the console renders the same moment black -- the
paired oracle capture is what settles that, and it is not in hand yet. If the
console is also black here, this frame is a legitimate fade-in and the black
gameplay the user reports is a different (or longer-lived) fault.

### Note (2026-08-07)
### Correction, same day: the CONSOLE renders that frame black too

The paired capture landed. At the first gameplay render the console's own last
two copies are entirely zero -- read back off its GPU immediately after each
copy, before anything else can touch them:

    copy11  srcC2D0 1280x720 f6 -> 1312F000   mean 0.2028, 23.6% non-zero
    copy17  srcC2D0 1280x720 f6 -> 1312F000   0.0000,  0.0% non-zero
    copy18  srcC2D0 1280x720 f6 -> 1F606000   0.0000,  0.0% non-zero   (front buffer)

So the level opens on a fade FROM black and the first gameplay frame is black on
both emulators. A comparison taken there compares two black frames, and the
"we are black and they are not" reading that the previous note was heading
towards would have been wrong.

What survives from the previous note: the frame IS black on our side, the scene
colour surface upstream of the post chain does carry content, and the NaN
constant in 0x9610bf8038af9aaf is real and present. What it does NOT establish
is that any of that is a defect at THIS frame -- black is the correct answer
here. Whether the NaN persists past the fade is the open question, and the
selector now takes GEARS_DRAW_FRAME_AFTER_GAMEPLAY / GEARS_ORACLE_DUMP_AFTER_
GAMEPLAY so the pair can be taken 300 frames later instead.
