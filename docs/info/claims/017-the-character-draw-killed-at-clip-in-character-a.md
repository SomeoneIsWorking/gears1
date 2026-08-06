---
id: C017
kind: claim
status: holds
created: 2026-08-06
tags: render,clip,catalog-77
depends: tools/skeleton_where.py
reconfirmed: 2026-08-06
verified_at: 2026-08-06 16:25:43
---

## Claim

The character draw killed at clip in character_auto.gfr (draw 319) is clipped CORRECTLY: the player is behind the camera. 43 of its 44 bone joints have w<0 after the guest's own world and view-projection, and the one remaining sits at ndc.x +3.86

## Evidence

tools/skeleton_where.py, calibrated in the same run against bright.gfr draw 460 -- which the GPU rasterised into 1431 primitives and 144191 fragments -- whose skeleton comes back 44 of 45 joints on screen spanning ndc.x -1.16..+0.11, ndc.y -0.68..+0.98. The transform chain was read out of vs 0x15cbc482459fe5b7's microcode (bone palette 3 rows per bone from c8, world x*c0+y*c2+z*c1+c3, view-projection c233..c236, all accumulator swizzles cancelling because the rotation P=(3,1,0,2) has order 3). Unused all-zero palette slots are excluded; counting them fabricated 9 on-screen joints.

## What would falsify it

the row-to-component ordering was fixed empirically against the calibration draw rather than unwound from four predicated accumulation sites, so a derivation from the microcode that contradicts the identity composition would falsify it; and this covers vs 0x15cbc482459fe5b7 only -- the killed character draws in prison_turn and play_v2 use different shaders whose layouts are unread

## Re-confirmed 2026-08-06

Strengthened 2026-08-06 after the operator asked whether an oracle compare had been run. It had not, and cannot settle this claim (see below), so the model was instead validated against EVERY draw in the tree whose GPU outcome is known -- all six draws using vs 0x15cbc482459fe5b7 were enumerated, three of which were issued: bright 460 rasterised 1431 prims -> 44 of 45 joints on screen, 0 behind; black 457 rasterised 1004 prims and 176754 fragments -> 14 of 45 on screen, 0 behind, the rest just past the frame edges; character_auto 319 killed at clip -> 0 on screen, 43 of 44 BEHIND THE CAMERA. Two positives from different captures and one negative, all agreeing with the hardware. WHY NO ORACLE: the offline capture->Xenia-trace path is instrument I013, DISTRUSTED (it renders a trace Xenia itself captured of a known-good frame as uniform black), and the live oracle cannot be aligned to a recorded frame -- this project's own control arm measures our run and the oracle at 17.7% identical pixels by guest frame 1200, and character_auto is frame 2913, so driving Xenia there renders a DIFFERENT game moment. The claim is about what the guest's own matrices did to this recorded frame, which the oracle cannot restate.
