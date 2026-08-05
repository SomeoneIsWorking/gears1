---
id: C008
kind: claim
status: holds
created: 2026-08-05
tags: gpu,perf,native-renderer
depends: runtime/gpu_draw.cpp, runtime/frame_ab.cpp
---

## Claim

Collapsing the console's predicated EDRAM tiling removes a quarter of a gameplay frame's draws and makes NO measurable difference to the draw loop

## Evidence

GEARS_DRAW_AB_UNTILE=1 alternates the collapsed and tiled arms frame by frame inside one run (runtime/frame_ab.h), which is the only way to resolve a difference this size here. Over 101 replayed frames per capture, with the collapse verified to fire on exactly 50 of 101: courtyard -0.26 ms against a 0.54 ms resolution, bright +0.34 against 0.49, play_v2 +0.08 against 0.84 -- all three NOT RESOLVED. The mechanism is clear from the same runs: fragment invocations are 1,730,163 tiled against 1,730,166 collapsed, so no shading is removed at all; the 174 dropped draws have state byte-identical to draws already issued in the frame, so every shader, pipeline, uniform and descriptor cache hits on them and they were nearly free. Two replay timings (1291 ms vs 939 ms) suggested a 27% win and were measuring the COLD frame, where the replayed tile's shaders and pipelines are never prepared -- a cost that appears in no steady-state frame.

## What would falsify it

a LIVE run rather than a replay showing a resolved difference -- the live draw loop is ~34 ms for 743 draws against this replay's ~8 ms for 726, so per-draw cost differs by 4x and the null result here may not transfer
