---
id: C038
kind: claim
status: holds
created: 2026-08-12
tags: render,present,resolve,oracle
depends: runtime/gpu_draw.cpp, runtime/gpu_draw_resolve.cpp
---

## Claim

Our presented frame is NOT inset. The presented image and the front-buffer resolve destination are the same picture, differing only by a near-constant ~3/255 brightness offset, and the 'inset' box is a luminance-threshold artefact of a dark scene.

## Evidence

scratch/camgate/match (camera-gated capture). tools/img_extent.py sweep: frame.ppm spans x 0..1279 at threshold 0.002 and x 254..1013 at 0.100; resolve_15 (front buffer, 0x00311000) spans x 0..1279 at 0.002 and x 248..1013 at 0.100; resolve_00 (srcC400, previously called 'the only inset pass') spans x 0..1279 at 0.002. img_extent.py compare frame.ppm vs resolve_15: mean |channel diff| 2.99/255, mean SIGNED diff -2.95/255 over 230,400 sampled pixels -- a constant offset, not a crop or a scale. Discriminator validated against both classes by --selftest.

## What would falsify it

a frame whose content box HOLDS below full width at threshold 0.002 -- i.e. genuinely nothing rendered outside it -- or a pixelwise compare of the presented frame against the front-buffer destination showing a geometric rather than constant-offset difference
