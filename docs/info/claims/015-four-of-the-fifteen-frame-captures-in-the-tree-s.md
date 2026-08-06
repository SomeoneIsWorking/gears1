---
id: C015
kind: claim
status: holds
created: 2026-08-06
tags: render,gameplay-scene,catalog-77
depends: runtime/frame_content.cpp, runtime/gpu_draw_xlate.cpp
---

## Claim

Four of the fifteen frame captures in the tree submit a skinned character mesh (bright, black, play_v2, prison_turn), not one; and in three of the five character-bearing captures EVERY colour-writing skinned draw is killed at clip while the same actor's colour-masked draws rasterise thousands of primitives in the same frame

## Evidence

tools/skinned_frames.sh over scratch/frames/*.gfr (4 of 15 FOUND). Clip asymmetry from joining each capture's skinned draw list against GEARS_DRAW_DIAG: colour-writing survive/killed = bright 3/0, black 3/0, play_v2 2/13, prison_turn 0/9, character_auto 0/3. Same-actor identity is measured, not assumed: GEARS_DRAW_VS_CONSTS=492,520 on character_auto.gfr shows both draws carrying identical bone rows (same pose, same frame), one with 4306 prims after clip and one with 0.

## What would falsify it

a capture where a colour-writing skinned draw is killed at clip while the player is demonstrably off screen would make the asymmetry a framing artefact rather than a defect; and the count is specific to the 15 captures in the tree on 2026-08-06
