---
id: C007
kind: claim
status: holds
created: 2026-08-05
tags: gpu,native-renderer,edram
depends: runtime/gpu_draw.cpp
---

## Claim

This title's predicated EDRAM tiling is a pure replay: the base pass is the same draw list issued twice with a different scissor band and window offset, and collapsing it to one pass reproduces the image

## Evidence

Per-draw comparison of the two tiles of the Act 1 courtyard frame: 174 draws each, and 40 of the 46 columns of the GEARS_DRAW_DIAG table are IDENTICAL on all 174 pairs -- same vertex and pixel shader, index count, primitive type, colour mask, blend, depth control, surface. Only three PROGRAMMED values differ: viewport height 720 vs 208, scissor height 512 vs 208, and PA_SC_WINDOW_OFFSET 0x0 vs 0x7e000000 (window_y_offset = -512). The other three differences are outcomes, not inputs. GEARS_DRAW_UNTILE=1 collapses it and the presented frame is BIT-EXACT on 3 of 4 captures (act1_v2, bright, play_v2: 2,764,800 of 2,764,800 channel samples). On courtyard 197 of 2,764,800 samples differ by one, all inside the second tile's band over 124 rows, and the cause is measured rather than assumed: primitives after clip fall 894->818 because a triangle spanning the seam is rasterised twice under tiling and once without, while fragment invocations move by +3 in 1,730,163 -- so it is rasterisation of seam-crossing triangles, not lost draws and not a depth-semantics change (bright and play_v2 have 557 and 453 seam-crossing primitives and are bit-exact).

## What would falsify it

a capture where collapsing changes fragment invocations by more than a handful, or where the differing pixels are NOT confined to the replayed tile's band -- either would mean the replay test is admitting something that is not a replay
