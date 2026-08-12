---
id: C051
kind: claim
status: holds
created: 2026-08-12
tags: 
---

## Claim

One host depth+stencil image per RB_DEPTH_INFO.depth_base is the correct model and the default: it takes the shadow atlas depth resolve from 0.34 to 0.999 against the console, and costs nothing once the depth SCALE is also correct

## Evidence

tools/depth_arm_ab.sh on scratch/camerapair_ps: one frozen camera replayed through four arms (shared, split, shared-again as a noise floor, and the variable UNSET to verify what ships), all scored against console frame 793. Atlas depth resolve srcD5A0 864x864 f22: 0.3395/0.3441 shared vs 0.9983/0.9989 split, stable across camera residuals from 0.00 to 0.72 thresholds so the model decides it and not the viewpoint. The UNSET arm tracked split on 11 of 11 passes, which is how the DEFAULT rather than the knob was verified. Implemented as SplitDepthEnabled() in runtime/gpu_draw_formats.h, one accessor for all three call sites because it was two independent statics and they diverged. The apparent mask #0 regression that held this open (0.95 -> 0.80) was NOT the depth model -- it was the half-scale depth of C050 becoming visible once the split removed the atlas corruption compensating for it. With both fixed, mask #0 scores 0.9899 on the split arm and shadows 11.35% against the console's 11.39%.

## What would falsify it

any capture where the shadow atlas depth resolve scores below 0.95 against the console, or a change to SplitDepthEnabled() or its call sites in gpu_draw.cpp and gpu_draw_resolve_decode.cpp
