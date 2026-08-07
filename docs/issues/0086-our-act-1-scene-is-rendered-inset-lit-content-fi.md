---
id: 86
title: Our Act 1 scene is rendered INSET: lit content fills 63% of the frame width where the console fills all of it
status: open
symptom: same scene, live oracle: our lit content spans x 155..962 of 1280 and y 43..708; the oracle's spans 0..1279 and 0..719; coverage above 0.02 luminance 44.5% against 89.1%
tags: render,viewport,oracle,gameplay-scene
created: 2026-08-07
updated: 2026-08-07
---

Found by the first LIVE same-scene oracle comparison (catalog #77, 2026-08-07),
both cores driven to Act 1 independently -- no trace replay on either side.

The geometry is not wrong: the brick wall, its three windows and the camera
framing match the oracle's shot proportionally. The whole scene is drawn SMALLER
AND CENTRED, with dark borders left, right, top and bottom, where the console's
image reaches every edge.

    ours   scratch/oracle/stream/ours_frames/frame_07200.ppm
           lit content x 155..962 (808 of 1280, 63%), y 43..708, mean 0.038
    oracle scratch/oracle/stream/theirs_frames/frame_006000.png
           lit content x 0..1279, y 0..719, mean 0.087
    coverage above 0.02 luminance: ours 44.5%, oracle 89.1%

NOT YET INVESTIGATED. The candidates, in the order worth trying:

  1. the viewport scale/offset our renderer derives -- `PA_CL_VTE_CNTL` and the
     raw viewport scale/offset registers are already columns in
     `GEARS_DRAW_DIAG`, so this costs one replay of a capture of this scene;
  2. a resolve destination pitch/extent mismatch;
  3. a present-time blit that letterboxes.

The two frames are NOT the same guest moment -- catalog #84 establishes that is
unavailable -- but the inset is a whole-frame GEOMETRIC property that does not
depend on the moment, and it is present in every frame of our run, so the
moment mismatch does not weaken it.

### Note (2026-08-07)
FALSIFIED 2026-08-07, in the session that filed it. There is no inset. The
bounding box that produced this entry could not tell "not rendered" from
"rendered dark", and both of its ends were wrong.

  * OUR SIDE. Gamma-boosting frame_07200 by 0.28 shows the "border" is fully
    rendered geometry -- the side walls, ceiling and floor of the dark room the
    camera is standing in. Of the 383,472 border pixels only 33% are exactly
    zero; 63% sit in 0.002..0.02, i.e. just under the threshold the measurement
    used. The bright rectangle is a DOORWAY, and the lit courtyard beyond it.
  * THEIR SIDE. Gamma-boosting frame_006000 the same way shows the oracle's
    camera is several metres FURTHER FORWARD: the wall fills its frame, there is
    no doorway in shot at all. The two frames are not the same camera position,
    so their bounding boxes were never comparable.

The instrument's real defect: a luminance-threshold bounding box over a scene
whose surround is legitimately dark reports the LIT SUBJECT's extent, not the
RENDERED extent, and the difference between the two reads as a geometry bug.
Comparing that number across two frames at different camera positions then
compounds it.

Ruled out along the way, and worth keeping: the draw-level viewport/scissor was
never the cause. The viewport census on both the live run and a replay of
scratch/frames/bright.gfr shows every scene draw at full width -- 1280x208 and
1280x720/scissor 1280x512 (the two predicated tiles), 1280x720 for the rest.
Candidate 1 in the original entry is disproved, not merely untried. A replay of
bright.gfr fills the frame edge to edge with no inset of any kind.

What survives from the comparison that filed this: the missing character and the
missing HUD (catalog #77), which are moment-INDEPENDENT -- third person puts
Marcus in shot in every gameplay frame -- and the brightness difference, which
is catalog #62.

### Dead end (2026-08-07)
There is no inset: the border is rendered-but-dark geometry (a doorway's surround) and the oracle frame is at a different camera position entirely. Falsified by a gamma boost of both frames
