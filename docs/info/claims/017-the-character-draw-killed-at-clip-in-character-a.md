---
id: C017
kind: claim
status: holds
created: 2026-08-06
tags: render,clip,catalog-77
depends: tools/skeleton_where.py
---

## Claim

The character draw killed at clip in character_auto.gfr (draw 319) is clipped CORRECTLY: the player is behind the camera. 43 of its 44 bone joints have w<0 after the guest's own world and view-projection, and the one remaining sits at ndc.x +3.86

## Evidence

tools/skeleton_where.py, calibrated in the same run against bright.gfr draw 460 -- which the GPU rasterised into 1431 primitives and 144191 fragments -- whose skeleton comes back 44 of 45 joints on screen spanning ndc.x -1.16..+0.11, ndc.y -0.68..+0.98. The transform chain was read out of vs 0x15cbc482459fe5b7's microcode (bone palette 3 rows per bone from c8, world x*c0+y*c2+z*c1+c3, view-projection c233..c236, all accumulator swizzles cancelling because the rotation P=(3,1,0,2) has order 3). Unused all-zero palette slots are excluded; counting them fabricated 9 on-screen joints.

## What would falsify it

the row-to-component ordering was fixed empirically against the calibration draw rather than unwound from four predicated accumulation sites, so a derivation from the microcode that contradicts the identity composition would falsify it; and this covers vs 0x15cbc482459fe5b7 only -- the killed character draws in prison_turn and play_v2 use different shaders whose layouts are unread
