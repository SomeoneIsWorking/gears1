---
id: 89
title: Guest frame 2920 is not the first gameplay render in every run, so a frame index is not a landmark
status: resolved
symptom: captured pass layers at guest frame 2920 on both sides; ours rendered a 3-draw loading frame with 1 resolve and the oracle dumped 1 copy, where the run the index came from had 586 draws there
tags: oracle,tooling,workflow,gameplay-scene
created: 2026-08-07
updated: 2026-08-07
---

The first gameplay render was located as guest frame 2920 by reading one run's
draw stream: a clean 3-draw -> 586-draw transition, frames 2908..2919 at 3 draws
and 2920 onward at ~650. That number is correct FOR THAT RUN and does not
survive another one.

Captured at 2920 in fresh runs on both sides:

    ours    a 3-draw frame, 1 resolve destination (0x311000), still loading
    oracle  1 copy at 0x1F606000, i.e. a front-buffer resolve and nothing else

So both sides were still on a loading screen at the index where the earlier run
was already in gameplay. The level load takes a variable number of presents --
it is I/O and decode bound, and our runtime additionally renders nothing while
it waits -- so the frame counter is a good INDEX for joining two filmstrips
(that is what it is for, catalog #87) and a bad LANDMARK for naming a moment.

Setting GEARS_DRAW_FRAME_AT also perturbs the thing it measures on our side:
skipping rendering until frame N changes how long the loading frames take, so
the guest reaches gameplay at a different count than in a run that rendered
throughout.

WHAT TO DO INSTEAD -- select the frame by CONTENT, not by index. The runtime
already has the pattern: GEARS_DRAW_FRAME_DUMP_SKINNED=1 makes the capture
self-selecting by scanning for the first frame that submits a skinned character
mesh. The equivalent for this is "the first frame with at least N draws", which
is a one-line predicate on the same scan, and the oracle needs the same
selector rather than --oracle_dump_at_frame.

Until that exists, a paired layer capture has to be taken by dumping across a
RANGE of frames and picking the gameplay ones afterwards, which is expensive on
the oracle: each resolve dump flushes the deferred command buffer and reads the
whole 512 MiB shared-memory buffer back, so dumping every resolve of every frame
runs at about 0.8 fps and never reaches gameplay at all.

### Resolution (2026-08-07)
content selection replaces the frame index on both sides: GEARS_DRAW_FRAME_MIN_DRAWS in the runtime, GEARS_ORACLE_DUMP_MIN_DRAWS in the fork, same rule (the frame AFTER the first with >= N draws) so the two land on the same game moment. Verified: three of our runs selected 2926/2935/2942 and the oracle selected 2947 from its own 2946, all genuine gameplay frames, where the fixed index 2920 had been a loading screen in both.
