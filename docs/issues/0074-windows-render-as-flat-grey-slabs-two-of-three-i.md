---
id: 74
title: Windows render as flat grey slabs: two of three identical draws are killed by clipping
status: open
symptom: in the courtyard capture two of the three windows on the back wall are flat untextured grey slabs while the third shows its content; the same defect appears on three of four windows in the bright capture
tags: gpu,draw,clip,vertex,basepass,windows,visible-defect
created: 2026-08-05
updated: 2026-08-05
---

## Symptom

`courtyard.gfr`: three windows on the back wall. The left and middle are flat
grey slabs with no content; the right one shows its texture. `bright.gfr` shows
the same thing on three of four windows.

## Localised, with the pixel trace

`GEARS_DRAW_PIXEL_TRACE=<x>,<y>` (new, see `docs/knobs.md`) reports every draw
that changed one texel. On a slab and on the good window:

    (395,258) slab:  after 263 draws = (2.72, 3.01, 3.03, 1)  <- draw 262 ps 0x9bab457164bad66d
    (835,258) good:  after 263 draws = (1.87, 2.27, 2.38, 1)  <- draw 262 ps 0x9bab457164bad66d
                     after 289 draws = (2.11, 1.89, 1.13, 1)  <- draw 288 ps 0xa657492ede132731

Draw 262 lays the flat grey under all three. The content comes from draw 288,
and it reaches only one window.

## The cause is upstream of the pixel shader

Draws 286, 287 and 288 are the SAME shader pair (vs `45121b3c36158d3d`, ps
`a657492ede132731`), the SAME index count (804) and, per the diag table,
BYTE-IDENTICAL raster state -- viewport 1280x720, scissor 1280x720, window
offset 0, `vte_cntl` 0x43f, `su_sc_mode` 0x18002, `clip_cntl` 0x80000. They are
three instances of one window mesh.

    286  prims_after_clip = 0   killed_by_clip_or_cull
    287  prims_after_clip = 0   killed_by_clip_or_cull
    288  prims_after_clip = 87  shaded, 90716 fragment invocations

289/290/291 are the same story with ps `ada78af5af7e3a2c` (342 indices): two
killed, one shaded. So two of every three window instances are thrown away
before rasterisation.

**Not culling.** `GEARS_DRAW_NOCULL=1` changes nothing for 286/287/289/290 --
they stay at 0 primitives -- while 288 goes 87 -> 131 and 291 80 -> 82. The
geometry is being CLIPPED, not back-face rejected.

**Not the EDRAM tiling.** Tile replays differ in window offset and scissor;
these three are identical in both. Same state, same counts, different outcome
means the difference is in what the vertex shader produced -- i.e. the vertex
constants (the per-instance transform) or the vertex fetch for those draws.

## Next

Find why the transformed positions of 286/287 fall outside the clip volume.
`GEARS_DRAW_VDUMP=286` dumps the draw's first vertices at the shader's own
stride; compare against 288. If the input vertices are identical, the per-draw
vertex float constants are the difference and `GEARS_DRAW_PS_CONSTS`' vertex
counterpart is what is needed next.
