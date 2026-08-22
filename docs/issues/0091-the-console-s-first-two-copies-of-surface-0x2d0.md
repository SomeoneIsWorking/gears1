---
id: 91
title: The first shadow masks were empty or under-marked
status: open
symptom: early shadow-mask outputs were flat or covered substantially less area than the reference
tags: oracle,render,resolve,gameplay-scene,mask
created: 2026-08-11
updated: 2026-08-13
---

## Root causes

Two interacting depth defects produced the symptom.

First, the renderer treated depth and stencil as one host image per sample count
instead of preserving distinct console memory bases. A shadow-atlas pass could
therefore overwrite scene depth used by a later mask pass. Depth ownership is now
keyed by the guest surface base, and intentional colour/depth aliasing is handled
explicitly.

Second, viewport depth mapping and fragment depth export used incompatible halves
of the console float-depth convention. A depth-restore pass consequently wrote a
buffer at half the required scale. Correlation had hidden this because it is
insensitive to uniform scale, while the depth test is not. The renderer now uses
one consistent depth convention across viewport and shader paths.

## Result

After both fixes, the first two shadow-mask outputs match the reference in
coverage and value variety, including the buffer that had previously been flat.
An interleaved control also confirmed that split depth ownership is required;
the shared-image arm still reproduced the empty mask after depth scaling was
corrected.

## Current status

The reported mask symptom is fixed, but this entry remains open because a later
whole-frame comparison used different UI states on the two sides and cannot serve
as the final regression gate. A replacement comparison must use shared input,
camera, UI state, and artifact provenance. Isolated zero-fragment stencil draws
are not defects by themselves; the reference also contains valid draws of that
shape.
