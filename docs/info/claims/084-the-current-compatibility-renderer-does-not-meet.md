---
id: C084
kind: claim
status: holds
created: 2026-08-27
tags: performance
depends: runtime/gpu_frame_timing.cpp, runtime/gpu_draw.cpp#Renderer::RenderFrameImpl
---

## Claim

The current compatibility renderer does not meet a 5 ms/200 fps frame budget on the measured RX 6700 XT workloads: live title frames execute in 7-8 ms and the captured 1,742-draw chapter-45 frame in 13.537 ms.

## Evidence

scratch/logs/gpu_timing_live_release_noreadback_20260827.log and scratch/logs/gpu_timing_full_novalidate_20260826.log; zero failed timestamp samples.

## What would falsify it

A representative post-change gameplay measurement with the same timestamp boundaries completes below 5 ms, or the timestamp control ceases to distinguish full and one-draw workloads.
