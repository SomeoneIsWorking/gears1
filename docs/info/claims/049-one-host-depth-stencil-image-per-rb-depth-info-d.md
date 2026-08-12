---
id: C049
kind: claim
status: falsified
created: 2026-08-12
tags: 
falsified_on: 2026-08-12
---

## Claim

One host depth+stencil image per RB_DEPTH_INFO.depth_base is the correct model and now the default; it takes the shadow atlas depth resolve from 0.34 to 0.999 against the console, and regresses the first shadow mask from 0.95 to 0.80

## Evidence

tools/depth_arm_ab.sh on scratch/camerapair_ps: one frozen camera replayed through four arms (shared, split, shared-again, variable UNSET) all scored against console frame 793. Atlas depth resolve srcD5A0 864x864 f22: 0.3395/0.3441 shared vs 0.9983/0.9989 split, stable across camera residuals from 0.00 to 0.72 thresholds. First mask srcC2D0 f6: 0.9484/0.9642 shared vs 0.8021/0.8059 split. The UNSET arm tracks split on 11 of 11 passes, which is how the DEFAULT (not just the knob) is verified. Implemented as SplitDepthEnabled() in runtime/gpu_draw_formats.h, one accessor for all three sites.

## What would falsify it

the mask #0 regression being explained, or any change to SplitDepthEnabled() or its three call sites in gpu_draw.cpp and gpu_draw_resolve_decode.cpp

## FALSIFIED 2026-08-12

Half right, and the wrong half was load-bearing. The first half stands and is stronger than ever: one host depth+stencil image per RB_DEPTH_INFO.depth_base takes the shadow atlas depth resolve from 0.34 to 0.999 against the console. The second half -- 'regresses the first shadow mask from 0.95 to 0.80' -- was not a property of the depth model at all. It was the half-scale depth buffer (C050) becoming visible once the split removed the atlas corruption that had been partly compensating for it: two defects were cancelling. With the scale fixed, mask #0 scores 0.9899 and shadows 11.35% against the console's 11.39%, on the SPLIT arm. I recorded that regression as a real quality loss and as the reason to hold the split's status open, and reasoning built on it -- including my own note arguing the split 'destroys real shadow' at 82% true-positive enrichment -- was measuring the scale error, not the memory model. Re-filed as C051 without the false half.

> Anything that cited this claim as proof must be re-checked. Grep the repo for it.
