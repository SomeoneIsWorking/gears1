---
id: 51
title: Comparing separate runs cannot measure a frame-cost change below ~10 ms
status: resolved
symptom: an optimisation that removes measured work makes the reported frame cost go UP
tags: perf,measurement,instrument,gpu,draw
created: 2026-07-30
updated: 2026-07-30
---

Gating a per-draw diagnostic that the timer measured at 3.6 ms of a 39.4 ms draw loop should
have made frames cheaper. Three runs of the same scripted Act 1 gameplay walk, each averaged
over its last 800 rendered frames at an identical 743 draws a frame:

    ungated   39.4 ms (within-run sd 1.7)
    gated     47.2 ms (within-run sd 5.6)
    gated     42.7 ms (within-run sd 2.5)

Both runs with the work REMOVED were slower than the run with it in. Between-run spread is
roughly 8 ms against a within-run spread of about 2, so the run itself is the dominant
variable and any effect smaller than it is unmeasurable that way -- in EITHER direction.
The tempting reading, "the change made it worse", has exactly as much support as "it helped",
which is none.

FIX: runtime/frame_ab.* alternates the two arms frame by frame inside ONE run, so both share
the machine state, the scene, the caches and the allocator, and drift over the run lands
equally on both. The summary refuses to report a difference smaller than the noise it could
have resolved (two standard errors of the difference of means, plus a 30-frame-per-arm floor
because early frames pay for shader translation and pipeline creation that no later frame
does). tests/test_frame_ab.cpp covers both directions: an unresolvable difference must be
reported as unresolved, and a real one must still be called real.

SETTLED THE ORIGINAL QUESTION: with GEARS_DRAW_AB_CENSUS=1 the census arm is +4.02 ms
(38.10 vs 34.08 ms over ~1500 frames each) against 2.01 ms of resolvable noise.

The rule for this project: no frame-cost change below ~10 ms a frame may be claimed from a
comparison of separate runs. Use the in-run A/B, or measure the removed work directly and say
plainly that the total cannot confirm it.
