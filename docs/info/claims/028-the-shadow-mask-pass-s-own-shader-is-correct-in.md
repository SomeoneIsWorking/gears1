---
id: C028
kind: claim
status: holds
created: 2026-08-11
tags: render,shadow,oracle
depends: runtime/gpu_draw_xlate.cpp, runtime/gpu_draw_pipelines.cpp
---

## Claim

The shadow-mask pass's own shader is correct in this renderer; the pass differs from the console only through its blend, which is src x dst

## Evidence

courtyard.gfr, GEARS_DRAW_NOBLEND=1 + GEARS_DRAW_SURFACE_DUMP=647: our ps 0x25e217359ed17863 writes a clean binary mask -- mean 0.5425, 45.6% of components exactly 0, 54.2% exactly 1.0, all four channels identical -- the same KIND of buffer as the console's copy7 (mean 0.8469, 10.9% exactly 0, 83.5% of pixels exactly 0xFFFFFFFF, all four channels equal). With blending on our copy reads 0.0239, i.e. the scene. RB_BLENDCONTROL 0x00080008 = srcblend kDstColor, destblend kZero, ADD, and Xenia maps factor 8 identically (vulkan_pipeline_cache.cc kBlendFactorMap), so both sides compute src x dst.

## What would falsify it

a frame in which our NOBLEND output for that shader is NOT a binary mask, or a measurement showing the console applies a different blend equation to that draw
