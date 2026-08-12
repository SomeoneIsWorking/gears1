---
id: C039
kind: claim
status: falsified
created: 2026-08-12
tags: render,colour,bloom,oracle
depends: tools/front_buffer_percentiles.py, runtime/gpu_draw_resolve.cpp
falsified_on: 2026-08-12
---

## Claim

At a matched camera our frame is short in the LOW-MIDS and not at the top: median 2.0x and p90 3.5x below the console, p99 18% above it, p99.9 0.85x, max equal. Catalog #62's 'we under-reach at the top' framing is an artefact of comparing unmatched moments.

## Evidence

tools/front_buffer_percentiles.py on scratch/camgate/match/resolve_15 (our front buffer 0x311000) against scratch/vsord/theirs/oracle_f571_copy12 (the console's front buffer 0x1F606000 from the frame our capture was camera-gated to). Green: ours median 0.0118 p90 0.0392 p99 0.3569 p99.9 0.7725 max 1.0000; theirs 0.0235 / 0.1373 / 0.3020 / 0.9059 / 1.0000. Mechanism candidate measured on the same pair: our three srcC5A0 352x182 bloom destinations are 0 of 192192 components non-zero while the console's carry 1.44-1.81% non-zero bytes (byte-level check, no decoder involved).

## What would falsify it

a second camera-matched pair in which our median and p90 land on the console's, or one in which our p99.9/max fall well short -- either would mean this shape is moment-specific rather than the renderer's

## FALSIFIED 2026-08-12

The camera-gated pair this claim rests on is NOT the same picture, so no per-pixel or per-percentile number taken from it means anything. Measured with a POSITIVE CONTROL, which is what was missing: log-space Pearson correlation of luminance between our front buffer and the console's is 0.073 (linear 0.127), and no vertical flip, horizontal flip or shift up to +/-64 px lifts it above 0.157. The SAME metric on a pair that must agree -- our frame.ppm against our own front-buffer resolve -- scores 0.934 (linear 0.986). So 0.93 is what 'the same picture' looks like through this metric at this quantization, and the cross-side pair scores 0.07. Confirmed independently by banding: our brightest 210 pixels (0.20..0.45, far above the 8-bit floor) sit where the console reads mean 0.0099, and the console's 33 pixels above 1.0 sit where we read 0.0039. Uncorrelated at the BRIGHT end, where quantization cannot explain it. The camera gate matched the view-projection constants to a distance of 3.77 and that evidently does not imply the same rendered scene.

> Anything that cited this claim as proof must be re-checked. Grep the repo for it.
