---
id: 34
title: The guest clears depth once per predicated tile; we cleared once per frame
status: resolved
symptom: a horizontal seam across the rendered gameplay frame at row 512, where the two predicated tiles meet
tags: gpu,draw,draw-backend,depth,clear,tiling,resolve,gameplay
created: 2026-07-28
updated: 2026-07-28
---

MEASURED on the captured Act 1 frame, then fixed.

On Xenos a clear is not a packet of its own -- it rides on a resolve. This
frame's copy draws show the guest clearing depth TWICE, once per predicated tile,
each clear riding on that tile's DEPTH resolve:

    draw 407: color@0x400 -> 0xbde0000  rect y   -0.5..511.5  window (0,0)
    draw 408: depth@0x0   -> 0xba40000  rect y   -0.5..511.5  window (0,0)     <- clears depth
    draw 604: color@0x400 -> 0xc2e0000  rect y  511.5..719.5  window (0,-512)
    draw 605: depth@0x0   -> 0xbcc0000  rect y  511.5..719.5  window (0,-512)  <- clears depth

We cleared depth exactly once, in the render pass that begins the frame. So the
second tile rendered its geometry against the FIRST tile's depth buffer, and
occlusion in the bottom 208 rows was decided by geometry that belongs to the top
512 -- which is the seam.

Compounding it, the depth resolves are the ones we cannot serve (no host depth
texture chain), so they were dropped entirely by the prep loop before their clear
was ever looked at. The copy being unserved does not make the clear unreal.

FIX: a kCopy draw's RB_COPY_CONTROL.depth_clear_enable is now read for EVERY copy
draw, including the ones whose copy cannot be served, and the clear is executed
at THAT POINT IN THE STREAM with the guest's own RB_DEPTH_CLEAR value (decoded
per RB_DEPTH_INFO.depth_format, so kD24FS8 goes through the 20e4 decode). A copy
that can only clear emits a clear-only entry.

MEASURED, brightness step across the tile boundary (mean of rows 505-511 against
rows 512-518):

    before (one clear per frame)   23.98 vs 22.46   step 1.52
    after  (one clear per tile)    24.10 vs 23.75   step 0.35

-- a 4.3x reduction, and the seam is no longer visible. 2 of 2 carried clears
execute. Non-black drops from 100.0% to 97.9%, which is the point: pixels that
should have been depth-rejected now are. Vulkan validation clean.
