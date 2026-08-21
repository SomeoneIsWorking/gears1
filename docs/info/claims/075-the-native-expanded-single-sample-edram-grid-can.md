---
id: C075
kind: claim
status: holds
created: 2026-08-21
tags:
depends: runtime/gpu_draw.cpp
---

## Claim

The native expanded single-sample EDRAM grid cannot represent both diagonal Xenos 2X raster sample positions and drops the in-game draw-650 wall light at the depth test.

## Evidence

walk_gameplay.gfr: native draw 650 has 0 fragment invocations versus Xenia 79,253; its exact 24,520-pixel color mask overlays a native-minus-Xenia depth offset of about 3.089e-5 written by draw 612. A diagnostic +0.25-pixel horizontal viewport shift reduced the depth residual below 1e-6 and restored exactly 79,253 fragments, while -0.25 doubled the error. The existing model places both stored 2X rows at x+0.5, but Xenos uses x+0.75 and x+0.25.

## What would falsify it

A reference capture disproves the Xenos 2X diagonal sample positions, or a replay using true per-sample positions still produces the same draw-650 depth-test loss.
