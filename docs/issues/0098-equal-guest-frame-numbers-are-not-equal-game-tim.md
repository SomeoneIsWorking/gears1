---
id: 98
title: Equal guest frame numbers are not equal game time, so paired captures compared two different scenes
status: resolved
symptom: layer_compare reported mean |d| 0.695 on the shadow mask; the console had rendered ONE shadow-casting light where the port rendered two
tags: oracle,layer-compare,methodology,instrument
created: 2026-08-11
updated: 2026-08-11
---

## What was measured

Two paired captures of the same level, taken hours apart, dumped the console's
frame 875 and its frame 873. Their pass structure DIFFERS:

    run A (f875)  copy6 srcD5A0 864x864 f22   copy9 srcD5A0 864x864 f22
    run B (f873)  copy6 srcD5A0 864x864 f22   (none)

That is one shadow-casting light against two, plus a third HDR resolve and two
1280x208 passes in run B that run A does not have. Against run B our renderer
was reported at mean |d| 0.695 on the first shadow-mask copy and 0.968 on the
second -- where the same build against run A measured 0.110 and 0.091.

## Why

Both emulators advance the guest by WALL-CLOCK delta time. The title is not
frame-locked, so guest frame N is a different moment in the animation in every
run and on every emulator. The capture script selected each side's frame by
INDEX -- "300 frames after the first frame with >= 400 draws" -- and then
checked only that the two indices were within a few frames of each other. That
check passed (gap 0) while the two sides were looking at different scenes.

A comparison that cannot tell a scene difference from a renderer difference
reports every scene difference as a renderer defect. It nearly did: the run
above was on its way to being read as a regression from the EDRAM sample model.

## The fix

* The oracle dumps a WINDOW of consecutive frames (`GEARS_ORACLE_DUMP_FRAMES`,
  wired as `GEARS_LAYER_ORACLE_FRAMES`, default 5) instead of one.
* `layer_compare.py` picks the console frame whose PASS STRUCTURE is closest to
  ours, prints what every candidate scored, and prints a loud NOTE when none of
  them has our structure -- which is what run B would now print.
* Chosen on structure ALONE. Picking the best-AGREEING frame would make the
  instrument confirm itself forever, so the self-test offers it a one-pass frame
  that matches BYTE FOR BYTE against a four-pass frame holding a real
  difference, and requires it to take the four-pass one and keep the difference.
  The choice line was then run against the other class -- a third frame that
  really is the best structural match -- and it moves.

## What this invalidates

Any earlier paired number whose run did not have matching pass structure. The
reference run (f875) did have it, so the EDRAM sample model's results stand;
the `GEARS_DRAW_NOCONVERT=3-2` control arm for catalog #95 does NOT -- it was
measured on run B and has to be taken again.

### Note (2026-08-11)
THIS IS THE CROSS-EMULATOR FACE OF CATALOG #84, which measured the same thing
about OUR OWN renderer: two runs of one build on one input script reach 98.9%
identical pixels at guest frame 300, 25.9% at 600 and 17.7% at 1200, because
the guest clock is real time and the simulation is a function of it.

So the frame window and the structure-based frame choice here are a workaround
for a defect #84 already names, and #84 already carries the attempted fix --
`GEARS_GUEST_CLOCK_STEP_NS`, a fixed step per presented frame -- along with why
it is off: stepping at VdSwap deadlocks the title at boot, and the freerun
control arm freezes the picture while passing the determinism check perfectly
(instrument I027).

A fixed guest clock would fix OUR side's run-to-run variance outright, which is
half of what makes this issue's numbers noisy (the shadow-mask pass varies
105k..157k shadowed pixels between runs of one build). It would NOT align us
with the oracle, which has no such knob -- that still needs the window.
