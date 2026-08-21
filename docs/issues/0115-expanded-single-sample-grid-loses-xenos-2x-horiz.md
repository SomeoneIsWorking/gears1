---
id: 115
title: Expanded single-sample grid loses Xenos 2X horizontal sample positions
status: investigating
symptom: An in-game light-volume draw shades zero fragments natively but 79,253 in Xenia, leaving the lower-right wall unlit
tags: render,msaa,depth,raster,gameplay-scene
created: 2026-08-21
updated: 2026-08-21
---

## Root cause

The native render target is a single-sample `1280x1440` EDRAM sample grid.
For a 2X `1280x720` draw, scaling the viewport vertically by two evaluates the
two stored rows at `(x + 0.5, y + 0.25)` and `(x + 0.5, y + 0.75)`. Xenos 2X
sample positions are diagonal: `(x + 0.75, y + 0.75)` and
`(x + 0.25, y + 0.25)`. The grid preserves the EDRAM addresses but loses both
horizontal subpixel positions.

On `scratch/walkcap/walk_gameplay.gfr`, draw 612 restores the scene depth that
the initial 2X pass resolved through sample 0. Under later draw 650's 24,520
covered pixels, native depth is higher than Xenia's logical depth at every
pixel by `3.077e-5..3.094e-5`. The plane's measured horizontal gradient is
`1.27338e-4` per pixel; one quarter of it is `3.183e-5`, identifying the missing
quarter-pixel horizontal sample position rather than float24 noise.

The observable consequence is categorical under reverse-Z GEQUAL: native draw
650 assembles six primitives, keeps two, and invokes zero fragments; Xenia uses
the same shaders, SPIR-V, geometry, constants and depth state but invokes
79,253 fragments. The missing fragments are the visibly absent light on the
lower-right wall.

## What was tried / dead ends

Ruled out with exact fingerprints or controls: shader translation, vertex and
index bytes, constants, texture fetches, viewport depth convention, stencil,
float24 quantization, polygon offset, and the presence/compare mode of the
depth attachment. The target draw's four polygon-offset registers are signed
zero.

A diagnostic `+0.25` shift of every 2X draw's host viewport reduced the
native-minus-Xenia depth residual under the target from `+3.087e-5` to about
`-9.7e-7` and changed draw 650 from zero to exactly 79,253 fragment
invocations. The missing wall light appeared. That change was removed: the
frame's 2X colour copies average samples 0 and 1, whose horizontal offsets are
opposite. Shifting both rows fixes selected depth sample 0 while making the
other depth sample and the colour average wrong; it is a falsifier, not a fix.

## Resolution

Open. The correct fix is real multisample render-target ownership (or an
equivalent representation that preserves per-sample raster positions), with
explicit pack/unpack when EDRAM ownership moves between 1X, 2X and 4X views.
The existing vertically expanded single-sample image cannot represent the two
diagonal 2X positions with one viewport transform. Do not reintroduce a global
quarter-pixel shift as a workaround.
