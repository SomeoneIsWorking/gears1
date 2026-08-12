---
id: 86
title: Our Act 1 scene is rendered INSET: lit content fills 63% of the frame width where the console fills all of it
status: open
symptom: same scene, live oracle: our lit content spans x 155..962 of 1280 and y 43..708; the oracle's spans 0..1279 and 0..719; coverage above 0.02 luminance 44.5% against 89.1%
tags: render,viewport,oracle,gameplay-scene
created: 2026-08-07
updated: 2026-08-12
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

### Note (2026-08-12)
STILL REPRODUCES AT A MATCHED CAMERA, HORIZONTALLY ONLY, AND THE BORDERS ARE ASYMMETRIC. Measured on the camera-gated capture (scratch/camgate/match/frame.ppm, our frame taken at the console's own view-projection): lit content above 0.02 luminance spans x 176..1014 of 1280 and y 0..718 of 720. Compare what this entry records -- x 155..962, y 43..708. THE HORIZONTAL INSET IS INTACT: 838 of 1280 columns, 65.5%, against the 63% in the title, and it is the same defect. THE VERTICAL INSET IS GONE: the frame is lit from row 0 to row 718 where this entry had 43..708. Something between then and now fixed the vertical axis and left the horizontal one, which is itself a clue -- whatever is wrong is not a uniform centred scale. AND THE BORDERS ARE NOT EQUAL: 176 columns dark on the left, 266 on the right (1280 - 1014). A centred scale would leave equal borders; unequal ones mean an OFFSET as well as an extent, so the viewport's x offset and x scale are both suspect rather than a single scale factor. DO NOT compare the coverage figure across these two measurements: this capture reads 4.9% against the 44.5% recorded here, because it is a dark shadow-heavy gameplay moment rather than the lit Act 1 shot this entry used. The bounding box is comparable between scenes and the coverage fraction is not. The camera gate makes this the first measurement of this defect taken at the console's own viewpoint, so the geometry it reports is not confounded by the two sides looking at different moments.

### Note (2026-08-12)
THE SCENE IS NOT DRAWN SMALLER. THE INSET IS INTRODUCED AFTER IT. This entry's reading -- 'the whole scene is drawn SMALLER AND CENTRED' -- is contradicted by its own passes, measured on the camera-matched capture. Horizontal extent of lit content, pass by pass, in the frame whose presented image is inset to x 176..1014: the DEPTH pass srcD000 1280x720 f23 spans x 0..1276, FULL WIDTH. The main colour passes srcC2D0 1280x720 f7 span 0..1272 and f6 spans 0..1276, 99-100% of the width. Both shadow atlases span their full 864. The ONLY inset pass is the very first resolve, srcC400 1280x720 f32 at draw 638, whose content sits at x 248..1012 -- 60% of the width, the same proportion as the presented frame's 65.5%. So the geometry, the viewport and the projection are all fine: we rasterise the scene across the whole target, and the depth buffer proves it independently of any colour or lighting question. Something downstream composites from an inset intermediate instead of from the full-width scene, and that is where this defect lives. WHERE TO LOOK: the chain from surface 0x400 rather than the viewport registers this entry points at. The asymmetric borders recorded in the previous note -- 176 columns left against 266 right -- are consistent with a composite reading a sub-rectangle at the wrong offset, and not with any scale applied to the scene itself. WHAT WOULD FALSIFY THIS: a frame in which the srcC2D0 colour passes are themselves inset. They are not in this one, and the depth pass agrees with them.
