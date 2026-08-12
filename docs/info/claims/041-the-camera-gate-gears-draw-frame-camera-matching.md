---
id: C041
kind: claim
status: holds
created: 2026-08-12
tags: oracle,camera,comparison,instrument
depends: runtime/vd_null_gpu.cpp, tools/camera_match.py
---

## Claim

The camera gate (GEARS_DRAW_FRAME_CAMERA) matching the guest view-projection to a distance of 3.77 does NOT deliver the same rendered scene as the console frame the constants came from.

## Evidence

scratch/camgate/match vs oracle frame 571. Log-luminance correlation of the front buffers 0.073 as given, 0.157 best over vertical/horizontal flips and all shifts to +/-64px; the same metric on our frame.ppm vs our own front-buffer resolve scores 0.934. Bright-end banding agrees where quantization cannot explain it: our brightest 210 px (0.20..0.45) sit where the console reads mean 0.0099, and the console's 33 px above 1.0 sit where we read 0.0039.

## What would falsify it

a camera-gated pair at a tighter threshold that PASSES tools/front_buffer_percentiles.py's same-picture gate -- that would mean 3.77 was merely too loose rather than the constants being the wrong discriminator
