---
id: C005
kind: claim
status: holds
created: 2026-08-22
tags: performance,renderer,indices
depends: runtime/gpu_draw_indices.cpp#IndexPreparer::Prepare, runtime/gpu_draw.cpp#Renderer::RenderFrameImpl
reconfirmed: 2026-08-22
verified_at: 2026-08-22 19:02:12
---

## Claim

Exact frame-local index conversion reuse removes a resolved 3.54 ms from the chapter-45 captured frame's CPU draw loop

## Evidence

Interleaved same-process A/B on chapter45_recovered.gfr: old 25.09 ms over 44 frames, reuse 21.54 ms over 43 frames, 0.81 ms resolution; 1276 of 1614 reusable draws hit; catalog #128

## What would falsify it

a full index-key mutation returns a hit, a reused arena range expires before the frame fence, or a representative paired replay does not resolve reuse faster

## Re-confirmed 2026-08-22

Committed implementation 54ec7b1; interleaved chapter45 replay old 25.09 ms over 44 frames versus reuse 21.54 ms over 43 frames, 0.81 ms resolution, 1276/1614 hits; 61/61 CTests and two-capture zero-VUID validation pass
