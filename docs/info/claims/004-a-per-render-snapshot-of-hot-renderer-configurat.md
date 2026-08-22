---
id: C004
kind: claim
status: holds
created: 2026-08-22
tags: performance,renderer
depends: runtime/gpu_draw_options.cpp#ReadFrameOptions, runtime/gpu_draw.cpp#Renderer::RenderFrameImpl
reconfirmed: 2026-08-22
verified_at: 2026-08-22 18:43:45
---

## Claim

A per-render snapshot of hot renderer configuration removes a resolved 5.13 ms from the chapter-45 captured frame's CPU draw loop

## Evidence

Interleaved same-process A/B on chapter45_recovered.gfr: old 39.39 ms over 44 frames, snapshot 34.25 ms over 43 frames, 2.30 ms resolution; catalog #127

## What would falsify it

a representative paired same-process replay does not resolve the snapshot faster, or a renderer control is allowed to change inside one RenderFrame call

## Re-confirmed 2026-08-22

Committed implementation f09e856; interleaved chapter45 replay old 39.39 ms over 44 frames versus snapshot 34.25 ms over 43 frames, 2.30 ms resolution; 60/60 CTests and two-capture zero-VUID validation pass
