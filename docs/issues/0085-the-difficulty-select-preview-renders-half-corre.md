---
id: 85
title: The difficulty-select preview renders half correct and half scrambled
status: open
symptom: on SELECT DIFFICULTY the preview panel shows correctly lit COG soldiers on its right and scrambled blue-green noise on its left, with a hard vertical boundary at x=447
tags: gpu,draw,menu,texture,movie,visible-defect
created: 2026-08-06
updated: 2026-08-06
---

## What is seen

`scratch/frames/ingame_v3.gfr`, replayed and gamma-boosted
(`scratch/h48/ingame_v3.png`): the SELECT DIFFICULTY screen. The surrounding menu
is correct -- Crimson-Omen red, legible text, the A/B glyphs in their right
colours -- which is itself the swizzle fix from this session working.

The preview panel in the middle is HALF RIGHT. Its right-hand side shows two COG
soldiers in armour, properly lit and properly coloured. Its left-hand side is
scrambled blue-green noise. The boundary is hard and vertical.

## Measured, not eyeballed

Column-mean discontinuity across the preview band (rows 120-340, full res):

    9.49 at x=447    <- the boundary
    8.68 at x=46
    8.25 at x=1147

**x=447 is NOT a tile boundary.** The frame is 1280 wide and EDRAM tiling would
split it at 640; the largest jump is nowhere near that, so the predicated-tiling
collapse is not the cause and `GEARS_DRAW_TILED=1` is not the arm to reach for.

## Why this matters beyond itself

It is the first evidence in this project that **characters CAN render correctly**
-- the soldiers on the right of that panel are lit, textured and correctly
coloured. Catalog #77's black character is therefore NOT a blanket "skinned
meshes render black" failure, and any theory that predicts one is wrong.

Whether this panel is live geometry or a played-back movie is NOT established
and decides where to look. The title's movie player hands the GPU new Y/U/V
planes at the same three addresses every frame (catalog #53), and a half-decoded
plane would look exactly like this. Check whether the frame contains a skinned
draw (a bone palette in its vertex constants, as catalog #77 identified for the
character) covering the panel's rectangle, or a YUV composite draw
(ps 0xea0007942db096ad is the movie's, and it has a native pass).

That one check decides between two very different investigations, and it is
cheap: `GEARS_DRAW_TEX_BINDS` and `GEARS_DRAW_VS_CONSTS` on the draws covering
x 400-880, y 120-340.
