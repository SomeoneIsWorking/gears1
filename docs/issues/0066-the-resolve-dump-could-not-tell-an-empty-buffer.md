---
id: 66
title: The resolve dump could not tell an empty buffer from a sub-unit one, and could not see negative values at all
status: resolved
symptom: a resolve target dumps as a pure black PPM and reports 'max colour component 0.000'; read as 'the renderer never wrote it'
tags: instruments,gpu,resolve,diagnostics
created: 2026-08-05
updated: 2026-08-05
---

## What it reported, and why that was a lie

GEARS_DRAW_RESOLVE_DUMP writes each resolve target to an 8-bit PPM and logged one
number: 'max colour component'. Both halves fail on a target that is not a colour
image:

- the PPM clamps to [0,1] and quantises to 8 bits, so any buffer whose values are
  fractions of 1/255 writes as PURE BLACK;
- the max was seeded at 0.0 and only ever grew, so NEGATIVE values could not move
  it. A buffer that is entirely negative reported 'max 0.000'.

So 'black PPM, max 0.000' covered three completely different states -- never
written, written with tiny values, written with negative values -- and only one of
them is a defect.

## How it misled

Resolve destination 0xcb81000/0xcb91000 dumps black with max 0.000 in every
capture, and it IS sampled by a later pass. That reads as a full-screen buffer the
renderer fails to produce, i.e. exactly the 'missing graphics' being hunted.

Disassembling the consumer settled it. ps 0x629226076307234e is MOTION BLUR: it
reads depth (tf1), a two-channel SIGNED VELOCITY buffer (tf2 = this target, which
is why the guest resolves it as k_16_16), derives a screen-space offset and loops
sampling the scene colour along it. Velocities are fractions of a pixel and
routinely negative -- precisely the values this dump cannot represent. A zero
velocity buffer is also the CORRECT content for a static camera, and it degrades
gracefully (the loop samples the same texel and the pass becomes a pass-through).

## The fix

The dump now reports the true range and the count:

    resolve target 0x6e4000 (352x182) ... (range -0.020905 .. 0.129761,
        145800 of 192192 components non-zero [75.9%])

and says so explicitly when a buffer is non-empty but entirely below one 8-bit
step, because that is the case where the PPM itself tells you nothing. With it,
0xcb91000 measures 0 of 2764800 non-zero -- genuinely empty, and correct for the
static frames captured.

## Note on captures

scratch/frames/act1.gfr is a v1 capture and is superseded. It reports its bloom
target as exactly zero where fresher captures of comparable scenes report 75.9%
non-zero. It still gives a valid A/B (both arms replay identical input), but stop
reading absolute values off it. tools/verify_native_pass.sh now defaults to the v2
captures.
