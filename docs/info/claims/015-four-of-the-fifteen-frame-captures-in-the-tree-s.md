---
id: C015
kind: claim
status: falsified
created: 2026-08-06
tags: render,gameplay-scene,catalog-77
depends: runtime/frame_content.cpp, runtime/gpu_draw_xlate.cpp
falsified_on: 2026-08-06
---

## Claim

Four of the fifteen frame captures in the tree submit a skinned character mesh (bright, black, play_v2, prison_turn), not one; and in three of the five character-bearing captures EVERY colour-writing skinned draw is killed at clip while the same actor's colour-masked draws rasterise thousands of primitives in the same frame

## Evidence

tools/skinned_frames.sh over scratch/frames/*.gfr (4 of 15 FOUND). Clip asymmetry from joining each capture's skinned draw list against GEARS_DRAW_DIAG: colour-writing survive/killed = bright 3/0, black 3/0, play_v2 2/13, prison_turn 0/9, character_auto 0/3. Same-actor identity is measured, not assumed: GEARS_DRAW_VS_CONSTS=492,520 on character_auto.gfr shows both draws carrying identical bone rows (same pose, same frame), one with 4306 prims after clip and one with 0.

## What would falsify it

a capture where a colour-writing skinned draw is killed at clip while the player is demonstrably off screen would make the asymmetry a framing artefact rather than a defect; and the count is specific to the 15 captures in the tree on 2026-08-06

## FALSIFIED 2026-08-06

The SECOND half was an unsupported inference and is withdrawn. The colour-masked skinned draws that survive clipping (character_auto 520/538-543, prison_turn 519-523, play_v2 641-643, bright 690/693-695) are all in the depth-only block that feeds a 'depth 0x0 -> 0xc520000' resolve -- SHADOW MAP renders from the LIGHT's point of view, per tools/pass_structure.py on all four captures. A mesh inside the light frustum says NOTHING about the camera frustum, so 'the player is off screen cannot explain the kill' does not follow, and the clip kill of the colour-writing draws is NOT shown to be a defect. Compare catalog #74, where the same shape ('instances of one mesh, some wrongly clipped') was retracted after the vertices were actually transformed and found to be genuinely outside the frustum. The FIRST half stands and is re-recorded as C016: 4 of 15 captures submit a skinned character.

> Anything that cited this claim as proof must be re-checked. Grep the repo for it.
