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
