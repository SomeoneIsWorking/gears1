---
id: 32
title: Predicated tiles stack on top of each other: the frame's bottom 208 rows are never written
status: open
symptom: the rendered gameplay frame's bottom 208 rows are flat clear colour while the top rows are overexposed; 195 of the HDR surface's 391 draws carry PA_SC_WINDOW_OFFSET window_y_offset = -512
tags: gpu,draw,draw-backend,edram,tiling,resolve,predication,gameplay
created: 2026-07-28
updated: 2026-07-28
---

MEASURED from the per-draw diagnostic table (GEARS_DRAW_DIAG) on a captured Act 1
frame -- no extra runs; the numbers were already in the table:

    window_offset = 0x0         vp_y=0 vp_h=720  sc_y=0 sc_h=512    195 draws
    window_offset = 0x7e000000  vp_y=0 vp_h=208  sc_y=0 sc_h=208    195 draws
    (0x7e000000 decodes to window_y_offset = -512)

The frame is rendered in TWO PREDICATED TILES, and 720 = 512 + 208. Tile 1
renders rows 0..511. Tile 2 is shifted by -512 and renders rows **0..207** --
the TOP of our host target, on top of tile 1's output. Nothing ever writes rows
512..719.

That is the flat band at the bottom of the frame, and it is also part of the
overexposure: the top 208 rows carry two tiles composited over each other.

This is not a bug in the window offset -- the offset is applied correctly, through
Xenia's own GetHostViewportInfo/GetScissor. It is CORRECT Xenos behaviour that we
model wrongly. On the console EDRAM holds ONE TILE at a time: the guest renders
tile 1 into the EDRAM surface, RESOLVES it to main memory at that tile's
destination offset, then renders tile 2 into the SAME EDRAM surface (base 0x400
again, which is why the render-target cache correctly gives them one host image)
and resolves it to a DIFFERENT destination offset. The final image is the
assembled destination in main memory, not the EDRAM surface.

Our renderer presents the EDRAM surface itself, so the tiles stack instead of
being assembled.

THE FIX is the resolve copy rectangle, which is already a named gap on
re-frontier gameplay-scene: a resolve's primitive selects the REGION of the EDRAM
surface to copy and where it lands at RB_COPY_DEST_BASE, and we currently blit
the whole surface with no rectangle and no destination offset. Implementing the
rectangle, and presenting the assembled resolve DESTINATION rather than the EDRAM
surface, is what puts tile 2 at rows 512..719.

IMPORTANT CORRECTION this also forces, to re-frontier gameplay-scene and to the
reading of issue #30: "331 of 391 world draws are still killed at clip" is NOT
by itself a defect. Under tiled rendering each tile legitimately clips away the
geometry that does not fall in it, and the same vertex shader has 253 dead and 35
live draws with identical clip/cull/viewport-transform state and the same vertex
buffer -- the survivors differ only in which index subset they draw. A large
clipped fraction is EXPECTED. It should not be chased as a defect until the tile
assembly is correct, because the tile model changes what "should have been
visible" even means.
