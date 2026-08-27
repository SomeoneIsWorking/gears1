---
id: C088
kind: claim
status: holds
created: 2026-08-27
tags: performance,renderer
depends: runtime/gpu_draw_targets.cpp#RenderTargetCache::GetSurfaceTarget, runtime/gpu_draw.cpp#Renderer::RenderFrameImpl
---

## Claim

A frame-local RenderTargetCache surface lookup removes 4.21 ms from the chapter-45 CPU draw loop without changing the final framebuffer

## Evidence

GEARS_DRAW_AB_TARGET_LOOKUP=1 frame_replay chapter45_recovered.gfr 201 measured 20.42 vs 24.63 ms over 94/94 frames at a 1.05 ms floor; after extracting the timing owner, a final 201-render run measured 39.91 vs 46.17 ms at a 3.58 ms floor. Default and GEARS_DRAW_NO_TARGET_LOOKUP_CACHE control outputs both hash to 3b34082ab05198fa4733a50c7fe6e671b32c3871be38f7b563154ec741f80c25

## What would falsify it

a same-process A/B on a representative draw-heavy capture fails to resolve the saving, the two arms produce different render output, or formatsPerBase can change after the first surface lookup in a frame
