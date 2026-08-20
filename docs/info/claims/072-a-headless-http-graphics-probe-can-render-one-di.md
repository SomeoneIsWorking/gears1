---
id: C072
kind: claim
status: holds
created: 2026-08-21
tags: graphics-probe
depends: runtime/vd_null_gpu.cpp#TriggerFrameRender, runtime/frame_probe_capture.h
---

## Claim

A headless HTTP graphics probe can render one diagnostic gameplay frame while DRAW_FRAME_AT or the content/camera selector holds the normal capture, without emitting report artifacts or consuming DRAW_FRAME_COUNT.

## Evidence

SP_Prison_P run scratch/logs/probe_alias_fixed.log: frame 914 rendered 1,159 draws for request 1 at guest frame 916; the log says the HTTP probe rendered without consuming 0/1 selected capture frames. The runtime wrote no renderer report or selected-capture artifacts; frame.ppm is the HTTP response (frame.png was converted from it manually for inspection).

## What would falsify it

A held-selector /api/frame.ppm request times out, increments the selected capture count, or writes a normal frame-report artifact.
