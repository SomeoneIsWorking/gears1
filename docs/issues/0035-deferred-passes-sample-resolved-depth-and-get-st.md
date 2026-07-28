---
id: 35
title: Deferred passes sample resolved DEPTH and get stale guest memory
status: open
symptom: 30 texture bindings in an Act 1 frame name a depth resolve destination; we do not serve depth resolves, so those bindings decode whatever the guest left in memory
tags: gpu,draw,draw-backend,resolve,depth,deferred,gameplay
created: 2026-07-28
updated: 2026-07-28
---

MEASURED, and it CORRECTS a claim I put on the re-frontier last iteration.

I recorded "only 24 of 5278 texture bindings are served by a resolve target" as
the largest structural gap, implying most bindings were being mis-served. The
census now classifies every base three ways -- IS a resolve target, falls INSIDE
one, or is an ordinary guest asset -- and the ratio is LEGITIMATE:

    136 distinct texture bases, 5278 bindings
     24 bindings name a colour resolve target, and are served from it
      0 bindings fall inside a resolve target (no sub-rectangle routing miss)
   ~5224 bindings are ordinary guest assets -- world art, correctly uploaded

So the frame is not mostly mis-served. Most texture bindings genuinely are guest
textures, and 24 render-target samples in a frame is a normal number.

THE REAL GAP is narrower and specific. Thirty bindings name a DEPTH resolve
destination:

    0xba40000   28 bindings   (tile 1's depth resolve destination)
    0xc510000    2 bindings

We do not serve depth resolves -- there is no host depth texture chain -- so
every one of those 30 bindings falls through to uploadTexture and decodes
whatever the guest last left at that address. On a deferred renderer these are
almost certainly the lighting and post passes reconstructing position from depth,
which is why it matters out of proportion to the count.

WHAT IT NEEDS, and why it is not just "run the existing resolve on the depth
image": the guest resolves depth to a k_8_8_8_8 destination.

    draw 408: depth@0x0 -> 0xba40000  pitch 1280 height 720
              format 6 (k_8_8_8_8) number 2 endian 2 (k8in32) exp_bias 0

That is the Xenos depth-as-colour resolve: the 24_8 depth/stencil is copied into
an 8888 texture and the sampling shaders decode it. Our host depth is
D32_SFLOAT, so serving this needs an ENCODE -- float32 depth back to the guest's
24-bit depth format (float24 20e4 for kD24FS8, unorm24 for kD24S8) packed with
its stencil into 8888, with the k8in32 endian applied -- not a copy.

The compute resolve built for catalog #33 is the right vehicle: this is a second
shader variant reading the depth image and writing the packed 8888 texture. The
20e4 encode is the inverse of the Depth20e4To32 already ported for the depth
clear.
