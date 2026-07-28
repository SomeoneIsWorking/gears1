---
id: 32
title: Predicated tiles stack on top of each other: the frame's bottom 208 rows are never written
status: resolved
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

### Note (2026-07-28)
THE DESTINATION SIDE IS NOW EXACT, measured with a new resolve census (every
resolve's rectangle from vf0, its window offset, and its RB_COPY_DEST_PITCH /
RB_COPY_DEST_INFO shape) over the captured Act 1 frame:

    draw 407: color@0x400 -> 0xbde0000  rect y -0.5..511.5  window (0,0)
              destination pitch 1280 height 720 format 32
    draw 604: color@0x400 -> 0xc2e0000  rect y 511.5..719.5 window (0,-512)
              destination pitch 1280 height 208 format 32

ColorFormat 32 is k_16_16_16_16_FLOAT: 8 bytes per pixel. So

    1280 * 512 * 8 = 5242880 = 0x500000 = 0xc2e0000 - 0xbde0000

exactly. The two tiles are not two textures. They are ONE 1280x720 half-float
HDR texture based at 0xbde0000 -- tile 1 declares the destination height as 720,
the FULL frame, while writing only its first 512 rows -- and the guest folds
tile 2's row offset directly into RB_COPY_DEST_BASE, which is why the second
base is exactly 512 rows further in.

That is the whole defect, and it is now two precise statements rather than one
vague one:

  1. Resolve targets are keyed by RB_COPY_DEST_BASE, so this one texture becomes
     two unrelated host images. Any later pass whose fetch constant names
     0xbde0000 -- the texture base -- samples an image holding only rows 0..511.
     The bottom 208 rows of the scene are in the OTHER image, unreachable.
  2. The resolve blit ignores the rectangle and the destination offset: it copies
     the whole source surface to the start of the destination. Even with one
     image, tile 2 would land at row 0.

THE FIX, scoped: a resolve destination is a REGION OF A TEXTURE, not a texture.
Key resolve targets by the containing texture (base, pitch, height, format --
all four are in the registers above, no guessing needed), and blit the source
rectangle to its offset within it, deriving that offset from
(RB_COPY_DEST_BASE - texture base) / (pitch * bytes-per-pixel). A resolve whose
base falls inside an existing target's extent belongs to that target.

Note this also explains why the EDRAM surface stacking is NOT itself wrong: both
tiles correctly share one EDRAM host image (same base 0x400), and tile 1 is
resolved OUT (draw 407) before tile 2's draws begin (draw 604+). Our renderer
already executes the resolves in submission order. Only the destination side is
wrong.

### Resolution (2026-07-28)
FIXED. A resolve destination is now a REGION OF A TEXTURE rather than a texture. getResolveTarget takes RB_COPY_DEST_PITCH/_INFO alongside the base, and a destination whose base falls inside an existing target -- same pitch, same bytes-per-pixel, a whole number of rows in -- joins that target at the implied row offset instead of minting an image of its own. Measured on the captured frame: 'resolve destination 0xc2e0000 is row 512 of the texture at 0xbde0000 (1280x720), not a target of its own', and the frame's resolve destinations drop from 6 to 5. The blit now copies the guest's RECTANGLE to the guest's OFFSET: the rectangle comes from vertex fetch constant 0 per Xenia's GetResolveInfo (three vertices of two floats, plus the PA_SU_VTX_CNTL half-pixel offset, converted to 16p8 fixed with the top-left rule, shifted by PA_SC_WINDOW_OFFSET, clamped to the scissor and aligned to 8), and the destination offset is that rectangle's origin plus the routed row offset. A SECOND defect had to be fixed for this to work at all: the resolve blit set oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, which permits the driver to DISCARD the destination -- harmless while a resolve wrote the whole image, fatal once one tile's rows must survive the other tile's resolve. It now preserves, tracked per target. RESULT, replayed on the captured Act 1 frame: rows 512-719 go from flat (mean 115.2 std 5.1) to real scene content (mean 186.8 std 75.6). Verified live too: a gameplay frame renders full height with legible in-game subtitle dialogue (scratch/screenshots/live-tiles2/best.png), and the routing fires on every gameplay frame. Menus unregressed. The frame is still badly OVEREXPOSED -- that is the HDR-to-LDR tonemap, a separate gap on re-frontier gameplay-scene, not this.
