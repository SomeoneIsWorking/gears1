---
id: 98
title: Equal guest frame numbers are not equal game time, so paired captures compared two different scenes
status: resolved
symptom: layer_compare reported mean |d| 0.695 on the shadow mask; the console had rendered ONE shadow-casting light where the port rendered two
tags: oracle,layer-compare,methodology,instrument
created: 2026-08-11
updated: 2026-08-12
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

### Note (2026-08-12)
THE OBVIOUS ROOT-CAUSE FIX -- give the ORACLE the same fixed guest timestep our side has, so both advance deterministically and land on identical frame content -- IS A DEAD END, and it is already measured. Do not build it. Xenia funnels every guest clock through Clock::UpdateGuestClock() and even exposes GetGuestTickCountPointer(), so a fixed-step mode there is small and tractable; the problem is not the plumbing. It is that all three ways of driving a fixed step were tried on OUR side and every one fails on this title (runtime/guest_clock.h documents each with its measurement): 'present' DEADLOCKS -- the title spins in guest code waiting for time before its first present, so time cannot advance until a frame is presented and a frame cannot be presented until time advances, 0 frames in 100 s against 9 on the real clock; 'vblank' cannot deadlock but is paced by a HOST SLEEP, so a run under it is not reproducible and no comparison may be quoted from it; 'vblank-freerun' FREEZES THE PICTURE -- the title keeps presenting to frame 10,800 but every frame from about 2,700 is bit-identical to the last, where the real-clock control's consecutive frames are 21-34% identical. That last one sets the trap worth remembering: a frozen picture is trivially reproducible, so the determinism control it exists to pass looks PERFECT under it, and the only thing that catches it is asking 'does the picture change at all'. Mirroring any of these into the oracle inherits the same failure, and a matched pair of deadlocks or a matched pair of frozen pictures would look like success. SO THE PRACTICAL ROUTE STAYS A WORKAROUND, and it should be built as one rather than dressed up: an OUTCOME-GATED capture. The frame gate currently decides before the frame renders, on draw count and content, and neither can express 'the frame in which this draw clipped to zero' because prims_after_clip is a pipeline statistic that exists only after rendering. The shape that fits the existing renderer: keep rendering past the gate and let the per-frame report test the condition, stopping the run on the first frame that satisfies it -- the diag table and the dumps are rewritten each frame, so the ones left on disk are that frame's. That is a real instrument, it selects by what HAPPENED rather than by identity, and it does not pretend to have fixed the non-determinism.
