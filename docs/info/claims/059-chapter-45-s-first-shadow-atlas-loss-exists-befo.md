---
id: C059
kind: claim
status: holds
created: 2026-08-14
tags: render,shadow,depth
depends: runtime/gpu_draw_probe.cpp#FrameProbe::ReportDepthDumps, runtime/gpu_draw_xlate.cpp#BuildDepthResolveComputeShader
---

## Claim

Chapter-45's first shadow-atlas loss exists before native's D24S8 resolve: live host depth after the four leading shadow draws reproduces the resolved atlas to 8-bit quantisation.

## Evidence

scratch/ch45_live_depth_valid_20260814/run.log and depth_after_diag2090/2093/2096/2099.npy; compared with scratch/camerapair_chapter45_atlas_stats_20260814/ours/resolve_04_srcD5A0_864x864_f22_0c520000_draw2086.ppm: matched 10932 tile mean 0.96384335 live vs 0.9638444 resolved.

## What would falsify it

if a same-frame live-depth and resolve capture differs before 8-bit quantisation, or if the dump is shown to read a different depth target/base than ResolveDepthTo
