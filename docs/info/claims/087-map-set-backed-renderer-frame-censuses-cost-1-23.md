---
id: C087
kind: claim
status: holds
created: 2026-08-27
tags: performance,renderer
depends: runtime/gpu_draw.cpp#Renderer::RenderFrameImpl, runtime/gpu_draw_census.cpp#FrameCensus::NoteDraw
---

## Claim

Map/set-backed renderer frame censuses cost 1.23 ms on the chapter-45 evidence frame and need not run on silent frames

## Evidence

GEARS_DRAW_AB_CENSUS=1 frame_replay chapter45_recovered.gfr 201: census-on 31.77 ms versus off 30.54 ms over 94/94 frames, 1.05 ms resolution floor; test_gpu_draw_census covers both collection states

## What would falsify it

an interleaved replay of the same capture no longer resolves census-on slower than census-off, or a report loses census facts with collection enabled
