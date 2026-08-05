---
id: 74
title: Windows render as flat grey slabs, and nothing in the frame was ever going to fill them
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

## RETRACTED: "three instances of one mesh, two wrongly clipped" is WRONG

The section above calls 286/287/288 "three instances of one window mesh" and
treats two of them being clipped as the defect. Measured, both halves are false.
**The clipping is correct**, and this frame contains no draw that could ever
have filled the other two openings.

**They share a mesh, not a place.** `GEARS_DRAW_VDUMP` on all six draws: the
geometry stream is byte-identical -- every one fetches `0xe585000` at stride 11
and dumps the same vertices. Only the second binding differs (`0xea90000` /
`0xea91400` / `0xea92800`, stride 3: per-vertex lighting). So the input
vertices ARE identical, which is what the old "Next" said to check.

**The per-instance transform is the difference**, read with the new
`GEARS_DRAW_VS_CONSTS` (see `docs/knobs.md`; `PS_CONSTS` keys on a shader hash
and cannot separate instances sharing one). Of 16 vec4s, c7..c11 -- the
view-projection -- are identical across all six, and only c0..c6 (the world
matrix) and c12..c14 (per-instance vertex lighting) differ. The world
translations are not neighbours on one wall:

| draws | world translation (c3) |
|---|---|
| 286, 289 | (15416.9, -2809.7, -32.2) |
| 287, 290 | (26035.1, 10463.1, -3560.0) |
| **288, 291** | **(-16564.2, 19323.8, -128.0)** |

The pairing is coherent -- each instance draws its glass (804 indices) and its
frame (342 indices) -- so this is one window mesh placed three times across the
level, tens of thousands of units apart, not three windows on one wall.

**The two clipped instances are genuinely outside the frustum, in the guest's
own numbers.** Pushing the dumped vertices through c0..c3 then c7..c10:

    draw 286: all 4 vertices w < 0            -- BEHIND THE CAMERA
    draw 287: all 4 vertices ndc.x -5.4..-3.8 -- far off the left of the screen
    draw 288: all 4 vertices inside the clip volume, ndc ~(+0.5, +0.4)

That arithmetic is `tools/clip_volume_check.py`, and it is only worth something
because it agrees with the GPU on the draw that DID rasterise: 288 comes out
inside, and it is the one the pipeline statistics credit with 87 primitives and
90716 fragments. The script prints that agreement as a pass/fail line every run
-- the constant order is a hypothesis, and a wrong guess at it makes the script
say so instead of producing a confident wrong answer.

## What is actually unexplained

Only these six draws in the whole frame use the window shaders
(`a657492ede132731`, `ada78af5af7e3a2c`), and the pixel trace on a slab pixel
(395,258) confirms the base pass touches it exactly once, at draw 262. So the
other two openings have no content draw in this frame at all -- their being
grey is not a draw that went wrong, it is a draw that was never submitted.

Which makes the open question a different one, and one that cannot be settled
inside the renderer: **is the guest right to submit nothing there?** The
renderer is faithfully drawing what the command stream asked for. Either the
title culled those meshes on the CPU using state we feed it wrongly, or the
openings are meant to look like that and this was never a defect. Do not
re-open this as a clipping bug; the clip is exonerated on measurement.

### Note (2026-08-05)
Clipping exonerated by measurement: the three 'identical' draws differ in their world matrix (GEARS_DRAW_VS_CONSTS, new) and 286/287 are genuinely outside the frustum -- 286 behind the camera. See the RETRACTED section and tools/clip_volume_check.py.
