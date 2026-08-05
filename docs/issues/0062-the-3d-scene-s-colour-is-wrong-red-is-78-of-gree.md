---
id: 62
title: The 3D scene's colour is wrong: red is 78% of green frame-wide, and lit surfaces flatten at 0.30
status: open
symptom: Gameplay looks flat grey-green with blown, detail-free lit surfaces; menus and the title screen look correct in the same run
tags: gpu,draw,colour,tonemap,resolve
created: 2026-08-05
updated: 2026-08-05
---

## The observation that redirects everything

The reported title-screen capture is CORRECT -- dark blue, proper contrast, crisp
text. Only the 3D scenes are wrong, in the same run. That rules out every
whole-frame explanation I pursued for two days: a swapchain format, a colour
space, a compositor transform and an sRGB encode would all wash the menus too.

It is content-specific, which means it is in the part of the pipeline the menus do
not use: the deferred scene path, its HDR surface, the resolve that carries it and
the tonemap that consumes it.

## Measured on our own presented gameplay frame

Whole frame, per channel:  R 0.0772  G 0.0996  B 0.0990
                           -> RED IS 78% OF GREEN, and green and blue are equal.

The sunlit wall through the doorway: mean 0.193, p99 0.298, per-channel
R 0.174 G 0.204 B 0.200. A daylit concrete wall is neutral or warm; this is
uniformly cool, and it flattens at 0.30 with no highlight above it.

Two separate defects in one number: a RED DEFICIT of about a fifth, and a CEILING
that a lit surface cannot pass.

## What this retracts

Every "the renderer's output is correct" in this session was my own judgement of a
dim scene against no reference. It was wrong. The frames I called correct have the
same cast and the same ceiling as the reported screenshots -- the reports were just
of brighter scenes where it is obvious. Catalog #60 and #61 chased the difference
between my captures and the window when the interesting difference was between
BOTH of them and the console.

## Where to look, in order

1. The resolve's `copy_dest_swap` and the exponent bias -- catalog #33 found the
   bias wrong once already (the tonemap's input was 8x too bright), and a red/blue
   asymmetry is exactly what a swizzle or a per-channel scale gets wrong.
2. The 7e3 surface format carried as half-float: an unequal-precision packing would
   hit one channel differently.
3. The tonemap shader's own constants, read from the register file.

An oracle settles it in one comparison and there is one available: Xenia canary
renders this title correctly (user's report), and `extern/xenia` is already
vendored. A single frame of its output next to ours ends the guessing that this
entry exists because of.

### Note (2026-08-05)
## The red deficit is a RED/BLUE SWAP, and it is 100% of pixels

Not a tonemap, not an exponent bias, not a 7e3 packing. The frame we present is
an exact channel exchange of the frame the guest composed.

`GEARS_DRAW_RESOLVE_DUMP=1` now reports **per-channel means** for every resolve
target (new; the old line's range was a max over all three channels, which is
identical whether red is short or not). On courtyard.gfr:

    resolve 0x311000   R 0.076818  G 0.077251  B 0.059357     <- the FRONT BUFFER
    resolve 0xc7f9000  R 0.059357  G 0.077251  B 0.076818
    presented frame    R 0.059357  G 0.077251  B 0.076818

Same three numbers, to six digits, with R and B exchanged. Confirmed per pixel,
not by aggregate:

    presented vs resolve 0xc7f9000 : identical             100.0% of pixels
    presented vs resolve 0x311000  : identical after R/B swap 100.0% of pixels

The capture's `frontBufferAddress` is 0xa0311000 in all three captures, so
0x311000 is the buffer the guest hands to scanout. **What we present is not it.**

R/G = 0.7688 was never a red deficit: red is carrying blue's level. The
"ceiling at 0.30" is a separate observation and is NOT explained by this.

## Which resolve swaps, measured with the existing control arm

`GEARS_DRAW_RESOLVE_NOSWAP=1` changes 0xc7f9000 (R 0.059357 -> 0.076818, i.e.
it matches 0x311000) and leaves 0x311000 untouched. So the guest asks for
`copy_dest_swap` on the resolve to 0xc7f9000 and NOT on the one to the front
buffer, and our resolve honours both correctly. The swap in the presented image
does not come from the resolve.

Both host formats are RGBA -- resolve targets are all R16G16B16A16_SFLOAT
(gpu_draw_xlate.cpp) and the readback is R8G8B8A8 -- so component 0 really is
red in both dumps and the channel labels above are not a BGRA mislabel.

## Next

The presented image is byte-identical to the 0xc7f9000 TEXTURE, which is a
resolve destination, not the surface the front buffer names. So the question is
in the present path, not the colour path: does it present the wrong image, or
does the 0xc7f9000 resolve write into the host image that gets presented?
Read the presentBase selection in gpu_draw.cpp (~line 1730) and what the
readback at ~1866 actually copies from.

Do not re-open the tonemap, the exponent bias or the 7e3 packing on this
evidence: a per-pixel 100% match cannot be produced by any of them.
